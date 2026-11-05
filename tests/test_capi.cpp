// test_capi.cpp - C ABI（include/binui_c.h）冒烟测试
//
// 只测"不需要窗口"的部分：编译、加载、节点查询、改文本、错误路径。
// 开窗 + OpenGL 需要桌面环境，不进 CI；交互行为由各语言绑定的 demo 覆盖。
//
// 这一组用例的真正价值在于**守住 ABI**：结构体字段顺序/类型、返回值语义、
// 两段式取文本的边界，任何一处改动破坏契约，这里会先炸，而不是等 Python
// ctypes 读到一半内存错位。
#include "binui_c.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); \
        failed++; \
    } else { \
        std::printf("PASS: %s\n", name); \
    } \
} while (0)

namespace {

const char* kHtml = R"HTML(
<html><body>
  <div id="card" style="width:360px;height:220px">
    <span id="title" style="font-size:20px;color:#1F2A37">C ABI</span>
    <button id="ok" style="width:120px;height:40px">确定</button>
    <input id="name" style="width:200px;height:36px" placeholder="请输入" />
  </div>
</body></html>
)HTML";

// 取节点文本（两段式，和宿主语言里要写的代码路径一致）
std::string NodeText(void* app, int32_t id) {
    const int32_t need = binui_node_text(app, id, nullptr, 0);
    if (need < 0) return std::string("<<no node>>");
    std::vector<char> buf(static_cast<size_t>(need) + 1, '\0');
    const int32_t got = binui_node_text(app, id, buf.data(), static_cast<int32_t>(buf.size()));
    if (got != need) return std::string("<<length mismatch>>");
    return std::string(buf.data(), static_cast<size_t>(got));
}

} // namespace

