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

#include "bilibili_api.hpp"
#include "core/config_manager.hpp"
#include "core/platform_util.hpp"
#include "core/qr_generator.hpp"
#include "http_client.hpp"
#include "json11/json11.hpp"
#include "plugin_utils.hpp"

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
	ACT_START_LIVE_BACKUP,
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
		set_status(LEVEL_WARNING, "已有操作进行中,请稍候(" + std::string(what) + " 未执行)");
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
		set_status(LEVEL_INFO, "登录状态: 未登录");
		return;
	}
	if (!begin_action("刷新登录态"))
		return;
	auto result = std::make_shared<struct LoginChainResult>();
	struct LoginChainResult {
		bool loggedIn = false;
		bool roomOk = false;
		bool tokenRefreshed = false;
		std::string message;
		std::string mid;
		std::string roomId;
		std::string csrf;
		std::string accessKey;
		std::string refreshToken;
		std::string roomState; // §3.1: 场次/封禁一句话(空=不可用)
		int liveStatus = -1;   // §3.1 live_status 原值(M1b 断流找回判据)
	};
	const bool legacy = g_config.config().use_legacy_endpoints;
	const std::string stored_refresh_token = g_config.config().refresh_token;
	Http::HttpClient::runAsync(
		[result, legacy, stored_refresh_token]() {
			result->loggedIn = Bili::BiliApi::checkLoginStatus(result->message, result->mid);
			// B3: 官方 H5 登录成功后刷新全域 Cookie(sso/list);失败不影响登录态(仅日志)。
			// 必须排在 access_key 交换**之前**:浏览器先完成全域 cookie 传播,
			// 再把 token 交给原生交换;此前顺序颠倒(交换跑在未传播状态上)。
			if (result->loggedIn) {
				std::string ssoMessage;
				Bili::BiliApi::propagateSso(ssoMessage);
			}
			// App OAuth 身份:官方登录后 SecretCore 一直持有 access_key,并在
			// 每次 app-blink 请求里发送。插件用持久化的 refresh_token 换/续。
			if (result->loggedIn && !stored_refresh_token.empty()) {
				std::string tokenMessage;
				if (Bili::BiliApi::refreshAccessToken(stored_refresh_token, result->accessKey,
								      result->refreshToken, tokenMessage)) {
					result->tokenRefreshed = true;
				} else {
					obs_log(LOG_WARNING, "刷新 access_key 失败(不影响登录): %s",
						tokenMessage.c_str());
				}
			}
			if (result->loggedIn)
				result->roomOk = Bili::BiliApi::getRoomIdAndCsrf(
					result->roomId, result->csrf, result->message, !legacy);
			if (result->roomOk) {
				// SSO 全域传播已提前到交换之前(B3)。
				if (!legacy) {
					// §3.3 room/GetInfo: live_status 实测在这里;§3.1
					// getInfoByUser 全树无该字段(2026-11-25 实网钉死,
					// 此前解析挂错端点导致在播恒显「未开播」)。
					// live_status 1=直播中 2=轮回;lock_status/lock_time
					// 为封禁信号。取不到只降级,不断链。
					std::string stateMessage;
					json11::Json data =
						Bili::BiliApi::getRoomInfoByUid(result->mid, stateMessage);
					if (data.is_object()) {
						const int live_status = data["live_status"].int_value();
						result->liveStatus = live_status;
						const char *state = live_status == 1 ? "直播中" : live_status == 2 ? "轮回" : "未开播";
						result->roomState = std::string("场次:") + state;
						if (data["lock_status"].int_value() != 0)
							result->roomState += ", 锁定中";
					} else {
						obs_log(LOG_WARNING, "房间态获取失败(不影响登录链): %s", stateMessage.c_str());
					}
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
			}
			if (result->tokenRefreshed) {
				cfg.access_key = result->accessKey;
				if (!result->refreshToken.empty())
					cfg.refresh_token = result->refreshToken;
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
					"登录状态: 已登录 (mid=" + result->mid +
					(result->roomOk ? ", 房间=" + result->roomId : ", 房间信息获取失败") +
					(result->roomState.empty() ? "" : ", " + result->roomState) + ")";
				if (g_session_in_progress && !cfg.streaming)
					status += " — 检测到服务端进行中场次而本地未推流,可点\"恢复推流\"找回";
				set_status(g_session_in_progress && !cfg.streaming ? LEVEL_WARNING : LEVEL_INFO,
					   status);
			} else {
				g_session_in_progress = false;
				set_status(LEVEL_ERROR, "登录状态: 未登录/已失效 - " + result->message);
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
			obs_log(LOG_INFO, "指纹: UA=%s | traceid=%s | buvid3=%s (%s)",
				Http::HttpClient::user_agent(), Http::HttpClient::traceid().c_str(),
				b3.empty() ? "获取失败" : b3.c_str(), from_config ? "来自配置" : "新取");
		},
		nullptr);
}

// One-shot login attempt: fetch QR, hand it to the browser, poll until the
// user confirms, the code expires, or the module unloads.
void do_login()
{
	if (!begin_action("扫码登录"))
		return;
	struct LoginResult {
		bool ok = false;
		std::string message;
		std::string refreshToken; // mini-login-v2 poll 的 App OAuth 凭证
	};
	auto result = std::make_shared<LoginResult>();

	Http::HttpClient::runAsync(
		[result]() {
			std::string qrData, qrKey, msg;
			if (!Bili::BiliApi::getQrCode(qrData, qrKey, msg)) {
				result->message = "获取二维码失败: " + msg;
				return;
			}

			// The panel cannot render images (ADR-0003): write the QR as SVG
			// and open it in the system browser; fall back to showing the URL.
			std::string target = qrData;
			const std::string svgPath = module_file("login_qr.svg");
			if (!svgPath.empty() && Core::QrGenerator::writeSvgFile(svgPath, qrData) &&
			    Core::openInBrowser(svgPath)) {
				Http::HttpClient::postToUi(guarded([]() {
					set_status(LEVEL_INFO, "二维码已在浏览器打开,等待扫码确认…");
				}));
			} else if (Core::openInBrowser(qrData)) {
				Http::HttpClient::postToUi(guarded([]() {
					set_status(LEVEL_INFO, "登录链接已在浏览器打开,等待确认…");
				}));
			} else {
				result->message = "无法打开浏览器,请手动访问: " + qrData;
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
				if (Bili::BiliApi::qrLogin(qrKey, result->refreshToken, pollMsg)) {
					result->ok = true;
					return;
				}
				if (pollMsg.find("失效") != std::string::npos) {
					result->message = pollMsg;
					return;
				}
				if (pollMsg != lastMessage) {
					lastMessage = pollMsg;
					std::string shown = pollMsg;
					Http::HttpClient::postToUi(guarded([shown]() {
						set_status(LEVEL_INFO, "登录轮询: " + shown);
					}));
				}
			}
			result->message = "二维码已过期,请重新点击扫码登录";
		},
		guarded([result]() {
			end_action();
			if (result->ok) {
				// 官方 H5 把 poll 的 refresh_token 交给原生换 access_key。
				// 先落盘,再走 refresh_login_chain(它会用该 token 换 access_key)。
				if (!result->refreshToken.empty()) {
					g_config.config().refresh_token = result->refreshToken;
					g_config.save();
				}
				refresh_login_chain();
				return;
			}
			if (!result->message.empty())
				set_status(LEVEL_ERROR, result->message);
			else
				set_status(LEVEL_WARNING, "登录已取消");
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
	if (!begin_action("恢复推流"))
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
				result->message = "推流找回失败: " + upMessage;
				return;
			}
			if (!canChange || urls.empty()) {
				result->message = "服务端不提供可推地址" +
						  (upMessage.empty() ? "" : ": " + upMessage);
				return;
			}
			if (config.rtmp_code.empty()) {
				result->message = "GetUpStreamRtmp 给出地址但本地无留存推流码,无法恢复密钥;请开播一次或手工填写";
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
				set_status(LEVEL_ERROR, "恢复推流失败: " + result->message);
				return;
			}
			auto &cfg = g_config.config();
			cfg.streaming = true;
			cfg.rtmp_addr = result->addr;
			cfg.rtmp_code = result->code;
			// 找回成功只落到展示线路表,不碰 OBS 配置。
			cfg.stream_targets.clear();
			cfg.stream_targets.push_back(Bili::Config::StreamTarget{
				"rtmp", "", result->addr, result->code, false});
			g_config.save();
			g_session_in_progress = false;
			set_status(LEVEL_INFO,
				   "已找回进行中场次(来源: " + result->source +
					   "),推流地址已展示(可复制),请手动填入 OBS 后点\"开始直播\"");
		}));
}

