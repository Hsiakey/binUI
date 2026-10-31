// binui_c.cpp - C ABI 实现（对外声明见 include/binui_c.h）
//
// 这一层只做三件事：把 C++ 的类翻译成不透明句柄、把回调翻译成事件队列、
// 把常用流程（标题栏拖动 / 文本框编辑 / 悬停重绘 / 动画推进）预先接好 ——
// 宿主语言那边看到的应该只是"喂事件、拿事件、改文本"，不该看见渲染器。
#include "binui_c.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "binui_component.h"
#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"

namespace {

// ============================================================
// 错误串（线程局部）
// ============================================================
thread_local std::string g_err;

void SetErr(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_err = buf;
}

// ============================================================
// 应用对象
// ============================================================
struct QueuedEvent {
    BinUIEvent ev;
    std::string text;
};

struct App {
    binui::BinUIRenderer renderer;
    std::unique_ptr<binui::BinUIAppGLGdi> shell;
    std::deque<QueuedEvent> queue;
    std::string eventText;   // poll 期间 text 指向这里（下一次 poll 前有效）
    std::string assetDir;    // 图片 / 外挂组件基准目录（末尾带分隔符）
    bool opened = false;
    bool closeSent = false;
    int width = 0, height = 0;
};

App* A(void* p) { return static_cast<App*>(p); }

BinUIEvent MakeEvent(int type) {
    BinUIEvent e;
    std::memset(&e, 0, sizeof(e));
    e.type = type;
    e.node_id = -1;
    return e;
}

void PushEvent(App* a, BinUIEvent ev, const std::string& text = std::string()) {
    if (!a) return;
    if (!text.empty()) {
        ev.text_len = static_cast<int32_t>(text.size());
    } else {
        ev.text = nullptr;
        ev.text_len = 0;
    }
    QueuedEvent q;
    q.ev = ev;
    q.text = text;
    a->queue.push_back(std::move(q));
    // 宿主不 poll 时别无限涨（正常用法一帧就排空了）
    if (a->queue.size() > 8192) a->queue.pop_front();
}

// 失效一个节点：矩形要用"动画后的包围盒"，否则 hover 放大时画不全（边框被削半边）
void InvalidateNode(App* a, int nodeId) {
    if (!a || !a->shell || nodeId < 0) return;
    float x = 0, y = 0, w = 0, h = 0;
    if (!a->renderer.invalidRect(static_cast<uint16_t>(nodeId), x, y, w, h)) return;
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iw = static_cast<int>(std::ceil(x + w)) - ix + 1;
    const int ih = static_cast<int>(std::ceil(y + h)) - iy + 1;
    if (iw > 0 && ih > 0) a->shell->invalidateRect(ix, iy, iw, ih);
}

const binui::BinUIRenderer::NodeInfo* FindNode(App* a, int nodeId) {
    if (!a || nodeId < 0) return nullptr;
    for (const auto& n : a->renderer.getNodes())
        if (n.id == static_cast<uint16_t>(nodeId)) return &n;
    return nullptr;
}

// 目录补尾分隔符（DirOf 依赖它才能正确取到目录本身）
std::string asDir(std::string d) {
    if (d.empty()) return d;
    const char c = d.back();
    if (c != '/' && c != '\\') d += '/';
    return d;
}

} // namespace

