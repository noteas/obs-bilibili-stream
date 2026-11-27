#include <obs-module.h>
#include <obs-frontend-api.h>
#include "ui/console.hpp"
#include "bilibili_api.hpp"
#include "http_client.hpp"
#include "plugin_utils.hpp"

#ifdef BILI_ENABLE_SRT
#include "srt/srt_module.hpp"
#endif

OBS_DECLARE_MODULE()
// NOTE: deliberately no OBS_MODULE_USE_DEFAULT_LOCALE — since ADR-0003 all UI
// text is a hard-coded Chinese literal, obs_module_text() is unused.

// The single tools-menu entry opens the console (ADR-0003): all state, all
// actions (login / room / live control) live in the console source's properties
// dialog — the menu stays a launcher only.
static void menu_open_console(void *)
{
	UI::ConsoleOpenPanel();
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "插件版本: %s, commit: %s", PLUGIN_VERSION, PLUGIN_COMMIT);
	Bili::BiliApi::init();
#ifdef BILI_ENABLE_SRT
	Srt::Register();
#endif
	obs_register_source(UI::ConsoleSourceInfo());
	UI::ConsoleCreate();

	obs_frontend_add_tools_menu_item("B站直播控制台", menu_open_console, nullptr);

	obs_log(LOG_INFO, "插件加载成功");
	return true;
}

void obs_module_post_load(void)
{
	// 此时所有模块（含 rtmp-services）都已加载，再把私有 SRT 服务条目合并进
	// 服务目录；不写用户的推流配置（ADR-0007）。
#ifdef BILI_ENABLE_SRT
	UI::ConsoleRefreshSrtCatalog();
#endif
}

void obs_module_unload(void)
{
	// Order matters: the console drops its source and refuses queued
	// continuations first; then the HTTP layer stops the worker and closes
	// the delivery channel.
	UI::ConsoleDestroy();
#ifdef BILI_ENABLE_SRT
	Srt::Unregister();
#endif
	Bili::BiliApi::cleanup();
	Http::HttpClient::cleanup();
	obs_log(LOG_INFO, "插件已卸载");
}
