// ============================================================
// src/backends/binui_paint.cpp —— 路径填充的覆盖率 → 条带（P2 迁移，v0.21.0）
//
// 这里原来在 include/binui_paint.h 里：一套自己的扫描线 + 覆盖率累加 + 条带合并
// （约 380 行）+ 4 条子扫描线的整数定点累加器。现在**光栅化交给 bin2d**
//（`bin2d::RasterizeCoverage`，与 bin2d 自己的 FillPath 同一个光栅器），本文件只留
// binUI 特有的一半：**覆盖率 → 条带**。
//
// 为什么这么切：
//   · 扫描线/求交/填充规则/AA 是"几何能力"，两个库各写一套必然漂移 —— 那就是要删的那份；
//   · 条带是 binUI/GPU 后端的概念（把覆盖率拼成 fillRect 下发），bin2d 不该知道。
//
// 口径（与迁移前逐位对齐的四个点，见 docs/migrate-to-bin2d.md §P2）：
//   1. 竖直子采样：老实现 AA 是 4 条、硬边是 1 条（行中心）——这里显式传 4 / 1；
//   2. 硬边的端点需要落到整像素（老实现是 round(端点) 铺满整像素），所以走
//      `CoverageStyle::HardEdge`，而不是"把 0..255 自己二值化"——后者会让亚像素细条消失；
//   3. 覆盖率 → alpha 的量化：`round(cov * 255)`，与老实现的
//      `(cov1024 * 255 + 512) / 1024` 在 1024 定点上逐值相等（差 ≤1 级）；
//   4. 条带合并规则不变：行内"等 alpha 的连续像素"合一条；行间位置/宽度/颜色都相同
//      就连成一条（少一次 draw call）。硬边路径额外保留"渐变沿 x 每级切一段"的老行为。
//
// 已经**删掉**的老东西（都不再有第二个使用者）：PaintScanEdge / BuildPaintScanEdges /
// ActivateScanEdges / 4 条子扫描线 + 1024 定点累加器 / rowStep 降级（bin2d 的覆盖率
// 没有"条带上限"这回事，降级路径失去存在理由）/ 超过 4096px 退回硬边的兜底。
// ============================================================

#include "binui_paint.h"

#include "bin2d/raster.h"