int main() {
    std::printf("=== test_capi: C ABI（无窗口）===\n\n");

    // ---- 1. 版本 / 错误串 ----
    TEST("abi version", binui_abi_version() == BINUI_C_ABI_VERSION);
    TEST("version string", binui_version() != nullptr && std::strlen(binui_version()) > 0);

    // ---- 2. 内存编译 ----
    void* layout = nullptr;
    int32_t layoutLen = 0;
    TEST("compile_memory ok",
         binui_compile_memory(kHtml, -1, &layout, &layoutLen) == 1);
    TEST("compile_memory non-empty", layout != nullptr && layoutLen > 0);

    void* empty = reinterpret_cast<void*>(0x1);
    int32_t emptyLen = 123;
    TEST("compile_memory rejects empty src",
         binui_compile_memory(nullptr, 0, &empty, &emptyLen) == 0 &&
         empty == nullptr && emptyLen == 0);
    TEST("compile_memory sets error", std::strlen(binui_last_error()) > 0);

    // ---- 3. 创建应用（不建窗）----
    TEST("create rejects null layout", binui_app_create(nullptr, 0, nullptr) == nullptr);

    std::vector<uint8_t> garbage(64, 0x7F);
    TEST("create rejects garbage",
         binui_app_create(garbage.data(), (int32_t)garbage.size(), nullptr) == nullptr);

    void* app = binui_app_create(layout, layoutLen, nullptr);
    TEST("create ok", app != nullptr);
    if (!app) {
        std::printf("\nC ABI 无法创建应用，后续用例跳过\n");
        binui_free(layout);
        return 1;
    }

    // ---- 4. 节点查询 ----
    const int32_t nodeCount = binui_node_count(app);
    TEST("node_count > 0", nodeCount > 0);

    BinUINode first;
    std::memset(&first, 0, sizeof(first));
    TEST("node_at(0)", binui_node_at(app, 0, &first) == 1);
    TEST("root is ROOT", first.type == BINUI_NODE_ROOT);
    TEST("node_at out of range", binui_node_at(app, nodeCount, &first) == 0);
    TEST("node_at negative index", binui_node_at(app, -1, &first) == 0);

    const int32_t idTitle = binui_find_node(app, "title");
    const int32_t idOk = binui_find_node(app, "ok");
    const int32_t idName = binui_find_node(app, "name");
    TEST("find title", idTitle >= 0);
    TEST("find ok", idOk >= 0);
    TEST("find name", idName >= 0);
    TEST("find missing -> -1", binui_find_node(app, "不存在的节点") == -1);
    TEST("find null -> -1", binui_find_node(app, nullptr) == -1);

    BinUINode node;
    std::memset(&node, 0, sizeof(node));
    TEST("node_by_id(title)", binui_node_by_id(app, idTitle, &node) == 1);
    TEST("title is LABEL", node.type == BINUI_NODE_LABEL);
    TEST("title width > 0", node.width > 0.0f);
    TEST("node_by_id(ok)", binui_node_by_id(app, idOk, &node) == 1);
    TEST("ok is BUTTON", node.type == BINUI_NODE_BUTTON);
    TEST("button interactive", (node.flags & BINUI_NF_INTERACTIVE) != 0);
    TEST("node_by_id(missing)", binui_node_by_id(app, 9999, &node) == 0);

    // 结构布局自检：字段偏移一旦被改，其他语言的绑定就会错位
    TEST("BinUINode size == 56", sizeof(BinUINode) == 56);
    TEST("BinUINode.id offset", offsetof(BinUINode, id) == 0);
    TEST("BinUINode.color offset", offsetof(BinUINode, color) == 32);
    TEST("BinUINode.z_index offset", offsetof(BinUINode, z_index) == 52);
    TEST("BinUIEvent.node_id offset", offsetof(BinUIEvent, node_id) == 4);
    TEST("BinUIEvent.text offset", offsetof(BinUIEvent, text) == 24);

    // ---- 5. 文本读取（两段式）----
    TEST("read title text", NodeText(app, idTitle) == "C ABI");
    TEST("read button text", NodeText(app, idOk) == "确定");
    TEST("node_text missing node -> -1", binui_node_text(app, 9999, nullptr, 0) == -1);

    // cap 不够时只返回长度、不写越界
    {
        char tiny[4] = { 'X', 'X', 'X', 'X' };
        const int32_t need = binui_node_text(app, idOk, tiny, 4);
        TEST("truncated read returns full length", need == 6);   // "确定" = 6 字节
        TEST("truncated read NUL-terminates", tiny[3] == '\0');
        TEST("truncated read keeps 3 bytes", std::memcmp(tiny, "确", 3) == 0);
    }

    // 输入框占位符：节点文本是提示语，真实值为空（v14 语义）
    TEST("placeholder text", NodeText(app, idName) == "请输入");

    // ---- 6. 修改 ----
    TEST("set_text ok", binui_set_text(app, idTitle, "改过了", -1) == 1);
    TEST("set_text visible", NodeText(app, idTitle) == "改过了");
    TEST("set_text by length",
         binui_set_text(app, idTitle, "ABCDEF", 3) == 1 && NodeText(app, idTitle) == "ABC");
    TEST("set_text missing node", binui_set_text(app, 9999, "x", -1) == 0);
    TEST("set_text null", binui_set_text(app, idTitle, nullptr, 0) == 1);
    TEST("set_text cleared", NodeText(app, idTitle).empty());

    TEST("set_visible ok", binui_set_visible(app, idTitle, 0) == 1);
    TEST("set_visible back", binui_set_visible(app, idTitle, 1) == 1);
    TEST("set_visible missing", binui_set_visible(app, 9999, 1) == 0);
    TEST("set_checked ok", binui_set_checked(app, idOk, 1) == 1);
    TEST("set_checked missing", binui_set_checked(app, 9999, 1) == 0);

    // ---- 7. 未开窗时的安全行为 ----
    TEST("pump without window -> closed", binui_pump(app, 0) == 1);
    TEST("app_size without window -> 0", binui_app_size(app, nullptr, nullptr) == 0);
    TEST("invalidate without window ok", binui_invalidate_node(app, idTitle) == 1);
    TEST("invalidate missing", binui_invalidate_node(app, 9999) == 0);
    binui_invalidate_all(app);      // 不崩即可
    binui_request_exit(app);        // 不崩即可

    BinUIEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    TEST("poll empty queue", binui_poll_event(app, &ev) == 0);
    TEST("poll null out", binui_poll_event(app, nullptr) == 0);

    // ---- 7.5 子窗口模式：只测参数校验与未开窗行为（真建窗需要宿主 HWND，不进 CI）----
    TEST("open_child rejects null parent",
         binui_app_open_child(app, nullptr, 0, 0, 200, 120, 0) == 0);
    TEST("open_child null parent sets error", std::strlen(binui_last_error()) > 0);
    TEST("open_child rejects zero size",
         binui_app_open_child(app, reinterpret_cast<void*>(0x1), 0, 0, 0, 120, 0) == 0);
    TEST("open_child rejects negative size",
         binui_app_open_child(app, reinterpret_cast<void*>(0x1), 0, 0, 200, -1, 0) == 0);
    TEST("app_hwnd(null)", binui_app_hwnd(nullptr) == nullptr);
    TEST("app_hwnd before open", binui_app_hwnd(app) == nullptr);
    TEST("render before open -> closed", binui_render(app) == 1);
    binui_resize(app, 320, 200);   // 未开窗：忽略，不崩
    binui_resize(app, 0, 0);
    binui_resize(nullptr, 100, 100);

    // ---- 7.6 运行期能力（docs/issues/07）：滚动 / 节点几何 / 动态节点 / 滚动条 ----
    {
        // 内容高度：没有可滚动的子节点时也是"下界 - 容器顶"，只要节点存在就是 >= 0
        TEST("content_height(title) >= 0", binui_content_height(app, idTitle) >= 0.0f);
        TEST("content_height missing -> -1", binui_content_height(app, 9999) == -1.0f);
        TEST("content_height(null) -> -1", binui_content_height(nullptr, 0) == -1.0f);

        // scroll_to / scroll_by：未开窗时只改状态，不崩即可
        binui_scroll_to(app, idTitle, 0, 10);
        binui_scroll_by(app, idTitle, 0, -5);
        binui_scroll_to(nullptr, 0, 1, 1);
        TEST("scroll_* 不崩", true);

        // set_node_bounds：合法改成功、非法拒绝
        BinUINode before;
        std::memset(&before, 0, sizeof(before));
        binui_node_by_id(app, idTitle, &before);
        TEST("set_node_bounds ok", binui_set_node_bounds(app, idTitle, 12, 34, 200, 44) == 1);
        BinUINode after;
        std::memset(&after, 0, sizeof(after));
        binui_node_by_id(app, idTitle, &after);
        TEST("set_node_bounds 生效", after.x == 12.0f && after.width == 200.0f && after.height == 44.0f);
        TEST("set_node_bounds 负宽度拒绝", binui_set_node_bounds(app, idTitle, 0, 0, -1, 10) == 0);
        TEST("set_node_bounds 无此节点", binui_set_node_bounds(app, 9999, 0, 0, 10, 10) == 0);
        TEST("set_node_bounds(null)", binui_set_node_bounds(nullptr, 0, 0, 0, 10, 10) == 0);

        // add_node / remove_node：列表/日志流的官方出口
        const int32_t newId = binui_add_node(app, 0, BINUI_NODE_LABEL, 10, 10, 100, 20,
                                             0xFF102030u, 4, 13);
        TEST("add_node 返回新 id", newId > 0);
        TEST("add_node 父节点不存在", binui_add_node(app, 9999, BINUI_NODE_LABEL, 0, 0, 10, 10,
                                                     0, 0, 0) == -1);
        TEST("add_node(null) == -1", binui_add_node(nullptr, 0, 0, 0, 0, 10, 10, 0, 0, 0) == -1);
        BinUINode added;
        std::memset(&added, 0, sizeof(added));
        TEST("新增节点可查回", binui_node_by_id(app, newId, &added) == 1 &&
                                added.color == 0xFF102030u && added.radius == 4);
        TEST("set_text 作用于新增节点", binui_set_text(app, newId, "动态行", -1) == 1);
        TEST("remove_node ok", binui_remove_node(app, newId) == 1);
        BinUINode gone;
        std::memset(&gone, 0, sizeof(gone));
        TEST("remove_node 只是隐藏（节点保留供复用）",
             binui_node_by_id(app, newId, &gone) == 1 && (gone.flags & BINUI_NF_VISIBLE) == 0);
        TEST("remove_node 幂等（重复移除仍是 1）", binui_remove_node(app, newId) == 1);
        TEST("remove_node 无此节点", binui_remove_node(app, 9999) == 0);
        TEST("remove_node(null)", binui_remove_node(nullptr, 0) == 0);

        // 滚动条样式：设置/复位都不崩，NULL 安全
        TEST("set_scrollbar_style ok", binui_set_scrollbar_style(app, 0xFFE5E7EBu, 0xFFC7CBD1u, 10) == 1);
        TEST("set_scrollbar_style(null)", binui_set_scrollbar_style(nullptr, 0, 0, 8) == 0);
        binui_reset_scrollbar_style(app);
        binui_reset_scrollbar_style(nullptr);
        TEST("scrollbar 样式接口不崩", true);
    }

    // ---- 8. 空指针防护（绑定层最容易传错的地方）----
    TEST("node_count(null) == 0", binui_node_count(nullptr) == 0);
    TEST("hit_test(null) == -1", binui_hit_test(nullptr, 0, 0) == -1);
    TEST("find_node(null) == -1", binui_find_node(nullptr, "x") == -1);
    binui_app_destroy(nullptr);     // 不崩即可

    // ---- 9. 销毁 ----
    binui_app_destroy(app);
    binui_free(layout);
    binui_free(nullptr);            // 不崩即可
    TEST("survived destroy", true);

    std::printf("\n%s\n", failed == 0 ? "test_capi: ALL PASS" : "test_capi: FAILED");
    return failed == 0 ? 0 : 1;
}
