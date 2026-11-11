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

// <canvas> 绘制回调：拿到的是**具体后端**（demo 里调 fillRect/drawText 等原语）
using UiCanvasDrawFn = std::function<void(UiBackend& gfx, uint16_t nodeId,
                                          float x, float y, float w, float h)>;

// 画一帧节点树（clip = 脏矩形，nullptr = 全屏）
// 三个后端都走共享遍历 drawNodesBinUI —— 连"具体后端"都不用转回去：
// 共享遍历吃 IBinUIGfx&，而 IBinUIGfx 的 <canvas> 回调正好也是接口类型。
inline void drawUi(UiBackend& gfx, const BinUIRenderer& renderer, const void* clipRect = nullptr,
                   const UiCanvasDrawFn& canvas = {}) {
    CanvasFn shared;
    if (canvas) {
        shared = [&canvas](IBinUIGfx& g, uint16_t id, float x, float y, float w, float h) {
            canvas(static_cast<UiBackend&>(g), id, x, y, w, h);
        };
    }
    drawNodesBinUI(gfx, renderer, clipRect, shared);
}

// 点击定位输入框光标：placeCaretAtClick(renderer, gfx, x, y) —— 模板在 binui_app.h 里，
// 各后端共用同一份换算（基于字形 advance）。

} // namespace binui

#endif // BINUI_UI_H
