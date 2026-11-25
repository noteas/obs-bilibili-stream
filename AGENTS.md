# obs-bilibili-stream

OBS 插件:代理 B站直播的**信令面**(开播/停播/恢复/质量协商),推流数据面归 OBS 本身。协议行为基准 = 官方直播姬 LiveHime 8.16 二进制逆向结论,一切协议问题以端点契约为准绳。

## 语言

代码与文档一律用 [GLOSSARY.md](GLOSSARY.md) 的规范术语:开播/停播专指服务端信令,推流专指 OBS 输出;备流是切换式;联播、连麦、双推是明确不做的范围(命名也不给它们留位置)。

## 仓库地图

- `src/bilibili_api.*` — 全部端点的 builder+parser。一个端点 = 一对函数;参数注入只走统一管线(appsign/csrf/公共参数,契约 §1),不在端点内手写签名或手拼参数
- `src/http_client.*` — libcurl 传输 + cookie jar;请求头/指纹的唯一出口
- `src/plugin-main.cpp`、`src/ui/` — OBS 原生 UI:工具菜单 + 隐藏私有 source「控制台」的 obs_properties(OBS 自己渲染,插件运行期只依赖 libobs/obs-frontend-api/libcurl,见 ADR-0003)
- `src/md5`、`src/json11`、`src/qrcodegen` — vendored,保持不动
- `docs/api/livehime-api.md` — **端点契约**:增/改任何请求参数、请求头、响应字段前必读,那里有每个字段的注入条件与官方解析顺序;契约里没有的字段 = 先补逆向再实现,不许猜
- `docs/roadmap.md` — 里程碑与缺口状态;每个里程碑收尾必须同步它
- `docs/adr/` — 架构决策。提出与任一 ADR 相左的方案前先读对应 ADR
- `re/` — 本地逆向档案,**不入 git**(Ghidra 反编译原文、证据 E-xx/F-地址、分析脚本)。契约中的证据编号只在这份档案里可解析

## 行为准则

- 未定项(契约 §8 的 U-xx)在动码前先向用户请示补钉(定点 Ghidra 反编译是我的活,查证不劳用户)
- 真实开播类调用(startLive/stopLive/恢复)**仅在用户在场并明确同意时**发起;平时验证 = 离线解析测试 + dry-run 日志
- 原子提交;每个里程碑收尾:行为变化同步 `docs/roadmap.md` 状态列,新领域术语进 `GLOSSARY.md`

## 构建

配置/构建 preset 见 `CMakePresets.json`(linux preset,依赖 = 系统 OBS 的 libobs cmake 包 + libcurl + libsimde-dev 头文件包,均无 Qt)。完成判据:`cmake --build` 产物 `.so` 拷入 OBS 32 用户插件目录 `~/.config/obs-studio/plugins/<插件名>/bin/64bit/<插件名>.so`(用户目录按 `%module%` 模板扫描,平铺 `.so` 不会被加载)后,OBS 启动日志出现「插件版本:」且工具菜单出现「B站直播控制台」。
