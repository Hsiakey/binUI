// binui_gpu.cpp - binUI 的设备层实现（建在 binGPU 上）
//
// 这一份**不建自己的设备**：设备、交换链、管线、采样器、命令缓冲、节拍器全部来自
// binGPU。binUI 只负责"画什么"（节点树 → 原语），"用什么画、怎么画"交给 binGPU。
// 逐条对照见 binui_gpu.h 的头注释。
//
// 与 binui_gl.cpp 的对应关系（搬的时候一条条对着改的）：
//   glBindFramebuffer + glViewport + glUniform2f(viewSize)  → beginPass()（binGPU 的
//       begin_render + set_viewport 已经把"像素坐标 → NDC"这条映射接管了，而且
//       binGPU 统一成 **左上原点、y 向下**，所以原来满地的 V 轴翻转全部消失）
//   glBindTexture + glBlendFunc + glScissor                → set_texture + 管线(blend) + set_scissor
//   glDrawArrays(TRIANGLES, n*6)                           → draw_quads（每 quad 4 顶点）
//   圆角的 shapeProgram（GLSL 里算 SDF）                    → pipeline(BGPU_PIPE_SHAPE) +
//       draw_shapes（SDF 在 binGPU 的片元里，binUI 这一侧不再有任何 AA 数学）
//   glGenerateMipmap                                       → BGPU_TEXTURE_MIPMAPS
//   glCopyTexSubImage / glBlitFramebuffer 抓背景            → 把源纹理画成四边形（见下）
//   wglSwapIntervalEXT + SwapBuffers                       → bingpu_swapchain_create(VSYNC) +
//       bingpu_frame_end（present 自己掐拍）+ bingpu_pace_*
//
// 为什么"抓背景"不需要新的拷贝命令：binUI 永远画进**自己那张窗口大小的 canvas**，
// present 时才把 canvas 当纹理贴到交换链目标上。于是"把当前目标的某一块抓到别的纹理"
// 就是"把 canvas 的某块 UV 画进目标纹理"—— 普通的 textured quad，不用 blit。
#include "backends/binui_gpu.h"

#include "binui_paths.h"   // binui::ex::StartupNowMs / StartupMark

#ifndef _WIN32
#error "binui_gpu 这一版只在 Windows 上接 binGPU 的窗口交换链"
#endif

#include "bingpu.h"        // binGPU 的 C API（唯一的外部依赖）

#include <windows.h>
// windows.h 把 `DrawState` 定义成 `DrawStateA`（Win32 的 DrawState 绘图 API），而
// binui_draw_nodes.h 里通篇用 `BinUIRenderer::DrawState` —— 不卸掉这个宏，
// 整个节点树遍历都编不过（"'DrawStateA' in 'class binui::BinUIRenderer' does not
// name a type"）。这里卸掉，只影响本编译单元。
#ifdef DrawState
#undef DrawState
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "binui_font_renderer.h"
#include "binui_paint.h"
#include "binui_filter.h"
#include "backends/binui_draw_nodes.h"
#include "backends/binui_glyph_atlas.h"
#include "backends/binui_win_image.h"

using namespace binui;

namespace binui {
namespace {

// **预乘色 → 直通色**。binGPU 的顶点色是"直通"口径 —— 它的片元公式是
//
//     rgb = t.rgb * vColor.rgb * vColor.a      （binGPU 的 2D / 形状两条管线都一样）
//
// 也就是**在设备层自己乘 srcA**。而 binUI 的共享代码（字形图集、BuildGradientQuads）
// 交出来的是**预乘**色（另三个后端自建着色器，直接 frag = vcol，所以它们是预乘口径）。
// 这两个口径不能混：直接喂预乘色会被再乘一次 alpha —— 表现正好是"整体暗一档"，
// 实测 opacity 页 (127,223,127) 变成 (127,175,127)，整页差 57%。
//
// 反过来除几乎无损：片元紧接着又乘回 alpha，往返误差 ≤ 1 级。
uint32_t UnPremult(uint32_t c) {
    const uint32_t a = (c >> 24) & 0xFFu;
    if (a == 255u) return c;
    if (a == 0u) return 0u;
    uint32_t r = (((c >> 16) & 0xFFu) * 255u) / a;
    uint32_t g = (((c >> 8) & 0xFFu) * 255u) / a;
    uint32_t b = ((c & 0xFFu) * 255u) / a;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

int64_t PaceNowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// 报错只喊一次：binGPU 的 last_error 里通常已经有一句人话，刷屏没有额外信息
void WarnOnce(const char* what, BgpuResult r) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    const char* detail = bingpu_last_error();
    std::fprintf(stderr, "[binui-gpu] %s 失败：%s%s%s\n", what, bingpu_result_name(r),
                 (detail && detail[0]) ? " —— " : "", (detail && detail[0]) ? detail : "");
}

inline void Ck(BgpuResult r, const char* what) {
    if (r != BGPU_OK) WarnOnce(what, r);
}

// 单次 draw 的四边形上限：binGPU 的 max_quads_per_draw 各家不一样，
// 统一切成这个尺寸最省心（一帧的批次数量远小于它）
constexpr uint32_t kQuadsPerDraw = 16384;

// 顶点上限（单位：四边形）。binui_gl 原来是 10 万顶点（≈1.67 万四边形）起步、
// 100 万顶点封顶；这里换成四边形计数，数值等价（4 顶点/quad 而非 6）。
constexpr size_t kInitialQuads = 25000;
constexpr size_t kMaxQuads = 262144;

} // namespace

// ============================================================
// Impl
// ============================================================
struct BinUIGpu::Impl {
    HWND hwnd = nullptr;
    int winW = 0, winH = 0;
    bool vsync = true;
    bool transparent = false;
    bool ready = false;
    bool headless = false;

    // ---- 设备层（binGPU）----
    BgpuBackendKind kind = BGPU_BACKEND_AUTO;
    BgpuDevice dev = 0;
    BgpuSwapchain sc = 0;
    BgpuCmd cmd = 0;
    BgpuPacer pacer = {};
    BgpuTexture scTarget = 0;      // 本帧借来的交换链目标（frame_begin 给的，不许 destroy）
    bool inFrame = false;
    bool shapeOk = false;          // 设备支持 BGPU_CAP_SHAPE
    bool vsyncPending = false;     // vsync 改了，下一帧重建交换链

    // ---- 渲染目标 ----
    BgpuTexture canvas = 0;        // 窗口大小的持久画布（脏区局部重绘的基准）
    bool canvasUndefined = true;   // 新建 / 改尺寸 → 整张必须先清一次
    bool inPass = false;
    BgpuTexture passTarget = 0;
    int passW = 0, passH = 0;

    // ---- 管线 / 采样器 ----
    BgpuSampler sampLinear = 0;    // 双线性 + CLAMP（图集 / 图片 / 合成）
    BgpuSampler sampPoint = 0;     // 最近点 + CLAMP（1:1 抓取）
    BgpuSampler sampRepeat = 0;    // 最近点 + REPEAT（毛玻璃颗粒）
    BgpuPipeline pipe2d[5] = {};
    BgpuPipeline pipeShape[5] = {};
    BgpuTexture whiteTex = 0;
    BgpuTexture noiseTex = 0;

    // ---- 场景（两条顶点流：四边形 4 顶点/个，形状 4 顶点/个）----
    std::vector<BgpuVertex> vq;
    size_t vcount = 0;
    std::vector<BgpuShapeVertex> sq;
    size_t scount = 0;

    struct Batch {
        uint32_t first = 0;      // 四边形下标（不是顶点下标）
        uint32_t count = 0;
        int sx = 0, sy = 0, sw = 0, sh = 0;
        BgpuTexture tex = 0;
        BgpuSampler samp = 0;
        BgpuBlend blend = BGPU_BLEND_PREMULTIPLIED;
        bool shape = false;
    };
    std::vector<Batch> batches;
    size_t curBatch = static_cast<size_t>(-1);

    int scissor[4] = {0, 0, 0, 0};
    int scissorDepth = 0;
    BgpuTexture curTex = 0;
    BgpuSampler curSamp = 0;
    BgpuBlend blend = BGPU_BLEND_PREMULTIPLIED;
    size_t frameQuads = 0;

    // ---- 图层 ----
    struct LayerTarget { BgpuTexture tex = 0; bool inUse = false; };
    struct LayerFrame {
        int idx = -1;
        float x = 0, y = 0, w = 0, h = 0;
        float alpha = 1;
        float blur = 0;
        std::vector<BinUIFilterDropShadowEntry> shadows;
        BgpuTexture prevTarget = 0;
    };
    std::vector<LayerTarget> layers;
    std::vector<LayerFrame> layerStack;
    static constexpr int kMaxLayers = 4;

