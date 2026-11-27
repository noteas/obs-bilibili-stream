#include "ui/dock.hpp"

#ifdef BILI_ENABLE_DOCK

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QClipboard>
#include <QDockWidget>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <vector>
#ifdef _WIN32
// Qt 头已先于 windows.h 解析;这里再包含 windows.h 前挡掉 min/max 宏(std::max 会中招)。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/platform_util.hpp"
#include "core/session.hpp"
#include "plugin_utils.hpp"
#include "qrcodegen/qrcodegen.hpp"
#include "ui/console.hpp"

// 侧栏 = Core::Session 的视图。全部控件只在本文件内创建,且:
//  - 不定义任何 Q_OBJECT 子类(免 moc):信号一律用「Qt 自带信号 + lambda」连接,
//    lambda 的上下文对象是会随之销毁的控件,连接自动断开;
//  - 状态变化不就地重建控件(session_changed → 0ms 单发定时器 → 统一重建):
//    绝不在某个按钮的 clicked 信号还在发射时把它 delete 掉(控制台当年两次
//    coredump 就是这个形状);
//  - 内容控件是 OBS 的 OBSDock 的子控件,dock 销毁即跟着销毁;文件级指针全部
//    用 QPointer / 在 DockDestroy() 清零,模块卸载后不会有任何回调再进来。
namespace {

constexpr const char *kDockId = "bilibiliStreamDock";
constexpr int kRefreshIntervalMs = 60 * 1000; // 周期对账刷新(只在登录后、空闲时)
constexpr int kCopyHintMs = 2000;

QPointer<QWidget> g_root;          // 内容根控件(OBSDock 的子控件)
QTimer *g_rebuild_timer = nullptr; // g_root 的子对象
QTimer *g_refresh_timer = nullptr;
QTimer *g_copy_hint_timer = nullptr;
QPointer<QLabel> g_copy_hint; // 当前内容里的「已复制」提示
bool g_event_added = false;
std::string g_rendered_signature;

Core::Session &session()
{
	return Core::Session::instance();
}

// obs_frontend_add_dock_by_id / obs_frontend_remove_dock 自 OBS 30.0.2 才有。直接引用
// 这两个符号会让模块在 30.0.0/30.0.1 上因「未定义符号」整体加载失败(连私有 SRT
// 输出一起没了),所以运行期解析,缺失时退回「只有控制台」。
using add_dock_fn = bool (*)(const char *, const char *, void *);
using remove_dock_fn = void (*)(const char *);

template<typename T> T resolve_frontend_symbol(const char *name)
{
	T fn = nullptr;
#ifdef _WIN32
	FARPROC proc = GetProcAddress(GetModuleHandleW(L"obs-frontend-api.dll"), name);
#else
	void *proc = dlsym(RTLD_DEFAULT, name);
#endif
	// 函数指针与 void*/FARPROC 之间不做 reinterpret_cast(MSVC /W4 报 C4191),
	// 经由同尺寸对象 memcpy(表示一致由 static_assert 保证)。
	static_assert(sizeof(fn) == sizeof(proc), "function pointer size mismatch");
	if (proc)
		std::memcpy(&fn, &proc, sizeof(fn));
	return fn;
}

// ---------------------------------------------------------------------------
// 小部件工厂
// ---------------------------------------------------------------------------

// OBS 32 起主题自带 .text-danger/.text-warning/.text-success/.text-muted 类
// (frontend/data/themes/Yami.obt);更早的主题没有,退回显式颜色。
bool theme_has_text_classes()
{
	static const bool has = (obs_get_version() >> 16) >= 32;
	return has;
}

void set_text_class(QWidget *widget, const char *cls)
{
	if (theme_has_text_classes())
		widget->setProperty("class", cls);
}

void set_level_style(QLabel *label, int level)
{
	if (level == LOG_INFO)
		return;
	if (theme_has_text_classes())
		label->setProperty("class", level == LOG_ERROR ? "text-danger" : "text-warning");
	else
		label->setStyleSheet(level == LOG_ERROR ? "color: #e74c3c;" : "color: #f39c12;");
}

QLabel *make_caption(const std::string &text)
{
	QLabel *label = new QLabel(QString::fromUtf8(text.c_str()));
	label->setTextFormat(Qt::PlainText);
	set_text_class(label, "text-muted");
	return label;
}

QLabel *make_value(const std::string &text, bool wrap = false)
{
	QLabel *label = new QLabel(QString::fromUtf8(text.c_str()));
	label->setTextFormat(Qt::PlainText); // 服务端字符串不当富文本(可含 <img src=…> 外链)
	label->setWordWrap(wrap);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	label->setToolTip(QString::fromUtf8(text.c_str()));
	return label;
}

QLabel *make_heading(const std::string &text, int level)
{
	QLabel *label = make_value(text, true);
	label->setTextInteractionFlags(Qt::NoTextInteraction);
	set_level_style(label, level);
	return label;
}

// 长串(地址/推流码)用只读单行框:可选中、可横向滚动,不会被截断成省略号。
QLineEdit *make_readonly_line(const std::string &text)
{
	QLineEdit *edit = new QLineEdit(QString::fromUtf8(text.c_str()));
	edit->setReadOnly(true);
	edit->setToolTip(QString::fromUtf8(text.c_str()));
	return edit;
}

using ButtonFn = std::function<void()>;

QPushButton *make_button(const char *text_key, ButtonFn fn, bool enabled, const char *tooltip_key = nullptr)
{
	QPushButton *button = new QPushButton(QString::fromUtf8(Utils::Text(text_key).c_str()));
	button->setEnabled(enabled);
	button->setAutoDefault(false);
	if (tooltip_key && *tooltip_key)
		button->setToolTip(QString::fromUtf8(Utils::Text(tooltip_key).c_str()));
	QObject::connect(button, &QPushButton::clicked, button, [fn = std::move(fn)]() { fn(); });
	return button;
}

void add_row(QGridLayout *grid, int row, const char *name_key, QWidget *value)
{
	grid->addWidget(make_caption(Utils::Text(name_key)), row, 0, Qt::AlignTop);
	grid->addWidget(value, row, 1, Qt::AlignTop);
}

QPixmap qr_pixmap(const std::string &data, int target_px)
{
	try {
		const qrcodegen::QrCode qr =
			qrcodegen::QrCode::encodeText(data.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
		const int n = qr.getSize();
		const int border = 2;
		const qreal dpr =
			QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
		const int scale = std::max(1, static_cast<int>(target_px * dpr) / (n + 2 * border));
		const int size = (n + 2 * border) * scale;
		QImage image(size, size, QImage::Format_RGB32);
		image.fill(QColor(Qt::white));
		QPainter painter(&image);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(Qt::black));
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				if (qr.getModule(x, y))
					painter.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
			}
		}
		painter.end();
		QPixmap pixmap = QPixmap::fromImage(std::move(image));
		pixmap.setDevicePixelRatio(dpr);
		return pixmap;
	} catch (const std::exception &) {
		return {};
	}
}

