// binui_d3d11.h - D3D11 绘制后端
//
// 定位与 GL / Vulkan 同构：实现 IBinUIGfx，节点树遍历是共享的
//（src/backends/binui_draw_nodes.h），本文件只管"把四边形画到 D3D11 上"。
//
// 与 GL 后端的关系：两者都是"顶点四边形 + 纹理采样 + scissor 裁剪"的立即模式，
// D3D11 侧的差别只在 API（ID3D11Device/Context）。窗口外壳、事件循环、输入/IME、
// 无边框拖拽、帧节流、脏矩形都由 src/backends/binui_app.h 的 BinUIAppT<Gfx> 提供。
//
// 呈现：画进持久离屏纹理（offTex，脏矩形只重绘局部），present 时整帧
// blit 到交换链；透明窗口 / 全屏直翻转守卫 / 拖拽期走"读回 + GDI 贴屏"。
#pragma once

#include <cstdint>
#include <string>

#include "backends/binui_app.h"   // 无边框窗口外壳模板（GL / VK / D3D11 共用同一份）
#include "binui_gfx.h"

namespace binui {

class BinUID3D : public IBinUIGfx {
public:
    BinUID3D();
    ~BinUID3D() override;
    BinUID3D(const BinUID3D&) = delete;
    BinUID3D& operator=(const BinUID3D&) = delete;

    // 创建 D3D11 设备 + 交换链（hwnd 为无边框窗口句柄）
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

    // 强制一次"读回 + GDI 贴屏"（拖拽 resize 期由上层调用；全屏直翻转守卫由
    // present() 内部按客户区是否铺满显示器自动处理）
    void presentViaGdi();

    // 离屏读回：把当前帧内容从离屏纹理读回 CPU（BGRA8，行序自上而下）。
    // 验收工具用（tools/d3d11_shot.cpp）—— 它不依赖窗口是否被 DWM 合成，
    // 所以能抓到被遮挡/隐藏窗口的真实像素。返回的指针在下一次读回前有效。
    const uint8_t* readbackPixels(int& outW, int& outH);

    // 窗口类名（无边框外壳用；同进程三个后端共存时不冲突）
    static const wchar_t* appClassName() { return L"BinUIAppD3DWindow"; }

private:
    struct Impl;
    Impl* impl_;
};

// 无边框窗口 + 事件循环：与 GL / Vulkan 后端共用同一份实现（src/backends/binui_app.h）
using BinUIAppD3D = BinUIAppT<BinUID3D>;

} // namespace binui