    static void warnLayerLimit() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr, "[binui-gpu] 图层超过 %d 层，多出来的退回逐元素乘 alpha\n", kMaxLayers);
    }

    // ---- 毛玻璃 / 模糊降采样链（按尺寸池化）----
    struct BlurLevel { BgpuTexture tex = 0; int w = 0, h = 0; };
    struct BlurSet { int w0 = 0, h0 = 0; int tick = 0; BlurLevel lv[8]; };
    std::vector<BlurSet> blurSets;
    int activeBlur = -1;
    int blurTick = 0;
    static constexpr size_t kMaxBlurSets = 16;
    bool ignoreLayerClip = false;

    // ---- 字形图集 / 图片 ----
    struct GpuGlyphHost : IGlyphTexHost {
        Impl* o = nullptr;
        void* createTex(int w, int h) override {
            // 零缓冲：图集里的空格子会被当真字形画，未初始化的显存会画成垃圾
            std::vector<uint8_t> zeros(static_cast<size_t>(w) * h * 4, 0);
            // PREMULTIPLIED 必须带上：字形图集里存的是**预乘**覆盖率（(a,a,a,a)）。
            // binGPU 的 upload 默认把直通数据预乘一遍，不声明就会被乘两次 ——
            // 表现是"文字整体淡一档"（覆盖率变成 a²），实测圆角页文字通道差 36 级。
            BgpuTexture t = o->makeTex(w, h, BGPU_FORMAT_BGRA8,
                                       BGPU_TEXTURE_DYNAMIC | BGPU_TEXTURE_PREMULTIPLIED,
                                       zeros.data(), w * 4);
            return reinterpret_cast<void*>(static_cast<uintptr_t>(t));
        }
        void updateTex(void* tex, int x, int y, int w, int h, const uint8_t* bgra) override {
            const BgpuTexture t = static_cast<BgpuTexture>(reinterpret_cast<uintptr_t>(tex));
            if (!t) return;
            BgpuBox box;
            box.x = static_cast<uint32_t>(x);
            box.y = static_cast<uint32_t>(y);
            box.w = static_cast<uint32_t>(w);
            box.h = static_cast<uint32_t>(h);
            Ck(bingpu_texture_upload(t, &box, bgra, static_cast<uint32_t>(w) * 4),
               "texture_upload(glyph)");
        }
        void destroyTex(void* tex) override {
            const BgpuTexture t = static_cast<BgpuTexture>(reinterpret_cast<uintptr_t>(tex));
            if (t && o->dev) bingpu_texture_destroy(o->dev, t);
        }
    };
    GpuGlyphHost glyphHost;
    GlyphAtlas atlas;

    // 圆角覆盖率模板（只有"仿射 / 梯度底"这两条老路还用得上；轴对齐的圆角走
    // binGPU 的解析圆角，一张模板都不要）。键 = 厚度 * 256 + 半径。
    std::unordered_map<int, BgpuTexture> roundTex;
    std::unordered_map<int, uint64_t> roundUse;
    uint64_t roundClock = 0;
    static constexpr size_t kMaxRoundTex = 48;

    BgpuTexture roundFor(int radius, int thickness) {
        int r = std::max(1, radius);
        int th = thickness < 0 ? 0 : thickness;
        if (th > r) th = r;
        const int key = th * 256 + r;
        auto it = roundTex.find(key);
        if (it != roundTex.end()) {
            roundUse[key] = ++roundClock;
            return it->second;
        }
        const int t = binui::RoundTemplateSize(r);
        std::vector<uint32_t> px(static_cast<size_t>(t) * t, 0u);
        const int built = (th == 0) ? binui::BuildRoundedTileRGBA(r, false, px.data())
                                    : binui::BuildRoundedRingRGBA(r, th, px.data());
        if (built != t) {
            std::fprintf(stderr, "[binui-gpu] 圆角模板生成失败 (%dx%d)\n", t, t);
            return 0;
        }
        // 模板是"灰度 + 覆盖率填满四个通道"= 预乘，理由同字形图集
        BgpuTexture tex = makeTex(t, t, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_PREMULTIPLIED, px.data(),
                                  t * 4);
        if (!tex) return 0;
        roundTex[key] = tex;
        roundUse[key] = ++roundClock;
        if (roundTex.size() > kMaxRoundTex) {
            // 淘汰最久未用。**不能当场 destroy**：本帧已经录进批次的四边形还引用着它 ——
            // 帧内销毁 + 复用同一个句柄会让先前那批采样到别人的内容（binui_gl 踩过）。
            int victimKey = 0;
            uint64_t victimUse = ~0ull;
            for (const auto& kv : roundUse) {
                if (kv.first == key) continue;
                if (kv.second < victimUse) { victimUse = kv.second; victimKey = kv.first; }
            }
            if (victimKey) {
                pendingTexFree.push_back(roundTex[victimKey]);
                roundTex.erase(victimKey);
                roundUse.erase(victimKey);
            }
        }
        return tex;
    }
    std::vector<BgpuTexture> pendingTexFree;

    std::unordered_map<uint64_t, BgpuTexture> images;
    std::unordered_map<uint64_t, std::pair<int, int>> imageDims;
    std::unordered_map<uint64_t, uint32_t> imageGen;
    std::unordered_map<uint64_t, uint8_t> rawMips;
    ColorMatrix imgFilter;
    uint64_t imgFilterKey = 0;
    uint64_t imgKey(uint16_t index) const {
        return static_cast<uint64_t>(index) | (imgFilterKey << 16);
    }

    // ---- 仿射状态（binUI 自己在顶点上做，不走 binGPU 的 set_affine）----
    bool affineOn = false;
    float aff[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    void affPoint(float lx, float ly, float& sx, float& sy) const {
        sx = aff[0] * lx + aff[1] * ly + aff[2];
        sy = aff[3] * lx + aff[4] * ly + aff[5];
    }
    void affCorners(float x0, float y0, float x1, float y1, float* p) const {
        affPoint(x0, y0, p[0], p[1]);
        affPoint(x1, y0, p[2], p[3]);
        affPoint(x0, y1, p[4], p[5]);
        affPoint(x1, y1, p[6], p[7]);
    }

    // ---- 分层窗口（透明窗）的 DIB ----
    HBITMAP layeredDib = nullptr;
    HDC layeredDC = nullptr;
    void* layeredBits = nullptr;
    int layeredW = 0, layeredH = 0;
    void* gdiBits = nullptr;
    HBITMAP gdiDib = nullptr;
    HDC gdiDC = nullptr;
    int gdiW = 0, gdiH = 0;

    ~Impl() { shutdown(); }

    // ============ 设备层小工具 ============
    BgpuTexture makeTex(int w, int h, BgpuFormat fmt, uint32_t flags, const void* data,
                        uint32_t stride = 0) {
        if (!dev || w <= 0 || h <= 0) return 0;
        BgpuTextureDesc td = {};
        td.struct_size = sizeof(td);
        td.width = static_cast<uint32_t>(w);
        td.height = static_cast<uint32_t>(h);
        td.format = fmt;
        td.flags = flags;
        BgpuTexture t = 0;
        if (bingpu_texture_create(dev, &td, &t) != BGPU_OK) {
            WarnOnce("texture_create", BGPU_ERR_BACKEND_FAILED);
            return 0;
        }
        if (data && bingpu_texture_upload(t, nullptr, data, stride) != BGPU_OK) {
            Ck(BGPU_ERR_BACKEND_FAILED, "texture_upload");
            bingpu_texture_destroy(dev, t);
            return 0;
        }
        return t;
    }
    void freeTex(BgpuTexture& t) {
        if (t && dev) bingpu_texture_destroy(dev, t);
        t = 0;
    }

    // ---------- pass 切换 ----------
    void endPass() {
        if (!inPass) return;
        flushAll();
        Ck(bingpu_cmd_end_render(cmd), "cmd_end_render");
        inPass = false;
        passTarget = 0;
        passW = passH = 0;
    }

    void beginPass(BgpuTexture target, int tw, int th, bool clear, uint32_t clearColor) {
        if (!cmd || !target) return;
        if (inPass && passTarget == target) return;   // 同一个目标：别重开（会丢内容）
        endPass();
        BgpuRenderBeginDesc rb = {};
        rb.struct_size = sizeof(rb);
        rb.target = target;
        rb.flags = clear ? static_cast<uint32_t>(BGPU_RENDER_CLEAR_COLOR) : 0u;
        rb.clear_color = clearColor;
        BgpuResult r = bingpu_cmd_begin_render(cmd, &rb);
        if (r != BGPU_OK) { Ck(r, "cmd_begin_render"); return; }
        inPass = true;
        passTarget = target;
        passW = tw;
        passH = th;
        Ck(bingpu_cmd_set_viewport(cmd, 0.0f, 0.0f, static_cast<float>(tw),
                                   static_cast<float>(th)), "cmd_set_viewport");
        Ck(bingpu_cmd_set_scissor(cmd, 0, 0, tw, th), "cmd_set_scissor");
        Ck(bingpu_cmd_set_affine(cmd, nullptr), "cmd_set_affine");
    }

    // ---------- 顶点流 ----------
    static void warnQuadsOverflow() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr,
                     "[binui-gpu] 四边形数撞上硬上限（%zu 个/帧），后面的图元被丢弃\n",
                     kMaxQuads);
    }

    BgpuVertex* quadRoom() {
        if (vcount + 1 > vq.size() / 4) {
            size_t cap = vq.empty() ? kInitialQuads : vq.size() / 4;
            while (cap < vcount + 1 && cap < kMaxQuads) cap *= 2;
            if (cap > kMaxQuads) cap = kMaxQuads;
            if (vcount + 1 > cap) { warnQuadsOverflow(); return nullptr; }
            vq.resize(cap * 4);
        }
        return vq.data() + vcount * 4;
    }

    BgpuShapeVertex* shapeRoom() {
        if (scount + 1 > sq.size() / 4) {
            size_t cap = sq.empty() ? 4096 : sq.size() / 4;
            while (cap < scount + 1 && cap < kMaxQuads) cap *= 2;
            if (cap > kMaxQuads) cap = kMaxQuads;
            if (scount + 1 > cap) { warnQuadsOverflow(); return nullptr; }
            sq.resize(cap * 4);
        }
        return sq.data() + scount * 4;
    }

    void beginBatch(bool shape = false) {
        if (curBatch != static_cast<size_t>(-1)) {
            const Batch& b = batches[curBatch];
            if (b.shape == shape && b.blend == blend && b.samp == curSamp &&
                b.tex == (shape ? static_cast<BgpuTexture>(0) : curTex) &&
                b.sx == scissor[0] && b.sy == scissor[1] &&
                b.sw == scissor[2] - scissor[0] && b.sh == scissor[3] - scissor[1])
                return;
        }
        Batch b;
        b.first = shape ? static_cast<uint32_t>(scount) : static_cast<uint32_t>(vcount);
        b.count = 0;
        b.sx = scissor[0];
        b.sy = scissor[1];
        b.sw = scissor[2] - scissor[0];
        b.sh = scissor[3] - scissor[1];
        b.tex = shape ? 0 : curTex;
        b.samp = curSamp;
        b.blend = blend;
        b.shape = shape;
        batches.push_back(b);
        curBatch = batches.size() - 1;
    }

    // 解析圆角：4 个 36 字节顶点，覆盖率在 binGPU 的片元里算。
    // 顶点色在这里就已经预乘好了（binUI 的惯例），binGPU 原样用。
    void emitShape(float x, float y, float w, float h, float radius, float thick,
                   uint32_t color) {
        if (!shapeOk || w <= 0.0f || h <= 0.0f) return;
        BgpuShapeVertex* o = shapeRoom();
        if (!o) return;
        beginBatch(true);
        const float hx = w * 0.5f, hy = h * 0.5f;
        const float x0 = x, x1 = x + w, y0 = y, y1 = y + h;
        o[0] = BgpuShapeVertex{ x0, y0, -hx, -hy, hx, hy, radius, thick, color };
        o[1] = BgpuShapeVertex{ x1, y0,  hx, -hy, hx, hy, radius, thick, color };
        o[2] = BgpuShapeVertex{ x0, y1, -hx,  hy, hx, hy, radius, thick, color };
        o[3] = BgpuShapeVertex{ x1, y1,  hx,  hy, hx, hy, radius, thick, color };
        scount += 1;
        batches[curBatch].count++;
    }

    void emitQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, uint32_t color) {
        if (affineOn) {
            float p[8], uv[8];
            affCorners(x0, y0, x1, y1, p);
            uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0;
            uv[4] = u0; uv[5] = v1; uv[6] = u1; uv[7] = v1;
            emitQuadUV(p, uv, color);
            return;
        }
        BgpuVertex* o = quadRoom();
        if (!o) return;
        beginBatch();
        o[0] = BgpuVertex{ x0, y0, 0.0f, u0, v0, color };
        o[1] = BgpuVertex{ x1, y0, 0.0f, u1, v0, color };
        o[2] = BgpuVertex{ x0, y1, 0.0f, u0, v1, color };
        o[3] = BgpuVertex{ x1, y1, 0.0f, u1, v1, color };
        vcount += 1;
        batches[curBatch].count++;
    }

    void emitQuadUV(const float* p, const float* uv, uint32_t color) {
        BgpuVertex* o = quadRoom();
        if (!o) return;
        beginBatch();
        o[0] = BgpuVertex{ p[0], p[1], 0.0f, uv[0], uv[1], color };
        o[1] = BgpuVertex{ p[2], p[3], 0.0f, uv[2], uv[3], color };
        o[2] = BgpuVertex{ p[4], p[5], 0.0f, uv[4], uv[5], color };
        o[3] = BgpuVertex{ p[6], p[7], 0.0f, uv[6], uv[7], color };
        vcount += 1;
        batches[curBatch].count++;
    }

    void emitQuadRot(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3, uint32_t color) {
        if (affineOn) {
            float p[8];
            affPoint(x0, y0, p[0], p[1]);
            affPoint(x1, y1, p[2], p[3]);
            affPoint(x2, y2, p[4], p[5]);
            affPoint(x3, y3, p[6], p[7]);
            const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            // 注意：这里的角序 (0,1,2,3) 是"带状"，与 emitQuadUV 的 TL/TR/BL/BR 不同。
            // r 序对 binGPU 没影响（它按 4 顶点展开两个三角形，绕序对齐即可），
            // 但 UV 必须与位置同序 —— 所以直接把四个角按顺序写，UV 用角的身份。
            BgpuVertex* o = quadRoom();
            if (!o) return;
            beginBatch();
            o[0] = BgpuVertex{ p[0], p[1], 0.0f, uv[0], uv[1], color };
            o[1] = BgpuVertex{ p[2], p[3], 0.0f, uv[2], uv[3], color };
            o[2] = BgpuVertex{ p[4], p[5], 0.0f, uv[4], uv[5], color };
            o[3] = BgpuVertex{ p[6], p[7], 0.0f, uv[6], uv[7], color };
            vcount += 1;
            batches[curBatch].count++;
            return;
        }
        BgpuVertex* o = quadRoom();
        if (!o) return;
        beginBatch();
        o[0] = BgpuVertex{ x0, y0, 0.0f, 0.0f, 0.0f, color };
        o[1] = BgpuVertex{ x1, y1, 0.0f, 1.0f, 0.0f, color };
        o[2] = BgpuVertex{ x2, y2, 0.0f, 1.0f, 1.0f, color };
        o[3] = BgpuVertex{ x3, y3, 0.0f, 0.0f, 1.0f, color };
        vcount += 1;
        batches[curBatch].count++;
    }

    void emitQuadUVColor(const BinUIGradQuad& q) {
        BgpuVertex* o = quadRoom();
        if (!o) return;
        beginBatch();
        // BuildGradientQuads 给的是**预乘**色（它在预乘空间插值，见 binui_paint.h），
        // binGPU 要直通色，所以每个角都除回去。
        o[0] = BgpuVertex{ q.x, q.y, 0.0f, q.u0, q.v0, UnPremult(q.c[0]) };
        o[1] = BgpuVertex{ q.x + q.w, q.y, 0.0f, q.u1, q.v0, UnPremult(q.c[1]) };
        o[2] = BgpuVertex{ q.x, q.y + q.h, 0.0f, q.u0, q.v1, UnPremult(q.c[2]) };
        o[3] = BgpuVertex{ q.x + q.w, q.y + q.h, 0.0f, q.u1, q.v1, UnPremult(q.c[3]) };
        vcount += 1;
        batches[curBatch].count++;
    }

    // ---------- 提交 ----------
    void flushAll() {
        if (!inPass || !cmd) return;
        if (vcount == 0 && scount == 0) return;
        frameQuads += vcount + scount;

        // 正在画进图层时，每条批次再和层框求交（层外的内容不该出现在层里）
        const LayerFrame* lf = layerStack.empty() ? nullptr : &layerStack.back();
        for (const Batch& b : batches) {
            if (b.count == 0) continue;
            int l = b.sx, t = b.sy, r = b.sx + b.sw, bot = b.sy + b.sh;
            if (lf && !ignoreLayerClip) {
                l = std::max(l, static_cast<int>(std::floor(lf->x)));
                t = std::max(t, static_cast<int>(std::floor(lf->y)));
                r = std::min(r, static_cast<int>(std::ceil(lf->x + lf->w)));
                bot = std::min(bot, static_cast<int>(std::ceil(lf->y + lf->h)));
            }
            if (r <= l || bot <= t) continue;
            Ck(bingpu_cmd_set_scissor(cmd, l, t, r - l, bot - t), "cmd_set_scissor");
            if (b.shape) {
                Ck(bingpu_cmd_set_pipeline(cmd, pipeShape[static_cast<int>(b.blend)]),
                   "cmd_set_pipeline(shape)");
                const BgpuShapeVertex* base = sq.data() + static_cast<size_t>(b.first) * 4;
                for (uint32_t done = 0; done < b.count; done += kQuadsPerDraw) {
                    const uint32_t n = std::min(kQuadsPerDraw, b.count - done);
                    Ck(bingpu_cmd_draw_shapes(cmd, base + static_cast<size_t>(done) * 4, n),
                       "cmd_draw_shapes");
                }
            } else {
                Ck(bingpu_cmd_set_pipeline(cmd, pipe2d[static_cast<int>(b.blend)]),
                   "cmd_set_pipeline");
                Ck(bingpu_cmd_set_texture(cmd, b.tex, b.samp), "cmd_set_texture");
                const BgpuVertex* base = vq.data() + static_cast<size_t>(b.first) * 4;
                for (uint32_t done = 0; done < b.count; done += kQuadsPerDraw) {
                    const uint32_t n = std::min(kQuadsPerDraw, b.count - done);
                    Ck(bingpu_cmd_draw_quads(cmd, base + static_cast<size_t>(done) * 4, n),
                       "cmd_draw_quads");
                }
            }
        }
        vcount = 0;
        scount = 0;
        batches.clear();
        curBatch = static_cast<size_t>(-1);
        // 本帧真正用完了，淘汰的圆角模板现在可以删
        for (BgpuTexture t : pendingTexFree) freeTex(t);
        pendingTexFree.clear();
    }

    // ---------- 目标 / 资源 ----------
    bool createCanvas() {
        freeTex(canvas);
        canvas = makeTex(winW, winH, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET, nullptr);
        canvasUndefined = true;
        return canvas != 0;
    }

    void destroyBlurSet(size_t idx) {
        if (idx >= blurSets.size()) return;
        for (int i = 0; i < 8; i++) freeTex(blurSets[idx].lv[i].tex);
    }
    void destroyAllBlurSets() {
        for (size_t i = 0; i < blurSets.size(); i++) destroyBlurSet(i);
        blurSets.clear();
        activeBlur = -1;
    }

    bool selectBlurSet(int w0, int h0) {
        if (w0 <= 0 || h0 <= 0) return false;
        for (size_t i = 0; i < blurSets.size(); i++) {
            if (blurSets[i].w0 == w0 && blurSets[i].h0 == h0) {
                blurSets[i].tick = ++blurTick;
                activeBlur = static_cast<int>(i);
                return true;
            }
        }
        if (blurSets.size() >= kMaxBlurSets) {
            size_t victim = 0;
            for (size_t i = 1; i < blurSets.size(); i++)
                if (blurSets[i].tick < blurSets[victim].tick) victim = i;
            destroyBlurSet(victim);
            blurSets.erase(blurSets.begin() + static_cast<long>(victim));
        }
        BlurSet s;
        s.w0 = w0;
        s.h0 = h0;
        s.tick = ++blurTick;
        blurSets.push_back(s);
        activeBlur = static_cast<int>(blurSets.size()) - 1;
        return true;
    }
    BlurLevel& blurAt(size_t i) {
        static BlurLevel dummy;
        if (activeBlur < 0) return dummy;
        BlurSet& s = blurSets[static_cast<size_t>(activeBlur)];
        if (i >= 8) { dummy = BlurLevel(); return dummy; }
        return s.lv[i];
    }
    bool ensureBlurLevel(size_t i, int w, int h) {
        if (i >= 8 || w <= 0 || h <= 0 || activeBlur < 0) return false;
        BlurLevel& b = blurAt(i);
        if (b.tex && b.w == w && b.h == h) return true;
        freeTex(b.tex);
        b.tex = makeTex(w, h, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET, nullptr);
        if (!b.tex) return false;
        b.w = w;
        b.h = h;
        return true;
    }

    static int blurLevelsFor(float blurPx) { return binui::PaintFilterBlurLevel(blurPx); }
    static int gaussTaps(float sigmaTexels, uint32_t* w, int maxN) {
        return binui::PaintFilterGaussTaps(sigmaTexels, w, maxN);
    }

    int acquireLayer() {
        for (size_t i = 0; i < layers.size(); i++)
            if (!layers[i].inUse) return static_cast<int>(i);
        if (static_cast<int>(layers.size()) >= kMaxLayers) return -1;
        LayerTarget t;
        t.tex = makeTex(winW, winH, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET, nullptr);
        if (!t.tex) return -1;
        layers.push_back(t);
        return static_cast<int>(layers.size() - 1);
    }
    void freeLayers() {
        for (LayerTarget& t : layers) freeTex(t.tex);
        layers.clear();
        layerStack.clear();
    }

    // ---------- 模糊 / 阴影 ----------
    bool layerRegion(const LayerFrame& f, int& rx, int& ry, int& rw, int& rh) const {
        int x0 = static_cast<int>(std::floor(f.x));
        int y0 = static_cast<int>(std::floor(f.y));
        int x1 = static_cast<int>(std::ceil(f.x + f.w));
        int y1 = static_cast<int>(std::ceil(f.y + f.h));
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(winW, x1);
        y1 = std::min(winH, y1);
        rx = x0; ry = y0; rw = x1 - x0; rh = y1 - y0;
        return rw >= 2 && rh >= 2;
    }

    // 分离高斯：level 上的横向 + 纵向累加（抽头权重来自 binui_paint.h 那一份）
    bool gaussBlurLevel(int level, float blurPx) {
        if (level < 0 || level >= 7) return false;
        if (activeBlur < 0) return false;
        const int lw = blurAt(static_cast<size_t>(level)).w;
        const int lh = blurAt(static_cast<size_t>(level)).h;
        const bool hasTex = blurAt(static_cast<size_t>(level)).tex != 0;
        const float sigmaPx = blurPx / static_cast<float>(1 << level);
        if (!hasTex || lw < 1 || lh < 1 || !(sigmaPx > 0.0f)) return false;
        if (!ensureBlurLevel(7, lw, lh)) return false;
        uint32_t w[33];
        const int n = gaussTaps(sigmaPx, w, 33);
        const int r = (n - 1) / 2;
        blend = BGPU_BLEND_ADDITIVE;
        ignoreLayerClip = true;
        for (int pass = 0; pass < 2; pass++) {
            BlurLevel& dst = blurAt(pass == 0 ? 7 : static_cast<size_t>(level));
            BlurLevel& from = blurAt(pass == 0 ? static_cast<size_t>(level) : 7);
            beginPass(dst.tex, lw, lh, true, 0x00000000u);
            curTex = from.tex;
            curSamp = sampLinear;
            for (int k = -r; k <= r; k++) {
                const float o = static_cast<float>(k);
                uint32_t c = (w[k + r] << 24) | 0x00FFFFFFu;
                if (pass == 0)
                    emitQuad(0.0f, 0.0f, static_cast<float>(lw), static_cast<float>(lh),
                             o / static_cast<float>(lw), 0.0f,
                             1.0f + o / static_cast<float>(lw), 1.0f, c);
                else
                    emitQuad(0.0f, 0.0f, static_cast<float>(lw), static_cast<float>(lh),
                             0.0f, o / static_cast<float>(lh),
                             1.0f, 1.0f + o / static_cast<float>(lh), c);
            }
            flushAll();
        }
        ignoreLayerClip = false;
        blend = BGPU_BLEND_PREMULTIPLIED;
        return true;
    }

    // level0（已就绪）→ 降到 blurLevelsFor(blurPx) → 该级跑分离高斯 → 按 alpha 合成回 target
    void compositeThroughLevels(float blurPx, BgpuTexture target, int rx, int ry, int rw,
                               int rh, uint32_t col) {
        const int levels = blurLevelsFor(blurPx);
        int finalLevel = 0;
        if (levels > 0) {
            blend = BGPU_BLEND_REPLACE;
            ignoreLayerClip = true;
            for (int i = 1; i <= levels; i++) {
                const int dw = std::max(1, blurAt(static_cast<size_t>(i - 1)).w / 2);
                const int dh = std::max(1, blurAt(static_cast<size_t>(i - 1)).h / 2);
                if (!ensureBlurLevel(static_cast<size_t>(i), dw, dh)) break;
                beginPass(blurAt(static_cast<size_t>(i)).tex, dw, dh, true, 0x00000000u);
                curTex = blurAt(static_cast<size_t>(i - 1)).tex;
                curSamp = sampLinear;
                emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                         0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
                flushAll();
                finalLevel = i;
            }
            ignoreLayerClip = false;
            blend = BGPU_BLEND_PREMULTIPLIED;
        }
        // 判据是"有没有 σ"，不是"降了几级"：blurPx < 4 时 levels=0 但 0 级也要卷积
        if (!gaussBlurLevel(finalLevel, blurPx)) finalLevel = levels;
        beginPass(target, winW, winH, false, 0x00000000u);
        curTex = blurAt(static_cast<size_t>(finalLevel)).tex;
        curSamp = sampLinear;
        emitQuad(static_cast<float>(rx), static_cast<float>(ry),
                 static_cast<float>(rx + rw), static_cast<float>(ry + rh),
                 0.0f, 0.0f, 1.0f, 1.0f, col);
        flushAll();
        curTex = whiteTex;
        curSamp = sampLinear;
    }

    // filter: blur —— 抓层内容 → 降采样链 → 正常 alpha 合成回原区域
    bool blurLayerContent(const LayerFrame& f, BgpuTexture layerTex, BgpuTexture prevTarget) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!layerRegion(f, rx, ry, rw, rh)) return false;
        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) return false;
        // 抓层内容 → level0：把层纹理按屏幕位置 1:1 画进去（整数对齐 + NEAREST = 精确拷贝）
        beginPass(blurAt(0).tex, rw, rh, true, 0x00000000u);
        blend = BGPU_BLEND_REPLACE;
        curTex = layerTex;
        curSamp = sampPoint;
        emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                 static_cast<float>(rx) / static_cast<float>(winW),
                 static_cast<float>(ry) / static_cast<float>(winH),
                 static_cast<float>(rx + rw) / static_cast<float>(winW),
                 static_cast<float>(ry + rh) / static_cast<float>(winH), 0xFFFFFFFFu);
        flushAll();
        blend = BGPU_BLEND_PREMULTIPLIED;
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        uint32_t col = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        compositeThroughLevels(f.blur, prevTarget, rx, ry, rw, rh, col);
        curTex = whiteTex;
        curSamp = sampLinear;
        return true;
    }

    // filter: drop-shadow —— 阴影色铺满 → 保留混合取轮廓 → 模糊 → 按偏移合成
    void drawDropShadow(const LayerFrame& f, BgpuTexture layerTex, BgpuTexture prevTarget,
                        const BinUIFilterDropShadowEntry& sh) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!layerRegion(f, rx, ry, rw, rh)) return;
        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) return;
        beginPass(blurAt(0).tex, rw, rh, true, 0x00000000u);
        ignoreLayerClip = true;
        blend = BGPU_BLEND_PREMULTIPLIED;
        {
            uint32_t c = sh.color;
            curTex = whiteTex;
            curSamp = sampPoint;
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     0.5f, 0.5f, 0.5f, 0.5f, c);
            flushAll();
            // 轮廓：dst *= srcA
            blend = BGPU_BLEND_KEEP;
            curTex = layerTex;
            curSamp = sampPoint;
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     static_cast<float>(rx) / static_cast<float>(winW),
                     static_cast<float>(ry) / static_cast<float>(winH),
                     static_cast<float>(rx + rw) / static_cast<float>(winW),
                     static_cast<float>(ry + rh) / static_cast<float>(winH), 0xFFFFFFFFu);
            flushAll();
            blend = BGPU_BLEND_PREMULTIPLIED;
        }
        ignoreLayerClip = false;
        // 偏移取整：合成四边形恒落在像素边界上（三端一致）
        const int sdx = static_cast<int>(std::lround(sh.dx));
        const int sdy = static_cast<int>(std::lround(sh.dy));
        compositeThroughLevels(sh.blur, prevTarget, rx + sdx, ry + sdy, rw, rh, 0xFFFFFFFFu);
        curTex = whiteTex;
        curSamp = sampLinear;
    }

    // ---------- 图层 ----------
    bool pushLayer(float x, float y, float w, float h, float alpha, float blur = 0.0f,
                   const BinUIFilterDropShadowEntry* shadows = nullptr, int shadowCount = 0) {
        if (!ready || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return false;
        if (static_cast<int>(layerStack.size()) >= kMaxLayers) { warnLayerLimit(); return false; }
        const BgpuTexture prev = passTarget;
        flushAll();
        const int idx = acquireLayer();
        if (idx < 0) { warnLayerLimit(); return false; }
        layers[static_cast<size_t>(idx)].inUse = true;
        LayerFrame f;
        f.idx = idx;
        f.x = x; f.y = y; f.w = w; f.h = h;
        f.alpha = alpha;
        f.blur = blur;
        if (shadows && shadowCount > 0) f.shadows.assign(shadows, shadows + shadowCount);
        f.prevTarget = prev;
        beginPass(layers[static_cast<size_t>(idx)].tex, winW, winH, true, 0x00000000u);
        layerStack.push_back(f);
        return true;
    }

    void popLayer() {
        if (layerStack.empty()) return;
        flushAll();
        const LayerFrame f = layerStack.back();
        layerStack.pop_back();
        LayerTarget& t = layers[static_cast<size_t>(f.idx)];
        for (int i = static_cast<int>(f.shadows.size()) - 1; i >= 0; i--)
            drawDropShadow(f, t.tex, f.prevTarget, f.shadows[static_cast<size_t>(i)]);
        if (f.blur > 0.0f && blurLayerContent(f, t.tex, f.prevTarget)) {
            curTex = whiteTex;
            curSamp = sampLinear;
            t.inUse = false;
            return;
        }
        beginPass(f.prevTarget, winW, winH, false, 0x00000000u);
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        const uint32_t av = static_cast<uint32_t>(a * 255.0f + 0.5f);
        uint32_t col = (av << 24) | 0x00FFFFFFu;
        blend = BGPU_BLEND_PREMULTIPLIED;
        curTex = t.tex;
        curSamp = sampPoint;
        emitQuad(f.x, f.y, f.x + f.w, f.y + f.h,
                 f.x / static_cast<float>(winW), f.y / static_cast<float>(winH),
                 (f.x + f.w) / static_cast<float>(winW),
                 (f.y + f.h) / static_cast<float>(winH), col);
        flushAll();
        curTex = whiteTex;
        curSamp = sampLinear;
        t.inUse = false;
    }

    // ---------- 噪声（毛玻璃颗粒）----------
    BgpuTexture ensureNoiseTex() {
        if (noiseTex) return noiseTex;
        const int N = 64;
        std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
        uint32_t st = 0x9E3779B9u;   // 固定种子：三端同一张噪声
        for (int i = 0; i < N * N; i++) {
            st = st * 1664525u + 1013904223u;
            const uint8_t a = static_cast<uint8_t>((st >> 16) & 0xFFu);
            const size_t o = static_cast<size_t>(i) * 4;
            px[o + 0] = a;
            px[o + 1] = a;
            px[o + 2] = a;
            px[o + 3] = a;
        }
        noiseTex = makeTex(N, N, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_PREMULTIPLIED, px.data(),
                           N * 4);
        return noiseTex;
    }

    // ---------- 图片 ----------
    bool uploadImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
        auto it = images.find(imgKey(index));
        auto git = imageGen.find(imgKey(index));
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != 0;

        int w = 0, h = 0;
        std::vector<uint8_t> px = DecodeImagePixels(data, size, w, h);
        if (!px.empty() && imgFilterKey) ApplyColorMatrixBGRA(px.data(), w, h, imgFilter);
        if (px.empty()) {
            images[imgKey(index)] = 0;
            imageGen[imgKey(index)] = generation;
            return false;
        }
        BgpuTexture& slot = images[imgKey(index)];
        freeTex(slot);
        slot = makeTex(w, h, BGPU_FORMAT_BGRA8, BGPU_TEXTURE_MIPMAPS, px.data(), w * 4);
        imageDims[imgKey(index)] = { w, h };
        imageGen[imgKey(index)] = generation;
        return slot != 0;
    }

    bool uploadImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                        uint32_t generation) {
        auto it = images.find(imgKey(index));
        auto git = imageGen.find(imgKey(index));
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != 0;
        if (w <= 0 || h <= 0 || !rgba) return false;
        BgpuTexture& slot = images[imgKey(index)];
        freeTex(slot);
        slot = makeTex(w, h, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_DYNAMIC, rgba, w * 4);
        imageDims[imgKey(index)] = { w, h };
        rawMips.erase(imgKey(index));
        imageGen[imgKey(index)] = generation;
        return slot != 0;
    }

    // ---------- 设备/交换链 ----------
    bool createSwapchain() {
        if (sc) { bingpu_swapchain_destroy(dev, sc); sc = 0; }
        BgpuSwapchainDesc sd = {};
        sd.struct_size = sizeof(sd);
        sd.native_window = headless ? nullptr : hwnd;
        sd.width = static_cast<uint32_t>(winW);
        sd.height = static_cast<uint32_t>(winH);
        sd.flags = vsync ? static_cast<uint32_t>(BGPU_DEVICE_VSYNC) : 0u;
        sd.format = BGPU_FORMAT_RGBA8;
        const BgpuResult r = bingpu_swapchain_create(dev, &sd, &sc);
        if (r != BGPU_OK) {
            WarnOnce("swapchain_create", r);
            sc = 0;
            return false;
        }
        return true;
    }

    bool init(void* nativeWindow, int width, int height, bool vsyncOn) {
        hwnd = static_cast<HWND>(nativeWindow);
        winW = width;
        winH = height;
        vsync = vsyncOn;
        headless = (hwnd == nullptr);

        // 后端由 binGPU 决定（BINUI_GPUBACKEND 只用于对照构建）。
        //
        // 0.36.0 曾经钉在 GL：当时 binGPU 的 VK 后端在"一帧里多次 begin_render/end_render、
        // 把刚当渲染目标的纹理再当采样源"这条路上是坏的（根因是顶点流扩容时把已经录进
        // 命令缓冲的 VkBuffer 当场销毁，见 binGPU 0.13.0）。**0.13.0 修掉之后这颗钉子
        // 就拔了** —— 现在按 bingpu_render_plan 走（VK > GL > D3D11），实测 VK 与 GL
        // 在 4 个对照页面上 0 像素差异（circle 0、opacity 0、svg-filter 0/max6、
        // appshow 整页 0/max7）。
        BgpuBackendKind want = BGPU_BACKEND_AUTO;
        if (const char* env = std::getenv("BINUI_GPUBACKEND")) {
            if (!std::strcmp(env, "gl")) want = BGPU_BACKEND_GL;
            else if (!std::strcmp(env, "vulkan") || !std::strcmp(env, "vk"))
                want = BGPU_BACKEND_VULKAN;
            else if (!std::strcmp(env, "d3d11") || !std::strcmp(env, "d3d"))
                want = BGPU_BACKEND_D3D11;
            else if (!std::strcmp(env, "cpu")) want = BGPU_BACKEND_CPU;
        }
        BgpuRenderPlan plan = {};
        plan.struct_size = sizeof(plan);
        if (bingpu_render_plan(want, &plan) != BGPU_OK) plan.backend = want;
        kind = plan.backend;

        const double t0 = binui::ex::StartupNowMs();
        BgpuDeviceDesc dd = {};
        dd.struct_size = sizeof(dd);
        dd.backend = kind;
        dd.native_window = headless ? nullptr : hwnd;
        dd.width = static_cast<uint32_t>(winW);
        dd.height = static_cast<uint32_t>(winH);
        dd.flags = static_cast<uint32_t>(BGPU_DEVICE_VALIDATE) |
                   (headless ? static_cast<uint32_t>(BGPU_DEVICE_HEADLESS) : 0u) |
                   (vsync ? static_cast<uint32_t>(BGPU_DEVICE_VSYNC) : 0u);
        // MSAA：binGPU 0.14.0 起四端能真开（VK/GL 到 8×），但**多目标 / 图层的路径上
        // 还没验通** —— `svg-filter` 页（图层 + 模糊 + 合成）在 4× 下整页空白，
        // 对着 `docs/pixels` 是 100% 差异、最大通道差 255，而 1× 下同一页是 0 差异。
        // 这个后端的定位是过渡期的参照实现（它会被 bin2d 的 GPU 执行体取代），
        // 所以这里先钉 1×：那是 binGPU 的账（多采样 + 多目标 resolve），记在
        // docs/backends.md 的欠账里，**不在 binUI 里自己补 AA**。
        // 真 mip 不受影响（它是逐纹理的，image-mip 页已从 37% 收到 0 差异）。
        dd.msaa_samples = 1u;
        BgpuResult r = bingpu_device_create(&dd, &dev);
        if (r != BGPU_OK) {
            WarnOnce("device_create", r);
            std::fprintf(stderr, "[binui-gpu] 设备起不来（backend=%s, hwnd=%p, %dx%d）：%s\n",
                         bingpu_backend_name(kind), static_cast<void*>(hwnd), winW, winH,
                         bingpu_last_error());
            return false;
        }
        binui::ex::StartupMark(t0, "binGPU device");

        BgpuCaps caps = {};
        caps.struct_size = sizeof(caps);
        Ck(bingpu_device_caps(dev, &caps), "device_caps");
        shapeOk = (caps.flags & BGPU_CAP_SHAPE) != 0;

        if (!createSwapchain()) return false;
        binui::ex::StartupMark(t0, "binGPU swapchain");

        Ck(bingpu_cmd_create(dev, &cmd), "cmd_create");
        if (!cmd) return false;

        // 采样器
        BgpuSamplerDesc lsd = {};
        lsd.struct_size = sizeof(lsd);
        lsd.min_filter = BGPU_FILTER_LINEAR;
        lsd.mag_filter = BGPU_FILTER_LINEAR;
        lsd.wrap_u = BGPU_WRAP_CLAMP;
        lsd.wrap_v = BGPU_WRAP_CLAMP;
        Ck(bingpu_sampler_create(dev, &lsd, &sampLinear), "sampler(linear)");
        BgpuSamplerDesc psd = lsd;
        psd.min_filter = BGPU_FILTER_NEAREST;
        psd.mag_filter = BGPU_FILTER_NEAREST;
        Ck(bingpu_sampler_create(dev, &psd, &sampPoint), "sampler(point)");
        BgpuSamplerDesc rsd = psd;
        rsd.wrap_u = BGPU_WRAP_REPEAT;
        rsd.wrap_v = BGPU_WRAP_REPEAT;
        Ck(bingpu_sampler_create(dev, &rsd, &sampRepeat), "sampler(repeat)");

        // 管线：5 种混合 × {2D, 形状}。混合语义与 binui_gl 的 glBlendFunc 一一对应。
        for (int b = 0; b < 5; b++) {
            BgpuPipelineDesc pd = {};
            pd.struct_size = sizeof(pd);
            pd.vertex_layout = BGPU_VERTEX_2D;
            pd.blend = static_cast<BgpuBlend>(b);
            pd.flags = BGPU_PIPE_TEXTURED;
            Ck(bingpu_pipeline_create(dev, &pd, &pipe2d[b]), "pipeline(2d)");
            if (shapeOk) {
                BgpuPipelineDesc sd2 = pd;
                sd2.vertex_layout = BGPU_VERTEX_SHAPE;
                sd2.flags = BGPU_PIPE_SHAPE;
                Ck(bingpu_pipeline_create(dev, &sd2, &pipeShape[b]), "pipeline(shape)");
            }
        }
        if (!shapeOk)
            std::fprintf(stderr, "[binui-gpu] 设备没有 BGPU_CAP_SHAPE，圆角退回覆盖率模板\n");

        const uint8_t white[4] = { 255, 255, 255, 255 };
        whiteTex = makeTex(1, 1, BGPU_FORMAT_RGBA8, 0, white, 4);
        if (!whiteTex) return false;
        binui::ex::StartupMark(t0, "binGPU pipelines");

        if (!createCanvas()) return false;
        scissor[0] = 0; scissor[1] = 0; scissor[2] = winW; scissor[3] = winH;
        curTex = whiteTex;
        curSamp = sampLinear;

        bingpu_pace_init(&pacer, headless ? nullptr : hwnd);

        glyphHost.o = this;
        atlas.setHost(&glyphHost);
        atlas.initFonts();
        binui::ex::StartupMark(t0, "font atlas initFonts");

        ready = true;
        binui::ex::StartupMark(t0, "binui-gpu init total");
        return true;
    }

    void shutdown() {
        atlas.clear();
        atlas.setHost(nullptr);
        for (auto& kv : roundTex) freeTex(kv.second);
        roundTex.clear();
        roundUse.clear();
        pendingTexFree.clear();
        freeTex(noiseTex);
        for (auto& kv : images) freeTex(kv.second);
        images.clear();
        imageDims.clear();
        imageGen.clear();
        rawMips.clear();
        destroyAllBlurSets();
        freeLayers();
        freeTex(canvas);
        freeTex(whiteTex);
        if (cmd && dev) { bingpu_cmd_destroy(dev, cmd); cmd = 0; }
        if (sc && dev) { bingpu_swapchain_destroy(dev, sc); sc = 0; }
        if (dev) { bingpu_device_destroy(dev); dev = 0; }
        if (layeredDib) { DeleteObject(layeredDib); layeredDib = nullptr; }
        if (layeredDC) { DeleteDC(layeredDC); layeredDC = nullptr; layeredBits = nullptr; }
        if (gdiDib) { DeleteObject(gdiDib); gdiDib = nullptr; }
        if (gdiDC) { DeleteDC(gdiDC); gdiDC = nullptr; gdiBits = nullptr; }
        inPass = false;
        inFrame = false;
        ready = false;
    }

    // 画布 → 交换链目标（1:1 贴一张）。present 和 presentViaGdi 都要走它：
    // binGPU 的 frame_read 读的是**交换链那一帧**，所以想读回像素就必须先把画布贴上去。
    void blitCanvasToScreen() {
        if (!scTarget) return;
        beginPass(scTarget, winW, winH, false, 0x00000000u);
        blend = BGPU_BLEND_REPLACE;
        curTex = canvas;
        curSamp = sampPoint;
        scissor[0] = 0; scissor[1] = 0; scissor[2] = winW; scissor[3] = winH;
        beginBatch();
        emitQuad(0.0f, 0.0f, static_cast<float>(winW), static_cast<float>(winH),
                 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
        flushAll();
        endPass();
        blend = BGPU_BLEND_PREMULTIPLIED;
        curSamp = sampLinear;
    }

    // ---------- 读回 ----------
    // binGPU 的 texture_read / frame_read 给的是**直通**颜色；binUI 的分层窗口与 GDI
    // 贴屏要的是**预乘 BGRA**（AC_SRC_ALPHA / SetDIBitsToDevice 都是这个口径）。
    // 直通→预乘会让自己算一遍（低 alpha 处 ±1 级），所以只在"读回"这条路上发生。
    bool readStraightRGBA(std::vector<uint8_t>& out, uint32_t& stride) {
        out.resize(static_cast<size_t>(winW) * winH * 4);
        // 一律走 frame_read：调用点都是"刚 frame_end 完"，那正是 binGPU 回读的时机
        // （窗口交换链在 presentFrame 里已经把像素抄了一份）。
        const BgpuResult r = bingpu_frame_read(sc, out.data(), out.size(), &stride);
        if (r != BGPU_OK) { WarnOnce("frame_read", r); return false; }
        if (stride == 0) stride = static_cast<uint32_t>(winW) * 4;
        return true;
    }

    static void straightRgbaToPremulBgra(uint8_t* dst, const uint8_t* src, int w, int h,
                                         uint32_t stride) {
        for (int y = 0; y < h; y++) {
            const uint8_t* s = src + static_cast<size_t>(y) * stride;
            uint8_t* d = dst + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
            for (int x = 0; x < w; x++, s += 4, d += 4) {
                const uint32_t a = s[3];
                d[0] = static_cast<uint8_t>((s[2] * a) / 255u);
                d[1] = static_cast<uint8_t>((s[1] * a) / 255u);
                d[2] = static_cast<uint8_t>((s[0] * a) / 255u);
                d[3] = static_cast<uint8_t>(a);
            }
        }
    }

    bool ensureLayeredDib(int w, int h) {
        if (layeredDib && layeredW == w && layeredH == h) return true;
        if (layeredDib) { DeleteObject(layeredDib); layeredDib = nullptr; }
        if (layeredDC) { DeleteDC(layeredDC); layeredDC = nullptr; }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        layeredDib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &layeredBits, nullptr, 0);
        layeredDC = CreateCompatibleDC(nullptr);
        layeredW = w;
        layeredH = h;
        return layeredDib != nullptr && layeredBits != nullptr;
    }

    bool ensureGdiDib(int w, int h) {
        if (gdiDib && gdiW == w && gdiH == h) return true;
        if (gdiDib) { DeleteObject(gdiDib); gdiDib = nullptr; }
        if (gdiDC) { DeleteDC(gdiDC); gdiDC = nullptr; }
        gdiDC = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        gdiDib = CreateDIBSection(gdiDC, &bi, DIB_RGB_COLORS, &gdiBits, nullptr, 0);
        if (gdiDib) SelectObject(gdiDC, gdiDib);
        gdiW = w;
        gdiH = h;
        return gdiDib != nullptr && gdiBits != nullptr;
    }
};

