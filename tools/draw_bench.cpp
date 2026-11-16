// draw_bench.cpp - 绘制遍历的 CPU 基准（无 GPU、无窗口、确定性）
//
// 为什么需要它：
//   docs/future.md 的 P0-1/P0-4 只有几个一次性手测数字（1052 µs/帧、约 9800 个四边形），
//   没有可复现的入口 —— 优化完没法证明真的快了，重构也没法证明没画歪。
//   这个工具把"一次绘制"变成可测的东西：真布局 → drawNodesBinUI → 只计数不上屏。
//
// 两个用途：
//   1. 计时：`draw_bench <ui> --frames 200` → 遍历 µs/帧、原语分类、四边形/KB 估算；
//   2. 回归：`draw_bench <ui> --hash` → 整个原语流的 64 位哈希。缓存/重构**不许改变**
//      这个值（比像素更强：像素只能证明"看起来一样"，哈希证明"发的每个原语都一样"）。
//
// 用法:
//   draw_bench <ui文件>|--default [--frames N] [--warm N] [--clip l,t,w,h]
//              [--scrollTo y] [--hover x,y] [--text id=text] [--hash] [--json]
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include "binui_compiler.h"
#include "binui_component.h"
#include "binui_renderer.h"
#include "backends/binui_gfx.h"
#include "backends/binui_ui.h"

using namespace binui;

namespace {

// ---------------------------------------------------------------- 计数后端
// 只实现契约，不做任何光栅化。四边形数按各后端真实的展开规则估：
//   fillRect/fillLine/drawImageEx = 1（圆角图片的条带是遍历自己发的，一条一算）
//   fillRounded = 半径 > 0 ? 九宫格 9 : 1
//   strokeRounded = 9 × 线宽层数（后端按同心环叠厚度）
//   drawText/drawTextWrapped = 码点数（每字形一个四边形）
// 这样估算与 GL/D3D11/VK 的 emitQuad 次数误差在"没有字形的节点"上为 0。
struct CountGfx : IBinUIGfx {
    // 窗口尺寸
    int w_ = 0, h_ = 0;
    // 计时与计数
    double primitiveUs = 0;      // 原语分发自身耗时（不含遍历里的计算）
    uint64_t prims = 0, quads = 0, bytes = 0;
    uint64_t byKind[16] = {};
    uint64_t hash = 1469598103934665603ull;    // FNV-1a 64
    bool hashOn = false;
    uint32_t textShadowDepth = 0;              // 阴影开着时每个字形多一个四边形

    void Mix(uint64_t v) {
        if (!hashOn) return;
        hash ^= v;
        hash *= 1099511628211ull;
    }
    void MixF(float f) {
        uint32_t b = 0;
        std::memcpy(&b, &f, sizeof(b));
        Mix(b);
    }
    void Count(int kind, uint64_t q) {
        prims++;
        quads += q;
        byKind[kind]++;
        bytes += q * 6 * 20;   // GLVertex = 5 个 4 字节字段 → 20 B/顶点，6 顶点/四边形
    }

