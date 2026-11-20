// binui_app.h - 无边框窗口 + 事件循环（与后端无关，模板化在 Gfx 上）
//
// 这份代码原来长在 binui_gl.cpp 里，叫 BinUIAppGL。P5 要做 GPU=gl|vk 时发现：
// 整个外壳只用到后端的 8 个方法（init/shutdown/resize/beginFrame/present/presentViaGdi/
// setTransparent/isReady），跟 GL 一点关系都没有 —— 那就模板化，各后端共用
// 同一份窗口/输入/IME/拖拽/节流逻辑，不会在"拖边框"这种边角上分叉。
// 0.37.x 之后实例只剩一个：BinUIApp2D = BinUIAppT<BinUI2D>（三份 GPU 后端已删除）。
//
// 后端只需要提供：
//   · IBinUIGfx 的 8 个方法（见上）
//   · static const wchar_t* appClassName()  —— 窗口类名（同进程两个后端并存时不冲突）
//
// 用法：
//   BinUIApp2D app(960, 660);
//   app.setDrawFn([&](IBinUIGfx& g, const void* clip) { drawUi(g, renderer, clip); });
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
#include "binui_paths.h" // binui::ex::StartupMark / StartupNowMs（BINUI_STARTUP 启动分段）

#include "binui_renderer.h"
#include "platform/binui_win32_foreground.h"   // BinUIForceForeground：把窗口提到前台

