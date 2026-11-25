# 路线图:缺口状态与里程碑

蒸馏版状态表(全量审计与官方证据细节在本地档案 `re/ROADMAP.md`,不入 git;端点契约在 [api/livehime-api.md](api/livehime-api.md);决策依据在 [adr/](adr/))。每里程碑收尾更新本文件状态列。

## 决策索引

| ADR | 决策 |
|---|---|
| 0001 | 开播/推流双轴分离,不代点推流(写回条款已由 ADR-0005 推翻) |
| 0002 | 全套原生 LiveHime 画像,无指纹开关 |
| 0003 | UI 全 OBS 原生渲染(obs_properties 控制台),插件不链接 Qt |
| 0004 | 备流/协议/恢复单输出、不双推(落地方式改为线路展示,见 ADR-0005) |

## 里程碑(按依赖排序,一个分支一个里程碑,原子提交)

| # | 分支 | 内容 | 修缺口 | 状态 |
|---|---|---|---|---|
| M0 | `milestone/m0-qt-free` | UI 迁移到控制台(字段清单见下),删 Qt/moc/`ENABLE_QT`;所有后续里程碑的可编译前提 | ADR-0003 | 完成(2026-11-25,OBS 32.2 真机加载/卸载验证) |
| M0b | `milestone/m0b-fingerprint` | 原生 UA、去 Origin/Referer 与 charset、buvid3 获取+持久化+全携带、client_traceid、轮询 2s、cookie jar 轮转回写、nav/stopLive 补 appsign、notice 以 ERROR 状态行透出 | A1-A7、B4① | 完成(2026-11-25,真机验证指纹/持久化/加载卸载;扫码登录链路待用户实测) |
| M0c | `milestone/m0c-endpoints` | `getInfoByUser`/app-blink `room/GetInfo` 替代 `room_id_by_uid`;`UpdatePreLiveInfo`/`AnchorChangeRoomArea` 替代 `Room/update`;SSO 跨域传播;隐藏配置 `use_legacy_endpoints` 回退开关(默认关) | B1、B2、B3 | 完成(2026-11-25:公共参数注入器 FUN_180063f70 钉死+公开端点实网 code=0;`device=pc`/`mobi_app=pc_link`/UpdatePreLiveInfo 签名例外/room-GetInfo 字段清单全钉死;B1-B3 关闭;登录态专属调用形状待用户账号实测) |
| M1 | `milestone/m1-startlive` | startLive 全参数集(checked="0"、type、recommend_protocol/ip、xcode_suffixes、条件注入)+ 响应 12 步全解析(live_key/protocols[]/protocols_backup/xcode_stream/notice/v_voucher/sub_session_key)+ 线路全量展示(所有协议,可复制;ADR-0005 后不写 OBS 配置)+ v_voucher 轻量链(浏览器验证页) | B4、C5(rtmp/srt) | 主体完成(2026-11-25:checked/type 钉死并注入、recommend_* 隐藏配置透传、响应第2-10步解析、PreCheck 阻断+播前状态观测、线路全量展示落地(B6);xcode/多路=不做项;多协议线路随响应全量展示,v_voucher 现为文案透出) |
| M1b | `milestone/m1b-reconnect` | 启动即查(已登录才查)`live_status==1` → 控制台提示恢复;`GetUpStreamRtmp`→`ReconnectLiveInfo{can_change,urls,message}`;恢复=找回地址后线路展示;「结束进行中场次」按钮(stopLive 清残留,二次确认) | C1 | 完成(2026-11-25:§7.1 解析器钉死,恢复链=get_stream_by_roomId 优先→GetUpStreamRtmp 备用;两段式确认结束场次;登录链 live_status 透出+启动即查已有;恢复形状待账号实测) |
| M2 | `milestone/m2-quality` | **前置已完成(2026-11-25,U-Q 钉死,用户确认新模型)**:GetQualityData/QualityCallback=线路质量探测/回执(非档位);码率口径=GetUpStreamVideoQualityConfig(推荐/默认+干预,管线例外 bitrate_intervention=1 sign 后);H265 读 can_enable、AV1 读 can_hard_encode。范围:拉取展示推荐码率+干预+编码能力 → 用户设码率只写当前视频编码器 bitrate;无档位/无回执 | C4 | 完成(2026-11-25:三端点+控制台质量区落地,真机加载验证;干预/推荐值形状待账号实测) |
| M3 | `milestone/m3-backup` | `backup_stream=1` 参数位 + `protocols_backup` 全量展示(按 ADR-0005 改为线路展示,不做「切到备流」按钮) | — | 完成(2026-11-25:开播(含备流)→主/备线路全量入 `config.stream_targets` 并逐条展示;backup_stream_type/stream_biz 字面值待 app 层反编译(LivehimeLiveRoomController::OpenLiveRoom→StartLiveInfo);**备流被拒回退**——实网 70003 自动按普通开播重试并上抛原因;语义更正:备流=主备双通道(可同时推、主流失效备流生效),插件只展示备流通道数据;U6 探针(hidden config `backup_stream_type_probe`/`stream_biz_probe`)待用户在场实测) |

