// binui_font_renderer.cpp - FreeType 光栅化 + HarfBuzz 整形实现
#include "binui_font_renderer.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include FT_TRUETYPE_TABLES_H

#include <hb.h>
#include <hb-ft.h>

#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace binui_font {

namespace {

// 复杂脚本范围（HarfBuzz 整形前检测用，粗略覆盖主流）
struct ScriptRange { uint32_t lo, hi; };
const ScriptRange kComplexScripts[] = {
    { 0x0590, 0x08FF },  // Hebrew / Arabic / Syriac / Thaana / N'Ko / Arabic Ext-A
    { 0x0900, 0x0DFF },  // Devanagari..Sinhala（印度语系）
    { 0x0E00, 0x0FFF },  // Thai / Lao / Tibetan
    { 0x1000, 0x109F },  // Myanmar
    { 0x1100, 0x11FF },  // Hangul Jamo（无 GSUB，但保守整形无碍）
    { 0x1700, 0x17FF },  // Tagalog..Khmer
    { 0x1900, 0x1CFF },  // Limbu..Sundanese/Vedic Ext
    { 0xA800, 0xA8FF },  // Kayah Li / Devanagari Extended
    { 0xA900, 0xA9FF },  // Javanese / Myanmar Ext-B
    { 0xAA00, 0xAA5F },  // Cham
    { 0xAA80, 0xAADF },  // Tai Viet
    { 0xAB00, 0xAB2F },  // Ethiopic Extended-A
    { 0xFB50, 0xFDFF },  // Arabic Presentation Forms-A
    { 0xFE70, 0xFEFF },  // Arabic Presentation Forms-B
};

bool inComplexScript(uint32_t cp) {
    for (const auto& r : kComplexScripts)
        if (cp >= r.lo && cp <= r.hi) return true;
    return false;
}

int round26d6(long v) { return static_cast<int>((v + 32) / 64); }

// ============================================================
// 字形光栅化的两个"观感"参数（只影响灰阶覆盖率，不影响度量）
// ============================================================
// 小字号启用"轻 hinting"：只调竖笔位置/宽度，救 12~16px 的清晰度；
// 更大字号仍走 NO_HINTING —— 保留 README 承诺的"大字号跨平台一致"。
constexpr float kLightHintMaxPx = 16.5f;

// 灰度覆盖率 gamma：FreeType 给的线性覆盖率直接当 alpha 会让文字偏细
// （亮底黑字偏粗、暗底白字偏细）。γ=1.3 接近 DirectWrite 的默认观感。
constexpr float kGlyphGamma = 1.3f;

FT_Int32 loadFlagsFor(float sizePx) {
    // 可用环境变量覆盖，方便不重编就对比三种观感（见 README「文字渲染」）：
    //   BINUI_FONT_HINT=0 全关（原来行为） / 1 全 light / 2 全 full（hinting 最强，笔画会吸附整数像素）
    // 默认（未设置）= 小字号 light、大字号关
    static const int mode = [] {
        const char* e = std::getenv("BINUI_FONT_HINT");
        if (!e || !*e) return 3;
        return std::atoi(e);
    }();
    const bool small = (sizePx <= kLightHintMaxPx);
    if (mode == 0) return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
    if (mode == 1) return FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    if (mode == 2) return FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP;
    // NO_BITMAP：只用轮廓（彩色 bitmap 字形由 COLR/彩色路径单独处理）
    if (small) return FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
}

// 256 项 gamma 查表（首末两端保持 0/255，单调不溢出）
const uint8_t* glyphGammaLut() {
    static uint8_t lut[256] = {};
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) {
            const float v = static_cast<float>(i) / 255.0f;
            const float g = std::pow(v, 1.0f / kGlyphGamma);
            int o = static_cast<int>(g * 255.0f + 0.5f);
            if (o < 0) o = 0;
            if (o > 255) o = 255;
            lut[i] = static_cast<uint8_t>(o);
        }
        lut[0] = 0;
        lut[255] = 255;
        built = true;
    }
    return lut;
}

} // namespace

struct FontEngine::Impl {
    FT_Library ft = nullptr;
    std::map<std::string, std::string> faces;   // 名 → 文件路径
    std::string defaultFace = "Segoe UI";

