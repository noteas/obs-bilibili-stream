#include "ui/console.hpp"

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

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
	ACT_STOP_LIVE,
	ACT_RECOVER,
	ACT_END_SESSION,
	ACT_FETCH_QUALITY,
	ACT_APPLY_BITRATE,
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
constexpr const char *KEY_BITRATE = "video_bitrate_kbps";

bool g_alive = false;
obs_source_t *g_source = nullptr;
Core::ConfigManager g_config;

bool g_busy = false;
std::string g_status;
int g_status_level = LEVEL_INFO;
std::string g_stream_info;
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
int g_draft_bitrate = 0;
// §5.4/§5.5 拉取结果的多行展示(推荐码率/干预/编码能力),空=未拉取。
std::string g_quality_info;

void set_status(int level, const std::string &text)
{
	g_status = text;
	g_status_level = level;
}

void refresh_props()
{
	if (g_alive && g_source)
		obs_source_update_properties(g_source);
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
		refresh_props();
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
		refresh_props();
		return;
	}
	if (!begin_action("刷新登录态"))
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
		int liveStatus = -1;   // §3.1 live_status 原值(M1b 断流找回判据)
	};
	const bool legacy = g_config.config().use_legacy_endpoints;
	Http::HttpClient::runAsync(
		[result, legacy]() {
			result->loggedIn = Bili::BiliApi::checkLoginStatus(result->message, result->mid);
			if (result->loggedIn)
				result->roomOk = Bili::BiliApi::getRoomIdAndCsrf(
					result->roomId, result->csrf, result->message, !legacy);
			if (result->roomOk) {
				// B3: 官方 H5 登录成功后刷新全域 Cookie;失败不影响登录态(仅日志)。
				std::string ssoMessage;
				Bili::BiliApi::propagateSso(ssoMessage);
				if (!legacy) {
					// §3.1 解析动作: live_status/have_live → 进行中场次,
					// lock_status_v2/lock_reason → 封禁提示。取不到只降级,不断链。
					std::string stateMessage;
					json11::Json data =
						Bili::BiliApi::getRoomInfo("getInfoByUser", result->roomId, stateMessage);
					if (data.is_object()) {
						const int live_status = data["live_status"].int_value();
						result->liveStatus = live_status;
						const char *state = live_status == 1 ? "直播中" : live_status == 2 ? "轮回" : "未开播";
						result->roomState = std::string("场次:") + state;
						if (data["lock_status_v2"].int_value() != 0)
							result->roomState += ", 锁定:" + data["lock_reason"].string_value();
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
			g_config.save();
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
					refresh_props();
				}));
			} else if (Core::openInBrowser(qrData)) {
				Http::HttpClient::postToUi(guarded([]() {
					set_status(LEVEL_INFO, "登录链接已在浏览器打开,等待确认…");
					refresh_props();
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
				if (Bili::BiliApi::qrLogin(qrKey, pollMsg)) {
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
						refresh_props();
					}));
				}
			}
			result->message = "二维码已过期,请重新点击扫码登录";
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
				set_status(LEVEL_WARNING, "登录已取消");
		}));
}

// M1 服务改写管线: 开播信令拿到的 addr/key 写进当前 profile 的推流服务
// (rtmp_custom 的 server 字段同时承载 rtmp:// 与 srt:// 方案,SRT 映射天然就位)。
// 双轴分离: 插件只写配置,"开始直播"永远由用户在 OBS 点。主线程调用。
void set_streaming_service(const std::string &addr, const std::string &code)
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile) {
		obs_log(LOG_WARNING, "无法访问 OBS profile 配置,推流地址未写入服务");
		return;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", addr.c_str());
	obs_data_set_string(settings, "key", code.c_str());
	const char *json = obs_data_get_json(settings);
	config_set_string(profile, "Service", "service", "rtmp_custom");
	config_set_string(profile, "Service", "settings", json ? json : "{}");
	if (json)
		bfree(const_cast<char *>(json)); // obs_data_get_json 分配, libobs 约定释放
	obs_data_release(settings);
	obs_frontend_save_streaming_service();
	obs_log(LOG_INFO, "推流地址已写入 OBS 推流服务(rtmp_custom)");
}

// M1b 断流找回(契约 §7): 服务端仍认为在播时找回推流地址/密钥并写回 OBS 服务。
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
			g_config.save();
			set_streaming_service(result->addr, result->code);
			g_stream_info = "推流地址: " + result->addr + "\n推流码: " + result->code;
			g_session_in_progress = false;
			set_status(LEVEL_INFO,
				   "已找回进行中场次(来源: " + result->source +
					   "),推流地址已写入 OBS 服务,点击 OBS\"开始直播\"续流");
		}));
}

