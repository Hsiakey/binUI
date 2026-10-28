// binui_gl.cpp - OpenGL GPU 渲染后端实现
// 架构与 binui_d3d11.cpp 完全同构：字形图集（FreeType 光栅化一次）+ 圆角九宫格模板 +
// 顶点批次；drawNodesGLGdi 与 drawNodesGDI/D3D 语义一致地遍历节点树，全部绘制为 GPU 四边形；
// BinUIAppGLGdi 提供无边框窗口与事件循环（VSync 按需呈现）。
// OpenGL 3.3 core（WGL 创建上下文 + GLSL 330），着色器数学与 D3D11 HLSL 版一致：
// 预乘 alpha 混合、纹理 × 顶点色、NDC 映射 viewSize。
// 文字渲染委托 binui_font_renderer（FreeType + HarfBuzz），与平台解耦。
#include "backends/binui_gl-gdi.h"

#ifndef _WIN32
#error "binui_gl is Windows-only (WGL); port by replacing context/swap with GLX/EGL"
#endif

#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#if __has_include(<GL/glext.h>)
#include <GL/glext.h>   // MinGW 的 gl.h 仅含 GL 1.1：扩展常量/类型来自 glext.h
#include <GL/wglext.h>
#else
#include "binui_gl_ext_min.h" // MSVC/Windows SDK 无 glext：最小扩展存根（值与 Khronos 一致）
#endif
#include <gdiplus.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>

#include "binui_font_renderer.h"
#include "binui_paint.h"   // v13 扩展绘制的条带几何（与 GDI/D3D11 后端共用同一份数学）

using namespace binui;

// ============================================================
// OpenGL 函数装载（wglGetProcAddress + opengl32 回退；GL 1.1 直接链接）
// ============================================================
typedef HGLRC(APIENTRY* PFNWGLCREATECONTEXTATTRIBSARB)(HDC, HGLRC, const int*);
typedef BOOL (APIENTRY* PFNWGLSWAPINTERVALEXT)(int);
typedef GLuint(APIENTRY* PFNGLCREATESHADER)(GLenum);
typedef void (APIENTRY* PFNGLSHADERSOURCE)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (APIENTRY* PFNGLCOMPILESHADER)(GLuint);
typedef void (APIENTRY* PFNGLGETSHADERIV)(GLuint, GLenum, GLint*);
typedef void (APIENTRY* PFNGLGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint(APIENTRY* PFNGLCREATEPROGRAM)(void);
typedef void (APIENTRY* PFNGLATTACHSHADER)(GLuint, GLuint);
typedef void (APIENTRY* PFNGLLINKPROGRAM)(GLuint);
typedef void (APIENTRY* PFNGLGETPROGRAMIV)(GLuint, GLenum, GLint*);
typedef void (APIENTRY* PFNGLGETPROGRAMINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (APIENTRY* PFNGLDELETESHADER)(GLuint);
typedef void (APIENTRY* PFNGLDELETEPROGRAM)(GLuint);
typedef void (APIENTRY* PFNGLUSEPROGRAM)(GLuint);
typedef GLint(APIENTRY* PFNGLGETUNIFORMLOCATION)(GLuint, const GLchar*);
typedef void (APIENTRY* PFNGLUNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY* PFNGLUNIFORM1I)(GLint, GLint);
typedef void (APIENTRY* PFNGLGENVERTEXARRAYS)(GLsizei, GLuint*);
typedef void (APIENTRY* PFNGLDELETEVERTEXARRAYS)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFNGLBINDVERTEXARRAY)(GLuint);
typedef void (APIENTRY* PFNGLGENBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY* PFNGLDELETEBUFFERS)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFNGLBINDBUFFER)(GLenum, GLuint);
typedef void (APIENTRY* PFNGLBUFFERDATA)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (APIENTRY* PFNGLVERTEXATTRIBPOINTER)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void (APIENTRY* PFNGLENABLEVERTEXATTRIBARRAY)(GLuint);
typedef void (APIENTRY* PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY* PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY* PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (APIENTRY* PFNGLBLITFRAMEBUFFER)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (APIENTRY* PFNGLGENRENDERBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY* PFNGLDELETERENDERBUFFERS)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFNGLBINDRENDERBUFFER)(GLenum, GLuint);
typedef void (APIENTRY* PFNGLRENDERBUFFERSTORAGEMULTISAMPLE)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY* PFNGLFRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);

namespace {
PFNWGLCREATECONTEXTATTRIBSARB wglCreateContextAttribsARB = nullptr;
PFNWGLSWAPINTERVALEXT wglSwapIntervalEXT = nullptr;
PFNGLCREATESHADER      glCreateShaderP      = nullptr;
PFNGLSHADERSOURCE      glShaderSourceP      = nullptr;
PFNGLCOMPILESHADER     glCompileShaderP     = nullptr;
PFNGLGETSHADERIV       glGetShaderivP       = nullptr;
PFNGLGETSHADERINFOLOG  glGetShaderInfoLogP  = nullptr;
PFNGLCREATEPROGRAM     glCreateProgramP     = nullptr;
PFNGLATTACHSHADER      glAttachShaderP      = nullptr;
PFNGLLINKPROGRAM       glLinkProgramP       = nullptr;
PFNGLGETPROGRAMIV      glGetProgramivP      = nullptr;
PFNGLGETPROGRAMINFOLOG glGetProgramInfoLogP = nullptr;
PFNGLDELETESHADER      glDeleteShaderP      = nullptr;
PFNGLDELETEPROGRAM     glDeleteProgramP     = nullptr;
PFNGLUSEPROGRAM        glUseProgramP        = nullptr;
PFNGLGETUNIFORMLOCATION glGetUniformLocationP = nullptr;
PFNGLUNIFORM2F         glUniform2fP         = nullptr;
PFNGLUNIFORM1I         glUniform1iP         = nullptr;
PFNGLGENVERTEXARRAYS   glGenVertexArraysP   = nullptr;
PFNGLDELETEVERTEXARRAYS glDeleteVertexArraysP = nullptr;
PFNGLBINDVERTEXARRAY   glBindVertexArrayP   = nullptr;
PFNGLGENBUFFERS        glGenBuffersP        = nullptr;
PFNGLDELETEBUFFERS     glDeleteBuffersP     = nullptr;
PFNGLBINDBUFFER        glBindBufferP        = nullptr;
PFNGLBUFFERDATA        glBufferDataP        = nullptr;
PFNGLVERTEXATTRIBPOINTER glVertexAttribPointerP = nullptr;
PFNGLENABLEVERTEXATTRIBARRAY glEnableVertexAttribArrayP = nullptr;
PFNGLGENFRAMEBUFFERS   glGenFramebuffersP   = nullptr;
PFNGLDELETEFRAMEBUFFERS glDeleteFramebuffersP = nullptr;
PFNGLBINDFRAMEBUFFER   glBindFramebufferP   = nullptr;
PFNGLFRAMEBUFFERTEXTURE2D glFramebufferTexture2DP = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUS glCheckFramebufferStatusP = nullptr;
PFNGLBLITFRAMEBUFFER    glBlitFramebufferP    = nullptr;
PFNGLGENRENDERBUFFERS   glGenRenderbuffersP   = nullptr;
PFNGLDELETERENDERBUFFERS glDeleteRenderbuffersP = nullptr;
PFNGLBINDRENDERBUFFER   glBindRenderbufferP   = nullptr;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLE glRenderbufferStorageMultisampleP = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFER glFramebufferRenderbufferP = nullptr;

void* GetGLProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!p) p = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("opengl32.dll"), name));
    return p;
}

bool LoadGL() {
#define LOAD_PROC(var, func, type) do { \
        var = reinterpret_cast<type>(GetGLProc(func)); \
        if (!var) { std::fprintf(stderr, "[gl] missing function: %s\n", func); return false; } \
    } while (0)
    LOAD_PROC(glCreateShaderP, "glCreateShader", PFNGLCREATESHADER);
    LOAD_PROC(glShaderSourceP, "glShaderSource", PFNGLSHADERSOURCE);
    LOAD_PROC(glCompileShaderP, "glCompileShader", PFNGLCOMPILESHADER);
    LOAD_PROC(glGetShaderivP, "glGetShaderiv", PFNGLGETSHADERIV);
    LOAD_PROC(glGetShaderInfoLogP, "glGetShaderInfoLog", PFNGLGETSHADERINFOLOG);
    LOAD_PROC(glCreateProgramP, "glCreateProgram", PFNGLCREATEPROGRAM);
    LOAD_PROC(glAttachShaderP, "glAttachShader", PFNGLATTACHSHADER);
    LOAD_PROC(glLinkProgramP, "glLinkProgram", PFNGLLINKPROGRAM);
    LOAD_PROC(glGetProgramivP, "glGetProgramiv", PFNGLGETPROGRAMIV);
    LOAD_PROC(glGetProgramInfoLogP, "glGetProgramInfoLog", PFNGLGETPROGRAMINFOLOG);
    LOAD_PROC(glDeleteShaderP, "glDeleteShader", PFNGLDELETESHADER);
    LOAD_PROC(glDeleteProgramP, "glDeleteProgram", PFNGLDELETEPROGRAM);
    LOAD_PROC(glUseProgramP, "glUseProgram", PFNGLUSEPROGRAM);
    LOAD_PROC(glGetUniformLocationP, "glGetUniformLocation", PFNGLGETUNIFORMLOCATION);
    LOAD_PROC(glUniform2fP, "glUniform2f", PFNGLUNIFORM2F);
    LOAD_PROC(glUniform1iP, "glUniform1i", PFNGLUNIFORM1I);
    LOAD_PROC(glGenVertexArraysP, "glGenVertexArrays", PFNGLGENVERTEXARRAYS);
    LOAD_PROC(glDeleteVertexArraysP, "glDeleteVertexArrays", PFNGLDELETEVERTEXARRAYS);
    LOAD_PROC(glBindVertexArrayP, "glBindVertexArray", PFNGLBINDVERTEXARRAY);
    LOAD_PROC(glGenBuffersP, "glGenBuffers", PFNGLGENBUFFERS);
    LOAD_PROC(glDeleteBuffersP, "glDeleteBuffers", PFNGLDELETEBUFFERS);
    LOAD_PROC(glBindBufferP, "glBindBuffer", PFNGLBINDBUFFER);
    LOAD_PROC(glBufferDataP, "glBufferData", PFNGLBUFFERDATA);
    LOAD_PROC(glVertexAttribPointerP, "glVertexAttribPointer", PFNGLVERTEXATTRIBPOINTER);
    LOAD_PROC(glEnableVertexAttribArrayP, "glEnableVertexAttribArray", PFNGLENABLEVERTEXATTRIBARRAY);
    LOAD_PROC(glGenFramebuffersP, "glGenFramebuffers", PFNGLGENFRAMEBUFFERS);
    LOAD_PROC(glDeleteFramebuffersP, "glDeleteFramebuffers", PFNGLDELETEFRAMEBUFFERS);
    LOAD_PROC(glBindFramebufferP, "glBindFramebuffer", PFNGLBINDFRAMEBUFFER);
    LOAD_PROC(glFramebufferTexture2DP, "glFramebufferTexture2D", PFNGLFRAMEBUFFERTEXTURE2D);
    LOAD_PROC(glCheckFramebufferStatusP, "glCheckFramebufferStatus", PFNGLCHECKFRAMEBUFFERSTATUS);
    LOAD_PROC(glBlitFramebufferP, "glBlitFramebuffer", PFNGLBLITFRAMEBUFFER);
    LOAD_PROC(glGenRenderbuffersP, "glGenRenderbuffers", PFNGLGENRENDERBUFFERS);
    LOAD_PROC(glDeleteRenderbuffersP, "glDeleteRenderbuffers", PFNGLDELETERENDERBUFFERS);
    LOAD_PROC(glBindRenderbufferP, "glBindRenderbuffer", PFNGLBINDRENDERBUFFER);
    LOAD_PROC(glRenderbufferStorageMultisampleP, "glRenderbufferStorageMultisample", PFNGLRENDERBUFFERSTORAGEMULTISAMPLE);
    LOAD_PROC(glFramebufferRenderbufferP, "glFramebufferRenderbuffer", PFNGLFRAMEBUFFERRENDERBUFFER);
#undef LOAD_PROC
    return true;
}
} // namespace

namespace {

// ---------- 小工具 ----------
// 按透明度缩放 alpha（0xAARRGGBB；RGB 保持直线色，Premult 时统一缩放）
uint32_t ScaleAlpha(uint32_t c, float op) {
    if (op >= 1.0f) return c;
    if (op <= 0.0f) return c & 0x00FFFFFF;
    uint32_t a = (uint32_t)(((c >> 24) & 0xFF) * op);
    if (a > 255) a = 255;
    return (c & 0x00FFFFFF) | (a << 24);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool gdiplusReady() {
    static bool ok = [] {
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

// PNG/JPEG 原始字节 -> BGRA8 像素（失败返回空）
std::vector<uint8_t> DecodeImagePixels(const uint8_t* data, size_t size, int& w, int& h) {
    w = h = 0;
    if (!gdiplusReady() || !data || size == 0) return {};

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return {};
    {
        void* p = GlobalLock(hg);
        if (p) { memcpy(p, data, size); GlobalUnlock(hg); }
    }
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK) { GlobalFree(hg); return {}; }
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return {}; }

    w = bmp->GetWidth();
    h = bmp->GetHeight();
    std::vector<uint8_t> px;
    if (w > 0 && h > 0) {
        Gdiplus::Rect r(0, 0, w, h);
        Gdiplus::BitmapData bd;
        if (bmp->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
            px.resize(static_cast<size_t>(w) * h * 4);
            const uint8_t* src = static_cast<const uint8_t*>(bd.Scan0);
            uint8_t* dst = px.data();
            for (int y = 0; y < h; y++) {
                memcpy(dst + static_cast<size_t>(y) * w * 4,
                       src + static_cast<size_t>(y) * bd.Stride, static_cast<size_t>(w) * 4);
            }
            bmp->UnlockBits(&bd);
        }
    }
    delete bmp;
    return px;
}

// UTF-8 → 码点迭代
struct Utf8Iter {
    const std::string& s;
    size_t i = 0;
    uint32_t cp = 0;
    bool next() {
        if (i >= s.size()) return false;
        uint8_t b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80) { cp = b0; i++; return true; }
        int n = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t c = b0 & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); k++)
            c = (c << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        cp = c;
        i += n;
        return true;
    }
};

