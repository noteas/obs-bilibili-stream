// rtmpsrt:// 目标解析的离线回归测试（CTest: srt_target_test）。
// 口径出处 = E-25 §2/§3（librtmp：app = 第一个 '/' 后全部含查询串、只裁一个结尾 '/'）。
#include <cstdio>
#include <string>

#include "srt/srt_target.hpp"

using namespace Srt;

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

static void test_target()
{
	Target t;
	std::string e;
	// E-25 §2：librtmp 不看 '?'，app = 第一个 '/' 之后的全部（含查询串），只裁一个结尾 '/'。
	CHECK(ParseTarget("rtmpsrt://live-push.bilivideo.com/live-bvc/?a=1", "", t, e), "URL 带查询串解析");
	CHECK(t.host == "live-push.bilivideo.com", "host");
	CHECK(t.port == 1935, "默认端口 1935");
	CHECK(t.app == "live-bvc/?a=1", "app 含查询串（librtmp 口径）");
	CHECK(t.tc_url == "rtmpsrt://live-push.bilivideo.com/live-bvc/?a=1", "tcUrl = URL 起始至 app 末尾");
	CHECK(t.stream == "?a=1", "stream 取查询串（推流码为空时的回退）");

	CHECK(ParseTarget("rtmpsrt://h:10080/live-bvc/", "?streamname=x&key=y", t, e), "显式端口 + key");
	CHECK(t.port == 10080, "显式端口");
	CHECK(t.app == "live-bvc", "无查询串时 app = 路径");
	CHECK(t.stream == "?streamname=x&key=y", "stream=key（含前导 ? 原样，E-25 §3）");
	CHECK(t.tc_url == "rtmpsrt://h:10080/live-bvc", "tcUrl 截到 app（不补端口）");

	// 官方形态（E-24 §6）：地址自带鉴权查询串 + 另给推流码，两者并存。
	CHECK(ParseTarget("rtmpsrt://h:1935/live-bvc/?streamname=x&schedule=1", "key=k", t, e),
	      "地址查询串与推流码并存");
	CHECK(t.app == "live-bvc/?streamname=x&schedule=1", "并存时 app 仍含查询串");
	CHECK(t.stream == "key=k", "并存时 publish 名 = 推流码");
	CHECK(t.tc_url == "rtmpsrt://h:1935/live-bvc/?streamname=x&schedule=1", "并存时 tcUrl 含查询串");

	CHECK(ParseTarget("RTMPSRT://h/live-bvc//", "k", t, e), "scheme 大小写宽松 + 结尾斜杠");
	CHECK(t.app == "live-bvc/", "只裁一个结尾 '/'（librtmp 同款）");

	CHECK(!ParseTarget("rtmp://h/live-bvc/", "k", t, e), "拒绝 rtmp://");
	CHECK(!ParseTarget("rtmpsrt://h/live-bvc/", "", t, e), "缺推流码报错");
}

static void test_is_srt_line()
{
	CHECK(IsSrtLine("rtmpsrt://h/live-bvc/"), "小写命中");
	CHECK(IsSrtLine("RTMPSRT://h/live-bvc/"), "大写命中");
	CHECK(!IsSrtLine("rtmp://h/live-bvc/"), "rtmp 不是 SRT 线路");
	CHECK(!IsSrtLine("srt://h:9999"), "标准 srt 不是私有线路");
	CHECK(!IsSrtLine("rtmpsrtx://h/"), "前缀相近不误判");
}

int main()
{
	test_target();
	test_is_srt_line();
	if (g_fail != 0) {
		std::printf("%d FAILED\n", g_fail);
		return 1;
	}
	std::printf("all srt_target_test checks passed\n");
	return 0;
}
