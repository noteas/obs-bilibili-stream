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
OBS 自身的输出动作(点 OBS 的"开始直播"按钮,启动 RTMP/SRT 输出)。插件只写入服务配置,永远不代替用户触发此动作。
_Avoid_: 开播、关播(这两个词专指信令轴)

**进行中场次**:
服务端认为仍在直播、但本地 OBS 未在推的场次(OBS 崩溃/断电后残留)。断流恢复的处置对象。

**断流恢复**:
检测到自己房间存在进行中场次(§getInfoByUser `live_status==1`)后,经 GetUpStreamRtmp 取回可推地址并重写 OBS 服务配置,使用户点一次推流即可接回原场次。
_Avoid_: 重连、断线重连(那是 OBS 输出层重试,不同概念)

### 推流路

**主推流路**:
startLive 响应 `protocols[]` 给出的当前应推地址(与全局 `live_key` 配对)。

**备流**:
同一房间的第二条备用推流路(startLive 请求 `backup_stream=1`,响应 `protocols_backup[]`)。本仓库语义 = **切换式**:主路故障时把 OBS 服务配置改写为备流地址,由用户重新推流;不是同时双推。
_Avoid_: 备份流、副流

**联播**:
官方的 multi_screen_* 多房间多画面协同。**明确不做**,代码中不得为它留参数位。

**推流目标(StreamTarget)**:
一个可推的 {protocol, addr, new_link, provider, key} 组合,是 startLive 响应、恢复响应与 OBS 服务配置之间的统一中间模型。

### UI 与集成

**控制台**:
提供属性面板式 UI 的隐藏私有 source,是插件唯一的 UI 载体,控件全部由 OBS 渲染。
_Avoid_: 对话框、弹窗、dock

**服务改写**:
把一个推流目标落到 OBS 服务配置的唯一起见(`set_streaming_service`+`save`);备流切换、协议切换、断流恢复都只是它的调用方。
_Avoid_: 换地址、自动重连(后者是 OBS 输出层内部重试)

### 协商与控制

**质量协商**:
GetQualityData 拉取账号质量档 → 用户选档 → 插件将档位映射为当前推流输出的视频编码器码率 → QualityCallback 向服务端回执。只写码率,不改编码器类型。

**上行选点**:
官方经测速接口得到 `recommend_protocol`/`recommend_ip` 并在开播请求中携带;插件把该二值作为透传参数,测速本身不在插件内做。

### 身份

**原生画像**:
模仿直播姬原生客户端的请求指纹:LiveHime UA、无 Origin/Referer、无 charset、携带 buvid3。全仓库唯一采用形态(与浏览器画像互斥,不做开关)。

**设备身份(buvid3/buvid4)**:
首启经 `x/frontend/finger/spi` 取一次,之后跨启动复用(持久化,见 Cookie 白名单);不是登录凭据,未登录也携带。

**会话追踪串(client_traceid)**:
每次插件加载生成的 UUIDv4,同一 OBS 会话内全部请求同值,随请求头携带。

**Cookie 白名单**:
`SESSDATA`/`bili_jct`/`DedeUserID`/`buvid3`/`buvid4` 五键——服务端轮转后唯一回写 config.json 的键;其余服务端 Set-Cookie 只存内存 jar。

**公共参数**:
每个 app-blink/live 签名请求由注入器统一携带的 ts/version/appkey/platform=pc_link/build 五键(值来自服务端版本接口与常量);Web 域端点(nav、passport)不走这套。UpdatePreLiveInfo 是已证的管线例外:签名只覆盖公共参数。

**开播信令身份 / 推流数据面**:
前者 = Cookie+appsign 的 HTTPS 请求(插件负责);后者 = OBS 输出到 addr/key(插件只负责算好并写入)。
