# B站直播 OBS 插件(obs-bilibili-stream)

OBS 插件,以官方直播姬(LiveHime)的协议行为为基准,代理 B站直播间的"开播/停播/恢复/质量"信令;推流动作本身始终归 OBS。

## Language

### 直播生命周期(两个独立轴,不可混用)

**开播**:
服务端打开直播房间的动作(startLive 信令)。完成后房间对观众存在,但尚无画面。
_Avoid_: 开始直播、开始推流、开流

**停播**:
服务端关闭直播房间的动作(stopLive 信令)。
_Avoid_: 停止直播、关播

**推流 / 停止推流**:
OBS 自身的输出动作(点 OBS 的"开始直播"按钮,启动 RTMP/SRT 输出)。插件只展示推流地址与推流码,永远不代替用户触发此动作,也不改写 OBS 的推流配置。
_Avoid_: 开播、关播(这两个词专指信令轴)

**进行中场次**:
服务端认为仍在直播、但本地 OBS 未在推的场次(OBS 崩溃/断电后残留)。断流恢复的处置对象。

**断流恢复**:
检测到自己房间存在进行中场次(§room/GetInfo `live_status==1`)后,经 get_stream_by_roomId / GetUpStreamRtmp 取回可推地址并在控制台/侧栏展示,由用户填入 OBS 后自行点推流接回原场次。插件不改写 OBS 配置。
_Avoid_: 重连、断线重连(那是 OBS 输出层重试,不同概念)

### 推流路

**主推流路**:
startLive 响应 `protocols[]` 给出的当前应推地址(与全局 `live_key` 配对)。

**备流**:
服务端为同一场次下发的**第二条独立推流通道**(`backup_stream=1` → 响应 `protocols_backup[]`/`rtmp_backup`,有自己的地址与推流码)。语义是**主备双通道**:可同时推流,主流失效时备流在 CDN 侧自动生效;不是"同一把密钥的多个地址"——同一 stream key 开第二个连接会被服务端拒绝。
_Avoid_: 备选地址、切换式(把备流当成同 key 的备用地址是错的)

**联播**:
官方的 multi_screen_* 多房间多画面协同。**明确不做**,代码中不得为它留参数位。

**推流目标(StreamTarget)**:
一个可推的 {protocol, addr, new_link, provider, key} 组合,是 startLive 响应、恢复响应与线路展示之间的统一中间模型。

### 传输协议

**RTMP over SRT(私有 SRT / `rtmpsrt://`)**:
B站 `protocols[]` 中 `protocol=srt` 线路的实际封装:RTMP/FLV 载荷跑在 SRT(UDP) 传输上,
地址 scheme 为 `rtmpsrt://`,官方客户端用 `bvc_srt_output`(`bvc-srt-stream.dll`)推流;
同族 `bmtsrt` = BMT over SRT。传输是**真 SRT**(ARQ/重传/RTT),但载荷不是 MPEG-TS。
固定口径(E-24):UDP **1935**、**无 SRT streamid**、SRT 参数 `TSBPD`/`TLPKTDROP` 关闭、
`CONNTIMEO=10s`、`PAYLOADSIZE=1456`、`LATENCY=200ms`、`MAXBW=2MiB/s`、`LINGER=1s`;
隧道里是完整 RTMP 握手 + chunk,无私有帧头、无加密。
_Avoid_: 增强 RTMP(E-RTMP 是 `rtmp://` 下的 codec 扩展,不是本协议)、裸称 SRT(会与标准 SRT 混淆)

**标准 SRT**:
业界默认的 MPEG-TS over SRT(FFmpeg `srt://` + mpegts muxer;OBS 原生 SRT 输出即此)。
与 B站私有 SRT **不互通**。

**私有 SRT 服务条目**:
OBS `rtmp-services` 服务目录(`plugin_config/rtmp-services/services.json`)里的一条
`protocol="bvc_srt"` 条目,由插件在 OBS 原生服务表基础上合并;用户在「设置→直播」选中它时,
OBS 用本插件的 `bvc_srt_output` 推 RTMP-over-SRT。插件不写用户已选服务/编码/输出配置。

**SRT 降级**:
OBS 对未知 scheme `rtmpsrt://` 的处理:librtmp 按 scheme 长度判定失败后回落为
`rtmp://host:1935`(TCP RTMP),不启用 SRT。因此在 OBS 里 `rtmpsrt://` 与 `rtmp://` 行为等价。

### UI 与集成

**控制台**:
「配置界面」——隐藏私有 source「控制台」的 obs_properties 面板,控件全部由 OBS 渲染
(改标题/分区、拉取推荐码率、线路可复制)。ADR-0003 的 Qt-free 载体;侧栏的「直播间设置…」
与工具菜单都打开它。_Avoid_: 对话框、弹窗(控制台内提示仍只走状态行与 OBS 日志)

