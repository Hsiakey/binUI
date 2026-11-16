// binui_d3d11.cpp - D3D11 GPU 渲染后端实现
// 架构：BinUID3D 维护字形图集（FreeType 光栅化一次）+ 圆角九宫格模板 + 顶点批次，
// drawNodesD3D 与 drawNodesGDI 语义一致地遍历节点树，全部绘制为 GPU 四边形，
// BinUIAppD3D 提供无边框窗口与事件循环（VSync 按需呈现）。
// 文字排版/光栅化/图集来自共享的 backends/binui_glyph_atlas.h（与 GL / Vulkan 同一份），
// 本后端只负责"位图 → D3D11 纹理"和三端一致的顶点发射。
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

#include "backends/binui_glyph_atlas.h"   // 字形图集 + 排版（GL / Vulkan / D3D11 共用）
#include "backends/binui_win_image.h"     // GDI+ 图片解码（GL / Vulkan / D3D11 共用）
// v13 扩展绘制段的共用几何（渐变色带 / 四边边框 / 文字装饰线 / 行高）：
// 与 GDI、GL 后端共用同一份数学，三端像素表现才一致
#include "binui_paint.h"
#include "binui_filter.h"      // v26：CSS filter 颜色矩阵（解码后逐像素应用）
#include "platform/binui_win32_foreground.h"

using namespace binui;

// mingw-w64 无 __uuidof，显式 GUID
static const IID IID_XDXGIDevice   = { 0x54EC77FA, 0x1377, 0x44E6, { 0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C } };
static const IID IID_XDXGIAdapter  = { 0x2411E7E1, 0x12AC, 0x4CCF, { 0xBD,0x14,0x97,0x98,0xE8,0x53,0x4D,0xC0 } };
static const IID IID_XDXGIFactory2 = { 0x50C83A1C, 0xE072, 0x4C48, { 0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0 } };
static const IID IID_XTexture2D    = { 0x6F15AAF2, 0xD208, 0x4E89, { 0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C } };

namespace {

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
    uint8_t wrap = 0;    // v0.16.28：1 = REPEAT 采样（颗粒噪声平铺）
    ID3D11ShaderResourceView* srv = nullptr;
};

struct RoundTex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int t = 0;         // 模板边长（RoundTemplateSize(r)）
    int corner = 0;    // 角区边长（2r+1）
    uint64_t lastUse = 0;   // LRU 时钟（缓存上限 48 张）
};

