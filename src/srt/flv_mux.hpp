#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Srt {

// 一条待发送的 FLV tag（= RTMP 媒体消息体）：
//   type = 8(音频) / 9(视频)
//   ts_ms = 相对推流起点的毫秒时间戳
struct FlvTag {
	uint8_t type = 0;
	uint32_t ts_ms = 0;
	std::vector<uint8_t> body;
};

// v1 只支持官方 track0「old style for compatibility」的 H.264 + AAC 组合（E-24）。
// `avcc` 必须是 obs_parse_avc_header 产出的 AVCDecoderConfigurationRecord；
// `nalu` 是 obs_parse_avc_packet 产出的 AVCC 长度前缀形式（调用方负责）。
bool MakeVideoHeaderTag(const uint8_t *avcc, size_t size, FlvTag &tag);
bool MakeVideoFrameTag(const uint8_t *nalu, size_t size, bool keyframe, int64_t ts_ms, int32_t ct_ms, FlvTag &tag);

bool MakeAudioHeaderTag(const uint8_t *asc, size_t size, FlvTag &tag);
bool MakeAudioFrameTag(const uint8_t *aac, size_t size, int64_t ts_ms, FlvTag &tag);

// 这里**没有** onMetaData/@setDataFrame：官方只在 HDR 分支才发 metadata，
// 普通 H.264/AAC 场次只发序列头 + 媒体 tag（E-25 §6），我们照做。

} // namespace Srt
