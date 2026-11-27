#pragma once

// SRT 输出注册壳（ADR-0007/0008）：把 fork（third_party/obs-rtmp-srt）的 RTMP-over-SRT
// 输出接进 OBS 输出体系。BILI_ENABLE_SRT 关闭时为空操作。
// 与 console source 同款生命周期：只注册不注销（libobs 无 output 注销 API，插件
// 生命周期 = OBS 进程）。
namespace Srt {

void Register();

} // namespace Srt
