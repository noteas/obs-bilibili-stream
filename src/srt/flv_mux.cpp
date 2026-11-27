#include "srt/flv_mux.hpp"

#include <algorithm>

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

} // namespace Srt
