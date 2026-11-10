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

    // 初始化 FreeType/HarfBuzz + 注册系统字体；false = 字体服务不可用（文本降级为空白）
    bool initFonts() {
        fontReady_ = font_.init();
        binui_font::registerSystemFonts(font_);
        return fontReady_;
    }
    bool fontReady() const { return fontReady_; }

    void setFontName(const char* name) {
        if (name && *name) asciiFont_ = name;
    }
    const std::string& fontName() const { return asciiFont_; }

    // 文本宽度（像素，逐字形 advance 之和；与 drawText 用的是同一套度量）
    float textWidth(float size, int weight, const std::string& utf8, bool italic) {
        if (!host_) return 0;
        const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);
        float w = 0;
        size_t i = 0;
        while (i < cps.size()) {
            const ClusterMetric m = clusterMetric(size, weight, cps.data() + i,
                                                 cps.size() - i, italic);
            w += m.adv;
            i += m.len;
        }
        return w;
    }

    // 行高与基线（主面度量）
    void fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
        Atlas* a = getAtlas(size, weight, 0, italic);
        if (!a) { ascent = lineHeight = static_cast<int>(size); return; }
        ascent = a->ascent;
        lineHeight = a->ascent + a->descent;
    }

    // 画一行文本。x/y 语义与 IBinUIGfx::drawText 一致：
    //   y 是基线（centerV 时为矩形中心），centerH 时 x 为矩形中心。
    void drawText(float x, float y, float size, int weight, const std::string& utf8,
                  uint32_t color, bool centerH, bool centerV, bool italic, const EmitFn& emit) {
        if (!host_ || utf8.empty()) return;
        if (centerH) x -= textWidth(size, weight, utf8, italic) / 2;

        int ascent = 0, lineH = 0;
        fontMetrics(size, weight, ascent, lineH, italic);
        if (centerV) y -= lineH / 2.0f;
        // 像素对齐：字形位图天生是整数像素设计，落在半像素上会被过滤糊一层。
        // advance 本身是整数，所以只要把起点/基线取整，整行字形就都在整数像素上。
        float baseline = std::floor(y + 0.5f);
        x = std::floor(x + 0.5f);

        const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);
        PremultColor(color);

        float cx = x;
        auto drawGray = [&](uint32_t cp) {
            Atlas* a = getAtlasForCp(size, weight, faceIdxForCp(cp), italic, cp);
            if (!a) return;
            const Glyph* g = glyphFor(a, cp);
            if (!g) return;
            const float t = static_cast<float>(a->sizePx);
            const float cell = static_cast<float>(a->cellW);
            const float gx = (g->cell % a->cols) * cell;
            const float gy = (g->cell / a->cols) * cell;
            const float top = baseline - (a->ascent + 1);
            emitGlyph(emit, a->tex, cx, top, cell, cell, gx, gy, cell, t, color);
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
                    emitGlyph(emit, ca->tex, cx, top, static_cast<float>(g->w),
                              static_cast<float>(g->h), gx, gy, static_cast<float>(g->w),
                              t, 0xFFFFFFFF);   // 彩色字形：白色顶点，不染色
                    cx += g->advW;
                    i += n;
                    continue;
                }
            }
            for (size_t k = 0; k < n; k++) drawGray(cps[i + k]);
            i += n;
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
                if (end > pos && w + m.adv > maxW) break;
                w += m.adv;
                end += m.len;
            }
            if (end == pos) end++;
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
        for (auto& kv : atlases_) {
            for (Atlas* a : kv.second) {
                if (a) {
                    if (a->tex && host_) host_->destroyTex(a->tex);
                    delete a;
                }
            }
        }
        atlases_.clear();
        for (auto& kv : colorAtlases_) {
            ColorAtlas* a = kv.second;
            if (!a) continue;
            if (a->tex && host_) host_->destroyTex(a->tex);
            delete a;
        }
        colorAtlases_.clear();
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
    Atlas* getAtlasForCp(float size, int weight, int faceIdx, bool italic, uint32_t cp) {
        const uint64_t key = atlasKey(size, weight, faceIdx, italic);
        auto it = atlases_.find(key);
        if (it != atlases_.end()) {
            for (Atlas* a : it->second)
                if (a->glyphs.count(cp)) return a;
            for (Atlas* a : it->second)
                if (hasRoom(a)) return a;
            if (it->second.size() < 8) return makeAtlas(key, size, weight, faceIdx, italic);
            return it->second.back();
        }
        return makeAtlas(key, size, weight, faceIdx, italic);
    }

    bool ensureScratch(int w, int h) {
        const size_t need = static_cast<size_t>(w) * h * 4;
        if (scratch_.size() < need) scratch_.resize(need);
        std::memset(scratch_.data(), 0, need);
        return true;
    }

    // 把单个码点光栅化进图集格子（FreeType 灰度字形 → 白字预乘 alpha）
    void rasterGlyph(Atlas* a, uint32_t cp) {
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
        bool ok = font_.renderGlyph(a->face, a->size, cp, a->weight >= 600, a->italic, g);
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
                a->glyphs[cp] = gl;
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
        a->glyphs[cp] = gl;
    }

    const Glyph* glyphFor(Atlas* a, uint32_t cp) {
        auto it = a->glyphs.find(cp);
        if (it != a->glyphs.end()) return &it->second;
        rasterGlyph(a, cp);
        it = a->glyphs.find(cp);
        return it != a->glyphs.end() ? &it->second : nullptr;
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
