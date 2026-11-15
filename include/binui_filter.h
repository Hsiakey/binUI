#pragma once
// ============================================================
// CSS filter 的颜色矩阵（v26）：编译期合成、运行期逐像素应用
//
// 为什么走 CPU 而不是着色器：CSS 的 grayscale/sepia/saturate/hue-rotate/invert/
// contrast 全是"RGB 上的一次仿射变换"（3×3 矩阵 + 偏移），用顶点色乘不出来，
// 要改片元着色器 —— 而 VK 的 SPIR-V 是仓库里预编译好的常量数组（没有构建期
// glslang），改一处就得手写 SPIR-V。后端解码图片时顺手跑一遍矩阵，代价是
// 上传时一次 O(w·h)，之后按 (imageIndex, 矩阵哈希) 缓存纹理，零每帧开销。
//
// 数学口径按 CSS Filter Effects Level 1（sRGB 空间、非预乘、逐函数合成）：
//   · 每个函数是一个仿射：p' = M·p + o
//   · 多个函数按**书写顺序**合成：总矩阵 = Fn ∘ … ∘ F1（先写的先作用）
//   · 结果 clamp 到 [0,1] 后再乘 255
// 亮度亮度（brightness）不在这里：它等价于"乘以一个顶点色"，从 0.16.15 起就走
// 后端现成的 tint 通路（paint.reserve[2]），GDI 也能吃 —— 所以这里不含 brightness。
// ============================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "binui_format.h"

namespace binui {

// 3×3 矩阵 + 偏移（作用在 0..1 的线性 RGB；行主序 m[0..8] = [rr rg rb; gr gg gb; br bg bb]）
struct ColorMatrix {
    float m[9];
    float o[3];
};

inline ColorMatrix ColorMatrixIdentity() {
    ColorMatrix c;
    c.m[0] = 1.0f; c.m[1] = 0.0f; c.m[2] = 0.0f;
    c.m[3] = 0.0f; c.m[4] = 1.0f; c.m[5] = 0.0f;
    c.m[6] = 0.0f; c.m[7] = 0.0f; c.m[8] = 1.0f;
    c.o[0] = c.o[1] = c.o[2] = 0.0f;
    return c;
}

// f ∘ g（先作用 g，再作用 f）
inline ColorMatrix ColorMatrixMul(const ColorMatrix& f, const ColorMatrix& g) {
    ColorMatrix r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i * 3 + j] = f.m[i * 3 + 0] * g.m[0 * 3 + j] +
                             f.m[i * 3 + 1] * g.m[1 * 3 + j] +
                             f.m[i * 3 + 2] * g.m[2 * 3 + j];
        }
        r.o[i] = f.m[i * 3 + 0] * g.o[0] + f.m[i * 3 + 1] * g.o[1] +
                 f.m[i * 3 + 2] * g.o[2] + f.o[i];
    }
    return r;
}

// 单位矩阵与目标矩阵按 amount 线性插值（spec 里所有"百分比"函数的通用写法）
inline ColorMatrix ColorMatrixLerp(float amount, const float target[9]) {
    const float a = amount;
    ColorMatrix r = ColorMatrixIdentity();
    for (int i = 0; i < 9; i++) r.m[i] = r.m[i] * (1.0f - a) + target[i] * a;
    r.o[0] = r.o[1] = r.o[2] = 0.0f;
    return r;
}

// grayscale(a)：a 为 0..1 的比例
inline ColorMatrix FilterGrayscale(float a) {
    static const float k[9] = {
        0.2126f, 0.7152f, 0.0722f,
        0.2126f, 0.7152f, 0.0722f,
        0.2126f, 0.7152f, 0.0722f,
    };
    return ColorMatrixLerp(a, k);
}

// sepia(a)
inline ColorMatrix FilterSepia(float a) {
    static const float k[9] = {
        0.393f, 0.769f, 0.189f,
        0.349f, 0.686f, 0.168f,
        0.272f, 0.534f, 0.131f,
    };
    return ColorMatrixLerp(a, k);
}

// saturate(s)：s 为 0..∞ 的比例（1 = 原样，0 = 灰度）
inline ColorMatrix FilterSaturate(float s) {
    const float r = 0.2126f, g = 0.7152f, b = 0.0722f;
    ColorMatrix c;
    c.m[0] = r + (1.0f - r) * s; c.m[1] = g - g * s;        c.m[2] = b - b * s;
    c.m[3] = r - r * s;          c.m[4] = g + (1.0f - g) * s; c.m[5] = b - b * s;
    c.m[6] = r - r * s;          c.m[7] = g - g * s;        c.m[8] = b + (1.0f - b) * s;
    c.o[0] = c.o[1] = c.o[2] = 0.0f;
    return c;
}

