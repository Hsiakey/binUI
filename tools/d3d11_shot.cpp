// d3d11_shot.cpp - 用 D3D11 后端把 UI 渲染成 PNG（窗口 + 读回，P7）
//
// 用法: d3d11_shot <ui file> <out.png> [--frames N] [--gdi] [--dirty]
//
// 与 vk_shot / gl_shot 的分工：D3D11 后端的交换链必须绑窗口，没有"无窗口离屏"这条腿，
// 所以这里开一个**隐藏窗口**跑 N 帧，再从离屏纹理读回像素（BinUID3D::readbackPixels）。
// 关键点：读回不经过 DWM 合成，所以窗口被遮挡、甚至没显示出来，抓到的都是真实像素 ——
// 屏幕截屏那套（PrintWindow）对 flip-model 交换链会拿到白屏。
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
#include "backends/binui_d3d11.h"
#include "save_png.h"

namespace {

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: d3d11_shot <ui file> <out.png> [--frames N] [--gdi] [--dirty]\n");
        return 1;
    }
    const char* uiPath = argv[1];
    const char* outPath = argv[2];
    int frames = 3;
    bool forceGdi = false;
    bool dirtyMode = false;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--gdi") forceGdi = true;
        else if (a == "--dirty") dirtyMode = true;
    }
    if (frames < 1) frames = 1;

    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    const bool gdip = Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr) == Gdiplus::Ok;

    binui::CompileResult res = binui::CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        std::fprintf(stderr, "[d3d11_shot] 编译失败（布局为空）\n");
        return 1;
    }
    for (const auto& w : res.warnings) std::fprintf(stderr, "[d3d11_shot] warning: %s\n", w.c_str());

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[d3d11_shot] 布局加载失败\n");
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

    // 隐藏窗口：交换链必须有 hwnd，但不需要真的显示出来（读回不经过 DWM）
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = binui::BinUID3D::appClassName();
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "[d3d11_shot] RegisterClassExW 失败: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"d3d11_shot", WS_POPUP,
                                -4000, -4000, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[d3d11_shot] 建窗失败\n");
        return 1;
    }

    binui::BinUID3D gfx;
    if (!gfx.init(hwnd, W, H, false)) {
        std::fprintf(stderr, "[d3d11_shot] D3D11 初始化失败\n");
        DestroyWindow(hwnd);
        return 1;
    }

    for (int i = 0; i < frames; i++) {
        if (!Pump()) break;
        if (dirtyMode && i > 0) {
            // 四分之一区域轮着画：内容应与会全量重画的完全一致（持久离屏 + 脏矩形的验收）
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
    }

    int rw = 0, rh = 0;
    const uint8_t* px = gfx.readbackPixels(rw, rh);
    int rc = 0;
    if (!px) {
        std::fprintf(stderr, "[d3d11_shot] 读回失败\n");
        rc = 1;
    } else if (!binuishot::SaveBgraPng(outPath, px, rw, rh)) {
        std::fprintf(stderr, "[d3d11_shot] 保存 PNG 失败: %s\n", outPath);
        rc = 1;
    } else {
        std::printf("saved %s (%dx%d)\n", outPath, rw, rh);
    }

    gfx.shutdown();
    DestroyWindow(hwnd);
    if (gdip) Gdiplus::GdiplusShutdown(gdipToken);
    return rc;
}