// 颜色预乘（着色器输出为预乘 alpha，图集/模板本身已预乘）
void Premult(uint32_t& c) {
    uint32_t a = (c >> 24) & 0xFF;
    if (a == 255) return;
    if (a == 0) { c = 0; return; }
    uint32_t r = (((c >> 16) & 0xFF) * a) / 255;
    uint32_t g = (((c >> 8) & 0xFF) * a) / 255;
    uint32_t b = ((c & 0xFF) * a) / 255;
    c = (a << 24) | (r << 16) | (g << 8) | b;
}

// ---------- 数据 ----------
// 顶点：pos(2×float) + color(4×u8，字节序 RGBA = 0xAABBGGRR) + uv(2×float)
struct GLVertex {
    float x, y;
    uint32_t color;
    float u, v;
};

struct Batch {
    uint32_t first = 0;  // 顶点起始（6 顶点/四边形）
    uint32_t count = 0;  // 四边形数
    int sx = 0, sy = 0, sw = 0, sh = 0;
    GLuint tex = 0;      // 采样纹理（0 = 未初始化；白色 1×1 用于纯色）
};

struct Glyph {
    uint16_t cell = 0;
    uint8_t advW = 0;
};

struct Atlas {
    GLuint tex = 0;
    int sizePx = 1024;
    int cellW = 0, cellH = 0;
    int cols = 0;
    int next = 1;             // 格子 0 保留给纯色用（见下）
    int ascent = 0, descent = 0;
    float size = 0;
    int weight = 400;
    bool cjk = false;
    bool italic = false;
    std::string face;
    std::unordered_map<uint32_t, Glyph> glyphs;
};

struct RoundTex {
    GLuint tex = 0;
    int t = 0;         // 模板边长（4r）
    int corner = 0;    // 角区边长（2r+1）
};

// ---------- emoji 簇检测（与 D3D11 后端一致） ----------
bool IsEmojiCp(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;
    if (cp >= 0x2190 && cp <= 0x21FF) return true;
    if (cp >= 0x2300 && cp <= 0x27BF) return true;
    if (cp >= 0x2900 && cp <= 0x297F) return true;
    if (cp >= 0x2B00 && cp <= 0x2BFF) return true;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
    if (cp == 0x200D) return true;
    if (cp == 0x20E3) return true;
    return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
           cp == 0x2122 || cp == 0x2139 || cp == 0x3030 || cp == 0x303D ||
           cp == 0x3297 || cp == 0x3299;
}

size_t EmojiClusterLen(const uint32_t* cps, size_t n) {
    uint32_t first = cps[0];
    if ((first == '#' || first == '*' || (first >= '0' && first <= '9')) && n > 1) {
        if (cps[1] == 0xFE0F && n > 2 && cps[2] == 0x20E3) return 3;
        if (cps[1] == 0x20E3) return 2;
    }
    if (!IsEmojiCp(first)) return 1;
    size_t j = 1;
    int ris = (first >= 0x1F1E6 && first <= 0x1F1FF) ? 1 : 0;
    while (j < n) {
        uint32_t c = cps[j];
        if (c == 0x200D) {
            if (j + 1 >= n) break;
            j += 2;
            continue;
        }
        if (c == 0xFE0F || c == 0xFE0E || c == 0x20E3) { j++; continue; }
        if (c >= 0x1F3FB && c <= 0x1F3FF) { j++; continue; }
        if (c >= 0x1F1E6 && c <= 0x1F1FF) {
            if (ris == 1) { j++; ris = 2; continue; }
            break;
        }
        break;
    }
    return j;
}

uint64_t EmojiClusterKey(const uint32_t* cps, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        uint64_t c = cps[i];
        for (int b = 0; b < 4; b++) {
            h ^= (c & 0xFF);
            h *= 1099511628211ull;
            c >>= 8;
        }
    }
    return h;
}

// ---------- 彩色字形（FreeType COLR 合成） ----------
struct ColorGlyph {
    uint16_t cell = 0;
    uint16_t advW = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    uint16_t baseline = 0;
};

struct ColorAtlas {
    GLuint tex = 0;
    int sizePx = 1024;
    int cellW = 0, cellH = 0;
    int cols = 0;
    int next = 0;
    float size = 0;
    std::unordered_map<uint64_t, ColorGlyph> glyphs;
};

constexpr size_t kMaxVerts = 100000; // 2MB 动态缓冲上限（约 16.6k 四边形）

// ---------- 纹理工具 ----------
GLuint CreateTexture2D(int w, int h, const void* data, bool linear, bool rgba = false) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // 解码图片走 GDI+：内存为 BGRA（PixelFormat32bppARGB）；原始像素直传走 RGBA 字节序
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 rgba ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, data);
    return t;
}

void UploadTexSub(GLuint tex, int x, int y, int w, int h, const void* data) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, data);
}

} // namespace

struct BinUIGLGdi::Impl {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC ctx = nullptr;
    int winW = 0, winH = 0;      // 窗口客户区尺寸（离屏/绘制/呈现目标）
    bool vsync = true;
    bool ready = false;
    bool transparent = false; // 分层窗口（UpdateLayeredWindow）

    // 分层窗口呈现缓冲（32bit BGRA premultiplied）
    HBITMAP layeredDib = nullptr;
    HDC layeredDC = nullptr;
    void* layeredBits = nullptr;
    int layeredW = 0, layeredH = 0;

    GLuint program = 0;
    GLint uniView = -1;
    GLuint vao = 0, vb = 0;
    GLuint whiteTex = 0; // 1×1 纯白（纯色四边形）
    // 持久离屏帧缓冲：局部重绘只更新脏区，present 时整帧 blit 到默认帧缓冲。
    // 4x MSAA 渲染缓冲（msFbo）为绘制目标，present 时解析（resolve）到单采样 offTex，
    // 之后 offTex 用于整帧 blit / 透明窗口读回 —— 线条与圆角边缘获得抗锯齿。
    GLuint offTex = 0, offFbo = 0;
    GLuint msRbo = 0, msFbo = 0;
    int    msSamples = 4;

    // 场景
    std::vector<GLVertex> verts;
    std::vector<Batch> batches;
    Batch* curBatch = nullptr;
    int scissor[4] = { 0, 0, 0, 0 };
    int scissorDepth = 0;
    GLuint curTex = 0;

    // 字形图集（按 size/weight/字体族）
    std::unordered_map<uint64_t, Atlas*> atlases;
    std::string asciiFont = "Segoe UI";
    static constexpr const char* cjkFont = "Microsoft YaHei";
    static constexpr const char* emojiFont = "Segoe UI Emoji";

    binui_font::FontEngine font;
    bool fontReady = false;

    std::unordered_map<uint32_t, ColorAtlas*> colorAtlases;
    std::unordered_map<int, RoundTex> rounds;

    std::unordered_map<uint16_t, GLuint> images;
    std::unordered_map<uint16_t, uint32_t> imageGen; // 换图 generation：不匹配时重新上传

    // GDI 光栅化 DC
    HDC rasterDC = nullptr;
    HBITMAP rasterBmp = nullptr;
    void* rasterBits = nullptr;
    int rasterW = 0, rasterH = 0;

    ~Impl() {
        shutdown();
        if (rasterBmp) DeleteObject(rasterBmp);
        if (rasterDC) DeleteDC(rasterDC);
    }

    // ---------- 上下文/程序/状态 ----------
    static int pickPixelFormat(HDC hdc) {
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;
        int pf = ChoosePixelFormat(hdc, &pfd);
        if (pf > 0) return pf;
        // 回退：遍历格式表选 32 位双缓冲 RGBA
        int best = 0;
        for (int i = 1; i <= 256; i++) {
            PIXELFORMATDESCRIPTOR d = {};
            if (!DescribePixelFormat(hdc, i, sizeof(d), &d)) continue;
            if (!(d.dwFlags & PFD_DRAW_TO_WINDOW)) continue;
            if (!(d.dwFlags & PFD_SUPPORT_OPENGL)) continue;
            if (!(d.dwFlags & PFD_DOUBLEBUFFER)) continue;
            if (d.iPixelType != PFD_TYPE_RGBA) continue;
            best = i;
            if (d.cColorBits >= 32) break;
        }
        return best;
    }

    bool createContext() {
        hdc = GetDC(hwnd);
        if (!hdc) {
            std::fprintf(stderr, "[gl] GetDC failed\n");
            return false;
        }
        int pf = pickPixelFormat(hdc);
        PIXELFORMATDESCRIPTOR pfd = {};
        if (pf <= 0 || !DescribePixelFormat(hdc, pf, sizeof(pfd), &pfd) ||
            !SetPixelFormat(hdc, pf, &pfd)) {
            std::fprintf(stderr, "[gl] SetPixelFormat failed\n");
            ReleaseDC(hwnd, hdc);
            hdc = nullptr;
            return false;
        }

        // 先建 1.x 上下文以取得 wglCreateContextAttribsARB，再切到 3.3 core
        HGLRC legacy = wglCreateContext(hdc);
        if (!legacy || !wglMakeCurrent(hdc, legacy)) {
            std::fprintf(stderr, "[gl] legacy context failed\n");
            if (legacy) wglDeleteContext(legacy);
            ReleaseDC(hwnd, hdc);
            hdc = nullptr;
            return false;
        }
        wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(
            GetGLProc("wglCreateContextAttribsARB"));
        if (!wglCreateContextAttribsARB) {
            std::fprintf(stderr, "[gl] wglCreateContextAttribsARB unavailable\n");
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(legacy);
            ReleaseDC(hwnd, hdc);
            hdc = nullptr;
            return false;
        }
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        ctx = wglCreateContextAttribsARB(hdc, nullptr, attribs);
        if (!ctx) {
            std::fprintf(stderr, "[gl] 3.3 core context failed\n");
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(legacy);
            ReleaseDC(hwnd, hdc);
            hdc = nullptr;
            return false;
        }
        wglMakeCurrent(hdc, ctx);
        wglDeleteContext(legacy);
        return true;
    }

    static GLuint compileShader(GLenum type, const char* src) {
        GLuint s = glCreateShaderP(type);
        const GLchar* psrc = src;
        glShaderSourceP(s, 1, &psrc, nullptr);
        glCompileShaderP(s);
        GLint ok = GL_FALSE;
        glGetShaderivP(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            GLsizei len = 0;
            glGetShaderInfoLogP(s, sizeof(log) - 1, &len, log);
            std::fprintf(stderr, "[gl] shader error: %s\n", log);
            glDeleteShaderP(s);
            return 0;
        }
        return s;
    }

    bool createProgram() {
        const char* vs =
            "#version 330 core\n"
            "layout(location=0) in vec2 pos;\n"
            "layout(location=1) in vec4 col;\n"
            "layout(location=2) in vec2 uv;\n"
            "uniform vec2 viewSize;\n"
            "out vec4 vcol;\n"
            "out vec2 vuv;\n"
            "void main() {\n"
            "  gl_Position = vec4(pos.x / viewSize.x * 2.0 - 1.0,\n"
            "                     1.0 - pos.y / viewSize.y * 2.0, 0.0, 1.0);\n"
            "  vcol = col;\n"
            "  vuv = uv;\n"
            "}\n";
        const char* fs =
            "#version 330 core\n"
            "in vec4 vcol;\n"
            "in vec2 vuv;\n"
            "uniform sampler2D tex;\n"
            "out vec4 frag;\n"
            "void main() {\n"
            "  vec4 t = texture(tex, vuv);\n"
            "  frag = vec4(t.rgb * vcol.rgb, t.a * vcol.a);\n"
            "}\n";
        GLuint s1 = compileShader(GL_VERTEX_SHADER, vs);
        GLuint s2 = compileShader(GL_FRAGMENT_SHADER, fs);
        if (!s1 || !s2) {
            if (s1) glDeleteShaderP(s1);
            if (s2) glDeleteShaderP(s2);
            return false;
        }
        program = glCreateProgramP();
        glAttachShaderP(program, s1);
        glAttachShaderP(program, s2);
        glLinkProgramP(program);
        glDeleteShaderP(s1);
        glDeleteShaderP(s2);
        GLint ok = GL_FALSE;
        glGetProgramivP(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            GLsizei len = 0;
            glGetProgramInfoLogP(program, sizeof(log) - 1, &len, log);
            std::fprintf(stderr, "[gl] program link error: %s\n", log);
            return false;
        }
        uniView = glGetUniformLocationP(program, "viewSize");
        glUseProgramP(program);
        glUniform1iP(glGetUniformLocationP(program, "tex"), 0);
        return true;
    }

