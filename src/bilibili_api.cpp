#include "bilibili_api.hpp"
#include "http_client.hpp"
#include "md5.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "plugin_utils.hpp"
#include "util/base.h"

namespace Bili {

// Bilibili puts the human readable error text in either "message" or "msg"
// depending on the endpoint. Accept both instead of showing an empty status.
static std::string jsonErrorMessage(const json11::Json &json)
{
	std::string message = json["message"].string_value();
	if (message.empty())
		message = json["msg"].string_value();
	return message;
}

// 响应体默认**不进日志**:成功响应里有推流码/live_key/登录 ticket/refresh_token,而 OBS
// 日志常被整份分享排查问题。只有显式设置环境变量 BILI_TRACE_HTTP=1 才落原文(排查契约
// 用,开着的日志不要外传);平时只记 HTTP 状态与字节数。
static bool trace_http()
{
	static const bool enabled = []() {
		const char *value = getenv("BILI_TRACE_HTTP");
		return value && strcmp(value, "1") == 0;
	}();
	return enabled;
}

static void log_response(const char *what, const Http::HttpResponse &response)
{
	if (trace_http())
		obs_log(LOG_INFO, "[trace] %s: HTTP %ld %s", what, response.status, response.data.c_str());
	else
		obs_log(LOG_INFO, "%s: HTTP %ld, %zu 字节", what, response.status, response.data.size());
}

// 信封的 code/message 摘要(不含 data)。
static std::string envelope_summary(const json11::Json &json)
{
	return "code=" + std::to_string(json["code"].int_value()) + " message=" + jsonErrorMessage(json);
}

// getHomePageLiveVersion cache (contract §3.4: build/curr_version feed the
// shared parameters of later signed requests). 只在 HTTP worker 线程读写(单队列
// 串行),UI 线程不触碰 ⇒ 无需加锁。
namespace {
long g_build = 0;
std::string g_curr_version;
} // namespace

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
// build and access_key. The official client's value comes from its App-OAuth
// SecretCore; the Web-login token has no bridge to that channel (E-20: the
// refresh_token exchange always answers -101), so the plugin always sends the
// key with an empty value — the same shape as an official Web-login session,
// and stopLive's 60034 policy has been proven to pass with an empty value.
// `appkey` itself is injected by appsign(), so it is NOT listed here (the
// official map dedups keys; our vector does not).
// Callers must ensureVersionInfo() first: version/build values come from §3.4.
// Wire-verified against the public getInfoByRoom endpoint (code=0).
static std::vector<std::pair<std::string, std::string>> common_params()
{
	return {{"ts", std::to_string(time(nullptr))},
		{"version", g_curr_version},
		{"platform", "pc_link"},
		{"build", std::to_string(g_build)},
		{"access_key", ""}};
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

bool BiliApi::getQrCode(std::string &qr_data, std::string &qr_key, std::string &message)
{
	// 官方 mini-login-v2 的 generate 带 source=$(origin)(LiveHime 为
	// main_electron_pc)与 go_url;这里与官方请求形状对齐(poll 的
	// refresh_token 已不再消费,见 qrLogin)。
	auto response = Http::HttpClient::get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate"
					      "?source=main_electron_pc");
	if (response.status != 200) {
		log_response("获取二维码", response);
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.Qr").c_str(),
				      std::to_string(response.status).c_str());
		if (!response.data.empty()) {
			message += Utils::Text("Api.DataSuffix", response.data.c_str());
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		message = Utils::Text("Api.FailJsonParse", err.c_str());
		return false;
	}

	qr_data = json["data"]["url"].string_value();
	qr_key = json["data"]["qrcode_key"].string_value();
	if (qr_data.empty() || qr_key.empty()) {
		message = Utils::Text("Api.QrNoData");
		return false;
	}

	message = Utils::Text("Api.QrOk", qr_data.c_str(), qr_key.c_str());
	return true;
}

Bili::QrPoll BiliApi::qrLogin(const std::string &qr_key, std::string &message)
{
	message.clear();
	std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" + qr_key +
			  "&source=main_electron_pc";
	auto response = Http::HttpClient::get(url);
	log_response("二维码轮询", response);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.QrPoll").c_str(),
				      std::to_string(response.status).c_str());
		if (!response.data.empty())
			message += Utils::Text("Api.DataSuffix", response.data.c_str());
		return QrPoll::Failed;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		obs_log(LOG_ERROR, "二维码轮询 JSON 解析失败: %s", err.c_str());
		message = Utils::Text("Api.FailJsonParse", err.c_str());
		return QrPoll::Failed;
	}

	// 契约 §2.2 data.code 取值域:0 成功 / 86038 失效(换码) / 86090 已扫码待确认 /
	// 86101 未扫码。未扫码与已扫码是轮询的正常节奏,不是错误(此前每 2 秒记一条 ERROR)。
	const int code = json["data"]["code"].int_value();
	switch (code) {
	case 86038:
		message = Utils::Text("Api.QrInvalid", json["message"].string_value().c_str());
		return QrPoll::Expired;
	case 86090:
		message = Utils::Text("Api.QrScanned");
		return QrPoll::Scanned;
	case 86101:
		message = Utils::Text("Api.QrNotScanned");
		return QrPoll::NotScanned;
	case 0:
		break;
	default:
		obs_log(LOG_WARNING, "二维码轮询异常 code: %d, message: %s", code,
			jsonErrorMessage(json).c_str());
		message = Utils::Text("Api.ApiError", code, jsonErrorMessage(json).c_str());
		return QrPoll::Failed;
	}

	// A successful poll answers with Set-Cookie: the HTTP layer has merged it
	// into the session jar already. Since 2026-08 the success URL is a
	// crossDomain ticket URL (passport.biligame.com/x/passport-login/web/
	// crossDomain?ticket=…) that no longer embeds SESSDATA/bili_jct/
	// DedeUserID; following it sets the login cookies via Set-Cookie, and the
	// legacy URL-query parameters stay as the last-resort fallback.
	// **ticket 必须无条件跟随**(官方 H5 浏览器行为如此):同账号重登时 jar 里
	// 有启动载入的旧 SESSDATA,曾按其存在短路跳过 ticket ⇒ 新会话 cookie 未落地、
	// 后续登录态请求仍带旧会话(实网 2026-11-26)。
	const std::string loginUrl = json["data"]["url"].string_value();
	if (!loginUrl.empty()) {
		obs_log(LOG_INFO, "跟随 crossDomain ticket URL 刷新 Cookies…");
		log_response("crossDomain", Http::HttpClient::get(loginUrl));
	}
	if (!Http::HttpClient::has_cookie("SESSDATA")) {
		if (!loginUrl.empty()) {
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
		message = Utils::Text("Api.NoCookies");
		return QrPoll::Failed;
	}

	message = Utils::Text("Api.QrLoginOk");
	return QrPoll::Success;
}

