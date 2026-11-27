// 无头集成测试(tests/harness):用**真实 libobs + 真实 Qt(offscreen)+ 假 B站服务端**把
// 编译出来的插件 .so 当成 OBS 会做的那样装进来(obs_open_module → obs_init_module →
// obs_shutdown),再从外部驱动侧栏:点按钮、读文案、处理确认框,并对照假服务端的请求
// 日志断言「发了什么、没发什么」。本文件扮演 OBS 前端:导出插件引用的全部
// obs_frontend_* 符号(可执行体 ENABLE_EXPORTS,优先级高于插件的 NEEDED 库)。
//
// 用法: bili_harness <插件.so> <data 目录> <OBS 配置目录> <请求日志> <场景名>
// 环境由 run_headless.sh 准备(QT_QPA_PLATFORM=offscreen、https_proxy、SSL_CERT_FILE)。
#include <obs.h>
#include <obs-frontend-api.h>

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDockWidget>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "json11/json11.hpp"

// ---------------------------------------------------------------------------
// 「OBS 前端」替身:只实现插件用到的 obs_frontend_* 子集,行为照抄 OBS 32.2
// (frontend/OBSStudioAPI.cpp):dock = QDockWidget(objectName = id)+ 右侧停靠区 +
// 初始隐藏浮动;事件回调按注册顺序同步派发(主线程)。
// ---------------------------------------------------------------------------
static QMainWindow *g_main = nullptr;
static bool g_streaming = false;
static int g_properties_opens = 0;
static std::atomic<int> g_docks_alive{0};
static std::atomic<int> g_properties_rebuilds{0};

struct EventCb {
	obs_frontend_event_cb cb;
	void *pd;
};
static std::vector<EventCb> g_events;

extern "C" {
void *obs_frontend_get_main_window(void)
{
	return g_main;
}

bool obs_frontend_streaming_active(void)
{
	return g_streaming;
}

// 模拟 PropertiesView:控制台对话框打开期间,obs_source_update_properties(source) 会让
// OBS 排队重建属性表(重新调 get_properties,见 OBS shared/properties-view/
// properties-view.cpp 的 ReloadProperties)。这里同样重取一次,好让「重建 → 再发请求 →
// 再重建」这类死循环在测试里可见(它不会让进程崩溃,只会狂发请求)。
static obs_source_t *g_console_source = nullptr;

static void on_update_properties(void *, calldata_t *)
{
	if (!g_console_source)
		return;
	++g_properties_rebuilds;
	obs_properties_t *props = obs_source_properties(g_console_source);
	obs_properties_destroy(props);
}

void obs_frontend_open_source_properties(obs_source_t *source)
{
	++g_properties_opens;
	if (!g_console_source) {
		g_console_source = source;
		signal_handler_connect(obs_source_get_signal_handler(source), "update_properties", on_update_properties,
				       nullptr);
	}
	obs_properties_t *props = obs_source_properties(source);
	obs_properties_destroy(props);
}

struct MenuItem {
	std::string name;
	obs_frontend_cb cb;
	void *pd;
};
static std::vector<MenuItem> g_menu;
void obs_frontend_add_tools_menu_item(const char *name, obs_frontend_cb callback, void *private_data)
{
	g_menu.push_back({name ? name : "", callback, private_data});
}

bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget)
{
	const QString name = QString::fromUtf8(id);
	if (g_main->findChild<QDockWidget *>(name))
		return false;
	QDockWidget *dock = new QDockWidget(QString::fromUtf8(title), g_main);
	dock->setObjectName(name);
	dock->setWidget(static_cast<QWidget *>(widget));
	g_main->addDockWidget(Qt::RightDockWidgetArea, dock);
	dock->setVisible(false);
	dock->setFloating(true);
	++g_docks_alive;
	QObject::connect(dock, &QObject::destroyed, [](QObject *) { --g_docks_alive; });
	return true;
}

