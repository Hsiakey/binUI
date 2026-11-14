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
#include "platform/binui_win32_foreground.h"

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
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
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
    BinUIApp* app = reinterpret_cast<BinUIApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);

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
                        return DefWindowProcW(hwnd, msg, wParam, lParam);
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    StartResize(hwnd, static_cast<int>(wParam), wr);
                    return 0;
                }
                default:
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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

    // Unicode 窗口类：ANSI 窗口在 CP936 下会把中文拆成两个 WM_CHAR（DBCS 字节），
    // 输入框里就成乱码（中 -> "ÖÐ"）；IME 输入同理
    const wchar_t* CLASS_NAME = L"BinUIAppWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

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

    wchar_t wtitle[512] = {};
    if (!title_.empty())
        MultiByteToWideChar(CP_UTF8, 0, title_.c_str(), -1, wtitle,
                            static_cast<int>(sizeof(wtitle) / sizeof(wtitle[0])));
    hwnd_ = CreateWindowExW(0, CLASS_NAME, wtitle,
        style, winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) {
        std::fprintf(stderr, "[app] Failed to create window\n");
        return 1;
    }

    SetWindowLongPtrW(reinterpret_cast<HWND>(hwnd_), GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(this));
    // 强制重算非客户区：客户区 = 窗口矩形（去掉 WS_THICKFRAME 边框线）
    SetWindowPos(reinterpret_cast<HWND>(hwnd_), nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(reinterpret_cast<HWND>(hwnd_), SW_SHOW);
    // SW_SHOW 不一定能抢到前台（前台锁），从控制台启动时窗口会被 cmd 压住
    BinUIForceForeground(reinterpret_cast<HWND>(hwnd_));
    UpdateWindow(reinterpret_cast<HWND>(hwnd_));

    auto lastTime = std::chrono::high_resolution_clock::now();
    running_ = true;
    MSG msg = {};

    while (running_) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running_ = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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
        // 完全透明 = 这条什么都不画。以前会拿 BlendOver 返回的"底色"去铺一块不透明矩形，
        // 于是透明节点在 GDI 上等于"把父底色重涂一遍"—— 压在它下面的兄弟/阴影会被擦掉
        // （v23 的透明元素 + box-shadow 用例一眼就看出来了：阴影被自身盖成一片白）。
        if (((b.color >> 24) & 0xFF) == 0) continue;
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

// ============================================================
// v23 阴影的 GDI 实现：烘成预乘 alpha 位图，再 AlphaBlend 贴上去
// ------------------------------------------------------------
// 为什么不沿用底色那套"按底色预混成不透明色、逐环填矩形"：
//   · **多条阴影会互相擦掉**。后一条阴影最外圈几乎全透明，却是不透明地盖在前一条
//     的渐变上 —— 实测 229 级被擦成 253，卡片边缘浮出一圈亮线；
//   · **太慢**。一帧里叠几十次全尺寸 RoundRect：300×200 的卡片 blur 32 要 20 多次
//     十几万像素的填充，GDI 画布直接顶不住。
// 烘成位图之后：每个阴影一次 AlphaBlend（真逐像素 alpha，底下是什么内容都对）、
// 位图只跟"盒尺寸 + 圆角 + 阴影参数 + 颜色"有关（与位置、不透明度无关），
// 所以同款卡片共用一张，翻页/滚动时不重建。
//
// 画位图的技巧：GDI 往 32 位 DIB 里填色**不写 alpha 字节**，所以不能直接把"alpha"
// 存进去。这里把每环的**覆盖率**编码成灰度（GDI 只写 RGB，R 通道就是覆盖率），
// 全部环填完之后再一趟 CPU 扫描，把它换成预乘 alpha 的像素。
// ============================================================
struct ShadowBitmap {
    HDC     dc = nullptr;      // 选中了 dib 的内存 DC
    HBITMAP dib = nullptr;
    void*   bits = nullptr;
    int     w = 0, h = 0;
    int     offX = 0, offY = 0;  // 位图左上角相对节点盒左上角的像素偏移
    uint64_t lastUse = 0;
    // 缓存键（节点盒 + 圆角 + 阴影参数 + 颜色）
    int      boxW = 0, boxH = 0, radius = 0;
    float    ox = 0, oy = 0, blur = 0, spread = 0;
    uint32_t color = 0;
};

// 缓存：正常页面就是几款卡片，24 条足够；单条位图按 16 MB 预算封顶（超大阴影降级为空）。
static std::vector<ShadowBitmap> g_shadowCache;
static uint64_t g_shadowUse = 0;

static void ShadowBitmapFree(ShadowBitmap& e) {
    if (e.dc) DeleteDC(e.dc);
    if (e.dib) DeleteObject(e.dib);
    e = ShadowBitmap();
}

// 返回该阴影的预乘位图（缓存命中直接复用）；offX/offY 写回相对节点盒左上角的偏移。
// 返回 nullptr = 这个尺寸画不了（太大 / 分配失败），调用方跳过这条阴影。
static ShadowBitmap* ShadowBitmapGet(HDC ref, int boxW, int boxH, int radius,
                                     const binui::BinUIShadow& s, int& offX, int& offY) {
    if (boxW <= 0 || boxH <= 0) return nullptr;
    ++g_shadowUse;
    for (ShadowBitmap& e : g_shadowCache) {
        if (e.boxW == boxW && e.boxH == boxH && e.radius == radius && e.ox == s.offsetX &&
            e.oy == s.offsetY && e.blur == s.blur && e.spread == s.spread && e.color == s.color) {
            e.lastUse = g_shadowUse;
            offX = e.offX;
            offY = e.offY;
            return &e;
        }
    }

    // 位图范围 = 最外圈环的外接矩形
    const float sigma = s.blur > 0.0f ? s.blur * 0.5f : 0.0f;
    const float ext = sigma > 0.4f ? sigma * 3.0f : 1.0f;
    const float L = -s.spread + s.offsetX - ext;
    const float T = -s.spread + s.offsetY - ext;
    const float R = (float)boxW + s.spread + s.offsetX + ext;
    const float B = (float)boxH + s.spread + s.offsetY + ext;
    const int il = (int)std::floor(L), it = (int)std::floor(T);
    const int iw = (int)std::ceil(R) - il, ih = (int)std::ceil(B) - it;
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) return nullptr;

    binui::PaintShadowRing rings[binui::kMaxShadowRings];
    const int rn = binui::BuildShadowRings(s, 0.0f, 0.0f, (float)boxW, (float)boxH,
                                           (float)radius, 1.0f, rings);
    if (rn <= 0) return nullptr;

    // 建 DIB（top-down 32bpp）
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = iw;
    bmi.bmiHeader.biHeight = -ih;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return nullptr; }
    HDC dc = CreateCompatibleDC(ref);
    if (!dc) { DeleteObject(dib); return nullptr; }
    SelectObject(dc, dib);
    // 底：全黑（= 覆盖率 0）
    RECT all = { 0, 0, iw, ih };
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &all, black);
    DeleteObject(black);
    // 逐环填"覆盖率灰度"：内环后画、整块盖住外环 ⇒ 每个像素留下包住它的最内环。
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int k = 0; k < rn; k++) {
        const binui::PaintShadowRing& rr = rings[k];
        const int g = (int)(rr.cover * 255.0f + 0.5f);
        if (g <= 0) continue;
        HBRUSH br = CreateSolidBrush(RGB(g, g, g));
        HGDIOBJ ob = SelectObject(dc, br);
        const int x0 = (int)std::lround(rr.x) - il;
        const int y0 = (int)std::lround(rr.y) - it;
        const int x1 = (int)std::lround(rr.x + rr.w) - il;
        const int y1 = (int)std::lround(rr.y + rr.h) - it;
        if (x1 > x0 && y1 > y0) {
            if (rr.radius > 0.5f) {
                // GDI 的 RoundRect 把填充范围**右/下各收 1px**（实测 [x0,x1-1)×[y0,y1-1)，
                // 上/左却是含的），不补回来的话每一环的右边和下边都短一行，环与环之间
                // 会漏出一条没画的缝（四端叠出来是一圈 1px 的亮线）。
                const int e2 = (int)std::lround(rr.radius * 2.0f);
                RoundRect(dc, x0, y0, x1 + 1, y1 + 1, e2, e2);
            } else {
                RECT r = { x0, y0, x1, y1 };
                FillRect(dc, &r, br);
            }
        }
        SelectObject(dc, ob);
        DeleteObject(br);
    }
    SelectObject(dc, oldPen);

    // 覆盖率 → 预乘 alpha 像素（AlphaBlend 要的就是预乘）
    {
        const uint32_t rgb = s.color & 0x00FFFFFFu;
        const uint32_t sr = (rgb >> 16) & 0xFFu, sg = (rgb >> 8) & 0xFFu, sb = rgb & 0xFFu;
        const uint32_t baseA = (s.color >> 24) & 0xFFu;
        uint8_t* px = static_cast<uint8_t*>(bits);
        const size_t n = (size_t)iw * (size_t)ih;
        for (size_t p = 0; p < n; p++, px += 4) {
            const uint32_t g = px[0];                       // R 通道 = 覆盖率
            if (!g) { px[3] = 0; continue; }
            const uint32_t a = (g * baseA + 127u) / 255u;   // 覆盖率 × 阴影自身 alpha
            px[0] = (uint8_t)((sb * a + 127u) / 255u);
            px[1] = (uint8_t)((sg * a + 127u) / 255u);
            px[2] = (uint8_t)((sr * a + 127u) / 255u);
            px[3] = (uint8_t)a;
        }
    }

    // 入缓存（先按"条数 + 字节数"淘汰最久没用的）。dib 一直选在 dc 里 ——
    // AlphaBlend 的源必须就是这块位图；删除时先 DeleteDC 再 DeleteObject。
    const size_t bytes = (size_t)iw * (size_t)ih * 4;
    size_t total = 0;
    for (const ShadowBitmap& e : g_shadowCache) total += (size_t)e.w * (size_t)e.h * 4;
    while (!g_shadowCache.empty() &&
           (g_shadowCache.size() >= 24 || total + bytes > (16u << 20))) {
        size_t oldest = 0;
        for (size_t k = 1; k < g_shadowCache.size(); k++)
            if (g_shadowCache[k].lastUse < g_shadowCache[oldest].lastUse) oldest = k;
        total -= (size_t)g_shadowCache[oldest].w * g_shadowCache[oldest].h * 4;
        ShadowBitmapFree(g_shadowCache[oldest]);
        g_shadowCache.erase(g_shadowCache.begin() + (long)oldest);
    }
    offX = il;
    offY = it;
    if (total + bytes > (16u << 20)) {          // 单条就超预算：不缓存，用一次就丢
        static ShadowBitmap scratch;
        ShadowBitmapFree(scratch);
        scratch.dc = dc; scratch.dib = dib; scratch.bits = bits;
        scratch.w = iw; scratch.h = ih;
        scratch.offX = il; scratch.offY = it;
        return &scratch;
    }

    ShadowBitmap e;
    e.dc = dc;
    e.dib = dib;
    e.bits = bits;
    e.w = iw; e.h = ih;
    e.offX = il; e.offY = it;
    e.lastUse = g_shadowUse;
    e.boxW = boxW; e.boxH = boxH; e.radius = radius;
    e.ox = s.offsetX; e.oy = s.offsetY; e.blur = s.blur; e.spread = s.spread; e.color = s.color;
    g_shadowCache.push_back(e);
    return &g_shadowCache.back();
}

