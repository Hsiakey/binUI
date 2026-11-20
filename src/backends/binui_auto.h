// binui_auto.h - 运行期后端选择（0.33.0）
//
// master 2026-11-20 那条：「用不用 GPU 应该是 binGPU 决定的吧，有 GPU；
// GPU > CPU（实在没有 GPU 才用这个，按理来说即便是集成显卡性能也比纯 CPU 算要好非常多）；
// VK > GL > D3D」。
//
// 这个类就是把那条落下来的一层**薄壳**：它实现同一份 `IBinUIGfx`，内部持有一个
// {BinUIVK, BinUIGL, BinUID3D, BinUI2D} 之一，`init()` 时按上面的优先级问 binGPU
// 「这台机器有什么」再决定。外壳 `BinUIAppT<Gfx>` 一行都不用改 —— 因为契约是接口。
//
// 为什么不做成"binUI 后端改成非模板 + 工厂"：那是更大的外科手术（外壳 2000 行、
// 演示程序 27 个方法、四份 .o 都要进宫）。薄壳同样达到目的，且**像素语义由内层后端
// 决定**，不引入第二套绘制路径。
//
// 强制的办法（金标准/对照构建仍然要能钉住后端）：
//   · 环境变量 `BINUI_GPU=vk|gl|d3d11|2d`（2d = 强制 CPU 光栅）
//   · 编译期宏 `-DBINUI_GPU_VK|GL|D3D11|2D`（binui_ui.h 里那份，行为不变）
#ifndef BINUI_AUTO_H
#define BINUI_AUTO_H

#include <memory>
#include <string>

#include "binui_gfx.h"
#include "backends/binui_app.h"   // 无边框窗口外壳模板（四个后端共用同一份）

// binGPU 的节拍器结构体（0.9.0）：这里只前置声明它的**标签**，全局命名空间 ——
// binUI 的公共头不该把 bingpu.h 带进来（纯 C++ 核心库要能在没有 binGPU 的构建里编过）。
// 完整定义只在 binui_auto.cpp 里落地（那里 include 了 bingpu.h）。
struct BgpuPacer;

namespace binui {

// 后端种类。Auto = 让 binGPU 决定。
//
// 0.36.0 新增 `BinGpu`：binUI 把设备层整个交给 binGPU（`BinUIGpu`，见 binui_gpu.h）。
// 它和 Vulkan/GL/D3D11 那三个**不是一回事**：那三个各自建自己的设备，BinGpu 只是
// binGPU 的薄壳 —— 圆角 AA / 混合 / 交换链 / 节拍全部在 binGPU 里。等 binGPU 补上
// MSAA 与 mip 采样之后（docs/backend_layering.md 第 3 步收尾），它就是唯一的后端。
enum class GpuBackend { Auto, Vulkan, GL, D3D11, Cpu, BinGpu };

// 把 Auto 解析成具体后端：先看 `BINUI_GPU`，再按 VK → GL → D3D11 问 binGPU，
// 一个都没有才退 CPU。四次都失败返回 Cpu（CPU 永远可用）。
GpuBackend ResolveGpuBackend();
const char* GpuBackendName(GpuBackend b);

// 薄壳。除 `IBinUIGfx` 之外，还提供外壳用到的 init/shutdown/presentViaGdi。
class BinUIAuto final : public IBinUIGfx {
public:
    BinUIAuto();
    ~BinUIAuto() override;

    // 外壳只要一个窗口类名：所有后端都用同一个（切换后端不影响窗口创建）。
    static const wchar_t* appClassName() { return L"BinUIAppAutoWindow"; }

    bool init(int width, int height);                              // 离屏（截图/金标准）
    bool init(void* hwnd, int width, int height, bool vsync = true);  // 窗口
    void shutdown();
    void presentViaGdi();

    // 实际选中的后端（诊断用；"bin2d" = 这台机器没有可用 GPU，走 CPU 光栅）
    GpuBackend activeBackend() const { return active_; }
    const IBinUIGfx* active() const { return inner_.get(); }

    // ---- IBinUIGfx ----
    bool isReady() const override;
    int  width() const override;
    int  height() const override;
    void resize(int width, int height) override;
    void setVsync(bool on) override;
    void setTransparent(bool t) override;
    void beginFrame(const void* clipRect) override;
    void present() override;
    bool presentPacesFrames() const override;

    // ---- 帧节拍（v0.34.0）----
    // bin2d 那条路自己就有节拍器（bin2d::GpuCompositor → binGPU），这里直通；
    // 仓里遗留的 gl/vk/d3d11 三份实现没有，于是**由这层壳持有一个 binGPU 节拍器**替它们算
    // —— 算法仍然只有 binGPU 那一份，这里只是把 present 的真实耗时喂进去。
    // （那三份实现按 docs/backend_layering.md 第 3 步要删，删完这段跟着没了。）
    bool paceAvailable() const override;
    double paceFrameMs() const override;
    long long paceWaitUs() override;
    double paceRefreshMs() const override;
    void setPacePolicyText(const char* text) override;
    const char* pacePolicyName() const override;

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
    std::string ellipsize(const std::string& utf8, float maxW, float size,
                          int weight, bool italic) override;
    bool setAffine(const float xf[6]) override;
    void setImageFilter(const BinUIImageFilter* f) override;
    bool setEraseBlend(bool on) override;
    bool setKeepBlend(bool on) override;
    bool supportsMask() const override;
    bool backdropBlur(float x, float y, float w, float h, float radius,
                      float blurPx, float grain) override;
    void setTextShadows(const BinUITextShadow* layers, int count) override;
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

private:
    bool open(GpuBackend which, void* hwnd, int width, int height, bool vsync);
    // 遗留三后端（gl/vk/d3d11）用的节拍器：内层自己不管节拍时才启用
    bool legacyPaceActive() const { return legacyPacer_ && inner_ && !inner_->paceAvailable(); }
    std::unique_ptr<IBinUIGfx> inner_;
    std::unique_ptr<BgpuPacer> legacyPacer_;
    char paceText_[32] = {0};
    GpuBackend active_ = GpuBackend::Cpu;
    bool offscreen_ = false;
};

} // namespace binui

#endif // BINUI_AUTO_H
