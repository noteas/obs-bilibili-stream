<!-- 本文件是随仓库分发的端点契约, 由本地逆向档案 re/API.md 迁移而来。
证据编号(E-xx §n、@0x… 、F-<地址>)解析到本地 re/evidence/ 档案, 不随仓库分发;
re/ 的位置与用途见 AGENTS.md。迁移时若 re/API.md 有更新, 以本文件重新迁移为准。 -->
# B站直播姬 8.16.0.11349 全链路 API 文档
## 登录 → 开播前 → 开播 → 推流 → 停播 / 断流恢复

每个端点固定三段:**请求 / 响应 / 证据**。
证据标记: `F-<地址>` = `re/evidence/11-flow-decompiled.txt` 反编译函数(VA, ImageBase 0x180000000);
`E-xx §n/@0x…` = `re/evidence/` 对应文件(§10 文件开头);仓库 = 本 OBS 插件 `src/`。
未钉死的取值一律标 **(未定)**, 不得当事实用。

---

# 1. 通用约定(适用于全部签名端点)

## 1.1 请求

**构造管线**(每个请求依次经过;全部核对过的端点均一致):

```
builder(业务参数, 条件注入)
→ FUN_1804b5330 : 注入 csrf_token=<bili_jct>、csrf=<bili_jct>(两键同值)
→ FUN_1804b3d60 : appsign(见下)
→ map["sign"] = <32位小写hex>
→ 请求头: Cookie=<会话Cookie> ; client_traceid=<追踪串>(非空才带)
→ URL → FUN_1801397f0 派发(scheme 白名单 http/https)
```

**appsign 算法**:
1. 参数全集(业务参数+`appkey`+`ts`+已注入的全部键)按 key 字典序排序;
2. 拼 `k1=v1&k2=v2&…`(值不转义);
3. 尾部**直接追加** `APP_SECRET`(无 `&`);
4. `MD5` → 16 字节 → 32 位小写 hex;
5. `sign` 并入输出末尾。

`appkey=aae92bc66f3edfab` `secret=af125a0d5279fd576c1b4418a3e8276d`

**公共参数**(谁注入):

| 参数 | 注入方 |
|---|---|
| `appkey` `ts` | 服务基类内联 |
| `csrf_token` `csrf` | FUN_1804b5330 |
| `version`(=服务端 curr_version) `build` | 服务基类(值来自 §3.4) |
| `platform=pc_link` `mobi_app` | app-blink builder |
| `access_key` | **公共参数注入器 FUN_180063f70 恒注**（所有 app-blink/startLive/stopLive；值=App OAuth access_token） |

**注入器实身 = FUN_180063f70**(app-blink/live 签名端点的 builder 序章,反编译实证):
一次注入 `ts`(=微秒/1e6 的秒值) `version` `appkey` `platform=pc_link` `build`
`access_key`。**Web 域端点不走此注入器**(§2.9 nav 仅 ts+appkey+sign)。
缺公共参数 ⇒ `-400 请求错误`(实网实证);带齐时公开端点 `getInfoByRoom` 无登录态即
`code=0`(2026-11-25)。stopLive 也走该注入器（E-15）。

**头**:
```
POST: Content-Type: application/x-www-form-urlencoded
Cookie: SESSDATA=..; bili_jct=..; DedeUserID=..; buvid3=..   (官方硬校验 buvid3)
User-Agent: Bililive/{ver}… / LiveHime/{ver} os/Windows pc_app/livehime build/{build}
Origin/Referer: 官方不发
client_traceid: <非空才发>
```

## 1.2 响应

统一信封 `{code:int, message:string, ttl:int, data:object}`;
parser 第一步校验 `code` 存在, `code!=0` 走错误分派, `code==0` 才进 `data` 解析。

| code | 语义 → 动作 |
|---|---|
| 0 | 成功 |
| -101 | 未登录 → 登录流程 |
| 60024 | 需人脸 → `data.qr` 出码 |
| 60043 | 需人脸 → `auth-middle.html?source_event=200&mid=` |
| 70003 | 「获取推流地址失败,请稍后重试」= 服务端地址分配失败(实网 2026-11-25 观测;与人脸未过审是否相关未定,官方无特判=按 message 提示重试)。2026-11-26 再观测:带 `backup_stream=1` 但缺 `backup_stream_type`/`stream_biz` 伴参的备流请求命中 70003,同会话普通开播成功;builder 已按 E-19 对齐恒注伴参,**是否因缺参被拒待对齐后复验**(2026-11-26 复验:伴参补齐后转 70002,见下行) |
| 70002 | 与 70003 同 message「获取推流地址失败,请稍后重试」。实网首观测 2026-11-26:备流请求已带 `backup_stream_type=2&stream_biz=Paylive` 但**仍缺 `src`** 时命中——错误码由 70003 变 70002 = 伴参已达服务端、分配流程走得更深;结合 E-19/G1(官方恒传 src)推断备流地址分配依赖 src,补齐后是否消解待复验。**复验 2026-11-26(src=8 已注入,仍 70002)**:body 与官方 Normal 默认字面量全等(`backup_stream=1&backup_stream_type=2&stream_biz=Paylive&type=1&checked=0&src`,LIVE-CHAIN §2.5 终稿)⇒ 差异不在 body。官方结果回调(`FUN_180d63460`,§2.7)对 startLive 失败**无回退重试**=直播姬同样会报此错,插件的「回退普通开播」是插件自有设计。**首要嫌疑=access_key 缺失**(公共参数注入器恒带非空 App OAuth 身份,服务端按它判定;先例=stopLive 60034),待扫码重登补 access_key 后复验。**复验 2026-11-26(扫码重登后仍 70002)**:access_key 假设**未获测**——refresh_token 交换本身失败(-101,见 60034 行),官方 web 登录用户的 access_key 可能同样为空(单 appkey 实证)⇒ 嫌疑降级。**排除清单**:body 参数(=官方默认字面量,指令级)、appkey(二进制内唯一)、src(=stream_line[0].src=8 已注入)、错误码特判(官方无)。**剩余候选**:①传输层指纹(Referer/Origin/cookie 完整度/traceid 格式)②服务端按账号/房间不下发备流资格(则官方同账号也会命中 70002=直播姬同样报错)。**裁决实验(定案前不再改参)**:同账号跑官方直播姬开播——若也报「获取推流地址失败」⇒②成立,插件对齐已到终点;若官方成功⇒①,以代理 capture 官方 startLive 对包定差异 |
| 60034 | stopLive 政策拦截「系统维护仅支持直播姬关播」(实网 2026-11-25)。根因=官方公共参数 `FUN_180063f70` 恒带 App OAuth `access_key`，插件旧实现漏发；现已用扫码 `refresh_token` 调 `/api/v2/oauth2/refresh_token` 换取并签名发送，待真实扫码/停播实测（E-15）。**实网复验 2026-11-26:E-15 的交换链路实测失败——web 扫码 poll 返回的 refresh_token 用官方同款 appkey(二进制内唯一 appkey=aae92bc66f3edfab,strings 实证)/同款构造 POST refresh 端点返回 -101「账号未登录」,与探针无效 token 同码 ⇒「web token 可直接换 access_key」为 E-15 [推断] 被证伪;真实交换还需什么(web↔app token 桥接)未知。停播已恢复正常但无法区分是 access_key 生效还是维护窗口结束——60034 归因存疑,以 capture 为准。** 备用口径=OBS 停止推流，无推流数分钟后服务端自动结束场次 |
| `data.notice` 非空 | 弹窗阻断(§4.1 第12步) |
| `data.risk_extra.v_voucher` | `passport…/captcha`(geetest4)→重试 |

