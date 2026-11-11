// binui_app.h - 无边框窗口 + 事件循环（与后端无关，模板化在 Gfx 上）
//
// 这份代码原来长在 binui_gl-gdi.cpp 里，叫 BinUIAppGLGdi。P5 要做 GPU=gl|vk 时发现：
// 整个外壳只用到后端的 8 个方法（init/shutdown/resize/beginFrame/present/presentViaGdi/
// setTransparent/isReady），跟 GL 一点关系都没有 —— 那就模板化，让 GL 与 Vulkan 共用
// 同一份窗口/输入/IME/拖拽/节流逻辑，两个后端不会在"拖边框"这种边角上分叉。
//
// 后端只需要提供：
//   · IBinUIGfx 的 8 个方法（见上）
//   · static const wchar_t* appClassName()  —— 窗口类名（同进程两个后端并存时不冲突）
//
// 用法（两个后端完全一样）：
//   BinUIAppVK app(960, 660);          // 或 BinUIAppGLGdi
//   app.setDrawFn([&](Gfx& g, const void* clip) { drawUi(g, renderer, clip); });
//   app.run();
//
// 线程亲和不变：窗口 + GPU 上下文都属于建窗线程，pump/renderFrame 会显式拦截跨线程调用。
#ifndef BINUI_APP_H
#define BINUI_APP_H

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "binui_gfx.h"   // IBinUIGfx：draw 回调只认接口，不认具体后端

#include "binui_renderer.h"
#include "platform/binui_win32_foreground.h"   // BinUIForceForeground：把窗口提到前台

namespace binui {

// 是否远程会话（RDP / 虚拟显示器）：这种环境下 swapchain 直翻转会停帧
// （GL 0.9.1 与 Vulkan 后端都实测过），**三个后端**据此默认走读回 + GDI 贴屏。
// BINUI_FORCE_REMOTE=1 可在本机模拟（本机没有 RDP 会话，否则这条分支无法验收）。
// 放在命名空间而不是模板静态里：后端（binui_gl-gdi / binui_vk / binui_d3d11）要调它。
inline bool RemoteSession() {
    static const bool remote = [] {
        const char* e = std::getenv("BINUI_FORCE_REMOTE");
        if (e && *e && std::strcmp(e, "0") != 0) return true;
        return GetSystemMetrics(SM_REMOTESESSION) != 0;
    }();
    return remote;
}

// ============================================================
// 帧耗时统计（BINUI_PERF=1 → 每 60 帧往 stderr 打一行）
// ------------------------------------------------------------
// 拖边框重排/重绘的性能验收靠它：avg/max 帧耗 + 分段（重排 / 绘制 / 呈现）。
// 只统计真正出帧的循环；没开环境变量时只有一个 bool 判断，零额外开销。
// 从 GL 后端搬到外壳：GL 与 Vulkan 共用同一份统计，BINUI_PERF=1 两边都能读。
// ============================================================
struct FramePerf {
    bool   on      = false;
    bool   inited  = false;
    double sumResize = 0, sumDraw = 0, sumPresent = 0;
    double sumTotal = 0, maxTotal = 0, maxPresent = 0, maxResize = 0;
    double sumRead = 0, sumBlit = 0;   // presentViaGdi 内部两段（GPU 读回 / GDI 贴屏）
    double sumPrep = 0, sumDib = 0;    // presentViaGdi：MSAA 解析 / DIB 重建
    double sumUpdate = 0;              // update_（动画/热重载轮询）耗时
    double sumCtx = 0;                 // 上下文切换（可能被驱动阻塞）耗时
    // 帧间隔（相邻两帧"出帧时刻"之差）：avg/max 之外还要看**掉拍**——
    // 平均帧率够但个别间隔翻倍，肉眼就是"微卡"。掉拍 = 间隔超过 1.5 个预算。
    double lastFrameMs = 0;
    double maxInterval = 0;
    long long missed = 0;
    // 帧间隔直方图（1ms 一档，0..19，20+ 归末档）：平均值看不出"两张快、一张慢"的
    // 拍子，直方图一眼就能看出来。缩放类动画对这种拍子最敏感（形状每帧都在变）。
    long long hist[21] = {};
    double winStart = 0;               // 本统计窗口的起始时刻（算 fps）
    int    frames  = 0;
    long long drawn = 0;               // 累计真正绘制的帧数（不随窗口重置）
};

inline FramePerf& Perf() {
    static FramePerf p;
    if (!p.inited) {
        p.inited = true;
        const char* e = std::getenv("BINUI_PERF");
        p.on = (e && *e && std::strcmp(e, "0") != 0);
    }
    return p;
}

inline double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline void PerfFlush() {
    FramePerf& p = Perf();
    if (!p.on || p.frames == 0) return;
    const double span = NowMs() - p.winStart;
    std::fprintf(stderr,
                 "[perf] %3d frames  %.1f fps  total avg=%.2f max=%.2f | 重排 avg=%.2f max=%.2f |"
                 " update avg=%.2f | 绘制 avg=%.2f | 呈现 avg=%.2f max=%.2f (含上下文切换 %.2f; 解析 %.2f DIB %.2f 读回 %.2f 贴屏 %.2f) | 间隔 max=%.2f 掉拍=%lld\n",
                 p.frames, span > 0 ? p.frames * 1000.0 / span : 0.0,
                 p.sumTotal / p.frames, p.maxTotal,
                 p.sumResize / p.frames, p.maxResize,
                 p.sumUpdate / p.frames,
                 p.sumDraw / p.frames,
                 p.sumPresent / p.frames, p.maxPresent,
                 p.sumCtx / p.frames,
                 p.sumPrep / p.frames, p.sumDib / p.frames,
                 p.sumRead / p.frames, p.sumBlit / p.frames,
                 p.maxInterval, p.missed);
    p.sumResize = p.sumDraw = p.sumPresent = p.sumTotal = 0;
    p.maxTotal = p.maxPresent = p.maxResize = 0;
    p.maxInterval = 0;
    p.missed = 0;
    {   // 直方图：一行紧凑打印，0ms..19ms、20ms+
        char line[512];
        int o = 0;
        for (int i = 0; i < 21 && o < 480; i++) {
            if (p.hist[i] == 0) continue;
            o += std::snprintf(line + o, sizeof(line) - static_cast<size_t>(o), "%s%dms:%lld",
                               o ? " " : "", i, p.hist[i]);
            p.hist[i] = 0;
        }
        std::fprintf(stderr, "[perf] 间隔直方图 %s\n", o ? line : "(无)");
    }
    p.sumRead = p.sumBlit = p.sumPrep = p.sumDib = p.sumUpdate = p.sumCtx = 0;
    p.frames = 0;
    p.winStart = NowMs();
    std::fflush(stderr);   // stderr 被重定向到文件时是带缓冲的：不刷就看不到行尾
}

// 显示器刷新率（拿不到就按 60Hz；>360 视为驱动乱报）。
// 帧节流（frameBudget_）和拖拽循环都用它当预算。
inline int DisplayRefreshHzFor(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    const int hz = dc ? GetDeviceCaps(dc, VREFRESH) : 0;
    if (dc) ReleaseDC(hwnd, dc);
    if (hz <= 1 || hz > 360) return 60;
    return hz;
}


// UTF-8 → UTF-16（光标定位与 IME 组字坐标都按 UTF-16 单位算，与 Win32 一致）
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// 点击定位输入框光标（基于字形 advance 测量，两个后端同一份）。
// 把点击位置换算成光标位置并落点；extend = 拖选（只挪 caret，选区起点不动）。
// 返回换算出来的 UTF-16 索引（双击选词要用同一个换算结果）。
template <class Gfx>
inline size_t placeCaretAtClick(BinUIRenderer& renderer, Gfx& gfx, float cx, float cy,
                                bool extend = false) {
    if (!renderer.isEditing()) return 0;
    const auto& edit = renderer.edit();

    const auto& nodes = renderer.getNodes();
    const binui::BinUIRenderer::NodeInfo* node = nullptr;
    for (const auto& n : nodes) {
        if (n.id == edit.nodeId) { node = &n; break; }
    }
    if (!node || node->type != BINUI_INPUT) return 0;
    if (cy < node->y || cy > node->y + node->height) return 0;

    std::wstring wtext = Utf8ToWide(edit.value);
    float textX = static_cast<float>(node->x) + node->padding + 4;
    float target = cx - textX;
    int fs = node->fontSize ? node->fontSize : 14;
    if (target <= 0) { renderer.setCaretByUtf16Index(0, extend); return 0; }

    // 累计前缀宽度（UTF-16 单位），找越过点击点的位置
    std::string prefix;
    size_t idx = 0;
    float w = 0;
    for (size_t i = 0; i < wtext.size(); i++) {
        unsigned cp = wtext[i];
        std::string ch;
        if (cp < 0x80) ch += static_cast<char>(cp);
        else if (cp < 0x800) {
            ch += static_cast<char>(0xC0 | (cp >> 6));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            ch += static_cast<char>(0xE0 | (cp >> 12));
            ch += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            ch += static_cast<char>(0xF0 | (cp >> 18));
            ch += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            ch += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
        }
        float cw = gfx.textWidth(static_cast<float>(fs), 400, prefix + ch) - w;
        if (w + cw / 2 >= target) break;
        w += cw;
        prefix += ch;
        idx = i + 1;
    }
    renderer.setCaretByUtf16Index(idx, extend);
    return idx;
}

// 输入法候选窗定位：把组字窗/候选框摆到当前编辑框的光标处（客户区坐标）。
// 模板在 Gfx 上只是为了拿 textWidth/fontMetrics —— 两个后端的文本度量本来就是同一份。
template <class Gfx>
bool imeCaretFromRenderer(const BinUIRenderer& renderer, Gfx& gfx, int& x, int& y, int& lineH);

// 光标闪烁相位（500ms 周期），与 GDI/D3D 后端共用语义
inline bool caretBlinkOn();

// 判断点击是否落在"标题栏"（窗口顶部通栏容器，非按钮/输入框）：
// 应用在 press 回调中先判此函数，命中则调用 app.beginWindowDrag()
inline bool isTitlebarDrag(const BinUIRenderer& renderer, float x, float y);

// 无边框（WS_POPUP）应用外壳：GPU 后端 + 事件循环。
// Gfx 必须是实现了 IBinUIGfx 且带 appClassName() 的后端（BinUIGLGdi / BinUIVK）。
template <class Gfx>
class BinUIAppT {
public:
    using UpdateFn  = std::function<void(float dt)>;
    // 绘制回调只认**接口**（IBinUIGfx）而不是具体后端：这样同一份 draw 代码既能被
    // GL / Vulkan / D3D11 三个编译期实例复用，将来做运行期后端选择（BINUI_GPU）时
    // 也不必再改一遍调用方。见 docs/vulkan_design.md §7.5。
    using DrawFn    = std::function<void(IBinUIGfx&, const void* clipRect)>;
    using MoveFn    = std::function<void(float x, float y)>;
    using PressFn   = std::function<void(float x, float y)>;
    using ReleaseFn = std::function<void(float x, float y)>;
    using LeaveFn   = std::function<void()>;
    using ResizeFn  = std::function<void(int w, int h)>;
    // DPI 变化（WM_DPICHANGED，或 BINUI_FORCE_DPI 模拟）：参数是新的 DPI（96 = 100%）
    using DpiFn     = std::function<void(int dpi)>;
    using CharFn    = std::function<void(const std::string& utf8)>;
    using KeyFn     = std::function<void(int vk)>;
    using WheelFn   = std::function<void(float x, float y, float dy)>;
    using FocusFn   = std::function<void(bool focused)>;
    // 光标查询：给定客户区坐标，返回 BinUICursorKind（0=箭头 1=手型 2=I 型 3=竖直 4=水平）。
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
    using ImeActiveFn = std::function<bool()>;

