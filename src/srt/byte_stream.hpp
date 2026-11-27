#pragma once

#include <cstddef>
#include <string>

namespace Srt {

// RTMP 只依赖这一个字节流接口：生产实现是 SRT（UDP），离线测试可换成 TCP/内存管道。
class ByteStream {
public:
	virtual ~ByteStream() = default;

	// 写满 len 字节；失败返回 false 并填 error。
	virtual bool Write(const void *data, size_t len, std::string &error) = 0;
	// 最多读 len 字节。>0 = 实际字节；0 = 超时；<0 = 对端关闭/错误（填 error）。
	virtual int Read(void *data, size_t len, int timeout_ms, std::string &error) = 0;
	virtual void Close() = 0;
};

} // namespace Srt
