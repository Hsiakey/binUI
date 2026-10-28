// cubedemo/main.cpp - 3D 演示：应用自己算 3D，引擎只负责 UI
//
// binUI 是 2D UI 库：没有矩阵栈、没有深度缓冲、没有三角图元。这个示例演示"正确用法"——
// 顶点变换 / 透视投影 / 深度排序全在应用里做，最后只用 gfx.fillLine 把 12 条棱画出来。
// 想把 UI 叠在 3D 之上，就让 UI 卡片本身不透明（引擎按节点顺序合成，后画的盖住先画的）。
#include <cmath>
#include <cstdio>
#include <string>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include <windows.h>

using namespace binui;

namespace {

struct V3 { float x, y, z; };

// 立方体 8 个顶点（±1）
const V3 kVerts[8] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};
// 12 条棱
const int kEdges[12][2] = {
    {0,1},{1,2},{2,3},{3,0},   // 后面
    {4,5},{5,6},{6,7},{7,4},   // 前面
    {0,4},{1,5},{2,6},{3,7},   // 连接
};

inline V3 RotY(const V3& v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}
inline V3 RotX(const V3& v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return { v.x, v.y * c - v.z * s, v.y * s + v.z * c };
}

// 透视投影：返回屏幕坐标（z 已转成深度，越大越远）
inline void Project(const V3& v, float cx, float cy, float scale, float dist,
                    float& sx, float& sy, float& depth) {
    const float z = v.z + dist;
    const float k = scale / (z > 0.05f ? z : 0.05f);
    sx = cx + v.x * k;
    sy = cy - v.y * k;
    depth = z;
}

// 按深度在"近蓝 → 远淡蓝"之间插值
inline uint32_t EdgeColor(float depth, float dist) {
    float t = (depth - dist) * 0.5f + 0.5f;         // 0 = 近, 0.5 = 中心
    if (t < 0) t = 0;
    if (t > 0.9f) t = 0.9f;
    const float r = 0x3E + (0xF0 - 0x3E) * t;
    const float g = 0x8B + (0xF5 - 0x8B) * t;
    const float b = 0xFF;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

}  // namespace

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
#ifdef _WIN32
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "cubedemo", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) res.layout = GenerateDefaultBinUI();
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[cubedemo] warning: %s\n", w.c_str());
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) return 1;

    const auto& wd = renderer.windowDesc();
    const auto& nodes0 = renderer.getNodes();
    int width  = wd.present ? (int)wd.width  : (nodes0.empty() ? 900 : (int)nodes0[0].width);
    int height = wd.present ? (int)wd.height : (nodes0.empty() ? 640 : (int)nodes0[0].height);

    BinUIAppGLGdi app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.title.c_str());

    auto invalidateNode = [&](uint16_t id) {
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    // 视口（与 HTML 里的卡片一致：left 16 / top 56 / 868 × 520）
    const float vpCx = 16 + 868 * 0.5f;
    const float vpCy = 56 + 520 * 0.5f + 20;   // 稍微下移，给标题留位置
    float spinX = 0.5f, spinY = 0.6f;

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) {
        drawNodesGLGdi(gfx, renderer, clip, nullptr);   // 先把 UI 画完
        // 再把 3D 线框画在 UI 之上（视口区域内没有别的节点，不会挡住控件）
        const float dist = 4.0f, scale = 300.0f;
        float sx[8], sy[8], sd[8];
        for (int i = 0; i < 8; i++) {
            V3 v = RotX(RotY(kVerts[i], spinY), spinX);
            Project(v, vpCx, vpCy, scale, dist, sx[i], sy[i], sd[i]);
        }
        for (const auto& e : kEdges) {
            const float depth = (sd[e[0]] + sd[e[1]]) * 0.5f;
            gfx.fillLine(sx[e[0]], sy[e[0]], sx[e[1]], sy[e[1]], 2.0f, EdgeColor(depth, dist));
        }
        // 顶点小方块，方便看清旋转
        for (int i = 0; i < 8; i++)
            gfx.fillRect(sx[i] - 3, sy[i] - 3, 6, 6, EdgeColor(sd[i] * 0.85f + dist * 0.15f, dist));
    });

    app.setUpdateFn([&](float dt) {
        spinX += dt * 0.7f;
        spinY += dt * 1.1f;
        static float acc = 0.0f;
        acc += dt;
        if (acc > 1.0f / 60.0f) {
            acc = 0.0f;
            app.invalidateAll();            // 应用自绘内容不进脏矩形体系，整窗重画
        }
    });

    app.setMoveFn([&](float x, float y) { renderer.setHover(x, y); });
    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.press(x, y);
    });
    app.setReleaseFn([&](float x, float y) {
        int id = renderer.release(x, y);
        if (id < 0) return;
        for (const auto& n : renderer.getNodes()) {
            if (n.id != (uint16_t)id) continue;
            if (n.text == "\xE2\x88\x92") app.minimize();
            else if (n.text == "\xE2\x96\xA1") app.maximize();
            else if (n.text == "\xE2\x9C\x95") app.requestExit();
            break;
        }
    });
    app.setLeaveFn([&]() { renderer.clearHover(); });
    app.setKeyFn([&](int vk) { if (vk == VK_ESCAPE) app.requestExit(); });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); }
    });

    uint16_t idState = renderer.findNodeByName("cube-state");
    if (idState) {
        renderer.setText(idState,
            "顶点变换 + 透视投影 + 逐边按深度调色，全部在应用里算；引擎只提供 fillLine / fillRect");
        invalidateNode(idState);
    }
    app.setResizeFn([&](int w, int h) { renderer.setSize(w, h); });

    std::printf("[cubedemo] 3D 演示 (OpenGL 3.3)\n");
    return app.run();
}
