// vk_window.cpp - 真实窗口 + 交换链的 Vulkan 验收工具（P2b）
//
// 用法: vk_window <ui file> [--frames N] [--out shot.png] [--resize] [--gdi] [--transparent]
//
// 为什么需要它：vk_shot 只证明"离屏渲染正确"，证明不了窗口模式 —— 交换链、acquire/present、
// resize 后重建、直翻转守卫、透明窗口读回都是窗口才有的路径。这个工具把窗口真正开起来，
// 跑 N 帧，然后**把交换链图像读回**写 PNG：
//   · 读的是交换链图像本身（setCaptureReadback），不是 resolve 图 —— 证明真的画进了交换链；
//   · 拿这张 PNG 与 gl_shot 的同尺寸输出逐像素比，就是窗口模式的像素验收。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"
#include "backends/binui_draw_nodes.h"
#include "backends/binui_vk.h"
#include "save_png.h"

namespace {

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, msg, wp, lp);
}

bool Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) return false;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return true;
}

double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: vk_window <ui file> [--frames N] [--out shot.png] [--resize] "
                    "[--gdi] [--transparent]\n");
        return 1;
    }
    const char* uiPath = argv[1];
    int frames = 60;
    const char* outPath = nullptr;
    bool doResize = false;
    bool forceGdi = false;
    bool fullscreen = false;
    bool transparent = false;
    bool dirtyMode = false;   // 只重画脏矩形（验证持久累积帧 + 局部清屏这条不变量）
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--resize") doResize = true;
        else if (a == "--gdi") forceGdi = true;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--transparent") transparent = true;
        else if (a == "--dirty") dirtyMode = true;
    }

    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    const bool gdip = Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr) == Gdiplus::Ok;

    binui::CompileResult res = binui::CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        std::fprintf(stderr, "[vk_window] 编译失败（布局为空）\n");
        return 1;
    }
    for (const auto& w : res.warnings) std::fprintf(stderr, "[vk_window] warning: %s\n", w.c_str());

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[vk_window] 布局加载失败\n");
        return 1;
    }
    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int W = wd.present && wd.width ? static_cast<int>(wd.width)
                                   : (nodes.empty() ? 900 : static_cast<int>(nodes[0].width));
    int H = wd.present && wd.height ? static_cast<int>(wd.height)
                                    : (nodes.empty() ? 640 : static_cast<int>(nodes[0].height));
    renderer.setSize(W, H);
    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(uiPath), paths, std::string());
            if (!imgs.empty()) renderer.loadResources(imgs);
        }
    }

    // --fullscreen：把窗口铺满整块显示器，用来验证直翻转守卫（应自动切到 GDI 读回）
    if (fullscreen) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        W = mi.rcMonitor.right - mi.rcMonitor.left;
        H = mi.rcMonitor.bottom - mi.rcMonitor.top;
        renderer.setSize(W, H);
        std::printf("[vk_window] --fullscreen: %dx%d\n", W, H);
    }

    // ---- 窗口 ----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW（没定义 UNICODE，宏是窄版）
    wc.lpszClassName = L"binui_vk_window";
    RegisterClassExW(&wc);

    const DWORD style = WS_POPUP | WS_VISIBLE;
    HWND hwnd = CreateWindowExW(transparent ? WS_EX_LAYERED : 0, wc.lpszClassName, L"vk_window",
                               style, 40, 40, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[vk_window] 建窗失败\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    binui::BinUIVK gfx;
    if (!gfx.init(hwnd, W, H, false)) {
        std::fprintf(stderr, "[vk_window] Vulkan 初始化失败\n");
        DestroyWindow(hwnd);
        return 1;
    }
    gfx.setTransparent(transparent);
    gfx.setCaptureReadback(true);   // 每帧把交换链图像读回，最后写 PNG

    double t0 = NowMs();
    int drawn = 0;
    int resizedAt = doResize ? frames / 2 : -1;
    for (int i = 0; i < frames;) {
        if (!Pump()) break;
        if (i == resizedAt) {
            // 模拟用户拖窗口：改窗口尺寸 + 通知渲染器/后端。交换链要在这一步重建。
            W += 160;
            H += 80;
            SetWindowPos(hwnd, nullptr, 40, 40, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
            renderer.setSize(W, H);
            gfx.resize(W, H);
            std::printf("[vk_window] resize -> %dx%d\n", W, H);
            i += 2;   // resize 帧不算常规帧（交换链可能这一帧才建好）
        }
        if (dirtyMode && i > 0) {
            // 只清/只画四分之一区域，四帧轮完一整幅：内容应与会全量重画的完全一致
            const int q = i % 4;
            int rc[4] = {(q & 1) ? W / 2 : 0, (q & 2) ? H / 2 : 0, 0, 0};
            rc[2] = rc[0] + W / 2;
            rc[3] = rc[1] + H / 2;
            gfx.beginFrame(rc);
            binui::drawNodesBinUI(gfx, renderer, rc, binui::CanvasFn{});
        } else {
            gfx.beginFrame(nullptr);
            binui::drawNodesBinUI(gfx, renderer, nullptr, binui::CanvasFn{});
        }
        if (forceGdi) gfx.presentViaGdi();
        else gfx.present();
        ++i;
        ++drawn;
    }
    const double ms = NowMs() - t0;

    if (outPath) {
        int rw = 0, rh = 0;
        const uint8_t* px = gfx.readbackPixels(rw, rh);
        if (!px) {
            std::fprintf(stderr, "[vk_window] 读回为空（交换链捕获没打开？）\n");
        } else if (!binuishot::SaveBgraPng(outPath, px, rw, rh)) {
            std::fprintf(stderr, "[vk_window] 保存 PNG 失败: %s\n", outPath);
        } else {
            std::printf("[vk_window] saved %s (%dx%d)\n", outPath, rw, rh);
        }
    }

    std::printf("[vk_window] %d 帧 / %.1f ms = %.1f fps，present=%s\n", drawn, ms,
                ms > 0 ? drawn * 1000.0 / ms : 0.0,
                gfx.presentUsedGdi() ? "GDI 读回（透明/全屏守卫/强制）" : "交换链");

    gfx.shutdown();
    DestroyWindow(hwnd);
    if (gdip) Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