## 1.3 证据

- 管线与逐端点一致性: E-09(13 个 builder 反汇编序列)、E-11
- appsign: E-04(`FUN_1804b3d60` 453 调用点;`0123456789abcdef`+16字节摘要;`FUN_1804b5f10` 样本)
- csrf 注入器: E-09 §9;appkey/ts 内联: E-09 §10
- buvid3 硬校验 / UA 模板 / 无 Origin-Referer: E-02 §5、§7;E-08 §7
- 信封/错误码: E-10(`Parse … missing \`code\` field` 簇)、E-06(H5 状态码)、F-180144bb0(`data.need_face_auth/data.qr`)
- 仓库 appsign 同构实现: `BiliApi::appsign()`

---

# 2. 登录

## 2.1 qrcode.generate

### 请求
```
GET https://passport.bilibili.com/x/passport-login/web/qrcode/generate
```
无业务参数, 无签名, Cookie 可空。

### 响应
```json
{"code":0, "data": {"url": "<二维码原文>", "qrcode_key": "<K>"}}
```
解析动作: 取 `url` 绘码, `qrcode_key` 交轮询。

### 证据
- 官方: 由 CEF 远程页 `mini-login-v2` 的 JS 调用(E-06 端点清单+JS 原文)。
- 仓库: `getQrCode()` 字段一致 ✅。

## 2.2 qrcode.poll

### 请求
```
GET https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=<K>
```
无签名。**节奏: 官方 H5 固定 2000ms**(JS 内 `2000`×4 实证)。

### 响应
```json
{"data": {"code": 0|86038|86090|86101, "message": "...", "url": "<成功时 crossDomain 链>", "timestamp": ...}}
```
| data.code | 含义 → 动作 |
|---|---|
| 0 | 成功 → 取 `data.url` 走 §2.3 |
| 86038 | 失效 → 重新 generate |
| 86090 | 已扫码待确认 → 继续轮询 |
| 86101 | 未扫码 → 继续轮询 |

### 证据
- 官方: H5 JS 内 86038/86090/86101 各 1 处、`2000` 4 处(E-06)。
- 仓库: `qrLogin()` 同码集 ✅;轮询实现 `src/ui/console.cpp`:**2s × 90 次(≈3 分钟)** ✅(与官方 H5 2000ms 一致,A6 已收口)。

## 2.3 crossDomain(换 Cookie)

### 请求
```
GET <data.url> = https://passport.biligame.com/x/passport-login/web/crossDomain?ticket=<T>&gourl=<G>&first_domain=<D>
```
跟随 302 链。

### 响应
`Set-Cookie: SESSDATA=…; bili_jct=…; DedeUserID=…` → 官方直接落 Chromium CookieJar(域名/过期/轮转管理)。

### 证据
- 官方: E-06(crossDomain 链)、E-08 §1(Cookie.* 配置族)。
- 仓库: 手动二次 GET 收 Set-Cookie `qrLogin()`(2026-08 新链适配)✅;持久键(SESSDATA/bili_jct/DedeUserID/buvid3/buvid4)已回写 config.json(`PERSIST_COOKIE_KEYS`)✅(A7 已收口)。

## 2.4 sso.list(跨域 SSO 传播)

### 请求
```
GET https://passport.bilibili.com/x/passport-login/web/sso/list?csrf=<bili_jct>
```
成功后逐个跟随返回的跳转 URL。

### 响应
`data.sso`: **URL 字符串数组**(H5 一手代码钉死:`Yy()` 携 `csrf=getCookie(bili_jct)` 请求,
`Wy()` 取 `code==0` 后遍历 `data.sso` 逐 URL 发请求刷新对应域 Cookie)。

### 证据
- 官方: E-06(H5 端点清单)+ E-06 JS 反混淆(`Wy`/`Yy` 函数体, `data.sso.map`)。
  仓库: `propagateSso()` 已实现(csrf 透传 + 逐 URL follow)✅(B3 已收口)。

## 2.5 captcha(登录风控, geetest4)

### 请求
```
POST https://passport.bilibili.com/x/passport-login/captcha
```
携带 geetest 结果回填 `captcha_key` 后重试 generate/poll。

### 响应
`geetest4` 参数(`support_captcha_types:"geetest4"`)+ 通过后 `captcha_key`。

### 证据
- 官方: E-06(JS 内 geetest×56、captcha_key×6、recaptcha×2)。仓库: 原生扫码路线不走 captcha（H5 内完成）。

## 2.6 getkey(App OAuth 路线, 第二条登录路线)

### 请求
passport 域 `getkey`, 无业务参数。

### 响应
```json
{"data": {"hash": "<salt>", "key": "<RSA公钥PEM>"}}
```
用于口令 `RSA(hash+password)` 加密。

