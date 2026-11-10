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
    // 提交场景但不 SwapBuffers：读回离屏像素 → StretchDIBits 到窗口 DC。
    // 用于 resize 拖拽期间——NVIDIA 驱动在"窗口 resize + SwapBuffers"下会崩
    //（nvoglv64 DrvPresentBuffers），走 GDI 呈现绕开驱动 present 路径。
    void presentViaGdi();

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
    // 图片：解码缓存 + 拉伸绘制（generation 换图版本号：不一致时重新上传纹理）
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

// 把 BinUIRenderer 节点树绘制到 GL 场景（与 drawNodesGDI/D3D 语义一致）
// canvas：<canvas> 节点的绘制回调（已裁剪到画布矩形），nullptr = 忽略
using CanvasDrawFn = std::function<void(BinUIGLGdi& gfx, uint16_t nodeId,
                                        float x, float y, float w, float h)>;
void drawNodesGLGdi(BinUIGLGdi& gfx, const BinUIRenderer& renderer, const void* clipRect,
                 const CanvasDrawFn& canvas = {});
// 点击定位输入框光标（基于字形 advance 测量）
// 把点击位置换算成光标位置并落点；extend = 拖选（只挪 caret，选区起点不动）。
// 返回换算出来的 UTF-16 索引（双击选词要用同一个换算结果）。
size_t placeCaretAtClickGLGdi(BinUIRenderer& renderer, BinUIGLGdi& gfx, float cx, float cy,
                              bool extend = false);

// IME 光标定位：按渲染器当前编辑态算出"光标在客户区的位置 + 行高"。
// 宿主在 setImeCaretFn 里直接调它即可，不用自己量文本宽度：
//     app.setImeCaretFn([&](int& x, int& y, int& h) {
//         return imeCaretFromRenderer(renderer, app.gfx(), x, y, h);
//     });
// 不接这个钩子时输入法只能把组字窗/候选框摆到默认位置（实测会落到窗口外），
// 表现为"打拼音屏幕上什么都没有"。
// 返回 false = 当前没有正在编辑的输入框（IME 侧按"没有光标"处理）。
bool imeCaretFromRenderer(const BinUIRenderer& renderer, BinUIGLGdi& gfx,
                          int& x, int& y, int& lineH);

// 光标闪烁相位（500ms 周期），与 GDI/D3D 后端共用语义
bool caretBlinkOn();

// 判断点击是否落在"标题栏"（窗口顶部通栏容器，非按钮/输入框）：
// 应用在 press 回调中先判此函数，命中则调用 BinUIAppGLGdi::beginWindowDrag()
bool isTitlebarDrag(const BinUIRenderer& renderer, float x, float y);

// 无边框（WS_POPUP）应用外壳：OpenGL 交换 + 事件循环，API 与 BinUIAppD3D 对齐
class BinUIAppGLGdi {
public:
    using UpdateFn  = std::function<void(float dt)>;
    using DrawFn    = std::function<void(BinUIGLGdi&, const void* clipRect)>;
    using MoveFn    = std::function<void(float x, float y)>;
    using PressFn   = std::function<void(float x, float y)>;
    using ReleaseFn = std::function<void(float x, float y)>;
    using LeaveFn   = std::function<void()>;
    using ResizeFn  = std::function<void(int w, int h)>;
    using CharFn    = std::function<void(const std::string& utf8)>;
    using KeyFn     = std::function<void(int vk)>;
    using WheelFn   = std::function<void(float x, float y, float dy)>;
    using FocusFn   = std::function<void(bool focused)>;
    // 光标查询：给定客户区坐标，返回 BinUICursorKind（0=箭头 1=手型 2=I 型 3=竖直 4=水平）。
    // 以前整个库只加载了 IDC_ARROW，文本框里不会变 I 型、按钮上没有手型 —— 用过就发现"不像原生"。
    using CursorFn  = std::function<int(float x, float y)>;
    // 双击：宿主拿到的是客户区坐标（双击选词用）
    using DoubleClickFn = std::function<void(float x, float y)>;
    // IME：返回光标在客户区的位置与行高（用于把输入法候选窗摆到光标下方）。
    // 返回 false = 当前没有正在编辑的输入框。
    using ImeCaretFn = std::function<bool(int& x, int& y, int& lineH)>;
    // IME 开关：返回 true = 现在需要输入法（通常就是 renderer.isEditing()）。
    // 中文输入法会把字母键改成 VK_PROCESSKEY 自己组字，没有编辑框时那样会把 WASD
    // 焦点导航整个吃掉（实测窗口收到的 wParam=229）。所以这个钩子返回 false 时窗口
    // 把 IME 上下文摘下来（字母原样到达），返回 true 再挂回去。
    // 不接这个钩子 = 保持老行为（IME 常挂），此时 WASD 在中文输入法下收不到。
    using ImeActiveFn = std::function<bool()>;

