// features/main.cpp - 功能演示：样式 / 语义 / 组件化 / 数据驱动
//
// 把原来 cssshow + components + vision_monitor 三个示例压成一个,覆盖:
//   · v11 运行时伪类 :hover / :active / :focus / :checked(状态样式段)
//   · v13 扩展绘制: 边框 / 渐变 / 字重 / 行高 / 文字装饰
//   · v14 HTML 元素默认语义(UA 样式表 / 占位符 / disabled / checkbox)
//   · v12 组件链接三形态: 源码 .bum.xml 内联 / 二进制 .bum / external 运行期挂载
//   · 数据驱动: 定时 setText + invalidateNode 局部重绘(不是整窗重画)
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
    std::printf("用法: features [--watch] [--ui <main.html>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载(UI 开发用)\n"
                "  --ui     显式指定 UI 文件(默认在 exe 同级目录查找)\n");
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "features", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[features] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[features] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[features] Failed to load layout\n");
        return 1;
    }

    // ---- 外挂组件（<use ... external>）：运行期挂载 ----
    const std::string uiDir = binui::ex::DirOf(uiPath);
    auto mountAll = [&]() {
        const int nodesBefore = (int)renderer.getNodes().size();
        const binui::ex::MountReport rep =
            binui::ex::MountAll(renderer, uiDir, renderer.layoutBytes());
        if (rep.mounted || rep.failed)
            std::printf("[features] 外挂组件：挂载 %d，跳过 %d，失败 %d（新增节点 %d）\n",
                        rep.mounted, rep.skipped, rep.failed,
                        (int)renderer.getNodes().size() - nodesBefore);
    };
    mountAll();

    const auto& wd = renderer.windowDesc();
    const auto& nodes0 = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes0.empty() ? 900 : (int)nodes0[0].width);
    int height = wd.present ? (int)wd.height : (nodes0.empty() ? 840 : (int)nodes0[0].height);

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
        // 失效矩形必须含动画后的包围盒：hover 放大时只按布局矩形失效，
        // 长出来的那一圈（边框/底色）不会被重绘，看上去就是"边框被削掉一半"
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    // ---- 数据驱动用到的节点 id（重编后要重新查） ----
    uint16_t idState = 0, idCount = 0, idLog = 0, idToggle = 0;
    auto reindexNodes = [&]() {
        idState  = renderer.findNodeByName("dv-state");
        idCount  = renderer.findNodeByName("dv-count");
        idLog    = renderer.findNodeByName("dv-log");
        idToggle = renderer.findNodeByName("tg");
    };
    reindexNodes();

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
        // 点输入框中部：光标落在点击位置，而不是永远跳到末尾
        if (renderer.isEditing()) placeCaretAtClickGLGdi(renderer, app.gfx(), x, y);
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
        if (n->text == "\xE2\x88\x92") { app.minimize();    return; }   // −
        if (n->text == "\xE2\x96\xA1") { app.maximize();    return; }   // □
        if (n->text == "\xE2\x9C\x95") { app.requestExit(); return; }   // ✕
        // 自定义开关：点一下切 :checked（渲染器只存状态，外观由作者 CSS 给）
        if ((uint16_t)id == idToggle) {
            static bool on = false;
            on = !on;
            renderer.setNodeChecked(idToggle, on);
            invalidateNode(idToggle);
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

    // ---- 数据驱动：每 0.4s 改三行文字，只失效这三行 ----
    int tick = 0;
    float acc = 0.0f;
    app.setUpdateFn([&](float dt) {
        if (hot.Tick(renderer, "features", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            mountAll();          // 重编后外挂组件要重新挂（新布局里只剩占位节点）
            reindexNodes();
            app.invalidateAll();
        }
        const auto animated = renderer.stepAnimations();   // 过渡/关键帧重绘
        for (uint16_t id : animated) invalidateNode(id);

        acc += dt;
        if (acc < 0.4f) return;
        acc = 0.0f;
        tick++;
        char buf[192];
        if (idState) {
            std::snprintf(buf, sizeof(buf),
                          "运行时状态：tick %d · 动画 %zu 条 · 尺寸 %dx%d · 渲染器只管算坐标",
                          tick, animated.size(), winW, winH);
            renderer.setText(idState, buf);
            invalidateNode(idState);
        }
        if (idCount) {
            std::snprintf(buf, sizeof(buf), "tick %d", tick);
            renderer.setText(idCount, buf);
            invalidateNode(idCount);
        }
        if (idLog && tick % 8 == 0) {
            std::snprintf(buf, sizeof(buf), "setText + invalidateNode：只重画这一行的矩形（tick %d）", tick);
            renderer.setText(idLog, buf);
            invalidateNode(idLog);
        }
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[features] 功能演示 (OpenGL 3.3)\n");
    return app.run();
}
