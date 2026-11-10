// pixel_diff - 两张 PNG 的逐像素比较（像素回归用）
//
// 用法:
//   pixel_diff <a.png> <b.png> [容差百分比] [diff输出.png]
//
// 退出码: 0 = 在容差内；1 = 超差；2 = 参数/解码错误。
//
// 为什么是独立工具而不是塞进 CI：CI 的 Windows runner 是软件光栅化，抗锯齿边缘与
// 真实 GPU 不一致；同一份代码在不同机器上会稳定地差出百分之几的像素。所以基准图
// 只在**本机**有意义 —— 改完绘制/布局后跑一次 `make pixels`，看差异率和差异区域。
//
// 比较口径：
//   · 每个像素逐通道取最大差，超过阈值才算"不同像素"（默认 8，容忍 1px AA 抖动）；
//   · 报告差异率、最大通道差、差异包围盒；
//   · 尺寸不一致直接判失败。
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace {

ULONG_PTR g_gdiplus = 0;

bool GdiplusInit() {
    Gdiplus::GdiplusStartupInput in;
    return Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr) == Gdiplus::Ok;
}

void GdiplusShutdown() {
    if (g_gdiplus) Gdiplus::GdiplusShutdown(g_gdiplus);
    g_gdiplus = 0;
}

// 载入 PNG/JPEG 到 ARGB（0xAARRGGBB），行序自上而下
bool LoadPixels(const char* path, int& w, int& h, std::vector<uint32_t>& px) {
    MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    wchar_t wp[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024);
    Gdiplus::Bitmap bmp(wp, FALSE);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    w = static_cast<int>(bmp.GetWidth());
    h = static_cast<int>(bmp.GetHeight());
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    px.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(bd.Scan0) + static_cast<size_t>(y) * bd.Stride);
        std::memcpy(px.data() + static_cast<size_t>(y) * w, src, sizeof(uint32_t) * w);
    }
    bmp.UnlockBits(&bd);
    return true;
}

bool SavePixels(const char* path, int w, int h, const std::vector<uint32_t>& px) {
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    for (int y = 0; y < h; y++) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(bd.Scan0) + static_cast<size_t>(y) * bd.Stride);
        std::memcpy(dst, px.data() + static_cast<size_t>(y) * w, sizeof(uint32_t) * w);
    }
    bmp.UnlockBits(&bd);
    CLSID png;
    if (CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png) != S_OK) return false;
    wchar_t wp[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024);
    // MinGW 的 GDI+ 头没给 encoderParams 默认值，这里显式传 nullptr
    return bmp.Save(wp, &png, nullptr) == Gdiplus::Ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: pixel_diff <a.png> <b.png> [tolerance_pct] [diff_out.png] "
                    "[channel_tol]\n");
        return 2;
    }
    const double tolPct = (argc > 3) ? std::atof(argv[3]) : 0.5;   // 默认 0.5%
    // 单通道容差：默认 8（容忍 1px AA 抖动）。GPU 后端的对比要给大一点 ——
    // GL 与 Vulkan 的 MSAA 在斜边上的采样归属不同，差值是"一个采样点"量级
    //（约 25% 覆盖率，深色底上 50~120 通道值），因此 VK↔GL 用 128 并配合
    // 容忍像素比例；这不是放宽标准，是把已知的、对称的差异写进判据。
    const int channelTol = (argc > 5) ? std::atoi(argv[5]) : 8;

    if (!GdiplusInit()) { std::printf("GDI+ 初始化失败\n"); return 2; }

    int aw = 0, ah = 0, bw = 0, bh = 0;
    std::vector<uint32_t> a, b;
    if (!LoadPixels(argv[1], aw, ah, a)) { std::printf("读不了 %s\n", argv[1]); GdiplusShutdown(); return 2; }
    if (!LoadPixels(argv[2], bw, bh, b)) { std::printf("读不了 %s\n", argv[2]); GdiplusShutdown(); return 2; }
    if (aw != bw || ah != bh) {
        std::printf("尺寸不一致: %dx%d vs %dx%d\n", aw, ah, bw, bh);
        GdiplusShutdown();
        return 1;
    }

    const size_t n = a.size();
    size_t diff = 0;
    int maxDelta = 0;
    int l = aw, t = ah, r = -1, bot = -1;
    std::vector<uint32_t> out;
    if (argc > 4) out.assign(n, 0);

    for (int y = 0; y < ah; y++) {
        for (int x = 0; x < aw; x++) {
            const size_t i = static_cast<size_t>(y) * aw + x;
            const uint32_t pa = a[i], pb = b[i];
            int d = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                const int ca = static_cast<int>((pa >> sh) & 0xFF);
                const int cb = static_cast<int>((pb >> sh) & 0xFF);
                d = std::max(d, std::abs(ca - cb));
            }
            if (d > channelTol) {
                ++diff;
                l = std::min(l, x); t = std::min(t, y);
                r = std::max(r, x); bot = std::max(bot, y);
            }
            maxDelta = std::max(maxDelta, d);
            if (!out.empty()) {
                // 差异像素画成红色，其余压成 25% 灰底，方便肉眼定位
                out[i] = (d > channelTol) ? 0xFFFF0000u
                                          : (0xFF000000u | ((pa >> 1) & 0x007F7F7Fu));
            }
        }
    }

    const double pct = n ? (100.0 * static_cast<double>(diff) / static_cast<double>(n)) : 0.0;
    std::printf("尺寸 %dx%d，不同像素 %zu (%.3f%%)，最大通道差 %d，阈值 %d（容差 %.2f%%）\n",
                aw, ah, diff, pct, maxDelta, channelTol, tolPct);
    if (diff) {
        std::printf("差异区域: (%d,%d) - (%d,%d)，%dx%d\n",
                    l, t, r, bot, r - l + 1, bot - t + 1);
    }
    if (!out.empty()) {
        if (SavePixels(argv[4], aw, ah, out)) std::printf("diff 图已写出 %s\n", argv[4]);
    }

    GdiplusShutdown();
    const bool ok = pct <= tolPct;
    if (!ok) std::printf("FAIL：差异超过容差\n");
    return ok ? 0 : 1;
}
