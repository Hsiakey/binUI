// partial_shot.cpp - 局部重绘一致性检查（hover 脏矩形 vs 整帧）
//
// 为什么需要它：binUI 的外壳在"只有某个节点变了"时只重画那个节点的脏矩形
// （renderer.invalidRect → InvalidateRect → beginFrame(clip)）。于是**同一帧画面
// 会由两条路径产生**：整帧重绘、以及"在一个脏矩形里重绘"。两条路径必须逐像素一致，
// 否则脏矩形边界上会留下硬边的"影子 / 边框"—— 而这类错在整帧像素基准里
// 永远看不到（clip = 整屏时判据的边界情况碰不到任何节点）。
//
// 做法（不需要人肉 hover，**也不需要窗口/GPU**）：
//   1. 同一个 renderer 状态（hover + 推进完过渡）下，从**干净的 Surface** 整帧画一遍 → 参照；
//   2. 再从另一个干净的 Surface 整帧画一遍，然后按 invalidRect 只重画那块 → 待测；
//   3. 逐像素比参照 / 待测，并把差异分成**落在 clip 内 / 落在 clip 外**两堆。
//
// 为什么要两个实例：参照帧必须是"从零画一遍"，而不是"在上一帧结果上再画一遍"。
// 实测这两者对当前页面等价（整帧重绘是幂等的，见 build-mingw/tmp/probe_partial.cpp），
// 但依赖这个巧合没必要 —— GL 版的参照帧本来就是被清过屏的，这里保持同一口径。
//
// **落在 clip 外的差异是真信号**：它意味着这一帧画到了自己的脏矩形之外（典型的是阴影 /
// 模糊图层只按元素盒裁剪，没跟 canvas 的 clipRect 求交）。0.37.x 移植后在 buttonshow 上
// 实测 1825 个差异像素里有 1786 个在 clip 外 —— 这条是 bin2d 后端的欠账，工具留着当判据。
//
// 阈值：bin2d 是纯 CPU 光栅、同状态同输入必须逐位相同，所以是 **0 容差**
// （GL 时代给 MSAA 解析留的"极少数像素差 1"豁免随 GL 后端一起退场）。
//
// 用法:
//   partial_shot <ui> <out.png> <x> <y> [frames] [W] [H]     出图（保存局部重绘结果）
//   partial_shot --check <ui> <x> <y> [frames] [W] [H]        对拍（非 0 退出 = 不一致）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>   // SetProcessDPIAware（不建窗口，只为了尺寸口径一致）

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "binui_paths.h"
#include "backends/binui_bin2d.h"
#include "backends/binui_draw_nodes.h"

