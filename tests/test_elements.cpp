// test_elements.cpp - 元素 / 布局 / 伪元素 / 交互 / 动画 / 热重载（无窗口）
//
// test_binui         : 二进制格式 -> 解析 -> 命中测试往返
// test_renderer      : 元素树 / 布局 / 事件 / docs/issues 回归
// test_renderer_real : 真实示例 UI 端到端（tests/fixtures/vision.binui.xml）
// test_paint         : 圆角九宫格 / 边框条带几何 + 文本尺寸粗估
// test_anim          : @keyframes / transition / 运行时样式 API
// test_hot_reload    : UI 路径解析 + 保存即重载（工作目录须为仓库根）
//
// 由原 tests/test_binui.cpp、test_renderer.cpp、test_renderer_real.cpp、test_paint.cpp、test_anim.cpp、test_hot_reload.cpp 合并而来，逐条用例代码未改（见下面每个 namespace 的分节注释）。

#include "binui_format.h"
#include "binui_renderer.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "binui_compiler.h"
#include <cmath>
#include <chrono>
#include <thread>
#include <string>
#include "binui_paint.h"
#include "binui_path.h"
#include "binui_text_estimate.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include "binui_hot_reload.h"
#include "binui_paths.h"

// ---------------------------------------------------------------------------
// 原 tests/test_binui.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_binui {

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

static std::vector<uint8_t> BuildTestBinUI() {
    const char strings[] = "Hello World\0Button\0User Input\0";
    uint32_t strSize = static_cast<uint32_t>(sizeof(strings));

    uint32_t nodeCount = 4;
    size_t bufSize = sizeof(BinUIHeader) + nodeCount * sizeof(BinUINode) + strSize;
    std::vector<uint8_t> buf(bufSize);
    uint8_t* ptr = buf.data();

    BinUIHeader hdr;
    hdr.magic = BINUI_MAGIC;
    hdr.version = BINUI_VERSION;
    hdr.nodeCount = nodeCount;
    hdr.stringSize = strSize;
    memcpy(ptr, &hdr, sizeof(hdr));
    ptr += sizeof(hdr);

    // Node 0: root container
    BinUINode root = {};
    root.id = 0;
    root.parentId = 0xFFFF;
    root.type = BINUI_ROOT;
    root.flags = BINUI_VISIBLE;
    root.x = 0; root.y = 0;
    root.width = 1280; root.height = 720;
    root.color = 0xFF1A1A2E;
    root.textOffset = 0;
    root.imageIndex = 0;
    root.padding = 0;
    root.fontSize = 0;
    memcpy(ptr, &root, sizeof(root));
    ptr += sizeof(root);

    // Node 1: label
    BinUINode label = {};
    label.id = 1;
    label.parentId = 0;
    label.type = BINUI_LABEL;
    label.flags = BINUI_VISIBLE;
    label.x = 100; label.y = 50;
    label.width = 400; label.height = 40;
    label.color = 0xFFFFFFFF;
    label.textOffset = 0; // "Hello World"
    label.imageIndex = 0;
    label.padding = 0;
    label.fontSize = 24;
    memcpy(ptr, &label, sizeof(label));
    ptr += sizeof(label);

    // Node 2: interactive button
    BinUINode button = {};
    button.id = 2;
    button.parentId = 0;
    button.type = BINUI_BUTTON;
    button.flags = BINUI_VISIBLE | BINUI_INTERACTIVE;
    button.x = 100; button.y = 120;
    button.width = 200; button.height = 50;
    button.color = 0xFF4A90D9;
    button.textOffset = 12; // "Button"
    button.imageIndex = 0;
    button.padding = 0;
    button.fontSize = 16;
    memcpy(ptr, &button, sizeof(button));
    ptr += sizeof(button);

    // Node 3: input field (interactive)
    BinUINode input = {};
    input.id = 3;
    input.parentId = 0;
    input.type = BINUI_INPUT;
    input.flags = BINUI_VISIBLE | BINUI_INTERACTIVE;
    input.x = 100; input.y = 200;
    input.width = 300; input.height = 36;
    input.color = 0xFF333333;
    input.textOffset = 19; // "User Input"
    input.imageIndex = 0;
    input.padding = 0;
    input.fontSize = 14;
    memcpy(ptr, &input, sizeof(input));
    ptr += sizeof(input);

    // String table
    memcpy(ptr, strings, strSize);

    return buf;
}

// 构造一个带指定 magic 的最小布局
static std::vector<uint8_t> BuildLayout(uint32_t magic) {
    const char str[] = "test\0";
    uint32_t nodeCount = 1;
    size_t bufSize = sizeof(BinUIHeader) + nodeCount * sizeof(BinUINode) + sizeof(str);
    std::vector<uint8_t> buf(bufSize);
    uint8_t* ptr = buf.data();

    BinUIHeader hdr;
    hdr.magic = magic;
    hdr.version = BINUI_VERSION;
    hdr.nodeCount = nodeCount;
    hdr.stringSize = sizeof(str);
    memcpy(ptr, &hdr, sizeof(hdr));
    ptr += sizeof(hdr);

    BinUINode node = {};
    node.id = 0;
    node.parentId = 0xFFFF;
    node.type = BINUI_ROOT;
    node.flags = BINUI_VISIBLE;
    node.x = 0; node.y = 0;
    node.width = 480; node.height = 800;
    node.color = 0xFF000000;
    node.textOffset = 0;
    node.imageIndex = 0;
    node.padding = 0;
    node.fontSize = 0;
    memcpy(ptr, &node, sizeof(node));
    ptr += sizeof(node);

    memcpy(ptr, str, sizeof(str));
    return buf;
}

int run() {
    printf("=== test_binui: binUI build / parse / hit-test roundtrip ===\n\n");

    // 两种 magic 均应被渲染器接受（binUI 旧格式 / XHTML 编译格式）
    BinUIRenderer magicRenderer;
    TEST("loadLayout BINUI_MAGIC", magicRenderer.loadLayout(BuildLayout(BINUI_MAGIC).data(), BuildLayout(BINUI_MAGIC).size()));
    TEST("loadLayout BINUI_HTML_MAGIC", magicRenderer.loadLayout(BuildLayout(BINUI_HTML_MAGIC).data(), BuildLayout(BINUI_HTML_MAGIC).size()));
    magicRenderer.shutdown();

    auto buf = BuildTestBinUI();
    size_t expectedStrSize = sizeof("Hello World\0Button\0User Input\0");
    TEST("Buffer size matches expected", buf.size() == sizeof(BinUIHeader) + 4 * sizeof(BinUINode) + expectedStrSize);

    // Parse via renderer
    BinUIRenderer renderer;
    TEST("loadLayout", renderer.loadLayout(buf.data(), buf.size()));

    const auto& nodes = renderer.getNodes();
    TEST("Node count == 4", nodes.size() == 4);

    if (nodes.size() >= 4) {
        TEST("Node[0] type = ROOT", nodes[0].type == BINUI_ROOT);
        TEST("Node[0] id = 0", nodes[0].id == 0);
        TEST("Node[0] visible", nodes[0].flags & BINUI_VISIBLE);
        TEST("Node[0] size 1280x720",
             nodes[0].width == 1280 && nodes[0].height == 720);

        TEST("Node[1] type = LABEL", nodes[1].type == BINUI_LABEL);
        TEST("Node[1] id = 1", nodes[1].id == 1);
        TEST("Node[1] text = Hello World", nodes[1].text == "Hello World");
        TEST("Node[1] position (100,50)",
             nodes[1].x == 100 && nodes[1].y == 50);

        TEST("Node[2] type = BUTTON", nodes[2].type == BINUI_BUTTON);
        TEST("Node[2] id = 2", nodes[2].id == 2);
        TEST("Node[2] interactive", nodes[2].flags & BINUI_INTERACTIVE);
        TEST("Node[2] text = Button", nodes[2].text == "Button");

        TEST("Node[3] type = INPUT", nodes[3].type == BINUI_INPUT);
        TEST("Node[3] id = 3", nodes[3].id == 3);
        TEST("Node[3] text = User Input", nodes[3].text == "User Input");
    }

    // Hit tests
    TEST("Hit on button (150, 140)", renderer.hitTest(150, 140) == 2);
    TEST("Hit on input (150, 210)", renderer.hitTest(150, 210) == 3);
    TEST("Label not hit (non-interactive)", renderer.hitTest(150, 60) == -1);
    TEST("Miss outside UI (0, 0)", renderer.hitTest(0, 0) == -1);
    TEST("Miss outside UI (5000, 5000)", renderer.hitTest(5000, 5000) == -1);
    TEST("Miss between nodes (150, 100)", renderer.hitTest(150, 100) == -1);

    // Update text via renderer API
    renderer.setText(1, "Updated!");
    TEST("Node[1] text updated", nodes[1].text == "Updated!");

    // Hide node
    renderer.setVisible(2, false);
    TEST("Node[2] hidden", !(nodes[2].flags & BINUI_VISIBLE));
    TEST("Hit on hidden button fails", renderer.hitTest(150, 140) == -1);

    // Show node again
    renderer.setVisible(2, true);
    TEST("Node[2] visible again", nodes[2].flags & BINUI_VISIBLE);
    TEST("Hit on visible button works", renderer.hitTest(150, 140) == 2);

    // Event callback test
    int callbackNodeId = -1;
    renderer.setEventCallback([&](const Event& ev) {
        callbackNodeId = ev.nodeId;
    });

    renderer.dispatchClick(150, 140);
    TEST("Event callback fired with node 2", callbackNodeId == 2);

    renderer.dispatchClick(0, 0);
    TEST("Event callback not fired on miss", callbackNodeId == 2);

    renderer.shutdown();
    printf("\n=== Results: %d tests failed ===\n", failed);
    return failed > 0 ? 1 : 0;
}

}  // namespace suite_binui

// ---------------------------------------------------------------------------
// 原 tests/test_renderer.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_renderer {

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

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

static std::vector<uint8_t> BuildMinimalBinUI() {
    BinUIHeader hdr;
    hdr.magic = BINUI_MAGIC;
    hdr.version = BINUI_VERSION;
    hdr.nodeCount = 1;
    hdr.stringSize = 1;

    BinUINode node;
    node.id = 0;
    node.parentId = 0xFFFF;
    node.type = BINUI_ROOT;
    node.flags = BINUI_VISIBLE;
    node.x = 0; node.y = 0;
    node.width = 800; node.height = 600;
    node.color = 0;
    node.textOffset = 0;
    node.imageIndex = 0;
    node.padding = 0;
    node.fontSize = 0;

    std::vector<uint8_t> buf(sizeof(hdr) + sizeof(node) + 1);
    memcpy(buf.data(), &hdr, sizeof(hdr));
    memcpy(buf.data() + sizeof(hdr), &node, sizeof(node));
    buf.back() = '\0';
    return buf;
}

// ============================================================
// docs/issues 回归：05 滚动条样式 / 06 setText 重算高度 / 07 setNodeBounds
// ============================================================
static const BinUIRenderer::NodeInfo* IssueNode(BinUIRenderer& r, uint16_t id) {
    for (const auto& n : r.getNodes())
        if (n.id == id) return &n;
    return nullptr;
}

static void TestIssueRegressions() {
    // ---- 05：滚动条样式默认按根节点底色派生，可覆写、可复位 ----
    {
        const char* dark = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <div style="position:absolute; left:0; top:0; width:200; height:100; overflow:auto">
    <div style="position:absolute; left:0; top:0; width:200; height:600; background:#222"></div>
  </div>
</body></html></app_window>)HTML";
        const char* light = R"HTML(<app_window width="400" height="300" bg="#FFFFFF">
<html width="400" height="300" bg="#FFFFFF"><body>
  <div style="position:absolute; left:0; top:0; width:200; height:100; overflow:auto">
    <div style="position:absolute; left:0; top:0; width:200; height:600; background:#EEE"></div>
  </div>
</body></html></app_window>)HTML";

        const auto binDark = CompileHTML(dark);
        const auto binLight = CompileHTML(light);
        BinUIRenderer rd, rl;
        TEST("05: 深色布局加载", !binDark.empty() && rd.loadLayout(binDark.data(), binDark.size()));
        TEST("05: 浅色布局加载", !binLight.empty() && rl.loadLayout(binLight.data(), binLight.size()));

        TEST("05: 深色主题滚动条保持深灰",
             rd.scrollbarStyle().track == 0xFF2E2E2Eu && rd.scrollbarStyle().thumb == 0xFF5A5A5Au);
        TEST("05: 浅色主题滚动条自动变浅",
             rl.scrollbarStyle().track == 0xFFE5E7EBu && rl.scrollbarStyle().thumb == 0xFFC7CBD1u);
        TEST("05: 默认宽度 8", rl.scrollbarStyle().width == 8);

        rl.setScrollbarStyle(0xFF112233u, 0xFF445566u, 12);
        TEST("05: setScrollbarStyle 覆盖滑块色", rl.scrollbarStyle().thumb == 0xFF112233u);
        TEST("05: setScrollbarStyle 覆盖轨道色", rl.scrollbarStyle().track == 0xFF445566u);
        TEST("05: setScrollbarStyle 覆盖宽度", rl.scrollbarStyle().width == 12);
        rl.setScrollbarStyle(0xFF112233u, 0xFF445566u, 1);
        TEST("05: 宽度下限钳到 2", rl.scrollbarStyle().width == 2);
        rl.setScrollbarStyle(0xFF112233u, 0xFF445566u, 999);
        TEST("05: 宽度上限钳到 32", rl.scrollbarStyle().width == 32);

        rl.resetScrollbarStyleToDefault();
        TEST("05: reset 回到按根底色派生的浅色",
             rl.scrollbarStyle().thumb == 0xFFC7CBD1u && rl.scrollbarStyle().width == 8);
    }

    // ---- 06：heightAuto 的节点 setText 换长文本后要重算高度 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <span id="log" style="position:absolute; left:20; top:20; width:300; font-size:13px">OK</span>
  <span id="fixed" style="position:absolute; left:20; top:200; width:300; height:40; font-size:13px">OK</span>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("06: 编译并加载", !bin.empty() && r.loadLayout(bin.data(), bin.size()));
        const uint16_t logId = r.findNodeByName("log");
        const uint16_t fixedId = r.findNodeByName("fixed");
        TEST("06: 找得到 log 节点", logId != 0);
        TEST("06: log 带 heightAuto 标记", r.specHeightAuto(logId));
        TEST("06: fixed 不带 heightAuto 标记", fixedId != 0 && !r.specHeightAuto(fixedId));

        const auto* n0 = IssueNode(r, logId);
        const float h0 = n0 ? n0->height : -1.0f;
        TEST("06: 初始高度是一行", h0 > 0.0f && h0 < 30.0f);

        // 换成 3 行长文本：「中」×23 @13px ≈ 299px，正好占满 300px 一行
        std::string longText;
        for (int line = 0; line < 3; line++)
            for (int i = 0; i < 23; i++) longText += "\xE4\xB8\xAD";
        r.setText(logId, longText);
        const auto* n1 = IssueNode(r, logId);
        const float h1 = n1 ? n1->height : -1.0f;
        TEST("06: 换长文本后高度变大", h1 > h0 + 30.0f);
        TEST("06: 高度至少容纳 3 行", h1 >= 3.0f * 13.0f * 1.35f - 0.5f);

        // 换回短文本：高度跟着变回来（不是只增不减）
        r.setText(logId, "OK");
        const auto* n2 = IssueNode(r, logId);
        TEST("06: 换回短文本后高度回落", n2 && std::fabs(n2->height - h0) < 0.5f);

        // 作者显式写了 height 的节点不重排
        const auto* f0 = IssueNode(r, fixedId);
        const float fh0 = f0 ? f0->height : -1.0f;
        r.setText(fixedId, longText + longText);
        const auto* f1 = IssueNode(r, fixedId);
        TEST("06: 显式高度节点保持原高", f1 && std::fabs(f1->height - fh0) < 0.01f);
    }

    // ---- 光标形状（WM_SETCURSOR 用）：按光标下是什么节点给 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <input id="fld" style="position:absolute; left:20; top:20; width:200; height:32"/>
  <button id="btn" style="position:absolute; left:20; top:70; width:120; height:32">按钮</button>
  <button id="dis" disabled style="position:absolute; left:20; top:120; width:120; height:32">禁用</button>
  <span id="txt" style="position:absolute; left:20; top:170; width:200; height:20; font-size:13px">纯文本</span>
  <div style="position:absolute; left:240; top:20; width:140; height:120; overflow:auto">
    <div style="position:absolute; left:0; top:0; width:140; height:600; background:#222"></div>
  </div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("光标: 编译并加载", !bin.empty() && r.loadLayout(bin.data(), bin.size()));
        using CK = BinUIRenderer::CursorKind;
        TEST("光标: 输入框是 I 型", r.cursorAt(60, 36) == CK::IBeam);
        TEST("光标: 按钮是手型", r.cursorAt(60, 86) == CK::Hand);
        TEST("光标: 禁用按钮是箭头", r.cursorAt(60, 136) == CK::Arrow);
        TEST("光标: 纯文本是箭头", r.cursorAt(60, 180) == CK::Arrow);
        TEST("光标: 空白处是箭头", r.cursorAt(200, 260) == CK::Arrow);
        // 滚动容器 240..380 宽、20..140 高；右侧 8px 是竖直滚动条
        TEST("光标: 竖直滚动条上是上下箭头", r.cursorAt(374, 60) == CK::SizeNS);
    }

    // ---- 元素缩放（CSS resize）：位图解析 / 热区光标 / 拖动改矩形 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <div id="all"  style="position:absolute; left:20;  top:20;  width:120; height:80; resize:both"></div>
  <div id="h"    style="position:absolute; left:20;  top:120; width:120; height:80; resize:horizontal"></div>
  <div id="tl"   style="position:absolute; left:20;  top:220; width:120; height:60; resize:top-left bottom-right"></div>
  <div id="none" style="position:absolute; left:200; top:20;  width:120; height:80"></div>
  <div id="dis"  disabled style="position:absolute; left:200; top:120; width:120; height:80; resize:both"></div>
  <div id="lr"   style="position:absolute; left:200; top:220; width:120; height:60; resize:left-resize right-resize"></div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("resize: 编译并加载", !bin.empty() && r.loadLayout(bin.data(), bin.size()));
        using CK = BinUIRenderer::CursorKind;
        const uint16_t idAll  = r.findNodeByName("all");
        const uint16_t idH    = r.findNodeByName("h");
        const uint16_t idTl   = r.findNodeByName("tl");
        const uint16_t idNone = r.findNodeByName("none");
        const uint16_t idLr   = r.findNodeByName("lr");
        TEST("resize: both 是八方向全集",
             r.resizeMaskOf(idAll) == RESIZE_ALL && RESIZE_ALL == 0xFF);
        TEST("resize: horizontal 只有左右",
             r.resizeMaskOf(idH) == (RESIZE_LEFT | RESIZE_RIGHT));
        TEST("resize: 只写两角时边中间不响应",
             r.resizeMaskOf(idTl) == (RESIZE_TOP_LEFT | RESIZE_BOTTOM_RIGHT));
        TEST("resize: 没写属性就没有热区", r.resizeMaskOf(idNone) == 0);
        TEST("resize: <名字>-resize 拼法等价", r.resizeMaskOf(idLr) == (RESIZE_LEFT | RESIZE_RIGHT));

        // 光标（盒子 all 在 20..140 × 20..100）
        TEST("resize: 左边是左右光标", r.cursorAt(21, 60) == CK::SizeWE);
        TEST("resize: 右边也是左右光标", r.cursorAt(139, 60) == CK::SizeWE);
        TEST("resize: 上边是上下光标", r.cursorAt(80, 21) == CK::SizeNS);
        TEST("resize: 左上角是 NW 光标", r.cursorAt(21, 21) == CK::SizeNWSE);
        TEST("resize: 右下角是 NW 光标", r.cursorAt(139, 99) == CK::SizeNWSE);
        TEST("resize: 右上角是 NE 光标", r.cursorAt(139, 21) == CK::SizeNESW);
        TEST("resize: 盒子中间是箭头（不是热区）", r.cursorAt(80, 60) == CK::Arrow);
        TEST("resize: horizontal 只有左右边，上面没有", r.cursorAt(80, 121) == CK::Arrow);
        TEST("resize: 只写角时左边中间没有热区", r.cursorAt(21, 250) == CK::Arrow);
        TEST("resize: 只写角时角上仍然有", r.cursorAt(21, 221) == CK::SizeNWSE);
        TEST("resize: 禁用节点没有热区", r.cursorAt(201, 121) == CK::Arrow);

        // 拖右下角：宽高各 +40
        const auto* n0 = IssueNode(r, idAll);
        const float w0 = n0 ? n0->width : 0, h0 = n0 ? n0->height : 0;
        r.press(139, 99);
        TEST("resize: 按下角进入缩放态", r.isResizing() && r.resizingNode() == idAll);
        r.setHover(179, 139);
        const auto* n1 = IssueNode(r, idAll);
        TEST("resize: 右下角拖动同时改宽高",
             n1 && std::fabs(n1->width - (w0 + 40)) < 0.6f &&
             std::fabs(n1->height - (h0 + 40)) < 0.6f && std::fabs(n1->x - 20) < 0.01f);
        float dx = 0, dy = 0, dw = 0, dh = 0;
        TEST("resize: 拖动给出脏矩形", r.takeResizeDirty(dx, dy, dw, dh) && dw >= w0);
        TEST("resize: 脏矩形取一次就清零", !r.takeResizeDirty(dx, dy, dw, dh));
        TEST("resize: 拖动中光标锁住形状", r.cursorAt(300, 300) == CK::SizeNWSE);
        TEST("resize: 松开不产生点击", r.release(179, 139) == -1 && !r.isResizing());

        // 拖左边：x 与宽反向变化，右边界不动
        const auto* ln = IssueNode(r, idAll);
        const float right0 = ln ? ln->x + ln->width : 0;
        r.press(21, 60);
        r.setHover(51, 60);
        const auto* n2 = IssueNode(r, idAll);
        TEST("resize: 拖左边时右边界不动",
             n2 && std::fabs(n2->x - 50) < 0.6f &&
             std::fabs((n2->x + n2->width) - right0) < 0.6f &&
             std::fabs(n2->y - ln->y) < 0.01f);
        r.release(51, 60);

        // 最小尺寸钳制：把右边往左拖过头（右边界随之前的拖动变过，按当前的取）
        const float rightNow = n2->x + n2->width;
        r.press(rightNow - 1, 60);
        r.setHover(-300, 60);
        const auto* n3 = IssueNode(r, idAll);
        TEST("resize: 最小尺寸钳到 8px", n3 && std::fabs(n3->width - BinUIRenderer::kResizeMinSize) < 0.01f);
        r.release(-300, 60);

        // 运行时开关：setResizeMask 关掉就没有热区
        TEST("resize: 运行期关掉热区", r.setResizeMask(idH, 0) && r.resizeMaskOf(idH) == 0 &&
             r.cursorAt(21, 160) == CK::Arrow);

        // 左上角：x/y 与宽高同时变（往左上拉大）
        const auto* t0 = IssueNode(r, idTl);
        TEST("resize: 对角拖拽前矩形正确",
             t0 && std::fabs(t0->x - 20) < 0.01f && std::fabs(t0->width - 120) < 0.01f);
        r.press(21, 221);
        r.setHover(-21, 199);
        const auto* t1 = IssueNode(r, idTl);
        TEST("resize: 拖左上角时 x/y 与宽高同时变",
             t1 && std::fabs(t1->x - (-22)) < 0.6f && std::fabs(t1->width - 162) < 0.6f &&
             std::fabs(t1->y - 198) < 0.6f && std::fabs(t1->height - 82) < 0.6f);
        r.release(-21, 199);
    }

    // ---- 局部重排：盒子变了，子树按规格段（百分比 / 流式 / 覆盖）重排 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <div id="box" style="position:absolute; left:20; top:20; width:200; height:120; padding:10">
    <span id="pct"   style="width:50%; height:16; font-size:12px">百分比</span>
    <span id="flow1" style="width:100%; height:16; font-size:12px">流式一</span>
    <span id="flow2" style="width:100%; height:16; font-size:12px">流式二</span>
  </div>
  <div id="col" style="position:absolute; left:20; top:170; width:200; height:110">
    <div id="a" style="height:40"><span style="height:16; font-size:12px">A</span></div>
    <div id="b" style="height:40"><span style="height:16; font-size:12px">B</span></div>
  </div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("重排: 编译并加载", !bin.empty() && r.loadLayout(bin.data(), bin.size()));
        const uint16_t box = r.findNodeByName("box");
        const uint16_t pct = r.findNodeByName("pct");
        const uint16_t flow1 = r.findNodeByName("flow1");
        const uint16_t flow2 = r.findNodeByName("flow2");
        const uint16_t col = r.findNodeByName("col");
        const uint16_t a = r.findNodeByName("a");
        const uint16_t b = r.findNodeByName("b");

        const auto* p0 = IssueNode(r, pct);
        const auto* f10 = IssueNode(r, flow1);
        const auto* f20 = IssueNode(r, flow2);
        // 内容盒宽 = 200 - 2*10 = 180；50% = 90，100% = 180
        TEST("重排: 初始百分比宽度", p0 && std::fabs(p0->width - 90.0f) < 0.6f);
        TEST("重排: 初始流式宽度", f10 && std::fabs(f10->width - 180.0f) < 0.6f);
        TEST("重排: 流式第二个排在第一个下面",
             f10 && f20 && f20->y >= f10->y + f10->height - 0.01f);

        // 盒子改宽 200 → 300：子节点的百分比/自动宽跟着变
        TEST("重排: resizeNodeBounds 生效", r.resizeNodeBounds(box, 20, 20, 300, 120));
        const auto* p1 = IssueNode(r, pct);
        const auto* f11 = IssueNode(r, flow1);
        TEST("重排: 百分比子节点跟着变宽", p1 && std::fabs(p1->width - 140.0f) < 0.6f);
        TEST("重排: 自动宽子节点跟着变宽", f11 && std::fabs(f11->width - 280.0f) < 0.6f);
        TEST("重排: 子节点跟着父盒子的 x 走",
             p1 && f11 && std::fabs(p1->x - (20 + 10)) < 0.01f &&
             std::fabs(f11->x - (20 + 10)) < 0.01f);

        // 只改盒子再手动重排（宿主换文本撑高时用）：同一个结果
        r.setNodeBounds(box, 20, 20, 100, 120);
        TEST("重排: 只改盒子时子节点先不动", [&] {
            const auto* q = IssueNode(r, pct);
            return q && std::fabs(q->width - 140.0f) < 0.6f;
        }());
        TEST("重排: relayoutSubtree 后百分比重算", r.relayoutSubtree(box));
        const auto* p2 = IssueNode(r, pct);
        TEST("重排: 100 宽的盒子 50% = 40", p2 && std::fabs(p2->width - 40.0f) < 0.6f);

        // 流式父容器：把 A 拖高，B 应该被顶下去
        const auto* a0 = IssueNode(r, a);
        const auto* b0 = IssueNode(r, b);
        const float by0 = b0 ? b0->y : 0;
        TEST("重排: 改高流式子项", r.resizeNodeBounds(a, a0->x, a0->y, a0->width, 90));
        const auto* b1 = IssueNode(r, b);
        TEST("重排: 兄弟跟着让位", b1 && std::fabs(b1->y - (by0 + 50)) < 0.6f);
        TEST("重排: 兄弟自身高度不变", b1 && std::fabs(b1->height - 40) < 0.6f);
        (void)col;

        // 钉住的尺寸在 window resize 后仍保持；解除后回到规格值
        r.setSize(400, 300);
        const auto* a1 = IssueNode(r, a);
        TEST("重排: 窗口重排后仍保持拖动尺寸", a1 && std::fabs(a1->height - 90) < 0.6f);
        r.clearBoundsOverride(a);
        TEST("重排: 解除覆盖后标记消失", !r.hasBoundsOverride(a));
        r.relayoutSubtree(col);
        const auto* a2 = IssueNode(r, a);
        const auto* b2 = IssueNode(r, b);
        TEST("重排: 解除后回到声明高度",
             a2 && std::fabs(a2->height - 40) < 0.6f && b2 && std::fabs(b2->y - by0) < 0.6f);
    }

    // ---- 局部重排 · 自然内容重排：文字换行 / flex 分配 / min·max·auto·百分比 ----
    {
        const char* html = R"HTML(<app_window width="480" height="400" bg="#101014">
<html width="480" height="400" bg="#101014"><body>
  <div id="card" style="position:absolute; left:20; top:20; width:400; height:300; padding:16">
    <span id="para" style="width:100%; font-size:13px">这是一段会换行的正文，用来验证盒子变窄时文字自然折行、节点高度跟着变高，后面的兄弟节点也要被顶下去。</span>
    <div id="chips" style="width:100%; height:30; display:flex; gap:8">
      <div style="flex:1; height:30"></div>
      <div style="flex:1; height:30"></div>
      <div style="flex:1; height:30"></div>
    </div>
    <div id="after" style="width:100%; height:20"></div>
  </div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("自然重排: 编译并加载", !bin.empty() && r.loadLayout(bin.data(), bin.size()));
        const uint16_t card = r.findNodeByName("card");
        const uint16_t para = r.findNodeByName("para");
        const uint16_t chips = r.findNodeByName("chips");
        const uint16_t after = r.findNodeByName("after");
        const uint16_t chip1 = r.getNodes()[0].id;   // 占位，下面现找

        auto node = [&](uint16_t id) { return IssueNode(r, id); };
        const auto* p0 = node(para);
        const auto* a0 = node(after);
        const float h0 = p0 ? p0->height : 0.0f;
        const float y0 = a0 ? a0->y : 0.0f;
        TEST("自然重排: 宽盒子时正文是一两行", h0 > 0.0f && h0 < 40.0f);

        // 把卡片压窄到 200：正文折行变高，后面的兄弟被顶下去
        TEST("自然重排: 压窄卡片", r.resizeNodeBounds(card, 20, 20, 200, 300));
        const auto* p1 = node(para);
        const auto* a1 = node(after);
        TEST("自然重排: 文字换行后高度变大", p1 && p1->height > h0 + 20.0f);
        TEST("自然重排: 兄弟被顶下去", a1 && a1->y > y0 + 20.0f);
        TEST("自然重排: 正文宽度跟着内容盒",
             p1 && std::fabs(p1->width - (200 - 32)) < 0.6f);

        // flex 行按新宽度重新分配：三个 flex:1 等分
        const auto* c1 = node(chips);
        TEST("自然重排: flex 行宽度跟着内容盒", c1 && std::fabs(c1->width - (200 - 32)) < 0.6f);
        std::vector<const BinUIRenderer::NodeInfo*> kids;
        for (const auto& n : r.getNodes())
            if (n.parentId == chips) kids.push_back(&n);
        TEST("自然重排: flex 子项重新等分", kids.size() == 3 &&
             std::fabs(kids[0]->width - kids[2]->width) < 0.6f &&
             kids[0]->width > 1.0f &&
             std::fabs((kids[2]->x + kids[2]->width) - (c1->x + c1->width)) < 0.6f);

        // 再拉宽到 600：正文回到矮的形态，兄弟回来
        const float hNarrow = p1 ? p1->height : 0.0f;   // 先存值：p1 指向的节点会被重排改写
        TEST("自然重排: 拉宽卡片", r.resizeNodeBounds(card, 20, 20, 600, 300));
        const auto* p2 = node(para);
        TEST("自然重排: 变宽后高度回落", p2 && p2->height < hNarrow);
        (void)chip1;
    }

    // ---- 剪贴板的文本层动作：selectedText / deleteSelection / selectAll ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <input id="fld" style="position:absolute; left:20; top:20; width:200; height:32"/>
  <input id="ro" readonly style="position:absolute; left:20; top:70; width:200; height:32" value="只读文本"/>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("剪贴板: 编译并加载", !bin.empty() && r.loadLayout(bin.data(), bin.size()));
        const uint16_t fld = r.findNodeByName("fld");
        const uint16_t ro = r.findNodeByName("ro");
        r.focus(fld);
        TEST("剪贴板: 聚焦输入框", r.isEditing() && r.edit().nodeId == fld);
        TEST("剪贴板: 没选区时 selectedText 为空", r.selectedText().empty());
        TEST("剪贴板: 没选区时 deleteSelection 返回 false", !r.deleteSelection());
        r.insertText("hello 中文");
        TEST("剪贴板: 插入后全选", r.selectAll());
        TEST("剪贴板: 全选取到整串", r.selectedText() == "hello 中文");
        TEST("剪贴板: 删选区清空", r.deleteSelection() && r.edit().value.empty());
        // 只读框：能全选/复制，但不能删
        r.focus(ro);
        TEST("剪贴板: 只读框可聚焦", r.isEditing() && r.edit().nodeId == ro);
        TEST("剪贴板: 只读框可全选", r.selectAll() && !r.selectedText().empty());
        TEST("剪贴板: 只读框拒绝删选区", !r.deleteSelection());
        r.blur();
    }

    // ---- 拖选（extend）与双击选词 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <input id="fld" style="position:absolute; left:20; top:20; width:260; height:32"/>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        r.loadLayout(bin.data(), bin.size());
        const uint16_t fld = r.findNodeByName("fld");
        r.focus(fld);
        r.insertText("hello world 中文");

        // 拖选：锚点落在 2，caret 拖到 5 → 选中 "llo"
        r.setCaretByUtf16Index(2);
        r.setCaretByUtf16Index(5, /*extend*/ true);
        TEST("拖选: 选中区间", r.selectedText() == "llo");
        // 反向拖：caret 回到 1，锚点仍在 2 → 选中 "e"（min..max 口径）
        r.setCaretByUtf16Index(1, true);
        TEST("拖选: 反向仍然正确", r.selectedText() == "e");
        // 不 extend 时清零选区
        r.setCaretByUtf16Index(3);
        TEST("拖选: 不 extend 会清选区", r.selectedText().empty());

        // 双击选词：点在 "world" 中间
        TEST("选词: 命中 world", r.selectWordByUtf16Index(8) && r.selectedText() == "world");
        // 点在开头空格上：退化成"连到下一个空白"的那段（这里就是空格本身之外的空段）

        // 点在空格上：不是"词",所以按"连续非空白"那一段选 —— 这里就是前面的 hello
        TEST("选词: 点在空格上选中前面那段非空白",
             r.selectWordByUtf16Index(5) && r.selectedText() == "hello");
        // CJK 连续段
        TEST("选词: 中文整段", r.selectWordByUtf16Index(12) && r.selectedText() == "中文");
        // 位置越界钳制
        TEST("选词: 越界钳制到末尾的词", r.selectWordByUtf16Index(9999) &&
                                          r.selectedText() == "中文");
        r.blur();
    }

    // ---- 07：setNodeBounds 的边界处理 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014"><body>
  <div id="box" style="position:absolute; left:20; top:20; width:100; height:40"></div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        r.loadLayout(bin.data(), bin.size());
        const uint16_t box = r.findNodeByName("box");
        TEST("07: setNodeBounds 改坐标", box && r.setNodeBounds(box, 33, 44, 120, 55));
        const auto* b = IssueNode(r, box);
        TEST("07: setNodeBounds 生效", b && std::fabs(b->x - 33.0f) < 0.01f &&
                                        std::fabs(b->height - 55.0f) < 0.01f);
        TEST("07: setNodeBounds 拒绝负尺寸", !r.setNodeBounds(box, 0, 0, -1, 10));
        TEST("07: setNodeBounds 拒绝 NaN", !r.setNodeBounds(box, 0, 0, NAN, 10));
        TEST("07: setNodeBounds 不存在的节点返回 false", !r.setNodeBounds(60000, 0, 0, 10, 10));
    }
}

