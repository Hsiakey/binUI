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
        std::printf("usage: offscreen_render <ui file>|--default <out.png> [--scrollTo y] [--setText id=text]\n");
        return 1;
    }
    GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &in, nullptr);

    // --default：渲染引擎内置的默认布局（GenerateDefaultBinUI，无 UI 文件时的兜底界面）。
    // 以前这张图只能手工截，现在 CI/README 都能一条命令重出。
    const bool useDefault = (std::string(argv[1]) == "--default");
    std::vector<uint8_t> layout;
    if (useDefault) {
        layout = binui::GenerateDefaultBinUI();
    } else {
        auto res = CompileBinUIFileEx(argv[1]);
        layout = res.layout;
    }
    if (layout.empty()) { std::printf("compile failed\n"); return 1; }

    BinUIRenderer r;
    if (!r.loadLayout(layout.data(), layout.size())) { std::printf("load failed\n"); return 1; }

    // 外挂组件（<use ... external>）：与运行时一样现场挂载，预览才完整
    const std::string uiDir = useDefault ? std::string() : binui::ex::DirOf(argv[1]);
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
        } else if (arg == "--dump") {
            // 列出节点（排查坐标/焦点用）：id 类型 矩形 文本
            for (const auto& nd : r.getNodes())
                std::printf("node %-3u type=%u rect=(%.0f,%.0f %.0fx%.0f) radius=%u text=\"%s\"\n",
                            (unsigned)nd.id, (unsigned)nd.type, nd.x, nd.y, nd.width, nd.height,
                            (unsigned)nd.radius, nd.text.c_str());
        } else if ((arg == "--hover" || arg == "--active") && i + 1 < argc) {
            // 模拟指针悬停/按下：验证 hover 缩放、状态样式、边框在缩放后的观感
            const std::string want = argv[i + 1];
            uint16_t nid = 0xFFFF;
            if (!want.empty() && want.find_first_not_of("0123456789") == std::string::npos) {
                nid = (uint16_t)atoi(want.c_str());
            } else {
                nid = r.findNodeByName(want);
                if (nid == 0xFFFF)
                    for (const auto& nd : r.getNodes())
                        if (nd.text == want) { nid = nd.id; break; }
            }
            const BinUIRenderer::NodeInfo* hit = nullptr;
            for (const auto& nd : r.getNodes()) if (nd.id == nid) { hit = &nd; break; }
            if (hit) {
                const float cx = hit->x + hit->width * 0.5f;
                const float cy = hit->y + hit->height * 0.5f;
                if (arg == "--hover") r.setHover(cx, cy);
                else r.press(cx, cy);
                // 让 transition 走完再画：stepAnimations 用的是真实时钟
                Sleep(280);
                r.stepAnimations();
                std::printf("%s node %u at (%.0f,%.0f)\n", arg.c_str() + 2, (unsigned)nid, cx, cy);
            } else {
                std::printf("warn: node not found for %s\n", arg.c_str());
            }
            i += 1;
        } else if (arg == "--focus" && i + 1 < argc) {
            // 让某个节点处于键盘焦点（验证焦点环/状态样式；按名字或 id 都行）
            const std::string want = argv[i + 1];
            bool found = false;
            uint16_t nid = 0xFFFF;
            if (!want.empty() && want.find_first_not_of("0123456789") == std::string::npos) {
                nid = (uint16_t)atoi(want.c_str());   // 纯数字 = 节点 id
                for (const auto& nd : r.getNodes()) if (nd.id == nid) { found = true; break; }
            } else {
                nid = r.findNodeByName(want);
                for (const auto& nd : r.getNodes()) if (nd.id == nid) { found = true; break; }
                if (!found) {
                    for (const auto& nd : r.getNodes())
                        if (nd.text == want) { nid = nd.id; found = true; break; }
                }
            }
            if (found) {
                r.setKeyboardFocus(nid);
                std::printf("focused node %u (%s)\n", (unsigned)nid, want.c_str());
            } else {
                std::printf("warn: node \"%s\" not found for --focus\n", want.c_str());
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

    // GDI+ 的 Bitmap(HBITMAP,…) 不拷贝像素：必须让 bmp 先析构，再 DeleteObject(dib)。
    // 以前 bmp 和 dib 同在 main 作用域里，dib 先被删、bmp 析构时访问已释放的 DIB，
    // 进程退出码变成 0xC0000005（PNG 其实已经写对了，但脚本会以为失败）。
    {
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
    }

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    std::fflush(stdout);
    GdiplusShutdown(token);
    return 0;
}
