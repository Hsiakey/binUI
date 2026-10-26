#include "binui_renderer.h"
#include "binui_compiler.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include <thread>

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

int main() {
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

    printf("\n=== Results: %d tests failed ===\n", failed);
    return failed > 0 ? 1 : 0;
}