// ---------------------------------------------------------------------------
// 复制 / 展示辅助
// ---------------------------------------------------------------------------

std::string format_time(std::time_t when)
{
	char buffer[32] = {0};
	std::tm *local = std::localtime(&when);
	if (!local || !std::strftime(buffer, sizeof(buffer), "%H:%M:%S", local))
		return {};
	return buffer;
}

void show_copy_hint(const std::string &text)
{
	if (!g_copy_hint || !g_copy_hint_timer)
		return;
	g_copy_hint->setText(QString::fromUtf8(text.c_str()));
	g_copy_hint_timer->start(kCopyHintMs);
}

void copy_to_clipboard(const std::string &text, const char *what_key)
{
	QGuiApplication::clipboard()->setText(QString::fromUtf8(text.c_str()));
	obs_log(LOG_INFO, "%s已复制到剪贴板", Utils::Text(what_key).c_str()); // 只记「复制了什么」,不记内容
	show_copy_hint(Utils::Text("Dock.Copied", Utils::Text(what_key).c_str()));
}

void open_room_page()
{
	const std::string url = "https://live.bilibili.com/" + session().config().room_id;
	if (!Core::openInBrowser(url))
		session().set_status(LOG_WARNING, Utils::Text("Core.BrowserFail", url.c_str()));
}

