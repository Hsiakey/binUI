// binui_bin2d.cpp - bin2d 后端：IBinUIGfx → bin2d::Canvas
//
// 这份实现是 master 2026-11-19 那条分层的落点：
//
//     binUI  ──►  bin2d / bin3d  ──►  binGPU  ──►  GL / Vulkan / D3D11
//
// 于是 binUI 这一层**一行图形 API 都没有**：没有 gl*、没有 vk*、没有 D3D 调用，
// 只有"把原语翻译成 bin2d 的调用"。像素由 bin2d 的覆盖率光栅决定（它是 CPU 像素
// 权威），所以这条路画出来的东西天然与 bin2d 的其他产物同口径，也与"将来 bin2d
// 走 binGPU 上屏"的路径一致 —— 换了显卡，像素不会变。
//
// 复用而不是重写（三件套，全部与 GL / VK / D3D11 共用同一份代码）：
//   · binui_glyph_atlas.h  —— 排版 / 字形光栅 / 整形缓存 / 文字阴影
//   · binui_paint.h        —— 九宫格切分、渐变四角色、圆角层几何
//   · binui_win_image.h    —— 图片解码（GDI+，三端同一份）
// 这也是为什么"文字与图片"这两块几乎不用写：它们本来就是后端无关的，唯一要做的
// 是把**出口**从"发射四边形给驱动"换成"画进 bin2d 的 Surface"。
#include "binui_bin2d.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 周期计数器（0.32.0）：原语探针以前用 steady_clock，在**被抢占的机器上会量出
// 4 倍的假耗时**（1020×684 的 buttonshow：同一进程 CPU 时间 4.03s / 4.12s 两次
// 完全一样，而"绘制"从 15.2ms 变成 109.7ms —— 差的全是等调度）。
// 单线程光栅的耗时应当用**自己吃掉的周期**度量：RDTSC 不受别的进程影响。
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

#include "bin2d/canvas.h"
#ifndef BINUI_NO_BINGPU
#include "bin2d/gpu.h"          // GpuCompositor：窗口呈现（present → bingpu 交换链）
// GpuExec（0.22.2）：**绘制本体**交给 binGPU。这一行就是分层图里的
//   binUI ──► bin2d ──► binGPU
// 的最后一跳：binUI 仍然只碰 bin2d（Canvas 的 API），bin2d 把图元发给执行体，
// 执行体落到设备上。binUI 这一侧没有一行 bingpu_*（连 include 都只有 bin2d 的头）。
#include "bin2d/gpu_exec.h"
#endif
#include "bin2d/image.h"
#include "bin2d/filter.h"       // ColorMatrix：图片滤镜（setImageFilter）
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/png.h"
#include "bin2d/raster.h"
#include "bin2d/surface.h"
#include "bin2d/transform.h"

#include "binui_filter.h"     // ImageFilterKey：图片滤镜的缓存键（四端同源）
#include "binui_format.h"
#include "binui_gfx.h"
#include "binui_glyph_atlas.h"
#include "binui_paint.h"
#include "binui_win_image.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace binui {
namespace {

// 0xAARRGGBB 直通 → bin2d 的预乘 Color。整份文件里只在这里做这个转换，
// 所以"颜色口径"只有一处可以改错。
inline bin2d::Color ToColor(uint32_t argb) { return bin2d::Color::argb(argb); }

// binUI 的 xf 布局是 [a c e; b d f]（见 binui_gfx.h 的 fillRoundedXf 注释），
// 与 bingpu_cmd_set_affine 的 xf[6] 完全一致，所以直接用 bin2d 的同名转换。
inline bin2d::Transform ToTransform(const float xf[6]) {
    return bin2d::Transform::fromBingpu(xf);
}

// ------------------------------------------------------------------
// 直通 BGRA（GDI+ 解码产物）→ bin2d 的预乘 Surface
//
// bin2d 内部一律预乘，而 DecodeImagePixels 给的是直通 BGRA。转换只有两处口径
// （这里是其中之一，另一处是 bin2d 自己的 gpu.h 上传路径），不能各写一份取整。
// ------------------------------------------------------------------
void BlitBgraStraightInto(bin2d::Surface& dst, int x, int y, int w, int h,
                          const uint8_t* bgra, int srcPitch) {
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= dst.height()) continue;
        const uint8_t* s = bgra + static_cast<size_t>(row) * static_cast<size_t>(srcPitch);
        uint8_t* d = dst.row(dy);
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            const uint8_t* sp = s + static_cast<size_t>(col) * 4;
            if (dx < 0 || dx >= dst.width()) continue;
            // BGRA 字节序 → 直通分量；再用 Color::straight 做一次预乘（唯一取整口径）
            const bin2d::Color c =
                bin2d::Color::straight(sp[2], sp[1], sp[0], sp[3]);
            uint8_t* dp = d + static_cast<size_t>(dx) * 4;
            dp[0] = c.r; dp[1] = c.g; dp[2] = c.b; dp[3] = c.a;
        }
    }
}

// 预乘 BGRA（字形图集用）→ 预乘 Surface：只换字节序，**不再乘一次 alpha**。
//
// 0.31.1 修的真 bug：字形位图从 binui_glyph_atlas.h 出来时已经是**预乘白色**
// （`p[0]=p[1]=p[2]=p[3]=覆盖率`，见那边的 rasterGray 注释），而这里曾经走
// `BlitBgraStraightInto`（按直通再预乘一次）。于是每个纹素变成
// `rgb = a²/255, alpha = a`—— 覆盖率在 RGB 上被平方了。
//
// 症状很具体：白字压在浅底上时，字缘比底色**更暗**（白底上核心只有 191 而不是
// 255），一眼就是"白色文本带一圈灰边"；同时整体墨量偏少，小字号拉丁看起来发虚，
// 与笔画密的汉字放一起就显得"有的粗有的细"。三份 GPU 后端直接把字节传上纹理，
// 所以只有 bin2d 后端有这个问题 —— 也正是它能被逐像素比对照出来的原因。
void BlitBgraPremulInto(bin2d::Surface& dst, int x, int y, int w, int h,
                        const uint8_t* bgra, int srcPitch) {
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= dst.height()) continue;
        const uint8_t* s = bgra + static_cast<size_t>(row) * static_cast<size_t>(srcPitch);
        uint8_t* d = dst.row(dy);
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= dst.width()) continue;
            const uint8_t* sp = s + static_cast<size_t>(col) * 4;
            uint8_t* dp = d + static_cast<size_t>(dx) * 4;
            dp[0] = sp[2];   // B G R A → R G B A（分量照抄，alpha 已预乘过）
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = sp[3];
        }
    }
}

// 预乘直通 RGBA（prepareImageRaw 的输入，见 binui_gfx.h 注释）→ 预乘 Surface
void BlitRgbaStraightInto(bin2d::Surface& dst, int x, int y, int w, int h,
                          const uint8_t* rgba) {
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= dst.height()) continue;
        const uint8_t* s = rgba + static_cast<size_t>(row) * static_cast<size_t>(w) * 4;
        uint8_t* d = dst.row(dy);
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= dst.width()) continue;
            const uint8_t* sp = s + static_cast<size_t>(col) * 4;
            const bin2d::Color c = bin2d::Color::straight(sp[0], sp[1], sp[2], sp[3]);
            uint8_t* dp = d + static_cast<size_t>(dx) * 4;
            dp[0] = c.r; dp[1] = c.g; dp[2] = c.b; dp[3] = c.a;
        }
    }
}

// ------------------------------------------------------------------
// 字形 UV（0.26.1 勘误）
//
// 一开始在这里做了一次"半个纹素"的 UV 补偿（把 u 反算成 u*sw-0.5 再除以 sw-1），
// 想对齐 bin2d 的 `u*(sw-1)` 采样口径。**那是错的方向**：补偿之后 UV 不再是整纹素
// 边界，bin2d 的 1:1 快路径（raster.cpp 的 ①b）判据就命不中了，字形只能走查表
// 双线性 —— 既慢（25 µs/字形）又仍然是偏的。
//
// 正确的落点是 bin2d 的 ①b：UV 保持**整纹素边界**时，那个快路径按 `sx0 + rx`
// 取纹素，正好等于 GPU 硬件在纹素中心采样的结果，而且只做一次查表 + 混合。
// 所以这里不加任何补偿 —— 传原始 UV 就是对的。
// ------------------------------------------------------------------

} // namespace

// ==================================================================
// 字形宿主：把"位图 → 纹理"这一步换成"位图 → bin2d::Image"
//
// GL 版的宿主是 glTexSubImage2D，VK 版是 staging buffer + copy，这里就是
// 往一张 Surface 里写。三种实现共用同一份排版/光栅/缓存（binui_glyph_atlas.h），
// 所以字形位图逐字节相同 —— 差异只可能来自后面的采样（见 drawText 的注释）。
//
// 0.22.3 加的第二条路（deviceTextures）：GPU 执行体下不再用 Image/Surface，
// 而是直接管执行体的纹理句柄 —— 与 binui_gpu.cpp 的 GpuGlyphHost 同一套做法
// （create 一次、每次只上传新格子那块矩形）。为什么必须这样：
//   · Image 那条路的口径是"图片不变"，图集边画边改就只能整张重传（1024² = 4 MB/次），
//     一页文字一帧要改几十个格子；
//   · 更要命的是先后关系：GPU 的绘制命令延迟到帧尾执行，而图集是边画边贴新格子，
//     "整张重传 + 已录进命令流的笔"之间残留的旧内容会被后面的笔采样到
//     （实测：整页文字串格、阴影层错位）。
// 设备纹理的上传在 binGPU 里是**同步**的（提交后等 fence），图集又只追加不改写
// 已有格子 ⇒ 先上传还是先录笔都不改变任何一笔的采样结果。
// CPU 光栅（没有执行体）仍然走 Image：那一边是逐像素立即落盘，本来就没有这个时序差。
// ==================================================================
struct GlyphHost2D : public IGlyphTexHost {
    std::vector<std::unique_ptr<bin2d::Image>> owned;
    // 当前帧的 Canvas（beginFrame / init 里更新）。CPU 路用它通知"图集原地改过像素"。
    bin2d::Canvas* canvas = nullptr;
    // true = 纹理句柄归 bin2d 的 GPU 执行体（见上面那段说明）。由 Impl 按执行体
    // 的有无来切；切换时必须先让图集把旧宿主上的纹理销毁（见 Impl::setGlyphDeviceTextures）。
    bool deviceTextures = false;
    // BGRA → RGBA 的交换缓冲（执行体的纹理是 RGBA8，字形位图是预乘 BGRA）
    std::vector<uint8_t> swz;

    bool deviceMode() const { return deviceTextures && canvas != nullptr; }

