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
#include <imm.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>

#include "binui_font_renderer.h"
#include "binui_paint.h"
#include "backends/binui_draw_nodes.h"   // v13 扩展绘制的条带几何（与 GDI/D3D11 后端共用同一份数学）
#include "backends/binui_glyph_atlas.h"  // 字形图集 + 排版（v19：与 Vulkan 后端共用同一份）
#include "backends/binui_win_image.h"    // GDI+ 图片解码（与 Vulkan 后端共用）
#include "platform/binui_win32_foreground.h"

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

struct RoundTex {
    GLuint tex = 0;
    int t = 0;         // 模板边长（RoundTemplateSize：2r+4）
    uint64_t lastUse = 0;   // LRU 计数（缓存满了按它淘汰）
};


// 顶点缓冲：**初始** 10 万顶点（2 MB，约 1.66 万个四边形），装不下就翻倍扩容，
// 硬上限 100 万顶点（20 MB，约 17 万个四边形）。以前这里是"满了就丢图元"的硬上限，
// 一屏图标超过约 34 个（每个 473 条带）后面的内容就凭空消失、还不报错 —— 见 docs/future.md P0-5。
constexpr size_t kMaxVerts = 100000;
constexpr size_t kMaxVertsCeiling = 1u << 20;

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

// 字形图集的纹理宿主：binui::GlyphAtlas 负责排版与光栅化，这里只做"位图→GL 纹理"。
// 句柄直接用 GLuint 装进 void*（GL 名字是 32 位无符号，0 表示失败）。
struct GLGlyphTexHost : binui::IGlyphTexHost {
    void* createTex(int w, int h) override {
        // 必须给零缓冲：图集里的"空格子"（空格/控符）会被当真字形画，
        // 未初始化的显存会把这些位置画成垃圾像素
        std::vector<uint8_t> zeros(static_cast<size_t>(w) * h * 4, 0);
        // LINEAR：动画节点的文字按亚像素落位（见 GlyphAtlas::snapText）。整像素位置下
        // 1:1 采样点正好落在纹素中心，与 NEAREST 逐像素相同，所以静态页面不受影响。
        const GLuint t = CreateTexture2D(w, h, zeros.data(), true);
        return reinterpret_cast<void*>(static_cast<uintptr_t>(t));
    }
    void updateTex(void* tex, int x, int y, int w, int h, const uint8_t* bgra) override {
        UploadTexSub(static_cast<GLuint>(reinterpret_cast<uintptr_t>(tex)), x, y, w, h, bgra);
    }
    void destroyTex(void* tex) override {
        GLuint t = static_cast<GLuint>(reinterpret_cast<uintptr_t>(tex));
        if (t) glDeleteTextures(1, &t);
    }
};

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
    // 顶点缓冲一次 resize 到 kMaxVerts，之后每帧只复位 vcount、按指针偏移写：
    // std::vector 的 insert/push_back 每条图元都要做容量检查 + size 递增（v20 之前
    // 就是 insert(verts.end(), v, v+6)），features 一页 8400 个顶点，实测这一段
    // 占一帧录制时间的 ~1/5。
    std::vector<GLVertex> verts;
    size_t vcount = 0;                 // 本帧已写顶点数
    std::vector<Batch> batches;
    // 记住的是**下标**不是指针：batches 是 vector，push_back 扩容会让 &back() 悬空，
    // 之后 beginBatch 里的比较读的是已释放内存、count++ 写的是野指针 —— 表现为
    // "某一块区域被涂成一块纯色方块"，且只在元素数跨过扩容阈值时出现（见 appshow 泡泡池）。
    size_t curBatch = static_cast<size_t>(-1);
    int scissor[4] = { 0, 0, 0, 0 };
    int scissorDepth = 0;
    GLuint curTex = 0;

    // 字形图集 + 文本排版：整份逻辑在 backends/binui_glyph_atlas.h（与 Vulkan 后端共用）。
    // 后端只提供纹理宿主与四边形发射 —— 两个 GPU 后端的文字因此是同一份像素。
    GLGlyphTexHost glyphHost;
    GlyphAtlas atlas;

    std::unordered_map<int, RoundTex> rounds;
    std::vector<GLuint> pendingTexDelete;   // 帧内淘汰的圆角模板：flushAll 之后才真删
    RoundTex emptyRound_;   // roundFor 失败时的空返回（调用方据此退回 fillRect）
    uint64_t roundClock_ = 0;   // 圆角模板缓存 LRU 时钟

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

    // 绑回自己的上下文。WGL 的 current context 是**线程级**的：同一线程里有两个
    // 后端实例（嵌多个面板、开多个窗口）时，谁最后 init 谁一直占着 —— 不逐帧切回来，
    // 绘制就全画到另一个实例身上，自己那块显示成黑的。
    bool makeCurrent() {
        if (!hdc || !ctx) return false;
        if (wglGetCurrentContext() == ctx) return true;
        return wglMakeCurrent(hdc, ctx) != FALSE;
    }

    bool createContext() {
        hdc = GetDC(hwnd);        if (!hdc) {
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


    // 圆角模板（九宫格）：把"圆弧紧贴边"的圆角矩形光栅化到临时 DIB，再传成纹理。
    // 模板边长 RoundTemplateSize(r) = 2r+4：角片只采样 [0,r]（圆弧的包围盒），
    // 末尾 4px 是直边段，供九宫格中段取一个实心像素。
    RoundTex& roundFor(int radius, bool stroke) {
        int r = std::max(1, radius);
        int key = stroke ? -r : r;
        auto it = rounds.find(key);
        if (it != rounds.end()) {
            it->second.lastUse = ++roundClock_;
            return it->second;
        }

        int t = RoundTemplateSize(r);
        // ensureRasterDC 失败时 rasterBits 还是**上一个尺寸**的缓冲，
        // 按新尺寸 memset 会踩坏堆（这里以前没判返回值）
        if (!ensureRasterDC(t, t)) {
            std::fprintf(stderr, "[gl] roundFor: 光栅化缓冲分配失败 (%dx%d)\n", t, t);
            return emptyRound_;
        }
        memset(rasterBits, 0, static_cast<size_t>(t) * t * 4);

        // 解析式逐像素覆盖率（1px 抗锯齿边缘），替代 GDI RoundRect（无 AA，转角锯齿明显）。
        // 形状铺满整个模板 [0,t]（不再内缩 1px）：这样圆弧的包围盒正好是 [0,r]，
        // 角片边长与弧长严格对应 —— 半径到短边一半时四角才能无缝拼成圆/胶囊。
        // 填充 = 内部，描边 = 1px 带。
        {
            uint32_t* px = static_cast<uint32_t*>(rasterBits);
            const float half = static_cast<float>(t) / 2.0f;      // 形状半边长
            const float inset = half - static_cast<float>(r);     // 直边段起点距中心
            const float c0 = half;                                // 模板中心
            for (int i = 0; i < t; i++) {
                for (int j = 0; j < t; j++) {
                    float fx = fabsf(j + 0.5f - c0) - inset;
                    float fy = fabsf(i + 0.5f - c0) - inset;
                    float dx = fx > 0 ? fx : 0.0f;
                    float dy = fy > 0 ? fy : 0.0f;
                    float dist = sqrtf(dx * dx + dy * dy) - static_cast<float>(r);
                    // 填充 = 内部（边界处 1px 抗锯齿）；描边 = 贴着**外侧边界往里**
                    // 1px 的实心带。
                    //
                    // 以前描边用的是 1-|dist|，环线是"骑在边界上"的：靠内的那半个像素
                    // 只有 50% 覆盖率，于是白色的焦点环画出来是一圈灰线（实测 #858990）。
                    // 现在把带子整体挪到内侧（dist ∈ (-1,0) 满覆盖），并且不再有外侧
                    // 那一半 —— 边框/焦点环的颜色就和作者写的一致了。
                    float cov = stroke ? std::min(0.5f - dist, dist + 1.5f) : 0.5f - dist;
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
        rt.tex = CreateTexture2D(t, t, rasterBits, true);
        if (!rt.tex) return emptyRound_;

        // 缓存上限：半径种类一多（动效 / 多主题）会一路涨显存。超了按 **LRU** 淘汰：
        // 以前淘汰"最大的一张"，可大的那张往往正是最热的主卡片圆角。
        rt.lastUse = ++roundClock_;
        if (rounds.size() >= 48) {
            auto victim = rounds.begin();
            for (auto k = rounds.begin(); k != rounds.end(); ++k)
                if (k->second.lastUse < victim->second.lastUse) victim = k;
            // 不能当场 glDeleteTextures：本帧**已经录进批次**的四边形还引用着这个纹理名
            // （圆角九宫格每片都记着 tex）。帧内删除 + 新模板复用了同一个 GL 名字，会让
            // 先前那些片采样到别人的内容 —— 实测表现是"某颗圆角盒子/圆环被整块涂成一个
            // 纯色圆"，且只在半径种类数跨过 48 时出现（appshow 泡泡池一页 >48 种半径）。
            // 统一拖到 flushAll 之后删。
            if (victim->second.tex) pendingTexDelete.push_back(victim->second.tex);
            rounds.erase(victim);
        }
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
    // 本帧顶点写入口：装不下就扩容（翻倍，上限 kMaxVertsCeiling），
    // 只有撞到硬上限才返回 nullptr（并且只告警一次）。
    GLVertex* vertRoom(size_t n) {
        if (vcount + n > verts.size()) {
            size_t cap = verts.size() ? verts.size() : kMaxVerts;
            while (cap < vcount + n && cap < kMaxVertsCeiling) cap *= 2;
            if (cap > kMaxVertsCeiling) cap = kMaxVertsCeiling;
            if (vcount + n > cap) { warnVertsOverflow(); return nullptr; }
            verts.resize(cap);
        }
        return verts.data() + vcount;
    }

    static void warnVertsOverflow() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr,
                     "[gl] 顶点数撞上硬上限（%zu 个四边形/帧），后面的图元被丢弃 —— "
                     "同屏内容太多，见 docs/future.md P0-5\n", kMaxVertsCeiling / 6);
    }

    void beginBatch() {
        if (curBatch != static_cast<size_t>(-1) &&
            batches[curBatch].sx == scissor[0] && batches[curBatch].sy == scissor[1] &&
            batches[curBatch].sw == scissor[2] - scissor[0] &&
            batches[curBatch].sh == scissor[3] - scissor[1] && batches[curBatch].tex == curTex)
            return;
        Batch b;
        b.first = static_cast<uint32_t>(vcount);
        b.count = 0;
        b.sx = scissor[0];
        b.sy = scissor[1];
        b.sw = scissor[2] - scissor[0];
        b.sh = scissor[3] - scissor[1];
        b.tex = curTex;
        batches.push_back(b);
        curBatch = batches.size() - 1;
    }

    void emitQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, uint32_t color) {
        GLVertex* o = vertRoom(6);
        if (!o) return;
        beginBatch();
        // 0xAARRGGBB → 0xAABBGGRR：UNSIGNED_BYTE 属性按内存字节序读（byte0=R）
        const uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        o[0] = GLVertex{ x0, y0, sw, u0, v0 };
        o[1] = GLVertex{ x1, y0, sw, u1, v0 };
        o[2] = GLVertex{ x0, y1, sw, u0, v1 };
        o[3] = o[2];
        o[4] = o[1];
        o[5] = GLVertex{ x1, y1, sw, u1, v1 };
        vcount += 6;
        batches[curBatch].count++;
    }

    // 任意四边形 + 每角 UV（旋转/斜切用）。p = 4 个角 ×2：0=左上 1=右上 2=左下 3=右下，
    // uv 同序 —— 与 emitQuad 的三角形划分（0,1,2 / 2,1,3）一致，保证绕序不变。
    void emitQuadUV(const float* p, const float* uv, uint32_t color) {
        GLVertex* o = vertRoom(6);
        if (!o) return;
        beginBatch();
        const uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        o[0] = GLVertex{ p[0], p[1], sw, uv[0], uv[1] };
        o[1] = GLVertex{ p[2], p[3], sw, uv[2], uv[3] };
        o[2] = GLVertex{ p[4], p[5], sw, uv[4], uv[5] };
        o[3] = o[2];
        o[4] = o[1];
        o[5] = GLVertex{ p[6], p[7], sw, uv[6], uv[7] };
        vcount += 6;
        batches[curBatch].count++;
    }

    // 任意旋转四边形（三角形带，白纹理；UV 无意义）
    void emitQuadRot(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3, uint32_t color) {
        GLVertex* o = vertRoom(6);
        if (!o) return;
        beginBatch();
        const uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        o[0] = GLVertex{ x0, y0, sw, 0, 0 };
        o[1] = GLVertex{ x1, y1, sw, 1, 0 };
        o[2] = GLVertex{ x2, y2, sw, 1, 1 };
        o[3] = o[2];
        o[4] = GLVertex{ x3, y3, sw, 0, 1 };
        o[5] = o[0];
        vcount += 6;
        batches[curBatch].count++;
    }

    void flushAll() {
        if (vcount == 0 || !vb) return;
        glBufferDataP(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vcount * sizeof(GLVertex)),
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
        // 本帧真正用完了，淘汰的模板现在可以删（见 roundFor 里的淘汰注释）
        for (GLuint t : pendingTexDelete) glDeleteTextures(1, &t);
        pendingTexDelete.clear();
    }

    void shutdown() {
        // 先放图集纹理：销毁上下文之后再 glDeleteTextures 是没有意义的
        atlas.clear();
        atlas.setHost(nullptr);
        if (ctx) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(ctx);
            ctx = nullptr;
        }
        if (hdc) { ReleaseDC(hwnd, hdc); hdc = nullptr; }
        for (GLuint t : pendingTexDelete) glDeleteTextures(1, &t);
        pendingTexDelete.clear();
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
    impl_->atlas.setHost(&impl_->glyphHost);
    impl_->atlas.initFonts();
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
    if (!impl_->makeCurrent()) return;   // 多实例：逐帧切回自己的上下文
    // 只复位计数，不 clear()：缓冲已经预分配到 kMaxVerts，clear() 每次会把
    // 2 MB 全部析构再由 vertRoom 重新 resize 一遍（白白 memset 一次）。
    impl_->vcount = 0;
    impl_->batches.clear();
    impl_->curBatch = static_cast<size_t>(-1);
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
    if (!impl_->makeCurrent()) return;
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

// present() 会不会自己阻塞到垂直同步：只有"真走 SwapBuffers 且 swap interval = 1"才算。
// 透明窗口走 UpdateLayeredWindow（不阻塞）、没拿到 wglSwapIntervalEXT 时也不阻塞 ——
// 这两种情况必须返回 false，否则外壳不掐帧就会满速空转。
bool BinUIGLGdi::presentPacesFrames() const {
    return impl_->ready && impl_->vsync && !impl_->transparent && wglSwapIntervalEXT != nullptr;
}

void BinUIGLGdi::presentViaGdi() {
    if (!impl_->ready) return;
    const bool perfOn = Perf().on;
    const double c0 = perfOn ? NowMs() : 0.0;
    const bool cur = impl_->makeCurrent();
    if (perfOn) Perf().sumCtx += NowMs() - c0;
    if (!cur) return;
    const double p0 = perfOn ? NowMs() : 0.0;
    impl_->flushAll();
    glBindFramebufferP(GL_READ_FRAMEBUFFER, impl_->msFbo);
    glBindFramebufferP(GL_DRAW_FRAMEBUFFER, impl_->offFbo);
    glScissor(0, 0, impl_->winW, impl_->winH);
    glBlitFramebufferP(0, 0, impl_->winW, impl_->winH,
                       0, 0, impl_->winW, impl_->winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    const double p1 = perfOn ? NowMs() : 0.0;
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
    const double p2 = perfOn ? NowMs() : 0.0;
    if (!impl_->layeredDib || !impl_->layeredBits) {
        glBindFramebufferP(GL_FRAMEBUFFER, 0);
        return;
    }
    glBindFramebufferP(GL_FRAMEBUFFER, impl_->offFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const double t0 = perfOn ? NowMs() : 0.0;
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, static_cast<uint8_t*>(impl_->layeredBits));
    glBindFramebufferP(GL_FRAMEBUFFER, 0);
    const double t1 = perfOn ? NowMs() : 0.0;
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
    // 1:1 搬运用 SetDIBitsToDevice（不做缩放/拉伸），比 StretchDIBits 少一遍采样
    SetDIBitsToDevice(wdc, 0, 0, w, h, 0, 0, 0, h,
                      impl_->layeredBits, &bi, DIB_RGB_COLORS);
    ReleaseDC(impl_->hwnd, wdc);
    if (perfOn) {
        const double t2 = NowMs();
        Perf().sumPrep += p1 - p0;
        Perf().sumDib += p2 - p1;
        Perf().sumRead += t1 - t0;
        Perf().sumBlit += t2 - t1;
    }
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

// 仿射圆角矩形（v20）：把局部坐标系里的九宫格每片按 xf 变换后发射。
// 与 fillRounded / strokeRounded 用**同一套**模板与九宫格几何，所以"不旋的时候"
// 与轴对齐版本逐像素一致（静态路径根本没走这里，这里只服务旋转/非等比缩放）。
void BinUIGLGdi::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                               uint32_t color, const float xf[6]) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    const int layers = stroke ? std::max(1, static_cast<int>(std::lround(std::max(1.0f, thickness))))
                              : 1;
    // 圆环模板是"1px 环"，多层同心 = 每层往里缩 1px、半径减 1（与 strokeRounded 一致）
    Premult(color);
    int emitted = 0;
    // 诊断（BINUI_XF_DUMP=1）：把前两次调用的九宫格分格与 UV 打出来。仿射路径"角看起来
    // 不对"时，先看这里的 local/uv/screen 三组数是否自洽（格边共享、UV 镜像、1:1 采样），
    // 再去怀疑模板或采样器。
    static int dumpLeft = getenv("BINUI_XF_DUMP") ? 2 : 0;
    for (int k = 0; k < layers; k++) {
        const float lx = static_cast<float>(k), ly = static_cast<float>(k);
        const float lw = w - 2.0f * k, lh = h - 2.0f * k;
        if (lw <= 1.0f || lh <= 1.0f) break;
        const float rk = rf - static_cast<float>(k);
        if (rk <= 0.0f) break;
        const int rri = std::max(1, static_cast<int>(std::lround(rk)));
        RoundTex& rt = impl_->roundFor(rri, stroke);
        if (!rt.tex) break;
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(lx, ly, lw, lh, rk, rt.t, q, 9);
        impl_->curTex = rt.tex;
        for (int i = 0; i < n; i++) {
            const PaintQuad& c = q[i];
            const float p[8] = { c.x, c.y, c.x + c.w, c.y, c.x, c.y + c.h, c.x + c.w, c.y + c.h };
            const float uv[8] = { c.u0, c.v0, c.u1, c.v0, c.u0, c.v1, c.u1, c.v1 };
            float sp[8];
            for (int v = 0; v < 4; v++) {
                const float lxp = p[v * 2], lyp = p[v * 2 + 1];
                sp[v * 2]     = xf[0] * lxp + xf[1] * lyp + xf[2];
                sp[v * 2 + 1] = xf[3] * lxp + xf[4] * lyp + xf[5];
            }
            impl_->emitQuadUV(sp, uv, color);
            emitted++;
            if (dumpLeft > 0) {
                std::fprintf(stderr,
                             "[xf] w=%.2f h=%.2f r=%.2f t=%d rri=%d cell=%d "
                             "local=(%.2f,%.2f %.2fx%.2f) uv=(%.3f,%.3f)-(%.3f,%.3f) "
                             "screen=(%.2f,%.2f)(%.2f,%.2f)(%.2f,%.2f)(%.2f,%.2f)\n",
                             (double)w, (double)h, (double)rf, rt.t, rri, i,
                             (double)c.x, (double)c.y, (double)c.w, (double)c.h,
                             (double)c.u0, (double)c.v0, (double)c.u1, (double)c.v1,
                             (double)sp[0], (double)sp[1], (double)sp[2], (double)sp[3],
                             (double)sp[4], (double)sp[5], (double)sp[6], (double)sp[7]);
            }
        }
    }
    impl_->curTex = impl_->whiteTex;
    if (dumpLeft > 0) { dumpLeft--; }
    if (emitted == 0) {
        // 模板不可用：至少把变换后的外框画出来，别让节点凭空消失
        const float p[8] = { 0, 0, w, 0, 0, h, w, h };
        const float uv[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        float sp[8];
        for (int v = 0; v < 4; v++) {
            const float lxp = p[v * 2], lyp = p[v * 2 + 1];
            sp[v * 2]     = xf[0] * lxp + xf[1] * lyp + xf[2];
            sp[v * 2 + 1] = xf[3] * lxp + xf[4] * lyp + xf[5];
        }
        impl_->emitQuadUV(sp, uv, color);
    }
}

void BinUIGLGdi::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    // 半径钳到短边一半：再大就是圆/胶囊，多出来的部分没有几何意义。
    // v0.11.7：半径的**几何**用浮点（缩放动画里半径要跟 w/h 一起连续变化），
    // 只有"角片模板"仍取最近的整数半径 —— 模板是位图，只能按整数建。
    // 静态页面传进来的本来就是整数，两种写法逐像素等价。
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) { fillRect(x, y, w, h, color); return; }

    const int r = std::max(1, static_cast<int>(std::lround(rf)));
    RoundTex& rt = impl_->roundFor(r, false);
    if (!rt.tex) { fillRect(x, y, w, h, color); return; }

    // 九宫格拆分走共用几何（include/binui_paint.h）：角片边长 = r，
    // 中段宽高按 max(0, …) 跳过，所以 r == 短边一半时四角正好拼成圆/胶囊。
    PaintQuad q[9];
    const int n = BuildRoundedNineSlice(x, y, w, h, rf, rt.t, q, 9);
    if (n <= 0) { fillRect(x, y, w, h, color); return; }

    Premult(color);
    impl_->curTex = rt.tex;
    for (int i = 0; i < n; i++)
        impl_->emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                        q[i].u0, q[i].v0, q[i].u1, q[i].v1, color);
    impl_->curTex = impl_->whiteTex;
}

void BinUIGLGdi::strokeRounded(float x, float y, float w, float h, float radius,
                            float thickness, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    // v0.11.8：几何半径用浮点（缩放动画要连续），模板按最近整数取
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) { fillRect(x, y, w, h, color); return; }

    // 线宽：以前这里 (void)thickness 把参数丢了，所有描边都是 1px（"2px 编辑环"实际
    // 只有 1px）。stroke 纹理本身就是 1px 的环，所以按厚度画多层同心圆角环叠出粗描边：
    // 每往里一层缩 1px、半径减 1。thickness<=1 时只有一层，和旧行为一致。
    int layers = static_cast<int>(std::lround(std::max(1.0f, thickness)));
    if (layers < 1) layers = 1;
    Premult(color);
    for (int k = 0; k < layers; k++) {
        const float ex = x + k, ey = y + k;
        const float ew = w - 2.0f * k, eh = h - 2.0f * k;
        if (ew <= 1.0f || eh <= 1.0f) break;
        const float rfk = rf - static_cast<float>(k);
        if (rfk <= 0.0f) break;
        const int rr = std::max(1, static_cast<int>(std::lround(rfk)));
        RoundTex& rt = impl_->roundFor(rr, true);
        if (!rt.tex) break;
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(ex, ey, ew, eh, rfk, rt.t, q, 9);
        if (n <= 0) break;
        impl_->curTex = rt.tex;
        for (int i = 0; i < n; i++)
            impl_->emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                            q[i].u0, q[i].v0, q[i].u1, q[i].v1, color);
    }
    impl_->curTex = impl_->whiteTex;
}