// 顶点缓冲：初始 10 万顶点（2 MB，约 1.66 万四边形），装不下就按需重建更大的，
// 硬上限 100 万顶点（20 MB）。以前满了就丢图元 —— 一屏图标超过约 34 个就凭空少画，
// 且不报错（docs/future.md P0-5）。
constexpr size_t kMaxVerts = 100000;
constexpr size_t kMaxVertsCeiling = 1u << 20;

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
    size_t vbCapVerts = 0;              // vb 目前能装多少顶点（扩容后跟着变大）
    ID3D11SamplerState* samPoint = nullptr;
    ID3D11SamplerState* samLinear = nullptr;
    ID3D11SamplerState* samPointWrap = nullptr;   // v0.16.28：颗粒噪声平铺（REPEAT + POINT）
    ID3D11ShaderResourceView* noiseSrv = nullptr; // v0.16.28：64×64 确定性噪声
    ID3D11BlendState* blend = nullptr;
    ID3D11BlendState* blendErase = nullptr;   // v27：clip-path 的 dst *= 1-srcA
    ID3D11BlendState* blendKeep = nullptr;    // v28：mask 保留趟的 dst *= srcA
    ID3D11BlendState* blendReplace = nullptr; // v29：毛玻璃合成的"直接覆盖"（ONE/ZERO）
    ID3D11RasterizerState* ras = nullptr;
    ID3D11ShaderResourceView* whiteSrv = nullptr; // 1×1 纯白（纯色四边形）
    // 持久离屏帧缓冲：局部重绘只更新脏区，present 时整帧 blit 到交换链
    ID3D11Texture2D* offTex = nullptr;
    ID3D11RenderTargetView* offRtv = nullptr;
    ID3D11ShaderResourceView* offSrv = nullptr;
    // 4× MSAA 绘制目标（持久累积帧）+ 它的 RTV；msSamples==1 时 msTarget 指向 offRtv
    ID3D11Texture2D* msTex = nullptr;
    ID3D11RenderTargetView* msRtv = nullptr;
    ID3D11RenderTargetView* msTarget = nullptr;
    int msSamples = 1;

    // 场景
    std::vector<D3DVertex> verts;      // 预分配到 kMaxVerts 的本帧顶点缓冲
    size_t vcount = 0;                 // 本帧已写顶点数
    std::vector<Batch> batches;
    // 同 GL：batches 是 vector，存下标而不是指针（push_back 扩容后 &back() 会悬空）
    size_t curBatch = static_cast<size_t>(-1);
    int scissor[4] = { 0, 0, 0, 0 };
    int scissorDepth = 0;
    uint8_t curLinear = 0;
    uint8_t curWrap = 0;   // v0.16.28：这块批次要不要 REPEAT 采样
    // v0.16.18 仿射状态：设置后 emitQuad/emitQuadRot 的四角都过这个 2×3 矩阵
    bool affineOn = false;
    float aff[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    void affPoint(float lx, float ly, float& sx, float& sy) const {
        sx = aff[0] * lx + aff[1] * ly + aff[2];
        sy = aff[3] * lx + aff[4] * ly + aff[5];
    }
    void affCorners(float x0, float y0, float x1, float y1, float* p) const {
        affPoint(x0, y0, p[0], p[1]);
        affPoint(x1, y0, p[2], p[3]);
        affPoint(x0, y1, p[4], p[5]);
        affPoint(x1, y1, p[6], p[7]);
    }

    uint8_t subpixelText = 0;   // setTextSubpixel(true)：字形批次走 LINEAR 采样
    ID3D11ShaderResourceView* curSrv = nullptr;


    // 圆角模板（按 radius；负 key = 描边）
    std::unordered_map<int, RoundTex> rounds;
    std::vector<RoundTex> pendingRoundRelease;   // 帧内淘汰的圆角模板：flushAll 之后才 Release
    uint64_t roundClock = 0;   // 圆角模板缓存 LRU 时钟

    // 图片纹理缓存（按 imageIndex）
    std::unordered_map<uint16_t, ID3D11ShaderResourceView*> images;
    // v25 九宫格要源图像素尺寸
    std::unordered_map<uint64_t, std::pair<int,int>> imageDims;
    // v0.16.20：原始像素纹理（视频帧/程序化纹理）的 mip 是否已补（见 uploadImageRaw）
    std::unordered_map<uint64_t, uint8_t> rawMips;
    std::unordered_map<uint64_t, uint32_t> imageGen; // 换图 generation：不一致时重新上传
    // v26：当前节点的图片滤镜（setImageFilter 设、解码时应用、按哈希进缓存键）
    binui::ColorMatrix imgFilter;
    uint64_t imgFilterKey = 0;
    uint64_t imgKey(uint16_t index) const {
        return static_cast<uint64_t>(index) | (imgFilterKey << 16);
    }

    // 字形图集 + 文本排版：整份逻辑在 backends/binui_glyph_atlas.h（GL / Vulkan / D3D11 共用）。
    // 句柄直接把 ID3D11ShaderResourceView* 装进 void*（SRV 与图集一一对应）。
    struct GlyphTexHost : IGlyphTexHost {
        Impl* owner = nullptr;
        void* createTex(int w, int h) override;
        void updateTex(void* tex, int x, int y, int w, int h, const uint8_t* bgra) override;
        void destroyTex(void* tex) override;
    };
    GlyphTexHost glyphHost;
    GlyphAtlas atlas;
    std::unordered_map<void*, ID3D11Texture2D*> glyphTexOf;   // SRV 句柄 → 纹理

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
        vbCapVerts = kMaxVerts;

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
        // v0.16.15：切到三线性（带 mip 插值）。单级纹理（字形图集/离屏目标/视频帧）
        // 没有 mip 链，MIN_MAG_MIP_LINEAR 与原来的 MIN_MAG_LINEAR 逐像素相同。
        smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        shr = device->CreateSamplerState(&smd, &samLinear);
        if (FAILED(shr)) {
            std::fprintf(stderr, "[d3d11] sampler linear failed hr=0x%08X removed=0x%08X\n",
                         (unsigned)shr, (unsigned)device->GetDeviceRemovedReason());
            return false;
        }
        {
            D3D11_SAMPLER_DESC swd = {};
            swd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            swd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            swd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            swd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            swd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&swd, &samPointWrap))) {
                std::fprintf(stderr, "[d3d11] sampler wrap failed\n");
                return false;
            }
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
    // v28 遮罩保留趟：color/alpha 都按 dst *= srcA 混合（掩码内保留）
    D3D11_BLEND_DESC bd3 = {};
    bd3.RenderTarget[0].BlendEnable = TRUE;
    bd3.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd3.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
    bd3.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd3.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd3.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    bd3.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd3.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd3, &blendKeep);
    // v29 毛玻璃合成：直接覆盖（背景被替换成模糊版）
    D3D11_BLEND_DESC bdr = {};
    bdr.RenderTarget[0].BlendEnable = TRUE;
    bdr.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bdr.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bdr.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bdr.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bdr.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bdr.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bdr.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bdr, &blendReplace);
    // v27 裁剪擦除：color/alpha 都按 dst *= (1-srcA) 混合
    D3D11_BLEND_DESC bd2 = {};
    bd2.RenderTarget[0].BlendEnable = TRUE;
    bd2.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd2.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd2.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd2.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd2.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd2.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd2.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd2, &blendErase);
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

    RoundTex& roundFor(int radius, bool stroke) {
        int r = std::max(1, radius);
        int key = stroke ? -r : r;
        auto it = rounds.find(key);
        if (it != rounds.end()) {
            it->second.lastUse = ++roundClock;
            return it->second;
        }

        // 模板尺寸与覆盖率算法**必须**和 GL / Vulkan 一致（RoundTemplateSize 在
        // binui_paint.h 里）：三端共用 BuildRoundedNineSlice 拼九宫格，模板边长差 1px、
        // 或形状在内缩过的盒子里铺（旧代码 half = (t-2)/2）都会让圆角边缘差一圈像素。
        int t = RoundTemplateSize(r);
        if (!ensureRasterDC(t, t)) {
            std::fprintf(stderr, "[d3d11] roundFor: 光栅化缓冲分配失败 (%dx%d)\n", t, t);
            return emptyRound_;
        }
        memset(rasterBits, 0, static_cast<size_t>(t) * t * 4);

        // 解析式逐像素覆盖率（1px 抗锯齿边缘），替代 GDI RoundRect（无 AA，转角锯齿明显）。
        // 形状铺满整个模板 [0,t]：圆弧包围盒正好是 [0,r]，角片边长与弧长严格对应，
        // 半径到短边一半时四角才能无缝拼成圆/胶囊。
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
        rt.lastUse = ++roundClock;
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
        // 缓存上限（半径种类一多会一路涨显存）：超了按 LRU 淘汰，与 GL 后端同一策略
        if (rounds.size() >= 48) {
            auto victim = rounds.begin();
            for (auto k = rounds.begin(); k != rounds.end(); ++k)
                if (k->second.lastUse < victim->second.lastUse) victim = k;
            // 与 GL 同一坑：本帧已录进批次的四边形还指着这个 SRV/纹理，帧内 Release
            // 会把对象真删掉，批次就悬空。挪到 flushAll 之后再释放。
            pendingRoundRelease.push_back(victim->second);
            rounds.erase(victim);
        }
        return rounds.emplace(key, rt).first->second;
    }

    // roundFor 失败时的空返回（调用方据此退回 fillRect）
    RoundTex emptyRound_;

    bool uploadImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
        auto it = images.find(imgKey(index));
        auto git = imageGen.find(imgKey(index));
        if (it != images.end() && git != imageGen.end() && git->second == generation)
            return it->second != nullptr;

        int w = 0, h = 0;
        std::vector<uint8_t> px = DecodeImagePixels(data, size, w, h);
        // v26：CSS filter 的颜色矩阵在解码后、生成 mip 链之前跑一遍
        if (!px.empty() && imgFilterKey)
            binui::ApplyColorMatrixBGRA(px.data(), w, h, imgFilter);
        if (px.empty()) {
            images[imgKey(index)] = nullptr;
            imageGen[imgKey(index)] = generation;
            return false;
        }
        if (it != images.end() && it->second) it->second->Release();
        // v0.16.15（P1-8）：图片生成完整 mip 链（缩小时不再"4 个纹素代表上百像素"）。
        // GenerateMips 要求 MipLevels=0 + RENDER_TARGET 绑定 + GENERATE_MIPS 标志，
        // 且只能喂第 0 级的初始数据。采样器那边切成 MIN_MAG_MIP_LINEAR —— 单级纹理
        // （字形图集/离屏目标）行为不变，等同原来的 MIN_MAG_LINEAR。
        const bool wantMip = (w >= 2 && h >= 2);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = wantMip ? 0 : 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // DecodeImagePixels 为 BGRA
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (wantMip ? D3D11_BIND_RENDER_TARGET : 0);
        td.MiscFlags = wantMip ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        D3D11_SUBRESOURCE_DATA init = { px.data(), static_cast<UINT>(w) * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        // 带 mip 的纹理**不能**在 CreateTexture2D 里只喂第 0 级（D3D11 要求 pInitialData
        // 覆盖整条链，否则创建直接失败 → 图片整块消失，实测过）。改成先建空纹理，
        // UpdateSubresource 写第 0 级，再 GenerateMips。
        if (wantMip) {
            if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)) || !tex ||
                FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
                if (tex) tex->Release();
                images[imgKey(index)] = nullptr;
                imageGen[imgKey(index)] = generation;
                return false;
            }
            ctx->UpdateSubresource(tex, 0, nullptr, px.data(), static_cast<UINT>(w) * 4, 0);
            ctx->GenerateMips(srv);
        } else if (FAILED(device->CreateTexture2D(&td, &init, &tex)) ||
                   FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
            if (tex) tex->Release();
            images[imgKey(index)] = nullptr;
            imageGen[imgKey(index)] = generation;
            return false;
        }
        tex->Release();
        imageDims[imgKey(index)] = { w, h };    // v25：九宫格切分用
        images[imgKey(index)] = srv;
        imageGen[imgKey(index)] = generation;
        return true;
    }

    // 原始 RGBA 像素直传纹理（RGBA 字节序，无解码）
    bool uploadImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
        auto it = images.find(imgKey(index));
        auto git = imageGen.find(imgKey(index));
        // v0.16.20：同一个 generation 又来了一次 = 这份原始像素不再每帧变（视频帧的
        // generation 每次都变，走不到这里）。此时**重建**成带完整 mip 链的纹理并
        // GenerateMips —— 程序化纹理（棋盘/渐变/噪声）缩小时才有 mip 可用，而视频帧
        // 不必为每帧的 GenerateMips 付钱。D3D11 的 mip 链不能在创建后追加，所以要重建。
        if (it != images.end() && git != imageGen.end() && git->second == generation &&
            it->second && !rawMips.count(imgKey(index)) && w >= 2 && h >= 2 && rgba) {
            std::vector<uint8_t> px(rgba, rgba + static_cast<size_t>(w) * h * 4);
            it->second->Release();
            images[imgKey(index)] = nullptr;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = static_cast<UINT>(w);
            td.Height = static_cast<UINT>(h);
            td.MipLevels = 0;   // 0 = 完整链（必须配合 GENERATE_MIPS + RENDER_TARGET）
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ID3D11Texture2D* tex = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &tex)) && tex &&
                SUCCEEDED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
                ctx->UpdateSubresource(tex, 0, nullptr, px.data(), static_cast<UINT>(w) * 4, 0);
                ctx->GenerateMips(srv);
                images[imgKey(index)] = srv;
                rawMips[imgKey(index)] = 1;
            }
            if (tex) tex->Release();
            return images[imgKey(index)] != nullptr;
        }
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
            images[imgKey(index)] = nullptr;
            imageGen[imgKey(index)] = generation;
            return false;
        }
        tex->Release();
        imageDims[imgKey(index)] = { w, h };    // v25：九宫格切分用
        images[imgKey(index)] = srv;
        rawMips.erase(index);
        imageGen[imgKey(index)] = generation;
        return true;
    }

    // 持久离屏帧缓冲（大小 = 窗口客户区）
    // 持久离屏帧缓冲：绘制目标是 4× MSAA 纹理（脏矩形只重绘局部，内容跨帧保留），
    // present / 读回前解析（Resolve）到单采样 offTex —— 与 GL 后端的
    // msFbo → offTex、Vulkan 后端的 msaa → resolve 是同一条路线，所以三端边缘 AA 同源。
    bool createOffscreen() {
        if (offSrv) { offSrv->Release(); offSrv = nullptr; }
        if (offRtv) { offRtv->Release(); offRtv = nullptr; }
        if (offTex) { offTex->Release(); offTex = nullptr; }
        if (msRtv) { msRtv->Release(); msRtv = nullptr; }
        if (msTex) { msTex->Release(); msTex = nullptr; }

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

        // 多采样绘制目标。驱动不支持 4× 时退回单采样（msTex 指向 offTex 的兄弟：直接复用 offTex）
        D3D11_TEXTURE2D_DESC md = td;
        md.BindFlags = D3D11_BIND_RENDER_TARGET;
        md.SampleDesc.Count = 4;
        md.SampleDesc.Quality = 0;
        if (FAILED(device->CreateTexture2D(&md, nullptr, &msTex)) ||
            FAILED(device->CreateRenderTargetView(msTex, nullptr, &msRtv))) {
            std::fprintf(stderr, "[d3d11] 4x MSAA 离屏创建失败，退回单采样\n");
            if (msRtv) { msRtv->Release(); msRtv = nullptr; }
            if (msTex) { msTex->Release(); msTex = nullptr; }
            msSamples = 1;
            msTarget = offRtv;
            if (offRtv) offRtv->AddRef();
        } else {
            msSamples = 4;
            msTarget = msRtv;
        }
        if (transparent) {
            float tr[4] = { 0, 0, 0, 0 };
            ctx->ClearRenderTargetView(msTarget, tr);
        } else {
            float white[4] = { 1, 1, 1, 1 };
            ctx->ClearRenderTargetView(msTarget, white);
        }
        return true;
    }

    // 把 MSAA 目标解析到单采样 offTex（present / 读回前的最后一步）
    void resolveOffscreen() {
        if (msSamples > 1 && msTex && offTex)
            ctx->ResolveSubresource(offTex, 0, msTex, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    }

    // ---------- 场景 ----------
    // 本帧顶点写入口（v20）：缓冲一次扩容到 kMaxVerts，之后每帧只复位 vcount、
    // 按指针偏移写。以前是 verts.insert(verts.end(), v, v+6)，每条图元都要做容量
    // 检查 + size 递增，和 GL/Vulkan 后端同一个坑（详见 binui_gl-gdi.cpp 的注释）。
    D3DVertex* vertRoom(size_t n) {
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
                     "[d3d11] 顶点数撞上硬上限（%zu 个四边形/帧），后面的图元被丢弃，"
                     "见 docs/future.md P0-5\n", kMaxVertsCeiling / 6);
    }

    // 顶点缓冲按需变大：D3D11 的动态 VB 尺寸建好就固定，所以只有"本帧顶点数超过
    // 现有 VB 容量"时才重建一次（之后一直复用它，不会每帧重建）。
    bool ensureVbCapacity(size_t needVerts) {
        if (vb && vbCapVerts >= needVerts) return true;
        size_t cap = vbCapVerts ? vbCapVerts : kMaxVerts;
        while (cap < needVerts && cap < kMaxVertsCeiling) cap *= 2;
        if (cap > kMaxVertsCeiling) cap = kMaxVertsCeiling;
        if (cap < needVerts) { warnVertsOverflow(); return false; }

        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = static_cast<UINT>(cap * sizeof(D3DVertex));
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* nb = nullptr;
        if (FAILED(device->CreateBuffer(&vbd, nullptr, &nb))) {
            std::fprintf(stderr, "[d3d11] VB 扩容失败（%zu 顶点）\n", cap);
            return false;
        }
        if (vb) vb->Release();
        vb = nb;
        vbCapVerts = cap;
        // 输入装配阶段引用的是缓冲对象本身，换了必须重新绑
        UINT stride = sizeof(D3DVertex), off = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        return true;
    }

    void beginBatch() {
        if (curBatch != static_cast<size_t>(-1) &&
            batches[curBatch].sx == scissor[0] && batches[curBatch].sy == scissor[1] &&
            batches[curBatch].sw == scissor[2] - scissor[0] && batches[curBatch].sh == scissor[3] - scissor[1] &&
            batches[curBatch].linear == curLinear && batches[curBatch].srv == curSrv &&
            batches[curBatch].wrap == curWrap)
            return;
        Batch b;
        b.first = static_cast<uint32_t>(vcount); // 起始顶点索引
        b.count = 0;
        b.sx = scissor[0];
        b.sy = scissor[1];
        b.sw = scissor[2] - scissor[0];
        b.sh = scissor[3] - scissor[1];
        b.linear = curLinear;
        b.srv = curSrv;
        b.wrap = curWrap;
        batches.push_back(b);
        curBatch = batches.size() - 1;
    }

    void emitQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, uint32_t color) {
        if (affineOn) {   // 仿射：四角过矩阵（文字/图片跟着卡片转）
            float p[8], uv[8];
            affCorners(x0, y0, x1, y1, p);
            uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0;
            uv[4] = u0; uv[5] = v1; uv[6] = u1; uv[7] = v1;
            emitQuadUV(p, uv, color);
            return;
        }
        D3DVertex* o = vertRoom(6);
        if (!o) return; // 缓冲上限，丢弃多余
        beginBatch();
        // 0xAARRGGBB → 0xAABBGGRR：R8G8B8A8_UNORM 按内存字节序读（byte0=R）
        const uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        o[0] = D3DVertex{ x0, y0, sw, u0, v0 };
        o[1] = D3DVertex{ x1, y0, sw, u1, v0 };
        o[2] = D3DVertex{ x0, y1, sw, u0, v1 };
        o[3] = o[2];
        o[4] = o[1];
        o[5] = D3DVertex{ x1, y1, sw, u1, v1 };
        vcount += 6;
        batches[curBatch].count++;
    }

    // 任意四边形 + 每角 UV（旋转/斜切用），角顺序 0=左上 1=右上 2=左下 3=右下
    void emitQuadUV(const float* p, const float* uv, uint32_t color) {
        D3DVertex* o = vertRoom(6);
        if (!o) return;
        beginBatch();
        const uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        o[0] = D3DVertex{ p[0], p[1], sw, uv[0], uv[1] };
        o[1] = D3DVertex{ p[2], p[3], sw, uv[2], uv[3] };
        o[2] = D3DVertex{ p[4], p[5], sw, uv[4], uv[5] };
        o[3] = o[2];
        o[4] = o[1];
        o[5] = D3DVertex{ p[6], p[7], sw, uv[6], uv[7] };
        vcount += 6;
        batches[curBatch].count++;
    }

    void emitQuadRot(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3, uint32_t color) {
        if (affineOn) {
            float p[8];
            affPoint(x0, y0, p[0], p[1]);
            affPoint(x1, y1, p[2], p[3]);
            affPoint(x2, y2, p[4], p[5]);
            affPoint(x3, y3, p[6], p[7]);
            const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            emitQuadUV(p, uv, color);
            return;
        }
        D3DVertex* o = vertRoom(6);
        if (!o) return;
        beginBatch();
        const uint32_t sw = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu);
        o[0] = D3DVertex{ x0, y0, sw, 0, 0 };
        o[1] = D3DVertex{ x1, y1, sw, 1, 0 };
        o[2] = D3DVertex{ x2, y2, sw, 1, 1 };
        o[3] = o[2];
        o[4] = D3DVertex{ x3, y3, sw, 0, 1 };
        o[5] = o[0];
        vcount += 6;
        batches[curBatch].count++;
    }

    // ---- 图层（v0.16.10）：组透明度的离屏合成 ----
    // 和 GL 同一个设计：池化**窗口大小**的 RGBA 目标（这里是 texture + RTV + SRV 三件套），
    // 坐标不用换算、UV 直接是像素坐标 ÷ 窗口尺寸。层内是单采样的：内容全是不带 MSAA 的
    // quad，圆角/描边本来就是解析式几何，少这层 MSAA 看不出差别。
    struct LayerTarget {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        bool inUse = false;
    };
    struct LayerFrame {
        int idx = -1;
        float x = 0, y = 0, w = 0, h = 0, alpha = 1;
        ID3D11RenderTargetView* prevTarget = nullptr;
    };
    std::vector<LayerTarget> layers;
    std::vector<LayerFrame> layerStack;
    static constexpr int kMaxLayers = 4;

    // ---- v29 毛玻璃：降采样链（每级 1/2，按需分配）+ 主目标 MSAA 解析中转 ----
    struct BlurLevel {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
    };
    std::vector<BlurLevel> blurLv;
    ID3D11Texture2D* capTex = nullptr;   // 窗口大小的单采样中转（MSAA 目标先解析到这里）
    // 降采样趟把图元画进小纹理时，坐标是"小图局部"的，不能再按层框裁
    bool ignoreLayerClip = false;

    void releaseBlurLevel(BlurLevel& b) {
        if (b.srv) { b.srv->Release(); b.srv = nullptr; }
        if (b.rtv) { b.rtv->Release(); b.rtv = nullptr; }
        if (b.tex) { b.tex->Release(); b.tex = nullptr; }
        b.w = b.h = 0;
    }

    bool ensureBlurLevel(size_t i, int w, int h) {
        if (i >= 8 || w <= 0 || h <= 0) return false;
        if (blurLv.size() <= i) blurLv.resize(i + 1);
        BlurLevel& b = blurLv[i];
        if (b.tex && b.w == w && b.h == h) return true;
        releaseBlurLevel(b);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &b.tex))) { b.tex = nullptr; return false; }
        if (FAILED(device->CreateRenderTargetView(b.tex, nullptr, &b.rtv)) ||
            FAILED(device->CreateShaderResourceView(b.tex, nullptr, &b.srv))) {
            releaseBlurLevel(b);
            return false;
        }
        b.w = w;
        b.h = h;
        return true;
    }

    bool ensureCapTex(int w, int h) {
        if (capTex) {
            D3D11_TEXTURE2D_DESC od = {};
            capTex->GetDesc(&od);
            if (static_cast<int>(od.Width) == w && static_cast<int>(od.Height) == h) return true;
            capTex->Release();
            capTex = nullptr;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &capTex));
    }

    static void warnLayerLimit() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr, "[d3d11] 图层超过 %d 层，多出来的退回逐元素乘 alpha（减少嵌套即可）\n",
                     kMaxLayers);
    }

    int acquireLayer() {
        for (size_t i = 0; i < layers.size(); i++)
            if (!layers[i].inUse) return static_cast<int>(i);
        if (static_cast<int>(layers.size()) >= kMaxLayers) return -1;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(winW);
        td.Height = static_cast<UINT>(winH);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        LayerTarget t;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &t.tex))) return -1;
        if (FAILED(device->CreateRenderTargetView(t.tex, nullptr, &t.rtv)) ||
            FAILED(device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) {
            if (t.srv) t.srv->Release();
            if (t.rtv) t.rtv->Release();
            t.tex->Release();
            return -1;
        }
        layers.push_back(t);
        return static_cast<int>(layers.size() - 1);
    }

    bool pushLayer(float x, float y, float w, float h, float alpha) {
        if (!ready || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return false;   // v27：alpha=1 也允许（裁剪用容器层）
        if (static_cast<int>(layerStack.size()) >= kMaxLayers) { warnLayerLimit(); return false; }
        flushAll();                     // 先把手头的图元画到当前目标（此时还没换 RT）
        const int idx = acquireLayer();
        if (idx < 0) { warnLayerLimit(); return false; }
        LayerFrame f;
        f.idx = idx; f.x = x; f.y = y; f.w = w; f.h = h; f.alpha = alpha;
        // 父目标 = 外层图层的 RT（嵌套时不能一律回到主目标，否则内层会直接合成到主目标上，
        // 外层那一次 alpha 就漏掉了 —— v0.16.12 用 6 层嵌套用例抓到，差 25% 像素）
        f.prevTarget = layerStack.empty() ? msTarget : layers[layerStack.back().idx].rtv;
        LayerTarget& t = layers[idx];
        t.inUse = true;
        ID3D11RenderTargetView* rt = t.rtv;
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->ClearRenderTargetView(t.rtv, zero);
        layerStack.push_back(f);
        return true;
    }

    // v0.16.28：颗粒噪声（64×64、确定性 LCG、预乘白）。三端同一种子 → 同一张图。
    bool ensureNoise() {
        if (noiseSrv) return true;
        const int N = 64;
        std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
        uint32_t st = 0x9E3779B9u;
        for (int i = 0; i < N * N; i++) {
            st = st * 1664525u + 1013904223u;
            const uint8_t a = static_cast<uint8_t>((st >> 16) & 0xFFu);
            const size_t o = static_cast<size_t>(i) * 4;
            px[o + 0] = a; px[o + 1] = a; px[o + 2] = a; px[o + 3] = a;   // BGRA：预乘白
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(N);
        td.Height = static_cast<UINT>(N);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = px.data();
        init.SysMemPitch = static_cast<UINT>(N) * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &init, &tex)) || !tex) return false;
        const HRESULT hr = device->CreateShaderResourceView(tex, nullptr, &noiseSrv);
        tex->Release();
        if (FAILED(hr)) { noiseSrv = nullptr; return false; }
        return true;
    }

    void popLayer() {
        if (layerStack.empty()) return;
        flushAll();                     // 层内容落盘（flushAll 会按层框裁剪）
        const LayerFrame f = layerStack.back();
        layerStack.pop_back();
        LayerTarget& t = layers[f.idx];
        ID3D11RenderTargetView* rt = f.prevTarget;
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        // 合成：层里是**预乘**颜色（混合 ONE/INV_SRC_ALPHA），顶点色也必须预乘 ——
        // 着色器算 (t.rgb * vcol.rgb, t.a * vcol.a)，不预乘的话通道会被整块加进去。
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        uint32_t col = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        Premult(col);
        // D3D 的纹理原点在左上，和屏幕坐标一致 —— 这里**不用**像 GL 那样翻转 V。
        curSrv = t.srv;
        curLinear = 0;
        scissor[0] = static_cast<int>(f.x);
        scissor[1] = static_cast<int>(f.y);
        scissor[2] = static_cast<int>(f.x + f.w);
        scissor[3] = static_cast<int>(f.y + f.h);
        emitQuad(f.x, f.y, f.x + f.w, f.y + f.h,
                 f.x / static_cast<float>(winW), f.y / static_cast<float>(winH),
                 (f.x + f.w) / static_cast<float>(winW), (f.y + f.h) / static_cast<float>(winH),
                 col);
        curSrv = whiteSrv;
        scissor[0] = 0; scissor[1] = 0; scissor[2] = winW; scissor[3] = winH;
        flushAll();
        t.inUse = false;
    }

    void flushAll() {
        if (vcount == 0 || !vb) return;
        if (!ensureVbCapacity(vcount)) return;   // 本帧顶点比 VB 大：先扩容（不会每帧发生）
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (FAILED(ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        memcpy(map.pData, verts.data(), vcount * sizeof(D3DVertex));
        ctx->Unmap(vb, 0);

        // 正在画进图层时，每条批次再和层框求交（层外的内容不该出现在层里）
        const LayerFrame* lf = layerStack.empty() ? nullptr : &layerStack.back();
        for (const auto& b : batches) {
            if (b.count == 0) continue;
            D3D11_RECT r = { b.sx, b.sy, b.sx + b.sw, b.sy + b.sh };
            if (lf && !ignoreLayerClip) {
                const LONG ll = static_cast<LONG>(std::floor(lf->x));
                const LONG lt = static_cast<LONG>(std::floor(lf->y));
                const LONG lr = static_cast<LONG>(std::ceil(lf->x + lf->w));
                const LONG lb = static_cast<LONG>(std::ceil(lf->y + lf->h));
                r.left = std::max(r.left, ll); r.top = std::max(r.top, lt);
                r.right = std::min(r.right, lr); r.bottom = std::min(r.bottom, lb);
            }
            if (r.right <= r.left || r.bottom <= r.top) continue;
            ctx->RSSetScissorRects(1, &r);
            ctx->PSSetShaderResources(0, 1, &b.srv);
            ctx->PSSetSamplers(0, 1, b.wrap ? &samPointWrap
                                             : (b.linear ? &samLinear : &samPoint));
            ctx->Draw(b.count * 6, b.first);
        }
        // 本帧绘制完毕，淘汰的模板现在才 Release（见 roundFor 里的淘汰注释）
        for (auto& rt : pendingRoundRelease) {
            if (rt.tex) rt.tex->Release();
            if (rt.srv) rt.srv->Release();
        }
        pendingRoundRelease.clear();
        // 图层（v0.16.10）：flush 之后必须清零，否则 present 会把合成四边形再画一遍
        // （GL 那边踩过：表现为组透明度彻底失效、画面全不透明）。
        vcount = 0;
        batches.clear();
        curBatch = static_cast<size_t>(-1);
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
        resolveOffscreen();
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
        // 字形图集先释放（纹理由 GlyphAtlas 通过 IGlyphTexHost 回调销毁）
        atlas.clear();
        atlas.setHost(nullptr);
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
        if (msTarget && msTarget != offRtv) { msTarget->Release(); msTarget = nullptr; }
        else if (msTarget) { msTarget->Release(); msTarget = nullptr; }
        if (msRtv) { msRtv->Release(); msRtv = nullptr; }
        if (msTex) { msTex->Release(); msTex = nullptr; }
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
        if (blendReplace) { blendReplace->Release(); blendReplace = nullptr; }
    if (samPointWrap) { samPointWrap->Release(); samPointWrap = nullptr; }
    if (noiseSrv) { noiseSrv->Release(); noiseSrv = nullptr; }
    for (BlurLevel& bl : blurLv) releaseBlurLevel(bl);
    blurLv.clear();
    if (capTex) { capTex->Release(); capTex = nullptr; }
    if (cb) { cb->Release(); cb = nullptr; }
        if (vb) { vb->Release(); vb = nullptr; }
        if (swap) { swap->Release(); swap = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        ready = false;
    }
};

// ============================================================
// 字形图集的纹理宿主（IGlyphTexHost）：把 GlyphAtlas 的位图搬进 D3D11 纹理
// ============================================================
// 句柄直接用 **SRV 指针**（绘制时正好要它）；纹理本身通过 glyphTexOf 反查更新/销毁。
// 与 GL 后端"句柄装 GLuint"同一个思路，只是这里要两个对象。
void* BinUID3D::Impl::GlyphTexHost::createTex(int w, int h) {
    if (!owner || !owner->device || w <= 0 || h <= 0) return nullptr;
    // 必须给零缓冲：图集里的"空格子"（空格/控符）会被当真字形画，未初始化的显存是噪点
    std::vector<uint8_t> zeros(static_cast<size_t>(w) * h * 4, 0);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { zeros.data(), static_cast<UINT>(w) * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(owner->device->CreateTexture2D(&td, &init, &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(owner->device->CreateShaderResourceView(tex, nullptr, &srv))) {
        tex->Release();
        return nullptr;
    }
    owner->glyphTexOf[srv] = tex;
    return srv;
}

void BinUID3D::Impl::GlyphTexHost::updateTex(void* handle, int x, int y, int w, int h,
                                             const uint8_t* bgra) {
    if (!owner || !owner->ctx || !handle || !bgra) return;
    auto it = owner->glyphTexOf.find(handle);
    if (it == owner->glyphTexOf.end() || !it->second) return;
    D3D11_BOX box = {};
    box.left = static_cast<UINT>(x);
    box.top = static_cast<UINT>(y);
    box.front = 0;
    box.right = static_cast<UINT>(x + w);
    box.bottom = static_cast<UINT>(y + h);
    box.back = 1;
    owner->ctx->UpdateSubresource(it->second, 0, &box, bgra, static_cast<UINT>(w) * 4, 0);
}

void BinUID3D::Impl::GlyphTexHost::destroyTex(void* handle) {
    if (!owner || !handle) return;
    auto it = owner->glyphTexOf.find(handle);
    if (it != owner->glyphTexOf.end()) {
        if (it->second) it->second->Release();
        owner->glyphTexOf.erase(it);
    }
    static_cast<ID3D11ShaderResourceView*>(handle)->Release();
}

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
    impl_->glyphHost.owner = impl_;
    impl_->atlas.setHost(&impl_->glyphHost);
    impl_->atlas.initFonts();
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
    // 只复位计数：缓冲已预分配到 kMaxVerts，clear() 每帧会把 2 MB 析构再扩容一遍
    impl_->vcount = 0;
    impl_->batches.clear();
    impl_->curBatch = static_cast<size_t>(-1);
    impl_->scissorDepth = 0;
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
    impl_->scissor[0] = 0;
    impl_->scissor[1] = 0;
    impl_->scissor[2] = impl_->winW;
    impl_->scissor[3] = impl_->winH;

    // 绘制目标 = 持久离屏帧缓冲
    impl_->ctx->OMSetRenderTargets(1, &impl_->msTarget, nullptr);

    // 不在此整帧 ClearRenderTargetView：该 API 不受 scissor 限制，会清空整个离屏，
    // 局部重绘时把脏区以外的持久像素抹白/抹透明（悬停后其余元素消失的根因）。
    // 脏区底色由 drawNodesD3D 的根节点背景填充（按 clipRect 范围）提供。
}

// Present(vsync ? 1 : 0) 走 flip-model 交换链时 SyncInterval=1 会阻塞到垂直同步；
// 读回 + GDI 贴屏那几条路径（透明窗 / 强制读回 / 铺满显示器 / 远程会话）不阻塞。
bool BinUID3D::presentPacesFrames() const {
    return impl_->ready && impl_->vsync && !impl_->transparent && !impl_->forceGdi &&
           !impl_->coversMonitor && !RemoteSession();
}

void BinUID3D::present() {
    if (!impl_->ready) return;
    impl_->flushAll(); // 画到离屏帧缓冲
    static const bool trace = std::getenv("BINUI_D3D_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "[d3d] present: verts=%zu batches=%zu transparent=%d forceGdi=%d covers=%d\n",
                     impl_->vcount, impl_->batches.size(), (int)impl_->transparent,
                     (int)impl_->forceGdi, (int)impl_->coversMonitor);

    // 读回呈现的三条触发条件与 GL/Vulkan 后端同一套理由：
    // 透明窗口必须自己合成（交换链拿不到预乘 alpha）；客户区铺满整块显示器时 DWM 会把
    // 呈现提升成直翻转（远程桌面/虚拟显示器上实测画面会停住）；拖拽 resize 期由上层强制。
    if (impl_->transparent || impl_->forceGdi || impl_->coversMonitor || RemoteSession()) {
        impl_->presentReadback();
        return;
    }

    // 整帧 blit：MSAA 解析到 offTex，再贴到交换链 backbuffer
    impl_->resolveOffscreen();
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

// v30：四角带色四边形（任意角度渐变）。顶点写法与 emitQuadUV 一致，只是每个顶点
// 各自一个颜色 —— 顶点色是逐顶点属性，着色器不动。
bool BinUID3D::fillGradientQuad(float x, float y, float w, float h, float radius,
                                float angleDeg, uint32_t from, uint32_t to) {
    if (!impl_->ready || w <= 0.0f || h <= 0.0f) return false;
    const float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;
    ID3D11ShaderResourceView* srv = impl_->whiteSrv;
    int tpl = 0;
    if (r > 0.5f) {
        const int rr = std::max(1, static_cast<int>(std::lround(r)));
        RoundTex& rt = impl_->roundFor(rr, false);
        if (!rt.srv) return false;              // 没模板：交回条带路径（像素不变）
        srv = rt.srv;
        tpl = rt.t;
    }
    binui::BinUIGradQuad q[9];
    const int n = binui::BuildGradientQuads(x, y, w, h, r, tpl, angleDeg, from, to, q, 9);
    if (n <= 0) return false;
    impl_->curLinear = (r > 0.5f) ? 1 : 0;
    impl_->curSrv = srv;
    auto sw = [](uint32_t c) {
        return (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    };
    for (int i = 0; i < n; i++) {
        D3DVertex* o = impl_->vertRoom(6);
        if (!o) return true;                    // 撞顶点硬上限：已告警，剩下的不发
        impl_->beginBatch();
        const uint32_t c0 = sw(q[i].c[0]), c1 = sw(q[i].c[1]);
        const uint32_t c2 = sw(q[i].c[2]), c3 = sw(q[i].c[3]);
        const float x0 = q[i].x, y0 = q[i].y, x1 = q[i].x + q[i].w, y1 = q[i].y + q[i].h;
        o[0] = D3DVertex{ x0, y0, c0, q[i].u0, q[i].v0 };
        o[1] = D3DVertex{ x1, y0, c1, q[i].u1, q[i].v0 };
        o[2] = D3DVertex{ x0, y1, c2, q[i].u0, q[i].v1 };
        o[3] = o[2];
        o[4] = o[1];
        o[5] = D3DVertex{ x1, y1, c3, q[i].u1, q[i].v1 };
        impl_->vcount += 6;
        impl_->batches[impl_->curBatch].count++;
    }
    return true;
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
    // v0.11.8：几何半径用浮点（缩放动画里半径要跟着连续变），角片模板仍取最近的整数。
    // 圆/胶囊在缩放时如果半径取整，2r 与 w 会周期性错开 1px，中间几块在 0/1/2px 之间跳。
    float c = std::min(radius, std::min(w, h) * 0.5f);
    if (c <= 0.0f) { fillRect(x, y, w, h, color); return; }

    const int r = std::max(1, static_cast<int>(std::lround(c)));
    RoundTex& rt = impl_->roundFor(r, false);
    if (!rt.srv) { fillRect(x, y, w, h, color); return; }

    float t = static_cast<float>(rt.t);
    float cu = c / t;
    float strip0 = 0.5f;
    float strip1 = 0.5f + 1.0f / t;

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

// 仿射圆角矩形（v20）：局部九宫格每片按 xf 变换后发射。几何与 fillRounded/strokeRounded
// 同源（同一套模板 + 同样的九格划分），所以"不旋转时"逐像素一致。
void BinUID3D::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                             uint32_t color, const float xf[6]) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    const int layers = stroke ? std::max(1, static_cast<int>(std::lround(std::max(1.0f, thickness))))
                              : 1;
    auto xform = [&](float lx, float ly, float* out) {
        out[0] = xf[0] * lx + xf[1] * ly + xf[2];
        out[1] = xf[3] * lx + xf[4] * ly + xf[5];
    };
    Premult(color);
    impl_->curLinear = 1;
    int emitted = 0;
    for (int k = 0; k < layers; k++) {
        const float lx = static_cast<float>(k), ly = static_cast<float>(k);
        const float lw = w - 2.0f * k, lh = h - 2.0f * k;
        if (lw <= 1.0f || lh <= 1.0f) break;
        const float rk = rf - static_cast<float>(k);
        if (rk <= 0.0f) break;
        const int rri = std::max(1, static_cast<int>(std::lround(rk)));
        RoundTex& rt = impl_->roundFor(rri, stroke);
        if (!rt.srv) break;
        const float t = static_cast<float>(rt.t);
        const float cu = rk / t;
        const float strip0 = 0.5f;
        const float strip1 = 0.5f + 1.0f / t;
        const float c = rk;
        impl_->curSrv = rt.srv;
        auto quad = [&](float lqx, float lqy, float lqw, float lqh,
                        float u0, float v0, float u1, float v1) {
            if (lqw <= 0.0f || lqh <= 0.0f) return;
            const float corners[8] = { lqx, lqy, lqx + lqw, lqy,
                                       lqx, lqy + lqh, lqx + lqw, lqy + lqh };
            const float uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
            float sp[8];
            for (int v = 0; v < 4; v++) xform(corners[v * 2], corners[v * 2 + 1], sp + v * 2);
            impl_->emitQuadUV(sp, uv, color);
            emitted++;
        };
        quad(lx, ly, c, c, 0, 0, cu, cu);
        quad(lx + lw - c, ly, c, c, 1 - cu, 0, 1, cu);
        quad(lx, ly + lh - c, c, c, 0, 1 - cu, cu, 1);
        quad(lx + lw - c, ly + lh - c, c, c, 1 - cu, 1 - cu, 1, 1);
        quad(lx + c, ly, lw - 2 * c, c, strip0, 0, strip1, cu);
        quad(lx + c, ly + lh - c, lw - 2 * c, c, strip0, 1 - cu, strip1, 1);
        quad(lx, ly + c, c, lh - 2 * c, 0, strip0, cu, strip1);
        quad(lx + lw - c, ly + c, c, lh - 2 * c, 1 - cu, strip0, 1, strip1);
        quad(lx + c, ly + c, lw - 2 * c, lh - 2 * c, strip0, strip0, strip1, strip1);
    }
    if (emitted == 0) {
        const float corners[8] = { 0, 0, w, 0, 0, h, w, h };
        const float uv[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        float sp[8];
        for (int v = 0; v < 4; v++) xform(corners[v * 2], corners[v * 2 + 1], sp + v * 2);
        impl_->emitQuadUV(sp, uv, color);
    }
    impl_->curLinear = 0;
    impl_->curSrv = impl_->whiteSrv;
}

void BinUID3D::strokeRounded(float x, float y, float w, float h, float radius,
                             float thickness, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    // v0.11.8：几何半径用浮点（缩放动画要连续），模板按最近整数取
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) { fillRect(x, y, w, h, color); return; }

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
        const float cr = std::min(rf - static_cast<float>(k), std::min(ew, eh) * 0.5f);
        if (cr <= 0.0f) break;
        const int rr = std::max(1, static_cast<int>(std::lround(cr)));
        RoundTex& rt = impl_->roundFor(rr, true);
        if (!rt.srv) break;

        const float t = static_cast<float>(rt.t);
        const float cu = cr / t;
        const float strip0 = 0.5f;
        const float strip1 = 0.5f + 1.0f / t;
        const float c = cr;
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
    return impl_->atlas.textWidth(size, weight, utf8, italic);
}

void BinUID3D::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    impl_->atlas.fontMetrics(size, weight, ascent, lineHeight, italic);
}

// 文字亚像素定位：D3D11 的采样器是**按批次**选的（见 flush 里的 samLinear/samPoint），
// 所以这里同时记一个标志，drawText 里把它写进 curLinear。
void BinUID3D::setTextSubpixel(bool on) {
    if (!impl_) return;
    impl_->atlas.snapText = !on;
    impl_->subpixelText = on ? 1 : 0;
}

// v24 文字阴影：状态交给共享图集，三端像素一致
// v0.16.18 仿射状态：见 GL 后端同名函数（三端语义一致）
// v26 图片滤镜：矩阵只影响"下一次上传"的像素，并按哈希进纹理缓存键。
void BinUID3D::setImageFilter(const BinUIImageFilter* f) {
    if (!impl_) return;
    impl_->imgFilterKey = binui::ImageFilterKey(f);
    if (!impl_->imgFilterKey) return;
    for (int i = 0; i < 9; i++) impl_->imgFilter.m[i] = f->m[i];
    for (int i = 0; i < 3; i++) impl_->imgFilter.o[i] = f->offset[i];
}

// v27 裁剪擦除
bool BinUID3D::setEraseBlend(bool on) {
    if (!impl_ || !impl_->ready) return false;
    impl_->flushAll();
    ID3D11BlendState* want = on ? impl_->blendErase : impl_->blend;
    if (!want) return false;
    float bf[4] = { 0, 0, 0, 0 };
    impl_->ctx->OMSetBlendState(want, bf, 0xFFFFFFFF);
    return true;
}

// v28 遮罩保留趟：dst *= srcA
bool BinUID3D::setKeepBlend(bool on) {
    if (!impl_ || !impl_->ready) return false;
    impl_->flushAll();
    ID3D11BlendState* want = on ? impl_->blendKeep : impl_->blend;
    if (!want) return false;
    float bf[4] = { 0, 0, 0, 0 };
    impl_->ctx->OMSetBlendState(want, bf, 0xFFFFFFFF);
    return true;
}

// v0.16.27 毛玻璃（backdrop blur）：抓当前目标的区域 → 多级 2× 降采样 → 圆角合成。
// 算法与 GL 参考实现逐字一致（同一套 UV 公式），这是三端像素一致的前提。
// D3D11 的纹理行 0 = 区域顶部（与屏幕同向），所以四边形 UV 正着来，**不做 V 翻转**。
bool BinUID3D::backdropBlur(float x, float y, float w, float h, float radius,
                            float blurPx, float grain) {
    if (!impl_ || !impl_->ready) return false;
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

    int levels = 1;
    {
        float v = 2.0f;
        while (levels < 6 && v < blurPx) { v *= 2.0f; levels++; }
    }

    d.flushAll();   // 手上攒的图元先落到当前目标（它们构成"背景"）

    // 当前目标（主目标 MSAA / 离屏层）
    ID3D11RenderTargetView* curRtv = d.layerStack.empty()
        ? d.msTarget : d.layers[d.layerStack.back().idx].rtv;
    ID3D11Texture2D* curTex = d.layerStack.empty()
        ? (d.msSamples > 1 ? d.msTex : d.offTex)
        : d.layers[d.layerStack.back().idx].tex;

    D3D11_VIEWPORT prevVp = { 0, 0, static_cast<float>(d.winW),
                              static_cast<float>(d.winH), 0, 1 };
    { UINT n = 1; d.ctx->RSGetViewports(&n, &prevVp); }

    if (!d.ensureBlurLevel(0, rw, rh)) return false;

    // 1) 抓背景：MSAA 主目标先解析到单采样中转，再按区域 Copy
    ID3D11Texture2D* srcTex = curTex;
    ID3D11RenderTargetView* nullRt[1] = { nullptr };
    d.ctx->OMSetRenderTargets(1, nullRt, nullptr);          // 读之前先解绑
    if (curTex == d.msTex && d.msSamples > 1) {
        if (!d.ensureCapTex(d.winW, d.winH)) {
            ID3D11RenderTargetView* back = curRtv;
            d.ctx->OMSetRenderTargets(1, &back, nullptr);
            return false;
        }
        d.ctx->ResolveSubresource(d.capTex, 0, d.msTex, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        srcTex = d.capTex;
    }
    {
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        d.ctx->PSSetShaderResources(0, 1, nullSrv);         // 别让 SRV 绑着被拷
        D3D11_BOX box = { static_cast<UINT>(rx), static_cast<UINT>(ry), 0,
                          static_cast<UINT>(rx1), static_cast<UINT>(ry1), 1 };
        d.ctx->CopySubresourceRegion(d.blurLv[0].tex, 0, 0, 0, 0, srcTex, 0, &box);
    }
    { ID3D11RenderTargetView* back = curRtv;
      d.ctx->OMSetRenderTargets(1, &back, nullptr); }

    // 2) 降采样链：每级把上一级缩一半（覆盖混合；目标先清成透明）
    const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    d.ctx->OMSetBlendState(d.blendReplace, bf, 0xFFFFFFFF);
    bool chainOk = true;
    for (int i = 1; i <= levels && chainOk; i++) {
        const int dw = std::max(1, d.blurLv[static_cast<size_t>(i - 1)].w / 2);
        const int dh = std::max(1, d.blurLv[static_cast<size_t>(i - 1)].h / 2);
        if (!d.ensureBlurLevel(static_cast<size_t>(i), dw, dh)) { chainOk = false; break; }
        Impl::BlurLevel& L = d.blurLv[static_cast<size_t>(i)];
        ID3D11RenderTargetView* rt = L.rtv;
        d.ctx->OMSetRenderTargets(1, &rt, nullptr);
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        d.ctx->ClearRenderTargetView(rt, zero);
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(dw),
                              static_cast<float>(dh), 0.0f, 1.0f };
        d.ctx->RSSetViewports(1, &vp);
        float vs[2] = { static_cast<float>(dw), static_cast<float>(dh) };
        d.ctx->UpdateSubresource(d.cb, 0, nullptr, vs, 0, 0);
        d.ignoreLayerClip = true;
        // ★ 关键：batches 的矩形取自 scissor（节点裁剪盒），小目标上会被整块裁掉
        const int savedSc[4] = { d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3] };
        d.scissor[0] = 0; d.scissor[1] = 0; d.scissor[2] = dw; d.scissor[3] = dh;
        d.curSrv = d.blurLv[static_cast<size_t>(i - 1)].srv;
        d.curLinear = 1;
        d.emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                   0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
        d.flushAll();
        d.ignoreLayerClip = false;
        d.scissor[0] = savedSc[0]; d.scissor[1] = savedSc[1];
        d.scissor[2] = savedSc[2]; d.scissor[3] = savedSc[3];
    }

    // 3) 合成：圆角条带 + 双线性放大，直接覆盖（背景被替换成模糊版）
    if (chainOk) {
        ID3D11RenderTargetView* back = curRtv;
        d.ctx->OMSetRenderTargets(1, &back, nullptr);
        d.ctx->RSSetViewports(1, &prevVp);
        float vs[2] = { static_cast<float>(d.winW), static_cast<float>(d.winH) };
        d.ctx->UpdateSubresource(d.cb, 0, nullptr, vs, 0, 0);
        d.ctx->OMSetBlendState(d.blendReplace, bf, 0xFFFFFFFF);
        d.curSrv = d.blurLv[static_cast<size_t>(levels)].srv;
        d.curLinear = 1;
        binui::PaintImageBand bands[binui::kMaxImageBands];
        const int nb = binui::BuildImageBands(x, y, w, h, radius, bands);
        for (int q = 0; q < nb; q++) {
            const binui::PaintImageBand& b = bands[q];
            if (b.w <= 0.0f || b.h <= 0.0f) continue;
            d.emitQuad(b.x, b.y, b.x + b.w, b.y + b.h,
                       b.u0, b.v0, b.u1, b.v1, 0xFFFFFFFFu);
        }
        d.flushAll();
    }

    // 4) 颗粒（grain）：极淡的确定性噪声，压 banding。一个纹素 = 一个屏幕像素，
    //    REPEAT + POINT，屏幕锚定（元素移动时颗粒不滑）。
    d.ctx->OMSetBlendState(d.blend, bf, 0xFFFFFFFF);   // 颗粒要"加"，不是覆盖
    if (chainOk && grain > 0.001f && d.ensureNoise()) {
        d.curSrv = d.noiseSrv;
        d.curLinear = 0;
        d.curWrap = 1;
        const uint32_t av = static_cast<uint32_t>(grain * 3.0f + 0.5f);
        const uint32_t ncol = (av << 24) | 0x00FFFFFFu;
        binui::PaintImageBand nb2[binui::kMaxImageBands];
        const int nnb = binui::BuildImageBands(x, y, w, h, radius, nb2);
        const float k = 1.0f / 64.0f;
        for (int q = 0; q < nnb; q++) {
            const binui::PaintImageBand& b = nb2[q];
            if (b.w <= 0.0f || b.h <= 0.0f) continue;
            // 不加半个纹素：像素中心（+0.5）正好落在纹素中心，见 GL 那边的说明
            d.emitQuad(b.x, b.y, b.x + b.w, b.y + b.h,
                       b.x * k, b.y * k,
                       (b.x + b.w) * k, (b.y + b.h) * k, ncol);
        }
        d.flushAll();
        d.curWrap = 0;
    }

    // 恢复：纹理回到白色（fillRect 用）
    d.curSrv = d.whiteSrv;
    d.curLinear = 0;
    d.curWrap = 0;
    return chainOk;
}

bool BinUID3D::setAffine(const float* xf) {
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

void BinUID3D::setTextShadow(float dx, float dy, float blur, uint32_t color) {
    if (impl_) impl_->atlas.setTextShadow(dx, dy, blur, color);
}

bool BinUID3D::pushLayer(float x, float y, float w, float h, float alpha) {
    return impl_ ? impl_->pushLayer(x, y, w, h, alpha) : false;
}

void BinUID3D::popLayer() {
    if (impl_) impl_->popLayer();
}

// 文本排版/光栅化/图集全在 binui::GlyphAtlas 里（与 GL / Vulkan 后端同一份代码），
// 这里只把回调给出的字形四边形丢进本后端的顶点批次 —— 三端文本因此是同一份像素。
void BinUID3D::drawText(float x, float y, float size, int weight,
                        const std::string& utf8, uint32_t color,
                        bool centerH, bool centerV, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    impl_->curLinear = impl_->subpixelText;
    impl_->curSrv = impl_->whiteSrv;
    impl_->atlas.drawText(x, y, size, weight, utf8, color, centerH, centerV, italic,
                          [this](const GlyphQuad& q) {
                              impl_->curSrv = static_cast<ID3D11ShaderResourceView*>(q.tex);
                              impl_->emitQuad(q.x, q.y, q.x + q.w, q.y + q.h,
                                              q.u0, q.v0, q.u1, q.v1, q.color);
                          });
    impl_->curSrv = impl_->whiteSrv;
}

void BinUID3D::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                               const std::string& utf8, uint32_t color, int align,
                               bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    impl_->curLinear = impl_->subpixelText;
    impl_->curSrv = impl_->whiteSrv;
    impl_->atlas.drawTextWrapped(x, y, maxW, size, weight, utf8, color, align, italic,
                                 [this](const GlyphQuad& q) {
                                     impl_->curSrv = static_cast<ID3D11ShaderResourceView*>(q.tex);
                                     impl_->emitQuad(q.x, q.y, q.x + q.w, q.y + q.h,
                                                     q.u0, q.v0, q.u1, q.v1, q.color);
                                 });
    impl_->curSrv = impl_->whiteSrv;
}
bool BinUID3D::prepareImage(uint16_t index, const uint8_t* data, size_t size, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImage(index, data, size, generation);
}

bool BinUID3D::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba, uint32_t generation) {
    if (!impl_->ready) return false;
    return impl_->uploadImageRaw(index, w, h, rgba, generation);
}

// v25 九宫格（切分数学见 binui_gfx.h 的 BuildNineSlice，三端共用）
void BinUID3D::drawImageNine(uint16_t index, float x, float y, float w, float h,
                            float slice, uint32_t tint) {
    if (!impl_) return;
    auto it = impl_->imageDims.find(impl_->imgKey(index));
    float q[9][8];
    const int n = it == impl_->imageDims.end()
        ? 0 : BuildNineSlice(x, y, w, h, it->second.first, it->second.second, slice, q);
    if (n <= 0) { drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, tint); return; }
    for (int i = 0; i < n; i++)
        drawImageEx(index, q[i][0], q[i][1], q[i][2], q[i][3],
                    q[i][4], q[i][5], q[i][6], q[i][7], tint);
}

void BinUID3D::drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                           float x, float y, float w, float h, uint32_t tint) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    auto it = impl_->images.find(impl_->imgKey(index));
    if (it == impl_->images.end() || !it->second) return;
    impl_->curLinear = 1;
    impl_->curSrv = it->second;
    impl_->emitQuad(x, y, x + w, y + h, u0, v0, u1, v1, tint);
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
    impl_->atlas.setFontName(name ? name : "");
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
