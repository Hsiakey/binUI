// binui_d3d11.cpp - D3D11 GPU 渲染后端实现
// 架构：BinUID3D 维护字形图集（FreeType 光栅化一次）+ 圆角九宫格模板 + 顶点批次，
// drawNodesD3D 与 drawNodesGDI 语义一致地遍历节点树，全部绘制为 GPU 四边形，
// BinUIAppD3D 提供无边框窗口与事件循环（VSync 按需呈现）。
// 文字渲染委托 binui_font_renderer（FreeType + HarfBuzz），与平台解耦。
#include "backends/binui_d3d11.h"

#ifndef _WIN32
#error "binui_d3d11 is Windows-only"
#endif

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
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
// v13 扩展绘制段的共用几何（渐变色带 / 四边边框 / 文字装饰线 / 行高）：
// 与 GDI、GL 后端共用同一份数学，三端像素表现才一致
#include "binui_paint.h"
#include "platform/binui_win32_foreground.h"

using namespace binui;

// mingw-w64 无 __uuidof，显式 GUID
static const IID IID_XDXGIDevice   = { 0x54EC77FA, 0x1377, 0x44E6, { 0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C } };
static const IID IID_XDXGIAdapter  = { 0x2411E7E1, 0x12AC, 0x4CCF, { 0xBD,0x14,0x97,0x98,0xE8,0x53,0x4D,0xC0 } };
static const IID IID_XDXGIFactory2 = { 0x50C83A1C, 0xE072, 0x4C48, { 0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0 } };
static const IID IID_XTexture2D    = { 0x6F15AAF2, 0xD208, 0x4E89, { 0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C } };

namespace {

// v13 行高（CSS line-height）：drawTextWrapped 的公开声明在 binui_d3d11.h 里只有 8 个
// 参数，本次改动只允许动 .cpp —— 参数加不进签名，就用这个文件级"当前行距"把它带进去。
// 只有 LABEL 多行那一处会在调用前设置、调用后立刻复位 0（0 = 沿用 fontMetrics 给出的
// 字体行高），其余调用点（列表、视频控制条……）永远是旧行为；后端是单线程立即模式，
// 设置与调用紧邻，不会被别的节点踩到。
float gWrappedLineStep = 0.0f;

// ---------- 小工具 ----------
// 按透明度缩放 alpha（0xAARRGGBB；RGB 保持直线色，Premult 时统一缩放）
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
struct D3DVertex {
    float x, y;
    uint32_t color; // 0xAARRGGBB（直通，PS 内预乘）
    float u, v;
};

struct Batch {
    uint32_t first = 0;  // 顶点起始（6 顶点/四边形）
    uint32_t count = 0;  // 四边形数
    int sx = 0, sy = 0, sw = 0, sh = 0;
    uint8_t linear = 0;  // 0=点采样(字形/纯色) 1=线性(圆角/图片)
    ID3D11ShaderResourceView* srv = nullptr;
};

struct Glyph {
    uint16_t cell = 0;
    uint8_t advW = 0;
};

struct Atlas {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int sizePx = 1024;
    int cellW = 0, cellH = 0;
    int cols = 0;
    int next = 1;             // 格子 0 保留给纯色用（见下）
    int ascent = 0, descent = 0;
    float size = 0;
    int weight = 400;
    int faceIdx = 0;          // 回退链上的面下标（0 = 主面；缺字的豆腐块也放这里）
    bool italic = false;
    std::string face;         // 字体面名（FontEngine 注册名）
    std::unordered_map<uint32_t, Glyph> glyphs;
};

struct RoundTex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int t = 0;         // 模板边长（4r）
    int corner = 0;    // 角区边长（2r+1）
};

// ---------- emoji 簇检测 ----------
// 是否 emoji 相关码点（含变体选择符/ZWJ/区域指示符/键帽）
bool IsEmojiCp(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;   // 表情/符号/扩展
    if (cp >= 0x2190 && cp <= 0x21FF) return true;     // 箭头
    if (cp >= 0x2300 && cp <= 0x27BF) return true;     // 杂项符号/技术/装饰
    if (cp >= 0x2900 && cp <= 0x297F) return true;     // 补充箭头
    if (cp >= 0x2B00 && cp <= 0x2BFF) return true;     // 杂项符号/箭头
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return true;   // 区域指示符（国旗）
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;     // 变体选择符
    if (cp == 0x200D) return true;                     // ZWJ
    if (cp == 0x20E3) return true;                     // 键帽
    return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
           cp == 0x2122 || cp == 0x2139 || cp == 0x3030 || cp == 0x303D ||
           cp == 0x3297 || cp == 0x3299;
}

// 从 cps[0] 起的 emoji 簇长度：键帽序列 / ZWJ 序列 / 肤色修饰 / VS / 区域指示符双拼
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
        if (c == 0x200D) {                 // ZWJ：吞掉其后的成员
            if (j + 1 >= n) break;
            j += 2;
            continue;
        }
        if (c == 0xFE0F || c == 0xFE0E || c == 0x20E3) { j++; continue; }
        if (c >= 0x1F3FB && c <= 0x1F3FF) { j++; continue; }   // 肤色修饰
        if (c >= 0x1F1E6 && c <= 0x1F1FF) {                   // 区域指示符：仅双拼
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
    uint16_t advW = 0;     // 簇前进宽（像素）
    uint16_t w = 0;        // 实际像素宽
    uint16_t h = 0;        // 实际像素高
    uint16_t baseline = 0; // 合成缓冲内基线 y
};

struct ColorAtlas {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int sizePx = 1024;
    int cellW = 0, cellH = 0;
    int cols = 0;
    int next = 0;
    float size = 0;
    std::unordered_map<uint64_t, ColorGlyph> glyphs;
};

constexpr size_t kMaxVerts = 100000; // 2MB 动态缓冲上限（约 16.6k 四边形）

} // namespace

struct BinUID3D::Impl {
    HWND hwnd = nullptr;
    int winW = 0, winH = 0;
    bool vsync = true;
    bool ready = false;
    bool transparent = false; // 分层窗口（UpdateLayeredWindow）
    bool forceGdi = false;    // 拖拽 resize 期由上层置位：强制读回 + GDI 贴屏
    bool coversMonitor = false; // 客户区铺满整块显示器 → 绕开直翻转

