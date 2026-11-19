// binui_bin2d.h - bin2d 后端：把 IBinUIGfx 整份实现成 bin2d::Canvas 的调用
//
// 这一层存在的理由（master 2026-11-19 定的分层）：
//
//     binUI   ──►  bin2d / bin3d  ──►  binGPU  ──►  GL / Vulkan / D3D11
//
// 也就是说 **binUI 不再自己写图形 API**。以前 binUI 的 GL / Vulkan / D3D11 三份后端
// 各自把"圆角矩形 → 九宫格四边形"、"渐变 → 条带"、"文字 → 图集四边形"发射给驱动，
// 于是任何一次绘制语义的调整都要在三个地方同时改对（VK 的 strokeRounded 就漏过
// thickness，见 0.25.0）。这个后端把那条边界**下移一层**：所有几何都交给 bin2d 的
// 覆盖率光栅（CPU 像素权威），binUI 只负责"节点树 → 原语调用"。
//
// 为什么不直接接 binGPU：
//   binGPU 是设备/交换链/资源/管线的 C ABI，它**不知道**圆角、渐变、字形、九宫格
//   （它只认 quad 和 mesh）。把这些语义塞进 binGPU 等于把 bin2d 的活重做一遍。
//   正确的分工是 bin2d 拥有 2D 语义、binGPU 拥有设备 —— binUI 只碰 bin2d。
//
// 与三份 GPU 后端的关系（本版本）：
//   **不是替代，是新增的一条路**。0.26.0 只做离屏出图（bin2d_shot），用来证明
//   "同一棵节点树用 bin2d 画出来的像素对不对"；窗口呈现（把 Surface 交给
//   bin2d::GpuCompositor → binGPU 交换链）是下一步，那时才会动 BinUIAppT 外壳。
//
// 坐标与状态语义（这是唯一容易搞错的地方，写死在这里）：
//   · 无仿射时，binUI 的 (x,y,w,h) 就是屏幕坐标；bin2d 的 Canvas 初始变换是单位阵，
//     所以两边一致，不需要任何转换。
//   · 有仿射时（setAffine），binUI 的调用方改用**节点局部坐标**画，矩阵负责转到屏幕；
//     bin2d 的 Canvas 也是"局部坐标 + 当前变换"。两边再次一致 —— 直接 setTransform。
//   · pushClip 是求交语义，bin2d 的 clipRect 也是求交，且同样按当前变换映射包围盒。
//   · pushLayer 的 (x,y,w,h) 是局部坐标下的矩形，bin2d 的 pushLayer(localBounds) 同义。
//   · 颜色一律 0xAARRGGBB 直通进、bin2d 内部预乘（Color::argb 就是那个转换点）。
#ifndef BINUI_BIN2D_H
#define BINUI_BIN2D_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

#include "binui_gfx.h"
#ifdef _WIN32
// 与 BinUIGL / BinUIVK / BinUID3D 三个后端**同一份**外壳模板（窗口 / 输入 / IME /
// 拖边框 / DPI / 帧节流都在里面，后端只提供 8 个方法 + appClassName）。
// 不无条件包含：binui_bin2d.h 也被 bin2d_shot 这类**不需要窗口**的工具包含，
// 而 binui_app.h 会拖进 windows.h。非 Windows 平台没有这个外壳。
#include "backends/binui_app.h"
#endif

namespace binui {

// 用 bin2d 的覆盖率光栅实现 IBinUIGfx 的全部原语。
//
// 生命周期与 BinUIGL 对齐（init → beginFrame/draw/present → shutdown），
// 但**不需要窗口**：init(width, height) 就够了，像素留在内部的 bin2d::Surface 上，
// 由 savePng() / surface() 取走。
class BinUI2D : public IBinUIGfx {
public:
    BinUI2D();
    ~BinUI2D() override;
    BinUI2D(const BinUI2D&) = delete;
    BinUI2D& operator=(const BinUI2D&) = delete;

    // ---- 生命周期 ----
    // 离屏初始化：只分配 bin2d::Surface 与字形图集，不碰任何图形 API。
    // 失败返回 false（尺寸非法 / 字体服务不可用不是失败 —— 文字会降级成空白）。
    bool init(int width, int height);
    // 窗口初始化：多建一台 bin2d::GpuCompositor（窗口版），present() 走
    // bingpu 交换链。bingpu 一个可用后端都起不来时**不是失败** —— 退回
    // "CPU 出 Surface + GDI 贴屏"，窗口照样能开（无显卡的机器也能跑）。
    bool init(void* hwnd, int width, int height, bool vsync = true);
    void shutdown();