    bool createState() {
        glGenVertexArraysP(1, &vao);
        glBindVertexArrayP(vao);
        glGenBuffersP(1, &vb);
        glBindBufferP(GL_ARRAY_BUFFER, vb);
        glBufferDataP(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kMaxVerts * sizeof(GLVertex)),
                      nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointerP(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void*)0);
        glEnableVertexAttribArrayP(0);
        glVertexAttribPointerP(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVertex), (void*)8);
        glEnableVertexAttribArrayP(1);
        glVertexAttribPointerP(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void*)12);
        glEnableVertexAttribArrayP(2);

        // 预乘 alpha 混合（与 D3D11 一致：src=ONE, dst=INV_SRC_ALPHA）
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_SCISSOR_TEST);

        // 1×1 纯白纹理（纯色四边形）
        const uint8_t white[4] = { 255, 255, 255, 255 };
        whiteTex = CreateTexture2D(1, 1, white, false);
        return whiteTex != 0;
    }

    // ---------- GDI 光栅化 ----------
    bool ensureRasterDC(int w, int h) {
        if (rasterDC && rasterW == w && rasterH == h) return true;
        if (rasterBmp) DeleteObject(rasterBmp);
        if (rasterDC) DeleteDC(rasterDC);
        rasterDC = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        rasterBmp = CreateDIBSection(rasterDC, &bi, DIB_RGB_COLORS, &rasterBits, nullptr, 0);
        if (!rasterBmp) return false;
        SelectObject(rasterDC, rasterBmp);
        rasterW = w;
        rasterH = h;
        return true;
    }

    Atlas* getAtlas(float size, int weight, bool cjk, bool emoji = false, bool italic = false) {
        // 斜体与粗体一样是"另一张图集"：合成倾斜发生在光栅化阶段，不能共用缓存
        uint64_t key = (static_cast<uint64_t>(std::lround(size)) << 32) |
                       (static_cast<uint64_t>((weight & 0x7FFF) | (italic ? 0x8000 : 0)) << 16) |
                       (cjk ? 1u : 0u) | (emoji ? 2u : 0u);
        auto it = atlases.find(key);
        if (it != atlases.end()) return it->second;

        Atlas* a = new Atlas();
        a->size = size;
        a->weight = weight;
        a->cjk = cjk;
        a->italic = italic;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH;
        a->cols = a->sizePx / a->cellW;
        a->face = emoji ? emojiFont : (cjk ? cjkFont : asciiFont);
        std::vector<uint8_t> zeros(static_cast<size_t>(a->sizePx) * a->sizePx * 4, 0);
        a->tex = CreateTexture2D(a->sizePx, a->sizePx, zeros.data(), false);
        if (!a->tex) {
            delete a;
            return nullptr;
        }
        binui_font::FontMetrics fm = font.metrics(a->face, size);
        a->ascent = fm.ascent;
        a->descent = fm.descent;

        atlases[key] = a;
        return a;
    }

    // 把单个码点光栅化进图集格子（FreeType 灰度字形 → 白字预乘 alpha）
    void rasterGlyph(Atlas* a, uint32_t cp) {
        int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) return;
        binui_font::GrayGlyph g;
        // 粗体：atlas key 里带 weight，粗体是独立的一张图集；这里按新签名让 FreeType
        // 合成加粗（FT_GlyphSlot_Embolden），否则 font-weight:700 只是换个空图集重复光栅化
        bool ok = font.renderGlyph(a->face, a->size, cp, a->weight >= 600, a->italic, g);
        if (!ok || !g.alpha || g.height <= 0) {
            // 空白字形（如空格）：无像素但有有效 advance —— 仍占一格并缓存，
            // 否则 glyphFor 返回 null，测宽/绘制都不推进，单词会挤在一起
            if (ok && g.advance > 0) {
                Glyph gl;
                gl.cell = static_cast<uint16_t>(a->next++); // 格子保持全零纹理（不可见）
                gl.advW = static_cast<uint8_t>(std::min(255, std::max(0, g.advance)));
                a->glyphs[cp] = gl;
            }
            return;
        }

        ensureRasterDC(a->cellW, a->cellH);
        memset(rasterBits, 0, static_cast<size_t>(a->cellW) * a->cellH * 4);
        int ox = 1 + g.left;
        int oy = (a->ascent + 1) - g.top;
        uint8_t* dst = static_cast<uint8_t*>(rasterBits);
        for (int y = 0; y < g.height; y++) {
            int py = oy + y;
            if (py < 0 || py >= a->cellH) continue;
            const uint8_t* src = g.alpha + static_cast<size_t>(y) * g.pitch;
            uint8_t* row = dst + static_cast<size_t>(py) * a->cellW * 4;
            for (int x = 0; x < g.width; x++) {
                int px = ox + x;
                if (px < 0 || px >= a->cellW) continue;
                uint8_t al = src[x];
                if (al == 0) continue;
                uint8_t* p = row + static_cast<size_t>(px) * 4;
                p[0] = p[1] = p[2] = p[3] = al;
            }
        }

        int cell = a->next++;
        int cx = (cell % a->cols) * a->cellW;
        int cy = (cell / a->cols) * a->cellH;
        UploadTexSub(a->tex, cx, cy, a->cellW, a->cellH, rasterBits);

        Glyph gl;
        gl.cell = static_cast<uint16_t>(cell);
        // advance 用 HarfBuzz/FreeType 的真实值（以前 +1，每个字形多占 1px：
        // 文字比真实排版宽、组合符号被推离基字、换行宽度与实际绘制不符）
        gl.advW = static_cast<uint8_t>(std::min(255, std::max(0, g.advance)));
        a->glyphs[cp] = gl;
    }

    const Glyph* glyphFor(Atlas* a, uint32_t cp) {
        auto it = a->glyphs.find(cp);
        if (it != a->glyphs.end()) return &it->second;
        rasterGlyph(a, cp);
        it = a->glyphs.find(cp);
        return it != a->glyphs.end() ? &it->second : nullptr;
    }

    // ---------- 彩色字形（FreeType COLR 合成） ----------
    bool rasterColorCluster(ColorAtlas* a, uint64_t key, const uint32_t* cps, size_t n) {
        int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) return false;

        binui_font::ColorGlyph cg;
        if (!font.renderColorCluster(emojiFont, a->size, cps, n, cg)) return false;
        if (cg.width <= 0 || cg.height <= 0 || !cg.bgra) return false;

        ensureRasterDC(a->cellW, a->cellH);
        memset(rasterBits, 0, static_cast<size_t>(a->cellW) * a->cellH * 4);
        int cw = std::min(cg.width, a->cellW);
        int ch = std::min(cg.height, a->cellH);
        uint8_t* dst = static_cast<uint8_t*>(rasterBits);
        for (int y = 0; y < ch; y++)
            memcpy(dst + static_cast<size_t>(y) * a->cellW * 4,
                   cg.bgra + static_cast<size_t>(y) * cg.width * 4,
                   static_cast<size_t>(cw) * 4);

        int cell = a->next++;
        int cx = (cell % a->cols) * a->cellW;
        int cy = (cell / a->cols) * a->cellH;
        UploadTexSub(a->tex, cx, cy, a->cellW, a->cellH, rasterBits);

        ColorGlyph g;
        g.cell = static_cast<uint16_t>(cell);
        g.advW = static_cast<uint16_t>(std::min(4095, cg.advance + 1));
        g.w = static_cast<uint16_t>(cw);
        g.h = static_cast<uint16_t>(ch);
        g.baseline = static_cast<uint16_t>(std::min(4095, cg.baseline));
        a->glyphs[key] = g;
        return true;
    }

    ColorAtlas* getColorAtlas(float size) {
        uint32_t key = static_cast<uint32_t>(std::lround(size));
        auto it = colorAtlases.find(key);
        if (it != colorAtlases.end()) return it->second;

        ColorAtlas* a = new ColorAtlas();
        a->size = size;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH * 4;
        a->cols = a->sizePx / a->cellW;
        if (a->cols < 1) { delete a; return nullptr; }
        std::vector<uint8_t> zeros(static_cast<size_t>(a->sizePx) * a->sizePx * 4, 0);
        a->tex = CreateTexture2D(a->sizePx, a->sizePx, zeros.data(), false);
        if (!a->tex) {
            delete a;
            return nullptr;
        }
        colorAtlases[key] = a;
        return a;
    }

    const ColorGlyph* colorGlyphFor(ColorAtlas* a, const uint32_t* cps, size_t n) {
        uint64_t key = EmojiClusterKey(cps, n);
        auto it = a->glyphs.find(key);
        if (it != a->glyphs.end()) return &it->second;
        rasterColorCluster(a, key, cps, n);
        it = a->glyphs.find(key);
        return it != a->glyphs.end() ? &it->second : nullptr;
    }

    struct ClusterMetric {
        size_t len = 1;
        float adv = 0;
        bool color = false;
    };

    ClusterMetric clusterMetric(float size, int weight, const uint32_t* cps, size_t n,
                                bool italic = false) {
        ClusterMetric m;
        m.len = EmojiClusterLen(cps, n);
        float fallback = std::ceil(size * 0.6f);
        if (!(m.len > 1 || IsEmojiCp(cps[0]))) {
            Atlas* a = getAtlas(size, weight, cps[0] >= 0x80, false, italic);
            const Glyph* g = a ? glyphFor(a, cps[0]) : nullptr;
            m.adv = g ? static_cast<float>(g->advW) : fallback;
            return m;
        }
        if (fontReady) {
            ColorAtlas* ca = getColorAtlas(size);
            const ColorGlyph* g = ca ? colorGlyphFor(ca, cps, m.len) : nullptr;
            if (g) {
                m.adv = static_cast<float>(g->advW);
                m.color = true;
                return m;
            }
        }
        for (size_t i = 0; i < m.len; i++) {
            Atlas* a = getAtlas(size, weight, cps[i] >= 0x80, IsEmojiCp(cps[i]), italic);
            const Glyph* g = a ? glyphFor(a, cps[i]) : nullptr;
            m.adv += g ? static_cast<float>(g->advW) : fallback;
        }
        return m;
    }

    // 圆角模板（九宫格）：4r×4r 白色圆角矩形（实心/2px 描边），预乘 alpha
    RoundTex& roundFor(int radius, bool stroke) {
        int r = std::max(1, radius);
        int key = stroke ? -r : r;
        auto it = rounds.find(key);
        if (it != rounds.end()) return it->second;

        int t = std::max(8, r * 4);
        ensureRasterDC(t, t);
        memset(rasterBits, 0, static_cast<size_t>(t) * t * 4);

        // 解析式逐像素覆盖率（1px 抗锯齿边缘），替代 GDI RoundRect（无 AA，转角锯齿明显）。
        // 模板几何与 GDI 一致：圆角矩形外框 [1, t-1]，角半轴 r；填充 = 内部，描边 = 2px 带。
        {
            uint32_t* px = static_cast<uint32_t*>(rasterBits);
            const float half = (t - 2.0f) / 2.0f;
            const float inset = half - static_cast<float>(r); // 直线段起始距中心
            const float c0 = static_cast<float>(t) / 2.0f;
            for (int i = 0; i < t; i++) {
                for (int j = 0; j < t; j++) {
                    float fx = fabsf(j + 0.5f - c0) - inset;
                    float fy = fabsf(i + 0.5f - c0) - inset;
                    float dx = fx > 0 ? fx : 0.0f;
                    float dy = fy > 0 ? fy : 0.0f;
                    float dist = sqrtf(dx * dx + dy * dy) - static_cast<float>(r);
                    float cov = stroke ? 1.0f - fabsf(dist) : 0.5f - dist;
                    cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
                    uint8_t lum = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                    uint32_t v = (static_cast<uint32_t>(lum) << 24) |
                                 (static_cast<uint32_t>(lum) << 16) |
                                 (static_cast<uint32_t>(lum) << 8) | lum;
                    px[i * t + j] = v;
                }
            }
        }

        RoundTex rt;
        rt.t = t;
        rt.corner = r * 2 + 1;
        rt.tex = CreateTexture2D(t, t, rasterBits, true);
        return rounds.emplace(key, rt).first->second;
    }

    bool uploadImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
        auto it = images.find(index);
        auto git = imageGen.find(index);
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != 0;

        int w = 0, h = 0;
        std::vector<uint8_t> px = DecodeImagePixels(data, size, w, h);
        if (px.empty()) {
            images[index] = 0;
            imageGen[index] = generation;
            return false;
        }
        if (it != images.end() && it->second) glDeleteTextures(1, &it->second);
        images[index] = CreateTexture2D(w, h, px.data(), true);
        imageGen[index] = generation;
        return images[index] != 0;
    }

    // 原始 RGBA 像素直传纹理（无解码；RGBA 字节序）
    bool uploadImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
        auto it = images.find(index);
        auto git = imageGen.find(index);
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != 0;
        if (w <= 0 || h <= 0 || !rgba) return false;
        if (it != images.end() && it->second) glDeleteTextures(1, &it->second);
        images[index] = CreateTexture2D(w, h, rgba, true, true);
        imageGen[index] = generation;
        return images[index] != 0;
    }

    // 持久离屏帧缓冲（大小 = 窗口客户区）
    bool createOffscreen() {
        if (offTex) { glDeleteTextures(1, &offTex); offTex = 0; }
        if (offFbo) { glDeleteFramebuffersP(1, &offFbo); offFbo = 0; }
        if (msRbo) { glDeleteRenderbuffersP(1, &msRbo); msRbo = 0; }
        if (msFbo) { glDeleteFramebuffersP(1, &msFbo); msFbo = 0; }
        offTex = CreateTexture2D(winW, winH, nullptr, true);
        if (!offTex) return false;
        glGenFramebuffersP(1, &offFbo);
        glBindFramebufferP(GL_FRAMEBUFFER, offFbo);
        glFramebufferTexture2DP(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, offTex, 0);
        if (glCheckFramebufferStatusP(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[gl] offscreen FBO incomplete\n");
            glBindFramebufferP(GL_FRAMEBUFFER, 0);
            return false;
        }
        // MSAA 绘制目标：渲染缓冲（RGBA8 x msSamples），present 时解析到 offTex
        glGenRenderbuffersP(1, &msRbo);
        glBindRenderbufferP(GL_RENDERBUFFER, msRbo);
        glRenderbufferStorageMultisampleP(GL_RENDERBUFFER, msSamples, GL_RGBA8, winW, winH);
        glGenFramebuffersP(1, &msFbo);
        glBindFramebufferP(GL_FRAMEBUFFER, msFbo);
        glFramebufferRenderbufferP(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msRbo);
        if (glCheckFramebufferStatusP(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[gl] MSAA FBO incomplete\n");
            glBindFramebufferP(GL_FRAMEBUFFER, 0);
            return false;
        }
        if (transparent) glClearColor(0, 0, 0, 0);
        else glClearColor(1, 1, 1, 1);
        glScissor(0, 0, winW, winH); // 复位全屏 scissor：初始清屏不受上次批次裁剪影响
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebufferP(GL_FRAMEBUFFER, 0);
        return true;
    }

    // ---------- 场景 ----------
    void beginBatch() {
        if (curBatch &&
            curBatch->sx == scissor[0] && curBatch->sy == scissor[1] &&
            curBatch->sw == scissor[2] - scissor[0] && curBatch->sh == scissor[3] - scissor[1] &&
            curBatch->tex == curTex)
            return;
        Batch b;
        b.first = static_cast<uint32_t>(verts.size());
        b.count = 0;
        b.sx = scissor[0];
        b.sy = scissor[1];
        b.sw = scissor[2] - scissor[0];
        b.sh = scissor[3] - scissor[1];
        b.tex = curTex;
        batches.push_back(b);
        curBatch = &batches.back();
    }

    void emitQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, uint32_t color) {
        if (verts.size() + 6 > kMaxVerts) return;
        beginBatch();
        // 0xAARRGGBB → 0xAABBGGRR：UNSIGNED_BYTE 属性按内存字节序读（byte0=R）
        uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        GLVertex v[6] = {
            { x0, y0, sw, u0, v0 }, { x1, y0, sw, u1, v0 }, { x0, y1, sw, u0, v1 },
            { x0, y1, sw, u0, v1 }, { x1, y0, sw, u1, v0 }, { x1, y1, sw, u1, v1 },
        };
        verts.insert(verts.end(), v, v + 6);
        curBatch->count++;
    }

    // 任意旋转四边形（三角形带，白纹理；UV 无意义）
    void emitQuadRot(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3, uint32_t color) {
        if (verts.size() + 6 > kMaxVerts) return;
        beginBatch();
        uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        GLVertex v[6] = {
            { x0, y0, sw, 0, 0 }, { x1, y1, sw, 1, 0 }, { x2, y2, sw, 1, 1 },
            { x2, y2, sw, 1, 1 }, { x3, y3, sw, 0, 1 }, { x0, y0, sw, 0, 0 },
        };
        verts.insert(verts.end(), v, v + 6);
        curBatch->count++;
    }

    void flushAll() {
        if (verts.empty() || !vb) return;
        glBindBufferP(GL_ARRAY_BUFFER, vb);
        glBufferDataP(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(GLVertex)),
                      verts.data(), GL_DYNAMIC_DRAW);
        for (const auto& b : batches) {
            if (b.count == 0) continue;
            int l = b.sx, t = b.sy, r = b.sx + b.sw, bot = b.sy + b.sh;
            if (r <= l || bot <= t) continue;
            // GL scissor 原点在左下：换算为从底部计
            glScissor(l, winH - bot, r - l, bot - t);
            glBindTexture(GL_TEXTURE_2D, b.tex);
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b.first), static_cast<GLsizei>(b.count * 6));
        }
    }

    void shutdown() {
        if (ctx) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(ctx);
            ctx = nullptr;
        }
        if (hdc) { ReleaseDC(hwnd, hdc); hdc = nullptr; }
        for (auto& kv : atlases) {
            if (kv.second->tex) glDeleteTextures(1, &kv.second->tex);
            delete kv.second;
        }
        atlases.clear();
        for (auto& kv : colorAtlases) {
            if (kv.second->tex) glDeleteTextures(1, &kv.second->tex);
            delete kv.second;
        }
        colorAtlases.clear();
        for (auto& kv : rounds)
            if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
        rounds.clear();
        for (auto& kv : images)
            if (kv.second) glDeleteTextures(1, &kv.second);
        images.clear();
        if (whiteTex) { glDeleteTextures(1, &whiteTex); whiteTex = 0; }
        if (offTex) { glDeleteTextures(1, &offTex); offTex = 0; }
        if (offFbo) { glDeleteFramebuffersP(1, &offFbo); offFbo = 0; }
        if (msRbo) { glDeleteRenderbuffersP(1, &msRbo); msRbo = 0; }
        if (msFbo) { glDeleteFramebuffersP(1, &msFbo); msFbo = 0; }
        if (program) { glDeleteProgramP(program); program = 0; }
        if (vb) { glDeleteBuffersP(1, &vb); vb = 0; }
        if (vao) { glDeleteVertexArraysP(1, &vao); vao = 0; }
        if (layeredDib) { DeleteObject(layeredDib); layeredDib = nullptr; }
        if (layeredDC) { DeleteDC(layeredDC); layeredDC = nullptr; }
        ready = false;
    }
};

