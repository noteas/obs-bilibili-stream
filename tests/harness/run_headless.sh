#!/usr/bin/env bash
# 无头集成测试入口(见 tests/harness/README.md):
#   cmake -S . -B build -DBUILD_HARNESS=ON … && cmake --build build
#   tests/harness/run_headless.sh build/bili_harness build/bilibili-stream-for-obs.so [场景…]
# 起假 B站服务端(HTTPS 代理 + TLS 卸载),逐场景跑 bili_harness(每个场景一个全新进程:
# 真实 libobs 一生只能 obs_startup 一次),汇总结果,并扫描 OBS 日志确认推流码/会话凭据
# 没有落盘。退出码 = 失败场景数(0 = 全过)。
set -u
HARNESS="${1:?usage: run_headless.sh <bili_harness> <plugin.so> [scenario…]}"
PLUGIN="${2:?plugin .so path}"
shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d /tmp/bili-headless.XXXXXX)"
# KEEP_WORK=1 保留工作目录(排查用)。
trap 'kill "${SERVER_PID:-0}" 2>/dev/null; [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"' EXIT

# ---- 测试 CA + 证书(SAN 覆盖插件会访问的全部主机) ----
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=bilibili-headless-test" \
  -addext "subjectAltName=DNS:api.bilibili.com,DNS:api.live.bilibili.com,DNS:live.bilibili.com,DNS:passport.bilibili.com,DNS:passport.biligame.com,DNS:www.bilibili.com,DNS:not-bilibili.example.com,DNS:*.bilibili.com,DNS:*.biligame.com,DNS:*.live.bilibili.com" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1 || { echo "openssl failed"; exit 2; }

PORT=$((20000 + RANDOM % 20000))
python3 "$HERE/fake_bilibili_server.py" "$WORK" "$PORT" 2>"$WORK/server.err" &
SERVER_PID=$!
for _ in $(seq 50); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then exec 3>&-; break; fi
  sleep 0.1
done

# libobs 的热键子系统在 obs_startup 时连 X(取键盘映射),没有 X display 直接失败;
# 需要 Xvfb 的机器由调用方先起(例: Xvfb :99 …; DISPLAY=:99 本脚本)。
export QT_QPA_PLATFORM=offscreen
export https_proxy="http://127.0.0.1:$PORT" HTTPS_PROXY="http://127.0.0.1:$PORT"
# libcurl 库 API 不读 SSL_CERT_FILE:用 LD_PRELOAD 垫片把测试 CA 追加进信任库。
SHIM="$(dirname "$HARNESS")/bili_test_ca_shim.so"
[ -f "$SHIM" ] || { echo "missing $SHIM (build with -DBUILD_HARNESS=ON)"; exit 2; }
export BILI_TEST_CA="$WORK/cert.pem"
PRELOAD_LIST="$SHIM"
# ASan 插桩的构建:libasan 必须排在 LD_PRELOAD 最前,否则运行库拒绝启动。
if nm -D "$HARNESS" 2>/dev/null | grep -q "__asan_init"; then
  ASAN_RT="$(gcc -print-file-name=libasan.so 2>/dev/null || true)"
  [ -n "$ASAN_RT" ] && PRELOAD_LIST="$ASAN_RT $SHIM"
  HARNESS_ENV+=( "ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
                 "UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" )
fi
# LD_PRELOAD 只给被测进程:预装到 tail/grep 这类未插桩的子进程会让 ASan 直接 abort。
HARNESS_ENV+=( "LD_PRELOAD=${LD_PRELOAD:+$LD_PRELOAD }$PRELOAD_LIST" )
export BILI_TRACE_HTTP="${BILI_TRACE_HTTP:-}"   # 默认关:测试「日志不落敏感数据」
unset http_proxy HTTP_PROXY

SCENARIOS=("$@")
if [ ${#SCENARIOS[@]} -eq 0 ]; then
  SCENARIOS=(startup_logged_out login_get_key_close close_cancel streaming_guard network_error_keeps_login
             recover stop_rejected exit_then_unload first_run_shows_dock short_dock_scrolls backup_fallback
             partition_fail_no_loop stale_lines_cleared empty_title_keeps_local
             unload_while_login_polling)
fi
failed=0
for sc in "${SCENARIOS[@]}"; do
  cfg="$WORK/$sc/obs-config"; log="$WORK/requests.log"; mkdir -p "$cfg"
  : > "$log"
  env "${HARNESS_ENV[@]}" "$HARNESS" "$PLUGIN" "$ROOT/data" "$cfg" "$log" "$sc" > "$WORK/$sc.out" 2> "$WORK/$sc.log"
  rc=$?
  tail -1 "$WORK/$sc.out"
  if [ $rc -ne 0 ]; then failed=$((failed + 1)); echo "  -> FAIL (rc=$rc); see $WORK/$sc.out $WORK/$sc.log"; cp "$WORK/$sc.out" "/tmp/bili-harness-$sc.out" 2>/dev/null; cp "$WORK/$sc.log" "/tmp/bili-harness-$sc.log" 2>/dev/null; fi
  if grep -q -E "ERROR: AddressSanitizer|runtime error: |LeakSanitizer|ERROR: UndefinedBehaviorSanitizer" "$WORK/$sc.out" "$WORK/$sc.log"; then
    failed=$((failed + 1)); echo "  -> SANITIZER finding in $sc:"; grep -h -E -m6 "ERROR: AddressSanitizer|runtime error: |SUMMARY|Direct leak|ERROR: UndefinedBehaviorSanitizer" "$WORK/$sc.out" "$WORK/$sc.log"
  fi
  # OBS 日志(libobs 默认打到 stderr)里不得出现任何推流码/会话凭据明文。
  if grep -q -E "K-SRT|K-RTMP|K-BACKUP|K-RECOVERED|key=RECOVERED|sess-fake|poll-secret|RT-SECRET|PAIR-KEY|live42&key" "$WORK/$sc.log"; then
    failed=$((failed + 1)); echo "  -> LEAK in $sc.log:"; grep -E "K-SRT|K-RTMP|K-BACKUP|key=RECOVERED|sess-fake|poll-secret|RT-SECRET|PAIR-KEY|live42&key" "$WORK/$sc.log" | head -5
  fi
done
echo "headless harness: $(( ${#SCENARIOS[@]} - failed ))/${#SCENARIOS[@]} ok (workdir $WORK)"
[ $failed -eq 0 ]