// ============================================================
// 版本 / 错误
// ============================================================
static int32_t OpenAppInternal(void* appp, int32_t width, int32_t height,
                               uint32_t flags, const char* title_utf8,
                               void* parent_hwnd, int32_t x, int32_t y) {
    App* a = A(appp);
    if (!a) { SetErr("binui_app_open: app 为空"); return 0; }
    if (a->opened) { SetErr("binui_app_open: 窗口已经打开"); return 0; }

    const auto& wd = a->renderer.windowDesc();
    const int w = width > 0 ? width
                            : (wd.present && wd.width > 0 ? static_cast<int>(wd.width) : 900);
    const int h = height > 0 ? height
                             : (wd.present && wd.height > 0 ? static_cast<int>(wd.height) : 840);
    const std::string title = (title_utf8 && *title_utf8)
                                  ? std::string(title_utf8)
                                  : (wd.present && !wd.title.empty() ? wd.title
                                                                    : std::string("binUI"));

    const bool child = parent_hwnd != nullptr;
    // 子窗口不支持分层（逐像素透明对子窗口无效），也没有"自己可缩放"的语义 —— 宿主说了算
    const bool resizable = !child && ((flags & BINUI_OPEN_RESIZABLE) != 0 ||
                                      (wd.present && wd.resizable()));
    const bool transparent = !child && ((flags & BINUI_OPEN_TRANSPARENT) != 0 ||
                                        (wd.present && wd.transparent()));

    a->shell.reset(new binui::BinUIAppGLGdi(w, h, (flags & BINUI_OPEN_NO_VSYNC) == 0));
    a->shell->setWindowOptions(resizable, child ? 0.0f : (wd.present ? wd.aspect : 0.0f),
                               transparent, title.c_str());

    binui::BinUIRenderer& r = a->renderer;
    binui::BinUIAppGLGdi& sh = *a->shell;

    // ---- 绘制 ----
    sh.setDrawFn([a](binui::BinUIGLGdi& gfx, const void* clip) {
        binui::drawNodesGLGdi(gfx, a->renderer, clip, nullptr);
    });

    // ---- 每帧：推进动画 / 视频时钟，只重绘变化的节点 ----
    sh.setUpdateFn([a](float) {
        for (uint16_t id : a->renderer.stepAnimations()) InvalidateNode(a, id);
    });

    // ---- 渲染器自身的事件（点击 / 文本变化）→ 事件队列 ----
    r.setEventCallback([a](const binui::Event& ev) {
        switch (ev.type) {
            case binui::EventType::CLICK: {
                BinUIEvent e = MakeEvent(BINUI_EV_CLICK);
                e.node_id = static_cast<int32_t>(ev.nodeId);
                PushEvent(a, e);
                InvalidateNode(a, ev.nodeId);
                break;
            }
            case binui::EventType::INPUT: {
                const std::string text = ev.input.text ? ev.input.text : "";
                BinUIEvent e = MakeEvent(BINUI_EV_INPUT);
                e.node_id = static_cast<int32_t>(ev.nodeId);
                PushEvent(a, e, text);
                break;
            }
            default:
                break;
        }
    });

    // ---- 鼠标移动：悬停切换（只重绘前后两个节点） ----
    sh.setMoveFn([a](float x, float y) {
        const int oldId = a->renderer.hoveredNode();
        a->renderer.setHover(x, y);
        const int newId = a->renderer.hoveredNode();
        if (oldId == newId) return;
        InvalidateNode(a, oldId);
        InvalidateNode(a, newId);
        BinUIEvent e = MakeEvent(BINUI_EV_HOVER);
        e.node_id = newId;
        e.x = x;
        e.y = y;
        PushEvent(a, e);
    });

    // ---- 按下：标题栏拖动 / 焦点 / 输入框光标定位 ----
    sh.setPressFn([a](float x, float y) {
        // 子窗口模式下"拖动整窗"是宿主标题栏的事，这里不能抢
        if (!a->shell->isChildMode() && binui::isTitlebarDrag(a->renderer, x, y)) {
            a->shell->beginWindowDrag();
            return;
        }
        const int oldFocus = a->renderer.keyboardNode();
        a->renderer.press(x, y);
        InvalidateNode(a, a->renderer.keyboardNode());
        if (oldFocus != a->renderer.keyboardNode()) InvalidateNode(a, oldFocus);
        // 点输入框中部：光标落在点击位置，而不是永远跳到末尾
        if (a->renderer.isEditing())
            binui::placeCaretAtClickGLGdi(a->renderer, a->shell->gfx(), x, y);
        if (a->renderer.isEditing()) InvalidateNode(a, a->renderer.edit().nodeId);
    });

    // ---- 松开：CLICK 由渲染器的 release() 派发（见 setEventCallback），这里只重绘 ----
    sh.setReleaseFn([a](float x, float y) {
        const int pressed = a->renderer.pressedNode();
        a->renderer.release(x, y);
        InvalidateNode(a, pressed);
    });

    sh.setLeaveFn([a]() {
        const int oldId = a->renderer.hoveredNode();
        a->renderer.clearHover();
        InvalidateNode(a, oldId);
        BinUIEvent e = MakeEvent(BINUI_EV_HOVER);   // node_id = -1：鼠标记号离开窗口
        PushEvent(a, e);
    });

    sh.setWheelFn([a](float x, float y, float dy) {
        const int id = a->renderer.scrollAt(x, y, dy);
        if (id >= 0) InvalidateNode(a, id);
        BinUIEvent e = MakeEvent(BINUI_EV_WHEEL);
        e.node_id = id;
        e.x = x;
        e.y = y;
        e.dy = dy;
        PushEvent(a, e);
    });

    sh.setResizeFn([a](int w, int h) {
        a->renderer.setSize(w, h);
        a->width = w;
        a->height = h;
        BinUIEvent e = MakeEvent(BINUI_EV_RESIZE);
        e.width = w;
        e.height = h;
        PushEvent(a, e);
    });

    // ---- 字符输入：库内部吃掉编辑，宿主只需看 INPUT 事件 ----
    sh.setCharFn([a](const std::string& utf8) {
        if (a->renderer.isEditing()) {
            a->renderer.insertText(utf8);
            InvalidateNode(a, a->renderer.edit().nodeId);
        }
        BinUIEvent e = MakeEvent(BINUI_EV_CHAR);
        e.node_id = static_cast<int32_t>(a->renderer.edit().nodeId);
        PushEvent(a, e, utf8);
    });

    // ---- 按键：先告知宿主，再做库内约定处理 ----
    sh.setKeyFn([a](int vk) {
        binui::BinUIRenderer& r = a->renderer;

        BinUIEvent e = MakeEvent(BINUI_EV_KEY);
        e.key = vk;
        e.node_id = r.isEditing() ? static_cast<int32_t>(r.edit().nodeId) : r.keyboardNode();
        PushEvent(a, e);

        const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (r.isEditing()) {
            const size_t len = r.edit().value.size();
            const uint16_t id = r.edit().nodeId;
            switch (vk) {
                case VK_BACK:   r.backspace(); break;
                case VK_DELETE: r.deleteForward(); break;
                case VK_LEFT:   r.moveCaret(-1, shift); break;
                case VK_RIGHT:  r.moveCaret(1, shift); break;
                case VK_HOME:   r.setCaret(0); break;
                case VK_END:    r.setCaret(len); break;
                case VK_RETURN:
                case VK_ESCAPE: r.blur(); break;
                default: break;
            }
            InvalidateNode(a, id);
            return;
        }

        const int oldFocus = r.keyboardNode();
        switch (vk) {
            case VK_TAB:    r.cycleKeyboardFocus(!shift); break;
            case VK_LEFT:   r.moveKeyboardFocus(binui::BinUIRenderer::KeyDirection::LEFT); break;
            case VK_RIGHT:  r.moveKeyboardFocus(binui::BinUIRenderer::KeyDirection::RIGHT); break;
            case VK_UP:     r.moveKeyboardFocus(binui::BinUIRenderer::KeyDirection::UP); break;
            case VK_DOWN:   r.moveKeyboardFocus(binui::BinUIRenderer::KeyDirection::DOWN); break;
            case VK_RETURN:
            case VK_SPACE:  r.activateKeyboardFocus(); break;
            default: break;
        }
        if (oldFocus != r.keyboardNode()) {
            InvalidateNode(a, oldFocus);
            InvalidateNode(a, r.keyboardNode());
        }
    });

    sh.setFocusFn([a](bool focused) {
        if (focused) return;
        a->renderer.cancelPress();
        a->renderer.clearHover();
        a->renderer.blur();
        if (a->shell) a->shell->invalidateAll();
    });

    // ---- 布局自带的图片资源（v12 资源段记录了 imageIndex -> 路径） ----
    if (!a->assetDir.empty()) {
        const auto& paths = a->renderer.imagePaths();
        if (!paths.empty()) {
            auto imgs = binui::ex::LoadImages(a->assetDir, paths, std::string());
            if (!imgs.empty()) a->renderer.loadResources(imgs);
        }
    }

    const bool ok = child ? sh.openChild(parent_hwnd, x, y, w, h) : sh.open();
    if (!ok) {
        SetErr("binui_app_open: 窗口或 OpenGL 上下文创建失败");
        a->shell.reset();
        return 0;
    }
    a->opened = true;
    a->width = w;
    a->height = h;
    return 1;
}

