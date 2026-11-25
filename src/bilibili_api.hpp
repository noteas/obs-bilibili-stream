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

private:
	static std::string appsign(const std::vector<std::pair<std::string, std::string>> &params,
				   const std::string &app_key, const std::string &app_sec);
};
} // namespace Bili
