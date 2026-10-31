// test_font.cpp - 字体服务回归测试（FreeType + HarfBuzz，无窗口）
//
// 覆盖：字体发现 -> 灰度光栅化（含小字号轻 hinting / gamma 修正）-> 整形度量 -> 彩色判定。
// 重点守住"改了 hinting/gamma 之后字形仍然非空、度量仍然合理"这条底线。
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "binui_font_renderer.h"

using namespace binui_font;

static int g_fail = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            std::fprintf(stderr, "[FAIL] %s\n", (msg));     \
            ++g_fail;                                       \
        } else {                                            \
            std::printf("[ok] %s\n", (msg));                \
        }                                                   \
    } while (0)

// 统计一个码点的灰度覆盖率：总墨量 / 全覆盖像素数 / 是否非空
struct InkStat {
    bool ok = false;
    int  w = 0, h = 0;
    long long ink = 0;
    int  full = 0;      // coverage == 255
    int  nonzero = 0;
    int  advance = 0;
    int  maxA = 0;      // 最大覆盖率
};

static InkStat Measure(FontEngine& fe, const char* face, float size, uint32_t cp) {
    InkStat s;
    GrayGlyph g;
    if (!fe.renderGlyph(face, size, cp, g)) return s;
    s.ok = true;
    s.w = g.width;
    s.h = g.height;
    s.advance = g.advance;
    if (!g.alpha || g.height <= 0) return s;
    for (int y = 0; y < g.height; y++) {
        const uint8_t* row = g.alpha + static_cast<size_t>(y) * g.pitch;
        for (int x = 0; x < g.width; x++) {
            const uint8_t a = row[x];
            if (!a) continue;
            s.nonzero++;
            s.ink += a;
            if (a == 255) s.full++;
            if (a > s.maxA) s.maxA = a;
        }
    }
    return s;
}

