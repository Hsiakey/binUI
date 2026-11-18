// ============================================================
// tools/migrate_gate.cpp —— 迁移开工条件（"UI 最大节点"单帧耗时）
//
// 为什么要有这个工具：bin2d 的 docs/layering.md §4 把迁移的**开工条件**写死成
// "UI 最大节点（800×600 卡片 + 200 个图元）单帧 ≤ 现实现 × 1.2"。这条不满足就
// 不许动 binUI 的路径光栅 —— 否则就是把"性能问题"搬进 UI，而不是把"几何能力"搬过来。
//
// 两条路线**同口径**：都从同一份设备空间折线出发，都落到同一块内存里的 Surface，
// 都不含布局/遍历（那部分由 draw_bench 量，两边一样）：
//   A) binUI 现状：BuildPathFillBandsAA（路径 → 条带）+ 后端 fillRect（条带 → 像素）
//      —— 这就是 GDI 后端今天做的事；GPU 后端把每条带变成一个四边形，CPU 成本只到 A 的一半。
//   B) bin2d 目标：Path + FillPath（扫描线 + 覆盖率 + 混合一次到底）
//
// 三个必须注意的口径细节（踩过，数字会差 10 倍）：
//   1. **几何必须是设备空间**：binUI 的条带构建器要的是设备坐标 + 设备裁剪框，
//      喂局部坐标会得到"几乎零条带"的假数字；
//   2. **裁剪框给形状自己的 box**，不是整个窗口 —— 传窗口会让条带构建器按窗口宽度
//      分配覆盖率累加器，单形状开销被放大 10 倍以上；
//   3. 两边都包含清屏（真实帧都要清），且都用同一块 1280×720 内存 Surface。
//
// 用法：make gate-migrate      （需要 ../bin2D 已构建出 libbin2d.a）
// ============================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "binui_paint.h"

#include "bin2d/canvas.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/surface.h"

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;
constexpr int kIcon = 120;          // 图标边长（bench-path 的 120px 档）
constexpr uint32_t kBg = 0xFF101418u;
constexpr uint32_t kCard = 0xE61B2430u;
constexpr uint32_t kIconColor = 0xFF3E8BFFu;

double NowUs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double, std::micro>(clock::now() - t0).count();
}

// binUI 的几何表示：设备坐标折线
struct DevPath {
    std::vector<float> xy;
    std::vector<uint32_t> first{0};
    std::vector<uint32_t> count;
    std::vector<uint8_t> closed{1};
    binui::PaintPath view() const {
        return binui::PaintPath{xy.data(), first.data(), count.data(), closed.data(),
                                static_cast<int>(first.size())};
    }
};

// 与 binUI 的 path_bench 同一批形状（星形 10 点 / 齿轮 16 齿 / 圆 64 段）
void EmitStar(std::vector<float>& xy, float size, float ox, float oy) {
    for (int i = 0; i < 10; ++i) {
        const float ang = -1.5707963f + static_cast<float>(i) * 0.62831853f;
        const float r = (i & 1) ? size * 0.22f : size * 0.48f;
        xy.push_back(ox + size * 0.5f + std::cos(ang) * r);
        xy.push_back(oy + size * 0.5f + std::sin(ang) * r);
    }
}

void EmitGear(std::vector<float>& xy, float size, float ox, float oy) {
    const int n = 16 * 4;
    for (int i = 0; i < n; ++i) {
        const float ang = static_cast<float>(i) * 6.2831853f / static_cast<float>(n);
        const int phase = i % 4;
        const float r = (phase == 1 || phase == 2) ? size * 0.46f : size * 0.34f;
        xy.push_back(ox + size * 0.5f + std::cos(ang) * r);
        xy.push_back(oy + size * 0.5f + std::sin(ang) * r);
    }
}

void EmitCircle(std::vector<float>& xy, float size, float ox, float oy) {
    const int n = 64;
    for (int i = 0; i < n; ++i) {
        const float ang = static_cast<float>(i) * 6.2831853f / static_cast<float>(n);
        xy.push_back(ox + size * 0.5f + std::cos(ang) * size * 0.48f);
        xy.push_back(oy + size * 0.5f + std::sin(ang) * size * 0.48f);
    }
}

