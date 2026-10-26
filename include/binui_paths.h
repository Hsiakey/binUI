// binui_paths.h - 可执行文件 / UI / 资源 路径解析（跨平台，header-only）
//
// 解决一个真实 bug：以前 7 个 demo 各自复制一份 ExeDir(argv[0])，
// 通过 PATH 启动、软链接调用、二进制被改名时 argv[0] 根本不是路径 -> 图片全 MISS。
//
// 规则（见 docs/app_layout.md §3）：
//   1. exeDir 用系统 API 取真实路径（Windows GetModuleFileName / Linux /proc/self/exe），不用 argv[0]
//   2. 不依赖 CWD（桌面环境双击启动时 CWD 通常是 $HOME 或 /）
//   3. 资源路径一律相对「UI 文件所在目录」解析；src 字符串逐字节作键，不做归一化
//
// 已知局限：Windows 下路径按 ANSI（CP_ACP）处理，与全项目 std::ifstream(char*) 一致；
// 非 ASCII（中文）路径属既有局限，不在本次修复范围。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <limits.h>
#  include <unistd.h>
#endif

namespace binui {
namespace ex {

// ============ 基础路径工具 ============

// 目录部分（含末尾分隔符）；无分隔符返回空
inline std::string DirOf(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

inline bool IsAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() > 1 && p[1] == ':';   // Windows 盘符
}

// 拼路径：b 为绝对路径时直接返回 b；a 末尾没有分隔符则按其分隔符风格补一个
inline std::string Join(const std::string& a, const std::string& b) {
    if (IsAbsolutePath(b) || a.empty()) return b;
    const char sep = (a.back() == '/' || a.back() == '\\')
                         ? a.back()
                         : (a.find('\\') != std::string::npos ? '\\' : '/');
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + sep + b;
}

// 当前可执行文件绝对路径（失败返回空）
inline std::string ExePath() {
#if defined(_WIN32)
    wchar_t wbuf[MAX_PATH * 2] = {};
    DWORD wn = ::GetModuleFileNameW(nullptr, wbuf,
                                    static_cast<DWORD>(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn == 0) return {};
    char buf[MAX_PATH * 4] = {};
    int n = ::WideCharToMultiByte(CP_ACP, 0, wbuf, static_cast<int>(wn),
                                  buf, static_cast<int>(sizeof(buf)) - 1, nullptr, nullptr);
    if (n <= 0) return {};
    return std::string(buf, static_cast<size_t>(n));
#else
    char buf[PATH_MAX] = {};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::string(buf, static_cast<size_t>(n));
#endif
}

// 可执行文件所在目录（末尾带分隔符；可能为空 = 取不到）
inline const std::string& ExeDir() {
    static const std::string dir = DirOf(ExePath());
    return dir;
}

inline bool FileExists(const std::string& p) {
    if (p.empty()) return false;
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

inline std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

// ============ 命令行 ============

struct LaunchArgs {
    std::string ui;                      // 显式 UI 路径（--ui <p> 或第一个非选项参数）
    bool watch = false;                  // --watch：保存即重载（UI 开发）
    bool help = false;                   // --help / -h
    std::vector<std::string> rest;       // 其它参数（各 demo 自用，如 videodemo 的 src 覆盖）
};

inline LaunchArgs ParseArgs(int argc, char* argv[]) {
    LaunchArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i] ? argv[i] : "";
        if (s.empty()) continue;
        if (s == "--watch" || s == "-w") {
            a.watch = true;
        } else if (s == "--help" || s == "-h") {
            a.help = true;
        } else if ((s == "--ui" || s == "-u") && i + 1 < argc) {
            ++i;
            a.ui = argv[i] ? argv[i] : "";
        } else if (a.ui.empty() && s[0] != '-') {
            a.ui = s;
        } else {
            a.rest.push_back(s);
        }
    }
    return a;
}

// ============ UI 文件定位 ============

// 查找顺序（docs/app_layout.md §3）：
//   1. --ui <path>
//   2. $BINUI_UI
//   3. <exeDir>/<appName>/<fileName>        子目录形态
//   4. <exeDir>/<fileName>                  平铺形态
//   5. <exeDir>/<appName>/<base>.binui      已编译产物（跳过编译）
//   6. <repo>/examples/<appName>/<fileName> 源码树内直接跑（未安装/未复制时）
// 返回真实路径；全部失败返回空（调用方回退 GenerateDefaultBinUI）。
inline std::string ResolveUI(const LaunchArgs& a, const std::string& appName,
                             const std::string& fileName) {
    auto hit = [](const std::string& p) { return FileExists(p) ? p : std::string(); };

    // 1. 显式指定：给了但不存在要吼一声，别静默回落
    if (!a.ui.empty()) {
        std::string p = hit(a.ui);
        if (!p.empty()) return p;
        std::fprintf(stderr, "[binui] --ui \"%s\" 不存在，回退默认查找\n", a.ui.c_str());
    }

    // 2. 环境变量
    if (const char* env = std::getenv("BINUI_UI")) {
        if (*env) {
            std::string p = hit(env);
            if (!p.empty()) return p;
        }
    }

    const std::string& exeDir = ExeDir();
    const std::string appDir = Join(exeDir, appName);

    // 3. 子目录形态
    if (std::string p = hit(Join(appDir, fileName)); !p.empty()) return p;

    // 4. 平铺形态
    if (std::string p = hit(Join(exeDir, fileName)); !p.empty()) return p;

    // 5. 已编译产物：同 base 名换 .binui 后缀
    {
        size_t dot = fileName.find_last_of('.');
        std::string base = dot == std::string::npos ? fileName : fileName.substr(0, dot);
        if (std::string p = hit(Join(appDir, base + ".binui")); !p.empty()) return p;
        if (std::string p = hit(Join(exeDir, base + ".binui")); !p.empty()) return p;
    }

    // 6. 源码树内直接跑（demo 从构建目录外启动时的兜底）
    return hit(Join("examples/" + appName, fileName));
}

// ============ 图片资源加载 ============

// 按编译器给出的 src 列表（CompileResult::images，逐字节作为键）加载图片字节。
// 查找顺序：UI 文件所在目录 -> exeDir -> exeDir/appName -> extraDirs（可选，源码树内跑时用）；
// 缺失只警告不中断。
inline std::unordered_map<std::string, std::vector<uint8_t>>
LoadImages(const std::string& uiPath, const std::vector<std::string>& images,
           const std::string& appName, const std::vector<std::string>& extraDirs = {}) {
    std::unordered_map<std::string, std::vector<uint8_t>> out;
    const std::string uiDir = DirOf(uiPath);
    const std::string& exeDir = ExeDir();
    const std::string appDir = Join(exeDir, appName);

    for (const auto& p : images) {
        std::vector<uint8_t> data = ReadFileBytes(Join(uiDir, p));
        if (data.empty()) data = ReadFileBytes(Join(exeDir, p));
        if (data.empty()) data = ReadFileBytes(Join(appDir, p));
        for (size_t i = 0; data.empty() && i < extraDirs.size(); ++i)
            data = ReadFileBytes(Join(extraDirs[i], p));
        if (data.empty()) {
            std::fprintf(stderr, "[binui] MISS image \"%s\"（查找 %s / %s）\n",
                         p.c_str(), uiDir.c_str(), appDir.c_str());
            continue;
        }
        out[p] = std::move(data);
    }
    return out;
}

} // namespace ex
} // namespace binui