    BinUIAppT(int width, int height, bool vsync = true);
    ~BinUIAppT();

    // 窗口选项（<canvas> 描述）：resizable / aspect(w:h 等比) / transparent / title
    void setWindowOptions(bool resizable, float aspect, bool transparent, const char* title);

    void setUpdateFn(UpdateFn fn)   { update_ = std::move(fn); }
    void setDrawFn(DrawFn fn)       { draw_ = std::move(fn); }
    void setMoveFn(MoveFn fn)       { move_ = std::move(fn); }
    void setPressFn(PressFn fn)     { press_ = std::move(fn); }
    void setReleaseFn(ReleaseFn fn) { release_ = std::move(fn); }
    void setLeaveFn(LeaveFn fn)     { leave_ = std::move(fn); }
    void setResizeFn(ResizeFn fn)   { resize_ = std::move(fn); }
    void setDpiChangedFn(DpiFn fn)  { dpiChanged_ = std::move(fn); }
    // 当前 DPI（96 = 100%）。BINUI_FORCE_DPI=120|144|168 可在一台单显示器机器上
    // 模拟换屏后的 DPI，用来验证这条路径（见 dpiOf）。
    int dpi() const { return dpi_; }
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
    void maximize();   // 最大化 / 还原（无边框自绘，直接走 ShowWindow）

    Gfx& gfx() { return gfx_; }

    int run(); // 创建无边框窗口 + GPU 设备 + 事件循环

    // ---------- 非阻塞外壳（给 C ABI / 其他语言绑定 / 自管循环的宿主用）----------
    // run() 完全等价于 open(); while (pump(1) == 0) {} close(); 三者的行为逐帧一致，
    // 只是把"谁拥有消息循环"交出去：宿主自己决定每帧什么时候推进。
    bool open();              // 建窗口 + GPU 设备（失败返回 false）
    // 推进一帧：收消息 → update → 绘制脏区。
    // 返回 0 = 继续；1 = 已关闭；**-1 = 调用线程不是建窗线程**（宿主写错了，别再继续）。
    // 窗口与 GPU 上下文都有线程亲和：从别的线程进来不会立刻炸，而是随机崩或退出时挂死，
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
    // 跨显示器 DPI（预留接口）：应用系统建议的窗口矩形 + 记录 DPI + 回调宿主
    void handleDpiChanged(int newDpi, const void* suggestedRect);
    // 取当前 DPI（BINUI_FORCE_DPI 可覆盖，用于单显示器模拟换屏）
    static int dpiOf(HWND hwnd);
    // IME UI 定位：把组字窗/候选框摆到宿主给出的光标处（客户区坐标）。
    // allowContextReset = 组字刚开始时重挂一次 IME 上下文 —— 不重挂的话输入法
    // 会沿用上一次建好的 UI，后续 ImmSetCompositionWindow 返回成功却不生效。
    bool applyImePlacement(HWND hwnd, bool allowContextReset);

    int width_ = 0, height_ = 0;
    bool vsync_ = true;
    bool gdiPresent_ = false; // resize 拖拽期：present 走 GDI（读回离屏 → StretchDIBits），不提交交换链
    // 客户区盖满整块显示器时也要走 GDI 贴屏：全屏直翻转会让驱动/远程显示器拿不到新帧
    //（应用还在 100fps 画，屏幕上却是旧画面）。由 applyPendingResize 重算。
    bool fullscreenPresent_ = false;
    bool clientCoversMonitor() const;
    Gfx gfx_;
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
    DpiFn dpiChanged_;
    int dpi_ = 96;
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
    // 建窗线程（Win32 窗口 + GPU 上下文都有线程亲和）。open()/openChild() 时记下，
    // pump()/renderFrame() 每次比对；0 = 还没建窗。
    unsigned long ownerThread_ = 0;
    std::chrono::steady_clock::time_point lastTime_;  // pump() 的帧间隔基准
    // 帧节流（自节拍）：按显示器刷新率出帧，其余时间睡觉。
    // 不节流的话，只要有动画/悬停这一类"每帧都有脏区"的状态，就会一直满速出帧；
    // 垂直同步下驱动会自旋等待（实测每帧 6~7ms CPU），空闲界面也能吃掉 80% 单核。
    std::chrono::steady_clock::time_point nextFrameTime_{};
    std::chrono::microseconds frameBudget_{16667};   // 1000000 / 刷新率（默认 60Hz）
    void requestFrameNow() { nextFrameTime_ = std::chrono::steady_clock::time_point{}; }
    // 节拍由 present() 的垂直同步阻塞决定（vsync 交换链），此时不再自己掐帧
    bool pacedByPresent() const;
    // 睡到指定时刻（保留消息可唤醒；用高精度定时器，见实现里的注释）
    void waitForFrame(std::chrono::steady_clock::time_point t);
    void* frameTimer_ = nullptr;   // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 的定时器句柄
    bool dirty_ = true;
    int dirtyL_ = 0, dirtyT_ = 0, dirtyR_ = 0, dirtyB_ = 0;
    int pendingW_ = 0, pendingH_ = 0; // 挂起的窗口尺寸（WM_SIZE 只记录，帧内应用一次）
    bool trackingLeave_ = false;
    wchar_t pendingHigh_ = 0;

