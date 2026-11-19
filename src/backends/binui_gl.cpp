// binui_gl.cpp - OpenGL GPU 渲染后端实现
// 架构与 binui_d3d11.cpp 完全同构：字形图集（FreeType 光栅化一次）+ 圆角九宫格模板 +
// 顶点批次；drawNodesGL 与 D3D 后端语义一致地遍历节点树，全部绘制为 GPU 四边形；
// BinUIAppGL 提供无边框窗口与事件循环（VSync 按需呈现）。
// OpenGL 3.3 core（WGL 创建上下文 + GLSL 330），着色器数学与 D3D11 HLSL 版一致：
// 预乘 alpha 混合、纹理 × 顶点色、NDC 映射 viewSize。
// 文字渲染委托 binui_font_renderer（FreeType + HarfBuzz），与平台解耦。
#include "backends/binui_gl.h"
#include "binui_paths.h"   // binui::ex::StartupNowMs / StartupMark（BINUI_STARTUP 启动分段）

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
#include "binui_filter.h"      // v26：CSS filter 颜色矩阵（解码后逐像素应用）
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
PFNGLGENERATEMIPMAPPROC glGenerateMipmapP = nullptr;   // v0.16.15：图片 mip

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
    LOAD_PROC(glGenerateMipmapP, "glGenerateMipmap", PFNGLGENERATEMIPMAPPROC);
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

struct BinUIGL::Impl {
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

    // ---- 图层（v0.16.9）：组透明度的离屏合成 ----
    // 每层一块**窗口大小**的 RGBA 纹理 + FBO：坐标不用换算、UV 直接是像素坐标 ÷ 窗口尺寸，
    // 代价是每层一张整窗纹理（1280×720 ≈ 3.7 MB），所以池子限 4 层，超了就让调用方退回
    // 逐元素乘 alpha。底色/边框/文字/图片都走同一套 quad，所以这一层不改任何绘制代码。
    struct LayerTarget { GLuint tex = 0, fbo = 0; bool inUse = false; };
    // v0.16.42：blur > 0 的层在 pop 时先把自己糊掉再合成（见 blurLayerContent）。
    struct LayerFrame {
        int idx = -1;
        float x = 0, y = 0, w = 0, h = 0;
        float alpha = 1;
        float blur = 0;
        std::vector<BinUIFilterDropShadowEntry> shadows;   // v0.16.43：外阴影（书写顺序）
        GLint prevFbo = 0;
    };
    std::vector<LayerTarget> layers;
    std::vector<LayerFrame> layerStack;
    static constexpr int kMaxLayers = 4;

