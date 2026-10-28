// app_win32.cpp - 极简 Win32 应用外壳 + GDI 节点绘制后端
#include "app_win32.h"

#ifndef _WIN32
#error "app_win32 is Windows-only; build a platform shell for other backends"
#endif

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <objidl.h>
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>

#include "binui_paint.h"

using namespace binui;

namespace {

bool gdiplusReady() {
    static bool ok = [] {
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

// 从原始字节解码 PNG/JPEG 等为 Gdiplus::Bitmap（失败返回 nullptr）
Gdiplus::Bitmap* DecodeImageBitmap(const uint8_t* data, size_t size) {
    if (!gdiplusReady() || !data || size == 0) return nullptr;

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return nullptr;
    {
        void* p = GlobalLock(hg);
        if (p) { memcpy(p, data, size); GlobalUnlock(hg); }
    }

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK) {
        GlobalFree(hg);
        return nullptr;
    }
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(stream);
    stream->Release(); // 释放后 hg 一并释放
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    return bmp;
}

// ---------- UTF-8 / UTF-16 转换 ----------
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// GDI 的 DrawTextW 只把 CRLF 当换行序列：裸 '\n' 会被当成空白吞掉
// （<br> 与 <pre> 在布局里存的是单个 '\n'，不补 '\r' 就整段连成一行）。
std::wstring LfToCrlf(const std::wstring& s) {
    if (s.find(L'\n') == std::wstring::npos) return s;
    std::wstring out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) out += L'\r';
        out += s[i];
    }
    return out;
}

std::string CodepointToUtf8(unsigned cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// 用 XOR 填充矩形（反色高亮，用于选区/光标，任何主题下都可见）
void XorFillRect(HDC hdc, int l, int t, int r, int b) {
    if (r <= l || b <= t) return;
    SetROP2(hdc, R2_NOT);
    PatBlt(hdc, l, t, r - l, b - t, PATCOPY);
    SetROP2(hdc, R2_COPYPEN);
}

struct AppState {
    BinUIApp* app = nullptr;
};

// 手动 resize 循环（窗口无 WS_THICKFRAME，系统不提供 resize，这里自行实现）
static void StartResize(HWND hwnd, int hit, RECT startRect) {
    const int MIN_W = 320, MIN_H = 240;
    SetCapture(hwnd);
    POINT start;
    GetCursorPos(&start);
    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        POINT cur;
        GetCursorPos(&cur);
        RECT r = startRect;
        int dx = cur.x - start.x, dy = cur.y - start.y;
        switch (hit) {
            case HTLEFT:          r.left += dx; break;
            case HTRIGHT:         r.right += dx; break;
            case HTTOP:           r.top += dy; break;
            case HTBOTTOM:        r.bottom += dy; break;
            case HTTOPLEFT:       r.left += dx; r.top += dy; break;
            case HTTOPRIGHT:      r.right += dx; r.top += dy; break;
            case HTBOTTOMLEFT:    r.left += dx; r.bottom += dy; break;
            case HTBOTTOMRIGHT:   r.right += dx; r.bottom += dy; break;
        }
        if (r.right - r.left < MIN_W) {
            if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT)
                r.left = r.right - MIN_W;
            else
                r.right = r.left + MIN_W;
        }
        if (r.bottom - r.top < MIN_H) {
            if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT)
                r.top = r.bottom - MIN_H;
            else
                r.bottom = r.top + MIN_H;
        }
        SetWindowPos(hwnd, nullptr, r.left, r.top,
            r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(1);
    }
    ReleaseCapture();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BinUIApp* app = reinterpret_cast<BinUIApp*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcA(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_NCCALCSIZE: {
            // 去掉 WS_THICKFRAME 的系统边框（顶部白线/焦点高亮线）：
            // wParam=TRUE 时客户区 = 窗口矩形（rgrc[0]），保留样式仅为维持系统 resize。
            // 首次（wParam=FALSE）由创建后 SWP_FRAMECHANGED 触发重算
            if (!wParam) return 0;
            return 0;
        }
        case WM_ACTIVATE: {
            // 跟踪焦点状态：驱动自绘聚焦细边框（无边框窗口没有系统焦点指示）
            bool f = LOWORD(wParam) != WA_INACTIVE;
            if (f != app->focused_) {
                app->focused_ = f;
                app->invalidateAll();
            }
            if (!f) app->OnFocusLost();
            return 0;
        }
        case WM_NCLBUTTONDOWN: {
            // 无 WS_THICKFRAME：命中边框热区时自行启动 resize 循环
            switch (wParam) {
                case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
                case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT:
                case HTBOTTOMRIGHT: {
                    if (IsZoomed(hwnd))
                        return DefWindowProcA(hwnd, msg, wParam, lParam);
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    StartResize(hwnd, static_cast<int>(wParam), wr);
                    return 0;
                }
                default:
                    return DefWindowProcA(hwnd, msg, wParam, lParam);
            }
        }
        case WM_NCHITTEST: {
            // 无边框窗口：边框 8px 热区用于调整大小，其余为客户区
            RECT rc;
            GetWindowRect(hwnd, &rc);
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int bw = 8;
            bool l = pt.x - rc.left < bw, t = pt.y - rc.top < bw;
            bool r = rc.right - pt.x < bw, b = rc.bottom - pt.y < bw;
            if (l && t) return HTTOPLEFT;
            if (r && t) return HTTOPRIGHT;
            if (l && b) return HTBOTTOMLEFT;
            if (r && b) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
            return HTCLIENT;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            app->OnDraw(hdc, &ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            // 首次移动时注册鼠标离开通知（仅当真实光标位于窗口内，
            // 否则 TrackMouseEvent 会因光标在外而立刻触发 WM_MOUSELEAVE 误清悬停）
            if (!app->trackingLeave_) {
                POINT cursor;
                RECT wrect;
                if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &wrect) && PtInRect(&wrect, cursor)) {
                    TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    app->trackingLeave_ = true;
                }
            }
            float mx = static_cast<float>(LOWORD(lParam));
            float my = static_cast<float>(HIWORD(lParam));
            app->OnMouseMove(mx, my);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float mx = static_cast<float>(LOWORD(lParam));
            float my = static_cast<float>(HIWORD(lParam));
            app->OnMousePress(mx, my);
            return 0;
        }
        case WM_LBUTTONUP: {
            float mx = static_cast<float>(LOWORD(lParam));
            float my = static_cast<float>(HIWORD(lParam));
            app->OnMouseRelease(mx, my);
            return 0;
        }
        case WM_MOUSELEAVE:
            app->trackingLeave_ = false;
            app->OnMouseLeave();
            return 0;
        case WM_MOUSEWHEEL: {
            short delta = static_cast<short>(HIWORD(wParam));
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            app->OnMouseWheel(static_cast<float>(pt.x), static_cast<float>(pt.y),
                              static_cast<float>(delta) / 120.0f * 40.0f);
            return 0;
        }
        case WM_CHAR:
            app->OnChar(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            app->OnKey(static_cast<int>(wParam));
            return 0;
        case WM_SETFOCUS:
            if (!app->focused_) {
                app->focused_ = true;
                app->invalidateAll();
            }
            return 0;
        case WM_KILLFOCUS:
            if (app->focused_) {
                app->focused_ = false;
                app->invalidateAll();
            }
            app->OnFocusLost();
            return 0;
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            app->OnResize(w, h);
            return 0;
        }
        case WM_CLOSE:
            app->requestExit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // namespace