// ============================================================
// BinUIGpu 外壳
// ============================================================
BinUIGpu::BinUIGpu() : impl_(new Impl()) {}
BinUIGpu::~BinUIGpu() { delete impl_; }

bool BinUIGpu::init(void* hwnd, int width, int height, bool vsync) {
    return impl_->init(hwnd, width, height, vsync);
}
void BinUIGpu::shutdown() { impl_->shutdown(); }
bool BinUIGpu::isReady() const { return impl_->ready; }
int BinUIGpu::width() const { return impl_->winW; }
int BinUIGpu::height() const { return impl_->winH; }
const char* BinUIGpu::backendName() const { return bingpu_backend_name(impl_->kind); }

void BinUIGpu::setVsync(bool on) {
    if (impl_->vsync == on) return;
    impl_->vsync = on;
    impl_->vsyncPending = true;   // 交换链的 present 模式是建的时候定的，下一帧重建
}

void BinUIGpu::resize(int width, int height) {
    Impl& d = *impl_;
    if (!d.ready || (width == d.winW && height == d.winH)) return;
    d.winW = width;
    d.winH = height;
    d.endPass();
    if (d.sc) bingpu_swapchain_resize(d.sc, static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height));
    d.freeLayers();
    d.destroyAllBlurSets();
    d.createCanvas();
    d.scissor[0] = 0; d.scissor[1] = 0; d.scissor[2] = width; d.scissor[3] = height;
}