    struct Session {
        FT_Face face = nullptr;
        hb_font_t* hfont = nullptr;
        FontMetrics m;
        std::vector<uint8_t> scratch;   // 灰度字形 + gamma 修正后的输出（renderGlyph 用）
    };
    std::unordered_map<std::string, Session*> sessions; // key: face|sizePx
    FT_Color* palette = nullptr;    // 当前 face 的 palette 0（随最近一次加载）
    FT_Face palFace = nullptr;

    std::vector<uint8_t> colorBuf;  // 彩色合成输出缓冲

    static Session* getSession(Impl* p, const std::string& face, float size);

    ~Impl();
};

FontEngine::Impl::~Impl() {
    for (auto& kv : sessions) {
        if (kv.second->hfont) hb_font_destroy(kv.second->hfont);
        if (kv.second->face) FT_Done_Face(kv.second->face);
        delete kv.second;
    }
    sessions.clear();
    if (ft) FT_Done_FreeType(ft);
}

FontEngine::FontEngine() : impl_(new Impl()) {}
FontEngine::~FontEngine() { delete impl_; }

bool FontEngine::init() {
    if (impl_->ft) return true;
    if (FT_Init_FreeType(&impl_->ft) != 0) return false;
    return true;
}

void FontEngine::shutdown() {
    delete impl_;
    impl_ = new Impl();
}

bool FontEngine::addFont(const std::string& name, const std::string& path) {
    if (name.empty() || path.empty()) return false;
    impl_->faces[name] = path;
    return true;
}

void FontEngine::setDefaultFace(const std::string& name) {
    if (!name.empty()) impl_->defaultFace = name;
}

const std::string& FontEngine::defaultFace() const { return impl_->defaultFace; }

// 取 (face, sizePx) 会话，必要时创建（FT_Face + hb_font + 度量）
FontEngine::Impl::Session* FontEngine::Impl::getSession(Impl* p, const std::string& face,
                                                        float size) {
    int sizePx = static_cast<int>(std::lround(size));
    std::string key = face + "|" + std::to_string(sizePx);
    auto it = p->sessions.find(key);
    if (it != p->sessions.end()) return it->second;

    auto fit = p->faces.find(face);
    std::string path;
    if (fit != p->faces.end()) path = fit->second;
    else {
        auto dit = p->faces.find(p->defaultFace);
        if (dit == p->faces.end()) return nullptr;
        path = dit->second;
    }

    FT_Face f = nullptr;
    if (FT_New_Face(p->ft, path.c_str(), 0, &f) != 0) return nullptr;
    FT_Set_Pixel_Sizes(f, 0, sizePx > 0 ? static_cast<unsigned>(sizePx) : 1);

    auto* s = new Session();
    s->face = f;
    s->hfont = hb_ft_font_create_referenced(f);
    // HarfBuzz 的整形 advance 必须与光栅化用同一套 hinting 开关，否则定位会偏
    hb_ft_font_set_load_flags(s->hfont, loadFlagsFor(static_cast<float>(sizePx)));
    s->m.ascent = round26d6(f->size->metrics.ascender);
    s->m.descent = -round26d6(f->size->metrics.descender);
    p->sessions[key] = s;
    return s;
}

FontMetrics FontEngine::metrics(const std::string& face, float size) {
    auto* s = Impl::getSession(impl_, face, size);
    if (!s) {
        FontMetrics m;
        m.ascent = static_cast<int>(std::ceil(size * 0.8f));
        m.descent = static_cast<int>(std::ceil(size * 0.2f));
        return m;
    }
    return s->m;
}

