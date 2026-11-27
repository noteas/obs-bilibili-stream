#include "ui/console.hpp"

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

#include "bilibili_api.hpp"
#include "core/platform_util.hpp"
#include "core/session.hpp"
#include "plugin_utils.hpp"

#include "srt/srt_catalog.hpp" // kServiceEntryName
#include "srt/srt_target.hpp"  // IsSrtLine

// All state below is touched from the OBS UI thread only. Runtime state lives
// in Core::Session; this file keeps nothing but the console source handle, the
// dialog drafts and the "what did we last render" signature.
namespace {

constexpr const char *CONSOLE_ID = "bilibili_stream_console";

enum Action : uintptr_t {
	ACT_LOGIN = 1,
	ACT_CANCEL_LOGIN,
	ACT_REFRESH,
	ACT_APPLY_TITLE,
	ACT_APPLY_PARTITION,
	ACT_START_LIVE,
	ACT_STOP_LIVE,
	ACT_RECOVER,
	ACT_FETCH_QUALITY,
	ACT_OPEN_ROOM,
};

enum StatusLevel {
	LEVEL_INFO = 0,
	LEVEL_WARNING,
	LEVEL_ERROR,
};

// The `data` handle for the console source: OBS 32 hands only this pointer to
// get_properties/destroy, so the source handle is kept inside it.
struct ConsoleCtx {
	obs_source_t *source; // borrowed reference; OBS owns the lifetime
};

// Settings keys of the properties dialog (defaults seeded from the session).
constexpr const char *KEY_TITLE = "room_title";
constexpr const char *KEY_PART = "room_part";
constexpr const char *KEY_AREA = "room_area";

bool g_alive = false;
obs_source_t *g_source = nullptr;

// 「关闭直播间」两段式确认:第一次点击置位并改口,再点才发 stopLive。
bool g_confirm_stop = false;
std::string g_confirm_text;

// 上次渲染的「面板可见内容」签名。后台对账刷新(周期 tick、动作后)结果没变时
// 不重建对话框——OBS 每次重建都销毁整棵控件树(闪烁、丢焦点,正在编辑的标题被
// 打回)。旧版每个动作固定重建 3 次、登录轮询每变一次文案再重建一次。
std::string g_rendered_signature;

// The values currently shown in the properties dialog. OBS 32 button callbacks
// cannot read the dialog's settings, so every editable property carries a
// modified callback that mirrors its value here the moment the user edits it;
// apply-buttons then submit this draft.
std::string g_draft_title;
int g_draft_part = 0;
int g_draft_area = 0;

Core::Session &session()
{
	return Core::Session::instance();
}

void refresh_props()
{
	if (g_alive && g_source)
		obs_source_update_properties(g_source);
}

// 面板「看得见」的全部状态:任一项变化才值得重建对话框。
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
	// 刻意不含 refreshing():后台对账刷新(周期 tick/动作后)每分钟两次,控制台并不展示
	// 「刷新中」,把它算进签名 = 对话框每分钟被重建两次(丢焦点、打掉正在输入的标题)。
	sig += s.obs_streaming() ? " S" : " -";
	sig += s.logged_in() ? " in" : " out";
	sig += login.active ? " login{" + login.hint + "|" + login.qr + "}" : "";
	sig += " room=" + cfg.room_id + "|" + cfg.title + "|" + std::to_string(cfg.part_id) + "," +
	       std::to_string(cfg.area_id);
	sig += room.known ? " known" : " unknown";
	sig += room.stale ? " stale" : " fresh";
	sig += " live=" + std::to_string(room.info.live_status);
	sig += room.info.locked ? " locked" : "";
	sig += room.info.title + room.info.area_name;
	sig += s.area_display();
	sig += g_confirm_stop ? " confirm" : "";
	for (const Bili::StreamTarget &t : cfg.stream_targets)
		sig += "\nline=" + t.protocol + t.provider + t.addr + t.key + std::to_string(t.backup);
	sig += s.partitions_loading() ? " loading" : "";
	sig += s.partitions().is_array() ? " parts" : " noparts";
	sig += "\nquality=" + s.quality_info();
	return sig;
}

