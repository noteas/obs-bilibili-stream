#include "srt/srt_transport.hpp"

#include <algorithm>
#include <mutex>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace Srt {

namespace {

constexpr int kConnectTimeoutMs = 10000;   // E-24：官方 SRTO_CONNTIMEO
constexpr int kPayloadSize = 1456;         // E-24：官方 SRTO_PAYLOADSIZE
constexpr int kLatencyMs = 200;            // E-24：官方 SRTO_LATENCY
constexpr int64_t kDefaultMaxBandwidth = 2 * 1024 * 1024; // E-24：官方默认 2 MiB/s
constexpr int kSendChunk = 1316;           // LiveCC 最大 payload（官方 0x560=1376 在 libsrt 1.5 会超限）
constexpr size_t kRecvBuffer = 64 * 1024;  // >= LiveCC 最大 payload
// 官方把 SRTO_RCVTIMEO 设为 srt_recv_timeout_sec；我们固定 500ms 只作为读线程的
// 轮询粒度（不改变线上行为），这样 stop() 能在一个轮询周期内结束。
constexpr int kRecvPollMs = 500;

bool IsRecoverableTimeout(int code)
{
	return code == SRT_EASYNCRCV || code == SRT_ETIMEOUT;
}

} // namespace

void EnsureSrtStarted()
{
	static std::once_flag startup_once;
	std::call_once(startup_once, [] { srt_startup(); });
}

SrtTransport::SrtTransport(const Target &target) : m_target(target) {}

SrtTransport::~SrtTransport()
{
	Close();
}

bool SrtTransport::SetIntOpt(SRT_SOCKOPT opt, int value, const char *name, std::string &error)
{
	if (srt_setsockopt(m_sock, 0, opt, &value, sizeof(value)) == SRT_ERROR) {
		error = std::string("设置 ") + name + " 失败: " + srt_getlasterror_str();
		return false;
	}
	return true;
}

bool SrtTransport::SetInt64Opt(SRT_SOCKOPT opt, int64_t value, const char *name, std::string &error)
{
	if (srt_setsockopt(m_sock, 0, opt, &value, sizeof(value)) == SRT_ERROR) {
		error = std::string("设置 ") + name + " 失败: " + srt_getlasterror_str();
		return false;
	}
	return true;
}

bool SrtTransport::ApplyOptions(std::string &error)
{
	// 顺序与官方 RTMP_Connect0 一致：先 TRANSTYPE，再逐项覆盖。
	if (!SetIntOpt(SRTO_TRANSTYPE, SRTT_LIVE, "SRTO_TRANSTYPE", error))
		return false;
	if (!SetIntOpt(SRTO_CONNTIMEO, kConnectTimeoutMs, "SRTO_CONNTIMEO", error))
		return false;
	if (!SetIntOpt(SRTO_PAYLOADSIZE, kPayloadSize, "SRTO_PAYLOADSIZE", error))
		return false;
	if (!SetIntOpt(SRTO_LATENCY, kLatencyMs, "SRTO_LATENCY", error))
		return false;
	if (!SetIntOpt(SRTO_TLPKTDROP, 0, "SRTO_TLPKTDROP", error))
		return false;
	if (!SetIntOpt(SRTO_TSBPDMODE, 0, "SRTO_TSBPDMODE", error))
		return false;

	const int64_t maxbw = m_target.max_bandwidth > 0 ? m_target.max_bandwidth : kDefaultMaxBandwidth;
	if (!SetInt64Opt(SRTO_MAXBW, maxbw, "SRTO_MAXBW", error))
		return false;
	// SRT 的 SRTO_LINGER 类型是 struct linger（不是 int）；官方 4 字节写法在
	// libsrt 1.5 会直接报 Bad parameters。这里给标准 8 字节 linger(1s)。
	struct linger ling {
		1, 1
	};
	if (srt_setsockopt(m_sock, 0, SRTO_LINGER, &ling, sizeof(ling)) == SRT_ERROR) {
		error = std::string("设置 SRTO_LINGER 失败: ") + srt_getlasterror_str();
		return false;
	}
	if (!SetIntOpt(SRTO_RCVTIMEO, kRecvPollMs, "SRTO_RCVTIMEO", error))
		return false;
	return true;
}

