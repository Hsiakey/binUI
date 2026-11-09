// fontshow/main.cpp - 字体与输入法演示：字号 / 字重 / 斜体 / 行高 / 装饰 / 混排 + IME
//
// ①–④ 是字体能力，全在布局层（CSS），外壳只负责载入、转发输入、按动画重绘。
//
// ⑤ 是输入法，这部分有硬要求（都是踩过的坑，改之前先看
// src/backends/binui_gl-gdi.cpp 里 WM_IME_* 那段的注释）：
//   · 字符来自 WM_IME_COMPOSITION 的 GCS_RESULTSTR（UTF-16），不是 WM_IME_CHAR
//     —— 后者的 wParam 是单字节 ANSI，按宽字符插就是「å å å」
//   · 组字期间的回车/空格/退格归输入法，宿主不要插一脚
//   · **必须**接 setImeCaretFn：不给光标位置，输入法把组字窗摆到窗口外，
//     现象是"打拼音屏幕上什么都没有"，但选词照样能上屏，特别容易误判成没工作
//   · 候选窗位置靠 IMR_QUERYCHARACTERPOSITION 应答，且必须是屏幕坐标
#include <cstdio>
#include <cstdint>
#include <string>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

// 把 UTF-8 首字符解成码点（演示用，只处理完整序列）
static uint32_t FirstCodePoint(const std::string& s) {
    if (s.empty()) return 0;
    const auto b0 = static_cast<unsigned char>(s[0]);
    if (b0 < 0x80) return b0;
    const size_t n = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 1;
    if (s.size() < n) return b0;
    uint32_t cp = b0 & (0xFF >> (n + 1));
    for (size_t i = 1; i < n; i++)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    return cp;
}

// 演示要用的节点：热重载换树后 id 会变，所以每次重载都按名字重新解析一遍
struct UiHandles {
    uint16_t in1 = 0, in2 = 0, echo = 0, codepoint = 0, stats = 0;

    void Resolve(BinUIRenderer& r) {
        in1 = r.findNodeByName("in1");
        in2 = r.findNodeByName("in2");
        echo = r.findNodeByName("echo");
        codepoint = r.findNodeByName("codepoint");
        stats = r.findNodeByName("stats");
    }
};