extern "C" {

const char* BINUI_C_CALL binui_version(void) { return "0.2.0"; }

int32_t BINUI_C_CALL binui_abi_version(void) { return BINUI_C_ABI_VERSION; }

const char* BINUI_C_CALL binui_last_error(void) { return g_err.c_str(); }

void BINUI_C_CALL binui_free(void* p) { std::free(p); }

// ============================================================
// 编译
// ============================================================
int32_t BINUI_C_CALL binui_compile_file(const char* src_path_utf8, const char* out_path_utf8) {
    if (!src_path_utf8 || !*src_path_utf8) { SetErr("binui_compile_file: 源路径为空"); return 0; }
    if (!out_path_utf8 || !*out_path_utf8) { SetErr("binui_compile_file: 输出路径为空"); return 0; }

    std::vector<uint8_t> bytes = binui::ex::ReadFileBytes(src_path_utf8);
    if (bytes.empty()) {
        SetErr("binui_compile_file: 读不到源文件 %s", src_path_utf8);
        return 0;
    }
    // 已经是二进制布局：原样写出（源码/二进制两条流水线共用一条命令）
    if (binui::IsLayoutData(bytes.data(), bytes.size())) {
        // 直接落盘
    } else {
        binui::CompileResult res = binui::CompileBinUIFileEx(src_path_utf8);
        for (const auto& w : res.warnings) {
            std::fprintf(stderr, "[binui_compile] warning: %s\n", w.c_str());
        }
        if (res.layout.empty()) {
            SetErr("binui_compile_file: 编译 %s 失败（空布局）", src_path_utf8);
            return 0;
        }
        bytes = std::move(res.layout);
    }

    std::FILE* f = std::fopen(out_path_utf8, "wb");
    if (!f) {
        SetErr("binui_compile_file: 无法写入 %s", out_path_utf8);
        return 0;
    }
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        SetErr("binui_compile_file: 写入 %s 不完整（%zu / %zu 字节）",
               out_path_utf8, n, bytes.size());
        return 0;
    }
    return 1;
}