### 证据
- 官方: F 锚 `getkey` 解析簇 E-10 @0x1145e65(`data.hash`,`data.key`);URL 明文 `passport.bilibili.com/api/v3/oauth2/login` E-05。

## 2.7 login-v3(App OAuth 登录)

### 请求
```
POST https://passport.bilibili.com/api/v3/oauth2/login
```
用户+RSA 加密口令等, 请求参数集 **(未定, U5)**。

### 响应
```json
{"data": {
  "token_info": {"mid":…, "access_token":"…", "refresh_token":"…", "expires_in":…},
  "cookie_info": {"cookies":[{"name","value",…}], "domains":[…]}
}}
```
解析动作: 存 access_token(access_key 路线)+按 domains 直接发 Cookie。

**扫码登录的真实来源(2026-11-26)**：官方 H5 `mini-login-v2` 的 qrcode poll 成功返回
`data.refresh_token`/`data.timestamp`，H5 通过 CEF 桥 `auth/setRefreshToken` 交给原生；
原生再用 `/api/v2/oauth2/refresh_token` 换 `data.token_info.access_token`（=公共参数
`access_key`）与 cookie_info。详见本地逆向档案 E-15（`re/evidence/15-access-key-60034.md`，不入 git）。

### 证据
- 官方: E-10 @0x1145f5a/@0x114603b(`data.token_info.*`、`data.cookie_info.cookies/domains` 簇)。
- refresh 构造/解析：F-1804bf080 / F-1804bf670（E-15）。

## 2.8 check-auth-token

### 请求
GET, App 路线 token 校验, 参数 access_key(值未钉死)。

### 响应
```json
{"data": {"mid":…, "expires_in":…, "access_token":"…"}}
```

### 证据
- 官方: E-10 @0x11463bd。

## 2.9 nav(原生登录态确认)

### 请求
```
GET https://api.bilibili.com/x/web-interface/nav?appkey=…&ts=…&sign=…
```
无业务参数; **官方带 appsign**(builder 内 sign 插入在 URL 前)。

### 响应
```json
{"data": {"isLogin": true, "mid": 123, …}}
```

### 证据
- 官方: F-1804c2200(builder: 无业务参数+sign+URL)。
- 仓库: `checkLoginStatus()` 消费同字段, **已按本 builder 带 appsign**(ts+appkey+sign)✅。

---

# 3. 房间信息与开播前

## 3.1 getInfoByUser / 3.2 getInfoByRoom(共用构造器)

### 请求
```
GET https://api.live.bilibili.com/xlive/app-room/v1/index/getInfoByUser
GET https://api.live.bilibili.com/xlive/app-room/v1/index/getInfoByRoom
```
| 参数 | 必填 | 说明 |
|---|---|---|
| `room_id` | 由调用方 | builder 字面量;getInfoByUser 的解析簇**不回读 room_id** ⇒ 服务端按会话/传参定位房间,调用方在调用前已持有房间号 |
| `device` | ✅ | **`"pc"`**(builder 内联 2 字节字面量 0x6370,钉死) |
| 公共+sign | ✅ | §1(缺则 -400,已实网验证 code=0) |

### 响应(`data`, 官方解析全字段)
```
area_v2_id, area_v2_name, parent_id,
live_status(0未开播/1直播中/2轮回), have_live,
identify_status(实名/人脸态), lock_status_v2, lock_time, lock_reason,
master_level, master_score, master_next_level_score,
medal_status, medal_name, guard_count, guard_warn.{is_warn,expired,will_expired},
join_slide, fans_club, appointment_before_live,
anchor_content, content_is_open, watched_show, ab,
hot_rank_info_v3, area_rank_info_v2, dm_length_max, xtemplate_config, rcost, fc_num
```
解析动作: `live_status/have_live` → 是否已有进行中场次(§6.1 前置);
`identify_status` → 预检; `lock_status_v2/lock_reason` → 封禁提示。

**实网更正(2026-11-25,登录态在播中实测)**:getInfoByUser 的 wire 响应**不含**
`live_status/have_live/lock_*`(全树检索无命中)——解析簇是 common_service 共用键表,
这些字段的实际供给方是 §3.3 room/GetInfo。仓库登录链房间态走 §3.3。

### 证据
- 官方: builder F-1800c3810 / F-1800c0050(参数仅 `room_id`、`device`,值已钉死);
  解析簇 E-10 @0x112c5bb、@0x112c8f6(内部名 `get-room-info`, common_service_impl2.cpp)。
- 仓库: `getRoomInfo()` 保留(通用查询);登录链房间态改走 §3.3 room/GetInfo
  (实网钉死 live_status/lock_status 只在 §3.3 供给);room_id 引导已切 app-blink
  room/GetInfo(§3.3,B1 关闭),失败回退 `room/v2/Room/room_id_by_uid`
  (仅 WARNING,`use_legacy_endpoints=true` 时直走老端点)。

## 3.3 app-blink room/GetInfo

### 请求
```
GET https://api.live.bilibili.com/xlive/app-blink/v1/room/GetInfo?uId=<mid>&<公共+sign>
```

### 响应(`data`, 解析器 F-1802b3690 钉死, 2026-11-25)
```
room_id ★, live_status, live_key, title, have_live, identify_status,
lock_status_v2, lock_time, lock_reason, area_v2_id, area_v2_name, parent_id,
master_level/score/next_level_score, medal_status/name, guard_count,
guard_warn.{is_warn,expired,will_expired}, join_slide, fans_club,
appointment_before_live, anchor_content, content_is_open, watched_show,
ab.pc_dm_scroll_ab, hot_rank_info_v3, area_rank_info_v2, rank_entrance_info,
room_rank_info, revenue_material_md5.wealth, rcost, fc_num, dm_length_max,
xtemplate_config, multi_screen_info.double_screen_visible,
multi_user_conn.entrances, collaboration_live_display_info
```
需登录态(未登录 -101,实网实证)。`room_id` 即房间引导正解(B1)。
**实网观测(2026-11-25)**: 在播时 `live_status=1`(0=未开播对照同端点观测);
锁定字段 wire 实际名为 `lock_status`+`lock_time`(非解析簇的 `lock_status_v2`,
`lock_reason` 未下发)。登录链房间态(场次/锁定/进行中场次判据)以本端点为准。