void do_start_live(bool with_backup)
{
	if (!begin_action(with_backup ? "开播(含备流)" : "开播"))
		return;
	auto config = g_config.config();
	struct StartResult {
		bool ok = false;
		std::string message;
		std::string addr;
		std::string code;
		std::string backupAddr;
		std::string backupCode;
		std::vector<Bili::Config::StreamTarget> targets;
		std::string faceQr;
		std::string facePage;
		std::string mid;
	};
	auto result = std::make_shared<StartResult>();
	result->mid = config.mid;

	Http::HttpClient::runAsync(
		[config, result, with_backup]() mutable {
			// 开播前置(官方序): PreCheck 合规拦截(§3.5)→ 播前状态观测(§3.6,
			// 取值域未钉死,仅记日志)→ startLive。legacy 下跳过 PreCheck
			// (官方端点专属)。
			if (!config.use_legacy_endpoints) {
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
			}
			result->ok = Bili::BiliApi::startLive(
				config, result->addr, result->code, result->message, result->faceQr, result->mid,
				with_backup, with_backup ? &result->backupAddr : nullptr,
				with_backup ? &result->backupCode : nullptr, &result->facePage);
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
				if (!result->backupAddr.empty()) {
					cfg.rtmp_backup_addr = result->backupAddr;
					cfg.rtmp_backup_code = result->backupCode;
				}
				cfg.stream_targets = result->targets;
				if (!result->mid.empty())
					cfg.mid = result->mid;
				g_config.save();
				const size_t n = cfg.stream_targets.size();
				// message 在成功分支只承载「备流被拒已回退」类提示。
				set_status(result->message.empty() ? LEVEL_INFO : LEVEL_WARNING,
					   "开播成功,已获取 " + std::to_string(n) +
						   " 条推流线路(地址/推流码可复制);插件未修改 OBS 配置,请手动填入 OBS 推流设置后点\"开始直播\"" +
						   (result->message.empty() ? "" : " — " + result->message));
			} else if (!result->facePage.empty()) {
				// 60043: auth-middle 是网页不是二维码,按官方直接开认证窗口。
				const bool shown = Core::openInBrowser(result->facePage);
				set_status(LEVEL_WARNING,
					   shown ? "需要人脸认证:认证页已在浏览器打开,完成后重新开播"
						 : "需要人脸认证,请手动访问: " + result->facePage);
			} else if (!result->faceQr.empty()) {
				// 60024: show the face-auth QR through the browser, same
				// channel as the login QR.
				const std::string svgPath = module_file("face_qr.svg");
				bool shown = (!svgPath.empty() && Core::QrGenerator::writeSvgFile(svgPath, result->faceQr) &&
					      Core::openInBrowser(svgPath)) ||
					     Core::openInBrowser(result->faceQr);
				set_status(LEVEL_WARNING,
					   shown ? "需要人脸认证:二维码已在浏览器打开,完成后重新开播"
						 : "需要人脸认证,请手动访问: " + result->faceQr);
			} else {
				set_status(LEVEL_ERROR, "开播失败: " + result->message);
			}
		}));
}

