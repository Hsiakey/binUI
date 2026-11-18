// partial_shot.cpp - 局部重绘一致性检查（hover 脏矩形 vs 整帧）
//
// 为什么需要它：binUI 的外壳在"只有某个节点变了"时只重画那个节点的脏矩形
// （renderer.invalidRect → InvalidateRect → beginFrame(clip)）。于是**同一帧画面
// 会由两条路径产生**：整帧重绘、以及"在一个脏矩形里重绘"。两条路径必须逐像素一致，
// 否则脏矩形边界上会留下硬边的"影子 / 边框"—— 而这类错在整帧渲染的像素基准里
// 永远看不到（clip = 整屏时判据的边界情况碰不到任何节点）。
//
// 做法（不需要人肉 hover）：
//   1. 建不显示的窗口 + GL 上下文，先整帧画一遍（模拟窗口刚出现时的第一帧）；
//   2. 在 (x,y) 触发 hover，推进过渡动画；
//   3. 按 invalidRect 算脏矩形，再画第二帧（只清 + 只画这块）；
//   4. 抓下这块的像素 B；随后把整帧重画一遍（同样的 hover 状态）抓 C；
//      逐像素比 B/C，差异超过阈值就非 0 退出。
//
// 阈值：允许极少数像素差 1 —— MSAA 4x 的解析对"只清了局部的缓冲"与"整帧清的缓冲"
// 在最外一圈覆盖率上是同一套数学，但浮点解析顺序仍可能在**脏矩形边界**上差 1。
// 真出问题（判据写错）时差的是整块区域的底色/阴影，差值是十几到几十，一眼就能分开。
//
// 用法:
//   partial_shot <ui> <out.png> <x> <y> [frames] [W] [H]     出图（保存局部重绘结果）
//   partial_shot --check <ui> <x> <y> [frames] [W] [H]        对拍（非 0 退出 = 不一致）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <gdiplus.h>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "binui_paths.h"
#include "backends/binui_gl.h"

using namespace Gdiplus;

namespace {

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<ImageCodecInfo> codecs(size / sizeof(ImageCodecInfo));
    GetImageEncoders(num, size, codecs.data());
    for (UINT i = 0; i < num; i++)
        if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; return 0; }
    return -1;
}

// 从窗口 DC 抓整幅客户端像素（RGBA 顺序的 BGRX，够用来比差异）
bool GrabClient(HDC dc, int w, int h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const bool ok = BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY) != 0;
    if (ok) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;      // 顶层向下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(mem, bmp, 0, static_cast<UINT>(h), out.data(), &bi, DIB_RGB_COLORS);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return ok;
}

