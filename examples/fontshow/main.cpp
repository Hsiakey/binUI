// fontshow/main.cpp - 字体演示：字号 / 字重 / 斜体 / 行高 / 文字装饰 / 混排与符号
//
// 全部能力都在布局层（CSS），所以这个示例的外壳和最小脚手架一样：
// 载入 .bui、转发输入、按动画重绘。重点看 examples/fontshow/main.html。
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
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 640 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.title.c_str());

    auto invalidateNode = [&](uint16_t id) {
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
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
        renderer.press(x, y);
        int n = renderer.hitTest(x, y);
        if (n >= 0) invalidateNode((uint16_t)n);
    });
    app.setReleaseFn([&](float x, float y) {
        int p = renderer.pressedNode();
        int id = renderer.release(x, y);
        if (p >= 0) invalidateNode((uint16_t)p);
        if (id < 0) return;
        invalidateNode((uint16_t)id);
        for (const auto& n : renderer.getNodes()) {
            if (n.id != (uint16_t)id) continue;
            if (n.text == "\xE2\x88\x92") app.minimize();
            else if (n.text == "\xE2\x96\xA1") app.maximize();
            else if (n.text == "\xE2\x9C\x95") app.requestExit();
            break;
        }
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    app.setKeyFn([&](int vk) { if (vk == VK_ESCAPE) app.requestExit(); });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); app.invalidateAll(); }
    });

    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;
    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "fontshow", winW, winH) == binui::ex::HotReload::Result::Reloaded)
            app.invalidateAll();
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[fontshow] 字体演示 (OpenGL 3.3)\n");
    return app.run();
}