// 面板「看得见」的全部状态:周期对账刷新结果没变就不重建控件(否则每分钟打掉
// 用户正在选中的文本)。
std::string visible_signature()
{
	Core::Session &s = session();
	const Core::Session::Status &status = s.status();
	const Bili::Config &cfg = s.config();
	const Core::RoomState &room = s.room();
	const Core::LoginFlow &login = s.login_flow();
	std::string sig;
	sig += std::to_string(status.level);
	sig += status.text;
	sig += '\n';
	sig += "busy=" + std::to_string(static_cast<int>(s.busy_what()));
	sig += s.obs_streaming() ? " S" : " -";
	sig += s.logged_in() ? " in" : " out";
	sig += login.active ? " login{" + login.hint + "|" + login.qr + "}" : "";
	sig += " room=" + cfg.room_id + "|" + cfg.title + "|" + s.area_display();
	sig += room.known ? " known" : " unknown";
	sig += room.stale ? " stale" : " fresh";
	sig += " live=" + std::to_string(room.info.live_status);
	sig += room.info.locked ? " locked" : "";
	sig += room.info.title + room.info.area_name;
	sig += " at=" + std::to_string(static_cast<long long>(room.fetched_at));
	sig += room.error;
	sig += s.face_qr().empty() ? "" : " face";
	for (const Bili::StreamTarget &t : cfg.stream_targets)
		sig += "\nline=" + t.protocol + t.provider + t.addr + t.key + std::to_string(t.backup);
	return sig;
}

void schedule_rebuild()
{
	if (!g_root || !g_rebuild_timer)
		return;
	if (visible_signature() == g_rendered_signature)
		return;
	// 0ms 单发定时器:同一轮事件循环里的多次变化合并成一次重建,且重建绝不发生在
	// 某个控件的信号发射栈内。
	g_rebuild_timer->start(0);
}

// ---------------------------------------------------------------------------
// 各分区
// ---------------------------------------------------------------------------

// 二维码面板(登录 / 人脸验证共用):图 + 提示 + 「在浏览器打开」。
QWidget *build_qr_panel(QWidget *parent, const std::string &qr, const std::string &hint, ButtonFn open_external,
			ButtonFn extra, const char *extra_key)
{
	QWidget *panel = new QWidget(parent);
	QVBoxLayout *box = new QVBoxLayout(panel);
	box->setContentsMargins(0, 0, 0, 0);
	// 二维码原文还没到(刚发起登录/取码失败)时不画图:空串也是合法二维码内容,
	// 不设防会显示一张「什么都扫不出」的码。
	const QPixmap pixmap = qr.empty() ? QPixmap() : qr_pixmap(qr, 180);
	if (!pixmap.isNull()) {
		QLabel *image = new QLabel;
		image->setPixmap(pixmap);
		image->setAlignment(Qt::AlignCenter);
		box->addWidget(image, 0, Qt::AlignHCenter);
	}
	QLabel *label = make_value(hint, true);
	label->setTextInteractionFlags(Qt::NoTextInteraction);
	label->setAlignment(Qt::AlignHCenter);
	box->addWidget(label);
	QHBoxLayout *row = new QHBoxLayout;
	row->addWidget(make_button("Btn.OpenQrInBrowser", std::move(open_external), !qr.empty()));
	if (extra)
		row->addWidget(make_button(extra_key, std::move(extra), true));
	box->addLayout(row);
	return panel;
}

QWidget *build_login_panel(QWidget *parent)
{
	const Core::LoginFlow &flow = session().login_flow();
	return build_qr_panel(
		parent, flow.qr, flow.hint, [] { session().open_login_qr_externally(); },
		[] { session().cancel_login(); }, "Btn.CancelLogin");
}

// 开播时服务端要求人脸验证(60024 / need_face_auth):二维码在侧栏直接可扫。
QWidget *build_face_panel(QWidget *parent)
{
	const std::string qr = session().face_qr();
	return build_qr_panel(
		parent, qr, Utils::Text("Api.FaceAuthNeeded"), [qr] { Core::openInBrowser(qr); }, nullptr, nullptr);
}

