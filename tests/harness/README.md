# 无头集成 harness(真实 libobs + 真实 Qt + 假 B站服务端)

把**编译出来的插件 `.so`** 按 OBS 的方式装进真实 libobs(`obs_open_module` → `obs_init_module` →
`obs_shutdown`),用真实 Qt(`QT_QPA_PLATFORM=offscreen`)驱动侧栏:点按钮、读文案、处理确认框,
并对照假服务端的请求日志断言「发了什么、没发什么」。它补的是离线解析单测覆盖不到的那一层:
线程(UI 续体、worker join)、控件生命周期(重建/销毁/模态框)、状态机迁移、请求序列。
**不替代真机**:停靠布局、主题、HiDPI、真实扫码仍要在用户 OBS 上验收。

## 组成

| 文件 | 作用 |
|---|---|
| `headless_main.cpp` | 可执行体 `bili_harness`。扮演 OBS 前端:导出插件用到的全部 `obs_frontend_*`(dock = 右侧停靠区的 `QDockWidget`,照抄 OBS 32.2 `OBSStudioAPI.cpp`;事件回调按注册序派发;`obs_frontend_open_source_properties` 把属性表构建一遍再释放,并挂上 `update_properties` 信号模拟 PropertiesView 的重建) |
| `fake_bilibili_server.py` | 假 B站:HTTPS CONNECT 代理 + TLS 卸载(一张测试证书,SAN 覆盖全部相关主机)+ 按「方法 + 主机 + 路径」的应答脚本。状态(是否在播)由 startLive/stopLive 真实翻转;`script.json` 控制异常分支(-412、70002、60034、分区接口 500…);每个请求写一行 JSON 到 `requests.log` |
| `libcurl_test_ca_shim.c` | `LD_PRELOAD` 垫片 `bili_test_ca_shim.so`:libcurl **库 API** 不读 `SSL_CERT_FILE`(只有 curl 命令行读),CA 路径是编译期固定;垫片拦截 `X509_STORE_load_file`,在系统 CA 之外追加测试 CA。被测插件代码零改动 |
| `run_headless.sh` | 入口:起服务端、逐场景跑 harness(每个场景一个全新进程,libobs 一生只能 `obs_startup` 一次)、汇总,并扫描 OBS 日志确认推流码/会话凭据没有落盘 |

## 构建

```bash
cmake --preset ubuntu-x86_64-debug   # 或 Release;CMakePresets 的 ubuntu-ci-x86_64 已默认带 BUILD_HARNESS
cmake --build --preset ubuntu-x86_64-debug -- -j8
```

harness 目标只在 `-DBUILD_HARNESS=ON` 时构建(CI 的 `ubuntu-ci-x86_64` preset 已带),不入 CTest。

## 运行

```bash
# 需要 X display(libobs 的热键子系统在 obs_startup 时连 X)、python3、openssl、nm
xvfb-run -a tests/harness/run_headless.sh build_x86_64_debug/tests/bili_harness build_x86_64_debug/bilibili-stream-for-obs.so [场景…]
```

退出码 = 失败场景数。`KEEP_WORK=1` 保留工作目录(请求日志、harness stdout/OBS 日志)排查。

### 场景

| 场景 | 覆盖 |
|---|---|
| `startup_logged_out` | 无 Cookie:未登录态、只做一次 buvid 引导、不发 nav |
| `login_get_key_close` | 扫码登录(86101/86090/0 序列、SSO 只一次、每次刷新只一个 room/GetInfo、非 B站域不带 Cookie)→ 获取推流码(无资格不发备流参数、公共参数/签名/服务端回填的分区)→ 线路与「复制」→ 对账为直播中 → 关闭直播间(确认框带房间号、服务端标题按纯文本)→ 对账为未开播 |
| `close_cancel` | 确认框点取消:不发 stopLive,线路保留 |
| `streaming_guard` | OBS 推流中:只有「确定」的警告框,不发 stopLive |
| `network_error_keeps_login` | nav 返回 -412:登录态保持、按钮可用 |
| `recover` | 服务端在播但本地无线路:找回推流码(首选源一次) |
| `stop_rejected` | stopLive 回 60034:策略文案透出、线路保留、可重试 |
| `stale_lines_cleared` | 服务端未开播而本地留着上一场线路:对账后清掉 |
| `exit_then_unload` | `OBS_FRONTEND_EVENT_EXIT`:dock 摘除、事件回调注销,随后卸载干净 |
| `first_run_shows_dock` | 首次加载 `FINISHED_LOADING`:dock 自动显示并停靠 |
| `backup_fallback` | 有备流资格(=5):先发备流参数,70003 后回退普通开播 |
| `partition_fail_no_loop` | 分区接口 500:只请求一次,属性表不重建循环 |
| `unload_while_login_polling` | 登录轮询进行中直接卸载:秒级返回,不崩 |

