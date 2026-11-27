#pragma once
// 端点响应的纯解析层(无 I/O、无 obs 依赖,可离线单测):把契约里逐字段钉死的
// 解析序(docs/api/livehime-api.md §3.3 / §2.9 / §4.1)从 HTTP 调用里拆出来。
// BiliApi:: 各端点 = 构造请求 → 传输 → 调这里的解析;tests/ 直接喂 fixtures。
#include <string>
#include <vector>

#include "json11/json11.hpp"

namespace Bili {

// 推流目标(StreamTarget,{protocol, provider, addr, key, backup})——startLive /
// 恢复响应与线路展示之间的统一中间模型(GLOSSARY)。定义在这里是因为解析层要产出它,
// 而 Config(持久化状态)要保存它,两者都不能依赖对方。
struct StreamTarget {
	std::string protocol; // rtmp / srt / bmtsrt / bmt …(官方给什么展示什么)
	std::string provider; // txy / qn / …(可空)
	std::string addr;
	std::string key;     // 直连线路推流码=响应条目自带 code(缺则 live_key);见契约 §4.1 第5步
	bool backup = false; // 来自 protocols_backup[]
};

namespace Parse {

// §3.3 app-blink room/GetInfo 的 data(F-1802b3690 回读簇的子集 = 插件消费的字段)。
struct RoomInfo {
	std::string room_id; // number 或 string 一律归一成十进制文本
	std::string title;
	int live_status = -1; // 0 未开播 / 1 直播中 / 2 轮回 / -1 字段缺失(未知)
	bool locked = false;  // wire 实名 lock_status(实网 2026-11-25,非解析簇的 lock_status_v2)
	int lock_time = 0;
	std::string lock_reason;
	int parent_id = 0; // 分区大类
	int area_id = 0;   // 二级分区 = startLive 的 area_v2
	std::string area_name;
};

// data 不是对象或 room_id 无效 ⇒ false(out 不变);其余字段缺失取各自默认。
bool ParseRoomInfo(const json11::Json &data, RoomInfo &out);

// §2.9 nav:三态——网络/风控失败不得把「已登录」误判成「未登录」。
enum class LoginState { LoggedIn, LoggedOut, Unknown };
// root = nav 信封。LoggedIn 时 mid 回填;LoggedOut/Unknown 时 message 回填可展示原因。
LoginState LoginFromNav(const json11::Json &root, std::string &mid, std::string &message);

// §4.1 startLive 信封的解析结果(F-180144bb0 第 1~12 步)。
enum class StartOutcome {
	Ok,            // 至少一条带 addr 的线路
	NeedFaceQr,    // 60024(data.qr)或 code==0 且 need_face_auth
	NeedFacePage,  // 60043:auth-middle 网页
	Blocked,       // notice 弹窗阻断 / risk_extra.v_voucher
	Rejected,      // 其它 code!=0(message = 服务端原因)
	MissingFields, // code==0 但没有任何可用线路(官方 missing fields)
};

struct StartLive {
	StartOutcome outcome = StartOutcome::MissingFields;
	int code = 0;
	std::string server_message; // envelope message/msg
	std::string notice_line;    // data.notice "title: msg" (empty = no blocking notice)
	std::string voucher;        // data.risk_extra.v_voucher (empty = no risk-control hit)
	// protocols[] + protocols_backup[] 里**带 addr** 的全部线路(ADR-0005 全量展示)。
	std::vector<StreamTarget> targets;
	// 兼容便捷副本:首条 rtmp 主线路(没有则取首条主线路)。
	std::string rtmp_addr;
	std::string rtmp_key;
	std::string primary_addr;
	std::string primary_key;
	std::string face_qr;   // NeedFaceQr: 二维码原文
	std::string face_page; // NeedFacePage: 认证页 URL(需 mid 拼接)
			       // Failure/blocked texts are localized (Utils::Text needs obs) in the BiliApi layer;
			       // this struct only carries raw data.   // 失败/阻断原因(给状态行);Ok 时为空
};

// root = startLive 信封,mid = 登录 mid(60043 页面参数)。
StartLive StartLiveEnvelope(const json11::Json &root, const std::string &mid);

} // namespace Parse
} // namespace Bili