    // 分层窗口呈现缓冲（32bit BGRA premultiplied）
    HBITMAP layeredDib = nullptr;
    HDC layeredDC = nullptr;
    void* layeredBits = nullptr;
    int layeredW = 0, layeredH = 0;
    ID3D11Texture2D* stagingTex = nullptr; // 离屏读回用（USAGE_STAGING）

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* il = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11SamplerState* samPoint = nullptr;
    ID3D11SamplerState* samLinear = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* ras = nullptr;
    ID3D11ShaderResourceView* whiteSrv = nullptr; // 1×1 纯白（纯色四边形）
    // 持久离屏帧缓冲：局部重绘只更新脏区，present 时整帧 blit 到交换链
    ID3D11Texture2D* offTex = nullptr;
    ID3D11RenderTargetView* offRtv = nullptr;
    ID3D11ShaderResourceView* offSrv = nullptr;

    // 场景
    std::vector<D3DVertex> verts;
    std::vector<Batch> batches;
    Batch* curBatch = nullptr;
    int scissor[4] = { 0, 0, 0, 0 };
    int scissorDepth = 0;
    uint8_t curLinear = 0;
    ID3D11ShaderResourceView* curSrv = nullptr;

    // 字形图集（按 size/weight/字体族）
    std::unordered_map<uint64_t, Atlas*> atlases;
    std::string asciiFont = "Segoe UI";
    static constexpr const char* cjkFont = "Microsoft YaHei";
    static constexpr const char* emojiFont = "Segoe UI Emoji";

    // 跨平台字体服务（FreeType 光栅化 + HarfBuzz 整形）
    binui_font::FontEngine font;
    bool fontReady = false;

    // ---------- 逐字形回退（与 GL 后端同一套语义）----------
    // 谁有字形是字体自报的 cmap，不是码点区间能推出来的。字体服务返回 -1 = 所有
    // 候选都没有；缺字的豆腐块借 0 号面的图集（几何与字体无关）。
    int faceIdxForCp(uint32_t cp) {
        if (!fontReady) return 0;
        const int idx = font.faceIndexFor(cp);
        return idx < 0 ? 0 : idx;
    }