    BinUIAppGLGdi(int width, int height, bool vsync = true);
    ~BinUIAppGLGdi();

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
    void setCursorFn(CursorFn fn)   { cursor_ = std::move(fn); }
    void setDoubleClickFn(DoubleClickFn fn) { dblclick_ = std::move(fn); }
    void setImeCaretFn(ImeCaretFn fn) { imeCaret_ = std::move(fn); }
    void setImeActiveFn(ImeActiveFn fn) { imeActive_ = std::move(fn); }

    void invalidateRect(int x, int y, int w, int h);
    void invalidateAll();
    void requestExit() { running_ = false; }

    // 取消输入法正在进行的组字（没提交的拼音直接丢掉）。
    //
    // 宿主在**自己**把编辑目标换掉时调一次（Tab 切焦点、代码里 blur、隐藏输入框等）：
    // 编辑会话结束时输入法还不知道，它会把没转换的拼音原样当结果串提交。
    //
    // 注意：鼠标点击不用接这个 —— 组字期间的那次点击会被输入法自己吃掉（它先把
    // 拼音提交掉，窗口根本收不到 WM_LBUTTONDOWN），补救不到。窗口失焦时本库已经
    // 自动调过一次（WM_KILLFOCUS）。
    void cancelImeComposition();

    // 每帧调一次：按 imeActive_() 把 IME 上下文挂上/摘下（见 ImeActiveFn 的注释）。
    void syncImeContext();

    // 无边框窗口交互：拖动（在标题栏按下时调用）/ 最小化
    void beginWindowDrag();
    void minimize();
    void maximize();   // 最大化 / 还原（GL/GDI 窗口同样是无边框自绘，syscall 直接走 ShowWindow）

    BinUIGLGdi& gfx() { return gfx_; }

    int run(); // 创建无边框窗口 + OpenGL 上下文 + 事件循环

    // ---------- 非阻塞外壳（给 C ABI / 其他语言绑定 / 自管循环的宿主用）----------
    // run() 完全等价于 open(); while (pump(1) == 0) {} close(); 三者的行为逐帧一致，
    // 只是把"谁拥有消息循环"交出去：宿主自己决定每帧什么时候推进。
    bool open();              // 建窗口 + GL 上下文（失败返回 false）
    // 推进一帧：收消息 → update → 绘制脏区。
    // 返回 0 = 继续；1 = 已关闭；**-1 = 调用线程不是建窗线程**（宿主写错了，别再继续）。
    // 窗口/WGL 上下文都有线程亲和：从别的线程进来不会立刻炸，而是随机崩或退出时挂死，
    // 所以这里显式拦一道（见 docs/issues/08）。
    int  pump(int timeoutMs);
    void close();             // 销毁窗口（幂等，可在 pump 返回 1 后调用）
    bool isOpen() const { return running_; }
    // 当前调用线程是不是建窗线程（binui_render / binui_pump 的自检用）
    bool onOwnerThread() const;