int run() {
    printf("=== test_renderer: BinUIRenderer parse / update / events ===\n\n");

    // 1. Default constructor state
    {
        BinUIRenderer r;
        TEST("No nodes after construction", r.getNodes().empty());
    }

    // 2. Load layout
    {
        BinUIRenderer r;
        auto buf = BuildMinimalBinUI();
        TEST("Load minimal binUI", r.loadLayout(buf.data(), buf.size()));
        TEST("1 node after load", r.getNodes().size() == 1);
        TEST("Node id == 0", r.getNodes()[0].id == 0);
        TEST("Node is ROOT", r.getNodes()[0].type == BINUI_ROOT);

        auto badBuf = std::vector<uint8_t>(4, 0);
        TEST("Reject bad magic", !r.loadLayout(badBuf.data(), badBuf.size()));

        r.shutdown();
        TEST("Nodes cleared after shutdown", r.getNodes().empty());
    }

    // 3. Load resources (无平台依赖)
    {
        BinUIRenderer r;
        std::unordered_map<std::string, std::vector<uint8_t>> res;
        res["test.ttf"] = std::vector<uint8_t>(256, 0);
        res["icon.png"] = std::vector<uint8_t>(512, 0);
        res["data.json"] = std::vector<uint8_t>(100, 0);
        r.loadResources(res);
    }

    // 4. setText / setVisible 对未知节点应安全失败
    {
        BinUIRenderer r;
        auto buf = BuildMinimalBinUI();
        r.loadLayout(buf.data(), buf.size());

        r.setText(99, "x");   // 未知节点
        TEST("setText unknown node no crash", r.getNodes()[0].text.empty());
        r.setVisible(99, true);
        TEST("setVisible unknown node no crash", r.getNodes()[0].flags & BINUI_VISIBLE);
    }

    // 5. setSize triggers resize event
    {
        BinUIRenderer r;
        int resizeW = 0, resizeH = 0;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::RESIZE) {
                resizeW = ev.resize.width;
                resizeH = ev.resize.height;
            }
        });

        r.setSize(1920, 1080);
        TEST("Resize event fired", resizeW == 1920 && resizeH == 1080);
    }

    // 6. 层级可见性：隐藏祖先使整棵子树不可见、不可命中
    {
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"300\">"
            "<card x=\"0\" y=\"0\" w=\"100\" h=\"100\">"
            "<button x=\"5\" y=\"5\" w=\"50\" h=\"30\">hi</button>"
            "</card>"
            "</binui>");
        TEST("hier layout loads", r.loadLayout(ufl.data(), ufl.size()));

        const auto& n = r.getNodes();
        if (n.size() != 3) {
            TEST("hier node count == 3", false);
        } else {
            uint16_t cardId = n[1].id;
            uint16_t btnId  = n[2].id;
            TEST("button parent is card", n[2].parentId == cardId);
            TEST("button visible initially", r.isNodeVisible(btnId));
            TEST("hit button works", r.hitTest(20, 20) == (int)btnId);

            r.setVisible(cardId, false);
            TEST("button hidden via ancestor", !r.isNodeVisible(btnId));
            TEST("hit on hidden subtree fails", r.hitTest(20, 20) == -1);

            r.setVisible(cardId, true);
            TEST("button visible again", r.isNodeVisible(btnId));
            TEST("hit button works again", r.hitTest(20, 20) == (int)btnId);
        }
    }

    // 7. 文本框编辑：焦点 / 光标 / 插入 / 退格 / 选区 / UTF-8 / 失焦提交
    {
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"100\">"
            "<input x=\"10\" y=\"10\" w=\"100\" h=\"30\">abc</input>"
            "</binui>");
        TEST("edit layout loads", r.loadLayout(ufl.data(), ufl.size()));

        uint16_t inputId = r.getNodes()[1].id;
        int focusEvents = 0, inputEvents = 0;
        int lastFocus = -1;
        std::string lastInput;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::FOCUS) { lastFocus = (int)ev.nodeId; focusEvents++; }
            else if (ev.type == EventType::INPUT) { lastInput = ev.input.text; inputEvents++; }
        });

        TEST("not editing initially", !r.isEditing());
        TEST("insertText while unfocused is no-op", !r.insertText("X"));

        TEST("click focuses input", r.dispatchClick(50, 25) == (int)inputId);
        TEST("editing active", r.isEditing() && r.isFocused(inputId));
        TEST("focus event fired", lastFocus == (int)inputId);
        TEST("initial value from layout", r.edit().value == "abc");
        TEST("caret at end", r.edit().caret == 3);

        TEST("insertText appends", r.insertText("X") && r.edit().value == "abcX" && r.edit().caret == 4);
        TEST("input event fired", lastInput == "abcX" && inputEvents == 1);

        TEST("moveCaret left", r.moveCaret(-1));
        TEST("insert in middle", r.insertText("Z") && r.edit().value == "abcZX" && r.edit().caret == 4);
        TEST("backspace deletes char", r.backspace() && r.edit().value == "abcX" && r.edit().caret == 3);

        TEST("move to start", r.moveCaret(-3) && r.edit().caret == 0);
        TEST("caret clamps at start", (r.moveCaret(-1), r.edit().caret == 0));
        TEST("deleteForward deletes char", r.deleteForward() && r.edit().value == "bcX");
        TEST("setCaret clamps to size", r.setCaret(999) && r.edit().caret == 3);

        TEST("shift-move creates selection", r.moveCaret(-1, true)
             && r.edit().selStart == 3 && r.edit().caret == 2);
        TEST("backspace deletes selection", r.backspace()
             && r.edit().value == "bc" && r.edit().selStart == r.edit().caret);
        TEST("insertText replaces selection",
             (r.moveCaret(-1, true), r.insertText("XY")) && r.edit().value == "bXY");

        // UTF-8 安全
        TEST("insert CJK", r.insertText("\xE4\xB8\xAD") /*中*/ && r.edit().value == "bXY中");
        TEST("backspace removes one CJK char",
             r.backspace() && r.edit().value == "bXY" && r.edit().caret == 3);
        TEST("setCaretByUtf16Index maps correctly",
             (r.insertText("\xE4\xB8\xAD\xE5\x9B\xBD"), /*中国*/
              r.setCaretByUtf16Index(3)) && r.edit().caret == 3);

        TEST("click outside blurs", r.dispatchClick(150, 50) == -1 && !r.isEditing());
        TEST("blur commits to node text", r.getNodes()[1].text == "bXY中国");
        TEST("commit fires input event", lastInput == "bXY中国");

        r.blur(); // 已失焦，应为 no-op
        TEST("blur twice is no-op", !r.isEditing());
        TEST("focus non-input is no-op", (r.focus(0), !r.isEditing() && !r.isFocused(0)));
    }

    // 8. 指针状态：hover / press / release 语义
    {
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"300\">"
            "<card x=\"0\" y=\"0\" w=\"100\" h=\"100\">"
            "<button x=\"5\" y=\"5\" w=\"50\" h=\"30\">ok</button>"
            "</card>"
            "<button x=\"5\" y=\"200\" w=\"50\" h=\"30\">two</button>"
            "<input x=\"5\" y=\"250\" w=\"60\" h=\"20\"></input>"
            "</binui>");
        TEST("pointer layout loads", r.loadLayout(ufl.data(), ufl.size()));

        uint16_t btn1 = r.getNodes()[2].id;
        uint16_t btn2 = r.getNodes()[3].id;
        uint16_t inp  = r.getNodes()[4].id;
        int hoverEvents = 0, pressEvents = 0, clickEvents = 0;
        int lastHover = -1, lastPress = -1, lastClick = -1;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::HOVER) { lastHover = ev.nodeId; hoverEvents++; }
            else if (ev.type == EventType::PRESS) { lastPress = ev.nodeId; pressEvents++; }
            else if (ev.type == EventType::CLICK) { lastClick = ev.nodeId; clickEvents++; }
        });

        TEST("no hover initially", r.hoveredNode() == -1 && r.pressedNode() == -1);

        r.setHover(20, 20);
        TEST("hover hits button1", r.hoveredNode() == (int)btn1 && r.isHovered(btn1));
        TEST("hover event fired", lastHover == (int)btn1 && hoverEvents == 1);
        r.setHover(20, 20);
        TEST("hover same pos no repeat event", hoverEvents == 1);
        r.setHover(20, 215);
        TEST("hover moves to button2", r.hoveredNode() == (int)btn2);
        TEST("hover event on enter", lastHover == (int)btn2 && hoverEvents == 2);
        r.setHover(150, 150);
        TEST("hover empty area clears", r.hoveredNode() == -1);
        r.clearHover();
        TEST("clearHover keeps -1", r.hoveredNode() == -1);

        // press / release：在原节点松开才触发 CLICK
        TEST("press button1", r.press(20, 20) == (int)btn1);
        TEST("pressed state", r.isPressed(btn1) && r.pressedNode() == (int)btn1);
        TEST("press event fired", lastPress == (int)btn1 && pressEvents == 1);
        TEST("release on same node clicks", r.release(20, 20) == (int)btn1);
        TEST("click event fired", lastClick == (int)btn1 && clickEvents == 1);
        TEST("pressed cleared after release", r.pressedNode() == -1);

        TEST("press drag-out no click", r.press(20, 20) == (int)btn1 && r.release(80, 80) == -1);
        TEST("no click on drag-out", clickEvents == 1);
        TEST("press empty no click", r.press(150, 150) == -1 && r.release(150, 150) == -1);
        TEST("no click on empty", clickEvents == 1);

        TEST("cancelPress clears", (r.press(20, 20), r.cancelPress(), r.pressedNode() == -1 && r.release(20, 20) == -1));

        // dispatchClick 兼容：一次调用 = press + release（原语义）
        TEST("dispatchClick fires click", r.dispatchClick(20, 20) == (int)btn1 && clickEvents == 2);

        // 按下输入框聚焦（release 后仍保持）
        TEST("press input focuses", r.press(20, 260) == (int)inp);
        TEST("input focused on press", r.isFocused(inp));
        TEST("release input keeps focus", r.release(20, 260) == (int)inp && r.isFocused(inp));
        TEST("press empty blurs", (r.press(150, 150), !r.isEditing()));
    }

    // 9. 键盘导航：方向键在可交互节点间移动焦点
    {
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"300\" height=\"300\">"
            "<button x=\"10\"  y=\"10\"  w=\"80\" h=\"40\">a</button>"
            "<button x=\"110\" y=\"10\"  w=\"80\" h=\"40\">b</button>"
            "<button x=\"210\" y=\"10\"  w=\"80\" h=\"40\">c</button>"
            "<button x=\"10\"  y=\"70\"  w=\"80\" h=\"40\">d</button>"
            "<button x=\"110\" y=\"70\"  w=\"80\" h=\"40\">e</button>"
            "<button x=\"210\" y=\"70\"  w=\"80\" h=\"40\">f</button>"
            "<button x=\"10\"  y=\"130\" w=\"80\" h=\"40\">g</button>"
            "<button x=\"110\" y=\"130\" w=\"80\" h=\"40\">h</button>"
            "<button x=\"210\" y=\"130\" w=\"80\" h=\"40\">i</button>"
            "</binui>");
        TEST("nav layout loads", r.loadLayout(ufl.data(), ufl.size()));
        // nodes[0]=root, nodes[1..9]=a..i
        uint16_t idA = r.getNodes()[1].id, idB = r.getNodes()[2].id, idC = r.getNodes()[3].id;
        uint16_t idD = r.getNodes()[4].id, idE = r.getNodes()[5].id, idF = r.getNodes()[6].id;
        uint16_t idH = r.getNodes()[8].id, idI = r.getNodes()[9].id;

        TEST("no keyboard focus initially", r.keyboardNode() == -1);
        TEST("no focus -> first node on any arrow", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT) == (int)idA);
        TEST("focus on first node", r.isKeyboardFocused(idA));

        TEST("a RIGHT -> b", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT) == (int)idB);
        TEST("b RIGHT -> c", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT) == (int)idC);
        TEST("c RIGHT no candidate keeps focus", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT) == (int)idC);
        TEST("c DOWN -> f", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::DOWN) == (int)idF);
        TEST("f DOWN -> i", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::DOWN) == (int)idI);
        TEST("i RIGHT keeps focus", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT) == (int)idI);
        TEST("i UP -> f", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::UP) == (int)idF);
        TEST("f LEFT -> e", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::LEFT) == (int)idE);
        TEST("e LEFT -> d", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::LEFT) == (int)idD);
        TEST("d UP -> a", r.moveKeyboardFocus(BinUIRenderer::KeyDirection::UP) == (int)idA);

        // 跳过：隐藏节点与不可见祖先
        r.setVisible(idE, false);
        TEST("hidden e skipped on d RIGHT",
             (r.setKeyboardFocus(idD), r.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT) == (int)idF));
        TEST("hidden e skipped on f LEFT",
             (r.setKeyboardFocus(idF), r.moveKeyboardFocus(BinUIRenderer::KeyDirection::LEFT) == (int)idD));
        r.setVisible(idE, true);

        // 直接设置焦点
        TEST("setKeyboardFocus", (r.setKeyboardFocus(idH), r.isKeyboardFocused(idH)));
        TEST("setKeyboardFocus invalid clears", (r.setKeyboardFocus(999), r.keyboardNode() == -1));
        TEST("setKeyboardFocus on hidden clears",
             (r.setVisible(idE, false), r.setKeyboardFocus(idE), r.setVisible(idE, true),
              r.keyboardNode() == -1));

        // 激活：CLICK 事件
        int clicks = 0;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::CLICK) clicks++;
        });
        r.setKeyboardFocus(idB);
        TEST("activate fires click", r.activateKeyboardFocus() && clicks == 1);

        // 激活 INPUT -> 编辑聚焦
        auto ufl2 = CompileXML(
            "<binui width=\"200\" height=\"100\">"
            "<input x=\"10\" y=\"10\" w=\"100\" h=\"30\">v</input>"
            "</binui>");
        BinUIRenderer r2;
        TEST("input nav layout loads", r2.loadLayout(ufl2.data(), ufl2.size()));
        uint16_t inId = r2.getNodes()[1].id;
        r2.setKeyboardFocus(inId);
        TEST("activate input starts editing", r2.activateKeyboardFocus() && r2.isEditing() && r2.isFocused(inId));

        // 点击同步键盘焦点
        TEST("click syncs keyboard focus", r.dispatchClick(130, 90) == (int)idE && r.isKeyboardFocused(idE));
        TEST("click empty keeps focus", r.dispatchClick(150, 250) == -1 && r.keyboardNode() == (int)idE);
    }

    // 9b. Tab 焦点循环：文档顺序、环绕、Shift+Tab 反向、跳过隐藏节点
    {
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"300\" height=\"300\">"
            "<button x=\"10\"  y=\"10\"  w=\"80\" h=\"40\">t1</button>"
            "<input   x=\"10\"  y=\"70\"  w=\"120\" h=\"30\">ed</input>"
            "<button x=\"10\"  y=\"130\" w=\"80\" h=\"40\">t2</button>"
            "<button x=\"200\" y=\"130\" w=\"80\" h=\"40\">hid</button>"
            "<button x=\"110\" y=\"130\" w=\"80\" h=\"40\">t3</button>"
            "</binui>");
        TEST("tab layout loads", r.loadLayout(ufl.data(), ufl.size()));
        uint16_t idT1 = r.getNodes()[1].id, idIn = r.getNodes()[2].id, idT2 = r.getNodes()[3].id;
        uint16_t idHid = r.getNodes()[4].id, idT3 = r.getNodes()[5].id;
        r.setVisible(idHid, false);

        TEST("no focus tab -> first in doc order", r.cycleKeyboardFocus(true) == (int)idT1);
        TEST("t1 tab -> input", r.cycleKeyboardFocus(true) == (int)idIn);
        TEST("input tab -> t2", r.cycleKeyboardFocus(true) == (int)idT2);
        TEST("hidden skipped: t2 tab -> t3", r.cycleKeyboardFocus(true) == (int)idT3);
        TEST("last wraps to first", r.cycleKeyboardFocus(true) == (int)idT1);
        TEST("shift-tab from first wraps to last", r.cycleKeyboardFocus(false) == (int)idT3);
        TEST("shift-tab t3 -> t2", r.cycleKeyboardFocus(false) == (int)idT2);

        // 当前焦点不在候选链（隐藏/失效）→ 视作无焦点，从头开始
        r.setKeyboardFocus(idT3);
        r.setVisible(idT3, false);
        TEST("stale focus restarts from first", r.cycleKeyboardFocus(true) == (int)idT1);
        r.setVisible(idT3, true);

        // 无可交互节点：返回 -1 且不设焦点
        BinUIRenderer r3;
        auto ufl3 = CompileXML("<binui width=\"100\" height=\"100\"><text x=\"0\" y=\"0\" w=\"80\" h=\"20\">hi</text></binui>");
        TEST("empty nav layout loads", r3.loadLayout(ufl3.data(), ufl3.size()));
        TEST("no interactive -> cycle -1", r3.cycleKeyboardFocus(true) == -1 && r3.keyboardNode() == -1);
    }

    // 9c. 键盘路由（handleKey）：WASD 移焦点 / 方向键滚页面 / 回车进·出输入态 / 焦点环超时
    {
        auto ufl = CompileXML(
            "<binui width=\"300\" height=\"300\">"
            "<input  x=\"10\"  y=\"10\" w=\"120\" h=\"30\"></input>"
            "<button x=\"150\" y=\"10\" w=\"80\"  h=\"30\">b</button>"
            "<button x=\"10\"  y=\"60\" w=\"80\"  h=\"30\">c</button>"
            "</binui>");
        BinUIRenderer r;
        TEST("key layout loads", r.loadLayout(ufl.data(), ufl.size()));
        uint16_t inId = r.getNodes()[1].id, bId = r.getNodes()[2].id, cId = r.getNodes()[3].id;
        auto kd = [&](int vk, uint32_t mods = 0) { return r.handleKey(vk, mods); };

        // WASD 移焦点
        TEST("focus starts empty", r.keyboardNode() == -1);
        BinUIRenderer::KeyResult kr = kd('D');
        TEST("D moves focus to first", kr.handled && r.keyboardNode() == (int)inId);
        TEST("ring visible after keyboard focus", r.focusRingVisible());
        TEST("D again -> button", (kd('D'), r.keyboardNode() == (int)bId));
        TEST("S moves down", (kd('S'), r.keyboardNode() == (int)cId));

        // 方向键：没有滚动内容 -> 不消费、焦点不动（以前是移焦点）
        kr = kd(0x28);   // VK_DOWN
        TEST("arrow without scroller unhandled", !kr.handled);
        TEST("arrow does not move focus", r.keyboardNode() == (int)cId);

        // 回车激活按钮 = CLICK
        int clicks = 0;
        r.setEventCallback([&](const Event& ev) { if (ev.type == EventType::CLICK) clicks++; });
        kr = kd(0x0D);
        TEST("enter activates button", kr.handled && clicks == 1);

        // 选中输入框 -> 回车进入编辑；编辑中 WASD 不动焦点；再回车退出
        r.setKeyboardFocus(inId);
        TEST("enter input -> editing", (kd(0x0D), r.isEditing() && r.isFocused(inId)));
        TEST("editing hides ring", !r.focusRingVisible());
        kr = kd('D');
        TEST("WASD ignored while editing", !kr.handled && r.keyboardNode() == (int)inId);
        r.insertText("abc");
        TEST("backspace in edit", kd(0x08).handled && r.edit().value == "ab");
        TEST("enter exits editing", kd(0x0D).handled && !r.isEditing());
        TEST("ring back after editing", r.focusRingVisible());
        TEST("WASD works after editing", (kd('D'), r.keyboardNode() == (int)bId));

        // 编辑中 Tab：退出编辑并跳到下一个控件（浏览器行为）
        r.setKeyboardFocus(inId);
        r.handleKey(0x0D);
        TEST("tab exits editing", kd(0x09).handled && !r.isEditing());
        TEST("tab moved focus on", r.keyboardNode() != (int)inId);

        // 方向键滚页面：焦点在滚动容器里 -> 滚那个容器，焦点不动
        auto scrollUI = CompileHTML(
            "<html width=\"200\" height=\"200\" bg=\"#000000\">"
            "<body>"
            "<div style=\"position:absolute; left:10; top:10; width:180; height:100; overflow:auto\">"
            "<button style=\"position:absolute; left:0; top:0; width:100; height:30\">top</button>"
            "<button style=\"position:absolute; left:0; top:300; width:100; height:30\">low</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer rs;
        TEST("scroll key layout loads", rs.loadLayout(scrollUI.data(), scrollUI.size()));
        uint16_t cont = rs.getNodes()[1].id, topBtn = rs.getNodes()[2].id;
        rs.setKeyboardFocus(topBtn);
        TEST("arrow scrolls focused container",
             rs.handleKey(0x28).handled && rs.scrollState(cont) &&
             rs.scrollState(cont)->y > 0.0f && rs.keyboardNode() == (int)topBtn);
        TEST("arrow up goes back", rs.handleKey(0x26).handled && rs.scrollState(cont)->y == 0.0f);
        TEST("PageDown pages", rs.handleKey(0x22).handled && rs.scrollState(cont)->y >= 90.0f);
        TEST("Home goes to top", rs.handleKey(0x24).handled && rs.scrollState(cont)->y == 0.0f);
        TEST("End goes to bottom",
             rs.handleKey(0x23).handled && rs.scrollState(cont)->y >= rs.contentHeight(cont) - 100.0f);
        // 没有焦点时方向键滚视口内面积最大的容器（浏览器"方向键滚页面"）
        rs.setKeyboardFocus(-1);
        TEST("arrow without focus scrolls page", rs.handleKey(0x24).handled);
        TEST("space without focus pages down",
             rs.handleKey(' ').handled && rs.scrollState(cont)->y > 0.0f);

        // 焦点环 2 秒无操作自动熄灭（测试里把时限压到 50ms）
        BinUIRenderer r2;
        TEST("ring layout loads", r2.loadLayout(ufl.data(), ufl.size()));
        uint16_t b2 = r2.getNodes()[2].id;
        r2.setFocusRingIdleTime(0.05);
        r2.setKeyboardFocus(b2);
        TEST("ring on after setKeyboardFocus", r2.focusRingVisible());
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto changed = r2.stepAnimations();
        TEST("ring expires after idle",
             !r2.focusRingVisible() &&
             std::find(changed.begin(), changed.end(), b2) != changed.end());

        // 鼠标点击不点亮焦点环（focus-visible 语义）
        BinUIRenderer rm;
        TEST("ring mouse layout loads", rm.loadLayout(ufl.data(), ufl.size()));
        rm.dispatchClick(190, 25);   // 按钮 b
        TEST("mouse click sets focus but no ring",
             rm.keyboardNode() >= 0 && !rm.focusRingVisible());

        // 先键盘亮环、再用鼠标点：环立刻收掉（鼠标接管）
        BinUIRenderer rr;
        TEST("ring takeover layout loads", rr.loadLayout(ufl.data(), ufl.size()));
        rr.setKeyboardFocus(rr.getNodes()[2].id);
        TEST("ring on before click", rr.focusRingVisible());
        rr.dispatchClick(190, 25);
        TEST("mouse click clears keyboard ring", !rr.focusRingVisible() && rr.keyboardNode() >= 0);

        // 焦点环三态：输入元素待编辑 = 虚线；按钮等普通焦点 = 实线；编辑中 = Editing（蓝）
        BinUIRenderer rf;
        TEST("ring style layout loads", rf.loadLayout(ufl.data(), ufl.size()));
        uint16_t rfIn = rf.getNodes()[1].id, rfBtn = rf.getNodes()[2].id;
        TEST("no ring by default", rf.focusRingOf(rfIn) == BinUIRenderer::FocusRing::None);
        rf.setKeyboardFocus(rfIn);
        TEST("input browse ring is dashed", rf.focusRingOf(rfIn) == BinUIRenderer::FocusRing::Dashed);
        TEST("other node has no ring", rf.focusRingOf(rfBtn) == BinUIRenderer::FocusRing::None);
        rf.setKeyboardFocus(rfBtn);
        TEST("button browse ring is solid", rf.focusRingOf(rfBtn) == BinUIRenderer::FocusRing::Solid);
        rf.setKeyboardFocus(rfIn);
        rf.handleKey(0x0D);   // Enter -> 进入编辑
        TEST("editing ring is Editing", rf.focusRingOf(rfIn) == BinUIRenderer::FocusRing::Editing);
        TEST("editing ring only on input", rf.focusRingOf(rfBtn) == BinUIRenderer::FocusRing::None);
        TEST("no dashed ring while editing", !rf.focusRingVisible());
        rf.handleKey(0x1B);   // Esc -> 回到元素焦点
        TEST("esc exits editing", !rf.isEditing());
        TEST("esc restores dashed ring", rf.focusRingOf(rfIn) == BinUIRenderer::FocusRing::Dashed);

        // 鼠标点进输入框：进编辑态、设 :focus，但**不画焦点框**（焦点框只给键盘）
        BinUIRenderer rcm;
        TEST("click-edit layout loads", rcm.loadLayout(ufl.data(), ufl.size()));
        uint16_t cmIn = rcm.getNodes()[1].id;
        rcm.dispatchClick(50, 20);   // 输入框 (10,10,120x30)
        TEST("click input enters editing", rcm.isEditing() && rcm.isFocused(cmIn));
        TEST("click input sets focus", rcm.keyboardNode() == (int)cmIn);
        TEST("click input no editing ring", rcm.focusRingOf(cmIn) == BinUIRenderer::FocusRing::None);
        TEST("click input no ring visible", !rcm.focusRingVisible());
        rcm.handleKey(0x1B);   // Esc 退出编辑
        TEST("esc after mouse-edit exits", !rcm.isEditing());
        TEST("esc after mouse-edit keeps no ring",
             rcm.focusRingOf(cmIn) == BinUIRenderer::FocusRing::None && !rcm.focusRingVisible());
        // 键盘再进同一个框：蓝环回来（说明只是"鼠标进来的不画"，不是把编辑环整个关掉）
        rcm.handleKey('D');
        rcm.setKeyboardFocus(cmIn);
        rcm.handleKey(0x0D);
        TEST("keyboard re-entry draws editing ring",
             rcm.isEditing() && rcm.focusRingOf(cmIn) == BinUIRenderer::FocusRing::Editing);

        // 虚线环几何（binui_paint.h，三端共用）：线段数合理，端点不跑出环矩形
        PaintSegment segs[kMaxFocusDashSegments];
        const int dn = BuildDashedRoundRing(10, 10, 120, 40, 8, 6, 4, segs, kMaxFocusDashSegments);
        TEST("dashed ring segments", dn > 10 && dn <= kMaxFocusDashSegments);
        bool inBounds = true;
        float totalLen = 0;
        for (int i = 0; i < dn; i++) {
            const PaintSegment& s = segs[i];
            if (s.x0 < 9.0f || s.x0 > 131.0f || s.y0 < 9.0f || s.y0 > 51.0f) inBounds = false;
            if (s.x1 < 9.0f || s.x1 > 131.0f || s.y1 < 9.0f || s.y1 > 51.0f) inBounds = false;
            totalLen += std::fabs(s.x1 - s.x0) + std::fabs(s.y1 - s.y0);
        }
        TEST("dashed ring within bounds", inBounds);
        TEST("dashed ring covers most of perimeter", totalLen > 0.45f * (120.0f + 40.0f) * 2.0f);

        // 宽输入框（flex:1 拉满窗口）周长很长：段数撞上限时也要整圈画完，
        // 不能只剩上半圈（flexshow 800+px 搜索框踩过这个坑）
        PaintSegment wide[kMaxFocusDashSegments];
        const int wn = BuildDashedRoundRing(0, 0, 1200, 40, 8, 6, 4, wide, kMaxFocusDashSegments);
        TEST("wide dashed ring capped", wn > 20 && wn <= kMaxFocusDashSegments);
        bool sawBottom = false, sawLeft = false, sawRight = false;
        float minX = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int i = 0; i < wn; i++) {
            const PaintSegment& s = wide[i];
            if (s.y0 > 38.0f || s.y1 > 38.0f) sawBottom = true;
            if (s.x0 > 1198.0f || s.x1 > 1198.0f) sawRight = true;
            if (s.x0 < 2.0f || s.x1 < 2.0f) sawLeft = true;
            minX = std::min(minX, std::min(s.x0, s.x1));
            maxX = std::max(maxX, std::max(s.x0, s.x1));
            maxY = std::max(maxY, std::max(s.y0, s.y1));
        }
        TEST("wide dashed ring covers bottom", sawBottom && maxY > 38.0f);
        TEST("wide dashed ring covers left and right", sawLeft && sawRight);
        TEST("wide dashed ring spans full width", minX < 8.0f && maxX > 1192.0f);

        // 超宽（最大化窗口 2500+px）：段数撞上限后按周长拉长实线，仍要整圈画完
        PaintSegment ultra[kMaxFocusDashSegments];
        const int un = BuildDashedRoundRing(0, 0, 3000, 40, 8, 6, 4, ultra, kMaxFocusDashSegments);
        TEST("ultra-wide dashed ring capped", un > 20 && un <= kMaxFocusDashSegments);
        bool ultraBottom = false, ultraLeft = false, ultraRight = false;
        for (int i = 0; i < un; i++) {
            const PaintSegment& s = ultra[i];
            if (s.y0 > 38.0f || s.y1 > 38.0f) ultraBottom = true;
            if (s.x0 > 2998.0f || s.x1 > 2998.0f) ultraRight = true;
            if (s.x0 < 2.0f || s.x1 < 2.0f) ultraLeft = true;
        }
        TEST("ultra-wide dashed ring covers whole outline", ultraBottom && ultraLeft && ultraRight);
    }

    // 9d. svg <line> 在运行期重排里的坐标（width/height 是"终点绝对坐标"，不是尺寸）
    //     回归：窗口 resize 后父容器挪位置，终点没跟着走，✕ 被拉成一条横贯窗口的直线
    {
        auto ufl = CompileHTML(
            "<html width=\"300\" height=\"100\" bg=\"#FFFFFF\"><body>"
            "<div style=\"position:absolute; left:0; top:0; width:100%; height:100%;"
            " display:flex; justify-content:flex-end\">"
            "<svg style=\"width:29; height:29\">"
            "<line x1=\"10.5\" y1=\"10.5\" x2=\"18.5\" y2=\"18.5\" stroke=\"#FF0000\" stroke-width=\"1\"/>"
            "</svg></div></body></html>");
        BinUIRenderer r;
        TEST("line relayout layout loads", r.loadLayout(ufl.data(), ufl.size()));
        auto lineNode = [&]() -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes())
                if (n.type == BINUI_LINE) return &n;
            return nullptr;
        };
        auto svgNode = [&]() -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes())
                if (n.type == BINUI_SVG) return &n;
            return nullptr;
        };
        const BinUIRenderer::NodeInfo* ln = lineNode();
        const BinUIRenderer::NodeInfo* sv = svgNode();
        TEST("line node exists", ln != nullptr && sv != nullptr);
        if (ln && sv) {
            const float dx0 = ln->width - ln->x, dy0 = ln->height - ln->y;
            const float off0 = ln->x - sv->x;
            const float svx0 = sv->x;
            TEST("line initial delta", std::fabs(dx0 - 8.0f) < 0.01f && std::fabs(dy0 - 8.0f) < 0.01f);
            r.setSize(600, 100);   // flex-end：svg 右移，线段必须整体跟着走
            ln = lineNode();
            sv = svgNode();
            const float dx1 = ln->width - ln->x, dy1 = ln->height - ln->y;
            TEST("line delta survives relayout",
                 std::fabs(dx1 - dx0) < 0.01f && std::fabs(dy1 - dy0) < 0.01f);
            TEST("line follows its parent", sv->x > svx0 + 1.0f &&
                 std::fabs((ln->x - sv->x) - off0) < 0.01f);
        }
    }

    // 9e. 元素级 ui_scale="fixed"：等比缩放窗口里，固定子树保持形状（标题栏三颗按钮不变形）
    {
        auto ufl = CompileHTML(
            "<app_window width=\"300\" height=\"200\" bg=\"#FFFFFF\" ui_scale=\"proportional\">"
            "<html width=\"300\" height=\"200\" bg=\"#FFFFFF\"><body>"
            "<div id=\"chrome\" ui_scale=\"fixed\" style=\"position:absolute; left:0; top:0;"
            " width:100%; height:20\">"
            "<div id=\"sq\" style=\"position:absolute; left:10; top:5; width:10; height:10;"
            " background:#FF0000\"></div></div></body></html></app_window>");
        BinUIRenderer r;
        TEST("fixed-scale layout loads", r.loadLayout(ufl.data(), ufl.size()));
        auto findById = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            const uint16_t id = r.findNodeByName(name);
            for (const auto& n : r.getNodes())
                if (n.id == id) return &n;
            return nullptr;
        };
        const BinUIRenderer::NodeInfo* sq = findById("sq");
        TEST("fixed-scale square exists", sq != nullptr);
        if (sq) {
            TEST("fixed-scale square 10x10 at design size",
                 std::fabs(sq->width - 10.0f) < 0.01f && std::fabs(sq->height - 10.0f) < 0.01f);
            r.setSize(600, 300);   // x 比例 2、y 比例 1.5：非等比缩放
            sq = findById("sq");
            TEST("fixed-scale square stays square",
                 std::fabs(sq->width - sq->height) < 0.01f && sq->width > 10.5f);
            TEST("fixed-scale square scales by vertical ratio",
                 std::fabs(sq->width - 15.0f) < 0.51f);
        }
    }

    // 10. flexbox（编译期布局）与滚动容器
    {
        // flex 等分布局
        auto flex = CompileHTML(
            "<html width=\"300\" height=\"100\" bg=\"#000000\">"
            "<body>"
            "<div style=\"display:flex; height:50; gap:4\">"
            "<button style=\"flex:1\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "<button style=\"flex:2\">C</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer r;
        TEST("flex layout loads", r.loadLayout(flex.data(), flex.size()));
        if (r.getNodes().size() >= 5) {
            // nodes[1]=flex 容器 div，nodes[2..4]=A B C
            const auto& a = r.getNodes()[2];
            const auto& b = r.getNodes()[3];
            const auto& c = r.getNodes()[4];
            TEST("flex row same row", std::fabs(a.y - b.y) < 0.01f && std::fabs(b.y - c.y) < 0.01f);
            // 内容宽 300：C 权重 2 -> 宽度 = 2/(1+1+2)*300，含 2 个 gap(4) 扣除
            float remain = 300 - 2 * 4.0f;
            TEST("flex weight widths", std::fabs(a.width - remain / 4) < 0.01f &&
                                       std::fabs(c.width - remain / 2) < 0.01f);
            TEST("flex gap respected", std::fabs(b.x - (a.x + a.width + 4)) < 0.01f);
            TEST("flex fills container", std::fabs(c.x + c.width - 300) < 0.01f);
        } else {
            TEST("flex node count", false);
        }

        // flex 列 + 交叉轴居中
        auto flexc = CompileHTML(
            "<html width=\"200\" height=\"120\" bg=\"#000000\">"
            "<body>"
            "<div style=\"display:flex; flex-direction:column; align-items:center; height:110\">"
            "<button style=\"width:40; height:30\">A</button>"
            "<button style=\"width:40; height:30\">B</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer r2;
        TEST("flex column loads", r2.loadLayout(flexc.data(), flexc.size()));
        if (r2.getNodes().size() >= 4) {
            // nodes[1]=容器，nodes[2]=A，nodes[3]=B
            TEST("flex column stacked", r2.getNodes()[3].y - r2.getNodes()[2].y > 29.0f);
            TEST("align center x", std::fabs(r2.getNodes()[2].x - (200 - 40) / 2) < 0.01f);
        }

        // ---- v22：flex 属性补齐（简写 / shrink / basis / wrap / align-self / order / align-content）----
        {
            auto nodeById = [](const BinUIRenderer& rr, uint16_t id) -> const BinUIRenderer::NodeInfo* {
                for (const auto& nd : rr.getNodes()) if (nd.id == id) return &nd;
                return nullptr;
            };

            // 1) flex 简写：`flex:0 0 120px` = 不伸不缩、固定 120。以前整条被当成一个权重
            //    （"不支持单位 0 120px"）回退成 flex:1 —— 写了固定宽的项静默变成等分。
            auto f1 = CompileHTML(
                "<html width=\"600\" height=\"80\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; height:40\">"
                "<button id=\"k1\" style=\"flex:0 0 120px; height:30\">A</button>"
                "<button id=\"k2\" style=\"flex:1; height:30\">B</button>"
                "</div></body></html>");
            BinUIRenderer r1;
            TEST("flex 简写: loads", r1.loadLayout(f1.data(), f1.size()));
            const auto* k1 = nodeById(r1, r1.findNodeByName("k1"));
            const auto* k2 = nodeById(r1, r1.findNodeByName("k2"));
            TEST("flex 简写: flex:0 0 120px 固定 120 宽", k1 && k2 &&
                 std::fabs(k1->width - 120.0f) < 0.6f && std::fabs(k1->x) < 0.01f);
            TEST("flex 简写: 剩下的 480 全给 flex:1", k1 && k2 &&
                 std::fabs(k2->x - 120.0f) < 0.6f && std::fabs(k2->width - 480.0f) < 0.6f);

            // 2) flex-shrink：溢出时按 shrink×基准 加权收缩（CSS 默认 shrink:1）。
            //    两个 150 挤 200 → 各让 50 变 100。
            auto f2 = CompileHTML(
                "<html width=\"200\" height=\"80\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; height:40\">"
                "<button id=\"s1\" style=\"width:150; height:30\">A</button>"
                "<button id=\"s2\" style=\"width:150; height:30\">B</button>"
                "</div></body></html>");
            BinUIRenderer r2;
            TEST("flex-shrink: loads", r2.loadLayout(f2.data(), f2.size()));
            const auto* s1 = nodeById(r2, r2.findNodeByName("s1"));
            const auto* s2 = nodeById(r2, r2.findNodeByName("s2"));
            TEST("flex-shrink: 默认 1，两个 150 收缩到 100", s1 && s2 &&
                 std::fabs(s1->width - 100.0f) < 0.6f && std::fabs(s2->width - 100.0f) < 0.6f &&
                 std::fabs(s2->x - 100.0f) < 0.6f);

            auto f3 = CompileHTML(
                "<html width=\"200\" height=\"80\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; height:40\">"
                "<button id=\"t1\" style=\"width:150; height:30; flex-shrink:0\">A</button>"
                "<button id=\"t2\" style=\"width:150; height:30\">B</button>"
                "</div></body></html>");
            BinUIRenderer r3;
            TEST("flex-shrink:0 loads", r3.loadLayout(f3.data(), f3.size()));
            const auto* t1 = nodeById(r3, r3.findNodeByName("t1"));
            const auto* t2 = nodeById(r3, r3.findNodeByName("t2"));
            TEST("flex-shrink:0 不缩（150 保持，另一个让到 50）", t1 && t2 &&
                 std::fabs(t1->width - 150.0f) < 0.6f && std::fabs(t2->width - 50.0f) < 0.6f);

            // 3) flex-wrap：300 宽里放 4 个 120（gap 10）→ 一行两个、折成两行；
            //    容器没写 height，auto 高 = 两行交叉轴 + 行间距 = 30 + 10 + 30。
            auto f4 = CompileHTML(
                "<html width=\"300\" height=\"200\" bg=\"#000000\"><body>"
                "<div id=\"wrapbox\" style=\"display:flex; flex-wrap:wrap; gap:10\">"
                "<div id=\"w1\" style=\"width:120; height:30\"></div>"
                "<div id=\"w2\" style=\"width:120; height:30\"></div>"
                "<div id=\"w3\" style=\"width:120; height:30\"></div>"
                "<div id=\"w4\" style=\"width:120; height:30\"></div>"
                "</div></body></html>");
            BinUIRenderer r4;
            TEST("flex-wrap: loads", r4.loadLayout(f4.data(), f4.size()));
            const auto* wb = nodeById(r4, r4.findNodeByName("wrapbox"));
            const auto* w1 = nodeById(r4, r4.findNodeByName("w1"));
            const auto* w2 = nodeById(r4, r4.findNodeByName("w2"));
            const auto* w3 = nodeById(r4, r4.findNodeByName("w3"));
            TEST("flex-wrap: 第三个换到下一行", w1 && w2 && w3 &&
                 std::fabs(w2->y - w1->y) < 0.01f && std::fabs(w3->y - w1->y - 40.0f) < 0.6f);
            TEST("flex-wrap: 容器 auto 高 = 两行 (70)", wb && std::fabs(wb->height - 70.0f) < 0.6f);
            // 运行期重排（窗口拉窄到 240）必须同样折行：编译期/运行期是同一份算法
            r4.setSize(240, 200);
            const auto* w3r = nodeById(r4, r4.findNodeByName("w3"));
            const auto* wbr = nodeById(r4, r4.findNodeByName("wrapbox"));
            TEST("flex-wrap: 运行期改窄后重新折行（4 行 = 30*4+10*3）", w3r && wbr &&
                 std::fabs(w3r->y - 80.0f) < 1.0f && std::fabs(wbr->height - 150.0f) < 1.0f);

            // 4) align-self：逐项覆盖容器的 align-items
            auto f5 = CompileHTML(
                "<html width=\"200\" height=\"120\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; align-items:flex-start; height:100\">"
                "<button id=\"a1\" style=\"width:40; height:20\">A</button>"
                "<button id=\"a2\" style=\"width:40; height:20; align-self:flex-end\">B</button>"
                "<button id=\"a3\" style=\"width:40; height:20; align-self:center\">C</button>"
                "</div></body></html>");
            BinUIRenderer r5;
            TEST("align-self: loads", r5.loadLayout(f5.data(), f5.size()));
            const auto* a2 = nodeById(r5, r5.findNodeByName("a2"));
            const auto* a3 = nodeById(r5, r5.findNodeByName("a3"));
            TEST("align-self:flex-end 贴底 (y = 80)", a2 && std::fabs(a2->y - 80.0f) < 0.6f);
            TEST("align-self:center 居中 (y = 40)", a3 && std::fabs(a3->y - 40.0f) < 0.6f);

            // 5) order：主轴顺序重排，但不改文档顺序
            auto f6 = CompileHTML(
                "<html width=\"300\" height=\"80\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; height:40\">"
                "<button id=\"o1\" style=\"flex:1; height:30; order:2\">1</button>"
                "<button id=\"o2\" style=\"flex:1; height:30; order:0\">2</button>"
                "<button id=\"o3\" style=\"flex:1; height:30; order:1\">3</button>"
                "</div></body></html>");
            BinUIRenderer r6;
            TEST("order: loads", r6.loadLayout(f6.data(), f6.size()));
            const auto* o1 = nodeById(r6, r6.findNodeByName("o1"));
            const auto* o2 = nodeById(r6, r6.findNodeByName("o2"));
            const auto* o3 = nodeById(r6, r6.findNodeByName("o3"));
            TEST("order: x 顺序是 0,1,2", o1 && o2 && o3 &&
                 std::fabs(o2->x) < 0.6f && std::fabs(o3->x - 100.0f) < 0.6f &&
                 std::fabs(o1->x - 200.0f) < 0.6f);

            // 6) align-content：多行在固定高度里 space-between
            auto f7 = CompileHTML(
                "<html width=\"300\" height=\"200\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; flex-wrap:wrap; align-content:space-between;"
                " height:170\">"
                "<div id=\"c1\" style=\"width:120; height:30\"></div>"
                "<div id=\"c2\" style=\"width:120; height:30\"></div>"
                "<div id=\"c3\" style=\"width:120; height:30\"></div>"
                "<div id=\"c4\" style=\"width:120; height:30\"></div>"
                "</div></body></html>");
            BinUIRenderer r7;
            TEST("align-content: loads", r7.loadLayout(f7.data(), f7.size()));
            const auto* c1 = nodeById(r7, r7.findNodeByName("c1"));
            const auto* c3 = nodeById(r7, r7.findNodeByName("c3"));
            // 两行各 30，剩 170-60 = 110 全给行间距：第二行 y = 140
            TEST("align-content:space-between 首尾贴边", c1 && c3 &&
                 std::fabs(c1->y) < 0.6f && std::fabs(c3->y - 140.0f) < 0.6f);

            // 7) flex-basis 长写 / flex:0 0 auto（基准 = 内容宽，不再被当成等分）
            auto f8 = CompileHTML(
                "<html width=\"400\" height=\"80\" bg=\"#000000\"><body>"
                "<div style=\"display:flex; height:40\">"
                "<span id=\"b1\" style=\"flex:0 0 auto; height:20\">AB</span>"
                "<div id=\"b2\" style=\"flex-basis:100px; height:20\"></div>"
                "</div></body></html>");
            BinUIRenderer r8;
            TEST("flex-basis: loads", r8.loadLayout(f8.data(), f8.size()));
            const auto* b1 = nodeById(r8, r8.findNodeByName("b1"));
            const auto* b2 = nodeById(r8, r8.findNodeByName("b2"));
            TEST("flex-basis:100px 固定 100", b2 && std::fabs(b2->width - 100.0f) < 0.6f);
            TEST("flex:0 0 auto 按内容宽（小于 100，不是等分）", b1 && b2 &&
                 b1->width > 0.0f && b1->width < 100.0f && std::fabs(b2->x - b1->width) < 0.6f);
        }

        // ---- v23：事件全家桶（指针 / 焦点 / 编辑 / 滚动 / 滚轮 / 拖拽 / 加载）----
        {
            auto evHtml = CompileHTML(
                "<html width=\"300\" height=\"200\" bg=\"#000000\"><body>"
                "<button id=\"b1\" style=\"position:absolute; left:10; top:10; width:100; height:30\">A</button>"
                "<div id=\"sc\" style=\"position:absolute; left:10; top:60; width:200; height:80; overflow:auto\">"
                "<div style=\"width:400; height:300\"></div></div>"
                "<input id=\"tx\" style=\"position:absolute; left:10; top:150; width:150; height:24\"/>"
                "<div id=\"rz\" style=\"position:absolute; left:220; top:10; width:60; height:40;"
                " background:#333333; resize:both\"></div>"
                "</body></html>");
            BinUIRenderer r;
            std::vector<Event> evs;
            r.setEventCallback([&](const Event& e) { evs.push_back(e); });
            // LOAD：回调先设好，加载完就该报一条（宿主据此做首帧后的事）
            TEST("事件: loadLayout 成功", r.loadLayout(evHtml.data(), evHtml.size()));
            TEST("事件: LOAD 事件", !evs.empty() && evs[0].type == EventType::LOAD);
            const uint16_t b1 = r.findNodeByName("b1");
            const uint16_t tx = r.findNodeByName("tx");
            const uint16_t rz = r.findNodeByName("rz");

            auto countOf = [&](EventType t) {
                int n = 0;
                for (const auto& e : evs) if (e.type == t) n++;
                return n;
            };
            auto lastOf = [&](EventType t) -> const Event* {
                for (size_t i = evs.size(); i-- > 0;) if (evs[i].type == t) return &evs[i];
                return nullptr;
            };

            // MOVE / HOVER / HOVER_LEAVE
            evs.clear();
            r.setHover(50, 25);
            TEST("事件: MOVE 带命中节点", countOf(EventType::MOVE) == 1 &&
                 lastOf(EventType::MOVE)->nodeId == b1 && lastOf(EventType::MOVE)->click.x == 50);
            TEST("事件: HOVER 进入", countOf(EventType::HOVER) == 1 &&
                 lastOf(EventType::HOVER)->nodeId == b1);
            evs.clear();
            r.setHover(250, 190);   // 空白处
            TEST("事件: HOVER_LEAVE 离开", countOf(EventType::HOVER_LEAVE) == 1 &&
                 lastOf(EventType::HOVER_LEAVE)->nodeId == b1);
            TEST("事件: MOVE 到空白 nodeId = 0xFFFF",
                 lastOf(EventType::MOVE) && lastOf(EventType::MOVE)->nodeId == 0xFFFF);

            // PRESS / RELEASE / CLICK / DBL_CLICK（引擎自己判双击，不用宿主接 WM_LBUTTONDBLCLK）
            evs.clear();
            r.press(50, 25);
            r.release(50, 25);
            {   // 中间可能夹着 FOCUS（点击会换键盘焦点）：只断言三者的相对顺序
                int ip = -1, ir = -1, ic = -1;
                for (size_t i = 0; i < evs.size(); i++) {
                    if (evs[i].type == EventType::PRESS && ip < 0) ip = (int)i;
                    else if (evs[i].type == EventType::RELEASE && ir < 0) ir = (int)i;
                    else if (evs[i].type == EventType::CLICK && ic < 0) ic = (int)i;
                }
                TEST("事件: PRESS → RELEASE → CLICK 顺序", ip >= 0 && ip < ir && ir < ic);
            }
            TEST("事件: 三个事件的节点/按键一致", evs[0].nodeId == b1 && evs[1].nodeId == b1 &&
                 evs[2].nodeId == b1 && evs[0].pointer.button == 0);
            TEST("事件: 单击不报双击", countOf(EventType::DBL_CLICK) == 0);
            r.press(50, 25);
            r.release(50, 25);
            TEST("事件: 400ms 内第二次点击 → DBL_CLICK", countOf(EventType::DBL_CLICK) == 1 &&
                 lastOf(EventType::DBL_CLICK)->nodeId == b1);
            r.press(50, 25);
            r.release(50, 25);
            TEST("事件: 三连击不再报双击", countOf(EventType::DBL_CLICK) == 1);

            // CONTEXT_MENU（右键：不进按下态、不抢焦点）
            evs.clear();
            r.press(50, 25, 2);   // DOM 编号：0=左 1=中 2=右
            // 命中口径是"光标下最上层节点"（按钮的文字子节点也算），不是"可交互节点"
            const int ctxNode = r.hitTestAny(50, 25);
            TEST("事件: 右键 → CONTEXT_MENU", countOf(EventType::CONTEXT_MENU) == 1 &&
                 ctxNode >= 0 && lastOf(EventType::CONTEXT_MENU)->nodeId == (uint16_t)ctxNode &&
                 lastOf(EventType::CONTEXT_MENU)->pointer.button == 2);
            TEST("事件: 右键不产生 PRESS", countOf(EventType::PRESS) == 0);

            // 中键：走正常按下流程，松开派发 AUX_CLICK（不产生 CLICK、不算双击）
            evs.clear();
            r.press(50, 25, 1);
            r.release(50, 25, 1);
            TEST("事件: 中键 → PRESS/RELEASE/AUX_CLICK",
                 countOf(EventType::PRESS) == 1 && countOf(EventType::RELEASE) == 1 &&
                 countOf(EventType::AUX_CLICK) == 1 && countOf(EventType::CLICK) == 0);
            TEST("事件: AUX_CLICK 带中键与节点", lastOf(EventType::AUX_CLICK)->nodeId == b1 &&
                 lastOf(EventType::AUX_CLICK)->pointer.button == 1);
            TEST("事件: 中键不进双击判定", countOf(EventType::DBL_CLICK) == 0);
            // 非主按钮的命中口径跟右键一样"点到哪报哪"：普通 div 上中键也要报 AUX_CLICK
            evs.clear();
            r.press(250, 20, 1);
            r.release(250, 20, 1);
            TEST("事件: 普通 div 上中键也报 AUX_CLICK", countOf(EventType::AUX_CLICK) == 1 &&
                 lastOf(EventType::AUX_CLICK)->nodeId == rz);
            // 右键的命中口径是"光标下最上层节点"，不是"可交互节点"：
            // 普通 div（没有 :hover/:active 状态样式）上右键也必须报出它，否则菜单永远不弹。
            evs.clear();
            r.press(250, 20, 2);   // rz 卡片内部（普通 div，无状态样式）
            TEST("事件: 普通 div 上右键也报 CONTEXT_MENU",
                 countOf(EventType::CONTEXT_MENU) == 1 &&
                 lastOf(EventType::CONTEXT_MENU)->nodeId == rz);

            // FOCUS：键盘焦点变化（以前 Tab/WASD/点击换焦点都没有事件）
            r.setKeyboardFocus(-1);   // 先清空，确保下面这次是"从无到有"
            evs.clear();
            r.setKeyboardFocus(b1);
            TEST("事件: 键盘焦点 → FOCUS(focused)", countOf(EventType::FOCUS) == 1 &&
                 lastOf(EventType::FOCUS)->nodeId == b1 && lastOf(EventType::FOCUS)->focus.focused);
            evs.clear();
            r.setKeyboardFocus(-1);
            TEST("事件: 焦点清空 → FOCUS(失焦)", countOf(EventType::FOCUS) == 1 &&
                 lastOf(EventType::FOCUS)->nodeId == b1 && !lastOf(EventType::FOCUS)->focus.focused);

            // EDIT / INPUT / CHANGE：编辑进入-逐字-提交三段分开
            evs.clear();
            r.focus(tx, true);
            TEST("事件: 进入编辑 → EDIT(editing)", countOf(EventType::EDIT) == 1 &&
                 lastOf(EventType::EDIT)->edit.editing);
            r.dispatchInput(tx, "ab");
            TEST("事件: 逐字 → INPUT", countOf(EventType::INPUT) == 1 &&
                 std::string(lastOf(EventType::INPUT)->input.text) == "ab");
            r.blur();
            TEST("事件: 提交 → CHANGE（与 INPUT 分开）", countOf(EventType::CHANGE) == 1 &&
                 lastOf(EventType::CHANGE)->nodeId == tx);
            TEST("事件: 退出编辑 → EDIT(!editing)", !lastOf(EventType::EDIT)->edit.editing);

            // WHEEL / SCROLL：载荷（位置 + 增量）
            evs.clear();
            const int scId = r.scrollAt(100, 100, -120.0f);
            TEST("事件: 滚轮命中滚动容器", scId == (int)r.findNodeByName("sc"));
            TEST("事件: WHEEL 带增量", countOf(EventType::WHEEL) == 1 &&
                 lastOf(EventType::WHEEL)->wheel.dy == -120.0f);
            TEST("事件: SCROLL 带位置与增量", countOf(EventType::SCROLL) == 1 &&
                 lastOf(EventType::SCROLL)->scroll.dy > 0.0f &&
                 lastOf(EventType::SCROLL)->scroll.y > 0.0f);

            // RESIZE_START / RESIZE_END（拖元素尺寸，宿主能拿到开始与最终矩形）
            evs.clear();
            const auto* rzNode = r.getNodes().data();
            float rw0 = 0;
            for (const auto& n : r.getNodes()) if (n.id == rz) rw0 = n.width;
            TEST("事件: 缩放热区按下", r.press(278, 48) == (int)rz);
            TEST("事件: RESIZE_START", countOf(EventType::RESIZE_START) == 1 &&
                 lastOf(EventType::RESIZE_START)->nodeId == rz);
            r.setHover(258, 48);      // 拖到左边 20px
            r.release(258, 48);
            const Event* re = lastOf(EventType::RESIZE_END);
            TEST("事件: RESIZE_END 带最终矩形", re && re->nodeId == rz &&
                 re->rect.w < rw0 - 10.0f && std::fabs(re->rect.w - (rw0 - 20.0f)) < 1.5f);
            (void)rzNode;
        }

        // 滚动容器：overflow:auto 标志 + 命中测试偏移 + 滚动事件
        // 滚动容器：overflow:auto 标志 + 命中测试偏移 + 滚动事件
        auto sc = CompileHTML(
            "<html width=\"200\" height=\"200\" bg=\"#000000\">"
            "<body>"
            "<div style=\"position:absolute; left:10; top:10; width:180; height:100; overflow:auto\">"
            "<button style=\"position:absolute; left:0; top:0; width:100; height:30\">top</button>"
            "<button style=\"position:absolute; left:0; top:200; width:100; height:30\">low</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer r3;
        TEST("scroll layout loads", r3.loadLayout(sc.data(), sc.size()));
        if (r3.getNodes().size() >= 3) {
            uint16_t cont = r3.getNodes()[1].id;
            uint16_t topBtn = r3.getNodes()[2].id;
            uint16_t lowBtn = r3.getNodes()[3].id;
            TEST("container is scrollable", r3.isScrollable(cont));
            TEST("child not scrollable", !r3.isScrollable(topBtn));
            TEST("content height exceeds viewport", r3.contentHeight(cont) > 100.0f);
            TEST("hit top button initially", r3.hitTest(20, 20) == (int)topBtn);
            TEST("low button clipped initially", r3.hitTest(20, 220) == -1);

            int scrollEvents = 0;
            r3.setEventCallback([&](const Event& ev) {
                if (ev.type == EventType::SCROLL) scrollEvents++;
            });
            r3.scrollTo(cont, 0, 150);
            TEST("scrolled state", r3.scrollState(cont) && r3.scrollState(cont)->y == 130.0f);
            TEST("scroll event fired", scrollEvents == 1);
            // 滚动后：按钮滚出视口不可命中；容器本身仍可命中（可交互，支撑悬停/滚轮/拖动）
            TEST("top button no longer hit", r3.hitTest(20, 20) == (int)cont);
            TEST("low button hit after scroll", r3.hitTest(20, 80) == (int)lowBtn);
            TEST("scroll clamped", (r3.scrollTo(cont, 0, 99999), r3.scrollState(cont)->y == 130.0f));
            TEST("scrollBy negative clamps", (r3.scrollBy(cont, 0, -500), r3.scrollState(cont)->y == 0.0f));

            // 滚动条命中
            bool vert = true;
            TEST("scrollbar hit on right edge", r3.hitScrollBar(189, 50, vert) == (int)cont && vert);
            TEST("scrollbar miss elsewhere", r3.hitScrollBar(50, 50, vert) == -1);

            // 拖动滚动条：按下(滚动条)->移动->松开
            r3.press(189, 50);
            r3.setHover(189, 80);
            TEST("drag scrolls", r3.scrollState(cont) && r3.scrollState(cont)->y > 0);
            TEST("release on scrollbar no click", r3.release(189, 80) == -1);

            // 滚轮：光标在容器上 -> 滚动
            r3.scrollTo(cont, 0, 50);
            r3.scrollAt(50, 50, -40);
            TEST("wheel scrolls container", r3.scrollState(cont)->y > 50);
        } else {
            TEST("scroll node count", false);
        }
    }

    // 11. 窗口缩放重排（setSize -> relayout：百分比/流式/flex/自动宽/滚动钳制）
    {
        // 百分比宽高 + 流式堆叠：缩放后按新尺寸重排
        auto ufl = CompileHTML(
            "<html width=\"200\" height=\"200\">"
            "<body>"
            "<div style=\"width:50%; height:25%\">a</div>"
            "<div style=\"width:50%; height:25%\">b</div>"
            "</body>"
            "</html>");
        BinUIRenderer r;
        TEST("resize layout loads", r.loadLayout(ufl.data(), ufl.size()));
        TEST("resize layout has spec", r.hasResizeSpec());
        int resizeEvents = 0, lastW = 0, lastH = 0;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::RESIZE) { resizeEvents++; lastW = ev.resize.width; lastH = ev.resize.height; }
        });

        r.setSize(400, 300);
        const auto& n = r.getNodes();
        TEST("root follows window", n[0].width == 400 && n[0].height == 300);
        TEST("pct width relaid", std::fabs(n[1].width - 200) < 0.01f);
        TEST("pct height relaid", std::fabs(n[1].height - 75) < 0.01f);
        TEST("flow y re-stacked", std::fabs(n[2].y - 75) < 0.01f);
        TEST("resize event fired", resizeEvents == 1 && lastW == 400 && lastH == 300);

        r.setSize(200, 100);
        TEST("second relayout recomputes",
             std::fabs(n[1].width - 100) < 0.01f && std::fabs(n[2].y - 25) < 0.01f);

        // 无 % 的静态节点（绝对定位、显式尺寸）：缩放后位置不变
        auto staticUfl = CompileHTML(
            "<html width=\"100\" height=\"100\">"
            "<body>"
            "<button style=\"position:absolute; left:10; top:10; width:50; height:20\">x</button>"
            "</body>"
            "</html>");
        BinUIRenderer r2;
        TEST("static layout loads", r2.loadLayout(staticUfl.data(), staticUfl.size()));
        r2.setSize(300, 200);
        const auto& s = r2.getNodes();
        TEST("static root follows window", s[0].width == 300 && s[0].height == 200);
        TEST("static child unchanged",
             s[1].width == 50 && s[1].height == 20 && s[1].x == 10 && s[1].y == 10);

        // 自动宽度（未声明 width）：随容器伸缩
        auto autoUfl = CompileHTML(
            "<html width=\"100\" height=\"100\">"
            "<body><button>x</button></body>"
            "</html>");
        BinUIRenderer r3;
        TEST("autow layout loads", r3.loadLayout(autoUfl.data(), autoUfl.size()));
        r3.setSize(500, 200);
        TEST("auto width stretches", std::fabs(r3.getNodes()[1].width - 500) < 0.01f);

        // flex 容器缩放重排：权重重新分配
        auto flexUfl = CompileHTML(
            "<html width=\"300\" height=\"100\">"
            "<body>"
            "<div style=\"display:flex; height:50; gap:4\">"
            "<button style=\"flex:1\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "<button style=\"flex:2\">C</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer r4;
        TEST("flex resize loads", r4.loadLayout(flexUfl.data(), flexUfl.size()));
        r4.setSize(600, 100);
        const auto& f = r4.getNodes();
        float remain = 600 - 2 * 4.0f;
        TEST("flex weight redistributes",
             std::fabs(f[2].width - remain / 4) < 0.01f && std::fabs(f[4].width - remain / 2) < 0.01f);
        TEST("flex refills container", std::fabs(f[4].x + f[4].width - 600) < 0.01f);

        // flex 运行时重排：百分比刚性项 + 弹性项混用（刚性项必须先占位，弹性项只分剩余）
        // 回归：以前 fixedSum 从不累加，弹性项会把百分比兄弟的位置再吃一遍 -> 重叠溢出
        auto flexMixUfl = CompileHTML(
            "<html width=\"640\" height=\"100\">"
            "<body>"
            "<div style=\"display:flex; height:50; gap:0\">"
            "<button style=\"width:25%\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer rm;
        TEST("flex mix loads", rm.loadLayout(flexMixUfl.data(), flexMixUfl.size()));
        rm.setSize(400, 100);
        {
            const auto& m = rm.getNodes();
            // 内容盒 400：A = 25% = 100，B = 剩余 300，B 紧跟 A
            TEST("flex mix: percent child keeps 25%", std::fabs(m[2].width - 100.0f) < 0.01f);
            TEST("flex mix: flexible child gets remainder", std::fabs(m[3].width - 300.0f) < 0.01f);
            TEST("flex mix: no overlap after reflow", std::fabs(m[3].x - 100.0f) < 0.01f);
            TEST("flex mix: row fills container", std::fabs(m[3].x + m[3].width - 400.0f) < 0.01f);
        }

        // flex column 运行时重排：flex:1 沿主轴（高度）分配
        auto flexColUfl = CompileHTML(
            "<html width=\"300\" height=\"400\">"
            "<body>"
            "<div style=\"display:flex; flex-direction:column; height:100%; gap:10\">"
            "<button style=\"flex:1\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer rc;
        TEST("flex column loads", rc.loadLayout(flexColUfl.data(), flexColUfl.size()));
        rc.setSize(300, 410); // 内容高 410 -> 每项 (410-10)/2 = 200
        {
            const auto& c = rc.getNodes();
            TEST("flex column: heights split main axis", std::fabs(c[2].height - 200.0f) < 0.01f);
            TEST("flex column: second starts after first+gap",
                 std::fabs(c[3].y - (c[2].y + c[2].height + 10.0f)) < 0.01f);
        }

        // 滚动容器缩放：视口变化后滚动偏移钳制
        auto scUfl = CompileHTML(
            "<html width=\"200\" height=\"200\">"
            "<body>"
            "<div style=\"position:absolute; left:0; top:0; width:100%; height:50%; overflow:auto\">"
            "<button style=\"position:absolute; left:0; top:0; width:100; height:300\">x</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer r5;
        TEST("scroll resize loads", r5.loadLayout(scUfl.data(), scUfl.size()));
        uint16_t cont = r5.getNodes()[1].id;
        r5.scrollTo(cont, 0, 200); // 初始视口 100，内容 300 -> maxY=200
        TEST("scroll before shrink", r5.scrollState(cont)->y == 200);
        r5.setSize(200, 300);      // 视口变 150 -> maxY=150，200 被钳制
        TEST("scroll clamped after shrink", r5.scrollState(cont)->y == 150);
    }

    // 12. min/max 尺寸约束（min/max-width/height，支持 px 与 %）
    {
        // min-width：自动宽度节点被撑到最小宽
        auto minUfl = CompileHTML(
            "<html width=\"200\" height=\"100\">"
            "<body><button style=\"min-width:120\">x</button></body>"
            "</html>");
        BinUIRenderer r;
        TEST("min-w layout loads", r.loadLayout(minUfl.data(), minUfl.size()));
        r.setSize(100, 100); // 容器 100 < min 120 -> 钳到 120
        TEST("min-width clamps up", std::fabs(r.getNodes()[1].width - 120) < 0.01f);
        r.setSize(300, 100); // 容器 300 > min -> 跟随容器
        TEST("min-width relaxes", std::fabs(r.getNodes()[1].width - 300) < 0.01f);

        // max-width：百分比宽度被钳到上限
        auto maxUfl = CompileHTML(
            "<html width=\"400\" height=\"100\">"
            "<body><button style=\"width:50%; max-width:120\">x</button></body>"
            "</html>");
        BinUIRenderer r2;
        TEST("max-w layout loads", r2.loadLayout(maxUfl.data(), maxUfl.size()));
        r2.setSize(400, 100); // 50% = 200 -> 钳到 120
        TEST("max-width clamps down", std::fabs(r2.getNodes()[1].width - 120) < 0.01f);
        r2.setSize(200, 100); // 50% = 100 < 120 -> 不钳
        TEST("max-width relaxes", std::fabs(r2.getNodes()[1].width - 100) < 0.01f);

        // min/max-height：流式堆叠按约束后高度推进
        auto hUfl = CompileHTML(
            "<html width=\"200\" height=\"200\">"
            "<body>"
            "<div style=\"min-height:80\">a</div>"
            "<div style=\"height:120; max-height:50\">b</div>"
            "</body>"
            "</html>");
        BinUIRenderer r3;
        TEST("h constraint layout loads", r3.loadLayout(hUfl.data(), hUfl.size()));
        const auto& h = r3.getNodes();
        TEST("min-height enforced", std::fabs(h[1].height - 80) < 0.01f);
        TEST("max-height enforced", std::fabs(h[2].height - 50) < 0.01f);
        TEST("stack uses clamped heights", std::fabs(h[2].y - 80) < 0.01f);
        r3.setSize(200, 500);
        TEST("min-height kept on resize", std::fabs(h[1].height - 80) < 0.01f);

        // % 约束：相对父内容盒
        auto pctUfl = CompileHTML(
            "<html width=\"400\" height=\"200\">"
            "<body><button style=\"width:100; min-width:50%\">x</button></body>"
            "</html>");
        BinUIRenderer r4;
        TEST("pct constraint loads", r4.loadLayout(pctUfl.data(), pctUfl.size()));
        r4.setSize(400, 200); // min = 50% * 400 = 200 > 100 -> 钳到 200
        TEST("pct min-width clamps", std::fabs(r4.getNodes()[1].width - 200) < 0.01f);
        r4.setSize(100, 200); // min = 50% * 100 = 50 < 100 -> 不钳
        TEST("pct min-width relaxes", std::fabs(r4.getNodes()[1].width - 100) < 0.01f);

        // flex 子项 max-width：弹性分配后仍受约束（钳制不向兄弟重新分配）
        auto flexConUfl = CompileHTML(
            "<html width=\"600\" height=\"100\">"
            "<body>"
            "<div style=\"display:flex; height:50\">"
            "<button style=\"flex:1; max-width:100\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "</div>"
            "</body>"
            "</html>");
        BinUIRenderer r5;
        TEST("flex constraint loads", r5.loadLayout(flexConUfl.data(), flexConUfl.size()));
        r5.setSize(600, 100);
        const auto& f = r5.getNodes();
        TEST("flex max-width caps item", std::fabs(f[2].width - 100) < 0.01f);
        TEST("flex sibling keeps share", std::fabs(f[3].width - 300) < 0.01f);
        TEST("flex clamped gap left unfilled", std::fabs(f[3].x - 100) < 0.01f);

        // 编译期（初始窗口）即生效：max-width 在初始布局钳制
        auto initUfl = CompileHTML(
            "<html width=\"300\" height=\"100\">"
            "<body><button style=\"width:100%; max-width:150\">x</button></body>"
            "</html>");
        BinUIRenderer r6;
        TEST("compile-time constraint loads", r6.loadLayout(initUfl.data(), initUfl.size()));
        TEST("compile-time max-width applied", std::fabs(r6.getNodes()[1].width - 150) < 0.01f);

        // XML 属性形式
        auto xmlUfl = CompileXML(
            "<binui width=\"300\" height=\"100\">"
            "<button x=\"10\" y=\"10\" w=\"200\" max-width=\"80\">x</button>"
            "</binui>");
        BinUIRenderer r7;
        TEST("xml constraint loads", r7.loadLayout(xmlUfl.data(), xmlUfl.size()));
        TEST("xml max-width applied", std::fabs(r7.getNodes()[1].width - 80) < 0.01f);
        r7.setSize(200, 100);
        TEST("xml max-width kept", std::fabs(r7.getNodes()[1].width - 80) < 0.01f);
    }

    // N. <video> 播放状态机（play/pause/toggle/seek/ended/loop）
    {
        auto res = CompileBinUIEx(
            "<html><body style=\"width:640px;height:480px;background:#111111\">"
            "<video id=\"vid\" src=\"v.mp4\" controls x=\"20\" y=\"20\" width=\"600\" height=\"400\">"
            "</video></body></html>");
        BinUIRenderer r;
        TEST("video layout loads", r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t vid = r.findNodeByName("vid");
        const BinUIRenderer::VideoState* v = r.findVideo(vid);
        TEST("video present", v != nullptr);

        int playEv = 0, pauseEv = 0, endEv = 0, seekEv = 0, clickEv = 0;
        r.setEventCallback([&](const Event& e) {
            switch (e.type) {
                case EventType::VIDEO_PLAY:  playEv++; break;
                case EventType::VIDEO_PAUSE: pauseEv++; break;
                case EventType::VIDEO_ENDED: endEv++; break;
                case EventType::VIDEO_SEEK:  seekEv++; break;
                case EventType::CLICK:       clickEv++; break;
                default: break;
            }
        });

        // 时长探测（引擎报告）+ seek 钳制
        r.setVideoDuration(vid, 10.0);
        TEST("duration reported", v && v->duration == 10.0);
        r.seekVideo(vid, 99.0);
        TEST("seek clamped to duration", v && std::fabs(v->position - 10.0) < 1e-9);
        r.seekVideo(vid, -5.0);
        TEST("seek clamped to zero", v && v->position == 0.0);

        // play/pause/toggle
        TEST("play starts", r.playVideo(vid));
        r.stepAnimations(); sleep_ms(30); r.stepAnimations();
        TEST("playing advances", v && v->playing && v->position > 0.0);
        TEST("play event fired", playEv == 1);
        double posAtPause = v ? v->position : 0;
        TEST("pause stops", r.pauseVideo(vid));
        TEST("pause freezes position", v && !v->playing &&
             std::fabs(v->position - posAtPause) < 0.05);
        TEST("pause event fired", pauseEv == 1);
        TEST("toggle resumes", r.toggleVideo(vid) == true);
        r.stepAnimations();
        TEST("toggle event pair", playEv == 2);

        // 播放到末尾 → ENDED；ended 后再 play 回绕到 0
        r.seekVideo(vid, 9.995);
        for (int i = 0; i < 40 && !(v && v->ended); i++) { r.stepAnimations(); sleep_ms(5); }
        TEST("ended reached", v && v->ended && !v->playing &&
             std::fabs(v->position - 10.0) < 0.05);
        TEST("ended event fired", endEv == 1);
        r.playVideo(vid);
        TEST("replay wraps to start", v && v->playing && v->position < 0.05);

        // loop：末尾回绕不触发 ENDED
        BinUIRenderer rl;
        auto lres = CompileBinUIEx(
            "<html><body style=\"width:640px;height:480px\"><video id=\"lv\" src=\"l.mp4\" loop "
            "x=\"20\" y=\"20\" width=\"600\" height=\"400\"></video></body></html>");
        rl.loadLayout(lres.layout.data(), lres.layout.size());
        uint16_t lid = rl.findNodeByName("lv");
        rl.setVideoDuration(lid, 1.0);
        int lEnd = 0;
        rl.setEventCallback([&](const Event& e) { if (e.type == EventType::VIDEO_ENDED) lEnd++; });
        rl.playVideo(lid);
        rl.seekVideo(lid, 0.999, /*quiet=*/true);
        for (int i = 0; i < 60; i++) { rl.stepAnimations(); sleep_ms(8); }
        const BinUIRenderer::VideoState* lv = rl.findVideo(lid);
        // 断言只依赖「回绕过 seek 点 + 没触发 ENDED」，不依赖实际等待了多久：
        // 之前写 position < 0.9 是时间敏感的——并行构建后的机器抖动会让 60×8ms 变成更长时间，
        // 位置推进超过 0.9 就误报失败（曾在本机 make test 里偶发）。
        TEST("loop wraps without ended",
             lv && lv->playing && lEnd == 0 && lv->position < 0.999);

        // 控制条交互：画面体点击切换播放（不产生普通 CLICK）
        clickEv = 0; playEv = pauseEv = 0;
        bool wasPlaying = v && v->playing;
        r.dispatchClick(320.0f, 100.0f); // 画面中部（控制条在 y=384..420）
        TEST("body click toggles playback", v && v->playing != wasPlaying);
        TEST("no plain CLICK from video", clickEv == 0);
        TEST("toggle emitted play or pause", (playEv + pauseEv) >= 1);

        // 进度条点击：直接定位（bar: y=384..420，轨道 x≈66..554）
        BinUIRenderer::VideoBarGeo gtmp;
        const BinUIRenderer::NodeInfo* vn = nullptr;
        for (const auto& n : r.getNodes()) if (n.id == vid) vn = &n;
        TEST("bar layout valid", vn && BinUIRenderer::VideoBarLayout(*vn, gtmp));
        float trackMidX = gtmp.trX + gtmp.trW * 0.5f;
        float barY = vn ? vn->y + vn->height - BinUIRenderer::kVideoBarH / 2.0f : 0;
        r.pauseVideo(vid);
        playEv = pauseEv = 0;
        r.dispatchClick(trackMidX, barY);
        TEST("slider click seeks", v && v->position > 4.0 && v->position < 6.0);
        TEST("slider click no toggle", playEv == 0 && pauseEv == 0);

        // 键盘激活：Enter/Space 切换播放
        bool beforeKb = v ? v->playing : false;
        r.setKeyboardFocus((int)vid);
        TEST("keyboard activate toggles",
             r.activateKeyboardFocus() && v && v->playing != beforeKb);
    }

    // 绘制顺序：父先于子。absolute 容器 + 流式子节点是常见写法（卡片 + 内容），
    // 曾经全局排序把容器排到子节点之后，容器底色把内容整片盖掉。
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"card\" style=\"position:absolute; left:10; top:10; width:200; height:100; background:#202020\">"
            "<span id=\"row1\" style=\"height:20px\">a</span>"
            "<span id=\"row2\" style=\"height:20px\">b</span>"
            "</div>"
            "<button id=\"btn\" style=\"position:absolute; left:10; top:150; width:80; height:30; background:#333333\">x</button>"
            "</body></html>");
        BinUIRenderer r;
        TEST("drawOrder: parses", r.loadLayout(res.layout.data(), res.layout.size()));
        const std::vector<uint32_t> order = r.drawOrder();
        auto posOf = [&](uint16_t id) -> int {
            for (size_t i = 0; i < order.size(); i++)
                if (r.getNodes()[order[i]].id == id) return static_cast<int>(i);
            return -1;
        };
        const int pc = posOf(r.findNodeByName("card"));
        const int p1 = posOf(r.findNodeByName("row1"));
        const int p2 = posOf(r.findNodeByName("row2"));
        const int pb = posOf(r.findNodeByName("btn"));
        TEST("drawOrder: covers every node", order.size() == r.getNodes().size());
        TEST("drawOrder: absolute container before its flow children", pc >= 0 && p1 > pc && p2 > pc);
        TEST("drawOrder: siblings keep DOM order", p1 >= 0 && p2 > p1);
        TEST("drawOrder: absolute sibling above container", pb > pc && pb > p2);
    }

    // ---- 失效矩形：必须覆盖动画缩放/平移后的包围盒 ----
    // 只按布局矩形失效时，hover 放大出来的那一圈不会被重绘 —— 表现就是
    // "边框被削掉一半"。这条用例把区间关系钉住。
    {
        BinUIRenderer r;
        auto res = CompileBinUIEx(
            "<html><body><button id=\"b\" style=\"left:100; top:60; width:120; height:40;"
            " transition: transform 0.2s ease; background:#333; color:#fff\">x</button>"
            "</body></html>");
        TEST("invalidRect: 布局可解析", !res.layout.empty() &&
             r.loadLayout(res.layout.data(), res.layout.size()));
        const uint16_t id = r.findNodeByName("b");
        float x0 = 0, y0 = 0, w0 = 0, h0 = 0;
        TEST("invalidRect: 静止时等于布局矩形",
             r.invalidRect(id, x0, y0, w0, h0, 0.0f) &&
             std::fabs(x0 - 100.0f) < 0.6f && std::fabs(y0 - 60.0f) < 0.6f &&
             std::fabs(w0 - 120.0f) < 0.6f && std::fabs(h0 - 40.0f) < 0.6f);

        // 触发 hover（按钮默认有 1.05 放大 + 提亮），把过渡走完
        r.setHover(160.0f, 80.0f);
        for (int i = 0; i < 60; i++) r.stepAnimations();
        const float sc = r.drawScale(id);
        float x1 = 0, y1 = 0, w1 = 0, h1 = 0;
        const bool ok = r.invalidRect(id, x1, y1, w1, h1, 0.0f);
        if (sc > 1.0f) {
            const float hw = 120.0f * sc * 0.5f, hh = 40.0f * sc * 0.5f;
            TEST("invalidRect: hover 放大后覆盖放大后的包围盒",
                 ok && x1 <= 160.0f - hw + 0.6f && y1 <= 80.0f - hh + 0.6f &&
                 x1 + w1 >= 160.0f + hw - 0.6f && y1 + h1 >= 80.0f + hh - 0.6f);
            TEST("invalidRect: 同时仍覆盖原布局矩形",
                 ok && x1 <= 100.6f && y1 <= 60.6f &&
                 x1 + w1 >= 219.4f && y1 + h1 >= 99.4f);
        } else {
            TEST("invalidRect: 未进入缩放态时仍返回布局矩形", ok &&
                 std::fabs(x1 - 100.0f) < 0.6f && std::fabs(w1 - 120.0f) < 0.6f);
        }
    }

        // ---- v14：disabled / readonly / placeholder 的运行时语义 ----
    {
        const char* ui =
            "<app_window width=\"400\" height=\"300\" bg=\"#101010\"><html><body>\n"
            "  <button id=\"on\" style=\"position:absolute;left:10;top:10;width:100;height:40\">ON</button>\n"
            "  <button id=\"off\" disabled style=\"position:absolute;left:10;top:60;width:100;height:40\">OFF</button>\n"
            "  <input id=\"ph\" type=\"text\" placeholder=\"提示\" style=\"position:absolute;left:10;top:110;width:200;height:40\">\n"
            "  <input id=\"ro\" type=\"text\" readonly value=\"只读\" style=\"position:absolute;left:10;top:160;width:200;height:40\">\n"
            "</body></html></app_window>\n";
        auto res = CompileBinUIEx(ui);
        BinUIRenderer r;
        TEST("v14r: 布局可解析", r.loadLayout(res.layout.data(), res.layout.size()));
        const uint16_t on = r.findNodeByName("on");
        const uint16_t off = r.findNodeByName("off");
        const uint16_t phNode = r.findNodeByName("ph");
        const uint16_t ro = r.findNodeByName("ro");

        auto rectOf = [&](uint16_t id, float& cx, float& cy) {
            for (const auto& n : r.getNodes())
                if (n.id == id) { cx = n.x + n.width * 0.5f; cy = n.y + n.height * 0.5f; return true; }
            return false;
        };
        float cx = 0, cy = 0;
        TEST("v14r: 正常按钮可命中", rectOf(on, cx, cy) && r.hitTest(cx, cy) == (int)on);
        TEST("v14r: 禁用按钮不可命中", rectOf(off, cx, cy) && r.hitTest(cx, cy) == -1);
        TEST("v14r: 点禁用按钮不产生按下", (r.press(cx, cy), r.pressedNode() < 0));
        // Tab 链跳过禁用控件
        int first = r.cycleKeyboardFocus(true);
        int second = r.cycleKeyboardFocus(true);
        TEST("v14r: Tab 链跳过禁用控件", first == (int)on && second != (int)off);

        // placeholder：聚焦后编辑值是空串（而不是提示语）
        r.setKeyboardFocus(0);
        r.focus(phNode);
        TEST("v14r: 占位符聚焦后编辑值为空", r.isEditing() && r.edit().value.empty());
        TEST("v14r: 占位符输入后标记撤销", (r.insertText("hi"), r.edit().value == "hi"));
        r.blur();
        TEST("v14r: 输入后变成真实值", !r.isPlaceholder(phNode) && r.inputValue(phNode) == "hi");

        // 失焦留空：占位符保留（换一份干净布局，上一段已经把标记消费掉了）
        BinUIRenderer r2;
        TEST("v14r: 第二份布局可解析", r2.loadLayout(res.layout.data(), res.layout.size()));
        const uint16_t ph2 = r2.findNodeByName("ph");
        r2.focus(ph2);
        r2.blur();
        TEST("v14r: 空输入失焦后仍是占位状态",
             r2.isPlaceholder(ph2) && r2.inputValue(ph2).empty() &&
             !r2.getNodes().empty());

        // readonly：可聚焦，但不能改
        r.focus(ro);
        TEST("v14r: 只读可聚焦", r.isEditing() && r.edit().nodeId == ro);
        TEST("v14r: 只读拒绝插入", !r.insertText("x"));
        TEST("v14r: 只读拒绝退格", !r.backspace());
        TEST("v14r: 只读不画光标", !r.isFocused(ro));
        r.blur();
    }

    TestIssueRegressions();

printf("\n=== Results: %d tests failed ===\n", failed);
    return failed > 0 ? 1 : 0;
}

}  // namespace suite_renderer

// ---------------------------------------------------------------------------
// 原 tests/test_renderer_real.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_renderer_real {
// test_renderer_real.cpp - 用真实应用 UI 文件驱动渲染器端到端测试
// 覆盖：实际 XML 编译 → 加载 → 命中测试 → 点击/悬停 → 运行时更新

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

int run() {
    printf("=== test_renderer_real: real app layouts end-to-end ===\n\n");

    // ============ tests/fixtures/vision.binui.xml（900x680 三面板监控，原 vision_monitor 示例） ============
    {
        BinUIRenderer r;
        int clicks = 0, lastClick = -1;
        r.setEventCallback([&](const Event& ev) {
            if (ev.type == EventType::CLICK) { lastClick = (int)ev.nodeId; clicks++; }
        });

        auto ufl = CompileBinUIFile("tests/fixtures/vision.binui.xml");
        TEST("vision: compile real xml", !ufl.empty());
        if (!ufl.empty()) {
            TEST("vision: layout loads", r.loadLayout(ufl.data(), ufl.size()));
            const auto& n = r.getNodes();
            TEST("vision: 72 nodes", n.size() == 72);
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

}  // namespace suite_renderer_real

// ---------------------------------------------------------------------------
// 原 tests/test_paint.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_paint {
// test_paint.cpp - 圆角九宫格几何（include/binui_paint.h）+ 文本尺寸粗估（binui_text_estimate.h）
//
// 这两块都是"三个后端共用的一份数学"，不依赖窗口/字体，所以能纯 CPU 验证。
// 重点是 docs/issues/01 那条：半径到短边一半时九个块必须仍然精确铺满盒子
// （正圆/胶囊），中段宽高为 0 而不是负数。


using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECK_NEAR(a, b, eps, msg) \
    do { if (std::fabs((a) - (b)) > (eps)) { \
        std::printf("FAIL: %s (%.4f vs %.4f)\n", msg, (double)(a), (double)(b)); g_fail++; } } while (0)

// 九块面积之和：等于 w*h 就说明既没有缝也没有重叠（九个块按 (c, mid, c) 正交切分）
static float SumArea(const PaintQuad* q, int n) {
    float s = 0;
    for (int i = 0; i < n; i++) s += q[i].w * q[i].h;
    return s;
}

static int CheckTiling(float w, float h, float r, int expectN, const char* tag) {
    PaintQuad q[9];
    const int t = RoundTemplateSize(static_cast<int>(r));
    const int n = BuildRoundedNineSlice(0, 0, w, h, r, t, q, 9);
    if (n != expectN) {
        std::printf("FAIL: %s 块数 %d != 期望 %d\n", tag, n, expectN);
        g_fail++;
        return n;
    }
    for (int i = 0; i < n; i++) {
        if (!(q[i].w > 0.0f) || !(q[i].h > 0.0f)) {
            std::printf("FAIL: %s 第 %d 块宽或高不为正 (%.2f x %.2f)\n", tag, i, q[i].w, q[i].h);
            g_fail++;
        }
        if (!(q[i].u0 >= -1e-6f) || !(q[i].u1 <= 1.0f + 1e-6f) ||
            !(q[i].v0 >= -1e-6f) || !(q[i].v1 <= 1.0f + 1e-6f)) {
            std::printf("FAIL: %s 第 %d 块 UV 出界\n", tag, i);
            g_fail++;
        }
    }
    const float area = SumArea(q, n);
    CHECK_NEAR(area, w * h, 1e-3f, tag);
    return n;
}

int run() {
    // ---------- 模板尺寸 ----------
    CHECK(RoundTemplateSize(1) == 8, "r=1 时模板至少 8px");
    CHECK(RoundTemplateSize(50) == 104, "r=50 时模板 = 2r+4 = 104");
    CHECK(RoundTemplateSize(200) < 4 * 200, "模板不再按 4r 走（省显存）");
    for (int r = 1; r <= 300; r++)
        CHECK(RoundTemplateSize(r) >= 2 * r + 4, "模板边长必须 >= 2r+4（中段要有直边可采）");

    // ---------- 常规圆角：九块 ----------
    CheckTiling(200.0f, 120.0f, 12.0f, 9, "200x120 r12 应九块铺满");
    // h=120、r=60：纵向中段为 0（左右两块与中心消失），只剩四角 + 上下两条
    CheckTiling(200.0f, 120.0f, 60.0f, 6, "200x120 r60（=短边一半）纵向中段应消失");

    // ---------- 正圆：只有四个 1/4 圆 ----------
    {
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(10.0f, 20.0f, 100.0f, 100.0f, 50.0f,
                                            RoundTemplateSize(50), q, 9);
        CHECK(n == 4, "100x100 r50 应只剩四块（中段宽高为 0）");
        CHECK_NEAR(SumArea(q, n), 10000.0f, 1e-3f, "正圆四块面积之和 = 10000");
        // 四角位置必须贴着盒子四角，边长为半径 r
        CHECK_NEAR(q[0].x, 10.0f, 1e-6f, "左上块 x");
        CHECK_NEAR(q[0].y, 20.0f, 1e-6f, "左上块 y");
        CHECK_NEAR(q[0].w, 50.0f, 1e-6f, "角片边长 = 半径");
        CHECK_NEAR(q[3].x + q[3].w, 110.0f, 1e-6f, "右下块右边界贴盒子右边");
        CHECK_NEAR(q[3].y + q[3].h, 120.0f, 1e-6f, "右下块下边界贴盒子下边");
        // 四个角互不重叠：左上块的右边界 == 右上块的左边界
        CHECK_NEAR(q[0].x + q[0].w, q[1].x, 1e-6f, "左右两块在 x 方向首尾相接");
        const float cu = 50.0f / static_cast<float>(RoundTemplateSize(50));
        CHECK_NEAR(q[0].u1 - q[0].u0, cu, 1e-6f, "角片 UV 跨度 = r/t");
    }

    // ---------- 胶囊：横向伸直，纵向中段为 0 ----------
    {
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(0.0f, 0.0f, 200.0f, 40.0f, 20.0f,
                                            RoundTemplateSize(20), q, 9);
        CHECK(n == 6, "200x40 r20 胶囊应为 6 块（上下两条中段消失）");
        CHECK_NEAR(SumArea(q, n), 8000.0f, 1e-3f, "胶囊面积之和 = 200x40");
    }

    // ---------- 半径超过短边一半：钳到一半，不产生负尺寸 ----------
    {
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(0.0f, 0.0f, 34.0f, 34.0f, 999.0f,
                                            RoundTemplateSize(999), q, 9);
        CHECK(n == 4, "34x34 r999 应钳成 r17 的正圆（四块）");
        CHECK_NEAR(SumArea(q, n), 34.0f * 34.0f, 1e-3f, "钳制后面积不变");
    }

    // ---------- 退化输入 ----------
    {
        PaintQuad q[9];
        CHECK(BuildRoundedNineSlice(0, 0, 100, 100, 0.0f, 16, q, 9) == 0, "r=0 不产生块");
        CHECK(BuildRoundedNineSlice(0, 0, 0, 100, 8.0f, 16, q, 9) == 0, "w=0 不产生块");
        CHECK(BuildRoundedNineSlice(0, 0, 100, 100, 8.0f, 16, nullptr, 9) == 0, "out 为空不崩");
    }

    // ---------- 文本尺寸粗估 ----------
    {
        // 13px 中文：4 个字 ≈ 52px，300px 宽 → 1 行
        CHECK(EstimateLineCount("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97\xE5\xBD\xA2", 13.0f, 300.0f) == 1,
              "4 个汉字在 300px 里是一行");
        // 拉丁 0.52×：13*0.52=6.76，55 个字符 ≈ 372px → 需要 2 行
        const std::string lat(55, 'a');
        CHECK(EstimateLineCount(lat, 13.0f, 300.0f) == 2, "55 个拉丁字符在 300px 里是两行");
        // 硬换行
        CHECK(EstimateLineCount("a\nb\nc", 13.0f, 300.0f) == 3, "两个 \\n 断成三行");
        // 行距：默认 1.35×，显式 200% 翻倍
        CHECK_NEAR(EstimateLineStep(20.0f, 0), 27.0f, 1e-4f, "默认行距 1.35×");
        CHECK_NEAR(EstimateLineStep(20.0f, 200), 40.0f, 1e-4f, "line-height:200% = 2×字号");
        CHECK_NEAR(EstimateTextHeight("a\nb", 20.0f, 300.0f, 0), 54.0f, 1e-4f, "两行 = 2×27");
        // 极窄可用宽度不能除零/死循环
        CHECK(EstimateLineCount("\xE4\xB8\xAD\xE6\x96\x87", 13.0f, 0.0f) >= 1, "可用宽度为 0 时仍返回正行数");
    }

    // ---------- 边框条带（v13 四边边框） ----------
    {
        BinUIPaintNode p{};
        p.flags = PAINT_BORDER;
        for (int i = 0; i < 4; i++) { p.borderW[i] = 1; p.borderColor[i] = 0xFFE6EDF7u; }

        PaintBand bb[kMaxBorderBands];
        const float W = 400.0f, H = 600.0f, R = 16.0f;

        // 高盒子的边框必须一路铺到底：以前逐行 push 左右交替、只看 out[n-1] 合并不了，
        // 条带数变成 O(高度)，撞上 cap 就在盒子顶部约 64px 处截断（下面整段没边框）。
        const int n = BuildBorderBands(&p, 0, 0, W, H, R, bb);
        CHECK(n > 0 && n < kMaxBorderBands, "600px 高卡片边框不撞条带上限");

        float maxBottom = 0.0f, leftStraight = 0.0f;
        for (int i = 0; i < n; i++) {
            maxBottom = std::max(maxBottom, bb[i].y + bb[i].h);
            if (bb[i].x <= 0.51f && bb[i].w <= 1.51f) leftStraight += bb[i].h;  // 直边段左边框
        }
        CHECK_NEAR(maxBottom, H, 1.01f, "边框必须画到盒子底边（不能中途截断）");
        CHECK(leftStraight >= H - 2.0f * R - 1.0f, "左边框直边段要覆盖 H-2r");

        // 条带数只跟半径有关：把盒子拉高一倍，条带数不该跟着涨
        const int n2 = BuildBorderBands(&p, 0, 0, W, H * 2.0f, R, bb);
        CHECK(n2 == n, "条带数与盒子高度无关");

        // 直角边框 = 四条
        BinUIPaintNode sq{};
        sq.flags = PAINT_BORDER;
        for (int i = 0; i < 4; i++) { sq.borderW[i] = 2; sq.borderColor[i] = 0xFF000000u; }
        CHECK(BuildBorderBands(&sq, 0, 0, 100, 40, 0.0f, bb) == 4, "直角边框 = 四条");

        // 半径大到装不下（cap 小）：退回直角四条，宁可方角也不能缺边。
        // 这里显式给个小 cap（不是 kMaxPaintBands）—— 后者会随填充条带上限一起调大，
        // 拿它当"装不下"的输入，这条断言就会随常量变化而失效。
        CHECK(BuildBorderBands(&p, 0, 0, 200, 200, 100.0f, bb, 32) == 4,
              "半径超过条带上限时退回直角四条");
    }

    if (g_fail == 0) std::printf("test_paint: 全部通过\n");
    return g_fail;
}

}  // namespace suite_paint

// ---------------------------------------------------------------------------
// 原 tests/test_anim.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_anim {
// test_anim.cpp - v7 动画引擎测试：@keyframes 关键帧动画 + transition 过渡 + 运行时样式 API

using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

// 轮询直到条件满足或超时
template <typename F>
static bool WaitFor(F f, int timeoutMs) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int run() {
    const char* html = R"HTML(<!DOCTYPE html>
<html width="400" height="300" bg="#101014">
<head><style>
@keyframes fadeUp {
    from { opacity: 0; transform: translateY(20px); }
    to   { opacity: 1; transform: translateY(0); }
}
@keyframes pulse {
    0%, 100% { transform: scale(1); }
    50%      { transform: scale(1.08); }
}
button { transition: background-color 0.3s ease, transform 0.2s ease; }
</style></head>
<body>
    <div animation="fadeUp 0.5s ease 0.15s 1 normal both"
         style="position:absolute; left:20; top:40; width:200; height:80; background:#16161D; border-radius:10">Card</div>
    <button animation="pulse 1s linear 0s infinite"
            style="position:absolute; left:20; top:140; width:120; height:44; background:#8B5CF6; border-radius:8">Btn</button>
    <button style="position:absolute; left:160; top:140; width:120; height:44; background:#03DAC6; border-radius:8; transition:opacity 0.4s ease">Btn2</button>
</body>
</html>)HTML";

    auto res = CompileBinUIEx(html);
    CHECK(!res.layout.empty(), "compile");
    BinUIRenderer r;
    CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "loadLayout");

    int cardId = -1, btnId = -1, btn2Id = -1;
    for (const auto& n : r.getNodes()) {
        if (n.type == BINUI_LIST) cardId = n.id;
        else if (n.type == BINUI_BUTTON && btnId < 0) btnId = n.id;
        else if (n.type == BINUI_BUTTON) btn2Id = n.id;
    }
    CHECK(cardId >= 0 && btnId >= 0 && btn2Id >= 0, "ids");

    // 关键帧：加载时刻应处于 delay 阶段（backwards fill → opacity 0，translateY 20）
    CHECK(r.drawOpacity((uint16_t)cardId) < 0.05f, "keyframe initial opacity");
    CHECK(r.drawOffsetY((uint16_t)cardId) > 19.0f, "keyframe initial translateY");

    // 动画推进：opacity 最终达到 1 并保持（fill forwards）
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawOpacity((uint16_t)cardId) > 0.95f; }, 2000),
          "keyframe reaches end");
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawOffsetY((uint16_t)cardId) < 1.0f; }, 2000),
          "keyframe translate returns to 0");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    r.stepAnimations();
    CHECK(r.drawOpacity((uint16_t)cardId) > 0.95f, "keyframe fill forwards holds");

    // 无限循环动画（pulse）：scale 应周期变化
    bool sawNot1 = false;
    for (int i = 0; i < 12; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        r.stepAnimations();
        float s = r.drawScale((uint16_t)btnId);
        if (s > 1.02f) sawNot1 = true;
    }
    CHECK(sawNot1, "infinite keyframe animates scale");

    // hover 过渡：进入按钮 → 背景色/缩放平滑过渡到高亮
    uint32_t c0 = r.drawColor((uint16_t)btnId);
    r.setHover(70, 170); // 命中 btn
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawScale((uint16_t)btnId) > 1.03f; }, 1000),
          "hover scale transitions in");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.stepAnimations();
    uint32_t c1 = r.drawColor((uint16_t)btnId);
    CHECK(c1 != c0, "hover color transitions in");

    // 离开 → 恢复
    auto nearColor = [](uint32_t a, uint32_t b, int eps) {
        int ra = (int)((a >> 16) & 0xFF), ga = (int)((a >> 8) & 0xFF), ba = (int)(a & 0xFF);
        int rb = (int)((b >> 16) & 0xFF), gb = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
        return std::abs(ra - rb) <= eps && std::abs(ga - gb) <= eps && std::abs(ba - bb) <= eps;
    };
    r.setHover(300, 170);
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawScale((uint16_t)btnId) < 1.01f; }, 1000),
          "hover scale transitions out");
    CHECK(WaitFor([&] { r.stepAnimations(); return nearColor(r.drawColor((uint16_t)btnId), c0, 4); }, 1000),
          "hover color restores");

    // v23：动画事件（animationend / animationiteration / transitionend）—— 宿主不用自己按
    // durationMs 定时，也不用猜"这一帧到底有没有结束"。
    {
        // hover 的 transform 0.2s / background-color 0.3s 要走完才会报 transitionend
        std::this_thread::sleep_for(std::chrono::milliseconds(360));
        r.stepAnimations();
        // 事件是"攒到取的时候"，所以两种结束（关键帧 / 过渡）都在同一批里
        bool cardEnd = false, infEnded = false, scaleEnd = false, colorEnd = false;
        uint32_t infIters = 0;
        for (const auto& e : r.takeAnimEvents()) {
            if (e.kind == BinUIRenderer::AnimEventKind::ANIM_END &&
                e.nodeId == (uint16_t)cardId && e.iteration == 1)
                cardEnd = true;
            if (e.nodeId == (uint16_t)btnId) {
                if (e.kind == BinUIRenderer::AnimEventKind::ANIM_END) infEnded = true;
                if (e.kind == BinUIRenderer::AnimEventKind::ANIM_ITERATION) infIters++;
                if (e.kind == BinUIRenderer::AnimEventKind::TRANSITION_END) {
                    if (e.prop == ANIM_SCALE) scaleEnd = true;
                    if (e.prop == ANIM_BG_COLOR) colorEnd = true;
                }
            }
        }
        CHECK(cardEnd, "animationend for the 1-iteration keyframe");
        CHECK(!infEnded, "infinite animation never reports animationend");
        CHECK(infIters >= 1, "infinite animation reports animationiteration");
        CHECK(scaleEnd, "transitionend for hover scale");
        CHECK(colorEnd, "transitionend for hover background-color");

        // 只报一次：再来一帧不能再冒一条 animationend
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        r.stepAnimations();
        bool dup = false;
        for (const auto& e : r.takeAnimEvents())
            if (e.kind == BinUIRenderer::AnimEventKind::ANIM_END && e.nodeId == (uint16_t)cardId)
                dup = true;
        CHECK(!dup, "animationend fires once");
    }

    // 运行时样式 API：setStyleFloat 触发 opacity 过渡
    r.setStyleFloat((uint16_t)btn2Id, ANIM_OPACITY, 0.5f);
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawOpacity((uint16_t)btn2Id) < 0.9f; }, 2000),
          "style opacity transition starts");
    CHECK(WaitFor([&] { r.stepAnimations(); float o = r.drawOpacity((uint16_t)btn2Id); return o > 0.42f && o < 0.58f; }, 2000),
          "style opacity transition settles");

    if (g_fail == 0) std::printf("=== Results: all animation tests passed ===\n");
    else std::printf("=== Results: %d FAILED ===\n", g_fail);
    return g_fail;
}

}  // namespace suite_anim

