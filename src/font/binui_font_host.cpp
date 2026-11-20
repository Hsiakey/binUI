// binui_font_host.cpp —— 平台字体发现（家族名 → 字体文件路径）
//
// 这份文件是"平台 API 留在外壳"的落点：EnumFontFamiliesEx / fontconfig / CoreText
// 的活儿都该在这里，而 bin2d 的文字模块只收一个 `FaceResolver`（名字 → 路径）。
//
// 注册表的形状与迁移前逐项一致（同一个家族名 → 同一个文件、同一条回退顺序），
// 因为"字体链命中顺序"会直接改变字形来源 —— 那也是像素回归最容易悄悄变的地方。
#include "font/binui_font_host.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "bin2d/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace binui {
namespace {

#ifdef _WIN32

constexpr const char* kFontDir = "C:/Windows/Fonts/";

bool FileExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 家族名 → 候选文件名（按迁移前的**登记顺序**，后面的覆盖前面的）。
// 顺序有意义：msyh.ttc 与 msyh.ttf 同时在时，老实现最后登记的是 ttf，这里保持同一结果。
struct FamilyFiles { const char* family; const char* files[3]; };

const FamilyFiles kWindowsFamilies[] = {
    { "Segoe UI",         { "segoeui.ttf", nullptr, nullptr } },
    { "Segoe UI Emoji",   { "seguiemj.ttf", nullptr, nullptr } },
    { "Segoe UI Symbol",  { "seguisym.ttf", nullptr, nullptr } },
    { "Microsoft YaHei",  { "msyh.ttc", "msyh.ttf", nullptr } },
    { "SimHei",           { "simhei.ttf", nullptr, nullptr } },
    { "SimSun",           { "simsun.ttc", nullptr, nullptr } },
    { "Arial",            { "arial.ttf", nullptr, nullptr } },
    // 回退链的"深水区"：中日韩生僻字、日文假名、韩文、印度语系、泰文
    { "MS Gothic",        { "msgothic.ttc", nullptr, nullptr } },
    { "Malgun Gothic",    { "malgun.ttf", nullptr, nullptr } },
    { "Nirmala UI",       { "Nirmala.ttf", nullptr, nullptr } },
    { "Leelawadee UI",    { "Leelaw.ttf", nullptr, nullptr } },
};

// 回退顺序就是"问字体的顺序"（命中即停）：
//   · 拉丁 —— Segoe UI
//   · 中日韩 —— 雅黑（界面观感最正）
//   · 符号 —— Segoe UI Symbol 在 Emoji 之前：⌘⌫⎋ 这类要黑白符号，
//     不能让它们落到 emoji 字体、被画成彩色图形（那是选面选错了，不是缺字）
//   · 真 emoji —— Segoe UI Emoji（含 COLR 彩色层）
//   · 生僻汉字 / 假名 / 韩文 / 印度语系 / 泰文 —— 依次兜底
//   · Arial 最后兜底（某些老系统上它覆盖最全）
const char* const kWindowsChain[] = {
    "Segoe UI", "Microsoft YaHei", "Segoe UI Symbol", "Segoe UI Emoji",
    "SimSun", "SimHei", "MS Gothic", "Malgun Gothic",
    "Nirmala UI", "Leelawadee UI", "Arial",
};

std::string ResolveWindowsFamily(const std::string& family) {
    for (const auto& f : kWindowsFamilies) {
        if (family != f.family) continue;
        std::string found;
        for (const char* file : f.files) {
            if (!file) break;
            std::string path = std::string(kFontDir) + file;
            if (FileExists(path)) found = path;   // 后面的覆盖前面的（与迁移前一致）
        }
        return found;
    }
    return std::string();
}

#else

bool FileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

struct Cand { const char* face; const char* path; bool cjk; };

// 非 Windows：按发行版/系统的常见安装路径探测（Linux 各发行版 + macOS）。
// 一个都没有也不报错 —— 应用可以自己给 Library 登记字体。
const Cand kCands[] = {
    {"Noto Sans CJK SC", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",      true},
    {"Noto Sans CJK SC", "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",      true},
    {"Noto Sans CJK JP", "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",    true},
    {"Noto Sans CJK JP", "/usr/share/fonts/truetype/noto/NotoSansCJKjp-Regular.otf",    true},
    {"WenQuanYi Micro Hei", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",           true},
    {"PingFang SC", "/System/Library/Fonts/PingFang.ttc",                               true},
    {"DejaVu Sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",                  false},
    {"Liberation Sans", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", false},
    {"Helvetica", "/System/Library/Fonts/Helvetica.ttc",                                false},
    // 符号 / emoji：回退链后半段，只有前面的面都不覆盖时才会被读到
    {"Noto Color Emoji", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",           false},
    {"Noto Color Emoji", "/usr/share/fonts/noto/NotoColorEmoji.ttf",                    false},
    {"Symbola", "/usr/share/fonts/truetype/ancient-scripts/Symbola_hint.ttf",           false},
    {"Apple Color Emoji", "/System/Library/Fonts/Apple Color Emoji.ttc",                false},
};

// 探测结果缓存（首次调用建一次）：家族名 → 路径。
// 别名（Segoe UI / Microsoft YaHei / Noto Sans）指向实际可用的那个文件 ——
// 上层代码与示例都按这三个名字取字体，跨平台就不用改调用方。
struct PosixTable {
    std::map<std::string, std::string> byName;
    std::vector<std::string> chain;
    std::string defaultPath;
};

const PosixTable& PosixFonts() {
    static const PosixTable table = [] {
        PosixTable t;
        std::vector<std::string> discovered;
        const char* firstCjk = nullptr;
        const char* firstAny = nullptr;
        for (const auto& c : kCands) {
            if (!FileExists(c.path)) continue;
            t.byName[c.face] = c.path;   // 同名后面的覆盖前面的（与迁移前一致）
            bool dup = false;
            for (const auto& n : discovered)
                if (n == c.face) { dup = true; break; }
            if (!dup) discovered.push_back(c.face);
            if (!firstAny) firstAny = c.path;
            if (!firstCjk && c.cjk) firstCjk = c.path;
        }
        const char* dflt = firstCjk ? firstCjk : firstAny;
        if (dflt) {
            t.byName["Segoe UI"] = dflt;
            t.byName["Microsoft YaHei"] = dflt;
            t.byName["Noto Sans"] = dflt;
            t.defaultPath = dflt;
            // 名字统一到上面三个别名（上层只认这三个），顺序即回退顺序
            t.chain = { "Segoe UI", "Microsoft YaHei", "Noto Sans" };
            for (const auto& n : discovered) t.chain.push_back(n);
        }
        return t;
    }();
    return table;
}

#endif

} // namespace

void RegisterSystemFonts(bin2d::text::Library& lib) {
    // 注入的不是"字体列表"而是一个**查询函数**：模块只在需要某个家族时问一次。
    // 这就是 issue 12 拍板的第三条 —— bin2d 只定义契约，枚举留在这里。
    lib.setResolver([](const bin2d::text::FaceDesc& d) -> std::string {
#ifdef _WIN32
        return ResolveWindowsFamily(d.family);
#else
        const PosixTable& t = PosixFonts();
        auto it = t.byName.find(d.family);
        return it == t.byName.end() ? std::string() : it->second;
#endif
    });

#ifdef _WIN32
    lib.setDefaultFace("Segoe UI");
    std::vector<std::string> chain;
    for (const char* n : kWindowsChain) chain.push_back(n);
    lib.setFallbackChain(chain);   // 本机没有的家族由模块按 Resolver 过滤掉
#else
    const PosixTable& t = PosixFonts();
    if (!t.defaultPath.empty()) {
        lib.setDefaultFace("Segoe UI");
        lib.setFallbackChain(t.chain);
    }
#endif
}

} // namespace binui
