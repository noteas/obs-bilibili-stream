#pragma once

#include <cstdint>
#include <string>

namespace Srt {

// 私有 SRT 线路的 scheme。librtmp 内部按大小写敏感的 strncmp 比协议名，我们对用户输入
// 放宽成大小写不敏感；「这条线路是不是私有 SRT」一律用 IsSrtLine() 判，避免各处口径不一。
inline constexpr const char *kSrtScheme = "rtmpsrt://";

bool IsSrtLine(const std::string &url);

// 一条私有 SRT 推流目标（rtmpsrt://）。字段口径与 `bvc_srt_output`/librtmp 对齐
// （E-24）：host/port 用于 SRT(UDP) 连接，app 用于 RTMP connect，stream 是 publish 的
// 推流名（= 服务端线路条目里的 code，原样，通常形如 `?streamname=…&key=…`），
// tcUrl 是原 URL 截到 app 末尾（保留 rtmpsrt scheme，官方同款）。
struct Target {
	std::string server_url;
	std::string host;
	uint16_t port = 1935;
	std::string app;
	std::string tc_url;
	std::string stream;
	// 以下来自 output settings（可选）；0 = 用官方默认值。
	int max_bandwidth = 0;    // B/s；0 → 2 MiB/s
	int recv_timeout_sec = 0; // 秒；0 → 3（官方 clamp 下界）
	std::string bind_ip;      // 空/"default" = 不绑定
};

// 解析 `rtmpsrt://host[:port]/app[/...][?query]` + 推流码。
// 端口缺省 1935（E-24）。纯 `srt://`/`rtmp://` 一律拒绝：本 output 只接私有 SRT 线路。
bool ParseTarget(const std::string &server_url, const std::string &stream_key, Target &out, std::string &error);

} // namespace Srt
