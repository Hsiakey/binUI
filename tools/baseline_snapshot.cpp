// baseline_snapshot - 迁移基线快照（三端像素哈希 + 原语流哈希 + 画质数字）
//
// 为什么要有这个工具：迁移 binUI 的路径光栅到 bin2d 之前，得先有一份"迁移前"的
// **可复现指纹**。像素回归（`make pixels` / `pixels-vk` / `pixels-d3d11`）只回答
// "在容差内吗"，回答不了"到底哪些用例动了、动了哪一端"。这个工具把每次渲染的结果
// 压成一行一个 64 位哈希，迁移每一步对一遍，就能把"我改的东西是不是只影响了它该影响
// 的地方"从感觉变成清单。
//
// 用法：
//   baseline_snapshot write <manifest.txt> <renderRoot> <draw_bench.txt> [quality.txt]
//   baseline_snapshot check <manifest.txt> <renderRoot> <draw_bench.txt> [quality.txt] [--strict]
//
//   renderRoot 下按后端分目录：gdi/ gl/ vk/ d3d11/，每个用例一张 <case>.png。
//   目录或某张图缺失不算错（GDI 侧本来就只有一部分用例有基准图）—— 只是不进快照。
//
// 退出码：
//   0 = 一致（或有哈希变化但没给 --strict：**哈希变化默认只报告**，因为迁移就是要改像素，
//       该不该改由人看清单决定；硬门禁仍然是 pixel_diff 的容差判据）
//   1 = 结构不一致（用例增删 / 尺寸变化），或 --strict 下有哈希变化
//   2 = 用法/IO 错误
//
// 哈希口径：width、height、逐行 ARGB 原始字节，FNV-1a 64。PNG 编码器换了不影响结果。
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

ULONG_PTR g_gdiplus = 0;

void GdiplusShutdown() {
    if (g_gdiplus) Gdiplus::GdiplusShutdown(g_gdiplus);
    g_gdiplus = 0;
}

bool GdiplusInit() {
    Gdiplus::GdiplusStartupInput in;
    return Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr) == Gdiplus::Ok;
}

bool LoadPixels(const char* path, int& w, int& h, std::vector<uint32_t>& px) {
    wchar_t wp[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024);
    Gdiplus::Bitmap bmp(wp, FALSE);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    w = static_cast<int>(bmp.GetWidth());
    h = static_cast<int>(bmp.GetHeight());
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    px.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    for (int y = 0; y < h; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(bd.Scan0) + static_cast<size_t>(y) * bd.Stride);
        std::memcpy(px.data() + static_cast<size_t>(y) * static_cast<size_t>(w), src,
                    sizeof(uint32_t) * static_cast<size_t>(w));
    }
    bmp.UnlockBits(&bd);
    return true;
}

uint64_t Fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string Rtrim(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) --n;
    return s.substr(0, n);
}

std::string ReadAll(const char* path, bool* ok) {
    std::string out;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        *ok = false;
        return out;
    }
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    *ok = true;
    return out;
}

// 一条快照记录：kind（px / draw / qa）+ 后端 + 用例 + 值
struct Entry {
    std::string kind;
    std::string backend;   // 只有 px 有
    std::string name;
    std::string value;     // "WxH <hash>" 或哈希 或 画质原文
    std::string key() const { return kind + "/" + backend + "/" + name; }
    std::string Line() const {
        if (kind == "px") return "px " + backend + " " + name + " " + value;
        return kind + " " + name + " " + value;
    }
};

