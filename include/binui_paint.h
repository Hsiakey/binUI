#pragma once
// ============================================================
// binUI 扩展绘制几何（v13 扩展绘制段的后端共用部分）
//
// 三个绘制后端（GDI 画布 / OpenGL / D3D11）能力差异很大，但底色渐变、
// 四边边框这两件事本质上是"一堆轴对齐矩形 + 一个颜色"，所以把几何算在
// 这里，各后端只负责把自己那套 fillRect / fillRounded 填上去 —— 保证
// 三端像素表现一致，且只有一份数学逻辑要维护。
//
// 用法（后端里都是这几行）：
//   PaintBand bands[kMaxPaintBands];
//   int n = BuildFillBands(paint, x, y, w, h, radius, color, bands, kMaxPaintBands);
//   for (int i = 0; i < n; i++)
//       if (bands[i].round) fillRounded(bands[i].x, bands[i].y, bands[i].w, bands[i].h,
//                                       bands[i].radius, bands[i].color);
//       else                fillRect(bands[i].x, bands[i].y, bands[i].w, bands[i].h,
//                                    bands[i].color);
//
// 说明：
//   · 渐变用横条/竖条近似（条数够多肉眼看不出分界），这样不需要着色器，
//     也不依赖后端的渐变画刷；半径 > 0 时按圆角收进每一行的左右内缩，
//     所以圆角渐变卡片也是对的。
//   · 对角渐变（135deg / to bottom right）按垂直渐变近似 —— 轴对齐条带
//     画不出真正的斜向过渡，写出来反而更假。
//   · 边框画在节点内侧，四边可不同宽；四角为方角（圆角+边框的圆环描边
//     需要路径填充，三个后端的接口都不统一，宁可简单一致）。
// ============================================================
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "binui_format.h"

