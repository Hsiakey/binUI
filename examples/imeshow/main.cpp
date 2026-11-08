// imeshow/main.cpp - binUI 中文输入法（IME）演示
//
// 这个示例只做一件事：把 IME 的每一步都摆到屏幕上，方便肉眼确认库里那条
// 消息链路是通的：
//   打拼音 → 组字串内联在光标处 → 候选框在光标下方 → 选词上屏成中文
//
// 关键点（都是踩过的坑，改这里之前先看 src/backends/binui_gl-gdi.cpp 里
// WM_IME_* 那段的注释）：
//   · 字符来自 WM_IME_COMPOSITION 的 GCS_RESULTSTR（UTF-16），不是 WM_IME_CHAR
//     —— 后者的 wParam 是单字节 ANSI，按宽字符插就是「å å å」
//   · 组字期间的回车/空格/退格归输入法，宿主不要插一脚
//   · **必须**接 setImeCaretFn：不给光标位置，输入法把组字窗摆到窗口外，
//     打拼音屏幕上什么都没有（例子见下面的 setImeCaretFn 调用）
//   · 候选窗位置靠 IMR_QUERYCHARPOSITION 应答，且必须是屏幕坐标
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include <windows.h>

using namespace binui;

static void PrintUsage() {
    std::printf("用法: imeshow [--ui <main.html>]\n"
                "  --ui  显式指定 UI 文件（默认在 exe 同级目录查找）\n"
                "\n"
                "演示中文输入法：打拼音看组字/候选框，选词后中文落进输入框。\n"
                "Esc 退出。\n");
}

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

int main(int argc, char* argv[]) {
    // 输出重定向到文件时 stdout 是全缓冲的，调完看不到任何诊断 —— 直接关掉缓冲
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "imeshow", "main.html");
    std::printf("[imeshow] ui = %s\n", uiPath.empty() ? "<none>" : uiPath.c_str());
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[imeshow] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        else
            std::fprintf(stderr, "[imeshow] 编译失败，使用默认布局\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[imeshow] Failed to load layout\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    const int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 760 : (int)nodes[0].width);
    const int height = wd.present ? (int)wd.height : (nodes.empty() ? 648 : (int)nodes[0].height);
    std::printf("[imeshow] windowDesc present=%d title='%s' size=%dx%d nodes=%zu\n",
                wd.present ? 1 : 0, wd.title.c_str(), width, height, nodes.size());

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true,
                         wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(),
                         wd.present ? wd.title.c_str() : "binUI · IME 演示");

    // 按名字拿到演示要用的几个节点
    const uint16_t in1   = renderer.findNodeByName("in1");
    const uint16_t in2   = renderer.findNodeByName("in2");
    const uint16_t quit  = renderer.findNodeByName("quit");
    const uint16_t echo  = renderer.findNodeByName("echo");
    const uint16_t cpN   = renderer.findNodeByName("codepoint");
    const uint16_t stats = renderer.findNodeByName("stats");
    if (in1 == 0 || in2 == 0) {
        std::fprintf(stderr, "[imeshow] 布局里找不到 in1/in2，检查 main.html\n");
        return 1;
    }

    auto invalidateNode = [&](uint16_t id) {
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    // 两个输入框的内容分别存着（renderer 的编辑态只认当前这一个）
    std::string valueOf[2];

    // 把编辑态的值同步回我们的缓存，并刷新统计行
    auto syncValue = [&]() {
        if (!renderer.isEditing()) return;
        const auto& ed = renderer.edit();
        if (ed.nodeId == in1) valueOf[0] = ed.value;
        else if (ed.nodeId == in2) valueOf[1] = ed.value;
        if (stats) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "输入框① %zu 字符 · 输入框② %zu 字符",
                          valueOf[0].size(), valueOf[1].size());
            renderer.setText(stats, buf);
            invalidateNode(stats);
        }
    };

    // ---- IME 光标定位：组字窗与候选框都贴着光标走 ----
    // 这个钩子**不接就是坏的**：输入法拿不到光标位置，只能把组字窗摆到默认处，
    // 实测会落到窗口外 —— 现象是"打拼音屏幕上什么都没有"，但选词照样能上屏，
    // 特别容易误判成输入法没工作。
    app.setImeCaretFn([&](int& x, int& y, int& h) {
        return imeCaretFromRenderer(renderer, app.gfx(), x, y, h);
    });

    // ---- 字符输入：IME 上屏的字符走这里 ----
    // 组字阶段的拼音不会来（那些是输入法自己在画），只有选词提交的字符才进来。
    app.setCharFn([&](const std::string& utf8) {
        if (!renderer.isEditing()) return;          // 没在编辑就丢掉
        renderer.insertText(utf8);
        invalidateNode(renderer.edit().nodeId);
        syncValue();

        // 回显：字符 + 码点
        if (echo) {
            renderer.setText(echo, "最后收到的字符：" + utf8);
            invalidateNode(echo);
        }
        if (cpN) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "码点：U+%04X　（%zu 字节 UTF-8）",
                          FirstCodePoint(utf8), utf8.size());
            renderer.setText(cpN, buf);
            invalidateNode(cpN);
        }
        std::printf("[imeshow] char: %s  U+%04X\n", utf8.c_str(), FirstCodePoint(utf8));
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
        invalidateNode(renderer.isEditing() ? renderer.edit().nodeId : in1);
        syncValue();
    });

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) { drawNodesGLGdi(gfx, renderer, clip); });
    app.setMoveFn([&](float x, float y) {
        const int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        const int cur = renderer.hoveredNode();
        if (old != cur) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
        }
    });
    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.press(x, y);
    });
    app.setReleaseFn([&](float x, float y) {
        const int pressed = renderer.pressedNode();
        const int id = renderer.release(x, y);
        if (pressed >= 0) invalidateNode((uint16_t)pressed);
        if (id >= 0) {
            if ((uint16_t)id == quit) { app.requestExit(); return; }
            invalidateNode((uint16_t)id);
            syncValue();
        }
        if (renderer.isEditing()) invalidateNode(renderer.edit().nodeId);
    });
    app.setLeaveFn([&]() {
        const int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    app.setFocusFn([&](bool focused) {
        if (!focused) {
            renderer.cancelPress();
            renderer.clearHover();
            renderer.blur();
            app.invalidateAll();
        }
    });
    app.setResizeFn([&](int w, int h) { renderer.setSize(w, h); });

    std::printf("[imeshow] binUI IME 演示\n");
    std::printf("[imeshow] 打拼音看组字与候选框；空格选词上屏；Esc 退出\n");
    return app.run();
}
