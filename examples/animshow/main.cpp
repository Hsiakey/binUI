// animshow/main.cpp - binUI 动画实验室（OpenGL 3.3 GPU 后端，无边框窗口）
// 展示：v7 动画系统（@keyframes 关键帧 + transition 过渡 + 运行时 setStyleColor/restartAnimations）
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unordered_map>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

static void PrintUsage() {
    std::printf("用法: animshow [--watch] [--ui <main.html>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（UI 开发用）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n");
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "animshow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[animshow] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[animshow] Failed to load layout\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 1000 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 720 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true,
                         wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(),
                         wd.title.c_str());

    auto findNode = [&](uint16_t id) -> const binui::BinUIRenderer::NodeInfo* {
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

    // 色块（swatch）：面板 (664,464) 内相对 (24,52)，尺寸 56x56
    uint16_t swatchId = 0;
    for (const auto& n : renderer.getNodes())
        if ((int)n.x == 688 && (int)n.y == 516 && (int)n.width == 56 && (int)n.height == 56) {
            swatchId = n.id;
            break;
        }
    const uint32_t palette[] = {
        0xFF3E8BFF, 0xFF2FBF9A, 0xFF38BDF8, 0xFFF2B544, 0xFFFF8A65, 0xFFFF6B6B,
    };
    int palIdx = 0;

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) { drawNodesGLGdi(gfx, renderer, clip); });
    app.setMoveFn([&](float x, float y) {
        int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        int cur = renderer.hoveredNode();
        if (old != cur) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
        }
        if (cur >= 0) {
            const auto* n = findNode((uint16_t)cur);
            if (n && (n->flags & BINUI_SCROLLABLE)) invalidateNode((uint16_t)cur);
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
            for (const auto& n : renderer.getNodes()) {
                if (n.id != (uint16_t)id) continue;
                if (n.text == "\xE2\x88\x92") app.minimize();
                else if (n.text == "\xE2\x96\xA1") app.maximize();
                else if (n.text == "\xE2\x9C\x95") app.requestExit();
                else if (n.text.find("\xE2\x86\xBB") != std::string::npos) {  // ↻ 重播*
                    renderer.restartAnimations();
                    app.invalidateAll();
                    std::printf("[animshow] restartAnimations()\n");
                } else if (n.text.find("\xE2\x96\xB6") != std::string::npos) {  // ▶ 切换色块颜色
                    palIdx = (palIdx + 1) % (sizeof(palette) / sizeof(palette[0]));
                    renderer.setStyleColor(swatchId, ANIM_BG_COLOR, palette[palIdx]);
                    app.invalidateAll();
                    std::printf("[animshow] setStyleColor(swatch) -> #%06X\n",
                                palette[palIdx] & 0xFFFFFF);
                }
                break;
            }
            invalidateNode((uint16_t)id);
        }
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    app.setKeyFn([&](int vk) {
        if (vk == VK_ESCAPE) { app.requestExit(); return; }
        if (renderer.isEditing()) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (vk == VK_BACK) renderer.backspace();
            else if (vk == VK_DELETE) renderer.deleteForward();
            else if (vk == VK_LEFT) renderer.moveCaret(-1, shift);
            else if (vk == VK_RIGHT) renderer.moveCaret(1, shift);
            else if (vk == VK_HOME) renderer.setCaret(0);
            else if (vk == VK_END) renderer.setCaret(renderer.edit().value.size());
            else if (vk == VK_RETURN || vk == VK_TAB || vk == VK_ESCAPE) renderer.blur();
        }
        if (renderer.isEditing()) invalidateNode(renderer.edit().nodeId);
    });
    app.setFocusFn([&](bool focused) {
        if (!focused) {
            renderer.cancelPress();
            renderer.clearHover();
            renderer.blur();
            app.invalidateAll();
        }
    });
    app.setWheelFn([&](float x, float y, float dy) {
        int id = renderer.scrollAt(x, y, dy);
        if (id >= 0) invalidateNode((uint16_t)id);
    });
    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;

    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "animshow", winW, winH) == binui::ex::HotReload::Result::Reloaded)
            app.invalidateAll();          // 换树后整屏重绘（动画时钟随新树重播）
        // v7 动画：推进动画时钟，重绘本帧有变化的节点
        for (uint16_t id : renderer.stepAnimations())
            invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    renderer.setEventCallback([&](const Event& ev) {
        if (ev.type == EventType::CLICK)
            std::printf("[animshow] Click: node=%u (%.1f, %.1f)\n", ev.nodeId, ev.click.x, ev.click.y);
    });

    std::printf("[animshow] binUI 动画实验室 (OpenGL 3.3)\n");
    return app.run();
}
