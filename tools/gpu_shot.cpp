// gpu_shot.cpp - 用 **binGPU 设备层**（binui_gpu 后端）把 UI 渲染成 PNG
//
// 与 gl_shot / vk_shot / d3d11_shot 同一套做法（隐藏窗口 → 渲染一帧 → presentViaGdi
// 读回 → 从窗口 DC BitBlt → PNG），差别只有一个：这里的后端**没有自己的设备**，
// 设备/交换链/管线全在 binGPU 里，圆角 AA 在 binGPU 的片元着色器里算。
//
// 用法: gpu_shot <ui file> <out.png> [width] [height] [--bench N]
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <gdiplus.h>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "binui_paths.h"
#include "backends/binui_gpu.h"
#include "bingpu.h"

using namespace Gdiplus;

static int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<ImageCodecInfo> codecs(size / sizeof(ImageCodecInfo));
    GetImageEncoders(num, size, codecs.data());
    for (UINT i = 0; i < num; i++)
        if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; return 0; }
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: gpu_shot <ui file> <out.png> [width] [height] [--bench N]\n");
        return 1;
    }
    int benchFrames = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--bench") benchFrames = std::atoi(argv[i + 1]);

    binui::CompileResult res = binui::CompileBinUIFileEx(argv[1]);
    for (const auto& w : res.warnings) std::fprintf(stderr, "[gpu_shot] warning: %s\n", w.c_str());
    if (res.layout.empty()) {
        std::fprintf(stderr, "gpu_shot: 编译失败 %s\n", argv[1]);
        return 1;
    }

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "gpu_shot: 布局解析失败\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    int W = argc > 3 ? std::atoi(argv[3]) : (wd.present && wd.width ? (int)wd.width : 900);
    int H = argc > 4 ? std::atoi(argv[4]) : (wd.present && wd.height ? (int)wd.height : 840);
    if (W <= 0 || H <= 0) { W = 900; H = 840; }
    renderer.setSize(W, H);

    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(argv[1]), paths, std::string());
            if (!imgs.empty()) { renderer.setImageOrder(paths); renderer.loadResources(imgs); }
        }
    }

    SetProcessDPIAware();
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"gpu_shot", WS_POPUP,
                                0, 0, W, H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "gpu_shot: 建窗失败\n");
        return 1;
    }

    binui::BinUIGpu gfx;
    if (!gfx.init(hwnd, W, H, false)) {
        std::fprintf(stderr, "gpu_shot: binGPU 设备创建失败（%s）\n", bingpu_last_error());
        DestroyWindow(hwnd);
        return 1;
    }
    std::fprintf(stderr, "[gpu_shot] 后端 = %s\n", gfx.backendName());

    if (benchFrames > 0) {
        // 先预热 5 帧（图集 / 圆角模板 / 纹理上传都在头几帧里建）
        for (int i = 0; i < 5; i++) {
            gfx.beginFrame(nullptr);
            binui::drawNodesGpu(gfx, renderer, nullptr);
            gfx.flushFrame();
        }
        std::vector<double> drawMs, submitMs;
        for (int i = 0; i < benchFrames; i++) {
            const auto t0 = std::chrono::steady_clock::now();
            gfx.beginFrame(nullptr);
            binui::drawNodesGpu(gfx, renderer, nullptr);
            const auto t1 = std::chrono::steady_clock::now();
            gfx.flushFrame();
            const auto t2 = std::chrono::steady_clock::now();
            drawMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            submitMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
        std::sort(drawMs.begin(), drawMs.end());
        std::sort(submitMs.begin(), submitMs.end());
        auto med = [](std::vector<double>& v) { return v.empty() ? 0.0 : v[v.size() / 2]; };
        std::printf("[gpu-bench] %s  %dx%d  %s  %d 帧：遍历+发射 %.3f ms，提交+绘制调用 %.3f ms，"
                    "合计 %.3f ms/帧（不含交换链）\n",
                    argv[1], W, H, gfx.backendName(), benchFrames, med(drawMs), med(submitMs),
                    med(drawMs) + med(submitMs));
        gfx.shutdown();
        DestroyWindow(hwnd);
        return 0;
    }

    gfx.beginFrame(nullptr);
    binui::drawNodesGpu(gfx, renderer, nullptr);

    // GDI 对**隐藏**窗口的 SetDIBitsToDevice 直接丢弃，所以必须先让窗口可见
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    gfx.presentViaGdi();

    HDC dc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, W, H, dc, 0, 0, SRCCOPY);

    GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &in, nullptr);

    int rc = 1;
    {
        Bitmap gbmp(bmp, nullptr);
        CLSID png;
        wchar_t wp[512];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wp, 512);
        if (GetEncoderClsid(L"image/png", &png) == 0 && gbmp.Save(wp, &png, nullptr) == Ok) {
            rc = 0;
        } else {
            std::fprintf(stderr, "gpu_shot: 保存 PNG 失败\n");
        }
    }

    GdiplusShutdown(token);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    gfx.shutdown();
    DestroyWindow(hwnd);
    return rc;
}
