#include "ui/console.hpp"

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bilibili_api.hpp"
#include "core/config_manager.hpp"
#include "core/platform_util.hpp"
#include "core/qr_generator.hpp"
#include "http_client.hpp"
#include "json11/json11.hpp"
#include "plugin_utils.hpp"

#include "srt/srt_catalog.hpp"
#include "srt/srt_target.hpp" // IsSrtLine / kServiceEntryName

// All state below is touched from the OBS UI thread only. HTTP workers write to
// the shared result structs they captured and hand control back through
// Http::postToUi()/runAsync() continuations, never touching this state
// directly. g_alive turns queued continuations into no-ops once the module
// starts unloading.
namespace {

constexpr const char *CONSOLE_ID = "bilibili_stream_console";

enum Action : uintptr_t {
	ACT_LOGIN = 1,
	ACT_REFRESH,
	ACT_APPLY_TITLE,
	ACT_APPLY_PARTITION,
	ACT_START_LIVE,
	ACT_STOP_LIVE,
	ACT_RECOVER,
	ACT_END_SESSION,
	ACT_FETCH_QUALITY,
	ACT_OPEN_ROOM,
};

enum StatusLevel {
	LEVEL_INFO = 0,
	LEVEL_WARNING,
	LEVEL_ERROR,
};

// The `data` handle for the console source: OBS 32 hands only this pointer to
// get_properties/destroy, so the source handle is kept inside it.
struct ConsoleCtx {
	obs_source_t *source; // borrowed reference; OBS owns the lifetime
};

// Settings keys of the properties dialog (defaults seeded from config.json).
constexpr const char *KEY_TITLE = "room_title";
constexpr const char *KEY_PART = "room_part";
constexpr const char *KEY_AREA = "room_area";

bool g_alive = false;
obs_source_t *g_source = nullptr;
Core::ConfigManager g_config;

bool g_busy = false;
std::string g_status;
int g_status_level = LEVEL_INFO;
// 服务端仍认为在播(live_status==1)且本地未推流 → "恢复推流"可用(M1b)。
bool g_session_in_progress = false;
// 「结束进行中场次」两段式确认:第一次点击置位并改口,再点才发 stopLive。
bool g_confirm_end_session = false;
json11::Json g_partitions;
bool g_partitions_loading = false;
bool g_partitions_tried = false;

// The values currently shown in the properties dialog. OBS 32 button callbacks
// cannot read the dialog's settings, so every editable property carries a
// modified callback that mirrors its value here the moment the user edits it;
// apply-buttons then submit this draft.
std::string g_draft_title;
int g_draft_part = 0;
int g_draft_area = 0;
// §5.4/§5.5 拉取结果的多行展示(推荐码率/干预/编码能力),空=未拉取。
std::string g_quality_info;

// 把当前场次的 rtmpsrt:// 线路合并进 OBS 的 rtmp-services 服务目录
// （用户级 services.json；只增服务条目，不写用户已选的推流配置，ADR-0007）。
// **无条件写**：条目本身是「本插件支持私有 SRT」的能力声明，即使本次没有 rtmpsrt:// 线路
// （恢复场次只给 rtmp 线路）也要保证服务下拉里有这一条，里面放默认入口，
// 用不用、改成什么地址由用户自己决定。
// 返回值 = 本次是否写入了真实的 rtmpsrt:// 线路，供文案区分。
bool refresh_srt_catalog()
{
	std::vector<Srt::CatalogLine> lines;
	const auto &cfg = g_config.config();
	for (size_t i = 0; i < cfg.stream_targets.size(); ++i) {
		const auto &t = cfg.stream_targets[i];
		if (!Srt::IsSrtLine(t.addr))
			continue;
		std::string name =
			Utils::Text(t.backup ? "Srt.LineBackup" : "Srt.LinePrimary", (int)i + 1) + " · " + t.protocol;
		if (!t.provider.empty())
			name += "/" + t.provider;
		lines.push_back({name, t.addr});
	}
	Srt::UpdateServiceCatalog(lines, Utils::Text("Srt.DefaultServer"));
	return !lines.empty();
}

void refresh_props()
{
	if (g_alive && g_source)
		obs_source_update_properties(g_source);
}

// UI-thread only (all callers run on it: button handlers, postToUi/guarded
// continuations). Rebuilds the dialog as part of setting, otherwise async
// completions leave a stale status line on screen — the panel is only redrawn
// on explicit refresh, which used to make login/quality results appear to
// "hang" until an unrelated button press.
void set_status(int level, const std::string &text)
{
	g_status = text;
	g_status_level = level;
	refresh_props();
}

// Wraps a UI-thread continuation so a task delivered after ConsoleDestroy()
// becomes a no-op instead of touching released state.
template<typename F> std::function<void()> guarded(F &&fn)
{
	return [fn = std::forward<F>(fn)]() {
		if (!g_alive)
			return;
		fn();
	};
}

std::string module_file(const char *name)
{
	char *path = obs_module_config_path(name);
	if (!path)
		return {};
	std::string result = path;
	bfree(path);
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(result).parent_path(), ec);
	return result;
}

// The Bilibili API sometimes returns area ids as JSON strings and sometimes as
// numbers; accept both shapes and never throw.
bool parse_area_id(const json11::Json &value, int &id)
{
	const std::string text = value.is_string() ? value.string_value() : value.dump();
	if (text.empty())
		return false;
	int parsed = 0;
	auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		return false;
	id = parsed;
	return true;
}

int part_of_parent(const json11::Json &parent)
{
	int id = 0;
	parse_area_id(parent["id"], id);
	return id;
}

// ---------------------------------------------------------------------------
// asynchronous actions (each started from the UI thread, busy-guarded)
// ---------------------------------------------------------------------------

