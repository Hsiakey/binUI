// flexshow/main.cpp - 自适应布局示例
//
// 布局全在 examples/flexshow/main.html 里（百分比 / flex:1 / min-max / 内容宽），
// 这个外壳只做三件事：
//   1. 把窗口尺寸写到状态栏（证明 resize 事件到达、且重排用的是新尺寸）；
//   2. Tab / 方向键 / Enter 接到焦点与激活上（键盘也能操作）；
//   3. 接 CLICK 事件，把"最后点了哪个按钮"写出来。
//
// 这个示例刻意开窗可缩放（其余固定布局的示例都写 window_resize="disable"）：
// 拖边框时后端按显示器刷新率重排 + 重绘，动画不中断。
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
    const std::string uiPath = binui::ex::ResolveUI(args, "flexshow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[flexshow] 未找到 main.html，使用默认布局\n");
        else
            std::fprintf(stderr, "[flexshow] 编译失败，使用默认布局\n");
        res.layout = GenerateDefaultBinUI();
    }
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[flexshow] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 1;

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 1100 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 720  : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.present ? wd.title.c_str() : "自适应布局");

    // 标题栏三颗控制按钮（components/titlebar.bum.html 里的 id：win-min / win-max / win-close）。
    // 组件只负责外观，动作归宿主：最小化 / 最大化·还原 / 关闭窗口。
    const uint16_t idWinMin   = renderer.findNodeByName("win-min");
    const uint16_t idWinMax   = renderer.findNodeByName("win-max");
    const uint16_t idWinClose = renderer.findNodeByName("win-close");

    uint16_t idStatus = renderer.findNodeByName("status");
    if (!idStatus) std::fprintf(stderr, "[flexshow] 布局里找不到 status\n");
    int winW = width, winH = height;   // 当前窗口尺寸（状态栏文案用；resize 时更新）

    auto invalidateNode = [&](uint16_t id) {
        if (id == 0) return;
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };
    // 状态栏那一行：窗口尺寸 + 左侧固定宽度，直观看出"重排用的是新尺寸"
    auto showSize = [&](int w, int h) {
        idStatus = idStatus ? idStatus : renderer.findNodeByName("status");   // 热重载后 id 可能变
        if (!idStatus) return;
        renderer.setText(idStatus, "窗口 " + std::to_string(w) + "×" + std::to_string(h) + " · 侧栏 200 固定");
        invalidateNode(idStatus);
    };
    showSize(width, height);

    renderer.setEventCallback([&](const Event& ev) {
        if (ev.type == EventType::RESIZE) {          // setSize 之后派发，直接用新尺寸
            showSize(ev.resize.width, ev.resize.height);
            return;
        }
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
        if (!isButton || !label) return;
        if (idStatus) {
            idStatus = idStatus ? idStatus : renderer.findNodeByName("status");
            renderer.setText(idStatus, "最后点击：" + *label + " · 窗口 " +
                                           std::to_string(winW) + "×" + std::to_string(winH));
            invalidateNode(idStatus);
        }
        std::printf("[flexshow] click: %s\n", label->c_str());
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
        renderer.release(x, y);
        if (pressed >= 0) invalidateNode((uint16_t)pressed);
    });
    // 滚轮：找光标下最近的可滚动容器滚它（这个示例里就是那段 flex:1 + overflow:auto 的列表）。
    // 不接这一条，滚动条画出来了却滚不动 —— 拖条子走的是 press/setHover 那条路，滚轮是另一条。
    app.setWheelFn([&](float x, float y, float dy) {
        int id = renderer.scrollAt(x, y, dy);
        if (id >= 0) invalidateNode((uint16_t)id);
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    // 键盘：WASD 移焦点、方向键滚页面（这里就是列表卡片）、回车进/出输入态。
    // 全部由 renderer.handleKey 决定，示例只重绘返回的节点 + 没被消费的 Esc 关窗。
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
    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "flexshow", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            app.invalidateAll();
            idStatus = 0;                       // 换树后重找，别拿旧 id
            showSize(winW, winH);
        }
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) {
        winW = w; winH = h;
        // 重排（百分比/flex 按新视口解析）。状态栏文案由 setSize 派发的 RESIZE 事件更新，
        // 这里不重复写，避免一次 resize 两次 setText。
        renderer.setSize(w, h);
    });

    std::printf("[flexshow] 自适应布局 (OpenGL 3.3) —— 拖边框看实时重排\n");
    return app.run();
}