    static void* AsHandle(uint64_t t) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(t));
    }
    static uint64_t AsTex(const void* p) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    }

    void* createTex(int w, int h) override {
        if (w <= 0 || h <= 0) return nullptr;
        if (deviceMode()) {
            // 必须全 0（透明）：图集里的"空格子"会被当真字形画，未初始化的显存会画成垃圾；
            // 执行体的 texCreate 自带零上传。
            const uint64_t t = canvas->createTexture(w, h, /*nearest=*/false);
            return t ? AsHandle(t) : nullptr;
        }
        // 必须全 0（透明）：图集里的"空格子"会被当真字形画（advance 有效、像素全透明），
        // Surface::resize 已经保证清零（见 surface.h）。
        auto img = std::make_unique<bin2d::Image>(w, h);
        if (img->empty()) return nullptr;
        void* handle = img.get();
        owned.push_back(std::move(img));
        return handle;
    }

    void updateTex(void* tex, int x, int y, int w, int h, const uint8_t* bgra) override {
        if (!tex || !bgra || w <= 0 || h <= 0) return;
        if (deviceMode()) {
            // 只传这一块：执行体的上传是同步的、图集只追加不改写，所以先传后画/先画后传
            // 在设备上看到的都是同一份最终内容。
            const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
            swz.resize(n * 4u);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* s = bgra + i * 4u;
                uint8_t* d = swz.data() + i * 4u;
                d[0] = s[2];   // B G R A → R G B A
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
            }
            canvas->uploadTexture(AsTex(tex), bin2d::Rect(static_cast<float>(x),
                                                          static_cast<float>(y),
                                                          static_cast<float>(w),
                                                          static_cast<float>(h)),
                                  swz.data(), static_cast<uint32_t>(w) * 4u);
            return;
        }
        auto* img = static_cast<bin2d::Image*>(tex);
        // 字形位图是**预乘**的（覆盖率同时写在 RGB 和 alpha 上），别再预乘一次
        BlitBgraPremulInto(img->surface(), x, y, w, h, bgra, w * 4);
        // 原地改完必须作废执行体的纹理缓存（CPU 执行体上这是空操作）
        if (canvas) canvas->imageChanged(img->surface());
    }

    void destroyTex(void* tex) override {
        if (!tex) return;
        if (deviceMode()) {
            // 执行体的 destroy 是延迟的（帧尾之后才真正释放），所以帧内销毁 + 重建
            // 不会让已经录好的笔采样到新纹理。
            canvas->destroyTexture(AsTex(tex));
            return;
        }
        for (size_t i = 0; i < owned.size(); ++i) {
            if (owned[i].get() == tex) {
                owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    void clear() {
        owned.clear();
        swz.clear();
        swz.shrink_to_fit();
    }
};

// ==================================================================

// ------------------------------------------------------------------
// 消融测量（0.32.0，BINUI2D_SKIP=rstl）
//
// 为什么不用"每次调用掐表"：这台机器上 `steady_clock::now()` / `__rdtsc()` 单次要约
// 40 µs（同一帧 234 次 fillRounded：关着 11.84 ms、只计数 15.59 ms、**开计时 69.73 ms**，
// 而整进程 CPU 只多 2.7 ms —— 是虚拟化把 TSC 读成陷阱，不是代码慢）。所以按调用掐表
// 在这种机器上量出来的"原语耗时"是假的。
//
// 换一条路：**把它关掉，看整体少了多少**。关掉某个原语后帧耗时的差值就是它的真实成本，
// 不依赖任何时钟精度。
//   r = fillRounded / strokeRounded 走空实现
//   s = strokeRounded 空实现
//   t = drawText / drawTextWrapped 空实现
//   l = pushLayer / popLayer 空实现（图层内容照样画，只是不再单独合成）
//   g = fillGradientQuad 空实现
// 例：BINUI2D_SKIP=r → 与基准的帧耗时差 = 圆角填充的成本。
// ------------------------------------------------------------------

// ==================================================================

// ------------------------------------------------------------------
// 原语级**计数**探针（0.30.0，BINUI2D_PROF=1）
//
// 为什么需要它：全屏重画 23.6 ms 这个数字本身没有信息量 —— 它既可能是"像素太多"
// 也可能是"每个原语都有固定开销"。用 bin2d 自己的基准反推（1.6 M 填充像素只值
// 2.2–8.3 ms，而一张卡片实测 236 µs、其中 53% 是那条 1px 圆角边框）只能给出
// **等价解**，不是这一页的真实构成。所以按原语分类在这里计一次数，
// 让"这一帧都画了什么"变成可读的数字，而不是猜。
//
// 计时默认关闭：这台机器（虚拟化）单次读时钟约 40 µs，按调用掐表会把帧耗时放大 6 倍
// （见上面的"消融测量"注释）。要看时间就用消融法；BINUI2D_PROF=time 才开计时。
// 计数本身的开销约 +3.7 ms/帧（同样因为有 234 次调用），测纯性能时别开。
struct ProfTimer;

// 当前核心的周期计数（不做序列化：探针只要"量级对"，不要精确到指令）。
inline unsigned long long RdtscNow() {
#if defined(_MSC_VER) || (defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__)))
    return __rdtsc();
#else
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// TSC 频率（GHz）：用 QPC 标定 20 ms。只标定一次，标定失败按 3.0 GHz 兜底
// （报告的 ms 会有个固定比例误差，但**分类占比**不受影响 —— 那才是这张表的用途）。
inline double TscGhz() {
    static const double ghz = [] {
        const auto t0 = std::chrono::steady_clock::now();
        const unsigned long long c0 = RdtscNow();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.02) {
        }
        const unsigned long long c1 = RdtscNow();
        const double sec = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - t0).count();
        if (sec <= 0.0 || c1 <= c0) return 3.0;
        return static_cast<double>(c1 - c0) / sec / 1e9;
    }();
    return ghz;
}

struct Prof {
    enum Slot {
        kFillRect, kFillRounded, kFillRoundedXf, kStrokeRounded, kFillLine, kGrad,
        kDrawImage, kDrawImageNine, kDrawText, kDrawTextWrapped,
        kPushLayer, kPushLayerEx, kPopLayer, kBackdropBlur, kPresent,
        kSlotCount
    };
    static const char* Name(int s) {
        switch (s) {
            case kFillRect:        return "fillRect";
            case kFillRounded:     return "fillRounded";
            case kFillRoundedXf:   return "fillRoundedXf";
            case kStrokeRounded:   return "strokeRounded";
            case kFillLine:        return "fillLine";
            case kGrad:            return "fillGradientQuad";
            case kDrawImage:       return "drawImageEx";
            case kDrawImageNine:   return "drawImageNine";
            case kDrawText:        return "drawText";
            case kDrawTextWrapped: return "drawTextWrapped";
            case kPushLayer:       return "pushLayer";
            case kPushLayerEx:     return "pushLayerEx";
            case kPopLayer:        return "popLayer";
            case kBackdropBlur:    return "backdropBlur";
            case kPresent:         return "present()";
            default:               return "?";
        }
    }
    bool on = false;
    // 只计数不计时（BINUI2D_PROF=count）：把"计数开销"和"计时开销"分开。
    bool timing = true;
    long long calls[kSlotCount] = {};
    // 累加的是**周期**，不是微秒：RDTSC 只算本核吃掉的拍子，被别的进程抢走的时间
    // 不会记到原语头上（steady_clock 会，实测能把同一段代码量成 4 倍）。
    unsigned long long cyc[kSlotCount] = {};
    // 盒面积累计：让"µs/次"能换算成"ns/像素" —— 否则一个 900×932 的圆角底
    // 和一个 12×14 的圆点在表里是同一行"次/帧"，看不出钱花在哪种面积上。
    double    boxPx[kSlotCount] = {};
    // 非整像素对齐的调用数：决定 bin2d 能不能走"中段整宽矩形"那条快速路径
    long long nonAligned[kSlotCount] = {};
    int frames = 0;

    void Reset() {
        for (int i = 0; i < kSlotCount; ++i) { calls[i] = 0; cyc[i] = 0; boxPx[i] = 0.0; nonAligned[i] = 0; }
        frames = 0;
    }
    // 按总耗时降序打印（每帧均摊）。frames==0 时按累计值打印。
    void Dump(const char* tag) const {
        if (!on) return;
        const double ghz = TscGhz();
        const double div = frames > 0 ? static_cast<double>(frames) : 1.0;
        unsigned long long totalCyc = 0;
        for (int i = 0; i < kSlotCount; ++i) totalCyc += cyc[i];
        const double totalMs = static_cast<double>(totalCyc) / (ghz * 1e6);
        std::fprintf(stderr,
                     "[bin2d-prof] %s frames=%d 原语总耗时 %.2f ms/帧（%s；TSC %.2f GHz）\n",
                     tag, frames, totalMs / div, timing ? "计时" : "仅计数", ghz);
        int order[kSlotCount];
        for (int i = 0; i < kSlotCount; ++i) order[i] = i;
        for (int i = 0; i < kSlotCount; ++i)
            for (int j = i + 1; j < kSlotCount; ++j)
                if (cyc[order[j]] > cyc[order[i]]) { const int t = order[i]; order[i] = order[j]; order[j] = t; }
        for (int k = 0; k < kSlotCount; ++k) {
            const int i = order[k];
            if (calls[i] == 0) continue;
            const double usTotal = static_cast<double>(cyc[i]) / ghz / 1e3;   // 周期 → µs
            const double perFrameMs = usTotal / div / 1000.0;
            const double pct = totalCyc > 0 ? 100.0 * static_cast<double>(cyc[i]) /
                                                  static_cast<double>(totalCyc) : 0.0;
            const double nsPerPx = boxPx[i] > 0.0 ? usTotal * 1000.0 / boxPx[i] : 0.0;
            std::fprintf(stderr,
                         "[bin2d-prof]   %-16s %7.1f 次/帧  %8.2f us/次  %7.3f ms/帧  %5.1f%%"
                         "  %7.0f kpx/帧  %6.2f ns/px  非整对齐 %.1f/帧\n",
                         Name(i), static_cast<double>(calls[i]) / div,
                         usTotal / static_cast<double>(calls[i]), perFrameMs, pct,
                         boxPx[i] / div / 1000.0, nsPerPx, static_cast<double>(nonAligned[i]) / div);
        }
        std::fflush(stderr);
    }
};

struct ProfTimer {
    Prof* p = nullptr;
    unsigned long long t0 = 0;
    int id = 0;
    ProfTimer(Prof& prof, int slot) : id(slot) {
        if (!prof.on) return;
        p = &prof;
        if (prof.timing) t0 = RdtscNow();
    }
    ~ProfTimer() {
        if (!p) return;
        if (p->timing) p->cyc[id] += RdtscNow() - t0;
        ++p->calls[id];
    }
};

struct BinUI2D::Impl {
    Prof prof;
    // 消融开关（BINUI2D_SKIP=rstlg，见上面的注释）。按字符打开，init 时解析一次。
    bool skipFillRounded = false;
    bool skipStrokeRounded = false;
    bool skipText = false;
    bool skipLayer = false;
    bool skipGradient = false;
    void parseSkip(const char* s) {
        if (!s) return;
        for (const char* p = s; *p; ++p) {
            switch (*p) {
                case 'r': skipFillRounded = true; break;
                case 's': skipStrokeRounded = true; break;
                case 't': skipText = true; break;
                case 'l': skipLayer = true; break;
                case 'g': skipGradient = true; break;
                default: break;
            }
        }
    }
    int width = 0;
    int height = 0;
    bool ready = false;

    bin2d::Surface surface;
    std::unique_ptr<bin2d::Canvas> canvas;

    // ---- 窗口呈现（0.26.1）----
    // 三种状态，从快到慢：
    //   ① compositor 就绪  → Surface 上传成纹理，present 到 bingpu 交换链（GPU 上屏）
    //   ② hwnd 非空        → Surface 反预乘成 BGRA，SetDIBitsToDevice 贴屏（无驱动兜底）
    //   ③ 都没有           → 离屏：像素留在 surface 上（bin2d_shot 走这条）
    // ---- 窗口呈现（0.26.1）----
    // 三种状态，从快到慢：
    //   ① compositor 就绪  → Surface 上传成纹理，present 到 bingpu 交换链（GPU 上屏）
    //   ② hwnd 非空        → Surface 反预乘成 BGRA，StretchDIBits 贴屏（无驱动兜底）
    //   ③ 都没有           → 离屏：像素留在 surface 上（bin2d_shot 走这条）
#ifndef BINUI_NO_BINGPU
    // ---- 绘制执行体（0.22.2）----
    // exec 非空 = 所有绘制都交给 bin2d::GpuExec（bin2d 的语义 + binGPU 的执行），
    // Surface **不再收像素**；present 走 exec->present()（交换链）。
    // 它和 compositor 的关系是"二选一，不是串起来"：一个进程一个设备 ——
    // exec 就绪时不再建 compositor（那条路是"CPU 出 Surface + 上传贴屏"）。
    std::unique_ptr<bin2d::GpuExec> exec;
    // 有一条路要上传——用 "bin2d+gpu" 还是 "bin2d+gpu-compositor" 区分，见 presentBackendName()
    std::unique_ptr<bin2d::GpuCompositor> compositor;
    bool gpuExecActive() const { return exec && exec->ready(); }

    // 建绘制执行体。hwnd == nullptr → HEADLESS（离屏，present 之后 readback 出像素）。
    // 后端交给 binGPU 的 AUTO 去挑（VK → GL → D3D11 → CPU），失败返回 false 让
    // 调用方退回下一级 —— **不是失败**，无显卡的机器也要能开窗。
    bool openExec(void* win, int w, int h, bool vsyncOn) {
        exec = std::make_unique<bin2d::GpuExec>();
        // 节拍策略必须在 create **之前**灌：交换链的 vsync 位是建链时定的。
        if (paceText[0]) exec->setPacePolicy(ParsePace(paceText));
        if (exec->create(win, w, h, BGPU_BACKEND_AUTO, vsyncOn)) {
            // 一行诊断：走的是哪个后端、MSAA 实际开到几×（档位由 binGPU 的
            // render_plan 定，这里只是如实转述交换链上报的值）。排查"画质停在 1×"
            // 时先看这一行，不要去猜环境变量。
            std::fprintf(stderr, "[binui2d] exec: backend=%s msaa=%dx\n",
                         exec->backendName(), exec->msaaSamples());
            return true;
        }
        exec.reset();
        return false;
    }

    // 建窗口版合成器（GL → Vulkan → D3D11，桌面平台上 WGL 最成熟所以先试）。
    // vsync 是**建链时**定的，所以换 vsync 只能重建 —— 见 setVsync。
    bool openCompositor(int w, int h, bool vsync) {
        // 顺序按 master 2026-11-20 定的优先级：**VK > GL > D3D11**
        // （以前是 GL 先 —— 当时 WGL 最成熟；现在 VK 的交换链路径已经稳定，且实测
        //  present 直接贴合成边界，比 GL 少一半呈现耗时，所以按优先级排。）
        const BgpuBackendKind order[] = {BGPU_BACKEND_VULKAN, BGPU_BACKEND_GL,
                                         BGPU_BACKEND_D3D11};
        for (BgpuBackendKind k : order) {
            if (!bingpu_backend_available(k)) continue;
            compositor = std::make_unique<bin2d::GpuCompositor>();
            // 节拍策略必须在 createWindow **之前**灌进去：交换链的垂直同步位要按策略决定
            // （unlimited / 240 fps 不关 vsync 就是一句空话）。见 bin2d::GpuCompositor。
            if (paceText[0]) compositor->setPacePolicy(ParsePace(paceText));
            if (compositor->createWindow(hwnd, w, h, k, vsync)) return true;
            compositor.reset();
        }
        return false;
    }

    // ---- 三级降级（0.22.2）----
    // ① exec 就绪  → 绘制在 GPU 上，present 直接是交换链（binUI → bin2d → binGPU）
    // ② compositor → CPU 出 Surface，整张上传贴屏（老的那条 GPU 呈现路）
    // ③ 都没有     → GDI 贴屏 / 离屏（无显卡的机器靠这条）
    // 一个进程一个设备：① 起来就不再建 ②。
    // 两条"不走 GPU"的理由要在这里就掐掉，不然建出来也是个死设备：
    //   · BINUI2D_NO_GPU=1（验收/隔离用）；
    //   · transparent：分层窗要的是**Surface 里的逐像素 alpha**，交换链没有它
    //     （见 present() 的 ⓪ 支）。
    bool openGpuPath(int w, int h, bool vsyncOn) {
        if (!gpuAllowed || transparent) return false;
        if (openExec(hwnd, w, h, vsyncOn)) {
            presentPath = "bin2d+gpu";
            // Canvas 建好就挂上执行体：图集懒加载字形（排版测宽那一步也不在帧内）
            // 就要能建纹理，所以不能等到第一次 beginFrame。
            if (canvas) canvas->attachGpu(exec.get());
            setGlyphDeviceTextures(true);
            return true;
        }
        if (openCompositor(w, h, vsyncOn)) { presentPath = "bin2d+gpu-compositor"; return true; }
        return false;
    }

    void closeGpuPath() {
        // 先让图集按**旧**宿主把手上的纹理销毁，再拆执行体：设备句柄属于执行体，
        // 执行体一没就既销毁不了、也不能再采样（见 setGlyphDeviceTextures）。
        setGlyphDeviceTextures(false);
        if (exec) { exec->destroy(); exec.reset(); }
        if (compositor) { compositor->destroy(); compositor.reset(); }
    }

#endif
    void* hwnd = nullptr;
    const char* presentPath = "bin2d+offscreen";
    // GPU 路径允不允许（BINUI2D_NO_GPU=1 关掉）。init 时解析一次，resize/setVsync
    // 重建时沿用 —— 环境变量不会中途变。
    bool gpuAllowed = true;
    // 帧节拍策略的**原文**（"standard"/"unlimited"/"240"/...）。
    // 存字符串而不是解析后的枚举：合成的 compositor 会被 setVsync / resize 重建，
    // 每次重建都要把策略再灌一遍；而解析表只有 binGPU 那一份（bingpu_pace_policy_parse）。
    char paceText[16] = {0};
#ifndef BINUI_NO_BINGPU
    static BgpuPacePolicy ParsePace(const char* text) {
        BgpuPacePolicy p = BGPU_PACE_STANDARD;
        return bingpu_pace_policy_parse(text, &p) ? p : BGPU_PACE_STANDARD;
    }
#endif
    // 反预乘用的缓冲（GDI 贴屏复用，避免每帧分配）
    std::vector<uint8_t> gdiPixels;

    // ---- 透明分层窗（0.27.1）----
    // 交换链那条路**没有逐像素 alpha**：WS_EX_LAYERED 窗口必须用
    // UpdateLayeredWindow 把一整块 premultiplied BGRA 交给 DWM 合成。
    // 所以 transparent 时 present() 的第一支就不是 compositor。
    //   · GL 后端靠 glReadPixels 拿回 premultiplied BGRA；
    //   · bin2d 的 Surface 本来就是 premultiplied RGBA —— 只差一次 R/B 交换，
    //     不需要反预乘（反预乘只服务 SRCCOPY 贴屏，见 presentViaGdi）。
#ifdef _WIN32
    HBITMAP layeredDib = nullptr;
    HDC     layeredDC = nullptr;
    void*   layeredBits = nullptr;
    int     layeredW = 0, layeredH = 0;
#endif

    // ---- 状态（每次 beginFrame 都要把它们重新压进新 Canvas）----
    bool hasAffine = false;
    float affine[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    bool eraseBlend = false;
    bool keepBlend = false;
    bool transparent = false;
    bool vsync = true;
    bool subpixelText = false;

    // ---- 裁剪栈 ----
    // 每项记录"进入该层裁剪时的 saveCount"。popClipTo(depth) 就是 restore 到这个深度。
    std::vector<int> clipMarks;

    // ---- 图层栈 ----
    // offscreen 为 true 的层在 pop 时走 popOffscreen + 自己处理效果（blur），
    // 因为 bin2d 的 pushLayer 不支持"pop 时模糊"。
    struct LayerRec {
        bool offscreen = false;
        float alpha = 1.0f;
        float blurPx = 0.0f;
        // 外阴影（0.27.1）：书写顺序，靠前的在上层 → 合成时逆序
        std::vector<BinUIFilterDropShadowEntry> shadows;
        size_t clipMark = 0;   // 入层时的 clipMarks.size()，出层要截断
    };
    std::vector<LayerRec> layers;

    // ---- 图片 ----
    struct ImageEntry {
        std::unique_ptr<bin2d::Image> img;
        uint32_t generation = 0xFFFFFFFFu;
        // 滤镜变体（0.27.1）：GL 那边是"矩阵进纹理缓存键，同一张图配两种滤镜 = 两份
        // 纹理"；bin2d 这里是"同一张图配两种滤镜 = 两份 Image"。键用 binui::
        // ImageFilterKey 的 64 位哈希，与三份 GPU 后端同一个函数 —— 三端缓存键同源。
        std::unique_ptr<bin2d::Image> filtered;
        uint64_t filterKey = 0;
    };
    std::unordered_map<uint32_t, ImageEntry> images;
    // setImageFilter 设进来的状态：键 0 = 无滤镜
    uint64_t filterKey = 0;
    bin2d::ColorMatrix filterMatrix = bin2d::ColorMatrix::identity();

    // ---- 文字 ----
    GlyphHost2D host;
    std::unique_ptr<GlyphAtlas> atlas;

    void ApplyState() {
        if (!canvas) return;
        canvas->setBlendMode(eraseBlend ? bin2d::BlendMode::Erase
                                        : (keepBlend ? bin2d::BlendMode::Keep
                                                     : bin2d::BlendMode::SrcOver));
        if (hasAffine) canvas->setTransform(ToTransform(affine));
        else canvas->setTransform(bin2d::Transform::identity());
    }

    // ---- 字形图集的纹理归属（0.22.3）----
    // 有了执行体就用**设备纹理**（句柄归执行体、每次只上传新格子那块矩形），
    // 没有就走 Image/Surface（CPU 光栅）。切换时必须先让图集按**旧**宿主把纹理
    // 销毁再翻标志：设备句柄属于执行体，执行体一没就销毁不了；而留在图集里的旧句柄
    // 会被当成新纹理去采样（表现就是整页文字串格）。
    void setGlyphDeviceTextures(bool on) {
        if (host.deviceTextures == on) return;
        if (atlas) atlas->clear();   // 此刻旧宿主（执行体 + Canvas）还活着
        host.deviceTextures = on;
    }

    // 一个字形四边形：两条宿主下出口不同（设备纹理 / CPU 光栅的 Image）
    void drawGlyphQuad(const GlyphQuad& g) {
        if (!canvas) return;
        const bin2d::Rect dst(g.x, g.y, g.w, g.h);
        const bin2d::Rect uv(g.u0, g.v0, g.u1 - g.u0, g.v1 - g.v0);
        const bin2d::Color c = ToColor(g.color);
        if (host.deviceMode()) {
            canvas->drawTexture(GlyphHost2D::AsTex(g.tex), dst, uv, c);
            return;
        }
        auto* img = static_cast<bin2d::Image*>(g.tex);
        if (!img || img->empty()) return;
        canvas->drawImage(*img, dst, uv, c, bin2d::ImageFilter::Linear);
    }

    bin2d::Image* findImage(uint16_t index) {
        auto it = images.find(index);
        if (it == images.end() || !it->second.img || it->second.img->empty()) return nullptr;
        return it->second.img.get();
    }

    // 绘制用的图：有滤镜时返回"按 (图, 矩阵) 缓存"的过滤变体。
    // 变体是**一次性**的（换图或换矩阵才重建），不进每帧热路径。
    bin2d::Image* findImageForDraw(uint16_t index) {
        auto it = images.find(index);
        if (it == images.end() || !it->second.img || it->second.img->empty()) return nullptr;
        ImageEntry& e = it->second;
        if (filterKey == 0 || filterMatrix.isIdentity()) return e.img.get();
        if (!e.filtered || e.filterKey != filterKey) {
            e.filtered = std::make_unique<bin2d::Image>(*e.img);
            if (e.filtered && !e.filtered->empty())
                bin2d::ApplyColorMatrix(e.filtered->surface(), filterMatrix);
            e.filterKey = filterKey;
            // mip 跟着旧图算出来的，形状换了就作废
            if (e.filtered) e.filtered->clearMips();
        }
        return (e.filtered && !e.filtered->empty()) ? e.filtered.get() : e.img.get();
    }

#ifdef _WIN32
    // 透明分层窗：premultiplied RGBA → premultiplied BGRA（顶层 DIB）→ UpdateLayeredWindow。
    // 返回 false = 这条路走不通（窗口不是 WS_EX_LAYERED / 没窗口），调用方退回贴屏。
    bool presentLayered() {
        if (!hwnd || surface.empty()) return false;
        HWND win = static_cast<HWND>(hwnd);
        if (!(GetWindowLongW(win, GWL_EXSTYLE) & WS_EX_LAYERED)) return false;
        const int w = surface.width(), h = surface.height();
        if (layeredDib == nullptr || layeredW != w || layeredH != h) {
            if (layeredDib) DeleteObject(layeredDib);
            if (layeredDC) DeleteDC(layeredDC);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;          // 负 = 顶层（与 Surface 行序一致）
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            layeredDib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &layeredBits, nullptr, 0);
            layeredDC = CreateCompatibleDC(nullptr);
            layeredW = w;
            layeredH = h;
            if (!layeredDib || !layeredDC || !layeredBits) return false;
        }
        // 只换字节序，**不动 alpha**：alpha 已经是预乘过的，ULW_ALPHA 要的就是它
        uint8_t* d = static_cast<uint8_t*>(layeredBits);
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = surface.row(y);
            uint8_t* row = d + static_cast<size_t>(y) * w * 4u;
            for (int x = 0; x < w; ++x, s += 4, row += 4) {
                row[0] = s[2];
                row[1] = s[1];
                row[2] = s[0];
                row[3] = s[3];
            }
        }
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) return false;
        HGDIOBJ old = SelectObject(layeredDC, layeredDib);
        RECT wr;
        GetWindowRect(win, &wr);
        POINT dst = { wr.left, wr.top };
        SIZE  sz = { w, h };
        POINT src = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        const BOOL ok = UpdateLayeredWindow(win, screenDC, &dst, &sz, layeredDC, &src, 0, &bf,
                                            ULW_ALPHA);
        SelectObject(layeredDC, old);
        ReleaseDC(nullptr, screenDC);
        return ok != FALSE;
    }
