// gl_shot.cpp - 用 GL 后端把 UI 渲染成 PNG（离屏抓图）
//
// 为什么需要它：OpenGL 窗口的内容抓不到 —— PrintWindow 对双缓冲 GL 窗口返回全黑，
// BitBlt 只能抓被合成到桌面的部分。所以文档图一直只能用 GDI 离屏渲染（tools/offscreen_render），
// 而 GDI 有自己的字体链接，"GL 后端画出来到底什么样"一直没人验证过。
//
// 做法：建一个**不显示**的窗口 → 建 GL 上下文 → 渲染一帧 → presentViaGdi 把离屏
// 像素读回并 StretchDIBits 到窗口 DC → 从 DC BitBlt 出来存 PNG。
// 走出来的是 GL 后端真实的像素，跟窗口可见性无关。
//
// 用法: gl_shot <ui file> <out.png> [width] [height]
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
#include "backends/binui_gl-gdi.h"

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
        std::printf("usage: gl_shot <ui file> <out.png> [width] [height]\n");
        return 1;
    }

    binui::CompileResult res = binui::CompileBinUIFileEx(argv[1]);
    for (const auto& w : res.warnings) std::fprintf(stderr, "[gl_shot] warning: %s\n", w.c_str());
    if (res.layout.empty()) {
        std::fprintf(stderr, "gl_shot: 编译失败 %s\n", argv[1]);
        return 1;
    }

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "gl_shot: 布局解析失败\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    int W = argc > 3 ? std::atoi(argv[3]) : (wd.present && wd.width ? (int)wd.width : 900);
    int H = argc > 4 ? std::atoi(argv[4]) : (wd.present && wd.height ? (int)wd.height : 840);
    if (W <= 0 || H <= 0) { W = 900; H = 840; }
    renderer.setSize(W, H);

    // 布局里的图片资源（GL 后端按 imageIndex 上传纹理）
    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(argv[1]), paths, std::string());
            if (!imgs.empty()) renderer.loadResources(imgs);
        }
    }

    // 不显示的窗口：GL 上下文不需要窗口可见
    SetProcessDPIAware();
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"gl_shot", WS_POPUP,
                                0, 0, W, H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "gl_shot: 建窗失败\n");
        return 1;
    }

    binui::BinUIGLGdi gfx;
    if (!gfx.init(hwnd, W, H, false)) {
        std::fprintf(stderr, "gl_shot: GL 上下文创建失败（这台机器没有可用的 OpenGL 3.3？）\n");
        DestroyWindow(hwnd);
        return 1;
    }

    gfx.beginFrame(nullptr);
    binui::drawNodesGLGdi(gfx, renderer, nullptr);
    // 必须让窗口可见：GDI 对**隐藏**窗口的 StretchDIBits 直接丢弃（不报错），
    // 读回路径会得到一片空白。这跟"能不能画"无关，纯粹是 GDI 的呈现优化。
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    gfx.presentViaGdi();   // 读回离屏像素 → StretchDIBits 到窗口 DC

    // 从窗口 DC 抓像素（此时像素已经在 DC 上，与窗口是否可见无关）
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
            std::printf("saved %s (%dx%d)\n", argv[2], W, H);
            rc = 0;
        } else {
            std::fprintf(stderr, "gl_shot: 保存失败\n");
        }
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    GdiplusShutdown(token);
    gfx.shutdown();
    DestroyWindow(hwnd);
    return rc;
}
