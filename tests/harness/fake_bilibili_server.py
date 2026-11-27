#!/usr/bin/env python3
"""无头集成测试用的假 B站服务端(tests/harness,不进 CTest 默认集)。

形态 = 「HTTPS 代理 + TLS 卸载 + 应答脚本」:被测插件经环境变量 https_proxy 把全部
请求以 CONNECT 送进来,本程序回 200 后用一张测试证书( SAN 覆盖全部相关主机)与插件
做 TLS,再按「方法 + 主机 + 路径」返回契约形状的 JSON。插件侧只多认一个环境变量
SSL_CERT_FILE(信任测试 CA),生产代码零改动。

控制面(文件,测试进程写、服务端每个请求现读):
  <dir>/script.json   应答脚本(见 DEFAULT_SCRIPT);缺省 = 全部成功
  <dir>/state.json    服务端内部状态(是否在播),「reset: true」的脚本会清零
  <dir>/requests.log  每个请求一行 JSON(方法/主机/路径/查询/请求体/响应码),供断言
用法: fake_bilibili_server.py <dir> <port>
"""
import json
import os
import socket
import ssl
import sys
import threading
from http.server import BaseHTTPRequestHandler
from socketserver import ThreadingTCPServer
from urllib.parse import urlsplit, parse_qsl

DEFAULT_SCRIPT = {
    "reset": False,
    "nav_code": 0,            # 0 = 已登录;-101 = 未登录;-412 = 风控(结果未知)
    "room_live": "auto",      # "auto" 跟随内部状态,或 0/1 强制
    "poll_sequence": [86101, 86101, 86090, 0],  # 轮询 data.code 序列,末项重复
    "visible_mode": 0,        # GetVisibleMode.upower_privilege_type
    "start_codes": [],        # startLive 依次返回的 code(耗尽后返回 0);空 = 恒 0
    "stop_code": 0,           # stopLive 的返回 code(如 60034)
    "partition_fail": False,  # Area/getList 返回 HTTP 500
    "getinfo_fail_once": False,
}

STATE = {"live": False, "poll_i": 0, "start_i": 0, "getinfo_failed": False, "lock": threading.Lock()}


