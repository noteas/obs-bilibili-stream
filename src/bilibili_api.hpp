#pragma once
#include <string>
#include <vector>
#include "json11/json11.hpp"

namespace Bili {
namespace {
const std::string APP_KEY = "aae92bc66f3edfab";
const std::string APP_SECRET = "af125a0d5279fd576c1b4418a3e8276d";
}

struct Config {
	std::string room_id;
	std::string csrf_token;
	std::string cookies;
	std::string mid;
	std::string title;
	bool login_status = false;
	bool streaming = false;
	std::string rtmp_addr;
	std::string rtmp_code;
	int part_id = 2;
	int area_id = 86;
	// 隐藏配置(无 UI,手改 config.json):true = 走 Web 老端点
	// (room_id_by_uid / Room/update),默认 false = 官方端点(契约 §3.1/§4.2/§4.3)。
	bool use_legacy_endpoints = false;
	// 上游选点透传(§5.7):非空才随 startLive 注入;插件不做测速本身。
	std::string recommend_protocol;
	std::string recommend_ip;
};

// Endpoints never carry identity arguments: cookies, UA and client_traceid are
// attached by Http::HttpClient (fingerprint single egress). Config.cookies only
// mirrors the persisted whitelist for config.json compatibility.
class BiliApi {
public:
	static void init();
	static void cleanup();
	static bool getQrCode(std::string &qr_data, std::string &qr_key, std::string &message);
	// Success = login cookies landed in the session jar (Set-Cookie merge or
	// the URL-query fallback), not a returned cookie string.
	static bool qrLogin(const std::string &qr_key, std::string &message);
	static bool checkLoginStatus(std::string &message, std::string &mid);
	// Room bootstrap. Official = app-blink room/GetInfo?uId=<mid> (§3.3; parser
	// F-1802b3690 reads data.room_id, proven); legacy = room/v2/Room/room_id_by_uid.
	// With prefer_official, a GetInfo failure logs a warning and falls back to the
	// legacy endpoint (transparent degradation, no silent endpoint swaps).
	static bool getRoomIdAndCsrf(std::string &room_id, std::string &csrf_token, std::string &message,
				     bool prefer_official = true);
	static json11::Json getPartitionList(std::string &message);
	static bool startLive(Config &config, std::string &rtmp_addr, std::string &rtmp_code, std::string &message,
			      std::string &face_qr, std::string &mid);
	static bool stopLive(const Config &config, std::string &message);
	static bool updateRoomInfo(const Config &config, std::string &message, const std::string &title = "",
				   int areaId = -1);
	// buvid3/buvid4 device identity via x/frontend/finger/spi (web-side
	// endpoint; the official client accumulates buvid3 through its CEF cookie
	// jar, which we do not have). Injects into the jar and persists; failures
	// are logged only — every current endpoint tolerates a missing buvid3.
	static void fetch_buvid();
	// B3 (contract §2.4): cross-domain login propagation. GET sso/list
	// (csrf=bili_jct) → GET every data.sso[] URL following redirects so each
	// domain's full 302 chain lands its cookies in the jar. Non-fatal: the
	// official H5 treats an sso failure as a logged telemetry event, not an error.
	static bool propagateSso(std::string &message);
	// getHomePageLiveVersion (contract §3.4) result, cached process-wide:
	// build/curr_version are the shared parameter values later signed requests
	// carry. Worker-thread only (single HTTP queue serializes access).
	static bool ensureVersionInfo(std::string &message);
	static long cachedBuild();
	// §4.3 AnchorChangeRoomArea (播中改分区): room_id+platform=pc_link+build+area_id
	// +csrf+sign, all four literals builder-proven (F-1801e8790).
	static bool changeRoomArea(const Config &config, int area_id, std::string &message);
	// §3.1/§3.2 shared builder F-1800c3810 (room_id + device="pc" + common + sign).
	// endpoint = "getInfoByUser" (own room, session-keyed; callers pass their known
	// room_id — the official parse cluster reads no room_id field back) or
	// "getInfoByRoom". Returns data on success, null Json + message on failure.
	static json11::Json getRoomInfo(const std::string &endpoint, const std::string &room_id, std::string &message);
	// §3.3 app-blink room/GetInfo (builder F-1802b1e60): uId-only business param,
	// session-required; parser F-1802b3690 proven to read data.room_id (plus the
	// §3.1 family incl. live_status/live_key/title). Returns data or null Json.
	static json11::Json getRoomInfoByUid(const std::string &mid, std::string &message);
	// §4.2 UpdatePreLiveInfo (播前改标题等): sign covers common params ONLY, then
	// mobi_app=pc_link + title (non-empty) + csrf pair are appended (F-180151c10).
	static bool updatePreLiveInfo(const Config &config, const std::string &title, std::string &message);
	// §3.5 PreCheck (开播前合规/推广拦截): POST with NO business params (builder
	// F-1804c4c00: bare common+csrf+sign). Returns false when data.game_promotion
	// .need_notice blocks the start; block_message carries title/content for the
	// status line. Transport/parse failures return false with the failure message.
	static bool preLiveCheck(std::string &block_message);
	// §3.6 GetRoomPreLiveStatus: GET, no business params (session-keyed; parser
	// F-180447550 reads data.status int ONLY). Value set not pinned — surfaced
	// raw for observation.
	static bool getRoomPreLiveStatus(int &status, std::string &message);

private:
	static std::string appsign(const std::vector<std::pair<std::string, std::string>> &params,
				   const std::string &app_key, const std::string &app_sec);
};
} // namespace Bili