int32_t BINUI_C_CALL binui_compile_memory(const char* src_utf8, int32_t src_len,
                                         void** out_data, int32_t* out_len) {
    if (!out_data || !out_len) { SetErr("binui_compile_memory: out 参数为空"); return 0; }
    *out_data = nullptr;
    *out_len = 0;
    if (!src_utf8 || src_len == 0) { SetErr("binui_compile_memory: 源码为空"); return 0; }

    const std::string src = (src_len > 0) ? std::string(src_utf8, static_cast<size_t>(src_len))
                                          : std::string(src_utf8);
    const binui::CompileResult res = binui::CompileBinUIEx(src);
    for (const auto& w : res.warnings) {
        std::fprintf(stderr, "[binui_compile] warning: %s\n", w.c_str());
    }
    if (res.layout.empty()) { SetErr("binui_compile_memory: 编译失败（空布局）"); return 0; }

    void* p = std::malloc(res.layout.size());
    if (!p) { SetErr("binui_compile_memory: 内存不足"); return 0; }
    std::memcpy(p, res.layout.data(), res.layout.size());
    *out_data = p;
    *out_len = static_cast<int32_t>(res.layout.size());
    return 1;
}

// ============================================================
// 应用生命周期
// ============================================================
void* BINUI_C_CALL binui_app_create(const void* layout, int32_t len, const char* asset_dir_utf8) {
    if (!layout || len <= 0) { SetErr("binui_app_create: 布局为空"); return nullptr; }

    std::unique_ptr<App> app(new App());
    if (asset_dir_utf8 && *asset_dir_utf8) app->assetDir = asDir(asset_dir_utf8);

    if (!app->renderer.loadLayout(static_cast<const uint8_t*>(layout), static_cast<size_t>(len))) {
        SetErr("binui_app_create: 布局解析失败（不是 .bui / 版本不匹配 / 数据截断）");
        return nullptr;
    }
    return app.release();
}

int32_t BINUI_C_CALL binui_app_open(void* appp, int32_t width, int32_t height,
                                    uint32_t flags, const char* title_utf8) {
    return OpenAppInternal(appp, width, height, flags, title_utf8, nullptr, 0, 0);
}

int32_t BINUI_C_CALL binui_app_open_child(void* appp, void* parent_hwnd, int32_t x, int32_t y,
                                         int32_t width, int32_t height, uint32_t flags) {
    if (!parent_hwnd) {
        SetErr("binui_app_open_child: parent_hwnd 为空");
        return 0;
    }
    if (width <= 0 || height <= 0) {
        SetErr("binui_app_open_child: 尺寸非法（%d x %d）", width, height);
        return 0;
    }
    return OpenAppInternal(appp, width, height, flags, nullptr, parent_hwnd, x, y);
}

int32_t BINUI_C_CALL binui_render(void* appp) {
    App* a = A(appp);
    if (!a || !a->shell) return 1;
    a->shell->renderFrame();
    // 子窗口没有 WM_QUIT：宿主销毁自己的窗口时 WM_DESTROY 会把 shell 关掉，这里就返回 1
    return a->shell->isOpen() ? 0 : 1;
}

