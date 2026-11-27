// SRT 输出核心的离线回归测试（CTest: srt_core_test）：
// URL 解析、AMF、FLV tag（legacy + Enhanced）、HEVC hvcC / AV1 av1C 构造与 OBU 转换
// （E-26 §3：逐字节口径 = 上游 OBS 32.2，样本来自本机 ffmpeg libx265/libsvtav1 编码 +
// mp4 盒真值比对）、RTMP 字节级口径（自建假服务端逐条核对 E-25）、
// RTMP-over-TCP 与 RTMP-over-SRT（需 ffmpeg，缺则跳过）、SRT 传输。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>

#include <srt/srt.h>

#include "srt/amf.hpp"
#include "srt/av1_parse.hpp"
#include "srt/byte_io.hpp"
#include "srt/flv_mux.hpp"
#include "srt/hevc_parse.hpp"
#include "srt/rtmp_client.hpp"
#include "srt/srt_target.hpp"
#include "srt/srt_transport.hpp"

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

// ffmpeg 互操作用例的公共前置：未装 ffmpeg 就跳过（字节级验收不依赖它，
// 由 test_rtmp_wire 的自建假服务端覆盖）。
static bool HaveFfmpeg()
{
	return std::system("command -v ffmpeg >/dev/null 2>&1") == 0;
}

static std::string TempDir()
{
	static const std::string dir = [] {
		std::error_code ec;
		const auto p = std::filesystem::temp_directory_path(ec) / "bvc_srt_core_test";
		std::filesystem::create_directories(p, ec);
		return p.string();
	}();
	return dir;
}

static void RunQuiet(const std::string &cmd)
{
	const int rc = std::system(cmd.c_str());
	(void)rc; // 测试辅助命令（pkill/后台 ffmpeg），成败都不致命
}

class TcpStream : public ByteStream {
public:
	bool Connect(const char *host, int port, std::string &err)
	{
		m_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (m_fd < 0) {
			err = "socket";
			return false;
		}
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(port));
		inet_pton(AF_INET, host, &addr.sin_addr);
		if (connect(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
			err = std::string("connect: ") + strerror(errno);
			return false;
		}
		return true;
	}
	bool Write(const void *data, size_t len, std::string &err) override
	{
		const auto *p = static_cast<const char *>(data);
		while (len) {
			const ssize_t n = ::send(m_fd, p, len, MSG_NOSIGNAL);
			if (n <= 0) {
				err = "send";
				return false;
			}
			p += n;
			len -= static_cast<size_t>(n);
		}
		return true;
	}
	int Read(void *data, size_t len, int timeout_ms, std::string &) override
	{
		fd_set set;
		FD_ZERO(&set);
		FD_SET(m_fd, &set);
		timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
		const int rc = select(m_fd + 1, &set, nullptr, nullptr, &tv);
		if (rc == 0)
			return 0;
		if (rc < 0)
			return -1;
		const ssize_t n = ::recv(m_fd, data, len, 0);
		return n == 0 ? -1 : static_cast<int>(n);
	}
	void Close() override
	{
		if (m_fd >= 0) {
			::close(m_fd);
			m_fd = -1;
		}
	}

private:
	int m_fd = -1;
};

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

static void test_amf()
{
	std::vector<uint8_t> b;
	Amf::WriteString(b, "_result");
	Amf::WriteNumber(b, 4);
	Amf::WriteNull(b);
	Amf::WriteNumber(b, 1);
	Amf::CommandInfo ci;
	CHECK(Amf::DecodeCommand(b.data(), b.size(), ci), "AMF 解码 _result");
	CHECK(ci.name == "_result" && ci.transaction == 4 && ci.has_last_number && ci.last_number == 1,
	      "AMF 字段");

	std::vector<uint8_t> o;
	Amf::WriteString(o, "onStatus");
	Amf::WriteNumber(o, 0);
	Amf::WriteNull(o);
	Amf::BeginObject(o);
	Amf::WriteKey(o, "level");
	Amf::WriteString(o, "status");
	Amf::WriteKey(o, "code");
	Amf::WriteString(o, "NetStream.Publish.Start");
	Amf::EndObject(o);
	CHECK(Amf::DecodeCommand(o.data(), o.size(), ci), "AMF 解码 onStatus");
	CHECK(ci.code == "NetStream.Publish.Start" && ci.level == "status", "onStatus code/level");
}