// ---------------------------------------------------------------------------
// 原 tests/test_hot_reload.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_hot_reload {
// test_hot_reload.cpp - UI 路径解析 + 热重载回归测试（无窗口，纯逻辑）
//
// 覆盖：
//   1. exe 真实路径解析（不再依赖 argv[0]）
//   2. ResolveUI 查找顺序（--ui 优先 / 源码树兜底）
//   3. 保存即重载：mtime 变化 -> 稳定 250ms 后重新编译 -> 换树成功
//   4. 换树后状态恢复（滚动偏移 / 键盘焦点）
//   5. 坏 UI / 文件消失时不把界面刷空、不误触发
//
// 为避免污染仓库，所有写操作发生在临时副本上（build-mingw/tmp/）。


using namespace binui;

static int g_fail = 0;

#define CHECK(cond, msg)                                \
    do {                                                \
        if (!(cond)) {                                  \
            std::fprintf(stderr, "[FAIL] %s\n", (msg)); \
            ++g_fail;                                   \
        } else {                                        \
            std::printf("[ok] %s\n", (msg));            \
        }                                               \
    } while (0)

static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool WriteAll(const std::string& path, const std::string& body) {
    SleepMs(20); // 让 mtime 至少跨过一个时钟粒度
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
    return o.good();
}

