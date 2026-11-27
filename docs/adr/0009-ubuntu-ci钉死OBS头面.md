# Ubuntu CI 依赖策略：钉死 OBS 头面 + apt 链接面，不在 CI 编译 OBS

**状态**：采纳（2026-11-27）

## 背景

fork 编译基线 = OBS 32.2 头（`OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE` 为 32.2.0 新增），
而发行版 `libobs-dev` 版本漂移且必旧（CI 实证：noble=30.0.2、resolute=32.1.0，均缺该宏）。
发布 3.1.0 时为在 Ubuntu CI 拿到 32.2 开发文件，先后尝试：

1. **OBS 官方 PPA**：只发 `obs-studio` 单包，无 `libobs-dev`，dev 文件布局不可依赖；
2. **仿 Windows/macOS 的 buildspec 源码构建**（`_setup_obs_studio`）：把 OBS 的
   configure 依赖树整个引进 CI —— ECM、X11/XCB/X11-XCB、Libdrm、UUID、PulseAudio、
   FFmpeg(6.1+)、OpenGL(glad)、SIMDe/Uthash/jansson（apt 12+ 包），OBS 构建系统每次
   演进都要重新对账；且 runner 镜像自带 CMake（4.5，随镜像更新漂移）与 OBS
   install-export 的单配置产物不兼容（实证 `IMPORTED_LOCATION not set for imported
   target "OBS::libobs" configuration "Release"`，generate 失败），本机 CMake 3.x/4.4
   均不可复现——不可控的二维漂移（OBS 构建系统 × runner CMake）。

## 决策

Linux CI **永不编译 OBS**，把头面与链接面拆开（`cmake/linux/buildspec.cmake`，CI 门控）：

- **头面**：下载 buildspec `obs-studio` 条目钉死的 32.2.0 源码 zip（与 Windows 同包
  同哈希），仅解包取 `libobs/` 与 `frontend/api/` 头，`include_directories(BEFORE …)`
  覆盖系统旧头；解包后校验钉死头必含 32.2 新宏（护栏）。
- **链接面**：apt `libobs-dev` 照旧。依据：插件 `.so` 是 MODULE，`obs_*` 符号在装载期
  由真机 OBS（32.x）解析；CI 链接只要求被引用符号存在（30 时代符号全集覆盖本插件
  与 fork 的引用；32.2 新增是编译期宏，不引入新符号）。
- **真 32.2 ABI 门槛**：flatpak job（flathub OBS 32.2 运行时编译+打包）与用户真机
  OBS 32.x 装载，均已在 CI/发布路径上。

本地开发（无 `CI` 环境变量）不进入该模块，仍走系统 OBS 的 libobs cmake 包。

## 后果

- Ubuntu apt 列表从 19 包缩回 10 包；OBS 构建系统演进、runner CMake 版本漂移
  与 Ubuntu 构建彻底解耦（头 zip 不可变，哈希钉死）。
- 头/库版本短暂混用（32.2 头 × 30/32.1 库）：符号面论证成立 + flatpak 真库门槛兜底；
  若未来插件调用 32.2 **新增函数符号**（而非宏），此混用会在真机装载期暴露——届时
  该函数必须像官方端点一样走契约流程，不会漏。
- Windows/macOS 维持 `_setup_obs_studio` 源码构建（多配置 VS 导出成熟，已绿），不动。