// ============================================================
// BinUIGLGdi
// ============================================================
BinUIGLGdi::BinUIGLGdi() : impl_(new Impl()) {}
BinUIGLGdi::~BinUIGLGdi() { delete impl_; }

bool BinUIGLGdi::init(void* hwnd, int width, int height, bool vsync) {
    impl_->hwnd = static_cast<HWND>(hwnd);
    impl_->winW = width;
    impl_->winH = height;
    impl_->vsync = vsync;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = width;
    impl_->scissor[3] = height;

    if (!impl_->createContext()) { impl_->shutdown(); return false; }
    if (!LoadGL()) { impl_->shutdown(); return false; }
    if (!impl_->createProgram()) { impl_->shutdown(); return false; }
    if (!impl_->createState()) { impl_->shutdown(); return false; }
    if (!impl_->createOffscreen()) { impl_->shutdown(); return false; }
    glViewport(0, 0, width, height);
    glUniform2fP(impl_->uniView, static_cast<float>(width), static_cast<float>(height));
    impl_->fontReady = impl_->font.init();
    binui_font::registerSystemFonts(impl_->font);
    wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXT>(
        GetGLProc("wglSwapIntervalEXT"));
    if (impl_->vsync && wglSwapIntervalEXT) wglSwapIntervalEXT(1);
    impl_->ready = true;
    return true;
}

void BinUIGLGdi::shutdown() { impl_->shutdown(); }
bool BinUIGLGdi::isReady() const { return impl_->ready; }
int BinUIGLGdi::width() const { return impl_->winW; }
int BinUIGLGdi::height() const { return impl_->winH; }
void BinUIGLGdi::setVsync(bool on) {
    impl_->vsync = on;
    if (impl_->ready && wglSwapIntervalEXT) wglSwapIntervalEXT(on ? 1 : 0);
}

void BinUIGLGdi::resize(int width, int height) {
    if (!impl_->ready || (width == impl_->winW && height == impl_->winH)) return;
    impl_->winW = width;
    impl_->winH = height;
    impl_->createOffscreen();
    glViewport(0, 0, width, height);
    glUniform2fP(impl_->uniView, static_cast<float>(width), static_cast<float>(height));
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = width;
    impl_->scissor[3] = height;
}