void BinUIGpu::setTransparent(bool t) { impl_->transparent = t; }

bool BinUIGpu::presentPacesFrames() const {
    if (!impl_->ready || impl_->transparent || impl_->headless) return false;
    return bingpu_present_paces_frames(impl_->sc) != 0;
}

bool BinUIGpu::paceAvailable() const { return impl_->ready && impl_->dev != 0; }
double BinUIGpu::paceFrameMs() const { return bingpu_pace_frame_ms(&impl_->pacer); }
long long BinUIGpu::paceWaitUs() {
    return static_cast<long long>(bingpu_pace_wait_us(&impl_->pacer, PaceNowUs()));
}
double BinUIGpu::paceRefreshMs() const {
    return impl_->pacer.refresh_hz > 0 ? 1000.0 / impl_->pacer.refresh_hz : 0.0;
}
void BinUIGpu::setPacePolicyText(const char* text) {
    BgpuPacePolicy p = BGPU_PACE_STANDARD;
    if (bingpu_pace_policy_parse(text, &p)) bingpu_pace_set_policy(&impl_->pacer, p);
}
const char* BinUIGpu::pacePolicyName() const {
    return bingpu_pace_policy_name(bingpu_pace_policy(&impl_->pacer));
}

void BinUIGpu::beginFrame(const void* clipRect) {
    Impl& d = *impl_;
    if (!d.ready) return;
    if (d.vsyncPending) { d.vsyncPending = false; d.createSwapchain(); }

    d.vcount = 0;
    d.scount = 0;
    d.frameQuads = 0;
    d.batches.clear();
    d.curBatch = static_cast<size_t>(-1);
    d.scissorDepth = 0;
    d.curTex = d.whiteTex;
    d.curSamp = d.sampLinear;
    d.blend = BGPU_BLEND_PREMULTIPLIED;
    d.scissor[0] = 0; d.scissor[1] = 0; d.scissor[2] = d.winW; d.scissor[3] = d.winH;

    BgpuResult r = bingpu_frame_begin(d.sc, d.cmd, &d.scTarget);
    if (r != BGPU_OK) { WarnOnce("frame_begin", r); d.scTarget = 0; return; }
    d.inFrame = true;
    bingpu_pace_frame_begin(&d.pacer, PaceNowUs());

    // 画布：新建/改尺寸后第一次必须整张清一次（内容未定义），之后靠脏区局部重绘
    const uint32_t bg = d.transparent ? 0x00000000u : 0xFFFFFFFFu;
    d.beginPass(d.canvas, d.winW, d.winH, d.canvasUndefined, bg);
    d.canvasUndefined = false;

    if (clipRect) {
        const int* c = static_cast<const int*>(clipRect);
        int l = c[0], t = c[1], rr = c[2], b = c[3];
        if (rr > l && b > t && l >= 0 && t >= 0 && rr <= d.winW && b <= d.winH) {
            // 脏区清成底色：用 REPLACE 混合画一个矩形 —— 与 binui_gl 的 scissor+clear 等价
            d.blend = BGPU_BLEND_REPLACE;
            d.curTex = d.whiteTex;
            d.curSamp = d.sampPoint;
            d.scissor[0] = l; d.scissor[1] = t; d.scissor[2] = l + (rr - l);
            d.scissor[3] = t + (b - t);
            d.beginBatch();
            d.emitQuad(static_cast<float>(l), static_cast<float>(t),
                       static_cast<float>(rr), static_cast<float>(b),
                       0.5f, 0.5f, 0.5f, 0.5f, bg);
            d.flushAll();
            d.blend = BGPU_BLEND_PREMULTIPLIED;
            d.curSamp = d.sampLinear;
            d.scissor[0] = 0; d.scissor[1] = 0; d.scissor[2] = d.winW; d.scissor[3] = d.winH;
        }
    }
}