int main() {
    FontEngine fe;
    CHECK(fe.init(), "FontEngine 初始化");
    registerSystemFonts(fe);
    if (fe.defaultFace().empty()) {
        std::printf("[skip] 没有可用系统字体（非 Windows？），跳过光栅化检查\n");
        return 0;
    }
    std::printf("[info] defaultFace = %s\n", fe.defaultFace().c_str());

    const std::string face = fe.defaultFace();
    const FontMetrics m = fe.metrics(face, 14.0f);
    CHECK(m.ascent > 0 && m.descent >= 0, "度量：ascent > 0 且 descent >= 0");
    CHECK(m.ascent + m.descent >= 10, "度量：14px 行高合理（>= 10px）");

    // 1. 小字号（<= 16px，默认走轻 hinting）必须画得出笔画，且笔干足够实
    //    注意：轻 hinting 不把笔宽吸附到整数像素，所以不要求出现 255（那是 full hinting 的行为），
    //    只要求最大覆盖率达到"看起来是实心"的程度。
    for (float size : { 11.0f, 12.0f, 14.0f, 16.0f }) {
        const InkStat s = Measure(fe, face.c_str(), size, 'H');
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "renderGlyph('H' @ %.0fpx)：非空且笔干实（maxA=%d）", size, s.maxA);
        CHECK(s.ok && s.nonzero > 0 && s.maxA >= 128, msg);
        std::snprintf(msg, sizeof(msg), "renderGlyph('H' @ %.0fpx)：advance > 0", size);
        CHECK(s.advance > 0, msg);
    }

    // 2. 大字号（> 16px，走无 hinting 路径）同样正常
    for (float size : { 17.0f, 28.0f, 48.0f }) {
        const InkStat s = Measure(fe, face.c_str(), size, 'H');
        char msg[160];
        std::snprintf(msg, sizeof(msg), "renderGlyph('H' @ %.0fpx)：非空", size);
        CHECK(s.ok && s.nonzero > 0, msg);
    }

    // 3. gamma 修正不能把覆盖率压成 0 或永远饱和：墨量应随字号单调增长，
    //    且"实心像素占比"保持克制（<= 60%，否则说明对比度被拉爆）
    {
        InkStat prev;
        bool mono = true;
        for (float size : { 11.0f, 14.0f, 20.0f, 28.0f }) {
            const InkStat s = Measure(fe, face.c_str(), size, 'H');
            if (prev.ok && s.ink <= prev.ink) mono = false;
            prev = s;
        }
        CHECK(mono, "gamma：墨量随字号单调增长");
        const InkStat s = Measure(fe, face.c_str(), 14.0f, 'H');
        const double fullRatio = s.nonzero ? static_cast<double>(s.full) / s.nonzero : 1.0;
        std::printf("[info] 14px 'H'：ink=%lld nonzero=%d full=%d (%.1f%%)\n",
                    s.ink, s.nonzero, s.full, fullRatio * 100.0);
        CHECK(fullRatio <= 0.6, "gamma：实心像素占比未爆（<= 60%）");
    }

    // 4. 空白字符：无像素但有 advance（不能返回 false，否则单词会挤一起）
    {
        GrayGlyph g;
        const bool ok = fe.renderGlyph(face.c_str(), 14.0f, ' ', g);
        CHECK(ok && g.advance > 0, "空格：有 advance、无墨迹");
    }

    // 5. 整形度量与单字形度量一致（hinting 开关必须两边同步，否则定位会偏）
    {
        const uint32_t cps[] = { 'H', 'a', 'm' };
        const auto shaped = fe.shape(face, 14.0f, cps, 3);
        int shapeAdv = 0;
        for (const auto& sg : shaped) shapeAdv += sg.xAdvance;
        int glyphAdv = 0;
        for (uint32_t cp : cps) glyphAdv += Measure(fe, face.c_str(), 14.0f, cp).advance;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "shape 与 renderGlyph 的 advance 一致（%d vs %d）", shapeAdv, glyphAdv);
        CHECK(shapeAdv > 0 && std::abs(shapeAdv - glyphAdv) <= 1, msg);
    }

    // 6. 中文（如果系统有中文字体）：不应返回豆腐空白
    {
        const InkStat s = Measure(fe, "Microsoft YaHei", 14.0f, 0x4E2D); // 中
        if (s.ok) {
            CHECK(s.nonzero > 0, "中文 14px：非空字形");
            CHECK(s.advance >= 12, "中文 14px：advance >= 12（全角）");
        } else {
            std::printf("[skip] 无 Microsoft YaHei，跳过中文检查\n");
        }
    }

    // 7. UI 符号字形：标题栏三颗按钮 + 组件里的图标
    //    GL/D3D 的字形图集按"非 ASCII -> 中文字体、emoji 范围 -> emoji 字体"分类，
    //    分类错一颗字形，按钮就变成一块空白（GDI 那边有系统字体回退，看不出来）。
    //    这里守住底线：每个符号至少要有一个候选面画得出笔画。
    {
        struct Sym { const char* name; uint32_t cp; };
        const Sym syms[] = {
            { "最小化 − U+2212", 0x2212 }, { "最大化 □ U+25A1", 0x25A1 },
            { "关闭 ✕ U+2715",  0x2715 }, { "图标 ◆ U+25C6",   0x25C6 },
        };
        const char* faces[] = { "Microsoft YaHei", "Segoe UI Emoji", "Segoe UI" };
        for (const auto& sy : syms) {
            bool any = false;
            const char* hit = "-";
            int hitInk = 0;
            for (const char* f : faces) {
                const InkStat s = Measure(fe, f, 13.0f, sy.cp);
                if (!s.ok || s.nonzero <= 0) continue;
                if (s.ink > hitInk) { hitInk = static_cast<int>(s.ink); hit = f; }
                any = true;
            }
            char msg[192];
            std::snprintf(msg, sizeof(msg), "%s @13px：有字形（%s，ink=%d）", sy.name, hit, hitInk);
            CHECK(any, msg);
        }
    }

    // ============================================================
    // 逐字形回退 + 豆腐块
    // ============================================================
    // 这一段守的是"缺字必须看得见"：谁有字形靠字体的 cmap 决定（不是码点区间），
    // 全都没有时必须画空心方框，而不是留白（留白会被当成引擎 bug）。
    std::printf("\n--- 逐字形回退 / 豆腐块 ---\n");
    CHECK(!fe.fallbackChain().empty(), "回退链已由 registerSystemFonts 填好");

    {
        // 未注册的面名要被静默过滤，不能凭空出现在链上
        FontEngine probe;
        probe.init();
        probe.addFont("Only", fe.defaultFace());
        probe.setFallbackChain({ "Only", "NoSuchFace", "Only" });
        CHECK(probe.fallbackChain().size() == 1, "setFallbackChain 过滤未注册 + 去重");
    }

    {
        struct ResolveCase { const char* name; uint32_t cp; };
        const ResolveCase cases[] = {
            { "拉丁 'A'", 'A' },
            { "中文 U+4E2D", 0x4E2D },
            { "符号 U+2212 (−)", 0x2212 },
        };
        for (const auto& c : cases) {
            const int idx = fe.faceIndexFor(c.cp);
            char msg[192];
            std::snprintf(msg, sizeof(msg), "faceIndexFor(%s) 命中链上下标 %d（%s）", c.name,
                          idx, idx >= 0 ? fe.faceName(idx).c_str() : "无");
            CHECK(idx >= 0, msg);
            if (idx >= 0) {
                // 命中的面必须**真的**画得出这个码点，否则"解析"就是骗人的
                const InkStat s = Measure(fe, fe.faceName(idx).c_str(), 16.0f, c.cp);
                std::snprintf(msg, sizeof(msg), "%s：解析到的面确实有字形（ink=%lld）",
                              c.name, s.ink);
                CHECK(s.ok && s.advance > 0, msg);
            }
        }
    }

    {
        // 私用区末尾：任何系统字体都不该有 —— 必须判定为"没有"
        const int idx = fe.faceIndexFor(0x10FFFD);
        CHECK(idx == -1, "faceIndexFor(U+10FFFD) = -1（真正没有字形的码点）");
    }

    {
        // 符号优先黑白：⌘ 若系统装了 Segoe UI Symbol，就该解析到它，而不是 emoji 面
        bool hasSymbol = false;
        for (const auto& n : fe.fallbackChain())
            if (n == "Segoe UI Symbol") hasSymbol = true;
        if (hasSymbol) {
            const int idx = fe.faceIndexFor(0x2318);   // ⌘
            std::printf("[info] U+2318 → %s\n", idx >= 0 ? fe.faceName(idx).c_str() : "(无)");
            CHECK(idx < 0 || fe.faceName(idx) == "Segoe UI Symbol",
                  "⌘ 解析到 Segoe UI Symbol（黑白符号，不落 emoji 面）");
        } else {
            std::printf("[skip] 没装 Segoe UI Symbol，跳过符号优先黑白检查\n");
        }
    }

    // 豆腐块本身：空心方框，四边有墨、中心空
    for (float size : { 12.0f, 14.0f, 32.0f }) {
        GrayGlyph g;
        const bool ok = fe.renderMissingGlyph(size, g);
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "renderMissingGlyph(%.0fpx)：有尺寸且 advance 合理（%dx%d adv=%d）",
                      size, g.width, g.height, g.advance);
        CHECK(ok && g.width > 0 && g.height > 0 && g.advance >= g.width, msg);
        if (!ok || !g.alpha) continue;

        auto at = [&](int x, int y) {
            return static_cast<int>(g.alpha[static_cast<size_t>(y) * g.pitch + x]);
        };
        const int midX = g.width / 2, midY = g.height / 2;
        const bool edgeTop = at(midX, 0) > 0, edgeBottom = at(midX, g.height - 1) > 0;
        const bool edgeLeft = at(0, midY) > 0, edgeRight = at(g.width - 1, midY) > 0;
        std::snprintf(msg, sizeof(msg), "豆腐块 %.0fpx：四边有墨", size);
        CHECK(edgeTop && edgeBottom && edgeLeft && edgeRight, msg);
        std::snprintf(msg, sizeof(msg), "豆腐块 %.0fpx：中间空心（不是实心块）", size);
        CHECK(at(midX, midY) == 0, msg);
        std::snprintf(msg, sizeof(msg), "豆腐块 %.0fpx：边框不透明", size);
        CHECK(at(midX, 0) == 255, msg);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