bool SrtTransport::Connect(std::string &error)
{
	if (m_sock != SRT_INVALID_SOCK) {
		error = "SRT 已连接";
		return false;
	}

	EnsureSrtStarted();

	m_sock = srt_create_socket();
	if (m_sock == SRT_INVALID_SOCK) {
		error = std::string("创建 SRT socket 失败: ") + srt_getlasterror_str();
		return false;
	}

	if (!ApplyOptions(error)) {
		Close();
		return false;
	}

	// 可选本地绑定（官方 bind_ip / RTMPStream.BindIP 同款）。
	if (!m_target.bind_ip.empty() && m_target.bind_ip != "default") {
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo *local = nullptr;
		const int rc = getaddrinfo(m_target.bind_ip.c_str(), nullptr, &hints, &local);
		if (rc != 0 || !local) {
			error = "解析绑定地址失败: " + m_target.bind_ip;
			Close();
			return false;
		}
		bool bound = false;
		for (addrinfo *ai = local; ai; ai = ai->ai_next) {
			if (srt_bind(m_sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != SRT_ERROR) {
				bound = true;
				break;
			}
		}
		freeaddrinfo(local);
		if (!bound) {
			error = std::string("SRT 绑定本地地址失败: ") + srt_getlasterror_str();
			Close();
			return false;
		}
	}

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *result = nullptr;
	const std::string port = std::to_string(m_target.port);
	const int rc = getaddrinfo(m_target.host.c_str(), port.c_str(), &hints, &result);
	if (rc != 0 || !result) {
		error = "解析 SRT 主机失败: " + m_target.host + " (" + gai_strerror(rc) + ")";
		Close();
		return false;
	}

	bool connected = false;
	std::string last_error;
	for (addrinfo *ai = result; ai; ai = ai->ai_next) {
		if (srt_connect(m_sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != SRT_ERROR) {
			connected = true;
			break;
		}
		last_error = srt_getlasterror_str();
	}
	freeaddrinfo(result);

	if (!connected) {
		error = "连接 SRT 入口失败 " + m_target.host + ":" + port + ": " + last_error;
		Close();
		return false;
	}
	return true;
}

bool SrtTransport::Write(const void *data, size_t len, std::string &error)
{
	if (m_sock == SRT_INVALID_SOCK) {
		error = "SRT 未连接";
		return false;
	}

	std::lock_guard<std::mutex> lock(m_write_mutex);
	const auto *p = static_cast<const uint8_t *>(data);
	size_t left = len;
	while (left > 0) {
		const int chunk = static_cast<int>(std::min<size_t>(left, kSendChunk));
		const int sent = srt_send(m_sock, reinterpret_cast<const char *>(p), chunk);
		if (sent <= 0) {
			error = std::string("SRT 发送失败: ") + srt_getlasterror_str();
			return false;
		}
		p += sent;
		left -= static_cast<size_t>(sent);
		m_bytes_sent.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
	}
	return true;
}

int SrtTransport::Read(void *data, size_t len, int /*timeout_ms*/, std::string &error)
{
	if (m_sock == SRT_INVALID_SOCK) {
		error = "SRT 未连接";
		return -1;
	}

	if (m_read_off >= m_read_len) {
		m_read_buf.resize(kRecvBuffer);
		const int got = srt_recv(m_sock, reinterpret_cast<char *>(m_read_buf.data()),
					 static_cast<int>(m_read_buf.size()));
		if (got <= 0) {
			if (got == 0) {
				error = "SRT 对端关闭连接";
				return -1;
			}
			int errno_loc = 0;
			const int code = srt_getlasterror(&errno_loc);
			if (IsRecoverableTimeout(code))
				return 0;
			error = std::string("SRT 接收失败: ") + srt_strerror(code, errno_loc);
			return -1;
		}
		m_read_off = 0;
		m_read_len = static_cast<size_t>(got);
	}

	const size_t take = std::min(len, m_read_len - m_read_off);
	std::memcpy(data, m_read_buf.data() + m_read_off, take);
	m_read_off += take;
	return static_cast<int>(take);
}

void SrtTransport::Close()
{
	if (m_sock != SRT_INVALID_SOCK) {
		srt_close(m_sock);
		m_sock = SRT_INVALID_SOCK;
	}
}

bool SrtTransport::UpdateMaxBandwidth(int bytes_per_sec, std::string &error)
{
	if (m_sock == SRT_INVALID_SOCK)
		return true; // 未连接时忽略，连接时用新值
	if (!SetInt64Opt(SRTO_MAXBW, bytes_per_sec, "SRTO_MAXBW", error))
		return false;
	m_target.max_bandwidth = bytes_per_sec;
	return true;
}

} // namespace Srt