void BinUIGLGdi::setTextSubpixel(bool on) {
    // 图集纹理本身已经是 LINEAR（见 GLGlyphTexHost::createTex），这里只切"要不要取整"
    if (impl_) impl_->atlas.snapText = !on;
}

float BinUIGLGdi::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->ready) return 0;
    return impl_->atlas.textWidth(size, weight, utf8, italic);
}

void BinUIGLGdi::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    impl_->atlas.fontMetrics(size, weight, ascent, lineHeight, italic);
}

// 文本排版/光栅化/图集全在 binui::GlyphAtlas 里（与 Vulkan 后端同一份代码），
// 这里只把回调给出的字形四边形丢进本后端的顶点批次。
void BinUIGLGdi::drawText(float x, float y, float size, int weight,
                       const std::string& utf8, uint32_t color,
                       bool centerH, bool centerV, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    impl_->curTex = impl_->whiteTex;
    impl_->atlas.drawText(x, y, size, weight, utf8, color, centerH, centerV, italic,
                          [this](const GlyphQuad& q) {
                              impl_->curTex = static_cast<GLuint>(
                                  reinterpret_cast<uintptr_t>(q.tex));
                              impl_->emitQuad(q.x, q.y, q.x + q.w, q.y + q.h,
                                              q.u0, q.v0, q.u1, q.v1, q.color);
                          });
    impl_->curTex = impl_->whiteTex;
}

