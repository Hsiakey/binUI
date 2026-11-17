// win_create_time.cpp - 对照实验：窗口创建到底花多少毫秒（量启动耗时时用来排除系统成本）
//
// 背景：binUI 的 flexshow 实测"建窗"那一段 ~200 ms，怀疑过是 CS_OWNDC / WS_VISIBLE /
// 无边框样式导致的。用这个程序量同一台机器上的**裸窗口**，结论是这是"进程里第一个窗口"
// 的一次性系统成本（user32/DWM/主题第一次初始化），与窗口样式、与 binUI 都无关：
//
//   popup+visible(裸)           CreateWindowEx  153.4 ms  hwnd=ok     ← 进程内第一个
//   popup+visible+OWNDC         CreateWindowEx    8.2 ms  hwnd=ok
//   popup 不 visible            CreateWindowEx    2.0 ms  hwnd=ok
//   overlapped 标准窗           CreateWindowEx    8.3 ms  hwnd=ok
//   再次 popup+visible          CreateWindowEx    8.1 ms  hwnd=ok
//
// 编译（在仓库根目录）：
//   x86_64-w64-mingw32-g++ -O2 -static-libgcc -static-libstdc++ \
//       -o build-mingw/tmp/win_create_time.exe tools/win_create_time.cpp -luser32 -lgdi32
//   ./build-mingw/tmp/win_create_time.exe
//
// 详见 docs/startup.md §3。
#include <windows.h>

#include <chrono>
#include <cstdio>

static double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// 正经窗口过程：WM_PAINT 直接 ValidateRect。以前测过一次"返回 DefWindowProcW(nullptr,..)"
// 的假窗口过程 —— 建窗会失败（hwnd=NULL）但仍然耗掉 140 ms，说明那段时间花在 CreateWindow
// 内部，不在我们的消息处理里。
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) { ValidateRect(h, nullptr); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void run(const char* name, UINT style, DWORD exStyle, UINT clsStyle) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = clsStyle;
    wchar_t cls[64];
    wsprintfW(cls, L"BinUITime%u%u", static_cast<unsigned>(style), static_cast<unsigned>(exStyle));
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    const double t1 = nowMs();
    HWND h = CreateWindowExW(exStyle, cls, L"t", style, 100, 100, 1100, 720,
                             nullptr, nullptr, wc.hInstance, nullptr);
    const double t2 = nowMs();
    std::printf("%-28s CreateWindowEx %6.1f ms  hwnd=%s\n",
                name, t2 - t1, h ? "ok" : "NULL");
    if (h) DestroyWindow(h);
    UnregisterClassW(cls, wc.hInstance);
}

int main() {
    run("popup+visible(裸)", WS_POPUP | WS_MAXIMIZEBOX | WS_VISIBLE, 0, 0);
    run("popup+visible+OWNDC", WS_POPUP | WS_MAXIMIZEBOX | WS_VISIBLE, 0,
        CS_OWNDC | CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS);
    run("popup 不 visible", WS_POPUP | WS_MAXIMIZEBOX, 0, 0);
    run("overlapped 标准窗", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0);
    run("再次 popup+visible", WS_POPUP | WS_MAXIMIZEBOX | WS_VISIBLE, 0, 0);
    return 0;
}
