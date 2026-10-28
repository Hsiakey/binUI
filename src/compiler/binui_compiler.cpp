// binui_compiler.cpp - UI 源码 -> binUI (.xhtml) 二进制布局编译器
#include "binui_compiler.h"
#include "binui_asset.h"
#include "binui_format.h"
#include "binui_link.h"

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
    if (AnimTrim(lo) == "all") return ANIM_OPACITY|ANIM_TRANSLATE|ANIM_SCALE|ANIM_BG_COLOR|ANIM_RADIUS;
    uint16_t m = 0;
    auto has = [&](const char* k) { return lo.find(k) != std::string::npos; };
    if (has("opacity")) m |= ANIM_OPACITY;
    if (has("transform")) m |= ANIM_TRANSLATE | ANIM_SCALE;
    if (has("translate")) m |= ANIM_TRANSLATE;
    if (has("scale")) m |= ANIM_SCALE;
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
            float tx = AnimFnNum(v, "translateX");
            float ty = AnimFnNum(v, "translateY");
            if (v.find("translate(") != std::string::npos) { tx = AnimFnNum(v, "translate", 0); ty = AnimFnNum(v, "translate", 1); }
            kf.tx = tx; kf.ty = ty;
            kf.propMask |= ANIM_TRANSLATE;
        }
        if (v.find("scale") != std::string::npos) {
            float s = AnimFnNum(v, "scale");
            kf.scale = (s == 0) ? 1.0f : s;
            kf.propMask |= ANIM_SCALE;
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
            t.propMask = props ? props : (ANIM_OPACITY|ANIM_TRANSLATE|ANIM_SCALE|ANIM_BG_COLOR|ANIM_RADIUS);
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

std::vector<uint8_t> CompileBinUISource(const std::string& source) {    return CompileBinUIEx(source).layout;
}

CompileResult CompileBinUIEx(const std::string& source) {
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
        if (xml)
            res.layout = CompileXML(source, &res.images);
        else
            res.layout = CompileHTML(source, &res.images);
    }
    return res;
}

std::vector<uint8_t> CompileBinUIFile(const std::string& path) {
    return CompileBinUIFileEx(path).layout;
}