#endif
};

BinUI2D::BinUI2D() : impl_(std::make_unique<Impl>()) {
    impl_->atlas = std::make_unique<GlyphAtlas>();
    impl_->atlas->setHost(&impl_->host);
    impl_->atlas->initFonts();
}

BinUI2D::~BinUI2D() { shutdown(); }

bool BinUI2D::init(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    shutdown();
    {
        // 原语级探针（0.30.0）：BINUI2D_PROF=1 打开，退出时按原语分类打一张表。
        // 为什么用它：全屏 23.6 ms 既可能是像素多、也可能是每个原语都有固定开销，
        // 用别的基准反推只能得到等价解；这里给的是**这一页**的真实构成。
        const char* e = std::getenv("BINUI2D_PROF");
        impl_->prof.on = e && *e && std::strcmp(e, "0") != 0;
        // 计时默认关：这台机器上一次 now() 约 40 µs（虚拟化），按调用掐表会把帧耗时
        // 放大 6 倍。要时间请用 BINUI2D_SKIP 的消融法。
        impl_->prof.timing = e && std::strcmp(e, "time") == 0;
        impl_->prof.Reset();
        impl_->parseSkip(std::getenv("BINUI2D_SKIP"));
        // BINUI2D_NO_GPU=1 强制走 CPU / GDI（验收兜底路径 / 隔离"慢在 CPU 还是慢在 GPU"）
        const char* noGpu = std::getenv("BINUI2D_NO_GPU");
        impl_->gpuAllowed = !(noGpu && *noGpu && std::strcmp(noGpu, "0") != 0);
    }
    impl_->width = width;
    impl_->height = height;
    if (!impl_->surface.resize(width, height)) return false;
    impl_->canvas = std::make_unique<bin2d::Canvas>(impl_->surface);
    impl_->host.canvas = impl_->canvas.get();   // 图集原地改像素要通知 canvas
    impl_->ready = true;
#ifndef BINUI_NO_BINGPU
    // 离屏**默认仍然走 CPU 光栅**：Surface 是像素权威（pixels-bin2d 的 0 容差基线就是
    // 它出的），换成 GPU 出图会让那扇门禁整个失去意义。
    // BINUI2D_GPU=1 才建 HEADLESS 执行体 —— 那条路上 present() 会 exec->readback()，
    // 像素照样落在 Surface 里，所以 pixels()/savePng() 的行为不变。
    const char* gpuEnv = std::getenv("BINUI2D_GPU");
    const bool wantGpu = gpuEnv && *gpuEnv && std::strcmp(gpuEnv, "0") != 0;
    if (wantGpu && impl_->gpuAllowed) {
        if (impl_->openExec(nullptr, width, height, false)) {
            impl_->presentPath = "bin2d+gpu";
            // 见 openGpuPath：图集在帧外（排版测宽）也可能懒加载字形，纹理必须能建。
            impl_->canvas->attachGpu(impl_->exec.get());
            impl_->setGlyphDeviceTextures(true);
        } else {
            // 不刷屏也不冒充成功：调用方（出图工具）看到的是 CPU 出来的像素
            std::fprintf(stderr, "[binui2d] BINUI2D_GPU=1 但 HEADLESS 执行体起不来，"
                                 "这一次仍按 CPU 光栅出图\n");
        }
    }
#endif
    // 字形图集与 Canvas 无关，init 之后一直活着（换图集代数由 atlas 自己管）
    if (!impl_->atlas) {
        impl_->atlas = std::make_unique<GlyphAtlas>();
        impl_->atlas->setHost(&impl_->host);
        impl_->atlas->initFonts();
    }
    return true;
}

