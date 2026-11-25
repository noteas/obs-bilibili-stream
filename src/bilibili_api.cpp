#include "bilibili_api.hpp"
#include "http_client.hpp"
#include "md5.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <sstream>
#include <iostream>
#include "plugin_utils.hpp"
#include "util/base.h"

namespace Bili {
// getHomePageLiveVersion cache (contract §3.4: build/curr_version feed the
// shared parameters of later signed requests). Touched on the single HTTP
// worker thread only — no locking needed.
namespace {
long g_build = 0;
std::string g_curr_version;
} // namespace

// Bilibili puts the human readable error text in either "message" or "msg"
// depending on the endpoint. Accept both instead of showing an empty dialog.
static std::string jsonErrorMessage(const json11::Json &json)
{
	std::string message = json["message"].string_value();
	if (message.empty())
		message = json["msg"].string_value();
	return message;
}

// Percent-encode a POST body value (titles carry & = % etc.).
static std::string urlEncode(const std::string &s)
{
	char *e = curl_easy_escape(nullptr, s.c_str(), 0);
	std::string out = e ? e : s;
	if (e)
		curl_free(e);
	return out;
}

// Clone of the official common-parameter injector (FUN_180063f70, decompiled from
// bililive_secret.dll): every signed request carries ts, version, appkey, platform
// and build. access_key is deliberately absent — it belongs to the App-OAuth
// identity path we do not implement. `appkey` itself is injected by appsign(), so
// it is NOT listed here (the official map dedups keys; our vector does not).
// Callers must ensureVersionInfo() first: version/build values come from §3.4.
// Wire-verified against the public getInfoByRoom endpoint (code=0).
static std::vector<std::pair<std::string, std::string>> common_params()
{
	return {{"ts", std::to_string(time(nullptr))},
		{"version", g_curr_version},
		{"platform", "pc_link"},
		{"build", std::to_string(g_build)}};
}

std::string BiliApi::appsign(const std::vector<std::pair<std::string, std::string>> &params, const std::string &app_key,
			     const std::string &app_sec)
{
	std::vector<std::pair<std::string, std::string>> sorted_params = params;
	sorted_params.emplace_back("appkey", app_key);
	std::sort(sorted_params.begin(), sorted_params.end());

	std::ostringstream query;
	for (size_t i = 0; i < sorted_params.size(); ++i) {
		if (i > 0)
			query << "&";
		query << sorted_params[i].first << "=" << sorted_params[i].second;
	}
	std::string query_str = query.str() + app_sec;

	unsigned char digest[16];
	Crypto::MD5Context ctx;
	Crypto::md5Init(&ctx);
	Crypto::md5Update(&ctx, reinterpret_cast<const unsigned char *>(query_str.c_str()), query_str.length());
	Crypto::md5Final(digest, &ctx);

	char md5_hex[33];
	for (int i = 0; i < 16; ++i) {
		snprintf(&md5_hex[i * 2], 3, "%02x", digest[i]);
	}
	md5_hex[32] = '\0';
	sorted_params.emplace_back("sign", md5_hex);

	query.str("");
	for (size_t i = 0; i < sorted_params.size(); ++i) {
		if (i > 0)
			query << "&";
		query << sorted_params[i].first << "=" << sorted_params[i].second;
	}
	return query.str();
}

void BiliApi::init()
{
	Http::HttpClient::init();
}

void BiliApi::cleanup()
{
	Http::HttpClient::cleanup();
}

bool BiliApi::getQrCode(std::string &qr_data, std::string &qr_key, std::string &message)
{
	auto response =
		Http::HttpClient::get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate");
	if (response.status != 200) {
		obs_log(LOG_ERROR, "获取二维码失败，状态码: %d, 数据: %s", response.status, response.data.c_str());
		message = "获取二维码失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {
			message += ", 数据: " + response.data;
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		message = "Json 解析失败: " + err;
		return false;
	}

	qr_data = json["data"]["url"].string_value();
	qr_key = json["data"]["qrcode_key"].string_value();
	if (qr_data.empty() || qr_key.empty()) {
		message = "无法提取二维码数据或密钥";
		return false;
	}

	message = "获取二维码成功，URL: " + qr_data + ", Key: " + qr_key;
	return true;
}

bool BiliApi::qrLogin(const std::string &qr_key, std::string &message)
{
	std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" + qr_key;
	auto response = Http::HttpClient::get(url);
	obs_log(LOG_INFO, "检查二维码登录状态: %s", response.data.c_str());
	if (response.status != 200) {
		message = "检查二维码登录状态失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {

			message += ", 数据: " + response.data;
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		obs_log(LOG_ERROR, "JSON 解析失败: %s", err.c_str());
		message = "JSON 解析失败: " + err;
		return false;
	}

	int code = json["data"]["code"].int_value();
	if (code != 0) {
		if (code == 86038) {
			obs_log(LOG_ERROR, "二维码已失效: %s", json["message"].string_value().c_str());
			message = "二维码已失效: " + json["message"].string_value();
		} else if (code == 86090) {
			obs_log(LOG_INFO, "二维码已扫描，等待确认");
			message = "二维码已扫描，等待确认";
		} else {
			obs_log(LOG_ERROR, "API 返回错误，code: %d, message: %s", code,
				json["message"].string_value().c_str());
			message = "API 返回错误，code: " + std::to_string(code) +
				  ", message: " + json["message"].string_value();
		}
		return false;
	}

	// A successful poll answers with Set-Cookie: the HTTP layer has merged it
	// into the session jar already. Since 2026-08 the success URL is a
	// crossDomain ticket URL (passport.biligame.com/x/passport-login/web/
	// crossDomain?ticket=…) that no longer embeds SESSDATA/bili_jct/
	// DedeUserID; following it sets the login cookies via Set-Cookie, and the
	// legacy URL-query parameters stay as the last-resort fallback.
	if (!Http::HttpClient::has_cookie("SESSDATA")) {
		std::string loginUrl = json["data"]["url"].string_value();
		if (!loginUrl.empty()) {
			obs_log(LOG_INFO, "从 crossDomain 跳转 URL 获取 Cookies...");
			Http::HttpClient::get(loginUrl);
		}
		if (!Http::HttpClient::has_cookie("SESSDATA")) {
			obs_log(LOG_INFO, "从 URL 解析 Cookies...");
			auto extractParam = [&](const std::string &key) -> std::string {
				std::string search = key + "=";
				size_t start = loginUrl.find(search);
				if (start == std::string::npos)
					return "";
				start += search.length();
				size_t end = loginUrl.find("&", start);
				if (end == std::string::npos)
					end = loginUrl.length();
				return loginUrl.substr(start, end - start);
			};
			for (const char *key : {"SESSDATA", "bili_jct", "DedeUserID"}) {
				std::string value = extractParam(key);
				if (!value.empty())
					Http::HttpClient::inject_cookie(key, value);
			}
		}
	}
	if (!Http::HttpClient::has_cookie("SESSDATA")) {
		obs_log(LOG_ERROR, "无法获取登录 Cookies");
		message = "无法获取登录 Cookies";
		return false;
	}

	message = "二维码登录成功";
	return true;
}

bool BiliApi::checkLoginStatus(std::string &message, std::string &mid)
{
	// Contract §2.9: the official nav builder carries appsign (no business
	// params, just ts/appkey/sign).
	std::vector<std::pair<std::string, std::string>> nav_params = {{"ts", std::to_string(time(nullptr))}};
	std::string nav_url = "https://api.bilibili.com/x/web-interface/nav?" + appsign(nav_params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(nav_url);
	obs_log(LOG_INFO, "检查登录状态: %s", response.data.c_str());
	if (response.status != 200) {
		message = "检查登录状态失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {
			message += ", 数据: " + response.data;
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		obs_log(LOG_ERROR, "JSON 解析失败: %s", err.c_str());
		message = "JSON 解析失败: " + err;
		return false;
	}

	bool is_login = json["data"]["isLogin"].bool_value();
	if (is_login) {
		mid = std::to_string(static_cast<long long>(json["data"]["mid"].number_value()));
	}
	obs_log(LOG_INFO, "检查登录状态: %s", is_login ? "已登录" : "未登录");
	message = "检查登录状态: " + std::string(is_login ? "已登录" : "未登录");
	return is_login;
}

bool BiliApi::getRoomIdAndCsrf(std::string &room_id, std::string &csrf_token, std::string &message)
{
	const std::string dede_user_id = Http::HttpClient::cookie_value("DedeUserID");
	if (dede_user_id.empty()) {
		obs_log(LOG_ERROR, "Cookie 中无 DedeUserID");
		message = "Cookie 中无 DedeUserID";
		return false;
	}

	std::string url = "https://api.live.bilibili.com/room/v2/Room/room_id_by_uid?uid=" + dede_user_id;
	auto response = Http::HttpClient::get(url);
	obs_log(LOG_INFO, "获取房间号: %s", response.data.c_str());
	if (response.status != 200) {
		message = "获取房间号失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {
			message += ", 数据: " + response.data;
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		obs_log(LOG_ERROR, "JSON 解析失败: %s", err.c_str());
		message = "Json 解析失败: " + err;
		return false;
	}
	if (json["code"].int_value() != 0) {
		const std::string detail = jsonErrorMessage(json);
		obs_log(LOG_ERROR, "API 返回错误，code: %d, message: %s", json["code"].int_value(), detail.c_str());
		message = "API 返回错误， code: " + std::to_string(json["code"].int_value()) + ", message: " + detail;
		return false;
	}

	room_id = std::to_string(json["data"]["room_id"].int_value());
	csrf_token = Http::HttpClient::cookie_value("bili_jct");
	if (csrf_token.empty()) {
		//obs_log(LOG_ERROR, "无法从 Cookies 中提取 bili_jct");
		return false;
	}

	//obs_log(LOG_INFO, "获取 room_id 和 csrf_token 成功: room_id=%s, csrf_token=%s", room_id.c_str(), csrf_token.c_str());
	return true;
}

json11::Json BiliApi::getPartitionList(std::string &message)
{
	auto response = Http::HttpClient::get("https://api.live.bilibili.com/room/v1/Area/getList");
	obs_log(LOG_INFO, "获取分区列表: %s", response.data.c_str());
	if (response.status != 200) {
		obs_log(LOG_ERROR, "获取分区列表失败，状态码: %d", response.status);
		message = "获取分区列表失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {
			message += ", 数据: " + response.data;
		}
		return json11::Json();
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || !json["data"].is_array()) {
		message = "解析分区列表失败: " + std::string(err.empty() ? "无数据数组" : err);
		return json11::Json();
	}
	message = "获取分区列表成功";
	return json["data"];
}

bool BiliApi::ensureVersionInfo(std::string &message)
{
	if (g_build != 0 && !g_curr_version.empty())
		return true;

	std::vector<std::pair<std::string, std::string>> version_params = {{"system_version", "2"},
									   {"ts", std::to_string(time(nullptr))}};
	std::string version_query = appsign(version_params, APP_KEY, APP_SECRET);
	std::string version_url =
		"https://api.live.bilibili.com/xlive/app-blink/v1/liveVersionInfo/getHomePageLiveVersion?" +
		version_query;

	auto version_response = Http::HttpClient::get(version_url);
	if (version_response.status != 200) {
		message = "获取直播版本信息失败，状态码: " + std::to_string(version_response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(version_response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "解析直播版本信息失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	g_build = json["data"]["build"].int_value();
	g_curr_version = json["data"]["curr_version"].string_value();
	if (g_build == 0 || g_curr_version.empty()) {
		g_build = 0;
		g_curr_version.clear();
		message = "无效的 build 或 curr_version";
		return false;
	}
	obs_log(LOG_INFO, "直播版本信息: build=%ld curr_version=%s", g_build, g_curr_version.c_str());
	return true;
}

long BiliApi::cachedBuild()
{
	return g_build;
}

bool BiliApi::propagateSso(std::string &message)
{
	const std::string csrf = Http::HttpClient::cookie_value("bili_jct");
	std::string url = "https://passport.bilibili.com/x/passport-login/web/sso/list";
	if (!csrf.empty())
		url += "?csrf=" + csrf;
	auto response = Http::HttpClient::get(url);
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "SSO 列表获取失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	int followed = 0;
	for (const auto &entry : json["data"]["sso"].array_items()) {
		if (!entry.is_string() || entry.string_value().empty())
			continue;
		// 每个 URL 的完整 302 链都会向自己的域下发 Cookie;结果本身不关心。
		Http::HttpClient::get_following(entry.string_value());
		++followed;
	}
	message = "SSO 传播完成(" + std::to_string(followed) + " 个域)";
	obs_log(LOG_INFO, "%s", message.c_str());
	return true;
}

bool BiliApi::changeRoomArea(const Config &config, int area_id, std::string &message)
{
	if (config.room_id.empty() || config.csrf_token.empty()) {
		message = "配置无效: 房间号/CSRF 缺失";
		return false;
	}
	if (!ensureVersionInfo(message))
		return false;

	// Builder F-1801e8790 adds room_id/platform/build/area_id; platform+build are
	// the same keys the common injector fills, so they arrive via common_params()
	// (the official std::map dedups; our vector must not double them).
	std::vector<std::pair<std::string, std::string>> params = {
		{"room_id", config.room_id},
		{"area_id", std::to_string(area_id)},
	};
	auto cp = common_params();
	params.insert(params.end(), cp.begin(), cp.end());
	params.push_back({"csrf_token", config.csrf_token});
	params.push_back({"csrf", config.csrf_token});
	std::string data = appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::post(
		"https://api.live.bilibili.com/xlive/app-blink/v2/room/AnchorChangeRoomArea", data);
	obs_log(LOG_INFO, "改分区: %s", response.data.c_str());
	if (response.status != 200) {
		message = "改分区失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "改分区失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	obs_log(LOG_INFO, "分区更新成功: area_id=%d", area_id);
	return true;
}

json11::Json BiliApi::getRoomInfo(const std::string &endpoint, const std::string &room_id, std::string &message)
{
	// Shared builder F-1800c3810 for getInfoByUser/getInfoByRoom (§3.1/§3.2):
	// room_id (caller's value), device="pc" inline literal, then the common
	// parameters, appsign and sign. GET — no csrf injection.
	if (!ensureVersionInfo(message))
		return json11::Json();
	std::vector<std::pair<std::string, std::string>> params = {
		{"room_id", room_id},
		{"device", "pc"},
	};
	auto cp = common_params();
	params.insert(params.end(), cp.begin(), cp.end());
	std::string url = "https://api.live.bilibili.com/xlive/app-room/v1/index/" + endpoint +
			  "?" + appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = endpoint + " 失败，状态码: " + std::to_string(response.status);
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = endpoint + " 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return json11::Json();
	}
	return json["data"];
}

bool BiliApi::updatePreLiveInfo(const Config &config, const std::string &title, std::string &message)
{
	if (config.room_id.empty() || config.csrf_token.empty()) {
		message = "配置无效: 房间号/CSRF 缺失";
		return false;
	}
	// Builder F-180151c10, instruction-level: appsign runs over the COMMON
	// parameters only; mobi_app/title/ai_title_task_id and the csrf pair are
	// inserted into the map AFTER sign — business values are deliberately not
	// covered by the signature (pipeline exception vs E-09, contract §4.2).
	// ai_title_task_id is omitted (non-empty-only injection; plugin does no AI titles).
	if (!ensureVersionInfo(message))
		return false;
	std::string data = appsign(common_params(), APP_KEY, APP_SECRET);
	data += "&mobi_app=pc_link";
	if (!title.empty())
		data += "&title=" + urlEncode(title);
	data += "&csrf_token=" + config.csrf_token + "&csrf=" + config.csrf_token;
	auto response = Http::HttpClient::post(
		"https://api.live.bilibili.com/xlive/app-blink/v1/preLive/UpdatePreLiveInfo", data);
	obs_log(LOG_INFO, "更新播前资料: %s", response.data.c_str());
	if (response.status != 200) {
		message = "更新播前资料失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "更新播前资料失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	obs_log(LOG_INFO, "播前资料更新成功: title_length=%zu", title.size());
	return true;
}

bool BiliApi::startLive(Config &config, std::string &rtmp_addr, std::string &rtmp_code, std::string &message,
			std::string &face_qr, std::string &mid)
{
	if (config.room_id.empty() || config.csrf_token.empty()) {
		obs_log(LOG_ERROR, "配置无效: room_id=%s, csrf_token=%s, title=%s", config.room_id.c_str(),
			config.csrf_token.c_str(), config.title.c_str());
		message = "配置无效: 房间号=" + config.room_id + ", csrf_token=" + config.csrf_token;
		return false;
	}

	if (!ensureVersionInfo(message))
		return false;

	// E-09 §1 order: business params → csrf pair → appsign → sign.
	// platform/build/version/ts arrive via the common injector clone.
	std::vector<std::pair<std::string, std::string>> start_params = {{"room_id", config.room_id},
									 {"area_v2", std::to_string(config.area_id)},
									 {"backup_stream", "0"},
									 {"csrf_token", config.csrf_token},
									 {"csrf", config.csrf_token}};
	auto cp = common_params();
	start_params.insert(start_params.end(), cp.begin(), cp.end());
	std::string start_data = appsign(start_params, APP_KEY, APP_SECRET);

	auto response = Http::HttpClient::post("https://api.live.bilibili.com/room/v1/Room/startLive", start_data);
	obs_log(LOG_INFO, "启动直播: %s", response.data.c_str());
	if (response.status != 200) {
		obs_log(LOG_ERROR, "启动直播失败，状态码: %ld", response.status);
		message = "启动直播失败，状态码: " + std::to_string(response.status);
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		obs_log(LOG_ERROR, "解析开播响应失败: %s", err.c_str());
		message = "解析开播响应失败: " + err;
		return false;
	}

	int code = json["code"].int_value();

	// B4① (contract §4.1 step 12, §1.2 dispatch table): data.notice is the
	// server's blocking dialog payload (封禁/警告). It must surface verbatim
	// as the error status line — currently a notice silently degrades into a
	// "未获取到推流地址" report. Full dialog UX (button_url) is M1 scope.
	std::string notice_line;
	if (json["data"]["notice"].is_object()) {
		const json11::Json notice = json["data"]["notice"];
		const std::string title = notice["title"].string_value();
		const std::string msg = notice["msg"].string_value();
		if (!title.empty() || !msg.empty()) {
			notice_line = (title.empty() ? "服务端提示" : title) + ": " + msg;
			const std::string url = notice["button_url"].string_value();
			if (!url.empty())
				notice_line += "(详情: " + url + ")";
		}
	}

	obs_log(LOG_INFO, "开始直播，mid: %s", mid.c_str());
	if (code != 0) {
		message = notice_line.empty() ? jsonErrorMessage(json) : notice_line;
		if (message.empty())
			message = "开播失败，错误码: " + std::to_string(code);

		// 如果是人脸识别
		if (code == 60024) {
			std::string face_url = json["data"]["qr"].string_value();
			obs_log(LOG_WARNING, "60024 需要人脸识别，URL: %s", face_url.c_str());
			// 这里可以弹出一个对话框或者在 UI 上显示二维码
			message = "需要人脸验证，请扫描二维码" + face_url;
			face_qr = face_url;
			return false;
		}
		if (code == 60043) {
			std::string face_url =
				"https://www.bilibili.com/blackboard/live/face-auth-middle.html?source_event=400&mid=" +
				mid;
			obs_log(LOG_WARNING, "60043 需要人脸识别，URL: %s", face_url.c_str());
			message = "需要人脸验证，请扫描二维码" + face_url;
			face_qr = face_url;
			return false;
		}
		return false;
	}

	// §4.1 第12步对成功分支的语义同样是弹窗阻断:有 notice 就不算可开播。
	if (!notice_line.empty()) {
		obs_log(LOG_ERROR, "开播被服务端 notice 阻断: %s", notice_line.c_str());
		message = notice_line;
		return false;
	}

	rtmp_addr = json["data"]["rtmp"]["addr"].string_value();
	rtmp_code = json["data"]["rtmp"]["code"].string_value();
	if (rtmp_addr.empty() || rtmp_code.empty()) {
		obs_log(LOG_ERROR, "无法解析 RTMP 地址或推流码");
		message = "开播请求成功，但未获取到推流地址，请稍后重试";
		return false;
	}

	//obs_log(LOG_INFO, "直播启动成功，RTMP 地址: %s, 推流码: %s", rtmp_addr.c_str(), rtmp_code.c_str());
	return true;
}

bool BiliApi::stopLive(const Config &config, std::string &message)
{
	// Contract §6.1 (builder F-1801482d0): room_id is the only business
	// parameter, csrf pair + ts/appkey/sign come from the pipeline — and the
	// official request carries no `platform`.
	std::vector<std::pair<std::string, std::string>> stop_params = {
		{"room_id", config.room_id},
		{"csrf_token", config.csrf_token},
		{"csrf", config.csrf_token},
		{"ts", std::to_string(time(nullptr))},
	};
	std::string data = appsign(stop_params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::post("https://api.live.bilibili.com/room/v1/Room/stopLive", data);
	obs_log(LOG_INFO, "停止直播: %s", response.data.c_str());
	if (response.status != 200) {
		message = "停止直播失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {
			message += ", 数据: " + response.data;
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		const std::string detail = err.empty() ? jsonErrorMessage(json) : err;
		obs_log(LOG_ERROR, "停止直播失败: %s", detail.c_str());
		message = "停止直播失败: " + detail;
		return false;
	}

	obs_log(LOG_INFO, "直播已停止");
	return true;
}

bool BiliApi::updateRoomInfo(const Config &config, std::string &message, const std::string &title, int areaId)
{
	if (title.empty() && areaId < 0) {
		message = "无可更新内容";
		return false;
	}

	if (!config.use_legacy_endpoints) {
		// 官方拆成两个端点(契约 §4.2/§4.3): 标题/封面 → UpdatePreLiveInfo,
		// 分区 → AnchorChangeRoomArea。老端点 Room/update 只在 legacy 开关下保留。
		if (!title.empty() && !updatePreLiveInfo(config, title, message))
			return false;
		if (areaId >= 0 && !changeRoomArea(config, areaId, message))
			return false;
		return true;
	}

	std::string data = "room_id=" + config.room_id + "&platform=pc_link";
	if (!title.empty()) {
		char *escaped = curl_easy_escape(nullptr, title.c_str(), 0);
		data += "&title=";
		data += escaped ? escaped : title;
		if (escaped)
			curl_free(escaped);
	}
	if (areaId >= 0) {
		data += "&area_id=" + std::to_string(areaId);
	}
	data += "&csrf_token=" + config.csrf_token + "&csrf=" + config.csrf_token;
	auto response = Http::HttpClient::post("https://api.live.bilibili.com/room/v1/Room/update", data);
	obs_log(LOG_INFO, "更新房间信息: %s", response.data.c_str());
	if (response.status != 200) {
		obs_log(LOG_ERROR, "获取更新房间信息失败，状态码: %ld", response.status);
		message = "获取更新房间信息失败，状态码: " + std::to_string(response.status);
		if (!response.data.empty()) {
			message += ", 数据: " + response.data;
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "更新直播间信息失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}

	obs_log(LOG_INFO, "直播间信息更新成功: %s", title.c_str());
	return true;
}

void BiliApi::fetch_buvid()
{
	auto response = Http::HttpClient::get("https://api.bilibili.com/x/frontend/finger/spi");
	if (response.status != 200) {
		obs_log(LOG_WARNING, "获取 buvid 失败，状态码: %ld（不影响现有功能，继续裸请求）", response.status);
		return;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		obs_log(LOG_WARNING, "获取 buvid 响应无效: %s", (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return;
	}
	const std::string b3 = json["data"]["b_3"].string_value();
	const std::string b4 = json["data"]["b_4"].string_value();
	if (b3.empty()) {
		obs_log(LOG_WARNING, "获取 buvid 响应缺少 b_3，忽略");
		return;
	}
	Http::HttpClient::inject_cookie("buvid3", b3);
	if (!b4.empty())
		Http::HttpClient::inject_cookie("buvid4", b4);
}
} // namespace Bili