void BinUIGLGdi::beginFrame(const void* clipRect) {
    if (!impl_->ready) return;
    impl_->verts.clear();
    impl_->batches.clear();
    impl_->curBatch = nullptr;
    impl_->scissorDepth = 0;
    impl_->curTex = impl_->whiteTex;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = impl_->winW;
    impl_->scissor[3] = impl_->winH;

    // 绘制目标 = 4x MSAA 离屏帧缓冲（present 时解析到 offTex）
    glBindFramebufferP(GL_FRAMEBUFFER, impl_->msFbo);
    glViewport(0, 0, impl_->winW, impl_->winH);

    // 清空本次绘制区到画布底色（透明窗口 = 全透明；否则白色）：局部重绘的基准底色，
    // RGBA 画布/半透明元素每次只与底色合成一次，持久离屏不会因重复重绘而叠深
    if (clipRect) {
        const int* c = static_cast<const int*>(clipRect);
        int l = c[0], t = c[1], r = c[2], b = c[3];
        if (r > l && b > t && l >= 0 && t >= 0 && r <= impl_->winW && b <= impl_->winH) {
            glScissor(l, impl_->winH - b, r - l, b - t); // GL 原点在左下
            if (impl_->transparent) glClearColor(0, 0, 0, 0);
            else glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glScissor(0, 0, impl_->winW, impl_->winH);
}

void BinUIGLGdi::present() {
    if (!impl_->ready) return;
    impl_->flushAll(); // 画到 MSAA 帧缓冲

    // MSAA 解析：msFbo -> offTex（单采样离屏纹理，供整帧 blit / 透明窗口读回）。
    // 注意：glBlitFramebuffer 受 scissor 裁剪——必须先复位全屏 scissor，
    // 否则只解析最后一批的绘制区，其余离屏区域保持未初始化（黑）。
    glBindFramebufferP(GL_READ_FRAMEBUFFER, impl_->msFbo);
    glBindFramebufferP(GL_DRAW_FRAMEBUFFER, impl_->offFbo);
    glScissor(0, 0, impl_->winW, impl_->winH);
    glBlitFramebufferP(0, 0, impl_->winW, impl_->winH,
                       0, 0, impl_->winW, impl_->winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (impl_->transparent) {
        // 分层窗口：离屏 premultiplied 像素读回 → UpdateLayeredWindow 合成到桌面
        int w = impl_->winW, h = impl_->winH;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        glBindFramebufferP(GL_FRAMEBUFFER, impl_->offFbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebufferP(GL_FRAMEBUFFER, 0);
        // GL 行 0 = 屏幕底部：翻转为顶层顺序（DIB top-down）
        {
            std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
            for (int y = 0; y < h / 2; y++) {
                uint8_t* a = px.data() + static_cast<size_t>(y) * w * 4;
                uint8_t* b = px.data() + static_cast<size_t>(h - 1 - y) * w * 4;
                memcpy(row.data(), a, row.size());
                memcpy(a, b, row.size());
                memcpy(b, row.data(), row.size());
            }
        }

        if (impl_->layeredDib == nullptr || impl_->layeredW != w || impl_->layeredH != h) {
            if (impl_->layeredDib) DeleteObject(impl_->layeredDib);
            if (impl_->layeredDC) DeleteDC(impl_->layeredDC);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h; // 顶层（top-down）
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            impl_->layeredDib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &impl_->layeredBits, nullptr, 0);
            impl_->layeredDC = CreateCompatibleDC(nullptr);
            impl_->layeredW = w;
            impl_->layeredH = h;
        }
        if (impl_->layeredDib && impl_->layeredBits) {
            memcpy(impl_->layeredBits, px.data(), static_cast<size_t>(w) * h * 4);
            HDC screenDC = GetDC(nullptr);
            HGDIOBJ old = SelectObject(impl_->layeredDC, impl_->layeredDib);
            RECT wr;
            GetWindowRect(impl_->hwnd, &wr);
            POINT dst = { wr.left, wr.top };
            SIZE sz = { w, h };
            POINT src = { 0, 0 };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(impl_->hwnd, screenDC, &dst, &sz,
                                impl_->layeredDC, &src, 0, &bf, ULW_ALPHA);
            SelectObject(impl_->layeredDC, old);
            ReleaseDC(nullptr, screenDC);
        }
        return;
    }

    // 整帧 blit：离屏 → 默认帧缓冲（交换链 backbuffer）。
    // 离屏纹理由 GL 渲染（纹理第 0 行 = 屏幕底部），采样时上下翻转
    glBindFramebufferP(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, impl_->winW, impl_->winH);
    glScissor(0, 0, impl_->winW, impl_->winH);
    float wf = static_cast<float>(impl_->winW);
    float hf = static_cast<float>(impl_->winH);
    uint32_t white = 0xFFFFFFFF;
    GLVertex v[6] = {
        { 0, 0, white, 0, 1 }, { wf, 0, white, 1, 1 }, { 0, hf, white, 0, 0 },
        { 0, hf, white, 0, 0 }, { wf, 0, white, 1, 1 }, { wf, hf, white, 1, 0 },
    };
    glBindBufferP(GL_ARRAY_BUFFER, impl_->vb);
    glBufferDataP(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, impl_->offTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    SwapBuffers(impl_->hdc);
}

void BinUIGLGdi::presentViaGdi() {
    if (!impl_->ready) return;
    impl_->flushAll();
    glBindFramebufferP(GL_READ_FRAMEBUFFER, impl_->msFbo);
    glBindFramebufferP(GL_DRAW_FRAMEBUFFER, impl_->offFbo);
    glScissor(0, 0, impl_->winW, impl_->winH);
    glBlitFramebufferP(0, 0, impl_->winW, impl_->winH,
                       0, 0, impl_->winW, impl_->winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    int w = impl_->winW, h = impl_->winH;
    if (impl_->layeredDib == nullptr || impl_->layeredW != w || impl_->layeredH != h) {
        if (impl_->layeredDib) DeleteObject(impl_->layeredDib);
        if (impl_->layeredDC) DeleteDC(impl_->layeredDC);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // 顶层（top-down）
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        impl_->layeredDib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &impl_->layeredBits, nullptr, 0);
        impl_->layeredDC = CreateCompatibleDC(nullptr);
        impl_->layeredW = w;
        impl_->layeredH = h;
    }
    if (!impl_->layeredDib || !impl_->layeredBits) {
        glBindFramebufferP(GL_FRAMEBUFFER, 0);
        return;
    }
    glBindFramebufferP(GL_FRAMEBUFFER, impl_->offFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, static_cast<uint8_t*>(impl_->layeredBits));
    glBindFramebufferP(GL_FRAMEBUFFER, 0);
    size_t rowBytes = static_cast<size_t>(w) * 4;
    uint8_t* p = static_cast<uint8_t*>(impl_->layeredBits);
    std::vector<uint8_t> row(rowBytes);
    for (int y = 0; y < h / 2; y++) {
        uint8_t* a = p + static_cast<size_t>(y) * rowBytes;
        uint8_t* b = p + static_cast<size_t>(h - 1 - y) * rowBytes;
        memcpy(row.data(), a, rowBytes);
        memcpy(a, b, rowBytes);
        memcpy(b, row.data(), rowBytes);
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC wdc = GetDC(impl_->hwnd);
    StretchDIBits(wdc, 0, 0, w, h, 0, 0, w, h,
                  impl_->layeredBits, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(impl_->hwnd, wdc);
}

void BinUIGLGdi::setTransparent(bool t) {
    impl_->transparent = t;
}

// ---------- 原语 ----------
void BinUIGLGdi::fillRect(float x, float y, float w, float h, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    impl_->curTex = impl_->whiteTex;
    Premult(color);
    impl_->emitQuad(x, y, x + w, y + h, 0, 0, 1, 1, color);
}

void BinUIGLGdi::fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    if (!impl_->ready || width <= 0) return;
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float px = -dy / len * width * 0.5f;
    float py = dx / len * width * 0.5f;
    impl_->curTex = impl_->whiteTex;
    Premult(color);
    impl_->emitQuadRot(x0 + px, y0 + py, x1 + px, y1 + py,
                       x1 - px, y1 - py, x0 - px, y0 - py, color);
}

void BinUIGLGdi::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    int r = static_cast<int>(radius);
    r = std::max(0, std::min(r, static_cast<int>(std::min(w, h) / 2)));
    if (r <= 0) { fillRect(x, y, w, h, color); return; }

    RoundTex& rt = impl_->roundFor(r, false);
    if (!rt.tex) { fillRect(x, y, w, h, color); return; }

    float t = static_cast<float>(rt.t);
    float cu = static_cast<float>(rt.corner) / t;
    float strip0 = 0.5f;
    float strip1 = 0.5f + 1.0f / t;
    float c = static_cast<float>(rt.corner);

    Premult(color);
    impl_->curTex = rt.tex;
    auto quad = [&](float qx, float qy, float qw, float qh,
                    float u0, float v0, float u1, float v1) {
        impl_->emitQuad(qx, qy, qx + qw, qy + qh, u0, v0, u1, v1, color);
    };
    quad(x, y, c, c, 0, 0, cu, cu);
    quad(x + w - c, y, c, c, 1 - cu, 0, 1, cu);
    quad(x, y + h - c, c, c, 0, 1 - cu, cu, 1);
    quad(x + w - c, y + h - c, c, c, 1 - cu, 1 - cu, 1, 1);
    quad(x + c, y, w - 2 * c, c, strip0, 0, strip1, cu);
    quad(x + c, y + h - c, w - 2 * c, c, strip0, 1 - cu, strip1, 1);
    quad(x, y + c, c, h - 2 * c, 0, strip0, cu, strip1);
    quad(x + w - c, y + c, c, h - 2 * c, 1 - cu, strip0, 1, strip1);
    quad(x + c, y + c, w - 2 * c, h - 2 * c, strip0, strip0, strip1, strip1);
    impl_->curTex = impl_->whiteTex;
}

void BinUIGLGdi::strokeRounded(float x, float y, float w, float h, float radius,
                            float thickness, uint32_t color) {
    (void)thickness;
    if (!impl_->ready || w <= 0 || h <= 0) return;
    int r = static_cast<int>(radius);
    r = std::max(0, std::min(r, static_cast<int>(std::min(w, h) / 2)));
    if (r <= 0) { fillRect(x, y, w, h, color); return; }

    RoundTex& rt = impl_->roundFor(r, true);
    if (!rt.tex) { fillRect(x, y, w, h, color); return; }

    float t = static_cast<float>(rt.t);
    float cu = static_cast<float>(rt.corner) / t;
    float strip0 = 0.5f;
    float strip1 = 0.5f + 1.0f / t;
    float c = static_cast<float>(rt.corner);

    Premult(color);
    impl_->curTex = rt.tex;
    auto quad = [&](float qx, float qy, float qw, float qh,
                    float u0, float v0, float u1, float v1) {
        impl_->emitQuad(qx, qy, qx + qw, qy + qh, u0, v0, u1, v1, color);
    };
    quad(x, y, c, c, 0, 0, cu, cu);
    quad(x + w - c, y, c, c, 1 - cu, 0, 1, cu);
    quad(x, y + h - c, c, c, 0, 1 - cu, cu, 1);
    quad(x + w - c, y + h - c, c, c, 1 - cu, 1 - cu, 1, 1);
    quad(x + c, y, w - 2 * c, c, strip0, 0, strip1, cu);
    quad(x + c, y + h - c, w - 2 * c, c, strip0, 1 - cu, strip1, 1);
    quad(x, y + c, c, h - 2 * c, 0, strip0, cu, strip1);
    quad(x + w - c, y + c, c, h - 2 * c, 1 - cu, strip0, 1, strip1);
    quad(x + c, y + c, w - 2 * c, h - 2 * c, strip0, strip0, strip1, strip1);
    impl_->curTex = impl_->whiteTex;
}

float BinUIGLGdi::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->ready) return 0;
    std::vector<uint32_t> cps;
    Utf8Iter it{ utf8 };
    while (it.next()) cps.push_back(it.cp);
    float w = 0;
    size_t i = 0;
    while (i < cps.size()) {
        auto m = impl_->clusterMetric(size, weight, cps.data() + i, cps.size() - i, italic);
        w += m.adv;
        i += m.len;
    }
    return w;
}

void BinUIGLGdi::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    Atlas* a = impl_->getAtlas(size, weight, false, false, italic);
    if (!a) { ascent = lineHeight = static_cast<int>(size); return; }
    ascent = a->ascent;
    lineHeight = a->ascent + a->descent;
}

void BinUIGLGdi::drawText(float x, float y, float size, int weight,
                       const std::string& utf8, uint32_t color,
                       bool centerH, bool centerV, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    float w = textWidth(size, weight, utf8, italic);
    if (centerH) x -= w / 2;

    int ascent = 0, lineH = 0;
    fontMetrics(size, weight, ascent, lineH, italic);
    if (centerV) y -= lineH / 2.0f;
    // 像素对齐：字形位图天生是整数像素设计，落在半像素上会被双线性过滤糊一层。
    // advance 本身是整数，所以只要把起点/基线取整，整行字形就都在整数像素上。
    float baseline = std::floor(y + 0.5f);
    x = std::floor(x + 0.5f);

    std::vector<uint32_t> cps;
    Utf8Iter it{ utf8 };
    while (it.next()) cps.push_back(it.cp);

    Premult(color);
    impl_->curTex = impl_->whiteTex;
    float cx = x;
    auto drawGray = [&](uint32_t cp) {
        Atlas* a = impl_->getAtlas(size, weight, cp >= 0x80, IsEmojiCp(cp), italic);
        if (!a) return;
        const Glyph* g = impl_->glyphFor(a, cp);
        if (!g) return;
        float t = static_cast<float>(a->sizePx);
        float cell = static_cast<float>(a->cellW);
        float gx = (g->cell % a->cols) * cell;
        float gy = (g->cell / a->cols) * cell;
        float top = baseline - (a->ascent + 1);
        impl_->curTex = a->tex;
        impl_->emitQuad(cx, top, cx + cell, top + cell,
                        gx / t, gy / t, (gx + cell) / t, (gy + cell) / t, color);
        cx += g->advW;
    };

    size_t i = 0;
    while (i < cps.size()) {
        size_t n = EmojiClusterLen(cps.data() + i, cps.size() - i);
        bool colorCp = impl_->fontReady && (n > 1 || IsEmojiCp(cps[i]));
        if (colorCp) {
            ColorAtlas* ca = impl_->getColorAtlas(size);
            const ColorGlyph* g = ca ? impl_->colorGlyphFor(ca, cps.data() + i, n) : nullptr;
            if (g) {
                float t = static_cast<float>(ca->sizePx);
                float cell = static_cast<float>(ca->cellW);
                float gx = (g->cell % ca->cols) * cell;
                float gy = (g->cell / ca->cols) * ca->cellH;
                float top = baseline - g->baseline;
                impl_->curTex = ca->tex;
                impl_->emitQuad(cx, top, cx + g->w, top + g->h,
                                gx / t, gy / t, (gx + g->w) / t, (gy + g->h) / t,
                                0xFFFFFFFF);   // 彩色字形：白色顶点，不染色
                cx += g->advW;
                i += n;
                continue;
            }
        }
        for (size_t k = 0; k < n; k++) drawGray(cps[i + k]);
        i += n;
    }
    impl_->curTex = impl_->whiteTex;
}

void BinUIGLGdi::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                              const std::string& utf8, uint32_t color, int align, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    int ascent = 0, lineH = 0;
    fontMetrics(size, weight, ascent, lineH, italic);

    std::vector<uint32_t> cps;
    Utf8Iter it{ utf8 };
    while (it.next()) cps.push_back(it.cp);

    float ly = y;
    size_t pos = 0;
    while (pos < cps.size()) {
        float w = 0;
        size_t end = pos;
        while (end < cps.size()) {
            uint32_t c = cps[end];
            if (c == '\n') break;
            auto m = impl_->clusterMetric(size, weight, cps.data() + end, cps.size() - end, italic);
            if (end > pos && w + m.adv > maxW) break;
            w += m.adv;
            end += m.len;
        }
        if (end == pos) end++;
        bool newline = end < cps.size() && cps[end] == '\n';
        std::string seg;
        for (size_t k = pos; k < end && cps[k] != '\n'; k++) {
            uint32_t c = cps[k];
            if (c < 0x80) seg += static_cast<char>(c);
            else if (c < 0x800) {
                seg += static_cast<char>(0xC0 | (c >> 6));
                seg += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                seg += static_cast<char>(0xE0 | (c >> 12));
                seg += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                seg += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                seg += static_cast<char>(0xF0 | (c >> 18));
                seg += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                seg += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                seg += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        if (!seg.empty()) {
            float lx = x;
            if (align == 1)      lx = x + (maxW - w) / 2.0f;
            else if (align == 2) lx = x + (maxW - w);
            if (lx < x) lx = x; // 超长行不向左溢出
            drawText(lx, ly + ascent, size, weight, seg, color, false, false, italic);
        }
        ly += lineH;
        pos = newline ? end + 1 : end;
    }
}

bool BinUIGLGdi::prepareImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImage(index, data, size, generation);
}

bool BinUIGLGdi::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImageRaw(index, w, h, rgba, generation);
}

void BinUIGLGdi::drawImage(uint16_t index, float x, float y, float w, float h) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    auto it = impl_->images.find(index);
    if (it == impl_->images.end() || !it->second) return;
    impl_->curTex = it->second;
    impl_->emitQuad(x, y, x + w, y + h, 0, 0, 1, 1, 0xFFFFFFFF);
    impl_->curTex = impl_->whiteTex;
}

int BinUIGLGdi::pushClip(float x, float y, float w, float h) {
    if (!impl_->ready) return impl_->scissorDepth;
    int depth = impl_->scissorDepth++;
    int l = std::max(impl_->scissor[0], static_cast<int>(x));
    int t = std::max(impl_->scissor[1], static_cast<int>(y));
    int r = std::min(impl_->scissor[2], static_cast<int>(x + w));
    int b = std::min(impl_->scissor[3], static_cast<int>(y + h));
    impl_->scissor[0] = l;
    impl_->scissor[1] = t;
    impl_->scissor[2] = r;
    impl_->scissor[3] = b;
    return depth;
}

void BinUIGLGdi::popClipTo(int depth) {
    impl_->scissorDepth = depth;
    if (impl_->scissorDepth == 0) {
        impl_->scissor[0] = 0;
        impl_->scissor[1] = 0;
        impl_->scissor[2] = impl_->winW;
        impl_->scissor[3] = impl_->winH;
    }
}

void BinUIGLGdi::setFontName(const char* name) {
    if (name && *name) impl_->asciiFont = name;
}