bool BinUI2D::init(void* hwnd, int width, int height, bool vsync) {
    if (!init(width, height)) return false;
    impl_->hwnd = hwnd;
    impl_->vsync = vsync;
    impl_->presentPath = hwnd ? "bin2d+gdi" : "bin2d+offscreen";
#ifndef BINUI_NO_BINGPU
    // 离屏那一支可能已经按 BINUI2D_GPU=1 建了 HEADLESS 执行体：窗口版要把它换成
    // 绑窗口的同一个设备（一个进程只有一个设备，绝不并存两份）。
    impl_->closeGpuPath();
    if (hwnd) {
        // 三级降级的入口：exec → compositor → GDI 贴屏。一个后端都起不来不是失败。
        impl_->openGpuPath(width, height, impl_->vsync);
    }
#endif
    return true;
}

void BinUI2D::shutdown() {
    impl_->prof.Dump("bin2d 后端原语分解");
#ifndef BINUI_NO_BINGPU
    impl_->closeGpuPath();
#endif
#ifdef _WIN32
    if (impl_->layeredDib) { DeleteObject(impl_->layeredDib); impl_->layeredDib = nullptr; }
    if (impl_->layeredDC) { DeleteDC(impl_->layeredDC); impl_->layeredDC = nullptr; }
    impl_->layeredBits = nullptr;
    impl_->layeredW = impl_->layeredH = 0;
#endif
    impl_->canvas.reset();
    impl_->images.clear();
    impl_->clipMarks.clear();
    impl_->layers.clear();
    impl_->gdiPixels.clear();
    impl_->surface = bin2d::Surface();
    impl_->ready = false;
    impl_->width = impl_->height = 0;
    impl_->hwnd = nullptr;
    impl_->presentPath = "bin2d+offscreen";
}

bool BinUI2D::isReady() const { return impl_->ready; }
int  BinUI2D::width() const { return impl_->width; }
int  BinUI2D::height() const { return impl_->height; }

void BinUI2D::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    impl_->width = width;
    impl_->height = height;
    impl_->surface.resize(width, height);
    impl_->canvas = std::make_unique<bin2d::Canvas>(impl_->surface);
    impl_->host.canvas = impl_->canvas.get();
    // 先挂再拆：closeGpuPath 要经这个 Canvas 把图集纹理销毁（没挂执行体的 Canvas
    // 上的 destroyTexture 是空操作 —— 纹理就漏了）
    if (impl_->gpuExecActive()) impl_->canvas->attachGpu(impl_->exec.get());
#ifndef BINUI_NO_BINGPU
    // 交换链的尺寸是建链时定的：执行体存在就必须销毁重建（没有第二条路），
    // 重建之后 Canvas 要重新 attachGpu —— 否则下一帧悄悄退回 CPU 光栅，
    // 而 Surface 又不会收到像素（黑屏）。
    if (impl_->exec) {
        const void* win = impl_->hwnd;
        impl_->closeGpuPath();
        impl_->presentPath = win ? "bin2d+gdi" : "bin2d+offscreen";
        if (!impl_->openGpuPath(width, height, impl_->vsync)) {
            std::fprintf(stderr, "[binui2d] resize 后 GPU 路径重建失败，退回 %s\n",
                         impl_->presentPath);
        }
    }
    if (impl_->gpuExecActive()) impl_->canvas->attachGpu(impl_->exec.get());