bool begin_action(const char *what)
{
	if (g_busy) {
		set_status(LEVEL_WARNING, Utils::Text("Console.Busy", what));
		return false;
	}
	g_busy = true;
	refresh_props();
	return true;
}

void end_action()
{
	g_busy = false;
	refresh_props();
}

// Verifies the session jar against nav + room endpoints and persists the
// result. The jar is the single identity store (Http layer owns it); config
// only mirrors the persisted cookie whitelist.
void refresh_login_chain()
{
	// buvid3 alone is not a login session: only SESSDATA justifies nav.
	if (!Http::HttpClient::has_cookie("SESSDATA")) {
		set_status(LEVEL_INFO, Utils::Text("Status.NotLoggedIn"));
		return;
	}
	if (!begin_action(Utils::Text("Action.Refresh").c_str()))
		return;
	auto result = std::make_shared<struct LoginChainResult>();
	struct LoginChainResult {
		bool loggedIn = false;
		bool roomOk = false;
		std::string message;
		std::string mid;
		std::string roomId;
		std::string csrf;
		std::string roomState; // §3.1: 场次/封禁一句话(空=不可用)
		std::string title;     // §3.3 data.title: 服务端房间名(面板房间行唯一权威来源)
		int liveStatus = -1;   // §3.1 live_status 原值(M1b 断流找回判据)
	};
	Http::HttpClient::runAsync(
		[result]() {
			result->loggedIn = Bili::BiliApi::checkLoginStatus(result->message, result->mid);
			// B3: 官方 H5 登录成功后刷新全域 Cookie(sso/list);失败不影响登录态(仅日志)。
			if (result->loggedIn) {
				std::string ssoMessage;
				Bili::BiliApi::propagateSso(ssoMessage);
			}
			if (result->loggedIn)
				result->roomOk =
					Bili::BiliApi::getRoomIdAndCsrf(result->roomId, result->csrf, result->message);
			if (result->roomOk) {
				// §3.3 room/GetInfo: live_status 实测在这里;§3.1
				// getInfoByUser 全树无该字段(2026-11-25 实网钉死,
				// 此前解析挂错端点导致在播恒显「未开播」)。
				// live_status 1=直播中 2=轮回;lock_status/lock_time
				// 为封禁信号。取不到只降级,不断链。
				std::string stateMessage;
				json11::Json data = Bili::BiliApi::getRoomInfoByUid(result->mid, stateMessage);
				if (data.is_object()) {
					result->title = data["title"].string_value();
					const int live_status = data["live_status"].int_value();
					result->liveStatus = live_status;
					const std::string state = live_status == 1   ? Utils::Text("Live.IsLive")
								  : live_status == 2 ? Utils::Text("Live.Rebroadcast")
										     : Utils::Text("Live.Offline");
					result->roomState = Utils::Text("Live.Session", state.c_str());
					if (data["lock_status"].int_value() != 0)
						result->roomState += Utils::Text("Live.Locked");
				} else {
					obs_log(LOG_WARNING, "房间态获取失败(不影响登录链): %s", stateMessage.c_str());
				}
			}
		},
		guarded([result]() {
			end_action();
			auto &cfg = g_config.config();
			cfg.cookies = Http::HttpClient::persistent_cookie_header();
			cfg.login_status = result->loggedIn;
			if (result->loggedIn)
				cfg.mid = result->mid;
			if (result->roomOk) {
				cfg.room_id = result->roomId;
				cfg.csrf_token = result->csrf;
				// 房间名以服务端为准(§3.3 data.title):换号/网页改名后
				// 刷新即回写,面板房间行不再滞留旧账号/旧标题。
				if (!result->title.empty() && cfg.title != result->title)
					cfg.title = result->title;
			}
			g_config.save();
			// 服务端场次是唯一权威(GLOSSARY 开播/停播=服务端信令):
			// 观测到未开播而本地仍记着已开播(空场次被掐等)→ 归零。
			if (result->liveStatus == 0 && cfg.streaming) {
				cfg.streaming = false;
				g_config.save();
			}
			// M1b 断流找回判据(契约 §7.2 前置): 服务端 live_status==1
			// 且本地无在播认知 → 状态行给出恢复入口提示。
			g_session_in_progress = result->liveStatus == 1;
			g_confirm_end_session = false; // 每次刷新重置确认态
			if (result->loggedIn) {
				std::string status =
					Utils::Text("Status.LoggedIn") + " (mid=" + result->mid +
					(result->roomOk ? ", " + Utils::Text("Status.Room", result->roomId.c_str())
							: ", " + Utils::Text("Status.RoomFail")) +
					(result->roomState.empty() ? "" : ", " + result->roomState) + ")";
				if (g_session_in_progress && !cfg.streaming)
					status += Utils::Text("Console.SessionHint");
				set_status(g_session_in_progress && !cfg.streaming ? LEVEL_WARNING : LEVEL_INFO,
					   status);
			} else {
				g_session_in_progress = false;
				set_status(LEVEL_ERROR, Utils::Text("Status.LoginInvalid", result->message.c_str()));
			}
		}));
}

// Fingerprint snapshot on the log (M0b acceptance): shows the fixed LiveHime
// UA, the session traceid and where buvid3 came from (persisted config vs a
// fresh x/frontend/finger/spi fetch). Runs on the worker; logs directly.
void start_fingerprint_probe()
{
	Http::HttpClient::runAsync(
		[]() {
			const bool from_config = Http::HttpClient::has_cookie("buvid3");
			if (!from_config)
				Bili::BiliApi::fetch_buvid();
			const std::string b3 = Http::HttpClient::cookie_value("buvid3");
			obs_log(LOG_INFO, "指纹: UA=%s | traceid=%s | buvid3=%s (%s)", Http::HttpClient::user_agent(),
				Http::HttpClient::traceid().c_str(), b3.empty() ? "获取失败" : b3.c_str(),
				from_config ? "来自配置" : "新取");
		},
		nullptr);
}

