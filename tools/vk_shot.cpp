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
#include "binui_paths.h"   // ex::LoadImages / DirOf（与 gl_shot 同一份图片加载）
#include "backends/binui_draw_nodes.h"
#include "backends/binui_vk.h"
#include "save_png.h"


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

    // 布局里的图片资源（与 gl_shot 同一份加载逻辑；后端按图片 index 上传纹理）
    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(uiPath), paths, std::string());
            if (!imgs.empty()) renderer.loadResources(imgs);
        }
    }

    binui::BinUIVK gfx;
    if (!gfx.init(nullptr, w, h, false)) {
        std::fprintf(stderr, "[vk_shot] Vulkan 后端初始化失败\n");
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    // 只渲一帧：P3 起纹理上传可以发生在帧内（帧外产生的上传在 beginFrame 里补录），
    // 没有"预热帧"这回事。一帧出图本身就是对它的验收 —— 需要两帧就说明还有资源
    // 是延迟一帧才就绪的。
    gfx.beginFrame(nullptr);
    binui::drawNodesBinUI(gfx, renderer, nullptr, binui::CanvasFn{});
    gfx.present();

    int rw = 0, rh = 0;
    const uint8_t* px = gfx.readbackPixels(rw, rh);
    if (!px) {
        std::fprintf(stderr, "[vk_shot] 读回失败\n");
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    if (!binuishot::SaveBgraPng(outPath, px, rw, rh)) {
        std::fprintf(stderr, "[vk_shot] 保存 PNG 失败: %s\n", outPath);
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    std::printf("saved %s (%dx%d)\n", outPath, rw, rh);
    gfx.shutdown();
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