void BINUI_C_CALL binui_resize(void* appp, int32_t width, int32_t height) {
    App* a = A(appp);
    if (!a || !a->shell || width <= 0 || height <= 0) return;
    a->shell->setClientSize(width, height);
    a->width = width;
    a->height = height;
}

void* BINUI_C_CALL binui_app_hwnd(void* appp) {
    App* a = A(appp);
    return (a && a->shell) ? a->shell->windowHandle() : nullptr;
}

int32_t BINUI_C_CALL binui_pump(void* appp, int32_t timeout_ms) {
    App* a = A(appp);
    if (!a || !a->shell) return 1;
    const int rc = a->shell->pump(timeout_ms);
    if (rc != 0 && !a->closeSent) {
        a->closeSent = true;
        BinUIEvent e = MakeEvent(BINUI_EV_CLOSE);
        PushEvent(a, e);
    }
    return rc;
}

int32_t BINUI_C_CALL binui_poll_event(void* appp, BinUIEvent* out) {
    App* a = A(appp);
    if (!a || !out || a->queue.empty()) return 0;
    QueuedEvent q = std::move(a->queue.front());
    a->queue.pop_front();
    a->eventText = std::move(q.text);
    *out = q.ev;
    if (a->eventText.empty()) {
        out->text = nullptr;
        out->text_len = 0;
    } else {
        out->text = a->eventText.c_str();
        out->text_len = static_cast<int32_t>(a->eventText.size());
    }
    return 1;
}

void BINUI_C_CALL binui_app_destroy(void* appp) {
    App* a = A(appp);
    if (!a) return;
    if (a->shell) {
        a->shell->close();
        a->shell.reset();
    }
    delete a;
}

// ============================================================
// 节点查询
// ============================================================
int32_t BINUI_C_CALL binui_node_count(void* appp) {
    App* a = A(appp);
    return a ? static_cast<int32_t>(a->renderer.getNodes().size()) : 0;
}

int32_t BINUI_C_CALL binui_node_at(void* appp, int32_t index, BinUINode* out) {
    App* a = A(appp);
    if (!a || !out || index < 0) return 0;
    const auto& nodes = a->renderer.getNodes();
    if (static_cast<size_t>(index) >= nodes.size()) return 0;
    const auto& n = nodes[static_cast<size_t>(index)];
    out->id = static_cast<int32_t>(n.id);
    out->parent_id = static_cast<int32_t>(n.parentId);
    out->type = static_cast<int32_t>(n.type);
    out->flags = static_cast<int32_t>(n.flags);
    out->x = n.x;
    out->y = n.y;
    out->width = n.width;
    out->height = n.height;
    out->color = n.color;
    out->image_index = static_cast<int32_t>(n.imageIndex);
    out->font_size = static_cast<int32_t>(n.fontSize);
    out->padding = static_cast<int32_t>(n.padding);
    out->radius = static_cast<int32_t>(n.radius);
    out->z_index = static_cast<int32_t>(n.zIndex);
    return 1;
}

int32_t BINUI_C_CALL binui_node_by_id(void* appp, int32_t id, BinUINode* out) {
    App* a = A(appp);
    if (!a || !out) return 0;
    const auto& nodes = a->renderer.getNodes();
    for (size_t i = 0; i < nodes.size(); i++) {
        if (static_cast<int32_t>(nodes[i].id) != id) continue;
        return binui_node_at(appp, static_cast<int32_t>(i), out);
    }
    return 0;
}

int32_t BINUI_C_CALL binui_find_node(void* appp, const char* name_utf8) {
    App* a = A(appp);
    if (!a || !name_utf8 || !*name_utf8) return -1;
    const uint16_t id = a->renderer.findNodeByName(name_utf8);
    return id ? static_cast<int32_t>(id) : -1;
}

int32_t BINUI_C_CALL binui_node_text(void* appp, int32_t id, char* buf, int32_t cap) {
    App* a = A(appp);
    const auto* n = FindNode(a, id);
    if (!n) return -1;
    const int32_t len = static_cast<int32_t>(n->text.size());
    if (buf && cap > 0) {
        const int32_t copy = (len < cap - 1) ? len : cap - 1;
        std::memcpy(buf, n->text.data(), static_cast<size_t>(copy));
        buf[copy] = '\0';
    }
    return len;
}

