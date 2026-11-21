// binui_compiler.cpp - UI 源码 -> binUI (.xhtml) 二进制布局编译器
#include "binui_compiler.h"
#include "binui_asset.h"
#include "binui_format.h"
#include "binui_link.h"
#include "binui_srcmap.h"
#include "binui_path.h"            // SVG 路径几何：d 解析 / 细分 / 包围盒（与运行期同一份数学）
#include "binui_text_estimate.h"   // 文本尺寸粗估：与渲染器运行期 setText 重算共用同一口径
#include "binui_flex_layout.h"     // flex 布局：与渲染器运行期重排共用同一份实现
#include "binui_filter.h"          // CSS filter 的颜色矩阵：编译期合成，后端解码时逐像素应用
#include "binui_paint.h"           // kMaxFilterBlurPx / PaintFilterBlurPad：模糊半径与图层外扩口径

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace binui {

using namespace binui;

// 将码点编码为 UTF-8 追加到 out
static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// HTML/XML 实体解码
//   数值实体（&#NNN; / &#xHH;）+ 常用命名实体表（排版/数学/箭头/货币/希腊/符号，约 120 个）
//   未知实体原样保留（不吞字符，便于发现拼写错误）
static const std::pair<const char*, uint32_t> kNamedEntities[] = {
    // 基本
    {"amp", 0x0026}, {"lt", 0x003C}, {"gt", 0x003E}, {"quot", 0x0022}, {"apos", 0x0027},
    {"nbsp", 0x00A0}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009},
    {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F},
    {"shy", 0x00AD},
    // 排版
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"bull", 0x2022},
    {"middot", 0x00B7}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"laquo", 0x00AB}, {"raquo", 0x00BB}, {"prime", 0x2032},
    {"Prime", 0x2033}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"permil", 0x2030},
    {"sect", 0x00A7}, {"para", 0x00B6}, {"copy", 0x00A9}, {"reg", 0x00AE},
    {"trade", 0x2122}, {"deg", 0x00B0},
    // 数学
    {"plusmn", 0x00B1}, {"times", 0x00D7}, {"divide", 0x00F7}, {"minus", 0x2212},
    {"micro", 0x00B5}, {"not", 0x00AC}, {"le", 0x2264}, {"ge", 0x2265},
    {"ne", 0x2260}, {"asymp", 0x2248}, {"equiv", 0x2261}, {"infin", 0x221E},
    {"radic", 0x221A}, {"sum", 0x2211}, {"prod", 0x220F}, {"part", 0x2202},
    {"isin", 0x2208}, {"forall", 0x2200}, {"exist", 0x2203}, {"empty", 0x2205},
    {"cap", 0x2229}, {"cup", 0x222A}, {"sub", 0x2282}, {"sup", 0x2283},
    {"ne", 0x2260}, {"lowast", 0x2217},
    // 分数 / 上标
    {"frac12", 0x00BD}, {"frac14", 0x00BC}, {"frac34", 0x00BE},
    {"sup1", 0x00B9}, {"sup2", 0x00B2}, {"sup3", 0x00B3},
    {"ordm", 0x00BA}, {"ordf", 0x00AA},
    // 箭头
    {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
    {"harr", 0x2194}, {"lArr", 0x21D0}, {"rArr", 0x21D2}, {"hArr", 0x21D4},
    // 货币
    {"euro", 0x20AC}, {"pound", 0x00A3}, {"yen", 0x00A5}, {"cent", 0x00A2},
    {"curren", 0x00A4},
    // 希腊
    {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3}, {"delta", 0x03B4},
    {"epsilon", 0x03B5}, {"zeta", 0x03B6}, {"eta", 0x03B7}, {"theta", 0x03B8},
    {"iota", 0x03B9}, {"kappa", 0x03BA}, {"lambda", 0x03BB}, {"mu", 0x03BC},
    {"nu", 0x03BD}, {"xi", 0x03BE}, {"omicron", 0x03BF}, {"pi", 0x03C0},
    {"rho", 0x03C1}, {"sigmaf", 0x03C2}, {"sigma", 0x03C3}, {"tau", 0x03C4},
    {"upsilon", 0x03C5}, {"phi", 0x03C6}, {"chi", 0x03C7}, {"psi", 0x03C8},
    {"omega", 0x03C9}, {"Gamma", 0x0393}, {"Delta", 0x0394}, {"Theta", 0x0398},
    {"Lambda", 0x039B}, {"Xi", 0x039E}, {"Pi", 0x03A0}, {"Sigma", 0x03A3},
    {"Phi", 0x03A6}, {"Psi", 0x03A8}, {"Omega", 0x03A9},
    // 符号
    {"hearts", 0x2665}, {"spades", 0x2660}, {"clubs", 0x2663}, {"diams", 0x2666},
    {"loz", 0x25CA}, {"check", 0x2713}, {"cross", 0x2717}, {"star", 0x2606},
    {"starf", 0x2605}, {"sung", 0x266A},
    // 拉丁重音 / 符号
    {"iexcl", 0x00A1}, {"iquest", 0x00BF}, {"szlig", 0x00DF}, {"acute", 0x00B4},
    {"uml", 0x00A8}, {"macr", 0x00AF}, {"cedil", 0x00B8}, {"tilde", 0x02DC},
    {"circ", 0x02C6}, {"aacute", 0x00E1}, {"agrave", 0x00E0}, {"eacute", 0x00E9},
    {"egrave", 0x00E8}, {"iacute", 0x00ED}, {"oacute", 0x00F3}, {"uacute", 0x00FA},
    {"ntilde", 0x00F1}, {"ccedil", 0x00E7}, {"OElig", 0x0152}, {"oelig", 0x0153},
};

static std::string DecodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) { out += '&'; continue; }

        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent.empty()) { out += '&'; continue; }

        // 数值实体：&#NNN; / &#xHH;
        if (ent[0] == '#') {
            const bool isHex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            long code = 0;
            try {
                code = isHex ? std::stol(ent.substr(2), nullptr, 16)
                             : std::stol(ent.substr(1), nullptr, 10);
            } catch (...) { out += '&'; continue; }
            if (code <= 0 || code > 0x10FFFF) { out += '&'; continue; }
            AppendUtf8(out, (uint32_t)code);
            i = semi;
            continue;
        }

        // 命名实体：查表（未命中保留原样）
        bool found = false;
        for (const auto& e : kNamedEntities) {
            if (ent == e.first) {
                AppendUtf8(out, e.second);
                found = true;
                break;
            }
        }
        if (found) { i = semi; continue; }
        out += '&';
    }
    return out;
}

// 解析 CSS 长度：数字/百分比（pct=true 表示相对父容器内容盒；无单位数字视为像素）
//
// ============================================================
// 编译期诊断：警告去重 + 收集进 CompileResult::warnings
// 用 thread_local 保存"当前编译会话"，避免给公开的 CompileXML/CompileHTML 改签名。
// ============================================================
struct CompileDiag {
    std::vector<std::string>* list = nullptr;
    std::unordered_set<std::string>* seen = nullptr;
};

static thread_local CompileDiag g_diag;

// 当前 UI 文件所在目录（组件内联时的相对路径基准）；由 CompileBinUIFileEx 设置，
// 字符串入口（CompileBinUIEx）为空 → <use src> 按工作目录解析。
static thread_local std::string tl_uiBaseDir;

// 主 UI 的文件名（不含目录）。打包清单里第 0 条就是它 —— 字符串入口没有文件名，
// 用 main.bui.html 兜底。
static thread_local std::string tl_uiSourceName;
// 主 UI 的绝对路径（有文件入口时才有）；没有就是空，打包清单会退回 CWD/main.bui.html。
static thread_local std::string tl_uiSourceAbs;

// 打包清单收集器（.bui.srcmap）。非空 = 本次编译正在收集：
// 主 UI 与每个被内联/外挂的组件的**原始字节** + 绝对路径（最后统一折算成
// "相对所有文件共同祖先"的相对路径，见 include/binui_srcmap.h）。
static thread_local std::vector<SrcMapEntry>* tl_srcMap = nullptr;
static thread_local std::unordered_set<std::string>* tl_srcMapSeen = nullptr;

// 已收集过就跳过（同一个组件被 use 多次只记一份）。absPath 先按原样存。
static void SrcMapCollect(const std::string& absPath, std::vector<uint8_t> bytes) {
    if (!tl_srcMap || !tl_srcMapSeen) return;
    if (absPath.empty()) return;
    if (!tl_srcMapSeen->insert(absPath).second) return;
    SrcMapEntry e;
    e.path = absPath;
    e.data = std::move(bytes);
    tl_srcMap->push_back(std::move(e));
}

static bool IsPathPrefix(const std::filesystem::path& prefix, const std::filesystem::path& p) {
    auto pi = prefix.begin();
    auto qi = p.begin();
    for (; pi != prefix.end(); ++pi, ++qi) {
        if (qi == p.end()) return false;
        if (*pi != *qi) return false;
    }
    return true;
}

// 把所有条目路径折算成"相对它们共同祖先目录"的相对路径 —— 也就是打包时的 appRootDir。
// 主 UI 在 examples/features/、组件用 ../../components/x 引用时，共同祖先是仓库根，
// 折算结果就是 examples/features/main.html + components/x.bum.html，解包即还原原树。
// 全都是同级文件时，折算结果就是纯文件名。
static void NormalizeSrcMapPaths(std::vector<SrcMapEntry>& entries) {
    if (entries.empty()) return;
    std::vector<std::filesystem::path> abs;
    abs.reserve(entries.size());
    for (const auto& e : entries) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::weakly_canonical(
            std::filesystem::absolute(e.path, ec), ec);
        if (ec || p.empty()) p = std::filesystem::absolute(e.path);
        abs.push_back(p);
    }
    std::filesystem::path common = abs[0].parent_path();
    for (const auto& p : abs) {
        while (!common.empty() && !IsPathPrefix(common, p))
            common = common.parent_path();
        if (common.empty()) break;
    }
    for (size_t i = 0; i < entries.size(); i++) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(abs[i], common, ec);
        if (ec || rel.empty()) rel = abs[i].filename();
        entries[i].path = rel.generic_string();
    }
}

// 同一条警告只报一次（同一份 UI 里 20 个元素写错单位，不该刷 20 行）。
// 装了收集器（CompileResult::warnings）就只入列表、不再直写 stderr ——
// 以前无条件两者都做，命令行工具于是把每条警告打印了两遍。
static void Warn(const std::string& msg) {
    if (g_diag.seen && !g_diag.seen->insert(msg).second) return;
    if (g_diag.list) {
        g_diag.list->push_back(msg);
        return;
    }
    std::fprintf(stderr, "[binui] warning: %s\n", msg.c_str());
}

struct DiagGuard {
    CompileDiag prev;
    DiagGuard(std::vector<std::string>* list, std::unordered_set<std::string>* seen) {
        prev = g_diag;
        g_diag.list = list;
        g_diag.seen = seen;
    }
    ~DiagGuard() { g_diag = prev; }
    DiagGuard(const DiagGuard&) = delete;
    DiagGuard& operator=(const DiagGuard&) = delete;
};

static std::string TrimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string LowerCopy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

// ============================================================
// CSS 长度解析
//   支持：px / 无单位（= px）/ % / em（相对元素 font-size）/ rem（相对根字号）
//         / vw / vh（相对编译期视口；不随运行时 resize 重排）
//   不支持：calc()/min()/max()/clamp()、pt/cm/mm/in/ex/ch/vmin/vmax/q 等
//           -> 明确警告 + 回退调用方给的默认值（绝不静默当成 px）
// ============================================================
struct LenPct { float value = 0; bool pct = false; bool ok = false; };

struct LenCtx {
    float fontSize = 14.0f;    // em 基准（该节点生效的字号）
    float rootFont = 14.0f;    // rem 基准（根字号）
    float viewW    = 1280.0f;  // vw 基准（编译期视口宽）
    float viewH    = 720.0f;   // vh 基准（编译期视口高）
};

static LenPct ParseLenPct(const std::string& raw, float def,
                          const LenCtx& ctx = LenCtx(), const char* what = "长度") {
    LenPct r;
    r.value = def;
    const std::string s = TrimCopy(raw);
    if (s.empty()) return r;

    if (s.find('(') != std::string::npos) {
        Warn(std::string(what) + "：不支持函数值 \"" + s + "\"（calc/min/max/clamp），已回退默认值");
        return r;
    }

    size_t p = 0;
    if (s[p] == '+' || s[p] == '-') p++;
    size_t digits = 0;
    while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '.')) { p++; digits++; }
    if (digits == 0) {
        // width:content 是"按文字宽度排"的 flex 关键字：flex 布局那一趟会算出来，
        // 这里的通用解析直接当"没声明宽度"处理，别刷警告。
        std::string low;
        for (char c : s) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (low != "content" && low != "auto")
            Warn(std::string(what) + "：无法解析长度值 \"" + s + "\"，已回退默认值");
        return r;
    }

    double num = 0;
    try {
        num = std::stod(s.substr(0, p));
    } catch (...) {
        Warn(std::string(what) + "：无法解析长度值 \"" + s + "\"，已回退默认值");
        return r;
    }

    const std::string unit = LowerCopy(TrimCopy(s.substr(p)));
    if (unit.empty() || unit == "px") { r.value = (float)num; r.ok = true; return r; }
    if (unit == "%")   { r.value = (float)num; r.pct = true; r.ok = true; return r; }
    if (unit == "em")  { r.value = (float)num * ctx.fontSize; r.ok = true; return r; }
    if (unit == "rem") { r.value = (float)num * ctx.rootFont; r.ok = true; return r; }
    if (unit == "vw")  { r.value = (float)num * ctx.viewW / 100.0f; r.ok = true; return r; }
    if (unit == "vh")  { r.value = (float)num * ctx.viewH / 100.0f; r.ok = true; return r; }

    Warn(std::string(what) + "：不支持单位 \"" + unit + "\"（\"" + s + "\"），已回退默认值");
    return r;
}

// ============================================================
// CSS 颜色解析
//   支持：CSS 命名色（含 transparent）、#RGB / #ARGB / #RRGGBB / #AARRGGBB、
//         rgb()/rgba()（逗号或空格分隔、可带 "/ alpha"、通道可写百分比）、hsl()/hsla()
//   注意：binUI 的 8 位十六进制是 **#AARRGGBB**（不是 CSS 的 #RRGGBBAA），
//         首次使用时警告一次，避免 CSS 习惯踩坑。
//   无法识别 -> 警告 + 返回 false（调用方保留自己的默认色）
// ============================================================
struct NamedColorEntry { const char* name; uint32_t argb; };

static const NamedColorEntry kNamedColors[] = {
    {"aliceblue",0xFFF0F8FFu},{"antiquewhite",0xFFFAEBD7u},{"aqua",0xFF00FFFFu},
    {"aquamarine",0xFF7FFFD4u},{"azure",0xFFF0FFFFu},{"beige",0xFFF5F5DCu},
    {"bisque",0xFFFFE4C4u},{"black",0xFF000000u},{"blanchedalmond",0xFFFFEBCDu},
    {"blue",0xFF0000FFu},{"blueviolet",0xFF8A2BE2u},{"brown",0xFFA52A2Au},
    {"burlywood",0xFFDEB887u},{"cadetblue",0xFF5F9EA0u},{"chartreuse",0xFF7FFF00u},
    {"chocolate",0xFFD2691Eu},{"coral",0xFFFF7F50u},{"cornflowerblue",0xFF6495EDu},
    {"cornsilk",0xFFFFF8DCu},{"crimson",0xFFDC143Cu},{"cyan",0xFF00FFFFu},
    {"darkblue",0xFF00008Bu},{"darkcyan",0xFF008B8Bu},{"darkgoldenrod",0xFFB8860Bu},
    {"darkgray",0xFFA9A9A9u},{"darkgreen",0xFF006400u},{"darkgrey",0xFFA9A9A9u},
    {"darkkhaki",0xFFBDB76Bu},{"darkmagenta",0xFF8B008Bu},{"darkolivegreen",0xFF556B2Fu},
    {"darkorange",0xFFFF8C00u},{"darkorchid",0xFF9932CCu},{"darkred",0xFF8B0000u},
    {"darksalmon",0xFFE9967Au},{"darkseagreen",0xFF8FBC8Fu},{"darkslateblue",0xFF483D8Bu},
    {"darkslategray",0xFF2F4F4Fu},{"darkslategrey",0xFF2F4F4Fu},{"darkturquoise",0xFF00CED1u},
    {"darkviolet",0xFF9400D3u},{"deeppink",0xFFFF1493u},{"deepskyblue",0xFF00BFFFu},
    {"dimgray",0xFF696969u},{"dimgrey",0xFF696969u},{"dodgerblue",0xFF1E90FFu},
    {"firebrick",0xFFB22222u},{"floralwhite",0xFFFFFAF0u},{"forestgreen",0xFF228B22u},
    {"fuchsia",0xFFFF00FFu},{"gainsboro",0xFFDCDCDCu},{"ghostwhite",0xFFF8F8FFu},
    {"gold",0xFFFFD700u},{"goldenrod",0xFFDAA520u},{"gray",0xFF808080u},
    {"green",0xFF008000u},{"greenyellow",0xFFADFF2Fu},{"grey",0xFF808080u},
    {"honeydew",0xFFF0FFF0u},{"hotpink",0xFFFF69B4u},{"indianred",0xFFCD5C5Cu},
    {"indigo",0xFF4B0082u},{"ivory",0xFFFFFFF0u},{"khaki",0xFFF0E68Cu},
    {"lavender",0xFFE6E6FAu},{"lavenderblush",0xFFFFF0F5u},{"lawngreen",0xFF7CFC00u},
    {"lemonchiffon",0xFFFFFACDu},{"lightblue",0xFFADD8E6u},{"lightcoral",0xFFF08080u},
    {"lightcyan",0xFFE0FFFFu},{"lightgoldenrodyellow",0xFFFAFAD2u},{"lightgray",0xFFD3D3D3u},
    {"lightgreen",0xFF90EE90u},{"lightgrey",0xFFD3D3D3u},{"lightpink",0xFFFFB6C1u},
    {"lightsalmon",0xFFFFA07Au},{"lightseagreen",0xFF20B2AAu},{"lightskyblue",0xFF87CEFAu},
    {"lightslategray",0xFF778899u},{"lightslategrey",0xFF778899u},{"lightsteelblue",0xFFB0C4DEu},
    {"lightyellow",0xFFFFFFE0u},{"lime",0xFF00FF00u},{"limegreen",0xFF32CD32u},
    {"linen",0xFFFAF0E6u},{"magenta",0xFFFF00FFu},{"maroon",0xFF800000u},
    {"mediumaquamarine",0xFF66CDAAu},{"mediumblue",0xFF0000CDu},{"mediumorchid",0xFFBA55D3u},
    {"mediumpurple",0xFF9370DBu},{"mediumseagreen",0xFF3CB371u},{"mediumslateblue",0xFF7B68EEu},
    {"mediumspringgreen",0xFF00FA9Au},{"mediumturquoise",0xFF48D1CCu},{"mediumvioletred",0xFFC71585u},
    {"midnightblue",0xFF191970u},{"mintcream",0xFFF5FFFAu},{"mistyrose",0xFFFFE4E1u},
    {"moccasin",0xFFFFE4B5u},{"navajowhite",0xFFFFDEADu},{"navy",0xFF000080u},
    {"oldlace",0xFFFDF5E6u},{"olive",0xFF808000u},{"olivedrab",0xFF6B8E23u},
    {"orange",0xFFFFA500u},{"orangered",0xFFFF4500u},{"orchid",0xFFDA70D6u},
    {"palegoldenrod",0xFFEEE8AAu},{"palegreen",0xFF98FB98u},{"paleturquoise",0xFFAFEEEEu},
    {"palevioletred",0xFFDB7093u},{"papayawhip",0xFFFFEFD5u},{"peachpuff",0xFFFFDAB9u},
    {"peru",0xFFCD853Fu},{"pink",0xFFFFC0CBu},{"plum",0xFFDDA0DDu},
    {"powderblue",0xFFB0E0E6u},{"purple",0xFF800080u},{"rebeccapurple",0xFF663399u},
    {"red",0xFFFF0000u},{"rosybrown",0xFFBC8F8Fu},{"royalblue",0xFF4169E1u},
    {"saddlebrown",0xFF8B4513u},{"salmon",0xFFFA8072u},{"sandybrown",0xFFF4A460u},
    {"seagreen",0xFF2E8B57u},{"seashell",0xFFFFF5EEu},{"sienna",0xFFA0522Du},
    {"silver",0xFFC0C0C0u},{"skyblue",0xFF87CEEBu},{"slateblue",0xFF6A5ACDu},
    {"slategray",0xFF708090u},{"slategrey",0xFF708090u},{"snow",0xFFFFFAFAu},
    {"springgreen",0xFF00FF7Fu},{"steelblue",0xFF4682B4u},{"tan",0xFFD2B48Cu},
    {"teal",0xFF008080u},{"thistle",0xFFD8BFD8u},{"tomato",0xFFFF6347u},
    {"turquoise",0xFF40E0D0u},{"violet",0xFFEE82EEu},{"wheat",0xFFF5DEB3u},
    {"white",0xFFFFFFFFu},{"whitesmoke",0xFFF5F5F5u},{"yellow",0xFFFFFF00u},
    {"yellowgreen",0xFF9ACD32u},
};

// 单个颜色参数：支持 "128" / "50%" / "0.5"；成功返回 true
static bool ParseColorChannel(const std::string& part, double maxVal, double* out) {
    std::string t = TrimCopy(part);
    if (t.empty()) return false;
    bool pct = (t.back() == '%');
    if (pct) t.pop_back();
    try {
        size_t used = 0;
        double v = std::stod(t, &used);
        if (used == 0) return false;
        *out = pct ? v / 100.0 * maxVal : v;
        return true;
    } catch (...) {
        return false;
    }
}

// 颜色参数切分：逗号 / 空格 / 斜杠（现代语法 rgb(255 0 0 / 50%)）
static std::vector<std::string> SplitColorArgs(const std::string& inner) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : inner) {
        if (c == ',' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

static void HslToRgb(double h, double s, double l, double* r, double* g, double* b) {
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (l < 0) l = 0;
    if (l > 1) l = 1;
    const double c = (1.0 - std::fabs(2.0 * l - 1.0)) * s;
    const double hp = h / 60.0;
    const double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
    double rr = 0, gg = 0, bb = 0;
    const int seg = (int)std::floor(hp) % 6;
    switch (seg) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    const double m = l - c / 2.0;
    *r = (rr + m) * 255.0;
    *g = (gg + m) * 255.0;
    *b = (bb + m) * 255.0;
}

static bool ParseColorValue(const std::string& raw, uint32_t* out, const char* what) {
    std::string s = TrimCopy(raw);
    if (s.empty()) return false;

    // 剥离 !important（CSS 合法写法；不剥离会让 hex 解析吃到多余字符 -> 得到 alpha=0 的透明色）
    {
        std::string low = LowerCopy(s);
        size_t bang = low.rfind('!');
        if (bang != std::string::npos &&
            TrimCopy(low.substr(bang + 1)).rfind("important", 0) == 0) {
            s = TrimCopy(s.substr(0, bang));
        }
        if (s.empty()) {
            Warn(std::string(what) + "：颜色值为空");
            return false;
        }
    }

    const std::string low = LowerCopy(s);
    if (low == "transparent") { *out = 0x00000000u; return true; }
    if (low == "inherit" || low == "currentcolor" || low == "initial" ||
        low == "unset" || low == "none" || low == "auto") {
        Warn(std::string(what) + "：不支持关键字 \"" + s + "\"（编译器无级联上下文，请写具体颜色）");
        return false;
    }
    for (const auto& nc : kNamedColors) {
        if (low == nc.name) { *out = nc.argb; return true; }
    }

    // ---- 十六进制 ----
    if (low[0] == '#') {
        const std::string h = low.substr(1);
        for (char c : h) {
            if (!std::isxdigit((unsigned char)c)) {
                Warn(std::string(what) + "：颜色 \"" + s + "\" 含非十六进制字符");
                return false;
            }
        }
        auto nib = [](char c) -> uint32_t {
            return (uint32_t)(std::isdigit((unsigned char)c) ? c - '0' : (c - 'a' + 10));
        };
        if (h.size() == 3 || h.size() == 4) {
            uint32_t v[4] = { 0, 0, 0, 0 };
            for (size_t i = 0; i < h.size(); ++i) v[i] = nib(h[i]) * 0x11;
            if (h.size() == 3) *out = 0xFF000000u | (v[0] << 16) | (v[1] << 8) | v[2];
            else               *out = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]; // #ARGB
            return true;
        }
        if (h.size() == 6) {
            *out = 0xFF000000u | (uint32_t)std::stoul(h, nullptr, 16);
            return true;
        }
        if (h.size() == 8) {
            *out = (uint32_t)std::stoul(h, nullptr, 16);
            Warn("8 位十六进制按 binUI 约定的 #AARRGGBB 解析（CSS 规范是 #RRGGBBAA）；"
                 "要 CSS 语义请用 rgba()");
            return true;
        }
        Warn(std::string(what) + "：颜色 \"" + s + "\" 位数非法（只支持 #RGB/#ARGB/#RRGGBB/#AARRGGBB）");
        return false;
    }

    // ---- rgb() / rgba() ----
    if (low.rfind("rgb(", 0) == 0 || low.rfind("rgba(", 0) == 0) {
        size_t lp = low.find('(');
        size_t rp = low.rfind(')');
        if (rp == std::string::npos || rp < lp) {
            Warn(std::string(what) + "：rgb() 括号不匹配 \"" + s + "\"");
            return false;
        }
        const auto parts = SplitColorArgs(s.substr(lp + 1, rp - lp - 1));
        if (parts.size() < 3) {
            Warn(std::string(what) + "：rgb() 需要 3 个通道值 \"" + s + "\"");
            return false;
        }
        double r = 0, g = 0, b = 0, a = 1.0;
        if (!ParseColorChannel(parts[0], 255.0, &r) ||
            !ParseColorChannel(parts[1], 255.0, &g) ||
            !ParseColorChannel(parts[2], 255.0, &b)) {
            Warn(std::string(what) + "：rgb() 通道值非法 \"" + s + "\"");
            return false;
        }
        if (parts.size() > 3) ParseColorChannel(parts[3], 1.0, &a);
        auto clamp255 = [](double v) -> uint32_t {
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            return (uint32_t)(v + 0.5);
        };
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        *out = ((uint32_t)(a * 255.0 + 0.5) << 24) |
               (clamp255(r) << 16) | (clamp255(g) << 8) | clamp255(b);
        return true;
    }

    // ---- hsl() / hsla() ----
    if (low.rfind("hsl(", 0) == 0 || low.rfind("hsla(", 0) == 0) {
        size_t lp = low.find('(');
        size_t rp = low.rfind(')');
        if (rp == std::string::npos || rp < lp) {
            Warn(std::string(what) + "：hsl() 括号不匹配 \"" + s + "\"");
            return false;
        }
        auto parts = SplitColorArgs(s.substr(lp + 1, rp - lp - 1));
        if (parts.size() < 3) {
            Warn(std::string(what) + "：hsl() 需要 3 个参数 \"" + s + "\"");
            return false;
        }
        if (parts[0].size() > 3 && LowerCopy(parts[0].substr(parts[0].size() - 3)) == "deg")
            parts[0] = parts[0].substr(0, parts[0].size() - 3);
        double hh = 0, ss = 0, ll = 0, a = 1.0;
        if (!ParseColorChannel(parts[0], 360.0, &hh) ||
            !ParseColorChannel(parts[1], 1.0, &ss) ||
            !ParseColorChannel(parts[2], 1.0, &ll)) {
            Warn(std::string(what) + "：hsl() 参数非法 \"" + s + "\"");
            return false;
        }
        if (parts.size() > 3) ParseColorChannel(parts[3], 1.0, &a);
        double r = 0, g = 0, b = 0;
        HslToRgb(hh, ss, ll, &r, &g, &b);
        auto clamp255 = [](double v) -> uint32_t {
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            return (uint32_t)(v + 0.5);
        };
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        *out = ((uint32_t)(a * 255.0 + 0.5) << 24) |
               (clamp255(r) << 16) | (clamp255(g) << 8) | clamp255(b);
        return true;
    }

    Warn(std::string(what) + "：无法识别的颜色 \"" + s + "\"（支持命名色/#hex/rgb()/rgba()/hsl()/transparent）");
    return false;
}

// ============================================================
// v7 动画解析辅助（文件作用域；编译 HTML/XML 均可复用）
// ============================================================
static std::string AnimLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string AnimTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a]==' '||s[a]=='\t'||s[a]=='\n'||s[a]=='\r')) a++;
    while (b > a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'||s[b-1]=='\r')) b--;
    return s.substr(a, b - a);
}
static float AnimNum(const std::string& s) {
    std::string num;
    for (char c : s) {
        if (std::isdigit((unsigned char)c) || c == '.' || c == '-') num += c;
        else break;
    }
    if (num.empty() || num == "-") return 0;
    try { return std::stof(num); } catch (...) { return 0; }
}
// CSS 时间 -> 毫秒（"0.3s"/"300ms"/"1.2"；无单位按秒容错）
static float AnimTimeMs(const std::string& s) {
    if (s.empty()) return 0;
    std::string lo = AnimLower(s);
    float v = AnimNum(s);
    if (lo.find("ms") != std::string::npos) return v;
    return v * 1000.0f;
}
// easing 关键字 -> 枚举；cubic-bezier(x1,y1,x2,y2) 存入 pts
static uint8_t AnimEasing(const std::string& s, float pts[4]) {
    pts[0] = pts[1] = pts[2] = pts[3] = 0;
    std::string lo = AnimLower(AnimTrim(s));
    if (lo == "linear") return BINUI_EASE_LINEAR;
    if (lo == "ease-in") return BINUI_EASE_IN;
    if (lo == "ease-out") return BINUI_EASE_OUT;
    if (lo == "ease-in-out") return BINUI_EASE_IN_OUT;
    if (lo.rfind("cubic-bezier", 0) == 0) {
        size_t a = lo.find('('), b = lo.find(')');
        if (a != std::string::npos && b != std::string::npos && b > a) {
            std::string inner = lo.substr(a + 1, b - a - 1);
            std::vector<float> vv;
            std::string cur;
            for (char c : inner) {
                if (c == ',' || c == ' ') { if (!cur.empty()) { vv.push_back(AnimNum(cur)); cur.clear(); } }
                else cur += c;
            }
            if (!cur.empty()) vv.push_back(AnimNum(cur));
            if (vv.size() == 4) {
                pts[0]=vv[0]; pts[1]=vv[1]; pts[2]=vv[2]; pts[3]=vv[3];
                return BINUI_EASE_CUBIC;
            }
        }
    }
    return BINUI_EASE_EASE;
}
// 属性名 -> 动画属性位掩码（transition 的 propMask / 提取 all）
static uint16_t AnimProps(const std::string& s) {
    std::string lo = AnimLower(s);
    if (AnimTrim(lo) == "all") return ANIM_OPACITY|ANIM_TRANSLATE|ANIM_SCALE|ANIM_BG_COLOR|ANIM_RADIUS|ANIM_SCALE_X|ANIM_SCALE_Y|ANIM_ROTATE;
    uint16_t m = 0;
    auto has = [&](const char* k) { return lo.find(k) != std::string::npos; };
    if (has("opacity")) m |= ANIM_OPACITY;
    // transform 可能只写了一维：平移 / 等比缩放 / 非等比缩放 / 旋转各自的 mask 由
    // AnimApplyKeyProp 按实际出现的函数再收窄（这里先给全，避免漏属性）
    if (has("transform")) m |= ANIM_TRANSLATE | ANIM_TRANSLATE_X | ANIM_TRANSLATE_Y |
                              ANIM_SCALE | ANIM_SCALE_X | ANIM_SCALE_Y | ANIM_ROTATE;
    if (has("translate")) m |= ANIM_TRANSLATE | ANIM_TRANSLATE_X | ANIM_TRANSLATE_Y;
    if (has("translatex")) m |= ANIM_TRANSLATE_X;
    if (has("translatey")) m |= ANIM_TRANSLATE_Y;
    if (has("scale")) m |= ANIM_SCALE;
    if (has("rotate")) m |= ANIM_ROTATE;
    if (has("background-color") || has("background") || has("color")) m |= ANIM_BG_COLOR;
    if (has("border-radius") || has("radius")) m |= ANIM_RADIUS;
    return m;
}
// 颜色解析（统一走 ParseColorValue；失败返回 0 = "无颜色"）
static uint32_t AnimColor(const std::string& s) {
    uint32_t c = 0;
    if (!ParseColorValue(AnimTrim(s), &c, "动画颜色")) return 0;
    return c;
}
// 提取 transform 函数第 arg 个参数数字：AnimFnNum("translateX(12px) scale(1.1)", "translateX")
static float AnimFnNum(const std::string& s, const std::string& fn, size_t arg = 0) {
    size_t p = s.find(fn + "(");
    for (size_t i = 0; p != std::string::npos && i < arg; i++) p = s.find(fn + "(", p + 1);
    if (p == std::string::npos) return 0;
    p += fn.size() + 1;
    while (p < s.size() && (s[p]==' '||s[p]=='\t')) p++;
    std::string num;
    while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p]=='.' || s[p]=='-')) { num += s[p]; p++; }
    if (num.empty() || num == "-") return 0;
    try { return std::stof(num); } catch (...) { return 0; }
}

// 取 transform 里 `fn(...)` 的**参数列表**（逗号分隔的数值，单位如 px/deg 会被跳过）。
// 为什么单独写一个：AnimFnNum 的 arg 是"第几次出现这个函数"，不是"第几个参数"，
// 用它取 translate(72px, 0) 的第二个值会去找第二个 "translate(" → 永远拿到 0。
static std::vector<float> AnimFnArgs(const std::string& s, const std::string& fn) {
    std::vector<float> out;
    size_t p = s.find(fn + "(");
    if (p == std::string::npos) return out;
    p += fn.size() + 1;
    while (p < s.size() && s[p] != ')') {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == ',')) p++;
        std::string num;
        while (p < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.' ||
                s[p] == '-' || s[p] == '+')) {
            num += s[p];
            p++;
        }
        if (!num.empty() && num != "-" && num != "+") {
            try { out.push_back(std::stof(num)); } catch (...) {}
        }
        while (p < s.size() && s[p] != ',' && s[p] != ')') p++;   // 跳过单位
    }
    return out;
}
// 关键帧声明应用到关键帧结构
static void AnimApplyKeyProp(BinUIKeyFrame& kf, const std::string& propRaw, const std::string& valRaw) {
    std::string p = AnimLower(AnimTrim(propRaw));
    std::string v = AnimTrim(valRaw);
    if (p == "opacity") {
        float o = AnimNum(v);
        kf.opacity = o < 0 ? 0 : (o > 1 ? 1 : o);
        kf.propMask |= ANIM_OPACITY;
    } else if (p == "transform") {
        if (v.find("translate") != std::string::npos) {
            float tx = 0, ty = 0;
            const std::vector<float> ta = AnimFnArgs(v, "translate");
            if (!ta.empty()) {                       // translate(tx, ty)
                tx = ta[0];
                ty = ta.size() > 1 ? ta[1] : 0.0f;
            } else {                                 // translateX / translateY
                tx = AnimFnNum(v, "translateX");
                ty = AnimFnNum(v, "translateY");
            }
            kf.tx = tx; kf.ty = ty;
            kf.propMask |= ANIM_TRANSLATE;
        }
        if (v.find("scale") != std::string::npos) {
            // v20：`scale(sx, sy)` 写的是**非等比**，走 SCALE_X/SCALE_Y；
            // 单参数 `scale(s)` 仍走等比的 ANIM_SCALE（老语义不变）。
            const std::vector<float> sa = AnimFnArgs(v, "scale");
            if (sa.size() >= 2) {
                kf.scaleX = (sa[0] == 0) ? 1.0f : sa[0];
                kf.scaleY = (sa[1] == 0) ? 1.0f : sa[1];
                kf.propMask |= ANIM_SCALE_X | ANIM_SCALE_Y;
            } else {
                const float s = sa.empty() ? 0.0f : sa[0];
                kf.scale = (s == 0) ? 1.0f : s;
                kf.propMask |= ANIM_SCALE;
            }
        }
        if (v.find("rotate") != std::string::npos) {
            // 只认数字（deg 可写可不写）；rad 也按数字读，不做单位换算 ——
            // 与其余属性"只认数值"的约定一致
            const std::vector<float> ra = AnimFnArgs(v, "rotate");
            kf.rotateDeg = ra.empty() ? 0.0f : ra[0];
            kf.propMask |= ANIM_ROTATE;
        }
    } else if (p == "background-color" || p == "background" || p == "color") {
        uint32_t c = AnimColor(v);
        if (c) { kf.bgColor = c; kf.propMask |= ANIM_BG_COLOR; }
    } else if (p == "border-radius" || p == "radius") {
        kf.radius = AnimNum(v);
        kf.propMask |= ANIM_RADIUS;
    }
}
// 解析 css 文本中的 @keyframes 定义（嵌套花括号按深度匹配）
static void AnimParseKeyframes(const std::string& css,
                               std::vector<std::pair<std::string, std::vector<BinUIKeyFrame>>>& groups) {
    size_t p = 0;
    while ((p = css.find("@keyframes", p)) != std::string::npos) {
        size_t open = css.find('{', p + 10);
        if (open == std::string::npos) break;
        std::string name = AnimTrim(css.substr(p + 10, open - (p + 10)));
        int depth = 0;
        size_t close = open;
        for (size_t q = open; q < css.size(); q++) {
            if (css[q] == '{') depth++;
            else if (css[q] == '}') { depth--; if (depth == 0) { close = q; break; } }
        }
        if (depth != 0) break;
        std::string body = css.substr(open + 1, close - open - 1);
        std::vector<BinUIKeyFrame> kfs;
        size_t q = 0;
        while (q < body.size()) {
            size_t ob = body.find('{', q);
            size_t cb = body.find('}', q);
            if (ob == std::string::npos || cb == std::string::npos || cb < ob) break;
            std::string sel = AnimTrim(body.substr(q, ob - q));
            std::string decls = body.substr(ob + 1, cb - ob - 1);
            size_t dq = 0;
            while (dq < sel.size()) {
                size_t comma = sel.find(',', dq);
                std::string part = AnimTrim(sel.substr(dq, (comma == std::string::npos) ? std::string::npos : comma - dq));
                int percent = -1;
                if (part == "from") percent = 0;
                else if (part == "to") percent = 10000;
                else if (part.find('%') != std::string::npos) percent = (int)(AnimNum(part) * 100.0f + 0.5f);
                if (percent >= 0 && percent <= 10000) {
                    BinUIKeyFrame kf;
                    std::memset(&kf, 0, sizeof(kf));
                    kf.percent = (uint16_t)percent;
                    kf.scale = 1.0f;
                    size_t dp = 0;
                    while (dp < decls.size()) {
                        size_t semi = decls.find(';', dp);
                        std::string d = decls.substr(dp, (semi == std::string::npos) ? std::string::npos : semi - dp);
                        size_t colon = d.find(':');
                        if (colon != std::string::npos)
                            AnimApplyKeyProp(kf, d.substr(0, colon), d.substr(colon + 1));
                        if (semi == std::string::npos) break;
                        dp = semi + 1;
                    }
                    kfs.push_back(kf);
                }
                if (comma == std::string::npos) break;
                dq = comma + 1;
            }
            q = cb + 1;
        }
        if (!kfs.empty()) groups.emplace_back(name, std::move(kfs));
        p = close + 1;
    }
}
// 解析 css 文本中带 transition/animation 声明的规则（单选择器：tag/.class/#id）
static void AnimParseRules(const std::string& css,
                           std::vector<std::pair<std::string, std::string>>& animRules,
                           std::vector<std::pair<std::string, std::string>>& transRules) {
    size_t p = 0;
    while (p < css.size()) {
        size_t ob = css.find('{', p);
        if (ob == std::string::npos) break;
        std::string sel = AnimTrim(css.substr(p, ob - p));
        // 深度匹配块结束（支持 @keyframes 内嵌套块）
        int depth = 0;
        size_t cb = css.size();
        for (size_t q = ob; q < css.size(); q++) {
            if (css[q] == '{') depth++;
            else if (css[q] == '}') { depth--; if (depth == 0) { cb = q; break; } }
        }
        if (cb >= css.size()) break;
        if (sel.empty() || sel[0] == '@' || sel.find(' ') != std::string::npos) { p = cb + 1; continue; }
        std::string body = css.substr(ob + 1, cb - ob - 1);
        std::string anim, trans;
        size_t dp = 0;
        while (dp < body.size()) {
            size_t semi = body.find(';', dp);
            std::string d = body.substr(dp, (semi == std::string::npos) ? std::string::npos : semi - dp);
            size_t colon = d.find(':');
            if (colon != std::string::npos) {
                std::string k = AnimLower(AnimTrim(d.substr(0, colon)));
                std::string v = AnimTrim(d.substr(colon + 1));
                if (k == "animation" || k == "animation-name") anim = v;
                else if (k == "transition") trans = v;
            }
            if (semi == std::string::npos) break;
            dp = semi + 1;
        }
        if (!anim.empty()) animRules.emplace_back(sel, anim);
        if (!trans.empty()) transRules.emplace_back(sel, trans);
        p = cb + 1;
    }
}
// 选择器匹配（tag / .class / #id / *；cls 为空格分隔的类列表）
static bool AnimMatchSelector(const std::string& sel, const std::string& tag,
                              const std::string& cls, const std::string& id) {
    std::string s = AnimTrim(sel);
    if (s == "*") return true;
    if (s.empty()) return false;
    if (s[0] == '.') {
        std::string c = s.substr(1);
        size_t q = 0;
        while (q <= cls.size()) {
            size_t sp = cls.find(' ', q);
            std::string part = cls.substr(q, (sp == std::string::npos) ? std::string::npos : sp - q);
            if (part == c) return true;
            if (sp == std::string::npos) break;
            q = sp + 1;
        }
        return false;
    }
    if (s[0] == '#') return id == s.substr(1);
    return s == tag;
}
// 解析 animation 简写（name duration [easing] [delay] [iteration] [direction] [fill]）
static bool AnimParseAnimation(const std::string& v, std::string& name, BinUIAnimDef& def) {
    std::vector<std::string> tok;
    std::string cur;
    for (char c : v) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) tok.push_back(cur);
    if (tok.empty()) return false;
    name = tok[0];
    bool seenDur = false;
    for (size_t i = 1; i < tok.size(); i++) {
        const std::string& t = tok[i];
        std::string lo = AnimLower(t);
        if (lo == "infinite") { def.iteration = 0; continue; }
        if (lo == "normal") { def.direction = BINUI_ANIM_NORMAL; continue; }
        if (lo == "reverse") { def.direction = BINUI_ANIM_REVERSE; continue; }
        if (lo == "alternate") { def.direction = BINUI_ANIM_ALTERNATE; continue; }
        if (lo == "alternate-reverse") { def.direction = BINUI_ANIM_ALT_REVERSE; continue; }
        if (lo == "forwards") { def.fillMode = BINUI_ANIM_FILL_FORWARDS; continue; }
        if (lo == "backwards") { def.fillMode = BINUI_ANIM_FILL_BACKWARDS; continue; }
        if (lo == "both") { def.fillMode = BINUI_ANIM_FILL_BOTH; continue; }
        if (lo == "linear" || lo == "ease" || lo == "ease-in" || lo == "ease-out" ||
            lo == "ease-in-out" || lo.rfind("cubic-bezier", 0) == 0) {
            def.easing = AnimEasing(t, &def.e0x);
            continue;
        }
        if (!lo.empty() && (std::isdigit((unsigned char)lo[0]) || lo[0] == '.')) {
            // 带 s/ms 单位 = 时间（先到为 duration，后到为 delay）；纯数字 = 迭代次数
            if (lo.find("ms") != std::string::npos || lo.find('s') != std::string::npos) {
                if (!seenDur) { def.durationMs = (uint32_t)AnimTimeMs(t); seenDur = true; }
                else def.delayMs = (uint32_t)AnimTimeMs(t);
            } else {
                float n = AnimNum(t);
                if (n > 0) def.iteration = (uint16_t)n;
            }
            continue;
        }
        float n = AnimNum(t);
        if (n > 0) { def.iteration = (uint16_t)n; continue; }
    }
    return !name.empty();
}
// 解析 transition 简写列表（逗号分隔；属性缺省 = all）
static void AnimParseTransitions(const std::string& v, uint16_t nodeId,
                                 std::vector<BinUITransition>& out) {
    size_t p = 0;
    while (p <= v.size()) {
        size_t comma = v.find(',', p);
        std::string part = AnimTrim(v.substr(p, (comma == std::string::npos) ? std::string::npos : comma - p));
        if (!part.empty()) {
            std::vector<std::string> tok;
            std::string cur;
            for (char c : part) {
                if (c == ' ' || c == '\t') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
                else cur += c;
            }
            if (!cur.empty()) tok.push_back(cur);
            BinUITransition t;
            std::memset(&t, 0, sizeof(t));
            t.nodeId = nodeId;
            t.easing = BINUI_EASE_EASE;
            uint16_t props = 0;
            bool seenDur = false;
            for (const auto& tk : tok) {
                std::string lo = AnimLower(tk);
                if (!lo.empty() && (std::isdigit((unsigned char)lo[0]) || lo[0] == '.')) {
                    if (!seenDur) { t.durationMs = (uint32_t)AnimTimeMs(tk); seenDur = true; }
                    else t.delayMs = (uint32_t)AnimTimeMs(tk);
                    continue;
                }
                if (lo == "linear" || lo == "ease" || lo == "ease-in" || lo == "ease-out" ||
                    lo == "ease-in-out" || lo.rfind("cubic-bezier", 0) == 0) {
                    t.easing = AnimEasing(tk, &t.e0x);
                    continue;
                }
                props |= AnimProps(tk);
            }
            t.propMask = props ? props : (ANIM_OPACITY|ANIM_TRANSLATE|ANIM_SCALE|ANIM_BG_COLOR|ANIM_RADIUS|ANIM_SCALE_X|ANIM_SCALE_Y|ANIM_ROTATE);
            out.push_back(t);
        }
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
}

// 追加动画段（v7）：uint32 animBytes + 数据（关键帧组 + 动画定义 + 过渡定义）
static void AppendAnimSection(std::vector<uint8_t>& buf,
                              const std::vector<std::pair<std::string, std::vector<BinUIKeyFrame>>>& groups,
                              const std::vector<BinUIAnimDef>& animDefs,
                              const std::vector<BinUITransition>& transitions) {
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    auto wb = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    };
    wu((uint32_t)groups.size());
    for (const auto& g : groups) {
        wu((uint32_t)g.second.size());
        if (!g.second.empty()) wb(g.second.data(), g.second.size() * sizeof(BinUIKeyFrame));
    }
    wu((uint32_t)animDefs.size());
    if (!animDefs.empty()) wb(animDefs.data(), animDefs.size() * sizeof(BinUIAnimDef));
    wu((uint32_t)transitions.size());
    if (!transitions.empty()) wb(transitions.data(), transitions.size() * sizeof(BinUITransition));

    size_t start = buf.size();
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加名称段（v8）：uint32 nameBytes + uint32 nameCount + {uint32 nodeId; uint32 nameOffset}[]
// nameOffset 指向字符串表（offsets[字符串索引]）；缺失 = 布局无命名节点
static void AppendNameSection(std::vector<uint8_t>& buf,
                              const std::vector<std::pair<uint32_t, uint32_t>>& names,
                              const std::vector<uint32_t>& offsets) {
    if (names.empty()) return;
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)names.size());
    for (const auto& pr : names) {
        wu(pr.first);          // nodeId
        wu(offsets[pr.second]); // 字符串表偏移
    }
    size_t start = buf.size();
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加视频描述段（v9）：uint32 vidBytes + uint32 vidCount + BinUIVideoDesc[]
// 恒定写出（无视频时 vidBytes=0），位于动画段之后、名称段之前——解析链确定；
// 解码由可选模块完成（BINUI_ENABLE_VIDEO）
static void AppendVideoSection(std::vector<uint8_t>& buf,
                               const std::vector<BinUIVideoDesc>& vids) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)vids.size());
    if (!vids.empty())
        data.insert(data.end(),
                    reinterpret_cast<const uint8_t*>(vids.data()),
                    reinterpret_cast<const uint8_t*>(vids.data() + vids.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加状态样式段（v11）：uint32 stateBytes + uint32 stateCount + BinUIStateStyle[]
// 恒定写出（无状态样式时 stateBytes=0），位于视频段之后、名称段之前
static void AppendStateSection(std::vector<uint8_t>& buf,
                               const std::vector<BinUIStateStyle>& states) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)states.size());
    if (!states.empty())
        data.insert(data.end(),
                    reinterpret_cast<const uint8_t*>(states.data()),
                    reinterpret_cast<const uint8_t*>(states.data() + states.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加资源段（v12）：uint32 imgBytes + uint32 imgCount + uint32 pathOffset[]
// 恒定写出（无资源时 imgBytes=4，只有计数 0），让布局自描述：外壳不必再依赖
// 编译器返回的图片列表就能按 imageIndex 加载；组件合并时资源下标在这里重排。
static void AppendImageSection(std::vector<uint8_t>& buf,
                               const std::vector<uint32_t>& imgOffsets) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)imgOffsets.size());
    for (uint32_t o : imgOffsets) wu(o);
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加组件引用段（v12）：uint32 compBytes + uint32 compCount + BinUIComponentRef[]
// 恒定写出（无外挂组件时 compBytes=4），位于资源段之后、名称段之前。
static void AppendComponentSection(std::vector<uint8_t>& buf,
                                   const std::vector<BinUIComponentRef>& refs) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)refs.size());
    if (!refs.empty())
        data.insert(data.end(),
                    reinterpret_cast<const uint8_t*>(refs.data()),
                    reinterpret_cast<const uint8_t*>(refs.data() + refs.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加扩展绘制段（v13）：uint32 paintBytes + uint32 paintCount + BinUIPaintNode[]
// 恒定写出（无扩展绘制时 paintBytes=4），位于组件引用段之后、名称段之前。
static void AppendPaintSection(std::vector<uint8_t>& buf,
                               const std::vector<BinUIPaintNode>& paints) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)paints.size());
    if (!paints.empty())
        data.insert(data.end(),
                    reinterpret_cast<const uint8_t*>(paints.data()),
                    reinterpret_cast<const uint8_t*>(paints.data() + paints.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加阴影段（v23）：uint32 shadowBytes + uint32 shadowCount + BinUIShadow[]
// 恒定写出（无阴影时 shadowBytes=4），位于路径段之后、名称段之前。
static void AppendShadowSection(std::vector<uint8_t>& buf,
                                const std::vector<BinUIShadow>& shadows) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)shadows.size());
    if (!shadows.empty())
        data.insert(data.end(),
                    reinterpret_cast<const uint8_t*>(shadows.data()),
                    reinterpret_cast<const uint8_t*>(shadows.data() + shadows.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加文字阴影段（v24）：uint32 tsBytes + uint32 tsCount + BinUITextShadow[]
// 恒定写出（无阴影时 tsBytes=4），位于阴影段之后、名称段之前。
static void AppendTextShadowSection(std::vector<uint8_t>& buf,
                                    const std::vector<BinUITextShadow>& s) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)s.size());
    if (!s.empty())
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(s.data()),
                    reinterpret_cast<const uint8_t*>(s.data() + s.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}
// 追加绘制附加段（v25）：uint32 peBytes + uint32 peCount + BinUIPaintEx[]
// 恒定写出（无条目时 peBytes=4），位于文字阴影段之后、名称段之前。
static void AppendPaintExSection(std::vector<uint8_t>& buf,
                                 const std::vector<BinUIPaintEx>& items) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)items.size());
    if (!items.empty())
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(items.data()),
                    reinterpret_cast<const uint8_t*>(items.data() + items.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 追加图片滤镜段（v26）：uint32 ffBytes + uint32 ffCount + BinUIImageFilter[]
// 恒定写出（无条目时 ffBytes=4），位于绘制附加段之后、名称段之前。
static void AppendFilterSection(std::vector<uint8_t>& buf,
                                const std::vector<BinUIImageFilter>& items) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wu = [&](uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        data.insert(data.end(), p, p + 4);
    };
    wu((uint32_t)items.size());
    if (!items.empty())
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(items.data()),
                    reinterpret_cast<const uint8_t*>(items.data() + items.size()));
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// v27 裁剪路径：节点 + 设备空间包围盒 + 归一化点（与路径段同一个口径）
struct PendingClip {
    uint16_t nodeId = 0;
    uint16_t flags = 0;
    float x = 0, y = 0, w = 0, h = 0;
    std::vector<uint32_t> first;
    std::vector<uint8_t>  closed;
    std::vector<uint16_t> pts;
};

// 追加裁剪路径段（v27）：uint32 bytes + uint32 count + 每条（定长头 + 子路径表 + 点表）
static void AppendClipSection(std::vector<uint8_t>& buf, const std::vector<PendingClip>& clips) {
    if (clips.empty()) {
        uint32_t zero = 4;
        size_t start = buf.size();
        buf.resize(start + 8, 0);
        memcpy(buf.data() + start, &zero, sizeof(zero));
        uint32_t c0 = 0;
        memcpy(buf.data() + start + 4, &c0, sizeof(c0));
        return;
    }
    std::vector<uint8_t> data;
    auto wbytes = [&](const void* b, size_t n) {
        const uint8_t* p2 = static_cast<const uint8_t*>(b);
        data.insert(data.end(), p2, p2 + n);
    };
    uint32_t count = (uint32_t)clips.size();
    wbytes(&count, sizeof(count));
    for (const PendingClip& c : clips) {
        BinUIClipEntry e = {};
        e.nodeId = c.nodeId;
        e.flags = c.flags;
        e.x = c.x; e.y = c.y; e.w = c.w; e.h = c.h;
        e.pointCount = (uint16_t)std::min<size_t>(65535, c.pts.size() / 2);
        e.contourCount = (uint16_t)std::min<size_t>(65535, c.first.size());
        wbytes(&e, sizeof(e));
        if (!c.first.empty())  wbytes(c.first.data(),  c.first.size()  * sizeof(uint32_t));
        if (!c.closed.empty()) wbytes(c.closed.data(), c.closed.size());
        if (!c.pts.empty())    wbytes(c.pts.data(),    c.pts.size()    * sizeof(uint16_t));
    }
    uint32_t nbytes = (uint32_t)data.size();
    size_t start = buf.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// v28 遮罩：一条 = 一组"同亮度"的掩码几何（同一个节点可以有多条，运行期按顺序两趟画）
struct PendingMask {
    uint16_t nodeId = 0;
    uint16_t flags = 0;                       // v34：MASK_FLAG_FILL_GRAD
    uint32_t color = 0xFFFFFFFFu;             // AARRGGBB：alpha = 亮度，RGB 恒白
    float rx = 0, ry = 0, rw = 0, rh = 0;     // mask 区域（设备空间，= 图层框）
    float x = 0, y = 0, w = 0, h = 0;         // 本组几何包围盒（归一化基准）
    bool hasGrad = false;
    BinUIPathGradient grad = {};              // v34：渐变内容（亮度已折进色标 alpha）
    std::vector<uint32_t> first;
    std::vector<uint8_t>  closed;
    std::vector<uint16_t> pts;
};

// 追加遮罩段（v28）：uint32 bytes + uint32 count + 每条（定长头 + 子路径表 + 点表）
static void AppendMaskSection(std::vector<uint8_t>& buf, const std::vector<PendingMask>& masks) {
    std::vector<uint8_t> data;
    auto wbytes = [&](const void* b, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(b);
        data.insert(data.end(), q, q + n);
    };
    if (!masks.empty()) {
        uint32_t count = (uint32_t)masks.size();
        wbytes(&count, sizeof(count));
        for (const PendingMask& m : masks) {
            BinUIMaskEntry e = {};
            e.nodeId = m.nodeId;
            e.flags = m.hasGrad ? MASK_FLAG_FILL_GRAD : uint16_t(0);
            e.color = m.color;
            e.rx = m.rx; e.ry = m.ry; e.rw = m.rw; e.rh = m.rh;
            e.x = m.x; e.y = m.y; e.w = m.w; e.h = m.h;
            e.pointCount = (uint16_t)std::min<size_t>(65535, m.pts.size() / 2);
            e.contourCount = (uint16_t)std::min<size_t>(65535, m.first.size());
            wbytes(&e, sizeof(e));
            if (m.hasGrad) wbytes(&m.grad, sizeof(m.grad));   // v34：渐变紧跟定长头
            if (!m.first.empty())  wbytes(m.first.data(),  m.first.size()  * sizeof(uint32_t));
            if (!m.closed.empty()) wbytes(m.closed.data(), m.closed.size());
            if (!m.pts.empty())    wbytes(m.pts.data(),    m.pts.size()    * sizeof(uint16_t));
        }
    }
    uint32_t nbytes = (uint32_t)data.size();
    size_t start = buf.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    if (!data.empty()) memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// v29 毛玻璃：节点 + 模糊半径 + 合成圆角（12 字节/条）
static void AppendBackdropSection(std::vector<uint8_t>& buf,
                                  const std::vector<BinUIBackdropEntry>& items) {
    std::vector<uint8_t> data;
    if (!items.empty()) {
        uint32_t count = (uint32_t)items.size();
        data.resize(4 + items.size() * sizeof(BinUIBackdropEntry));
        memcpy(data.data(), &count, sizeof(count));
        memcpy(data.data() + 4, items.data(), items.size() * sizeof(BinUIBackdropEntry));
    }
    uint32_t nbytes = (uint32_t)data.size();
    size_t start = buf.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    if (!data.empty()) memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// v33：外阴影段（filter: drop-shadow）。与毛玻璃段同一个形状：恒定写出，空 = 只有 0。
static void AppendDropShadowSection(std::vector<uint8_t>& buf,
                                    const std::vector<BinUIFilterDropShadowEntry>& items) {
    std::vector<uint8_t> data;
    if (!items.empty()) {
        uint32_t count = (uint32_t)items.size();
        data.resize(4 + items.size() * sizeof(BinUIFilterDropShadowEntry));
        memcpy(data.data(), &count, sizeof(count));
        memcpy(data.data() + 4, items.data(), items.size() * sizeof(BinUIFilterDropShadowEntry));
    }
    uint32_t nbytes = (uint32_t)data.size();
    size_t start = buf.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    if (!data.empty()) memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

// 点用 uint16 归一化存（0..65535 映射包围盒），比 float 省一半且精度远超显示能力。
struct PendingPath {
    BinUIPathEntry head = {};
    bool hasGrad = false;       // v16：head.flags & PATH_FILL_GRAD 时紧跟一个 BinUIPathGradient
    BinUIPathGradient grad = {};
    bool hasStrokeGrad = false; // v17：head.flags & PATH_STROKE_GRAD 时再跟一个（在填充渐变之后）
    BinUIPathGradient strokeGrad = {};
    std::vector<uint32_t> first;
    std::vector<uint8_t>  closed;
    std::vector<uint16_t> pts;
};

// 追加路径段（v15 起，v16/v17 加渐变）：uint32 pathBytes + uint32 pathCount + 每条子记录
// 恒定写出（无路径时 pathBytes=4），位于扩展绘制段之后、名称段之前。
// 子记录顺序（读完定长头 + flag 才知道后面有没有渐变）：
//   头 → 填充渐变(可选) → 描边渐变(可选) → 子路径表 → 点表。
static void AppendPathSection(std::vector<uint8_t>& buf,
                              const std::vector<PendingPath>& paths) {
    size_t start = buf.size();
    std::vector<uint8_t> data;
    auto wbytes = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    };
    uint32_t count = (uint32_t)paths.size();
    wbytes(&count, sizeof(count));
    for (const PendingPath& pp : paths) {
        wbytes(&pp.head, sizeof(pp.head));
        if (pp.hasGrad) wbytes(&pp.grad, sizeof(pp.grad));
        if (pp.hasStrokeGrad) wbytes(&pp.strokeGrad, sizeof(pp.strokeGrad));
        if (!pp.first.empty())  wbytes(pp.first.data(),  pp.first.size()  * sizeof(uint32_t));
        if (!pp.closed.empty()) wbytes(pp.closed.data(), pp.closed.size());
        if (!pp.pts.empty())    wbytes(pp.pts.data(),    pp.pts.size()    * sizeof(uint16_t));
    }
    uint32_t nbytes = (uint32_t)data.size();
    buf.resize(start + sizeof(uint32_t) + data.size());
    memcpy(buf.data() + start, &nbytes, sizeof(nbytes));
    memcpy(buf.data() + start + sizeof(uint32_t), data.data(), data.size());
}

std::vector<uint8_t> CompileBinUISource(const std::string& source) {    return CompileBinUIEx(source).layout;
}

CompileResult CompileBinUIEx(const std::string& source, const CompileOptions& opt) {
    // 找到第一个真实标签（跳过 <?xml?> 处理指令 / <!DOCTYPE> 声明 / <!-- --> 注释），
    // 决定用 XML 还是 HTML 解析
    bool xml = false;
    size_t lt = 0;
    while (lt < source.size()) {
        lt = source.find('<', lt);
        if (lt == std::string::npos || lt + 1 >= source.size()) break;
        if (source[lt + 1] == '?' || source[lt + 1] == '!') {
            if (source.compare(lt, 4, "<!--") == 0) {
                size_t end = source.find("-->", lt + 4);
                lt = (end == std::string::npos) ? source.size() : end + 3;
            } else {
                size_t gt = source.find('>', lt);
                lt = (gt == std::string::npos) ? source.size() : gt + 1;
            }
            continue;
        }
        size_t gt = source.find('>', lt);
        if (gt == std::string::npos) break;
        std::string piece = source.substr(lt + 1, gt - lt - 1);
        while (!piece.empty() && (piece.front()==' '||piece.front()=='\t'||piece.front()=='\n'||piece.front()=='\r'))
            piece.erase(piece.begin());
        std::string tag;
        for (char c : piece) {
            if (c==' '||c=='\t'||c=='\n'||c=='\r') break;
            tag += static_cast<char>(std::tolower((unsigned char)c));
        }
        if (tag == "binui" || tag == "root") xml = true;
        else if (tag == "app_window") {
            // <app_window> 作根：若正文含 HTML 特征（DOCTYPE/<html>/<body>/<head>）按 HTML 解析，
            // 否则按 XML 解析并合成 ROOT 节点（.html 示例带 DOCTYPE+<html>，天然走 HTML）
            std::string rest = source.substr(gt + 1);
            for (auto& c : rest) c = (char)std::tolower((unsigned char)c);
            bool htmlLike = rest.find("<!doctype") != std::string::npos ||
                            rest.find("<html") != std::string::npos ||
                            rest.find("<body") != std::string::npos ||
                            rest.find("<head") != std::string::npos;
            if (!htmlLike) xml = true;
        }
        break;
    }

    CompileResult res;
    {
        // 本次编译的警告收集到 res.warnings（同一会话内去重）
        std::unordered_set<std::string> seen;
        DiagGuard guard(&res.warnings, &seen);

        // 打包清单：主 UI 自己排第 0 条，后面是各组件（ExpandComponents 里追加）。
        // 路径一律相对 appRootDir（= 主 UI 所在目录），所以路径就是文件名本身。
        std::vector<SrcMapEntry> srcEntries;
        std::unordered_set<std::string> srcSeen;
        tl_srcMap = opt.emitSrcMap ? &srcEntries : nullptr;
        tl_srcMapSeen = opt.emitSrcMap ? &srcSeen : nullptr;
        if (opt.emitSrcMap) {
            // 主 UI 排第 0 条；路径先存绝对，最后统一折算成相对 appRootDir
            const std::string rootPath = tl_uiSourceAbs.empty()
                ? (std::filesystem::current_path() / "main.bui.html").generic_string()
                : tl_uiSourceAbs;
            srcSeen.insert(rootPath);
            SrcMapEntry root;
            root.path = rootPath;
            root.data.assign(source.begin(), source.end());
            srcEntries.push_back(std::move(root));
        }

        if (xml) {
            // XML 路径已降级为 legacy：没有 CSS 级联、不认 <use> 组件、也没有 v13/v14 的
            // 扩展绘制段。能力上全面落后于 HTML 路径，但没有任何地方提示过作者。
            // 按 CONTRIBUTING 的规矩"认识但不支持必须 Warn"——这里是"支持但落后",照样提醒。
            Warn("XML 格式（<binui>/<root>）已降级为 legacy：不支持 <style> 级联与 <use> 组件，"
                 "建议改写为 HTML 格式（.html）；两者编译产物同版本，运行期行为一致");
            res.layout = CompileXML(source, &res.images);
        } else {
            res.layout = CompileHTML(source, &res.images);
        }

        tl_srcMap = nullptr;
        tl_srcMapSeen = nullptr;
        if (opt.emitSrcMap && !srcEntries.empty()) {
            NormalizeSrcMapPaths(srcEntries);
            res.srcMap = BuildSrcMap(srcEntries);
        }
    }
    return res;
}

std::vector<uint8_t> CompileBinUIFile(const std::string& path) {
    return CompileBinUIFileEx(path).layout;
}

CompileResult CompileBinUIFileEx(const std::string& path, const CompileOptions& opt) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    // 组件内联（<use src="…">）要相对"当前 UI 文件所在目录"解析路径
    struct BaseDirGuard {
        std::string prevDir;
        std::string prevName;
        std::string prevAbs;
        explicit BaseDirGuard(const std::string& d, const std::string& n, const std::string& a)
            : prevDir(tl_uiBaseDir), prevName(tl_uiSourceName), prevAbs(tl_uiSourceAbs) {
            tl_uiBaseDir = d;
            tl_uiSourceName = n;
            tl_uiSourceAbs = a;
        }
        ~BaseDirGuard() {
            tl_uiBaseDir = prevDir;
            tl_uiSourceName = prevName;
            tl_uiSourceAbs = prevAbs;
        }
    } guard{std::filesystem::path(path).parent_path().generic_string(),
            std::filesystem::path(path).filename().generic_string(),
            std::filesystem::absolute(path).generic_string()};
    return CompileBinUIEx(ss.str(), opt);
}

// 追加缩放规格段（v3）：uint32 specCount + BinUISpecNode[] + uint32 flexCount + BinUISpecFlex[]
static void AppendSpecSection(std::vector<uint8_t>& buf,
                              const std::vector<BinUISpecNode>& specNodes,
                              const std::vector<BinUISpecFlex>& specFlex) {
    uint32_t specCount = (uint32_t)specNodes.size();
    uint32_t flexCount = (uint32_t)specFlex.size();
    size_t start = buf.size();
    buf.resize(start + sizeof(uint32_t)
                   + specNodes.size() * sizeof(BinUISpecNode)
                   + sizeof(uint32_t)
                   + specFlex.size() * sizeof(BinUISpecFlex));
    uint8_t* p = buf.data() + start;
    memcpy(p, &specCount, sizeof(specCount)); p += sizeof(specCount);
    if (!specNodes.empty()) memcpy(p, specNodes.data(), specNodes.size() * sizeof(BinUISpecNode));
    p += specNodes.size() * sizeof(BinUISpecNode);
    memcpy(p, &flexCount, sizeof(flexCount)); p += sizeof(flexCount);
    if (!specFlex.empty()) memcpy(p, specFlex.data(), specFlex.size() * sizeof(BinUISpecFlex));
}

// 窗口描述段（v5）：<app_window> / <binui> 的窗口属性；present=false 时仍写 4 字节 0
struct WinDesc {
    bool present = false;
    uint32_t width = 0, height = 0;
    uint16_t flags = BINUI_WIN_RESIZABLE;
    float aspect = 0;
    uint32_t bgColor = 0xFFFFFFFF;
    std::string title;
};
static void AppendWindowDesc(std::vector<uint8_t>& buf, const WinDesc& d) {
    size_t start = buf.size();
    if (!d.present) {
        uint32_t zero = 0;
        buf.resize(start + sizeof(uint32_t));
        memcpy(buf.data() + start, &zero, sizeof(zero));
        return;
    }
    BinUIWindowDesc wd = {};
    wd.width = d.width; wd.height = d.height;
    wd.flags = d.flags;
    wd.aspect = d.aspect;
    wd.bgColor = d.bgColor;
    wd.titleLen = (uint16_t)std::min<size_t>(d.title.size(), 0xFFFF);
    uint32_t bytes = (uint32_t)(sizeof(wd) + wd.titleLen);
    buf.resize(start + sizeof(uint32_t) + bytes);
    uint8_t* p = buf.data() + start;
    memcpy(p, &bytes, sizeof(bytes)); p += sizeof(bytes);
    memcpy(p, &wd, sizeof(wd)); p += sizeof(wd);
    if (wd.titleLen) memcpy(p, d.title.data(), wd.titleLen);
}

// ============================================================
// XML -> binUI
// ============================================================
std::vector<uint8_t> CompileXML(const std::string& xml, std::vector<std::string>* outImages) {
    {   // 直接调 CompileXML/CompileHTML（绕过 Ex 变体）时也要有诊断容器，否则警告不去重
        if (!g_diag.seen) {
            static thread_local std::unordered_set<std::string> s_seen;
            static thread_local std::vector<std::string> s_list;
            s_seen.clear(); s_list.clear();
            g_diag.seen = &s_seen;
            g_diag.list = &s_list;
        }
    }
    struct XMLElem {
        std::string tag;
        std::unordered_map<std::string, std::string> attrs;
        std::string text;
        size_t parentIdx;   // SIZE_MAX = 无父（根）
    };
    std::vector<XMLElem> elems;
    std::vector<size_t> openStack;

    auto trimText = [](const std::string& raw) -> std::string {
        size_t s = raw.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) return {};
        size_t e = raw.find_last_not_of(" \t\n\r");
        return raw.substr(s, e - s + 1);
    };

    size_t i = 0;
    while (i < xml.size()) {
        size_t open = xml.find('<', i);
        if (open == std::string::npos) {
            if (!openStack.empty()) {
                std::string t = DecodeEntities(trimText(xml.substr(i)));
                if (!t.empty()) elems[openStack.back()].text += t;
            }
            break;
        }
        if (open > i && !openStack.empty()) {
            std::string t = DecodeEntities(trimText(xml.substr(i, open - i)));
            if (!t.empty()) elems[openStack.back()].text += t;
        }
        if (open + 1 >= xml.size()) break;

        char c = xml[open + 1];
        if (c == '/') {
            size_t gt = xml.find('>', open);
            if (gt == std::string::npos) break;
            if (!openStack.empty()) openStack.pop_back();
            i = gt + 1;
            continue;
        }
        if (c == '?' || c == '!') {
            if (c == '!' && xml.compare(open, 9, "<![CDATA[") == 0) {
                size_t end = xml.find("]]>", open + 9);
                if (end == std::string::npos) break;
                if (!openStack.empty())
                    elems[openStack.back()].text += xml.substr(open + 9, end - open - 9);
                i = end + 3;
            } else if (c == '!' && xml.compare(open, 4, "<!--") == 0) {
                size_t end = xml.find("-->", open + 4);
                i = (end == std::string::npos) ? xml.size() : end + 3;
            } else {
                size_t gt = xml.find('>', open);
                i = (gt == std::string::npos) ? xml.size() : gt + 1;
            }
            continue;
        }

        size_t gt = xml.find('>', open);
        if (gt == std::string::npos) break;
        std::string piece = xml.substr(open + 1, gt - open - 1);
        i = gt + 1;

        bool selfClose = !piece.empty() && piece.back() == '/';
        if (selfClose) piece.pop_back();

        size_t sp = piece.find_first_of(" \t\n\r");
        std::string tag = (sp == std::string::npos) ? piece : piece.substr(0, sp);

        XMLElem elem;
        elem.tag = tag;
        elem.parentIdx = openStack.empty() ? SIZE_MAX : openStack.back();

        std::string attrs = (sp == std::string::npos) ? "" : piece.substr(sp);
        size_t ap = 0;
        while (ap < attrs.size()) {
            while (ap < attrs.size() && (attrs[ap] == ' ' || attrs[ap] == '\t')) ap++;
            if (ap >= attrs.size()) break;
            size_t eq = attrs.find('=', ap);
            if (eq == std::string::npos) {
                // 无值属性（布尔语义，如 <video muted>）：存空串，不再中断解析
                size_t e2 = ap;
                while (e2 < attrs.size() && attrs[e2] != ' ' && attrs[e2] != '\t' &&
                       attrs[e2] != '\n' && attrs[e2] != '\r') e2++;
                std::string key = attrs.substr(ap, e2 - ap);
                while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
                if (!key.empty()) elem.attrs[key] = "";
                ap = e2;
                continue;
            }
            std::string key = attrs.substr(ap, eq - ap);
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
            ap = eq + 1;
            while (ap < attrs.size() && (attrs[ap] == ' ' || attrs[ap] == '\t')) ap++;
            std::string value;
            if (ap < attrs.size() && (attrs[ap] == '"' || attrs[ap] == '\'')) {
                char q = attrs[ap];
                size_t q2 = attrs.find(q, ap + 1);
                value = (q2 == std::string::npos) ? attrs.substr(ap + 1)
                                                  : attrs.substr(ap + 1, q2 - ap - 1);
                ap = (q2 == std::string::npos) ? attrs.size() : q2 + 1;
            } else {
                size_t e2 = ap;
                while (e2 < attrs.size() && attrs[e2] != ' ' && attrs[e2] != '\t' &&
                       attrs[e2] != '\n' && attrs[e2] != '\r') e2++;
                value = attrs.substr(ap, e2 - ap);
                ap = e2;
            }
            if (!key.empty()) elem.attrs[key] = DecodeEntities(value);
        }

        elems.push_back(elem);
        if (!selfClose) openStack.push_back(elems.size() - 1);
    }

    if (elems.empty()) return {};

    // map XML tag to node type
    auto tagToType = [](const std::string& tag) -> uint8_t {
        if (tag == "binui" || tag == "root") return BINUI_ROOT;
        if (tag == "label") return BINUI_LABEL;
        if (tag == "button") return BINUI_BUTTON;
        if (tag == "input") return BINUI_INPUT;
        if (tag == "fab" || tag == "image") return BINUI_IMAGE;
        if (tag == "line") return BINUI_LINE;
        if (tag == "svg") return BINUI_SVG;
        if (tag == "canvas") return BINUI_CANVAS;
        if (tag == "video") return BINUI_VIDEO;
        return BINUI_LIST; // card, statusbar, appbar, navbar, divider, etc.
    };

    auto getAttr = [](const XMLElem& e, const std::string& key, const std::string& def) -> std::string {
        auto it = e.attrs.find(key);
        return (it != e.attrs.end()) ? it->second : def;
    };
    auto getColor = [&](const XMLElem& e, const std::string& key, uint32_t def) -> uint32_t {
        std::string s = getAttr(e, key, "");
        if (s.empty()) return def;
        uint32_t c = def;
        // 统一走 ParseColorValue：命名色 / transparent / #hex / rgb() / rgba() / hsl()
        // 解析失败保持调用的默认色并已打警告
        if (!ParseColorValue(s, &c, key.c_str())) return def;
        return c;
    };
    auto getInt = [&](const XMLElem& e, const std::string& key, int def) -> int {
        std::string s = getAttr(e, key, "");
        if (s.empty()) return def;
        try { return std::stoi(s); } catch (...) { return def; }
    };
    auto getPad = [&](const XMLElem& e, const std::string& key, uint16_t def) -> uint16_t {
        std::string s = getAttr(e, key, "");
        if (s.empty()) return def;
        // 去掉尾部单位（px）
        while (!s.empty() && !(s.back() >= '0' && s.back() <= '9') && s.back() != '.') s.pop_back();
        try { return (uint16_t)std::stof(s); } catch (...) { return def; }
    };

    // ---- 窗口描述：可选顶层 <app_window> 包裹层；否则取 <binui> 的 width/height/bg ----
    WinDesc wdesc;
    bool hasAppWindow = false;
    for (const auto& e : elems) {
        if (e.tag != "app_window" || e.parentIdx != SIZE_MAX) continue;
        hasAppWindow = true;
        wdesc.present = true;
        wdesc.width  = (uint32_t)getInt(e, "width", (int)getInt(e, "w", 0));
        wdesc.height = (uint32_t)getInt(e, "height", (int)getInt(e, "h", 0));
        wdesc.bgColor = getColor(e, "bg", getColor(e, "background", 0xFFFFFFFF));
        std::string rs = getAttr(e, "resizable", "true");
        for (auto& c : rs) c = (char)std::tolower((unsigned char)c);
        if (rs == "false" || rs == "no" || rs == "0") wdesc.flags &= ~BINUI_WIN_RESIZABLE;
        std::string asp = getAttr(e, "aspect", "");
        if (!asp.empty()) {
            wdesc.flags |= BINUI_WIN_ASPECT;
            std::string al = asp, ar = "";
            size_t colon = asp.find(':');
            if (colon != std::string::npos) { al = asp.substr(0, colon); ar = asp.substr(colon + 1); }
            float a = 0, b = 1;
            try { a = std::stof(al); } catch (...) { a = 0; }
            if (!ar.empty()) { try { b = std::stof(ar); } catch (...) { b = 1; } }
            wdesc.aspect = (b != 0) ? a / b : 1.0f;
            if (wdesc.aspect <= 0) wdesc.aspect = 1.0f;
        }
        // window_resize: allow(允许) / disable(禁用) / aspect(仅等比例) —— 覆盖 resizable/aspect
        std::string wrz = getAttr(e, "window_resize", "");
        for (auto& c : wrz) c = (char)std::tolower((unsigned char)c);
        if (wrz == "disable" || wrz == "no" || wrz == "0") {
            wdesc.flags &= ~BINUI_WIN_RESIZABLE;
        } else if (wrz == "aspect") {
            wdesc.flags |= BINUI_WIN_RESIZABLE | BINUI_WIN_ASPECT;
        } else if (wrz == "allow" || wrz == "yes" || wrz == "1") {
            wdesc.flags |= BINUI_WIN_RESIZABLE;
            wdesc.flags &= ~BINUI_WIN_ASPECT;
            wdesc.aspect = 0;
        }
        // ui_scale="proportional"：resize 时整棵节点树（坐标/字号/圆角）随窗口等比缩放
        std::string us = getAttr(e, "ui_scale", "");
        for (auto& c : us) c = (char)std::tolower((unsigned char)c);
        if (us == "proportional" || us == "fit" || us == "yes" || us == "1")
            wdesc.flags |= BINUI_WIN_SCALE;
        std::string tp = getAttr(e, "transparent", "");
        for (auto& c : tp) c = (char)std::tolower((unsigned char)c);
        bool transparent = (tp == "true" || tp == "yes" || tp == "1")
                        || ((wdesc.bgColor >> 24) & 0xFF) < 0xFF;
        if (transparent) wdesc.flags |= BINUI_WIN_TRANSPARENT;
        wdesc.title = getAttr(e, "title", "");
        break;
    }
    if (!hasAppWindow && !elems.empty() &&
        (elems[0].tag == "binui" || elems[0].tag == "root")) {
        const auto& r = elems[0];
        wdesc.present = true;
        wdesc.width  = (uint32_t)getInt(r, "width", (int)getInt(r, "w", 0));
        wdesc.height = (uint32_t)getInt(r, "height", (int)getInt(r, "h", 0));
        wdesc.bgColor = getColor(r, "bg", getColor(r, "background", 0xFFFFFFFF));
        if (((wdesc.bgColor >> 24) & 0xFF) < 0xFF) wdesc.flags |= BINUI_WIN_TRANSPARENT;
    }

    uint32_t id = 0;
    std::vector<BinUINode> nodes;
    std::vector<BinUISpecNode> specNodes;
    std::vector<BinUISpecFlex> specFlex;
    std::vector<BinUIAnimDef> animDefs;        // v7 动画（XML：无 @keyframes 源，通常为空）
    std::vector<BinUITransition> transitions;  // v7 过渡（transition 属性）
    std::vector<std::pair<uint32_t, uint32_t>> nodeNames; // v8 名称表 {nodeId, 字符串索引}
    std::vector<BinUIVideoDesc> videoDescs;    // v9 视频描述段（<video> 节点）

    std::vector<std::string> strings;
    strings.push_back(""); // 字符串表首位 = 空串：无文本节点的 textOffset=0 指向空串
    std::unordered_map<uint32_t, std::string> textMap;

    auto getOrAddString = [&](const std::string& s) -> uint32_t {
        for (uint32_t i = 0; i < strings.size(); i++)
            if (strings[i] == s) return i;
        strings.push_back(s);
        return (uint32_t)(strings.size() - 1);
    };

    // build string table offsets from linear concatenation
    auto buildStringTable = [&](std::vector<char>& out) {
        std::vector<uint32_t> offsets(strings.size());
        uint32_t cur = 0;
        for (size_t i = 0; i < strings.size(); i++) {
            offsets[i] = cur;
            for (char c : strings[i]) out.push_back(c);
            out.push_back('\0');
            cur += (uint32_t)strings[i].size() + 1;
        }
        return offsets;
    };

    // assign IDs in order: root first, then children
    // 坐标父级相对：子节点绝对位置 = 父内容盒原点 + 显式坐标
    struct XCtx { uint16_t nodeId; size_t nodeIdx; float ox, oy, cw; };
    std::vector<XCtx> xstack;
    std::vector<uint32_t> elemId(elems.size(), 0xFFFF);

    // <app_window> 作根且无显式 <binui>/<root>：合成 ROOT 节点（id 0），其子元素挂根下
    bool hasExplicitRoot = false;
    size_t appWindowIdx = SIZE_MAX;
    for (size_t idx = 0; idx < elems.size(); idx++) {
        if (elems[idx].tag == "binui" || elems[idx].tag == "root") hasExplicitRoot = true;
        if (appWindowIdx == SIZE_MAX && elems[idx].tag == "app_window" &&
            elems[idx].parentIdx == SIZE_MAX)
            appWindowIdx = idx;
    }
    if (hasAppWindow && !hasExplicitRoot) {
        BinUINode root = {};
        root.id = 0; root.parentId = 0xFFFF;
        root.type = BINUI_ROOT; root.flags = BINUI_VISIBLE;
        root.x = 0; root.y = 0;
        root.width = (float)(wdesc.width > 0 ? wdesc.width : 1280);
        root.height = (float)(wdesc.height > 0 ? wdesc.height : 720);
        root.color = wdesc.bgColor;
        nodes.push_back(root);
        id = 1; // 根已占用 id 0
        if (appWindowIdx != SIZE_MAX) elemId[appWindowIdx] = 0; // 其子元素父级 = 根
        xstack.push_back({ 0, 0, root.x + root.padding, root.y + root.padding,
                           root.width - 2 * root.padding });
    }

    for (size_t idx = 0; idx < elems.size(); idx++) {
        if (id >= 0x10000) break; // 节点 id 为 uint16_t，超限停止生成
        auto& e = elems[idx];
        if (e.tag == "app_window") continue; // 包裹层：不进节点树，其子元素直接挂根
        uint32_t pnodeId = (e.parentIdx == SIZE_MAX) ? 0xFFFF : elemId[e.parentIdx];
        while (xstack.size() > 0 && xstack.back().nodeId != pnodeId) xstack.pop_back();
        float ox = xstack.empty() ? 0 : xstack.back().ox;
        float oy = xstack.empty() ? 0 : xstack.back().oy;
        float cw = xstack.empty() ? 100 : xstack.back().cw;
        // 父容器内容盒高（% 高参考）
        float ch = 0;
        if (!xstack.empty()) {
            const BinUINode& pn = nodes[xstack.back().nodeIdx];
            ch = pn.height - 2.0f * pn.padding;
            if (ch < 0) ch = 0;
        }

        // <app_window> 提供尺寸时作为 <binui> 根节点的缺省尺寸
        std::string dfltW, dfltH;
        uint32_t dfltBg = 0xFFFFFFFF;
        if (e.tag == "binui" || e.tag == "root") {
            if (hasAppWindow && wdesc.width > 0) dfltW = std::to_string((int)wdesc.width);
            if (hasAppWindow && wdesc.height > 0) dfltH = std::to_string((int)wdesc.height);
            if (hasAppWindow) dfltBg = wdesc.bgColor;
        }

        BinUINode n = {};
        n.id = id++;
        elemId[idx] = n.id;
        n.parentId = pnodeId;
        n.type = tagToType(e.tag);
        n.flags = BINUI_VISIBLE;
        if (e.tag == "titlebar")
            n.flags |= BINUI_TITLEBAR;
        // x/y/w/h 支持 %：相对父容器内容盒；也支持 em/rem/vw/vh（em 以本元素 size 为基准）
        LenCtx xctx;
        xctx.fontSize = (float)getInt(e, "size", (int)getInt(e, "fontsize", 14));
        xctx.rootFont = 14.0f;
        xctx.viewW = (float)(wdesc.width  ? wdesc.width  : 1280);
        xctx.viewH = (float)(wdesc.height ? wdesc.height : 720);
        LenPct px = ParseLenPct(getAttr(e, "x", ""), 0, xctx, "x");
        LenPct py = ParseLenPct(getAttr(e, "y", ""), 0, xctx, "y");
        LenPct pw = ParseLenPct(getAttr(e, "w", getAttr(e, "width", dfltW)), cw, xctx, "width");
        LenPct ph = ParseLenPct(getAttr(e, "h", getAttr(e, "height", dfltH)), 40, xctx, "height");
        n.x = ox + (px.pct ? px.value / 100.0f * cw : px.value);
        n.y = oy + (py.pct ? py.value / 100.0f * ch : py.value);
        n.width = pw.pct ? pw.value / 100.0f * cw : pw.value;
        n.height = ph.pct ? ph.value / 100.0f * ch : ph.value;
        float baseW = n.width, baseH = n.height; // 声明值（未钳制）：运行时重排基准
        // 尺寸约束：min/max-width/height（支持 px 与 %；属性缺省 = 未指定，值 -1）
        float mnW = -1, mxW = -1, mnH = -1, mxH = -1;
        uint16_t mnWp = 0, mxWp = 0, mnHp = 0, mxHp = 0;
        auto getCon = [&](const std::string& key, float& out, uint16_t& pct) {
            std::string s = getAttr(e, key, "");
            if (s.empty()) { out = -1; return; }
            LenPct p = ParseLenPct(s, 0, xctx, key.c_str());
            if (p.pct) { out = p.value / 100.0f; pct = 1; }
            else out = p.value;
        };
        getCon("min-width",  mnW, mnWp);
        getCon("max-width",  mxW, mxWp);
        getCon("min-height", mnH, mnHp);
        getCon("max-height", mxH, mxHp);
        float effMnW = mnW >= 0 ? (mnWp ? mnW * cw : mnW) : -1;
        float effMxW = mxW >= 0 ? (mxWp ? mxW * cw : mxW) : -1;
        float effMnH = mnH >= 0 ? (mnHp ? mnH * ch : mnH) : -1;
        float effMxH = mxH >= 0 ? (mxHp ? mxH * ch : mxH) : -1;
        if (effMnW >= 0 && n.width < effMnW) n.width = effMnW;
        if (effMxW >= 0 && n.width > effMxW) n.width = effMxW;
        if (effMnH >= 0 && n.height < effMnH) n.height = effMnH;
        if (effMxH >= 0 && n.height > effMxH) n.height = effMxH;
        n.color = getColor(e, "color", getColor(e, "bg", dfltBg));
        n.fontSize = (uint16_t)getInt(e, "size", (int)getInt(e, "fontsize", 14));
        n.padding = getPad(e, "padding", (uint16_t)getInt(e, "pad", 0));
        n.radius = (uint16_t)getInt(e, "radius", 0); // 圆角：未指定 = 直角
        n.zIndex = (int16_t)getInt(e, "z-index", (int)getInt(e, "z", 0)); // 层叠顺序：未指定 = 0

        if (e.tag == "button" || e.tag == "fab" || e.tag == "input" || e.tag == "video")
            n.flags |= BINUI_INTERACTIVE;

        if (!e.text.empty())
            textMap[n.id] = e.text;

        // 图片节点：src 属性 -> imageIndex（引用编译结果中的资源路径表）
        if (n.type == BINUI_IMAGE && outImages) {
            std::string src = getAttr(e, "src", getAttr(e, "image", ""));
            if (!src.empty()) {
                n.imageIndex = (uint16_t)outImages->size();
                outImages->push_back(src);
            }
        }

        // 视频节点（XML）：src -> 伪图片槽 + 视频描述段；解码由可选模块完成
        if (n.type == BINUI_VIDEO) {
            std::string vsrc = getAttr(e, "src", "");
            BinUIVideoDesc vd = {};
            vd.nodeId = n.id;
            if (!vsrc.empty()) {
                vd.srcOffset = getOrAddString(vsrc);
                if (outImages) { // 伪图片槽与 img 共用编号空间
                    n.imageIndex = (uint16_t)outImages->size();
                    outImages->push_back(vsrc);
                }
            }
            vd.imageIndex = n.imageIndex;
            auto flagOf = [&](const char* a) -> bool {
                auto it = e.attrs.find(a); // XML 布尔属性同样无值
                if (it == e.attrs.end()) return false;
                std::string v = it->second;
                for (auto& c : v) c = (char)std::tolower((unsigned char)c);
                return v != "false" && v != "0" && v != "off";
            };
            if (flagOf("autoplay")) vd.flags |= VID_AUTOPLAY;
            if (flagOf("loop"))     vd.flags |= VID_LOOP;
            if (flagOf("muted"))    vd.flags |= VID_MUTED;
            if (flagOf("controls")) vd.flags |= VID_CONTROLS;
            float vol = 1.0f;
            try { vol = std::stof(getAttr(e, "volume", "1")); } catch (...) {}
            vd.volume = std::min(std::max(vol, 0.0f), 1.0f);
            videoDescs.push_back(vd);
        }

        // v7 过渡（XML 属性 transition="background-color 0.3s"）
        {
            std::string tr = getAttr(e, "transition", "");
            if (!tr.empty()) AnimParseTransitions(tr, n.id, transitions);
        }

        nodes.push_back(n);

        // v8 名称表：id/name 属性 -> 节点名（运行时 findNodeByName 查找）
        if (e.attrs.count("id") && !e.attrs.at("id").empty())
            nodeNames.push_back({ n.id, getOrAddString(e.attrs.at("id")) });

        // 缩放规格：setSize 重排依据
        BinUISpecNode sp = {};
        sp.nodeId = n.id;
        sp.pctX = px.pct ? px.value / 100.0f : -1;
        sp.pctY = py.pct ? py.value / 100.0f : -1;
        sp.pctW = pw.pct ? pw.value / 100.0f : -1;
        sp.pctH = ph.pct ? ph.value / 100.0f : -1;
        if (px.pct) sp.flags |= SPEC_PCT_X;
        if (py.pct) sp.flags |= SPEC_PCT_Y;
        if (pw.pct) sp.flags |= SPEC_PCT_W;
        if (ph.pct) sp.flags |= SPEC_PCT_H;
        if (!e.attrs.count("w") && !e.attrs.count("width")) sp.flags |= SPEC_AUTO_W;
        sp.relX = n.x - ox;
        sp.relY = n.y - oy;
        sp.minW = mnW; sp.maxW = mxW; sp.minH = mnH; sp.maxH = mxH;
        if (mnWp) sp.flags |= SPEC_MINW_PCT;
        if (mxWp) sp.flags |= SPEC_MAXW_PCT;
        if (mnHp) sp.flags |= SPEC_MINH_PCT;
        if (mxHp) sp.flags |= SPEC_MAXH_PCT;
        sp.relW = (e.attrs.count("w") || e.attrs.count("width")) && !pw.pct ? baseW : -1;
        sp.relH = (e.attrs.count("h") || e.attrs.count("height")) && !ph.pct ? baseH : -1;
        specNodes.push_back(sp);

        // 容器内容盒 = 自身位置 + padding
        xstack.push_back({ n.id, nodes.size() - 1, n.x + n.padding, n.y + n.padding, n.width - 2 * n.padding });
    }

    // build string table
    // v12 资源段存的是偏移，图片路径必须先进表
    if (outImages)
        for (const auto& p : *outImages) getOrAddString(p);
    for (auto& [nodeId, txt] : textMap)
        getOrAddString(txt);

    std::vector<char> stringData;
    auto offsets = buildStringTable(stringData);

    for (auto& n : nodes) {
        auto it = textMap.find(n.id);
        if (it != textMap.end())
            n.textOffset = offsets[getOrAddString(it->second)];
        else
            n.textOffset = 0;
    }

    // serialize
    uint32_t nodeCount = (uint32_t)nodes.size();
    uint32_t strSize = (uint32_t)stringData.size();
    std::vector<uint8_t> buf(sizeof(BinUIHeader) + nodeCount * sizeof(BinUINode) + strSize);
    uint8_t* ptr = buf.data();

    BinUIHeader hdr;
    hdr.magic = BINUI_HTML_MAGIC;
    hdr.version = BINUI_VERSION;
    hdr.nodeCount = nodeCount;
    hdr.stringSize = strSize;
    memcpy(ptr, &hdr, sizeof(hdr)); ptr += sizeof(hdr);
    memcpy(ptr, nodes.data(), nodeCount * sizeof(BinUINode)); ptr += nodeCount * sizeof(BinUINode);
    memcpy(ptr, stringData.data(), strSize);
    AppendSpecSection(buf, specNodes, specFlex);
    // window_resize="aspect" 且未显式给 aspect：用声明的窗口宽高比兜底
    if ((wdesc.flags & BINUI_WIN_ASPECT) && wdesc.aspect <= 0 &&
        wdesc.width > 0 && wdesc.height > 0)
        wdesc.aspect = (float)wdesc.width / (float)wdesc.height;
    AppendWindowDesc(buf, wdesc);
    {
        std::vector<std::pair<std::string, std::vector<BinUIKeyFrame>>> emptyGroups;
        AppendAnimSection(buf, emptyGroups, animDefs, transitions);
    }
    AppendVideoSection(buf, videoDescs);
    AppendStateSection(buf, {});   // XML 路径：无 <style>，但段恒定写出保证解析链确定
    {
        std::vector<uint32_t> imgOffsets;
        if (outImages)
            for (const auto& p : *outImages) imgOffsets.push_back(offsets[getOrAddString(p)]);
        AppendImageSection(buf, imgOffsets);
    }
    AppendComponentSection(buf, {}); // XML 路径：无 <use>，段恒定写出保证解析链确定
    AppendPaintSection(buf, {});     // v13：XML 路径无扩展绘制，段同样恒定写出
    AppendPathSection(buf, {});      // v15：XML 路径无 SVG 路径，段同样恒定写出
    AppendShadowSection(buf, {});    // v23：XML 路径无 box-shadow，段同样恒定写出
    AppendTextShadowSection(buf, {}); // v24：XML 路径无 text-shadow，段同样恒定写出
    AppendPaintExSection(buf, {});  // v25：XML 路径无九宫格图片
    AppendFilterSection(buf, {});     // v26：这条路径没有图片滤镜
    AppendClipSection(buf, {});       // v27：这条路径没有裁剪路径
    AppendMaskSection(buf, {});       // v28：XML 路径没有 mask
    AppendBackdropSection(buf, {});   // v29：XML 路径没有毛玻璃
    AppendDropShadowSection(buf, {}); // v33：XML 路径没有 drop-shadow
    AppendNameSection(buf, nodeNames, offsets);

    return buf;
}

// ============================================================
// CSS 规则（<style> 里的非动画声明）
//
// 选择器：tag / .class / #id / tag.class / *（逗号分组），可叠加伪类与伪元素：
//   运行时伪类  :hover :active :focus :checked
//               → v11 状态样式段，渲染器在指针/焦点/勾选变化时切换目标值
//   结构伪类    :first-child :last-child :only-child
//               :nth-child() :nth-last-child() :nth-of-type() :nth-last-of-type()
//               :first-of-type :last-of-type :only-of-type :not(简单选择器) :root
//               → 编译期按兄弟序号求值
//   伪元素      ::before / ::after（要求 content）→ 编译期合成节点或并入宿主文本
// 不支持：后代/子代/兄弟组合子，:visited/:disabled/:focus-within 等交互伪类，
//         ::first-line/::first-letter/::selection/::placeholder（无对应渲染阶段）。
// 级联优先级（与浏览器一致）：内联 style > #id(100) > .class(10) > tag(1) > *(0)，
// 伪类按 class 计权（+10），伪元素按元素计权（+1）；同权重后写的胜。
// @ 开头的 at-rule（@keyframes/@media…）整块跳过，交给动画解析器。
// ============================================================

// 单个元素的选择器匹配上下文（结构伪类需要的兄弟信息 + 运行时状态掩码）
struct CssElemCtx {
    std::string tag, cls, id;
    int  idx = -1;      // 同父兄弟中的序号（0 起）；-1 = 无结构信息
    int  cnt = 0;       // 同父兄弟总数
    int  typeIdx = -1;  // 同父同 tag 的序号
    int  typeCnt = 0;
    bool isRoot = false;
    int  stateMask = 0; // BinUIStateFlag 的或；基础样式求值时恒为 0
    // 属性选择器 [attr=v] 取值用。由 ctxOf() 指向元素的属性表（编译期元素表稳定，
    // 且伪元素展开后有一次统一重绑，见 CompileHTML 里的 attrs 重绑循环）
    const std::unordered_map<std::string, std::string>* attrs = nullptr;
};

// 属性选择器：[attr] / [attr=v] / [attr^=v] / [attr$=v] / [attr*=v] / [attr~=v] / [attr|=v]
struct CssAttrSel {
    std::string name, op, val;   // op: ""(存在) "=" "^=" "$=" "*=" "~=" "|="
};

struct CssPseudo {
    enum Kind {
        HOVER, ACTIVE, FOCUS, CHECKED,
        FIRST_CHILD, LAST_CHILD, ONLY_CHILD,
        NTH_CHILD, NTH_LAST_CHILD,
        FIRST_OF_TYPE, LAST_OF_TYPE, ONLY_OF_TYPE,
        NTH_OF_TYPE, NTH_LAST_OF_TYPE,
        NOT_SEL, ROOT,
    } kind = HOVER;
    int a = 0, b = 0;      // An+B（结构伪类）
    int spec = 10;         // 特异性附加
    // :not(简单选择器) 的内层选择器
    std::string notTag, notId;
    std::vector<std::string> notClasses;
    std::vector<CssAttrSel> notAttrs;
};

struct CssRule {
    // 单条复杂选择器拆成的若干"简单选择器段"，从右往左匹配（CSS 标准方向）。
    // 例：".card > .title:hover" → parts = [ {".card", comb='|'}, {".title:hover", comb='>'} ]
    struct Simple {
        std::string tag;                        // "" / "*" = 通配
        std::string id;
        std::vector<std::string> classes;       // .a.b.c 全部要命中
        std::vector<CssAttrSel> attrs;
        std::vector<CssPseudo> pseudos;
        int spec = 0;
        bool empty() const {
            return tag.empty() && id.empty() && classes.empty() && attrs.empty() && pseudos.empty();
        }
    };
    struct Part {
        Simple sel;
        char   comb = '|';                      // 与"左边那段"的关系：'|' 起始，' ' 后代，'>' 子，'+' 相邻，'~' 兄弟
    };

    std::vector<Part> parts;                    // 至少 1 段
    std::string pseudoEl;                       // "" = 普通规则；"before"/"after" = 伪元素（只允许单段）
    int spec = 0;
    size_t order = 0;                           // 文档序（同权重时后者胜）
    std::unordered_map<std::string, std::string> decls;

    // 常用快捷访问：单段规则才有意义（伪元素规则必然单段）
    const Simple& head() const { return parts.front().sel; }
};

// "An+B" / "odd" / "even" / "5" → (a, b)
static bool ParseNthExpr(const std::string& raw, int* outA, int* outB) {
    std::string t;
    for (char c : LowerCopy(TrimCopy(raw)))
        if (c != ' ' && c != '\t') t += c;
    if (t.empty()) return false;
    if (t == "odd")  { *outA = 2; *outB = 1; return true; }
    if (t == "even") { *outA = 2; *outB = 2; return true; }
    const size_t npos = t.find('n');
    if (npos == std::string::npos) {
        *outA = 0;
        try { *outB = std::stoi(t); } catch (...) { return false; }
        return true;
    }
    const std::string as = t.substr(0, npos);
    if (as.empty() || as == "+") *outA = 1;
    else if (as == "-") *outA = -1;
    else { try { *outA = std::stoi(as); } catch (...) { return false; } }
    const std::string bs = t.substr(npos + 1);
    if (bs.empty()) { *outB = 0; return true; }
    if (bs[0] != '+' && bs[0] != '-') return false;
    try { *outB = std::stoi(bs); } catch (...) { return false; }
    return true;
}

// An+B 是否命中第 idx1 项（idx1 从 1 开始）
static bool NthMatch(int a, int b, int idx1) {
    if (a == 0) return idx1 == b;
    const int d = idx1 - b;
    return (d % a) == 0 && (d / a) >= 0;
}

// 单个"简单选择器段"：tag / .a.b / #id / [attr=v] / :pseudo(...) 的任意组合
// 例："input.btn.primary:focus"、"a[href][target=_blank]"
static bool ParseSimpleSelFull(const std::string& one, CssRule::Simple& out, std::string& why) {
    const std::string s = TrimCopy(one);
    why.clear();
    if (s.empty()) { why = "为空"; return false; }
    if (s == "*") return true;

    size_t p = 0;
    auto isNameChar = [](char c) {
        return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '*' ||
               static_cast<unsigned char>(c) >= 0x80;
    };

    // 标签名（可省略）
    if (s[p] != '.' && s[p] != '#' && s[p] != '[' && s[p] != ':') {
        const size_t st = p;
        while (p < s.size() && isNameChar(s[p])) p++;
        if (p == st) { why = "标签名无法解析"; return false; }
        out.tag = LowerCopy(s.substr(st, p - st));
        if (out.tag != "*") out.spec += 1;
        else out.tag.clear();
    }

    while (p < s.size()) {
        const char c = s[p];
        if (c == '.') {
            const size_t st = ++p;
            while (p < s.size() && isNameChar(s[p]) && s[p] != '*') p++;
            if (p == st) { why = "类名为空"; return false; }
            out.classes.push_back(s.substr(st, p - st));
            out.spec += 10;
        } else if (c == '#') {
            const size_t st = ++p;
            while (p < s.size() && isNameChar(s[p]) && s[p] != '*') p++;
            if (p == st) { why = "id 为空"; return false; }
            out.id = s.substr(st, p - st);
            out.spec += 100;
        } else if (c == '[') {
            const size_t close = s.find(']', p);
            if (close == std::string::npos) { why = "属性选择器缺少 ]"; return false; }
            std::string inner = TrimCopy(s.substr(p + 1, close - p - 1));
            p = close + 1;
            CssAttrSel a;
            // 运算符：= ^= $= *= ~= |=
            size_t op = std::string::npos;
            for (size_t k = 1; k < inner.size(); k++) {
                if (inner[k] == '=') {
                    const char prev = inner[k - 1];
                    if (prev == '^' || prev == '$' || prev == '*' || prev == '~' || prev == '|') {
                        a.name = TrimCopy(inner.substr(0, k - 1));
                        a.op = std::string(1, prev) + "=";
                    } else {
                        a.name = TrimCopy(inner.substr(0, k));
                        a.op = "=";
                    }
                    op = k;
                    break;
                }
            }
            if (op == std::string::npos) {
                a.name = inner;
            } else {
                a.val = TrimCopy(inner.substr(op + 1));
                if (a.val.size() >= 2 &&
                    ((a.val.front() == '"' && a.val.back() == '"') ||
                     (a.val.front() == '\'' && a.val.back() == '\'')))
                    a.val = a.val.substr(1, a.val.size() - 2);
            }
            if (a.name.empty()) { why = "属性选择器的属性名为空"; return false; }
            if (a.name.size() > 2 && a.name[0] == '-' && a.name[1] == '-') {
                why = "不支持按自定义属性（--x）选择";
                return false;
            }
            a.name = LowerCopy(a.name);
            out.attrs.push_back(std::move(a));
            out.spec += 10;
        } else if (c == ':') {
            const bool isElem = (p + 1 < s.size() && s[p + 1] == ':');
            p += isElem ? 2 : 1;
            const size_t ns = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '-')) p++;
            const std::string name = LowerCopy(s.substr(ns, p - ns));
            std::string arg;
            if (p < s.size() && s[p] == '(') {
                const size_t as = ++p;
                int depth = 1;
                while (p < s.size() && depth > 0) {
                    if (s[p] == '(') depth++;
                    else if (s[p] == ')') { if (--depth == 0) break; }
                    p++;
                }
                arg = s.substr(as, (p < s.size()) ? (p - as) : std::string::npos);
                if (p < s.size()) p++;
            }
            if (name.empty()) { why = "伪类名为空"; return false; }
            if (isElem) {
                // 伪元素只在最后一段出现，交给 ParseCssSelector 处理
                why = "伪元素位置错误";
                return false;
            }
            CssPseudo ps;
            ps.spec = 10;
            if (name == "hover")        ps.kind = CssPseudo::HOVER;
            else if (name == "active")  ps.kind = CssPseudo::ACTIVE;
            else if (name == "focus")   ps.kind = CssPseudo::FOCUS;
            else if (name == "checked") ps.kind = CssPseudo::CHECKED;
            else if (name == "first-child")     { ps.kind = CssPseudo::FIRST_CHILD; }
            else if (name == "last-child")      { ps.kind = CssPseudo::LAST_CHILD; }
            else if (name == "only-child")      { ps.kind = CssPseudo::ONLY_CHILD; }
            else if (name == "first-of-type")   { ps.kind = CssPseudo::FIRST_OF_TYPE; }
            else if (name == "last-of-type")    { ps.kind = CssPseudo::LAST_OF_TYPE; }
            else if (name == "only-of-type")    { ps.kind = CssPseudo::ONLY_OF_TYPE; }
            else if (name == "nth-child" || name == "nth-last-child" ||
                     name == "nth-of-type" || name == "nth-last-of-type") {
                if (!ParseNthExpr(arg, &ps.a, &ps.b)) {
                    why = "伪类 :" + name + "(" + arg + ") 的 An+B 表达式无法解析";
                    return false;
                }
                ps.kind = (name == "nth-child") ? CssPseudo::NTH_CHILD
                        : (name == "nth-last-child") ? CssPseudo::NTH_LAST_CHILD
                        : (name == "nth-of-type") ? CssPseudo::NTH_OF_TYPE
                        : CssPseudo::NTH_LAST_OF_TYPE;
            } else if (name == "not") {
                // :not(简单选择器段)：内层也允许 .a.b / [attr]
                CssRule::Simple inner;
                std::string w2;
                if (!ParseSimpleSelFull(arg, inner, w2) || inner.empty()) {
                    why = "伪类 :not(" + arg + ") 只支持内嵌简单选择器（tag/.class/#id/[attr]）";
                    return false;
                }
                ps.kind = CssPseudo::NOT_SEL;
                ps.notTag = inner.tag;
                ps.notId = inner.id;
                ps.notClasses = inner.classes;
                ps.notAttrs.clear();
                for (const auto& a : inner.attrs)
                    ps.notAttrs.push_back({ a.name, a.op, a.val });
                ps.spec = inner.spec;
            } else if (name == "root") {
                ps.kind = CssPseudo::ROOT;
            } else {
                why = "伪类 :" + name + " 暂不支持";
                return false;
            }
            if (ps.kind <= CssPseudo::CHECKED) {
                int runtimeCount = 0;
                for (const auto& q : out.pseudos)
                    if (q.kind <= CssPseudo::CHECKED) runtimeCount++;
                if (++runtimeCount > 1) {
                    why = "同一选择器出现多个运行时伪类（:hover/:active/:focus/:checked 只支持一个）";
                    return false;
                }
            }
            out.pseudos.push_back(ps);
            out.spec += ps.spec;
        } else {
            why = std::string("无法识别选择器字符 '") + c + "'";
            return false;
        }
    }
    return !out.empty();
}

// 复杂选择器：按组合子切成若干净段（后代 ' '/子 '>' /相邻 '+' /兄弟 '~'）
static bool SplitComplexSelector(const std::string& in,
                                 std::vector<std::pair<char, std::string>>& out,
                                 std::string& why) {
    std::string cur;
    char pending = '|';        // 与"左邻"的关系；'|' = 首段
    int depth = 0;             // () 与 [] 内的组合子不算
    auto flush = [&]() -> bool {
        const std::string t = TrimCopy(cur);
        cur.clear();
        if (t.empty()) return true;
        out.emplace_back(pending == '|' && !out.empty() ? ' ' : pending, t);
        pending = ' ';
        return true;
    };
    for (char c : in) {
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth > 0) depth--; }
        if (depth == 0) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!TrimCopy(cur).empty()) flush();
                continue;
            }
            if (c == '>' || c == '+' || c == '~') {
                if (!flush()) return false;
                if (TrimCopy(cur).empty() && out.empty()) { why = "组合子出现在选择器开头"; return false; }
                pending = c;
                continue;
            }
        }
        cur += c;
    }
    if (!TrimCopy(cur).empty()) flush();
    if (out.empty()) { why = "为空"; return false; }
    // 首段的 comb 必须是 '|'
    out.front().first = '|';
    return true;
}

// 解析单个复杂选择器（含组合子/复合类/属性选择器/伪类/伪元素）
static bool ParseCssSelector(const std::string& oneIn, CssRule& r, std::string& why) {
    const std::string one = TrimCopy(oneIn);
    why.clear();
    if (one.empty()) { why = "为空"; return false; }

    std::vector<std::pair<char, std::string>> raw;
    if (!SplitComplexSelector(one, raw, why)) return false;

    // 伪元素拆出来（只能出现在最右段的末尾）
    std::string lastText = raw.back().second;
    std::string pseudoEl;
    {
        // 找最后一个顶层的 "::name"（不在 ()/[] 内）
        int depth = 0;
        size_t at = std::string::npos;
        for (size_t i = 0; i + 1 < lastText.size(); i++) {
            if (lastText[i] == '(' || lastText[i] == '[') depth++;
            else if (lastText[i] == ')' || lastText[i] == ']') { if (depth > 0) depth--; }
            if (depth == 0 && lastText[i] == ':' && lastText[i + 1] == ':') at = i;
        }
        if (at != std::string::npos) {
            const std::string after = lastText.substr(at + 2);
            std::string name;
            size_t q = 0;
            while (q < after.size() && (std::isalnum((unsigned char)after[q]) || after[q] == '-')) q++;
            name = LowerCopy(after.substr(0, q));
            if (name == "before" || name == "after") {
                pseudoEl = name;
                lastText = lastText.substr(0, at);
                if (lastText.empty()) lastText = "*";
                r.spec += 1;
            } else {
                why = "伪元素 ::" + name + " 没有对应渲染阶段（只支持 ::before/::after）";
                return false;
            }
        }
    }

    for (size_t k = 0; k < raw.size(); k++) {
        CssRule::Part part;
        part.comb = raw[k].first;
        const std::string& text = (k + 1 == raw.size()) ? lastText : raw[k].second;
        std::string w2;
        if (!ParseSimpleSelFull(text, part.sel, w2)) {
            why = "的 " + text + " 段" + w2;
            return false;
        }
        r.spec += part.sel.spec;
        r.parts.push_back(std::move(part));
    }
    if (r.parts.empty()) { why = "没有可匹配的简单选择器"; return false; }
    r.pseudoEl = pseudoEl;
    return true;
}


// 规则是否带指定运行时伪类（:hover/:active/:focus/:checked；任一段里出现即算）
static bool CssHasStatePseudo(const CssRule& r, uint8_t bit) {
    for (const auto& part : r.parts) {
        for (const auto& ps : part.sel.pseudos) {
            if (bit == BINUI_ST_HOVER   && ps.kind == CssPseudo::HOVER)   return true;
            if (bit == BINUI_ST_ACTIVE  && ps.kind == CssPseudo::ACTIVE)  return true;
            if (bit == BINUI_ST_FOCUS   && ps.kind == CssPseudo::FOCUS)   return true;
            if (bit == BINUI_ST_CHECKED && ps.kind == CssPseudo::CHECKED) return true;
        }
    }
    return false;
}

// class="a b c" 里是否含 want（空白分隔 token）
static bool CssClassHit(const std::string& cls, const std::string& want) {
    size_t p = 0;
    while (p < cls.size()) {
        const size_t e = cls.find_first_of(" \t", p);
        const std::string tok = cls.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
        if (tok == want) return true;
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return false;
}

// 属性选择器是否命中
static bool CssAttrMatches(const CssAttrSel& a, const CssElemCtx& c) {
    if (!c.attrs) return false;
    auto it = c.attrs->find(a.name);
    if (it == c.attrs->end()) return false;
    if (a.op.empty()) return true;
    const std::string& v = it->second;
    if (a.op == "=")  return v == a.val;
    if (a.op == "^=") return !a.val.empty() && v.rfind(a.val, 0) == 0;
    if (a.op == "$=") return !a.val.empty() && v.size() >= a.val.size() &&
                             v.compare(v.size() - a.val.size(), a.val.size(), a.val) == 0;
    if (a.op == "*=") return !a.val.empty() && v.find(a.val) != std::string::npos;
    if (a.op == "~=") return !a.val.empty() && CssClassHit(v, a.val);
    if (a.op == "|=") return v == a.val ||
                             (v.size() > a.val.size() &&
                              v.compare(0, a.val.size() + 1, a.val + "-") == 0);
    return false;
}

// 单个"简单选择段"是否命中：tag / #id / .a.b / [attr] / 伪类求值
// （组合子与祖先/兄弟关系由 CssRuleMatchesDom 处理，不在这里做）
static bool CssSimpleMatches(const CssRule::Simple& sel, const CssElemCtx& c) {
    if (!sel.id.empty() && sel.id != c.id) return false;
    if (!sel.tag.empty() && sel.tag != c.tag) return false;
    for (const auto& want : sel.classes)
        if (!CssClassHit(c.cls, want)) return false;
    for (const auto& a : sel.attrs)
        if (!CssAttrMatches(a, c)) return false;

    for (const auto& ps : sel.pseudos) {
        switch (ps.kind) {
            case CssPseudo::HOVER:   if (!(c.stateMask & BINUI_ST_HOVER))   return false; break;
            case CssPseudo::ACTIVE:  if (!(c.stateMask & BINUI_ST_ACTIVE))  return false; break;
            case CssPseudo::FOCUS:   if (!(c.stateMask & BINUI_ST_FOCUS))   return false; break;
            case CssPseudo::CHECKED: if (!(c.stateMask & BINUI_ST_CHECKED)) return false; break;
            case CssPseudo::FIRST_CHILD:  if (c.idx != 0) return false; break;
            case CssPseudo::LAST_CHILD:   if (c.idx < 0 || c.idx != c.cnt - 1) return false; break;
            case CssPseudo::ONLY_CHILD:   if (c.idx != 0 || c.cnt != 1) return false; break;
            case CssPseudo::NTH_CHILD:
                if (c.idx < 0 || !NthMatch(ps.a, ps.b, c.idx + 1)) return false;
                break;
            case CssPseudo::NTH_LAST_CHILD:
                if (c.idx < 0 || !NthMatch(ps.a, ps.b, c.cnt - c.idx)) return false;
                break;
            case CssPseudo::FIRST_OF_TYPE: if (c.typeIdx != 0) return false; break;
            case CssPseudo::LAST_OF_TYPE:
                if (c.typeIdx < 0 || c.typeIdx != c.typeCnt - 1) return false;
                break;
            case CssPseudo::ONLY_OF_TYPE:
                if (c.typeIdx != 0 || c.typeCnt != 1) return false;
                break;
            case CssPseudo::NTH_OF_TYPE:
                if (c.typeIdx < 0 || !NthMatch(ps.a, ps.b, c.typeIdx + 1)) return false;
                break;
            case CssPseudo::NTH_LAST_OF_TYPE:
                if (c.typeIdx < 0 || !NthMatch(ps.a, ps.b, c.typeCnt - c.typeIdx)) return false;
                break;
            case CssPseudo::NOT_SEL: {
                // 内层全部命中 → 整体不命中
                bool hit = true;
                if (!ps.notId.empty() && ps.notId != c.id) hit = false;
                if (hit && !ps.notTag.empty() && ps.notTag != c.tag) hit = false;
                if (hit)
                    for (const auto& want : ps.notClasses)
                        if (!CssClassHit(c.cls, want)) { hit = false; break; }
                if (hit)
                    for (const auto& a : ps.notAttrs)
                        if (!CssAttrMatches(a, c)) { hit = false; break; }
                if (hit) return false;
                break;
            }
            case CssPseudo::ROOT: if (!c.isRoot) return false; break;
        }
    }
    return true;
}

static std::string CssStripComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            i = (e == std::string::npos) ? s.size() : e + 2;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// "a:b; c:d" -> {a:b, c:d}（去掉 !important，本编译器不区分优先级）
static std::unordered_map<std::string, std::string> ParseDeclBlock(const std::string& body) {
    std::unordered_map<std::string, std::string> out;
    size_t p = 0;
    while (p < body.size()) {
        const size_t semi = body.find(';', p);
        const std::string decl = body.substr(p, (semi == std::string::npos) ? std::string::npos : semi - p);
        const size_t colon = decl.find(':');
        if (colon != std::string::npos) {
            std::string k = LowerCopy(TrimCopy(decl.substr(0, colon)));
            std::string v = TrimCopy(decl.substr(colon + 1));
            const size_t bang = v.rfind('!');
            if (bang != std::string::npos &&
                LowerCopy(TrimCopy(v.substr(bang + 1))).rfind("important", 0) == 0) {
                v = TrimCopy(v.substr(0, bang));
            }
            if (!k.empty() && !v.empty()) out[k] = v;
        }
        if (semi == std::string::npos) break;
        p = semi + 1;
    }
    return out;
}

// ============================================================
// 内置 UA 默认样式（浏览器 UA stylesheet 的等价物）
// ------------------------------------------------------------
// 只管"作者什么都没写时该长什么样"：优先级低于任何作者声明（<style> 规则与内联
// style 都会覆盖它），所以对已有界面零影响 —— 除非那个界面本来就是靠"没默认值"
// 才成立的（v13 之前根底默认白、文字色默认白，于是白底白字；这就是要修的东西）。
// 字号不放在这里：h1~h6/small 的基准字号由 defaultHeight/字号表统一给，
// 放两处会出现 em 换算基准不一致。
// ============================================================
static const std::unordered_map<std::string, std::string>& UaDeclsOf(const std::string& tag) {
    static const std::unordered_map<std::string, std::unordered_map<std::string, std::string>> table = [] {
        static const std::pair<const char*, const char*> raw[] = {
            // 标题：加粗（字号见字号表）
            { "h1", "font-weight:700" }, { "h2", "font-weight:700" },
            { "h3", "font-weight:700" }, { "h4", "font-weight:700" },
            { "h5", "font-weight:700" }, { "h6", "font-weight:700" },
            // 行内语义
            { "b", "font-weight:700" },      { "strong", "font-weight:700" },
            { "th", "font-weight:700;text-align:center" },
            { "i", "font-style:italic" },    { "em", "font-style:italic" },
            { "cite", "font-style:italic" }, { "var", "font-style:italic" },
            { "dfn", "font-style:italic" },
            { "u", "text-decoration:underline" },   { "ins", "text-decoration:underline" },
            { "s", "text-decoration:line-through" },{ "del", "text-decoration:line-through" },
            { "strike", "text-decoration:line-through" },
            // 高亮：浅色底 + 深色字，深色主题下同样可读
            { "mark", "background:#FFF59D;color:#1A1A1A" },
            // 分隔线：v13 容器默认透明后 <hr> 变成隐形，这里给它一条 2px 中灰
            { "hr", "height:2px;background:#8A8A94" },
            // 链接：只加下划线；颜色跟随继承（深色主题里蓝字反而看不清）
            { "a", "text-decoration:underline" },
            // 上下标：没有基线偏移能力，至少按小字号行内排（见 README 已知限制）
            { "sub", "font-size:0.75em" }, { "sup", "font-size:0.75em" },
            // 预排版：保留原始换行与缩进（否则代码块被折叠成一行）
            { "pre", "white-space:pre" },
        };
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m;
        for (const auto& r : raw) m[r.first] = ParseDeclBlock(r.second);
        return m;
    }();
    static const std::unordered_map<std::string, std::string> empty;
    auto it = table.find(tag);
    return it == table.end() ? empty : it->second;
}

// HTML 属性白名单：真正被编译器消费的属性。其余一律视为"写了没效果"并告警。
// 也是 <use> 参数过滤的依据：组件参数名是任意标识符，除了这些有语义的名字之外，
// 都不该被当成属性挂到替换出来的节点上（否则会冒出一堆假属性）。
static const std::unordered_set<std::string>& UsedHtmlAttrs() {
    static const std::unordered_set<std::string> kSet = {
        "id", "class", "style", "type", "value", "placeholder", "src", "image", "alt",
        "checked", "disabled", "readonly", "href", "title",
        "x", "y", "w", "h", "width", "height", "z", "z-index", "radius",
        "bg", "background", "background-color", "color", "text-color",
        "size", "font-size", "weight", "font-weight", "font-style", "line-height",
        "text-decoration", "border-radius", "border", "border-width", "border-color",
        "border-top", "border-right", "border-bottom", "border-left",
        "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
        "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
        "min-width", "max-width", "min-height", "max-height",
        "display", "position", "left", "top", "right", "bottom", "overflow", "opacity", "align",
        // v30：inset 四值简写 + 长属性（贴右/贴下）
        "inset", "inset-top", "inset-right", "inset-bottom", "inset-left",
        "resize",
        "flex", "flex-direction", "justify-content", "align-items", "gap",
        // v22：flex 属性补齐（简写/收缩/基准/换行/逐项对齐/排序）
        "flex-wrap", "flex-shrink", "flex-grow", "flex-basis",
        "align-content", "align-self", "order",
        "animation", "transition", "transform", "white-space",
        "aspect", "resizable", "window_resize", "transparent", "ui_scale",
        "volume", "autoplay", "loop", "muted", "controls",
        "points", "stroke", "stroke-width", "fill", "rx", "cx", "cy", "r",
        "x1", "y1", "x2", "y2",
        // v15 SVG 路径：d 是路径数据；其余是描边/填充规则的表现属性
        "d", "ry", "fill-rule", "fill-opacity", "stroke-opacity",
        "stroke-linecap", "stroke-linejoin", "stroke-dasharray", "stroke-dashoffset", "stroke-miterlimit",
        "shape-rendering",
        "viewbox", "preserveaspectratio", "xlink:href",
        // v16 SVG 渐变：端点/圆心/焦点/色标/单位/变换/spread（x1/y1/x2/y2/cx/cy/r 上面已有）
        "fx", "fy", "offset", "stop-color", "stop-opacity",
        // v0.16.23：SVG <marker>（编译期展开）
        "marker", "marker-start", "marker-mid", "marker-end",
        "clip-path", "clippathunits",
        // v0.16.25：SVG <pattern>
        "patternunits", "patterncontentunits", "patterntransform",
        // v0.16.26：SVG <mask>（v0.16.54 起也认 mask-type）
        "mask", "maskunits", "maskcontentunits", "mask-type",
        // v0.16.49：九宫格中段填充模式
        "image-slice", "nine-slice", "border-image-repeat",
        // v0.16.27：CSS backdrop-filter
        "backdrop-filter", "material",
        "markerwidth", "markerheight", "markerunits", "refx", "refy", "orient",
        "gradientunits", "gradienttransform", "spreadmethod", "text-anchor",
        // v0.16.47：SVG <filter>（含不支持的原语的属性 —— 那些原语本身会单独告警一次，
        // 不再逐属性刷屏）
        "filter", "stddeviation", "flood-color", "flood-opacity", "in", "in2", "result",
        "operator", "dx", "dy", "mode", "values", "tablevalues", "slope", "intercept",
        "amplitude", "exponent", "scale", "radius", "azimuth", "elevation", "surfacescale",
        "specularconstant", "specularexponent", "k1", "k2", "k3", "k4", "preservealpha",
        "basefrequency", "numoctaves", "stitchtiles", "seed", "targetx", "targety",
    };
    return kSet;
}

// 标签的基准字号（HTML 标题梯度）。em 换算与"未声明 font-size"共用这一份口径 ——
// 以前三处各写一遍，改一处忘两处就会出现"em 按 14 算、实际字号 20"的错位。
static float DefaultFontSizeOf(const std::string& tag) {
    if (tag == "h1") return 28.0f;
    if (tag == "h2") return 24.0f;
    if (tag == "h3") return 20.0f;
    if (tag == "h4") return 18.0f;
    if (tag == "h5") return 16.0f;
    if (tag == "h6") return 14.0f;
    if (tag == "small") return 12.0f;
    return 14.0f;
}

static void ParseCssRules(const std::string& cssRaw, std::vector<CssRule>& out) {
    const std::string css = CssStripComments(cssRaw);
    auto startsWithNoCase = [](const std::string& s, const char* prefix) {
        for (size_t i = 0; prefix[i]; i++) {
            if (i >= s.size()) return false;
            if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i]))
                return false;
        }
        return true;
    };
    size_t i = 0;
    while (i < css.size()) {
        const size_t brace = css.find('{', i);
        if (brace == std::string::npos) break;
        const std::string sel = TrimCopy(css.substr(i, brace - i));
        // 找到块结束（数花括号，@media 里还能嵌规则块）
        size_t j = brace + 1;
        int depth = 1;
        while (j < css.size() && depth > 0) {
            if (css[j] == '{') depth++;
            else if (css[j] == '}') depth--;
            j++;
        }
        const std::string body =
            css.substr(brace + 1, (j >= brace + 2) ? (j - brace - 2) : 0);
        i = j;
        if (sel.empty()) continue;
        if (sel[0] == '@') {   // at-rule 整块跳过（@keyframes/@media/...）
            if (startsWithNoCase(sel, "@media"))
                Warn("CSS @media 暂不支持（编译期不知道运行时尺寸），整块已忽略；"
                     "自适应请用百分比/流式/flex");
            continue;
        }

        // 选择器组：".a, .b"
        size_t sp = 0;
        while (sp <= sel.size()) {
            const size_t comma = sel.find(',', sp);
            const std::string one =
                TrimCopy(sel.substr(sp, (comma == std::string::npos) ? std::string::npos : comma - sp));
            sp = (comma == std::string::npos) ? sel.size() + 1 : comma + 1;
            if (one.empty()) continue;
            CssRule r;
            std::string why;
            if (!ParseCssSelector(one, r, why)) {
                Warn("CSS 选择器 \"" + one + "\" " + why + "，已忽略");
                continue;
            }
            r.decls = ParseDeclBlock(body);
            if (r.decls.empty()) continue;
            r.order = out.size();
            out.push_back(std::move(r));
        }
    }
}

// ============================================================
// HTML5 -> binUI
// 支持显式 x/y/w/h 或 style 绝对定位；未定位的元素按流式自上而下堆叠
// ============================================================
// ============================================================
// 片段解析：HTML/XML 源码 → 扁平元素列表（深度优先顺序，父索引恒小于子索引）
// 编译器主体与 <use> 组件内联共用同一份容错解析逻辑。
// styleSheets 追加收集 <style> 文本（组件自带的 CSS 也会并进宿主样式表）。
// ============================================================
struct HNode {
    std::string tag;
    std::unordered_map<std::string, std::string> attrs;
    std::string text;      // 空白折叠后的文本（HTML 默认排版语义）
    std::string rawText;   // 原样文本（含换行/缩进，供 white-space:pre / <pre> 使用）
    size_t parentIdx;   // SIZE_MAX = 无父（顶层）
};

static std::vector<HNode> ParseElements(const std::string& html, std::string& styleSheets) {
    const std::unordered_set<std::string> voidTags = {
        "br", "hr", "img", "input", "meta", "link", "area", "base",
        "col", "embed", "source", "track", "wbr"
    };

    auto lowerStr = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return s;
    };

    std::vector<HNode> elems;
    std::vector<size_t> openStack;

    auto appendText = [&](const std::string& raw) {
        std::string t;
        bool space = false;
        for (char ch : raw) {
            if (ch==' '||ch=='\t'||ch=='\n'||ch=='\r') {
                if (!t.empty()) space = true;
            } else {
                if (space && !t.empty()) t += ' ';
                space = false;
                t += ch;
            }
        }
        if (space) t += ' '; // 保留段尾空格：行内文本跨元素拼接不丢空格
        if (!t.empty() && !openStack.empty())
            elems[openStack.back()].text += DecodeEntities(t);
        // 原样副本：<pre> / white-space:pre 的元素用这份（缩进与换行是排版的一部分）
        if (!raw.empty() && !openStack.empty())
            elems[openStack.back()].rawText += DecodeEntities(raw);
    };

    auto parseAttrs = [&](const std::string& s) -> std::unordered_map<std::string, std::string> {
        std::unordered_map<std::string, std::string> out;
        size_t p = 0;
        auto isSpace = [](char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; };
        while (p < s.size()) {
            while (p < s.size() && isSpace(s[p])) p++;
            if (p >= s.size()) break;
            size_t start = p;
            while (p < s.size() && s[p] != '=' && !isSpace(s[p])) p++;
            std::string key = s.substr(start, p - start);
            while (p < s.size() && isSpace(s[p])) p++;
            std::string value;
            if (p < s.size() && s[p] == '=') {
                p++;
                while (p < s.size() && isSpace(s[p])) p++;
                if (p < s.size() && (s[p]=='"' || s[p]=='\'')) {
                    char q = s[p];
                    size_t q2 = s.find(q, p + 1);
                    value = (q2 == std::string::npos) ? s.substr(p + 1) : s.substr(p + 1, q2 - p - 1);
                    p = (q2 == std::string::npos) ? s.size() : q2 + 1;
                } else {
                    size_t e = p;
                    while (e < s.size() && !isSpace(s[e])) e++;
                    value = s.substr(p, e - p);
                    p = e;
                }
            }
            key = lowerStr(key);
            if (!key.empty()) out[key] = DecodeEntities(value);
        }
        return out;
    };

    size_t i = 0;
    while (i < html.size()) {
        size_t lt = html.find('<', i);
        if (lt == std::string::npos) { appendText(html.substr(i)); break; }
        if (lt > i) appendText(html.substr(i, lt - i));
        if (lt + 1 >= html.size()) break;

        char c = html[lt + 1];
        if (c == '!') {
            if (html.compare(lt, 4, "<!--") == 0) {
                size_t close = html.find("-->", lt + 4);
                i = (close == std::string::npos) ? html.size() : close + 3;
            } else {
                size_t close = html.find('>', lt);
                i = (close == std::string::npos) ? html.size() : close + 1;
            }
            continue;
        }
        if (c == '/') {
            size_t gt = html.find('>', lt);
            std::string name = (gt == std::string::npos)
                ? html.substr(lt + 2)
                : html.substr(lt + 2, gt - lt - 2);
            name = lowerStr(name);
            for (size_t k = openStack.size(); k > 0; --k) {
                if (elems[openStack[k - 1]].tag == name) {
                    openStack.resize(k - 1);
                    break;
                }
            }
            i = (gt == std::string::npos) ? html.size() : gt + 1;
            continue;
        }

        size_t gt = html.find('>', lt);

        std::string raw = html.substr(lt + 1, gt - lt - 1);
        bool selfClose = !raw.empty() && raw.back() == '/';
        if (selfClose) raw.pop_back();

        size_t sp = 0;
        while (sp < raw.size() && (raw[sp]==' '||raw[sp]=='\t'||raw[sp]=='\n'||raw[sp]=='\r')) sp++;
        size_t te = sp;
        while (te < raw.size() && raw[te]!=' ' && raw[te]!='\t' && raw[te]!='\n' && raw[te]!='\r') te++;
        std::string tag = lowerStr(raw.substr(sp, te - sp));
        i = gt + 1;

        if (tag.empty()) continue;
        // 容错：片段跨过闭合标签（含 <）或 tag 非字母开头（如 "5 < 3 > 1"）→ 当文本
        bool looksLikeTag = true;
        for (size_t ci = 0; ci < raw.size(); ci++) {
            if (raw[ci] == '<') { looksLikeTag = false; break; }
        }
        if (looksLikeTag && !tag.empty()) {
            char c0 = tag[0];
            if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z')))
                looksLikeTag = false;
        }
        if (!looksLikeTag) {
            appendText("<");
            i = lt + 1; // 从 < 之后重新解析，闭合标签等仍正常处理
            continue;
        }
        if (tag == "script" || tag == "style") {
            std::string closeTag = "</" + tag;
            size_t cs = html.find(closeTag, i);
            std::string content;
            if (cs == std::string::npos) {
                content = html.substr(i);
                i = html.size();
            } else {
                content = html.substr(i, cs - i);
                size_t gt = html.find('>', cs);
                i = (gt == std::string::npos) ? html.size() : gt + 1;
            }
            if (tag == "style") styleSheets += content;
            continue;
        }

        // <br>：硬换行——在父元素文本里插入 '\n'，不生成节点
        // （渲染端 LABEL/LIST 的 drawTextWrapped 已按 '\n' 断行）
        if (tag == "br") {
            if (!openStack.empty()) elems[openStack.back()].text += '\n';
            continue;
        }

        HNode node;
        node.tag = tag;
        node.attrs = parseAttrs(raw.substr(te));
        node.parentIdx = openStack.empty() ? SIZE_MAX : openStack.back();
        elems.push_back(node);

        bool isVoid = selfClose || voidTags.count(tag) > 0;
        if (!isVoid) openStack.push_back(elems.size() - 1);
    }
    return elems;
}

// ============================================================
// 组件内联：<use src="路径.bum.html"> / <component src=…>
//
// 语义（见 docs/components.md）：
//   · src 相对"当前 UI 文件所在目录"解析；按 include/binui_asset.h 的规则选文件
//     （二进制 .bum/.bui 优先，源码更新时改用源码并告警）
//   · 源码组件：{{参数名}} 用 <use> 上的同名属性替换；未提供则替换为空并警告；
//     组件顶层元素视为"组件根"，其属性合入替换节点，<use> 自身属性优先
//   · 二进制组件：留占位节点，编译期由 binui_link 离线合并（与运行期挂载同一套规则）
//   · <use ... external>：只留占位节点 + 组件引用段，运行期 ex::MountAll() 挂载
//   · 组件自带的 <style> 并进宿主样式表：组件可被宿主主题化（也意味着选择器会互相影响，
//     建议组件内用带前缀的类名）
//   · 嵌套组件递归展开，同一文件引用多次就展开多份（每个实例独立）
// ============================================================
// ============================================================
// 选择器匹配（需要元素树）：从最右段往左走组合子
//   parts[i].comb 表示第 i 段与第 i-1 段的关系（' ' 后代 / '>' 子 / '+' 相邻 / '~' 兄弟）
//   kids[父] = 会参与布局的子元素（与结构伪类同一口径）
// ============================================================
static std::vector<std::vector<size_t>> BuildKidIndex(const std::vector<HNode>& elems) {
    auto nonVisual = [](const std::string& tag) {
        return tag == "head" || tag == "title" || tag == "meta" || tag == "link" ||
               tag == "script" || tag == "style" || tag == "app_window";
    };
    std::vector<std::vector<size_t>> kids(elems.size());
    for (size_t i = 0; i < elems.size(); i++) {
        if (nonVisual(elems[i].tag)) continue;
        if (elems[i].attrs.count("_pseudo")) continue;   // 合成的伪元素盒不算元素（与浏览器一致）
        const size_t p = elems[i].parentIdx;
        if (p < elems.size()) kids[p].push_back(i);
    }
    return kids;
}

static bool CssRuleMatchesDom(const CssRule& r,
                              const std::vector<HNode>& elems,
                              const std::vector<std::vector<size_t>>& kids,
                              const std::vector<CssElemCtx>& ctxs,
                              size_t ei,
                              int stateOverride = 0) {
    if (r.parts.empty() || ei >= elems.size()) return false;
    const auto& parts = r.parts;

    // 同一父下的前一个可视兄弟
    auto prevSibling = [&](size_t at) -> size_t {
        const size_t par = elems[at].parentIdx;
        if (par < kids.size()) {
            const auto& kv = kids[par];
            for (size_t k = 0; k < kv.size(); k++) {
                if (kv[k] != at) continue;
                return (k > 0) ? kv[k - 1] : SIZE_MAX;
            }
            return SIZE_MAX;
        }
        size_t found = SIZE_MAX;   // 没建索引（理论上不会）：线性兜底
        for (size_t k = 0; k < elems.size() && k < at; k++)
            if (elems[k].parentIdx == par && elems[k].parentIdx < elems.size()) found = k;
        return found;
    };

    std::function<bool(size_t, size_t)> match = [&](size_t pi, size_t at) -> bool {
        if (at >= elems.size()) return false;
        CssElemCtx c = (at < ctxs.size()) ? ctxs[at] : CssElemCtx{};
        if (c.tag.empty() && c.cls.empty() && c.id.empty()) {
            c.tag = elems[at].tag;
            auto ci = elems[at].attrs.find("class");
            c.cls = (ci != elems[at].attrs.end()) ? ci->second : "";
            auto ii = elems[at].attrs.find("id");
            c.id = (ii != elems[at].attrs.end()) ? ii->second : "";
        }
        c.attrs = &elems[at].attrs;
        if (stateOverride) c.stateMask = stateOverride;
        if (!CssSimpleMatches(parts[pi].sel, c)) return false;
        if (pi == 0) return true;

        const char comb = parts[pi].comb;
        if (comb == '>') {
            const size_t par = elems[at].parentIdx;
            return par < elems.size() && match(pi - 1, par);
        }
        if (comb == ' ') {
            for (size_t p2 = elems[at].parentIdx; p2 < elems.size(); p2 = elems[p2].parentIdx) {
                if (match(pi - 1, p2)) return true;
                if (elems[p2].parentIdx == SIZE_MAX) break;
            }
            return false;
        }
        if (comb == '+') {
            const size_t prev = prevSibling(at);
            return prev != SIZE_MAX && match(pi - 1, prev);
        }
        if (comb == '~') {
            for (size_t p2 = prevSibling(at); p2 != SIZE_MAX; p2 = prevSibling(p2))
                if (match(pi - 1, p2)) return true;
            return false;
        }
        return false;
    };
    return match(parts.size() - 1, ei);
}

static bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// {{name}} → <use> 上的同名属性值（未提供则空串 + 警告）
// defaultsOnly=true 时只替换带默认值的形式 {{name=默认}}：用于"把组件源码
// 单独编成 .bum"——参数取默认值，裸 {{name}} 原样留给内联时替换。
static std::string SubstituteParams(const std::string& src,
                                    const std::unordered_map<std::string, std::string>& params,
                                    bool defaultsOnly = false) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ) {
        if (src[i] == '{' && i + 1 < src.size() && src[i + 1] == '{') {
            const size_t end = src.find("}}", i + 2);
            if (end != std::string::npos) {
                // 支持 {{name}} 与 {{name=默认值}}；默认值让"可选参数"不再每处都报警告
                const std::string inner = TrimCopy(src.substr(i + 2, end - i - 2));
                const size_t eq = inner.find('=');
                const std::string key = LowerCopy(TrimCopy(eq == std::string::npos
                                                            ? inner : inner.substr(0, eq)));
                const std::string fallback = (eq == std::string::npos)
                                             ? std::string() : TrimCopy(inner.substr(eq + 1));
                auto it = params.find(key);
                const std::string* val = nullptr;
                if (it != params.end()) val = &it->second;
                else if (eq != std::string::npos) val = &fallback;
                if (defaultsOnly && eq == std::string::npos) {
                    out += src.substr(i, end + 2 - i);   // 裸 {{name}}：留给内联时替换
                    i = end + 2;
                    continue;
                }
                if (val) {
                    // 参数值按纯文本注入：转义 & < > —— 否则传进来的值里若含 <use> 之类的标签，
                    // 会被后续解析当成真元素（踩过一次：body="内部 <use> …" 凭空多出一个节点）
                    for (char ch : *val) {
                        if (ch == '&') out += "&amp;";
                        else if (ch == '<') out += "&lt;";
                        else if (ch == '>') out += "&gt;";
                        else out += ch;
                    }
                } else {
                    Warn("组件参数 \"" + key + "\" 未在 <use> 上提供，已按空串替换");
                }
                i = end + 2;
                continue;
            }
        }
        out += src[i++];
    }
    return out;
}

// 单独编译组件源码时：{{param=默认}} 取默认值（裸 {{param}} 留给内联时替换）。
// 这样 .bum.html 既能被 <use> 内联传参，也能自己编成 .bum —— 两种形态语义一致。
static std::string ApplyParamDefaults(const std::string& src) {
    static const std::unordered_map<std::string, std::string> kNoParams;
    return SubstituteParams(src, kNoParams, /*defaultsOnly=*/true);
}

// ============================================================
// 组件来源解析：.bum/.bui 二进制 与 .bum.html/.bum.xml（旧）/.bui.html/.bui.xml 源码
//
// 作者可以只放二进制、只放源码，或两种都放（甚至按组件混放）：
//   · 无扩展名的 src 依次探测 x.bum / x.bui → x.bum.html / x.bum.xml → x.html / x.xml
//   · 二进制优先；但源码比二进制新（mtime）时按源码内联并告警 —— 改源码保存后
//     热重载就能看到效果，不会因为忘了重编 .bum 而"改了没反应"
//   · <use src="x.bum" external> 表示运行期挂载：编译期只留占位节点 + 组件引用段
// ============================================================
struct PendingComponent {
    std::string path;             // 解析后的组件文件路径（链接器直接用）
    std::string label;            // 诊断用（<use> 的 id 或标签）
    int         linkIndex = 0;
    bool        external = false; // true = 运行期挂载（写组件引用段）
    // <use> 上的尺寸/底色覆盖：二进制组件里参数已固化，这几个仍可覆盖
    // （原样保留属性文本，等 CompileHTML 里用统一的值解析器换算）
    std::string wAttr, hAttr, radiusAttr, bgAttr;
};

static thread_local std::vector<PendingComponent> tl_pendingComponents;
static thread_local std::unordered_map<int, uint16_t> tl_pendingAnchor; // linkIndex -> 占位节点 id

// 相对宿主 UI 目录改写：外挂组件的路径要能跟着 .bui 一起搬
static std::string RebaseToHost(const std::string& hostDir, const std::string& path) {
    if (hostDir.empty() || path.empty()) return path;
    std::error_code ec;
    const auto rel = std::filesystem::relative(std::filesystem::path(path),
                                               std::filesystem::path(hostDir), ec);
    if (ec) return path;
    const std::string s = rel.generic_string();
    return s.empty() ? path : s;
}

static void ExpandComponents(std::vector<HNode>& elems, std::string& styleSheets,
                             const std::string& baseDir, int depth) {    bool any = false;
    for (const auto& e : elems)
        if (e.tag == "use" || e.tag == "component") { any = true; break; }
    if (!any) return;
    if (depth > 8) {
        Warn("组件嵌套超过 8 层，疑似循环引用，已停止展开");
        return;
    }


    std::vector<HNode> out;
    out.reserve(elems.size() * 2);
    std::vector<size_t> remap(elems.size(), SIZE_MAX);

    for (size_t oi = 0; oi < elems.size(); oi++) {
        HNode& e = elems[oi];
        // SVG 的 <use href="#id"> 是引用克隆，不是组件内联：原样留下，交给 ExpandSvgUse
        if (e.tag == "use" && (e.attrs.count("href") || e.attrs.count("xlink:href"))) {
            HNode copy = e;
            copy.parentIdx = (copy.parentIdx == SIZE_MAX) ? SIZE_MAX : remap[copy.parentIdx];
            remap[oi] = out.size();
            out.push_back(std::move(copy));
            continue;
        }
        if (e.tag != "use" && e.tag != "component") {
            HNode copy = e;
            copy.parentIdx = (copy.parentIdx == SIZE_MAX) ? SIZE_MAX : remap[copy.parentIdx];
            remap[oi] = out.size();
            out.push_back(std::move(copy));
            continue;
        }

        // ---- src 与参数 ----
        const std::string srcRel = e.attrs.count("src") ? TrimCopy(e.attrs.at("src")) : std::string();
        const std::string label = e.attrs.count("id") ? ("#" + e.attrs.at("id")) : ("<" + e.tag + ">");
        // 除 src 之外的所有属性都可以被组件里的 {{参数}} 引用：几何/样式类属性
        // 既作为参数，也照样落到替换节点上（传 w=132 → 组件根宽 132，且 {{w}} = 132）
        std::unordered_map<std::string, std::string> params;
        for (const auto& kv : e.attrs)
            if (kv.first != "src") params[kv.first] = kv.second;

        std::vector<HNode> compElems;
        std::string compStyles;
        std::string compDir = baseDir;
        std::string failReason;
        bool deferred = false;   // 二进制内联 / 运行期外挂：留占位节点，稍后处理
        const bool external = e.attrs.count("external") > 0 ||
                              (e.attrs.count("mode") > 0 &&
                               LowerCopy(TrimCopy(e.attrs.at("mode"))) == "external");
        // 二进制 / 源码的选择规则见 include/binui_asset.h（二进制优先，源码更新则优先源码）
        const AssetFile af = ProbeAsset(baseDir, srcRel);
        if (srcRel.empty()) {
            failReason = "缺少 src 属性";
        } else if (!af.valid()) {
            failReason = "读不到组件文件「" + srcRel + "」（.bum / .bum.html / .bum.xml 都不存在）";
        } else {
            compDir = std::filesystem::path(af.path).parent_path().generic_string();
            const bool binary = (af.kind == AssetKind::BinaryLayout);
            // 二进制在、但源码更新：这次用源码（改完保存就该看到效果），并明确告警
            if (af.sourceNewer)
                Warn("组件 " + srcRel + " 的源码比二进制新，已改用源码内联（建议重编后发布）");
            if (binary || external) {
                deferred = true;   // 占位节点 + 链接器（编译期）/渲染器（运行期）
                // 二进制/外挂组件也进打包清单：存原始 .bum 字节，解包时原样还原。
                std::string compBytes;
                if (ReadWholeFile(af.path, compBytes))
                    SrcMapCollect(af.path, std::vector<uint8_t>(compBytes.begin(), compBytes.end()));
            } else {
                std::string compSrc;
                if (!ReadWholeFile(af.path, compSrc)) {
                    failReason = "读不到组件文件 " + af.path;
                } else {
                    // 打包清单存**替换参数之前**的原始源码：解包出来的组件文件要能
                    // 原样放回去再编一次，参数占位 {{x=默认}} 必须还在。
                    SrcMapCollect(af.path, std::vector<uint8_t>(compSrc.begin(), compSrc.end()));
                    compSrc = SubstituteParams(compSrc, params);
                    compElems = ParseElements(compSrc, compStyles);
                    if (compElems.empty()) failReason = "组件 " + af.path + " 解析后没有元素";
                    else ExpandComponents(compElems, compStyles, compDir, depth + 1);
                }
            }
        }
        if (!failReason.empty())
            Warn("组件内联 " + label + "：" + failReason + "（已按空容器处理）");

        // ---- 替换节点：<use> 属性优先，其次是组件根属性 ----
        // 组件根的**标签**必须跟着搬过来：以前这里恒为 "div"，于是
        //   · 以 <button> 为根的组件实例变成普通容器（不可点击、文字不居中）；
        //   · 以 <input> 为根的组件实例丢掉占位符/编辑语义；
        //   · 以 <span> 为根的组件实例变成块级 LIST（文字带 12px 内缩且顶部对齐）。
        HNode root;
        root.tag = "div";
        // 顶层元素个数 > 1 = 多根组件（"片段"写法）：用一个合成的 div 承载所有顶层元素。
        // 口径与二进制组件一致 —— binui_link 把组件里所有无父节点都挂到锚点节点下。
        size_t topCount = 0;
        for (size_t k = 0; k < compElems.size(); k++)
            if (compElems[k].parentIdx == SIZE_MAX) topCount++;
        const bool multiRoot = (topCount > 1);
        size_t compRootIdx = SIZE_MAX;
        if (multiRoot) {
            // 合成容器不带组件的任何属性（属性归宿主 <use>）：否则第一个根元素的
            // 样式会变成"容器的样式"，两个根元素互相挤在一起
            Warn("组件 " + label + " 有 " + std::to_string(topCount) +
                 " 个顶层元素，已用隐式 <div> 容器包裹（建议合并为单一根元素）");
        } else if (!compElems.empty()) {
            for (size_t k = 0; k < compElems.size(); k++)
                if (compElems[k].parentIdx == SIZE_MAX) { compRootIdx = k; break; }
            if (compRootIdx != SIZE_MAX) {
                if (!compElems[compRootIdx].tag.empty())
                    root.tag = compElems[compRootIdx].tag;
                root.attrs = compElems[compRootIdx].attrs;
                root.text = compElems[compRootIdx].text;
            }
        }
        // <use> 属性覆盖组件根属性；style/class 是"叠加"而非"替换"——
        // 否则给组件传 style 会把组件自己的背景/尺寸全抹掉
        for (const auto& kv : e.attrs) {
            if (kv.first == "src") continue;
            // 只有"有语义的属性"（宽高/样式/类…）该落到替换节点上；其余键是组件参数，
            // 已经在 SubstituteParams 里换进文本了，留成属性只会变成无效属性噪音
            if (!UsedHtmlAttrs().count(kv.first) &&
                kv.first.rfind("data-", 0) != 0 && kv.first.rfind("aria-", 0) != 0)
                continue;
            auto rit = root.attrs.find(kv.first);
            if (rit != root.attrs.end() && !rit->second.empty() && !kv.second.empty() &&
                (kv.first == "style" || kv.first == "class")) {
                rit->second += (kv.first == "style" ? ";" : " ") + kv.second;
                continue;
            }
            root.attrs[kv.first] = kv.second;
        }
        root.parentIdx = (e.parentIdx == SIZE_MAX) ? SIZE_MAX : remap[e.parentIdx];

        // ---- 二进制内联 / 运行期外挂：登记待处理项，占位节点带上角标 ----
        // （链接器（编译期）或渲染器（运行期）按锚点节点 id 把组件挂上来）
        if (deferred) {
            PendingComponent pc;
            pc.path = af.path;
            pc.label = label;
            pc.linkIndex = static_cast<int>(tl_pendingComponents.size());
            pc.external = external;
            // <use> 的尺寸/底色/圆角对二进制组件依然有效（其余参数已在编 .bum 时固化）
            auto pickAttr = [&](std::initializer_list<const char*> keys) -> std::string {
                for (const char* k : keys) {
                    auto it = e.attrs.find(k);
                    if (it != e.attrs.end() && !it->second.empty()) return it->second;
                }
                return {};
            };
            pc.wAttr = pickAttr({"w", "width"});
            pc.hAttr = pickAttr({"h", "height"});
            pc.radiusAttr = pickAttr({"radius", "border-radius", "r"});
            pc.bgAttr = pickAttr({"bg", "background"});
            tl_pendingComponents.push_back(pc);
            root.attrs["data-binui-link"] = std::to_string(pc.linkIndex);
        }

        // ---- 组件内的相对资源路径 → 宿主相对路径 ----
        for (auto& ce : compElems) {
            if (ce.tag != "img" && ce.tag != "video") continue;
            auto sit = ce.attrs.find("src");
            if (sit == ce.attrs.end() || sit->second.empty() || baseDir.empty()) continue;
            if (compDir == baseDir) continue;
            std::error_code ec;
            const auto rel = std::filesystem::relative(
                std::filesystem::path(compDir) / sit->second,
                std::filesystem::path(baseDir), ec);
            if (!ec) sit->second = rel.generic_string();
        }

        // ---- 落地：替换节点 + 组件子元素（父索引按局部表重映射）----
        remap[oi] = out.size();
        out.push_back(std::move(root));
        if (!compElems.empty()) {
            std::vector<size_t> cremap(compElems.size(), SIZE_MAX);
            for (size_t k = 0; k < compElems.size(); k++) {
                if (!multiRoot && k == compRootIdx) continue;   // 单根：根已被替换节点取代
                HNode ce = compElems[k];
                // 顶层节点（parentIdx == SIZE_MAX）与"根的直接子节点"都挂到替换节点上；
                // 其余按组件内部索引重映射。多根时 parentIdx == SIZE_MAX 是常态，必须显式
                // 分支 —— 以前直接查 cremap[SIZE_MAX]，越界读（debug 断言 abort，release
                // 把元素挂到垃圾父节点上或整个丢掉）。
                if (ce.parentIdx == SIZE_MAX || ce.parentIdx == compRootIdx) ce.parentIdx = remap[oi];
                else ce.parentIdx = cremap[ce.parentIdx];
                cremap[k] = out.size();
                out.push_back(std::move(ce));
            }
        }
        styleSheets += compStyles;
    }
    elems.swap(out);
}
// ============================================================
// SVG 引用展开：<defs>/<symbol> 当模板，<use href="#id"> 克隆一份
//
// 为什么在**元素表**里展开而不是等到节点生成：<use> 的语义是"把目标子树搬到
// 这里再画一遍"，父级变了、祖先的 transform/fill 也变了；在扁平元素表里克隆
// 一次，后面所有既有逻辑（继承、viewBox、定位、样式）都自动生效。
//
// 约定：
//   · 模板子树打上 `_def` 标记，节点生成时按"隐藏"跳过（不会被直接画出来）；
//   · 克隆出来的副本去掉 `_def` 与 `id`（避免重复 id），根元素若是 <symbol>/<svg>
//     改成 <g>（<g> 不建立坐标系，viewBox 改由克隆根的 transform 承担）；
//   · <use src="x.bum"> 是组件内联，早在 ExpandComponents 里处理完了，这里跳过。
// ============================================================
static void MarkSvgDefinitions(std::vector<HNode>& elems) {
    for (size_t i = 0; i < elems.size(); i++) {
        size_t k = i;
        size_t guard = elems.size() + 1;
        while (k != SIZE_MAX && k < elems.size() && guard-- > 0) {
            const std::string& t = elems[k].tag;
            // 渐变定义（含 <stop>）也是模板：即使写在 <defs> 外面，也不该画出一个空盒子
            if (t == "defs" || t == "symbol" || t == "lineargradient" ||
                t == "radialgradient" || t == "stop" || t == "marker" || t == "clippath" ||
                t == "pattern" || t == "mask" || t == "filter") {
                elems[i].attrs["_def"] = "1";
                break;
            }
            k = elems[k].parentIdx;
        }
    }
}

// 纯数值长度（"24" / "24px" / "1.5"）：<use> 的 x/y/width/height 与 viewBox 都只按数字用
static float SvgLenPlain(const std::string& s, float dflt) {
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\t') p++;
    char* end = nullptr;
    const float v = static_cast<float>(std::strtod(p, &end));
    return end == p ? dflt : v;
}

static void ExpandSvgUse(std::vector<HNode>& elems, std::string (*lowerStr)(std::string)) {
    // 一份 id 索引（同名取第一个，SVG 里重复 id 本来就是作者错误）
    for (int pass = 0; pass < 8; pass++) {
        std::unordered_map<std::string, size_t> ids;
        for (size_t i = 0; i < elems.size(); i++) {
            auto it = elems[i].attrs.find("id");
            if (it != elems[i].attrs.end() && !it->second.empty() && !ids.count(it->second))
                ids.emplace(it->second, i);
        }
        size_t useIdx = SIZE_MAX;
        for (size_t i = 0; i < elems.size(); i++) {
            if (lowerStr(elems[i].tag) != "use") continue;
            if (elems[i].attrs.count("data-binui-link") || elems[i].attrs.count("src")) continue;
            useIdx = i;
            break;
        }
        if (useIdx == SIZE_MAX) break;

        const HNode& u = elems[useIdx];
        std::string href;
        if (u.attrs.count("href")) href = u.attrs.at("href");
        else if (u.attrs.count("xlink:href")) href = u.attrs.at("xlink:href");
        if (href.empty() || href[0] != '#') {
            Warn("<use> 只支持 href=\"#id\" 的本地引用（外链请用 <img>），已忽略");
            elems[useIdx].attrs.erase("href");
            elems[useIdx].attrs.erase("xlink:href");
            elems[useIdx].attrs["_def"] = "1";   // 失败的 <use> 不生成空盒子
            continue;
        }
        const std::string key = href.substr(1);
        auto tit = ids.find(key);
        if (tit == ids.end()) {
            Warn("<use href=\"" + href + "\"> 找不到对应元素（缺 id 或拼错）");
            elems[useIdx].attrs.erase("href");
            elems[useIdx].attrs.erase("xlink:href");
            elems[useIdx].tag = "span"; elems[useIdx].attrs["_def"] = "1";   // 失败的 <use> 不留空盒子   // 退化成普通容器，不再重复告警
            continue;
        }
        size_t target = tit->second;
        // 自引用 / 引用祖先：直接拒绝（否则会无限展开）
        for (size_t k = useIdx; k != SIZE_MAX && k < elems.size();) {
            if (k == target) {
                Warn("<use href=\"" + href + "\"> 引用了自己的祖先（循环），已忽略");
                elems[useIdx].tag = "span"; elems[useIdx].attrs["_def"] = "1";   // 失败的 <use> 不留空盒子
                elems[useIdx].attrs.erase("href");
                elems[useIdx].attrs.erase("xlink:href");
                target = SIZE_MAX;
                break;
            }
            k = elems[k].parentIdx;
        }
        if (target == SIZE_MAX) continue;
        // 目标子树在 DFS 前序里是连续区间 [target, end)
        size_t tend = target + 1;
        while (tend < elems.size() && elems[tend].parentIdx != SIZE_MAX &&
               elems[tend].parentIdx >= target)
            tend++;

        // <use> 自己的 x/y（SVG 语义：平移）+ <symbol>/<svg> 的 viewBox 缩放
        const float ux = SvgLenPlain(u.attrs.count("x") ? u.attrs.at("x") : "", 0.0f);
        const float uy = SvgLenPlain(u.attrs.count("y") ? u.attrs.at("y") : "", 0.0f);
        std::string xf = "translate(" + std::to_string(ux) + " " + std::to_string(uy) + ")";
        const std::string tt = lowerStr(elems[target].tag);
        if (tt == "symbol" || tt == "svg") {
            float vbx = 0, vby = 0, vbw = 0, vbh = 0;
            bool haveVb = false;
            if (elems[target].attrs.count("viewbox")) {
                // 四个数
                std::string s = elems[target].attrs.at("viewbox");
                for (char& ch : s) if (ch == ',') ch = ' ';
                float v[4] = { 0, 0, 0, 0 };
                int n = 0;
                std::string cur;
                for (size_t i2 = 0; i2 <= s.size(); i2++) {
                    if (i2 == s.size() || s[i2] == ' ' || s[i2] == '\t') {
                        if (!cur.empty()) {
                            if (n < 4) v[n++] = static_cast<float>(std::strtod(cur.c_str(), nullptr));
                            cur.clear();
                        }
                    } else {
                        cur += s[i2];
                    }
                }
                if (n == 4 && v[2] > 0 && v[3] > 0) {
                    vbx = v[0]; vby = v[1]; vbw = v[2]; vbh = v[3];
                    haveVb = true;
                }
            }
            const float uw = SvgLenPlain(u.attrs.count("width") ? u.attrs.at("width") : "", 0.0f);
            const float uh = SvgLenPlain(u.attrs.count("height") ? u.attrs.at("height") : "", 0.0f);
            if (haveVb && uw > 0.0f && uh > 0.0f) {
                const float sc = std::min(uw / vbw, uh / vbh);
                const float tx = ux + (uw - vbw * sc) * 0.5f;
                const float ty = uy + (uh - vbh * sc) * 0.5f;
                xf = "translate(" + std::to_string(tx) + " " + std::to_string(ty) + ") scale(" +
                     std::to_string(sc) + ")";
            } else if (haveVb) {
                // 没给显示尺寸：按 viewBox 原尺寸铺开
                const float tx = ux - vbx;
                const float ty = uy - vby;
                xf = "translate(" + std::to_string(tx) + " " + std::to_string(ty) + ")";
            }
        }

        std::vector<HNode> out;
        std::vector<size_t> remap(elems.size(), SIZE_MAX);
        std::vector<size_t> cremap(elems.size(), SIZE_MAX);
        out.reserve(elems.size() + (tend - target));
        for (size_t i = 0; i < elems.size(); i++) {
            if (i == useIdx) {
                HNode g = elems[i];
                g.tag = "g";
                g.attrs["transform"] = xf;
                g.attrs.erase("href");
                g.attrs.erase("xlink:href");
                remap[i] = out.size();
                out.push_back(std::move(g));
                for (size_t k = target; k < tend; k++) {
                    HNode c = elems[k];
                    c.attrs.erase("_def");
                    c.attrs.erase("id");
                    if (k == target) c.parentIdx = remap[useIdx];
                    else if (c.parentIdx != SIZE_MAX) c.parentIdx = cremap[c.parentIdx];
                    const std::string ct = lowerStr(c.tag);
                    if (ct == "symbol" || ct == "svg") {
                        c.tag = "g";
                        c.attrs.erase("viewbox");   // 属性名解析时已统一小写
                        c.attrs.erase("preserveaspectratio");
                    }
                    cremap[k] = out.size();
                    out.push_back(std::move(c));
                }
                continue;
            }
            if (i > target && i < tend) {
                // 模板子树原样保留（带 `_def` 标记，不生成节点）：
                // 后面还有别的 <use> 要克隆它，每轮只展开一个引用，模板必须留着
                HNode c = elems[i];
                if (c.parentIdx != SIZE_MAX && c.parentIdx < remap.size() &&
                    remap[c.parentIdx] != SIZE_MAX)
                    c.parentIdx = remap[c.parentIdx];
                remap[i] = out.size();
                out.push_back(std::move(c));
                continue;
            }
            HNode c = elems[i];
            if (c.parentIdx != SIZE_MAX && c.parentIdx < remap.size() &&
                remap[c.parentIdx] != SIZE_MAX)
                c.parentIdx = remap[c.parentIdx];
            remap[i] = out.size();
            out.push_back(std::move(c));
        }
        elems.swap(out);
    }
}

std::vector<uint8_t> CompileHTML(const std::string& html, std::vector<std::string>* outImages) {
    auto lowerStr = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return s;
    };

    // ---- 1. 容错解析 HTML 为扁平元素列表（与组件内联共用 ParseElements）----
    tl_pendingComponents.clear();   // 每次编译重置组件登记（CompileHTML 可被单独调用）
    tl_pendingAnchor.clear();
    std::string styleSheets; // <style> 文本（@keyframes 与 transition/animation 规则）
    std::vector<HNode> elems = ParseElements(ApplyParamDefaults(html), styleSheets);

    // ---- 1.5 组件内联：<use src="x.bum.html"> 展开成真实元素 ----
    ExpandComponents(elems, styleSheets, tl_uiBaseDir, 0);
    // ---- 1.6 SVG 引用：<defs>/<symbol> 打模板标记，<use href="#id"> 展开成克隆 ----
    MarkSvgDefinitions(elems);
    ExpandSvgUse(elems, lowerStr);

    if (elems.empty()) return {};

    // ---- v7 动画：解析 <style> 中的 @keyframes 与 transition/animation 规则 ----
    std::vector<std::pair<std::string, std::vector<BinUIKeyFrame>>> kfGroups;
    std::vector<std::pair<std::string, std::string>> cssAnimRules, cssTransRules;
    if (!styleSheets.empty()) {
        AnimParseKeyframes(styleSheets, kfGroups);
        AnimParseRules(styleSheets, cssAnimRules, cssTransRules);
    }

    // ---- <style> 里的普通 CSS 声明（selector 参与级联；含伪类/伪元素） ----
    std::vector<CssRule> cssRules;
    if (!styleSheets.empty()) ParseCssRules(styleSheets, cssRules);

    // 结构伪类（:first-child / :nth-child / :first-of-type …）需要的兄弟上下文：
    // 按父元素分组统计序号。只统计"会参与布局"的元素 —— head/title/meta/link/script/
    // style/app_window 这类不生成节点的包裹层不计入（与浏览器对元素的计数口径一致，
    // 差异只在 display:none 的元素仍被计数）。
    auto isNonVisualTag = [](const std::string& tag) {
        return tag == "head" || tag == "title" || tag == "meta" || tag == "link" ||
               tag == "script" || tag == "style" || tag == "app_window";
    };
    bool hasHtmlElem = false;
    for (const auto& e : elems)
        if (e.tag == "html") hasHtmlElem = true;
    // 可视兄弟索引（父 -> 子），组合子与结构伪类共用。伪元素展开会改元素表，
    // 展开后再建一次（selectors 只需与自己那张表配套的那一份）。
    std::vector<std::vector<size_t>> cssKids = BuildKidIndex(elems);
    std::vector<CssElemCtx> elemCtx(elems.size());
    {
        for (size_t k = 0; k < elems.size(); k++) {
            if (isNonVisualTag(elems[k].tag)) continue;
            CssElemCtx& c = elemCtx[k];
            c.tag = elems[k].tag;
            c.cls = elems[k].attrs.count("class") ? elems[k].attrs.at("class") : "";
            c.id  = elems[k].attrs.count("id")    ? elems[k].attrs.at("id")    : "";
            c.attrs = &elems[k].attrs;
            c.isRoot = (hasHtmlElem ? elems[k].tag == "html"
                                    : (elems[k].tag == "body" && elems[k].parentIdx == SIZE_MAX));
        }
        for (size_t p = 0; p < cssKids.size(); p++) {
            const auto& kv = cssKids[p];
            if (kv.empty()) continue;
            std::unordered_map<std::string, int> typeCnt;
            for (size_t k : kv) typeCnt[elems[k].tag]++;
            std::unordered_map<std::string, int> seen;
            for (size_t n = 0; n < kv.size(); n++) {
                const size_t k = kv[n];
                CssElemCtx& c = elemCtx[k];
                c.idx = (int)n;
                c.cnt = (int)kv.size();
                c.typeIdx = seen[elems[k].tag]++;
                c.typeCnt = typeCnt[elems[k].tag];
            }
        }
    }
    // 无结构信息的元素（伪元素合成节点等）：只匹配 tag/class/id
    auto ctxOf = [&](size_t ei, const HNode& e) -> CssElemCtx {
        CssElemCtx c;
        if (ei < elemCtx.size()) c = elemCtx[ei];
        if (c.tag.empty() && c.cls.empty() && c.id.empty()) {
            c.tag = e.tag;
            c.cls = e.attrs.count("class") ? e.attrs.at("class") : "";
            c.id  = e.attrs.count("id")    ? e.attrs.at("id")    : "";
        }
        c.attrs = &e.attrs;   // 属性选择器要读实元素（合成节点也有属性表）
        return c;
    };

    // 按选择器命中为元素配置动画/过渡（内联声明优先，CSS 规则兜底）
    auto cssAnimFor = [&](const HNode& e, const std::string& styleAnim) {
        if (!styleAnim.empty()) return styleAnim;
        auto eTag = e.tag, eCls = (e.attrs.count("class") ? e.attrs.at("class") : ""),
             eId  = (e.attrs.count("id")    ? e.attrs.at("id")    : "");
        for (const auto& r : cssAnimRules)
            if (AnimMatchSelector(r.first, eTag, eCls, eId)) return r.second;
        return std::string();
    };
    auto cssTransFor = [&](const HNode& e, const std::string& styleTrans) {
        if (!styleTrans.empty()) return styleTrans;
        auto eTag = e.tag, eCls = (e.attrs.count("class") ? e.attrs.at("class") : ""),
             eId  = (e.attrs.count("id")    ? e.attrs.at("id")    : "");
        for (const auto& r : cssTransRules)
            if (AnimMatchSelector(r.first, eTag, eCls, eId)) return r.second;
        return std::string();
    };

    // ---- 2. HTML 语义 -> 节点 ----
    // 行内文本标签：一律 BINUI_LABEL（单行文字 + 自动撑高）。
    // 这里漏一个标签的后果不是"没样式"，而是掉到默认分支变 BINUI_LIST——块级容器，
    // 会把同一段文字里的一小截打成独立整行块（<u>/<s>/<mark>/<code>/<sub>/<sup>
    // 以前就是这样）。新增 HTML 标签时先想清楚它是行内还是块级。
    static const std::unordered_set<std::string> kInlineTextTags = {
        "h1", "h2", "h3", "h4", "h5", "h6", "p", "span", "label", "a",
        "b", "i", "em", "strong", "small", "u", "s", "strike", "del", "ins",
        "mark", "code", "kbd", "samp", "var", "abbr", "cite", "q", "dfn", "time",
        "sub", "sup", "li", "td", "th", "dt", "dd", "title", "legend",
        "figcaption", "summary", "caption",
    };
    auto tagToType = [&](const std::string& tag) -> uint8_t {
        if (tag == "html" || tag == "body") return BINUI_ROOT;
        if (tag == "button") return BINUI_BUTTON;
        if (tag == "input") return BINUI_INPUT;
        if (tag == "img") return BINUI_IMAGE;
        if (kInlineTextTags.count(tag)) return BINUI_LABEL;
        if (tag == "svg") return BINUI_SVG;
        if (tag == "canvas") return BINUI_CANVAS;
        if (tag == "line") return BINUI_LINE;
        if (tag == "path" || tag == "ellipse") return BINUI_PATH;
        if (tag == "text") return BINUI_LABEL;      // SVG 文字 → 普通标签（定位见几何段）
        if (tag == "image") return BINUI_IMAGE;     // SVG 图片 → 普通图片节点
        if (tag == "video") return BINUI_VIDEO;
        if (tag == "rect" || tag == "circle") return BINUI_LIST;
        return BINUI_LIST;
    };

    auto defaultHeight = [](const std::string& tag, uint8_t type) -> float {
        switch (type) {
            case BINUI_LABEL:
                if (tag=="h1") return 36;
                if (tag=="h2") return 32;
                if (tag=="h3") return 28;
                if (tag=="h4") return 24;
                if (tag=="li" || tag=="td" || tag=="th") return 24;
                return 20;
            case BINUI_BUTTON: return 44;
            case BINUI_INPUT:  return 40;
            case BINUI_IMAGE:  return 64;
            case BINUI_VIDEO:  return 150;   // 浏览器默认视频高度
            default:         return 40;
        }
    };

    // 长度/颜色解析统一走文件作用域的 ParseLenPct / ParseColorValue（支持 em/rem/vw/vh、
    // 命名色、transparent、现代 rgb()/hsl()，并把不支持的单位/函数明确报警告）。
    // curCtx 保存"当前元素的字号"，供 em 换算用；每处理一个元素前更新。
    LenCtx curCtx;

    auto parseLen = [&](const std::string& s, float def, const char* what = "长度") -> float {
        // 未替换的组件参数占位（{{w}} 之类）不是作者的错：编 .bum 时它还没被代入，
        // 内联时会替换。静默回退默认值，别刷警告。
        if (s.find("{{") != std::string::npos) return def;
        return ParseLenPct(s, def, curCtx, what).value;
    };

    // 解析颜色；失败返回 def（并已打警告）
    auto parseColorWith = [&](const std::string& s, uint32_t def, const char* what = "颜色") -> uint32_t {
        if (s.empty()) return def;
        if (s.find("{{") != std::string::npos) return def;   // 同 parseLen：参数占位
        uint32_t c = def;
        if (!ParseColorValue(s, &c, what)) return def;
        return c;
    };
    auto parseColor = [&](const std::string& s) -> uint32_t {
        return parseColorWith(s, 0xFFFFFFFF, "颜色");
    };

    // 元素生效样式 = <style> 里命中的规则（按特异性/文档序级联） + 内联 style（最高优先级）
    // ei = 元素下标（结构伪类求值用）；SIZE_MAX = 无结构信息（伪元素合成节点/包裹层）
    //
    // 为什么要 std::function + 记忆化：CSS 自定义属性（`var(--w, 120px)`，issue 15 的
    // M1）是**继承**的 —— 展开某个元素的 var() 时要往祖先链上找 `--w`，而"祖先的声明"
    // 就是祖先的生效样式，于是 styleOf 必须能递归调自己。按元素下标记住结果，避免同一
    // 元素被反复级联（顺带把原来的重复计算也省掉了）。
    // 注意：elems 在这之后还会增长（组件内联/伪元素合成都会 append），所以向量不能只按
    // 这里的尺寸分配一次 —— 每次用到时按当前 elems.size() 补齐，否则越界写会把堆写坏
    // （症状是别处 std::unordered_map 拷贝时抛 bad_array_new_length，极难往回追）。
    std::vector<std::unordered_map<std::string, std::string>> styleMemo;
    std::vector<char> styleDone;
    std::function<std::unordered_map<std::string, std::string>(const HNode&, size_t)> styleOf;
    styleOf = [&](const HNode& e, size_t ei) -> std::unordered_map<std::string, std::string> {
        // 伪元素（_pseudo 合成节点）按元素下标拿不到唯一答案：同一个下标会被"本体"和
        // "::after 变体"分别问一次，两者级联结果不同，所以它们不进缓存。
        const bool memoable = !e.attrs.count("_pseudo");
        if (memoable && ei < elems.size() && styleMemo.size() < elems.size()) {
            styleMemo.resize(elems.size());
            styleDone.resize(elems.size(), 0);
        }
        if (memoable && ei < styleMemo.size() && styleDone[ei]) return styleMemo[ei];

        // 最低层：内置 UA 默认样式（作者任何声明都会覆盖它）
        std::unordered_map<std::string, std::string> out = UaDeclsOf(e.tag);
        if (!cssRules.empty() && !e.attrs.count("_pseudo")) {
            std::unordered_map<std::string, int> specOf;   // 每个属性当前生效的特异性
            for (const auto& r : cssRules) {               // 文档顺序：同权重后者覆盖
                if (!r.pseudoEl.empty()) continue;          // 伪元素规则见 ExpandPseudo 合成
                if (ei >= elems.size()) {
                    // 无树位置的合成节点：只有单段规则能匹配
                    if (r.parts.size() != 1) continue;
                    if (!CssSimpleMatches(r.parts.front().sel, ctxOf(ei, e))) continue;
                } else if (!CssRuleMatchesDom(r, elems, cssKids, elemCtx, ei)) {
                    continue;
                }
                for (const auto& kv : r.decls) {
                    auto sit = specOf.find(kv.first);
                    if (sit != specOf.end() && r.spec < sit->second) continue;
                    out[kv.first] = kv.second;
                    specOf[kv.first] = r.spec;
                }
            }
        }
        auto it = e.attrs.find("style");                   // 内联覆盖一切
        if (it != e.attrs.end()) {
            for (auto& kv : ParseDeclBlock(it->second)) out[kv.first] = kv.second;
        }
        // v0.16.28：材质预设（material: glass / smoked / thin）——
        // 只补**作者没写**的声明，所以想微调哪条就写哪条（CSS 的"层叠"直觉）。
        // 预设只是把毛玻璃 + 半透明底 + 1px 高光边 + 圆角 + 投影打包成一句，
        // 落地仍然是 backdrop-filter 那一套，不需要任何新的格式/后端能力。
        {
            auto mit = out.find("material");
            if (mit != out.end()) {
                const std::string mv = lowerStr(TrimCopy(mit->second));
                const char* pbg = nullptr;
                const char* pbd = nullptr;
                const char* pfl = nullptr;
                if (mv == "glass" || mv == "frosted") {
                    pbg = "linear-gradient(180deg, rgba(255,255,255,0.56), rgba(255,255,255,0.26))";
                    pbd = "1px solid rgba(255,255,255,0.62)";
                    pfl = "blur(24px) grain(0.7)";
                } else if (mv == "smoked") {
                    pbg = "linear-gradient(180deg, rgba(22,24,28,0.58), rgba(22,24,28,0.36))";
                    pbd = "1px solid rgba(255,255,255,0.20)";
                    pfl = "blur(20px) grain(0.5)";
                } else if (mv == "thin") {
                    pbg = "linear-gradient(180deg, rgba(255,255,255,0.34), rgba(255,255,255,0.16))";
                    pbd = "1px solid rgba(255,255,255,0.42)";
                    pfl = "blur(12px) grain(0.4)";
                } else {
                    Warn("material: " + mv +
                         " 不是支持的材质预设（glass / smoked / thin），已忽略");
                }
                if (pbg) {
                    auto putIfAbsent = [&](const char* key, const char* val) {
                        if (out.find(key) == out.end()) out[key] = val;
                    };
                    putIfAbsent("background", pbg);
                    putIfAbsent("border", pbd);
                    putIfAbsent("border-radius", "24");
                    putIfAbsent("backdrop-filter", pfl);
                    putIfAbsent("box-shadow", "0 10 34 0 rgba(12,18,28,0.22)");
                }
            }
        }

        // ---- CSS 自定义属性展开（issue 15 的 M1）----
        //
        // `width: var(--w, 120px)` 这类写法有两个来源：作者手写（合法 HTML 的要求），
        // 以及组件参数（宿主写 `--w:132px` 覆盖）。binUI 的布局是**编译期**算的，
        // 所以变量也必须在编的时候就有值 —— 这里一次性把 var() 换成具体值，后面
        // ParseLenPct / ParseColorValue 看到的就只剩普通声明，不用认识 var()。
        //
        // 查找顺序与 CSS 一致：元素自己生效的声明（含内联 style）里的 `--x` →
        // 祖先链 → var() 的 fallback。都没有且没写 fallback：警告并**丢掉这条声明**
        // （对应 CSS 的 invalid-at-computed-value 语义，别偷偷当成 0 —— 0 会让盒子
        // 塌成不可见，比报错更难查）。
        {
            std::vector<std::string> drop;
            for (auto& kv : out) {
                if (kv.first.rfind("--", 0) == 0) continue;      // 自定义属性本身不展开
                std::string v = kv.second;
                if (v.find("var(") == std::string::npos) continue;

                bool bad = false;
                for (int depth = 0; depth < 8; ++depth) {
                    const size_t p = v.find("var(");
                    if (p == std::string::npos) break;
                    size_t r = p + 4;
                    int nest = 1;
                    for (; r < v.size(); ++r) {
                        if (v[r] == '(') nest++;
                        else if (v[r] == ')' && --nest == 0) break;
                    }
                    if (r >= v.size()) {                     // 括号不配对
                        Warn("var() 括号不匹配：\"" + kv.second + "\"，已忽略这条声明");
                        bad = true;
                        break;
                    }
                    const std::string inner = v.substr(p + 4, r - p - 4);
                    // 顶层逗号切 name / fallback（fallback 里可能有 rgb(…,…) 的逗号）
                    size_t comma = std::string::npos;
                    int cn = 0;
                    for (size_t k = 0; k < inner.size(); ++k) {
                        const char c = inner[k];
                        if (c == '(') cn++;
                        else if (c == ')') cn--;
                        else if (c == ',' && cn == 0) { comma = k; break; }
                    }
                    const std::string name = TrimCopy(
                        comma == std::string::npos ? inner : inner.substr(0, comma));
                    const std::string fallback = (comma == std::string::npos)
                        ? std::string() : TrimCopy(inner.substr(comma + 1));

                    std::string resolved;
                    bool found = false;
                    if (!name.empty() && name.rfind("--", 0) == 0) {
                        auto self = out.find(name);
                        if (self != out.end() && self->second != kv.second) {
                            resolved = self->second;
                            found = true;
                        }
                        // 祖先链：祖先生效样式里的同名自定义属性（CSS 的继承语义）
                        size_t cur = e.parentIdx;
                        for (int guard = 0; cur < elems.size() && guard < 64; ++guard) {
                            const auto pst = styleOf(elems[cur], cur);
                            auto pit = pst.find(name);
                            if (pit != pst.end()) { resolved = pit->second; found = true; break; }
                            cur = elems[cur].parentIdx;
                        }
                    }
                    if (!found) {
                        if (fallback.empty()) {
                            Warn("自定义属性 " + name + " 未定义，且 var() 没写 fallback："
                                 "已忽略这条声明（\"" + kv.first + "\" 用默认值）");
                            bad = true;
                            break;
                        }
                        resolved = fallback;
                    }
                    v = v.substr(0, p) + resolved + v.substr(r + 1);
                }
                if (bad) drop.push_back(kv.first);
                else kv.second = v;
            }
            for (const auto& k : drop) out.erase(k);
        }
        if (memoable && ei < styleMemo.size()) {
            styleMemo[ei] = out;
            styleDone[ei] = 1;
        }
        return out;
    };
    auto pick = [](const std::unordered_map<std::string, std::string>& a,
                   const std::unordered_map<std::string, std::string>& s,
                   const std::string& ak, const std::string& sk,
                   const std::string& def) -> std::string {
        auto it = a.find(ak);
        if (it != a.end()) return it->second;
        auto si = s.find(sk);
        if (si != s.end()) return si->second;
        return def;
    };

    // ============================================================
    // SVG 渐变定义（v16）：<linearGradient>/<radialGradient> + <stop>
    //
    // 为什么要在这里先建一份"注册表"，而不是在节点循环里现查：
    //   · 渐变定义通常在 <defs> 里（甚至写在用它的图形之后），节点循环是单趟向前的，
    //     回头再找元素很别扭；先扫一遍建索引，后面按 id O(1) 取。
    //   · href 继承（<linearGradient id="b" href="#a">）要把属性与色标沿链合并，
    //     合并只做一次、带环检测，比每次用的时候递归安全。
    //   · 色标颜色要乘 stop-opacity，而 fill-opacity/opacity 是**元素级**的，
    //     所以注册表里只折 stop-opacity，元素级的留到发射时折（见路径段那段）。
    //
    // 坐标语义（与运行期约定一致）：发射时再映射成"节点盒归一化坐标"，
    // 这里存的是 SVG 原始数值（objectBoundingBox 就是 0..1 的分数）。
    // ============================================================
    struct SvgGradStopDef {
        float    off = -1.0f;      // < 0 = 没写 offset（后面按 SVG 规则在相邻色标间插值）
        uint32_t color = 0xFF000000u;  // 已折 stop-opacity
    };
    struct SvgGradDef {
        bool     found = false;
        uint8_t  type = PATH_GRAD_LINEAR;
        uint8_t  spread = PATH_GRAD_PAD;
        bool     userSpace = false;     // gradientUnits="userSpaceOnUse"（默认是 objectBoundingBox）
        PathMatrix xform{};             // gradientTransform
        bool     xformRotated = false;  // 含旋转/斜切（径向渐变会因此变形，要告警）
        float    x1 = 0.0f, y1 = 0.0f, x2 = 1.0f, y2 = 0.0f;  // 线性端点（默认 0%/0% → 100%/0%）
        float    cx = 0.5f, cy = 0.5f, r = 0.5f;              // 径向（默认 50%/50%/50%）
        // v18：径向焦点（默认 = 圆心 = 普通同心圆）。与圆心不同时运行期按 SVG 锥形语义解。
        float    fx = 0.5f, fy = 0.5f;
        // 哪些字段写的是百分比：objectBoundingBox 下 /100 就是对的，
        // userSpaceOnUse 下还要再乘 viewport 尺寸（在 toNormSpace 里折算）。
        bool x1Pct = false, y1Pct = false, x2Pct = false, y2Pct = false;
        bool cxPct = false, cyPct = false, rPct = false, fxPct = false, fyPct = false;
        std::vector<SvgGradStopDef> stops;
        std::unordered_map<std::string, std::string> attrs;   // 沿 href 链合并后的属性
    };

    // 渐变里的数字：支持 "50%"、"0.5"、"1"（百分比一律 /100，两套 units 语义都是一样的除法）
    auto GradNum = [](const std::string& s, float dflt) -> float {
        std::string t;
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r') t += c;
        if (t.empty()) return dflt;
        bool pct = false;
        if (t.back() == '%') { pct = true; t.pop_back(); }
        char* end = nullptr;
        const float v = static_cast<float>(std::strtod(t.c_str(), &end));
        if (end == t.c_str()) return dflt;
        return pct ? v / 100.0f : v;
    };

    // fill="url(#id)" / url("#id") / url( #id ) → id（后面的兜底色一律忽略，用渐变）
    auto ParseUrlId = [](const std::string& v, std::string& id) -> bool {
        size_t i = 0;
        auto skipWs = [&]() { while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\r')) i++; };
        skipWs();
        std::string head;
        while (i < v.size() && v[i] != '(' && v[i] != ' ' && v[i] != '\t') head += v[i++];
        for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (head != "url") return false;
        skipWs();
        if (i >= v.size() || v[i] != '(') return false;
        i++;
        skipWs();
        char quote = 0;
        if (i < v.size() && (v[i] == '"' || v[i] == '\'')) quote = v[i++];
        std::string inner;
        while (i < v.size() && v[i] != ')' && v[i] != quote) inner += v[i++];
        while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t')) inner.pop_back();
        if (inner.empty() || inner[0] != '#') return false;
        id = inner.substr(1);
        while (!id.empty() && (id.back() == ' ' || id.back() == '\t')) id.pop_back();
        return !id.empty();
    };

    // 渐变元素上写 currentColor 时取谁的色：沿祖先链找第一个显式 color（近似）
    auto gradInheritedColor = [&](size_t idx) -> uint32_t {
        size_t k = idx;
        size_t guard = elems.size() + 1;
        while (k != SIZE_MAX && k < elems.size() && guard-- > 0) {
            auto st = styleOf(elems[k], k);
            auto cit = st.find("color");
            if (cit != st.end() && !cit->second.empty())
                return parseColorWith(cit->second, 0xFFFFFFFFu, "文字颜色");
            k = elems[k].parentIdx;
        }
        // 祖先链上没有显式 color：与 defaultTextCol 的默认一致（浅底深字那条只影响文字）
        return 0xFFFFFFFFu;
    };

    std::unordered_map<std::string, SvgGradDef> svgGrads;
    {
        std::unordered_map<std::string, size_t> gradIdx;   // id -> 元素下标（同名取第一个）
        for (size_t i = 0; i < elems.size(); i++) {
            const std::string t = lowerStr(elems[i].tag);
            if (t != "lineargradient" && t != "radialgradient") continue;
            auto idit = elems[i].attrs.find("id");
            if (idit == elems[i].attrs.end() || idit->second.empty()) continue;
            if (!gradIdx.count(idit->second)) gradIdx.emplace(idit->second, i);
        }
        std::unordered_set<std::string> resolving;
        std::function<SvgGradDef(const std::string&, int)> resolveGrad =
            [&](const std::string& gid, int depth) -> SvgGradDef {
            SvgGradDef out;
            auto it = gradIdx.find(gid);
            if (it == gradIdx.end()) return out;                    // found = false
            auto cacheIt = svgGrads.find(gid);
            if (cacheIt != svgGrads.end()) return cacheIt->second;
            if (depth > 8 || resolving.count(gid)) {
                Warn("SVG 渐变 #" + gid + " 的 href 链出现循环或超过 8 层，已按空渐变处理");
                return out;
            }
            resolving.insert(gid);
            const size_t gi = it->second;
            const HNode& ge = elems[gi];
            // 1) href 继承：目标先解析一遍，自己的属性覆盖它，自己的色标优先
            std::string href;
            if (ge.attrs.count("href")) href = ge.attrs.at("href");
            else if (ge.attrs.count("xlink:href")) href = ge.attrs.at("xlink:href");
            if (!href.empty()) {
                if (href[0] != '#') {
                    Warn("渐变 #" + gid + " 的 href 只支持 #本地引用（外链渐变不支持）");
                } else {
                    SvgGradDef base = resolveGrad(href.substr(1), depth + 1);
                    if (base.found) out = base;
                    else Warn("渐变 #" + gid + " 的 href=\"" + href + "\" 找不到定义（按本元素属性处理）");
                }
            }
            out.found = true;
            out.type = (lowerStr(ge.tag) == "radialgradient") ? PATH_GRAD_RADIAL : PATH_GRAD_LINEAR;
            for (const auto& kv : ge.attrs) {
                if (kv.first == "href" || kv.first == "xlink:href" || kv.first == "id") continue;
                out.attrs[kv.first] = kv.second;
            }
            // 2) 自己的色标（没有就沿用继承来的）
            const uint32_t curCol = gradInheritedColor(gi);
            std::vector<SvgGradStopDef> own;
            for (size_t k = 0; k < elems.size(); k++) {
                if (elems[k].parentIdx != gi) continue;
                if (lowerStr(elems[k].tag) != "stop") continue;
                const auto st = styleOf(elems[k], k);
                auto stopField = [&](const char* key) -> std::string {
                    auto sit = st.find(key);
                    if (sit != st.end() && !sit->second.empty()) return sit->second;
                    auto ait = elems[k].attrs.find(key);
                    return ait != elems[k].attrs.end() ? ait->second : std::string();
                };
                SvgGradStopDef sd;
                const std::string off = stopField("offset");
                if (!off.empty()) sd.off = GradNum(off, -1.0f);
                const std::string col = stopField("stop-color");
                uint32_t c = 0xFF000000u;
                if (lowerStr(col) == "currentcolor") c = curCol;      // 规范：用渐变元素自身的 color
                else if (!col.empty()) c = parseColorWith(col, 0xFF000000u, "stop-color");
                const std::string sop = stopField("stop-opacity");
                if (!sop.empty()) {
                    float op = GradNum(sop, 1.0f);
                    op = op < 0.0f ? 0.0f : (op > 1.0f ? 1.0f : op);
                    const uint32_t a = (uint32_t)(((c >> 24) & 0xFFu) * op + 0.5f);
                    c = (a << 24) | (c & 0x00FFFFFFu);
                }
                sd.color = c;
                own.push_back(sd);
            }
            if (!own.empty()) out.stops = std::move(own);
            resolving.erase(gid);
            svgGrads[gid] = out;
            return out;
        };
        for (const auto& kv : gradIdx) resolveGrad(kv.first, 0);

        // 3) 收尾：属性解析（units / transform / spread / 端点）+ 色标 offset 规则
        for (auto& kv : svgGrads) {
            SvgGradDef& g = kv.second;
            if (!g.found) continue;
            auto A = [&](const char* key) -> std::string {
                auto it = g.attrs.find(key);
                return it == g.attrs.end() ? std::string() : it->second;
            };
            g.userSpace = (lowerStr(A("gradientunits")) == "userspaceonuse");
            // 百分比记号：objectBoundingBox 下 /100 就是"包围盒的比例"（正确）；
            // userSpaceOnUse 下规范是"视口尺寸的比例"，要在 toNormSpace 里乘 viewport 尺寸。
            // 这里只记下"哪些字段写的是百分比"，两种单位各自的折算放到用的时候做。
            auto markPct = [&](const char* key, bool& flag) {
                std::string v = A(key);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                flag = !v.empty() && v.back() == '%';
            };
            markPct("x1", g.x1Pct); markPct("y1", g.y1Pct);
            markPct("x2", g.x2Pct); markPct("y2", g.y2Pct);
            markPct("cx", g.cxPct); markPct("cy", g.cyPct);
            markPct("r", g.rPct);   markPct("fx", g.fxPct); markPct("fy", g.fyPct);
            const std::string gt = A("gradienttransform");
            g.xform = PathMatrix{};
            if (!gt.empty()) {
                std::string gerr;
                if (!ParseSvgTransform(gt, g.xform, &gerr)) {
                    Warn("渐变 #" + kv.first + " 的 gradientTransform 解析失败（" + gerr +
                         "），按无变换处理");
                    g.xform = PathMatrix{};
                }
            }
            g.xformRotated = std::fabs(g.xform.b) > 1e-6f || std::fabs(g.xform.c) > 1e-6f;
            const std::string sp = lowerStr(A("spreadmethod"));
            g.spread = (sp == "repeat") ? PATH_GRAD_REPEAT
                     : (sp == "reflect") ? PATH_GRAD_REFLECT : PATH_GRAD_PAD;
            g.x1 = GradNum(A("x1"), 0.0f);
            g.y1 = GradNum(A("y1"), 0.0f);
            g.x2 = GradNum(A("x2"), 1.0f);
            g.y2 = GradNum(A("y2"), 0.0f);
            g.cx = GradNum(A("cx"), 0.5f);
            g.cy = GradNum(A("cy"), 0.5f);
            g.r  = GradNum(A("r"), 0.5f);
            // v18：径向焦点。没写 fx/fy 就是圆心；写在圆外时规范没定义，按同心圆画并告警。
            g.fx = GradNum(A("fx"), g.cx);
            g.fy = GradNum(A("fy"), g.cy);
            if (g.type == PATH_GRAD_RADIAL && g.r > 1e-6f) {
                const float dx = (g.fx - g.cx) / g.r, dy = (g.fy - g.cy) / g.r;
                if (dx * dx + dy * dy >= 1.0f)
                    Warn("渐变 #" + kv.first + "：径向焦点 fx/fy 落在渐变圆外（规范未定义），"
                         "已按同心圆绘制（把焦点挪到圆内，或去掉 fx/fy）");
            }
            // 色标上限
            if (g.stops.size() > (size_t)kPathGradMaxStops) {
                Warn("渐变 #" + kv.first + " 有 " + std::to_string(g.stops.size()) +
                     " 个色标，超过 " + std::to_string(kPathGradMaxStops) +
                     " 个上限，已截断（取前 8 个）");
                g.stops.resize(kPathGradMaxStops);
            }
            // offset 补齐：没写的在相邻已知色标之间等距插值（首尾分别贴 0 / 1）
            const size_t n = g.stops.size();
            if (n == 1) {
                // 只有一个色标 = 纯色；格式要求至少 2 个（运行期按两端插值），复制一份
                g.stops.push_back(g.stops[0]);
            } else if (n >= 2) {
                size_t k = 0;
                while (k < n && g.stops[k].off < 0.0f) k++;      // 开头连续缺失
                if (k == n) {
                    // 全部没写 offset：按 SVG 规则在首个 0 与末个 1 之间均分
                    // （踩过：这里曾按"首个已知值 = 0"处理，2 个色标全缺时两个 offset 都成 0，
                    //  运行期插值退化成纯色）
                    for (size_t i = 0; i < n; i++)
                        g.stops[i].off = static_cast<float>(i) / static_cast<float>(n - 1);
                } else {
                    const float firstKnown = g.stops[k].off;
                    for (size_t i = 0; i < k; i++)
                        g.stops[i].off = firstKnown * static_cast<float>(i) / static_cast<float>(k);
                    for (size_t i = k + 1; i < n; i++) {
                        if (g.stops[i].off >= 0.0f) continue;
                        // 找这段缺失的右端点（没有就贴 1）
                        size_t j = i;
                        while (j < n && g.stops[j].off < 0.0f) j++;
                        const float leftOff = g.stops[i - 1].off;
                        const float rightOff = (j < n) ? g.stops[j].off : 1.0f;
                        const size_t cnt = j - i;
                        for (size_t m = 0; m < cnt; m++)
                            g.stops[i + m].off = leftOff + (rightOff - leftOff) *
                                static_cast<float>(m + 1) / static_cast<float>(cnt + 1);
                        i = j;
                    }
                }
                // 非递减夹紧 + 夹到 [0,1]（SVG 要求色标单调，后一个小于前一个就抬平）
                float prev = 0.0f;
                for (size_t i = 0; i < n; i++) {
                    float o = g.stops[i].off;
                    o = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
                    if (o < prev) o = prev;
                    g.stops[i].off = o;
                    prev = o;
                }
            }
        }
    }

    // 解析 CSS box 简写（margin/padding）为 {top,right,bottom,left}，支持 1~4 值 + 单边覆盖
    auto parseBox = [&](const std::unordered_map<std::string, std::string>& style,
                        const std::string& shorthand,
                        const std::string& t, const std::string& r,
                        const std::string& b, const std::string& l,
                        float def) -> std::array<float, 4> {
        std::array<float, 4> out = { def, def, def, def };
        auto it = style.find(shorthand);
        if (it != style.end()) {
            std::vector<float> vals;
            std::string cur;
            auto flush = [&] {
                if (!cur.empty()) { vals.push_back(parseLen(cur, def)); cur.clear(); }
            };
            for (char c : it->second) {
                if (c == ' ' || c == '\t') flush();
                else cur += c;
            }
            flush();
            if (vals.size() == 1) out = { vals[0], vals[0], vals[0], vals[0] };
            else if (vals.size() == 2) out = { vals[0], vals[1], vals[0], vals[1] };
            else if (vals.size() == 3) out = { vals[0], vals[1], vals[2], vals[1] };
            else if (vals.size() >= 4) out = { vals[0], vals[1], vals[2], vals[3] };
        }
        auto over = [&](const std::string& k, size_t idx) {
            auto si = style.find(k);
            if (si != style.end()) out[idx] = parseLen(si->second, def);
        };
        over(t, 0); over(r, 1); over(b, 2); over(l, 3);
        return out;
    };

    // ---- v13 扩展绘制：边框 / 渐变 / 字重 / 行高 / 文字装饰 ----
    // CSS 值切词：空白分隔，但括号内的空格不切（rgb(0 0 0) / linear-gradient(...) 要整块保留）
    auto splitCssTokens = [](const std::string& v) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::string cur;
        int depth = 0;
        for (char c : v) {
            if (c == '(') depth++;
            else if (c == ')' && depth > 0) depth--;
            if ((c == ' ' || c == '\t') && depth == 0) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };

    // 解析一个"边框简写"（"1px solid #444" / "2px #f00" / "none" / "thin"）
    struct BorderSide { uint8_t w = 0; bool hasColor = false; uint32_t color = 0; };
    auto parseBorderSide = [&](const std::string& v, const char* what) -> BorderSide {
        BorderSide out;
        const std::string s = LowerCopy(TrimCopy(v));
        if (s.empty() || s == "none" || s == "0") return out;
        for (const auto& tok : splitCssTokens(s)) {
            if (tok == "solid" || tok == "dashed" || tok == "dotted" || tok == "double" ||
                tok == "groove" || tok == "ridge" || tok == "inset" || tok == "outset") {
                if (tok != "solid") Warn(std::string("CSS ") + what + " 只支持 solid，\"" + tok + "\" 按 solid 绘制");
                continue;
            }
            if (tok == "thin")   { out.w = 1; continue; }
            if (tok == "medium") { out.w = 3; continue; }
            if (tok == "thick")  { out.w = 5; continue; }
            const char c0 = tok.empty() ? ' ' : tok[0];
            if (c0 == '#' || std::isalpha((unsigned char)c0)) {
                uint32_t c = 0;
                if (ParseColorValue(tok, &c, "边框颜色")) { out.color = c; out.hasColor = true; }
                continue;
            }
            auto p = parseLen(tok, 0, what);
            if (p > 0) out.w = (uint8_t)std::min(64.0f, p);
        }
        return out;
    };

    // 边框解析：四边宽 + 四边色（CSS 的 border/border-width/border-color 简写
    // 按 1~4 值展开，border-<side> 再单边覆盖；颜色与宽度独立设置）
    struct BorderSpec {
        uint8_t  w[4] = { 0, 0, 0, 0 };                    // 上右下左
        uint32_t color[4] = { 0xFF3A3A46, 0xFF3A3A46, 0xFF3A3A46, 0xFF3A3A46 };
        bool any = false;
    };
    auto parseBorderSpec = [&](const HNode& e,
                               const std::unordered_map<std::string, std::string>& style) -> BorderSpec {
        BorderSpec bs;
        auto setAllColor = [&](uint32_t c) { for (int k = 0; k < 4; k++) bs.color[k] = c; };

        // 全局简写：border: <width> <style> <color>（任意顺序）
        std::string bord = pick(e.attrs, style, "border", "border", "");
        if (!bord.empty()) {
            BorderSide g = parseBorderSide(bord, "border");
            for (int k = 0; k < 4; k++) bs.w[k] = g.w;
            if (g.hasColor) setAllColor(g.color);
        }
        // border-width：1~4 值展开（与 CSS 一致）
        std::string bwv = style.count("border-width") ? style.at("border-width") : "";
        if (!bwv.empty()) {
            const auto toks = splitCssTokens(bwv);
            uint8_t ww[4] = { 0, 0, 0, 0 };
            if (toks.size() == 1) ww[0] = ww[1] = ww[2] = ww[3] = (uint8_t)parseLen(toks[0], 0);
            else if (toks.size() == 2) {
                ww[0] = ww[2] = (uint8_t)parseLen(toks[0], 0);
                ww[1] = ww[3] = (uint8_t)parseLen(toks[1], 0);
            } else if (toks.size() == 3) {
                ww[0] = (uint8_t)parseLen(toks[0], 0);
                ww[1] = ww[3] = (uint8_t)parseLen(toks[1], 0);
                ww[2] = (uint8_t)parseLen(toks[2], 0);
            } else if (toks.size() >= 4) {
                for (int k = 0; k < 4; k++) ww[k] = (uint8_t)parseLen(toks[k], 0);
            }
            for (int k = 0; k < 4; k++) bs.w[k] = ww[k];
        }
        // border-color：1~4 值展开
        std::string bcv = style.count("border-color") ? style.at("border-color") : "";
        if (!bcv.empty()) {
            const auto toks = splitCssTokens(bcv);
            uint32_t cc[4] = { 0, 0, 0, 0 };
            bool ok[4] = { false, false, false, false };
            if (toks.size() == 1) {
                uint32_t c = 0;
                if (ParseColorValue(toks[0], &c, "border-color")) {
                    for (int k = 0; k < 4; k++) { cc[k] = c; ok[k] = true; }
                }
            } else if (toks.size() >= 2) {
                for (size_t k = 0; k < toks.size() && k < 4; k++)
                    ok[k] = ParseColorValue(toks[k], &cc[k], "border-color");
                if (toks.size() == 2) { cc[2] = cc[0]; ok[2] = ok[0]; cc[3] = cc[1]; ok[3] = ok[1]; }
                else if (toks.size() == 3) { cc[3] = cc[1]; ok[3] = ok[1]; }
            }
            for (int k = 0; k < 4; k++) if (ok[k]) bs.color[k] = cc[k];
        }
        // border-<side>：宽度与颜色单边覆盖
        static const char* kSide[4] = { "top", "right", "bottom", "left" };
        for (int k = 0; k < 4; k++) {
            const std::string key = std::string("border-") + kSide[k];
            auto it = style.find(key);
            if (it == style.end()) continue;
            BorderSide s = parseBorderSide(it->second, key.c_str());
            bs.w[k] = s.w;
            if (s.hasColor) bs.color[k] = s.color;
        }
        for (int k = 0; k < 4; k++) if (bs.w[k]) bs.any = true;
        return bs;
    };

    // 文字宽度估算（编译期没有字体度量）：CJK/全角按 1.0×字号，其余按 0.52×字号。
    // 算法只有一份，在 include/binui_text_estimate.h —— 渲染器运行期对 heightAuto
    // 节点 setText 后重算高度必须与编译期同口径，否则"编译期刚好放下、换文本就压字"。
    // 这里保留 lambda 只是让调用点沿用原来的名字。
    auto estTextWidth = [](const std::string& text, float fontSize) -> float {
        return EstimateTextWidth(text, fontSize);
    };
    // 行距（像素）：line-height 支持无单位倍数 / % / px；未声明按 1.35×字号。
    // 解析留在这里（style 是编译器的私有类型），换算口径（1.35×字号兜底、50%~400% 有效）
    // 走共享头的 EstimateLineStep。极端值（如 line-height:5）共享口径按未声明处理，
    // 与旧实现的 5×字号略有出入 —— 正常布局用不到，且宁可估低不压字。
    auto estLineStep = [](const std::unordered_map<std::string, std::string>& style,
                          float fontSize) -> float {
        auto it = style.find("line-height");
        int pct = 0;   // 0 = 未声明/不可解析 → EstimateLineStep 按 1.35×字号
        if (it != style.end()) {
            const std::string v = TrimCopy(it->second);
            if (!v.empty() && LowerCopy(v) != "normal") {
                if (v.back() == '%') {
                    try { pct = (int)std::lround(std::stof(v)); } catch (...) {}
                } else {
                    const bool unitless = v.find_first_not_of("0123456789.+-") == std::string::npos;
                    float num = 0;
                    try { num = std::stof(v); } catch (...) { num = 0; }
                    if (unitless) pct = (int)std::lround(num * 100.0f);
                    else if (num > 0.5f && fontSize > 0.5f)
                        pct = (int)std::lround(num / fontSize * 100.0f);   // px → 百分数
                }
            }
        }
        return EstimateLineStep(fontSize, pct);
    };


    // 渐变底色：linear-gradient(方向, 起色, 止色[, ...])  只取首尾两色
    //
    // v0.16.13：方向**真的按角度解析**了。以前只认 `to right/bottom` 这类关键字，
    // 数字角度（`135deg` / `0.5turn` / `1.57rad`）一律当默认（= 垂直），于是
    // `linear-gradient(135deg,…)` 会被画成上下渐变 —— 一眼能看出是假的（P1-6）。
    // 编码：`angle` ∈ [0,180) 度，写入 paint.reserve[1] = angle + 1（0 = 未声明）；
    // 轴对齐的 0/90 度仍走老三方向（V/H），这样历史页面的像素一位都不变。
    struct GradSpec {
        bool ok = false;
        uint32_t from = 0, to = 0;
        uint8_t dir = BINUI_GRAD_V;
        int angle = -1;   // <0 = 未声明；否则 [0,180) 度（from 在渐变线起点一侧）
    };
    auto parseLinearGradient = [&](const std::string& raw) -> GradSpec {
        GradSpec gs;
        std::string s = TrimCopy(raw);
        if (s.empty()) return gs;
        const std::string low = LowerCopy(s);
        if (low.find("radial-gradient") != std::string::npos) {
            Warn("CSS radial-gradient 暂不支持，已忽略（只支持 linear-gradient）");
            return gs;
        }
        const size_t at = low.find("linear-gradient(");
        if (at == std::string::npos) {
            if (low.rfind("url(", 0) == 0 || low.find("image-set(") != std::string::npos)
                Warn("CSS background-image: url(...) 暂不支持，已忽略（用 <img> 或 background:linear-gradient）");
            return gs;
        }
        const size_t open = at + std::string("linear-gradient").size();
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; i < s.size(); i++) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') { if (--depth == 0) { close = i; break; } }
        }
        if (close == std::string::npos) { Warn("CSS linear-gradient(...) 括号不匹配，已忽略"); return gs; }
        const std::string inner = s.substr(open + 1, close - open - 1);

        // 顶层逗号切分
        std::vector<std::string> parts;
        {
            std::string cur;
            int d2 = 0;
            for (char c : inner) {
                if (c == '(') d2++;
                else if (c == ')' && d2 > 0) d2--;
                if (c == ',' && d2 == 0) { parts.push_back(TrimCopy(cur)); cur.clear(); continue; }
                cur += c;
            }
            if (!TrimCopy(cur).empty()) parts.push_back(TrimCopy(cur));
        }
        if (parts.size() < 2) { Warn("CSS linear-gradient 至少需要两个颜色，已忽略"); return gs; }

        // 首项若是方向/角度就吃掉
        size_t cs = 0;
        float gdeg = 180.0f;   // CSS 角度：0 = to top，顺时针为正；默认 to bottom
        {
            const std::string first = LowerCopy(parts[0]);
            const bool looksDir = first.rfind("to ", 0) == 0 || first.find("deg") != std::string::npos ||
                                  first.find("turn") != std::string::npos ||
                                  first.find("rad") != std::string::npos ||
                                  first.find("grad") != std::string::npos;
            if (looksDir) {
                cs = 1;
                if (first.rfind("to ", 0) == 0) {
                    // 关键字 → 角度。`right bottom` 与 `bottom right` 同义，都按位判。
                    const bool r = first.find("right") != std::string::npos;
                    const bool l = first.find("left") != std::string::npos;
                    const bool b = first.find("bottom") != std::string::npos;
                    const bool t = first.find("top") != std::string::npos;
                    if (r && b) gdeg = 135.0f;
                    else if (r && t) gdeg = 45.0f;
                    else if (l && b) gdeg = 225.0f;
                    else if (l && t) gdeg = 315.0f;
                    else if (r) gdeg = 90.0f;
                    else if (l) gdeg = 270.0f;
                    else if (t) gdeg = 0.0f;
                    else gdeg = 180.0f;
                } else {
                    // 数字角度：strtod 吃掉数字，再按后缀换算（deg / turn / rad / grad）
                    const char* s2 = first.c_str();
                    char* end = nullptr;
                    const double v = std::strtod(s2, &end);
                    if (end && end != s2) {
                        const std::string unit = TrimCopy(std::string(end));
                        if (unit == "turn")      gdeg = (float)(v * 360.0);
                        else if (unit == "rad")  gdeg = (float)(v * 180.0 / 3.14159265358979323846);
                        else if (unit == "grad") gdeg = (float)(v * 0.9);
                        else                     gdeg = (float)v;   // deg / 无单位
                    }
                }
            }
        }
        if (parts.size() - cs < 2) { Warn("CSS linear-gradient 至少需要两个颜色，已忽略"); return gs; }
        // 颜色项可能带位置（"#fff 20%"），只取前面那段颜色
        auto colorOf = [&](const std::string& item, uint32_t* out) -> bool {
            auto toks = splitCssTokens(item);
            if (toks.empty()) return false;
            return ParseColorValue(toks.front(), out, "渐变色");
        };
        uint32_t c1 = 0, c2 = 0;
        if (!colorOf(parts[cs], &c1)) return gs;
        if (!colorOf(parts.back(), &c2)) return gs;

        // 角度归一化到 [0,180)：θ 与 θ-180 只差"两端颜色互换"，线性渐变上完全等价。
        float a = std::fmod(gdeg, 360.0f);
        if (a < 0.0f) a += 360.0f;
        bool swap = false;
        if (a >= 180.0f) { a -= 180.0f; swap = true; }
        if (swap) std::swap(c1, c2);          // 现在 c1 在渐变线起点（a 方向的反侧）
        // 轴对齐的方向仍交给老三方向路径：历史页面（180deg 与 to right 占绝大多数）像素一位不变
        if (a <= 0.5f) {                       // to top：老 V 是"from 在上"，再换一次色
            gs.dir = BINUI_GRAD_V;
            std::swap(c1, c2);
        } else if (std::fabs(a - 90.0f) <= 0.5f) {
            gs.dir = BINUI_GRAD_H;
        } else {
            // 其余角度（含 45/135 这类对角）走任意角度条带。gradDir 只记"最近的老方向"，
            // 作为条带放不下时的降级近似（老 D 本身就是垂直近似）。
            gs.angle = (int)std::lround(a);
            if (gs.angle < 1) gs.angle = 1;
            if (gs.angle > 179) gs.angle = 179;
            gs.dir = (a < 45.0f) ? BINUI_GRAD_V : (a < 135.0f ? BINUI_GRAD_H : BINUI_GRAD_D);
        }
        gs.from = c1; gs.to = c2; gs.ok = true;
        return gs;
    };

    // ---- 2.5 伪元素 ::before/::after —— 编译期合成 ----
    // binUI 没有行内盒模型，只有"节点"和"节点的文本"，所以按声明的属性选择两种语义：
    //   · 内联模式：声明里没有任何盒属性 → 内容字符串前插/后附到宿主自身文本，不新增节点
    //     （`li::before{content:"▸ "}`、`.tag::after{content:" *"}` 都属于这类）
    //   · 盒模式：出现 width/height/background*/border-radius/padding/margin/display/
    //     border/position 之一 → 合成一个 span 子节点（LABEL），可作为分隔条/角标
    // content 支持普通字符串、转义（\41 / 引号 / \n）与 attr(属性名)；
    // counter()/counters() 没有计数器系统，警告后按空内容处理。
    {
        // 解码 CSS content 字符串（去引号 + 反斜杠转义 + \XX 十六进制码点）
        auto decodeContent = [](const std::string& raw) -> std::string {
            std::string s = TrimCopy(raw);
            if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                                  (s.front() == '\'' && s.back() == '\'')))
                s = s.substr(1, s.size() - 2);
            std::string out;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] != '\\') { out += s[i]; continue; }
                if (i + 1 >= s.size()) break;
                const char c = s[++i];
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == '\\' || c == '"' || c == '\'') out += c;
                else if (std::isxdigit((unsigned char)c)) {
                    std::string hex(1, c);
                    while (i + 1 < s.size() && hex.size() < 6 &&
                           std::isxdigit((unsigned char)s[i + 1])) hex += s[++i];
                    AppendUtf8(out, (uint32_t)std::strtoul(hex.c_str(), nullptr, 16));
                } else out += c;
            }
            return out;
        };

        std::vector<HNode> expanded;
        std::vector<CssElemCtx> expandedCtx;
        expanded.reserve(elems.size() * 2);
        expandedCtx.reserve(elems.size() * 2);
        std::vector<size_t> remap(elems.size(), SIZE_MAX);

        for (size_t oi = 0; oi < elems.size(); oi++) {
            HNode& host = elems[oi];
            const CssElemCtx hctx = ctxOf(oi, host);
            const auto hostStyle = styleOf(host, oi);

            // 收集 ::before / ::after 的生效声明（按特异性级联，同权重后写胜），
            // 内联模式下直接改宿主文本并返回 false（不合成节点）
            auto build = [&](const char* which, HNode& out) -> bool {
                std::unordered_map<std::string, std::string> merged;
                std::unordered_map<std::string, int> specOf;
                for (const auto& r : cssRules) {
                    if (r.pseudoEl != which) continue;
                    if (!CssRuleMatchesDom(r, elems, cssKids, elemCtx, oi)) continue;
                    for (const auto& kv : r.decls) {
                        auto sit = specOf.find(kv.first);
                        if (sit != specOf.end() && r.spec < sit->second) continue;
                        merged[kv.first] = kv.second;
                        specOf[kv.first] = r.spec;
                    }
                }
                if (merged.empty()) return false;
                auto cit = merged.find("content");
                if (cit == merged.end()) {
                    Warn(std::string("CSS ::") + which +
                         " 缺少 content 声明，已忽略（纯装饰请写 content:\"\")");
                    return false;
                }
                std::string text;
                {
                    const std::string raw = TrimCopy(cit->second);
                    const std::string low = LowerCopy(raw);
                    if (low.rfind("attr(", 0) == 0) {
                        const size_t close = raw.find(')');
                        const std::string name = LowerCopy(TrimCopy(raw.substr(
                            5, close == std::string::npos ? std::string::npos : close - 5)));
                        auto ait = host.attrs.find(name);
                        if (ait != host.attrs.end()) text = ait->second;
                    } else if (low.rfind("counter(", 0) == 0 || low.find("counters(") != std::string::npos) {
                        Warn("CSS content 里的 counter()/counters() 暂不支持（无计数器系统），按空内容处理");
                    } else {
                        text = decodeContent(raw);
                    }
                }
                static const char* kBoxKeys[] = {
                    "width", "w", "height", "h", "background", "background-color", "bg",
                    "border-radius", "padding", "padding-top", "padding-left", "margin",
                    "margin-top", "margin-left", "display", "border", "position",
                };
                bool box = false;
                for (const char* k : kBoxKeys)
                    if (merged.count(k)) { box = true; break; }

                if (!box) {   // 内联：并入宿主文本
                    if (merged.count("color") || merged.count("text-color") ||
                        merged.count("font-size") || merged.count("size"))
                        Warn(std::string("内联 ::") + which +
                             " 与宿主共用字号/颜色（同一节点只有一个文本样式），color/font-size 已忽略；"
                             "需要独立样式请声明 width/height/background 走盒模式");
                    if (std::string(which) == "before") host.text = text + host.text;
                    else                                host.text += text;
                    if (host.tag == "li" && std::string(which) == "before")
                        host.attrs["_no_bullet"] = "1";   // 作者自带符号：抑制 <li> 自动项目符号
                    return false;
                }

                out = HNode{};
                // 盒模式用 div（BINUI_LIST）而不是 span（LABEL）：只有 LIST 会填底色，
                // LABEL 在三个后端里都只画文字——纯装饰（content:""）的盒子会看不见。
                out.tag = "div";
                out.attrs["_pseudo"] = which;
                if (!text.empty()) out.text = text;
                std::string style;
                for (const auto& kv : merged) {
                    if (kv.first == "content") continue;
                    if (!style.empty()) style += ';';
                    style += kv.first + ':' + kv.second;
                }
                // 没给底色就按透明处理（否则 LIST 的默认灰底会凭空冒出一个色块）
                if (!merged.count("background") && !merged.count("background-color") &&
                    !merged.count("bg")) {
                    style += ";background:transparent";
                    Warn(std::string("CSS ::") + which +
                         " 盒模式未声明 background，已按透明盒子处理（只画内容/尺寸）");
                }
                // 继承宿主字号（CSS 伪元素默认继承）：转成绝对 px 落到合成节点上
                if (!merged.count("font-size") && !merged.count("size")) {
                    const float dfltFs = DefaultFontSizeOf(host.tag);
                    float hfs = dfltFs;
                    std::string fsv = host.attrs.count("font-size") ? host.attrs.at("font-size")
                                    : host.attrs.count("size") ? host.attrs.at("size")
                                    : (hostStyle.count("font-size") ? hostStyle.at("font-size") : "");
                    if (!fsv.empty()) {
                        LenCtx fsCtx{ dfltFs, curCtx.rootFont, curCtx.viewW, curCtx.viewH };
                        auto fp = ParseLenPct(fsv, dfltFs, fsCtx, "font-size");
                        if (fp.ok && fp.value > 0) hfs = fp.value;
                    }
                    style += ";font-size:" + std::to_string((int)std::lround(hfs)) + "px";
                }
                // 继承宿主文字颜色
                if (!merged.count("color") && !merged.count("text-color")) {
                    std::string hc = hostStyle.count("color") ? hostStyle.at("color")
                                   : (host.attrs.count("color") ? host.attrs.at("color") : "");
                    if (!hc.empty()) style += ";color:" + hc;
                }
                out.attrs["style"] = style;
                return true;
            };

            HNode pre, post;
            // 先算两个伪元素（内联模式下它们会改写 host 文本，必须在拷贝宿主之前完成）
            const bool hasPre  = build("before", pre);
            const bool hasPost = build("after", post);
            if (hasPre) {
                pre.parentIdx = expanded.size() + 1;   // 宿主紧随其后
                expanded.push_back(std::move(pre));
                expandedCtx.push_back(CssElemCtx{});
            }
            HNode h = host;
            h.parentIdx = (h.parentIdx == SIZE_MAX) ? SIZE_MAX : remap[h.parentIdx];
            remap[oi] = expanded.size();
            expanded.push_back(std::move(h));
            expandedCtx.push_back(hctx);
            if (hasPost) {
                post.parentIdx = remap[oi];
                expanded.push_back(std::move(post));
                expandedCtx.push_back(CssElemCtx{});
            }
        }
        if (expanded.size() != elems.size()) elems = std::move(expanded);
        elemCtx.swap(expandedCtx);
    }
    // 伪元素展开换过元素表：兄弟索引重建，属性指针重绑（旧指针已随缓冲区失效）。
    // 结构伪类的 idx/cnt 沿用展开前的口径（合成盒不算元素），不在这里重算。
    cssKids = BuildKidIndex(elems);
    for (size_t k = 0; k < elemCtx.size() && k < elems.size(); k++)
        elemCtx[k].attrs = &elems[k].attrs;

    // ---- 3. 根尺寸 ----
    // <app_window>：编译期窗口包裹层（不进节点树），其属性定义窗口描述
    WinDesc wdesc;
    bool hasAppWindow = false;
    for (size_t ci = 0; ci < elems.size(); ci++) {
        const auto& ce = elems[ci];
        if (ce.tag != "app_window") continue;
        if (ce.parentIdx != SIZE_MAX) continue; // 只认顶层
        hasAppWindow = true;
        auto cst = styleOf(ce, ci);
        wdesc.present = true;
        std::string cw = pick(ce.attrs, cst, "width", "width", "");
        std::string ch = pick(ce.attrs, cst, "height", "height", "");
        wdesc.width  = cw.empty() ? 0 : (uint32_t)ParseLenPct(cw, 0).value;
        wdesc.height = ch.empty() ? 0 : (uint32_t)ParseLenPct(ch, 0).value;
        std::string cbg = pick(ce.attrs, cst, "bg", "background", "");
        if (cbg.empty()) cbg = pick(ce.attrs, cst, "color", "background-color", "");
        wdesc.bgColor = cbg.empty() ? 0xFFFFFFFF : parseColor(cbg);
        std::string rs = lowerStr(pick(ce.attrs, cst, "resizable", "resizable", "true"));
        if (rs == "false" || rs == "no" || rs == "0") wdesc.flags &= ~BINUI_WIN_RESIZABLE;
        std::string asp = pick(ce.attrs, cst, "aspect", "aspect", "");
        if (!asp.empty()) {
            wdesc.flags |= BINUI_WIN_ASPECT;
            std::string al = asp, ar = "";
            size_t colon = asp.find(':');
            if (colon != std::string::npos) { al = asp.substr(0, colon); ar = asp.substr(colon + 1); }
            float a = parseLen(al, 0), b = ar.empty() ? 1.0f : parseLen(ar, 1.0f);
            wdesc.aspect = (b != 0) ? a / b : 1.0f;
            if (wdesc.aspect <= 0) wdesc.aspect = 1.0f;
        }
        // window_resize: allow(允许) / disable(禁用) / aspect(仅等比例) —— 覆盖 resizable/aspect
        std::string wrz = lowerStr(pick(ce.attrs, cst, "window_resize", "window_resize", ""));
        if (wrz == "disable" || wrz == "no" || wrz == "0") {
            wdesc.flags &= ~BINUI_WIN_RESIZABLE;
        } else if (wrz == "aspect") {
            wdesc.flags |= BINUI_WIN_RESIZABLE | BINUI_WIN_ASPECT;
        } else if (wrz == "allow" || wrz == "yes" || wrz == "1") {
            wdesc.flags |= BINUI_WIN_RESIZABLE;
            wdesc.flags &= ~BINUI_WIN_ASPECT;
            wdesc.aspect = 0;
        }
        // ui_scale="proportional"：resize 时整棵节点树随窗口等比缩放
        std::string us = lowerStr(pick(ce.attrs, cst, "ui_scale", "ui_scale", ""));
        if (us == "proportional" || us == "fit" || us == "yes" || us == "1")
            wdesc.flags |= BINUI_WIN_SCALE;
        std::string tp = lowerStr(pick(ce.attrs, cst, "transparent", "transparent", ""));
        bool transparent = (tp == "true" || tp == "yes" || tp == "1")
                        || ((wdesc.bgColor >> 24) & 0xFF) < 0xFF; // bg 带 alpha 也视为透明窗口
        if (transparent) wdesc.flags |= BINUI_WIN_TRANSPARENT;
        wdesc.title = pick(ce.attrs, cst, "title", "title", "");
        break;
    }

    float rootW = 1280, rootH = 720;
    if (hasAppWindow && wdesc.width > 0 && wdesc.height > 0) {
        rootW = (float)wdesc.width;
        rootH = (float)wdesc.height;
    } else {
        std::string htmlBg;
        for (size_t hi = 0; hi < elems.size(); hi++) {
            const auto& e = elems[hi];
            if (e.tag != "html" && e.tag != "body") continue;
            auto style = styleOf(e, hi);
            std::string w = pick(e.attrs, style, "width", "width", "");
            std::string h = pick(e.attrs, style, "height", "height", "");
            // 根尺寸支持 %：相对默认窗口（1280x720）；运行时窗口大小由 setSize 驱动
            if (!w.empty()) {
                auto rw = ParseLenPct(w, rootW);
                rootW = rw.pct ? rootW * rw.value / 100.0f : rw.value;
            }
            if (!h.empty()) {
                auto rh = ParseLenPct(h, rootH);
                rootH = rh.pct ? rootH * rh.value / 100.0f : rh.value;
            }
            std::string bg = pick(e.attrs, style, "bg", "background", "");
            if (bg.empty()) bg = pick(e.attrs, style, "color", "background-color", "");
            if (htmlBg.empty() && !bg.empty()) htmlBg = bg;
        }
        // 无 <app_window>：以 <html>/<body> 的尺寸/背景生成窗口描述（外壳窗口默认属性）
        if (!hasAppWindow) {
            wdesc.present = true;
            wdesc.width = (uint32_t)rootW;
            wdesc.height = (uint32_t)rootH;
            if (!htmlBg.empty()) wdesc.bgColor = parseColor(htmlBg);
            if (((wdesc.bgColor >> 24) & 0xFF) < 0xFF) wdesc.flags |= BINUI_WIN_TRANSPARENT;
        }
    }

    // vw/vh 的换算基准 = 编译期视口（窗口描述；无描述时回退根尺寸）
    curCtx.viewW = wdesc.width  ? (float)wdesc.width  : rootW;
    curCtx.viewH = wdesc.height ? (float)wdesc.height : rootH;

    // ---- 4. 生成节点（父级相对定位 + 每父容器独立流式布局） ----
    uint32_t id = 0;
    std::vector<BinUINode> nodes;
    std::vector<BinUISpecNode> specNodes;
    std::vector<BinUISpecFlex> specFlex;
    std::vector<BinUIAnimDef> animDefs;        // v7 动画（@keyframes / animation）
    std::vector<BinUITransition> transitions;  // v7 过渡
    std::vector<std::pair<uint32_t, uint32_t>> nodeNames; // v8 名称表 {nodeId, 字符串索引}
    std::vector<BinUIVideoDesc> videoDescs;    // v9 视频描述段（<video> 节点）
    std::vector<BinUIStateStyle> stateStyles;  // v11 状态样式段（:hover/:active/:focus/:checked）
    std::vector<BinUIPaintNode> paintNodes;    // v13 扩展绘制段（文字色/边框/渐变/字重/行高/装饰）
    std::vector<BinUIShadow> shadowNodes;      // v23 阴影段（box-shadow，绘制顺序 = 下标升序）
    std::vector<BinUITextShadow> textShadowNodes;  // v24 文字阴影段（text-shadow）
    std::vector<BinUIPaintEx> paintExNodes;        // v25 绘制附加段（九宫格图片等）
    std::vector<BinUIImageFilter> filterNodes;     // v26 图片滤镜段（CSS filter 的颜色矩阵）
    std::vector<PendingClip> clipNodes;            // v27 裁剪路径段（clip-path）
    std::vector<PendingMask> maskNodes;            // v28 遮罩段（<mask>）
    std::vector<BinUIBackdropEntry> backdropNodes; // v29 毛玻璃段（backdrop-filter）
    std::vector<BinUIFilterDropShadowEntry> dropShadowNodes; // v33 外阴影段（filter: drop-shadow）
    std::vector<PendingPath> pendingPaths;     // v15 路径段（SVG <path>/<ellipse> 的折线）
    std::vector<std::string> strings;
    strings.push_back(""); // 字符串表首位 = 空串：无文本节点的 textOffset=0 指向空串
    std::unordered_map<uint32_t, std::string> textMap;

    auto getOrAddString = [&](const std::string& s) -> uint32_t {
        for (uint32_t i = 0; i < strings.size(); i++)
            if (strings[i] == s) return i;
        strings.push_back(s);
        return (uint32_t)(strings.size() - 1);
    };

    // 布局上下文：每个容器一个；坐标相对其内容盒（父原点 + padding）
    struct Ctx {
        uint16_t nodeId;
        float    ox, oy;      // 内容盒原点（根坐标）
        float    cursorY;     // 本容器流式光标（相对 oy）
        float    contentW;    // 内容宽度（节点宽 - 2*padding）
        size_t   nodeIdx;     // 对应 nodes 下标（用于自动扩展高度）
        uint32_t textColor;   // 生效文字色（CSS color 的继承值）
        bool     hasText;     // 祖先是否显式给过文字色
        bool     flow;        // 这个容器是被父级流光标摆进去的（不是显式 y / flex）
        // v0.16.40：生效的 letter-spacing（像素，0 = 未声明）。CSS 里它是**可继承**属性，
        // 所以走 Ctx 而不是每次重新从 style 里查。放在 flow 之后 = 聚合初始化的第 10 项。
        float    letterSpacing = 0.0f;
        // ---- SVG（v15）----
        // 约定：绝对坐标 = (ox, oy) + svgXform × 用户坐标。
        // svgXform 只描述"用户坐标 → 本容器内容盒局部坐标"，把 viewBox 与
        // 祖先 <g transform> 全部折进来，所以后面的几何代码还是只加 ox/oy。
        PathMatrix svgXform{};
        bool     inSvg = false;                       // 在 <svg> 子树里才受 SVG 语义影响
        uint32_t fillColor = 0xFF000000u;             // SVG 默认：黑色填充
        bool     hasFill = true;
        uint32_t strokeColor = 0xFF000000u;           // SVG 默认：无描边
        bool     hasStroke = false;
        uint8_t  strokeW = 1;
        bool     evenOdd = false;                     // fill-rule="evenodd"
        // 当前 <svg> 视口在**用户单位**下的尺寸：userSpaceOnUse 里写百分比时要按它折算，
        // 例如 x1="50%" 等于 viewport 宽度的一半（SVG 规范）。没有 viewBox 时取 width/height。
        float    vpW = 0.0f, vpH = 0.0f;
        // v19：shape-rendering（可继承）。true = auto/optimizeQuality（边缘抗锯齿），
        // false = crispEdges/optimizeSpeed（硬边，也是 binUI 的默认）。
        bool     aa = false;
        // v16：fill 用的渐变 id（空 = 纯色）。渐变是继承属性，<svg>/<g> 上写一次全子树生效；
        // fillColor 同时存"中间色标的代表色"，供运行期兜底与不支持渐变的通路使用。
        std::string fillGradId{};
        // v17：stroke 用的渐变 id（同样可继承）。子元素写纯色 stroke 时会被清掉。
        std::string strokeGradId{};
        // v18：fill / stroke 是否被"显式"给过（元素自己或祖先）。
        // 用途：裸 <polyline points="…"> 按 SVG 默认是"黑填充"，但现成素材里几乎都是手误，
        // 凭空多一个黑色色块比什么都不画更难查 —— 只画显式给了颜料的（polygon/polyline 用）。
        bool fillStated = false;
        bool strokeStated = false;
        // "fill 被提到过"（写成 none 也算）：用来区分"作者明确说不要填充"和"压根没写"。
        // 两者在 SVG 里含义不同 —— 没写 = 默认黑填充，写了 none = 不填。
        bool fillMentioned = false;
        // v24：生效的文字阴影（CSS text-shadow 是**继承属性**：卡片上写一次，卡片里
        // 所有文字都带）。空 = 本子树没有阴影；写了 none / 全透明就从这里清掉。
        // v0.16.50：**多层**（数组顺序 = 绘制顺序，见 binui_format.h 的段注释）。
        std::vector<BinUITextShadow> tsLayers{};
    };

    // 根节点固定 id 0，位于坐标原点。画布默认白色；<app_window bg>/<html bg> 覆盖
    BinUINode root = {};
    root.id = 0; root.parentId = 0xFFFF;
    root.type = BINUI_ROOT; root.flags = BINUI_VISIBLE;
    root.x = 0; root.y = 0; root.width = rootW; root.height = rootH;
    root.color = hasAppWindow ? wdesc.bgColor : 0xFFFFFFFF;
    nodes.push_back(root);
    id = 1; // 根已占用 id 0

    // 默认文字色：按根底色亮度自适应（浅底深字 / 深底浅字）。
    // 以前这里硬编码白色 —— 而根底色默认是白，于是"没写 color 的页面"整页白底白字，
    // 只有作者到处写 color 才看得见。深色主题亮度低仍取白字，现有界面观感不变；
    // 根背景带 alpha（透明窗口）时不知道桌面是什么色，沿用白色。
    uint32_t defaultTextCol = 0xFFFFFFFFu;
    {
        const uint32_t rootBg = root.color;
        if (((rootBg >> 24) & 0xFFu) >= 0x80u) {
            const float lum = (0.2126f * (float)((rootBg >> 16) & 0xFFu) +
                               0.7152f * (float)((rootBg >> 8) & 0xFFu) +
                               0.0722f * (float)(rootBg & 0xFFu)) / 255.0f;
            defaultTextCol = (lum > 0.5f) ? 0xFF1A1A1Au : 0xFFFFFFFFu;
        }
    }

    std::vector<Ctx> stack;
    // 栈深最多 = 元素数 + 1（每个元素最多压一层）：一次性预留，push_back 就不会扩容。
    // 扩容会把下面 `Ctx& pc = stack.back()` 弄成悬空引用（踩过：读到已释放的视口宽高，
    // 走到 0xC0000005）。
    stack.reserve(elems.size() + 1);
    stack.push_back({ 0, 0, 0, 0, rootW, 0, 0xFFFFFFFFu, false, false });

    // viewBox="minX minY w h"：四个数（SVG 允许逗号/空白分隔）
    auto ParseViewBox = [](const std::string& s, float& vx, float& vy,
                           float& vw, float& vh) -> bool {
        float v[4] = { 0, 0, 0, 0 };
        int n = 0;
        std::string cur;
        auto flush = [&]() {
            if (cur.empty() || n >= 4) return;
            v[n++] = static_cast<float>(std::strtod(cur.c_str(), nullptr));
            cur.clear();
        };
        for (char c : s) {
            if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') flush();
            else cur += c;
        }
        flush();
        if (n < 4) return false;
        vx = v[0]; vy = v[1]; vw = v[2]; vh = v[3];
        return vw > 0.0f && vh > 0.0f;
    };
    // 元素是否落在某个 <svg> 子树里（健康检查要用：transform/viewBox 的告警只对 SVG 之外报）
    auto InSvgSubtree = [&](size_t idx) -> bool {
        size_t guard = elems.size() + 1;
        while (idx != SIZE_MAX && idx < elems.size() && guard-- > 0) {
            if (lowerStr(elems[idx].tag) == "svg") return true;
            idx = elems[idx].parentIdx;
        }
        return false;
    };

    // 容器高度不足时自动扩展到容纳流式内容（滚动容器保持固定视口）
    auto growToContent = [&](Ctx& c) {
        BinUINode& cn = nodes[c.nodeIdx];
        if (cn.flags & BINUI_SCROLLABLE) return;
        float need = c.cursorY + cn.padding * 2;
        if (need > cn.height) cn.height = need;
    };

    // 弹出一个容器，并把它"被内容撑高"的增量补到父容器的流光标上。
    // 不补的话：后声明的兄弟会按旧高度接着排，直接压在这个容器身上
    // （卡片里放几行字，后面的元素就盖上来 —— 以前只能手写死高度绕开）。
    auto popAndPropagate = [&]() {
        const size_t top = stack.size() - 1;
        const float before = nodes[stack[top].nodeIdx].height;
        growToContent(stack[top]);
        const float grew = nodes[stack[top].nodeIdx].height - before;
        const bool flow = stack[top].flow;
        stack.pop_back();
        if (grew > 0.5f && flow && !stack.empty())
            stack.back().cursorY += grew;
    };

    // elems 索引 -> 节点 id（被跳过的元素为 0xFFFF）
    std::vector<uint32_t> hIdxToNodeId(elems.size(), 0xFFFF);
    auto resolveParentId = [&](size_t idx) -> uint32_t {
        while (idx != SIZE_MAX && idx < elems.size()) {
            if (hIdxToNodeId[idx] != 0xFFFF) return hIdxToNodeId[idx];
            idx = elems[idx].parentIdx;
        }
        return 0xFFFF;
    };

    // 预处理：每个元素的直接子元素索引（HTML 解析器已按深度优先排好序）
    std::vector<std::vector<size_t>> childrenOf(elems.size());
    for (size_t i = 0; i < elems.size(); i++) {
        size_t p = elems[i].parentIdx;
        if (p < elems.size()) childrenOf[p].push_back(i);
    }

    // flex 子项属性（v22）：CSS 解析结果 + 规格段回写需要的基准值
    struct FlexRec {
        float   grow      = 0.0f;
        float   shrink    = 1.0f;
        uint8_t alignSelf = 0xFF;   // 0xFF = 跟随容器
        uint8_t order     = 0;
        uint8_t basisKind = 0;      // 0=无 1=像素 2=百分比（0..1）
        float   basisVal  = 0.0f;   // 主轴基准（按 basisKind 解释）
        float   crossPx   = 0.0f;   // 交叉轴基准尺寸（像素）
        bool    crossAuto = false;  // 交叉轴尺寸是默认值（可被 stretch 拉伸）
        bool    content   = false;  // 主轴基准是内容估算
    };

    // flex 布局（编译期）：容器内容盒 (ox,oy,cw,ch)，子项坐标相对内容盒写入 out。
    // 算法本身在 include/binui_flex_layout.h —— 运行期重排用的是同一份，别在这里另写一遍。
    // 返回容器内容盒所需的**交叉轴长度**（容器没声明高度/宽度时拿它撑开自己）。
    auto layoutFlex = [&](const std::vector<size_t>& kids,
                          float cw, float ch,
                          const std::string& dir, const std::string& justify,
                          const std::string& align, const std::string& alignContent,
                          uint8_t wrap, float gap, bool crossDefinite,
                          std::unordered_map<size_t, float[4]>& out,
                          std::unordered_map<size_t, FlexRec>& recs) -> float {
        const bool row = (dir != "column");
        // 只收集会生成节点的子项：跳过 display:none 与 head/meta/script 等非可视元素
        std::vector<size_t> active;
        for (size_t k : kids) {
            if (hIdxToNodeId[k] != 0xFFFF) continue; // 已生成节点（父前已处理）不在 flex 子项中
            const auto& ke = elems[k];
            auto kst = styleOf(ke, k);
            std::string kdisp = lowerStr(pick(ke.attrs, kst, "display", "display", ""));
            if (kdisp == "none") continue;
            if (ke.tag=="head" || ke.tag=="title" || ke.tag=="meta" || ke.tag=="link" ||
                ke.tag=="script" || ke.tag=="style" || ke.tag=="app_window") continue;
            // position:fixed 脱离流（含定位盒是视口），不该占 flex 的一个槽位：
            // 不滤掉的话，一个右下角的悬浮按钮会在按钮行里凭空挤出 120px 的空位。
            const std::string kpos = lowerStr(kst.count("position") ? kst.at("position") : "");
            if (kpos == "fixed") continue;
            active.push_back(k);
        }
        size_t n = active.size();
        std::vector<binui_flex::Item> items(n);

        // ---- CSS 取值小工具 ----
        auto alignCode = [](const std::string& v) -> uint8_t {
            if (v == "center")  return binui_flex::AL_CENTER;
            if (v == "flex-end" || v == "end") return binui_flex::AL_END;
            if (v == "stretch") return binui_flex::AL_STRETCH;
            return binui_flex::AL_START;   // flex-start / baseline / 其它
        };
        // 纯数字 token（flex-grow / flex-shrink / order）
        auto numTok = [](const std::string& v, float def) -> float {
            const std::string t = TrimCopy(v);
            if (t.empty()) return def;
            char* end = nullptr;
            const float r = std::strtof(t.c_str(), &end);
            if (end == t.c_str() || !std::isfinite(r)) return def;
            while (*end == ' ' || *end == '\t') end++;
            if (*end != '\0') return def;
            return r;
        };
        // flex-basis 取值：auto / content / px / %（kind：0=auto 1=像素 2=百分比 3=content）
        auto basisTok = [&](const std::string& raw, int& kind, float& val) {
            const std::string t = LowerCopy(TrimCopy(raw));
            kind = 0; val = 0.0f;
            if (t.empty() || t == "auto") return;
            if (t == "content") { kind = 3; return; }
            auto p = ParseLenPct(t, 0, curCtx, "flex-basis");
            if (p.pct) { kind = 2; val = p.value / 100.0f; }
            else { kind = 1; val = p.value; }
        };

        const uint8_t containerAlign = alignCode(align);
        const int jc = (justify == "flex-end") ? binui_flex::J_END
                     : (justify == "center") ? binui_flex::J_CENTER
                     : (justify == "space-between") ? binui_flex::J_BETWEEN
                     : (justify == "space-around") ? binui_flex::J_AROUND
                     : (justify == "space-evenly") ? binui_flex::J_EVENLY : binui_flex::J_START;
        const int ac = (alignContent == "center") ? binui_flex::AC_CENTER
                     : (alignContent == "flex-end" || alignContent == "end") ? binui_flex::AC_END
                     : (alignContent == "space-between") ? binui_flex::AC_BETWEEN
                     : (alignContent == "space-around") ? binui_flex::AC_AROUND
                     : (alignContent == "space-evenly") ? binui_flex::AC_EVENLY
                     : (alignContent == "stretch") ? binui_flex::AC_STRETCH : binui_flex::AC_START;

        for (size_t i = 0; i < n; i++) {
            size_t k = active[i];
            const auto& e = elems[k];
            auto st = styleOf(e, k);
            const uint8_t ktype = tagToType(e.tag);

            // 子项字号（本项 em 的换算基准）
            {
                const float dfltFs = DefaultFontSizeOf(e.tag);
                std::string fsv = pick(e.attrs, st, "size", "font-size", "");
                curCtx.fontSize = dfltFs;
                if (!fsv.empty()) {
                    LenCtx fsCtx{ dfltFs, curCtx.rootFont, curCtx.viewW, curCtx.viewH };
                    auto fp = ParseLenPct(fsv, dfltFs, fsCtx, "font-size");
                    if (fp.ok && fp.value > 0) curCtx.fontSize = fp.value;
                }
            }

            std::string sw = pick(e.attrs, st, "w", "width", "");
            if (sw.empty()) sw = pick(e.attrs, st, "width", "width", "");
            std::string sh = pick(e.attrs, st, "h", "height", "");
            if (sh.empty()) sh = pick(e.attrs, st, "height", "height", "");
            if (lowerStr(sw) == "auto") sw.clear();
            if (lowerStr(sh) == "auto") sh.clear();
            // width:content = 按文字宽度排（flex 里没有"宽度自适应"，光靠固定像素
            // 要么留一条大缝、要么把字裁掉）。用编译器自己的字宽估算，与换行同一套度量。
            const bool wContent = lowerStr(sw) == "content";
            if (wContent) sw.clear();
            const float contentPx = estTextWidth(e.text, curCtx.fontSize) * 1.08f + 4.0f;   // 字间距在这里不参与（仅估算槽位宽）

            // ---- flex 简写 / 长写属性 ----
            FlexRec rec;
            float grow = 0.0f, shrink = 1.0f;
            bool  flexGiven = false, basisGiven = false;
            int   bkind = 0;
            float bval = 0.0f;
            if (auto fit = st.find("flex"); fit != st.end()) {
                flexGiven = true;
                const auto toks = splitCssTokens(fit->second);
                const std::string first = toks.empty() ? std::string() : lowerStr(toks[0]);
                if (first == "none")         { grow = 0.0f; shrink = 0.0f; }
                else if (first == "auto")    { grow = 1.0f; shrink = 1.0f; }
                else if (first == "initial") { grow = 0.0f; shrink = 1.0f; }
                else {
                    // 带单位的 token = 主轴基准；纯数字按顺序是 grow、shrink
                    int numSeen = 0;
                    for (const auto& tok : toks) {
                        const std::string t = lowerStr(tok);
                        const float num = numTok(t, std::numeric_limits<float>::quiet_NaN());
                        if (std::isfinite(num)) {
                            if (numSeen == 0) grow = num;
                            else if (numSeen == 1) shrink = num;
                            else if (!basisGiven) {
                                // 第三个数字当主轴基准（CSS 要求带单位；binUI 全线允许无单位长度，
                                // 这里同样宽松：`flex:1 1 120` 就是 basis 120px）
                                bkind = 1; bval = num; basisGiven = true;
                            }
                            numSeen++;
                        } else {
                            basisTok(t, bkind, bval);
                            basisGiven = true;
                        }
                    }
                    // CSS 的 flex:<number>：主轴基准取 0（不是 auto）
                    if (numSeen > 0 && !basisGiven) { bkind = 1; bval = 0.0f; basisGiven = true; }
                }
            }
            if (auto lt = st.find("flex-grow"); lt != st.end())   { flexGiven = true; grow = numTok(lt->second, grow); }
            if (auto lt = st.find("flex-shrink"); lt != st.end()) { flexGiven = true; shrink = numTok(lt->second, shrink); }
            if (auto lt = st.find("flex-basis"); lt != st.end()) {
                flexGiven = true;
                basisTok(lt->second, bkind, bval);
                basisGiven = true;
            }

            // ---- 主轴基准 / 交叉轴尺寸 ----
            // row：主轴 = 宽、交叉 = 高；column：主轴 = 高、交叉 = 宽
            auto resolveLen = [&](const std::string& decl, float ref, const char* what,
                                  float& px, float& pctVal) -> bool {
                px = 0.0f; pctVal = -1.0f;
                if (decl.empty()) return false;
                auto p = ParseLenPct(decl, 0, curCtx, what);
                if (p.pct) { pctVal = p.value / 100.0f; px = pctVal * ref; }
                else px = p.value;
                return true;
            };

            float cmp = 0.0f, cpct = -1.0f;
            bool crossDeclared = false;
            if (row) {
                crossDeclared = resolveLen(sh, ch, "flex 子项 height", cmp, cpct);
                if (!crossDeclared) cmp = defaultHeight(e.tag, ktype);
            } else if (wContent) {
                crossDeclared = true;              // width:content → 交叉轴按内容宽
                cmp = contentPx;
            } else {
                crossDeclared = resolveLen(sw, cw, "flex 子项 width", cmp, cpct);
                if (!crossDeclared) cmp = cw;      // 未声明宽 = 拉伸到内容盒（既有行为）
            }

            float mmp = 0.0f, mpct = -1.0f;
            const bool mainDeclared = row
                ? resolveLen(sw, cw, "flex 子项 width", mmp, mpct)
                : resolveLen(sh, ch, "flex 子项 height", mmp, mpct);

            float basisPx = 0.0f;
            uint8_t specKind = 0;
            float   specVal  = 0.0f;
            const float mainRef = row ? cw : ch;
            auto useDeclared = [&]() {
                if (mpct >= 0.0f) { specKind = 2; specVal = mpct; basisPx = mmp; }
                else { specKind = 1; specVal = mmp; basisPx = mmp; }
            };
            auto useContent = [&]() {
                specKind = 1; specVal = contentPx; basisPx = contentPx; rec.content = true;
            };
            if (basisGiven) {
                if (bkind == 1)      { specKind = 1; specVal = bval; basisPx = bval; }
                else if (bkind == 2) { specKind = 2; specVal = bval; basisPx = bval * mainRef; }
                else if (bkind == 3) { useContent(); }
                else if (mainDeclared) { useDeclared(); }
                else if (!e.text.empty()) { useContent(); }
            } else if (mainDeclared) {
                useDeclared();
            } else if (row && wContent) {
                useContent();
            } else if (row) {
                // 只写了 flex 长写属性（grow/shrink）而没给基准：有文字就按内容宽当基准
                // （CSS 的 flex-basis:auto，`flex-shrink:0` 这种写法才有意义）。
                // 什么都没写的子项走 binUI 的便利行为 flex:1 —— 既有布局全依赖它。
                if (flexGiven && !e.text.empty()) useContent();
                else if (grow <= 0.0f) grow = 1.0f;
            } else {
                // column：没写高度的按默认盒高，不参与主轴伸缩
                specKind = 1; specVal = defaultHeight(e.tag, ktype); basisPx = specVal;
            }

            // 尺寸约束：row 的主轴约束是 width 那组，column 的主轴约束是 height 那组
            auto con = [&](const std::string& key, float& o, uint8_t& pct) {
                std::string v = pick(e.attrs, st, key, key, "");
                if (lowerStr(v) == "auto") v.clear();
                if (v.empty()) { o = -1; return; }
                auto p = ParseLenPct(v, 0, curCtx, key.c_str());
                if (p.pct) { o = p.value / 100.0f; pct = 1; }
                else o = p.value;
            };
            float mnw = -1, mxw = -1, mnh = -1, mxh = -1;
            uint8_t mnwp = 0, mxwp = 0, mnhp = 0, mxhp = 0;
            con("min-width",  mnw, mnwp);
            con("max-width",  mxw, mxwp);
            con("min-height", mnh, mnhp);
            con("max-height", mxh, mxhp);
            const float crossRef = row ? ch : cw;
            auto pxOf = [](float v, uint8_t pct, float ref) -> float {
                if (v < 0.0f) return -1.0f;
                return pct ? v * ref : v;
            };
            binui_flex::Item& it = items[i];
            it.basis    = basisPx;
            it.cross    = cmp;
            it.minMain  = row ? pxOf(mnw, mnwp, mainRef)  : pxOf(mnh, mnhp, mainRef);
            it.maxMain  = row ? pxOf(mxw, mxwp, mainRef)  : pxOf(mxh, mxhp, mainRef);
            it.minCross = row ? pxOf(mnh, mnhp, crossRef) : pxOf(mnw, mnwp, crossRef);
            it.maxCross = row ? pxOf(mxh, mxhp, crossRef) : pxOf(mxw, mxwp, crossRef);
            it.grow     = grow;
            it.shrink   = shrink;

            // align-self / order
            uint8_t alignSelf = 0xFF;
            if (auto lt = st.find("align-self"); lt != st.end()) {
                const std::string v = lowerStr(TrimCopy(lt->second));
                if (v != "auto") alignSelf = alignCode(v);
            }
            uint8_t order = 0;
            if (auto lt = st.find("order"); lt != st.end())
                order = (uint8_t)std::lround(std::clamp(numTok(lt->second, 0.0f), 0.0f, 255.0f));
            it.alignSelf = alignSelf;
            it.order     = order;
            // 交叉轴拉伸：作者写了 stretch；或者 column 里压根没写宽度（既有行为）。
            // 声明了交叉轴尺寸的项不会被拉伸（CSS 也一样）。
            it.stretch = !crossDeclared &&
                         (alignSelf == binui_flex::AL_STRETCH ||
                          containerAlign == binui_flex::AL_STRETCH || !row);

            rec.grow = grow; rec.shrink = shrink;
            rec.alignSelf = alignSelf; rec.order = order;
            rec.basisKind = specKind; rec.basisVal = specVal;
            rec.crossPx = cmp; rec.crossAuto = !crossDeclared;
            recs[k] = rec;
        }

        binui_flex::Cfg cfg;
        cfg.row          = row;
        cfg.wrap         = wrap;
        cfg.justify      = (uint8_t)jc;
        cfg.align        = containerAlign;
        cfg.alignContent = (uint8_t)ac;
        cfg.gap          = gap;
        cfg.mainLen      = row ? cw : ch;
        cfg.crossLen     = row ? ch : cw;
        cfg.crossDefinite = crossDefinite;

        std::vector<binui_flex::Rect> rects(n);
        const float need = binui_flex::Layout(cfg, items.data(), n, rects.data());
        for (size_t i = 0; i < n; i++) {
            out[active[i]][0] = rects[i].x;
            out[active[i]][1] = rects[i].y;
            out[active[i]][2] = rects[i].w;
            out[active[i]][3] = rects[i].h;
        }
        return need;
    };

    // 容器元素索引 -> 已计算的 flex 子项位置（elemIdx -> x/y/w/h）与子项属性
    std::unordered_map<size_t, std::unordered_map<size_t, float[4]>> flexLayouts;
    std::unordered_map<size_t, std::unordered_map<size_t, FlexRec>> flexProps;
    // column 方向的 flex 容器（元素索引）。用来判断"本元素的主轴尺寸是不是被 flex 定死的"：
    // column 下的 flex:N 子项，高度由剩余空间决定，不能再被"未声明高度 → 按内容撑开"改写。
    std::unordered_set<size_t> columnFlexParents;

    // ============================================================
    // HTML 标签/属性体检（只在 HTML 模式跑；XML 允许自定义标签）
    // ------------------------------------------------------------
    // CSS 那边早就有 kIgnoredCss 这类"写了没效果就明说"的机制，HTML 这边一条都没有，
    // 于是 <u>/<mark>/placeholder/disabled 这些东西默默编成别的东西也没人知道。
    // 这里只做体检、不改行为：不改 HTML 的容错性（未知标签照样按容器处理）。
    // 每个标签/属性全文档只报一次，避免刷屏。
    // ============================================================
// 把一个 SVG 形状元素（path/polyline/polygon/line/rect/circle/ellipse）解析成折线，
// 并应用它自己的 transform。<marker> 内容与 <clipPath> 内容共用这一份。
auto shapeOf = [&](const HNode& ce, PathShape& cs, float tol = 0.1f) -> bool {
    const std::string ct = lowerStr(ce.tag);
    std::string perr;
    if (ct == "path") {
        const std::string d = ce.attrs.count("d") ? ce.attrs.at("d") : "";
        if (d.empty() || !ParsePathData(d, tol, cs, &perr)) return false;
    } else if (ct == "polyline" || ct == "polygon") {
        const std::string pts = ce.attrs.count("points") ? ce.attrs.at("points") : "";
        if (pts.empty()) return false;
        ParsePathData("M" + pts + (ct == "polygon" ? " Z" : ""), tol, cs, &perr);
    } else if (ct == "line") {
        const float x1 = parseLen(ce.attrs.count("x1") ? ce.attrs.at("x1") : "", 0);
        const float y1 = parseLen(ce.attrs.count("y1") ? ce.attrs.at("y1") : "", 0);
        const float x2 = parseLen(ce.attrs.count("x2") ? ce.attrs.at("x2") : "", 0);
        const float y2 = parseLen(ce.attrs.count("y2") ? ce.attrs.at("y2") : "", 0);
        PathShape::Contour c2;
        c2.first = 0; c2.count = 2; c2.closed = false;
        cs.pts.push_back({ x1, y1 });
        cs.pts.push_back({ x2, y2 });
        cs.contours.push_back(c2);
        cs.RecomputeBounds();
    } else if (ct == "ellipse" || ct == "circle") {
        const float cx = parseLen(ce.attrs.count("cx") ? ce.attrs.at("cx") : "", 0);
        const float cy = parseLen(ce.attrs.count("cy") ? ce.attrs.at("cy") : "", 0);
        const float rx = ct == "circle"
            ? parseLen(ce.attrs.count("r") ? ce.attrs.at("r") : "", 0)
            : parseLen(ce.attrs.count("rx") ? ce.attrs.at("rx") : "", 0);
        const float ry = ct == "circle" ? rx
            : parseLen(ce.attrs.count("ry") ? ce.attrs.at("ry") : "", 0);
        if (rx <= 0.0f || ry <= 0.0f) return false;
        AppendEllipse(cs, cx, cy, rx, ry, tol);
    } else if (ct == "rect") {
        const float rx0 = parseLen(ce.attrs.count("x") ? ce.attrs.at("x") : "", 0);
        const float ry0 = parseLen(ce.attrs.count("y") ? ce.attrs.at("y") : "", 0);
        const float rw0 = parseLen(ce.attrs.count("width") ? ce.attrs.at("width") : "", 0);
        const float rh0 = parseLen(ce.attrs.count("height") ? ce.attrs.at("height") : "", 0);
        float cr = 0.0f;
        if (ce.attrs.count("rx")) cr = parseLen(ce.attrs.at("rx"), 0);
        else if (ce.attrs.count("ry")) cr = parseLen(ce.attrs.at("ry"), 0);
        if (rw0 <= 0.0f || rh0 <= 0.0f) return false;
        AppendRoundedRect(cs, rx0, ry0, rw0, rh0, cr, tol);
    } else {
        return false;
    }
    if (cs.Empty()) return false;
    auto tfIt = ce.attrs.find("transform");
    if (tfIt != ce.attrs.end() && !tfIt->second.empty()) {
        PathMatrix cm;
        if (ParseSvgTransform(tfIt->second, cm, nullptr)) TransformShape(cs, cm);
    }
    return true;
};

    // ============ v0.16.23：SVG <marker>（编译期展开） ============
    // marker 是**静态几何**：每个实例的形状只取决于顶点位置与切线方向，所以在编译期
    // 把它换算成引用路径节点里的**额外子路径** —— 运行期与三个后端零改动。
    // 支持 marker-start / marker-mid / marker-end（含 CSS 简写 marker）、viewBox、
    //   refX/refY、markerWidth/markerHeight、markerUnits、orient（auto /
    //   auto-start-reverse / 固定角度）；marker 内容支持 path / polygon / polyline /
    //   line / rect / circle / ellipse。
    // 已知边界：preserveAspectRatio 按非等比拉伸（不是 meet 居中）；marker 里的 <g>
    //   不展开；一条路径只有一个 fill/stroke，所以各实例统一用引用路径的颜料
    //   （marker 内容自带 fill 时忽略并告警）。
    struct SvgMarkerChild {
        PathShape shape;
        bool hasFill = false;   uint32_t fill = 0;   bool fillCurrent = false;
        bool hasStroke = false; uint32_t stroke = 0; bool strokeCurrent = false;
        float strokeW = 0.0f;
    };
    struct SvgMarkerDef {
        bool found = false;
        float vbX = 0, vbY = 0, vbW = -1, vbH = -1;
        float refX = 0, refY = 0;
        float mw = 3, mh = 3;
        bool  userUnits = false;
        int   orient = 0;        // 0=auto  1=auto-start-reverse  2=固定角度
        float angle = 0;
        std::vector<SvgMarkerChild> kids;
    };
    std::unordered_map<std::string, SvgMarkerDef> svgMarkers;
    for (size_t mi = 0; mi < elems.size(); mi++) {
        if (lowerStr(elems[mi].tag) != "marker") continue;
        const HNode& me = elems[mi];
        std::string mid = me.attrs.count("id") ? TrimCopy(me.attrs.at("id")) : "";
        if (mid.empty()) {
            Warn("<marker> 没有 id，无法被 marker-start/mid/end 引用（已忽略）");
            continue;
        }
        SvgMarkerDef md;
        md.found = true;
        auto num = [&](const char* a, float dflt) -> float {
            auto it = me.attrs.find(a);
            return it == me.attrs.end() ? dflt : parseLen(it->second, dflt);
        };
        md.refX = num("refx", 0.0f);
        md.refY = num("refy", 0.0f);
        md.mw = std::max(0.0f, num("markerwidth", 3.0f));
        md.mh = std::max(0.0f, num("markerheight", 3.0f));
        {
            auto it = me.attrs.find("markerunits");
            md.userUnits = it != me.attrs.end() &&
                           lowerStr(TrimCopy(it->second)) == "userspaceonuse";
        }
        if (me.attrs.count("viewbox")) {
            std::string vb = TrimCopy(me.attrs.at("viewbox")) + " ";
            float v[4] = { 0, 0, -1, -1 };
            int got = 0;
            std::string cur;
            for (char ch : vb) {
                if (ch == ' ' || ch == ',' || ch == '\t') {
                    if (!cur.empty() && got < 4) v[got++] = parseLen(cur, 0.0f);
                    cur.clear();
                } else cur += ch;
            }
            if (got >= 4 && v[2] > 0.0f && v[3] > 0.0f) {
                md.vbX = v[0]; md.vbY = v[1]; md.vbW = v[2]; md.vbH = v[3];
            } else {
                Warn("<marker id=\"" + mid + "\"> 的 viewBox 解析失败（按无 viewBox 处理）");
            }
        }
        {
            auto it = me.attrs.find("orient");
            const std::string o = it == me.attrs.end() ? "" : lowerStr(TrimCopy(it->second));
            if (o == "auto-start-reverse") md.orient = 1;
            else if (o == "auto" || o.empty()) md.orient = 0;
            else { md.orient = 2; md.angle = parseLen(o, 0.0f, "orient"); }
        }
        for (size_t ci = 0; ci < elems.size(); ci++) {
            if (elems[ci].parentIdx != mi) continue;
            const HNode& ce = elems[ci];
            const std::string ct = lowerStr(ce.tag);
            if (ct == "g") {
                Warn("<marker id=\"" + mid + "\"> 里的 <g> 不展开（把形状直接写在 <marker> 下）");
                continue;
            }
            PathShape cs;
            if (!shapeOf(ce, cs)) continue;
            SvgMarkerChild kid;
            kid.shape = std::move(cs);
            auto ft = ce.attrs.find("fill");
            if (ft != ce.attrs.end() && !TrimCopy(ft->second).empty()) {
                const std::string fv = TrimCopy(ft->second);
                const std::string fl = lowerStr(fv);
                if (fl == "currentcolor" || fl == "context-stroke" || fl == "context-fill") {
                    kid.hasFill = true; kid.fillCurrent = true;
                } else if (fl != "none") {
                    kid.hasFill = true;
                    kid.fill = parseColorWith(fv, 0xFF000000u, "SVG 颜色");
                }
            } else {
                kid.hasFill = true; kid.fillCurrent = true;   // 不写 = 沿用引用处颜料
            }
            auto st = ce.attrs.find("stroke");
            if (st != ce.attrs.end()) {
                const std::string sv = TrimCopy(st->second);
                if (lowerStr(sv) != "none" && !sv.empty()) {
                    kid.hasStroke = true;
                    kid.strokeCurrent = lowerStr(sv) == "currentcolor";
                    if (!kid.strokeCurrent) kid.stroke = parseColorWith(sv, 0xFF000000u, "SVG 颜色");
                    kid.strokeW = parseLen(ce.attrs.count("stroke-width")
                                               ? ce.attrs.at("stroke-width") : "", 0.0f);
                }
            }
            md.kids.push_back(std::move(kid));
        }
        if (md.kids.empty())
            Warn("<marker id=\"" + mid + "\"> 里没有可绘制的形状（marker 为空）");
        svgMarkers[mid] = std::move(md);
    }

    // ============ v0.16.24：SVG <clipPath> ============
    // clipPath 的内容是**静态几何**，所以和 marker 一样在编译期折算成"设备空间里的一条
    // 折线"，写进裁剪路径段（v27）。运行期只做两件事：把节点画进离屏图层，再用
    // "图层矩形 + 这条路径"的 even-odd 填充把外面擦掉 —— 任意形状都能裁。
    struct SvgClipDef {
        bool evenOdd = false;
        int  units = 0;          // 0 = userSpaceOnUse（默认）  1 = objectBoundingBox
        std::vector<size_t> kids;   // 子形状下标：用到时按 units 选容差解析
    };
    std::unordered_map<std::string, SvgClipDef> svgClips;
    for (size_t ci = 0; ci < elems.size(); ci++) {
        if (lowerStr(elems[ci].tag) != "clippath") continue;
        const HNode& ce2 = elems[ci];
        std::string cid = ce2.attrs.count("id") ? TrimCopy(ce2.attrs.at("id")) : "";
        if (cid.empty()) {
            Warn("<clipPath> 没有 id，无法被 clip-path 引用（已忽略）");
            continue;
        }
        SvgClipDef cd;
        {
            auto it = ce2.attrs.find("clippathunits");
            if (it != ce2.attrs.end())
                cd.units = lowerStr(TrimCopy(it->second)) == "objectboundingbox" ? 1 : 0;
        }
        for (size_t cc = 0; cc < elems.size(); cc++) {
            if (elems[cc].parentIdx != ci) continue;
            const std::string ct = lowerStr(elems[cc].tag);
            if (ct == "g") {
                Warn("<clipPath id=\"" + cid + "\"> 里的 <g> 不展开（把形状直接写在 <clipPath> 下）");
                continue;
            }
            cd.kids.push_back(cc);
        }
        if (cd.kids.empty())
            Warn("<clipPath id=\"" + cid + "\"> 里没有可用的形状（裁剪路径为空）");
        svgClips[cid] = std::move(cd);
    }

    // ============ v0.16.25：SVG <pattern>（编译期平铺展开） ============
    // 和 marker / clipPath 一条路：pattern 的内容是**静态几何**，铺几块、每块落在哪，
    // 编译期就能算死 —— 于是展开成若干条普通路径，运行期与三个后端零改动。
    // tile 必须被元素自身轮廓裁掉（否则图案会溢出到元素外），这里直接借 v27 的裁剪
    // 路径段：每个展开出来的节点挂一条"元素轮廓"的裁剪记录。
    // 支持 width/height/x/y、patternUnits、patternContentUnits、viewBox（meet 居中）、
    //   patternTransform（平移+缩放）；内容支持 path/polygon/polyline/line/rect/
    //   circle/ellipse 各自的 fill/stroke。
    // 已知边界（都会告警）：patternTransform 的旋转/斜切按轴对齐近似；内容里的 <g> 不
    //   展开；内容渐变按中间色；平铺块数或点数超预算时退回"首色实填"。
    struct SvgPatternChild {
        PathShape shape;
        bool hasFill = true;    uint32_t fill = 0xFF000000u;
        bool hasStroke = false; uint32_t stroke = 0xFF000000u; float strokeW = 0.0f;
        uint8_t cap = BINUI_CAP_BUTT, join = BINUI_JOIN_ROUND;
    };
    struct SvgPatternDef {
        bool  found = false;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        int   units = 0;         // 0 = userSpaceOnUse（默认）  1 = objectBoundingBox
        int   contentUnits = 0;  // 同上
        bool  hasViewBox = false;
        float vbX = 0.0f, vbY = 0.0f, vbW = 0.0f, vbH = 0.0f;
        bool  hasTf = false;
        PathMatrix tf;
        std::vector<SvgPatternChild> kids;
    };
    std::unordered_map<std::string, SvgPatternDef> svgPatterns;
    for (size_t qi = 0; qi < elems.size(); qi++) {
        if (lowerStr(elems[qi].tag) != "pattern") continue;
        const HNode& pe = elems[qi];
        std::string pid = pe.attrs.count("id") ? TrimCopy(pe.attrs.at("id")) : "";
        if (pid.empty()) {
            Warn("<pattern> 没有 id，无法被 fill=\"url(#…)\" 引用（已忽略）");
            continue;
        }
        SvgPatternDef pd;
        pd.found = true;
        auto pnum = [&](const char* a, float dflt) -> float {
            auto it = pe.attrs.find(a);
            return it == pe.attrs.end() ? dflt : parseLen(it->second, dflt);
        };
        pd.x = pnum("x", 0.0f);
        pd.y = pnum("y", 0.0f);
        pd.w = pnum("width", 0.0f);
        pd.h = pnum("height", 0.0f);
        {
            auto it = pe.attrs.find("patternunits");
            if (it != pe.attrs.end())
                pd.units = lowerStr(TrimCopy(it->second)) == "objectboundingbox" ? 1 : 0;
        }
        {
            auto it = pe.attrs.find("patterncontentunits");
            if (it != pe.attrs.end())
                pd.contentUnits = lowerStr(TrimCopy(it->second)) == "objectboundingbox" ? 1 : 0;
        }
        if (pe.attrs.count("viewbox")) {
            std::string vb = TrimCopy(pe.attrs.at("viewbox")) + " ";
            float v[4] = { 0, 0, -1, -1 };
            int got = 0;
            std::string cur;
            for (char ch : vb) {
                if (ch == ' ' || ch == ',' || ch == '\t') {
                    if (!cur.empty() && got < 4) v[got++] = parseLen(cur, 0.0f);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
            if (!cur.empty() && got < 4) v[got++] = parseLen(cur, 0.0f);
            if (got >= 4 && v[2] > 0.0f && v[3] > 0.0f) {
                pd.hasViewBox = true;
                pd.vbX = v[0]; pd.vbY = v[1]; pd.vbW = v[2]; pd.vbH = v[3];
            } else {
                Warn("<pattern id=\"" + pid + "\"> 的 viewBox 无效（已忽略）");
            }
        }
        {
            auto it = pe.attrs.find("patterntransform");
            if (it != pe.attrs.end() && !it->second.empty()) {
                std::string terr;
                if (ParseSvgTransform(it->second, pd.tf, &terr)) pd.hasTf = true;
                else Warn("<pattern id=\"" + pid + "\"> 的 patternTransform 解析失败：" + terr);
            }
        }
        auto patCol = [&](const std::string& v, uint32_t dflt) -> uint32_t {
            std::string gid2;
            if (ParseUrlId(v, gid2)) {
                auto gi2 = svgGrads.find(gid2);
                if (gi2 != svgGrads.end() && gi2->second.found && !gi2->second.stops.empty()) {
                    Warn("<pattern id=\"" + pid + "\"> 的内容不支持渐变（按中间色）");
                    return gi2->second.stops[gi2->second.stops.size() / 2].color;
                }
                Warn("<pattern id=\"" + pid + "\"> 里的 url(#" + gid2 + ") 找不到定义（按黑色）");
                return dflt;
            }
            return parseColorWith(v, dflt, "SVG 颜色");
        };
        for (size_t qj = 0; qj < elems.size(); qj++) {
            if (elems[qj].parentIdx != qi) continue;
            const HNode& ce = elems[qj];
            const std::string ct = lowerStr(ce.tag);
            if (ct == "g") {
                Warn("<pattern id=\"" + pid + "\"> 里的 <g> 不展开（把形状直接写在 <pattern> 下）");
                continue;
            }
            SvgPatternChild kid;
            if (!shapeOf(ce, kid.shape, 0.1f)) continue;
            auto ft = ce.attrs.find("fill");
            if (ft != ce.attrs.end()) {
                const std::string fv = TrimCopy(ft->second);
                const std::string fl = lowerStr(fv);
                if (fl == "none") kid.hasFill = false;
                else if (fl != "currentcolor") kid.fill = patCol(fv, 0xFF000000u);
            }
            auto st = ce.attrs.find("stroke");
            if (st != ce.attrs.end()) {
                const std::string sv2 = TrimCopy(st->second);
                const std::string sl = lowerStr(sv2);
                if (sl != "none" && !sv2.empty()) {
                    kid.hasStroke = true;
                    kid.stroke = patCol(sv2, 0xFF000000u);
                    kid.strokeW = ce.attrs.count("stroke-width")
                                      ? parseLen(ce.attrs.at("stroke-width"), 0.0f) : 1.0f;
                    const std::string lc = ce.attrs.count("stroke-linecap")
                                               ? lowerStr(TrimCopy(ce.attrs.at("stroke-linecap"))) : "";
                    if (lc == "square") kid.cap = BINUI_CAP_SQUARE;
                    else if (lc == "round") kid.cap = BINUI_CAP_ROUND;
                    const std::string lj = ce.attrs.count("stroke-linejoin")
                                               ? lowerStr(TrimCopy(ce.attrs.at("stroke-linejoin"))) : "";
                    if (lj == "miter") kid.join = BINUI_JOIN_MITER;
                    else if (lj == "bevel") kid.join = BINUI_JOIN_BEVEL;
                }
            }
            // <line>/<polyline> 这类"只有描边才看得见"的形状：没写 fill 时别给它黑填充
            if ((ct == "line" || ct == "polyline") && ft == ce.attrs.end()) kid.hasFill = false;
            if (!kid.hasFill && !kid.hasStroke) continue;
            pd.kids.push_back(std::move(kid));
        }
        if (pd.kids.empty()) {
            Warn("<pattern id=\"" + pid + "\"> 里没有可绘制的形状（引用它的元素不填充）");
            continue;
        }
        svgPatterns[pid] = std::move(pd);
    }

    // ============ v0.16.26：SVG <mask>（编译期折算 + 运行期两趟混合） ============
    // 掩码是"逐像素乘一次 alpha"。做法与裁剪同一套离屏图层，只是多一趟：
    //   ① 保留趟：把掩码几何按 alpha 用 dst *= srcA 画一遍（白保留、灰压暗）；
    //   ② 补集趟：图层矩形 ∪ 全部掩码几何（even-odd）用 dst *= 1-srcA 画一遍，把掩码
    //      没盖到的像素清零（SVG 语义：mask 内容之外 = 黑 = 透明）。
    // 亮度在编译期折进 alpha（sRGB 系数 0.2126/0.7152/0.0722 × fill-opacity × opacity），
    // 所以运行期只需要"几何 + 一个 32 位颜色"，三端零着色器改动。
    // 已知边界（都会告警）：内容渐变按中间色（不产生渐变过渡）；内容里的 <g> 不展开；
    //   多个形状**重叠**时保留趟会乘两次（重叠区更暗），补集趟的 even-odd 也会把重叠区
    //   当成"外"—— 掩码内容请写不重叠的形状。
    struct SvgMaskChild {
        PathShape shape;
        uint32_t  color = 0xFFFFFFFFu;   // 已折好亮度（无渐变时用）
        // v0.16.53：内容可以用**渐变**填充。这里存渐变定义，等几何落到设备空间、
        // 拿到"本组几何包围盒"之后再折算成 BinUIPathGradient（亮度折进色标 alpha）。
        bool      hasGrad = false;
        SvgGradDef grad;
        float     gbMinX = 0, gbMinY = 0, gbW = 1, gbH = 1;   // 渐变 OBB 基准（kid 自身盒）
        float     opa = 1.0f;            // fill-opacity × opacity（渐变时要折进色标 alpha）
    };
    struct SvgMaskDef {
        bool  found = false;
        int   units = 1;          // maskUnits：0 = userSpaceOnUse  1 = objectBoundingBox（默认）
        int   contentUnits = 0;   // maskContentUnits：0 = userSpaceOnUse（默认） 1 = objectBoundingBox
        // v0.16.54：mask-type —— 0 = luminance（SVG/默认：用**亮度**当 alpha），
        // 1 = alpha（CSS Masking：直接用内容自己的 alpha，颜色亮度不参与）
        int   alphaType = 0;
        bool  hasX = false, hasY = false, hasW = false, hasH = false;
        float x = -0.1f, y = -0.1f, w = 1.2f, h = 1.2f;
        bool  xPct = false, yPct = false, wPct = false, hPct = false;
        std::vector<SvgMaskChild> kids;
    };
    std::unordered_map<std::string, SvgMaskDef> svgMasks;
    for (size_t mi2 = 0; mi2 < elems.size(); mi2++) {
        if (lowerStr(elems[mi2].tag) != "mask") continue;
        const HNode& me2 = elems[mi2];
        std::string mid2 = me2.attrs.count("id") ? TrimCopy(me2.attrs.at("id")) : "";
        if (mid2.empty()) {
            Warn("<mask> 没有 id，无法被 mask=\"url(#…)\" 引用（已忽略）");
            continue;
        }
        SvgMaskDef md2;
        md2.found = true;
        {
            auto it = me2.attrs.find("maskunits");
            if (it != me2.attrs.end())
                md2.units = lowerStr(TrimCopy(it->second)) == "userspaceonuse" ? 0 : 1;
        }
        {
            auto it = me2.attrs.find("maskcontentunits");
            if (it != me2.attrs.end())
                md2.contentUnits = lowerStr(TrimCopy(it->second)) == "objectboundingbox" ? 1 : 0;
        }
        {
            // v0.16.54：mask-type="luminance | alpha"（CSS Masking）。
            // 默认 luminance（SVG 1.1 的语义）；alpha 时内容颜色**只贡献 alpha**，
            // 于是"白色半透明圆"= 半透明掩码，而不是"白色 = 全保留、灰色 = 半保留"。
            auto it = me2.attrs.find("mask-type");
            if (it == me2.attrs.end()) it = me2.attrs.find("masktype");
            if (it != me2.attrs.end()) {
                const std::string mv2 = lowerStr(TrimCopy(it->second));
                if (mv2 == "alpha") md2.alphaType = 1;
                else if (mv2 == "luminance" || mv2.empty()) md2.alphaType = 0;
                else
                    Warn("<mask id=\"" + mid2 + "\"> 的 mask-type 只认 luminance / alpha：\"" +
                         mv2 + "\"（按 luminance 处理）");
            }
        }
        // 区域：缺省 -10% .. 120%（SVG 规范默认值）；百分比按元素盒的比例算
        auto region = [&](const char* key, float& out, bool& has, bool& pct) {
            auto it = me2.attrs.find(key);
            if (it == me2.attrs.end()) return;
            std::string v = TrimCopy(it->second);
            if (!v.empty() && v.back() == '%') {
                pct = true;
                out = parseLen(v.substr(0, v.size() - 1), 0.0f) / 100.0f;
            } else {
                out = parseLen(v, 0.0f);
            }
            has = true;
        };
        region("x", md2.x, md2.hasX, md2.xPct);
        region("y", md2.y, md2.hasY, md2.yPct);
        region("width", md2.w, md2.hasW, md2.wPct);
        region("height", md2.h, md2.hasH, md2.hPct);
        if (md2.units == 0 && (md2.xPct || md2.yPct || md2.wPct || md2.hPct))
            Warn("<mask id=\"" + mid2 + "\" maskUnits=\"userSpaceOnUse\"> 的区域写了百分比："
                 "按元素包围盒的比例折算（SVG 规范是相对视口）");
        for (size_t mj = 0; mj < elems.size(); mj++) {
            if (elems[mj].parentIdx != mi2) continue;
            const HNode& ce2 = elems[mj];
            const std::string ct2 = lowerStr(ce2.tag);
            if (ct2 == "g") {
                Warn("<mask id=\"" + mid2 + "\"> 里的 <g> 不展开（把形状直接写在 <mask> 下）");
                continue;
            }
            SvgMaskChild kid2;
            // objectBoundingBox 的内容坐标是 0..1：容差要跟着缩，否则圆会被切成多边形
            //（与 clipPath 的 objectBoundingBox 同一个坑，见 14.2）
            const float mtol = (md2.contentUnits == 1) ? 0.002f : 0.1f;
            if (!shapeOf(ce2, kid2.shape, mtol)) continue;
            // 亮度：SVG 规范默认 luminance 掩码（白 = 可见）
            uint32_t fc = 0xFF000000u;
            float opa = 1.0f;
            auto ft2 = ce2.attrs.find("fill");
            if (ft2 != ce2.attrs.end()) {
                const std::string fv2 = TrimCopy(ft2->second);
                const std::string fl2 = lowerStr(fv2);
                if (fl2 == "none") continue;                 // 不填 = 不参与遮罩
                std::string gid3;
                if (ParseUrlId(fv2, gid3)) {
                    auto gi3 = svgGrads.find(gid3);
                    if (gi3 != svgGrads.end() && gi3->second.found && !gi3->second.stops.empty()) {
                        // v0.16.53：渐变内容**真做**（不再按中间色）。几何照旧，颜色交给
                        // 渐变色标（亮度已折进 alpha），运行期保留趟用 fillGrad 画条带。
                        kid2.hasGrad = true;
                        kid2.grad = gi3->second;
                        kid2.shape.RecomputeBounds();
                        kid2.gbMinX = kid2.shape.minX;
                        kid2.gbMinY = kid2.shape.minY;
                        kid2.gbW = std::max(1e-6f, kid2.shape.Width());
                        kid2.gbH = std::max(1e-6f, kid2.shape.Height());
                    } else {
                        Warn("<mask id=\"" + mid2 + "\"> 里的 url(#" + gid3 + ") 找不到定义"
                             "（按白色）");
                        fc = 0xFF000000u | 0x00FFFFFFu;
                    }
                } else if (fl2 == "currentcolor") {
                    fc = 0xFFFFFFFFu;
                } else {
                    fc = parseColorWith(fv2, 0xFFFFFFFFu, "SVG 颜色");
                }
            } else {
                fc = 0xFFFFFFFFu;    // 缺省白色（= 全保留）
            }
            auto opOf = [&](const char* key) -> float {
                auto it = ce2.attrs.find(key);
                if (it == ce2.attrs.end()) return 1.0f;
                float v = parseLen(it->second, 1.0f);
                return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            };
            opa = opOf("fill-opacity") * opOf("opacity");
            const float r2 = (float)((fc >> 16) & 0xFFu);
            const float g2 = (float)((fc >> 8) & 0xFFu);
            const float b2 = (float)(fc & 0xFFu);
            const float a2 = (float)((fc >> 24) & 0xFFu) / 255.0f;
            float lum = (0.2126f * r2 + 0.7152f * g2 + 0.0722f * b2) / 255.0f;
            // v0.16.54：mask-type="alpha" 时颜色亮度不参与，只看 alpha（fill-opacity /
            // opacity 照旧要乘）—— 两者在"白色"上等价，在灰/彩色上差很远。
            if (md2.alphaType == 1) lum = 1.0f;
            if (kid2.hasGrad) {
                // 渐变：逐点亮度在后面折算，这里只需保证"整体不是全透明"
                kid2.opa = opa;
                md2.kids.push_back(std::move(kid2));
                continue;
            }
            float alpha = lum * opa * a2;
            if (alpha <= 0.002f) continue;                   // 纯黑 = 完全透明，不用画
            const uint32_t av = (uint32_t)(alpha * 255.0f + 0.5f);
            kid2.color = (av << 24) | 0x00FFFFFFu;
            md2.kids.push_back(std::move(kid2));
        }
        if (md2.kids.empty()) {
            Warn("<mask id=\"" + mid2 + "\"> 里没有可见形状（引用它的元素会被完全遮住；"
                 "已按\"不遮罩\"处理，见文档）");
            continue;
        }
        svgMasks[mid2] = std::move(md2);
    }

    // ============ v0.16.47：SVG <filter>（编译期归约，运行期/格式零改动）============
    // 运行期能做的"离屏原子效果"只有两种（见后端 popLayer）：整组模糊（= CSS blur()）
    // 和身后的彩色影子（= CSS drop-shadow()）。所以 <filter> 的原语链在**编译期**就归约
    // 成这两样的组合：
    //   · feGaussianBlur(SourceGraphic)                        → 整组模糊
    //   · feDropShadow                                         → 一条影子
    //   · feGaussianBlur(SourceAlpha) → feOffset → feFlood
    //     → feComposite(operator="in") → feMerge[影子, 源]       → 一条影子（带模糊）
    //   · feOffset → feFlood → feComposite(in) → feMerge        → 一条硬边影子（σ=0）
    // 归约不了的原语（feColorMatrix / feBlend / feTurbulence / 光照…）逐条告警，
    // 绝不静默近似 —— 这条仓库的规矩：写了没效果必须说出来。
    struct SvgFilterShadow {
        float dx = 0.0f, dy = 0.0f, blur = 0.0f;
        uint32_t color = 0xFF000000u;
    };
    struct SvgFilterDef {
        bool found = false;
        float blur = 0.0f;                       // 对元素整组施加的模糊（σ）
        std::vector<SvgFilterShadow> shadows;    // SVG 叠放顺序：[0] 在最下面
        std::string ignored;                     // 认不出的原语（去重，逗号分隔）
    };
    std::unordered_map<std::string, SvgFilterDef> svgFilters;
    {
        // 归约过程中传递的"值"：只关心这几件事 —— 是源图形还是源 alpha、积累的偏移/模糊、
        // 有没有被纯色上色（上了色就是"影子"，否则还是本体的一个视图）。
        struct FxVal {
            int kind = 0;        // 0 = 源图形  1 = 带色影子  2 = 纯色(flood)  3 = 源 alpha
            float dx = 0.0f, dy = 0.0f, blur = 0.0f;
            uint32_t color = 0;
            bool valid = false;
        };
        for (size_t fi = 0; fi < elems.size(); fi++) {
            if (lowerStr(elems[fi].tag) != "filter") continue;
            const HNode& fel = elems[fi];
            const std::string fid = fel.attrs.count("id") ? TrimCopy(fel.attrs.at("id")) : "";
            if (fid.empty()) {
                Warn("<filter> 没有 id，无法被 filter=\"url(#…)\" 引用（已忽略）");
                continue;
            }
            SvgFilterDef def;
            def.found = true;
            // 区域参数（x/y/width/height）我们不按它裁：运行期的图层框是"元素框外扩 3σ"。
            // 只在**比 SVG 默认区域（-10%..110%）更窄**时告警 —— 那时作者以为自己限制了
            // 绘制范围，实际我们会画到框外（多半是好事，但必须说出来）。四个值都是百分比
            // 才比较得动；有些值写了 px 就没法判断，按"不告警"处理（别给正常素材刷屏）。
            {
                bool allPct = true;
                float rx = -0.10f, ry = -0.10f, rw = 1.20f, rh = 1.20f;
                const char* keys[4] = { "x", "y", "width", "height" };
                float* outs[4] = { &rx, &ry, &rw, &rh };
                for (int ki = 0; ki < 4; ki++) {
                    auto it = fel.attrs.find(keys[ki]);
                    if (it == fel.attrs.end()) continue;
                    const std::string v = TrimCopy(it->second);
                    if (v.empty() || v.back() != '%') { allPct = false; continue; }
                    *outs[ki] = parseLen(v.substr(0, v.size() - 1), 0.0f) / 100.0f;
                }
                if (allPct && (rx > -0.0999f || ry > -0.0999f ||
                               rx + rw < 1.0999f || ry + rh < 1.0999f)) {
                    Warn("<filter id=\"" + fid + "\"> 的滤镜区域（x/y/width/height）比默认的 "
                         "-10%..110% 窄：未按它裁剪，按元素框外扩 3σ 绘制");
                }
            }

            std::unordered_map<std::string, FxVal> vals;   // result 名 → 值
            FxVal prevOut;
            bool hasPrev = false;
            auto getIn = [&](const std::string& inName) -> FxVal {
                const std::string n = lowerStr(TrimCopy(inName));
                if (n.empty()) {
                    if (hasPrev) return prevOut;
                    FxVal s;
                    s.kind = 0;
                    s.valid = true;
                    return s;
                }
                if (n == "sourcegraphic") { FxVal s; s.kind = 0; s.valid = true; return s; }
                if (n == "sourcealpha") { FxVal s; s.kind = 3; s.valid = true; return s; }
                auto it = vals.find(n);
                return it == vals.end() ? FxVal{} : it->second;
            };
            // flood-color / flood-opacity（SVG 缺省：黑色、全不透明）
            auto floodColOf = [&](const HNode& h) -> uint32_t {
                uint32_t c = 0xFF000000u;
                auto it = h.attrs.find("flood-color");
                if (it != h.attrs.end()) {
                    const std::string v = TrimCopy(it->second);
                    // 定义里的 currentColor 没有宿主可以继承 → 按黑（SVG 缺省色）处理
                    if (lowerStr(v) != "currentcolor")
                        c = parseColorWith(v, 0xFF000000u, "SVG filter flood-color");
                }
                float op = 1.0f;
                auto io = h.attrs.find("flood-opacity");
                if (io != h.attrs.end()) {
                    op = parseLen(TrimCopy(io->second), 1.0f);
                    if (op < 0.0f) op = 0.0f;
                    if (op > 1.0f) op = 1.0f;
                }
                const float a = (float)((c >> 24) & 0xFFu) * op;
                return ((uint32_t)(a + 0.5f) << 24) | (c & 0x00FFFFFFu);
            };
            // stdDeviation="σx [σy]"（逗号或空白分隔；只写一个 = 各向同性）
            auto stdDevOf = [&](const HNode& h, float& sx, float& sy) -> bool {
                sx = sy = 0.0f;
                auto it = h.attrs.find("stddeviation");
                if (it == h.attrs.end()) return false;
                std::vector<std::string> t;
                {
                    std::string cur;
                    for (char ch : TrimCopy(it->second)) {
                        if (ch == ' ' || ch == '\t' || ch == ',') {
                            if (!cur.empty()) { t.push_back(cur); cur.clear(); }
                            continue;
                        }
                        cur += ch;
                    }
                    if (!cur.empty()) t.push_back(cur);
                }
                if (t.empty()) return false;
                sx = parseLen(t[0], 0.0f);
                sy = t.size() > 1 ? parseLen(t[1], 0.0f) : sx;
                if (sx < 0.0f) sx = 0.0f;
                if (sy < 0.0f) sy = 0.0f;
                return true;
            };
            auto clampBlur = [&](float& sig, const std::string& who) {
                if (sig > kMaxFilterBlurPx) {
                    sig = kMaxFilterBlurPx;
                    Warn("SVG <filter id=\"" + fid + "\"> 的 " + who + " 上限 64px，按 64px 处理");
                }
            };

            for (size_t fj = 0; fj < elems.size(); fj++) {
                if (elems[fj].parentIdx != fi) continue;
                const HNode& pe = elems[fj];
                const std::string pt = lowerStr(pe.tag);
                if (pt == "femergenode") continue;   // 由父 feMerge 统一处理
                auto attrOf = [&](const char* k) -> std::string {
                    auto it = pe.attrs.find(k);
                    return it == pe.attrs.end() ? std::string() : TrimCopy(it->second);
                };
                const std::string in1 = attrOf("in");
                const std::string resName = lowerStr(attrOf("result"));
                FxVal out;
                bool handled = true;

                if (pt == "fegaussianblur") {
                    float sx = 0.0f, sy = 0.0f;
                    if (!stdDevOf(pe, sx, sy) || sx <= 0.0f) {
                        // 没有 stdDeviation = 不变的恒等原语，直接透传
                        out = getIn(in1);
                        if (!out.valid) handled = false;
                    } else {
                        FxVal v = getIn(in1);
                        if (!v.valid) {
                            handled = false;
                        } else if (v.kind == 2) {
                            handled = false;   // 给纯色再模糊没有意义（也没法画）
                        } else if (v.blur > 0.0f) {
                            // 连续两次模糊：运行期只有一趟。σ 合并成 sqrt(σ1²+σ2²) 是数学上
                            // 正确的做法，但我们的降采样链只按单一 σ 走 → 明确告警。
                            Warn("SVG <filter id=\"" + fid + "\"> 里连续两层 feGaussianBlur "
                                 "无法合成（运行期只做一趟模糊），按第一层的 σ 处理");
                            out = v;
                        } else {
                            if (std::fabs(sx - sy) > 0.01f) {
                                Warn("SVG <filter id=\"" + fid + "\"> 的 stdDeviation 各向异性"
                                     "（σx=" + std::to_string(sx) + ", σy=" + std::to_string(sy) +
                                     "）不支持，按 σx 处理");
                            }
                            v.blur = sx;
                            clampBlur(v.blur, "feGaussianBlur stdDeviation");
                            out = v;
                        }
                    }
                } else if (pt == "feoffset") {
                    FxVal v = getIn(in1);
                    if (!v.valid || v.kind == 2) {
                        handled = false;
                    } else {
                        v.dx += parseLen(attrOf("dx"), 0.0f);
                        v.dy += parseLen(attrOf("dy"), 0.0f);
                        out = v;
                    }
                } else if (pt == "feflood") {
                    FxVal v;
                    v.kind = 2;
                    v.color = floodColOf(pe);
                    v.valid = true;
                    out = v;
                } else if (pt == "fecomposite") {
                    const std::string op = lowerStr(attrOf("operator"));
                    FxVal a = getIn(in1);
                    FxVal b = getIn(attrOf("in2"));
                    // 只支持 operator="in"：一个输入是纯色、另一个是"形状的 alpha 视图"
                    // → 得到"上色的轮廓"，也就是影子。其它算子（out/atop/xor/arithmetic）
                    // 需要真正的双输入像素运算，运行期没有。
                    if (op.empty() || op == "in") {
                        if (a.valid && a.kind != 2 && b.valid && b.kind == 2) {
                            out = a;
                            out.kind = 1;
                            out.color = b.color;
                        } else if (a.valid && a.kind == 2 && b.valid && b.kind != 2) {
                            out = b;
                            out.kind = 1;
                            out.color = a.color;
                        } else {
                            handled = false;
                        }
                    } else {
                        handled = false;
                    }
                } else if (pt == "fedropshadow") {
                    // SVG2：feDropShadow = 偏移+模糊+上色+叠回本体。我们的管线本来就
                    // "本体最后画在最上面"，所以直接记一条影子即可。
                    float sx = 0.0f, sy = 0.0f;
                    stdDevOf(pe, sx, sy);
                    SvgFilterShadow sh;
                    sh.dx = parseLen(attrOf("dx"), 0.0f);
                    sh.dy = parseLen(attrOf("dy"), 0.0f);
                    sh.blur = sx;
                    clampBlur(sh.blur, "feDropShadow stdDeviation");
                    sh.color = floodColOf(pe);
                    def.shadows.push_back(sh);
                    FxVal v;
                    v.kind = 0;
                    v.valid = true;
                    out = v;
                } else if (pt == "femerge") {
                    std::vector<FxVal> layers;
                    for (size_t fk = 0; fk < elems.size(); fk++) {
                        if (elems[fk].parentIdx != fj) continue;
                        if (lowerStr(elems[fk].tag) != "femergenode") continue;
                        std::string inName;
                        auto ii = elems[fk].attrs.find("in");
                        if (ii != elems[fk].attrs.end()) inName = ii->second;
                        layers.push_back(getIn(inName));
                    }
                    if (layers.empty()) {
                        handled = false;
                    } else {
                        int nSrc = 0, nShadow = 0, lastSrc = -1;
                        for (size_t lk = 0; lk < layers.size() && handled; lk++) {
                            const FxVal& lv = layers[lk];
                            if (!lv.valid || lv.kind == 2) { handled = false; break; }
                            if (lv.kind == 0) {
                                nSrc++;
                                lastSrc = (int)lk;
                                if (lv.blur > def.blur) def.blur = lv.blur;
                            } else if (lv.kind == 1) {
                                nShadow++;
                            } else {
                                handled = false;   // 源 alpha 直接进 merge 画不出来
                            }
                        }
                        if (handled) {
                            if (nSrc == 0)
                                Warn("SVG <filter id=\"" + fid + "\"> 的 feMerge 里没有源图形层："
                                     "运行期总是把元素本体画在影子上方，已按此处理");
                            else if (nSrc > 1)
                                Warn("SVG <filter id=\"" + fid + "\"> 的 feMerge 里有多层源图形"
                                     "（含模糊版本），运行期只画一次，按第一层处理");
                            if (nShadow > 0 && lastSrc >= 0) {
                                for (size_t lk = (size_t)lastSrc + 1; lk < layers.size(); lk++)
                                    if (layers[lk].kind == 1) {
                                        Warn("SVG <filter id=\"" + fid + "\"> 有影子写在源图形"
                                             "之后（= 盖在元素上方）：运行期只把影子放在元素下方，"
                                             "已按下方处理");
                                        break;
                                    }
                            }
                            for (size_t lk = 0; lk < layers.size(); lk++) {
                                const FxVal& lv = layers[lk];
                                if (lv.kind != 1) continue;
                                SvgFilterShadow sh;
                                sh.dx = lv.dx;
                                sh.dy = lv.dy;
                                sh.blur = lv.blur;
                                clampBlur(sh.blur, "影子模糊");
                                sh.color = lv.color;
                                def.shadows.push_back(sh);
                            }
                            out.kind = 0;
                            out.valid = true;
                        }
                    }
                } else {
                    handled = false;
                }

                if (!handled) {
                    if (def.ignored.empty()) def.ignored = pt;
                    else if (def.ignored.find(pt) == std::string::npos) def.ignored += "," + pt;
                    continue;
                }
                if (!resName.empty()) vals[resName] = out;
                prevOut = out;
                hasPrev = true;
            }

            // 链尾仍是"被平移/被模糊的源图形" → 分别是"整体平移"（不支持）与整组模糊（支持）
            if (hasPrev && prevOut.valid) {
                if (prevOut.kind == 0 && prevOut.blur > 0.0f && def.blur <= 0.0f)
                    def.blur = prevOut.blur;
                if (prevOut.kind == 0 && (std::fabs(prevOut.dx) > 0.01f ||
                                          std::fabs(prevOut.dy) > 0.01f)) {
                    Warn("SVG <filter id=\"" + fid + "\"> 的 feOffset 直接作用在源图形上"
                         "（= 整体平移元素）不支持：请改用元素自己的 transform/x/y");
                }
            }
            if (!def.ignored.empty()) {
                Warn("SVG <filter id=\"" + fid + "\"> 的原语 " + def.ignored +
                     " 不支持（本运行期只有 feGaussianBlur / feOffset / feFlood / "
                     "feComposite(operator=in) / feMerge / feDropShadow），已忽略");
            }
            if (def.blur <= 0.0f && def.shadows.empty() && def.ignored.empty()) {
                Warn("SVG <filter id=\"" + fid + "\"> 没有可用的原语，引用它的元素看起来和没写一样");
            }
            svgFilters[fid] = std::move(def);
        }
    }

    {
        static const std::unordered_set<std::string> kKnownTags = {
            // 结构/包裹
            "app_window", "binui", "root", "html", "body", "head", "title", "meta", "link",
            "script", "style", "div", "span", "section", "header", "footer", "nav", "aside",
            "main", "article", "form", "fieldset", "template", "noscript",
            // 文本
            "h1", "h2", "h3", "h4", "h5", "h6", "p", "label", "a", "b", "i", "em", "strong",
            "small", "u", "s", "strike", "del", "ins", "mark", "code", "kbd", "samp", "var",
            "abbr", "cite", "q", "dfn", "time", "sub", "sup", "pre", "br", "hr", "blockquote",
            "address", "dl", "dt", "dd", "figure", "figcaption", "ul", "ol", "li", "legend",
            "caption", "td", "th", "colgroup", "col",
            // 控件
            "input", "button", "video", "img", "source", "track", "titlebar", "use", "component",
            // 向量/画布
            "svg", "canvas", "line", "circle", "rect", "polyline", "polygon",
            "path", "ellipse", "g", "defs", "symbol", "text", "image",
            "lineargradient", "radialgradient", "stop",
            "marker", "clippath", "pattern", "mask",
            // v0.16.47：SVG <filter> 与它的原语（原语由滤镜注册表消费；不支持的原语
            // 在注册表那一趟逐条告警，这里只负责"别报未知标签"）
            "filter", "fegaussianblur", "feoffset", "feflood", "fecomposite", "femerge",
            "femergenode", "fedropshadow", "fecolormatrix", "feblend", "feboxblur",
            "femorphology", "feturbulence", "feimage", "fetile", "fecomponenttransfer",
            "fedisplacementmap", "fediffuselighting", "fespecularlighting", "fedistantlight",
            "fepointlight", "fespotlight",
        };
        // 认得但只能糊弄过去的标签：明确说清楚会变成什么，别让人以为生效了
        static const std::unordered_map<std::string, const char*> kShimTags = {
            { "select", "<select>/<option> 没有下拉控件，option 会按块级元素竖排铺开" },
            { "option", "<select>/<option> 没有下拉控件，option 会按块级元素竖排铺开" },
            { "optgroup", "<select>/<option> 没有下拉控件，option 会按块级元素竖排铺开" },
            { "datalist", "<datalist> 不支持，内容按普通容器渲染" },
            { "textarea", "<textarea> 不可编辑（渲染器只有单行文本编辑），按普通容器渲染" },
            { "table", "<table> 没有表格算法，行列会按块级元素堆叠" },
            { "thead", "<table> 没有表格算法，行列会按块级元素堆叠" },
            { "tbody", "<table> 没有表格算法，行列会按块级元素堆叠" },
            { "tfoot", "<table> 没有表格算法，行列会按块级元素堆叠" },
            { "tr", "<table> 没有表格算法，行列会按块级元素堆叠" },
            { "details", "<details> 不支持折叠，内容常显" },
            { "summary", "<details> 不支持折叠，内容常显" },
            { "progress", "<progress> 没有内置外观，建议用图表 API 的 ProgressBar" },
            { "meter", "<meter> 没有内置外观，建议用图表 API 的 ProgressBar" },
            { "dialog", "<dialog> 不支持模态，按普通容器渲染" },
            { "output", "<output> 不支持，按普通容器渲染" },
            { "include", "<include> 不生效：组件内联用 <use src=\"x.bum.html\">（成对写法也可以，"
                         "宿主子元素会落进组件根容器）" },
            { "import", "<import> 不生效：组件内联用 <use src=\"x.bum.html\">" },
            // ---- SVG：path/ellipse/line/circle/rect/polyline/polygon 已支持，其余别猜 ----
            // 这些标签被"当普通容器"时画面是**空**的（不是画歪了），所以必须说清楚换成什么，
            // 否则作者只会看到自己那一段什么都不显示，以为是自己写错了。
            // v15 起 path/ellipse 已实现，从这张表里移出（kKnownTags 里有了）。
            
            { "tspan", "SVG <tspan> 不支持富文本：把文字写进 <text> 本体，或拆成多个 <text>" },
        };
        // 真正被编译器消费的属性（其余一律视为"写了没效果"）
        const auto& kUsedAttrs = UsedHtmlAttrs();

        bool inlineNestedWarned = false;   // 行内嵌套警告全文档只报一次
        for (size_t idx = 0; idx < elems.size(); idx++) {
            const auto& e = elems[idx];
            if (e.tag.empty() || e.tag == "head" || e.tag == "meta" || e.tag == "link" ||
                e.tag == "script" || e.tag == "style")
                continue;
            // 行内元素套行内元素：binUI 没有 inline formatting context，子元素会独占一行。
            // 不说清楚的话，作者会以为自己写错了（<p>正文 <b>强调</b></p> 一定会换行）。
            if (!inlineNestedWarned && kInlineTextTags.count(e.tag) && e.parentIdx != SIZE_MAX &&
                e.parentIdx < elems.size() && kInlineTextTags.count(elems[e.parentIdx].tag)) {
                inlineNestedWarned = true;
                Warn("HTML 行内元素（<" + e.tag + "> 套在 <" + elems[e.parentIdx].tag +
                     "> 里）会各自独占一行：binUI 没有行内格式化上下文，"
                     "同一行混排多种样式需要富文本段（暂未实现）");
            }
            if (!kKnownTags.count(e.tag)) {
                auto shim = kShimTags.find(e.tag);
                if (shim != kShimTags.end()) Warn(std::string("HTML 标签 <") + e.tag + ">：" + shim->second);
                else Warn("未知 HTML 标签 <" + e.tag + ">，已按普通容器处理");
            } else if (kShimTags.count(e.tag)) {
                Warn(std::string("HTML 标签 <") + e.tag + ">：" + kShimTags.at(e.tag));
            }
            for (const auto& kv : e.attrs) {
                const std::string& k = kv.first;
                if (k.empty() || k[0] == '_') continue;                    // 编译器内部标记
                if (k.rfind("data-", 0) == 0 || k.rfind("aria-", 0) == 0) continue;
                // SVG 里 transform / viewBox 现在真的生效（v15）；SVG 之外仍然是"写了没效果"，
                // 这两条只在非 SVG 子树里报，避免给正常的 SVG 素材刷告警。
                if (k == "transform" && !InSvgSubtree(idx)) {
                    Warn("transform 属性不生效（HTML 元素不支持几何变换；只有 <svg> 子树里的 "
                         "translate/scale/rotate/matrix 会编译进坐标）：请直接改坐标");
                    continue;
                }
                if (k == "viewbox" && e.tag != "svg" && e.tag != "symbol" &&
                    e.tag != "marker" && e.tag != "pattern") {
                    Warn("viewBox 只对 <svg>/<symbol> 有效（当前写在 <" + e.tag + "> 上，已忽略）");
                    continue;
                }
                if (kUsedAttrs.count(k)) continue;
                Warn("HTML 属性 \"" + k + "\"（<" + e.tag + ">）未实现，已忽略");
            }
        }
    }

    // ---- 3.5 display:none：把整棵子树标出来 ----
    // 只跳过元素本身是不够的：子元素的父级解析（resolveParentId）会继续往上找最近的
    // 已生成祖先，于是隐藏容器里的绝对定位子元素被重挂到根/上层容器，按自己的原始坐标
    // 画到别处（而且零警告，issue 03）。这里先按"级联后的 style"（class 规则也算）
    // 算出每个元素自身是否 display:none，再沿 parentIdx 链向上判断是否落在隐藏子树里。
    // 不用单趟前向传播：::before 合成节点的 parentIdx 可能大于自身下标（宿主紧随其后入表），
    // 沿父链向上查对元素顺序没有要求。结构伪类（:nth-child）仍把 display:none 的元素
    // 计入兄弟序号（见文件上方注释），本预处理只管"生成节点"，不动选择器口径。
    std::vector<char> hiddenSelf(elems.size(), 0);
    std::vector<char> hidden(elems.size(), 0);
    for (size_t hi = 0; hi < elems.size(); hi++) {
        const auto& he = elems[hi];
        auto hst = styleOf(he, hi);
        if (lowerStr(pick(he.attrs, hst, "display", "display", "")) == "none") hiddenSelf[hi] = 1;
        // <defs>/<symbol> 是模板（只在 <use> 的克隆里画），自己永远不生成节点
        if (he.attrs.count("_def")) hiddenSelf[hi] = 1;
    }
    for (size_t hi = 0; hi < elems.size(); hi++) {
        size_t k = hi;
        size_t guard = elems.size() + 1;   // 防御 parentIdx 成环（脏输入）
        while (k != SIZE_MAX && k < elems.size() && guard-- > 0) {
            if (hiddenSelf[k]) { hidden[hi] = 1; break; }
            k = elems[k].parentIdx;
        }
    }

    for (size_t ei = 0; ei < elems.size(); ei++) {
        if (id >= 0x10000) break; // 节点 id 为 uint16_t，超限停止生成
        if (hidden[ei]) continue; // display:none 的整棵子树都不生成节点（见上面的预处理）
        const auto& e = elems[ei];
        auto style = styleOf(e, ei);
        uint8_t type = tagToType(e.tag);

        // 认识但不生效的常见 CSS 属性：明确警告（同一属性全文档只报一条），
        // 避免"写了没效果"这种最耗时间的困惑。
        // v13 起 border*/font-weight/line-height/text-decoration 已经实现，从清单里移除。
        // v14 起 font-style（italic）/white-space（pre）也实现了，同样移除。
        // v23 起 box-shadow、v24 起 text-shadow 也实现了，两者都从清单里移除。
        {
            static const char* kIgnoredCss[] = {
                "word-break", "visibility", "float",
                "outline",
                "cursor", "grid-template-columns", "object-fit",
                // v30：right / bottom / inset 已实现（贴右/贴下 + 四值简写），从清单里移除
        "transform", "aspect-ratio",
            };
            for (const char* k : kIgnoredCss)
                if (style.count(k))
                    Warn(std::string("CSS 属性 ") + k + " 暂不支持，已忽略");

            // 滚动条颜色/宽度：CSS 侧没有对应通道（后端写死 8px/#2E2E2E/#5A5A5A，
            // 浅色主题下是一条黑杠，issue 05）。如果静默忽略，作者会以为改主题生效了。
            // 同一属性全文档只报一条（Warn 自带去重）。
            static const char* kScrollbarCss[] = { "scrollbar-color", "scrollbar-width" };
            for (const char* k : kScrollbarCss)
                if (style.count(k))
                    Warn(std::string("CSS 属性 ") + k +
                         " 暂不支持，请用 binui_set_scrollbar_style() 在运行期设置");
        }

        // ---- white-space / <pre>：文本是否保留原始换行与缩进 ----
        // 解析阶段同时存了折叠版（text）与原样版（rawText），这里按 CSS 选择用哪一份。
        // 以前 <pre> 的缩进被当普通空白折叠掉，代码块排版整个塌成一行。
        const std::string wsVal = lowerStr(pick(e.attrs, style, "white-space", "white-space", ""));
        bool keepWs = (wsVal == "pre" || wsVal == "pre-wrap" || wsVal == "break-spaces" ||
                       wsVal == "pre-line");
        if (!wsVal.empty() && !keepWs)
            Warn("CSS white-space 只支持 pre/pre-wrap/pre-line，\"" + wsVal + "\" 按 normal 处理");
        std::string preText;
        if (keepWs) {
            preText = e.rawText;
            // HTML 规范：<pre> 紧跟开始标签的那个换行不算内容
            if (!preText.empty() && preText[0] == '\n') preText.erase(0, 1);
            else if (preText.size() >= 2 && preText[0] == '\r' && preText[1] == '\n') preText.erase(0, 2);
        }
        const std::string& srcText = keepWs ? preText : e.text;

        // ---- <input>：按 type 决定节点语义 ----
        //   button/submit/reset     → BUTTON（文案取 value）
        //   checkbox/radio          → 18×18 方块/圆点（LIST + 可交互，勾选视觉交给 :checked）
        //   hidden                  → 完全不生成节点（以前会画成一个 40px 高的可见输入框）
        //   其余（text/password/…） → INPUT 单行文本框
        std::string itype = "text";
        bool isCheckbox = false;
        std::string inputVal;        // 输入框真实值 / 按钮文案
        bool inputPlaceholder = false; // inputVal 是 placeholder（真实值为空）
        if (e.tag == "input") {
            itype = lowerStr(pick(e.attrs, style, "type", "type", "text"));
            if (itype == "hidden") {
                continue;
            } else if (itype == "button" || itype == "submit" || itype == "reset") {
                type = BINUI_BUTTON;
            } else if (itype == "checkbox" || itype == "radio") {
                type = BINUI_LIST;
                isCheckbox = true;
                // 作者没写外观时给一个能看出来的默认样子（走既有边框/底色通道）
                if (!style.count("border") && !style.count("border-width") &&
                    !style.count("border-top") && !style.count("border-color"))
                    style["border"] = "2px solid #8A8A94";
                if (!style.count("border-radius")) style["border-radius"] = (itype == "radio") ? "9" : "3";
                if (!style.count("background") && !style.count("background-color"))
                    style["background"] = "transparent";
            } else {
                type = BINUI_INPUT;
                // 只做了单行文本：其余 HTML 输入类型当文本框处理，但要说明白
                static const std::unordered_set<std::string> kTextLike = {
                    "text", "password", "search", "email", "number", "tel", "url",
                    "date", "time", "datetime-local", "month", "week", "color", "range", "file",
                };
                if (!kTextLike.count(itype))
                    Warn("input type=\"" + itype + "\" 不支持，已按普通文本框编译");
                if (itype == "password")
                    Warn("input type=password 不遮蔽显示（binUI 没有掩码渲染），按明文文本框编译");
            }

            // 文本来源：value 是真实值，placeholder 只占位（渲染成 60% 灰，首次输入即清空）。
            // 以前 placeholder 被直接写进真实文本 —— 点进去就能编辑提示语，删掉还算一次 INPUT。
            const std::string val = e.attrs.count("value") ? e.attrs.at("value") : "";
            if (type == BINUI_BUTTON) {
                inputVal = val;
                if (inputVal.empty()) {   // 浏览器默认按钮文案
                    if (itype == "submit") inputVal = "Submit";
                    else if (itype == "reset") inputVal = "Reset";
                    else if (itype == "button") inputVal = "Button";
                }
            } else if (type == BINUI_INPUT && !val.empty()) {
                inputVal = val;
            } else if (type == BINUI_INPUT && e.attrs.count("placeholder")) {
                inputVal = e.attrs.at("placeholder");
                inputPlaceholder = true;
            }
        }

        if (e.tag=="head" || e.tag=="title" || e.tag=="meta" || e.tag=="link" ||
            e.tag=="script" || e.tag=="style" || e.tag=="app_window")
            continue;

        if (type == BINUI_ROOT) {
            // html/body：更新根节点背景（尺寸已在第 3 步读取；<app_window> 已提供 bg 则不再覆盖）
            if (!hasAppWindow) {
                std::string bg = pick(e.attrs, style, "bg", "background", "");
                if (bg.empty()) bg = pick(e.attrs, style, "color", "background-color", "");
                if (!bg.empty()) nodes[0].color = parseColor(bg);
            }
            // 根元素的 CSS color 是整棵树的默认文字色（继承的起点）。
            // 以前这里直接 continue，导致 body{color:#333} 完全没用。
            {
                std::string tc = pick(e.attrs, style, "text-color", "color", "");
                if (!tc.empty()) {
                    stack.back().textColor = parseColorWith(tc, 0xFFFFFFFFu, "文字颜色");
                    stack.back().hasText = true;
                }
            }
            continue;
        }

        uint32_t pnodeId = resolveParentId(e.parentIdx);
        if (pnodeId == 0xFFFF) pnodeId = 0; // 顶层挂根

        // 收缩上下文栈到父容器
        while (stack.size() > 1 && stack.back().nodeId != pnodeId)
            popAndPropagate();
        Ctx& pc = stack.back();

        // ---- SVG 坐标与颜料继承（v15）----
        // 坐标：约定 绝对坐标 = (pc.ox, pc.oy) + svgX × 用户坐标，
        //   所以下面所有图形代码照旧只加 pc.ox/pc.oy，变换全部折进 svgX。
        // 颜料：fill/stroke/stroke-width/fill-rule 是**可继承**的表现属性；
        //   CSS 规则优先于表现属性（SVG 规范，与 binUI 其它属性"属性盖 CSS"相反 ——
        //   这里按规范来，因为现成 SVG 素材都是属性写法，与 CSS 冲突才会踩到）。
        const bool inSvg = pc.inSvg || (e.tag == "svg");
        PathMatrix svgOwn;          // 本元素自己的 transform（父空间）
        bool hasOwnXform = false;
        PathMatrix svgX = pc.svgXform;
        if (inSvg) {
            auto tfIt = e.attrs.find("transform");
            if (tfIt != e.attrs.end() && !tfIt->second.empty()) {
                std::string terr;
                if (ParseSvgTransform(tfIt->second, svgOwn, &terr)) {
                    hasOwnXform = true;
                    if (e.tag != "svg") svgX = PathMul(svgX, svgOwn);
                } else {
                    Warn("transform 属性解析失败（支持 translate/scale/rotate/skewX/skewY/matrix）：" + terr);
                }
            }
        }
        uint32_t eFill = pc.fillColor;
        bool eHasFill = pc.hasFill;
        uint32_t eStroke = pc.strokeColor;
        bool eHasStroke = pc.hasStroke;
        uint8_t eStrokeW = pc.strokeW;
        bool eEvenOdd = pc.evenOdd;
        bool eAA = pc.aa;          // v19：shape-rendering 的继承值
        std::string fillGradId = pc.fillGradId;   // v16：继承下来的渐变填充（空 = 纯色）
        std::string strokeGradId = pc.strokeGradId;  // v17：继承下来的渐变描边（空 = 纯色）
        // v0.16.25：pattern 填充（空 = 不是 pattern）。平铺网格必须按**用户空间**算，
        // 所以另外留一份"过 svgX 之前"的包围盒。
        std::string patternFillId;
        float patUBx = 0.0f, patUBy = 0.0f, patUBw = 0.0f, patUBh = 0.0f;
        bool eFillStated = pc.fillStated;         // v18：颜料是否被显式给过（含祖先）
        bool eFillMentioned = pc.fillMentioned;   // fill 被提到过（含 fill="none"）
        bool eStrokeStated = pc.strokeStated;
        if (inSvg) {
            // CSS（含内联 style / 类规则）> 表现属性 > 继承 > SVG 默认
            auto pickSvgPaint = [&](const char* key) -> std::string {
                auto it = style.find(key);
                if (it != style.end() && !it->second.empty()) return it->second;
                auto a = e.attrs.find(key);
                return a != e.attrs.end() ? a->second : std::string();
            };
            const uint32_t curCol = pc.hasText ? pc.textColor : defaultTextCol;
            auto parsePaint = [&](const std::string& v, uint32_t dflt) -> uint32_t {
                return lowerStr(v) == "currentcolor" ? curCol : parseColorWith(v, dflt, "SVG 颜色");
            };
            // 渐变填充的代表色：取中间色标（运行期兜底/不支持渐变的通路用这个颜色）
            auto gradMidColor = [](const SvgGradDef& g) -> uint32_t {
                if (g.stops.empty()) return 0xFF000000u;
                return g.stops[g.stops.size() / 2].color;
            };
            const std::string f = pickSvgPaint("fill");
            if (!f.empty()) eFillMentioned = true;
            if (!f.empty()) {
                std::string gid;
                if (lowerStr(f) == "none") {
                    eHasFill = false;
                    eFillStated = false;          // fill="none" 不算"给了颜料"
                    fillGradId.clear();
                } else if (ParseUrlId(f, gid)) {
                    auto pit = svgPatterns.find(gid);
                    auto git = svgGrads.find(gid);
                    if (pit != svgPatterns.end() && pit->second.found) {
                        patternFillId = gid;          // v0.16.25：编译期平铺（不占格式版本）
                        fillGradId.clear();
                        eFillStated = true;
                        eHasFill = true;              // 元素本身不填，展开出来的 tile 才填
                    } else if (git == svgGrads.end() || !git->second.found || git->second.stops.empty()) {
                        Warn("fill=\"url(#" + gid + ")\" 找不到渐变或 <pattern> 定义（按纯色填充，"
                             "请检查 id 拼写或色标是否写在 <linearGradient> 里）");
                        fillGradId.clear();
                        eFillStated = true;
                        eHasFill = true;              // 退回纯色（保持原来的 eFill）
                    } else {
                        fillGradId = gid;             // 色标等细节在路径段发射时再展开
                        eFillStated = true;
                        eHasFill = true;
                        eFill = gradMidColor(git->second);
                    }
                } else {
                    eFill = parsePaint(f, eFill);
                    eHasFill = true;
                    eFillStated = true;
                    fillGradId.clear();
                }
            }
            const std::string s = pickSvgPaint("stroke");
            if (!s.empty()) {
                std::string sgid;
                if (lowerStr(s) == "none") {
                    eHasStroke = false;
                    eStrokeStated = false;
                    strokeGradId.clear();
                } else if (ParseUrlId(s, sgid)) {
                    // v17：描边渐变真的用了（以前只能告警退纯色）
                    auto git = svgGrads.find(sgid);
                    if (git == svgGrads.end() || !git->second.found || git->second.stops.empty()) {
                        Warn("stroke=\"url(#" + sgid + ")\" 找不到渐变定义（按纯色描边，"
                             "请检查 id 拼写或色标是否写在 <linearGradient> 里）");
                        strokeGradId.clear();
                        eStrokeStated = true;
                        eHasStroke = true;            // 退回纯色（保持原来的 eStroke）
                    } else {
                        strokeGradId = sgid;          // 色标等细节在路径段发射时再展开
                        eStrokeStated = true;
                        eHasStroke = true;
                        eStroke = git->second.stops[0].color;   // 代表色：首个色标（兜底用）
                    }
                } else {
                    eStroke = parsePaint(s, eStroke);
                    eHasStroke = true;
                    eStrokeStated = true;
                    strokeGradId.clear();   // 纯色覆盖祖先继承来的渐变描边
                }
            }
            const std::string swv = pickSvgPaint("stroke-width");
            if (!swv.empty()) {
                // 用户单位 → 设备像素（与坐标同一个变换系数），否则放大后线还那么细
                float wv = parseLen(swv, 1.0f) * PathScaleFactor(svgX);
                if (wv < 0.0f) wv = 0.0f;
                eStrokeW = (uint8_t)std::min(255.0f, wv);
                if (eStrokeW == 0) eHasStroke = false;
            }
            const std::string fr = pickSvgPaint("fill-rule");
            if (!fr.empty()) eEvenOdd = (lowerStr(fr) == "evenodd");
            // v19：shape-rendering 决定填充边缘要不要抗锯齿。
            // 默认（没写/auto 以外的未知值）保持 binUI 的硬边；显式 auto/optimizeQuality 才开 AA。
            const std::string sr = pickSvgPaint("shape-rendering");
            if (!sr.empty()) {
                const std::string lv = lowerStr(sr);
                if (lv == "auto" || lv == "optimizequality") eAA = true;
                else if (lv == "crispedges" || lv == "optimizespeed") eAA = false;
                else Warn("shape-rendering=\"" + sr + "\" 不认识，可选 auto | optimizeQuality | "
                          "crispEdges | optimizeSpeed（前两个开边缘抗锯齿）");
            }
        }

        // <polyline points="x,y x,y ..."> / <polygon>：展开成若干 BINUI_LINE 子节点
        // 坐标为 svg 容器坐标系（pc.ox/pc.oy 即容器内容盒原点）
        // <polyline points="x,y x,y …"> / <polygon>：**编成路径**（v18 之前是拆成一堆 BINUI_LINE，
        // 于是填充完全无效、渐变描边也用不了）。points 借 ParsePathData 解析 ——
        // 它能吃 "1.5.5"、"1-2"、逗号/空白混排这些写法，自己手写切词反而漏格式。
        // 裸 <polyline>（没写 fill 也没写 stroke）不产生节点：SVG 默认黑填充会把漏写属性的
        // 折线变成一块黑色色块，比"什么都不画"更难查。
        const bool polyPaintable = (e.tag == "polyline" || e.tag == "polygon") &&
                                   ((eHasFill && eFillStated) || (eHasStroke && eStrokeStated));
        if (e.tag == "polyline" || e.tag == "polygon") {
            if (!polyPaintable) continue;   // 不生成节点、不推容器上下文
            // 有描边、却没写 fill：按 SVG 规范这是"黑色填充"（浏览器同样如此，是经典坑）。
            // 折线/轮廓被填成一块黑比不画更难查，所以明确告警并点出替代写法。
            if (eHasStroke && eStrokeStated && !eFillMentioned) {
                Warn("<" + e.tag + "> 没写 fill：SVG 默认是黑色填充（浏览器同理），"
                     "只要描边请写 fill=\"none\"");
            }
        }

        // <path d="…"> / <ellipse cx cy rx ry>：编译期细分成折线
        //
        // 坐标与 <line>/<circle> 一致：svg 容器坐标系（pc.ox/pc.oy 为内容盒原点），
        // 节点盒 = 几何包围盒（有描边时外扩 strokeW/2，笔画就不会溢出节点盒、命中区也盖得住）。
        // 细分容差 0.1 用户单位：24 单位的图标 ≈ 0.1px，放大 3 倍也就 0.3px。
        // 归一化点由路径段存，运行期只做"归一化点 × 节点盒"，不重算曲线。
        PathShape svgPathShape;
        // v0.16.23：marker 几何单独攒一份，编译完再**单独成节点**。为什么不能直接并进
        // svgPathShape：引用路径可能是 fill:none（典型箭头），而 marker 是填充形状，
        // 一个节点只有一个填充色 —— 混在一起会把开放路径也填上（第一版实拍就画成了
        // 一整块蓝色三角形）。
        PathShape svgMarkerShape;
        uint32_t svgMarkerFill = 0xFF000000u;
        bool svgMarkerFillSet = false;
        bool hasSvgPath = false;
        bool pathFill = true, pathEvenOdd = false, pathStroke = false;
        uint32_t pathFillColor = 0xFF000000u, pathStrokeColor = 0xFF000000u;
        float pathStrokeW = 0.0f;
        uint8_t pathCap = BINUI_CAP_BUTT;   // stroke-linecap
        uint8_t pathJoin = BINUI_JOIN_ROUND; // stroke-linejoin（圆角是 binUI 的历史默认）
        uint8_t pathMiterLimit = 0;          // stroke-miterlimit（0 = 运行期按 SVG 默认 4）
        bool pathHasGrad = false;           // v16：填充用渐变
        BinUIPathGradient pathGrad = {};
        bool pathHasStrokeGrad = false;     // v17：描边用渐变（记录写在填充渐变之后）
        BinUIPathGradient pathStrokeGrad = {};
        // <line>：只有两个端点，平时用 BINUI_LINE 更省（一个节点、命中测试也简单）；
        // 但虚线 / 渐变描边 / 端帽 / 连接样式这四样 LINE 节点表达不了，遇到就升级成 2 点开放路径。
        // v0.16.23：带 marker 的 <line> 也必须升级成路径 —— marker 的展开只发生在路径
        // 分支（要顶点 + 切线方向），BINUI_LINE 节点只有两个端点、不参与 marker。
        const bool lineHasMarker = (e.tag == "line") &&
            (style.count("marker") || e.attrs.count("marker") ||
             style.count("marker-start") || e.attrs.count("marker-start") ||
             style.count("marker-mid") || e.attrs.count("marker-mid") ||
             style.count("marker-end") || e.attrs.count("marker-end"));
        const bool lineNeedsPath = (e.tag == "line") &&
            (style.count("stroke-dasharray") || e.attrs.count("stroke-dasharray") ||
             !strokeGradId.empty() || lineHasMarker ||
             style.count("stroke-linecap") || e.attrs.count("stroke-linecap") ||
             style.count("stroke-linejoin") || e.attrs.count("stroke-linejoin"));
        // <path>/<ellipse>/<polyline>/<polygon>：永远是路径；<line> 按需升级。
        // <circle>/<rect>：带描边、或填充是渐变时必须编成路径 —— 轻量映射（LIST + radius）
        // 只能表达一个颜色，fill 与 stroke 并存、多条色标都走不通；没有描边也不是渐变时
        // 继续走轻量映射，因为圆角矩形的九宫格填充比扫描线条带更干净（边缘更准、也没有取整毛边）。
        const bool shapeAsPath = (e.tag == "path" || e.tag == "ellipse" || e.tag == "polyline" ||
                                  e.tag == "polygon" || lineNeedsPath) ||
                                 ((e.tag == "circle" || e.tag == "rect") &&
                                  ((eHasStroke && eStrokeW > 0) || !fillGradId.empty() ||
                                   !patternFillId.empty()));
        if (shapeAsPath) {
            if (e.tag == "path") {
                const std::string d = e.attrs.count("d") ? e.attrs.at("d") : "";
                std::string perr;
                if (d.empty()) {
                    Warn("<path> 没有 d 属性（空路径不生成节点）");
                } else if (!ParsePathData(d, 0.1f, svgPathShape, &perr)) {
                    Warn("<path> 的 d 解析失败：" + perr);
                }
            } else if (e.tag == "polyline" || e.tag == "polygon") {
                // points 的每对数字等价于 M/L 的参数：借 ParsePathData 的切词与纠错，
                // "M10 10 20 20 30 30" = 一个 moveto + 两个 lineto，正好是折线语义。
                const std::string pts = e.attrs.count("points") ? e.attrs.at("points") : "";
                std::string perr;
                const std::string d = "M" + pts + (e.tag == "polygon" ? " Z" : "");
                if (pts.empty()) {
                    Warn("<" + e.tag + "> 没有 points 属性（空路径不生成节点）");
                } else if (!ParsePathData(d, 0.1f, svgPathShape, &perr)) {
                    Warn("<" + e.tag + "> 的 points 解析失败：" + perr);
                }
            } else if (e.tag == "line") {
                // 升级成 2 点开放路径（坐标是用户单位，稍后统一过 svgX）
                const float x1 = parseLen(e.attrs.count("x1") ? e.attrs.at("x1") : "", 0);
                const float y1 = parseLen(e.attrs.count("y1") ? e.attrs.at("y1") : "", 0);
                const float x2 = parseLen(e.attrs.count("x2") ? e.attrs.at("x2") : "", 0);
                const float y2 = parseLen(e.attrs.count("y2") ? e.attrs.at("y2") : "", 0);
                PathShape::Contour ct;
                ct.first = 0;
                ct.count = 2;
                ct.closed = false;
                svgPathShape.pts.push_back({ x1, y1 });
                svgPathShape.pts.push_back({ x2, y2 });
                svgPathShape.contours.push_back(ct);
                svgPathShape.RecomputeBounds();
            } else if (e.tag == "ellipse") {
                const float ecx = parseLen(e.attrs.count("cx") ? e.attrs.at("cx") : "", 0);
                const float ecy = parseLen(e.attrs.count("cy") ? e.attrs.at("cy") : "", 0);
                const float erx = parseLen(e.attrs.count("rx") ? e.attrs.at("rx") : "", 0);
                const float ery = parseLen(e.attrs.count("ry") ? e.attrs.at("ry") : "", 0);
                if (erx <= 0.0f || ery <= 0.0f) Warn("<ellipse> 的 rx/ry 必须大于 0（跳过）");
                else AppendEllipse(svgPathShape, ecx, ecy, erx, ery, 0.1f);
            } else if (e.tag == "circle") {
                const float ccx = parseLen(e.attrs.count("cx") ? e.attrs.at("cx") : "", 0);
                const float ccy = parseLen(e.attrs.count("cy") ? e.attrs.at("cy") : "", 0);
                const float cr = parseLen(e.attrs.count("r") ? e.attrs.at("r") : "", 0);
                if (cr <= 0.0f) Warn("<circle> 的 r 必须大于 0（跳过）");
                else AppendEllipse(svgPathShape, ccx, ccy, cr, cr, 0.1f);
            } else {   // rect（带描边时才走到这里）
                const float rx0 = parseLen(e.attrs.count("x") ? e.attrs.at("x") : "", 0);
                const float ry0 = parseLen(e.attrs.count("y") ? e.attrs.at("y") : "", 0);
                const float rw0 = parseLen(e.attrs.count("width") ? e.attrs.at("width") : "", 0);
                const float rh0 = parseLen(e.attrs.count("height") ? e.attrs.at("height") : "", 0);
                float cr = 0.0f;
                if (e.attrs.count("rx")) cr = parseLen(e.attrs.at("rx"), 0);
                else if (e.attrs.count("ry")) cr = parseLen(e.attrs.at("ry"), 0);
                if (rw0 <= 0.0f || rh0 <= 0.0f) Warn("<rect> 的 width/height 必须大于 0（跳过）");
                else AppendRoundedRect(svgPathShape, rx0, ry0, rw0, rh0, cr, 0.1f);
            }
            if (svgPathShape.truncated)
                Warn("<path> 细分点数超过上限已截断：简化路径或拆成多段");
            if (!svgPathShape.Empty()) {
                // v0.16.25：pattern 网格在用户空间算，先留一份未过 svgX 的包围盒
                if (!patternFillId.empty()) {
                    patUBx = svgPathShape.minX;
                    patUBy = svgPathShape.minY;
                    patUBw = svgPathShape.Width();
                    patUBh = svgPathShape.Height();
                }
                // viewBox / <g transform>（含自己的 transform）一次性映射完
                TransformShape(svgPathShape, svgX);
                // fill / stroke 走继承链（见上面的 SVG 颜料块），none = 不画
                pathFill = eHasFill;
                pathFillColor = eFill;
                pathStroke = eHasStroke && eStrokeW > 0;
                pathStrokeColor = eStroke;
                pathStrokeW = (float)eStrokeW;
                pathEvenOdd = eEvenOdd;
                // 半透明折进 alpha（与 static opacity 同一做法，不做离屏合成）
                // 取值顺序：CSS（含内联 style）> 表现属性 —— 以前只看 style，
                // 于是 fill-opacity="0.5" / stroke-opacity="0.5" 这种属性写法被静默忽略。
                auto fade = [&](uint32_t c, const char* key) {
                    std::string ov;
                    auto oit = style.find(key);
                    if (oit != style.end()) ov = oit->second;
                    else {
                        auto ait = e.attrs.find(key);
                        if (ait != e.attrs.end()) ov = ait->second;
                    }
                    if (ov.empty()) return c;
                    float op = parseLen(ov, 1.0f, key);
                    op = op < 0.0f ? 0.0f : (op > 1.0f ? 1.0f : op);
                    const uint32_t a = (uint32_t)(((c >> 24) & 0xFFu) * op + 0.5f);
                    return (a << 24) | (c & 0x00FFFFFFu);
                };
                pathFillColor = fade(pathFillColor, "fill-opacity");
                pathStrokeColor = fade(pathStrokeColor, "stroke-opacity");
                pathFillColor = fade(pathFillColor, "opacity");
                pathStrokeColor = fade(pathStrokeColor, "opacity");
                // stroke-dasharray：编译期按弧长把折线切成实线段（运行期照常画折线）。
                // 长度是**用户单位**，而几何已经被 svgX 映射过，所以要乘同一个缩放系数。
                if (pathStroke) {
                    std::string da = style.count("stroke-dasharray") ? style.at("stroke-dasharray") : "";
                    if (da.empty() && e.attrs.count("stroke-dasharray"))
                        da = e.attrs.at("stroke-dasharray");
                    if (!da.empty() && lowerStr(da) != "none") {
                        std::vector<float> pat;
                        std::string cur;
                        const float xk = PathScaleFactor(svgX);
                        for (char ch : da) {
                            if (ch == ',' || ch == ' ' || ch == '\t') {
                                if (!cur.empty()) { pat.push_back(parseLen(cur, 0) * xk); cur.clear(); }
                            } else {
                                cur += ch;
                            }
                        }
                        if (!cur.empty()) pat.push_back(parseLen(cur, 0) * xk);
                        if (pat.size() % 2 == 1) pat.insert(pat.end(), pat.begin(), pat.end());
                        // stroke-dashoffset：与 pattern 同一个口径（用户单位 × svgX 缩放），
                        // 负值由 ApplyDashPattern 内部取模折算
                        float dashOffset = 0.0f;
                        std::string doff = style.count("stroke-dashoffset")
                                            ? style.at("stroke-dashoffset") : "";
                        if (doff.empty() && e.attrs.count("stroke-dashoffset"))
                            doff = e.attrs.at("stroke-dashoffset");
                        if (!doff.empty())
                            dashOffset = parseLen(doff, 0.0f, "stroke-dashoffset") * xk;
                        if (!ApplyDashPattern(svgPathShape, pat, dashOffset)) {
                            Warn("stroke-dasharray 把整条路径吃光了（描边已关闭）");
                            pathStroke = false;
                        }
                    }
                }
                // stroke-linecap：butt（默认）/ round（两端补圆点）；square 按 butt 画并告警。
                // stroke-linejoin：round（默认）/ miter / bevel 都实现了（缺口填多边形）；
                //   miter 超过 stroke-miterlimit（SVG 默认 4）时按 bevel 画，与规范一致。
                if (pathStroke) {
                    std::string lc = style.count("stroke-linecap") ? style.at("stroke-linecap") : "";
                    if (lc.empty() && e.attrs.count("stroke-linecap"))
                        lc = e.attrs.at("stroke-linecap");
                    lc = lowerStr(lc);
                    if (lc == "round") pathCap = BINUI_CAP_ROUND;
                    else if (lc == "square") pathCap = BINUI_CAP_SQUARE;   // v17：两端各外延半线宽
                    else if (!lc.empty() && lc != "butt")
                        Warn("stroke-linecap=\"" + lc + "\" 不认识，按 butt 画"
                             "（可选 butt / round / square）");
                    std::string lj = style.count("stroke-linejoin") ? style.at("stroke-linejoin") : "";
                    if (lj.empty() && e.attrs.count("stroke-linejoin"))
                        lj = e.attrs.at("stroke-linejoin");
                    lj = lowerStr(lj);
                    if (lj == "miter" || lj == "miter-clip") {
                        pathJoin = BINUI_JOIN_MITER;
                    } else if (lj == "bevel") {
                        pathJoin = BINUI_JOIN_BEVEL;
                    } else if (lj == "round") {
                        pathJoin = BINUI_JOIN_ROUND;
                    } else if (!lj.empty()) {
                        Warn("stroke-linejoin=\"" + lj + "\" 不认识，按圆角连接画（可选 miter / bevel / round）");
                    }
                    // stroke-miterlimit：只对 miter 有用；格式里借 head.reserve 这个空闲字节存
                    std::string ml = style.count("stroke-miterlimit") ? style.at("stroke-miterlimit") : "";
                    if (ml.empty() && e.attrs.count("stroke-miterlimit"))
                        ml = e.attrs.at("stroke-miterlimit");
                    if (!ml.empty()) {
                        const float lim = parseLen(ml, 4.0f, "stroke-miterlimit");
                        if (lim < 1.0f)
                            Warn("stroke-miterlimit 不能小于 1，已按 1 处理（规范要求 ≥1）");
                        pathMiterLimit = (uint8_t)std::min(255.0f, std::max(1.0f, lim));
                    }
                }
                // ---- v0.16.23：marker-start / marker-mid / marker-end ----
                // 顶点与方向都取**设备空间**的点（已过 svgX），所以旋转/缩放的路径方向天然正确。
                // 实例几何直接追加进 svgPathShape：一条路径仍是一个节点、一份颜料，
                // 运行期照旧"归一化点 × 节点盒"，没有新格式段、没有后端改动。
                if (!svgMarkers.empty()) {
                    auto markerRef = [&](const char* attr, int slot) -> std::string {
                        std::string v = style.count(attr) ? TrimCopy(style.at(attr)) : "";
                        if (v.empty() && e.attrs.count(attr)) v = TrimCopy(e.attrs.at(attr));
                        if (v.empty()) {   // CSS 简写 marker: <start> <mid> <end>
                            std::string sh = style.count("marker") ? TrimCopy(style.at("marker")) : "";
                            if (sh.empty() && e.attrs.count("marker"))
                                sh = TrimCopy(e.attrs.at("marker"));
                            if (!sh.empty()) {
                                std::vector<std::string> parts;
                                std::string cur;
                                for (char ch : sh) {
                                    if (ch == ' ' || ch == '\t') {
                                        if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
                                    } else cur += ch;
                                }
                                if (!cur.empty()) parts.push_back(cur);
                                if (parts.size() == 1) v = parts[0];
                                else if (static_cast<int>(parts.size()) > slot) v = parts[slot];
                            }
                        }
                        if (v.empty() || lowerStr(v) == "none") return "";
                        std::string gid;
                        if (!ParseUrlId(v, gid)) {
                            Warn("marker 属性不是 url(#id) 写法，已忽略：" + v);
                            return "";
                        }
                        return gid;
                    };
                    bool warnedFill = false;
                    int totalInst = 0;
                    auto placeSlot = [&](const std::string& gid, int slot) {
                        if (gid.empty()) return;
                        auto mit = svgMarkers.find(gid);
                        if (mit == svgMarkers.end()) {
                            Warn("marker 引用 \"url(#" + gid + ")\" 找不到 <marker> 定义");
                            return;
                        }
                        const SvgMarkerDef& md = mit->second;
                        if (md.kids.empty()) return;
                        const float unit = md.userUnits ? 1.0f
                                       : (pathStrokeW > 0.0f ? pathStrokeW : 1.0f);
                        PathMatrix V;                       // 默认单位阵
                        PathPointF pref{ md.refX, md.refY };
                        if (md.vbW > 0.0f && md.vbH > 0.0f) {
                            V = PathMul(PathScale(md.mw / md.vbW, md.mh / md.vbH),
                                        PathTranslate(-md.vbX, -md.vbY));
                            pref = PathApply(V, md.refX, md.refY);
                        }
                        struct Hit { float x, y, ang; };
                        std::vector<Hit> hits;
                        const size_t nc = svgPathShape.contours.size();
                        for (size_t ci2 = 0; ci2 < nc; ci2++) {
                            const PathShape::Contour& ct2 = svgPathShape.contours[ci2];
                            if (ct2.count < 2) continue;
                            auto pt = [&](size_t k) -> PathPointF {
                                return svgPathShape.pts[ct2.first + (k % ct2.count)];
                            };
                            if (slot == 0 && ci2 == 0) {
                                const PathPointF a = pt(0), bq = pt(1);
                                hits.push_back({ a.x, a.y,
                                    std::atan2(bq.y - a.y, bq.x - a.x) * 57.29577951308232f });
                            } else if (slot == 2 && ci2 + 1 == nc) {
                                const PathPointF a = pt(ct2.count - 2), bq = pt(ct2.count - 1);
                                hits.push_back({ bq.x, bq.y,
                                    std::atan2(bq.y - a.y, bq.x - a.x) * 57.29577951308232f });
                            } else if (slot == 1) {
                                for (uint32_t k = 1; k + 1 < ct2.count; k++) {
                                    const PathPointF p0 = pt(k), p1 = pt(k + 1);
                                    hits.push_back({ p0.x, p0.y,
                                        std::atan2(p1.y - p0.y, p1.x - p0.x) * 57.29577951308232f });
                                }
                            }
                        }
                        for (const Hit& h : hits) {
                            if (++totalInst > 4000) {
                                Warn("marker 实例超过 4000 个，其余已丢弃（路径顶点太多）");
                                break;
                            }
                            float ang = (md.orient == 2) ? md.angle : h.ang;
                            if (md.orient == 1 && slot == 0) ang += 180.0f;
                            // 内容 → viewBox(V) → 对齐参考点 → 单位缩放 → 旋转 → 平移到顶点
                            PathMatrix M = PathMul(PathTranslate(-pref.x, -pref.y), V);
                            if (unit != 1.0f) M = PathMul(PathScale(unit, unit), M);
                            if (ang != 0.0f) M = PathMul(PathRotate(ang), M);
                            M = PathMul(PathTranslate(h.x, h.y), M);
                            M = PathMul(svgX, M);          // 用户空间 → 设备空间
                            for (const SvgMarkerChild& kid : md.kids) {
                                if (!kid.hasFill && !kid.hasStroke) continue;
                                // 填充色：marker 内容自己写了实色就用它（多个 marker 各写各的
                                // 颜色时只认第一个并告警）；没写 / currentColor / context-stroke
                                // 就用引用路径的描边色（SVG 的 context-stroke 语义）。
                                if (!svgMarkerFillSet) {
                                    if (kid.hasFill && !kid.fillCurrent) {
                                        svgMarkerFill = kid.fill;
                                    } else if (pathStroke) {
                                        svgMarkerFill = pathStrokeColor;
                                    } else {
                                        svgMarkerFill = pathFillColor;
                                    }
                                    svgMarkerFillSet = true;
                                } else if (kid.hasFill && !kid.fillCurrent &&
                                           kid.fill != svgMarkerFill && !warnedFill) {
                                    Warn("marker 只有一种填充色：后续实例声明的 fill 已被忽略");
                                    warnedFill = true;
                                }
                                const uint32_t base = static_cast<uint32_t>(svgMarkerShape.pts.size());
                                for (const PathPointF& sp : kid.shape.pts)
                                    svgMarkerShape.pts.push_back(PathApply(M, sp.x, sp.y));
                                for (const PathShape::Contour& sc : kid.shape.contours) {
                                    PathShape::Contour nc2;
                                    nc2.first = base + sc.first;
                                    nc2.count = sc.count;
                                    nc2.closed = sc.closed;
                                    svgMarkerShape.contours.push_back(nc2);
                                }
                            }
                        }
                    };
                    placeSlot(markerRef("marker-start", 0), 0);
                    placeSlot(markerRef("marker-mid", 1), 1);
                    placeSlot(markerRef("marker-end", 2), 2);
                    if (totalInst > 0 && !svgMarkerShape.Empty())
                        svgMarkerShape.RecomputeBounds();
                }
                // 几何盒（不含描边）：渐变 objectBoundingBox 与"斜接撑盒"都以它为基准
                const float geomMinX = svgPathShape.minX, geomMinY = svgPathShape.minY;
                const float geomW = svgPathShape.Width(), geomH = svgPathShape.Height();
                if (pathStroke) {   // 描边外扩后才是节点盒
                    const float e2 = pathStrokeW * 0.5f;
                    svgPathShape.minX -= e2; svgPathShape.minY -= e2;
                    svgPathShape.maxX += e2; svgPathShape.maxY += e2;
                }
                // 斜接尖角能伸出半线宽之外 (ratio-1)·半线宽。三个后端都"把每个节点裁剪到
                // 自己的矩形"，所以盒子必须盖住尖角，否则尖角会被切平（看起来像斜面）。
                // 用与运行期同一个 PathJoinAt 算交点，编译期与运行期不会各算一份。
                if (pathStroke && pathJoin == BINUI_JOIN_MITER) {
                    const float half = pathStrokeW * 0.5f;
                    const float lim = pathMiterLimit > 0 ? (float)pathMiterLimit : 4.0f;
                    for (const PathShape::Contour& ct : svgPathShape.contours) {
                        if (ct.count < 2) continue;
                        for (uint32_t k = 0; k < ct.count; k++) {
                            const PathPointF& a2 = svgPathShape.pts[ct.first + k];
                            const PathPointF& b2 = svgPathShape.pts[ct.first + (k + 1) % ct.count];
                            const PathPointF& c2 = svgPathShape.pts[ct.first + (k + 2) % ct.count];
                            if (!PathNeedJoin(a2.x, a2.y, b2.x, b2.y, c2.x, c2.y)) continue;
                            const PathJoinGeom jg = PathJoinAt(a2.x, a2.y, b2.x, b2.y,
                                                               c2.x, c2.y, half);
                            if (!jg.ok || jg.ratio > lim) continue;   // 超限按斜面画，不会伸出去
                            if (jg.mx < svgPathShape.minX) svgPathShape.minX = jg.mx;
                            if (jg.my < svgPathShape.minY) svgPathShape.minY = jg.my;
                            if (jg.mx > svgPathShape.maxX) svgPathShape.maxX = jg.mx;
                            if (jg.my > svgPathShape.maxY) svgPathShape.maxY = jg.my;
                        }
                    }
                }
                // ---- v16/v17 渐变：把"渐变空间"折进"节点盒归一化空间" ----
                // 运行期拿到的是归一化坐标（设备 = 节点盒左上 + 归一化 × 盒尺寸），
                // 所以这里把端点/圆心/半径用同一个仿射 M 折算过去，运行期只做插值。
                //   userSpaceOnUse : M = N ∘ svgX ∘ gradientTransform
                //   objectBBox     : M = N ∘ B ∘ gradientTransform（B 把单位正方形铺到几何盒）
                // 填充与描边**共用这一个 lambda**：两边唯一不同的是元素级透明度的键
                // （fill-opacity / stroke-opacity）——
                // 抄两份的代价是"改一处忘一处"，而这种几何最容易出现两处差一点点。
                auto toNormSpace = [&](const SvgGradDef& gd, const char* alphaKey,
                                       const std::string& idForWarn) -> BinUIPathGradient {
                    // userSpaceOnUse 的百分比按**视口尺寸**折算（SVG 规范）：x/y 用宽高，
                    // r 用对角线等效半径 sqrt((w²+h²)/2)。objectBoundingBox 下百分比就是包围盒比例，
                    // 已经在 /100 时算好了，不用动。
                    const SvgGradDef* gdp = &gd;
                    SvgGradDef gLocal;
                    if (gd.userSpace) {
                        const float vpW = pc.vpW > 1e-6f ? pc.vpW : 0.0f;
                        const float vpH = pc.vpH > 1e-6f ? pc.vpH : 0.0f;
                        if (vpW > 0.0f && vpH > 0.0f) {
                            gLocal = gd;
                            if (gLocal.x1Pct) gLocal.x1 *= vpW;
                            if (gLocal.x2Pct) gLocal.x2 *= vpW;
                            if (gLocal.cxPct) gLocal.cx *= vpW;
                            if (gLocal.fxPct) gLocal.fx *= vpW;
                            if (gLocal.y1Pct) gLocal.y1 *= vpH;
                            if (gLocal.y2Pct) gLocal.y2 *= vpH;
                            if (gLocal.cyPct) gLocal.cy *= vpH;
                            if (gLocal.fyPct) gLocal.fy *= vpH;
                            if (gLocal.rPct)
                                gLocal.r *= std::sqrt((vpW * vpW + vpH * vpH) * 0.5f);
                            gdp = &gLocal;
                        }
                    }
                    const SvgGradDef& g = *gdp;
                    const float boxX = svgPathShape.minX, boxY = svgPathShape.minY;
                    const float boxW = svgPathShape.Width(), boxH = svgPathShape.Height();
                    // N：设备坐标 → 节点盒归一化（先把盒原点挪到 0，再除以盒尺寸）。
                    // 注意 PathMul 的语义是 "先右后左"：这里要先平移再缩放，
                    // 所以写成 PathMul(S, T) 而不是 PathMul(T, S)。
                    const PathMatrix N = PathMul(
                        PathScale(boxW > 1e-6f ? 1.0f / boxW : 0.0f,
                                  boxH > 1e-6f ? 1.0f / boxH : 0.0f),
                        PathTranslate(-boxX, -boxY));
                    PathMatrix M = N;
                    if (g.userSpace) {
                        M = PathMul(M, PathMul(svgX, g.xform));
                    } else {
                        // objectBoundingBox：坐标是"几何包围盒（**不含描边**）的单位正方形"。
                        // 注意不能拿节点盒减 2·e2 反推：斜接撑盒可能是不对称的，
                        // 所以填充和描边必须用同一个 geomMinX/geomW。
                        const float fw = std::max(1e-6f, geomW);
                        const float fh = std::max(1e-6f, geomH);
                        M = PathMul(M, PathMul(PathTranslate(geomMinX, geomMinY),
                                               PathScale(fw, fh)));
                        M = PathMul(M, g.xform);
                    }
                    BinUIPathGradient pg = {};
                    pg.type = g.type;
                    pg.spread = g.spread;
                    // 色标：元素级的 fill-opacity / stroke-opacity + opacity 在这里折进 alpha
                    const int ns = static_cast<int>(
                        std::min<size_t>(kPathGradMaxStops, g.stops.size()));
                    pg.stopCount = static_cast<uint8_t>(std::max(2, ns));
                    for (int si = 0; si < ns; si++) {
                        float off = g.stops[si].off;
                        off = off < 0.0f ? 0.0f : (off > 1.0f ? 1.0f : off);
                        pg.stops[si].offset = off;
                        pg.stops[si].color = fade(g.stops[si].color, alphaKey);
                        pg.stops[si].color = fade(pg.stops[si].color, "opacity");
                    }
                    if (ns < 2) pg.stops[1] = pg.stops[0];   // 兜底（注册表已保证 ≥ 2）
                    if (g.type == PATH_GRAD_RADIAL) {
                        if (g.xformRotated) {
                            Warn("渐变 #" + idForWarn + "：径向渐变的 gradientTransform 含旋转/斜切，"
                                 "已近似为轴对齐椭圆（去掉旋转，或改用线性渐变）");
                        }
                        const PathPointF gc = PathApply(M, g.cx, g.cy);
                        pg.x1 = gc.x;
                        pg.y1 = gc.y;
                        // v18：焦点按同一个 M 折到归一化空间（= 圆心时运行期走快路）
                        const PathPointF gf = PathApply(M, g.fx, g.fy);
                        pg.fx = gf.x;
                        pg.fy = gf.y;
                        // 半径按 M 两列的范数放大：仿射变换下圆的轴长正好是这个
                        pg.rx = std::fabs(g.r) * std::sqrt(M.a * M.a + M.b * M.b);
                        pg.ry = std::fabs(g.r) * std::sqrt(M.c * M.c + M.d * M.d);
                    } else {
                        const PathPointF gp1 = PathApply(M, g.x1, g.y1);
                        const PathPointF gp2 = PathApply(M, g.x2, g.y2);
                        pg.x1 = gp1.x; pg.y1 = gp1.y;
                        pg.x2 = gp2.x; pg.y2 = gp2.y;
                    }
                    return pg;
                };
                if (pathFill && !fillGradId.empty()) {
                    auto git = svgGrads.find(fillGradId);
                    if (git != svgGrads.end() && git->second.found && !git->second.stops.empty()) {
                        pathGrad = toNormSpace(git->second, "fill-opacity", fillGradId);
                        pathHasGrad = true;
                    }
                }
                if (pathStroke && !strokeGradId.empty()) {
                    auto git = svgGrads.find(strokeGradId);
                    if (git != svgGrads.end() && git->second.found && !git->second.stops.empty()) {
                        pathStrokeGrad = toNormSpace(git->second, "stroke-opacity", strokeGradId);
                        pathHasStrokeGrad = true;
                    }
                    // 找不到定义的告警在上面颜料块里已经报过并清空了 id，这里不再重复
                }
                if (svgPathShape.Width() <= 0.0f || svgPathShape.Height() <= 0.0f) {
                    Warn("<path> 的包围盒为零（跳过：只有水平/垂直直线时请改用 <line>）");
                } else {
                    hasSvgPath = true;   // 包围盒在 <line>/<circle> 那段统一落进 sx/sy/sw/sh
                    type = BINUI_PATH;   // <circle>/<rect> 带描边时改走路径
                }
            }
        }

        // 局部（父级相对）定位值；x/y/w/h 支持 %（相对父容器内容盒）
        float lx = 0, ly = 0, lh = defaultHeight(e.tag, type);
        bool hasX = false, hasY = false, hasW = false;
        float pctX = -1, pctY = -1, pctW = -1, pctH = -1;

        std::string sx = pick(e.attrs, style, "x", "left", "");
        std::string sy = pick(e.attrs, style, "y", "top", "");
        std::string sw = pick(e.attrs, style, "w", "width", "");
        std::string sh = pick(e.attrs, style, "h", "height", "");
        if (sw.empty()) sw = pick(e.attrs, style, "width", "width", "");
        if (sh.empty()) sh = pick(e.attrs, style, "height", "height", "");
        // 宽度/高度/left/top:auto -> 视为未声明（流式自动宽度 / flex 弹性 / 默认高度）
        if (lowerStr(sx) == "auto") sx.clear();
        if (lowerStr(sy) == "auto") sy.clear();
        if (lowerStr(sw) == "auto") sw.clear();
        if (lowerStr(sh) == "auto") sh.clear();

        // <svg> 没写 width/height 时按 viewBox 的尺寸（否则落到通用默认 40px，图标被压扁）
        if (e.tag == "svg") {
            std::string vbs = e.attrs.count("viewbox") ? e.attrs.at("viewbox") : "";
            float vbx = 0, vby = 0, vbw = 0, vbh = 0;
            if (!vbs.empty() && ParseViewBox(vbs, vbx, vby, vbw, vbh)) {
                if (sw.empty()) sw = std::to_string(vbw);
                if (sh.empty()) sh = std::to_string(vbh);
            }
        }

        // 先确定本元素字号：width/height/margin/padding 里写的 em 以此为基准
        // （没有 CSS 级联，em 取元素自身字号；font-size 自身写 em 时按默认字号近似）
        float nodeFontSize = 14.0f;
        {
            const float dfltFs = DefaultFontSizeOf(e.tag);
            std::string fsv = pick(e.attrs, style, "size", "font-size", "");
            nodeFontSize = dfltFs;
            if (!fsv.empty()) {
                LenCtx fsCtx{ dfltFs, curCtx.rootFont, curCtx.viewW, curCtx.viewH };
                auto fp = ParseLenPct(fsv, dfltFs, fsCtx, "font-size");
                if (fp.ok && fp.value > 0) nodeFontSize = fp.value;
            }
            curCtx.fontSize = nodeFontSize;
        }

        // v0.16.40：字间距（CSS letter-spacing，**可继承**）。存在扩展绘制段的
        // `letterSpacing`（float 像素；0 = 未声明）。这里先算出"本元素生效值"，
        // 下面的行数估算用它，子容器再继承它。
        float nodeLetterSpacing = pc.letterSpacing;
        {
            std::string lsv = TrimCopy(pick(e.attrs, style, "letter-spacing", "letter-spacing", ""));
            if (!lsv.empty()) {
                const std::string lsl = lowerStr(lsv);
                if (lsl == "normal" || lsl == "0" || lsl == "0px") {
                    nodeLetterSpacing = 0.0f;   // 显式复位（CSS 里 normal = 0）
                } else {
                    float px = parseLen(lsv, 0, "letter-spacing");
                    // em/rem 的换算基准是字号：parseLen 只认 px 一类，em 得自己折
                    if (lsl.size() > 2 && lsl.compare(lsl.size() - 2, 2, "em") == 0 &&
                        lsl.compare(lsl.size() - 3, 3, "rem") != 0) {
                        px = parseLen(lsv.substr(0, lsl.size() - 2), 0, "letter-spacing") * nodeFontSize;
                    }
                    if (std::isfinite(px)) nodeLetterSpacing = px;
                }
            }
        }

        // 文本节点未声明高度时估算行数并撑开高度。
        // 没有这一步，段落按单行高排布：换行后的第二行会压到下一个元素身上
        // （三个后端都按节点矩形绘制，谁也没办法把节点撑高）。估算口径统一在
        // include/binui_text_estimate.h —— 渲染器运行期 setText 重算高度用的必须是同一份，
        // 否则"编译期刚好放下、换成长文本就溢出"（issue 06）。
        //   · CJK/全角按 1.0×字号，其余按 0.52×字号（拉丁小写平均宽度），宁可高一点
        //   · 行距取 line-height（未声明则 1.35×字号）
        //   · 显式写了 height 的不动（作者说了算）
        // heightAuto：高度是估算撑开的就标 1（即使这次估算没撑高也要标 —— 运行期换成
        // 更长的文本同样需要重算），写进缩放规格段，供 renderer.setText 判断。
        bool heightAuto = false;
        uint16_t autoBase = 0;   // 估算之前的高度（元素默认盒高），运行期重算的基线
        if (sh.empty() && !srcText.empty() &&
            (type == BINUI_LABEL || type == BINUI_LIST || type == BINUI_BUTTON)) {
            heightAuto = true;
            autoBase = static_cast<uint16_t>(std::lround(lh));
            const float step = estLineStep(style, nodeFontSize);
            float avail = pc.contentW;
            if (!sw.empty()) {   // sw 已在上面的几何区里解析过（'auto' 被清空）
                auto sp = ParseLenPct(sw, 0, curCtx, "width");
                avail = sp.pct ? sp.value / 100.0f * pc.contentW : sp.value;
            }
            avail -= type == BINUI_LIST ? 24.0f : 0.0f;   // LIST 文字左右各有 12px 内缩
            const int lines = EstimateLineCount(srcText, nodeFontSize, avail, nodeLetterSpacing); // 至少 1 行，'\n' 硬断
            const float needH = (float)lines * step;
            if (needH > lh) lh = needH;
        }

        // SVG 图形属性 -> 通用定位：<line> 用 x1/y1/x2/y2 端点，<circle> 用 cx/cy/r
        // 端点在绘制后端按"起点 + 相对偏移"还原（见节点生成处的 BINUI_LINE 覆盖）
        float svgR = -1, svgLineX1 = 0, svgLineY1 = 0, svgLineX2 = -1, svgLineY2 = -1;
        if (hasSvgPath) {
            // <path>/<ellipse>，以及带描边的 <circle>/<rect>：几何包围盒（含描边外扩）当节点盒
            sx = std::to_string(svgPathShape.minX);
            sy = std::to_string(svgPathShape.minY);
            sw = std::to_string(svgPathShape.Width());
            sh = std::to_string(svgPathShape.Height());
        } else if (e.tag == "line") {
            float x1 = parseLen(e.attrs.count("x1") ? e.attrs.at("x1") : "", 0);
            float y1 = parseLen(e.attrs.count("y1") ? e.attrs.at("y1") : "", 0);
            float x2 = parseLen(e.attrs.count("x2") ? e.attrs.at("x2") : "", 0);
            float y2 = parseLen(e.attrs.count("y2") ? e.attrs.at("y2") : "", 0);
            // 端点走 SVG 变换（viewBox / <g transform>）
            const PathPointF p1 = PathApply(svgX, x1, y1);
            const PathPointF p2 = PathApply(svgX, x2, y2);
            x1 = p1.x; y1 = p1.y; x2 = p2.x; y2 = p2.y;
            sx = std::to_string(x1); sy = std::to_string(y1);
            sw = std::to_string(x2); sh = std::to_string(y2);
            svgLineX1 = x1; svgLineY1 = y1;
            svgLineX2 = x2; svgLineY2 = y2;
        } else if (e.tag == "circle") {
            float cx = parseLen(e.attrs.count("cx") ? e.attrs.at("cx") : "", 0);
            float cy = parseLen(e.attrs.count("cy") ? e.attrs.at("cy") : "", 0);
            svgR = parseLen(e.attrs.count("r") ? e.attrs.at("r") : "", 0) * PathScaleFactor(svgX);
            const PathPointF cp = PathApply(svgX, cx, cy);
            cx = cp.x; cy = cp.y;
            sx = std::to_string(cx - svgR); sy = std::to_string(cy - svgR);
            sw = std::to_string(2 * svgR); sh = std::to_string(2 * svgR);

        } else if (e.tag == "rect" && inSvg) {
            // 矩形两角映射后当"声明值"用，下游流式/百分比/规格段全部一致
            const float rx0 = parseLen(sx, 0), ry0 = parseLen(sy, 0);
            const float rw0 = parseLen(sw, 0), rh0 = parseLen(sh, 0);
            const PathPointF p0 = PathApply(svgX, rx0, ry0);
            const PathPointF p1 = PathApply(svgX, rx0 + rw0, ry0 + rh0);
            sx = std::to_string(std::min(p0.x, p1.x));
            sy = std::to_string(std::min(p0.y, p1.y));
            sw = std::to_string(std::fabs(p1.x - p0.x));
            sh = std::to_string(std::fabs(p1.y - p0.y));
        } else if (e.tag == "g" && inSvg) {
            // <g> 在 SVG 里不建立坐标系、也不是布局盒子：给它一个零尺寸的绝对定位节点，
            // 子元素直接挂在父容器原点（见下面 push 处对 <g> 的 ox/oy 处理）
            sx = "0"; sy = "0"; sw = "0"; sh = "0";
        } else if (e.tag == "text" && inSvg) {
            // <text x y font-size fill>：编成普通 LABEL（绝对定位）。
            // SVG 的 y 是**基线**，binUI 按盒顶排版，所以上移 0.8 个字号近似；
            // 没有富文本能力，<tspan> 仍然不支持（会单独告警）。
            const float tx = SvgLenPlain(e.attrs.count("x") ? e.attrs.at("x") : "", 0.0f);
            const float ty = SvgLenPlain(e.attrs.count("y") ? e.attrs.at("y") : "", 0.0f);
            const float fs = SvgLenPlain((e.attrs.count("font-size") ? e.attrs.at("font-size")
                                          : (style.count("font-size") ? style.at("font-size") : "")),
                                         14.0f);
            const PathPointF tp = PathApply(svgX, tx, ty);
            nodeFontSize = fs;   // font-size 是 SVG 属性名，走 pick("size") 取不到，这里补上
            // text-anchor：SVG 的 x 是"锚点"，默认锚在文本左端。binUI 的 LABEL 从盒左边画起，
            // 所以 middle/end 要在编译期把盒子左边左移（盒宽用与运行期同一口径的字宽估算）。
            std::string ta = style.count("text-anchor") ? style.at("text-anchor")
                            : (e.attrs.count("text-anchor") ? e.attrs.at("text-anchor") : "");
            ta = lowerStr(ta);
            const float estW = estTextWidth(srcText, fs) * PathScaleFactor(svgX);
            sx = std::to_string(tp.x);
            sy = std::to_string(tp.y - fs * 0.8f * PathScaleFactor(svgX));
            if (ta == "middle") {
                sx = std::to_string(tp.x - estW * 0.5f);
            } else if (ta == "end") {
                sx = std::to_string(tp.x - estW);
            } else if (!ta.empty() && ta != "start") {
                Warn("text-anchor=\"" + ta + "\" 不认识，按 start 处理"
                     "（可选 start / middle / end）");
            }
            if (sw.empty()) {
                if (ta == "middle" || ta == "end") {
                    // 居中/右对齐的盒子必须按估算宽度收窄，否则"撑到容器右边"会把锚点带偏
                    sw = std::to_string(std::max(4.0f, estW + 2.0f));
                } else {
                    const float avail = pc.contentW - tp.x;
                    sw = std::to_string(avail > 4.0f ? avail : 4.0f);
                }
            }
            sh = std::to_string(fs * 1.35f * PathScaleFactor(svgX) + 2.0f);
        } else if (e.tag == "image" && inSvg) {
            // <image x y width height href>：编成普通 IMAGE（资源路径相对 UI 文件）
            const float ix = SvgLenPlain(e.attrs.count("x") ? e.attrs.at("x") : "", 0.0f);
            const float iy = SvgLenPlain(e.attrs.count("y") ? e.attrs.at("y") : "", 0.0f);
            const float iw = SvgLenPlain(e.attrs.count("width") ? e.attrs.at("width") : "", 0.0f);
            const float ih = SvgLenPlain(e.attrs.count("height") ? e.attrs.at("height") : "", 0.0f);
            const float k = PathScaleFactor(svgX);
            const PathPointF ip = PathApply(svgX, ix, iy);
            sx = std::to_string(ip.x);
            sy = std::to_string(ip.y);
            if (sw.empty()) sw = std::to_string(iw * k);
            if (sh.empty()) sh = std::to_string(ih * k);
        }

        // <br>：换行占位——块式布局下每个元素已独占一行，br 只占零高度
        if (e.tag == "br") lh = 0;

        // 复选框/单选框：没声明宽高时是 18×18 的小方块（以前是全宽 40px 高文本框）
        if (isCheckbox) {
            if (sw.empty()) sw = "18";
            if (sh.empty()) sh = "18";
        }

        // ---- position（必须在解析 left/top 之前读出来）----
        // 为什么前移：sticky 的 `top` 是"粘滞偏移"，不是定位坐标 —— 若先按 left/top 解析，
        // 这个节点会被当成"显式定位"（hasY = true）脱离文档流，sticky 就变成了 absolute。
        std::string pos = lowerStr(style.count("position") ? style.at("position") : "");
        bool isAbs = (pos == "absolute" || pos == "fixed");
        // v30：`position: fixed` 是**真的钉在窗口上**，不是"贴父容器"：
        // 含定位盒换成视口（根节点内容盒），left/top/right/bottom/百分比全按视口解析，
        // 绘制不吃祖先滚动偏移、不被祖先 overflow 裁剪、层级提到根层级（见 ANCHOR_FIXED）。
        // 下面所有位置换算统一走 cb（= fixed 时的视口上下文），口径与渲染器重排一致。
        const bool isFixed = (pos == "fixed");
        const Ctx& cb = isFixed ? stack.front() : pc;
        // v0.16.39：`position: sticky` —— 留在文档流里，绘制时按滚动视口钳制（见 ANCHOR_STICKY）。
        // top 偏移单独接住（不写进 ly/hasY），left 只支持横向的粘滞，其余边明确告警。
        const bool isSticky = (pos == "sticky");
        float stickyTopPx = 0.0f, stickyTopPct = -1.0f;
        bool  hasStickyTop = false;
        if (isSticky) {
            if (!sy.empty()) {
                auto p = ParseLenPct(sy, 0, curCtx, "top");
                stickyTopPx = p.value;
                if (p.pct) stickyTopPct = p.value / 100.0f;
                hasStickyTop = true;
            }
            if (!hasStickyTop)
                Warn("position:sticky 没有写 top：没有钉住的阈值，等价于 static（请写 top:0 之类）");
        }

        if (!sx.empty() && !isSticky) { auto p = ParseLenPct(sx, 0, curCtx, "left"); lx = p.value; if (p.pct) pctX = p.value / 100.0f; hasX = true; }
        if (!sy.empty() && !isSticky) { auto p = ParseLenPct(sy, 0, curCtx, "top"); ly = p.value; if (p.pct) pctY = p.value / 100.0f; hasY = true; }
        // v30：贴右 / 贴下（CSS right / bottom / inset）。位置 = 父内容盒宽高 − 偏移 − 自身尺寸，
        // 而自身尺寸要到下面才算得出来，所以这里只解析偏移，落位在 w/lh 定下来之后。
        const std::string sr = pick(e.attrs, style, "right", "right", "");
        const std::string sb = pick(e.attrs, style, "bottom", "bottom", "");
        float rightPx = -1.0f, bottomPx = -1.0f, pctR = -1.0f, pctB = -1.0f;
        if (!sr.empty()) { auto p = ParseLenPct(sr, 0, curCtx, "right"); rightPx = p.value; if (p.pct) pctR = p.value / 100.0f; }
        if (!sb.empty()) { auto p = ParseLenPct(sb, 0, curCtx, "bottom"); bottomPx = p.value; if (p.pct) pctB = p.value / 100.0f; }
        // sticky 只做纵向：其它三边明确告警（静默忽略会让作者以为钉住了）
        if (isSticky && (!sx.empty() || !sr.empty() || !sb.empty()))
            Warn("position:sticky 目前只支持纵向的 top（left/right/bottom 已忽略）");
        // inset：四值简写（1/2/4 个值都按 CSS 的简写规则展开），等价于 top/right/bottom/left。
        // 只展开作者**没单独写**的边（长属性优先，与 CSS 层叠一致）。
        if (style.count("inset") || style.count("inset-top")) {
            const auto ins = parseBox(style, "inset", "inset-top", "inset-right",
                                      "inset-bottom", "inset-left", -1.0f);
            if (ins[0] >= 0 && !hasY && !isSticky) { ly = ins[0]; hasY = true; }
            if (ins[2] >= 0 && sb.empty()) { bottomPx = ins[2]; pctB = -1.0f; }
            if (ins[3] >= 0 && !hasX && !isSticky) { lx = ins[3]; hasX = true; }
            if (ins[1] >= 0 && sr.empty()) { rightPx = ins[1]; pctR = -1.0f; }
        }
        if (!sw.empty()) { auto p = ParseLenPct(sw, 0, curCtx, "width"); if (p.pct) pctW = p.value / 100.0f; hasW = true; }
        if (!sh.empty()) { auto p = ParseLenPct(sh, 0, curCtx, "height"); lh = p.value; if (p.pct) pctH = p.value / 100.0f; }

        // 尺寸约束：min/max-width/height（支持 px 与 %；'auto'/空 = 未指定，值 -1）
        float mnW = -1, mxW = -1, mnH = -1, mxH = -1;
        uint16_t mnWp = 0, mxWp = 0, mnHp = 0, mxHp = 0;
        auto parseCon = [&](const std::string& key, float& out, uint16_t& pctOut) {
            std::string v = pick(e.attrs, style, key, key, "");
            if (lowerStr(v) == "auto") v.clear();
            if (v.empty()) { out = -1; return; }
            auto p = ParseLenPct(v, 0, curCtx, key.c_str());
            if (p.pct) { out = p.value / 100.0f; pctOut = 1; }
            else out = p.value;
        };
        parseCon("min-width",  mnW, mnWp);
        parseCon("max-width",  mxW, mxWp);
        parseCon("min-height", mnH, mnHp);
        parseCon("max-height", mxH, mxHp);

        // position:absolute/fixed 且未显式 left/top：视为 (0,0)（脱离文档流 + 绘制在最上层）
        // 作者到底写没写 left/top —— 必须在这两行"绝对定位补 0"之前记下来，
        // 否则 right/bottom 会被误判成"同时写了 left/right"。
        const bool declaredX = hasX, declaredY = hasY;
        if (isAbs) {
            if (!hasX) { lx = 0; hasX = true; }
            if (!hasY) { ly = 0; hasY = true; }
        }
        // v30：贴右/贴下只在"脱离文档流"或已经写了 left/top 的节点上生效（CSS 的 static
        // 元素本来就不认 right/bottom）。同时写 left 和 right 时按 left 定位 —— 定宽元素
        // 在 CSS 里也是 left 赢，这里明确告警而不是静默二选一。
        const bool outOfFlow = isAbs || hasX || hasY;
        bool useRight = false, useBottom = false;
        if (rightPx >= 0 || pctR >= 0) {
            if (!outOfFlow && !hasX) {
                Warn("CSS right 只对 position:absolute/fixed 生效，本节点没有定位，按未声明处理");
                rightPx = -1.0f; pctR = -1.0f;
            } else if (declaredX) {
                Warn("同时写了 left 和 right：按 left 定位（CSS 在元素有确定宽度时也是 left 赢）");
                rightPx = -1.0f; pctR = -1.0f;
            } else {
                useRight = true;
                // right 定位的节点仍然"有 x"，只是 x 要在宽度算出来之后才能定 ——
                // 这里把 hasX 置上（脱离流式），lx 稍后覆盖。
                hasX = true;
            }
        }
        if (bottomPx >= 0 || pctB >= 0) {
            if (!outOfFlow && !hasY) {
                Warn("CSS bottom 只对 position:absolute/fixed 生效，本节点没有定位，按未声明处理");
                bottomPx = -1.0f; pctB = -1.0f;
            } else if (declaredY) {
                Warn("同时写了 top 和 bottom：按 top 定位（CSS 在元素有确定高度时也是 top 赢）");
                bottomPx = -1.0f; pctB = -1.0f;
            } else {
                useBottom = true;
                hasY = true;
            }
        }

        // margin 仅在流式布局中生效
        float mt = 0, mr = 0, mb = 0, ml = 0;
        if (!hasY) {
            auto m = parseBox(style, "margin",
                              "margin-top", "margin-right", "margin-bottom", "margin-left", 0);
            mt = m[0]; mr = m[1]; mb = m[2]; ml = m[3];
        }

        // 百分比：相对**含定位盒**内容盒解析（width 相对内容宽，height/left/top 相对内容高）。
        // 普通元素 = 父容器内容盒；position:fixed = 视口（cb，见上面的 isFixed）。
        float pcContentH = 0;
        {
            const BinUINode& pcn = nodes[cb.nodeIdx];
            pcContentH = pcn.height - 2.0f * pcn.padding;
            if (pcContentH < 0) pcContentH = 0;
        }
        if (pctX >= 0) lx = pctX * cb.contentW;
        if (pctY >= 0) ly = pctY * pcContentH;
        if (pctH >= 0) lh = pctH * pcContentH;

        // 尺寸约束按含定位盒内容解析（% 时）后的实际像素值；-1 = 未指定
        float effMnW = mnW >= 0 ? (mnWp ? mnW * cb.contentW : mnW) : -1;
        float effMxW = mxW >= 0 ? (mxWp ? mxW * cb.contentW : mxW) : -1;
        float effMnH = mnH >= 0 ? (mnHp ? mnH * pcContentH : mnH) : -1;
        float effMxH = mxH >= 0 ? (mxHp ? mxH * pcContentH : mxH) : -1;

        float x, y, w;
        bool flexPlaced = false;
        float baseW = 0, baseH = lh; // 显式声明值（未钳制）：运行时重排基准
        // position:fixed 不参与父级的 flex 排布（CSS：脱离流 = 不是 flex 项）。
        // 注意 layoutFlex 那边也已经把它从子项里滤掉了，这里再挡一次是防"父级是 flex
        // 但 fixed 子项恰好落在 flexLayouts 里"的旧产物。
        auto flIt = isFixed ? flexLayouts.end() : flexLayouts.find(e.parentIdx);
        if (flIt != flexLayouts.end()) {
            auto pIt = flIt->second.find(ei);
            if (pIt != flIt->second.end()) {
                const float* p = pIt->second;
                x = cb.ox + p[0]; y = cb.oy + p[1]; w = p[2]; lh = p[3];
                flexPlaced = true;
            }
        }
        if (!flexPlaced) {
            baseH = lh;
            // 高度先钳制再推进流光标（约束高度影响流式堆叠位置）
            if (effMnH >= 0 && lh < effMnH) lh = effMnH;
            if (effMxH >= 0 && lh > effMxH) lh = effMxH;
            // 宽度要先算出来：贴右定位的 x 依赖它
            if (hasW) w = (pctW >= 0) ? pctW * cb.contentW : parseLen(sw, 0);
            else      w = cb.contentW - (ml + mr > 0 ? (ml + mr) : 0);
            baseW = w;
            if (effMnW >= 0 && w < effMnW) w = effMnW;
            if (effMxW >= 0 && w > effMxW) w = effMxW;
            if (useRight) {
                // v30：x = 含定位盒右边 − 偏移 − 自身宽（与渲染器重排同一个式子）
                const float r = (pctR >= 0) ? pctR * cb.contentW : rightPx;
                x = cb.ox + cb.contentW - r - w;
                lx = x - cb.ox;
            } else if (hasX) {
                x = cb.ox + lx;
            } else {
                x = cb.ox + ml;
            }
            if (useBottom) {
                const float b = (pctB >= 0) ? pctB * pcContentH : bottomPx;
                y = cb.oy + pcContentH - b - lh;
                ly = y - cb.oy;
            } else if (hasY) {
                y = cb.oy + ly;
            } else {
                y = pc.oy + pc.cursorY + mt;
                pc.cursorY += mt + lh + mb;
            }
        }

        // padding：节点只有一个 padding 字段（内缩 + 子元素基准都用它），
        // 四值简写只取首值；四边确实不同时明确告警，别让作者以为生效了。
        uint16_t pad = 0;
        auto padIt = style.find("padding");
        if (padIt != style.end()) {
            const auto toks = splitCssTokens(padIt->second);
            if (!toks.empty()) pad = (uint16_t)parseLen(toks[0], 0);
            if (toks.size() > 1) {
                bool differ = false;
                for (const auto& t : toks)
                    if (parseLen(t, 0) != (float)pad) differ = true;
                if (differ)
                    Warn("CSS padding 四值简写只取第一个值（节点只有单一 padding 字段）；"
                         "需要四边不等请用嵌套容器");
            }
        } else {
            auto padT = style.find("padding-top");
            if (padT != style.end()) pad = (uint16_t)parseLen(padT->second, 0);
        }

        BinUINode n = {};
        n.id = id++;
        hIdxToNodeId[ei] = n.id;
        n.parentId = pnodeId;
        // 组件占位节点：记下锚点 id（编译期离线合并 / 运行期挂载都按它对接）
        if (auto lk = e.attrs.find("data-binui-link"); lk != e.attrs.end())
            tl_pendingAnchor[std::atoi(lk->second.c_str())] = n.id;
        n.type = type;
        n.flags = BINUI_VISIBLE;
        // HTML 模式下声明 left/top 或 position:absolute/fixed 的节点标记 ABSOLUTE
        // 绘制时位于流式内容之上（CSS 绘制顺序：positioned 元素在 static 之后）
        if (hasX || hasY || isAbs)
            n.flags |= BINUI_ABSOLUTE;
        // 自绘标题栏：非交互子区域用于拖动窗口（HTML 下置于最上层）
        if (e.tag == "titlebar") {
            n.flags |= BINUI_TITLEBAR;
            if (!hasX) n.flags |= BINUI_ABSOLUTE;
        }
        if (type == BINUI_BUTTON || type == BINUI_INPUT || type == BINUI_VIDEO)
            n.flags |= BINUI_INTERACTIVE;
        // 复选框/单选框占位、带 href 的链接：都该能被点（CLICK 事件 + :hover/:focus）
        if (isCheckbox) n.flags |= BINUI_INTERACTIVE;
        if (e.tag == "a" && e.attrs.count("href")) n.flags |= BINUI_INTERACTIVE;
        // v14 禁用：不进焦点链、不响应悬停/点击/编辑，后端按 55% 淡绘制
        {
            auto boolAttr = [&](const char* k) -> bool {
                auto it2 = e.attrs.find(k);
                if (it2 == e.attrs.end()) return false;
                const std::string v = lowerStr(it2->second);
                return v != "false" && v != "0" && v != "off";
            };
            if (boolAttr("disabled") || boolAttr("aria-disabled"))
                n.flags |= BINUI_DISABLED;
        }
        // overflow：auto/scroll = 可滚动容器；hidden = 只裁剪（不给滚动条，也不响应滚轮）。
        // 两者都要标 SCROLLABLE —— 后端按它决定"高度固定、内容可溢出"，绘制时按祖先矩形裁剪。
        std::string ov = lowerStr(pick(e.attrs, style, "overflow", "overflow", ""));
        const bool clipOnly = (ov == "hidden");
        if (ov == "auto" || ov == "scroll" || clipOnly) {
            n.flags |= BINUI_SCROLLABLE;
            if (!clipOnly) n.flags |= BINUI_INTERACTIVE;
        }
        // text-align：借 flags 的两个空闲位编码（不新增节点字段，旧布局两位为 0 = 左对齐）
        {
            std::string ta = lowerStr(pick(e.attrs, style, "align", "text-align", ""));
            if (ta == "center") n.flags |= BINUI_TEXT_CENTER;
            else if (ta == "right" || ta == "end") n.flags |= BINUI_TEXT_RIGHT;
            // <button> 的 UA 默认值是居中（浏览器如此）。后端以前把按钮文字写死居中，
            // 所以这份"默认"一直没人写；现在后端改成读 flags 了，必须在这里补上，
            // 否则所有没写 text-align 的按钮会突然变左对齐（演示全都要改观感）。
            else if (type == BINUI_BUTTON && ta.empty()) n.flags |= BINUI_TEXT_CENTER;
        }

        // 嵌套 <svg>：它的 x/y/width/height 是**父用户坐标**，而普通元素的 left/top 是容器像素。
        // 祖先有 transform 时（例如 `<g transform="scale(2)">` 包一个 <svg>），不折算就会出现
        // "子图形按变换画、自己的盒子还在原位"—— 盒子把内容全裁掉（踩过：嵌套 svg 整块空白）。
        // 这里把矩形四个角过一遍父变换取包围盒；旋转时用轴对齐包围盒（子图形仍按真变换画）。
        if (e.tag == "svg" && pc.inSvg) {
            const float ux0 = x - pc.ox, uy0 = y - pc.oy;
            const PathPointF c0 = PathApply(svgX, ux0, uy0);
            const PathPointF c1 = PathApply(svgX, ux0 + w, uy0);
            const PathPointF c2 = PathApply(svgX, ux0, uy0 + lh);
            const PathPointF c3 = PathApply(svgX, ux0 + w, uy0 + lh);
            const float mnx = std::min(std::min(c0.x, c1.x), std::min(c2.x, c3.x));
            const float mny = std::min(std::min(c0.y, c1.y), std::min(c2.y, c3.y));
            const float mxx = std::max(std::max(c0.x, c1.x), std::max(c2.x, c3.x));
            const float mxy = std::max(std::max(c0.y, c1.y), std::max(c2.y, c3.y));
            x = pc.ox + mnx;
            y = pc.oy + mny;
            w = std::max(0.0f, mxx - mnx);
            lh = std::max(0.0f, mxy - mny);
        }

        // 静态 opacity 在颜色赋值之后处理（见下方），这里先落几何
        n.x = x; n.y = y; n.width = w; n.height = lh;
        n.padding = pad;

        // ---- 底色 / 文字色（v13 正式分家）----
        //   底色   = XML bg → CSS background → CSS background-color；XML 传统用 color 指容器底色
        //   文字色 = XML text-color → CSS color；XML 传统用 color 指文本颜色（仅 LABEL）
        // LABEL 且没写底色时沿用旧约定：node.color 存文字色（旧二进制/旧后端行为不变）；
        // 一旦写了底色，文字色就改走扩展绘制段的 textColor。
        std::string bgRaw = pick(e.attrs, style, "bg", "background", "");
        if (bgRaw.empty() && style.count("background-color")) bgRaw = style.at("background-color");
        const std::string xmlColor = e.attrs.count("color") ? e.attrs.at("color") : "";
        if (bgRaw.empty() && type != BINUI_LABEL) bgRaw = xmlColor;
        std::string txtRaw = pick(e.attrs, style, "text-color", "color", "");
        if (txtRaw.empty() && type == BINUI_LABEL) txtRaw = xmlColor;

        // 渐变：background / background-image 里的 linear-gradient()
        BinUIPaintNode paint = {};
        uint16_t pflags = 0;
        GradSpec gs = parseLinearGradient(bgRaw);
        if (!gs.ok && style.count("background-image")) gs = parseLinearGradient(style.at("background-image"));

        // 没写底色的默认值：v13 起容器默认透明（以前是 #222222，浅色主题直接被吃掉），
        // 但当时漏了 BUTTON/INPUT 这一档 —— 浅底页面上"不写 background 的按钮"就是一坨黑块。
        // 现在统一成透明：没写就是透明，要底色请显式声明 background（示例与组件库都写了）。
        uint32_t defBg = (type == BINUI_LABEL)  ? 0xFFFFFFFFu
                       : (type == BINUI_SVG || type == BINUI_CANVAS) ? 0x00000000u
                       : (type == BINUI_VIDEO) ? 0xFF101010u
                       : 0x00000000u;   // 容器 / BUTTON / INPUT 默认透明
        if (bgRaw.empty()) n.color = defBg;
        else if (gs.ok) n.color = gs.from;   // 渐变：底色兜底用起点色（免得被当颜色解析刷警告）
        else n.color = parseColorWith(bgRaw, defBg, "背景色");
        if (gs.ok) {
            paint.gradFrom = gs.from; paint.gradTo = gs.to; paint.gradDir = gs.dir;
            // v0.16.13：任意角度写进 reserve[1]（角度+1，0 = 没声明 → 走老三方向）。
            // PAINT_* 的 16 位已经用满（见 PAINT_OPACITY 的先例），而旧布局这一位恒为 0，
            // 行为与以前完全一致，所以不升 BINUI_VERSION。
            if (gs.angle > 0) paint.reserve[1] = static_cast<uint8_t>(gs.angle + 1);
            pflags |= PAINT_GRADIENT;
            n.color = gs.from;   // 不支持渐变的后端/旧布局退回起点色，不至于变黑
        }

        // 文字色：显式声明 > 从父容器继承 > 默认色（按根底色亮度自适应）
        uint32_t textCol = defaultTextCol;
        bool hasOwnText = false;
        if (!txtRaw.empty()) { textCol = parseColorWith(txtRaw, defaultTextCol, "文字颜色"); hasOwnText = true; }
        else if (pc.hasText) { textCol = pc.textColor; }
        const bool labelLegacy = (type == BINUI_LABEL && bgRaw.empty());
        if (labelLegacy) {
            n.color = textCol;               // 旧约定：LABEL 的 color 就是文字色
        } else if (type == BINUI_LABEL) {
            paint.textColor = textCol;       // 有底色 → 文字色必须显式给（否则后端会用底色画字）
            pflags |= PAINT_TEXT_COLOR;
        } else if (textCol != defaultTextCol) {
            paint.textColor = textCol;
            pflags |= PAINT_TEXT_COLOR;
        }
        (void)hasOwnText;

        // 边框（画在节点内侧，不参与布局；四边可不同宽）
        {
            BorderSpec bs = parseBorderSpec(e, style);
            if (bs.any) {
                for (int k = 0; k < 4; k++) {
                    paint.borderW[k] = bs.w[k];
                    paint.borderColor[k] = bs.color[k];
                }
                pflags |= PAINT_BORDER;
                if (bs.w[0]) pflags |= PAINT_BORDER_TOP;
                if (bs.w[1]) pflags |= PAINT_BORDER_RIGHT;
                if (bs.w[2]) pflags |= PAINT_BORDER_BOTTOM;
                if (bs.w[3]) pflags |= PAINT_BORDER_LEFT;
            }
        }

        // 字重 / 行高 / 文字装饰
        {
            std::string fw = lowerStr(pick(e.attrs, style, "weight", "font-weight", ""));
            if (!fw.empty() && fw != "normal" && fw != "400") {
                uint8_t w8 = 4;   // 归一化成"百位"：4 = 常规，7 = 粗体
                if (fw == "bold" || fw == "bolder" || fw == "700") w8 = 7;
                else {
                    try {
                        const int num = std::stoi(fw);
                        w8 = (num >= 600) ? 7 : 4;
                    } catch (...) {
                        Warn("CSS font-weight: 无法解析 \"" + fw + "\"，按 normal 处理");
                    }
                }
                if (w8 != 4) { paint.weight = w8; pflags |= PAINT_WEIGHT; }
            }
            std::string lhv = TrimCopy(pick(e.attrs, style, "line-height", "line-height", ""));
            if (!lhv.empty() && lowerStr(lhv) != "normal") {
                int pct = 0;
                if (lhv.back() == '%') {
                    pct = (int)std::lround(parseLen(lhv.substr(0, lhv.size() - 1), 0, "line-height"));
                } else if (lhv.find_first_of("0123456789") == 0) {
                    const float num = parseLen(lhv, 0, "line-height");
                    // 无单位 = 倍数（CSS 语义），带 px = 绝对行高
                    const bool unitless = lhv.find_first_not_of("0123456789.+-") == std::string::npos;
                    pct = unitless ? (int)std::lround(num * 100.0f)
                                   : (nodeFontSize > 0.5f ? (int)std::lround(num / nodeFontSize * 100.0f) : 0);
                }
                if (pct > 0) {
                    if (pct < 100) pct = 100;
                    if (pct > 255) pct = 255;
                    paint.lineHeight = (uint8_t)pct;
                    pflags |= PAINT_LINE_HEIGHT;
                }
            }
            // v0.16.40：字间距落进 reserve[0]（int8 / ¼ 像素），与上面的生效值同一个来源
            paint.letterSpacing = nodeLetterSpacing;
            std::string td = pick(e.attrs, style, "decoration", "text-decoration", "");
            if (td.empty() && style.count("text-decoration-line")) td = style.at("text-decoration-line");
            const std::string tdl = lowerStr(td);
            if (tdl.find("underline") != std::string::npos)    pflags |= PAINT_UNDERLINE;
            if (tdl.find("line-through") != std::string::npos) pflags |= PAINT_STRIKE;
            if (tdl.find("overline") != std::string::npos)     pflags |= PAINT_OVERLINE;
            // 装饰线颜色跟随文字色；LABEL 走旧约定时 textColor 没置位，
            // 这里显式填上（否则下划线会画成透明，看不见）
            if (pflags & (PAINT_UNDERLINE | PAINT_STRIKE | PAINT_OVERLINE))
                paint.textColor = textCol;
        }

        // v0.16.41：text-overflow: ellipsis（单行截断 + 省略号）
        {
            const std::string tov = lowerStr(TrimCopy(
                pick(e.attrs, style, "text-overflow", "text-overflow", "")));
            if (tov == "ellipsis") {
                paint.textFlags |= TEXT_ELLIPSIS;
                // CSS 里 text-overflow 只对**单行**生效（配合 white-space:nowrap +
                // overflow:hidden）。多行文本（含 '\n' 或会折行）在这里明确告警，
                // 免得作者以为"每行都会被省略号收尾"。
                if (srcText.find('\n') != std::string::npos)
                    Warn("text-overflow:ellipsis 只对单行文本生效；这段文本含换行，已按普通多行绘制");
            } else if (!tov.empty() && tov != "clip" && tov != "auto") {
                Warn("CSS text-overflow: \"" + tov + "\" 暂不支持（只实现了 ellipsis），已忽略");
            }
        }

        // v14 文字/表单附加标记（斜体 / 占位符 / 只读 / 初始勾选）
        {
            const std::string fst = lowerStr(pick(e.attrs, style, "font-style", "font-style", ""));
            if (fst == "italic" || fst == "oblique") {
                paint.textFlags |= TEXT_ITALIC;
            } else if (!fst.empty() && fst != "normal") {
                Warn("CSS font-style 只支持 normal/italic，\"" + fst + "\" 按 normal 处理");
            }
            if (inputPlaceholder) paint.textFlags |= TEXT_PLACEHOLDER;
            if (isCheckbox && e.attrs.count("checked")) {
                const std::string cv = lowerStr(e.attrs.at("checked"));
                if (cv != "false" && cv != "0" && cv != "off") paint.textFlags |= TEXT_CHECKED;
            }
            if ((type == BINUI_INPUT || type == BINUI_BUTTON) && e.attrs.count("readonly")) {
                const std::string rv = lowerStr(e.attrs.at("readonly"));
                if (rv != "false" && rv != "0" && rv != "off") paint.textFlags |= TEXT_READONLY;
            }
        }

        if (clipOnly) pflags |= PAINT_CLIP_ONLY;

        // v21 resize：把鼠标贴到这条边/这个角上就能拖动改盒子尺寸（浏览器 `resize` 的
        // 加强版 —— 浏览器的 resize 只认 both/horizontal/vertical，这里按边/角分别开放）。
        // 取值：none | both|all | horizontal | vertical | left right top bottom |
        //       top-left top-right bottom-left bottom-right，也接受 `<名字>-resize` 拼法
        //       （left-resize / top-left-resize）。只写两个角时，两条边的中间段不响应。
        {
            std::string rv = lowerStr(pick(e.attrs, style, "resize", "resize", ""));
            if (!rv.empty() && rv != "none") {
                uint8_t mask = 0;
                bool bad = false;
                std::string norm;
                for (char ch : rv) norm.push_back(ch == ',' ? ' ' : ch);   // 逗号当分隔符
                for (const std::string& rawTok : splitCssTokens(norm)) {
                    std::string tok = rawTok;
                    if (tok.size() > 7 && tok.compare(tok.size() - 7, 7, "-resize") == 0)
                        tok.resize(tok.size() - 7);                        // left-resize → left
                    if (tok == "both" || tok == "all")          mask |= RESIZE_ALL;
                    else if (tok == "horizontal")                mask |= RESIZE_LEFT | RESIZE_RIGHT;
                    else if (tok == "vertical")                  mask |= RESIZE_TOP | RESIZE_BOTTOM;
                    else if (tok == "left")                      mask |= RESIZE_LEFT;
                    else if (tok == "right")                     mask |= RESIZE_RIGHT;
                    else if (tok == "top")                       mask |= RESIZE_TOP;
                    else if (tok == "bottom")                    mask |= RESIZE_BOTTOM;
                    else if (tok == "top-left" || tok == "left-top")       mask |= RESIZE_TOP_LEFT;
                    else if (tok == "top-right" || tok == "right-top")     mask |= RESIZE_TOP_RIGHT;
                    else if (tok == "bottom-left" || tok == "left-bottom") mask |= RESIZE_BOTTOM_LEFT;
                    else if (tok == "bottom-right" || tok == "right-bottom") mask |= RESIZE_BOTTOM_RIGHT;
                    else bad = true;
                }
                if (bad)
                    Warn("CSS resize 的取值只支持 none/both|all/horizontal/vertical/"
                         "left/right/top/bottom/top-left/top-right/bottom-left/bottom-right"
                         "（也可写 <名字>-resize）：\"" + rv + "\"");
                if (mask) {
                    paint.resizeMask = mask;
                    pflags |= PAINT_RESIZE;
                }
            }
        }

        // 静态 opacity（0.16.8）：**不再折进颜色**，改成存进扩展绘制段的 reserve[0]，
        // 由渲染器按祖先链级联（CSS 的组透明度语义）—— 以前折进颜色等于"子节点完全不受影响"：
        // 一个卡片 fade 时背景在淡、文字和子块纹丝不动（docs/future.md P1-7）。
        // 动画里的 opacity 仍走 v7 动画系统；两者在 drawState() 里相乘。
        {
            auto oit = style.find("opacity");
            if (oit != style.end()) {
                float op = parseLen(oit->second, 1.0f, "opacity");
                if (op < 0.0f) op = 0.0f;
                if (op > 1.0f) op = 1.0f;
                if (op < 1.0f) {
                    pflags |= PAINT_OPACITY;
                    paint.reserve[0] = (uint8_t)(op * 255.0f + 0.5f);
                }
            }
        }

        // ---- v0.16.15 / v26：CSS filter ----
        // 三条落点，按"能不能用顶点色乘出来 / 要不要离屏合成"分工：
        //   · brightness() → 乘一个灰度顶点色，系数存扩展绘制段 reserve[2]（1..255，0 = 未声明）；
        //   · grayscale/sepia/saturate/hue-rotate/invert/contrast → RGB 上的一次仿射变换，
        //     顶点色乘不出来，编译期按书写顺序合成成 3×3+偏移写进滤镜段（v26），
        //     后端解码图片后逐像素跑一遍（详见 include/binui_filter.h）。
        //   · blur() → 组语义（节点 + 整棵子树），只能离屏：半径写进绘制段 blurPx（v32），
        //     绘制期把子树包进图层、按半径降采样模糊后再合成（与 backdrop-filter 同一条链）。
        //   · url(#f) → 引用 SVG <filter>：那一趟已经在前面把原语链**归约**成
        //     "整组模糊 + 身后的彩色影子"（v0.16.47），这里只把结果抄进同样的落点。
        //     归约不了的原语在 <filter> 定义处逐条告警，不在这里重复。
        {
            // filter 既是 CSS 属性也是 SVG 表现属性（<g filter="url(#f)">）：属性优先，
            // 与仓库其它属性口径一致（pick）。
            std::string filtRaw = TrimCopy(pick(e.attrs, style, "filter", "filter", ""));
            if (filtRaw.empty()) {
                auto wf = style.find("-webkit-filter");
                if (wf != style.end()) filtRaw = TrimCopy(wf->second);
            }
            if (!filtRaw.empty()) {
                const std::string fv = LowerCopy(filtRaw);
                if (!fv.empty() && fv != "none") {
                    // 按空白拆函数（括号内的空格不拆）
                    std::vector<std::string> fns;
                    {
                        std::string cur;
                        int depth = 0;
                        for (char ch : fv) {
                            if (ch == '(') depth++;
                            else if (ch == ')' && depth > 0) depth--;
                            if ((ch == ' ' || ch == '\t') && depth == 0) {
                                if (!cur.empty()) { fns.push_back(cur); cur.clear(); }
                                continue;
                            }
                            cur += ch;
                        }
                        if (!cur.empty()) fns.push_back(cur);
                    }
                    // 单个参数：支持百分比（50%）与纯数字（0.5）；返回 0..1 之外的也原样给
                    auto amount = [&](const std::string& arg, float dflt) -> float {
                        if (arg.empty()) return dflt;
                        if (arg.back() == '%')
                            return parseLen(arg.substr(0, arg.size() - 1), dflt * 100.0f, "filter") / 100.0f;
                        return parseLen(arg, dflt, "filter");
                    };
                    // 角度：deg / turn / rad / grad（无单位按度）
                    auto angle = [&](const std::string& arg) -> float {
                        if (arg.empty()) return 0.0f;
                        const std::string u = LowerCopy(arg);
                        if (u.size() > 3 && u.compare(u.size() - 4, 4, "turn") == 0)
                            return parseLen(arg.substr(0, arg.size() - 4), 0.0f, "filter") * 360.0f;
                        if (u.size() > 3 && u.compare(u.size() - 4, 4, "grad") == 0)
                            return parseLen(arg.substr(0, arg.size() - 4), 0.0f, "filter") * 0.9f;
                        if (u.size() > 2 && u.compare(u.size() - 3, 3, "rad") == 0)
                            return parseLen(arg.substr(0, arg.size() - 3), 0.0f, "filter") *
                                   57.29577951308232f;
                        if (u.size() > 2 && u.compare(u.size() - 3, 3, "deg") == 0)
                            return parseLen(arg.substr(0, arg.size() - 3), 0.0f, "filter");
                        return parseLen(arg, 0.0f, "filter");
                    };
                    binui::ColorMatrix cm = binui::ColorMatrixIdentity();
                    bool anyMatrix = false;
                    bool sawUnsupported = false;
                    bool sawDropShadow = false;
                    bool sawBlurFromUrl = false;   // 模糊来自 url(#f)：那是 SVG 本来的语义，不告警
                    for (const std::string& fn : fns) {
                        const size_t lp = fn.find('(');
                        if (lp == std::string::npos || fn.back() != ')') { sawUnsupported = true; continue; }
                        const std::string name = fn.substr(0, lp);
                        const std::string arg = fn.substr(lp + 1, fn.size() - lp - 2);
                        binui::ColorMatrix one;
                        bool known = true;
                        if (name == "brightness") {
                            // 只乘暗（>1 需要提亮：顶点色只能乘、不能加），保持 0.16.15 的行为
                            float k = amount(arg, 1.0f);
                            if (k < 0.0f) k = 0.0f;
                            if (k < 1.0f) {
                                int b8 = (int)std::lround(k * 255.0f);
                                if (b8 < 1) b8 = 1;
                                paint.reserve[2] = (uint8_t)b8;
                            } else if (k > 1.0f) {
                                Warn("CSS filter: brightness() 只支持乘暗（0~1），\"" + fn + "\" 按 1 处理");
                            }
                            continue;
                        } else if (name == "blur") {
                            // CSS：blur() 只接受**长度**（px / em / rem），0 = 没事干。
                            // 百分比在 CSS Filter Effects 里对 blur() 无效（那是 SVG 的
                            // stdDeviation 语法），写错就按 0 处理并告警。
                            const std::string a2 = TrimCopy(arg);
                            float bp = 0.0f;
                            if (!a2.empty() && a2.back() == '%') {
                                Warn("CSS filter: blur() 只支持长度（如 blur(4px)），百分比已忽略");
                            } else {
                                bp = parseLen(a2, 0.0f, "filter");
                                if (bp < 0.0f) bp = 0.0f;
                                // 上限 64px：降采样链最多 6 级（2^6），再大的半径只是把
                                // 已经糊成一片的画面继续糊，却要多扫几遍整块区域。
                                if (bp > kMaxFilterBlurPx) {
                                    bp = kMaxFilterBlurPx;
                                    Warn("CSS filter: blur() 上限 64px，\"" + fn + "\" 按 64px 处理");
                                }
                            }
                            if (bp > 0.0f) paint.blurPx = bp;
                            continue;
                        } else if (name == "url") {
                            // v0.16.47：filter: url(#f) → 取前面归约好的 SVG <filter>
                            std::string fid2;
                            if (!ParseUrlId(fn, fid2)) {
                                Warn("CSS filter: " + fn + " 不是 url(#id) 写法，已忽略");
                            } else {
                                auto sf = svgFilters.find(fid2);
                                if (sf == svgFilters.end() || !sf->second.found) {
                                    Warn("CSS filter: url(#" + fid2 + ") 找不到对应的 <filter> "
                                         "定义（请检查 id 拼写，或 <filter> 是不是写在别的文件里）");
                                } else {
                                    const SvgFilterDef& sd = sf->second;
                                    if (sd.blur > 0.0f) {
                                        paint.blurPx = sd.blur;
                                        sawBlurFromUrl = true;
                                    }
                                    // SVG 叠放顺序是"先写的在下面"，而阴影段的口径是
                                    // "下标 0 在最上面"（后端逆序画）→ 这里倒着放。
                                    for (int si = (int)sd.shadows.size() - 1; si >= 0; si--) {
                                        const SvgFilterShadow& sh = sd.shadows[(size_t)si];
                                        BinUIFilterDropShadowEntry e = {};
                                        e.nodeId = n.id;
                                        e.dx = sh.dx;
                                        e.dy = sh.dy;
                                        e.blur = sh.blur;
                                        e.color = sh.color;
                                        dropShadowNodes.push_back(e);
                                        sawDropShadow = true;
                                    }
                                }
                            }
                            continue;
                        } else if (name == "drop-shadow") {
                            // v33：语法 <color>? && <length>{2,3} —— 两个偏移必填、
                            // 第三个是模糊、颜色可写在任意位置（缺省 = currentColor）。
                            // 阴影是"轮廓的影子"，所以它跟 blur 一样走离屏图层（见绘制侧）；
                            // 这里只按顺序记下参数，多条阴影按书写顺序（写在前面的在上层）。
                            std::vector<std::string> toks;
                            {
                                std::string cur;
                                int dep = 0;
                                for (char ch : arg) {
                                    if (ch == '(') dep++;
                                    else if (ch == ')' && dep > 0) dep--;
                                    if ((ch == ' ' || ch == '\t' || ch == ',') && dep == 0) {
                                        if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
                                        continue;
                                    }
                                    cur += ch;
                                }
                                if (!cur.empty()) toks.push_back(cur);
                            }
                            float lens[3] = {0.0f, 0.0f, 0.0f};
                            int nl = 0;
                            bool hasCol = false;
                            uint32_t scol = textCol;   // CSS 缺省 = currentColor
                            for (const std::string& tk : toks) {
                                const bool isColorTok =
                                    (!tk.empty() && (tk[0] == '#' ||
                                                     tk.find('(') != std::string::npos)) ||
                                    (tk.size() > 1 && (tk[0] == 'r' || tk[0] == 'h' || tk[0] == 'b'));
                                if (isColorTok) {
                                    scol = parseColorWith(tk, textCol, "filter: drop-shadow 颜色");
                                    hasCol = true;
                                    continue;
                                }
                                if (nl < 3) lens[nl++] = parseLen(tk, 0.0f, "filter");
                            }
                            (void)hasCol;
                            if (nl < 2) {
                                Warn("CSS filter: drop-shadow() 至少要两个偏移量（如 drop-shadow(0 4px 8px #0003)），已忽略");
                            } else {
                                float sb = lens[2] > 0.0f ? lens[2] : 0.0f;
                                if (sb > kMaxFilterBlurPx) {
                                    sb = kMaxFilterBlurPx;
                                    Warn("CSS filter: drop-shadow() 的模糊上限 64px，\"" + fn + "\" 按 64px 处理");
                                }
                                BinUIFilterDropShadowEntry e = {};
                                e.nodeId = n.id;
                                e.dx = lens[0];
                                e.dy = lens[1];
                                e.blur = sb;
                                e.color = scol;
                                dropShadowNodes.push_back(e);
                                sawDropShadow = true;
                            }
                            continue;
                        } else if (name == "grayscale") {
                            one = binui::FilterGrayscale(amount(arg, 1.0f));
                        } else if (name == "sepia") {
                            one = binui::FilterSepia(amount(arg, 1.0f));
                        } else if (name == "saturate") {
                            one = binui::FilterSaturate(amount(arg, 1.0f));
                        } else if (name == "hue-rotate") {
                            one = binui::FilterHueRotate(angle(arg));
                        } else if (name == "invert") {
                            one = binui::FilterInvert(amount(arg, 1.0f));
                        } else if (name == "contrast") {
                            one = binui::FilterContrast(amount(arg, 1.0f));
                        } else {
                            known = false;
                        }
                        if (!known) {
                            sawUnsupported = true;
                            continue;
                        }
                        cm = binui::ColorMatrixMul(one, cm);   // 先写的先作用
                        anyMatrix = true;
                    }
                    if (anyMatrix && !binui::ColorMatrixIsIdentity(cm)) {
                        BinUIImageFilter f = {};
                        f.nodeId = n.id;
                        f.flags = 1;
                        for (int k = 0; k < 9; k++) f.m[k] = cm.m[k];
                        for (int k = 0; k < 3; k++) f.offset[k] = cm.o[k];
                        filterNodes.push_back(f);
                    }
                    if (sawUnsupported)
                        Warn("CSS filter: 有认不出的函数，已忽略"
                             "（blur/drop-shadow/url(#id)/grayscale/sepia/saturate/hue-rotate/"
                             "invert/contrast/brightness 已支持）");
                    // 语义顺序提醒：CSS 里滤镜是**从左到右**复合的，`blur() drop-shadow()`
                    // 的阴影本该取自"已经糊过"的轮廓。我们的离屏层只做一趟，阴影与本体
                    // 都取自同一张（未糊的）层内容 —— 两者同写时明确告警，不静默近似。
                    if (sawDropShadow && paint.blurPx > 0.0f && !sawBlurFromUrl)
                        Warn("CSS filter: 同时写 blur() 与 drop-shadow() 时，阴影按**未模糊**的"
                             "轮廓生成（两者共用同一张离屏图）");
                }
            }
        }

        // 注意 reserve[2]（filter: brightness）没有对应的 PAINT_* 位可用，所以它自己也算
        // "这个节点要出扩展绘制条目"的理由 —— 否则一个只写了 filter 的 <img> 不会进段。
        if (pflags || paint.textFlags || paint.reserve[2] || paint.letterSpacing != 0.0f ||
            paint.blurPx != 0.0f) {
            paint.nodeId = n.id;
            paint.flags = pflags;
            paintNodes.push_back(paint);
        }

        // ---- v23 box-shadow（阴影段）----
        // 语法：none | [inset? && <length>{2,4} && <color>] [, ...]*
        //   两个长度必填（offset-x / offset-y），第三、四个是 blur / spread。
        //   颜色可写在这几个长度的任意位置；不写 = 当前文字色（CSS 的 currentColor）。
        // 顺序：数组按**绘制顺序**存（先画的在下面），而 CSS 是"先写的在上面"，
        //   所以这里把作者顺序反过来写进段里（渲染器只顺序画一遍）。
        // inset 不支持（后端只有 fillRect/fillRounded，画不出"挖内孔"的几何），
        //   只告警不写出 —— 与其画成外阴影（看起来像"卡片浮起来"），不如干脆不画。
        {
            std::string shv = pick(e.attrs, style, "shadow", "box-shadow", "");
            if (!shv.empty()) {
                // 顶层逗号切分（rgba(0,0,0,.3) 里的逗号不算）
                std::vector<std::string> items;
                {
                    std::string cur;
                    int depth = 0;
                    for (char ch : shv) {
                        if (ch == '(') depth++;
                        else if (ch == ')' && depth > 0) depth--;
                        if (ch == ',' && depth == 0) { items.push_back(cur); cur.clear(); continue; }
                        cur += ch;
                    }
                    if (!cur.empty()) items.push_back(cur);
                }
                std::vector<BinUIShadow> authored;
                for (const std::string& raw : items) {
                    const std::string piece = TrimCopy(raw);
                    if (piece.empty()) continue;
                    const std::string lpiece = LowerCopy(piece);
                    if (lpiece == "none") continue;
                    BinUIShadow s = {};
                    float lens[4] = { 0, 0, 0, 0 };
                    int nlen = 0;
                    bool inset = false, hasColor = false;
                    for (const std::string& tok : splitCssTokens(piece)) {
                        if (tok == "inset") { inset = true; continue; }
                        const char c0 = tok.empty() ? ' ' : tok[0];
                        if (c0 == '#' || std::isalpha(static_cast<unsigned char>(c0))) {
                            uint32_t c = 0;
                            if (ParseColorValue(tok, &c, "box-shadow 颜色")) { s.color = c; hasColor = true; }
                            continue;
                        }
                        const LenPct lp = ParseLenPct(tok, 0.0f, curCtx, "box-shadow");
                        if (lp.pct) {
                            Warn("CSS box-shadow 的偏移/模糊不接受百分比（\"" + tok +
                                 "\"），按 0 处理");
                            continue;
                        }
                        if (nlen < 4) lens[nlen] = lp.value;
                        nlen++;
                    }
                    if (inset) {
                        Warn("CSS box-shadow 目前不支持 inset（内阴影）：\"" + piece + "\" 已忽略");
                        continue;
                    }
                    if (nlen < 2) {
                        Warn("CSS box-shadow 至少需要 2 个长度（offset-x / offset-y）：\"" +
                             piece + "\" 已忽略");
                        continue;
                    }
                    if (nlen > 4)
                        Warn("CSS box-shadow 最多 4 个长度（offset-x / offset-y / blur / spread）：\""
                             + piece + "\"，多出来的忽略");
                    for (int k = 0; k < 4; k++) if (lens[k] < 0.0f && k >= 2) lens[k] = 0.0f;  // blur/spread 负值无意义
                    if (!hasColor) s.color = textCol;      // currentColor
                    if (((s.color >> 24) & 0xFFu) == 0) continue;   // 全透明 = 不画
                    s.offsetX = lens[0];
                    s.offsetY = lens[1];
                    s.blur    = lens[2];
                    s.spread  = lens[3];
                    s.nodeId  = n.id;
                    s.flags   = 0;
                    authored.push_back(s);
                }
                // 反序写出：数组顺序 = 绘制顺序（后画的盖在上面 = 作者先写的那条在上面）
                for (size_t k = authored.size(); k > 0; k--) shadowNodes.push_back(authored[k - 1]);
            }
        }

        // ---- v24 text-shadow（文字阴影段）----
        // 语法：none | [<length>{2,3} && <color>] [, ...]*
        //   两个长度必填（offset-x / offset-y），第三个是 blur，**没有 spread、没有 inset**。
        //   颜色不写 = 当前文字色（CSS 的 currentColor）。
        // 可继承：本元素没写就沿用祖先的生效值（CSS 规定 text-shadow 是继承属性）；
        //   写了 none / 全透明 = 从本元素起清掉整棵子树。
        // v0.16.50：**多层**。CSS 的绘制顺序是"写在前面的在最上面"，而段的数组顺序
        //   = 绘制顺序（下标小的先画），所以下面按"书写顺序的逆序"写出（与 box-shadow
        //   同一套约定）。每层在 GPU 端多一份模糊覆盖率图集、在 GDI 端多一张位图，
        //   所以层数封在 BINUI_MAX_TEXT_SHADOWS，超出的点名忽略。
        std::vector<BinUITextShadow> tsLayers = pc.tsLayers;
        {
            std::string tsv = pick(e.attrs, style, "text-shadow", "text-shadow", "");
            if (!tsv.empty()) {
                std::vector<std::string> items;
                {
                    std::string cur;
                    int depth = 0;
                    for (char ch : tsv) {
                        if (ch == '(') depth++;
                        else if (ch == ')' && depth > 0) depth--;
                        if (ch == ',' && depth == 0) { items.push_back(cur); cur.clear(); continue; }
                        cur += ch;
                    }
                    if (!cur.empty()) items.push_back(cur);
                }
                std::vector<BinUITextShadow> authored;   // 书写顺序（第一个在最上面）
                for (const std::string& raw : items) {
                    const std::string piece = TrimCopy(raw);
                    if (piece.empty()) continue;
                    if (LowerCopy(piece) == "none") continue;   // 由 authored 是否为空决定清不清
                    BinUITextShadow s = {};
                    float lens[3] = { 0, 0, 0 };
                    int nlen = 0;
                    bool hasColor = false;
                    for (const std::string& tok : splitCssTokens(piece)) {
                        const char c0 = tok.empty() ? ' ' : tok[0];
                        if (c0 == '#' || std::isalpha(static_cast<unsigned char>(c0))) {
                            uint32_t c = 0;
                            if (LowerCopy(tok) == "currentcolor") { s.color = textCol; hasColor = true; continue; }
                            if (ParseColorValue(tok, &c, "text-shadow 颜色")) { s.color = c; hasColor = true; }
                            continue;
                        }
                        const LenPct lp = ParseLenPct(tok, 0.0f, curCtx, "text-shadow");
                        if (lp.pct) {
                            Warn("CSS text-shadow 的偏移/模糊不接受百分比（\"" + tok +
                                 "\"），按 0 处理");
                            continue;
                        }
                        if (nlen < 3) lens[nlen] = lp.value;
                        nlen++;
                    }
                    if (nlen < 2) {
                        Warn("CSS text-shadow 至少需要 2 个长度（offset-x / offset-y）：\"" +
                             piece + "\" 已忽略");
                        continue;
                    }
                    if (nlen > 3)
                        Warn("CSS text-shadow 最多 3 个长度（offset-x / offset-y / blur）：\"" +
                             piece + "\"，多出来的按 0 处理（text-shadow 没有 spread）");
                    if (lens[2] < 0.0f) lens[2] = 0.0f;      // 负的 blur 半径无意义
                    if (!hasColor) s.color = textCol;        // currentColor
                    if (((s.color >> 24) & 0xFFu) == 0) continue;   // 全透明 = 这一层不画
                    s.offsetX = lens[0];
                    s.offsetY = lens[1];
                    s.blur    = lens[2];
                    s.flags   = 0;
                    authored.push_back(s);
                }
                if (authored.empty()) {
                    tsLayers.clear();                    // 只写了 none / 全透明
                } else {
                    if (static_cast<int>(authored.size()) > BINUI_MAX_TEXT_SHADOWS) {
                        Warn("CSS text-shadow 最多支持 " +
                             std::to_string(BINUI_MAX_TEXT_SHADOWS) + " 层：\"" + tsv +
                             "\" 里多出来的 " +
                             std::to_string(static_cast<int>(authored.size()) -
                                            BINUI_MAX_TEXT_SHADOWS) +
                             " 层被忽略（每层都要多烘一份字形模糊覆盖率，"
                             "再多对观感没有增量）");
                        authored.resize(static_cast<size_t>(BINUI_MAX_TEXT_SHADOWS));
                    }
                    tsLayers.clear();
                    // 反序：数组顺序 = 绘制顺序（作者写的第一个在最上面 = 最后画）
                    for (size_t k = authored.size(); k > 0; k--) tsLayers.push_back(authored[k - 1]);
                }
            }
        }
        // 只有真正会画字的节点才写条目：阴影是"按字形位图模糊"，没字就没得画。
        // （INPUT 的文字来自运行期的输入值，编译期可能是空的 —— 它是单行输入框，
        //   占位符/内容一定非空时才画，所以用 srcText 判断已足够。）
        if (!tsLayers.empty() && !srcText.empty()) {
            for (BinUITextShadow ts : tsLayers) {   // 数组顺序 = 绘制顺序
                ts.nodeId = n.id;
                ts.flags = 0;
                textShadowNodes.push_back(ts);
            }
        }

        // 字号已在元素开头算好（供 em 换算用），这里直接落库
        {
            float fsz = nodeFontSize;
            if (fsz < 1.0f) fsz = 1.0f;
            if (fsz > 4096.0f) fsz = 4096.0f;
            n.fontSize = (uint16_t)(fsz + 0.5f);
        }

        // 圆角：border-radius 指定才有圆角；未指定 = 直角

        {
            std::string br = style.count("border-radius") ? style.at("border-radius") : "";
            n.radius = br.empty() ? 0 : (uint16_t)parseLen(br, 0);
        }

        // 层叠顺序：style z-index（或 XML 风格 z 属性）；未指定 = 0
        {
            std::string zi = pick(e.attrs, style, "z-index", "z-index", "");
            if (zi.empty()) zi = pick(e.attrs, style, "z", "z", "");
            n.zIndex = zi.empty() ? 0 : (int16_t)std::atoi(zi.c_str());
        }

        // ---- SVG 图形语义（<line>/<circle>/<rect>）----
        // 颜色一律走继承链（eFill/eStroke，见上面的 SVG 颜料块）：
        // 以前这里各自 pick() 一次，<svg fill="#fff"> 对子元素完全无效。
        if (e.tag == "line" && !hasSvgPath) {
            // x,y = 起点（绝对）；width,height = 终点（绝对）＝ 起点 + 声明偏移
            n.x = pc.ox + parseLen(sx, 0);
            n.y = pc.oy + parseLen(sy, 0);
            n.width = n.x + (svgLineX2 - parseLen(sx, 0));
            n.height = n.y + (svgLineY2 - parseLen(sy, 0));
            n.color = eHasStroke ? eStroke : 0xFF000000u;   // 线的默认色：黑
            n.radius = (uint16_t)(eStrokeW > 0 ? eStrokeW : 1);
            n.flags |= BINUI_ABSOLUTE;
        } else if (e.tag == "circle" && !hasSvgPath) {
            n.color = eHasFill ? eFill : 0;                 // fill="none" → 不画底色块
            n.radius = (uint16_t)svgR;
        } else if (e.tag == "rect" && !hasSvgPath) {
            n.color = eHasFill ? eFill : 0;
            std::string rx = e.attrs.count("rx") ? e.attrs.at("rx") : "";
            if (!rx.empty()) n.radius = (uint16_t)(parseLen(rx, 0) * PathScaleFactor(svgX));
        } else if (e.tag == "text" && inSvg) {
            // 文字色走标签自己的填充色（LABEL 的旧约定：color 就是文字色）。
            // 渐变文字做不到（文字是字形图集里逐个 BLEND 画出来的，没有沿字形取色的通路）。
            if (!fillGradId.empty()) {
                Warn("<text> 的渐变填充未实现，按首个色标纯色绘制"
                     "（要渐变文字请把文字转成路径）");
                auto git = svgGrads.find(fillGradId);
                if (git != svgGrads.end() && !git->second.stops.empty())
                    n.color = git->second.stops[0].color;
            } else if (eHasFill) {
                n.color = eFill;
            }
            // 描边对文字完全没有通路（字形是图集位图，没有轮廓可以描）——
            // 只在元素自己写了 stroke 时提醒，别让 <svg stroke="…"> 的继承把每行文字都刷一遍
            if (style.count("stroke") || e.attrs.count("stroke"))
                Warn("SVG <text> 不支持描边（文字没有轮廓描边通路），"
                     "需要描边请把文字转成 <path>");
        }

        // 文本来源：元素自身文本 > 输入框/按钮的值 > 图片的 alt 兜底
        if (!srcText.empty()) {
            textMap[n.id] = srcText;
        } else if (type == BINUI_INPUT || type == BINUI_BUTTON || isCheckbox) {
            if (!inputVal.empty()) textMap[n.id] = inputVal;
        } else if (type == BINUI_IMAGE && e.attrs.count("alt") && !e.attrs.at("alt").empty()) {
            // 图片资源缺失时后端会把这行文字居中画出来（alt 兜底这条路早就有了，只是没人写进去）
            textMap[n.id] = e.attrs.at("alt");
        }

        // <li>：无序列表加项目符号，有序列表（父 <ol>）加序号 —— 列表起码看得出来是列表
        // 作者用 li::before{content:"▸ "} 自带符号时不再叠加自动项目符号
        if (e.tag == "li" && !e.attrs.count("_no_bullet")) {
            std::string prefix = "\xE2\x80\xA2 "; // "• "
            if (e.parentIdx != SIZE_MAX && e.parentIdx < elems.size() &&
                lowerStr(elems[e.parentIdx].tag) == "ol") {
                int index = 1;
                for (size_t sib : childrenOf[e.parentIdx]) {
                    if (sib == ei) break;
                    if (lowerStr(elems[sib].tag) == "li") index++;
                }
                prefix = std::to_string(index) + ". ";
            }
            auto it = textMap.find(n.id);
            textMap[n.id] = prefix + (it != textMap.end() ? it->second : std::string());
        }

        // 图片节点：src 属性 -> imageIndex（引用编译结果中的资源路径表）
        if (type == BINUI_IMAGE && outImages) {
            std::string src = e.attrs.count("src") ? e.attrs.at("src") : "";
            // SVG <image href="x.png">：资源路径同样相对 UI 文件（<img src> 的老路径）
            if (src.empty() && e.attrs.count("href")) src = e.attrs.at("href");
            if (src.empty() && e.attrs.count("xlink:href")) src = e.attrs.at("xlink:href");
            if (!src.empty() && src.rfind("data:", 0) != 0) {
                n.imageIndex = (uint16_t)outImages->size();
                outImages->push_back(src);
            } else if (src.rfind("data:", 0) == 0) {
                Warn("SVG <image> 的 data: 内联图片不支持（用独立图片文件）");
            }
        }

        // v25 九宫格图片：`image-slice: <内缩像素>`（CSS 的 border-image-slice 简化版）。
        // 只支持**四边同宽**：`BinUIPaintEx` 只占 4 字节（一个 u8 内缩），而"四边不等"
        // 在 UI 里几乎见不到（九宫格贴图本身是对称的）。与 border-radius 互斥：
        // 九宫格的圆角通常已经烘进贴图，再叠加圆角裁剪只会把四角切掉一层。
        if (type == BINUI_IMAGE) {
            std::string slv = pick(e.attrs, style, "image-slice", "image-slice", "");
            if (slv.empty()) slv = pick(e.attrs, style, "nine-slice", "nine-slice", "");
            if (!slv.empty()) {
                std::vector<std::string> toks = splitCssTokens(slv);
                float vals[4] = { 0, 0, 0, 0 };
                int nv = 0;
                for (const std::string& tk : toks) {
                    const LenPct lp = ParseLenPct(tk, 0.0f, curCtx, "image-slice");
                    if (lp.pct) { Warn("CSS image-slice 不接受百分比（\"" + tk + "\"），按 0 处理"); continue; }
                    if (nv < 4) vals[nv] = lp.value;
                    nv++;
                }
                float sl = vals[0];
                if (nv >= 2) {
                    sl = vals[0];
                    bool same = true;
                    for (int k = 0; k < std::min(nv, 4); k++) if (vals[k] != sl) same = false;
                    if (!same) Warn("CSS image-slice 只支持四边同宽，\"" + slv +
                                   "\" 取第一个值");
                }
                int ipx = (int)(sl + 0.5f);
                if (ipx < 1) ipx = 0;
                if (ipx > 255) { Warn("CSS image-slice 上限 255px，已钳制"); ipx = 255; }
                if (ipx > 0) {
                    const bool hasRadius = style.count("border-radius") != 0 || e.attrs.count("radius") != 0;
                    if (hasRadius)
                        Warn("图片同时写了 image-slice 与 border-radius：九宫格优先，圆角忽略");
                    // v0.16.49：中段怎么填 —— CSS `border-image-repeat: <x> [<y>]`
                    // （stretch | repeat | round；v0.16.51 起 space）。
                    // 两个值分别管"上/下边"和"左/右边"，缺省时两者相同。
                    uint8_t repFlags = 0;
                    {
                        std::string rv = pick(e.attrs, style, "border-image-repeat",
                                              "border-image-repeat", "");
                        if (!rv.empty()) {
                            int modes[2] = { 0, 0 };   // 0=stretch 1=repeat 2=round 3=space
                            int nm = 0;
                            for (const std::string& tk : splitCssTokens(rv)) {
                                const std::string lt = LowerCopy(tk);
                                int m = 0;
                                if (lt == "stretch") m = 0;
                                else if (lt == "repeat") m = 1;
                                else if (lt == "round") m = 2;
                                else if (lt == "space") m = 3;
                                else {
                                    Warn("CSS border-image-repeat 认不出的值（stretch/repeat/"
                                         "round/space）：\"" + tk + "\"，已按 stretch 处理");
                                }
                                if (nm < 2) modes[nm] = m;
                                nm++;
                            }
                            if (nm >= 1) {
                                if (nm == 1) modes[1] = modes[0];
                                repFlags = (uint8_t)((modes[0] & 3) | ((modes[1] & 3) << 2));
                            }
                        }
                    }
                    BinUIPaintEx pe = {};
                    pe.nodeId = n.id;
                    pe.imageSlice = (uint8_t)ipx;
                    pe.flags = repFlags;
                    paintExNodes.push_back(pe);
                    n.radius = 0;   // 九宫格自己带角，别再裁一层
                }
            } else if (style.count("border-image-repeat") ||
                       e.attrs.count("border-image-repeat")) {
                Warn("border-image-repeat 只对九宫格图片（image-slice）生效："
                     "当前图片没写 image-slice，已忽略");
            }
        }

        // 视频节点：src -> 伪图片槽 + 视频描述段；解码帧由可选模块经
        // updateImageRaw 推送，无模块时仅绘制底色（优雅降级）
        if (type == BINUI_VIDEO) {
            std::string vsrc = e.attrs.count("src") ? e.attrs.at("src") : "";
            BinUIVideoDesc vd = {};
            vd.nodeId = n.id;
            if (!vsrc.empty()) {
                vd.srcOffset = getOrAddString(vsrc);
                if (outImages) { // 伪图片槽与 img 共用编号空间
                    n.imageIndex = (uint16_t)outImages->size();
                    outImages->push_back(vsrc);
                }
            }
            vd.imageIndex = n.imageIndex;
            auto flagOf = [&](const char* a) -> bool {
                auto it = e.attrs.find(a); // HTML 布尔属性无值（controls/autoplay/…）
                if (it == e.attrs.end()) return false;
                std::string v = lowerStr(it->second);
                return v != "false" && v != "0" && v != "off";
            };
            if (flagOf("autoplay")) vd.flags |= VID_AUTOPLAY;
            if (flagOf("loop"))     vd.flags |= VID_LOOP;
            if (flagOf("muted"))    vd.flags |= VID_MUTED;
            if (flagOf("controls")) vd.flags |= VID_CONTROLS;
            float vol = parseLen(e.attrs.count("volume") ? e.attrs.at("volume") : "", 1.0f);
            vd.volume = std::min(std::max(vol, 0.0f), 1.0f);
            videoDescs.push_back(vd);
        }

        // ---- v0.16.24：clip-path（v27 裁剪路径段） ----
        // clipPath 的几何在编译期换算到设备空间、按自己的包围盒归一化；运行期只要
        // "画进离屏图层 → 用 even-odd 互补区擦掉外面"。
        if (!svgClips.empty()) {
            std::string cpv = style.count("clip-path") ? TrimCopy(style.at("clip-path")) : "";
            if (cpv.empty() && e.attrs.count("clip-path"))
                cpv = TrimCopy(e.attrs.at("clip-path"));
            if (!cpv.empty() && lowerStr(cpv) != "none") {
                std::string cgid;
                if (!ParseUrlId(cpv, cgid)) {
                    Warn("clip-path 只支持 url(#id) 写法（" + cpv + "）");
                } else {
                    auto cit = svgClips.find(cgid);
                    if (cit == svgClips.end()) {
                        Warn("clip-path=\"url(#" + cgid + ")\" 找不到 <clipPath> 定义");
                    } else if (!cit->second.kids.empty()) {
                        // 元素包围盒（设备空间、不含页面偏移）：objectBoundingBox 用它放大单位坐标
                        float bx = 0, by = 0, bw = 0, bh = 0;
                        if (hasSvgPath) {
                            bx = svgPathShape.minX; by = svgPathShape.minY;
                            bw = svgPathShape.Width(); bh = svgPathShape.Height();
                        } else {
                            bx = parseLen(sx, 0); by = parseLen(sy, 0);
                            bw = parseLen(sw, 0); bh = parseLen(sh, 0);
                        }
                        PathMatrix M;
                        if (cit->second.units == 1) {
                            M = PathMul(PathTranslate(bx, by),
                                        PathScale(bw > 0.0f ? bw : 1.0f, bh > 0.0f ? bh : 1.0f));
                            if (svgX.b != 0.0f || svgX.c != 0.0f)
                                Warn("clipPathUnits=objectBoundingBox 遇到旋转/斜切："
                                     "按轴对齐包围盒近似");
                        } else {
                            M = svgX;
                        }
                        PathShape merged;
                        // objectBoundingBox 的坐标是 0..1：容差跟着缩（0.1 会切出多边形）
                        const float tol = (cit->second.units == 1) ? 0.002f : 0.1f;
                        for (size_t ki : cit->second.kids) {
                            PathShape tmp;
                            if (!shapeOf(elems[ki], tmp, tol)) continue;
                            TransformShape(tmp, M);
                            const uint32_t base = static_cast<uint32_t>(merged.pts.size());
                            for (const PathPointF& q2 : tmp.pts)
                                merged.pts.push_back(q2);
                            for (const PathShape::Contour& sc : tmp.contours) {
                                PathShape::Contour nc;
                                nc.first = base + sc.first;
                                nc.count = sc.count;
                                nc.closed = sc.closed;
                                merged.contours.push_back(nc);
                            }
                        }
                        merged.RecomputeBounds();
                        PathNormData cnd;
                        if (!merged.Empty() && NormalizeShape(merged, cnd)) {
                            PendingClip cp;
                            cp.nodeId = n.id;
                            cp.flags = cit->second.evenOdd ? 1 : 0;
                            cp.x = merged.minX + pc.ox;
                            cp.y = merged.minY + pc.oy;
                            cp.w = merged.Width();
                            cp.h = merged.Height();
                            cp.first = std::move(cnd.contourFirst);
                            cp.closed = std::move(cnd.contourClosed);
                            cp.pts = std::move(cnd.pts);
                            clipNodes.push_back(std::move(cp));
                        }
                    }
                }
            }
        }

        // ---- v0.16.26：<mask>（v28 遮罩段） ----
        // 掩码几何在编译期折算到设备空间并按"亮度分组"，运行期只做两趟混合。
        // 元素自己照常画（不像 pattern 要交给 tile），只是画完在图层里被乘一次 alpha。
        // v0.16.54：mask-type 是**<mask> 元素**的属性（CSS Masking），写在别的元素上无效 ——
        // 这条仓库的规矩是"写了没效果必须说出来"，所以点名。
        if (e.attrs.count("mask-type") || e.attrs.count("masktype") || style.count("mask-type")) {
            Warn("mask-type 只对 <mask> 元素自身生效（写在 " + e.tag +
                 " 上被忽略）：它描述的是\"用亮度还是 alpha 当遮罩\"");
        }
        if (!svgMasks.empty()) {
            std::string mv = style.count("mask") ? TrimCopy(style.at("mask")) : "";
            if (mv.empty() && e.attrs.count("mask")) mv = TrimCopy(e.attrs.at("mask"));
            if (!mv.empty() && lowerStr(mv) != "none") {
                std::string mgid;
                if (!ParseUrlId(mv, mgid)) {
                    Warn("mask 只支持 url(#id) 写法（" + mv + "）");
                } else {
                    auto mit = svgMasks.find(mgid);
                    if (mit == svgMasks.end()) {
                        Warn("mask=\"url(#" + mgid + ")\" 找不到 <mask> 定义（元素不遮罩）");
                    } else if (!mit->second.kids.empty()) {
                        const SvgMaskDef& md3 = mit->second;
                        // 元素包围盒（设备空间、不含页面偏移）：SVG 图形用几何盒，
                        // 其它元素用节点盒
                        float bx3 = 0, by3 = 0, bw3 = 0, bh3 = 0;
                        if (hasSvgPath) {
                            bx3 = svgPathShape.minX; by3 = svgPathShape.minY;
                            bw3 = svgPathShape.Width(); bh3 = svgPathShape.Height();
                        } else {
                            bx3 = (float)n.x - pc.ox; by3 = (float)n.y - pc.oy;
                            bw3 = (float)n.width; bh3 = (float)n.height;
                        }
                        const float ubw3 = bw3 > 0.0f ? bw3 : 1.0f;
                        const float ubh3 = bh3 > 0.0f ? bh3 : 1.0f;
                        // 区域 → 设备空间
                        // 没写 x/y/width/height → 用 SVG 默认的 -10%..120%（按元素盒算，
                        // 直接落在设备空间）；写了就按 maskUnits 解析：
                        //   objectBoundingBox → 元素盒的比例；userSpaceOnUse → 用户坐标（过 svgX）
                        float r0 = 0, r1 = 0, r2b = 0, r3b = 0;
                        const bool anyRegion = md3.hasX || md3.hasY || md3.hasW || md3.hasH;
                        if (!anyRegion) {
                            r0 = bx3 - 0.1f * ubw3;
                            r1 = by3 - 0.1f * ubh3;
                            r2b = r0 + 1.2f * ubw3;
                            r3b = r1 + 1.2f * ubh3;
                        } else {
                            auto regVal = [&](float v, bool pct, float ref, bool obb) -> float {
                                if (pct) return v * ref;      // 百分比一律按元素盒算（见告警）
                                return obb ? v * ref : v;
                            };
                            const float ox3 = regVal(md3.x, md3.xPct, ubw3, md3.units == 1);
                            const float oy3 = regVal(md3.y, md3.yPct, ubh3, md3.units == 1);
                            // v0.16.53 修正：这里以前两处都写死 obb=true，于是
                            // maskUnits="userSpaceOnUse" 的 width/height 被**乘了一遍元素盒**
                            // （190×60 的元素 + 200×120 的 mask 区域 → 38000×7200 的图层框）。
                            // 百分比那一支本来就按元素盒算（见上面的告警），不受影响。
                            const float ww3 = regVal(md3.w, md3.wPct, ubw3, md3.units == 1);
                            const float hh3 = regVal(md3.h, md3.hPct, ubh3, md3.units == 1);
                            // objectBoundingBox 的坐标本来就相对元素盒，直接加包围盒原点；
                            // userSpaceOnUse 的是用户坐标，要过 svgX（非 SVG 元素过元素原点）
                            PathMatrix Mreg = (md3.units == 1)
                                ? PathMatrix{}
                                : (hasSvgPath ? svgX : PathTranslate(bx3, by3));
                            if (Mreg.b != 0.0f || Mreg.c != 0.0f)
                                Warn("<mask id=\"" + mgid + "\"> 的区域遇到旋转/斜切："
                                     "按轴对齐包围盒近似");
                            const float ux0 = md3.units == 1 ? bx3 + ox3 : ox3;
                            const float uy0 = md3.units == 1 ? by3 + oy3 : oy3;
                            PathPointF c0 = PathApply(Mreg, ux0, uy0);
                            PathPointF c1 = PathApply(Mreg, ux0 + ww3, uy0 + hh3);
                            r0 = std::min(c0.x, c1.x); r1 = std::min(c0.y, c1.y);
                            r2b = std::max(c0.x, c1.x); r3b = std::max(c0.y, c1.y);
                        }
                        const float rw3 = r2b - r0, rh3 = r3b - r1;
                        if (rw3 <= 0.0f || rh3 <= 0.0f) {
                            Warn("<mask id=\"" + mgid + "\"> 的区域宽高必须大于 0（元素不遮罩）");
                        } else {
                            // 内容矩阵：userSpaceOnUse → 元素用户空间；objectBoundingBox → 元素盒
                            const PathMatrix Mc = (md3.contentUnits == 1)
                                ? PathMul(PathTranslate(bx3, by3), PathScale(ubw3, ubh3))
                                : (hasSvgPath ? svgX : PathTranslate(bx3, by3));
                            // 按亮度分组：同一个 alpha 的几何合成一条（运行期每条一次 draw）。
                            // v0.16.53：**渐变形状自己独占一条** —— 它的颜色不是常量，
                            // 和别的形状合并就没法表达；一条渐变 = 一条记录。
                            struct MaskGroup {
                                uint32_t color = 0;
                                PathShape shape;
                                const SvgMaskChild* gradKid = nullptr;
                            };
                            std::vector<MaskGroup> mgroups;
                            for (const SvgMaskChild& kid3 : md3.kids) {
                                size_t gi3 = mgroups.size();
                                if (!kid3.hasGrad) {
                                    for (size_t g4 = 0; g4 < mgroups.size(); g4++) {
                                        if (mgroups[g4].gradKid) continue;
                                        if (mgroups[g4].color == kid3.color) { gi3 = g4; break; }
                                    }
                                }
                                if (gi3 == mgroups.size()) {
                                    MaskGroup g5;
                                    g5.color = kid3.color;
                                    g5.gradKid = kid3.hasGrad ? &kid3 : nullptr;
                                    mgroups.push_back(std::move(g5));
                                }
                                PathShape tmp3 = kid3.shape;
                                TransformShape(tmp3, Mc);
                                PathShape& dst3 = mgroups[gi3].shape;
                                const uint32_t base3 = (uint32_t)dst3.pts.size();
                                for (const PathPointF& q4 : tmp3.pts) dst3.pts.push_back(q4);
                                for (const PathShape::Contour& sc3 : tmp3.contours) {
                                    PathShape::Contour nc3;
                                    nc3.first = base3 + sc3.first;
                                    nc3.count = sc3.count;
                                    nc3.closed = sc3.closed;
                                    dst3.contours.push_back(nc3);
                                }
                            }
                            // 渐变折算：把渐变控制点用 M = N ∘ Mc ∘ [kid 盒] ∘ gradientTransform
                            // 落到"本组几何包围盒归一化"空间（与路径渐变同一套口径）；
                            // 色标的 alpha 折入**亮度** —— 运行期保留趟那句 dst *= srcA 拿到的
                            // 就是逐点的掩码亮度，渐变蒙版天然成立。
                            auto buildMaskGrad = [&](const SvgMaskChild& kid3,
                                                     const PathShape& shp,
                                                     BinUIPathGradient& out) -> bool {
                                const SvgGradDef& g = kid3.grad;
                                if (g.stops.size() < 2) return false;
                                // userSpaceOnUse 的百分比按视口尺寸折算（与路径渐变一致）
                                SvgGradDef gl = g;
                                if (gl.userSpace && pc.vpW > 1e-6f && pc.vpH > 1e-6f) {
                                    if (gl.x1Pct) gl.x1 *= pc.vpW;
                                    if (gl.x2Pct) gl.x2 *= pc.vpW;
                                    if (gl.cxPct) gl.cx *= pc.vpW;
                                    if (gl.fxPct) gl.fx *= pc.vpW;
                                    if (gl.y1Pct) gl.y1 *= pc.vpH;
                                    if (gl.y2Pct) gl.y2 *= pc.vpH;
                                    if (gl.cyPct) gl.cy *= pc.vpH;
                                    if (gl.fyPct) gl.fy *= pc.vpH;
                                    if (gl.rPct)
                                        gl.r *= std::sqrt((pc.vpW * pc.vpW + pc.vpH * pc.vpH) * 0.5f);
                                }
                                const float bw2 = std::max(1e-6f, shp.Width());
                                const float bh2 = std::max(1e-6f, shp.Height());
                                const PathMatrix N = PathMul(PathScale(1.0f / bw2, 1.0f / bh2),
                                                             PathTranslate(-shp.minX, -shp.minY));
                                PathMatrix M = PathMul(N, Mc);
                                if (!gl.userSpace) {   // OBB：默认单位正方形 = kid 自己的盒
                                    M = PathMul(M, PathMul(PathTranslate(kid3.gbMinX, kid3.gbMinY),
                                                           PathScale(kid3.gbW, kid3.gbH)));
                                }
                                M = PathMul(M, gl.xform);
                                BinUIPathGradient pg = {};
                                pg.type = gl.type;
                                pg.spread = gl.spread;
                                const int ns = static_cast<int>(
                                    std::min<size_t>(kPathGradMaxStops, gl.stops.size()));
                                pg.stopCount = static_cast<uint8_t>(std::max(2, ns));
                                for (int si = 0; si < ns; si++) {
                                    float off = gl.stops[si].off;
                                    off = off < 0.0f ? 0.0f : (off > 1.0f ? 1.0f : off);
                                    pg.stops[si].offset = off;
                                    const uint32_t sc = gl.stops[si].color;
                                    const float r8 = (float)((sc >> 16) & 0xFFu);
                                    const float g8 = (float)((sc >> 8) & 0xFFu);
                                    const float b8 = (float)(sc & 0xFFu);
                                    const float a8 = (float)((sc >> 24) & 0xFFu);
                                    float lum2 = (0.2126f * r8 + 0.7152f * g8 + 0.0722f * b8) / 255.0f;
                                    // v0.16.54：mask-type="alpha" 时渐变色标也只贡献 alpha
                                    if (md3.alphaType == 1) lum2 = 1.0f;
                                    float av2 = (lum2 * (a8 / 255.0f) * kid3.opa);
                                    av2 = av2 < 0.0f ? 0.0f : (av2 > 1.0f ? 1.0f : av2);
                                    pg.stops[si].color =
                                        ((uint32_t)(av2 * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
                                }
                                if (ns < 2) pg.stops[1] = pg.stops[0];
                                if (gl.type == PATH_GRAD_RADIAL) {
                                    if (gl.xformRotated || std::fabs(M.b) > 1e-4f ||
                                        std::fabs(M.c) > 1e-4f)
                                        Warn("<mask id=\"" + mgid + "\"> 的径向渐变内容遇到旋转/斜切："
                                             "已近似为轴对齐椭圆（改用线性渐变）");
                                    const PathPointF gc = PathApply(M, gl.cx, gl.cy);
                                    pg.x1 = gc.x; pg.y1 = gc.y;
                                    const PathPointF gf = PathApply(M, gl.fx, gl.fy);
                                    pg.fx = gf.x; pg.fy = gf.y;
                                    pg.rx = std::fabs(gl.r) * std::sqrt(M.a * M.a + M.b * M.b);
                                    pg.ry = std::fabs(gl.r) * std::sqrt(M.c * M.c + M.d * M.d);
                                } else {
                                    const PathPointF gp1 = PathApply(M, gl.x1, gl.y1);
                                    const PathPointF gp2 = PathApply(M, gl.x2, gl.y2);
                                    pg.x1 = gp1.x; pg.y1 = gp1.y;
                                    pg.x2 = gp2.x; pg.y2 = gp2.y;
                                }
                                out = pg;
                                return true;
                            };
                            for (MaskGroup& g6 : mgroups) {
                                if (g6.shape.Empty()) continue;
                                g6.shape.RecomputeBounds();
                                PathNormData nd4;
                                if (!NormalizeShape(g6.shape, nd4)) continue;
                                PendingMask pm;
                                pm.nodeId = n.id;
                                pm.color = g6.color;
                                if (g6.gradKid) {
                                    if (buildMaskGrad(*g6.gradKid, g6.shape, pm.grad)) {
                                        pm.hasGrad = true;
                                        pm.color = 0xFFFFFFFFu;   // 亮度全交给渐变色标
                                    } else {
                                        Warn("<mask id=\"" + mgid + "\"> 的渐变色标不足 2 个"
                                             "（按白色全保留）");
                                    }
                                }
                                pm.rx = r0 + pc.ox; pm.ry = r1 + pc.oy;
                                pm.rw = rw3;        pm.rh = rh3;
                                pm.x = g6.shape.minX + pc.ox;
                                pm.y = g6.shape.minY + pc.oy;
                                pm.w = g6.shape.Width();
                                pm.h = g6.shape.Height();
                                pm.first = std::move(nd4.contourFirst);
                                pm.closed = std::move(nd4.contourClosed);
                                pm.pts = std::move(nd4.pts);
                                maskNodes.push_back(std::move(pm));
                            }
                        }
                    }
                }
            }
        }

        // ---- v0.16.27：毛玻璃（backdrop-filter: blur(Npx)，v29 毛玻璃段） ----
        // CSS 里写 backdrop-filter（含 -webkit- 前缀）→ 运行期把**背景**抓下来做多级
        // 降采样模糊，再以 replace 混合按圆角画回去；元素自己的半透明底色叠在上面。
        // 只支持 blur()：saturate()/brightness() 之类要片元级运算（着色器只做"纹理 ×
        // 顶点色"），告警忽略。
        {
            std::string bfv;
            auto bit = style.find("backdrop-filter");
            if (bit != style.end()) bfv = TrimCopy(bit->second);
            if (bfv.empty()) {
                bit = style.find("-webkit-backdrop-filter");
                if (bit != style.end()) bfv = TrimCopy(bit->second);
            }
            if (bfv.empty() && e.attrs.count("backdrop-filter"))
                bfv = TrimCopy(e.attrs.at("backdrop-filter"));
            if (!bfv.empty() && lowerStr(bfv) != "none") {
                float blurPx = 0.0f;
                float grain = -1.0f;   // <0 = 作者没写 grain
                // grain(<0..1>) 是本库的扩展记号：先单独抽出来，再从待解析串里删掉
                //（否则括号里的数字会被当成未知函数参数报"只支持 blur()"）
                {
                    const std::string scan = lowerStr(bfv);
                    size_t gp = scan.find("grain(");
                    if (gp != std::string::npos) {
                        size_t e = scan.find(')', gp);
                        if (e == std::string::npos || e <= gp + 6) {
                            grain = 1.0f;
                            bfv.erase(gp, (e == std::string::npos ? std::string::npos
                                                                 : e - gp + 1));
                        } else {
                            grain = parseLen(TrimCopy(bfv.substr(gp + 6, e - gp - 6)), 1.0f);
                            if (grain < 0.0f) grain = 0.0f;
                            if (grain > 1.0f) grain = 1.0f;
                            bfv.erase(gp, e - gp + 1);
                        }
                    } else if (scan.find("grain") != std::string::npos) {
                        // 裸写 grain = 默认强度
                        grain = 1.0f;
                        size_t gp2 = scan.find("grain");
                        bfv.erase(gp2, 5);
                    }
                }
                bool anyBlur = false, otherFn = false;
                std::string cur2;
                for (size_t k = 0; k <= bfv.size(); k++) {
                    const char ch = (k < bfv.size()) ? bfv[k] : ')';
                    if (ch == '(' || ch == ')' || ch == ' ' || ch == ',') {
                        if (!cur2.empty()) {
                            const std::string fn = lowerStr(cur2);
                            if (fn.rfind("blur", 0) == 0) anyBlur = true;
                            else if (fn != "and" && fn != "px") otherFn = true;
                            if (fn.size() > 2 && fn.back() == 'x' &&
                                fn.find("px") != std::string::npos && fn[0] >= '0' && fn[0] <= '9') {
                                // 形如 "12px"：数值 + 单位
                                float v = parseLen(fn, 0.0f);
                                blurPx = std::max(blurPx, v);
                                anyBlur = true;
                            }
                            cur2.clear();
                        }
                    } else {
                        cur2 += ch;
                    }
                }
                if (otherFn)
                    Warn("backdrop-filter 只支持 blur()（saturate/brightness 等需要片元运算，"
                         "已忽略）");
                if (anyBlur && blurPx > 0.0f) {
                    BinUIBackdropEntry be = {};
                    be.nodeId = n.id;
                    // flags：bit0 = 颗粒（grain），bit8..15 = 颗粒强度 0..255
                    be.flags = 0;
                    if (grain > 0.0f) {
                        const float g = grain > 1.0f ? 1.0f : grain;
                        be.flags = static_cast<uint16_t>(
                            1u | (static_cast<uint32_t>(g * 255.0f + 0.5f) << 8));
                    }
                    // 缩放口径：SVG 子树里按 svgX 的缩放折算（同一用户单位体系），
                    // HTML 元素按 1:1（窗口的 ui_scale 由运行期 setSize 统一缩节点，
                    // 模糊半径跟着矩形走，不需要编译期再乘一次）
                    be.blur = blurPx * (hasSvgPath ? PathScaleFactor(svgX) : 1.0f);
                    be.radius = static_cast<float>(n.radius);
                    backdropNodes.push_back(be);
                } else if (anyBlur) {
                    Warn("backdrop-filter: blur(0) 等于不模糊（已忽略）");
                }
            }
        }

        nodes.push_back(n);

        // v15 路径段：把折线存进来（节点盒已经是几何包围盒，见上面的 <path> 分支）
        if (hasSvgPath) {
            PendingPath pp;
            pp.head.nodeId = n.id;
            pp.head.flags = ((pathFill && patternFillId.empty()) ? PATH_FILL : 0) |
                            (pathStroke ? PATH_STROKE : 0) |
                            (pathEvenOdd ? PATH_EVENODD : 0) |
                            (pathHasGrad ? PATH_FILL_GRAD : 0) |
                            (pathHasStrokeGrad ? PATH_STROKE_GRAD : 0) |
                            ((pathFill && eAA) ? PATH_AA : 0);   // v19：边缘抗锯齿
            pp.hasGrad = pathHasGrad;
            if (pathHasGrad) pp.grad = pathGrad;
            pp.hasStrokeGrad = pathHasStrokeGrad;
            if (pathHasStrokeGrad) pp.strokeGrad = pathStrokeGrad;
            pp.head.fillColor = pathFillColor;
            pp.head.strokeColor = pathStrokeColor;
            pp.head.designW = svgPathShape.Width();
            pp.head.designH = svgPathShape.Height();
            pp.head.strokeW = (uint8_t)std::min(255.0f, pathStrokeW);
            pp.head.join = pathJoin;
            pp.head.cap = pathCap;
            pp.head.reserve = pathMiterLimit;   // 借空闲字节存 miter-limit（0 = 默认 4）
            PathNormData nd;
            if (NormalizeShape(svgPathShape, nd)) {
                pp.head.pointCount = (uint16_t)std::min<size_t>(65535, nd.pts.size() / 2);
                pp.head.contourCount = (uint16_t)std::min<size_t>(65535, nd.contourFirst.size());
                pp.pts = std::move(nd.pts);
                pp.first = std::move(nd.contourFirst);
                pp.closed = std::move(nd.contourClosed);
                pendingPaths.push_back(std::move(pp));
            }
        }

        // ---- v0.16.23：marker 的几何单独成一个子节点 ----
        // 挂在路径节点下（子节点天然后画 = 压在路径上），只填充不描边。
        // 三个后端一行都不用改：它仍然只是一条普通 BINUI_PATH。
        if (hasSvgPath && !svgMarkerShape.Empty()) {
            BinUINode mn = {};
            mn.id = id++;
            mn.type = BINUI_PATH;
            mn.parentId = n.id;
            mn.zIndex = n.zIndex;
            mn.flags = BINUI_VISIBLE | BINUI_ABSOLUTE;
            mn.x = static_cast<int32_t>(std::lround(pc.ox + svgMarkerShape.minX));
            mn.y = static_cast<int32_t>(std::lround(pc.oy + svgMarkerShape.minY));
            mn.width = static_cast<int32_t>(std::lround(std::max(1.0f, svgMarkerShape.Width())));
            mn.height = static_cast<int32_t>(std::lround(std::max(1.0f, svgMarkerShape.Height())));
            nodes.push_back(mn);
            PendingPath mp;
            mp.head.nodeId = mn.id;
            mp.head.flags = PATH_FILL;      // 只填充：marker 的形状是"实心块"
            mp.head.fillColor = svgMarkerFill;
            mp.head.designW = svgMarkerShape.Width();
            mp.head.designH = svgMarkerShape.Height();
            mp.head.strokeW = 0;
            mp.head.join = BINUI_JOIN_ROUND;
            mp.head.cap = BINUI_CAP_BUTT;
            PathNormData mnd;
            if (NormalizeShape(svgMarkerShape, mnd)) {
                mp.head.pointCount = (uint16_t)std::min<size_t>(65535, mnd.pts.size() / 2);
                mp.head.contourCount = (uint16_t)std::min<size_t>(65535, mnd.contourFirst.size());
                mp.pts = std::move(mnd.pts);
                mp.first = std::move(mnd.contourFirst);
                mp.closed = std::move(mnd.contourClosed);
                pendingPaths.push_back(std::move(mp));
            } else {
                nodes.pop_back();
                id--;
            }
        }


        // ---- v0.16.25：pattern 平铺展开 ----
        // 同色同笔的 tile 合成**一个**路径节点：一块一个节点的话，几十块就是几十次
        // "进图层 → 擦除 → 出图层"，而图层池只有 4 层、视频内存也按层算。
        // 每个展开节点都挂一条"元素自身轮廓"的裁剪记录（复用 v27 的补集擦除），
        // 于是任意形状都能被图案填满而不溢出。
        if (hasSvgPath && !patternFillId.empty()) {
            auto pit = svgPatterns.find(patternFillId);
            const bool defOk = pit != svgPatterns.end() && pit->second.found &&
                               !pit->second.kids.empty();
            const float ubw = patUBw > 0.0f ? patUBw : 1.0f;
            const float ubh = patUBh > 0.0f ? patUBh : 1.0f;
            float tw = 0.0f, th = 0.0f, gx = 0.0f, gy = 0.0f;
            if (defOk) {
                const SvgPatternDef& pd2 = pit->second;
                if (pd2.units == 1) {
                    tw = pd2.w * ubw; th = pd2.h * ubh;
                    gx = patUBx + pd2.x * ubw;
                    gy = patUBy + pd2.y * ubh;
                } else {
                    tw = pd2.w; th = pd2.h; gx = pd2.x; gy = pd2.y;
                }
            }
            if (!defOk) {
                Warn("fill=\"url(#" + patternFillId + ")\" 的 <pattern> 无内容（元素不填充）");
            } else if (tw <= 0.0f || th <= 0.0f) {
                Warn("<pattern id=\"" + patternFillId + "\"> 的 width/height 必须大于 0"
                     "（元素不填充）");
            } else {
                const SvgPatternDef& pd2 = pit->second;
                bool fallback = false;      // true = 退回"第一种颜色实填"
                // v0.16.52：patternTransform **整份**生效。以前只吃"平移 + 缩放"，
                // 旋转/斜切按轴对齐近似（旋转过的 tile 被画成轴对齐，斜线纹理整个歪掉）。
                //
                // 两种情形分开放，是为了让**老页面逐位不变**：
                //   · 轴对齐（b = c = 0）：网格仍按老办法在**用户空间**枚举
                //     （格原点先过 T、步长乘 a/d）；
                //   · 有旋转/斜切：平铺网格是一个**平行四边形格**，枚举改到 "tile 空间"
                //     （= 施加 patternTransform 之前的坐标系）做 —— 把元素包围盒的四角
                //     用 T⁻¹ 变换回去取 aabb 就是枚举范围，每块再整体乘 T 回到用户空间。
                PathMatrix tf, tfInv;
                bool rotated = false;
                if (pd2.hasTf) {
                    tf = pd2.tf;
                    if (!PathInvert(tf, tfInv)) {
                        Warn("<pattern id=\"" + patternFillId + "\"> 的 patternTransform "
                             "不可逆（行列式为 0），图案不填充");
                        fallback = true;
                    } else {
                        rotated = std::fabs(tf.b) > 1e-6f || std::fabs(tf.c) > 1e-6f;
                        if (!rotated) {
                            const PathPointF go = PathApply(tf, gx, gy);
                            gx = go.x; gy = go.y;
                            tw *= tf.a; th *= tf.d;
                        }
                    }
                }
                // 斜切下圆笔变成椭圆，而路径段只有一个 strokeW → 只能按几何平均宽度画。
                // 旋转/等比缩放是**精确**的（相似变换），所以只在非相似时告警。
                if (rotated && !PathIsSimilarity(tf)) {
                    bool strokeKid = false;
                    for (const SvgPatternChild& kid : pd2.kids)
                        if (kid.hasStroke && kid.strokeW > 0.0f) strokeKid = true;
                    if (strokeKid)
                        Warn("<pattern id=\"" + patternFillId + "\"> 的 patternTransform 带斜切："
                             "描边按几何平均宽度画（SVG 规范是椭圆笔，路径段只存一个线宽）");
                }
                // 枚举范围：轴对齐用绝对步长（老口径）；旋转/斜切在 tile 空间取元素包围盒的 aabb
                const float gxT = gx, gyT = gy;                         // 格原点（tile 空间）
                const float twT = std::fabs(tw), thT = std::fabs(th);   // 格步长（tile 空间）
                float bx0 = patUBx, by0 = patUBy;
                float bx1 = patUBx + ubw, by1 = patUBy + ubh;
                if (rotated) {
                    float mx0 = 1e30f, my0 = 1e30f, mx1 = -1e30f, my1 = -1e30f;
                    const float cx4[4] = { patUBx, patUBx + ubw, patUBx + ubw, patUBx };
                    const float cy4[4] = { patUBy, patUBy, patUBy + ubh, patUBy + ubh };
                    for (int ci = 0; ci < 4; ci++) {
                        const PathPointF q4 = PathApply(tfInv, cx4[ci], cy4[ci]);
                        mx0 = std::min(mx0, q4.x); mx1 = std::max(mx1, q4.x);
                        my0 = std::min(my0, q4.y); my1 = std::max(my1, q4.y);
                    }
                    bx0 = mx0; bx1 = mx1; by0 = my0; by1 = my1;
                }
                const float txa = rotated ? (twT > 0.0f ? twT : 1.0f) : std::fabs(tw);
                const float tya = rotated ? (thT > 0.0f ? thT : 1.0f) : std::fabs(th);
                const int k0 = (int)std::floor((bx0 - gxT) / txa);
                const int k1 = (int)std::ceil((bx1 - gxT) / txa);
                const int l0 = (int)std::floor((by0 - gyT) / tya);
                const int l1 = (int)std::ceil((by1 - gyT) / tya);
                // 一块 tile 落在哪、要不要画：**同一份判定**给"预算估计"和"真正展开"用。
                // 旋转/斜切时枚举范围是 T⁻¹ 之后的 aabb（比实际覆盖的区域大 √2 倍量级），
                // 所以预算必须数**真的会画的块**，否则斜着的图案会被误判成"太大"而退回纯色
                // （第一版就是这样：rotate(30) 的 178 块被估成 420 块，直接降级）。
                auto tileAt = [&](int kk2, int ll2, float& ox2, float& oy2) -> bool {
                    ox2 = gxT + (float)kk2 * txa;
                    oy2 = gyT + (float)ll2 * tya;
                    if (rotated) {
                        // 平行四边形格：算这一块在用户空间的外接 aabb 再和元素盒比
                        float ux0 = 1e30f, uy0 = 1e30f, ux1 = -1e30f, uy1 = -1e30f;
                        const float corx[4] = { ox2, ox2 + txa, ox2 + txa, ox2 };
                        const float cory[4] = { oy2, oy2, oy2 + tya, oy2 + tya };
                        for (int ci = 0; ci < 4; ci++) {
                            const PathPointF q5 = PathApply(tf, corx[ci], cory[ci]);
                            ux0 = std::min(ux0, q5.x); ux1 = std::max(ux1, q5.x);
                            uy0 = std::min(uy0, q5.y); uy1 = std::max(uy1, q5.y);
                        }
                        if (ux1 <= patUBx || uy1 <= patUBy ||
                            ux0 >= patUBx + ubw || uy0 >= patUBy + ubh) return false;
                        return true;
                    }
                    // 整块落在元素包围盒外 → 裁掉也看不见
                    return !(ox2 + txa <= patUBx || oy2 + tya <= patUBy ||
                             ox2 >= patUBx + ubw || oy2 >= patUBy + ubh);
                };
                bool warnedPatStrokeClip = false;   // 只告警一次
                long long ntiles = 0;
                for (int kk2 = k0; kk2 <= k1; kk2++)
                    for (int ll2 = l0; ll2 <= l1; ll2++) {
                        float ax = 0.0f, ay = 0.0f;
                        if (tileAt(kk2, ll2, ax, ay)) ntiles++;
                    }
                size_t ptsPerTile = 0;
                for (const SvgPatternChild& kid : pd2.kids) ptsPerTile += kid.shape.pts.size();
                const long long kMaxTiles = 400;
                const long long kMaxPts = 60000;
                if (!fallback && (ntiles > kMaxTiles ||
                                  (long long)ptsPerTile * ntiles > kMaxPts)) {
                    Warn("<pattern id=\"" + patternFillId + "\"> 平铺展开太大（" +
                         std::to_string(ntiles) + " 块 × " + std::to_string(ptsPerTile) +
                         " 点），退回用第一种颜色实填（把 tile 调大或平铺区域调小）");
                    fallback = true;
                }
                struct PatGroup {
                    bool stroke = false;
                    uint32_t color = 0;
                    float w = 0.0f;
                    uint8_t cap = BINUI_CAP_BUTT, join = BINUI_JOIN_ROUND;
                    PathShape shape;
                };
                std::vector<PatGroup> groups;
                auto groupOf = [&](const SvgPatternChild& kid, bool stroke) -> size_t {
                    const uint32_t col = stroke ? kid.stroke : kid.fill;
                    for (size_t gi2 = 0; gi2 < groups.size(); gi2++) {
                        const PatGroup& g = groups[gi2];
                        if (g.stroke != stroke || g.color != col) continue;
                        if (stroke && (std::fabs(g.w - kid.strokeW) > 0.01f ||
                                       g.cap != kid.cap || g.join != kid.join)) continue;
                        return gi2;
                    }
                    PatGroup g;
                    g.stroke = stroke;
                    g.color = col;
                    if (stroke) { g.w = kid.strokeW; g.cap = kid.cap; g.join = kid.join; }
                    groups.push_back(g);
                    return groups.size() - 1;
                };
                auto appendTo = [](PathShape& dst, const PathShape& src2) {
                    const uint32_t base2 = (uint32_t)dst.pts.size();
                    for (const PathPointF& q3 : src2.pts) dst.pts.push_back(q3);
                    for (const PathShape::Contour& sc2 : src2.contours) {
                        PathShape::Contour nc2;
                        nc2.first = base2 + sc2.first;
                        nc2.count = sc2.count;
                        nc2.closed = sc2.closed;
                        dst.contours.push_back(nc2);
                    }
                };
                float strokeK = 1.0f;   // 内容用户单位 → 设备像素
                if (fallback) {
                    PatGroup g;
                    g.stroke = false;
                    g.color = pd2.kids[0].hasFill ? pd2.kids[0].fill : pd2.kids[0].stroke;
                    g.shape = svgPathShape;
                    groups.push_back(g);
                } else {
                    // 内容映射：objectBoundingBox 缩放 · viewBox（meet 居中）
                    PathMatrix cb;
                    if (pd2.contentUnits == 1) cb = PathScale(txa, tya);
                    if (pd2.hasViewBox) {
                        const float kk = std::min(txa / pd2.vbW, tya / pd2.vbH);
                        PathMatrix vb = PathMul(PathScale(kk, kk),
                                                PathTranslate(-pd2.vbX, -pd2.vbY));
                        vb.e += (txa - pd2.vbW * kk) * 0.5f;
                        vb.f += (tya - pd2.vbH * kk) * 0.5f;
                        cb = PathMul(vb, cb);
                    }
                    strokeK = PathScaleFactor(svgX) * PathScaleFactor(cb);
                    if (rotated) strokeK *= PathScaleFactor(tf);   // det 可乘：面积比相乘
                    for (int kk2 = k0; kk2 <= k1; kk2++) {
                        for (int ll2 = l0; ll2 <= l1; ll2++) {
                            float ox2 = 0.0f, oy2 = 0.0f;
                            if (!tileAt(kk2, ll2, ox2, oy2)) continue;
                            const PathMatrix md = rotated
                                ? PathMul(svgX, PathMul(tf, PathMul(PathTranslate(ox2, oy2), cb)))
                                : PathMul(svgX, PathMul(PathTranslate(ox2, oy2), cb));
                            // v0.17.2：内容的 tile 框（设备空间，凸）。内容的变换是
                            // svgX·[tf]·T(格原点)·cb，而 cb 已经把内容映射进 (0,0)-(txa,tya)，
                            // 所以 tile 框就是"不含 cb 的那半"作用在这个矩形上的像 ——
                            // 轴对齐时是矩形，旋转/斜切时是平行四边形，两者都凸。
                            const PathMatrix tileM = rotated
                                ? PathMul(svgX, PathMul(tf, PathTranslate(ox2, oy2)))
                                : PathMul(svgX, PathTranslate(ox2, oy2));
                            const PathPointF win[4] = {
                                PathApply(tileM, 0.0f, 0.0f),
                                PathApply(tileM, txa, 0.0f),
                                PathApply(tileM, txa, tya),
                                PathApply(tileM, 0.0f, tya) };
                            for (const SvgPatternChild& kid : pd2.kids) {
                                PathShape tmp = kid.shape;
                                TransformShape(tmp, md);
                                if (tmp.Empty()) continue;
                                // 只对**真的越界**的内容裁：正常写法（内容不越出 tile）像素
                                // 一位不变。越界 + 有描边时告警 —— 几何裁剪会在 tile 边界上
                                // 多画一条线（规范要的是"渲染后裁剪"，代价见 docs/svg.md 14.3）。
                                if (!ShapeInsideConvex(tmp, win)) {
                                    if (kid.hasStroke && kid.strokeW > 0.0f && !warnedPatStrokeClip) {
                                        Warn("<pattern id=\"" + patternFillId +
                                             "\"> 的内容越出 tile 框且带描边：几何裁剪会在 tile "
                                             "边界上多画一条线（见 docs/svg.md 14.3）");
                                        warnedPatStrokeClip = true;
                                    }
                                    ClipShapeConvex(tmp, win);
                                    if (tmp.Empty()) continue;
                                }
                                if (kid.hasFill) appendTo(groups[groupOf(kid, false)].shape, tmp);
                                if (kid.hasStroke && kid.strokeW > 0.0f)
                                    appendTo(groups[groupOf(kid, true)].shape, tmp);
                            }
                        }
                    }
                }
                // 元素轮廓：归一化一次，复制给每个展开节点（运行期补集擦除要闭包）
                PendingClip clipTmpl;
                bool hasClip = false;
                {
                    PathShape outline = svgPathShape;
                    outline.RecomputeBounds();
                    PathNormData cnd2;
                    if (!outline.Empty() && NormalizeShape(outline, cnd2)) {
                        clipTmpl.flags = pathEvenOdd ? 1 : 0;
                        clipTmpl.x = outline.minX + pc.ox;
                        clipTmpl.y = outline.minY + pc.oy;
                        clipTmpl.w = outline.Width();
                        clipTmpl.h = outline.Height();
                        clipTmpl.first = std::move(cnd2.contourFirst);
                        clipTmpl.closed = std::move(cnd2.contourClosed);
                        clipTmpl.pts = std::move(cnd2.pts);
                        hasClip = true;
                    }
                }
                for (PatGroup& g : groups) {
                    if (g.shape.Empty()) continue;
                    g.shape.RecomputeBounds();
                    PathNormData nd3;
                    if (!NormalizeShape(g.shape, nd3)) continue;
                    BinUINode mn = {};
                    mn.id = id++;
                    mn.type = BINUI_PATH;
                    mn.parentId = n.id;
                    mn.zIndex = n.zIndex;
                    mn.flags = BINUI_VISIBLE | BINUI_ABSOLUTE;
                    mn.x = static_cast<int32_t>(std::lround(pc.ox + g.shape.minX));
                    mn.y = static_cast<int32_t>(std::lround(pc.oy + g.shape.minY));
                    mn.width = static_cast<int32_t>(std::lround(std::max(1.0f, g.shape.Width())));
                    mn.height = static_cast<int32_t>(std::lround(std::max(1.0f, g.shape.Height())));
                    nodes.push_back(mn);
                    PendingPath pp2;
                    pp2.head.nodeId = mn.id;
                    pp2.head.flags = (g.stroke ? PATH_STROKE : PATH_FILL) |
                                     ((eAA && !g.stroke) ? PATH_AA : 0);
                    pp2.head.fillColor = g.color;
                    pp2.head.strokeColor = g.color;
                    pp2.head.designW = g.shape.Width();
                    pp2.head.designH = g.shape.Height();
                    pp2.head.strokeW = (uint8_t)std::min(255.0f, g.w * strokeK);
                    pp2.head.join = g.join;
                    pp2.head.cap = g.cap;
                    pp2.head.pointCount = (uint16_t)std::min<size_t>(65535, nd3.pts.size() / 2);
                    pp2.head.contourCount = (uint16_t)std::min<size_t>(65535, nd3.contourFirst.size());
                    pp2.pts = std::move(nd3.pts);
                    pp2.first = std::move(nd3.contourFirst);
                    pp2.closed = std::move(nd3.contourClosed);
                    pendingPaths.push_back(std::move(pp2));
                    if (hasClip) {
                        PendingClip cp2 = clipTmpl;
                        cp2.nodeId = mn.id;
                        clipNodes.push_back(std::move(cp2));
                    }
                }
            }
        }

        // v8 名称表：HTML id 属性 -> 节点名（运行时 findNodeByName 查找）
        if (e.attrs.count("id") && !e.attrs.at("id").empty())
            nodeNames.push_back({ n.id, getOrAddString(e.attrs.at("id")) });

        // v7 动画/过渡：内联 style（或 XML 属性）+ CSS 规则（规则覆盖内联）
        {
            std::string sAnim = pick(e.attrs, style, "animation", "animation", "");
            if (sAnim.empty()) sAnim = style.count("animation") ? style.at("animation") : "";
            std::string sTrans = pick(e.attrs, style, "transition", "transition", "");
            if (sTrans.empty()) sTrans = style.count("transition") ? style.at("transition") : "";
            sAnim = cssAnimFor(e, sAnim);
            sTrans = cssTransFor(e, sTrans);
            if (!sTrans.empty()) AnimParseTransitions(sTrans, n.id, transitions);
            if (!sAnim.empty()) {
                BinUIAnimDef d;
                std::memset(&d, 0, sizeof(d));
                d.nodeId = n.id;
                d.easing = BINUI_EASE_EASE;
                std::string name;
                if (AnimParseAnimation(sAnim, name, d)) {
                    for (size_t gi = 0; gi < kfGroups.size(); gi++) {
                        if (kfGroups[gi].first != name) continue;
                        d.groupIdx = (uint16_t)gi;
                        uint16_t gm = 0;
                        for (const auto& kf : kfGroups[gi].second) gm |= kf.propMask;
                        d.propMask = gm;
                        animDefs.push_back(d);
                        break;
                    }
                }
            }
        }

        // v11 状态样式：CSS 运行时伪类（:hover/:active/:focus/:checked）→ 状态段。
        // 渲染器在指针/焦点/勾选状态变化时把这些目标值应用到节点（声明了 transition
        // 的节点走现成的过渡通道，否则瞬时切换）。颜色口径与基础样式一致：
        // LABEL 的 color 是文字色，其它节点只有背景色字段。
        if (!cssRules.empty() && !e.attrs.count("_pseudo")) {
            static const uint8_t kStateBits[4] = { BINUI_ST_HOVER, BINUI_ST_ACTIVE,
                                                   BINUI_ST_FOCUS, BINUI_ST_CHECKED };
            for (uint8_t sb : kStateBits) {
                std::unordered_map<std::string, std::string> merged;
                std::unordered_map<std::string, int> specOf;
                for (const auto& r : cssRules) {
                    if (!r.pseudoEl.empty()) continue;
                    if (!CssHasStatePseudo(r, sb)) continue;   // 只看声明了该状态的规则
                    if (!CssRuleMatchesDom(r, elems, cssKids, elemCtx, ei, sb)) continue;
                    for (const auto& kv : r.decls) {
                        auto sit = specOf.find(kv.first);
                        if (sit != specOf.end() && r.spec < sit->second) continue;
                        merged[kv.first] = kv.second;
                        specOf[kv.first] = r.spec;
                    }
                }
                if (merged.empty()) continue;
                BinUIStateStyle ss = {};
                ss.nodeId = n.id;
                ss.state = sb;
                ss.scale = 1.0f;
                ss.radius = -1.0f;
                std::string col = merged.count("background") ? merged["background"] : "";
                if (col.empty() && merged.count("background-color")) col = merged["background-color"];
                if (col.empty() && merged.count("bg")) col = merged["bg"];
                if (col.empty() && type == BINUI_LABEL) {
                    if (merged.count("text-color")) col = merged["text-color"];
                    else if (merged.count("color")) col = merged["color"];
                    // LABEL 已经有底色时，文字色固化在扩展绘制段的 textColor 上，
                    // 状态段只能改"节点颜色"（即底色）—— 别让作者以为改了文字色
                    if (!labelLegacy && !col.empty() &&
                        !merged.count("background") && !merged.count("background-color") &&
                        !merged.count("bg"))
                        Warn("LABEL 同时有底色与文字色时，状态伪类里的 color 改的是底色"
                             "（文字色已固化在扩展绘制段）；要改文字色请给该状态声明 background 或改用其它元素");
                }
                if (!col.empty()) {
                    uint32_t c = 0;
                    if (ParseColorValue(col, &c, "状态样式颜色")) {
                        float op = 1.0f;
                        if (merged.count("opacity")) {
                            op = parseLen(merged["opacity"], 1.0f, "opacity");
                            if (op < 0) op = 0;
                            if (op > 1) op = 1;
                        }
                        const uint32_t a = (uint32_t)(((c >> 24) & 0xFFu) * op + 0.5f);
                        ss.color = (a << 24) | (c & 0x00FFFFFFu);
                        ss.fields |= BINUI_STF_COLOR;
                    }
                }
                // transform: scale(N) —— 只认 scale()，其它变换没有对应通道
                if (merged.count("transform")) {
                    const std::string tv = LowerCopy(merged["transform"]);
                    const size_t sc = tv.find("scale(");
                    if (sc != std::string::npos) {
                        std::string num;
                        for (size_t q = sc + 6;
                             q < tv.size() && (std::isdigit((unsigned char)tv[q]) || tv[q] == '.' || tv[q] == '-');
                             q++)
                            num += tv[q];
                        if (!num.empty()) {
                            try { ss.scale = std::stof(num); ss.fields |= BINUI_STF_SCALE; } catch (...) {}
                        }
                    } else {
                        Warn("CSS 状态样式 transform 只支持 scale()，其它变换（translate/rotate…）已忽略");
                    }
                }
                if (merged.count("border-radius")) {
                    const float r = parseLen(merged["border-radius"], 0, "border-radius");
                    if (r >= 0) { ss.radius = r; ss.fields |= BINUI_STF_RADIUS; }
                }
                if (ss.fields) stateStyles.push_back(ss);
            }
        }

        // 缩放规格：setSize 重排依据
        BinUISpecNode sp = {};
        sp.nodeId = n.id;
        sp.pctX = pctX; sp.pctY = pctY; sp.pctW = pctW; sp.pctH = pctH;
        if (pctX >= 0) sp.flags |= SPEC_PCT_X;
        if (pctY >= 0) sp.flags |= SPEC_PCT_Y;
        if (pctW >= 0) sp.flags |= SPEC_PCT_W;
        if (pctH >= 0) sp.flags |= SPEC_PCT_H;
        if (flexPlaced) {
            sp.flags |= SPEC_IN_FLEX;
            // 未声明宽度：column flex 下运行时按交叉轴拉伸到内容盒（SPEC_AUTO_W 供重排使用）
            if (!hasW) sp.flags |= SPEC_AUTO_W;
            auto wIt = flexProps.find(e.parentIdx);
            if (wIt != flexProps.end()) {
                auto cIt = wIt->second.find(ei);
                if (cIt != wIt->second.end()) {
                    const FlexRec& fr = cIt->second;
                    sp.flexWeight = (uint8_t)std::lround(std::clamp(fr.grow, 0.0f, 255.0f));
                    sp.flexShrink = (uint8_t)std::lround(std::clamp(fr.shrink, 0.0f, 255.0f));
                    sp.alignSelf  = fr.alignSelf;
                    sp.flexOrder  = fr.order;
                    sp.basisKind  = fr.basisKind;
                    sp.flexBasis  = fr.basisVal;
                    sp.flexCross  = fr.crossPx;
                    if (fr.crossAuto) sp.flags |= SPEC_CROSS_AUTO;
                    if (fr.content)   sp.flags |= SPEC_BASIS_CONTENT;
                }
            }
        } else {
            if (!hasY) sp.flags |= SPEC_FLOW_Y;
            if (!hasW) sp.flags |= SPEC_AUTO_W;
        }
        sp.relX = x - cb.ox;
        sp.relY = y - cb.oy;
        // v30：贴右/贴下 —— 静态布局已经在上面算好落位，这两位是给运行期 resize 重排用的
        // （没有它们，窗口变宽后"贴右"的元素会留在左边）。口径与编译期完全一致。
        if (isFixed) sp.anchorFlags |= ANCHOR_FIXED;   // 重排的含定位盒 = 视口
        // v0.16.39：sticky 的 top 偏移借 anchorB 存（它本来是 absolute 的 bottom，
        // 同一个节点不可能两者兼有）。运行期钳制见 binui_paint.h 的 PaintStickyShiftY。
        if (isSticky && hasStickyTop) {
            sp.anchorB = (stickyTopPct >= 0) ? stickyTopPct : stickyTopPx;
            sp.anchorFlags |= ANCHOR_STICKY | ANCHOR_B;
            if (stickyTopPct >= 0) sp.anchorFlags |= ANCHOR_B_PCT;
        }
        if (useRight) {
            sp.anchorR = (pctR >= 0) ? pctR : rightPx;
            sp.flags |= SPEC_ANCHOR_R;
            if (pctR >= 0) sp.flags |= SPEC_ANCHOR_R_PCT;
        }
        if (useBottom) {
            sp.anchorB = (pctB >= 0) ? pctB : bottomPx;
            sp.anchorFlags |= ANCHOR_B;
            if (pctB >= 0) sp.anchorFlags |= ANCHOR_B_PCT;
        }
        sp.ml = ml; sp.mr = mr; sp.mt = mt; sp.mb = mb;
        sp.minW = mnW; sp.maxW = mxW; sp.minH = mnH; sp.maxH = mxH;
        if (mnWp) sp.flags |= SPEC_MINW_PCT;
        if (mxWp) sp.flags |= SPEC_MAXW_PCT;
        if (mnHp) sp.flags |= SPEC_MINH_PCT;
        if (mxHp) sp.flags |= SPEC_MAXH_PCT;
        sp.relW = (!flexPlaced && hasW && pctW < 0) ? baseW : -1;
        sp.relH = (!flexPlaced && !sh.empty() && pctH < 0) ? baseH : -1;
        // 线段：width/height 是"终点绝对坐标"，重排规格必须存**起点偏移 + 端点增量**，
        // 否则 relayout 会把终点当尺寸，父容器一挪线段就被拉成一条横贯窗口的直线。
        // 升级成路径的 <line> 不算：它是普通路径节点，盒子就是包围盒。
        if (e.tag == "line" && !hasSvgPath) {
            sp.relX = svgLineX1;
            sp.relY = svgLineY1;
            sp.relW = svgLineX2 - svgLineX1;
            sp.relH = svgLineY2 - svgLineY1;
        }
        // 元素级 ui_scale="fixed"：这个子树不参与窗口的等比缩放（标题栏这类 chrome 用）
        if (lowerStr(pick(e.attrs, style, "ui_scale", "ui_scale", "")) == "fixed")
            sp.flags |= SPEC_FIXED_SCALE;
        // 高度是编译期按文本估算撑开的：运行期 setText 换文本后需要重算（issue 06 的第 1 步）
        if (heightAuto) { sp.heightAuto = 1; sp.autoBase = autoBase; }
        specNodes.push_back(sp);

        // 容器上下文：内容盒 = 自身位置 + padding
        // display:flex 容器：先为全部子项做 flex 布局（auto 高度时扩展容器）
        std::string disp = lowerStr(pick(e.attrs, style, "display", "display", ""));
        if (disp == "flex") {
            std::string dir  = lowerStr(style.count("flex-direction") ? style.at("flex-direction") : "row");
            std::string just = lowerStr(style.count("justify-content") ? style.at("justify-content") : "flex-start");
            std::string algn = lowerStr(style.count("align-items") ? style.at("align-items") : "flex-start");
            std::string algnC = lowerStr(style.count("align-content") ? style.at("align-content") : "flex-start");
            std::string wrp  = lowerStr(style.count("flex-wrap") ? style.at("flex-wrap") : "nowrap");
            uint8_t wrap = (wrp == "wrap" || wrp == "wrap-reverse") ? 1 : 0;
            if (wrp == "wrap-reverse") wrap = 2;
            float gap = 0;
            auto git = style.find("gap");
            if (git != style.end()) gap = parseLen(git->second, 0);

            // 本元素是不是"column 父级里带 flex:N 的主轴项"——是的话高度已由父级分配，
            // 这里不能再用内容高度盖掉（见下面 auto 高度那段注释）。
            bool flexedMainAxis = false;
            if (auto pw = flexProps.find(e.parentIdx); pw != flexProps.end()) {
                auto wi = pw->second.find(ei);
                flexedMainAxis = (wi != pw->second.end() && wi->second.grow > 0 &&
                                  columnFlexParents.count(e.parentIdx) > 0);
            }

            std::unordered_map<size_t, FlexRec> recs;
            std::unordered_map<size_t, float[4]> pos;
            // 行容器的交叉轴（高）是不是"确定"的：没写 height 且高度没被父级 flex 分配时，
            // 高度是这个容器自己按内容撑出来的 —— 单行对齐不能按它算（见 binui_flex_layout.h）
            const bool containerAutoH = sh.empty() && !flexedMainAxis;
            const bool crossDefinite = (dir == "column") ? true : !containerAutoH;
            float need = layoutFlex(childrenOf[ei], w - 2 * pad, lh - 2 * pad,
                                    dir, just, algn, algnC, wrap, gap, crossDefinite, pos, recs);
            flexLayouts[ei] = std::move(pos);
            flexProps[ei] = std::move(recs);

            // column 方向要记下来：它的 flex:N 子项高度是父级剩余空间定死的
            if (dir == "column") columnFlexParents.insert(ei);

            // auto 高度（未声明 height）：内容盒高度 = flex 交叉轴需求。
            // 但**column 下的弹性子项例外**：它的主轴高度已经由父级 flexProps 分配好
            // （就是上面 lh），这里再按内容撑一次会把 600 改成内容高（比如 40），
            // 而父级/兄弟的位置仍按 600 排 —— 表现为"撑满剩余高度的容器塌成一条、
            // 后面的兄弟压上来"，`flex:1` + `overflow:auto` 的滚动视口更是直接消失。
            if (sh.empty() && !flexedMainAxis) {
                BinUINode& cn = nodes.back();
                cn.height = need + 2 * pad;
                // auto 高同样受 min/max-height 约束钳制
                if (effMnH >= 0 && cn.height < effMnH) cn.height = effMnH;
                if (effMxH >= 0 && cn.height > effMxH) cn.height = effMxH;
                // 流式父级的**光标早就在前面按旧高度推进过了**（那时还不知道 flex 要折几行）。
                // 这里不补差值，后面的兄弟就会压到换行后的第二行上 —— flex-wrap 的经典表现。
                if (!hasY) pc.cursorY += cn.height - lh;
            }

            BinUISpecFlex fs = {};
            fs.nodeId = n.id;
            fs.dir = dir == "column" ? 1 : 0;
            fs.justify = just == "flex-end" ? 1 : just == "center" ? 2
                       : just == "space-between" ? 3 : just == "space-around" ? 4
                       : just == "space-evenly" ? 5 : 0;
            fs.align = algn == "center" ? 1 : (algn == "flex-end" || algn == "end") ? 2
                     : algn == "stretch" ? 3 : 0;
            fs.autoH = containerAutoH ? 1 : 0;
            fs.wrap = wrap;
            fs.alignContent = algnC == "center" ? 1 : (algnC == "flex-end" || algnC == "end") ? 2
                            : algnC == "space-between" ? 3 : algnC == "space-around" ? 4
                            : algnC == "space-evenly" ? 5 : algnC == "stretch" ? 6 : 0;
            fs.gap = gap;
            specFlex.push_back(fs);
        }

        // push_back 之后还要用父上下文这几个字段 → 先拷出来。栈已经 reserve 过，
        // 但别让正确性依赖"不会扩容"：pc 是 stack.back() 的引用，扩容即悬空。
        const float pcOx = pc.ox, pcOy = pc.oy, pcContentW = pc.contentW;
        const float pcVpW = pc.vpW, pcVpH = pc.vpH;
        stack.push_back({ n.id, x + pad, y + pad, 0, w - 2 * pad, nodes.size() - 1,
                          textCol, hasOwnText || pc.hasText, !hasY && !flexPlaced,
                          nodeLetterSpacing });
        {
            // SVG 上下文：坐标系与颜料继承往下传
            Ctx& child = stack.back();
            child.inSvg = inSvg;
            child.fillColor = eFill;
            child.hasFill = eHasFill;
            child.strokeColor = eStroke;
            child.hasStroke = eHasStroke;
            child.strokeW = eStrokeW;
            child.evenOdd = eEvenOdd;
            child.aa = eAA;
            child.fillMentioned = eFillMentioned;   // fill（含 none）的"提到过"也沿树继承
            child.fillGradId = fillGradId;   // 渐变填充是继承属性（<svg>/<g> 写一次全子树生效）
            child.strokeGradId = strokeGradId;   // 渐变描边同理（v17）
            // v24：文字阴影同样是继承属性，往整棵子树传（写的 none 已在上面清成空）
            child.tsLayers = tsLayers;
            if (e.tag == "svg") {
                // 子元素坐标 = viewBox 用户单位 → 本容器内容盒局部坐标。
                // 注意**不乘位移**：Ctx.ox 已经是内容盒原点，再加一次就是双重偏移
                // （踩过：节点盒从 8.3 变成 18.3，图形整体右下漂 10px）。
                PathMatrix base = svgX;                       // = pc.svgXform（自己的 transform 还没乘）
                if (hasOwnXform) base = PathMul(base, svgOwn); // 整个 svg 组一起变换
                std::string vbs = e.attrs.count("viewbox") ? e.attrs.at("viewbox") : "";
                float vbx = 0, vby = 0, vbw = 0, vbh = 0;
                // 视口尺寸（用户单位）：userSpaceOnUse 的百分比按它折算。
                // 有 viewBox 就是 viewBox 的宽高；没有就退回声明的 width/height。
                if (!vbs.empty() && ParseViewBox(vbs, vbx, vby, vbw, vbh) && vbw > 0 && vbh > 0) {
                    child.vpW = vbw;
                    child.vpH = vbh;
                } else {
                    child.vpW = parseLen(sw, w);
                    child.vpH = parseLen(sh, lh);
                }
                if (!vbs.empty()) {
                    if (ParseViewBox(vbs, vbx, vby, vbw, vbh)) {
                        PathMatrix vb;
                        // 内容盒用**设备尺寸**：节点盒已经过父变换（嵌套 svg 时是缩放后的 px），
                        // viewBox 映射到这个设备盒就是对的；若用声明的用户尺寸会少算一次缩放。
                        const float boxW = std::max(0.0f, w - 2.0f * pad);
                        const float boxH = std::max(0.0f, lh - 2.0f * pad);
                        const std::string par = e.attrs.count("preserveaspectratio")
                                                ? e.attrs.at("preserveaspectratio") : "";
                        if (BuildViewBoxMatrix(vbx, vby, vbw, vbh, boxW, boxH, par, vb)) {
                            base = PathMul(base, vb);
                            // 9 宫格对齐 + meet/slice + 可选 defer 前缀都由 BuildViewBoxMatrix 解析；
                            // 这里只把"认不出的值"点出来，免得作者以为写了就生效
                            const std::string lp = lowerStr(par);
                            bool badTok = false;
                            std::string tok;
                            for (size_t ci = 0; ci <= lp.size(); ci++) {
                                const char ch = ci < lp.size() ? lp[ci] : ' ';
                                if (ch != ' ' && ch != '\t' && ch != ',') { tok += ch; continue; }
                                if (tok.empty()) continue;
                                bool okTok = (tok == "none" || tok == "defer" ||
                                              tok == "meet" || tok == "slice");
                                static const char* kAlign[] = {
                                    "xminymin", "xmidymin", "xmaxymin",
                                    "xminymid", "xmidymid", "xmaxymid",
                                    "xminymax", "xmidymax", "xmaxymax" };
                                for (const char* a2 : kAlign) if (tok == a2) okTok = true;
                                if (!okTok) badTok = true;
                                tok.clear();
                            }
                            if (badTok)
                                Warn("preserveAspectRatio=\"" + par + "\" 里有认不出的值，"
                                     "已按默认 xMidYMid meet 处理（可选 none | "
                                     "xMinYMin … xMaxYMax | meet | slice）");
                        }
                    } else {
                        Warn("viewBox 解析失败（需要 4 个数：minX minY width height）");
                    }
                }
                child.svgXform = base;
            } else if (e.tag == "g") {
                // <g> 不建立坐标系：子元素与 g 共用父容器原点与内容宽度
                child.ox = pcOx;
                child.oy = pcOy;
                child.contentW = pcContentW;
                child.svgXform = svgX;
                child.vpW = pcVpW;      // 视口尺寸跟着祖先走（userSpaceOnUse 的百分比要用）
                child.vpH = pcVpH;
            } else {
                child.svgXform = svgX;
                child.vpW = pcVpW;
                child.vpH = pcVpH;
            }
        }
    }

    // 收尾：弹出剩余容器并自动扩展，最后扩展根
    while (stack.size() > 1)
        popAndPropagate();
    growToContent(stack[0]);

    // ---- v0.16.47：给"带滤镜的空盒子容器"补一个包围盒 ----
    // <g>/<svg> 这类容器自己的盒子常常是 0×0（子形状是绝对定位的独立节点），而滤镜的
    // 离屏图层框 = 节点框外扩 3σ —— 框是空的就把层里的内容**全裁掉**（实测：套了
    // filter="url(#f)" 的 <g> 整块消失）。所以只给"带滤镜且盒子为空"的节点算一个
    // 子树并集当盒子；别的容器不动，免得悄悄改变命中与布局。
    {
        auto hasFx = [&](uint16_t nid) {
            for (const auto& pn2 : paintNodes) {
                if (pn2.nodeId == nid && pn2.blurPx > 0.0f) return true;
            }
            for (const auto& ds2 : dropShadowNodes) {
                if (ds2.nodeId == nid) return true;
            }
            return false;
        };
        std::vector<char> needBox(nodes.size(), 0);
        bool anyNeed = false;
        for (size_t ni = 1; ni < nodes.size(); ni++) {
            if (nodes[ni].width >= 1.0f && nodes[ni].height >= 1.0f) continue;
            if (!hasFx(nodes[ni].id)) continue;
            needBox[ni] = 1;
            anyNeed = true;
        }
        if (anyNeed) {
            // 绝对坐标已经算好：把每个**非空**节点的框并进它所有祖先的累加器（一遍就够了，
            // 与元素顺序无关）。id 与 nodes 下标同序（下面根节点自动撑大那段也这么假设）。
            const float kBig = 1e30f;
            std::vector<float> ux0(nodes.size(), kBig), uy0(nodes.size(), kBig);
            std::vector<float> ux1(nodes.size(), -kBig), uy1(nodes.size(), -kBig);
            for (size_t ni = 1; ni < nodes.size(); ni++) {
                const BinUINode& nd = nodes[ni];
                if (nd.width <= 0.0f && nd.height <= 0.0f) continue;
                const float ax0 = nd.x, ay0 = nd.y;
                const float ax1 = nd.x + nd.width, ay1 = nd.y + nd.height;
                uint16_t pid = nd.parentId;
                int guard = 0;
                while (pid != 0xFFFF && pid != 0 && pid < nodes.size() && guard++ < 256) {
                    if (ax0 < ux0[pid]) ux0[pid] = ax0;
                    if (ay0 < uy0[pid]) uy0[pid] = ay0;
                    if (ax1 > ux1[pid]) ux1[pid] = ax1;
                    if (ay1 > uy1[pid]) uy1[pid] = ay1;
                    pid = nodes[pid].parentId;
                }
            }
            for (size_t ni = 1; ni < nodes.size(); ni++) {
                if (!needBox[ni]) continue;
                if (ux1[ni] <= ux0[ni] || uy1[ni] <= uy0[ni]) continue;   // 子树也是空的
                // 只**放大盒子**去盖住子树，不动自己的 x/y：动位置会把流式光标和兄弟
                // 一起带偏（<g> 的盒子本来就没有布局意义，只有渲染层框用它）。
                const float growW = ux1[ni] - nodes[ni].x;
                const float growH = uy1[ni] - nodes[ni].y;
                if (growW > nodes[ni].width) nodes[ni].width = growW;
                if (growH > nodes[ni].height) nodes[ni].height = growH;
                // 同步重排规格：否则运行期 setSize 会按"自动宽度 = 父内容盒宽/0"把它
                // 重算回空盒子（实测：编译期补的盒子在 setSize 之后又变回 0×0）。
                for (auto& sp2 : specNodes) {
                    if (sp2.nodeId != nodes[ni].id) continue;
                    sp2.flags &= static_cast<uint16_t>(
                        ~(SPEC_AUTO_W | SPEC_PCT_W | SPEC_PCT_H | SPEC_FLOW_Y));
                    sp2.relW = nodes[ni].width;
                    sp2.relH = nodes[ni].height;
                    break;
                }
            }
        }
    }

    // 视口 = 窗口：给了尺寸就别让内容把根撑大。
    // 不钳的话，内容比窗口高时根会"长"出窗口（脏矩形、命中、绘制全落到窗口外，
    // 自适应布局里一缩窗口就错位）；超出的部分应该归滚动容器/裁剪去处理。
    if (hasAppWindow && wdesc.width > 0 && wdesc.height > 0) {
        nodes[0].width = rootW;
        nodes[0].height = rootH;
    }

    if (nodes.empty()) return {};

    // 受内容支撑：<app_window> 无显式尺寸时，按内容包围盒推断窗口大小
    if (hasAppWindow && wdesc.width == 0 && wdesc.height == 0) {
        float maxX = 0, maxY = 0;
        for (size_t ni = 1; ni < nodes.size(); ni++) { // 跳过根节点
            BinUINode& nd = nodes[ni];
            float ax = nd.x, ay = nd.y;
            uint16_t pid = nd.parentId;
            while (pid != 0xFFFF && pid != 0 && pid < nodes.size()) {
                const BinUINode& pn = nodes[pid];
                ax += pn.x + pn.padding;
                ay += pn.y + pn.padding;
                pid = pn.parentId;
            }
            maxX = std::max(maxX, ax + nd.width);
            maxY = std::max(maxY, ay + nd.height);
        }
        if (maxX > 1) rootW = maxX;
        if (maxY > 1) rootH = maxY;
        nodes[0].width = rootW;
        nodes[0].height = rootH;
        wdesc.width = (uint32_t)rootW;
        wdesc.height = (uint32_t)rootH;
    }

    // ---- 4.9 v12 段的字符串：图片路径 / 外挂组件路径 ----
    // 段里存的是字符串表偏移，必须赶在计算表之前入表。
    if (outImages)
        for (const auto& p : *outImages) getOrAddString(p);
    std::vector<std::pair<uint16_t, std::string>> extRefs; // 锚点节点 id -> 组件路径
    for (const auto& pc : tl_pendingComponents) {
        auto it = tl_pendingAnchor.find(pc.linkIndex);
        if (it == tl_pendingAnchor.end() || !pc.external) continue;
        const std::string rel = RebaseToHost(tl_uiBaseDir, pc.path);
        getOrAddString(rel);
        extRefs.emplace_back(it->second, rel);
    }

    // ---- 5. 字符串表 + 序列化 ----
    for (auto& [nodeId, txt] : textMap)
        getOrAddString(txt);

    std::vector<char> stringData;
    std::vector<uint32_t> offsets(strings.size());
    uint32_t cur = 0;
    for (size_t k = 0; k < strings.size(); k++) {
        offsets[k] = cur;
        for (char c : strings[k]) stringData.push_back(c);
        stringData.push_back('\0');
        cur += (uint32_t)strings[k].size() + 1;
    }

    for (auto& n : nodes) {
        auto it = textMap.find(n.id);
        n.textOffset = (it != textMap.end()) ? offsets[getOrAddString(it->second)] : 0;
    }

    uint32_t nodeCount = (uint32_t)nodes.size();
    uint32_t strSize = (uint32_t)stringData.size();
    std::vector<uint8_t> buf(sizeof(BinUIHeader) + nodeCount * sizeof(BinUINode) + strSize);
    uint8_t* ptr = buf.data();

    BinUIHeader hdr;
    hdr.magic = BINUI_HTML_MAGIC;
    hdr.version = BINUI_VERSION;
    hdr.nodeCount = nodeCount;
    hdr.stringSize = strSize;
    memcpy(ptr, &hdr, sizeof(hdr)); ptr += sizeof(hdr);
    memcpy(ptr, nodes.data(), nodeCount * sizeof(BinUINode)); ptr += nodeCount * sizeof(BinUINode);
    memcpy(ptr, stringData.data(), strSize);
    AppendSpecSection(buf, specNodes, specFlex);
    // window_resize="aspect" 且未显式给 aspect：用根（窗口）宽高比兜底
    if ((wdesc.flags & BINUI_WIN_ASPECT) && wdesc.aspect <= 0 && rootW > 0 && rootH > 0)
        wdesc.aspect = rootW / rootH;
    AppendWindowDesc(buf, wdesc);
    AppendAnimSection(buf, kfGroups, animDefs, transitions);
    AppendVideoSection(buf, videoDescs);
    AppendStateSection(buf, stateStyles);
    {
        // 资源段（v12）：imageIndex -> 图片路径
        std::vector<uint32_t> imgOffsets;
        if (outImages)
            for (const auto& p : *outImages) imgOffsets.push_back(offsets[getOrAddString(p)]);
        AppendImageSection(buf, imgOffsets);
    }
    {
        // 组件引用段（v12）：外挂组件的锚点与路径
        std::vector<BinUIComponentRef> refs;
        for (const auto& er : extRefs) {
            BinUIComponentRef r = {};
            r.anchorId = er.first;
            r.srcOffset = offsets[getOrAddString(er.second)];
            refs.push_back(r);
        }
        AppendComponentSection(buf, refs);
    }
    AppendPaintSection(buf, paintNodes);
    AppendPathSection(buf, pendingPaths);
    AppendShadowSection(buf, shadowNodes);
    AppendTextShadowSection(buf, textShadowNodes);
    AppendPaintExSection(buf, paintExNodes);
    AppendFilterSection(buf, filterNodes);
    AppendClipSection(buf, clipNodes);
    AppendMaskSection(buf, maskNodes);   // v28 遮罩段
    AppendBackdropSection(buf, backdropNodes);   // v29 毛玻璃段
    AppendDropShadowSection(buf, dropShadowNodes);   // v33 外阴影段
    AppendNameSection(buf, nodeNames, offsets);

    // ---- 6. 二进制组件：编译期离线合并 ----
    // 与运行期 renderer.mountComponent() 用的是同一个链接器（binui_link），
    // 所以"内嵌"和"外挂"的合并结果一致。
    bool linked = false;
    for (const auto& pc : tl_pendingComponents) {
        if (pc.external) continue;
        auto it = tl_pendingAnchor.find(pc.linkIndex);
        if (it == tl_pendingAnchor.end()) continue;
        LinkOptions lo;
        lo.compDir = std::filesystem::path(pc.path).parent_path().generic_string();
        lo.hostDir = tl_uiBaseDir;
        if (!pc.wAttr.empty()) { lo.hasW = true; lo.w = parseLen(pc.wAttr, 0); }
        if (!pc.hAttr.empty()) { lo.hasH = true; lo.h = parseLen(pc.hAttr, 0); }
        if (!pc.radiusAttr.empty()) { lo.hasRadius = true; lo.radius = parseLen(pc.radiusAttr, 0); }
        if (!pc.bgAttr.empty()) { lo.hasColor = true; lo.color = parseColorWith(pc.bgAttr, 0, "组件底色"); }
        std::string err;
        if (MergeLayoutFile(buf, pc.path, it->second, &lo, nullptr, &err)) {
            linked = true;
        } else {
            Warn("组件链接 " + pc.label + "：" + err + "（保留空占位节点）");
        }
    }
    if (linked && outImages) *outImages = ReadImagePaths(buf.data(), buf.size());

    return buf;
}

// ============================================================
// 默认布局
// ============================================================
std::vector<uint8_t> GenerateDefaultBinUI() {
    // 字符串表：多段文字按顺序拼接，偏移由长度累加得到。
    // 以前这里把偏移写成了魔数（0/5/18/29/66/95/106/115/130/135），文案一改
    // 就整体错位 —— 默认布局渲染出来的文字会串到别的节点上。改成算出来。
    static const char* kTexts[] = {
        // 第 0 项必须是空串：容器的 textOffset 恒为 0，靠它表示"没有文字"
        "", "9:41", "binUI Demo", "Card Title",
        "Default layout from the core renderer", "no UI file, no compiler pass:",
        "light surface + rounded corners.",
        "CLICK ME", "Enter text...", "Same palette as the examples", "+",
    };
    const uint32_t kTextCount = (uint32_t)(sizeof(kTexts) / sizeof(kTexts[0]));
    std::string blob;
    uint32_t textOff[sizeof(kTexts) / sizeof(kTexts[0])] = {};
    for (uint32_t i = 0; i < kTextCount; i++) {
        textOff[i] = (uint32_t)blob.size();
        blob.append(kTexts[i]);
        blob.push_back('\0');
    }
    const char* strings = blob.data();
    const uint32_t nodeCount = 17;
    const uint32_t strSize = (uint32_t)blob.size();

    std::vector<uint8_t> buf(sizeof(BinUIHeader) + nodeCount * sizeof(BinUINode) + strSize);
    uint8_t* ptr = buf.data();

    BinUIHeader hdr;
    hdr.magic = BINUI_MAGIC;
    hdr.version = BINUI_VERSION;
    hdr.nodeCount = nodeCount;
    hdr.stringSize = strSize;
    memcpy(ptr, &hdr, sizeof(hdr)); ptr += sizeof(hdr);

    auto add = [&](BinUINode n) { memcpy(ptr, &n, sizeof(n)); ptr += sizeof(n); };
    BinUINode tn; auto& n = tn;

    // 0: Root container - dark background
    memset(&n, 0, sizeof(n));
    n.id = 0; n.parentId = 0xFFFF; n.type = BINUI_ROOT; n.flags = 1;
    n.width = 480; n.height = 800; n.color = 0xFFF4F7FC;
    add(n);

    // 1: Status bar background
    memset(&n, 0, sizeof(n));
    n.id = 1; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 0; n.y = 0; n.width = 480; n.height = 36;
    n.color = 0xFFFFFFFF;
    add(n);

    // 2: Status bar time
    memset(&n, 0, sizeof(n));
    n.id = 2; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 420; n.y = 6; n.width = 50; n.height = 24;
    n.color = 0xFF1F2A37; n.textOffset = textOff[1]; n.fontSize = 13;
    add(n);

    // 3: App bar background
    memset(&n, 0, sizeof(n));
    n.id = 3; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 0; n.y = 36; n.width = 480; n.height = 56;
    n.color = 0xFF3E8BFF;
    add(n);

    // 4: App bar title
    memset(&n, 0, sizeof(n));
    n.id = 4; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 16; n.y = 46; n.width = 300; n.height = 36;
    n.color = 0xFFFFFFFF; n.textOffset = textOff[2]; n.fontSize = 20;
    add(n);

    // 5: Card 1 surface
    memset(&n, 0, sizeof(n));
    n.id = 5; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 16; n.y = 112; n.width = 448; n.height = 140;
    n.color = 0xFFFFFFFF; n.radius = 12;
    add(n);

    // 6: Card 1 title
    memset(&n, 0, sizeof(n));
    n.id = 6; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 124; n.width = 200; n.height = 28;
    n.color = 0xFF1F2A37; n.textOffset = textOff[3]; n.fontSize = 18;
    add(n);

    // 7: Card 1 body line 1
    memset(&n, 0, sizeof(n));
    n.id = 7; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 156; n.width = 416; n.height = 20;
    n.color = 0xFF66768C; n.textOffset = textOff[4]; n.fontSize = 13;
    add(n);

    // 8: Card 1 body line 2
    memset(&n, 0, sizeof(n));
    n.id = 8; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 178; n.width = 416; n.height = 20;
    n.color = 0xFF66768C; n.textOffset = textOff[5]; n.fontSize = 13;
    add(n);

    // 9: Card 1 body line 3
    memset(&n, 0, sizeof(n));
    n.id = 9; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 200; n.width = 416; n.height = 20;
    n.color = 0xFF66768C; n.textOffset = textOff[6]; n.fontSize = 13;
    add(n);

    // 10: Material button (purple accent)
    memset(&n, 0, sizeof(n));
    n.id = 10; n.parentId = 0; n.type = BINUI_BUTTON;
    // 手写布局也得带"按钮文字默认居中"这条 UA 语义（编译期路径见上面 type==BINUI_BUTTON
    // 补 BINUI_TEXT_CENTER 那段）：后端改成读 flags 后，漏了这条按钮文字会变左对齐。
    n.flags = 5 | BINUI_TEXT_CENTER;
    n.x = 32; n.y = 276; n.width = 416; n.height = 48;
    n.color = 0xFF3E8BFF; n.textOffset = textOff[7]; n.fontSize = 14; n.radius = 8;
    add(n);

    // 11: Outlined input field (underline style)
    memset(&n, 0, sizeof(n));
    n.id = 11; n.parentId = 0; n.type = BINUI_INPUT; n.flags = 5;
    n.x = 32; n.y = 348; n.width = 416; n.height = 48;
    n.color = 0xFFF7FAFF; n.textOffset = textOff[8]; n.fontSize = 14;
    add(n);

    // 12: Divider line
    memset(&n, 0, sizeof(n));
    n.id = 12; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 32; n.y = 420; n.width = 416; n.height = 1;
    n.color = 0xFFDCE5F2;
    add(n);

    // 13: Card 2 surface
    memset(&n, 0, sizeof(n));
    n.id = 13; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 16; n.y = 440; n.width = 448; n.height = 80;
    n.color = 0xFFFFFFFF; n.radius = 12;
    add(n);

    // 14: Card 2 text
    memset(&n, 0, sizeof(n));
    n.id = 14; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 460; n.width = 416; n.height = 24;
    n.color = 0xFF1F2A37; n.textOffset = textOff[9]; n.fontSize = 15;
    add(n);

    // 15: Bottom nav background
    memset(&n, 0, sizeof(n));
    n.id = 15; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 0; n.y = 752; n.width = 480; n.height = 48;
    n.color = 0xFFFFFFFF;
    add(n);

    // 16: Floating action button (purple circle)
    memset(&n, 0, sizeof(n));
    n.id = 16; n.parentId = 0; n.type = BINUI_IMAGE; n.flags = 5;
    n.x = 400; n.y = 690; n.width = 56; n.height = 56;
    n.color = 0xFF2FBF9A; n.textOffset = textOff[10]; n.fontSize = 14; n.radius = 28;
    add(n);

    memcpy(ptr, strings, strSize);

    WinDesc wdesc;
    wdesc.present = true;
    wdesc.width = 480; wdesc.height = 800;
    wdesc.bgColor = 0xFFF4F7FC;
    wdesc.title = "binUI Demo";
    AppendWindowDesc(buf, wdesc);
    // 手写布局没有规格/动画/视频/状态段，但 v12 的两段恒定写出，保证解析链一致
    AppendImageSection(buf, {});
    AppendComponentSection(buf, {});
    AppendPaintSection(buf, {});
    AppendPathSection(buf, {});      // v15：默认布局没有 SVG，段恒定写出保证解析链确定
    AppendShadowSection(buf, {});    // v23：默认布局没有 box-shadow，段同样恒定写出
    AppendTextShadowSection(buf, {}); // v24：默认布局没有 text-shadow
    AppendPaintExSection(buf, {});  // v25：默认布局没有九宫格图片
    AppendFilterSection(buf, {});     // v26：这条路径没有图片滤镜
    AppendClipSection(buf, {});       // v27：这条路径没有裁剪路径
    AppendMaskSection(buf, {});       // v28：这条路径没有 mask
    AppendBackdropSection(buf, {});   // v29：这条路径没有毛玻璃

    return buf;
}

} // namespace binui