// One-shot login attempt: fetch QR, hand it to the browser, poll until the
// user confirms, the code expires, or the module unloads.
void do_login()
{
	if (!begin_action(Utils::Text("Action.Login").c_str()))
		return;
	struct LoginResult {
		bool ok = false;
		std::string message;
	};
	auto result = std::make_shared<LoginResult>();

	Http::HttpClient::runAsync(
		[result]() {
			std::string qrData, qrKey, msg;
			if (!Bili::BiliApi::getQrCode(qrData, qrKey, msg)) {
				result->message = Utils::Text("Login.QrFetchFail", msg.c_str());
				return;
			}

			// The panel cannot render images (ADR-0003): write the QR as SVG
			// and open it in the system browser; fall back to showing the URL.
			std::string target = qrData;
			const std::string svgPath = module_file("login_qr.svg");
			if (!svgPath.empty() && Core::QrGenerator::writeSvgFile(svgPath, qrData) &&
			    Core::openInBrowser(svgPath)) {
				Http::HttpClient::postToUi(
					guarded([]() { set_status(LEVEL_INFO, Utils::Text("Login.QrOpened")); }));
			} else if (Core::openInBrowser(qrData)) {
				Http::HttpClient::postToUi(
					guarded([]() { set_status(LEVEL_INFO, Utils::Text("Login.LinkOpened")); }));
			} else {
				result->message = Utils::Text("Core.BrowserFail", qrData.c_str());
				return;
			}

			std::string lastMessage;
			// Official login page polls at ~2s (E-06); keep the 3-minute
			// overall expiry window.
			for (int attempt = 0; attempt < 90; ++attempt) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				if (Http::HttpClient::isStopping())
					return;
				std::string pollMsg;
				bool expired = false;
				if (Bili::BiliApi::qrLogin(qrKey, pollMsg, &expired)) {
					result->ok = true;
					return;
				}
				if (expired) {
					result->message = pollMsg;
					return;
				}
				if (pollMsg != lastMessage) {
					lastMessage = pollMsg;
					std::string shown = pollMsg;
					Http::HttpClient::postToUi(guarded([shown]() {
						set_status(LEVEL_INFO, Utils::Text("Login.Polling", shown.c_str()));
					}));
				}
			}
			result->message = Utils::Text("Login.QrExpired");
		},
		guarded([result]() {
			end_action();
			if (result->ok) {
				refresh_login_chain();
				return;
			}
			if (!result->message.empty())
				set_status(LEVEL_ERROR, result->message);
			else
				set_status(LEVEL_WARNING, Utils::Text("Login.Cancelled"));
		}));
}

// ADR-0005:插件不写 OBS 服务配置——开播所得线路
// 全部只读展示(地址/推流码可复制),用户在 OBS「设置→直播」里自行填写。
// 保持的那条轴:插件从不点 OBS「开始直播」。

// M1b 断流找回(契约 §7): 服务端仍认为在播时找回推流地址/密钥,仅展示(ADR-0005)。
// 官方序: get_stream_by_roomId 直接给 data.rtmp{addr,code};取不到再问
// GetUpStreamRtmp(can_change 时用 urls[0],密钥用本地留存的推流码)。
void do_recover_stream()
{
	if (!begin_action(Utils::Text("Action.Recover").c_str()))
		return;
	auto config = g_config.config();
	struct RecoverResult {
		bool ok = false;
		std::string message;
		std::string addr;
		std::string code;
		std::string source; // 日志用:端点来源
	};
	auto result = std::make_shared<RecoverResult>();
	Http::HttpClient::runAsync(
		[config, result]() {
			json11::Json stream = Bili::BiliApi::getActiveStream(config.room_id, result->message);
			if (stream.is_object()) {
				result->addr = stream["rtmp"]["addr"].string_value();
				result->code = stream["rtmp"]["code"].string_value();
				if (!result->addr.empty() && !result->code.empty()) {
					result->source = "get_stream_by_roomId";
					result->ok = true;
					return;
				}
			}
			// §7.2: 找回失败时问可推地址;密钥只能复用本地留存值。
			bool canChange = false;
			std::vector<std::string> urls;
			std::string upMessage;
			if (!Bili::BiliApi::getUpStreamRtmp(config.room_id, canChange, urls, upMessage)) {
				result->message = Utils::Text("Recover.Fail", upMessage.c_str());
				return;
			}
			if (!canChange || urls.empty()) {
				result->message =
					Utils::Text("Recover.NoAddr") + (upMessage.empty() ? "" : ": " + upMessage);
				return;
			}
			if (config.rtmp_code.empty()) {
				result->message = Utils::Text("Recover.NoKey");
				return;
			}
			result->addr = urls[0];
			result->code = config.rtmp_code;
			result->source = "GetUpStreamRtmp";
			result->ok = true;
		},
		guarded([result]() {
			end_action();
			if (!result->ok) {
				set_status(LEVEL_ERROR, Utils::Text("Recover.Failed", result->message.c_str()));
				return;
			}
			auto &cfg = g_config.config();
			cfg.streaming = true;
			cfg.rtmp_addr = result->addr;
			cfg.rtmp_code = result->code;
			// 找回成功只落到展示线路表,不碰 OBS 配置。
			cfg.stream_targets.clear();
			cfg.stream_targets.push_back(
				Bili::Config::StreamTarget{"rtmp", "", result->addr, result->code, false});
			g_config.save();
			// 恢复路径只产出 rtmp 线路：refresh_srt_catalog() 无 rtmpsrt:// 线路时
			// 条目回退为默认占位入口（ADR-0007 无条件写 = 能力声明，上次的真实线路
			// 随本次响应没有 SRT 线路而失效，盖回占位是预期行为）。
			refresh_srt_catalog();
			g_session_in_progress = false;
			set_status(LEVEL_INFO, Utils::Text("Recover.Success", result->source.c_str()));
		}));
}