static void test_rtmp_tcp()
{
	if (!HaveFfmpeg()) {
		std::printf("SKIP: 未装 ffmpeg，跳过 RTMP-over-TCP 互操作用例\n");
		return;
	}
	const std::string sink = TempDir() + "/rtmp_sink.flv";
	RunQuiet("pkill -f 'rtmp://127.0.0.1:19350' >/dev/null 2>&1");
	RunQuiet(("ffmpeg -hide_banner -loglevel error -listen 1 -i rtmp://127.0.0.1:19350/live/test "
		     "-c copy -f flv -y " +
		     sink + " >/dev/null 2>&1 &")
			    .c_str());
	std::this_thread::sleep_for(std::chrono::milliseconds(800));

	Target t;
	std::string e;
	ParseTarget("rtmpsrt://127.0.0.1:19350/live/", "?streamname=test&key=k", t, e);

	TcpStream stream;
	CHECK(stream.Connect("127.0.0.1", 19350, e), "TCP 连接 ffmpeg RTMP 监听");
	RtmpClient client(stream, t);
	const bool published = client.HandshakeAndPublish(e);
	if (!published)
		std::printf("  publish error: %s\n", e.c_str());
	CHECK(published, "RTMP 握手 + publish（对 ffmpeg）");

	if (published) {
		// 媒体路径：发一个 AVC 序列头（v1 不发 metadata，E-25 §6）
		FlvTag header;
		const uint8_t avcc[] = {0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1};
		CHECK(MakeVideoHeaderTag(avcc, sizeof(avcc), header), "构造 AVC 序列头");
		CHECK(client.SendTag(header, e), "发送 AVC 序列头 tag");
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
	stream.Close();
	RunQuiet("pkill -f 'rtmp://127.0.0.1:19350' >/dev/null 2>&1");
}

// ---------------------------------------------------------------------------
// 字节级验收：自建假 RTMP 服务端，逐条核对 E-25 的线上口径
// ---------------------------------------------------------------------------
namespace {

struct WireMsg {
	uint8_t type = 0;
	uint32_t msid = 0;
	std::vector<uint8_t> body;
};

bool rd_exact(int fd, uint8_t *p, size_t n)
{
	while (n > 0) {
		const ssize_t r = ::recv(fd, p, n, 0);
		if (r <= 0)
			return false;
		p += static_cast<size_t>(r);
		n -= static_cast<size_t>(r);
	}
	return true;
}

bool wr_all(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
		if (w <= 0)
			return false;
		p += static_cast<size_t>(w);
		n -= static_cast<size_t>(w);
	}
	return true;
}

// 读一条 fmt0 头的消息（我们的客户端只用 fmt0 起新消息 + fmt3 续块）
bool rd_msg(int fd, uint32_t chunk, WireMsg &m, uint8_t *out_fmt, uint32_t *out_csid)
{
	uint8_t basic = 0;
	if (!rd_exact(fd, &basic, 1))
		return false;
	const uint8_t fmt = basic >> 6;
	if (out_fmt)
		*out_fmt = fmt;
	if (out_csid)
		*out_csid = basic & 0x3F;
	if (fmt != 0)
		return false;
	uint8_t h[11];
	if (!rd_exact(fd, h, sizeof(h)))
		return false;
	const uint32_t len = ReadU24(h + 3);
	m.type = h[6];
	m.msid = ReadU32LE(h + 7);
	m.body.clear();
	while (m.body.size() < len) {
		const size_t take = std::min<size_t>(len - m.body.size(), chunk ? chunk : 128);
		const size_t off = m.body.size();
		m.body.resize(off + take);
		if (!rd_exact(fd, m.body.data() + off, take))
			return false;
		if (m.body.size() < len) {
			uint8_t cont = 0;
			if (!rd_exact(fd, &cont, 1))
				return false;
			if ((cont >> 6) != 3)
				return false;
		}
	}
	return true;
}

// 服务端 → 客户端：fmt0 + 4096 分块
void wr_msg(int fd, uint8_t type, uint32_t csid, uint32_t msid, const std::vector<uint8_t> &body,
	    uint32_t chunk = 4096)
{
	std::vector<uint8_t> h;
	h.push_back(static_cast<uint8_t>(csid & 0x3F));
	PutU24(h, 0);
	PutU24(h, static_cast<uint32_t>(body.size()));
	h.push_back(type);
	PutU32LE(h, msid);
	wr_all(fd, h.data(), h.size());
	size_t sent = 0;
	bool first = true;
	while (sent < body.size()) {
		if (!first) {
			const uint8_t cont = static_cast<uint8_t>(0xC0 | (csid & 0x3F));
			wr_all(fd, &cont, 1);
		}
		const size_t take = std::min<size_t>(body.size() - sent, chunk);
		wr_all(fd, body.data() + sent, take);
		sent += take;
		first = false;
	}
}

// AMF0 字符串（0x02 + u16 长度 + 字节）在缓冲里的位置
size_t amf_str_pos(const std::vector<uint8_t> &b, const std::string &s)
{
	std::vector<uint8_t> pat;
	Amf::WriteString(pat, s);
	if (pat.size() > b.size())
		return std::string::npos;
	for (size_t i = 0; i + pat.size() <= b.size(); ++i) {
		if (std::equal(pat.begin(), pat.end(), b.begin() + static_cast<long>(i)))
			return i;
	}
	return std::string::npos;
}

size_t amf_key_pos(const std::vector<uint8_t> &b, const std::string &k)
{
	std::vector<uint8_t> pat;
	Amf::WriteKey(pat, k);
	if (pat.size() > b.size())
		return std::string::npos;
	for (size_t i = 0; i + pat.size() <= b.size(); ++i) {
		if (std::equal(pat.begin(), pat.end(), b.begin() + static_cast<long>(i)))
			return i;
	}
	return std::string::npos;
}

} // namespace