    void applyPendingResize(); // 应用挂起 resize：渲染器重排 + gfx 重建 + 全量重绘
    void renderOnce();         // 应用挂起 resize 后绘制一帧并呈现
    void startResize(int hit); // 手动 resize 拖拽循环（每轮同步渲染）
};

// ============================================================
// 以下：辅助函数与实现（P5 从 binui_gl-gdi.cpp 整块搬来，模板化在 Gfx 上）
// ============================================================

template <class Gfx>
inline bool imeCaretFromRenderer(const BinUIRenderer& renderer, Gfx& gfx,
                          int& x, int& y, int& lineH) {
    if (!renderer.isEditing()) return false;
    const auto& edit = renderer.edit();

    const binui::BinUIRenderer::NodeInfo* node = nullptr;
    for (const auto& n : renderer.getNodes()) {
        if (n.id == edit.nodeId) { node = &n; break; }
    }
    if (!node) return false;

    const float fs = static_cast<float>(node->fontSize ? node->fontSize : 14);
    int asc = 0, lh = 0;
    gfx.fontMetrics(fs, 400, asc, lh);
    if (lh <= 0) lh = static_cast<int>(fs) + 2;

    // 光标位置是 UTF-8 字节偏移；有选区时组字从选区左端开始（与绘制光标同一口径）
    size_t caret = edit.caret < edit.selStart ? edit.caret : edit.selStart;
    if (caret > edit.value.size()) caret = edit.value.size();
    const float tw = gfx.textWidth(fs, 400, edit.value.substr(0, caret));

    x = static_cast<int>(static_cast<float>(node->x) + static_cast<float>(node->padding) + 4.0f + tw);
    y = static_cast<int>(static_cast<float>(node->y) +
                         (static_cast<float>(node->height) - static_cast<float>(lh)) * 0.5f);
    lineH = lh;
    return true;
}

// ============================================================
// 光标闪烁
// ============================================================
inline bool caretBlinkOn() {
    using namespace std::chrono;
    static auto last = steady_clock::now();
    static bool on = true;
    auto now = steady_clock::now();
    if (duration_cast<milliseconds>(now - last).count() >= 500) {
        on = !on;
        last = now;
    }
    return on;
}

// 标题栏判定（与 GDI/D3D 后端语义一致）
inline bool isTitlebarDrag(const BinUIRenderer& renderer, float x, float y) {
    const auto& nodes = renderer.getNodes();
    if (renderer.hitTest(x, y) >= 0) return false;
    for (const auto& n : nodes)
        if ((n.flags & BINUI_TITLEBAR) && (n.flags & BINUI_VISIBLE) &&
            x >= n.x && x <= n.x + n.width && y >= n.y && y <= n.y + n.height)
            return true;
    return false;
}


template <class Gfx>
inline void BinUIAppT<Gfx>::beginWindowDrag() {
    if (!hwnd_ || childMode_) return;   // 子窗口没有"拖动整窗"，那是宿主的标题栏生意
    ReleaseCapture();
    POINT p;
    GetCursorPos(&p);
    SendMessageA(static_cast<HWND>(hwnd_), WM_NCLBUTTONDOWN, HTCAPTION,
                 MAKELPARAM(p.x, p.y));
}

template <class Gfx>
inline void BinUIAppT<Gfx>::minimize() {
    if (hwnd_ && !childMode_) ShowWindow(static_cast<HWND>(hwnd_), SW_MINIMIZE);
}

template <class Gfx>
inline void BinUIAppT<Gfx>::maximize() {
    if (!hwnd_ || childMode_) return;
    const HWND h = static_cast<HWND>(hwnd_);
    ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
}

// 手动 resize 拖拽循环（窗口无 WS_THICKFRAME，系统不提供 resize，这里自行实现）。
// 拖拽期间每轮实时渲染到新尺寸离屏 + GDI blit 到窗口（实时重绘、跟手）。
// 关键：拖拽期间绝不能 SwapBuffers——NVIDIA 驱动在"窗口 resize + SwapBuffers"组合下
// 必然崩溃（nvoglv64 DrvPresentBuffers），已实测 vsync 开关/上下文 detach/节流呈现
// 均无法规避；走 presentViaGdi（读回离屏像素 → 贴到窗口 DC）完全绕开驱动 present 路径。
// 松手时最后再以 GDI 呈现一次清晰帧，然后恢复正常 SwapBuffers（surface 已稳定）。
//
// 帧率：按显示器刷新率出帧（浏览器拖窗口时内容也是按刷新率重绘的）。
// 以前这里是"每轮循环都重排+整屏呈现 + Sleep(1)"：一次拖拽每秒要跑几百帧，
// CPU 打满、消息队列堆在后面，表现反而是一顿一顿的。节流之后每帧有完整的
// 16.7ms（60Hz）预算，实测单帧 5~7ms，留足余量。
// 同时**每帧都调 update_**：拖拽期间元素动画不许停（以前动画帧只在 renderFrame 里走，
// 拖边时 update_ 被跳过，动画会冻住）。
template <class Gfx>
inline void BinUIAppT<Gfx>::startResize(int hit) {
    const int MIN_W = 320, MIN_H = 240;
    HWND hwnd = static_cast<HWND>(hwnd_);
    SetCapture(hwnd);
    POINT start;
    GetCursorPos(&start);
    RECT startRect;
    GetWindowRect(hwnd, &startRect);
    gdiPresent_ = true;

    // 出帧节拍跟 renderFrame() 用同一个时钟（nextFrameTime_），
    // 这样拖拽期和平时是同一套节奏，不会两个节流器互相打架。
    RECT applied = startRect;
    const bool perfOn = Perf().on;
    const double dragT0 = perfOn ? NowMs() : 0.0;
    int dragFrames = 0;
    double dragFrameSum = 0.0, dragFrameMax = 0.0;
    if (perfOn) std::fprintf(stderr, "[perf] drag start: budget=%.2fms\n",
                            frameBudget_.count() / 1000.0);

    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        POINT cur;
        GetCursorPos(&cur);
        RECT r = startRect;
        int dx = cur.x - start.x, dy = cur.y - start.y;
        switch (hit) {
            case HTLEFT:          r.left += dx; break;
            case HTRIGHT:         r.right += dx; break;
            case HTTOP:           r.top += dy; break;
            case HTBOTTOM:        r.bottom += dy; break;
            case HTTOPLEFT:       r.left += dx; r.top += dy; break;
            case HTTOPRIGHT:      r.right += dx; r.top += dy; break;
            case HTBOTTOMLEFT:    r.left += dx; r.bottom += dy; break;
            case HTBOTTOMRIGHT:   r.right += dx; r.bottom += dy; break;
        }
        if (r.right - r.left < MIN_W) {
            if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT)
                r.left = r.right - MIN_W;
            else
                r.right = r.left + MIN_W;
        }
        if (r.bottom - r.top < MIN_H) {
            if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT)
                r.top = r.bottom - MIN_H;
            else
                r.bottom = r.top + MIN_H;
        }
        // 等比缩放（<canvas aspect="...">）：以主导边为基准约束另一边
        if (aspect_ > 0) {
            LONG wd = r.right - r.left, ht = r.bottom - r.top;
            if (hit == HTTOP || hit == HTBOTTOM) {
                wd = static_cast<LONG>(ht * aspect_ + 0.5f);
                if (hit == HTTOP) r.top = r.bottom - ht;
                else r.bottom = r.top + ht;
            } else {
                ht = static_cast<LONG>(wd / aspect_ + 0.5f);
                if (hit == HTTOPLEFT || hit == HTTOP || hit == HTTOPRIGHT)
                    r.top = r.bottom - ht;
                else
                    r.bottom = r.top + ht;
            }
            if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT)
                r.left = r.right - wd;
            else
                r.right = r.left + wd;
        }
        // 窗口边框立刻跟手（SetWindowPos 很便宜），内容按刷新率出帧
        if (r.left != applied.left || r.top != applied.top ||
            r.right != applied.right || r.bottom != applied.bottom) {
            SetWindowPos(hwnd, nullptr, r.left, r.top,
                r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            applied = r;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextFrameTime_) {
            const double f0 = perfOn ? NowMs() : 0.0;
            renderFrame();       // update_（动画）+ applyPendingResize + 一帧呈现（内部按同一时钟节流）
            if (perfOn) {
                const double dt = NowMs() - f0;
                ++dragFrames;
                dragFrameSum += dt;
                if (dt > dragFrameMax) dragFrameMax = dt;
            }
        } else {
            // 等到下一帧点或新输入（睡眠，不空转）。
            // 这里也走 waitForFrame：毫秒级的 MsgWait 超时会被系统计时器精度（15.6ms）
            // 兑现，拖拽期同样会出现"一帧跨两个刷新"的卡顿。
            waitForFrame(nextFrameTime_);
        }
    }
    // 松手：应用最终尺寸并以 GDI 呈现清晰帧（surface 可能仍未稳定，不 SwapBuffers）
    applyPendingResize();
    renderFrame();
    gdiPresent_ = false; // 此后动画帧恢复正常 SwapBuffers（surface 已稳定）
    ReleaseCapture();
    if (perfOn) {
        const double span = NowMs() - dragT0;
        std::fprintf(stderr, "[perf] drag end: %d frames / %.0fms = %.1f fps | 帧耗时 avg=%.2f max=%.2f ms\n",
                     dragFrames, span,
                     span > 0 ? dragFrames * 1000.0 / span : 0.0,
                     dragFrames ? dragFrameSum / dragFrames : 0.0, dragFrameMax);
        std::fflush(stderr);
    }
}

