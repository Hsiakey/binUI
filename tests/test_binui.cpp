#include "binui_format.h"
#include "binui_renderer.h"
#include <cstdio>
#include <cstring>
#include <vector>

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

int main() {
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