    static bool IsInvisibleCp(uint32_t cp) {
        if (cp < 0x20) return true;
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) return true;
        if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
        if (cp >= 0xE0100 && cp <= 0xE01EF) return true;
        if (cp == 0x20E3) return true;
        return false;
    }

    int emojiFaceIdx() {
        if (!emojiIdxInit) {
            emojiIdxInit = true;
            emojiIdx = -1;
            if (fontReady) {
                const auto& chain = font.fallbackChain();
                for (size_t i = 0; i < chain.size(); i++)
                    if (chain[i] == emojiFont) { emojiIdx = static_cast<int>(i); break; }
            }
        }
        return emojiIdx;
    }
    bool emojiIdxInit = false;
    int emojiIdx = -1;

    std::unordered_map<uint32_t, ColorAtlas*> colorAtlases;        // 按字号
    // 圆角模板（按 radius；负 key = 描边）
    std::unordered_map<int, RoundTex> rounds;

    // 图片纹理缓存（按 imageIndex）
    std::unordered_map<uint16_t, ID3D11ShaderResourceView*> images;
    std::unordered_map<uint16_t, uint32_t> imageGen; // 换图 generation：不一致时重新上传

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

    bool createDeviceAndSwapchain() {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                       D3D_FEATURE_LEVEL_10_0 };
        // BGRA_SUPPORT：交换链/纹理使用 B8G8R8A8 所需
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx);
        }
        if (FAILED(hr) || !device || !ctx) {
            std::fprintf(stderr, "[d3d11] CreateDevice failed hr=0x%08X\n", (unsigned)hr);
            return false;
        }

        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory2 = nullptr;
        if (FAILED(device->QueryInterface(IID_XDXGIDevice, (void**)&dxgiDev)) ||
            FAILED(dxgiDev->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_XDXGIFactory2, (void**)&factory2))) {
            std::fprintf(stderr, "[d3d11] DXGI factory resolve failed\n");
            if (dxgiDev) dxgiDev->Release();
            if (adapter) adapter->Release();
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = static_cast<UINT>(winW);
        sd.Height = static_cast<UINT>(winH);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory2->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr,
                                              (IDXGISwapChain1**)&swap);
        factory2->Release();
        dxgiDev->Release();
        adapter->Release();
        if (FAILED(hr) || !swap) {
            std::fprintf(stderr, "[d3d11] CreateSwapChainForHwnd failed hr=0x%08X\n", (unsigned)hr);
            return false;
        }
        return createRenderTarget();
    }

    bool createRenderTarget() {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap->GetBuffer(0, IID_XTexture2D, (void**)&back))) {
            std::fprintf(stderr, "[d3d11] GetBuffer failed\n");
            return false;
        }
        HRESULT hr = device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
        if (FAILED(hr)) std::fprintf(stderr, "[d3d11] CreateRTV failed hr=0x%08X\n", (unsigned)hr);
        return SUCCEEDED(hr);
    }

    bool compileShaders() {
        const char* hlsl =
            "cbuffer CB : register(b0) { float2 viewSize; };\n"
            "struct VSIn { float2 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };\n"
            "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };\n"
            "PSIn vs(VSIn v) {\n"
            "  PSIn o;\n"
            "  o.pos = float4(v.pos.x / viewSize.x * 2 - 1, 1 - v.pos.y / viewSize.y * 2, 0, 1);\n"
            "  o.col = v.col;\n"
            "  o.uv = v.uv;\n"
            "  return o;\n"
            "}\n"
            "Texture2D tex : register(t0);\n"
            "SamplerState sam : register(s0);\n"
            "float4 ps(PSIn i) : SV_Target {\n"
            "  float4 t = tex.Sample(sam, i.uv);\n"
            "  return float4(t.rgb * i.col.rgb, t.a * i.col.a);\n"
            "}\n";
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(hlsl, strlen(hlsl), "binui", nullptr, nullptr, "vs", "vs_4_0",
                              0, 0, &vsBlob, &err)) ||
            FAILED(D3DCompile(hlsl, strlen(hlsl), "binui", nullptr, nullptr, "ps", "ps_4_0",
                              0, 0, &psBlob, &err))) {
            if (err) {
                std::fprintf(stderr, "[d3d11] shader error: %s\n",
                             static_cast<const char*>(err->GetBufferPointer()));
                err->Release();
            }
            return false;
        }
        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
        if (!vs || !ps) { vsBlob->Release(); psBlob->Release(); return false; }

        D3D11_INPUT_ELEMENT_DESC elems[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        device->CreateInputLayout(elems, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &il);
        vsBlob->Release();
        psBlob->Release();
        return il != nullptr;
    }

    bool createStates() {
        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&cbd, nullptr, &cb))) {
            std::fprintf(stderr, "[d3d11] CB failed\n");
            return false;
        }

        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = static_cast<UINT>(kMaxVerts * sizeof(D3DVertex));
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&vbd, nullptr, &vb))) {
            std::fprintf(stderr, "[d3d11] VB failed\n");
            return false;
        }

        D3D11_SAMPLER_DESC smd = {};
        smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.MaxAnisotropy = 1;
        smd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        smd.MinLOD = 0;
        smd.MaxLOD = D3D11_FLOAT32_MAX;
        HRESULT shr = device->CreateSamplerState(&smd, &samPoint);
        if (FAILED(shr)) {
            std::fprintf(stderr, "[d3d11] sampler point failed hr=0x%08X removed=0x%08X\n",
                         (unsigned)shr, (unsigned)device->GetDeviceRemovedReason());
            return false;
        }
        smd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        shr = device->CreateSamplerState(&smd, &samLinear);
        if (FAILED(shr)) {
            std::fprintf(stderr, "[d3d11] sampler linear failed hr=0x%08X removed=0x%08X\n",
                         (unsigned)shr, (unsigned)device->GetDeviceRemovedReason());
            return false;
        }

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; // 预乘 alpha
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bd, &blend))) {
            std::fprintf(stderr, "[d3d11] blend failed\n");
            return false;
        }

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rd, &ras))) {
            std::fprintf(stderr, "[d3d11] rasterizer failed\n");
            return false;
        }

        // 1×1 纯白纹理（纯色四边形使用）
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint8_t white[4] = { 255, 255, 255, 255 };
        D3D11_SUBRESOURCE_DATA init = { white, 4, 0 };
        ID3D11Texture2D* wt = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &init, &wt)) ||
            FAILED(device->CreateShaderResourceView(wt, nullptr, &whiteSrv))) {
            std::fprintf(stderr, "[d3d11] white texture failed\n");
            if (wt) wt->Release();
            return false;
        }
        wt->Release();

        UINT stride = sizeof(D3DVertex);
        UINT offset = 0;
        float blendFactor[4] = { 0, 0, 0, 0 };
        ctx->OMSetBlendState(blend, blendFactor, 0xFFFFFFFF);
        ctx->RSSetState(ras);
        D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(winW), static_cast<float>(winH), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(il);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        return true;
    }

    // ---------- GDI 光栅化 ----------
    // DIB 必须与目标尺寸精确一致：UpdateSubresource 按 cellW*4 行距读取，
    // 复用更大的 DIB 会导致字形/模板数据错位（斜条纹垃圾）
    bool ensureRasterDC(int w, int h) {
        if (rasterDC && rasterW == w && rasterH == h) return true;
        if (rasterBmp) DeleteObject(rasterBmp);
        if (rasterDC) DeleteDC(rasterDC);
        rasterDC = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // 自上而下
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

    Atlas* getAtlas(float size, int weight, int faceIdx, bool italic = false) {
        // 斜体与粗体一样是"另一张图集"：合成倾斜发生在光栅化阶段，不能共用缓存。
        // 低 6 位放**面下标**（原来是 cjk / emoji 两个布尔）：同一个码点解析到不同
        // 字体时必须分图集，否则字形会串。
        const uint32_t fidx = static_cast<uint32_t>(faceIdx < 0 ? 0 : faceIdx) & 0x3Fu;
        uint64_t key = (static_cast<uint64_t>(std::lround(size)) << 32) |
                       (static_cast<uint64_t>((weight & 0x7FFF) | (italic ? 0x8000 : 0)) << 16) |
                       fidx;
        auto it = atlases.find(key);
        if (it != atlases.end()) return it->second;

        Atlas* a = new Atlas();
        a->size = size;
        a->weight = weight;
        a->faceIdx = static_cast<int>(fidx);
        a->italic = italic;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH;
        a->cols = a->sizePx / a->cellW;
        a->face = fontReady ? font.faceName(a->faceIdx) : asciiFont;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = static_cast<UINT>(a->sizePx);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint8_t> zeros(static_cast<size_t>(a->sizePx) * a->sizePx * 4, 0);
        D3D11_SUBRESOURCE_DATA init = { zeros.data(), static_cast<UINT>(a->sizePx) * 4, 0 };
        if (FAILED(device->CreateTexture2D(&td, &init, &a->tex)) ||
            FAILED(device->CreateShaderResourceView(a->tex, nullptr, &a->srv))) {
            if (a->tex) a->tex->Release();
            a->tex = nullptr;
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
        if (a->next >= total) return; // 图集已满（调用方兜底：跳过绘制）
        binui_font::GrayGlyph g;
        // 字重必须真的进光栅化：以前 weight 只参与图集 key（同一字号被画成同一份
        // 常规字形），CSS 的 font-weight:700 因此完全不生效。bold=true 走 FreeType
        // 的合成加粗（FT_GlyphSlot_Embolden），图集 key 已含 weight，不会串档。
        bool ok = font.renderGlyph(a->face, a->size, cp, a->weight >= 600, a->italic, g);
        if (!ok && !IsInvisibleCp(cp)) {
            // 缺字 → 画豆腐块：留白会被当成"引擎显示 bug"，空方框才是明确信号
            ok = font.renderMissingGlyph(a->size, a->weight >= 600, a->italic, g);
        }
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
        // 以基线摆放：字形顶距格子顶部 ascent+1 像素（与绘制端 top = baseline - (ascent+1) 对齐）
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
                p[0] = p[1] = p[2] = p[3] = al;   // BGRA 白 + alpha（预乘）
            }
        }

        int cell = a->next++;
        int cx = (cell % a->cols) * a->cellW;
        int cy = (cell / a->cols) * a->cellH;
        D3D11_BOX box = { static_cast<UINT>(cx), static_cast<UINT>(cy), 0,
                          static_cast<UINT>(cx + a->cellW), static_cast<UINT>(cy + a->cellH), 1 };
        ctx->UpdateSubresource(a->tex, 0, &box, rasterBits, static_cast<UINT>(a->cellW) * 4, 0);

        Glyph gl;
        gl.cell = static_cast<uint16_t>(cell);
        // 真实 advance（以前 +1：文字比真实排版宽、组合符号被推离基字）
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
    // 用 HarfBuzz 整形整簇（GSUB 合体/旗帜/肤色），FreeType 枚举 COLR 层合成，
    // 结果（预乘 BGRA）上传进彩色图集格子
    bool rasterColorCluster(ColorAtlas* a, uint64_t key, const uint32_t* cps, size_t n) {
        int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) return false;

        binui_font::ColorGlyph cg;
        if (!font.renderColorCluster(emojiFont, a->size, cps, n, cg)) return false;
        if (cg.width <= 0 || cg.height <= 0 || !cg.bgra) return false;

        ensureRasterDC(a->cellW, a->cellH);
        memset(rasterBits, 0, static_cast<size_t>(a->cellW) * a->cellH * 4);
        // 合成图 → 格子左上角（超出格子部分裁剪）
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
        D3D11_BOX box = { static_cast<UINT>(cx), static_cast<UINT>(cy), 0,
                          static_cast<UINT>(cx + a->cellW), static_cast<UINT>(cy + a->cellH), 1 };
        ctx->UpdateSubresource(a->tex, 0, &box, rasterBits, static_cast<UINT>(a->cellW) * 4, 0);

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
        a->cellW = a->cellH * 4;   // 家庭/旗帜等宽序列（≈2em）留足空间
        a->cols = a->sizePx / a->cellW;
        if (a->cols < 1) { delete a; return nullptr; }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = static_cast<UINT>(a->sizePx);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint8_t> zeros(static_cast<size_t>(a->sizePx) * a->sizePx * 4, 0);
        D3D11_SUBRESOURCE_DATA init = { zeros.data(), static_cast<UINT>(a->sizePx) * 4, 0 };
        if (FAILED(device->CreateTexture2D(&td, &init, &a->tex)) ||
            FAILED(device->CreateShaderResourceView(a->tex, nullptr, &a->srv))) {
            if (a->tex) a->tex->Release();
            a->tex = nullptr;
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

    // 簇度量：len = 簇长，adv = 前进宽，color = 是否彩色图集
    struct ClusterMetric {
        size_t len = 1;
        float adv = 0;
        bool color = false;
    };

    ClusterMetric clusterMetric(float size, int weight, const uint32_t* cps, size_t n,
                                bool italic = false) {
        ClusterMetric m;
        m.len = EmojiClusterLen(cps, n);
        const float fallback = std::ceil(size * 0.6f);
        const int firstFace = faceIdxForCp(cps[0]);
        // 彩色只在该码点确实由 emoji 面覆盖时才尝试（符号走灰度，真 emoji 走 COLR）
        const bool tryColor = fontReady && (m.len > 1 || firstFace == emojiFaceIdx());
        if (tryColor) {
            ColorAtlas* ca = getColorAtlas(size);
            const ColorGlyph* g = ca ? colorGlyphFor(ca, cps, m.len) : nullptr;
            if (g) {
                m.adv = static_cast<float>(g->advW);
                m.color = true;
                return m;
            }
        }
        if (m.len <= 1) {
            Atlas* a = getAtlas(size, weight, firstFace, italic);
            const Glyph* g = a ? glyphFor(a, cps[0]) : nullptr;
            m.adv = g ? static_cast<float>(g->advW) : fallback;
            return m;
        }
        // 降级：逐码点走灰度图集（各自解析到覆盖它的那个面）
        for (size_t i = 0; i < m.len; i++) {
            Atlas* a = getAtlas(size, weight, faceIdxForCp(cps[i]), italic);
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
                    // 填充 = 内部；描边 = 贴着外侧边界往里 1px 的实心带（详见 GL 后端同段注释：
                    // 以前骑在边界上画，靠内半个像素只有 50%，白色焦点环会变成灰线）
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
        rt.corner = r * 2 + 1;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = static_cast<UINT>(t);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { rasterBits, static_cast<UINT>(t) * 4, 0 };
        if (FAILED(device->CreateTexture2D(&td, &init, &rt.tex)) ||
            FAILED(device->CreateShaderResourceView(rt.tex, nullptr, &rt.srv))) {
            if (rt.tex) rt.tex->Release();
            rt.tex = nullptr;
            rt.srv = nullptr;
        }
        return rounds.emplace(key, rt).first->second;
    }

    bool uploadImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
        auto it = images.find(index);
        auto git = imageGen.find(index);
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != nullptr;

        int w = 0, h = 0;
        std::vector<uint8_t> px = DecodeImagePixels(data, size, w, h);
        if (px.empty()) {
            images[index] = nullptr;
            imageGen[index] = generation;
            return false;
        }
        if (it != images.end() && it->second) it->second->Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // DecodeImagePixels 为 BGRA
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { px.data(), static_cast<UINT>(w) * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &init, &tex)) ||
            FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
            if (tex) tex->Release();
            images[index] = nullptr;
            imageGen[index] = generation;
            return false;
        }
        tex->Release();
        images[index] = srv;
        imageGen[index] = generation;
        return true;
    }

    // 原始 RGBA 像素直传纹理（RGBA 字节序，无解码）
    bool uploadImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
        auto it = images.find(index);
        auto git = imageGen.find(index);
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != nullptr;
        if (w <= 0 || h <= 0 || !rgba) return false;
        if (it != images.end() && it->second) it->second->Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // 原始像素为 RGBA
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { rgba, static_cast<UINT>(w) * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &init, &tex)) ||
            FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
            if (tex) tex->Release();
            images[index] = nullptr;
            imageGen[index] = generation;
            return false;
        }
        tex->Release();
        images[index] = srv;
        imageGen[index] = generation;
        return true;
    }

    // 持久离屏帧缓冲（大小 = 窗口客户区）
    bool createOffscreen() {
        if (offSrv) { offSrv->Release(); offSrv = nullptr; }
        if (offRtv) { offRtv->Release(); offRtv = nullptr; }
        if (offTex) { offTex->Release(); offTex = nullptr; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(winW);
        td.Height = static_cast<UINT>(winH);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &offTex)) ||
            FAILED(device->CreateRenderTargetView(offTex, nullptr, &offRtv)) ||
            FAILED(device->CreateShaderResourceView(offTex, nullptr, &offSrv))) {
            if (offTex) { offTex->Release(); offTex = nullptr; }
            if (offRtv) { offRtv->Release(); offRtv = nullptr; }
            if (offSrv) { offSrv->Release(); offSrv = nullptr; }
            return false;
        }
        if (transparent) {
            float tr[4] = { 0, 0, 0, 0 };
            ctx->ClearRenderTargetView(offRtv, tr);
        } else {
            float white[4] = { 1, 1, 1, 1 };
            ctx->ClearRenderTargetView(offRtv, white);
        }
        return true;
    }

    // ---------- 场景 ----------
    void beginBatch() {
        if (curBatch &&
            curBatch->sx == scissor[0] && curBatch->sy == scissor[1] &&
            curBatch->sw == scissor[2] - scissor[0] && curBatch->sh == scissor[3] - scissor[1] &&
            curBatch->linear == curLinear && curBatch->srv == curSrv)
            return;
        Batch b;
        b.first = static_cast<uint32_t>(verts.size()); // 起始顶点索引
        b.count = 0;
        b.sx = scissor[0];
        b.sy = scissor[1];
        b.sw = scissor[2] - scissor[0];
        b.sh = scissor[3] - scissor[1];
        b.linear = curLinear;
        b.srv = curSrv;
        batches.push_back(b);
        curBatch = &batches.back();
    }

    void emitQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, uint32_t color) {
        if (verts.size() + 6 > kMaxVerts) return; // 缓冲上限，丢弃多余
        beginBatch();
        // 0xAARRGGBB → 0xAABBGGRR：R8G8B8A8_UNORM 按内存字节序读（byte0=R）
        uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        D3DVertex v[6] = {
            { x0, y0, sw, u0, v0 }, { x1, y0, sw, u1, v0 }, { x0, y1, sw, u0, v1 },
            { x0, y1, sw, u0, v1 }, { x1, y0, sw, u1, v0 }, { x1, y1, sw, u1, v1 },
        };
        verts.insert(verts.end(), v, v + 6);
        curBatch->count++;
    }

    void emitQuadRot(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3, uint32_t color) {
        if (verts.size() + 6 > kMaxVerts) return;
        beginBatch();
        uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        D3DVertex v[6] = {
            { x0, y0, sw, 0, 0 }, { x1, y1, sw, 1, 0 }, { x2, y2, sw, 1, 1 },
            { x2, y2, sw, 1, 1 }, { x3, y3, sw, 0, 1 }, { x0, y0, sw, 0, 0 },
        };
        verts.insert(verts.end(), v, v + 6);
        curBatch->count++;
    }

    void flushAll() {
        if (verts.empty() || !vb) return;
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (FAILED(ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        memcpy(map.pData, verts.data(), verts.size() * sizeof(D3DVertex));
        ctx->Unmap(vb, 0);

        for (const auto& b : batches) {
            if (b.count == 0) continue;
            D3D11_RECT r = { b.sx, b.sy, b.sx + b.sw, b.sy + b.sh };
            if (r.right <= r.left || r.bottom <= r.top) continue;
            ctx->RSSetScissorRects(1, &r);
            ctx->PSSetShaderResources(0, 1, &b.srv);
            ctx->PSSetSamplers(0, 1, b.linear ? &samLinear : &samPoint);
            ctx->Draw(b.count * 6, b.first);
        }
    }

    // 读回 present：离屏 → staging → CPU 内存 → GDI 贴屏。
    // 透明窗用 UpdateLayeredWindow（预乘 alpha 自己合成），普通窗用 SetDIBitsToDevice。
    // 与 GL/Vulkan 后端的读回路径同一套语义，像素来源仍是同一张 offTex。
    bool presentReadback() {
        const int w = winW, h = winH;
        if (!offTex || w <= 0 || h <= 0) return false;
        if (!stagingTex || layeredW != w || layeredH != h) {
            if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
            if (layeredDib) { DeleteObject(layeredDib); layeredDib = nullptr; }
            if (layeredDC) { DeleteDC(layeredDC); layeredDC = nullptr; }
            D3D11_TEXTURE2D_DESC td = {};
            offTex->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &stagingTex))) {
                std::fprintf(stderr, "[d3d11] staging texture failed\n");
                return false;
            }
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h; // 顶层（top-down）
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            layeredDib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &layeredBits, nullptr, 0);
            layeredDC = CreateCompatibleDC(nullptr);
            layeredW = w;
            layeredH = h;
        }
        if (!stagingTex || !layeredDib || !layeredBits) return false;
        ctx->CopyResource(stagingTex, offTex);
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (FAILED(ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &map))) return false;
        const uint8_t* src = static_cast<const uint8_t*>(map.pData);
        uint8_t* dst = static_cast<uint8_t*>(layeredBits);
        const size_t srcPitch = map.RowPitch;
        const size_t dstPitch = static_cast<size_t>(w) * 4;
        for (int y = 0; y < h; y++) memcpy(dst + y * dstPitch, src + y * srcPitch, dstPitch);
        ctx->Unmap(stagingTex, 0);

        if (transparent) {
            HDC screenDC = GetDC(nullptr);
            HGDIOBJ old = SelectObject(layeredDC, layeredDib);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            POINT dstPt = { wr.left, wr.top };
            SIZE sz = { w, h };
            POINT srcPt = { 0, 0 };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, screenDC, &dstPt, &sz, layeredDC, &srcPt, 0, &bf, ULW_ALPHA);
            SelectObject(layeredDC, old);
            ReleaseDC(nullptr, screenDC);
            return true;
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(hwnd);
        SetDIBitsToDevice(dc, 0, 0, w, h, 0, 0, 0, h, layeredBits, &bi, DIB_RGB_COLORS);
        ReleaseDC(hwnd, dc);
        return true;
    }

    // 客户区是否铺满整块显示器（borderless 全屏）。
    // 与 GL 0.9.1 / Vulkan 后端同一判定与同一理由：这种状态下 DWM 会把呈现提升成
    // 直翻转，实测在远程桌面/虚拟显示器上画面会停住（应用仍在满帧绘制）。
    bool clientCoversMonitor() const {
        if (!hwnd) return false;
        RECT cr;
        if (!GetClientRect(hwnd, &cr)) return false;
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
        const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
        const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        return (cr.right - cr.left) >= mw && (cr.bottom - cr.top) >= mh;
    }

    void shutdown() {
        for (auto& kv : atlases) {            if (kv.second->tex) kv.second->tex->Release();
            if (kv.second->srv) kv.second->srv->Release();
            delete kv.second;
        }
        atlases.clear();
        for (auto& kv : colorAtlases) {
            if (kv.second->tex) kv.second->tex->Release();
            if (kv.second->srv) kv.second->srv->Release();
            delete kv.second;
        }
        colorAtlases.clear();
        for (auto& kv : rounds) {
            if (kv.second.tex) kv.second.tex->Release();
            if (kv.second.srv) kv.second.srv->Release();
        }
        rounds.clear();
        for (auto& kv : images) {
            if (kv.second) kv.second->Release();
        }
        images.clear();
        if (whiteSrv) { whiteSrv->Release(); whiteSrv = nullptr; }
        if (offSrv) { offSrv->Release(); offSrv = nullptr; }
        if (offRtv) { offRtv->Release(); offRtv = nullptr; }
        if (offTex) { offTex->Release(); offTex = nullptr; }
        if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
        if (layeredDib) { DeleteObject(layeredDib); layeredDib = nullptr; }
        if (layeredDC) { DeleteDC(layeredDC); layeredDC = nullptr; }
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (samPoint) { samPoint->Release(); samPoint = nullptr; }
        if (samLinear) { samLinear->Release(); samLinear = nullptr; }
        if (blend) { blend->Release(); blend = nullptr; }
        if (ras) { ras->Release(); ras = nullptr; }
        if (il) { il->Release(); il = nullptr; }
        if (vs) { vs->Release(); vs = nullptr; }
        if (ps) { ps->Release(); ps = nullptr; }
        if (cb) { cb->Release(); cb = nullptr; }
        if (vb) { vb->Release(); vb = nullptr; }
        if (swap) { swap->Release(); swap = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        ready = false;
    }
};

