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

#include "binui_format.h"   // v26：setImageFilter 的 BinUIImageFilter

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
    // v0.16.18 起：文字/图片**也参与**仿射，但走下面的 `setAffine` **状态**（文字是几百个
    // 小四边形，逐四边形传矩阵不现实）。这个函数仍只负责"底/边框"这类**整块**几何。
    virtual void fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                              uint32_t color, const float xf[6]) = 0;
    virtual void fillLine(float x0, float y0, float x1, float y1, float width,
                          uint32_t color) = 0;
    virtual void strokeRounded(float x, float y, float w, float h, float radius,
                               float thickness, uint32_t color) = 0;
    // ---- 图片（v0.16.15：子矩形 + 染色）----
    // u0..v1 是**源图的归一化子矩形**，tint 与纹理相乘（0xFFFFFFFF = 原色）。
    // 为什么需要这两个参数：后端没有"圆角裁剪"能力，圆角图片是"按行切细条、每条只画
    // 该行的横向区间"画出来的（见 binui_paint.h 的 BuildImageBands）—— 每条都要取源图
    // 对应的一行，所以必须能指定 UV 子矩形；染色则是 CSS filter: brightness() 的落点
    // （三个 GPU 后端的着色器本来就做"纹理 × 顶点色"，接到这里不用改着色器）。
    virtual void drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                             float x, float y, float w, float h, uint32_t tint) = 0;
    // 整图、不染色的便捷写法（老接口的语义）
    void drawImage(uint16_t index, float x, float y, float w, float h) {
        drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, 0xFFFFFFFFu);
    }

    // ---- 九宫格图片（v25：`image-slice: <n>`）----
    // 把源图按 `slice` 像素切成 9 块：四角保尺寸、四边与中心拉伸。UI 里的"带框/带圆角
    // 的贴图容器"（按钮底、卡片框、气泡）都靠它 —— 否则一张 32×32 的框拉伸到 300×120
    // 后边框粗成 20px。
    //
    // 为什么放在后端而不是 drawNodes 里切：切分要**源图像素尺寸**，只有解码过图片的
    // 后端知道。默认实现退回普通整图拉伸（GDI 这类遗留后端不必为此改）——
    // 这样"不支持九宫格"退化成"框被拉粗"，而不是什么都不画。
    virtual void drawImageNine(uint16_t index, float x, float y, float w, float h,
                               float slice, uint32_t tint) {
        (void)slice;
        drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, tint);
    }

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
    // 文字亚像素定位（v0.16.4 / v24 换成 ¼ 像素相位）：默认 false = 字形按整像素落位
    // （位图 1:1，最锐利）。有动画的节点传 true —— 位置不再取整，而是量化到 ¼ 像素，
    // 每份相位是一张独立的字形位图（不再靠 LINEAR 采样糊一层），文字既能连续滑又保持锐利。
    virtual void setTextSubpixel(bool on) { (void)on; }

    // ---- 仿射状态（v0.16.18：文字/图片参与旋转）----
    // 设置之后，**接下来所有原语**的四边形都按 xf（2×3 行主序 [a c e; b d f]）变换：
    //   屏幕 = (a*x + c*y + e, b*x + d*y + f)
    // 于是"旋转卡片里的文字、倾斜的图标"不需要新原语 —— 调用方改用**节点局部坐标**画，
    // 矩阵负责转到屏幕。传 nullptr 关闭。
    //
    // 返回 false = 这个后端不支持（GDI：字形与位图都交给系统光栅化，没有任意四边形）→
    // 调用方必须退回"只转盒子、文字/图片保持轴对齐"的旧行为，这样老页面的像素一位不变。
    //
    // 注意：fillRoundedXf 自带矩阵，**不受**这个状态影响（否则圆角底会被变换两次）。
    //
    // v0.16.21：xf 由绘制层**沿祖先链复合**而来（M[局部] = M[父] ∘ 平移 ∘ 自身变换），
    // 所以一次 setAffine 就覆盖了整棵子树——后端不需要知道自己转的是"第几层"。
    virtual bool setAffine(const float xf[6]) { (void)xf; return false; }

    // ---- 图片滤镜（v26：CSS filter 的颜色矩阵）----
    // 设置之后**下一次** prepareImage / prepareImageRaw 解码时会把这个矩阵应用到像素上，
    // 纹理按 (imageIndex, 矩阵哈希) 单独缓存 —— 同一张图配不同滤镜就是两份纹理。
    // 传 nullptr 清除（键退回 imageIndex）。默认空实现：不支持的后端（GDI 遗留路径）
    // 照常画原图，只是没有滤镜效果。
    // 注意：brightness() 不走这里（它等价于乘一个顶点色，见 paint.reserve[2]）。
    virtual void setImageFilter(const BinUIImageFilter* f) { (void)f; }

    // ---- 文字阴影（v24：CSS text-shadow）----
    // 在画一个节点的文字**之前**设好，节点画完用 alpha=0 清掉（或下个节点重设）。
    // color 的 alpha 已折入元素 opacity；blur < 0.5px 由实现按"硬阴影"（偏移一份字形）处理。
    // 默认空实现：不支持的后端（如 GDI 的老路径）文字照常画，只是没有阴影。
    virtual void setTextShadow(float dx, float dy, float blur, uint32_t color) {
        (void)dx; (void)dy; (void)blur; (void)color;
    }

    // ---- 图层 / 组透明度（v0.16.9）----
    // push 之后的所有绘制先落到一块离屏图层（框 = x,y,w,h），pop 时按 alpha 整体合成回来。
    // 语义对齐 CSS 的组透明度：子元素互相重叠时重叠区不会被叠深两次。
    // 返回 false = 这个后端（或这个调用）不支持 → 调用方退回"逐元素乘 alpha"（0.16.8 行为），
    // 所以没实现的后端在像素上不会有任何变化。
    virtual bool pushLayer(float x, float y, float w, float h, float alpha) {
        (void)x; (void)y; (void)w; (void)h; (void)alpha;
        return false;
    }
    virtual void popLayer() {}

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
