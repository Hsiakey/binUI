// singlewindow/main.cpp - binUI 单窗口模式（OpenGL 3.3 GPU 后端，无边框窗口）
// 全部 UI（标题栏/立绘/对话框/按钮）都在同一个窗口里，用
// examples/assets/noela.png 作 galgame 立绘，展示：
//   1) <img src> 编译 -> imageIndex -> 运行时按路径加载字节 -> GPU 解码/纹理/四边形绘制
//   2) 多图层合成 + z-index 分层：背光(1) < 立绘(2) < 半透明前景遮挡(5) < HUD(8)
//      < 对话框(10) < 头像框(11) < 头像(12) < 按钮(20)；同一张 PNG 复用为立绘与头像
//   3) 等比自适应：window_resize="aspect" + ui_scale="proportional" -> 拖动边框时
//      整棵 UI 保持 1000×700 比例，不变形
//   4) 交互：setVisible 显隐、setText 改按钮文字、restartAnimations 重播、
//      updateImage 换图（新 generation 触发纹理重传）、标题栏三键用 id 区分
#include <cstdio>
#include <cmath>
#include <algorithm>
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
    std::printf("用法: singlewindow [--watch] [--ui <main.html>]\n"
                "  --watch  保存 UI 后自动重新编译并热重载（UI 开发用）\n"
                "  --ui     显式指定 UI 文件（默认在 exe 同级目录查找）\n");
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) { PrintUsage(); return 0; }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "singlewindow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        if (uiPath.empty())
            std::fprintf(stderr, "[singlewindow] 未找到 UI 文件，使用默认布局（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[singlewindow] Failed to load layout\n");
        return 1;
    }

    // ---- 图片资源：按编译产出的路径表查找文件并交给渲染器 ----
    // 查找顺序：UI 目录 -> exeDir -> <exe>/singlewindow/ -> tests/（源码树内运行时）
    std::unordered_map<std::string, std::vector<uint8_t>> resources =
        binui::ex::LoadImages(uiPath, res.images, "singlewindow", {"tests/"});
    for (const auto& p : res.images)
        std::printf("[singlewindow] image \"%s\" %s\n", p.c_str(),
                    resources.count(p) ? "loaded" : "MISS");
    renderer.setImageOrder(res.images);
    renderer.loadResources(resources);

    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes.empty() ? 1000 : (int)nodes[0].width);
    int height = wd.present ? (int)wd.height : (nodes.empty() ? 700 : (int)nodes[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true,
                         wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(),
                         wd.title.c_str());

    uint16_t heroId   = renderer.findNodeByName("hero");
    uint16_t avatarId = renderer.findNodeByName("avatar");
    uint16_t scrimId  = renderer.findNodeByName("scrim");
    uint16_t btnTog   = renderer.findNodeByName("btnToggle");
    uint16_t btnScrim = renderer.findNodeByName("btnScrim");
    uint16_t btnShake = renderer.findNodeByName("btnShake");
    uint16_t btnBg    = renderer.findNodeByName("btnBg");
    uint16_t btnReload= renderer.findNodeByName("btnReload");
    uint16_t btnMin   = renderer.findNodeByName("btnMin");
    uint16_t btnClose = renderer.findNodeByName("btnClose");

    uint16_t heroImageIndex = 0;
    for (const auto& n : renderer.getNodes())
        if (n.id == heroId) { heroImageIndex = n.imageIndex; break; }

    auto findNode = [&](uint16_t id) -> const binui::BinUIRenderer::NodeInfo* {
        for (const auto& n : renderer.getNodes())
            if (n.id == id) return &n;
        return nullptr;
    };
        auto invalidateNode = [&](uint16_t id) {
        // 失效矩形必须含动画后的包围盒：hover 放大时只按布局矩形失效，
        // 长出来的那一圈（边框/底色）不会被重绘，看上去就是"边框被削掉一半"
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    bool heroVisible = true;
    bool scrimVisible = true;
    const uint32_t bgCycle[] = { 0xFF171223, 0xFF0E1A17, 0xFF1A1210, 0xFF101A26 };
    int bgIdx = 0;

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) { drawNodesGLGdi(gfx, renderer, clip); });

    app.setMoveFn([&](float x, float y) {
        int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        int cur = renderer.hoveredNode();
        if (old != cur) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
        }
        if (cur >= 0) {
            const auto* n = findNode((uint16_t)cur);
            if (n && (n->flags & BINUI_SCROLLABLE)) invalidateNode((uint16_t)cur);
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
        if (id < 0) return;
        uint16_t nid = (uint16_t)id;
        if (nid == btnMin)   { app.minimize();    return; }
        if (nid == btnClose) { app.requestExit(); return; }
        if (nid == btnTog) {
            const auto* n = findNode(heroId);
            heroVisible = !(n && (n->flags & BINUI_VISIBLE));
            renderer.setVisible(heroId, heroVisible);
            app.invalidateAll();
            std::printf("[singlewindow] setVisible(hero, %d)\n", (int)heroVisible);
            return;
        }
        if (nid == btnScrim) {
            const auto* n = findNode(scrimId);
            scrimVisible = !(n && (n->flags & BINUI_VISIBLE));
            renderer.setVisible(scrimId, scrimVisible);
            renderer.setText(btnScrim, scrimVisible ? "前景遮挡" : "显示前景");
            invalidateNode(scrimId);
            invalidateNode(btnScrim);
            std::printf("[singlewindow] setVisible(scrim, %d)\n", (int)scrimVisible);
            return;
        }
        if (nid == btnShake) {
            renderer.restartAnimations();
            app.invalidateAll();
            std::printf("[singlewindow] restartAnimations()\n");
            return;
        }
        if (nid == btnBg) {
            bgIdx = (bgIdx + 1) % (int)(sizeof(bgCycle) / sizeof(bgCycle[0]));
            renderer.setStyleColor(0, ANIM_BG_COLOR, bgCycle[bgIdx]);
            app.invalidateAll();
            std::printf("[singlewindow] setStyleColor(root) -> #%06X\n", bgCycle[bgIdx] & 0xFFFFFF);
            return;
        }
        if (nid == btnReload) {
            // 重新读取磁盘图片 -> updateImage（编码字节，新 generation 触发纹理重传）
            const std::string name = res.images.empty() ? std::string() : res.images[0];
            std::vector<uint8_t> data = binui::ex::ReadFileBytes("tests/" + name);
            if (data.empty()) data = binui::ex::ReadFileBytes("examples/singlewindow/" + name);
            if (!data.empty() && renderer.updateImage(heroImageIndex, data.data(), data.size())) {
                invalidateNode(heroId);
                invalidateNode(avatarId);
                std::printf("[singlewindow] updateImage(\"%s\") gen=%u\n",
                            name.c_str(), renderer.imageGeneration(heroImageIndex));
            } else {
                std::fprintf(stderr, "[singlewindow] reload failed\n");
            }
            return;
        }
        invalidateNode(nid);
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    app.setKeyFn([&](int vk) {
        if (vk == VK_ESCAPE) { app.requestExit(); return; }
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

    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "singlewindow", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            // 换树后按 id 重新取节点（id 随 DOM 顺序，结构不变则编号不变）
            heroId    = renderer.findNodeByName("hero");
            avatarId  = renderer.findNodeByName("avatar");
            scrimId   = renderer.findNodeByName("scrim");
            btnTog    = renderer.findNodeByName("btnToggle");
            btnScrim  = renderer.findNodeByName("btnScrim");
            btnShake  = renderer.findNodeByName("btnShake");
            btnBg     = renderer.findNodeByName("btnBg");
            btnReload = renderer.findNodeByName("btnReload");
            btnMin    = renderer.findNodeByName("btnMin");
            app.invalidateAll();
        }
        for (uint16_t id : renderer.stepAnimations())
            invalidateNode(id);
    });
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    std::printf("[singlewindow] binUI 单窗口模式 (OpenGL 3.3) — %d 图片资源\n", (int)res.images.size());
    return app.run();
}
