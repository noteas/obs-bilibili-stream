#pragma once
#include <obs.h>

namespace UI {

// The plugin's only UI (ADR-0003): a hidden private source whose properties OBS
// renders natively. No widget toolkit is linked; every control is an
// obs_property. Menu entries and the properties dialog both dispatch into the
// entry points below. All of them must be called on the OBS UI thread.

// Register the console source type; call once before ConsoleCreate().
obs_source_info *ConsoleSourceInfo();
// Load config, create the private source, kick off the login-state refresh.
void ConsoleCreate();
// Module unload: stop accepting UI continuations, drop the source.
void ConsoleDestroy();

void ConsoleOpenPanel(); // open the properties dialog
void ConsoleStartLogin();
void ConsoleOpenRoom();

} // namespace UI
