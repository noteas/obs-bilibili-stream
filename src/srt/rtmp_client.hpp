#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "srt/amf.hpp"
#include "srt/byte_stream.hpp"
#include "srt/flv_mux.hpp"
#include "srt/srt_target.hpp"

namespace Srt {

// 最小 RTMP 发布端（只做 publish，不做 play/鉴权/加密）。
// 协议行为 = 官方 bvc-srt-stream.dll，而它就是 OBS rtmp-stream + OBS vendored librtmp 的 fork
// （E-24），所以字段与时序一律对齐 OBS 32.2 的 librtmp（逐条出处见 E-25）：
//   简单握手 → Set Chunk Size(4096) → connect(app/type=nonprivate/flashVer/swfUrl=tcUrl/tcUrl)
//   → releaseStream → FCPublish → createStream → publish(playpath, "live")
//   → 媒体 FLV tag（chunk stream 4）→ 停播 FCUnpublish → deleteStream → 关 socket。
// 入站处理 Set Chunk Size / Ping / Window Ack；不发 Window Ack Size、Set Peer Bandwidth、主动 Ping
// （librtmp 里这三类只出现在入站分支，E-25 §4）。
class RtmpClient {
public:
	RtmpClient(ByteStream &stream, const Target &target);

	bool HandshakeAndPublish(std::string &error);
	bool SendTag(const FlvTag &tag, std::string &error);

	// 停播收尾：按 librtmp `RTMP_Close()` 的同序发 FCUnpublish + deleteStream（E-25 §5），
	// 不等响应、写失败也忽略；调用方随后关闭字节流。createStream 之前（无 stream id）不发。
	void SendTeardown();

	// 读一轮入站数据：1=处理了一条消息，0=暂时无数据，-1=连接错误。
	int PollIncoming(std::string &error);

	bool published() const { return m_publish_started.load(std::memory_order_relaxed); }
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
	// 读线程会回 Ping/Ack，写线程发媒体；整条消息必须串行化，否则 chunk 头会被交错。
	std::mutex m_send_mutex;

	uint32_t m_out_chunk_size = 4096;
	uint32_t m_in_chunk_size = 128;
	uint32_t m_stream_id = 0; // 0 = createStream 还没成功；非 0 = 服务端给的 stream id（0 按 1 处理）
	bool m_teardown_sent = false;
	double m_transaction = 0;

	uint32_t m_window_ack_size = 0;
	uint64_t m_bytes_received = 0;
	uint64_t m_last_ack = 0;

	std::atomic<bool> m_publish_started{false};
	std::string m_last_status;
	Amf::CommandInfo m_last_command;
	bool m_last_command_valid = false;

	std::map<uint32_t, ChunkState> m_chunks;
};

} // namespace Srt