    static void warnLayerLimit() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr, "[gl] 图层超过 %d 层，多出来的退回逐元素乘 alpha（减少嵌套即可）\n",
                     kMaxLayers);
    }

    int acquireLayer() {
        for (size_t i = 0; i < layers.size(); i++)
            if (!layers[i].inUse) return static_cast<int>(i);
        if (static_cast<int>(layers.size()) >= kMaxLayers) return -1;
        LayerTarget t;
        t.tex = CreateTexture2D(winW, winH, nullptr, true);
        if (!t.tex) return -1;
        glGenFramebuffersP(1, &t.fbo);
        bindFbo(t.fbo);
        glFramebufferTexture2DP(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        layers.push_back(t);
        return static_cast<int>(layers.size() - 1);
    }

    // ---- v0.16.35：自己维护"当前绑定的 FBO"，不再用 glGetIntegerv 查询 ----
    // glGetIntegerv(GL_FRAMEBUFFER_BINDING) 是一次**同步查询**：驱动必须把之前排队的
    // 命令处理到"当前状态确定"才能回答，等于把 GPU 流水线 drain 一次。它原来出现在
    // pushLayer / popLayer / 毛玻璃三处，每帧每个图层一次 —— 实测 4 个 opacity 组的
    // 页面：绘制 0.097 ms/帧，其中一大半是这两个查询（改成跟踪后 0.018 ms）。
    // 规矩：所有改绑定的地方都走 bindFbo()，不要直接调 glBindFramebufferP。
    GLuint curFbo = 0;
    void bindFbo(GLuint f) {
        glBindFramebufferP(GL_FRAMEBUFFER, f);
        curFbo = f;
    }

    bool pushLayer(float x, float y, float w, float h, float alpha, float blur = 0.0f,
                   const BinUIFilterDropShadowEntry* shadows = nullptr, int shadowCount = 0) {
        if (!ready || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return false;   // v27：alpha=1 也允许（裁剪用的纯容器层）
        if (static_cast<int>(layerStack.size()) >= kMaxLayers) { warnLayerLimit(); return false; }
        // 顺序很重要：先把"当前目标"和手头的图元收拾干净，再去碰层缓冲 ——
        // acquireLayer 会顺手把新 FBO 绑上，先 flush 的话那些图元会画进空层里再被清掉。
        const GLuint prevFbo = curFbo;
        flushAll();
        const int idx = acquireLayer();
        if (idx < 0) { warnLayerLimit(); return false; }
        LayerTarget& t = layers[idx];
        t.inUse = true;
        LayerFrame f;
        f.idx = idx; f.x = x; f.y = y; f.w = w; f.h = h; f.alpha = alpha; f.blur = blur;
        if (shadows && shadowCount > 0)
            f.shadows.assign(shadows, shadows + shadowCount);
        f.prevFbo = prevFbo;
        bindFbo(t.fbo);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        layerStack.push_back(f);
        return true;
    }

    // ---- v29 毛玻璃：降采样链（每级 1/2，按需分配；窗口尺寸不变就复用）----
    struct BlurLevel {
        GLuint tex = 0;
        GLuint fbo = 0;
        int w = 0, h = 0;
    };
    // v0.17.3：降采样链按尺寸池化。
    // 以前只有"一套"链，第 0 级尺寸由第一个毛玻璃/模糊节点决定；同帧出现第二个不同尺寸
    // 的节点就把整条链（8 级纹理 + FBO）删了重建。而"大玻璃卡 + 小玻璃按钮 + 模糊浮层"
    // 三种尺寸同页很常见 —— 每帧几十次纹理创建/销毁，掉的是帧不是像素。
    struct BlurSet {
        int w0 = 0, h0 = 0;          // 第 0 级（抓取尺寸）
        int tick = 0;                // 最近使用顺序（淘汰用）
        std::vector<BlurLevel> lv;   // 0..5 降采样 + 7 暂存
    };
    std::vector<BlurSet> blurSets;
    int activeBlur = -1;
    int blurTick = 0;
    static constexpr size_t kMaxBlurSets = 16;

    // v0.16.28：毛玻璃的颗粒（grain）—— 64×64 可平铺噪声，确定性 LCG 生成。
    // 采样用 NEAREST + REPEAT，并且按"一个噪声纹素 = 一个屏幕像素"打，
    // 于是三端取到的是**同一个纹素**，一致不依赖各家的双线性精度。
    GLuint noiseTex = 0;
    GLuint ensureNoiseTex() {
        if (noiseTex) return noiseTex;
        const int N = 64;
        std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
        uint32_t st = 0x9E3779B9u;   // 固定种子：三端同一张噪声
        for (int i = 0; i < N * N; i++) {
            st = st * 1664525u + 1013904223u;
            const uint8_t a = static_cast<uint8_t>((st >> 16) & 0xFFu);
            const size_t o = static_cast<size_t>(i) * 4;
            px[o + 0] = a;   // 预乘白：rgb 跟着 alpha 走
            px[o + 1] = a;
            px[o + 2] = a;
            px[o + 3] = a;
        }
        noiseTex = CreateTexture2D(N, N, px.data(), false);   // false = NEAREST
        if (noiseTex) {
            glBindTexture(GL_TEXTURE_2D, noiseTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D, whiteTex);
        }
        return noiseTex;
    }
    // 降采样趟把图元画进小 FBO 时，坐标是"小图局部"的，不能再用层框去裁
    bool ignoreLayerClip = false;
    // 诊断（BINUI_GL_VERTS=1）：本帧上传了多少顶点 / 字节。P0-4「脏节点上传」的账必须用
    // 实测数据算 —— bench-draw 的"1 个原语 ≈ 1 个四边形"是估算，看不到描边自 v0.17.1
    // 起走 CPU 条带后被拆成 2~8 个四边形这一层。
    size_t frameVerts = 0;

    // 取一套以 (w0,h0) 为第 0 级的链：命中复用，否则新建（超过 kMaxBlurSets 淘汰最久未用）。
    bool selectBlurSet(int w0, int h0) {
        if (w0 <= 0 || h0 <= 0) return false;
        for (size_t i = 0; i < blurSets.size(); i++) {
            if (blurSets[i].w0 == w0 && blurSets[i].h0 == h0) {
                blurSets[i].tick = ++blurTick;
                activeBlur = static_cast<int>(i);
                return true;
            }
        }
        if (blurSets.size() >= kMaxBlurSets) {
            size_t victim = 0;
            for (size_t i = 1; i < blurSets.size(); i++)
                if (blurSets[i].tick < blurSets[victim].tick) victim = i;
            destroyBlurSet(victim);
            blurSets.erase(blurSets.begin() + static_cast<long>(victim));
        }
        BlurSet s;
        s.w0 = w0;
        s.h0 = h0;
        s.tick = ++blurTick;
        s.lv.resize(8);
        blurSets.push_back(std::move(s));
        activeBlur = static_cast<int>(blurSets.size()) - 1;
        return true;
    }
    BlurLevel& blurAt(size_t i) {
        static BlurLevel dummy;
        if (activeBlur < 0) return dummy;
        std::vector<BlurLevel>& lv = blurSets[static_cast<size_t>(activeBlur)].lv;
        if (i >= lv.size()) { dummy = BlurLevel(); return dummy; }
        return lv[i];
    }
    void destroyBlurSet(size_t idx) {
        if (idx >= blurSets.size()) return;
        for (BlurLevel& b : blurSets[idx].lv) {
            if (b.fbo) { glDeleteFramebuffersP(1, &b.fbo); b.fbo = 0; }
            if (b.tex) { glDeleteTextures(1, &b.tex); b.tex = 0; }
        }
    }
    void destroyAllBlurSets() {
        for (size_t i = 0; i < blurSets.size(); i++) destroyBlurSet(i);
        blurSets.clear();
        activeBlur = -1;
    }

    bool ensureBlurLevel(size_t i, int w, int h) {
        if (i >= 8 || w <= 0 || h <= 0 || activeBlur < 0) return false;
        BlurLevel& b = blurAt(i);
        if (b.tex && b.fbo && b.w == w && b.h == h) return true;
        if (b.fbo) { glDeleteFramebuffersP(1, &b.fbo); b.fbo = 0; }
        if (b.tex) { glDeleteTextures(1, &b.tex); b.tex = 0; }
        static const bool traceLvl = std::getenv("BINUI_BLUR_TRACE") != nullptr;
        if (traceLvl) std::fprintf(stderr, "[gl][blurlevel] create %zu %dx%d\n", i, w, h);
        b.tex = CreateTexture2D(w, h, nullptr, true);
        if (!b.tex) return false;
        glGenFramebuffersP(1, &b.fbo);
        if (!b.fbo) { glDeleteTextures(1, &b.tex); b.tex = 0; return false; }
        glBindFramebufferP(GL_FRAMEBUFFER, b.fbo);
        glFramebufferTexture2DP(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
        b.w = w;
        b.h = h;
        return true;
    }

    // 级数与抽头算法在 include/binui_paint.h（PaintFilterBlurLevel / PaintFilterGaussTaps）——
    // 三端与单元测试共用一份实现，这里只留别名，别再各写一遍（以前三份拷贝，改一处漏两处）。
    static int blurLevelsFor(float blurPx) { return binui::PaintFilterBlurLevel(blurPx); }
    static int gaussTaps(float sigmaTexels, uint32_t* w, int maxN) {
        return binui::PaintFilterGaussTaps(sigmaTexels, w, maxN);
    }

    bool gaussBlurLevel(int level, float blurPx) {
        if (level < 0 || level >= 7) return false;          // 7 号留给暂存
        if (activeBlur < 0) return false;
        // ★ 不要在这里抓 BlurLevel& 引用：下面的 ensureBlurLevel(7, ...) 可能重建链，
        // 引用会失效 —— 读到的 w/h 是已释放内存。下面一律先取标量。
        const int lw = blurAt(static_cast<size_t>(level)).w;
        const int lh = blurAt(static_cast<size_t>(level)).h;
        const bool hasTex = blurAt(static_cast<size_t>(level)).tex != 0;
        const float sigmaPx = blurPx / static_cast<float>(1 << level);
        if (!hasTex || lw < 1 || lh < 1 || !(sigmaPx > 0.0f)) return false;
        if (!ensureBlurLevel(7, lw, lh)) return false;
        uint32_t w[33];
        const int n = gaussTaps(sigmaPx, w, 33);
        const int r = (n - 1) / 2;
        static const bool trace = std::getenv("BINUI_BLUR_TRACE") != nullptr;
        if (trace)
            std::fprintf(stderr, "[gl][gauss] 级 %d %dx%d σ=%.2f 纹素 抽头 %d 个\n",
                         level, lw, lh, sigmaPx, n);
        glBlendFunc(GL_ONE, GL_ONE);
        ignoreLayerClip = true;
        for (int pass = 0; pass < 2; pass++) {
            BlurLevel& dst = blurAt(pass == 0 ? 7 : static_cast<size_t>(level));
            BlurLevel& from = blurAt(pass == 0 ? static_cast<size_t>(level) : 7);
            bindFbo(dst.fbo);
            glViewport(0, 0, lw, lh);
            glUniform2fP(uniView, static_cast<float>(lw), static_cast<float>(lh));
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            curTex = from.tex;
            for (int k = -r; k <= r; k++) {
                const float o = static_cast<float>(k);
                uint32_t c = (w[k + r] << 24) | 0x00FFFFFFu;
                Premult(c);
                if (pass == 0)   // 横向
                    emitQuad(0.0f, 0.0f, static_cast<float>(lw), static_cast<float>(lh),
                             o / static_cast<float>(lw), 1.0f,
                             1.0f + o / static_cast<float>(lw), 0.0f, c);
                else             // 纵向（v 轴翻转是 GL 的 FBO 行序，抽头对称所以不影响结果）
                    emitQuad(0.0f, 0.0f, static_cast<float>(lw), static_cast<float>(lh),
                             0.0f, 1.0f + o / static_cast<float>(lh),
                             1.0f, 0.0f + o / static_cast<float>(lh), c);
            }
            flushAll();
        }
        ignoreLayerClip = false;
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return true;
    }

    bool layerRegion(const LayerFrame& f, int& rx, int& ry, int& rw, int& rh) const {
        int x0 = static_cast<int>(std::floor(f.x));
        int y0 = static_cast<int>(std::floor(f.y));
        int x1 = static_cast<int>(std::ceil(f.x + f.w));
        int y1 = static_cast<int>(std::ceil(f.y + f.h));
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(winW, x1);
        y1 = std::min(winH, y1);
        rx = x0; ry = y0; rw = x1 - x0; rh = y1 - y0;
        return rw >= 2 && rh >= 2;
    }

    // 把 level0（已就绪）降到 blurLevelsFor(blurPx) 那一级、在那一级上跑分离高斯，
    // 再以正常 alpha 把 (rx,ry,rw,rh) 合成回 target。
    // 入口约定：scissor 已关、当前目标是 level0/层目标（这里会自己切回来）。
    void compositeThroughLevels(float blurPx, GLuint target, int rx, int ry, int rw, int rh,
                                uint32_t col) {
        const int levels = blurLevelsFor(blurPx);
        int finalLevel = 0;
        if (levels > 0) {
            glBlendFunc(GL_ONE, GL_ZERO);
            ignoreLayerClip = true;
            for (int i = 1; i <= levels; i++) {
                const int dw = std::max(1, blurAt(static_cast<size_t>(i - 1)).w / 2);
                const int dh = std::max(1, blurAt(static_cast<size_t>(i - 1)).h / 2);
                if (!ensureBlurLevel(static_cast<size_t>(i), dw, dh)) break;
                bindFbo(blurAt(static_cast<size_t>(i)).fbo);
                glViewport(0, 0, dw, dh);
                glUniform2fP(uniView, static_cast<float>(dw), static_cast<float>(dh));
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                curTex = blurAt(static_cast<size_t>(i - 1)).tex;
                emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                         0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
                flushAll();
                finalLevel = i;
            }
            ignoreLayerClip = false;
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        // 分离高斯：**每一级都要跑，包括 0 级**。
        // v0.16.44 起这句被关在 `if (levels > 0)` 里面，于是 blurPx < 4（levels=0）
        // 的滤镜一次卷积都没跑 —— `filter: blur(3px)` / `stdDeviation="3"` 在
        // GL/VK/D3D11 三端**完全没糊**，只是把抓下来的区域 1:1 拷了回去。
        // 0.27.2 修：判据是"有没有 σ"，不是"降了几级"。
        if (!gaussBlurLevel(finalLevel, blurPx)) finalLevel = levels;
        bindFbo(target);
        glViewport(0, 0, winW, winH);
        glUniform2fP(uniView, static_cast<float>(winW), static_cast<float>(winH));
        curTex = blurAt(static_cast<size_t>(finalLevel)).tex;
        emitQuad(static_cast<float>(rx), static_cast<float>(ry),
                 static_cast<float>(rx + rw), static_cast<float>(ry + rh),
                 0.0f, 1.0f, 1.0f, 0.0f, col);
        flushAll();
        curTex = whiteTex;
        glEnable(GL_SCISSOR_TEST);
    }

    // filter: blur —— 抓层内容 → 降采样链 → 正常 alpha 合成回原区域
    bool blurLayerContent(const LayerFrame& f, GLuint layerFbo, GLuint prevFbo) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!layerRegion(f, rx, ry, rw, rh)) return false;
        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) return false;
        static const bool traceBlur = std::getenv("BINUI_BLUR_TRACE") != nullptr;
        if (traceBlur)
            std::fprintf(stderr, "[gl][blur] 层框 %.2f,%.2f %.2fx%.2f 抓取 %d,%d %dx%d 级数 %d\n",
                         f.x, f.y, f.w, f.h, rx, ry, rw, rh, blurLevelsFor(f.blur));
        // 抓层内容 → level0（GL 的 FBO 行从下往上，世界 y 从上往下，翻转）
        glDisable(GL_SCISSOR_TEST);   // glBlitFramebuffer 受 scissor 裁剪
        glBindFramebufferP(GL_READ_FRAMEBUFFER, layerFbo);
        glBindFramebufferP(GL_DRAW_FRAMEBUFFER, blurAt(0).fbo);
        curFbo = blurAt(0).fbo;
        {
            const int by = winH - (ry + rh);
            glBlitFramebufferP(rx, by, rx + rw, by + rh, 0, 0, rw, rh,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        bindFbo(prevFbo);
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        uint32_t col = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        Premult(col);
        compositeThroughLevels(f.blur, prevFbo, rx, ry, rw, rh, col);
        return true;
    }

    // filter: drop-shadow —— 一条外阴影。
    // 没有着色器就取不到"把 RGB 压成单色、只留轮廓"的效果，所以用两趟混合凑出来：
    //   ① 阴影色（预乘）铺满 level0 的整块区域；
    //   ② "保留混合"（dst *= srcA）把层内容画一遍 → dst 的 RGB/alpha 都乘上层内容的 alpha，
    //      于是得到 (c.rgb·c.a·a, c.a·a)：**上色的轮廓**，形状来自元素自己的 alpha 通道
    //      （所以星形图标的影子是星形，不是方块 —— 这正是它跟 box-shadow 的本质区别）。
    //   ③ 按阴影半径降采样链，再按偏移（取整）合成到元素下面。
    void drawDropShadow(const LayerFrame& f, GLuint layerFbo, GLuint prevFbo,
                        const BinUIFilterDropShadowEntry& sh) {
        (void)layerFbo;   // 本趟在 blur 链里画，不用层的 FBO（保留参数是为了三端签名一致）
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!layerRegion(f, rx, ry, rw, rh)) return;
        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) return;
        glDisable(GL_SCISSOR_TEST);
        bindFbo(blurAt(0).fbo);
        glViewport(0, 0, rw, rh);
        glUniform2fP(uniView, static_cast<float>(rw), static_cast<float>(rh));
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ignoreLayerClip = true;      // 小图局部坐标：不按层框裁
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        {
            uint32_t c = sh.color;
            Premult(c);
            curTex = whiteTex;
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     0.5f, 0.5f, 0.5f, 0.5f, c);
            flushAll();
            // 轮廓：dst *= srcA（层纹理在原屏幕位置的那一块，1:1 对到 level0）
            glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
            curTex = layers[static_cast<size_t>(f.idx)].tex;
            const float u0 = static_cast<float>(rx) / static_cast<float>(winW);
            const float u1 = static_cast<float>(rx + rw) / static_cast<float>(winW);
            // 注意 V 的方向：层纹理的 row 0 在**下**（GL 的 FBO 行序），所以屏幕上方
            // （屏幕 y = ry）对应 v = 1 - ry/winH。**上下别写反** —— 写反了就是一个
            // 上下镜像的轮廓：对称元素看不出来，星形/文字这类不对称形状一眼就能看出
            // 「影子长在元素上方」。
            const float v0 = 1.0f - static_cast<float>(ry) / static_cast<float>(winH);
            const float v1 = 1.0f - static_cast<float>(ry + rh) / static_cast<float>(winH);
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     u0, v0, u1, v1, 0xFFFFFFFFu);
            flushAll();
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        ignoreLayerClip = false;
        // 偏移取整：合成四边形恒落在像素边界上，三端一致（小数偏移的视觉差可以忽略）
        const int sdx = static_cast<int>(std::lround(sh.dx));
        const int sdy = static_cast<int>(std::lround(sh.dy));
        if (std::getenv("BINUI_BLUR_TRACE"))
            std::fprintf(stderr, "[gl][shadow] region %d,%d %dx%d 级数 %d 偏移 %d,%d 层框 %.1f,%.1f %.1fx%.1f scissor %d,%d-%d,%d\n",
                         rx, ry, rw, rh, blurLevelsFor(sh.blur), sdx, sdy, f.x, f.y, f.w, f.h,
                         scissor[0], scissor[1], scissor[2], scissor[3]);
        compositeThroughLevels(sh.blur, prevFbo, rx + sdx, ry + sdy, rw, rh,
                               0xFFFFFFFFu);
    }

    void popLayer() {
        if (layerStack.empty()) return;
        flushAll();                       // 层内容落盘（flushAll 会按层框裁剪）
        const LayerFrame f = layerStack.back();
        layerStack.pop_back();
        LayerTarget& t = layers[static_cast<size_t>(f.idx)];
        // v0.16.43：外阴影先铺（书写顺序里靠前的在上层 → 逆序画），本体最后画在最上面
        for (int i = static_cast<int>(f.shadows.size()) - 1; i >= 0; i--)
            drawDropShadow(f, t.fbo, f.prevFbo, f.shadows[static_cast<size_t>(i)]);
        // v0.16.42：模糊层 —— 先把层内容糊掉并合成掉；成功就直接收工
        if (f.blur > 0.0f && blurLayerContent(f, t.fbo, f.prevFbo)) {
            curTex = whiteTex;
            t.inUse = false;
            return;
        }
        bindFbo(f.prevFbo);
        // 合成：把层纹理按 alpha 画回父目标。层里存的是**预乘**颜色（混合是 ONE/ONE_MINUS_SRC_ALPHA），
        // 着色器输出 t.rgb*vcol.rgb / t.a*vcol.a，所以 vcol 全给 alpha 就是"整组乘一次"。
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        const uint32_t av = static_cast<uint32_t>(a * 255.0f + 0.5f);
        uint32_t col = (av << 24) | 0x00FFFFFFu;
        // 着色器算的是 frag = (t.rgb * vcol.rgb, t.a * vcol.a)：不预乘的话 rgb 会被整块加进去
        // （蓝通道直接爆到 255），所以顶点色必须走和 fillRect 一样的预乘。
        Premult(col);
        curTex = t.tex;
        // V 轴要翻转：把同一套投影渲染进 FBO 时，纹理在内存里是"上下颠倒"的（GL 的纹理原点在
        // 左下，而屏幕坐标是 y 向下）。不翻的话层内容会整体倒过来。
        const float u0 = f.x / static_cast<float>(winW);
        const float u1 = (f.x + f.w) / static_cast<float>(winW);
        const float v0 = 1.0f - f.y / static_cast<float>(winH);              // 顶边
        const float v1 = 1.0f - (f.y + f.h) / static_cast<float>(winH);       // 底边
        emitQuad(f.x, f.y, f.x + f.w, f.y + f.h, u0, v0, u1, v1, col);
        curTex = whiteTex;
        flushAll();
        t.inUse = false;
    }

    // 字形图集 + 文本排版：整份逻辑在 backends/binui_glyph_atlas.h（与 Vulkan 后端共用）。
    // 后端只提供纹理宿主与四边形发射 —— 两个 GPU 后端的文字因此是同一份像素。
    // v0.16.18 仿射状态：设置后所有 emitQuad/emitQuadRot 的四边形都过这个 2×3 矩阵
    // （文字/图片因此能跟着卡片一起旋转）。fillRoundedXf 自带矩阵，走 emitQuadUV，
    // 不经过这里 —— 否则圆角底会被变换两次。
    bool affineOn = false;
    float aff[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    void affPoint(float lx, float ly, float& sx, float& sy) const {
        sx = aff[0] * lx + aff[1] * ly + aff[2];
        sy = aff[3] * lx + aff[4] * ly + aff[5];
    }
    // 轴对齐矩形 → 四角（0=左上 1=右上 2=左下 3=右下，与 emitQuadUV 的角序一致）
    void affCorners(float x0, float y0, float x1, float y1, float* p) const {
        affPoint(x0, y0, p[0], p[1]);
        affPoint(x1, y0, p[2], p[3]);
        affPoint(x0, y1, p[4], p[5]);
        affPoint(x1, y1, p[6], p[7]);
    }

    GLGlyphTexHost glyphHost;
    GlyphAtlas atlas;

    std::unordered_map<int, RoundTex> rounds;
    std::vector<GLuint> pendingTexDelete;   // 帧内淘汰的圆角模板：flushAll 之后才真删
    RoundTex emptyRound_;   // roundFor 失败时的空返回（调用方据此退回 fillRect）
    uint64_t roundClock_ = 0;   // 圆角模板缓存 LRU 时钟

    // v26：图片缓存键 = (imageIndex | 滤镜哈希 << 16) —— 同一张图配不同滤镜是两份纹理
    std::unordered_map<uint64_t, GLuint> images;
    // v25 九宫格要**源图像素尺寸**才切得开（UV 是归一化的，切分点得按像素算）
    std::unordered_map<uint64_t, std::pair<int,int>> imageDims;
    // v0.16.20：原始像素纹理（视频帧/程序化纹理）的 mip 是否已补（见 uploadImageRaw）
    std::unordered_map<uint64_t, uint8_t> rawMips;
    std::unordered_map<uint64_t, uint32_t> imageGen; // 换图 generation：不匹配时重新上传
    // v26：当前节点的图片滤镜（setImageFilter 设、解码时应用、并按哈希进缓存键）
    binui::ColorMatrix imgFilter;
    uint64_t imgFilterKey = 0;
    uint64_t imgKey(uint16_t index) const {
        return static_cast<uint64_t>(index) | (imgFilterKey << 16);
    }

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
    RoundTex& roundFor(int radius, bool stroke) { return roundForThick(radius, stroke ? 1 : 0); }
    // 模板键：thickness == 0 = 填充模板；> 0 = 该厚度的整圈环带模板。
    // 同一个半径下 1px / 2px / 3px 是三张不同的覆盖率位图，键里必须带厚度。
    RoundTex& roundForThick(int radius, int thickness) {
        int r = std::max(1, radius);
        int th = thickness < 0 ? 0 : thickness;
        if (th > r) th = r;                 // 带宽不能超过角片
        int key = th * 256 + r;
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

        // 模板内容来自 bin2d（v0.23.0 / v0.34.4）：三端以前各写一份同样的解析式距离循环，
        // 现在只有 bin2d 里那一份。**0.20.0 起那份内容不再是有符号距离场，而是
        // CPU 光栅自己的扫描线覆盖率** —— 于是 GPU 三端与 bin2d 的 CPU 光栅
        // 画出来的是同一批像素，不再是"两套看起来差不多的 AA"。
        const int built = (th == 0)
                              ? BuildRoundedTileRGBA(r, false, static_cast<uint32_t*>(rasterBits))
                              : BuildRoundedRingRGBA(r, th, static_cast<uint32_t*>(rasterBits));
        if (built != t) {
            std::fprintf(stderr, "[gl] roundFor: 圆角模板生成失败 (%dx%d)\n", t, t);
            return emptyRound_;
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
        auto it = images.find(imgKey(index));
        auto git = imageGen.find(imgKey(index));
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != 0;

        int w = 0, h = 0;
        std::vector<uint8_t> px = DecodeImagePixels(data, size, w, h);
        // v26：CSS filter 的颜色矩阵在解码后、上传前逐像素跑一遍（一次性开销）
        if (!px.empty() && imgFilterKey)
            binui::ApplyColorMatrixBGRA(px.data(), w, h, imgFilter);
        if (px.empty()) {
            images[imgKey(index)] = 0;
            imageGen[imgKey(index)] = generation;
            return false;
        }
        if (it != images.end() && it->second) glDeleteTextures(1, &it->second);
        images[imgKey(index)] = CreateTexture2D(w, h, px.data(), true);
    imageDims[imgKey(index)] = { w, h };    // v25：九宫格切分要用源图像素尺寸
        // v0.16.15（P1-8）：给图片生成 mip 链，并把缩小采样改成三线性。
        // 只做双线性时"大图缩小"只取最近的 4 个纹素 —— 一像素的屏幕区域要代表上百个
        // 源像素，远看会闪、摩尔纹；mip 让 GPU 自动挑合适的层级再插值。
        if (images[imgKey(index)] && glGenerateMipmapP) {
            glBindTexture(GL_TEXTURE_2D, images[imgKey(index)]);
            glGenerateMipmapP(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
        imageGen[imgKey(index)] = generation;
        return images[imgKey(index)] != 0;
    }

    // 原始 RGBA 像素直传纹理（无解码；RGBA 字节序）
    bool uploadImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
        auto it = images.find(imgKey(index));
        auto git = imageGen.find(imgKey(index));
        if (it != images.end() && git != imageGen.end() && git->second == generation) {
            // v0.16.20：**同一个 generation 又来了一次** = 这份原始像素不再每帧变
            //（视频帧的 generation 每次都变，永远走不到这里）。此时补一次 mip 链：
            // 程序化纹理（棋盘/渐变/噪声）缩小显示时才有 mip 可用，而视频帧不必为
            // 每帧的 glGenerateMipmap 付钱。只做一次（mipsBuilt 标记）。
            if (it->second && glGenerateMipmapP && !rawMips.count(imgKey(index))) {
                glBindTexture(GL_TEXTURE_2D, it->second);
                glGenerateMipmapP(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                rawMips[imgKey(index)] = 1;
            }
            return it->second != 0;
        }
        if (w <= 0 || h <= 0 || !rgba) return false;
        if (it != images.end() && it->second) glDeleteTextures(1, &it->second);
        images[imgKey(index)] = CreateTexture2D(w, h, rgba, true, true);
        imageDims[imgKey(index)] = { w, h };    // v25：九宫格切分要用源图像素尺寸
        rawMips.erase(index);
        imageGen[imgKey(index)] = generation;
        return images[imgKey(index)] != 0;
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
        if (affineOn) {   // 仿射：换成任意四边形（四角过矩阵），UV/颜色不变
            float p[8], uv[8];
            affCorners(x0, y0, x1, y1, p);
            uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0;
            uv[4] = u0; uv[5] = v1; uv[6] = u1; uv[7] = v1;
            emitQuadUV(p, uv, color);
            return;
        }
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
        if (affineOn) {   // 线段同样跟着转
            float p[8];
            affPoint(x0, y0, p[0], p[1]);
            affPoint(x1, y1, p[2], p[3]);
            affPoint(x2, y2, p[4], p[5]);
            affPoint(x3, y3, p[6], p[7]);
            const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            emitQuadUV(p, uv, color);
            return;
        }
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

    // v30：四角带色四边形（任意角度渐变）。顶点写法与 emitQuadUV 逐字一致，只是
// **每个顶点各自一个颜色** —— 顶点色本来就是逐顶点属性，着色器不用改。
void emitQuadUVColor(const BinUIGradQuad& q) {
    GLVertex* o = vertRoom(6);
    if (!o) return;
    beginBatch();
    auto sw = [](uint32_t c) {
        return (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    };
    const uint32_t c0 = sw(q.c[0]), c1 = sw(q.c[1]), c2 = sw(q.c[2]), c3 = sw(q.c[3]);
    o[0] = GLVertex{ q.x, q.y, c0, q.u0, q.v0 };
    o[1] = GLVertex{ q.x + q.w, q.y, c1, q.u1, q.v0 };
    o[2] = GLVertex{ q.x, q.y + q.h, c2, q.u0, q.v1 };
    o[3] = o[2];
    o[4] = o[1];
    o[5] = GLVertex{ q.x + q.w, q.y + q.h, c3, q.u1, q.v1 };
    vcount += 6;
    batches[curBatch].count++;
}

void flushAll() {
        if (vcount == 0 || !vb) return;
        frameVerts += vcount;
        glBufferDataP(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vcount * sizeof(GLVertex)),
                      verts.data(), GL_DYNAMIC_DRAW);
        // 正在画进图层时，每条批次再和层框求交（层外的内容不该出现在层里）
        const LayerFrame* lf = layerStack.empty() ? nullptr : &layerStack.back();
        for (const auto& b : batches) {
            if (b.count == 0) continue;
            int l = b.sx, t = b.sy, r = b.sx + b.sw, bot = b.sy + b.sh;
            if (lf && !ignoreLayerClip) {
                l = std::max(l, static_cast<int>(std::floor(lf->x)));
                t = std::max(t, static_cast<int>(std::floor(lf->y)));
                r = std::min(r, static_cast<int>(std::ceil(lf->x + lf->w)));
                bot = std::min(bot, static_cast<int>(std::ceil(lf->y + lf->h)));
            }
            if (r <= l || bot <= t) continue;
            // GL scissor 原点在左下：换算为从底部计
            glScissor(l, winH - bot, r - l, bot - t);
            glBindTexture(GL_TEXTURE_2D, b.tex);
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b.first), static_cast<GLsizei>(b.count * 6));
        }
        // 本帧真正用完了，淘汰的模板现在可以删（见 roundFor 里的淘汰注释）
        for (GLuint t : pendingTexDelete) glDeleteTextures(1, &t);
        pendingTexDelete.clear();
        // 图层（v0.16.9）：flush 之后必须清零，否则 present 会把同一批图元再画一遍 ——
        // 空层里那次没事，但"合成四边形"会被重复画到主目标上，表现为组透明度失效（全不透明）。
        vcount = 0;
        batches.clear();
        curBatch = static_cast<size_t>(-1);
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
        if (noiseTex) { glDeleteTextures(1, &noiseTex); noiseTex = 0; }
        destroyAllBlurSets();
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
// BinUIGL
// ============================================================
BinUIGL::BinUIGL() : impl_(new Impl()) {}
BinUIGL::~BinUIGL() { delete impl_; }

bool BinUIGL::init(void* hwnd, int width, int height, bool vsync) {
    impl_->hwnd = static_cast<HWND>(hwnd);
    impl_->winW = width;
    impl_->winH = height;
    impl_->vsync = vsync;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = width;
    impl_->scissor[3] = height;

    // 启动分段：BINUI_STARTUP=1 时把 init 拆到每一步（首帧慢时才知道是哪一步慢）
    const double t0 = binui::ex::StartupNowMs();
    if (!impl_->createContext()) { impl_->shutdown(); return false; }
    binui::ex::StartupMark(t0, "GL context (WGL 3.3)");
    if (!LoadGL()) { impl_->shutdown(); return false; }
    binui::ex::StartupMark(t0, "GL LoadGL (proc)");
    if (!impl_->createProgram()) { impl_->shutdown(); return false; }
    binui::ex::StartupMark(t0, "GL shaders");
    if (!impl_->createState()) { impl_->shutdown(); return false; }
    if (!impl_->createOffscreen()) { impl_->shutdown(); return false; }
    binui::ex::StartupMark(t0, "GL state+framebuffer");
    glViewport(0, 0, width, height);
    glUniform2fP(impl_->uniView, static_cast<float>(width), static_cast<float>(height));
    impl_->atlas.setHost(&impl_->glyphHost);
    impl_->atlas.initFonts();
    binui::ex::StartupMark(t0, "font atlas initFonts");
    wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXT>(
        GetGLProc("wglSwapIntervalEXT"));
    // **两个方向都要调**。老写法只在 vsync 为真时调 `wglSwapIntervalEXT(1)`，于是传
    // vsync=false 等于什么都没做 —— 驱动的默认 interval 就是 1，SwapBuffers 每帧照样等
    // 刷新。实测：vsync=false 时 present 8.8 ms、卡在 ~105 fps；真的调过 (0) 之后
    // 同一窗口 800+ fps（见 binGPU 的 bingpu_gl.cpp 同一处修复）。
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(impl_->vsync ? 1 : 0);
    impl_->ready = true;
    binui::ex::StartupMark(t0, "GL init total");
    return true;
}

void BinUIGL::shutdown() { impl_->shutdown(); }
bool BinUIGL::isReady() const { return impl_->ready; }
int BinUIGL::width() const { return impl_->winW; }
int BinUIGL::height() const { return impl_->winH; }
void BinUIGL::setVsync(bool on) {
    impl_->vsync = on;
    if (impl_->ready && wglSwapIntervalEXT) wglSwapIntervalEXT(on ? 1 : 0);
}

void BinUIGL::resize(int width, int height) {
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

void BinUIGL::beginFrame(const void* clipRect) {
    if (!impl_->ready) return;
    if (!impl_->makeCurrent()) return;   // 多实例：逐帧切回自己的上下文
    // 只复位计数，不 clear()：缓冲已经预分配到 kMaxVerts，clear() 每次会把
    // 2 MB 全部析构再由 vertRoom 重新 resize 一遍（白白 memset 一次）。
    impl_->vcount = 0;
    impl_->frameVerts = 0;
    impl_->batches.clear();
    impl_->curBatch = static_cast<size_t>(-1);
    impl_->scissorDepth = 0;
    impl_->curTex = impl_->whiteTex;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = impl_->winW;
    impl_->scissor[3] = impl_->winH;

    // 绘制目标 = 4x MSAA 离屏帧缓冲（present 时解析到 offTex）
    impl_->bindFbo(impl_->msFbo);
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

void BinUIGL::present() {
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
bool BinUIGL::presentPacesFrames() const {
    return impl_->ready && impl_->vsync && !impl_->transparent && wglSwapIntervalEXT != nullptr;
}

// v0.16.33：只提交不呈现 —— 基准工具用来把 "flushAll（顶点上传 + 绘制调用）" 与
// "MSAA resolve + SwapBuffers" 分开量。SwapBuffers 在窗口可见时会被合成器节流
// （实测 8.2 ms/帧，正好半个刷新周期），混在一起就量不出 CPU 成本了。
void BinUIGL::flushFrame() {
    if (!impl_->ready) return;
    if (!impl_->makeCurrent()) return;
    impl_->flushAll();
    static const bool traceVerts = std::getenv("BINUI_GL_VERTS") != nullptr;
    if (traceVerts)
        std::fprintf(stderr, "[gl][verts] %zu 顶点 %.1f KB（%zu 个四边形）\n",
                     impl_->frameVerts,
                     static_cast<double>(impl_->frameVerts * sizeof(GLVertex)) / 1024.0,
                     impl_->frameVerts / 6);
}

void BinUIGL::presentViaGdi() {
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

void BinUIGL::setTransparent(bool t) {
    impl_->transparent = t;
}

// ---------- 原语 ----------
void BinUIGL::fillRect(float x, float y, float w, float h, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    impl_->curTex = impl_->whiteTex;
    Premult(color);
    impl_->emitQuad(x, y, x + w, y + h, 0, 0, 1, 1, color);
}

bool BinUIGL::fillGradientQuad(float x, float y, float w, float h, float radius,
                                  float angleDeg, uint32_t from, uint32_t to) {
    if (!impl_ || !impl_->ready || w <= 0.0f || h <= 0.0f) return false;
    Impl& d = *impl_;
    const float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;
    GLuint tex = d.whiteTex;
    int tpl = 0;
    if (r > 0.5f) {
        const int rr = std::max(1, static_cast<int>(std::lround(r)));
        RoundTex& rt = d.roundFor(rr, false);
        if (!rt.tex) return false;              // 没模板：交回条带路径（像素不变）
        tex = rt.tex;
        tpl = rt.t;
    }
    binui::BinUIGradQuad q[9];
    const int n = binui::BuildGradientQuads(x, y, w, h, r, tpl, angleDeg, from, to, q, 9);
    if (n <= 0) return false;
    d.curTex = tex;
    for (int i = 0; i < n; i++) d.emitQuadUVColor(q[i]);
    d.curTex = d.whiteTex;
    return true;
}

void BinUIGL::fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    if (!impl_->ready || width <= 0) return;
    // v0.17.1：非仿射时走 CPU 覆盖率条带。4× MSAA 的采样子像素位置在 GL 与 VK 上
    // 互为镜像，"边缘斜穿像素"的细长四边形覆盖率会差一档（实测浅斜线差 62/255）。
    // 填充本来就是 CPU 条带，所以三端一致；描边必须跟它用同一条路。
    // 仿射下仍用原四边形：条带是轴对齐矩形，过矩阵后相邻条带之间会露缝。
    if (!impl_->affineOn) {
        const uint64_t lkey = binui::MakeLineBandKey(x0, y0, x1, y1, width, color);
        auto& lcache = binui::LineBandCache();
        auto lit = lcache.find(lkey);
        const std::vector<binui::PaintBand>* lbands = nullptr;
        if (lit != lcache.end()) {
            lbands = &lit->second;
        } else {
            static thread_local std::vector<binui::PaintBand> lineBands;
            if (lineBands.size() < static_cast<size_t>(binui::kMaxPathBands))
                lineBands.resize(binui::kMaxPathBands);
            const int n = binui::BuildLineBandsAA(x0, y0, x1, y1, width, color,
                                                  lineBands.data(),
                                                  static_cast<int>(lineBands.size()));
            if (lcache.size() >= binui::kLineBandCacheCap) lcache.clear();
            std::vector<binui::PaintBand>& slot = lcache[lkey];
            slot.assign(lineBands.begin(), lineBands.begin() + n);
            lbands = &slot;
        }
        for (size_t i = 0; i < lbands->size(); i++)
            fillRect((*lbands)[i].x, (*lbands)[i].y, (*lbands)[i].w, (*lbands)[i].h,
                     (*lbands)[i].color);
        return;
    }
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
void BinUIGL::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                               uint32_t color, const float xf[6]) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    const int layers = RoundedLayerCount(w, h, radius, stroke, thickness);
    Premult(color);
    int emitted = 0;
    // 诊断（BINUI_XF_DUMP=1）：把前两次调用的分格屏幕坐标打出来。仿射路径"角看起来
    // 不对"时，先看这里的 screen 四角是否自洽，再去怀疑模板或采样器。
    static int dumpLeft = getenv("BINUI_XF_DUMP") ? 2 : 0;
    for (int k = 0; k < layers; ++k) {
        RoundedLayer L;
        if (!BuildRoundedLayer(0.0f, 0.0f, w, h, radius, stroke, k, L)) break;
        RoundTex& rt = impl_->roundFor(L.tileRadius, stroke);
        if (!rt.tex) break;
        RoundedQuadXf q[9];
        const int n = BuildRoundedLayerQuadsXf(L, rt.t, xf, q, 9);
        if (n <= 0) break;
        impl_->curTex = rt.tex;
        for (int i = 0; i < n; i++) {
            impl_->emitQuadUV(q[i].pos, q[i].uv, color);
            emitted++;
            if (dumpLeft > 0 && k == 0) {
                std::fprintf(stderr,
                             "[xf] w=%.2f h=%.2f r=%.2f t=%d cell=%d "
                             "screen=(%.2f,%.2f)(%.2f,%.2f)(%.2f,%.2f)(%.2f,%.2f)\n",
                             (double)w, (double)h, (double)L.r, rt.t, i,
                             (double)q[i].pos[0], (double)q[i].pos[1], (double)q[i].pos[2],
                             (double)q[i].pos[3], (double)q[i].pos[4], (double)q[i].pos[5],
                             (double)q[i].pos[6], (double)q[i].pos[7]);
            }
        }
    }
    impl_->curTex = impl_->whiteTex;
    if (dumpLeft > 0) { dumpLeft--; }
    if (emitted == 0) {
        // 模板不可用：至少把变换后的外框画出来，别让节点凭空消失
        const float corners[8] = { 0, 0, w, 0, 0, h, w, h };
        RoundedQuadXf q;
        for (int v = 0; v < 4; v++) {
            const float lx = corners[v * 2], ly = corners[v * 2 + 1];
            q.pos[v * 2] = xf[0] * lx + xf[1] * ly + xf[2];
            q.pos[v * 2 + 1] = xf[3] * lx + xf[4] * ly + xf[5];
            q.uv[v * 2] = 0.5f;
            q.uv[v * 2 + 1] = 0.5f;
        }
        impl_->emitQuadUV(q.pos, q.uv, color);
    }
}

void BinUIGL::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    // 半径钳到短边一半、角片模板按最近整数取 —— 这两条与另两端共用 binui_paint.h 的
    // BuildRoundedLayer（v0.25.0 起三端不再各写一份）。
    RoundedLayer L;
    if (!BuildRoundedLayer(x, y, w, h, radius, false, 0, L)) { fillRect(x, y, w, h, color); return; }
    RoundTex& rt = impl_->roundFor(L.tileRadius, false);
    if (!rt.tex) { fillRect(x, y, w, h, color); return; }
    PaintQuad q[9];
    const int n = BuildRoundedLayerQuads(L, rt.t, q, 9);
    if (n <= 0) { fillRect(x, y, w, h, color); return; }
    Premult(color);
    impl_->curTex = rt.tex;
    for (int i = 0; i < n; i++)
        impl_->emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                        q[i].u0, q[i].v0, q[i].u1, q[i].v1, color);
    impl_->curTex = impl_->whiteTex;
}

void BinUIGL::strokeRounded(float x, float y, float w, float h, float radius,
                            float thickness, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    // v0.34.4：**一整圈环带模板 + 一次九宫格**，不再叠同心 1px 环。
    // 叠层在直边上是对的，弧上会丢掉约 40% 的覆盖率（相邻两层的抗锯齿边各贡献
    // 约一半，源覆盖叠加两次 = 0.5 + 0.5×0.5 = 0.75），肉眼就是"发灰发断"。
    // 新模板的几何与 bin2d 的 `Canvas::strokePath` 完全同一条（内缩 t/2 + 居中描边 t）。
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) {
        // 半径退化：直角四条边（与 bin2d 的 strokePath 直角分支同一套）
        const float t = thickness;
        fillRect(x, y, w, t, color);
        fillRect(x, y + h - t, w, t, color);
        if (h - 2.0f * t > 0.0f) {
            fillRect(x, y + t, t, h - 2.0f * t, color);
            fillRect(x + w - t, y + t, t, h - 2.0f * t, color);
        }
        return;
    }
    int th = static_cast<int>(std::lround(std::max(1.0f, thickness)));
    const int tileRadius = std::max(1, static_cast<int>(std::lround(rf)));
    if (th > tileRadius) th = tileRadius;
    RoundTex& rt = impl_->roundForThick(tileRadius, th);
    if (!rt.tex) {
        // 模板起不来：退回矩形四条边，别让边框凭空消失
        fillRect(x, y, w, thickness, color);
        fillRect(x, y + h - thickness, w, thickness, color);
        return;
    }
    RoundedLayer L;
    L.x = x;
    L.y = y;
    L.w = w;
    L.h = h;
    L.r = rf;
    L.tileRadius = tileRadius;
    PaintQuad q[9];
    const int n = BuildRoundedLayerQuads(L, rt.t, q, 9);
    if (n <= 0) return;
    Premult(color);
    impl_->curTex = rt.tex;
    for (int i = 0; i < n; i++)
        impl_->emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                        q[i].u0, q[i].v0, q[i].u1, q[i].v1, color);
    impl_->curTex = impl_->whiteTex;
}

void BinUIGL::setLetterSpacing(float px) {
    // v0.16.40：字间距只影响排版，字形位图不变（整形缓存的 key 里带它）
    impl_->atlas.setLetterSpacing(px);
}
void BinUIGL::setTextSubpixel(bool on) {
    // 图集纹理本身已经是 LINEAR（见 GLGlyphTexHost::createTex），这里只切"要不要取整"
    if (impl_) impl_->atlas.snapText = !on;
}

// v24 文字阴影：图集负责"字形位图 → 模糊覆盖率"，这里只要把状态传下去。
// 节点内的每一次 drawText 都会先铺一遍阴影四边形，再画清晰字形。
// v0.16.18 仿射状态：文字/图片/填充的四边形统一过矩阵（见 Impl::affineOn）。
// 只在"节点有旋转/斜切/非等比缩放"时打开，关掉时 emitQuad 走原来的整数/浮点快路径。
// v0.16.27 毛玻璃（backdrop blur）：抓当前目标的区域 → 多级 2× 降采样 → 圆角合成。
// 算法与另两端**逐字一致**（同一套 UV 公式），这是像素一致的前提：
//   级数 = 让 2^levels >= blurPx 的最小值（1..6）；每级 2×2 盒式模糊（双线性取
//   目标像素中心 = 上一级 2×2 的平均），链起来逼近高斯；
//   合成用 BuildImageBands 的圆角条带 + replace 混合（背景被糊掉，不是叠一层模糊）。
bool BinUIGL::backdropBlur(float x, float y, float w, float h, float radius,
                              float blurPx, float grain) {
    if (!impl_ || !impl_->ready || !impl_->makeCurrent()) return false;
    Impl& d = *impl_;
    if (blurPx <= 0.0f || w < 2.0f || h < 2.0f) return false;
    int rx = static_cast<int>(std::floor(x));
    int ry = static_cast<int>(std::floor(y));
    int rx1 = static_cast<int>(std::ceil(x + w));
    int ry1 = static_cast<int>(std::ceil(y + h));
    rx = std::max(0, rx); ry = std::max(0, ry);
    rx1 = std::min(d.winW, rx1); ry1 = std::min(d.winH, ry1);
    const int rw = rx1 - rx, rh = ry1 - ry;
    if (rw < 2 || rh < 2) return false;

    int levels = 0;
    {
        // v0.16.44：与 filter: blur 用同一个取法（降到 σ 还剩 ≥2 纹素那一级，剩下的交给
        // 分离高斯）。以前是"让 2^levels ≥ 半径"，大半径时最粗级只剩几个纹素 ——
        // 毛玻璃的背板多半是平滑渐变所以不明显，但硬边背板一眼就能看出梯形边缘。
        while (levels < 5 && blurPx / static_cast<float>(1 << (levels + 1)) >= 2.0f) levels++;
    }

    d.flushAll();   // 手上攒的图元先落到当前目标（它们构成"背景"）

    // v0.16.35：这两个查询都是同步点（驱动要 drain 流水线才能回答），改成用自己维护的值。
    // 视口在这一刻恒为 (0,0,winW,winH)：beginFrame 设一次，层/降采样链结束后都会恢复。
    const GLuint prevFbo = d.curFbo;
    const GLint prevVp[4] = { 0, 0, d.winW, d.winH };
    const bool scissorWas = glIsEnabled(GL_SCISSOR_TEST) != 0;

    // 1) 抓背景：当前目标 → level0（GL 的 FBO 行从下往上，世界 y 从上往下，翻转）
    if (!d.selectBlurSet(rw, rh) || !d.ensureBlurLevel(0, rw, rh)) {
        d.bindFbo(prevFbo);
        return false;
    }
    glDisable(GL_SCISSOR_TEST);   // glBlitFramebuffer 受 scissor 裁剪
    glBindFramebufferP(GL_READ_FRAMEBUFFER, prevFbo);
    glBindFramebufferP(GL_DRAW_FRAMEBUFFER, d.blurAt(0).fbo);
    d.curFbo = d.blurAt(0).fbo;
    {
        const int by = d.winH - ry1;
        glBlitFramebufferP(rx, by, rx1, by + rh, 0, 0, rw, rh,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    d.bindFbo(prevFbo);

    // 2) 降采样链：每级把上一级缩一半（覆盖混合；目标先清成透明）
    glBlendFunc(GL_ONE, GL_ZERO);
    d.ignoreLayerClip = true;
    bool chainOk = true;
    for (int i = 1; i <= levels && chainOk; i++) {
        const int dw = std::max(1, d.blurAt(static_cast<size_t>(i - 1)).w / 2);
        const int dh = std::max(1, d.blurAt(static_cast<size_t>(i - 1)).h / 2);
        if (!d.ensureBlurLevel(static_cast<size_t>(i), dw, dh)) { chainOk = false; break; }
        d.bindFbo(d.blurAt(static_cast<size_t>(i)).fbo);
        glViewport(0, 0, dw, dh);
        glUniform2fP(d.uniView, static_cast<float>(dw), static_cast<float>(dh));
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        d.curTex = d.blurAt(static_cast<size_t>(i - 1)).tex;
        // 目标像素中心对上一级：v 轴翻转（纹理行 0 = 目标底部）
        d.emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                   0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
        d.flushAll();
    }
    d.ignoreLayerClip = false;
    // v0.16.44：末级上跑分离高斯（横 7 抽头 + 纵 7 抽头）。失败就只是"糊得糙一点"，
    // 不影响后面照常合成。scissor 必须先关：它既裁 glClear（只清到节点框，框外留着上一帧的
    // 残影）又裁累加趟 —— 毛玻璃的合成在下面自己会重新打开。
    if (chainOk) {
        glDisable(GL_SCISSOR_TEST);
        d.gaussBlurLevel(levels, blurPx);
    }
    d.bindFbo(prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glUniform2fP(d.uniView, static_cast<float>(d.winW), static_cast<float>(d.winH));

    // 3) 合成：圆角条带 + 双线性放大，replace 混合（背景被替换成模糊版）
    if (chainOk) {
        // gaussBlurLevel 退出时把混合恢复成了正常 alpha（它自己的累加趟用 ADD），
        // 毛玻璃的合成必须是 replace —— 这里显式再设一次，别依赖"上一段代码留的状态"。
        glBlendFunc(GL_ONE, GL_ZERO);
        if (scissorWas) glEnable(GL_SCISSOR_TEST);   // 合成要遵守节点的裁剪
        d.curTex = d.blurAt(static_cast<size_t>(levels)).tex;
        binui::PaintImageBand bands[binui::kMaxImageBands];
        const int nb = binui::BuildImageBands(x, y, w, h, radius, bands);
        for (int q = 0; q < nb; q++) {
            const binui::PaintImageBand& b = bands[q];
            if (b.w <= 0.0f || b.h <= 0.0f) continue;
            d.emitQuad(b.x, b.y, b.x + b.w, b.y + b.h,
                       b.u0, 1.0f - b.v0, b.u1, 1.0f - b.v1, 0xFFFFFFFFu);
        }
        d.flushAll();
    }
    // 4) 颗粒（grain）：一层极淡的确定性噪声，压住大块模糊面的 banding。
    //    屏幕锚定 + 一个纹素 = 一个屏幕像素（u = x/64, v = y/64），元素移动时颗粒
    //    不跟着滑；NEAREST 采样保证三端取到同一个纹素。
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // 颗粒要"加"，不是覆盖
    if (chainOk && grain > 0.001f) {
        const GLuint nt = d.ensureNoiseTex();
        if (nt) {
            d.curTex = nt;
            // 顶点色 alpha：最大 15/255 ≈ 6%（再乘噪声自身 alpha，实际平均 ~3%）
            const uint32_t av = static_cast<uint32_t>(grain * 3.0f + 0.5f);
            const uint32_t ncol = (av << 24) | 0x00FFFFFFu;
            binui::PaintImageBand nb2[binui::kMaxImageBands];
            const int nnb = binui::BuildImageBands(x, y, w, h, radius, nb2);
            for (int q = 0; q < nnb; q++) {
                const binui::PaintImageBand& b = nb2[q];
                if (b.w <= 0.0f || b.h <= 0.0f) continue;
                const float k = 1.0f / 64.0f;
                // 不要再加 0.5 纹素：光栅化时取的是**像素中心**（x+0.5），
                // u = x/64 时像素中心正好落在纹素中心；再加半个纹素就落到边界上，
                // NEAREST 取哪个纹素由实现决定（GL 与 D3D11 会不一致 —— 实测过）。
                d.emitQuad(b.x, b.y, b.x + b.w, b.y + b.h,
                           b.x * k, b.y * k,
                           (b.x + b.w) * k, (b.y + b.h) * k, ncol);
            }
            d.flushAll();
        }
    }
    d.curTex = d.whiteTex;
    if (scissorWas) glEnable(GL_SCISSOR_TEST);
    return chainOk;
}

bool BinUIGL::setAffine(const float* xf) {
    if (!impl_) return false;
    if (!xf || !(xf[0] != 1.0f || xf[1] != 0.0f || xf[2] != 0.0f ||
                 xf[3] != 0.0f || xf[4] != 1.0f || xf[5] != 0.0f)) {
        impl_->affineOn = false;
        return true;
    }
    for (int i = 0; i < 6; i++) impl_->aff[i] = xf[i];
    impl_->affineOn = true;
    return true;
}

// v27 裁剪擦除：切换混合前先 flush（批次是延迟提交的，状态必须跟着批次走）
bool BinUIGL::setEraseBlend(bool on) {
    if (!impl_ || !impl_->ready) return false;
    impl_->flushAll();
    // GL 1.1 头里没有 glBlendFuncSeparate；两种状态下 color/alpha 因子相同，一个够了
    if (on) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);   // dst *= (1-srcA)：擦掉补集
    else    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

// v28 遮罩保留趟：dst *= srcA（掩码内保留、掩码外先不动 —— 由补集趟清零）
bool BinUIGL::setKeepBlend(bool on) {
    if (!impl_ || !impl_->ready) return false;
    impl_->flushAll();
    if (on) glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
    else    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}



void BinUIGL::setTextShadows(const BinUITextShadow* layers, int count) {
    if (impl_) impl_->atlas.setTextShadows(layers, count);
}

// v26 图片滤镜：矩阵只影响"下一次上传"的像素，并按哈希进纹理缓存键 ——
// 同一张图配两种滤镜 = 两份纹理；没有滤镜时 imgFilterKey = 0，键退化为 imageIndex。
void BinUIGL::setImageFilter(const BinUIImageFilter* f) {
    if (!impl_) return;
    impl_->imgFilterKey = binui::ImageFilterKey(f);
    if (!impl_->imgFilterKey) return;
    for (int i = 0; i < 9; i++) impl_->imgFilter.m[i] = f->m[i];
    for (int i = 0; i < 3; i++) impl_->imgFilter.o[i] = f->offset[i];
}

bool BinUIGL::pushLayer(float x, float y, float w, float h, float alpha) {
    return impl_ ? impl_->pushLayer(x, y, w, h, alpha) : false;
}

// v0.16.43：带效果（blur / drop-shadow）的离屏图层。框已由调用方按外扩算好并取整。
bool BinUIGL::pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) {
    if (!impl_ || (fx.shadowCount > 0 && !fx.shadows)) return false;
    return impl_->pushLayer(x, y, w, h, fx.alpha, fx.blurPx, fx.shadows, fx.shadowCount);
}

void BinUIGL::popLayer() {
    if (impl_) impl_->popLayer();
}

float BinUIGL::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->ready) return 0;
    return impl_->atlas.textWidth(size, weight, utf8, italic);
}

void BinUIGL::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    impl_->atlas.fontMetrics(size, weight, ascent, lineHeight, italic);
}

// 文本排版/光栅化/图集全在 binui::GlyphAtlas 里（与 Vulkan 后端同一份代码），
// 这里只把回调给出的字形四边形丢进本后端的顶点批次。
void BinUIGL::drawText(float x, float y, float size, int weight,
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

void BinUIGL::drawTextWrapped(float x, float y, float maxW, float size, int weight,
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
bool BinUIGL::prepareImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImage(index, data, size, generation);
}

bool BinUIGL::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImageRaw(index, w, h, rgba, generation);
}

// v25 九宫格：切分数学在 binui_gfx.h 的 BuildNineSlice（三端共用），这里只负责
// 按算出的小块发九个四边形。源图尺寸缺失就退回整图拉伸（不至于什么都不画）。
void BinUIGL::drawImageNine(uint16_t index, float x, float y, float w, float h,
                              float slice, int repX, int repY, uint32_t tint) {
    if (!impl_ || !impl_->ready) return;
    auto it = impl_->imageDims.find(impl_->imgKey(index));
    float q[kMaxNineSliceQuads][8];
    const int n = it == impl_->imageDims.end()
        ? 0 : BuildNineSlice(x, y, w, h, it->second.first, it->second.second, slice,
                             repX, repY, &q[0][0], kMaxNineSliceQuads);
    if (n <= 0) { drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, tint); return; }
    for (int i = 0; i < n; i++)
        drawImageEx(index, q[i][0], q[i][1], q[i][2], q[i][3],
                    q[i][4], q[i][5], q[i][6], q[i][7], tint);
}