void do_start_live()
{
	if (!begin_action(Utils::Text("Action.StartLive").c_str()))
		return;
	auto config = g_config.config();
	struct StartResult {
		bool ok = false;
		std::string message;
		std::string addr;
		std::string code;
		std::vector<Bili::Config::StreamTarget> targets;
		std::string faceQr;
		std::string facePage;
		std::string mid;
	};
	auto result = std::make_shared<StartResult>();
	result->mid = config.mid;

	Http::HttpClient::runAsync(
		[config, result]() mutable {
			// 开播前置(官方序): PreCheck 合规拦截(§3.5)→ 播前状态观测(§3.6,
			// 取值域未钉死,仅记日志)→ startLive。
			if (!Bili::BiliApi::preLiveCheck(result->message)) {
				obs_log(LOG_WARNING, "开播前置检查未通过: %s", result->message.c_str());
				return;
			}
			int preStatus = -1;
			std::string statusMessage;
			if (Bili::BiliApi::getRoomPreLiveStatus(preStatus, statusMessage))
				obs_log(LOG_INFO, "播前状态 GetRoomPreLiveStatus: status=%d", preStatus);
			else
				obs_log(LOG_INFO, "播前状态获取失败(不阻断开播): %s", statusMessage.c_str());
			// 备流自动判定(E-22 官方同款分派):with_backup=true 模式在
			// startLive 内先查 GetVisibleMode——upower_privilege_type==5 才发
			// 备流三参请求,否则自动按官方普通开播发送;原因经 message 上抛
			// (成功分支以提示形式展示),UI 不再提供单独的备流开播按钮。
			result->ok = Bili::BiliApi::startLive(config, result->addr, result->code, result->message,
							      result->faceQr, result->mid, true, &result->facePage);
			// startLive 把 protocols[]/protocols_backup[] 全量写进 config 的副本;
			// 必须带回 UI 线程,否则成功分支会把 stream_targets 覆盖成空表。
			result->targets = config.stream_targets;
		},
		guarded([result]() {
			end_action();
			if (result->ok) {
				auto &cfg = g_config.config();
				cfg.streaming = true;
				cfg.rtmp_addr = result->addr;
				cfg.rtmp_code = result->code;
				cfg.stream_targets = result->targets;
				if (!result->mid.empty())
					cfg.mid = result->mid;
				g_config.save();
				std::string srt_hint;
				if (refresh_srt_catalog())
					srt_hint = Utils::Text("Srt.CatalogHint", Srt::kServiceEntryName);
				const size_t n = cfg.stream_targets.size();
				// message 在成功分支只承载「备流自动判定」类提示
				// (无资格已按普通发送 / 备流被拒已回退)。
				set_status(result->message.empty() ? LEVEL_INFO : LEVEL_WARNING,
					   Utils::Text("Start.Success", n) + srt_hint +
						   (result->message.empty() ? "" : " — " + result->message));
			} else if (!result->facePage.empty()) {
				// 60043: auth-middle 是网页不是二维码,按官方直接开认证窗口。
				const bool shown = Core::openInBrowser(result->facePage);
				set_status(LEVEL_WARNING,
					   shown ? Utils::Text("Start.FacePageOpened")
						 : Utils::Text("Start.FaceManual", result->facePage.c_str()));
			} else if (!result->faceQr.empty()) {
				// 60024: show the face-auth QR through the browser, same
				// channel as the login QR.
				const std::string svgPath = module_file("face_qr.svg");
				bool shown =
					(!svgPath.empty() && Core::QrGenerator::writeSvgFile(svgPath, result->faceQr) &&
					 Core::openInBrowser(svgPath)) ||
					Core::openInBrowser(result->faceQr);
				set_status(LEVEL_WARNING,
					   shown ? Utils::Text("Start.FaceQrOpened")
						 : Utils::Text("Start.FaceManual", result->faceQr.c_str()));
			} else {
				set_status(LEVEL_ERROR, Utils::Text("Start.Failed", result->message.c_str()));
			}
		}));
}

void do_stop_live()
{
	// 官方 CloseLiveRoomPre 的顺序是「先停流(输出)再停播(信令)」。
	// 插件不驱动 OBS 输出(ADR-0001/0005),因此推流仍在进行时先提示用户
	// 在 OBS 停止推流,避免房间已关而画面上传仍在继续。
	if (obs_frontend_streaming_active()) {
		set_status(LEVEL_WARNING, Utils::Text("Stop.NeedStopStreamFirst"));
		return;
	}
	if (!begin_action(Utils::Text("Action.StopLive").c_str()))
		return;
	auto config = g_config.config();
	struct StopResult {
		bool ok = false;
		std::string message;
	};
	auto result = std::make_shared<StopResult>();
	Http::HttpClient::runAsync(
		[config, result]() { result->ok = Bili::BiliApi::stopLive(config, result->message); },
		guarded([result]() {
			end_action();
			if (result->ok) {
				g_config.config().streaming = false;
				g_session_in_progress = false; // 服务端已停播,恢复入口关闭
				// 场次结束,线路与密钥随场次作废(密钥每场新生成)。
				g_config.config().stream_targets.clear();
				g_config.save();
				set_status(LEVEL_INFO, Utils::Text("Stop.Done"));
			} else {
				set_status(LEVEL_ERROR, Utils::Text("Stop.Failed", result->message.c_str()));
			}
		}));
}

