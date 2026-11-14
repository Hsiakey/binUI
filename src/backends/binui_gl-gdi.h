// binui_gl-gdi.h - Windows WGL + GDI 变体的 OpenGL 渲染后端：直接绘制 binUI 场景树
// （WGL 建上下文 + SwapBuffers 呈现，resize 拖拽期走 GDI StretchDIBits 规避 NVIDIA
// 驱动"窗口 resize + SwapBuffers"崩溃；图片解码/字形光栅化走 GDI+ / GDI）。
// 与 binui_d3d11.h 完全同构（API/语义一致），仅交换链与着色语言不同：
// WGL + OpenGL 3.3 core（GLSL）替代 D3D11 + HLSL。
// 窗口为 WS_POPUP 无边框，所有 UI 内容由 GPU 绘制：
// 纯色/圆角矩形 = 顶点四边形（九宫格模板），文本 = FreeType 字形图集，滚动/裁剪 = scissor。
// VSync 按需呈现，空闲时 CPU 0%。
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "binui_renderer.h"
#include "binui_gfx.h"   // IBinUIGfx：后端契约（v19）
#include "backends/binui_app.h"   // 无边框窗口外壳模板（与 Vulkan 共用同一份）

namespace binui {

// GPU 场景渲染器：立即模式原语 + 帧呈现（对布局/命中测试零依赖）
class BinUIGLGdi : public IBinUIGfx {
public:
    BinUIGLGdi();
    ~BinUIGLGdi();
    BinUIGLGdi(const BinUIGLGdi&) = delete;
    BinUIGLGdi& operator=(const BinUIGLGdi&) = delete;

    // 创建 OpenGL 上下文 + 离屏帧缓冲（hwnd 为无边框窗口句柄）
    bool init(void* hwnd, int width, int height, bool vsync = true);
    void shutdown();
    bool isReady() const;
    int width() const;
    int height() const;
    void setVsync(bool on);
    void setTransparent(bool t); // 分层窗口：画布 alpha 透出桌面
    void resize(int width, int height);

    // 帧开始：绑定离屏帧缓冲 + 重置场景（clipRect 为脏矩形，nullptr = 全窗口）
    void beginFrame(const void* clipRect);
    // 提交场景（按批次/裁剪/纹理顺序绘制）并呈现（VSync 时阻塞）
    void present();
    // present() 在 vsync 开着的普通窗口路径上会阻塞到下一个垂直同步（SwapBuffers +
    // wglSwapIntervalEXT(1)），所以外壳可以让它当节拍器、自己不再掐帧。
    bool presentPacesFrames() const;
    // 提交场景但不 SwapBuffers：读回离屏像素 → StretchDIBits 到窗口 DC。
    // 用于 resize 拖拽期间——NVIDIA 驱动在"窗口 resize + SwapBuffers"下会崩
    //（nvoglv64 DrvPresentBuffers），走 GDI 呈现绕开驱动 present 路径。
    void presentViaGdi();

    // ---------- 立即模式绘制原语（颜色 0xAARRGGBB） ----------
    void fillRect(float x, float y, float w, float h, uint32_t color);
    void fillRounded(float x, float y, float w, float h, float radius, uint32_t color);
    // 仿射圆角矩形（旋转 / 非等比缩放）：xf = [a c e; b d f]，见 binui_gfx.h
    void fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                       uint32_t color, const float xf[6]);
    // 线段（端点 x0,y0 -> x1,y1，宽度像素，实心旋转四边形）
    void fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color);
    void strokeRounded(float x, float y, float w, float h, float radius,
                       float thickness, uint32_t color);
    // 文本：x/y 为基线（centerH/centerV 时 x/y 为矩形中心）
    // italic：FreeType 合成倾斜（<i>/<em>/font-style:italic），与粗体一样占独立字形图集
    void drawText(float x, float y, float size, int weight,
                  const std::string& utf8, uint32_t color,
                  bool centerH = false, bool centerV = false, bool italic = false);
    // 自动换行绘制；align：0=左 1=居中 2=右（相对 maxW 的可用宽度）
    void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                         const std::string& utf8, uint32_t color, int align = 0,
                         bool italic = false);
    float textWidth(float size, int weight, const std::string& utf8, bool italic = false);
    void fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic = false);
    // 文字亚像素定位：true = 位置保留小数（图集 LINEAR 采样），动画节点用
    void setTextSubpixel(bool on);
    // v0.16.9：组透明度的离屏合成（见 binui_gfx.h 的 pushLayer 注释）
    bool pushLayer(float x, float y, float w, float h, float alpha);
    void popLayer();
    // 图片：解码缓存 + 拉伸绘制（generation 换图版本号：不一致时重新上传纹理）
    bool prepareImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation);
    // 原始 RGBA 像素直传纹理（RGBA 字节序，无解码）
    bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation);
    void drawImage(uint16_t index, float x, float y, float w, float h);

    // 裁剪栈：与当前 scissor 求交，返回进入前的深度（popClipTo 恢复）
    int pushClip(float x, float y, float w, float h);
    void popClipTo(int depth);

    void setFontName(const char* name);

    // 窗口类名（无边框外壳用；同一进程里 GL 与 VK 两个外壳并存时不冲突）
    static const wchar_t* appClassName() { return L"BinUIAppGLGdiWindow"; }

private:
    struct Impl;
    Impl* impl_;
};

// 把 BinUIRenderer 节点树绘制到 GL 场景（与 drawNodesGDI/D3D 语义一致）
// canvas：<canvas> 节点的绘制回调（已裁剪到画布矩形），nullptr = 忽略
using CanvasDrawFn = std::function<void(BinUIGLGdi& gfx, uint16_t nodeId,
                                        float x, float y, float w, float h)>;
void drawNodesGLGdi(BinUIGLGdi& gfx, const BinUIRenderer& renderer, const void* clipRect,
                 const CanvasDrawFn& canvas = {});

// 窗口 + 事件循环外壳：与 Vulkan 后端共用同一份实现（src/backends/binui_app.h）。
// 两个后端只差一个模板参数，输入/IME/拖拽/节流逻辑不会分叉。
using BinUIAppGLGdi = BinUIAppT<BinUIGLGdi>;

} // namespace binui
