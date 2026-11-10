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
#include "backends/binui_ui.h"   // 编译期选后端：默认 GL，-DBINUI_GPU_VK 走 Vulkan
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

    UiApp app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true,
                         wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(),
                         wd.title.c_str());

    // 标题栏三颗控制按钮（components/titlebar.bum.html 里的 id：win-min / win-max / win-close）。
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

    auto invalidateNode = [&](uint16_t id) {
        // 失效矩形必须含动画后的包围盒：hover 放大时只按布局矩形失效，
        // 长出来的那一圈（边框/底色）不会被重绘，看上去就是"边框被削掉一半"
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    // <canvas id="cvEq"> 绘制面。**不要缓存它的矩形**：窗口一改大小，layout 会把这
    // 个盒子挪走/缩放（百分比/flex），缓存的旧矩形失效后，每帧只重绘旧位置那一块，
    // 画布剩下的部分就永远停在上一帧 —— 表现为"最大化后动画停了 + 画布割裂"。
    // 失效一律用 renderer.invalidRect(id) 现取（它含动画后的包围盒）。
    uint16_t cvId = renderer.findNodeByName("cvEq");
    if (!cvId) std::fprintf(stderr, "[svgdemo] MISS canvas \"cvEq\"\n");

    // 频谱条数据（updateFn 每帧推进，drawFn 的 canvas 回调读取绘制）
    const int kN = 20;
    float bars[kN] = {};
    for (int k = 0; k < kN; k++) bars[k] = 0.2f + 0.6f * (float)k / (float)kN;

    // ---- 绘制：节点树 + <canvas> 命令式回调 ----
    auto canvasCb = [&](UiBackend& gfx, uint16_t id, float x, float y, float w, float h) {
        if (id != cvId) return;
        float gap = 4.0f;
        float bw = (w - (kN + 1) * gap) / kN;
        for (int k = 0; k < kN; k++) {
            float bh = std::max(4.0f, bars[k] * (h - 16));
            uint32_t c = k % 3 == 0 ? 0xFF3E8BFF : (k % 3 == 1 ? 0xFF38BDF8 : 0xFF2FBF9A);
            gfx.fillRounded(x + gap + k * (bw + gap), y + h - bh, bw, bh, bw / 2, c);
        }
    };
    app.setDrawFn([&](UiBackend& gfx, const void* clip) {
        drawUi(gfx, renderer, clip, canvasCb);
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
    // 键盘：WASD 移焦点 / 方向键滚页面 / 回车进·出输入态，统一走 renderer.handleKey。
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
            app.invalidateAll();
        }
        static const double t0 = NowSec();
        float t = (float)(NowSec() - t0);

        // 每帧推进频谱条 → invalidate 画布区域 → 下一帧 canvas 回调重绘
        for (int k = 0; k < kN; k++) {
            float target = 0.18f + 0.82f * (0.5f + 0.5f * sinf(t * 2.1f + k * 0.62f));
            bars[k] += (target - bars[k]) * std::min(1.0f, dt * 4.0f);
        }
        // 每帧只重绘画布本身：矩形现取（见上面 cvId 处的注释），布局变了也不会失效
        if (cvId) invalidateNode(cvId);

        // CSS 动画推进
        for (uint16_t id : renderer.stepAnimations())
            invalidateNode(id);
        (void)blinkOn; (void)lastBlink;
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[svgdemo] SVG / Canvas 演示 (OpenGL 3.3)\n");
    return app.run();
}
