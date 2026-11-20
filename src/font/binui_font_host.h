#pragma once
// ============================================================
// binui_font_host.h —— 平台字体发现（外壳侧）
//
// 分工（见 docs/issues/12-font-layer-ownership.md）：
//   · 字形光栅化 / 整形 / 图集打包在 bin2d 的可选文字模块（libbin2d_text）；
//   · **字体枚举与文件位置**是平台知识，留在 binUI 外壳：模块只定义
//     `bin2d::text::FaceResolver`（家族名 → 字体文件路径）这个注入契约。
//
// 于是 bin2d 那一侧一行平台 API 都没有（它的 `make layers` 守着这条），
// 而 Windows 的 C:/Windows/Fonts、Linux 的 /usr/share/fonts、macOS 的
// /System/Library/Fonts 这些路径都只出现在这份文件里。
// ============================================================

namespace bin2d {
namespace text {
class Library;
}
} // namespace bin2d

namespace binui {

// 把本机可用的系统字体注入 Library：装 Resolver、设默认 face 与回退链。
// 一个字体都没有时是空操作（模块会把文字画成空白，不致命）。
void RegisterSystemFonts(bin2d::text::Library& lib);

} // namespace binui
