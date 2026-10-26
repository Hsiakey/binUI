// check_examples.cpp - 示例 UI 零警告检查
// 用法: check_examples [目录...]（默认 examples）
// 对目录下所有 UI 源码（*.html / *.bum.xml / *.bui.xml / *.binui.xml）逐个编译：
// 编译为空或产生任何编译警告都算失败。
// "写了没效果" 是 bug —— 这条检查就是防止示例 UI 里悄悄积累不支持的写法。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "binui_asset.h"
#include "binui_compiler.h"
#include "binui_link.h"

namespace fs = std::filesystem;

static bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() > n && s.compare(s.size() - n, n, tail) == 0;
}

int main(int argc, char** argv) {
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; i++) dirs.emplace_back(argv[i]);
    if (dirs.empty()) dirs.push_back("examples");

    std::vector<std::string> files;
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::exists(d, ec)) {
            std::fprintf(stderr, "[check_examples] 目录不存在: %s\n", d.c_str());
            return 2;
        }
        for (const auto& ent : fs::recursive_directory_iterator(d, ec)) {
            if (!ent.is_regular_file()) continue;
            // mockup*.html 是设计稿/对照稿（不是构建产物），允许出现未支持的写法
            const std::string base = ent.path().filename().generic_string();
            if (base.rfind("mockup", 0) == 0) continue;
            const std::string p = ent.path().generic_string();
            if (EndsWith(p, ".html") || EndsWith(p, ".bum.xml") || EndsWith(p, ".bui.xml") ||
                EndsWith(p, ".binui.xml"))
                files.push_back(p);
        }
        if (ec) {
            std::fprintf(stderr, "[check_examples] 遍历 %s 失败: %s\n", d.c_str(), ec.message().c_str());
            return 2;
        }
    }

    int failed = 0;
    for (const auto& f : files) {
        binui::CompileResult r = binui::CompileBinUIFileEx(f);
        if (r.layout.empty()) {
            std::printf("FAIL %s: 编译结果为空\n", f.c_str());
            failed++;
            continue;
        }
        for (const auto& w : r.warnings) {
            std::printf("WARN %s: %s\n", f.c_str(), w.c_str());
            failed++;
        }
        // 外挂组件（<use ... external>）：引用的组件必须真的能解析到（二进制或源码），
        // 否则运行期会静默少一块界面 —— 这类问题值得在 CI 就拦住
        const std::string srcDir = fs::path(f).parent_path().generic_string();
        for (const auto& ref : binui::ReadComponentRefs(r.layout.data(), r.layout.size())) {
            const binui::AssetFile af = binui::ProbeAsset(srcDir, ref.src);
            if (!af.valid()) {
                std::printf("FAIL %s: 外挂组件缺失 %s（锚点 #%u）\n",
                            f.c_str(), ref.src.c_str(), (unsigned)ref.anchorId);
                failed++;
            } else {
                std::printf("OK   %s: 外挂组件 %s -> %s\n", f.c_str(), ref.src.c_str(),
                            af.kind == binui::AssetKind::BinaryLayout ? "二进制" : "源码");
            }
        }
        std::printf("OK   %s: %zu bytes, %zu images\n", f.c_str(), r.layout.size(), r.images.size());
    }
    std::printf("\n%d 个示例 UI 检查完毕，%d 处问题\n", (int)files.size(), failed);
    return failed > 0 ? 1 : 0;
}
