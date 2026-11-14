// binui_mip.h - mip 链生成（纯 CPU，header-only）
//
// 为什么要有这一份：三个 GPU 后端生成 mip 的路径不一样 ——
//   · GL / D3D11：硬件 `glGenerateMipmap` / `GenerateMips`（2×2 平均，驱动做的）
//   · Vulkan：这里**在 CPU 上做 2×2 盒式滤波再逐级上传**（后端的纹理记账模型是
//     "一张图一个布局状态"，blit 链要逐级切布局，反而更容易错）
// 两条路必须等价，否则"同一张图缩小时三端看着不一样"。硬件那条改不了，所以 CPU 这条
// 的公式在这里写死、单测钉住：第 l 级像素 = 上一级 2×2 的**四舍五入平均**
// （(a+b+c+d+2)/4），边缘按 min(w-1, …) 夹取（奇数尺寸时最后一行/列复制）。
//
// 另外它也是"视频帧/程序化纹理要不要 mip"的判据落点：见各后端的 prepareImageRaw。
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace binui {

// BGRA8 图（字节序与 0xAARRGGBB 的内存序一致，也是各后端上传用的格式）的 mip 层数：
// 一直减半到 1×1，最多 16 级（65536 已到极限）。
inline uint32_t MipLevels(int w, int h) {
    uint32_t levels = 1;
    int m = std::max(w, h);
    while (m > 1 && levels < 16) { m >>= 1; levels++; }
    return levels;
}

// 生成完整 mip 链（含第 0 级）。src 是 w*h*4 的 BGRA8；返回 levels 个缓冲，
// 每个依次是上一级的一半（1×1 为止）。参数不合法返回空。
inline std::vector<std::vector<uint8_t>> BuildMipChainBGRA(const uint8_t* src, int w, int h) {
    std::vector<std::vector<uint8_t>> out;
    if (!src || w <= 0 || h <= 0) return out;
    const uint32_t levels = MipLevels(w, h);
    out.reserve(levels);
    out.emplace_back(src, src + static_cast<size_t>(w) * h * 4);
    int cw = w, ch = h;
    for (uint32_t l = 1; l < levels; l++) {
        const int nw = std::max(1, cw / 2), nh = std::max(1, ch / 2);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        const std::vector<uint8_t>& cur = out.back();
        for (int y = 0; y < nh; y++) {
            const int y0 = std::min(ch - 1, y * 2), y1 = std::min(ch - 1, y * 2 + 1);
            for (int x = 0; x < nw; x++) {
                const int x0 = std::min(cw - 1, x * 2), x1 = std::min(cw - 1, x * 2 + 1);
                for (int c = 0; c < 4; c++) {
                    const uint32_t s = cur[(static_cast<size_t>(y0) * cw + x0) * 4 + c] +
                                       cur[(static_cast<size_t>(y0) * cw + x1) * 4 + c] +
                                       cur[(static_cast<size_t>(y1) * cw + x0) * 4 + c] +
                                       cur[(static_cast<size_t>(y1) * cw + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] =
                        static_cast<uint8_t>((s + 2) / 4);
                }
            }
        }
        out.push_back(std::move(next));
        cw = nw;
        ch = nh;
    }
    return out;
}

} // namespace binui
