# 路线图:缺口状态与里程碑

蒸馏版状态表(全量审计与官方证据细节在本地档案 `re/ROADMAP.md`,不入 git;端点契约在 [api/livehime-api.md](api/livehime-api.md);决策依据在 [adr/](adr/))。每里程碑收尾更新本文件状态列。

## 决策索引

| ADR | 决策 |
|---|---|
| 0001 | 开播/推流双轴分离,插件只服务改写,不代点推流 |
| 0002 | 全套原生 LiveHime 画像,无指纹开关 |
| 0003 | UI 全 OBS 原生渲染(obs_properties 控制台),插件不链接 Qt |
| 0004 | 备流/协议/恢复 = 切换式服务改写,单输出,不双推 |

## 里程碑(按依赖排序,一个分支一个里程碑,原子提交)

| # | 分支 | 内容 | 修缺口 | 状态 |
|---|---|---|---|---|
| M0 | `milestone/m0-qt-free` | UI 迁移到控制台(字段清单见下),删 Qt/moc/`ENABLE_QT`;所有后续里程碑的可编译前提 | ADR-0003 | 完成(2026-11-25,OBS 32.2 真机加载/卸载验证) |
| M0b | `milestone/m0b-fingerprint` | 原生 UA、去 Origin/Referer 与 charset、buvid3 获取+持久化+全携带、client_traceid、轮询 2s、cookie jar 轮转回写、nav/stopLive 补 appsign、notice 以 ERROR 状态行透出 | A1-A7、B4① | 完成(2026-11-25,真机验证指纹/持久化/加载卸载;扫码登录链路待用户实测) |
| M0c | `milestone/m0c-endpoints` | `getInfoByUser`/app-blink `room/GetInfo` 替代 `room_id_by_uid`;`UpdatePreLiveInfo`/`AnchorChangeRoomArea` 替代 `Room/update`;SSO 跨域传播;隐藏配置 `use_legacy_endpoints` 回退开关(默认关) | B1、B2、B3 | 完成(2026-11-25:公共参数注入器 FUN_180063f70 钉死+公开端点实网 code=0;`device=pc`/`mobi_app=pc_link`/UpdatePreLiveInfo 签名例外/room-GetInfo 字段清单全钉死;B1-B3 关闭;登录态专属调用形状待用户账号实测) |
| M1 | `milestone/m1-startlive` | startLive 全参数集(checked="0"、type、recommend_protocol/ip、xcode_suffixes、条件注入)+ 响应 12 步全解析(live_key/protocols[]/protocols_backup/xcode_stream/notice/v_voucher/sub_session_key)+ 服务改写管线(rtmp_custom;SRT 映射就位)+ 协议下拉 + v_voucher 轻量链(浏览器验证页) | B4、C5(rtmp/srt) | 待开工 |
| M1b | `milestone/m1b-reconnect` | 启动即查(已登录才查)`live_status==1` → 控制台提示恢复;`GetUpStreamRtmp`→`ReconnectLiveInfo{can_change,urls,message}`;恢复=服务改写;「结束进行中场次」按钮(stopLive 清残留,二次确认) | C1 | 待开工 |
| M2 | `milestone/m2-quality` | **前置**:定点 Ghidra 钉死 `GetQualityData`/`QualityCallback` 响应字段与档位→码率口径,结论先经用户确认。然后:质量档下拉 → 仅写当前流输出视频编码器 bitrate → `QualityCallback` 回执;`CanUseH265/AV1` 结果仅展示 | C4 | 阻塞于前置 |
| M3 | `milestone/m3-backup` | `backup_stream=1` 参数位 + `protocols_backup` 存储 + 控制台备流行与「切到备流」 | — | 待开工 |

## 控制台字段清单(M0 验收口径,后续里程碑逐项激活)

- **状态行**(TEXT_INFO,含 WARNING/ERROR 色):登录态(mid/昵称)、房间态(room_id、live_status、锁房 notice/风控)、当前推流目标(协议、addr、key 打码)、备流地址(M3)、质量档(M2)、最近一次操作结果
- **按钮**:扫码登录(QR SVG→系统浏览器;轮询 2s/90 次≈3 分钟,完成弹日志+状态行)、刷新、开播、停播、检测恢复(M1b)、结束进行中场次(M1b)、切到备流(M3)、打开验证页(M1 风控)
- **下拉**:二级分区(M0c)、推送协议(M1)、质量档(M2)
- **输入**:标题(经 `UpdatePreLiveInfo`,M0c)
- **开关**:启用备流(M3)
- 隐藏配置(不入面板):`use_legacy_endpoints`

## 明确不做

联播(multi_screen_*)、连麦/PK/语音房、弹幕与运营族、遥测、同时双推、ARTC/WHIP 实施(仅在协议映射表留 `webrtc→未支持` 占位)。

## 未定项闸门(动码前必须补钉,方法=定点 Ghidra 反编译)

| U | 内容 | 卡哪个里程碑 |
|---|---|---|
| U1 | `protocols[].protocol` 除 rtmp 外的取值集、`type` 参数值 | M1(可先按"未知协议忽略"策略放行) |
| U-Q | `GetQualityData`/`QualityCallback` 请求响应字段 | M2 |
| U-R | `get_stream_by_roomId` 响应字段(恢复主路径用 GetUpStreamRtmp,不阻塞) | M1b(降级) |
| U-L | login-v3 请求参数集(App OAuth 路线,本插件走 Web Cookie 路线,不阻塞) | — |

## 真实调用测试原则

离线验证(解析单测 + 反编译对照 + dry-run 日志)为默认;到关键节点列"待实测清单",用户在场并同意才真发一次 startLive/stopLive/恢复。