### 证据: builder F-1802b1e60(字面量 `uId`+sign;派发回调=F-1802b3690)。
仓库: `getRoomInfoByUid()`;登录链默认先走本端点,失败 WARNING 后回退老端点。

## 3.4 getHomePageLiveVersion

### 请求
```
GET https://api.live.bilibili.com/xlive/app-blink/v1/liveVersionInfo/getHomePageLiveVersion
```
| 参数 | 必填 | 说明 |
|---|---|---|
| `system_version` | ✅ | 官方 builder 仅此业务参数 |
| 公共+sign | ✅ | 仓库另显式带 `ts`(合法变体) |

### 响应
```json
{"data": {"build": 11349, "curr_version": "8.16.0.11349",
          "download_url":"…", "file_size":…, "file_md5":"…",
          "instruction":"…", "is_mandatory":…, "is_push":…, "hdiffpatch_switch":…}}
```
解析动作: 双方都取 `build`/`curr_version` 作为**后续全部请求的公共参数值**;
官方另解析升级提示组(仓库不需要)。

### 证据
- 官方: builder F-18003f460;解析簇 E-10 @0x1111a3c。
- 仓库: `ensureVersionInfo()` ✅ 完全一致(另带 ts 合法变体)。

## 3.5 PreCheck

### 请求
```
GET https://api.live.bilibili.com/xlive/app-blink/v1/preLive/PreCheck
```
参数: **无业务参数**(builder F-1804c4c00 指令级:map=公共参数,直接签名;
旧文「%s 动态格式化」是 URL 模板字样,非参数)。**传输层 = GET**——实网实测
(2026-11-25 wire)POST 该路径 405 Method Not Allowed;csrf 不注入
(GET 管线无 csrf,§1 csrf 规则=仅 POST)。

### 响应(实网实测形状)
```
{code, message, ttl, data.realname_status}
```
`realname_status`=0 视为实名未过(仓库阻断)。二进制解析簇里的
`data.game_promotion.{need_notice, biz_id, title, content, effect_button,
no_effect_button, cool_down_days}`(E-10 @0x112cfad,合规弹窗/推广拦截)**本会话
实测未下发**,该簇可能服务别的 preLive 报文;解析保留以防服务端按场景下发。

### 证据: builder F-1804c4c00;解析簇 E-10 @0x112cfad;实网 GET/响应 2026-11-25。仓库 `preLiveCheck()`。

## 3.6 GetRoomPreLiveStatus

### 请求
```
GET https://api.live.bilibili.com/xlive/app-blink/v1/index/GetRoomPreLiveStatus
```
builder 无业务参数(指令级:仅公共 map 签名;旧推测「仅 room_id」不成立,按会话定位)。

### 响应
解析器 F-180447550(`get-pre-live-status-result`)**只回读 `data.status`(int)**;
取值域未钉死(仓库 `getRoomPreLiveStatus()` 透出原始值记日志观测)。

### 证据: builder F-1804466d0 + 派发解析回调 F-180447550。仓库 `getRoomPreLiveStatus()`。

## 3.7 prelive-info(播前房间资料, 含开播拦截)

### 请求: app-blink preLive 详情接口(路径组 E-05)。

### 响应
```
data.{title, cover, multi_cover, area, schedule}
auditStatus / auditReason / selectStatus
need_intercept, intercept_toast
cover_title_start_live_intercept      ← 标题/封面不合规 → 开播拦截
multi_cover_start_live_intercept      ← 多封面不合规 → 开播拦截
```

### 证据: 解析簇 E-10 @0x1123989、@0x1123bf5、@0x1123c88。仓库 ❌(C2/C3);
部分替代:`PreCheck` 覆盖实名与 `game_promotion.need_notice`,startLive 的 `data.notice` 覆盖服务端弹窗阻断;封面/标题合规拦截(need_intercept/cover_*_intercept)**仍未实现**。

## 3.8 Area/getList

### 请求
```
GET https://api.live.bilibili.com/room/v1/Area/getList
```
无参数。

### 响应
```json
{"data": [ {"id":…, "name":"…", "parent_id":…, …} ]}
```

### 证据: builder F-18013b430(无参)。仓库 `getPartitionList()` ✅。

## 3.9 人脸状态组

### 请求
`preLive/GetRealnameApplyStatus`、`preLive/IsUserIdentifiedByFaceAuth`、`preLive/ReportFaceRecognition`(路径 E-05, 参数 **(未定)**)。

### 证据: 端点明文 E-05;字符串锚点命中但无 xref(DumpFlow 输出)。仓库: 60024 二维码、60043 `auth-middle.html?source_event=200&mid=` ✅；`IsUserIdentifiedByFaceAuth`/`GetRealnameApplyStatus` 未单独调用（PreCheck 覆盖实名）。

---

# 4. 开播与房间设置

## 4.1 startLive ★核心(内部名 open-live-room)

### 请求
```
POST https://api.live.bilibili.com/room/v1/Room/startLive
Content-Type: application/x-www-form-urlencoded
Cookie: <会话>  client_traceid: <非空才带>
```
官方两个构造路径(F-180141ed0 标准 / F-180146070 备流·多路), 注入条件=builder 实际逻辑:

