// binui_font_renderer.h - 跨平台字体服务：FreeType 光栅化 + HarfBuzz 整形
// 与绘制后端无关：输出 8bit alpha（灰度）或预乘 BGRA（COLR 彩色）像素 +
// advance/基线度量，后端负责图集上传与绘制。
// 依赖：third_party/freetype + third_party/harfbuzz（均为静态库）。
// 字体通过文件路径注册；未注册的名字回退到默认 face。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace binui_font {

// 字体度量（像素，size 为 em 高）
struct FontMetrics {
    int ascent = 0;   // 基线以上高度
    int descent = 0;  // 基线以下深度（正值）
};

// 灰度字形：alpha 覆盖（白字），调用方按 alpha 写入颜色
struct GrayGlyph {
    int width = 0, height = 0; // 位图尺寸
    int pitch = 0;             // 行距（可能 > width）
    int left = 0, top = 0;     // 相对基线的偏移（top > 0 = 基线以上）
    int advance = 0;           // 前进宽（像素）
    const uint8_t* alpha = nullptr; // width*height，8bit（下次调用失效）
};

// 彩色字形：COLR 层合成结果（预乘 BGRA）
struct ColorGlyph {
    int width = 0, height = 0;
    int baseline = 0;          // 基线在缓冲内的 y 坐标（绘制：quadTop = baselineY - baseline）
    int advance = 0;           // 簇总前进宽（像素）
    const uint8_t* bgra = nullptr; // width*height*4（下次调用失效）
};

// HarfBuzz 整形输出（glyph 级）
struct ShapedGlyph {
    uint32_t gid = 0;      // 字体内 glyph id
    uint32_t cluster = 0;  // 输入码点索引（该 glyph 归属的文本簇起点）
    int xAdvance = 0;      // 前进宽（像素）
    int xOffset = 0;       // 横向偏移（像素，重排脚本用）
};

// 字体引擎：FT 字形缓存按 (face, sizePx) 建会话
class FontEngine {
public:
    FontEngine();
    ~FontEngine();
    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    // 初始化 FreeType/HarfBuzz；失败返回 false（调用方回退 GDI/DWrite）
    bool init();
    void shutdown();

    // 注册字体文件；名字重复时覆盖。未注册名回退到 default face
    bool addFont(const std::string& name, const std::string& path);
    void setDefaultFace(const std::string& name);
    const std::string& defaultFace() const;

    // 度量
    FontMetrics metrics(const std::string& face, float size);

    // 灰度渲染单个码点；false = 无字形（advance 置 0，调用方兜底）
    bool renderGlyph(const std::string& face, float size, uint32_t cp, GrayGlyph& out);
    // 同上，bold=true 时用 FreeType 的合成加粗（FT_GlyphSlot_Embolden）：
    // 加粗笔画并同步加宽 advance，所以"测宽"与"绘制"始终一致。
    bool renderGlyph(const std::string& face, float size, uint32_t cp, bool bold, GrayGlyph& out);

    // 彩色渲染整个簇（内部经 HarfBuzz 做 GSUB 合体/旗帜/肤色等）；
    // false = 该簇无 COLR 层（调用方降级灰度）
    bool renderColorCluster(const std::string& face, float size,
                            const uint32_t* cps, size_t n, ColorGlyph& out);

    // 该簇是否有彩色字形可用（shape 后首 glyph 有 COLR 层）
    bool hasColor(const std::string& face, float size, const uint32_t* cps, size_t n);

    // HarfBuzz 整形整段文本（复杂脚本/emoji 序列），返回 glyph 序列
    std::vector<ShapedGlyph> shape(const std::string& face, float size,
                                   const uint32_t* cps, size_t n);

    // 粗略脚本检测：序列是否含需要整形的复杂脚本（阿拉伯/希伯来/印度系/泰文…）
    bool needsShaping(const uint32_t* cps, size_t n);

private:
    struct Impl;
    Impl* impl_;
};

// 注册系统字体：
//   Windows  —— C:/Windows/Fonts 下的 Segoe UI / Segoe UI Emoji / 微软雅黑 / 黑体 / Arial
//   其他平台 —— 探测常见路径（Noto CJK / WenQuanYi / DejaVu / macOS PingFang·Helvetica）
// 两者都会把默认字形名 "Segoe UI" 别名到实际可用的字体（优先带中日韩字形），
// 上层代码不必关心平台。系统里一个字体都没有时是空操作，应用可自行 addFont()。
void registerSystemFonts(FontEngine& engine);

} // namespace binui_font
