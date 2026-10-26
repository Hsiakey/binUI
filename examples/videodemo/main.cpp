// videodemo/main.cpp - <video> 原生播放演示（OpenGL 后端 + 可选 FFmpeg 引擎）
//
// 始终可用（无需解码模块）：<video> 节点、浏览器式控制条、点击/键盘播放暂停、
//                           进度条拖动定位、loop 回绕、VIDEO_* 事件。
// BINUI_ENABLE_VIDEO=ON 时：FFmpeg 解码帧经 updateImageRaw 推送，WASAPI 出声。
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unordered_map>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_video.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

static void PrintUsage() {
    std::printf("用法: videodemo [--watch] [--ui <main.html>] [--src <video.mp4>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（UI 开发用）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n"
                "  --src    覆盖 <video> 的 src（相对 UI 目录或绝对路径）\n");
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "videodemo", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[videodemo] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 1;

    // 媒体路径解析基准 = UI 所在目录（src 相对它）；去掉末尾分隔符交给解码模块拼接
    std::string base = binui::ex::DirOf(uiPath);
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();

    // --src <file> 覆盖 <video> 的 src
    std::string srcOverride;
    for (size_t i = 0; i + 1 < args.rest.size(); ++i) {
        if (args.rest[i] == "--src") { srcOverride = args.rest[i + 1]; break; }
    }

    renderer.setImageOrder(res.images);
    renderer.loadResources(binui::ex::LoadImages(uiPath, res.images, "videodemo"));

    uint16_t vid = renderer.findNodeByName("vid");
    uint16_t stateLbl = renderer.findNodeByName("state");
    if (!srcOverride.empty()) renderer.setVideoSource(vid, srcOverride);

    const auto& wd = renderer.windowDesc();
    int width  = wd.present ? (int)wd.width  : 960;
    int height = wd.present ? (int)wd.height : 640;

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true,
                         wd.present ? wd.aspect : 0,
                         false, wd.title.c_str());

    auto findNode = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
        for (const auto& n : renderer.getNodes())
            if (n.id == id) return &n;
        return nullptr;
    };
    auto invalidateNode = [&](uint16_t id) {
        const auto* n = findNode(id);
        if (n && (n->flags & BINUI_VISIBLE))
            app.invalidateRect((int)n->x, (int)n->y, (int)n->width, (int)n->height);
    };

    // ---- 可选解码引擎（桩实现恒返 false → 显示降级提示） ----
    bool engine = AttachVideoEngine(renderer, base);
    std::string stateText = "engine: stub - rebuild with BINUI_ENABLE_VIDEO=ON + FFMPEG_ROOT to decode";
    if (engine) {
        const auto* v = renderer.findVideo(vid);
        char buf[160];
        if (v && v->duration > 0)
            snprintf(buf, sizeof(buf), "engine: FFmpeg+WASAPI | %.1fs | click or Space to toggle",
                     v->duration);
        else
            snprintf(buf, sizeof(buf), "engine: FFmpeg+WASAPI | media not found (media/sample.mp4)");
        stateText = buf;
    }
    if (stateLbl) {
        renderer.setText(stateLbl, stateText);
        invalidateNode(stateLbl);
    }

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) { drawNodesGLGdi(gfx, renderer, clip); });
    app.setMoveFn([&](float x, float y) {
        int old = renderer.hoveredNode();
        renderer.setHover(x, y); // 拖动进度条时渲染器内部处理
        int cur = renderer.hoveredNode();
        if (old != cur || renderer.videoControlsVisible((uint16_t)std::max(cur, 0))) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
            if (vid > 0) invalidateNode(vid); // 控制条悬停显隐
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
        int id = renderer.release(x, y); // 视频控制条交互在渲染器内部完成
        if (p >= 0) invalidateNode((uint16_t)p);
        if (id >= 0) {
            for (const auto& n : renderer.getNodes()) {
                if (n.id != (uint16_t)id) continue;
                if (n.text == "\xE2\x94\x80") app.minimize();
                else if (n.text == "\xE2\x9C\x95") app.requestExit();
                break;
            }
        }
        if (id >= 0) invalidateNode((uint16_t)id);
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
        invalidateNode(vid); // 悬停消失可能隐藏控制条
    });
    app.setKeyFn([&](int vk) {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (renderer.isEditing()) {
            if (vk == VK_BACK) renderer.backspace();
            else if (vk == VK_ESCAPE) renderer.blur();
            else if (vk == VK_TAB) { // 提交编辑并移出（浏览器行为）
                int old = renderer.keyboardNode();
                renderer.blur();
                renderer.cycleKeyboardFocus(!shift);
                if (old >= 0) invalidateNode((uint16_t)old);
                if (renderer.keyboardNode() >= 0)
                    invalidateNode((uint16_t)renderer.keyboardNode());
            }
        } else {
            if (vk == VK_ESCAPE) { app.requestExit(); return; }
            if (vk == VK_LEFT)       renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::LEFT);
            else if (vk == VK_RIGHT) renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::RIGHT);
            else if (vk == VK_UP)    renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::UP);
            else if (vk == VK_DOWN)  renderer.moveKeyboardFocus(BinUIRenderer::KeyDirection::DOWN);
            else if (vk == VK_RETURN || vk == VK_SPACE) renderer.activateKeyboardFocus();
            else if (vk == VK_TAB) { // Tab/Shift+Tab 循环焦点
                int old = renderer.keyboardNode();
                renderer.cycleKeyboardFocus(!shift);
                if (old >= 0) invalidateNode((uint16_t)old);
            }
            else if (vid && (vk == 'M')) { // M 静音切换
                const auto* v = renderer.findVideo(vid);
                if (v) renderer.setVideoMuted(vid, !(v->flags & VID_MUTED));
            }
            if (renderer.keyboardNode() >= 0)
                invalidateNode((uint16_t)renderer.keyboardNode());
        }
    });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); app.invalidateAll(); }
    });

    // 每帧：动画 + 视频时钟推进；变化的节点局部失效（视频播放中每帧重绘）
    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = width, winH = height;

    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "videodemo", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            // 换树：重取节点 + 恢复状态标签（解码引擎不重启，帧继续推同一伪槽位）
            vid = renderer.findNodeByName("vid");
            stateLbl = renderer.findNodeByName("state");
            if (!srcOverride.empty()) renderer.setVideoSource(vid, srcOverride);
            if (stateLbl) renderer.setText(stateLbl, stateText);
            app.invalidateAll();
        }
        static bool lastOn = caretBlinkOn();
        bool on = caretBlinkOn();
        if (on != lastOn) { lastOn = on; if (renderer.isEditing()) invalidateNode(renderer.edit().nodeId); }
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) {
        winW = w; winH = h;
        renderer.setSize(w, h);
        app.invalidateAll();
    });

    renderer.setEventCallback([&](const Event& ev) {
        switch (ev.type) {
            case EventType::VIDEO_PLAY:  std::printf("[video] play #%u\n", ev.nodeId); break;
            case EventType::VIDEO_PAUSE: std::printf("[video] pause #%u\n", ev.nodeId); break;
            case EventType::VIDEO_SEEK:  std::printf("[video] seek #%u\n", ev.nodeId); break;
            case EventType::VIDEO_ENDED: std::printf("[video] ended #%u\n", ev.nodeId); break;
            default: break;
        }
        if ((int)ev.type >= (int)EventType::VIDEO_PLAY &&
            (int)ev.type <= (int)EventType::VIDEO_ENDED)
            invalidateNode(ev.nodeId); // 控制条/画面立即重绘
    });

    std::printf("[videodemo] running (GL backend)\n");
    int code = app.run();
    DetachVideoEngine();
    return code;
}
