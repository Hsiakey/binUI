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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/png.h"
#include "bin2d/raster.h"
#include "bin2d/surface.h"
#include "bin2d/transform.h"

#include "binui_format.h"
#include "binui_gfx.h"
#include "binui_glyph_atlas.h"
#include "binui_paint.h"
#include "binui_win_image.h"

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

} // namespace

// ==================================================================
// 字形宿主：把"位图 → 纹理"这一步换成"位图 → bin2d::Image"
//
// GL 版的宿主是 glTexSubImage2D，VK 版是 staging buffer + copy，这里就是
// 往一张 Surface 里写。三种实现共用同一份排版/光栅/缓存（binui_glyph_atlas.h），
// 所以字形位图逐字节相同 —— 差异只可能来自后面的采样（见 drawText 的注释）。
// ==================================================================
struct GlyphHost2D : public IGlyphTexHost {
    std::vector<std::unique_ptr<bin2d::Image>> owned;

    void* createTex(int w, int h) override {
        if (w <= 0 || h <= 0) return nullptr;
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
        auto* img = static_cast<bin2d::Image*>(tex);
        BlitBgraStraightInto(img->surface(), x, y, w, h, bgra, w * 4);
    }

    void destroyTex(void* tex) override {
        if (!tex) return;
        for (size_t i = 0; i < owned.size(); ++i) {
            if (owned[i].get() == tex) {
                owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    void clear() { owned.clear(); }
};

// ==================================================================

struct BinUI2D::Impl {
    int width = 0;
    int height = 0;
    bool ready = false;

    bin2d::Surface surface;
    std::unique_ptr<bin2d::Canvas> canvas;

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
        size_t clipMark = 0;   // 入层时的 clipMarks.size()，出层要截断
    };
    std::vector<LayerRec> layers;

    // ---- 图片 ----
    struct ImageEntry {
        std::unique_ptr<bin2d::Image> img;
        uint32_t generation = 0xFFFFFFFFu;
    };
    std::unordered_map<uint32_t, ImageEntry> images;

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

    bin2d::Image* findImage(uint16_t index) {
        auto it = images.find(index);
        if (it == images.end() || !it->second.img || it->second.img->empty()) return nullptr;
        return it->second.img.get();
    }
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
    impl_->width = width;
    impl_->height = height;
    if (!impl_->surface.resize(width, height)) return false;
    impl_->canvas = std::make_unique<bin2d::Canvas>(impl_->surface);
    impl_->ready = true;
    // 字形图集与 Canvas 无关，init 之后一直活着（换图集代数由 atlas 自己管）
    if (!impl_->atlas) {
        impl_->atlas = std::make_unique<GlyphAtlas>();
        impl_->atlas->setHost(&impl_->host);
        impl_->atlas->initFonts();
    }
    return true;
}

bool BinUI2D::init(void* /*hwnd*/, int width, int height, bool vsync) {
    impl_->vsync = vsync;
    return init(width, height);
}

void BinUI2D::shutdown() {
    impl_->canvas.reset();
    impl_->images.clear();
    impl_->clipMarks.clear();
    impl_->layers.clear();
    impl_->surface = bin2d::Surface();
    impl_->ready = false;
    impl_->width = impl_->height = 0;
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
    impl_->clipMarks.clear();
    impl_->layers.clear();
}

void BinUI2D::setVsync(bool on) { impl_->vsync = on; }
void BinUI2D::setTransparent(bool t) { impl_->transparent = t; }

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
    // 每帧重建 Canvas：Canvas 构造绑定目标 Surface，而 Surface 只 resize 一次。
    // 重建的代价是一次 State 拷贝（几百字节），换来的是"帧与帧之间状态绝对不串"
    // —— 上一帧漏掉的 restore 不会把这一帧的裁剪带歪（那类 bug 极难复现）。
    impl_->canvas = std::make_unique<bin2d::Canvas>(impl_->surface);
    impl_->clipMarks.clear();
    impl_->layers.clear();

    if (clipRect) {
        const int* r = static_cast<const int*>(clipRect);
        const bin2d::Rect area(static_cast<float>(r[0]), static_cast<float>(r[1]),
                               static_cast<float>(r[2] - r[0]),
                               static_cast<float>(r[3] - r[1]));
        impl_->surface.clearRect(area);
        impl_->canvas->clipRect(area);
    }
    impl_->ApplyState();
    if (impl_->atlas) impl_->atlas->snapText = !impl_->subpixelText;
}

void BinUI2D::present() {
    // CPU 路径没有呈现步：像素已经在 surface 里。
    // 窗口呈现（Surface → bin2d::GpuCompositor → binGPU 交换链）是下一步。
}

// ------------------------------------------------------------------
// 几何
// ------------------------------------------------------------------
void BinUI2D::fillRect(float x, float y, float w, float h, uint32_t color) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    impl_->canvas->fillRect(bin2d::Rect(x, y, w, h), ToColor(color));
}

