#include "core/session.hpp"

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "core/platform_util.hpp"
#include "core/qr_generator.hpp"
#include "http_client.hpp"
#include "plugin_utils.hpp"
#include "srt/srt_catalog.hpp"
#include "srt/srt_target.hpp"

namespace Core {
namespace {

constexpr int kPollIntervalMs = 2000;  // 官方 H5 固定 2000ms(契约 §2.2)
constexpr int kPollAttemptsPerQr = 90; // 一张二维码 ≈ 3 分钟,之后必然 86038 ⇒ 换新

std::string module_file(const char *name)
{
	char *path = obs_module_config_path(name);
	if (!path)
		return {};
	std::string result = path;
	bfree(path);
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(result).parent_path(), ec);
	return result;
}

// B站 JSON 的 id 类字段 number/string 两种形状都出现过,一律按整数解析。
bool parse_area_id(const json11::Json &value, int &id)
{
	std::string text = value.is_string() ? value.string_value() : value.dump();
	if (text.empty())
		return false;
	// json11 的 dump() 对整数输出不带小数点。
	int parsed = 0;
	char *end = nullptr;
	const long value_long = std::strtol(text.c_str(), &end, 10);
	if (!end || *end != '\0' || value_long <= 0 || value_long > 0x7fffffff)
		return false;
	parsed = static_cast<int>(value_long);
	id = parsed;
	return true;
}

} // namespace

Session &Session::instance()
{
	static Session session;
	return session;
}

template<typename F> std::function<void()> Session::ui_guarded(F &&fn)
{
	return [fn = std::forward<F>(fn)]() {
		if (instance().m_alive)
			fn();
	};
}

void Session::notify()
{
	// 监听者可能在回调里注销自己:按副本遍历。
	const std::vector<Listener *> listeners = m_listeners;
	for (Listener *listener : listeners)
		listener->session_changed();
}

void Session::add_listener(Listener *listener)
{
	if (!listener)
		return;
	for (const Listener *existing : m_listeners)
		if (existing == listener)
			return;
	m_listeners.push_back(listener);
}

void Session::remove_listener(Listener *listener)
{
	for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it) {
		if (*it == listener) {
			m_listeners.erase(it);
			return;
		}
	}
}

void Session::set_obs_streaming_probe(std::function<bool()> probe)
{
	m_obs_streaming = std::move(probe);
}

void Session::set_login_qr_external(bool external)
{
	m_login_qr_external = external;
}

std::string Session::busy_name() const
{
	switch (m_busy) {
	case Busy::Login:
		return Utils::Text("Action.Login");
	case Busy::Start:
		return Utils::Text("Action.StartLive");
	case Busy::Stop:
		return Utils::Text("Action.StopLive");
	case Busy::Recover:
		return Utils::Text("Action.Recover");
	case Busy::Quality:
		return Utils::Text("Action.Quality");
	case Busy::Title:
		return Utils::Text("Action.ApplyTitle");
	case Busy::Area:
		return Utils::Text("Action.ApplyPart");
	case Busy::None:
		break;
	}
	return {};
}

bool Session::can_start_live() const
{
	return m_logged_in && !m_config.config().room_id.empty() && !server_live();
}

bool Session::can_stop_live() const
{
	return (server_live() || has_lines()) && !m_config.config().room_id.empty();
}

void Session::set_status(int level, const std::string &text)
{
	m_status.level = level;
	m_status.text = text;
}

bool Session::begin(Busy what, const char *what_text)
{
	if (m_busy != Busy::None) {
		set_status(LOG_WARNING, Utils::Text("Console.Busy", what_text ? what_text : busy_name().c_str()));
		notify();
		return false;
	}
	m_busy = what;
	notify();
	return true;
}