// ============================================================
// BinUID3D
// ============================================================
BinUID3D::BinUID3D() : impl_(new Impl()) {}
BinUID3D::~BinUID3D() { delete impl_; }

bool BinUID3D::init(void* hwnd, int width, int height, bool vsync) {
    impl_->hwnd = static_cast<HWND>(hwnd);
    impl_->winW = width;
    impl_->winH = height;
    impl_->vsync = vsync;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = width;
    impl_->scissor[3] = height;

    if (!impl_->createDeviceAndSwapchain()) { impl_->shutdown(); return false; }
    if (!impl_->compileShaders()) { impl_->shutdown(); return false; }
    if (!impl_->createStates()) { impl_->shutdown(); return false; }
    if (!impl_->createOffscreen()) { impl_->shutdown(); return false; }
    impl_->fontReady = impl_->font.init();
    binui_font::registerSystemFonts(impl_->font);   // 失败仅降级：文本/emoji 不绘制
    impl_->ready = true;
    impl_->coversMonitor = impl_->clientCoversMonitor();
    float v[2] = { static_cast<float>(width), static_cast<float>(height) };
    impl_->ctx->UpdateSubresource(impl_->cb, 0, nullptr, v, 0, 0);
    return true;
}

void BinUID3D::shutdown() { impl_->shutdown(); }
bool BinUID3D::isReady() const { return impl_->ready; }
int BinUID3D::width() const { return impl_->winW; }
int BinUID3D::height() const { return impl_->winH; }
void BinUID3D::setVsync(bool on) { impl_->vsync = on; }