namespace binui {

// 是否远程会话（RDP / 虚拟显示器）：这种环境下 swapchain 直翻转会停帧
// （当年的 GL 0.9.1 与 Vulkan 后端都实测过；那两份后端已在 0.37.x 删除），
// 后端据此默认走读回 + GDI 贴屏。
// BINUI_FORCE_REMOTE=1 可在本机模拟（本机没有 RDP 会话，否则这条分支无法验收）。
// 放在命名空间而不是模板静态里：后端（现在的 binui_bin2d.cpp）要调它。
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
    // 抖动（v0.32.0）：间隔**不是刷新周期的整数倍**的次数。这才是"动画看着卡"的
    // 直接指标 —— 60fps 落在 120Hz 屏上是 2 个刷新/帧，稳；而"33.6ms 一帧"落在
    // 120Hz 上是 4.03 个刷新，于是每 25 帧多挤出一个 5 刷新周期，肉眼就是一顿一顿。
    // 帧率（avg fps）达标也可能满屏抖动，所以单列这一项。
    long long judder = 0;
    long long ivN = 0;
    double sumPeriods = 0;        // 间隔 / 刷新周期 的累加（单位：刷新周期）
    double minPeriods = 1e9, maxPeriods = 0;
    // 帧间隔直方图：**按刷新周期分档**（0.25 个周期一档，0..40 档 = 0..10 周期）。
    // 以前是 1ms 一档，在 120Hz 上等于"每档 1/8 个周期"，看不出拍子；
    // 换成周期刻度后，"4.00 档很宽"= 每帧正好吃 4 个刷新（稳），
    // "4.00 和 5.00 两档都有一堆"= 一顿一顿。缩放类动画对这种拍子最敏感。
    long long hist[41] = {};
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
    // 抖动单独一行：它回答的是"帧率够了，为什么还是看着卡"。
    // `标准` 策略**允许**一帧超过一个刷新周期（性能不够就尽力多画，见 bingpu.h 的口径），
    // 那种情况下间隔本来就不可能是整数倍 —— 那不是节拍器出错，是画不完，所以单独点出来。
    {
        const double avgPeriods = p.ivN ? p.sumPeriods / static_cast<double>(p.ivN) : 0.0;
        std::fprintf(stderr,
                     "[perf] 刷新周期/帧 min/avg/max=%.2f/%.2f/%.2f  抖动=%lld/%lld"
                     "（间隔不是刷新周期整数倍的次数）%s\n",
                     p.minPeriods > 1e8 ? 0.0 : p.minPeriods, avgPeriods, p.maxPeriods,
                     p.judder, p.ivN,
                     avgPeriods > 1.05
                         ? "  ← 一帧画不完一个刷新周期：标准策略下这就是「尽力多画」，"
                           "抖动是必然的，不是节拍错"
                         : "");
    }
    p.sumResize = p.sumDraw = p.sumPresent = p.sumTotal = 0;
    p.maxTotal = p.maxPresent = p.maxResize = 0;
    p.maxInterval = 0;
    p.missed = 0;
    p.judder = 0;
    p.ivN = 0;
    p.sumPeriods = 0;
    p.minPeriods = 1e9;
    p.maxPeriods = 0;
    {   // 直方图：一行紧凑打印，0ms..19ms、20ms+
        char line[512];
        int o = 0;
        for (int i = 0; i < 41 && o < 460; i++) {
            if (p.hist[i] == 0) continue;
            o += std::snprintf(line + o, sizeof(line) - static_cast<size_t>(o), "%s%.2f:%lld",
                               o ? " " : "", i / 4.0, p.hist[i]);
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

// 实际用来算帧预算的频率：**默认跟显示器刷新率**，`BINUI_FPS=<n>` 可封顶（0 = 跟显示器）。
//
// 曾经想把默认值封顶到 60Hz（省一半呈现开销），但**页切换那种整页横移的动画立刻变糙**：
// 1058px 的滑动在 200ms 里走完，120Hz 是 2.6px/帧、60Hz 是 5.3px/帧，肉眼差得很明显。
// 交互正在发生的时候值得给满刷新率 —— 真正的省电收益在"没动画/后台/最小化"那几条
// （见 renderFrame 的节拍闸门与 IsIconic 短路），不在砍活跃帧率。
//
// v0.34.0 起这只是**没有设备层节拍器的后端**的兜底（仓里遗留的三份 GPU 实现）：
// 只要后端能回答 paceAvailable()，节拍就由 binGPU 算，这个频率不再参与。
inline int FrameHzFor(HWND hwnd) {
    const char* e = std::getenv("BINUI_FPS");
    if (e && *e) {
        const int v = std::atoi(e);
        if (v > 0 && v <= 360) return v;
    }
    return DisplayRefreshHzFor(hwnd);
}

// 帧节拍策略（v0.34.0）：**外壳不再自己算节拍**，只把"最多多少帧"的意图交给后端。
//   BIN2D_PACE / BINUI_PACE = standard | unlimited | 240 | 120 | 90 | 60 | 30
//   BINUI_FPS=<n> 保留为兼容写法（0 = unlimited，数字 = 对应档位；其它数字走 standard）
// 为什么名字里带 BIN2D：这个 knob 属于**绘制语义层**（bin2D/bin3D），不是外壳的。
// 外壳只做两件事：把字符串转下去、按后端给的 wait 睡觉。
inline const char* PaceTextFromEnv() {
    if (const char* e = std::getenv("BIN2D_PACE"); e && *e) return e;
    if (const char* e = std::getenv("BINUI_PACE"); e && *e) return e;
    return std::getenv("BINUI_FPS");
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

// 无边框（WS_POPUP）应用外壳：绘制后端 + 事件循环。
// Gfx 必须是实现了 IBinUIGfx 且带 appClassName() 的后端（现在只有 BinUI2D）。
template <class Gfx>
class BinUIAppT {
public:
    using UpdateFn  = std::function<void(float dt)>;
    // 绘制回调只认**接口**（IBinUIGfx）而不是具体后端：三份老 GPU 后端能整份删掉、
    // 而 9 个示例一行都不用改，靠的就是这条契约。
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
    // 光标查询：给定客户区坐标，返回 BinUICursorKind
    // （0=箭头 1=手型 2=I 型 3=竖直拖动 4=水平拖动 5=左上/右下角 6=右上/左下角）。
    using CursorFn  = std::function<int(float x, float y)>;
    // 双击：宿主拿到的是客户区坐标（双击选词用）
    using DoubleClickFn = std::function<void(float x, float y)>;
    // 右/中键（left 仍走 setPressFn / setReleaseFn）：引擎要节点才能派发 CONTEXT_MENU /
    // AUX_CLICK，所以外壳把按键号一起交出去（0=左 1=中 2=右，与 DOM 同号），
    // 宿主转手 renderer.press(x,y,button) / release(x,y,button)。
    using ButtonFn = std::function<void(float x, float y, int button, bool down)>;
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
    void setButtonFn(ButtonFn fn)   { btn_ = std::move(fn); }
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
    bool gdiPresent_ = false; // resize 拖拽期：present 走 GDI（读回离屏 → SetDIBitsToDevice），不提交交换链
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
    ButtonFn btn_;
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
    // 显示器刷新周期（ms，**不受 BINUI_FPS 影响**）：抖动指标与直方图的刻度。
    double refreshMs_ = 1000.0 / 60.0;
    void requestFrameNow() { nextFrameTime_ = std::chrono::steady_clock::time_point{}; }
    // 全屏标脏但**不动节拍相位**：量测（BINUI_FRAME_EXIT 每帧强制重画）不能顺手把
    // 刷新网格的相位砸掉，否则量出来的帧间隔根本没有参考价值。
    void markAllDirty();
    // 节拍由 present() 的垂直同步阻塞决定（vsync 交换链），此时不再自己掐帧
    bool pacedByPresent() const;
    // 建窗/init 之后把节拍参数定下来（v0.34.0：有设备层节拍器就问它，没有才用老网格）
    void setupPacing(HWND hwnd);
    // 睡到指定时刻（保留消息可唤醒；用高精度定时器，见实现里的注释）
    void waitForFrame(std::chrono::steady_clock::time_point t);
    void* frameTimer_ = nullptr;   // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 的定时器句柄
    bool dirty_ = true;    // 上一拍是否真的画了一帧（renderOnce 里置位）。用来提前判断"这一拍还有活要干"：
    // 动画的脏区要到 update_ 跑完才出现，而 pump 的等待发生在 update_ 之前。
    bool lastDrew_ = false;
    // 上一次 present 花了多少毫秒：>半个刷新周期就说明它自己在等合成边界
    double lastPresentMs_ = 0.0;
    // 网格相位锚过没有（v0.32.0）：第一帧结束后用一次 DwmFlush 把网格贴到显示的
    // 合成边界上，之后靠外壳自己的高精度定时器维持 —— 每帧都 flush 要多等一个周期。
    bool phaseAnchored_ = false;
    // 当前每帧占几个刷新周期（滞回用，见 renderFrame 末尾）。0 = 还没学到。
    long long frameSteps_ = 0;
    int dirtyL_ = 0, dirtyT_ = 0, dirtyR_ = 0, dirtyB_ = 0;
    int pendingW_ = 0, pendingH_ = 0; // 挂起的窗口尺寸（WM_SIZE 只记录，帧内应用一次）
    bool trackingLeave_ = false;
    wchar_t pendingHigh_ = 0;

    void applyPendingResize(); // 应用挂起 resize：渲染器重排 + gfx 重建 + 全量重绘
    void resizeBenchTick();    // BINUI_RESIZE_BENCH：每帧抖一次窗口尺寸，退出前打印两段耗时
    void renderOnce();         // 应用挂起 resize 后绘制一帧并呈现
    void startResize(int hit); // 手动 resize 拖拽循环（每轮同步渲染）
};

// ============================================================
// 以下：辅助函数与实现（P5 从 binui_gl.cpp 整块搬来，模板化在 Gfx 上）
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
    static HCURSOR cache[7] = {};
    if (kind < 0 || kind > 6) kind = 0;
    if (!cache[kind]) {
        LPCWSTR id = reinterpret_cast<LPCWSTR>(IDC_ARROW);
        switch (kind) {
            case 1: id = reinterpret_cast<LPCWSTR>(IDC_HAND);    break;  // 可交互（按钮等）
            case 2: id = reinterpret_cast<LPCWSTR>(IDC_IBEAM);   break;  // 输入框
            case 3: id = reinterpret_cast<LPCWSTR>(IDC_SIZENS);  break;  // 竖直滚动条 / 上下边
            case 4: id = reinterpret_cast<LPCWSTR>(IDC_SIZEWE);  break;  // 水平滚动条 / 左右边
            case 5: id = reinterpret_cast<LPCWSTR>(IDC_SIZENWSE); break; // 左上-右下角
            case 6: id = reinterpret_cast<LPCWSTR>(IDC_SIZENESW); break; // 右上-左下角
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
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            // 与 DOM MouseEvent.button 同号：0=左 1=中 2=右
            const int btn = (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2 : 1;
            const bool down = (msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
            if (down) SetFocus(hwnd);
            if (app->btn_) app->btn_(static_cast<float>(LOWORD(lParam)),
                                     static_cast<float>(HIWORD(lParam)), btn, down);
            return 0;
        }
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
inline void BinUIAppT<Gfx>::markAllDirty() {
    dirty_ = true;
    dirtyL_ = 0; dirtyT_ = 0; dirtyR_ = width_; dirtyB_ = height_;
}

template <class Gfx>
inline void BinUIAppT<Gfx>::invalidateAll() {
    markAllDirty();
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
    binui::ex::StartupMark("open done (window visible)");
    // 空闲时的醒来间隔 = 一个帧预算（以前固定 1ms）。输入本来就会立刻唤醒
    // MsgWaitForMultipleObjectsEx，1ms 纯属"每秒空转 1000 次"——稳态每帧的 update_
    // 都要跑一遍（宿主在 update_ 里才能决定脏区），于是后台闲着也烧 0.6% 一个核。
    // 用帧预算当上限：动画粒度不变（120Hz 屏 8ms、60Hz 屏 16ms），空转换醒少 8~16 倍。
    const int idleMs = static_cast<int>(frameBudget_.count() / 1000) + 1;
    while (running_) pump(idleMs);
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
    binui::ex::StartupMark("open: DPI+class");

    RECT work = { 0, 0, 800, 600 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    binui::ex::StartupMark("open: SPI_GETWORKAREA");
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
    binui::ex::StartupMark("open: CreateWindowEx");
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
    // 节拍策略要在 init **之前**交下去：交换链的 vsync 位是建链时定的，
    // 而 "unlimited"/"240" 这类策略不开 vsync 才有意义（见 bingpu_pace_wants_vsync）。
    gfx_.setPacePolicyText(PaceTextFromEnv());
    if (!gfx_.init(hwnd, winW, winH, vsync_)) {
        std::fprintf(stderr, "[gl] OpenGL init failed\n");
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        return false;
    }
    binui::ex::StartupMark("open: gfx.init total");
    // WS_VISIBLE 只保证"可见"，SetFocus 只改本线程键盘焦点 —— 都不会把窗口提到
    // 前台，于是从控制台启动时窗口被 cmd 压在后面。这里补一次显式前台化。
    BinUIForceForeground(hwnd);
    SetFocus(hwnd);
    width_ = winW;
    height_ = winH;
    invalidateAll(); // 首帧全量绘制（初始脏矩形为空，避免首帧仅动画区域被绘制，其余留白/留黑）

    lastTime_ = std::chrono::steady_clock::now();
    setupPacing(hwnd);
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
    gfx_.setPacePolicyText(PaceTextFromEnv());
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
    setupPacing(hwnd);
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
    // 最小化：窗口根本不可见，不推进 update_、也不画。必须同时清脏区和 lastDrew_，
    // 否则 pump 认为"还有活"→ present 又不阻塞（窗口不可见）→ 空转吃满一个核。
    if (hwnd_ && IsIconic(static_cast<HWND>(hwnd_))) {
        lastTime_ = now;
        dirty_ = false;
        lastDrew_ = false;
        return;
    }
    // 节拍闸门：**宿主时钟也按帧跑**，别被 pump 的空转带着跑。
    // 以前 renderFrame 第一件事就是 update_(dt)，而 run() 是 while (pump(1)) ——
    // 闲着的时候宿主每秒被推进 1000 次（每次都算一遍漂移/重排判定），任务管理器里
    // "放后台还占 0.6~7%" 就是这么来的。现在：有脏区立刻跑，否则最多每帧预算一次。
    if (!dirty_ && pendingW_ <= 0 && now - lastTime_ < frameBudget_) {
        lastDrew_ = false;   // 这一拍照样没活：必须清掉，否则 pump 一直认为"还有活"→ 自旋
        // 同时把唤醒点摆到"本拍预算用完"的那一刻。**不能留着上一拍的旧值**：pump 只在
        // "nextFrameTime_ 还在未来"时才睡，留一个过去的时刻等于不睡 —— 醒来又进这里，
        // 一秒几十万次空转（实测就是 100% 单核、0 帧）。这是宿主时钟的节拍，不是帧节拍：
        // 真出帧的节拍由设备层（binGPU 的 pace*）给，见 renderOnce 之后那段。
        using Clock = std::chrono::steady_clock;
        nextFrameTime_ = lastTime_ + frameBudget_;
        if (nextFrameTime_ <= now) nextFrameTime_ = now + std::chrono::microseconds(1000);
        return;
    }
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
    // 帧节拍（v0.32.0）：**统一按刷新网格量化**，不再分"present 掐拍 / 外壳掐拍"两条路。
    //   旧写法分两条：（A）present 会等合成 → 外壳完全不管；（B）否则外壳按"现在 + 预算"。
    //   实测两条都有洞：
    //     · A 依赖"交换链声明 vsync == 驱动真的阻塞"。DWM 合成下 wglSwapBuffers(1) 常常
    //       立刻返回，外壳于是彻底放手，帧落点自由漂移；
    //     · B 的"现在 + 预算"只保证**至少**隔一个预算，落点相对刷新边界任意。
    // 节拍闸门（v0.34.0）：**问后端"还差多久"，不再自己算网格**。
    //
    // 老写法（v0.32.0）在外壳里按刷新网格量化 + 滞回，还要用
    // `present 耗时 >= 半个刷新周期` 猜"它自己等过没有"。那个启发式在 Vulkan 上猜错，
    // 于是 present 等一格、外壳再补一格 = **双重等待**：120 Hz 屏实测 55 fps，
    // 而同一份代码裸跑 present 是 119.9 fps（bin2D/build-mingw/tmp/present_probe）。
    // 判据（刷新率 / present mode / present 真实耗时）只有设备层知道，所以闸门搬到了
    // binGPU（b2d::GpuCompositor → bingpu_pace_*），外壳只剩"问 + 睡"。
    const bool byDevice = gfx_.paceAvailable();
    if (byDevice) {
        const long long waitUs = gfx_.paceWaitUs();
        if (waitUs > 0) {
            // 睡完就画，**不再复查**：定时器粒度 ~0.5 ms，早醒最多早半个毫秒，
            // 而下一拍是"本拍开始 + 目标周期"的绝对值，早一点不会累积漂移。
            // 复查过的版本会多走一趟 pump（消息泵 + 闸门重算），实测把 240 fps
            // 拉到 169 —— 每帧白扔 1.7 ms，全是醒来之后的往返。
            waitForFrame(now + std::chrono::microseconds(waitUs));
        }
    } else if (now < nextFrameTime_) {
        return;
    }
    renderOnce();
    if (byDevice) {
        // 设备层节拍器接管：**下一拍的唤醒点必须在这里落下来**。
        //
        // 以前这里直接 return，`nextFrameTime_` 就永远停在建窗那一刻。pump 的两条等待
        // 分支都要求"下一拍还在未来"才会睡（见 pump），于是从第二拍起它全程不睡：
        //   while (running_) pump(9) → PeekMessage → renderFrame（没脏区，秒退）→ 再一遍
        // 一秒空转几十万次，一个核吃满 —— 而这一秒里**一帧都没画**（BINUI_PERF 的
        // frames drawn 停在 1）。实测 buttonshow / fontshow 100%，就是这条自旋。
        //
        // 两种唤醒点：
        //   · 这一拍真画了 → 问节拍器还差多久（binGPU 的 bingpu_pace_wait_us）；
        //   · 这一拍没画（没脏区）→ 按帧预算醒来。这不是"为了画"，是为了推进宿主时钟：
        //     动画的脏区只能在 update_ 里出现（见上面节拍闸门的注释），不醒就永远不会开始。
        using Clock = std::chrono::steady_clock;
        const auto endT = Clock::now();
        long long waitUs;
        if (lastDrew_) {
            // 真画了一拍：**只问节拍器**。0 = "立刻可以再画"（present 已经等过垂直同步，
            // 或这一拍已经超过目标周期）—— 这时候**不许**自己补一帧预算，否则就是双重等待：
            // 实测 120 Hz 屏上这一下正好把 animshow 从 119.7 fps 拉到 68 fps。
            waitUs = gfx_.paceWaitUs();
            if (waitUs < 0) waitUs = 0;
        } else {
            // 这一拍没画：按帧预算醒来，只为了推进宿主时钟（见上一条注释）。
            waitUs = static_cast<long long>(frameBudget_.count());
        }
        nextFrameTime_ = endT + std::chrono::microseconds(waitUs);
        return;
    }

    // 下一拍的时间点（v0.32.0：**刷新网格**，不是"现在 + 预算"）。
    //
    // 为什么要网格：120Hz 屏刷新周期 8.33ms，一帧 19.3ms = 2.3 个周期。旧写法
    // `now + 预算` 只保证"至少等一个预算"，帧落点相对刷新边界**自由漂移**：实测
    // 间隔落在 1.75/2.00/2.25/2.50/2.75 个刷新周期上各有一堆，同一帧有时被合成 2 次、
    // 有时 3 次 —— 平均帧率还行，肉眼就是一顿一顿。
    // 现在把下一拍钉在**整数个刷新周期**之后：帧耗时 19.3ms → 占 3 个周期（25ms），
    // 每帧稳定吃 3 个刷新，宁可慢一点也不抖。
    //
    // 基准必须是**本拍真正结束的时刻**（含 present 可能等到的那一截），不是本拍开始
    // 的时刻：从开始算的话，帧耗时一过整格，算出来的"下一拍"已经在过去，
    // pump 立刻返回，帧间隔就退化成"纯耗时"，又漂回非整数格。
    using Clock = std::chrono::steady_clock;
    const auto endT = Clock::now();
    // 这一帧 present 是不是自己阻塞等过刷新边界（见下面 sleepSteps）
    const bool presentWaited = lastPresentMs_ >= refreshMs_ * 0.5;
    {
        const long long gridUs = static_cast<long long>(refreshMs_ * 1000.0 + 0.5);
        const auto grid = std::chrono::duration_cast<Clock::duration>(
            std::chrono::microseconds(gridUs > 0 ? gridUs : 16667));
        // BINUI_FPS 封顶（比刷新慢）时，一拍要占**整数个**刷新周期 —— 60fps 落在 120Hz
        // 屏上就是 2 个周期，不能是 16.67ms 这种"1.999 个周期"的自由落点。
        const long long budgetUs = static_cast<long long>(frameBudget_.count());
        // 四舍五入到最近的整档：8.3333ms 写成整数 µs 是 8333，直接 ceil 会把
        // "33.33ms = 4 个刷新"算成 5 个（(33333+8333-1)/8333 恰好进位）—— 实测
        // BINUI_FPS=30 因此变成了 5 个周期（41.7ms）。
        long long minSteps = gridUs > 0 ? (budgetUs + gridUs / 2) / gridUs : 1;
        if (minSteps < 1) minSteps = 1;
        Clock::time_point t = nextFrameTime_;
        // 落后太多（被挂起 / 窗口被拖走 / 长 stall / 首次）就以"现在"为基准重新学：
        // 别一格一格追（那会连续空转几百拍），也别把 96 个周期的停顿当成新节拍。
        if (t < endT - grid * 4 || t > endT) {
            t = endT - grid;
            frameSteps_ = 0;
        }
        // 本拍真正占了多少个刷新周期
        const double need = grid.count() > 0
                                ? static_cast<double>((endT - t).count()) /
                                      static_cast<double>(grid.count())
                                : 1.0;
        long long k = frameSteps_ > 0 ? frameSteps_
                                      : static_cast<long long>(std::ceil(need > 0.0 ? need : 1.0));
        // 滞回（v0.32.0）：装不下立刻升档；**降档要求本拍真的装得下更小的档**（留 15% 余量）。
        //   没有滞回时，帧耗时刚好压在"2 个周期"的边界上会 2/3 交替 —— 那本身就是
        //   一顿一顿（实测 appshow：2.25/2.75/3.00 三档混着来，抖动 112/179）。
        //   宁可多睡一点、节拍不动，也不要每帧换一个周期数。
        //   注意"降档"的判据是 `need <= k-1`（本拍能在 k-1 个周期内画完），
        //   不是 `need <= k-1+余量` —— 后者会让 k=1 却要在 1.2 个周期里画完，
        //   结果是每帧都迟到、节拍反而更烂（踩过，实测退成 ~10ms 的杂乱节拍）。
        if (need > static_cast<double>(k)) k = static_cast<long long>(std::ceil(need));
        else if (k > 1 && need <= static_cast<double>(k - 1) - 0.15) --k;
        if (k < minSteps) k = minSteps;
        if (k < 1) k = 1;
        if (k > 512) k = 512;
        frameSteps_ = k;
        // present 自己已经等掉一拍时（它阻塞到了刷新边界，实测 ≥ 半个刷新周期），
        // 下一拍就**不要再补一整格** —— 否则就是双重等待：VK 后端 FIFO 交换链实测
        // present 7.9 ms、帧耗时 8.33 ms，却按 2.03 个刷新周期出帧（120fps 掉成 60）。
        // 判据用**实测**的 present 耗时，不用后端声明；GDI/贴屏那条路 present 很快，
        // 不会被误判。
        const long long sleepSteps = (presentWaited && k > 1) ? k - 1 : k;
        nextFrameTime_ = t + grid * sleepSteps;
    }
}

// present 自己会不会等到垂直同步（**只用于启动时打一行诊断**）。
// v0.32.0 起帧相位不再交给它：交换链声明 vsync ≠ 驱动真的阻塞（DWM 合成下
// wglSwapBuffers(1) 经常立刻返回），所以节拍统一由外壳按刷新网格量化，
// present 等不等都不影响稳定性 —— 这一项留着是为了解释"慢在等合成还是慢在画"。
template <class Gfx>
inline bool BinUIAppT<Gfx>::pacedByPresent() const {
    return gfx_.presentPacesFrames() && !gdiPresent_ && !fullscreenPresent_;
}

// 节拍参数定下来（v0.34.0）。
//
// 两条路的区别只有"谁算"：
//   · 后端能回答 paceAvailable()（= bin2D/bin3D → binGPU 的节拍器）→ 目标周期由它给，
//     外壳只负责睡；
//   · 不能（仓里已删除的 gl/vk/d3d11 三份实现就是这种，见 docs/backend_layering.md 第 3 步）
//     → 外壳按刷新率兜底。
// `frameBudget_` 同时是"宿主时钟多久推进一次"：unlimited（0ms）时它退化成 0，
// 于是 update_ 被尽可能频繁地调用 —— 这正是"无限制"的含义。
template <class Gfx>
inline void BinUIAppT<Gfx>::setupPacing(HWND hwnd) {
    const int panelHz = DisplayRefreshHzFor(hwnd);
    const bool byDevice = gfx_.paceAvailable();
    if (byDevice) {
        const double paceMs = gfx_.paceFrameMs();
        const double devRefreshMs = gfx_.paceRefreshMs();
        frameBudget_ = std::chrono::microseconds(
            paceMs > 0.0 ? static_cast<long long>(paceMs * 1000.0 + 0.5) : 0);
        // 抖动指标要用**显示器刷新周期**，不是帧预算：`BIN2D_PACE=30` 之后两者不同，
        // 用预算当刻度会把"4 个刷新的稳定 30fps 节拍"算成 1.00 个周期（看着很稳，其实 30fps）。
        refreshMs_ = devRefreshMs > 0.0 ? devRefreshMs : 1000.0 / panelHz;
    } else {
        frameBudget_ = std::chrono::microseconds(1000000 / FrameHzFor(hwnd));
        refreshMs_ = 1000.0 / panelHz;
    }
    if (!Perf().on) return;
    const double budgetMs = frameBudget_.count() / 1000.0;
    if (byDevice) {
        if (frameBudget_.count() > 0)
            std::fprintf(stderr,
                         "[perf] display refresh=%dHz 节拍=%s（binGPU 节拍器，目标 %.2fms ≈ %d fps；"
                         "present 自等垂直同步=%s）\n",
                         panelHz, gfx_.pacePolicyName(), budgetMs,
                         static_cast<int>(1000.0 / budgetMs + 0.5),
                         pacedByPresent() ? "是" : "否");
        else
            std::fprintf(stderr,
                         "[perf] display refresh=%dHz 节拍=%s（binGPU 节拍器，**不掐拍**；"
                         "present 自等垂直同步=%s）\n",
                         panelHz, gfx_.pacePolicyName(), pacedByPresent() ? "是" : "否");
    } else {
        std::fprintf(stderr,
                     "[perf] display refresh=%dHz 节拍=%s（外壳兜底网格 %.2fms；"
                     "present 自等垂直同步=%s）\n",
                     panelHz, gfx_.pacePolicyName(), budgetMs, pacedByPresent() ? "是" : "否");
    }
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
        /* 实际迟到是**稳定的 ~1.0 ms**，不是"分辨率不够"而是"对齐到 ~1 ms tick"：
         *   实测（build-mingw/tmp/timer_probe）：请求 1/2/4/8/16 ms → 中位迟到
         *   967/999/1000/949/821 µs；而请求 500 µs → 只迟到 16 µs。
         * 所以请求时间**提前 900 µs**：醒来落在目标附近（±0.1 ms），一帧 2.4 ms 的
         * 抖动就没了。提前醒不会累积漂移（下一拍是"本拍开始 + 周期"的绝对值），
         * vsync 下 present 也会自己等到刷新边界 —— 白等的时间它本来就吃掉了。
         * 不这么做的后果是实测出来的：240 fps 档只能跑到 163 fps（每帧多睡 1.9 ms），
         * 60 fps 档跑到 57.8。 */
        auto req = remain;
        if (req > microseconds(900)) req -= microseconds(900);
        LARGE_INTEGER due;
        // 负值 = 相对时间，单位 100ns
        due.QuadPart = -static_cast<LONGLONG>(duration_cast<nanoseconds>(req).count() / 100);
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
    // 等待：有挂起 resize 就立刻（尺寸要跟手）；有活要干就按节拍等；都没有就睡到输入。
    // 等"到下一帧点"必须精确到亚毫秒 —— 120Hz 的预算只有 8.33ms，而 MsgWait 的毫秒
    // 超时按系统计时器精度兑现（默认 15.6ms），一睡就跨两个刷新（v0.11.6 的坑）。
    MSG msg = {};
    if (pendingW_ > 0 && pendingH_ > 0) {
        // 尺寸变化：这一拍就出帧，不等
    } else if (dirty_ || lastDrew_) {
        // 有脏区，或者上一拍刚画过（动画通常每拍都会产生脏区，但那要等 update_ 跑完
        // 才知道 —— 用 lastDrew_ 提前判断，免得在帧点前先睡掉一整拍）。
        // 统一精确等到下一拍（刷新网格上的那个点），pump 用高精度定时器，输入会提前唤醒。
        const auto now = std::chrono::steady_clock::now();
        waitForFrame(nextFrameTime_ > now ? nextFrameTime_ : now);
    } else if (timeoutMs > 0) {
        // 完全没活：也睡到帧点（v0.32.0：以前 paced 时用毫秒粗睡省电，但那样醒来时刻
        // 落在 15.6ms 的 OS 节拍上，动画一开动就要多漂半拍；有空转闸门在，粗睡省不了多少）。
        //
        // v0.35.0：**必须保证真睡**。这条分支的旧写法是"nextFrameTime_ 在未来才睡"，
        // 于是只要上游少更新一次唤醒点（设备层节拍器接管后漏更新，就是那个 100% 单核的
        // bug），这里就退化成纯自旋。现在：帧点在未来就睡到帧点；帧点已经过去（不该发生，
        // 属于上游漏更新）按调用方给的超时粗睡 —— 兜底是 ~120 次/秒，不是几十万次。
        // 注意**不能**用"now + 一帧预算"当兜底：定时器会早醒不到 1ms，那 1ms 的早醒会让
        // 闸门提前返回，再垫一整帧预算 → 实测 animshow 从 119.7 fps 掉到 68 fps（1.75 个
        // 刷新周期/帧）。这里的兜底只在唤醒点真的过期时才生效。
        const auto now = std::chrono::steady_clock::now();
        const auto wake = nextFrameTime_ > now
                              ? nextFrameTime_
                              : now + std::chrono::milliseconds(timeoutMs);
        waitForFrame(wake);
    }

    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { running_ = false; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!running_) return 1;

    renderFrame();
    resizeBenchTick();   // 没设 BINUI_RESIZE_BENCH 时是一次静态判断，零开销
    return running_ ? 0 : 1;
}

template <class Gfx>
inline void BinUIAppT<Gfx>::close() {
    running_ = false;
    if (Perf().on && Perf().drawn > 0) {
        PerfFlush();   // 短跑（BINUI_FRAME_EXIT）也要一份统计，否则只有 exit 一行空话
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

// ============ 拖动 resize 压测（BINUI_RESIZE_BENCH=n）============
//
// "拖边框时卡"和"启动慢"是两件事：前者每到一次 WM_SIZE 就要重排整棵树 + 后端重建
// 交换链/离屏目标。三个后端共用这份外壳，所以压测也放在外壳里做 A/B：
// 每帧用 SetWindowPos 把窗口宽度抖动 ±40px，逼出"每个 WM_SIZE 都重建"这条最坏路径，
// 画满 n 帧（默认 60）后打印两段耗时 —— 布局（宿主回调）与后端（gfx_.resize）分开算，
// 这样能直接看出是谁慢。见 docs/startup.md §5。
struct ResizeBenchStats {
    int frames = 0;
    int baseW = 0;
    int baseH = 0;
    double sumLayout = 0, maxLayout = 0;
    double sumGfx = 0, maxGfx = 0;
};
inline ResizeBenchStats& ResizeBench() {
    static ResizeBenchStats s;
    return s;
}
// 0 = 关闭（一次 getenv 的静态判断后就是常数）
inline int ResizeBenchFrames() {
    static const int n = [] {
        const char* e = std::getenv("BINUI_RESIZE_BENCH");
        if (!e || !*e) return 0;
        const int v = std::atoi(e);
        return v > 0 ? v : 60;   // BINUI_RESIZE_BENCH=1 也按 60 帧
    }();
    return n;
}

template <class Gfx>
inline void BinUIAppT<Gfx>::applyPendingResize() {
    if (pendingW_ <= 0 || pendingH_ <= 0) return;
    int w = pendingW_, h = pendingH_;
    pendingW_ = pendingH_ = 0;
    // 拖边框排查：确认 WM_SIZE 挂起的尺寸真的走到了这里（BINUI_RESIZE_TRACE=1）
    {
        static const bool rt = std::getenv("BINUI_RESIZE_TRACE") != nullptr;
        if (rt)
            std::fprintf(stderr, "[app][resize] apply %dx%d（gfx ready=%d）\n", w, h,
                         gfx_.isReady() ? 1 : 0);
    }
    width_ = w;
    height_ = h;
    const bool bench = ResizeBenchFrames() > 0;
    const double tL0 = bench ? NowMs() : 0.0;
    if (resize_) resize_(w, h);
    const double tL1 = bench ? NowMs() : 0.0;
    gfx_.resize(w, h);
    if (bench) {
        ResizeBenchStats& s = ResizeBench();
        const double tG = NowMs() - tL1;
        const double tL = tL1 - tL0;
        s.sumLayout += tL; if (tL > s.maxLayout) s.maxLayout = tL;
        s.sumGfx += tG;    if (tG > s.maxGfx)    s.maxGfx = tG;
    }
    // 客户区铺满显示器 → 绕开直翻转（DWM 会把呈现提升成直翻转，远程/虚拟显示器上会停帧）
    fullscreenPresent_ = clientCoversMonitor();
    // 远程会话（RDP）本身也走读回贴屏：这条路径在虚拟显示器上是实测唯一稳定的。
    // 本机没有 RDP 会话时用 BINUI_FORCE_REMOTE=1 模拟（见 RemoteSession）。
    if (RemoteSession()) fullscreenPresent_ = true;
    invalidateAll();
}

template <class Gfx>
inline void BinUIAppT<Gfx>::resizeBenchTick() {
    const int total = ResizeBenchFrames();
    if (total <= 0) return;
    ResizeBenchStats& s = ResizeBench();
    if (s.frames == 0) {
        s.baseW = width_;
        s.baseH = height_;
    }
    ++s.frames;
    // 每帧换一次尺寸（±40px）：WM_SIZE 一路走到 applyPendingResize，交换链/离屏每帧重建
    const int w = s.baseW + ((s.frames & 1) ? 40 : -40);
    SetWindowPos(static_cast<HWND>(hwnd_), nullptr, 0, 0, w, s.baseH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s.frames >= total) {
        const int n = s.frames;
        std::fprintf(stderr,
                     "[resize-bench] %d 次 resize（每帧 ±40px，%dx%d 基准）\n"
                     "[resize-bench]   布局(宿主回调) avg=%.2f max=%.2f ms\n"
                     "[resize-bench]   后端 gfx.resize avg=%.2f max=%.2f ms\n",
                     n, s.baseW, s.baseH,
                     n ? s.sumLayout / n : 0.0, s.maxLayout,
                     n ? s.sumGfx / n : 0.0, s.maxGfx);
        std::fflush(stderr);
        s = ResizeBenchStats{};
        requestExit();
    }
}

template <class Gfx>
inline void BinUIAppT<Gfx>::renderOnce() {
    FramePerf& perf = Perf();
    const double t0 = perf.on ? NowMs() : 0.0;
    // 首帧分段：BINUI_STARTUP 时把"第一帧"再拆成 beginFrame / draw_ / present 三截，
    // 否则只知道"首帧花了 300ms"，不知道是遍历上传慢还是 SwapBuffers 冷启动慢。
    const bool sp = binui::ex::StartupOn();
    static int probeFrame = -1;   // -1 = 还没出过帧
    applyPendingResize();
    const double t1 = perf.on ? NowMs() : 0.0;
    lastDrew_ = false;
    if (dirty_ && gfx_.isReady()) {
        const bool firstDraw = (probeFrame < 0);
        if (sp && firstDraw) binui::ex::StartupMark("frame1: begin");
        lastDrew_ = true;
        int rc[4] = { dirtyL_, dirtyT_, dirtyR_, dirtyB_ };
        gfx_.beginFrame(rc);
        if (sp && firstDraw) binui::ex::StartupMark("frame1: beginFrame (msaa)");
        if (draw_) draw_(gfx_, rc);
        if (sp && firstDraw) binui::ex::StartupMark("frame1: draw_ (verts)");
        const double t2 = perf.on ? NowMs() : 0.0;
        // 拖拽 resize 期与"客户区铺满显示器"时都走 GDI 贴屏：
        // 前者绕开 NVIDIA 在 resize+SwapBuffers 组合下的崩溃，
        // 后者绕开全屏直翻转导致的"画面停住"（见 clientCoversMonitor 注释）。
        if ((gdiPresent_ || fullscreenPresent_) && !transparent_) gfx_.presentViaGdi();
        else {
            // v0.11.9：present 之后要不要再等一次合成器，**按实测决定**。
            //   · GL / D3D11 的 SwapBuffers / Present(1) 在 DWM 合成下往往立刻返回
            //     （实测 0.3ms），帧就会被同一个刷新周期里的两次合成"挤掉一帧" —— 必须 Flush；
            //   · Vulkan 用 2 图交换链时 acquire 本身就卡在合成边界（实测 8ms+），
            //     再 Flush 就是**双重等待**（实测 15.4ms = 两个合成周期，直接掉到 64fps）。
            // 所以判据不是"哪个后端"，而是"present 刚才到底等了多久"。
            const auto pStart = std::chrono::steady_clock::now();
            gfx_.present();
            lastPresentMs_ = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - pStart).count();
            // 相位锚定：**只在相位未知时**补一次 DwmFlush（见 anchorPhase_）。
            // 每帧都 flush 会把"本拍结束时刻"硬贴到合成边界上，代价是每帧多等最多一个
            // 刷新周期 —— 实测 40fps 与 60fps 的差别就来自这一下，而它解决不了网格
            // 已经解决的东西（帧落点仍然只在每帧被重新锚定一次）。
            // v0.34.0：设备层有节拍器时不需要这一下 —— 它自己就是按刷新边界算的，
            // 外壳再 Flush 一次等于又插一次等待（正是"双重等待"那类问题）。
            if (!gfx_.paceAvailable() && !phaseAnchored_ && lastPresentMs_ < refreshMs_ * 0.5) {
                DwmFlush();
                phaseAnchored_ = true;
            }
        }
        if (sp && firstDraw) {
            binui::ex::StartupMark("frame1: present done");
            probeFrame = 0;
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
                // 掉拍阈值要按**目标周期**算；unlimited（预算 0）时这一项没有意义，跳过
                if (frameBudget_.count() > 0 &&
                    iv > frameBudget_.count() / 1000.0 * 1.5) ++perf.missed;
                // 抖动：把间隔换算成"显示刷新周期数"，接近整数才算跟住了刷新节拍。
                // 1.5 个预算以上的掉拍通常就是"多占了 1~2 个刷新"，这项会把它点出来。
                const double refreshMs = refreshMs_ > 0.0 ? refreshMs_ : 1.0;
                const double periods = iv / refreshMs;
                if (periods >= 0.7) {
                    const double frac = std::fabs(periods - std::floor(periods + 0.5));
                    if (frac > 0.15) ++perf.judder;
                    ++perf.ivN;
                    perf.sumPeriods += periods;
                    if (periods < perf.minPeriods) perf.minPeriods = periods;
                    if (periods > perf.maxPeriods) perf.maxPeriods = periods;
                }
                // 直方图按"刷新周期"分档（见 FramePerf::hist）
                int slot = static_cast<int>(periods * 4.0);
                if (slot < 0) slot = 0;
                if (slot > 40) slot = 40;
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
    // 启动分段 + 自动退出（量测/截图用）：
    //   BINUI_STARTUP → 首帧画完的绝对耗时（窗口可见到第一帧真正上屏）
    //   BINUI_FRAME_EXIT=n → 画满 n 帧就正常退出，免得外部 Stop-Process 强杀把
    //   还在缓冲区里的诊断输出一起带走
    if (lastDrew_) {
        static const int frameExit = [] {
            const char* e = std::getenv("BINUI_FRAME_EXIT");
            return e && *e ? std::atoi(e) : 0;
        }();
        static int startupFrames = 0;
        ++startupFrames;
        if (frameExit > 0) {
            // 强制连出 n 帧：静态页面本来只画 1 帧（没脏区就不出帧），n>1 就成了"永远不退出"。
            // 量测要的是"画够 n 帧自己退"，不是外部 Stop-Process 强杀（那样缓冲里的诊断会丢）。
            if (startupFrames >= frameExit) requestExit();
            else markAllDirty();   // 强制再画一帧，但**不动节拍相位**（量测要的是真实间隔）
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