#endif
    impl_->clipMarks.clear();
    impl_->layers.clear();
}

void BinUI2D::setVsync(bool on) {
    if (impl_->vsync == on) return;
    impl_->vsync = on;
#ifndef BINUI_NO_BINGPU
    // 交换链的 vsync 位是建链时定的，运行期改它只能重建 —— 否则 setVsync 就成了一句空话
    // （诊断里还会继续报"present 可阻塞垂直同步"，等于撒谎）。
    if (impl_->hwnd && (impl_->exec || (impl_->compositor && impl_->compositor->ready()))) {
        impl_->closeGpuPath();
        impl_->presentPath = "bin2d+gdi";
        if (impl_->openGpuPath(impl_->width, impl_->height, on))
            impl_->canvas->attachGpu(impl_->gpuExecActive() ? impl_->exec.get() : nullptr);
    }
#endif
}
void BinUI2D::setTransparent(bool t) {
    if (impl_->transparent == t) return;
    impl_->transparent = t;
#ifndef BINUI_NO_BINGPU
    // 分层透明窗要的是 **Surface 里的逐像素 alpha**（UpdateLayeredWindow），交换链
    // 给不出 —— 所以打开透明就得把执行体拆掉、整帧退回 CPU 光栅。
    // 这是一条**已知的互斥**，不是 bug：要么 GPU 出图（不透明），要么逐像素透明。
    if (t && (impl_->exec || impl_->compositor)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "[binui2d] transparent=true 与 GPU 出图互斥：拆执行体，"
                         "退回 CPU 光栅 + UpdateLayeredWindow\n");
        }
        impl_->closeGpuPath();
        impl_->presentPath = impl_->hwnd ? "bin2d+gdi" : "bin2d+offscreen";
    }
#endif
}

const void* BinUI2D::pixels() const {
    return impl_->surface.empty() ? nullptr : impl_->surface.pixels().data();
}

size_t BinUI2D::pixelsSize() const {
    return impl_->surface.empty() ? 0 : impl_->surface.byteSize();
}

bool BinUI2D::savePng(const char* path) const {
    if (impl_->surface.empty() || !path) return false;
    return bin2d::writePng(impl_->surface, std::string(path));
}

// ------------------------------------------------------------------
// 帧
// ------------------------------------------------------------------
void BinUI2D::beginFrame(const void* clipRect) {
    if (!impl_->ready) return;
    ++impl_->prof.frames;
#ifndef BINUI_NO_BINGPU
    // 节拍锚点 = **帧开始**（不是 present 开始）：外壳量到的帧间隔才不会跟着绘制耗时抖。
    if (impl_->gpuExecActive()) impl_->exec->paceFrameBegin();
    else if (impl_->compositor && impl_->compositor->ready())
        impl_->compositor->paceFrameBegin();
#endif
    // 每帧重建 Canvas：Canvas 构造绑定目标 Surface，而 Surface 只 resize 一次。
    // 重建的代价是一次 State 拷贝（几百字节），换来的是"帧与帧之间状态绝对不串"
    // —— 上一帧漏掉的 restore 不会把这一帧的裁剪带歪（那类 bug 极难复现）。
    impl_->canvas = std::make_unique<bin2d::Canvas>(impl_->surface);
    impl_->host.canvas = impl_->canvas.get();   // 每帧重建 Canvas：这一个指针也得跟着换
    impl_->clipMarks.clear();
    impl_->layers.clear();

    // 新 Canvas 不带执行体：**每帧都必须重新挂**。漏这一行的症状是"什么都不报、
    // 画面全黑"：绘制悄悄退回 CPU 光栅（Surface 被写），而屏幕上贴的是 GPU 纹理
    // （从没收到过像素）。
    const bool gpu = impl_->gpuExecActive();
    if (gpu) impl_->canvas->attachGpu(impl_->exec.get());

    bin2d::Rect area;
    const bool clipped = (clipRect != nullptr);
    if (clipped) {
        const int* r = static_cast<const int*>(clipRect);
        area = bin2d::Rect(static_cast<float>(r[0]), static_cast<float>(r[1]),
                           static_cast<float>(r[2] - r[0]), static_cast<float>(r[3] - r[1]));
    }
    if (gpu) {
        // 开 render pass **必须在任何 canvas 绘制之前**：beginFrame 之前发图元会落进
        // "没有 pass"的状态，执行体只能丢掉它们。
        // clear = 只有整帧重绘（clipRect == nullptr）时才清整张 canvas；
        // 脏矩形那一支走下面的 canvas->clear()（只换那一块，其余原地保留）。
        impl_->exec->beginFrame(0x00000000u, /*clear=*/!clipped);
        if (clipped) {
            // Canvas::clear 是 Replace 语义（连 alpha 一起换），与 binUI 的
            // beginFrame 的"只清这块矩形"是同一条口径。
            impl_->canvas->save();
            impl_->canvas->clipRect(area);
            impl_->canvas->clear(bin2d::Color::trans());
            impl_->canvas->restore();
            impl_->canvas->clipRect(area);   // 定下本帧的绘制裁剪
        }
    } else if (clipped) {
        impl_->surface.clearRect(area);
        impl_->canvas->clipRect(area);
    }
    impl_->ApplyState();
    if (impl_->atlas) impl_->atlas->snapText = !impl_->subpixelText;
}

void BinUI2D::present() {
    // present 是每帧固定成本里最大的一块（3 条路径各自的价钱差别很大），
    // 所以把它也当"原语"记一档：贴屏 10 ms 里到底多少是转换、多少是交换链，
    // 靠这一行与 upload 的分段一起看。
    ProfTimer _pt(impl_->prof, Prof::kPresent);
    // ⓪ 透明分层窗（0.27.1）：交换链没有逐像素 alpha，必须先判。
    //    compositor 一旦建起来就会一直贴着这个窗口，这里直接拆掉，
    //    否则后面 setTransparent(false) 切回来时它已经错过了帧。
#ifdef _WIN32
    if (impl_->transparent) {
#ifndef BINUI_NO_BINGPU
        // 分层窗与 GPU 出图互斥（见 setTransparent）：这里的兜底只是防御 ——
        // 正常路径上 setTransparent(true) 已经把执行体拆干净了。
        if (impl_->exec) impl_->closeGpuPath();
#endif
        if (impl_->presentLayered()) {
            impl_->presentPath = "bin2d+layered";
            return;
        }
        // 窗口没有 WS_EX_LAYERED（宿主自己关掉了）→ 退回普通贴屏：
        // 半透明区域会变成"直接盖住桌面"，但画面不会丢。
        impl_->presentPath = impl_->hwnd ? "bin2d+gdi" : "bin2d+offscreen";
        presentViaGdi();
        return;
    }
#endif
    // ⓪ GPU 执行体（0.22.2）：绘制本来就在设备上，present 只是 canvas → 交换链。
    //    离屏（HEADLESS）多一步 readback，让 pixels()/savePng() 照样取得到像素。
#if !defined(BINUI_NO_BINGPU)
    if (impl_->gpuExecActive()) {
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = impl_->exec->present();
        if (ok && impl_->hwnd == nullptr) ok = impl_->exec->readback(impl_->surface);
        const double presentMs = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0).count();
        // present() 里已经收过节拍了（那边量的是交换链那一段）；这里再调一次是
        // 空操作 —— 留着是为了让"谁收尾"这件事在调用顺序上也是自洽的，
        // 见 bin2d::GpuExec::paceFrameEnd 的守卫。
        impl_->exec->paceFrameEnd(presentMs);
        if (ok) return;
        // 一次失败就永久退回：present 每帧都调，不能每帧重试一条坏路。
        // 注意这一帧的内容在 GPU 上，Surface 里没有像素 —— 退回去的这一帧会是空的，
        // 下一帧 beginFrame 重新按 CPU 光栅画（canvas 不再 attachGpu）。
        impl_->closeGpuPath();
        impl_->presentPath = impl_->hwnd ? "bin2d+gdi" : "bin2d+offscreen";
        std::fprintf(stderr, "[binui2d] GPU 执行体 present 失败，永久退回 %s\n",
                     impl_->presentPath);
    }
#endif
    // ① GPU 上屏（CPU 出 Surface，整张上传贴屏）
#ifndef BINUI_NO_BINGPU
    if (impl_->compositor && impl_->compositor->ready()) {
        if (impl_->compositor->presentSurface(impl_->surface)) return;
        // 一次失败就永久退回 GDI：present 每帧都调，不能每帧重试一条坏路。
        // 原因会留在 compositor 的 error 里（调试时看 presentBackendName 变了没）。
        impl_->compositor->destroy();
        impl_->compositor.reset();
        impl_->presentPath = impl_->hwnd ? "bin2d+gdi" : "bin2d+offscreen";
    }
#endif
    // ② 没有可用 GPU 后端：CPU 出图 + 系统贴屏（无显卡的机器靠这条）
    presentViaGdi();
}

void BinUI2D::presentViaGdi() {
#ifndef BINUI_NO_BINGPU
    // 执行体在的时候"贴屏"没有意义：这一帧的像素只在设备里，Surface 里是上一帧的
    // 残留（或者空的）。外壳在拖拽 resize 时会直接调这个函数（为了绕开驱动的
    // present 崩溃），那条路上也应当走交换链 —— 于是这里回到 present()。
    if (impl_->gpuExecActive()) { present(); return; }
#endif
#ifdef _WIN32
    // 贴屏这条路不经过交换链 → 节拍器要由这里收尾（present_ms 传 0：它不等刷新，
    // 节拍按"从本拍开始算一格"处理）。不调的话 beginFrame 开的那一帧永远不结束，
    // 节拍器就学不到任何东西，于是整条 GDI 路径等于没节拍。
#ifndef BINUI_NO_BINGPU
    if (impl_->compositor && impl_->compositor->ready()) impl_->compositor->paceFrameEnd(0.0);
#endif
    if (!impl_->hwnd || impl_->surface.empty()) return;
    // presentBackendName() 必须**跟着实际路径走**：透明分层窗关掉之后还报
    // "bin2d+layered" 就是在诊断信息里撒谎（测试就是这么发现的）。
    impl_->presentPath = "bin2d+gdi";
    const int w = impl_->surface.width(), h = impl_->surface.height();
    impl_->gdiPixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    // 预乘 RGBA → 直通 BGRA（BITMAPINFO 的 32bpp 是 BGRA 字节序 + 直通 alpha）
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = impl_->surface.row(y);
        uint8_t* d = impl_->gdiPixels.data() + static_cast<size_t>(y) * w * 4u;
        for (int x = 0; x < w; ++x, s += 4, d += 4) {
            const uint32_t a = s[3];
            if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            auto un = [a](uint8_t v) {
                const uint32_t r = (static_cast<uint32_t>(v) * 255u + a / 2u) / a;
                return static_cast<uint8_t>(r > 255u ? 255u : r);
            };
            d[0] = un(s[2]); d[1] = un(s[1]); d[2] = un(s[0]);
            d[3] = static_cast<uint8_t>(a);
        }
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // 负 = 自上而下（与 Surface 行序一致）
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(static_cast<HWND>(impl_->hwnd));
    if (!dc) return;
    // StretchDIBits 而不是 SetDIBitsToDevice：窗口尺寸与画布可能差一两个像素
    // （DPI 取整 / 拖拽中间态），拉伸一条路径比"Set 完再补一块"简单且不出缝。
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, w, h, 0, 0, w, h, impl_->gdiPixels.data(), &bi, DIB_RGB_COLORS,
                  SRCCOPY);
    ReleaseDC(static_cast<HWND>(impl_->hwnd), dc);