void BinUID3D::setTransparent(bool t) { impl_->transparent = t; }

void BinUID3D::resize(int width, int height) {
    if (!impl_->ready || (width == impl_->winW && height == impl_->winH)) return;
    impl_->winW = width;
    impl_->winH = height;
    if (impl_->swap) {
        HRESULT hr = impl_->swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                                DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[d3d11] ResizeBuffers hr=0x%08X (retry)\n", (unsigned)hr);
            // 重试一次（快速连续 resize 时 Present 尚未完成可能失败）
            hr = impl_->swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                            DXGI_FORMAT_UNKNOWN, 0);
        }
        impl_->createRenderTarget();
    }
    impl_->createOffscreen(); // 离屏随窗口重建（内容丢弃，下帧全量重绘）
    impl_->coversMonitor = impl_->clientCoversMonitor();
    float v[2] = { static_cast<float>(width), static_cast<float>(height) };
    impl_->ctx->UpdateSubresource(impl_->cb, 0, nullptr, v, 0, 0);
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = width;
    impl_->scissor[3] = height;
}

void BinUID3D::beginFrame(const void* clipRect) {
    // 不在这里整帧 ClearRenderTargetView：该 API 不受 scissor 限制，会清空整个离屏，
    // 局部重绘时把脏区以外的持久像素抹掉（悬停后其余元素消失的根因）。
    // 脏区底色由共享遍历里根节点的背景填充提供（与 GL/Vulkan 同一份 drawNodesBinUI）。
    (void)clipRect;
    if (!impl_->ready) return;
    impl_->verts.clear();
    impl_->batches.clear();
    impl_->curBatch = nullptr;
    impl_->scissorDepth = 0;
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = impl_->winW;
    impl_->scissor[3] = impl_->winH;

    // 绘制目标 = 持久离屏帧缓冲
    impl_->ctx->OMSetRenderTargets(1, &impl_->offRtv, nullptr);

    // 不在此整帧 ClearRenderTargetView：该 API 不受 scissor 限制，会清空整个离屏，
    // 局部重绘时把脏区以外的持久像素抹白/抹透明（悬停后其余元素消失的根因）。
    // 脏区底色由 drawNodesD3D 的根节点背景填充（按 clipRect 范围）提供。
}