void BinUIGL::drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                             float x, float y, float w, float h, uint32_t tint) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    auto it = impl_->images.find(impl_->imgKey(index));
    if (it == impl_->images.end() || !it->second) return;
    impl_->curTex = it->second;
    impl_->emitQuad(x, y, x + w, y + h, u0, v0, u1, v1, tint);
    impl_->curTex = impl_->whiteTex;
}

int BinUIGL::pushClip(float x, float y, float w, float h) {
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

void BinUIGL::popClipTo(int depth) {
    impl_->scissorDepth = depth;
    if (impl_->scissorDepth == 0) {
        impl_->scissor[0] = 0;
        impl_->scissor[1] = 0;
        impl_->scissor[2] = impl_->winW;
        impl_->scissor[3] = impl_->winH;
    }
}

void BinUIGL::setFontName(const char* name) {
    impl_->atlas.setFontName(name);
}

namespace binui {

// v19：节点树遍历已抽到 binui_draw_nodes.h（后端无关）。这里只剩一个薄壳，
// 保持既有调用点（demo / tools）不变；CanvasDrawFn 收到的是具体后端类型，
// 进共享遍历前转成 IBinUIGfx&。
void drawNodesGL(BinUIGL& gfx, const BinUIRenderer& renderer, const void* clipRect,
                    const CanvasDrawFn& canvas) {
    CanvasFn shared;
    if (canvas) {
        shared = [&canvas](IBinUIGfx& g, uint16_t id, float x, float y, float w, float h) {
            canvas(static_cast<BinUIGL&>(g), id, x, y, w, h);
        };
    }
    drawNodesBinUI(gfx, renderer, clipRect, shared);
}



} // namespace binui