    // ---------- 子窗口模式（作为控件嵌进宿主窗口）----------
    // 和 open() 的差别只有三处：WS_CHILD 而不是顶层无边框窗；**不碰进程 DPI 策略**
    //（DPI 由宿主决定）；**不自己收消息**（宿主 DispatchMessage 会把鼠标/键盘投递给
    //  本窗口的 wndProc，事件链路原样可用）。边框拖动 / 缩放 / 最小化在此模式下无效。
    bool openChild(void* parent, int x, int y, int width, int height);
    bool isChildMode() const { return childMode_; }
    // 只出一帧（推进动画时钟 + 绘制脏区），不碰消息队列。子窗口模式的主入口。
    void renderFrame();
    // 宿主改了本区域尺寸时调用（等价于收到 WM_SIZE，下一帧生效）
    void setClientSize(int width, int height);
    // 本窗口句柄（宿主布局子窗口时要 SetWindowPos 它）
    void* windowHandle() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static bool ensureClass();   // 注册（并缓存）窗口类：顶层窗与子窗口共用
    void handleMouseMove(float x, float y);
    void handleMousePress(float x, float y);
    void handleMouseRelease(float x, float y);
    void handleMouseLeave();
    void handleMouseWheel(float x, float y, float dy);
    void handleChar(wchar_t ch);
    void handleKey(int vk);
    void handleFocusLost();
    void handleResize(int w, int h);
    // IME UI 定位：把组字窗/候选框摆到宿主给出的光标处（客户区坐标）。
    // allowContextReset = 组字刚开始时重挂一次 IME 上下文 —— 不重挂的话输入法
    // 会沿用上一次建好的 UI，后续 ImmSetCompositionWindow 返回成功却不生效。
    bool applyImePlacement(HWND hwnd, bool allowContextReset);

    int width_ = 0, height_ = 0;
    bool vsync_ = true;
    bool gdiPresent_ = false; // resize 拖拽期：present 走 GDI（读回离屏 → StretchDIBits），不 SwapBuffers
    // 客户区盖满整块显示器时也要走 GDI 贴屏：全屏直翻转会让驱动/远程显示器拿不到新帧
    // （应用还在 100fps 画，屏幕上却是旧画面）。由 applyPendingResize 重算。
    bool fullscreenPresent_ = false;
    bool clientCoversMonitor() const;
    BinUIGLGdi gfx_;
    void* hwnd_ = nullptr;

    bool resizable_ = true;
    float aspect_ = 0;       // 等比 w/h（0 = 自由）
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
    CursorFn cursor_;
    DoubleClickFn dblclick_;
    ImeCaretFn imeCaret_;
    ImeActiveFn imeActive_;        // 宿主说"现在要不要输入法"
    bool imeAttached_ = true;      // 当前 IME 上下文是否挂在窗口上
    // 输入法正在组字：这期间的按键/字符归 IME，不能拿去改我们自己的文本
    bool composing_ = false;
    // 组字结束时刻（ms）。组字刚结束时输入法放行的那个 Esc 是"取消候选"，
    // 窗口吞掉一次，避免连编辑态一起退掉（见 WM_KEYDOWN 处注释）。
    double composeEndMs_ = -1e9;
    // 上一次摆 IME UI 用的客户区坐标（-1 = 还没摆过）。位置没变就不重复摆，
    // 免得每次按键都惊动输入法 UI。
    int imePosX_ = -1, imePosY_ = -1;

    bool running_ = false;
    bool childMode_ = false;  // true = WS_CHILD（宿主窗口里的控件），不走自己的消息循环
    // 建窗线程（Win32 窗口 + WGL 上下文都有线程亲和）。open()/openChild() 时记下，
    // pump()/renderFrame() 每次比对；0 = 还没建窗。
    unsigned long ownerThread_ = 0;
    std::chrono::steady_clock::time_point lastTime_;  // pump() 的帧间隔基准
    // 帧节流（自节拍）：按显示器刷新率出帧，其余时间睡觉。
    // 不节流的话，只要有动画/悬停这一类"每帧都有脏区"的状态，就会一直满速出帧；
    // GL 的 SwapBuffers 在垂直同步上会让驱动自旋等待（实测每帧 6~7ms CPU），
    // 空闲界面也能吃掉 80% 单核。见 renderFrame()/pump() 里注释。
    std::chrono::steady_clock::time_point nextFrameTime_{};
    std::chrono::microseconds frameBudget_{16667};   // 1000000 / 刷新率（默认 60Hz）
    void requestFrameNow() { nextFrameTime_ = std::chrono::steady_clock::time_point{}; }
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
