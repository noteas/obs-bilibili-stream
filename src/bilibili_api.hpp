#pragma once
#include <string>
#include <vector>
#include "json11/json11.hpp"

namespace Bili {
namespace {
const std::string APP_KEY = "aae92bc66f3edfab";
const std::string APP_SECRET = "af125a0d5279fd576c1b4418a3e8276d";
} // namespace

struct Config {
	std::string room_id;
	std::string csrf_token;
	std::string cookies;
	std::string mid;
	std::string title;
	bool login_status = false;
	bool streaming = false;
	std::string rtmp_addr;
	std::string rtmp_code;
	int part_id = 2;
	int area_id = 86;
	// 质量协商(§5.4):用户设定的视频码率 kbps;0 = 不干预。应用=写当前视频编码器。
	int bitrate_kbps = 0;
	// 推流线路(M1/U1):开播响应的 protocols[]/protocols_backup[] 全量保存,
	// 控制台逐条只读展示(地址与推流码均可复制)。插件**不改** OBS 服务配置,
	// 用哪条线路、何时切、怎么填 OBS 全由用户决定(ADR-0005)。
	struct StreamTarget {
		std::string protocol; // rtmp / srt / bmtsrt / bmt …(官方给什么展示什么)
		std::string provider; // txy / qn / …(可空)
		std::string addr;
		std::string key; // 直连线路推流码=响应条目自带 code(缺则 live_key);见契约 §4.1 第5步
		bool backup = false; // 来自 protocols_backup[]
	};
	std::vector<StreamTarget> stream_targets;
};

// Endpoints never carry identity arguments: cookies, UA and client_traceid are
// attached by Http::HttpClient (fingerprint single egress). Config.cookies only
// mirrors the persisted whitelist for config.json compatibility.
class BiliApi {
public:
	static void init();
	static void cleanup();
	static bool getQrCode(std::string &qr_data, std::string &qr_key, std::string &message);
	// Success = login cookies landed in the session jar (Set-Cookie merge or
	// the URL-query fallback), not a returned cookie string.
	// expired(可选)回带「二维码已失效(86038)」判据;调用方不要按 message 文本嗅探
	// (文案随 UI 语言变化)。
	static bool qrLogin(const std::string &qr_key, std::string &message, bool *expired = nullptr);
	static bool checkLoginStatus(std::string &message, std::string &mid);
	// Room bootstrap. app-blink room/GetInfo?uId=<mid> (§3.3; parser
	// F-1802b3690 reads data.room_id, proven). 官方端点失败即失败——旧 Web
	// 端点 room/v2/Room/room_id_by_uid 的兼容回退已于 3.0.2 移除。
	static bool getRoomIdAndCsrf(std::string &room_id, std::string &csrf_token, std::string &message);
	static json11::Json getPartitionList(std::string &message);
	// with_backup=true = 官方同款自动分派(E-22,UI 单「开播」按钮即此模式):
	// 先查 GetVisibleMode,upower_privilege_type==5 才走备流构造
	// (F-180146070 参数位: src(E-19/G1,取 get_stream_by_roomId 的
	// stream_line[].src)+ backup_stream=1 + backup_stream_type=2/stream_biz=
	// Paylive,3.0.2 起固定官方字面量);≠5 或查询后无资格则按官方普通开播发送并在
	// message 说明。资格查询失败维持强发+失败回退普通开播兜底。
	// face_qr 只在服务端给二维码(data.qr)时填充;face_page_url 用于 60043
	// 的 auth-middle 网页(官方直接开窗口,不是二维码)。
	static bool startLive(Config &config, std::string &rtmp_addr, std::string &rtmp_code, std::string &message,
			      std::string &face_qr, std::string &mid, bool with_backup = false,
			      std::string *face_page_url = nullptr);
	static bool stopLive(const Config &config, std::string &message);
	static bool updateRoomInfo(const Config &config, std::string &message, const std::string &title = "",
				   int areaId = -1);
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
	static long cachedBuild();
	// §4.3 AnchorChangeRoomArea (播中改分区): room_id+platform=pc_link+build+area_id
	// +csrf+sign, all four literals builder-proven (F-1801e8790).
	static bool changeRoomArea(const Config &config, int area_id, std::string &message);
	// §3.1/§3.2 shared builder F-1800c3810 (room_id + device="pc" + common + sign).
	// endpoint = "getInfoByUser" (own room, session-keyed; callers pass their known
	// room_id — the official parse cluster reads no room_id field back) or
	// "getInfoByRoom". Returns data on success, null Json + message on failure.
	static json11::Json getRoomInfo(const std::string &endpoint, const std::string &room_id, std::string &message);
	// §3.3 app-blink room/GetInfo (builder F-1802b1e60): uId-only business param,
	// session-required; parser F-1802b3690 proven to read data.room_id (plus the
	// §3.1 family incl. live_status/live_key/title). Returns data or null Json.
	static json11::Json getRoomInfoByUid(const std::string &mid, std::string &message);
	// §4.2 UpdatePreLiveInfo (播前改标题等): sign covers common params ONLY, then
	// mobi_app=pc_link + title (non-empty) + csrf pair are appended (F-180151c10).
	static bool updatePreLiveInfo(const Config &config, const std::string &title, std::string &message);
	// §3.5 PreCheck (开播前合规/推广拦截): POST with NO business params (builder
	// F-1804c4c00: bare common+csrf+sign). Returns false when data.game_promotion
	// .need_notice blocks the start; block_message carries title/content for the
	// status line. Transport/parse failures return false with the failure message.
	static bool preLiveCheck(std::string &block_message);
	// §3.6 GetRoomPreLiveStatus: GET, no business params (session-keyed; parser
	// F-180447550 reads data.status int ONLY). Value set not pinned — surfaced
	// raw for observation.
	static bool getRoomPreLiveStatus(int &status, std::string &message);
	// §7.1 get_stream_by_roomId (F-18014bf40; parser F-18014cf60): 公共参数
	// injector(F-180063f70)+ room_id + sign(业务参数只有 room_id)。data =
	// {rtmp:{addr,code}, stream_line:[{src,cdn_name,checked}]}。进行中场次的
	// 推流地址与密钥。Returns data or null Json.
	static json11::Json getActiveStream(const std::string &room_id, std::string &message);
	// §3.10 GetVisibleMode (builder F-18044c590; parser F-18044d410): 无业务
	// 参数,公共参数+appsign。data = {visible_mode, guard_visible_type,
	// upower_privilege_type}。E-22:官方开播分派器(bililive.dll F-180d5de60)
	// 按 upower_privilege_type 选 startLive builder——==5 才走备流 builder
	// (backup_stream=1/type=2/stream_biz=Paylive),其余值走无备流参 builder。
	static json11::Json getVisibleMode(std::string &message);
	// §7.2 GetUpStreamRtmp (F-18014d920): ReconnectLiveInfo{can_change, urls[],
	// message}. 恢复推流前的可推地址查询(配套 §3.1 live_status 判在播)。
	static bool getUpStreamRtmp(const std::string &room_id, bool &can_change, std::vector<std::string> &urls,
				    std::string &message);
	// §5.4 GetUpStreamVideoQualityConfig (视频码率口径): GET, 管线例外——sign 只覆盖
	// 公共参数,bitrate_intervention="1" 在 sign 后注入 (F-1802d40f0)。顶层带
	// recommend_bitrate/strategy_source + data.bitrate_config{default_bitrate,
	// recommend_bitrate_{area,member}_intervention} (解析器 F-1802d5060)。
	static json11::Json videoQualityConfig(std::string &message);
	// §5.5 编码能力查询: GET 无业务参数;H265 解析器只读 data.can_enable,
	// AV1 只读 data.can_hard_encode (F-1802d08c0/F-1802d0f30)。
	static bool canUseH265Encoder(bool &can_enable, std::string &message);
	static bool canUseAv1Encoder(bool &can_hard_encode, std::string &message);

private:
	static std::string appsign(const std::vector<std::pair<std::string, std::string>> &params,
				   const std::string &app_key, const std::string &app_sec);
};
} // namespace Bili
