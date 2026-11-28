#include "bili_parsers.hpp"

#include <cmath>

namespace Bili {
namespace Parse {

namespace {

// Bilibili 把人类可读的错误文本放在 "message" 或 "msg"(因端点而异),两者都认。
std::string jsonErrorMessage(const json11::Json &json)
{
	std::string message = json["message"].string_value();
	if (message.empty())
		message = json["msg"].string_value();
	return message;
}

// id 类字段在 B站 JSON 里 number/string 两种形状都出现过(实网:分区 id、房间号),
// 一律归一成十进制文本;非整数(含 NaN/超大)返回空。
std::string id_to_string(const json11::Json &value)
{
	if (value.is_string())
		return value.string_value();
	if (!value.is_number())
		return {};
	const double number = value.number_value();
	if (!std::isfinite(number) || number != std::floor(number))
		return {};
	return std::to_string(static_cast<long long>(number));
}

bool is_positive_id(const std::string &id)
{
	return !id.empty() && id != "0" && id[0] != '-';
}

// §4.1 第 5 步:protocols[] 的一个条目 → StreamTarget。缺 addr 的条目由调用方丢弃。
StreamTarget target_from_entry(const json11::Json &entry, const std::string &live_key, bool backup)
{
	StreamTarget t;
	t.protocol = entry["protocol"].string_value().empty() ? "rtmp" : entry["protocol"].string_value();
	t.provider = entry["provider"].string_value();
	t.addr = entry["addr"].string_value();
	// 直连线路的推流码 = 条目自带 code("?streamname=…&key=…" 查询串)。
	// 实证(2026-11-26 实网):拿 live_key 当推流码会被 edge 在 publish 后 ~0.4s 内拒绝;
	// live_key 只是官方解析器的内部配对存储,不是推流用 key。code 缺失才回落 live_key。
	t.key = entry["code"].string_value().empty() ? live_key : entry["code"].string_value();
	t.backup = backup;
	return t;
}

} // namespace

bool ParseRoomInfo(const json11::Json &data, RoomInfo &out)
{
	if (!data.is_object())
		return false;
	const std::string room_id = id_to_string(data["room_id"]);
	if (!is_positive_id(room_id))
		return false;
	RoomInfo info;
	info.room_id = room_id;
	info.title = data["title"].string_value();
	info.live_status = data["live_status"].is_number() ? data["live_status"].int_value() : -1;
	// wire 实名 lock_status + lock_time(契约 §3.3 实网观测);解析簇名 lock_status_v2
	// 一并兼容,两者任一非零即视为锁定。
	const bool locked_v2 = data["lock_status_v2"].is_number() && data["lock_status_v2"].int_value() != 0;
	const bool locked = data["lock_status"].is_number() && data["lock_status"].int_value() != 0;
	info.locked = locked || locked_v2;
	info.lock_time = data["lock_time"].is_number() ? data["lock_time"].int_value() : 0;
	info.lock_reason = data["lock_reason"].string_value();
	const std::string parent = id_to_string(data["parent_id"]);
	const std::string area = id_to_string(data["area_v2_id"]);
	info.parent_id = is_positive_id(parent) ? std::stoi(parent) : 0;
	info.area_id = is_positive_id(area) ? std::stoi(area) : 0;
	info.area_name = data["area_v2_name"].string_value();
	out = std::move(info);
	return true;
}

LoginState LoginFromNav(const json11::Json &root, std::string &mid, std::string &message)
{
	if (!root.is_object()) {
		message = "nav: response is not an object";
		return LoginState::Unknown;
	}
	const int code = root["code"].int_value();
	const bool is_login = root["data"]["isLogin"].bool_value();
	if (code == 0 && is_login) {
		mid = id_to_string(root["data"]["mid"]);
		if (mid.empty()) {
			message = "nav: missing data.mid";
			return LoginState::Unknown;
		}
		return LoginState::LoggedIn;
	}
	// -101 = 账号未登录(契约 §1.2);code==0 且 isLogin=false 同义。
	if (code == -101 || (code == 0 && root["data"].is_object())) {
		message = jsonErrorMessage(root);
		return LoginState::LoggedOut;
	}
	// 其余(-412 风控 / -799 限频 / -509 …)= 结果未知,调用方保持原登录态。
	message = "code=" + std::to_string(code) + " " + jsonErrorMessage(root);
	return LoginState::Unknown;
}

StartLive StartLiveEnvelope(const json11::Json &root, const std::string &mid)
{
	StartLive result;
	if (!root.is_object()) {
		result.outcome = StartOutcome::MissingFields;
		result.server_message = "startLive: response is not an object";
		return result;
	}
	result.code = root["code"].int_value();
	result.server_message = jsonErrorMessage(root);
	const json11::Json data = root["data"];

	// 第 12 步: data.notice = 服务端阻断载荷(封禁/警告),有 title/msg 即视为有内容。
	if (data["notice"].is_object()) {
		const json11::Json notice = data["notice"];
		const std::string title = notice["title"].string_value();
		const std::string msg = notice["msg"].string_value();
		if (!title.empty() || !msg.empty())
			result.notice_line = (title.empty() ? std::string("notice") : title) + ": " + msg;
	}

	if (result.code != 0) {
		result.voucher = data["risk_extra"]["v_voucher"].string_value();
		// 60024 / code==0+need_face_auth 走同一个二维码通道。
		if (result.code == 60024) {
			result.face_qr = data["qr"].string_value();
			result.outcome = StartOutcome::NeedFaceQr;
			return result;
		}
		if (result.code == 60043) {
			// 官方 60043 中间页(契约 §1.2/§3.9):auth-middle.html?source_event=200&mid=
			result.face_page =
				"https://www.bilibili.com/blackboard/live/auth-middle.html?source_event=200&mid=" + mid;
			result.outcome = StartOutcome::NeedFacePage;
			return result;
		}
		result.outcome = result.notice_line.empty() && result.voucher.empty() ? StartOutcome::Rejected
										      : StartOutcome::Blocked;
		return result;
	}

	// code==0: 官方第 12 步对成功分支同样按弹窗阻断处理——有 notice 就不算可开播。
	if (!result.notice_line.empty()) {
		result.outcome = StartOutcome::Blocked;
		return result;
	}
	// risk_extra.v_voucher = geetest4 风控凭证(§2.5 同族),插件不内嵌 geetest。
	result.voucher = data["risk_extra"]["v_voucher"].string_value();
	if (!result.voucher.empty()) {
		result.outcome = StartOutcome::Blocked;
		return result;
	}
	// 第 2 步: need_face_auth(服务端可能在 code==0 时同样要求人脸)。
	if (data["need_face_auth"].bool_value()) {
		result.face_qr = data["qr"].string_value();
		result.outcome = StartOutcome::NeedFaceQr;
		return result;
	}

	// 第 3~6 步: 默认协议字面量 "rtmp";protocols[] 非空 → 逐条 StreamTarget,
	// 任一条带 addr 即成功;否则旧式回落 rtmp.new_link(必须存在)+ rtmp.addr。
	const std::string live_key = data["live_key"].string_value();
	const json11::Json protocols = data["protocols"];
	for (const json11::Json &entry : protocols.array_items()) {
		const StreamTarget t = target_from_entry(entry, live_key, false);
		if (t.addr.empty())
			continue;
		if (result.primary_addr.empty()) {
			result.primary_addr = t.addr;
			result.primary_key = t.key;
		}
		if (t.protocol == "rtmp" && result.rtmp_addr.empty()) {
			result.rtmp_addr = t.addr;
			result.rtmp_key = t.key;
		}
		result.targets.push_back(t);
	}
	if (result.targets.empty()) {
		const json11::Json rtmp = data["rtmp"];
		// 第 6 步: rtmp.new_link 必须存在(缺→无效),addr 非空才算有效。
		if (rtmp.is_object() && !rtmp["new_link"].is_null() && !rtmp["addr"].string_value().empty()) {
			StreamTarget t;
			t.protocol = "rtmp";
			t.addr = rtmp["addr"].string_value();
			t.key = rtmp["code"].string_value();
			result.targets.push_back(t);
			result.rtmp_addr = t.addr;
			result.rtmp_key = t.key;
			result.primary_addr = t.addr;
			result.primary_key = t.key;
		}
	}
	// 第 7 步: protocols_backup[] 与主流同构(空数组 = 无操作)。
	for (const json11::Json &entry : data["protocols_backup"].array_items()) {
		const StreamTarget t = target_from_entry(entry, live_key, true);
		if (t.addr.empty())
			continue;
		result.targets.push_back(t);
	}
	result.outcome = result.targets.empty() ? StartOutcome::MissingFields : StartOutcome::Ok;
	return result;
}

} // namespace Parse
} // namespace Bili
