// binui_auto.cpp - 运行期后端选择：问 binGPU「这台机器有什么」，再决定用谁上屏。
//
// 优先级（master 2026-11-20 定的）：**VK > GL > D3D11 > CPU(bin2d)**。
// CPU 只在 binGPU 一个图形后端都起不来时使用（没有 GPU / RDP / 驱动坏了）。
//
// 为什么"问 binGPU"而不是自己猜：显卡有没有、驱动能不能建上下文，只有设备层知道。
// binGPU 已经给了 `bingpu_backend_available()`，这里只是把它翻译成"选哪个 binUI 后端"。
#include "backends/binui_auto.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "backends/binui_bin2d.h"
#include "backends/binui_d3d11.h"
#include "backends/binui_gl.h"
#include "backends/binui_gpu.h"
#include "backends/binui_vk.h"
#include "bingpu.h"

namespace binui {

namespace {

// 节拍用的单调时钟（微秒）。外壳与节拍器必须共用同一个时间原点 —— 只在这个文件里取。
long long PacerNowUs() {
    using namespace std::chrono;
    static const auto epoch = steady_clock::now();
    return static_cast<long long>(duration_cast<microseconds>(steady_clock::now() - epoch).count());
}

// `BINUI_GPU=vk|gl|d3d11|2d` 强制指定；空/未知 = 自动。
GpuBackend FromEnv(const char* v) {
    if (!v || !*v) return GpuBackend::Auto;
    if (!std::strcmp(v, "vk") || !std::strcmp(v, "vulkan")) return GpuBackend::Vulkan;
    if (!std::strcmp(v, "gl") || !std::strcmp(v, "opengl")) return GpuBackend::GL;
    if (!std::strcmp(v, "d3d11") || !std::strcmp(v, "d3d")) return GpuBackend::D3D11;
    if (!std::strcmp(v, "2d") || !std::strcmp(v, "bin2d") || !std::strcmp(v, "cpu"))
        return GpuBackend::Cpu;
    // 0.36.0：设备层直接建在 binGPU 上（圆角 AA 是那边片元里的一句距离场）
    if (!std::strcmp(v, "gpu") || !std::strcmp(v, "bingpu")) return GpuBackend::BinGpu;
    std::fprintf(stderr, "[binui] BINUI_GPU=%s 不认识（用 gpu|vk|gl|d3d11|2d），按自动处理\n", v);
    return GpuBackend::Auto;
}

// 让 **binGPU** 决定用哪个 API、在哪执行（0.33.1：决策权归 binGPU）。
// binUI 这边只负责把计划翻译成"用哪份 binUI 后端实现"——这是过渡期的翻译层，
// 等 bin2d 的 GPU 执行模式做完（见 docs/backend_layering.md 的三步），这一层就删掉。
bool AskBinGpu(GpuBackend want, GpuBackend* out) {
    BgpuRenderPlan plan{};
    BgpuBackendKind k = BGPU_BACKEND_AUTO;
    switch (want) {
        case GpuBackend::Vulkan: k = BGPU_BACKEND_VULKAN; break;
        case GpuBackend::GL:     k = BGPU_BACKEND_GL; break;
        case GpuBackend::D3D11:  k = BGPU_BACKEND_D3D11; break;
        case GpuBackend::Cpu:    k = BGPU_BACKEND_CPU; break;
        default:                 k = BGPU_BACKEND_AUTO; break;
    }
    if (bingpu_render_plan(k, &plan) != BGPU_OK) return false;
    switch (plan.backend) {
        case BGPU_BACKEND_VULKAN: *out = GpuBackend::Vulkan; break;
        case BGPU_BACKEND_GL:     *out = GpuBackend::GL; break;
        case BGPU_BACKEND_D3D11:  *out = GpuBackend::D3D11; break;
        default:                  *out = GpuBackend::Cpu; break;
    }
    std::fprintf(stderr, "[binui] binGPU 计划: backend=%s exec=%s msaa=%u\n",
                 bingpu_backend_name(plan.backend), bingpu_exec_mode_name(plan.exec),
                 plan.msaa_samples);
    return true;
}

} // namespace

const char* GpuBackendName(GpuBackend b) {
    switch (b) {
        case GpuBackend::Vulkan: return "vulkan";
        case GpuBackend::GL:     return "gl";
        case GpuBackend::D3D11:  return "d3d11";
        case GpuBackend::Cpu:    return "bin2d";
        case GpuBackend::BinGpu: return "bingpu";
        default:                 return "auto";
    }
}

GpuBackend ResolveGpuBackend() {
    const GpuBackend want = FromEnv(std::getenv("BINUI_GPU"));
    GpuBackend picked = GpuBackend::Cpu;
    if (!AskBinGpu(want, &picked)) return want == GpuBackend::Auto ? GpuBackend::Cpu : want;
    return picked;
}

BinUIAuto::BinUIAuto() = default;
BinUIAuto::~BinUIAuto() { shutdown(); }

bool BinUIAuto::open(GpuBackend which, void* hwnd, int width, int height, bool vsync) {
    inner_.reset();
    active_ = which;
    std::unique_ptr<IBinUIGfx> made;
    switch (which) {
        case GpuBackend::Vulkan: made = std::make_unique<BinUIVK>(); break;
        case GpuBackend::GL:     made = std::make_unique<BinUIGL>(); break;
        case GpuBackend::D3D11:  made = std::make_unique<BinUID3D>(); break;
        case GpuBackend::BinGpu: made = std::make_unique<BinUIGpu>(); break;
        default:                 made = std::make_unique<BinUI2D>(); break;
    }
    // 策略必须在 init **之前**转给内层：bin2d 那条路要在建交换链时按策略决定 vsync。
    made->setPacePolicyText(paceText_);
    bool ok;
    if (which == GpuBackend::Cpu) {
        BinUI2D* c = static_cast<BinUI2D*>(made.get());
        ok = hwnd ? c->init(hwnd, width, height, vsync) : c->init(width, height);
    } else if (which == GpuBackend::Vulkan) {
        ok = static_cast<BinUIVK*>(made.get())->init(hwnd, width, height, vsync);
    } else if (which == GpuBackend::GL) {
        ok = static_cast<BinUIGL*>(made.get())->init(hwnd, width, height, vsync);
    } else if (which == GpuBackend::BinGpu) {
        ok = static_cast<BinUIGpu*>(made.get())->init(hwnd, width, height, vsync);
    } else {
        ok = static_cast<BinUID3D*>(made.get())->init(hwnd, width, height, vsync);
    }
    if (!ok) {
        std::fprintf(stderr, "[binui] 后端 %s 起不来\n", GpuBackendName(which));
        return false;   // made 析构时后端自己 shutdown
    }
    inner_ = std::move(made);
    offscreen_ = (hwnd == nullptr);
    return true;
}

bool BinUIAuto::init(int width, int height) {
    // 离屏：GPU 三端的离屏路径都依赖"没有窗口就没有交换链"，行为不一致，
    // 所以离屏**固定用 CPU 光栅**（截图/金标准要的是确定像素，不是速度）。
    const bool ok = open(GpuBackend::Cpu, nullptr, width, height, false);
    if (ok)
        std::fprintf(stderr, "[binui] 后端: bin2d（离屏固定走 CPU 光栅，像素确定）\n");
    return ok;
}

bool BinUIAuto::init(void* hwnd, int width, int height, bool vsync) {
    // 节拍器先起来（在选后端之前）：交换链的垂直同步位要按策略决定 ——
    // "unlimited"/"240" 不关 vsync 就永远超不过刷新率。
    legacyPacer_.reset(new BgpuPacer);
    bingpu_pace_init(legacyPacer_.get(), hwnd);
    if (paceText_[0]) {
        BgpuPacePolicy pol = BGPU_PACE_STANDARD;
        if (bingpu_pace_policy_parse(paceText_, &pol))
            bingpu_pace_set_policy(legacyPacer_.get(), pol);
    }
    const bool allowVsync = vsync && bingpu_pace_wants_vsync(legacyPacer_.get()) != 0;

    GpuBackend want = ResolveGpuBackend();
    const char* env = std::getenv("BINUI_GPU");
    // 自动选的那一个起不来时往后退（VK → GL → D3D11 → CPU），保证总能出画面。
    const GpuBackend order[] = {want, GpuBackend::BinGpu, GpuBackend::Vulkan,
                               GpuBackend::GL, GpuBackend::D3D11, GpuBackend::Cpu};
    for (GpuBackend b : order) {
        if (b == GpuBackend::Auto) continue;
        if (open(b, hwnd, width, height, allowVsync)) {
            std::fprintf(stderr, "[binui] 后端: %s（%s；binGPU 可用: VK=%d GL=%d D3D11=%d）\n",
                         GpuBackendName(b),
                         env && *env ? "BINUI_GPU 指定" : "按 binGPU 探测自动选",
                         bingpu_backend_available(BGPU_BACKEND_VULKAN) ? 1 : 0,
                         bingpu_backend_available(BGPU_BACKEND_GL) ? 1 : 0,
                         bingpu_backend_available(BGPU_BACKEND_D3D11) ? 1 : 0);
            return true;
        }
    }
    return false;
}

void BinUIAuto::shutdown() {
    if (!inner_) return;
    if (active_ == GpuBackend::Cpu) static_cast<BinUI2D*>(inner_.get())->shutdown();
    else if (active_ == GpuBackend::Vulkan) static_cast<BinUIVK*>(inner_.get())->shutdown();
    else if (active_ == GpuBackend::GL) static_cast<BinUIGL*>(inner_.get())->shutdown();
    else if (active_ == GpuBackend::BinGpu) static_cast<BinUIGpu*>(inner_.get())->shutdown();
    else static_cast<BinUID3D*>(inner_.get())->shutdown();
    inner_.reset();
}

void BinUIAuto::presentViaGdi() {
    if (!inner_) return;
    if (!legacyPaceActive()) {
        if (active_ == GpuBackend::Cpu) static_cast<BinUI2D*>(inner_.get())->presentViaGdi();
        else if (active_ == GpuBackend::Vulkan) static_cast<BinUIVK*>(inner_.get())->presentViaGdi();
        else if (active_ == GpuBackend::GL) static_cast<BinUIGL*>(inner_.get())->presentViaGdi();
    else if (active_ == GpuBackend::BinGpu) static_cast<BinUIGpu*>(inner_.get())->presentViaGdi();
        else if (active_ == GpuBackend::BinGpu) static_cast<BinUIGpu*>(inner_.get())->presentViaGdi();
        else static_cast<BinUID3D*>(inner_.get())->presentViaGdi();
        return;
    }
    // 贴屏不等刷新 → present_ms 传 0，节拍器按"从本拍开始算一格"处理。
    if (active_ == GpuBackend::Vulkan) static_cast<BinUIVK*>(inner_.get())->presentViaGdi();
    else if (active_ == GpuBackend::GL) static_cast<BinUIGL*>(inner_.get())->presentViaGdi();
    else if (active_ == GpuBackend::D3D11) static_cast<BinUID3D*>(inner_.get())->presentViaGdi();
    bingpu_pace_frame_end(legacyPacer_.get(), PacerNowUs(), 0.0);
}

// ---- IBinUIGfx：全部直通（内层为空时给安全的空答复） ----
bool BinUIAuto::isReady() const { return inner_ && inner_->isReady(); }
int  BinUIAuto::width() const { return inner_ ? inner_->width() : 0; }
int  BinUIAuto::height() const { return inner_ ? inner_->height() : 0; }
void BinUIAuto::resize(int w, int h) { if (inner_) inner_->resize(w, h); }
void BinUIAuto::setVsync(bool on) { if (inner_) inner_->setVsync(on); }
void BinUIAuto::setTransparent(bool t) { if (inner_) inner_->setTransparent(t); }
void BinUIAuto::beginFrame(const void* rc) {
    if (legacyPaceActive()) bingpu_pace_frame_begin(legacyPacer_.get(), PacerNowUs());
    if (inner_) inner_->beginFrame(rc);
}

void BinUIAuto::present() {
    if (!legacyPaceActive()) { if (inner_) inner_->present(); return; }
    // 只量 present 自身：这是"它到底有没有在等垂直同步"的唯一证据（见 binGPU 的说明）。
    // 注意 presentViaGdi() 不走这里 —— 贴屏路径由外壳按刷新网格兜底。
    const long long t0 = PacerNowUs();
    if (inner_) inner_->present();
    const long long t1 = PacerNowUs();
    bingpu_pace_frame_end(legacyPacer_.get(), t1, static_cast<double>(t1 - t0) / 1000.0);
}

bool BinUIAuto::presentPacesFrames() const { return inner_ && inner_->presentPacesFrames(); }

// ---- 帧节拍 ----
bool BinUIAuto::paceAvailable() const {
    if (inner_ && inner_->paceAvailable()) return true;
    return legacyPaceActive();
}

double BinUIAuto::paceFrameMs() const {
    if (inner_ && inner_->paceAvailable()) return inner_->paceFrameMs();
    if (legacyPaceActive()) return bingpu_pace_frame_ms(legacyPacer_.get());
    return 0.0;
}

long long BinUIAuto::paceWaitUs() {
    if (inner_ && inner_->paceAvailable()) return inner_->paceWaitUs();
    if (legacyPaceActive()) return static_cast<long long>(bingpu_pace_wait_us(legacyPacer_.get(), PacerNowUs()));
    return 0;
}

double BinUIAuto::paceRefreshMs() const {
    if (inner_ && inner_->paceAvailable()) return inner_->paceRefreshMs();
    if (legacyPaceActive()) {
        const int hz = legacyPacer_->refresh_hz;
        return hz > 1 ? 1000.0 / static_cast<double>(hz) : 0.0;
    }
    return 0.0;
}

void BinUIAuto::setPacePolicyText(const char* text) {
    if (!text) text = "";
    std::snprintf(paceText_, sizeof(paceText_), "%s", text);
    if (inner_) inner_->setPacePolicyText(paceText_);
    if (legacyPacer_) {
        BgpuPacePolicy pol = BGPU_PACE_STANDARD;
        if (bingpu_pace_policy_parse(paceText_, &pol))
            bingpu_pace_set_policy(legacyPacer_.get(), pol);
    }
}

const char* BinUIAuto::pacePolicyName() const {
    if (inner_ && inner_->paceAvailable()) return inner_->pacePolicyName();
    if (legacyPaceActive()) return bingpu_pace_policy_name(bingpu_pace_policy(legacyPacer_.get()));
    return "unavailable";
}

void BinUIAuto::fillRect(float x, float y, float w, float h, uint32_t c) {
    if (inner_) inner_->fillRect(x, y, w, h, c);
}
void BinUIAuto::fillRounded(float x, float y, float w, float h, float r, uint32_t c) {
    if (inner_) inner_->fillRounded(x, y, w, h, r, c);
}
void BinUIAuto::fillRoundedXf(float w, float h, float r, bool stroke, float th, uint32_t c,
                              const float xf[6]) {
    if (inner_) inner_->fillRoundedXf(w, h, r, stroke, th, c, xf);
}
void BinUIAuto::fillLine(float x0, float y0, float x1, float y1, float w, uint32_t c) {
    if (inner_) inner_->fillLine(x0, y0, x1, y1, w, c);
}
void BinUIAuto::strokeRounded(float x, float y, float w, float h, float r, float th,
                              uint32_t c) {
    if (inner_) inner_->strokeRounded(x, y, w, h, r, th, c);
}
bool BinUIAuto::fillGradientQuad(float x, float y, float w, float h, float r, float ang,
                                 uint32_t from, uint32_t to) {
    return inner_ && inner_->fillGradientQuad(x, y, w, h, r, ang, from, to);
}
void BinUIAuto::drawImageEx(uint16_t i, float u0, float v0, float u1, float v1,
                            float x, float y, float w, float h, uint32_t tint) {
    if (inner_) inner_->drawImageEx(i, u0, v0, u1, v1, x, y, w, h, tint);
}
void BinUIAuto::drawImageNine(uint16_t i, float x, float y, float w, float h, float slice,
                              int repX, int repY, uint32_t tint) {
    if (inner_) inner_->drawImageNine(i, x, y, w, h, slice, repX, repY, tint);
}
void BinUIAuto::drawText(float x, float y, float size, int weight, const std::string& s,
                         uint32_t c, bool ch, bool cv, bool it) {
    if (inner_) inner_->drawText(x, y, size, weight, s, c, ch, cv, it);
}
void BinUIAuto::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                                const std::string& s, uint32_t c, int align, bool it) {
    if (inner_) inner_->drawTextWrapped(x, y, maxW, size, weight, s, c, align, it);
}
float BinUIAuto::textWidth(float size, int weight, const std::string& s, bool it) {
    return inner_ ? inner_->textWidth(size, weight, s, it) : 0.0f;
}
void BinUIAuto::fontMetrics(float size, int weight, int& a, int& lh, bool it) {
    if (inner_) inner_->fontMetrics(size, weight, a, lh, it);
    else { a = 0; lh = 0; }
}
void BinUIAuto::setTextSubpixel(bool on) { if (inner_) inner_->setTextSubpixel(on); }
void BinUIAuto::setLetterSpacing(float px) { if (inner_) inner_->setLetterSpacing(px); }
std::string BinUIAuto::ellipsize(const std::string& s, float maxW, float size, int weight,
                                bool it) {
    return inner_ ? inner_->ellipsize(s, maxW, size, weight, it) : s;
}
bool BinUIAuto::setAffine(const float xf[6]) { return inner_ && inner_->setAffine(xf); }
void BinUIAuto::setImageFilter(const BinUIImageFilter* f) {
    if (inner_) inner_->setImageFilter(f);
}
bool BinUIAuto::setEraseBlend(bool on) { return inner_ && inner_->setEraseBlend(on); }
bool BinUIAuto::setKeepBlend(bool on) { return inner_ && inner_->setKeepBlend(on); }
bool BinUIAuto::supportsMask() const { return inner_ && inner_->supportsMask(); }
bool BinUIAuto::backdropBlur(float x, float y, float w, float h, float r, float blur,
                             float grain) {
    return inner_ && inner_->backdropBlur(x, y, w, h, r, blur, grain);
}
void BinUIAuto::setTextShadows(const BinUITextShadow* l, int n) {
    if (inner_) inner_->setTextShadows(l, n);
}
bool BinUIAuto::pushLayer(float x, float y, float w, float h, float a) {
    return inner_ && inner_->pushLayer(x, y, w, h, a);
}
bool BinUIAuto::pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) {
    return inner_ && inner_->pushLayerEx(x, y, w, h, fx);
}
void BinUIAuto::popLayer() { if (inner_) inner_->popLayer(); }
bool BinUIAuto::prepareImage(uint16_t i, const uint8_t* d, size_t n, uint32_t g) {
    return inner_ && inner_->prepareImage(i, d, n, g);
}
bool BinUIAuto::prepareImageRaw(uint16_t i, int w, int h, const uint8_t* rgba, uint32_t g) {
    return inner_ && inner_->prepareImageRaw(i, w, h, rgba, g);
}
int BinUIAuto::pushClip(float x, float y, float w, float h) {
    return inner_ ? inner_->pushClip(x, y, w, h) : -1;
}
void BinUIAuto::popClipTo(int depth) { if (inner_) inner_->popClipTo(depth); }
void BinUIAuto::setFontName(const char* name) { if (inner_) inner_->setFontName(name); }

} // namespace binui
