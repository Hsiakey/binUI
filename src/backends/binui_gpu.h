// binui_gpu.h - binUI 的设备层：直接建在 **binGPU** 上（0.36.0）
//
// 为什么要有它（docs/backend_layering.md 第 3 步的第一步）：
//
//   binUI 原来有三份自建的 GPU 设备层（binui_gl / binui_vk / binui_d3d11），每份都自己
//   `wglCreateContext` / `vkCreateDevice` / `D3D11CreateDevice`，自己管交换链、自己的
//   着色器。结果是"怎么绘制"这件事被抄了三遍：圆角 AA 的解析式在 binUI 里躺了 3 份，
//   在 binGPU 里另有 4 份 —— 一条数学公式全栈 7 份拷贝，改一处漏六处。
//
//   master 的口径是 **binGPU 决定怎么绘制、用什么绘制**，binUI 只决定功能。所以这个
//   后端不建自己的设备：它把 binGPU 当一个设备用（`bingpu_device_create`），
//   几何全部走 `bingpu_cmd_*`，圆角走 `bingpu_cmd_draw_shapes`（SDF 在 binGPU 的片元
//   着色器里），混合模式走 binGPU 的 `BgpuBlend`，节拍走 binGPU 的 `BgpuPacer`。
//   于是 binUI 这一侧**一份 AA 数学都不留**。
//
// 和另三份后端的分工差异（有意为之）：
//   · 三者各自实现 IBinUIGfx 的全部原语；这里同样实现，但**原语的落点**是命令缓冲。
//   · 三者的字体图集/图片解码/条带几何本来就共用同一份头（binui_glyph_atlas.h /
//     binui_win_image.h / binui_paint.h），这里继续共用，一行都没重写。
//   · 三者自己有窗口/交换链/节拍；这里全部问 binGPU（`bingpu_swapchain_*`、
//     `bingpu_frame_*`、`bingpu_pace_*`）。
//
// 目标（一张自有的、窗口大小的离屏画布 canvas）：
//   所有 UI 画进 canvas（支持脏区局部重绘，语义与 binui_gl 的 offTex 一致），present
//   时把 canvas 当一个纹理四边形贴到 binGPU 的交换链目标上。这样"抓背景做毛玻璃"
//   "图层合成"全都退化成"把一个纹理画成四边形"，不需要任何 copy/blit 命令。
//
// 已知的两处差异（0.36.0，先记账）：
//   1. binGPU 目前不提供 MSAA（四端 msaa_samples 恒为 1，这是它 0 差验收的前提）。
//      binui_gl 原来用 4× MSAA 软化"非轴对齐几何"（旋转卡片、斜线）的边缘；
//      换到 binGPU 之后这些边按 1 样本光栅化。解析圆角**不受影响**（SDF 自带 AA）。
//   2. binGPU 的 mip 链目前降级为 1 级（`BGPU_TEXTURE_MIPMAPS` 被接受但没有真正的
//      mip 采样），缩小的图片会退化成单级双线性。
//   两条都是 binGPU 该补的能力，不该在这一层补回来。
#ifndef BINUI_GPU_H
#define BINUI_GPU_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "binui_renderer.h"
#include "binui_gfx.h"            // IBinUIGfx：后端契约
#include "backends/binui_app.h"   // 无边框窗口外壳模板（与另三个后端共用同一份）

namespace binui {

class BinUIGpu : public IBinUIGfx {
public:
    BinUIGpu();
    ~BinUIGpu() override;
    BinUIGpu(const BinUIGpu&) = delete;
    BinUIGpu& operator=(const BinUIGpu&) = delete;

    // 后端种类由 binGPU 决定（`bingpu_render_plan`），不再由 binUI 自己问 vk/gl/d3d。
    // 想钉住某个后端做对照：环境变量 `BINUI_GPUBACKEND=gl|vulkan|d3d11|cpu`。
    bool init(void* hwnd, int width, int height, bool vsync = true);
    void shutdown();
    bool isReady() const;
    int width() const;
    int height() const;
    void setVsync(bool on);
    void setTransparent(bool t);
    void resize(int width, int height);

