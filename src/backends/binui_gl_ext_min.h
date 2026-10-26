// binui_gl_ext_min.h - 无 Khronos 头时的最小 GL/WGL 扩展存根（MSVC + Windows SDK）
// Windows SDK 的 gl.h 仅含 GL 1.1；MinGW 环境有完整 <GL/glext.h>/<GL/wglext.h>，
// 本文件只在缺失时提供本后端用到的扩展常量与类型（值与 Khronos 官方定义一致）。
#pragma once

#include <GL/gl.h>

#ifndef GLchar
typedef char GLchar;
#endif
#ifndef GLsizeiptr
typedef ptrdiff_t GLsizeiptr;
#endif
#ifndef GLintptr
typedef ptrdiff_t GLintptr;
#endif

// ---- shader / buffer（GL 2.0/3.0） ----
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#endif

// ---- 采样（GL 1.2+） ----
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

// ---- 帧缓冲对象（GL 3.0） ----
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

// ---- WGL 3.3 core context（wglext） ----
#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
