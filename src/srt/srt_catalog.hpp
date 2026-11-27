#pragma once

#include <string>
#include <vector>

namespace Srt {

// 一条要写进 OBS「服务」下拉的私有 SRT 入口。
struct CatalogLine {
	std::string name;
	std::string url; // rtmpsrt://host[:port]/app/
};

// 把「B站私有 SRT」服务条目合并进 rtmp-services 的用户级 services.json
// （OBS 原生列表 + 本条目；协议 bvc_srt 由本插件的 output 注册）。
// 只改服务目录，不改用户已选的 service/编码/输出配置（ADR-0007）。
void UpdateServiceCatalog(const std::vector<CatalogLine> &lines);

// 纯函数版（便于离线验证）：把条目合并进 services.json 文本。成功返回 true。
bool MergeServiceCatalogJson(const std::string &base, const std::vector<CatalogLine> &lines,
			     std::string &out, std::string &error);

} // namespace Srt
