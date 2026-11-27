#pragma once
// 端点层:每个端点 = builder + parser(契约 docs/api/livehime-api.md)。参数注入只走本文件
// 的统一管线(公共参数 appsign / csrf / 签名),响应解析委托 bili_parsers(纯函数,可单测)。
#include <string>
#include <vector>

#include "bili_parsers.hpp"
#include "json11/json11.hpp"

namespace Bili {
namespace {
const std::string APP_KEY = "aae92bc66f3edfab";
const std::string APP_SECRET = "af125a0d5279fd576c1b4418a3e8276d";
} // namespace

// 持久化到 config.json 的插件状态(经 Core::ConfigManager 读写)。运行期态(登录态、
// 进行中场次、忙碌标志)不在这里——它们以服务端为准,由 Core::Session 在刷新时得出。
struct Config {
	std::string room_id;    // 服务端房间号(登录后由 room/GetInfo 回填;无占位值)
	std::string csrf_token; // bili_jct 的镜像(登录态刷新时回填)
	std::string cookies;    // 持久化 Cookie 白名单镜像(SESSDATA/bili_jct/DedeUserID/buvid*)
	std::string mid;
	std::string title; // 服务端标题镜像(room/GetInfo 每次刷新回填)
	// 分区:part_id = 分区大类(parent_id),area_id = 二级分区(= startLive 的 area_v2)。
	// 初值 0 = 未知;登录后由 room/GetInfo 的 parent_id/area_v2_id 同步,控制台
	// 「应用分区」成功后改写为用户选择。
	int part_id = 0;
	int area_id = 0;
	// 推流线路(M1/U1):开播/找回响应的 protocols[]/protocols_backup[] 全量保存,
	// 控制台与侧栏逐条只读展示(地址与推流码可复制)。插件**不改** OBS 服务配置,
	// 用哪条线路、何时切、怎么填 OBS 全由用户决定(ADR-0005)。
	// 线路表见 Bili::StreamTarget(bili_parsers.hpp):官方给什么存什么,ADR-0005 全量展示。
	std::vector<StreamTarget> stream_targets;
	// 侧栏(ADR-0010)首次加载时自动显示过一次:只做一次,之后尊重用户自己的停靠布局。
	bool dock_introduced = false;
};

// §2.2 二维码轮询结果。Success = 登录 Cookie 已进会话 jar(Set-Cookie 合并或 URL
// 参数回落),不是返回 cookie 字符串。NotScanned/Scanned 是轮询的正常节奏;
// Expired = 86038,调用方换码后继续;Failed 携带可展示的 message。
enum class QrPoll { Success, NotScanned, Scanned, Expired, Failed };

// Endpoints never carry identity arguments: cookies, UA and client_traceid are
// attached by Http::HttpClient (fingerprint single egress). Config.cookies only
// mirrors the persisted whitelist for config.json compatibility.
class BiliApi {
public:
	// §2.1 取登录二维码(原文 + qrcode_key)。
	static bool getQrCode(std::string &qr_data, std::string &qr_key, std::string &message);
	static QrPoll qrLogin(const std::string &qr_key, std::string &message);
	// §2.9 nav 登录态确认。三态:LoggedOut(服务端明确未登录,-101)与
	// Unknown(传输失败/风控/限频)必须区分——后者不得把已登录误判成未登录。
	static Parse::LoginState checkLoginStatus(std::string &message, std::string &mid);
	static json11::Json getPartitionList(std::string &message);
	// §4.1 startLive,官方同款自动分派(E-22):先查 GetVisibleMode,
	// upower_privilege_type==5 才走备流构造(F-180146070 参数位: src(E-19/G1,取
	// get_stream_by_roomId 的 stream_line[].src)+ backup_stream=1 +
	// backup_stream_type=2/stream_biz=Paylive);≠5 按官方普通开播发送。备流被服务端
	// 拒绝(非人脸/阻断类)时回退一次普通开播——仅当本次真的发了备流请求。
	// 成功 = 返回 Ok,config.stream_targets 被全量线路覆盖。message 承载失败原因或
	// 成功分支的提示(无备流资格 / 备流被拒已回退);face_qr / face_page 供 UI 展示。
	static Parse::StartOutcome startLive(Config &config, const std::string &mid, std::string &message,
					     std::string &face_qr, std::string &face_page);
	static bool stopLive(const Config &config, std::string &message);
	// 播前改标题(§4.2 UpdatePreLiveInfo)与改分区(§4.3 AnchorChangeRoomArea)。
	static bool updatePreLiveInfo(const Config &config, const std::string &title, std::string &message);
	static bool changeRoomArea(const Config &config, int area_id, std::string &message);
	// buvid3/buvid4 device identity via x/frontend/finger/spi (web-side
	// endpoint; the official client accumulates buvid3 through its CEF cookie
	// jar, which we do not have). Injects into the jar and persists; failures
	// are logged only — every current endpoint tolerates a missing buvid3.
	static void fetch_buvid();
	// B3 (contract §2.4): cross-domain login propagation. GET sso/list
	// (csrf=bili_jct) → GET every data.sso[] URL following redirects so each
	// domain's full 302 chain lands its cookies in the jar. Non-fatal: the
	// official H5 treats an sso failure as a logged telemetry event, not an error.
	static bool propagateSso(std::string &message);
	// getHomePageLiveVersion (contract §3.4) result, cached process-wide:
	// build/curr_version are the shared parameter values later signed requests
	// carry. Worker-thread only (single HTTP queue serializes access).
	static bool ensureVersionInfo(std::string &message);
	// §3.3 app-blink room/GetInfo (builder F-1802b1e60): uId-only business param,
	// session-required; parser F-1802b3690 proven to read data.room_id (plus the
	// §3.1 family incl. live_status/title/area)。Returns data or null Json.
	static json11::Json getRoomInfoByUid(const std::string &mid, std::string &message);
	// §3.5 PreCheck (开播前合规/推广拦截): 实网 wire 为 GET(POST 405),无业务参数
	// (builder F-1804c4c00)。返回 false 表示阻断或传输/解析失败,block_message 给状态行。
	static bool preLiveCheck(std::string &block_message);
	// §3.6 GetRoomPreLiveStatus: GET, no business params (session-keyed; parser
	// F-180447550 reads data.status int ONLY)。取值域未钉死,仅透出观测。
	static bool getRoomPreLiveStatus(int &status, std::string &message);
	// §7.1 get_stream_by_roomId (F-18014bf40; parser F-18014cf60):进行中场次的
	// 推流地址与密钥。data = {rtmp:{addr,code}, stream_line:[{src,cdn_name,checked}]}。
	static json11::Json getActiveStream(const std::string &room_id, std::string &message);
	// §3.10 GetVisibleMode:无业务参数,公共参数+appsign。data = {visible_mode,
	// guard_visible_type, upower_privilege_type}。
	static json11::Json getVisibleMode(std::string &message);
	// §7.2 GetUpStreamRtmp (F-18014d920): ReconnectLiveInfo{can_change, urls[], message}。
	static bool getUpStreamRtmp(const std::string &room_id, bool &can_change, std::vector<std::string> &urls,
				    std::string &message);
	// §5.4 GetUpStreamVideoQualityConfig:管线例外——sign 只覆盖公共参数,
	// bitrate_intervention="1" 在 sign 后注入 (F-1802d40f0)。返回整个信封。
	static json11::Json videoQualityConfig(std::string &message);
	// §5.5 编码能力查询: H265 只读 data.can_enable, AV1 只读 data.can_hard_encode。
	static bool canUseH265Encoder(bool &can_enable, std::string &message);
	static bool canUseAv1Encoder(bool &can_hard_encode, std::string &message);

private:
	static std::string appsign(const std::vector<std::pair<std::string, std::string>> &params,
				   const std::string &app_key, const std::string &app_sec);
};
} // namespace Bili
