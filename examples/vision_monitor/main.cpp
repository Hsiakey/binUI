// vision_monitor/main.cpp - 视觉监视器演示（静态链接，模拟数据流）
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "platform/app_win32.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"

using namespace binui;

static void PrintUsage() {
    std::printf("用法: vision_monitor [--watch] [--ui <main.binui.xml>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（UI 开发用）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n");
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "vision_monitor", "main.binui.xml");
    std::vector<uint8_t> binui;
    if (!uiPath.empty()) binui = CompileBinUIFile(uiPath);
    if (binui.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[vision_monitor] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        binui = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(binui.data(), binui.size())) {
        std::fprintf(stderr, "[vision_monitor] Failed to load layout\n");
        return 1;
    }

    static const char* logMessages[] = {
        "[12:00:01] WebSocket 连接成功",
        "[12:00:02] 帧数据接收中...",
        "[12:00:03] 识别完成: score=0.9821",
        "[12:00:04] 目标匹配: (320,240) 64x64",
        "[12:00:05] OCR 结果: 1234",
        "[12:00:06] 模板匹配耗时 8ms",
        "[12:00:07] 连接稳定, FPS=30",
        "[12:00:08] 帧数据接收中...",
    };

    float elapsed = 0;
    int logIndex = 0;

    // 运行时更新目标节点：按文本查找（避免硬编码节点 id，布局变更后错位）
    uint16_t infoId = 0, logAId = 0, logBId = 0;
    auto resolveIds = [&]() {
        infoId = logAId = logBId = 0;
        for (const auto& n : renderer.getNodes()) {
            if (n.text == "等待连接...") infoId = n.id;
            else if (n.text == "[系统] 视觉监视器已启动") logAId = n.id;
            else if (n.text == "[系统] 等待 WebSocket 连接...") logBId = n.id;
        }
    };
    resolveIds();

    const auto& nodes = renderer.getNodes();
    int width = nodes.empty() ? 900 : (int)nodes[0].width;
    int height = nodes.empty() ? 680 : (int)nodes[0].height;

    BinUIApp app("Vision Monitor", width, height);

    // 脏矩形：只重绘发生变化的节点区域（局部重绘）
        auto invalidateNode = [&](uint16_t id) {
        // 失效矩形必须含动画后的包围盒：hover 放大时只按布局矩形失效，
        // 长出来的那一圈（边框/底色）不会被重绘，看上去就是"边框被削掉一半"
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    app.setUpdateFn([&](float dt) {
        if (hot.Tick(renderer, "vision_monitor", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            resolveIds();            // 换树后按文本重新定位目标节点
            app.invalidateAll();
        }
        elapsed += dt;
        if (elapsed < 1.0f) return;
        elapsed = 0;

        float fps = 30.0f + (float)(rand() % 10 - 5);
        float score = 0.95f + (float)(rand() % 50) / 1000.0f;
        int matchX = 300 + rand() % 50;
        int matchY = 200 + rand() % 50;

        char infoBuf[512];
        std::snprintf(infoBuf, sizeof(infoBuf),
            "FPS:      %.0f\n"
            "Status:   RUNNING\n"
            "Score:    %.4f\n"
            "Match:    (%d,%d) 64x64\n"
            "OCR:      1234\n"
            "Latency:  %dms",
            fps, score, matchX, matchY, 8 + rand() % 5);
        renderer.setText(infoId, infoBuf);   // 信息面板
        invalidateNode(infoId);

        const char* msg = logMessages[logIndex % 8];
        logIndex++;
        if (logIndex % 2 == 0) {
            renderer.setText(logAId, msg);
            invalidateNode(logAId);
        } else {
            renderer.setText(logBId, msg);
            invalidateNode(logBId);
        }

        std::printf("[vision_monitor] %s\n", msg);
    });

    app.setDrawFn([&](void* hdc, const void* clip) { drawNodesGDI(hdc, renderer, clip); });
    app.setMoveFn([&](float x, float y) {
        int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        int cur = renderer.hoveredNode();
        if (old != cur) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
        }
        if (cur >= 0) {
            const auto* n = [&]() -> const binui::BinUIRenderer::NodeInfo* {
                for (const auto& nd : renderer.getNodes())
                    if (nd.id == (uint16_t)cur) return &nd;
                return nullptr;
            }();
            if (n && (n->flags & BINUI_SCROLLABLE)) invalidateNode((uint16_t)cur);
        }
    });
    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.dispatchClick(x, y);
        int n = renderer.hitTest(x, y);
        if (n >= 0) invalidateNode((uint16_t)n);
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });

    renderer.setEventCallback([&](const Event& ev) {
        if (ev.type == EventType::CLICK) {
            std::printf("[vision_monitor] Click: node=%u (%.1f, %.1f)\n",
                        ev.nodeId, ev.click.x, ev.click.y);
            // 自绘标题栏按钮：─ 最小化，□ 最大化，✕ 关闭
            const auto& nodes = renderer.getNodes();
            for (const auto& n : nodes) {
                if (n.id != ev.nodeId) continue;
                if (n.text == "\xE2\x94\x80") app.minimize();
                else if (n.text == "\xE2\x96\xA1") app.maximize();
                else if (n.text == "\xE2\x9C\x95") app.requestExit();
                break;
            }
        }
    });

    std::printf("[vision_monitor] 视觉监视器已启动\n");
    return app.run();
}
