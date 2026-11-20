// binui_ui.h - 示例/工具的统一入口：**只有一条绘制路径**
//
//     binUI  ──►  bin2d / bin3d  ──►  binGPU  ──►  GL / Vulkan / D3D11
//
// 这段分层终于在 0.37.x 收尾（docs/backend_layering.md 第 3c 步）：binUI 这一层
// **不再有任何图形 API**、也不再有自己的 2D 光栅。binUI 只负责"节点树 → 原语调用"，
// 2D 语义在 bin2d，设备/交换链在 binGPU。
//
// **已删除**（2026-11-20，三份老 GPU 后端 + 设备层那份 + 运行期选择器，共 ~12k 行）：
//   src/backends/binui_gl.{h,cpp}      —— OpenGL 3.3 后端（含 WGL、自己的 GLSL）
//   src/backends/binui_vk.{h,cpp}      —— Vulkan 后端（含 SPIR-V 常量表 binui_vk_shaders.h）
//   src/backends/binui_d3d11.{h,cpp}   —— D3D11 后端（含自己的 HLSL）
//   src/backends/binui_gpu.{h,cpp}     —— 设备层直接建在 binGPU 上的那份（binUI 终点实现的前身）
//   src/backends/binui_auto.{h,cpp}    —— 运行期按 VK > GL > D3D11 > CPU 挑后端的选择器
// 于是 `-DBINUI_GPU_GL|VK|D3D11|2D` 这四个编译期宏、以及 `BINUI_GPU=vk|gl|d3d11|gpu|2d`
// 这个环境变量一起退场 —— 没有可挑的了，只剩 bin2d 这一条。
//
// 它们的历史实现记录仍在 docs/vulkan_design.md / docs/p3b-plan.md（已标注"已删除"），
// 但源码不再保留。
//
// 历史：0.29.0 起默认后端就是 bin2d；0.33.0 一度改成运行期选择（BinUIAuto）；0.37.x
// 把另外四份删干净，UiApp / UiBackend 直接固定成 bin2d 的那一份。
#ifndef BINUI_UI_H
#define BINUI_UI_H

#include <functional>

#include "backends/binui_draw_nodes.h"   // drawNodesBinUI（后端无关遍历）
#include "backends/binui_bin2d.h"        // BinUI2D / BinUIApp2D

namespace binui {

// 编制期只剩一个后端，`UiBackend` / `UiApp` 这两个名字留着是为了不动 9 个示例的源码
// 与 tools/draw_bench.cpp 的调用点。
using UiBackend = BinUI2D;
#ifdef _WIN32
// 无边框窗口外壳（BinUIAppT<Gfx>）只在 Windows 上有；非 Windows 平台只用 BinUI2D 出图。
using UiApp     = BinUIApp2D;
#endif

// <canvas> 绘制回调：参数是**接口**（IBinUIGfx），不是具体后端 —— 这样 demo 的绘制代码
// 与"谁在执行"完全解耦（这条契约就是三份老后端能删掉的原因之一）。
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

// 当前后端名（日志/诊断用）。以前这里按宏返回 "vulkan"/"d3d11"/"gl"/"auto"，
// 现在只有 bin2d；**实际生效的那一级**由 BinUI2D::presentBackendName() 报
// （"bin2d+gpu" / "bin2d+gpu-compositor" / "bin2d+gdi" / "bin2d+offscreen"）。
inline const char* UiBackendName() {
    return "bin2d";
}

// 点击定位输入框光标：placeCaretAtClick(renderer, gfx, x, y) —— 模板在 binui_app.h 里，
// 各后端共用同一份换算（基于字形 advance）。

} // namespace binui

#endif // BINUI_UI_H
