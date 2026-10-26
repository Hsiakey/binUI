// binui_asset.h - 资产文件探测：二进制优先、源码更新则优先源码
//
// binUI 的组件化允许作者按任意组合分发：
//
//   单个 UI：      main.bui                                   （内嵌式，一个文件）
//   组件式：       main.bui + titlebar.bum + button.bum       （外挂式）
//   混放：         main.bui + titlebar.bum + button.bum.xml   （二进制 + 源码）
//   双份：         button.bum + button.bum.xml                （二进制优先，源码可改/可热更新）
//
// 本头文件只做"给定目录与 src，选出该用哪个文件"，不依赖任何平台 API
// （纯 <filesystem>/<fstream>），编译器与运行期加载器共用同一份规则。
//
// 规则：
//   1. 显式扩展名按扩展名：.bum/.bui/.binui = 二进制；.xml/.html/.htm = 源码；
//      无扩展名则依次探测 <src>.bum / <src>.bui → <src>.bum.xml / <src>.bui.xml / <src>.html / <src>.xml
//   2. 二进制存在 → 用二进制（快、可发行、保护源码）
//   3. 但源码 mtime 比二进制新 → 改用源码（"改了没重新编译"不该是静默陷阱），
//      并置 sourceNewer 让调用方给一条告警
//   4. 文件内容兜底：扩展名说二进制但 magic 不对（或反之）→ 按内容判定
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "binui_format.h"

namespace binui {

enum class AssetKind { None, BinaryLayout, SourceXml };

struct AssetFile {
    std::string path;         // 选中的文件（相对 dir 已拼好）
    AssetKind   kind = AssetKind::None;
    bool        hasBinary = false;  // 同名二进制存在
    bool        hasSource = false;  // 同名源码存在
    bool        sourceNewer = false;// 二进制存在，但源码更新（本次选了源码）
    bool valid() const { return kind != AssetKind::None; }
};

inline bool AssetEndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() > n && s.compare(s.size() - n, n, tail) == 0;
}

inline std::string AssetLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string AssetJoin(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return rel;
    return (std::filesystem::path(dir) / rel).lexically_normal().generic_string();
}

inline bool AssetFileNewer(const std::string& a, const std::string& b) {
    std::error_code ec1, ec2;
    const auto ta = std::filesystem::last_write_time(a, ec1);
    const auto tb = std::filesystem::last_write_time(b, ec2);
    if (ec1 || ec2) return false;
    return ta > tb;
}

// 文件内容判定：前 4 字节是否是 binUI magic
inline bool AssetIsBinaryFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return magic == BINUI_MAGIC || magic == BINUI_HTML_MAGIC;
}

// 给定目录与 src，选出该用哪个组件/UI 文件
inline AssetFile ProbeAsset(const std::string& dir, const std::string& src) {
    AssetFile out;
    if (src.empty()) return out;
    const std::string low = AssetLower(src);
    std::vector<std::string> binCands, srcCands;
    if (AssetEndsWith(low, ".bum") || AssetEndsWith(low, ".bui") || AssetEndsWith(low, ".binui")) {
        binCands.push_back(src);
        srcCands.push_back(src + ".xml");
    } else if (AssetEndsWith(low, ".xml") || AssetEndsWith(low, ".html") || AssetEndsWith(low, ".htm")) {
        srcCands.push_back(src);
    } else {
        binCands = {src + ".bum", src + ".bui", src + ".binui"};
        srcCands = {src + ".bum.xml", src + ".bui.xml", src + ".binui.xml",
                    src + ".html", src + ".xml"};
    }
    auto pick = [&](const std::vector<std::string>& cands, bool wantBinary) -> std::string {
        for (const auto& c : cands) {
            const std::string p = AssetJoin(dir, c);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(p, ec)) continue;
            if (AssetIsBinaryFile(p) != wantBinary) continue; // 内容兜底
            return p;
        }
        return {};
    };
    const std::string bin = pick(binCands, true);
    const std::string src2 = pick(srcCands, false);
    out.hasBinary = !bin.empty();
    out.hasSource = !src2.empty();
    if (!bin.empty() && (src2.empty() || !AssetFileNewer(src2, bin))) {
        out.path = bin;
        out.kind = AssetKind::BinaryLayout;
        return out;
    }
    if (!src2.empty()) {
        out.path = src2;
        out.kind = AssetKind::SourceXml;
        out.sourceNewer = !bin.empty();
        return out;
    }
    if (!bin.empty()) {
        out.path = bin;
        out.kind = AssetKind::BinaryLayout;
    }
    return out;
}

} // namespace binui
