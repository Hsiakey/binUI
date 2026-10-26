// app_win32.h - 极简 Win32 应用外壳（演示程序用）
// 窗口创建 + 消息循环 + 帧回调 + GDI 绘制。UI 核心库本身不依赖它。
#pragma once
#include <functional>
#include <string>

#include "binui_renderer.h"

class BinUIApp {
public:
    using UpdateFn = std::function<void(float deltaTime)>;
    using DrawFn   = std::function<void(void* hdc, const void* clipRect)>; // 在给定 HDC 上绘制，clipRect 为脏矩形（null=全窗口）
    using MoveFn   = std::function<void(float x, float y)>;      // 鼠标移动（悬停）
    using PressFn  = std::function<void(float x, float y)>;      // 左键按下
    using ReleaseFn = std::function<void(float x, float y)>;     // 左键松开
    using LeaveFn  = std::function<void()>;                      // 鼠标离开窗口
    using ResizeFn = std::function<void(int w, int h)>;
    using CharFn   = std::function<void(const std::string& utf8)>; // 文本字符（含 IME）
    using KeyFn    = std::function<void(int vk)>;                  // 按键（VK_* 码）
    using WheelFn  = std::function<void(float x, float y, float dy)>; // 滚轮（dy>0 向上）
    using FocusFn  = std::function<void(bool focused)>;            // 窗口焦点变化

    BinUIApp(const std::string& title, int width, int height);
    ~BinUIApp();

    void setUpdateFn(UpdateFn fn);
    void setDrawFn(DrawFn fn);
    void setMoveFn(MoveFn fn);
    void setPressFn(PressFn fn);
    void setReleaseFn(ReleaseFn fn);
    void setLeaveFn(LeaveFn fn);
    void setResizeFn(ResizeFn fn);
    void setCharFn(CharFn fn);
    void setKeyFn(KeyFn fn);
    void setWheelFn(WheelFn fn);
    void setFocusFn(FocusFn fn);

    // 脏矩形：只重绘变化区域（x,y 为客户区坐标，w/h 为宽高）
    void invalidateRect(int x, int y, int w, int h);
    void invalidateAll();

    int run();
    void requestExit();

    // 无边框窗口支持：窗口句柄 / 标题栏拖拽 / 最小化 / 最大化（供自绘标题栏调用）
    void* hwnd();
    void beginWindowDrag(); // 按下标题栏时调用：转入系统拖动
    void minimize();
    void maximize();

    // 自绘聚焦细边框（1px，窗口获得焦点时显示；color 为 0xRRGGBB，默认白）
    void setFocusBorder(bool show, uint32_t color = 0xFFFFFF);
    bool focused() const { return focused_; }

    // 由窗口过程调用（内部）
    void OnDraw(void* hdc, const void* clipRect);
    void OnMouseMove(float x, float y);
    void OnMousePress(float x, float y);
    void OnMouseRelease(float x, float y);
    void OnMouseLeave();
    void OnResize(int w, int h);
    void OnChar(wchar_t ch);   // UTF-16 代码单元，代理对自动合并
    void OnKey(int vk);
    void OnMouseWheel(float x, float y, float dy);
    void OnFocusLost();

    bool trackingLeave_ = false; // WM_MOUSELEAVE 已注册（WndProc 访问）
    bool focused_ = false;       // 窗口是否获得焦点（WndProc/WM_ACTIVATE 维护）

private:
    void* hwnd_ = nullptr;
    bool running_ = false;
    int width_;
    int height_;
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
    wchar_t pendingHigh_ = 0; // 待合并的高位代理

    bool focusBorder_ = true;        // 是否显示自绘聚焦细边框（无系统边框时作为焦点指示）
    uint32_t focusBorderColor_ = 0xFFFFFF; // 0xRRGGBB

    void* memDC_ = nullptr;    // 持久离屏缓冲（重建时恢复原位图再删除）
    void* memBmp_ = nullptr;
    void* memOldBmp_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;
};

// 光标闪烁相位（500ms 周期；绘制与刷新共用同一相位）
bool caretBlinkOn();

// 设置 GDI 绘制用字体（默认 Segoe UI；终端等场景用等宽字体如 Consolas）
void setGdiFontName(const char* name);

// 将 BinUIRenderer 节点树用 GDI 绘制到指定 HDC（演示用 GDI 后端）。
// clipRect 为脏矩形（RECT*，null=按窗口客户区全量）；要求 hdc 已被裁剪到 clipRect。
void drawNodesGDI(void* hdc, const binui::BinUIRenderer& renderer, const void* clipRect = nullptr);

// 点击定位文本框光标：在聚焦输入框内按点击 x 坐标放置光标（渲染器无字体测量能力，由后端完成）
void placeCaretAtClick(binui::BinUIRenderer& renderer, float x, float y);

// 自绘标题栏拖拽判断：命中非交互节点且其祖先链含 BINUI_TITLEBAR → 可拖动窗口。
// 标题栏内的按钮等交互控件返回 false（响应点击而非拖拽）。
bool isTitlebarDrag(const binui::BinUIRenderer& renderer, float x, float y);