void BinUIGpu::present() {
    Impl& d = *impl_;
    if (!d.ready) return;
    const int64_t t0 = PaceNowUs();

    d.flushAll();
    d.endPass();

    // 画布 → 交换链目标。透明窗口也照画：binGPU 的 frame_read 只能读到 present 过的
    // 那一帧，分层窗口要靠它拿像素。
    d.blitCanvasToScreen();

    if (d.inFrame) {
        BgpuResult r = bingpu_frame_end(d.sc, d.cmd);
        if (r != BGPU_OK) WarnOnce("frame_end", r);
        d.inFrame = false;
    }
    const int64_t t1 = PaceNowUs();
    bingpu_pace_frame_end(&d.pacer, t1, static_cast<double>(t1 - t0) / 1000.0);

    if (d.transparent) {
        std::vector<uint8_t> px;
        uint32_t stride = 0;
        if (d.readStraightRGBA(px, stride) && d.ensureLayeredDib(d.winW, d.winH)) {
            Impl::straightRgbaToPremulBgra(static_cast<uint8_t*>(d.layeredBits), px.data(),
                                           d.winW, d.winH, stride);
            HDC screenDC = GetDC(nullptr);
            HGDIOBJ old = SelectObject(d.layeredDC, d.layeredDib);
            RECT wr;
            GetWindowRect(d.hwnd, &wr);
            POINT dst = { wr.left, wr.top };
            SIZE sz = { d.winW, d.winH };
            POINT src = { 0, 0 };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(d.hwnd, screenDC, &dst, &sz, d.layeredDC, &src, 0, &bf,
                                ULW_ALPHA);
            SelectObject(d.layeredDC, old);
            ReleaseDC(nullptr, screenDC);
        }
    }
}