std::vector<Entry> Collect(const std::string& root, const std::string& drawTxt,
                           const std::string& qualityTxt, int* error) {
    std::vector<Entry> out;
    static const char* kBackends[] = {"gdi", "gl", "vk", "d3d11"};
    for (const char* be : kBackends) {
        const std::string dir = root + "/" + be;
        WIN32_FIND_DATAA fd;
        const std::string pat = dir + "/*.png";
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::string> names;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn = fd.cFileName;
            if (fn.size() <= 4 || fn.substr(fn.size() - 4) != ".png") continue;
            names.push_back(fn.substr(0, fn.size() - 4));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end());
        for (const std::string& nm : names) {
            const std::string path = dir + "/" + nm + ".png";
            int w = 0, h2 = 0;
            std::vector<uint32_t> px;
            if (!LoadPixels(path.c_str(), w, h2, px)) {
                std::printf("读不了 %s\n", path.c_str());
                *error = 1;
                continue;
            }
            uint64_t hv = Fnv1a(&w, sizeof(w));
            hv = Fnv1a(&h2, sizeof(h2), hv);
            hv = Fnv1a(px.data(), px.size() * sizeof(uint32_t), hv);
            char val[64];
            std::snprintf(val, sizeof(val), "%dx%d %016llx", w, h2,
                          static_cast<unsigned long long>(hv));
            out.push_back(Entry{"px", be, nm, val});
        }
    }

    if (!drawTxt.empty()) {
        bool ok = false;
        const std::string txt = ReadAll(drawTxt.c_str(), &ok);
        if (!ok) {
            std::printf("读不了 %s（原语流哈希）\n", drawTxt.c_str());
            *error = 1;
        } else {
            size_t i = 0;
            while (i < txt.size()) {
                size_t e = txt.find('\n', i);
                if (e == std::string::npos) e = txt.size();
                const std::string line = Rtrim(txt.substr(i, e - i));
                i = e + 1;
                if (line.empty() || line[0] == '#') continue;
                // 期望格式：draw <name> <hash>
                if (line.rfind("draw ", 0) != 0) continue;
                const size_t sp = line.find(' ', 5);
                if (sp == std::string::npos) continue;
                out.push_back(Entry{"draw", "", line.substr(5, sp - 5), line.substr(sp + 1)});
            }
        }
    }

    if (!qualityTxt.empty()) {
        bool ok = false;
        const std::string txt = ReadAll(qualityTxt.c_str(), &ok);
        if (ok) {
            size_t i = 0;
            int idx = 0;
            while (i < txt.size()) {
                size_t e = txt.find('\n', i);
                if (e == std::string::npos) e = txt.size();
                std::string line = Rtrim(txt.substr(i, e - i));
                i = e + 1;
                if (line.empty()) continue;
                // 只收带方括号标签的行（工具输出里的数字行），其余是进度噪音
                if (line[0] != '[') continue;
                char nm[32];
                std::snprintf(nm, sizeof(nm), "%02d", idx++);
                out.push_back(Entry{"qa", "", nm, line});
            }
        } else {
            std::printf("读不了 %s（画质数字，可以不给）\n", qualityTxt.c_str());
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.key() < b.key(); });
    return out;
}

int WriteMode(const char* manifest, const std::string& root, const std::string& drawTxt,
              const std::string& qualityTxt) {
    int err = 0;
    const std::vector<Entry> entries = Collect(root, drawTxt, qualityTxt, &err);
    if (err) return 2;
    if (entries.empty()) {
        std::printf("快照是空的：%s 下没有可读的 PNG\n", root.c_str());
        return 2;
    }
    FILE* f = std::fopen(manifest, "wb");
    if (!f) {
        std::printf("写不了 %s\n", manifest);
        return 2;
    }
    std::fprintf(f, "# binUI 迁移基线快照 —— 由 `make baseline` 生成，别手改\n");
    std::fprintf(f, "# 校验：`make baseline-check`（报告）/ `make baseline-strict`（哈希必须完全相同）\n");
    std::fprintf(f, "# px <后端> <用例> <W>x<H> <FNV-1a 64 over 原始 ARGB>\n");
    std::fprintf(f, "# draw <用例> <原语流哈希>    qa <序号> <bin2d make quality 的原文>\n");
    int px = 0, dr = 0, qa = 0;
    for (const Entry& e : entries) {
        std::fprintf(f, "%s\n", e.Line().c_str());
        if (e.kind == "px") ++px;
        else if (e.kind == "draw") ++dr;
        else ++qa;
    }
    std::fclose(f);
    std::printf("基线写好了：%s（像素 %d 项 / 原语流 %d 项 / 画质 %d 项）\n", manifest, px, dr, qa);
    return 0;
}