    bool isReady() const override;
    int  width() const override;
    int  height() const override;
    void resize(int width, int height) override;
    void setVsync(bool on) override;
    void setTransparent(bool t) override;

    // ---- 帧 ----
    // clipRect = int[4]{l,t,r,b}（nullptr = 全窗口）。只清这块矩形再画，
    // 语义与三个 GPU 后端的 beginFrame 一致。
    void beginFrame(const void* clipRect) override;
    // 窗口版：Surface → bin2d::GpuCompositor::presentSurface → bingpu 交换链。
    // 离屏版（init 没给窗口）：空操作，像素留在 surface 上。
    void present() override;
    // 外壳在"拖拽 resize"时会调这个（GL 那边是为了绕开 nvoglv64 的 present 崩溃）。
    // bin2d 后端没有交换链直翻转那条路，所以它就是 present。
    void presentViaGdi();
    // 窗口类名（BinUIAppT 要求）：与 GL/VK/D3D11 的外壳在同一进程里并存时不冲突
    static const wchar_t* appClassName();
    bool presentPacesFrames() const override;

    // ---- 帧节拍（v0.34.0）：全部转发给 bin2d::GpuCompositor → binGPU ----
    // 这一层**不许**自己算节拍（算过，算错过：见 binui_gfx.h 里那段注释）。
    bool paceAvailable() const override;
    double paceFrameMs() const override;
    long long paceWaitUs() override;
    double paceRefreshMs() const override;
    void setPacePolicyText(const char* text) override;
    const char* pacePolicyName() const override;

    // 实际用上的呈现路径（诊断 / 日志用）：
    //   "bin2d+gpu(gl)" / "bin2d+gpu(vulkan)" / "bin2d+gdi" / "bin2d+offscreen"
    const char* presentBackendName() const;

    // ---- 像素出口（bin2d 后端独有，用于出图与对拍）----
    // 预乘 RGBA8，尺寸 = width()×height()。有效期到下一次 beginFrame/shutdown。
    const void* pixels() const;
    size_t      pixelsSize() const;
    // 反预乘后写 PNG（复用 bin2d 的编码器，与 bin2d 的其他产物同一口径）
    bool savePng(const char* path) const;

    // ---- 原语 ----
    void fillRect(float x, float y, float w, float h, uint32_t color) override;
    void fillRounded(float x, float y, float w, float h, float radius,
                     uint32_t color) override;
    void fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                       uint32_t color, const float xf[6]) override;
    void fillLine(float x0, float y0, float x1, float y1, float width,
                  uint32_t color) override;
    bool fillGradientQuad(float x, float y, float w, float h, float radius,
                          float angleDeg, uint32_t from, uint32_t to) override;
    void strokeRounded(float x, float y, float w, float h, float radius,
                       float thickness, uint32_t color) override;

    // ---- 图片 ----
    bool prepareImage(uint16_t index, const uint8_t* data, size_t size,
                      uint32_t generation) override;
    bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                         uint32_t generation) override;
    void drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                     float x, float y, float w, float h, uint32_t tint) override;
    void drawImageNine(uint16_t index, float x, float y, float w, float h,
                       float slice, int repX, int repY, uint32_t tint) override;

    // ---- 文本（排版与光栅复用 binui_glyph_atlas.h，与 GL/VK/D3D11 同一份）----
    void drawText(float x, float y, float size, int weight,
                  const std::string& utf8, uint32_t color,
                  bool centerH = false, bool centerV = false,
                  bool italic = false) override;
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
    void setFontName(const char* name) override;

    // ---- 状态 ----
    bool setAffine(const float xf[6]) override;
    void setImageFilter(const BinUIImageFilter* f) override;
    bool setEraseBlend(bool on) override;
    bool setKeepBlend(bool on) override;
    bool supportsMask() const override { return true; }
    bool backdropBlur(float x, float y, float w, float h, float radius,
                      float blurPx, float grain) override;

    // ---- 裁剪栈 / 图层 ----
    int  pushClip(float x, float y, float w, float h) override;
    void popClipTo(int depth) override;
    bool pushLayer(float x, float y, float w, float h, float alpha) override;
    bool pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) override;
    void popLayer() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _WIN32
// 无边框窗口外壳 + bin2d 后端（与 BinUIAppGL / BinUIAppVK / BinUIAppD3D 同一份模板）。
// 有没有这一行，决定了"三份 GPU 后端能不能删"：外壳只用到后端的 8 个方法，
// BinUI2D 把它们全实现了，那么输入 / IME / 拖边框就不再是 GPU 后端的独占能力。
using BinUIApp2D = BinUIAppT<BinUI2D>;
#endif

} // namespace binui

#endif // BINUI_BIN2D_H