namespace {

// 预乘 RGBA8（BinUI2D::pixels() 的格式）。拷贝一份，因为 Surface 下一次
// beginFrame/shutdown 就失效。
std::vector<uint8_t> Snapshot(const binui::BinUI2D& gfx) {
    const auto* p = static_cast<const uint8_t*>(gfx.pixels());
    const size_t n = gfx.pixelsSize();
    if (!p || n == 0) return {};
    return std::vector<uint8_t>(p, p + n);
}

void DrawFull(binui::BinUI2D& gfx, binui::BinUIRenderer& renderer) {
    gfx.beginFrame(nullptr);
    binui::drawNodesBinUI(gfx, renderer, nullptr, binui::CanvasFn{});
}

std::unique_ptr<binui::BinUI2D> MakeGfx(int w, int h) {
    auto g = std::make_unique<binui::BinUI2D>();
    if (!g->init(w, h)) return nullptr;
    return g;
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

    // ---- hover + 推进过渡：**只在这时推进一次**，两条路共用同一份渲染器状态 ----
    renderer.setHover(hoverX, hoverY);
    for (int i = 0; i < frames; ++i) { renderer.stepAnimations(); Sleep(8); }

    // ---- 脏矩形：与外壳 invalidateNode 同口径 ----
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
    std::printf("[partial] hovered=%d clip=(%d,%d)-(%d,%d)\n", id, clip[0], clip[1], clip[2], clip[3]);

    // ---- 待测：整帧一遍 + 脏矩形一遍（同一张 Surface，模拟窗口里的第二帧）----
    std::unique_ptr<binui::BinUI2D> partialGfx = MakeGfx(W, H);
    if (!partialGfx) { std::fprintf(stderr, "partial_shot: BinUI2D 初始化失败（%dx%d）\n", W, H); return 2; }
    DrawFull(*partialGfx, renderer);
    partialGfx->beginFrame(clip);
    binui::drawNodesBinUI(*partialGfx, renderer, clip, binui::CanvasFn{});
    const std::vector<uint8_t> partial = Snapshot(*partialGfx);

    int rc = 0;
    if (check) {
        // ---- 参照：**干净的 Surface** 整帧一遍（两个实例，见文件头注释）----
        std::unique_ptr<binui::BinUI2D> refGfx = MakeGfx(W, H);
        if (!refGfx) { std::fprintf(stderr, "partial_shot: 参照实例初始化失败\n"); return 2; }
        DrawFull(*refGfx, renderer);
        const std::vector<uint8_t> ref = Snapshot(*refGfx);
        refGfx->shutdown();

        if (ref.size() != partial.size() || ref.empty()) {
            std::fprintf(stderr, "[partial] 像素缓冲尺寸不一致（%zu vs %zu）\n",
                         partial.size(), ref.size());
            partialGfx->shutdown();
            return 2;
        }

        // 预乘 RGBA：四通道全比（纯 CPU 光栅，没有 MSAA 解析顺序这种免罪符）
        size_t diffPx = 0, outsideClip = 0;
        int maxDiff = 0;
        int firstX = -1, firstY = -1;
        for (int y = 0; y < H; ++y) {
            const uint8_t* a = ref.data() + static_cast<size_t>(y) * W * 4;
            const uint8_t* b = partial.data() + static_cast<size_t>(y) * W * 4;
            for (int x = 0; x < W; ++x, a += 4, b += 4) {
                const int d = std::max(std::max(std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])),
                                                std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]))),
                                       std::max(std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2])),
                                                std::abs(static_cast<int>(a[3]) - static_cast<int>(b[3]))));
                if (d > maxDiff) maxDiff = d;
                if (d > 0) {
                    ++diffPx;
                    if (x < clip[0] || x >= clip[2] || y < clip[1] || y >= clip[3]) ++outsideClip;
                    if (firstX < 0) { firstX = x; firstY = y; }
                }
            }
        }
        const bool ok = (diffPx == 0 && maxDiff == 0);
        std::printf("[partial] 局部重绘 vs 整帧: diff=%zu (clip 内 %zu / clip 外 %zu) max=%d%s\n",
                    diffPx, diffPx - outsideClip, outsideClip, maxDiff,
                    firstX >= 0 ? "" : " (pixel-identical)");
        if (firstX >= 0) std::printf("[partial] 首个差异像素 (%d,%d)\n", firstX, firstY);
        if (outsideClip > 0) {
            std::printf("[partial] 注意：%zu 个差异像素落在脏矩形**外面** —— "
                        "这一帧画到了自己的 clip 之外\n", outsideClip);
        }
        if (!ok) {
            std::printf("[partial] FAIL: 局部重绘与整帧不一致（阈值 diff==0）\n");
            partialGfx->savePng("build-mingw/tmp/partial_diff_partial.png");
            rc = 1;
        } else {
            std::printf("[partial] OK\n");
        }
    } else if (std::strcmp(outPath, "-") != 0) {
        if (!partialGfx->savePng(outPath)) { std::fprintf(stderr, "[partial] 保存失败\n"); rc = 2; }
        else std::printf("saved %s (%dx%d)\n", outPath, W, H);
    }

    partialGfx->shutdown();
    return rc;
}