bool SavePng(const std::vector<uint8_t>& rgba, int w, int h, const char* path) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::memcpy(bits, rgba.data(), rgba.size());

    GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &in, nullptr);
    bool ok = false;
    {
        Bitmap gbmp(bmp, nullptr);
        CLSID png;
        wchar_t wp[512];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 512);
        ok = (GetEncoderClsid(L"image/png", &png) == 0 && gbmp.Save(wp, &png, nullptr) == Ok);
    }
    GdiplusShutdown(token);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    bool check = false;
    int argi = 1;
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0) { check = true; argi = 2; }
    if (argc < argi + 4) {
        std::printf("usage: partial_shot [--check] <ui> <out.png|-> <x> <y> [frames] [W] [H]\n");
        return 2;
    }
    const char* uiPath = argv[argi];
    const char* outPath = argv[argi + 1];
    const float hoverX = static_cast<float>(std::atof(argv[argi + 2]));
    const float hoverY = static_cast<float>(std::atof(argv[argi + 3]));
    const int frames = argc > argi + 4 ? std::atoi(argv[argi + 4]) : 40;
    const int Warg = argc > argi + 5 ? std::atoi(argv[argi + 5]) : 0;
    const int Harg = argc > argi + 6 ? std::atoi(argv[argi + 6]) : 0;

    binui::CompileResult res = binui::CompileBinUIFileEx(uiPath);
    for (const auto& w : res.warnings) std::fprintf(stderr, "[partial] warning: %s\n", w.c_str());
    if (res.layout.empty()) { std::fprintf(stderr, "partial_shot: 编译失败 %s\n", uiPath); return 2; }

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 2;

    const auto& wd = renderer.windowDesc();
    int W = Warg > 0 ? Warg : (wd.present && wd.width ? static_cast<int>(wd.width) : 900);
    int H = Harg > 0 ? Harg : (wd.present && wd.height ? static_cast<int>(wd.height) : 840);
    if (W <= 0 || H <= 0) { W = 900; H = 840; }
    renderer.setSize(W, H);

    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(uiPath), paths, std::string());
            if (!imgs.empty()) { renderer.setImageOrder(paths); renderer.loadResources(imgs); }
        }
    }

    SetProcessDPIAware();
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"partial_shot", WS_POPUP,
                                0, 0, W, H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 2;

    binui::BinUIGL gfx;
    if (!gfx.init(hwnd, W, H, false)) {
        std::fprintf(stderr, "partial_shot: GL 上下文创建失败（这台机器没有 OpenGL 3.3？）\n");
        DestroyWindow(hwnd);
        return 2;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    HDC dc = GetDC(hwnd);

    // ---- 第 1 帧：整屏（窗口刚出现时的那一帧）----
    gfx.beginFrame(nullptr);
    binui::drawNodesGL(gfx, renderer, nullptr);
    gfx.presentViaGdi();

    // ---- hover + 推进过渡 ----
    renderer.setHover(hoverX, hoverY);
    for (int i = 0; i < frames; ++i) { renderer.stepAnimations(); Sleep(8); }

    // ---- 第 2 帧：只重画 hovered 节点的脏矩形（与外壳 invalidateNode 同口径）----
    int clip[4] = {0, 0, W, H};
    const int id = renderer.hoveredNode();
    float bx = 0, by = 0, bw = 0, bh = 0;
    if (id >= 0 && renderer.invalidRect(static_cast<uint16_t>(id), bx, by, bw, bh)) {
        clip[0] = static_cast<int>(bx);
        clip[1] = static_cast<int>(by);
        clip[2] = static_cast<int>(bx) + static_cast<int>(bw);
        clip[3] = static_cast<int>(by) + static_cast<int>(bh);
        if (clip[0] < 0) clip[0] = 0;
        if (clip[1] < 0) clip[1] = 0;
        if (clip[2] > W) clip[2] = W;
        if (clip[3] > H) clip[3] = H;
    }
    gfx.beginFrame(clip);
    binui::drawNodesGL(gfx, renderer, clip);
    gfx.presentViaGdi();

    std::vector<uint8_t> partial;
    GrabClient(dc, W, H, partial);
    std::printf("[partial] hovered=%d clip=(%d,%d)-(%d,%d)\n", id, clip[0], clip[1], clip[2], clip[3]);

    int rc = 0;
    if (check) {
        // ---- 第 3 帧：同样 hover 状态下整帧重画，作为"标准答案"----
        gfx.beginFrame(nullptr);
        binui::drawNodesGL(gfx, renderer, nullptr);
        gfx.presentViaGdi();
        std::vector<uint8_t> full;
        GrabClient(dc, W, H, full);

        // BGRX：只比 RGB 三通道（alpha 在窗口 DC 上不可靠）
        size_t diffPx = 0;
        int maxDiff = 0;
        int firstX = -1, firstY = -1;
        for (int y = 0; y < H; ++y) {
            const uint8_t* a = full.data() + static_cast<size_t>(y) * W * 4;
            const uint8_t* b = partial.data() + static_cast<size_t>(y) * W * 4;
            for (int x = 0; x < W; ++x, a += 4, b += 4) {
                const int d0 = std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0]));
                const int d1 = std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]));
                const int d2 = std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
                const int d = std::max(d0, std::max(d1, d2));
                if (d > maxDiff) maxDiff = d;
                if (d > 0) {
                    ++diffPx;
                    if (firstX < 0) { firstX = x; firstY = y; }
                }
            }
        }
        // 允许：极少像素差 1（MSAA 解析顺序）；不允许：任何像素差 ≥2 或成片差 1
        const size_t kMaxPixels = 64;
        const bool ok = (diffPx <= kMaxPixels && maxDiff <= 1);
        std::printf("[partial] 局部重绘 vs 整帧: diff=%zu max=%d%s\n", diffPx, maxDiff,
                    firstX >= 0 ? "" : "（逐像素一致）");
        if (firstX >= 0) std::printf("[partial] 首个差异像素 (%d,%d)\n", firstX, firstY);
        if (!ok) {
            std::printf("[partial] FAIL: 局部重绘与整帧不一致（阈值 diff<=%zu 且 max<=1）\n",
                        kMaxPixels);
            if (firstX >= 0) SavePng(partial, W, H, "build-mingw/tmp/partial_diff_partial.png");
            rc = 1;
        } else {
            std::printf("[partial] OK\n");
        }
    } else if (std::strcmp(outPath, "-") != 0) {
        if (!SavePng(partial, W, H, outPath)) { std::fprintf(stderr, "[partial] 保存失败\n"); rc = 2; }
        else std::printf("saved %s (%dx%d)\n", outPath, W, H);
    }

    ReleaseDC(hwnd, dc);
    gfx.shutdown();
    DestroyWindow(hwnd);
    return rc;
}