void obs_frontend_remove_dock(const char *id)
{
	QDockWidget *dock = g_main->findChild<QDockWidget *>(QString::fromUtf8(id));
	if (!dock)
		return;
	g_main->removeDockWidget(dock);
	delete dock; // 连同内容控件一起销毁(OBS: extraDocks[idx].reset())
}

void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	g_events.push_back({callback, private_data});
}

void obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	for (auto it = g_events.begin(); it != g_events.end(); ++it) {
		if (it->cb == callback && it->pd == private_data) {
			g_events.erase(it);
			return;
		}
	}
}
}

static void fire_event(enum obs_frontend_event event)
{
	// OBS 32.2 OBSStudioAPI::on_event:从尾到头遍历(允许回调在派发中注销自己)。
	for (size_t i = g_events.size(); i > 0; i--) {
		if (i - 1 >= g_events.size())
			continue;
		const EventCb cb = g_events[i - 1];
		cb.cb(event, cb.pd);
	}
}

// ---------------------------------------------------------------------------
// UI 任务队列:与 OBS 的 ui_task_handler(OBSApp.cpp)同义——已在主线程就直调,
// 否则投递到 Qt 主线程。
// ---------------------------------------------------------------------------
static void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	auto run = [task, param]() {
		task(param);
	};
	if (QThread::currentThread() == qApp->thread()) {
		run();
		return;
	}
	QMetaObject::invokeMethod(qApp, run, wait ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// 断言与驱动小工具
// ---------------------------------------------------------------------------
static int g_fail = 0;
static int g_pass = 0;
static std::string g_scenario;

static void check(bool cond, const std::string &what)
{
	if (cond) {
		++g_pass;
		return;
	}
	++g_fail;
	std::printf("FAIL[%s]: %s\n", g_scenario.c_str(), what.c_str());
}

// 等待条件成立:期间持续跑 Qt 事件循环(UI 续体、定时器都靠它)。
static bool wait_until(const std::function<bool()> &pred, int timeout_ms = 15000)
{
	QElapsedTimer timer;
	timer.start();
	while (!pred()) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		QThread::msleep(10);
		if (timer.elapsed() > timeout_ms)
			return false;
	}
	QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	return true;
}

static void pump(int ms)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < ms) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		QThread::msleep(10);
	}
}

static QDockWidget *dock()
{
	return g_main ? g_main->findChild<QDockWidget *>(QString::fromUtf8("bilibiliStreamDock")) : nullptr;
}

static QWidget *content()
{
	QDockWidget *d = dock();
	return d ? d->widget() : nullptr;
}

static QPushButton *button(const char *text)
{
	QWidget *c = content();
	if (!c)
		return nullptr;
	const QString want = QString::fromUtf8(text);
	for (QPushButton *b : c->findChildren<QPushButton *>()) {
		if (b->text() == want)
			return b;
	}
	return nullptr;
}

static bool label_has(const char *text)
{
	QWidget *c = content();
	if (!c)
		return false;
	const QString want = QString::fromUtf8(text);
	for (QLabel *l : c->findChildren<QLabel *>()) {
		if (l->text().contains(want))
			return true;
	}
	return false;
}

static bool line_has(const char *text)
{
	QWidget *c = content();
	if (!c)
		return false;
	const QString want = QString::fromUtf8(text);
	for (QLineEdit *e : c->findChildren<QLineEdit *>()) {
		if (e->text().contains(want))
			return true;
	}
	return false;
}

static int line_count()
{
	QWidget *c = content();
	return c ? static_cast<int>(c->findChildren<QLineEdit *>().size()) : 0;
}

static bool has_qr_image()
{
	QWidget *c = content();
	if (!c)
		return false;
	for (QLabel *l : c->findChildren<QLabel *>()) {
		if (!l->pixmap().isNull())
			return true;
	}
	return false;
}

static void click(const char *text)
{
	QPushButton *b = button(text);
	check(b != nullptr, std::string("button exists: ") + text);
	if (b)
		b->click();
}

