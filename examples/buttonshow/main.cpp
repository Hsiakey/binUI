// buttonshow/main.cpp - 按钮样式一览
//
// 外观全在 examples/buttonshow/main.html 里（纯 CSS），这个外壳只做三件事：
//   1. 把键盘丢给渲染器的 handleKey（WASD 移动焦点、方向键滚页面、回车激活/进出输入态）；
//   2. 接 CLICK 事件，把"最后点击了哪个按钮 + 点击次数"写到底部状态行；
//   3. 维护一个 :checked 切换按钮（核心库不认勾选语义，得外壳自己 setNodeChecked）。
#include <cstdio>
#include <string>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "buttonshow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[buttonshow] 未找到 main.html，使用默认布局\n");
        else
            std::fprintf(stderr, "[buttonshow] 编译失败，使用默认布局\n");
        res.layout = GenerateDefaultBinUI();
    }
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[buttonshow] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 1;

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 1020 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 700 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.present ? wd.title.c_str() : "binUI · 按钮样式");

    // 标题栏三颗控制按钮（components/titlebar.bum.html 里的 id：win-min / win-max / win-close）。
    // 组件只负责外观，动作归宿主：最小化 / 最大化·还原 / 关闭窗口。
    const uint16_t idWinMin   = renderer.findNodeByName("win-min");
    const uint16_t idWinMax   = renderer.findNodeByName("win-max");
    const uint16_t idWinClose = renderer.findNodeByName("win-close");

    auto invalidateNode = [&](uint16_t id) {
        if (id == 0) return;
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    const uint16_t idStatus = renderer.findNodeByName("status");
    const uint16_t idCount  = renderer.findNodeByName("count");
    const uint16_t idTog    = renderer.findNodeByName("tog");
    if (!idStatus || !idCount) std::fprintf(stderr, "[buttonshow] 布局里找不到 status/count\n");
    int clicks = 0;

    // ---- 点击：鼠标释放与键盘 Enter/Space 走同一条 CLICK 事件 ----
    renderer.setEventCallback([&](const Event& ev) {
        if (ev.type != EventType::CLICK) return;
        if (ev.nodeId == idWinMin)   { app.minimize(); return; }
        if (ev.nodeId == idWinMax)   { app.maximize(); return; }
        if (ev.nodeId == idWinClose) { app.requestExit(); return; }

        const std::string* label = nullptr;
        bool isButton = false;
        for (const auto& n : renderer.getNodes()) {
            if (n.id != ev.nodeId) continue;
            label = &n.text;
            isButton = (n.type == BINUI_BUTTON);
            break;
        }
        if (!isButton || !label) return;     // 点到底色卡片/文字上也别记一笔

        // 「选中」演示按钮：核心库只存状态，切换是外壳的事
        if (ev.nodeId == idTog) {
            renderer.setNodeChecked(idTog, !renderer.isNodeChecked(idTog));
            invalidateNode(idTog);
        }

        ++clicks;
        if (idStatus) {
            renderer.setText(idStatus, "最后点击：" + *label);
            invalidateNode(idStatus);
        }
        if (idCount) {
            renderer.setText(idCount, "点击次数 " + std::to_string(clicks));
            invalidateNode(idCount);
        }
        std::printf("[buttonshow] click: %s\n", label->c_str());
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
        int old = renderer.pressedNode();
        renderer.press(x, y);
        if (old >= 0) invalidateNode((uint16_t)old);
        if (renderer.pressedNode() >= 0) invalidateNode((uint16_t)renderer.pressedNode());
    });
    app.setReleaseFn([&](float x, float y) {
        int pressed = renderer.pressedNode();
        renderer.release(x, y);              // CLICK 事件在 release 里派发
        if (pressed >= 0) invalidateNode((uint16_t)pressed);
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    // 键盘：焦点 / 滚动 / 编辑三态全交给渲染器 —— WASD 移焦点、方向键滚页面、
    // 回车进/出输入状态、Tab 循环，焦点环 2 秒无操作自动熄灭。示例只管重绘和 Esc 退出。
    app.setKeyFn([&](int vk) {
        uint32_t mods = 0;
        if (::GetKeyState(VK_SHIFT)   & 0x8000) mods |= BinUIRenderer::KEYMOD_SHIFT;
        if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= BinUIRenderer::KEYMOD_CTRL;
        if (::GetKeyState(VK_MENU)    & 0x8000) mods |= BinUIRenderer::KEYMOD_ALT;
        const auto kr = renderer.handleKey(vk, mods);
        for (uint16_t id : kr.changed) invalidateNode(id);
        if (!kr.handled && vk == VK_ESCAPE) app.requestExit();
    });

    // 中文输入法会把字母键变成 VK_PROCESSKEY 自己组字 —— 非编辑态必须把 IME 摘掉，
    // WASD 焦点导航才收得到；进输入框编辑时窗口会自动挂回去（组字/候选框照旧）。
    app.setImeActiveFn([&] { return renderer.isEditing(); });
    app.setFocusFn([&](bool focused) {
        if (!focused) {
            renderer.cancelPress(); renderer.clearHover(); renderer.blur();
            renderer.hideFocusRing();
            app.invalidateAll();
        }
    });

    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;
    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "buttonshow", winW, winH) == binui::ex::HotReload::Result::Reloaded)
            app.invalidateAll();
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[buttonshow] 按钮样式一览 (OpenGL 3.3)\n");
    return app.run();
}