// ============================================================
// drawNodesGLGdi：与 drawNodesGDI/D3D 语义一致的节点树 GL 绘制
// ============================================================
namespace binui {
// ============================================================
// 视频节点绘制辅助（v9，与 D3D11 后端同构）
// ============================================================

static void FormatVideoTime(double sec, char* buf, size_t n) {
    if (sec < 0) sec = 0;
    int s = (int)(sec + 0.5);
    snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

// 控制条：底条 + 进度轨道/已播放/滑块 + 播放暂停图标 + 时间文本
template <typename Gfx>
static void DrawVideoControlsTmpl(Gfx& gfx, const BinUIRenderer& renderer,
                                  const BinUIRenderer::NodeInfo& node,
                                  float x, float y, float nw, float opacity) {
    (void)y;   // 控制条几何按节点矩形算，纵向不用外部传入
    BinUIRenderer::VideoBarGeo g;
    if (!BinUIRenderer::VideoBarLayout(node, g)) return;
    const auto* v = renderer.findVideo(node.id);
    if (!v) return;

    uint32_t barBg   = ScaleAlpha(0xB4000000, opacity);
    uint32_t trackC  = ScaleAlpha(0x4DFFFFFF, opacity);
    uint32_t solidC  = ScaleAlpha(0xFFFFFFFF, opacity);
    uint32_t timeC   = ScaleAlpha(0xD9FFFFFF, opacity);

    // 底条（覆盖到视频底缘）
    gfx.fillRect(x, g.y, nw, g.h, barBg);

    double dur = v->duration, pos = v->position;
    float ratio = (dur > 0) ? (float)std::min(std::max(pos / dur, 0.0), 1.0) : 0.0f;

    // 进度轨道 + 已播放段
    gfx.fillRounded(g.trX, g.trY, g.trW, g.trH, g.trH * 0.5f, trackC);
    float playedW = g.trW * ratio;
    if (playedW >= 2.0f)
        gfx.fillRounded(g.trX, g.trY, playedW, g.trH, g.trH * 0.5f, solidC);

    // 滑块（圆）
    float tcx = g.trX + playedW;
    float tcy = g.trY + g.trH * 0.5f;
    gfx.fillRounded(tcx - g.thumbR, tcy - g.thumbR,
                    g.thumbR * 2, g.thumbR * 2, g.thumbR, solidC);

    // 播放/暂停图标（按钮中心）
    float bx = g.btnX + g.btnS * 0.5f;
    float by = g.btnY + g.btnS * 0.5f;
    if (v->playing) { // ⏸ 两根竖条
        gfx.fillRect(bx - 5, by - 7, 3.5f, 14.0f, solidC);
        gfx.fillRect(bx + 1.5f, by - 7, 3.5f, 14.0f, solidC);
    } else {          // ▶ 行扫描三角形（宽边在左、顶点朝右）
        for (int j = 0; j < 7; j++) {
            float frac = 1.0f - std::fabs(j - 3) / 4.0f;
            float wl = 13.0f * frac;
            if (wl >= 1.0f) gfx.fillRect(bx - 5.5f, by - 7 + j * 2.0f, wl, 2.0f, solidC);
        }
    }

    // 时间文本（右对齐）：m:ss / m:ss
    char cur[16] = "0:00", tot[16] = "0:00";
    FormatVideoTime(pos, cur, sizeof(cur));
    if (dur > 0) FormatVideoTime(dur, tot, sizeof(tot));
    std::string label = std::string(cur) + " / " + tot;
    float tw = gfx.textWidth(12.0f, 400, label);
    int asc = 0, lh = 0;
    gfx.fontMetrics(12.0f, 400, asc, lh);
    gfx.drawText(x + nw - 10 - tw, g.y + (g.h - lh) * 0.5f + asc,
                 12.0f, 400, label, timeC);
}

void drawNodesGLGdi(BinUIGLGdi& gfx, const BinUIRenderer& renderer, const void* clipRect,
                 const CanvasDrawFn& canvas) {
    const auto& nodes = renderer.getNodes();
    int winW = gfx.width();
    int winH = gfx.height();

    int clipL = 0, clipT = 0, clipR = winW, clipB = winH;
    if (clipRect) {
        const int* c = static_cast<const int*>(clipRect);
        int l = c[0], t = c[1], r = c[2], b = c[3];
        if (r > l && b > t && l >= 0 && t >= 0 && r <= winW && b <= winH) {
            clipL = l; clipT = t; clipR = r; clipB = b;
        }
    }

    std::unordered_map<uint16_t, size_t> byId;
    byId.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) byId[nodes[i].id] = i;

    std::unordered_map<uint16_t, binui::BinUIRenderer::ScrollState> scrolls;
    for (const auto& node : nodes) {
        if (node.flags & BINUI_SCROLLABLE) {
            if (const auto* s = renderer.scrollState(node.id)) scrolls[node.id] = *s;
        }
    }

    // 预计算：父传子祖先索引 / 隐藏 / 滚动偏移 / 最内层滚动视口
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

    // 根节点背景（填充绘制范围；RGBA 画布按 alpha 与底色合成）
    for (const auto& node : nodes) {
        if (node.type == BINUI_ROOT && (node.flags & BINUI_VISIBLE)) {
            gfx.fillRect(static_cast<float>(clipL), static_cast<float>(clipT),
                         static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT),
                         node.color);
            break;
        }
    }

