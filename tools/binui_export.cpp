// binui_export.cpp - 把 .bui 还原成打包前的目录结构（靠旁挂的 .bui.srcmap）
//
// 用法:
//   binui_export <x.bui|.bum> [-o <outDir>] [-q] [--list]
//
//   <x.bui>  由 binui_compile 产出的布局；同目录要有 binui.res（清单）
//   -o       输出目录（默认：<布局文件名>.src/）
//   --list   只列出清单里的路径，不写文件
//
// 打包清单里存的是**原始文件字节** + 相对 appRootDir 的路径（见 include/binui_srcmap.h），
// 所以还原出来的就是打包前的那些文件本身 —— 不是"节点树反推的等价 HTML"那种有损转换。
//
// 没带 binui.res 的包不可解包（清单要显式 `--res` 才生成，默认不带），但**不影响运行**：
// 布局里没有任何东西依赖它。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "binui_asset.h"
#include "binui_srcmap.h"

namespace fs = std::filesystem;

namespace {

void Usage() {
    std::printf(
        "用法: binui_export <x.bui|.bum> [-o <outDir>] [-q] [--list]\n"
        "  <x.bui>  由 binui_compile 产出的布局，同目录需要 binui.res\n"
        "  -o       输出目录（默认 <布局文件名>.src/）\n"
        "  --list   只列路径不写文件\n"
        "  -q       静默（只打错误）\n");
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 路径必须落在 outDir 里面：拒绝绝对路径与 ".."
bool SafeRelPath(const std::string& p) {
    if (p.empty()) return false;
    if (p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\')) return false;
    std::string norm;
    size_t i = 0;
    while (i <= p.size()) {
        const size_t j = p.find_first_of("/\\", i);
        const std::string seg = p.substr(i, (j == std::string::npos ? p.size() : j) - i);
        if (seg == "..") return false;
        i = (j == std::string::npos) ? p.size() + 1 : j + 1;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string in, outDir;
    bool quiet = false, listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "-h" || a == "--help") { Usage(); return 0; }
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "--list") listOnly = true;
        else if (a == "-o" && i + 1 < argc) { ++i; outDir = argv[i] ? argv[i] : ""; }
        else if (!a.empty() && a[0] != '-' && in.empty()) in = a;
        else { std::fprintf(stderr, "binui_export: 无法识别的参数 \"%s\"\n", a.c_str()); Usage(); return 1; }
    }
    if (in.empty()) { Usage(); return 1; }

    // 输入可以是布局文件，也可以直接指清单（binui.res / *.srcmap）
    // 查找顺序：<x.bui>.srcmap（旧约定，一 UI 一清单）→ 同目录 binui.res（新约定，一目录一份）
    std::string mapPath = in;
    const std::string low = binui::AssetLower(in);
    if (!binui::AssetEndsWith(low, ".srcmap") && !binui::AssetEndsWith(low, ".res")) {
        const std::filesystem::path inPath(in);
        const std::filesystem::path dir =
            inPath.has_parent_path() ? inPath.parent_path() : std::filesystem::path(".");
        mapPath.clear();
        std::error_code ec;
        const std::string resPath = (dir / "binui.res").generic_string();
        if (std::filesystem::is_regular_file(resPath, ec)) mapPath = resPath;
        else mapPath = in + ".srcmap";
    }

    std::vector<uint8_t> mapBytes;
    if (!ReadFile(mapPath, mapBytes)) {
        std::fprintf(stderr, "binui_export: 找不到打包清单 %s\n", mapPath.c_str());
        std::fprintf(stderr, "  （这个包没带 binui.res，无法解包。编译时加 --res 即可产出。）\n");
        return 1;
    }
    std::vector<binui::SrcMapEntry> entries;
    if (!binui::ParseSrcMap(mapBytes.data(), mapBytes.size(), entries) || entries.empty()) {
        std::fprintf(stderr, "binui_export: %s 不是有效的打包清单\n", mapPath.c_str());
        return 1;
    }

    if (listOnly) {
        for (const auto& e : entries) std::printf("%8zu  %s\n", e.data.size(), e.path.c_str());
        std::printf("共 %zu 项\n", entries.size());
        return 0;
    }

    if (outDir.empty()) {
        std::string base = fs::path(in).filename().generic_string();
        outDir = base + ".src";
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "binui_export: 建不了输出目录 %s: %s\n", outDir.c_str(), ec.message().c_str());
        return 1;
    }

    size_t written = 0, skipped = 0;
    for (const auto& e : entries) {
        if (!SafeRelPath(e.path)) {
            std::fprintf(stderr, "binui_export: 跳过可疑路径 \"%s\"（必须相对、且不含 ..）\n", e.path.c_str());
            ++skipped;
            continue;
        }
        const fs::path dst = fs::path(outDir) / fs::path(e.path);
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream f(dst, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "binui_export: 写不了 %s\n", dst.generic_string().c_str());
            ++skipped;
            continue;
        }
        f.write(reinterpret_cast<const char*>(e.data.data()),
                static_cast<std::streamsize>(e.data.size()));
        ++written;
        if (!quiet) std::printf("  %-52s %8zu 字节\n", e.path.c_str(), e.data.size());
    }

    if (!quiet)
        std::printf("[binui_export] %s -> %s/（%zu 个文件%s）\n", mapPath.c_str(), outDir.c_str(), written,
                    skipped ? "，有跳过" : "");
    return skipped ? 2 : 0;
}
