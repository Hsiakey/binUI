// binui_glyph_atlas.h - 后端无关的字形图集与文本排版（GL / Vulkan 共用）
//
// 为什么要有这一层：文本是 UI 里几何最碎、最容易两端跑偏的东西。谁自己写一份
// "格子边长 / advance / 基线 / 图集代" 的算法，两端的换行位置和字间距就会慢慢
// 分叉，像素回归也就只能靠"容差"遮着。所以这里把**排版 + 光栅化 + 缓存**整份
// 抽出来，后端只负责两件事：
//
//   1) IGlyphTexHost —— 把位图变成纹理（GL: glTexSubImage2D；VK: staging + vkCmdCopyBufferToImage）
//   2) 把回调给出的四边形画出来（顶点、批次、裁剪都由后端管）
//
// 这样 GL 与 Vulkan 的文字是**同一份像素**：像素回归里两端必须 0 差异。
//
// 图集格式统一 BGRA8（白字预乘 alpha + 彩色字形原样），与 GL 历来的做法一致：
// 一个着色器、一种取样公式就能覆盖灰度字、彩色 emoji 和图片。
// 单通道 R8 图集能省 3/4 显存，但要多一条着色器分支，属于 P4 的内存优化，不在 P3 做。
//
// 这份头文件只有 C++ 标准库 + binui_font_renderer 依赖，两个后端都能直接 include。
#ifndef BINUI_GLYPH_ATLAS_H
#define BINUI_GLYPH_ATLAS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "binui_font_renderer.h"

namespace binui {

// ============================================================
// UTF-8 / emoji 簇小工具
// ============================================================

// UTF-8 → 码点迭代（容错：非法序列按首字节长度跳过，不抛不崩）
struct Utf8Iter {
    const std::string& s;
    size_t i = 0;
    uint32_t cp = 0;
    explicit Utf8Iter(const std::string& str) : s(str) {}
    bool next() {
        if (i >= s.size()) return false;
        const uint8_t b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80) { cp = b0; i++; return true; }
        const int n = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t c = b0 & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); k++)
            c = (c << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        cp = c;
        i += n;
        return true;
    }
};

inline std::vector<uint32_t> Utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    Utf8Iter it(s);
    while (it.next()) cps.push_back(it.cp);
    return cps;
}

inline std::string CodepointsToUtf8(const uint32_t* cps, size_t n) {
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        const uint32_t c = cps[i];
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (c >> 18));
            s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return s;
}

inline bool IsEmojiCp(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;
    if (cp >= 0x2190 && cp <= 0x21FF) return true;
    if (cp >= 0x2300 && cp <= 0x27BF) return true;
    if (cp >= 0x2900 && cp <= 0x297F) return true;
    if (cp >= 0x2B00 && cp <= 0x2BFF) return true;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
    if (cp == 0x200D) return true;
    if (cp == 0x20E3) return true;
    return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
           cp == 0x2122 || cp == 0x2139 || cp == 0x3030 || cp == 0x303D ||
           cp == 0x3297 || cp == 0x3299;
}

// emoji 簇的长度：ZWJ 连写、肤色修饰、变体选择符、键帽、区域旗帜
inline size_t EmojiClusterLen(const uint32_t* cps, size_t n) {
    const uint32_t first = cps[0];
    if ((first == '#' || first == '*' || (first >= '0' && first <= '9')) && n > 1) {
        if (cps[1] == 0xFE0F && n > 2 && cps[2] == 0x20E3) return 3;
        if (cps[1] == 0x20E3) return 2;
    }
    if (!IsEmojiCp(first)) return 1;
    size_t j = 1;
    int ris = (first >= 0x1F1E6 && first <= 0x1F1FF) ? 1 : 0;
    while (j < n) {
        const uint32_t c = cps[j];
        if (c == 0x200D) {
            if (j + 1 >= n) break;
            j += 2;
            continue;
        }
        if (c == 0xFE0F || c == 0xFE0E || c == 0x20E3) { j++; continue; }
        if (c >= 0x1F3FB && c <= 0x1F3FF) { j++; continue; }
        if (c >= 0x1F1E6 && c <= 0x1F1FF) {
            if (ris == 1) { j++; ris = 2; continue; }
            break;
        }
        break;
    }
    return j;
}

inline uint64_t EmojiClusterKey(const uint32_t* cps, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        uint64_t c = cps[i];
        for (int b = 0; b < 4; b++) {
            h ^= (c & 0xFF);
            h *= 1099511628211ull;
            c >>= 8;
        }
    }
    return h;
}

// 颜色预乘（图集/模板本身已是预乘，着色器输出预乘 alpha）
inline void PremultColor(uint32_t& c) {
    const uint32_t a = (c >> 24) & 0xFF;
    if (a == 255) return;
    if (a == 0) { c = 0; return; }
    const uint32_t r = (((c >> 16) & 0xFF) * a) / 255;
    const uint32_t g = (((c >> 8) & 0xFF) * a) / 255;
    const uint32_t b = ((c & 0xFF) * a) / 255;
    c = (a << 24) | (r << 16) | (g << 8) | b;
}

// ============================================================
// 纹理宿主：后端把位图变成可采样的纹理
// ============================================================
class IGlyphTexHost {
public:
    virtual ~IGlyphTexHost() = default;
    // 建一张 w×h 的 BGRA8 纹理，内容必须全 0（透明）——
    // 图集里"空格子"会被当真字形画（advance 有效、像素全透明），
    // 未初始化的显存会把这几个空格画成垃圾（GL 靠 glTexImage2D 传零缓冲，
    // VK 图像不会被驱动清零，必须自己 clear）。
    // 返回 0 = 失败。
    virtual void* createTex(int w, int h) = 0;
    // 上传子矩形：bgra 为 w*h*4 字节，预乘 alpha（BGRA 字节序，与 0xAARRGGBB 的内存序一致）
    virtual void updateTex(void* tex, int x, int y, int w, int h, const uint8_t* bgra) = 0;
    virtual void destroyTex(void* tex) = 0;
};

// 一次字形绘制请求：后端把它变成一个四边形
struct GlyphQuad {
    void* tex = nullptr;        // IGlyphTexHost 的句柄
    float x = 0, y = 0, w = 0, h = 0;   // 目标矩形（左上角 + 尺寸，像素）
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // 归一化 UV
    uint32_t color = 0xFFFFFFFF;        // 已预乘 alpha 的顶点色
};