// ---------------------------------------------------------------------------
// 模态框自动处理:插件用 QMessageBox::exec()(嵌套事件循环),这里用定时器轮询
// activeModalWidget(),记下文案与按钮,再点指定文本的按钮让 exec() 返回。
// ---------------------------------------------------------------------------
struct DialogSeen {
	std::string text;
	std::vector<std::string> buttons;
};
static std::vector<DialogSeen> g_dialogs;

static void arm_dialog_click(const char *button_text)
{
	QTimer *timer = new QTimer(qApp);
	timer->setInterval(20);
	QObject::connect(timer, &QTimer::timeout, timer, [timer, button_text]() {
		QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
		if (!box)
			return;
		DialogSeen seen;
		seen.text = box->text().toStdString();
		const QList<QAbstractButton *> buttons = box->findChildren<QAbstractButton *>();
		for (QAbstractButton *b : buttons)
			seen.buttons.push_back(b->text().toStdString());
		g_dialogs.push_back(seen);
		timer->stop();
		timer->deleteLater();
		for (QAbstractButton *b : buttons) {
			if (b->text() == QString::fromUtf8(button_text)) {
				b->click();
				return;
			}
		}
		box->reject(); // 没找到目标按钮:别把测试挂死
	});
	timer->start();
}

static size_t dialog_count()
{
	return g_dialogs.size();
}

// ---------------------------------------------------------------------------
// 请求日志(假服务端写的 JSON Lines)
// ---------------------------------------------------------------------------
struct Request {
	std::string method, host, path, cookie, body;
	int status = 0;
};
static std::string g_request_log;

static std::vector<Request> requests()
{
	std::vector<Request> out;
	QFile file(QString::fromStdString(g_request_log));
	if (!file.open(QIODevice::ReadOnly))
		return out;
	for (const QByteArray &line : file.readAll().split('\n')) {
		if (line.trimmed().isEmpty())
			continue;
		std::string err;
		const json11::Json j = json11::Json::parse(line.toStdString(), err);
		if (!err.empty() || !j.is_object())
			continue;
		Request r;
		r.method = j["method"].string_value();
		r.host = j["host"].string_value();
		r.path = j["path"].string_value();
		r.cookie = j["cookie"].string_value();
		r.body = j["body"].string_value();
		r.status = j["status"].int_value();
		out.push_back(r);
	}
	return out;
}

static int count_requests(const char *method, const char *path_contains, const char *body_contains = nullptr)
{
	int n = 0;
	for (const Request &r : requests()) {
		if (r.method != method || r.path.find(path_contains) == std::string::npos)
			continue;
		if (body_contains && r.body.find(body_contains) == std::string::npos)
			continue;
		++n;
	}
	return n;
}

static void write_file(const std::string &path, const std::string &text)
{
	QFileInfo info(QString::fromStdString(path));
	QDir().mkpath(info.absolutePath());
	QFile f(QString::fromStdString(path));
	f.open(QIODevice::WriteOnly | QIODevice::Truncate);
	f.write(text.data(), static_cast<qint64>(text.size()));
}