// 光标闪烁（500ms 周期），绘制与脏矩形刷新共用同一相位
static std::string gGdiFontName = "Segoe UI";

// 字体的抗锯齿质量。**不能再用 DEFAULT_QUALITY**：那等于"跟随系统设置"，
// 系统开着 ClearType 时 GDI 会做次像素（RGB 分量各自覆盖）渲染，把字画到
// 有色背景的离屏 DIB 上就会渗出红/蓝/黄的毛边 —— 放大看就是"像素没对齐"。
// 两个 GPU 后端走的是 FreeType 灰度字形（单通道 alpha），不会这样；
// 这里用 ANTIALIASED_QUALITY 强制灰度抗锯齿，三端观感一致。
#ifndef BINUI_GDI_FONT_QUALITY
#define BINUI_GDI_FONT_QUALITY ANTIALIASED_QUALITY
#endif
static const int gGdiFontQuality = BINUI_GDI_FONT_QUALITY;

void setGdiFontName(const char* name) { if (name && *name) gGdiFontName = name; }

bool caretBlinkOn() {
    using namespace std::chrono;
    static auto last = steady_clock::now();
    static bool on = true;
    auto now = steady_clock::now();
    if (duration_cast<milliseconds>(now - last).count() >= 500) {
        on = !on;
        last = now;
    }
    return on;
}

BinUIApp::BinUIApp(const std::string& title, int width, int height)
    : width_(width), height_(height), title_(title) {}

BinUIApp::~BinUIApp() {
    if (memDC_ && memOldBmp_)
        SelectObject(static_cast<HDC>(memDC_), static_cast<HGDIOBJ>(memOldBmp_));
    if (memBmp_) DeleteObject(static_cast<HBITMAP>(memBmp_));
    if (memDC_) DeleteDC(static_cast<HDC>(memDC_));
}

void BinUIApp::invalidateRect(int x, int y, int w, int h) {
    if (hwnd_ && w > 0 && h > 0) {
        RECT r = { x, y, x + w, y + h };
        InvalidateRect(reinterpret_cast<HWND>(hwnd_), &r, FALSE);
    }
}

void BinUIApp::invalidateAll() {
    if (hwnd_) InvalidateRect(reinterpret_cast<HWND>(hwnd_), nullptr, FALSE);
}

void BinUIApp::setUpdateFn(UpdateFn fn) { update_ = std::move(fn); }
void BinUIApp::setDrawFn(DrawFn fn)     { draw_ = std::move(fn); }
void BinUIApp::setMoveFn(MoveFn fn)     { move_ = std::move(fn); }
void BinUIApp::setPressFn(PressFn fn)   { press_ = std::move(fn); }
void BinUIApp::setReleaseFn(ReleaseFn fn) { release_ = std::move(fn); }
void BinUIApp::setLeaveFn(LeaveFn fn)   { leave_ = std::move(fn); }
void BinUIApp::setResizeFn(ResizeFn fn) { resize_ = std::move(fn); }
void BinUIApp::setCharFn(CharFn fn)     { char_ = std::move(fn); }
void BinUIApp::setKeyFn(KeyFn fn)       { key_ = std::move(fn); }
void BinUIApp::setWheelFn(WheelFn fn)   { wheel_ = std::move(fn); }
void BinUIApp::setFocusFn(FocusFn fn)   { focus_ = std::move(fn); }

void BinUIApp::requestExit() { running_ = false; }

void* BinUIApp::hwnd() { return hwnd_; }

void BinUIApp::beginWindowDrag() {
    HWND h = reinterpret_cast<HWND>(hwnd_);
    if (!h) return;
    ReleaseCapture();
    // 转发为系统标题栏按下：驱动窗口拖动（最小化/关闭按钮按下不进入拖拽，由 demo 层判断）
    SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void BinUIApp::minimize() {
    HWND h = reinterpret_cast<HWND>(hwnd_);
    if (h) ShowWindow(h, SW_MINIMIZE);
}

void BinUIApp::maximize() {
    HWND h = reinterpret_cast<HWND>(hwnd_);
    if (!h) return;
    ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
}

int BinUIApp::run() {
    // DPI 感知：布局坐标使用物理像素，避免窗口被系统缩放放大超出屏幕
    SetProcessDPIAware();

    const char* CLASS_NAME = "BinUIAppWindow";

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassA(&wc);

    // 无边框窗口：外尺寸 = 布局尺寸。无 WS_THICKFRAME（避免 DWM 边框/焦点框），
    // resize 由 WM_NCLBUTTONDOWN 手动实现；焦点指示用自绘 1px 细框
    DWORD style = WS_POPUP | WS_MAXIMIZEBOX;
    int winW = width_;
    int winH = height_;

    // 在工作区内居中，超出工作区时限制到工作区大小
    RECT work = { 0, 0, 800, 600 };
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    if (winW > work.right - work.left) winW = work.right - work.left;
    if (winH > work.bottom - work.top) winH = work.bottom - work.top;
    int winX = work.left + ((work.right - work.left) - winW) / 2;
    int winY = work.top + ((work.bottom - work.top) - winH) / 2;

    hwnd_ = CreateWindowExA(0, CLASS_NAME, title_.c_str(),
        style, winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) {
        std::fprintf(stderr, "[app] Failed to create window\n");
        return 1;
    }

    SetWindowLongPtrA(reinterpret_cast<HWND>(hwnd_), GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(this));
    // 强制重算非客户区：客户区 = 窗口矩形（去掉 WS_THICKFRAME 边框线）
    SetWindowPos(reinterpret_cast<HWND>(hwnd_), nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(reinterpret_cast<HWND>(hwnd_), SW_SHOW);
    UpdateWindow(reinterpret_cast<HWND>(hwnd_));

    auto lastTime = std::chrono::high_resolution_clock::now();
    running_ = true;
    MSG msg = {};

    while (running_) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running_ = false; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.1f) dt = 1.0f / 60.0f;

        if (update_) update_(dt);
        Sleep(1);
    }

    DestroyWindow(reinterpret_cast<HWND>(hwnd_));
    hwnd_ = nullptr;
    return 0;
}

