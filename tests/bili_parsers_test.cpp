// 端点响应解析的离线回归(CTest):fixtures 取自契约 docs/api/livehime-api.md 钉死的
// 形状(§2.2 nav、§3.3 room/GetInfo、§4.1 startLive 解析序)。无网络、无 OBS。
#include "bili_parsers.hpp"

#include <cstdio>
#include <string>

using namespace Bili;

namespace {
int g_fail = 0;
int g_pass = 0;

void check(bool cond, const std::string &what)
{
	if (cond) {
		++g_pass;
	} else {
		++g_fail;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

json11::Json json(const std::string &text)
{
	std::string err;
	const json11::Json value = json11::Json::parse(text, err);
	check(err.empty(), "fixture JSON parses: " + text.substr(0, 40));
	return value;
}

void test_room_info()
{
	Parse::RoomInfo info;
	check(!Parse::ParseRoomInfo(json(R"({"live_status":1})"), info), "room: no room_id → false");
	check(!Parse::ParseRoomInfo(json("[]"), info), "room: non-object → false");
	check(Parse::ParseRoomInfo(json(R"({"room_id":1234567,"live_status":"1"})"), info),
	      "room: string live_status tolerated");
	check(info.room_id == "1234567" && info.live_status == -1, "room: non-numeric live_status = unknown");

	// 实网形状(2026-11-25):wire 锁定字段实名 lock_status + lock_time。
	check(Parse::ParseRoomInfo(
		      json(R"({"room_id":"21452505","title":"测试","live_status":1,"lock_status":0,"lock_time":0,
		             "parent_id":2,"area_v2_id":86,"area_v2_name":"电竞"})"),
		      info),
	      "room: full object");
	check(info.room_id == "21452505" && info.title == "测试" && info.live_status == 1 && !info.locked &&
		      info.parent_id == 2 && info.area_id == 86 && info.area_name == "电竞",
	      "room: fields");

	check(Parse::ParseRoomInfo(json(R"({"room_id":1,"lock_status":1,"lock_time":1700000000,"lock_reason":"r"})"),
				   info),
	      "room: locked");
	check(info.locked && info.lock_time == 1700000000 && info.lock_reason == "r", "room: lock fields");
	check(Parse::ParseRoomInfo(json(R"({"room_id":1,"lock_status_v2":2})"), info) && info.locked,
	      "room: parser-cluster name lock_status_v2 also means locked");
	check(Parse::ParseRoomInfo(json(R"({"room_id":0})"), info) == false, "room: room_id 0 invalid");
	check(Parse::ParseRoomInfo(json(R"({"room_id":1.5})"), info) == false, "room: fractional id invalid");
}

void test_login_from_nav()
{
	std::string mid, message;
	check(Parse::LoginFromNav(json(R"({"code":0,"message":"0","data":{"isLogin":true,"mid":42}})"), mid, message) ==
		      Parse::LoginState::LoggedIn &&
		      mid == "42",
	      "nav: logged in (numeric mid)");
	check(Parse::LoginFromNav(json(R"({"code":0,"data":{"isLogin":true,"mid":"77"}})"), mid, message) ==
		      Parse::LoginState::LoggedIn &&
		      mid == "77",
	      "nav: logged in (string mid)");
	check(Parse::LoginFromNav(json(R"({"code":-101,"message":"账号未登录","data":{"isLogin":false}})"), mid,
				  message) == Parse::LoginState::LoggedOut && message == "账号未登录",
	      "nav: -101 = logged out");
	check(Parse::LoginFromNav(json(R"({"code":0,"data":{"isLogin":false}})"), mid, message) ==
		      Parse::LoginState::LoggedOut,
	      "nav: code 0 + isLogin false = logged out");
	// 旧版把这两种情况也当成「未登录」→ 按钮全部禁用。
	check(Parse::LoginFromNav(json(R"({"code":-412,"message":"请求被拦截"})"), mid, message) ==
		      Parse::LoginState::Unknown,
	      "nav: -412 risk control = unknown (not logged-out)");
	check(Parse::LoginFromNav(json(R"({"code":0,"message":"0"})"), mid, message) == Parse::LoginState::Unknown,
	      "nav: code 0 without data object = unknown");
	check(Parse::LoginFromNav(json("null"), mid, message) == Parse::LoginState::Unknown, "nav: null body = unknown");
	check(Parse::LoginFromNav(json(R"({"code":0,"data":{"isLogin":true}})"), mid, message) ==
		      Parse::LoginState::Unknown,
	      "nav: logged in without mid = unknown");
}

const char *kStartOk = R"({"code":0,"message":"0","data":{
  "change":"other","status":0,"room_type":0,"check":0,"prompts":0,"audit":0,"recv":0,
  "live_key":"SESSION-PAIRING-KEY","live_model":"","room_id":21452505,
  "protocols":[
    {"protocol":"bmtsrt","provider":"cn"},{"protocol":"bmt","provider":"cn"},
    {"protocol":"srt","provider":"txy","new_link":true,"addr":"rtmpsrt://txy.live-push.bilivideo.com/live-bvc/","code":"?streamname=abc&key=K1"},
    {"protocol":"srt","provider":"qn","new_link":true,"addr":"rtmpsrt://qn.live-push.bilivideo.com/live-bvc/?upbvc=1","code":"?streamname=abc&key=K2"},
    {"protocol":"rtmp","provider":"txy","new_link":true,"addr":"rtmp://txy.live-push.bilivideo.com/live-bvc/","code":"?streamname=abc&key=K3"},
    {"protocol":"rtmp","provider":"qn","new_link":true,"addr":"rtmp://qn.live-push.bilivideo.com/live-bvc/?upbvc=1","code":"?streamname=abc&key=K4"}
  ],
  "protocols_backup":null,"rtmp":{"new_link":true,"addr":"rtmp://txy.live-push.bilivideo.com/live-bvc/","code":"old"},
  "xcode_stream":null,"sub_session_key":"","need_face_auth":false,"qr":null,"notice":null,"risk_extra":null}})";