void BinUID3D::present() {
    if (!impl_->ready) return;
    impl_->flushAll(); // 画到离屏帧缓冲
    if (std::getenv("BINUI_D3D_TRACE"))
        std::fprintf(stderr, "[d3d] present: verts=%zu batches=%zu transparent=%d forceGdi=%d covers=%d\n",
                     impl_->verts.size(), impl_->batches.size(), (int)impl_->transparent,
                     (int)impl_->forceGdi, (int)impl_->coversMonitor);

    // 读回呈现的三条触发条件与 GL/Vulkan 后端同一套理由：
    // 透明窗口必须自己合成（交换链拿不到预乘 alpha）；客户区铺满整块显示器时 DWM 会把
    // 呈现提升成直翻转（远程桌面/虚拟显示器上实测画面会停住）；拖拽 resize 期由上层强制。
    if (impl_->transparent || impl_->forceGdi || impl_->coversMonitor) {
        impl_->presentReadback();
        return;
    }

    // 整帧 blit：离屏 → 交换链 backbuffer
    impl_->ctx->OMSetRenderTargets(1, &impl_->rtv, nullptr);
    D3D11_RECT full = { 0, 0, impl_->winW, impl_->winH };
    impl_->ctx->RSSetScissorRects(1, &full);
    impl_->ctx->PSSetShaderResources(0, 1, &impl_->offSrv);
    impl_->ctx->PSSetSamplers(0, 1, &impl_->samLinear);
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(impl_->ctx->Map(impl_->vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        float wf = static_cast<float>(impl_->winW);
        float hf = static_cast<float>(impl_->winH);
        D3DVertex v[6] = {
            { 0, 0, 0xFFFFFFFF, 0, 0 }, { wf, 0, 0xFFFFFFFF, 1, 0 }, { 0, hf, 0xFFFFFFFF, 0, 1 },
            { 0, hf, 0xFFFFFFFF, 0, 1 }, { wf, 0, 0xFFFFFFFF, 1, 0 }, { wf, hf, 0xFFFFFFFF, 1, 1 },
        };
        memcpy(map.pData, v, sizeof(v));
        impl_->ctx->Unmap(impl_->vb, 0);
        impl_->ctx->Draw(6, 0);
    }
    HRESULT hr = impl_->swap->Present(impl_->vsync ? 1 : 0, 0);
    if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED)
        std::fprintf(stderr, "[d3d11] Present hr=0x%08X\n", (unsigned)hr);
}