namespace binui {

struct PaintBand {
    float    x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0;
    float    radius = 0;   // > 0 时后端应走 fillRounded
    bool     round = false;
};

constexpr int kMaxPaintBands = 128;

inline uint32_t PaintLerpArgb(uint32_t a, uint32_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    auto ch = [&](int shift) {
        const float ca = (float)((a >> shift) & 0xFF);
        const float cb = (float)((b >> shift) & 0xFF);
        return (uint32_t)std::lround(ca + (cb - ca) * t) & 0xFFu;
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// 圆角矩形在"距边 dy"处的水平内缩（dy 从边的中心算起，超过 radius 就没内缩）
//   dy = 0   → 内缩 = radius（圆角矩形最高处只剩中间那段）
//   dy >= r  → 0（进入直边）
// 注意 dy=0 时 (r-dy)² 正好等于 r²，别把这一支当"越界"丢掉 —— 那会让圆角变方角。
inline float PaintRoundInset(float dy, float radius) {
    if (radius <= 0.0f || dy >= radius) return 0.0f;
    const float d = radius - dy;          // 距圆心的竖直距离
    const float rr = radius * radius;
    const float dd = d * d;
    if (dd > rr) return 0.0f;
    return radius - std::sqrt(rr - dd);
}

// 底色：单色（1 条，可带圆角）或渐变（多条，逐行内缩近似圆角）
//
// 条带切分不是均分的：圆角区域每条 1px（否则圆角会变成三四大块阶梯 —— 肉眼一眼能看出），
// 中间直边区按 ~32px 一条均分（那里只有颜色在变，条带高点看不出来）。
inline int BuildFillBands(const BinUIPaintNode* p, float x, float y, float w, float h,
                          float radius, uint32_t color,
                          PaintBand* out, int cap = kMaxPaintBands) {
    if (w <= 0.0f || h <= 0.0f || cap <= 0) return 0;
    if (!p || !(p->flags & PAINT_GRADIENT)) {
        out[0] = { x, y, w, h, color, radius, radius > 0.0f };
        return 1;
    }

    const bool horizontal = (p->gradDir == BINUI_GRAD_H);
    const float span = horizontal ? w : h;
    if (span <= 0.0f) return 0;
    float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;
    if (2.0f * r > span) r = span * 0.5f;

    // 先把"沿渐变方向的切点"算出来（含 0 与 span），再逐段生成条带
    float edges[kMaxPaintBands + 2];
    int ne = 0;
    edges[ne++] = 0.0f;
    if (r > 0.5f) {
        const int rb = std::max(2, std::min((int)std::ceil(r), (cap - 2) / 2));  // 圆角区细分
        for (int i = 1; i <= rb; i++) edges[ne++] = r * (float)i / (float)rb;
        const float mid0 = r, mid1 = span - r;
        if (mid1 > mid0 + 0.5f) {
            const int room = std::max(1, cap - ne + 1);
            const int mb = std::max(1, std::min(room, (int)std::ceil((mid1 - mid0) / 32.0f)));
            for (int i = 1; i <= mb; i++)
                edges[ne++] = mid0 + (mid1 - mid0) * (float)i / (float)mb;
        }
        for (int i = 1; i <= rb; i++) {
            const float e = (span - r) + r * (float)i / (float)rb;
            if (e > edges[ne - 1] + 0.01f) edges[ne++] = e;
        }
    } else {
        const int n = std::min(std::max((int)std::ceil(span / 24.0f), 2), cap);
        for (int i = 1; i <= n; i++) edges[ne++] = span * (float)i / (float)n;
    }
    if (edges[ne - 1] < span - 0.01f) edges[ne++] = span;   // 防御：切点没走到末尾

    int count = 0;
    for (int i = 0; i + 1 < ne && count < cap; i++) {
        const float a = edges[i], b2 = edges[i + 1];
        if (b2 <= a + 0.001f) continue;
        const float t = (a + b2) * 0.5f / span;
        PaintBand& bd = out[count++];
        bd.color = PaintLerpArgb(p->gradFrom, p->gradTo, t);
        bd.round = false;
        bd.radius = 0;
        if (horizontal) {
            bd.x = x + a;
            bd.y = y;
            bd.w = (b2 - a) + 0.5f;    // 轻微重叠：取整后不露缝
            bd.h = h;
            if (r > 0.5f) {
                const float dl = PaintRoundInset(std::min(a, r), r);
                const float dr = PaintRoundInset(std::min(span - b2, r), r);
                const float in = std::max(dl, dr);
                bd.y = y + in;
                bd.h = h - 2.0f * in;
                if (bd.h <= 0.0f) bd.h = 0.5f;
            }
        } else {
            bd.x = x;
            bd.y = y + a;
            bd.w = w;
            bd.h = (b2 - a) + 0.5f;
            if (r > 0.5f) {
                const float dt = PaintRoundInset(std::min(a, r), r);
                const float db = PaintRoundInset(std::min(span - b2, r), r);
                const float in = std::max(dt, db);
                bd.x = x + in;
                bd.w = w - 2.0f * in;
                if (bd.w <= 0.0f) bd.w = 0.5f;
            }
        }
    }
    return count;
}

// 边框：直角 → 4 条矩形；圆角 → 沿圆弧逐行铺薄条的"环形边框"。
//
// 为什么不能拿 4 条轴对齐矩形凑圆角：矩形是方的，圆角按钮上会露出方框的角
// （截图里那种"白框套圆角"的脏边就是这么来的）。环形边框的做法：
//   · 圆心角区逐行（1px 一行）算外轮廓内缩 O 与内轮廓内缩 I，
//     左右各铺一条 [O, I] 的薄条 —— 切片的水平宽度天然随切线方向变宽，几何是对的
//   · 直边区 O、I 恒定，相邻行自动合并成一条高条（省 draw call）
//   · 上/下横条区整行铺（含圆角弧），四边颜色各自生效
// 只用到 fillRect，三个后端共用；合并后条数通常是个位数。
inline int BuildBorderBands(const BinUIPaintNode* p, float x, float y, float w, float h,
                            float radius, PaintBand* out, int cap = kMaxPaintBands) {
    if (!p || !(p->flags & PAINT_BORDER) || cap <= 0) return 0;
    if (w <= 0.0f || h <= 0.0f) return 0;
    const float lim = std::min(w, h) * 0.5f;
    const float bt = std::min((float)p->borderW[0], lim);
    const float br = std::min((float)p->borderW[1], lim);
    const float bb = std::min((float)p->borderW[2], lim);
    const float bl = std::min((float)p->borderW[3], lim);
    if (bt <= 0.0f && br <= 0.0f && bb <= 0.0f && bl <= 0.0f) return 0;

    int n = 0;
    const float r = (radius > 0.5f) ? std::min(radius, lim) : 0.0f;
    if (r <= 0.5f) {   // 直角：四条，角上不叠画（半透明时叠画会变深）
        if (bt > 0 && n < cap) out[n++] = { x + bl, y, std::max(0.0f, w - bl - br), bt,
                                            p->borderColor[0], 0, false };
        if (bb > 0 && n < cap) out[n++] = { x + bl, y + h - bb, std::max(0.0f, w - bl - br), bb,
                                            p->borderColor[2], 0, false };
        if (bl > 0 && n < cap) out[n++] = { x, y, bl, h, p->borderColor[3], 0, false };
        if (br > 0 && n < cap) out[n++] = { x + w - br, y, br, h, p->borderColor[1], 0, false };
        return n;
    }

    const float iy = y + bt;                                     // 内容盒（内轮廓）
    const float ih = std::max(0.0f, h - bt - bb);
    const float bwMax = std::max(std::max(bt, br), std::max(bb, bl));
    const float ir = std::max(0.0f, r - bwMax);                  // 内轮廓圆角

    auto push = [&](float bx, float by, float bwid, float bh, uint32_t color) {
        if (bwid <= 0.01f || bh <= 0.01f || n >= cap) return;
        if (n > 0) {                                             // 与上一段紧邻且同色同宽 → 合并
            PaintBand& pv = out[n - 1];
            if (!pv.round && pv.color == color &&
                std::fabs(pv.x - bx) < 0.02f && std::fabs(pv.w - bwid) < 0.02f &&
                std::fabs((pv.y + pv.h) - by) < 0.51f) {
                pv.h = by + bh - pv.y;
                return;
            }
        }
        out[n++] = { bx, by, bwid, bh, color, 0, false };
    };

    const int rows = (int)std::ceil(h);
    for (int i = 0; i < rows && n < cap; i++) {
        const float rt = y + (float)i;
        const float rb = std::min(rt + 1.0f, y + h);
        const float O = std::max(PaintRoundInset(rt - y, r), PaintRoundInset((y + h) - rb, r));
        if (rt < iy - 0.01f) {                                   // 上横条（含上圆角弧）
            push(x + O, rt, w - 2.0f * O, rb - rt, p->borderColor[0]);
        } else if (rt >= iy + ih - 0.01f) {                      // 下横条
            push(x + O, rt, w - 2.0f * O, rb - rt, p->borderColor[2]);
        } else {                                                 // 左右竖条（含左右圆角弧）
            const float k = std::max(PaintRoundInset(std::max(0.0f, rt - iy), ir),
                                     PaintRoundInset(std::max(0.0f, (iy + ih) - rb), ir));
            const float li = bl + k, ri = br + k;
            push(x + O, rt, li - O, rb - rt, p->borderColor[3]);
            push(x + w - ri, rt, ri - O, rb - rt, p->borderColor[1]);
        }
    }
    return n;
}

// CSS font-weight 归一化到后端认识的"常规/粗体"两档
inline int PaintWeightOf(const BinUIPaintNode* p) {
    return (p && (p->flags & PAINT_WEIGHT) && p->weight >= 6) ? 700 : 400;
}

// 行高：返回像素行距（无扩展绘制段或未声明时用 1.35 × 字号，与旧行为一致）
inline float PaintLineHeight(const BinUIPaintNode* p, float fontSize) {
    if (p && (p->flags & PAINT_LINE_HEIGHT) && p->lineHeight >= 100)
        return fontSize * (float)p->lineHeight / 100.0f;
    return fontSize * 1.35f;
}

// 文字装饰线条（下划线/删除线/上划线）：返回要画的条数，最多 3
inline int BuildTextRuleBands(const BinUIPaintNode* p, float x, float textTop, float w,
                              float fontSize, PaintBand* out, int cap = 3) {
    if (!p || cap <= 0) return 0;
    int n = 0;
    if (p->flags & PAINT_UNDERLINE) {
        const BinUITextRule r = BinUITextRuleOf(0, fontSize);
        if (n < cap) out[n++] = { x, textTop + r.off, w, r.thickness, p->textColor, 0, false };
    }
    if (p->flags & PAINT_STRIKE) {
        const BinUITextRule r = BinUITextRuleOf(1, fontSize);
        if (n < cap) out[n++] = { x, textTop + r.off, w, r.thickness, p->textColor, 0, false };
    }
    if (p->flags & PAINT_OVERLINE) {
        const BinUITextRule r = BinUITextRuleOf(2, fontSize);
        if (n < cap) out[n++] = { x, textTop + r.off, w, r.thickness, p->textColor, 0, false };
    }
    return n;
}

} // namespace binui