QWidget *build_room_box(QWidget *parent)
{
	Core::Session &s = session();
	const Bili::Config &cfg = s.config();
	const Core::RoomState &room = s.room();
	QGroupBox *box = new QGroupBox(QString::fromUtf8(Utils::Text("Dock.Room").c_str()), parent);
	QGridLayout *grid = new QGridLayout(box);
	grid->setColumnStretch(1, 1);
	int row = 0;

	QWidget *cell = new QWidget;
	QHBoxLayout *line = new QHBoxLayout(cell);
	line->setContentsMargins(0, 0, 0, 0);
	line->addWidget(make_value(cfg.room_id), 1);
	line->addWidget(make_button("Btn.OpenRoom", [] { open_room_page(); }, true));
	add_row(grid, row++, "Dock.RoomId", cell);

	add_row(grid, row++, "Dock.TitleLabel", make_value(cfg.title, true));
	add_row(grid, row++, "Dock.Area", make_value(s.area_display()));

	{
		std::string state = Utils::Text("Common.Unknown");
		const char *cls = "text-muted";
		if (room.known) {
			switch (room.info.live_status) {
			case 1:
				state = Utils::Text("Live.IsLive");
				cls = "text-success";
				break;
			case 2:
				state = Utils::Text("Live.Rebroadcast");
				break;
			default:
				state = Utils::Text("Live.Offline");
				break;
			}
			if (room.info.locked)
				state += Utils::Text("Live.Locked");
		}
		QLabel *label = make_heading(state, LOG_INFO);
		set_text_class(label, cls);
		add_row(grid, row++, "Dock.State", label);
	}
	{
		QLabel *label = make_heading(
			s.obs_streaming() ? Utils::Text("Dock.Streaming") : Utils::Text("Dock.NotStreaming"), LOG_INFO);
		set_text_class(label, s.obs_streaming() ? "text-success" : "text-muted");
		add_row(grid, row++, "Dock.Obs", label);
	}
	{
		QLabel *label = make_value(room.fetched_at ? format_time(room.fetched_at) : std::string("-"));
		// 陈旧快照:原因放 tooltip,状态行另有 WARNING。
		label->setToolTip(QString::fromUtf8(
			room.stale ? Utils::Text("Status.RoomStale", room.error.c_str()).c_str() : ""));
		add_row(grid, row++, "Dock.Updated", label);
	}
	return box;
}

QWidget *build_lines_box(QWidget *parent)
{
	Core::Session &s = session();
	QGroupBox *box = new QGroupBox(QString::fromUtf8(Utils::Text("Dock.Lines").c_str()), parent);
	QVBoxLayout *boxLayout = new QVBoxLayout(box);
	const std::vector<Bili::StreamTarget> &targets = s.config().stream_targets;
	for (size_t i = 0; i < targets.size(); ++i) {
		const Bili::StreamTarget &t = targets[i];
		QGridLayout *grid = new QGridLayout;
		grid->setColumnStretch(1, 1);
		grid->addWidget(make_heading(s.line_label(i), LOG_INFO), 0, 0, 1, 3);
		grid->addWidget(make_caption(Utils::Text("Dock.Address")), 1, 0);
		grid->addWidget(make_readonly_line(t.addr), 1, 1);
		grid->addWidget(make_button(
					"Btn.CopyAddr", [addr = t.addr] { copy_to_clipboard(addr, "Dock.Address"); },
					true),
				1, 2);
		if (!t.key.empty()) {
			grid->addWidget(make_caption(Utils::Text("Dock.StreamKey")), 2, 0);
			grid->addWidget(make_readonly_line(t.key), 2, 1);
			grid->addWidget(make_button(
						"Btn.CopyKey",
						[key = t.key] { copy_to_clipboard(key, "Dock.StreamKey"); }, true),
					2, 2);
		}
		boxLayout->addLayout(grid);
	}
	g_copy_hint = new QLabel;
	set_text_class(g_copy_hint, "text-muted");
	boxLayout->addWidget(g_copy_hint);
	return box;
}