// ============================================================
// 整形结果缓存（v20）：同一串文本每帧都要打，而"码点 → 面 → 图集格子"这条链
// 每次都要 UTF-8 解码 + 逐簇 faceIdxForCp + 两级哈希查表。实测 1.35 µs/标签
// （features 一帧 43 个标签 = 0.058 ms，占遍历 2/3），静态页面上这些结果其实**永远不变**。
// 所以按 (文本, 字号, 字重, 斜体, 字体面) 缓存一份"相对原点 + 相对基线"的四边形表，
// 命中时只做 emit。图集代数（纹理新建/销毁、换字体）一变整表作废。
// ============================================================
struct GlyphRunQuad {
    void* tex = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool white = false;   // 彩色字形用白顶点（不染色）
    // v24 文字阴影：同一字形的阴影格。
    //   blur = 0 → 直接复用字形格（sTex == tex、同 UV、sPad = 0），"阴影 = 偏移一份字形"；
    //   blur > 0 → 模糊图集里的一格，每边比字形格多 sPad 像素，画的时候按 sPad 外扩。
    // 彩色字形（emoji）不挂阴影（见 FillShadowQuad 注释），sTex 保持 nullptr。
    void* sTex = nullptr;
    float su0 = 0, sv0 = 0, su1 = 0, sv1 = 0;
    float sPad = 0;
};
struct GlyphRun {
    uint64_t gen = 0;                 // 建立时的图集代数
    float advance = 0;                // 总前进宽（textWidth 直接复用）
    float lineH = 0;                  // 行高（centerV 用）
    float ascent = 0;                 // 基线偏移
    std::vector<GlyphRunQuad> quads;  // 相对 (0, baseline) 的位置
    uint64_t lastUse = 0;             // LRU 时钟
};

// ============================================================
// GlyphAtlas：排版 + 光栅化 + 图集代管理
// ============================================================
class GlyphAtlas {
public:
    using EmitFn = std::function<void(const GlyphQuad&)>;

    GlyphAtlas() = default;
    ~GlyphAtlas() { clear(); }
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    void setHost(IGlyphTexHost* host) { host_ = host; }
    bool hasHost() const { return host_ != nullptr; }

    // 文字是否按**整像素**落位（默认 true：字形位图 1:1，整像素最锐利）。
    // 有动画的节点要设成 false —— 位置保留小数，文字才能跟着亚像素位移连续滑；
    // 否则每 2 帧跳 1px，肉眼就是"字在抖"（见 CHANGELOG 0.16.4）。
    //
    // v24 起 false 的含义变了：不再靠图集 LINEAR 采样"糊一层"换平滑，而是把小数部分
    // 量化成 **¼ 像素相位**，每份相位是一张独立的字形位图（见 PhaseOf）。整数位置下
    // 相位恒为 0，与旧行为逐像素相同，静态页面不受影响。
    bool snapText = true;

    // ---- v24：文字阴影（后端在画一个节点的文字之前设好，节点画完清掉）----
    // 语义与 CSS text-shadow 一致：先把整段文字的**模糊覆盖率**按 (dx,dy) 偏移铺一遍，
    // 再把清晰文字盖上去。模糊在 CPU 上做（字形位图 → 高斯），按 (字形, 模糊半径)
    // 缓存在独立的图集里，所以同一段文字每帧只是多发射一批四边形。
    //
    // blur < 0.5px 视为 0：这时候"阴影"就是偏移一份字形位图，不需要模糊图集。
    void setTextShadow(float dx, float dy, float blur, uint32_t color) {
        shadow_.on = ((color >> 24) & 0xFFu) != 0;
        shadow_.dx = dx;
        shadow_.dy = dy;
        shadow_.blur = blur > 0.0f ? blur : 0.0f;
        shadow_.color = color;
        int q = static_cast<int>(std::lround(shadow_.blur * 4.0f));
        if (q < 2) q = 0;                 // < 0.5px 的模糊等于没模糊（硬阴影）
        if (q > kMaxShadowBlurQ) q = kMaxShadowBlurQ;
        shadow_.blurQ = q;
    }
    void clearTextShadow() { shadow_.on = false; }
    bool textShadowActive() const { return shadow_.on; }

    // 纹理/字体变动即让整形缓存失效（见 GlyphRun 注释）
    void bumpAtlasGen() {
        ++atlasGen_;
        runs_.clear();
    }

    static constexpr size_t kMaxRuns = 1024;   // 整形缓存条目上限（LRU）
    // 文字阴影的模糊半径上限（¼ 像素单位：64 = 16px）。再大就只是"一团灰"，
    // 而图集格子会膨胀到装不下几个字形 —— 超过的按上限处理。
    static constexpr int kMaxShadowBlurQ = 64;

