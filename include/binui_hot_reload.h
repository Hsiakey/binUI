// binui_hot_reload.h - UI 热重载（--watch，header-only）
//
// UI 开发阶段的价值：改一行 HTML/XML 保存 -> 自动重新编译 -> 换节点树 -> 保留
// 窗口尺寸/滚动位置/输入框内容/键盘焦点 -> 立即看到效果，不用重启进程。
//
// 实现要点：
//   - 轮询 mtime+size（跨平台，无 inotify/ReadDirectoryChangesW 平台分支），
//     采样节流 100ms，文件稳定 250ms 才动（避免编辑器写一半时读到半截文件）。
//   - 编译失败 **保留旧 UI**，只在 stderr 报错——热重载不能把界面刷成黑的。
//   - 动态节点（图表 addNode 之类）在换树后消失，这是预期行为。
//   - 状态按 **节点 id** 恢复：id 由编译器按 DOM 顺序分配，改动不影响编号时状态不丢。
//
// 用法：
//   binui::ex::HotReload hot;
//   hot.Init(args.watch, uiPath);
//   app.setUpdateFn([&](float dt) {
//       if (hot.Tick(renderer, "svgdemo", winW, winH) == HotReload::Result::Reloaded) {
//           /* 重新取本 demo 缓存的节点几何 */ app.invalidateAll();
//       }
//       ...
//   });
#pragma once

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/stat.h>
#endif

namespace binui {
namespace ex {

struct FileStamp {
    bool exists = false;
    long long mtime = 0;               // Windows: 100ns 单位；POSIX: 秒
    unsigned long long size = 0;

    bool operator==(const FileStamp& o) const {
        return exists == o.exists && mtime == o.mtime && size == o.size;
    }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

inline FileStamp StatFile(const std::string& path) {
    FileStamp st;
    if (path.empty()) return st;
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return st;
    st.exists = true;
    ULARGE_INTEGER w;
    w.LowPart = d.ftLastWriteTime.dwLowDateTime;
    w.HighPart = d.ftLastWriteTime.dwHighDateTime;
    st.mtime = static_cast<long long>(w.QuadPart);
    st.size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
#else
    struct stat sb = {};
    if (::stat(path.c_str(), &sb) != 0) return st;
    st.exists = true;
    st.mtime = static_cast<long long>(sb.st_mtime);
    st.size = static_cast<unsigned long long>(sb.st_size);
#endif
    return st;
}

// 节点是否仍存在（换树后按 id 判断状态能不能恢复）
inline const BinUIRenderer::NodeInfo* FindNode(const BinUIRenderer& r, uint16_t id) {
    for (const auto& n : r.getNodes()) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

class HotReload {
public:
    enum class Result { Idle, Reloaded, Failed };

    void Init(bool enabled, const std::string& uiPath) {
        enabled_ = enabled && !uiPath.empty();
        path_ = uiPath;
        last_ = StatFile(path_);
        lastSample_ = NowSec();
        if (enabled_) {
            std::printf("[binui] --watch: 监听 %s（保存即重编译；编译失败保留旧 UI）\n",
                        path_.c_str());
            std::fflush(stdout);
        }
    }

    bool enabled() const { return enabled_; }
    int reloads() const { return reloads_; }

    // 每帧调用。winW/winH = 当前窗口尺寸（<=0 表示不重设布局尺寸）。
    Result Tick(BinUIRenderer& r, const std::string& appName, int winW = 0, int winH = 0) {
        if (!enabled_) return Result::Idle;

        const double now = NowSec();
        if (now - lastSample_ < 0.10) return Result::Idle;   // 采样节流
        lastSample_ = now;

        const FileStamp st = StatFile(path_);
        if (st == last_) {                                   // 与上次生效版本一致
            pending_ = false;
            return Result::Idle;
        }
        if (!pending_ || st != pendingStamp_) {              // 首次发现变化 / 又变了：重新计时
            pending_ = true;
            pendingStamp_ = st;
            pendingAt_ = now;
            return Result::Idle;
        }
        if (now - pendingAt_ < 0.25) return Result::Idle;    // 文件稳定 250ms 才动

        pending_ = false;
        last_ = st;
        return Reload(r, appName, winW, winH) ? Result::Reloaded : Result::Failed;
    }

private:
    static double NowSec() {
        using namespace std::chrono;
        return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
    }

    bool Reload(BinUIRenderer& r, const std::string& appName, int winW, int winH) {
        // ---- 1. 抓状态（换树前） ----
        const int kbFocus = r.keyboardNode();
        const EditState editSnap = r.edit();          // active / nodeId / value / caret / selStart
        std::vector<std::pair<uint16_t, BinUIRenderer::ScrollState>> scrolls;
        for (const auto& n : r.getNodes()) {
            if (const BinUIRenderer::ScrollState* s = r.scrollState(n.id))
                scrolls.emplace_back(n.id, *s);
        }

        // ---- 2. 重新编译（失败不动当前 UI） ----
        const CompileResult res = CompileBinUIFileEx(path_);
        if (res.layout.empty()) {
            std::fprintf(stderr, "[binui] --watch: 编译失败，保留当前 UI（%s）\n", path_.c_str());
            return false;
        }

        // ---- 3. 换树 + 重载资源 ----
        if (!r.loadLayout(res.layout.data(), res.layout.size())) {
            std::fprintf(stderr, "[binui] --watch: 布局解析失败，保留当前 UI\n");
            return false;
        }
        r.setImageOrder(res.images);
        r.loadResources(LoadImages(path_, res.images, appName));
        if (winW > 0 && winH > 0) r.setSize(winW, winH);

        // ---- 4. 恢复状态 ----
        for (const auto& kv : scrolls) {
            if (FindNode(r, kv.first)) r.scrollTo(kv.first, kv.second.x, kv.second.y);
        }
        if (kbFocus >= 0 && FindNode(r, static_cast<uint16_t>(kbFocus))) {
            r.setKeyboardFocus(kbFocus);
        }
        if (editSnap.active && !editSnap.value.empty()) {
            const BinUIRenderer::NodeInfo* n = FindNode(r, editSnap.nodeId);
            if (n && n->type == BINUI_INPUT) {
                if (n->text != editSnap.value) r.setText(editSnap.nodeId, editSnap.value);
                r.focus(editSnap.nodeId);
                if (editSnap.caret > 0) r.setCaret(editSnap.caret);
            }
        }

        ++reloads_;
        std::printf("[binui] --watch: 已重载 #%d（%zu 节点，%zu 图片）\n",
                    reloads_, r.getNodes().size(), res.images.size());
        std::fflush(stdout);
        return true;
    }

    bool enabled_ = false;
    std::string path_;
    FileStamp last_;
    FileStamp pendingStamp_;
    bool pending_ = false;
    double lastSample_ = 0.0;
    double pendingAt_ = 0.0;
    int reloads_ = 0;
};

} // namespace ex
} // namespace binui
