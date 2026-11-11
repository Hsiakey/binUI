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
    ID3D11ShaderResourceView* srv = nullptr;
};

struct RoundTex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int t = 0;         // 模板边长（RoundTemplateSize(r)）
    int corner = 0;    // 角区边长（2r+1）
    uint64_t lastUse = 0;   // LRU 时钟（缓存上限 48 张）
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
    // 4× MSAA 绘制目标（持久累积帧）+ 它的 RTV；msSamples==1 时 msTarget 指向 offRtv
    ID3D11Texture2D* msTex = nullptr;
    ID3D11RenderTargetView* msRtv = nullptr;
    ID3D11RenderTargetView* msTarget = nullptr;
    int msSamples = 1;

    // 场景
    std::vector<D3DVertex> verts;      // 预分配到 kMaxVerts 的本帧顶点缓冲
    size_t vcount = 0;                 // 本帧已写顶点数
    std::vector<Batch> batches;
    Batch* curBatch = nullptr;
    int scissor[4] = { 0, 0, 0, 0 };
    int scissorDepth = 0;
    uint8_t curLinear = 0;
    ID3D11ShaderResourceView* curSrv = nullptr;


    // 圆角模板（按 radius；负 key = 描边）
    std::unordered_map<int, RoundTex> rounds;
    uint64_t roundClock = 0;   // 圆角模板缓存 LRU 时钟

    // 图片纹理缓存（按 imageIndex）
    std::unordered_map<uint16_t, ID3D11ShaderResourceView*> images;
    std::unordered_map<uint16_t, uint32_t> imageGen; // 换图 generation：不一致时重新上传

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
            if (victim->second.tex) victim->second.tex->Release();
            if (victim->second.srv) victim->second.srv->Release();
            rounds.erase(victim);
        }
        return rounds.emplace(key, rt).first->second;
    }

    // roundFor 失败时的空返回（调用方据此退回 fillRect）
    RoundTex emptyRound_;

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
        if (verts.size() < kMaxVerts) verts.resize(kMaxVerts);
        if (vcount + n > verts.size()) return nullptr;
        return verts.data() + vcount;
    }

    void beginBatch() {
        if (curBatch &&
            curBatch->sx == scissor[0] && curBatch->sy == scissor[1] &&
            curBatch->sw == scissor[2] - scissor[0] && curBatch->sh == scissor[3] - scissor[1] &&
            curBatch->linear == curLinear && curBatch->srv == curSrv)
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
        batches.push_back(b);
        curBatch = &batches.back();
    }

    void emitQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, uint32_t color) {
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
        curBatch->count++;
    }

    void emitQuadRot(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3, uint32_t color) {
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
        curBatch->count++;
    }

    void flushAll() {
        if (vcount == 0 || !vb) return;
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (FAILED(ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        memcpy(map.pData, verts.data(), vcount * sizeof(D3DVertex));
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
    impl_->curBatch = nullptr;
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
    return impl_->atlas.textWidth(size, weight, utf8, italic);
}

void BinUID3D::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    impl_->atlas.fontMetrics(size, weight, ascent, lineHeight, italic);
}

// 文本排版/光栅化/图集全在 binui::GlyphAtlas 里（与 GL / Vulkan 后端同一份代码），
// 这里只把回调给出的字形四边形丢进本后端的顶点批次 —— 三端文本因此是同一份像素。
void BinUID3D::drawText(float x, float y, float size, int weight,
                        const std::string& utf8, uint32_t color,
                        bool centerH, bool centerV, bool italic) {
    if (!impl_->ready || utf8.empty()) return;
    impl_->curLinear = 0;
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
    impl_->curLinear = 0;
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