### ASan/UBSan

```bash
cmake -S . -B build_asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_HARNESS=ON -DENABLE_FRONTEND_API=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build_asan -j8 && tests/harness/run_headless.sh build_asan/tests/bili_harness build_asan/bilibili-stream-for-obs.so
```

`run_headless.sh` 检测到 ASan 插桩的 harness 会把 `libasan.so` 排在 `LD_PRELOAD` 最前,并把任何
Sanitizer 报告计为失败(默认 `detect_leaks=1`;插件故意泄漏的两个堆块被全局引用持有,不算泄漏)。

## 没有系统 OBS 的机器怎么准备环境(2026-11-27 在一台无桌面的构建机上验证过)

不装任何系统包,全部解包到仓库里被 `.gitignore` 忽略的 `.deps/`(以 Ubuntu 24.04 为例):

```bash
# 1) 头面:buildspec 钉死的 OBS 32.2.0 源码包(同 CI,cmake/linux/buildspec.cmake 的解包目标)
#    https://github.com/obsproject/obs-studio/archive/refs/tags/32.2.0.zip → .deps/obs-studio-32.2.0/
# 2) 链接面:发行版包解包到 .deps/root/usr(qt6-base-dev qt6-base-dev-tools qmake6 libqt6{core,gui,widgets,dbus}6t64
#    libgl-dev libglx-dev libopengl-dev libegl-dev libobs-dev libobs0t64 libjansson-dev libjansson4
#    libsrt-gnutls-dev libsrt1.5-gnutls libmbedtls-dev libmbedtls14t64 libmbedx509-1t64 libsimde-dev)
#    用 `apt-get download <pkg>` + `dpkg -x <deb> .deps/root`。
# 3) 运行时用真 OBS 32.2.0 的 libobs(PPA 的 obs-studio_32.2.0-0obsproject1~noble 单包)解包到 .deps/obs322/root
#    —— 30.0.2 的 libobs 在 obs_startup 就要 X,32.2 不需要;删掉包里的 obs-plugins/CEF 省磁盘。
# 4) Xvfb(发行版包解包到 .deps/xvfb/usr/bin)给 libobs 热键子系统一个 display。
```

```bash
PREFIX=$PWD/.deps/root/usr
CI=true cmake -S . -B build_x86_64 -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DENABLE_FRONTEND_API=ON \
  -DBUILD_HARNESS=ON -DCMAKE_PREFIX_PATH=$PREFIX "-DCMAKE_C_FLAGS=-I$PREFIX/include" "-DCMAKE_CXX_FLAGS=-I$PREFIX/include" \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=""
cmake --build build_x86_64 -j8 && ctest --test-dir build_x86_64
LD_LIBRARY_PATH=$PWD/.deps/obs322/root/usr/lib/x86_64-linux-gnu:$PREFIX/lib/x86_64-linux-gnu \
  DISPLAY=:99 tests/harness/run_headless.sh build_x86_64/tests/bili_harness build_x86_64/bilibili-stream-for-obs.so
```

坑(都踩过):发行版 cmake 配置按自身位置重定位,但解包的运行库与 `-l` 解析有先后——换过包要删构建目录重配;
`make` 输出别丢 `/dev/null`;`libcurl` 8.5 + OpenSSL 3 的 CA 装载走 `X509_STORE_load_file`(垫片拦截点),
换发行版若失效先确认这一点;工程全局 `-fvisibility=hidden`,垫片里要 `__attribute__((visibility("default")))`。
