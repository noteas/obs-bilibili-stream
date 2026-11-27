#pragma once
#include <obs.h>

namespace UI {

// 配置界面(控制台):隐藏私有 source 的 obs_properties 面板,由 OBS 原生渲染,不链接
// Qt(ADR-0003)。它是 Core::Session 的一个视图:只读快照渲染 + 把按钮点击转成
// Session 动作。侧栏(src/ui/dock.hpp,ADR-0010)是另一个视图,入口经 ConsoleOpenPanel()
// 打开本面板。所有入口必须在 OBS UI 线程调用。

// Register the console source type; call once before ConsoleCreate().
obs_source_info *ConsoleSourceInfo();
// Subscribe to the session and create the private source.
void ConsoleCreate();
// Module unload: stop accepting UI continuations, drop the source.
void ConsoleDestroy();

void ConsoleOpenPanel(); // open the properties dialog

} // namespace UI