#endif
}

const wchar_t* BinUI2D::appClassName() {
    return L"BinUIApp2DWindow";
}

const char* BinUI2D::presentBackendName() const {
#ifndef BINUI_NO_BINGPU
    // 三级降级要**如实**报到后端名上，不许撒谎：
    //   "gl" / "vulkan" / "d3d11"        ← exec（绘制真的在设备上，binUI→bin2d→binGPU）
    //   "gl" / "vulkan" / "d3d11"        ← compositor（CPU 出图，整张上传贴屏）
    // 两条路的区别由 presentPath 分开（"bin2d+gpu" / "bin2d+gpu-compositor"），
    // 但对外报的是**真正在干活的那个后端名** —— 两个都是交换链，名字一样。
    if (impl_->gpuExecActive()) return impl_->exec->backendName();
    if (impl_->presentPath && std::strcmp(impl_->presentPath, "bin2d+gpu-compositor") == 0 &&
        impl_->compositor)
        return impl_->compositor->backendName();
#endif
    return impl_->presentPath;
}

bool BinUI2D::presentPacesFrames() const {
    // 只有真正走交换链、而且这条链**确实会阻塞到垂直同步**时，才认为 present 自己掐拍。
    // GDI 贴屏与离屏必须返回 false —— 它们不阻塞，外壳要按刷新率节流。
    //
    // 这个答案不能猜：以前这里写死 false（理由写的是"建链时没开 VSYNC 标志"），
    // 于是 120Hz 屏上外壳按 8.33ms 自己掐拍、而交换链是 IMMEDIATE 模式不锁刷新，
    // 帧落点漂在两档之间（实测 2.75 / 3.00 个刷新周期各占一半，抖动 44/239）。
    // 现在问 binGPU 的交换链本身（bingpu_swapchain_info 的 present_paces_frames）。
    //
    // v0.34.0 起这个值**只是诊断**：节拍已经统一交给 pace* 那组（binGPU 的节拍器），
    // 外壳不再拿它当分支条件。
#ifndef BINUI_NO_BINGPU
    if (impl_->gpuExecActive()) return impl_->exec->presentPacesFrames();
    if (impl_->compositor && impl_->compositor->ready())
        return impl_->compositor->presentPacesFrames();
#endif
    return false;
}

// ------------------------------------------------------------------
// 帧节拍（v0.34.0）：全套转发给 bin2d（GpuExec / GpuCompositor）→ binGPU
//
// **这一层不算节拍**。有执行体就问执行体，没有才问合成器；两条都没有
// （GDI / 离屏）就老实回答"没有节拍器"，让外壳按刷新网格兜底。
// ------------------------------------------------------------------
bool BinUI2D::paceAvailable() const {
#ifndef BINUI_NO_BINGPU
    if (impl_->gpuExecActive()) return true;
    // 只有交换链那条路才有"设备层"可以问。GDI 贴屏/离屏没有 —— 那两条路
    // present 不阻塞，由外壳按刷新网格兜底（老路径，本来也是对的）。
    return impl_->compositor && impl_->compositor->ready();
#else
    return false;
#endif
}

double BinUI2D::paceFrameMs() const {
#ifndef BINUI_NO_BINGPU
    if (impl_->gpuExecActive()) return impl_->exec->paceFrameMs();
    if (impl_->compositor && impl_->compositor->ready()) return impl_->compositor->paceFrameMs();
#endif
    return 0.0;
}

long long BinUI2D::paceWaitUs() {
#ifndef BINUI_NO_BINGPU
    if (impl_->gpuExecActive()) return static_cast<long long>(impl_->exec->paceWaitUs());
    if (impl_->compositor && impl_->compositor->ready())
        return static_cast<long long>(impl_->compositor->paceWaitUs());
#endif
    return 0;
}

double BinUI2D::paceRefreshMs() const {
#ifndef BINUI_NO_BINGPU
    if (impl_->gpuExecActive()) return impl_->exec->paceRefreshMs();
    if (impl_->compositor && impl_->compositor->ready())
        return impl_->compositor->paceRefreshMs();
#endif
    return 0.0;
}

void BinUI2D::setPacePolicyText(const char* text) {
    // 存原文，等执行体 / 合成器（可能被 setVsync / resize 重建）建起来时再灌。
    if (!text) text = "";
    std::snprintf(impl_->paceText, sizeof(impl_->paceText), "%s", text);
#ifndef BINUI_NO_BINGPU
    if (impl_->exec) impl_->exec->setPacePolicy(Impl::ParsePace(impl_->paceText));
    if (impl_->compositor) impl_->compositor->setPacePolicy(Impl::ParsePace(impl_->paceText));
#endif
}

const char* BinUI2D::pacePolicyName() const {
#ifndef BINUI_NO_BINGPU
    if (impl_->gpuExecActive())
        return bingpu_pace_policy_name(impl_->exec->pacePolicy());
    if (impl_->compositor && impl_->compositor->ready())
        return bingpu_pace_policy_name(impl_->compositor->pacePolicy());
    return bingpu_pace_policy_name(Impl::ParsePace(impl_->paceText));
#else
    return "standard";
#endif
}

// ------------------------------------------------------------------
// 几何
// ------------------------------------------------------------------
void BinUI2D::fillRect(float x, float y, float w, float h, uint32_t color) {
    ProfTimer _pt(impl_->prof, Prof::kFillRect);
    if (impl_->prof.on) impl_->prof.boxPx[Prof::kFillRect] += w * h;
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    impl_->canvas->fillRect(bin2d::Rect(x, y, w, h), ToColor(color));
}

void BinUI2D::fillRounded(float x, float y, float w, float h, float radius,
                          uint32_t color) {
    ProfTimer _pt(impl_->prof, Prof::kFillRounded);
    if (impl_->prof.on) {
        impl_->prof.boxPx[Prof::kFillRounded] += w * h;
        if (x != std::floor(x) || y != std::floor(y) || w != std::floor(w) || h != std::floor(h))
            ++impl_->prof.nonAligned[Prof::kFillRounded];
    }
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    if (impl_->skipFillRounded) return;    // BINUI2D_SKIP=r：消融测量
    // 半径 <= 0 = 直角（三端统一的回退语义，见 binui_gfx.h）
    if (radius <= 0.0f) {
        impl_->canvas->fillRect(bin2d::Rect(x, y, w, h), ToColor(color));
        return;
    }
    if (!impl_->prof.timing) {
        // 不掐表时别读时钟（这台机器一次 ~40 µs，会把整帧放大 6 倍）
        impl_->canvas->fillRounded(bin2d::Rect(x, y, w, h), radius, ToColor(color));
        return;
    }
    const unsigned long long slow0 = RdtscNow();
    impl_->canvas->fillRounded(bin2d::Rect(x, y, w, h), radius, ToColor(color));
    {
        // 单次 >250 µs 的调用单独点名：分类表只能说"fillRounded 贵"，
        // 要优化得知道贵在**多大、什么半径、对齐不对齐**的那几次。
        const double us = static_cast<double>(RdtscNow() - slow0) / TscGhz() / 1e3;
        const bool aligned = x == std::floor(x) && y == std::floor(y) &&
                             w == std::floor(w) && h == std::floor(h);
        static int budget = 40;
        if (us > 250.0 && budget-- > 0)
            std::fprintf(stderr,
                         "[bin2d-slow] fillRounded %.0fx%.0f r=%.1f 对齐=%d  %.0f us  (%.2f ns/px)\n",
                         w, h, radius, aligned ? 1 : 0, us,
                         w * h > 0 ? us * 1000.0 / (w * h) : 0.0);
    }
}

void BinUI2D::strokeRounded(float x, float y, float w, float h, float radius,
                            float thickness, uint32_t color) {
    ProfTimer _pt(impl_->prof, Prof::kStrokeRounded);
    if (impl_->prof.on) impl_->prof.boxPx[Prof::kStrokeRounded] += w * h;
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    if (impl_->skipStrokeRounded) return;   // BINUI2D_SKIP=s
    if (thickness <= 0.0f) return;
    // 0.22.2：走 bin2d 的**圆角描边原语**，不再自己拼 Path + strokePath。
    // 为什么必须换：GPU 执行体对任意路径描边只有一句"不支持"，挂上 GPU 之后
    // 所有圆角边框会整片消失（UI 里到处都是）。环的解析解在 GPU 上（SDF），
    // CPU 上仍然是"外圆角 + 内圆角组成环"的路径覆盖 —— 像素与以前逐位相同。
    //
    // 0.22.1 以前这里的等价关系（逐字节验过，别改成"外扩"）：binUI 的描边是
    // **向内**的同心层，等价于"内缩 thickness/2 的矩形 + 居中描边 thickness"。
    // 那条数学现在住在 bin2d::Canvas::strokeRounded 里，只有一份。
    impl_->canvas->strokeRounded(bin2d::Rect(x, y, w, h), radius, thickness, ToColor(color));
}

void BinUI2D::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                            uint32_t color, const float xf[6]) {
    ProfTimer _pt(impl_->prof, Prof::kFillRoundedXf);
    if (impl_->prof.on) impl_->prof.boxPx[Prof::kFillRoundedXf] += w * h;
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f || !xf) return;
    // fillRoundedXf 自带矩阵，**不受** setAffine 状态影响（见 binui_gfx.h 的注释：
    // 否则圆角底会被变换两次）。所以这里换掉当前变换、画完再还回去。
    const bin2d::Transform saved = impl_->canvas->transform();
    impl_->canvas->setTransform(ToTransform(xf));
    if (stroke) {
        // 描边：局部矩形是 (0,0,w,h)，内缩语义与 strokeRounded 一致（同一条原语）
        impl_->canvas->strokeRounded(bin2d::Rect(0.0f, 0.0f, w, h), radius, thickness,
                                     ToColor(color));
    } else if (radius <= 0.0f) {
        impl_->canvas->fillRect(bin2d::Rect(0.0f, 0.0f, w, h), ToColor(color));
    } else {
        impl_->canvas->fillRounded(bin2d::Rect(0.0f, 0.0f, w, h), radius, ToColor(color));
    }
    impl_->canvas->setTransform(saved);
}

