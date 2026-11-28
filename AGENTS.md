# obs-bilibili-stream

OBS 插件:代理 B站直播的**信令面**(开播/停播/恢复/质量协商),推流数据面归 OBS 本身。协议行为基准 = 官方直播姬 LiveHime 8.16 二进制逆向结论,一切协议问题以端点契约为准绳。

## 语言

代码与文档一律用 [GLOSSARY.md](GLOSSARY.md) 的规范术语:开播/停播专指服务端信令,推流专指 OBS 输出;备流是主备双通道(插件只展示线路,不驱动第二路);联播、连麦、ARTC/WHIP 实施明确不做;插件不改写 OBS 配置,只展示线路与推流码。

## 仓库地图

- `src/bilibili_api.*` — 全部端点的 builder。一个端点 = 一个函数;参数注入只走统一管线(appsign/csrf/公共参数,契约 §1),不在端点内手写签名或手拼参数
- `src/bili_parsers.*` — 响应解析的纯函数层(无 I/O、无 obs 依赖):nav 三态、room/GetInfo、startLive 信封解析序;`tests/bili_parsers_test.cpp` 喂契约形状的 fixtures
- `src/http_client.*` — libcurl 传输 + cookie jar;请求头/指纹的唯一出口。单 worker 线程,卸载时中止在途请求并 **join**(模块随 dlclose 卸掉,不能留线程跑在自己的代码里)
- `src/core/session.*` — **运行期状态的唯一所有者**(登录态/房间态快照/线路表/忙碌/分区表);一切 UI 只是它的 Listener 视图。「是否在播」以服务端 room/GetInfo 为准
- `src/plugin-main.cpp`、`src/ui/` — UI:`console.*` = 隐藏私有 source「控制台」的 obs_properties(配置界面,Qt-free);`dock.*` = 状态侧栏(Qt Widgets,ADR-0010,**工程里唯一包含 Qt 头的翻译单元**,不定义 Q_OBJECT 免 moc)。两处都不写业务状态,只读 Session、调 Session
- `src/md5`、`src/json11`、`src/qrcodegen` — vendored,保持不动
- `third_party/obs-rtmp-srt` — git submodule,own fork(基线 = 上游 OBS 32.2.0 的 `librtmp` + `rtmp-stream`/`flv-mux` 逐字导入)。rtmpsrt 重实现的载体:**RTMP/FLV/AMF 逻辑零改动,只在 socket 层加 SRT 分支**,SRT 改造全部落在本 fork、不在主仓手搓协议(见 ADR-0008、E-27)。第三方依赖一律 `find_package` → imported target(fork 的 `cmake/finders/` = OBS 32.2 官方 Find 模块,libsrt 仅加 Debian 名扩展);禁止裸路径变量/裸 `-l` 链接
- `docs/api/livehime-api.md` — **端点契约**:增/改任何请求参数、请求头、响应字段前必读,那里有每个字段的注入条件与官方解析顺序;契约里没有的字段 = 先补逆向再实现,不许猜
- `docs/roadmap.md` — 里程碑与缺口状态;每个里程碑收尾必须同步它
- `docs/adr/` — 架构决策。提出与任一 ADR 相左的方案前先读对应 ADR
- `tests/` — 三层离线验证:`ctest`(srt_target/srt_catalog/bili_parsers 解析);`tests/harness`(无头集成:真实 libobs + 真实 Qt offscreen + 假 B站服务端,见 `tests/harness/README.md`);`tests/check_qt_symbols.py`(Qt 符号 ⊆ allowlist,ADR-0010;Windows 另并入 `qt-symbol-allowlist-msvc.txt`)
- `re/` — 本地逆向档案,**不入 git**(Ghidra 反编译原文、证据 E-xx/F-地址、分析脚本)。契约中的证据编号只在这份档案里可解析

## 工具

Ghidra 12.2: /home/zxd/tools/ghidra_12.2_PUBLIC，全套套件，含文档。使用Headless模式，操作时不要凭记忆，参考套件内自带的离线文档！

你可以用本机的其他工具，但不要安装工具，除非必须。在这种情况下，你需要暂停，由我自己安装。

注意:Ghidra 与 `re/` 逆向档案只存在于主开发机;其他开发机(含无桌面环境的构建机)上没有。没有档案时**只能引用契约里已有的结论**,不得新增端点/字段/参数,需要新逆向就停下来告诉用户。

### Ghidra 使用准则（2026-11-26 确立，详见 `re/GHIDRA-NOTES.md`）

