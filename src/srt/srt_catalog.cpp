#include "srt/srt_catalog.hpp"

#include <obs-module.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "json11/json11.hpp"

namespace Srt {

namespace {

constexpr const char *kProtocol = "bvc_srt";
constexpr const char *kDefaultServerUrl = "rtmpsrt://live-push.bilivideo.com/live-bvc/";
constexpr int kFallbackFormatVersion = 5;
// 早期版本写过的条目名。OBS 按 `name` 认条目，不清掉的话下拉里会叠出重复条目，
// 所以合并时一并替换；同一份名单也用作 alt_names（OBS 的搜索别名）。
constexpr const char *kLegacyEntryNames[] = {"B站私有 SRT", "bilibili_srt", "bili_srt",
					     "Bilibili Live - SRT | 哔哩哔哩直播 - SRT"};

std::string ReadFile(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool WriteFile(const std::string &path, const std::string &text)
{
	std::error_code ec;
	const std::filesystem::path p(path);
	if (p.has_parent_path())
		std::filesystem::create_directories(p.parent_path(), ec);
	if (ec) {
		blog(LOG_WARNING, "[bvc-srt] 创建服务目录失败: %s", ec.message().c_str());
		return false;
	}

	const std::filesystem::path tmp = p.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) {
			blog(LOG_WARNING, "[bvc-srt] 写入临时 services.json 失败: %s", tmp.string().c_str());
			return false;
		}
		out << text;
		if (!out.good()) {
			blog(LOG_WARNING, "[bvc-srt] 写 services.json 失败: %s", tmp.string().c_str());
			return false;
		}
	}
	std::filesystem::rename(tmp, p, ec);
	if (ec) {
		std::filesystem::remove(p, ec);
		ec.clear();
		std::filesystem::rename(tmp, p, ec);
	}
	if (ec) {
		blog(LOG_WARNING, "[bvc-srt] 替换 services.json 失败: %s", ec.message().c_str());
		return false;
	}
	return true;
}

json11::Json MakeServiceEntry(const std::vector<CatalogLine> &lines, const std::string &default_server_name)
{
	json11::Json::array servers;
	std::set<std::string> seen;
	for (const auto &line : lines) {
		if (line.url.empty() || !seen.insert(line.url).second)
			continue;
		json11::Json::object server;
		server["name"] = line.name.empty() ? line.url : line.name;
		server["url"] = line.url;
		servers.emplace_back(std::move(server));
	}
	if (servers.empty()) {
		json11::Json::object server;
		server["name"] = default_server_name;
		server["url"] = kDefaultServerUrl;
		servers.emplace_back(std::move(server));
	}

	json11::Json::object entry;
	entry["name"] = kServiceEntryName;
	// 默认（未勾「显示全部服务」）也要出现在服务下拉里。
	entry["common"] = true;
	json11::Json::array alt_names;
	for (const char *legacy : kLegacyEntryNames)
		alt_names.emplace_back(legacy);
	entry["alt_names"] = json11::Json(std::move(alt_names));
	entry["protocol"] = kProtocol;
	entry["stream_key_link"] = "https://link.bilibili.com/p/center/index#/my-room/start-live";
	entry["servers"] = json11::Json(std::move(servers));
	return json11::Json(std::move(entry));
}

bool IsOurEntry(const json11::Json &item)
{
	if (!item.is_object())
		return false;
	const std::string name = item["name"].string_value();
	if (name == kServiceEntryName)
		return true;
	return std::find(std::begin(kLegacyEntryNames), std::end(kLegacyEntryNames), name) !=
	       std::end(kLegacyEntryNames);
}

} // namespace

bool MergeServiceCatalogJson(const std::string &base, const std::vector<CatalogLine> &lines, std::string &out,
			     std::string &error, const std::string &default_server_name)
{
	std::string parse_error;
	const json11::Json root = json11::Json::parse(base, parse_error);
	if (!root.is_object()) {
		error = parse_error.empty() ? "services.json 根不是对象" : parse_error;
		return false;
	}

	json11::Json::array services;
	for (const auto &item : root["services"].array_items()) {
		if (!IsOurEntry(item))
			services.push_back(item);
	}
	services.push_back(MakeServiceEntry(lines, default_server_name));

	// 顶层键原样保留（`$schema`、将来 OBS 可能新增的字段……），只替换我们负责的两项。
	json11::Json::object obj = root.object_items();
	obj["format_version"] = root["format_version"].is_number() ? root["format_version"]
								   : json11::Json(kFallbackFormatVersion);
	obj["services"] = json11::Json(std::move(services));
	out = json11::Json(std::move(obj)).dump() + "\n";
	return true;
}

void UpdateServiceCatalog(const std::vector<CatalogLine> &lines, const std::string &default_server_name)
{
	// 读-改-写整个文件必须串行。当前调用方全在 OBS UI 线程（开播成功回调、
	// 打开控制台），加锁是为未来新增调用方兜底，也明确这个函数的线程契约。
	static std::mutex write_mutex;
	std::lock_guard<std::mutex> lock(write_mutex);

	obs_module_t *mod = obs_get_module("rtmp-services");
	if (!mod) {
		blog(LOG_WARNING, "[bvc-srt] 未找到 rtmp-services 模块，跳过服务目录更新");
		return;
	}

	char *user_path_raw = obs_module_get_config_path(mod, "services.json");
	char *bundled_raw = obs_find_module_file(mod, "services.json");
	const std::string user_path = user_path_raw ? user_path_raw : "";
	const std::string bundled_path = bundled_raw ? bundled_raw : "";
	bfree(user_path_raw);
	bfree(bundled_raw);

	if (user_path.empty()) {
		blog(LOG_WARNING, "[bvc-srt] 无法定位 rtmp-services 用户配置目录");
		return;
	}

	// 用户已有自己的 services.json 时在其基础上合并；否则以 OBS 自带列表为底。
	std::string base = ReadFile(user_path);
	if (base.empty())
		base = ReadFile(bundled_path);
	if (base.empty()) {
		blog(LOG_WARNING, "[bvc-srt] 读取 services.json 失败（用户与 OBS 自带均不可用）");
		return;
	}

	std::string merge_error;
	std::string text;
	if (!MergeServiceCatalogJson(base, lines, text, merge_error, default_server_name)) {
		blog(LOG_WARNING, "[bvc-srt] services.json 解析失败，保持原文件不动: %s", merge_error.c_str());
		return;
	}
	if (text == base) {
		blog(LOG_DEBUG, "[bvc-srt] services.json 已是最新");
		return;
	}
	if (!WriteFile(user_path, text))
		return;

	blog(LOG_INFO,
	     "[bvc-srt] 已更新 OBS 服务目录（%s）：设置→直播→服务选「%s」，服务器下拉即本次写入的 "
	     "rtmpsrt:// 线路（本次无 rtmpsrt 线路时为默认入口），推流码按控制台复制粘贴手动填写"
	     "（插件不写用户推流配置）",
	     user_path.c_str(), kServiceEntryName);
}

} // namespace Srt
