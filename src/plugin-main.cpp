#include <obs-module.h>
#include <obs-frontend-api.h>
#include "ui/console.hpp"
#include "bilibili_api.hpp"
#include "http_client.hpp"
#include "plugin_utils.hpp"
#include "srt/srt_module.hpp"

OBS_DECLARE_MODULE()
// Locale 处理走 OBS 标准宏（与上游 obs-outputs 同款）：本 .so 内定义 obs_module_text()
// 等函数——libobs 不导出它们，缺了这行 rtmp-stream.c（fork 静态库）的引用会让 dlopen
// 报 undefined symbol。全部界面文案经 data/locale/{zh-CN,en-US}.ini 国际化
// （Utils::Text，见 plugin_utils.hpp）；default_locale=zh-CN 兼作缺键回落层，
// libobs 在 obs_module_load() 之前完成 set_locale（32.2 obs-module.c 实证），
// 因此 load 阶段取文案安全。
OBS_MODULE_USE_DEFAULT_LOCALE("bilibili-stream-for-obs", "zh-CN")

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
	obs_register_source(UI::ConsoleSourceInfo());
	Srt::Register();
	UI::ConsoleCreate();

	obs_frontend_add_tools_menu_item(Utils::Text("Menu.Console").c_str(), menu_open_console, nullptr);

	obs_log(LOG_INFO, "插件加载成功");
	return true;
}

void obs_module_unload(void)
{
	// Order matters: the console drops its source and refuses queued
	// continuations first; then the HTTP layer stops the worker and closes
	// the delivery channel.
	UI::ConsoleDestroy();
	Bili::BiliApi::cleanup();
	Http::HttpClient::cleanup();
	Srt::Cleanup();
	obs_log(LOG_INFO, "插件已卸载");
}
