# obs-bilibili-stream

OBS 插件:代理 B站直播的**信令面**(开播/停播/恢复/质量协商),推流数据面归 OBS 本身。协议行为基准 = 官方直播姬 LiveHime 8.16 二进制逆向结论,一切协议问题以端点契约为准绳。

## 语言

代码与文档一律用 [GLOSSARY.md](GLOSSARY.md) 的规范术语:开播/停播专指服务端信令,推流专指 OBS 输出;备流是主备双通道(插件只展示线路,不驱动第二路);联播、连麦、ARTC/WHIP 实施明确不做;插件不改写 OBS 配置,只展示线路与推流码。

## 仓库地图

- `src/bilibili_api.*` — 全部端点的 builder+parser。一个端点 = 一对函数;参数注入只走统一管线(appsign/csrf/公共参数,契约 §1),不在端点内手写签名或手拼参数
- `src/http_client.*` — libcurl 传输 + cookie jar;请求头/指纹的唯一出口
- `src/plugin-main.cpp`、`src/ui/` — OBS 原生 UI:工具菜单 + 隐藏私有 source「控制台」的 obs_properties(OBS 自己渲染,插件运行期只依赖 libobs/obs-frontend-api/libcurl,见 ADR-0003)
- `src/md5`、`src/json11`、`src/qrcodegen` — vendored,保持不动
- `docs/api/livehime-api.md` — **端点契约**:增/改任何请求参数、请求头、响应字段前必读,那里有每个字段的注入条件与官方解析顺序;契约里没有的字段 = 先补逆向再实现,不许猜
- `docs/roadmap.md` — 里程碑与缺口状态;每个里程碑收尾必须同步它
- `docs/adr/` — 架构决策。提出与任一 ADR 相左的方案前先读对应 ADR
- `re/` — 本地逆向档案,**不入 git**(Ghidra 反编译原文、证据 E-xx/F-地址、分析脚本)。契约中的证据编号只在这份档案里可解析

## 工具

Ghidra 12.2: /home/zxd/tools/ghidra_12.2_PUBLIC，全套套件，含文档。使用Headless模式，操作时不要凭记忆，参考套件内自带的离线文档！

你可以用本机的其他工具，但不要安装工具，除非必须。在这种情况下，你需要暂停，由我自己安装。

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

## 构建

配置/构建 preset 见 `CMakePresets.json`(configure preset `ubuntu-x86_64`,产物目录 `build_x86_64/`,依赖 = 系统 OBS 的 libobs cmake 包 + libcurl + libsimde-dev 头文件包,均无 Qt)。完成判据:`cmake --build` 产物 `.so` 拷入 OBS 32 用户插件目录 `~/.config/obs-studio/plugins/<插件名>/bin/64bit/<插件名>.so`(用户目录按 `%module%` 模板扫描,平铺 `.so` 不会被加载)后,OBS 启动日志出现「插件版本:」且工具菜单出现「B站直播控制台」。

**调试构建一律 build type = Debug**(2026-11-26 用户确立):preset `ubuntu-x86_64-debug`(产物 `build_x86_64_debug/`),`cmake --preset ubuntu-x86_64-debug && cmake --build --preset ubuntu-x86_64-debug`。崩溃/疑难排查必须带完整符号——配合 systemd-coredump(`coredumpctl info <pid>`)+ `addr2line -f -C -i -e <so> <偏移>` 定位;禁止拿 Release/RelWithDebInfo 排崩溃。日常安装与发版仍走 Release preset。

## 安装，运行

将二进制放在~/.config/obs-studio/plugins/bilibili-stream-for-obs/bin/64bit/，OBS将自动加载。
