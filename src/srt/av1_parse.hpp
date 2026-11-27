#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Srt {

// extra data（OBU 序列或已是 AV1CodecConfigurationRecord）→ av1C + seq/meta OBU。
// 字节口径 = 上游 OBS 32.2 `plugins/obs-outputs/rtmp-av1.c` 的 `obs_parse_av1_header`。
bool BuildAv1CodecConfRecord(const uint8_t *extra, size_t size, std::vector<uint8_t> &out);

// 编码 packet → Enhanced-RTMP AV1 帧负载：剔除 Temporal Delimiter / Redundant Frame Header /
// Tile List OBU，其余 OBU 原样拼接；并按首帧头重算 keyframe/priority（上游 compute_av1_keyframe_priority，
// 编码器已给 priority 的（QSV/AMF）不覆盖）。口径 = 上游 `obs_parse_av1_packet`。
void ConvertAv1TemporalUnit(const uint8_t *data, size_t size, bool *keyframe, int *priority,
			    std::vector<uint8_t> &out);

} // namespace Srt
