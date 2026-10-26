// window_capture.cpp - 抓取指定窗口内容为 PNG（PrintWindow + GDI+）
// 用法: window_capture <窗口标题子串> <输出.png>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <windows.h>
#include <gdiplus.h>

using namespace Gdiplus;

static HWND g_found = nullptr;
static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    char t[256];
    GetWindowTextA(hwnd, t, sizeof(t));
    if (strstr(t, (const char*)lp)) { g_found = hwnd; return FALSE; }
    return TRUE;
}
// 按类名匹配（前缀 "class:"）
static BOOL CALLBACK EnumProcClass(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    char t[256];
    GetClassNameA(hwnd, t, sizeof(t));
    if (strcmp(t, (const char*)lp) == 0) { g_found = hwnd; return FALSE; }
    return TRUE;
}

static int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<ImageCodecInfo> codecs(size / sizeof(ImageCodecInfo));
    GetImageEncoders(num, size, codecs.data());
    for (UINT i = 0; i < num; i++)
        if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; return 0; }
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: window_capture <title sub> <out.png>\n"); return 1; }
    GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &in, nullptr);

    for (int tries = 0; tries < 100 && !g_found; tries++) {
        if (strncmp(argv[1], "class:", 6) == 0)
            EnumWindows(EnumProcClass, (LPARAM)(argv[1] + 6));
        else
            EnumWindows(EnumProc, (LPARAM)argv[1]);
        if (!g_found) Sleep(100);
    }
    if (!g_found) { printf("window not found\n"); return 1; }

    // 窗口完整尺寸（含标题栏/边框）
    RECT wr;
    GetWindowRect(g_found, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;

    HDC winDC = GetDC(g_found);
    HDC memDC = CreateCompatibleDC(winDC);
    HBITMAP bmp = CreateCompatibleBitmap(winDC, w, h);
    HGDIOBJ old = SelectObject(memDC, bmp);
    // PrintWindow 抓取窗口内容（包括离屏缓冲绘制的部分）
    BOOL ok = PrintWindow(g_found, memDC, PW_RENDERFULLCONTENT);

    // PrintWindow 对 D3D 窗口可能抓黑：内容全黑时回退 BitBlt（屏幕合成内容）
    bool black = false;
    {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        if (GetDIBits(winDC, bmp, 0, h, px.data(), &bmi, DIB_RGB_COLORS)) {
            size_t nz = 0;
            for (uint32_t v : px) if (v != 0) nz++;
            if (nz == 0) black = true;
        }
    }
    if (!ok || black) {
        // D3D 窗口客户区不经 GDI：从屏幕 DC 抓桌面合成后的内容
        RECT wr2;
        GetWindowRect(g_found, &wr2);
        HDC screenDC = GetDC(nullptr);
        BitBlt(memDC, 0, 0, w, h, screenDC, wr2.left, wr2.top, SRCCOPY);
        ReleaseDC(nullptr, screenDC);
    }

    // 裁剪客户区：PrintWindow 坐标相对窗口左上角，客户区起点 = 窗口左上角偏移
    RECT cl;
    GetClientRect(g_found, &cl);
    POINT pt = { 0, 0 };
    ClientToScreen(g_found, &pt);
    int cx = pt.x - wr.left, cy = pt.y - wr.top;
    int cw = cl.right - cl.left, chh = cl.bottom - cl.top;

    Gdiplus::Bitmap gbmp(bmp, nullptr);
    Gdiplus::Bitmap* cropped = gbmp.Clone(cx, cy, cw, chh, PixelFormat32bppARGB);
    CLSID png;
    if (GetEncoderClsid(L"image/png", &png) == 0) {
        wchar_t wp[512];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wp, 512);
        if (cropped && cropped->Save(wp, &png, nullptr) == Ok)
            printf("captured %s (%dx%d crop %d,%d %dx%d) ok=%d\n",
                   argv[2], w, h, cx, cy, cw, chh, (int)ok);
        else printf("save failed\n");
    }
    delete cropped;
    SelectObject(memDC, old);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(g_found, winDC);
    GdiplusShutdown(token);
    return 0;
}