void test_start_live_ok()
{
	const Parse::StartLive r = Parse::StartLiveEnvelope(json(kStartOk), "1");
	check(r.outcome == Parse::StartOutcome::Ok, "start: ok");
	check(r.targets.size() == 4, "start: only entries with addr become lines");
	check(r.targets[0].protocol == "srt" && r.targets[0].provider == "txy" && !r.targets[0].backup &&
		      r.targets[0].key == "?streamname=abc&key=K1",
	      "start: line keeps its own code as stream key");
	check(r.targets[2].protocol == "rtmp" && r.targets[2].key == "?streamname=abc&key=K3", "start: rtmp line");
	check(r.rtmp_addr == "rtmp://txy.live-push.bilivideo.com/live-bvc/" &&
		      r.rtmp_key != "?streamname=abc&key=K1",
	      "start: compat rtmp copy is the first rtmp line (not the srt key)");
	check(r.rtmp_key == "?streamname=abc&key=K3", "start: compat rtmp key");
	check(r.primary_addr == "rtmpsrt://txy.live-push.bilivideo.com/live-bvc/" && r.primary_key == "?streamname=abc&key=K1",
	      "start: primary copy is the first line");
	check(r.face_qr.empty() && r.face_page.empty() && r.notice_line.empty() && r.voucher.empty(),
	      "start: no blocking payloads");

	// protocols[] 条目缺 code ⇒ 回落 live_key(契约 §4.1 第 5 步)。
	std::string with_keyless = kStartOk;
	with_keyless.replace(with_keyless.find(R"(,"code":"?streamname=abc&key=K1")"),
			     std::string(R"(,"code":"?streamname=abc&key=K1")").size(), "");
	const Parse::StartLive r2 = Parse::StartLiveEnvelope(json(with_keyless), "1");
	check(r2.outcome == Parse::StartOutcome::Ok && r2.targets[0].key == "SESSION-PAIRING-KEY",
	      "start: entry without code falls back to live_key");
}