| 参数 | 注入条件(官方) | 值 | 仓库 |
|---|---|---|---|
| `room_id` | 恒注 | 房间号 | ✅ `:365` |
| `area_v2` | 恒注 | 二级分区 | ✅ `:367` |
| `checked` | **恒注 `"0"`**(itoa(0) 指令实证) | `"0"` | ✅ 恒注 |
| `type` | 仅当本地配置 `custom-rtmp` 关(F-180141ed0 `FUN_180904490(cfg,"custom-rtmp")==0` 分支) | **`"1"+开播类型索引`**(`param_7+'1'` 指令实证;custom-rtmp 开时固定 `"1"`) | ✅ 恒注 `"1"`(默认配置+索引0) |
| `recommend_protocol` | 非空才注 | 合法值含 `"rtmp"`(§响应第3步默认值实证); 其余 U1 | ✅ 隐藏配置透传 |
| `recommend_ip` | 非空才注 | §5.7 测速结果 | ✅ 隐藏配置透传 |
| `multi_screen_type` | 多路时 itoa(1) | `"1"` | ❌ |
| `multi_screen_info` | 非空才注, `,` 连接(0x18111260d) | 联播列表 | ❌ |
| `xcode_suffixes` | 非空才注(标准 builder) | 后缀列表 | ❌ |
| `backup_stream` | 备流 builder(F-180146070) | `itoa(param_8)`(builder 第 8 参,指令实证) | ✅ `startLive(with_backup=true)`(「开播(含备流)」) |
| `backup_stream_type` | 备流 builder | `itoa(param_9)`(第 9 参,数字);官方恒传 `2`(E-19) | ✅ 默认恒注 `2`;探针 `backup_stream_type_probe` 非空可覆盖(实网复核) |
| `stream_biz` | 备流 builder | `param_10`(第 10 参,字符串原样搬运);官方恒传 `"Paylive"`(教室房 `"ClassRoom"`,E-19) | ✅ 默认恒注 `Paylive`;探针 `stream_biz_probe` 非空可覆盖(插件不做教室房) |
| `src` | 备流 builder,**仅 `param_6==0`** | `itoa(param_5)`(键 `DAT_181120655`);**值域=服务端 `get_stream_by_roomId` 响应 `data.stream_line[].src`(整数)**,经 service 子对象 `vt[0xf8]` getter 原样透传(E-19);同分支内 `checked="0"` | ✅ 备流路径(2026-11-26):开播(含备流)先调 `getActiveStream` 取 `stream_line[].src` 原样透传,checked≠0 优先/无则首条/取不到告警不带。注:官方 `RequestStartLive` 恒走备流 builder(U6)⇒ 官方普通开播**也恒带** src+备流三参;仓库「开播」=标准 builder 形态(实网兼容已证),是否收敛到官方恒带形态待定 |
| `client_traceid` | 备流 builder,非空才注 | `param_11` | ✅ 请求头 |
| `platform=pc_link` `version` `build` | 官方公共参数 | — | ✅ 显式放 body `:366,371-372` |
| `csrf_token` `csrf` `ts` `appkey` `sign` | 管线 | §1 | ✅ |

**备流语义(用户澄清 2026-11-26)**:`backup_stream=1` 请求的是**第二条独立推流通道**
(`protocols_backup[]`/`rtmp_backup`,自己的地址与推流码),与主流可同时推;
主流失效时转发侧自动切到备流。**不是**同 key 的多个地址(同 key 二次连接会被拒)。
U6 已闭合(E-19:官方恒传 `backup_stream_type=2`/`stream_biz="Paylive"`);builder 默认
恒注官方值,`config.json` 的 `backup_stream_type_probe`/`stream_biz_probe`(非空时覆盖
默认值)保留作实网复核。

### 响应
```json
{"code":0, "message":"0", "data": {
  "room_type": <int>,
  "need_face_auth": <bool>, "qr": "<人脸二维码URL>",
  "live_key": "<推流密钥>",
  "protocols":         [ {"protocol":"rtmp|…", "addr":"…", "new_link":…, "provider":"…"} ],
  "protocols_backup":  [ 同结构 ],
  "rtmp": {"new_link": <必存在>, "code": "<旧推流码>", "addr": "<旧地址>"},
  "xcode_stream": [ {"suffix":"…", "protocols":[…]} ],
  "sub_session_key": "<多路子会话>",
  "notice": {"type","status","title","msg","button_text","button_url"},
  "risk_extra": {"v_voucher": "<风控凭证>"}
}}
```
**解析器 F-180144bb0 执行序**(指令级还原):
1. `code!=0` → 错误分派(60024→`data.qr`);
2. 解析 `room_type`(int)、`need_face_auth`(bool)、`qr`(string);
3. 协议默认值写入字面量 `"rtmp"`(0x706d7472 小端, 实证);
4. 读 `data.live_key`;
5. `data.protocols[]` 非空 → 逐条转 StreamTarget(字段 `protocol/addr/new_link/provider`, 与 live_key 配对, F-1801ef020);**直连推流(addr 非空)的推流码 = 条目自带 `code`**(实网实证 2026-11-26:拿 live_key 当推流码,edge 在 publish 后 ~0.4s 拒绝连接/EPIPE;live_key 仅为官方客户端内部会话配对存储,不是推流密钥);
6. 否则旧式回落: **`rtmp.new_link` 必须存在**(缺→无效), 再读 `rtmp.code`、`rtmp.addr`(addr 存在=有效);
7. `data.protocols_backup[]` 同 5(含 code 推流码规则);
8. `data.xcode_stream[]` 逐项 memcmp 目标协议 → 取子对象 `protocols`(协议↔xcode 后缀映射);
9. 3-8 后无效 → `Parse open-live-room failure: missing fields!` + 原文 → 失败;
10. 成功 → 日志 `pay live rtmp_info.addr: / new_link: / key:` → 地址+密钥交 OBS;
11. 读 `sub_session_key`;
12. 透传 `notice.*`(弹窗阻断)+`risk_extra.v_voucher`(→§2.5 同族风控)。

### 证据
- 构造: F-180141ed0、F-180146070(E-09 §1/§2 序列);`checked` itoa 链 F-180708c20→F-1806dc6e0;`type` 键 `DAT_181114fa6`;URL xref 0x180142cef/0x180146f42 E-03。
- **F-180146070 参数→键映射(2026-11-25 指令级钉死,全文
  `re/evidence/07-full-decompile-secret-dll.txt:661-1204`)**:签名 14 参;
  `room_id=param_3` / `area_v2=param_4` / `src=itoa(param_5)`+`checked="0"`
  (仅 `param_6==0`)/`type=param_7+'1'`(custom-rtmp 关)/
  `backup_stream=itoa(param_8)` / `backup_stream_type=itoa(param_9)` /
  `stream_biz=param_10` / `client_traceid=param_11` /
  `multi_screen_type="1"`+`multi_screen_info=param_13`(仅 `param_12==1`)。
  `recommend_ip`/`recommend_protocol` 取 `GetSecretCore()` 虚表 +0xa0→(+0x120/+0x130/
  +0x140) 的测速结果,**均为非空才注**(+0x130 空则用 +0x140)。