// 局部重绘：脏矩形先在持久离屏缓冲上重画，再只把脏区 BitBlt 到窗口。
// 缓冲按客户区尺寸创建，尺寸变化时重建并回退为全量重绘。
void BinUIApp::OnDraw(void* hdc, const void* clipRect) {
    HDC screen = static_cast<HDC>(hdc);
    RECT client;
    GetClientRect(reinterpret_cast<HWND>(hwnd_), &client);
    int cw = client.right - client.left;
    int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) return;

    RECT full = client;
    const RECT* clip = static_cast<const RECT*>(clipRect);
    if (!clip || clip->right <= clip->left || clip->bottom <= clip->top ||
        clip->left < 0 || clip->top < 0 || clip->right > cw || clip->bottom > ch)
        clip = &full;

    if (!memDC_ || !memBmp_ || bufW_ != cw || bufH_ != ch) {
        // 缓冲不存在或尺寸不符：重建并全量重绘
        if (memDC_ && memOldBmp_)
            SelectObject(static_cast<HDC>(memDC_), static_cast<HGDIOBJ>(memOldBmp_));
        if (memBmp_) { DeleteObject(static_cast<HBITMAP>(memBmp_)); memBmp_ = nullptr; }
        if (memDC_) { DeleteDC(static_cast<HDC>(memDC_)); memDC_ = nullptr; }
        memOldBmp_ = nullptr;
        HDC mdc = CreateCompatibleDC(screen);
        HBITMAP mb = CreateCompatibleBitmap(screen, cw, ch);
        if (!mdc || !mb) {
            if (mb) DeleteObject(mb);
            if (mdc) DeleteDC(mdc);
            if (draw_) draw_(screen, &full); // 缓冲失败退化为直接绘制
            return;
        }
        memOldBmp_ = SelectObject(mdc, mb);
        memDC_ = mdc;
        memBmp_ = mb;
        bufW_ = cw;
        bufH_ = ch;
        clip = &full;
    }

    HDC mdc = static_cast<HDC>(memDC_);
    int saved = SaveDC(mdc);
    IntersectClipRect(mdc, clip->left, clip->top, clip->right, clip->bottom);
    if (draw_) draw_(mdc, clip);

    // 自绘聚焦细边框：1px 环绕窗口四周（画在缓冲上，与内容一同呈现）
    if (focused_ && focusBorder_) {
        uint32_t c = focusBorderColor_;
        HPEN pen = CreatePen(PS_SOLID, 1,
            RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
        HGDIOBJ oldPen = SelectObject(mdc, pen);
        HGDIOBJ oldBr = SelectObject(mdc, GetStockObject(NULL_BRUSH));
        Rectangle(mdc, 0, 0, cw, ch);
        SelectObject(mdc, oldBr);
        SelectObject(mdc, oldPen);
        DeleteObject(pen);
    }

    RestoreDC(mdc, saved);

    BitBlt(screen, clip->left, clip->top,
           clip->right - clip->left, clip->bottom - clip->top,
           mdc, clip->left, clip->top, SRCCOPY);
}

void BinUIApp::OnMouseMove(float x, float y) {
    if (move_) move_(x, y);
}

void BinUIApp::OnMousePress(float x, float y) {
    if (press_) press_(x, y);
}

void BinUIApp::OnMouseRelease(float x, float y) {
    if (release_) release_(x, y);
}

void BinUIApp::OnMouseLeave() {
    if (leave_) leave_();
}

void BinUIApp::OnResize(int w, int h) {
    width_ = w;
    height_ = h;
    if (resize_) resize_(w, h);
    invalidateAll(); // 客户区变化，全部重绘
}

// UTF-16 代码单元 → 合并代理对 → UTF-8 回调
void BinUIApp::OnChar(wchar_t ch) {
    if (ch >= 0xD800 && ch <= 0xDBFF) { pendingHigh_ = ch; return; } // 高位代理
    unsigned cp;
    if (ch >= 0xDC00 && ch <= 0xDFFF && pendingHigh_) {
        cp = 0x10000 + ((static_cast<unsigned>(pendingHigh_) - 0xD800) << 10) + (ch - 0xDC00);
        pendingHigh_ = 0;
    } else {
        pendingHigh_ = 0;
        cp = static_cast<unsigned>(ch);
    }
    // 控制字符（退格/回车/制表等）由 WM_KEYDOWN 处理，不作为文本插入
    if (cp < 0x20 || cp == 0x7F) return;
    if (char_) char_(CodepointToUtf8(cp));
}

void BinUIApp::OnKey(int vk) {
    if (key_) key_(vk);
}

void BinUIApp::OnMouseWheel(float x, float y, float dy) {
    if (wheel_) wheel_(x, y, dy);
}

void BinUIApp::OnFocusLost() {
    pendingHigh_ = 0;
    if (focus_) focus_(false);
}

void BinUIApp::setFocusBorder(bool show, uint32_t color) {
    focusBorder_ = show;
    focusBorderColor_ = color;
    invalidateAll();
}

// ============================================================
// GDI 绘制后端：把 BinUIRenderer 节点树画到 HDC
// ============================================================
// 视频控制条时间格式（m:ss；与 GPU 后端一致）
static void FormatVideoTime(double sec, char* buf, size_t n) {
    if (sec < 0) sec = 0;
    int s = (int)(sec + 0.5);
    snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

// v13 扩展绘制：把"底色/边框"的条带画到 GDI 画布上。
// GDI 画布没有 alpha 合成，半透明色按"与节点自身底色混合"近似（边框通常压在
// 节点底色上，这样最接近正确结果）；渐变端点一般是不透明的，直接画即可。
static uint32_t BlendOver(uint32_t fg, uint32_t bg) {
    const uint32_t a = (fg >> 24) & 0xFF;
    if (a >= 255) return fg;
    if (a == 0) return bg;
    auto mix = [&](int sh) {
        const uint32_t f = (fg >> sh) & 0xFF, b = (bg >> sh) & 0xFF;
        return ((f * a + b * (255 - a)) / 255) & 0xFF;
    };
    return (0xFFu << 24) | (mix(16) << 16) | (mix(8) << 8) | mix(0);
}

static void FillPaintBands(HDC hdc, const binui::PaintBand* bands, int n, uint32_t over) {
    for (int i = 0; i < n; i++) {
        const binui::PaintBand& b = bands[i];
        if (b.w <= 0.0f || b.h <= 0.0f) continue;
        const uint32_t c = BlendOver(b.color, over);
        if (((c >> 24) & 0xFF) == 0) continue;
        HBRUSH br = CreateSolidBrush(RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
        if (b.round && b.radius > 0.0f) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
            HGDIOBJ oldBr = SelectObject(hdc, br);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            const int rr = (int)std::lround(b.radius * 2.0f);
            RoundRect(hdc, (int)std::lround(b.x), (int)std::lround(b.y),
                      (int)std::lround(b.x + b.w), (int)std::lround(b.y + b.h), rr, rr);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBr);
            DeleteObject(pen);
        } else {
            RECT r = { (int)std::lround(b.x), (int)std::lround(b.y),
                       (int)std::lround(b.x + b.w), (int)std::lround(b.y + b.h) };
            FillRect(hdc, &r, br);
        }
        DeleteObject(br);
    }
}

