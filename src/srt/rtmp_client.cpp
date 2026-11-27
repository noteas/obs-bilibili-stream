#include "srt/rtmp_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace Srt {

namespace {

constexpr uint8_t kMsgSetChunkSize = 1;
constexpr uint8_t kMsgAbort = 2;
constexpr uint8_t kMsgAck = 3;
constexpr uint8_t kMsgUserControl = 4;
constexpr uint8_t kMsgWindowAckSize = 5;
constexpr uint8_t kMsgSetPeerBandwidth = 6;
constexpr uint8_t kMsgCommandAmf0 = 20;

constexpr uint32_t kCmdChunkStream = 3;
constexpr uint32_t kMediaChunkStream = 4;
constexpr uint32_t kCtrlChunkStream = 2;

uint32_t ReadU24(const uint8_t *p)
{
	return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
}

uint32_t ReadU32BE(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t ReadU32LE(const uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void PutU24(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void PutU32BE(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void PutU32LE(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 24));
}

std::string StatusText(const Amf::CommandInfo &ci)
{
	if (!ci.code.empty() && !ci.level.empty())
		return ci.level + " " + ci.code;
	return ci.code.empty() ? std::string("无状态信息") : ci.code;
}

} // namespace

RtmpClient::RtmpClient(ByteStream &stream, const Target &target) : m_stream(stream), m_target(target) {}

int RtmpClient::ReadExact(void *data, size_t len, int timeout_ms, std::string &error)
{
	auto *p = static_cast<uint8_t *>(data);
	size_t got = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (got < len) {
		const int n = m_stream.Read(p + got, len - got, 200, error);
		if (n < 0)
			return -1;
		if (n > 0) {
			got += static_cast<size_t>(n);
			m_bytes_received += static_cast<uint64_t>(n);
			continue;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			if (got == 0) {
				error.clear();
				return 0;
			}
			error = "RTMP 读取超时（消息不完整）";
			return -1;
		}
	}
	return 1;
}

bool RtmpClient::SendMessage(uint8_t type, uint32_t csid, uint32_t msid, uint32_t timestamp, const uint8_t *body,
			     size_t len, std::string &error)
{
	const bool extended = timestamp >= 0xFFFFFF;
	std::vector<uint8_t> header;
	header.reserve(16);
	header.push_back(static_cast<uint8_t>(csid & 0x3F)); // fmt = 0（绝对时间戳，省去 delta 状态）
	PutU24(header, extended ? 0xFFFFFF : timestamp);
	PutU24(header, static_cast<uint32_t>(len));
	header.push_back(type);
	PutU32LE(header, msid);
	if (extended)
		PutU32BE(header, timestamp);

	if (!m_stream.Write(header.data(), header.size(), error))
		return false;

	size_t sent = 0;
	bool first = true;
	while (sent < len) {
		if (!first) {
			uint8_t cont = static_cast<uint8_t>(0xC0 | (csid & 0x3F)); // fmt3 续块
			if (!m_stream.Write(&cont, 1, error))
				return false;
			if (extended) {
				std::vector<uint8_t> ext;
				PutU32BE(ext, timestamp);
				if (!m_stream.Write(ext.data(), ext.size(), error))
					return false;
			}
		}
		const size_t chunk = std::min<size_t>(len - sent, m_out_chunk_size);
		if (chunk > 0 && !m_stream.Write(body + sent, chunk, error))
			return false;
		sent += chunk;
		first = false;
	}
	return true;
}

bool RtmpClient::ReadChunkBody(ChunkState &st, std::string &error)
{
	const size_t remain = st.length - st.received;
	const size_t take = std::min<size_t>(remain, m_in_chunk_size);
	if (take > 0) {
		const int rc = ReadExact(st.buffer.data() + st.received, take, 5000, error);
		if (rc != 1)
			return false;
		st.received += take;
	}
	return true;
}

int RtmpClient::ReadChunk(int timeout_ms, std::string &error)
{
	uint8_t basic = 0;
	const int first_rc = ReadExact(&basic, 1, timeout_ms, error);
	if (first_rc <= 0)
		return first_rc;

	const int fmt = basic >> 6;
	uint32_t csid = basic & 0x3F;
	uint8_t extra[2];
	if (csid == 0) {
		if (ReadExact(extra, 1, 5000, error) != 1)
			return -1;
		csid = 64u + extra[0];
	} else if (csid == 1) {
		if (ReadExact(extra, 2, 5000, error) != 1)
			return -1;
		csid = 64u + uint32_t(extra[0]) + uint32_t(extra[1]) * 256u;
	}

	ChunkState &st = m_chunks[csid];

	if (fmt == 0) {
		uint8_t h[11];
		if (ReadExact(h, sizeof(h), 5000, error) != 1)
			return -1;
		uint32_t ts = ReadU24(h);
		st.length = ReadU24(h + 3);
		st.type = h[6];
		st.msid = ReadU32LE(h + 7);
		st.extended = ts == 0xFFFFFF;
		if (st.extended) {
			uint8_t e[4];
			if (ReadExact(e, sizeof(e), 5000, error) != 1)
				return -1;
			ts = ReadU32BE(e);
		}
		st.timestamp = ts;
		st.delta = 0;
		st.buffer.assign(st.length, 0);
		st.received = 0;
	} else if (fmt == 1) {
		uint8_t h[7];
		if (ReadExact(h, sizeof(h), 5000, error) != 1)
			return -1;
		uint32_t delta = ReadU24(h);
		st.length = ReadU24(h + 3);
		st.type = h[6];
		st.extended = delta == 0xFFFFFF;
		if (st.extended) {
			uint8_t e[4];
			if (ReadExact(e, sizeof(e), 5000, error) != 1)
				return -1;
			delta = ReadU32BE(e);
		}
		st.delta = delta;
		st.timestamp += delta;
		st.buffer.assign(st.length, 0);
		st.received = 0;
	} else if (fmt == 2) {
		uint8_t h[3];
		if (ReadExact(h, sizeof(h), 5000, error) != 1)
			return -1;
		uint32_t delta = ReadU24(h);
		st.extended = delta == 0xFFFFFF;
		if (st.extended) {
			uint8_t e[4];
			if (ReadExact(e, sizeof(e), 5000, error) != 1)
				return -1;
			delta = ReadU32BE(e);
		}
		st.delta = delta;
		st.timestamp += delta;
		st.buffer.assign(st.length, 0);
		st.received = 0;
	} else {
		// fmt3：当前消息的续块；若当前无消息，则沿用上一头开始新消息。
		if (st.received == 0) {
			st.timestamp += st.delta;
			st.buffer.assign(st.length, 0);
			if (st.extended) {
				uint8_t e[4];
				if (ReadExact(e, sizeof(e), 5000, error) != 1)
					return -1;
			}
		}
	}

	if (!ReadChunkBody(st, error))
		return -1;

	if (st.received >= st.length) {
		if (!HandleMessage(st, error))
			return -1;
		SendAcknowledgement();
		st.received = 0;
	}
	return 1;
}

bool RtmpClient::HandleMessage(const ChunkState &st, std::string &error)
{
	(void)error;
	m_last_command_valid = false;

	switch (st.type) {
	case kMsgSetChunkSize:
		if (st.buffer.size() >= 4)
			m_in_chunk_size = ReadU32BE(st.buffer.data());
		return true;
	case kMsgAbort:
	case kMsgAck:
		return true;
	case kMsgUserControl: {
		if (st.buffer.size() >= 2) {
			const uint16_t event = static_cast<uint16_t>((st.buffer[0] << 8) | st.buffer[1]);
			if (event == 6) { // PingRequest → PingResponse
				std::vector<uint8_t> pong = {0x00, 0x07};
				pong.insert(pong.end(), st.buffer.begin() + std::min<size_t>(2, st.buffer.size()),
					    st.buffer.end());
				std::string werr;
				SendMessage(kMsgUserControl, kCtrlChunkStream, 0, 0, pong.data(), pong.size(), werr);
			}
		}
		return true;
	}
	case kMsgWindowAckSize:
		if (st.buffer.size() >= 4)
			m_window_ack_size = ReadU32BE(st.buffer.data());
		return true;
	case kMsgSetPeerBandwidth:
		return true;
	case kMsgCommandAmf0: {
		Amf::CommandInfo info;
		if (Amf::DecodeCommand(st.buffer.data(), st.buffer.size(), info)) {
			m_last_command = info;
			m_last_command_valid = true;
			if (info.name == "onStatus") {
				m_last_status = StatusText(info);
				if (info.code == "NetStream.Publish.Start")
					m_publish_started = true;
			}
		}
		return true;
	}
	default:
		return true;
	}
}

void RtmpClient::SendAcknowledgement()
{
	if (m_window_ack_size == 0)
		return;
	if (m_bytes_received - m_last_ack < m_window_ack_size)
		return;

	uint8_t body[4];
	body[0] = static_cast<uint8_t>(m_bytes_received >> 24);
	body[1] = static_cast<uint8_t>(m_bytes_received >> 16);
	body[2] = static_cast<uint8_t>(m_bytes_received >> 8);
	body[3] = static_cast<uint8_t>(m_bytes_received);
	std::string err;
	SendMessage(kMsgAck, kCtrlChunkStream, 0, 0, body, sizeof(body), err);
	m_last_ack = m_bytes_received;
}

bool RtmpClient::WaitFor(const char *what, const std::function<bool(const Amf::CommandInfo &)> &pred,
			 int timeout_ms, std::string &error)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		const int rc = ReadChunk(200, error);
		if (rc < 0)
			return false;
		if (rc == 0)
			continue;
		if (m_last_command_valid && pred(m_last_command))
			return true;
	}
	error = std::string("等待 ") + what + " 超时";
	return false;
}

