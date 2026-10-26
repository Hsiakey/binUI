// multiwindow/main.cpp - binUI 独立窗口模式（多进程 · 多窗口）
// 同一个可执行文件既当启动器又当窗口进程：
//   无参数          -> 启动器：为每个窗口各起一个子进程（CREATE_NO_WINDOW），
//                      再按屏幕位置摆好，最后把立绘窗口置顶，随即退出；
//   --win=stage|hero|dialog -> 工作进程：只编译并运行自己那一块 UI 的窗口。
// 三个窗口都是独立顶层窗口（各自的进程、各自的消息循环、各自关闭），
// 立绘窗口为分层透明窗口（<app_window transparent> -> UpdateLayeredWindow）。
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "backends/binui_gl-gdi.h"
#include "binui_paths.h"
#include "binui_hot_reload.h"
#include <windows.h>

using namespace binui;

namespace {

struct WinSpec {
    const char* role;
    const char* html;
    const char* title;   // 唯一 ASCII 标题（启动器用 FindWindowA 定位）
    int   w, h, x, y;    // 尺寸 + 相对工作区的摆放位置
    bool  transparent;
};

// 三个窗口的规格（同时决定工作进程要用哪份 HTML）
const WinSpec kWins[] = {
    { "stage",  "stage.html",  "BinUI MW Stage",  1000, 680,  60,  40, false },
    { "dialog", "dialog.html", "BinUI MW Dialog",  920, 210, 100, 490, false },
    { "hero",   "hero.html",   "BinUI MW Hero",     560, 600, 280,  10, true  },
};
const int kWinsN = static_cast<int>(sizeof(kWins) / sizeof(kWins[0]));

// 工作进程：编译本窗口 HTML、加载图片、跑一个 BinUIAppGLGdi 窗口
int RunWorker(const WinSpec& spec, bool watch) {
    binui::ex::LaunchArgs args;
    args.watch = watch;
    const std::string uiPath = binui::ex::ResolveUI(args, "multiwindow", spec.html);

    BinUIRenderer renderer;
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        std::fprintf(stderr, "[multiwindow:%s] 未找到 %s，使用默认布局\n", spec.role, spec.html);
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[multiwindow:%s] Failed to load layout\n", spec.role);
        return 1;
    }

    renderer.setImageOrder(res.images);
    renderer.loadResources(binui::ex::LoadImages(uiPath, res.images, "multiwindow", {"tests/"}));

    const auto& wd = renderer.windowDesc();
    int w = (wd.present && wd.width)  ? (int)wd.width  : spec.w;
    int h = (wd.present && wd.height) ? (int)wd.height : spec.h;

    BinUIAppGLGdi app(w, h);
    app.setWindowOptions(false, 0, wd.present && wd.transparent(), spec.title);

    uint16_t btnMin   = renderer.findNodeByName("btnMin");
    uint16_t btnClose = renderer.findNodeByName("btnClose");


        auto invalidateNode = [&](uint16_t id) {
        // 失效矩形必须含动画后的包围盒：hover 放大时只按布局矩形失效，
        // 长出来的那一圈（边框/底色）不会被重绘，看上去就是"边框被削掉一半"
        float bx, by, bw, bh;
        if (renderer.invalidRect(id, bx, by, bw, bh))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    };