- **第一准则：系统思维，禁止死磕汇编（2026-11-26 用户红线）**。反编译可以、看汇编可以，但**陷入复杂局面时绝不允许逐指令死磕**——死磕是没用的，你死磕不了。每次卡住，按此清单系统反思，而不是继续往下钻：
  1. **有必要吗**——现在的粒度（指令级/函数级/类级）是不是解决问题的最小粒度？能否先跳到更高维度？
  2. **有没有更好的方法**——Ghidra 的便利功能是不是全用上了：官方增值脚本（RecoverClassesFromRTTIScript / StringParameterPropagator / Decompiler Parameter ID）、反编译器（读高语言视图而非汇编）、xref（数据/代码引用）、Data Type Manager（结构体字段）、字符串？
  3. **全量分析的产出用上了没有**——完整的分析管线已产出字符串、RTTI 类名、vftable、参数类型、结构体。出现 `UNDEFINED`/`FUN_xxx`/`undefined8` 是分析没吃透的信号，先补分析（RTTI 类恢复等官方脚本），不是自己拿汇编硬猜。
  4. **我目前掌握了哪些信息**——先把已知事实清点出来（字符串目录、类名、URL、符号表命中），从已知推向未知，而不是跳进未知啃指令。
  5. **有没有其他突破口**——字符串、RTTI 字符串、源码路径、URL、已知类的方法、别的入口函数。换一个突破口常常比死磕快一个量级。
- **二进制内嵌源码路路径 = 头号突破口**：LiveHime/secret 的二进制内嵌 `..\..\..\bililive\secret\...\*.h:行号` 与 `*.c*:行号`（LOG/CHECK/assert 宏的 `__FILE__`/`__LINE__`）及完整端点 URL 字符串。**这是官方从来不公开、但二进制自带的"目录册"**——遇到不认识的功能或函数，先 `strings` 全量 dump 源码路径/文件名清单，用它定位函数归属与语义，比反编译快一个量级。
- **废弃旧打法**：不要再「复用旧工程 + `-noanalysis` + 定向 DumpVAs」一条路走到黑——它跳过整个分析管线，产物里全是 `UNK_*`、`FUN_xxx`、`undefined8`，字符串/RTTI/vftable/参数全没建出来。
- **工程粒度 = 整个直播姬**：livehime.exe / bililive.dll / bililive_secret.dll 是**同一程序的三件套**，一律放**同一个 Ghidra 工程** `re/ghidra_full`（livehime_full）内、每个二进制一个 Program——**不要按 DLL 拆工程**（跨 DLL 虚表槽位/符号/调用点对照只有同工程才顺手）。bililive.dll 于 11-26 全分析完成；bililive_secret.dll 原被误拆到 `ghidra_secret`，2026-11-26 并入联合工程重新全分析；livehime.exe 待并入。
- **新二进制第一件事 = 完整分析**：`-import` 进统一工程（默认开全部分析器），用 `-analysisTimeoutPerFile`、`-max-cpu` 兜底，并**显式给堆**——headless 默认仅 2G，70MB 级必 OOM（用 `GHIDRA_HEADLESS_MAXMEM=16G`，实测 70MB 级一次 20~40 分钟；35MB 级如 secret.dll 8G 堆约 8 分钟足够）。
- **全分析之后用官方增值脚本**：`RecoverClassesFromRTTIScript.java`（RTTI→类/方法名，能把 `FUN_180d5c120` 变成 `LivehimeLiveRoomController::OpenLiveRoom`）、Decompiler Parameter ID、`StringParameterPropagator.java` 等；DumpVAs 只作增量抽查。
- **无 PDB(2026-11-26 复核)**：三条主 PE 的 Debug Directory **有** CodeView/RSDS 条目(GUID+age+`*.pdb` 名,指纹已记 GHIDRA-NOTES §3.1),但 PDB 文件从不随包分发、私有构建无处可取——按「拿不到符号文件」处理,符号恢复靠 RTTI + 字符串 + 全分析,别浪费时间找 PDB。
- **RTTI 即证据**：`.?AV<lambda_N>@?0??Method@Class@@...@Z@` 直接给出真实 C++ 方法签名，遇到先 strings 全量扫一遍。

## 行为准则

- 文档优先：Ghidra, OBS等第三方软件的使用方法/API以最新官方文档为准，你可以在线检索，不只凭记忆；Bilibili（B站）文档除外，不要参考官方文档，只以逆向结论为准，因为官方不公开。
- 重要发现，重要结果，重要结论第一时间固化，为后续工作提供基础。
- 未定项在动码前先确认
- 真实开播类调用(startLive/stopLive/恢复)**仅在用户在场并明确同意时**发起;平时验证 = 离线解析测试 + dry-run 日志
- 每项独立改动原子提交;文档、注释时刻与代码同步
- **日志零凭据**:任何情况下 OBS 日志不得出现推流码、live_key、登录 ticket/refresh_token、Cookie、完整响应体(只记 HTTP 状态 + code/message;传输失败记主机名 + curl 原因,不记 URL 查询串)。排查契约问题要原文时由用户显式设 `BILI_TRACE_HTTP=1`,且提醒不要外传该日志
- **UI 只读 Session**:新增界面状态一律进 `Core::Session`,视图(控制台/侧栏)不持有业务状态;Qt 只允许出现在 `src/ui/dock.cpp`;不要在控件信号的调用栈里 delete 控件(重建一律经 0ms 单发定时器)
- **构建失败时不要吞输出**(2026-11-27 教训:把 make 输出丢进 /dev/null,链接失败没被发现,拿旧 .so 跑了一轮测试得到假结论)
- **跨平台原则**:主仓与 `third_party/` fork 的全部 C/C++ 在 Linux(主开发平台)与 Windows(MSVC,官方 OBS 构建链)都必须可编译、可测试;平台差异(套接字 API、线程句柄、printf 格式符、动态库导出等)只隔离在薄的编译期分支层,主体逻辑禁止平台专有写法;libsrt 等第三方依赖在两端都要有对应链接路径(Windows 官方 OBS 自带 srt.dll)。拿不准写法时,照 OBS 上游同场景的可移植写法
- **零警告原则**:Release/Debug 构建与全部测试一律 **0 警告**;新代码与被打补丁的上游代码都不许新增警告,更禁止用抑制警告的编译选项掩盖问题;第三方 fork 上游文件自带的警告不强制清理(保持 diff 最小),但必须确认无害