// ============================================================
// BinUIAppT：无边框窗口 + 事件循环
// ============================================================
// IME/按键消息日志：设了 BINUI_IME_LOG=1 就把窗口收到的消息写进 binui_ime.log。
// 输入法的问题几乎只能靠"看消息序列"定位 —— 谁是组字、谁把键吃了、候选窗什么时候该收，
// 都藏在消息顺序里。默认关（环境变量不设就一行都不写）。
inline void ImeLog(const char* fmt, ...) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("BINUI_IME_LOG");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    if (!on) return;
    FILE* f = std::fopen("binui_ime.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

// 输入法当前是否正在组字。
//
// 用 IME 上下文实时问，不要靠 WM_IME_START/ENDCOMPOSITION 的成对计数 ——
// 组字被打断时 END 可能不来，标志位一挂死，之后所有按键就再也进不了编辑器。
static bool ImeComposing(HWND hwnd) {
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return false;
    const LONG n = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(hwnd, himc);
    return n > 0;
}

// 把输入法的组字窗与候选框摆到光标处（客户区坐标），让它们贴着光标走。
//
// 这里有两个坑，都在 Win11 + 微软拼音 + 自绘窗口（Cicero-unaware）上实测过：
//
// 1. **不摆就是坏的**：输入法拿不到光标位置，只能把组字窗放到默认位置 —— 实测
//    落在窗口外（窗口在屏幕 (900,376) 时组字窗跑到 (1030,151)）。现象是"打拼音
//    屏幕上什么都没有"，但选词照样能上屏，特别容易误判成输入法没工作。
//
// 2. **只摆一次也不够**：ImmSetCompositionWindow / ImmSetCandidateWindow 只在
//    输入法第一次为这个窗口建 UI 时被采纳，之后都返回 TRUE 却不生效。从第一个
//    输入框切到第二个输入框、或在同一个框里接着打下一个词时，组字串画对了、
//    候选框还赖在旧位置。解决办法是把 IME 上下文重挂一次（摘掉再挂回），输入法
//    就会丢掉旧 UI，下一次 ImmSet* 才重新被采纳。
//
// 重挂上下文只在**组字开始**时做：组字中途重挂会把正在进行的组字弄断。
// 位置没变时直接跳过（每次按键都重设 IME UI 会让它闪）。
template <class Gfx>
inline bool BinUIAppT<Gfx>::applyImePlacement(HWND hwnd, bool allowContextReset) {
    if (!hwnd || !imeCaret_) return false;
    int cx = 0, cy = 0, ch = 16;
    if (!imeCaret_(cx, cy, ch)) return false;

    const bool moved = (cx != imePosX_ || cy != imePosY_);
    if (!moved) return true;

    ImeLog("  ime place (%d,%d) h=%d reset=%d", cx, cy, ch, allowContextReset ? 1 : 0);

    if (allowContextReset) {
        if (HIMC prev = ImmAssociateContext(hwnd, nullptr)) ImmAssociateContext(hwnd, prev);
    }

    imePosX_ = cx;
    imePosY_ = cy;

    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return false;

    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = cx;
    cf.ptCurrentPos.y = cy;
    ImmSetCompositionWindow(himc, &cf);

    // 候选框也显式摆一次（稳健起见：有的输入法认这条，微软拼音的候选框实际跟着
    // 组字窗走，所以上面那条才是关键）。
    CANDIDATEFORM cdf = {};
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos.x = cx;
    cdf.ptCurrentPos.y = cy + (ch > 0 ? ch : 16);   // 列在光标下方一行
    ImmSetCandidateWindow(himc, &cdf);

    // 组字字体跟随输入框行高：不设的话系统用默认 UI 字体，13px 的输入框里会冒出
    // 一行明显更大的组字文本，看着像"位置不对"。
    LOGFONTW lf = {};
    lf.lfHeight = -(ch > 0 ? ch : 16);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
    ImmSetCompositionFontW(himc, &lf);

    ImmReleaseContext(hwnd, himc);
    return true;
}

// 光标形状缓存：IDC_* 是系统共享资源，LoadCursor 只想加载一次
static HCURSOR CursorHandleOf(int kind) {
    static HCURSOR cache[5] = {};
    if (kind < 0 || kind > 4) kind = 0;
    if (!cache[kind]) {
        LPCWSTR id = reinterpret_cast<LPCWSTR>(IDC_ARROW);
        switch (kind) {
            case 1: id = reinterpret_cast<LPCWSTR>(IDC_HAND);   break;  // 可交互（按钮等）
            case 2: id = reinterpret_cast<LPCWSTR>(IDC_IBEAM);  break;  // 输入框
            case 3: id = reinterpret_cast<LPCWSTR>(IDC_SIZENS); break;  // 竖直滚动条
            case 4: id = reinterpret_cast<LPCWSTR>(IDC_SIZEWE); break;  // 水平滚动条
            default: break;
        }
        cache[kind] = LoadCursorW(nullptr, id);
    }
    return cache[kind];
}

template <class Gfx>
inline LRESULT CALLBACK BinUIAppT<Gfx>::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BinUIAppT* app = reinterpret_cast<BinUIAppT*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_IME_ENDCOMPOSITION: {
            app->composing_ = false;
            app->composeEndMs_ = NowMs();   // 记时刻：紧跟其后的 Esc 是"取消候选"，不是"退出编辑"
            ImeLog("msg=WM_IME_ENDCOMPOSITION");
            // 交给 DefWindowProc 收尾。
            //
            // 以前这里记着一条"候选窗取消组字后留在屏幕上不走，四种手段都推不动"
            // 的结论 —— 那是被另一个问题盖住了：组字窗/候选框的位置从来没跟着光标
            // 走（见 applyImePlacement），输入法一直沿用第一次建好的 UI，看起来就像
            // "候选窗赖着不走"。位置能跟上之后，提交/取消都会正常收起。
            //
            // 真正修不掉的是"组字期间点击被输入法吃掉"：微软拼音遇到鼠标点击会先把
            // 拼音当结果串提交掉，那一次点击不会送到窗口（实测连点"退出"按钮都没
            // 反应）。要让点击不被吃，得给窗口实现一套 TSF 文本存储（ITextStoreACP），
            // 那已经不是这个外壳该管的事了。
            break;
        }
        case WM_IME_NOTIFY:
            // wParam 是 IMN_*：关闭候选窗是 IMN_CLOSECANDIDATE(0x4)，
            // 打开是 IMN_OPENCANDIDATE(0x5)。候选窗的生死全在这条消息里。
            ImeLog("msg=WM_IME_NOTIFY imn=0x%04X lp=0x%zX",
                   static_cast<unsigned>(wParam), static_cast<size_t>(lParam));
            break;
        case WM_IME_SETCONTEXT:
        case WM_IME_REQUEST:
            ImeLog("msg=0x%04X wp=0x%zX lp=0x%zX",
                   static_cast<unsigned>(msg),
                   static_cast<size_t>(wParam), static_cast<size_t>(lParam));
            break;
        default:
            break;
    }

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!app->trackingLeave_) {
                POINT cursor;
                RECT wrect;
                if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &wrect) && PtInRect(&wrect, cursor)) {
                    TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    app->trackingLeave_ = true;
                }
            }
            app->handleMouseMove(static_cast<float>(LOWORD(lParam)),
                                 static_cast<float>(HIWORD(lParam)));
            return 0;
        }
        case WM_LBUTTONDOWN:
            app->handleMousePress(static_cast<float>(LOWORD(lParam)),
                                  static_cast<float>(HIWORD(lParam)));
            SetFocus(hwnd);
            return 0;
        case WM_LBUTTONDBLCLK:
            // 第二次按下走的是这条（第一次仍是 WM_LBUTTONDOWN）。先按一次"按下"把
            // 焦点/光标放好，再交给宿主做"选词"。
            app->handleMousePress(static_cast<float>(LOWORD(lParam)),
                                  static_cast<float>(HIWORD(lParam)));
            if (app->dblclick_)
                app->dblclick_(static_cast<float>(LOWORD(lParam)),
                               static_cast<float>(HIWORD(lParam)));
            SetFocus(hwnd);
            return 0;
        case WM_LBUTTONUP:
            app->handleMouseRelease(static_cast<float>(LOWORD(lParam)),
                                    static_cast<float>(HIWORD(lParam)));
            return 0;
        case WM_MOUSELEAVE:
            app->trackingLeave_ = false;
            app->handleMouseLeave();
            return 0;
        case WM_MOUSEWHEEL: {
            short delta = static_cast<short>(HIWORD(wParam));
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            app->handleMouseWheel(static_cast<float>(pt.x), static_cast<float>(pt.y),
                                  static_cast<float>(delta) / 120.0f * 40.0f);
            return 0;
        }
        case WM_CHAR:
            app->handleChar(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            // 组字期间的回车/空格/退格/Esc 都是**输入法在用**，我们一个都不能接。
            // 接了就等于替用户取消组字：IME 发现会话没了，会把还没转换的拼音串
            // 原样当结果串提交（输 zhongguo 按回车 → 输入框里躺着 "zhongguo"
            // 而不是"中国"，就是这么来的）。
            ImeLog("  keydown vk=%d composing=%d", static_cast<int>(wParam), (int)app->composing_);
            // 组字刚结束时输入法会把用户的 Esc 原样放行：这一下是"取消候选"，不是
            // "退出编辑"。吞掉它（只吞一次、且只吞刚结束组字那一小段时间内的），
            // 否则编辑态会连同候选窗一起被 Esc 退掉 —— 用户按 Esc 取消拼音，却把
            // 整个输入状态也丢了。再按一次 Esc 才退出编辑。
            if (wParam == VK_ESCAPE)
                ImeLog("  esc delta=%.1fms", NowMs() - app->composeEndMs_);
            if (wParam == VK_ESCAPE && NowMs() - app->composeEndMs_ < 200.0) {
                app->composeEndMs_ = -1e9;
                ImeLog("  keydown ESC (组字刚结束，这一下归输入法，吞掉)");
                return 0;
            }
            if (app->composing_ && ImeComposing(hwnd)) {
                ImeLog("  keydown vk=%d (组字中，不归我们处理)", static_cast<int>(wParam));
                break;   // 交给 DefWindowProc：IME 灌进来的 VK_PROCESSKEY 要在这里走完
            }
            app->handleKey(static_cast<int>(wParam));
            return 0;
        case WM_SETFOCUS:
            ImeLog("msg=SETFOCUS");
            break;
        case WM_ACTIVATE:
            ImeLog("msg=ACTIVATE wp=0x%zX", static_cast<size_t>(wParam));
            break;
        case WM_KILLFOCUS:
            ImeLog("msg=KILLFOCUS");
            // 失焦时把**没提交的组字**取消掉。不取消的话输入法会留着一个悬着的
            // 候选窗；而且我们的 blur() 已经把编辑会话结束了，残留的组字之后再提交
            // 就会落到"没有编辑中的输入框"，表现为"只进了第一个字 / 后面全丢"。
            // （切焦点测出来的那串怪现象，根子在宿主窗口被别的窗口抢了前台。）
            app->cancelImeComposition();
            app->handleFocusLost();
            return 0;
        case WM_SIZE:
            app->handleResize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_DPICHANGED:
            // 跨显示器 DPI（预留接口）：应用系统建议的矩形，再走 resize 路径重排。
            // 进程不是 per-monitor aware 时这个窗口消息根本不会来，所以变量必须带默认值。
            app->handleDpiChanged(HIWORD(wParam), reinterpret_cast<const void*>(lParam));
            return 0;
        case WM_NCLBUTTONDOWN: {
            // 无 WS_THICKFRAME：命中边框热区时自行启动 resize 循环
            // 子窗口没有非客户区，也没有"让系统带着拖动"的语义 —— 一律交给宿主
            if (app->childMode_ || !app->resizable_)
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            switch (wParam) {
                case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
                case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT:
                case HTBOTTOMRIGHT: {
                    if (IsZoomed(hwnd))
                        return DefWindowProcW(hwnd, msg, wParam, lParam);
                    app->startResize(static_cast<int>(wParam));
                    return 0;
                }
                default:
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        }
        case WM_NCHITTEST: {
            // 无边框窗口：边框 8px 热区用于调整大小，其余为客户区
            if (app->childMode_ || !app->resizable_) return HTCLIENT;
            RECT rc;
            GetWindowRect(hwnd, &rc);
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int bw = 8;
            bool l = pt.x - rc.left < bw, t = pt.y - rc.top < bw;
            bool r = rc.right - pt.x < bw, b = rc.bottom - pt.y < bw;
            if (l && t) return HTTOPLEFT;
            if (r && t) return HTTOPRIGHT;
            if (l && b) return HTBOTTOMLEFT;
            if (r && b) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
            return HTCLIENT;
        }
        case WM_IME_SETCONTEXT: {
            // 这一位**必须留着**：ISC_SHOWUICOMPOSITIONWINDOW 的含义是"允许输入法
            // 显示组字窗口"。以前这里把它清掉（注释还写反成"清掉才是让输入法自己画"），
            // 结果系统不画组字、我们也没自己画 —— 打拼音屏幕上什么都没有，只剩候选窗
            // 孤零零挂着（候选窗不受这一位控制，所以它能出来、拼音却出不来）。
            break;   // 原样交给 DefWindowProc
        }
        case WM_IME_STARTCOMPOSITION: {
            app->composing_ = true;
            // 组字开始：把组字窗/候选框摆到光标处（顺带重挂上下文，见 applyImePlacement）
            app->applyImePlacement(hwnd, /*allowContextReset=*/true);
            break;   // 组字内容仍然交给 DefWindowProc
        }
        case WM_IME_COMPOSITION: {
            // 结果串自己取，**不能**交给 DefWindowProc。
            //
            // 交给它的话，它会把结果串折成 WM_IME_CHAR，而那条消息的 wParam 是
            // **系统代码页里的单个字节**：一个汉字要被拆成两次送过来（0xE5 0x90
            // 这种），逐字节当宽字符插进去就是「å å å」；再叠上"每次按键都可能带
            // 一个孤立字节"的兼容行为，就表现为只进第一个字、后面全乱。
            //
            // GCS_RESULTSTR 是 UTF-16，取整串直接喂 handleChar 是唯一干净的路。
            if (lParam & GCS_RESULTSTR) {
                if (HIMC himc = ImmGetContext(hwnd)) {
                    const LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);
                    if (bytes > 0) {
                        std::wstring w(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                        ImmGetCompositionStringW(himc, GCS_RESULTSTR, &w[0], bytes);
                        ImeLog("  resultstr bytes=%ld cps=%zu", bytes, w.size());
                        for (wchar_t ch : w) app->handleChar(ch);
                    }
                    ImmReleaseContext(hwnd, himc);
                }
                // 结果串自己取、自己插，但**仍然交给 DefWindowProc**：让它把 IME
                // 的收尾流程走完。它随后会为结果串逐字符发 WM_IME_CHAR，那些消息
                // 由下面那条 case 丢掉（wParam 是单字节 ANSI，绝不能当宽字符插），
                // 所以不会重复上屏 —— 实测提交「你好」后框里就是两个字。
                //
                // 早先这里直接 return 0（理由是"别让它生成 WM_IME_CHAR"），能上屏，
                // 但候选窗的收尾得靠下面那条 case 兜着，属于两处得配合的隐式约定，
                // 不如交给系统走完。
                break;
            }
            // 只有组字更新（GCS_COMPSTR 等）：交给 DefWindowProc，它负责让输入法
            // 重画组字窗口。顺手补一次位置（组字期间光标可能在框内移动）——
            // 这里**不**重挂上下文：组字中途重挂会把它弄断。
            app->applyImePlacement(hwnd, /*allowContextReset=*/false);
            ImeLog("  composition lp=0x%zX (交给 DefWindowProc)", static_cast<size_t>(lParam));
            break;
        }
        case WM_IME_CHAR:
            // 走到这里说明结果串没被 WM_IME_COMPOSITION 接走（某些老输入法只发这条）。
            // wParam 是单字节 ANSI，绝不能当宽字符插 —— 直接丢弃，日志留痕。
            ImeLog("  ime_char wp=0x%zX (丢弃)", static_cast<size_t>(wParam));
            return 0;
        case WM_IME_REQUEST: {
            // 新式 IME 用 IMR_CANDIDATEWINDOW 问"候选窗该放哪"，比 CANDIDATEFORM 可靠
            if (wParam == IMR_CANDIDATEWINDOW && app->imeCaret_ && lParam) {
                auto* cdf = reinterpret_cast<CANDIDATEFORM*>(lParam);
                int cx = 0, cy = 0, ch = 16;
                if (app->imeCaret_(cx, cy, ch)) {
                    cdf->dwIndex = 0;
                    cdf->dwStyle = CFS_CANDIDATEPOS;
                    cdf->ptCurrentPos.x = cx;
                    cdf->ptCurrentPos.y = cy + ch;   // 候选窗列在光标下方
                    return TRUE;
                }
            }
            // TSF 系输入法（Win11 微软拼音走这条）用 IMR_QUERYCHARPOSITION 问
            // "字符在屏幕上画哪儿"：不回或回 FALSE，它只能退回老式组字窗口，
            // 位置和内联排版都对不上。
            //
            // **pt / rcDocument 必须是屏幕坐标**（MSDN 对 IMECHARPOSITION 的明确要求）。
            // 这里一度直接塞了客户区坐标，结果候选窗算出来的位置落到屏幕别处 ——
            // 表现就是"组字看得见、候选框压根不出来"。
            if (wParam == IMR_QUERYCHARPOSITION && app->imeCaret_ && lParam) {
                // 组字串为空（拼音全被退格删掉）时不存在"第几个字符在哪"，回 FALSE
                // 才是正确应答 —— 硬塞一个光标位置会让输入法以为那个字符还在。
                // （注意：这并不会让候选窗自己收掉，候选窗是另一个问题，见
                //  WM_IME_ENDCOMPOSITION 那段的说明。）
                {
                    bool empty = true;
                    if (HIMC himc = ImmGetContext(hwnd)) {
                        empty = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0) <= 0;
                        ImmReleaseContext(hwnd, himc);
                    }
                    if (empty) break;   // → DefWindowProc 返回 FALSE
                }
                auto* icp = reinterpret_cast<IMECHARPOSITION*>(lParam);
                int cx = 0, cy = 0, ch = 16;
                if (app->imeCaret_(cx, cy, ch)) {
                    ImeLog("  querycharpos -> client(%d,%d) h=%d", cx, cy, ch);
                    POINT pt = { cx, cy };
                    ClientToScreen(hwnd, &pt);
                    icp->dwSize = sizeof(IMECHARPOSITION);
                    icp->pt = pt;
                    icp->cLineHeight = static_cast<UINT>(ch > 0 ? ch : 16);
                    RECT rc;
                    GetClientRect(hwnd, &rc);          // 客户区 → 屏幕，同样是屏幕坐标
                    POINT tl = { rc.left, rc.top }, br = { rc.right, rc.bottom };
                    ClientToScreen(hwnd, &tl);
                    ClientToScreen(hwnd, &br);
                    SetRect(&icp->rcDocument, tl.x, tl.y, br.x, br.y);
                    return TRUE;
                }
            }
            break;
        }
        case WM_SETCURSOR: {
            // 非客户区（边框/标题栏）交给系统，客户区按"光标下是什么"换形状
            if (LOWORD(lParam) != HTCLIENT) break;
            if (!app->cursor_) break;
            POINT pt;
            if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)) break;
            SetCursor(CursorHandleOf(app->cursor_(static_cast<float>(pt.x),
                                                  static_cast<float>(pt.y))));
            return TRUE;   // 处理了这次设置，别让 DefWindowProc 又改回箭头
        }
        case WM_CLOSE:
            app->requestExit();
            return 0;
        case WM_PAINT:
            // 就绪守卫：GL 已经拆掉 / 窗口正在销毁时，任何重绘都只会去碰已析构的状态。
            // 直接把脏区标成已处理，别落到 DefWindowProc 去真画一遍（docs/issues/02）。
            if (!app->gfx_.isReady()) {
                ValidateRect(hwnd, nullptr);
                return 0;
            }
            break;   // 就绪时交给 DefWindowProc（本窗口的绘制由 GL/分层表面负责）
        case WM_DESTROY:
            // 子窗口：宿主销毁自己的窗口时子窗口跟着被销毁，这里必须当场收摊 ——
            // 否则 hwnd_ 变悬垂指针、GL 上下文还挂着一个已经没了的 DC。
            if (app->childMode_) {
                app->gfx_.shutdown();
                app->running_ = false;
                app->hwnd_ = nullptr;
                return 0;      // 绝不 PostQuitMessage：那会把宿主的消息循环干掉
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

template <class Gfx>
inline BinUIAppT<Gfx>::BinUIAppT(int width, int height, bool vsync)
    : width_(width), height_(height), vsync_(vsync) {}

template <class Gfx>
inline void BinUIAppT<Gfx>::setWindowOptions(bool resizable, float aspect, bool transparent, const char* title) {
    resizable_ = resizable;
    aspect_ = aspect > 0 ? aspect : 0;
    transparent_ = transparent;
    if (title) title_ = title;
}

template <class Gfx>
inline BinUIAppT<Gfx>::~BinUIAppT() { gfx_.shutdown(); }

template <class Gfx>
inline void BinUIAppT<Gfx>::invalidateRect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dirty_) {
        dirtyL_ = std::min(dirtyL_, x);
        dirtyT_ = std::min(dirtyT_, y);
        dirtyR_ = std::max(dirtyR_, x + w);
        dirtyB_ = std::max(dirtyB_, y + h);
    } else {
        dirty_ = true;
        dirtyL_ = x; dirtyT_ = y; dirtyR_ = x + w; dirtyB_ = y + h;
    }
}

