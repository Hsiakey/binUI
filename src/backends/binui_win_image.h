// binui_win_image.h - Windows 图片解码（GDI+）：PNG/JPEG 字节 → BGRA8
//
// GL 与 Vulkan 后端共用（原本两份拷贝各在 gl / d3d11 里）。d3d11 那条链路还没接
// Makefile，暂不动它。
#ifndef BINUI_WIN_IMAGE_H
#define BINUI_WIN_IMAGE_H

#ifndef _WIN32
#error "binui_win_image.h 是 Windows 专用（GDI+）"
#endif

#include <windows.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace binui {

// GDI+ 进程级初始化（只需一次；失败后所有解码返回空）
inline bool gdiplusReady() {
    static bool ok = [] {
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

// PNG/JPEG 原始字节 -> BGRA8 像素（失败返回空）
inline std::vector<uint8_t> DecodeImagePixels(const uint8_t* data, size_t size, int& w, int& h) {
    w = h = 0;
    if (!gdiplusReady() || !data || size == 0) return {};

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return {};
    {
        void* p = GlobalLock(hg);
        if (p) { std::memcpy(p, data, size); GlobalUnlock(hg); }
    }
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK) { GlobalFree(hg); return {}; }
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return {}; }

    w = static_cast<int>(bmp->GetWidth());
    h = static_cast<int>(bmp->GetHeight());
    std::vector<uint8_t> px;
    if (w > 0 && h > 0) {
        Gdiplus::Rect r(0, 0, w, h);
        Gdiplus::BitmapData bd;
        if (bmp->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) ==
            Gdiplus::Ok) {
            px.resize(static_cast<size_t>(w) * h * 4);
            const uint8_t* src = static_cast<const uint8_t*>(bd.Scan0);
            uint8_t* dst = px.data();
            for (int y = 0; y < h; y++) {
                std::memcpy(dst + static_cast<size_t>(y) * w * 4,
                            src + static_cast<size_t>(y) * bd.Stride,
                            static_cast<size_t>(w) * 4);
            }
            bmp->UnlockBits(&bd);
        }
    }
    delete bmp;
    return px;
}

} // namespace binui

#endif // BINUI_WIN_IMAGE_H
