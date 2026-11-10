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

    void fillRect(float x, float y, float w, float h, uint32_t color) override;
    void fillRounded(float x, float y, float w, float h, float radius,
                     uint32_t color) override;
    void fillLine(float x0, float y0, float x1, float y1, float width,
                  uint32_t color) override;
    void strokeRounded(float x, float y, float w, float h, float radius,
                       float thickness, uint32_t color) override;
    void drawImage(uint16_t index, float x, float y, float w, float h) override;

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

    bool prepareImage(uint16_t index, const uint8_t* data, size_t size,
                      uint32_t generation) override;
    bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                         uint32_t generation) override;

    int  pushClip(float x, float y, float w, float h) override;
    void popClipTo(int depth) override;
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

private:
    struct Impl;
    Impl* impl_;
};

} // namespace binui

#endif // BINUI_VK_H