    // 绘制顺序：按 z-index 分层（v10）由渲染器统一给出，同层流式先于 absolute、DOM 顺序
    const std::vector<uint32_t> order = renderer.drawOrder();
    for (size_t oi = 0; oi < order.size(); oi++) {
        const size_t i = order[oi];
        const auto& node = nodes[i];
        if (!(node.flags & BINUI_VISIBLE) || hidden[i]) continue;
        if (node.type == BINUI_ROOT) continue;

        int x = static_cast<int>(node.x) - static_cast<int>(offX[i]);
        int y = static_cast<int>(node.y) - static_cast<int>(offY[i]);
        int nw = static_cast<int>(node.width);
        int nh = static_cast<int>(node.height);

        // BINUI_LINE：x,y = 起点；width,height = 终点；radius = 线宽
        if (node.type == BINUI_LINE) {
            float x1 = node.x - offX[i], y1 = node.y - offY[i];
            float x2 = node.width - offX[i], y2 = node.height - offY[i];
            uint32_t dc = renderer.drawColor(node.id);
            float op = renderer.drawOpacity(node.id);
            float ddx = renderer.drawOffsetX(node.id), ddy = renderer.drawOffsetY(node.id);
            x1 += ddx; y1 += ddy; x2 += ddx; y2 += ddy;
            if (op < 1.0f) dc = ScaleAlpha(dc, op);
            float mnx = std::min(x1, x2), mny = std::min(y1, y2);
            float mxx = std::max(x1, x2), mxy = std::max(y1, y2);
            // 线宽扩展：水平/垂直线某维跨度为 0，若按零面积 clip 会在 flush 时被
            // 批次的 scissor 检查（r<=l || bot<=t）整批丢弃——必须外扩线宽保证非零面积
            float lw = (float)node.radius;
            mnx -= lw; mny -= lw; mxx += lw; mxy += lw;
            if (mxx <= clipL || mnx >= clipR || mxy <= clipT || mny >= clipB) continue;
            if (hasVp[i] && (mxx <= vpL[i] || mnx >= vpR[i] || mxy <= vpT[i] || mny >= vpB[i])) continue;
            int savedDepth = gfx.pushClip(mnx, mny, mxx - mnx, mxy - mny);
            gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                         static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
            bool draw = true;
            size_t cur = i;
            while (draw) {
                const auto& anc = nodes[cur];
                if (anc.parentId == 0xFFFF) break;
                auto pit = byId.find(anc.parentId);
                if (pit == byId.end()) break;
                cur = pit->second;
                const auto& par = nodes[cur];
                if (!(par.flags & BINUI_VISIBLE)) { draw = false; break; }
                gfx.pushClip(par.x, par.y, par.width, par.height);
            }
            if (draw) gfx.fillLine(x1, y1, x2, y2, (float)node.radius, dc);
            gfx.popClipTo(savedDepth);
            continue;
        }

        // v7 动画：颜色（含 hover/按下 tint 与过渡）、透明度、平移、缩放、圆角
        uint32_t dc = renderer.drawColor(node.id);
        // v13 扩展绘制段：文字色 / 边框 / 渐变 / 字重 / 行高 / 文字装饰
        const BinUIPaintNode* pt = renderer.paintOf(node.id);
        const bool hasBorder = pt && (pt->flags & PAINT_BORDER);
        const bool hasGrad = pt && (pt->flags & PAINT_GRADIENT);
        float opacity = renderer.drawOpacity(node.id);
        // v14 禁用：整块压到 55%（颜色/文字/图片一起淡，不引入额外合成通道）
        const bool disabled = renderer.isDisabled(node.id);
        if (disabled) opacity *= 0.55f;
        float dx = renderer.drawOffsetX(node.id);
        float dy = renderer.drawOffsetY(node.id);
        float scale = renderer.drawScale(node.id);
        float animRadius = renderer.drawRadius(node.id);
        if (opacity < 1.0f) dc = ScaleAlpha(dc, opacity);
        if (scale != 1.0f) {
            float cx = x + nw / 2.0f, cy = y + nh / 2.0f;
            nw = (int)(nw * scale); nh = (int)(nh * scale);
            x = (int)(cx - nw / 2.0f); y = (int)(cy - nh / 2.0f);
        }
        x += (int)dx; y += (int)dy;
        int effRadius = animRadius >= 0 ? (int)animRadius : (int)node.radius;

        if (x + nw <= clipL || x >= clipR || y + nh <= clipT || y >= clipB) continue;
        if (hasVp[i] && (x + nw <= vpL[i] || x >= vpR[i] ||
                         y + nh <= vpT[i] || y >= vpB[i])) continue;

        // 裁剪链：自身 + 祖先 + 本次重绘范围（scissor 求交）。
        // 关键：持久离屏上节点重绘若不做脏矩形裁剪，会把整个节点矩形重涂，
        // 覆盖掉脏区以外（未重绘）的子元素/兄弟元素 → 悬停时内部元素消失。
        int savedDepth = gfx.pushClip(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(nw), static_cast<float>(nh));
        gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                     static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
        bool draw = true;
        size_t cur = i;
        while (draw) {
            const auto& anc = nodes[cur];
            if (anc.parentId == 0xFFFF) break;
            auto pit = byId.find(anc.parentId);
            if (pit == byId.end()) break;
            cur = pit->second;
            const auto& par = nodes[cur];
            if (!(par.flags & BINUI_VISIBLE)) { draw = false; break; }
            gfx.pushClip(par.x, par.y, par.width, par.height);
        }

        if (draw) {
            uint32_t cc = dc;
            uint32_t textCol = ScaleAlpha(0xFFFFFFFF, opacity);
            uint32_t accentCol = ScaleAlpha(0xFFBB86FC, opacity);
            int pad = node.padding;
            int fs = node.fontSize ? node.fontSize : 14;
            // v14：斜体 / 占位符（同一份 textFlags 在三端语义一致）
            const bool italic = pt && (pt->textFlags & TEXT_ITALIC) != 0;
            const bool placeholder = pt && (pt->textFlags & TEXT_PLACEHOLDER) != 0;

            // ---- v13 扩展绘制：底色/边框的统一入口（几何来自 binui_paint.h） ----
            // GL/D3D11 有 alpha 混合，条带直接画即可，不需要像 GDI 那样自己先混合底色
            auto fillNodeBg = [&](uint32_t color, float radius) {
                binui::PaintBand bands[binui::kMaxPaintBands];
                const int bn = binui::BuildFillBands(pt, static_cast<float>(x), static_cast<float>(y),
                                                     static_cast<float>(nw), static_cast<float>(nh),
                                                     radius, color, bands);
                for (int k = 0; k < bn; k++) {
                    const binui::PaintBand& b = bands[k];
                    if (b.w <= 0.0f || b.h <= 0.0f) continue;
                    if (b.round && b.radius > 0.0f)
                        gfx.fillRounded(b.x, b.y, b.w, b.h, b.radius, b.color);
                    else
                        gfx.fillRect(b.x, b.y, b.w, b.h, b.color);
                }
            };
            auto drawNodeBorder = [&]() {
                if (!hasBorder) return;
                binui::PaintBand bb[binui::kMaxPaintBands];
                const int bn = binui::BuildBorderBands(pt, static_cast<float>(x), static_cast<float>(y),
                                                       static_cast<float>(nw), static_cast<float>(nh),
                                                       static_cast<float>(effRadius), bb);
                for (int k = 0; k < bn; k++)
                    if (bb[k].w > 0.0f && bb[k].h > 0.0f)
                        gfx.fillRect(bb[k].x, bb[k].y, bb[k].w, bb[k].h, bb[k].color);
            };
            // 文字装饰线（下划线/删除线/上划线）：一条细矩形，位置/粗细由 binui_format.h
            // 统一给（三端一致）；超出节点的部分由压在上面的 scissor 裁掉
            auto drawTextRules = [&](float left, float top, float w) {
                binui::PaintBand rules[3];
                const int rn = binui::BuildTextRuleBands(pt, left, top, w,
                                                         static_cast<float>(fs), rules);
                for (int k = 0; k < rn; k++)
                    if (rules[k].w > 0.0f)
                        gfx.fillRect(rules[k].x, rules[k].y, rules[k].w, rules[k].h, rules[k].color);
            };

            switch (node.type) {
                case BINUI_LABEL: {
                    // 文字色 / 字重（v13）：作者显式写了 color / font-weight 就用它，
                    // 否则沿用节点自身颜色与常规字重（旧行为不变）
                    const bool paintText = pt && (pt->flags & PAINT_TEXT_COLOR);
                    const uint32_t lcol = paintText ? ScaleAlpha(pt->textColor, opacity) : cc;
                    const int wt = binui::PaintWeightOf(pt);
                    // LABEL 现在也可以有底色：只在有渐变、或显式给了不透明底色时铺底。
                    // 纯文字标签满屏都是，透明底再画一层只是白烧一个四边形。
                    if (hasGrad || (paintText && ((node.color >> 24) & 0xFF)))
                        fillNodeBg(dc, static_cast<float>(node.radius));
                    if (node.text.empty()) break;

                    int asc = 0, lh = 0;
                    gfx.fontMetrics(static_cast<float>(fs), wt, asc, lh, italic);
                    const float availW = static_cast<float>(nw - 2 * pad);
                    const int align = (node.flags & BINUI_TEXT_CENTER) ? 1
                                    : (node.flags & BINUI_TEXT_RIGHT) ? 2 : 0;
                    // 装饰线要落在文字上：逐行跟着实际画文字的位置走
                    const bool hasRule = pt && (pt->flags & (PAINT_UNDERLINE | PAINT_STRIKE | PAINT_OVERLINE));
                    if (node.text.find('\n') != std::string::npos) {
                        // 多行文本（<br> / '\n'）：整块垂直居中，逐行换行绘制（按 align 对齐）
                        size_t lines = 1;
                        for (char c : node.text) if (c == '\n') lines++;
                        // 行距：CSS line-height 优先（v13），未声明时用字体行高（旧行为）。
                        // drawTextWrapped 内部按字体行高推进，跨 '\n' 的段间距只能在这里补
                        const float lstep = (pt && (pt->flags & PAINT_LINE_HEIGHT))
                                          ? binui::PaintLineHeight(pt, static_cast<float>(fs))
                                          : static_cast<float>(lh);
                        float blockH = static_cast<float>(lines) * lstep;
                        float top = static_cast<float>(y) + (nh - blockH) / 2.0f;
                        if (top < static_cast<float>(y)) top = static_cast<float>(y);
                        const float lineLeft = static_cast<float>(x + pad);
                        float ly = top;
                        size_t p0 = 0;
                        while (p0 <= node.text.size()) {
                            const size_t nl = node.text.find('\n', p0);
                            const std::string seg = node.text.substr(
                                p0, (nl == std::string::npos) ? std::string::npos : nl - p0);
                            gfx.drawTextWrapped(lineLeft, ly, availW > 1.0f ? availW : 1.0f,
                                                static_cast<float>(fs), wt, seg, lcol, align, italic);
                            // 装饰线逐行画：只画一条的话，多行文本就只有第一行有下划线
                            if (hasRule && !seg.empty()) {
                                const float tw = gfx.textWidth(static_cast<float>(fs), wt, seg, italic);
                                float rx = lineLeft;
                                if (align == 1) rx += (availW - tw) * 0.5f;
                                else if (align == 2) rx += availW - tw;
                                drawTextRules(rx, ly, tw);
                            }
                            if (nl == std::string::npos) break;
                            ly += lstep;
                            p0 = nl + 1;
                        }
                    } else {
                        float base = static_cast<float>(y) + (nh - lh) / 2.0f + asc;
                        float tx = static_cast<float>(x + pad);
                        float tw = 0.0f;
                        if (align != 0 || hasRule)
                            tw = gfx.textWidth(static_cast<float>(fs), wt, node.text, italic);
                        if (align != 0) {
                            tx += (align == 1) ? (availW - tw) / 2.0f : (availW - tw);
                            if (tx < static_cast<float>(x + pad)) tx = static_cast<float>(x + pad);
                        }
                        gfx.drawText(tx, base, static_cast<float>(fs), wt, node.text, lcol,
                                     false, false, italic);
                        if (hasRule)
                            drawTextRules(tx, static_cast<float>(y) + (nh - lh) / 2.0f, tw);
                    }
                    break;
                }
                case BINUI_BUTTON: {
                    fillNodeBg(dc, static_cast<float>(effRadius));
                    if (!node.text.empty()) {
                        // 文字色：v13 显式文字色优先，否则保持白字（旧行为）
                        const uint32_t tcol = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                                  ? ScaleAlpha(pt->textColor, opacity) : textCol;
                        int bs = node.fontSize ? node.fontSize : 14;
                        float tw = gfx.textWidth(static_cast<float>(bs), 700, node.text, italic);
                        int asc = 0, lh = 0;
                        gfx.fontMetrics(static_cast<float>(bs), 700, asc, lh, italic);
                        float tx = x + pad + (nw - 2 * pad - tw) / 2.0f;
                        float base = y + (nh - lh) / 2.0f + asc;
                        gfx.drawText(tx, base, static_cast<float>(bs), 700, node.text, tcol,
                                     false, false, italic);
                    }
                    break;
                }
                case BINUI_INPUT: {
                    fillNodeBg(dc, static_cast<float>(node.radius));
                    gfx.fillRect(static_cast<float>(x), static_cast<float>(y + nh - 2),
                                 static_cast<float>(nw), 2, accentCol);

                    bool editing = renderer.isFocused(node.id);
                    std::string text = node.text;
                    size_t caret = 0, selStart = 0;
                    if (editing) {
                        text = renderer.edit().value;
                        caret = renderer.edit().caret;
                        selStart = renderer.edit().selStart;
                    }

                    if (!text.empty()) {
                        int asc = 0, lh = 0;
                        gfx.fontMetrics(static_cast<float>(fs), 400, asc, lh, italic);
                        float base = static_cast<float>(y) + (nh - lh) / 2.0f + asc;
                        float tx = static_cast<float>(x + pad + 4);
                        // 占位符按 45% 色画（不是真实内容，视觉上要和已输入文本区分开）
                        const uint32_t inCol = (placeholder && !editing)
                                                   ? ScaleAlpha(accentCol, 0.55f) : accentCol;
                        gfx.drawText(tx, base, static_cast<float>(fs), 400, text, inCol,
                                     false, false, italic);

                        if (editing) {
                            size_t a = std::min(caret, selStart);
                            size_t b = std::max(caret, selStart);
                            if (a != b) {
                                float wa = gfx.textWidth(static_cast<float>(fs), 400, text.substr(0, a));
                                float wb = gfx.textWidth(static_cast<float>(fs), 400, text.substr(0, b));
                                gfx.fillRect(tx + wa, static_cast<float>(y + 2),
                                             wb - wa, static_cast<float>(nh - 4),
                                             ScaleAlpha(0x66BB86FC, opacity));
                            }
                            if (caretBlinkOn()) {
                                float wc = gfx.textWidth(static_cast<float>(fs), 400, text.substr(0, caret));
                                gfx.fillRect(tx + wc, static_cast<float>(y + 2),
                                             2, static_cast<float>(nh - 4), accentCol);
                            }
                        }
                    }
                    break;
                }
                case BINUI_LIST: {
                    fillNodeBg(dc, static_cast<float>(effRadius));
                    if (!node.text.empty()) {
                        // 文字色 / 字重（v13）：显式写法优先，否则白字 + 常规字重（旧行为）
                        const uint32_t tcol = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                                  ? ScaleAlpha(pt->textColor, opacity) : textCol;
                        const int wt = binui::PaintWeightOf(pt);
                        const int align = (node.flags & BINUI_TEXT_CENTER) ? 1
                                        : (node.flags & BINUI_TEXT_RIGHT) ? 2 : 0;
                        gfx.drawTextWrapped(static_cast<float>(x + pad + 12),
                                            static_cast<float>(y + pad + 8),
                                            static_cast<float>(nw - 2 * pad - 24),
                                            static_cast<float>(fs), wt, node.text, tcol, align, italic);
                    }
                    break;
                }
                case BINUI_IMAGE: {
                    const ImageResource* img = renderer.getImage(node.imageIndex);
                    bool drawn = false;
                    if (img && !img->data.empty()) {
                        uint32_t gen = renderer.imageGeneration(node.imageIndex);
                        if (img->rawPixels) {
                            if (gfx.prepareImageRaw(node.imageIndex, img->width, img->height,
                                                    img->data.data(), gen))
                                gfx.drawImage(node.imageIndex, static_cast<float>(x),
                                              static_cast<float>(y),
                                              static_cast<float>(nw), static_cast<float>(nh));
                            drawn = true;
                        } else if (gfx.prepareImage(node.imageIndex, img->data.data(),
                                                    img->data.size(), gen)) {
                            gfx.drawImage(node.imageIndex, static_cast<float>(x),
                                          static_cast<float>(y),
                                          static_cast<float>(nw), static_cast<float>(nh));
                            drawn = true;
                        }
                    }
                    if (!drawn) {
                        fillNodeBg(dc, static_cast<float>(effRadius));
                        if (!node.text.empty()) {
                            int asc = 0, lh = 0;
                            gfx.fontMetrics(24.0f, 300, asc, lh);
                            gfx.drawText(x + nw / 2.0f, y + nh / 2.0f - lh / 2.0f + asc,
                                         24.0f, 300, node.text, textCol, true, false);
                        }
                    }
                    break;
                }
                case BINUI_VIDEO: {
                    // 底色：无解码帧时显示（深色海报底）
                    fillNodeBg(dc, static_cast<float>(effRadius));

                    // 解码帧（伪图片槽，rawPixels）：contain-fit letterbox
                    const ImageResource* img = renderer.getImage(node.imageIndex);
                    if (img && img->rawPixels && !img->data.empty() &&
                        img->width > 0 && img->height > 0) {
                        uint32_t gen = renderer.imageGeneration(node.imageIndex);
                        if (gfx.prepareImageRaw(node.imageIndex, img->width, img->height,
                                                img->data.data(), gen)) {
                            float ar = static_cast<float>(img->width) / img->height;
                            float boxAr = static_cast<float>(nw) / nh;
                            float fw, fh;
                            if (ar > boxAr) { fw = static_cast<float>(nw); fh = fw / ar; }
                            else            { fh = static_cast<float>(nh); fw = fh * ar; }
                            gfx.drawImage(node.imageIndex,
                                          x + (nw - fw) * 0.5f, y + (nh - fh) * 0.5f, fw, fh);
                        }
                    }

                    // 浏览器式控制条（暂停常显/播放中悬停显示）
                    if (renderer.videoControlsVisible(node.id))
                        DrawVideoControlsTmpl(gfx, renderer, node,
                                              static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(nw), opacity);
                    break;
                }
                case BINUI_SVG:
                case BINUI_CANVAS: {
                    // 容器矩形：有底色则绘制（svg/canvas 默认透明）
                    fillNodeBg(dc, static_cast<float>(effRadius));
                    if (node.type == BINUI_CANVAS && canvas)
                        canvas(gfx, node.id, static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(nw), static_cast<float>(nh));
                    break;
                }
                default:
                    break;
            }

            // 边框（v13）：画在节点内侧，四边可不同宽，压在底色之上
            drawNodeBorder();

            if (renderer.isKeyboardFocused(node.id)) {
                // 焦点环必须跟随节点自己的圆角：以前写死半径 10、内缩 2，
                // 圆角 8 的按钮上就会套出一个半径不同的环，看着像"双线框"。
                const float fr = effRadius > 2 ? static_cast<float>(effRadius) - 1.0f : 0.0f;
                const float fx = static_cast<float>(x) + 1.0f, fy = static_cast<float>(y) + 1.0f;
                const float fw = static_cast<float>(nw) - 2.0f, fh = static_cast<float>(nh) - 2.0f;
                if (fr >= 1.0f) {
                    gfx.strokeRounded(fx, fy, fw, fh, fr, 1.0f, textCol);
                } else {
                    // 直角节点不能用 strokeRounded：它 r<=0 时会退化成整块填充，
                    // 焦点框会把节点涂白。这里直接画四条 1px 线。
                    gfx.fillRect(fx, fy, fw, 1.0f, textCol);
                    gfx.fillRect(fx, fy + fh - 1.0f, fw, 1.0f, textCol);
                    gfx.fillRect(fx, fy, 1.0f, fh, textCol);
                    gfx.fillRect(fx + fw - 1.0f, fy, 1.0f, fh, textCol);
                }
            }
        }

        gfx.popClipTo(savedDepth);
    }

    // 滚动条（轨道 + 滑块）：同样裁剪到本次重绘范围，避免滚动条整条重涂
    int sbDepth = gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                               static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
    for (const auto& node : nodes) {
        if (!(node.flags & BINUI_SCROLLABLE) || !(node.flags & BINUI_VISIBLE)) continue;
        float ch = renderer.contentHeight(node.id);
        auto sit = scrolls.find(node.id);
        float sy = sit != scrolls.end() ? sit->second.y : 0;
        int x = static_cast<int>(node.x), y = static_cast<int>(node.y);
        int nw = static_cast<int>(node.width), nh = static_cast<int>(node.height);
        if (ch <= nh + 0.5f) continue;

        int barW = 8;
        gfx.fillRect(static_cast<float>(x + nw - barW), static_cast<float>(y),
                     static_cast<float>(barW), static_cast<float>(nh), 0xFF2E2E2E);
        float thumbH = std::max(24.0f, static_cast<float>(nh) * nh / ch);
        float maxScroll = ch - nh;
        float thumbY = maxScroll > 0 ? sy / maxScroll * (nh - thumbH) : 0;
        gfx.fillRect(static_cast<float>(x + nw - barW + 2), static_cast<float>(y + thumbY),
                     4.0f, thumbH, 0xFF5A5A5A);
    }
    gfx.popClipTo(sbDepth);
}

