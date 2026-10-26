// svgdemo/main.cpp - binUI <svg> / <canvas> 标签演示（OpenGL 3.3 无边框窗口）
// <svg>：HTML 里声明 rect/circle/line/polyline/polygon，编译器转成普通节点（可 tween/可过渡）
// <canvas>：HTML 空绘制面，drawNodesGLGdi 遇该节点时回调应用（已裁剪到画布矩形），命令式绘制
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
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
    std::printf("用法: svgdemo [--watch] [--ui <main.html>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（UI 开发用）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n");
}

static double NowSec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "svgdemo", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[svgdemo] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[svgdemo] Failed to load layout\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 960 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 660 : (int)nodes[0].height);

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
        const auto* n = findNode(id);
        if (!n || !(n->flags & BINUI_VISIBLE)) return;
        app.invalidateRect((int)n->x, (int)n->y, (int)n->width, (int)n->height);
    };

    // <canvas id="cvEq"> 绘制面：取得节点矩形（绝对坐标）
    uint16_t cvId = renderer.findNodeByName("cvEq");
    if (!cvId) std::fprintf(stderr, "[svgdemo] MISS canvas \"cvEq\"\n");
    float cvX = 0, cvY = 0, cvW = 0, cvH = 0;
    if (const auto* n = findNode(cvId)) { cvX = n->x; cvY = n->y; cvW = n->width; cvH = n->height; }

    // 频谱条数据（updateFn 每帧推进，drawFn 的 canvas 回调读取绘制）
    const int kN = 20;
    float bars[kN] = {};
    for (int k = 0; k < kN; k++) bars[k] = 0.2f + 0.6f * (float)k / (float)kN;

    // ---- 绘制：节点树 + <canvas> 命令式回调 ----
    auto canvasCb = [&](BinUIGLGdi& gfx, uint16_t id, float x, float y, float w, float h) {
        if (id != cvId) return;
        float gap = 4.0f;
        float bw = (w - (kN + 1) * gap) / kN;
        for (int k = 0; k < kN; k++) {
            float bh = std::max(4.0f, bars[k] * (h - 16));
            uint32_t c = k % 3 == 0 ? 0xFF8B5CF6 : (k % 3 == 1 ? 0xFF06B6D4 : 0xFF10B981);
            gfx.fillRounded(x + gap + k * (bw + gap), y + h - bh, bw, bh, bw / 2, c);
        }
    };
    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) {
        drawNodesGLGdi(gfx, renderer, clip, canvasCb);
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
            for (const auto& n : renderer.getNodes()) {
                if (n.id != (uint16_t)id) continue;
                if (n.text == "\xE2\x94\x80") app.minimize();
                else if (n.text == "\xE2\x9C\x95") app.requestExit();
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
    });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); app.invalidateAll(); }
    });
    app.setWheelFn([&](float x, float y, float dy) {
        int id = renderer.scrollAt(x, y, dy);
        if (id >= 0) invalidateNode((uint16_t)id);
    });

    double lastBlink = 0;
    bool blinkOn = true;
    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;

    app.setUpdateFn([&](float dt) {
        if (hot.Tick(renderer, "svgdemo", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            // 换树后本 demo 缓存的节点几何全部失效：按 id 重新取
            cvId = renderer.findNodeByName("cvEq");
            cvX = cvY = cvW = cvH = 0;
            if (const auto* n = findNode(cvId)) { cvX = n->x; cvY = n->y; cvW = n->width; cvH = n->height; }
            app.invalidateAll();
        }
        static const double t0 = NowSec();
        float t = (float)(NowSec() - t0);

        // 每帧推进频谱条 → invalidate 画布区域 → 下一帧 canvas 回调重绘
        for (int k = 0; k < kN; k++) {
            float target = 0.18f + 0.82f * (0.5f + 0.5f * sinf(t * 2.1f + k * 0.62f));
            bars[k] += (target - bars[k]) * std::min(1.0f, dt * 4.0f);
        }
        if (cvId && cvW > 0) {
            app.invalidateRect((int)cvX, (int)cvY, (int)cvW, (int)cvH);
            // 频谱条闪烁的"节拍"指示：脉动画布右下小圆（用 canvas 里画）
        }

        // CSS 动画推进
        for (uint16_t id : renderer.stepAnimations())
            invalidateNode(id);
        (void)blinkOn; (void)lastBlink;
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[svgdemo] SVG / Canvas 演示 (OpenGL 3.3)\n");
    return app.run();
}
