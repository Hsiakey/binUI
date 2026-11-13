// binui_flex_layout.h - flex 布局的唯一实现（header-only，无第三方依赖）
//
// 为什么单独抽出来：同一套 flex 算法现在有两个使用方 ——
//   · 编译器（src/compiler/binui_compiler.cpp）：生成节点树时的第一次布局；
//   · 渲染器（src/renderer/binui_renderer.cpp）：窗口 resize / relayoutSubtree 时按规格重排。
// 以前两边各写了一份（一份在 layoutFlex，一份在 relayoutFrom 的 flex 段），
// 加 flex-wrap / flex-shrink 这种新特性要改两处，而且很容易改出"编译期和运行期不一样"
// 的偏差。现在只有一个实现，调用方只负责把每个子项的输入凑齐。
//
// 语义按 CSS Flexbox 走，但有三处明确偏离（都为了不破坏既有布局，见 docs/ui_orchestration.md）：
//   1. align-items 默认 flex-start（CSS 是 stretch）；
//   2. 行容器里"未声明宽度且没写 flex"的子项按 flex:1 处理（binUI 便利行为）；
//   3. align-items/align-self:baseline 按 flex-start 处理（没有字体基线度量）。
#ifndef BINUI_FLEX_LAYOUT_H
#define BINUI_FLEX_LAYOUT_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace binui_flex {

// ---- 枚举码：与 BinUISpecFlex / BinUISpecNode 里的字节值一一对应（改这里就要改那边）----
enum Wrap : uint8_t {
    WRAP_NOWRAP  = 0,
    WRAP_WRAP    = 1,
    WRAP_REVERSE = 2,
};

enum Align : uint8_t {          // 交叉轴对齐（容器 align-items / 子项 align-self 共用）
    AL_START   = 0,
    AL_CENTER  = 1,
    AL_END     = 2,
    AL_STRETCH = 3,
    AL_AUTO    = 0xFF,          // 仅 alignSelf 用：跟随容器
};

enum AlignContent : uint8_t {   // 多行时行与行之间怎么分交叉轴剩余空间
    AC_START   = 0,
    AC_CENTER  = 1,
    AC_END     = 2,
    AC_BETWEEN = 3,
    AC_AROUND  = 4,
    AC_EVENLY  = 5,
    AC_STRETCH = 6,
};

enum Justify : uint8_t {        // 主轴剩余空间分配（与旧版 BinUISpecFlex::justify 编码兼容）
    J_START   = 0,
    J_END     = 1,
    J_CENTER  = 2,
    J_BETWEEN = 3,
    J_AROUND  = 4,
    J_EVENLY  = 5,
};

// 一个 flex 子项的输入。所有长度都是**内容盒像素**，百分比/内容估算由调用方先解析。
struct Item {
    float basis   = 0.0f;   // 主轴假设尺寸（flex-basis；未声明的主轴尺寸也折算到这里）
    float cross   = 0.0f;   // 交叉轴假设尺寸（未声明时由调用方给默认盒高/内容盒宽）
    float minMain = -1.0f, maxMain = -1.0f;   // -1 = 无约束
    float minCross = -1.0f, maxCross = -1.0f; // -1 = 无约束
    float grow    = 0.0f;   // 主轴剩余空间按权重分
    float shrink  = 1.0f;   // 主轴溢出时按 shrink×basis 加权收缩（CSS 默认 1）
    uint8_t alignSelf = AL_AUTO;
    uint8_t order     = 0;
    // 交叉轴拉伸：尺寸取"整行交叉轴长度"（align-self:stretch / align-items:stretch，
    // 以及 column 容器里未声明宽度的子项——后者是既有行为）。仍然受 min/max-cross 钳制。
    bool stretch = false;
};

struct Cfg {
    bool    row          = true;          // 主轴 = 宽
    uint8_t wrap         = WRAP_NOWRAP;
    uint8_t justify      = J_START;
    uint8_t align        = AL_START;
    uint8_t alignContent = AC_START;
    float   gap          = 0.0f;
    float   mainLen      = 0.0f;          // 主轴可用长度（内容盒）
    float   crossLen     = 0.0f;          // 交叉轴可用长度（内容盒）
    // 交叉轴尺寸是否"确定"（容器没写高度时的 auto 高是内容撑出来的，不确定）。
    // 单行容器这条为真时，这一行就是整个内容盒：交叉轴居中对齐按容器算，
    // 而不是按"这一行最高的子项"算 —— CSS 的 single-line flex container 就是这么定的。
    bool    crossDefinite = true;
};

