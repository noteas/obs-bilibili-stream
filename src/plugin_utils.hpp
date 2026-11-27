#pragma once
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;
extern const char *PLUGIN_COMMIT;

void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}

// i18n：OBS 标准 locale 机制（模块入口 OBS_MODULE_USE_DEFAULT_LOCALE，数据文件
// data/locale/{zh-CN,en-US}.ini，缺键回落到 default_locale=zh-CN）。
// Text(key) 取文案；Text(key, args...) 以文案为 printf 格式串填入动态部分，
// 语序由各语言 ini 自行决定（不要把文案拆成前后缀在 C++ 里拼）。
#include <cstdio>
#include <string>
#include <vector>
#include <obs-module.h>

namespace Utils {

inline std::string Text(const char *key)
{
	return obs_module_text(key);
}

template<typename... Args> std::string Text(const char *key, const Args &...args)
{
	const std::string fmt = obs_module_text(key);
	const int n = std::snprintf(nullptr, 0, fmt.c_str(), args...);
	if (n <= 0)
		return {};
	std::vector<char> buf(static_cast<size_t>(n) + 1);
	std::snprintf(buf.data(), buf.size(), fmt.c_str(), args...);
	return std::string(buf.data(), static_cast<size_t>(n));
}

} // namespace Utils

#endif