    // 缓存 key：文本 + 字号 + 字重 + 斜体 + 字体面 + 亚像素相位 + letter-spacing（FNV-1a）
    uint64_t RunKey(const std::string& s, float size, int weight, bool italic,
                    int phase = 0) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; i++) {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 1099511628211ull;
            }
        };
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        uint32_t su = 0;
        std::memcpy(&su, &size, sizeof(su));
        mix(su);
        mix(static_cast<uint64_t>(weight));
        mix(italic ? 1u : 0u);
        mix(static_cast<uint64_t>(phase & 3));
        // v0.16.40：letter-spacing 参与 key（¼ 像素量化）—— 间距不同就是另一份 run
        mix(static_cast<uint64_t>(static_cast<uint32_t>(
            static_cast<int32_t>(std::lround(letterSpacing_ * 4.0f)))));
        for (unsigned char c : asciiFont_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    // 重放整形结果：颜色按调用方给的（灰字形染色，彩色字形白顶点）。
    // 有文字阴影时**先整条阴影、再整条字形** —— 逐字交错会让后面那个字的阴影
    // 压到前面那个字上（半透明文字会看出脏边）。
    void EmitRun(const GlyphRun& run, float x, float baseline, uint32_t color,
                 const EmitFn& emit) {
        const bool sh = textShadowActive();
        uint32_t shColor = 0;
        if (sh) { shColor = shadow_.color; PremultColor(shColor); }
        if (sh) {
            for (const GlyphRunQuad& q : run.quads) {
                if (!q.sTex) continue;
                GlyphQuad g;
                g.tex = q.sTex;
                g.x = x + q.x - q.sPad + shadow_.dx;
                g.y = baseline + q.y - q.sPad + shadow_.dy;
                g.w = q.w + 2.0f * q.sPad;
                g.h = q.h + 2.0f * q.sPad;
                g.u0 = q.su0; g.v0 = q.sv0; g.u1 = q.su1; g.v1 = q.sv1;
                g.color = shColor;
                emit(g);
            }
        }
        for (const GlyphRunQuad& q : run.quads) {
            GlyphQuad g;
            g.tex = q.tex;
            g.x = x + q.x;
            g.y = baseline + q.y;
            g.w = q.w; g.h = q.h;
            g.u0 = q.u0; g.v0 = q.v0; g.u1 = q.u1; g.v1 = q.v1;
            g.color = q.white ? 0xFFFFFFFFu : color;
            emit(g);
        }
    }

    // 初始化 FreeType/HarfBuzz + 注册系统字体；false = 字体服务不可用（文本降级为空白）
    bool initFonts() {
        fontReady_ = font_.init();
        binui_font::registerSystemFonts(font_);
        return fontReady_;
    }
    bool fontReady() const { return fontReady_; }

    void setFontName(const char* name) {
        if (name && *name && asciiFont_ != name) {
            asciiFont_ = name;
            bumpAtlasGen();   // 换字体面 → 整形缓存里的 tex/UV 全部作废
        }
    }
    const std::string& fontName() const { return asciiFont_; }

    // 文本宽度（像素，逐字形 advance 之和；与 drawText 用的是同一套度量）。
    // 命中整形缓存时直接返回缓存里的总宽，不走解码 + 逐簇度量。
    // v0.16.40：包含 letter-spacing（每个"字形簇"后加一次，含末尾那个 —— 与浏览器一致，
    // 所以 textWidth 与 drawText 的实际前进量永远相等）。
    float textWidth(float size, int weight, const std::string& utf8, bool italic) {
        if (!host_ || utf8.empty()) return 0;
        const uint64_t key = RunKey(utf8, size, weight, italic);
        auto it = runs_.find(key);
        if (it != runs_.end() && it->second.gen == atlasGen_) {
            it->second.lastUse = ++runClock_;
            return it->second.advance;
        }
        const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);
        float w = 0;
        size_t i = 0;
        while (i < cps.size()) {
            const ClusterMetric m = clusterMetric(size, weight, cps.data() + i,
                                                 cps.size() - i, italic);
            w += m.adv + letterSpacing_;
            i += m.len;
        }
        return w;
    }

    // letter-spacing（v0.16.40）：每个字形簇后额外的前进量（像素，可为负）。
    // 只影响排版，不影响字形位图 —— 所以整形缓存的 key 里带上它（同一个字符串在
    // 0 / 1px 间距下是两份 run）。
    void setLetterSpacing(float px) { letterSpacing_ = px; }
    float letterSpacing() const { return letterSpacing_; }

    // 行高与基线（主面度量）
    void fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
        Atlas* a = getAtlas(size, weight, 0, italic);
        if (!a) { ascent = lineHeight = static_cast<int>(size); return; }
        ascent = a->ascent;
        lineHeight = a->ascent + a->descent;
    }

    // 画一行文本。x/y 语义与 IBinUIGfx::drawText 一致：
    //   y 是基线（centerV 时为矩形中心），centerH 时 x 为矩形中心。
    //
    // v24：snapText=false 时不再靠 LINEAR 采样换平滑，而是把 x 的小数部分量化成
    // ¼ 像素相位，取对应相位的字形位图（位图里已经烘进了那 ¼ 像素的位移）。
    // 于是"文字在连续滑动"与"字形逐像素锐利"同时成立。
    // 纵向仍按整像素吸附：4×4 变体会让图集体积爆炸，而纵向模糊的观感代价远小于横向。
    void drawText(float x, float y, float size, int weight, const std::string& utf8,
                  uint32_t color, bool centerH, bool centerV, bool italic, const EmitFn& emit) {
        if (!host_ || utf8.empty()) return;
        uint32_t premult = color;
        PremultColor(premult);

        if (centerH) x -= textWidth(size, weight, utf8, italic) / 2;
        int ascent = 0, lineH = 0;
        fontMetrics(size, weight, ascent, lineH, italic);
        if (centerV) y -= lineH / 2.0f;

        // 落位：x 量化到"整数 + ¼ 相位"，基线取整
        float qx = x;
        int phase = 0;
        if (snapText) qx = std::floor(x + 0.5f);
        else if (PhaseEnabled()) QuantizeSubpixelX(x, qx, phase);
        const float baseline = std::floor(y + 0.5f);

        // ---- 整形缓存：命中时只剩"按四边形表发射"这一件事 ----
        const uint64_t key = RunKey(utf8, size, weight, italic, phase);
        auto it = runs_.find(key);
        if (it != runs_.end() && it->second.gen == atlasGen_) {
            it->second.lastUse = ++runClock_;
            EmitRun(it->second, qx, baseline, premult, emit);
            return;
        }

        const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);
        color = premult;

        // 边画边记一份 run（相对 x=0、基线 = 0），下次直接重放
        GlyphRun built;
        built.gen = atlasGen_;
        built.lineH = static_cast<float>(lineH);
        built.ascent = static_cast<float>(ascent);
        const float originX = qx;
        const float originBaseline = baseline;
        auto record = [&](void* tex, float qx0, float qy0, float qw, float qh,
                          float gx, float gy, float gw, float gh, float t,
                          bool white) -> GlyphRunQuad& {
            GlyphRunQuad q;
            q.tex = tex;
            q.x = qx0 - originX;
            q.y = qy0 - originBaseline;
            q.w = qw; q.h = qh;
            q.u0 = gx / t; q.v0 = gy / t;
            q.u1 = (gx + gw) / t; q.v1 = (gy + gh) / t;
            q.white = white;
            built.quads.push_back(q);
            return built.quads.back();
        };

        float cx = qx;
        auto drawGray = [&](uint32_t cp) {
            Atlas* a = getAtlasForCp(size, weight, faceIdxForCp(cp), italic, cp, phase);
            if (!a) return;
            const Glyph* g = glyphFor(a, cp, phase);
            if (!g) return;
            const float t = static_cast<float>(a->sizePx);
            const float cell = static_cast<float>(a->cellW);
            const float gx = (g->cell % a->cols) * cell;
            const float gy = (g->cell / a->cols) * cell;
            const float top = baseline - (a->ascent + 1);
            GlyphRunQuad& q = record(a->tex, cx, top, cell, cell, gx, gy, cell, cell, t, false);
            FillShadowQuad(q, a, cp, gx, gy, cell, t);
            cx += g->advW;
        };

        size_t i = 0;
        while (i < cps.size()) {
            const size_t n = EmojiClusterLen(cps.data() + i, cps.size() - i);
            // 彩色只在该码点确实由 emoji 面覆盖时才尝试（真 emoji 走 COLR；符号走灰度）
            const bool colorCp = fontReady_ &&
                                 (n > 1 || faceIdxForCp(cps[i]) == emojiFaceIdx());
            if (colorCp) {
                ColorAtlas* ca = getColorAtlas(size);
                const ColorGlyph* g = ca ? colorGlyphFor(ca, cps.data() + i, n) : nullptr;
                if (g) {
                    const float t = static_cast<float>(ca->sizePx);
                    const float ccw = static_cast<float>(ca->cellW);
                    const float gx = (g->cell % ca->cols) * ccw;
                    const float gy = (g->cell / ca->cols) * ca->cellH;
                    const float top = baseline - g->baseline;
                    // 彩色字形不挂文字阴影：阴影要用"覆盖率"染色，而彩色格子里
                    // 每个像素自带颜色（乘上阴影色会糊成一团彩色，不是剪影）。
                    record(ca->tex, cx, top, static_cast<float>(g->w),
                           static_cast<float>(g->h), gx, gy, static_cast<float>(g->w),
                           static_cast<float>(g->h), t, true);
                    cx += g->advW + letterSpacing_;
                    i += n;
                    continue;
                }
            }
            for (size_t k = 0; k < n; k++) drawGray(cps[i + k]);
            cx += letterSpacing_;      // letter-spacing 按"字形簇"加一次（含末尾那个）
            i += n;
        }

        built.advance = cx - originX;
        built.lastUse = ++runClock_;
        // 缓存上限（主题/字号一多会一路涨）：超了按 LRU 淘汰
        if (runs_.size() >= kMaxRuns && !built.quads.empty()) {
            auto victim = runs_.begin();
            for (auto k = runs_.begin(); k != runs_.end(); ++k)
                if (k->second.lastUse < victim->second.lastUse) victim = k;
            runs_.erase(victim);
        }
        if (!built.quads.empty()) {
            EmitRun(built, qx, baseline, premult, emit);
            runs_.emplace(key, std::move(built));
        }
    }

    // 中文避头点：这些标点不能出现在行首（CSS line-break: normal 的常见规则）。
    // 折行时多带一个字符（允许极轻微超出 maxW）比行首孤零零一个"。"自然得多。
    static bool NoLineStart(uint32_t cp) {
        switch (cp) {
            case 0x3001: case 0x3002:                     // 、。
            case 0xFF0C: case 0xFF1A: case 0xFF1B:        // ，：；
            case 0xFF01: case 0xFF1F:                     // ！？
            case 0x300B: case 0x300D: case 0x300F:        // 》」』
            case 0x3011: case 0xFF09:                     // 】）
            case 0x2026: case 0xFF5E:                     // …～
                return true;
            default:
                return false;
        }
    }

    // 自动换行绘制。对齐：0=左 1=居中 2=右（相对 [x, x+maxW]）
    void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                         const std::string& utf8, uint32_t color, int align, bool italic,
                         const EmitFn& emit) {
        if (!host_ || utf8.empty()) return;
        int ascent = 0, lineH = 0;
        fontMetrics(size, weight, ascent, lineH, italic);

        const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);
        float ly = y;
        size_t pos = 0;
        while (pos < cps.size()) {
            float w = 0;
            size_t end = pos;
            while (end < cps.size()) {
                const uint32_t c = cps[end];
                if (c == '\n') break;
                const ClusterMetric m = clusterMetric(size, weight, cps.data() + end,
                                                     cps.size() - end, italic);
                // 换行宽度也要算上 letter-spacing（否则带间距的长标题会在最后一个字
                // 刚好越界的地方"多挤一个"，与 textWidth 的结论不一致）
                const float adv = m.adv + letterSpacing_;
                if (end > pos && w + adv > maxW) break;
                w += adv;
                end += m.len;
            }
            if (end == pos) end++;
            // 避头点：行首那个字符不能是句读标点，把它并到本行末尾
            if (end < cps.size() && cps[end] != '\n' && NoLineStart(cps[end]))
                end++;
            const bool newline = end < cps.size() && cps[end] == '\n';
            const size_t segEnd = (end > pos && cps[pos] != '\n') ? end : pos;
            if (segEnd > pos) {
                const std::string seg = CodepointsToUtf8(cps.data() + pos, segEnd - pos);
                float lx = x;
                if (align == 1)      lx = x + (maxW - w) / 2.0f;
                else if (align == 2) lx = x + (maxW - w);
                if (lx < x) lx = x;   // 超长行不向左溢出
                drawText(lx, ly + ascent, size, weight, seg, color, false, false, italic, emit);
            }
            ly += lineH;
            pos = newline ? end + 1 : end;
        }
    }

    // 释放所有图集纹理与缓存（shutdown / 上下文重建时调用）
    void clear() {
        atlasGen_++;
        runs_.clear();
        for (auto& kv : atlases_) {
            for (Atlas* a : kv.second) {
                if (a->tex && host_) host_->destroyTex(a->tex);
                delete a;
            }
        }
        atlases_.clear();
        for (auto& kv : colorAtlases_) {
            ColorAtlas* a = kv.second;
            if (a->tex && host_) host_->destroyTex(a->tex);
            delete a;
        }
        colorAtlases_.clear();
        for (ShadowAtlas* sa : shadowAtlases_) {
            if (sa->tex && host_) host_->destroyTex(sa->tex);
            delete sa;
        }
        shadowAtlases_.clear();
        shadow_.on = false;
        scratch_.clear();
        scratch_.shrink_to_fit();
    }