void Session::end()
{
	m_busy = Busy::None;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void Session::start()
{
	m_alive = true;
	m_config.load();

	// Session identity boot: config.json carries the persisted cookie
	// whitelist into the jar; server-side rotations of those keys come back
	// through the change callback (worker thread → UI thread → config.json).
	Http::HttpClient::load_session_cookies(m_config.config().cookies);

	// 持久化镜像给出「临时」登录态与房间态:上次的房间号/标题/分区先显示(标陈旧),第一次
	// nav 的结果再定性。OBS 刚启动时网络未就绪(超时/-412)不至于把整个界面锁成「未登录」
	// ——旧版正是这样:开机第一次刷新失败,开播/分区全部变灰,只能手动再点刷新。
	if (Http::HttpClient::has_cookie("SESSDATA") && !m_config.config().room_id.empty()) {
		const Bili::Config &cfg = m_config.config();
		m_logged_in = true;
		m_room.info.room_id = cfg.room_id;
		m_room.info.title = cfg.title;
		m_room.info.parent_id = cfg.part_id;
		m_room.info.area_id = cfg.area_id;
		m_room.known = true;
		m_room.stale = true;
		m_room.error = Utils::Text("Status.Bootstrapped");
	}
	Http::HttpClient::set_cookie_change_callback([](const std::string &persisted) {
		Http::HttpClient::postToUi([persisted]() {
			auto &session = instance();
			if (!session.m_alive || session.m_config.config().cookies == persisted)
				return;
			session.m_config.config().cookies = persisted;
			session.m_config.save();
		});
	});

	// 指纹快照:固定 LiveHime UA + buvid3 来源(持久化配置 vs 新取 finger/spi)。
	Http::HttpClient::runAsync(
		[]() {
			const bool from_config = Http::HttpClient::has_cookie("buvid3");
			if (!from_config)
				Bili::BiliApi::fetch_buvid();
			// buvid3 是设备标识,日志里只留头尾便于对账。
			const std::string b3 = Http::HttpClient::cookie_value("buvid3");
			const std::string masked = b3.size() > 8 ? b3.substr(0, 4) + "..." + b3.substr(b3.size() - 4)
								 : (b3.empty() ? "获取失败" : "...");
			obs_log(LOG_INFO, "指纹: UA=%s | traceid=%s | buvid3=%s (%s)", Http::HttpClient::user_agent(),
				Http::HttpClient::traceid().c_str(), masked.c_str(), from_config ? "来自配置" : "新取");
		},
		nullptr);

	if (Http::HttpClient::has_cookie("SESSDATA"))
		refresh(false);
	else
		set_status(LOG_INFO, Utils::Text("Console.NotLoggedInHint"));
}

void Session::stop()
{
	m_alive = false;
	// Drop the persistence hook so a late worker-side merge cannot call into
	// a destroyed session.
	Http::HttpClient::set_cookie_change_callback(nullptr);
}

// ---------------------------------------------------------------------------
// 刷新:登录态 + 房间态(nav → room/GetInfo,共两个请求)
// ---------------------------------------------------------------------------

void Session::refresh(bool quiet)
{
	if (!m_alive)
		return;
	if (m_busy == Busy::Login)
		return; // 登录流程独占 worker 线程,结束时自带一次刷新
	if (m_refreshing)
		return; // 合并重复请求(周期 tick 与手动刷新撞车时只跑一次)
	if (m_busy != Busy::None) {
		if (!quiet) {
			set_status(LOG_WARNING, Utils::Text("Console.Busy", busy_name().c_str()));
			notify();
		}
		return;
	}
	if (!Http::HttpClient::has_cookie("SESSDATA")) {
		m_logged_in = false;
		if (!quiet)
			set_status(LOG_INFO, Utils::Text("Status.NotLoggedIn"));
		notify();
		return;
	}

	struct RefreshResult {
		Bili::Parse::LoginState login = Bili::Parse::LoginState::Unknown;
		bool room_ok = false;
		std::string message;
		std::string mid;
		Bili::Parse::RoomInfo room;
	};
	auto res = std::make_shared<RefreshResult>();
	m_refreshing = true;
	if (!quiet)
		set_status(LOG_INFO, Utils::Text("Status.Refreshing"));
	notify();

	Http::HttpClient::runAsync(
		[res]() {
			// 旧版每次刷新都跑 nav + sso/list + N 个 SSO 域 + room/GetInfo ×2
			// (≥ 9 个串行请求)。SSO 传播只在扫码登录成功后做一次;room/GetInfo
			// 本身就回读 room_id,不必再调一遍。刷新 = 2 个请求。
			res->login = Bili::BiliApi::checkLoginStatus(res->message, res->mid);
			if (res->login != Bili::Parse::LoginState::LoggedIn)
				return;
			const json11::Json data = Bili::BiliApi::getRoomInfoByUid(res->mid, res->message);
			res->room_ok = data.is_object() && Bili::Parse::ParseRoomInfo(data, res->room);
		},
		ui_guarded([res, quiet]() {
			auto &session = instance();
			session.m_refreshing = false;
			Bili::Config &cfg = session.m_config.config();
			switch (res->login) {
			case Bili::Parse::LoginState::LoggedIn: {
				session.m_logged_in = true;
				cfg.mid = res->mid;
				if (res->room_ok) {
					// 服务端是唯一权威:房间/标题/分区镜像全部以本次响应为准。
					session.m_room.info = res->room;
					session.m_room.known = true;
					session.m_room.stale = false;
					session.m_room.error.clear();
					session.m_room.fetched_at = std::time(nullptr);
					cfg.room_id = res->room.room_id;
					cfg.title = res->room.title;
					cfg.csrf_token = Http::HttpClient::cookie_value("bili_jct");
					if (res->room.area_id > 0) {
						cfg.part_id = res->room.parent_id;
						cfg.area_id = res->room.area_id;
					}
					// 线路表随场次作废(ADR-0005:密钥每场新生成):服务端说未开播,本地还留着
					// 上一场的线路就清掉——否则 OBS 重启后界面一直挂着失效的推流码。宽限期
					// 30s:刚拿到线路时服务端的 live_status 可能还没翻到 1(最终一致)。
					// m_lines_obtained_ms == 0 = 线路是上次运行留下的(从磁盘读回),没有宽限。
					constexpr uint64_t kLinesGraceMs = 30000;
					const uint64_t age_ms =
						os_gettime_ns() / 1000000ULL - session.m_lines_obtained_ms;
					if (res->room.live_status == 0 && !cfg.stream_targets.empty() &&
					    (session.m_lines_obtained_ms == 0 || age_ms > kLinesGraceMs)) {
						obs_log(LOG_INFO, "服务端未在播,清除上一场次的 %zu 条线路",
							cfg.stream_targets.size());
						cfg.stream_targets.clear();
					}
				} else {
					// 房间态取不到只降级(保留上次快照并标记陈旧),不推翻登录态。
					session.m_room.stale = true;
					session.m_room.error = res->message;
				}
				session.m_config.save();
				if (!quiet || session.m_room.stale) {
					if (session.m_room.stale)
						session.set_status(LOG_WARNING, Utils::Text("Status.RoomStale",
											    res->message.c_str()));
					else
						session.set_status(LOG_INFO, Utils::Text("Status.Refreshed"));
				}
				break;
			}
			case Bili::Parse::LoginState::LoggedOut:
				// 服务端明确未登录(-101):登录态作废,保留房间镜像供下次登录后对照;
				// 本地线路表一并作废(换号登录后不能还挂着上个账号的推流码)。
				session.m_logged_in = false;
				session.m_face_qr.clear();
				if (!cfg.stream_targets.empty()) {
					cfg.stream_targets.clear();
					session.m_config.save();
				}
				session.set_status(LOG_ERROR, Utils::Text("Status.LoginInvalid", res->message.c_str()));
				break;
			case Bili::Parse::LoginState::Unknown:
				// 网络/风控/限频:结果未知,保持原登录态,只提示(旧版把它一律显示成
				// 「未登录/已失效」,是「状态乱跳」的直接原因之一)。
				session.m_room.stale = true;
				session.m_room.error = res->message;
				session.set_status(LOG_WARNING, Utils::Text("Status.CheckFail", res->message.c_str()));
				break;
			}
			session.notify();
		}));
}

void Session::tick()
{
	if (!m_alive || !m_logged_in || m_busy != Busy::None || m_refreshing || m_login.active)
		return;
	refresh(true);
}

// ---------------------------------------------------------------------------
// 扫码登录
// ---------------------------------------------------------------------------

void Session::open_login_qr_externally()
{
	if (m_login.qr.empty())
		return;
	// 控制台面板渲染不了图片(ADR-0003):二维码写成 SVG 交系统浏览器。
	const std::string svgPath = module_file("login_qr.svg");
	const bool shown =
		(!svgPath.empty() && QrGenerator::writeSvgFile(svgPath, m_login.qr) && openInBrowser(svgPath)) ||
		openInBrowser(m_login.qr);
	set_status(shown ? LOG_INFO : LOG_ERROR,
		   shown ? Utils::Text("Login.QrOpened") : Utils::Text("Core.BrowserFail", m_login.qr.c_str()));
	notify();
}

void Session::login()
{
	if (!m_alive || m_login.active)
		return;
	if (!begin(Busy::Login, nullptr))
		return;

	struct LoginResult {
		std::atomic<bool> cancel{false};
		bool ok = false;
		bool cancelled = false;
		std::string message;
	};
	auto res = std::make_shared<LoginResult>();
	m_login_cancel = std::shared_ptr<std::atomic<bool>>(res, &res->cancel); // 别名指针
	m_login.active = true;
	m_login.qr.clear();
	m_login.hint = Utils::Text("Login.Starting");
	m_face_qr.clear();
	const bool external = m_login_qr_external;
	notify();

	Http::HttpClient::runAsync(
		[res, external]() {
			// 阶段提示/新二维码一律投递回 UI 线程再改 Session。
			auto post = [res, external](const std::string &qr, const std::string &hint) {
				Http::HttpClient::postToUi([res, external, qr, hint]() {
					auto &session = instance();
					if (!session.m_alive || !session.m_login.active)
						return;
					session.m_login.qr = qr;
					session.m_login.hint = hint;
					session.notify();
					if (!qr.empty() && external)
						session.open_login_qr_externally();
				});
			};
			std::string last_hint;
			while (!res->cancel) {
				std::string qrData, qrKey, msg;
				if (!Bili::BiliApi::getQrCode(qrData, qrKey, msg)) {
					res->message = Utils::Text("Login.QrFetchFail", msg.c_str());
					return;
				}
				last_hint.clear();
				post(qrData, Utils::Text("Login.WaitScan"));
				for (int attempt = 0; attempt < kPollAttemptsPerQr; ++attempt) {
					if (!Http::HttpClient::sleep_unless_stopping(kPollIntervalMs)) {
						res->cancelled = true;
						return;
					}
					if (res->cancel) {
						res->cancelled = true;
						return;
					}
					std::string pollMsg;
					switch (Bili::BiliApi::qrLogin(qrKey, pollMsg)) {
					case Bili::QrPoll::Success: {
						// B3: 登录成功后刷新全域 Cookie(sso/list);失败不影响登录态。
						std::string ssoMessage;
						Bili::BiliApi::propagateSso(ssoMessage);
						res->ok = true;
						return;
					}
					case Bili::QrPoll::NotScanned:
						continue;
					case Bili::QrPoll::Scanned:
						if (last_hint != pollMsg) {
							last_hint = pollMsg;
							post(qrData, pollMsg);
						}
						continue;
					case Bili::QrPoll::Failed:
						res->message = pollMsg;
						return;
					case Bili::QrPoll::Expired:
						break;
					}
					break; // Expired ⇒ 换码后重新开始
				}
				post(std::string(), Utils::Text("Login.ExpiredRefreshing"));
			}
			res->cancelled = true;
		},
		ui_guarded([res]() {
			auto &session = instance();
			session.end();
			session.m_login.active = false;
			session.m_login.qr.clear();
			session.m_login_cancel.reset();
			if (res->ok) {
				session.set_status(LOG_INFO, Utils::Text("Login.Success"));
				session.notify();
				session.refresh(false);
				return;
			}
			if (res->cancelled)
				session.set_status(LOG_WARNING, Utils::Text("Login.Cancelled"));
			else
				session.set_status(LOG_ERROR, res->message);
			session.notify();
		}));
}

void Session::cancel_login()
{
	if (m_login_cancel)
		*m_login_cancel = true;
}

// ---------------------------------------------------------------------------
// 直播间设置:标题 / 分区
// ---------------------------------------------------------------------------

void Session::apply_title(const std::string &title)
{
	if (title.empty()) {
		set_status(LOG_WARNING, Utils::Text("Title.Empty"));
		notify();
		return;
	}
	if (!begin(Busy::Title, nullptr))
		return;
	const Bili::Config cfg = m_config.config();
	struct TitleResult {
		bool ok = false;
		std::string message;
	};
	auto res = std::make_shared<TitleResult>();
	Http::HttpClient::runAsync(
		[cfg, title, res]() { res->ok = Bili::BiliApi::updatePreLiveInfo(cfg, title, res->message); },
		ui_guarded([res, title]() {
			auto &session = instance();
			session.end();
			if (res->ok) {
				session.m_config.config().title = title;
				session.m_room.info.title = title;
				session.m_config.save();
				session.set_status(LOG_INFO, Utils::Text("Title.Done"));
			} else {
				session.set_status(LOG_ERROR, Utils::Text("Title.Fail", res->message.c_str()));
			}
			session.notify();
		}));
}

void Session::apply_area(int part_id, int area_id)
{
	if (area_id <= 0) {
		set_status(LOG_WARNING, Utils::Text("Part.NeedArea"));
		notify();
		return;
	}
	if (!begin(Busy::Area, nullptr))
		return;
	const Bili::Config cfg = m_config.config();
	struct AreaResult {
		bool ok = false;
		std::string message;
	};
	auto res = std::make_shared<AreaResult>();
	Http::HttpClient::runAsync(
		[cfg, area_id, res]() { res->ok = Bili::BiliApi::changeRoomArea(cfg, area_id, res->message); },
		ui_guarded([res, part_id, area_id]() {
			auto &session = instance();
			session.end();
			if (res->ok) {
				session.m_config.config().part_id = part_id;
				session.m_config.config().area_id = area_id;
				session.m_config.save();
				session.set_status(LOG_INFO, Utils::Text("Part.Done"));
			} else {
				session.set_status(LOG_ERROR, Utils::Text("Part.Fail", res->message.c_str()));
			}
			session.notify();
			session.refresh(true); // 分区名以服务端回读为准
		}));
}

// ---------------------------------------------------------------------------
// 开播(「获取推流码」)/ 停播(「关闭直播间」)/ 找回
// ---------------------------------------------------------------------------

void Session::start_live()
{
	if (!can_start_live()) {
		set_status(LOG_WARNING, Utils::Text("Start.NotReady"));
		notify();
		return;
	}
	if (!begin(Busy::Start, nullptr))
		return;
	Bili::Config cfg = m_config.config();
	cfg.stream_targets.clear();
	const std::string mid = cfg.mid;
	struct StartResult {
		Bili::Parse::StartOutcome outcome = Bili::Parse::StartOutcome::MissingFields;
		Bili::Config cfg;
		std::string message;
		std::string face_qr;
		std::string face_page;
	};
	auto res = std::make_shared<StartResult>();
	res->cfg = cfg;
	Http::HttpClient::runAsync(
		[res, mid]() {
			// 开播前置(官方序): PreCheck 合规拦截(§3.5)→ 播前状态观测(§3.6,
			// 取值域未钉死,仅记日志)→ startLive(自动判定备流资格,E-22)。
			if (!Bili::BiliApi::preLiveCheck(res->message)) {
				obs_log(LOG_WARNING, "开播前置检查未通过: %s", res->message.c_str());
				res->outcome = Bili::Parse::StartOutcome::Blocked;
				return;
			}
			int preStatus = -1;
			std::string statusMessage;
			if (Bili::BiliApi::getRoomPreLiveStatus(preStatus, statusMessage))
				obs_log(LOG_INFO, "播前状态 GetRoomPreLiveStatus: status=%d", preStatus);
			else
				obs_log(LOG_INFO, "播前状态获取失败(不阻断开播): %s", statusMessage.c_str());
			res->outcome =
				Bili::BiliApi::startLive(res->cfg, mid, res->message, res->face_qr, res->face_page);
		},
		ui_guarded([res]() {
			auto &session = instance();
			session.end();
			if (res->outcome == Bili::Parse::StartOutcome::Ok) {
				Bili::Config &cfg = session.m_config.config();
				cfg.stream_targets = res->cfg.stream_targets;
				session.m_lines_obtained_ms = os_gettime_ns() / 1000000ULL;
				session.m_config.save();
				// 乐观更新:服务端已打开场次;随后的对账刷新以 room/GetInfo 为准。
				session.m_room.info.live_status = 1;
				session.m_room.stale = false;
				session.m_face_qr.clear();
				const std::string srt_hint =
					session.refresh_srt_catalog()
						? Utils::Text("Srt.CatalogHint", Srt::kServiceEntryName)
						: std::string();
				// message 在成功分支只承载「备流自动判定」类提示(无资格已按普通
				// 发送 / 备流被拒已回退)。
				session.set_status(res->message.empty() ? LOG_INFO : LOG_WARNING,
						   Utils::Text("Start.Success", cfg.stream_targets.size()) + srt_hint +
							   (res->message.empty() ? "" : " — " + res->message));
				session.notify();
				session.refresh(true);
				return;
			}
			if (res->outcome == Bili::Parse::StartOutcome::NeedFacePage) {
				// 60043: auth-middle 是网页不是二维码,按官方直接开认证窗口。
				const bool shown = openInBrowser(res->face_page);
				session.set_status(LOG_WARNING,
						   shown ? Utils::Text("Start.FacePageOpened")
							 : Utils::Text("Start.FaceManual", res->face_page.c_str()));
			} else if (res->outcome == Bili::Parse::StartOutcome::NeedFaceQr) {
				// 60024 / need_face_auth:侧栏直接渲染二维码;没有侧栏时交浏览器。
				session.m_face_qr = res->face_qr;
				if (session.m_login_qr_external && !res->face_qr.empty()) {
					const std::string svgPath = module_file("face_qr.svg");
					const bool shown =
						(!svgPath.empty() && QrGenerator::writeSvgFile(svgPath, res->face_qr) &&
						 openInBrowser(svgPath)) ||
						openInBrowser(res->face_qr);
					session.set_status(LOG_WARNING, shown ? Utils::Text("Start.FaceQrOpened")
									      : Utils::Text("Start.FaceManual",
											    res->face_qr.c_str()));
				} else {
					session.set_status(LOG_WARNING, Utils::Text("Api.FaceAuthNeeded"));
				}
			} else {
				session.set_status(LOG_ERROR, Utils::Text("Start.Failed", res->message.c_str()));
			}
			session.notify();
		}));
}

void Session::stop_live()
{
	// 官方 CloseLiveRoomPre 的顺序是「先停流(输出)再停播(信令)」。插件不驱动 OBS
	// 输出(ADR-0001/0005),因此推流仍在进行时先提示用户在 OBS 停止推流,避免房间
	// 已关而画面上传仍在继续。
	if (obs_streaming()) {
		set_status(LOG_WARNING, Utils::Text("Stop.NeedStopStreamFirst"));
		notify();
		return;
	}
	if (!can_stop_live()) {
		set_status(LOG_WARNING, Utils::Text("Stop.NotLive"));
		notify();
		return;
	}
	if (!begin(Busy::Stop, nullptr))
		return;
	const Bili::Config cfg = m_config.config();
	struct StopResult {
		bool ok = false;
		std::string message;
	};
	auto res = std::make_shared<StopResult>();
	Http::HttpClient::runAsync(
		[cfg, res]() { res->ok = Bili::BiliApi::stopLive(cfg, res->message); }, ui_guarded([res]() {
			auto &session = instance();
			session.end();
			if (res->ok) {
				// 场次结束,线路与密钥随场次作废(密钥每场新生成)。
				session.m_config.config().stream_targets.clear();
				session.m_config.save();
				session.m_room.info.live_status = 0;
				session.m_room.stale = false;
				session.m_face_qr.clear();
				session.set_status(LOG_INFO, Utils::Text("Stop.Done"));
			} else {
				session.set_status(LOG_ERROR, Utils::Text("Stop.Failed", res->message.c_str()));
			}
			session.notify();
			if (res->ok)
				session.refresh(true);
		}));
}

void Session::recover()
{
	const std::string room_id = m_config.config().room_id;
	if (!m_logged_in || room_id.empty()) {
		set_status(LOG_WARNING, Utils::Text("Room.Empty"));
		notify();
		return;
	}
	if (!begin(Busy::Recover, nullptr))
		return;
	const Bili::Config cfg = m_config.config();
	struct RecoverResult {
		bool ok = false;
		std::string message;
		std::string addr;
		std::string code;
		std::string source; // 端点来源(给状态行)
	};
	auto res = std::make_shared<RecoverResult>();
	Http::HttpClient::runAsync(
		[cfg, res]() {
			// M1b 断流找回(契约 §7): 官方序 = get_stream_by_roomId 直接给
			// data.rtmp{addr,code};取不到再问 GetUpStreamRtmp(can_change 时用
			// urls[0],密钥复用本地留存的推流码)。
			const json11::Json stream = Bili::BiliApi::getActiveStream(cfg.room_id, res->message);
			if (stream.is_object()) {
				res->addr = stream["rtmp"]["addr"].string_value();
				res->code = stream["rtmp"]["code"].string_value();
				if (!res->addr.empty() && !res->code.empty()) {
					res->source = "get_stream_by_roomId";
					res->ok = true;
					return;
				}
			}
			bool canChange = false;
			std::vector<std::string> urls;
			std::string upMessage;
			if (!Bili::BiliApi::getUpStreamRtmp(cfg.room_id, canChange, urls, upMessage)) {
				res->message = Utils::Text("Recover.Fail", upMessage.c_str());
				return;
			}
			if (!canChange || urls.empty()) {
				res->message =
					Utils::Text("Recover.NoAddr") + (upMessage.empty() ? "" : ": " + upMessage);
				return;
			}
			std::string local_key;
			for (const auto &t : cfg.stream_targets) {
				if (!t.backup && !t.key.empty()) {
					local_key = t.key;
					break;
				}
			}
			if (local_key.empty()) {
				res->message = Utils::Text("Recover.NoKey");
				return;
			}
			res->addr = urls[0];
			res->code = local_key;
			res->source = "GetUpStreamRtmp";
			res->ok = true;
		},
		ui_guarded([res]() {
			auto &session = instance();
			session.end();
			if (!res->ok) {
				session.set_status(LOG_ERROR, Utils::Text("Recover.Failed", res->message.c_str()));
				session.notify();
				return;
			}
			Bili::Config &cfg = session.m_config.config();
			// 找回成功只落到展示线路表,不碰 OBS 配置。
			cfg.stream_targets.clear();
			cfg.stream_targets.push_back(Bili::StreamTarget{"rtmp", "", res->addr, res->code, false});
			session.m_lines_obtained_ms = os_gettime_ns() / 1000000ULL;
			session.m_config.save();
			// 恢复路径只产出 rtmp 线路:目录条目回退为默认占位入口(ADR-0007 预期行为)。
			session.refresh_srt_catalog();
			session.m_room.info.live_status = 1;
			session.set_status(LOG_INFO, Utils::Text("Recover.Success", res->source.c_str()));
			session.notify();
		}));
}

// ---------------------------------------------------------------------------
// 质量口径(只读展示) / 分区表
// ---------------------------------------------------------------------------

void Session::fetch_quality()
{
	if (!begin(Busy::Quality, nullptr))
		return;
	const int area_id = m_config.config().area_id;
	struct QualityResult {
		std::string lines;
	};
	auto res = std::make_shared<QualityResult>();
	Http::HttpClient::runAsync(
		[res, area_id]() {
			std::string message;
			const json11::Json json = Bili::BiliApi::videoQualityConfig(message);
			if (json.is_object()) {
				// 实网形状(2026-11-25 实测,契约 §5.4):推荐码率不在顶层,
				// 而是 data.bitrate_config.recommend_bitrate_area_intervention.
				// recommend_bitrate_area_v2_interventions[] 里按二级分区命中;
				// 未命中回落 default_bitrate。
				const json11::Json bc = json["data"]["bitrate_config"];
				const int default_bitrate = static_cast<int>(bc["default_bitrate"].number_value());
				int recommend = default_bitrate;
				std::string source = Utils::Text("Quality.SourceDefault");
				for (const auto &tier :
				     bc["recommend_bitrate_area_intervention"]["recommend_bitrate_area_v2_interventions"]
					     .array_items()) {
					bool hit = false;
					for (const auto &area : tier["area_v2_ids"].array_items()) {
						if (area.int_value() == area_id) {
							hit = true;
							break;
						}
					}
					if (hit) {
						recommend = static_cast<int>(tier["recommend_bitrate"].number_value());
						source = Utils::Text("Quality.SourceIntervention",
								     tier["strategy_source"].string_value().c_str());
						break;
					}
				}
				res->lines =
					Utils::Text("Quality.Recommend", recommend, source.c_str(), default_bitrate) +
					"\n" +
					Utils::Text("Quality.Member",
						    bc["recommend_bitrate_member_intervention"].bool_value()
							    ? Utils::Text("Common.Yes").c_str()
							    : Utils::Text("Common.No").c_str()) +
					"\n" + Utils::Text("Quality.Note");
			} else {
				res->lines = Utils::Text("Quality.Fail", message.c_str());
			}
			// 编码能力各自降级,不互相牵连。
			bool canH265 = false, canAv1 = false;
			if (Bili::BiliApi::canUseH265Encoder(canH265, message))
				res->lines += "\n" + Utils::Text("Quality.H265",
								 canH265 ? Utils::Text("Common.Is").c_str()
									 : Utils::Text("Common.NoShort").c_str());
			else
				res->lines += "\n" + Utils::Text("Quality.H265Fail", message.c_str());
			if (Bili::BiliApi::canUseAv1Encoder(canAv1, message))
				res->lines += " | " + Utils::Text("Quality.Av1",
								  canAv1 ? Utils::Text("Common.Is").c_str()
									 : Utils::Text("Common.NoShort").c_str());
			else
				res->lines += " | " + Utils::Text("Quality.Av1Fail", message.c_str());
		},
		ui_guarded([res]() {
			auto &session = instance();
			session.end();
			session.m_quality = res->lines;
			session.set_status(LOG_INFO, Utils::Text("Quality.Updated"));
			session.notify();
		}));
}

void Session::load_partitions()
{
	if (!m_logged_in || m_partitions_loading)
		return;
	struct ListResult {
		json11::Json data;
	};
	auto res = std::make_shared<ListResult>();
	m_partitions_loading = true;
	notify();
	Http::HttpClient::runAsync(
		[res]() {
			std::string message;
			res->data = Bili::BiliApi::getPartitionList(message);
		},
		ui_guarded([res]() {
			auto &session = instance();
			session.m_partitions_loading = false;
			session.m_partitions = res->data;
			// 无论成败都算「试过」:失败只允许显式重试(重开控制台)。旧版失败时
			// tried 保持 false,而失败回调又会刷新属性表 → 再次触发加载 ⇒ 请求死循环
			// (断网/API 改版时每 ~100ms 一次,直到触发风控)。
			session.m_partitions_tried = true;
			session.notify();
		}));
}

void Session::ensure_partitions(bool force)
{
	if (!m_alive)
		return;
	if (force)
		m_partitions_tried = false;
	if (m_partitions_tried || m_partitions_loading || !m_logged_in)
		return;
	load_partitions();
}

std::string Session::line_label(size_t index) const
{
	// 主/备各自顺序编号(与 OBS 服务列表里的入口名同源,见 refresh_srt_catalog)。
	size_t primary = 0, backup = 0;
	const auto &targets = m_config.config().stream_targets;
	std::string label;
	for (size_t i = 0; i <= index && i < targets.size(); ++i) {
		const Bili::StreamTarget &t = targets[i];
		label = t.backup ? Utils::Text("Srt.LineBackup", static_cast<int>(++backup))
				 : Utils::Text("Srt.LinePrimary", static_cast<int>(++primary));
		label += " · " + t.protocol;
		if (!t.provider.empty())
			label += "/" + t.provider;
	}
	return label;
}

bool Session::refresh_srt_catalog()
{
	std::vector<Srt::CatalogLine> lines;
	const auto &targets = m_config.config().stream_targets;
	for (size_t i = 0; i < targets.size(); ++i) {
		if (!Srt::IsSrtLine(targets[i].addr))
			continue;
		lines.push_back({line_label(i), targets[i].addr});
	}
	Srt::UpdateServiceCatalog(lines, Utils::Text("Srt.DefaultServer"));
	return !lines.empty();
}

std::string Session::area_display() const
{
	const Bili::Config &cfg = m_config.config();
	const Bili::Parse::RoomInfo &info = m_room.info;
	const int part = cfg.part_id > 0 ? cfg.part_id : info.parent_id;
	const int area = cfg.area_id > 0 ? cfg.area_id : info.area_id;
	std::string part_name;
	std::string area_name = info.area_name;
	if (m_partitions.is_array()) {
		for (const auto &parent : m_partitions.array_items()) {
			int parent_id = 0;
			if (!parse_area_id(parent["id"], parent_id) || parent_id != part)
				continue;
			part_name = parent["name"].string_value();
			for (const auto &entry : parent["list"].array_items()) {
				int entry_id = 0;
				if (parse_area_id(entry["id"], entry_id) && entry_id == area)
					area_name = entry["name"].string_value();
			}
		}
	}
	const std::string unknown = Utils::Text("Common.Unknown");
	if (part_name.empty())
		part_name = part > 0 ? std::to_string(part) : unknown;
	if (area_name.empty())
		area_name = area > 0 ? std::to_string(area) : unknown;
	return part_name + " / " + area_name;
}

} // namespace Core
