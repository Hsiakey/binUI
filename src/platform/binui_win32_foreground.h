// 窗口前台化：把刚创建的顶层窗口提到最前并激活。
//
// 为什么需要它：Windows 的前台锁（SPI_GETFOREGROUNDLOCKTIMEOUT）不允许后台进程
// 随意抢前台。`CreateWindowExW(..., WS_VISIBLE, ...)` 只是让窗口"可见"，
// `SetFocus()` 只改本线程的键盘焦点，两者都不会把窗口提到前台 —— 于是从控制台
// 启动的程序，窗口老老实实待在 cmd 后面，用户第一眼看到的是控制台。
//
// 兜底手段是 AttachThreadInput：临时把本线程的输入队列挂到当前前台窗口的线程上，
// 让系统认为"是本前台进程在请求"，抢完立刻摘掉。这是 Win32 下唯一不需要
// AllowSetForegroundWindow 配合的做法，且不改变任何持久状态。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// 把 hwnd 提到前台。已被系统拦下时会走 AttachThreadInput 兜底。
// 注意：宿主自己决定前台策略时（子窗口模式）不要调这个。
inline void BinUIForceForeground(HWND hwnd) {
    if (!hwnd) return;

    // 被最小化的窗口得先还原，否则 SetForegroundWindow 只是闪一下任务栏
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else               ShowWindow(hwnd, SW_SHOW);

    if (SetForegroundWindow(hwnd)) return;

    // 兜底：依附到当前前台窗口的输入队列后再抢
    const HWND fg = GetForegroundWindow();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD myTid = GetCurrentThreadId();
    if (!fgTid || fgTid == myTid) {
        // 前台窗口属于本线程（或没有前台窗口）时，SetForegroundWindow 失败
        // 只可能是本线程没有活动窗口，直接激活即可
        SetActiveWindow(hwnd);
        return;
    }

    if (AttachThreadInput(myTid, fgTid, TRUE)) {
        SetForegroundWindow(hwnd);
        AttachThreadInput(myTid, fgTid, FALSE);
    }
    if (GetForegroundWindow() != hwnd) SetActiveWindow(hwnd);
}