namespace binui {
namespace {

// 跨行合并用的小表（与老实现同尺寸：96 条"正在延伸的条带"）
struct OpenRun {
    int x0 = 0, w = 0, endY = 0, idx = 0;
};

// 覆盖率 → 条带。cov 是 bin2d 导出的 8bit 覆盖率矩形。
//   splitX = true：渐变沿 x 变化时把一条带再按"一级几像素"切段（硬边路径的老行为）
int EmitBands(const bin2d::CoverageRect& cov, const PathShade& shade, PaintBand* out, int cap,
              bool* truncated, bool splitX) {
    if (truncated) *truncated = false;
    if (cov.empty() || !out || cap <= 0) return 0;
    OpenRun open[kMaxOpenRuns];
    int openN = 0;
    int n = 0;
    const int yEnd = cov.y0 + cov.h;
    auto flushStale = [&](int y) {
        int w = 0;
        for (int i = 0; i < openN; i++)
            if (open[i].endY > y) open[w++] = open[i];
        openN = w;
    };

    for (int y = cov.y0; y < yEnd; y++) {
        const uint8_t* r = cov.row(y);
        const float yc = static_cast<float>(y) + 0.5f;
        int p = 0;
        while (p < cov.w) {
            const int a = r[p];
            if (a <= 0) { p++; continue; }
            int q = p + 1;
            while (q < cov.w && r[q] == a) q++;
            const int runX = cov.x0 + p;
            const int runW = q - p;
            // 渐变：沿 x 走一级需要几个像素 —— 一条带太长就切段，级内颜色恒定
            int segs = 1;
            if (splitX && shade.Active() && shade.dtdx > 1e-9f) {
                const float stepPx = 1.0f / (static_cast<float>(shade.levels) * shade.dtdx);
                const int k = static_cast<int>(std::ceil(static_cast<float>(runW) /
                                                         std::max(stepPx, 1.0f)));
                if (k > segs) segs = std::min(k, kMaxGradSplits);
            }
            for (int sg = 0; sg < segs; sg++) {
                const int sx0 = runX + runW * sg / segs;
                const int sx1 = runX + runW * (sg + 1) / segs;
                const int w2 = sx1 - sx0;
                if (w2 <= 0) continue;
                const uint32_t base =
                    shade.At(static_cast<float>(sx0) + static_cast<float>(w2) * 0.5f, yc);
                // 整像素覆盖率 255 时不走乘法（与老实现"内部像素覆盖率恰为 1"同一条优化）
                const uint32_t col =
                    (a == 255) ? base : PathScaleArgb(base, static_cast<float>(a) / 255.0f);
                bool merged = false;
                for (int k = 0; k < openN; k++) {
                    if (open[k].x0 == sx0 && open[k].w == w2 && open[k].endY == y &&
                        out[open[k].idx].color == col) {
                        out[open[k].idx].h += 1.0f;
                        open[k].endY = y + 1;
                        merged = true;
                        break;
                    }
                }
                if (merged) continue;
                if (n >= cap) {
                    if (truncated) *truncated = true;
                    return n;
                }
                out[n] = {static_cast<float>(sx0), static_cast<float>(y),
                          static_cast<float>(w2), 1.0f, col, 0.0f, false};
                if (openN < kMaxOpenRuns) {
                    open[openN++] = {sx0, w2, y + 1, n};
                } else {
                    // 合并表满了只丢合并机会，像素仍然正确
                    int oldest = 0;
                    for (int k = 1; k < openN; k++)
                        if (open[k].endY < open[oldest].endY) oldest = k;
                    open[oldest] = {sx0, w2, y + 1, n};
                }
                n++;
            }
            p = q;
        }
        flushStale(y);
    }
    return n;
}

// 本线程的覆盖率缓冲（复用，避免每个节点每帧一次分配）
bin2d::CoverageRect& ThreadCoverage() {
    static thread_local bin2d::CoverageRect cov;
    return cov;
}

bool Rasterize(const PaintPath& path, bool evenOdd, float clipX0, float clipY0, float clipX1,
               float clipY1, bin2d::CoverageStyle style, int subSamples,
               bin2d::CoverageRect& cov) {
    if (!path.Valid()) return false;
    if (clipX1 <= clipX0 || clipY1 <= clipY0) return false;
    bin2d::CoverageRequest req;
    req.xy = path.pts;
    req.first = path.first;
    req.count = path.count;
    req.contours = path.contours;
    req.rule = evenOdd ? bin2d::FillRule::EvenOdd : bin2d::FillRule::NonZero;
    req.style = style;
    req.subSamples = subSamples;
    req.clip = bin2d::Rect(clipX0, clipY0, clipX1 - clipX0, clipY1 - clipY0);
    return bin2d::RasterizeCoverage(req, cov);
}

} // namespace

// 硬边填充（shape-rendering="crispEdges" / optimizeSpeed，也是 <path> 的默认路径）：
// 1 条子扫描线 + 端点四舍五入到整像素 + 渐变按级切段。
int BuildPathFillBands(const PaintPath& path, bool evenOdd, float clipX0, float clipY0,
                       float clipX1, float clipY1, const PathShade& shade, PaintBand* out,
                       int cap, int rowStep, bool* truncated) {
    (void)rowStep;   // 保留只为签名兼容：bin2d 的覆盖率没有条带上限，降级路径已删
    bin2d::CoverageRect& cov = ThreadCoverage();
    if (!Rasterize(path, evenOdd, clipX0, clipY0, clipX1, clipY1,
                   bin2d::CoverageStyle::HardEdge, 1, cov))
        return 0;
    return EmitBands(cov, shade, out, cap, truncated, /*splitX=*/true);
}

// 覆盖率 AA 填充：4 条竖直子采样（与迁移前的老实现同一个数，输出逐位对齐）。
int BuildPathFillBandsAA(const PaintPath& path, bool evenOdd, float clipX0, float clipY0,
                         float clipX1, float clipY1, const PathShade& shade, PaintBand* out,
                         int cap, bool* truncated) {
    bin2d::CoverageRect& cov = ThreadCoverage();
    if (!Rasterize(path, evenOdd, clipX0, clipY0, clipX1, clipY1,
                   bin2d::CoverageStyle::Fractional, 4, cov))
        return 0;
    // AA 路径不按 x 切段：老实现这里就是"一条带一个颜色"（渐变填充走的是硬边路径）
    return EmitBands(cov, shade, out, cap, truncated, /*splitX=*/false);
}

// 描边（v0.22.0）：几何与覆盖率都来自 bin2d 的描边覆盖率（与它的 StrokePath 同源），
// 本文件只把并集覆盖率拼成条带。
//
// 三处口径要记牢（都是"不这么写就会变样"的点）：
//   1. subSamples = 4：与填充 AA 同一个数，也是迁移前老描边（逐段 BuildLineBandsAA）的数；
//   2. splitX = true：描边跨行跨列都比填充长（一根横线一条带能横贯几百像素），渐变不按 x
//      切段的话整根线会取一个颜色 —— 填充 AA 能关是因为渐变填充走的是硬边路径；
//   3. BEVEL = miterLimit 1：bin2d 的斜接在 ratio > limit 时退化成斜面，而直角处
//      ratio = √2 > 1 —— 这正是 SVG 规范里 bevel 的定义，所以不必让 bin2d 认识 bevel。
int BuildPathStrokeBands(const PaintPath& path, float width, uint8_t cap, uint8_t join,
                         uint8_t miterLimit, float clipX0, float clipY0, float clipX1,
                         float clipY1, const PathShade& shade, PaintBand* out, int maxBands,
                         bool* truncated) {
    if (truncated) *truncated = false;
    if (!path.Valid() || width <= 0.0f) return 0;
    if (clipX1 <= clipX0 || clipY1 <= clipY0) return 0;

    bin2d::StrokeCoverageRequest req;
    req.xy = path.pts;
    req.first = path.first;
    req.count = path.count;
    req.closed = path.closed;
    req.contours = path.contours;
    req.style.width = width;
    req.style.roundCap = (cap == BINUI_CAP_ROUND);
    req.style.squareCap = (cap == BINUI_CAP_SQUARE);
    req.style.roundJoin = (join == BINUI_JOIN_ROUND);
    if (join == BINUI_JOIN_BEVEL) {
        req.style.miterLimit = 1.0f;
    } else {
        req.style.miterLimit = miterLimit > 0 ? static_cast<float>(miterLimit) : 4.0f;
    }
    req.subSamples = 4;
    req.clip = bin2d::Rect(clipX0, clipY0, clipX1 - clipX0, clipY1 - clipY0);

    bin2d::CoverageRect& cov = ThreadCoverage();
    if (!bin2d::RasterizeStrokeCoverage(req, cov)) return 0;
    return EmitBands(cov, shade, out, maxBands, truncated, /*splitX=*/true);
}

} // namespace binui