void test_start_live_fallback_and_missing()
{
	// 第 6 步旧式回落:无 protocols[],rtmp.new_link 必须存在。
	const Parse::StartLive r = Parse::StartLiveEnvelope(
		json(R"({"code":0,"data":{"rtmp":{"new_link":true,"addr":"rtmp://a/live/","code":"C"}}})"), "1");
	check(r.outcome == Parse::StartOutcome::Ok && r.targets.size() == 1 && r.targets[0].protocol == "rtmp" &&
		      r.targets[0].addr == "rtmp://a/live/" && r.targets[0].key == "C",
	      "start: legacy rtmp.new_link fallback");
	const Parse::StartLive r2 = Parse::StartLiveEnvelope(
		json(R"({"code":0,"data":{"rtmp":{"addr":"rtmp://a/live/","code":"C"}}})"), "1");
	check(r2.outcome == Parse::StartOutcome::MissingFields, "start: rtmp without new_link = missing fields");
	check(Parse::StartLiveEnvelope(json(R"({"code":0,"data":{"protocols":[]}})"), "1").outcome ==
		      Parse::StartOutcome::MissingFields,
	      "start: empty protocols = missing fields");
	check(Parse::StartLiveEnvelope(json(R"({"code":0,"data":null})"), "1").outcome ==
		      Parse::StartOutcome::MissingFields,
	      "start: null data = missing fields");
	{
		// 非法 JSON:信封层解析失败后交给解析器的只会是 null Json。
		const Parse::StartLive broken = Parse::StartLiveEnvelope(json11::Json(), "1");
		check(broken.outcome == Parse::StartOutcome::MissingFields, "start: null envelope = missing fields");
	}
	// 备流:protocols_backup[] 与主流同构。
	const Parse::StartLive r3 = Parse::StartLiveEnvelope(
		json(R"({"code":0,"data":{"live_key":"L","protocols":[{"protocol":"rtmp","addr":"rtmp://p/","code":"P"}],
		        "protocols_backup":[{"protocol":"rtmp","addr":"rtmp://b/","code":"B"}]}})"),
		"1");
	check(r3.outcome == Parse::StartOutcome::Ok && r3.targets.size() == 2 && r3.targets[1].backup &&
		      r3.targets[1].key == "B" && r3.primary_key == "P",
	      "start: backup lines appended and flagged");
}

void test_start_live_blocks()
{
	std::string mid, message;
	// 第 12 步:成功分支的 notice 同样是弹窗阻断。
	const Parse::StartLive notice = Parse::StartLiveEnvelope(
		json(R"({"code":0,"data":{"protocols":[{"protocol":"rtmp","addr":"rtmp://p/"}],"notice":{"title":"标题违规","msg":"请修改"}}})"),
		"9");
	check(notice.outcome == Parse::StartOutcome::Blocked && notice.notice_line == "标题违规: 请修改",
	      "start: success-branch notice blocks");
	const Parse::StartLive voucher = Parse::StartLiveEnvelope(
		json(R"({"code":0,"data":{"protocols":[{"protocol":"rtmp","addr":"rtmp://p/"}],"risk_extra":{"v_voucher":"V"}}})"),
		"9");
	check(voucher.outcome == Parse::StartOutcome::Blocked && voucher.voucher == "V", "start: v_voucher blocks");
	const Parse::StartLive face = Parse::StartLiveEnvelope(
		json(R"({"code":0,"data":{"need_face_auth":true,"qr":"https://face/qr"}})"), "9");
	check(face.outcome == Parse::StartOutcome::NeedFaceQr && face.face_qr == "https://face/qr",
	      "start: need_face_auth on code 0");
	const Parse::StartLive c60024 =
		Parse::StartLiveEnvelope(json(R"({"code":60024,"message":"需要人脸","data":{"qr":"https://face/60024"}})"), "9");
	check(c60024.outcome == Parse::StartOutcome::NeedFaceQr && c60024.face_qr == "https://face/60024" &&
		      c60024.code == 60024,
	      "start: 60024 face QR");
	const Parse::StartLive c60043 = Parse::StartLiveEnvelope(json(R"({"code":60043,"message":"x","data":{}})"), "123");
	check(c60043.outcome == Parse::StartOutcome::NeedFacePage &&
		      c60043.face_page == "https://www.bilibili.com/blackboard/live/auth-middle.html?source_event=200&mid=123",
	      "start: 60043 auth-middle page carries mid");
	// 备流被拒(70002/70003)= 普通 Rejected,由 BiliApi 决定回退;其它业务错误同理。
	const Parse::StartLive c70002 =
		Parse::StartLiveEnvelope(json(R"({"code":70002,"message":"获取推流地址失败,请稍后重试"})"), "9");
	check(c70002.outcome == Parse::StartOutcome::Rejected && c70002.code == 70002 &&
		      c70002.server_message == "获取推流地址失败,请稍后重试",
	      "start: 70002 rejected with server message");
	const Parse::StartLive c101 = Parse::StartLiveEnvelope(json(R"({"code":-101,"message":"账号未登录"})"), "9");
	check(c101.outcome == Parse::StartOutcome::Rejected, "start: -101 rejected");
	const Parse::StartLive blocked_notice = Parse::StartLiveEnvelope(
		json(R"({"code":70003,"message":"x","data":{"notice":{"msg":"封禁"}}})"), "9");
	check(blocked_notice.outcome == Parse::StartOutcome::Blocked, "start: notice on error = blocked (no retry)");
}
} // namespace

int main()
{
	test_room_info();
	test_login_from_nav();
	test_start_live_ok();
	test_start_live_fallback_and_missing();
	test_start_live_blocks();
	std::printf(g_fail ? "\n%d passed, %d FAILED\n" : "\n%d passed, ALL PASS\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
