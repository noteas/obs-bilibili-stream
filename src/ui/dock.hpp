#pragma once
#include <obs.h>

namespace UI {

// 直播间状态侧栏(ADR-0010):Qt Widgets 停靠面板,经 obs_frontend_add_dock_by_id 挂进
// OBS 的停靠栏体系(Docks 菜单可开关、布局随 OBS 保存)。它是 Core::Session 的一个
// 视图:展示服务端房间态/登录二维码/推流线路(一键复制),并把「必要操作」转成
// Session 动作;「关闭直播间」先弹确认框。「直播间设置…」按钮打开控制台面板
// (ConsoleOpenPanel)。所有入口必须在 OBS UI 线程调用。

// 在 obs_module_load() 里调用。返回 false = 本机 OBS 没有 dock API(< 30.0.2)或
// dock id 已被占用,插件退回「只有控制台」的形态。
bool DockCreate();
// OBS 退出(OBS_FRONTEND_EVENT_EXIT)或模块卸载时调用:摘掉事件回调、销毁内容控件、
// 从 OBS 摘掉 dock。重复调用安全。
void DockDestroy();
// 把(可能被用户关掉的)侧栏显示出来;返回是否找到并显示。
bool DockShow();

} // namespace UI
