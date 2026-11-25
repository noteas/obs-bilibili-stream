# UI 全部走 OBS 原生渲染机制,插件不链接 Qt

旧 UI 是 QWidget 系(dialog_factory ~300 行 + moc),带来 Qt6 编译期依赖且与"插件只做信令"的定位不符。决定:UI 收敛为三样 —— 工具菜单动作、隐藏私有 source「控制台」的 obs_properties(按钮/下拉/文本/开关由 OBS 自己渲染,经 `obs_frontend_open_source_properties` 弹出,`tick()`+`obs_source_update_properties()` 实时刷新)、二维码与人脸码生成 PNG 交系统浏览器打开;`ENABLE_QT` 移除。OBS 本身就是 Qt 应用,插件自绘控件是重复依赖;免 Qt 后编译依赖只剩 OBS 头与 libcurl,天然跨平台。已接受的代价:OBS 32 前端 API 无消息框,所有提示走控制台状态行与 OBS 日志;扫码登录进度需控制台开着才可见。
