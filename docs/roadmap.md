# 路线图:缺口状态与里程碑

蒸馏版状态表(剩余缺口与后续逆向计划在本地档案 `re/ROADMAP.md`,不入 git;端点契约在 [api/livehime-api.md](api/livehime-api.md);决策依据在 [adr/](adr/))。每里程碑收尾更新本文件状态列。

## 决策索引

| ADR | 决策 |
|---|---|
| 0001 | 开播/推流双轴分离，不代点推流；不改写 OBS 配置（ADR-0005） |
| 0002 | 全套原生 LiveHime 画像，无指纹开关 |
| 0003 | UI 全 OBS 原生渲染（obs_properties 控制台），插件不链接 Qt |
| 0004 | 备流/协议/恢复只展示线路，不驱动第二路推流（ADR-0005） |
| 0005 | 线路全量展示，不写 OBS 配置 |
| 0006 | SRT 线路只展示；私有 RTMP-over-SRT 输出不复用 OBS 原生 SRT；U-SRT 已逆向钉死（E-24） |
| 0007 | 私有 SRT 输出：注册 `bvc_srt_output`，向 rtmp-services 用户级 services.json 合并「B站私有 SRT」条目；不写用户推流配置 |
| 0008 | fork 上游 OBS 32.2 RTMP 栈（`third_party/obs-rtmp-srt`），RTMP/FLV 逻辑零改动，只在 socket 层加 SRT 分支 |

## 里程碑(按依赖排序,一个分支一个里程碑,原子提交)

