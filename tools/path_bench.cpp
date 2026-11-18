// path_bench.cpp —— 路径填充 / 描边几何的 CPU 成本（无窗口 / 无 GPU / 确定性）
//
// 量的是两条：「光栅化把折线路径变成轴对齐条带」的开销（填充硬边 + AA），
// 以及「描边的并集覆盖率 → 条带」（v0.22.0 起描边也走覆盖率）。
// 只跑 BuildPathFillBands / BuildPathFillBandsAA / BuildPathStrokeBands 并计时 ——
// 不画、不上传、不碰 GPU，所以换后端、换窗口、换刷新率都不影响这个数。
//
// 描边那一节还带一列"旧口径"：逐段 BuildLineBandsAA 之和 + 拐点（旧实现拐点是后端
// fillRounded，CPU 侧免费）。把它和新口径并排放，才看得出换覆盖率到底花了多少 CPU。
//
// 用法: path_bench [图标数，默认 50]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "binui_paint.h"

using namespace binui;

static double NowUs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double, std::micro>(clock::now() - t0).count();
}

// 一条子路径（设备坐标），first/count/closed 各 1 项
struct DevPath {
    std::vector<float> xy;
    std::vector<uint32_t> first{ 0 };
    std::vector<uint32_t> count;
    std::vector<uint8_t> closed{ 1 };
    PaintPath view() const {
        return PaintPath{ xy.data(), first.data(), count.data(), closed.data(),
                          static_cast<int>(first.size()) };
    }
};

// 星形：10 个折点（凹多边形，even-odd 与 nonzero 都能填）
static DevPath MakeStar(float size) {
    DevPath d;
    const float cx = size * 0.5f, cy = size * 0.5f;
    for (int i = 0; i < 10; i++) {
        const float ang = -1.5707963f + (float)i * 0.62831853f;   // 36°
        const float r = (i & 1) ? size * 0.22f : size * 0.48f;
        d.xy.push_back(cx + std::cos(ang) * r);
        d.xy.push_back(cy + std::sin(ang) * r);
    }
    d.count.push_back(10);
    return d;
}

// 齿轮：teeth 个齿 → 4×teeth 个折点（模拟真实图标里的"复杂路径"）
static DevPath MakeGear(float size, int teeth) {
    DevPath d;
    const float cx = size * 0.5f, cy = size * 0.5f;
    const int n = teeth * 4;
    for (int i = 0; i < n; i++) {
        const float ang = (float)i * 6.2831853f / (float)n;
        const int phase = i % 4;
        const float r = (phase == 1 || phase == 2) ? size * 0.46f : size * 0.34f;
        d.xy.push_back(cx + std::cos(ang) * r);
        d.xy.push_back(cy + std::sin(ang) * r);
    }
    d.count.push_back((uint32_t)n);
    return d;
}

// 圆：segments 段折线
static DevPath MakeCircle(float size, int segments) {
    DevPath d;
    const float cx = size * 0.5f, cy = size * 0.5f;
    for (int i = 0; i < segments; i++) {
        const float ang = (float)i * 6.2831853f / (float)segments;
        d.xy.push_back(cx + std::cos(ang) * size * 0.48f);
        d.xy.push_back(cy + std::sin(ang) * size * 0.48f);
    }
    d.count.push_back((uint32_t)segments);
    return d;
}