// 只提交不呈现（基准工具用来把 CPU 侧和 present 分开量）
void BinUIGpu::flushFrame() {
    if (!impl_->ready) return;
    impl_->flushAll();
}

// GDI 贴屏（resize 拖拽期间绕开驱动 present 路径）
void BinUIGpu::presentViaGdi() {
    Impl& d = *impl_;
    if (!d.ready) return;
    const bool perfOn = Perf().on;
    const double p0 = perfOn ? NowMs() : 0.0;

    d.flushAll();
    d.endPass();
    d.blitCanvasToScreen();
    if (d.inFrame) {
        BgpuResult r = bingpu_frame_end(d.sc, d.cmd);
        if (r != BGPU_OK) WarnOnce("frame_end(gdi)", r);
        d.inFrame = false;
        bingpu_pace_frame_end(&d.pacer, PaceNowUs(), static_cast<double>(NowMs() - p0));
    }
    if (!d.ensureGdiDib(d.winW, d.winH)) return;

    std::vector<uint8_t> px;
    uint32_t stride = 0;
    if (!d.readStraightRGBA(px, stride)) return;
    Impl::straightRgbaToPremulBgra(static_cast<uint8_t*>(d.gdiBits), px.data(), d.winW, d.winH,
                                   stride);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = d.winW;
    bi.bmiHeader.biHeight = -d.winH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC wdc = GetDC(d.hwnd);
    SetDIBitsToDevice(wdc, 0, 0, d.winW, d.winH, 0, 0, 0, d.winH, d.gdiBits, &bi,
                      DIB_RGB_COLORS);
    ReleaseDC(d.hwnd, wdc);
    if (perfOn) Perf().sumDib += NowMs() - p0;
}