    void beginFrame(const void* clipRect);
    void present();
    void presentViaGdi();
    void flushFrame();
    bool presentPacesFrames() const;

    // ---- 帧节拍（binGPU 的 BgpuPacer：闸门在设备层，这里只是转一手）----
    bool paceAvailable() const override;
    double paceFrameMs() const override;
    long long paceWaitUs() override;
    double paceRefreshMs() const override;
    void setPacePolicyText(const char* text) override;
    const char* pacePolicyName() const override;

    // ---- 原语 ----
    void fillRect(float x, float y, float w, float h, uint32_t color) override;
    void fillRounded(float x, float y, float w, float h, float radius,
                     uint32_t color) override;
    void fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                       uint32_t color, const float xf[6]) override;
    void fillLine(float x0, float y0, float x1, float y1, float width,
                  uint32_t color) override;
    void strokeRounded(float x, float y, float w, float h, float radius,
                       float thickness, uint32_t color) override;
    bool fillGradientQuad(float x, float y, float w, float h, float radius,
                          float angleDeg, uint32_t from, uint32_t to) override;

    void drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                     float x, float y, float w, float h, uint32_t tint) override;
    void drawImageNine(uint16_t index, float x, float y, float w, float h,
                       float slice, int repX, int repY, uint32_t tint) override;

    void drawText(float x, float y, float size, int weight,
                  const std::string& utf8, uint32_t color,
                  bool centerH = false, bool centerV = false, bool italic = false) override;
    void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                         const std::string& utf8, uint32_t color, int align = 0,
                         bool italic = false) override;
    float textWidth(float size, int weight, const std::string& utf8,
                    bool italic = false) override;
    void fontMetrics(float size, int weight, int& ascent, int& lineHeight,
                     bool italic = false) override;
    void setTextSubpixel(bool on) override;
    void setLetterSpacing(float px) override;
    void setTextShadows(const BinUITextShadow* layers, int count) override;
    bool setAffine(const float xf[6]) override;
    void setImageFilter(const BinUIImageFilter* f) override;

    bool setEraseBlend(bool on) override;
    bool setKeepBlend(bool on) override;
    bool supportsMask() const override { return true; }

    bool backdropBlur(float x, float y, float w, float h, float radius,
                      float blurPx, float grain) override;
    bool pushLayer(float x, float y, float w, float h, float alpha) override;
    bool pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) override;
    void popLayer() override;

    bool prepareImage(uint16_t index, const uint8_t* data, size_t size,
                      uint32_t generation) override;
    bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                         uint32_t generation) override;

    int  pushClip(float x, float y, float w, float h) override;
    void popClipTo(int depth) override;
    void setFontName(const char* name) override;

    // 诊断：实际用的 binGPU 后端名（"gl" / "vulkan" / "d3d11" / "cpu"）
    const char* backendName() const;

    static const wchar_t* appClassName() { return L"BinUIAppGpuWindow"; }

private:
    struct Impl;
    Impl* impl_;
};

// 把 BinUIRenderer 的节点树画到一个 IBinUIGfx 上（与 drawNodesGL 同一份遍历）。
using GpuCanvasDrawFn = std::function<void(BinUIGpu& gfx, uint16_t nodeId,
                                           float x, float y, float w, float h)>;
void drawNodesGpu(BinUIGpu& gfx, const BinUIRenderer& renderer, const void* clipRect,
                  const GpuCanvasDrawFn& canvas = {});

// 窗口 + 事件循环外壳（与另三个后端共用 binui_app.h 那一份实现）
using BinUIAppGpu = BinUIAppT<BinUIGpu>;

} // namespace binui

#endif // BINUI_GPU_H
