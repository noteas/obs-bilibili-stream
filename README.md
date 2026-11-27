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

### Ubuntu/Debian

1.  **下载插件**：从 [Releases 页面](https://github.com/Zarosmm/obs-bilibili-stream/releases) 下载最新的 `.deb` 包（例如 `bilibili-stream-for-obs-*-x86_64-ubuntu-24.04.deb`）。
2.  **安装插件**：运行以下命令：
    ```bash
    sudo dpkg -i bilibili-stream-for-obs-*.deb
    ```
3.  **修复依赖**（如需要）：
    ```bash
    sudo apt-get install -f
    ```
4.  **启动 OBS**：重新启动 OBS Studio，插件将自动加载。

插件将安装到 `/usr/lib/obs-plugins/` 目录。

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

> v2.2 起插件不再使用独立弹窗,全部操作集中在 OBS 原生渲染的 **「B站直播控制台」** 窗口(ADR-0003);插件不再依赖 Qt。

1. **打开控制台**:启动 OBS,菜单栏 **工具(Tools)→ B站直播控制台**。

2. **登录 Bilibili**:
    - 在控制台点击 **扫码登录**:登录二维码会在系统浏览器中打开(若无法打开浏览器,状态栏会给出链接供手动访问)。
    - 手机 B站 App 扫码并确认后,控制台状态行自动更新为"已登录"。
    - 也可直接用菜单 **工具 → B站扫码登录**,无需先开控制台。

3. **更新直播间信息**:
    - 在控制台填写 **直播间标题**、选择 **分区大类 / 二级分区**(分区列表登录后自动加载)。
    - 分别点击 **应用标题** / **应用分区** 提交。

4. **开播(打开 B站直播场次)**:
    - 控制台点击 **开播**;成功后**全部线路**(主/备、协议/provider、地址、推流码)显示在控制台,均可选中复制。
    - 在 OBS 中,打开 **设置** → **推流**,按线路协议选择服务并粘贴地址/推流码:
        - RTMP 线路:服务选 **自定义**,服务器粘贴推流地址(例如 `rtmp://live-push.bilivideo.com/live-bvc/?streamname=...`)
        - SRT 线路(`protocol=srt`,地址为 `rtmpsrt://...`):插件在开播成功/打开控制台时会把私有 SRT 条目
          合并进 OBS 服务列表(无条件写:本次没有 rtmpsrt 线路时条目内为默认入口)。
          服务选 **「Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT」**,
          服务器选本次的 `rtmpsrt://` 地址,推流码从控制台复制后手动填写(若设置窗口已打开,先关闭再打开以刷新列表)。OBS 会用本插件的私有 SRT 输出
          (RTMP over SRT,UDP 1935),而不是 OBS 原生 SRT(MPEG-TS,接不上)。详见契约 §4.6 / ADR-0007
    - 点击 OBS 右下角 **开始输出/开始推流**。插件只负责开播信令、线路展示与服务目录合并,
      不改写你的已选服务/编码/输出配置,推流按钮永远由你在 OBS 点击(ADR-0005)。

5. **停播**:
    - 先在 OBS 停止推流,再在控制台点击 **停播** 关闭 B站直播间。
    - **打开直播间**(菜单或控制台按钮)可在浏览器打开自己的直播页面。

## 注意事项

- **私有 SRT 服务条目**：插件会在 OBS 的 `plugin_config/rtmp-services/services.json` 里合并一条
  「Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT」（协议 `bvc_srt`），这是 OBS 的服务目录数据文件，不是你的推流配置；
  删除该文件即可恢复 OBS 原生服务列表。插件不写你的已选服务、编码器或输出配置。
- **私有 SRT 输出限制（v1）**：只支持 H.264 视频 + AAC 音频；OBS 若使用 HEVC/AV1，请先在
  编码器设置里切到 H.264，否则输出会以「仅支持 H.264」拒绝启动。
- **日志查看**：如果遇到问题，检查 OBS 日志文件：
  - Windows: `C:\Users\<YourUser>\AppData\Roaming\obs-studio\logs`
  - macOS: `~/Library/Logs/obs-studio/`
  - Linux: `~/.config/obs-studio/logs/`

## 依赖

- OBS Studio 30.0 或更高版本
- Windows / Ubuntu 24.04 或更高版本
- 构建私有 SRT 输出需要 libsrt 开发包：Debian/Ubuntu `libsrt-gnutls-dev`
  （或 `libsrt-openssl-dev`）；运行需要对应的 `libsrt1.5-gnutls`（或 openssl 变体）。
  未找到开发包时 CMake 会关闭该功能，插件其余部分仍可正常构建/使用。

## 贡献

欢迎提交问题或拉取请求至 [GitHub 仓库](https://github.com/Zarosmm/obs-bilibili-stream)。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=Zarosmm/obs-bilibili-stream&type=date&legend=top-left)](https://www.star-history.com/#Zarosmm/obs-bilibili-stream&type=date&legend=top-left)
