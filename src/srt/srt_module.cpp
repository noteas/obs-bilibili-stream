#include "srt_module.hpp"

#ifdef BILI_ENABLE_SRT

#include <obs-module.h>

// fork（obs-rtmp-srt）rtmp-stream.c 里的输出定义；身份 = id "bvc_srt_output" /
// protocols "bvc_srt"（ADR-0007：控制台服务目录按此协议串让 OBS 自动实例化本输出）。
extern "C" struct obs_output_info rtmp_output_info;

namespace Srt {

void Register()
{
	obs_register_output(&rtmp_output_info);
}

} // namespace Srt

#else // !BILI_ENABLE_SRT

namespace Srt {

void Register() {}

} // namespace Srt

#endif
