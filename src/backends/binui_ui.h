// binui_ui.h - 示例/工具的统一入口：编译期选后端（BINUI_GPU_VK / BINUI_GPU_D3D11）
//
// 一份 demo 源码要能编出多个后端版本（`make demos-vk` → examples/*_vk.exe，
// `make demos-d3d11` → examples/*_d3d11.exe），所以 demo 里不再直接写后端类名，
// 而是用这里的 UiBackend / UiApp / drawUi。默认 GL。
//
// 三个后端的契约是同一份 IBinUIGfx，所以 demo 的绘制代码（含 <canvas> 回调）不用改。
#if !defined(BINUI_UI_H)
#define BINUI_UI_H

#include <functional>

#include "backends/binui_draw_nodes.h"   // drawNodesBinUI（后端无关遍历）

#if defined(BINUI_GPU_VK)
#include "backends/binui_vk.h"
#elif defined(BINUI_GPU_D3D11)
#include "backends/binui_d3d11.h"
#else
#include "backends/binui_gl-gdi.h"
#endif

namespace binui {

#if defined(BINUI_GPU_VK)
using UiBackend = BinUIVK;
using UiApp     = BinUIAppT<BinUIVK>;
#elif defined(BINUI_GPU_D3D11)
using UiBackend = BinUID3D;
using UiApp     = BinUIAppT<BinUID3D>;
#else
using UiBackend = BinUIGLGdi;
using UiApp     = BinUIAppT<BinUIGLGdi>;
#endif

// <canvas> 绘制回调：参数是**接口**（IBinUIGfx），不是具体后端 —— 这样 demo 的绘制代码
// 在三个后端之间完全通用，也满足"将来做运行期后端选择"的前置条件。
using UiCanvasDrawFn = std::function<void(IBinUIGfx& gfx, uint16_t nodeId,
                                          float x, float y, float w, float h)>;

// 画一帧节点树（clip = 脏矩形，nullptr = 全屏）
inline void drawUi(IBinUIGfx& gfx, const BinUIRenderer& renderer, const void* clipRect = nullptr,
                   const UiCanvasDrawFn& canvas = {}) {
    CanvasFn shared;
    if (canvas) {
        shared = [&canvas](IBinUIGfx& g, uint16_t id, float x, float y, float w, float h) {
            canvas(g, id, x, y, w, h);
        };
    }
    drawNodesBinUI(gfx, renderer, clipRect, shared);
}

// ---------------------------------------------------------------------------
// 运行期后端选择（BINUI_GPU=vk|gl|d3d11）—— **预留接口，尚未实现**，见
// docs/vulkan_design.md §7.5。现状是编译期选：本文件顶部的 BINUI_GPU_VK /
// BINUI_GPU_D3D11 决定 UiBackend / UiApp，一份源码编出三个 exe。
//
// 要接的位置就三处（现在已把前两处清理干净了）：
//   1. 调用方已不依赖具体类型：setDrawFn / canvas 回调都是 IBinUIGfx&；
//   2. 遍历、图集、外壳都吃接口或模板（drawNodesBinUI / GlyphAtlas / BinUIAppT<Gfx>）；
//   3. 缺的是 —— 一个 `IBinUIApp` 抽象（run/pump/退出/setXXX）+ 工厂
//      `std::unique_ptr<IBinUIApp> CreateUiApp(BackendId)`，工厂里按
//      getenv("BINUI_GPU") 选中某个 BinUIAppT<BinUIVK|BinUIGLGDgi|BinUID3D> 实例。
//      再做这一步时，三个后端的 .o 要同时链进同一个 exe（现在是每个 exe 只链一个）。
// ---------------------------------------------------------------------------
inline const char* UiBackendName() {
#if defined(BINUI_GPU_VK)
    return "vulkan";
#elif defined(BINUI_GPU_D3D11)
    return "d3d11";
#else
    return "gl";
#endif
}

// 点击定位输入框光标：placeCaretAtClick(renderer, gfx, x, y) —— 模板在 binui_app.h 里，
// 各后端共用同一份换算（基于字形 advance）。

} // namespace binui

#endif // BINUI_UI_H