struct ConsoleListener : Core::Session::Listener {
	void session_changed() override
	{
		if (!g_alive)
			return;
		// 确认态只跟着它自己那条提示活:状态行被别的结果覆盖(刷新/别的动作)即撤销。
		if (g_confirm_stop && session().status().text != g_confirm_text)
			g_confirm_stop = false;
		if (visible_signature() != g_rendered_signature)
			refresh_props();
	}
} g_listener;

// The Bilibili API sometimes returns area ids as JSON strings and sometimes as
// numbers; accept both shapes and never throw.
bool parse_area_id(const json11::Json &value, int &id)
{
	const std::string text = value.is_string() ? value.string_value() : value.dump();
	if (text.empty())
		return false;
	int parsed = 0;
	auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		return false;
	id = parsed;
	return true;
}

int part_of_parent(const json11::Json &parent)
{
	int id = 0;
	parse_area_id(parent["id"], id);
	return id;
}

const json11::Json *find_parent(const json11::Json &partitions, int partId);

void open_live_room()
{
	const std::string url = "https://live.bilibili.com/" + session().config().room_id;
	if (!Core::openInBrowser(url))
		session().set_status(LOG_WARNING, Utils::Text("Core.BrowserFail", url.c_str()));
}

// ---------------------------------------------------------------------------
// obs source plumbing (OBS renders everything; priv carries the action id)
// ---------------------------------------------------------------------------

// Mirrors the dialog's editable values into the draft so the apply-buttons can
// submit them (OBS 32 gives button callbacks no access to dialog settings).
void sync_draft(obs_data_t *settings)
{
	g_draft_title = obs_data_get_string(settings, KEY_TITLE);
	g_draft_part = static_cast<int>(obs_data_get_int(settings, KEY_PART));
	g_draft_area = static_cast<int>(obs_data_get_int(settings, KEY_AREA));
}

bool on_value_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
	return false;
}

bool on_part_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	sync_draft(settings);
	const json11::Json &partitions = session().partitions();
	if (!props || !partitions.is_array())
		return true;
	// OBS 32.2 机制(源码 + 两次 coredump 实证):modified 回调返回值只决定
	// 是否把 RefreshProperties 排队(Qt 队列,事件处理完后按**已缓存的**
	// properties 对象重建控件);回调内既不重跑 get_properties,也绝不能
	// 触发对话框刷新——update_properties 的 ReloadProperties 同线程直调,会
	// 同步销毁控件树/重放 modified 回调,把仍在发信号的 combo 和正在执行的
	// 回调自己拆掉(64386 无限递归爆栈、65402 Qt currentTextChanged 悬垂)。
	// 正确最简做法 = 用 libobs 公共 API 原地改写缓存 property 的条目
	// (obs_properties_get + list_clear + list_add_int),return true 让
	// 排队的 Refresh 按新条目重建;选中值取对话框 settings,需同步写回。
	obs_property_t *area_list = obs_properties_get(props, KEY_AREA);
	const json11::Json *parent = find_parent(partitions, g_draft_part);
	if (!area_list || !parent)
		return true;
	obs_property_list_clear(area_list);
	int first_area = 0;
	bool area_valid = false;
	for (const auto &area : (*parent)["list"].array_items()) {
		int id = 0;
		if (!parse_area_id(area["id"], id))
			continue;
		obs_property_list_add_int(area_list, area["name"].string_value().c_str(), id);
		if (first_area == 0)
			first_area = id;
		if (id == g_draft_area)
			area_valid = true;
	}
	// 现值仍指向旧大类的条目 → 自动切到新大类首个二级并写回对话框设置
	// (Refresh 重建 combo 时按 settings 选中)。
	if (!area_valid && first_area > 0) {
		obs_data_set_int(settings, KEY_AREA, first_area);
		g_draft_area = first_area;
	}
	return true; // queued RefreshProperties rebuilds the combo with new items
}

