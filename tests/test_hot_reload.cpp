// test_hot_reload.cpp - UI 路径解析 + 热重载回归测试（无窗口，纯逻辑）
//
// 覆盖：
//   1. exe 真实路径解析（不再依赖 argv[0]）
//   2. ResolveUI 查找顺序（--ui 优先 / 源码树兜底）
//   3. 保存即重载：mtime 变化 -> 稳定 250ms 后重新编译 -> 换树成功
//   4. 换树后状态恢复（滚动偏移 / 键盘焦点）
//   5. 坏 UI / 文件消失时不把界面刷空、不误触发
//
// 为避免污染仓库，所有写操作发生在临时副本上（build-mingw/tmp/）。
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "binui_compiler.h"
#include "binui_hot_reload.h"
#include "binui_paths.h"
#include "binui_renderer.h"

using namespace binui;

static int g_fail = 0;

#define CHECK(cond, msg)                                \
    do {                                                \
        if (!(cond)) {                                  \
            std::fprintf(stderr, "[FAIL] %s\n", (msg)); \
            ++g_fail;                                   \
        } else {                                        \
            std::printf("[ok] %s\n", (msg));            \
        }                                               \
    } while (0)

static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool WriteAll(const std::string& path, const std::string& body) {
    SleepMs(20); // 让 mtime 至少跨过一个时钟粒度
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
    return o.good();
}

// 反复 Tick 直到重载或超时
static bool PumpUntilReload(ex::HotReload& hot, BinUIRenderer& r, int maxMs) {
    for (int waited = 0; waited < maxMs; waited += 30) {
        if (hot.Tick(r, "svgdemo", 0, 0) == ex::HotReload::Result::Reloaded) return true;
        SleepMs(30);
    }
    return false;
}