template <class Gfx>
inline void BinUIAppT<Gfx>::invalidateAll() {
    dirty_ = true;
    dirtyL_ = 0; dirtyT_ = 0; dirtyR_ = width_; dirtyB_ = height_;
    requestFrameNow();   // 尺寸变化/热重载/失焦这类"整屏变了"的事件不等下一拍
}

template <class Gfx>
inline void BinUIAppT<Gfx>::cancelImeComposition() {
    composing_ = false;
    composeEndMs_ = -1e9;   // 程序主动取消不算"用户按 Esc 取消候选"，别吞后续 Esc
    imePosX_ = imePosY_ = -1;
    HWND win = static_cast<HWND>(hwnd_);
    if (!win) return;
    if (HIMC himc = ImmGetContext(win)) {
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(win, himc);
    }
}

template <class Gfx>
inline void BinUIAppT<Gfx>::syncImeContext() {
    if (!imeActive_) return;              // 宿主没接：保持 IME 常挂（老行为）
    HWND win = static_cast<HWND>(hwnd_);
    if (!win) return;
    const bool want = imeActive_();
    if (want == imeAttached_) return;
    if (want) {
        // 重新挂回默认输入法上下文：进入编辑框时中文输入法要能组字
        ImmAssociateContextEx(win, nullptr, IACE_DEFAULT);
        imeAttached_ = true;
        imePosX_ = imePosY_ = -1;         // 重挂后组字窗要重新摆一次
    } else {
        // 没有编辑框时把 IME 摘掉：否则中文输入法把 WASD 当拼音吃掉（wParam=VK_PROCESSKEY），
        // 焦点导航就全哑了。摘之前先把可能残留的组字取消掉。
        cancelImeComposition();
        ImmAssociateContextEx(win, nullptr, 0);
        imeAttached_ = false;
    }
    ImeLog("  ime %s", imeAttached_ ? "attached" : "detached");
}