bool on_button(obs_properties_t *, obs_property_t *, void *data)
{
	const auto action = static_cast<Action>(reinterpret_cast<uintptr_t>(data));
	Core::Session &s = session();
	if (action != ACT_STOP_LIVE)
		g_confirm_stop = false; // 任何别的动作都撤销「关闭直播间」的确认态
	switch (action) {
	case ACT_LOGIN:
		s.login();
		break;
	case ACT_CANCEL_LOGIN:
		s.cancel_login();
		break;
	case ACT_REFRESH:
		s.refresh(false);
		break;
	case ACT_APPLY_TITLE:
		s.apply_title(g_draft_title);
		break;
	case ACT_APPLY_PARTITION:
		s.apply_area(g_draft_part, g_draft_area);
		break;
	case ACT_START_LIVE:
		s.start_live();
		break;
	case ACT_STOP_LIVE:
		// 两段式确认:真实停播信令,第一次只改口,第二次才发(AGENTS:开播类
		// 调用需用户在场并明确同意 —— 两次点击即在场同意)。OBS 仍在推流时
		// 直接交给 Session 拒绝并提示「先停止推流」,不进入确认态。
		if (!g_confirm_stop && !s.obs_streaming()) {
			g_confirm_stop = true;
			g_confirm_text = Utils::Text("Stop.Confirm");
			s.set_status(LOG_WARNING, g_confirm_text);
			refresh_props();
		} else {
			g_confirm_stop = false;
			s.stop_live();
		}
		break;
	case ACT_RECOVER:
		s.recover();
		break;
	case ACT_FETCH_QUALITY:
		s.fetch_quality();
		break;
	case ACT_OPEN_ROOM:
		open_live_room();
		break;
	}
	return false; // async results refresh through obs_source_update_properties()
}

obs_property_t *add_action_button(obs_properties_t *props, const char *name, const char *text, Action action,
				  bool enabled)
{
	obs_property_t *prop = obs_properties_add_button2(
		props, name, text, +[](obs_properties_t *p, obs_property_t *b, void *d) { return on_button(p, b, d); },
		reinterpret_cast<void *>(action));
	obs_property_set_enabled(prop, enabled);
	return prop;
}

