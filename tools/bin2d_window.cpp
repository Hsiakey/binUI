// bin2d_window.cpp - 用 bin2d 后端开一个**真窗口**跑 UI，验证"窗口呈现"这条路
//
// 它回答的问题只有一个：bin2d 后端能不能当窗口后端用。
//
// 呈现路径有两条，工具会把实际走的那条打出来（presentBackendName）：
//   bin2d+gpu    Surface → bin2d::GpuCompositor::presentSurface → binGPU 交换链
//                （这就是 master 那条分层里"bin2d 的后端交给 binGPU"）
//   bin2d+gdi    Surface → 反预乘 BGRA → StretchDIBits 贴屏
//                （binGPU 一个后端都起不来时的兜底，无显卡机器靠它）
//
// 用法: bin2d_window <ui file> [--frames N] [--shot out.png] [--headless]
//   --frames N   跑完 N 帧就退出（默认 180）
//   --shot       跑完后把窗口 DC 抓成 PNG（只有 GDI 贴屏路径能抓到 —— 交换链图像
//                抓不到，那是 gl_shot / vk_shot 一开始就踩过的坑）
//   --headless   不建窗口，纯离屏跑 N 帧（验证"没窗口时也不崩"）
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"

#include "backends/binui_bin2d.h"
#include "backends/binui_draw_nodes.h"
#include "save_png.h"

using namespace binui;

namespace {

struct Args {
    std::string ui;
    std::string shot;
    int frames = 180;
    bool headless = false;
    bool forceGdi = false;
    // 脏矩形（外壳正常情况下会自己算，这里是拿它做"脏区 vs 全屏"的对照实验）
    int clip[4] = {0, 0, 0, 0};
    bool hasClip = false;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--frames" && i + 1 < argc) a.frames = std::atoi(argv[++i]);
        else if (s == "--shot" && i + 1 < argc) a.shot = argv[++i];
        else if (s == "--headless") a.headless = true;
        else if (s == "--gdi") a.forceGdi = true;
        else if (s == "--clip" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d,%d,%d,%d", &a.clip[0], &a.clip[1], &a.clip[2],
                            &a.clip[3]) == 4)
                a.hasClip = true;
        } else if (a.ui.empty() && s.size() > 0 && s[0] != '-') a.ui = s;
    }
    return a;
}

// 从窗口 DC 抓像素（GDI 贴屏路径下 DC 上就是刚画的那一帧）
bool GrabWindow(HWND hwnd, int w, int h, const char* path) {
    HDC dc = GetDC(hwnd);
    if (!dc) return false;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    std::vector<uint8_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const int lines = GetDIBits(mem, bmp, 0, static_cast<UINT>(h), bgra.data(), &bi,
                                DIB_RGB_COLORS);
    bool ok = false;
    if (lines == h) {
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
        ok = binuishot::SaveBgraPng(path, bgra.data(), w, h);
        Gdiplus::GdiplusShutdown(token);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);
    if (args.ui.empty()) {
        std::printf("usage: bin2d_window <ui file> [--frames N] [--shot out.png] [--headless]\n");
        return 1;
    }

    CompileResult res = CompileBinUIFileEx(args.ui.c_str());
    for (const auto& w : res.warnings) std::fprintf(stderr, "[2d-window] warning: %s\n", w.c_str());
    if (res.layout.empty()) {
        std::fprintf(stderr, "bin2d_window: 编译失败 %s\n", args.ui.c_str());
        return 1;
    }
    BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "bin2d_window: 布局解析失败\n");
        return 1;
    }
    const auto& wd = renderer.windowDesc();
    int W = (wd.present && wd.width) ? static_cast<int>(wd.width) : 900;
    int H = (wd.present && wd.height) ? static_cast<int>(wd.height) : 640;
    renderer.setSize(W, H);
    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(args.ui), paths, std::string());
            if (!imgs.empty()) { renderer.setImageOrder(paths); renderer.loadResources(imgs); }
        }
    }

    BinUI2D gfx;
    HWND hwnd = nullptr;
    if (args.forceGdi) {
        // 强制走"CPU 出图 + GDI 贴屏"，绕开 binGPU（验收兜底路径用）
        _putenv("BINUI2D_NO_GPU=1");
    }
    if (!args.headless) {
        SetProcessDPIAware();
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = BinUI2D::appClassName();
        if (!RegisterClassExW(&wc)) {
            std::fprintf(stderr, "bin2d_window: 注册窗口类失败\n");
            return 1;
        }
        hwnd = CreateWindowExW(0, BinUI2D::appClassName(), L"bin2d backend",
                               WS_POPUP | WS_VISIBLE, 60, 60, W, H, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) {
            std::fprintf(stderr, "bin2d_window: 建窗失败\n");
            return 1;
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    if (!gfx.init(hwnd, W, H, true)) {
        std::fprintf(stderr, "bin2d_window: gfx.init 失败\n");
        if (hwnd) DestroyWindow(hwnd);
        return 1;
    }
    std::printf("[2d-window] %dx%d  present=%s\n", W, H, gfx.presentBackendName());

    const auto t0 = std::chrono::steady_clock::now();
    double drawMs = 0.0, presentMs = 0.0;
    const void* clip = args.hasClip ? static_cast<const void*>(args.clip) : nullptr;
    for (int i = 0; i < args.frames; ++i) {
        const auto a0 = std::chrono::steady_clock::now();
        gfx.beginFrame(clip);
        drawNodesBinUI(gfx, renderer, clip, CanvasFn{});
        const auto a1 = std::chrono::steady_clock::now();
        gfx.present();
        const auto a2 = std::chrono::steady_clock::now();
        drawMs += std::chrono::duration<double, std::milli>(a1 - a0).count();
        presentMs += std::chrono::duration<double, std::milli>(a2 - a1).count();
        if (hwnd) {
            // 泵消息：无边框窗口也要处理 WM_PAINT / WM_CLOSE，否则会被系统判为无响应
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { i = args.frames; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const int n = args.frames > 0 ? args.frames : 1;
    std::printf("[2d-window] %d 帧 %.1f ms（%.2f ms/帧）：光栅+遍历 %.2f ms，呈现 %.2f ms\n",
                args.frames, ms, ms / n, drawMs / n, presentMs / n);

    int rc = 0;
    if (!args.shot.empty()) {
        if (hwnd && GrabWindow(hwnd, W, H, args.shot.c_str())) {
            std::printf("[2d-window] 抓图 -> %s\n", args.shot.c_str());
        } else {
            std::fprintf(stderr,
                         "[2d-window] 抓图失败（交换链路径抓不到窗口 DC；用 bin2d_shot 出离屏图）\n");
            rc = 2;
        }
    }

    gfx.shutdown();
    if (hwnd) DestroyWindow(hwnd);
    return rc;
}