**侧栏(Dock)**:
`src/ui/dock.*` 注册的 Qt Widgets 停靠面板(ADR-0010,OBS「Docks」菜单可开关、布局随 OBS
保存):登录二维码、房间态快照、线路一键复制、「获取推流码/关闭直播间/找回推流码」。
它是 Core::Session 的视图,不含任何业务状态;OBS 仍在推流时拒绝关闭直播间。
_Avoid_: 把侧栏叫「控制台」(两者是同一状态的两个视图,入口不同)

**会话控制器(Core::Session)**:
插件运行期状态的唯一所有者(登录态/房间态快照/线路表/忙碌/分区表);控制台与侧栏只是
它的 Listener 视图。「是否在播」以服务端 room/GetInfo 的 live_status 为准,本地标志只做
乐观更新并在下一次刷新被覆盖。

**按钮口径**:
界面标签按用户口径(2026-11-27):开播按钮写「获取推流码」(点完拿到的是推流地址与推流码),
停播按钮写「关闭直播间」(需二次确认),恢复写「找回推流码」。文档里 开播/停播 仍专指信令。

**线路**:
开播响应 `protocols[]`/`protocols_backup[]` 里的一条推流目标(协议/providers/地址/推流码)。官方给几条就展示几条,插件不排序、不选择。
_Avoid_: 档位、备选(易与质量档混淆)

**线路展示**:
把**全部线路**以可复制文本呈现给用户(控制台:可选中复制;侧栏:「复制」按钮到剪贴板),
由用户在 OBS 里自行填写。插件**不改写** OBS 服务配置(ADR-0005)。线路随场次作废:
停播、或服务端判定未开播后,下一次刷新即清掉本地线路表。
_Avoid_: 服务改写、写回、自动填充

### 协商与控制

**质量协商**:
GetUpStreamVideoQualityConfig 拉取官方推荐/默认码率与分区/会员干预标志(H265/AV1 编码能力同步查询展示)→ 用户设定码率数值 → 插件只写当前推流输出视频编码器的 bitrate,不改编码器类型。服务端无档位枚举、无档位回执。
_Avoid_: 选档、档位回执、自动换编码器(注:GetQualityData/QualityCallback 是**上行线路质量**探测/回执,不属质量协商,归上行选点)

**上行选点**:
官方经测速接口得到 `recommend_protocol`/`recommend_ip` 并在开播请求中携带;插件把该二值作为透传参数,测速本身不在插件内做。

### 身份

**原生画像**:
模仿直播姬原生客户端的请求指纹:LiveHime UA、无 Origin/Referer、无 charset、携带 buvid3。全仓库唯一采用形态(与浏览器画像互斥,不做开关)。

**设备身份(buvid3/buvid4)**:
首启经 `x/frontend/finger/spi` 取一次,之后跨启动复用(持久化,见 Cookie 白名单);不是登录凭据,未登录也携带。

**会话追踪串(client_traceid)**:
官方仅在会话 traceid 非空时才发该请求头;插件没有服务端来源,**恒不发**
(E-21:曾自造随机 UUID,已移除)。`Http::HttpClient::traceid()` 恒为空。

**Cookie 白名单**:
`SESSDATA`/`bili_jct`/`DedeUserID`/`buvid3`/`buvid4` 五键——服务端轮转后唯一回写 config.json 的键;其余服务端 Set-Cookie 只存内存 jar。
jar 本身域无关,所以 Cookie 只发给 B站自有域名后缀(bilibili.com / biligame.com 等,
`http_client.cpp` 的 `kCookieHostSuffixes`);删除型 Set-Cookie(`name=`)会清掉本地键。

**公共参数**:
每个 app-blink/live 签名请求由注入器统一携带的 ts/version/appkey/platform=pc_link/build/access_key 六键(值来自服务端版本接口、常量与 App OAuth 身份);`nav` 也按官方 builder 携带 appsign;passport 登录 H5 的请求由页面自己处理。UpdatePreLiveInfo 是已证的管线例外:签名只覆盖公共参数。

**App OAuth 身份(access_key)**:
官方每条 app-blink/startLive/stopLive 请求都带的 `access_key`(=OAuth access_token),由扫码登录 H5 返回的 `refresh_token` 经 `/api/v2/oauth2/refresh_token` 换取并轮换;它是服务端区分“直播姬本体”的身份凭据(stopLive 60034 的判定依据)。插件持久化 refresh_token,启动/登录后自动换 access_key。

**开播信令身份 / 推流数据面**:
前者 = Cookie+appsign 的 HTTPS 请求(插件负责);后者 = OBS 输出到 addr/key(插件只负责算好并展示,不写 OBS 配置)。