// ============================================================
// placeCaretAtClickGLGdi（基于字形 advance 测量）
// ============================================================
void placeCaretAtClickGLGdi(BinUIRenderer& renderer, BinUIGLGdi& gfx, float cx, float cy) {
    if (!renderer.isEditing()) return;
    const auto& edit = renderer.edit();

    const auto& nodes = renderer.getNodes();
    const binui::BinUIRenderer::NodeInfo* node = nullptr;
    for (const auto& n : nodes) {
        if (n.id == edit.nodeId) { node = &n; break; }
    }
    if (!node || node->type != BINUI_INPUT) return;
    if (cy < node->y || cy > node->y + node->height) return;

    std::wstring wtext = Utf8ToWide(edit.value);
    float textX = static_cast<float>(node->x) + node->padding + 4;
    float target = cx - textX;
    int fs = node->fontSize ? node->fontSize : 14;
    if (target <= 0) { renderer.setCaretByUtf16Index(0); return; }

    // 累计前缀宽度（UTF-16 单位），找越过点击点的位置
    std::string prefix;
    size_t idx = 0;
    float w = 0;
    for (size_t i = 0; i < wtext.size(); i++) {
        unsigned cp = wtext[i];
        std::string ch;
        if (cp < 0x80) ch += static_cast<char>(cp);
        else if (cp < 0x800) {
            ch += static_cast<char>(0xC0 | (cp >> 6));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            ch += static_cast<char>(0xE0 | (cp >> 12));
            ch += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            ch += static_cast<char>(0xF0 | (cp >> 18));
            ch += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            ch += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
        }
        float cw = gfx.textWidth(static_cast<float>(fs), 400, prefix + ch) - w;
        if (w + cw / 2 >= target) break;
        w += cw;
        prefix += ch;
        idx = i + 1;
    }
    renderer.setCaretByUtf16Index(idx);
}

// ============================================================
// 光标闪烁
// ============================================================
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

// 标题栏判定（与 GDI/D3D 后端语义一致）
bool isTitlebarDrag(const BinUIRenderer& renderer, float x, float y) {
    const auto& nodes = renderer.getNodes();
    if (renderer.hitTest(x, y) >= 0) return false;
    for (const auto& n : nodes)
        if ((n.flags & BINUI_TITLEBAR) && (n.flags & BINUI_VISIBLE) &&
            x >= n.x && x <= n.x + n.width && y >= n.y && y <= n.y + n.height)
            return true;
    return false;
}

void BinUIAppGLGdi::beginWindowDrag() {
    if (!hwnd_) return;
    ReleaseCapture();
    POINT p;
    GetCursorPos(&p);
    SendMessageA(static_cast<HWND>(hwnd_), WM_NCLBUTTONDOWN, HTCAPTION,
                 MAKELPARAM(p.x, p.y));
}

void BinUIAppGLGdi::minimize() {
    if (hwnd_) ShowWindow(static_cast<HWND>(hwnd_), SW_MINIMIZE);
}

// 手动 resize 拖拽循环（窗口无 WS_THICKFRAME，系统不提供 resize，这里自行实现）。
// 拖拽期间每轮实时渲染到新尺寸离屏 + GDI blit 到窗口（实时重绘、跟手）。
// 关键：拖拽期间绝不能 SwapBuffers——NVIDIA 驱动在"窗口 resize + SwapBuffers"组合下
// 必然崩溃（nvoglv64 DrvPresentBuffers），已实测 vsync 开关/上下文 detach/节流呈现
// 均无法规避；走 presentViaGdi（读回离屏像素 → StretchDIBits）完全绕开驱动 present 路径。
// 松手时最后再以 GDI 呈现一次清晰帧，然后恢复正常 SwapBuffers（surface 已稳定）。
void BinUIAppGLGdi::startResize(int hit) {
    const int MIN_W = 320, MIN_H = 240;
    HWND hwnd = static_cast<HWND>(hwnd_);
    SetCapture(hwnd);
    POINT start;
    GetCursorPos(&start);
    RECT startRect;
    GetWindowRect(hwnd, &startRect);
    gdiPresent_ = true;
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
        // 等比缩放（<canvas aspect="...">）：以主导边为基准约束另一边
        if (aspect_ > 0) {
            LONG wd = r.right - r.left, ht = r.bottom - r.top;
            if (hit == HTTOP || hit == HTBOTTOM) {
                wd = static_cast<LONG>(ht * aspect_ + 0.5f);
                if (hit == HTTOP) r.top = r.bottom - ht;
                else r.bottom = r.top + ht;
            } else {
                ht = static_cast<LONG>(wd / aspect_ + 0.5f);
                if (hit == HTTOPLEFT || hit == HTTOP || hit == HTTOPRIGHT)
                    r.top = r.bottom - ht;
                else
                    r.bottom = r.top + ht;
            }
            if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT)
                r.left = r.right - wd;
            else
                r.right = r.left + wd;
        }
        SetWindowPos(hwnd, nullptr, r.left, r.top,
            r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyPendingResize();
        renderOnce(); // gdiPresent_ = true → presentViaGdi，不 SwapBuffers
        Sleep(1);
    }
    // 松手：应用最终尺寸并以 GDI 呈现清晰帧（surface 可能仍未稳定，不 SwapBuffers）
    applyPendingResize();
    renderOnce();
    gdiPresent_ = false; // 此后动画帧恢复正常 SwapBuffers（surface 已稳定）
    ReleaseCapture();
}

// ============================================================
// BinUIAppGLGdi：无边框窗口 + 事件循环
// ============================================================
LRESULT CALLBACK BinUIAppGLGdi::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BinUIAppGLGdi* app = reinterpret_cast<BinUIAppGLGdi*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcA(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!app->trackingLeave_) {
                POINT cursor;
                RECT wrect;
                if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &wrect) && PtInRect(&wrect, cursor)) {
                    TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    app->trackingLeave_ = true;
                }
            }
            app->handleMouseMove(static_cast<float>(LOWORD(lParam)),
                                 static_cast<float>(HIWORD(lParam)));
            return 0;
        }
        case WM_LBUTTONDOWN:
            app->handleMousePress(static_cast<float>(LOWORD(lParam)),
                                  static_cast<float>(HIWORD(lParam)));
            SetFocus(hwnd);
            return 0;
        case WM_LBUTTONUP:
            app->handleMouseRelease(static_cast<float>(LOWORD(lParam)),
                                    static_cast<float>(HIWORD(lParam)));
            return 0;
        case WM_MOUSELEAVE:
            app->trackingLeave_ = false;
            app->handleMouseLeave();
            return 0;
        case WM_MOUSEWHEEL: {
            short delta = static_cast<short>(HIWORD(wParam));
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            app->handleMouseWheel(static_cast<float>(pt.x), static_cast<float>(pt.y),
                                  static_cast<float>(delta) / 120.0f * 40.0f);
            return 0;
        }
        case WM_CHAR:
            app->handleChar(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            app->handleKey(static_cast<int>(wParam));
            return 0;
        case WM_KILLFOCUS:
            app->handleFocusLost();
            return 0;
        case WM_SIZE:
            app->handleResize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_NCLBUTTONDOWN: {
            // 无 WS_THICKFRAME：命中边框热区时自行启动 resize 循环
            if (!app->resizable_) return DefWindowProcA(hwnd, msg, wParam, lParam);
            switch (wParam) {
                case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
                case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT:
                case HTBOTTOMRIGHT: {
                    if (IsZoomed(hwnd))
                        return DefWindowProcA(hwnd, msg, wParam, lParam);
                    app->startResize(static_cast<int>(wParam));
                    return 0;
                }
                default:
                    return DefWindowProcA(hwnd, msg, wParam, lParam);
            }
        }
        case WM_NCHITTEST: {
            // 无边框窗口：边框 8px 热区用于调整大小，其余为客户区
            if (!app->resizable_) return HTCLIENT;
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
        case WM_CLOSE:
            app->requestExit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

BinUIAppGLGdi::BinUIAppGLGdi(int width, int height, bool vsync)
    : width_(width), height_(height), vsync_(vsync) {}

void BinUIAppGLGdi::setWindowOptions(bool resizable, float aspect, bool transparent, const char* title) {
    resizable_ = resizable;
    aspect_ = aspect > 0 ? aspect : 0;
    transparent_ = transparent;
    if (title) title_ = title;
}

BinUIAppGLGdi::~BinUIAppGLGdi() { gfx_.shutdown(); }

void BinUIAppGLGdi::invalidateRect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dirty_) {
        dirtyL_ = std::min(dirtyL_, x);
        dirtyT_ = std::min(dirtyT_, y);
        dirtyR_ = std::max(dirtyR_, x + w);
        dirtyB_ = std::max(dirtyB_, y + h);
    } else {
        dirty_ = true;
        dirtyL_ = x; dirtyT_ = y; dirtyR_ = x + w; dirtyB_ = y + h;
    }
}

void BinUIAppGLGdi::invalidateAll() {
    dirty_ = true;
    dirtyL_ = 0; dirtyT_ = 0; dirtyR_ = width_; dirtyB_ = height_;
}

int BinUIAppGLGdi::run() {
    SetProcessDPIAware();

    const char* CLASS_NAME = "BinUIAppGLGdiWindow";
    WNDCLASSA wc = {};
    // CS_OWNDC：窗口独占 DC，resize 时系统不回收该 DC（避免 SwapBuffers 用到
    // 被拆掉的旧后备缓冲 —— GDI 呈现路径在快速 resize 时会因此崩溃）
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = BinUIAppGLGdi::wndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassA(&wc);

    RECT work = { 0, 0, 800, 600 };
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int winW = std::min(width_, static_cast<int>(work.right - work.left));
    int winH = std::min(height_, static_cast<int>(work.bottom - work.top));
    int winX = work.left + ((work.right - work.left) - winW) / 2;
    int winY = work.top + ((work.bottom - work.top) - winH) / 2;

    // 无边框：WS_POPUP 无标题栏/边框/系统菜单；WS_MAXIMIZEBOX 允许 Win+↑ 最大化
    DWORD exStyle = 0;
    if (transparent_) exStyle |= WS_EX_LAYERED;
    HWND hwnd = CreateWindowExA(exStyle, CLASS_NAME, title_.c_str(),
        WS_POPUP | WS_MAXIMIZEBOX | WS_VISIBLE, winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[gl] Failed to create window\n");
        return 1;
    }
    hwnd_ = hwnd;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // 禁用 Windows 11 DWM 自动圆角：UI 圆角由 border-radius 属性自控（跨平台一致）
    DWORD corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    gfx_.setTransparent(transparent_);
    if (!gfx_.init(hwnd, winW, winH, vsync_)) {
        std::fprintf(stderr, "[gl] OpenGL init failed\n");
        return 1;
    }
    SetFocus(hwnd);
    width_ = winW;
    height_ = winH;
    invalidateAll(); // 首帧全量绘制（初始脏矩形为空，避免首帧仅动画区域被绘制，其余留白/留黑）

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

        renderOnce();
        Sleep(1);
    }

    DestroyWindow(hwnd);
    hwnd_ = nullptr;
    return 0;
}

void BinUIAppGLGdi::handleMouseMove(float x, float y) { if (move_) move_(x, y); }
void BinUIAppGLGdi::handleMousePress(float x, float y) { if (press_) press_(x, y); }
void BinUIAppGLGdi::handleMouseRelease(float x, float y) { if (release_) release_(x, y); }
void BinUIAppGLGdi::handleMouseLeave() { if (leave_) leave_(); }

void BinUIAppGLGdi::handleMouseWheel(float x, float y, float dy) { if (wheel_) wheel_(x, y, dy); }

void BinUIAppGLGdi::handleResize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    // 只记录：拖拽期间 WM_SIZE 高频到达，实际 resize（重排/FBO 重建/重绘）每帧应用一次
    pendingW_ = w;
    pendingH_ = h;
}

void BinUIAppGLGdi::applyPendingResize() {
    if (pendingW_ <= 0 || pendingH_ <= 0) return;
    int w = pendingW_, h = pendingH_;
    pendingW_ = pendingH_ = 0;
    width_ = w;
    height_ = h;
    if (resize_) resize_(w, h);
    gfx_.resize(w, h);
    invalidateAll();
}

void BinUIAppGLGdi::renderOnce() {
    applyPendingResize();
    if (dirty_ && gfx_.isReady()) {
        int rc[4] = { dirtyL_, dirtyT_, dirtyR_, dirtyB_ };
        gfx_.beginFrame(rc);
        if (draw_) draw_(gfx_, rc);
        if (gdiPresent_ && !transparent_) gfx_.presentViaGdi();
        else gfx_.present();
        dirty_ = false;
    }
}

void BinUIAppGLGdi::handleChar(wchar_t ch) {
    if (ch >= 0xD800 && ch <= 0xDBFF) { pendingHigh_ = ch; return; }
    unsigned cp;
    if (ch >= 0xDC00 && ch <= 0xDFFF && pendingHigh_) {
        cp = 0x10000 + ((static_cast<unsigned>(pendingHigh_) - 0xD800) << 10) + (ch - 0xDC00);
        pendingHigh_ = 0;
    } else {
        pendingHigh_ = 0;
        cp = static_cast<unsigned>(ch);
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (char_) {
        std::string s;
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800) {
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
        char_(s);
    }
}

void BinUIAppGLGdi::handleKey(int vk) {
    if (key_) key_(vk);
}

void BinUIAppGLGdi::handleFocusLost() {
    pendingHigh_ = 0;
    if (focus_) focus_(false);
}

} // namespace binui
