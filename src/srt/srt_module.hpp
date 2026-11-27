#pragma once

namespace Srt {

// 注册私有 SRT 的 output（OBS 模块加载时调用一次）。
// 服务下拉条目由 srt_catalog 写入 rtmp-services 的 services.json，
// 协议名 bvc_srt 由本 output 的 protocols 注册。
void Register();
void Unregister();

} // namespace Srt