// 假服务端逐条核对：Set Chunk Size → connect 字段集与顺序 → releaseStream/FCPublish/
// createStream → publish（名含前导 ?）→ 媒体 tag → FCUnpublish + deleteStream。
static void test_rtmp_wire()
{
	constexpr int kPort = 19360;
	const std::string url = "rtmpsrt://127.0.0.1:" + std::to_string(kPort) + "/live/test?streamname=wire&key=wk";
	const std::string key = "?streamname=wire&key=wk";
	const std::string tcUrl = url; // app 含查询串 ⇒ tcUrl = 整个 URL（E-25 §2）

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(listener >= 0, "wire: socket");
	int yes = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kPort);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	CHECK(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "wire: bind");
	CHECK(listen(listener, 1) == 0, "wire: listen");

	std::thread server([&] {
		const int fd = accept(listener, nullptr, nullptr);
		if (fd < 0)
			return;

		// 简单握手：C0+C1 → S0+S1 → C2（客户端还会再读 S2）
		std::vector<uint8_t> c1(1537);
		rd_exact(fd, c1.data(), c1.size());
		std::vector<uint8_t> s0s1(1537, 0x03);
		wr_all(fd, s0s1.data(), s0s1.size());
		std::vector<uint8_t> s2(1536, 0x04);
		wr_all(fd, s2.data(), s2.size());
		std::vector<uint8_t> c2(1536);
		rd_exact(fd, c2.data(), c2.size());

		uint32_t chunk = 128;
		WireMsg m;

		// 1) Set Chunk Size = 4096，且必须在 connect 之前（E-25 §1 表格首行）
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 1)
			goto done;
		CHECK(m.body.size() == 4 && ReadU32BE(m.body.data()) == 4096, "wire: 先发 Set Chunk Size 4096");
		chunk = 4096;

		// 2) connect
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto done;
		CHECK(amf_str_pos(m.body, "connect") == 0, "wire: 首条命令是 connect");
		{
			const size_t app = amf_key_pos(m.body, "app");
			const size_t type = amf_key_pos(m.body, "type");
			const size_t flash = amf_key_pos(m.body, "flashVer");
			const size_t swf = amf_key_pos(m.body, "swfUrl");
			const size_t tc = amf_key_pos(m.body, "tcUrl");
			CHECK(app != std::string::npos && type > app && flash > type && swf > flash && tc > swf,
			      "wire: connect 字段顺序 app/type/flashVer/swfUrl/tcUrl");
			CHECK(amf_str_pos(m.body, "live/test?streamname=wire&key=wk") != std::string::npos,
			      "wire: app 含查询串");
			CHECK(amf_str_pos(m.body, "nonprivate") != std::string::npos, "wire: type=nonprivate");
			CHECK(amf_str_pos(m.body, "FMLE/3.0 (compatible; FMSc/1.0)") != std::string::npos,
			      "wire: flashVer = OBS 同款");
			CHECK(amf_str_pos(m.body, tcUrl) != std::string::npos, "wire: tcUrl = 原 URL");
			// 发布方向不发这些（librtmp 只在播放方向发，E-25 §1）
			CHECK(amf_key_pos(m.body, "fpad") == std::string::npos, "wire: 无 fpad");
			CHECK(amf_key_pos(m.body, "capabilities") == std::string::npos, "wire: 无 capabilities");
			CHECK(amf_key_pos(m.body, "audioCodecs") == std::string::npos, "wire: 无 audioCodecs");
			CHECK(amf_key_pos(m.body, "videoCodecs") == std::string::npos, "wire: 无 videoCodecs");
			CHECK(amf_key_pos(m.body, "videoFunction") == std::string::npos, "wire: 无 videoFunction");
			CHECK(amf_key_pos(m.body, "pageUrl") == std::string::npos, "wire: 无 pageUrl");
			CHECK(amf_key_pos(m.body, "objectEncoding") == std::string::npos, "wire: 无 objectEncoding");
		}
		{
			std::vector<uint8_t> r;
			Amf::WriteString(r, "_result");
			Amf::WriteNumber(r, 1);
			Amf::BeginObject(r);
			Amf::EndObject(r);
			wr_msg(fd, 20, 3, 0, r);
		}

		// 3) releaseStream / FCPublish（都带 playpath）
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto done;
		CHECK(amf_str_pos(m.body, "releaseStream") == 0, "wire: 第二条 releaseStream");
		CHECK(amf_str_pos(m.body, key) != std::string::npos, "wire: releaseStream 带 publish 名");
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto done;
		CHECK(amf_str_pos(m.body, "FCPublish") == 0, "wire: 第三条 FCPublish");

		// 4) createStream → 回 stream id 1
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto done;
		CHECK(amf_str_pos(m.body, "createStream") == 0, "wire: 第四条 createStream");
		{
			Amf::CommandInfo ci;
			Amf::DecodeCommand(m.body.data(), m.body.size(), ci);
			std::vector<uint8_t> r;
			Amf::WriteString(r, "_result");
			Amf::WriteNumber(r, ci.transaction); // 必须回同一个事务号
			Amf::WriteNull(r);
			Amf::WriteNumber(r, 1);
			wr_msg(fd, 20, 3, 0, r);
			CHECK(ci.transaction == 4, "wire: 事务号 connect=1→releaseStream=2→FCPublish=3→createStream=4");
		}

		// 5) publish：名 = 推流码原文（含前导 '?'），type = "live"，msid = 1
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto done;
		CHECK(amf_str_pos(m.body, "publish") == 0, "wire: 第五条 publish");
		CHECK(m.msid == 1, "wire: publish 在 createStream 给的 stream id 上");
		CHECK(amf_str_pos(m.body, key) != std::string::npos, "wire: publish 名含前导 '?'（E-25 §3）");
		CHECK(amf_str_pos(m.body, "live") != std::string::npos, "wire: publish type=live");
		{
			std::vector<uint8_t> r;
			Amf::WriteString(r, "onStatus");
			Amf::WriteNumber(r, 0);
			Amf::WriteNull(r);
			Amf::BeginObject(r);
			Amf::WriteKey(r, "level");
			Amf::WriteString(r, "status");
			Amf::WriteKey(r, "code");
			Amf::WriteString(r, "NetStream.Publish.Start");
			Amf::EndObject(r);
			wr_msg(fd, 20, 3, 1, r);
		}

		// 6) 第一条媒体消息必须是序列头（type 9），不是 metadata（type 18）
		if (!rd_msg(fd, chunk, m, nullptr, nullptr))
			goto done;
		CHECK(m.type == 9, "wire: 首条媒体是视频序列头，不发 onMetaData（E-25 §6）");

		// 7) 收尾：FCUnpublish → deleteStream（E-25 §5）
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto wire_done;
		CHECK(amf_str_pos(m.body, "FCUnpublish") == 0, "wire: 收尾第一条 FCUnpublish");
		CHECK(amf_str_pos(m.body, key) != std::string::npos, "wire: FCUnpublish 带 publish 名");
		if (!rd_msg(fd, chunk, m, nullptr, nullptr) || m.type != 20)
			goto wire_done;
		CHECK(amf_str_pos(m.body, "deleteStream") == 0, "wire: 收尾第二条 deleteStream（E-25 §5）");
	wire_done:
		;
	done:
		::close(fd);
	});

	TcpStream stream;
	std::string e;
	CHECK(stream.Connect("127.0.0.1", kPort, e), "wire: 客户端连接");
	Target t;
	CHECK(ParseTarget(url, key, t, e), "wire: 解析地址");
	RtmpClient client(stream, t);
	if (!client.HandshakeAndPublish(e))
		std::printf("  wire publish error: %s\n", e.c_str());
	CHECK(client.published(), "wire: 走完 connect→publish");

	if (client.published()) {
		FlvTag header;
		const uint8_t avcc[] = {0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1};
		MakeVideoHeaderTag(avcc, sizeof(avcc), header);
		client.SendTag(header, e);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		client.SendTeardown();
	}
	stream.Close();
	server.join();
	::close(listener);
}