Parse::LoginState BiliApi::checkLoginStatus(std::string &message, std::string &mid)
{
	message.clear();
	// Contract §2.9: the official nav builder carries appsign (no business
	// params, just ts/appkey/sign).
	std::vector<std::pair<std::string, std::string>> nav_params = {{"ts", std::to_string(time(nullptr))}};
	std::string nav_url =
		"https://api.bilibili.com/x/web-interface/nav?" + appsign(nav_params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(nav_url);
	log_response("nav", response);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.LoginCheck").c_str(),
				      std::to_string(response.status).c_str());
		return Parse::LoginState::Unknown;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty()) {
		obs_log(LOG_ERROR, "nav JSON 解析失败: %s", err.c_str());
		message = Utils::Text("Api.FailJsonParse", err.c_str());
		return Parse::LoginState::Unknown;
	}

	const Parse::LoginState state = Parse::LoginFromNav(json, mid, message);
	obs_log(LOG_INFO, "检查登录状态: %s (mid=%s)",
		state == Parse::LoginState::LoggedIn    ? "已登录"
		: state == Parse::LoginState::LoggedOut ? "未登录"
							: "未知",
		mid.c_str());
	return state;
}

json11::Json BiliApi::getPartitionList(std::string &message)
{
	auto response = Http::HttpClient::get("https://api.live.bilibili.com/room/v1/Area/getList");
	log_response("分区列表", response);
	if (response.status != 200) {
		obs_log(LOG_ERROR, "获取分区列表失败，状态码: %ld", response.status);
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.Partitions").c_str(),
				      std::to_string(response.status).c_str());
		if (!response.data.empty()) {
			message += Utils::Text("Api.DataSuffix", response.data.c_str());
		}
		return json11::Json();
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || !json["data"].is_array()) {
		message = Utils::Text("Api.FailParse", Utils::Text("Ep.Partitions").c_str(),
				      (err.empty() ? Utils::Text("Api.PartNoData") : err).c_str());
		return json11::Json();
	}
	message = Utils::Text("Api.PartOk");
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
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.Version").c_str(),
				      std::to_string(version_response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(version_response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", Utils::Text("Ep.Version").c_str(),
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return false;
	}
	g_build = json["data"]["build"].int_value();
	g_curr_version = json["data"]["curr_version"].string_value();
	if (g_build == 0 || g_curr_version.empty()) {
		g_build = 0;
		g_curr_version.clear();
		message = Utils::Text("Api.VersionInvalid");
		return false;
	}
	obs_log(LOG_INFO, "直播版本信息: build=%ld curr_version=%s", g_build, g_curr_version.c_str());
	return true;
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
		message = Utils::Text("Api.SsoFail", (err.empty() ? jsonErrorMessage(json) : err).c_str());
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
	message = Utils::Text("Api.SsoDone", followed);
	obs_log(LOG_INFO, "%s", message.c_str());
	return true;
}