void BinUI2D::fillRounded(float x, float y, float w, float h, float radius,
                          uint32_t color) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    // 半径 <= 0 = 直角（三端统一的回退语义，见 binui_gfx.h）
    if (radius <= 0.0f) {
        impl_->canvas->fillRect(bin2d::Rect(x, y, w, h), ToColor(color));
        return;
    }
    impl_->canvas->fillRounded(bin2d::Rect(x, y, w, h), radius, ToColor(color));
}

void BinUI2D::strokeRounded(float x, float y, float w, float h, float radius,
                            float thickness, uint32_t color) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    if (thickness <= 0.0f) return;
    if (radius <= 0.0f) {
        // 直角框：四条矩形边的极限情况由下方的一般路径自然覆盖（半径会被夹到 0），
        // 这里只处理"退化到 0 半径"的入口，避免 addRoundedRect 收到 0 半径。
        const float t = thickness;
        impl_->canvas->fillRect(bin2d::Rect(x, y, w, t), ToColor(color));
        impl_->canvas->fillRect(bin2d::Rect(x, y + h - t, w, t), ToColor(color));
        impl_->canvas->fillRect(bin2d::Rect(x, y + t, t, h - 2.0f * t), ToColor(color));
        impl_->canvas->fillRect(bin2d::Rect(x + w - t, y + t, t, h - 2.0f * t),
                                ToColor(color));
        return;
    }
    // binUI 的描边是**向内**的同心层（每层外框缩 1px、半径减 1，见 binui_paint.h 的
    // BuildRoundedLayer），等价于"内缩 thickness/2 的矩形 + 居中描边 thickness"。
    // 这条等价关系是逐字节验过的（像素门禁），别改成"外扩"。
    const float t = thickness;
    const float in = t * 0.5f;
    const bin2d::Rect outer(x + in, y + in, w - t, h - t);
    if (outer.w <= 0.0f || outer.h <= 0.0f) return;
    const float r = std::min(radius, std::min(w, h) * 0.5f) - in;
    if (r <= 0.0f) {
        impl_->canvas->fillRect(outer, ToColor(color));
        return;
    }
    bin2d::Path path;
    path.addRoundedRect(outer, r);
    bin2d::StrokeStyle style;
    style.width = t;
    style.roundCap = false;
    style.squareCap = false;
    style.roundJoin = true;
    impl_->canvas->strokePath(path, bin2d::Paint::solid(ToColor(color)), style);
}

void BinUI2D::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                            uint32_t color, const float xf[6]) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f || !xf) return;
    // fillRoundedXf 自带矩阵，**不受** setAffine 状态影响（见 binui_gfx.h 的注释：
    // 否则圆角底会被变换两次）。所以这里换掉当前变换、画完再还回去。
    const bin2d::Transform saved = impl_->canvas->transform();
    impl_->canvas->setTransform(ToTransform(xf));
    if (stroke) {
        // 描边：局部矩形是 (0,0,w,h)，内缩语义与 strokeRounded 一致
        const float t = thickness;
        const float in = t * 0.5f;
        const bin2d::Rect outer(in, in, w - t, h - t);
        if (outer.w > 0.0f && outer.h > 0.0f) {
            const float r = std::min(radius, std::min(w, h) * 0.5f) - in;
            if (r <= 0.0f) {
                impl_->canvas->fillRect(outer, ToColor(color));
            } else {
                bin2d::Path path;
                path.addRoundedRect(outer, r);
                bin2d::StrokeStyle style;
                style.width = t;
                style.roundJoin = true;
                impl_->canvas->strokePath(path, bin2d::Paint::solid(ToColor(color)), style);
            }
        }
    } else if (radius <= 0.0f) {
        impl_->canvas->fillRect(bin2d::Rect(0.0f, 0.0f, w, h), ToColor(color));
    } else {
        impl_->canvas->fillRounded(bin2d::Rect(0.0f, 0.0f, w, h), radius, ToColor(color));
    }
    impl_->canvas->setTransform(saved);
}

