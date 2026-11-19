// test_shell.cpp - 后端无关外壳（BinUIAppT）跑在 bin2d 后端上的回归测试
//
// 这个套件要回答的问题只有一个：**三份 GPU 后端能不能删**。
//
// 它们留到现在的唯一理由是"输入 / IME / 拖边框 / DPI 只在 GL/VK/D3D11 上真跑过"。
// 但外壳（src/backends/binui_app.h）跟图形 API 一点关系都没有 —— 它只要求后端提供
// IBinUIGfx 的 8 个方法 + appClassName()。所以真正要验证的不是"bin2d 画得对不对"
// （那是 pixels-bin2d，容差 0），而是：
//
//   1. 窗口：子窗口模式下能建窗、能吃 WM_SIZE / WM_DPICHANGED；
//   2. 消息：鼠标移动/按下/抬起/双击/右中键/滚轮/离开、WM_CHAR（含代理对）、
//      WM_KEYDOWN 全部按客户区坐标交给宿主；
//   3. 输入法：编辑态切换时 IME 上下文能真的挂上/摘下（不是空调用）；
//   4. 帧：beginFrame(脏区) → draw → present 三段落在地上，且**只清脏区** ——
//      脏区外的像素必须保留上一帧的值（这是 GL 后端一直隐含的行为，bin2d 后端
//      必须逐像素一样，否则窗口一抖动就整屏闪）；
//   5. 图层 / 裁剪栈：组透明度真的乘了一次 alpha，popClipTo 真的恢复了裁剪；
//   6. 生命周期：pump 跨线程被拒、close 幂等、同进程两个实例并存不打架。
//
// 为什么不测"看见的画面"：无窗口环境下没有肉眼。像素级回归在 make pixels-bin2d
// （18 个用例，容差 0），真窗口在 tools/bin2d_window 与 examples/appshow_2d。
// 这一份守的是**接口契约**，可以进 CI。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cwchar>
#include <string>
#include <thread>

#include <windows.h>

#include "backends/binui_bin2d.h"

using namespace binui;

static int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            std::fprintf(stderr, "[FAIL] %s\n", (msg));     \
            ++g_fail;                                       \
        } else {                                            \
            std::printf("[ok] %s\n", (msg));                \
        }                                                   \
    } while (0)

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

// 宿主父窗口：不给 WS_VISIBLE，子窗口跟着不显示 —— 测试要能无感地跑在 CI 里。
HWND MakeHiddenParent() {
    static bool reg = false;
    const wchar_t* kCls = L"BinUIShellTestParent";
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.lpszClassName = kCls;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassExW(&wc)) return nullptr;
        reg = true;
    }
    return CreateWindowExW(0, kCls, L"", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr,
                           nullptr, inst, nullptr);
}

// 子窗口模式下消息由宿主循环派发（外壳在 pump 里**不会** PeekMessage，
// 见 binui_app.h 的注释），所以这里替宿主的循环收消息。
//
// 那句 sleep 不是凑数：外壳自己有帧节拍闸门（renderFrame 里 `now < nextFrameTime_`
// 就整帧跳过）。bin2d 后端的 present 不阻塞垂直同步 → paced=false → 节拍完全由外壳
// 按刷新率推算。不等这一拍就 pump，读到的是"上一帧还挂着"，脏区测试会全部落空。
void PumpOnce(BinUIApp2D& app) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    app.pump(1);
}

// 像素读取（预乘 RGBA8，行紧排）
struct Px {
    uint8_t r, g, b, a;
    bool eq(int rr, int gg, int bb, int aa) const {
        return r == rr && g == gg && b == bb && a == aa;
    }
    // 允许每通道 ±1（bin2d 的混合在 8 位里做舍入）
    bool approx(int rr, int gg, int bb, int aa) const {
        auto d = [](uint8_t v, int e) { return v >= e - 1 && v <= e + 1; };
        return d(r, rr) && d(g, gg) && d(b, bb) && d(a, aa);
    }
};