void do_start_live()
{
	if (!begin_action("开播"))
		return;
	auto config = g_config.config();
	struct StartResult {
		bool ok = false;
		std::string message;
		std::string addr;
		std::string code;
		std::string faceQr;
		std::string mid;
	};
	auto result = std::make_shared<StartResult>();
	result->mid = config.mid;

	Http::HttpClient::runAsync(
		[config, result]() mutable {
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
			result->ok = Bili::BiliApi::startLive(config, result->addr, result->code, result->message,
							     result->faceQr, result->mid);
		},
		guarded([result]() {
			end_action();
			if (result->ok) {
				auto &cfg = g_config.config();
				cfg.streaming = true;
				cfg.rtmp_addr = result->addr;
				cfg.rtmp_code = result->code;
				if (!result->mid.empty())
					cfg.mid = result->mid;
				g_config.save();
				set_streaming_service(result->addr, result->code);
				g_stream_info = "推流地址: " + result->addr + "\n推流码: " + result->code;
				set_status(LEVEL_INFO, "开播成功,推流地址已写入 OBS 服务,点击 OBS\"开始直播\"推流");
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
				g_stream_info.clear();
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
	struct QualityResult {
		std::string lines;
	};
	auto result = std::make_shared<QualityResult>();
	Http::HttpClient::runAsync(
		[result]() {
			std::string message;
			json11::Json json = Bili::BiliApi::videoQualityConfig(message);
			if (json.is_object()) {
				const json11::Json bc = json["data"]["bitrate_config"];
				result->lines = "推荐码率: " + std::to_string((int)json["recommend_bitrate"].number_value()) +
						  " kbps, 默认: " + std::to_string((int)bc["default_bitrate"].number_value()) +
						  " kbps\n干预: 会员=" +
						  (bc["recommend_bitrate_member_intervention"].bool_value() ? "有" : "无") +
						  " 分区=" + (bc["recommend_bitrate_area_intervention"].bool_value() ? "有" : "无") +
						  ", strategy_source=" + std::to_string(json["strategy_source"].int_value());
			} else {
				result->lines = "推荐码率: 获取失败(" + message + ")";
			}
			// 编码能力各自降级,不互相牵连。
			bool canH265 = false, canAv1 = false;
			if (Bili::BiliApi::canUseH265Encoder(canH265, message))
				result->lines += "\nH265 可用: " + std::string(canH265 ? "是" : "否");
			else
				result->lines += "\nH265 查询失败: " + message;
			if (Bili::BiliApi::canUseAv1Encoder(canAv1, message))
				result->lines += " | AV1 硬编可用: " + std::string(canAv1 ? "是" : "否");
			else
				result->lines += " | AV1 查询失败: " + message;
		},
		guarded([result]() {
			end_action();
			g_quality_info = result->lines;
			set_status(LEVEL_INFO, "质量口径已更新");
		}));
}

// 只写码率、不动编码器类型(GLOSSARY 质量协商边界)。输出活动与否都允许
// (bitrate 属可动态更新的编码参数);找不到流输出时仅提示。主线程。
bool apply_bitrate_to_current_encoder(int kbps, std::string &message)
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output) {
		message = "当前没有流输出实例(设置页确认已选择推流服务)";
		return false;
	}
	obs_encoder_t *encoder = obs_output_get_video_encoder(output);
	obs_output_release(output);
	if (!encoder) {
		message = "找不到当前视频编码器";
		return false;
	}
	obs_data_t *encSettings = obs_encoder_get_settings(encoder);
	obs_data_set_int(encSettings, "bitrate", kbps);
	obs_encoder_update(encoder, encSettings); // void;生效与否以下面回读为准
	obs_data_release(encSettings);
	obs_data_t *check = obs_encoder_get_settings(encoder);
	const bool updated = (int)obs_data_get_int(check, "bitrate") == kbps;
	obs_data_release(check);
	obs_log(LOG_INFO, "质量协商: 视频码率写 %d kbps → 编码器 %s(%s)", kbps,
		obs_encoder_get_name(encoder), obs_encoder_get_id(encoder));
	if (!updated) {
		message = "编码器拒绝了码率更新";
		return false;
	}
	return true;
}

void do_apply_bitrate()
{
	const int kbps = g_draft_bitrate;
	if (kbps <= 0) {
		set_status(LEVEL_WARNING, "码率为空或为 0,未应用");
		refresh_props();
		return;
	}
	std::string message;
	if (!apply_bitrate_to_current_encoder(kbps, message)) {
		set_status(LEVEL_ERROR, "应用码率失败: " + message);
		return;
	}
	g_config.config().bitrate_kbps = kbps;
	g_config.save();
	set_status(LEVEL_INFO, "已应用视频码率 " + std::to_string(kbps) + " kbps(仅码率,编码器类型不变)");
}

void do_apply_title()
{
	const std::string title = g_draft_title;
	if (title.empty()) {
		set_status(LEVEL_WARNING, "标题为空,未提交");
		refresh_props();
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
		refresh_props();
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
		refresh_props();
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
	g_draft_bitrate = static_cast<int>(obs_data_get_int(settings, KEY_BITRATE));
}

bool on_value_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
	return false;
}

bool on_part_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
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
			set_status(LEVEL_WARNING, "将向服务端发送停播信令结束进行中场次——再点一次确认");
			refresh_props();
		} else {
			g_confirm_end_session = false;
			do_stop_live();
		}
		break;
	case ACT_FETCH_QUALITY:
		do_fetch_quality();
		break;
	case ACT_APPLY_BITRATE:
		do_apply_bitrate();
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

	// --- 状态 ---
	add_status_line(props, "st_login",
			cfg.login_status ? ("登录状态: 已登录 (mid=" + cfg.mid + ")") : std::string("登录状态: 未登录"),
			cfg.login_status ? LEVEL_INFO : LEVEL_WARNING);
	if (!cfg.room_id.empty())
		obs_properties_add_text(props, "st_room",
					("房间: " + cfg.room_id + "  标题: " + cfg.title).c_str(), OBS_TEXT_INFO);
	if (!g_status.empty())
		add_status_line(props, "st_last", g_status, g_status_level);
	if (!g_stream_info.empty())
		obs_properties_add_text(props, "st_stream", g_stream_info.c_str(), OBS_TEXT_INFO);

	// --- 登录 ---
	obs_property_t *btnLogin = obs_properties_add_button2(
		props, "btn_login", "扫码登录(在浏览器打开二维码)",
		+[](obs_properties_t *p, obs_property_t *b, void *d) { return on_button(p, b, d); },
		reinterpret_cast<void *>(ACT_LOGIN));
	obs_property_set_enabled(btnLogin, !g_busy);
	obs_property_t *btnRefresh = obs_properties_add_button2(
		props, "btn_refresh", "刷新登录态与房间信息",
		+[](obs_properties_t *p, obs_property_t *b, void *d) { return on_button(p, b, d); },
		reinterpret_cast<void *>(ACT_REFRESH));
	obs_property_set_enabled(btnRefresh, !g_busy);

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
		const int wantedPart = static_cast<int>(obs_data_get_int(settings, KEY_PART));
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
		const int wantedArea = static_cast<int>(obs_data_get_int(settings, KEY_AREA));
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
	obs_data_set_default_int(settings, KEY_BITRATE, cfg.bitrate_kbps);
	if (!g_quality_info.empty())
		obs_properties_add_text(props, "st_quality", g_quality_info.c_str(), OBS_TEXT_INFO);
	add_action_button(props, "btn_fetch_quality", "拉取推荐码率与编码能力", ACT_FETCH_QUALITY,
			  !g_busy && cfg.login_status && !cfg.use_legacy_endpoints);
	obs_property_t *bitrate = obs_properties_add_int(props, KEY_BITRATE, "视频码率 (kbps, 0=不干预)", 0,
							 100000, 50);
	obs_property_set_enabled(bitrate, !g_busy);
	obs_property_set_modified_callback(bitrate, on_value_changed);
	add_action_button(props, "btn_apply_bitrate", "应用码率(只写码率)", ACT_APPLY_BITRATE, !g_busy);

	// --- 开播/停播 ---
	add_action_button(props, "btn_start_live", "开播", ACT_START_LIVE,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming);
	add_action_button(props, "btn_stop_live", "停播", ACT_STOP_LIVE, !g_busy && cfg.streaming);
	// M1b: 仅"服务端在播而本地未推流"时可用(恢复=找回地址写回服务,不碰推流按钮)。
	add_action_button(props, "btn_recover", "恢复推流", ACT_RECOVER,
			  !g_busy && cfg.login_status && !cfg.room_id.empty() && !cfg.streaming &&
				  g_session_in_progress);
	// 残留场次清理(真实停播信令):与恢复同场景互斥展示,两段式确认。
	if (g_session_in_progress && !cfg.streaming)
		add_action_button(props, "btn_end_session",
				  g_confirm_end_session ? "确认结束场次!" : "结束进行中场次", ACT_END_SESSION,
				  !g_busy && cfg.login_status && !cfg.room_id.empty());
	add_action_button(props, "btn_open_room", "打开直播间", ACT_OPEN_ROOM, true);

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
	if (g_source)
		obs_frontend_open_source_properties(g_source);
}

void ConsoleStartLogin()
{
	if (g_alive)
		do_login();
}

void ConsoleOpenRoom()
{
	if (g_alive)
		open_live_room();
}

} // namespace UI