void BinUI2D::fillLine(float x0, float y0, float x1, float y1, float width,
                       uint32_t color) {
    ProfTimer _pt(impl_->prof, Prof::kFillLine);
    if (!impl_->canvas || width <= 0.0f) return;
    // GPU 执行体没有三角化：任意路径描边在那条路上是"不支持"。而"一条线"在几何上
    // 就是"一个旋转过的矩形"—— 平端帽、端点正好在 (x0,y0)/(x1,y1)。所以 GPU 模式下
    // 换成"平移 + 旋转 + fillRect"，几何完全等价（AA 口径不同，见报告里的已知退化）。
    // CPU 那条路**不动**：仍走 strokePath，像素与 0 容差的基线逐位相同。
    if (impl_->canvas->gpuAttached()) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 0.0f)) return;
        const bin2d::Transform saved = impl_->canvas->transform();
        // 线段局部坐标 (u,v) ∈ [0,len]×[-w/2,w/2] 先旋转、再平移到 (x0,y0)，
        // 最后套上**当前**变换：T = saved ∘ translate ∘ rotate（Transform::operator*
        // 的语义是"先应用右边"）。
        const bin2d::Transform local =
            saved * bin2d::Transform::translate(x0, y0) *
            bin2d::Transform::rotate(std::atan2(dy, dx));
        impl_->canvas->setTransform(local);
        impl_->canvas->fillRect(bin2d::Rect(0.0f, -width * 0.5f, len, width), ToColor(color));
        impl_->canvas->setTransform(saved);
        return;
    }
    bin2d::Path path;
    path.moveTo(x0, y0);
    path.lineTo(x1, y1);
    bin2d::StrokeStyle style;
    style.width = width;
    // 平端帽、不外延：binUI 的 fillLine 是"实心旋转四边形"，端点正好在 (x0,y0)/(x1,y1)
    style.roundCap = false;
    style.squareCap = false;
    style.roundJoin = true;
    impl_->canvas->strokePath(path, bin2d::Paint::solid(ToColor(color)), style);
}

bool BinUI2D::fillGradientQuad(float x, float y, float w, float h, float radius,
                               float angleDeg, uint32_t from, uint32_t to) {
    ProfTimer _pt(impl_->prof, Prof::kGrad);
    if (impl_->prof.on) impl_->prof.boxPx[Prof::kGrad] += w * h;
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (impl_->skipGradient) return false;   // BINUI2D_SKIP=g
    const bin2d::Rect box(x, y, w, h);
    // CSS 语义（0° = 向上、顺时针增）由 bin2d 的 linearAngle 实现，与 GPU 端的
    // BuildGradientQuads 是同一套端点公式（两边都按盒子的投影长度取半长）。
    bin2d::Paint paint = bin2d::Paint::linearAngle(box, angleDeg, ToColor(from),
                                                  ToColor(to));
    // 关抖动（Bayer 8×8）：bin2d 默认开它是为了压住低 alpha 渐变的色带，但 GPU 端
    // 没有这一层 —— 开了就与三份 GPU 后端**永远**差 ±0.5 级色阶，像素门禁失去意义。
    // 对齐优先，画质增益留给后续"bin2d 成为唯一权威"的时候再统一开。
    paint.dither = false;
    if (radius <= 0.0f) {
        impl_->canvas->fillRect(box, paint);
    } else {
        impl_->canvas->fillRounded(box, radius, paint);
    }
    return true;
}

// ------------------------------------------------------------------
// 图片
// ------------------------------------------------------------------
bool BinUI2D::prepareImage(uint16_t index, const uint8_t* data, size_t size,
                           uint32_t generation) {
    if (!impl_->ready || !data || size == 0) return false;
    int w = 0, h = 0;
    const std::vector<uint8_t> bgra = DecodeImagePixels(data, size, w, h);
    if (bgra.empty() || w <= 0 || h <= 0) return false;

    auto img = std::make_unique<bin2d::Image>(w, h);
    if (img->empty()) return false;
    // DecodeImagePixels 的行距是 w*4（GDI+ LockBits 后按 w*4 排）
    BlitBgraStraightInto(img->surface(), 0, 0, w, h, bgra.data(), w * 4);

    auto& entry = impl_->images[index];
    entry.img = std::move(img);
    entry.generation = generation;
    return true;
}

bool BinUI2D::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                              uint32_t generation) {
    if (!impl_->ready || !rgba || w <= 0 || h <= 0) return false;
    auto img = std::make_unique<bin2d::Image>(w, h);
    if (img->empty()) return false;
    BlitRgbaStraightInto(img->surface(), 0, 0, w, h, rgba);

    auto& entry = impl_->images[index];
    entry.img = std::move(img);
    entry.generation = generation;
    return true;
}

void BinUI2D::drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                          float x, float y, float w, float h, uint32_t tint) {
    ProfTimer _pt(impl_->prof, Prof::kDrawImage);
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    bin2d::Image* img = impl_->findImageForDraw(index);
    if (!img) return;
    impl_->canvas->drawImage(*img, bin2d::Rect(x, y, w, h),
                             bin2d::Rect(u0, v0, u1 - u0, v1 - v0), ToColor(tint),
                             bin2d::ImageFilter::Linear);
}

void BinUI2D::drawImageNine(uint16_t index, float x, float y, float w, float h,
                            float slice, int repX, int repY, uint32_t tint) {
    ProfTimer _pt(impl_->prof, Prof::kDrawImageNine);
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    bin2d::Image* img = impl_->findImageForDraw(index);
    if (!img) return;
    // 切分数学在 binui_gfx.h 的 BuildNineSlice（三端共用），这里只按结果逐块画。
    // "九宫格要源图像素尺寸"这件事在 bin2d 侧是现成的（Image 自己知道宽高）。
    float q[kMaxNineSliceQuads][8];
    const int n = BuildNineSlice(x, y, w, h, static_cast<float>(img->width()),
                                 static_cast<float>(img->height()), slice, repX, repY,
                                 &q[0][0], kMaxNineSliceQuads);
    if (n <= 0) {
        drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, tint);
        return;
    }
    for (int i = 0; i < n; ++i) {
        drawImageEx(index, q[i][0], q[i][1], q[i][2], q[i][3],
                    q[i][4], q[i][5], q[i][6], q[i][7], tint);
    }
}

// ------------------------------------------------------------------
// 文字
// ------------------------------------------------------------------
void BinUI2D::drawText(float x, float y, float size, int weight,
                       const std::string& utf8, uint32_t color,
                       bool centerH, bool centerV, bool italic) {
    ProfTimer _pt(impl_->prof, Prof::kDrawText);
    if (!impl_->canvas || utf8.empty() || !impl_->atlas) return;
    if (impl_->skipText) return;   // BINUI2D_SKIP=t
    impl_->atlas->drawText(x, y, size, weight, utf8, color, centerH, centerV, italic,
                           [this](const GlyphQuad& g) { impl_->drawGlyphQuad(g); });
}

void BinUI2D::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                              const std::string& utf8, uint32_t color, int align,
                              bool italic) {
    ProfTimer _pt(impl_->prof, Prof::kDrawTextWrapped);
    if (!impl_->canvas || utf8.empty() || !impl_->atlas) return;
    if (impl_->skipText) return;   // BINUI2D_SKIP=t
    impl_->atlas->drawTextWrapped(x, y, maxW, size, weight, utf8, color, align, italic,
                                  [this](const GlyphQuad& g) { impl_->drawGlyphQuad(g); });
}

float BinUI2D::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->atlas) return 0.0f;
    return impl_->atlas->textWidth(size, weight, utf8, italic);
}

void BinUI2D::fontMetrics(float size, int weight, int& ascent, int& lineHeight,
                          bool italic) {
    if (!impl_->atlas) {
        ascent = static_cast<int>(size);
        lineHeight = static_cast<int>(size);
        return;
    }
    impl_->atlas->fontMetrics(size, weight, ascent, lineHeight, italic);
}

void BinUI2D::setTextSubpixel(bool on) {
    impl_->subpixelText = on;
    if (impl_->atlas) impl_->atlas->snapText = !on;
}

void BinUI2D::setLetterSpacing(float px) {
    if (impl_->atlas) impl_->atlas->setLetterSpacing(px);
}

void BinUI2D::setTextShadows(const BinUITextShadow* layers, int count) {
    if (impl_->atlas) impl_->atlas->setTextShadows(layers, count);
}

void BinUI2D::setFontName(const char* name) {
    if (impl_->atlas) impl_->atlas->setFontName(name);
}

// ------------------------------------------------------------------
// 状态
// ------------------------------------------------------------------
bool BinUI2D::setAffine(const float xf[6]) {
    if (!impl_->canvas) return false;
    if (!xf || (xf[0] == 1.0f && xf[1] == 0.0f && xf[2] == 0.0f &&
                xf[3] == 0.0f && xf[4] == 1.0f && xf[5] == 0.0f)) {
        impl_->hasAffine = false;
        impl_->canvas->setTransform(bin2d::Transform::identity());
        return true;
    }
    for (int i = 0; i < 6; ++i) impl_->affine[i] = xf[i];
    impl_->hasAffine = true;
    impl_->canvas->setTransform(ToTransform(xf));
    return true;
}

// v26 图片滤镜（0.27.1）：`BinUIImageFilter` 是"3×3 + 3 偏移、作用在直通 RGB"，
// bin2d 的 `ColorMatrix` 是"4×5、同样作用在直通空间" —— 两者是同一个东西的不同写法，
// 所以这里是**逐位搬运**而不是重新解释：
//   · 行 0..2 = f->m[0..8]（3×3，行主序），常数项 = f->offset[0..2]；
//   · 行 3 = alpha 恒等（1），第 4 列 0 —— 滤镜不动 alpha，这与 CSS filter 一致
//     （opacity 另走图层）。
// 矩阵的合成（多个 filter 函数按书写顺序相乘）在编译期就做完了，运行期只做逐像素乘。
void BinUI2D::setImageFilter(const BinUIImageFilter* f) {
    const uint64_t key = f ? binui::ImageFilterKey(f) : 0;
    if (key == 0) {
        impl_->filterKey = 0;
        impl_->filterMatrix = bin2d::ColorMatrix::identity();
        return;
    }
    bin2d::ColorMatrix m = bin2d::ColorMatrix::identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m.m[r][c] = f->m[r * 3 + c];
    for (int r = 0; r < 3; ++r) m.m[r][4] = f->offset[r];
    impl_->filterMatrix = m;
    impl_->filterKey = key;
}

bool BinUI2D::setEraseBlend(bool on) {
    if (!impl_->canvas) return false;
    impl_->eraseBlend = on;
    if (on) impl_->keepBlend = false;
    impl_->canvas->setBlendMode(on ? bin2d::BlendMode::Erase : bin2d::BlendMode::SrcOver);
    return true;
}

bool BinUI2D::setKeepBlend(bool on) {
    if (!impl_->canvas) return false;
    impl_->keepBlend = on;
    if (on) impl_->eraseBlend = false;
    impl_->canvas->setBlendMode(on ? bin2d::BlendMode::Keep : bin2d::BlendMode::SrcOver);
    return true;
}

