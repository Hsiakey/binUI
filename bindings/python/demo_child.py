"""子窗口模式演示：宿主是一个普通 Win32 窗口（ctypes 建的），里面嵌两块 binUI。

要点（也是把它嵌进 WPF / Qt / WinForms 时要做的同一套）：
  · 宿主负责建窗、消息循环、布局、DPI 策略；
  · binUI 只是 WS_CHILD 子窗口，用 binui_app_open_child 挂进来；
  · 每帧由宿主的节拍（这里是 WM_TIMER）驱动 binui_render() —— 库不碰消息队列；
  · 鼠标/键盘仍然由宿主的 DispatchMessage 投递给子窗口，事件从 poll() 出来。

跑之前先在仓库根目录 `mingw32-make SHELL=sh capi`，然后：
    python bindings/python/demo_child.py
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
from pathlib import Path

sys.stdout.reconfigure(line_buffering=True, encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import binui  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ctypes.wintypes 里没有 HCURSOR / PAINTSTRUCT（只有 HICON），补上
HCURSOR = wt.HANDLE


class PAINTSTRUCT(ctypes.Structure):
    _fields_ = [
        ("hdc", wt.HDC),
        ("fErase", wt.BOOL),
        ("rcPaint", wt.RECT),
        ("fRestore", wt.BOOL),
        ("fIncUpdate", wt.BOOL),
        ("rgbReserved", ctypes.c_byte * 32),
    ]

# DPI 感知：宿主说了算（binUI 在子窗口模式下刻意不碰这个进程级策略）
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2

WM_DESTROY, WM_SIZE, WM_TIMER, WM_PAINT, WM_ERASEBKGND = 0x0002, 0x0005, 0x0113, 0x000F, 0x0014
WS_OVERLAPPEDWINDOW = 0x00CF0000
WS_CLIPCHILDREN = 0x02000000   # 必须：否则宿主的 WM_PAINT 会盖掉子窗口（GL 子窗口没有留存面）
WS_VISIBLE = 0x10000000
SWP_NOZORDER = 0x0004
SWP_SHOWWINDOW = 0x0040

CLIENT_W, CLIENT_H = 600, 404
GAP = 12
LEFT_W = 220

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", wt.UINT),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wt.HINSTANCE),
        ("hIcon", wt.HICON),
        ("hCursor", HCURSOR),
        ("hbrBackground", wt.HBRUSH),
        ("lpszMenuName", wt.LPCWSTR),
        ("lpszClassName", wt.LPCWSTR),
    ]


class State:
    """宿主持有的东西：窗口句柄、两块 binUI、以及一个共享计数。"""
    hwnd = 0
    app_a: binui.App | None = None
    app_b: binui.App | None = None
    ids_a: dict[str, int] = {}
    ids_b: dict[str, int] = {}
    count = 0
    layout_ready = False


S = State()


def layout_children(w: int, h: int) -> None:
    """宿主自己的布局：左边定宽，右边吃掉剩下的宽度。"""
    if not S.app_a or not S.app_b:
        return
    right_w = max(160, w - LEFT_W - GAP * 3)
    panel_h = max(160, h - GAP * 2)
    user32.SetWindowPos(S.app_a.hwnd, None, GAP, GAP, LEFT_W, panel_h,
                        SWP_NOZORDER | SWP_SHOWWINDOW)
    user32.SetWindowPos(S.app_b.hwnd, None, GAP * 2 + LEFT_W, GAP, right_w, panel_h,
                        SWP_NOZORDER | SWP_SHOWWINDOW)


def wnd_proc(hwnd, msg, wparam, lparam):
    if msg == WM_PAINT:
        ps = PAINTSTRUCT()
        hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
        rc = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rc))
        brush = gdi32.CreateSolidBrush(0x00FAF3EE)   # 0x00BBGGRR = #EEF3FA
        user32.FillRect(hdc, ctypes.byref(rc), brush)
        gdi32.DeleteObject(brush)
        user32.EndPaint(hwnd, ctypes.byref(ps))
        return 0
    if msg == WM_ERASEBKGND:
        return 1                                  # 背景在 WM_PAINT 里画，避免闪
    if msg == WM_SIZE:
        if S.layout_ready:
            layout_children(lparam & 0xFFFF, (lparam >> 16) & 0xFFFF)
        return 0
    if msg == WM_TIMER:
        return 0                                  # 只是把主循环叫醒一帧，渲染在循环里做
    if msg == WM_DESTROY:
        user32.PostQuitMessage(0)
        return 0
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


WNDPROC_REF = WNDPROC(wnd_proc)     # 必须留引用，否则回调被 GC 掉直接崩

# ---- Win32 原型 ----
# 不声明 argtypes 的话，ctypes 按 32 位 int 传参，像 HINSTANCE/HWND 这种
# 「值大于 2^31 的句柄」会直接 OverflowError，或者被静默截断成野指针。
user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
user32.RegisterClassW.restype = wt.WORD
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.CreateWindowExW.restype = wt.HWND
user32.DestroyWindow.argtypes = [wt.HWND]
user32.DestroyWindow.restype = wt.BOOL
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wt.UINT]
user32.SetWindowPos.restype = wt.BOOL
user32.LoadCursorW.argtypes = [wt.HINSTANCE, wt.LPCWSTR]
user32.LoadCursorW.restype = HCURSOR
user32.BeginPaint.argtypes = [wt.HWND, ctypes.POINTER(PAINTSTRUCT)]
user32.BeginPaint.restype = wt.HDC
user32.EndPaint.argtypes = [wt.HWND, ctypes.POINTER(PAINTSTRUCT)]
user32.EndPaint.restype = wt.BOOL
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetClientRect.restype = wt.BOOL
user32.AdjustWindowRect.argtypes = [ctypes.POINTER(wt.RECT), wt.DWORD, wt.BOOL]
user32.AdjustWindowRect.restype = wt.BOOL
user32.FillRect.argtypes = [wt.HDC, ctypes.POINTER(wt.RECT), wt.HBRUSH]
user32.FillRect.restype = ctypes.c_int
user32.SetTimer.argtypes = [wt.HWND, ctypes.c_size_t, wt.UINT, wt.LPVOID]
user32.SetTimer.restype = ctypes.c_size_t
user32.PostQuitMessage.argtypes = [ctypes.c_int]
user32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
user32.GetMessageW.restype = wt.BOOL
user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
gdi32.CreateSolidBrush.argtypes = [wt.COLORREF]
gdi32.CreateSolidBrush.restype = wt.HBRUSH
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteObject.restype = wt.BOOL
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetModuleHandleW.restype = wt.HINSTANCE


def show_counter() -> None:
    if S.app_a:
        S.app_a.set_text(S.ids_a["a_count"], str(S.count))
    if S.app_b:
        S.app_b.set_text(S.ids_b["b_count"], str(S.count))
    print(f"计数 -> {S.count}")


def drain(app: binui.App, panel: str, ids: dict[str, int]) -> None:
    while (ev := app.poll()) is not None:
        if ev.type == binui.CLICK:
            name = next((k for k, v in ids.items() if v == ev.node_id), f"#{ev.node_id}")
            print(f"[{panel}] click: {name}")
            if name in ("a_inc", "b_inc"):
                S.count += 1
                show_counter()
            elif name == "b_quit":
                user32.DestroyWindow(S.hwnd)
        elif ev.type == binui.INPUT:
            print(f"[{panel}] input: {ev.text!r}")
            text = f"输入框内容：{ev.text}" if ev.text else "…"
            app.set_text(ids["b_echo"], text)
        elif ev.type == binui.RESIZE:
            print(f"[{panel}] resize -> {ev.width}x{ev.height}")


def build_panel(html_path: Path, ids_key: str, names: tuple[str, ...]) -> binui.App:
    layout = binui.compile_source(html_path.read_text(encoding="utf-8"))
    app = binui.App(layout, asset_dir=HERE)
    ids = {n: app.find(n) for n in names}
    getattr(S, ids_key).update(ids)
    return app


def main() -> int:
    print(f"binui_c {binui.version()} / python {sys.version.split()[0]}")

    hinst = kernel32.GetModuleHandleW(None)
    cls = "BinUIChildHostDemo"
    wc = WNDCLASSW()
    wc.style = 0x0002 | 0x0001            # CS_HREDRAW | CS_VREDRAW
    wc.lpfnWndProc = WNDPROC_REF
    wc.hInstance = hinst
    wc.hCursor = user32.LoadCursorW(None, ctypes.cast(ctypes.c_void_p(32512), wt.LPCWSTR))
    wc.lpszClassName = cls
    if not user32.RegisterClassW(ctypes.byref(wc)):
        print("RegisterClassW 失败", file=sys.stderr)
        return 1

    rect = wt.RECT(0, 0, CLIENT_W, CLIENT_H)
    user32.AdjustWindowRect(ctypes.byref(rect), WS_OVERLAPPEDWINDOW, False)
    hwnd = user32.CreateWindowExW(
        0, cls, "binUI 子窗口模式 · 宿主 Win32 窗口", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
        120, 80, rect.right - rect.left, rect.bottom - rect.top,
        None, None, hinst, None)
    if not hwnd:
        print("CreateWindowExW 失败", file=sys.stderr)
        return 1
    S.hwnd = hwnd

    # ---- 两块 binUI：各自独立编译、独立实例 ----
    S.app_a = build_panel(HERE / "child_panel_a.html", "ids_a", ("a_inc", "a_count"))
    S.app_b = build_panel(HERE / "child_panel_b.html", "ids_b",
                          ("b_inc", "b_quit", "b_count", "b_input", "b_echo"))

    cli = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cli))
    w, h = cli.right - cli.left, cli.bottom - cli.top
    right_w = max(160, w - LEFT_W - GAP * 3)
    panel_h = max(160, h - GAP * 2)

    S.app_a.open_child(hwnd, GAP, GAP, LEFT_W, panel_h)
    S.app_b.open_child(hwnd, GAP * 2 + LEFT_W, GAP, right_w, panel_h)
    S.layout_ready = True

    show_counter()
    user32.SetTimer(hwnd, 1, 8, None)     # 8ms 节拍：动画/脏区靠它驱动
    print(f"宿主窗口 {w}x{h}，两块子窗口已挂上")

    msg = wt.MSG()
    while True:
        r = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
        if r == 0 or r == -1:
            break
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))
        # 宿主的渲染节拍：每轮消息后各出一帧（库不碰消息队列）
        for app, key, panel in ((S.app_a, "ids_a", "A"), (S.app_b, "ids_b", "B")):
            if app is None:
                continue
            if not app.render():
                print("窗口已关闭")
                break
            drain(app, panel, getattr(S, key))

    print("退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