// ============================================================
// 节点修改
// ============================================================
int32_t BINUI_C_CALL binui_set_text(void* appp, int32_t id, const char* utf8, int32_t len) {
    App* a = A(appp);
    if (!a || !FindNode(a, id)) return 0;
    const std::string text = utf8 ? ((len >= 0) ? std::string(utf8, static_cast<size_t>(len))
                                                : std::string(utf8))
                                  : std::string();
    a->renderer.setText(static_cast<uint16_t>(id), text);
    InvalidateNode(a, id);
    return 1;
}

int32_t BINUI_C_CALL binui_set_visible(void* appp, int32_t id, int32_t visible) {
    App* a = A(appp);
    if (!a || !FindNode(a, id)) return 0;
    a->renderer.setVisible(static_cast<uint16_t>(id), visible != 0);
    InvalidateNode(a, id);
    if (a->shell) a->shell->invalidateAll();   // 显隐会牵动兄弟节点的布局
    return 1;
}

int32_t BINUI_C_CALL binui_set_checked(void* appp, int32_t id, int32_t on) {
    App* a = A(appp);
    if (!a || !FindNode(a, id)) return 0;
    a->renderer.setNodeChecked(static_cast<uint16_t>(id), on != 0);
    InvalidateNode(a, id);
    return 1;
}

int32_t BINUI_C_CALL binui_invalidate_node(void* appp, int32_t id) {
    App* a = A(appp);
    if (!a || !FindNode(a, id)) return 0;
    InvalidateNode(a, id);
    return 1;
}

void BINUI_C_CALL binui_invalidate_all(void* appp) {
    App* a = A(appp);
    if (a && a->shell) a->shell->invalidateAll();
}

int32_t BINUI_C_CALL binui_hit_test(void* appp, float x, float y) {
    App* a = A(appp);
    return a ? a->renderer.hitTest(x, y) : -1;
}

int32_t BINUI_C_CALL binui_app_size(void* appp, int32_t* w, int32_t* h) {
    App* a = A(appp);
    if (!a || !a->opened) return 0;
    if (w) *w = a->width;
    if (h) *h = a->height;
    return 1;
}

// ============================================================
// 窗口动作
// ============================================================
void BINUI_C_CALL binui_request_exit(void* appp) {
    App* a = A(appp);
    if (a && a->shell) a->shell->requestExit();
}

void BINUI_C_CALL binui_minimize(void* appp) {
    App* a = A(appp);
    if (a && a->shell) a->shell->minimize();
}

void BINUI_C_CALL binui_maximize(void* appp) {
    App* a = A(appp);
    if (a && a->shell) a->shell->maximize();
}

void BINUI_C_CALL binui_begin_window_drag(void* appp) {
    App* a = A(appp);
    if (a && a->shell) a->shell->beginWindowDrag();
}

// ============================================================
// 资源
// ============================================================
int32_t BINUI_C_CALL binui_set_image_raw(void* appp, int32_t index, int32_t w, int32_t h,
                                         const void* rgba) {
    App* a = A(appp);
    if (!a || !rgba || w <= 0 || h <= 0 || index < 0) return 0;
    if (!a->renderer.updateImageRaw(static_cast<uint16_t>(index), w, h,
                                    static_cast<const uint8_t*>(rgba))) {
        SetErr("binui_set_image_raw: 槽 %d 更新失败", index);
        return 0;
    }
    if (a->shell) a->shell->invalidateAll();
    return 1;
}

int32_t BINUI_C_CALL binui_set_image(void* appp, int32_t index, const void* data, int32_t len) {
    App* a = A(appp);
    if (!a || !data || len <= 0 || index < 0) return 0;
    if (!a->renderer.updateImage(static_cast<uint16_t>(index),
                                 static_cast<const uint8_t*>(data),
                                 static_cast<size_t>(len))) {
        SetErr("binui_set_image: 槽 %d 更新失败（不是可解码的图片？）", index);
        return 0;
    }
    if (a->shell) a->shell->invalidateAll();
    return 1;
}

int32_t BINUI_C_CALL binui_mount_components(void* appp, const char* layout_dir_utf8) {
    App* a = A(appp);
    if (!a) return -1;
    const std::string dir = (layout_dir_utf8 && *layout_dir_utf8)
                                ? asDir(layout_dir_utf8)
                                : a->assetDir;
    const binui::ex::MountReport rep =
        binui::ex::MountAll(a->renderer, dir, a->renderer.layoutBytes());
    if (a->shell) a->shell->invalidateAll();
    if (!rep.ok()) {
        SetErr("binui_mount_components: 失败 %d 项（缺文件或编译失败，见 stderr）", rep.failed);
    }
    return rep.mounted;
}

} // extern "C"