// 反复 Tick 直到重载或超时
static bool PumpUntilReload(ex::HotReload& hot, BinUIRenderer& r, int maxMs) {
    for (int waited = 0; waited < maxMs; waited += 30) {
        if (hot.Tick(r, "svgdemo", 0, 0) == ex::HotReload::Result::Reloaded) return true;
        SleepMs(30);
    }
    return false;
}

int run() {
    std::error_code ec;
    std::filesystem::create_directories("build-mingw/tmp", ec);

    // ---------- 1. exe 真实路径（本次修复的核心） ----------
    const std::string exePath = ex::ExePath();
    std::printf("[info] ExePath() = \"%s\"\n", exePath.c_str());
    CHECK(!exePath.empty(), "ExePath() 非空（不依赖 argv[0]）");
    CHECK(ex::FileExists(exePath), "ExePath() 指向真实存在的文件");
    CHECK(!ex::ExeDir().empty(), "ExeDir() 非空");

    // ---------- 2. UI 定位顺序 ----------
    ex::LaunchArgs none;
    const std::string repoUI = ex::ResolveUI(none, "svgdemo", "main.html");
    std::printf("[info] ResolveUI(svgdemo/main.html) = \"%s\"\n", repoUI.c_str());
    CHECK(ex::FileExists(repoUI), "ResolveUI 返回的 UI 文件真实存在");
    CHECK(repoUI.find("svgdemo") != std::string::npos &&
              repoUI.find("main.html") != std::string::npos,
          "ResolveUI 命中 svgdemo 的 main.html（exe 同级或源码树）");

    // 只在源码树里存在的文件 -> 必须走第 6 级兜底（证明编译目录外也能跑）
    // 用 main_video_backup.html：它不会被 copy 到 build-mingw/bin/，只能从源码树找到
    const std::string repoOnly = ex::ResolveUI(none, "features", "NOTES.md");
    std::printf("[info] ResolveUI(features/NOTES.md) = \"%s\"\n", repoOnly.c_str());
    CHECK(repoOnly == "examples/features/NOTES.md", "源码树兜底命中 examples/<app>/<file>");

    CHECK(ex::ResolveUI(none, "svgdemo", "no_such_file.html").empty(),
          "文件不存在时返回空（调用方回退默认布局）");

    ex::LaunchArgs withUi;
    withUi.ui = "examples/animshow/main.html";
    CHECK(ex::ResolveUI(withUi, "svgdemo", "main.html") == "examples/animshow/main.html",
          "--ui 显式指定优先于默认查找");

    // ---------- 3. 首次编译 + 加载 ----------
    const CompileResult initial = CompileBinUIFileEx(repoUI);
    CHECK(!initial.layout.empty(), "示例 HTML 编译成功");
    BinUIRenderer renderer;
    CHECK(renderer.loadLayout(initial.layout.data(), initial.layout.size()), "布局解析成功");
    const size_t nodes0 = renderer.getNodes().size();
    CHECK(nodes0 > 0, "节点树非空");

    // ---------- 4. 临时副本上验证"保存即重载" ----------
    const std::string tmpPath = "build-mingw/tmp/hot_ui.html";
    const std::string html = ReadAll(repoUI);
    CHECK(!html.empty(), "读到示例 HTML");
    CHECK(WriteAll(tmpPath, html), "临时副本写入成功");

    ex::HotReload hot;
    hot.Init(true, tmpPath);
    CHECK(hot.enabled(), "--watch 启用");

    int early = 0;
    for (int i = 0; i < 20; ++i) {
        if (hot.Tick(renderer, "svgdemo", 0, 0) == ex::HotReload::Result::Reloaded) ++early;
        SleepMs(30);
    }
    CHECK(early == 0, "文件未改动时不触发重载");

    CHECK(WriteAll(tmpPath, html), "模拟保存（重写同一份内容）");
    CHECK(PumpUntilReload(hot, renderer, 3000), "保存后自动重载");
    CHECK(hot.reloads() == 1, "重载计数 == 1");
    CHECK(renderer.getNodes().size() == nodes0, "重载后节点数一致");

    // ---------- 5. 状态恢复：滚动偏移 / 键盘焦点 ----------
    uint16_t scrollNode = 0, focusNode = 0;
    for (const auto& n : renderer.getNodes()) {
        if (!scrollNode && renderer.isScrollable(n.id)) scrollNode = n.id;
        if (!focusNode && (n.flags & BINUI_INTERACTIVE)) focusNode = (uint16_t)n.id;
    }

    float expectedScrollY = 0.0f;
    int expectedFocus = -1;
    if (scrollNode) {
        renderer.scrollTo(scrollNode, 0.0f, 17.0f);
        const auto* st = renderer.scrollState(scrollNode);
        expectedScrollY = st ? st->y : 0.0f;
        std::printf("[info] 滚动容器 %u：requested=17.0 clamped=%.1f\n", scrollNode, expectedScrollY);
    }
    if (focusNode) {
        renderer.setKeyboardFocus(focusNode);
        expectedFocus = renderer.keyboardNode();
        std::printf("[info] 键盘焦点节点 %u -> %d\n", focusNode, expectedFocus);
    }

    CHECK(WriteAll(tmpPath, html), "再次模拟保存");
    CHECK(PumpUntilReload(hot, renderer, 3000), "第二次重载成功");

    if (scrollNode) {
        const auto* st = renderer.scrollState(scrollNode);
        char msg[192];
        std::snprintf(msg, sizeof(msg), "滚动偏移恢复（节点 %u：期望 %.1f，实际 %.1f）",
                      scrollNode, expectedScrollY, st ? st->y : -1.0f);
        CHECK(st && st->y == expectedScrollY, msg);
    } else {
        std::printf("[skip] 示例 UI 无滚动容器，跳过滚动恢复\n");
    }

    if (focusNode) {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "键盘焦点恢复（期望 %d，实际 %d）",
                      expectedFocus, renderer.keyboardNode());
        CHECK(renderer.keyboardNode() == expectedFocus, msg);
    } else {
        std::printf("[skip] 示例 UI 无可交互节点，跳过焦点恢复\n");
    }

    // ---------- 5b. 滚动恢复：用合成 UI 保证一定溢出 ----------
    {
        std::string html2 =
            "<html><body><div id=\"sc\" style=\"position:absolute;left:0;top:0;"
            "width:300;height:120;overflow:auto\">";
        for (int i = 0; i < 40; ++i)
            html2 += "<p style=\"height:24\">row " + std::to_string(i) + "</p>";
        html2 += "</div></body></html>";

        const std::string scrollPath = "build-mingw/tmp/hot_scroll.html";
        CHECK(WriteAll(scrollPath, html2), "合成滚动 UI 写入成功");

        BinUIRenderer r2;
        const CompileResult cr = CompileBinUIFileEx(scrollPath);
        CHECK(!cr.layout.empty() && r2.loadLayout(cr.layout.data(), cr.layout.size()),
              "合成滚动 UI 编译并加载");
        const uint16_t sc = r2.findNodeByName("sc");
        CHECK(sc != 0 && r2.isScrollable(sc), "找到滚动容器 #sc");
        const float contentH = sc ? r2.contentHeight(sc) : 0.0f;
        std::printf("[info] #sc contentHeight=%.1f\n", contentH);
        CHECK(contentH > 120.0f, "合成 UI 内容确实溢出（可滚动）");

        r2.scrollTo(sc, 0.0f, 300.0f);
        const auto* st0 = r2.scrollState(sc);
        const float wantY = st0 ? st0->y : -1.0f;
        std::printf("[info] #sc requested=300.0 clamped=%.1f\n", wantY);
        CHECK(wantY > 0.0f, "滚动偏移已生效（未被钳制为 0）");

        ex::HotReload hot2;
        hot2.Init(true, scrollPath);
        CHECK(WriteAll(scrollPath, html2), "模拟保存滚动 UI");
        CHECK(PumpUntilReload(hot2, r2, 3000), "滚动 UI 重载成功");
        const auto* st1 = r2.scrollState(sc);
        char msg[192];
        std::snprintf(msg, sizeof(msg), "滚动偏移跨重载保留（期望 %.1f，实际 %.1f）",
                      wantY, st1 ? st1->y : -1.0f);
        CHECK(st1 && st1->y == wantY, msg);
    }

    // ---------- 6. 坏 UI：不允许把界面刷空 ----------
    CHECK(WriteAll(tmpPath, "<div><<<< 垃圾 & 未闭合 <span>"), "写入一份坏 UI");
    bool reloaded = false;
    for (int i = 0; i < 60 && !reloaded; ++i) {
        const auto res = hot.Tick(renderer, "svgdemo", 0, 0);
        if (res != ex::HotReload::Result::Idle) reloaded = true;
        SleepMs(30);
    }
    std::printf("[info] 坏 UI 处理：%s；当前节点数 %zu\n",
                reloaded ? "已处理（编译容错则重载，失败则保留旧树）" : "未触发",
                renderer.getNodes().size());
    CHECK(renderer.getNodes().size() > 0, "坏 UI 后节点树没有变空（仍可渲染）");

    // ---------- 7. 文件消失：不误触发 ----------
    std::remove(tmpPath.c_str());
    ex::HotReload gone;
    gone.Init(true, "build-mingw/tmp/does_not_exist.html");
    int bogus = 0;
    for (int i = 0; i < 20; ++i) {
        if (gone.Tick(renderer, "svgdemo", 0, 0) != ex::HotReload::Result::Idle) ++bogus;
        SleepMs(10);
    }
    CHECK(bogus == 0, "文件不存在时 20 次 Tick 全 Idle");

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

}  // namespace suite_hot_reload


