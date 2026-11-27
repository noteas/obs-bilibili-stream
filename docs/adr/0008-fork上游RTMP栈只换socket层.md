# rtmpsrt 重实现：fork 上游 OBS RTMP 栈，只换 socket 层

**状态**：采纳（2026-11-27）

## 背景

M4 第一版私有 SRT 输出是主仓手搓协议栈（`src/srt/` 下自写 AMF/FLV mux/RTMP client/SRT transport，
约 2000 行）。H.264/AAC 真机验收虽已通过，但「与直播姬字节对齐」路线投入大、且方向存疑。
E-27 二进制取证定案（`re/evidence/27-official-raw-rtmp-srt-same-stack.md`）：

1. 官方原始 RTMP 线路 = **纯上游 OBS obs-outputs**（`obs-outputs.dll` 无任何 B站改写痕迹），
   其被 TCP 入口接受 = 上游 OBS 字节口径已被服务器接受；
2. 官方 rtmpsrt 线路（`bvc-srt-stream.dll`）的 RTMP 层与上游 `rtmp-stream.c`+`flv-mux.c`+librtmp
   同源，B站自加补丁只有**时间戳防御与日志**；
3. ⇒ 服务器无逐字节要求，rtmpsrt 只需守**传输契约（E-24）+ 鉴权/信令契约（E-25 §1–§5）**。

据此手搓实现已整体回退（`a3a1796`），进入重实现。

## 决策

1. **符合性判据 = 服务器接受且流正常**（真机验收），不是与直播姬逐字节一致。
   字节级取证只用于**发现契约**（参数、鉴权、必须的防御），不用于对齐实现细节
   （chunk 头档位、Ack/Ping 细节、metadata 内容等一律以上游行为为准）。
2. **实现载体 = `third_party/obs-rtmp-srt`**：own fork（git submodule，
   `github.com/noteas/obs-rtmp-srt`）。基线 = 上游 OBS 32.2.0（commit `7546be72…`）的
   `librtmp/*` + `rtmp-stream/hevc/av1` + `flv-mux` + `net-if` + `happy-eyeballs` 逐字导入；
   其上独立 commit 只做两件事：
   - **socket 层 SRT 分支**（E-24 参数；新文件承载，上游文件只留分支点）；
   - output 身份 = `bvc_srt_output`、scheme = `rtmpsrt://`（ADR-0007 的注册与目录合并机制不变）。
   RTMP/FLV/AMF/重连逻辑**零改动**；HEVC/AV1 Enhanced 随上游 flv-mux 获得。
3. **第三方依赖通用策略**：只有几个文件、无需修改的 → 直接内嵌（vendored，如
   `src/md5`、`src/json11`、`src/qrcodegen`、fork 内的 `happy-eyeballs/`）；
   体量大或**需要打补丁**的 → fork 成独立仓库、以 git submodule 入构建树（如 `obs-rtmp-srt`）。
   对上游的修改一律留在 fork 里、以最小 diff + 协议标志门控，禁止在主仓复制粘贴上游代码再改。
4. B站自加的 ts 防御（每轨单调钳位、跨重连续接，E-27 §3）**默认不跟**：
   属官方自愿加固而非服务器要求，真机验收暴露问题再在 fork 里加。

## 后果

- 协议正确性继承上游（含 HLS/HEVC/AV1 等既有行为），主仓不再有自写 RTMP 的维护面与失真风险。
- fork 有自己的 git 历史：上游文件 verbatim 基线 commit + 补丁 commit 序列，
  `diff` 即「我们改了什么」的完整答案；上游升级走「重导基线 + rebase 补丁」。
- 构建新增依赖：libsrt（fork 的 SRT 传输层需要）；CMake 以 `add_subdirectory` 引入 fork。
- 服务器契约风险不变的部分仍由主仓把关：E-24 参数、`rtmpsrt://` 解析（`srt_target`）、
  服务目录合并（`srt_catalog`）留在主仓并已有离线测试。
- 验收判据放宽后，旧「假服务端逐条核对 E-25 字节」类测试退役；以真机推流 +
  契约级（参数/URL/connect/publish）测试为准。