// ============================================================
// 原语
// ============================================================
void BinUIGpu::fillRect(float x, float y, float w, float h, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    d.curTex = d.whiteTex;
    d.curSamp = d.sampPoint;
    d.emitQuad(x, y, x + w, y + h, 0.5f, 0.5f, 0.5f, 0.5f, color);
}

bool BinUIGpu::fillGradientQuad(float x, float y, float w, float h, float radius,
                                float angleDeg, uint32_t from, uint32_t to) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0.0f || h <= 0.0f) return false;
    const float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;
    BgpuTexture tex = d.whiteTex;
    int tpl = 0;
    if (r > 0.5f) {
        const int rr = std::max(1, static_cast<int>(std::lround(r)));
        tex = d.roundFor(rr, 0);
        if (!tex) return false;          // 没模板：交回条带路径（像素不变）
        tpl = binui::RoundTemplateSize(rr);
    }
    BinUIGradQuad q[9];
    const int n = binui::BuildGradientQuads(x, y, w, h, r, tpl, angleDeg, from, to, q, 9);
    if (n <= 0) return false;
    d.curTex = tex;
    d.curSamp = d.sampLinear;
    for (int i = 0; i < n; i++) d.emitQuadUVColor(q[i]);
    d.curTex = d.whiteTex;
    return true;
}

void BinUIGpu::fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || width <= 0) return;
    if (!d.affineOn) {
        // 非仿射：走 CPU 覆盖率条带（与填充同一条路，三端一致；见 binui_gl 的同一段注释）
        const uint64_t lkey = binui::MakeLineBandKey(x0, y0, x1, y1, width, color);
        auto& lcache = binui::LineBandCache();
        auto lit = lcache.find(lkey);
        const std::vector<binui::PaintBand>* lbands = nullptr;
        if (lit != lcache.end()) {
            lbands = &lit->second;
        } else {
            static thread_local std::vector<binui::PaintBand> lineBands;
            if (lineBands.size() < static_cast<size_t>(binui::kMaxPathBands))
                lineBands.resize(binui::kMaxPathBands);
            const int n = binui::BuildLineBandsAA(x0, y0, x1, y1, width, color,
                                                  lineBands.data(),
                                                  static_cast<int>(lineBands.size()));
            if (lcache.size() >= binui::kLineBandCacheCap) lcache.clear();
            std::vector<binui::PaintBand>& slot = lcache[lkey];
            slot.assign(lineBands.begin(), lineBands.begin() + n);
            lbands = &slot;
        }
        for (size_t i = 0; i < lbands->size(); i++)
            fillRect((*lbands)[i].x, (*lbands)[i].y, (*lbands)[i].w, (*lbands)[i].h,
                     (*lbands)[i].color);
        return;
    }
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float px = -dy / len * width * 0.5f;
    const float py = dx / len * width * 0.5f;
    d.curTex = d.whiteTex;
    d.curSamp = d.sampPoint;
    d.emitQuadRot(x0 + px, y0 + py, x1 + px, y1 + py,
                  x1 - px, y1 - py, x0 - px, y0 - py, color);
}

void BinUIGpu::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                             uint32_t color, const float xf[6]) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    const int layers = binui::RoundedLayerCount(w, h, radius, stroke, thickness);
    int emitted = 0;
    for (int k = 0; k < layers; ++k) {
        binui::RoundedLayer L;
        if (!binui::BuildRoundedLayer(0.0f, 0.0f, w, h, radius, stroke, k, L)) break;
        const int th = stroke ? static_cast<int>(std::lround(std::max(1.0f, thickness))) : 0;
        int thClamped = th;
        if (thClamped > L.tileRadius) thClamped = L.tileRadius;
        BgpuTexture tex = d.roundFor(L.tileRadius, thClamped);
        if (!tex) break;
        binui::RoundedQuadXf q[9];
        const int n = binui::BuildRoundedLayerQuadsXf(L, binui::RoundTemplateSize(L.tileRadius),
                                                     xf, q, 9);
        if (n <= 0) break;
        d.curTex = tex;
        d.curSamp = d.sampLinear;
        for (int i = 0; i < n; i++) {
            d.emitQuadUV(q[i].pos, q[i].uv, color);
            emitted++;
        }
    }
    d.curTex = d.whiteTex;
    if (emitted == 0) {
        // 模板不可用：至少把变换后的外框画出来
        const float corners[8] = { 0, 0, w, 0, 0, h, w, h };
        float pos[8], uv[8];
        for (int v = 0; v < 4; v++) {
            const float lx = corners[v * 2], ly = corners[v * 2 + 1];
            pos[v * 2] = xf[0] * lx + xf[1] * ly + xf[2];
            pos[v * 2 + 1] = xf[3] * lx + xf[4] * ly + xf[5];
            uv[v * 2] = 0.5f;
            uv[v * 2 + 1] = 0.5f;
        }
        d.emitQuadUV(pos, uv, color);
    }
}

void BinUIGpu::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    // 解析圆角：覆盖率在 **binGPU 的片元着色器**里算（binUI 这一侧没有 AA 数学）。
    // 只在没有仿射时走 —— 旋转/斜切会把局部坐标非均匀拉伸，距离场不知道这件事。
    if (d.shapeOk && !d.affineOn) {
        const float rf = std::min(radius, std::min(w, h) * 0.5f);
        if (rf <= 0.0f) { fillRect(x, y, w, h, color); return; }
        uint32_t c = color;
        d.emitShape(x, y, w, h, rf, 0.0f, c);
        return;
    }
    binui::RoundedLayer L;
    if (!binui::BuildRoundedLayer(x, y, w, h, radius, false, 0, L)) {
        fillRect(x, y, w, h, color);
        return;
    }
    BgpuTexture tex = d.roundFor(L.tileRadius, 0);
    if (!tex) { fillRect(x, y, w, h, color); return; }
    binui::PaintQuad q[9];
    const int n = binui::BuildRoundedLayerQuads(L, binui::RoundTemplateSize(L.tileRadius), q, 9);
    if (n <= 0) { fillRect(x, y, w, h, color); return; }
    d.curTex = tex;
    d.curSamp = d.sampLinear;
    for (int i = 0; i < n; i++)
        d.emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color);
    d.curTex = d.whiteTex;
}

void BinUIGpu::strokeRounded(float x, float y, float w, float h, float radius,
                             float thickness, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) {
        const float t = thickness;
        fillRect(x, y, w, t, color);
        fillRect(x, y + h - t, w, t, color);
        if (h - 2.0f * t > 0.0f) {
            fillRect(x, y + t, t, h - 2.0f * t, color);
            fillRect(x + w - t, y + t, t, h - 2.0f * t, color);
        }
        return;
    }
    // 解析圆角环带：`|d + t/2| - t/2` 就是到带子中心线的距离，厚度是浮点原值
    if (d.shapeOk && !d.affineOn) {
        uint32_t c = color;
        d.emitShape(x, y, w, h, rf, thickness, c);
        return;
    }
    int th = static_cast<int>(std::lround(std::max(1.0f, thickness)));
    const int tileRadius = std::max(1, static_cast<int>(std::lround(rf)));
    if (th > tileRadius) th = tileRadius;
    BgpuTexture tex = d.roundFor(tileRadius, th);
    if (!tex) {
        fillRect(x, y, w, thickness, color);
        fillRect(x, y + h - thickness, w, thickness, color);
        return;
    }
    binui::RoundedLayer L;
    L.x = x; L.y = y; L.w = w; L.h = h; L.r = rf;
    L.tileRadius = tileRadius;
    binui::PaintQuad q[9];
    const int n = binui::BuildRoundedLayerQuads(L, binui::RoundTemplateSize(tileRadius), q, 9);
    if (n <= 0) return;
    d.curTex = tex;
    d.curSamp = d.sampLinear;
    for (int i = 0; i < n; i++)
        d.emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color);
    d.curTex = d.whiteTex;
}

void BinUIGpu::setLetterSpacing(float px) { impl_->atlas.setLetterSpacing(px); }
void BinUIGpu::setTextSubpixel(bool on) { impl_->atlas.snapText = !on; }

void BinUIGpu::setTextShadows(const BinUITextShadow* layers, int count) {
    impl_->atlas.setTextShadows(layers, count);
}

bool BinUIGpu::setAffine(const float* xf) {
    Impl& d = *impl_;
    if (!xf || (xf[0] == 1.0f && xf[1] == 0.0f && xf[2] == 0.0f &&
                xf[3] == 0.0f && xf[4] == 1.0f && xf[5] == 0.0f)) {
        d.affineOn = false;
        return true;
    }
    for (int i = 0; i < 6; i++) d.aff[i] = xf[i];
    d.affineOn = true;
    return true;
}

void BinUIGpu::setImageFilter(const BinUIImageFilter* f) {
    Impl& d = *impl_;
    d.imgFilterKey = binui::ImageFilterKey(f);
    if (!d.imgFilterKey) return;
    for (int i = 0; i < 9; i++) d.imgFilter.m[i] = f->m[i];
    for (int i = 0; i < 3; i++) d.imgFilter.o[i] = f->offset[i];
}

bool BinUIGpu::setEraseBlend(bool on) {
    Impl& d = *impl_;
    if (!d.ready) return false;
    d.flushAll();
    d.blend = on ? BGPU_BLEND_ERASE : BGPU_BLEND_PREMULTIPLIED;
    return true;
}

bool BinUIGpu::setKeepBlend(bool on) {
    Impl& d = *impl_;
    if (!d.ready) return false;
    d.flushAll();
    d.blend = on ? BGPU_BLEND_KEEP : BGPU_BLEND_PREMULTIPLIED;
    return true;
}

