// 窗口前台化：把刚创建的顶层窗口提到最前并激活。
//
// 为什么需要它：Windows 的前台锁（SPI_GETFOREGROUNDLOCKTIMEOUT）不允许后台进程
// 随意抢前台。`CreateWindowExW(..., WS_VISIBLE, ...)` 只是让窗口"可见"，
// `SetFocus()` 只改本线程的键盘焦点，两者都不会把窗口提到前台 —— 于是从终端里
// 启动的程序，窗口老老实实待在终端后面，用户第一眼看到的是命令行。
//
// 三个坑，踩过才知道：
//   1. `SetForegroundWindow` 被前台锁拦下时**也会返回 TRUE**。不能拿返回值当
//      "成功"的判据，必须用 GetForegroundWindow() 复核。
//   2. Windows Terminal / conhost 这类宿主，前台窗口属于别的进程（WindowsTerminal.exe
//      / conhost.exe），"由前台进程启动"这条规则不成立，只能靠 AttachThreadInput
//      把本线程的输入队列临时挂过去。
//   3. 前台锁最顽固时连 AttachThreadInput 也破不开，补一次 Alt 轻敲 —— 系统看到
//      "本线程刚收到用户输入"，就允许设置前台。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// 把 hwnd 提到前台并激活。子窗口模式不要调（焦点归属宿主，抢前台是越界行为）。
inline void BinUIForceForeground(HWND hwnd) {
    if (!hwnd) return;

    // 被最小化的窗口得先还原，否则 SetForegroundWindow 只是闪一下任务栏
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNORMAL);

    if (GetForegroundWindow() == hwnd && GetActiveWindow() == hwnd) return;

    const HWND fg = GetForegroundWindow();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD myTid = GetCurrentThreadId();

    bool attached = false;
    if (fgTid && fgTid != myTid)
        attached = AttachThreadInput(myTid, fgTid, TRUE) != FALSE;

    // 无条件置顶一次再取消：绕开前台锁对 Z 序的干预，保证窗口物理上压在最前面。
    // 用 SWP_NOACTIVATE，免得这次移动又把激活状态搅乱。
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    SetForegroundWindow(hwnd);   // 不看返回值，见上面第 1 条

    if (GetForegroundWindow() != hwnd) {
        // 前台锁还在：轻敲一次 Alt，让系统认账"本线程刚收到用户输入"
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_MENU;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(hwnd);
    }

    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (attached) AttachThreadInput(myTid, fgTid, FALSE);
}