template <class Gfx>
inline int BinUIAppT<Gfx>::run() {
    if (!open()) return 1;
    while (running_) pump(1);
    close();
    return 0;
}

// 窗口类注册（顶层窗与子窗口共用同一个类）。

// 本**模块**的实例句柄（DLL 里就是 DLL 的，不是宿主 exe 的）。
// GetModuleHandleW(nullptr) 返回的是 exe —— 把类注册进别人的 atom 表是越界行为：
// 顶层窗侥幸能用，子窗口模式会被父窗口的模块关系放大成问题。
static HINSTANCE wcInstance() {
    HMODULE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&wcInstance), &module);
    return module ? static_cast<HINSTANCE>(module) : GetModuleHandleW(nullptr);
}

// hCursor 给 IDC_ARROW：子窗口没有非客户区，光标只能由类来定。
template <class Gfx>
inline bool BinUIAppT<Gfx>::ensureClass() {
    static bool done = false;
    if (done) return true;

    // 窗口类必须是 Unicode（RegisterClassW + DispatchMessageW）：
    // ANSI 窗口在 CP936 等下，一个中文字符会被系统拆成两个 WM_CHAR（DBCS 字节），
    // handleChar 逐字节转 UTF-8 就得到乱码（中 -> "ÖÐ"）。IME 输入同理。
    WNDCLASSW wc = {};
    // CS_OWNDC：窗口独占 DC，resize 时系统不回收该 DC（避免 SwapBuffers 用到
    // 被拆掉的旧后备缓冲 —— GDI 呈现路径在快速 resize 时会因此崩溃）
    // CS_DBLCLKS：没有它系统不会发 WM_LBUTTONDBLCLK，双击选词就永远收不到
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = BinUIAppT::wndProc;
    wc.hInstance = wcInstance();
    wc.lpszClassName = Gfx::appClassName();   // 两个后端各有类名，同进程并存不冲突
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!RegisterClassW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::fprintf(stderr, "[gl] RegisterClassW failed: %lu\n", (unsigned long)err);
            return false;
        }
    }
    done = true;
    return true;
}

template <class Gfx>
inline bool BinUIAppT<Gfx>::open() {
    SetProcessDPIAware();
    if (!ensureClass()) return false;

    RECT work = { 0, 0, 800, 600 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int winW = std::min(width_, static_cast<int>(work.right - work.left));
    int winH = std::min(height_, static_cast<int>(work.bottom - work.top));
    int winX = work.left + ((work.right - work.left) - winW) / 2;
    int winY = work.top + ((work.bottom - work.top) - winH) / 2;

    // 标题按 UTF-8 传进来，这里转 UTF-16（无边框窗口标题不显示，但任务栏/枚举要用）
    wchar_t wtitle[512] = {};
    if (!title_.empty())
        MultiByteToWideChar(CP_UTF8, 0, title_.c_str(), -1, wtitle,
                            static_cast<int>(sizeof(wtitle) / sizeof(wtitle[0])));

    // 无边框：WS_POPUP 无标题栏/边框/系统菜单；WS_MAXIMIZEBOX 允许 Win+↑ 最大化
    DWORD exStyle = 0;
    if (transparent_) exStyle |= WS_EX_LAYERED;
    HWND hwnd = CreateWindowExW(exStyle, Gfx::appClassName(), wtitle,
        WS_POPUP | WS_MAXIMIZEBOX | WS_VISIBLE, winX, winY, winW, winH,
        nullptr, nullptr, wcInstance(), nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[gl] Failed to create window\n");
        return false;
    }
    hwnd_ = hwnd;
    childMode_ = false;
    // 记下建窗时的 DPI（BINUI_FORCE_DPI 可覆盖）。换屏后 WM_DPICHANGED 会更新它，
    // 并通过 setDpiChangedFn 的回调通知宿主。
    dpi_ = dpiOf(hwnd);
    if (std::getenv("BINUI_FORCE_DPI"))
        std::fprintf(stderr, "[dpi] BINUI_FORCE_DPI=%d（模拟换屏后的 DPI）\n", dpi_);
    if (RemoteSession()) std::fprintf(stderr, "[dpi] 远程会话：呈现默认走读回 + GDI 贴屏\n");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // 禁用 Windows 11 DWM 自动圆角：UI 圆角由 border-radius 属性自控（跨平台一致）
    DWORD corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    gfx_.setTransparent(transparent_);
    if (!gfx_.init(hwnd, winW, winH, vsync_)) {
        std::fprintf(stderr, "[gl] OpenGL init failed\n");
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        return false;
    }
    // WS_VISIBLE 只保证"可见"，SetFocus 只改本线程键盘焦点 —— 都不会把窗口提到
    // 前台，于是从控制台启动时窗口被 cmd 压在后面。这里补一次显式前台化。
    BinUIForceForeground(hwnd);
    SetFocus(hwnd);
    width_ = winW;
    height_ = winH;
    invalidateAll(); // 首帧全量绘制（初始脏矩形为空，避免首帧仅动画区域被绘制，其余留白/留黑）

    lastTime_ = std::chrono::steady_clock::now();
    {
        const int hz = DisplayRefreshHzFor(hwnd);
        frameBudget_ = std::chrono::microseconds(1000000 / hz);
        if (Perf().on) {
            std::fprintf(stderr, "[perf] display refresh=%dHz budget=%.2fms\n",
                         hz, frameBudget_.count() / 1000.0);
            // 节拍来自谁：present 声明自己能阻塞到垂直同步时，帧相位由显示器兜底；
            // 否则完全靠外壳的合成节拍（依赖高精度定时器，见 waitForFrame）。
            // 注意这只说明后端的"声明"，实际阻塞与否由驱动/合成器决定（窗口模式下
            // 有的驱动让 SwapBuffers 立刻返回），所以两种情况下外壳都保留 nextFrameTime_ 闸门。
            std::fprintf(stderr, "[perf] 节拍: 合成预算 %.2fms（高精度定时器）；present 可阻塞垂直同步=%s\n",
                         frameBudget_.count() / 1000.0, pacedByPresent() ? "是" : "否");
        }
    }
    nextFrameTime_ = std::chrono::steady_clock::now();
    running_ = true;
    ownerThread_ = static_cast<unsigned long>(GetCurrentThreadId());
    return true;
}

// 子窗口模式：作为控件挂进宿主窗口的客户区。
//   · 不调 SetProcessDPIAware —— DPI 感知是进程级策略，库去改宿主的策略是越界行为
//   · 不 SetFocus —— 焦点归宿主管，抢焦点会把宿主自己的控件踢掉
//   · 不设 WS_EX_LAYERED —— 分层窗口属性对子窗口无效（逐像素透明在子窗口上做不到）
template <class Gfx>
inline bool BinUIAppT<Gfx>::openChild(void* parent, int x, int y, int width, int height) {
    const HWND par = static_cast<HWND>(parent);
    if (!par) {
        std::fprintf(stderr, "[gl] openChild: parent 为空\n");
        return false;
    }
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "[gl] openChild: 尺寸非法 %dx%d\n", width, height);
        return false;
    }
    if (!ensureClass()) return false;

    HWND hwnd = CreateWindowExW(0, Gfx::appClassName(), L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        x, y, width, height, par, nullptr, wcInstance(), nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[gl] openChild: CreateWindowExW 失败 (%lu)\n",
                     (unsigned long)GetLastError());
        return false;
    }
    hwnd_ = hwnd;
    childMode_ = true;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    gfx_.setTransparent(false);
    if (!gfx_.init(hwnd, width, height, vsync_)) {
        std::fprintf(stderr, "[gl] openChild: OpenGL init failed\n");
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        childMode_ = false;
        return false;
    }
    width_ = width;
    height_ = height;
    invalidateAll();

    lastTime_ = std::chrono::steady_clock::now();
    {
        const int hz = DisplayRefreshHzFor(hwnd);
        frameBudget_ = std::chrono::microseconds(1000000 / hz);
        if (Perf().on) {
            std::fprintf(stderr, "[perf] display refresh=%dHz budget=%.2fms\n",
                         hz, frameBudget_.count() / 1000.0);
            // 节拍来自谁：present 声明自己能阻塞到垂直同步时，帧相位由显示器兜底；
            // 否则完全靠外壳的合成节拍（依赖高精度定时器，见 waitForFrame）。
            // 注意这只说明后端的"声明"，实际阻塞与否由驱动/合成器决定（窗口模式下
            // 有的驱动让 SwapBuffers 立刻返回），所以两种情况下外壳都保留 nextFrameTime_ 闸门。
            std::fprintf(stderr, "[perf] 节拍: 合成预算 %.2fms（高精度定时器）；present 可阻塞垂直同步=%s\n",
                         frameBudget_.count() / 1000.0, pacedByPresent() ? "是" : "否");
        }
    }
    nextFrameTime_ = std::chrono::steady_clock::now();
    running_ = true;
    ownerThread_ = static_cast<unsigned long>(GetCurrentThreadId());
    return true;
}

