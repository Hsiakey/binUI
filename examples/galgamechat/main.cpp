// galgamechat/main.cpp - binUI Galgame 式 Agent 前端原型
// 交互循环（与视觉小说一致）：
//   角色逐字说话 -> 按 Enter -> 弹出「你 + 输入框」-> 输入回车 ->
//   玩家这句与被顶上去的历史一起上滚进对话面板 -> 角色接着逐字回话。
//
// 实现要点（全部用现有原语）：
//   - 逐字打字机：每帧 setText(row, 前缀) 推进 UTF-8 码点。
//   - 上滚 backlog：对话面板是个 overflow:hidden 滚动容器，正文行用 addNode
//     追加为子节点，追加后 scrollTo 到底 -> 旧行被顶上去（offY 由后端统一应用）。
//   - 玩家回合：切换 <input> 的可见性并 focus()，字符经 setCharFn -> insertText。
//   - 后端钩子：agentReply 为预留接口；接真 LLM 时设置它（或改为异步后再调
//     startNpcLine 推送回复），默认走脚本台词。
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <functional>
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
    std::printf("用法: galgamechat [--watch] [--ui <main.html>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（对话状态会从头开始）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n");
}

static size_t Utf8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

static std::string Utf8Prefix(const std::string& s, size_t count) {
    size_t i = 0, seen = 0;
    while (i < s.size() && seen < count) {
        i++;
        while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
        seen++;
    }
    return s.substr(0, i);
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "galgamechat", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[galgamechat] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[galgamechat] Failed to load layout\n");
        return 1;
    }

    renderer.setImageOrder(res.images);
    renderer.loadResources(binui::ex::LoadImages(uiPath, res.images, "galgamechat", {"tests/"}));

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 960 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 680 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : false, 0,
                         wd.present && wd.transparent(), wd.title.c_str());

    uint16_t logId      = renderer.findNodeByName("log");
    uint16_t composerId = renderer.findNodeByName("composer");
    uint16_t inputId    = renderer.findNodeByName("chatInput");
    uint16_t hintId     = renderer.findNodeByName("hint");
    uint16_t btnMin     = renderer.findNodeByName("btnMin");
    uint16_t btnClose   = renderer.findNodeByName("btnClose");

    auto findNode = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
        for (const auto& n : renderer.getNodes()) if (n.id == id) return &n;
        return nullptr;
    };
        auto invalidateNode = [&](uint16_t id) {
        // 失效矩形必须含动画后的包围盒：hover 放大时只按布局矩形失效，
        // 长出来的那一圈（边框/底色）不会被重绘，看上去就是"边框被削掉一半"
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    const BinUIRenderer::NodeInfo* ln = findNode(logId);
    float logX = 0, logY = 0, logW = 0, logH = 0;
    if (ln) { logX = ln->x; logY = ln->y; logW = ln->width; logH = ln->height; }
    auto invalidateLog = [&]() { app.invalidateRect((int)logX, (int)logY, (int)logW, (int)logH); };

    // ---- 对话状态机 ----
    enum class Phase { TYPING, DONE, INPUT };
    Phase phase = Phase::TYPING;
    bool  started = false;
    float contentCur = 12.0f;          // log 内容累计高度（含顶部留白）
    uint16_t npcRow = 0;
    std::string npcFull;
    int    npcChars = 0;
    double typeAcc = 0.0;

    // 脚本台词（默认后端）。
    const std::vector<std::string> npcScript = {
        "……啊，你终于来了。我是诺艾拉，这个界面的向导。",
        "在这里，我们的对话像文字小说一样推进：你按 Enter，我就继续说。",
        "试试在下面输入点什么吧——我会照着你的话回应。",
        "（她歪了歪头）……哦？就这些？",
        "有意思。也许我们能把一个 Agent，整个装进一部视觉小说里。",
        "再来一句？我这边随时待命。",
    };
    int npcCursor = 0;

    // ---- 预留后端钩子：给用户输入，返回回复文本（接真 LLM 时设置它）----
    // 异步接法：在此发起请求，拿到结果后再调用 startNpcLine(回复)（见下方注释）。
    std::function<std::string(const std::string&)> agentReply;

    auto scrollBottom = [&]() {
        float ch = renderer.contentHeight(logId);
        if (ch > logH) renderer.scrollTo(logId, 0.0f, ch - logH);
    };

    // 追加一轮：名字（LABEL，着色）+ 正文（LIST，自动换行，白字）
    auto appendTurn = [&](const std::string& who, uint32_t whoColor,
                          const std::string& text) -> uint16_t {
        const float fMsg = 14.0f;                 // LIST 分支固定 14px
        int asc = 0, lh = 0;
        app.gfx().fontMetrics(fMsg, 400, asc, lh);
        float rowW   = logW - 36.0f;
        float availW = std::max(1.0f, rowW - 24.0f);
        float tw     = app.gfx().textWidth(fMsg, 400, text);
        int   lines  = std::max(1, (int)std::ceil(tw / availW));
        float msgH   = (float)lines * (float)lh + 10.0f;

        float nameY = logY + contentCur;
        float msgY  = nameY + 20.0f + 2.0f;

        uint16_t nameId = renderer.addNode(logId, BINUI_LABEL,
            logX + 18.0f, nameY, 200.0f, 20.0f, whoColor, 0, 13);
        renderer.setText(nameId, who);
        uint16_t msgId = renderer.addNode(logId, BINUI_LIST,
            logX + 18.0f, msgY, rowW, msgH, 0x00000000u, 0, 14);
        renderer.setText(msgId, text);

        contentCur += 20.0f + 2.0f + msgH + 12.0f;
        return msgId;
    };

    // 开始一段角色台词（逐字）。异步后端拿到回复后同样调用它。
    std::function<void(const std::string&)> startNpcLine;
    startNpcLine = [&](const std::string& text) {
        npcFull  = text;
        npcChars = 0;
        typeAcc  = 0.0;
        npcRow   = appendTurn("诺艾拉", 0xFFC4B5FD, text);
        renderer.setText(npcRow, "");
        renderer.setVisible(hintId, false);
        phase = Phase::TYPING;
        scrollBottom();
        app.invalidateAll();
    };

    auto openComposer = [&]() {
        phase = Phase::INPUT;
        renderer.setVisible(hintId, false);
        renderer.setText(inputId, "");
        renderer.setVisible(composerId, true);
        renderer.focus(inputId);
        app.invalidateAll();
    };

    auto enterAdvance = [&]() {
        if (phase == Phase::TYPING) {           // 第一次 Enter：立即补全当前句
            renderer.setText(npcRow, npcFull);
            phase = Phase::DONE;
            renderer.setVisible(hintId, true);
            invalidateLog();
        } else if (phase == Phase::DONE) {      // 第二次 Enter：轮到玩家
            openComposer();
        }
    };

    auto submit = [&]() {
        std::string msg = renderer.edit().value;
        size_t a = msg.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return;
        size_t b = msg.find_last_not_of(" \t\r\n");
        msg = msg.substr(a, b - a + 1);

        renderer.blur();
        renderer.setText(inputId, "");
        renderer.setVisible(composerId, false);

        appendTurn("你", 0xFF7BE0B0, msg);
        scrollBottom();

        std::string reply;
        if (agentReply) reply = agentReply(msg);
        else { reply = npcScript[npcCursor % npcScript.size()]; npcCursor++; }
        startNpcLine(reply);
    };

    // 初始：隐藏输入栏与提示
    renderer.setVisible(composerId, false);
    renderer.setVisible(hintId, false);

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) { drawNodesGLGdi(gfx, renderer, clip); });

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
        uint16_t nid = (uint16_t)id;
        if (nid == btnMin)    { app.minimize();    return; }
        if (nid == btnClose)  { app.requestExit(); return; }
        invalidateNode(nid);
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
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });

    app.setCharFn([&](const std::string& utf8) {
        if (!renderer.isEditing()) return;
        if (utf8.size() == 1 && (unsigned char)utf8[0] < 0x20) return; // 过滤控制字符
        renderer.insertText(utf8);
        invalidateNode(renderer.edit().nodeId);
    });

    app.setKeyFn([&](int vk) {
        if (vk == VK_ESCAPE) { app.requestExit(); return; }
        if (renderer.isEditing()) {
            if (vk == VK_RETURN) { submit(); return; }
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (vk == VK_BACK) renderer.backspace();
            else if (vk == VK_DELETE) renderer.deleteForward();
            else if (vk == VK_LEFT) renderer.moveCaret(-1, shift);
            else if (vk == VK_RIGHT) renderer.moveCaret(1, shift);
            else if (vk == VK_HOME) renderer.setCaret(0);
            else if (vk == VK_END) renderer.setCaret(renderer.edit().value.size());
            if (renderer.isEditing()) invalidateNode(renderer.edit().nodeId);
            return;
        }
        if (vk == VK_RETURN) enterAdvance();
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
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    app.setUpdateFn([&](float dt) {
        if (hot.Tick(renderer, "galgamechat", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            // 换树后动态对话行全部消失：重取节点 + 重新开始对话（dev 阶段的预期行为）
            logId      = renderer.findNodeByName("log");
            composerId = renderer.findNodeByName("composer");
            inputId    = renderer.findNodeByName("chatInput");
            hintId     = renderer.findNodeByName("hint");
            btnMin     = renderer.findNodeByName("btnMin");
            btnClose   = renderer.findNodeByName("btnClose");
            if (const auto* n = findNode(logId)) {
                logX = n->x; logY = n->y; logW = n->width; logH = n->height;
            }
            phase = Phase::TYPING;
            started = false;
            contentCur = 12.0f;
            npcRow = 0;
            npcFull.clear();
            npcChars = 0;
            typeAcc = 0.0;
            npcCursor = 0;
            app.invalidateAll();
        }
        if (!started) { started = true; startNpcLine(npcScript[0]); npcCursor = 1; }

        if (phase == Phase::TYPING) {
            typeAcc += (double)dt * 30.0;      // 约 30 字/秒
            int want  = (int)typeAcc;
            int total = (int)Utf8Len(npcFull);
            if (want >= total) {
                renderer.setText(npcRow, npcFull);
                renderer.setVisible(hintId, true);
                phase = Phase::DONE;
            } else {
                renderer.setText(npcRow, Utf8Prefix(npcFull, (size_t)want));
            }
            invalidateLog();
        }
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });

    std::printf("[galgamechat] binUI Galgame Chat 原型 (OpenGL 3.3) — Enter 推进/发送\n");
    return app.run();
}
