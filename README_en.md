# OBS Bilibili Streaming Plugin

A plugin for OBS Studio that simplifies the streaming process on Bilibili platform. Supports QR code login, updating livestream room info, and obtaining all push lines (RTMP/SRT, primary/backup) with their addresses and stream keys. The plugin only displays these lines and never rewrites OBS configuration.

## Installation

### Windows

1.  **Download plugin**: Download the latest `bilibili-stream-for-obs-*-windows-x64.zip` from the [Releases page](https://github.com/Zarosmm/obs-bilibili-stream/releases).
2.  **Extract files**: Unzip the downloaded archive.
3.  **Move to plugins folder**: Copy the extracted folder to:
    `C:\ProgramData\obs-studio\plugins`
4.  **Verify structure**: Ensure the directory structure follows this format:
    ```text
    C:\ProgramData\obs-studio\plugins\
    └── bilibili-stream-for-obs\
        ├── bin\
        │   └── 64bit\
        │       └── bilibili-stream-for-obs.dll
        └── data\
            └── locale\
                └── (language .ini files)
    ```
5.  **Launch OBS**: Restart OBS Studio, the plugin will load automatically.

### Ubuntu/Debian (user plugin directory zip)

1.  **Download plugin**: Download the latest user-plugin-directory zip from the [Releases page](https://github.com/Zarosmm/obs-bilibili-stream/releases) (e.g. `bilibili-stream-for-obs-*-x86_64-ubuntu-24.04.zip`).
2.  **Unzip to install**: the zip's top level is the plugin-name directory; extract it straight into the per-user plugin directory to get `bilibili-stream-for-obs/bin/64bit/bilibili-stream-for-obs.so` and `.../data/locale/`:
    ```bash
    unzip -o bilibili-stream-for-obs-*-x86_64-ubuntu-*.zip -d ~/.config/obs-studio/plugins/
    ```
3.  **Runtime library**: the private SRT output needs `libsrt1.5-gnutls` (Debian/Ubuntu: `sudo apt install libsrt1.5-gnutls`); the Qt6 used by the dock is a runtime dependency of OBS (bundled on Windows/macOS, installed from the distro along with OBS on Linux), so there is nothing extra to install.
4.  **Launch OBS**: restart OBS Studio; the plugin loads automatically.

### Flatpak (OBS Studio Flatpak)

The Flatpak plugin is only compatible with the stable Flatpak build of OBS Studio and is currently provided for x86_64.

1. **Download plugin**: Download the latest `bilibili-stream-for-obs-*-flatpak-x86_64.flatpak` from the [Releases page](https://github.com/Zarosmm/obs-bilibili-stream/releases).
2. **Install plugin**: Run the following command in the download directory:
    ```bash
    flatpak install --user ./bilibili-stream-for-obs-*-flatpak-x86_64.flatpak
    ```
3. **Launch OBS**: Start the Flatpak build of OBS Studio; the plugin will load automatically:
    ```bash
    flatpak run com.obsproject.Studio
    ```

To update the plugin, download the new bundle and run:

```bash
flatpak install --user --or-update ./bilibili-stream-for-obs-*-flatpak-x86_64.flatpak
```

To uninstall the plugin, run:

```bash
flatpak uninstall --user com.obsproject.Studio.Plugin.BilibiliStream
```

This bundle cannot be used with a native OBS Studio installation from a system package. Use a `.deb` or `.rpm` package for those installations.

### Fedora/openSUSE（Unoffical）

1.  **Download plugin**： Download the latest '.rpm' packages（e.g., `obs-bilibili-stream-2.1.3-5.1.x86_64.rpm）from the [openSUSE Build Service](https://build.opensuse.org/package/show/home:whoaml/obs-bilibili-stream).
2.  **Install plugin**：Run the following command：
    ```bash
    sudo rpm -i obs-bilibili-stream-*.rpm
    ```
3.  **Launch OBS**: Restart OBS Studio, the plugin will load automatically.

The plugin will be installed to `/usr/lib/obs-plugins/` directory.


## Usage

> Since v3.2 there are two entry points showing the **same server state**: the **"Bilibili Live" dock** (status at a glance plus the everyday actions) and the **「B站直播控制台」** (Bilibili Live Console: title/area, recommended bitrate). On first load the dock appears in the right dock area; after that OBS remembers your layout. The plugin never starts streaming and never rewrites your OBS stream settings (ADR-0001/0005).

1. **Dock**: menu **Docks → Bilibili Live**. It shows the sign-in state, room id/title/area, live session status (offline / live / rebroadcast / locked), whether OBS is streaming, the time of the last server check, and every stream line of the current session (server address and stream key, each with a **Copy** button). The state is reconciled with the server once a minute and right after OBS starts/stops streaming; if a check fails, the previous data is kept and the reason is shown.

2. **Login to Bilibili**:
    - Click **Sign in with QR code** (扫码登录) in the dock: the QR code is rendered right there — scan with the Bilibili mobile app and confirm (or open it in your browser). Expired codes are refreshed automatically; **Cancel sign-in** aborts at any time.
    - On OBS versions without the Qt dock API (< 30.0.2) the QR code opens in the system browser instead; if no browser can be launched, the status line shows the URL.

3. **Update livestream room info** (console):
    - Fill in the room title and pick the category / subcategory (the list loads after login; the defaults are the server's current values).
    - Click **Apply title** / **Apply area** to submit.

4. **Get stream key (open the Bilibili live session)**:
    - Click **Get stream key** (获取推流码) in the dock or the console; **all lines** (primary/backup, protocol/provider, address, stream key) appear and can be copied.
    - In OBS, go to **Settings → Stream**, choose the service that matches the line, and paste:
        - RTMP line: Service **Custom**, Server = push URL (e.g. `rtmp://live-push.bilivideo.com/live-bvc/`)
        - SRT line (`rtmpsrt://`): the plugin merges a **Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT**
          entry (protocol `bvc_srt`) into OBS's rtmp-services catalog whenever a stream starts or the
          console opens — unconditionally, so even sessions without an `rtmpsrt://` line keep the
          entry (with the default server). Choose it as the Service, pick this session's
          `rtmpsrt://` server, and paste the stream key. OBS then uses this
          plugin's private SRT output (RTMP over SRT, UDP 1935), not OBS's native MPEG-TS SRT.
          Close/reopen the Settings dialog to refresh the service list.
    - Click **Start Streaming** in OBS. The plugin only handles go-live signalling, line display and
      the service catalog entry; **the stream button is always yours**.

5. **Close live room (end the live session)**:
    - Stop streaming in OBS first, then click **Close live room** (关闭直播间). The dock asks for confirmation (Enter and Esc both default to **Cancel**; the console uses a two-step button); only then is the stop-live signal sent. While OBS is still streaming it only tells you to stop streaming first.
    - After the session ends the stream lines are discarded and cleared automatically; press **Get stream key** again before the next stream.
    - **Open room page** opens your live page in the browser.

6. **Recover stream key**: if OBS crashed or was restarted while the server still shows **live** (the dock says live but no lines are shown), click **Recover stream key** (找回推流码) to fetch this session's address/key again, paste them into OBS and press Start Streaming to rejoin the same session.

7. **Room settings…**: dock button that opens the console (same entry as **Tools → B站直播控制台**).

## Troubleshooting

- **Private SRT service entry**: the plugin merges a
  "Bilibili Live - RTMPSRT | 哔哩哔哩直播 - RTMPSRT" entry (protocol `bvc_srt`) into OBS's
  `plugin_config/rtmp-services/services.json`. That is OBS's service catalog data file,
  not your streaming configuration; delete the file to restore the stock list. The plugin
  never writes your selected service, encoder or output settings.
- **Private SRT output codecs**: H.264 / HEVC / AV1 are supported (Enhanced-RTMP
  encapsulation, same behaviour as upstream OBS RTMP output; the output is a fork of the
  upstream OBS 32.2 RTMP stack, see ADR-0008).
- **Scope**: the plugin only proxies signalling (start/stop/resume live, quality
  negotiation) and displays stream keys and server lines. Streaming itself is done by
  OBS; the plugin never rewrites OBS settings. Backup channels are display-only (no
  second push); multi-platform simulcast, co-broadcasting and ARTC/WHIP are out of scope.
- **Stream key lifetime**: keys delivered at start-live are session-scoped; once the
  session ends (or the server reports offline) the plugin drops the stale lines at the
  next check — press **Get stream key** again for a fresh one.
- **Logs and privacy**: the plugin log **never contains** stream keys, `live_key`, login
  tickets or cookies (only HTTP status and code/message), so logs can be shared as-is.
  To capture raw responses while debugging protocol issues, start OBS with the
  environment variable `BILI_TRACE_HTTP=1` (such logs do contain credentials — do not share them).
- **Logs**: If you encounter issues, check OBS log files:
  - Windows: `C:\Users\<YourUser>\AppData\Roaming\obs-studio\logs`
  - macOS: `~/Library/Logs/obs-studio/`
  - Linux: `~/.config/obs-studio/logs/`

## Requirements

- OBS Studio 30.0 or higher (the dock needs the dock API of 30.0.2+; older versions
  automatically fall back to the console-only form)
- Windows / Ubuntu 24.04 or higher
- **Runtime**: Qt6 Widgets (shipped with OBS, nothing to install); libsrt 1.5 runtime
- **Build**: libobs/obs-frontend-api dev files, libcurl, Qt6 dev package
  (Debian/Ubuntu `qt6-base-dev`; Windows/macOS use the pre-built Qt from obs-deps),
  libsrt headers (`libsrt-gnutls-dev` or `libsrt-openssl-dev`).
  `-DENABLE_DOCK=OFF` builds the Qt-free form without the dock; without libsrt
  headers pass `-DENABLE_SRT_OUTPUT=OFF` (the default is ON and fails at configure
  time when the library is missing).

## Contributing

Feel free to submit issues or pull requests to the [GitHub repository](https://github.com/Zarosmm/obs-bilibili-stream).

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=Zarosmm/obs-bilibili-stream&type=date&legend=top-left)](https://www.star-history.com/#Zarosmm/obs-bilibili-stream&type=date&legend=top-left)
