// binui_compile.cpp - UI / 组件源码 -> 二进制布局（.bui / .bum）
//
// 用法:
//   binui_compile <src> [-o <out>] [-q] [--no-srcmap]
//
//   <src>  .bui.html / .bum.html / .html / .xml / 已编译的 .bui|.bum
//   -o     输出路径（默认：x.bum.html -> x.bum，x.bui.html -> x.bui）
//   -q     静默（只打错误）
//   --no-srcmap  不产出 <out>.srcmap
//
// 默认会在 <out> 所在目录写一份 **打包清单** `binui.res`：主 UI 与每个被内联/外挂的
// 组件的**原始字节** + 相对 appRootDir 的路径。binui_export 靠它把 .bui 还原成打包前
// 的目录结构（逐字节一致）。可有可无：不带它就是不可解包，运行完全不受影响。
// 一个目录一份（多个 .bui 编到同一目录会合并），--no-srcmap 不生成。
//
// 约定（docs/components.md）：
//   .bui  单个 UI（应用入口，可直接 loadLayout）
//   .bum  组件（被 .bui 内联，或运行期 mountComponent）
//   两者都可以只发源码、只发二进制，或两种都发；加载时二进制优先。
//
// 退出码：0 = 成功（允许编译告警）；1 = 失败（读不到源码 / 编译结果为空）
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_link.h"
#include "binui_srcmap.h"

namespace {

void Usage() {
    std::printf("用法: binui_compile <src> [-o <out>] [-q] [--no-srcmap]\n"
                "  <src>  .bui.html / .bum.html / .html / .xml / 已编译的 .bui|.bum\n"
                "  -o     输出路径（默认同目录：x.bum.html -> x.bum，x.bui.html -> x.bui）\n"
                "  -q     静默（只打错误）\n"
                "  --no-srcmap  不产出 binui.res（打包清单，binui_export 靠它解包）\n");
}

bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() > n && s.compare(s.size() - n, n, tail) == 0;
}

