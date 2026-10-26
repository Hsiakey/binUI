// binui_compile.cpp - UI / 组件源码 -> 二进制布局（.bui / .bum）
//
// 用法:
//   binui_compile <src> [-o <out>] [-q]
//
//   <src>  .bui.xml / .bum.xml / .html / .xml / 已编译的 .bui|.bum
//   -o     输出路径（默认：与源文件同目录，去掉 .xml 换成 .bui/.bum）
//   -q     静默（只打错误）
//
// 约定（docs/components.md）：
//   .bui  单个 UI（应用入口，可直接 loadLayout）
//   .bum  组件（被 .bui 内联，或运行期 mountComponent）
//   两者都可以只发源码、只发二进制，或两种都发；加载时二进制优先。
//
// 退出码：0 = 成功（允许编译告警）；1 = 失败（读不到源码 / 编译结果为空）
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "binui_compiler.h"
#include "binui_link.h"

namespace {

void Usage() {
    std::printf("用法: binui_compile <src> [-o <out>] [-q]\n"
                "  <src>  .bui.xml / .bum.xml / .html / .xml\n"
                "  -o     输出路径（默认同目录，.xml -> .bui/.bum）\n"
                "  -q     静默（只打错误）\n");
}

bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() > n && s.compare(s.size() - n, n, tail) == 0;
}

// 默认输出名：x.bum.xml -> x.bum；x.bui.xml -> x.bui；x.html -> x.bui
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
        return base + ".bui";
    }
    if (EndsWith(base, ".bum")) return base;
    return base + ".bui";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string in, out;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "-h" || a == "--help") { Usage(); return 0; }
        else if (a == "-q" || a == "--quiet") quiet = true;
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

    const binui::CompileResult res = binui::CompileBinUIFileEx(in);
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "[binui_compile] warning: %s\n", w.c_str());
    if (res.layout.empty()) {
        std::fprintf(stderr, "binui_compile: 编译 %s 失败（空布局）\n", in.c_str());
        return 1;
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "binui_compile: 无法写入 %s\n", out.c_str());
        return 1;
    }
    std::fwrite(res.layout.data(), 1, res.layout.size(), f);
    std::fclose(f);

    if (!quiet) {
        std::printf("[binui_compile] %s -> %s（%zu 字节", in.c_str(), out.c_str(), res.layout.size());
        if (!res.images.empty()) std::printf("，资源 %zu 项", res.images.size());
        std::printf("）\n");
    }
    return 0;
}