// 「关闭直播间」二次确认。确认框的 parent 是 OBS 的 dock(随 OBS 生存),不是会被
// 重建的内容控件——exec() 的嵌套事件循环里内容可能被重建,弹窗不能跟着被销毁。
void confirm_and_stop()
{
	Core::Session &s = session();
	QWidget *dock = g_root ? g_root->parentWidget() : nullptr;
	const QString title = QString::fromUtf8(Utils::Text("Btn.StopLive").c_str());
	const char *text_key = "Dock.ConfirmStopText";
	if (s.obs_streaming()) {
		text_key = "Stop.NeedStopStreamFirst";
	}
	QMessageBox box(
		QMessageBox::Warning, title,
		QString::fromUtf8(Utils::Text(text_key, s.config().room_id.c_str(), s.config().title.c_str()).c_str()),
		QMessageBox::NoButton, dock);
	box.setTextFormat(Qt::PlainText); // 房间标题来自服务端,不当富文本渲染
	if (s.obs_streaming()) {
		box.addButton(QString::fromUtf8(Utils::Text("Common.Ok").c_str()), QMessageBox::AcceptRole);
		box.exec();
		return;
	}
	QPushButton *yes = box.addButton(QString::fromUtf8(Utils::Text("Btn.StopLiveConfirm").c_str()),
					 QMessageBox::DestructiveRole);
	QPushButton *no = box.addButton(QString::fromUtf8(Utils::Text("Btn.Cancel").c_str()), QMessageBox::RejectRole);
	box.setDefaultButton(no); // 回车/Esc 默认都是「取消」:误触不会关掉正在直播的房间
	box.setEscapeButton(no);
	box.exec();
	if (box.clickedButton() == yes)
		s.stop_live();
}

QLayout *build_actions()
{
	Core::Session &s = session();
	const bool busy = s.busy();
	QVBoxLayout *box = new QVBoxLayout;
	box->setContentsMargins(0, 0, 0, 0);
	box->setSpacing(6);
	QHBoxLayout *row1 = new QHBoxLayout;
	QHBoxLayout *row2 = new QHBoxLayout;
	if (!s.logged_in()) {
		row1->addWidget(make_button("Btn.Login", [] { session().login(); }, !busy, "Tip.Login"));
	} else {
		row1->addWidget(make_button(
			"Btn.StartLive", [] { session().start_live(); }, !busy && s.can_start_live(), "Tip.StartLive"));
		row1->addWidget(make_button(
			"Btn.StopLive", [] { confirm_and_stop(); }, !busy && s.can_stop_live(), "Tip.StopLive"));
		row2->addWidget(make_button(
			"Btn.Recover", [] { session().recover(); }, !busy && !s.config().room_id.empty(),
			"Tip.Recover"));
	}
	row2->addWidget(make_button("Btn.Refresh", [] { session().refresh(false); }, !busy, "Tip.Refresh"));
	box->addLayout(row1);
	box->addLayout(row2);
	box->addWidget(make_button("Btn.Settings", [] { UI::ConsoleOpenPanel(); }, true, "Tip.Settings"));
	return box;
}

QWidget *build_content()
{
	Core::Session &s = session();
	QWidget *content = new QWidget(g_root);
	QVBoxLayout *box = new QVBoxLayout(content);
	box->setContentsMargins(8, 8, 8, 8);
	box->setSpacing(8);
	if (!s.status().text.empty())
		box->addWidget(make_heading(s.status().text, s.status().level));
	if (s.login_flow().active) {
		box->addWidget(build_login_panel(content));
	} else {
		if (!s.face_qr().empty())
			box->addWidget(build_face_panel(content));
		if (s.logged_in())
			box->addWidget(build_room_box(content));
		if (s.has_lines())
			box->addWidget(build_lines_box(content));
	}
	box->addLayout(build_actions());
	box->addStretch(1);
	return content;
}

void rebuild()
{
	if (!g_root)
		return;
	g_rendered_signature = visible_signature();
	g_copy_hint = nullptr;
	if (QLayout *layout = g_root->layout()) {
		while (QLayoutItem *item = layout->takeAt(0)) {
			// 旧内容延迟销毁:此刻可能还有某个控件的信号在调用栈上。
			if (QWidget *old = item->widget())
				old->deleteLater();
			delete item;
		}
		g_root->layout()->addWidget(build_content());
	}
}

struct DockListener : Core::Session::Listener {
	void session_changed() override { schedule_rebuild(); }
} g_listener;

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (!g_root)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// OBS 把新 dock 建成「隐藏 + 浮动」,首次加载自动停到右侧并显示一次;之后
		// 尊重用户自己的停靠布局(restoreState 在此之前已恢复)。
		if (!session().config().dock_introduced) {
			session().mark_dock_introduced();
			UI::DockShow();
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		// OBS 推流状态是侧栏展示的一部分。
		schedule_rebuild();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		// 停止推流后服务端会在数分钟内结束场次 → 立刻对账一次,再交给周期刷新。
		schedule_rebuild();
		session().tick();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// 主窗口仍在、前端回调仍有效:此刻摘 dock 最干净。
		UI::DockDestroy();
		break;
	default:
		break;
	}
}

} // namespace

