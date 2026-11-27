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

// ── Enhanced RTMP（Y2023 FourCC 封装）──────────────────────────────────────────
// 插件自有扩展（E-26 §3 决策）：官方 rtmpsrt 引擎只承载 H.264/AAC，HEVC/AV1 走 BMT。
// 这里的字节口径 = 上游 OBS 32.2 `plugins/obs-outputs/flv-mux.c` `flv_packet_ex` 族
// （用户实测 OBS 以 ERTMP 向 B站推 HEVC/AV1 可行，插件把同一口径推广到 rtmpsrt）。
enum class EnhancedVideoCodec {
	Hevc, // FourCC "hvc1"
	Av1,  // FourCC "av01"
};

// payload: HEVC = hvcC（BuildHevcHvcC 产物）；AV1 = av1C+OBU（BuildAv1CodecConfRecord 产物）。
bool MakeVideoHeaderTagEx(const uint8_t *payload, size_t size, EnhancedVideoCodec codec, FlvTag &tag);
// payload: HEVC = 4 字节长度前缀 NAL（obs_parse_hevc_packet 产物）；AV1 = ConvertAv1TemporalUnit 产物。
// dts_eq_pts：HEVC 用它走 FRAMESX 省 CT（上游 flv_packet_frames 优化）；AV1 无 CT。
bool MakeVideoFrameTagEx(const uint8_t *payload, size_t size, bool keyframe, int64_t ts_ms, int32_t ct_ms,
			 bool dts_eq_pts, EnhancedVideoCodec codec, FlvTag &tag);
// 停播收尾 SEQ_END（上游 send_footers：track0 非 H.264 才发）。ts 用上游同款 int32 环绕。
bool MakeVideoSeqEndTag(int64_t ts_ms, EnhancedVideoCodec codec, FlvTag &tag);
// HDR colorInfo metadata tag（上游 flv_packet_metadata，仅非 H.264 且 PQ/HLG 时发，ts 恒 0）。
bool MakeColorInfoTag(EnhancedVideoCodec codec, int bits_per_raw_sample, int color_primaries, int color_trc,
		      int color_space, int min_luminance, int max_luminance, FlvTag &tag);

// 这里**没有** onMetaData/@setDataFrame：官方只在 HDR 分支才发 metadata，
// 普通 H.264/AAC 场次只发序列头 + 媒体 tag（E-25 §6），我们照做。

} // namespace Srt