    // ---- 生命周期 ----
    bool isReady() const override { return true; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void resize(int width, int height) override { w_ = width; h_ = height; }
    void setVsync(bool) override {}
    void setTransparent(bool) override {}
    void beginFrame(const void*) override {}
    void present() override {}

    // ---- 原语 ----
    void fillRect(float x, float y, float w, float h, uint32_t c) override {
        Count(0, 1);
        Mix(1); MixF(x); MixF(y); MixF(w); MixF(h); Mix(c);
    }
    void fillRounded(float x, float y, float w, float h, float r, uint32_t c) override {
        Count(1, r > 0 ? 9 : 1);
        Mix(2); MixF(x); MixF(y); MixF(w); MixF(h); MixF(r); Mix(c);
    }
    void fillRoundedXf(float w, float h, float r, bool stroke, float th, uint32_t c,
                       const float xf[6]) override {
        const uint64_t q = stroke ? static_cast<uint64_t>(9 * std::max(1.0f, std::round(th)))
                                  : (r > 0 ? 9u : 1u);
        Count(2, q);
        Mix(3); MixF(w); MixF(h); MixF(r); Mix(stroke ? 1u : 0u); MixF(th); Mix(c);
        for (int i = 0; i < 6; i++) MixF(xf[i]);
    }
    void fillLine(float x0, float y0, float x1, float y1, float width, uint32_t c) override {
        Count(3, 1);
        Mix(4); MixF(x0); MixF(y0); MixF(x1); MixF(y1); MixF(width); Mix(c);
    }
    void strokeRounded(float x, float y, float w, float h, float r, float th,
                       uint32_t c) override {
        const uint64_t q = r > 0 ? static_cast<uint64_t>(9 * std::max(1.0f, std::round(th))) : 1u;
        Count(4, q);
        Mix(5); MixF(x); MixF(y); MixF(w); MixF(h); MixF(r); MixF(th); Mix(c);
    }
    void drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                     float x, float y, float w, float h, uint32_t tint) override {
        Count(5, 1);
        Mix(6); Mix(index); MixF(u0); MixF(v0); MixF(u1); MixF(v1);
        MixF(x); MixF(y); MixF(w); MixF(h); Mix(tint);
    }
    void drawImageNine(uint16_t index, float x, float y, float w, float h, float slice,
                       int repX, int repY, uint32_t tint) override {
        Count(6, 9);
        Mix(7); Mix(index); MixF(x); MixF(y); MixF(w); MixF(h); MixF(slice); Mix(tint);
        Mix((uint32_t)repX); Mix((uint32_t)repY);
    }
    void drawText(float x, float y, float size, int weight, const std::string& s,
                  uint32_t c, bool ch, bool cv, bool it) override {
        uint64_t n = 0;
        for (size_t i = 0; i < s.size(); i++)
            if ((static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u) n++;   // 码点计数
        Count(7, std::max<uint64_t>(1, n));
        Mix(8); MixF(x); MixF(y); MixF(size); Mix(static_cast<uint64_t>(weight));
        Mix(c); Mix(ch ? 1u : 0u); Mix(cv ? 1u : 0u); Mix(it ? 1u : 0u);
        for (char ch2 : s) Mix(static_cast<unsigned char>(ch2));
    }
    void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                         const std::string& s, uint32_t c, int align, bool it) override {
        drawText(x, y, size, weight, s, c, false, false, it);
        MixF(maxW); Mix(static_cast<uint64_t>(align));
    }
    float textWidth(float size, int weight, const std::string& s, bool it) override {
        (void)weight; (void)it;
        // 近似度量（只影响自动换行/居中的落位，不影响计数）：中文一个码点 1em、
        // 其余 0.55em。真实后端用 FreeType/DirectWrite，这里要的是确定性而不是像素。
        float wd = 0;
        for (size_t i = 0; i < s.size();) {
            const unsigned char b = static_cast<unsigned char>(s[i]);
            const int len = (b < 0x80) ? 1 : ((b >> 5) == 0x6) ? 2 : ((b >> 4) == 0xE) ? 3 : 4;
            wd += (len >= 3) ? size : size * 0.55f;
            i += static_cast<size_t>(len);
        }
        return wd;
    }
    void fontMetrics(float size, int weight, int& asc, int& lh, bool it) override {
        (void)weight; (void)it;
        asc = static_cast<int>(size * 0.8f);
        lh = static_cast<int>(size * 1.25f);
    }
    void setTextSubpixel(bool) override {}
    bool setAffine(const float* xf) override {
        Mix(9);
        for (int i = 0; i < (xf ? gibberishSize() : 0); i++) MixF(xf[i]);
        return true;   // 三个 GPU 后端都支持
    }
    static int gibberishSize() { return 6; }
    void setImageFilter(const BinUIImageFilter*) override {}
    bool setEraseBlend(bool) override { return true; }
    bool setKeepBlend(bool) override { return true; }
    bool supportsMask() const override { return true; }
    bool backdropBlur(float x, float y, float w, float h, float r, float blur,
                      float grain) override {
        Count(8, 1);
        Mix(10); MixF(x); MixF(y); MixF(w); MixF(h); MixF(r); MixF(blur); MixF(grain);
        return true;
    }
    // v0.16.50：多层文字阴影。计数按"层数"记 —— 每层对每个字形多一个四边形，
    // 所以层数是这里唯一有意义的量；参数也混进哈希，免得改层参数时计数不变。
    void setTextShadows(const BinUITextShadow* layers, int n) override {
        Mix(11);
        Mix(static_cast<uint32_t>(n));
        if (layers) {
            for (int i = 0; i < n; i++) {
                MixF(layers[i].offsetX); MixF(layers[i].offsetY);
                MixF(layers[i].blur); Mix(layers[i].color);
            }
        }
    }
    bool pushLayer(float x, float y, float w, float h, float a) override {
        Mix(12); MixF(x); MixF(y); MixF(w); MixF(h); MixF(a);
        return true;
    }
    void popLayer() override { Mix(13); }
    // v30：任意角度渐变 = 四角带色四边形。计数后端必须"支持"它，否则量到的永远是
    // 条带兜底路径（那就等于没改）。
    bool fillGradientQuad(float x, float y, float w, float h, float radius,
                          float angleDeg, uint32_t from, uint32_t to) override {
        Count(9, radius > 0.5f ? 9 : 1);
        Mix(19); MixF(x); MixF(y); MixF(w); MixF(h); MixF(radius); MixF(angleDeg);
        Mix(from); Mix(to);
        return true;
    }
    bool prepareImage(uint16_t index, const uint8_t*, size_t, uint32_t gen) override {
        Mix(14); Mix(index); Mix(gen);
        return true;
    }
    bool prepareImageRaw(uint16_t index, int, int, const uint8_t*, uint32_t gen) override {
        Mix(15); Mix(index); Mix(gen);
        return true;
    }
    int pushClip(float x, float y, float w, float h) override {
        Mix(16); MixF(x); MixF(y); MixF(w); MixF(h);
        return 1;
    }
    void popClipTo(int) override { Mix(17); }
    void setFontName(const char* name) override {
        Mix(18);
        if (name) for (const char* p = name; *p; p++) Mix(static_cast<unsigned char>(*p));
    }
};

const char* kKindNames[10] = {
    "fillRect", "fillRounded", "fillRoundedXf", "fillLine", "strokeRounded",
    "drawImageEx", "drawImageNine", "drawText", "backdropBlur", "fillGradientQuad"
};

double NowUs() {
    using namespace std::chrono;
    return duration<double, std::micro>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: draw_bench <ui file>|--default [--frames N] [--warm N] [--clip l,t,w,h]\n"
                    "                  [--scrollTo y] [--hover x,y] [--text id=text] [--hash] [--json]\n");
        return 1;
    }
    const bool useDefault = (std::string(argv[1]) == "--default");
    int frames = 100, warm = 10;
    int clipL = 0, clipT = 0, clipW = 0, clipH = 0;
    float scrollTo = -1.0f;
    float hoverX = -1.0f, hoverY = 0.0f;
    std::vector<std::pair<uint16_t, std::string>> texts;
    bool hashMode = false, json = false;

    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](float& dst) { if (i + 1 < argc) dst = static_cast<float>(atof(argv[++i])); };
        if (a == "--frames" && i + 1 < argc) frames = std::max(1, atoi(argv[++i]));
        else if (a == "--warm" && i + 1 < argc) warm = std::max(0, atoi(argv[++i]));
        else if (a == "--clip" && i + 1 < argc) {
            std::sscanf(argv[++i], "%d,%d,%d,%d", &clipL, &clipT, &clipW, &clipH);
        } else if (a == "--scrollTo") next(scrollTo);
        else if (a == "--hover" && i + 1 < argc) {
            std::sscanf(argv[++i], "%f,%f", &hoverX, &hoverY);
        } else if (a == "--text" && i + 1 < argc) {
            std::string spec = argv[++i];
            const size_t eq = spec.find('=');
            if (eq != std::string::npos)
                texts.emplace_back(static_cast<uint16_t>(atoi(spec.substr(0, eq).c_str())),
                                   spec.substr(eq + 1));
        } else if (a == "--hash") hashMode = true;
        else if (a == "--json") json = true;
        else if (a.rfind("--", 0) == 0) {
            std::printf("unknown option: %s\n", a.c_str());
            return 1;
        }
    }

    std::vector<uint8_t> layout;
    if (useDefault) layout = binui::GenerateDefaultBinUI();
    else layout = binui::CompileBinUIFileEx(argv[1]).layout;
    if (layout.empty()) { std::printf("[bench] 编译失败: %s\n", argv[1]); return 1; }

    BinUIRenderer r;
    if (!r.loadLayout(layout.data(), layout.size())) { std::printf("[bench] loadLayout 失败\n"); return 1; }

    const std::string uiDir = useDefault ? std::string() : binui::ex::DirOf(argv[1]);
    if (!r.componentRefs().empty()) binui::ex::MountAll(r, uiDir, r.layoutBytes());

    const auto& nodes = r.getNodes();
    const int W = static_cast<int>(nodes[0].width);
    const int H = static_cast<int>(nodes[0].height);
    if (clipW <= 0 || clipH <= 0) { clipL = 0; clipT = 0; clipW = W; clipH = H; }
    clipL = std::max(0, std::min(clipL, W - 1));
    clipT = std::max(0, std::min(clipT, H - 1));
    clipW = std::min(clipW, W - clipL);
    clipH = std::min(clipH, H - clipT);

    // 图片资源：几何不需要像素，但 image 节点会走 prepareImage（计数后端的 hash 里含 index）
    const std::vector<std::string> imgPaths = r.imagePaths();
    std::unordered_map<std::string, std::vector<uint8_t>> resources;
    r.setImageOrder(imgPaths);
    r.loadResources(resources);

    if (scrollTo >= 0) {
        // 第一个可滚动节点的 id（与 offscreen_render 的 --scrollTo 一致）
        for (const auto& nd : nodes)
            if (nd.flags & BINUI_SCROLLABLE) { r.scrollTo(nd.id, 0, scrollTo); break; }
    }
    for (const auto& kv : texts) r.setText(kv.first, kv.second);
    if (hoverX >= 0) { r.setHover(hoverX, hoverY); r.stepAnimations(); }

    CountGfx g;
    g.w_ = W;
    g.h_ = H;
    g.hashOn = hashMode;

    const int rc[4] = { clipL, clipT, clipL + clipW, clipT + clipH };
    const bool fullClip = (clipW == W && clipH == H);
    const void* clipPtr = fullClip ? nullptr : &rc;

    // 预热：首帧会把条带缓存/图集索引建起来，不计入平均值
    for (int i = 0; i < warm; i++) binui::drawUi(g, r, clipPtr);
    g = CountGfx{};
    g.w_ = W; g.h_ = H; g.hashOn = hashMode;

    std::vector<double> us;
    us.reserve(static_cast<size_t>(frames));
    for (int f = 0; f < frames; f++) {
        const double t0 = NowUs();
        binui::drawUi(g, r, clipPtr);
        us.push_back(NowUs() - t0);
    }
    std::sort(us.begin(), us.end());
    const double sum = [&] {
        double s = 0;
        for (double v : us) s += v;
        return s;
    }();
    const double avg = sum / static_cast<double>(us.size());
    const double p50 = us[us.size() / 2];
    const double p95 = us[std::min(us.size() - 1, (us.size() * 95) / 100)];

    const double perFrame = static_cast<double>(frames);
    const double quadsPerFrame = static_cast<double>(g.quads) / perFrame;
    const double kbPerFrame = static_cast<double>(g.bytes) / perFrame / 1024.0;
    const double primsPerFrame = static_cast<double>(g.prims) / perFrame;

    if (hashMode) {
        // 只打哈希：门禁脚本 diff 这一行
        std::printf("%016llx\n", static_cast<unsigned long long>(g.hash));
        return 0;
    }

    if (json) {
        std::printf("{\"ui\":\"%s\",\"nodes\":%zu,\"w\":%d,\"h\":%d,\"clip\":[%d,%d,%d,%d],"
                    "\"frames\":%d,\"us_avg\":%.1f,\"us_p50\":%.1f,\"us_p95\":%.1f,"
                    "\"prims\":%.1f,\"quads\":%.1f,\"kb\":%.2f}\n",
                    useDefault ? "--default" : argv[1], nodes.size(), W, H,
                    clipL, clipT, clipW, clipH, frames, avg, p50, p95,
                    primsPerFrame, quadsPerFrame, kbPerFrame);
    } else {
        std::printf("[bench] %s  节点 %zu  窗口 %dx%d  clip=%d,%d %dx%d%s\n",
                    useDefault ? "--default" : argv[1], nodes.size(), W, H,
                    clipL, clipT, clipW, clipH, fullClip ? "（全窗）" : "");
        std::printf("[bench] %d 帧：遍历 平均 %.1f / p50 %.1f / p95 %.1f µs"
                    "  |  原语 %.0f、≈四边形 %.0f、≈%.1f KB / 帧\n",
                    frames, avg, p50, p95, primsPerFrame, quadsPerFrame, kbPerFrame);
        std::printf("[bench] 分类：");
        for (int k = 0; k < 10; k++) {
            if (!g.byKind[k]) continue;
            std::printf("%s %.0f  ", kKindNames[k], static_cast<double>(g.byKind[k]) / perFrame);
        }
        std::printf("\n");
    }
    std::fflush(stdout);
    return 0;
}