bool RtmpClient::HandshakeAndPublish(std::string &error)
{
	// --- 简单握手（官方 librtmp 同款：C0=3 + 1536B C1，回 C2=S1）---
	std::vector<uint8_t> c1(1537);
	c1[0] = 0x03;
	std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
	for (size_t i = 9; i < c1.size(); ++i)
		c1[i] = static_cast<uint8_t>(rng());
	if (!m_stream.Write(c1.data(), c1.size(), error))
		return false;

	std::vector<uint8_t> s0s1(1537);
	{
		size_t got = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (got < s0s1.size()) {
			const int rc = ReadExact(s0s1.data() + got, s0s1.size() - got, 1000, error);
			if (rc < 0)
				return false;
			got += rc == 1 ? s0s1.size() - got : 0;
			if (rc == 0 && std::chrono::steady_clock::now() >= deadline) {
				error = "RTMP 握手读取 S0/S1 超时";
				return false;
			}
		}
	}
	if (s0s1[0] != 0x03) {
		error = "RTMP 握手失败：服务端版本号不是 3";
		return false;
	}
	if (!m_stream.Write(s0s1.data() + 1, 1536, error))
		return false;

	std::vector<uint8_t> s2(1536);
	{
		size_t got = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (got < s2.size()) {
			const int rc = ReadExact(s2.data() + got, s2.size() - got, 1000, error);
			if (rc < 0)
				return false;
			got += rc == 1 ? s2.size() - got : 0;
			if (rc == 0 && std::chrono::steady_clock::now() >= deadline) {
				error = "RTMP 握手读取 S2 超时";
				return false;
			}
		}
	}

	// --- Set Chunk Size (4096)，与 librtmp 一致 ---
	{
		uint8_t body[4];
		body[0] = static_cast<uint8_t>(m_out_chunk_size >> 24);
		body[1] = static_cast<uint8_t>(m_out_chunk_size >> 16);
		body[2] = static_cast<uint8_t>(m_out_chunk_size >> 8);
		body[3] = static_cast<uint8_t>(m_out_chunk_size);
		if (!SendMessage(kMsgSetChunkSize, kCtrlChunkStream, 0, 0, body, sizeof(body), error))
			return false;
	}

	// --- connect ---
	{
		std::vector<uint8_t> b;
		Amf::WriteString(b, "connect");
		m_transaction = 1;
		Amf::WriteNumber(b, m_transaction);
		Amf::BeginObject(b);
		Amf::WriteKey(b, "app");
		Amf::WriteString(b, m_target.app);
		Amf::WriteKey(b, "type");
		Amf::WriteString(b, "nonprivate");
		Amf::WriteKey(b, "flashVer");
		Amf::WriteString(b, "FMLE/3.0 (compatible; FMSc/1.0)");
		Amf::WriteKey(b, "swfUrl");
		Amf::WriteString(b, m_target.tc_url);
		Amf::WriteKey(b, "tcUrl");
		Amf::WriteString(b, m_target.tc_url);
		Amf::EndObject(b);
		if (!SendMessage(kMsgCommandAmf0, kCmdChunkStream, 0, 0, b.data(), b.size(), error))
			return false;
	}

	const double connect_txn = m_transaction;
	if (!WaitFor("connect 响应",
		     [connect_txn](const Amf::CommandInfo &ci) {
			     return ci.name == "_error" ||
				    (ci.name == "_result" && ci.transaction == connect_txn);
		     },
		     15000, error))
		return false;
	if (m_last_command.name == "_error") {
		error = "RTMP connect 被拒绝: " + StatusText(m_last_command);
		return false;
	}

	// --- releaseStream / FCPublish / createStream ---
	const auto send_simple = [&](const char *name) -> bool {
		std::vector<uint8_t> b;
		Amf::WriteString(b, name);
		Amf::WriteNumber(b, ++m_transaction);
		Amf::WriteNull(b);
		Amf::WriteString(b, m_target.stream);
		return SendMessage(kMsgCommandAmf0, kCmdChunkStream, 0, 0, b.data(), b.size(), error);
	};
	if (!send_simple("releaseStream"))
		return false;
	if (!send_simple("FCPublish"))
		return false;

	{
		std::vector<uint8_t> b;
		Amf::WriteString(b, "createStream");
		Amf::WriteNumber(b, ++m_transaction);
		Amf::WriteNull(b);
		if (!SendMessage(kMsgCommandAmf0, kCmdChunkStream, 0, 0, b.data(), b.size(), error))
			return false;
	}
	const double create_txn = m_transaction;
	if (!WaitFor("createStream 响应",
		     [create_txn](const Amf::CommandInfo &ci) {
			     return ci.name == "_error" ||
				    (ci.name == "_result" && ci.transaction == create_txn && ci.has_last_number);
		     },
		     15000, error))
		return false;
	if (m_last_command.name == "_error") {
		error = "RTMP createStream 被拒绝: " + StatusText(m_last_command);
		return false;
	}
	m_stream_id = static_cast<uint32_t>(m_last_command.last_number);
	if (m_stream_id == 0)
		m_stream_id = 1;

	// --- publish ---
	{
		std::vector<uint8_t> b;
		Amf::WriteString(b, "publish");
		Amf::WriteNumber(b, ++m_transaction);
		Amf::WriteNull(b);
		Amf::WriteString(b, m_target.stream);
		Amf::WriteString(b, "live");
		if (!SendMessage(kMsgCommandAmf0, kMediaChunkStream, m_stream_id, 0, b.data(), b.size(), error))
			return false;
	}

	if (!WaitFor("publish 确认",
		     [](const Amf::CommandInfo &ci) {
			     return ci.name == "_error" ||
				    (ci.name == "onStatus" &&
				     (ci.code == "NetStream.Publish.Start" || ci.level == "error" ||
				      ci.code.find("Failed") != std::string::npos ||
				      ci.code.find("BadName") != std::string::npos ||
				      ci.code.find("Denied") != std::string::npos));
		     },
		     15000, error))
		return false;

	if (!m_publish_started) {
		error = "RTMP publish 失败: " +
			(m_last_command.name == "_error" ? StatusText(m_last_command) : m_last_status);
		return false;
	}
	return true;
}

bool RtmpClient::SendTag(const FlvTag &tag, std::string &error)
{
	if (!m_publish_started) {
		error = "RTMP 尚未进入 publish 状态";
		return false;
	}
	return SendMessage(tag.type, kMediaChunkStream, m_stream_id, tag.ts_ms, tag.body.data(), tag.body.size(),
			   error);
}

int RtmpClient::PollIncoming(std::string &error)
{
	const int rc = ReadChunk(300, error);
	if (rc <= 0)
		return rc;
	if (m_last_command_valid && m_last_command.name == "onStatus" && m_last_command.level == "error") {
		error = "服务端状态错误: " + m_last_status;
		return -1;
	}
	return 1;
}

} // namespace Srt