int CheckMode(const char* manifest, const std::string& root, const std::string& drawTxt,
              const std::string& qualityTxt, bool strict) {
    bool ok = false;
    const std::string txt = ReadAll(manifest, &ok);
    if (!ok) {
        std::printf("读不了基线 %s（先跑一次 make baseline）\n", manifest);
        return 2;
    }
    std::vector<Entry> base;
    size_t i = 0;
    while (i < txt.size()) {
        size_t e = txt.find('\n', i);
        if (e == std::string::npos) e = txt.size();
        const std::string line = Rtrim(txt.substr(i, e - i));
        i = e + 1;
        if (line.empty() || line[0] == '#') continue;
        Entry en;
        size_t p = 0;
        auto tok = [&]() {
            const size_t s = line.find(' ', p);
            std::string t = (s == std::string::npos) ? line.substr(p) : line.substr(p, s - p);
            p = (s == std::string::npos) ? line.size() : s + 1;
            return t;
        };
        en.kind = tok();
        if (en.kind == "px") {
            en.backend = tok();
            en.name = tok();
        } else {
            en.name = tok();
        }
        en.value = line.substr(std::min(p, line.size()));
        base.push_back(en);
    }

    int err = 0;
    const std::vector<Entry> cur = Collect(root, drawTxt, qualityTxt, &err);
    if (err) return 2;

    // 按 key 索引两侧
    auto keyOf = [](const std::string& kind, const std::string& be, const std::string& nm) {
        return kind + "/" + be + "/" + nm;
    };
    std::vector<std::string> baseKeys;
    std::vector<std::string> curKeys;
    for (const Entry& e : base) baseKeys.push_back(keyOf(e.kind, e.backend, e.name));
    for (const Entry& e : cur) curKeys.push_back(keyOf(e.kind, e.backend, e.name));

    auto valueOf = [&keyOf](const std::vector<Entry>& v,
                            const std::string& key) -> const std::string* {
        for (const Entry& e : v) {
            if (keyOf(e.kind, e.backend, e.name) == key) return &e.value;
        }
        return nullptr;
    };

    int nMissing = 0, nExtra = 0, nSize = 0, nHash = 0, nQa = 0;
    std::vector<std::string> detail;

    for (const std::string& k : baseKeys) {
        const std::string* cv = valueOf(cur, k);
        if (!cv) {
            ++nMissing;
            if (detail.size() < 40) detail.push_back("  消失  " + k);
            continue;
        }
        const std::string* bv = valueOf(base, k);
        if (!bv || *bv == *cv) continue;
        const bool isPx = (k.rfind("px/", 0) == 0);
        if (isPx) {
            // 值形如 "420x140 <hash>"：尺寸变了就是结构变化
            const size_t sp1 = bv->find(' '), sp2 = cv->find(' ');
            const std::string bs = bv->substr(0, sp1), cs = cv->substr(0, sp2);
            if (bs != cs) {
                ++nSize;
                if (detail.size() < 40) detail.push_back("  尺寸  " + k + "：" + bs + " -> " + cs);
            } else {
                ++nHash;
                if (detail.size() < 40)
                    detail.push_back("  哈希  " + k + "：" + bv->substr(sp1 + 1) + " -> " +
                                     cv->substr(sp2 + 1));
            }
        } else if (k.rfind("qa/", 0) == 0) {
            ++nQa;
            if (detail.size() < 40)
                detail.push_back("  画质  " + k + "\n         基线: " + *bv + "\n         现在: " + *cv);
        } else {
            ++nHash;
            if (detail.size() < 40) detail.push_back("  原语  " + k + "：" + *bv + " -> " + *cv);
        }
    }
    for (const std::string& k : curKeys) {
        bool found = false;
        for (const std::string& bk : baseKeys) {
            if (bk == k) { found = true; break; }
        }
        if (!found) {
            ++nExtra;
            if (detail.size() < 40) detail.push_back("  新增  " + k);
        }
    }

    std::printf("比较基线 %s：%zu 项\n", manifest, base.size());
    if (detail.empty()) {
        std::printf("  完全一致（像素 / 原语流 / 画质数字全部与基线相同）\n");
    } else {
        for (const std::string& d : detail) std::printf("%s\n", d.c_str());
        if (static_cast<int>(detail.size()) >= 40)
            std::printf("  ...（只列前 40 条，完整差异见上面的计数）\n");
    }
    std::printf("统计：消失 %d / 新增 %d / 尺寸变 %d / 哈希变 %d / 画质变 %d\n", nMissing, nExtra,
                nSize, nHash, nQa);

    if (nMissing || nExtra || nSize) {
        std::printf("FAIL：用例集合或尺寸变了 —— 这不是渲染细节，是结构变化\n");
        return 1;
    }
    if (strict && (nHash || nQa)) {
        std::printf("FAIL（--strict）：哈希/画质有变化，而这个阶段要求逐位相同\n");
        return 1;
    }
    if (nHash || nQa) {
        std::printf("注意：有变化但没给 --strict —— 迁移阶段该变的像素要能逐条解释；\n"
                    "      确认后跑 `make baseline` 重生成快照，并在 CHANGELOG 里写明为什么变。\n");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: baseline_snapshot write|check <manifest.txt> <renderRoot> "
                    "<draw_bench.txt> [quality.txt] [--strict]\n");
        return 2;
    }
    const std::string mode = argv[1];
    bool strict = false;
    std::string qualityPath;
    for (int i = 5; i < argc; i++) {
        if (std::strcmp(argv[i], "--strict") == 0) strict = true;
        else if (argv[i][0] != '-') qualityPath = argv[i];
    }
    if (!GdiplusInit()) {
        std::printf("GDI+ 初始化失败\n");
        return 2;
    }
    int rc = 2;
    if (mode == "write")
        rc = WriteMode(argv[2], argv[3], argv[4], qualityPath);
    else if (mode == "check")
        rc = CheckMode(argv[2], argv[3], argv[4], qualityPath, strict);
    else
        std::printf("未知模式 %s\n", mode.c_str());
    GdiplusShutdown();
    return rc;
}
