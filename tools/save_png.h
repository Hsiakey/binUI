// save_png.h - 把 BGRA 像素写成 PNG（GDI+）
//
// 抓图工具共用（vk_shot / vk_window / gl_shot 都是同一套需求），避免三份拷贝。
// 输入是**预乘 alpha 的 BGRA**（后端读回的格式，行序自上而下）；
// GDI+ 的 PixelFormat32bppARGB 要的是直通 alpha，所以半透明像素要反预乘。
#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace binuishot {

inline int GetEncoderClsid(const wchar_t* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return 0;
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; i++) {
        if (std::wcscmp(codecs[i].MimeType, format) == 0) {
            *pClsid = codecs[i].Clsid;
            return 1;
        }
    }
    return 0;
}

inline bool SaveBgraPng(const char* path, const uint8_t* bgra, int w, int h) {
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData bd{};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = bgra + static_cast<size_t>(y) * w * 4;
        auto* dst = reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(bd.Scan0) + y * bd.Stride);
        for (int x = 0; x < w; x++) {
            const uint8_t a = src[x * 4 + 3];
            dst[x * 4 + 0] = src[x * 4 + 0];   // B
            dst[x * 4 + 1] = src[x * 4 + 1];   // G
            dst[x * 4 + 2] = src[x * 4 + 2];   // R
            dst[x * 4 + 3] = a;
        }
    }
    bmp.UnlockBits(&bd);
    CLSID png;
    if (GetEncoderClsid(L"image/png", &png) == 0) return false;
    const std::wstring wp(path, path + std::strlen(path));
    return bmp.Save(wp.c_str(), &png, nullptr) == Gdiplus::Ok;
}

} // namespace binuishot