void do_stop_live()
{
	// 官方 CloseLiveRoomPre 的顺序是「先停流(输出)再停播(信令)」。
	// 插件不驱动 OBS 输出(ADR-0001/0005),因此推流仍在进行时先提示用户
	// 在 OBS 停止推流,避免房间已关而画面上传仍在继续。
	if (obs_frontend_streaming_active()) {
		set_status(LEVEL_WARNING,
			   "请先在 OBS 点击「停止推流」,再点「停播」(官方顺序:先停流再停播)");
		return;
	}
	if (!begin_action("停播"))
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
				g_config.config().rtmp_backup_addr.clear();
				g_config.config().rtmp_backup_code.clear();
				g_config.config().stream_targets.clear();
				g_config.save();
				set_status(LEVEL_INFO, "已停播");
			} else {
				set_status(LEVEL_ERROR, "停播失败: " + result->message);
			}
		}));
}

// M2 质量协商(§5.4/§5.5 实证模型):拉推荐码率+干预标志+编码能力,展示用。
// 官方口径无档位枚举/无回执端点(QualityCallback 是线路回执,不属本链)。
void do_fetch_quality()
{
	if (!begin_action("拉取质量口径"))
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
				std::string source = "=默认(本分区无干预)";
				for (const auto &tier : bc["recommend_bitrate_area_intervention"]
							     ["recommend_bitrate_area_v2_interventions"]
							     .array_items()) {
					bool hit = false;
					for (const auto &area : tier["area_v2_ids"].array_items())
						if (area.int_value() == area_id) {
							hit = true;
							break;
						}
					if (hit) {
						recommend = (int)tier["recommend_bitrate"].number_value();
						source = "分区干预:" + tier["strategy_source"].string_value();
						break;
					}
				}
				result->lines = "推荐码率: " + std::to_string(recommend) + " kbps (" + source +
						  "), 默认: " + std::to_string(default_bitrate) +
						  " kbps\n会员码率干预: " +
						  (bc["recommend_bitrate_member_intervention"].bool_value() ? "有"
													       : "无") +
						  "\n说明: 官方推荐值是建议,不是推流上限;未设置二级分区时按默认值。" +
						  "实际视频码率请在 OBS 编码器设置中自行配置。";
			} else {
				result->lines = "推荐码率: 获取失败(" + message + ")";
			}
			// 编码能力各自降级,不互相牵连。
			bool canH265 = false, canAv1 = false;
			if (Bili::BiliApi::canUseH265Encoder(canH265, message))
				result->lines += "\nH265 服务端许可: " + std::string(canH265 ? "是" : "否") +
						  "(受官方客户端身份/分区影响;本地硬件以 OBS 编码器列表为准)";
			else
				result->lines += "\nH265 查询失败: " + message;
			if (Bili::BiliApi::canUseAv1Encoder(canAv1, message))
				result->lines += " | AV1 服务端许可: " + std::string(canAv1 ? "是" : "否");
			else
				result->lines += " | AV1 查询失败: " + message;
		},
		guarded([result]() {
			end_action();
			g_quality_info = result->lines;
			set_status(LEVEL_INFO, "质量口径已更新");
		}));
}

