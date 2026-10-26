// offscreen_render.cpp - 离屏渲染工具：真实布局 -> drawNodesGDI -> PNG
// 用法: offscreen_render <ui文件> <输出.png> [--scrollTo y] [--setText id=text ...]
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <windows.h>
#include <gdiplus.h>
#include "binui_compiler.h"
#include "binui_component.h"
#include "binui_renderer.h"
#include "platform/app_win32.h"

using namespace binui;
using namespace Gdiplus;

static int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<ImageCodecInfo> codecs(size / sizeof(ImageCodecInfo));
    GetImageEncoders(num, size, codecs.data());
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(codecs[i].MimeType, format) == 0) {
            *clsid = codecs[i].Clsid;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: offscreen_render <ui file> <out.png> [--scrollTo y] [--setText id=text]\n");
        return 1;
    }
    GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &in, nullptr);

    auto res = CompileBinUIFileEx(argv[1]);
    if (res.layout.empty()) { std::printf("compile failed\n"); return 1; }

    BinUIRenderer r;
    if (!r.loadLayout(res.layout.data(), res.layout.size())) { std::printf("load failed\n"); return 1; }

    // 外挂组件（<use ... external>）：与运行时一样现场挂载，预览才完整
    const std::string uiDir = binui::ex::DirOf(argv[1]);
    if (!r.componentRefs().empty()) {
        const binui::ex::MountReport rep = binui::ex::MountAll(r, uiDir, r.layoutBytes());
        std::printf("mounted %d external component(s), %d failed\n", rep.mounted, rep.failed);
    }

    const auto& nodes = r.getNodes();
    int W = (int)nodes[0].width, H = (int)nodes[0].height;

    // 资源清单来自布局自带的 v12 资源段（不再依赖编译期的 res.images）
    const std::vector<std::string> imgPaths = r.imagePaths();
    std::unordered_map<std::string, std::vector<uint8_t>> resources;
    for (const auto& p : imgPaths) {
        FILE* f = nullptr;
        fopen_s(&f, (uiDir + p).c_str(), "rb");
        if (!f) fopen_s(&f, p.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> buf(sz);
            fread(buf.data(), 1, sz, f);
            fclose(f);
            resources[p] = std::move(buf);
        } else {
            std::printf("warn: image %s not found\n", p.c_str());
        }
    }
    r.setImageOrder(imgPaths);
    r.loadResources(resources);

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--scrollTo" && i + 2 < argc) {
            r.scrollTo(r.getNodes()[1].id, 0, (float)atof(argv[i + 2]));
            i += 2;
        } else if (arg == "--setText" && i + 1 < argc) {
            std::string spec = argv[i + 1];
            size_t eq = spec.find('=');
            if (eq != std::string::npos) {
                r.setText((uint16_t)atoi(spec.substr(0, eq).c_str()), spec.substr(eq + 1));
            }
            i += 1;
        }
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);

    RECT clip = { 0, 0, W, H };
    drawNodesGDI(mem, r, &clip);

    Bitmap bmp(dib, nullptr);
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) == 0) {
        wchar_t wpath[512];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wpath, 512);
        if (bmp.Save(wpath, &pngClsid, nullptr) == Ok) {
            std::printf("saved %s (%dx%d)\n", argv[2], W, H);
        } else {
            std::printf("save failed\n");
        }
    }

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    GdiplusShutdown(token);
    return 0;
}
