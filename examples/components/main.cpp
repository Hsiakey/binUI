// components/main.cpp - 组件化演示（.bui / .bum / .bum.xml 混放）
// 覆盖 v12 的组件链接能力：
//   · 源码组件 .bum.xml：编译期内联（可参数化 {{param}}）
//   · 二进制组件 .bum：编译期由 binui_link 离线合并（同一套合并规则）
//   · 外挂组件 external：编译器只留占位节点，运行期 ex::MountAll() 现场挂载
// 另外演示 v11 的运行时伪类 :hover / :active / :focus（状态样式段）
#include <cstdio>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_component.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

static void PrintUsage() {
    std::printf("用法: components [--watch] [--ui <main.html>]\n"
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
    const std::string uiPath = binui::ex::ResolveUI(args, "components", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[components] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[components] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[components] Failed to load layout\n");
        return 1;
    }

    // ---- 外挂组件（<use ... external>）：运行期挂载 ----
    const std::string uiDir = binui::ex::DirOf(uiPath);
    auto mountAll = [&]() {
        const int nodesBefore = (int)renderer.getNodes().size();
        const binui::ex::MountReport rep =
            binui::ex::MountAll(renderer, uiDir, renderer.layoutBytes());
        if (rep.mounted || rep.failed)
            std::printf("[components] 外挂组件：挂载 %d，跳过 %d，失败 %d（新增节点 %d）\n",
                        rep.mounted, rep.skipped, rep.failed,
                        (int)renderer.getNodes().size() - nodesBefore);
    };
    mountAll();

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
        if (hot.Tick(renderer, "components", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            mountAll();          // 重编后外挂组件要重新挂（新布局里只剩占位节点）
            app.invalidateAll();
        }
        for (uint16_t id : renderer.stepAnimations())   // 过渡/关键帧重绘
            invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[components] 组件化演示 (OpenGL 3.3)\n");
    return app.run();
}