namespace UI {

bool DockCreate()
{
	if (g_root)
		return true;
	const add_dock_fn add = resolve_frontend_symbol<add_dock_fn>("obs_frontend_add_dock_by_id");
	if (!add) {
		obs_log(LOG_WARNING, "本机 OBS 无 obs_frontend_add_dock_by_id(需 30.0.2+),侧栏不可用;功能全部在"
				     "「工具→B站直播控制台」");
		return false;
	}

	QWidget *root = new QWidget;
	root->setMinimumWidth(260);
	root->setLayout(new QVBoxLayout);
	root->layout()->setContentsMargins(0, 0, 0, 0);
	g_root = root;

	g_rebuild_timer = new QTimer(root);
	g_rebuild_timer->setSingleShot(true);
	QObject::connect(g_rebuild_timer, &QTimer::timeout, root, [] { rebuild(); });

	g_copy_hint_timer = new QTimer(root);
	g_copy_hint_timer->setSingleShot(true);
	QObject::connect(g_copy_hint_timer, &QTimer::timeout, root, [] {
		if (g_copy_hint)
			g_copy_hint->clear();
	});

	g_refresh_timer = new QTimer(root);
	g_refresh_timer->setInterval(kRefreshIntervalMs);
	QObject::connect(g_refresh_timer, &QTimer::timeout, root, [] { session().tick(); });

	rebuild();

	if (!add(kDockId, Utils::Text("Dock.Title").c_str(), root)) {
		// id 已被占用(重复库?):OBS 没有接管控件,自己回收。
		g_root = nullptr;
		g_rebuild_timer = g_refresh_timer = g_copy_hint_timer = nullptr;
		root->deleteLater();
		obs_log(LOG_ERROR, "侧栏 id「%s」已被占用,侧栏未创建", kDockId);
		return false;
	}
	// 自此 root 归 OBS 的 OBSDock 所有(OBS 32.2 OBSStudioAPI.cpp: setWidget + 右侧停靠区)。

	Core::Session::instance().add_listener(&g_listener);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	g_event_added = true;
	g_refresh_timer->start();
	obs_log(LOG_INFO, "侧栏已创建:Docks 菜单可开关,「直播间设置…」打开控制台");
	return true;
}

void DockDestroy()
{
	if (g_event_added) {
		obs_frontend_remove_event_callback(on_frontend_event, nullptr);
		g_event_added = false;
	}
	Core::Session::instance().remove_listener(&g_listener);
	QWidget *root = g_root;
	g_rebuild_timer = g_refresh_timer = g_copy_hint_timer = nullptr;
	g_copy_hint = nullptr;
	g_rendered_signature.clear();
	if (!root)
		return;
	const remove_dock_fn remove = resolve_frontend_symbol<remove_dock_fn>("obs_frontend_remove_dock");
	if (remove)
		remove(kDockId); // OBSDock 销毁 → 内容控件随之销毁
	// 兜底:dock API 已不可用(EXIT 未到就卸载)时,至少在模块代码被卸掉之前摘掉
	// 自己的内容控件(它是 QDockWidget 的子控件,单独删除是 Qt 支持的操作)。
	if (g_root)
		delete g_root.data();
}

bool DockShow()
{
	QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	QDockWidget *dock = main ? main->findChild<QDockWidget *>(QString::fromUtf8(kDockId)) : nullptr;
	if (!dock)
		return false;
	dock->setFloating(false);
	dock->show();
	dock->raise();
	return true;
}

} // namespace UI

#else // !BILI_ENABLE_DOCK

namespace UI {
// 未启用 Qt 侧栏(ENABLE_DOCK=OFF):插件退回「只有控制台」的形态。
bool DockCreate()
{
	return false;
}
void DockDestroy() {}
bool DockShow()
{
	return false;
}
} // namespace UI

#endif // BILI_ENABLE_DOCK
