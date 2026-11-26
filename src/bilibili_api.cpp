#include "bilibili_api.hpp"
#include "http_client.hpp"
#include "md5.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <mutex>
#include "plugin_utils.hpp"
#include "util/base.h"

namespace Bili {
// getHomePageLiveVersion cache (contract §3.4: build/curr_version feed the
// shared parameters of later signed requests). Touched on the single HTTP
// worker thread only — no locking needed.
namespace {
long g_build = 0;
std::string g_curr_version;
// App OAuth identity: official common-parameter injector FUN_180063f70 always
// adds access_key (from SecretCore), so every app-blink/startLive/stopLive
// request carries it. Set from config at boot and refreshed on the HTTP
// worker; mutex only guards the UI-thread boot write vs worker reads.
std::mutex g_access_mutex;
std::string g_access_key;
} // namespace

// Bilibili puts the human readable error text in either "message" or "msg"
// depending on the endpoint. Accept both instead of showing an empty status.
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
// bililive_secret.dll): every signed request carries ts, version, appkey, platform,
// build and access_key. access_key is the App-OAuth identity the official client
// obtains through the refresh_token exchange; the server uses it to distinguish
// the official client (notably stopLive's 60034 policy). `appkey` itself is
// injected by appsign(), so it is NOT listed here (the official map dedups keys;
// our vector does not).
// Callers must ensureVersionInfo() first: version/build values come from §3.4.
// Wire-verified against the public getInfoByRoom endpoint (code=0).
static std::vector<std::pair<std::string, std::string>> common_params()
{
	std::string access_key;
	{
		std::lock_guard<std::mutex> lock(g_access_mutex);
		access_key = g_access_key;
	}
	return {{"ts", std::to_string(time(nullptr))},
		{"version", g_curr_version},
		{"platform", "pc_link"},
		{"build", std::to_string(g_build)},
		{"access_key", access_key}};
}

void BiliApi::setAccessKey(const std::string &access_key)
{
	std::lock_guard<std::mutex> lock(g_access_mutex);
	g_access_key = access_key;
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
	// 官方 mini-login-v2 的 generate 带 source=$(origin)(LiveHime 为
	// main_electron_pc)与 go_url;source 决定服务端下发 App 登录结果
	// (poll 成功时的 refresh_token),因此这里与官方对齐。
	auto response = Http::HttpClient::get(
		"https://passport.bilibili.com/x/passport-login/web/qrcode/generate"
		"?source=main_electron_pc");
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

bool BiliApi::qrLogin(const std::string &qr_key, std::string &refresh_token, std::string &message)
{
	refresh_token.clear();
	std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" + qr_key +
			  "&source=main_electron_pc";
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

	// 官方 H5 在 poll 成功后把 data.refresh_token 交给原生
	// (`auth/setRefreshToken`),原生再用它换 access_key。旧版响应可能没有该字段。
	refresh_token = json["data"]["refresh_token"].string_value();
	if (!refresh_token.empty())
		obs_log(LOG_INFO, "扫码登录:拿到 refresh_token(将在本地换 access_key)");
	else
		obs_log(LOG_WARNING, "扫码登录:响应无 refresh_token,无法换取 App OAuth access_key"
				     "(stopLive 可能仍被 60034 拦截)");

	message = "二维码登录成功";
	return true;
}

bool BiliApi::refreshAccessToken(const std::string &refresh_token, std::string &access_key,
				 std::string &new_refresh_token, std::string &message)
{
	access_key.clear();
	new_refresh_token.clear();
	if (refresh_token.empty()) {
		message = "无 refresh_token，无法换取 access_key";
		return false;
	}
	// 官方 OAuth injector(FUN_1804bb820)只注 appkey/ts/platform;业务参数为
	// access_token + refresh_token,随后 appsign(FUN_1804b3d60)。POST 表单。
	// access_token 允许为空(首次交换时 SecretCore 里还没有 token)。
	std::vector<std::pair<std::string, std::string>> params = {
		{"ts", std::to_string(time(nullptr))},
		{"platform", "pc_link"},
		{"access_token", ""},
		{"refresh_token", refresh_token},
	};
	std::string data = appsign(params, APP_KEY, APP_SECRET);
	auto response =
		Http::HttpClient::post("https://passport.bilibili.com/api/v2/oauth2/refresh_token", data);
	obs_log(LOG_INFO, "换取 access_key: HTTP %ld", response.status);
	if (response.status != 200) {
		message = "换取 access_key 失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "换取 access_key 失败: " +
			  (err.empty() ? jsonErrorMessage(json) : err);
		obs_log(LOG_WARNING, "换取 access_key 失败: code=%d %s", json["code"].int_value(),
			message.c_str());
		return false;
	}
	const json11::Json token_info = json["data"]["token_info"];
	access_key = token_info["access_token"].string_value();
	new_refresh_token = token_info["refresh_token"].string_value();
	if (access_key.empty()) {
		message = "换取 access_key 失败: 响应缺少 data.token_info.access_token";
		return false;
	}
	setAccessKey(access_key);
	// token_info 与 cookie_info 同响应下发;cookie 落 jar,过期由服务端轮转。
	int cookies = 0;
	for (const auto &c : json["data"]["cookie_info"]["cookies"].array_items()) {
		const std::string name = c["name"].string_value();
		const std::string value = c["value"].string_value();
		if (name.empty() || value.empty())
			continue;
		Http::HttpClient::inject_cookie(name, value);
		++cookies;
	}
	obs_log(LOG_INFO, "access_key 已刷新(刷新令牌轮换: %s, cookies: %d)",
		new_refresh_token.empty() ? "否" : "是", cookies);
	message = "access_key 已刷新";
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

bool BiliApi::getRoomIdAndCsrf(std::string &room_id, std::string &csrf_token, std::string &message,
			       bool prefer_official)
{
	const std::string dede_user_id = Http::HttpClient::cookie_value("DedeUserID");
	if (dede_user_id.empty()) {
		obs_log(LOG_ERROR, "Cookie 中无 DedeUserID");
		message = "Cookie 中无 DedeUserID";
		return false;
	}

	if (prefer_official) {
		// §3.3 room/GetInfo (builder F-1802b1e60: 业务参数仅 uId, 公共+sign, GET);
		// parser F-1802b3690 实证回读 data.room_id。需登录态(未登录 -101)。
		std::string stateMessage;
		json11::Json data = getRoomInfoByUid(dede_user_id, stateMessage);
		if (data.is_object()) {
			const long long rid = (long long)data["room_id"].number_value();
			if (rid > 0) {
				room_id = std::to_string(rid);
				csrf_token = Http::HttpClient::cookie_value("bili_jct");
				if (!csrf_token.empty())
					return true;
				message = "无法从 Cookies 中提取 bili_jct";
				return false;
			}
			stateMessage = "room/GetInfo 响应无有效 room_id";
		}
		obs_log(LOG_WARNING, "官方房间引导失败,回退老端点: %s", stateMessage.c_str());
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

json11::Json BiliApi::getRoomInfoByUid(const std::string &mid, std::string &message)
{
	if (!ensureVersionInfo(message))
		return json11::Json();
	std::vector<std::pair<std::string, std::string>> params = {{"uId", mid}};
	auto cp = common_params();
	params.insert(params.end(), cp.begin(), cp.end());
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/room/GetInfo?" +
			  appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "room/GetInfo 失败，状态码: " + std::to_string(response.status);
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "room/GetInfo 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
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

bool BiliApi::preLiveCheck(std::string &block_message)
{
	// 实网钉死(2026-11-25 wire):GET——POST 该路径 405。builder F-1804c4c00 的
	// map=公共参数直接签名结论不变,传输层此前判错。实响应仅 data.realname_status。
	if (!ensureVersionInfo(block_message))
		return false;
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/preLive/PreCheck?" +
			  appsign(common_params(), APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		block_message = "PreCheck 失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		block_message = "PreCheck 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	obs_log(LOG_INFO, "PreCheck: %s", response.data.c_str());
	// 实网回读字段:实名未过则阻断(0=未实名)。
	if (json["data"].is_object() && !json["data"]["realname_status"].is_null() &&
	    json["data"]["realname_status"].int_value() == 0) {
		block_message = "开播被阻断: 账号实名未通过(PreCheck realname_status=0)";
		return false;
	}
	// PreLiveBizCheck 解析组(E-10 @0x112cfad): game_promotion.need_notice = 合规/推广弹窗阻断。
	// 实测本会话响应无此键,保留解析以防服务端按场景下发。
	const json11::Json promo = json["data"]["game_promotion"];
	if (promo.is_object() && promo["need_notice"].bool_value()) {
		block_message = "开播被合规提示阻断: " + promo["title"].string_value() + " - " +
				promo["content"].string_value();
		obs_log(LOG_WARNING, "PreCheck game_promotion 阻断: biz_id=%d cool_down_days=%d",
			promo["biz_id"].int_value(), promo["cool_down_days"].int_value());
		return false;
	}
	return true;
}

bool BiliApi::getRoomPreLiveStatus(int &status, std::string &message)
{
	if (!ensureVersionInfo(message))
		return false;
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/index/GetRoomPreLiveStatus?" +
			  appsign(common_params(), APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "GetRoomPreLiveStatus 失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "GetRoomPreLiveStatus 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	status = json["data"]["status"].int_value(); // F-180447550: 唯一回读字段
	return true;
}

json11::Json BiliApi::getActiveStream(const std::string &room_id, std::string &message)
{
	if (!ensureVersionInfo(message))
		return json11::Json();
	// 契约 §7.1(builder FUN_18014bf40 指令级):先调公共参数注入器
	// FUN_180063f70(ts/version/platform/build),再注 room_id + appkey/sign。
	// 业务参数只有 room_id,但公共参数组与其他 app-blink 请求一致。
	std::vector<std::pair<std::string, std::string>> params = {{"room_id", room_id}};
	auto cp = common_params();
	params.insert(params.end(), cp.begin(), cp.end());
	std::string url = "https://api.live.bilibili.com/live_stream/v1/StreamList/get_stream_by_roomId?" +
			  appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "get_stream_by_roomId 失败，状态码: " + std::to_string(response.status);
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "get_stream_by_roomId 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return json11::Json();
	}
	return json["data"];
}

bool BiliApi::getUpStreamRtmp(const std::string &room_id, bool &can_change, std::vector<std::string> &urls,
			      std::string &message)
{
	if (!ensureVersionInfo(message))
		return false;
	std::vector<std::pair<std::string, std::string>> params = {{"room_id", room_id}};
	auto cp = common_params();
	params.insert(params.end(), cp.begin(), cp.end());
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/live/GetUpStreamRtmp?" +
			  appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "GetUpStreamRtmp 失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "GetUpStreamRtmp 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	can_change = json["data"]["can_change"].bool_value();
	for (const auto &u : json["data"]["urls"].array_items())
		if (u.is_string())
			urls.push_back(u.string_value());
	message = json["data"]["message"].string_value(); // ReconnectLiveInfo 附言
	return true;
}

json11::Json BiliApi::videoQualityConfig(std::string &message)
{
	if (!ensureVersionInfo(message))
		return json11::Json();
	// F-1802d40f0: sign 覆盖公共参数;bitrate_intervention="1" sign 后追加(§5.4 例外)。
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/upStreamConfig/"
			  "GetUpStreamVideoQualityConfig?" +
			  appsign(common_params(), APP_KEY, APP_SECRET) + "&bitrate_intervention=1";
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "GetUpStreamVideoQualityConfig 失败，状态码: " + std::to_string(response.status);
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "GetUpStreamVideoQualityConfig 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return json11::Json();
	}
	obs_log(LOG_INFO, "视频质量配置: %s", response.data.c_str());
	return json;
}

bool BiliApi::canUseH265Encoder(bool &can_enable, std::string &message)
{
	if (!ensureVersionInfo(message))
		return false;
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/upStreamConfig/"
			  "CanUseH265Encoder?" +
			  appsign(common_params(), APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "CanUseH265Encoder 失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "CanUseH265Encoder 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	can_enable = json["data"]["can_enable"].bool_value(); // F-1802d08c0: 唯一回读字段
	return true;
}

bool BiliApi::canUseAv1Encoder(bool &can_hard_encode, std::string &message)
{
	if (!ensureVersionInfo(message))
		return false;
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v1/upStreamConfig/"
			  "CanUseAV1Encoder?" +
			  appsign(common_params(), APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = "CanUseAV1Encoder 失败，状态码: " + std::to_string(response.status);
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = "CanUseAV1Encoder 失败: " + (err.empty() ? jsonErrorMessage(json) : err);
		return false;
	}
	can_hard_encode = json["data"]["can_hard_encode"].bool_value(); // F-1802d0f30: 唯一回读字段
	return true;
}

bool BiliApi::startLive(Config &config, std::string &rtmp_addr, std::string &rtmp_code, std::string &message,
			std::string &face_qr, std::string &mid, bool with_backup, std::string *backup_addr,
			std::string *backup_code, std::string *face_page_url)
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
	// checked="0": 恒注 (itoa(0) 实证); type="1": custom-rtmp 配置默认关时恒注,
	// 值 = '1'+UI 开播类型索引, 插件无类型选择 ⇒ 索引 0 (F-180141ed0)。
	// backup_stream 只由备流 builder(F-180146070)注入;标准 builder 不带该键。
	std::vector<std::pair<std::string, std::string>> start_params = {{"room_id", config.room_id},
									 {"area_v2", std::to_string(config.area_id)},
									 {"checked", "0"},
									 {"type", "1"}};
	// 官方备流 builder 恒带 src(E-19/G1):值 = get_stream_by_roomId 响应
	// data.stream_line[].src,原样透传;服务端用它定位备流落点线路。
	// 实网观测 2026-11-26:缺伴参→70003,带伴参但无 src→70002,同会话普通
	// 开播始终成功 ⇒ 备流地址分配依赖 src。选取规则:checked!=0 条目优先,
	// 无则首条(官方 service 子对象只存单个 src,即选中线路)。取不到=告警
	// +无 src 发送(等价已知现状,不阻断)。
	if (with_backup) {
		std::string streamMsg;
		const json11::Json lines =
			getActiveStream(config.room_id, streamMsg)["stream_line"];
		// src 值域已定为数字(G1),防御性兼容字符串:原样透传,不猜。
		auto src_of = [](const json11::Json &line) {
			const json11::Json s = line["src"];
			return s.is_string() ? s.string_value() : s.dump();
		};
		for (size_t i = 0; i < lines.array_items().size(); i++) {
			const json11::Json &line = lines.array_items()[i];
			obs_log(LOG_INFO, "stream_line[%zu]: name=%s cdn_name=%s src=%s checked=%d", i,
				line["name"].string_value().c_str(), line["cdn_name"].string_value().c_str(),
				src_of(line).c_str(), line["checked"].bool_value() ? 1 : 0);
		}
		std::string src_value;
		for (const auto &line : lines.array_items()) {
			if (line["checked"].bool_value() && src_of(line) != "null") {
				src_value = src_of(line);
				break;
			}
		}
		if (src_value.empty() && !lines.array_items().empty() && src_of(lines.array_items()[0]) != "null")
			src_value = src_of(lines.array_items()[0]);
		if (src_value.empty())
			obs_log(LOG_WARNING, "stream_line 取不到 src(%s),备流请求将不带 src 发送(已知形态=70002)",
				streamMsg.c_str());
		else
			start_params.push_back({"src", src_value});
		start_params.push_back({"backup_stream", "1"});
		// U6 已闭合(E-19):官方备流 builder 恒传 param_9=2、param_10="Paylive"
		// (教室房 "ClassRoom",插件不做教室房)⇒ 默认恒注对齐。
		// 探针(隐藏配置)保留作实网复核通道:非空时覆盖官方值。
		start_params.push_back({"backup_stream_type", config.backup_stream_type_probe.empty()
								     ? "2"
								     : config.backup_stream_type_probe});
		start_params.push_back({"stream_biz", config.stream_biz_probe.empty() ? "Paylive"
										       : config.stream_biz_probe});
	}
	// 上游选点透传(§5.7 测速结果;插件不做测速,仅当配置非空才注入,契约 §4.1)。
	if (!config.recommend_protocol.empty())
		start_params.push_back({"recommend_protocol", config.recommend_protocol});
	if (!config.recommend_ip.empty())
		start_params.push_back({"recommend_ip", config.recommend_ip});
	start_params.push_back({"csrf_token", config.csrf_token});
	start_params.push_back({"csrf", config.csrf_token});
	auto cp = common_params();
	start_params.insert(start_params.end(), cp.begin(), cp.end());
	std::string start_data = appsign(start_params, APP_KEY, APP_SECRET);

	// 官方 LiveStreamingService.StartLive 的调用超时是 60,000 ms
	// (LIVE-CHAIN §2.6 / FUN_180d5f5e0);默认 10s 会在服务端排队时误判失败。
	auto response = Http::HttpClient::post("https://api.live.bilibili.com/room/v1/Room/startLive", start_data,
					       60000);
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
	// server's blocking payload (封禁/警告). It must surface verbatim as the
	// error status line instead of degrading into a generic "no address" report.
	// Full notice UX (button_url) is M1 scope.
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
			// 二维码交控制台/浏览器展示,不在此处开独立对话框。
			message = "需要人脸验证，请扫描二维码" +
				  (face_url.empty() ? std::string() : ": " + face_url);
			face_qr = face_url;
			return false;
		}
		if (code == 60043) {
			// 官方 60043 中间页(契约 §1.2/§3.9,二进制实证):
			// auth-middle.html?source_event=200。它是网页不是二维码,
			// 官方直接开认证窗口,控制台也应直接打开该 URL。
			std::string face_url =
				"https://www.bilibili.com/blackboard/live/auth-middle.html?source_event=200&mid=" + mid;
			obs_log(LOG_WARNING, "60043 需要人脸识别，URL: %s", face_url.c_str());
			message = "需要人脸验证，请在打开的页面完成认证后重新开播: " + face_url;
			if (face_page_url)
				*face_page_url = face_url;
			else
				face_qr = face_url;
			return false;
		}
		// 备流被服务端拒绝(实网 70003「获取推流地址失败,请稍后重试」):
		// 备流是可选能力,其分配失败不应阻断开播主链 —— 回退普通开播,
		// 并把服务端的拒绝原因经 message 上抛(成功分支由控制台展示)。
		// 人脸/风控类(code 60024/60043/notice/v_voucher)已在上方先返回,
		// 不会走到这里。
		if (with_backup) {
			const std::string reject = std::to_string(code) + " " + message;
			obs_log(LOG_WARNING, "备流开播失败(%s),回退普通开播", reject.c_str());
			if (startLive(config, rtmp_addr, rtmp_code, message, face_qr, mid, false, nullptr, nullptr,
				      face_page_url)) {
				message = "备流被服务端拒绝(" + reject + "),本场按普通开播";
				return true;
			}
			obs_log(LOG_ERROR, "备流回退普通开播也失败: %s", message.c_str());
		}
		return false;
	}

	// §4.1 第12步对成功分支的语义同样是弹窗阻断:有 notice 就不算可开播。
	if (!notice_line.empty()) {
		obs_log(LOG_ERROR, "开播被服务端 notice 阻断: %s", notice_line.c_str());
		message = notice_line;
		return false;
	}

	// 第12步(续): risk_extra.v_voucher = geetest4 风控凭证(§2.5 同族)。
	// 插件不内嵌 geetest;透出让用户经官方通道完成验证后重试。
	const std::string voucher = json["data"]["risk_extra"]["v_voucher"].string_value();
	if (!voucher.empty()) {
		obs_log(LOG_WARNING, "开播命中风控, v_voucher=%s", voucher.c_str());
		message = "命中服务端风控(需极验验证),凭证 " + voucher + ";请在浏览器登录直播页完成验证后重试";
		return false;
	}

	// 步骤2: need_face_auth + qr(code==0 时同样可能要求人脸,阻断开播)。
	if (json["data"]["need_face_auth"].bool_value()) {
		face_qr = json["data"]["qr"].string_value();
		obs_log(LOG_WARNING, "需要人脸认证(need_face_auth): %s", face_qr.c_str());
		message = "需要人脸认证,请完成二维码验证后重新开播";
		return false;
	}

	// 步骤3-6: 协议选择。默认协议字面量 "rtmp"(0x706d7472 实证)。
	// protocols[] 非空 → 逐条 StreamTarget(protocol/addr/new_link/provider)与 live_key 配对;
	// 否则旧式回落: rtmp.new_link 必须存在(缺→无效), addr 存在=有效。
	const std::string live_key = json["data"]["live_key"].string_value();
	// U1 观测: 记录响应给出的全部协议名(取值域钉死前的实网样本)。
	{
		std::string available;
		for (const auto &t : json["data"]["protocols"].array_items())
			available += (available.empty() ? "" : ",") + t["protocol"].string_value();
		if (!available.empty())
			obs_log(LOG_INFO, "startLive 可用协议: %s", available.c_str());
	}
	// 全量收集线路(ADR-0005):官方给什么展示什么,控制台逐条供用户抄走。
	// 有效性按官方解析序第5步:protocols[] 里有任意一条带 addr 即为成功
	// (不要求必须是 rtmp);rtmp_addr/code 只是兼容旧调用点的便捷副本。
	config.stream_targets.clear();
	bool valid = false;
	for (const auto &target : json["data"]["protocols"].array_items()) {
		Config::StreamTarget t;
		t.protocol = target["protocol"].string_value().empty() ? "rtmp"
								       : target["protocol"].string_value();
		t.provider = target["provider"].string_value();
		t.addr = target["addr"].string_value();
		// 直连线路的推流码 = 条目自带 code("?streamname=…&key=…"查询串)。
		// 实证(2026-11-26 实网):拿 live_key 当推流码会被 edge 在 publish 后
		// ~0.4s 内拒绝(EPIPE);live_key 只是官方解析器的内部配对存储(§4.1 第5步),
		// 不是推流用key。code 缺失(如鉴权在 URL 内的线路)才回落 live_key。
		t.key = target["code"].string_value().empty() ? live_key : target["code"].string_value();
		obs_log(LOG_INFO, "开播线路: protocol=%s provider=%s new_link=%s addr=%s",
			t.protocol.c_str(), t.provider.c_str(),
			target["new_link"].bool_value() ? "1" : "0", t.addr.c_str());
		if (t.addr.empty())
			continue;
		config.stream_targets.push_back(t);
		valid = true;
		if (t.protocol == "rtmp" && rtmp_addr.empty()) {
			rtmp_addr = t.addr;
			rtmp_code = t.key;
		}
	}
	if (!valid) {
		// 第6步旧式回落:rtmp.new_link 必须存在(缺→无效),addr 非空=有效。
		const json11::Json rtmp = json["data"]["rtmp"];
		if (rtmp.is_object() && !rtmp["new_link"].is_null()) { // 新式但无 protocols[]:key=code
			rtmp_addr = rtmp["addr"].string_value();
			rtmp_code = rtmp["code"].string_value();
			valid = !rtmp_addr.empty();
			if (valid)
				config.stream_targets.push_back(
					Config::StreamTarget{"rtmp", "", rtmp_addr, rtmp_code, false});
		}
	}
	if (!valid) {
		// 官方第9步原文案(丢字段=响应契约破坏,把原文留在日志里以便补钉)。
		obs_log(LOG_ERROR, "解析开播响应缺字段(missing fields!),原文: %s", response.data.c_str());
		message = "开播响应缺少推流字段(missing fields),详见日志原文";
		return false;
	}

	// 步骤7: protocols_backup[] 与 protocols[] 同构、与 live_key 配对(§4.1 解析序)。
	// 官方解析器无条件读取该数组;插件同样全量收集(空数组=无操作),是否请求
	// 备流只决定服务端是否下发。备流语义=主备双通道(GLOSSARY):只保存并展示
	// 线路,不写 OBS 配置、不驱动第二路。
	bool backup_rtmp = false;
	for (const auto &target : json["data"]["protocols_backup"].array_items()) {
		Config::StreamTarget t;
		t.protocol = target["protocol"].string_value().empty() ? "rtmp"
								       : target["protocol"].string_value();
		t.provider = target["provider"].string_value();
		t.addr = target["addr"].string_value();
		// 与主流同构:直连备流线路同样用条目自带 code(见上,实证)。
		t.key = target["code"].string_value().empty() ? live_key : target["code"].string_value();
		t.backup = true;
		obs_log(LOG_INFO, "备流线路: protocol=%s provider=%s new_link=%s addr=%s",
			t.protocol.c_str(), t.provider.c_str(),
			target["new_link"].bool_value() ? "1" : "0", t.addr.c_str());
		if (t.addr.empty())
			continue;
		config.stream_targets.push_back(t);
		if (backup_addr && backup_code && t.protocol == "rtmp" && !backup_rtmp) {
			*backup_addr = t.addr;
			*backup_code = t.key;
			backup_rtmp = true;
		}
	}
	if (backup_addr && backup_addr->empty())
		obs_log(LOG_WARNING, "请求了备流但响应无 protocols_backup rtmp 项(观测: %s)",
			response.data.c_str());

	// 步骤10: 官方成功日志形态(addr/new_link/key),key 只记掩码。
	obs_log(LOG_INFO, "开播成功: addr=%s key=%s", rtmp_addr.c_str(),
		rtmp_code.empty() ? "(空)" : "****");
	return true;
}

bool BiliApi::stopLive(const Config &config, std::string &message)
{
	// Contract §6.1 (builder F-1801482d0): common injector FUN_180063f70
	// (ts/version/appkey/platform/build/access_key) → room_id → appsign →
	// csrf pair (FUN_1804b5330). 官方请求不带 `platform` 单独项,platform
	// 只在公共参数组里。
	std::vector<std::pair<std::string, std::string>> stop_params = {{"room_id", config.room_id}};
	auto cp = common_params();
	stop_params.insert(stop_params.end(), cp.begin(), cp.end());
	stop_params.push_back({"csrf_token", config.csrf_token});
	stop_params.push_back({"csrf", config.csrf_token});
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
		// 60034(实网 2026-11-25):服务端只认直播姬的 App OAuth 身份。官方每个
		// app-blink 请求都带公共参数 access_key(FUN_180063f70);插件此前漏发,
		// 现在用扫码 refresh_token 换 access_key 并随 stopLive 签名发送。
		if (json["code"].int_value() == 60034) {
			bool have_access_key = false;
			{
				std::lock_guard<std::mutex> lock(g_access_mutex);
				have_access_key = !g_access_key.empty();
			}
			if (have_access_key)
				message = "服务端策略:仅直播姬可关播(60034)。已随请求发送 access_key "
					  "但仍被拒;请重新扫码登录刷新 App OAuth 身份后重试。"
					  "也可在 OBS 停止推流,无推流数分钟后服务端自动结束本场次";
			else
				message = "服务端策略:仅直播姬可关播(60034)。当前没有 App OAuth "
					  "access_key;请重新扫码登录以换取 access_key 后再停播。"
					  "也可在 OBS 停止推流,无推流数分钟后服务端自动结束本场次";
		} else
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