void do_apply_title()
{
	const std::string title = g_draft_title;
	if (title.empty()) {
		set_status(LEVEL_WARNING, "标题为空,未提交");
		return;
	}
	if (!begin_action("更新标题"))
		return;
	auto config = g_config.config();
	struct TitleResult {
		bool ok = false;
		std::string message;
	};
	auto result = std::make_shared<TitleResult>();
	Http::HttpClient::runAsync(
		[config, title, result]() { result->ok = Bili::BiliApi::updateRoomInfo(config, result->message, title, -1); },
		guarded([title, result]() {
			end_action();
			if (result->ok) {
				g_config.config().title = title;
				g_config.save();
				set_status(LEVEL_INFO, "直播间标题已更新");
			} else {
				set_status(LEVEL_ERROR, "更新标题失败: " + result->message);
			}
		}));
}

void do_apply_partition()
{
	const int areaId = g_draft_area;
	const int partId = g_draft_part;
	if (areaId <= 0) {
		set_status(LEVEL_WARNING, "请先选择二级分区");
		return;
	}
	if (!begin_action("更新分区"))
		return;
	auto config = g_config.config();
	struct PartResult {
		bool ok = false;
		std::string message;
	};
	auto result = std::make_shared<PartResult>();
	Http::HttpClient::runAsync(
		[config, areaId, result]() { result->ok = Bili::BiliApi::updateRoomInfo(config, result->message, "", areaId); },
		guarded([areaId, partId, result]() {
			end_action();
			if (result->ok) {
				auto &cfg = g_config.config();
				cfg.area_id = areaId;
				cfg.part_id = partId;
				g_config.save();
				set_status(LEVEL_INFO, "直播间分区已更新");
			} else {
				set_status(LEVEL_ERROR, "更新分区失败: " + result->message);
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
	Http::HttpClient::runAsync(
		[result]() { result->data = Bili::BiliApi::getPartitionList(result->message); },
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
		set_status(LEVEL_WARNING, "房间号为空,请先登录并刷新");
		return;
	}
	const std::string url = "https://live.bilibili.com/" + cfg.room_id;
	if (!Core::openInBrowser(url))
		set_status(LEVEL_WARNING, "无法打开浏览器,请手动访问: " + url);
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

bool on_part_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
	// 二级下拉框跟随大类(用户报障 2026-11-26):OBS 在 modified 回调返回 true 后
	// 重建属性表,但 get_properties 只能读到**已提交**的 source 设置(对话框编辑值
	// 在重建后才经 UpdateSettings 回填控件)⇒ 二级列表条目必须按草稿里的新大类
	// 重建;且对话框现值若仍指向旧大类的二级条目,须自动切到新大类首个二级并写回
	// 对话框设置,否则下拉框显示不到位,看似未刷新。
	if (g_partitions.is_array()) {
		const json11::Json *parent = find_parent(g_partitions, g_draft_part);
		int first_area = 0;
		bool area_valid = false;
		if (parent) {
			for (const auto &area : (*parent)["list"].array_items()) {
				int id = 0;
				if (!parse_area_id(area["id"], id))
					continue;
				if (first_area == 0)
					first_area = id;
				if (id == g_draft_area)
					area_valid = true;
			}
		}
		if (!area_valid && first_area > 0) {
			obs_data_set_int(settings, KEY_AREA, first_area);
			g_draft_area = first_area;
		}
	}
	return true; // rebuild the dialog so the sub-area list follows the parent
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
		do_start_live(false);
		break;
	case ACT_START_LIVE_BACKUP:
		do_start_live(true);
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
			set_status(LEVEL_WARNING, "将向服务端发送停播信令结束进行中场次——再点一次确认");
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
		props, name, text,
		+[](obs_properties_t *p, obs_property_t *b, void *d) { return on_button(p, b, d); },
		reinterpret_cast<void *>(action));
	obs_property_set_enabled(prop, enabled);
	return prop;
}

obs_property_t *add_status_line(obs_properties_t *props, const char *name, const std::string &text, int level)
{
	obs_property_t *prop = obs_properties_add_text(props, name, text.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_type(
		prop, level == LEVEL_ERROR ? OBS_TEXT_INFO_ERROR : (level == LEVEL_WARNING ? OBS_TEXT_INFO_WARNING
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
			cfg.login_status ? ("登录状态: 已登录 (mid=" + cfg.mid + ")") : std::string("登录状态: 未登录"),
			cfg.login_status ? LEVEL_INFO : LEVEL_WARNING);
	// 扫码登录与登录态成对:未登录时是主入口,已登录时降级为换号动作。
	add_action_button(props, "btn_login",
			  cfg.login_status ? "重新扫码登录(换号)" : "扫码登录(在浏览器打开二维码)", ACT_LOGIN,
			  !g_busy);
	add_action_button(props, "btn_refresh", "刷新登录态与房间信息", ACT_REFRESH, !g_busy);
	if (!cfg.room_id.empty()) {
		obs_properties_add_text(props, "st_room",
					("房间: " + cfg.room_id + "  标题: " + cfg.title).c_str(), OBS_TEXT_INFO);
		add_action_button(props, "btn_open_room", "在浏览器打开B站直播间", ACT_OPEN_ROOM, true);
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
			const std::string label = std::string(t.backup ? "备流" : "主线路") +
						  std::to_string(idx) + " · " + t.protocol +
						  (t.provider.empty() ? "" : "/" + t.provider);
			const std::string addr_id = "cp_addr_" + std::to_string(idx);
			obs_properties_add_text(props, addr_id.c_str(),
						(label + " 地址(可复制)").c_str(), OBS_TEXT_DEFAULT);
			obs_data_set_default_string(settings, addr_id.c_str(), t.addr.c_str());
			if (!t.key.empty()) {
				const std::string key_id = "cp_key_" + std::to_string(idx);
				obs_properties_add_text(props, key_id.c_str(),
							(label + " 推流码(可复制)").c_str(), OBS_TEXT_DEFAULT);
				obs_data_set_default_string(settings, key_id.c_str(), t.key.c_str());
			}
		}
	}

	// --- 直播间设置 ---
	obs_property_t *title =
		obs_properties_add_text(props, KEY_TITLE, "直播间标题", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(title, !g_busy);
	obs_property_set_modified_callback(title, on_value_changed);
	add_action_button(props, "btn_apply_title", "应用标题", ACT_APPLY_TITLE, !g_busy);

	obs_property_t *partList =
		obs_properties_add_list(props, KEY_PART, "分区大类", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_t *areaList =
		obs_properties_add_list(props, KEY_AREA, "二级分区", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	ensure_partitions_loaded();
	if (!g_partitions.is_array()) {
		const char *placeholder = g_partitions_loading ? "分区列表加载中…" : "分区列表未加载(需先登录)";
		obs_property_list_add_int(partList, placeholder, 0);
		obs_property_list_add_int(areaList, placeholder, 0);
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
	add_action_button(props, "btn_apply_partition", "应用分区", ACT_APPLY_PARTITION, !g_busy);

	// --- 质量协商(§5.4:官方口径=推荐/默认码率+干预,无档位/无回执) ---
	// 只读展示:插件不改写 OBS 配置,视频码率由用户在 OBS 编码器设置里自行配置(ADR-0005)。
	if (!g_quality_info.empty())
		obs_properties_add_text(props, "st_quality", g_quality_info.c_str(), OBS_TEXT_INFO);
	add_action_button(props, "btn_fetch_quality", "拉取推荐码率与编码能力", ACT_FETCH_QUALITY,
			  !g_busy && cfg.login_status && !cfg.use_legacy_endpoints);

	// --- 开播/停播 ---
	add_action_button(props, "btn_start_live", "开播", ACT_START_LIVE,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming);
	add_action_button(props, "btn_start_live_backup", "开播(含备流)", ACT_START_LIVE_BACKUP,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming);
	add_action_button(props, "btn_stop_live", "停播", ACT_STOP_LIVE, !g_busy && cfg.streaming);
	// M1b: 仅"服务端在播而本地未推流"时可用(恢复=找回地址后展示,不碰推流按钮/OBS 配置)。
	add_action_button(props, "btn_recover", "恢复推流", ACT_RECOVER,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming &&
				  g_session_in_progress);
	// 残留场次清理(真实停播信令):与恢复同场景互斥展示,两段式确认。
	if (g_session_in_progress && !cfg.streaming)
		add_action_button(props, "btn_end_session",
				  g_confirm_end_session ? "确认结束场次!" : "结束进行中场次", ACT_END_SESSION,
				  !g_busy && cfg.login_status && !cfg.room_id.empty());

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
		g_console_info.get_name = [](void *) -> const char * { return "B站直播控制台"; };
		g_console_info.create = [](obs_data_t *, obs_source_t *source) -> void * {
			// OBS 32 property callbacks only receive the source's `data`
			// pointer, so the source handle itself is stashed here.
			return new ConsoleCtx{source};
		};
		g_console_info.destroy = [](void *data) { delete static_cast<ConsoleCtx *>(data); };
		g_console_info.get_properties = console_properties;
	}
	return &g_console_info;
}

void ConsoleCreate()
{
	g_alive = true;
	g_config.load();
	// App OAuth 身份随公共参数发送(官方 FUN_180063f70 恒注 access_key)。
	Bili::BiliApi::setAccessKey(g_config.config().access_key);
	// client_traceid 请求头:官方仅在会话 traceid 非空时才发(E-21);插件默认不
	// 自造,值(若要用)由隐藏配置 client_traceid_probe 注入。
	Http::HttpClient::set_traceid(g_config.config().client_traceid_probe);
	g_draft_title = g_config.config().title;
	g_draft_part = g_config.config().part_id;
	g_draft_area = g_config.config().area_id;
	g_source = obs_source_create_private(CONSOLE_ID, "B站直播控制台", nullptr);

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
		set_status(LEVEL_INFO, "未登录:点击\"扫码登录\"");
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