    app.setDrawFn([&](BinUIGLGdi& gfx, const void* clip) { drawNodesGLGdi(gfx, renderer, clip); });
    app.setMoveFn([&](float x, float y) {
        int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        int cur = renderer.hoveredNode();
        if (old != cur) {
            if (old >= 0) invalidateNode((uint16_t)old);
            if (cur >= 0) invalidateNode((uint16_t)cur);
        }
    });
    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.press(x, y);
        int n = renderer.hitTest(x, y);
        if (n >= 0) invalidateNode((uint16_t)n);
    });
    app.setReleaseFn([&](float x, float y) {
        int p = renderer.pressedNode();
        int id = renderer.release(x, y);
        if (p >= 0) invalidateNode((uint16_t)p);
        if (id < 0) return;
        uint16_t nid = (uint16_t)id;
        if (nid == btnMin)        { app.minimize();    return; }
        if (nid == btnClose)      { app.requestExit(); return; }
        invalidateNode(nid);
    });
    app.setLeaveFn([&]() {
        int hcur = renderer.hoveredNode();
        renderer.clearHover();
        if (hcur >= 0) invalidateNode((uint16_t)hcur);
    });
    app.setKeyFn([&](int vk) { if (vk == VK_ESCAPE) app.requestExit(); });
    app.setFocusFn([&](bool focused) {
        if (!focused) { renderer.cancelPress(); renderer.clearHover(); renderer.blur(); app.invalidateAll(); }
    });
    binui::ex::HotReload hot;
    hot.Init(args.watch, uiPath);
    int winW = w, winH = h;

    app.setUpdateFn([&](float) {
        if (hot.Tick(renderer, "multiwindow", winW, winH) == binui::ex::HotReload::Result::Reloaded) {
            btnMin   = renderer.findNodeByName("btnMin");
            btnClose = renderer.findNodeByName("btnClose");
            app.invalidateAll();
        }
        for (uint16_t id : renderer.stepAnimations()) invalidateNode(id);
    });
    app.setResizeFn([&](int nw, int nh) { winW = nw; winH = nh; renderer.setSize(nw, nh); });

    std::printf("[multiwindow:%s] window running (%dx%d)\n", spec.role, w, h);
    return app.run();
}

// 启动器：拉起三个工作进程，摆好窗口位置，退出
int LaunchAll(bool watch) {
    FreeConsole(); // 隐藏启动器控制台（子进程用 CREATE_NO_WINDOW，无额外控制台）

    const std::string exe = binui::ex::ExePath();
    const std::string dir = binui::ex::ExeDir();

    for (int i = 0; i < kWinsN; i++) {
        std::string cmd = "\"" + exe + "\" --win=" + kWins[i].role;
        if (watch) cmd += " --watch";
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            std::fprintf(stderr, "[multiwindow] spawn %s failed (%lu)\n",
                         kWins[i].role, GetLastError());
        }
    }

    RECT work = { 0, 0, 1280, 800 };
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);

    for (int i = 0; i < kWinsN; i++) {
        HWND hwnd = nullptr;
        for (int tries = 0; tries < 60 && !hwnd; tries++) {
            hwnd = FindWindowA(nullptr, kWins[i].title);
            if (!hwnd) Sleep(100);
        }
        if (!hwnd) continue;
        SetWindowPos(hwnd, HWND_TOP,
                     work.left + kWins[i].x, work.top + kWins[i].y,
                     kWins[i].w, kWins[i].h, SWP_SHOWWINDOW);
    }

    // 立绘窗口置顶，保证它浮在舞台之上
    if (HWND hero = FindWindowA(nullptr, "BinUI MW Hero"))
        SetWindowPos(hero, HWND_TOPMOST,
                     work.left + 280, work.top + 10, 560, 600, SWP_SHOWWINDOW);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) {
        std::printf("用法: multiwindow [--watch]\n"
                    "  启动器：为 stage/hero/dialog 各起一个子进程（各自独立窗口）\n"
                    "  --watch  保存 UI 后自动重编译热重载（会透传给三个工作进程）\n"
                    "内部参数: --win=stage|hero|dialog（由启动器拉起，一般不用手写）\n");
        return 0;
    }

    // 工作进程：--win=stage|hero|dialog（由启动器拉起，--watch 一并透传）
    const std::string winArg = "--win=";
    std::string role;
    for (const auto& a : args.rest) {
        if (a.rfind(winArg, 0) == 0) { role = a.substr(winArg.size()); break; }
    }
    if (!role.empty()) {
        for (int i = 0; i < kWinsN; i++)
            if (role == kWins[i].role) return RunWorker(kWins[i], args.watch);
        std::fprintf(stderr, "[multiwindow] unknown role '%s'\n", role.c_str());
        return 1;
    }
    return LaunchAll(args.watch);
}