obs_property_t *add_status_line(obs_properties_t *props, const char *name, const std::string &text, int level)
{
	obs_property_t *prop = obs_properties_add_text(props, name, text.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_type(prop, level == LEVEL_ERROR ? OBS_TEXT_INFO_ERROR
								   : (level == LEVEL_WARNING ? OBS_TEXT_INFO_WARNING
											     : OBS_TEXT_INFO_NORMAL));
	return prop;
}

const json11::Json *find_parent(const json11::Json &partitions, int partId)
{
	for (const auto &item : partitions.array_items()) {
		if (part_of_parent(item) == partId)
			return &item;
	}
	return nullptr;
}

obs_properties_t *console_properties(void *data)
{
	auto *ctx = static_cast<ConsoleCtx *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	Core::Session &s = session();
	const Bili::Config &cfg = s.config();
	const Core::RoomState &room = s.room();

	obs_data_set_default_string(settings, KEY_TITLE, cfg.title.c_str());
	obs_data_set_default_int(settings, KEY_PART, cfg.part_id);
	obs_data_set_default_int(settings, KEY_AREA, cfg.area_id);

	// --- 登录状态(统一入口:扫码登录/刷新/房间) ---
	std::string login_line = s.logged_in() ? Utils::Text("Status.LoggedIn") + " (mid=" + cfg.mid + ")"
					       : Utils::Text("Status.NotLoggedIn");
	int login_level = s.logged_in() ? LEVEL_INFO : LEVEL_WARNING;
	if (room.stale) {
		login_line += Utils::Text("Status.StaleSuffix", room.error.c_str());
		login_level = LEVEL_WARNING;
	}
	add_status_line(props, "st_login", login_line, login_level);

	// 扫码登录与登录态成对:未登录时是主入口,已登录时降级为换号动作。
	if (s.login_flow().active) {
		// 登录进行中:阶段提示(等待扫码/已扫码)与取消入口。二维码本体在侧栏里;
		// 没有侧栏的形态下 Session 已把它交系统浏览器打开。
		if (!s.login_flow().hint.empty())
			add_status_line(props, "st_login_hint", s.login_flow().hint, LEVEL_INFO);
		add_action_button(props, "btn_cancel_login", Utils::Text("Btn.CancelLogin").c_str(), ACT_CANCEL_LOGIN,
				  true);
	} else {
		add_action_button(props, "btn_login",
				  s.logged_in() ? Utils::Text("Btn.LoginSwitch").c_str()
						: Utils::Text("Btn.Login").c_str(),
				  ACT_LOGIN, !s.busy());
	}
	add_action_button(props, "btn_refresh", Utils::Text("Btn.Refresh").c_str(), ACT_REFRESH, !s.busy());

	if (s.logged_in() && !cfg.room_id.empty()) {
		std::string room_line = Utils::Text("Console.RoomLine", cfg.room_id.c_str(), cfg.title.c_str(),
						    s.area_display().c_str());
		if (room.known) {
			room_line += Utils::Text("Live.Session", room.info.live_status == 1 ? Utils::Text("Live.IsLive")
								 : room.info.live_status == 2
									 ? Utils::Text("Live.Rebroadcast")
									 : Utils::Text("Live.Offline"));
			if (room.info.locked)
				room_line += Utils::Text("Live.Locked");
		}
		obs_properties_add_text(props, "st_room", room_line.c_str(), OBS_TEXT_INFO);
		add_action_button(props, "btn_open_room", Utils::Text("Btn.OpenRoom").c_str(), ACT_OPEN_ROOM, true);
	}

	// --- 最近操作 ---
	if (!s.status().text.empty())
		add_status_line(props, "st_last", s.status().text, s.status().level);
	// 推流线路(ADR-0005):服务端给的每条线路只读展示,地址与推流码均可选中复制;
	// 插件不改 OBS 服务配置,用哪条由用户在 OBS「设置→直播」里自行填写。
	// 展示条件 = 有线路表(旧版还要本地 streaming 标志为真:该标志不持久化,
	// OBS 重启后明明存着线路却被藏起来)。
	for (size_t i = 0; i < cfg.stream_targets.size(); ++i) {
		const Bili::StreamTarget &t = cfg.stream_targets[i];
		const std::string label = s.line_label(i);
		const std::string suffix = std::to_string(i + 1);
		const std::string addr_id = "cp_addr_" + suffix;
		obs_properties_add_text(props, addr_id.c_str(), (label + Utils::Text("Srt.AddrSuffix")).c_str(),
					OBS_TEXT_DEFAULT);
		obs_data_set_default_string(settings, addr_id.c_str(), t.addr.c_str());
		if (!t.key.empty()) {
			const std::string key_id = "cp_key_" + suffix;
			obs_properties_add_text(props, key_id.c_str(), (label + Utils::Text("Srt.KeySuffix")).c_str(),
						OBS_TEXT_DEFAULT);
			obs_data_set_default_string(settings, key_id.c_str(), t.key.c_str());
		}
	}

	// --- 私有 SRT 输出(ADR-0007) ---
	// 服务条目由插件合并进 rtmp-services 的 services.json;用户仍在 OBS
	// 「设置→直播」自己选服务、自己粘贴推流码、自己点开始直播,插件不写用户配置。
	if (s.has_lines()) {
		bool has_srt = false;
		for (const Bili::StreamTarget &t : cfg.stream_targets)
			has_srt = has_srt || Srt::IsSrtLine(t.addr);
		const std::string hint = has_srt ? Utils::Text("Srt.HintWith", Srt::kServiceEntryName)
						 : Utils::Text("Srt.HintWithout");
		obs_properties_add_text(props, "st_srt_hint", hint.c_str(), OBS_TEXT_INFO);
	}

	// --- 直播间设置 ---
	obs_property_t *title =
		obs_properties_add_text(props, KEY_TITLE, Utils::Text("Prop.Title").c_str(), OBS_TEXT_DEFAULT);
	obs_property_set_enabled(title, !s.busy());
	obs_property_set_modified_callback(title, on_value_changed);
	add_action_button(props, "btn_apply_title", Utils::Text("Btn.ApplyTitle").c_str(), ACT_APPLY_TITLE,
			  !s.busy() && s.logged_in());

	obs_property_t *partList = obs_properties_add_list(props, KEY_PART, Utils::Text("Prop.Partition").c_str(),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_t *areaList = obs_properties_add_list(props, KEY_AREA, Utils::Text("Prop.Area").c_str(),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// 分区表登录后惰性加载。这里绝不能 force:属性表每次重建都会再进本函数,失败回调
	// 又会触发重建 ⇒ 「请求→重建→再请求」死循环(旧版的 P1)。重试只发生在
	// ConsoleOpenPanel()(用户重开窗口),每个窗口生命周期至多多试一次。
	s.ensure_partitions(false);
	const json11::Json &partitions = s.partitions();
	if (!partitions.is_array()) {
		const std::string placeholder = s.partitions_loading() ? Utils::Text("Prop.PartLoading")
								       : Utils::Text("Prop.PartNotLoaded");
		obs_property_list_add_int(partList, placeholder.c_str(), 0);
		obs_property_list_add_int(areaList, placeholder.c_str(), 0);
	} else {
		// 条目重建以草稿为准(= 对话框当前显示的大类):OBS 重建属性表时
		// get_properties 读到的是已提交设置(滞后于对话框编辑),草稿由
		// modified 回调实时同步,与 UpdateSettings 回填后的控件一致。
		const int wantedPart = g_draft_part > 0 ? g_draft_part
							: static_cast<int>(obs_data_get_int(settings, KEY_PART));
		int shownPart = cfg.part_id;
		for (const auto &item : partitions.array_items()) {
			const int id = part_of_parent(item);
			obs_property_list_add_int(partList, item["name"].string_value().c_str(), id);
			if (id == wantedPart)
				shownPart = id;
		}
		if (wantedPart <= 0)
			obs_data_set_int(settings, KEY_PART, shownPart);

		const json11::Json *parent = find_parent(partitions, shownPart);
		const int wantedArea = g_draft_area > 0 ? g_draft_area
							: static_cast<int>(obs_data_get_int(settings, KEY_AREA));
		int shownArea = cfg.area_id;
		if (parent) {
			for (const auto &area : (*parent)["list"].array_items()) {
				int id = 0;
				if (!parse_area_id(area["id"], id))
					continue;
				obs_property_list_add_int(areaList, area["name"].string_value().c_str(), id);
				if (id == wantedArea)
					shownArea = id;
			}
		}
		if (wantedArea <= 0)
			obs_data_set_int(settings, KEY_AREA, shownArea);
	}
	obs_property_set_modified_callback(partList, on_part_changed);
	obs_property_set_modified_callback(areaList, on_value_changed);
	obs_property_set_enabled(partList, !s.busy());
	obs_property_set_enabled(areaList, !s.busy());
	add_action_button(props, "btn_apply_partition", Utils::Text("Btn.ApplyPart").c_str(), ACT_APPLY_PARTITION,
			  !s.busy() && s.logged_in());

	// --- 质量协商(§5.4:官方口径=推荐/默认码率+干预,无档位/无回执) ---
	// 只读展示:插件不改写 OBS 配置,视频码率由用户在 OBS 编码器设置里自行配置(ADR-0005)。
	if (!s.quality_info().empty())
		obs_properties_add_text(props, "st_quality", s.quality_info().c_str(), OBS_TEXT_INFO);
	add_action_button(props, "btn_fetch_quality", Utils::Text("Btn.Quality").c_str(), ACT_FETCH_QUALITY,
			  !s.busy() && s.logged_in());

	// --- 开播 / 停播 ---
	// 「获取推流码」= 开播信令(用户口径:点了拿到推流地址与推流码);单按钮自动判定
	// 备流资格(E-22 官方同款分派)。
	add_action_button(props, "btn_start_live", Utils::Text("Btn.StartLive").c_str(), ACT_START_LIVE,
			  !s.busy() && s.can_start_live());
	// 「关闭直播间」= 停播信令:服务端在播或本地还留着本场线路时可点,两段式确认。
	add_action_button(props, "btn_stop_live",
			  g_confirm_stop ? Utils::Text("Btn.StopLiveConfirm").c_str()
					 : Utils::Text("Btn.StopLive").c_str(),
			  ACT_STOP_LIVE, !s.busy() && s.can_stop_live());
	// 「找回推流码」:进行中场次(OBS 重启后本地没有线路)取回推流地址/密钥,只展示。
	add_action_button(props, "btn_recover", Utils::Text("Btn.Recover").c_str(), ACT_RECOVER,
			  !s.busy() && s.logged_in() && !cfg.room_id.empty());

	g_rendered_signature = visible_signature();
	obs_data_release(settings);
	return props;
}

obs_source_info g_console_info = {};

} // namespace

namespace UI {

obs_source_info *ConsoleSourceInfo()
{
	if (g_console_info.id == nullptr) {
		g_console_info.id = CONSOLE_ID;
		g_console_info.type = OBS_SOURCE_TYPE_INPUT;
		// 从不渲染、不进场景:CAP_DISABLED 让 OBS「添加来源」列表不列出它
		// (旧版 output_flags=0,用户能把它拖进场景,第二实例的面板永远不刷新)。
		g_console_info.output_flags = OBS_SOURCE_CAP_DISABLED;
		g_console_info.get_name = [](void *) -> const char * {
			return obs_module_text("Console.Name");
		};
		g_console_info.create = [](obs_data_t *, obs_source_t *source) -> void * {
			// OBS 32 property callbacks only receive the source's `data`
			// pointer, so the source handle itself is stashed here.
			return new ConsoleCtx{source};
		};
		g_console_info.destroy = [](void *data) {
			delete static_cast<ConsoleCtx *>(data);
		};
		g_console_info.get_properties = console_properties;
	}
	return &g_console_info;
}

void ConsoleCreate()
{
	g_alive = true;
	g_source = obs_source_create_private(CONSOLE_ID, obs_module_text("Console.Name"), nullptr);
	g_draft_title = session().config().title;
	g_draft_part = session().config().part_id;
	g_draft_area = session().config().area_id;
	session().add_listener(&g_listener);
}

void ConsoleDestroy()
{
	session().remove_listener(&g_listener);
	g_alive = false;
	if (g_source) {
		obs_source_release(g_source);
		g_source = nullptr;
	}
}

void ConsoleOpenPanel()
{
	if (!g_source)
		return;
	Core::Session &s = session();
	// OBS 的 rtmp-services 会在启动时联网更新 services.json 并整表覆盖;用户打开
	// 控制台时再合并一次,保证私有 SRT 条目在 OBS 设置里可用(本次没有 rtmpsrt://
	// 线路时写入默认入口)。
	s.refresh_srt_catalog();
	// 分区表没加载成功过(或从没试过)就在打开时补一次;已成功的不重复拉。
	s.ensure_partitions(!s.partitions().is_array());
	// 草稿与已提交设置对齐(上次 OK/取消/异步刷新遗留的草稿不污染本次对话框:
	// 二级列表条目与下拉选中都以草稿为锚,见 on_part_changed)。无已提交值时回
	// 落到服务端镜像(= 属性表注入的默认值)。
	obs_data_t *settings = obs_source_get_settings(g_source);
	g_draft_title = obs_data_has_user_value(settings, KEY_TITLE) ? obs_data_get_string(settings, KEY_TITLE)
								     : s.config().title;
	g_draft_part = obs_data_has_user_value(settings, KEY_PART)
			       ? static_cast<int>(obs_data_get_int(settings, KEY_PART))
			       : s.config().part_id;
	g_draft_area = obs_data_has_user_value(settings, KEY_AREA)
			       ? static_cast<int>(obs_data_get_int(settings, KEY_AREA))
			       : s.config().area_id;
	obs_data_release(settings);
	// 房间态对账:打开配置界面时刷一次(手动,非静默),用户一眼看到的是服务端现状。
	if (!s.busy() && !s.refreshing() && s.logged_in())
		s.refresh(true);
	obs_frontend_open_source_properties(g_source);
}

} // namespace UI