bool BinUIGpu::backdropBlur(float x, float y, float w, float h, float radius, float blurPx,
                            float grain) {
    Impl& d = *impl_;
    if (!d.ready) return false;
    if (blurPx <= 0.0f || w < 2.0f || h < 2.0f) return false;
    int rx = static_cast<int>(std::floor(x));
    int ry = static_cast<int>(std::floor(y));
    int rx1 = static_cast<int>(std::ceil(x + w));
    int ry1 = static_cast<int>(std::ceil(y + h));
    rx = std::max(0, rx);
    ry = std::max(0, ry);
    rx1 = std::min(d.winW, rx1);
    ry1 = std::min(d.winH, ry1);
    const int rw = rx1 - rx, rh = ry1 - ry;
    if (rw < 2 || rh < 2) return false;

    int levels = 0;
    while (levels < 5 && blurPx / static_cast<float>(1 << (levels + 1)) >= 2.0f) levels++;

    d.flushAll();   // 手上攒的图元先落到当前目标（它们构成"背景"）
    const BgpuTexture prevTarget = d.passTarget;
    if (!prevTarget) return false;

    // 1) 抓背景：当前目标 → level0（整数对齐的 1:1 拷贝，NEAREST 采样）
    if (!d.selectBlurSet(rw, rh) || !d.ensureBlurLevel(0, rw, rh)) return false;
    d.beginPass(d.blurAt(0).tex, rw, rh, true, 0x00000000u);
    d.blend = BGPU_BLEND_REPLACE;
    d.curTex = prevTarget;
    d.curSamp = d.sampPoint;
    d.emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
               static_cast<float>(rx) / static_cast<float>(d.winW),
               static_cast<float>(ry) / static_cast<float>(d.winH),
               static_cast<float>(rx1) / static_cast<float>(d.winW),
               static_cast<float>(ry1) / static_cast<float>(d.winH), 0xFFFFFFFFu);
    d.flushAll();
    d.blend = BGPU_BLEND_PREMULTIPLIED;

    // 2) 降采样链
    bool chainOk = true;
    d.ignoreLayerClip = true;
    for (int i = 1; i <= levels && chainOk; i++) {
        const int dw = std::max(1, d.blurAt(static_cast<size_t>(i - 1)).w / 2);
        const int dh = std::max(1, d.blurAt(static_cast<size_t>(i - 1)).h / 2);
        if (!d.ensureBlurLevel(static_cast<size_t>(i), dw, dh)) { chainOk = false; break; }
        d.beginPass(d.blurAt(static_cast<size_t>(i)).tex, dw, dh, true, 0x00000000u);
        d.curTex = d.blurAt(static_cast<size_t>(i - 1)).tex;
        d.curSamp = d.sampLinear;
        d.emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                   0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
        d.flushAll();
    }
    d.ignoreLayerClip = false;

    if (chainOk) d.gaussBlurLevel(levels, blurPx);

    // 3) 合成：圆角条带 + 双线性放大，replace 混合（背景被替换成模糊版）
    if (chainOk) {
        d.beginPass(prevTarget, d.winW, d.winH, false, 0x00000000u);
        d.blend = BGPU_BLEND_REPLACE;
        d.curTex = d.blurAt(static_cast<size_t>(levels)).tex;
        d.curSamp = d.sampLinear;
        binui::PaintImageBand bands[binui::kMaxImageBands];
        const int nb = binui::BuildImageBands(x, y, w, h, radius, bands);
        for (int q = 0; q < nb; q++) {
            const binui::PaintImageBand& b = bands[q];
            if (b.w <= 0.0f || b.h <= 0.0f) continue;
            d.emitQuad(b.x, b.y, b.x + b.w, b.y + b.h, b.u0, b.v0, b.u1, b.v1, 0xFFFFFFFFu);
        }
        d.flushAll();
    }

    // 4) 颗粒：一层极淡的确定性噪声（屏幕锚定，一个纹素 = 一个屏幕像素）
    d.blend = BGPU_BLEND_PREMULTIPLIED;
    if (chainOk && grain > 0.001f) {
        const BgpuTexture nt = d.ensureNoiseTex();
        if (nt) {
            d.curTex = nt;
            d.curSamp = d.sampRepeat;
            const uint32_t av = static_cast<uint32_t>(grain * 3.0f + 0.5f);
            const uint32_t ncol = (av << 24) | 0x00FFFFFFu;
            binui::PaintImageBand nb2[binui::kMaxImageBands];
            const int nnb = binui::BuildImageBands(x, y, w, h, radius, nb2);
            for (int q = 0; q < nnb; q++) {
                const binui::PaintImageBand& b = nb2[q];
                if (b.w <= 0.0f || b.h <= 0.0f) continue;
                const float k = 1.0f / 64.0f;
                d.emitQuad(b.x, b.y, b.x + b.w, b.y + b.h, b.x * k, b.y * k,
                           (b.x + b.w) * k, (b.y + b.h) * k, ncol);
            }
            d.flushAll();
        }
    }
    d.curTex = d.whiteTex;
    d.curSamp = d.sampLinear;
    return chainOk;
}

bool BinUIGpu::pushLayer(float x, float y, float w, float h, float alpha) {
    return impl_->pushLayer(x, y, w, h, alpha);
}
bool BinUIGpu::pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) {
    if (fx.shadowCount > 0 && !fx.shadows) return false;
    return impl_->pushLayer(x, y, w, h, fx.alpha, fx.blurPx, fx.shadows, fx.shadowCount);
}
void BinUIGpu::popLayer() { impl_->popLayer(); }

// ============================================================
// 文本 / 图片
// ============================================================
float BinUIGpu::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->ready) return 0;
    return impl_->atlas.textWidth(size, weight, utf8, italic);
}
void BinUIGpu::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    impl_->atlas.fontMetrics(size, weight, ascent, lineHeight, italic);
}

void BinUIGpu::drawText(float x, float y, float size, int weight, const std::string& utf8,
                        uint32_t color, bool centerH, bool centerV, bool italic) {
    Impl& d = *impl_;
    if (!d.ready || utf8.empty()) return;
    d.curTex = d.whiteTex;
    d.curSamp = d.sampLinear;
    d.atlas.drawText(x, y, size, weight, utf8, color, centerH, centerV, italic,
                     [&d](const GlyphQuad& q) {
                         d.curTex = static_cast<BgpuTexture>(
                             reinterpret_cast<uintptr_t>(q.tex));
                         d.curSamp = d.sampLinear;
                         // ???? color ?**??**??PremultColor?????
                         d.emitQuad(q.x, q.y, q.x + q.w, q.y + q.h, q.u0, q.v0,
                                    q.u1, q.v1, UnPremult(q.color));
                     });
    d.curTex = d.whiteTex;
}

void BinUIGpu::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                               const std::string& utf8, uint32_t color, int align,
                               bool italic) {
    Impl& d = *impl_;
    if (!d.ready || utf8.empty()) return;
    d.curTex = d.whiteTex;
    d.curSamp = d.sampLinear;
    d.atlas.drawTextWrapped(x, y, maxW, size, weight, utf8, color, align, italic,
                            [&d](const GlyphQuad& q) {
                                d.curTex = static_cast<BgpuTexture>(
                                    reinterpret_cast<uintptr_t>(q.tex));
                                d.curSamp = d.sampLinear;
                                d.emitQuad(q.x, q.y, q.x + q.w, q.y + q.h, q.u0, q.v0,
                                           q.u1, q.v1, UnPremult(q.color));
                            });
    d.curTex = d.whiteTex;
}

bool BinUIGpu::prepareImage(uint16_t index, const uint8_t* data, size_t size,
                            uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImage(index, data, size, generation);
}
bool BinUIGpu::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                               uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImageRaw(index, w, h, rgba, generation);
}

void BinUIGpu::drawImageNine(uint16_t index, float x, float y, float w, float h, float slice,
                             int repX, int repY, uint32_t tint) {
    Impl& d = *impl_;
    if (!d.ready) return;
    auto it = d.imageDims.find(d.imgKey(index));
    float q[kMaxNineSliceQuads][8];
    const int n = it == d.imageDims.end()
                      ? 0
                      : BuildNineSlice(x, y, w, h, it->second.first, it->second.second, slice,
                                       repX, repY, &q[0][0], kMaxNineSliceQuads);
    if (n <= 0) {
        drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, tint);
        return;
    }
    for (int i = 0; i < n; i++)
        drawImageEx(index, q[i][0], q[i][1], q[i][2], q[i][3], q[i][4], q[i][5], q[i][6],
                    q[i][7], tint);
}

void BinUIGpu::drawImageEx(uint16_t index, float u0, float v0, float u1, float v1, float x,
                           float y, float w, float h, uint32_t tint) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    auto it = d.images.find(d.imgKey(index));
    if (it == d.images.end() || !it->second) return;
    d.curTex = it->second;
    d.curSamp = d.sampLinear;
    d.emitQuad(x, y, x + w, y + h, u0, v0, u1, v1, tint);
    d.curTex = d.whiteTex;
}

int BinUIGpu::pushClip(float x, float y, float w, float h) {
    Impl& d = *impl_;
    if (!d.ready) return d.scissorDepth;
    const int depth = d.scissorDepth++;
    const int l = std::max(d.scissor[0], static_cast<int>(x));
    const int t = std::max(d.scissor[1], static_cast<int>(y));
    const int r = std::min(d.scissor[2], static_cast<int>(x + w));
    const int b = std::min(d.scissor[3], static_cast<int>(y + h));
    d.scissor[0] = l;
    d.scissor[1] = t;
    d.scissor[2] = r;
    d.scissor[3] = b;
    return depth;
}

void BinUIGpu::popClipTo(int depth) {
    Impl& d = *impl_;
    d.scissorDepth = depth;
    if (d.scissorDepth == 0) {
        d.scissor[0] = 0;
        d.scissor[1] = 0;
        d.scissor[2] = d.winW;
        d.scissor[3] = d.winH;
    }
}

void BinUIGpu::setFontName(const char* name) { impl_->atlas.setFontName(name); }

// ============================================================
// 节点树遍历（与 drawNodesGL 同一份实现）
// ============================================================
void drawNodesGpu(BinUIGpu& gfx, const BinUIRenderer& renderer, const void* clipRect,
                  const GpuCanvasDrawFn& canvas) {
    CanvasFn shared;
    if (canvas) {
        shared = [&canvas](IBinUIGfx& g, uint16_t id, float x, float y, float w, float h) {
            canvas(static_cast<BinUIGpu&>(g), id, x, y, w, h);
        };
    }
    drawNodesBinUI(gfx, renderer, clipRect, shared);
}

} // namespace binui