// 输出：相对容器内容盒的矩形
struct Rect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };

inline float ClampSpan(float v, float mn, float mx) {
    if (mn >= 0.0f && v < mn) v = mn;
    if (mx >= 0.0f && v > mx) v = mx;
    return v;
}

// 跑一次 flex 布局。out 与 items 等长、同下标。
// 返回**内容盒需要的交叉轴长度**（容器没声明高度/宽度时用它撑开自己）。
inline float Layout(const Cfg& cfg, const Item* items, size_t n, Rect* out) {
    for (size_t i = 0; i < n; i++) out[i] = Rect();
    if (n == 0) return 0.0f;

    // ---- order：主轴顺序（同 order 保持文档顺序）----
    std::vector<size_t> seq(n);
    for (size_t i = 0; i < n; i++) seq[i] = i;
    std::stable_sort(seq.begin(), seq.end(), [&](size_t a, size_t b) {
        return items[a].order < items[b].order;
    });

    std::vector<float> main(n), cross(n);
    for (size_t i = 0; i < n; i++) {
        main[i] = ClampSpan(items[i].basis, items[i].minMain, items[i].maxMain);
        if (main[i] < 0.0f) main[i] = 0.0f;
        cross[i] = ClampSpan(items[i].cross, items[i].minCross, items[i].maxCross);
        if (cross[i] < 0.0f) cross[i] = 0.0f;
    }

    // ---- 分行（wrap=nowrap 恒一行；否则按假设主轴尺寸贪心装）----
    std::vector<size_t> ls, le;   // 每行在 seq 里的 [ls, le) 区间
    if (cfg.wrap == WRAP_NOWRAP) {
        ls.push_back(0);
        le.push_back(n);
    } else {
        size_t s = 0;
        float used = 0.0f;
        for (size_t k = 0; k < n; k++) {
            const float m = main[seq[k]];
            if (k > s && used + cfg.gap + m > cfg.mainLen + 0.01f) {
                ls.push_back(s); le.push_back(k);
                s = k;
                used = m;
            } else {
                used += (k > s ? cfg.gap : 0.0f) + m;
            }
        }
        ls.push_back(s); le.push_back(n);
    }
    const size_t lines = ls.size();

    std::vector<float> lineMain(lines, 0.0f), lineCross(lines, 0.0f);
    for (size_t li = 0; li < lines; li++) {
        const size_t s = ls[li], e = le[li];
        const size_t cnt = e - s;
        const float gaps = (cnt > 1 ? static_cast<float>(cnt - 1) : 0.0f) * cfg.gap;
        float used = gaps;
        for (size_t k = s; k < e; k++) used += main[seq[k]];

        const float free = cfg.mainLen - used;
        if (free > 0.0f) {
            // 主轴余量按 flex-grow 权重分
            float gsum = 0.0f;
            for (size_t k = s; k < e; k++) gsum += items[seq[k]].grow;
            if (gsum > 0.0f) {
                for (size_t k = s; k < e; k++)
                    main[seq[k]] += free * items[seq[k]].grow / gsum;
            }
        } else if (free < 0.0f) {
            // 溢出：按 CSS 的"缩放收缩因子"（flex-shrink × 基准尺寸）加权收缩。
            // 基准为 0 的弹性项不参与（它本来就是被分配出来的），否则会先被抽干。
            const float over = -free;
            float ssum = 0.0f;
            for (size_t k = s; k < e; k++)
                ssum += items[seq[k]].shrink * std::max(items[seq[k]].basis, 0.0f);
            if (ssum > 0.0f) {
                for (size_t k = s; k < e; k++) {
                    const float wgt = items[seq[k]].shrink * std::max(items[seq[k]].basis, 0.0f);
                    if (wgt <= 0.0f) continue;
                    main[seq[k]] -= over * wgt / ssum;
                    if (main[seq[k]] < 0.0f) main[seq[k]] = 0.0f;
                }
            }
        }
        // 分配完再按 min/max 钳制（CSS 也是"先伸缩后钳制"，允许最后溢出）
        for (size_t k = s; k < e; k++) {
            float m = ClampSpan(main[seq[k]], items[seq[k]].minMain, items[seq[k]].maxMain);
            if (m < 0.0f) m = 0.0f;
            main[seq[k]] = m;
            lineMain[li] += m;
            lineCross[li] = std::max(lineCross[li], cross[seq[k]]);
        }
        lineMain[li] += gaps;
    }

    // ---- 交叉轴：行总长 + align-content 分布 ----
    // contentCross 是"内容需要多少交叉轴"（容器 auto 高/宽时拿它撑开自己），
    // 必须在下面那条单行覆盖之前算出来。
    float contentCross = (lines > 1 ? static_cast<float>(lines - 1) : 0.0f) * cfg.gap;
    for (size_t li = 0; li < lines; li++) contentCross += lineCross[li];

    // 单行 + 容器交叉轴尺寸确定：这一行就是整个内容盒（CSS single-line 规则），
    // 否则 align-items:center 会按"本行最高的子项"居中，而不是按容器内高居中。
    if (lines == 1 && cfg.crossDefinite) lineCross[0] = cfg.crossLen;

    float crossTotal = (lines > 1 ? static_cast<float>(lines - 1) : 0.0f) * cfg.gap;
    for (size_t li = 0; li < lines; li++) crossTotal += lineCross[li];
    float crossFree = cfg.crossLen - crossTotal;
    if (crossFree < 0.0f) crossFree = 0.0f;

    float crossLead = 0.0f, crossGap = 0.0f;
    std::vector<float> lineAdd(lines, 0.0f);
    const float L = static_cast<float>(lines);
    switch (cfg.alignContent) {
        case AC_CENTER:  crossLead = crossFree * 0.5f; break;
        case AC_END:     crossLead = crossFree; break;
        case AC_BETWEEN: crossGap  = (lines > 1) ? crossFree / (L - 1.0f) : 0.0f; break;
        case AC_AROUND:  crossLead = crossFree / (2.0f * L);
                         crossGap  = crossFree / L; break;
        case AC_EVENLY:  crossLead = crossFree / (L + 1.0f);
                         crossGap  = crossLead; break;
        case AC_STRETCH: {
            const float add = crossFree / L;
            for (size_t li = 0; li < lines; li++) lineAdd[li] = add;
            break;
        }
        default: break;
    }

    // ---- 落位 ----
    float cy = crossLead;
    for (size_t j = 0; j < lines; j++) {
        // wrap-reverse：行顺序反向（第一行贴在交叉轴末端）
        const size_t li = (cfg.wrap == WRAP_REVERSE) ? (lines - 1 - j) : j;
        const size_t s = ls[li], e = le[li];
        const size_t cnt = e - s;
        const float lineCS = lineCross[li] + lineAdd[li];

        float free2 = cfg.mainLen - lineMain[li];
        if (free2 < 0.0f) free2 = 0.0f;
        float lead = 0.0f, gap2 = 0.0f;
        switch (cfg.justify) {
            case J_END:     lead = free2; break;
            case J_CENTER:  lead = free2 * 0.5f; break;
            case J_BETWEEN: gap2 = (cnt > 1) ? free2 / static_cast<float>(cnt - 1) : 0.0f; break;
            case J_AROUND:  lead = (cnt > 0) ? free2 / (2.0f * static_cast<float>(cnt)) : 0.0f;
                            gap2 = (cnt > 0) ? free2 / static_cast<float>(cnt) : 0.0f; break;
            case J_EVENLY:  lead = free2 / static_cast<float>(cnt + 1);
                            gap2 = lead; break;
            default: break;
        }

        float pos = lead;
        for (size_t k = s; k < e; k++) {
            const size_t idx = seq[k];
            const Item& it = items[idx];

            const uint8_t align = (it.alignSelf != AL_AUTO) ? it.alignSelf : cfg.align;
            float cr = cross[idx];
            if (it.stretch && align == AL_STRETCH) cr = std::max(cr, lineCS);
            cr = ClampSpan(cr, it.minCross, it.maxCross);
            if (cr < 0.0f) cr = 0.0f;

            float off = 0.0f;
            if (align == AL_CENTER) off = (lineCS - cr) * 0.5f;
            else if (align == AL_END) off = lineCS - cr;

            if (cfg.row) {
                out[idx].x = pos;
                out[idx].y = cy + off;
                out[idx].w = main[idx];
                out[idx].h = cr;
            } else {
                out[idx].x = cy + off;
                out[idx].y = pos;
                out[idx].w = cr;
                out[idx].h = main[idx];
            }
            pos += main[idx] + cfg.gap + gap2;
        }
        cy += lineCS + cfg.gap + crossGap;
    }

    return contentCross;
}

} // namespace binui_flex

#endif // BINUI_FLEX_LAYOUT_H
