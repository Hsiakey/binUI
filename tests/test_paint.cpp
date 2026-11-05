// test_paint.cpp - 圆角九宫格几何（include/binui_paint.h）+ 文本尺寸粗估（binui_text_estimate.h）
//
// 这两块都是"三个后端共用的一份数学"，不依赖窗口/字体，所以能纯 CPU 验证。
// 重点是 docs/issues/01 那条：半径到短边一半时九个块必须仍然精确铺满盒子
// （正圆/胶囊），中段宽高为 0 而不是负数。
#include "binui_paint.h"
#include "binui_text_estimate.h"

#include <cmath>
#include <cstdio>

using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECK_NEAR(a, b, eps, msg) \
    do { if (std::fabs((a) - (b)) > (eps)) { \
        std::printf("FAIL: %s (%.4f vs %.4f)\n", msg, (double)(a), (double)(b)); g_fail++; } } while (0)

// 九块面积之和：等于 w*h 就说明既没有缝也没有重叠（九个块按 (c, mid, c) 正交切分）
static float SumArea(const PaintQuad* q, int n) {
    float s = 0;
    for (int i = 0; i < n; i++) s += q[i].w * q[i].h;
    return s;
}

static int CheckTiling(float w, float h, float r, int expectN, const char* tag) {
    PaintQuad q[9];
    const int t = RoundTemplateSize(static_cast<int>(r));
    const int n = BuildRoundedNineSlice(0, 0, w, h, r, t, q, 9);
    if (n != expectN) {
        std::printf("FAIL: %s 块数 %d != 期望 %d\n", tag, n, expectN);
        g_fail++;
        return n;
    }
    for (int i = 0; i < n; i++) {
        if (!(q[i].w > 0.0f) || !(q[i].h > 0.0f)) {
            std::printf("FAIL: %s 第 %d 块宽或高不为正 (%.2f x %.2f)\n", tag, i, q[i].w, q[i].h);
            g_fail++;
        }
        if (!(q[i].u0 >= -1e-6f) || !(q[i].u1 <= 1.0f + 1e-6f) ||
            !(q[i].v0 >= -1e-6f) || !(q[i].v1 <= 1.0f + 1e-6f)) {
            std::printf("FAIL: %s 第 %d 块 UV 出界\n", tag, i);
            g_fail++;
        }
    }
    const float area = SumArea(q, n);
    CHECK_NEAR(area, w * h, 1e-3f, tag);
    return n;
}