int main(int argc, char** argv) {
    int icons = 50;
    if (argc > 1) icons = std::atoi(argv[1]);
    if (icons < 1) icons = 1;

    std::vector<PaintBand> bands(static_cast<size_t>(kMaxPathBands));

    struct Shape { const char* name; DevPath (*make)(float); };
    const float sizes[] = { 24.0f, 48.0f, 120.0f, 300.0f };

    std::printf("路径填充几何（单线程，best-of-5，每轮 20 次）\n");
    std::printf("%-10s %6s %8s %10s %10s %10s\n", "形状", "尺寸", "边界数", "硬边 µs", "AA µs", "AA 像素/µs");
    double sumHard = 0.0, sumAA = 0.0;
    int counted = 0;
    for (int si = 0; si < static_cast<int>(sizeof(sizes) / sizeof(sizes[0])); si++) {
        const float sz = sizes[si];
        DevPath paths[3] = { MakeStar(sz), MakeGear(sz, 16), MakeCircle(sz, 64) };
        const char* names[3] = { "星形10点", "齿轮16齿", "圆64段" };
        for (int pi = 0; pi < 3; pi++) {
            const PaintPath pp = paths[pi].view();
            bool trunc = false;
            int nb = 0;
            double bestHard = 1e30, bestAA = 1e30;
            for (int rep = 0; rep < 5; rep++) {
                const double t0 = NowUs();
                for (int it = 0; it < 20; it++)
                    nb = BuildPathFillBands(pp, false, 0.0f, 0.0f, sz, sz, 0xFF3366CCu,
                                            bands.data(), static_cast<int>(bands.size()), 1, &trunc);
                bestHard = std::min(bestHard, (NowUs() - t0) / 20.0);
            }
            const int nbAA = BuildPathFillBandsAA(pp, false, 0.0f, 0.0f, sz, sz, PathShade{},
                                                  bands.data(), static_cast<int>(bands.size()),
                                                  &trunc);
            if (nbAA > 0) {
                for (int rep = 0; rep < 5; rep++) {
                    const double t0 = NowUs();
                    for (int it = 0; it < 20; it++)
                        BuildPathFillBandsAA(pp, false, 0.0f, 0.0f, sz, sz, PathShade{},
                                             bands.data(), static_cast<int>(bands.size()),
                                             &trunc);
                    bestAA = std::min(bestAA, (NowUs() - t0) / 20.0);
                }
            } else {
                bestAA = -1.0;
            }
            std::printf("%-10s %6.0f %8d %10.2f %10.2f %10s\n", names[pi], (double)sz, nb,
                        bestHard, bestAA, bestAA > 0 ? "" : "（太宽，退回硬边）");
            if (std::fabs(sz - 120.0f) < 0.5f) { sumHard += bestHard; sumAA += bestAA; counted++; }
        }
    }
    if (counted > 0) {
        std::printf("\n120px 三个图标合计: 硬边 %.1f µs / AA %.1f µs", sumHard, sumAA);
        std::printf(" → %d 个图标 = %.2f ms（硬边）/ %.2f ms（AA）\n",
                    icons, sumHard * icons / counted / 1000.0, sumAA * icons / counted / 1000.0);
    }

    // ---- 描边（v0.22.0）：并集覆盖率 vs 旧口径（逐段四边形 + 拐点免费）----
    {
        std::printf("\n路径描边几何（单线程，best-of-5，每轮 20 次；线宽 4）\n");
        std::printf("%-10s %6s %8s %12s %12s %8s %10s\n", "形状", "尺寸", "段数",
                    "新(并集) µs", "旧(逐段) µs", "倍数", "条带");
        double newSum = 0.0, oldSum = 0.0;
        int ncounted = 0;
        for (int si = 0; si < static_cast<int>(sizeof(sizes) / sizeof(sizes[0])); si++) {
            const float sz = sizes[si];
            DevPath paths[3] = { MakeStar(sz), MakeGear(sz, 16), MakeCircle(sz, 64) };
            const char* names[3] = { "星形10点", "齿轮16齿", "圆64段" };
            for (int pi = 0; pi < 3; pi++) {
                const PaintPath pp = paths[pi].view();
                PathShade shade;                 // 纯色：描边色不随位置变
                shade.color = 0xFF3366CCu;
                bool trunc = false;
                int nb = 0;
                double bestNew = 1e30, bestOld = 1e30;
                for (int rep = 0; rep < 5; rep++) {
                    const double t0 = NowUs();
                    for (int it = 0; it < 20; it++)
                        nb = BuildPathStrokeBands(pp, 4.0f, BINUI_CAP_ROUND, BINUI_JOIN_ROUND, 0,
                                                  0.0f, 0.0f, sz, sz, shade, bands.data(),
                                                  static_cast<int>(bands.size()), &trunc);
                    bestNew = std::min(bestNew, (NowUs() - t0) / 20.0);
                }
                // 旧口径：每段一个四边形（= 后端 fillLine 做的 BuildLineBandsAA），拐点不计
                int segs = 0;
                for (int rep = 0; rep < 5; rep++) {
                    const double t0 = NowUs();
                    for (int it = 0; it < 20; it++) {
                        segs = 0;
                        for (int c = 0; c < pp.contours; c++) {
                            const uint32_t f = pp.first[c], n = pp.count[c];
                            if (n < 2) continue;
                            const bool closed = pp.closed && pp.closed[c];
                            const uint32_t sc = closed ? n : n - 1;
                            for (uint32_t i = 0; i < sc; i++) {
                                const uint32_t ia = f + i, ib = f + (i + 1) % n;
                                BuildLineBandsAA(pp.pts[ia * 2], pp.pts[ia * 2 + 1],
                                                 pp.pts[ib * 2], pp.pts[ib * 2 + 1], 4.0f,
                                                 shade.color, bands.data(),
                                                 static_cast<int>(bands.size()));
                                segs++;
                            }
                        }
                    }
                    bestOld = std::min(bestOld, (NowUs() - t0) / 20.0);
                }
                std::printf("%-10s %6.0f %8d %12.2f %12.2f %7.2fx %10d\n", names[pi], (double)sz,
                            segs, bestNew, bestOld, bestOld > 0 ? bestNew / bestOld : 0.0, nb);
                if (std::fabs(sz - 120.0f) < 0.5f) {
                    newSum += bestNew;
                    oldSum += bestOld;
                    ncounted++;
                }
            }
        }
        if (ncounted > 0) {
            std::printf("\n120px 三个图标合计: 新 %.1f µs / 旧 %.1f µs（%.2fx）",
                        newSum, oldSum, oldSum > 0 ? newSum / oldSum : 0.0);
            std::printf(" → %d 个图标 = %.2f ms（新）/ %.2f ms（旧）\n", icons,
                        newSum * icons / ncounted / 1000.0, oldSum * icons / ncounted / 1000.0);
        }
    }
    return 0;
}
