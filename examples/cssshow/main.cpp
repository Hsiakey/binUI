// cssshow/main.cpp - CSS 伪类 / 伪元素演示（OpenGL 3.3 无边框窗口）
// 覆盖 v11 的新能力：
//   · 运行时伪类 :hover / :active / :focus / :checked —— 编译成状态样式段，
//     渲染器按指针/焦点/勾选状态切换目标值（声明了 transition 就走过渡）
//   · 结构伪类 :first-child / :nth-child() / :last-child —— 编译期按兄弟序号求值
//   · 伪元素 ::before / ::after —— 内联模式并入宿主文本，盒模式合成子节点
#include <cstdio>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

static void PrintUsage() {
    std::printf("用法: cssshow [--watch] [--ui <main.html>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（UI 开发用）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n");
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "cssshow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[cssshow] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[cssshow] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[cssshow] Failed to load layout\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 720 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 520 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true,
                         wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(),
                         wd.title.c_str());

    auto findNode = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
        for (const auto& n : renderer.getNodes())
            if (n.id == id) return &n;
        return nullptr;
    };
    auto invalidateNode = [&](uint16_t id) {
        const auto* n = findNode(id);
        if (!n || !(n->flags & BINUI_VISIBLE)) return;
        app.invalidateRect((int)n->x, (int)n->y, (int)n->width, (int)n->height);
    };

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
        int old = renderer.keyboardNode();   // :focus 可能在按下时变化
        renderer.press(x, y);
        int n = renderer.hitTest(x, y);
        if (n >= 0) invalidateNode((uint16_t)n);
        if (old >= 0 && old != renderer.keyboardNode()) invalidateNode((uint16_t)old);
    });
    app.setReleaseFn([&](float x, float y) {
        int p = renderer.pressedNode();
        int id = renderer.release(x, y);
        if (p >= 0) invalidateNode((uint16_t)p);
        if (id < 0) return;
        invalidateNode((uint16_t)id);
        const auto* n = findNode((uint16_t)id);
        if (!n) return;
        if (n->text == "\xE2\x9C\x95") { app.requestExit(); return; }   // ✕
        // 勾选方块：外壳自己维护 :checked 状态（核心库没有勾选控件）
        static uint16_t toggleId = 0;
        if (!toggleId) toggleId = renderer.findNodeByName("b4");
        if ((uint16_t)id == toggleId) {
            renderer.setNodeChecked(toggleId, !renderer.isNodeChecked(toggleId));
            app.invalidateAll();
        }
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    app.setKeyFn([&](int vk) {
        if (vk == VK_ESCAPE) { app.requestExit(); return; }
        // Tab / 方向键：焦点移动 → :focus 状态切换
        int old = renderer.keyboardNode();
        if (vk == VK_TAB)         renderer.cycleKeyboardFocus(!(::GetKeyState(VK_SHIFT) & 0x8000));
        else if (vk == VK_RIGHT)  renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT);
        else if (vk == VK_LEFT)   renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::LEFT);
        else if (vk == VK_DOWN)   renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::DOWN);
        else if (vk == VK_UP)     renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::UP);
        if (old != renderer.keyboardNode()) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (renderer.keyboardNode() >= 0) invalidateNode((uint16_t)renderer.keyboardNode());
        }
    });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); app.invalidateAll(); }
    });
    app.setWheelFn([&](float x, float y, float dy) {
        int id = renderer.scrollAt(x, y, dy);
        if (id >= 0) invalidateNode((uint16_t)id);
    });

    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;

    app.setUpdateFn([&](float dt) {
        (void)dt;
        if (hot.Tick(renderer, "cssshow", winW, winH) == binui::ex::HotReload::Result::Reloaded)
            app.invalidateAll();
        for (uint16_t id : renderer.stepAnimations())   // 过渡/关键帧重绘
            invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[cssshow] CSS 伪类/伪元素演示 (OpenGL 3.3)\n");
    return app.run();
}
