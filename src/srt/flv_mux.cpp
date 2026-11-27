#include "srt/flv_mux.hpp"

#include <obs.h>

#include <algorithm>

#include "srt/amf.hpp"

namespace Srt {

namespace {

constexpr uint8_t kAudioTag = 8;
constexpr uint8_t kVideoTag = 9;
constexpr uint8_t kMetadataTag = 18;

uint32_t ClampTs(int64_t ms)
{
	if (ms < 0)
		return 0;
	if (ms > 0xFFFFFFFFLL)
		return 0xFFFFFFFFu;
	return static_cast<uint32_t>(ms);
}

void PutU24(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
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

bool MakeVideoFrameTag(const uint8_t *avcc, size_t size, bool keyframe, int64_t ts_ms, int32_t ct_ms, FlvTag &tag)
{
	if (!avcc || size == 0)
		return false;
	tag.type = kVideoTag;
	tag.ts_ms = ClampTs(ts_ms);
	tag.body.clear();
	tag.body.reserve(size + 5);
	tag.body.push_back(keyframe ? 0x17 : 0x27);
	tag.body.push_back(0x01); // AVCPacketType = NALU
	PutI24(tag.body, ct_ms);
	tag.body.insert(tag.body.end(), avcc, avcc + size);
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

bool MakeMetadataTag(obs_output *output, FlvTag &tag)
{
	if (!output)
		return false;

	uint32_t width = 0;
	uint32_t height = 0;
	if (obs_encoder_t *venc = obs_output_get_video_encoder(output)) {
		width = obs_encoder_get_width(venc);
		height = obs_encoder_get_height(venc);
	}

	std::vector<uint8_t> &b = tag.body;
	b.clear();
	Amf::WriteString(b, "@setDataFrame");
	Amf::WriteString(b, "onMetaData");
	Amf::BeginEcmaArray(b, 6);
	Amf::WriteKey(b, "duration");
	Amf::WriteNumber(b, 0.0);
	Amf::WriteKey(b, "width");
	Amf::WriteNumber(b, width);
	Amf::WriteKey(b, "height");
	Amf::WriteNumber(b, height);
	Amf::WriteKey(b, "videocodecid");
	Amf::WriteNumber(b, 7.0); // AVC
	Amf::WriteKey(b, "audiocodecid");
	Amf::WriteNumber(b, 10.0); // AAC
	Amf::WriteKey(b, "encoder");
	Amf::WriteString(b, "obs-bilibili-stream (rtmpsrt)");
	Amf::EndObject(b);

	tag.type = kMetadataTag;
	tag.ts_ms = 0;
	return true;
}

} // namespace Srt