private:
    // 一张图集里的一个字形：格子下标 + advance
    struct Glyph {
        uint16_t cell = 0;
        uint8_t advW = 0;
    };

    struct Atlas {
        void* tex = nullptr;
        int sizePx = 1024;
        int cellW = 0, cellH = 0;
        int cols = 0;
        int next = 1;             // 格子 0 保留给"纯色"（见 rasterGlyph 的空白字形分支）
        int ascent = 0, descent = 0;
        float size = 0;
        int weight = 400;
        int faceIdx = 0;          // 回退链上的面下标（0 = 主面；豆腐块也放这里）
        bool italic = false;
        std::string face;
        bool warnedFull = false;  // 图集满时只喊一次
        std::unordered_map<uint32_t, Glyph> glyphs;
    };

    struct ColorGlyph {
        uint16_t cell = 0;
        uint16_t advW = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        uint16_t baseline = 0;
    };

    struct ColorAtlas {
        void* tex = nullptr;
        int sizePx = 1024;
        int cellW = 0, cellH = 0;
        int cols = 0;
        int next = 0;
        float size = 0;
        bool warnedFull = false;
        std::unordered_map<uint64_t, ColorGlyph> glyphs;
    };

    // ---------- v24 文字阴影的模糊图集 ----------
    // 阴影要的是"字形的模糊覆盖率"，与字形图集不同：格子每边要大出一圈模糊余量
    // （pad），而且内容是可复用的灰度覆盖率。所以按 (字号, 字重, 斜体, 面, 模糊半径)
    // 单独开图集 —— 一个页面通常只有一两个模糊半径，实际就一两张纹理。
    struct ShadowGlyph {
        uint16_t cell = 0;
    };
    struct ShadowAtlas {
        void* tex = nullptr;
        int sizePx = 1024;
        int cell = 0;        // 格子边长 = 基础字形格 + 2*pad
        int pad = 0;         // 每边外扩的模糊余量（像素）
        int cols = 0;
        int next = 0;
        float size = 0;
        int weight = 400;
        int faceIdx = 0;     // 合成面下标（回退链）
        bool italic = false;
        int blurQ = 0;       // ¼ 像素量化的模糊半径
        float blur = 0.0f;
        bool warnedFull = false;
        std::unordered_map<uint32_t, ShadowGlyph> glyphs;   // 码点（相位 0）→ 格
    };
    // 一个字形对应的阴影格引用（UV + 外扩量）
    struct ShadowRef {
        void* tex = nullptr;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        float pad = 0;
    };
    // 当前生效的文字阴影（由 setTextShadow 写入）
    struct ShadowState {
        bool on = false;
        float dx = 0, dy = 0, blur = 0;
        int blurQ = 0;
        uint32_t color = 0;
    };
    ShadowState shadow_;
    std::vector<ShadowAtlas*> shadowAtlases_;

    struct ClusterMetric {
        size_t len = 1;
        float adv = 0;
        bool color = false;
    };

    static void emitGlyph(const EmitFn& emit, void* tex, float x, float y, float w, float h,
                          float gx, float gy, float gw, float t, uint32_t color) {
        if (!emit || !tex) return;
        GlyphQuad q;
        q.tex = tex;
        q.x = x; q.y = y; q.w = w; q.h = h;
        q.u0 = gx / t; q.v0 = gy / t;
        q.u1 = (gx + gw) / t; q.v1 = (gy + h) / t;
        q.color = color;
        emit(q);
    }

    uint64_t atlasKey(float size, int weight, int faceIdx, bool italic) const {
        // 斜体与粗体一样是"另一张图集"：合成倾斜发生在光栅化阶段，不能共用缓存。
        // 低 6 位放面下标（同一个码点解析到不同字体时必须分图集，否则字形会串）。
        const uint32_t fidx = static_cast<uint32_t>(faceIdx < 0 ? 0 : faceIdx) & 0x3Fu;
        return (static_cast<uint64_t>(std::lround(size)) << 32) |
               (static_cast<uint64_t>((weight & 0x7FFF) | (italic ? 0x8000 : 0)) << 16) | fidx;
    }

    static bool hasRoom(const Atlas* a) {
        return a && a->next < a->cols * (a->sizePx / a->cellH);
    }

    Atlas* makeAtlas(uint64_t key, float size, int weight, int faceIdx, bool italic) {
        if (!host_) return nullptr;
        const uint32_t fidx = static_cast<uint32_t>(faceIdx < 0 ? 0 : faceIdx) & 0x3Fu;
        Atlas* a = new Atlas();
        a->size = size;
        a->weight = weight;
        a->faceIdx = static_cast<int>(fidx);
        a->italic = italic;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH;
        a->cols = a->sizePx / a->cellW;
        a->face = fontReady_ ? font_.faceName(a->faceIdx) : asciiFont_;
        a->tex = host_->createTex(a->sizePx, a->sizePx);
        if (!a->tex) {
            delete a;
            return nullptr;
        }
        const binui_font::FontMetrics fm = font_.metrics(a->face, size);
        a->ascent = fm.ascent;
        a->descent = fm.descent;
        atlases_[key].push_back(a);
        return a;
    }

    // 主图集（不带码点）：测量用。有空间就用，全满则开新一代。
    Atlas* getAtlas(float size, int weight, int faceIdx, bool italic = false) {
        const uint64_t key = atlasKey(size, weight, faceIdx, italic);
        auto it = atlases_.find(key);
        if (it != atlases_.end()) {
            for (Atlas* a : it->second)
                if (hasRoom(a)) return a;
            return it->second.back();   // 全满：交给 rasterGlyph 的告警兜底
        }
        return makeAtlas(key, size, weight, faceIdx, italic);
    }

    // 按码点取图集：优先返回**已经含有该字形**的那一代（UV 必须和纹理对得上），
    // 否则找有空间的一代，都没有就再开一代 —— 图集满了是"多占一张纹理"，
    // 而不是"新字直接不出现"。
    Atlas* getAtlasForCp(float size, int weight, int faceIdx, bool italic, uint32_t cp,
                         int phase = 0) {
        const uint64_t key = atlasKey(size, weight, faceIdx, italic);
        const uint32_t gk = GlyphKey(cp, phase);
        auto it = atlases_.find(key);
        if (it != atlases_.end()) {
            for (Atlas* a : it->second)
                if (a->glyphs.count(gk)) return a;
            for (Atlas* a : it->second)
                if (hasRoom(a)) return a;
            if (it->second.size() < 8) return makeAtlas(key, size, weight, faceIdx, italic);
            return it->second.back();
        }
        return makeAtlas(key, size, weight, faceIdx, italic);
    }

    // x 量化成"整数落位 + ¼ 像素相位"：0.37 → (floor, 1)；0.9 → (floor+1, 0)。
    // 相位取"最近的 ¼"，所以最坏位置误差 0.125px（肉眼不可见），换来 4 份位图。
    static void QuantizeSubpixelX(float x, float& outX, int& outPhase) {
        const float fl = std::floor(x);
        int ph = static_cast<int>(std::lround((x - fl) * 4.0f));
        float ix = fl;
        if (ph >= 4) { ph = 0; ix = fl + 1.0f; }
        else if (ph < 0) ph = 0;
        outX = ix;
        outPhase = ph & 3;
    }

    // A/B 开关：BINUI_TEXT_PHASE_OFF=1 退回 0.16.15 的行为（小数落位 + 图集 LINEAR 采样）。
    // 只用来做"锐利度前后对比"的证据图，运行期读一次环境变量，不参与任何逻辑判断。
    static bool PhaseEnabled() {
        static const bool on = std::getenv("BINUI_TEXT_PHASE_OFF") == nullptr;
        return on;
    }
    // 图集里一个字形格的键：码点 × 4 + 相位（码点 ≤ 0x10FFFF，不会溢出）
    static uint32_t GlyphKey(uint32_t cp, int phase) { return (cp << 2) | (uint32_t)(phase & 3); }

    bool ensureScratch(int w, int h) {
        const size_t need = static_cast<size_t>(w) * h * 4;
        if (scratch_.size() < need) scratch_.resize(need);
        std::memset(scratch_.data(), 0, need);
        return true;
    }

    // 把单个码点（指定 ¼ 像素相位）光栅化进图集格子（FreeType 灰度字形 → 白字预乘 alpha）。
    // 相位是**位图的一部分**：renderGlyph 用 FT_Set_Transform 把轮廓先右移 phase/4 像素，
    // 所以同一个字形最多占 4 个格子；只有真的用到某个相位时才造它（懒加载）。
    void rasterGlyph(Atlas* a, uint32_t cp, int phase = 0) {
        const int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) {
            // 图集满了：以前这里**静默 return**，新字形直接不出现 —— 用户看到的是
            // "某些字时有时无"，最难查。至少喊一声（同一张图集只喊一次）。
            if (!a->warnedFull) {
                a->warnedFull = true;
                std::fprintf(stderr,
                    "[atlas] 字形图集已满（%.0fpx / weight %d / face #%d，共 %d 格）："
                    "该组合下的新字形将被丢弃。\n"
                    "        这是容量上限，不是排版问题；减少字号/字重组合，或把这一条当 bug 报。\n",
                    static_cast<double>(a->size), a->weight, a->faceIdx, total);
            }
            return;
        }
        binui_font::GrayGlyph g;
        // 粗体：atlas key 里带 weight，粗体是独立的一张图集；这里让 FreeType
        // 合成加粗（FT_GlyphSlot_Embolden），否则 font-weight:700 只是换个空图集重复光栅化
        bool ok = font_.renderGlyph(a->face, a->size, cp, a->weight >= 600, a->italic,
                                    phase, g);
        if (!ok && !IsInvisibleCp(cp)) {
            // 缺字 → 画豆腐块。留白会被当成"引擎显示 bug"，空方框才是给用户的
            // 明确信号：这个字体链里没有这个字符。浏览器和终端都是这么做的。
            ok = font_.renderMissingGlyph(a->size, a->weight >= 600, a->italic, g);
        }
        if (!ok || !g.alpha || g.height <= 0) {
            // 空白字形（如空格）：无像素但有有效 advance —— 仍占一格并缓存，
            // 否则 glyphFor 返回 null，测宽/绘制都不推进，单词会挤在一起
            if (ok && g.advance > 0) {
                Glyph gl;
                gl.cell = static_cast<uint16_t>(a->next++);   // 格子保持全零纹理（不可见）
                gl.advW = static_cast<uint8_t>(std::min(255, std::max(0, g.advance)));
                a->glyphs[GlyphKey(cp, phase)] = gl;
            }
            return;
        }

        ensureScratch(a->cellW, a->cellH);
        const int ox = 1 + g.left;
        const int oy = (a->ascent + 1) - g.top;
        uint8_t* dst = scratch_.data();
        for (int y = 0; y < g.height; y++) {
            const int py = oy + y;
            if (py < 0 || py >= a->cellH) continue;
            const uint8_t* src = g.alpha + static_cast<size_t>(y) * g.pitch;
            uint8_t* row = dst + static_cast<size_t>(py) * a->cellW * 4;
            for (int x = 0; x < g.width; x++) {
                const int px = ox + x;
                if (px < 0 || px >= a->cellW) continue;
                const uint8_t al = src[x];
                if (al == 0) continue;
                uint8_t* p = row + static_cast<size_t>(px) * 4;
                p[0] = p[1] = p[2] = p[3] = al;   // BGRA 全 = alpha（白字预乘）
            }
        }

        const int cell = a->next++;
        const int cx = (cell % a->cols) * a->cellW;
        const int cy = (cell / a->cols) * a->cellH;
        host_->updateTex(a->tex, cx, cy, a->cellW, a->cellH, scratch_.data());

        Glyph gl;
        gl.cell = static_cast<uint16_t>(cell);
        // advance 用 HarfBuzz/FreeType 的真实值（以前 +1，每个字形多占 1px：
        // 文字比真实排版宽、组合符号被推离基字、换行宽度与实际绘制不符）
        gl.advW = static_cast<uint8_t>(std::min(255, std::max(0, g.advance)));
        a->glyphs[GlyphKey(cp, phase)] = gl;
    }

    const Glyph* glyphFor(Atlas* a, uint32_t cp, int phase = 0) {
        const uint32_t gk = GlyphKey(cp, phase);
        auto it = a->glyphs.find(gk);
        if (it != a->glyphs.end()) return &it->second;
        rasterGlyph(a, cp, phase);
        it = a->glyphs.find(gk);
        return it != a->glyphs.end() ? &it->second : nullptr;
    }

    // ---------- v24：文字阴影 ----------
    // 可分离高斯模糊（σ = blur/2，核截断 3σ）。核按权重和归一化：阴影的"深浅"必须
    // 与字形覆盖率线性对应，否则大模糊会整体发灰。
    static void BlurAlpha(std::vector<float>& buf, int w, int h, float sigma) {
        if (sigma <= 0.0f || w <= 0 || h <= 0) return;
        const int rad = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        std::vector<float> k(static_cast<size_t>(rad) * 2 + 1);
        float sum = 0.0f;
        const float inv = 1.0f / (2.0f * sigma * sigma);
        for (int i = -rad; i <= rad; i++) {
            const float v = std::exp(-static_cast<float>(i * i) * inv);
            k[static_cast<size_t>(i + rad)] = v;
            sum += v;
        }
        for (float& v : k) v /= sum;
        std::vector<float> tmp(buf.size(), 0.0f);
        for (int y = 0; y < h; y++) {
            const float* src = buf.data() + static_cast<size_t>(y) * w;
            float* dst = tmp.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; x++) {
                float acc = 0.0f;
                const int lo = std::max(0, x - rad), hi = std::min(w - 1, x + rad);
                for (int xx = lo; xx <= hi; xx++)
                    acc += src[xx] * k[static_cast<size_t>(xx - x + rad)];
                dst[x] = acc;
            }
        }
        for (int y = 0; y < h; y++) {
            float* dst = buf.data() + static_cast<size_t>(y) * w;
            const int lo = std::max(0, y - rad), hi = std::min(h - 1, y + rad);
            for (int x = 0; x < w; x++) {
                float acc = 0.0f;
                for (int yy = lo; yy <= hi; yy++)
                    acc += tmp[static_cast<size_t>(yy) * w + x] * k[static_cast<size_t>(yy - y + rad)];
                dst[x] = acc;
            }
        }
    }

    // 取（必要时建）某个 (字号, 字重, 斜体, 面, 模糊半径) 的模糊图集。
    // 条目很少（一页通常 1~2 个模糊半径），线性找足够。
    ShadowAtlas* getShadowAtlas(Atlas* base, int blurQ) {
        if (!host_ || !base || blurQ <= 0) return nullptr;
        for (ShadowAtlas* sa : shadowAtlases_) {
            if (sa->blurQ == blurQ && sa->faceIdx == base->faceIdx &&
                sa->weight == base->weight && sa->italic == base->italic &&
                static_cast<int>(sa->size) == static_cast<int>(base->size))
                return sa;
        }
        // 模糊半径 → σ = blur/2 → 余量 3σ+1（与 box-shadow 的裁断距离同一口径）
        const float blur = static_cast<float>(blurQ) * 0.25f;
        const float sigma = blur * 0.5f;
        int pad = static_cast<int>(std::ceil(sigma * 3.0f)) + 1;
        int cell = base->cellW + 2 * pad;
        const int kMaxCell = 192;   // 再大就装不下几个格子了，模糊按上限收
        if (cell > kMaxCell) {
            pad = std::max(0, (kMaxCell - base->cellW) / 2);
            cell = base->cellW + 2 * pad;
        }
        if (cell <= 0) return nullptr;
        ShadowAtlas* sa = new ShadowAtlas();
        sa->size = base->size;
        sa->weight = base->weight;
        sa->faceIdx = base->faceIdx;
        sa->italic = base->italic;
        sa->blurQ = blurQ;
        sa->blur = blur;
        sa->pad = pad;
        sa->cell = cell;
        sa->cols = std::max(1, sa->sizePx / cell);
        sa->tex = host_->createTex(sa->sizePx, sa->sizePx);
        if (!sa->tex) { delete sa; return nullptr; }
        shadowAtlases_.push_back(sa);
        return sa;
    }

    // 在模糊图集里造一格：先按相位 0 光栅化字形，把覆盖率放进"基础格 + 每边 pad"
    // 的大格子里，再做高斯模糊 —— 模糊的结果就是这块字形在该半径下的柔影。
    void rasterShadowGlyph(ShadowAtlas* sa, Atlas* base, uint32_t cp) {
        const int total = sa->cols * (sa->sizePx / sa->cell);
        if (sa->next >= total) {
            if (!sa->warnedFull) {
                sa->warnedFull = true;
                std::fprintf(stderr,
                    "[atlas] 文字阴影图集已满（%.0fpx / blur %.2f / 共 %d 格）："
                    "该组合下新字形的阴影将缺失（文字本身正常）。\n",
                    static_cast<double>(sa->size), static_cast<double>(sa->blur), total);
            }
            return;
        }
        binui_font::GrayGlyph g;
        bool ok = font_.renderGlyph(base->face, base->size, cp, base->weight >= 600,
                                    base->italic, 0, g);
        if (!ok && !IsInvisibleCp(cp))
            ok = font_.renderMissingGlyph(base->size, base->weight >= 600, base->italic, g);
        // 空白字形（空格等）没有像素：不留格子，直接记一个"无阴影"标记即可
        // —— 但那样每次查表都要重新光栅化，所以还是占一格全零（与字形图集一致的做法）
        const int cs = sa->cell;
        std::vector<float> cov(static_cast<size_t>(cs) * cs, 0.0f);
        if (ok && g.alpha && g.height > 0) {
            const int ox = 1 + g.left + sa->pad;
            const int oy = (base->ascent + 1) - g.top + sa->pad;
            for (int y = 0; y < g.height; y++) {
                const int py = oy + y;
                if (py < 0 || py >= cs) continue;
                const uint8_t* src = g.alpha + static_cast<size_t>(y) * g.pitch;
                float* row = cov.data() + static_cast<size_t>(py) * cs;
                for (int x = 0; x < g.width; x++) {
                    const int px = ox + x;
                    if (px < 0 || px >= cs) continue;
                    row[px] = static_cast<float>(src[x]);
                }
            }
        } else if (!ok) {
            return;                      // 连豆腐块都画不出：不占格子（下帧再试）
        }

        BlurAlpha(cov, cs, cs, sa->blur * 0.5f);

        ensureScratch(cs, cs);
        uint8_t* dst = scratch_.data();
        for (int i = 0; i < cs * cs; i++) {
            int a = static_cast<int>(cov[static_cast<size_t>(i)] + 0.5f);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            // 白字预乘：BGRA 全 = 覆盖率（画的时候用顶点色染成阴影色）
            dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = dst[i * 4 + 3] =
                static_cast<uint8_t>(a);
        }
        const int cell = sa->next++;
        const int cx = (cell % sa->cols) * sa->cell;
        const int cy = (cell / sa->cols) * sa->cell;
        host_->updateTex(sa->tex, cx, cy, sa->cell, sa->cell, scratch_.data());
        ShadowGlyph sg;
        sg.cell = static_cast<uint16_t>(cell);
        sa->glyphs[cp] = sg;
    }

    // 取一个字形的阴影格。没有阴影 / 造不出来都返回空（调用方跳过这个字的阴影）。
    ShadowRef shadowRef(Atlas* base, uint32_t cp) {
        ShadowRef r;
        if (!shadow_.on || !base || shadow_.blurQ <= 0) return r;
        ShadowAtlas* sa = getShadowAtlas(base, shadow_.blurQ);
        if (!sa) return r;
        auto it = sa->glyphs.find(cp);
        if (it == sa->glyphs.end()) {
            rasterShadowGlyph(sa, base, cp);
            it = sa->glyphs.find(cp);
            if (it == sa->glyphs.end()) return r;
        }
        const float cs = static_cast<float>(sa->cell);
        const float t = static_cast<float>(sa->sizePx);
        r.tex = sa->tex;
        r.u0 = (it->second.cell % sa->cols) * cs / t;
        r.v0 = (it->second.cell / sa->cols) * cs / t;
        r.u1 = (it->second.cell % sa->cols) * cs / t + cs / t;
        r.v1 = (it->second.cell / sa->cols) * cs / t + cs / t;
        r.pad = static_cast<float>(sa->pad);
        return r;
    }

    // 给一个字形四边形挂上阴影格：
    //   blur = 0 → 复用字形自己那一格（阴影就是偏移一份字形，最精确、零额外纹理）；
    //   blur > 0 → 模糊图集里的一格（每边多 pad 像素）。
    void FillShadowQuad(GlyphRunQuad& q, Atlas* a, uint32_t cp,
                        float gx, float gy, float cell, float t) {
        if (!shadow_.on) return;
        if (shadow_.blurQ <= 0) {
            q.sTex = a->tex;
            q.su0 = gx / t; q.sv0 = gy / t;
            q.su1 = (gx + cell) / t; q.sv1 = (gy + cell) / t;
            q.sPad = 0.0f;
            return;
        }
        const ShadowRef r = shadowRef(a, cp);
        if (!r.tex) return;
        q.sTex = r.tex;
        q.su0 = r.u0; q.sv0 = r.v0; q.su1 = r.u1; q.sv1 = r.v1;
        q.sPad = r.pad;
    }

    // ---------- 彩色字形（FreeType COLR 合成） ----------
    bool rasterColorCluster(ColorAtlas* a, uint64_t key, const uint32_t* cps, size_t n) {
        const int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) {
            if (!a->warnedFull) {
                a->warnedFull = true;
                std::fprintf(stderr,
                    "[atlas] 彩色字形图集已满（%.0fpx，共 %d 格）：emoji 将退回灰度/豆腐块。\n",
                    static_cast<double>(a->size), total);
            }
            return false;
        }

        binui_font::ColorGlyph cg;
        if (!font_.renderColorCluster(kEmojiFont, a->size, cps, n, cg)) return false;
        if (cg.width <= 0 || cg.height <= 0 || !cg.bgra) return false;

        ensureScratch(a->cellW, a->cellH);
        const int cw = std::min(cg.width, a->cellW);
        const int ch = std::min(cg.height, a->cellH);
        uint8_t* dst = scratch_.data();
        for (int y = 0; y < ch; y++)
            std::memcpy(dst + static_cast<size_t>(y) * a->cellW * 4,
                        cg.bgra + static_cast<size_t>(y) * cg.width * 4,
                        static_cast<size_t>(cw) * 4);

        const int cell = a->next++;
        const int cx = (cell % a->cols) * a->cellW;
        const int cy = (cell / a->cols) * a->cellH;
        host_->updateTex(a->tex, cx, cy, a->cellW, a->cellH, scratch_.data());

        ColorGlyph g;
        g.cell = static_cast<uint16_t>(cell);
        g.advW = static_cast<uint16_t>(std::min(4095, cg.advance + 1));
        g.w = static_cast<uint16_t>(cw);
        g.h = static_cast<uint16_t>(ch);
        g.baseline = static_cast<uint16_t>(std::min(4095, cg.baseline));
        a->glyphs[key] = g;
        return true;
    }

    ColorAtlas* getColorAtlas(float size) {
        const uint32_t key = static_cast<uint32_t>(std::lround(size));
        auto it = colorAtlases_.find(key);
        if (it != colorAtlases_.end()) return it->second;
        if (!host_) return nullptr;

        ColorAtlas* a = new ColorAtlas();
        a->size = size;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH * 4;
        a->cols = a->sizePx / a->cellW;
        if (a->cols < 1) { delete a; return nullptr; }
        a->tex = host_->createTex(a->sizePx, a->sizePx);
        if (!a->tex) { delete a; return nullptr; }
        colorAtlases_[key] = a;
        return a;
    }

    const ColorGlyph* colorGlyphFor(ColorAtlas* a, const uint32_t* cps, size_t n) {
        const uint64_t key = EmojiClusterKey(cps, n);
        auto it = a->glyphs.find(key);
        if (it != a->glyphs.end()) return &it->second;
        rasterColorCluster(a, key, cps, n);
        it = a->glyphs.find(key);
        return it != a->glyphs.end() ? &it->second : nullptr;
    }

    ClusterMetric clusterMetric(float size, int weight, const uint32_t* cps, size_t n,
                                bool italic = false) {
        ClusterMetric m;
        m.len = EmojiClusterLen(cps, n);
        const float fallback = std::ceil(size * 0.6f);
        const int firstFace = faceIdxForCp(cps[0]);
        // 彩色只在该码点确实由 emoji 面覆盖时才尝试：符号（⌘⌫✕）命中 Segoe UI Symbol
        // 就走灰度，不会被画成彩色图形；真 emoji 才走 COLR。
        const bool tryColor = fontReady_ && (m.len > 1 || firstFace == emojiFaceIdx());
        if (tryColor) {
            ColorAtlas* ca = getColorAtlas(size);
            const ColorGlyph* g = ca ? colorGlyphFor(ca, cps, m.len) : nullptr;
            if (g) {
                m.adv = static_cast<float>(g->advW);
                m.color = true;
                return m;
            }
        }
        if (m.len <= 1) {
            Atlas* a = getAtlasForCp(size, weight, firstFace, italic, cps[0]);
            const Glyph* g = a ? glyphFor(a, cps[0]) : nullptr;
            m.adv = g ? static_cast<float>(g->advW) : fallback;
            return m;
        }
        for (size_t i = 0; i < m.len; i++) {
            Atlas* a = getAtlasForCp(size, weight, faceIdxForCp(cps[i]), italic, cps[i]);
            const Glyph* g = a ? glyphFor(a, cps[i]) : nullptr;
            m.adv += g ? static_cast<float>(g->advW) : fallback;
        }
        return m;
    }

    // ---------- 逐字形回退 ----------
    // 谁有字形是字体自报的 cmap，不是码点区间能推出来的（Unicode 的 Block 只是
    // 管理分组，标准明确说不能用来选字体）。所以这里就一件事：问字体。
    // 字体服务返回 -1 = 所有候选都没有 → 用 0 号面（豆腐块是几何，跟哪个字体无关）。
    int faceIdxForCp(uint32_t cp) {
        if (!fontReady_) return 0;
        const int idx = font_.faceIndexFor(cp);
        return idx < 0 ? 0 : idx;
    }

    // 设计上就不可见的码点：不画豆腐块（否则一串 emoji 里会凭空多出方框）
    static bool IsInvisibleCp(uint32_t cp) {
        if (cp < 0x20) return true;                                    // C0 控制符
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) return true;
        if (cp >= 0xFE00 && cp <= 0xFE0F) return true;                  // 变体选择符 VS1–16
        if (cp >= 0xE0100 && cp <= 0xE01EF) return true;                // 变体选择符 VS17–256
        if (cp == 0x20E3) return true;                                  // 键帽组合符
        return false;
    }

    // emoji 面在链上的下标（-1 = 没有这个候选）。彩色路径只在这个面上尝试：
    // 真 emoji 走彩色，符号（⌘⌫✕）命中 Segoe UI Symbol 就不会被画成彩色图形。
    int emojiFaceIdx() {
        if (!emojiIdxInit_) {
            emojiIdxInit_ = true;
            emojiIdx_ = -1;
            if (fontReady_) {
                const auto& chain = font_.fallbackChain();
                for (size_t i = 0; i < chain.size(); i++)
                    if (chain[i] == kEmojiFont) { emojiIdx_ = static_cast<int>(i); break; }
            }
        }
        return emojiIdx_;
    }

    static constexpr const char* kEmojiFont = "Segoe UI Emoji";

    IGlyphTexHost* host_ = nullptr;
    // letter-spacing（v0.16.40）：每个字形簇后额外的前进量（像素，可为负）。
    // 排版期唯一的"字间"自由度，所以放在这里而不是每个调用点各传一次。
    float letterSpacing_ = 0.0f;
    // 整形结果缓存（键 = 文本+字号+字重+斜体+字体面+letter-spacing）
    std::unordered_map<uint64_t, GlyphRun> runs_;
    uint64_t runClock_ = 0;
    // 图集代数：任何纹理新建/销毁都 ++，缓存条目按它判断是否过期
    uint64_t atlasGen_ = 1;
    binui_font::FontEngine font_;
    bool fontReady_ = false;
    std::string asciiFont_ = "Segoe UI";
    std::unordered_map<uint64_t, std::vector<Atlas*>> atlases_;
    std::unordered_map<uint32_t, ColorAtlas*> colorAtlases_;
    std::vector<uint8_t> scratch_;   // 光栅化临时缓冲（cellW*cellH*4，BGRA）
    bool emojiIdxInit_ = false;
    int emojiIdx_ = -1;
};

} // namespace binui

#endif // BINUI_GLYPH_ATLAS_H