// M2 质量协商(§5.4/§5.5 实证模型):拉推荐码率+干预标志+编码能力,展示用。
// 官方口径无档位枚举/无回执端点(QualityCallback 是线路回执,不属本链)。
void do_fetch_quality()
{
	if (!begin_action(Utils::Text("Action.Quality").c_str()))
		return;
	// 分区匹配是 UI 线程认知,先取草稿外的已保存二级分区 id。
	const int area_id = g_config.config().area_id;
	struct QualityResult {
		std::string lines;
	};
	auto result = std::make_shared<QualityResult>();
	Http::HttpClient::runAsync(
		[result, area_id]() {
			std::string message;
			json11::Json json = Bili::BiliApi::videoQualityConfig(message);
			if (json.is_object()) {
				// 实网形状(2026-11-25 实测,契约 §5.4):推荐码率不在顶层,
				// 而是 data.bitrate_config.recommend_bitrate_area_intervention.
				// recommend_bitrate_area_v2_interventions[] 里按二级分区命中;
				// 未命中回落 default_bitrate。
				const json11::Json bc = json["data"]["bitrate_config"];
				const int default_bitrate = (int)bc["default_bitrate"].number_value();
				int recommend = default_bitrate;
				std::string source = Utils::Text("Quality.SourceDefault");
				for (const auto &tier :
				     bc["recommend_bitrate_area_intervention"]["recommend_bitrate_area_v2_interventions"]
					     .array_items()) {
					bool hit = false;
					for (const auto &area : tier["area_v2_ids"].array_items())
						if (area.int_value() == area_id) {
							hit = true;
							break;
						}
					if (hit) {
						recommend = (int)tier["recommend_bitrate"].number_value();
						source = Utils::Text("Quality.SourceIntervention",
								     tier["strategy_source"].string_value().c_str());
						break;
					}
				}
				result->lines =
					Utils::Text("Quality.Recommend", recommend, source.c_str(), default_bitrate) +
					"\n" +
					Utils::Text("Quality.Member",
						    bc["recommend_bitrate_member_intervention"].bool_value()
							    ? Utils::Text("Common.Yes").c_str()
							    : Utils::Text("Common.No").c_str()) +
					"\n" + Utils::Text("Quality.Note");
			} else {
				result->lines = Utils::Text("Quality.Fail", message.c_str());
			}
			// 编码能力各自降级,不互相牵连。
			bool canH265 = false, canAv1 = false;
			if (Bili::BiliApi::canUseH265Encoder(canH265, message))
				result->lines += "\n" + Utils::Text("Quality.H265",
								    canH265 ? Utils::Text("Common.Is").c_str()
									    : Utils::Text("Common.NoShort").c_str());
			else
				result->lines += "\n" + Utils::Text("Quality.H265Fail", message.c_str());
			if (Bili::BiliApi::canUseAv1Encoder(canAv1, message))
				result->lines += " | " + Utils::Text("Quality.Av1",
								     canAv1 ? Utils::Text("Common.Is").c_str()
									    : Utils::Text("Common.NoShort").c_str());
			else
				result->lines += " | " + Utils::Text("Quality.Av1Fail", message.c_str());
		},
		guarded([result]() {
			end_action();
			g_quality_info = result->lines;
			set_status(LEVEL_INFO, Utils::Text("Quality.Updated"));
		}));
}

void do_apply_title()
{
	const std::string title = g_draft_title;
	if (title.empty()) {
		set_status(LEVEL_WARNING, Utils::Text("Title.Empty"));
		return;
	}
	if (!begin_action(Utils::Text("Action.ApplyTitle").c_str()))
		return;
	auto config = g_config.config();
	struct TitleResult {
		bool ok = false;
		std::string message;
	};
	auto result = std::make_shared<TitleResult>();
	Http::HttpClient::runAsync(
		[config, title, result]() {
			result->ok = Bili::BiliApi::updateRoomInfo(config, result->message, title, -1);
		},
		guarded([title, result]() {
			end_action();
			if (result->ok) {
				g_config.config().title = title;
				g_config.save();
				set_status(LEVEL_INFO, Utils::Text("Title.Done"));
			} else {
				set_status(LEVEL_ERROR, Utils::Text("Title.Fail", result->message.c_str()));
			}
		}));
}

void do_apply_partition()
{
	const int areaId = g_draft_area;
	const int partId = g_draft_part;
	if (areaId <= 0) {
		set_status(LEVEL_WARNING, Utils::Text("Part.NeedArea"));
		return;
	}
	if (!begin_action(Utils::Text("Action.ApplyPart").c_str()))
		return;
	auto config = g_config.config();
	struct PartResult {
		bool ok = false;
		std::string message;
	};
	auto result = std::make_shared<PartResult>();
	Http::HttpClient::runAsync(
		[config, areaId, result]() {
			result->ok = Bili::BiliApi::updateRoomInfo(config, result->message, "", areaId);
		},
		guarded([areaId, partId, result]() {
			end_action();
			if (result->ok) {
				auto &cfg = g_config.config();
				cfg.area_id = areaId;
				cfg.part_id = partId;
				g_config.save();
				set_status(LEVEL_INFO, Utils::Text("Part.Done"));
			} else {
				set_status(LEVEL_ERROR, Utils::Text("Part.Fail", result->message.c_str()));
			}
		}));
}

void ensure_partitions_loaded()
{
	if (g_partitions_tried || g_partitions_loading || !g_config.config().login_status)
		return;
	g_partitions_loading = true;
	struct ListResult {
		json11::Json data;
		std::string message;
	};
	auto result = std::make_shared<ListResult>();
	Http::HttpClient::runAsync([result]() { result->data = Bili::BiliApi::getPartitionList(result->message); },
				   guarded([result]() {
					   g_partitions_loading = false;
					   g_partitions = result->data;
					   g_partitions_tried = result->data.is_array();
					   refresh_props();
				   }));
}