int main() {
    // ---------- 模板尺寸 ----------
    CHECK(RoundTemplateSize(1) == 8, "r=1 时模板至少 8px");
    CHECK(RoundTemplateSize(50) == 104, "r=50 时模板 = 2r+4 = 104");
    CHECK(RoundTemplateSize(200) < 4 * 200, "模板不再按 4r 走（省显存）");
    for (int r = 1; r <= 300; r++)
        CHECK(RoundTemplateSize(r) >= 2 * r + 4, "模板边长必须 >= 2r+4（中段要有直边可采）");

    // ---------- 常规圆角：九块 ----------
    CheckTiling(200.0f, 120.0f, 12.0f, 9, "200x120 r12 应九块铺满");
    // h=120、r=60：纵向中段为 0（左右两块与中心消失），只剩四角 + 上下两条
    CheckTiling(200.0f, 120.0f, 60.0f, 6, "200x120 r60（=短边一半）纵向中段应消失");

    // ---------- 正圆：只有四个 1/4 圆 ----------
    {
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(10.0f, 20.0f, 100.0f, 100.0f, 50.0f,
                                            RoundTemplateSize(50), q, 9);
        CHECK(n == 4, "100x100 r50 应只剩四块（中段宽高为 0）");
        CHECK_NEAR(SumArea(q, n), 10000.0f, 1e-3f, "正圆四块面积之和 = 10000");
        // 四角位置必须贴着盒子四角，边长为半径 r
        CHECK_NEAR(q[0].x, 10.0f, 1e-6f, "左上块 x");
        CHECK_NEAR(q[0].y, 20.0f, 1e-6f, "左上块 y");
        CHECK_NEAR(q[0].w, 50.0f, 1e-6f, "角片边长 = 半径");
        CHECK_NEAR(q[3].x + q[3].w, 110.0f, 1e-6f, "右下块右边界贴盒子右边");
        CHECK_NEAR(q[3].y + q[3].h, 120.0f, 1e-6f, "右下块下边界贴盒子下边");
        // 四个角互不重叠：左上块的右边界 == 右上块的左边界
        CHECK_NEAR(q[0].x + q[0].w, q[1].x, 1e-6f, "左右两块在 x 方向首尾相接");
        const float cu = 50.0f / static_cast<float>(RoundTemplateSize(50));
        CHECK_NEAR(q[0].u1 - q[0].u0, cu, 1e-6f, "角片 UV 跨度 = r/t");
    }

    // ---------- 胶囊：横向伸直，纵向中段为 0 ----------
    {
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(0.0f, 0.0f, 200.0f, 40.0f, 20.0f,
                                            RoundTemplateSize(20), q, 9);
        CHECK(n == 6, "200x40 r20 胶囊应为 6 块（上下两条中段消失）");
        CHECK_NEAR(SumArea(q, n), 8000.0f, 1e-3f, "胶囊面积之和 = 200x40");
    }

    // ---------- 半径超过短边一半：钳到一半，不产生负尺寸 ----------
    {
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(0.0f, 0.0f, 34.0f, 34.0f, 999.0f,
                                            RoundTemplateSize(999), q, 9);
        CHECK(n == 4, "34x34 r999 应钳成 r17 的正圆（四块）");
        CHECK_NEAR(SumArea(q, n), 34.0f * 34.0f, 1e-3f, "钳制后面积不变");
    }

    // ---------- 退化输入 ----------
    {
        PaintQuad q[9];
        CHECK(BuildRoundedNineSlice(0, 0, 100, 100, 0.0f, 16, q, 9) == 0, "r=0 不产生块");
        CHECK(BuildRoundedNineSlice(0, 0, 0, 100, 8.0f, 16, q, 9) == 0, "w=0 不产生块");
        CHECK(BuildRoundedNineSlice(0, 0, 100, 100, 8.0f, 16, nullptr, 9) == 0, "out 为空不崩");
    }

    // ---------- 文本尺寸粗估 ----------
    {
        // 13px 中文：4 个字 ≈ 52px，300px 宽 → 1 行
        CHECK(EstimateLineCount("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97\xE5\xBD\xA2", 13.0f, 300.0f) == 1,
              "4 个汉字在 300px 里是一行");
        // 拉丁 0.52×：13*0.52=6.76，55 个字符 ≈ 372px → 需要 2 行
        const std::string lat(55, 'a');
        CHECK(EstimateLineCount(lat, 13.0f, 300.0f) == 2, "55 个拉丁字符在 300px 里是两行");
        // 硬换行
        CHECK(EstimateLineCount("a\nb\nc", 13.0f, 300.0f) == 3, "两个 \\n 断成三行");
        // 行距：默认 1.35×，显式 200% 翻倍
        CHECK_NEAR(EstimateLineStep(20.0f, 0), 27.0f, 1e-4f, "默认行距 1.35×");
        CHECK_NEAR(EstimateLineStep(20.0f, 200), 40.0f, 1e-4f, "line-height:200% = 2×字号");
        CHECK_NEAR(EstimateTextHeight("a\nb", 20.0f, 300.0f, 0), 54.0f, 1e-4f, "两行 = 2×27");
        // 极窄可用宽度不能除零/死循环
        CHECK(EstimateLineCount("\xE4\xB8\xAD\xE6\x96\x87", 13.0f, 0.0f) >= 1, "可用宽度为 0 时仍返回正行数");
    }

    if (g_fail == 0) std::printf("test_paint: 全部通过\n");
    return g_fail;
}