void drawNodesGDI(void* hdcCtx, const BinUIRenderer& renderer, const void* clipRect) {    HDC hdc = static_cast<HDC>(hdcCtx);
    const auto& nodes = renderer.getNodes();

    // 绘制范围：脏矩形（局部重绘）或窗口客户区（全量）
    RECT clip;
    const RECT* clipp = static_cast<const RECT*>(clipRect);
    if (clipp && clipp->right > clipp->left && clipp->bottom > clipp->top) {
        clip = *clipp;
    } else {
        RECT client;
        GetClientRect(WindowFromDC(hdc), &client);
        clip = client;
        clipp = &clip;
    }
    int w = clip.right - clip.left;
    int h = clip.bottom - clip.top;
    if (w <= 0 || h <= 0) return;

    SetBkMode(hdc, TRANSPARENT);
    SetStretchBltMode(hdc, HALFTONE);

    // id -> index，用于沿祖先链裁剪
    std::unordered_map<uint16_t, size_t> byId;
    byId.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) byId[nodes[i].id] = i;

    // 滚动容器 id -> 滚动偏移
    std::unordered_map<uint16_t, binui::BinUIRenderer::ScrollState> scrolls;
    for (const auto& node : nodes) {
        if (node.flags & BINUI_SCROLLABLE) {
            if (const auto* s = renderer.scrollState(node.id)) scrolls[node.id] = *s;
        }
    }

    // 预计算：父传子的祖先索引 / 隐藏 / 滚动偏移 / 最内层滚动视口（O(n)，避免逐节点查找）
    std::vector<int32_t> parentIdx(nodes.size(), -1);
    std::vector<uint8_t> hidden(nodes.size(), 0);
    std::vector<float> offX(nodes.size(), 0), offY(nodes.size(), 0);
    std::vector<int32_t> vpL(nodes.size(), 0), vpT(nodes.size(), 0),
                         vpR(nodes.size(), 0), vpB(nodes.size(), 0);
    std::vector<uint8_t> hasVp(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); i++) {
        const auto& node = nodes[i];
        if (node.parentId == 0xFFFF) continue;
        auto pit = byId.find(node.parentId);
        if (pit == byId.end()) continue;
        const size_t pi = pit->second;
        parentIdx[i] = static_cast<int32_t>(pi);
        if (hidden[pi] || !(nodes[pi].flags & BINUI_VISIBLE)) { hidden[i] = 1; continue; }
        offX[i] = offX[pi];
        offY[i] = offY[pi];
        const auto& par = nodes[pi];
        if (par.flags & BINUI_SCROLLABLE) {
            auto sit = scrolls.find(par.id);
            if (sit != scrolls.end()) { offX[i] += sit->second.x; offY[i] += sit->second.y; }
            // 最内层滚动容器视口 = 父容器自身
            hasVp[i] = 1;
            vpL[i] = static_cast<int32_t>(par.x);
            vpT[i] = static_cast<int32_t>(par.y);
            vpR[i] = static_cast<int32_t>(par.x + par.width);
            vpB[i] = static_cast<int32_t>(par.y + par.height);
        } else {
            hasVp[i] = hasVp[pi];
            vpL[i] = vpL[pi]; vpT[i] = vpT[pi];
            vpR[i] = vpR[pi]; vpB[i] = vpB[pi];
        }
    }

    // 根节点背景（外层已把 hdc 裁剪到脏矩形，只填充脏区）
    uint32_t pageBg = 0xFF000000u;
    for (const auto& node : nodes) {
        if (node.type == BINUI_ROOT && (node.flags & BINUI_VISIBLE)) {
            pageBg = node.color | 0xFF000000u;
            HBRUSH bg = CreateSolidBrush(RGB(
                (node.color >> 16) & 0xFF,
                (node.color >> 8) & 0xFF,
                node.color & 0xFF));
            FillRect(hdc, &clip, bg);
            DeleteObject(bg);
            break;
        }
    }

    // 绘制顺序：按 z-index 分层（v10），同层流式先于 absolute、DOM 顺序；
    // 由渲染器统一给出，保证与命中测试一致
    const std::vector<uint32_t> order = renderer.drawOrder();
    for (size_t oi = 0; oi < order.size(); oi++) {
        const size_t i = order[oi];
        const auto& node = nodes[i];
        if (!(node.flags & BINUI_VISIBLE) || hidden[i]) continue;
        if (node.type == BINUI_ROOT) continue; // 已绘制

        int x = static_cast<int>(node.x) - static_cast<int>(offX[i]);
        int y = static_cast<int>(node.y) - static_cast<int>(offY[i]);
        int nw = static_cast<int>(node.width);
        int nh = static_cast<int>(node.height);
        // 动画的位移/缩放：以前 GDI 画布只取颜色，hover 放大在 GDI 后端根本不发生，
        // 与 GL/D3D 表现不一致。绕中心缩放 + 平移，算法与两个 GPU 后端一致。
        {
            const float sc = renderer.drawScale(node.id);
            if (sc != 1.0f && sc > 0.0f) {
                const float cx = x + nw / 2.0f, cy = y + nh / 2.0f;
                nw = static_cast<int>(nw * sc);
                nh = static_cast<int>(nh * sc);
                x = static_cast<int>(cx - nw / 2.0f);
                y = static_cast<int>(cy - nh / 2.0f);
            }
            x += static_cast<int>(renderer.drawOffsetX(node.id));
            y += static_cast<int>(renderer.drawOffsetY(node.id));
        }
        // 节点当前生效颜色直接问渲染器：CSS 状态样式（:hover/:active/:focus/:checked）、
        // transition 过渡与 @keyframes 动画都在渲染器里算好（与 GL/D3D11 后端一致）。
        // 以前这里自己按 hover/按下做 tint —— 作者写的颜色被无视，动画也不生效。
        const uint32_t dc = renderer.drawColor(node.id);
        // v13 扩展绘制段：文字色 / 边框 / 渐变 / 字重 / 行高 / 文字装饰
        const BinUIPaintNode* pt = renderer.paintOf(node.id);
        const bool hasBorder = pt && (pt->flags & PAINT_BORDER);
        const bool hasGrad = pt && (pt->flags & PAINT_GRADIENT);
        // 全透明：GDI 没有 alpha 合成，画出来是一块黑，所以整块跳过。
        // 但有文字的节点必须画（<pre>/<div>文字</div> 这种没底色的容器否则整个消失）。
        if (((dc >> 24) & 0xFF) == 0 && !hasBorder && !hasGrad && node.text.empty()) {
            // GDI 没有 alpha 合成：全透明节点画出来是一块黑。图片/视频帧例外（只看自身像素）
            const bool drawsOwnPixels =
                (node.type == BINUI_IMAGE && renderer.getImage(node.imageIndex) != nullptr) ||
                node.type == BINUI_VIDEO;
            if (!drawsOwnPixels) continue;
        }


        // 与脏矩形不相交的节点快速跳过（GDI 裁剪链较贵，仅在相交节点上做）
        if (x + nw <= clip.left || x >= clip.right ||
            y + nh <= clip.top || y >= clip.bottom)
            continue;
        // 滚动容器内：视口外的节点（内容溢出/未滚动到）纯整数裁剪，避免 GDI 链
        if (hasVp[i] && (x + nw <= vpL[i] || x >= vpR[i] ||
                         y + nh <= vpT[i] || y >= vpB[i]))
            continue;
        RECT rc = { x, y, x + nw, y + nh };
        int pad = node.padding;

        // ---- v13 扩展绘制：底色/边框的统一入口 ----
        const float nfx = (float)x, nfy = (float)y, nfw = (float)nw, nfh = (float)nh;
        uint32_t underBg = pageBg;
        {
            auto pit = byId.find(node.parentId);
            if (pit != byId.end()) {
                const uint32_t pbg = renderer.drawColor(nodes[pit->second].id);
                if ((pbg >> 24) & 0xFF) underBg = pbg | 0xFF000000u;
            }
        }
        // v14：禁用/斜体/占位符。GDI 画布没有 alpha 合成，所以"淡"用与底色混合近似。
        const bool disabled = renderer.isDisabled(node.id);
        const bool italic = pt && (pt->textFlags & TEXT_ITALIC) != 0;
        const bool placeholder = pt && (pt->textFlags & TEXT_PLACEHOLDER) != 0;
        auto fadeOver = [&](uint32_t c, int alpha) -> uint32_t {
            return BlendOver((c & 0x00FFFFFFu) | ((uint32_t)alpha << 24), underBg);
        };
        auto fillNodeBg = [&](uint32_t color, float radius) {
            binui::PaintBand bands[binui::kMaxPaintBands];
            const int bn = binui::BuildFillBands(pt, nfx, nfy, nfw, nfh, radius, color, bands);
            FillPaintBands(hdc, bands, bn, underBg);
        };
        auto drawNodeBorder = [&]() {
            if (!hasBorder) return;
            binui::PaintBand bb[binui::kMaxPaintBands];
            const int bn = binui::BuildBorderBands(pt, nfx, nfy, nfw, nfh, (float)node.radius, bb);
            FillPaintBands(hdc, bb, bn, dc | 0xFF000000u);
        };

        // 层级裁剪：先把裁剪区设为自身（滚动后）范围，再逐层与祖先求交；
        // 任一祖先不可见或交集为空则跳过绘制
        int savedDC = SaveDC(hdc);
        bool draw = true;
        RECT nr = rc;
        if (IntersectClipRect(hdc, nr.left, nr.top, nr.right, nr.bottom) == NULLREGION)
            draw = false;

        size_t cur = i;
        while (draw) {
            const auto& anc = nodes[cur];
            if (anc.parentId == 0xFFFF) break;
            auto pit = byId.find(anc.parentId);
            if (pit == byId.end()) break;
            cur = pit->second;
            const auto& par = nodes[cur];
            if (!(par.flags & BINUI_VISIBLE)) { draw = false; break; }
            RECT pr = { (int)par.x, (int)par.y, (int)(par.x + par.width), (int)(par.y + par.height) };
            if (IntersectClipRect(hdc, pr.left, pr.top, pr.right, pr.bottom) == NULLREGION) {
                draw = false;
                break;
            }
        }

        if (draw) {
        switch (node.type) {
            case BINUI_LABEL: {
                // 文字色 / 字重 / 行高 / 下划线（v13 扩展绘制段）
                const bool paintText = pt && (pt->flags & PAINT_TEXT_COLOR);
                uint32_t tcol = paintText ? pt->textColor : node.color;
                if (disabled) tcol = fadeOver(tcol, 140);   // 55% 淡
                const int fw = binui::PaintWeightOf(pt);
                // LABEL 也可以有底色了（作者显式给底色时 node.color 存的是底色）
                if (hasGrad || (paintText && ((node.color >> 24) & 0xFF)))
                    fillNodeBg(dc, (float)node.radius);
                if (node.text.empty()) break;
                const int fs = node.fontSize ? node.fontSize : 14;
                SetTextColor(hdc, RGB((tcol >> 16) & 0xFF, (tcol >> 8) & 0xFF, tcol & 0xFF));
                HFONT font = CreateFontA(
                    -fs, 0, 0, 0, fw >= 600 ? FW_BOLD : FW_NORMAL,
                    italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                HGDIOBJ oldFont = SelectObject(hdc, font);
                RECT tr = { x + pad, y, x + nw - pad, y + nh };
                std::wstring wtext = LfToCrlf(Utf8ToWide(node.text));
                const UINT halign = (node.flags & BINUI_TEXT_CENTER) ? DT_CENTER
                                  : (node.flags & BINUI_TEXT_RIGHT) ? DT_RIGHT : DT_LEFT;
                // 含 '\n'（<br>）：GDI 的 DT_VCENTER 只能配 SINGLELINE，多行改为顶部对齐逐行；
                // 单行但比内容盒宽时同样走 WORDBREAK —— GPU 后端的 drawTextWrapped 一直会换行，
                // GDI 以前硬套 SINGLELINE，长段落会被直接裁掉（两端表现不一致）。
                if (node.text.find('\n') != std::string::npos) {
                    DrawTextW(hdc, wtext.c_str(), -1, &tr, halign | DT_WORDBREAK);
                } else {
                    RECT mr = { 0, 0, 0, 10 };
                    DrawTextW(hdc, wtext.c_str(), -1, &mr, DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
                    // 只有"明显放不下"（超宽 20% 以上）才换行：GPU 后端画单行文本从不换行，
                    // 阈值取松一点可以兼顾"长段落别被裁掉"和"窄盒子里的短标签别乱折行"
                    if (mr.right > (nw - 2 * pad) * 1.2f)
                        DrawTextW(hdc, wtext.c_str(), -1, &tr, halign | DT_WORDBREAK);
                    else
                        DrawTextW(hdc, wtext.c_str(), -1, &tr, halign | DT_VCENTER | DT_SINGLELINE);
                }

                // 文字装饰线（下划线/删除线/上划线）：测量文字块位置后补画细条
                if (pt && (pt->flags & (PAINT_UNDERLINE | PAINT_STRIKE | PAINT_OVERLINE))) {
                    TEXTMETRICW tm = {};
                    GetTextMetricsW(hdc, &tm);
                    const float lh = (float)tm.tmHeight;
                    auto measure = [&](const std::wstring& s) -> float {
                        RECT mr = { 0, 0, 0, 10 };
                        DrawTextW(hdc, s.c_str(), -1, &mr, DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
                        return (float)mr.right;
                    };
                    binui::PaintBand rules[3];
                    if (node.text.find('\n') == std::string::npos) {
                        const float tw = measure(wtext);
                        float tx = (float)(x + pad);
                        if (halign == DT_CENTER) tx += ((float)(nw - 2 * pad) - tw) * 0.5f;
                        else if (halign == DT_RIGHT) tx += (float)(nw - 2 * pad) - tw;
                        const float top = (float)y + ((float)nh - lh) * 0.5f;
                        const int rn = binui::BuildTextRuleBands(pt, tx, top, tw, (float)fs, rules);
                        FillPaintBands(hdc, rules, rn, dc | 0xFF000000u);
                    } else {
                        // 多行：逐行（按 '\n' 切分）画，行距按 line-height（未声明用字体行高）
                        const float step = binui::PaintLineHeight(pt, (float)fs);
                        float ly = (float)y + pad;
                        size_t p0 = 0;
                        while (p0 <= node.text.size()) {
                            const size_t nl = node.text.find('\n', p0);
                            const std::string seg = node.text.substr(
                                p0, (nl == std::string::npos) ? std::string::npos : nl - p0);
                            const std::wstring ws = Utf8ToWide(seg);
                            const float tw = measure(ws);
                            const int rn = binui::BuildTextRuleBands(pt, (float)(x + pad), ly, tw, (float)fs, rules);
                            FillPaintBands(hdc, rules, rn, dc | 0xFF000000u);
                            if (nl == std::string::npos) break;
                            p0 = nl + 1;
                            ly += std::max(step, lh);
                        }
                    }
                }
                SelectObject(hdc, oldFont);
                DeleteObject(font);
                break;
            }
            case BINUI_BUTTON: {
                fillNodeBg(disabled ? fadeOver(dc, 140) : dc, (float)node.radius);
                if (!node.text.empty()) {
                    uint32_t tcol = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                          ? pt->textColor : 0xFFFFFFFFu;
                    if (disabled) tcol = fadeOver(tcol, 140);
                    SetTextColor(hdc, RGB((tcol >> 16) & 0xFF, (tcol >> 8) & 0xFF, tcol & 0xFF));
                    HFONT font = CreateFontA(
                        -(node.fontSize ? node.fontSize : 14), 0, 0, 0, FW_BOLD,
                        italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                    HGDIOBJ oldFont = SelectObject(hdc, font);
                    RECT tr = { x + pad, y, x + nw - pad, y + nh };
                    // 文本是 UTF-8：必须转宽字符用 DrawTextW。早先用 DrawTextA，
                    // 中文会被按系统代码页解释成乱码（叠加成一团）。
                    std::wstring wtext = LfToCrlf(Utf8ToWide(node.text));
                    DrawTextW(hdc, wtext.c_str(), -1, &tr,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                }
                break;
            }
            case BINUI_INPUT: {
                // 输入框背景（style 里的 background / 渐变）
                fillNodeBg(dc, (float)node.radius);

                HPEN underlinePen = CreatePen(PS_SOLID, 2, RGB(0xBB, 0x86, 0xFC));
                HGDIOBJ oldPen = SelectObject(hdc, underlinePen);
                MoveToEx(hdc, x, y + nh - 2, nullptr);
                LineTo(hdc, x + nw, y + nh - 2);
                SelectObject(hdc, oldPen);
                DeleteObject(underlinePen);

                bool editing = renderer.isFocused(node.id);
                std::string text = node.text;
                size_t caret = 0, selStart = 0;
                if (editing) {
                    text = renderer.edit().value;
                    caret = renderer.edit().caret;
                    selStart = renderer.edit().selStart;
                }

                if (!text.empty()) {
                    HFONT font = CreateFontA(
                        -(node.fontSize ? node.fontSize : 14), 0, 0, 0, FW_NORMAL,
                        italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                    HGDIOBJ oldFont = SelectObject(hdc, font);
                    RECT tr = { x + pad + 4, y, x + nw - pad - 4, y + nh };

                    std::wstring wtext = Utf8ToWide(text);
                    // 占位符画 45% 淡、禁用再压到 55%（GDI 无 alpha，按与底色混合近似）
                    uint32_t inCol = 0xFFBB86FCu;
                    if (placeholder && !editing) inCol = fadeOver(inCol, 140);
                    if (disabled) inCol = fadeOver(inCol, 140);
                    SetTextColor(hdc, RGB((inCol >> 16) & 0xFF, (inCol >> 8) & 0xFF, inCol & 0xFF));
                    DrawTextW(hdc, wtext.c_str(), -1, &tr,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    if (editing) {
                        // 选区：测量起止宽度，XOR 反色高亮
                        size_t a = std::min(caret, selStart);
                        size_t b = std::max(caret, selStart);
                        if (a != b) {
                            RECT ra = { tr.left, 0, tr.right, 10 };
                            RECT rb = { tr.left, 0, tr.right, 10 };
                            std::wstring wa = Utf8ToWide(text.substr(0, a));
                            std::wstring wb = Utf8ToWide(text.substr(0, b));
                            DrawTextW(hdc, wa.c_str(), -1, &ra,
                                DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
                            DrawTextW(hdc, wb.c_str(), -1, &rb,
                                DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
                            XorFillRect(hdc, ra.right, tr.top + 2, rb.right, tr.bottom - 2);
                        }

                        // 闪烁光标：测量光标前文本宽度，XOR 竖线
                        if (caretBlinkOn()) {
                            RECT cr = { tr.left, 0, tr.right, 10 };
                            std::wstring wc = Utf8ToWide(text.substr(0, caret));
                            DrawTextW(hdc, wc.c_str(), -1, &cr,
                                DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
                            XorFillRect(hdc, cr.right, tr.top + 2, cr.right + 2, tr.bottom - 2);
                        }
                    }

                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                }
                break;
            }
            case BINUI_LIST: {
                fillNodeBg(disabled ? fadeOver(dc, 140) : dc, (float)node.radius);

                if (!node.text.empty()) {
                    uint32_t tcol = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                              ? pt->textColor : 0xFFFFFFFFu;
                    if (disabled) tcol = fadeOver(tcol, 140);
                    SetTextColor(hdc, RGB((tcol >> 16) & 0xFF, (tcol >> 8) & 0xFF, tcol & 0xFF));
                    const int fw = binui::PaintWeightOf(pt);
                    HFONT font = CreateFontA(-(node.fontSize ? node.fontSize : 14), 0, 0, 0,
                        fw >= 600 ? FW_BOLD : FW_NORMAL,
                        italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                    HGDIOBJ oldFont = SelectObject(hdc, font);
                    RECT tr = { x + pad + 12, y + pad + 8, x + nw - pad - 12, y + nh - pad - 8 };
                    const UINT halign = (node.flags & BINUI_TEXT_CENTER) ? DT_CENTER
                                      : (node.flags & BINUI_TEXT_RIGHT) ? DT_RIGHT : DT_LEFT;
                    std::wstring wtext = LfToCrlf(Utf8ToWide(node.text));
                    DrawTextW(hdc, wtext.c_str(), -1, &tr,
                        halign | DT_WORDBREAK);   // '\n'（来自 <br>）同样断行
                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                }
                break;
            }
            case BINUI_IMAGE: {
                // 有真实图片资源则绘制图片，否则回退到 FAB 圆形
                const ImageResource* img = renderer.getImage(node.imageIndex);
                if (img && !img->data.empty()) {
                    Gdiplus::Bitmap* bmp = DecodeImageBitmap(img->data.data(), img->data.size());
                    if (bmp) {
                        Gdiplus::Graphics g(hdc);
                        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                        g.DrawImage(bmp, (Gdiplus::REAL)x, (Gdiplus::REAL)y,
                                    (Gdiplus::REAL)nw, (Gdiplus::REAL)nh);
                        delete bmp;
                        break;
                    }
                }

                fillNodeBg(dc, (float)node.radius);

                if (!node.text.empty()) {
                    SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
                    HFONT font = CreateFontA(24, 0, 0, 0, FW_LIGHT,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                    HGDIOBJ oldFont = SelectObject(hdc, font);
                    std::wstring wtext = LfToCrlf(Utf8ToWide(node.text));
                    DrawTextW(hdc, wtext.c_str(), -1, &rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                }
                break;
            }
            case BINUI_VIDEO: {
                // 底色（无解码帧时的海报底）
                fillNodeBg(dc, (float)node.radius);

                // 解码帧（rawPixels RGBA）：GDI 需转成 BGRA 再包 Bitmap，contain-fit 绘制
                const ImageResource* img = renderer.getImage(node.imageIndex);
                if (img && img->rawPixels && !img->data.empty() &&
                    img->width > 0 && img->height > 0) {
                    size_t px = (size_t)img->width * img->height;
                    std::vector<uint8_t> bgra(px * 4);
                    for (size_t p = 0; p < px; p++) {
                        bgra[p*4+0] = img->data[p*4+2];
                        bgra[p*4+1] = img->data[p*4+1];
                        bgra[p*4+2] = img->data[p*4+0];
                        bgra[p*4+3] = img->data[p*4+3];
                    }
                    Gdiplus::Bitmap bmp(img->width, img->height, img->width * 4,
                                        PixelFormat32bppRGB, bgra.data());
                    float ar = (float)img->width / img->height;
                    float boxAr = (float)nw / nh;
                    float fw, fh;
                    if (ar > boxAr) { fw = (float)nw; fh = fw / ar; }
                    else            { fh = (float)nh; fw = fh * ar; }
                    Gdiplus::Graphics g(hdc);
                    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    g.DrawImage(&bmp,
                                (Gdiplus::REAL)(x + (nw - fw) * 0.5f),
                                (Gdiplus::REAL)(y + (nh - fh) * 0.5f),
                                (Gdiplus::REAL)fw, (Gdiplus::REAL)fh);
                }

                // 控制条（GDI 简化版：实色，无透明混合）
                if (renderer.videoControlsVisible(node.id)) {
                    BinUIRenderer::VideoBarGeo geo;
                    if (BinUIRenderer::VideoBarLayout(node, geo)) {
                        const auto* v = renderer.findVideo(node.id);
                        if (v) {
                            HBRUSH bar = CreateSolidBrush(RGB(0x1B, 0x1B, 0x1B));
                            RECT br = { x, y + nh - (int)BinUIRenderer::kVideoBarH,
                                        x + nw, y + nh };
                            FillRect(hdc, &br, bar);
                            DeleteObject(bar);

                            double dur = v->duration;
                            float ratio = (dur > 0)
                                ? (float)std::min(std::max(v->position / dur, 0.0), 1.0) : 0.0f;
                            HBRUSH trk = CreateSolidBrush(RGB(0x66, 0x66, 0x66));
                            HBRUSH sol = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
                            // 轨道坐标换算：geo 相对节点原点 → 屏幕坐标
                            int barTop = y + nh - (int)BinUIRenderer::kVideoBarH;
                            RECT trkR;
                            trkR.left = x + (int)geo.trX;              trkR.top = barTop + (int)geo.trY;
                            trkR.right = x + (int)(geo.trX + geo.trW); trkR.bottom = barTop + (int)(geo.trY + geo.trH);
                            FillRect(hdc, &trkR, trk);
                            if (ratio > 0) {
                                trkR.right = x + (int)(geo.trX + geo.trW * ratio);
                                FillRect(hdc, &trkR, sol);
                            }
                            // 滑块
                            float tcx = x + geo.trX + geo.trW * ratio;
                            float tcy = barTop + geo.trY + geo.trH * 0.5f;
                            HGDIOBJ oldBr = SelectObject(hdc, sol);
                            HGDIOBJ oldPn = SelectObject(hdc, GetStockObject(NULL_PEN));
                            Ellipse(hdc, (int)(tcx - geo.thumbR), (int)(tcy - geo.thumbR),
                                    (int)(tcx + geo.thumbR), (int)(tcy + geo.thumbR));
                            SelectObject(hdc, GetStockObject(BLACK_BRUSH));
                            DeleteObject(sol); SelectObject(hdc, oldBr); SelectObject(hdc, oldPn);

                            // 图标 + 时间
                            float bx = x + geo.btnX + geo.btnS * 0.5f;
                            float by = barTop + (int)(geo.btnY - (barTop)) + geo.btnS * 0.5f;
                            HBRUSH wbr = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
                            oldBr = SelectObject(hdc, wbr);
                            if (v->playing) {
                                RECT i1 = { (int)bx - 5, (int)by - 7, (int)bx - 2, (int)by + 7 };
                                RECT i2 = { (int)bx + 2, (int)by - 7, (int)bx + 5, (int)by + 7 };
                                FillRect(hdc, &i1, wbr); FillRect(hdc, &i2, wbr);
                            } else {
                                POINT tri[3] = { { (int)bx - 5, (int)by - 7 },
                                                 { (int)bx - 5, (int)by + 7 },
                                                 { (int)bx + 7, (int)by } };
                                Polygon(hdc, tri, 3);
                            }
                            SelectObject(hdc, oldBr);
                            DeleteObject(wbr);

                            char cur[16] = "0:00", tot[16] = "0:00";
                            FormatVideoTime(v->position, cur, sizeof(cur));
                            if (dur > 0) FormatVideoTime(dur, tot, sizeof(tot));
                            char label[40];
                            snprintf(label, sizeof(label), "%s / %s", cur, tot);
                            SetTextColor(hdc, RGB(0xE6, 0xE6, 0xE6));
                            SetBkMode(hdc, TRANSPARENT);
                            HFONT font = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                            HGDIOBJ oldFont = SelectObject(hdc, font);
                            RECT timeR = { x + nw - 76, barTop, x + nw - 8, barTop + (int)BinUIRenderer::kVideoBarH };
                            DrawTextA(hdc, label, -1, &timeR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                            SelectObject(hdc, oldFont);
                            DeleteObject(font);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }

        // 边框（v13）：画在节点内侧，四边可不同宽，压在底色之上
        drawNodeBorder();

        // 键盘焦点高亮边框（内缩绘制，避免超出裁剪区）。
        // 圆角要跟着节点走：以前写死 10，圆角 8 的按钮上会凸出一个半径不同的环。
        if (draw && renderer.isKeyboardFocused(node.id)) {
            const int fr = node.radius > 2 ? (int)node.radius - 1 : 0;
            const int th = nh >= 24 ? 2 : 1;
            HPEN fp = CreatePen(PS_SOLID, th, RGB(0xFF, 0xFF, 0xFF));
            HGDIOBJ oldFp = SelectObject(hdc, fp);
            HGDIOBJ oldFb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, x + 1, y + 1, x + nw - 1, y + nh - 1, fr * 2, fr * 2);
            SelectObject(hdc, oldFb);
            SelectObject(hdc, oldFp);
            DeleteObject(fp);
        }
        }

        RestoreDC(hdc, savedDC);
    }

    // 滚动条（轨道 + 滑块）；overflow:hidden 的只裁剪容器不出滚动条
    for (const auto& node : nodes) {
        if (!renderer.isUserScrollable(node.id) || !(node.flags & BINUI_VISIBLE)) continue;
        float ch = renderer.contentHeight(node.id);
        auto sit = scrolls.find(node.id);
        float sy = sit != scrolls.end() ? sit->second.y : 0;
        int x = (int)node.x, y = (int)node.y, nw = (int)node.width, nh = (int)node.height;
        if (ch <= nh + 0.5f) continue; // 内容未溢出不显示

        int barW = 8;
        HBRUSH track = CreateSolidBrush(RGB(0x2E, 0x2E, 0x2E));
        RECT tr = { x + nw - barW, y, x + nw, y + nh };
        FillRect(hdc, &tr, track);
        DeleteObject(track);

        // 滑块：高度 = 视口比例，位置 = 滚动比例
        float thumbH = std::max(24.0f, (float)nh * nh / ch);
        float maxScroll = ch - nh;
        float thumbY = maxScroll > 0 ? sy / maxScroll * (nh - thumbH) : 0;
        HBRUSH thumb = CreateSolidBrush(RGB(0x5A, 0x5A, 0x5A));
        RECT ur = { x + nw - barW + 2, y + (int)thumbY, x + nw - 2, y + (int)(thumbY + thumbH) };
        FillRect(hdc, &ur, thumb);
        DeleteObject(thumb);
    }
}

// 点击定位文本框光标：在聚焦输入框内按点击 x 坐标放置光标（渲染器无字体测量能力，由后端完成）
void placeCaretAtClick(binui::BinUIRenderer& renderer, float cx, float cy) {
    if (!renderer.isEditing()) return;
    const auto& edit = renderer.edit();

    const auto& nodes = renderer.getNodes();
    const binui::BinUIRenderer::NodeInfo* node = nullptr;
    for (const auto& n : nodes) {
        if (n.id == edit.nodeId) { node = &n; break; }
    }
    if (!node || node->type != BINUI_INPUT) return;
    if (cy < node->y || cy > node->y + node->height) return; // 只响应框内点击

    HDC hdc = GetDC(nullptr); // 测量用屏幕 DC（与绘制同 DPI、同字体）
    HFONT font = CreateFontA(
        node->fontSize ? node->fontSize : 14, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
    HGDIOBJ oldFont = SelectObject(hdc, font);

    std::wstring wtext = Utf8ToWide(edit.value);
    int textX = static_cast<int>(node->x) + node->padding + 4;
    int target = static_cast<int>(cx);
    size_t idx = wtext.size();
    RECT tr = { textX, 0, 0, 10 };
    for (size_t i = 0; i < wtext.size(); i++) {
        RECT pr = tr;
        DrawTextW(hdc, wtext.c_str(), static_cast<int>(i + 1), &pr,
            DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
        if (pr.right >= target) { idx = i; break; }
        tr = pr;
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, hdc);

    renderer.setCaretByUtf16Index(idx);
}

bool isTitlebarDrag(const BinUIRenderer& renderer, float x, float y) {
    const auto& nodes = renderer.getNodes();
    // 命中交互节点（按钮/输入等）→ 响应点击而非拖拽
    int hit = renderer.hitTest(x, y);
    if (hit >= 0) return false;
    // hitTest 只命中交互节点；非交互的标题栏背景/文本用矩形检测：
    // 点落在任一可见 TITLEBAR 节点矩形内即可拖动
    for (const auto& n : nodes)
        if ((n.flags & BINUI_TITLEBAR) && (n.flags & BINUI_VISIBLE) &&
            x >= n.x && x <= n.x + n.width && y >= n.y && y <= n.y + n.height)
            return true;
    return false;
}