int main() {
    std::error_code ec;
    std::filesystem::create_directories("build-mingw/tmp", ec);

    // ---------- 1. exe 真实路径（本次修复的核心） ----------
    const std::string exePath = ex::ExePath();
    std::printf("[info] ExePath() = \"%s\"\n", exePath.c_str());
    CHECK(!exePath.empty(), "ExePath() 非空（不依赖 argv[0]）");
    CHECK(ex::FileExists(exePath), "ExePath() 指向真实存在的文件");
    CHECK(!ex::ExeDir().empty(), "ExeDir() 非空");

    // ---------- 2. UI 定位顺序 ----------
    ex::LaunchArgs none;
    const std::string repoUI = ex::ResolveUI(none, "svgdemo", "main.html");
    std::printf("[info] ResolveUI(svgdemo/main.html) = \"%s\"\n", repoUI.c_str());
    CHECK(ex::FileExists(repoUI), "ResolveUI 返回的 UI 文件真实存在");
    CHECK(repoUI.find("svgdemo") != std::string::npos &&
              repoUI.find("main.html") != std::string::npos,
          "ResolveUI 命中 svgdemo 的 main.html（exe 同级或源码树）");

    // 只在源码树里存在的文件 -> 必须走第 6 级兜底（证明编译目录外也能跑）
    // 用 main_video_backup.html：它不会被 copy 到 build-mingw/bin/，只能从源码树找到
    const std::string repoOnly = ex::ResolveUI(none, "videodemo", "main_video_backup.html");
    std::printf("[info] ResolveUI(videodemo/main_video_backup.html) = \"%s\"\n", repoOnly.c_str());
    CHECK(repoOnly == "examples/videodemo/main_video_backup.html", "源码树兜底命中 examples/<app>/<file>");

    CHECK(ex::ResolveUI(none, "svgdemo", "no_such_file.html").empty(),
          "文件不存在时返回空（调用方回退默认布局）");

    ex::LaunchArgs withUi;
    withUi.ui = "examples/animshow/main.html";
    CHECK(ex::ResolveUI(withUi, "svgdemo", "main.html") == "examples/animshow/main.html",
          "--ui 显式指定优先于默认查找");

    // ---------- 3. 首次编译 + 加载 ----------
    const CompileResult initial = CompileBinUIFileEx(repoUI);
    CHECK(!initial.layout.empty(), "示例 HTML 编译成功");
    BinUIRenderer renderer;
    CHECK(renderer.loadLayout(initial.layout.data(), initial.layout.size()), "布局解析成功");
    const size_t nodes0 = renderer.getNodes().size();
    CHECK(nodes0 > 0, "节点树非空");

    // ---------- 4. 临时副本上验证"保存即重载" ----------
    const std::string tmpPath = "build-mingw/tmp/hot_ui.html";
    const std::string html = ReadAll(repoUI);
    CHECK(!html.empty(), "读到示例 HTML");
    CHECK(WriteAll(tmpPath, html), "临时副本写入成功");

    ex::HotReload hot;
    hot.Init(true, tmpPath);
    CHECK(hot.enabled(), "--watch 启用");

    int early = 0;
    for (int i = 0; i < 20; ++i) {
        if (hot.Tick(renderer, "svgdemo", 0, 0) == ex::HotReload::Result::Reloaded) ++early;
        SleepMs(30);
    }
    CHECK(early == 0, "文件未改动时不触发重载");

    CHECK(WriteAll(tmpPath, html), "模拟保存（重写同一份内容）");
    CHECK(PumpUntilReload(hot, renderer, 3000), "保存后自动重载");
    CHECK(hot.reloads() == 1, "重载计数 == 1");
    CHECK(renderer.getNodes().size() == nodes0, "重载后节点数一致");

    // ---------- 5. 状态恢复：滚动偏移 / 键盘焦点 ----------
    uint16_t scrollNode = 0, focusNode = 0;
    for (const auto& n : renderer.getNodes()) {
        if (!scrollNode && renderer.isScrollable(n.id)) scrollNode = n.id;
        if (!focusNode && (n.flags & BINUI_INTERACTIVE)) focusNode = (uint16_t)n.id;
    }

    float expectedScrollY = 0.0f;
    int expectedFocus = -1;
    if (scrollNode) {
        renderer.scrollTo(scrollNode, 0.0f, 17.0f);
        const auto* st = renderer.scrollState(scrollNode);
        expectedScrollY = st ? st->y : 0.0f;
        std::printf("[info] 滚动容器 %u：requested=17.0 clamped=%.1f\n", scrollNode, expectedScrollY);
    }
    if (focusNode) {
        renderer.setKeyboardFocus(focusNode);
        expectedFocus = renderer.keyboardNode();
        std::printf("[info] 键盘焦点节点 %u -> %d\n", focusNode, expectedFocus);
    }

    CHECK(WriteAll(tmpPath, html), "再次模拟保存");
    CHECK(PumpUntilReload(hot, renderer, 3000), "第二次重载成功");

    if (scrollNode) {
        const auto* st = renderer.scrollState(scrollNode);
        char msg[192];
        std::snprintf(msg, sizeof(msg), "滚动偏移恢复（节点 %u：期望 %.1f，实际 %.1f）",
                      scrollNode, expectedScrollY, st ? st->y : -1.0f);
        CHECK(st && st->y == expectedScrollY, msg);
    } else {
        std::printf("[skip] 示例 UI 无滚动容器，跳过滚动恢复\n");
    }

    if (focusNode) {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "键盘焦点恢复（期望 %d，实际 %d）",
                      expectedFocus, renderer.keyboardNode());
        CHECK(renderer.keyboardNode() == expectedFocus, msg);
    } else {
        std::printf("[skip] 示例 UI 无可交互节点，跳过焦点恢复\n");
    }

    // ---------- 5b. 滚动恢复：用合成 UI 保证一定溢出 ----------
    {
        std::string html2 =
            "<html><body><div id=\"sc\" style=\"position:absolute;left:0;top:0;"
            "width:300;height:120;overflow:auto\">";
        for (int i = 0; i < 40; ++i)
            html2 += "<p style=\"height:24\">row " + std::to_string(i) + "</p>";
        html2 += "</div></body></html>";

        const std::string scrollPath = "build-mingw/tmp/hot_scroll.html";
        CHECK(WriteAll(scrollPath, html2), "合成滚动 UI 写入成功");

        BinUIRenderer r2;
        const CompileResult cr = CompileBinUIFileEx(scrollPath);
        CHECK(!cr.layout.empty() && r2.loadLayout(cr.layout.data(), cr.layout.size()),
              "合成滚动 UI 编译并加载");
        const uint16_t sc = r2.findNodeByName("sc");
        CHECK(sc != 0 && r2.isScrollable(sc), "找到滚动容器 #sc");
        const float contentH = sc ? r2.contentHeight(sc) : 0.0f;
        std::printf("[info] #sc contentHeight=%.1f\n", contentH);
        CHECK(contentH > 120.0f, "合成 UI 内容确实溢出（可滚动）");

        r2.scrollTo(sc, 0.0f, 300.0f);
        const auto* st0 = r2.scrollState(sc);
        const float wantY = st0 ? st0->y : -1.0f;
        std::printf("[info] #sc requested=300.0 clamped=%.1f\n", wantY);
        CHECK(wantY > 0.0f, "滚动偏移已生效（未被钳制为 0）");

        ex::HotReload hot2;
        hot2.Init(true, scrollPath);
        CHECK(WriteAll(scrollPath, html2), "模拟保存滚动 UI");
        CHECK(PumpUntilReload(hot2, r2, 3000), "滚动 UI 重载成功");
        const auto* st1 = r2.scrollState(sc);
        char msg[192];
        std::snprintf(msg, sizeof(msg), "滚动偏移跨重载保留（期望 %.1f，实际 %.1f）",
                      wantY, st1 ? st1->y : -1.0f);
        CHECK(st1 && st1->y == wantY, msg);
    }

    // ---------- 6. 坏 UI：不允许把界面刷空 ----------
    CHECK(WriteAll(tmpPath, "<div><<<< 垃圾 & 未闭合 <span>"), "写入一份坏 UI");
    bool reloaded = false;
    for (int i = 0; i < 60 && !reloaded; ++i) {
        const auto res = hot.Tick(renderer, "svgdemo", 0, 0);
        if (res != ex::HotReload::Result::Idle) reloaded = true;
        SleepMs(30);
    }
    std::printf("[info] 坏 UI 处理：%s；当前节点数 %zu\n",
                reloaded ? "已处理（编译容错则重载，失败则保留旧树）" : "未触发",
                renderer.getNodes().size());
    CHECK(renderer.getNodes().size() > 0, "坏 UI 后节点树没有变空（仍可渲染）");

    // ---------- 7. 文件消失：不误触发 ----------
    std::remove(tmpPath.c_str());
    ex::HotReload gone;
    gone.Init(true, "build-mingw/tmp/does_not_exist.html");
    int bogus = 0;
    for (int i = 0; i < 20; ++i) {
        if (gone.Tick(renderer, "svgdemo", 0, 0) != ex::HotReload::Result::Idle) ++bogus;
        SleepMs(10);
    }
    CHECK(bogus == 0, "文件不存在时 20 次 Tick 全 Idle");

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