bool FontEngine::renderGlyph(const std::string& face, float size, uint32_t cp, GrayGlyph& out) {
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->face) return false;
    FT_Face f = s->face;

    FT_UInt gid = FT_Get_Char_Index(f, cp);
    if (gid == 0) return false;
    if (FT_Load_Glyph(f, gid, loadFlagsFor(size)) != 0) return false;
    if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;

    const FT_Bitmap& bm = f->glyph->bitmap;
    out.width = static_cast<int>(bm.width);
    out.height = static_cast<int>(bm.rows);
    out.left = f->glyph->bitmap_left;
    out.top = f->glyph->bitmap_top;
    out.advance = round26d6(f->glyph->advance.x);

    // gamma 修正：把线性覆盖率映射成观感接近 DirectWrite 的 alpha。
    // 需要可写副本（FreeType 的 bitmap buffer 是只读语义且下一次调用即失效，
    // 与原契约一致：out.alpha 在下次 renderGlyph 前有效）。
    if (!bm.buffer || bm.rows == 0 || bm.width == 0) {
        out.pitch = 0;
        out.alpha = nullptr;
        return true;
    }
    const uint8_t* lut = glyphGammaLut();
    const size_t rowBytes = static_cast<size_t>(bm.width);
    s->scratch.assign(rowBytes * bm.rows, 0);
    for (unsigned r = 0; r < bm.rows; r++) {
        const uint8_t* src = bm.buffer + static_cast<ptrdiff_t>(r) * bm.pitch;
        uint8_t* dst = s->scratch.data() + r * rowBytes;
        for (size_t c = 0; c < rowBytes; c++) dst[c] = lut[src[c]];
    }
    out.pitch = static_cast<int>(rowBytes);
    out.alpha = s->scratch.data();
    return true;
}

// COLR v0：枚举一个 glyph 的层（非空即彩色）
static bool glyphHasColorLayers(FT_Face f, FT_UInt gid) {
    FT_LayerIterator it = {};
    FT_UInt lg = 0, lpal = 0;
    return FT_Get_Color_Glyph_Layer(f, gid, &lg, &lpal, &it) != 0;
}

// 彩色判定：COLR 层或内嵌彩色位图（CBDT/SBBIX，如区域指示符国旗）
static bool glyphIsColor(FT_Face f, FT_UInt gid) {
    if (glyphHasColorLayers(f, gid)) return true;
    if (FT_Load_Glyph(f, gid, FT_LOAD_COLOR) != 0) return false;
    return f->glyph->format == FT_GLYPH_FORMAT_BITMAP &&
           f->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
}

bool FontEngine::hasColor(const std::string& face, float size,
                          const uint32_t* cps, size_t n) {
    if (!cps || n == 0) return false;
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->face) return false;
    std::vector<ShapedGlyph> gs = shape(face, size, cps, n);
    if (gs.empty()) return false;
    return glyphIsColor(s->face, gs[0].gid);
}