// hue-rotate(deg)：RGB 立方体绕灰度轴旋转（spec 给的闭式解）
inline ColorMatrix FilterHueRotate(float deg) {
    const float rad = deg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    ColorMatrix c;
    c.m[0] = 0.213f + cs * 0.787f - sn * 0.213f;
    c.m[1] = 0.715f - cs * 0.715f - sn * 0.715f;
    c.m[2] = 0.072f - cs * 0.072f + sn * 0.928f;
    c.m[3] = 0.213f - cs * 0.213f + sn * 0.143f;
    c.m[4] = 0.715f + cs * 0.285f + sn * 0.140f;
    c.m[5] = 0.072f - cs * 0.072f - sn * 0.283f;
    c.m[6] = 0.213f - cs * 0.213f - sn * 0.787f;
    c.m[7] = 0.715f - cs * 0.715f + sn * 0.715f;
    c.m[8] = 0.072f + cs * 0.928f + sn * 0.072f;
    c.o[0] = c.o[1] = c.o[2] = 0.0f;
    return c;
}

// invert(a)：p' = (1-2a)·p + a
inline ColorMatrix FilterInvert(float a) {
    const float k = 1.0f - 2.0f * a;
    ColorMatrix c = ColorMatrixIdentity();
    c.m[0] = c.m[4] = c.m[8] = k;
    c.o[0] = c.o[1] = c.o[2] = a;
    return c;
}

// contrast(a)：p' = a·p + (0.5 - 0.5a)
inline ColorMatrix FilterContrast(float a) {
    ColorMatrix c = ColorMatrixIdentity();
    c.m[0] = c.m[4] = c.m[8] = a;
    c.o[0] = c.o[1] = c.o[2] = 0.5f - 0.5f * a;
    return c;
}

// 像素矩阵是否可以跳过（≈单位矩阵）
inline bool ColorMatrixIsIdentity(const ColorMatrix& c) {
    for (int i = 0; i < 9; i++) {
        const float want = (i % 4 == 0) ? 1.0f : 0.0f;
        if (std::fabs(c.m[i] - want) > 0.0005f) return false;
    }
    for (int i = 0; i < 3; i++)
        if (std::fabs(c.o[i]) > 0.0005f) return false;
    return true;
}

// 就地把 BGRA8 像素过一遍矩阵（straight alpha：只改颜色，alpha 不动）
inline void ApplyColorMatrixBGRA(uint8_t* px, int w, int h, const ColorMatrix& c) {
    if (!px || w <= 0 || h <= 0) return;
    const float m0 = c.m[0], m1 = c.m[1], m2 = c.m[2];
    const float m3 = c.m[3], m4 = c.m[4], m5 = c.m[5];
    const float m6 = c.m[6], m7 = c.m[7], m8 = c.m[8];
    const float o0 = c.o[0], o1 = c.o[1], o2 = c.o[2];
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    uint8_t* p = px;
    for (size_t i = 0; i < n; i++, p += 4) {
        if (p[3] == 0) continue;   // 全透明像素没有颜色信息（也避免 ABGR 乱序被放大）
        const float b = p[0] / 255.0f, g = p[1] / 255.0f, r = p[2] / 255.0f;
        float nr = m0 * r + m1 * g + m2 * b + o0;
        float ng = m3 * r + m4 * g + m5 * b + o1;
        float nb = m6 * r + m7 * g + m8 * b + o2;
        nr = nr < 0.0f ? 0.0f : (nr > 1.0f ? 1.0f : nr);
        ng = ng < 0.0f ? 0.0f : (ng > 1.0f ? 1.0f : ng);
        nb = nb < 0.0f ? 0.0f : (nb > 1.0f ? 1.0f : nb);
        p[2] = static_cast<uint8_t>(nr * 255.0f + 0.5f);
        p[1] = static_cast<uint8_t>(ng * 255.0f + 0.5f);
        p[0] = static_cast<uint8_t>(nb * 255.0f + 0.5f);
    }
}

// 过滤器内容 → 64 位缓存键（后端把"同一张图 + 不同滤镜"当两个纹理）
inline uint64_t ImageFilterKey(const BinUIImageFilter* f) {
    if (!f || !(f->flags & 1)) return 0;
    // FNV-1a over 矩阵 + 偏移（48 字节）
    const uint8_t* p = reinterpret_cast<const uint8_t*>(f);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 4; i < 52; i++) {   // 跳过 nodeId/flags/reserved
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h | 1ull;   // 0 保留给"无滤镜"
}

} // namespace binui