static void test_srt_transport()
{
	srt_startup();
	SRTSOCKET listener = srt_create_socket();
	int yes = 1;
	srt_setsockopt(listener, 0, SRTO_REUSEADDR, &yes, sizeof(yes));
	int trans = SRTT_LIVE;
	srt_setsockopt(listener, 0, SRTO_TRANSTYPE, &trans, sizeof(trans));
	int msgapi = 1;
	srt_setsockopt(listener, 0, SRTO_MESSAGEAPI, &msgapi, sizeof(msgapi));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(19351);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	CHECK(srt_bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != SRT_ERROR, "SRT listener bind");
	CHECK(srt_listen(listener, 1) != SRT_ERROR, "SRT listener listen");

	std::atomic<bool> accepted{false};
	std::atomic<int> streamid_len{-1};
	std::string received;
	std::thread server([&] {
		sockaddr_storage peer{};
		int peer_len = sizeof(peer);
		SRTSOCKET s = srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_len);
		if (s == SRT_INVALID_SOCK)
			return;
		accepted = true;
		char sid[512];
		int sid_len = sizeof(sid);
		if (srt_getsockopt(s, 0, SRTO_STREAMID, sid, &sid_len) != SRT_ERROR)
			streamid_len = sid_len;
		char buf[1500];
		int msgapi = -1, optlen = sizeof(msgapi);
		srt_getsockopt(s, 0, SRTO_MESSAGEAPI, &msgapi, &optlen);
		const int n = srt_recvmsg(s, buf, sizeof(buf));
		int e2 = 0;
		const int ec = n < 0 ? srt_getlasterror(&e2) : 0;
		std::printf("  listener recv n=%d msgapi=%d code=%d errno=%d str=%s\n", n, msgapi, ec, e2,
			    n < 0 ? srt_strerror(ec, e2) : "ok");
		if (n > 0)
			received.assign(buf, static_cast<size_t>(n));
		srt_close(s);
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	Target t;
	t.host = "127.0.0.1";
	t.port = 19351;
	SrtTransport transport(t);
	std::string e;
	if (!transport.Connect(e)) std::printf("  srt connect error: %s\n", e.c_str());
	CHECK(transport.connected(), "SRT 连接本地 listener");
	CHECK(transport.Write("hello-srt", 9, e), "SRT 发送字节");
	if (!e.empty()) std::printf("  write error: %s\n", e.c_str());
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	transport.Close();

	server.join();
	srt_close(listener);
	srt_cleanup();

	CHECK(accepted.load(), "listener 收到连接");
	CHECK(received == "hello-srt", "listener 收到 payload");
	CHECK(streamid_len.load() == 0, "streamid 为空（官方口径）");
}


static void test_rtmp_over_srt()
{
	// SRT→TCP 桥：把 SRT 上的字节流接到 ffmpeg 的 RTMP 监听，验证完整
	// “RTMP over SRT” 路径（客户端 → SRT → TCP → ffmpeg RTMP server）。
	if (!HaveFfmpeg()) {
		std::printf("SKIP: 未装 ffmpeg，跳过 RTMP-over-SRT 桥接用例\n");
		return;
	}
	const std::string sink = TempDir() + "/rtmp_srt_sink.flv";
	RunQuiet("pkill -f 'rtmp://127.0.0.1:19350' >/dev/null 2>&1");
	RunQuiet(("ffmpeg -hide_banner -loglevel error -listen 1 -i rtmp://127.0.0.1:19350/live/test "
		    "-c copy -f flv -y " + sink + " >/dev/null 2>&1 &").c_str());
	std::this_thread::sleep_for(std::chrono::milliseconds(800));

	srt_startup();
	SRTSOCKET listener = srt_create_socket();
	int yes = 1;
	srt_setsockopt(listener, 0, SRTO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(19352);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	CHECK(srt_bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != SRT_ERROR, "SRT 桥 bind");
	CHECK(srt_listen(listener, 1) != SRT_ERROR, "SRT 桥 listen");

	std::atomic<bool> ready{false};
	std::thread bridge([&] {
		sockaddr_storage peer{};
		int peer_len = sizeof(peer);
		SRTSOCKET s = srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_len);
		if (s == SRT_INVALID_SOCK)
			return;
		int tcp = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in taddr{};
		taddr.sin_family = AF_INET;
		taddr.sin_port = htons(19350);
		inet_pton(AF_INET, "127.0.0.1", &taddr.sin_addr);
		if (connect(tcp, reinterpret_cast<sockaddr *>(&taddr), sizeof(taddr)) != 0) {
			srt_close(s);
			return;
		}
		ready = true;
		std::thread up([&] {
			char buf[1500];
			for (;;) {
				const int n = srt_recv(s, buf, sizeof(buf));
				if (n <= 0)
					break;
				if (send(tcp, buf, static_cast<size_t>(n), MSG_NOSIGNAL) <= 0)
					break;
			}
			shutdown(tcp, SHUT_WR);
		});
		char buf[4096];
		for (;;) {
			ssize_t n = recv(tcp, buf, sizeof(buf), 0);
			if (n <= 0)
				break;
			for (ssize_t off = 0; off < n;) {
				const int part = static_cast<int>(std::min<ssize_t>(n - off, 1316));
				if (srt_send(s, buf + off, part) < 0)
					goto bridge_done;
				off += part;
			}
		}
	bridge_done:
		up.join();
		srt_close(s);
		::close(tcp);
	});

	Target t;
	std::string e;
	ParseTarget("rtmpsrt://127.0.0.1:19352/live/", "?streamname=test&key=k", t, e);
	SrtTransport transport(t);
	const bool connected = transport.Connect(e);
	if (!connected)
		std::printf("  srt connect error: %s\n", e.c_str());
	CHECK(connected, "SRT 连接本地桥");
	if (connected) {
		RtmpClient client(transport, t);
		const bool published = client.HandshakeAndPublish(e);
		if (!published)
			std::printf("  publish error: %s\n", e.c_str());
		CHECK(published, "RTMP over SRT 握手 + publish（经本地桥到 ffmpeg）");
		if (published) {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		}
		transport.Close();
	}

	RunQuiet("pkill -f 'rtmp://127.0.0.1:19350' >/dev/null 2>&1");
	srt_close(listener);
	bridge.join();
	srt_cleanup();
}


static void test_flv_tags()
{
	FlvTag t;
	const uint8_t asc[] = {0x12, 0x10};
	CHECK(MakeAudioHeaderTag(asc, 2, t) && t.type == 8 && t.body.size() == 4 && t.body[0] == 0xAF &&
		      t.body[1] == 0x00,
	      "AAC 序列头 tag");
	CHECK(MakeAudioFrameTag(asc, 2, 1234, t) && t.type == 8 && t.ts_ms == 1234 && t.body[0] == 0xAF &&
		      t.body[1] == 0x01,
	      "AAC 帧 tag");

	const uint8_t avcc[] = {0, 0, 0, 2, 0x65, 0x88};
	CHECK(MakeVideoFrameTag(avcc, 6, true, 567, 40, t) && t.type == 9 && t.ts_ms == 567 &&
		      t.body[0] == 0x17 && t.body[1] == 0x01 && t.body[2] == 0 && t.body[3] == 0 &&
		      t.body[4] == 40 && t.body.size() == 11,
	      "H.264 关键帧 tag（legacy 0x17 + 正 ct）");
	CHECK(MakeVideoFrameTag(avcc, 6, false, 0, -40, t) && t.body[0] == 0x27 && t.body[2] == 0xFF &&
		      t.body[3] == 0xFF && t.body[4] == 0xD8,
	      "H.264 非关键帧 tag（0x27 + 负 ct 补码）");
	FlvTag h;
	CHECK(MakeVideoHeaderTag(avcc, 6, h) && h.type == 9 && h.body[0] == 0x17 && h.body[1] == 0x00 &&
		      h.body.size() == 11,
	      "AVCDecoderConfigurationRecord tag");
}

// ── Enhanced RTMP tag 构造（E-26 §3：字节口径 = 上游 OBS 32.2 flv_packet_ex 族）──
static void test_flv_enhanced_tags()
{
	const uint8_t pl[] = {0xAA, 0xBB};
	FlvTag t;

	CHECK(MakeVideoHeaderTagEx(pl, 2, EnhancedVideoCodec::Hevc, t) && t.type == 9 && t.ts_ms == 0 &&
		      t.body.size() == 7 && t.body[0] == 0x90 &&
		      memcmp(&t.body[1], "hvc1", 4) == 0,
	      "HEVC SEQ_START tag（0x90 + hvc1）");
	CHECK(MakeVideoHeaderTagEx(pl, 2, EnhancedVideoCodec::Av1, t) && t.body[0] == 0x90 &&
		      memcmp(&t.body[1], "av01", 4) == 0,
	      "AV1 SEQ_START tag（0x90 + av01）");

	CHECK(MakeVideoFrameTagEx(pl, 2, true, 1000, 33, false, EnhancedVideoCodec::Hevc, t) &&
		      t.ts_ms == 1000 && t.body[0] == 0x91 && t.body[5] == 0x00 && t.body[6] == 0x00 &&
		      t.body[7] == 0x21 && t.body.size() == 10,
	      "HEVC FRAMES 关键帧（0x91 + CT24=33）");
	CHECK(MakeVideoFrameTagEx(pl, 2, false, 0, -17, false, EnhancedVideoCodec::Hevc, t) &&
		      t.body[0] == 0xA1 && t.body[5] == 0xFF && t.body[6] == 0xFF && t.body[7] == 0xEF,
	      "HEVC FRAMES 帧间（0xA1 + 负 CT 补码）");
	CHECK(MakeVideoFrameTagEx(pl, 2, true, 0, 0, true, EnhancedVideoCodec::Hevc, t) &&
		      t.body[0] == 0x93 && t.body.size() == 7,
	      "HEVC dts==pts 用 FRAMESX（0x93 省 CT）");
	CHECK(MakeVideoFrameTagEx(pl, 2, true, 0, 0, true, EnhancedVideoCodec::Av1, t) &&
		      t.body[0] == 0x91 && t.body.size() == 7,
	      "AV1 恒 FRAMES 无 CT（上游同款）");

	// 上游 send_footers：dts=0 → ts = -start_dts_offset，int32 环绕。
	CHECK(MakeVideoSeqEndTag(-5000, EnhancedVideoCodec::Av1, t) && t.type == 9 &&
		      t.ts_ms == static_cast<uint32_t>(static_cast<int32_t>(-5000)) && t.body.size() == 5 &&
		      t.body[0] == 0xA2 && memcmp(&t.body[1], "av01", 4) == 0,
	      "SEQ_END tag（0xA2 + 负 ts 环绕）");

	// HDR colorInfo：完整 body 与上游 AMF0 口径逐字节比对（含成员名不带 STRING marker 的 quirk）。
	static const uint8_t kColorInfoBodyHvc1[] = {
		0x84,0x68,0x76,0x63,0x31,0x02,0x00,0x09,0x63,0x6f,0x6c,0x6f,
		0x72,0x49,0x6e,0x66,0x6f,0x03,0x00,0x0b,0x63,0x6f,0x6c,0x6f,
		0x72,0x43,0x6f,0x6e,0x66,0x69,0x67,0x03,0x00,0x08,0x62,0x69,
		0x74,0x44,0x65,0x70,0x74,0x68,0x00,0x40,0x24,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x0e,0x63,0x6f,0x6c,0x6f,0x72,0x50,0x72,
		0x69,0x6d,0x61,0x72,0x69,0x65,0x73,0x00,0x3f,0xf0,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x17,0x74,0x72,0x61,0x6e,0x73,0x66,
		0x65,0x72,0x43,0x68,0x61,0x72,0x61,0x63,0x74,0x65,0x72,0x69,
		0x73,0x74,0x69,0x63,0x73,0x00,0x40,0x30,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x12,0x6d,0x61,0x74,0x72,0x69,0x78,0x43,0x6f,
		0x65,0x66,0x66,0x69,0x63,0x69,0x65,0x6e,0x74,0x73,0x00,0x40,
		0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x00,0x07,
		0x68,0x64,0x72,0x4d,0x64,0x63,0x76,0x03,0x00,0x0c,0x6d,0x61,
		0x78,0x4c,0x75,0x6d,0x69,0x6e,0x61,0x6e,0x63,0x65,0x00,0x40,
		0x8f,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0x6d,0x69,0x6e,
		0x4c,0x75,0x6d,0x69,0x6e,0x61,0x6e,0x63,0x65,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x00,0x00,0x09,
	};
	CHECK(MakeColorInfoTag(EnhancedVideoCodec::Hevc, 10, 1, 16, 9, 0, 1000, t) && t.type == 9 &&
		      t.ts_ms == 0 && t.body.size() == sizeof(kColorInfoBodyHvc1) &&
		      memcmp(t.body.data(), kColorInfoBodyHvc1, t.body.size()) == 0,
	      "colorInfo metadata tag（0x84 + AMF0 逐字节）");
	// maxLum=0 → 上游不写 hdrMdcv 子对象。
	CHECK(MakeColorInfoTag(EnhancedVideoCodec::Av1, 8, 1, 1, 1, 0, 0, t), "colorInfo tag 构造");
	const uint8_t needle[] = {'h', 'd', 'r', 'M', 'd', 'c', 'v'};
	auto it = std::search(t.body.begin(), t.body.end(), std::begin(needle), std::end(needle));
	CHECK(t.body[0] == 0x84 && memcmp(&t.body[1], "av01", 4) == 0 &&
		      memcmp(t.body.data() + 5, kColorInfoBodyHvc1 + 5, 13) == 0 &&
		      it == t.body.end(),
	      "colorInfo maxLum=0 不写 hdrMdcv");
}

// ── HEVC hvcC 构造（真值：本机 ffmpeg libx265 编码 → mp4 hvcC 盒比对，
//    仅 3 处 array_completeness 位不同——上游 OBS 恒传 1，ffmpeg muxer 传 0）──
static void test_hevc_parse()
{
	static const uint8_t kHevcExtra[] = {
		0x00,0x00,0x00,0x01,0x40,0x01,0x0c,0x01,0xff,0xff,0x04,0x08,
		0x00,0x00,0x03,0x00,0x9f,0xa8,0x00,0x00,0x03,0x00,0x00,0x3c,
		0xba,0x02,0x40,0x00,0x00,0x00,0x01,0x42,0x01,0x01,0x04,0x08,
		0x00,0x00,0x03,0x00,0x9f,0xa8,0x00,0x00,0x03,0x00,0x00,0x3c,
		0xa0,0x0a,0x08,0x0f,0x16,0x5b,0xa4,0xa4,0xc2,0xf0,0x16,0x80,
		0x80,0x00,0x00,0x03,0x00,0x80,0x00,0x00,0x0f,0x04,0x00,0x00,
		0x00,0x01,0x44,0x01,0xc0,0x71,0x83,0x12,
	};
	static const uint8_t kHevcHvcCUpstream[] = {
		0x01,0x04,0x08,0x00,0x00,0x00,0x9f,0xa8,0x00,0x00,0x00,0x00,
		0x3c,0xf0,0x00,0xfc,0xfd,0xf8,0xf8,0x00,0x00,0x0f,0x03,0xa0,
		0x00,0x01,0x00,0x17,0x40,0x01,0x0c,0x01,0xff,0xff,0x04,0x08,
		0x00,0x00,0x03,0x00,0x9f,0xa8,0x00,0x00,0x03,0x00,0x00,0x3c,
		0xba,0x02,0x40,0xa1,0x00,0x01,0x00,0x27,0x42,0x01,0x01,0x04,
		0x08,0x00,0x00,0x03,0x00,0x9f,0xa8,0x00,0x00,0x03,0x00,0x00,
		0x3c,0xa0,0x0a,0x08,0x0f,0x16,0x5b,0xa4,0xa4,0xc2,0xf0,0x16,
		0x80,0x80,0x00,0x00,0x03,0x00,0x80,0x00,0x00,0x0f,0x04,0xa2,
		0x00,0x01,0x00,0x06,0x44,0x01,0xc0,0x71,0x83,0x12,
	};
	std::vector<uint8_t> out;
	CHECK(BuildHevcHvcC(kHevcExtra, sizeof(kHevcExtra), out) &&
		      out.size() == sizeof(kHevcHvcCUpstream) &&
		      memcmp(out.data(), kHevcHvcCUpstream, out.size()) == 0,
	      "annexb → hvcC 逐字节（libx265 样本，PTL/VUI/PPS 全字段）");

	// 已是 hvcC（首字节 1）→ 原样透传（上游同款）。
	CHECK(BuildHevcHvcC(kHevcHvcCUpstream, sizeof(kHevcHvcCUpstream), out) &&
		      out.size() == sizeof(kHevcHvcCUpstream),
	      "已 hvcC 透传");

	// 无起始码 → 原样透传（上游同款）。
	static const uint8_t raw[] = {0x11, 0x22, 0x33};
	CHECK(BuildHevcHvcC(raw, sizeof(raw), out) && out.size() == 3 && out[0] == 0x11, "无起始码透传");

	// 有起始码但没有 VPS/SPS/PPS → 失败（false + 空 out；上游此处会返回垃圾缓冲）。
	static const uint8_t idr_only[] = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x11, 0x22, 0x33, 0x44};
	out.clear();
	CHECK(!BuildHevcHvcC(idr_only, sizeof(idr_only), out) && out.empty(), "缺参数集 → false");
}

