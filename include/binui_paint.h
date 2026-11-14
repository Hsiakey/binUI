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
//   · 任意角度的渐变底色都按**真正的渐变轴**切条带（v0.16.13 起）：
//     180°/90° 走逐行 / 逐列的老路（条带最少），其余角度走
//     BuildFillBandsAngleGrad（逐行取圆角区间 + 沿 x 自适应切段 + 纵向合并）。
//     0.16.12 及以前 `135deg` 是按垂直渐变近似画的。
//   · 边框画在节点内侧，四边可不同宽；圆角盒子的边框按行贴着外圆弧走
//     （1px 步进），直边段合并成一整条 —— 条带数只跟半径有关，与高度无关。
//     半径大到装不下时退回直角四条（宁可方角，也不要缺边）。
// ============================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "binui_format.h"
#include "binui_path.h"

namespace binui {

struct PaintBand {
    float    x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0;
    float    radius = 0;   // > 0 时后端应走 fillRounded
    bool     round = false;
};

// 填充条带上限：渐变直边区按 **1px** 一条切（见 BuildFillBands 的说明），所以上限要跟着
// 屏幕高度走；768 覆盖到 ~253px 高的半透明元素（半透明走逐行 + 四边部分覆盖，一条最多
// 拆成 3 块），更高的退化为均分/硬边（仍然比原来的 24px 细）。
constexpr int kMaxPaintBands = 768;

// 渐变条带的"色阶"口径（底色渐变与 SVG 路径渐变共用，所以放在这里）：
//   每 ~2 设备像素一级（8~96 级），单行最多再切 96 段。
// 任意角度的底色渐变（BuildFillBandsAngleGrad）也用同一套参数 —— 两个渐变实现
// 的粗细/n 一致，同一张图里"路径渐变"和"底色渐变"才不会看着是两种质感。
constexpr int kMaxGradSplits = 96;       // 单行条带内最多再切几段
constexpr int kPathGradLevelsMin = 8;
constexpr int kPathGradLevelsMax = 96;
// 任意角度底色渐变（BuildFillBandsAngleGrad）的条带上限：斜向上"每行横向切段 + 纵向合并"
// 之后的数量是千级（180×140 的 135° 卡片实测约 2.3k 条），所以单开一份更大的预算。
// 调用方用堆上的复用缓冲（不要压在栈上），见 binui_draw_nodes.h 的 fillNodeBg。
constexpr int kAngleGradBands = 4096;
// 边框条带上限：圆角边框按行贴弧，条带数 ≈ 4r+常数（与盒子高度无关），
// 半径大的胶囊（比如 100px 高的按钮 r=50）也要装得下，所以比填充宽。
constexpr int kMaxBorderBands = 512;

inline uint32_t PaintLerpArgb(uint32_t a, uint32_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    // 用 (v + 0.5f) 截断代替 std::lround：通道值都 ≥ 0，两者结果逐位相同，
    // 但不用走 libm 的调用约定。渐变底色每帧要给几十个条带 × 4 个通道插值，
    // 这是一条真的会被跑到的热路径（见 docs/vulkan_design.md §7.6）。
    auto ch = [&](int shift) {
        const float ca = (float)((a >> shift) & 0xFF);
        const float cb = (float)((b >> shift) & 0xFF);
        return (uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFFu;
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// 同 PaintLerpArgb，但把 R/G/B 的取整误差按 err 带到下一条 —— 1D 抖动。
// 条带是**纯色矩形**，没法在像素级抖动；把条带切到 1px 之后按行累积误差，相邻行就会在
// floor/ceil 之间交替，视觉上把"每 40px 一个色阶"揉成 1px 细噪点（浏览器处理深色渐变
// 用的是同一招）。alpha 不抖动：半透明叠加上的抖动会变成可见的噪点。
inline uint32_t PaintLerpArgbDither(uint32_t a, uint32_t b, float t,
                                    float& errR, float& errG, float& errB) {
    auto chq = [&](int shift) -> uint32_t {   // 不抖动（alpha）
        const float ca = (float)((a >> shift) & 0xFF);
        const float cb = (float)((b >> shift) & 0xFF);
        return (uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFFu;
    };
    auto chd = [&](int shift, float& err) -> uint32_t {
        const float ca = (float)((a >> shift) & 0xFF);
        const float cb = (float)((b >> shift) & 0xFF);
        const float v = ca + (cb - ca) * t + err;
        float q = std::floor(v + 0.5f);
        if (q < 0.0f) q = 0.0f;
        if (q > 255.0f) q = 255.0f;
        err = v - q;
        if (err > 1.0f) err = 1.0f;
        if (err < -1.0f) err = -1.0f;
        return (uint32_t)q;
    };
    return (chq(24) << 24) | (chd(16, errR) << 16) | (chd(8, errG) << 8) | chd(0, errB);
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

// 按比例缩放颜色的 alpha（用于边缘部分覆盖：抗锯齿）
inline uint32_t PaintScaleAlpha(uint32_t c, float k) {
    if (k >= 0.999f) return c;
    if (k <= 0.0f) return c & 0x00FFFFFFu;
    uint32_t a = (uint32_t)((float)((c >> 24) & 0xFFu) * k + 0.5f);
    if (a > 255u) a = 255u;
    return (a << 24) | (c & 0x00FFFFFFu);
}

// 把一个（可能带小数的）矩形按"像素覆盖率"拆成最多 3×3 块：边界那一列/行用
// alpha × 覆盖率发出去，中间整覆盖。这样矩形边缘也是**抗锯齿**的 —— 后端只有
// fillRect（按像素中心一刀切），不这么做，填充边缘和解析式 AA 的描边就会各吸各的
// 像素（亚像素动画时每隔一帧差 1px，实机看着就是"在抖"）。
//
// 整数坐标时：左右列覆盖率为 0 ⇒ 只发中间一块，且与不拆的写法逐像素相同。
inline void PaintEmitAa(float x0, float x1, float y0, float y1, uint32_t color,
                        PaintBand* out, int& count, int cap) {
    if (x1 <= x0 || y1 <= y0 || count >= cap) return;
    const float ix0 = std::floor(x0), ix1 = std::ceil(x1);
    const float iy0 = std::floor(y0), iy1 = std::ceil(y1);
    const float fl = (x0 > ix0) ? (ix0 + 1.0f - x0) : 0.0f;   // 左列覆盖率
    const float fr = (x1 < ix1) ? (x1 - (ix1 - 1.0f)) : 0.0f; // 右列覆盖率
    const float ft = (y0 > iy0) ? (iy0 + 1.0f - y0) : 0.0f;   // 上行覆盖率
    const float fb = (y1 < iy1) ? (y1 - (iy1 - 1.0f)) : 0.0f; // 下行覆盖率
    const float mx0 = ix0 + (fl > 0.0f ? 1.0f : 0.0f);
    const float mx1 = ix1 - (fr > 0.0f ? 1.0f : 0.0f);
    const float my0 = iy0 + (ft > 0.0f ? 1.0f : 0.0f);
    const float my1 = iy1 - (fb > 0.0f ? 1.0f : 0.0f);

    float cx[3][3];   // {起点, 终点, 覆盖率}
    int nc = 0;
    if (fl > 0.0f) { cx[nc][0] = ix0;        cx[nc][1] = ix0 + 1.0f; cx[nc][2] = fl; ++nc; }
    if (mx1 > mx0) { cx[nc][0] = mx0;        cx[nc][1] = mx1;        cx[nc][2] = 1.0f; ++nc; }
    if (fr > 0.0f) { cx[nc][0] = ix1 - 1.0f; cx[nc][1] = ix1;        cx[nc][2] = fr; ++nc; }
    float cy[3][3];
    int nr = 0;
    if (ft > 0.0f) { cy[nr][0] = iy0;        cy[nr][1] = iy0 + 1.0f; cy[nr][2] = ft; ++nr; }
    if (my1 > my0) { cy[nr][0] = my0;        cy[nr][1] = my1;        cy[nr][2] = 1.0f; ++nr; }
    if (fb > 0.0f) { cy[nr][0] = iy1 - 1.0f; cy[nr][1] = iy1;        cy[nr][2] = fb; ++nr; }

    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc && count < cap; j++) {
            const float k = cx[j][2] * cy[i][2];
            if (k <= 0.002f) continue;
            PaintBand& b = out[count++];
            b.x = cx[j][0];
            b.y = cy[i][0];
            b.w = cx[j][1] - cx[j][0];
            b.h = cy[i][1] - cy[i][0];
            b.color = PaintScaleAlpha(color, k);
            b.radius = 0.0f;
            b.round = false;
        }
    }
}

// 任意角度线性渐变的条带生成（定义在本文件下方，与 SVG 渐变同一套算法）。
// 声明放在这里是因为 BuildFillBands 要先调它。
inline int BuildFillBandsAngleGrad(uint32_t from, uint32_t to, float angleDeg,
                                   float x, float y, float w, float h, float radius,
                                   PaintBand* out, int cap);

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
    if (p->flags & PAINT_GRAD_RADIAL) {
        // 布局里的底色径向渐变（CSS radial-gradient）不支持：退回单色底，别把整块画歪
        out[0] = { x, y, w, h, color, radius, radius > 0.0f };
        return 1;
    }

    // v0.16.13：任意角度（reserve[1] = 角度+1）走二维条带；V/H 仍走下面那条
    // 逐行/逐列的老路 —— 老路的条带更少、且逐像素与历史基准一致。
    if (p->reserve[1] != 0) {
        const int na = BuildFillBandsAngleGrad(p->gradFrom, p->gradTo,
                                               (float)(p->reserve[1] - 1),
                                               x, y, w, h, radius, out, cap);
        if (na > 0) return na;
        // 条带放不下（很大的元素）→ 落到下面的老三方向近似，宁可方向粗一点也不能少画
    }

    const bool horizontal = (p->gradDir == BINUI_GRAD_H);
    const float span = horizontal ? w : h;
    if (span <= 0.0f) return 0;
    // 半透明渐变也要细分/抖动：alpha 0.2~0.3 的色块叠在深底上时，24px 一条的台阶
    // 肉眼一样看得出来（"泡泡"的内层就是这种）。判定提前到这里，下面两处细分都要用。
    const bool translucent = ((p->gradFrom >> 24) & 0xFFu) != 0xFFu ||
                             ((p->gradTo >> 24) & 0xFFu) != 0xFFu;
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
            // 直边区的细分看"每个色阶占多少像素"：色阶台阶超过 ~6px 就会被看成一条条
            // 明显的色带（深色渐变尤其明显）—— 这种切 1px 并配合下面的误差扩散抖动；
            // 台阶本来就很细（颜色变化快）的渐变保持粗条带，省顶点/绘制次数。
            const float midLen = mid1 - mid0;
            auto delta = [&](int shift) {
                return (float)std::abs((int)((p->gradFrom >> shift) & 0xFFu) -
                                       (int)((p->gradTo >> shift) & 0xFFu));
            };
            const float dmax = std::max(delta(16), std::max(delta(8), delta(0)));
            const float stepPx = dmax > 0.0f ? midLen / dmax : midLen;
            const float bandPx = (stepPx > 6.0f || translucent) ? 1.0f : 32.0f;
            const int mb = std::max(1, std::min(room, (int)std::ceil(midLen / bandPx)));
            for (int i = 1; i <= mb; i++)
                edges[ne++] = mid0 + midLen * (float)i / (float)mb;
        }
        for (int i = 1; i <= rb; i++) {
            const float e = (span - r) + r * (float)i / (float)rb;
            if (e > edges[ne - 1] + 0.01f) edges[ne++] = e;
        }
    } else {
        // 无圆角：整条都是"直边"，细分口径与上面直边区一致（色阶台阶 > 6px 就切 1px）。
        auto delta0 = [&](int shift) {
            return (float)std::abs((int)((p->gradFrom >> shift) & 0xFFu) -
                                   (int)((p->gradTo >> shift) & 0xFFu));
        };
        const float dmax0 = std::max(delta0(16), std::max(delta0(8), delta0(0)));
        const float stepPx0 = dmax0 > 0.0f ? span / dmax0 : span;
        const float bandPx0 = (stepPx0 > 6.0f || translucent) ? 1.0f : 24.0f;
        const int n = std::min(std::max((int)std::ceil(span / bandPx0), 2), cap);
        for (int i = 1; i <= n; i++) edges[ne++] = span * (float)i / (float)n;
    }
    if (edges[ne - 1] < span - 0.01f) edges[ne++] = span;   // 防御：切点没走到末尾

    // v0.12.8：**半透明**渐变不能用"每条多 0.5px"去补缝。
    // 条带是 alpha 混合的：同一条像素行被相邻两条各盖一次 = 混合两次，于是
    // 每两条之间浮出一条横纹。深色底上（泡泡池的薄膜）能一眼看出横向条纹。
    // 半透明时条带首尾相接、**边缘保留小数**（切点相邻但不重叠）；不透明的老路径
    // 逐像素不变（`pixels` 金标就是靠这条保证的）。
    //
    // v0.16.2：半透明的切点**不能再吸整像素**。以前这里 std::round 把每条的上/下边
    // 取整，静态图看不出问题，但一动画就露馅：填充的上边界被吸到 round(y)，而边框
    // 是九宫格按小数 y 光栅化的（它自己按像素中心规则取整），两者每隔一帧就差 1px ——
    // 泡泡往上飘时顶边会周期性地浮出一条 1px 的"半透明填充"（实测 x270..300 顶边：
    // 偶数帧 rim 分两行 39/60，奇数帧 rim 单行 + 上方多一行纯填充 18 级亮度差）。
    // 保留小数后两边都交给同一个光栅化规则，切点相邻 ⇒ 既不会双混合也不会错位。
    //
    // v0.16.3：光"不吸整像素"还不够 —— 条带边缘仍然是**硬边**（光栅化按像素中心一刀
    // 切），描边是解析式 AA 的，亚像素位移时两者依旧差半个像素。所以半透明改走
    // **逐行 + 四边部分覆盖**（见 PaintEmitAa）：上下边界各半行、左右边界各拆一条
    // 部分列，填充边缘就和描边一样连续了。整数坐标时覆盖率恒为 1 ⇒ 发出去的矩形
    // 与旧的逐像素相同（`pixels` 金标不受影响）。
    int count = 0;
    float dithR = 0.0f, dithG = 0.0f, dithB = 0.0f;   // 沿渐变方向的取整误差（1D 抖动）
    if (translucent) {
        // 逐行光栅化：一行一条，颜色取**像素中心**处的渐变值，行覆盖率按形状边界算
        const int rows = (int)std::ceil(span) + 1;
        if (3 * rows + 8 <= cap) {
            const float along0 = horizontal ? x : y;          // 渐变轴起点
            const float cross0 = horizontal ? y : x;          // 交叉轴起点
            const float crossLen = horizontal ? h : w;
            const int first = (int)std::floor(along0);
            const int last = (int)std::ceil(along0 + span);
            for (int rr = first; rr < last; ++rr) {
                const float ra = (float)rr;
                float cov = 1.0f;
                if (ra < along0) cov = ra + 1.0f - along0;                  // 首行露出的部分
                if (ra + 1.0f > along0 + span)
                    cov = std::min(cov, along0 + span - ra);                // 末行露出的部分
                if (cov <= 0.002f) continue;
                float aF = ra - along0;                                     // 该行对应的条带起点
                if (aF < 0.0f) aF = 0.0f;
                if (aF > span - 1.0f) aF = span - 1.0f;
                const float t = (aF + 0.5f) / span;
                uint32_t col = PaintLerpArgbDither(p->gradFrom, p->gradTo, t, dithR, dithG, dithB);
                if (cov < 0.998f) col = PaintScaleAlpha(col, cov);
                float cA = cross0, cLen = crossLen;
                if (r > 0.5f) {                                             // 圆角区逐行内缩
                    const float dt = PaintRoundInset(std::min(aF, r), r);
                    const float db = PaintRoundInset(std::min(span - aF - 1.0f, r), r);
                    const float in = std::max(dt, db);
                    cA = cross0 + in;
                    cLen = crossLen - 2.0f * in;
                }
                if (cLen <= 0.0f) continue;
                if (horizontal) PaintEmitAa(ra, ra + 1.0f, cA, cA + cLen, col, out, count, cap);
                else            PaintEmitAa(cA, cA + cLen, ra, ra + 1.0f, col, out, count, cap);
            }
            return count;
        }
        // 行数太多（超高元素）→ 退回硬边条带，宁可没有 AA 也不截断
    }
    for (int i = 0; i + 1 < ne && count < cap; i++) {
        const float a = edges[i], b2 = edges[i + 1];
        if (b2 <= a + 0.001f) continue;
        const float t = (a + b2) * 0.5f / span;
        PaintBand bd;
        bd.color = PaintLerpArgbDither(p->gradFrom, p->gradTo, t, dithR, dithG, dithB);
        bd.round = false;
        bd.radius = 0;
        if (horizontal) {
            float e0 = x + a, e1 = x + b2;
            bd.x = e0;
            bd.y = y;
            bd.w = translucent ? (e1 - e0) : (b2 - a) + 0.5f;   // 不透明：轻微重叠，取整后不露缝
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
            float e0 = y + a, e1 = y + b2;
            bd.x = x;
            bd.y = e0;
            bd.w = w;
            bd.h = translucent ? (e1 - e0) : (b2 - a) + 0.5f;
            if (r > 0.5f) {
                const float dt = PaintRoundInset(std::min(a, r), r);
                const float db = PaintRoundInset(std::min(span - b2, r), r);
                const float in = std::max(dt, db);
                bd.x = x + in;
                bd.w = w - 2.0f * in;
                if (bd.w <= 0.0f) bd.w = 0.5f;
            }
        }
        if (bd.w <= 0.0f || bd.h <= 0.0f) continue;   // 退化段（切点重合）直接丢
        out[count++] = bd;
    }
    return count;
}

// ============================================================
// 任意角度线性渐变（v0.16.13，P1-6 落地）
// ------------------------------------------------------------
// 老实现只认 V/H/D 三个方向，而且 D（对角）也是按**垂直**画的：轴对齐条带在固定 y 上
// 颜色恒定，画不出"颜色沿 x 也变"的过渡（0.16.12 及以前就是这么近似）。这里把它补上：
//
//   · 逐像素行取圆角矩形的横跨区间（圆角用 PaintRoundInset 解析式内缩，仍是连续弧）；
//   · 按"沿 x 走一个色阶需要几个像素"把该行切成若干段，段内取段中心色；
//   · 段的位置和颜色与上一行完全相同就合并成高条带。
//
// 这就是 SVG 渐变（BuildPathFillBands）的同一套算法，只是形状用解析式圆角矩形而不是
// 折线路径。条带仍然是**轴对齐矩形**，所以后端契约不变（各端只画 fillRect），
// 三端像素也仍然逐位相同。
//
// 返回条带数；返回 -1 表示这个尺寸下条带放不下（调用方退回老三方向近似）。
// ============================================================
inline int BuildFillBandsAngleGrad(uint32_t from, uint32_t to, float angleDeg,
                                   float x, float y, float w, float h, float radius,
                                   PaintBand* out, int cap) {
    if (!out || cap <= 0 || w <= 0.0f || h <= 0.0f) return 0;
    const float kPi = 3.14159265358979323846f;
    const float th = angleDeg * (kPi / 180.0f);
    const float dx = std::sin(th);
    const float dy = -std::cos(th);              // CSS：0deg = to top，屏幕 y 向下
    // CSS 规范：渐变线过盒中心，长度 = |w·dx| + |h·dy| —— 于是四个角正好投影到两端。
    const float len = std::fabs(w * dx) + std::fabs(h * dy);
    if (len <= 1e-4f) {                          // 退化：两端色直接铺平
        out[0] = { x, y, w, h, PaintLerpArgb(from, to, 0.5f), 0.0f, false };
        return 1;
    }
    const float sx = x + w * 0.5f - dx * len * 0.5f;
    const float sy = y + h * 0.5f - dy * len * 0.5f;
    const float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;

    // 色阶密度与 SVG 渐变同一口径（每 ~2 设备像素一级，8~96 级），横向切段据此推出来。
    int levels = (int)std::lround(len * 0.5f);
    levels = levels < kPathGradLevelsMin ? kPathGradLevelsMin
                                         : (levels > kPathGradLevelsMax ? kPathGradLevelsMax : levels);
    const float dtdx = std::fabs(dx) / len;      // 每设备像素 t 的变化量
    const float stepPx = dtdx > 1e-9f ? 1.0f / ((float)levels * dtdx) : 1e9f;

    struct OpenRun { float x0, w; float endY; int idx; };
    enum { kMaxOpen = 96 };
    OpenRun open[kMaxOpen];
    int openN = 0;
    int n = 0;

    const int row0 = (int)std::floor(y);
    const int row1 = (int)std::ceil(y + h);
    for (int row = row0; row < row1; row++) {
        float ry0 = std::max((float)row, y);
        float ry1 = std::min((float)row + 1.0f, y + h);
        if (ry1 <= ry0 + 0.002f) continue;
        float cA = x, cLen = w;
        if (r > 0.5f) {
            // 用行中心算内缩：与老 V/H 渐变完全同一套口径，圆角才不会被切出台阶
            const float dyTop = (ry0 + ry1) * 0.5f - y;
            const float dyBot = (y + h) - (ry0 + ry1) * 0.5f;
            const float in = std::max(PaintRoundInset(std::min(dyTop, r), r),
                                      PaintRoundInset(std::min(dyBot, r), r));
            cA = x + in;
            cLen = w - 2.0f * in;
        }
        if (cLen <= 0.002f) continue;

        int segs = 1;
        if (dtdx > 1e-9f) {
            const int k = (int)std::ceil(cLen / std::max(stepPx, 1.0f));
            if (k > 1) segs = std::min(k, kMaxGradSplits);
        }
        for (int sg = 0; sg < segs; sg++) {
            const float sx0 = cA + cLen * (float)sg / (float)segs;
            const float sx1 = cA + cLen * (float)(sg + 1) / (float)segs;
            if (sx1 <= sx0 + 0.002f) continue;
            // 段中心处的 t（量化到级中心）
            const float px = (sx0 + sx1) * 0.5f;
            const float py = (ry0 + ry1) * 0.5f;
            float t = ((px - sx) * dx + (py - sy) * dy) / len;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            if (levels > 1) {
                float q = (std::floor(t * (float)levels) + 0.5f) / (float)levels;
                if (q > 1.0f) q = 1.0f;
                t = q;
            }
            // ★ 这里**不做误差扩散抖动**（老 V/H 路径才抖）：抖动会让相邻行的颜色
            // 永不相等，纵向合并直接失效 —— 实测 180×140 的 135° 卡片从 260 条涨到
            // 7700+ 条、直接撞 cap 退回"垂直近似"（等于没修）。级内恒定的代价是
            // 能看出色阶，这与 SVG 渐变（BuildPathFillBands）是同一条底线。
            const uint32_t col = PaintLerpArgb(from, to, t);
            // 与上一行同位置同色 → 直接加高（省一个 draw call）
            bool merged = false;
            for (int k = 0; k < openN; k++) {
                if (open[k].x0 == sx0 && open[k].w == (sx1 - sx0) && open[k].endY == ry0 &&
                    out[open[k].idx].color == col) {
                    out[open[k].idx].h += (ry1 - ry0);
                    open[k].endY = ry1;
                    merged = true;
                    break;
                }
            }
            if (merged) continue;
            if (n >= cap) return -1;             // 放不下：交给调用方降级
            out[n] = { sx0, ry0, sx1 - sx0, ry1 - ry0, col, 0.0f, false };
            if (openN < kMaxOpen) {
                open[openN++] = { sx0, sx1 - sx0, ry1, n };
            } else {
                int oldest = 0;
                for (int k = 1; k < openN; k++)
                    if (open[k].endY < open[oldest].endY) oldest = k;
                open[oldest] = { sx0, sx1 - sx0, ry1, n };
            }
            n++;
        }
    }
    return n;
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

// ============================================================
// 阴影（v23 box-shadow）：高斯模糊的盒 → 一串同心圆角矩形"环"
// ------------------------------------------------------------
// 后端只有 fillRect / fillRounded，没有"模糊"原语。做法是把模糊本身解析地算出来：
// box-shadow 的形状 = 边框盒按 spread 外扩（圆角半径同步 +spread）之后的圆角矩形，
// 再按 σ = blur/2 做高斯模糊。高斯模糊一个形状，等价于"把形状沿法线外扩 d、覆盖率
// 取 0.5·erfc(d/σ√2)"，所以整块阴影就是**一串同心圆角矩形**：
//
//   环 i：几何 = 盒边外扩 d_i 的圆角矩形，颜色 = 该区间中点的覆盖率
//
// **必须从外往里画**（`out[0]` 是最外面那环）：后画的环整块盖住先画的，于是每个像素
// 最终拿到的是"包住它的最内环"的覆盖率 —— 正好等于该点的真实覆盖率，而且不需要
// 后端提供"环形（挖孔）"几何。代价是重叠区域的过绘制（32 环 × 阴影面积），GPU 上
// 可以忽略；GDI 画布本来就慢，见 app_win32.cpp 里的按色去重。
//
// 两个字段的用法不同，别混：
//   · GPU（GL/VK/D3D11）走真 alpha 混合，环与环是"叠加"关系：直接用 cover 会在
//     **重叠区叠深**（形状内部的像素被每一环都盖一次，0.30 的阴影最后变成 0.5）。
//     所以 color 里预先做了累计校正 a_i = (T_i−T_{i−1})/(1−T_{i−1})，逐环源覆盖之后
//     正好得到 T_i = A×cover_i（多个阴影叠加时也自然是 1−(1−a)(1−b)）；
//   · GDI 没有 alpha 合成：位图里每环是**直接覆盖**（不是叠加），所以用 cover
//     （覆盖率）自己算出 alpha，见 app_win32.cpp 的 ShadowBitmapGet。
//
// 近似之处（有意为之）：圆角处的真实二维覆盖率略低于"按直边法线距离算"的值
// （凸角收到的高斯权重更少），但阴影是柔和渐变，肉眼分不出来；类比 SVG 渐变
// 只切 8~96 级色阶是同一条底线 —— 宁可近似，也不引入后端私有能力。
// ============================================================
struct PaintShadowRing {
    float    x = 0, y = 0, w = 0, h = 0;
    float    radius = 0;
    uint32_t color = 0;   // GPU 用：已做累计 alpha 校正（直接源覆盖画）
    float    cover = 0;   // 0..1 目标累计覆盖率（GDI 用：按底色混合）
};

constexpr int kMaxShadowRings = 48;

// 直边沿法线偏移 d 处的覆盖率（d < 0 = 在形状内部 ⇒ 1）
inline float PaintGaussCoverage(float d, float sigma) {
    if (sigma <= 0.0f) return d <= 0.0f ? 1.0f : 0.0f;
    return 0.5f * std::erfc(d / (sigma * 1.41421356237309505f));
}

// x/y/w/h 是节点盒，radius 是节点当前生效的圆角（含动画缩放后的值），
// opacity 是级联后的元素不透明度（阴影跟着一起淡）。
// 返回环数（0 = 不画：全透明 / 盒退化）；out[0] 是最外圈，画到最后才盖住中心。
inline int BuildShadowRings(const BinUIShadow& s, float x, float y, float w, float h,
                            float radius, float opacity,
                            PaintShadowRing* out, int cap = kMaxShadowRings) {
    if (!out || cap <= 0 || w <= 0.0f || h <= 0.0f) return 0;
    const float A = (float)((s.color >> 24) & 0xFFu) / 255.0f * opacity;
    if (A <= 0.002f) return 0;
    const uint32_t rgb = s.color & 0x00FFFFFFu;
    // spread：整块阴影向四周外扩，圆角半径同步增大（CSS 的语法就是这么定义的）
    const float bx = x - s.spread + s.offsetX;
    const float by = y - s.spread + s.offsetY;
    const float bw = w + 2.0f * s.spread;
    const float bh = h + 2.0f * s.spread;
    if (bw <= 0.05f || bh <= 0.05f) return 0;       // spread 负到把盒子收没了
    float r0 = radius + s.spread;
    if (r0 < 0.0f) r0 = 0.0f;
    const float lim = std::min(bw, bh) * 0.5f;
    if (r0 > lim) r0 = lim;

    const float sigma = s.blur > 0.0f ? s.blur * 0.5f : 0.0f;   // CSS：blur-radius = 2σ
    if (sigma <= 0.4f) {                                        // 硬边阴影：一环就够（自带解析 AA）
        out[0] = { bx, by, bw, bh, r0,
                   ((uint32_t)(A * 255.0f + 0.5f) << 24) | rgb, 1.0f };
        return 1;
    }

    // 环数按"相邻两环的透明度台阶 ≤ 3/255"定 —— 肉眼看不出台阶，环数也不失控
    int steps = (int)std::ceil(A * 255.0f / 3.0f);
    if (steps < 2) steps = 2;
    if (steps > 32) steps = 32;
    if (steps > cap) steps = cap;
    const float stepCov = 1.0f / (float)steps;

    // 候选边界：沿 d 从 3σ 均匀取到 0（外 → 内），再按覆盖率台阶贪心合并成环。
    // 3σ 之外覆盖率 < 0.2%，画了也看不出来（浏览器也是在这个距离截断的）。
    float bnd[64], cov[64];
    int nb = 0;
    {
        const float ext = sigma * 3.0f;
        const int NG = 48;
        for (int k = 0; k <= NG && nb < 64; k++) {
            const float d = ext * (1.0f - (float)k / (float)NG);
            bnd[nb] = d;
            cov[nb] = PaintGaussCoverage(d, sigma);
            nb++;
        }
    }

    int n = 0, last = 0;
    float prevT = 0.0f;
    for (int i = 1; i < nb && n < cap; i++) {
        const bool tail = (i == nb - 1);
        if (cov[i] - cov[last] < stepCov && !tail) continue;
        const float k = bnd[last];                        // 环的外边界（相对盒边外扩）
        const float cover = 0.5f * (cov[last] + cov[i]);  // 该区间的目标覆盖率
        const float T = A * cover;                        // 目标累计 alpha
        float a = 1.0f;                                   // 校正后的源 alpha（见上）
        if (1.0f - prevT > 1e-4f) a = (T - prevT) / (1.0f - prevT);
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        PaintShadowRing& rr = out[n++];
        rr.x = bx - k;
        rr.y = by - k;
        rr.w = bw + 2.0f * k;
        rr.h = bh + 2.0f * k;
        rr.radius = r0 + k;
        rr.color = ((uint32_t)(a * 255.0f + 0.5f) << 24) | rgb;
        rr.cover = cover;
        prevT = T;
        last = i;
    }
    // 最后一环 = 形状本身（外扩 0）。**必须有这一环**：外扩环只往"外面"长，
    // 形状内部的像素（底色透明的元素、或阴影比盒子还大的情形）只有它才盖得到，
    // 覆盖率取 1（形状内部几乎全被覆盖）。alpha 同样要按 prevT 做累计校正 ——
    // 直接写 A 会把内部叠成 1−(1−T)(1−A)（实测 0.28 的阴影内部变成 0.38）。
    if (n < cap) {
        float a = 1.0f;
        if (1.0f - prevT > 1e-4f) a = (A - prevT) / (1.0f - prevT);
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        out[n++] = { bx, by, bw, bh, r0, ((uint32_t)(a * 255.0f + 0.5f) << 24) | rgb, 1.0f };
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
constexpr int kMaxPathBands = 4096;      // 单条路径的条带上限（v16 渐变要比纯色多）
constexpr int kMaxPathCrossings = 1024;  // 单条扫描线的交点上限
constexpr int kMaxRowSpans = 128;        // 单行条带上限
constexpr int kMaxOpenRuns = 96;         // 跨行合并的"正在延伸的条带"表

// ---- v16 路径渐变（SVG <linearGradient>/<radialGradient>）------------------
//
// 渐变怎么落到"只有 fillRect"的后端上：路径填充本来就是"扫描线 → 轴对齐条带"，
// 渐变只是把每条带的颜色从常量换成"该带中心点的颜色"。两个关键取舍：
//
//   · **量化**。t 先量化到 levels 级、级内取级中心色。这样同一级里的相邻行颜色
//     完全相等，才能继续走"上下合并成高条带"那条老路；否则每条带颜色都不同，
//     一个 400px 高的形状就是 400 条带，直接撞上限。
//   · **按方向切**。颜色沿 x 变时（水平/斜向/径向）按"沿 x 走一级要几个像素"把
//     这一行的条带切细；沿 x 不变的（纯垂直渐变）一段都不切，全靠纵向合并收敛。
//     于是横、竖两个方向的条带数都只跟 levels 同量级，不随面积增长。
//
// 代价是**色阶可见**：levels 每级约 2 设备像素，放大到 400px 的高饱和度渐变能看出
// 台阶。这与"不做抗锯齿、只用 fillRect"是同一条底线，不是这里偷懒。
// （kMaxGradSplits / kPathGradLevelsMin / kPathGradLevelsMax 声明在本文件上方 ——
//  任意角度渐变 BuildFillBandsAngleGrad 也要用同一套口径。）
// 单节点缓存上限。**别按"够用"去调小**：条带数 ≈ 节点高度 × 1~3（AA 逐行），
// 300×300 的图标实测 1173 条 —— 以前的 1024 让这种图标一张就"永不缓存、每帧重算 199 µs"，
// 而 240×240（935 条）刚好能缓存，阈值卡在两个相邻尺寸之间，等于随机。
// 现在单节点放宽到 16384 条（≈384 KB），全局预算由渲染器的 pathBandsBudget() 把关，
// 超预算的节点退回"不缓存"，不会把内存吃穿。
constexpr int kMaxCachedBands = 16384;

// spreadMethod：把 t 折进 0..1（pad 夹紧 / repeat 取小数 / reflect 折返）
inline float PathGradSpreadT(float t, uint8_t spread) {
    if (spread == PATH_GRAD_REPEAT) return t - std::floor(t);
    if (spread == PATH_GRAD_REFLECT) {
        const float u = t * 0.5f - std::floor(t * 0.5f);   // 三角波，0..1
        return u <= 0.5f ? u * 2.0f : (1.0f - u) * 2.0f;
    }
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// 归一化点 → 渐变轴位置 t
//   线性：投影到 (x1,y1)→(x2,y2)
//   径向：把坐标按 (rx,ry) 归一化后
//     · 焦点 = 圆心（绝大多数素材）：t 就是到圆心的距离，t=1 在椭圆边界上
//     · 焦点 ≠ 圆心（v18）：按 SVG 的锥形语义 —— 从焦点 F 出发经过 P 的射线打到
//       单位圆于 Q，t = |P-F| / |Q-F|。也就是解 |F + s·(P-F)| = 1 取正根，再 t = 1/s。
//   两轴都用**包围盒归一化坐标**算，所以包围盒非正方时径向天然是椭圆（SVG 语义）。
inline float PathGradT(const BinUIPathGradient& g, float nx, float ny) {
    if (g.type == PATH_GRAD_RADIAL) {
        const float rx = std::fabs(g.rx) > 1e-6f ? std::fabs(g.rx) : 1e-6f;
        const float ry = std::fabs(g.ry) > 1e-6f ? std::fabs(g.ry) : 1e-6f;
        const float u = (nx - g.x1) / rx;
        const float v = (ny - g.y1) / ry;
        const float fu = (g.fx - g.x1) / rx;    // 焦点（同样归一化）
        const float fv = (g.fy - g.y1) / ry;
        const bool hasFocus = (std::fabs(fu) > 1e-4f) || (std::fabs(fv) > 1e-4f);
        if (hasFocus) {
            // 焦点必须落在单位圆内；跑到外面就退回同心圆（规范没定义，别画出鬼影）
            const float fr2 = fu * fu + fv * fv;
            if (fr2 < 0.9999f) {
                const float dx = u - fu, dy = v - fv;
                const float a = dx * dx + dy * dy;
                if (a <= 1e-12f) return PathGradSpreadT(0.0f, g.spread);   // 正好点在焦点上
                const float b = 2.0f * (fu * dx + fv * dy);
                const float c = fr2 - 1.0f;
                const float disc = b * b - 4.0f * a * c;
                if (disc > 0.0f) {
                    const float s2 = (-b + std::sqrt(disc)) / (2.0f * a);   // 正根 = 打到圆上的位置
                    if (s2 > 1e-6f) return PathGradSpreadT(1.0f / s2, g.spread);
                }
            }
        }
        return PathGradSpreadT(std::sqrt(u * u + v * v), g.spread);
    }
    const float ax = g.x2 - g.x1, ay = g.y2 - g.y1;
    const float len2 = ax * ax + ay * ay;
    if (len2 <= 1e-12f) return 0.0f;                     // 起止点重合：退化纯色
    return PathGradSpreadT(((nx - g.x1) * ax + (ny - g.y1) * ay) / len2, g.spread);
}

// t（已折叠到 0..1）→ 颜色：色标间线性插值，两端用端色标
inline uint32_t PathGradColorAt(const BinUIPathGradient& g, float t) {
    const int n = (int)g.stopCount;
    if (n <= 0) return 0xFF000000u;
    if (n == 1 || t <= g.stops[0].offset) return g.stops[0].color;
    for (int i = 1; i < n; i++) {
        const float o1 = g.stops[i].offset;
        if (t <= o1) {
            const float o0 = g.stops[i - 1].offset;
            const float span = o1 - o0;
            const float k = span > 1e-6f ? (t - o0) / span : 1.0f;
            return PaintLerpArgb(g.stops[i - 1].color, g.stops[i].color, k);
        }
    }
    return g.stops[n - 1].color;
}

// 节点整体不透明度：纯色由调用方先乘好，渐变有多个色标只能在这里逐条乘
inline uint32_t PathScaleArgb(uint32_t c, float op) {
    if (op >= 1.0f) return c;
    if (op <= 0.0f) return c & 0x00FFFFFFu;
    uint32_t a = (uint32_t)(((c >> 24) & 0xFFu) * op);
    if (a > 255u) a = 255u;
    return (c & 0x00FFFFFFu) | (a << 24);
}

// 一条路径的填充取样器：纯色与渐变共用同一个接口，后端与扫描线那边不用分支
struct PathShade {
    const BinUIPathGradient* grad = nullptr;   // nullptr = 纯色
    uint32_t color = 0xFF000000u;              // 纯色 / 渐变不可用时的兜底色
    int      levels = 0;                       // 量化级数（0 = 不量化）
    float    alpha = 1.0f;                     // v16：渐变色的 alpha 缩放（节点 opacity）
    float    bx = 0.0f, by = 0.0f;             // 节点盒（归一化基准）
    float    bw = 1.0f, bh = 1.0f;
    float    dtdx = 0.0f;                      // 每设备像素 t 的变化量（横向切段密度）

    bool Active() const { return grad && grad->stopCount >= 2 && levels > 0; }

    // 设备点 → 带颜色（渐变按量化级取级中心色，级内恒定 → 可纵向合并）
    uint32_t At(float x, float y) const {
        if (!Active()) return color;
        const float t = PathGradT(*grad, (x - bx) / bw, (y - by) / bh);
        uint32_t c = 0;
        if (levels <= 1) {
            c = PathGradColorAt(*grad, t);
        } else {
            float q = (std::floor(t * (float)levels) + 0.5f) / (float)levels;
            if (q > 1.0f) q = 1.0f;
            c = PathGradColorAt(*grad, q);
        }
        return PathScaleArgb(c, alpha);
    }
};

// 按节点盒与渐变几何一次性备好取样器（级数按渐变轴在设备上的长度取）
inline PathShade MakePathShade(const BinUIPathGradient* g, uint32_t fallback, float opacity,
                               float x, float y, float w, float h) {
    PathShade s;
    s.color = fallback;
    s.alpha = opacity;
    s.bx = x; s.by = y;
    s.bw = w > 0.0f ? w : 1.0f;
    s.bh = h > 0.0f ? h : 1.0f;
    if (!g || g->stopCount < 2) return s;                 // 1 个色标 = 纯色
    float len = 0.0f, dtdx = 0.0f;
    if (g->type == PATH_GRAD_RADIAL) {
        const float rx = std::fabs(g->rx) > 1e-6f ? std::fabs(g->rx) : 1e-6f;
        const float ry = std::fabs(g->ry) > 1e-6f ? std::fabs(g->ry) : 1e-6f;
        len = (rx * s.bw + ry * s.bh) * 0.5f;             // t: 0→1 的平均设备距离
        dtdx = 1.0f / (rx * s.bw);                        // 最陡处（过圆心那一行）
    } else {
        const float ax = g->x2 - g->x1, ay = g->y2 - g->y1;
        const float len2 = ax * ax + ay * ay;
        if (len2 <= 1e-12f) return s;                     // 退化：纯色
        len = std::sqrt((ax * s.bw) * (ax * s.bw) + (ay * s.bh) * (ay * s.bh)) / len2;
        dtdx = std::fabs(ax) / (s.bw * len2);
    }
    int n = (int)std::lround(len * 0.5f);                 // 每 ~2 设备像素一级
    if (n < kPathGradLevelsMin) n = kPathGradLevelsMin;
    if (n > kPathGradLevelsMax) n = kPathGradLevelsMax;
    s.levels = n;
    s.dtdx = dtdx;
    s.grad = g;
    return s;
}

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
// shade 决定每条带的颜色（纯色或 v16 渐变）；rowStep>1 是降级模式（行更粗、条带更少）。
// truncated = true 表示撞上 cap / 交点上限，调用方应放大 rowStep 重试。
inline int BuildPathFillBands(const PaintPath& path, bool evenOdd,
                              float clipX0, float clipY0, float clipX1, float clipY1,
                              const PathShade& shade, PaintBand* out, int cap,
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
            if (x1 - x0 <= 0) continue;
            // 渐变：按"沿 x 走一级需要几个像素"把这一行切成几段（纯色永远 1 段）。
            // 沿 x 不变的渐变（纯垂直）dtdx≈0，一段都不切，全靠纵向合并收敛。
            int segs = 1;
            if (shade.Active() && shade.dtdx > 1e-9f) {
                const float stepPx = 1.0f / ((float)shade.levels * shade.dtdx);
                const int k = (int)std::ceil((float)(x1 - x0) / std::max(stepPx, 1.0f));
                if (k > segs) segs = std::min(k, kMaxGradSplits);
            }
            const int spanW = x1 - x0;
            for (int sg = 0; sg < segs; sg++) {
                const int sx0 = x0 + spanW * sg / segs;
                const int sx1 = x0 + spanW * (sg + 1) / segs;
                const int w2 = sx1 - sx0;
                if (w2 <= 0) continue;
                // 颜色取这一小段中心点：级内恒定，相邻行同色才能合并成高条带
                const uint32_t col = shade.At((float)sx0 + (float)w2 * 0.5f, yc);
                // 与上一行的条带对齐（位置与颜色都相同）→ 直接加高，省一个 draw call
                bool merged = false;
                for (int k = 0; k < openN; k++) {
                    if (open[k].x0 == sx0 && open[k].w == w2 && open[k].endY == y &&
                        out[open[k].idx].color == col) {
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
                out[n] = { (float)sx0, (float)y, (float)w2, (float)rowStep, col, 0.0f, false };
                if (openN < kMaxOpenRuns) {
                    open[openN++] = { sx0, w2, y + rowStep, n };
                } else {
                    // 合并表满了：只丢合并机会，像素仍然正确 —— 找一条最老的顶掉
                    int oldest = 0;
                    for (int k = 1; k < openN; k++)
                        if (open[k].endY < open[oldest].endY) oldest = k;
                    open[oldest] = { sx0, w2, y + rowStep, n };
                }
                n++;
            }
        }
        flushStale(y);
    }
    return n;
}

// 纯色短路：老调用点（测试、内部工具）不必包一层 PathShade
inline int BuildPathFillBands(const PaintPath& path, bool evenOdd,
                              float clipX0, float clipY0, float clipX1, float clipY1,
                              uint32_t color, PaintBand* out, int cap,
                              int rowStep = 1, bool* truncated = nullptr) {
    PathShade s;
    s.color = color;
    return BuildPathFillBands(path, evenOdd, clipX0, clipY0, clipX1, clipY1, s, out, cap,
                              rowStep, truncated);
}

// ============================================================
// 边缘抗锯齿（v19，shape-rendering=auto/optimizeQuality）
//
// 仍然只画 fillRect，所以不做 MSAA/stencil：改用**逐像素覆盖率**。
//   1. 每个像素行纵向做 kPathAASubRows 次子采样，把每条子扫描线覆盖到的像素
//      按重叠比例累加进覆盖率缓冲（0..255）；
//   2. 把覆盖率相同的**连续像素**合并成一条带，颜色 alpha 乘覆盖率/255。
// 于是斜边上的像素被"部分覆盖"，台阶就淡了 —— 三端各自合成本端自己的 alpha
// （GL/D3D 是真合成；GDI 走 BlendOver 近似，见下）。
//
// 已知代价（写进 docs/svg.md）：
//   · 条带数变多（每条行多出左右两条边缘带，整行内区仍会合并成一条）；
//   · **三端不再逐像素一致**：GDI 没有真 alpha 合成，半透明的边缘带是按"底色"
//     近似混合的，同一张图在 GDI 与 GL 的边缘像素会差几十级通道；
//   · 因此像素基准只在"没开 AA"时保证零差异。
// ============================================================
constexpr int kPathAASubRows  = 4;      // 每个像素行的纵向子采样数
constexpr int kMaxPathAAWidth = 4096;   // 覆盖率缓冲宽度上限（更宽就退回硬边）

// 返回条带数；返回 -1 表示"这形状太宽，AA 做不了"（调用方退回硬边填充）。
inline int BuildPathFillBandsAA(const PaintPath& path, bool evenOdd,
                                float clipX0, float clipY0, float clipX1, float clipY1,
                                const PathShade& shade, PaintBand* out, int cap,
                                std::vector<uint16_t>& covAcc, bool* truncated) {
    if (truncated) *truncated = false;
    if (!path.Valid() || !out || cap <= 0) return 0;
    if (clipX1 <= clipX0 || clipY1 <= clipY0) return 0;
    const int xLo = (int)std::floor(clipX0);
    const int xHi = (int)std::ceil(clipX1);
    const int width = xHi - xLo;
    if (width <= 0) return 0;
    if (width > kMaxPathAAWidth) return -1;
    covAcc.assign(static_cast<size_t>(width), 0);

    struct Crossing { float x; int dir; };
    Crossing crossings[kMaxPathCrossings];
    struct SpanF { float a, b; };
    SpanF spans[kMaxRowSpans];
    struct OpenRun { int x0, w, endY, idx; };
    OpenRun open[kMaxOpenRuns];
    int openN = 0;
    const int yBegin = (int)std::floor(clipY0);
    const int yEnd = (int)std::ceil(clipY1);
    auto flushStale = [&](int y) {
        int w = 0;
        for (int i = 0; i < openN; i++)
            if (open[i].endY > y) open[w++] = open[i];
        openN = w;
    };

    int n = 0;
    for (int y = yBegin; y < yEnd; y++) {
        std::fill(covAcc.begin(), covAcc.end(), static_cast<uint16_t>(0));
        // ---- 1. 纵向子采样，累加覆盖率 ----
        for (int sr = 0; sr < kPathAASubRows; sr++) {
            const float yc = (float)y + ((float)sr + 0.5f) / (float)kPathAASubRows;
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
                    if (ay == by) continue;
                    if ((ay <= yc) == (by <= yc)) continue;
                    if (nc >= kMaxPathCrossings) { overflow = true; break; }
                    const float t = (yc - ay) / (by - ay);
                    crossings[nc++] = { ax + t * (bx - ax), ay < by ? 1 : -1 };
                }
            }
            if (overflow) {
                if (truncated) *truncated = true;
                return n;
            }
            if (nc < 2) continue;
            std::sort(crossings, crossings + nc,
                      [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
            int ns = 0;
            if (evenOdd) {
                for (int i = 0; i + 1 < nc; i += 2) {
                    if (ns >= kMaxRowSpans) break;
                    spans[ns++] = { crossings[i].x, crossings[i + 1].x };
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
                        if (ns >= kMaxRowSpans) break;
                        spans[ns++] = { startX, crossings[i].x };
                    }
                }
            }
            // 把浮点区间按"每个像素重叠多少"累加。
            // 累加器用 1024 定点（不是 255/子采样数）：整数除法 255/4=63，
            // 满覆盖也只剩 252/255，会把整块面积系统性低估 1.2%（实测到的偏差）。
            constexpr int kCovScale = 1024;
            const int unit = kCovScale / kPathAASubRows;
            for (int i = 0; i < ns; i++) {
                int p0 = (int)std::floor(spans[i].a);
                int p1 = (int)std::floor(spans[i].b);
                if (p0 < xLo) p0 = xLo;
                if (p1 >= xHi) p1 = xHi - 1;
                if (p1 < p0) continue;
                if (p0 == p1) {
                    const float ov = spans[i].b - spans[i].a;
                    covAcc[p0 - xLo] =
                        static_cast<uint16_t>(covAcc[p0 - xLo] + ov * unit);
                    continue;
                }
                const float ovFirst = std::min(spans[i].b, (float)p0 + 1.0f) - spans[i].a;
                const float ovLast = spans[i].b - std::max(spans[i].a, (float)p1);
                covAcc[p0 - xLo] = static_cast<uint16_t>(covAcc[p0 - xLo] + ovFirst * unit);
                for (int q = p0 + 1; q < p1; q++)
                    covAcc[q - xLo] = static_cast<uint16_t>(covAcc[q - xLo] + unit);
                covAcc[p1 - xLo] = static_cast<uint16_t>(covAcc[p1 - xLo] + ovLast * unit);
            }
        }
        // ---- 2. 覆盖率量化成 0..255，相同覆盖率的连续像素合并成一条带 ----
        const float ycRow = (float)y + 0.5f;
        int p = 0;
        while (p < width) {
            auto alphaOf = [](uint16_t cov) -> int {
                int a = (static_cast<int>(cov) * 255 + 512) / 1024;
                return a > 255 ? 255 : a;
            };
            int a = alphaOf(covAcc[p]);
            if (a <= 0) { p++; continue; }
            int q = p + 1;
            while (q < width && alphaOf(covAcc[q]) == a) q++;
            const int runX = xLo + p, runW = q - p;
            const uint32_t col =
                PathScaleArgb(shade.At((float)runX + (float)runW * 0.5f, ycRow),
                              (float)a / 255.0f);
            bool merged = false;
            for (int k = 0; k < openN; k++) {
                if (open[k].x0 == runX && open[k].w == runW && open[k].endY == y &&
                    out[open[k].idx].color == col) {
                    out[open[k].idx].h += 1.0f;
                    open[k].endY = y + 1;
                    merged = true;
                    break;
                }
            }
            if (merged) { p = q; continue; }
            if (n >= cap) {
                if (truncated) *truncated = true;
                return n;
            }
            out[n] = { (float)runX, (float)y, (float)runW, 1.0f, col, 0.0f, false };
            if (openN < kMaxOpenRuns) {
                open[openN++] = { runX, runW, y + 1, n };
            } else {
                int oldest = 0;
                for (int k = 1; k < openN; k++)
                    if (open[k].endY < open[oldest].endY) oldest = k;
                open[oldest] = { runX, runW, y + 1, n };
            }
            n++;
            p = q;
        }
        flushStale(y);
    }
    return n;
}

// 折线描边：逐段画线，转角按 stroke-linejoin 补缺口（圆角补圆点 / 斜接与斜面填多边形）。
// 三个后端的 fillLine / fillRounded / fillRect 签名一致，所以这里用回调模板，
// 后端只传"怎么画一根线 / 一个圆 / 一堆条带"，几何只写一份。
//
// ---- 描边回调契约 ----
// line(x0,y0,x1,y1,w,c) → 后端画一根 w 粗的线（颜色逐段给：渐变描边靠这个）
// dot(x,y,w,h,r,c)      → 后端画一个实心圆（圆角连接 / 圆头）
// poly(xy, n, c)        → 后端填一个多边形（斜接/斜面缺口）
// colorAt(x,y)          → 该点颜色（纯色时是常量，渐变描边时逐段取色）
// cap = BinUIPathCap：BUTT（端点即路径端点）/ ROUND（两端半圆）/ SQUARE（两端各外延半线宽）
// join = BinUIPathJoin；miterLimit 为 0 时按 SVG 默认 4
// 返回画出的线段数（供调用方估算开销）
template <class ColorAt, class LineFn, class DotFn, class PolyFn>
inline int PaintPathStroke(const PaintPath& path, float width, uint8_t cap,
                           uint8_t join, uint8_t miterLimit, ColorAt&& colorAt,
                           LineFn&& line, DotFn&& dot, PolyFn&& poly) {
    if (!path.Valid() || width <= 0.0f) return 0;
    const float half = width * 0.5f;
    const float limit = miterLimit > 0 ? static_cast<float>(miterLimit) : 4.0f;
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
            // 原始端点（转角几何一律用它 —— 方头外延只影响画出来的那根线）
            const float oax = path.pts[ia * 2], oay = path.pts[ia * 2 + 1];
            const float obx = path.pts[ib * 2], oby = path.pts[ib * 2 + 1];
            float ax = oax, ay = oay, bx = obx, by = oby;
            if (!closed && cap == BINUI_CAP_SQUARE) {
                // 方头 = 端点沿切线方向各外延半线宽（这就是 SVG square cap 的定义）
                const float lx = obx - oax, ly = oby - oay;
                const float ll = std::sqrt(lx * lx + ly * ly);
                if (ll > 1e-6f) {
                    const float ex = lx / ll * half, ey = ly / ll * half;
                    if (i == 0) { ax -= ex; ay -= ey; }
                    if (i + 1 == segCount) { bx += ex; by += ey; }
                }
            }
            line(ax, ay, bx, by, width, colorAt((ax + bx) * 0.5f, (ay + by) * 0.5f));
            segs++;
            // 顶点 b 上还有没有下一段：闭合子路径的最后一段也有
            const bool hasNext = (i + 1 < segCount) || closed;
            if (hasNext) {
                const uint32_t ic = first + (i + 2) % cnt;
                const float cx = path.pts[ic * 2], cy = path.pts[ic * 2 + 1];
                if (!PathNeedJoin(oax, oay, obx, oby, cx, cy)) continue;
                if (join == BINUI_JOIN_MITER || join == BINUI_JOIN_BEVEL) {
                    const PathJoinGeom g = PathJoinAt(oax, oay, obx, oby, cx, cy, half);
                    if (!g.ok) continue;
                    const uint32_t col = colorAt(obx, oby);
                    if (join == BINUI_JOIN_MITER && g.ratio <= limit) {
                        const float q[8] = { g.p1x, g.p1y, g.mx, g.my, g.p2x, g.p2y, obx, oby };
                        poly(q, 4, col);
                    } else {
                        // 斜面，或斜接超过 miter-limit（规范：超限按 bevel 画）
                        const float q[6] = { g.p1x, g.p1y, obx, oby, g.p2x, g.p2y };
                        poly(q, 3, col);
                    }
                } else {
                    dot(obx - half, oby - half, width, width, half, colorAt(obx, oby));
                }
            }
        }
        if (!closed && cap == BINUI_CAP_ROUND) {
            const float ax = path.pts[first * 2], ay = path.pts[first * 2 + 1];
            const float bx = path.pts[(first + cnt - 1) * 2], by = path.pts[(first + cnt - 1) * 2 + 1];
            dot(ax - half, ay - half, width, width, half, colorAt(ax, ay));
            dot(bx - half, by - half, width, width, half, colorAt(bx, by));
        }
    }
    return segs;
}

// 渲染一个 BINUI_PATH 节点的全部几何：归一化点 → 设备折线 → 填充 + 描边。
//
// 三端共用：每端只传三个"怎么画"的回调（条带 / 线 / 圆），几何与颜色顺序只写一份。
// 缓冲由后端持有（PathRenderBuffers），避免每帧分配。
//
// 颜色由调用方先乘好 opacity（GDI 端没有合成，淡出要折进 alpha）；渐变则按色标逐条乘。
// 描边线宽按"节点盒高 / 设计盒高"缩放：归一化点会自动跟随节点盒，线宽必须显式跟，
// 否则等比缩放后"形状变大、线条没变粗"。与边框、字号只跟纵向比例是同一处取舍。
//
// 条带缓存条目（v19）：把"形状 → 条带"的结果按节点缓存起来，避免每帧重算。
//
// 为什么放在这里而不是各后端：条带是后端无关的 POD，缓存也只有一份实现时
// 三端才必然一致。命中条件由调用方给出的 key 决定（节点盒尺寸、颜色、透明度、
// 是否渐变都在 key 里），布局变化时由渲染器整表作废。
struct PathBandCacheEntry {
    uint64_t key = 0;               // 0 = 空（永不命中：编译期 key 从 1 起）
    uint64_t gen = 0;               // 渲染器布局代数（作废判断）
    size_t   accounted = 0;         // 已经计入渲染器全局预算的条带数（记账用）
    std::vector<PaintBand> bands;
};

// 颜料集中成 PathPaintSpec：填充/描边各有"纯色 + 可选渐变"两组，v17 起描边也能带渐变。
struct PathPaintSpec {
    uint32_t fillColor   = 0xFF000000u;
    uint32_t strokeColor = 0xFF000000u;
    const BinUIPathGradient* fillGrad   = nullptr;
    const BinUIPathGradient* strokeGrad = nullptr;
    float opacity = 1.0f;
};

struct PathRenderBuffers {
    std::vector<PathPointF> pts;    // 设备坐标
    std::vector<float> xy;          // PaintPath 需要的 x,y 交错布局
    std::vector<PaintBand> bands;   // 填充条带（含降级重试时的容量）
    std::vector<uint16_t> covAcc;   // v19：AA 的逐像素覆盖率累加缓冲
};

// 条带缓存的 key：形状盒尺寸 + 填充颜料 + 透明度 + 相关 flags（渐变指针也进去，
// 因为换渐变会改颜色）。key==0 表示"不缓存"。
inline uint64_t MakePathBandKey(float x, float y, float w, float h,
                                const PathPaintSpec& spec, uint16_t flags) {
    uint64_t k = 0x9E3779B97F4A7C15ull;
    auto mix = [&](uint64_t v) { k ^= v + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2); };
    auto mixF = [&](float f) {
        uint32_t u = 0;
        std::memcpy(&u, &f, sizeof(u));
        mix(u);
    };
    mixF(x); mixF(y); mixF(w); mixF(h);
    mix(spec.fillColor); mix(spec.strokeColor);
    mixF(spec.opacity);
    mix((uint64_t)(uintptr_t)spec.fillGrad);
    mix((uint64_t)(uintptr_t)spec.strokeGrad);
    mix(flags);
    return k ? k : 1;   // 0 留给"空条目"
}

template <class FillBands, class LineFn, class DotFn>
inline void PaintPathNode(const BinUIPathEntry& head, const uint16_t* normPts,
                          const uint32_t* first, const uint32_t* count,
                          const uint8_t* closed, int contours,
                          float x, float y, float w, float h,
                          const PathPaintSpec& spec,
                          PathBandCacheEntry* cache,   // 可为 nullptr（不缓存）
                          uint64_t cacheGen,
                          size_t cacheBudget,          // 本节点最多还能占多少条带（渲染器给）
                          PathRenderBuffers& buf,
                          FillBands&& fillBands, LineFn&& line, DotFn&& dot) {
    if (!normPts || !first || !count || contours <= 0 || head.pointCount == 0) return;
    if (w <= 0.0f || h <= 0.0f) return;

    const uint32_t total = head.pointCount;
    buf.pts.resize(total);
    buf.xy.resize(static_cast<size_t>(total) * 2);
    DenormPoints(normPts, total, x, y, w, h, buf.pts.data());
    for (uint32_t i = 0; i < total; i++) {
        buf.xy[i * 2] = buf.pts[i].x;
        buf.xy[i * 2 + 1] = buf.pts[i].y;
    }
    const PaintPath view{ buf.xy.data(), first, count, closed, contours };

    if (head.flags & PATH_FILL) {
        // v19：先看缓存 —— 形状、尺寸、颜色、透明度都没变时直接重放上一次的条带。
        // 命不中就算一遍并（在预算内）存下来；单节点超过 kMaxCachedBands 条、或者吃掉
        // 渲染器剩下的全局预算（cacheBudget）就不缓存 —— 少缓存只影响速度，不影响画面。
        const uint64_t key = cache ? MakePathBandKey(x, y, w, h, spec, head.flags) : 0;
        if (cache && cache->key == key && cache->gen == cacheGen && !cache->bands.empty()) {
            fillBands(cache->bands.data(), static_cast<int>(cache->bands.size()));
        } else {
            if (buf.bands.size() < static_cast<size_t>(kMaxPathBands))
                buf.bands.resize(kMaxPathBands);
            const bool evenOdd = (head.flags & PATH_EVENODD) != 0;
            // v16：渐变填充（无渐变时退化成纯色取样器，与老路径完全一致）
            const PathShade shade =
                MakePathShade((head.flags & PATH_FILL_GRAD) ? spec.fillGrad : nullptr,
                              spec.fillColor, spec.opacity, x, y, w, h);
            int n = 0;
            bool truncated = false;
            bool aaDone = false;
            // v19：shape-rendering=auto/optimizeQuality 时走覆盖率 AA；
            // 形状太宽（>kMaxPathAAWidth）或撞上限时退回硬边，保证"画得完"优先。
            if (head.flags & PATH_AA) {
                const int na = BuildPathFillBandsAA(view, evenOdd, x, y, x + w, y + h, shade,
                                                    buf.bands.data(), kMaxPathBands,
                                                    buf.covAcc, &truncated);
                if (na >= 0 && !truncated) {
                    n = na;
                    aaDone = true;
                }
            }
            if (!aaDone) {
                truncated = false;
                // 撞上限：按行步长降级重来（1 → 2 → 4 → 8），保证画完而不是画一半
                for (int step = 1; step <= 8; step *= 2) {
                    n = BuildPathFillBands(view, evenOdd, x, y, x + w, y + h, shade,
                                           buf.bands.data(), kMaxPathBands, step, &truncated);
                    if (!truncated) break;
                }
            }
            if (n > 0) fillBands(buf.bands.data(), n);
            if (cache) {
                // 单节点 16384 条的安全上限 + 渲染器给的全局预算；任何一个不够就不缓存
                // （不缓存只是每帧重算，不影响画对，所以这里是"宁可少缓存"）。
                if (n > 0 && n <= kMaxCachedBands && static_cast<size_t>(n) <= cacheBudget) {
                    cache->bands.assign(buf.bands.data(), buf.bands.data() + n);
                    cache->key = key;
                    cache->gen = cacheGen;
                } else {
                    cache->bands.clear();
                    cache->key = 0;
                }
            }
        }
    }

    if (head.flags & PATH_STROKE) {
        const float k = head.designH > 0.0f ? h / head.designH : 1.0f;
        const float sw = static_cast<float>(head.strokeW) * k;
        if (sw > 0.05f) {
            // v17：渐变描边。色标按设备点取色（量化级与填充同一套），所以每一小段线的
            // 颜色都对得上 —— 折线本来就是细分的，逐段取色就是逐像素取色。
            const PathShade strokeShade =
                MakePathShade((head.flags & PATH_STROKE_GRAD) ? spec.strokeGrad : nullptr,
                              spec.strokeColor, spec.opacity, x, y, w, h);
            auto colorAt = [&](float px, float py) { return strokeShade.At(px, py); };
            // 斜接/斜面的缺口是多边形：复用填充那套条带，三端仍然只画 fillRect。
            //
            // 裁剪盒要**外扩**：节点盒只按"半线宽"外扩了几何盒（SVG 的 bbox 本来就不含描边），
            // 而斜接尖角可以伸出 (ratio-1)·半线宽 —— 不外扩的话尖角会被自己节点的盒子切平，
            // 看起来像被削过的斜面。外扩量取 miter-limit 允许的最大值；
            // 父级裁剪仍然照常生效（SVG 的 viewport 语义）。
            const float limit = head.reserve > 0 ? static_cast<float>(head.reserve) : 4.0f;
            const float pad = limit * sw * 0.5f;
            auto poly = [&](const float* xy, int n, uint32_t color) {
                if (!xy || n < 3) return;
                if (buf.bands.size() < static_cast<size_t>(kMaxPathBands))
                    buf.bands.resize(kMaxPathBands);
                const uint32_t zero = 0;
                const uint32_t cnt = static_cast<uint32_t>(n);
                const uint8_t one = 1;
                const PaintPath pg{ xy, &zero, &cnt, &one, 1 };
                PathShade ps;
                ps.color = color;
                bool tr = false;
                const int bn = BuildPathFillBands(pg, false, x - pad, y - pad, x + w + pad,
                                                  y + h + pad, ps, buf.bands.data(),
                                                  kMaxPathBands, 1, &tr);
                if (bn > 0) fillBands(buf.bands.data(), bn);
            };
            PaintPathStroke(view, sw, head.cap, head.join, head.reserve, colorAt,
                            line, dot, poly);
        }
    }
}

} // namespace binui
