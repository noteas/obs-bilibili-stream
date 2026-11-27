#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Srt {

// Annex-B（或已是 hvcC）的 extra data → AVCCLike 的 HEVCDecoderConfigurationRecord(hvcC)。
// 字节口径 = 上游 OBS 32.2 `plugins/obs-outputs/rtmp-hevc.c` 的 `obs_parse_hevc_header`
// 逐字移植（Enhanced-RTMP/HEVC 序列头内容，E-26 §3 决策：插件自有扩展）。
// 成功返回 true；失败（含上游会踩未初始化指针的畸形输入路径）返回 false，out 保持为空。
bool BuildHevcHvcC(const uint8_t *extra, size_t size, std::vector<uint8_t> &out);

} // namespace Srt