// 当前线程是不是建窗线程。窗口与 WGL 上下文都有线程亲和：从别的线程调 pump/render
// 不会立刻报错，而是随机崩（GL 状态被并发改）或退出时挂死 —— 见 docs/issues/08。
template <class Gfx>
inline bool BinUIAppT<Gfx>::onOwnerThread() const {
    if (ownerThread_ == 0) return true;   // 还没建窗：交给上层按"未打开"处理
    return ownerThread_ == static_cast<unsigned long>(GetCurrentThreadId());
}

template <class Gfx>
inline void BinUIAppT<Gfx>::renderFrame() {
    if (!running_) return;
    if (!onOwnerThread()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[binui] binui_render/binui_pump 在**非建窗线程**上被调用 —— "
                "Windows 窗口与 OpenGL 上下文都有线程亲和，继续跑会随机崩溃或退出挂死。\n"
                "        Go 宿主：在 main 里调一次 runtime.LockOSThread()（单线程锁住，"
                "不是只加在子 goroutine 上）。\n");
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastTime_).count();
    lastTime_ = now;
    if (dt > 0.1f) dt = 1.0f / 60.0f;
    syncImeContext();   // 编辑态变了就把 IME 挂上/摘下（非编辑态不能让 IME 吃掉 WASD）
    const double u0 = Perf().on ? NowMs() : 0.0;
    if (update_) update_(dt);
    if (Perf().on) Perf().sumUpdate += NowMs() - u0;
    // 帧节拍（v0.11.6）：**固定网格 + 精确等待**。
    //   旧写法两个毛病叠在一起，120Hz 屏上直接变 ~64fps 并且微抖：
    //   ①`nextFrameTime_ = now + budget` 把每帧自身耗时也算进周期，逐帧累积漂移；
    //   ②`MsgWaitForMultipleObjectsEx(timeoutMs)` 的毫秒超时按**系统计时器精度**兑现，
    //     默认 15.6ms —— 想睡 8.33ms 实际睡 15.6ms，一帧跨过两个刷新；
    //   ③ 再加个"+1ms"保险，越垫越晚。
    // 现在：网格用 `+=`（落后太多才重新对齐），等待用高精度可等待定时器（精度 ~0.5ms，
    // 见 waitForFrame），并且**保留 nextFrameTime_ 闸门**——present 会阻塞到垂直同步时，
    // 闸门正好挡住"刚提交完下一轮又立刻重画"。
    if (now < nextFrameTime_) return;
    renderOnce();
    nextFrameTime_ += frameBudget_;
    const auto after = std::chrono::steady_clock::now();
    if (nextFrameTime_ < after - frameBudget_ * 2) nextFrameTime_ = after + frameBudget_;
}

// 这一帧是否由 present() 的垂直同步阻塞来决定节拍（只用于启动时打一行诊断）
template <class Gfx>
inline bool BinUIAppT<Gfx>::pacedByPresent() const {
    return gfx_.presentPacesFrames() && !gdiPresent_ && !fullscreenPresent_;
}

// 睡到 t（同时保持消息可唤醒）。毫秒级的 MsgWait 超时会被系统计时器精度
// （默认 15.6ms）兑现，128Hz/120Hz 这种亚 10ms 的节拍完全没法用 —— 所以优先用
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION（Win10 1803+，精度 ~0.5ms，且不像
// timeBeginPeriod 那样改全局电源策略）。拿不到就退回毫秒等待（老行为）。
template <class Gfx>
inline void BinUIAppT<Gfx>::waitForFrame(std::chrono::steady_clock::time_point t) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const auto remain = t - now;
    if (remain <= microseconds(0)) return;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    if (!frameTimer_) {
        frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                             CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
    }
    HANDLE h = static_cast<HANDLE>(frameTimer_);
    if (h) {
        LARGE_INTEGER due;
        // 负值 = 相对时间，单位 100ns
        due.QuadPart = -static_cast<LONGLONG>(duration_cast<nanoseconds>(remain).count() / 100);
        if (SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) {
            MsgWaitForMultipleObjectsEx(1, &h, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return;
        }
    }
    const int ms = static_cast<int>(duration_cast<milliseconds>(remain).count()) + 1;
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(ms), QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
}

template <class Gfx>
inline void BinUIAppT<Gfx>::setClientSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;
    // 走和 WM_SIZE 相同的"挂起 → 下一帧应用"路径，避免宿主在绘制中途改尺寸
    pendingW_ = width;
    pendingH_ = height;
    invalidateAll();
}

template <class Gfx>
inline int BinUIAppT<Gfx>::pump(int timeoutMs) {
    if (!running_) return 1;
    // 跨线程调用：明确报错，别让它变成"随机崩 / 退出挂死"（docs/issues/08）
    if (!onOwnerThread()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[binui] binui_pump 在**非建窗线程**上被调用，已拒绝（返回 -1）。\n"
                "        Windows 窗口与 OpenGL 上下文都有线程亲和：宿主必须在同一个 OS 线程上"
                "建窗、pump、销毁。\n"
                "        Go 宿主：在 main 里调一次 runtime.LockOSThread()（main goroutine "
                "会被调度到别的线程，只加在子 goroutine 上不够）。\n");
        }
        return -1;
    }

    // 子窗口模式：消息由宿主的循环派发（DispatchMessage 会投递到本窗口的 wndProc），
    // 这里**绝不能** PeekMessage —— 那会把宿主的消息抽走，宿主自己的控件就不响应了。
    if (childMode_) {
        renderFrame();
        return running_ ? 0 : 1;
    }

    // 等待多久：有挂起 resize 就立刻（尺寸要跟手），有脏区就等到下一帧点，
    // 都没有就阻塞到输入或超时。等的时候是**睡眠**（不烧 CPU），
    // 以前"脏区就一直满速出帧"是空闲 80% 单核的主因。
#ifndef MWMO_INPUTAVAILABLE
#define MWMO_INPUTAVAILABLE 0x0004
#endif
    // 等待：有挂起 resize 就立刻（尺寸要跟手），有脏区就等到下一帧点，
    // 都没有就阻塞到输入或超时。等的时候是**睡眠**（不烧 CPU）。
    // v0.11.6：等"到下一帧点"这件事必须精确到亚毫秒 —— 120Hz 的预算只有 8.33ms，
    // 而 MsgWait 的毫秒超时按系统计时器精度兑现（默认 15.6ms），一睡就跨两个刷新。
    MSG msg = {};
    if (pendingW_ > 0 && pendingH_ > 0) {
        // 尺寸变化：这一拍就出帧，不等
    } else if (dirty_) {
        // 等到（或超过）下一帧点：present 会阻塞到垂直同步时它会先返回，
        // 闸门是 nextFrameTime_，这里只负责别提前空转。
        const auto now = std::chrono::steady_clock::now();
        waitForFrame(nextFrameTime_ > now ? nextFrameTime_ : now);
    } else if (timeoutMs > 0) {
        // 没有脏区也要按帧点醒来：上一帧画完 dirty_ 就清了，"有没有下一帧"要到
        // 下一次 renderFrame 里 update_（推进动画）才知道。这里若按 timeoutMs（1ms）试，
        // 要么退化成 15.6ms 的粗睡（动画掉回 64fps），要么高精度地每秒醒 1000 次。
        // 直接睡到 nextFrameTime_：这就是 120 次/秒的"帧点"，输入照样能唤醒（QS_ALLINPUT）。
        const auto now = std::chrono::steady_clock::now();
        if (nextFrameTime_ > now) waitForFrame(nextFrameTime_);
    }

    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { running_ = false; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!running_) return 1;

    renderFrame();
    return running_ ? 0 : 1;
}

template <class Gfx>
inline void BinUIAppT<Gfx>::close() {
    running_ = false;
    if (Perf().on && Perf().drawn > 0) {
        std::fprintf(stderr, "[perf] exit: %lld frames drawn\n", Perf().drawn);
        std::fflush(stderr);
    }
    // 销毁顺序：先把窗口过程摘回 DefWindowProc（之后任何消息都不进我们的代码，
    // 也不会有悬垂的 GWLP_USERDATA），再拆 GL，最后 DestroyWindow。
    //
    // 为什么是这个顺序：窗口还活着时 DestroyWindow 会**同步**把 WM_KILLFOCUS /
    // WM_NCDESTROY 派发回窗口过程；"先拆 GL 再销毁"在那条重入路径上会踩到已析构的成员。
    // 这是一处真隐患，但不是 docs/issues/02 那个"约 50% AV"的根因 —— 事后复验发现，
    // 那条崩溃的真因是**宿主跨线程**调用（Go 的 main goroutine 没 LockOSThread 会迁移），
    // 见 docs/issues/02 的「复验」一节与 docs/issues/08。这里的加固保留，另加线程自检。
    HWND win = static_cast<HWND>(hwnd_);
    if (win && !onOwnerThread()) {
        // 跨线程销毁是 docs/issues/02 那个"约 50% AV"的真因（宿主没锁 OS 线程）。
        // 走到这一步说明宿主已经写错了，但能打的提示还是要打：不然现象只有随机崩/挂。
        std::fprintf(stderr,
            "[binui] binui_app_destroy 在**非建窗线程**上被调用 —— "
            "这是未定义行为（DestroyWindow / wglDeleteContext 要求建窗线程）。\n"
            "        Go 宿主：在 main 里调一次 runtime.LockOSThread()。\n");
    }
    if (win) {
        SetWindowLongPtrW(win, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DefWindowProcW));
        SetWindowLongPtrW(win, GWLP_USERDATA, 0);
        ShowWindow(win, SW_HIDE);    // 隐藏后销毁不会再产生一次真实绘制
    }
    gfx_.shutdown();   // 拆 GL（含 ReleaseDC，必须在窗口销毁前）；shutdown 幂等
    if (frameTimer_) {
        CloseHandle(static_cast<HANDLE>(frameTimer_));
        frameTimer_ = nullptr;
    }
    if (win) {
        DestroyWindow(win);
        hwnd_ = nullptr;
    }
    // 排空残留消息（含 WM_QUIT），避免宿主下一轮 pump 读到上一个窗口的退出消息。
    // 子窗口模式下消息队列是宿主的，清空等于把宿主没处理完的消息吃掉 —— 绝不这么做。
    if (!childMode_) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {}
    }
    childMode_ = false;
}

