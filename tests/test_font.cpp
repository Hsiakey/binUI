// test_font.cpp - 字体与输入法回归测试（FreeType + HarfBuzz，无窗口）
//
// 覆盖：
//   1. 字体发现 -> 灰度光栅化（含小字号轻 hinting / gamma 修正）-> 整形度量 -> 彩色判定
//      重点守住"改了 hinting/gamma 之后字形仍然非空、度量仍然合理"这条底线。
//   2. 输入法（IME）桥接：Win32 侧组字串与光标索引都是 UTF-16，引擎内部是 UTF-8 字节
//      偏移，`setCaretByUtf16Index` / `insertText` / `blur` 就是后端要走的整条链。
//      代理对（emoji）与全角字必须落在字符边界上，否则组字窗会飘、候选框会算错位置。
//      开窗才能测的部分（组字窗/候选框定位、组字期间点击被 IME 吞掉）见
//      examples/fontshow 的 ⑤ 输入法小节。
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_font_renderer.h"
#include "binui_renderer.h"

using namespace binui_font;
using namespace binui;          // 输入法桥接用例要用渲染器的编辑会话 API

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

// 编译一段只有一个输入框的界面，返回该输入框的节点 id（找不到 -1）
static int LoadInput(BinUIRenderer& r, const char* value) {
    std::string html = "<html width=\"360\" height=\"120\"><body><input id=\"box\" style=\""
                       "position:absolute;left:16px;top:16px;width:240px;height:32px;"
                       "font-size:16px\" value=\"" +
                       std::string(value) + "\"/></body></html>";
    auto res = CompileBinUIEx(html);
    if (res.layout.empty() || !r.loadLayout(res.layout.data(), res.layout.size())) return -1;
    for (const auto& n : r.getNodes())
        if (n.type == BINUI_INPUT) return static_cast<int>(n.id);
    return -1;
}

// 输入法桥接：UTF-16 索引 -> UTF-8 字节偏移 -> 编辑会话
static void TestImeBridge() {
    // ---- 1. 全角字（BMP，1 个 UTF-16 单元 = 3 字节）----
    {
        BinUIRenderer r;
        const int id = LoadInput(r, "ab你好");
        CHECK(id >= 0, "IME: 输入框布局加载");
        if (id < 0) return;

        r.focus(static_cast<uint16_t>(id));
        CHECK(r.isEditing(), "IME: focus 后进入编辑态");
        CHECK(r.edit().value == "ab你好", "IME: 初值来自 value 属性");
        CHECK(r.edit().caret == 8, "IME: 初始光标在末尾（2 + 3 + 3 字节）");

        r.setCaretByUtf16Index(2);   // GCS_CURSORPOS 语义：'a','b' 之后
        CHECK(r.edit().caret == 2, "IME: UTF-16 索引 2 -> 字节 2");
        r.setCaretByUtf16Index(3);   // '你' 之后
        CHECK(r.edit().caret == 5, "IME: UTF-16 索引 3 -> 字节 5（全角 3 字节）");
        r.setCaretByUtf16Index(4);
        CHECK(r.edit().caret == 8, "IME: UTF-16 索引 4 -> 字节 8（末尾）");
        r.setCaretByUtf16Index(999);
        CHECK(r.edit().caret == 8, "IME: 索引越界钳到末尾");

        // 上屏：GCS_RESULTSTR 转 UTF-8 后 insertText，光标停在提交串之后
        r.setCaretByUtf16Index(2);
        CHECK(r.insertText("测试"), "IME: 组字结果上屏");
        CHECK(r.edit().value == "ab测试你好", "IME: 上屏插入位置正确");
        CHECK(r.edit().caret == 8, "IME: 光标跟在提交串之后（2 + 6）");

        // 失去焦点时提交到节点文本（后端 blur 前会先拿 edit().value）
        r.blur();
        bool committed = false;
        for (const auto& n : r.getNodes())
            if (n.id == id && n.text == "ab测试你好") committed = true;
        CHECK(committed, "IME: blur 把编辑中的文本提交到节点");
    }

    // ---- 2. 代理对（emoji）：1 个 emoji = 2 个 UTF-16 单元 / 4 字节 ----
    {
        BinUIRenderer r;
        const int id = LoadInput(r, "x😀y");
        CHECK(id >= 0, "IME: emoji 输入框布局加载");
        if (id < 0) return;
        r.focus(static_cast<uint16_t>(id));
        CHECK(r.edit().value == "x😀y", "IME: emoji 初值");
        CHECK(r.edit().caret == 6, "IME: emoji 末尾 = 1 + 4 + 1 字节");

        r.setCaretByUtf16Index(1);
        CHECK(r.edit().caret == 1, "IME: 索引 1 -> emoji 起点（字节 1）");
        r.setCaretByUtf16Index(2);   // 代理对中间：绝不能停在半个 emoji 里
        CHECK(r.edit().caret == 1, "IME: 索引 2 落在代理对中间 -> 夹到 emoji 起点");
        r.setCaretByUtf16Index(3);   // emoji 之后（2 个单元）
        CHECK(r.edit().caret == 5, "IME: 索引 3 -> emoji 之后（字节 5）");
        r.setCaretByUtf16Index(4);
        CHECK(r.edit().caret == 6, "IME: 索引 4 -> 末尾（字节 6）");

        // 在 emoji 与 'y' 之间上屏一个全角字
        r.setCaretByUtf16Index(3);
        CHECK(r.insertText("字"), "IME: emoji 后上屏");
        CHECK(r.edit().value == "x😀字y", "IME: emoji 后插入不切坏代理对");
    }

    // ---- 3. 光标的字符边界（setCaret 传字节偏移时也要夹）----
    {
        BinUIRenderer r;
        const int id = LoadInput(r, "x😀y");
        if (id < 0) return;
        r.focus(static_cast<uint16_t>(id));
        r.setCaret(3);   // emoji 中间（字节 1..4）
        CHECK(r.edit().caret == 1, "IME: setCaret 落在字符中间 -> 夹到字符起点");
        r.setCaret(100);
        CHECK(r.edit().caret == 6, "IME: setCaret 越界 -> 末尾");
    }
}

int main() {
    // 输入法桥接（UTF-16 -> UTF-8 光标）不依赖字体，先跑：
    // 非 Windows / 无系统字体的环境下字体检查会 skip，但这段必须一直守。
    TestImeBridge();

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