void BinUIGLGdi::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                              const std::string& utf8, uint32_t color, int align, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    impl_->curTex = impl_->whiteTex;
    impl_->atlas.drawTextWrapped(x, y, maxW, size, weight, utf8, color, align, italic,
                                 [this](const GlyphQuad& q) {
                                     impl_->curTex = static_cast<GLuint>(
                                         reinterpret_cast<uintptr_t>(q.tex));
                                     impl_->emitQuad(q.x, q.y, q.x + q.w, q.y + q.h,
                                                     q.u0, q.v0, q.u1, q.v1, q.color);
                                 });
    impl_->curTex = impl_->whiteTex;
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
    impl_->atlas.setFontName(name);
}

namespace binui {

// v19：节点树遍历已抽到 binui_draw_nodes.h（后端无关）。这里只剩一个薄壳，
// 保持既有调用点（demo / tools）不变；CanvasDrawFn 收到的是具体后端类型，
// 进共享遍历前转成 IBinUIGfx&。
void drawNodesGLGdi(BinUIGLGdi& gfx, const BinUIRenderer& renderer, const void* clipRect,
                    const CanvasDrawFn& canvas) {
    CanvasFn shared;
    if (canvas) {
        shared = [&canvas](IBinUIGfx& g, uint16_t id, float x, float y, float w, float h) {
            canvas(static_cast<BinUIGLGdi&>(g), id, x, y, w, h);
        };
    }
    drawNodesBinUI(gfx, renderer, clipRect, shared);
}



} // namespace binui