| # | 分支 | 内容 | 修缺口 | 状态 |
|---|---|---|---|---|
| M0 | `milestone/m0-qt-free` | UI 迁移到控制台(字段清单见下),删 Qt/moc/`ENABLE_QT`;所有后续里程碑的可编译前提 | ADR-0003 | 完成(2026-11-25,OBS 32.2 真机加载/卸载验证) |
| M0b | `milestone/m0b-fingerprint` | 原生 UA、去 Origin/Referer 与 charset、buvid3 获取+持久化+全携带、client_traceid、轮询 2s、cookie jar 轮转回写、nav/stopLive 补 appsign、notice 以 ERROR 状态行透出 | A1-A7、B4① | 完成(2026-11-25,真机验证指纹/持久化/加载卸载;扫码登录链路待用户实测) |
| M0c | `milestone/m0c-endpoints` | `getInfoByUser`/app-blink `room/GetInfo` 替代 `room_id_by_uid`;`UpdatePreLiveInfo`/`AnchorChangeRoomArea` 替代 `Room/update`;SSO 跨域传播;隐藏配置 `use_legacy_endpoints` 回退开关(默认关;3.0.2 已移除) | B1、B2、B3 | 完成(2026-11-25:公共参数注入器 FUN_180063f70 钉死+公开端点实网 code=0;`device=pc`/`mobi_app=pc_link`/UpdatePreLiveInfo 签名例外/room-GetInfo 字段清单全钉死;B1-B3 关闭;登录态专属调用形状待用户账号实测) |
| M1 | `milestone/m1-startlive` | startLive 参数集(checked="0"、type、recommend_protocol/ip、backup_stream、条件注入)+ 响应解析(live_key/protocols[]/protocols_backup/notice/v_voucher)+ 线路全量展示(所有协议,可复制;ADR-0005：不写 OBS 配置)+ v_voucher 轻量链(浏览器验证页) | B4、C5(rtmp/srt) | 主体完成(2026-11-25:checked/type 钉死并注入、recommend_* 隐藏配置透传、响应解析、PreCheck 阻断+播前状态观测、线路全量展示落地;xcode/多路=不做项;多协议线路随响应全量展示,v_voucher 现为文案透出;2026-11-26 实证修正:直连线路推流码=条目自带 code,live_key 仅会话配对——此前误用 live_key 致 edge 拒绝推流) |
| M1b | `milestone/m1b-reconnect` | 启动即查(已登录才查)`live_status==1` → 控制台提示恢复;`GetUpStreamRtmp`→`ReconnectLiveInfo{can_change,urls,message}`;恢复=找回地址后线路展示;「结束进行中场次」按钮(stopLive 清残留,二次确认) | C1 | 完成(2026-11-25:§7.1 解析器钉死,恢复链=get_stream_by_roomId 优先→GetUpStreamRtmp 备用;两段式确认结束场次;登录链 live_status 透出+启动即查已有;恢复形状待账号实测) |
| M2 | `milestone/m2-quality` | **前置已完成(2026-11-25,U-Q 钉死,用户确认新模型)**:GetQualityData/QualityCallback=线路质量探测/回执(非档位);码率口径=GetUpStreamVideoQualityConfig(推荐/默认+干预,管线例外 bitrate_intervention=1 sign 后);H265 读 can_enable、AV1 读 can_hard_encode。范围:拉取展示推荐码率+干预+编码能力(只读展示,插件不改写 OBS 配置,码率由用户在 OBS 中自行设置);无档位/无回执 | C4 | 完成(2026-11-25:三端点+控制台质量区落地,真机加载验证;干预/推荐值形状待账号实测) |
| R3.0.2 | — | 清理死链路与残留:access_key 交换(`refresh_token→oauth2/refresh_token`,E-20 恒 -101)、探针(`backup_stream_type_probe`/`stream_biz_probe`/`client_traceid_probe`)、`recommend_protocol`/`recommend_ip` 透传、`use_legacy_endpoints` + 旧端点(`room_id_by_uid`/`Room/update`)、`rtmp_backup_*` 兼容副本;公共参数 `access_key` 保留键、恒发空值 | — | 完成(2026-11-27,3.0.2 Release) |
| M3 | `milestone/m3-backup` | `backup_stream=1` 参数位 + `protocols_backup` 全量展示(按 ADR-0005 改为线路展示,不做「切到备流」按钮) | — | 完成(2026-11-25:开播(含备流)→主/备线路全量入 `config.stream_targets` 并逐条展示;backup_stream_type/stream_biz 字面值已定(E-19:RequestStartLive 14 参 vcall 实锤 backup_stream_type=2/stream_biz="Paylive",教室房 "ClassRoom";U6 路由=槽 0x68 备流 builder);**备流被拒回退**——实网 70003 自动按普通开播重试并上抛原因;语义更正:备流=主备双通道(可同时推、主流失效备流生效),插件只展示备流通道数据;U6 闭合后已对齐(2026-11-26):备流请求默认恒注 backup_stream_type=2/stream_biz=Paylive(3.0.2 起固定官方字面量,探针已移除);复验记录:①恒注伴参后 70003→70002(伴参已达服务端,分配走得更深)②备流路径补注 src(get_stream_by_roomId.stream_line,checked 优先)待用户在场实测;③src=8 注入后仍 70002,body 与官方默认字面量指令级全等⇒排除 body;④access_key 假设经 E-20 定案弱化(web token 无桥,官方 web 登录大概率为空);⑤E-21 定位传输层具体嫌疑=插件自造的 client_traceid UUID(官方非空才注、原生 32-hex),已改默认不发+client_traceid_probe 探针,实网复验仍 70002;⑥**E-22 根因定案(2026-11-26,订正 E-19「官方恒带备流伴参」)**:官方开播分派器 F-180d5de60 按 upower_privilege_type(§3.10 GetVisibleMode)switch,仅 ==5("PRELIVE_UPOWER")走备流 builder,其余走标准 builder(不带备流参)⇒ 无资格账号官方从不发 backup_stream,70002=插件强发伴参的业务层正确拒绝(普通开播 protocols_backup:null 互证;libcurl 其余接口全成功 ⇒ 指纹出局)。仓库已修:单「开播」按钮自动判定(先 getVisibleMode 查资格,==5 才注 src+备流三参,≠5 按官方普通请求发送并提示无资格;「开播(含备流)」独立按钮已并入);待实网复验回值) |
| M4 | `milestone/m4-srt-output` | 私有 SRT 输出（`bvc_srt_output` + `bvc_srt` 协议 + 服务目录合并）：完整 RTMP over SRT（E-24 参数）；HEVC/AV1 走 Enhanced-RTMP（E-26 §3）；用户在 OBS「设置→直播」选「Bilibili Live - RTMPSRT」条目，插件不写用户推流配置 | U-SRT | 手搓实现已回退（2026-11-27，E-27：服务器无逐字节要求，官方 rtmpsrt 与原始 RTMP 的 RTMP 层同源=上游 OBS 血统）；保留服务目录无条件写 + rtmpsrt 线路解析/展示（真机验收过的旧实现留档 git 历史）。重实现口径 = fork submodule `third_party/obs-rtmp-srt`（基线 = 上游 OBS 32.2.0 RTMP 栈逐字导入），仅 socket 层改 SRT（E-24 参数），RTMP/FLV 逻辑零改动（ADR-0008）；真机验收判据 = 服务器接受且流正常（E-27 §4）。**2026-11-27 实现完成**：`bvc_srt_output` 已链入插件（srt_module 注册壳），crypto 走 OBS 官方 mbedTLS 路径，Release/Debug 双构建 + CTest 全绿；实机加载失败已修（缺 `OBS_MODULE_USE_DEFAULT_LOCALE`，rtmp-stream.c 引用的 `obs_module_text` 须由插件侧宏定义）；全插件界面文案 i18n 落地（zh-CN/en-US `data/locale/`，ADR-0003 补充；`data/locale/` 列为安装必装项）；待真机推流验收 |