void drawNodesGDI(void* hdcCtx, const BinUIRenderer& renderer, const void* clipRect) {    HDC hdc = static_cast<HDC>(hdcCtx);    const auto& nodes = renderer.getNodes();

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
    // 真要出竖直滚动条的容器 → 留 gutter（右侧扣掉滚动条宽度），
    // 否则滚动条直接压住最右一列内容（docs/issues/05）。
    std::unordered_map<uint16_t, int32_t> gutterR, gutterB;
    const int32_t sbW = static_cast<int32_t>(renderer.scrollbarStyle().width);
    for (const auto& node : nodes) {
        if (node.flags & BINUI_SCROLLABLE) {
            if (const auto* s = renderer.scrollState(node.id)) scrolls[node.id] = *s;
            if (renderer.contentHeight(node.id) > node.height + 0.5f) gutterR[node.id] = sbW;
            if (renderer.contentWidth(node.id) > node.width + 0.5f) gutterB[node.id] = sbW;
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
            auto gitR = gutterR.find(par.id);
            if (gitR != gutterR.end()) vpR[i] -= gitR->second;
            auto gitB = gutterB.find(par.id);
            if (gitB != gutterB.end()) vpB[i] -= gitB->second;
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
    // 裁剪链缓存：每个节点复用，避免逐节点分配
    std::vector<uint16_t> clipAncs;
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
        const float nodeScale = renderer.drawScale(node.id);
        {
            if (nodeScale != 1.0f && nodeScale > 0.0f) {
                const float cx = x + nw / 2.0f, cy = y + nh / 2.0f;
                nw = static_cast<int>(nw * nodeScale);
                nh = static_cast<int>(nh * nodeScale);
                x = static_cast<int>(cx - nw / 2.0f);
                y = static_cast<int>(cy - nh / 2.0f);
            }
            x += static_cast<int>(renderer.drawOffsetX(node.id));
            y += static_cast<int>(renderer.drawOffsetY(node.id));
        }
        // 节点当前生效颜色直接问渲染器：CSS 状态样式（:hover/:active/:focus/:checked）、
        // transition 过渡与 @keyframes 动画都在渲染器里算好（与 GL/D3D11 后端一致）。
        // 以前这里自己按 hover/按下做 tint —— 作者写的颜色被无视，动画也不生效。
        // 0.16.8：静态 opacity 已经不再折进颜色（那样子节点完全不受影响），
        // 改成运行时按"自己 × 祖先"折进 alpha —— GDI 没有 alpha 合成，下面 FillPaintBands
        // 会拿它去和底色 BlendOver，效果与 GL/VK/D3D11 一致。
        const float nodeOp = renderer.drawOpacity(node.id);
        uint32_t dc = renderer.drawColor(node.id);
        if (nodeOp < 1.0f) {
            const uint32_t oa = (uint32_t)(((dc >> 24) & 0xFFu) * nodeOp + 0.5f);
            dc = (oa << 24) | (dc & 0x00FFFFFFu);
        }
        // v13 扩展绘制段：文字色 / 边框 / 渐变 / 字重 / 行高 / 文字装饰
        const BinUIPaintNode* pt = renderer.paintOf(node.id);
        const bool hasBorder = pt && (pt->flags & PAINT_BORDER);
        const bool hasGrad = pt && (pt->flags & PAINT_GRADIENT);
        // v23 阴影段（box-shadow）：阴影露在盒子外面，剔除与裁剪都要按外扩算
        int shN = 0;
        const BinUIShadow* shs = renderer.shadowsOf(node.id, shN);
        float shPadX = 0.0f, shPadY = 0.0f;
        for (int k = 0; k < shN; k++) {
            const float sig = shs[k].blur > 0.0f ? shs[k].blur * 0.5f : 0.0f;
            const float pad = (std::fabs(shs[k].spread) + sig * 3.0f + 1.0f) * nodeScale;
            shPadX = std::max(shPadX, pad + std::fabs(shs[k].offsetX) * nodeScale);
            shPadY = std::max(shPadY, pad + std::fabs(shs[k].offsetY) * nodeScale);
        }
        const int shPadXi = (int)std::ceil(shPadX), shPadYi = (int)std::ceil(shPadY);
        // 全透明：GDI 没有 alpha 合成，画出来是一块黑，所以整块跳过。
        // 但有文字的节点必须画（<pre>/<div>文字</div> 这种没底色的容器否则整个消失）。
        if (((dc >> 24) & 0xFF) == 0 && !hasBorder && !hasGrad && node.text.empty() && shN == 0) {
            // GDI 没有 alpha 合成：全透明节点画出来是一块黑。图片/视频帧例外（只看自身像素）
            const bool drawsOwnPixels =
                (node.type == BINUI_IMAGE && renderer.getImage(node.imageIndex) != nullptr) ||
                node.type == BINUI_VIDEO || node.type == BINUI_PATH;
            if (!drawsOwnPixels) continue;
        }


        // 与脏矩形不相交的节点快速跳过（GDI 裁剪链较贵，仅在相交节点上做）
        if (x - shPadXi + nw <= clip.left || x + shPadXi >= clip.right ||
            y - shPadYi + nh <= clip.top || y + shPadYi >= clip.bottom)
            continue;
        // 滚动容器内：视口外的节点（内容溢出/未滚动到）纯整数裁剪，避免 GDI 链
        if (hasVp[i] && (x - shPadXi + nw <= vpL[i] || x + shPadXi >= vpR[i] ||
                         y - shPadYi + nh <= vpT[i] || y + shPadYi >= vpB[i]))
            continue;
        // BINUI_LINE：x,y = 起点；width,height = 终点；radius = 线宽。
        // 这条分支以前在 GDI 后端是缺的 —— SVG 的 line / polyline / polygon 在 GDI
        // 画布上一根都画不出来（GL/D3D 正常），三端表现不一致。另外线段的包围盒
        // 不是 (x,y,w,h)，必须按端点自己算，否则斜线会被节点矩形整条裁掉。
        if (node.type == BINUI_LINE) {
            const float ddx = renderer.drawOffsetX(node.id);
            const float ddy = renderer.drawOffsetY(node.id);
            const float x1 = node.x - offX[i] + ddx, y1 = node.y - offY[i] + ddy;
            const float x2 = node.width - offX[i] + ddx, y2 = node.height - offY[i] + ddy;
            const int lw = node.radius > 0 ? (int)node.radius : 1;
            const float mnx = std::min(x1, x2) - lw, mxx = std::max(x1, x2) + lw;
            const float mny = std::min(y1, y2) - lw, mxy = std::max(y1, y2) + lw;
            if (mxx <= clip.left || mnx >= clip.right || mxy <= clip.top || mny >= clip.bottom)
                continue;
            if (hasVp[i] && (mxx <= vpL[i] || mnx >= vpR[i] || mxy <= vpT[i] || mny >= vpB[i]))
                continue;
            uint32_t lc = renderer.drawColor(node.id);
            float lop = renderer.drawOpacity(node.id);
            if (renderer.isDisabled(node.id)) lop *= 0.55f;
            // GDI 画布没有 alpha 合成：半透明线段按与页面底色混合近似
            const uint32_t la = (lc >> 24) & 0xFFu;
            if (la < 255u || lop < 1.0f) {
                float a = (float)la * (lop < 1.0f ? lop : 1.0f);
                lc = BlendOver((lc & 0x00FFFFFFu) | ((uint32_t)(a + 0.5f) << 24), pageBg);
            }
            int lSaved = SaveDC(hdc);
            RECT lr = { (LONG)std::floor(mnx), (LONG)std::floor(mny),
                        (LONG)std::ceil(mxx), (LONG)std::ceil(mxy) };
            bool lDraw = IntersectClipRect(hdc, lr.left, lr.top, lr.right, lr.bottom) != NULLREGION;
            size_t lcur = i;
            while (lDraw) {
                const auto& anc = nodes[lcur];
                if (anc.parentId == 0xFFFF) break;
                auto pit = byId.find(anc.parentId);
                if (pit == byId.end()) break;
                lcur = pit->second;
                const auto& par = nodes[lcur];
                if (!(par.flags & BINUI_VISIBLE)) { lDraw = false; break; }
                const float pox = offX[lcur], poy = offY[lcur];
                RECT pr = { (LONG)(par.x - pox), (LONG)(par.y - poy),
                            (LONG)(par.x - pox + par.width), (LONG)(par.y - poy + par.height) };
                if (IntersectClipRect(hdc, pr.left, pr.top, pr.right, pr.bottom) == NULLREGION) {
                    lDraw = false;
                    break;
                }
            }
            if (lDraw) {
                HPEN pen = CreatePen(PS_SOLID, lw,
                                     RGB((lc >> 16) & 0xFF, (lc >> 8) & 0xFF, lc & 0xFF));
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                MoveToEx(hdc, (int)std::lround(x1), (int)std::lround(y1), nullptr);
                LineTo(hdc, (int)std::lround(x2), (int)std::lround(y2));
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }
            RestoreDC(hdc, lSaved);
            continue;
        }

        RECT rc = { x, y, x + nw, y + nh };
        int pad = node.padding;

        // ---- v13 扩展绘制：底色/边框的统一入口 ----
        const float nfx = (float)x, nfy = (float)y, nfw = (float)nw, nfh = (float)nh;
        uint32_t underBg = pageBg;
        {
            auto pit = byId.find(node.parentId);
            if (pit != byId.end()) {
                const uint16_t pid = nodes[pit->second].id;
                uint32_t pbg = renderer.drawColor(pid);
                // 0.16.8：父元素可能被 opacity 淡过（自己或祖先）。GDI 没有 alpha 合成，
                // 这里先把它的"看起来的颜色"近似算出来（与页面底色混合），子元素再往上叠，
                // 否则半透明父容器里的子元素会按"父元素的原始饱和色"去混合，明显偏深。
                const float pop = renderer.drawOpacity(pid);
                if (pop < 1.0f) {
                    const uint32_t oa = (uint32_t)(((pbg >> 24) & 0xFFu) * pop + 0.5f);
                    pbg = BlendOver((oa << 24) | (pbg & 0x00FFFFFFu), pageBg);
                }
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
        // 0.16.8：级联后的 opacity 折进颜色（GDI 没有分层合成，按与底色混合近似）
        auto fadeOpac = [&](uint32_t c) -> uint32_t {
            if (nodeOp >= 1.0f) return c;
            const int a = (int)(((c >> 24) & 0xFFu) * nodeOp + 0.5f);
            return fadeOver(c, a);
        };
        auto fillNodeBg = [&](uint32_t color, float radius) {
            binui::PaintBand bands[binui::kMaxPaintBands];
            binui::PaintBand* bp = bands;
            int bcap = binui::kMaxPaintBands;
            // 任意角度渐变（v0.16.13）条带是千级（见 binui_paint.h 的 kAngleGradBands），
            // 跟共享遍历 binui_draw_nodes.h 用同一套复用缓冲策略
            if (pt && (pt->flags & PAINT_GRADIENT) && pt->reserve[1] != 0) {
                static std::vector<binui::PaintBand> angleBands;
                angleBands.resize(binui::kAngleGradBands);
                bp = angleBands.data();
                bcap = binui::kAngleGradBands;
            }
            const int bn = binui::BuildFillBands(pt, nfx, nfy, nfw, nfh, radius, color, bp, bcap);
            FillPaintBands(hdc, bp, bn, underBg);
        };
        auto drawNodeBorder = [&]() {
            if (!hasBorder) return;
            binui::PaintBand bb[binui::kMaxBorderBands];
            const int bn = binui::BuildBorderBands(pt, nfx, nfy, nfw, nfh, (float)node.radius, bb);
            FillPaintBands(hdc, bb, bn, dc | 0xFF000000u);
        };

        // ---- v23 box-shadow：先画阴影，再进"节点自身盒子"的裁剪 ----
        // 阴影属于元素自己的背景层（CSS 里画在 background 之下），而盒子裁剪会把它整块
        // 切掉，所以这段单独开一层裁剪：自身盒**外扩**到阴影范围，祖先裁剪照旧。
        // GDI 没有 alpha 合成：每环按"目标累计覆盖率 × 阴影色"与父容器底色预混成不透明色。
        // 由内向外画时后画的环整块盖住先画的，所以最终颜色 = 包住该点的最内环 ⇒ 正确。
        if (shN > 0 && nodeOp > 0.002f) {
            int sSaved = SaveDC(hdc);
            RECT sr = { (LONG)std::floor((float)x - shPadX), (LONG)std::floor((float)y - shPadY),
                        (LONG)std::ceil((float)(x + nw) + shPadX),
                        (LONG)std::ceil((float)(y + nh) + shPadY) };
            bool sDraw = IntersectClipRect(hdc, sr.left, sr.top, sr.right, sr.bottom) != NULLREGION;
            size_t scur = i;
            while (sDraw) {
                const auto& anc = nodes[scur];
                if (anc.parentId == 0xFFFF) break;
                auto pit = byId.find(anc.parentId);
                if (pit == byId.end()) break;
                scur = pit->second;
                const auto& par = nodes[scur];
                if (!(par.flags & BINUI_VISIBLE)) { sDraw = false; break; }
                RECT pr = { (int)(par.x - offX[scur]), (int)(par.y - offY[scur]),
                            (int)(par.x - offX[scur] + par.width),
                            (int)(par.y - offY[scur] + par.height) };
                if (IntersectClipRect(hdc, pr.left, pr.top, pr.right, pr.bottom) == NULLREGION) {
                    sDraw = false;
                    break;
                }
            }
            if (sDraw) {
                for (int k = 0; k < shN; k++) {
                    binui::BinUIShadow s2 = shs[k];
                    if (nodeScale != 1.0f) {           // 缩放动画：阴影跟着一起放大
                        s2.offsetX *= nodeScale; s2.offsetY *= nodeScale;
                        s2.blur *= nodeScale;    s2.spread *= nodeScale;
                    }
                    const int srad = (int)std::lround((float)node.radius *
                                                      (nodeScale > 0.0f ? nodeScale : 1.0f));
                    int offXb = 0, offYb = 0;
                    ShadowBitmap* sb = ShadowBitmapGet(hdc, nw, nh, srad, s2, offXb, offYb);
                    if (!sb) continue;
                    // 一次 AlphaBlend 完成整条阴影：预乘位图 + 逐像素 alpha，
                    // 叠加顺序与 CSS 一致（作者先写的在最后画 = 压在上面）
                    BLENDFUNCTION bf = { AC_SRC_OVER, 0,
                                         (BYTE)(nodeOp * 255.0f + 0.5f), AC_SRC_ALPHA };
                    const int dx = x + offXb, dy = y + offYb;
                    static const bool shDbg = std::getenv("BINUI_SHADOW_DBG") != nullptr;
                    if (shDbg)
                        std::fprintf(stderr,
                                     "[shadow] node=%u box=(%d,%d %dx%d) r=%d off=(%.1f,%.1f) "
                                     "blur=%.1f spread=%.1f bmp=%dx%d at=(%d,%d) op=%.2f\n",
                                     (unsigned)node.id, x, y, nw, nh, srad, (double)s2.offsetX,
                                     (double)s2.offsetY, (double)s2.blur, (double)s2.spread,
                                     sb->w, sb->h, dx, dy, (double)nodeOp);
                    AlphaBlend(hdc, dx, dy, sb->w, sb->h, sb->dc, 0, 0, sb->w, sb->h, bf);
                }
            }
            RestoreDC(hdc, sSaved);
        }

        // 层级裁剪：先把裁剪区设为自身（滚动后）范围，再与「裁剪容器」祖先求交；
        // 交集为空则跳过绘制。裁剪容器只有 overflow:auto/scroll/hidden（编译期标 SCROLLABLE）——
        // 普通 div（含组件自己的根 div）按 CSS 语义不裁子元素，否则组件里 hover
        // 放大 1.04 的按钮会被与它等大的根 div 切回原来那块。
        int savedDC = SaveDC(hdc);
        bool draw = true;
        RECT nr = rc;
        if (IntersectClipRect(hdc, nr.left, nr.top, nr.right, nr.bottom) == NULLREGION)
            draw = false;

        renderer.clippingAncestors(node.id, clipAncs);
        for (uint16_t ancId : clipAncs) {
            auto pit = byId.find(ancId);
            if (pit == byId.end()) continue;
            const auto& par = nodes[pit->second];
            if (!(par.flags & BINUI_VISIBLE)) { draw = false; break; }
            RECT pr = { (int)(par.x - offX[pit->second]), (int)(par.y - offY[pit->second]),
                        (int)(par.x - offX[pit->second] + par.width),
                        (int)(par.y - offY[pit->second] + par.height) };
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
                SetTextColor(hdc, RGB((fadeOpac(tcol) >> 16) & 0xFF, (fadeOpac(tcol) >> 8) & 0xFF, fadeOpac(tcol) & 0xFF));
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
                    // v21 自然排版：一行放不下就折行（与 GPU 后端同一口径，见
                    // binui_draw_nodes.h 的 LABEL 分支）。以前是"超宽 20% 才折"，
                    // 与 GPU 端对不上：同一份 UI 在 GDI 里单行裁掉、在 GL 里折成两行。
                    if (mr.right > (nw - 2 * pad))
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
                    SetTextColor(hdc, RGB((fadeOpac(tcol) >> 16) & 0xFF, (fadeOpac(tcol) >> 8) & 0xFF, fadeOpac(tcol) & 0xFF));
                    // 字重：作者声明了 font-weight 就用声明值（取法与 LABEL / LIST 同一套
                    // PaintWeightOf → FW_* 换算）；没声明才回落 700 粗体，
                    // 以前写死 FW_BOLD，font-weight 写了也没效果。
                    const int fw = (pt && (pt->flags & PAINT_WEIGHT))
                                 ? binui::PaintWeightOf(pt) : 700;
                    HFONT font = CreateFontA(
                        -(node.fontSize ? node.fontSize : 14), 0, 0, 0,
                        fw >= 600 ? FW_BOLD : FW_NORMAL,
                        italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        gGdiFontQuality, DEFAULT_PITCH, gGdiFontName.c_str());
                    HGDIOBJ oldFont = SelectObject(hdc, font);
                    RECT tr = { x + pad, y, x + nw - pad, y + nh };
                    // 水平对齐：与 LABEL 分支同一套 flags 语义（都不置位 = 左对齐）。
                    // 以前固定 DT_CENTER，text-align:left 与 padding 都被忽略。
                    const UINT halign = (node.flags & BINUI_TEXT_CENTER) ? DT_CENTER
                                      : (node.flags & BINUI_TEXT_RIGHT) ? DT_RIGHT : DT_LEFT;
                    // 文本是 UTF-8：必须转宽字符用 DrawTextW。早先用 DrawTextA，
                    // 中文会被按系统代码页解释成乱码（叠加成一团）。
                    std::wstring wtext = LfToCrlf(Utf8ToWide(node.text));
                    DrawTextW(hdc, wtext.c_str(), -1, &tr,
                        halign | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, oldFont);
                    DeleteObject(font);
                }
                break;
            }
            case BINUI_INPUT: {
                // 输入框背景（style 里的 background / 渐变）
                fillNodeBg(dc, (float)node.radius);

                // 输入框的强调色：作者给了 textColor 就用它，否则回退内置紫
                //（以前写死 #BB86FC，浅色主题下没法跟着配色走）
                const uint32_t inAccent = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                              ? pt->textColor : 0xFFBB86FCu;
                // 底部强调线压到 35%（GDI 无 alpha，按与底色混合近似）。
                // 作者给了 border 就不再画：那是直角 Material 下划线的遗留，
                // 在圆角带框的输入框上会变成一道悬空横杠（v13 之后已多余）
                const bool inHasBorder = (pt && (pt->flags & PAINT_BORDER));
                if (!inHasBorder) {
                    const uint32_t inLineCol = fadeOver(inAccent, 90);
                    HPEN underlinePen = CreatePen(PS_SOLID, 2,
                                                  RGB((inLineCol >> 16) & 0xFF, (inLineCol >> 8) & 0xFF, inLineCol & 0xFF));
                    HGDIOBJ oldPen = SelectObject(hdc, underlinePen);
                    MoveToEx(hdc, x, y + nh - 2, nullptr);
                    LineTo(hdc, x + nw, y + nh - 2);
                    SelectObject(hdc, oldPen);
                    DeleteObject(underlinePen);
                }

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
                    // 光标/选区的竖直范围 = 文字行高，不是整条节点高度
                    //（以前用 tr.top+2 .. tr.bottom-2：36px 的输入框里光标有 32px）
                    TEXTMETRICW tm = {};
                    GetTextMetricsW(hdc, &tm);
                    const int glineH = tm.tmHeight > 0 ? tm.tmHeight : (node.fontSize ? node.fontSize : 14);
                    const int glineTop = y + (nh - glineH) / 2;

                    std::wstring wtext = Utf8ToWide(text);
                    // 占位符画 45% 淡、禁用再压到 55%（GDI 无 alpha，按与底色混合近似）
                    uint32_t inCol = inAccent;
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
                            XorFillRect(hdc, ra.right, glineTop, rb.right, glineTop + glineH);
                        }

                        // 闪烁光标：测量光标前文本宽度，XOR 竖线
                        if (caretBlinkOn()) {
                            RECT cr = { tr.left, 0, tr.right, 10 };
                            std::wstring wc = Utf8ToWide(text.substr(0, caret));
                            DrawTextW(hdc, wc.c_str(), -1, &cr,
                                DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
                            XorFillRect(hdc, cr.right, glineTop, cr.right + 2, glineTop + glineH);
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
                                              ? pt->textColor
                                              : binui::PaintContrastText(dc | 0xFF000000u);
                    if (disabled) tcol = fadeOver(tcol, 140);
                    SetTextColor(hdc, RGB((fadeOpac(tcol) >> 16) & 0xFF, (fadeOpac(tcol) >> 8) & 0xFF, fadeOpac(tcol) & 0xFF));
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
            case BINUI_PATH: {
                // v15 SVG 路径：节点盒 = 几何包围盒，折线在路径段里。
                // 填充走条带 + FillPaintBands（GDI 没有 alpha 合成，按页面底色混合近似）；
                // 描边逐段画线、转角补圆 —— 几何与 GL/D3D 共用 binui_paint.h 那一份。
                const auto* pr = renderer.pathOf(node.id);
                if (!pr || pr->first.empty()) break;
                const float pop = disabled ? renderer.drawOpacity(node.id) * 0.55f
                                           : renderer.drawOpacity(node.id);
                auto fadeA = [&](uint32_t c) -> uint32_t {
                    if (pop >= 1.0f) return c;
                    const uint32_t a = (uint32_t)(((c >> 24) & 0xFFu) * pop + 0.5f);
                    return (a << 24) | (c & 0x00FFFFFFu);
                };
                const uint32_t pfc = fadeA(pr->head.fillColor);
                const uint32_t psc = fadeA(pr->head.strokeColor);
                const uint32_t pbg = underBg;   // 与边框/文字同一底色口径
                static thread_local binui::PathRenderBuffers pbuf;
                // 描边笔按"宽度 + 颜色"缓存：一条折线几十上百段，每段建/删笔纯属浪费；
                // 渐变描边每段颜色都不同，缓存键里必须带颜色，否则会拿到上一段的笔。
                HPEN pen = nullptr;
                int penW = -1;
                uint32_t penC = 0;
                auto penFor = [&](float lw, uint32_t color) -> HPEN {
                    const int iw = std::max(1, (int)std::lround(lw));
                    const uint32_t c = BlendOver(color, pbg);
                    if (pen && penW == iw && penC == c) return pen;
                    if (pen) DeleteObject(pen);
                    pen = CreatePen(PS_SOLID, iw, RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
                    penW = iw;
                    penC = c;
                    return pen;
                };
                binui::PathPaintSpec pspec;
                pspec.fillColor = pfc;
                pspec.strokeColor = psc;
                pspec.fillGrad = pr->hasGrad ? &pr->grad : nullptr;
                pspec.strokeGrad = pr->hasStrokeGrad ? &pr->strokeGrad : nullptr;
                pspec.opacity = pop;
                binui::PaintPathNode(
                    pr->head, pr->pts.data(), pr->first.data(), pr->count.data(),
                    pr->closed.data(), static_cast<int>(pr->first.size()),
                    nfx, nfy, nfw, nfh, pspec,
                    renderer.pathBandCache(node.id), renderer.pathCacheGen(),
                    renderer.pathBandsBudget(), pbuf,
                    [&](const binui::PaintBand* b, int n) { FillPaintBands(hdc, b, n, pbg); },
                    [&](float x0, float y0, float x1, float y1, float lw, uint32_t lc) {
                        HPEN hp = penFor(lw, lc);
                        if (!hp) return;
                        HGDIOBJ old = SelectObject(hdc, hp);
                        MoveToEx(hdc, (int)std::lround(x0), (int)std::lround(y0), nullptr);
                        LineTo(hdc, (int)std::lround(x1), (int)std::lround(y1));
                        SelectObject(hdc, old);
                    },
                    [&](float rx, float ry, float rw, float rh, float, uint32_t dc) {
                        const uint32_t c = BlendOver(dc, pbg);
                        HBRUSH br = CreateSolidBrush(RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
                        HGDIOBJ ob = SelectObject(hdc, br);
                        Ellipse(hdc, (int)std::lround(rx), (int)std::lround(ry),
                                (int)std::lround(rx + rw), (int)std::lround(ry + rh));
                        SelectObject(hdc, ob);
                        DeleteObject(br);
                    });
                renderer.commitPathBands(node.id);   // 缓存记账（全局预算用）
                if (pen) DeleteObject(pen);
                break;
            }
        }

        // 边框（v13）：画在节点内侧，四边可不同宽，压在底色之上
        drawNodeBorder();

        // 焦点环三态（与 GL/D3D 后端同一套语义，见 docs/keyboard_focus.md）：
        //   输入元素待编辑 = 虚线 + 对比色（2 秒无操作自动消失）
        //   按钮等普通焦点 = 实线 + 对比色（同样 2 秒自动消失）
        //   编辑中         = 实线加粗 + 强调蓝（常亮）
        // 线型 + 颜色双编码：只换颜色对色弱用户不友好，虚线/实线一眼能分。
        {
            const binui::BinUIRenderer::FocusRing ring = renderer.focusRingOf(node.id);
            if (draw && ring != binui::BinUIRenderer::FocusRing::None) {
                const bool editing = ring == binui::BinUIRenderer::FocusRing::Editing;
                const bool dashed = ring == binui::BinUIRenderer::FocusRing::Dashed;
                const int fr = node.radius > 2 ? (int)node.radius - 1 : 0;
                uint32_t ringCol = 0xFFFFFFFFu;
                if (editing) {
                    ringCol = binui::BinUIRenderer::kFocusRingEditColor;
                } else if (((dc >> 24) & 0xFFu) >= 0x80u) {
                    const float bgLum = (0.2126f * static_cast<float>((dc >> 16) & 0xFFu) +
                                         0.7152f * static_cast<float>((dc >> 8) & 0xFFu) +
                                         0.0722f * static_cast<float>(dc & 0xFFu)) / 255.0f;
                    if (bgLum > 0.7f) ringCol = 0xFF1A1A1Au;
                }
                HPEN fp = CreatePen(PS_SOLID, editing ? 2 : 1,
                                    RGB((ringCol >> 16) & 0xFF, (ringCol >> 8) & 0xFF, ringCol & 0xFF));
                HGDIOBJ oldFp = SelectObject(hdc, fp);
                if (dashed) {
                    // 虚线：沿圆角路径逐段 LineTo（不依赖 PS_DASH 在不同驱动上的实现差异）
                    binui::PaintSegment segs[binui::kMaxFocusDashSegments];
                    const int n = binui::BuildDashedRoundRing(static_cast<float>(x) + 1.0f,
                                                              static_cast<float>(y) + 1.0f,
                                                              static_cast<float>(nw) - 2.0f,
                                                              static_cast<float>(nh) - 2.0f,
                                                              static_cast<float>(fr), 6.0f, 4.0f,
                                                              segs, binui::kMaxFocusDashSegments);
                    for (int si = 0; si < n; si++) {
                        MoveToEx(hdc, (int)std::lround(segs[si].x0), (int)std::lround(segs[si].y0), nullptr);
                        LineTo(hdc, (int)std::lround(segs[si].x1), (int)std::lround(segs[si].y1));
                    }
                } else {
                    HGDIOBJ oldFb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    RoundRect(hdc, x + 1, y + 1, x + nw - 1, y + nh - 1, fr * 2, fr * 2);
                    SelectObject(hdc, oldFb);
                }
                SelectObject(hdc, oldFp);
                DeleteObject(fp);
            }
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
        float cw = renderer.contentWidth(node.id);
        int x = (int)node.x, y = (int)node.y, nw = (int)node.width, nh = (int)node.height;
        float sx = sit != scrolls.end() ? sit->second.x : 0;
        const bool needV = ch > nh + 0.5f;
        const bool needH = cw > nw + 0.5f;
        if (!needV && !needH) continue; // 两个方向都不溢出就不画

        // 滚动条配色/宽度由渲染器提供（C ABI 与编译器 scrollbar-* 落到同一处）。
        // 以前写死 8px + #2E2E2E/#5A5A5A，浅色主题下就是右边一条黑杠，
        // 而且没有任何入口能覆盖。GDI 无 alpha 合成，颜色取 RGB 分量即可。
        const auto& sb = renderer.scrollbarStyle();
        const int barW = sb.width;
        HBRUSH track = CreateSolidBrush(RGB((sb.track >> 16) & 0xFF, (sb.track >> 8) & 0xFF,
                                            sb.track & 0xFF));
        HBRUSH thumb = CreateSolidBrush(RGB((sb.thumb >> 16) & 0xFF, (sb.thumb >> 8) & 0xFF,
                                            sb.thumb & 0xFF));

        if (needV) {
            RECT tr = { x + nw - barW, y, x + nw, y + nh };
            FillRect(hdc, &tr, track);
            // 滑块：高度 = 视口比例，位置 = 滚动比例（位置算法保持原样）
            float thumbH = std::max(24.0f, (float)nh * nh / ch);
            float maxScroll = ch - nh;
            float thumbY = maxScroll > 0 ? sy / maxScroll * (nh - thumbH) : 0;
            // 滑块宽度沿用"轨道一半"的老比例（轨道 8 → 滑块 4），左右各留 (barW-thumbW)/2
            const int thumbW = std::max(2, barW / 2);
            const int thumbX = x + nw - barW + (barW - thumbW) / 2;
            RECT ur = { thumbX, y + (int)thumbY, thumbX + thumbW, y + (int)(thumbY + thumbH) };
            FillRect(hdc, &ur, thumb);
        }
        if (needH) {
            // 水平条：贴底边，宽度扣掉竖直条那一格，避免右下角叠在一起
            const int trackW = nw - (needV ? barW : 0);
            RECT tr = { x, y + nh - barW, x + trackW, y + nh };
            FillRect(hdc, &tr, track);
            float thumbW = std::max(24.0f, (float)nw * nw / cw);
            float maxScroll = cw - nw;
            float thumbX = maxScroll > 0 ? sx / maxScroll * (nw - thumbW) : 0;
            const int thick = std::max(2, barW / 2);
            const int thumbY = y + nh - barW + (barW - thick) / 2;
            RECT ur = { x + (int)thumbX, thumbY, x + (int)(thumbX + thumbW), thumbY + thick };
            FillRect(hdc, &ur, thumb);
        }
        DeleteObject(track);
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