## 控制台字段清单(M0 验收口径,后续里程碑逐项激活)

- **状态行**(TEXT_INFO,含 WARNING/ERROR 色):登录态(mid/昵称)、房间态(room_id、live_status、锁房/风控)、质量信息(推荐码率+干预+编码能力)、最近一次操作结果
- **线路区**(文本行,可选中复制):开播/找回所得的**全部线路**逐条展示(`主线路N · 协议/provider 地址`+`推流码`;备流同样逐条),插件不写 OBS 配置(ADR-0005)
- **按钮**:扫码登录(QR SVG→系统浏览器;轮询 2s/90 次≈3 分钟)、刷新、应用标题、应用分区、拉取推荐码率与编码能力、应用码率、开播、开播(含备流)、停播、恢复推流(M1b)、结束进行中场次(M1b,两段式确认)
- **下拉**:分区大类/二级分区(M0c)
- **输入**:直播间标题(经 `UpdatePreLiveInfo`)、视频码率(只写当前编码器 bitrate)
- 隐藏配置(不入面板):`use_legacy_endpoints`、`recommend_protocol`、`recommend_ip`

## 明确不做

联播(multi_screen_*)、连麦/PK/语音房、弹幕与运营族、遥测、同时双推、ARTC/WHIP 实施(仅在协议映射表留 `webrtc→未支持` 占位)。

## 未定项闸门(动码前必须补钉,方法=定点 Ghidra 反编译)

| U | 内容 | 卡哪个里程碑 |
|---|---|---|
| U1 | `protocols[].protocol` 取值集与 `protocols_backup[]` 的形状(`type` 已钉死=`"1"`+类型索引) | 已解除:不再做协议选择,线路全量展示(ADR-0005);实测取值 `bmtsrt,bmt,srt×3,rtmp×2` |
| U-Q | ~~GetQualityData/QualityCallback 字段~~ 已钉死:**线路质量探测/回执**(非码率档);视频码率正解=GetUpStreamVideoQualityConfig(§5.4,无档位列表);遗留 U-qs=streamName/key 取值构造 | M2 已按新模型完成(A0) |
| U-R | ~~`get_stream_by_roomId` 响应字段~~ 已钉死:`data.rtmp.{addr,code}`+`stream_line[].{src,cdn_name,checked}`(解析器 F-18014cf60) | M1b(已解除) |
| U-L | login-v3 请求参数集(App OAuth 路线,本插件走 Web Cookie 路线,不阻塞) | — |

## 真实调用测试原则

离线验证(解析单测 + 反编译对照 + dry-run 日志)为默认;到关键节点列"待实测清单",用户在场并同意才真发一次 startLive/stopLive/恢复。