Px At(const BinUI2D& g, int x, int y) {
    const uint8_t* p = static_cast<const uint8_t*>(g.pixels());
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(g.width()) +
                      static_cast<size_t>(x)) * 4u;
    return Px{ static_cast<uint8_t>(p ? p[i] : 0), static_cast<uint8_t>(p ? p[i + 1] : 0),
               static_cast<uint8_t>(p ? p[i + 2] : 0), static_cast<uint8_t>(p ? p[i + 3] : 0) };
}

// 绘制回调：按 phase 画不同的东西，并记下外壳这次给的脏矩形。
struct Probe {
    int  phase = 0;
    int  calls = 0;
    int  clip[4] = { -1, -1, -1, -1 };
    bool layerOk = false;

    void Draw(IBinUIGfx& gfx, const void* clipPtr) {
        ++calls;
        const int* rc = static_cast<const int*>(clipPtr);
        for (int i = 0; i < 4; ++i) clip[i] = rc ? rc[i] : -1;
        switch (phase) {
            case 0:
                // 铺满：0xFF3366CC 全不透明 → 预乘后字节序正好等于直通值
                gfx.fillRect(0.0f, 0.0f, 400.0f, 400.0f, 0xFF3366CCu);
                break;
            case 1:
                // 超出窗口的矩形：只应落在脏区里（脏区外由 beginFrame 负责保留）
                gfx.fillRect(0.0f, 0.0f, 400.0f, 400.0f, 0xFFFF0000u);
                break;
            case 2:
                // 组透明度 50%：黑层合成到 0xFF3366CC 上。
                // 背景必须**先画**：beginFrame(整窗脏区) 会先把画布清成透明，
                // 只画图层的话合成出来是 (0,0,0,128)（黑 50% over 透明），
                // 那是正确行为，不是这条用例要测的东西。
                gfx.fillRect(0.0f, 0.0f, 400.0f, 400.0f, 0xFF3366CCu);
                if (gfx.pushLayer(0.0f, 0.0f, (float)kW, (float)kH, 0.5f)) {
                    gfx.fillRect(0.0f, 0.0f, (float)kW, (float)kH, 0xFF000000u);
                    gfx.popLayer();
                    layerOk = true;
                }
                break;
            case 3: {
                // 裁剪栈：pushClip 内部填绿，popClipTo 之后再填蓝
                const int d = gfx.pushClip(10.0f, 20.0f, 30.0f, 40.0f);
                gfx.fillRect(0.0f, 0.0f, 400.0f, 400.0f, 0xFF00FF00u);
                gfx.popClipTo(d);
                gfx.fillRect(0.0f, 0.0f, 5.0f, 5.0f, 0xFF0000FFu);
                break;
            }
            default:
                break;
        }
    }
};

} // namespace