- **两 builder 的归属**:`secret::LiveStreamingServiceImpl` 是 RTTI 命中
  (`.?AVLiveStreamingServiceImpl@secret@@`,TD@0x181565d50,COL@0x181123380,
  vtable@0x18111f628);槽 13(+0x68)=F-180146070(备流/多路),
  槽 14(+0x70)=F-180141ed0(标准)。二者均无静态 CALL xref(纯虚调用),
  实参由 app 层(`bililive.dll` 的 `LivehimeLiveRoomController::OpenLiveRoom`)
  经 `secret::LiveStreamingService::StartLiveInfo` 结构传入。
- **U6 已闭合(2026-11-26, E-19)**:`bililive.dll` 的 `RequestStartLive`(入口 `0x180d603a0`,
  0x180d604e8 在函数体内)尾部 **14 实参 vcall** 固定打槽 `0x68`(slot13)=F-180146070(备流/多路);
  实参字面量 `param_8=1 / param_9=2 / param_10="Paylive"`(教室房 `"ClassRoom"`) ⇒ 实网 URL
  **恒带** `backup_stream=1&backup_stream_type=2&stream_biz=Paylive`;14 实参几何上排除
  12 参的槽 `0x70`(标准 builder)。同 vcall 前置 getter 链:service 子对象 `vt[0x60]`/`vt[0x98]`/
  `vt[0xf8]`/`vt[0x100]` = room_id/area_v2/src/checked;`src` 值域 =
  `get_stream_by_roomId` 响应 `data.stream_line[].src`(E-19 §三)。
- 解析: F-180144bb0 全文 E-11;字段簇 E-10 @0x1121fe3/@0x11220d6(窗口原文 E-08 §8)。
- 仓库: 参数与官方对齐(checked/type/透传已补;备流请求默认恒注
  `backup_stream=1&backup_stream_type=2&stream_biz=Paylive`,E-19 对齐 2026-11-26);
  响应解析按 F-180144bb0 第2-10步移植
  (protocols[]↔live_key 会话配对存储,但直连线路推流键=条目自带 `code`——
  实网实证修正 2026-11-26,见第5步注、rtmp.new_link 存在性校验、need_face_auth 阻断、
  notice 阻断 B4①、v_voucher 透出);第7步 `protocols_backup[]` 同规则,
  备流与多协议线路**全部只读展示**(`config.stream_targets`,地址/推流码可复制),
  插件不写 OBS 配置(ADR-0005)。**多路/xcode_stream 为明确不做项**(联播范围外)。

## 4.2 UpdatePreLiveInfo(播前改资料)

### 请求
```
POST https://api.live.bilibili.com/xlive/app-blink/v1/preLive/UpdatePreLiveInfo
```
| 参数 | 说明 |
|---|---|
| `mobi_app` | **`"pc_link"`**(builder 内联 7 字节字面量,钉死) |
| `title` | 非空才注入(长度检查后 map 插入) |
| `ai_title_task_id` | 非空才注入(`has_task_id` 日志实证) |
| +csrf+sign | §1 |

**管线例外(指令级实证,F-180151c10)**:appsign 先于业务参数执行——sign 只覆盖
公共参数;`mobi_app`/`title`/`ai_title_task_id` 与 csrf 对(FUN_1804b5330 在 builder 末尾)
都是 sign 插入之后才进 map ⇒ 业务值不在签名覆盖内。这解释了契约旧文「封面/简介等
运行期字符串注入」:它们本就不参与签名。

### 响应: 信封级(code 校验)。

### 证据
- 官方: F-180151c10(含日志 `[UpdateRoomInfo] room_id=… title_length=… has_task_id=… task_id=…`)。
- 仓库: `updatePreLiveInfo()` 默认路径;老端点 `room/v1/Room/update` 仅在
  `use_legacy_endpoints=true` 保留(B2 关闭)。

## 4.3 AnchorChangeRoomArea(播中改分区)

### 请求
```
POST https://api.live.bilibili.com/xlive/app-blink/v2/room/AnchorChangeRoomArea
```
| 参数 | 值 |
|---|---|
| `room_id` | 房间号 |
| `platform` | `"pc_link"`(builder 字面量实证) |
| `build` | 当前 build |
| `area_id` | 新分区 |
| +sign | §1 |

### 响应: 信封级。

### 证据: F-1801e8790(四参数+platform 字面量全实证)。仓库 `changeRoomArea()`(播前/播中改分区统一走此端点,platform/build 经公共参数注入)。

## 4.4 bind-live-topic / get-live-topic

### 请求
bind(POST)/get(GET), 参数动态注入 (未定)。

### 响应(get)
```json
{"data": {"topic_id":"…", "topic_name":"…", "act_id":…, "status":…}}
```
bind: 仅 `message`。

### 证据: F-1801496e0、F-18014aa20;簇 E-10 @0x11221c9。仓库 ❌。

## 4.5 封面 / AI 标题 / 敏感词 / 临时设置

### 请求
- 封面上传: `POST https://api.bilibili.com/x/upload/web/image`(multipart, 参数未钉死)
- 封面评分: `GetCoverAdviceAndQualityScore`
- AI 标题: `generateAiTitle` → 轮询 `getAiTitleResult`
- `sensitiveWordsCheck`、`updateTemporaryConf`(簇 @0x1123e61)

### 响应
- 评分: `data.{score, score_color, domains, params, recommended_tags}`
- AI 结果: `progress_text, fail_reason` + 结果列表

### 证据: E-10 @0x112d502、@0x1125147、@0x1123e61;URL E-05。仓库 ❌(C3/C6)。

---

# 5. 推流

## 5.1 主路(与本插件同机制)

`startLive` 所得 `addr+key` → RTMP 输出(libobs)。官方内置 OBS(obs_proxy), 仓库交用户 OBS。**等价** ✅。

## 5.2 GetUpStreamConfig

### 请求: GET app-blink/v1/live/GetUpStreamConfig(+sign)。

### 响应
```
data.latency_measure_config.{t1, t2, acceptable_clock_max_error_ms,
                             retry_interval_ms, max_retry_count, kv_state, content}
data.sei_interval, sei_interval_switch
```

