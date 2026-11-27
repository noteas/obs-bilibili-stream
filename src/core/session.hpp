#pragma once
// 会话控制器:插件全部运行期状态的唯一所有者(UI 线程)。控制台(obs_properties)与
// 侧栏(Qt dock)都只是它的视图——读 Session 的快照渲染、把按钮点击转成 Session 的
// 动作、通过 Listener::session_changed() 得到重绘通知。
//
// 状态口径(GLOSSARY):开播/停播 = 服务端信令;「是否在播」以服务端 room/GetInfo 的
// live_status 为准(本地标志只做乐观更新,下一次刷新即被服务端覆盖);推流归 OBS。
// 线程:公开方法一律在 OBS UI 线程调用;HTTP 在 worker 线程,结果经
// Http::runAsync 的 UI 续体回到这里(模块卸载后续体被丢弃或被 alive_ 拦下)。
#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <obs.h>

#include "bilibili_api.hpp"
#include "bili_parsers.hpp"
#include "core/config_manager.hpp"
#include "json11/json11.hpp"

namespace Core {

// 服务端房间态快照(§3.3 room/GetInfo),由 Session::refresh() 更新。
struct RoomState {
	Bili::Parse::RoomInfo info; // 最近一次成功快照
	bool known = false;         // 至少成功取到过一次
	bool stale = false;         // 最近一次刷新失败,当前显示的是上次结果
	std::time_t fetched_at = 0; // 上次成功快照的墙钟时间
	std::string error;          // 最近一次失败原因(stale 时有效)
};

// 扫码登录流程的可观察状态(侧栏据此渲染二维码与阶段提示)。
struct LoginFlow {
	bool active = false;
	std::string qr;   // 二维码原文(空 = 尚未取到)
	std::string hint; // 当前阶段提示(等待扫码 / 已扫码 / 已换新…)
};

class Session {
public:
	// 监听者回调在 OBS UI 线程;监听者必须在自身销毁前 remove_listener()。
	class Listener {
	public:
		virtual void session_changed() = 0;

	protected:
		~Listener() = default;
	};

	// 独占动作(同一时刻只有一个;刷新不算独占,见 refreshing())。
	enum class Busy { None, Login, Start, Stop, Recover, Quality, Title, Area };
	struct Status {
		int level = LOG_INFO;
		std::string text;
	};

	static Session &instance();

	// 模块加载(UI 线程):读配置、灌 Cookie、探测 buvid、已有登录态则刷新。
	void start();
	// 模块卸载:拦下所有在途 UI 续体,断开 Cookie 回调。
	void stop();

	void add_listener(Listener *listener);
	void remove_listener(Listener *listener);

	// OBS 推流状态探针(由 plugin-main 注入 obs_frontend_streaming_active,
	// 本类不直接依赖 obs-frontend-api)。
	void set_obs_streaming_probe(std::function<bool()> probe);
	// 无侧栏(或侧栏不可用)时,登录二维码只能交系统浏览器展示。
	void set_login_qr_external(bool external);

	// --- 查询(全部只读,UI 线程) ---
	const Bili::Config &config() const { return m_config.config(); }
	const RoomState &room() const { return m_room; }
	const LoginFlow &login_flow() const { return m_login; }
	const Status &status() const { return m_status; }
	const std::string &quality_info() const { return m_quality; }
	// 开播时服务端要求的人脸验证二维码(60024 / need_face_auth);空 = 无待处理认证。
	const std::string &face_qr() const { return m_face_qr; }

	bool alive() const { return m_alive; }
	bool logged_in() const { return m_logged_in; }
	bool busy() const { return m_busy != Busy::None; }
	Busy busy_what() const { return m_busy; }
	bool refreshing() const { return m_refreshing; }
	bool server_live() const { return m_room.known && m_room.info.live_status == 1; }
	bool has_lines() const { return !m_config.config().stream_targets.empty(); }
	bool obs_streaming() const { return m_obs_streaming && m_obs_streaming(); }
	// 「获取推流码」可用:已登录、知道房间、服务端未在播、无独占动作。
	bool can_start_live() const;
	// 「关闭直播间」可用:服务端认为在播或本地还留着本场线路。
	bool can_stop_live() const;

	// 分区表(控制台用):登录后惰性加载;force = 重试上次的失败。
	const json11::Json &partitions() const { return m_partitions; }
	bool partitions_loading() const { return m_partitions_loading; }
	bool partitions_tried() const { return m_partitions_tried; }
	void ensure_partitions(bool force);
	// 「大类名 / 二级名」展示文本(查不到名字时回落 id)。
	std::string area_display() const;

	// --- 动作(立即返回;结果经 session_changed 通知) ---
	void login();         // 扫码登录(二维码过期自动换新,直到成功/取消/卸载)
	void cancel_login();
	void open_login_qr_externally(); // 在系统浏览器打开当前登录二维码
	void refresh(bool quiet);        // quiet = 成功不改状态行(周期/动作后的对账刷新)
	void tick();                     // 周期调用:条件满足时做一次 quiet 刷新
	void apply_title(const std::string &title);
	void apply_area(int part_id, int area_id);
	void start_live();  // 「获取推流码」
	void stop_live();   // 「关闭直播间」:调用方负责二次确认
	void recover();     // 「找回推流码」:进行中场次取回推流地址/密钥
	void fetch_quality();
	// UI 可复用状态行做轻提示(如「已复制」)。
	void set_status(int level, const std::string &text);
	void save_config() { m_config.save(); }
	// 侧栏首次加载自动显示只做一次(ADR-0010)。
	void mark_dock_introduced()
	{
		m_config.config().dock_introduced = true;
		m_config.save();
	}
	// 线路展示名:「主线路N / 备流N · 协议/提供方」(主备各自编号,控制台/侧栏/OBS
	// 服务列表入口名同源)。index 越界返回空串。
	std::string line_label(size_t index) const;
	// 把当前场次的 rtmpsrt:// 线路合并进 OBS 服务目录(ADR-0007,无条件写);
	// 返回是否写入了真实线路。
	bool refresh_srt_catalog();

private:
	Session() = default;
	void notify();
	bool begin(Busy what, const char *what_text);
	void end();
	template<typename F> static std::function<void()> ui_guarded(F &&fn);
	void load_partitions();
	std::string busy_name() const;

	ConfigManager m_config;
	std::vector<Listener *> m_listeners;
	std::function<bool()> m_obs_streaming;
	std::shared_ptr<std::atomic<bool>> m_login_cancel; // 取消登录轮询的标志(worker 线程读)
	bool m_login_qr_external = true;

	bool m_alive = false;
	bool m_logged_in = false;
	Busy m_busy = Busy::None;
	bool m_refreshing = false;
	uint64_t m_lines_obtained_ms = 0; // 最近一次拿到本场线路的单调时间(线路作废判定的宽限期起点)
	Status m_status;
	RoomState m_room;
	LoginFlow m_login;
	std::string m_face_qr;
	std::string m_quality;
	json11::Json m_partitions;
	bool m_partitions_loading = false;
	bool m_partitions_tried = false;
};

} // namespace Core
