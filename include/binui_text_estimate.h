// binui_text_estimate.h - 没有字体度量时的文本尺寸粗估（header-only，无第三方依赖）
//
// 为什么单独抽出来：同一条估算口径现在有两个使用方 ——
//   · 编译器（src/compiler/binui_compiler.cpp）：文本节点未声明 height 时估算行数撑开高度；
//   · 渲染器（src/renderer/binui_renderer.cpp）：运行期 setText 换长文本后重算高度
//     （节点在规格段里带 heightAuto 标记时）。
// 两边口径必须一致，否则"编译期能放下、运行期换文本就溢出"。
//
// 这不是度量：核心库不依赖 FreeType，拿不到真实 advance。口径是
//   · CJK / 全角 / emoji 按 1.0×字号，其余按 0.52×字号（拉丁小写平均宽度）
//   · 行距取 line-height（无单位倍数 / % / px），未声明按 1.35×字号
// 宁可估高一点：多一行总比把字压在下一个元素身上好。
#ifndef BINUI_TEXT_ESTIMATE_H
#define BINUI_TEXT_ESTIMATE_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace binui {

// 估算一段 UTF-8 文本在给定字号下的单行宽度（像素）。'\t' 按 2 个字宽。
inline float EstimateTextWidth(const std::string& text, float fontSize) {
    float w = 0.0f;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (len > 1 && i + len <= text.size()) {
            cp = (len == 2) ? static_cast<uint32_t>(c & 0x1F)
               : (len == 3) ? static_cast<uint32_t>(c & 0x0F)
                            : static_cast<uint32_t>(c & 0x07);
            for (size_t k = 1; k < len; k++)
                cp = (cp << 6) | static_cast<uint32_t>(static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        i += len;
        if (cp == '\t') { w += fontSize * 2.0f; continue; }
        const bool wide = cp >= 0x1100 &&
                          (cp <= 0x115F || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF));
        w += wide ? fontSize : fontSize * 0.52f;
    }
    return w;
}

// 行距（像素）。lineHeightPct：0 = 未声明（按 1.35×字号），否则为百分数（150 = 1.5 倍）。
// 编译期从 CSS line-height 解析成百分数写进 BinUIPaintNode::lineHeight，运行期直接用它。
inline float EstimateLineStep(float fontSize, int lineHeightPct) {
    if (lineHeightPct < 50 || lineHeightPct > 400) return fontSize * 1.35f;
    return fontSize * static_cast<float>(lineHeightPct) / 100.0f;
}

// 给定可用宽度 avail 时文本要占几行（至少 1 行；'\n' 硬断行）。
inline int EstimateLineCount(const std::string& text, float fontSize, float avail) {
    if (avail < 8.0f) avail = 8.0f;
    size_t lines = 0;
    size_t p0 = 0;
    while (true) {
        const size_t nl = text.find('\n', p0);
        const std::string seg = text.substr(p0, (nl == std::string::npos) ? std::string::npos : nl - p0);
        lines += static_cast<size_t>(
            std::max(1.0f, std::ceil(EstimateTextWidth(seg, fontSize) / avail)));
        if (nl == std::string::npos) break;
        p0 = nl + 1;
    }
    if (lines < 1) lines = 1;
    return static_cast<int>(lines);
}

// 文本块应有的高度：行数 × 行距。
inline float EstimateTextHeight(const std::string& text, float fontSize, float avail,
                                int lineHeightPct) {
    return static_cast<float>(EstimateLineCount(text, fontSize, avail)) *
           EstimateLineStep(fontSize, lineHeightPct);
}

} // namespace binui

#endif // BINUI_TEXT_ESTIMATE_H