bool FontEngine::renderColorCluster(const std::string& face, float size,
                                    const uint32_t* cps, size_t n, ColorGlyph& out) {
    if (!cps || n == 0) return false;
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->face) return false;
    FT_Face f = s->face;

    std::vector<ShapedGlyph> gs = shape(face, size, cps, n);
    if (gs.empty()) return false;
    if (!glyphIsColor(f, gs[0].gid)) return false;

    // 每层：位置 + 像素拷贝 + 颜色（bgra=true 时为直通 BGRA 内嵌位图）
    // 注意：必须立即拷贝像素——后续 FT_Load_Glyph 会覆盖 face->glyph->bitmap
    struct Layer { int x, y, w, h, pitch; std::vector<uint8_t> px; uint8_t r, g, b, a; bool bgra; };
    std::vector<Layer> layers;
    int xAccum = 0;
    int advTotal = 0;
    for (const auto& g : gs) {
        advTotal += g.xAdvance;
        if (FT_Load_Glyph(f, g.gid, FT_LOAD_COLOR) != 0) return false;

        // 直接彩色位图（CBDT/SBBIX 内嵌位图）
        if (f->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            const FT_Bitmap& bm = f->glyph->bitmap;
            if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
                Layer L;
                L.x = f->glyph->bitmap_left + xAccum + g.xOffset;
                L.y = f->glyph->bitmap_top;
                L.w = static_cast<int>(bm.width);
                L.h = static_cast<int>(bm.rows);
                L.pitch = bm.pitch;
                L.px.assign(bm.buffer, bm.buffer + static_cast<size_t>(bm.rows) * bm.pitch);
                L.bgra = true;
                if (L.w > 0 && L.h > 0) layers.push_back(L);
                xAccum += g.xAdvance;
                continue;
            }
            // 其他位图模式：不处理，走层路径
        }

        // COLR 层枚举
        if (!glyphHasColorLayers(f, g.gid)) {
            // 无 COLR 层（如旗帜第二半等）：按单色轮廓走
            if (FT_Load_Glyph(f, g.gid, loadFlagsFor(size)) != 0) return false;
            if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;
            const FT_Bitmap& bm = f->glyph->bitmap;
            Layer L;
            L.x = f->glyph->bitmap_left + xAccum + g.xOffset;
            L.y = f->glyph->bitmap_top;
            L.w = static_cast<int>(bm.width);
            L.h = static_cast<int>(bm.rows);
            L.pitch = bm.pitch;
            L.px.assign(bm.buffer, bm.buffer + static_cast<size_t>(bm.rows) * bm.pitch);
            L.bgra = false;
            L.r = L.g = L.b = L.a = 255;
            if (L.w > 0 && L.h > 0) layers.push_back(L);
            xAccum += g.xAdvance;
            continue;
        }

        if (impl_->palFace != f) {
            impl_->palette = nullptr;
            if (FT_Palette_Select(f, 0, &impl_->palette) != 0) impl_->palette = nullptr;
            impl_->palFace = f;
        }

        FT_LayerIterator it = {};
        FT_UInt lg = 0, lpal = 0;
        int nLayers = 0;
        while (FT_Get_Color_Glyph_Layer(f, g.gid, &lg, &lpal, &it)) nLayers++;
        it.num_layers = 0; it.layer = 0; it.p = nullptr;
        for (int i = 0; i < nLayers && FT_Get_Color_Glyph_Layer(f, g.gid, &lg, &lpal, &it); i++) {
            if (FT_Load_Glyph(f, lg, loadFlagsFor(size)) != 0) continue;
            if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
            const FT_Bitmap& bm = f->glyph->bitmap;
            Layer L;
            L.x = f->glyph->bitmap_left + xAccum + g.xOffset;
            L.y = f->glyph->bitmap_top;
            L.w = static_cast<int>(bm.width);
            L.h = static_cast<int>(bm.rows);
            L.pitch = bm.pitch;
            L.px.assign(bm.buffer, bm.buffer + static_cast<size_t>(bm.rows) * bm.pitch);
            L.bgra = false;
            if (impl_->palette) {
                const FT_Color& c = impl_->palette[lpal];
                L.r = c.red; L.g = c.green; L.b = c.blue; L.a = c.alpha;
            } else { L.r = L.g = L.b = L.a = 255; }
            if (L.w > 0 && L.h > 0) layers.push_back(L);
        }
        xAccum += g.xAdvance;
    }
    if (layers.empty()) return false;

    // 合成 bounding box（y 向上为基线上方）
    int leftMin = 0x7FFFFFFF, rightMax = -0x7FFFFFFF;
    int topMax = -0x7FFFFFFF, bottomMin = 0x7FFFFFFF;
    for (const auto& L : layers) {
        leftMin = std::min(leftMin, L.x);
        rightMax = std::max(rightMax, L.x + L.w);
        topMax = std::max(topMax, L.y);
        bottomMin = std::min(bottomMin, L.y - L.h);
    }
    if (rightMax <= leftMin || topMax <= bottomMin) return false;
    int W = rightMax - leftMin;
    int H = topMax - bottomMin;

    impl_->colorBuf.assign(static_cast<size_t>(W) * H * 4, 0);
    uint8_t* dst = impl_->colorBuf.data();
    // COLR 层列表顺序 = 绘制顺序（顶→底）：合成时反转，底先画
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const Layer& L = *it;
        int ox = L.x - leftMin;
        int oy = (topMax - L.y);   // 缓冲 y（顶=0）
        if (L.a == 0) continue;
        for (int y = 0; y < L.h; y++) {
            const uint8_t* src = L.px.data() + static_cast<size_t>(y) * L.pitch;
            uint8_t* row = dst + static_cast<size_t>(oy + y) * W * 4 + static_cast<size_t>(ox) * 4;
            if (L.bgra) {
                // 内嵌 BGRA 位图：直通 alpha → 预乘 over
                for (int x = 0; x < L.w; x++) {
                    const uint8_t* s = src + static_cast<size_t>(x) * 4;
                    uint8_t sa = s[3];
                    if (sa == 0) continue;
                    uint8_t* p = row + static_cast<size_t>(x) * 4;
                    uint32_t sb = static_cast<uint32_t>(s[0]) * sa / 255;
                    uint32_t sg = static_cast<uint32_t>(s[1]) * sa / 255;
                    uint32_t sr = static_cast<uint32_t>(s[2]) * sa / 255;
                    uint32_t inv = 255 - p[3];
                    uint8_t na = static_cast<uint8_t>(sa + static_cast<uint32_t>(p[3]) * inv / 255);
                    p[0] = static_cast<uint8_t>(p[0] + sb * inv / 255);
                    p[1] = static_cast<uint8_t>(p[1] + sg * inv / 255);
                    p[2] = static_cast<uint8_t>(p[2] + sr * inv / 255);
                    p[3] = na;
                }
            } else {
                // 单色 alpha 层 + 层色：预乘 over
                for (int x = 0; x < L.w; x++) {
                    uint8_t sa = static_cast<uint8_t>((src[x] * L.a) / 255);
                    if (sa == 0) continue;
                    uint8_t* p = row + static_cast<size_t>(x) * 4;
                    uint32_t sb = static_cast<uint32_t>(L.b) * sa / 255;
                    uint32_t sg = static_cast<uint32_t>(L.g) * sa / 255;
                    uint32_t sr = static_cast<uint32_t>(L.r) * sa / 255;
                    uint32_t inv = 255 - p[3];
                    uint8_t na = static_cast<uint8_t>(sa + static_cast<uint32_t>(p[3]) * inv / 255);
                    p[0] = static_cast<uint8_t>(p[0] + sb * inv / 255);
                    p[1] = static_cast<uint8_t>(p[1] + sg * inv / 255);
                    p[2] = static_cast<uint8_t>(p[2] + sr * inv / 255);
                    p[3] = na;
                }
            }
        }
    }

    out.width = W;
    out.height = H;
    out.baseline = topMax;
    out.advance = advTotal;
    out.bgra = dst;
    return true;
}