// 圆角矩形折线（16 段/角；两边用同一份点，避免"几何本身就不一样"）
void EmitRoundRect(std::vector<float>& xy, float x, float y, float w, float h, float rad) {
    const int seg = 16;
    const float cx[4] = {x + w - rad, x + w - rad, x + rad, x + rad};
    const float cy[4] = {y + rad, y + h - rad, y + h - rad, y + rad};
    const float a0[4] = {-1.5707963f, 0.0f, 1.5707963f, 3.1415927f};
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= seg; ++i) {
            const float ang = a0[c] + static_cast<float>(i) * 1.5707963f / static_cast<float>(seg);
            xy.push_back(cx[c] + std::cos(ang) * rad);
            xy.push_back(cy[c] + std::sin(ang) * rad);
        }
    }
}

bin2d::Path ToBin2d(const std::vector<float>& xy, float ox, float oy) {
    bin2d::Path p;
    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        const float x = xy[i] - ox, y = xy[i + 1] - oy;
        if (i == 0) p.moveTo(x, y);
        else p.lineTo(x, y);
    }
    p.close();
    return p;
}

// ---------------- 后端 fillRect 的等价物 ----------------
inline uint32_t Div255(uint32_t v) { return (v + 127u) / 255u; }

void FillBand(bin2d::Surface& dst, const binui::PaintBand& b) {
    const int x0 = std::max(0, static_cast<int>(std::floor(b.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(b.y)));
    const int x1 = std::min(dst.width(), static_cast<int>(std::ceil(b.x + b.w)));
    const int y1 = std::min(dst.height(), static_cast<int>(std::ceil(b.y + b.h)));
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t argb = b.color;
    const uint32_t a = (argb >> 24) & 0xFFu;
    if (a == 0) return;
    const uint8_t sr = static_cast<uint8_t>(((argb >> 16) & 0xFFu) * a / 255u);
    const uint8_t sg = static_cast<uint8_t>(((argb >> 8) & 0xFFu) * a / 255u);
    const uint8_t sb = static_cast<uint8_t>((argb & 0xFFu) * a / 255u);
    const uint32_t ia = 255u - a;
    for (int y = y0; y < y1; ++y) {
        uint8_t* d = dst.row(y) + static_cast<size_t>(x0) * 4;
        if (a == 255) {
            for (int x = x0; x < x1; ++x, d += 4) {
                d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255;
            }
        } else {
            for (int x = x0; x < x1; ++x, d += 4) {
                d[0] = static_cast<uint8_t>(sr + Div255(d[0] * ia));
                d[1] = static_cast<uint8_t>(sg + Div255(d[1] * ia));
                d[2] = static_cast<uint8_t>(sb + Div255(d[2] * ia));
                d[3] = static_cast<uint8_t>(a + Div255(d[3] * ia));
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const int icons = (argc > 1) ? std::atoi(argv[1]) : 200;

    // ---- 预建几何（真实 UI 里这是解析/布局阶段的产物，不计入单帧）----
    std::vector<DevPath> dev;
    std::vector<bin2d::Path> b2d;
    std::vector<float> ox(static_cast<size_t>(icons)), oy(static_cast<size_t>(icons));
    for (int i = 0; i < icons; ++i) {
        // 16 列铺开，行距 = 图标边长（不刻意制造重叠：真实界面的图标基本不叠）
        ox[static_cast<size_t>(i)] = 24.0f + static_cast<float>((i % 16) * 76);
        oy[static_cast<size_t>(i)] = 24.0f + static_cast<float>((i / 16) % 9 * 76);
        DevPath d;
        const int kind = i % 3;
        if (kind == 0) EmitStar(d.xy, kIcon, ox[static_cast<size_t>(i)], oy[static_cast<size_t>(i)]);
        else if (kind == 1) EmitGear(d.xy, kIcon, ox[static_cast<size_t>(i)], oy[static_cast<size_t>(i)]);
        else EmitCircle(d.xy, kIcon, ox[static_cast<size_t>(i)], oy[static_cast<size_t>(i)]);
        d.count.push_back(static_cast<uint32_t>(d.xy.size() / 2));
        dev.push_back(std::move(d));
        b2d.push_back(ToBin2d(dev.back().xy, ox[static_cast<size_t>(i)], oy[static_cast<size_t>(i)]));
    }
    std::vector<float> cardXY;
    EmitRoundRect(cardXY, 40.0f, 40.0f, 800.0f, 600.0f, 24.0f);
    DevPath cardDev;
    cardDev.xy = cardXY;
    cardDev.count.push_back(static_cast<uint32_t>(cardXY.size() / 2));
    const bin2d::Path cardB2d = ToBin2d(cardXY, 0.0f, 0.0f);

    std::vector<binui::PaintBand> bands(8192);
    std::vector<uint16_t> cov(2048);
    bin2d::Surface dst(kW, kH);

    // ---- A1：只建条带（几何成本）----
    auto bandsOnly = [&]() {
        int total = 0;
        bool trunc = false;
        const binui::PaintPath cp = cardDev.view();
        total += binui::BuildPathFillBandsAA(cp, false, 0.0f, 0.0f, static_cast<float>(kW),
                                             static_cast<float>(kH), binui::PathShade{},
                                             bands.data(), static_cast<int>(bands.size()), cov,
                                             &trunc);
        for (int i = 0; i < icons; ++i) {
            const float x = ox[static_cast<size_t>(i)], y = oy[static_cast<size_t>(i)];
            total += binui::BuildPathFillBandsAA(dev[static_cast<size_t>(i)].view(), false, x, y,
                                                 x + kIcon, y + kIcon, binui::PathShade{},
                                                 bands.data(), static_cast<int>(bands.size()),
                                                 cov, &trunc);
        }
        return total;
    };
    // ---- A2：条带 + 落像素（与 B 同口径）----
    auto bandsAndFill = [&]() {
        dst.clear(bin2d::Color::argb(kBg));
        bool trunc = false;
        const binui::PaintPath cp = cardDev.view();
        int n = binui::BuildPathFillBandsAA(cp, false, 0.0f, 0.0f, static_cast<float>(kW),
                                            static_cast<float>(kH), binui::PathShade{},
                                            bands.data(), static_cast<int>(bands.size()), cov,
                                            &trunc);
        for (int i = 0; i < n; ++i) FillBand(dst, bands[static_cast<size_t>(i)]);
        for (int i = 0; i < icons; ++i) {
            const float x = ox[static_cast<size_t>(i)], y = oy[static_cast<size_t>(i)];
            n = binui::BuildPathFillBandsAA(dev[static_cast<size_t>(i)].view(), false, x, y,
                                            x + kIcon, y + kIcon, binui::PathShade{},
                                            bands.data(), static_cast<int>(bands.size()), cov,
                                            &trunc);
            for (int k = 0; k < n; ++k) FillBand(dst, bands[static_cast<size_t>(k)]);
        }
    };
    // ---- B：bin2d（路径 → 覆盖率 → 像素一次到底）----
    auto b2dWay = [&]() {
        dst.clear(bin2d::Color::argb(kBg));
        bin2d::Canvas c(dst);
        c.fillPath(cardB2d, bin2d::Color::argb(kCard));
        for (int i = 0; i < icons; ++i) {
            bin2d::Canvas cc(dst);
            cc.setTransform(bin2d::Transform::translate(ox[static_cast<size_t>(i)],
                                                        oy[static_cast<size_t>(i)]));
            cc.fillPath(b2d[static_cast<size_t>(i)], bin2d::Color::argb(kIconColor));
        }
    };
    auto bestOf = [](auto&& fn) {
        fn();
        double best = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const double t0 = NowUs();
            (void)fn();
            best = std::min(best, NowUs() - t0);
        }
        return best;
    };

    const int nBands = bandsOnly();
    const double a1 = bestOf(bandsOnly);
    const double a2 = bestOf(bandsAndFill);
    const double b = bestOf(b2dWay);
    const double ratio = b / a2;

    std::printf("场景：800x600 圆角卡片 + %d 个 %dpx 图标（%d 条带/帧）\n", icons, kIcon, nBands);
    std::printf("  A1 binUI 只建条带                 : %7.2f ms\n", a1 / 1000.0);
    std::printf("  A2 binUI 条带 + 落像素（同口径）   : %7.2f ms\n", a2 / 1000.0);
    std::printf("  B  bin2d FillPath                 : %7.2f ms\n", b / 1000.0);
    std::printf("  B / A2 = %.2f×  → %s（开工条件 ≤ 1.20×）\n", ratio,
                ratio <= 1.20 ? "达标" : "未达标");
    return 0;
}
