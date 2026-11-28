# UI 全部走 OBS 原生渲染机制，插件不链接 Qt

UI 收敛为三样 —— 工具菜单动作、隐藏私有 source「控制台」的 obs_properties（按钮/下拉/文本/开关由 OBS 自己渲染，经 `obs_frontend_open_source_properties` 弹出，`tick()`+`obs_source_update_properties()` 实时刷新）、二维码与人脸码生成 PNG 交系统浏览器打开；不引入 Qt。OBS 本身就是 Qt 应用，插件自绘控件是重复依赖；免 Qt 后编译依赖只剩 OBS 头与 libcurl，天然跨平台。已接受的代价：OBS 32 前端 API 无消息框，所有提示走控制台状态行与 OBS 日志；扫码登录进度需控制台开着才可见。

补充（2026-11-27）：全部 UI 可见文案改走 OBS 标准 locale 机制（`OBS_MODULE_USE_DEFAULT_LOCALE` + `data/locale/{zh-CN,en-US}.ini`，随 OBS 界面语言切换；helper `Utils::Text`，见 `src/plugin_utils.hpp`），不再硬编码中文；default_locale=zh-CN 兼作缺键回落层，`data/locale/` 必须随插件安装。日志不翻译。

**2026-11-27 修订（ADR-0010）**：按用户需求新增**状态侧栏**（Qt Widgets dock，`src/ui/dock.*`，承载登录二维码、房间态、线路复制、「关闭直播间」二次确认与「直播间设置…」入口）。本文「控制台是插件唯一 UI 载体」「前端 API 无消息框」两句自此仅对控制台成立：OBS 没有 Qt-free 的 dock API，侧栏内允许 `QMessageBox`。其余不变——控制台仍走 obs_properties、文案仍走 `obs_module_text`、不引入任何第三种 UI 载体，Qt 头只出现在 dock.cpp。
