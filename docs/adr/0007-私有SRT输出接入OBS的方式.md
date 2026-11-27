# 私有 SRT 输出接入 OBS：向 rtmp-services 服务目录合并「B站私有 SRT」条目

**状态**：采纳（2026-11-27）

## 背景

ADR-0006 决定实现 B站私有 RTMP-over-SRT 输出（形态 A：`obs_output` 插件），协议见 E-24。
关键约束：**不写用户的推流配置、选择权留给用户**。但 OBS 前端的「服务」下拉不是插件扩展点：

- 下拉只来自 `rtmp-services` 的 `services.json`，外加前端硬编码的 WHIP 一项。
- `rtmp_custom`（「自定义」）对未知 scheme 一律返回 `"RTMP"`（`rtmp_custom_get_protocol`），
  `GetStreamOutputType` 对 `"RTMP"` 直接选第一方 `rtmp_output`；插件无法拦截。
- 但 `rtmp_common` 服务是**数据驱动**的：
  - `services.json` 每条可带 `"protocol"` 字段，`get_protocol()` 原样返回；
  - 列表构建时用 `is_protocol_available()` 过滤：`obs_is_output_protocol_registered(protocol)`
    必须为真；
  - `plugin_config/rtmp-services/services.json`（用户级）优先于 OBS 自带文件，存在时**整表替换**。

因此“在 OBS 原生基础上加入 rtmpsrt”有一个受支持的落点：**插件把原生服务表与自己的条目合并后，
写入用户级 services.json**；协议名由插件注册的 output 提供。

## 决策

1. 注册 output `bvc_srt_output`，`protocols="bvc_srt"`，`flags=AV|ENCODED|SERVICE`。
   （不再注册自定义 `obs_service_info`；服务走 OBS 自带的 `rtmp_common`。）
2. 插件在**每次 startLive/恢复成功后**与**打开控制台时**，把一条
   `"Bilibili Live - SRT | 哔哩哔哩直播 - SRT"` 合并进
   `plugin_config/rtmp-services/services.json`：
   - **无条件写**：条目是「本插件支持私有 SRT」的能力声明，即使本次响应没有
     `rtmpsrt://` 线路（如恢复场次只给 rtmp）也要保证下拉里有这一条，
     实际用不用、改成什么地址由用户决定；
   - 底表：用户已有该文件则在其上合并，否则以 OBS 自带 `services.json` 为底；
   - 只替换/追加本插件这一条（按固定 name/alt_names），其余条目原样保留；
   - `servers[]` 用当前场次的 `rtmpsrt://` 地址（地址去重；无线路时给默认入口
     `rtmpsrt://live-push.bilivideo.com/live-bvc/`）；
   - 内容无变化不落盘；原子写（`.tmp` + rename）；读-改-写全程持锁串行。
3. **用户选择权不变**：用户在 OBS「设置 → 直播」自己选服务「Bilibili Live - SRT |
   哔哩哔哩直播 - SRT」、自己在「服务器」下拉选线路、自己粘贴控制台显示的推流码、
   自己点「开始直播」。插件**不调用**
   `obs_frontend_set_streaming_service` / `save_streaming_service`，不写 profile 的
   `service.json`，不碰编码器/输出/多路配置。
4. 这是对 ADR-0005 的唯一扩展：允许写 **rtmp-services 的服务目录**（数据文件），
   不允许写用户的 service 选择与推流配置。

## 后果

- OBS 的服务下拉里会出现「B站私有 SRT」，服务器列表就是本次开播的 SRT 线路地址；
  用户选中后 OBS 的 `GetStreamOutputType` 走第三方协议分支 → `bvc_srt_output`。
- 用户级 services.json 会**遮蔽** OBS 自带的同版本列表：OBS 升级新增的服务在用户删除该文件前
  不会出现。插件以“用户已有文件为底”合并，尽量保留用户自己的条目；删除该文件即可回到原生列表。
- **rtmp-services 会联网更新并整表覆盖 services.json**（OBS 32.2 实测：启动时拉到 version 295
  后覆盖）。插件刻意**不在模块加载时写**（避免与更新线程互相覆盖），只在开播成功/打开控制台时
  合并；因此 OBS 刚启动、更新完成后，条目要等一次开播或打开控制台才出现。
- **写入后无需重启 OBS**：`rtmp-services` 在每次构建 `rtmp_common` 的 properties 时重新读用户级
  services.json（OBS 32.2 + libobs 实测：写入前服务列表 5 条、写入后重建 properties 变 6 条且含
  「Bilibili Live - SRT」），所以**关掉再打开「设置→直播」窗口**即可看到条目。
- 覆盖只在**远端版本更新**时发生：`plugin_config/rtmp-services/package.json` 记录的版本与远端一致
  时 rtmp-services 不再下载（实测：条目写入后连续启动 OBS 仍留存），所以条目通常是稳定的。
- 若 `rtmp-services` 模块不存在或文件解析失败，插件只记日志、保持原文件不动（功能降级为
  控制台仍展示线路，OBS 侧无 SRT 服务条目）。
- 若用户之后卸载插件：服务条目仍在，但协议 `bvc_srt` 没有 output 注册，OBS 会报“没有兼容的输出”；
  用户改回其他服务即可（或删除该条目）。
- 编解码口径随 fork 的上游 flv-mux：H.264/AAC 为 legacy 口径（与官方 track0「old style for
  compatibility」一致），HEVC/AV1 走 Enhanced-RTMP（E-26 §3）；接入方式不变。
