// 服务目录合并的离线回归测试（CTest: srt_catalog_test）：
// MergeServiceCatalogJson = 只增/替换自己的条目，顶层键与其余条目原样保留。
// 用内联合成夹具保证自洽；系统上存在 OBS 自带 services.json 时额外对真实表跑一遍。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "srt/srt_catalog.hpp"
#include "json11/json11.hpp"

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                    \
			++g_fail;                                                                                      \
		} else {                                                                                               \
			std::printf("ok: %s\n", msg);                                                                  \
		}                                                                                                      \
	} while (0)

namespace {

// 合成一份与 OBS 用户级 services.json 同构的底表：$schema/额外顶层键必须原样保留。
std::string SyntheticBase()
{
	return R"({
  "$schema": "https://obsproject.com/schema/2022-02/service.schema.json",
  "format_version": 5,
  "services": [
    { "name": "A", "server": "rtmp://a", "common": true },
    { "name": "B站私有 SRT", "protocol": "bvc_srt",
      "servers": [ { "name": "旧", "url": "rtmpsrt://x/y/" } ] },
    { "name": "B", "server": "rtmp://b" }
  ],
  "future_key": { "kept": true }
}
)";
}

void CheckMergeAgainst(const std::string &base, int base_service_count, const char *label)
{
	const std::vector<Srt::CatalogLine> lines = {
		{"主线路3 · srt/txy", "rtmpsrt://live-push.bilivideo.com/live-bvc/"},
		{"主线路4 · srt/qn", "rtmpsrt://txy.live-push.bilivideo.com/live-bvc/"},
		{"重复条目", "rtmpsrt://live-push.bilivideo.com/live-bvc/"}, // 同 URL 要去重
	};
	std::string out, err;
	if (!Srt::MergeServiceCatalogJson(base, lines, out, err)) {
		std::printf("FAIL: 合并不应失败 (%s): %s\n", label, err.c_str());
		++g_fail;
		return;
	}
	std::string parse_error;
	const auto j = json11::Json::parse(out, parse_error);
	CHECK(j.is_object(), (std::string(label) + ": 输出是合法 JSON 对象").c_str());
	if (!j.is_object())
		return;

	// 顶层键一个不丢，一个不多
	const auto base_json = json11::Json::parse(base, parse_error);
	for (auto &kv : j.object_items())
		CHECK(!base_json[kv.first].is_null(),
		      (std::string(label) + ": 顶层键 " + kv.first + " 原本就存在").c_str());
	CHECK(j.object_items().size() == base_json.object_items().size(),
	      (std::string(label) + ": 顶层键数量不变").c_str());
	CHECK(!j["$schema"].is_null(), (std::string(label) + ": 保留 $schema").c_str());

	// 我们的条目恰好一条，字段口径正确
	int ours = 0;
	for (const auto &s : j["services"].array_items()) {
		if (s["name"].string_value() != Srt::kServiceEntryName)
			continue;
		++ours;
		CHECK(s["protocol"].string_value() == "bvc_srt", (std::string(label) + ": protocol=bvc_srt").c_str());
		CHECK(s["common"].bool_value(), (std::string(label) + ": common=true（不勾全量也可见）").c_str());
		CHECK(s["servers"].array_items().size() == 2,
		      (std::string(label) + ": servers 去重后 = 2 条本次线路").c_str());
		CHECK(s["servers"].array_items()[0]["url"].string_value() ==
			      "rtmpsrt://live-push.bilivideo.com/live-bvc/",
		      (std::string(label) + ": servers[0].url 是 rtmpsrt 地址").c_str());
	}
	CHECK(ours == 1, (std::string(label) + ": 我们的条目恰好一条（旧名字被就地替换）").c_str());
	// 总数 = 底表 - 被替换的旧名字条目 + 我们的 1 条
	int stale = 0;
	for (const auto &s : base_json["services"].array_items()) {
		const std::string name = s["name"].string_value();
		if (name == Srt::kServiceEntryName || name == "B站私有 SRT" || name == "bilibili_srt" ||
		    name == "bili_srt")
			++stale;
	}
	CHECK(j["services"].array_items().size() == static_cast<size_t>(base_service_count - stale + 1),
	      (std::string(label) + ": 服务条目总数 = 底表 - 旧条目 + 1").c_str());

	// 重复合并幂等
	std::string out2, err2;
	if (!Srt::MergeServiceCatalogJson(out, lines, out2, err2)) {
		std::printf("FAIL: 第二遍合并不应失败 (%s)\n", err2.c_str());
		++g_fail;
		return;
	}
	CHECK(out2 == out, (std::string(label) + ": 重复合并幂等（不叠条目）").c_str());
}

void TestNoLinesWritesDefaultEntry()
{
	std::string out, err;
	if (!Srt::MergeServiceCatalogJson(SyntheticBase(), {}, out, err)) {
		std::printf("FAIL: 空线路也必须能合并（能力声明）: %s\n", err.c_str());
		++g_fail;
		return;
	}
	std::string parse_error;
	const auto j = json11::Json::parse(out, parse_error);
	bool found = false;
	for (const auto &s : j["services"].array_items()) {
		if (s["name"].string_value() != Srt::kServiceEntryName)
			continue;
		found = true;
		CHECK(s["servers"].array_items().size() == 1, "无线路时写默认入口一条");
		CHECK(s["servers"].array_items()[0]["url"].string_value() ==
			      "rtmpsrt://live-push.bilivideo.com/live-bvc/",
		      "默认入口 URL");
	}
	CHECK(found, "无线路时条目仍在（能力声明）");
}

void TestBadBaseFailsCleanly()
{
	std::string out, err;
	CHECK(!Srt::MergeServiceCatalogJson("{ not json", {}, out, err), "坏 JSON 报错而不是崩");
	CHECK(!err.empty(), "坏 JSON 带错误信息");
	CHECK(!Srt::MergeServiceCatalogJson("[1,2]", {}, out, err), "根不是对象 = 拒绝");
}

} // namespace

int main()
{
	std::string parse_error;
	const auto base_json = json11::Json::parse(SyntheticBase(), parse_error);
	CheckMergeAgainst(SyntheticBase(), static_cast<int>(base_json["services"].array_items().size()), "合成夹具");
	TestNoLinesWritesDefaultEntry();
	TestBadBaseFailsCleanly();

	// 真实环境附加：系统装了 rtmp-services 的话，对官方表跑同一断言（官方 84 条 + 我们 1 条）。
	const char *real_paths[] = {
		"/usr/share/obs/obs-plugins/rtmp-services/services.json",
		"/usr/local/share/obs/obs-plugins/rtmp-services/services.json",
	};
	for (const char *path : real_paths) {
		std::ifstream in(path);
		if (!in)
			continue;
		std::ostringstream ss;
		ss << in.rdbuf();
		const std::string real = ss.str();
		const auto real_json = json11::Json::parse(real, parse_error);
		if (real_json.is_object())
			CheckMergeAgainst(real, static_cast<int>(real_json["services"].array_items().size()),
					"系统 services.json");
		break;
	}

	std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fail);
	return g_fail ? 1 : 0;
}
