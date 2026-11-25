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
	static bool getRoomIdAndCsrf(std::string &room_id, std::string &csrf_token, std::string &message);
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

private:
	static std::string appsign(const std::vector<std::pair<std::string, std::string>> &params,
				   const std::string &app_key, const std::string &app_sec);
};
} // namespace Bili
