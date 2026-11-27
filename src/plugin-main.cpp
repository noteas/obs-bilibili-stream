#include <obs-module.h>
#include <obs-frontend-api.h>
#include "bilibili_api.hpp"
#include "core/session.hpp"
#include "http_client.hpp"
#include "plugin_utils.hpp"
#include "srt/srt_module.hpp"
#include "ui/console.hpp"
#include "ui/dock.hpp"

OBS_DECLARE_MODULE()
// Locale 处理走 OBS 标准宏(与上游 obs-outputs 同款):本 .so 内定义 obs_module_text()
// 等函数——libobs 不导出它们,缺了这行 rtmp-stream.c(fork 静态库)的引用会让 dlopen
// 报 undefined symbol。全部界面文案经 data/locale/{zh-CN,en-US}.ini 国际化
// (Utils::Text,见 plugin_utils.hpp);default_locale=zh-CN 兼作缺键回落层,
// libobs 在 obs_module_load() 之前完成 set_locale(32.2 obs-module.c 实证),
// 因此 load 阶段取文案安全。
OBS_MODULE_USE_DEFAULT_LOCALE("bilibili-stream-for-obs", "zh-CN")

// 工具菜单只做启动器:打开配置界面(控制台面板)。状态与常用操作在侧栏(Docks 菜单)。
static void menu_open_console(void *)
{
	UI::ConsoleOpenPanel();
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "插件版本: %s, commit: %s", PLUGIN_VERSION, PLUGIN_COMMIT);
	Http::HttpClient::init();
	obs_register_source(UI::ConsoleSourceInfo());
	Srt::Register();

	// 运行期状态的唯一所有者:读配置、灌 Cookie、已有登录态则对账刷新。
	Core::Session &session = Core::Session::instance();
	session.set_obs_streaming_probe([] { return obs_frontend_streaming_active(); });
	session.start();

	// 侧栏(Qt dock,ADR-0010)。本机 OBS 没有 dock API 时返回 false,插件退回
	// 「只有控制台」的形态,登录二维码改交系统浏览器。
	const bool dock = UI::DockCreate();
	session.set_login_qr_external(!dock);
	UI::ConsoleCreate();

	obs_frontend_add_tools_menu_item(Utils::Text("Menu.Console").c_str(), menu_open_console, nullptr);

	obs_log(LOG_INFO, "插件加载成功%s", dock ? "" : "(本机 OBS 无侧栏 API,仅控制台)");
	return true;
}

void obs_module_unload(void)
{
	// Order matters: 视图先摘掉自己和 Session 的监听(侧栏通常已在 OBS 的 EXIT
	// 事件里摘过);Session 停收续体;HTTP 层中止在途请求并 **join** worker——模块
	// 马上要被 dlclose,不能留一条线程跑在本模块的代码里;最后配平 libsrt。
	UI::DockDestroy();
	UI::ConsoleDestroy();
	Core::Session::instance().stop();
	Http::HttpClient::cleanup();
	Srt::Cleanup();
	obs_log(LOG_INFO, "插件已卸载");
}