void open_live_room()
{
	const auto &cfg = g_config.config();
	if (cfg.room_id.empty()) {
		set_status(LEVEL_WARNING, Utils::Text("Room.Empty"));
		return;
	}
	const std::string url = "https://live.bilibili.com/" + cfg.room_id;
	if (!Core::openInBrowser(url))
		set_status(LEVEL_WARNING, Utils::Text("Core.BrowserFail", url.c_str()));
}

// ---------------------------------------------------------------------------
// obs source plumbing (OBS renders everything; priv carries the action id)
// ---------------------------------------------------------------------------

// Mirrors the dialog's editable values into the draft so the apply-buttons can
// submit them (OBS 32 gives button callbacks no access to dialog settings).
void sync_draft(obs_data_t *settings)
{
	g_draft_title = obs_data_get_string(settings, KEY_TITLE);
	g_draft_part = static_cast<int>(obs_data_get_int(settings, KEY_PART));
	g_draft_area = static_cast<int>(obs_data_get_int(settings, KEY_AREA));
}

bool on_value_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
	return false;
}

const json11::Json *find_parent(const json11::Json &partitions, int partId);

bool on_part_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
	if (!props || !g_partitions.is_array())
		return true;
	// OBS 32.2 机制(源码 + 两次 coredump 实证):modified 回调返回值只决定
	// 是否把 RefreshProperties 排队(Qt 队列,事件处理完后按**已缓存的**
	// properties 对象重建控件);回调内既不重跑 get_properties,也绝不能
	// 触发对话框刷新——update_properties 的 ReloadProperties 同线程直调,会
	// 同步销毁控件树/重放 modified 回调,把仍在发信号的 combo 和正在执行的
	// 回调自己拆掉(64386 无限递归爆栈、65402 Qt currentTextChanged 悬垂)。
	// 正确最简做法 = 用 libobs 公共 API 原地改写缓存 property 的条目
	// (obs_properties_get + list_clear + list_add_int),return true 让
	// 排队的 Refresh 按新条目重建;选中值取对话框 settings,需同步写回。
	obs_property_t *area_list = obs_properties_get(props, KEY_AREA);
	const json11::Json *parent = find_parent(g_partitions, g_draft_part);
	if (!area_list || !parent)
		return true;
	obs_property_list_clear(area_list);
	int first_area = 0;
	bool area_valid = false;
	for (const auto &area : (*parent)["list"].array_items()) {
		int id = 0;
		if (!parse_area_id(area["id"], id))
			continue;
		obs_property_list_add_int(area_list, area["name"].string_value().c_str(), id);
		if (first_area == 0)
			first_area = id;
		if (id == g_draft_area)
			area_valid = true;
	}
	// 现值仍指向旧大类的条目 → 自动切到新大类首个二级并写回对话框设置
	// (Refresh 重建 combo 时按 settings 选中)。
	if (!area_valid && first_area > 0) {
		obs_data_set_int(settings, KEY_AREA, first_area);
		g_draft_area = first_area;
	}
	return true; // queued RefreshProperties rebuilds the combo with new items
}

bool on_button(obs_properties_t *, obs_property_t *, void *data)
{
	const auto action = static_cast<Action>(reinterpret_cast<uintptr_t>(data));
	switch (action) {
	case ACT_LOGIN:
		do_login();
		break;
	case ACT_REFRESH:
		refresh_login_chain();
		break;
	case ACT_APPLY_TITLE:
		do_apply_title();
		break;
	case ACT_APPLY_PARTITION:
		do_apply_partition();
		break;
	case ACT_START_LIVE:
		do_start_live();
		break;
	case ACT_STOP_LIVE:
		do_stop_live();
		break;
	case ACT_RECOVER:
		do_recover_stream();
		break;
	case ACT_END_SESSION:
		// 两段式确认:真实停播信令,第一次只改口,第二次才发(AGENTS:开播类
		// 调用需用户在场并明确同意 —— 两次点击即在场同意)。
		if (!g_confirm_end_session) {
			g_confirm_end_session = true;
			set_status(LEVEL_WARNING, Utils::Text("EndSession.Confirm"));
		} else {
			g_confirm_end_session = false;
			do_stop_live();
		}
		break;
	case ACT_FETCH_QUALITY:
		do_fetch_quality();
		break;
	case ACT_OPEN_ROOM:
		open_live_room();
		break;
	}
	return false; // async results refresh through obs_source_update_properties()
}

obs_property_t *add_action_button(obs_properties_t *props, const char *name, const char *text, Action action,
				  bool enabled)
{
	obs_property_t *prop = obs_properties_add_button2(
		props, name, text, +[](obs_properties_t *p, obs_property_t *b, void *d) { return on_button(p, b, d); },
		reinterpret_cast<void *>(action));
	obs_property_set_enabled(prop, enabled);
	return prop;
}

