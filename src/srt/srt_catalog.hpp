#pragma once

#include <string>
#include <vector>

namespace Srt {

// OBS「服务」下拉里那条目的名字。OBS 直接拿 services.json 的 `name` 原文当标签
// （rtmp-common.c 的 add_service），所以控制台/README/日志文案必须逐字引用这个常量。
// 双语「英文 | 本地语言」是 OBS 官方服务列表的既有约定。
inline constexpr const char *kServiceEntryName = "Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT";

// 无 rtmpsrt:// 线路时写入的占位入口名。仅作为默认回退值；OBS 内实际写入时由调用方
// 传入按当前 UI 语言本地化的名字（data/locale 键 Srt.DefaultServer）。
inline constexpr const char *kDefaultServerName = "B站 RTMPSRT 默认入口 | Default";

// 一条要写进 OBS「服务」下拉的私有 SRT 入口。
struct CatalogLine {
	std::string name;
	std::string url; // rtmpsrt://host[:port]/app/
};

// 把「B站私有 RTMPSRT」服务条目合并进 rtmp-services 的用户级 services.json
// （OBS 原生列表 + 本条目；协议 bvc_srt 由本插件的 output 注册）。
// 只改服务目录，不改用户已选的 service/编码/输出配置（ADR-0007）。
// default_server_name = 无线路时的占位入口显示名（本地化文案由调用方注入）。
void UpdateServiceCatalog(const std::vector<CatalogLine> &lines,
			  const std::string &default_server_name = kDefaultServerName);

// 纯函数版（便于离线验证）：把条目合并进 services.json 文本。成功返回 true。
bool MergeServiceCatalogJson(const std::string &base, const std::vector<CatalogLine> &lines, std::string &out,
			     std::string &error, const std::string &default_server_name = kDefaultServerName);

} // namespace Srt