## 构建

配置/构建 preset 见 `CMakePresets.json`(configure preset `ubuntu-x86_64`,产物目录 `build_x86_64/`,依赖 = 系统 OBS 的 libobs cmake 包 + libcurl + libsimde-dev 头文件包 + **Qt6 Widgets 开发包**(`qt6-base-dev`,侧栏用,ADR-0010;`-DENABLE_DOCK=OFF` 回到 Qt-free 形态)+ libsrt/mbedtls/jansson/zlib 开发包(fork 的私有 SRT 输出用,经 fork 的官方 Find 模块解析为 imported target,见「仓库地图」))。安装布局不写在根 CMakeLists:`set_target_properties_plugin`(helpers)统一处理 Linux(`lib/…/obs-plugins` + `share/obs/obs-plugins/<name>`)/Windows(`<name>/bin/64bit` + `<name>/data`)/flatpak 扩展三种约定路径。完成判据:`cmake --build` 产物 `.so` 拷入 OBS 32 用户插件目录 `~/.config/obs-studio/plugins/<插件名>/bin/64bit/<插件名>.so`(用户目录按 `%module%` 模板扫描,平铺 `.so` 不会被加载)后,OBS 启动日志出现「插件版本:」且工具菜单出现「B站直播控制台」。

**调试构建一律 build type = Debug**(2026-11-26 用户确立):preset `ubuntu-x86_64-debug`(产物 `build_x86_64_debug/`),`cmake --preset ubuntu-x86_64-debug && cmake --build --preset ubuntu-x86_64-debug`。崩溃/疑难排查必须带完整符号——配合 systemd-coredump(`coredumpctl info <pid>`)+ `addr2line -f -C -i -e <so> <偏移>` 定位;禁止拿 Release/RelWithDebInfo 排崩溃。日常安装与发版仍走 Release preset。

## 验证

- `ctest --test-dir build_x86_64`(离线解析回归)。
- `python3 tests/check_qt_symbols.py --selftest && python3 tests/check_qt_symbols.py build_x86_64/bilibili-stream-for-obs.so`(插件引用的 Qt 符号 ⊆ `tests/qt-symbol-allowlist.txt`;新符号先核对存在于 Qt 6.2.4 再入表)。只在 Windows 出现的引用(MSVC 本地生成 vftable、不内联 dllimport 内联成员)入 `tests/qt-symbol-allowlist-msvc.txt`,核对口径是 Qt 6.2.4 的 msvc2019_64 导入库。
- CI 矩阵分工(`.github/workflows/build-project.yaml`):ubuntu-24.04 = 符号审计 + 无头 harness(面向「OBS 30.0+」的那份产物);ubuntu-26.04 = 最新 Qt 头文件的编译检查与打包(不审计,原因见 ADR-0010 §4;新头文件才有的编译错误只在它上面出现,如 Qt 6.10 起 `QFile::open` 带 `[[nodiscard]]`,本机 Qt 旧时复现不了,直接看 CI 日志);Windows = 审计并入 MSVC 名单。矩阵默认 fail-fast:一个 job 挂了其余会被取消,「cancelled」的 job 要看它被取消前有没有真失败。
- 无头集成(需 X display、python3、openssl;构建时 `-DBUILD_HARNESS=ON`):`xvfb-run -a tests/harness/run_headless.sh build_x86_64/tests/bili_harness build_x86_64/bilibili-stream-for-obs.so`。加 `-fsanitize=address,undefined` 构建再跑一遍可查内存/未定义行为。它不替代真机:停靠布局、主题、HiDPI、真实扫码必须在用户 OBS 上验收。

## 安装，运行

将二进制放在~/.config/obs-studio/plugins/bilibili-stream-for-obs/bin/64bit/，并将 `data/locale/` 放在 `~/.config/obs-studio/plugins/bilibili-stream-for-obs/data/locale/`（界面文案 i18n 必需，缺了 obs_module_text 只回显键名），OBS将自动加载。加载后:Docks 菜单出现侧栏「B站直播」(首次自动显示一次),工具菜单出现「B站直播控制台」。