template <class Gfx>
inline void BinUIAppT<Gfx>::handleMouseMove(float x, float y) { if (move_) move_(x, y); }
template <class Gfx>
inline void BinUIAppT<Gfx>::handleMousePress(float x, float y) { if (press_) press_(x, y); }
template <class Gfx>
inline void BinUIAppT<Gfx>::handleMouseRelease(float x, float y) { if (release_) release_(x, y); }
template <class Gfx>
inline void BinUIAppT<Gfx>::handleMouseLeave() { if (leave_) leave_(); }

template <class Gfx>
inline void BinUIAppT<Gfx>::handleMouseWheel(float x, float y, float dy) { if (wheel_) wheel_(x, y, dy); }

template <class Gfx>
inline void BinUIAppT<Gfx>::handleResize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    // 只记录：拖拽期间 WM_SIZE 高频到达，实际 resize（重排/FBO 重建/重绘）每帧应用一次
    pendingW_ = w;
    pendingH_ = h;
}

// 取 DPI：BINUI_FORCE_DPI 优先（单显示器机器上模拟换屏），否则用 GetDpiForWindow
// （Win10 1607+，不存在时退回 GetDeviceCaps）。返回 96 = 100%。
template <class Gfx>
inline int BinUIAppT<Gfx>::dpiOf(HWND hwnd) {
    static const int forced = [] {
        const char* e = std::getenv("BINUI_FORCE_DPI");
        if (!e || !*e) return 0;
        const int v = std::atoi(e);
        return (v >= 48 && v <= 960) ? v : 0;
    }();
    if (forced > 0) return forced;
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getDpi = [] {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        return u ? reinterpret_cast<GetDpiForWindowFn>(
                       reinterpret_cast<void*>(GetProcAddress(u, "GetDpiForWindow")))
                 : nullptr;
    }();
    if (getDpi && hwnd) {
        const UINT d = getDpi(hwnd);
        if (d >= 48) return static_cast<int>(d);
    }
    HDC dc = hwnd ? GetDC(hwnd) : GetDC(nullptr);
    int d = 96;
    if (dc) {
        d = static_cast<int>(GetDeviceCaps(dc, LOGPIXELSX));
        if (hwnd) ReleaseDC(hwnd, dc);
        else ReleaseDC(nullptr, dc);
    }
    return d > 0 ? d : 96;
}

// WM_DPICHANGED，wParam 低字是新 DPI、lParam 是系统建议的新窗口矩形。
// 库这一层能做的只有三件（都不碰进程 DPI 策略，见 openChild 的注释）：
//   1. 按系统建议的矩形摆窗口（不摆的话 Windows 会把窗口按位图拉伸）；
//   2. 走和 WM_SIZE 相同的"挂起 → 帧内应用"路径（重排 + 后端 resize + 全量重绘）；
//   3. 记下新 DPI 并回调宿主（应用据此调 setUiScale / 换图标等）。
// 真正的"跨屏缩放"取决于应用是否把进程设成 per-monitor-v2；本机单显示器时这条
// 路径只能用 BINUI_FORCE_DPI 模拟（见 dpiOf），所以它现在是**接口 + 模拟**状态，
// 见 docs/vulkan_design.md §7.5。
template <class Gfx>
inline void BinUIAppT<Gfx>::handleDpiChanged(int newDpi, const void* suggestedRect) {
    if (newDpi > 0) dpi_ = newDpi;
    const HWND h = static_cast<HWND>(hwnd_);
    if (h && suggestedRect && !childMode_) {
        const RECT* rc = static_cast<const RECT*>(suggestedRect);
        SetWindowPos(h, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dpiChanged_) dpiChanged_(dpi_);
}

// 客户区是否盖满整块显示器（borderless 全屏）。
// 这种状态下 NVIDIA 的 SwapBuffers 会被 DWM 提升成直翻转，实测在远程桌面/虚拟显示器上
// 画面会**停住不动**（应用仍在 100fps 绘制，屏幕却是旧帧），切回普通尺寸就恢复。
// 判定：客户区宽高都 ≥ 所在显示器的尺寸。
template <class Gfx>
inline bool BinUIAppT<Gfx>::clientCoversMonitor() const {
    const HWND h = static_cast<HWND>(hwnd_);
    if (!h || childMode_) return false;
    RECT cr;
    if (!GetClientRect(h, &cr)) return false;
    const HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return (cr.right - cr.left) >= mw && (cr.bottom - cr.top) >= mh;
}

template <class Gfx>
inline void BinUIAppT<Gfx>::applyPendingResize() {
    if (pendingW_ <= 0 || pendingH_ <= 0) return;
    int w = pendingW_, h = pendingH_;
    pendingW_ = pendingH_ = 0;
    width_ = w;
    height_ = h;
    if (resize_) resize_(w, h);
    gfx_.resize(w, h);
    // 客户区铺满显示器 → 绕开直翻转（DWM 会把呈现提升成直翻转，远程/虚拟显示器上会停帧）
    fullscreenPresent_ = clientCoversMonitor();
    // 远程会话（RDP）本身也走读回贴屏：这条路径在虚拟显示器上是实测唯一稳定的。
    // 本机没有 RDP 会话时用 BINUI_FORCE_REMOTE=1 模拟（见 RemoteSession）。
    if (RemoteSession()) fullscreenPresent_ = true;
    invalidateAll();
}

template <class Gfx>
inline void BinUIAppT<Gfx>::renderOnce() {
    FramePerf& perf = Perf();
    const double t0 = perf.on ? NowMs() : 0.0;
    applyPendingResize();
    const double t1 = perf.on ? NowMs() : 0.0;
    if (dirty_ && gfx_.isReady()) {
        int rc[4] = { dirtyL_, dirtyT_, dirtyR_, dirtyB_ };
        gfx_.beginFrame(rc);
        if (draw_) draw_(gfx_, rc);
        const double t2 = perf.on ? NowMs() : 0.0;
        // 拖拽 resize 期与"客户区铺满显示器"时都走 GDI 贴屏：
        // 前者绕开 NVIDIA 在 resize+SwapBuffers 组合下的崩溃，
        // 后者绕开全屏直翻转导致的"画面停住"（见 clientCoversMonitor 注释）。
        if ((gdiPresent_ || fullscreenPresent_) && !transparent_) gfx_.presentViaGdi();
        else {
            gfx_.present();
            // v0.11.8：**与桌面合成器对齐**。present 只是把帧排进队列，真正的显示节拍
            // 是 DWM 的合成（每刷新周期一次）。不对齐的话，我们按 8.33±1ms 出帧，
            // 越过刷新边界的那些帧会被多留一个周期 —— 平均 120fps 但肉眼是"抖"：
            // VK 后端实测帧间隔 7ms/8ms/9ms 三档混在一起（GL 那边每 15.6ms 一次，
            // 正好是刷新的整数倍，反而看着"均匀但慢"）。
            // 只在真走交换链时 Flush：GDI 读回 / 远程会话 / 拖拽那几条路径不该被合成器拖住。
            if (pacedByPresent()) DwmFlush();
        }
        dirty_ = false;
        if (perf.on) {
            const double t3 = NowMs();
            if (perf.frames == 0) perf.winStart = t0;
            ++perf.frames;
            ++perf.drawn;
            // 帧间隔（出帧时刻之差）：掉拍 = 超过 1.5 个预算 —— 平均 fps 达标但偶发翻倍
            // 的间隔，肉眼就是微卡，avg/max 帧耗看不出来这一项。
            if (perf.lastFrameMs > 0) {
                const double iv = t0 - perf.lastFrameMs;
                if (iv > perf.maxInterval) perf.maxInterval = iv;
                if (iv > frameBudget_.count() / 1000.0 * 1.5) ++perf.missed;
                int slot = static_cast<int>(iv);
                if (slot < 0) slot = 0;
                if (slot > 20) slot = 20;
                ++perf.hist[slot];
                // 单次 >100ms 的间隔单独打一行（带时间偏移）：排查"卡一下"时最需要的就是
                // "什么时候卡的、卡了多久"，而不是平均帧率。
                if (iv > 100.0)
                    std::fprintf(stderr, "[perf] 帧间隔异常 %.1f ms @ %.2f s\n", iv,
                                 (t0 - perf.winStart) / 1000.0);
            }
            perf.lastFrameMs = t0;
            const double rs = t1 - t0;
            perf.sumResize += rs;
            if (rs > perf.maxResize) perf.maxResize = rs;
            perf.sumDraw += t2 - t1;
            const double ps = t3 - t2;
            perf.sumPresent += ps;
            if (ps > perf.maxPresent) perf.maxPresent = ps;
            perf.sumTotal += t3 - t0;
            if (t3 - t0 > perf.maxTotal) perf.maxTotal = t3 - t0;
            if (perf.frames >= 600) PerfFlush();
        }
    }
}

template <class Gfx>
inline void BinUIAppT<Gfx>::handleChar(wchar_t ch) {
    if (ch >= 0xD800 && ch <= 0xDBFF) { pendingHigh_ = ch; return; }
    unsigned cp;
    if (ch >= 0xDC00 && ch <= 0xDFFF && pendingHigh_) {
        cp = 0x10000 + ((static_cast<unsigned>(pendingHigh_) - 0xD800) << 10) + (ch - 0xDC00);
        pendingHigh_ = 0;
    } else {
        pendingHigh_ = 0;
        cp = static_cast<unsigned>(ch);
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (char_) {
        std::string s;
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        char_(s);
    }
}

template <class Gfx>
inline void BinUIAppT<Gfx>::handleKey(int vk) {
    if (key_) key_(vk);
}

template <class Gfx>
inline void BinUIAppT<Gfx>::handleFocusLost() {
    pendingHigh_ = 0;
    if (focus_) focus_(false);
}


} // namespace binui

#endif // BINUI_APP_H