bool BinUI2D::backdropBlur(float x, float y, float w, float h, float radius,
                           float blurPx, float grain) {
    ProfTimer _pt(impl_->prof, Prof::kBackdropBlur);
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (blurPx <= 0.0f) return false;

    const bin2d::Rect box(x, y, w, h);
    // GPU 执行体：整条链（抓背景 → 2× 降采样 → 分离高斯 → 圆角遮罩 → 颗粒）在
    // bin2d 里，binUI 只负责"传参 + 判断能不能做"。CPU 执行体返回 false，
    // 这里接着走下面那份本地的 CPU 实现。
    if (impl_->canvas->gpuAttached()) {
        return impl_->canvas->backdropBlur(box, radius, blurPx, grain);
    }
    const int bx = static_cast<int>(std::floor(box.x));
    const int by = static_cast<int>(std::floor(box.y));
    const int bw = static_cast<int>(std::ceil(box.w));
    const int bh = static_cast<int>(std::ceil(box.h));
    if (bw <= 0 || bh <= 0) return false;

    // 1) 抓当前目标的这块背景（bin2d 的 Blur 只吃 Surface，所以先拷出来）
    bin2d::Surface bg(bw, bh);
    if (bg.empty()) return false;
    for (int row = 0; row < bh; ++row) {
        const int sy = by + row;
        if (sy < 0 || sy >= impl_->surface.height()) continue;
        const uint8_t* s = impl_->surface.row(sy);
        uint8_t* d = bg.row(row);
        for (int col = 0; col < bw; ++col) {
            const int sx = bx + col;
            if (sx < 0 || sx >= impl_->surface.width()) continue;
            std::memcpy(d + static_cast<size_t>(col) * 4,
                        s + static_cast<size_t>(sx) * 4, 4);
        }
    }
    bin2d::BlurSigma(bg, blurPx);   // σ 语义：CSS blur(Npx) 的 N 就是 σ（bin2d 0.12.0）

    // 2) 圆角覆盖率：用一张白色圆角矩形当遮罩（AA 与形状同一套语义）
    bin2d::Surface mask(bw, bh);
    if (mask.empty()) return false;
    {
        bin2d::Canvas mc(mask);
        if (radius > 0.0f) {
            mc.fillRounded(bin2d::Rect(0.0f, 0.0f, static_cast<float>(bw),
                                       static_cast<float>(bh)),
                           radius, bin2d::Color::white());
        } else {
            mc.fillRect(bin2d::Rect(0.0f, 0.0f, static_cast<float>(bw),
                                    static_cast<float>(bh)),
                        bin2d::Color::white());
        }
    }

    // 3) 按覆盖率把模糊结果**替换**回背景位置（replace 语义：背景是被糊掉，不是叠一层）
    for (int row = 0; row < bh; ++row) {
        const int dy = by + row;
        if (dy < 0 || dy >= impl_->surface.height()) continue;
        const uint8_t* mp = mask.row(row);
        const uint8_t* bp = bg.row(row);
        uint8_t* dp = impl_->surface.row(dy);
        for (int col = 0; col < bw; ++col) {
            const int dx = bx + col;
            if (dx < 0 || dx >= impl_->surface.width()) continue;
            const uint8_t m = mp[static_cast<size_t>(col) * 4 + 3];
            if (m == 0) continue;
            uint8_t* p = dp + static_cast<size_t>(dx) * 4;
            const uint8_t* bp4 = bp + static_cast<size_t>(col) * 4;
            if (m == 255) {
                p[0] = bp4[0]; p[1] = bp4[1]; p[2] = bp4[2]; p[3] = bp4[3];
                continue;
            }
            // 预乘空间里按覆盖率插值是安全的（透明色分量本来就是 0）
            const bin2d::Color dst(p[0], p[1], p[2], p[3]);
            const bin2d::Color src(bp4[0], bp4[1], bp4[2], bp4[3]);
            const float k = static_cast<float>(m) / 255.0f;
            const bin2d::Color out = bin2d::lerp(dst, src, k);
            p[0] = out.r; p[1] = out.g; p[2] = out.b; p[3] = out.a;
        }
    }
    return true;
}

// ------------------------------------------------------------------
// 裁剪 / 图层
// ------------------------------------------------------------------
int BinUI2D::pushClip(float x, float y, float w, float h) {
    if (!impl_->canvas) return 0;
    const int depth = static_cast<int>(impl_->clipMarks.size());
    // 传进去的是**屏幕坐标**（drawNodes 在 pushClip 前会关掉 affine）：bin2d 的
    // Canvas::clipRect 按 deviceTransform()（= xf 减层原点）映射，所以层内也正确
    // —— 屏幕坐标减 origin 正好是层局部坐标。
    impl_->canvas->save();
    impl_->canvas->clipRect(bin2d::Rect(x, y, w, h));
    impl_->clipMarks.push_back(impl_->canvas->saveCount());
    return depth;
}

void BinUI2D::popClipTo(int depth) {
    if (!impl_->canvas) return;
    if (depth < 0) depth = 0;
    while (static_cast<int>(impl_->clipMarks.size()) > depth) {
        impl_->canvas->restore();
        impl_->clipMarks.pop_back();
    }
}

bool BinUI2D::pushLayer(float x, float y, float w, float h, float alpha) {
    ProfTimer _pt(impl_->prof, Prof::kPushLayer);
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (alpha <= 0.0f) return false;
    if (impl_->skipLayer) return false;   // BINUI2D_SKIP=l
    Impl::LayerRec rec;
    rec.offscreen = false;
    rec.alpha = alpha;
    rec.clipMark = impl_->clipMarks.size();
    if (!impl_->canvas->pushLayer(bin2d::Rect(x, y, w, h), alpha)) return false;
    impl_->layers.push_back(rec);
    return true;
}

bool BinUI2D::pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) {
    ProfTimer _pt(impl_->prof, Prof::kPushLayerEx);
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (fx.alpha <= 0.0f) return false;
    if (impl_->skipLayer) return false;   // BINUI2D_SKIP=l
    if (fx.shadowCount > 0 && !fx.shadows) return false;
    // 有 blur 或外阴影就必须走离屏：bin2d 的 pushLayer 只做"组 alpha 一次乘"，
    // 而模糊/阴影都是"pop 时对整层内容做的操作"。
    const bool needOffscreen = fx.blurPx > 0.0f || fx.shadowCount > 0;
    if (!needOffscreen) return pushLayer(x, y, w, h, fx.alpha);

    Impl::LayerRec rec;
    rec.offscreen = true;
    rec.alpha = fx.alpha;
    rec.blurPx = fx.blurPx;
    // 指针只在调用期间有效（编译器的 drop-shadow 段是另一份内存），必须拷进来
    if (fx.shadowCount > 0 && fx.shadows)
        rec.shadows.assign(fx.shadows, fx.shadows + fx.shadowCount);
    rec.clipMark = impl_->clipMarks.size();
    if (!impl_->canvas->pushOffscreen(bin2d::Rect(x, y, w, h))) {
        // GPU 执行体下 pushOffscreen 必然失败：那套 API 的出口是 Surface，而像素
        // 在设备上。**降级但不丢内容**：改成普通图层（组 alpha 一次乘），跳过模糊/外阴影。
        // 只告警一次 —— 这两类效果在每个卡片上都会出现，逐帧刷屏没有额外信息。
        static bool warned = false;
        if (!warned && impl_->canvas->gpuAttached()) {
            warned = true;
            std::fprintf(stderr,
                         "[binui2d] GPU 执行体下 pushOffscreen 不可用："
                         "图层模糊(blur %.1fpx)/外阴影(%d 条) 降级为纯组 alpha，"
                         "内容照常绘制\n",
                         static_cast<double>(fx.blurPx), fx.shadowCount);
        }
        rec.offscreen = false;
        rec.blurPx = 0.0f;
        rec.shadows.clear();
        if (!impl_->canvas->pushLayer(bin2d::Rect(x, y, w, h), fx.alpha)) return false;
        impl_->layers.push_back(rec);
        return true;
    }
    impl_->layers.push_back(rec);
    return true;
}

// 外阴影（0.27.1）：**轮廓的影子**，形状来自层内容自己的 alpha 通道。
// 算法与三份 GPU 后端同一套：
//   ① 铺一张纯色（预乘），② dst *= 层 alpha（得到 c·a 的上色轮廓），
//   ③ 按半径模糊，④ 按偏移（取整）合成到元素**下面**。
// 与 box-shadow 的本质区别就在这里：星形图标的投影是星形，不是它所在的方块。
void BinUI2D::popLayer() {
    ProfTimer _pt(impl_->prof, Prof::kPopLayer);
    if (!impl_->canvas || impl_->layers.empty()) return;
    const Impl::LayerRec rec = impl_->layers.back();
    impl_->layers.pop_back();

    // 层内漏掉的 popClip 不能把裁剪状态带到层外（bin2d 的 pushLayer 内部会截断，
    // 但 offscreen 路径自己要管这件事）
    while (impl_->clipMarks.size() > rec.clipMark) {
        impl_->canvas->restore();
        impl_->clipMarks.pop_back();
    }

    if (!rec.offscreen) {
        impl_->canvas->popLayer();
        return;
    }
    bin2d::Surface content;
    int ox = 0, oy = 0;
    if (!impl_->canvas->popOffscreen(content, ox, oy)) return;

    // ① 外阴影先铺（书写顺序里靠前的在上层 → 逆序画），本体最后画在最上面。
    //    阴影各自从**未模糊**的层内容取轮廓：模糊只作用于本体，不叠加到影子上。
    if (!rec.shadows.empty() && !content.empty()) {
        const int cw = content.width(), ch = content.height();
        for (int i = static_cast<int>(rec.shadows.size()) - 1; i >= 0; --i) {
            const BinUIFilterDropShadowEntry& sh = rec.shadows[static_cast<size_t>(i)];
            bin2d::Surface shadow(cw, ch);
            if (shadow.empty()) break;
            // 上色：out = premul(color) × 层 alpha —— 预乘空间里逐通道同乘，无需反预乘
            const bin2d::Color c = ToColor(sh.color);
            for (int y = 0; y < ch; ++y) {
                const uint8_t* s = content.row(y);
                uint8_t* d = shadow.row(y);
                for (int x = 0; x < cw; ++x, s += 4, d += 4) {
                    const uint32_t a = s[3];
                    if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                    d[0] = static_cast<uint8_t>(c.r * a / 255u);
                    d[1] = static_cast<uint8_t>(c.g * a / 255u);
                    d[2] = static_cast<uint8_t>(c.b * a / 255u);
                    d[3] = static_cast<uint8_t>(c.a * a / 255u);
                }
            }
            if (sh.blur > 0.0f) bin2d::BlurSigma(shadow, sh.blur);   // 阴影的 stdDeviation 也是 σ
            // 偏移取整：合成四边形恒落在像素边界上，与三份 GPU 后端同口径
            const int sdx = static_cast<int>(std::lround(sh.dx));
            const int sdy = static_cast<int>(std::lround(sh.dy));
            // 与三份 GPU 后端一致：阴影不乘组 alpha（f.alpha 只作用于本体）。
            // 这是一处**已知的口径**而不是笔误 —— 要改就四端一起改。
            impl_->canvas->compositeSurface(shadow, ox + sdx, oy + sdy, 1.0f,
                                            bin2d::BlendMode::SrcOver);
        }
    }

    if (rec.blurPx > 0.0f) bin2d::BlurSigma(content, rec.blurPx);   // filter: blur(Npx) → σ=N
    impl_->canvas->compositeSurface(content, ox, oy, rec.alpha,
                                    bin2d::BlendMode::SrcOver);
}

} // namespace binui