// ---------------------------------------------------------------------------
// 内置悬停放大 -> 缩回：失效矩形必须仍覆盖"放大时画过的范围"
//
// 按钮没有 :hover 样式时，引擎给的内置悬停效果是 1.05 倍 + 12% 提亮，而且**没有过渡**：
// 缩放一帧跳上去、一帧跳回来。缩回那一帧如果只按"当前（=原尺寸）"算失效矩形，
// 放大时在最外沿画过的像素就没人擦 —— 表现为按钮右边缘留一道 1px 残影。
// 用 200px 宽的按钮把边界撑到 pad（2px）之外，别让 pad 把问题盖住。
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_hover_dirty {

using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_fail; } } while (0)

static int run() {
    const char* html = R"HTML(<html width="600" height="300" bg="#FFFFFF"><body>
      <button id="wide" style="position:absolute;left:100;top:100;width:200;height:40;background:#3E8BFF">宽按钮</button>
    </body></html>)HTML";
    auto res = CompileBinUIEx(html);
    BinUIRenderer r;
    CHECK(!res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()),
          "hover-dirty: 布局编译 / 加载");
    int id = -1;
    for (const auto& n : r.getNodes())
        if (n.type == BINUI_BUTTON) id = n.id;
    CHECK(id >= 0, "hover-dirty: 找到按钮");
    if (id < 0) return g_fail;

    float x = 0, y = 0, w = 0, h = 0;
    r.invalidRect(static_cast<uint16_t>(id), x, y, w, h);
    CHECK(w <= 204.5f, "hover-dirty: 静止时失效矩形 = 节点 + pad，不该凭空变大");

    r.setHover(110, 120);   // 悬停在按钮上
    r.stepAnimations();
    CHECK(r.drawScale(static_cast<uint16_t>(id)) > 1.04f, "hover-dirty: 内置悬停放大生效");
    float x2 = 0, y2 = 0, w2 = 0, h2 = 0;
    r.invalidRect(static_cast<uint16_t>(id), x2, y2, w2, h2);
    CHECK(w2 >= 210.0f, "hover-dirty: 放大后失效矩形覆盖放大范围");

    r.clearHover();         // 移开 -> 立即缩回
    r.stepAnimations();
    CHECK(r.drawScale(static_cast<uint16_t>(id)) < 1.001f, "hover-dirty: 移开后回到原尺寸");
    float x3 = 0, y3 = 0, w3 = 0, h3 = 0;
    r.invalidRect(static_cast<uint16_t>(id), x3, y3, w3, h3);
    CHECK(w3 >= 210.0f,
          "hover-dirty: 缩回那一帧的失效矩形仍覆盖放大过的范围（否则边缘留残影）");
    return g_fail;
}

#undef CHECK

}  // namespace suite_hover_dirty

// ---------------------------------------------------------------------------
// 裁剪链：只有 overflow:auto/scroll/hidden 的祖先才裁子元素
//
// 以前三个后端都无条件按"每个祖先矩形"裁剪。组件若写了一层与按钮等大的根 div
// （早期 features 的 button.bum.html 就是这样），按钮 hover 放大 1.04 时会被切回
// 原来那块 —— 看起来像"按钮被困在盒子里放大"。
// CSS 语义里普通 div 不裁子元素，要裁得靠 overflow。这条用例把判定口径钉在渲染器上
// （三个后端都调 clippingAncestors，同一份规则）。
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_clip_chain {

using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_fail; } } while (0)

static int run() {
    const char* html = R"HTML(<html width="600" height="400" bg="#FFFFFF"><body>
      <div style="position:absolute;left:0;top:0;width:400;height:300">
        <div style="position:absolute;left:20;top:20;width:300;height:200;overflow:hidden">
          <button id="inner" style="position:absolute;left:20;top:20;width:120;height:40">裁剪容器里的按钮</button>
        </div>
      </div>
      <button id="loose" style="position:absolute;left:20;top:340;width:120;height:40">普通祖先</button>
    </body></html>)HTML";
    auto res = CompileBinUIEx(html);
    BinUIRenderer r;
    CHECK(!res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()),
          "clip-chain: 布局编译 / 加载");

    uint16_t inner = 0, loose = 0;
    for (const auto& n : r.getNodes()) {
        if (n.text == "裁剪容器里的按钮") inner = n.id;
        else if (n.text == "普通祖先") loose = n.id;
    }
    CHECK(inner != 0 && loose != 0, "clip-chain: 找到两个按钮");

    std::vector<uint16_t> chain;
    r.clippingAncestors(inner, chain);
    CHECK(chain.size() == 1, "clip-chain: overflow:hidden 的祖先进裁剪链（且只有它）");

    r.clippingAncestors(loose, chain);
    CHECK(chain.empty(), "clip-chain: 普通 div 祖先不裁剪子元素（hover 放大不该被切）");
    return g_fail;
}

#undef CHECK

}  // namespace suite_clip_chain

