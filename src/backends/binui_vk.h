// binui_vk.h - Vulkan 绘制后端（v19，P4：离屏 + 窗口 + 鲁棒性）
//
// 定位与 GL/D3D11 后端同构：实现 IBinUIGfx，节点树遍历是共享的
//（src/backends/binui_draw_nodes.h），本文件只管"把四边形画到 Vulkan 上"。
//
// 运行期依赖：**只** LoadLibrary("vulkan-1.dll") + vkGetInstanceProcAddr，
// 不链接 vulkan-1.lib、不需要 Vulkan SDK（头文件在 third_party/vulkan）。
//
// 当前范围（见 docs/vulkan_design.md §4、§5.9、§5.10）：全部 7 个原语（矩形/圆角/线段/
// 描边/图片/文本/换行文本）+ 裁剪栈 + 4x MSAA 累积帧 + 读回 + 窗口交换链呈现。
// 文本与图片走与 GL 共用的一份实现（src/backends/binui_glyph_atlas.h、
// binui_win_image.h），所以在像素回归里两端逐位相同。
#ifndef BINUI_VK_H
#define BINUI_VK_H

#include <cstdint>
#include <string>

#include "backends/binui_app.h"   // 无边框窗口外壳模板（与 GL 共用同一份）
#include "binui_gfx.h"

namespace binui {

class BinUIVK : public IBinUIGfx {
public:
    BinUIVK();
    ~BinUIVK() override;

    // hwnd == nullptr → 离屏模式（vk_shot 用）；hwnd != nullptr → 窗口 + 交换链。
    // 交换链创建失败时自动降级为"读回 + GDI 贴屏"，不会整个初始化失败。
    bool init(void* hwnd, int width, int height, bool vsync = true);
    void shutdown();

    // ---- IBinUIGfx ----
    bool isReady() const override;
    int  width() const override;
    int  height() const override;
    void resize(int width, int height) override;
    void setVsync(bool on) override;
    void setTransparent(bool t) override;
    void beginFrame(const void* clipRect) override;
    void present() override;
    // 走交换链 + FIFO（vsync）时，vkAcquireNextImageKHR 会把调用线程阻塞到下一个垂直同步
    // → 让 present 当节拍器，外壳不再自己掐帧（帧相位天然对齐刷新，消除"打拍子"微抖）。
    bool presentPacesFrames() const override;

    void fillRect(float x, float y, float w, float h, uint32_t color) override;
    void fillRounded(float x, float y, float w, float h, float radius,
                     uint32_t color) override;
    // 仿射圆角矩形（旋转 / 非等比缩放）：xf = [a c e; b d f]，见 binui_gfx.h
    void fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                       uint32_t color, const float xf[6]) override;
    void fillLine(float x0, float y0, float x1, float y1, float width,
                  uint32_t color) override;
    void strokeRounded(float x, float y, float w, float h, float radius,
                       float thickness, uint32_t color) override;
    // 任意角度线性渐变（v30）：四角带色四边形 + 圆角模板，1~9 个四边形代替上千条带。
    bool fillGradientQuad(float x, float y, float w, float h, float radius,
                          float angleDeg, uint32_t from, uint32_t to) override;
    void drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                     float x, float y, float w, float h, uint32_t tint) override;
    void drawImageNine(uint16_t index, float x, float y, float w, float h,
                       float slice, uint32_t tint) override;

    void drawText(float x, float y, float size, int weight, const std::string& utf8,
                  uint32_t color, bool centerH = false, bool centerV = false,
                  bool italic = false) override;
    void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                         const std::string& utf8, uint32_t color, int align = 0,
                         bool italic = false) override;
    float textWidth(float size, int weight, const std::string& utf8,
                    bool italic = false) override;
    void fontMetrics(float size, int weight, int& ascent, int& lineHeight,
                     bool italic = false) override;
    void setTextSubpixel(bool on) override;
    void setLetterSpacing(float px) override;   // v0.16.40
    void setTextShadow(float dx, float dy, float blur, uint32_t color) override;
    bool setAffine(const float xf[6]) override;
    void setImageFilter(const BinUIImageFilter* f) override;   // v26：CSS filter 颜色矩阵
    bool setEraseBlend(bool on) override;                       // v27：clip-path 擦除
    bool setKeepBlend(bool on) override;                        // v28：mask 保留趟
    bool supportsMask() const override { return true; }         // v28：mask 可用
    // v29 毛玻璃（CSS backdrop-filter: blur）：抓当前目标的区域 → 多级 2× 降采样 →
    // 圆角条带 + replace 混合画回去。算法与 GL 后端逐字一致（见 binui_gfx.h 的接口注释）。
    // 需要 Vulkan 1.3 动态渲染（要在帧内结束/重开渲染作用域）；拿不到就返回 false。
    bool backdropBlur(float x, float y, float w, float h, float radius,
                      float blurPx, float grain) override;

    bool prepareImage(uint16_t index, const uint8_t* data, size_t size,
                      uint32_t generation) override;
    bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                         uint32_t generation) override;

    int  pushClip(float x, float y, float w, float h) override;
    void popClipTo(int depth) override;

    // 组透明度（v0.16.12）：把"节点 + 子树"画进窗口大小的离屏层，再整组乘一次 alpha 合成。
    // 需要 Vulkan 1.3 动态渲染；拿不到就返回 false，调用方退回逐元素乘 alpha。
    bool pushLayer(float x, float y, float w, float h, float alpha) override;
    // v0.16.42：filter: blur(Npx) 的模糊层（pop 时先糊掉层内容再按 alpha 合成）
    bool pushLayerBlurred(float x, float y, float w, float h, float alpha, float blurPx) override;
    void popLayer() override;
    void setFontName(const char* name) override;

    // ---- 离屏读回（vk_shot 用）：BGRA8 prepremultiplied，行序自上而下 ----
    const uint8_t* readbackPixels(int& outW, int& outH);

    // ---- 窗口呈现（P2b）----
    // 强制一次"读回 + GDI 贴屏"（拖拽 resize 期由上层调用；全屏直翻转守卫由
    // present() 内部按 clientCoversMonitor 自动处理，不用上层管）。
    void presentViaGdi();
    // 每帧把**交换链图像**读回缓冲（验收/截图用；默认关，开了会多一次全屏拷贝）
    void setCaptureReadback(bool on);
    // 最近一次 present 走的是不是交换链（false = 读回 + GDI 贴屏）
    bool swapchainActive() const;
    bool presentUsedGdi() const;
    // 我们自己 vkAllocateMemory 出来的字节数（不含驱动持有的交换链图像）。
    // 测试用它区分「后端漏了显存」和「驱动把释放的显存留在池里」。
    size_t deviceMemoryBytes() const;

    // 窗口类名（无边框外壳用；同一进程里 GL 与 VK 两个外壳并存时不冲突）
    static const wchar_t* appClassName() { return L"BinUIAppVKWindow"; }

private:
    struct Impl;
    Impl* impl_;
};


// 无边框窗口 + 事件循环：与 GL 后端共用同一份实现（src/backends/binui_app.h）
using BinUIAppVK = BinUIAppT<BinUIVK>;
} // namespace binui

#endif // BINUI_VK_H