// ── AV1 av1C 构造 + temporal unit 转换（真值：libsvtav1 编码 → mp4 av1C 盒；
//    OBU 流用合成样本覆盖各类型处置）──
static void test_av1_parse()
{
	static const uint8_t kAv1SeqObu[] = {
		0x0a,0x0b,0x00,0x00,0x00,0x04,0x3c,0xff,0xbc,0x4a,0xf9,0x00,
		0x40,
	};
	static const uint8_t kAv1CfgExpected[] = {
		0x81,0x00,0x0c,0x00,0x0a,0x0b,0x00,0x00,0x00,0x04,0x3c,0xff,
		0xbc,0x4a,0xf9,0x00,0x40,
	};
	std::vector<uint8_t> out;
	CHECK(BuildAv1CodecConfRecord(kAv1SeqObu, sizeof(kAv1SeqObu), out) &&
		      out.size() == sizeof(kAv1CfgExpected) &&
		      memcmp(out.data(), kAv1CfgExpected, out.size()) == 0,
	      "seq OBU → av1C+OBU 逐字节（libsvtav1 样本）");
	CHECK(BuildAv1CodecConfRecord(kAv1CfgExpected, sizeof(kAv1CfgExpected), out) &&
		      out.size() == sizeof(kAv1CfgExpected),
	      "已 av1C（marker|version=1）透传");
	CHECK(kAv1CfgExpected[0] == 0x81, "av1C marker 透传前提成立");
	static const uint8_t bad_ver[] = {0x82, 0x00, 0x0c, 0x00};
	CHECK(!BuildAv1CodecConfRecord(bad_ver, sizeof(bad_ver), out), "version!=1 → false");
	static const uint8_t td_only[] = {0x12, 0x00};
	CHECK(!BuildAv1CodecConfRecord(td_only, sizeof(td_only), out), "无序列头 → false");
	// 两个序列头 OBU → 上游拒绝（nb_seq>1）。
	std::vector<uint8_t> two(kAv1SeqObu, kAv1SeqObu + sizeof(kAv1SeqObu));
	two.insert(two.end(), std::begin(kAv1SeqObu), std::end(kAv1SeqObu));
	CHECK(!BuildAv1CodecConfRecord(two.data(), two.size(), out), "双序列头 → false");

	// OBU 流处置：TD/冗余帧头/Tile List 剔除，其余原样拼接；按帧头重算 keyframe/priority。
	std::vector<uint8_t> tu = {0x12, 0x00}; // Temporal Delimiter（剔）
	tu.insert(tu.end(), std::begin(kAv1SeqObu), std::end(kAv1SeqObu));
	tu.insert(tu.end(), {0x32, 0x03, 0x08, 0x11, 0x11}); // FRAME OBU，payload 首字节:
	                                                     // show_existing=0, frame_type=KEY
	tu.insert(tu.end(), {0x3a, 0x01, 0x20}); // REDUNDANT_FRAME_HEADER（剔）
	tu.insert(tu.end(), {0x1a, 0x01, 0x30}); // FRAME_HEADER，frame_type=INTER+show（保留，
	                                         // priority 已定 3 → 不覆盖）
	tu.insert(tu.end(), {0x42, 0x01, 0x40}); // TILE_LIST（剔）
	tu.insert(tu.end(), {0x7a, 0x01, 0xff}); // PADDING（保留，上游 default 分支）
	bool kf = false;
	int pri = 0;
	ConvertAv1TemporalUnit(tu.data(), tu.size(), &kf, &pri, out);
	std::vector<uint8_t> want;
	want.insert(want.end(), std::begin(kAv1SeqObu), std::end(kAv1SeqObu));
	want.insert(want.end(), {0x32, 0x03, 0x08, 0x11, 0x11});
	want.insert(want.end(), {0x1a, 0x01, 0x30});
	want.insert(want.end(), {0x7a, 0x01, 0xff});
	CHECK(kf && pri == 3 && out == want, "TD/冗余/TileList 剔除 + KEY 重算 kf=1 pri=3");

	// 编码器已给 priority（QSV/AMF 口径）→ 不覆盖。
	kf = true;
	pri = 2;
	std::vector<uint8_t> inter_only = {0x1a, 0x01, 0x30};
	ConvertAv1TemporalUnit(inter_only.data(), inter_only.size(), &kf, &pri, out);
	CHECK(kf && pri == 2 && out == inter_only, "priority 非零不覆盖");
}

int main()
{
	test_target();
	test_amf();
	test_flv_tags();
	test_flv_enhanced_tags();
	test_hevc_parse();
	test_av1_parse();
	test_rtmp_wire();
	test_rtmp_tcp();
	test_srt_transport();
	test_rtmp_over_srt();
	std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fail);
	return g_fail ? 1 : 0;
}
