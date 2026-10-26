// check_examples.cpp - 示例 UI 零警告检查
// 用法: check_examples [目录...]（默认 examples）
// 对目录下所有 *.html 与 *.binui.xml 逐个编译：编译为空或产生任何编译警告都算失败。
// "写了没效果" 是 bug —— 这条检查就是防止示例 UI 里悄悄积累不支持的写法。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "binui_compiler.h"

namespace fs = std::filesystem;

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
            if (p.size() > 5 && p.compare(p.size() - 5, 5, ".html") == 0)
                files.push_back(p);
            else if (p.size() > 11 && p.compare(p.size() - 11, 11, ".binui.xml") == 0)
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
        std::printf("OK   %s: %zu bytes, %zu images\n", f.c_str(), r.layout.size(), r.images.size());
    }
    std::printf("\n%d 个示例 UI 检查完毕，%d 处问题\n", (int)files.size(), failed);
    return failed > 0 ? 1 : 0;
}
