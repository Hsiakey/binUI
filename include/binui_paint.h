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
#include <unordered_map>
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

// 渐变插值（v0.16.38）：**在预乘空间插值**，与 CSS Color 4 / 浏览器一致。
// 为什么必须单独一个函数：渐变两端 alpha 不同时，"先按直色插值再预乘"（老行为）和
// "先预乘再插值"（CSS）结果差很远 —— rgba(255,120,80,1) → transparent 的中点在
// CSS 里是"同一个颜色、alpha 0.5"，老行为却是"颜色也一起变暗"（正中 ≈ (72,40,32)）。
// 两端 alpha **相同**时两者数学等价（只差整数取整），所以那一支刻意走老函数 ——
// 既有像素基准一位不变，只有真正需要修的那类渐变才会变。
inline uint32_t PaintLerpArgbGrad(uint32_t a, uint32_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    const uint32_t aa = (a >> 24) & 0xFFu, ba = (b >> 24) & 0xFFu;
    if (aa == ba) return PaintLerpArgb(a, b, t);          // 等价分支：逐位不变
    const float af = (float)aa + ((float)ba - (float)aa) * t;   // 插值后的 alpha
    const uint32_t oa = ((uint32_t)(af + 0.5f)) & 0xFFu;
    if (oa == 0u) return 0u;                              // 全透明：RGB 已无意义
    auto ch = [&](int shift) -> uint32_t {
        // 预乘端（0..255 尺度）→ 线性插值 → 除回直色
        const float ca = (float)((a >> shift) & 0xFF) * (float)aa / 255.0f;
        const float cb = (float)((b >> shift) & 0xFF) * (float)ba / 255.0f;
        float v = (ca + (cb - ca) * t) * 255.0f / af;
        if (v < 0.0f) v = 0.0f;
        if (v > 255.0f) v = 255.0f;
        return ((uint32_t)(v + 0.5f)) & 0xFFu;
    };
    return (oa << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// 渐变条带的抖动版：alpha 不同的那一支**不抖**（误差扩散定义在直色通道上，
// 换成预乘空间后"带误差的颜色"已经不是同一个语义，硬抖反而会出现彩边）。
// 这类渐变的条带通常只有几十条（淡出），抖动收益本来也看不到。
inline uint32_t PaintLerpArgbGradDither(uint32_t a, uint32_t b, float t,
                                        float& errR, float& errG, float& errB) {
    if ((((a >> 24) ^ (b >> 24)) & 0xFFu) != 0u)
        return PaintLerpArgbGrad(a, b, t);
    return PaintLerpArgbDither(a, b, t, errR, errG, errB);
}

// position: sticky 的纵向钳制（v0.16.39）。参数全是**屏幕坐标**，返回"额外位移"：
//   0 = 不动；正 = 向下平移（元素被滚出视口上沿 → 钉在边上）；负 = 向上平移（父容器滚走时被带走）。
//
//   naturalTop        元素本该在的屏幕 y（已含祖先滚动偏移）
//   h                 元素高
//   portTop/Bottom    最近的可滚动祖先的视口上下沿（屏幕）
//   blockBottom       包含块（父节点盒）的下沿（屏幕）—— 越过它就被父容器带走
//   insetTop          `top` 偏移（px；百分比由调用方先折算）
//
// 为什么只钳纵向：桌面 UI 里 99% 的 sticky 是"表头/分组标题钉在列表顶部"，横向 sticky
// （左列钉住）需求极少，留到真有人要时再补 —— 少一个维度就少一半边角情形（见编译器告警）。
inline float PaintStickyShiftY(float naturalTop, float h,
                               float portTop, float portBottom,
                               float blockBottom, float insetTop) {
    (void)portBottom;                             // 只用来算 insetTop 的百分比（调用方折算）
    const float pinTop = portTop + insetTop;      // 钉住后的上沿
    const float lowest = blockBottom - h;         // 不能越过包含块下沿（被父容器带走）
    float t = naturalTop;
    if (t < pinTop) t = pinTop;                   // 滚过上沿 → 钉住
    if (t > lowest) t = lowest;                   // 父容器滚走 → 被带走（CSS：永不离开包含块）
    return t - naturalTop;
}

// ---- CSS filter: blur(Npx)（v0.16.42）----
// 上限 64px：底下那条降采样链最多 6 级（2^6 = 64），再大的半径只是把已经糊成一片的
// 画面继续糊，却要多扫几遍整块区域。编译器按这个值钳制并告警。
inline constexpr float kMaxFilterBlurPx = 64.0f;

// 模糊会**溢出元素盒子**：离屏图层与合成矩形都要外扩这么多（像素，已向上取整）。
// 3σ 是高斯"肉眼看不到尾巴"的经验距离；降采样链的等效 σ ≈ 半径，与 box-shadow 的
// 3σ 取法同一个口径。返回整数：图层框与合成四边形都走整像素，三端才能逐像素一致。
inline int PaintFilterBlurPad(float blurPx) {
    if (!(blurPx > 0.0f)) return 0;
    const float pad = std::ceil(blurPx * 3.0f);
    return static_cast<int>(pad > kMaxFilterBlurPx * 3.0f ? kMaxFilterBlurPx * 3.0f : pad);
}

// ---- 分离高斯（v0.16.44）----
// 模糊的执行方式（三个后端共用这一份规则）：
//   1) 先按 2× 逐级降采样，降到"σ 还剩 >= 2 纹素"的那一级就停；
//   2) 在那一级上跑横/纵两趟加权卷积（每个纹素一个抽头，加法混合累加）。
// 为什么不再"一路降到底"：最粗那一级在"大半径 + 小元素"时只剩 5~7 个纹素，形状信息在那一步
// 就丢了，双线性放大回来是一条斜坡 + 折点（肉眼就是梯形/平台；14px 的圆点糊出来是个十字）。
// 口径：σ = 半径（CSS `blur(N)` / `drop-shadow(0 0 N)` 的定义），所以外扩恰好是 3σ = 3N，
// 与上面的 PaintFilterBlurPad 一致。
//
// 0 级 = 不降采样（合成时把抓下来的区域 1:1 拷回去）：σ 很小时用它，省一趟填充。
// v0.16.44 曾把最小级数定成 1，用来把"1× 层里圆角图片"的一处三端差异摊平；v0.16.45 修掉
// 了那个差异的真正原因（VK 图层用了 4× MSAA 主管线），所以 0 级重新放开。
inline int PaintFilterBlurLevel(float blurPx) {
    if (!(blurPx > 0.0f)) return 0;
    int levels = 0;
    while (levels < 5 && blurPx / static_cast<float>(1 << (levels + 1)) >= 2.0f) levels++;
    return levels;
}

// 抽头权重：k = -3σ..+3σ，**每个纹素一个**（按 ±kσ 布会留 σ 宽的空档，卷积核变梳状、
// 输出带栅栏）。返回抽头个数（奇数），权重写入 w[0..n-1] 且**和正好 255** —— 量化误差补在
// 中心抽头上，否则总亮度会随半径漂移、三端还各漂各的。
inline int PaintFilterGaussTaps(float sigmaTexels, unsigned* w, int maxN) {
    if (!(sigmaTexels > 0.0f) || !w || maxN < 3) return 0;
    int r = static_cast<int>(std::ceil(3.0f * sigmaTexels));
    if (r < 1) r = 1;
    if (2 * r + 1 > maxN) r = (maxN - 1) / 2;
    const int n = 2 * r + 1;
    float f[64];
    float sum = 0.0f;
    for (int k = -r; k <= r; k++) {
        const float t = static_cast<float>(k) / sigmaTexels;
        f[k + r] = std::exp(-0.5f * t * t);
        sum += f[k + r];
    }
    int acc = 0;
    for (int i = 0; i < n; i++) {
        w[i] = static_cast<unsigned>(f[i] / sum * 255.0f + 0.5f);
        acc += static_cast<int>(w[i]);
    }
    w[r] = static_cast<unsigned>(static_cast<int>(w[r]) + (255 - acc));
    return n;
}

// 一个节点的离屏图层要外扩多少（v0.16.43）—— 把**所有**会画到盒子外面的效果叠起来：//   · filter: blur(N)      → 3σ（上面的 PaintFilterBlurPad）
//   · filter: drop-shadow  → 它自己的 3σ + 偏移的绝对值（阴影整体挪出去了）
//     （box-shadow 不走图层，它的外扩在 PaintShadowPad / invalidRect 里单独算）
// 参数传的是"这个节点自身"的值；后代的图层不需要祖先的外扩，因为祖先的层本来就更大。
inline int PaintLayerPad(float blurPx, const float* shadowDx, const float* shadowDy,
                         const float* shadowBlur, int shadowCount) {
    int pad = PaintFilterBlurPad(blurPx);
    for (int i = 0; i < shadowCount; i++) {
        int p = PaintFilterBlurPad(shadowBlur[i]);
        const int ox = static_cast<int>(std::ceil(std::fabs(shadowDx[i])));
        const int oy = static_cast<int>(std::ceil(std::fabs(shadowDy[i])));
        p += ox > oy ? ox : oy;
        if (p > pad) pad = p;
    }
    return pad;
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

// 节点（含阴影等"画到盒子外面"的外扩 pad）是否与裁剪矩形**相交**。
// 判据只允许一种写法：**四边都"完全在外"才算剔除**。
//   右：x + w + padX <= clipL（节点整个在裁剪区左边）
//   左：x - padX >= clipR（整个在右边）
//   下：y + h + padY <= clipT（整个在上边）
//   上：y - padY >= clipB（整个在下边）
//
// 为什么值得单独立一个函数：v0.16.31 及以前，绘制遍历里把右/下两条写成了
// `x + padX >= clipR` / `y + padY >= clipB` —— 那判的是"节点的左/上边**进入**了
// 裁剪区右/下边 pad 以内"，于是**紧贴裁剪边缘、明明有一部分可见**的节点被整块丢掉。
// 整帧重绘时裁剪区 = 整屏，四边的差别碰不到任何节点，所以像素基准一直没抓到；
// 一旦走局部重绘（hover 只重画一个节点的脏矩形），脏矩形边缘那一圈兄弟节点的
// 底色/阴影就凭空消失，沿着脏矩形留下一圈硬边 —— 看起来像"hover 之后多出一块影子/边框"。
// 这类判据必须只有一份实现 + 单测（tests/test_elements.cpp 的"局部重绘剔除"小节）。
inline bool PaintNodeInClip(float x, float y, float w, float h,
                            float padX, float padY,
                            float clipL, float clipT, float clipR, float clipB) {
    if (x + w + padX <= clipL) return false;
    if (x - padX >= clipR) return false;
    if (y + h + padY <= clipT) return false;
    if (y - padY >= clipB) return false;
    return true;
}

// box-shadow 会画到盒子外面：外扩量 = |spread| + 3σ + 1（与 BuildShadowRings 的裁断距离
// 一致），再叠上偏移。**这一份必须只有一处实现** —— 绘制（决定裁剪/剔除）和
// 失效（决定脏矩形多大）用的是同一组数字，两边算不一样就会出现"重绘没盖住阴影"
// 的残影（v0.16.32 之前 invalidRect 完全没算阴影，局部重绘后按钮阴影边缘留一圈旧像素）。
inline void PaintShadowPad(const BinUIShadow& s, float scale, float& padX, float& padY) {
    const float sig = s.blur > 0.0f ? s.blur * 0.5f : 0.0f;
    const float pad = (std::fabs(s.spread) + sig * 3.0f + 1.0f) * scale;
    padX = std::max(padX, pad + std::fabs(s.offsetX) * scale);
    padY = std::max(padY, pad + std::fabs(s.offsetY) * scale);
}

// text-shadow 同理：字形模糊会溢出节点盒（CSS 的 text-shadow 不受盒子边界约束）。
inline void PaintTextShadowPad(const BinUITextShadow& t, float& padX, float& padY) {
    const float sig = t.blur > 0.0f ? t.blur * 0.5f : 0.0f;
    const float pad = sig * 3.0f + 1.0f;
    padX = std::max(padX, pad + std::fabs(t.offsetX));
    padY = std::max(padY, pad + std::fabs(t.offsetY));
}

// 按比例缩放颜色的 alpha（用于边缘部分覆盖：抗锯齿）
inline uint32_t PaintScaleAlpha(uint32_t c, float k) {    if (k >= 0.999f) return c;
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
                uint32_t col = PaintLerpArgbGradDither(p->gradFrom, p->gradTo, t, dithR, dithG, dithB);
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
        bd.color = PaintLerpArgbGradDither(p->gradFrom, p->gradTo, t, dithR, dithG, dithB);
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
// ============================================================
// 任意角度渐变（v30）：渐变轴 + 四角带色几何
// ------------------------------------------------------------
// 0.16.13 起任意角度渐变是"逐行 1px 条带 + 沿 x 自适应切段"，一张 320×180 的 135°
// 卡片实测约 3.7k 条。条带的锅在于**表示**：颜色是逐条恒定的纯色矩形，只能靠"切得
// 足够细"逼近渐变。而线性渐变在屏幕空间就是仿射函数，仿射函数被三角形上的线性插值
// 精确复现 —— 一个四边形（两个三角形）+ 四角带色就是精确解。
//
// 于是这里给出两条平行实现，由调用方按后端能力选：
//   · BuildFillBandsAngleGrad —— 条带（GDI 遗留路径、圆角模板缺失时的兜底）；
//   · BuildGradientQuads      —— 四角带色（GL / Vulkan / D3D11）。
// 两者几何口径完全一样（同一个 PaintGradAxisOf），所以盒子/角度不会两边算不一样。
// ============================================================

// CSS linear-gradient 的角度 → 渐变轴。0deg = to top（屏幕 y 向下），90deg = to right；
// 规范里渐变线过盒中心、长度 = |w·dx| + |h·dy|，于是四角正好投影到两端。
// 顺带给出 t=0 端的起点 (sx,sy)，调用方算 t 时不用再减中心。
inline void PaintGradAxisOf(float angleDeg, float x, float y, float w, float h,
                            float& dx, float& dy, float& len, float& sx, float& sy) {
    const float kPi = 3.14159265358979323846f;
    const float th = angleDeg * (kPi / 180.0f);
    dx = std::sin(th);
    dy = -std::cos(th);
    len = std::fabs(w * dx) + std::fabs(h * dy);
    sx = x + w * 0.5f - dx * len * 0.5f;
    sy = y + h * 0.5f - dy * len * 0.5f;
}

// 与三个后端各自那份 Premult 同一公式（整数、(c*a)/255）。放在这里算一次，
// 是为了让"四角带色"这条路在三端写出来的顶点颜色逐字节相同。
inline uint32_t PaintGradPremulColor(uint32_t c) {
    const uint32_t a = (c >> 24) & 0xFFu;
    if (a == 255u) return c;
    if (a == 0u) return 0u;
    const uint32_t r = (((c >> 16) & 0xFFu) * a) / 255u;
    const uint32_t g = (((c >> 8) & 0xFFu) * a) / 255u;
    const uint32_t b = ((c & 0xFFu) * a) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// 屏幕上一点 (px,py) 处的渐变颜色，返回**预乘色**（四角带色四边形直接用）。
// v0.16.38：两端 alpha 不同时在**预乘空间**插值（CSS Color 4），且这正是顶点色要的东西 ——
// GPU 在三角形上插值的本来就是预乘通道，于是"淡出渐变"也是一块精确的四边形，
// 不再需要为了插值空间不同而退回条带（见 BuildGradientQuads）。
// 两端 alpha 相同的分支刻意保持老写法（逐位不变），既有像素基准不受影响。
inline uint32_t PaintGradColorAtPoint(float px, float py, float dx, float dy, float len,
                                      float sx, float sy, uint32_t from, uint32_t to) {
    float t = len > 1e-6f ? (((px - sx) * dx + (py - sy) * dy) / len) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    if ((((from >> 24) ^ (to >> 24)) & 0xFFu) == 0u)
        return PaintGradPremulColor(PaintLerpArgb(from, to, t));
    const uint32_t p0 = PaintGradPremulColor(from), p1 = PaintGradPremulColor(to);
    auto ch = [&](int shift) -> uint32_t {
        const float ca = (float)((p0 >> shift) & 0xFF);
        const float cb = (float)((p1 >> shift) & 0xFF);
        return ((uint32_t)(ca + (cb - ca) * t + 0.5f)) & 0xFFu;
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

inline int BuildFillBandsAngleGrad(uint32_t from, uint32_t to, float angleDeg,
                                   float x, float y, float w, float h, float radius,
                                   PaintBand* out, int cap) {
    if (!out || cap <= 0 || w <= 0.0f || h <= 0.0f) return 0;
    float dx = 0.0f, dy = 0.0f, len = 0.0f, sx = 0.0f, sy = 0.0f;
    PaintGradAxisOf(angleDeg, x, y, w, h, dx, dy, len, sx, sy);
    if (len <= 1e-4f) {                          // 退化：两端色直接铺平
        out[0] = { x, y, w, h, PaintLerpArgbGrad(from, to, 0.5f), 0.0f, false };
        return 1;
    }
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
            const uint32_t col = PaintLerpArgbGrad(from, to, t);
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
// 可以忽略（GDI 渲染后端已于 0.24.0 移除，那段按色去重也随之删除）。
//
// 两个字段的用法不同，别混：
//   · GPU（GL/VK/D3D11）走真 alpha 混合，环与环是"叠加"关系：直接用 cover 会在
//     **重叠区叠深**（形状内部的像素被每一环都盖一次，0.30 的阴影最后变成 0.5）。
//     所以 color 里预先做了累计校正 a_i = (T_i−T_{i−1})/(1−T_{i−1})，逐环源覆盖之后
//     正好得到 T_i = A×cover_i（多个阴影叠加时也自然是 1−(1−a)(1−b)）；
//   · （0.24.0 之前 GDI 渲染后端没有 alpha 合成，它另走一份"环 → 预乘位图"的
//     路径；该后端已移除，这里只留 GPU 的累计校正那一条。）
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

// ============================================================
// 圆角图片（v0.16.15 P1-8）：把"圆角矩形内的图片"切成横向细条
// ------------------------------------------------------------
// 后端没有"圆角裁剪"：`fillRounded` 只能填色、`drawImage` 只能画轴对齐整图。
// 但圆角矩形的横截面是**每行一个左右内缩**（`PaintRoundInset` 的解析式圆弧），
// 所以把图片按行切成细条、每条只画该行的 [内缩, w-内缩] 区间，并取源图对应的一行
// UV —— 形状就对了。条带仍是轴对齐矩形（后端只要支持 UV 子矩形 + 顶点色），
// 边缘用后端的 MSAA 抗锯齿，不需要新原语、也不需要改着色器。
//
// 条带数：上下两个圆角区各 r 行（1px 一行），中间直边一条 —— 只跟半径有关，
// 与图片高度无关（100px 高的头像 1 条，正圆头像 = 2r+1 条）。
// 超过上限（超大圆形）时按比例放大行高，保证画完而不是画一半。
// ============================================================
struct PaintImageBand {
    float x = 0, y = 0, w = 0, h = 0;      // 目标矩形（已按圆弧内缩）
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // 源图归一化子矩形
};

constexpr int kMaxImageBands = 512;

inline int BuildImageBands(float x, float y, float w, float h, float radius,
                           PaintImageBand* out, int cap = kMaxImageBands) {
    if (!out || cap <= 0 || w <= 0.0f || h <= 0.0f) return 0;
    float r = radius;
    if (r > 0.0f) r = std::min(r, std::min(w, h) * 0.5f);
    if (r <= 0.5f || cap == 1) {                 // 直角：整图一条（像素中性）
        out[0] = { x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f };
        return 1;
    }
    const float rc = r;                          // 钳制后的半径
    // 行数超上限时放大行高（宁可边缘粗一点，也不能只画一半）
    const int rowsNeeded = static_cast<int>(2.0f * std::ceil(rc)) + 1;
    const float step = (cap > 1 && rowsNeeded > cap) ? (2.0f * rc / static_cast<float>(cap - 1))
                                                    : 1.0f;
    int n = 0;
    float cur = y;
    const float bottom = y + h;
    while (cur < bottom - 0.001f && n < cap) {
        const float by0 = cur;
        float ht = std::min(step, bottom - cur);
        // 圆角区细分，直边区一条走到底
        if (by0 >= y + rc - 0.001f && by0 + ht <= bottom - rc + 0.001f)
            ht = bottom - rc - by0;
        if (ht <= 0.001f) ht = std::min(step, bottom - cur);
        const float by1 = by0 + ht;
        // 用行中心算内缩（与渐变底色同一套口径，圆弧才不会切出台阶）
        const float dyTop = (by0 + by1) * 0.5f - y;
        const float dyBot = bottom - (by0 + by1) * 0.5f;
        const float in = std::max(PaintRoundInset(std::min(dyTop, rc), rc),
                                  PaintRoundInset(std::min(dyBot, rc), rc));
        const float bx = x + in;
        const float bw = w - 2.0f * in;
        if (bw > 0.0f) {
            PaintImageBand& b = out[n++];
            b.x = bx;
            b.y = by0;
            b.w = bw;
            b.h = by1 - by0;
            // UV 跟着内缩走：图片仍然"按整个盒子缩放"，被圆角**裁掉**的是两侧那部分，
            // 所以这一行要取源图 [in/w, 1-in/w] 这一段（CSS 的 border-radius 就是这个语义；
            // 若这里恒取 0..1，圆角区的图片会被横向拉伸，看起来像"鱼眼"）。
            b.u0 = in / w;
            b.u1 = 1.0f - in / w;
            b.v0 = (by0 - y) / h;
            b.v1 = (by1 - y) / h;
        }
        cur = by1;
    }
    if (n == 0) {                                 // 防御：一条都没生成就画整图
        out[0] = { x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f };
        return 1;
    }
    return n;
}

// ============================================================
// 九宫格切分（v25）：把"源图 + 目标盒 + 内缩"算成最多 9 个四边形。
// 输出 out[i] = { u0, v0, u1, v1, x, y, w, h }（UV 归一化、坐标是屏幕像素），返回条数。
// 三个 GPU 后端共用这一份数学 → 九宫格的像素必须一致。
//
// 内缩会被**两端同时**钳制：源图方向上不超过半张图（两块角不能重叠），
// 目标盒方向上不超过半个盒（否则中间那块会被拉成负宽）。
// ============================================================
// 九宫格中段的一个"块"：目标轴上的起止 + 源 UV 上的分数起止
// （repeat 的末块要裁切，所以源区间不能总是 0..1）。
struct PaintNineTile {
    float t0 = 0.0f, t1 = 0.0f;   // 目标轴（屏幕像素）
    float f0 = 0.0f, f1 = 1.0f;   // 源 UV 的分数（相对这一条切片）
};

// 单轴最多切几块。超过就退成"等分"（等价 round）—— 一个 300px 的框里塞几百块既
// 没意义又会把顶点预算打满（这条是预算护栏，不是审美取舍）。
constexpr int kNineMaxTiles = 32;
// 一次九宫格最多输出几个四边形（两端都平铺时是"四角 + 两向边 + 中心"的乘积）：
// 调用方按这个值准备缓冲（256 × 8 × 4B = 8 KB，栈上够用），超了按比例压缩块数。
constexpr int kMaxNineSliceQuads = 256;

// 把目标轴区间 [a, b) 按 mode 切块（0=stretch 1=repeat 2=round 3=space），写进 out，返回块数。
inline int SplitNineAxis(int mode, float a, float b, float natural,
                         PaintNineTile* out, int maxTiles) {
    const float len = b - a;
    if (len <= 0.0f || out == nullptr || maxTiles <= 0) return 0;
    if (mode != 0 && natural > 0.5f) {
        if (mode == 2 || mode == 3) {                           // round / space：块数取整
            int cnt = static_cast<int>(mode == 2 ? std::lround(len / natural)
                                                 : std::floor(len / natural + 1e-4f));
            if (cnt < 1) cnt = 1;
            if (cnt > maxTiles) cnt = maxTiles;
            if (mode == 2) {                                    // round：块数取整后等分
                const float tw = len / static_cast<float>(cnt);
                for (int i = 0; i < cnt; i++) {
                    out[i].t0 = a + static_cast<float>(i) * tw;
                    out[i].t1 = out[i].t0 + tw;
                    out[i].f0 = 0.0f;
                    out[i].f1 = 1.0f;
                }
                return cnt;
            }
            // space：块尺寸 = 自然尺寸（**不缩放、不裁切**），余数均分成 cnt+1 个间隙 ——
            // 两端各一个、块间一个，全部等宽（CSS 的原话是 "the extra space is distributed
            // around the tiles"，所以端头也有间隙，而不是首末块贴边）。
            // 只有一种例外：中段比一块还窄（cnt = 1 且 natural > len）—— 这时候没有"余数"
            // 可均分，只能把这一块缩到区间长（等价 stretch 一块）。CSS 对 space 的要求是
            // "不被裁切"，缩到刚好放得下正是这个意思。
            float tw = natural;
            if (cnt == 1 && natural > len) tw = len;
            const float gap = (len - static_cast<float>(cnt) * tw) / static_cast<float>(cnt + 1);
            for (int i = 0; i < cnt; i++) {
                out[i].t0 = a + gap + static_cast<float>(i) * (tw + gap);
                out[i].t1 = out[i].t0 + tw;
                out[i].f0 = 0.0f;
                out[i].f1 = 1.0f;
            }
            return cnt;
        }
        // repeat：按自然尺寸铺，末块裁到区间边界（源 UV 也按同一比例裁）
        const int full = static_cast<int>(std::floor(len / natural + 1e-4f));
        const float rest = len - static_cast<float>(full) * natural;
        const int need = full + (rest > 0.01f ? 1 : 0);
        if (need <= maxTiles) {
            int n = 0;
            for (int i = 0; i < full; i++) {
                out[n].t0 = a + static_cast<float>(i) * natural;
                out[n].t1 = out[n].t0 + natural;
                out[n].f0 = 0.0f;
                out[n].f1 = 1.0f;
                n++;
            }
            if (rest > 0.01f) {
                out[n].t0 = a + static_cast<float>(full) * natural;
                out[n].t1 = b;
                out[n].f0 = 0.0f;
                out[n].f1 = rest / natural;
                n++;
            }
            return n;
        }
        // 预算不够：退成等分（块被拉宽，看起来接近 round）
        const float tw = len / static_cast<float>(maxTiles);
        for (int i = 0; i < maxTiles; i++) {
            out[i].t0 = a + static_cast<float>(i) * tw;
            out[i].t1 = out[i].t0 + tw;
            out[i].f0 = 0.0f;
            out[i].f1 = 1.0f;
        }
        return maxTiles;
    }
    out[0].t0 = a;
    out[0].t1 = b;
    out[0].f0 = 0.0f;
    out[0].f1 = 1.0f;
    return 1;
}

// 把区间均分成 cnt 块（预算压缩时用：块被拉宽，看起来接近 round）
inline int SplitNineAxisCount(float a, float b, int cnt, PaintNineTile* out) {
    const float len = b - a;
    if (len <= 0.0f || cnt <= 0 || out == nullptr) return 0;
    const float tw = len / static_cast<float>(cnt);
    for (int i = 0; i < cnt; i++) {
        out[i].t0 = a + static_cast<float>(i) * tw;
        out[i].t1 = out[i].t0 + tw;
        out[i].f0 = 0.0f;
        out[i].f1 = 1.0f;
    }
    return cnt;
}

// 九宫格切分：out 每 8 个 float 一个四边形（u0,v0,u1,v1,x,y,w,h），返回块数。
// repX / repY 是两轴的填充模式（见 BinUISliceRepeat）；都为 0 时与老行为**逐字节相同**
// （块的顺序也照旧：行优先 r×c）。
inline int BuildNineSlice(float x, float y, float w, float h, int srcW, int srcH,
                          float slice, int repX, int repY, float* out, int cap) {
    if (out == nullptr || cap <= 0) return 0;
    if (w <= 0.0f || h <= 0.0f || srcW <= 0 || srcH <= 0) return 0;
    float sx = slice;
    if (sx > static_cast<float>(srcW) * 0.5f) sx = static_cast<float>(srcW) * 0.5f;
    if (sx > static_cast<float>(srcH) * 0.5f) sx = static_cast<float>(srcH) * 0.5f;
    float dx = slice;
    if (dx > w * 0.5f) dx = w * 0.5f;
    if (dx > h * 0.5f) dx = h * 0.5f;
    if (sx <= 0.0f || dx <= 0.0f) return 0;

    // 源图的 4 条竖线 / 横线（UV）与目标盒的 4 条线（像素）
    const float su[4] = { 0.0f, sx / static_cast<float>(srcW), 1.0f - sx / static_cast<float>(srcW), 1.0f };
    const float sv[4] = { 0.0f, sx / static_cast<float>(srcH), 1.0f - sx / static_cast<float>(srcH), 1.0f };
    const float tx[4] = { x, x + dx, x + w - dx, x + w };
    const float ty[4] = { y, y + dx, y + h - dx, y + h };

    // 两条轴各自的"带"：0 = 角/边（永远一块），1 = 中段（按模式切）
    PaintNineTile bx[3][kNineMaxTiles];
    PaintNineTile by[3][kNineMaxTiles];
    int nbx[3] = { 1, 1, 1 }, nby[3] = { 1, 1, 1 };
    bx[0][0] = { tx[0], tx[1], 0.0f, 1.0f };
    bx[2][0] = { tx[2], tx[3], 0.0f, 1.0f };
    by[0][0] = { ty[0], ty[1], 0.0f, 1.0f };
    by[2][0] = { ty[2], ty[3], 0.0f, 1.0f };
    nbx[1] = SplitNineAxis(repX, tx[1], tx[2], dx, bx[1], kNineMaxTiles);
    nby[1] = SplitNineAxis(repY, ty[1], ty[2], dx, by[1], kNineMaxTiles);
    // 预算：两端都平铺时块数是乘积 —— 超了就按比例压缩块数（退成等分，别把上限撑爆）
    {
        const int budget = cap;
        int nx = nbx[1] > 0 ? nbx[1] : 1;
        int ny = nby[1] > 0 ? nby[1] : 1;
        bool shrunk = false;
        while ((nx + 2) * (ny + 2) > budget && (nx > 1 || ny > 1)) {
            if (nx >= ny && nx > 1) nx--;
            else if (ny > 1) ny--;
            else nx--;
            shrunk = true;
        }
        if (shrunk) {
            nbx[1] = SplitNineAxisCount(tx[1], tx[2], nx, bx[1]);
            nby[1] = SplitNineAxisCount(ty[1], ty[2], ny, by[1]);
        }
    }

    int n = 0;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (nby[r] <= 0 || nbx[c] <= 0) continue;
            for (int yi = 0; yi < nby[r]; yi++) {
                for (int xi = 0; xi < nbx[c]; xi++) {
                    const PaintNineTile& ht = bx[c][xi];
                    const PaintNineTile& vt = by[r][yi];
                    if (ht.t1 - ht.t0 <= 0.0f || vt.t1 - vt.t0 <= 0.0f) continue;
                    if (n >= cap) return n;
                    float* o = out + n * 8;
                    o[0] = su[c] + (su[c + 1] - su[c]) * ht.f0;
                    o[1] = sv[r] + (sv[r + 1] - sv[r]) * vt.f0;
                    o[2] = su[c] + (su[c + 1] - su[c]) * ht.f1;
                    o[3] = sv[r] + (sv[r + 1] - sv[r]) * vt.f1;
                    o[4] = ht.t0;
                    o[5] = vt.t0;
                    o[6] = ht.t1 - ht.t0;
                    o[7] = vt.t1 - vt.t0;
                    n++;
                }
            }
        }
    }
    return n;
}

// 老调用点（单测、内部工具）：三段拉伸，够放九块
inline int BuildNineSlice(float x, float y, float w, float h, int srcW, int srcH,
                          float slice, float out[9][8]) {
    return BuildNineSlice(x, y, w, h, srcW, srcH, slice, 0, 0, &out[0][0], 9);
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

// 圆角模板的**内容**（v0.23.0）：写 t×t 个 0xAArrggbb，四个通道都等于覆盖率
// （三端模板纹理是灰度 + alpha，用同一个值填满四个通道）。返回 t（= RoundTemplateSize），
// 0 = 失败（调用方退回 fillRect / 空模板）。
//
// 内容来自 bin2d（`bin2d::RasterizeRoundedTileCoverage`）：以前 GL / VK / D3D11 **各写一份**
// 一模一样的解析式距离循环，任何一份改了（边长、内缩、"边界骑在像素中间还是贴在里侧"）
// 三端圆角就差一圈像素 —— 与 P2 的填充/描边是同一类漂移，所以同样收口。实现在
// src/backends/binui_paint.cpp（唯一依赖 bin2d 的翻译单元）。
int BuildRoundedTileRGBA(int radius, bool stroke, uint32_t* out);

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

// ------------------------------------------------------------
// 圆角图层（v0.25.0）：填充 / 描边 / 仿射圆角共用的那一层循环
//
// 缺口在哪：圆角模板是 **1px 环**，粗描边 = 同心环叠层（每层往里缩 1px、半径减 1）。
// 这段循环以前 GL / VK / D3D11 各写一份，结果 VK 那份把 thickness 丢了（>1 也只有
// 1px），而 D3D11 又把九宫格 UV 手抄了一遍。收口到这里之后，三端都只负责
// "把某一层的九块发出去"，几何与层数只有一个来源。
//
// 为什么分"层数 + 取一层"而不是一次算完：层数上界是短边的一半（400+ 层 × 9 块），
// 一次算完要几百 KB；后端本来就是逐层取模板纹理的。
// ------------------------------------------------------------
struct RoundedLayer {
    float x = 0, y = 0, w = 0, h = 0;  // 这一层的外框
    float r = 0;                        // 几何半径（浮点：缩放动画里半径要连续）
    int tileRadius = 1;                 // 角片模板的整数半径（模板是位图，只能整数）
};

// 0 = 半径无效（调用方退回 fillRect）。填充恒为 1 层；描边 = round(max(1,thickness))，
// 再被"半径耗尽"限制住。
inline int RoundedLayerCount(float w, float h, float radius, bool stroke, float thickness) {
    if (w <= 0.0f || h <= 0.0f) return 0;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) return 0;
    if (!stroke) return 1;
    int layers = static_cast<int>(std::lround(std::max(1.0f, thickness)));
    const int radiusCap = static_cast<int>(std::floor(rf));
    if (layers > radiusCap) layers = radiusCap;
    return layers < 1 ? 0 : layers;
}

// 第 layer 层；false = 这一层已退化（半径耗尽 / 外框不足 1px），调用方应当停。
inline bool BuildRoundedLayer(float x, float y, float w, float h, float radius, bool stroke,
                              int layer, RoundedLayer& out) {
    if (w <= 0.0f || h <= 0.0f || layer < 0) return false;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) return false;
    if (!stroke) {
        if (layer != 0) return false;
        out.x = x;
        out.y = y;
        out.w = w;
        out.h = h;
        out.r = rf;
        out.tileRadius = std::max(1, static_cast<int>(std::lround(rf)));
        return true;
    }
    const float k = static_cast<float>(layer);
    out.x = x + k;
    out.y = y + k;
    out.w = w - 2.0f * k;
    out.h = h - 2.0f * k;
    if (out.w <= 1.0f || out.h <= 1.0f) return false;
    out.r = std::min(rf - k, std::min(out.w, out.h) * 0.5f);
    if (out.r <= 0.0f) return false;
    out.tileRadius = std::max(1, static_cast<int>(std::lround(out.r)));
    return true;
}

// 一层 → 九块（tileSize = 后端为该层取到的模板边长，必须 = RoundTemplateSize(tileRadius)）
inline int BuildRoundedLayerQuads(const RoundedLayer& L, int tileSize, PaintQuad* out,
                                  int cap = 9) {
    return BuildRoundedNineSlice(L.x, L.y, L.w, L.h, L.r, tileSize, out, cap);
}

// 仿射路径：一层 → 九块的四角坐标 + UV（pos/uv 都是 4 个顶点 × 2 分量）
struct RoundedQuadXf {
    float pos[8];
    float uv[8];
};

inline int BuildRoundedLayerQuadsXf(const RoundedLayer& L, int tileSize, const float xf[6],
                                    RoundedQuadXf* out, int cap = 9) {
    if (!out || cap <= 0) return 0;
    PaintQuad q[9];
    const int n = BuildRoundedNineSlice(L.x, L.y, L.w, L.h, L.r, tileSize, q, 9);
    const int m = n < cap ? n : cap;
    for (int i = 0; i < m; i++) {
        const PaintQuad& c = q[i];
        const float corners[8] = { c.x,     c.y,     c.x + c.w, c.y,
                                   c.x,     c.y + c.h, c.x + c.w, c.y + c.h };
        RoundedQuadXf& o = out[i];
        for (int v = 0; v < 4; v++) {
            const float lx = corners[v * 2], ly = corners[v * 2 + 1];
            o.pos[v * 2] = xf[0] * lx + xf[1] * ly + xf[2];
            o.pos[v * 2 + 1] = xf[3] * lx + xf[4] * ly + xf[5];
        }
        o.uv[0] = c.u0; o.uv[1] = c.v0;
        o.uv[2] = c.u1; o.uv[3] = c.v0;
        o.uv[4] = c.u0; o.uv[5] = c.v1;
        o.uv[6] = c.u1; o.uv[7] = c.v1;
    }
    return m;
}

// 盒子 → 1 个（直角）或 9 个（圆角）四角带色四边形。
// tpl = 后端圆角模板的边长（RoundTemplateSize(r)）；直角时忽略。
//
// v0.16.38：**两端 alpha 不同也能走这条路**（以前返回 0 交回条带）。前提是条带/路径/
// GDI 一起换成"在预乘空间插值"（`PaintLerpArgbGrad`），与这里的顶点色语义一致：
// 顶点色是预乘色，GPU 在三个通道上做线性插值，而预乘空间里的线性渐变仍然是
// 屏幕坐标的仿射函数 —— 于是"淡出渐变"也是一块精确的四边形，不再有上万条带。
inline int BuildGradientQuads(float x, float y, float w, float h, float radius, int tpl,
                              float angleDeg, uint32_t from, uint32_t to,
                              BinUIGradQuad* out, int cap = 9) {
    if (!out || cap <= 0 || w <= 0.0f || h <= 0.0f) return 0;
    float dx, dy, len, sx, sy;
    PaintGradAxisOf(angleDeg, x, y, w, h, dx, dy, len, sx, sy);
    const float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;
    auto shade = [&](float qx, float qy, float qw, float qh,
                     float u0, float v0, float u1, float v1, BinUIGradQuad& q) {
        q.x = qx; q.y = qy; q.w = qw; q.h = qh;
        q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1;
        q.c[0] = PaintGradColorAtPoint(qx, qy, dx, dy, len, sx, sy, from, to);
        q.c[1] = PaintGradColorAtPoint(qx + qw, qy, dx, dy, len, sx, sy, from, to);
        q.c[2] = PaintGradColorAtPoint(qx, qy + qh, dx, dy, len, sx, sy, from, to);
        q.c[3] = PaintGradColorAtPoint(qx + qw, qy + qh, dx, dy, len, sx, sy, from, to);
    };
    if (r <= 0.5f) {                       // 直角：一个四边形，四角带色
        shade(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, out[0]);
        return 1;
    }
    if (tpl <= 0 || cap < 9) return 0;      // 没有圆角模板 → 交回条带路径
    PaintQuad q9[9];
    const int n = BuildRoundedNineSlice(x, y, w, h, r, tpl, q9, 9);
    if (n <= 0) return 0;
    const int m = n < cap ? n : cap;
    for (int i = 0; i < m; i++)
        shade(q9[i].x, q9[i].y, q9[i].w, q9[i].h, q9[i].u0, q9[i].v0, q9[i].u1, q9[i].v1, out[i]);
    return m;
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
// 路径填充（SVG <path>/<ellipse>）：折线 → 覆盖率 → 轴对齐条带
//
// 光栅化在 bin2d（v0.21.0 起，`bin2d::RasterizeCoverage`）；这里只剩 binUI 特有的那半段
// —— 覆盖率拼成条带。为什么必须经过覆盖率：三个后端都没有"填充任意多边形"的原语
// （GL/D3D 没有 stencil 与三角化接口，GDI 只有硬编码的三角形调用），共用一个求交器、
// 各端只负责 fillRect，三端像素才必然一致。
//
// 条带上限的兜底是**放大缓冲重试**（不是降精度）：条带数按几何复杂度增长，估不准，
// 与其写死一个上限后画一半，不如按需翻倍 —— 命中上限的形状很少，翻了就一直在。
// ============================================================
constexpr int kMaxPathBands = 4096;       // 单条路径的条带初始缓冲（不够就翻倍）
constexpr int kMaxPathBandsHard = 65536;  // 硬上限：宁可画不全也不吃爆内存
constexpr int kMaxOpenRuns = 96;          // 跨行合并的"正在延伸的条带"表

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
            // v0.16.38：色标之间也在预乘空间插值（SVG 2 / CSS Color 4 同口径）。
            // stop-opacity 不同的色标以前会把中间段"越插越暗"，浏览器不会。
            return PaintLerpArgbGrad(g.stops[i - 1].color, g.stops[i].color, k);
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

// ============================================================
// 路径填充：覆盖率 → 条带（P2 迁移，v0.21.0）
//
// **光栅化已经交给 bin2d**（`bin2d::RasterizeCoverage`，与 bin2d 的 FillPath 同一个
// 光栅器）。本文件只留 binUI 特有的那一半：把覆盖率拼成条带（GPU 后端的 fillRect）。
// 实现在 src/backends/binui_paint.cpp —— 这样"依赖 bin2d"只落在一个 .o 上，而不是让
// 每个包含本头文件的翻译单元都去 link 它。
//
// 删掉的东西（都不再有第二个使用者）：PaintScanEdge / BuildPaintScanEdges /
// ActivateScanEdges / 4 条子扫描线 + 1024 定点累加器 / rowStep 降级 / 4096px 兜底。
// 覆盖率的语义（8 条子采样、水平解析求交）现在只有一份，在 bin2d 里。
// ============================================================

// 硬边填充（shape-rendering="crispEdges"/optimizeSpeed）：端点落到整像素，覆盖率 0/255。
// rowStep 保留只为签名兼容（老实现用它降级，bin2d 的覆盖率没有条带上限）。
int BuildPathFillBands(const PaintPath& path, bool evenOdd,
                       float clipX0, float clipY0, float clipX1, float clipY1,
                       const PathShade& shade, PaintBand* out, int cap,
                       int rowStep = 1, bool* truncated = nullptr);

// 覆盖率 AA 填充（shape-rendering="auto"/optimizeQuality）：4 条竖直子采样。
int BuildPathFillBandsAA(const PaintPath& path, bool evenOdd,
                         float clipX0, float clipY0, float clipX1, float clipY1,
                         const PathShade& shade, PaintBand* out, int cap, bool* truncated);

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

// ============ 路径描边（v0.22.0：覆盖率换成 bin2d 的描边覆盖率） ============
//
// 为什么描边也必须走 CPU 覆盖率（量出来的，不是猜的）：
// 4× MSAA 的**采样子像素位置**由 API 与驱动决定，OpenGL 与 Vulkan 在同一块 GPU 上
// 并不相同（x 方向互为镜像）。填充走的是 CPU 覆盖率条带，所以三端必然一致；描边以前
// 直接发细长四边形，于是差异只在"边缘斜着穿过像素"时出现：
//   · 轴对齐线、任意宽度 → 0 差异（覆盖数与采样位置无关）
//   · 45° 线 → 0 差异（采样位置镜像对称，覆盖数不变）
//   · 9.5° 线（120×20）→ 整条差 62/255，正好一档 4× MSAA（25%）
//   · `<circle>` 描边 → 圆是折线，整圈都差（实测 320 个像素、最大通道差 123）
// 所以描边几何一律算覆盖率，三端调同一份代码 —— 采样位置不可设置（OpenGL 没有对应
// API），这是"三端逐像素相同"在斜线上唯一能兑现的办法。
//
// 为什么是**一张并集覆盖率**而不是"逐段各自混合"（v0.22.0 换掉的老做法）：
// 逐段混合时相邻段在接缝处各混一次 → 半透明描边的每个接缝深一条；拐点补的圆/多边形
// 也是各自混一次。现在交给 bin2d：每段四边形 + 拐点连接块 + 端帽逐块栅格化，同一像素
// **取 max** 合成并集，最后只混一次（语义与 bin2d 的 `StrokePath` 同源，见其
// `RasterizeStrokeCoverage`）。顺带解决两件事：
//   · 拐点/端帽以前走的是后端的 fillRounded（= GPU 圆角矩形，4× MSAA）→ 三端本来就会
//     在"圆 vs 24 边形"上分岔，现在统一由覆盖率给出；
//   · 斜接/斜面缺口以前走硬边条带（无 AA），现在跟填充一样有 AA。
//
// 代价与兜底：一条路径的描边条带数比填充更随机（AA 边缘每行的 alpha 都不一样，跨行
// 合并不上），所以缓冲按需翻倍，硬上限 kMaxPathBandsHard。
//
// 仿射（rotate/scale）下的描边仍然是"条带过矩阵"：条带是轴对齐矩形，过矩阵后相邻条带
// 之间会露缝，质量反而更差 —— 这与填充走的是同一条路，仿射路径另有像素门禁用例兜底。
//
// cap = BinUIPathCap：BUTT（端点即路径端点）/ ROUND（两端半圆）/ SQUARE（两端各外延半线宽）
// join = BinUIPathJoin：ROUND（拐点补圆）/ MITER（斜接，超 miterLimit 退化成斜面）/
//        BEVEL（斜面 = miterLimit 取 1 的斜接，正是 SVG 规范里"退化成斜面"的定义）
// miterLimit 为 0 时按 SVG 默认 4。返回画出的条带数。
int BuildPathStrokeBands(const PaintPath& path, float width, uint8_t cap, uint8_t join,
                         uint8_t miterLimit, float clipX0, float clipY0, float clipX1,
                         float clipY1, const PathShade& shade, PaintBand* out, int maxBands,
                         bool* truncated);

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
    uint64_t key = 0;               // 0 = 还没算过；命中还要求对应列表非空（超预算就不存）
    uint64_t gen = 0;               // 渲染器布局代数（作废判断）
    size_t   accounted = 0;         // 已经计入渲染器全局预算的条带数（记账用）
    std::vector<PaintBand> bands;        // 填充条带
    std::vector<PaintBand> strokeBands;  // 描边条带（v0.22.0：与填充同一个 key）
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
    std::vector<PathPointF> pts;       // 设备坐标
    std::vector<float> xy;             // PaintPath 需要的 x,y 交错布局
    std::vector<PaintBand> bands;      // 填充条带（含"撞上限就翻倍"的余量）
    std::vector<PaintBand> strokeBands; // 描边条带（v0.22.0）
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

template <class FillBands>
inline void PaintPathNode(const BinUIPathEntry& head, const uint16_t* normPts,
                          const uint32_t* first, const uint32_t* count,
                          const uint8_t* closed, int contours,
                          float x, float y, float w, float h,
                          const PathPaintSpec& spec,
                          PathBandCacheEntry* cache,   // 可为 nullptr（不缓存）
                          uint64_t cacheGen,
                          size_t cacheBudget,          // 本节点最多还能占多少条带（渲染器给）
                          PathRenderBuffers& buf,
                          FillBands&& fillBands) {
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

    // v19：先看缓存 —— 形状、尺寸、颜色、透明度、flags 都没变时直接重放上一次拼好的条带。
    // 填充与描边共用一个 key（同一个节点、同一批输入），但各存一份列表：某一份超预算
    // 就是不缓存那一份（下帧重算），不牵连另一份。少缓存只影响速度，不影响画面。
    const uint64_t key = cache ? MakePathBandKey(x, y, w, h, spec, head.flags) : 0;
    const bool hit = cache && key != 0 && cache->key == key && cache->gen == cacheGen;
    if (cache && !hit) {
        // 换了几何/颜料：整条作废（不做逐项失效 —— key 里已经含了全部输入）
        cache->bands.clear();
        cache->strokeBands.clear();
        cache->key = key;
        cache->gen = cacheGen;
    }

    if (head.flags & PATH_FILL) {
        if (hit && !cache->bands.empty()) {
            fillBands(cache->bands.data(), static_cast<int>(cache->bands.size()));
        } else {
            const bool evenOdd = (head.flags & PATH_EVENODD) != 0;
            // v16：渐变填充（无渐变时退化成纯色取样器，与老路径完全一致）
            const PathShade shade =
                MakePathShade((head.flags & PATH_FILL_GRAD) ? spec.fillGrad : nullptr,
                              spec.fillColor, spec.opacity, x, y, w, h);
            // v19：shape-rendering=auto/optimizeQuality 时走覆盖率 AA，否则硬边。
            // 条带撞上限就**放大缓冲重试**（不是降精度、也不是退回另一个分支）。
            if (buf.bands.size() < static_cast<size_t>(kMaxPathBands))
                buf.bands.resize(kMaxPathBands);
            int n = 0;
            bool truncated = false;
            for (;;) {
                truncated = false;
                if (head.flags & PATH_AA) {
                    n = BuildPathFillBandsAA(view, evenOdd, x, y, x + w, y + h, shade,
                                             buf.bands.data(), static_cast<int>(buf.bands.size()),
                                             &truncated);
                } else {
                    n = BuildPathFillBands(view, evenOdd, x, y, x + w, y + h, shade,
                                           buf.bands.data(), static_cast<int>(buf.bands.size()), 1,
                                           &truncated);
                }
                if (!truncated || buf.bands.size() >= static_cast<size_t>(kMaxPathBandsHard)) break;
                buf.bands.resize(std::min<size_t>(buf.bands.size() * 2, kMaxPathBandsHard));
            }
            if (n > 0) fillBands(buf.bands.data(), n);
            if (cache) {
                // 单节点 16384 条的安全上限 + 渲染器给的全局预算；任何一个不够就不缓存
                // （不缓存只是每帧重算，不影响画对，所以这里是"宁可少缓存"）。
                if (n > 0 && n <= kMaxCachedBands && static_cast<size_t>(n) <= cacheBudget)
                    cache->bands.assign(buf.bands.data(), buf.bands.data() + n);
                else
                    cache->bands.clear();
            }
        }
    }

    if (head.flags & PATH_STROKE) {
        const float k = head.designH > 0.0f ? h / head.designH : 1.0f;
        const float sw = static_cast<float>(head.strokeW) * k;
        if (sw > 0.05f) {
            if (hit && !cache->strokeBands.empty()) {
                fillBands(cache->strokeBands.data(),
                          static_cast<int>(cache->strokeBands.size()));
            } else {
                // v17：渐变描边按**像素**取色（量化级与填充同一套），所以一条斜线的
                // 每根条带颜色都跟着渐变走，而不是"一段线一个颜色"。
                const PathShade strokeShade =
                    MakePathShade((head.flags & PATH_STROKE_GRAD) ? spec.strokeGrad : nullptr,
                                  spec.strokeColor, spec.opacity, x, y, w, h);
                // 裁剪盒要**外扩**：节点盒不含描边（SVG 的 bbox 本来就不含），斜接尖角还能
                // 伸出 (ratio-1)·半线宽 —— 不外扩的话尖角会被自己节点的盒子切平，看起来
                // 像被削过的斜面。外扩量取 miter-limit 允许的最大值；父级裁剪照常生效。
                const uint8_t limitB = head.reserve > 0 ? head.reserve : 4;
                const float limit = static_cast<float>(limitB > 1 ? limitB : 1);
                const float pad = sw * 0.5f * limit + 1.0f;
                if (buf.strokeBands.size() < static_cast<size_t>(kMaxPathBands))
                    buf.strokeBands.resize(kMaxPathBands);
                int n = 0;
                bool truncated = false;
                for (;;) {
                    truncated = false;
                    n = BuildPathStrokeBands(view, sw, head.cap, head.join, head.reserve,
                                             x - pad, y - pad, x + w + pad, y + h + pad,
                                             strokeShade, buf.strokeBands.data(),
                                             static_cast<int>(buf.strokeBands.size()), &truncated);
                    if (!truncated || buf.strokeBands.size() >= static_cast<size_t>(kMaxPathBandsHard))
                        break;
                    buf.strokeBands.resize(
                        std::min<size_t>(buf.strokeBands.size() * 2, kMaxPathBandsHard));
                }
                if (n > 0) fillBands(buf.strokeBands.data(), n);
                if (cache) {
                    if (n > 0 && n <= kMaxCachedBands && static_cast<size_t>(n) <= cacheBudget)
                        cache->strokeBands.assign(buf.strokeBands.data(),
                                                  buf.strokeBands.data() + n);
                    else
                        cache->strokeBands.clear();
                }
            }
        }
    }
}

// ============ v0.17.1：后端 fillLine 的 CPU 覆盖率条带 ============
//
// 这套（`BuildLineBandsAA` + `LineBandCache`）现在**只服务后端原语 fillLine**
// （边框、焦点环、虚线、分隔线……），路径描边已经改走 `BuildPathStrokeBands`
//（= bin2d 的描边覆盖率，v0.22.0）。两者共用"CPU 算覆盖率 → 条带"的原因一样：
//
// 为什么单根线不能用 GPU 四边形（量出来的，不是猜的）：
// 4× MSAA 的**采样子像素位置**由 API 与驱动决定，OpenGL 与 Vulkan 在同一块 GPU 上
// 并不相同（x 方向互为镜像）。填充走的是 CPU 覆盖率条带，所以三端必然一致；线以前
// 直接发细长四边形，于是差异只在"边缘斜着穿过像素"时出现：
//   · 轴对齐线、任意宽度 → 0 差异（覆盖数与采样位置无关）
//   · 45° 线 → 0 差异（采样位置镜像对称，覆盖数不变）
//   · 9.5° 线（120×20）→ 整条差 62/255，正好一档 4× MSAA（25%）
//   · `<circle>` 描边 → 圆是折线，整圈都差（实测 320 个像素、最大通道差 123）
//   · 斜三角形填充 / 圆角矩形 / 小数坐标矩形 → 0 差异（都不经过 GPU 光栅化的斜边）
// 所以线几何一律在 CPU 上算覆盖率，三端调同一份代码 —— 采样位置不可设置
// （OpenGL 没有对应 API），这是"三端逐像素相同"在斜线上唯一能兑现的办法。
//
// 代价：一段线从 1 个四边形变成 2~8 条条带（轴对齐线合并成 1 条，斜线按行拆）。
// 仿射（rotate/scale）下 fillLine **仍走原来的四边形**：条带是轴对齐矩形，过矩阵后
// 相邻条带之间会露出缝隙，质量反而更差；仿射路径另有像素门禁用例兜底。
// 注意：**轴对齐也不能交给 GPU 四边形**。本来想给"dx==0 或 dy==0"留一条快路径
// （四边与像素格平行，覆盖数像是精确值），实测翻车：竖线 stroke-width=3 落在 x=150、
// 边缘恰好在 148.5 / 151.5 这两条**像素边界**上时，GL 与 VK 在 (148,68) / (151,68)
// 差 55 —— 边缘正好压住采样点的归属，两个 API 判定不同。45° 同样不保险。
// 所以不留快路径：所有非仿射线一律走 CPU 覆盖率，成本靠下面的条带缓存压回去。

// ---- 线描边的跨帧缓存（v0.17.1）----
// 为什么需要：CPU 覆盖率比原来的"一个 GPU 四边形"贵 3~5 倍（svgdemo 实测 0.078 →
// 0.335 ms/帧）。但静态页面的线几何每帧完全一样，key 一命中就整段重放，成本回到
// "发 N 个条带"；动画页面每帧 miss，可动画本来每帧都要重算。
// 缓存做成 thread_local 的全局表：后端 fillLine 是逐段调用、拿不到节点上下文，
// 而 key 里已经含了几何与颜色 —— 变了自己就 miss，不需要失效通知。
// 满了一律清空（不做 LRU：写淘汰逻辑的成本比"极端场景重算一帧"更高）。
inline uint64_t MakeLineBandKey(float x0, float y0, float x1, float y1, float width,
                                uint32_t color) {
    uint64_t k = 0x9E3779B97F4A7C15ull;
    auto mix = [&](uint64_t v) { k ^= v + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2); };
    const float fs[5] = { x0, y0, x1, y1, width };
    for (int i = 0; i < 5; i++) {
        uint32_t u = 0;
        std::memcpy(&u, &fs[i], sizeof(u));
        mix(u);
    }
    mix(color);
    return k ? k : 1;
}

constexpr size_t kLineBandCacheCap = 4096;

inline std::unordered_map<uint64_t, std::vector<PaintBand>>& LineBandCache() {
    static thread_local std::unordered_map<uint64_t, std::vector<PaintBand>> cache;
    return cache;
}

inline int BuildLineBandsAA(float x0, float y0, float x1, float y1, float width,
                            uint32_t color, PaintBand* out, int cap) {
    if (!out || cap <= 0 || width <= 0.0f) return 0;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return 0;
    const float nx = -dy / len * width * 0.5f;
    const float ny = dx / len * width * 0.5f;
    const float pts[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny,
                           x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    float minX = pts[0], maxX = pts[0], minY = pts[1], maxY = pts[1];
    for (int i = 1; i < 4; i++) {
        minX = std::min(minX, pts[i * 2]);
        maxX = std::max(maxX, pts[i * 2]);
        minY = std::min(minY, pts[i * 2 + 1]);
        maxY = std::max(maxY, pts[i * 2 + 1]);
    }
    uint32_t first = 0;
    const uint32_t cnt = 4;
    const uint8_t closed = 1;
    const PaintPath pg{ pts, &first, &cnt, &closed, 1 };
    PathShade ps;
    ps.color = color;          // 纯色：渐变已在 colorAt 里逐段取色，这里不再二次取样
    bool truncated = false;
    int n = BuildPathFillBandsAA(pg, false, minX, minY, maxX, maxY, ps, out, cap, &truncated);
    return n > 0 ? n : 0;
}

// ============ v0.16.21：仿射矩阵工具（级联用） ============
// 2×3 仿射，存储顺序与后端 setAffine / fillRoundedXf 一致：[a c e; b d f]，
// 即 p' = (a·x + c·y + e, b·x + d·y + f)。
// 单独抽出来是因为"矩阵复合"是最容易写错的一步：转一个卡片时子元素的矩阵是
// 父矩阵 ∘ 平移 ∘ 自身旋转/缩放，少乘一项就是"整页内容全挤到左上角"。
inline void AffineMul(const float m1[6], const float m0[6], float out[6]) {
    // out = m1 ∘ m0（先作用 m0，再作用 m1）
    const float a1 = m1[0], c1 = m1[1], e1 = m1[2];
    const float b1 = m1[3], d1 = m1[4], f1 = m1[5];
    const float a0 = m0[0], c0 = m0[1], e0 = m0[2];
    const float b0 = m0[3], d0 = m0[4], f0 = m0[5];
    const float r0 = a1 * a0 + c1 * b0;
    const float r1 = a1 * c0 + c1 * d0;
    const float r2 = a1 * e0 + c1 * f0 + e1;
    const float r3 = b1 * a0 + d1 * b0;
    const float r4 = b1 * c0 + d1 * d0;
    const float r5 = b1 * e0 + d1 * f0 + f1;
    out[0] = r0; out[1] = r1; out[2] = r2;
    out[3] = r3; out[4] = r4; out[5] = r5;
}

// 绕 (cx,cy) 旋转 rotateDeg 度 + 非等比缩放 (kx,ky)（CSS transform 的 rotate∘scale，
// 顺时针为正，y 轴朝下）。中心点是不动点。
inline void AffineRotScaleAbout(float cx, float cy, float rotateDeg, float kx, float ky,
                                float out[6]) {
    const float rad = rotateDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float a = cs * kx, b = sn * kx;
    const float c = -sn * ky, d = cs * ky;
    out[0] = a; out[1] = c; out[2] = cx - (a * cx + c * cy);
    out[3] = b; out[4] = d; out[5] = cy - (b * cx + d * cy);
}

} // namespace binui