int main() {
    // 这个套件测的是外壳契约，不是 GPU 上屏：强制走 GDI 贴屏，免得
    // "隐藏窗口上建交换链"这种无关变量把结果搅浑（GPU 呈现由
    // tools/bin2d_window 与 make pixels-bin2d 负责）。
    _putenv("BINUI2D_NO_GPU=1");

    HWND parent = MakeHiddenParent();
    CHECK(parent != nullptr, "宿主父窗口建成（隐藏）");
    if (!parent) {
        std::printf("\nFAILED (no parent window)\n");
        return 1;
    }

    // ---------------- 1. 建窗 + 参数校验 ----------------
    std::printf("\n--- 1. openChild 契约 ---\n");
    {
        BinUIApp2D bad(kW, kH);
        CHECK(!bad.openChild(nullptr, 0, 0, 100, 100), "openChild(parent=nullptr) 返回 false");
    }
    {
        BinUIApp2D bad(kW, kH);
        CHECK(!bad.openChild(parent, 0, 0, 0, 100), "openChild(宽=0) 返回 false");
    }

    BinUIApp2D app(kW, kH);
    Probe probe;
    app.setDrawFn([&probe](IBinUIGfx& gfx, const void* rc) { probe.Draw(gfx, rc); });

    CHECK(app.openChild(parent, 0, 0, kW, kH), "openChild(BinUIApp2D) 成功");
    CHECK(app.gfx().isReady(), "后端 isReady");
    CHECK(app.gfx().width() == kW && app.gfx().height() == kH, "后端尺寸 = 请求尺寸");

    {
        // 子窗口对象在外壳内部，用类名一致性间接验证"两套后端同进程不打架"
        HWND child = nullptr;
        EnumChildWindows(
            parent,
            [](HWND h, LPARAM p) -> BOOL {
                *reinterpret_cast<HWND*>(p) = h;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&child));
        wchar_t got[128] = {};
        if (child) GetClassNameW(child, got, 128);
        CHECK(child != nullptr && wcscmp(got, BinUI2D::appClassName()) == 0,
              "子窗口类名 = BinUI2D::appClassName()（与 GL/VK/D3D11 的类名不同）");
    }
    std::printf("[info] present 路径 = %s\n", app.gfx().presentBackendName());
    CHECK(app.gfx().presentBackendName() && *app.gfx().presentBackendName(),
          "presentBackendName() 非空");
    CHECK(app.dpi() >= 96, "DPI ≥ 96");

    // ---------------- 2. 首帧：全屏铺满 ----------------
    std::printf("\n--- 2. 首帧绘制 ---\n");
    // openChild 内部已经 invalidateAll()，先把那一帧消化掉，计数从 0 开始
    PumpOnce(app);
    probe.calls = 0;
    app.invalidateAll();
    PumpOnce(app);
    CHECK(probe.calls == 1, "首帧 draw 回调被调用一次");
    CHECK(probe.clip[0] == 0 && probe.clip[1] == 0 && probe.clip[2] == kW && probe.clip[3] == kH,
          "invalidateAll → 脏矩形 = 整窗");
    CHECK(app.gfx().pixels() != nullptr, "有像素出口");
    CHECK(app.gfx().pixelsSize() == (size_t)kW * kH * 4u, "像素大小 = w*h*4");
    {
        const Px p = At(app.gfx(), 5, 5);
        CHECK(p.eq(0x33, 0x66, 0xCC, 0xFF), "左上角 = 0xFF3366CC");
        const Px q = At(app.gfx(), 300, 230);
        CHECK(q.eq(0x33, 0x66, 0xCC, 0xFF), "右下角 = 0xFF3366CC");
    }

    // ---------------- 3. 脏区：只清脏区，其余保留 ----------------
    std::printf("\n--- 3. 脏区语义 ---\n");
    probe.phase = 1;
    app.invalidateRect(10, 20, 30, 40);
    PumpOnce(app);
    CHECK(probe.clip[0] == 10 && probe.clip[1] == 20 && probe.clip[2] == 40 && probe.clip[3] == 60,
          "invalidateRect(10,20,30,40) → 脏矩形 = {10,20,40,60}");
    {
        const Px inside = At(app.gfx(), 20, 30);
        CHECK(inside.eq(0xFF, 0x00, 0x00, 0xFF), "脏区内的点被重画成红色");
        const Px outL = At(app.gfx(), 5, 5);
        CHECK(outL.eq(0x33, 0x66, 0xCC, 0xFF), "脏区左侧的点保留上一帧（没有被清成透明）");
        const Px outB = At(app.gfx(), 20, 100);
        CHECK(outB.eq(0x33, 0x66, 0xCC, 0xFF), "脏区下方的点保留上一帧");
        const Px edge = At(app.gfx(), 39, 30);
        CHECK(edge.eq(0xFF, 0x00, 0x00, 0xFF), "脏区右边界内侧（x=39）被重画");
        const Px outEdge = At(app.gfx(), 40, 30);
        CHECK(outEdge.eq(0x33, 0x66, 0xCC, 0xFF), "脏区右边界外（x=40）保留上一帧");
    }

    // ---------------- 4. 图层：组透明度真的乘一次 ----------------
    std::printf("\n--- 4. 组透明度（pushLayer/popLayer）---\n");
    probe.phase = 2;
    app.invalidateAll();
    PumpOnce(app);
    CHECK(probe.layerOk, "pushLayer 返回 true（bin2d 后端支持离屏图层）");
    {
        const Px p = At(app.gfx(), 300, 230);
        std::printf("[info] 50%% 黑层合成实测 = (%d,%d,%d,%d)\n", p.r, p.g, p.b, p.a);
        // 黑 50% over (0x33,0x66,0xCC) → (≈26, 51, 102)，alpha 仍 255
        CHECK(p.approx(26, 51, 102, 255), "50% 黑层合成结果 ≈ (26,51,102,255)");
    }

    // ---------------- 5. 裁剪栈：pushClip 生效、popClipTo 恢复 ----------------
    std::printf("\n--- 5. 裁剪栈 ---\n");
    probe.phase = 3;
    app.invalidateRect(0, 0, 60, 60);
    PumpOnce(app);
    {
        const Px inClip = At(app.gfx(), 20, 30);
        CHECK(inClip.eq(0x00, 0xFF, 0x00, 0xFF), "裁剪框内 = 绿色");
        const Px afterPop = At(app.gfx(), 2, 2);
        CHECK(afterPop.eq(0x00, 0x00, 0xFF, 0xFF),
              "popClipTo 之后画的蓝块落在裁剪框外（裁剪已恢复）");
        const Px cleared = At(app.gfx(), 45, 40);
        CHECK(cleared.eq(0x00, 0x00, 0x00, 0x00),
              "脏区内但裁剪外的点 = 透明（裁剪真的挡住了绿色填充）");
    }

    // ---------------- 6. 输入：坐标与回调 ----------------
    std::printf("\n--- 6. 输入消息 ---\n");
    HWND child = nullptr;
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM p) -> BOOL {
            *reinterpret_cast<HWND*>(p) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&child));

    float gx = -1.0f, gy = -1.0f;
    int   clicks = 0, releases = 0, dbls = 0, leaves = 0, moves = 0;
    float wheelDy = 0.0f;
    float wx = -1.0f, wy = -1.0f;
    std::string chars;
    int lastKey = -1;
    int rightBtn = -100, midBtn = -100;
    bool rightDown = false, midDown = false;
    int lastCursorKind = -1;
    int resizedW = 0, resizedH = 0;
    int dpiSeen = 0;
    bool imeWanted = false;

    app.setMoveFn([&](float x, float y) { ++moves; gx = x; gy = y; });
    app.setPressFn([&](float, float) { ++clicks; });
    app.setReleaseFn([&](float, float) { ++releases; });
    app.setDoubleClickFn([&](float, float) { ++dbls; });
    app.setLeaveFn([&] { ++leaves; });
    app.setWheelFn([&](float x, float y, float dy) { wx = x; wy = y; wheelDy = dy; });
    app.setCharFn([&](const std::string& s) { chars += s; });
    app.setKeyFn([&](int vk) { lastKey = vk; });
    app.setButtonFn([&](float, float, int btn, bool down) {
        if (btn == 2) { rightBtn = btn; rightDown = down; }
        if (btn == 1) { midBtn = btn; midDown = down; }
    });
    app.setCursorFn([&](float, float) { lastCursorKind = 0; return 1; });
    app.setResizeFn([&](int w, int h) { resizedW = w; resizedH = h; });
    app.setDpiChangedFn([&](int dpi) { dpiSeen = dpi; });
    app.setImeActiveFn([&] { return imeWanted; });

    // 鼠标移动：客户区坐标必须原样交给宿主
    PostMessageW(child, WM_MOUSEMOVE, 0, MAKELPARAM(37, 91));
    PumpOnce(app);
    CHECK(moves == 1 && gx == 37.0f && gy == 91.0f, "WM_MOUSEMOVE → move(37,91)");

    // 左键：按下 / 抬起
    PostMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 34));
    PumpOnce(app);
    PostMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(12, 34));
    PumpOnce(app);
    CHECK(clicks == 1 && releases == 1, "WM_LBUTTONDOWN/UP → press 一次 + release 一次");

    // 双击：先按一次"按下"再交双击
    PostMessageW(child, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(50, 60));
    PumpOnce(app);
    CHECK(dbls == 1, "WM_LBUTTONDBLCLK → 双击回调一次");
    CHECK(clicks == 2, "双击也会先补一次 press（焦点/光标先摆好）");

    // 右键 / 中键：按钮号与 DOM 同号（2 = 右，1 = 中）
    PostMessageW(child, WM_RBUTTONDOWN, 0, MAKELPARAM(7, 8));
    PostMessageW(child, WM_MBUTTONUP, 0, MAKELPARAM(9, 10));
    PumpOnce(app);
    CHECK(rightBtn == 2 && rightDown, "WM_RBUTTONDOWN → btn(2, down)");
    CHECK(midBtn == 1 && !midDown, "WM_MBUTTONUP → btn(1, up)");

    // 离开
    PostMessageW(child, WM_MOUSELEAVE, 0, 0);
    PumpOnce(app);
    CHECK(leaves == 1, "WM_MOUSELEAVE → leave 回调一次");

    // 滚轮：lParam 是**屏幕**坐标，外壳内部转回客户区；delta 120 → 40px
    {
        POINT pt = { 40, 70 };
        ClientToScreen(child, &pt);
        PostMessageW(child, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), MAKELPARAM(pt.x, pt.y));
        PumpOnce(app);
        CHECK(wheelDy == 40.0f, "WM_MOUSEWHEEL(+120) → wheelDy = 40px");
        CHECK(wx == 40.0f && wy == 70.0f, "滚轮的屏幕坐标被转回客户区坐标");
    }

    // 字符：ASCII 一个字节，中文三个字节，代理对四个字节（emoji）
    chars.clear();
    PostMessageW(child, WM_CHAR, 'A', 0);
    PumpOnce(app);
    CHECK(chars == "A", "WM_CHAR('A') → \"A\"");

    chars.clear();
    PostMessageW(child, WM_CHAR, 0x4E2D /*中*/, 0);
    PumpOnce(app);
    CHECK(chars.size() == 3 && chars[0] == '\xE4' && chars[1] == '\xB8' && chars[2] == '\xAD',
          "WM_CHAR(L'中') → UTF-8 三字节 E4 B8 AD");

    chars.clear();
    PostMessageW(child, WM_CHAR, 0xD83D /*高代理*/, 1);
    PostMessageW(child, WM_CHAR, 0xDE00 /*低代理*/, 1);
    PumpOnce(app);
    CHECK(chars.size() == 4 && chars[0] == '\xF0' && chars[1] == '\x9F' &&
              chars[2] == '\x98' && chars[3] == '\x80',
          "代理对 → UTF-8 四字节 F0 9F 98 80");

    // 控制字符不上屏
    chars.clear();
    PostMessageW(child, WM_CHAR, '\r', 0);
    PumpOnce(app);
    CHECK(chars.empty(), "WM_CHAR('\\r') 被丢弃（控制字符不产生字符事件）");

    // 按键
    PostMessageW(child, WM_KEYDOWN, VK_TAB, 0);
    PumpOnce(app);
    CHECK(lastKey == VK_TAB, "WM_KEYDOWN(VK_TAB) → key(9)");

    // 光标形状查询
    PostMessageW(child, WM_SETCURSOR, (WPARAM)child, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    PumpOnce(app);
    CHECK(lastCursorKind == 0, "WM_SETCURSOR(HTCLIENT) → cursor 回调被调用");

    // ---------------- 7. 输入法：编辑态切换真的挂上/摘下 ----------------
    std::printf("\n--- 7. 输入法上下文 ---\n");
    imeWanted = false;
    app.invalidateAll();
    PumpOnce(app);
    CHECK(ImmGetContext(child) == nullptr, "imeActive=false → IME 上下文被摘下");
    imeWanted = true;
    app.invalidateAll();
    PumpOnce(app);
    {
        HIMC himc = ImmGetContext(child);
        CHECK(himc != nullptr, "imeActive=true → IME 上下文被挂回");
        if (himc) ImmReleaseContext(child, himc);
    }

    // ---------------- 8. 尺寸变化：挂起到帧内应用 ----------------
    std::printf("\n--- 8. WM_SIZE ---\n");
    probe.phase = 0;
    PostMessageW(child, WM_SIZE, SIZE_RESTORED, MAKELPARAM(360, 280));
    PumpOnce(app);
    CHECK(resizedW == 360 && resizedH == 280, "WM_SIZE → resize 回调 (360,280)");
    CHECK(app.gfx().width() == 360 && app.gfx().height() == 280, "后端跟着改尺寸");
    CHECK(app.gfx().pixelsSize() == (size_t)360 * 280 * 4u, "像素缓冲跟着改大小");

    // ---------------- 9. DPI ----------------
    std::printf("\n--- 9. WM_DPICHANGED ---\n");
    SendMessageW(child, WM_DPICHANGED, MAKEWPARAM(120, 120), 0);
    PumpOnce(app);
    CHECK(dpiSeen == 120, "WM_DPICHANGED → dpiChanged 回调 (120)");
    CHECK(app.dpi() == 120, "dpi() = 120");

    // ---------------- 10. 跨线程 pump 被拒 ----------------
    std::printf("\n--- 10. 线程亲和 ---\n");
    {
        int rc = 0;
        std::thread t([&] { rc = app.pump(1); });
        t.join();
        CHECK(rc == -1, "非建窗线程 pump → 返回 -1（明确拒绝，不是随机崩）");
        CHECK(app.onOwnerThread(), "建窗线程 onOwnerThread() = true");
    }

    // ---------------- 11. 同进程第二个实例并存 ----------------
    std::printf("\n--- 11. 双实例并存 ---\n");
    {
        BinUIApp2D app2(120, 100);
        int calls2 = 0;
        app2.setDrawFn([&calls2](IBinUIGfx& gfx, const void*) {
            ++calls2;
            gfx.fillRect(0.0f, 0.0f, 120.0f, 100.0f, 0xFF112233u);
        });
        CHECK(app2.openChild(parent, 0, 0, 120, 100), "第二个实例 openChild 成功（同一个窗口类）");
        app2.invalidateAll();
        PumpOnce(app2);
        CHECK(calls2 == 1, "第二个实例独立出帧");
        CHECK(app2.gfx().pixelsSize() == (size_t)120 * 100 * 4u, "第二个实例尺寸独立");
        const Px p = At(app2.gfx(), 60, 50);
        CHECK(p.eq(0x11, 0x22, 0x33, 0xFF), "第二个实例画的是自己的内容");
        // 第一个实例的像素不受影响
        const Px q = At(app.gfx(), 5, 5);
        CHECK(q.eq(0x33, 0x66, 0xCC, 0xFF), "第一个实例的像素没有被第二个实例搅动");
        app2.close();
    }

    // ---------------- 12. 生命周期 ----------------
    std::printf("\n--- 12. 收尾 ---\n");
    app.requestExit();
    CHECK(app.pump(1) == 1, "requestExit → pump 返回 1");
    app.close();
    app.close();   // 幂等
    CHECK(true, "close() 可重复调用");
    CHECK(app.pump(1) == 1, "close 之后 pump 仍返回 1");

    DestroyWindow(parent);
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