// ---------- 原语 ----------
void BinUID3D::fillRect(float x, float y, float w, float h, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
    Premult(color);
    impl_->emitQuad(x, y, x + w, y + h, 0, 0, 1, 1, color);
}

void BinUID3D::fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    if (!impl_->ready || width <= 0) return;
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float px = -dy / len * width * 0.5f;
    float py = dx / len * width * 0.5f;
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
    Premult(color);
    impl_->emitQuadRot(x0 + px, y0 + py, x1 + px, y1 + py,
                       x1 - px, y1 - py, x0 - px, y0 - py, color);
}

void BinUID3D::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    int r = static_cast<int>(radius);
    r = std::max(0, std::min(r, static_cast<int>(std::min(w, h) / 2)));
    if (r <= 0) { fillRect(x, y, w, h, color); return; }

    RoundTex& rt = impl_->roundFor(r, false);
    if (!rt.srv) { fillRect(x, y, w, h, color); return; }

    float t = static_cast<float>(rt.t);
    float cu = static_cast<float>(rt.corner) / t;
    float strip0 = 0.5f;
    float strip1 = 0.5f + 1.0f / t;
    float c = static_cast<float>(rt.corner);

    Premult(color);
    impl_->curLinear = 1;
    impl_->curSrv = rt.srv;
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
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
}

void BinUID3D::strokeRounded(float x, float y, float w, float h, float radius,
                             float thickness, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    int r = static_cast<int>(radius);
    r = std::max(0, std::min(r, static_cast<int>(std::min(w, h) / 2)));
    if (r <= 0) { fillRect(x, y, w, h, color); return; }

    // 线宽：不再丢弃 thickness —— stroke 纹理是 1px 环，按厚度画同心层叠出粗描边
    // （GL 后端同款做法，保证"2px 编辑环"三端一致）。
    int layers = static_cast<int>(std::lround(std::max(1.0f, thickness)));
    if (layers < 1) layers = 1;

    Premult(color);
    impl_->curLinear = 1;
    for (int k = 0; k < layers; k++) {
        const float ex = x + k, ey = y + k;
        const float ew = w - 2.0f * k, eh = h - 2.0f * k;
        if (ew <= 1.0f || eh <= 1.0f) break;
        const int rr = std::max(1, r - k);
        RoundTex& rt = impl_->roundFor(rr, true);
        if (!rt.srv) break;

        const float t = static_cast<float>(rt.t);
        const float cu = static_cast<float>(rt.corner) / t;
        const float strip0 = 0.5f;
        const float strip1 = 0.5f + 1.0f / t;
        const float c = static_cast<float>(rt.corner);
        impl_->curSrv = rt.srv;
        auto quad = [&](float qx, float qy, float qw, float qh,
                        float u0, float v0, float u1, float v1) {
            impl_->emitQuad(qx, qy, qx + qw, qy + qh, u0, v0, u1, v1, color);
        };
        quad(ex, ey, c, c, 0, 0, cu, cu);
        quad(ex + ew - c, ey, c, c, 1 - cu, 0, 1, cu);
        quad(ex, ey + eh - c, c, c, 0, 1 - cu, cu, 1);
        quad(ex + ew - c, ey + eh - c, c, c, 1 - cu, 1 - cu, 1, 1);
        quad(ex + c, ey, ew - 2 * c, c, strip0, 0, strip1, cu);
        quad(ex + c, ey + eh - c, ew - 2 * c, c, strip0, 1 - cu, strip1, 1);
        quad(ex, ey + c, c, eh - 2 * c, 0, strip0, cu, strip1);
        quad(ex + ew - c, ey + c, c, eh - 2 * c, 1 - cu, strip0, 1, strip1);
        quad(ex + c, ey + c, ew - 2 * c, eh - 2 * c, strip0, strip0, strip1, strip1);
    }
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
}

