// test_renderer_real.cpp - 用真实应用 UI 文件驱动渲染器端到端测试
// 覆盖：实际 XML 编译 → 加载 → 命中测试 → 点击/悬停 → 运行时更新
#include "binui_renderer.h"
#include "binui_compiler.h"
#include <cstdio>
#include <cmath>
#include <string>

static int failed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); \
        failed++; \
    } else { \
        printf("PASS: %s\n", name); \
    } \
} while(0)

using namespace binui;

int main() {
    printf("=== test_renderer_real: real app layouts end-to-end ===\n\n");

    // ============ examples/vision_monitor（900x680 三面板监控） ============
    {
        BinUIRenderer r;
        int clicks = 0, lastClick = -1;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::CLICK) { lastClick = (int)ev.nodeId; clicks++; }
        });

        auto ufl = CompileBinUIFile("examples/vision_monitor/main.binui.xml");
        TEST("vision: compile real xml", !ufl.empty());
        if (!ufl.empty()) {
            TEST("vision: layout loads", r.loadLayout(ufl.data(), ufl.size()));
            const auto& n = r.getNodes();
            TEST("vision: 21 nodes", n.size() == 21);
            TEST("vision: root 900x680", n[0].width == 900 && n[0].height == 680);

            // 标题栏三个窗口按钮 ─ □ ✕
            uint16_t closeId = n[8].id; // ✕ (880,0) 20x30
            TEST("vision: hit close button", r.hitTest(890, 15) == (int)closeId);
            TEST("vision: hit minimize", r.hitTest(850, 15) == (int)n[6].id);
            TEST("vision: hit maximize", r.hitTest(870, 15) == (int)n[7].id);
            TEST("vision: click close fires", (r.dispatchClick(890, 15), lastClick == (int)closeId && clicks == 1));

            // 面板/日志区为静态卡片，不可交互
            TEST("vision: panel card not interactive", !(n[1].flags & BINUI_INTERACTIVE));
            TEST("vision: panel area not hit", r.hitTest(90, 200) == -1);
            TEST("vision: log area not hit", r.hitTest(450, 500) == -1);

            // 隐藏"识别"面板不影响其他命中
            r.setVisible(n[14].id, false);
            TEST("vision: hide recognition panel", !r.isNodeVisible(n[14].id));
            TEST("vision: title close still hit", r.hitTest(890, 15) == (int)closeId);
            TEST("vision: hidden panel region not hit", r.hitTest(400, 200) == -1);

            // 运行时更新日志文本
            r.setText(n[19].id, "[系统] WebSocket 已连接");
            TEST("vision: update log line", r.getNodes()[19].text == "[系统] WebSocket 已连接");
            r.setText(n[16].id, "设备: cam-01  帧率: 30fps");
            TEST("vision: update info label", r.getNodes()[16].text == "设备: cam-01  帧率: 30fps");
        }
    }

    printf("\n=== Results: %d tests failed ===\n", failed);
    return failed > 0 ? 1 : 0;
}
