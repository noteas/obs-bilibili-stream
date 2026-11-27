#pragma once
#include <obs.h>

namespace UI {

// The plugin's only UI (ADR-0003): a hidden private source whose properties OBS
// renders natively. No widget toolkit is linked; every control is an
// obs_property. The tools menu only opens the dialog; every action (login,
// room, live control) is a dialog button. All entry points must be called on
// the OBS UI thread.

// Register the console source type; call once before ConsoleCreate().
obs_source_info *ConsoleSourceInfo();
// Load config, create the private source, kick off the login-state refresh.
void ConsoleCreate();
// Module unload: stop accepting UI continuations, drop the source.
void ConsoleDestroy();

void ConsoleOpenPanel(); // open the properties dialog

// 把所有已保存的 rtmpsrt:// 线路刷新进 OBS 的 rtmp-services 服务目录
// （模块加载完成后调用；rtmp-services 必须已加载）。
void ConsoleRefreshSrtCatalog();

} // namespace UI