### 证据: parser F-1801df430(GetUpStreamConfigParser);簇 E-10 @0x1125559。仓库 ❌。

## 5.3 GetQualityData / QualityCallback

**语义修正(2026-11-25 解析器钉死)**:本对端点是**上行线路质量探测与回执**
(RTTI `StreamQualityData`、日志 tag `get-stream-quality-data`),与视频码率档位**无关**;
§5.7 的响应形状实为本端点同族。原「质量档列表」推测不成立。

### 请求 GetQualityData
```
GET live/GetQualityData → 参数 = 公共 + streamName + key,sign 覆盖全部(标准管线)。
```
`streamName`/`key` 值语义 = 客户端会话内部值(U-qs,取值构造未钉死)。

### 响应(解析器 F-18019cf50)
```
data.{fpsResult, reason, recommendAddr,
      scheduleInfo.{edge, mode, protocol},
      openInfo|serverInfo|userInfo.{ip, country, province, city, isp}}
```

### 请求 QualityCallback
参数 = 公共 + `streamName` + `addrChanged`("true"/"f") + `reason` + `time` +
`recommendAddr` + `autoReport`("true"/"f") + `extensions`,sign 覆盖全部。
解析器 F-18019f6a0 不回读任何字段(只认 code)。

### 证据: builder F-18019bc00/F-18019e150(URL 字面量 findBytes+xref);解析器上列。仓库 ❌。

## 5.4 GetUpStreamVideoQualityConfig ★视频码率的正解来源

### 请求
```
GET upStreamConfig/GetUpStreamVideoQualityConfig
```
参数 = 公共 + sign(sign 只覆盖公共参数),`bitrate_intervention="1"` 在 **sign 之后**注入
(F-1802d40f0 指令级:4b3d60 计算 sign → 回写 map → 再插 bitrate_intervention)。
管线例外同 §4.2 UpdatePreLiveInfo。

### 响应(解析器 F-1802d5060 `ParseStreamVideoQualityConfigResponse`)
二进制回读的键:`recommend_bitrate`、`strategy_source`、`data.bitrate_config.{default_bitrate,
recommend_bitrate_member_intervention, recommend_bitrate_area_intervention}`、
`recommend_bitrate_area_v2_interventions[].area_v2_ids`。

**实网形状(2026-11-25 wire 实测,登录态 GET)**:顶层**没有** `recommend_bitrate`/`strategy_source`;
干预列表嵌在 `data.bitrate_config.recommend_bitrate_area_intervention.recommend_bitrate_area_v2_interventions[]`,
每档 = `{area_v2_ids[], recommend_bitrate, strategy_source}`(如 high=6000/mid=4000);
`recommend_bitrate_member_intervention` 可为 null;兜底 = `default_bitrate`(实测 3000)。
即官方口径的「推荐码率」= 按二级分区命中干预档,未命中回落 default。

**无档位枚举列表**——官方口径是「推荐/默认码率 + 分区/会员干预」,用户端只做码率数值,不做选档。

### 证据: builder F-1802d40f0;解析器 F-1802d5060。
仓库 ✅ `videoQualityConfig()`(按实网形状解析:干预列表命中二级分区,回落 default_bitrate);
控制台只读展示推荐/默认码率与编码能力,不写 OBS 编码器(视频码率由用户在 OBS 中自行配置);不做选档。

## 5.5 CanUseH265Encoder / CanUseAV1Encoder

### 请求
GET,无业务参数(纯公共+sign;builder F-1802d04b0/F-1802d0b20 无参数插桩)。

### 响应(解析器逐一定死,旧清单 {can_enable,can_hard_encode,md5,sign} 有误)
- H265(F-1802d08c0 `ParseCanUseHEVCEncodeResponse`):只回读 `data.can_enable`。
- AV1(F-1802d0f30 `ParseCanUseAV1EncoderResponse`):只回读 `data.can_hard_encode`。

### 证据: URL findBytes+xref,builder/解析器如列。仓库 ✅ `canUseH265Encoder()`/`canUseAv1Encoder()`(只回读各自唯一字段)。

## 5.6 GetClientXCodeAbility / 时移

### 响应: `data.recommend_xcode_suffix`;时移 `data.{time_shift, min_time_shift, max_time_shift}`。

### 证据: E-10 @0x112cc66、@0x112cda4。仓库 ❌。

## 5.7 GetUpStreamSpeedTestInfo(get-speedtest-addr)

### 请求: 测速探测(+sign)。

### 响应
```
data.{fpsResult, recommendAddr, reason},
data.nodes[],
data.{userInfo, serverInfo, openInfo}.{ip, province, country, city, isp},
data.scheduleInfo.{protocol, mode, edge}
```
⇒ 官方 `recommend_protocol`/`recommend_ip`(§4.1)的数据源: **开播前先测速再带上**。
直播中热切节点: `GetUpStreamChangeAddr`。

### 证据: 簇 E-10 @0x11240d7;builder URL E-05。仓库 ❌(C5)。

## 5.8 ARTC(WebRTC)第二路线

### 请求
`POST https://api.live.bilibili.com/xlive/open-interface/v2/rtc/getJoinToken`(+getAsrProxyServer)

### 证据: 字符串实证 WHIP(`WHIP TCP bind error`)/Colive/`check rtc reconnect`;实体 `obs-bilirtc-loader.dll`、`bilibili-rtc.dll`;SDP/ICE 细节 U3(E-08 §2)。

## 5.9 房间时延/时间监控(簇 @0x1125559)

`get_live_delay_measure_time` 响应: `data.{t1, t2, acceptable_clock_max_error_ms, retry_interval_ms, max_retry_count, kv_state, content}`。证据: E-10。仓库 ❌。

---

# 6. 停播与收尾

## 6.1 stopLive(内部名 close-live-room)

### 请求
```
POST https://api.live.bilibili.com/room/v1/Room/stopLive
```
| 参数 | 说明 | 仓库 |
|---|---|---|
| `room_id` | **builder 业务参数** | ✅ |
| `csrf_token` `csrf` | FUN_1804b5330 注入 | ✅ |
| `ts` `version` `platform=pc_link` `build` `access_key` | 公共参数注入器 FUN_180063f70 | ✅ |
| `appkey` `sign` | 管线 | ✅ |