float BinUID3D::textWidth(float size, int weight, const std::string& utf8, bool italic) {
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

void BinUID3D::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    Atlas* a = impl_->getAtlas(size, weight, 0, italic);   // 度量用主面
    if (!a) { ascent = lineHeight = static_cast<int>(size); return; }
    ascent = a->ascent;
    lineHeight = a->ascent + a->descent;
}

void BinUID3D::drawText(float x, float y, float size, int weight,
                        const std::string& utf8, uint32_t color,
                        bool centerH, bool centerV, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    float w = textWidth(size, weight, utf8, italic);
    if (centerH) x -= w / 2;

    int ascent = 0, lineH = 0;
    fontMetrics(size, weight, ascent, lineH, italic);
    if (centerV) y -= lineH / 2.0f;
    // 像素对齐：字形位图按整数像素设计，半像素定位会被双线性过滤糊掉；
    // advance 是整数，所以起点/基线取整后整行字形都落在整数像素上
    float baseline = std::floor(y + 0.5f);
    x = std::floor(x + 0.5f);

    std::vector<uint32_t> cps;
    Utf8Iter it{ utf8 };
    while (it.next()) cps.push_back(it.cp);

    Premult(color);
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
    float cx = x;
    auto drawGray = [&](uint32_t cp) {
        Atlas* a = impl_->getAtlas(size, weight, impl_->faceIdxForCp(cp), italic);
        if (!a) return;
        const Glyph* g = impl_->glyphFor(a, cp);
        if (!g) return;
        float t = static_cast<float>(a->sizePx);
        float cell = static_cast<float>(a->cellW);
        float gx = (g->cell % a->cols) * cell;
        float gy = (g->cell / a->cols) * cell;
        float top = baseline - (a->ascent + 1);
        impl_->curSrv = a->srv;
        impl_->emitQuad(cx, top, cx + cell, top + cell,
                        gx / t, gy / t, (gx + cell) / t, (gy + cell) / t, color);
        cx += g->advW;
    };

    size_t i = 0;
    while (i < cps.size()) {
        size_t n = EmojiClusterLen(cps.data() + i, cps.size() - i);
        bool colorCp = impl_->fontReady &&
                       (n > 1 || impl_->faceIdxForCp(cps[i]) == impl_->emojiFaceIdx());
        if (colorCp) {
            ColorAtlas* ca = impl_->getColorAtlas(size);
            const ColorGlyph* g = ca ? impl_->colorGlyphFor(ca, cps.data() + i, n) : nullptr;
            if (g) {
                float t = static_cast<float>(ca->sizePx);
                float cell = static_cast<float>(ca->cellW);
                float gx = (g->cell % ca->cols) * cell;
                float gy = (g->cell / ca->cols) * ca->cellH;
                float top = baseline - g->baseline;
                impl_->curSrv = ca->srv;
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
    impl_->curSrv = impl_->whiteSrv;
}

// 行距取 gWrappedLineStep（0 = 字体自身的行高），由 LABEL 多行绘制按 line-height 设置
void BinUID3D::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                               const std::string& utf8, uint32_t color, int align,
                               bool italic) {
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
            drawText(lx, ly + ascent, size, weight, seg, color);
        }
        ly += (gWrappedLineStep > 0.0f) ? gWrappedLineStep : static_cast<float>(lineH);
        pos = newline ? end + 1 : end;
    }
}

bool BinUID3D::prepareImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImage(index, data, size, generation);
}

bool BinUID3D::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImageRaw(index, w, h, rgba, generation);
}

void BinUID3D::drawImage(uint16_t index, float x, float y, float w, float h) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    auto it = impl_->images.find(index);
    if (it == impl_->images.end() || !it->second) return;
    impl_->curLinear = 1;
    impl_->curSrv = it->second;
    impl_->emitQuad(x, y, x + w, y + h, 0, 0, 1, 1, 0xFFFFFFFF);
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
}

int BinUID3D::pushClip(float x, float y, float w, float h) {
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

void BinUID3D::popClipTo(int depth) {
    impl_->scissorDepth = depth;
    if (impl_->scissorDepth == 0) {
        impl_->scissor[0] = 0;
        impl_->scissor[1] = 0;
        impl_->scissor[2] = impl_->winW;
        impl_->scissor[3] = impl_->winH;
    }
}

void BinUID3D::setFontName(const char* name) {
    if (name && *name) impl_->asciiFont = name;
}


// ============================================================
// 读回 + GDI 贴屏（拖拽 resize 期 / 全屏直翻转守卫 / 无交换链降级）
// ============================================================
void BinUID3D::presentViaGdi() {
    if (!impl_->ready) return;
    impl_->forceGdi = true;
    present();
    impl_->forceGdi = false;
}

const uint8_t* BinUID3D::readbackPixels(int& outW, int& outH) {
    if (!impl_->ready) return nullptr;
    if (!impl_->presentReadback()) return nullptr;
    outW = impl_->winW;
    outH = impl_->winH;
    return static_cast<const uint8_t*>(impl_->layeredBits);
}
