#include <obs-module.h>

#include <srt/srt.h>

#include "srt/srt_module.hpp"
#include "srt/srt_output.hpp"
#include "srt/srt_transport.hpp"

namespace Srt {

void Register()
{
	InitOutputInfo();
	obs_register_output(OutputInfo());
	EnsureSrtStarted();
	blog(LOG_INFO, "[bvc-srt] 私有 SRT 输出已注册（bvc_srt_output / protocol=bvc_srt）");
}

void Unregister()
{
	srt_cleanup();
}

} // namespace Srt