### 响应
仅信封: parser F-180148980 只校验 `code/message`, 无 data 字段。

### 60034
服务端按 App OAuth 身份判定。官方每个 stopLive 都带 `access_key`;插件旧实现漏发,
故被“仅直播姬可关播”拦截。2026-11-26 已改为用扫码 refresh_token 换 access_key
并随公共参数签名发送(E-15),待真实扫码/停播实测。

### 证据: builder F-1801482d0(URL xref 0x1801484e0, E-09 §3);公共参数 FUN_180063f70(E-15);
仓库 `stopLive()` ✅(access_key 已对齐,待实网验证)。

## 6.2 停播收尾组

| 请求 | 响应解析(证实) | 证据/仓库 |
|---|---|---|
| `stopLiveAdvice`、`complete-Stoplive-question-info` | 信封级+问卷 | E-10 @0x1125559;❌ |
| `get-live-data`(播后总结) | `data.{LiveTime, AddFans, HamsterRmb, NewFansClub, DanmuNum, MaxOnline, WatchedCount}`;`data.{cover_exposure_count, watch_count, new_follow_count, new_guard_count}.{count, data_status}`;`data.{paid_rate, cover_click_rate, danmaku_rate}.{rate, data_status, last_live_change}` | E-10 @0x1142a85/@0x1142b27;❌ |
| 关播总结页 | 浏览器开 `https://play-live.bilibili.com/play-dialog` | E-05;❌ |
| 录像回放 `anchor-get-setting`/`anchor-save-replay` | `data.{is_ok, is_auto_archive, is_auto_sync_dynamic, is_fan_archive, other_edit_*, has_all_permission}` | E-10 @0x114287d;❌ |

---

# 7. 断流恢复(OBS 崩溃重启接回流)

## 7.1 get_stream_by_roomId

### 请求
```
GET https://api.live.bilibili.com/live_stream/v1/StreamList/get_stream_by_roomId?room_id=<房号>
```
业务参数仅 `room_id`+sign;公共参数组(`ts/version/platform/build`,注入器 FUN_180063f70)与其他 app-blink 请求一致。

### 响应(解析器 F-18014cf60 钉死, 2026-11-25)
```json
{"data": {"rtmp": {"addr": "…", "code": "…"},
          "stream_line": [{"src": "…", "cdn_name": "…", "checked": …}]}}
```
`rtmp.{addr,code}` = 进行中场次的推流地址与密钥(断流找回正解);
`stream_line[]` = CDN 线路表(`src` 字面量 0x181120655 实证;插件未消费)。

### 证据: builder FUN_18014bf40(E-09 §12), 派发解析回调 F-18014cf60。
仓库 `getActiveStream()`(M1b 恢复首选源;读取 `data.rtmp`,公共参数已注入)。

## 7.2 GetUpStreamRtmp

### 请求
```
GET https://api.live.bilibili.com/xlive/app-blink/v1/live/GetUpStreamRtmp?room_id=<房号>
```

### 响应(`ReconnectLiveInfo`)
```json
{"data": {"can_change": <bool>, "urls": ["<可推地址>"], "message": "…"}}
```

### 证据: builder FUN_18014d920(E-09 §12);解析簇 E-10 @0x112d6db。
仓库 `getUpStreamRtmp()`(M1b 恢复备用源: `can_change` 且有 urls 时用 `urls[0]`,
密钥复用本地留存推流码)。
配套前置: §3.1 `live_status/have_live` 判"服务端仍认为在播"(登录链/刷新链 liveStatus 透出)。

## 7.3 连麦断线(非推流)

`xInteract/ReconnectLink`、`video-conn-reconnect-link` →
`data.{accept_connection, accept_each_connection, time_out}`(E-10 @0x1147f09)。

---

# 8. 未定项(本文档诚实边界)

| # | 未定 | 出现处 |
|---|---|---|
| U1 | `protocols[].protocol` 取值集(除 `"rtmp"` 实证) | §4.1 |
| U6 | ~~备流 `backup_stream_type`/`stream_biz` 取值~~ | §4.1。🟢 **闭合(2026-11-26, E-19)**:`RequestStartLive`(`bililive.dll` 入口 `0x180d603a0`)尾部 **14 实参 vcall 固定路由到 service vtable 槽 `0x68`(slot13)=F-180146070 备流/多路 builder**(14 实参几何上排除 12 参的标准 builder 槽 `0x70`=F-180141ed0);官方恒传 `param_8=1, param_9=2, param_10="Paylive"`(教室房 `"ClassRoom"`) ⇒ 实网 URL **恒带** `backup_stream=1&backup_stream_type=2&stream_biz=Paylive`。证据 `re/evidence/19-u6-g1-closed.md`。**仓库已对齐(2026-11-26)**:备流请求默认恒注两参,探针退化为覆盖通道;70003 是否因缺参触发待实网复验 |
| U-QS | `GetQualityData` 的 `streamName`/`key` 取值构造 | §5.3 |
| U3 | ARTC/WHIP SDP/ICE 流程 | §5.8 |
| U5 | login-v3 请求参数集 | §2.7 |
| — | ~~get_stream_by_roomId 响应字段~~ 已钉死(§7.1);R4 封面上传参数 | §4.5 |

已钉死(2026-11-25, Ghidra 定点反编译 M0cDump/Dump2/Dump3 + 公开端点实网):
`device="pc"`(builder 内联 2 字节字面量)、公共参数注入器=FUN_180063f70(键集见 §1.1)、
UpdatePreLiveInfo 注入条件(§4.2 管线例外)、`mobi_app="pc_link"`(builder 内联 7 字节字面量)、
room/GetInfo 响应字段清单含 `data.room_id`(解析器 F-1802b3690,§3.3)、
`type="1"+索引`(F-180141ed0 寄存器级)、PreCheck/GetRoomPreLiveStatus 均无业务参数
(§3.5/§3.6,解析器 F-180447550 仅 `data.status`)。

复现对应功能前必须先补对应 U 项(方法见 docs/roadmap.md)。
