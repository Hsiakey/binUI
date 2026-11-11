// binui_gfx.h - 绘制后端契约（v19）
//
// 任何 binUI 绘制后端（GDI / D3D11 / GL / Vulkan / 将来的 Metal）都实现这一套接口：
// 节点树遍历只认这 7 个原语 + 裁剪栈，不碰任何后端私有状态。
//
// 六条硬约定（docs/vulkan_design.md §1）：
//   1. 只走这些原语；新增绘制能力要先进这个接口，不许让遍历去摸后端私有 API。
//   2. beginFrame(clipRect) → 只在 clipRect 内清屏并绘制 → present() 整帧贴出。
//      绝不允许"在交换链 image 上做局部重绘"（交换链内容不保证保留）。
//   3. 颜色 0xAARRGGBB，着色器输出预乘 alpha，混合 (ONE, ONE_MINUS_SRC_ALPHA)，不用 sRGB 帧缓冲。
//   4. 初始化失败 → isReady() == false，所有原语变 no-op，宿主窗口照常跑。
//   5. 没有脏区时不许占 CPU（睡在消息等待上）。
//   6. 一个窗口同一时刻只有一个呈现 API（VK_KHR_win32_surface Issue 2）。
#ifndef BINUI_GFX_H
#define BINUI_GFX_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace binui {

class IBinUIGfx {
public:
    virtual ~IBinUIGfx() = default;

    // ---- 生命周期 ----
    virtual bool isReady() const = 0;
    virtual int  width() const = 0;
    virtual int  height() const = 0;
    virtual void resize(int width, int height) = 0;
    virtual void setVsync(bool on) = 0;
    virtual void setTransparent(bool t) = 0;   // 分层窗口：画布 alpha 透出桌面

    // ---- 帧（clipRect = int[4]{l,t,r,b}，nullptr = 全窗口）----
    virtual void beginFrame(const void* clipRect) = 0;
    virtual void present() = 0;

    // present() 自己会不会把调用线程**阻塞到下一个垂直同步**（vsync 开着且真走交换链）。
    // 返回 true 时应用外壳**不再自己掐帧**——让 acquire/present 当节拍器，帧相位因此
    // 天然对齐刷新，不会和显示器"打拍子"（合成节拍与真实刷新差个零点几毫秒，几帧就错开
    // 一格，表现为肉眼可见的微抖/微卡）。走读回 + GDI 贴屏、透明窗、远程会话这些不阻塞
    // 的路径必须返回 false，由外壳按刷新率节流。
    virtual bool presentPacesFrames() const { return false; }

    // ---- 原语（颜色 0xAARRGGBB）----
    virtual void fillRect(float x, float y, float w, float h, uint32_t color) = 0;
    virtual void fillRounded(float x, float y, float w, float h, float radius,
                             uint32_t color) = 0;
    // ---- 仿射圆角矩形（v20：旋转 / 非等比缩放 / 斜切）----
    // xf 是 2×3 行主序仿射 [a c e; b d f]：把**局部坐标** (lx, ly)（矩形左上角为原点，
    // 范围 [0,w]×[0,h]）映射到屏幕 X = a*lx + c*ly + e, Y = b*lx + d*ly + f。
    // radius <= 0 时就是一整个四边形；> 0 时把圆角模板的九宫格每片按 xf 变换后发射。
    // stroke=true 走 1px 圆环模板、thickness 像素多层同心（与 strokeRounded 同语义）。
    // 约定：文字/图片/路径**不参与**仿射（字形与贴图要按像素对齐，旋转会糊），
    // 调用方负责只把"底/边框"交给它。
    virtual void fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                              uint32_t color, const float xf[6]) = 0;
    virtual void fillLine(float x0, float y0, float x1, float y1, float width,
                          uint32_t color) = 0;
    virtual void strokeRounded(float x, float y, float w, float h, float radius,
                               float thickness, uint32_t color) = 0;
    virtual void drawImage(uint16_t index, float x, float y, float w, float h) = 0;

    // ---- 文本（x/y 为基线；centerH/centerV 时 x/y 为矩形中心）----
    virtual void drawText(float x, float y, float size, int weight,
                          const std::string& utf8, uint32_t color,
                          bool centerH = false, bool centerV = false,
                          bool italic = false) = 0;
    // 自动换行绘制；align：0=左 1=居中 2=右（相对 maxW 的可用宽度）
    virtual void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                                 const std::string& utf8, uint32_t color, int align = 0,
                                 bool italic = false) = 0;
    virtual float textWidth(float size, int weight, const std::string& utf8,
                            bool italic = false) = 0;
    virtual void fontMetrics(float size, int weight, int& ascent, int& lineHeight,
                             bool italic = false) = 0;

    // ---- 图片（generation 变化时重新上传）----
    virtual bool prepareImage(uint16_t index, const uint8_t* data, size_t size,
                              uint32_t generation) = 0;
    virtual bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                                 uint32_t generation) = 0;

    // ---- 裁剪栈（与当前 scissor 求交，返回进入前深度）----
    virtual int  pushClip(float x, float y, float w, float h) = 0;
    virtual void popClipTo(int depth) = 0;

    virtual void setFontName(const char* name) = 0;
};

// 画布回调：宿主拿到绘制面矩形，用后端无关的原语自己画（<canvas> 节点）。
// 参数类型是接口而不是具体后端 —— 这样同一份 demo 代码能跑任意后端。
using CanvasFn = std::function<void(IBinUIGfx& gfx, uint16_t nodeId, float x, float y,
                                    float w, float h)>;

} // namespace binui

#endif // BINUI_GFX_H
