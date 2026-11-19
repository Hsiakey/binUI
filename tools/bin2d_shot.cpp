// bin2d_shot.cpp - 用 bin2d 后端把 UI 渲染成 PNG（**纯离屏**，不需要窗口）
//
// 与 gl_shot / vk_shot / d3d11_shot 的区别：那三个都要先建一个窗口（交换链绑窗口），
// 再靠 GDI 把窗口 DC 抓出来 —— 因为交换链图像抓不到。bin2d 后端没有交换链：
// 像素直接落在 bin2d::Surface 上，写完就能存 PNG。
//
// 所以这个工具是**无窗口、无驱动、无 GPU** 的：一台没有显卡、没有 OpenGL 的机器
// 也能跑（CI 里跑像素门禁就靠这一点）。它同时是"binUI 后端能不能交给 bin2d"的
// 最直接证据 —— 同一棵节点树、同一份 HTML，两条路画出来的 PNG 应该一样。
//
// 用法: bin2d_shot <ui file> <out.png> [width] [height]
#include <windows.h>
#include <gdiplus.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"

#include "backends/binui_bin2d.h"
#include "backends/binui_draw_nodes.h"
#include "save_png.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: bin2d_shot <ui file> <out.png> [width] [height]\n");
        return 1;
    }

    binui::CompileResult res = binui::CompileBinUIFileEx(argv[1]);
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[bin2d_shot] warning: %s\n", w.c_str());
    if (res.layout.empty()) {
        std::fprintf(stderr, "bin2d_shot: 编译失败 %s\n", argv[1]);
        return 1;
    }

    binui::BinUIRenderer renderer;
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "bin2d_shot: 布局解析失败\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    int W = argc > 3 ? std::atoi(argv[3]) : (wd.present && wd.width ? (int)wd.width : 900);
    int H = argc > 4 ? std::atoi(argv[4]) : (wd.present && wd.height ? (int)wd.height : 840);
    if (W <= 0 || H <= 0) { W = 900; H = 840; }
    renderer.setSize(W, H);

    // 布局里的图片资源（后端按 imageIndex 变成 bin2d::Image）
    {
        const auto& paths = renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(binui::ex::DirOf(argv[1]), paths, std::string());
            if (!imgs.empty()) {
                renderer.setImageOrder(paths);
                renderer.loadResources(imgs);
            }
        }
    }

    binui::BinUI2D gfx;
    if (!gfx.init(W, H)) {
        std::fprintf(stderr, "bin2d_shot: 初始化失败（尺寸 %dx%d）\n", W, H);
        return 1;
    }

    gfx.beginFrame(nullptr);
    binui::drawNodesBinUI(gfx, renderer, nullptr, binui::CanvasFn{});
    gfx.present();

    // 出图走 GDI+ 编码器（与 gl_shot / vk_shot 同一份 SaveBgraPng），
    // 而**不是** bin2d::writePng：后者是零依赖的"能写就行"，PNG 用 stored 块不做压缩
    // （1400×620 的 radius-matrix 要 3.4 MB，18 个基准图会到 15 MB）。基准图要入库，
    // 编码器必须压缩。预乘 RGBA8 → 直通 BGRA 的换算在这里做一次。
    const int pw = gfx.width(), ph = gfx.height();
    const auto* src = static_cast<const uint8_t*>(gfx.pixels());
    std::vector<uint8_t> bgra(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4, 0);
    for (int y = 0; y < ph; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * pw * 4;
        uint8_t* d = bgra.data() + static_cast<size_t>(y) * pw * 4;
        for (int x = 0; x < pw; ++x, s += 4, d += 4) {
            const uint32_t a = s[3];
            if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            // 反预乘：GDI+ 的 32bppARGB 是直通 alpha
            auto un = [a](uint8_t v) {
                const uint32_t r = (static_cast<uint32_t>(v) * 255u + a / 2u) / a;
                return static_cast<uint8_t>(r > 255u ? 255u : r);
            };
            d[0] = un(s[2]);   // B
            d[1] = un(s[1]);   // G
            d[2] = un(s[0]);   // R
            d[3] = static_cast<uint8_t>(a);
        }
    }

    Gdiplus::GdiplusStartupInput gpin;
    ULONG_PTR gptoken = 0;
    Gdiplus::GdiplusStartup(&gptoken, &gpin, nullptr);
    const bool ok = binuishot::SaveBgraPng(argv[2], bgra.data(), pw, ph);
    Gdiplus::GdiplusShutdown(gptoken);

    int rc = 1;
    if (ok) {
        std::printf("saved %s (%dx%d)\n", argv[2], W, H);
        rc = 0;
    } else {
        std::fprintf(stderr, "bin2d_shot: 保存失败 %s\n", argv[2]);
    }
    gfx.shutdown();
    return rc;
}
