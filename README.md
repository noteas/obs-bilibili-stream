# OBS Bilibili 直播插件

这是一个为 OBS Studio 开发的插件，用于简化在 Bilibili 平台上的直播流程。插件支持扫码登录 Bilibili，更新直播间信息，并获取推流线路（RTMP/SRT，含主/备）的地址与推流码；插件只展示线路，不改写 OBS 配置。

**[English README](README_en.md)**

## 安装方法

<div align="center">
  <img width="785" height="916" alt="image" src="https://github.com/user-attachments/assets/bf0b35cb-b7ce-49d5-b783-41cbaffefd08" />
  <p><i>OBS 官方插件安装路径说明</i></p>
</div>

### Windows

1.  **下载插件**：从 [Releases 页面](https://github.com/Zarosmm/obs-bilibili-stream/releases) 下载最新的 `bilibili-stream-for-obs-*-windows-x64.zip`。
2.  **解压文件**：将压缩包解压。
3.  **放置目录**：将解压后的文件夹移动至以下路径：
    `C:\ProgramData\obs-studio\plugins`
4.  **校验结构**：请确保您的目录结构严格遵守以下格式：
    ```text
    C:\ProgramData\obs-studio\plugins\
    └── bilibili-stream-for-obs\
        ├── bin\
        │   └── 64bit\
        │       └── bilibili-stream-for-obs.dll
        └── data\
            └── locale\
                └── (相关的 .ini 语言文件)
    ```
5.  **启动 OBS**：重新启动 OBS Studio，插件将自动加载。

### Ubuntu/Debian(用户插件目录 zip)

1.  **下载插件**：从 [Releases 页面](https://github.com/Zarosmm/obs-bilibili-stream/releases) 下载最新的用户插件目录 zip(例如 `bilibili-stream-for-obs-*-x86_64-ubuntu-24.04.zip`)。
2.  **解压即装**：zip 顶层即插件名目录,直接解压到用户插件目录,得到 `bilibili-stream-for-obs/bin/64bit/bilibili-stream-for-obs.so` 与 `.../data/locale/`：
    ```bash
    unzip -o bilibili-stream-for-obs-*-x86_64-ubuntu-*.zip -d ~/.config/obs-studio/plugins/
    ```
3.  **运行库**：私有 SRT 输出需要 `libsrt1.5-gnutls`(Debian/Ubuntu:`sudo apt install libsrt1.5-gnutls`);侧栏用的 Qt6 是 OBS 的运行依赖(Windows/macOS 随 OBS 打包,Linux 由发行版随 OBS 装上),无需另装。
4.  **启动 OBS**：重新启动 OBS Studio,插件将自动加载。

### Flatpak（OBS Studio Flatpak）

Flatpak 插件仅适用于通过 Flatpak 安装的 OBS Studio stable 版本，目前提供 x86_64 架构。

1. **下载插件**：从 [Releases 页面](https://github.com/Zarosmm/obs-bilibili-stream/releases) 下载最新的 `bilibili-stream-for-obs-*-flatpak-x86_64.flatpak`。
2. **安装插件**：在下载目录运行：
    ```bash
    flatpak install --user ./bilibili-stream-for-obs-*-flatpak-x86_64.flatpak
    ```
3. **启动 OBS**：运行 Flatpak 版 OBS Studio，插件将自动加载：
    ```bash
    flatpak run com.obsproject.Studio
    ```

更新插件时，下载新的 bundle 并运行：

```bash
flatpak install --user --or-update ./bilibili-stream-for-obs-*-flatpak-x86_64.flatpak
```

卸载插件：

```bash
flatpak uninstall --user com.obsproject.Studio.Plugin.BilibiliStream
```

该 bundle 不适用于通过系统软件包安装的原生 OBS Studio；此类安装请使用 `.deb` 或 `.rpm` 包。

### Fedora/openSUSE（非官方）

1.  **下载插件**：从 [openSUSE Build Service](https://build.opensuse.org/package/show/home:whoaml/obs-bilibili-stream) 下载最新的 `.rpm` 包（例如 `obs-bilibili-stream-2.1.3-5.1.x86_64.rpm）。
2.  **安装插件**：运行以下命令：
    ```bash
    sudo rpm -i obs-bilibili-stream-*.rpm
    ```

3.  **启动 OBS**：重新启动 OBS Studio，插件将自动加载。

插件将安装到 `/usr/lib/obs-plugins/` 目录。


## 使用方法

> v3.2 起有两个入口,显示的是**同一份服务端状态**:**侧栏「B站直播」**(常态看状态、点常用操作)和**「B站直播控制台」**(改标题/分区、拉取推荐码率)。首次加载时侧栏自动显示在右侧停靠区,之后布局由你自己安排(OBS 会记住)。插件不启动推流、不改写 OBS 推流配置(ADR-0001/0005)。

1. **侧栏**:菜单栏 **停靠栏(Docks) → B站直播**。显示登录态、房间号/标题/分区、场次状态(未开播/直播中/轮回/锁定)、OBS 是否在推流、最近一次与服务端对账的时间,以及本场全部推流线路(地址与推流码各带「复制」按钮)。信息每分钟对账一次,OBS 开始/停止推流后立即刷新;刷新失败时保留上次结果并提示原因。停靠区比内容矮时(侧栏拉矮,或取到推流码后内容变高)侧栏可以上下滚动,控件不会被压扁或裁掉,按钮列在内容末尾。

2. **登录 B站**:
    - 侧栏点 **扫码登录**:二维码直接显示在侧栏里,用手机 B站 App 扫码确认(也可点「在浏览器打开二维码」)。二维码过期自动换新,**取消登录**可随时中止。
    - 没有 Qt 侧栏的形态(OBS < 30.0.2)下二维码改在系统浏览器打开;若浏览器也打不开,状态行给出链接。

3. **更新直播间信息**(控制台):
    - 填写**直播间标题**、选择**分区大类 / 二级分区**(分区列表登录后自动加载;标题/分区默认值就是服务端当前值)。
    - 分别点击 **应用标题** / **应用分区** 提交。

4. **获取推流码(向服务端打开直播场次)**:
    - 侧栏或控制台点 **获取推流码**;成功后**全部线路**(主/备、协议/provider、地址、推流码)逐条出现,均可复制。
    - 在 OBS 中打开 **设置 → 直播**,按线路协议选择服务并粘贴地址/推流码:
        - RTMP 线路:服务选 **自定义**,服务器粘贴推流地址(例如 `rtmp://live-push.bilivideo.com/live-bvc/`)
        - SRT 线路(`protocol=srt`,地址为 `rtmpsrt://...`):插件在开播成功/打开控制台时会把私有 SRT 条目
          合并进 OBS 服务列表(无条件写:本次没有 rtmpsrt 线路时条目内为默认入口)。
          服务选 **「Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT」**,
          服务器选本次的 `rtmpsrt://` 地址,推流码粘贴(若设置窗口已打开,先关闭再打开以刷新列表)。OBS 会用本插件的私有 SRT 输出
          (RTMP over SRT,UDP 1935),而不是 OBS 原生 SRT(MPEG-TS,接不上)。详见契约 §4.6 / ADR-0007
    - 点击 OBS 右下角 **开始直播**。插件只负责开播信令、线路展示与服务目录合并,**推流按钮永远由你点**。

5. **关闭直播间(结束直播场次)**:
    - 先在 OBS 停止推流,再点 **关闭直播间**。侧栏会弹确认框(回车/Esc 默认都是「取消」;控制台是两段式按钮),确认后才向服务端发送停播信令。OBS 还在推流时只提示「先停止推流」,不会关房间。
    - 停播后线路随场次作废并自动清空;想再播需重新「获取推流码」。
    - **打开直播间页** 可在浏览器打开自己的直播页面。

6. **找回推流码**:OBS 崩溃/重启后服务端仍在直播(侧栏显示「直播中」但没有线路)时,点 **找回推流码** 取回本场的推流地址与推流码,填入 OBS 后点开始直播即可接回原场次。

7. **直播间设置…**:侧栏按钮,打开控制台(与工具菜单 **B站直播控制台** 同一入口)。

## 注意事项

- **私有 SRT 服务条目**：插件会在 OBS 的 `plugin_config/rtmp-services/services.json` 里合并一条
  「Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT」（协议 `bvc_srt`），这是 OBS 的服务目录数据文件，不是你的推流配置；
  删除该文件即可恢复 OBS 原生服务列表。插件不写你的已选服务、编码器或输出配置。
- **私有 SRT 输出编码器**：支持 H.264 / HEVC / AV1（Enhanced-RTMP 封装，与上游 OBS
  RTMP 输出口径一致；fork 自上游 OBS 32.2 的 RTMP 栈，见 ADR-0008）。
- **功能边界**：插件只代理开播/停播/恢复开播与质量协商等信令，并展示线路与推流码；
  推流由 OBS 本身完成，插件不改写 OBS 设置。备流（主备双通道）仅展示线路，
  不驱动第二路推流；联播、连麦、ARTC/WHIP 不在本插件范围。
- **推流码时效**：开播信令下发的推流码为会话级有效；停播或服务端判定场次结束后,插件会在下一次对账时自动清掉失效线路,重新「获取推流码」即可。
- **日志与隐私**:插件日志**不包含**推流码、live_key、登录票据或 Cookie(只记 HTTP 状态与
  code/message),分享日志前不必再手工打码;排查协议问题需要响应原文时,临时设置环境变量
  `BILI_TRACE_HTTP=1` 再启动 OBS(开着它产生的日志含凭据,不要外传)。
- **日志查看**：如果遇到问题，检查 OBS 日志文件：
  - Windows: `C:\Users\<YourUser>\AppData\Roaming\obs-studio\logs`
  - macOS: `~/Library/Logs/obs-studio/`
  - Linux: `~/.config/obs-studio/logs/`

## 依赖

- OBS Studio 30.0 或更高版本(侧栏需要 30.0.2+ 的 dock API;更老的 OBS 自动退回「只有控制台」)
- Windows / Ubuntu 24.04 或更高版本
- **运行**:Qt6 Widgets(OBS 自带,无需额外安装);libsrt 1.5 运行库(Debian/Ubuntu `libsrt1.5-gnutls`)
- **构建**:libobs/obs-frontend-api 开发包、libcurl、Qt6 开发包(Debian/Ubuntu `qt6-base-dev`;
  Windows/macOS 走 obs-deps 预编译 Qt)、libsrt 开发包(`libsrt-gnutls-dev` 或 `libsrt-openssl-dev`)。
  `-DENABLE_DOCK=OFF` 可构建不带侧栏的 Qt-free 形态;没装 libsrt 开发包时用
  `-DENABLE_SRT_OUTPUT=OFF` 关闭私有 SRT 输出(默认开,缺库会在配置阶段失败),插件其余功能照常。

## 贡献

欢迎提交问题或拉取请求至 [GitHub 仓库](https://github.com/Zarosmm/obs-bilibili-stream)。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=Zarosmm/obs-bilibili-stream&type=date&legend=top-left)](https://www.star-history.com/#Zarosmm/obs-bilibili-stream&type=date&legend=top-left)
