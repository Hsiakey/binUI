// texturedemo/main.cpp - 纹理演示：updateImageRaw 运行期上传
//
// 布局里的 <img src="tex/proc.png"> 指向一个**不存在**的文件：编译器照样给它分配图片槽
// （imageIndex），运行期由这里每帧生成 RGBA 像素塞进那个槽。视频帧走的是同一条通路。
//
// 要点：
//   · updateImageRaw(index, w, h, rgba) 自增该槽的版本号 → 渲染器的纹理缓存失效
//   · 外壳只需要 invalidate 图片节点自己的矩形，窗口其余部分不重画
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include <windows.h>

using namespace binui;

static const int kTexSize = 128;   // 程序化纹理边长（RGBA8）

// 生成一帧纹理：极坐标等离子 + 网格线，随时间旋转
static void MakeTexture(std::vector<uint8_t>& px, float t) {
    px.resize((size_t)kTexSize * kTexSize * 4);
    for (int y = 0; y < kTexSize; y++) {
        for (int x = 0; x < kTexSize; x++) {
            const float fx = (float)x / kTexSize - 0.5f;
            const float fy = (float)y / kTexSize - 0.5f;
            const float d = std::sqrt(fx * fx + fy * fy);
            const float a = std::atan2(fy, fx);
            const float v = 0.5f + 0.5f * std::sin(10.0f * d - t * 3.0f + 3.0f * a);
            const float w = 0.5f + 0.5f * std::cos(6.0f * d + t * 2.0f - 2.0f * a);
            uint8_t* p = px.data() + ((size_t)y * kTexSize + x) * 4;
            p[0] = (uint8_t)(60 + 180 * v);              // R
            p[1] = (uint8_t)(120 + 120 * w);             // G
            p[2] = (uint8_t)(230 - 90 * v * w);          // B
            p[3] = 255;
            // 每 16 px 一条淡网格线，方便看清纹理边界
            if ((x % 16 == 0) || (y % 16 == 0)) {
                p[0] = (uint8_t)(p[0] * 0.75f);
                p[1] = (uint8_t)(p[1] * 0.75f);
                p[2] = (uint8_t)(p[2] * 0.75f);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "texturedemo", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) res.layout = GenerateDefaultBinUI();
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[texturedemo] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 1;

    // 布局里引用了不存在的图片：这里把槽补齐，避免加载器报缺图后槽位留空
    renderer.reserveImageSlots(std::max<size_t>(1, res.images.size()));

    const auto& wd = renderer.windowDesc();
    const auto& nodes0 = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes0.empty() ? 900 : (int)nodes0[0].width);
    int height = wd.present ? (int)wd.height : (nodes0.empty() ? 640 : (int)nodes0[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.title.c_str());

    // 收集所有图片节点：(节点 id, 图片槽)
    struct ImgSlot { uint16_t nodeId; uint16_t index; };
    std::vector<ImgSlot> imgs;
    for (const auto& n : renderer.getNodes())
        if (n.type == BINUI_IMAGE && n.imageIndex != 0xFFFF)
            imgs.push_back({ n.id, n.imageIndex });
    std::printf("[texturedemo] 图片节点 %zu 个，槽位：", imgs.size());
    for (const auto& s : imgs) std::printf("%u ", (unsigned)s.index);
    std::printf("\n");

    auto invalidateNode = [&](uint16_t id) {
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };
    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) {
        drawNodesGLGdi(gfx, renderer, clip, nullptr);
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
    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.press(x, y);
    });
    app.setReleaseFn([&](float x, float y) {
        int p = renderer.pressedNode();
        int id = renderer.release(x, y);
        if (p >= 0) invalidateNode((uint16_t)p);
        if (id < 0) return;
        for (const auto& n : renderer.getNodes()) {
            if (n.id != (uint16_t)id) continue;
            if (n.text == "\xE2\x88\x92") app.minimize();
            else if (n.text == "\xE2\x96\xA1") app.maximize();
            else if (n.text == "\xE2\x9C\x95") app.requestExit();
            break;
        }
    });
    app.setLeaveFn([&]() {
        int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) invalidateNode((uint16_t)h);
    });
    app.setKeyFn([&](int vk) { if (vk == VK_ESCAPE) app.requestExit(); });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); app.invalidateAll(); }
    });

    std::vector<uint8_t> pixels;
    float t = 0.0f, acc = 0.0f;
    int frames = 0;
    uint16_t idState = renderer.findNodeByName("tex-state");
    uint16_t idNote  = renderer.findNodeByName("tex-note");

    app.setUpdateFn([&](float dt) {
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);

        t += dt;
        acc += dt;
        if (acc < 1.0f / 30.0f) return;   // 30Hz 上传就够了
        acc = 0.0f;
        frames++;

        // 每帧只生成一次像素，多个槽共用
        MakeTexture(pixels, t);
        for (const auto& s : imgs)
            renderer.updateImageRaw(s.index, kTexSize, kTexSize, pixels.data());
        for (const auto& s : imgs)
            invalidateNode(s.nodeId);     // 只失效图片矩形

        if (idState && frames % 15 == 0) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "已上传 %d 帧 · %dx%d RGBA8 · 槽版本 %u · 脏矩形只在图片上",
                          frames, kTexSize, kTexSize,
                          (unsigned)(imgs.empty() ? 0 : renderer.imageGeneration(imgs[0].index)));
            renderer.setText(idState, buf);
            invalidateNode(idState);
        }
        if (idNote && frames == 15) {
            renderer.setText(idNote, "纹理内容与 HTML 里的路径无关：路径只用来占一个槽位");
            invalidateNode(idNote);
        }
    });
    app.setResizeFn([&](int w, int h) { renderer.setSize(w, h); });

    std::printf("[texturedemo] 纹理演示 (OpenGL 3.3)\n");
    return app.run();
}