CompileResult CompileBinUIFileEx(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    // 组件内联（<use src="…">）要相对"当前 UI 文件所在目录"解析路径
    struct BaseDirGuard {
        std::string prev;
        explicit BaseDirGuard(const std::string& d) : prev(tl_uiBaseDir) { tl_uiBaseDir = d; }
        ~BaseDirGuard() { tl_uiBaseDir = prev; }
    } guard{std::filesystem::path(path).parent_path().generic_string()};
    return CompileBinUIEx(ss.str());
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
        "display", "position", "left", "top", "overflow", "opacity", "align",
        "flex", "flex-direction", "justify-content", "align-items", "gap",
        "animation", "transition", "transform", "white-space",
        "aspect", "resizable", "window_resize", "transparent", "ui_scale",
        "volume", "autoplay", "loop", "muted", "controls",
        "points", "stroke", "stroke-width", "fill", "rx", "cx", "cy", "r",
        "x1", "y1", "x2", "y2",
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
// 组件内联：<use src="路径.bum.xml"> / <component src=…>
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
// 这样 .bum.xml 既能被 <use> 内联传参，也能自己编成 .bum —— 两种形态语义一致。
static std::string ApplyParamDefaults(const std::string& src) {
    static const std::unordered_map<std::string, std::string> kNoParams;
    return SubstituteParams(src, kNoParams, /*defaultsOnly=*/true);
}

// ============================================================
// 组件来源解析：.bum/.bui 二进制 与 .bum.xml/.bui.xml 源码
//
// 作者可以只放二进制、只放源码，或两种都放（甚至按组件混放）：
//   · 无扩展名的 src 依次探测 x.bum / x.bui → x.bum.xml / x.bui.xml / x.html / x.xml
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
            failReason = "读不到组件文件「" + srcRel + "」（.bum / .bui / .bum.xml 都不存在）";
        } else {
            compDir = std::filesystem::path(af.path).parent_path().generic_string();
            const bool binary = (af.kind == AssetKind::BinaryLayout);
            // 二进制在、但源码更新：这次用源码（改完保存就该看到效果），并明确告警
            if (af.sourceNewer)
                Warn("组件 " + srcRel + " 的源码比二进制新，已改用源码内联（建议重编后发布）");
            if (binary || external) {
                deferred = true;   // 占位节点 + 链接器（编译期）/渲染器（运行期）
            } else {
                std::string compSrc;
                if (!ReadWholeFile(af.path, compSrc)) {
                    failReason = "读不到组件文件 " + af.path;
                } else {
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
        HNode root;
        root.tag = "div";
        size_t compRootIdx = SIZE_MAX;
        if (!compElems.empty()) {
            for (size_t k = 0; k < compElems.size(); k++)
                if (compElems[k].parentIdx == SIZE_MAX) { compRootIdx = k; break; }
            if (compRootIdx != SIZE_MAX) {
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
                if (k == compRootIdx) continue;   // 组件根已被替换节点取代
                HNode ce = compElems[k];
                ce.parentIdx = (ce.parentIdx == compRootIdx) ? remap[oi] : cremap[ce.parentIdx];
                cremap[k] = out.size();
                out.push_back(std::move(ce));
            }
        }
        styleSheets += compStyles;
    }
    elems.swap(out);
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

    // ---- 1.5 组件内联：<use src="x.bum.xml"> 展开成真实元素 ----
    ExpandComponents(elems, styleSheets, tl_uiBaseDir, 0);

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
    auto styleOf = [&](const HNode& e, size_t ei = SIZE_MAX) -> std::unordered_map<std::string, std::string> {
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
    // 只用于"未声明高度的文本节点该占多高"，宁可按偏宽估（多一行总比压字好）。
    auto estTextWidth = [](const std::string& text, float fontSize) -> float {
        float w = 0.0f;
        for (size_t i = 0; i < text.size(); ) {
            const unsigned char c = (unsigned char)text[i];
            uint32_t cp = c;
            size_t len = 1;
            if (c >= 0xF0) len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;
            if (len > 1 && i + len <= text.size()) {
                cp = (len == 2) ? (uint32_t)(c & 0x1F)
                   : (len == 3) ? (uint32_t)(c & 0x0F) : (uint32_t)(c & 0x07);
                for (size_t k = 1; k < len; k++)
                    cp = (cp << 6) | (uint32_t)((unsigned char)text[i + k] & 0x3F);
            }
            i += len;
            if (cp == '\t') { w += fontSize * 2.0f; continue; }
            const bool wide = cp >= 0x1100 && (cp <= 0x115F || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                               (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                               (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                               (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF));
            w += wide ? fontSize : fontSize * 0.52f;
        }
        return w;
    };
    // 行距（像素）：line-height 支持无单位倍数 / % / px；未声明按 1.35×字号
    auto estLineStep = [](const std::unordered_map<std::string, std::string>& style,
                          float fontSize) -> float {
        auto it = style.find("line-height");
        if (it == style.end()) return fontSize * 1.35f;
        const std::string v = TrimCopy(it->second);
        if (v.empty() || LowerCopy(v) == "normal") return fontSize * 1.35f;
        const bool pct = v.back() == '%';
        const bool unitless = v.find_first_not_of("0123456789.+-") == std::string::npos;
        float num = 0;
        try { num = std::stof(v); } catch (...) { return fontSize * 1.35f; }
        if (unitless) return num > 0.1f ? num * fontSize : fontSize * 1.35f;
        if (pct) return num > 10.0f ? fontSize * num / 100.0f : fontSize * 1.35f;
        return num > 0.5f ? num : fontSize * 1.35f;   // px
    };


    // 渐变底色：linear-gradient(方向, 起色, 止色[, ...])  只取首尾两色
    struct GradSpec { bool ok = false; uint32_t from = 0, to = 0; uint8_t dir = BINUI_GRAD_V; };
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
        {
            const std::string first = LowerCopy(parts[0]);
            const bool looksDir = first.rfind("to ", 0) == 0 || first.find("deg") != std::string::npos ||
                                  first.find("turn") != std::string::npos ||
                                  first.find("rad") != std::string::npos;
            if (looksDir) {
                cs = 1;
                if (first.find("right") != std::string::npos && first.find("bottom") != std::string::npos)
                    gs.dir = BINUI_GRAD_D;
                else if (first.find("right") != std::string::npos) gs.dir = BINUI_GRAD_H;
                else if (first.find("top") != std::string::npos)   gs.dir = BINUI_GRAD_V;
                else if (first.find("left") != std::string::npos)  gs.dir = BINUI_GRAD_H;
                else gs.dir = BINUI_GRAD_V;
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
    stack.push_back({ 0, 0, 0, 0, rootW, 0, 0xFFFFFFFFu, false, false });

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

    // flex 布局（编译期）：容器内容盒 (ox,oy,cw,ch)，子项坐标相对内容盒写入 out
    // 返回容器内容盒所需高度（auto 高度时用于扩展容器）；weights 输出各子项弹性权重
    auto layoutFlex = [&](const std::vector<size_t>& kids,
                          float cw, float ch,
                          const std::string& dir, const std::string& justify,
                          const std::string& align, float gap,
                          std::unordered_map<size_t, float[4]>& out,
                          std::unordered_map<size_t, float>& weights) -> float {
        bool row = (dir != "column");
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
            active.push_back(k);
        }
        size_t n = active.size();
        float totalGap = (n > 1 ? (float)(n - 1) : 0) * gap;

        // ---- 主轴尺寸分配 ----
        // row（主轴=宽）：声明宽/百分比宽 = 固定尺寸；flex:N 或未声明宽度 = 弹性分配
        // column（主轴=高）：声明高/百分比高 = 固定尺寸；flex:N = 弹性分配；未声明宽度 = 拉伸到内容盒
        // fixedSum 必须把百分比尺寸也算进去 —— 否则弹性项会把百分比兄弟占的位置再吃一遍（重叠溢出）
        std::vector<float> w(n), h(n), weight(n, 0);
        std::vector<float> mnw(n, -1), mxw(n, -1), mnh(n, -1), mxh(n, -1);
        std::vector<uint8_t> mnwp(n, 0), mxwp(n, 0), mnhp(n, 0), mxhp(n, 0);
        float weightSum = 0, fixedSum = 0;
        for (size_t i = 0; i < n; i++) {
            size_t k = active[i];
            const auto& e = elems[k];
            auto st = styleOf(e, k);
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
            auto sp = ParseLenPct(sw, 0, curCtx, "flex 子项 width");
            auto fit = st.find("flex");
            float fw = fit != st.end() ? parseLen(fit->second, 0, "flex 权重") : 0;
            weights[k] = 0;

            // 交叉轴先按声明值算（主轴那一个稍后被弹性分配覆盖）
            w[i] = sp.pct ? sp.value / 100.0f * cw : sp.value;
            std::string sh = pick(e.attrs, st, "h", "height", "");
            if (sh.empty()) sh = pick(e.attrs, st, "height", "height", "");
            auto hp = ParseLenPct(sh, 0, curCtx, "flex 子项 height");
            if (hp.pct) h[i] = hp.value / 100.0f * ch;
            else h[i] = !sh.empty() ? hp.value : defaultHeight(e.tag, tagToType(e.tag));

            if (row) {
                if (sp.pct) fixedSum += w[i];                    // 百分比宽 = 固定比例（占位）
                else if (fw > 0) { weights[k] = fw; weightSum += fw; }
                else if (!sw.empty() && sp.value > 0) fixedSum += w[i];
                else { weights[k] = 1.0f; weightSum += 1.0f; }  // 未声明宽 = 弹性
            } else {
                if (fw > 0) { weights[k] = fw; weightSum += fw; }
                else fixedSum += h[i];
                if (sw.empty()) w[i] = cw;                      // 交叉轴拉伸（CSS stretch 默认）
            }

            // 子项 min/max 尺寸约束
            auto con = [&](const std::string& key, float& out, uint8_t& pct) {
                std::string v = pick(e.attrs, st, key, key, "");
                if (lowerStr(v) == "auto") v.clear();
                if (v.empty()) { out = -1; return; }
                auto p = ParseLenPct(v, 0, curCtx, key.c_str());
                if (p.pct) { out = p.value / 100.0f; pct = 1; }
                else out = p.value;
            };
            con("min-width",  mnw[i], mnwp[i]);
            con("max-width",  mxw[i], mxwp[i]);
            con("min-height", mnh[i], mnhp[i]);
            con("max-height", mxh[i], mxhp[i]);
        }

        // 主轴剩余长度按权重分配（row 分宽，column 分高）
        const float mainLen = row ? cw : ch;
        float remain = mainLen - fixedSum - totalGap;
        if (remain < 0) remain = 0;
        float extra = weightSum > 0 ? remain / weightSum : 0;
        for (size_t i = 0; i < n; i++) {
            if (weights[active[i]] <= 0) continue;
            const float v = extra * weights[active[i]];
            if (row) w[i] = v; else h[i] = v;
        }

        // 弹性分配后按 min/max 约束钳制（% 相对父内容盒）
        for (size_t i = 0; i < n; i++) {
            float mn = mnw[i] >= 0 ? (mnwp[i] ? mnw[i] * cw : mnw[i]) : -1;
            float mx = mxw[i] >= 0 ? (mxwp[i] ? mxw[i] * cw : mxw[i]) : -1;
            float mh = mnh[i] >= 0 ? (mnhp[i] ? mnh[i] * ch : mnh[i]) : -1;
            float xh = mxh[i] >= 0 ? (mxhp[i] ? mxh[i] * ch : mxh[i]) : -1;
            if (mn >= 0 && w[i] < mn) w[i] = mn;
            if (mx >= 0 && w[i] > mx) w[i] = mx;
            if (mh >= 0 && h[i] < mh) h[i] = mh;
            if (xh >= 0 && h[i] > xh) h[i] = xh;
        }

        float mainTotal = 0;
        for (size_t i = 0; i < n; i++) mainTotal += row ? w[i] : h[i];
        float cross = row ? ch : cw;
        float main = row ? cw : ch; // 主轴可用长度

        // 自由空间：溢出时按 CSS 处理为 0（否则 space-between/-around 会算出负间距，
        // 让子项往回堆叠甚至出现负坐标）
        float freeMain = main - mainTotal - totalGap;
        if (freeMain < 0) freeMain = 0;

        // justify-content：首/尾/项间额外间距（沿主轴）
        float leading = 0, gap2 = 0;
        if (justify == "flex-end") leading = freeMain;
        else if (justify == "center") leading = freeMain / 2.0f;
        else if (justify == "space-between")
            gap2 = n > 1 ? freeMain / (float)(n - 1) : 0;
        else if (justify == "space-around") {
            // CSS：每项两侧各占一半，首尾只剩半个间隔
            leading = n > 0 ? freeMain / (2.0f * (float)n) : 0;
            gap2 = n > 0 ? freeMain / (float)n : 0;
        }
        float pos = leading;
        float maxCross = 0;
        for (size_t i = 0; i < n; i++) {
            size_t k = active[i];
            float c = 0; // 交叉轴
            if (align == "center") c = (cross - (row ? h[i] : w[i])) / 2;
            else if (align == "flex-end") c = cross - (row ? h[i] : w[i]);
            out[k][0] = row ? pos : c;
            out[k][1] = row ? c : pos;
            out[k][2] = w[i];
            out[k][3] = h[i];
            maxCross = std::max(maxCross, row ? h[i] : w[i]);
            pos += (row ? w[i] : h[i]) + gap + gap2;
        }
        return row ? maxCross : mainTotal + totalGap;
    };

    // 容器元素索引 -> 已计算的 flex 子项位置（elemIdx -> x/y/w/h）与弹性权重
    std::unordered_map<size_t, std::unordered_map<size_t, float[4]>> flexLayouts;
    std::unordered_map<size_t, std::unordered_map<size_t, float>> flexWeights;

    // ============================================================
    // HTML 标签/属性体检（只在 HTML 模式跑；XML 允许自定义标签）
    // ------------------------------------------------------------
    // CSS 那边早就有 kIgnoredCss 这类"写了没效果就明说"的机制，HTML 这边一条都没有，
    // 于是 <u>/<mark>/placeholder/disabled 这些东西默默编成别的东西也没人知道。
    // 这里只做体检、不改行为：不改 HTML 的容错性（未知标签照样按容器处理）。
    // 每个标签/属性全文档只报一次，避免刷屏。
    // ============================================================
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
        };
        // 真正被编译器消费的属性（其余一律视为"写了没效果"）
        const auto& kUsedAttrs = UsedHtmlAttrs();
        bool inlineNestedWarned = false;   // 行内嵌套警告全文档只报一次
        for (const auto& e : elems) {
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
                if (kUsedAttrs.count(k)) continue;
                Warn("HTML 属性 \"" + k + "\"（<" + e.tag + ">）未实现，已忽略");
            }
        }
    }

    for (size_t ei = 0; ei < elems.size(); ei++) {
        if (id >= 0x10000) break; // 节点 id 为 uint16_t，超限停止生成
        const auto& e = elems[ei];
        auto style = styleOf(e, ei);
        uint8_t type = tagToType(e.tag);

        // 认识但不生效的常见 CSS 属性：明确警告（同一属性全文档只报一条），
        // 避免"写了没效果"这种最耗时间的困惑。
        // v13 起 border*/font-weight/line-height/text-decoration 已经实现，从清单里移除。
        // v14 起 font-style（italic）/white-space（pre）也实现了，同样移除。
        {
            static const char* kIgnoredCss[] = {
                "box-shadow", "text-shadow", "letter-spacing",
                "word-break", "visibility", "float", "flex-wrap",
                "flex-shrink", "align-self", "align-content", "order", "outline",
                "cursor", "grid-template-columns", "text-overflow", "object-fit",
                "right", "bottom", "inset", "transform", "filter", "aspect-ratio",
            };
            for (const char* k : kIgnoredCss)
                if (style.count(k))
                    Warn(std::string("CSS 属性 ") + k + " 暂不支持，已忽略");
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

        std::string display = pick(e.attrs, style, "display", "display", "");
        if (display == "none") continue;

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

        // <polyline points="x,y x,y ..."> / <polygon>：展开成若干 BINUI_LINE 子节点
        // 坐标为 svg 容器坐标系（pc.ox/pc.oy 即容器内容盒原点）
        if (e.tag == "polyline" || e.tag == "polygon") {
            std::string pts = e.attrs.count("points") ? e.attrs.at("points") : "";
            std::vector<float> coords;
            std::string cur;
            for (char ch : pts) {
                if (ch==',' || ch==' ' || ch=='\t' || ch=='\n' || ch=='\r') {
                    if (!cur.empty()) { coords.push_back(parseLen(cur, 0)); cur.clear(); }
                } else cur += ch;
            }
            if (!cur.empty()) coords.push_back(parseLen(cur, 0));
            size_t m = coords.size() / 2;
            bool closed = (e.tag == "polygon") && m >= 2;
            if (m >= 2) {
                std::string st = pick(e.attrs, style, "stroke", "stroke", "");
                uint32_t stroke = st.empty() ? 0xFF000000u : parseColor(st);
                std::string swt = pick(e.attrs, style, "stroke-width", "stroke-width", "");
                uint16_t sw2 = (uint16_t)parseLen(swt, 1.0f);
                uint32_t firstId = 0;
                auto emitSeg = [&](size_t a, size_t b) {
                    BinUINode ln = {};
                    ln.id = id++;
                    if (!firstId) firstId = ln.id;
                    ln.parentId = (uint16_t)pnodeId;
                    ln.type = BINUI_LINE;
                    ln.flags = BINUI_VISIBLE | BINUI_ABSOLUTE;
                    ln.x = pc.ox + coords[2*a]; ln.y = pc.oy + coords[2*a+1];
                    ln.width = pc.ox + coords[2*b]; ln.height = pc.oy + coords[2*b+1];
                    ln.color = stroke;
                    ln.radius = sw2;
                    nodes.push_back(ln);
                };
                for (size_t k = 0; k + 1 < m; k++) emitSeg(k, k + 1);
                if (closed) emitSeg(m - 1, 0);
                hIdxToNodeId[ei] = firstId; // 代表节点：供其后代解析父级（polyline 无子元素）
            }
            continue; // 不生成普通节点、不推容器上下文
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

        // 文本节点未声明高度时估算行数并撑开高度。
        // 没有这一步，段落按单行高排布：换行后的第二行会压到下一个元素身上
        // （三个后端都按节点矩形绘制，谁也没办法把节点撑高）。估算口径：
        //   · CJK/全角按 1.0×字号，其余按 0.52×字号（拉丁小写平均宽度），宁可高一点
        //   · 行距取 line-height（未声明则 1.35×字号）
        //   · 显式写了 height 的不动（作者说了算）
        if (sh.empty() && !srcText.empty() &&
            (type == BINUI_LABEL || type == BINUI_LIST || type == BINUI_BUTTON)) {
            const float step = estLineStep(style, nodeFontSize);
            float avail = pc.contentW;
            if (!sw.empty()) {   // sw 已在上面的几何区里解析过（'auto' 被清空）
                auto sp = ParseLenPct(sw, 0, curCtx, "width");
                avail = sp.pct ? sp.value / 100.0f * pc.contentW : sp.value;
            }
            avail -= type == BINUI_LIST ? 24.0f : 0.0f;   // LIST 文字左右各有 12px 内缩
            if (avail < 8.0f) avail = 8.0f;
            size_t lines = 0;
            size_t p0 = 0;
            while (true) {
                const size_t nl = srcText.find('\n', p0);
                const std::string seg = srcText.substr(
                    p0, (nl == std::string::npos) ? std::string::npos : nl - p0);
                lines += (size_t)std::max(1.0f, std::ceil(estTextWidth(seg, nodeFontSize) / avail));
                if (nl == std::string::npos) break;
                p0 = nl + 1;
            }
            if (lines < 1) lines = 1;
            const float needH = (float)lines * step;
            if (needH > lh) lh = needH;
        }

        // SVG 图形属性 -> 通用定位：<line> 用 x1/y1/x2/y2 端点，<circle> 用 cx/cy/r
        // 端点在绘制后端按"起点 + 相对偏移"还原（见节点生成处的 BINUI_LINE 覆盖）
        float svgR = -1, svgLineX2 = -1, svgLineY2 = -1;
        if (e.tag == "line") {
            float x1 = parseLen(e.attrs.count("x1") ? e.attrs.at("x1") : "", 0);
            float y1 = parseLen(e.attrs.count("y1") ? e.attrs.at("y1") : "", 0);
            float x2 = parseLen(e.attrs.count("x2") ? e.attrs.at("x2") : "", 0);
            float y2 = parseLen(e.attrs.count("y2") ? e.attrs.at("y2") : "", 0);
            sx = std::to_string(x1); sy = std::to_string(y1);
            sw = std::to_string(x2); sh = std::to_string(y2);
            svgLineX2 = x2; svgLineY2 = y2;
        } else if (e.tag == "circle") {
            float cx = parseLen(e.attrs.count("cx") ? e.attrs.at("cx") : "", 0);
            float cy = parseLen(e.attrs.count("cy") ? e.attrs.at("cy") : "", 0);
            svgR = parseLen(e.attrs.count("r") ? e.attrs.at("r") : "", 0);
            sx = std::to_string(cx - svgR); sy = std::to_string(cy - svgR);
            sw = std::to_string(2 * svgR); sh = std::to_string(2 * svgR);

        }

        // <br>：换行占位——块式布局下每个元素已独占一行，br 只占零高度
        if (e.tag == "br") lh = 0;

        // 复选框/单选框：没声明宽高时是 18×18 的小方块（以前是全宽 40px 高文本框）
        if (isCheckbox) {
            if (sw.empty()) sw = "18";
            if (sh.empty()) sh = "18";
        }

        if (!sx.empty()) { auto p = ParseLenPct(sx, 0, curCtx, "left"); lx = p.value; if (p.pct) pctX = p.value / 100.0f; hasX = true; }
        if (!sy.empty()) { auto p = ParseLenPct(sy, 0, curCtx, "top"); ly = p.value; if (p.pct) pctY = p.value / 100.0f; hasY = true; }
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
        std::string pos = lowerStr(style.count("position") ? style.at("position") : "");
        bool isAbs = (pos == "absolute" || pos == "fixed");
        if (isAbs) {
            if (!hasX) { lx = 0; hasX = true; }
            if (!hasY) { ly = 0; hasY = true; }
        }

        // margin 仅在流式布局中生效
        float mt = 0, mr = 0, mb = 0, ml = 0;
        if (!hasY) {
            auto m = parseBox(style, "margin",
                              "margin-top", "margin-right", "margin-bottom", "margin-left", 0);
            mt = m[0]; mr = m[1]; mb = m[2]; ml = m[3];
        }

        // 百分比：相对父容器内容盒解析（width 相对父内容宽，height/left/top 相对父内容高）
        float pcContentH = 0;
        {
            const BinUINode& pcn = nodes[pc.nodeIdx];
            pcContentH = pcn.height - 2.0f * pcn.padding;
            if (pcContentH < 0) pcContentH = 0;
        }
        if (pctX >= 0) lx = pctX * pc.contentW;
        if (pctY >= 0) ly = pctY * pcContentH;
        if (pctH >= 0) lh = pctH * pcContentH;

        // 尺寸约束按父内容盒解析（% 时）后的实际像素值；-1 = 未指定
        float effMnW = mnW >= 0 ? (mnWp ? mnW * pc.contentW : mnW) : -1;
        float effMxW = mxW >= 0 ? (mxWp ? mxW * pc.contentW : mxW) : -1;
        float effMnH = mnH >= 0 ? (mnHp ? mnH * pcContentH : mnH) : -1;
        float effMxH = mxH >= 0 ? (mxHp ? mxH * pcContentH : mxH) : -1;

        float x, y, w;
        bool flexPlaced = false;
        float baseW = 0, baseH = lh; // 显式声明值（未钳制）：运行时重排基准
        auto flIt = flexLayouts.find(e.parentIdx);
        if (flIt != flexLayouts.end()) {
            auto pIt = flIt->second.find(ei);
            if (pIt != flIt->second.end()) {
                const float* p = pIt->second;
                x = pc.ox + p[0]; y = pc.oy + p[1]; w = p[2]; lh = p[3];
                flexPlaced = true;
            }
        }
        if (!flexPlaced) {
            baseH = lh;
            // 高度先钳制再推进流光标（约束高度影响流式堆叠位置）
            if (effMnH >= 0 && lh < effMnH) lh = effMnH;
            if (effMxH >= 0 && lh > effMxH) lh = effMxH;
            if (hasX) x = pc.ox + lx;
            else      x = pc.ox + ml;
            if (hasY) y = pc.oy + ly;
            else {
                y = pc.oy + pc.cursorY + mt;
                pc.cursorY += mt + lh + mb;
            }
            if (hasW) w = (pctW >= 0) ? pctW * pc.contentW : parseLen(sw, 0);
            else      w = pc.contentW - (ml + mr > 0 ? (ml + mr) : 0);
            baseW = w;
            if (effMnW >= 0 && w < effMnW) w = effMnW;
            if (effMxW >= 0 && w > effMxW) w = effMxW;
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

        uint32_t defBg = (type == BINUI_LABEL)  ? 0xFFFFFFFFu
                       : (type == BINUI_SVG || type == BINUI_CANVAS) ? 0x00000000u
                       : (type == BINUI_VIDEO) ? 0xFF101010u
                       : (type == BINUI_BUTTON || type == BINUI_INPUT) ? 0xFF222222u
                       : 0x00000000u;   // v13：容器默认透明（以前是 #222222，浅色主题直接被吃掉）
        if (bgRaw.empty()) n.color = defBg;
        else if (gs.ok) n.color = gs.from;   // 渐变：底色兜底用起点色（免得被当颜色解析刷警告）
        else n.color = parseColorWith(bgRaw, defBg, "背景色");
        if (gs.ok) {
            paint.gradFrom = gs.from; paint.gradTo = gs.to; paint.gradDir = gs.dir;
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

        // 静态 opacity：编译期折进节点颜色 alpha（binUI 没有合成组，子节点不受影响；
        // 动画里的 opacity 仍走 v7 动画系统）。必须在 n.color 赋值之后处理。
        {
            auto oit = style.find("opacity");
            if (oit != style.end()) {
                float op = parseLen(oit->second, 1.0f, "opacity");
                if (op < 0.0f) op = 0.0f;
                if (op > 1.0f) op = 1.0f;
                auto fade = [&](uint32_t c) {
                    uint32_t a = (uint32_t)(((c >> 24) & 0xFFu) * op + 0.5f);
                    return (a << 24) | (c & 0x00FFFFFFu);
                };
                n.color = fade(n.color);
                // 扩展绘制段里的颜色同一个语义：整块淡出时文字/边框/渐变都要跟着淡
                if (pflags & PAINT_TEXT_COLOR)  paint.textColor = fade(paint.textColor);
                if (pflags & PAINT_BORDER)      for (int k = 0; k < 4; k++) paint.borderColor[k] = fade(paint.borderColor[k]);
                if (pflags & PAINT_GRADIENT) { paint.gradFrom = fade(paint.gradFrom);
                                               paint.gradTo = fade(paint.gradTo); }
            }
        }

        if (pflags || paint.textFlags) {
            paint.nodeId = n.id;
            paint.flags = pflags;
            paintNodes.push_back(paint);
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
        if (e.tag == "line") {
            // x,y = 起点（绝对）；width,height = 终点（绝对）＝ 起点 + 声明偏移
            n.x = pc.ox + parseLen(sx, 0);
            n.y = pc.oy + parseLen(sy, 0);
            n.width = n.x + (svgLineX2 - parseLen(sx, 0));
            n.height = n.y + (svgLineY2 - parseLen(sy, 0));
            std::string st = pick(e.attrs, style, "stroke", "stroke", "");
            n.color = st.empty() ? 0xFF000000u : parseColor(st);
            std::string swt = pick(e.attrs, style, "stroke-width", "stroke-width", "");
            n.radius = (uint16_t)parseLen(swt, 1.0f);
            n.flags |= BINUI_ABSOLUTE;
        } else if (e.tag == "circle") {
            std::string fl = pick(e.attrs, style, "fill", "fill", "");
            n.color = fl.empty() ? 0xFF000000u : parseColor(fl); // SVG 默认 fill=黑
            n.radius = (uint16_t)svgR;
        } else if (e.tag == "rect") {
            std::string fl = pick(e.attrs, style, "fill", "fill", "");
            n.color = fl.empty() ? 0xFF000000u : parseColor(fl);
            std::string rx = e.attrs.count("rx") ? e.attrs.at("rx") : "";
            if (!rx.empty()) n.radius = (uint16_t)parseLen(rx, 0);
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
            if (!src.empty()) {
                n.imageIndex = (uint16_t)outImages->size();
                outImages->push_back(src);
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

        nodes.push_back(n);

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
            auto wIt = flexWeights.find(e.parentIdx);
            if (wIt != flexWeights.end()) {
                auto cIt = wIt->second.find(ei);
                if (cIt != wIt->second.end())
                    sp.flexWeight = (uint8_t)std::lround(std::clamp(cIt->second, 0.0f, 255.0f));
            }
        } else {
            if (!hasY) sp.flags |= SPEC_FLOW_Y;
            if (!hasW) sp.flags |= SPEC_AUTO_W;
        }
        sp.relX = x - pc.ox;
        sp.relY = y - pc.oy;
        sp.ml = ml; sp.mr = mr; sp.mt = mt; sp.mb = mb;
        sp.minW = mnW; sp.maxW = mxW; sp.minH = mnH; sp.maxH = mxH;
        if (mnWp) sp.flags |= SPEC_MINW_PCT;
        if (mxWp) sp.flags |= SPEC_MAXW_PCT;
        if (mnHp) sp.flags |= SPEC_MINH_PCT;
        if (mxHp) sp.flags |= SPEC_MAXH_PCT;
        sp.relW = (!flexPlaced && hasW && pctW < 0) ? baseW : -1;
        sp.relH = (!flexPlaced && !sh.empty() && pctH < 0) ? baseH : -1;
        specNodes.push_back(sp);

        // 容器上下文：内容盒 = 自身位置 + padding
        // display:flex 容器：先为全部子项做 flex 布局（auto 高度时扩展容器）
        std::string disp = lowerStr(pick(e.attrs, style, "display", "display", ""));
        if (disp == "flex") {
            std::string dir  = lowerStr(style.count("flex-direction") ? style.at("flex-direction") : "row");
            std::string just = lowerStr(style.count("justify-content") ? style.at("justify-content") : "flex-start");
            std::string algn = lowerStr(style.count("align-items") ? style.at("align-items") : "flex-start");
            float gap = 0;
            auto git = style.find("gap");
            if (git != style.end()) gap = parseLen(git->second, 0);

            std::unordered_map<size_t, float> wts;
            std::unordered_map<size_t, float[4]> pos;
            float need = layoutFlex(childrenOf[ei], w - 2 * pad, lh - 2 * pad,
                                    dir, just, algn, gap, pos, wts);
            flexLayouts[ei] = std::move(pos);
            flexWeights[ei] = std::move(wts);

            // auto 高度（未声明 height）：内容盒高度 = flex 主轴需求
            if (sh.empty()) {
                BinUINode& cn = nodes.back();
                cn.height = need + 2 * pad;
                // auto 高同样受 min/max-height 约束钳制
                if (effMnH >= 0 && cn.height < effMnH) cn.height = effMnH;
                if (effMxH >= 0 && cn.height > effMxH) cn.height = effMxH;
            }

            BinUISpecFlex fs = {};
            fs.nodeId = n.id;
            fs.dir = dir == "column" ? 1 : 0;
            fs.justify = just == "flex-end" ? 1 : just == "center" ? 2
                       : just == "space-between" ? 3 : just == "space-around" ? 4 : 0;
            fs.align = algn == "center" ? 1 : algn == "flex-end" ? 2 : 0;
            fs.autoH = sh.empty() ? 1 : 0;
            fs.gap = gap;
            specFlex.push_back(fs);
        }

        stack.push_back({ n.id, x + pad, y + pad, 0, w - 2 * pad, nodes.size() - 1,
                          textCol, hasOwnText || pc.hasText, !hasY && !flexPlaced });
    }

    // 收尾：弹出剩余容器并自动扩展，最后扩展根
    while (stack.size() > 1)
        popAndPropagate();
    growToContent(stack[0]);

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
    const char strings[] =
        "9:41\0"
        "Material Demo\0"
        "Card Title\0"
        "This is a Material Design card\0"
        "with a dark theme and rounded\0"
        "corners.\0"
        "CLICK ME\0"
        "Enter text...\0"
        "+\0"
        "HOME\0"
        "FAV\0"
        "SETTINGS\0";
    const uint32_t nodeCount = 17;
    const uint32_t strSize = sizeof(strings);

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
    n.width = 480; n.height = 800; n.color = 0xFF121212;
    add(n);

    // 1: Status bar background
    memset(&n, 0, sizeof(n));
    n.id = 1; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 0; n.y = 0; n.width = 480; n.height = 36;
    n.color = 0xFF000000;
    add(n);

    // 2: Status bar time
    memset(&n, 0, sizeof(n));
    n.id = 2; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 420; n.y = 6; n.width = 50; n.height = 24;
    n.color = 0xFFFFFFFF; n.textOffset = 0; n.fontSize = 13;
    add(n);

    // 3: App bar background
    memset(&n, 0, sizeof(n));
    n.id = 3; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 0; n.y = 36; n.width = 480; n.height = 56;
    n.color = 0xFF1976D2;
    add(n);

    // 4: App bar title
    memset(&n, 0, sizeof(n));
    n.id = 4; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 16; n.y = 46; n.width = 300; n.height = 36;
    n.color = 0xFFFFFFFF; n.textOffset = 5; n.fontSize = 20;
    add(n);

    // 5: Card 1 surface
    memset(&n, 0, sizeof(n));
    n.id = 5; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 16; n.y = 112; n.width = 448; n.height = 140;
    n.color = 0xFF1E1E1E; n.radius = 12;
    add(n);

    // 6: Card 1 title
    memset(&n, 0, sizeof(n));
    n.id = 6; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 124; n.width = 200; n.height = 28;
    n.color = 0xFFFFFFFF; n.textOffset = 18; n.fontSize = 18;
    add(n);

    // 7: Card 1 body line 1
    memset(&n, 0, sizeof(n));
    n.id = 7; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 156; n.width = 416; n.height = 20;
    n.color = 0xFF9E9E9E; n.textOffset = 29; n.fontSize = 13;
    add(n);

    // 8: Card 1 body line 2
    memset(&n, 0, sizeof(n));
    n.id = 8; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 178; n.width = 416; n.height = 20;
    n.color = 0xFF9E9E9E; n.textOffset = 66; n.fontSize = 13;
    add(n);

    // 9: Card 1 body line 3
    memset(&n, 0, sizeof(n));
    n.id = 9; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 200; n.width = 416; n.height = 20;
    n.color = 0xFF9E9E9E; n.textOffset = 95; n.fontSize = 13;
    add(n);

    // 10: Material button (purple accent)
    memset(&n, 0, sizeof(n));
    n.id = 10; n.parentId = 0; n.type = BINUI_BUTTON; n.flags = 5;
    n.x = 32; n.y = 276; n.width = 416; n.height = 48;
    n.color = 0xFFBB86FC; n.textOffset = 106; n.fontSize = 14; n.radius = 8;
    add(n);

    // 11: Outlined input field (underline style)
    memset(&n, 0, sizeof(n));
    n.id = 11; n.parentId = 0; n.type = BINUI_INPUT; n.flags = 5;
    n.x = 32; n.y = 348; n.width = 416; n.height = 48;
    n.color = 0xFF1E1E1E; n.textOffset = 115; n.fontSize = 14;
    add(n);

    // 12: Divider line
    memset(&n, 0, sizeof(n));
    n.id = 12; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 32; n.y = 420; n.width = 416; n.height = 1;
    n.color = 0xFF2E2E2E;
    add(n);

    // 13: Card 2 surface
    memset(&n, 0, sizeof(n));
    n.id = 13; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 16; n.y = 440; n.width = 448; n.height = 80;
    n.color = 0xFF1E1E1E; n.radius = 12;
    add(n);

    // 14: Card 2 text
    memset(&n, 0, sizeof(n));
    n.id = 14; n.parentId = 0; n.type = BINUI_LABEL; n.flags = 1;
    n.x = 32; n.y = 460; n.width = 416; n.height = 24;
    n.color = 0xFFFFFFFF; n.textOffset = 130; n.fontSize = 15;
    add(n);

    // 15: Bottom nav background
    memset(&n, 0, sizeof(n));
    n.id = 15; n.parentId = 0; n.type = BINUI_LIST; n.flags = 1;
    n.x = 0; n.y = 752; n.width = 480; n.height = 48;
    n.color = 0xFF1E1E1E;
    add(n);

    // 16: Floating action button (purple circle)
    memset(&n, 0, sizeof(n));
    n.id = 16; n.parentId = 0; n.type = BINUI_IMAGE; n.flags = 5;
    n.x = 400; n.y = 690; n.width = 56; n.height = 56;
    n.color = 0xFFBB86FC; n.textOffset = 135; n.fontSize = 14; n.radius = 28;
    add(n);

    memcpy(ptr, strings, strSize);

    WinDesc wdesc;
    wdesc.present = true;
    wdesc.width = 480; wdesc.height = 800;
    wdesc.bgColor = 0xFF121212;
    wdesc.title = "binUI Demo";
    AppendWindowDesc(buf, wdesc);
    // 手写布局没有规格/动画/视频/状态段，但 v12 的两段恒定写出，保证解析链一致
    AppendImageSection(buf, {});
    AppendComponentSection(buf, {});
    AppendPaintSection(buf, {});

    return buf;
}

} // namespace binui