bool BiliApi::changeRoomArea(const Config &config, int area_id, std::string &message)
{
	if (config.room_id.empty() || config.csrf_token.empty()) {
		message = Utils::Text("Api.ConfigInvalid");
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
	log_response("改分区", response);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.Partition").c_str(),
				      std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", Utils::Text("Ep.Partition").c_str(),
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return false;
	}
	obs_log(LOG_INFO, "分区更新成功: area_id=%d", area_id);
	return true;
}

json11::Json BiliApi::getRoomInfoByUid(const std::string &mid, std::string &message)
{
	if (!ensureVersionInfo(message))
		return json11::Json();
	std::vector<std::pair<std::string, std::string>> params = {{"uId", mid}};
	auto cp = common_params();
	params.insert(params.end(), cp.begin(), cp.end());
	std::string url =
		"https://api.live.bilibili.com/xlive/app-blink/v1/room/GetInfo?" + appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", "room/GetInfo", std::to_string(response.status).c_str());
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "room/GetInfo",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return json11::Json();
	}
	return json["data"];
}

bool BiliApi::updatePreLiveInfo(const Config &config, const std::string &title, std::string &message)
{
	if (config.room_id.empty() || config.csrf_token.empty()) {
		message = Utils::Text("Api.ConfigInvalid");
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
	log_response("更新播前资料", response);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.PreLive").c_str(),
				      std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", Utils::Text("Ep.PreLive").c_str(),
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
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
		block_message = Utils::Text("Api.FailStatus", "PreCheck", std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		block_message =
			Utils::Text("Api.FailParse", "PreCheck", (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return false;
	}
	log_response("PreCheck", response);
	obs_log(LOG_INFO, "PreCheck realname_status=%d", json["data"]["realname_status"].int_value());
	// 实网回读字段:实名未过则阻断(0=未实名)。
	if (json["data"].is_object() && !json["data"]["realname_status"].is_null() &&
	    json["data"]["realname_status"].int_value() == 0) {
		block_message = Utils::Text("Api.RealnameBlocked");
		return false;
	}
	// PreLiveBizCheck 解析组(E-10 @0x112cfad): game_promotion.need_notice = 合规/推广弹窗阻断。
	// 实测本会话响应无此键,保留解析以防服务端按场景下发。
	const json11::Json promo = json["data"]["game_promotion"];
	if (promo.is_object() && promo["need_notice"].bool_value()) {
		block_message = Utils::Text("Api.ComplianceBlocked", promo["title"].string_value().c_str(),
					    promo["content"].string_value().c_str());
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
		message =
			Utils::Text("Api.FailStatus", "GetRoomPreLiveStatus", std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "GetRoomPreLiveStatus",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
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
		message =
			Utils::Text("Api.FailStatus", "get_stream_by_roomId", std::to_string(response.status).c_str());
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "get_stream_by_roomId",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return json11::Json();
	}
	return json["data"];
}

json11::Json BiliApi::getVisibleMode(std::string &message)
{
	if (!ensureVersionInfo(message))
		return json11::Json();
	// 契约 §3.10(builder F-18044c590,指令级):无业务参数,公共参数+appsign。
	// 解析器 F-18044d410 读 data.{visible_mode, guard_visible_type,
	// upower_privilege_type};E-22:upower_privilege_type 是官方开播分派
	// (F-180d5de60)选备流/普通 builder 的开关。
	std::vector<std::pair<std::string, std::string>> params = common_params();
	std::string url = "https://api.live.bilibili.com/xlive/app-blink/v2/preLive/GetVisibleMode?" +
			  appsign(params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::get(url);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", "GetVisibleMode", std::to_string(response.status).c_str());
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "GetVisibleMode",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
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
		message = Utils::Text("Api.FailStatus", "GetUpStreamRtmp", std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "GetUpStreamRtmp",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
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
		message = Utils::Text("Api.FailStatus", "GetUpStreamVideoQualityConfig",
				      std::to_string(response.status).c_str());
		return json11::Json();
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "GetUpStreamVideoQualityConfig",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return json11::Json();
	}
	log_response("GetUpStreamVideoQualityConfig", response);
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
		message = Utils::Text("Api.FailStatus", "CanUseH265Encoder", std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "CanUseH265Encoder",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
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
		message = Utils::Text("Api.FailStatus", "CanUseAV1Encoder", std::to_string(response.status).c_str());
		return false;
	}
	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		message = Utils::Text("Api.FailParse", "CanUseAV1Encoder",
				      (err.empty() ? jsonErrorMessage(json) : err).c_str());
		return false;
	}
	can_hard_encode = json["data"]["can_hard_encode"].bool_value(); // F-1802d0f30: 唯一回读字段
	return true;
}

// 把解析结果翻成给状态行看的本地化文案(解析层本身不依赖 obs)。
static std::string start_failure_text(const Parse::StartLive &result)
{
	switch (result.outcome) {
	case Parse::StartOutcome::Ok:
		return {};
	case Parse::StartOutcome::NeedFaceQr:
		// 60024 给二维码;code==0 的 need_face_auth 同一通道(官方同样出码)。
		if (result.code == 0)
			return Utils::Text("Api.FaceAuthNeeded");
		return Utils::Text("Api.FaceQr") + (result.face_qr.empty() ? std::string() : ": " + result.face_qr);
	case Parse::StartOutcome::NeedFacePage:
		return Utils::Text("Api.FacePage", result.face_page.c_str());
	case Parse::StartOutcome::Blocked:
		if (!result.notice_line.empty())
			return result.notice_line;
		return Utils::Text("Api.RiskVoucher", result.voucher.c_str());
	case Parse::StartOutcome::Rejected:
		return result.server_message.empty() ? Utils::Text("Api.StartErrCode", result.code)
						     : result.server_message;
	case Parse::StartOutcome::MissingFields:
		break;
	}
	return Utils::Text("Api.MissingFields");
}

Parse::StartOutcome BiliApi::startLive(Config &config, const std::string &mid, std::string &message,
				       std::string &face_qr, std::string &face_page)
{
	message.clear();
	face_qr.clear();
	face_page.clear();
	if (config.room_id.empty() || config.csrf_token.empty()) {
		obs_log(LOG_ERROR, "配置无效: room_id/csrf_token 为空,不能开播");
		message = Utils::Text("Api.ConfigInvalid");
		return Parse::StartOutcome::Rejected;
	}
	if (!ensureVersionInfo(message))
		return Parse::StartOutcome::Rejected;

	Parse::StartLive result;
	std::string fallback_note; // 备流被拒、回退普通开播成功时附带给用户的原因
	for (int attempt = 0; attempt < 2; ++attempt) {
		const bool with_backup = attempt == 0;
		// E-09 §1 order: business params → csrf pair → appsign → sign.
		// platform/build/version/ts arrive via the common injector clone.
		// checked="0": 恒注 (itoa(0) 实证); type="1": custom-rtmp 配置默认关时恒注,
		// 值 = '1'+UI 开播类型索引, 插件无类型选择 ⇒ 索引 0 (F-180141ed0)。
		std::vector<std::pair<std::string, std::string>> start_params = {
			{"room_id", config.room_id}, {"area_v2", std::to_string(config.area_id)},
			{"checked", "0"},            {"type", "1"}};
		std::string hint;        // 成功分支的提示(无备流资格)
		bool backup_sent = false; // 本次请求是否真的带了备流三参
		if (with_backup) {
			// E-22(2026-11-26,订正 E-19「官方恒带备流伴参」误判):官方开播分派
			// (bililive.dll F-180d5de60)按 upower_privilege_type(源自 §3.10
			// GetVisibleMode)switch:仅 ==5(日志字面 "PRELIVE_UPOWER")走备流 builder
			// F-180146070(src+backup_stream=1/type=2/stream_biz=Paylive);其余值
			// (0/2/3/0xffffffff)走标准 builder F-180141ed0=**不带备流参数**。
			// ⇒ 无备流资格账号官方根本不发备流请求;强发 = 服务端备流地址分配失败
			// = 70002(实网三次观测与此吻合,同会话普通开播恒成功)。对齐同款判定:
			// 先查 GetVisibleMode,==5 才注备流伴参(资格查询失败则按 case-5 形态
			// 强发,保留回退普通开播兜底)。src 仅备流 builder 注入(E-19/G1):值 =
			// get_stream_by_roomId 响应 data.stream_line[].src,原样透传,选取
			// checked!=0 优先、无则首条;取不到=告警+无 src 发送,不阻断。
			bool backup_gate = false;
			std::string vmMsg;
			const json11::Json vm = getVisibleMode(vmMsg);
			if (!vmMsg.empty()) {
				obs_log(LOG_WARNING, "GetVisibleMode 资格查询失败,按官方 case-5 形态强发备流参数: %s",
					vmMsg.c_str());
				backup_gate = true;
			} else {
				const int privilege = vm["upower_privilege_type"].int_value();
				obs_log(LOG_INFO,
					"备流资格 GetVisibleMode: upower_privilege_type=%d visible_mode=%d guard_visible_type=%d"
					"(官方仅 ==5 走备流 builder)",
					privilege, vm["visible_mode"].int_value(), vm["guard_visible_type"].int_value());
				backup_gate = privilege == 5;
				if (!backup_gate)
					hint = Utils::Text("Api.NoBackupPriv", privilege);
			}
			if (backup_gate) {
				backup_sent = true;
				std::string streamMsg;
				const json11::Json lines = getActiveStream(config.room_id, streamMsg)["stream_line"];
				// src 值域已定为数字(G1),防御性兼容字符串:原样透传,不猜。
				auto src_of = [](const json11::Json &line) {
					const json11::Json v = line["src"];
					return v.is_string() ? v.string_value() : v.dump();
				};
				std::string src_value;
				for (const auto &line : lines.array_items()) {
					if (line["checked"].bool_value() && src_of(line) != "null") {
						src_value = src_of(line);
						break;
					}
				}
				if (src_value.empty() && !lines.array_items().empty() &&
				    src_of(lines.array_items()[0]) != "null")
					src_value = src_of(lines.array_items()[0]);
				if (src_value.empty())
					obs_log(LOG_WARNING, "stream_line 取不到 src(%s),备流请求将不带 src 发送",
						streamMsg.c_str());
				else
					start_params.push_back({"src", src_value});
				start_params.push_back({"backup_stream", "1"});
				// 备流 builder F-180146070 恒传 param_8=2、param_9="Paylive"
				// (教室房 "ClassRoom",插件不做教室房)。
				start_params.push_back({"backup_stream_type", "2"});
				start_params.push_back({"stream_biz", "Paylive"});
			}
		}
		start_params.push_back({"csrf_token", config.csrf_token});
		start_params.push_back({"csrf", config.csrf_token});
		auto cp = common_params();
		start_params.insert(start_params.end(), cp.begin(), cp.end());
		const std::string start_data = appsign(start_params, APP_KEY, APP_SECRET);

		// 官方 LiveStreamingService.StartLive 的调用超时是 60,000 ms
		// (LIVE-CHAIN §2.6 / FUN_180d5f5e0);默认 10s 会在服务端排队时误判失败。
		auto response = Http::HttpClient::post("https://api.live.bilibili.com/room/v1/Room/startLive",
						       start_data, 60000);
		log_response("startLive", response);
		if (response.status != 200) {
			obs_log(LOG_ERROR, "开播失败，状态码: %ld", response.status);
			message = Utils::Text("Api.FailStatus", Utils::Text("Ep.StartLive").c_str(),
					      std::to_string(response.status).c_str());
			if (trace_http() && !response.data.empty())
				message += Utils::Text("Api.DataSuffix", response.data.c_str());
			return Parse::StartOutcome::Rejected;
		}

		std::string err;
		json11::Json json = json11::Json::parse(response.data, err);
		if (!err.empty()) {
			obs_log(LOG_ERROR, "解析开播响应失败: %s", err.c_str());
			message = Utils::Text("Api.FailParse", Utils::Text("Ep.StartLive").c_str(), err.c_str());
			return Parse::StartOutcome::Rejected;
		}
		result = Parse::StartLiveEnvelope(json, mid);
		obs_log(LOG_INFO, "startLive: %s, 线路 %zu 条", envelope_summary(json).c_str(),
			result.targets.size());
		for (const auto &t : result.targets)
			obs_log(LOG_INFO, "%s线路: protocol=%s provider=%s addr=%s", t.backup ? "备流" : "主",
				t.protocol.c_str(), t.provider.c_str(), t.addr.c_str());

		if (result.outcome == Parse::StartOutcome::Ok) {
			config.stream_targets = result.targets;
			bool has_backup = false;
			for (const auto &t : result.targets)
				has_backup = has_backup || t.backup;
			if (backup_sent && !has_backup)
				obs_log(LOG_WARNING, "请求了备流但响应无 protocols_backup 直连线路");
			// 成功分支的 message 只承载提示(备流被拒已回退 > 无资格已按普通发送)。
			message = !fallback_note.empty() ? Utils::Text("Api.BackupRejected", fallback_note.c_str()) : hint;
			// 步骤10: 官方成功日志形态,推流码只记掩码。
			obs_log(LOG_INFO, "开播成功: addr=%s key=%s", result.primary_addr.c_str(),
				result.primary_key.empty() ? "(空)" : "****");
			return Parse::StartOutcome::Ok;
		}

		message = start_failure_text(result);
		face_qr = result.face_qr;
		face_page = result.face_page;
		// 人脸/风控/notice 阻断类不重试;备流被服务端拒绝(实网 70003/70002「获取推流地址
		// 失败,请稍后重试」)且本次确实发了备流参数 ⇒ 备流是可选能力,回退一次普通开播,
		// 并把服务端的拒绝原因经 message 上抛。此前条件是 with_backup(不管有没有真发),
		// 无资格账号的任何失败都会被原样重发一次。
		// 只对「获取推流地址失败」两码回退(契约 §1.2 的备流分配失败码);未登录/风控/
		// 其它业务拒绝重发一次普通开播没有意义,官方结果回调对 startLive 失败也不重试。
		const bool address_allocation_failed = result.code == 70002 || result.code == 70003;
		if (result.outcome == Parse::StartOutcome::Rejected && backup_sent &&
		    address_allocation_failed) {
			fallback_note = std::to_string(result.code) + " " + result.server_message;
			obs_log(LOG_WARNING, "备流开播失败(%s),回退普通开播", fallback_note.c_str());
			continue;
		}
		return result.outcome;
	}
	// 回退的普通开播也失败:上面的 return 已给出失败文案,这里只为编译器。
	message = start_failure_text(result);
	return result.outcome;
}

bool BiliApi::stopLive(const Config &config, std::string &message)
{
	// Contract §6.1 (builder F-1801482d0): common injector FUN_180063f70
	// (ts/version/appkey/platform/build/access_key) → room_id → appsign →
	// csrf pair (FUN_1804b5330). 官方请求不带 `platform` 单独项,platform
	// 只在公共参数组里。
	if (config.room_id.empty() || config.csrf_token.empty()) {
		message = Utils::Text("Api.ConfigInvalid");
		return false;
	}
	if (!ensureVersionInfo(message))
		return false;
	std::vector<std::pair<std::string, std::string>> stop_params = {{"room_id", config.room_id}};
	auto cp = common_params();
	stop_params.insert(stop_params.end(), cp.begin(), cp.end());
	stop_params.push_back({"csrf_token", config.csrf_token});
	stop_params.push_back({"csrf", config.csrf_token});
	std::string data = appsign(stop_params, APP_KEY, APP_SECRET);
	auto response = Http::HttpClient::post("https://api.live.bilibili.com/room/v1/Room/stopLive", data);
	log_response("stopLive", response);
	if (response.status != 200) {
		message = Utils::Text("Api.FailStatus", Utils::Text("Ep.StopLive").c_str(),
				      std::to_string(response.status).c_str());
		if (!response.data.empty()) {
			message += Utils::Text("Api.DataSuffix", response.data.c_str());
		}
		return false;
	}

	std::string err;
	json11::Json json = json11::Json::parse(response.data, err);
	if (!err.empty() || json["code"].int_value() != 0) {
		const std::string detail = err.empty() ? jsonErrorMessage(json) : err;
		obs_log(LOG_ERROR, "停止直播失败: %s", detail.c_str());
		// 60034(实网 2026-11-25/26):服务端按 App 版本参数指纹放行停播;
		// access_key 为空同样可过(实网定案,E-20)。此处只如实上抛政策拦截。
		if (json["code"].int_value() == 60034) {
			message = Utils::Text("Api.Stop60034");
		} else
			message = Utils::Text("Api.FailParse", Utils::Text("Ep.StopLive").c_str(), detail.c_str());
		return false;
	}

	obs_log(LOG_INFO, "直播已停止");
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
