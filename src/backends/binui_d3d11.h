// binui_d3d11.h - 高性能 GPU 渲染后端：D3D11 直接绘制 binUI 场景树
// 窗口仅用于交换链呈现（WS_POPUP 无边框），所有 UI 内容由 GPU 直接绘制：
// 纯色/圆角矩形 = 顶点四边形，文本 = GDI 字形图集纹理，滚动/裁剪 = scissor。
// 无 DWM/系统接口调校，VSync 按需呈现，空闲时 CPU 0%。
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "binui_renderer.h"

namespace binui {

// GPU 场景渲染器：立即模式原语 + 帧呈现（对布局/命中测试零依赖）
class BinUID3D {
public:
    BinUID3D();
    ~BinUID3D();
    BinUID3D(const BinUID3D&) = delete;
    BinUID3D& operator=(const BinUID3D&) = delete;

    // 创建 D3D11 设备 + 交换链（hwnd 为无边框窗口句柄）
    bool init(void* hwnd, int width, int height, bool vsync = true);
    void shutdown();
    bool isReady() const;
    int width() const;
    int height() const;
    void setVsync(bool on);
    void resize(int width, int height);
    void setTransparent(bool t); // 分层窗口：画布 alpha 透出桌面

    // 帧开始：清屏 + 重置场景（clipRect 为脏矩形，nullptr = 全窗口）
    void beginFrame(const void* clipRect);
    // 提交场景（按批次/裁剪/采样器顺序绘制）并呈现（VSync 时阻塞）
    void present();

    // ---------- 立即模式绘制原语（颜色 0xAARRGGBB） ----------
    void fillRect(float x, float y, float w, float h, uint32_t color);
    void fillRounded(float x, float y, float w, float h, float radius, uint32_t color);
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
    // 图片：解码缓存 + 拉伸绘制（generation 换图版本号：变化时重新上传）
    bool prepareImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation);
    // 原始 RGBA 像素直传纹理（RGBA 字节序，无解码）
    bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation);
    void drawImage(uint16_t index, float x, float y, float w, float h);

    // 裁剪栈：与当前 scissor 求交，返回进入前的深度（popClipTo 恢复）
    int pushClip(float x, float y, float w, float h);
    void popClipTo(int depth);

    void setFontName(const char* name);

private:
    struct Impl;
    Impl* impl_;
};

// 把 BinUIRenderer 节点树绘制到 GPU 场景（与 drawNodesGDI 语义一致）
void drawNodesD3D(BinUID3D& gfx, const BinUIRenderer& renderer, const void* clipRect,
                  const std::function<void(BinUID3D&, uint16_t, float, float, float, float)>& canvas = {});
// 点击定位输入框光标（基于字形 advance 测量，无 GDI 依赖）
void placeCaretAtClickD3D(BinUIRenderer& renderer, BinUID3D& gfx, float cx, float cy);

// 光标闪烁相位（500ms 周期），与 GDI 后端共用语义
bool caretBlinkOn();

// 判断点击是否落在"标题栏"（窗口顶部通栏容器，非按钮/输入框）：
// 应用在 press 回调中先判此函数，命中则调用 BinUIAppD3D::beginWindowDrag()
bool isTitlebarDrag(const BinUIRenderer& renderer, float x, float y);

// 无边框（WS_POPUP）应用外壳：D3D11 交换链 + 事件循环，API 与 BinUIApp 对齐
class BinUIAppD3D {
public:
    using UpdateFn  = std::function<void(float dt)>;
    using DrawFn    = std::function<void(BinUID3D&, const void* clipRect)>;
    using MoveFn    = std::function<void(float x, float y)>;
    using PressFn   = std::function<void(float x, float y)>;
    using ReleaseFn = std::function<void(float x, float y)>;
    using LeaveFn   = std::function<void()>;
    using ResizeFn  = std::function<void(int w, int h)>;
    using CharFn    = std::function<void(const std::string& utf8)>;
    using KeyFn     = std::function<void(int vk)>;
    using WheelFn   = std::function<void(float x, float y, float dy)>;
    using FocusFn   = std::function<void(bool focused)>;

    BinUIAppD3D(int width, int height, bool vsync = true);
    ~BinUIAppD3D();

    // 窗口选项（<canvas> 描述）：resizable / aspect(w:h 等比) / transparent / title
    void setWindowOptions(bool resizable, float aspect, bool transparent, const char* title);

    void setUpdateFn(UpdateFn fn)   { update_ = std::move(fn); }
    void setDrawFn(DrawFn fn)       { draw_ = std::move(fn); }
    void setMoveFn(MoveFn fn)       { move_ = std::move(fn); }
    void setPressFn(PressFn fn)     { press_ = std::move(fn); }
    void setReleaseFn(ReleaseFn fn) { release_ = std::move(fn); }
    void setLeaveFn(LeaveFn fn)     { leave_ = std::move(fn); }
    void setResizeFn(ResizeFn fn)   { resize_ = std::move(fn); }
    void setCharFn(CharFn fn)       { char_ = std::move(fn); }
    void setKeyFn(KeyFn fn)         { key_ = std::move(fn); }
    void setWheelFn(WheelFn fn)     { wheel_ = std::move(fn); }
    void setFocusFn(FocusFn fn)     { focus_ = std::move(fn); }

    void invalidateRect(int x, int y, int w, int h);
    void invalidateAll();
    void requestExit() { running_ = false; }

    // 无边框窗口交互：拖动（在标题栏按下时调用）/ 最小化
    void beginWindowDrag();
    void minimize();

    BinUID3D& gfx() { return gfx_; }

    int run(); // 创建无边框窗口 + D3D11 + 事件循环

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void handleMouseMove(float x, float y);
    void handleMousePress(float x, float y);
    void handleMouseRelease(float x, float y);
    void handleMouseLeave();
    void handleMouseWheel(float x, float y, float dy);
    void handleChar(wchar_t ch);
    void handleKey(int vk);
    void handleFocusLost();
    void handleResize(int w, int h);

    int width_ = 0, height_ = 0;
    bool vsync_ = true;
    BinUID3D gfx_;
    void* hwnd_ = nullptr;

    bool resizable_ = true;
    float aspect_ = 0;
    bool transparent_ = false;
    std::string title_;

    UpdateFn update_;
    DrawFn draw_;
    MoveFn move_;
    PressFn press_;
    ReleaseFn release_;
    LeaveFn leave_;
    ResizeFn resize_;
    CharFn char_;
    KeyFn key_;
    WheelFn wheel_;
    FocusFn focus_;

    bool running_ = false;
    bool dirty_ = true;
    int dirtyL_ = 0, dirtyT_ = 0, dirtyR_ = 0, dirtyB_ = 0;
    int pendingW_ = 0, pendingH_ = 0; // 挂起的窗口尺寸（WM_SIZE 只记录，帧内应用一次）
    bool trackingLeave_ = false;
    wchar_t pendingHigh_ = 0;

    void applyPendingResize(); // 应用挂起 resize：渲染器重排 + gfx 重建 + 全量重绘
    void renderOnce();         // 应用挂起 resize 后绘制一帧并呈现
    void startResize(int hit); // 手动 resize 拖拽循环（每轮同步渲染）
};

} // namespace binui