int main(int argc, char* argv[]) {
    // 输出重定向到文件时 stdout 是全缓冲的，调完看不到任何诊断 —— 直接关掉缓冲
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "fontshow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) res.layout = GenerateDefaultBinUI();
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[fontshow] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 1;

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 900 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 760 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.title.c_str());

    // 标题栏三颗控制按钮（components/titlebar.bum.xml 里的 id：win-min / win-max / win-close）。
    // 组件只负责外观，动作归宿主：最小化 / 最大化·还原 / 关闭窗口。
    const uint16_t idWinMin   = renderer.findNodeByName("win-min");
    const uint16_t idWinMax   = renderer.findNodeByName("win-max");
    const uint16_t idWinClose = renderer.findNodeByName("win-close");

    renderer.setEventCallback([&](const Event& ev) {
        if (ev.type != EventType::CLICK) return;
        if (ev.nodeId == idWinMin)   { app.minimize(); return; }
        if (ev.nodeId == idWinMax)   { app.maximize(); return; }
        if (ev.nodeId == idWinClose) { app.requestExit(); return; }
    });

    UiHandles h;
    h.Resolve(renderer);

    auto invalidateNode = [&](uint16_t id) {
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    // 两个输入框的内容各自缓存（renderer 的编辑会话只记当前这一个）
    std::string valueOf[2];

    // 把编辑态的值同步回缓存，并刷新统计行
    auto syncValue = [&]() {
        if (!renderer.isEditing()) return;
        const auto& ed = renderer.edit();
        if (ed.nodeId == h.in1) valueOf[0] = ed.value;
        else if (ed.nodeId == h.in2) valueOf[1] = ed.value;
        if (h.stats) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "输入框① %zu 字符 · 输入框② %zu 字符",
                          valueOf[0].size(), valueOf[1].size());
            renderer.setText(h.stats, buf);
            invalidateNode(h.stats);
        }
    };

    // ---- IME 光标定位：组字窗与候选框都贴着光标走 ----
    // 不接这个钩子就是坏的（见文件头的坑），但它对热重载无感：内部读的是编辑会话。
    app.setImeCaretFn([&](int& x, int& y, int& hh) {
        return imeCaretFromRenderer(renderer, app.gfx(), x, y, hh);
    });

    // ---- 字符输入：输入法选词上屏的字符走这里 ----
    app.setCharFn([&](const std::string& utf8) {
        if (!renderer.isEditing()) return;
        renderer.insertText(utf8);
        invalidateNode(renderer.edit().nodeId);
        syncValue();
        if (h.echo) {
            renderer.setText(h.echo, "最后收到的字符：" + utf8);
            invalidateNode(h.echo);
        }
        if (h.codepoint) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "码点：U+%04X　（%zu 字节 UTF-8）",
                          FirstCodePoint(utf8), utf8.size());
            renderer.setText(h.codepoint, buf);
            invalidateNode(h.codepoint);
        }
        std::printf("[fontshow] char: %s  U+%04X\n", utf8.c_str(), FirstCodePoint(utf8));
    });

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) {
        drawNodesGLGdi(gfx, renderer, clip, nullptr);
    });
    app.setMoveFn([&](float x, float y) {
        int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        int cur = renderer.hoveredNode();
        if (old != cur) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
        }
    });
    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.press(x, y);
        int n = renderer.hitTest(x, y);
        if (n >= 0) invalidateNode((uint16_t)n);
    });
    app.setReleaseFn([&](float x, float y) {
        int p = renderer.pressedNode();
        int id = renderer.release(x, y);
        if (p >= 0) invalidateNode((uint16_t)p);
        if (id >= 0) {
            invalidateNode((uint16_t)id);
            for (const auto& n : renderer.getNodes()) {
                if (n.id != (uint16_t)id) continue;
                break;
            }
        }
        syncValue();
        if (renderer.isEditing()) invalidateNode(renderer.edit().nodeId);
    });
    app.setLeaveFn([&]() {
        int hv = renderer.hoveredNode();
        renderer.clearHover();
        if (hv >= 0) invalidateNode((uint16_t)hv);
    });
    // ---- 按键：组字期间的键由库拦在窗口层，走不到这里 ----
    app.setKeyFn([&](int vk) {
        if (vk == VK_ESCAPE) { app.requestExit(); return; }
        if (!renderer.isEditing()) return;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (vk) {
            case VK_BACK:   renderer.backspace(); break;
            case VK_DELETE: renderer.deleteForward(); break;
            case VK_LEFT:   renderer.moveCaret(-1, shift); break;
            case VK_RIGHT:  renderer.moveCaret(1, shift); break;
            case VK_HOME:   renderer.setCaret(0); break;
            case VK_END:    renderer.setCaret(renderer.edit().value.size()); break;
            case VK_RETURN:
            case VK_TAB:    renderer.blur(); break;
            default: break;
        }
        invalidateNode(renderer.isEditing() ? renderer.edit().nodeId : h.in1);
        syncValue();
    });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); app.invalidateAll(); }
    });

    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;
    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "fontshow", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            h.Resolve(renderer);   // 换树后节点 id 变了，重解析
            syncValue();
            app.invalidateAll();
        }
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });
    app.setResizeFn([&](int w, int hh) { winW = w; winH = hh; renderer.setSize(w, hh); });

    std::printf("[fontshow] 字体与输入法演示 (OpenGL 3.3)\n");
    std::printf("[fontshow] ⑤ 输入法：打拼音看组字与候选框，空格选词上屏；Esc 退出\n");
    return app.run();
}
