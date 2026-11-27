#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "srt/amf.hpp"
#include "srt/byte_stream.hpp"
#include "srt/flv_mux.hpp"
#include "srt/srt_target.hpp"

namespace Srt {

// 最小 RTMP 发布端（只做 publish，不做 play/鉴权/加密）。
// 协议行为按官方 bvc-srt-stream.dll = OBS rtmp-stream + librtmp fork（E-24）：
// 简单握手 → connect → releaseStream/FCPublish/createStream → publish，
// 之后把 FLV tag 作为 RTMP 消息（chunk stream 4）发送；入站消息处理
// Set Chunk Size / Ping / Window Ack。
class RtmpClient {
public:
	RtmpClient(ByteStream &stream, const Target &target);

	bool HandshakeAndPublish(std::string &error);
	bool SendTag(const FlvTag &tag, std::string &error);

	// 读一轮入站数据：1=处理了一条消息，0=暂时无数据，-1=连接错误。
	int PollIncoming(std::string &error);

	bool published() const { return m_publish_started; }
	uint32_t stream_id() const { return m_stream_id; }

private:
	struct ChunkState {
		uint32_t timestamp = 0;
		uint32_t delta = 0;
		uint32_t length = 0;
		uint8_t type = 0;
		uint32_t msid = 0;
		bool extended = false;
		std::vector<uint8_t> buffer;
		size_t received = 0;
	};

	// 1=读满，0=一个字节都没读到（超时），-1=错误/对端关闭。
	int ReadExact(void *data, size_t len, int timeout_ms, std::string &error);
	int ReadChunk(int timeout_ms, std::string &error);
	bool ReadChunkBody(ChunkState &st, std::string &error);
	bool HandleMessage(const ChunkState &st, std::string &error);
	bool SendMessage(uint8_t type, uint32_t csid, uint32_t msid, uint32_t timestamp, const uint8_t *body,
			 size_t len, std::string &error);
	bool WaitFor(const char *what, const std::function<bool(const Amf::CommandInfo &)> &pred, int timeout_ms,
		     std::string &error);
	void SendAcknowledgement();

	ByteStream &m_stream;
	Target m_target;

	uint32_t m_out_chunk_size = 4096;
	uint32_t m_in_chunk_size = 128;
	uint32_t m_stream_id = 1;
	double m_transaction = 0;

	uint32_t m_window_ack_size = 0;
	uint64_t m_bytes_received = 0;
	uint64_t m_last_ack = 0;

	bool m_publish_started = false;
	std::string m_last_status;
	Amf::CommandInfo m_last_command;
	bool m_last_command_valid = false;

	std::map<uint32_t, ChunkState> m_chunks;
};

} // namespace Srt
