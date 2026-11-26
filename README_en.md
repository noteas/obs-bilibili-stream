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

### Ubuntu/Debian

1.  **Download plugin**: Download the latest `.deb` package (e.g., `bilibili-stream-for-obs-*-x86_64-ubuntu-24.04.deb`) from the [Releases page](https://github.com/Zarosmm/obs-bilibili-stream/releases).
2.  **Install plugin**: Run the following command:
    ```bash
    sudo dpkg -i bilibili-stream-for-obs-*.deb
    ```
3.  **Fix dependencies** (if needed):
    ```bash
    sudo apt-get install -f
    ```
4.  **Launch OBS**: Restart OBS Studio, the plugin will load automatically.

The plugin will be installed to `/usr/lib/obs-plugins/` directory.

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

> Since v2.2 all controls live in the **「B站直播控制台」** (Bilibili Stream Console) window, rendered natively by OBS (ADR-0003). The plugin no longer depends on Qt.

1. **Open the console**: Start OBS, then go to menu **Tools → B站直播控制台**.

2. **Login to Bilibili**:
    - Click **扫码登录** (QR login) in the console; the login QR code opens in your system browser (if no browser can be launched, the status line shows the URL to open manually).
    - Scan with the Bilibili mobile app and confirm; the console status line updates to "已登录" (logged in) automatically.
    - You can also use **Tools → B站扫码登录** directly, without opening the console first.

3. **Update Livestream Room Info**:
    - In the console, fill in the room title and pick the category / subcategory (the list loads automatically after login).
    - Click **应用标题** (Apply title) / **应用分区** (Apply category) to submit.

4. **Start live (open the Bilibili live session)**:
    - Click **开播** (Go live) in the console; **all lines** (primary/backup, protocol/provider, address, stream key) appear in the console and can be copied.
    - In OBS, go to **Settings** → **Stream**, choose the service that matches the line, and paste:
        - RTMP line: Service **Custom**, Server = push URL (e.g., `rtmp://live-push.bilivideo.com/live-bvc/?streamname=...`)
        - SRT line: Service **SRT**, fill the SRT fields OBS provides
    - Click **Start Streaming** in OBS. The plugin only handles go-live signalling and line display; it does not modify OBS configuration, and starting the actual stream is always your call (ADR-0005).

5. **Stop**:
    - Stop streaming in OBS first, then click **停播** (End live) in the console to close the Bilibili live room.
    - **打开直播间** (Open live room), from the menu or the console, opens your live page in the browser.

## Troubleshooting

- **Logs**: If you encounter issues, check OBS log files:
  - Windows: `C:\Users\<YourUser>\AppData\Roaming\obs-studio\logs`
  - macOS: `~/Library/Logs/obs-studio/`
  - Linux: `~/.config/obs-studio/logs/`

## Requirements

- OBS Studio 30.0 or higher
- Windows / Ubuntu 24.04 or higher

## Contributing

Feel free to submit issues or pull requests to the [GitHub repository](https://github.com/Zarosmm/obs-bilibili-stream).

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=Zarosmm/obs-bilibili-stream&type=date&legend=top-left)](https://www.star-history.com/#Zarosmm/obs-bilibili-stream&type=date&legend=top-left)
