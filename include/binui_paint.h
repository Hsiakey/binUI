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
//   · 边框画在节点内侧，四边可不同宽；圆角盒子的边框按行贴着外圆弧走
//     （1px 步进），直边段合并成一整条 —— 条带数只跟半径有关，与高度无关。
//     半径大到装不下时退回直角四条（宁可方角，也不要缺边）。
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
// 边框条带上限：圆角边框按行贴弧，条带数 ≈ 4r+常数（与盒子高度无关），
// 半径大的胶囊（比如 100px 高的按钮 r=50）也要装得下，所以比填充宽。
constexpr int kMaxBorderBands = 512;

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
//   · 直边区 O、I 恒定，逐行延伸成一条高条（省 draw call）
//   · 上/下横条区整行铺（含圆角弧），四边颜色各自生效
// 只用到 fillRect，三个后端共用；合并后条数只跟半径有关，与盒子高度无关。
// （合并要按"这条边上一次的条带"记下标 —— 只看上一条会因左右交替而永远合不上，
//  高盒子会被 cap 截断，见下方 pushRun 的注释。）
inline int BuildBorderBands(const BinUIPaintNode* p, float x, float y, float w, float h,
                            float radius, PaintBand* out, int cap = kMaxBorderBands) {
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

    // 条带数只跟半径有关（≈4r+常数），与盒子高度无关。半径大到装不下时退回直角
    // 四条 —— 宁可方角，也不要"边框画到一半就没了"。
    if (4.0f * std::ceil(r) + 16.0f > static_cast<float>(cap)) {
        const float sh = std::max(0.0f, h - bt - bb);
        if (bt > 0 && n < cap) out[n++] = { x, y, w, bt, p->borderColor[0], 0, false };
        if (bb > 0 && n < cap) out[n++] = { x, y + h - bb, w, bb, p->borderColor[2], 0, false };
        if (bl > 0 && sh > 0 && n < cap) out[n++] = { x, y + bt, bl, sh, p->borderColor[3], 0, false };
        if (br > 0 && sh > 0 && n < cap) out[n++] = { x + w - br, y + bt, br, sh, p->borderColor[1], 0, false };
        return n;
    }

    // 四条边各自记住"上一次那条"的下标，逐行往后延伸。
    //
    // **不能只看 out[n-1]**：左右竖条是交替 push 的，上一个永远是另一条边，于是
    // 每行都新开一条，条带数变成 O(高度) —— 盒子一高就撞 cap 被截断。圆角 14 的
    // 卡片实测边框只画到盒子顶部约 52 行处，下面整段没有边框（三个后端都中招）。
    int lastTop = -1, lastBottom = -1, lastLeft = -1, lastRight = -1;
    auto pushRun = [&](int& last, float bx, float by, float bwid, float bh, uint32_t color) {
        if (bwid <= 0.01f || bh <= 0.01f) return;
        if (last >= 0) {                                         // 与这条边的上一段紧邻且同色同宽 → 延伸
            PaintBand& pv = out[last];
            if (pv.color == color && std::fabs(pv.x - bx) < 0.02f &&
                std::fabs(pv.w - bwid) < 0.02f && std::fabs((pv.y + pv.h) - by) < 0.02f) {
                pv.h = by + bh - pv.y;
                return;
            }
        }
        if (n >= cap) return;
        last = n;
        out[n++] = { bx, by, bwid, bh, color, 0, false };
    };

    const int rows = (int)std::ceil(h);
    for (int i = 0; i < rows; i++) {
        const float rt = y + (float)i;
        const float rb = std::min(rt + 1.0f, y + h);
        const float O = std::max(PaintRoundInset(rt - y, r), PaintRoundInset((y + h) - rb, r));
        if (rt < iy - 0.01f) {                                   // 上横条（含上圆角弧）
            pushRun(lastTop, x + O, rt, w - 2.0f * O, rb - rt, p->borderColor[0]);
        } else if (rt >= iy + ih - 0.01f) {                      // 下横条
            pushRun(lastBottom, x + O, rt, w - 2.0f * O, rb - rt, p->borderColor[2]);
        } else {                                                 // 左右竖条（含左右圆角弧）
            const float k = std::max(PaintRoundInset(std::max(0.0f, rt - iy), ir),
                                     PaintRoundInset(std::max(0.0f, (iy + ih) - rb), ir));
            const float li = bl + k, ri = br + k;
            pushRun(lastLeft, x + O, rt, li - O, rb - rt, p->borderColor[3]);
            pushRun(lastRight, x + w - ri, rt, ri - O, rb - rt, p->borderColor[1]);
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

// 容器（BINUI_LIST）文字色的兜底：按节点自身底色的亮度选深/浅。
// 作者显式给了 PAINT_TEXT_COLOR 时不用它。以前一律白字 —— 浅底容器里
// （尤其是运行期 addNode 出来的列表行）文字直接看不见。
// 底色近乎透明时无从判断，沿用白字（旧行为）。
inline uint32_t PaintContrastText(uint32_t bg) {
    if (((bg >> 24) & 0xFFu) < 0x80u) return 0xFFFFFFFFu;
    const float lum = (0.2126f * static_cast<float>((bg >> 16) & 0xFFu) +
                       0.7152f * static_cast<float>((bg >> 8) & 0xFFu) +
                       0.0722f * static_cast<float>(bg & 0xFFu)) / 255.0f;
    return lum > 0.5f ? 0xFF1A1A1Au : 0xFFFFFFFFu;
}

// ============================================================
// 圆角矩形 → 九宫格四边形（GL / D3D11 共用）
//
// 旧做法把"角片边长"写死成 2r+1（模板 4r×4r，形状内缩 1px），角片比圆弧本身
// 需要的 r 大一倍多。后果是 2×角片很快超过盒子短边：两个角片互相重叠（四边中部
// 露凹口），中段宽高转负（整块被角片自带的弧裁掉）—— 半径一到短边一半就退化成
// 直角，胶囊按钮、border-radius:999 的圆形头像、<svg><circle> 全部命中。
//
// 这里改成"角片边长 = r"：四分之一圆弧的包围盒恰好是 r×r，多一个像素都是浪费。
// 同时模板里的形状铺满 [0,t]（不再内缩 1px），于是圆弧紧贴模板边缘、角片尺寸
// 与弧长严格对应。半径钳到 min(w,h)/2 后，
//   · 2r < min(w,h)：九块无缝铺满，无重叠；
//   · 2r == min(w,h)：中段宽高为 0，四个 1/4 圆正好拼成胶囊/正圆。
//
// 模板边长取 2r+4 而不是 4r：角片只采样 [0,r]，多出来的部分只用于给中段取一个
// 直边像素，4r 纯属浪费显存（r=200 时 800×800×4 = 2.5MB → 404×404×4 = 653KB）。
// ============================================================
struct PaintQuad {
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

// 圆角模板边长：2r + 4（末尾 4px 是直边段，供中段取一个实心像素）
inline int RoundTemplateSize(int radius) {
    const int r = std::max(1, radius);
    return std::max(8, r * 2 + 4);
}

// 把圆角矩形拆成九个轴对齐块（四角 + 四边中段 + 中心）。返回块数；
// 0 = 半径无效，调用方应退回 fillRect。t 必须是 RoundTemplateSize(radius)。
inline int BuildRoundedNineSlice(float x, float y, float w, float h, float radius, int t,
                                 PaintQuad* out, int cap = 9) {
    if (!out || cap < 9 || w <= 0.0f || h <= 0.0f) return 0;
    const float r = std::min(radius, std::min(w, h) * 0.5f);
    if (r <= 0.0f) return 0;
    const float T = static_cast<float>(std::max(t, 1));
    const float c = r;                 // 角片边长 = 四分之一圆弧的包围盒
    const float cu = c / T;
    // 中段取模板正中 1 个像素：t>=2r+4 时它一定落在直边段里（不会把弧采样进来）
    const float s0 = 0.5f;
    const float s1 = 0.5f + 1.0f / T;

    const float mx = w - 2.0f * c, my = h - 2.0f * c;
    int n = 0;
    auto push = [&](float qx, float qy, float qw, float qh,
                    float u0, float v0, float u1, float v1) {
        if (qw <= 0.0f || qh <= 0.0f || n >= cap) return;   // 中段为 0 时正好跳过
        out[n++] = { qx, qy, qw, qh, u0, v0, u1, v1 };
    };
    push(x, y, c, c, 0, 0, cu, cu);                               // 左上
    push(x + w - c, y, c, c, 1 - cu, 0, 1, cu);                   // 右上
    push(x, y + h - c, c, c, 0, 1 - cu, cu, 1);                   // 左下
    push(x + w - c, y + h - c, c, c, 1 - cu, 1 - cu, 1, 1);       // 右下
    push(x + c, y, mx, c, s0, 0, s1, cu);                         // 上
    push(x + c, y + h - c, mx, c, s0, 1 - cu, s1, 1);             // 下
    push(x, y + c, c, my, 0, s0, cu, s1);                         // 左
    push(x + w - c, y + c, c, my, 1 - cu, s0, 1, s1);             // 右
    push(x + c, y + c, mx, my, s0, s0, s1, s1);                   // 中
    return n;
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

// ============================================================
// 焦点环虚线（"待编辑"态）：沿圆角矩形路径按 实线/空白 交替取线段
//
// 三个后端都只有 fillLine / fillRect / strokeRounded，"画虚线"得自己采样。
// 这里沿路径（上直边 → 右上角 → 右直边 → … → 左上角）按弧长走，每个实线段输出
// 一条线（两端点）；直边上是水平/竖直线，圆角段上是弦（dash 6px 时弦高误差 < 1px，
// 1px 细线上看不出来）。后端拿 PaintSegment 逐个 fillLine 即可，三端像素一致。
//
// 为什么用虚线而不是"细一点":待编辑 vs 编辑中是**两种状态**，线型 + 颜色双编码
// （虚线深色 = 待编辑，实线蓝色 = 编辑中）比只换颜色更容易分辨，色弱用户也能看出来。
// ============================================================
struct PaintSegment {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

constexpr int kMaxFocusDashSegments = 256;

// x/y/w/h 为焦点环内缩后的矩形，radius 为圆角半径（0 = 直角）。
// dash/gap 是实线段长与间隔（像素）；返回线段数。
//
// 段数上限是"保险丝"，不是"画到一半就停"：`flex:1` 拉满窗口的输入框周长能到两三千
// 像素，固定 6px 实线 / 4px 间隔要画一两百段，上限太小就会只剩上半圈（真踩过：
// flexshow 800 多像素宽的搜索框只画出上边和右边）。所以超限时按周长把实线和间隔
// 等比拉长，整圈一定画完；正常尺寸下虚线还是 6/4。
inline int BuildDashedRoundRing(float x, float y, float w, float h, float radius,
                                float dash, float gap,
                                PaintSegment* out, int cap = kMaxFocusDashSegments) {
    if (!out || cap <= 0 || w <= 0.0f || h <= 0.0f) return 0;
    const float r = std::max(0.0f, std::min(radius, std::min(w, h) * 0.5f));
    if (dash < 1.0f) dash = 1.0f;
    if (gap < 1.0f) gap = 1.0f;

    const float lw = std::max(0.0f, w - 2.0f * r);   // 直边长度
    const float lh = std::max(0.0f, h - 2.0f * r);
    const float arc = 3.14159265358979323846f * r * 0.5f;
    const float total = 2.0f * lw + 2.0f * lh + 4.0f * arc;

    // 路径上弧长 s 处的点。顺序：上边（从左到右）→ 右上角 → 右边 → 右下角 →
    // 下边（从右到左）→ 左下角 → 左边（从下到上）→ 左上角。
    auto pointAt = [&](float s, float& px, float& py) {
        float a = s;
        // 上边
        if (a <= lw) { px = x + r + a; py = y; return; }
        a -= lw;
        // 右上角：-90° → 0°
        if (a <= arc) {
            const float t = -1.57079632679489661923f + (arc > 0 ? a / r : 0.0f);
            px = x + w - r + r * std::cos(t);
            py = y + r + r * std::sin(t);
            return;
        }
        a -= arc;
        // 右边
        if (a <= lh) { px = x + w; py = y + r + a; return; }
        a -= lh;
        // 右下角：0° → 90°
        if (a <= arc) {
            const float t = (arc > 0 ? a / r : 0.0f);
            px = x + w - r + r * std::cos(t);
            py = y + h - r + r * std::sin(t);
            return;
        }
        a -= arc;
        // 下边（从右到左）
        if (a <= lw) { px = x + w - r - a; py = y + h; return; }
        a -= lw;
        // 左下角：90° → 180°
        if (a <= arc) {
            const float t = 1.57079632679489661923f + (arc > 0 ? a / r : 0.0f);
            px = x + r + r * std::cos(t);
            py = y + h - r + r * std::sin(t);
            return;
        }
        a -= arc;
        // 左边（从下到上）
        if (a <= lh) { px = x; py = y + h - r - a; return; }
        a -= lh;
        // 左上角：180° → 270°
        const float t = 3.14159265358979323846f + (arc > 0 ? std::min(a, arc) / r : 0.0f);
        px = x + r + r * std::cos(t);
        py = y + r + r * std::sin(t);
    };

    int n = 0;
    // 段数超上限：等比拉长实线/间隔，保证整圈画完（见函数头注释）
    const float step = std::max(dash + gap, total / static_cast<float>(cap));
    const float dashLen = step * dash / (dash + gap);
    for (float s = 0.0f; s < total && n < cap; s += step) {
        float s1 = s + dashLen;
        if (s1 > total) s1 = total;
        float x0, y0, x1, y1;
        pointAt(s, x0, y0);
        pointAt(s1, x1, y1);
        if (x0 != x1 || y0 != y1) out[n++] = { x0, y0, x1, y1 };
    }
    return n;
}

// ============================================================
// 路径填充（SVG <path>/<ellipse>）：折线 → 逐行扫描线 → 轴对齐条带
//
// 为什么只能这么做：三个后端都没有"填充任意多边形"的原语（GL/D3D 没有
// stencil 与三角化接口，GDI 只有硬编码的三角形调用）。共用一个扫描线求交
// 器，各端只负责 fillRect，三端像素才必然一致 —— 与上面的 BuildFillBands
// 是同一个分工。
//
// 两个实现细节：
//   · 相邻同宽同色的行要合并（pushRun 那套）。一个 24px 图标逐行画是 24 条，
//     合并后通常只剩"两侧弧边几十条 + 实心区一条"，差一个数量级。
//   · 撞条带上限时**按行步长降级**（1 → 2 → 4 → …），保证画完而不是画一半；
//     调用方从返回值/truncated 判断要不要重试。
// ============================================================
constexpr int kMaxPathBands = 2048;      // 单条路径的条带上限
constexpr int kMaxPathCrossings = 1024;  // 单条扫描线的交点上限
constexpr int kMaxRowSpans = 128;        // 单行条带上限
constexpr int kMaxOpenRuns = 96;         // 跨行合并的"正在延伸的条带"表

// 设备坐标下的折线路径（点按子路径首尾相接；指向调用方内存，不拷）
struct PaintPath {
    const float*    pts = nullptr;      // x,y 交替
    const uint32_t* first = nullptr;    // 子路径起始点下标
    const uint32_t* count = nullptr;    // 子路径点数（≥2）
    const uint8_t*  closed = nullptr;   // 1 = 原始 Z（描边时连接首尾）
    int contours = 0;
    bool Valid() const { return pts && first && count && contours > 0; }
};

// 扫描线填充。clip 为设备像素裁剪盒（通常是节点包围盒）。
// color 写进每个条带；rowStep>1 是降级模式（行更粗、条带更少）。
// truncated = true 表示撞上 cap / 交点上限，调用方应放大 rowStep 重试。
inline int BuildPathFillBands(const PaintPath& path, bool evenOdd,
                              float clipX0, float clipY0, float clipX1, float clipY1,
                              uint32_t color, PaintBand* out, int cap,
                              int rowStep = 1, bool* truncated = nullptr) {
    if (truncated) *truncated = false;
    if (!path.Valid() || !out || cap <= 0) return 0;
    if (rowStep < 1) rowStep = 1;
    if (clipX1 <= clipX0 || clipY1 <= clipY0) return 0;

    struct Crossing { float x; int dir; };
    Crossing crossings[kMaxPathCrossings];
    struct RowSpan { int x0, x1; };
    RowSpan spans[kMaxRowSpans];
    // 跨行合并：记下"上一行还在延伸"的条带
    struct OpenRun { int x0, w, endY, idx; };
    OpenRun open[kMaxOpenRuns];
    int openN = 0;

    int n = 0;
    const int yBegin = (int)std::floor(clipY0);
    const int yEnd = (int)std::ceil(clipY1);
    auto flushStale = [&](int y) {
        int w = 0;
        for (int i = 0; i < openN; i++)
            if (open[i].endY > y) open[w++] = open[i];
        openN = w;
    };

    for (int y = yBegin; y < yEnd; y += rowStep) {
        const float yc = (float)y + (float)rowStep * 0.5f;
        int nc = 0;
        bool overflow = false;
        for (int c = 0; c < path.contours && !overflow; c++) {
            const uint32_t first = path.first[c];
            const uint32_t cnt = path.count[c];
            if (cnt < 2) continue;
            for (uint32_t i = 0; i < cnt; i++) {
                const uint32_t ia = first + i;
                const uint32_t ib = first + (i + 1) % cnt;
                const float ax = path.pts[ia * 2], ay = path.pts[ia * 2 + 1];
                const float bx = path.pts[ib * 2], by = path.pts[ib * 2 + 1];
                if (ay == by) continue;                       // 水平边不产生交点
                if ((ay <= yc) == (by <= yc)) continue;       // 半开区间，顶点不重复计数
                if (nc >= kMaxPathCrossings) { overflow = true; break; }
                const float t = (yc - ay) / (by - ay);
                crossings[nc++] = { ax + t * (bx - ax), ay < by ? 1 : -1 };
            }
        }
        if (overflow) {                                   // 交点太多：降级重来
            if (truncated) *truncated = true;
            return n;
        }
        if (nc < 2) { flushStale(y); continue; }
        std::sort(crossings, crossings + nc,
                  [](const Crossing& a, const Crossing& b) { return a.x < b.x; });

        int ns = 0;
        if (evenOdd) {
            for (int i = 0; i + 1 < nc; i += 2) {
                if (ns >= kMaxRowSpans) { if (truncated) *truncated = true; break; }
                spans[ns++] = { (int)std::lround(crossings[i].x),
                                (int)std::lround(crossings[i + 1].x) };
            }
        } else {
            int winding = 0;
            float startX = 0.0f;
            for (int i = 0; i < nc; i++) {
                const int prev = winding;
                winding += crossings[i].dir;
                if (prev == 0 && winding != 0) {
                    startX = crossings[i].x;
                } else if (prev != 0 && winding == 0) {
                    if (ns >= kMaxRowSpans) { if (truncated) *truncated = true; break; }
                    spans[ns++] = { (int)std::lround(startX), (int)std::lround(crossings[i].x) };
                }
            }
        }

        const int xLo = (int)std::floor(clipX0);
        const int xHi = (int)std::ceil(clipX1);
        for (int i = 0; i < ns; i++) {
            int x0 = std::max(spans[i].x0, xLo);
            int x1 = std::min(spans[i].x1, xHi);
            const int w = x1 - x0;
            if (w <= 0) continue;                         // 亚像素条带丢掉（不做抗锯齿）
            // 与上一行的条带对齐 → 直接加高，省一个 draw call
            bool merged = false;
            for (int k = 0; k < openN; k++) {
                if (open[k].x0 == x0 && open[k].w == w && open[k].endY == y) {
                    out[open[k].idx].h += (float)rowStep;
                    open[k].endY = y + rowStep;
                    merged = true;
                    break;
                }
            }
            if (merged) continue;
            if (n >= cap) {
                if (truncated) *truncated = true;
                return n;
            }
            out[n] = { (float)x0, (float)y, (float)w, (float)rowStep, color, 0.0f, false };
            if (openN < kMaxOpenRuns) {
                open[openN++] = { x0, w, y + rowStep, n };
            } else {
                // 合并表满了：只丢合并机会，像素仍然正确 —— 找一条最老的顶掉
                int oldest = 0;
                for (int k = 1; k < openN; k++)
                    if (open[k].endY < open[oldest].endY) oldest = k;
                open[oldest] = { x0, w, y + rowStep, n };
            }
            n++;
        }
        flushStale(y);
    }
    return n;
}

// 折线描边：逐段画线，转角超过阈值时补一个圆点（圆角连接）。
// 三个后端的 fillLine / fillRounded 签名一致，所以这里用回调模板，
// 后端只传"怎么画一根线 / 一个圆"，几何只写一份。
//
// 为什么只在转角补点：折线是细分出来的，绝大多数相邻段几乎共线，
// 每段都补圆会把 draw call 翻倍；15° 以下的缺口在 1–2px 线宽上看不见。
inline float PathJoinCosThreshold() { return 0.966f; }   // cos(15°)

// 两段方向夹角是否需要补圆点（补 = 转角超过 15°）
inline bool PathNeedJoin(float ax, float ay, float bx, float by, float cx, float cy) {
    const float ux = bx - ax, uy = by - ay;
    const float vx = cx - bx, vy = cy - by;
    const float lu = std::sqrt(ux * ux + uy * uy);
    const float lv = std::sqrt(vx * vx + vy * vy);
    if (lu <= 1e-6f || lv <= 1e-6f) return false;
    const float cs = (ux * vx + uy * vy) / (lu * lv);
    return cs < PathJoinCosThreshold();
}

// line(x0,y0,x1,y1) → 后端画一根 lineWidth 粗的线
// dot(x,y,w,h,r)    → 后端画一个实心圆（转角补洞）
// roundCap 为 true 时开放子路径的两端也补圆（P2 的 linecap=round）
// 返回画出的线段数（供调用方估算开销）
template <class LineFn, class DotFn>
inline int PaintPathStroke(const PaintPath& path, float width, bool roundCap,
                           LineFn&& line, DotFn&& dot) {
    if (!path.Valid() || width <= 0.0f) return 0;
    const float half = width * 0.5f;
    int segs = 0;
    for (int c = 0; c < path.contours; c++) {
        const uint32_t first = path.first[c];
        const uint32_t cnt = path.count[c];
        if (cnt < 2) continue;
        const bool closed = path.closed && path.closed[c];
        const uint32_t segCount = closed ? cnt : cnt - 1;
        for (uint32_t i = 0; i < segCount; i++) {
            const uint32_t ia = first + i;
            const uint32_t ib = first + (i + 1) % cnt;
            const float ax = path.pts[ia * 2], ay = path.pts[ia * 2 + 1];
            const float bx = path.pts[ib * 2], by = path.pts[ib * 2 + 1];
            line(ax, ay, bx, by);
            segs++;
            // 顶点 b 上还有没有下一段：闭合子路径的最后一段也有
            const bool hasNext = (i + 1 < segCount) || closed;
            if (hasNext) {
                const uint32_t ic = first + (i + 2) % cnt;
                const float cx = path.pts[ic * 2], cy = path.pts[ic * 2 + 1];
                if (PathNeedJoin(ax, ay, bx, by, cx, cy))
                    dot(bx - half, by - half, width, width, half);
            }
        }
        if (!closed && roundCap) {
            const float ax = path.pts[first * 2], ay = path.pts[first * 2 + 1];
            const float bx = path.pts[(first + cnt - 1) * 2], by = path.pts[(first + cnt - 1) * 2 + 1];
            dot(ax - half, ay - half, width, width, half);
            dot(bx - half, by - half, width, width, half);
        }
    }
    return segs;
}

} // namespace binui