obs_property_t *add_status_line(obs_properties_t *props, const char *name, const std::string &text, int level)
{
	obs_property_t *prop = obs_properties_add_text(props, name, text.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_type(prop, level == LEVEL_ERROR ? OBS_TEXT_INFO_ERROR
								   : (level == LEVEL_WARNING ? OBS_TEXT_INFO_WARNING
											     : OBS_TEXT_INFO_NORMAL));
	return prop;
}

const json11::Json *find_parent(const json11::Json &partitions, int partId)
{
	for (const auto &item : partitions.array_items()) {
		if (part_of_parent(item) == partId)
			return &item;
	}
	return nullptr;
}

obs_properties_t *console_properties(void *data)
{
	auto *ctx = static_cast<ConsoleCtx *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	auto &cfg = g_config.config();

	obs_data_set_default_string(settings, KEY_TITLE, cfg.title.c_str());
	obs_data_set_default_int(settings, KEY_PART, cfg.part_id);
	obs_data_set_default_int(settings, KEY_AREA, cfg.area_id);

	// --- 登录状态(统一入口:扫码登录/刷新/房间) ---
	add_status_line(props, "st_login",
			cfg.login_status ? (Utils::Text("Status.LoggedIn") + " (mid=" + cfg.mid + ")")
					 : Utils::Text("Status.NotLoggedIn"),
			cfg.login_status ? LEVEL_INFO : LEVEL_WARNING);
	// 扫码登录与登录态成对:未登录时是主入口,已登录时降级为换号动作。
	add_action_button(props, "btn_login",
			  cfg.login_status ? Utils::Text("Btn.LoginSwitch").c_str() : Utils::Text("Btn.Login").c_str(),
			  ACT_LOGIN, !g_busy);
	add_action_button(props, "btn_refresh", Utils::Text("Btn.Refresh").c_str(), ACT_REFRESH, !g_busy);
	if (!cfg.room_id.empty()) {
		obs_properties_add_text(props, "st_room",
					Utils::Text("Console.RoomLine", cfg.room_id.c_str(), cfg.title.c_str()).c_str(),
					OBS_TEXT_INFO);
		add_action_button(props, "btn_open_room", Utils::Text("Btn.OpenRoom").c_str(), ACT_OPEN_ROOM, true);
	}

	// --- 最近操作 ---
	if (!g_status.empty())
		add_status_line(props, "st_last", g_status, g_status_level);
	// 推流线路(ADR-0005):服务端给的每条线路只读展示,地址与推流码均可选中复制;
	// 插件不改 OBS 服务配置,用哪条由用户在 OBS「设置→直播」里自行填写。
	if (cfg.streaming && !cfg.stream_targets.empty()) {
		int idx = 0;
		for (const auto &t : cfg.stream_targets) {
			++idx;
			const std::string label = Utils::Text(t.backup ? "Srt.LineBackup" : "Srt.LinePrimary", idx) +
						  " · " + t.protocol + (t.provider.empty() ? "" : "/" + t.provider);
			const std::string addr_id = "cp_addr_" + std::to_string(idx);
			obs_properties_add_text(props, addr_id.c_str(), (label + Utils::Text("Srt.AddrSuffix")).c_str(),
						OBS_TEXT_DEFAULT);
			obs_data_set_default_string(settings, addr_id.c_str(), t.addr.c_str());
			if (!t.key.empty()) {
				const std::string key_id = "cp_key_" + std::to_string(idx);
				obs_properties_add_text(props, key_id.c_str(),
							(label + Utils::Text("Srt.KeySuffix")).c_str(),
							OBS_TEXT_DEFAULT);
				obs_data_set_default_string(settings, key_id.c_str(), t.key.c_str());
			}
		}
	}

	// --- 私有 SRT 输出(ADR-0007) ---
	// 服务条目由插件合并进 rtmp-services 的 services.json；用户仍在 OBS
	// 「设置→直播」自己选服务、自己粘贴推流码、自己点开始直播，插件不写用户配置。
	if (cfg.streaming) {
		bool has_srt = false;
		for (const auto &t : cfg.stream_targets)
			has_srt = has_srt || Srt::IsSrtLine(t.addr);
		const std::string hint = has_srt ? Utils::Text("Srt.HintWith", Srt::kServiceEntryName)
						 : Utils::Text("Srt.HintWithout");
		obs_properties_add_text(props, "st_srt_hint", hint.c_str(), OBS_TEXT_INFO);
	}

	// --- 直播间设置 ---
	obs_property_t *title =
		obs_properties_add_text(props, KEY_TITLE, Utils::Text("Prop.Title").c_str(), OBS_TEXT_DEFAULT);
	obs_property_set_enabled(title, !g_busy);
	obs_property_set_modified_callback(title, on_value_changed);
	add_action_button(props, "btn_apply_title", Utils::Text("Btn.ApplyTitle").c_str(), ACT_APPLY_TITLE, !g_busy);

	obs_property_t *partList = obs_properties_add_list(props, KEY_PART, Utils::Text("Prop.Partition").c_str(),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_t *areaList = obs_properties_add_list(props, KEY_AREA, Utils::Text("Prop.Area").c_str(),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	ensure_partitions_loaded();
	if (!g_partitions.is_array()) {
		const std::string placeholder = g_partitions_loading ? Utils::Text("Prop.PartLoading")
								     : Utils::Text("Prop.PartNotLoaded");
		obs_property_list_add_int(partList, placeholder.c_str(), 0);
		obs_property_list_add_int(areaList, placeholder.c_str(), 0);
	} else {
		// 条目重建以草稿为准(= 对话框当前显示的大类):OBS 重建属性表时
		// get_properties 读到的是已提交设置(滞后于对话框编辑),草稿由
		// modified 回调实时同步,与 UpdateSettings 回填后的控件一致。
		const int wantedPart = g_draft_part > 0 ? g_draft_part
							: static_cast<int>(obs_data_get_int(settings, KEY_PART));
		int shownPart = cfg.part_id;
		for (const auto &item : g_partitions.array_items()) {
			const int id = part_of_parent(item);
			obs_property_list_add_int(partList, item["name"].string_value().c_str(), id);
			if (id == wantedPart)
				shownPart = id;
		}
		if (wantedPart <= 0)
			obs_data_set_int(settings, KEY_PART, shownPart);

		const json11::Json *parent = find_parent(g_partitions, shownPart);
		const int wantedArea = g_draft_area > 0 ? g_draft_area
							: static_cast<int>(obs_data_get_int(settings, KEY_AREA));
		int shownArea = cfg.area_id;
		if (parent) {
			for (const auto &area : (*parent)["list"].array_items()) {
				int id = 0;
				if (!parse_area_id(area["id"], id))
					continue;
				obs_property_list_add_int(areaList, area["name"].string_value().c_str(), id);
				if (id == wantedArea)
					shownArea = id;
			}
		}
		if (wantedArea <= 0)
			obs_data_set_int(settings, KEY_AREA, shownArea);
	}
	obs_property_set_modified_callback(partList, on_part_changed);
	obs_property_set_modified_callback(areaList, on_value_changed);
	obs_property_set_enabled(partList, !g_busy);
	obs_property_set_enabled(areaList, !g_busy);
	add_action_button(props, "btn_apply_partition", Utils::Text("Btn.ApplyPart").c_str(), ACT_APPLY_PARTITION,
			  !g_busy);

	// --- 质量协商(§5.4:官方口径=推荐/默认码率+干预,无档位/无回执) ---
	// 只读展示:插件不改写 OBS 配置,视频码率由用户在 OBS 编码器设置里自行配置(ADR-0005)。
	if (!g_quality_info.empty())
		obs_properties_add_text(props, "st_quality", g_quality_info.c_str(), OBS_TEXT_INFO);
	add_action_button(props, "btn_fetch_quality", Utils::Text("Btn.Quality").c_str(), ACT_FETCH_QUALITY,
			  !g_busy && cfg.login_status);

	// --- 开播/停播 ---
	// 单「开播」按钮自动判定备流资格(E-22 官方同款分派):startLive 内先查
	// GetVisibleMode,==5 自动发备流请求,否则自动按官方普通开播发送并提示。
	add_action_button(props, "btn_start_live", Utils::Text("Btn.StartLive").c_str(), ACT_START_LIVE,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming);
	add_action_button(props, "btn_stop_live", Utils::Text("Btn.StopLive").c_str(), ACT_STOP_LIVE,
			  !g_busy && cfg.streaming);
	// M1b: 仅"服务端在播而本地未推流"时可用(恢复=找回地址后展示,不碰推流按钮/OBS 配置)。
	add_action_button(props, "btn_recover", Utils::Text("Btn.Recover").c_str(), ACT_RECOVER,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming &&
				  g_session_in_progress);
	// 残留场次清理(真实停播信令):与恢复同场景互斥展示,两段式确认。
	if (g_session_in_progress && !cfg.streaming)
		add_action_button(props, "btn_end_session",
				  g_confirm_end_session ? Utils::Text("Btn.EndSessionConfirm").c_str()
							: Utils::Text("Btn.EndSession").c_str(),
				  ACT_END_SESSION, !g_busy && cfg.login_status && !cfg.room_id.empty());

	obs_data_release(settings);
	return props;
}

obs_source_info g_console_info = {};

} // namespace

namespace UI {

obs_source_info *ConsoleSourceInfo()
{
	if (g_console_info.id == nullptr) {
		g_console_info.id = CONSOLE_ID;
		g_console_info.type = OBS_SOURCE_TYPE_INPUT;
		g_console_info.output_flags = 0; // never rendered, never in a scene
		g_console_info.get_name = [](void *) -> const char * {
			return obs_module_text("Console.Name");
		};
		g_console_info.create = [](obs_data_t *, obs_source_t *source) -> void * {
			// OBS 32 property callbacks only receive the source's `data`
			// pointer, so the source handle itself is stashed here.
			return new ConsoleCtx{source};
		};
		g_console_info.destroy = [](void *data) {
			delete static_cast<ConsoleCtx *>(data);
		};
		g_console_info.get_properties = console_properties;
	}
	return &g_console_info;
}

void ConsoleCreate()
{
	g_alive = true;
	g_config.load();
	g_draft_title = g_config.config().title;
	g_draft_part = g_config.config().part_id;
	g_draft_area = g_config.config().area_id;
	g_source = obs_source_create_private(CONSOLE_ID, obs_module_text("Console.Name"), nullptr);

	// Session identity boot: config.json carries the persisted cookie
	// whitelist into the jar; server-side rotations of those keys come back
	// through the change callback (worker thread → UI thread → config.json).
	Http::HttpClient::load_session_cookies(g_config.config().cookies);
	Http::HttpClient::set_cookie_change_callback([](const std::string &persisted) {
		Http::HttpClient::postToUi(guarded([persisted]() {
			auto &cfg = g_config.config();
			if (cfg.cookies == persisted)
				return;
			cfg.cookies = persisted;
			g_config.save();
		}));
	});
	start_fingerprint_probe();

	if (Http::HttpClient::has_cookie("SESSDATA"))
		refresh_login_chain();
	else
		set_status(LEVEL_INFO, Utils::Text("Console.NotLoggedInHint"));
}

void ConsoleDestroy()
{
	g_alive = false;
	if (g_source) {
		obs_source_release(g_source);
		g_source = nullptr;
	}
}

void ConsoleOpenPanel()
{
	if (!g_source)
		return;
	// OBS 的 rtmp-services 会在启动时联网更新 services.json 并整表覆盖；用户打开
	// 控制台时再合并一次，保证私有 SRT 条目在 OBS 设置里可用（本次没有 rtmpsrt://
	// 线路时写入默认入口）。
	refresh_srt_catalog();
	// 草稿与已提交设置对齐(上次 OK/取消/异步刷新遗留的草稿不污染本次对话框:
	// 二级列表条目与下拉选中都以草稿为锚,见 on_part_changed)。无已提交值时回
	// 落到 config.json(= 属性表注入的默认值)。
	obs_data_t *s = obs_source_get_settings(g_source);
	g_draft_title = obs_data_has_user_value(s, KEY_TITLE) ? obs_data_get_string(s, KEY_TITLE)
							      : g_config.config().title;
	g_draft_part = obs_data_has_user_value(s, KEY_PART) ? static_cast<int>(obs_data_get_int(s, KEY_PART))
							    : g_config.config().part_id;
	g_draft_area = obs_data_has_user_value(s, KEY_AREA) ? static_cast<int>(obs_data_get_int(s, KEY_AREA))
							    : g_config.config().area_id;
	obs_data_release(s);
	obs_frontend_open_source_properties(g_source);
}

} // namespace UI