// ---------------------------------------------------------------------------
// suite_svgpath：SVG 路径几何（include/binui_path.h）+ 扫描线填充（binui_paint.h）
//
// 这两块都是"编译期与运行期共用的一份数学"，和 suite_paint 同一个思路：
// 纯 CPU 可验证，不需要窗口/后端。重点：
//   · `d` 的语法边界（隐式重复、紧贴负数、隐式小数点、弧标志紧贴）
//   · 细分质量（弧离真圆的最大偏差）与容差的方向性（容差越大点越少）
//   · 扫描线的正确性用"面积"和"采样点"两个独立口径互相印证
//   · 条带必须合并（否则一个实心图标就是每行一个 draw call）
//   · 撞上限时必须 truncated（调用方降级重试），而不是画一半
// ---------------------------------------------------------------------------
namespace suite_svgpath {

using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECK_NEAR(a, b, eps, msg) \
    do { if (std::fabs((a) - (b)) > (eps)) { \
        std::printf("FAIL: %s (%.4f vs %.4f)\n", msg, (double)(a), (double)(b)); g_fail++; } } while (0)

// PathShape → 设备坐标的 PaintPath（点直接拷贝到本地缓冲，调用方持有）
struct DevicePath {
    std::vector<float> pts;
    std::vector<uint32_t> first, count;
    std::vector<uint8_t> closed;
    PaintPath view() const {
        return PaintPath{ pts.data(), first.data(), count.data(), closed.data(),
                          static_cast<int>(first.size()) };
    }
};

static DevicePath ToDevice(const PathShape& s, float dx = 0.0f, float dy = 0.0f) {
    DevicePath d;
    for (const PathShape::Contour& c : s.contours) {
        d.first.push_back(c.first);
        d.count.push_back(c.count);
        d.closed.push_back(c.closed ? 1u : 0u);
    }
    for (const PathPointF& p : s.pts) {
        d.pts.push_back(p.x + dx);
        d.pts.push_back(p.y + dy);
    }
    return d;
}

static bool BandCovers(const PaintBand* bands, int n, float px, float py) {
    for (int i = 0; i < n; i++)
        if (px >= bands[i].x && px < bands[i].x + bands[i].w &&
            py >= bands[i].y && py < bands[i].y + bands[i].h) return true;
    return false;
}

static double BandArea(const PaintBand* bands, int n) {
    double a = 0;
    for (int i = 0; i < n; i++) a += static_cast<double>(bands[i].w) * bands[i].h;
    return a;
}

static void TestParse() {
    PathShape s;
    std::string err;

    // 绝对坐标 + 一条命令后跟多组坐标（隐式 lineto）
    CHECK(ParsePathData("M0 0 L10 0 10 10 0 10 Z", 0.1f, s, &err), "解析：绝对 + 隐式 lineto");
    CHECK(s.pts.size() == 4, "解析：矩形 4 个点");
    CHECK(s.contours.size() == 1 && s.contours[0].closed, "解析：一条闭合子路径");
    CHECK_NEAR(s.maxX, 10, 1e-3f, "解析：包围盒右边界");
    CHECK_NEAR(s.maxY, 10, 1e-3f, "解析：包围盒下边界");

    // 相对命令 + 紧贴负数（"l5-3" 合法）
    CHECK(ParsePathData("m10,10 l5-3", 0.1f, s, &err), "解析：相对命令 + 紧贴负数");
    CHECK_NEAR(s.pts.back().x, 15, 1e-3f, "解析：相对命令终点 x");
    CHECK_NEAR(s.pts.back().y, 7, 1e-3f, "解析：相对命令终点 y");

    // 隐式小数点（"1.5.5" = 1.5, 0.5）
    CHECK(ParsePathData("M1.5.5 L3 3", 0.1f, s, &err), "解析：隐式小数点");
    CHECK_NEAR(s.pts[0].x, 1.5f, 1e-3f, "解析：1.5.5 → x=1.5");
    CHECK_NEAR(s.pts[0].y, 0.5f, 1e-3f, "解析：1.5.5 → y=0.5");

    // H/V
    CHECK(ParsePathData("M0 0 H10 V10 H0 Z", 0.1f, s, &err), "解析：H/V");
    CHECK_NEAR(s.maxX, 10, 1e-3f, "解析：H 生效");
    CHECK_NEAR(s.maxY, 10, 1e-3f, "解析：V 生效");

    // 三次 + S（反射控制点）、二次 + T
    CHECK(ParsePathData("M0 0 C0 10 10 10 10 0 S0 -10 0 0", 0.05f, s, &err), "解析：C + S");
    CHECK_NEAR(s.pts.back().x, 0, 1e-3f, "解析：S 终点 x");
    CHECK_NEAR(s.pts.back().y, 0, 1e-3f, "解析：S 终点 y");
    CHECK(ParsePathData("M0 0 Q5 5 10 0 T20 0", 0.05f, s, &err), "解析：Q + T");
    CHECK_NEAR(s.pts.back().x, 20, 1e-3f, "解析：T 终点 x");
    CHECK_NEAR(s.pts.back().y, 0, 1e-3f, "解析：T 终点 y");

    // 弧：四分之一圆，圆心 (0,10) 半径 10 —— 每个细分点都该落在圆上
    CHECK(ParsePathData("M0 0 A10 10 0 0 1 10 10", 0.05f, s, &err), "解析：弧");
    CHECK_NEAR(s.pts.back().x, 10, 1e-3f, "解析：弧终点 x");
    CHECK_NEAR(s.pts.back().y, 10, 1e-3f, "解析：弧终点 y");
    {
        float worst = 0;
        for (const PathPointF& p : s.pts)
            worst = std::max(worst, std::fabs(std::sqrt(p.x * p.x + (p.y - 10) * (p.y - 10)) - 10.0f));
        std::printf("  [svgpath] 弧离真圆最大偏差 %.4f px（容差 0.05）\n", worst);
        CHECK(worst < 0.05f, "解析：弧的细分精度（< 容差）");
    }

    // 多子路径 + 填充规则
    CHECK(ParsePathData("M0 0 H100 V100 H0 Z M25 25 H75 V75 H25 Z", 0.01f, s, &err),
          "解析：两条子路径（带洞）");
    CHECK(s.contours.size() == 2, "解析：子路径数 2");
    CHECK(!PointInPath(s, 50, 50, true), "填充规则：evenodd 挖掉内洞");
    CHECK(PointInPath(s, 50, 50, false), "填充规则：nonzero 同向时内洞仍实心");
    CHECK(PointInPath(s, 8, 50, true), "填充规则：环上算实心（射线穿过 3 条边）");

    // 椭圆（走同一套细分容差）
    {
        PathShape e;
        AppendEllipse(e, 0, 0, 20, 10, 0.05f);
        CHECK(e.contours.size() == 1 && e.contours[0].closed, "椭圆：闭合子路径");
        CHECK_NEAR(e.minX, -20, 0.02f, "椭圆：左边界");
        CHECK_NEAR(e.maxX, 20, 0.02f, "椭圆：右边界");
        CHECK_NEAR(e.minY, -10, 0.02f, "椭圆：上边界");
        CHECK_NEAR(e.maxY, 10, 0.02f, "椭圆：下边界");
        float worst = 0;
        for (const PathPointF& p : e.pts)
            worst = std::max(worst, std::fabs((p.x * p.x) / 400.0f + (p.y * p.y) / 100.0f - 1.0f));
        CHECK(worst < 0.01f, "椭圆：细分精度");
    }

    // 变换（viewBox / <g transform> 都走这一条）
    {
        PathShape t;
        ParsePathData("M0 0 L10 0", 0.1f, t, &err);
        TransformShape(t, PathRotate(90.0f));
        CHECK_NEAR(t.pts.back().x, 0, 0.01f, "变换：rotate 90 → x");
        CHECK_NEAR(t.pts.back().y, 10, 0.01f, "变换：rotate 90 → y");
        TransformShape(t, PathTranslate(5, 5));
        CHECK_NEAR(t.pts.back().x, 5, 0.01f, "变换：translate 可叠加");
        CHECK_NEAR(t.pts.back().y, 15, 0.01f, "变换：translate 可叠加（y）");
        PathShape v;
        ParsePathData("M0 0 L1 1", 0.1f, v, &err);
        TransformShape(v, PathMul(PathScale(2, 3), PathTranslate(1, 1)));
        CHECK_NEAR(v.pts.back().x, 4, 0.01f, "变换：先平移再缩放（x）");
        CHECK_NEAR(v.pts.back().y, 6, 0.01f, "变换：先平移再缩放（y）");
    }

    // 归一化往返（v15 路径段的存储形态）
    {
        PathShape n;
        ParsePathData("M0 0 C3 0 7 10 10 10", 0.05f, n, &err);
        PathNormData nd;
        CHECK(NormalizeShape(n, nd), "归一化：成功");
        CHECK(nd.pts.size() == n.pts.size() * 2, "归一化：点数一致");
        CHECK(nd.pts[0] == 0 && nd.pts[1] == 0, "归一化：首点落在 0,0");
        std::vector<PathPointF> back(n.pts.size());
        DenormPoints(nd.pts.data(), static_cast<uint32_t>(n.pts.size()),
                     n.minX, n.minY, n.Width(), n.Height(), back.data());
        float worst = 0;
        for (size_t i = 0; i < back.size(); i++) {
            worst = std::max(worst, std::fabs(back[i].x - n.pts[i].x));
            worst = std::max(worst, std::fabs(back[i].y - n.pts[i].y));
        }
        std::printf("  [svgpath] 归一化往返误差 %.6f px\n", worst);
        CHECK(worst < 0.001f, "归一化：往返误差 < 0.001px");
    }

    // 容差方向：越粗点越少，但仍在可接受区间
    {
        PathShape fine, coarse;
        ParsePathData("M0 0 C0 50 100 50 100 0", 0.01f, fine, &err);
        ParsePathData("M0 0 C0 50 100 50 100 0", 1.0f, coarse, &err);
        std::printf("  [svgpath] 容差 0.01 → %d 点，容差 1.0 → %d 点\n",
                    (int)fine.pts.size(), (int)coarse.pts.size());
        CHECK(coarse.pts.size() < fine.pts.size(), "容差：越粗点数越少");
        CHECK(coarse.pts.size() >= 4 && coarse.pts.size() <= 64, "容差：1.0 的点数合理");
    }

    // 错误输入：报错但不崩
    {
        PathShape bad;
        CHECK(!ParsePathData("M10", 0.1f, bad, &err), "错误：缺坐标要报错");
        CHECK(!err.empty(), "错误：有可读的原因");
        CHECK(!ParsePathData("X10 10", 0.1f, bad, &err), "错误：未知命令要报错");
        CHECK(!ParsePathData("M0 0 A10 10 0 2 1 10 10", 0.1f, bad, &err),
              "错误：弧标志只能是 0/1");
        CHECK(ParsePathData("", 0.1f, bad, &err) && bad.Empty(), "错误：空串不是错误，得到空路径");
        CHECK(ParsePathData("M10 10", 0.1f, bad, &err) && bad.Empty(),
              "错误：只有 M 没有面积，按空路径处理");
    }

    // 点数上限：撞上限置 truncated（调用方提高容差重来）
    {
        PathShape cap;
        ParsePathData("M0 0 C0 500 500 500 500 0", 0.001f, cap, &err, 64);
        CHECK(cap.truncated, "上限：撞上限置 truncated");
        CHECK(cap.pts.size() <= 64, "上限：绝不超限");
    }
}

static void TestFill() {
    // 直角三角形：面积与采样双向验证
    {
        PathShape tri;
        std::string err;
        ParsePathData("M0 0 L40 0 L0 40 Z", 0.01f, tri, &err);
        DevicePath dev = ToDevice(tri);
        PaintBand bands[kMaxPathBands];
        bool trunc = false;
        const int n = BuildPathFillBands(dev.view(), false, 0, 0, 64, 64, 0xFF00FF00u,
                                         bands, kMaxPathBands, 1, &trunc);
        std::printf("  [svgpath] 三角形：%d 条带，面积 %.0f（理论 800）\n", n, BandArea(bands, n));
        CHECK(!trunc, "填充：小图形不截断");
        CHECK_NEAR(BandArea(bands, n), 800.0, 60.0, "填充：面积与解析解一致");
        CHECK(n <= 40, "填充：条带数不超过行数");
        CHECK(BandCovers(bands, n, 10, 10), "填充：重心被覆盖");
        CHECK(!BandCovers(bands, n, 30, 30), "填充：外部不覆盖");
    }

    // 带洞的环：evenodd 挖空 + 条带合并（4 条：上边、左右两段、下边）
    {
        PathShape donut;
        std::string err;
        ParsePathData("M0 0 H100 V100 H0 Z M25 25 H75 V75 H25 Z", 0.01f, donut, &err);
        DevicePath dev = ToDevice(donut);
        PaintBand bands[kMaxPathBands];
        bool t1 = false, t2 = false;
        const int ne = BuildPathFillBands(dev.view(), true, 0, 0, 120, 120, 0xFF000000u,
                                          bands, kMaxPathBands, 1, &t1);
        CHECK(!t1, "填充：环不截断");
        CHECK(!BandCovers(bands, ne, 50, 50), "填充：evenodd 挖掉内洞");
        CHECK(BandCovers(bands, ne, 10, 50), "填充：evenodd 环上实心");
        CHECK(ne <= 8, "填充：环的条带被合并（≤8 条）");
        const int nz = BuildPathFillBands(dev.view(), false, 0, 0, 120, 120, 0xFF000000u,
                                          bands, kMaxPathBands, 1, &t2);
        CHECK(!t2, "填充：nonzero 不截断");
        CHECK(BandCovers(bands, nz, 50, 50), "填充：nonzero 同向时内洞仍然实心");
        CHECK(nz <= 4, "填充：nonzero 整块合并成少量条带");
    }

    // 撞 cap：必须 truncated，且降级（更大的 cap / 更粗步长）能画完
    {
        PathShape big;
        AppendEllipse(big, 200, 200, 200, 200, 0.01f);
        DevicePath dev = ToDevice(big);
        PaintBand smallCap[16];
        bool trunc = false;
        const int n = BuildPathFillBands(dev.view(), false, 0, 0, 400, 400, 0xFF000000u,
                                         smallCap, 16, 1, &trunc);
        CHECK(trunc && n <= 16, "填充：cap 触顶置 truncated 且不越界");
        PaintBand bigCap[256];
        bool trunc2 = false;
        const int n2 = BuildPathFillBands(dev.view(), false, 0, 0, 400, 400, 0xFF000000u,
                                          bigCap, 256, 8, &trunc2);
        std::printf("  [svgpath] 400px 圆：cap=16 截断=%d；cap=256 步长8 → %d 条 截断=%d\n",
                    (int)trunc, n2, (int)trunc2);
        CHECK(!trunc2 && n2 <= 256, "填充：降级后能完整画完");
    }
}

// 收集描边回调：线段数 / 圆点数 / 多边形顶点（斜接与斜面靠它验证）
struct StrokeRec {
    int lines = 0, dots = 0;
    struct Seg { float x0, y0, x1, y1; uint32_t color; };
    Seg seg[64] = {};
    int nseg = 0;
    std::vector<std::vector<std::pair<float, float>>> polys;
    void Reset() { lines = dots = 0; nseg = 0; polys.clear(); }
    // 任意点是否落在收集到的某个多边形内（凸多边形，按顶点绕向逐边同号判定）
    bool Covers(float px, float py) const {
        for (const auto& p : polys) {
            const size_t n = p.size();
            if (n < 3) continue;
            // 有向面积定绕向：正 = 逆时针（内侧叉积恒正），负 = 顺时针（恒负）
            float area2 = 0.0f;
            for (size_t i = 0; i < n; i++) {
                const auto& a = p[i];
                const auto& b = p[(i + 1) % n];
                area2 += a.first * b.second - b.first * a.second;
            }
            const float want = area2 >= 0.0f ? 1.0f : -1.0f;
            bool in = true;
            for (size_t i = 0; i < n && in; i++) {
                const auto& a = p[i];
                const auto& b = p[(i + 1) % n];
                const float cross = (b.first - a.first) * (py - a.second) -
                                    (b.second - a.second) * (px - a.first);
                if (cross * want < 0.0f) in = false;
            }
            if (in) return true;
        }
        return false;
    }
};

static int StrokeWith(const DevicePath& dev, float width, bool roundCap, uint8_t join,
                      uint8_t miterLimit, StrokeRec& rec, uint32_t strokeColor = 0xFF000000u,
                      uint8_t cap = 0) {
    rec.Reset();
    return PaintPathStroke(dev.view(), width, cap ? cap : static_cast<uint8_t>(roundCap ? BINUI_CAP_ROUND : BINUI_CAP_BUTT),
        join, miterLimit,
        [&](float, float) { return strokeColor; },
        [&](float x0, float y0, float x1, float y1, float, uint32_t c) {
            rec.lines++;
            if (rec.nseg < 64) rec.seg[rec.nseg++] = { x0, y0, x1, y1, c };
        },
        [&](float, float, float, float, float, uint32_t) { rec.dots++; },
        [&](const float* xy, int n, uint32_t) {
            std::vector<std::pair<float, float>> p;
            for (int i = 0; i < n; i++) p.push_back({ xy[i * 2], xy[i * 2 + 1] });
            rec.polys.push_back(std::move(p));
        });
}

static void TestStroke() {
    PathShape sq;
    std::string err;
    ParsePathData("M0 0 H100 V100 H0 Z", 0.01f, sq, &err);
    DevicePath dev = ToDevice(sq);
    StrokeRec rec;
    const int segs = StrokeWith(dev, 2.0f, false, BINUI_JOIN_ROUND, 0, rec);
    CHECK(segs == 4 && rec.lines == 4, "描边：闭合路径段数 = 点数");
    CHECK(rec.dots == 4, "描边：圆角连接在四个直角各补一个圆点");
    CHECK(rec.polys.empty(), "描边：圆角连接不产生多边形");

    PathShape open;
    ParsePathData("M0 0 H100 V100", 0.01f, open, &err);
    DevicePath odev = ToDevice(open);
    const int osegs = StrokeWith(odev, 2.0f, false, BINUI_JOIN_ROUND, 0, rec);
    CHECK(osegs == 2 && rec.lines == 2, "描边：开放路径段数 = 点数-1");
    CHECK(rec.dots == 1, "描边：只在真实转角补点");

    // ---- 斜接 / 斜面：直角转弯（0,0)→(100,0)→(100,100)，线宽 10（半宽 5）----
    // 转角缺口是 (100,-5) (100,0) (105,0) 这个 5×5 方块的下三角。
    const int nBevel = StrokeWith(odev, 10.0f, false, BINUI_JOIN_BEVEL, 0, rec);
    CHECK(nBevel == 2 && rec.polys.size() == 1, "斜面：只补一个多边形");
    CHECK(rec.polys.size() == 1 && rec.polys[0].size() == 3, "斜面：补的是三角形");
    CHECK(rec.dots == 0, "斜面：不再补圆点");
    if (rec.polys.size() == 1) {
        const auto& p = rec.polys[0];
        std::printf("  [svgpath] 斜面三角 = (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)\n",
                    (double)p[0].first, (double)p[0].second, (double)p[1].first,
                    (double)p[1].second, (double)p[2].first, (double)p[2].second);
        CHECK_NEAR(p[0].first, 100.0f, 0.01f, "斜面：p1 在入射段外侧");
        CHECK_NEAR(p[0].second, -5.0f, 0.01f, "斜面：p1 偏移半线宽");
        CHECK(rec.Covers(102.0f, -1.0f), "斜面：缺口靠外的三角区被覆盖");
        CHECK(!rec.Covers(104.0f, -4.0f), "斜面：尖角（斜接）区域不覆盖");
    }

    const int nMiter = StrokeWith(odev, 10.0f, false, BINUI_JOIN_MITER, 0, rec);
    CHECK(nMiter == 2 && rec.polys.size() == 1, "斜接：补一个多边形");
    CHECK(rec.polys.size() == 1 && rec.polys[0].size() == 4, "斜接：补的是四边形");
    CHECK(rec.Covers(104.0f, -4.0f), "斜接：尖角区域被覆盖（斜面盖不到的那块）");
    if (rec.polys.size() == 1) {
        const auto& p = rec.polys[0];
        std::printf("  [svgpath] 斜接四边形 = (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)\n",
                    (double)p[0].first, (double)p[0].second, (double)p[1].first,
                    (double)p[1].second, (double)p[2].first, (double)p[2].second,
                    (double)p[3].first, (double)p[3].second);
        CHECK_NEAR(p[1].first, 105.0f, 0.01f, "斜接点 x = 顶点 + 半线宽");
        CHECK_NEAR(p[1].second, -5.0f, 0.01f, "斜接点 y = 顶点 - 半线宽（直角 1/sin45° = √2）");
    }

    // miter-limit：直角比值 √2 ≈ 1.414，限制给 1 时必须退回斜面
    const int nLim = StrokeWith(odev, 10.0f, false, BINUI_JOIN_MITER, 1, rec);
    CHECK(nLim == 2 && rec.polys.size() == 1 && rec.polys[0].size() == 3,
          "斜接：超过 miter-limit 退回斜面（规范要求）");

    // 尖角：几乎原路折返 → 斜接长度爆炸（1/sin(θ/2) 远大于 4），默认 limit 必须退回斜面。
    // 注意"几乎直线"反而是 length≈1 的正常斜接，别拿它当尖角。
    PathShape sharp;
    ParsePathData("M0 0 L100 0 L4 4", 0.01f, sharp, &err);
    DevicePath sharpDev = ToDevice(sharp);
    const int nSharp = StrokeWith(sharpDev, 10.0f, false, BINUI_JOIN_MITER, 0, rec);
    CHECK(nSharp == 2 && rec.polys.size() == 1 && rec.polys[0].size() == 3,
          "斜接：尖角超过默认 miter-limit 4 退回斜面");
    // 换成 limit 上限（255）就该真的画尖角
    StrokeWith(sharpDev, 10.0f, false, BINUI_JOIN_MITER, 255, rec);
    CHECK(rec.polys.size() == 1 && rec.polys[0].size() == 4, "斜接：limit 放大后画真尖角");
    // 几乎直线的折点（转角 < 15°）：连缺口都不补 —— 折线是从曲线细分出来的，
    // 这种微折点每个都补会让 draw call 翻倍，而缺口只有零点几像素。
    PathShape nearly;
    ParsePathData("M0 0 L100 0 L200 4", 0.01f, nearly, &err);
    DevicePath nearlyDev = ToDevice(nearly);
    StrokeWith(nearlyDev, 10.0f, false, BINUI_JOIN_MITER, 0, rec);
    CHECK(rec.polys.empty() && rec.dots == 0, "斜接：转角 < 15° 的微折点不补缺口");

    // 共线（没有转角）：不该补任何东西
    PathShape flat;
    ParsePathData("M0 0 H50 H100", 0.01f, flat, &err);
    DevicePath flatDev = ToDevice(flat);
    StrokeWith(flatDev, 10.0f, false, BINUI_JOIN_MITER, 0, rec);
    CHECK(rec.polys.empty() && rec.dots == 0, "共线：不补缺口");

    // ---- stroke-linecap="square"：两端各沿切线外延半线宽 ----
    // 开放路径 (0,0)→(100,0)，线宽 10：butt 是 0..100，square 应该是 -5..105
    PathShape seg1;
    ParsePathData("M0 0 H100", 0.01f, seg1, &err);
    DevicePath seg1Dev = ToDevice(seg1);
    StrokeWith(seg1Dev, 10.0f, false, BINUI_JOIN_ROUND, 0, rec, 0xFF000000u, BINUI_CAP_BUTT);
    CHECK(rec.lines == 1 && rec.seg[0].x0 == 0.0f && rec.seg[0].x1 == 100.0f,
          "线帽 butt：端点就是路径端点");
    StrokeWith(seg1Dev, 10.0f, false, BINUI_JOIN_ROUND, 0, rec, 0xFF000000u, BINUI_CAP_SQUARE);
    std::printf("  [svgpath] 方头端点：(%.1f,%.1f) → (%.1f,%.1f)\n",
                (double)rec.seg[0].x0, (double)rec.seg[0].y0,
                (double)rec.seg[0].x1, (double)rec.seg[0].y1);
    CHECK_NEAR(rec.seg[0].x0, -5.0f, 0.01f, "线帽 square：起点外延半线宽");
    CHECK_NEAR(rec.seg[0].x1, 105.0f, 0.01f, "线帽 square：终点外延半线宽");
    CHECK(rec.dots == 0, "线帽 square：不补圆点");
    // 圆头仍然只在两端补圆
    StrokeWith(seg1Dev, 10.0f, true, BINUI_JOIN_ROUND, 0, rec, 0xFF000000u, BINUI_CAP_ROUND);
    CHECK(rec.dots == 2, "线帽 round：两端各补一个圆");
}


// 编译 → 加载 → 路径段往返：<path>/<ellipse> 必须编成 BINUI_PATH 节点，
// 节点盒 = 几何包围盒，路径段里的折线能被渲染器取到（这是 B 阶段"端到端通"的证据）。
static void TestCompileAndLoad() {
    const char* html =
        "<app_window width=\"220\" height=\"120\">\n"
        "<html width=\"220\" height=\"120\" bg=\"#000000\">\n<body>\n"
        "<svg width=\"220\" height=\"120\">\n"
        "  <path d=\"M0 0 H100 V100 H0 Z M30 30 H70 V70 H30 Z\" fill=\"#FF3B30\" fill-rule=\"evenodd\"/>\n"
        "  <ellipse cx=\"170\" cy=\"60\" rx=\"30\" ry=\"20\" fill=\"#34C759\"/>\n"
        "  <path d=\"M20 0 L60 0 L40 30 Z\" fill=\"none\" stroke=\"#0A84FF\" stroke-width=\"4\"/>\n"
        "</svg>\n</body></html></app_window>\n";
    auto res = binui::CompileBinUIEx(html);
    CHECK(!res.layout.empty(), "编译：产出布局");
    for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
    CHECK(res.warnings.empty(), "编译：<path>/<ellipse> 不再报警告");

    BinUIRenderer r;
    CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "加载：布局可解析（v15）");

    std::vector<uint16_t> pathIds;
    for (const auto& n : r.getNodes())
        if (n.type == BINUI_PATH) pathIds.push_back(n.id);
    CHECK(pathIds.size() == 3, "加载：三个图形都是 BINUI_PATH 节点");
    if (pathIds.size() == 3) {
        const BinUIRenderer::NodeInfo* donut = nullptr;
        const BinUIRenderer::NodeInfo* ell = nullptr;
        const BinUIRenderer::NodeInfo* tri = nullptr;
        for (const auto& n : r.getNodes()) {
            if (n.id == pathIds[0]) donut = &n;
            if (n.id == pathIds[1]) ell = &n;
            if (n.id == pathIds[2]) tri = &n;
        }
        if (donut) {
            std::printf("  [svgpath] donut 盒 = (%.1f,%.1f %.1fx%.1f)\n",
                        (double)donut->x, (double)donut->y,
                        (double)donut->width, (double)donut->height);
            CHECK_NEAR(donut->x, 0, 1.0f, "包围盒：环的左侧");
            CHECK_NEAR(donut->width, 100, 1.0f, "包围盒：环的宽度");
            CHECK_NEAR(donut->height, 100, 1.0f, "包围盒：环的高度");
        }
        if (ell) {
            CHECK_NEAR(ell->width, 60, 1.0f, "包围盒：椭圆宽 = 2rx");
            CHECK_NEAR(ell->height, 40, 1.0f, "包围盒：椭圆高 = 2ry");
        }
        if (tri) {
            // fill=none + stroke-width 4：盒要外扩 2px
            CHECK_NEAR(tri->height, 34, 1.5f, "包围盒：描边外扩（高 + strokeW）");
        }
        const auto* rec = r.pathOf(pathIds[0]);
        CHECK(rec != nullptr, "路径段：环能取到记录");
        if (rec) {
            CHECK(rec->head.contourCount == 2, "路径段：两条子路径（外框 + 洞）");
            CHECK((rec->head.flags & PATH_FILL) != 0, "路径段：填充位");
            CHECK((rec->head.flags & PATH_EVENODD) != 0, "路径段：evenodd 位");
            CHECK(rec->pts.size() == (size_t)rec->head.pointCount * 2, "路径段：点在表内");
            CHECK(!rec->count.empty() && rec->count[0] >= 4, "路径段：子路径点数派生正确");
            // 归一化：所有点都在 0..65535
            bool inRange = true;
            for (uint16_t v : rec->pts) if (v > kPathNormMax) inRange = false;
            CHECK(inRange, "路径段：归一化坐标在 0..65535");
        }
        const auto* trec = r.pathOf(pathIds[2]);
        CHECK(trec && (trec->head.flags & PATH_STROKE) && !(trec->head.flags & PATH_FILL),
              "路径段：fill=none 的三角只有描边位");
        CHECK(trec && trec->head.strokeW == 4, "路径段：stroke-width 落库");
    }
}

// SVG 语义层：viewBox / preserveAspectRatio / <g transform> / 颜料继承 / CSS 优先级。
// 全部用"编译 → 加载 → 量节点盒"验证，不需要渲染后端。
static void TestSvgSemantics() {
    auto compile1 = [](const char* svgBody, float& x, float& y, float& w, float& h,
                       uint32_t* fillOut, bool* ok) -> bool {
        std::string html = std::string(
            "<app_window width=\"300\" height=\"200\"><html width=\"300\" height=\"200\" bg=\"#000000\">"
            "<head><style>.cssfill{fill:#0000FF;}</style></head><body>") + svgBody +
            "</body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        *ok = !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size());
        if (!*ok) return false;
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            x = n.x; y = n.y; w = n.width; h = n.height;
            if (fillOut) {
                const auto* rec = r.pathOf(n.id);
                if (rec) *fillOut = rec->head.fillColor;
            }
            return true;
        }
        return false;
    };

    float x = 0, y = 0, w = 0, h = 0;
    bool ok = false;

    // viewBox 0 0 24 24 → 96x96：铺满
    {
        compile1("<svg x=\"10\" y=\"10\" width=\"96\" height=\"96\" viewBox=\"0 0 24 24\">"
                 "<path d=\"M0 0 H24 V24 H0 Z\" fill=\"#FF0000\"/></svg>", x, y, w, h, nullptr, &ok);
        CHECK(ok, "viewBox：编译并加载");
        std::printf("  [svgpath] viewBox 铺满：盒 = (%.1f,%.1f %.1fx%.1f)\n",
                    (double)x, (double)y, (double)w, (double)h);
        CHECK_NEAR(w, 96, 1.0f, "viewBox：24 单位铺满 96px");
        CHECK_NEAR(h, 96, 1.0f, "viewBox：高度同样铺满");
        CHECK_NEAR(x, 10, 1.0f, "viewBox：位置跟着 svg 走");
    }

    // 非等比容器 + meet：等比缩到 50，水平居中
    {
        compile1("<svg x=\"0\" y=\"0\" width=\"100\" height=\"50\" viewBox=\"0 0 24 24\">"
                 "<path d=\"M0 0 H24 V24 H0 Z\" fill=\"#FF0000\"/></svg>", x, y, w, h, nullptr, &ok);
        CHECK(ok, "meet：编译并加载");
        std::printf("  [svgpath] meet 居中：盒 = (%.1f,%.1f %.1fx%.1f)\n",
                    (double)x, (double)y, (double)w, (double)h);
        CHECK_NEAR(w, 50, 1.0f, "preserveAspectRatio：等比缩到短边");
        CHECK_NEAR(x, 25, 1.0f, "preserveAspectRatio：水平居中");
        CHECK_NEAR(h, 50, 1.0f, "preserveAspectRatio：高度铺满");
    }

    // 颜料继承：<svg fill> 管到子元素
    {
        uint32_t col = 0;
        compile1("<svg x=\"0\" y=\"0\" width=\"48\" height=\"48\" viewBox=\"0 0 24 24\" fill=\"#00FF00\">"
                 "<path d=\"M0 0 H24 V24 H0 Z\"/></svg>", x, y, w, h, &col, &ok);
        CHECK(ok, "继承：编译并加载");
        CHECK(col == 0xFF00FF00u, "继承：<svg fill> 传给子 <path>");
    }

    // CSS 规则优先于表现属性（SVG 规范；与 binUI 其它属性的口径相反）
    {
        uint32_t col = 0;
        compile1("<svg x=\"0\" y=\"0\" width=\"48\" height=\"48\" viewBox=\"0 0 24 24\">"
                 "<path class=\"cssfill\" fill=\"#FF0000\" d=\"M0 0 H24 V24 H0 Z\"/></svg>",
                 x, y, w, h, &col, &ok);
        CHECK(ok, "CSS 优先级：编译并加载");
        CHECK(col == 0xFF0000FFu, "CSS 优先级：类规则盖掉 fill 属性（蓝）");
    }

    // <g transform="scale(2)">：坐标翻倍
    {
        compile1("<svg x=\"0\" y=\"0\" width=\"96\" height=\"96\" viewBox=\"0 0 48 48\">"
                 "<g transform=\"scale(2)\"><path d=\"M2 2 H10 V10 H2 Z\" fill=\"#FF0000\"/></g></svg>",
                 x, y, w, h, nullptr, &ok);
        CHECK(ok, "g transform：编译并加载");
        std::printf("  [svgpath] g scale(2)：盒 = (%.1f,%.1f %.1fx%.1f)\n",
                    (double)x, (double)y, (double)w, (double)h);
        // 2..10 用户单位 × 2（g）× 2（viewBox 48→96）= 8..40
        CHECK_NEAR(x, 8, 1.0f, "g transform：起点跟着缩放");
        CHECK_NEAR(w, 32, 1.0f, "g transform：尺寸跟着缩放");
    }

    // stroke-width 跟着 viewBox 一起放大（否则放大后线条还是细的）
    {
        std::string html =
            "<app_window width=\"200\" height=\"100\"><html width=\"200\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"96\" height=\"96\" viewBox=\"0 0 24 24\">"
            "<path d=\"M0 0 H24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\"/></svg>"
            "</body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        CHECK(!res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()),
              "stroke 缩放：编译并加载");
        bool found = false;
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            const auto* rec = r.pathOf(n.id);
            if (rec) {
                found = true;
                std::printf("  [svgpath] stroke-width 2 → 落库 %u（viewBox 24→96 应为 8）\n",
                            (unsigned)rec->head.strokeW);
                CHECK(rec->head.strokeW == 8, "stroke 缩放：2 单位 × 4 = 8px");
            }
        }
        CHECK(found, "stroke 缩放：取到路径记录");
    }
}