## 控制台字段清单(M0 验收口径,后续里程碑逐项激活)

- **状态行**(TEXT_INFO,含 WARNING/ERROR 色):登录态(mid/昵称)、房间态(room_id、live_status、锁房/风控)、质量信息(推荐码率+干预+编码能力)、最近一次操作结果
- **线路区**(文本行,可选中复制):开播/找回所得的**全部线路**逐条展示(`主线路N · 协议/provider 地址`+`推流码`;备流同样逐条),插件不写 OBS 配置(ADR-0005)
- **按钮**:扫码登录(QR SVG→系统浏览器;轮询 2s/90 次≈3 分钟)、刷新、应用标题、应用分区、拉取推荐码率与编码能力、开播(自动判定备流资格:先查 GetVisibleMode,==5 发备流请求,否则普通请求,E-22)、停播、恢复推流(M1b)、结束进行中场次(M1b,两段式确认)
- **下拉**:分区大类/二级分区(M0c)
- **输入**:直播间标题(经 `UpdatePreLiveInfo`)、视频码率(只写当前编码器 bitrate)
- 隐藏配置(不入面板):无。3.0.2 起 `use_legacy_endpoints`、`recommend_protocol`、
  `recommend_ip` 及全部探针字段已移除,`config.json` 只保留面板可达状态

## 明确不做

联播(multi_screen_*)、连麦/PK/语音房、弹幕与运营族、遥测、ARTC/WHIP 实施(仅在协议映射表留 `webrtc→未支持` 占位)明确不做；插件自身不驱动第二路推流（备流线路仍如实展示，由用户决定如何配置）。

## 未定项闸门(动码前必须补钉,方法=定点 Ghidra 反编译)

| U | 内容 | 卡哪个里程碑 |
|---|---|---|
| U1 | `protocols[].protocol` 取值集与 `protocols_backup[]` 的形状(`type` 已钉死=`"1"`+类型索引) | 已解除:不再做协议选择,线路全量展示(ADR-0005);实测取值 `bmtsrt,bmt,srt×3,rtmp×2`;2026-11-27 补 scheme 语义与官方输出分派(§4.6/E-23);U-SRT 已随之解除(E-24) |
| U-SRT | ~~`bvc-srt-stream.dll` 私有 SRT 封装~~ | 已解除(2026-11-27,E-24):UDP **1935**、**无 streamid**、载荷=完整 RTMP(握手+chunk 内 FLV tag)、鉴权走 RTMP URL、SRT 参数硬编码(`TSBPD/TLPKTDROP=off`、`CONNTIMEO=10s`、`PAYLOADSIZE=1456`、`LATENCY=200ms`、`MAXBW=2MiB/s`、`LINGER=1s`)、libsrt 1.4.4。字节级口径另见 E-25；M4 重实现完成（fork 内嵌链入插件，待真机验收） |
| U-Q | ~~GetQualityData/QualityCallback 字段~~ 已钉死:**线路质量探测/回执**(非码率档);视频码率正解=GetUpStreamVideoQualityConfig(§5.4,无档位列表);遗留 U-qs=streamName/key 取值构造 | M2 已按新模型完成(A0) |
| U-R | ~~`get_stream_by_roomId` 响应字段~~ 已钉死:`data.rtmp.{addr,code}`+`stream_line[].{src,cdn_name,checked}`(解析器 F-18014cf60);`src`(整数)=startLive `src` 参数唯一来源,经 service 子对象 `vt[0xf8]` 透传(E-19) | M1b(已解除) |
| U-L | login-v3 请求参数集(App OAuth 路线,本插件走 Web Cookie 路线,不阻塞) | — |
| U-A | stopLive 60034: **已消解(2026-11-26 实网,生效项=version/platform/build 参数指纹)**;access_key 交换定案(E-20):web token 无桥,官方 token 仅账密/SNS 产出,可选路线=实现 v3/oauth2/login(需用户密码+验证码,未决策)。**3.0.2**:交换死链路已从仓库移除,`access_key` 键恒发空值 | 已闭合 |

## 真实调用测试原则

离线验证(解析单测 + 反编译对照 + dry-run 日志)为默认;到关键节点列"待实测清单",用户在场并同意才真发一次 startLive/stopLive/恢复。