def load(path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FakeBili/1.0"

    def log_message(self, fmt, *args):  # 静默:请求日志自己写
        pass

    # ------------------------------------------------------------------ utils
    @property
    def dir(self):
        return self.server.state_dir

    def script(self):
        s = dict(DEFAULT_SCRIPT)
        s.update(load(os.path.join(self.dir, "script.json"), {}))
        if s.get("reset"):
            with STATE["lock"]:
                STATE.update(live=False, poll_i=0, start_i=0, getinfo_failed=False)
            s["reset"] = False
            with open(os.path.join(self.dir, "script.json"), "w", encoding="utf-8") as f:
                json.dump(s, f)
        return s

    def send_json(self, obj, status=200, cookies=(), headers=()):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for c in cookies:
            self.send_header("Set-Cookie", c)
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        return status

    def redirect(self, location, cookies=()):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        for c in cookies:
            self.send_header("Set-Cookie", c)
        self.end_headers()
        self.wfile.flush()
        return 302

    def log_request(self, status, request_size=None):  # 由 send_response() 调用,每个响应一次
        rec = {
            "method": self.command,
            "host": self.headers.get("Host", ""),
            "path": self.path,
            "cookie": self.headers.get("Cookie", ""),
            "body": self.body if self.body is not None else None,
            "status": status,
        }
        with self.server.log_lock:
            with open(os.path.join(self.dir, "requests.log"), "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    # -------------------------------------------------------------- dispatch
    def do_GET(self):
        self.body = None
        self.dispatch()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        self.dispatch()

    def dispatch(self):
        try:
            self.route()
        except BrokenPipeError:
            pass
        except Exception as e:  # 测试服务端不能因为一个异常拖垮整个套件
            sys.stderr.write("fake server error: %r\n" % (e,))
            try:
                self.send_json({"code": -500, "message": "fake server error"}, 500)
            except Exception:
                pass

    def route(self):
        u = urlsplit(self.path)
        host = (self.headers.get("Host") or "").split(":")[0]
        path = u.path
        q = dict(parse_qsl(u.query))
        s = self.script()
        R = self.send_json

        # ---- 指纹 / 版本 ---------------------------------------------------
        if path == "/x/frontend/finger/spi":
            return R({"code": 0, "data": {"b_3": "BUVID3-fake-device-0001", "b_4": "BUVID4-fake"}})
        if path == "/xlive/app-blink/v1/liveVersionInfo/getHomePageLiveVersion":
            return R({"code": 0, "data": {"build": 11349, "curr_version": "8.16.0.11349"}})

        # ---- 登录 -----------------------------------------------------------
        if path == "/x/passport-login/web/qrcode/generate":
            return R({"code": 0, "data": {"url": "https://passport.bilibili.com/h5/qr?k=FAKE%d" % STATE["poll_i"],
                                           "qrcode_key": "FAKEKEY"}})
        if path == "/x/passport-login/web/qrcode/poll":
            seq = s["poll_sequence"]
            with STATE["lock"]:
                i = STATE["poll_i"]
                STATE["poll_i"] += 1
            code = seq[min(i, len(seq) - 1)]
            if code == 0:
                return R({"code": 0, "data": {"code": 0, "url": "https://passport.biligame.com/x/passport-login/web/crossDomain?ticket=T1&gourl=https%3A%2F%2Fwww.bilibili.com",
                                               "refresh_token": "RT-SECRET", "timestamp": 1}},
                         cookies=["SESSDATA=poll-secret; Path=/; Domain=.bilibili.com; HttpOnly",
                                  "bili_jct=poll-csrf; Path=/; Domain=.bilibili.com"])
            msg = {86101: "未扫码", 86090: "已扫码未确认", 86038: "二维码已失效"}.get(code, "?")
            return R({"code": 0, "data": {"code": code, "message": msg}})
        if path == "/x/passport-login/web/crossDomain":
            return self.redirect("https://www.bilibili.com/",
                                 ["SESSDATA=sess-fake-1234567890; Path=/; Domain=.bilibili.com; HttpOnly",
                                  "bili_jct=csrf-fake-1234567890; Path=/; Domain=.bilibili.com",
                                  "DedeUserID=42; Path=/; Domain=.bilibili.com"])
        if path == "/x/passport-login/web/sso/list":
            return R({"code": 0, "data": {"sso": ["https://passport.biligame.com/x/passport-login/web/sso?sso=1",
                                                  "https://not-bilibili.example.com/x/passport-login/web/sso?sso=2"]}})
        if path == "/x/passport-login/web/sso":
            return self.redirect("https://passport.bilibili.com/x/passport-login/web/sso/ok", ["sso_ok=1; Path=/"])

        # ---- nav / 房间 ------------------------------------------------------
        if path == "/x/web-interface/nav":
            cookies = self.headers.get("Cookie", "")
            logged = "SESSDATA=" in cookies and "sess-fake" in cookies or "poll-secret" in cookies
            if not logged or s["nav_code"] != 0:
                code = s["nav_code"] if s["nav_code"] != 0 else -101
                return R({"code": code, "message": "账号未登录" if code == -101 else "请求被拦截", "data": {"isLogin": False}})
            return R({"code": 0, "message": "0", "data": {"isLogin": True, "mid": 42, "uname": "tester"}})
        if path == "/xlive/app-blink/v1/room/GetInfo":
            if s.get("getinfo_fail_once") and not STATE["getinfo_failed"]:
                STATE["getinfo_failed"] = True
                return R({"code": -500, "message": "server busy"}, 500)
            live = STATE["live"] if s["room_live"] == "auto" else bool(s["room_live"])
            return R({"code": 0, "message": "0", "data": {
                "room_id": 21452505, "live_status": 1 if live else 0, "title": "测试直播间<b>&amp;",
                "lock_status": 0, "lock_time": 0, "parent_id": 2, "area_v2_id": 86, "area_v2_name": "电竞"}})
        if path == "/room/v1/Area/getList":
            if s["partition_fail"]:
                return R({"code": -500, "message": "boom"}, 500)
            return R({"code": 0, "data": [{"id": 2, "name": "游戏", "list": [{"id": 86, "name": "电竞"}, {"id": 87, "name": "单机"}]},
                                          {"id": 1, "name": "娱乐", "list": [{"id": 21, "name": "聊天"}]}]})

        # ---- 开播前 --------------------------------------------------------
        if path == "/xlive/app-blink/v1/preLive/PreCheck":
            return R({"code": 0, "data": {"realname_status": 1}})
        if path == "/xlive/app-blink/v1/index/GetRoomPreLiveStatus":
            return R({"code": 0, "data": {"status": 0}})
        if path == "/xlive/app-blink/v2/preLive/GetVisibleMode":
            return R({"code": 0, "data": {"visible_mode": 0, "guard_visible_type": 0, "upower_privilege_type": s["visible_mode"]}})
        if path == "/live_stream/v1/StreamList/get_stream_by_roomId":
            if not STATE["live"] and s["room_live"] != 1:
                return R({"code": 0, "data": {"rtmp": None, "stream_line": []}})
            return R({"code": 0, "data": {"rtmp": {"addr": "rtmp://dl.live-push.bilivideo.com/live-bvc/", "code": "?streamname=live42&key=RECOVERED"},
                                          "stream_line": [{"src": 8, "cdn_name": "txy", "checked": True}]}})
        if path == "/xlive/app-blink/v1/live/GetUpStreamRtmp":
            return R({"code": 0, "data": {"can_change": True, "urls": ["rtmp://up.live-push.bilivideo.com/live-bvc/"], "message": ""}})

        # ---- 开播 / 停播 ----------------------------------------------------
        if path == "/room/v1/Room/startLive":
            if q.get("backup_stream") == "1" and not s["start_codes"]:
                # 默认:只有带备流参数才「分配失败」,用来验证回退普通开播
                code = 70002
            else:
                codes = s["start_codes"]
                with STATE["lock"]:
                    i = STATE["start_i"]
                    STATE["start_i"] += 1
                code = codes[i] if i < len(codes) else 0
            if code != 0:
                return R({"code": code, "message": "获取推流地址失败,请稍后重试", "data": {}})
            with STATE["lock"]:
                STATE["live"] = True
            return R({"code": 0, "message": "0", "data": {
                "room_type": 0, "need_face_auth": False, "live_key": "PAIR-KEY-NOT-FOR-STREAM",
                "protocols": [
                    {"protocol": "srt", "provider": "txy", "new_link": True, "addr": "rtmpsrt://txy.live-push.bilivideo.com/live-bvc/", "code": "?streamname=live42&key=K-SRT"},
                    {"protocol": "rtmp", "provider": "txy", "new_link": True, "addr": "rtmp://txy.live-push.bilivideo.com/live-bvc/", "code": "?streamname=live42&key=K-RTMP"},
                    {"protocol": "bmt", "provider": "cn"},
                ],
                "protocols_backup": [{"protocol": "rtmp", "provider": "qn", "addr": "rtmp://qn.live-push.bilivideo.com/live-bvc/", "code": "?streamname=live42&key=K-BACKUP"}],
                "rtmp": {"new_link": True, "addr": "rtmp://legacy/live/", "code": "legacy"}, "notice": None, "risk_extra": None}})
        if path == "/room/v1/Room/stopLive":
            if s["stop_code"]:
                return R({"code": s["stop_code"], "message": "系统维护仅支持直播姬关播", "data": {}})
            with STATE["lock"]:
                STATE["live"] = False
            return R({"code": 0, "message": "0", "data": {}})

        # ---- 资料 / 质量 ---------------------------------------------------
        if path == "/xlive/app-blink/v1/preLive/UpdatePreLiveInfo":
            return R({"code": 0, "message": "0"})
        if path == "/xlive/app-blink/v2/room/AnchorChangeRoomArea":
            return R({"code": 0, "message": "0"})
        if path == "/xlive/app-blink/v1/upStreamConfig/GetUpStreamVideoQualityConfig":
            return R({"code": 0, "data": {"bitrate_config": {"default_bitrate": 3000,
                     "recommend_bitrate_member_intervention": None,
                     "recommend_bitrate_area_intervention": {"recommend_bitrate_area_v2_interventions": [
                         {"area_v2_ids": [86], "recommend_bitrate": 6000, "strategy_source": "high"}]}}}})
        if path == "/xlive/app-blink/v1/upStreamConfig/CanUseH265Encoder":
            return R({"code": 0, "data": {"can_enable": True}})
        if path == "/xlive/app-blink/v1/upStreamConfig/CanUseAV1Encoder":
            return R({"code": 0, "data": {"can_hard_encode": False}})

        return R({"code": -404, "message": "fake server: no route for %s %s" % (self.command, path)}, 404)


class ProxyServer(ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, state_dir, ssl_ctx):
        super().__init__(addr, None)
        self.state_dir = state_dir
        self.ssl_ctx = ssl_ctx
        self.log_lock = threading.Lock()

    def handle_error(self, request, client_address):
        # 客户端拿到响应后直接 RST/关闭是常态(libcurl 每请求一个新 easy handle)
        pass

    def finish_request(self, request, client_address):
        # 1) 读 CONNECT;2) 回 200;3) TLS 握手;4) 按普通 HTTP 处理(一个请求一连接)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = request.recv(4096)
            if not chunk:
                return
            buf += chunk
        first = buf.split(b"\r\n", 1)[0].decode("latin-1")
        if not first.upper().startswith("CONNECT "):
            request.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return
        request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        try:
            tls = self.ssl_ctx.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError) as e:
            sys.stderr.write("tls handshake failed: %r\n" % (e,))
            return
        self.process_request_tls(tls, client_address)

    def process_request_tls(self, sock, addr):
        handler = Handler(sock, addr, self)
        try:
            handler.handle()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass
        finally:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    state_dir, port = sys.argv[1], int(sys.argv[2])
    cert = os.path.join(state_dir, "cert.pem")
    key = os.path.join(state_dir, "key.pem")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    server = ProxyServer(("127.0.0.1", port), state_dir, ctx)
    sys.stderr.write("fake bilibili server listening on 127.0.0.1:%d\n" % port)
    sys.stderr.flush()
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main() or 0)