// 圆角矩形路径 + stroke-dasharray（编译期切段）+ 圆/矩形带描边时改走路径
static void TestShapesAndDash() {
    // 圆角矩形：包围盒 = 外框，四个角是弧
    {
        PathShape rr;
        AppendRoundedRect(rr, 10, 20, 60, 40, 8, 0.05f);
        CHECK(rr.contours.size() == 1 && rr.contours[0].closed, "圆角矩形：一条闭合子路径");
        CHECK_NEAR(rr.minX, 10, 0.1f, "圆角矩形：左边界");
        CHECK_NEAR(rr.maxX, 70, 0.1f, "圆角矩形：右边界");
        CHECK_NEAR(rr.minY, 20, 0.1f, "圆角矩形：上边界");
        CHECK_NEAR(rr.maxY, 60, 0.1f, "圆角矩形：下边界");
        CHECK(PointInPath(rr, 40, 40, false), "圆角矩形：中心在内部");
        CHECK(!PointInPath(rr, 12, 22, false), "圆角矩形：切掉的外角在外部");
        CHECK(rr.pts.size() > 4, "圆角矩形：圆角处有细分点");
    }

    // 虚线：10 单位直线 + dash 2/2 → 3 段实线（0-2 / 4-6 / 8-10）
    {
        PathShape line;
        std::string err;
        ParsePathData("M0 0 H10", 0.1f, line, &err);
        std::vector<float> pat{ 2.0f, 2.0f };
        const bool ok = ApplyDashPattern(line, pat);
        std::printf("  [svgpath] 虚线：%d 条子路径（期望 3）\n", (int)line.contours.size());
        CHECK(ok && line.contours.size() == 3, "虚线：切成 3 段实线");
        double total = 0;
        for (const auto& c : line.contours) {
            CHECK(c.count >= 2, "虚线：每段至少两个点");
            const PathPointF& a = line.pts[c.first];
            const PathPointF& b = line.pts[c.first + c.count - 1];
            total += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        }
        CHECK_NEAR(total, 6.0, 0.05, "虚线：实线总长 = 3 段 × 2");
        CHECK(!ApplyDashPattern(line, { 0.0f, 0.0f }), "虚线：全 0 模式 → 不画");
    }

    // 编译期：带描边的 <circle>/<rect> 必须变成 BINUI_PATH 且 fill/stroke 都在
    {
        std::string html =
            "<app_window width=\"200\" height=\"100\"><html width=\"200\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"96\" height=\"96\" viewBox=\"0 0 24 24\">"
            "<circle cx=\"12\" cy=\"12\" r=\"9\" fill=\"#123456\" stroke=\"#7AAAFF\" stroke-width=\"2\"/>"
            "<rect x=\"2\" y=\"2\" width=\"8\" height=\"8\" rx=\"2\" fill=\"none\" stroke=\"#4ED17E\"/>"
            "<path d=\"M0 20 H24\" fill=\"none\" stroke=\"#F59E0B\" stroke-width=\"2\" stroke-dasharray=\"4 2\"/>"
            "</svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
        CHECK(res.warnings.empty(), "形状降级：不再告警");
        BinUIRenderer r;
        CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "形状降级：布局可解析");
        int pathNodes = 0, fillStroke = 0, dashPaths = 0;
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            pathNodes++;
            const auto* rec = r.pathOf(n.id);
            if (!rec) continue;
            const bool hasFill = (rec->head.flags & PATH_FILL) != 0;
            const bool hasStroke = (rec->head.flags & PATH_STROKE) != 0;
            if (hasFill && hasStroke) fillStroke++;
            if (hasStroke && rec->first.size() >= 3) dashPaths++;   // 虚线被切成多条子路径
        }
        std::printf("  [svgpath] 形状降级：路径节点 %d，fill+stroke %d，虚线路径 %d\n",
                    pathNodes, fillStroke, dashPaths);
        CHECK(pathNodes == 3, "形状降级：三个图形都是路径节点");
        CHECK(fillStroke == 1, "形状降级：圆是 fill+stroke 并存");
        CHECK(dashPaths == 1, "形状降级：虚线路径有多条子路径");
    }
}

// <defs>/<symbol> + <use href>：模板不直接画，引用处按 x/y 平移，
// symbol 的 viewBox 按 use 的 width/height 缩放
static void TestUseDefs() {
    std::string html =
        "<app_window width=\"200\" height=\"80\"><html width=\"200\" height=\"80\" bg=\"#000000\">"
        "<body><svg x=\"0\" y=\"0\" width=\"200\" height=\"80\" viewBox=\"0 0 200 80\">"
        "<defs>"
        "  <g id=\"tile\"><rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"#7AAAFF\"/></g>"
        "</defs>"
        "<use href=\"#tile\" x=\"10\" y=\"10\"/>"
        "<use href=\"#tile\" x=\"40\" y=\"10\"/>"
        "<symbol id=\"sq\" viewBox=\"0 0 10 10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#F2B544\"/></symbol>"
        "<use href=\"#sq\" x=\"80\" y=\"10\" width=\"40\" height=\"40\"/>"
        "</svg></body></html></app_window>";
    auto res = binui::CompileBinUIEx(html);
    for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
    CHECK(res.warnings.empty(), "use/defs：不报警告");
    BinUIRenderer r;
    CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "use/defs：布局可解析");

    std::vector<const BinUIRenderer::NodeInfo*> tiles;
    const BinUIRenderer::NodeInfo* bigSq = nullptr;
    for (const auto& n : r.getNodes()) {
        // 只有填充的 <rect> 编成 LIST（九宫格），带描边才是 PATH；这里按尺寸认
        if (std::fabs(n.width - 20.0f) < 0.5f && std::fabs(n.height - 20.0f) < 0.5f &&
            (n.type == BINUI_PATH || n.type == BINUI_LIST))
            tiles.push_back(&n);
        if (std::fabs(n.width - 40.0f) < 1.5f && std::fabs(n.height - 40.0f) < 1.5f) bigSq = &n;
    }
    std::printf("  [svgpath] use/defs：20px 方块 %d 个，40px 方块 %s\n",
                (int)tiles.size(), bigSq ? "有" : "无");
    CHECK(tiles.size() == 2, "use/defs：模板被引用两次 → 两个方块");
    if (tiles.size() == 2) {
        CHECK_NEAR(tiles[0]->x, 10, 1.0f, "use/defs：第一个引用按 x 平移");
        CHECK_NEAR(tiles[1]->x, 40, 1.0f, "use/defs：第二个引用按 x 平移");
    }
    CHECK(bigSq != nullptr, "use/symbol：按 width/height 缩放（40px）");
    if (bigSq) CHECK_NEAR(bigSq->x, 80, 2.0f, "use/symbol：x 平移生效");
}

// <text> → LABEL（绝对定位、fill 当文字色）、<image href> → IMAGE（资源路径）
static void TestSvgTextImage() {
    std::string html =
        "<app_window width=\"200\" height=\"100\"><html width=\"200\" height=\"100\" bg=\"#000000\">"
        "<body><svg x=\"0\" y=\"0\" width=\"200\" height=\"100\" viewBox=\"0 0 200 100\">"
        "<text x=\"10\" y=\"40\" font-size=\"20\" fill=\"#7AAAFF\">SVG 文本</text>"
        "<image x=\"120\" y=\"20\" width=\"40\" height=\"40\" href=\"pic.png\"/>"
        "</svg></body></html></app_window>";
    auto res = binui::CompileBinUIEx(html);
    for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
    CHECK(res.warnings.empty(), "text/image：不报警告");
    BinUIRenderer r;
    CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "text/image：布局可解析");

    const BinUIRenderer::NodeInfo* label = nullptr;
    const BinUIRenderer::NodeInfo* img = nullptr;
    for (const auto& n : r.getNodes()) {
        if (n.type == BINUI_LABEL && !n.text.empty()) label = &n;
        if (n.type == BINUI_IMAGE) img = &n;
    }
    CHECK(label != nullptr, "text：编成 LABEL 且带文本");
    if (label) {
        std::printf("  [svgpath] svg text：盒 = (%.1f,%.1f %.1fx%.1f) 色 = %08X 文本 = %s\n",
                    (double)label->x, (double)label->y, (double)label->width,
                    (double)label->height, label->color, label->text.c_str());
        CHECK_NEAR(label->x, 10, 1.0f, "text：x 生效");
        // y=40 是基线，盒顶 ≈ 40 - 0.8*20 = 24
        CHECK(std::fabs(label->y - 24.0f) < 3.0f, "text：基线 → 盒顶的近似");
        CHECK(label->color == 0xFF7AAAFFu, "text：fill 当文字色");
        std::printf("  [svgpath] svg text fontSize = %u\n", (unsigned)label->fontSize);
        CHECK(label->fontSize == 20, "text：font-size 生效");
    }
    CHECK(img != nullptr, "image：编成 IMAGE 节点");
    if (img) {
        CHECK_NEAR(img->x, 120, 1.0f, "image：x 生效");
        CHECK_NEAR(img->width, 40, 1.0f, "image：width 生效");
        const auto& paths = r.imagePaths();
        CHECK(img->imageIndex < paths.size() && paths[img->imageIndex] == "pic.png",
              "image：href 进了资源表");
    }
}

// v16 渐变条带：量化 + 横向切段 + 纵向合并 + spread 折叠（纯几何，不经过编译器）
static void TestGradientBands() {
    auto linGrad = [](float x1, float y1, float x2, float y2) {
        BinUIPathGradient g{};
        g.type = PATH_GRAD_LINEAR;
        g.spread = PATH_GRAD_PAD;
        g.stopCount = 2;
        g.x1 = x1; g.y1 = y1; g.x2 = x2; g.y2 = y2;
        g.stops[0] = { 0.0f, 0xFFFF0000u };
        g.stops[1] = { 1.0f, 0xFF0000FFu };
        return g;
    };
    auto run = [](const DevicePath& dev, const PathShade& s, float w, float h,
                  PaintBand* bands, bool* trunc) {
        return BuildPathFillBands(dev.view(), false, 0, 0, w, h, s, bands, kMaxPathBands, 1, trunc);
    };

    PathShape sq;
    std::string err;
    ParsePathData("M0 0 H100 V100 H0 Z", 0.01f, sq, &err);
    DevicePath dev = ToDevice(sq);
    PaintBand bands[kMaxPathBands];

    // 纯色：取样器引入后必须还是 1 条整块（老行为不能变）
    {
        PathShade solid;
        solid.color = 0xFF123456u;
        bool trunc = false;
        const int n = run(dev, solid, 100, 100, bands, &trunc);
        CHECK(!trunc && n == 1, "渐变：纯色仍合并成 1 条");
        CHECK(n == 1 && bands[0].color == 0xFF123456u, "渐变：纯色颜色不变");
        CHECK(n == 1 && bands[0].w == 100.0f && bands[0].h == 100.0f, "渐变：纯色覆盖整盒");
    }

    // 垂直渐变（上红下蓝）：级内同色 → 纵向合并，条数与级数同量级
    {
        BinUIPathGradient g = linGrad(0, 0, 0, 1);
        const PathShade s = MakePathShade(&g, 0xFFFFFFFFu, 1.0f, 0, 0, 100, 100);
        bool trunc = false;
        const int n = run(dev, s, 100, 100, bands, &trunc);
        std::printf("  [svgpath] 渐变垂直：levels=%d 条带=%d 首条高=%.0f 首色=%08X 末色=%08X\n",
                    s.levels, n, n > 0 ? (double)bands[0].h : 0.0, bands[0].color, bands[n - 1].color);
        CHECK(!trunc, "渐变垂直：不截断");
        CHECK(n <= s.levels * 2 + 4, "渐变垂直：条数与量化级同量级（纵向合并生效）");
        CHECK(bands[0].h > 1.5f, "渐变垂直：同色行合并成高条带");
        CHECK(((bands[0].color >> 16) & 0xFF) > 200 && (bands[0].color & 0xFF) < 60,
              "渐变垂直：顶部接近红");
        CHECK((bands[n - 1].color & 0xFF) > 200 && ((bands[n - 1].color >> 16) & 0xFF) < 60,
              "渐变垂直：底部接近蓝");
        CHECK_NEAR(BandArea(bands, n), 10000.0, 1.0, "渐变垂直：覆盖面积与纯色一致");
        // 单调性：越往下越蓝（红通道不回升）
        bool mono = true;
        for (int i = 1; i < n; i++)
            if (((bands[i].color >> 16) & 0xFF) > ((bands[i - 1].color >> 16) & 0xFF)) mono = false;
        CHECK(mono, "渐变垂直：红通道单调下降（没有乱序条带）");
    }

    // 水平渐变（左红右蓝）：横向切段 + 每一列合并成整列高
    {
        BinUIPathGradient g = linGrad(0, 0, 1, 0);
        const PathShade s = MakePathShade(&g, 0xFFFFFFFFu, 1.0f, 0, 0, 100, 100);
        bool trunc = false;
        const int n = run(dev, s, 100, 100, bands, &trunc);
        std::printf("  [svgpath] 渐变水平：levels=%d 条带=%d 首列=(%.0fx%.0f)\n",
                    s.levels, n, (double)bands[0].w, (double)bands[0].h);
        CHECK(!trunc, "渐变水平：不截断");
        CHECK(n <= s.levels * 2 + 4, "渐变水平：条数与量化级同量级");
        CHECK(bands[0].h == 100.0f, "渐变水平：每列合成整列高");
        CHECK(bands[0].w <= 3.0f, "渐变水平：列宽 ≈ 2px（一级一列）");
        CHECK_NEAR(BandArea(bands, n), 10000.0, 1.0, "渐变水平：覆盖面积与纯色一致");
    }

    // 对角渐变 + 大图：撞上限时靠 rowStep 降级画完（不是画一半）
    {
        PathShape bigSq;
        ParsePathData("M0 0 H400 V400 H0 Z", 0.01f, bigSq, &err);
        DevicePath big = ToDevice(bigSq);
        BinUIPathGradient g = linGrad(0, 0, 1, 1);
        const PathShade s = MakePathShade(&g, 0xFFFFFFFFu, 1.0f, 0, 0, 400, 400);
        bool trunc = false;
        int usedStep = 0, n = 0;
        for (int step = 1; step <= 8; step *= 2) {
            n = BuildPathFillBands(big.view(), false, 0, 0, 400, 400, s, bands, kMaxPathBands,
                                   step, &trunc);
            usedStep = step;
            if (!trunc) break;
        }
        std::printf("  [svgpath] 渐变 400px 对角：levels=%d 条带=%d rowStep=%d\n",
                    s.levels, n, usedStep);
        CHECK(!trunc, "渐变大图：降级后画完");
        CHECK_NEAR(BandArea(bands, n), 160000.0, 400.0, "渐变大图：降级后面积仍然完整");
    }

    // 径向渐变：中心红、边缘蓝；非正方盒仍按椭圆（归一化坐标）
    {
        BinUIPathGradient g{};
        g.type = PATH_GRAD_RADIAL;
        g.stopCount = 2;
        g.x1 = 0.5f; g.y1 = 0.5f; g.rx = 0.5f; g.ry = 0.5f;
        g.stops[0] = { 0.0f, 0xFFFF0000u };
        g.stops[1] = { 1.0f, 0xFF0000FFu };
        const PathShade s = MakePathShade(&g, 0xFFFFFFFFu, 1.0f, 0, 0, 100, 100);
        const PathShade wide = MakePathShade(&g, 0xFFFFFFFFu, 1.0f, 0, 0, 200, 100);
        std::printf("  [svgpath] 渐变径向：中心=%08X 右缘=%08X 宽盒右缘=%08X\n",
                    s.At(50, 50), s.At(99, 50), wide.At(199, 50));
        CHECK(((s.At(50, 50) >> 16) & 0xFF) > 200 && (s.At(50, 50) & 0xFF) < 60,
              "渐变径向：中心接近红");
        CHECK((s.At(99, 50) & 0xFF) > 200, "渐变径向：右缘接近蓝");
        CHECK((wide.At(199, 50) & 0xFF) > 200, "渐变径向：非正方盒按椭圆归一化");
        bool trunc = false;
        const int n = run(dev, s, 100, 100, bands, &trunc);
        CHECK(!trunc && n > 0, "渐变径向：小图一次画完");
        CHECK_NEAR(BandArea(bands, n), 10000.0, 1.0, "渐变径向：覆盖面积与纯色一致");
    }

    // v18 径向焦点（SVG 锥形语义）：焦点自身 t=0，圆心按 |C-F|/(r-|F-C|)
    {
        BinUIPathGradient g{};
        g.type = PATH_GRAD_RADIAL;
        g.stopCount = 2;
        g.x1 = g.y1 = 0.5f;
        g.rx = g.ry = 0.5f;
        g.fx = 0.1f; g.fy = 0.5f;         // 焦点在圆心左侧，仍在圆内
        std::printf("  [svgpath] 径向焦点：焦点=%.3f 圆心=%.3f 右缘=%.3f 上缘=%.3f\n",
                    (double)PathGradT(g, 0.1f, 0.5f), (double)PathGradT(g, 0.5f, 0.5f),
                    (double)PathGradT(g, 0.999f, 0.5f), (double)PathGradT(g, 0.5f, 0.001f));
        CHECK_NEAR(PathGradT(g, 0.1f, 0.5f), 0.0f, 1e-3, "焦点：焦点自身 t=0");
        // 手算：(0.5-0.1)/(0.5+0.5-0.1) = 0.4/0.9
        CHECK_NEAR(PathGradT(g, 0.5f, 0.5f), 0.4444f, 2e-3, "焦点：圆心按锥形语义得 4/9");
        CHECK_NEAR(PathGradT(g, 0.999f, 0.5f), 1.0f, 5e-3, "焦点：右缘 t=1");
        CHECK_NEAR(PathGradT(g, 0.5f, 0.001f), 1.0f, 5e-3, "焦点：上缘 t=1");
        // 焦点跑到圆外：必须退回同心圆，不许出鬼影
        BinUIPathGradient outside = g;
        outside.fx = 3.0f;
        CHECK_NEAR(PathGradT(outside, 0.5f, 0.5f), 0.0f, 1e-3, "焦点：焦点在圆外时退回同心圆");
    }

    // v17 stroke-dashoffset：相位偏移把虚实图案沿弧长前后挪，段数不变
    {
        PathShape line;
        ParsePathData("M0 0 H100", 0.01f, line, &err);
        PathShape da = line, db = line, dc = line;
        CHECK(ApplyDashPattern(da, { 10.0f, 10.0f }, 0.0f), "虚线：offset=0 能切");
        CHECK(ApplyDashPattern(db, { 10.0f, 10.0f }, 5.0f), "虚线：offset=5 能切");
        CHECK(ApplyDashPattern(dc, { 10.0f, 10.0f }, 10.0f), "虚线：offset=整周期能切");
        // 第一段实线的 [起点, 终点]
        auto firstDash = [](const PathShape& sh, float& a, float& b) {
            if (sh.contours.empty() || sh.contours[0].count < 2) { a = b = -1.0f; return; }
            const auto& c = sh.contours[0];
            a = sh.pts[c.first].x;
            b = sh.pts[c.first + c.count - 1].x;
        };
        float a0, b0, a5, b5, a10, b10;
        firstDash(da, a0, b0);
        firstDash(db, a5, b5);
        firstDash(dc, a10, b10);
        std::printf("  [svgpath] 虚线相位：offset 0/5/10 → 首段实线 [%.0f,%.0f] [%.0f,%.0f] [%.0f,%.0f]\n",
                    (double)a0, (double)b0, (double)a5, (double)b5, (double)a10, (double)b10);
        CHECK_NEAR(a0, 0.0f, 1e-3, "虚线：offset=0 从起点开始画");
        CHECK_NEAR(b0, 10.0f, 1e-3, "虚线：offset=0 首段实线 0..10");
        // offset=5：图案往后挪 5，起点仍是 0，但首段只剩后半截（0..5）
        CHECK_NEAR(a5, 0.0f, 1e-3, "虚线：offset=5 起点仍在 0");
        CHECK_NEAR(b5, 5.0f, 1e-3, "虚线：offset=5 首段实线只剩 0..5");
        // offset=10：正好落在空隙里 → 首个实线从 10 开始
        CHECK_NEAR(a10, 10.0f, 1e-3, "虚线：offset=整周期时先空一段，首段实线从 10 开始");
        CHECK_NEAR(b10, 20.0f, 1e-3, "虚线：offset=整周期 首段实线 10..20");
    }

    // v17 preserveAspectRatio：slice + 9 宫格对齐
    {
        PathMatrix m{};
        const float vw = 100.0f, vh = 50.0f, bw = 100.0f, bh = 100.0f;
        BuildViewBoxMatrix(0, 0, vw, vh, bw, bh, "xMidYMid meet", m);
        CHECK_NEAR(m.a, 1.0f, 1e-4, "viewBox meet：sx=1");
        CHECK_NEAR(m.f, 25.0f, 1e-4, "viewBox meet：纵向居中");
        BuildViewBoxMatrix(0, 0, vw, vh, bw, bh, "xMidYMid slice", m);
        CHECK_NEAR(m.a, 2.0f, 1e-4, "viewBox slice：取较大的缩放 2");
        CHECK_NEAR(m.e, -50.0f, 1e-4, "viewBox slice：横向居中溢出 -50");
        BuildViewBoxMatrix(0, 0, vw, vh, bw, bh, "xMinYMin slice", m);
        CHECK_NEAR(m.e, 0.0f, 1e-4, "viewBox slice+xMin：不偏移");
        CHECK_NEAR(m.f, 0.0f, 1e-4, "viewBox slice+yMin：不偏移");
        BuildViewBoxMatrix(0, 0, vw, vh, bw, bh, "xMaxYMax slice", m);
        CHECK_NEAR(m.e, -100.0f, 1e-4, "viewBox slice+xMax：贴右边");
        BuildViewBoxMatrix(0, 0, vw, vh, bw, bh, "none", m);
        CHECK_NEAR(m.a, 1.0f, 1e-4, "viewBox none：sx=boxW/vw");
        CHECK_NEAR(m.d, 2.0f, 1e-4, "viewBox none：sy=boxH/vh");
    }

    // spreadMethod 的 t 折叠
    CHECK(PathGradSpreadT(-0.5f, PATH_GRAD_PAD) == 0.0f, "渐变 spread：pad 负向夹紧");
    CHECK(PathGradSpreadT(1.5f, PATH_GRAD_PAD) == 1.0f, "渐变 spread：pad 正向夹紧");
    CHECK_NEAR(PathGradSpreadT(1.25f, PATH_GRAD_REPEAT), 0.25, 1e-5, "渐变 spread：repeat 取小数");
    CHECK_NEAR(PathGradSpreadT(1.5f, PATH_GRAD_REFLECT), 0.5, 1e-5, "渐变 spread：reflect 折返");
    CHECK_NEAR(PathGradSpreadT(0.25f, PATH_GRAD_REFLECT), 0.25, 1e-5,
               "渐变 spread：reflect 在 0..1 内不折返（只处理越界）");

    // 退化与兜底：起止点重合 / 无渐变 / 只有一个色标 → 纯色一条
    {
        BinUIPathGradient g = linGrad(0.5f, 0.5f, 0.5f, 0.5f);
        const PathShade s = MakePathShade(&g, 0xFF00FF00u, 1.0f, 0, 0, 100, 100);
        CHECK(!s.Active(), "渐变退化：起止点重合不启用");
        bool trunc = false;
        const int n = run(dev, s, 100, 100, bands, &trunc);
        CHECK(n == 1 && bands[0].color == 0xFF00FF00u, "渐变退化：退回兜底纯色");
        const PathShade none = MakePathShade(nullptr, 0xFF112233u, 1.0f, 0, 0, 100, 100);
        CHECK(!none.Active() && none.At(5, 5) == 0xFF112233u, "渐变缺失：纯色直通");
        BinUIPathGradient one = linGrad(0, 0, 1, 0);
        one.stopCount = 1;
        const PathShade s1 = MakePathShade(&one, 0xFF445566u, 1.0f, 0, 0, 100, 100);
        CHECK(!s1.Active() && s1.At(50, 50) == 0xFF445566u, "渐变单色标：按纯色");
    }

    // 渐变描边：每段线按自己的中点取色（v17）—— 折线是细分出来的，逐段取色就是逐像素
    {
        // 用折线（不是单段）：单段只会在中点取一次色，看不出渐变
        PathShape line2;
        ParsePathData("M0 0 L10 4 L20 0 L30 4 L40 0 L50 4 L60 0 L70 4 L80 0 L90 4 L100 0",
                      0.01f, line2, &err);
        DevicePath ldev = ToDevice(line2);
        BinUIPathGradient g = linGrad(0, 0, 1, 0);
        const PathShade sh = MakePathShade(&g, 0xFFFFFFFFu, 1.0f, 0, 0, 100, 100);
        auto strokeRec = [&](const PathShade& s2, uint32_t solid) {
            std::vector<uint32_t> cols;
            PaintPathStroke(ldev.view(), 4.0f, BINUI_CAP_BUTT, BINUI_JOIN_ROUND, 0,
                            [&](float px, float py) { return s2.At(px, py); },
                            [&](float, float, float, float, float, uint32_t c) { cols.push_back(c); },
                            [&](float, float, float, float, float, uint32_t) {},
                            [&](const float*, int, uint32_t) {});
            (void)solid;
            return cols;
        };
        PathShade solidShade;
        solidShade.color = 0xFF00FF00u;
        const auto solidCols = strokeRec(solidShade, 0xFF00FF00u);
        const auto gradCols = strokeRec(sh, 0);
        std::printf("  [svgpath] 渐变描边：%zu 段，纯色首段=%08X，渐变首段=%08X 末段=%08X\n",
                    gradCols.size(), solidCols.empty() ? 0u : solidCols.front(),
                    gradCols.empty() ? 0u : gradCols.front(),
                    gradCols.empty() ? 0u : gradCols.back());
        CHECK(!solidCols.empty() && solidCols.size() == gradCols.size(), "渐变描边：段数与纯色一致");
        CHECK(!solidCols.empty() && solidCols[0] == 0xFF00FF00u && solidCols.back() == 0xFF00FF00u,
              "渐变描边：纯色时段段同色");
        CHECK(!gradCols.empty(), "渐变描边：有线段颜色");
        CHECK(!gradCols.empty() && ((gradCols.front() >> 16) & 0xFF) > 200,
              "渐变描边：起点接近红");
        CHECK(!gradCols.empty() && (gradCols.back() & 0xFF) > 200, "渐变描边：终点接近蓝");
    }

    // 节点 opacity：渐变每条带的 alpha 都要折进去（纯色是调用方先折好的）
    {
        BinUIPathGradient g = linGrad(0, 0, 1, 0);
        const PathShade s = MakePathShade(&g, 0xFFFFFFFFu, 0.5f, 0, 0, 100, 100);
        const uint32_t a = (s.At(50, 50) >> 24) & 0xFFu;
        std::printf("  [svgpath] 渐变 + opacity=0.5：alpha=%02X\n", a);
        CHECK(a >= 0x7Eu && a <= 0x81u, "渐变透明度：alpha 折半");
    }
}