void BinUI2D::fillLine(float x0, float y0, float x1, float y1, float width,
                       uint32_t color) {
    if (!impl_->canvas || width <= 0.0f) return;
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
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
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
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    bin2d::Image* img = impl_->findImage(index);
    if (!img) return;
    impl_->canvas->drawImage(*img, bin2d::Rect(x, y, w, h),
                             bin2d::Rect(u0, v0, u1 - u0, v1 - v0), ToColor(tint),
                             bin2d::ImageFilter::Linear);
}

void BinUI2D::drawImageNine(uint16_t index, float x, float y, float w, float h,
                            float slice, int repX, int repY, uint32_t tint) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return;
    bin2d::Image* img = impl_->findImage(index);
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
    if (!impl_->canvas || utf8.empty() || !impl_->atlas) return;
    impl_->atlas->drawText(x, y, size, weight, utf8, color, centerH, centerV, italic,
                           [this](const GlyphQuad& g) {
                               auto* img = static_cast<bin2d::Image*>(g.tex);
                               if (!img || img->empty()) return;
                               impl_->canvas->drawImage(
                                   *img, bin2d::Rect(g.x, g.y, g.w, g.h),
                                   bin2d::Rect(g.u0, g.v0, g.u1 - g.u0, g.v1 - g.v0),
                                   ToColor(g.color), bin2d::ImageFilter::Linear);
                           });
}

void BinUI2D::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                              const std::string& utf8, uint32_t color, int align,
                              bool italic) {
    if (!impl_->canvas || utf8.empty() || !impl_->atlas) return;
    impl_->atlas->drawTextWrapped(x, y, maxW, size, weight, utf8, color, align, italic,
                                  [this](const GlyphQuad& g) {
                                      auto* img = static_cast<bin2d::Image*>(g.tex);
                                      if (!img || img->empty()) return;
                                      impl_->canvas->drawImage(
                                          *img, bin2d::Rect(g.x, g.y, g.w, g.h),
                                          bin2d::Rect(g.u0, g.v0, g.u1 - g.u0, g.v1 - g.v0),
                                          ToColor(g.color), bin2d::ImageFilter::Linear);
                                  });
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

void BinUI2D::setImageFilter(const BinUIImageFilter* /*f*/) {
    // 未实现（P4 遗留）：bin2d 有 ColorMatrix（filter.h），但"按 (图, 矩阵) 分键缓存"
    // 还没接。像素用例 svg-filter 会因此与 GL 不同 —— 门禁里已经把它单列出来。
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
                           float blurPx, float /*grain*/) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (blurPx <= 0.0f) return false;

    const bin2d::Rect box(x, y, w, h);
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
    bin2d::Blur(bg, blurPx);

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
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (alpha <= 0.0f) return false;
    Impl::LayerRec rec;
    rec.offscreen = false;
    rec.alpha = alpha;
    rec.clipMark = impl_->clipMarks.size();
    if (!impl_->canvas->pushLayer(bin2d::Rect(x, y, w, h), alpha)) return false;
    impl_->layers.push_back(rec);
    return true;
}

bool BinUI2D::pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) {
    if (!impl_->canvas || w <= 0.0f || h <= 0.0f) return false;
    if (fx.alpha <= 0.0f) return false;
    // 有 blur 就必须走离屏（bin2d 的 pushLayer 不支持"pop 时模糊"）；
    // drop-shadow 同理，但本版本还没实现（见 popLayer 的注释）。
    const bool needOffscreen = fx.blurPx > 0.0f || fx.shadowCount > 0;
    if (!needOffscreen) return pushLayer(x, y, w, h, fx.alpha);

    Impl::LayerRec rec;
    rec.offscreen = true;
    rec.alpha = fx.alpha;
    rec.blurPx = fx.blurPx;
    rec.clipMark = impl_->clipMarks.size();
    if (!impl_->canvas->pushOffscreen(bin2d::Rect(x, y, w, h))) return false;
    impl_->layers.push_back(rec);
    return true;
}

void BinUI2D::popLayer() {
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
    if (rec.blurPx > 0.0f) bin2d::Blur(content, rec.blurPx);
    // drop-shadow 未实现：LayerFx::shadows 目前被忽略（migrate 文档已记）
    impl_->canvas->compositeSurface(content, ox, oy, rec.alpha,
                                    bin2d::BlendMode::SrcOver);
}

} // namespace binui
