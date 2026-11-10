// vk_shot.cpp - 用 Vulkan 后端把 UI 渲染成 PNG（离屏，P2）
//
// 用法: vk_shot <ui file> <out.png> [width] [height]
//
// 与 gl_shot 的分工：gl_shot 走"真实窗口 + GL 离屏 FBO + GDI 贴屏"，
// vk_shot 走"无窗口 + Vulkan 离屏目标 + vkCmdCopyImageToBuffer 读回"，
// 因此它同时是 P2/P3 的**像素验收工具**（和 GDI 基准逐像素比）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <gdiplus.h>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_draw_nodes.h"
#include "backends/binui_vk.h"

namespace {

int GetEncoderClsid(const wchar_t* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return 0;
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; i++) {
        if (std::wcscmp(codecs[i].MimeType, format) == 0) {
            *pClsid = codecs[i].Clsid;
            return 1;
        }
    }
    return 0;
}

// BGRA（预乘）→ PNG。像素 alpha 不透明时与普通 BGRA 相同；
// 半透明像素（AA 边缘）按 GDI+ 的要求反预乘回直通 alpha。
bool SaveBgraPng(const char* path, const uint8_t* bgra, int w, int h) {
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData bd{};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = bgra + static_cast<size_t>(y) * w * 4;
        auto* dst = reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(bd.Scan0) + y * bd.Stride);
        for (int x = 0; x < w; x++) {
            const uint8_t a = src[x * 4 + 3];
            dst[x * 4 + 0] = src[x * 4 + 0];   // B
            dst[x * 4 + 1] = src[x * 4 + 1];   // G
            dst[x * 4 + 2] = src[x * 4 + 2];   // R
            dst[x * 4 + 3] = a;
        }
    }
    bmp.UnlockBits(&bd);
    CLSID png;
    if (GetEncoderClsid(L"image/png", &png) == 0) return false;
    const std::wstring wp(path, path + std::strlen(path));
    return bmp.Save(wp.c_str(), &png, nullptr) == Gdiplus::Ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: vk_shot <ui file> <out.png> [width] [height]\n");
        return 1;
    }
    const char* uiPath = argv[1];
    const char* outPath = argv[2];

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &gsi, nullptr) != Gdiplus::Ok) {
        std::fprintf(stderr, "[vk_shot] GDI+ 初始化失败\n");
        return 1;
    }

    binui::CompileResult res = binui::CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        std::fprintf(stderr, "[vk_shot] 编译失败（布局为空）\n");
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    for (const auto& wmsg : res.warnings)
        std::fprintf(stderr, "[vk_shot] warning: %s\n", wmsg.c_str());

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[vk_shot] 布局加载失败\n");
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    int w = argc > 3 ? std::atoi(argv[3]) : 0;
    int h = argc > 4 ? std::atoi(argv[4]) : 0;
    if (w <= 0 || h <= 0) {
        const auto& nodes = renderer.getNodes();
        w = nodes.empty() ? 640 : static_cast<int>(nodes[0].width);
        h = nodes.empty() ? 480 : static_cast<int>(nodes[0].height);
    }
    renderer.setSize(w, h);

    binui::BinUIVK gfx;
    if (!gfx.init(nullptr, w, h, false)) {
        std::fprintf(stderr, "[vk_shot] Vulkan 后端初始化失败\n");
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    // 渲两帧：第 1 帧把圆角模板/图集等资源补建起来（帧内不许上传纹理），
    // 第 2 帧才是要出图的画面 —— 与真机窗口的"首帧预热"同构。
    for (int frame = 0; frame < 2; frame++) {
        gfx.beginFrame(nullptr);
        binui::drawNodesBinUI(gfx, renderer, nullptr, binui::CanvasFn{});
        gfx.present();
    }

    int rw = 0, rh = 0;
    const uint8_t* px = gfx.readbackPixels(rw, rh);
    if (!px) {
        std::fprintf(stderr, "[vk_shot] 读回失败\n");
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    if (!SaveBgraPng(outPath, px, rw, rh)) {
        std::fprintf(stderr, "[vk_shot] 保存 PNG 失败: %s\n", outPath);
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    std::printf("saved %s (%dx%d)\n", outPath, rw, rh);
    gfx.shutdown();
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