std::vector<ShapedGlyph> FontEngine::shape(const std::string& face, float size,
                                           const uint32_t* cps, size_t n) {
    std::vector<ShapedGlyph> result;
    if (!cps || n == 0) return result;
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->hfont) return result;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf32(buf, cps, static_cast<int>(n), 0, static_cast<int>(n));
    hb_buffer_guess_segment_properties(buf);
    hb_shape(s->hfont, buf, nullptr, 0);

    unsigned int count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
    result.reserve(count);
    for (unsigned int i = 0; i < count; i++) {
        ShapedGlyph g;
        g.gid = info[i].codepoint;
        g.cluster = info[i].cluster;
        g.xAdvance = round26d6(pos ? pos[i].x_advance : 0);
        g.xOffset = round26d6(pos ? pos[i].x_offset : 0);
        result.push_back(g);
    }
    hb_buffer_destroy(buf);
    return result;
}

bool FontEngine::needsShaping(const uint32_t* cps, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (inComplexScript(cps[i])) return true;
    return false;
}

void registerSystemFonts(FontEngine& engine) {
#ifdef _WIN32
    const char* dir = "C:/Windows/Fonts/";
    char p[512];
    auto has = [&](const char* f) {
        snprintf(p, sizeof(p), "%s%s", dir, f);
        return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    };
    auto reg = [&](const char* name, const char* file) {
        snprintf(p, sizeof(p), "%s%s", dir, file);
        if (has(file)) engine.addFont(name, p);
    };
    reg("Segoe UI", "segoeui.ttf");
    reg("Segoe UI Emoji", "seguiemj.ttf");
    reg("Microsoft YaHei", "msyh.ttc");
    reg("Microsoft YaHei", "msyh.ttf");
    reg("SimHei", "simhei.ttf");
    reg("SimSun", "simsun.ttc");
    reg("Arial", "arial.ttf");
    if (engine.defaultFace().empty()) engine.setDefaultFace("Segoe UI");
#endif
}

} // namespace binui_font