// 描边连接：join / miter-limit 落库，以及"斜接尖角把节点盒撑开"（不撑开会被后端切平）
// 精确命中（v17）：有填充的路径按"点在路径内"判，环的洞 / 星形凹口不再误命中
// SVG 收尾（v18）：polygon/polyline 编成路径、line 按需升级、text-anchor、
// stroke-dashoffset、preserveAspectRatio slice、嵌套 svg 继承祖先变换
static void TestSvgFinish() {
    auto compile = [](const std::string& body) {
        const std::string html =
            "<app_window width=\"120\" height=\"120\"><html width=\"120\" height=\"120\" bg=\"#000000\">"
            "<body>" + body + "</body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        r.loadLayout(res.layout.data(), res.layout.size());
        return std::make_pair(res.warnings, r);
    };

    // ---- polygon / polyline 编成路径 ----
    {
        auto [warns, r] = compile(
            "<svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
            "<polygon points=\"12 2 22 22 2 22\" fill=\"#F2B544\"/>"
            "<polyline points=\"2 20 8 6 14 16 20 4\" fill=\"none\" stroke=\"#7AAAFF\" stroke-width=\"2\"/>"
            "<polygon points=\"2 2 6 2 6 6\"/>"
            "</svg>");
        for (const auto& s : warns) std::printf("  [svgpath][warn] %s\n", s.c_str());
        int paths = 0, lines = 0;
        for (const auto& n : r.getNodes()) {
            if (n.type == BINUI_PATH) paths++;
            if (n.type == BINUI_LINE) lines++;
        }
        std::printf("  [svgpath] polygon/polyline：PATH %d 个，LINE %d 个\n", paths, lines);
        CHECK(paths == 2, "polygon/polyline：有颜料的编成路径（裸 polygon 不产生节点）");
        CHECK(lines == 0, "polygon/polyline：不再拆成线段节点");
        CHECK(warns.empty(), "polygon/polyline：不报警告（渐变/填充能力已具备）");
        bool hasFillPath = false, hasStrokeGradOrStroke = false;
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            const auto* pr = r.pathOf(n.id);
            if (!pr) continue;
            if (pr->head.flags & PATH_FILL) hasFillPath = true;
            if (pr->head.flags & PATH_STROKE) hasStrokeGradOrStroke = true;
        }
        CHECK(hasFillPath, "polygon：fill 走路径填充");
        CHECK(hasStrokeGradOrStroke, "polyline：stroke 走路径描边");
    }

    // ---- <line>：普通线仍是 LINE，需要路径能力时升级 ----
    {
        auto [warns, r] = compile(
            "<svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
            "<line x1=\"2\" y1=\"4\" x2=\"22\" y2=\"4\" stroke=\"#4ED17E\" stroke-width=\"2\"/>"
            "<line x1=\"2\" y1=\"12\" x2=\"22\" y2=\"12\" stroke=\"#F25A5A\" stroke-width=\"2\" "
            "stroke-dasharray=\"3 2\"/>"
            "</svg>");
        for (const auto& s : warns) std::printf("  [svgpath][warn] %s\n", s.c_str());
        int lines = 0, paths = 0;
        for (const auto& n : r.getNodes()) {
            if (n.type == BINUI_LINE) lines++;
            if (n.type == BINUI_PATH) paths++;
        }
        std::printf("  [svgpath] line：LINE %d 个，PATH %d 个（虚线那条升级）\n", lines, paths);
        CHECK(lines == 1, "line：普通线段仍走轻量 LINE 节点");
        CHECK(paths == 1, "line：写了 dasharray 升级成路径");
        CHECK(warns.empty(), "line：升级后不再告警");
    }

    // ---- text-anchor：同一个 x，三种锚点给出不同盒子左边 ----
    {
        auto [warns, r] = compile(
            "<svg x=\"0\" y=\"0\" width=\"120\" height=\"120\" viewBox=\"0 0 120 120\">"
            "<text x=\"60\" y=\"40\" font-size=\"12\" fill=\"#FFFFFF\">AAAAAAAA</text>"
            "<text x=\"60\" y=\"70\" font-size=\"12\" fill=\"#FFFFFF\" text-anchor=\"middle\">AAAAAAAA</text>"
            "<text x=\"60\" y=\"100\" font-size=\"12\" fill=\"#FFFFFF\" text-anchor=\"end\">AAAAAAAA</text>"
            "</svg>");
        for (const auto& s : warns) std::printf("  [svgpath][warn] %s\n", s.c_str());
        std::vector<const BinUIRenderer::NodeInfo*> labels;
        for (const auto& n : r.getNodes())
            if (n.type == BINUI_LABEL && !n.text.empty()) labels.push_back(&n);
        CHECK(labels.size() == 3, "text-anchor：三个标签都编出来");
        if (labels.size() == 3) {
            std::printf("  [svgpath] text-anchor：start x=%.1f  middle x=%.1f  end x=%.1f（宽 %.1f）\n",
                        (double)labels[0]->x, (double)labels[1]->x, (double)labels[2]->x,
                        (double)labels[0]->width);
            CHECK(labels[0]->x > labels[1]->x && labels[1]->x > labels[2]->x,
                  "text-anchor：end < middle < start 的盒子左边依次左移");
            const float halfW = labels[0]->width * 0.5f;
            CHECK_NEAR(labels[1]->x, labels[0]->x - halfW, labels[0]->width * 0.6f,
                       "text-anchor=middle：盒子左移约半个文本宽");
        }
        CHECK(warns.empty(), "text-anchor：不报警告");
    }

    // ---- userSpaceOnUse 下的百分比按视口尺寸折算（v19）----
    {
        auto [warns, r] = compile(
            "<svg x=\"0\" y=\"0\" width=\"200\" height=\"100\" viewBox=\"0 0 100 50\">"
            "<defs><linearGradient id=\"gp\" gradientUnits=\"userSpaceOnUse\" "
            "x1=\"0%\" y1=\"0%\" x2=\"100%\" y2=\"100%\">"
            "<stop offset=\"0\" stop-color=\"#FF0000\"/><stop offset=\"1\" stop-color=\"#0000FF\"/>"
            "</linearGradient></defs>"
            "<path d=\"M0 0 H100 V50 H0 Z\" fill=\"url(#gp)\"/>"
            "</svg>");
        for (const auto& s : warns) std::printf("  [svgpath][warn] %s\n", s.c_str());
        CHECK(warns.empty(), "userSpace 百分比：不再告警（已按视口比例折算）");
        const BinUIRenderer::PathRecord* pr = nullptr;
        for (const auto& n : r.getNodes())
            if (n.type == BINUI_PATH) { pr = r.pathOf(n.id); break; }
        CHECK(pr && pr->hasGrad, "userSpace 百分比：渐变记录写出");
        if (pr && pr->hasGrad) {
            std::printf("  [svgpath] userSpace 百分比：p1=(%.3f,%.3f) p2=(%.3f,%.3f)\n",
                        (double)pr->grad.x1, (double)pr->grad.y1,
                        (double)pr->grad.x2, (double)pr->grad.y2);
            CHECK_NEAR(pr->grad.x1, 0.0f, 0.02f, "userSpace 百分比：x1=0% → 盒左");
            CHECK_NEAR(pr->grad.y1, 0.0f, 0.02f, "userSpace 百分比：y1=0% → 盒顶");
            CHECK_NEAR(pr->grad.x2, 1.0f, 0.02f, "userSpace 百分比：x2=100% → 盒右");
            CHECK_NEAR(pr->grad.y2, 1.0f, 0.02f, "userSpace 百分比：y2=100% → 盒底");
        }
    }

    // ---- 嵌套 svg 继承祖先 transform ----
    {
        auto [w1, r1] = compile(
            "<svg x=\"0\" y=\"0\" width=\"120\" height=\"120\" viewBox=\"0 0 120 120\">"
            "<g transform=\"translate(10 10) scale(0.5)\">"
            "<svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 10 10\">"
            "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#7AAAFF\"/>"
            "</svg></g></svg>");
        auto [w2, r2] = compile(
            "<svg x=\"0\" y=\"0\" width=\"120\" height=\"120\" viewBox=\"0 0 120 120\">"
            "<svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 10 10\">"
            "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#7AAAFF\"/>"
            "</svg></svg>");
        for (const auto& s : w1) std::printf("  [svgpath][warn] %s\n", s.c_str());
        CHECK(w1.empty(), "嵌套 svg：不报警告");
        // 找那个方块（BINUI_LIST + 不透明底色；<rect> 没描边时走轻量映射）
        auto findRect = [](BinUIRenderer& r, float& x, float& w) {
            for (const auto& n : r.getNodes()) {
                if (n.type != BINUI_LIST) continue;
                if (((n.color >> 24) & 0xFF) == 0) continue;
                x = n.x;
                w = n.width;
                return;
            }
            x = w = -1.0f;
        };
        float x1 = 0, w1v = 0, x2 = 0, w2v = 0;
        findRect(r1, x1, w1v);
        findRect(r2, x2, w2v);
        std::printf("  [svgpath] 嵌套 svg：带 g(scale 0.5) 盒 = (%.1f,%.1f)  不带 = (%.1f,%.1f)\n",
                    (double)x1, (double)w1v, (double)x2, (double)w2v);
        CHECK(w1v > 0.0f && w2v > 0.0f, "嵌套 svg：两种写法都编出了方块");
        if (w1v > 0.0f && w2v > 0.0f) {
            CHECK(w1v < w2v * 0.75f, "嵌套 svg：<g scale> 包住时盒子跟着缩小");
            CHECK(x1 > x2 + 1.0f, "嵌套 svg：<g translate> 包住时盒子跟着平移");
        }
    }
}

// v19 条带缓存：命中就重放、换了 key 就重算、条带太多不缓存
static void TestPathBandCache() {
    PathShape sq;
    std::string err;
    ParsePathData("M0 0 H100 V100 H0 Z", 0.01f, sq, &err);
    DevicePath dev = ToDevice(sq);
    PathNormData nd;
    CHECK(NormalizeShape(sq, nd), "缓存：归一化成功");
    std::vector<uint32_t> cnt(nd.contourFirst.size());
    for (size_t i = 0; i < cnt.size(); i++)
        cnt[i] = (i + 1 < cnt.size() ? nd.contourFirst[i + 1] : nd.pts.size() / 2) -
                 nd.contourFirst[i];
    BinUIPathEntry head{};
    head.flags = PATH_FILL;
    head.fillColor = 0xFFFF0000u;
    head.designW = sq.Width();
    head.designH = sq.Height();
    head.pointCount = (uint16_t)(nd.pts.size() / 2);
    head.contourCount = (uint16_t)(nd.contourFirst.size());

    PathPaintSpec spec;
    spec.fillColor = 0xFFFF0000u;
    PathBandCacheEntry cache;
    PathRenderBuffers buf;
    uint32_t seenColor = 0;
    int nBands = 0;
    auto draw = [&](PathBandCacheEntry* c, uint32_t color) {
        spec.fillColor = color;
        return PaintPathNode(head, nd.pts.data(), nd.contourFirst.data(), cnt.data(),
                             nd.contourClosed.data(), (int)nd.contourFirst.size(),
                             0, 0, 100, 100, spec, c, 1, buf,
                             [&](const PaintBand* b, int n) { nBands = n; seenColor = b[0].color; },
                             [](float, float, float, float, float, uint32_t) {},
                             [](float, float, float, float, float, uint32_t) {});
    };

    draw(&cache, 0xFFFF0000u);
    std::printf("  [svgpath] 缓存：首次条带 %d 条，key=%llu\n", nBands,
                (unsigned long long)cache.key);
    CHECK(nBands == 1, "缓存：纯色方块 1 条带");
    CHECK(cache.key != 0, "缓存：首次计算后写入 key");
    CHECK(seenColor == 0xFFFF0000u, "缓存：首次颜色正确");

    // 篡改缓存内容：第二次若命中缓存，画出来的就是被篡改的颜色（证明"没重算"）
    cache.bands[0].color = 0xFF00FF00u;
    draw(&cache, 0xFFFF0000u);
    CHECK(seenColor == 0xFF00FF00u, "缓存：同 key 第二次直接重放缓存（没有重算）");

    // 换颜色 → key 变 → 必须重算，拿到真正的新颜色
    draw(&cache, 0xFF0000FFu);
    CHECK(seenColor == 0xFF0000FFu, "缓存：换颜色后 key 变了，重新计算");
    CHECK(cache.bands[0].color == 0xFF0000FFu, "缓存：重算结果写回缓存");

    // 换布局代数（relayout/loadLayout 会 ++gen）→ 整表作废
    draw(nullptr, 0xFF0000FFu);   // 不缓存时也必须能正常画
    CHECK(seenColor == 0xFF0000FFu, "缓存：传 nullptr 时不缓存但照常绘制");

    // 条带太多（> kMaxCachedBands）不缓存：换成一个会被截断的大图形
    {
        PathShape big;
        AppendEllipse(big, 200, 200, 200, 200, 0.01f);
        PathNormData nd2;
        if (NormalizeShape(big, nd2)) {
            std::vector<uint32_t> cnt2(nd2.contourFirst.size());
            for (size_t i = 0; i < cnt2.size(); i++)
                cnt2[i] = (i + 1 < cnt2.size() ? nd2.contourFirst[i + 1] : nd2.pts.size() / 2) -
                          nd2.contourFirst[i];
            BinUIPathEntry h2 = head;
            h2.pointCount = (uint16_t)(nd2.pts.size() / 2);
            h2.contourCount = (uint16_t)(nd2.contourFirst.size());
            PathBandCacheEntry c2;
            PathRenderBuffers b2;
            PathPaintSpec s2;
            s2.fillColor = 0xFF00FF00u;
            int nb = 0;
            PaintPathNode(h2, nd2.pts.data(), nd2.contourFirst.data(), cnt2.data(),
                          nd2.contourClosed.data(), (int)nd2.contourFirst.size(),
                          0, 0, 400, 400, s2, &c2, 1, b2,
                          [&](const PaintBand*, int n) { nb = n; },
                          [](float, float, float, float, float, uint32_t) {},
                          [](float, float, float, float, float, uint32_t) {});
            std::printf("  [svgpath] 缓存：400px 圆 %d 条带，缓存条目 key=%llu（0 = 未缓存）\n",
                        nb, (unsigned long long)c2.key);
            CHECK(nb > 0, "缓存：大图形仍然正常绘制");
            if (nb > kMaxCachedBands) CHECK(c2.key == 0, "缓存：条带超上限时不缓存（省内存）");
        }
    }
}

// v19 shape-rendering：auto/optimizeQuality 开 AA，crispEdges/optimizeSpeed 关，可继承
static void TestShapeRendering() {
    auto flagsOf = [](const std::string& svgAttrs, const std::string& pathAttrs,
                      std::vector<std::string>* warns) {
        const std::string html =
            "<app_window width=\"100\" height=\"100\"><html width=\"100\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\" " +
            svgAttrs + ">"
            "<g " + pathAttrs + "><path d=\"M0 0 H100 V100 H0 Z\" fill=\"#FF0000\"/></g>"
            "</svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        if (warns) *warns = res.warnings;
        binui::BinUIRenderer r;
        r.loadLayout(res.layout.data(), res.layout.size());
        for (const auto& n : r.getNodes())
            if (n.type == BINUI_PATH) {
                const auto* pr = r.pathOf(n.id);
                if (pr) return static_cast<int>(pr->head.flags);
            }
        return -1;
    };
    std::vector<std::string> w;
    int f = flagsOf("shape-rendering=\"auto\"", "", &w);
    CHECK(w.empty(), "shape-rendering：auto 不报警告");
    CHECK(f > 0 && (f & PATH_AA), "shape-rendering=auto：<svg> 上的值被子元素继承 → PATH_AA");
    f = flagsOf("", "shape-rendering=\"optimizeQuality\"", &w);
    CHECK(f > 0 && (f & PATH_AA), "shape-rendering=optimizeQuality：<g> 上同样生效");
    f = flagsOf("shape-rendering=\"crispEdges\"", "", &w);
    CHECK(f > 0 && !(f & PATH_AA), "shape-rendering=crispEdges：不开 AA");
    f = flagsOf("", "", &w);
    CHECK(f > 0 && !(f & PATH_AA), "shape-rendering：没写就是硬边（默认不变）");
    f = flagsOf("shape-rendering=\"optimizeSpeed\"", "", &w);
    CHECK(f > 0 && !(f & PATH_AA), "shape-rendering=optimizeSpeed：不开 AA");
    flagsOf("shape-rendering=\"blurry\"", "", &w);
    bool warned = false;
    for (const auto& x : w) {
        std::printf("  [svgpath][warn] %s\n", x.c_str());
        if (x.find("shape-rendering") != std::string::npos) warned = true;
    }
    CHECK(warned, "shape-rendering：不认识的值要告警");
}

// v19 边缘抗锯齿：覆盖率条带（部分 alpha）+ 面积守恒 + 太宽时退回硬边
static void TestPathAA() {
    PathShape tri;
    std::string err;
    ParsePathData("M0 0 L100 0 L0 100 Z", 0.01f, tri, &err);
    DevicePath dev = ToDevice(tri);
    // 设备盒 100x100：解析面积 5000
    PathShade shade;
    shade.color = 0xFFFF0000u;
    PaintBand bands[kMaxPathBands];
    std::vector<uint16_t> covAcc;
    bool trunc = false;

    auto areaOf = [&](int n) {
        double a = 0.0;
        for (int i = 0; i < n; i++)
            a += (double)bands[i].w * (double)bands[i].h *
                 (double)((bands[i].color >> 24) & 0xFF) / 255.0;
        return a;
    };
    int partial = 0;

    const int n = BuildPathFillBandsAA(dev.view(), false, 0, 0, 100, 100, shade, bands,
                                       kMaxPathBands, covAcc, &trunc);
    CHECK(n > 0 && !trunc, "AA：三角形能画完");
    for (int i = 0; i < n; i++)
        if (((bands[i].color >> 24) & 0xFF) < 255) partial++;
    std::printf("  [svgpath] AA：条带 %d，其中半透明边缘 %d 条，覆盖面积 %.1f（理论 5000）\n",
                n, partial, areaOf(n));
    CHECK(partial > 0, "AA：边缘出现了部分覆盖的像素（alpha < 255）");
    CHECK_NEAR(areaOf(n), 5000.0, 60.0, "AA：alpha 加权面积与解析解一致");

    // 硬边：不应有半透明像素，面积误差在行取整范围内
    const int nh = BuildPathFillBands(dev.view(), false, 0, 0, 100, 100, 0xFFFF0000u, bands,
                                      kMaxPathBands, 1, &trunc);
    partial = 0;
    for (int i = 0; i < nh; i++)
        if (((bands[i].color >> 24) & 0xFF) < 255) partial++;
    std::printf("  [svgpath] 硬边：条带 %d，半透明 %d 条，面积 %.1f\n", nh, partial, areaOf(nh));
    CHECK(partial == 0, "硬边：没有半透明像素");
    CHECK_NEAR(areaOf(nh), 5000.0, 200.0, "硬边：面积误差在行取整范围内");

    // 太宽（> kMaxPathAAWidth）时必须报 -1，让调用方退回硬边
    const int tooWide = BuildPathFillBandsAA(dev.view(), false, 0, 0, 5000, 100, shade, bands,
                                             kMaxPathBands, covAcc, &trunc);
    CHECK(tooWide == -1, "AA：超出宽度上限时返回 -1（调用方退回硬边）");
}

static void TestPathHitTest() {
    // 一个 20x20 的环（evenodd 挖洞），放在 100x100 窗口里
    const std::string html =
        "<app_window width=\"100\" height=\"100\"><html width=\"100\" height=\"100\" bg=\"#000000\">"
        "<head><style> .hit:hover { background: #333333; } </style></head>"
        "<body><svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
        "<path class=\"hit\" d=\"M2 2 H22 V22 H2 Z M8 8 H16 V16 H8 Z\" "
        "fill=\"#7AAAFF\" fill-rule=\"evenodd\"/></svg></body></html></app_window>";
    auto res = binui::CompileBinUIEx(html);
    for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
    BinUIRenderer r;
    CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "命中：布局可解析");
    uint16_t pathId = 0xFFFF;
    for (const auto& n : r.getNodes())
        if (n.type == BINUI_PATH) pathId = n.id;
    CHECK(pathId != 0xFFFF, "命中：找到路径节点");
    if (pathId == 0xFFFF) return;
    // 盒 = (8.3,8.3 83.3x83.3)：环带在 x∈[8.3,33.3]，洞在中间
    const int inRing = r.hitTestEx(20.0f, 50.0f, true, BINUI_ST_HOVER);
    const int inHole = r.hitTestEx(50.0f, 50.0f, true, BINUI_ST_HOVER);
    std::printf("  [svgpath] 命中：环带 = %d，洞心 = %d（节点 %u）\n", inRing, inHole,
                (unsigned)pathId);
    CHECK(inRing == pathId, "命中：环带上的点命中路径");
    CHECK(inHole == -1, "命中：环的洞里不再命中（精确命中生效）");
    // 盒外仍然是 -1（包围盒粗筛没坏）
    CHECK(r.hitTestEx(2.0f, 2.0f, true, BINUI_ST_HOVER) == -1, "命中：盒外不命中");
}

static void TestLineJoin() {
    const std::string starD =
        "M12 2 L15.09 8.26 L22 9.27 L17 14.14 L18.18 21.02 L12 17.77 "
        "L5.82 21.02 L7 14.14 L2 9.27 L8.91 8.26 Z";
    auto compile = [&](const std::string& attrs, std::vector<std::string>* warns) {
        const std::string html =
            "<app_window width=\"96\" height=\"96\"><html width=\"96\" height=\"96\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"96\" height=\"96\" viewBox=\"0 0 24 24\">"
            "<path d=\"" + starD + "\" fill=\"none\" stroke=\"#F2B544\" stroke-width=\"2\" " +
            attrs + "/></svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        if (warns) *warns = res.warnings;
        BinUIRenderer r;
        if (!r.loadLayout(res.layout.data(), res.layout.size())) return BinUIRenderer::PathRecord{};
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            const auto* pr = r.pathOf(n.id);
            if (pr) return *pr;
        }
        return BinUIRenderer::PathRecord{};
    };

    std::vector<std::string> w;
    const auto roundRec = compile("stroke-linejoin=\"round\"", &w);
    CHECK(w.empty(), "连接：round 不报警告");
    CHECK(roundRec.head.join == BINUI_JOIN_ROUND, "连接：round 落库");
    const auto miterRec = compile("stroke-linejoin=\"miter\" stroke-miterlimit=\"2\"", &w);
    CHECK(w.empty(), "连接：miter + miterlimit 不报警告");
    CHECK(miterRec.head.join == BINUI_JOIN_MITER, "连接：miter 落库");
    CHECK(miterRec.head.reserve == 2, "连接：stroke-miterlimit 落库");
    const auto bevelRec = compile("stroke-linejoin=\"bevel\"", &w);
    CHECK(w.empty(), "连接：bevel 不报警告");
    CHECK(bevelRec.head.join == BINUI_JOIN_BEVEL, "连接：bevel 落库");
    CHECK(bevelRec.head.reserve == 0, "连接：没写 miterlimit 时留 0（运行期按默认 4）");

    std::printf("  [svgpath] 连接盒：round=%.2fx%.2f  miter=%.2fx%.2f  bevel=%.2fx%.2f\n",
                (double)roundRec.head.designW, (double)roundRec.head.designH,
                (double)miterRec.head.designW, (double)miterRec.head.designH,
                (double)bevelRec.head.designW, (double)bevelRec.head.designH);
    // 五角星尖角的斜接比 ≈ 1/sin(18°) ≈ 3.24；limit=2 → 按斜面画，盒子不该被撑开
    CHECK_NEAR(miterRec.head.designW, roundRec.head.designW, 0.02f,
               "连接：miterlimit=2 时尖角超限按斜面画，盒子与圆角一致");
    // limit 给足 → 斜接尖角伸出几何盒，节点盒必须跟着变大，否则后端会把它切平
    const auto miterBig = compile("stroke-linejoin=\"miter\" stroke-miterlimit=\"10\"", &w);
    CHECK(w.empty(), "连接：miterlimit=10 不报警告");
    std::printf("  [svgpath] 斜接撑盒：round=%.2fx%.2f  miter(limit=10)=%.2fx%.2f\n",
                (double)roundRec.head.designW, (double)roundRec.head.designH,
                (double)miterBig.head.designW, (double)miterBig.head.designH);
    CHECK(miterBig.head.designW > roundRec.head.designW + 0.5f,
          "连接：斜接尖角把节点盒撑宽（不会被自己节点的裁剪矩形切平）");
    CHECK(miterBig.head.designH > roundRec.head.designH + 0.5f,
          "连接：斜接尖角把节点盒撑高");

    // 认不出的值：明确告警，按圆角画
    compile("stroke-linejoin=\"diagonal\"", &w);
    bool warned = false;
    for (const auto& s : w) { std::printf("  [svgpath][warn] %s\n", s.c_str());
        if (s.find("stroke-linejoin") != std::string::npos) warned = true; }
    CHECK(warned, "连接：不认识的值要告警");
    // v17：square 线帽实现了，落库 + 不报警告
    const auto capSq = compile("stroke-linecap=\"square\"", &w);
    CHECK(w.empty(), "连接：linecap=square 不再报警告");
    CHECK(capSq.head.cap == BINUI_CAP_SQUARE, "连接：linecap=square 落库");
}

static void TestGradientCompile() {
    // <defs><linearGradient> + fill="url(#…)"：默认 objectBoundingBox 单位 → 归一化端点 0,0→1,0
    {
        std::string html =
            "<app_window width=\"100\" height=\"100\"><html width=\"100\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
            "<defs><linearGradient id=\"g\">"
            "<stop offset=\"0\" stop-color=\"#ff0000\"/><stop offset=\"1\" stop-color=\"#0000ff\"/>"
            "</linearGradient></defs>"
            "<path d=\"M2 2 H22 V22 H2 Z\" fill=\"url(#g)\"/>"
            "</svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
        CHECK(res.warnings.empty(), "渐变编译：默认渐变不报警告");
        BinUIRenderer r;
        CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "渐变编译：布局可解析");
        const BinUIRenderer::PathRecord* pr = nullptr;
        for (const auto& n : r.getNodes())
            if (n.type == BINUI_PATH) { pr = r.pathOf(n.id); break; }
        CHECK(pr != nullptr, "渐变编译：编成 PATH 节点");
        if (pr) {
            std::printf("  [svgpath] 渐变编译：flags=%u hasGrad=%d type=%u stops=%u "
                        "p1=(%.3f,%.3f) p2=(%.3f,%.3f) 首色=%08X 末色=%08X\n",
                        (unsigned)pr->head.flags, (int)pr->hasGrad, (unsigned)pr->grad.type,
                        (unsigned)pr->grad.stopCount, (double)pr->grad.x1, (double)pr->grad.y1,
                        (double)pr->grad.x2, (double)pr->grad.y2, pr->grad.stops[0].color,
                        pr->grad.stops[1].color);
            CHECK((pr->head.flags & PATH_FILL) && (pr->head.flags & PATH_FILL_GRAD),
                  "渐变编译：PATH_FILL + PATH_FILL_GRAD 都置位");
            CHECK(pr->hasGrad, "渐变编译：渐变记录随路径段写出");
            CHECK(pr->grad.type == PATH_GRAD_LINEAR, "渐变编译：类型 = 线性");
            CHECK(pr->grad.stopCount == 2, "渐变编译：两个色标");
            CHECK_NEAR(pr->grad.x1, 0.0f, 0.01f, "渐变编译：x1 默认 0");
            CHECK_NEAR(pr->grad.y1, 0.0f, 0.01f, "渐变编译：y1 默认 0");
            CHECK_NEAR(pr->grad.x2, 1.0f, 0.01f, "渐变编译：x2 默认 100%");
            CHECK_NEAR(pr->grad.y2, 0.0f, 0.01f, "渐变编译：y2 默认 0");
            CHECK(pr->grad.stops[0].color == 0xFFFF0000u, "渐变编译：首标红");
            CHECK(pr->grad.stops[1].color == 0xFF0000FFu, "渐变编译：末标蓝");
            CHECK_NEAR(pr->grad.stops[0].offset, 0.0f, 1e-4, "渐变编译：首标 offset=0");
            CHECK_NEAR(pr->grad.stops[1].offset, 1.0f, 1e-4, "渐变编译：末标 offset=1");
        }
    }

    // gradientUnits="userSpaceOnUse" + gradientTransform：端点按用户空间 → 节点归一化折算
    {
        std::string html =
            "<app_window width=\"100\" height=\"100\"><html width=\"100\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
            "<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" "
            "x1=\"0\" y1=\"0\" x2=\"24\" y2=\"0\">"
            "<stop offset=\"0\" stop-color=\"#00ff00\"/><stop offset=\"1\" stop-color=\"#ff00ff\"/>"
            "</linearGradient></defs>"
            "<path d=\"M2 2 H22 V22 H2 Z\" fill=\"url(#g)\"/>"
            "</svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        CHECK(res.warnings.empty(), "渐变编译：userSpaceOnUse 不报警告");
        BinUIRenderer r;
        CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "渐变编译：user 布局可解析");
        const BinUIRenderer::PathRecord* pr = nullptr;
        for (const auto& n : r.getNodes())
            if (n.type == BINUI_PATH) { pr = r.pathOf(n.id); break; }
        if (pr) {
            std::printf("  [svgpath] userSpaceOnUse：p1=(%.3f,%.3f) p2=(%.3f,%.3f)\n",
                        (double)pr->grad.x1, (double)pr->grad.y1, (double)pr->grad.x2,
                        (double)pr->grad.y2);
            CHECK(pr->hasGrad, "渐变编译：user 记录写出");
            // 用户空间 0..24 映射到设备 0..100，节点盒是 (8.33,8.33,83.33,83.33)
            CHECK_NEAR(pr->grad.x1, -0.1f, 0.02f, "渐变编译：user 起点折到盒外 -0.1");
            CHECK_NEAR(pr->grad.x2, 1.1f, 0.02f, "渐变编译：user 终点折到盒外 1.1");
        }
    }

    // href 继承色标 + 径向默认 50%
    {
        std::string html =
            "<app_window width=\"100\" height=\"100\"><html width=\"100\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
            "<defs><linearGradient id=\"a\">"
            "<stop offset=\"0\" stop-color=\"#ffffff\"/><stop offset=\"1\" stop-color=\"#000000\"/>"
            "</linearGradient>"
            "<linearGradient id=\"b\" href=\"#a\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\"/></defs>"
            "<path d=\"M2 2 H22 V22 H2 Z\" fill=\"url(#b)\"/>"
            "<radialGradient id=\"c\"><stop offset=\"0\" stop-color=\"#ff0000\" stop-opacity=\"0.5\"/>"
            "<stop offset=\"1\" stop-color=\"#0000ff\"/></radialGradient>"
            "<path d=\"M2 12 H10 V20 H2 Z\" fill=\"url(#c)\"/>"
            "</svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        for (const auto& w : res.warnings) std::printf("  [svgpath][warn] %s\n", w.c_str());
        CHECK(res.warnings.empty(), "渐变编译：href 继承不报警告");
        BinUIRenderer r;
        CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "渐变编译：继承布局可解析");
        int linearN = 0, radialN = 0;
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            const auto* pr = r.pathOf(n.id);
            if (!pr || !pr->hasGrad) continue;
            if (pr->grad.type == PATH_GRAD_LINEAR) {
                linearN++;
                CHECK(pr->grad.stopCount == 2, "渐变编译：href 继承到两个色标");
                CHECK_NEAR(pr->grad.x2, 0.0f, 0.01f, "渐变编译：href 子元素自己的 x2 生效");
                CHECK_NEAR(pr->grad.y2, 1.0f, 0.01f, "渐变编译：href 子元素自己的 y2 生效");
            } else if (pr->grad.type == PATH_GRAD_RADIAL) {
                radialN++;
                std::printf("  [svgpath] 径向：c=(%.3f,%.3f) r=(%.3f,%.3f) 首色=%08X\n",
                            (double)pr->grad.x1, (double)pr->grad.y1, (double)pr->grad.rx,
                            (double)pr->grad.ry, pr->grad.stops[0].color);
                CHECK_NEAR(pr->grad.x1, 0.5f, 0.02f, "渐变编译：径向圆心默认 50%");
                CHECK_NEAR(pr->grad.rx, 0.5f, 0.02f, "渐变编译：径向 r 默认 50%");
                CHECK_NEAR(pr->grad.ry, 0.5f, 0.02f, "渐变编译：径向 ry 默认 50%");
                CHECK(((pr->grad.stops[0].color >> 24) & 0xFF) < 0xC0,
                      "渐变编译：stop-opacity 折进色标 alpha");
            }
        }
        CHECK(linearN == 1, "渐变编译：线性渐变的引用被解析");
        CHECK(radialN == 1, "渐变编译：径向渐变的引用被解析");
    }

    // 引用了不存在的渐变：必须告警，并按纯色画（不许静默）
    {
        std::string html =
            "<app_window width=\"100\" height=\"100\"><html width=\"100\" height=\"100\" bg=\"#000000\">"
            "<body><svg x=\"0\" y=\"0\" width=\"100\" height=\"100\" viewBox=\"0 0 24 24\">"
            "<path d=\"M2 2 H22 V22 H2 Z\" fill=\"url(#nope)\"/>"
            "</svg></body></html></app_window>";
        auto res = binui::CompileBinUIEx(html);
        bool warned = false;
        for (const auto& w : res.warnings) {
            std::printf("  [svgpath][warn] %s\n", w.c_str());
            if (w.find("nope") != std::string::npos) warned = true;
        }
        CHECK(warned, "渐变编译：找不到渐变定义要告警");
        BinUIRenderer r;
        CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "渐变编译：兜底布局可解析");
        for (const auto& n : r.getNodes()) {
            if (n.type != BINUI_PATH) continue;
            const auto* pr = r.pathOf(n.id);
            if (pr) CHECK(!pr->hasGrad, "渐变编译：找不到定义时不写渐变记录");
        }
    }
}

static int run() {
    std::printf("\n[suite_svgpath] 路径几何（binui_path.h）+ 扫描线填充（binui_paint.h）\n");
    TestParse();
    TestFill();
    TestStroke();
    TestCompileAndLoad();
    TestSvgSemantics();
    TestShapesAndDash();
    TestUseDefs();
    TestSvgTextImage();
    TestGradientBands();
    TestGradientCompile();
    TestLineJoin();
    TestPathHitTest();
    TestPathAA();
    TestShapeRendering();
    TestPathBandCache();
    TestSvgFinish();
    std::printf("[suite_svgpath] %s\n", g_fail ? "FAILED" : "全部通过");
    return g_fail;
}

#undef CHECK
#undef CHECK_NEAR

}  // namespace suite_svgpath

// ===========================================================================
// 汇总入口：任一子套件有失败则整体失败
// ===========================================================================
int main() {
    int fails = 0;
    fails += suite_binui::run();
    fails += suite_renderer::run();
    fails += suite_renderer_real::run();
    fails += suite_paint::run();
    fails += suite_anim::run();
    fails += suite_hot_reload::run();
    fails += suite_hover_dirty::run();
    fails += suite_clip_chain::run();
    fails += suite_svgpath::run();
    std::printf("\n=== test_elements: %s ===\n", fails ? "FAILED" : "全部通过");
    return fails ? 1 : 0;
}