// 已登录态的种子配置(模拟上次 OBS 正常退出):插件启动即带 Cookie 对账刷新。
// 路径 = <obs_startup 的 module_config_path>/<模块名>/config.json(libobs
// obs_module_get_config_path:目录名 = 模块文件名去掉扩展名;真 OBS 传的是
// <用户配置>/plugin_config)。
static void seed_config(const std::string &config_dir, bool with_lines, bool dock_introduced = true)
{
	const std::string lines =
		with_lines
			// 与 ConfigManager::save 同形:线路表是「存成字符串的 JSON 数组」(obs_data 没有数组值)。
			? R"(,"stream_targets":"[{\"protocol\":\"rtmp\",\"provider\":\"txy\",\"addr\":\"rtmp://txy.live-push.bilivideo.com/live-bvc/\",\"key\":\"?streamname=live42&key=SEEDED\",\"backup\":false}]")"
			: R"(,"stream_targets":"[]")";
	write_file(
		config_dir + "/bilibili-stream-for-obs/config.json",
		R"({"room_id":"21452505","csrf_token":"csrf-fake-1234567890","mid":"42","title":"测试直播间","part_id":2,"area_id":86,"cookies":"SESSDATA=sess-fake-1234567890; bili_jct=csrf-fake-1234567890; DedeUserID=42; buvid3=BUVID3-seeded")" +
			lines + R"(,"dock_introduced":)" + (dock_introduced ? "true" : "false") + "}");
}

static std::string g_state_dir;

static void script(const std::string &json_text)
{
	write_file(g_state_dir + "/script.json", json_text);
}

// 侧栏文案(zh-CN,插件 default_locale)。按 UTF-8 字面量写,便于对照 data/locale/zh-CN.ini。
static const char *kBtnLogin = "扫码登录";
static const char *kBtnCancelLogin = "取消登录";
static const char *kBtnStart = "获取推流码";
static const char *kBtnStop = "关闭直播间";
static const char *kBtnStopConfirm = "确认关闭直播间";
static const char *kBtnCancel = "取消";
static const char *kBtnOk = "确定";
static const char *kBtnRecover = "找回推流码";
static const char *kBtnSettings = "直播间设置…";
static const char *kBtnCopyKey = "复制推流码";
static const char *kNotLoggedIn = "未登录";
static const char *kOffline = "未开播";
static const char *kLive = "直播中";
static const char *kObsStreaming = "推流中";
static const char *kObsIdle = "未推流";
static const char *kClosed = "直播间已关闭";
static const char *kCheckFail = "检查登录状态失败";
static const char *kRecovered = "已找回";
static const char *kBackupRejected = "备流被服务端拒绝";
static const char *kRoomId = "21452505";
static const char *kTitle = "测试直播间";

// 等到「已登录且房间态到货」:登录二维码流程(约 6 s)或种子配置的启动刷新。
static bool wait_logged_in(int timeout_ms = 30000)
{
	return wait_until([]() { return label_has(kRoomId); }, timeout_ms);
}

// ---------------------------------------------------------------------------
// 场景
// ---------------------------------------------------------------------------

static void scenario_startup_logged_out()
{
	check(wait_until([]() { return button(kBtnLogin) != nullptr && button(kBtnLogin)->isEnabled(); }),
	      "logged out: 扫码登录 button present and enabled");
	check(label_has(kNotLoggedIn), "logged out: status says 未登录");
	check(button(kBtnStart) == nullptr, "logged out: 获取推流码 hidden (not logged in)");
	pump(500);
	check(count_requests("GET", "/x/web-interface/nav") == 0, "logged out: no nav without SESSDATA");
	check(count_requests("GET", "/x/frontend/finger/spi") == 1, "logged out: buvid bootstrap once");
}

static void scenario_login_get_key_close()
{
	check(button(kBtnLogin) != nullptr, "login: button present");
	click(kBtnLogin);
	check(wait_until([]() { return has_qr_image(); }, 10000), "login: QR image rendered in dock");
	check(button(kBtnCancelLogin) != nullptr, "login: 取消登录 offered");
	check(wait_logged_in(), "login: signed in after poll sequence");
	check(label_has(kOffline) && label_has(kObsIdle) && label_has(kTitle),
	      "login: room box shows offline/idle/title");
	const size_t getinfo = count_requests("GET", "/xlive/app-blink/v1/room/GetInfo");
	check(getinfo == 1, "login: exactly one room/GetInfo per refresh (was 2)");
	check(count_requests("GET", "/x/passport-login/web/sso/list") == 1, "login: sso list once, after login only");
	check(count_requests("GET", "/x/passport-login/web/qrcode/poll") == 4, "login: 4 polls (86101,86101,86090,0)");
	check(count_requests("GET", "/x/web-interface/nav") >= 1, "login: nav called");
	// Cookie 白名单:非 B站域名不带 Cookie。
	for (const Request &r : requests()) {
		if (r.host.find("not-bilibili.example.com") != std::string::npos)
			check(r.cookie.empty(), "login: no Cookie header to non-Bilibili SSO host");
	}

	check(button(kBtnStart) && button(kBtnStart)->isEnabled(), "start: 获取推流码 enabled when offline");
	check(button(kBtnStop) && !button(kBtnStop)->isEnabled(), "start: 关闭直播间 disabled before going live");
	click(kBtnStart);
	check(wait_until([]() { return line_has("K-SRT") && line_has("K-RTMP") && line_has("K-BACKUP"); }),
	      "start: three lines with their own keys shown (bmt without addr dropped)");
	check(line_count() == 6, "start: 3 lines × (address + key)");
	const int starts = count_requests("POST", "/room/v1/Room/startLive");
	check(starts == 1, "start: exactly one startLive");
	const Request start = []() {
		for (const Request &r : requests())
			if (r.path.find("/room/v1/Room/startLive") != std::string::npos)
				return r;
		return Request();
	}();
	check(start.body.find("backup_stream=") == std::string::npos, "start: no backup params (privilege != 5)");
	check(start.body.find("room_id=21452505") != std::string::npos &&
		      start.body.find("area_v2=86") != std::string::npos,
	      "start: room_id + server-synced area_v2");
	check(start.body.find("type=1") != std::string::npos && start.body.find("checked=0") != std::string::npos &&
		      start.body.find("platform=pc_link") != std::string::npos &&
		      start.body.find("build=11349") != std::string::npos &&
		      start.body.find("version=8.16.0.11349") != std::string::npos &&
		      start.body.find("sign=") != std::string::npos,
	      "start: common params + sign present");
	check(count_requests("GET", "/xlive/app-blink/v1/preLive/PreCheck") == 1, "start: PreCheck once");
	// 线路不进日志(日志里只允许掩码)——由 run_headless.sh 检查 OBS 日志文件。
	QPushButton *copy = button(kBtnCopyKey);
	check(copy != nullptr, "lines: 复制推流码 button present");
	if (copy)
		copy->click();
	check(QApplication::clipboard()->text().contains("key=K-SRT"), "lines: copy puts first key on clipboard");
	check(wait_until([]() { return label_has(kLive); }), "start: quiet refresh confirms 直播中");
	check(button(kBtnStop) && button(kBtnStop)->isEnabled(), "live: 关闭直播间 enabled");

	// 关闭直播间:二次确认 → 确认
	arm_dialog_click(kBtnStopConfirm);
	click(kBtnStop);
	check(wait_until([]() { return dialog_count() == 1; }, 5000), "close: confirmation dialog shown");
	check(!g_dialogs.empty() && g_dialogs[0].text.find(kRoomId) != std::string::npos,
	      "close: dialog names the room");
	check(!g_dialogs.empty() && g_dialogs[0].text.find("<b>") != std::string::npos,
	      "close: server title passed through verbatim (plain text, not rich text)");
	check(wait_until([]() { return label_has(kClosed) && line_count() == 0; }), "close: status + lines cleared");
	check(count_requests("POST", "/room/v1/Room/stopLive") == 1, "close: exactly one stopLive");
	check(wait_until([]() { return label_has(kOffline); }), "close: quiet refresh confirms 未开播");
}

static void scenario_close_cancel()
{
	check(wait_logged_in(), "cancel: seeded session loaded");
	check(wait_until([]() { return button(kBtnStop) && button(kBtnStop)->isEnabled(); }), "cancel: stop enabled");
	check(line_has("SEEDED"), "cancel: seeded line visible without any local flag");
	arm_dialog_click(kBtnCancel);
	click(kBtnStop);
	check(wait_until([]() { return dialog_count() == 1; }, 5000), "cancel: dialog shown");
	pump(800);
	check(count_requests("POST", "/room/v1/Room/stopLive") == 0, "cancel: no stopLive sent");
	check(line_has("SEEDED"), "cancel: lines kept");
	check(button(kBtnStop) && button(kBtnStop)->isEnabled(), "cancel: stop still available");
}

static void scenario_streaming_guard()
{
	check(wait_logged_in(), "guard: seeded session loaded");
	g_streaming = true;
	fire_event(OBS_FRONTEND_EVENT_STREAMING_STARTED);
	check(wait_until([]() { return label_has(kObsStreaming); }), "guard: dock shows 推流中");
	arm_dialog_click(kBtnOk);
	click(kBtnStop);
	check(wait_until([]() { return dialog_count() == 1; }, 5000), "guard: warning dialog shown");
	check(!g_dialogs.empty() && g_dialogs[0].buttons.size() == 1, // 只有「确定」,没有确认按钮
	      "guard: warning has no confirm button");
	pump(500);
	check(count_requests("POST", "/room/v1/Room/stopLive") == 0, "guard: no stopLive while OBS streams");
	g_streaming = false;
	fire_event(OBS_FRONTEND_EVENT_STREAMING_STOPPED);
	check(wait_until([]() { return label_has(kObsIdle); }), "guard: dock back to 未推流");
}

static void scenario_network_error_keeps_login()
{
	check(wait_until([]() { return label_has(kCheckFail); }), "netfail: status reports login check failure");
	check(label_has(kRoomId), "netfail: room box still shown (state kept, not flipped to logged-out)");
	check(button(kBtnStart) && button(kBtnStart)->isEnabled(), "netfail: 获取推流码 still enabled");
	check(label_has("21452505"), "netfail: previous room data kept");
}

static void scenario_recover()
{
	check(wait_until([]() { return label_has(kLive) && button(kBtnRecover) && button(kBtnRecover)->isEnabled(); }),
	      "recover: live on server + 找回推流码 enabled");
	check(button(kBtnStart) && !button(kBtnStart)->isEnabled(),
	      "recover: 获取推流码 disabled while server is live");
	click(kBtnRecover);
	check(wait_until([]() { return line_has("RECOVERED") && label_has(kRecovered); }), "recover: line recovered");
	check(count_requests("GET", "/live_stream/v1/StreamList/get_stream_by_roomId") == 1,
	      "recover: primary source once");
	check(count_requests("GET", "/xlive/app-blink/v1/live/GetUpStreamRtmp") == 0, "recover: fallback not needed");
}

static void scenario_stop_rejected()
{
	check(wait_until([]() { return button(kBtnStop) && button(kBtnStop)->isEnabled(); }), "60034: stop enabled");
	arm_dialog_click(kBtnStopConfirm);
	click(kBtnStop);
	check(wait_until([]() { return dialog_count() == 1; }, 5000), "60034: dialog shown");
	check(wait_until([]() { return label_has("60034"); }), "60034: policy message surfaced");
	check(line_has("SEEDED"), "60034: lines kept after rejected stop");
	check(button(kBtnStop) && button(kBtnStop)->isEnabled(), "60034: can retry closing");
}

// OBS 重启后本地还留着上一场的线路,而服务端早已结束场次:启动对账发现未开播 ⇒ 清掉
// 失效线路(密钥每场新生成),「获取推流码」重新可用。
// 服务端 data.title 为空(上游 ae01b1b 的守卫):面板保留本地标题,不被刷成空白。
static void scenario_empty_title_keeps_local()
{
	check(wait_until([]() { return label_has(kOffline); }, 10000), "title: first refresh completed");
	check(label_has(kTitle), "title: local title kept when the server returns an empty title");
}

static void scenario_stale_lines_cleared()
{
	check(wait_until([]() { return button(kBtnStart) && button(kBtnStart)->isEnabled(); }),
	      "stale: 获取推流码 enabled once the server says offline");
	check(wait_until([]() { return label_has(kOffline); }), "stale: room shows 未开播");
	pump(300);
	check(line_count() == 0, "stale: previous session's lines dropped when server reports offline");
	check(!line_has("SEEDED"), "stale: stale key no longer displayed");
}

static void scenario_unload_while_login_polling()
{
	click(kBtnLogin);
	check(wait_until([]() { return has_qr_image(); }, 10000), "unload: login polling started");
	// 直接进入卸载(obs_shutdown → obs_module_unload → join):必须秒级返回且不崩。
}

static void scenario_exit_then_unload()
{
	check(wait_logged_in(), "exit: seeded session loaded");
	check(dock() != nullptr, "exit: dock exists");
	fire_event(OBS_FRONTEND_EVENT_EXIT);
	check(dock() == nullptr, "exit: dock removed on OBS_FRONTEND_EVENT_EXIT");
	check(g_docks_alive.load() == 0, "exit: dock widget destroyed");
	check(g_events.empty(), "exit: frontend event callback removed");
}

static void scenario_first_run_shows_dock()
{
	// dock_introduced=false + FINISHED_LOADING → 侧栏自动显示一次;再次触发不再动布局。
	check(wait_logged_in(), "firstrun: seeded session loaded");
	QDockWidget *d = dock();
	check(d && !d->isVisible(), "firstrun: OBS creates new docks hidden");
	fire_event(OBS_FRONTEND_EVENT_FINISHED_LOADING);
	pump(100);
	check(d && d->isVisible() && !d->isFloating(), "firstrun: dock shown and docked after FINISHED_LOADING");
}

static void scenario_backup_fallback()
{
	check(wait_logged_in(), "backup: seeded session loaded");
	check(wait_until([]() { return button(kBtnStart) && button(kBtnStart)->isEnabled(); }),
	      "backup: start enabled");
	click(kBtnStart);
	check(wait_until([]() { return label_has(kBackupRejected) && line_has("K-RTMP"); }),
	      "backup: fell back, lines shown");
	const int starts = count_requests("POST", "/room/v1/Room/startLive");
	check(starts == 2, "backup: exactly two startLive (backup attempt + fallback)");
	check(count_requests("POST", "/room/v1/Room/startLive", "backup_stream=1") == 1 &&
		      count_requests("POST", "/room/v1/Room/startLive", "backup_stream_type=2") == 1 &&
		      count_requests("POST", "/room/v1/Room/startLive", "stream_biz=Paylive") == 1,
	      "backup: first request carried the backup params");
	// 未在播时 get_stream_by_roomId 不给 stream_line ⇒ 备流请求不带 src(告警不阻断)。
	check(count_requests("POST", "/room/v1/Room/startLive", "src=") == 0,
	      "backup: no src when the server has no stream_line");
	check(count_requests("GET", "/live_stream/v1/StreamList/get_stream_by_roomId") >= 1, "backup: src looked up");
}

static void scenario_partition_fail_no_loop()
{
	check(wait_logged_in(), "parts: seeded session loaded");
	check(button(kBtnSettings) != nullptr, "parts: 直播间设置… present");
	click(kBtnSettings);
	check(wait_until([]() { return g_properties_opens == 1; }, 5000), "parts: console panel opened");
	check(wait_until([]() { return count_requests("GET", "/room/v1/Area/getList") == 1; }, 5000),
	      "parts: one getList attempt");
	pump(1500);
	check(count_requests("GET", "/room/v1/Area/getList") == 1, "parts: failed load is not retried in a loop");
	check(g_properties_rebuilds.load() <= 2, "parts: properties rebuilt at most twice (no rebuild loop)");
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	if (argc != 6) {
		std::printf("usage: bili_harness <plugin.so> <data dir> <obs config dir> <request log> <scenario>\n");
		return 2;
	}
	const std::string plugin = argv[1], data_dir = argv[2], config_dir = argv[3];
	g_request_log = argv[4];
	g_scenario = argv[5];
	QFileInfo info(QString::fromStdString(g_request_log));
	g_state_dir = info.absolutePath().toStdString();

	struct Scenario {
		const char *name;
		std::function<void()> run;
		const char *script;   // 假服务端应答脚本
		bool seed;            // 模块加载前写一份「上次正常退出」的 config.json
		bool seed_lines;      // 种子里带线路表
		bool dock_introduced; // 种子里侧栏是否已做过首启显示
	};
	const Scenario scenarios[] = {
		{"startup_logged_out", scenario_startup_logged_out, "", false, false, true},
		{"login_get_key_close", scenario_login_get_key_close, R"({"reset": true})", false, false, true},
		{"unload_while_login_polling", scenario_unload_while_login_polling, R"({"reset": true})", false, false,
		 true},
		{"close_cancel", scenario_close_cancel, R"({"reset": true, "room_live": 1})", true, true, true},
		{"streaming_guard", scenario_streaming_guard, R"({"reset": true, "room_live": 1})", true, true, true},
		{"network_error_keeps_login", scenario_network_error_keeps_login,
		 R"({"reset": true, "nav_code": -412})", true, false, true},
		{"recover", scenario_recover, R"({"reset": true, "room_live": 1})", true, false, true},
		{"stop_rejected", scenario_stop_rejected, R"({"reset": true, "room_live": 1, "stop_code": 60034})",
		 true, true, true},
		{"exit_then_unload", scenario_exit_then_unload, R"({"reset": true})", true, false, true},
		{"first_run_shows_dock", scenario_first_run_shows_dock, R"({"reset": true})", true, false, false},
		{"backup_fallback", scenario_backup_fallback,
		 R"({"reset": true, "visible_mode": 5, "start_codes": [70003]})", true, false, true},
		{"partition_fail_no_loop", scenario_partition_fail_no_loop,
		 R"({"reset": true, "partition_fail": true})", true, false, true},
		{"stale_lines_cleared", scenario_stale_lines_cleared, R"({"reset": true, "room_live": 0})", true, true,
		 true},
		{"empty_title_keeps_local", scenario_empty_title_keeps_local,
		 R"({"reset": true, "room_live": 0, "title_empty": true})", true, false, true},
	};
	const Scenario *scenario = nullptr;
	for (const Scenario &sc : scenarios)
		if (g_scenario == sc.name)
			scenario = &sc;
	if (!scenario) {
		std::printf("unknown scenario %s\n", g_scenario.c_str());
		return 2;
	}
	// 种子(已登录态)必须在模块加载前落盘:插件 obs_module_load 时读它。
	if (scenario->seed)
		seed_config(config_dir, scenario->seed_lines, scenario->dock_introduced);
	script(scenario->script);

	QMainWindow window;
	g_main = &window;
	window.resize(1280, 800);
	window.show();

	if (!obs_startup("zh-CN", config_dir.c_str(), nullptr)) {
		std::printf("FAIL: obs_startup\n");
		return 2;
	}
	obs_set_ui_task_handler(ui_task_handler);

	obs_module_t *module = nullptr;
	if (obs_open_module(&module, plugin.c_str(), data_dir.c_str()) != MODULE_SUCCESS || !module ||
	    !obs_init_module(module)) {
		std::printf("FAIL: plugin failed to load (obs_open_module/obs_init_module)\n");
		obs_shutdown();
		return 2;
	}
	obs_post_load_modules();

	QElapsedTimer scenario_timer;
	scenario_timer.start();
	scenario->run();

	QElapsedTimer teardown;
	teardown.start();
	obs_shutdown(); // → obs_module_unload + dlclose:worker 必须 join,侧栏/监听必须已摘
	const qint64 teardown_ms = teardown.elapsed();
	check(teardown_ms < 4000, "teardown: obs_shutdown returned promptly (" + std::to_string(teardown_ms) + " ms)");
	std::printf("%s[%s]: %d passed, %d failed (scenario %lld ms, teardown %lld ms)\n", g_fail ? "FAILED" : "PASSED",
		    g_scenario.c_str(), g_pass, g_fail, static_cast<long long>(scenario_timer.elapsed()),
		    static_cast<long long>(teardown_ms));
	return g_fail ? 1 : 0;
}
