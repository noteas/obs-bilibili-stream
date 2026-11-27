#include "srt/flv_mux.hpp"

#include <algorithm>

#include "srt/amf.hpp"
#include "srt/byte_io.hpp"

namespace Srt {

namespace {

constexpr uint8_t kAudioTag = 8;
constexpr uint8_t kVideoTag = 9;

uint32_t ClampTs(int64_t ms)
{
	if (ms < 0)
		return 0;
	if (ms > 0xFFFFFFFFLL)
		return 0xFFFFFFFFu;
	return static_cast<uint32_t>(ms);
}

void PutI24(std::vector<uint8_t> &out, int32_t v)
{
	v = std::max<int32_t>(-0x800000, std::min<int32_t>(0x7FFFFF, v));
	PutU24(out, static_cast<uint32_t>(v) & 0xFFFFFF);
}

} // namespace

bool MakeVideoHeaderTag(const uint8_t *avcc, size_t size, FlvTag &tag)
{
	if (!avcc || size == 0)
		return false;
	tag.type = kVideoTag;
	tag.ts_ms = 0;
	tag.body.clear();
	tag.body.reserve(size + 5);
	tag.body.push_back(0x17); // keyframe + AVC
	tag.body.push_back(0x00); // AVCPacketType = sequence header
	tag.body.push_back(0x00);
	tag.body.push_back(0x00);
	tag.body.push_back(0x00);
	tag.body.insert(tag.body.end(), avcc, avcc + size);
	return true;
}

bool MakeVideoFrameTag(const uint8_t *nalu, size_t size, bool keyframe, int64_t ts_ms, int32_t ct_ms, FlvTag &tag)
{
	if (!nalu || size == 0)
		return false;
	tag.type = kVideoTag;
	tag.ts_ms = ClampTs(ts_ms);
	tag.body.clear();
	tag.body.reserve(size + 5);
	tag.body.push_back(keyframe ? 0x17 : 0x27);
	tag.body.push_back(0x01); // AVCPacketType = NALU
	PutI24(tag.body, ct_ms);
	tag.body.insert(tag.body.end(), nalu, nalu + size);
	return true;
}

bool MakeAudioHeaderTag(const uint8_t *asc, size_t size, FlvTag &tag)
{
	if (!asc || size == 0)
		return false;
	tag.type = kAudioTag;
	tag.ts_ms = 0;
	tag.body.clear();
	tag.body.reserve(size + 2);
	tag.body.push_back(0xAF); // AAC, 44kHz, 16bit, stereo
	tag.body.push_back(0x00); // AACPacketType = sequence header
	tag.body.insert(tag.body.end(), asc, asc + size);
	return true;
}

bool MakeAudioFrameTag(const uint8_t *aac, size_t size, int64_t ts_ms, FlvTag &tag)
{
	if (!aac || size == 0)
		return false;
	tag.type = kAudioTag;
	tag.ts_ms = ClampTs(ts_ms);
	tag.body.clear();
	tag.body.reserve(size + 2);
	tag.body.push_back(0xAF);
	tag.body.push_back(0x01); // AAC raw
	tag.body.insert(tag.body.end(), aac, aac + size);
	return true;
}

namespace {

// 上游 flv-mux.c（OBS 32.2）：FRAME_HEADER_EX = 8<<4；FT_KEY = 1<<4，FT_INTER = 2<<4；
// PACKETTYPE_SEQ_START=0 / FRAMES=1 / SEQ_END=2 / FRAMESX=3 / METADATA=4。
constexpr uint8_t kFrameHeaderEx = 0x80;
constexpr uint8_t kFtKey = 0x10;
constexpr uint8_t kFtInter = 0x20;
constexpr uint8_t kPktSeqStart = 0;
constexpr uint8_t kPktFrames = 1;
constexpr uint8_t kPktSeqEnd = 2;
constexpr uint8_t kPktFramesX = 3;
constexpr uint8_t kPktMetadata = 4;

void PutFourcc(std::vector<uint8_t> &out, EnhancedVideoCodec codec)
{
	const char *cc = codec == EnhancedVideoCodec::Hevc ? "hvc1" : "av01";
	for (int i = 0; i < 4; i++)
		out.push_back(static_cast<uint8_t>(cc[i]));
}

} // namespace

bool MakeVideoHeaderTagEx(const uint8_t *payload, size_t size, EnhancedVideoCodec codec, FlvTag &tag)
{
	if (!payload || size == 0)
		return false;
	tag.type = kVideoTag;
	tag.ts_ms = 0; // 上游序列头 packet dts=0
	tag.body.clear();
	tag.body.reserve(size + 5);
	tag.body.push_back(kFrameHeaderEx | kPktSeqStart | kFtKey); // 0x90（上游 header 恒 keyframe=true）
	PutFourcc(tag.body, codec);
	tag.body.insert(tag.body.end(), payload, payload + size);
	return true;
}

bool MakeVideoFrameTagEx(const uint8_t *payload, size_t size, bool keyframe, int64_t ts_ms, int32_t ct_ms,
			 bool dts_eq_pts, EnhancedVideoCodec codec, FlvTag &tag)
{
	if (!payload || size == 0)
		return false;
	// 上游 flv_packet_frames：H.264/HEVC 且 dts==pts 用 FRAMESX 省 3 字节 CT；AV1 从无 CT。
	const bool needs_ct = codec == EnhancedVideoCodec::Hevc;
	const int packet_type = needs_ct && dts_eq_pts ? kPktFramesX : kPktFrames;

	tag.type = kVideoTag;
	tag.ts_ms = ClampTs(ts_ms);
	tag.body.clear();
	tag.body.reserve(size + 5 + 3);
	tag.body.push_back(kFrameHeaderEx | static_cast<uint8_t>(packet_type) | (keyframe ? kFtKey : kFtInter));
	PutFourcc(tag.body, codec);
	if (needs_ct && packet_type == kPktFrames)
		PutI24(tag.body, ct_ms);
	tag.body.insert(tag.body.end(), payload, payload + size);
	return true;
}

bool MakeVideoSeqEndTag(int64_t ts_ms, EnhancedVideoCodec codec, FlvTag &tag)
{
	tag.type = kVideoTag;
	// 上游 footer 包 dts=0 → ts = 0 - start_dts_offset，s_wtimestamp 按 int32 环绕写出。
	tag.ts_ms = static_cast<uint32_t>(static_cast<int32_t>(ts_ms));
	tag.body.clear();
	tag.body.reserve(5);
	tag.body.push_back(kFrameHeaderEx | kPktSeqEnd | kFtInter); // 0xA2（footer keyframe=false）
	PutFourcc(tag.body, codec);
	return true;
}

bool MakeColorInfoTag(EnhancedVideoCodec codec, int bits_per_raw_sample, int color_primaries, int color_trc,
		      int color_space, int min_luminance, int max_luminance, FlvTag &tag)
{
	std::vector<uint8_t> payload;
	// 上游 flv_packet_metadata 的 AMF0 载荷（含其怪癖："colorInfo" 前带 STRING marker，
	// 对象成员名则不带 marker）。
	payload.push_back(0x02); // DATA_TYPE_STRING
	Amf::WriteKey(payload, "colorInfo");
	Amf::BeginObject(payload);
	{
		Amf::WriteKey(payload, "colorConfig");
		Amf::BeginObject(payload);
		{
			Amf::WriteKey(payload, "bitDepth");
			Amf::WriteNumber(payload, bits_per_raw_sample);
			Amf::WriteKey(payload, "colorPrimaries");
			Amf::WriteNumber(payload, color_primaries);
			Amf::WriteKey(payload, "transferCharacteristics");
			Amf::WriteNumber(payload, color_trc);
			Amf::WriteKey(payload, "matrixCoefficients");
			Amf::WriteNumber(payload, color_space);
		}
		Amf::EndObject(payload);
		if (max_luminance != 0) {
			Amf::WriteKey(payload, "hdrMdcv");
			Amf::BeginObject(payload);
			{
				Amf::WriteKey(payload, "maxLuminance");
				Amf::WriteNumber(payload, max_luminance);
				Amf::WriteKey(payload, "minLuminance");
				Amf::WriteNumber(payload, min_luminance);
			}
			Amf::EndObject(payload);
		}
	}
	Amf::EndObject(payload);

	tag.type = kVideoTag;
	tag.ts_ms = 0; // 上游 s_wtimestamp(0)
	tag.body.clear();
	tag.body.reserve(payload.size() + 5);
	tag.body.push_back(kFrameHeaderEx | kPktMetadata); // 无 frame_type 位（上游同款）
	PutFourcc(tag.body, codec);
	tag.body.insert(tag.body.end(), payload.begin(), payload.end());
	return true;
}

} // namespace Srt