// 默认输出名：x.bum.html -> x.bum；x.bui.xml -> x.bui；x.html -> x.bui
std::string DefaultOut(const std::string& in) {
    std::string base = in;
    if (EndsWith(base, ".xml")) {
        base.resize(base.size() - 4);
        if (EndsWith(base, ".bum") || EndsWith(base, ".bui") || EndsWith(base, ".binui"))
            return base;
        return base + ".bui";
    }
    if (EndsWith(base, ".html") || EndsWith(base, ".htm")) {
        base.resize(base.find_last_of('.'));
        // x.bum.html / x.bui.html / x.binui.html：组件源码，去掉的是 .html 而不是
        // 整段后缀（以前这里直接把 ".html" 换成 ".bui"，x.bum.html 会输出 x.bum.bui）。
        if (EndsWith(base, ".bum") || EndsWith(base, ".bui") || EndsWith(base, ".binui"))
            return base;
        return base + ".bui";
    }
    if (EndsWith(base, ".bum")) return base;
    return base + ".bui";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string in, out;
    bool quiet = false;
    bool srcmap = true;   // 默认产出 .bui.srcmap（想"不泄露源码"就 --no-srcmap）
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "-h" || a == "--help") { Usage(); return 0; }
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "--no-srcmap") srcmap = false;
        else if (a == "--srcmap") srcmap = true;
        else if (a == "-o" && i + 1 < argc) { ++i; out = argv[i] ? argv[i] : ""; }
        else if (a.size() && a[0] != '-' && in.empty()) in = a;
        else {
            std::fprintf(stderr, "binui_compile: 无法识别的参数 \"%s\"\n", a.c_str());
            Usage();
            return 1;
        }
    }
    if (in.empty()) { Usage(); return 1; }
    if (out.empty()) out = DefaultOut(in);

    // 已经是二进制就原样拷贝（便于"源码/二进制"两条流水线共用一条命令）
    {
        std::vector<uint8_t> raw;
        {
            FILE* f = std::fopen(in.c_str(), "rb");
            if (!f) {
                std::fprintf(stderr, "binui_compile: 读不到 %s\n", in.c_str());
                return 1;
            }
            uint8_t buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) raw.insert(raw.end(), buf, buf + n);
            std::fclose(f);
        }
        if (binui::IsLayoutData(raw.data(), raw.size())) {
            FILE* f = std::fopen(out.c_str(), "wb");
            if (!f) {
                std::fprintf(stderr, "binui_compile: 无法写入 %s\n", out.c_str());
                return 1;
            }
            std::fwrite(raw.data(), 1, raw.size(), f);
            std::fclose(f);
            if (!quiet) std::printf("[binui_compile] %s 已是二进制布局，原样拷贝到 %s\n", in.c_str(), out.c_str());
            return 0;
        }
    }

    binui::CompileOptions opt;
    opt.emitSrcMap = srcmap;
    const binui::CompileResult res = binui::CompileBinUIFileEx(in, opt);
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[binui_compile] warning: %s\n", w.c_str());
    if (res.layout.empty()) {
        std::fprintf(stderr, "binui_compile: 编译 %s 失败（空布局）\n", in.c_str());
        return 1;
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        // 输出目录可能还不存在（-o 指到新目录很常见），建一次再试
        std::error_code ec;
        const auto parent = std::filesystem::path(out).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        f = std::fopen(out.c_str(), "wb");
    }
    if (!f) {
        std::fprintf(stderr, "binui_compile: 无法写入 %s\n", out.c_str());
        return 1;
    }
    std::fwrite(res.layout.data(), 1, res.layout.size(), f);
    std::fclose(f);

    // 打包清单：<out 所在目录>/binui.res（binui_export 靠它还原打包前的目录结构）。
    // 一个目录一份清单：多个 .bui 编到同一目录时会合并（同路径以本次为准），
    // 所以"清单 + 原路径"是整包级别的，而不是每个 .bui 一个。
    bool resOk = false;
    if (!res.srcMap.empty()) {
        const std::filesystem::path outPath(out);
        const std::filesystem::path mapPath =
            (outPath.has_parent_path() ? outPath.parent_path() : std::filesystem::path(".")) / "binui.res";

        std::vector<uint8_t> merged = res.srcMap;
        // 已有清单 -> 先读进来，按路径合并
        {
            std::vector<uint8_t> oldBytes;
            std::ifstream oldIn(mapPath, std::ios::binary);
            if (oldIn) {
                oldBytes.assign(std::istreambuf_iterator<char>(oldIn), std::istreambuf_iterator<char>());
                std::vector<binui::SrcMapEntry> oldEnts, newEnts;
                if (binui::ParseSrcMap(oldBytes.data(), oldBytes.size(), oldEnts) &&
                    binui::ParseSrcMap(res.srcMap.data(), res.srcMap.size(), newEnts)) {
                    std::vector<binui::SrcMapEntry> all = oldEnts;
                    for (auto& ne : newEnts) {
                        bool replaced = false;
                        for (auto& oe : all) {
                            if (oe.path == ne.path) { oe = ne; replaced = true; break; }
                        }
                        if (!replaced) all.push_back(ne);
                    }
                    merged = binui::BuildSrcMap(all);
                }
            }
        }

        FILE* mf = std::fopen(mapPath.generic_string().c_str(), "wb");
        if (mf) {
            std::fwrite(merged.data(), 1, merged.size(), mf);
            std::fclose(mf);
            resOk = true;
        } else {
            std::fprintf(stderr, "binui_compile: 无法写入 %s（布局已正常产出）\n",
                         mapPath.generic_string().c_str());
        }
    }

    if (!quiet) {
        std::printf("[binui_compile] %s -> %s（%zu 字节", in.c_str(), out.c_str(), res.layout.size());
        if (!res.images.empty()) std::printf("，资源 %zu 项", res.images.size());
        if (resOk) std::printf("，打包清单 %zu 字节", res.srcMap.size());
        std::printf("）\n");
    }
    return 0;
}
