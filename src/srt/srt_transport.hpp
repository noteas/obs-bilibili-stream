#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <srt/srt.h>

#include "srt/byte_stream.hpp"
#include "srt/srt_target.hpp"

namespace Srt {

// 全局只调用一次 srt_startup()；Unregister 里对应一次 srt_cleanup()。
void EnsureSrtStarted();

// B站私有 SRT 传输。参数固定按官方 bvc-srt-stream.dll 逆向结论（E-24）：
//   TRANSTYPE=live, CONNTIMEO=10000ms, PAYLOADSIZE=1456, LATENCY=200ms,
//   TSBPD=off, TLPKTDROP=off, MAXBW=2MiB/s(可覆盖), LINGER=1s。
// 不设 streamid、不加密——鉴权在 RTMP URL 里。
class SrtTransport : public ByteStream {
public:
	SrtTransport(const Target &target);
	~SrtTransport() override;

	bool Connect(std::string &error);

	bool Write(const void *data, size_t len, std::string &error) override;
	int Read(void *data, size_t len, int timeout_ms, std::string &error) override;
	void Close() override;

	// 运行期改 MAXBW（官方 update 回调同款；失败只记录，不中断推流）。
	bool UpdateMaxBandwidth(int bytes_per_sec, std::string &error);

	uint64_t bytes_sent() const { return m_bytes_sent.load(std::memory_order_relaxed); }
	bool connected() const { return m_sock != SRT_INVALID_SOCK; }

private:
	bool ApplyOptions(std::string &error);
	bool SetIntOpt(SRT_SOCKOPT opt, int value, const char *name, std::string &error);
	bool SetInt64Opt(SRT_SOCKOPT opt, int64_t value, const char *name, std::string &error);

	Target m_target;
	SRTSOCKET m_sock = SRT_INVALID_SOCK;
	std::atomic<uint64_t> m_bytes_sent{0};
	std::mutex m_write_mutex;
	// LiveCC 要求每次 srt_recv 的缓冲区 >= 最大 payload（约 1316B），否则报
	// "buffer size too small"。RTMP 会按 1/11 字节读头，所以这里先收进大缓冲再分发。
	std::vector<uint8_t> m_read_buf;
	size_t m_read_off = 0;
	size_t m_read_len = 0;
};

} // namespace Srt
