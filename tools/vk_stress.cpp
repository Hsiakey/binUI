// vk_stress.cpp - Vulkan 后端鲁棒性压力测试（P4）
//
// 用法: vk_stress <ui file> [--rounds N] [--out shot.png]
//
// 一轮 = 若干段"真实窗口会遇到的坏事"，每段之间都要求：不崩、不卡死、最终像素正确。
//   1. resize 抖动：每帧换一次尺寸（模拟拖边框），尺寸里含奇数、极小、极大值
//   2. 同一尺寸反复 set：验证 resize 早退不会踩空
//   3. 最小化/恢复：客户区 0×0 → 还原（交换链不能因此坏死）
//   4. 透明窗来回切（走读回路径 → 再切回交换链）
//   5. 强制 GDI 呈现来回切（拖拽 resize 期的走法）
//   6. 快速 init/shutdown 循环（资源释放是否干净：看进程私有内存不再涨）
//   7. 双实例并存（两个 BinUIVK 各自持有实例/设备/交换链）
// 最后把画面读回写 PNG，交给 pixel_diff 与 gl_shot 比（默认 `make vk-stress` 里做）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"
#include "backends/binui_draw_nodes.h"
#include "backends/binui_vk.h"
#include "save_png.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail++;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, msg, wp, lp);
}

void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) return;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.PagefileUsage;   // 提交量：GPU 之外最直观的泄漏晴雨表
}

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

struct Scene {
    binui::BinUIRenderer renderer;
    bool ok = false;
};

bool LoadScene(Scene& s, const char* uiPath) {
    binui::CompileResult res = binui::CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) return false;
    if (!s.renderer.loadLayout(res.layout.data(), res.layout.size())) return false;
    const auto& wd = s.renderer.windowDesc();
    const auto& nodes = s.renderer.getNodes();
    int W = wd.present && wd.width ? static_cast<int>(wd.width)
                                   : (nodes.empty() ? 900 : static_cast<int>(nodes[0].width));
    int H = wd.present && wd.height ? static_cast<int>(wd.height)
                                    : (nodes.empty() ? 640 : static_cast<int>(nodes[0].height));
    s.renderer.setSize(W, H);
    const auto& paths = s.renderer.imagePaths();
    if (!paths.empty()) {
        auto imgs = binui::ex::LoadImages(binui::ex::DirOf(uiPath), paths, std::string());
        if (!imgs.empty()) s.renderer.loadResources(imgs);
    }
    s.ok = true;
    return true;
}

// 画一帧（不 present）
void DrawFrame(binui::BinUIVK& gfx, binui::BinUIRenderer& r) {
    gfx.beginFrame(nullptr);
    binui::drawNodesBinUI(gfx, r, nullptr, binui::CanvasFn{});
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: vk_stress <ui file> [--rounds N] [--out shot.png]\n");
        return 1;
    }
    const char* uiPath = argv[1];
    int rounds = 3;
    const char* outPath = nullptr;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
    }

    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    const bool gdip = Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr) == Gdiplus::Ok;

    Scene scene;
    if (!LoadScene(scene, uiPath)) {
        std::fprintf(stderr, "[vk_stress] 加载 UI 失败: %s\n", uiPath);
        return 1;
    }
    binui::BinUIRenderer& renderer = scene.renderer;
    const auto& wd = renderer.windowDesc();
    const auto& nodes = renderer.getNodes();
    const int baseW = wd.present && wd.width ? static_cast<int>(wd.width)
                                             : (nodes.empty() ? 900 : static_cast<int>(nodes[0].width));
    const int baseH = wd.present && wd.height ? static_cast<int>(wd.height)
                                              : (nodes.empty() ? 640 : static_cast<int>(nodes[0].height));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"binui_vk_stress";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"vk_stress", WS_POPUP | WS_VISIBLE, 30, 30,
                               baseW, baseH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { std::fprintf(stderr, "[vk_stress] 建窗失败\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    const size_t mem0 = PrivateBytes();
    const double t0 = NowMs();
    int frames = 0;

    binui::BinUIVK gfx;
    if (!gfx.init(hwnd, baseW, baseH, false)) {
        std::fprintf(stderr, "[vk_stress] 初始化失败\n");
        return 1;
    }
    gfx.setCaptureReadback(true);

    // ---- 1. resize 抖动：尺寸含奇数、极小、极大 ----
    std::printf("[1] resize 抖动\n");
    {
        const int dw[] = {0, -1, -3, 1, 7, 33, 120, -17, 0};
        int w = baseW, h = baseH;
        for (int i = 0; i < 60 * rounds; i++) {
            Pump();
            w = std::max(16, baseW + dw[i % 9] * 3);
            h = std::max(16, baseH - dw[(i / 3) % 9] * 2);
            SetWindowPos(hwnd, nullptr, 30, 30, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            renderer.setSize(w, h);
            gfx.resize(w, h);
            DrawFrame(gfx, renderer);
            gfx.present();
            frames++;
        }
        Check(gfx.isReady(), "resize 抖动后后端仍就绪");
        Check(static_cast<int>(gfx.width()) == w && static_cast<int>(gfx.height()) == h,
              "resize 后记录尺寸与请求一致");
    }

    // ---- 2. 同尺寸反复 set（必须早退，不能重建）----
    std::printf("[2] 同尺寸反复 resize（提交 %.1f MB，后端显存 %.1f MB）\n", PrivateBytes() / 1048576.0, gfx.deviceMemoryBytes() / 1048576.0);
    {
        const int w = gfx.width(), h = gfx.height();
        for (int i = 0; i < 50 * rounds; i++) { gfx.resize(w, h); frames++; }
        Check(gfx.isReady(), "同尺寸 resize 不破坏后端");
    }

    // ---- 3. 最小化 / 恢复 ----
    std::printf("[3] 最小化 → 恢复（提交 %.1f MB，后端显存 %.1f MB）\n", PrivateBytes() / 1048576.0, gfx.deviceMemoryBytes() / 1048576.0);
    {
        ShowWindow(hwnd, SW_MINIMIZE);
        Pump();
        for (int i = 0; i < 10; i++) {   // 最小化期间应用仍会收到 0 尺寸
            gfx.resize(0, 0);            // 必须被忽略
            DrawFrame(gfx, renderer);
            gfx.present();
            frames++;
        }
        Check(gfx.isReady(), "最小化期间 0×0 resize 被忽略");
        ShowWindow(hwnd, SW_RESTORE);
        Pump();
        SetWindowPos(hwnd, nullptr, 30, 30, baseW, baseH, SWP_NOZORDER | SWP_NOACTIVATE);
        renderer.setSize(baseW, baseH);
        gfx.resize(baseW, baseH);
        DrawFrame(gfx, renderer);
        gfx.present();
        frames++;
        Check(gfx.isReady(), "恢复后仍在绘制");
    }

    // ---- 4. 透明窗来回切 ----
    std::printf("[4] 透明窗切换（提交 %.1f MB，后端显存 %.1f MB）\n", PrivateBytes() / 1048576.0, gfx.deviceMemoryBytes() / 1048576.0);
    {
        for (int i = 0; i < 20; i++) {
            Pump();
            gfx.setTransparent((i % 2) == 0);
            DrawFrame(gfx, renderer);
            gfx.present();
            frames++;
        }
        gfx.setTransparent(false);
        Check(gfx.isReady(), "透明/非透明来回切后仍就绪");
    }

    // ---- 5. 强制 GDI 呈现来回切 ----
    std::printf("[5] GDI 呈现切换（提交 %.1f MB，后端显存 %.1f MB）\n", PrivateBytes() / 1048576.0, gfx.deviceMemoryBytes() / 1048576.0);
    {
        for (int i = 0; i < 20; i++) {
            Pump();
            DrawFrame(gfx, renderer);
            if (i % 2) gfx.presentViaGdi();
            else gfx.present();
            frames++;
        }
        Check(gfx.isReady(), "GDI/交换链来回切后仍就绪");
    }

    // ---- 6. 快速 init/shutdown 循环（资源释放）----
    std::printf("[6] init/shutdown 循环 ×20（提交 %.1f MB，后端显存 %.1f MB）\n", PrivateBytes() / 1048576.0, gfx.deviceMemoryBytes() / 1048576.0);
    {
        for (int i = 0; i < 20; i++) {
            binui::BinUIVK tmp;
            if (!tmp.init(hwnd, 320, 200, false)) { Check(false, "临时实例初始化"); break; }
            tmp.beginFrame(nullptr);
            tmp.fillRect(0, 0, 320, 200, 0xFF3355AA);
            tmp.present();
            tmp.shutdown();
        }
        Check(true, "20 次 init/shutdown 不崩");
    }

    // ---- 7. 双实例并存 ----
    std::printf("[7] 双实例并存（提交 %.1f MB，后端显存 %.1f MB）\n", PrivateBytes() / 1048576.0, gfx.deviceMemoryBytes() / 1048576.0);
    {
        binui::BinUIVK second;
        const bool inited = second.init(hwnd, baseW, baseH, false);
        Check(inited, "第二个实例初始化成功");
        if (inited) {
            for (int i = 0; i < 10; i++) {
                Pump();
                DrawFrame(second, renderer);
                second.present();
                DrawFrame(gfx, renderer);
                gfx.present();
                frames++;
            }
            // 先销毁第二个实例：不能把还活着的第一个实例弄坏
            second.shutdown();
            DrawFrame(gfx, renderer);
            gfx.present();
            frames++;
            Check(gfx.isReady(), "销毁第二个实例后第一个仍可用");
        }
    }

    // ---- 收尾：恢复原始尺寸，捕获一帧供 diff ----
    SetWindowPos(hwnd, nullptr, 30, 30, baseW, baseH, SWP_NOZORDER | SWP_NOACTIVATE);
    renderer.setSize(baseW, baseH);
    gfx.resize(baseW, baseH);
    for (int i = 0; i < 3; i++) { DrawFrame(gfx, renderer); gfx.present(); frames++; }

    const double ms = NowMs() - t0;
    const size_t mem1 = PrivateBytes();
    std::printf("[vk_stress] %d 帧 / %.1f ms = %.1f fps，提交内存 %.1f MB → %.1f MB（+%.1f）\n",
                frames, ms, ms > 0 ? frames * 1000.0 / ms : 0.0,
                mem0 / 1048576.0, mem1 / 1048576.0, (mem1 - mem0) / 1048576.0);

    if (outPath) {
        int rw = 0, rh = 0;
        const uint8_t* px = gfx.readbackPixels(rw, rh);
        if (!px) std::fprintf(stderr, "[vk_stress] 读回为空\n");
        else if (!binuishot::SaveBgraPng(outPath, px, rw, rh))
            std::fprintf(stderr, "[vk_stress] 保存 PNG 失败: %s\n", outPath);
        else std::printf("[vk_stress] saved %s (%dx%d)\n", outPath, rw, rh);
    }

    gfx.shutdown();
    const size_t memAfterFirst = PrivateBytes();
    const size_t gpuAfterFirst = gfx.deviceMemoryBytes();   // shutdown 后应为 0

    // ---- 8. 泄漏判定：再跑一遍"初始化 + resize 抖动 + shutdown" ----
    // 思路：真泄漏会随轮数线性上涨；驱动/分配器的池化只会波动。
    // 判据：第二轮结束时的提交内存不超过第一轮的 +32MB（含图集/交换链池的合理波动）。
    std::printf("[8] 泄漏复跑（第一轮结束：提交 %.1f MB，后端显存 %.1f MB）\n",
                memAfterFirst / 1048576.0, gpuAfterFirst / 1048576.0);
    Check(gpuAfterFirst == 0, "shutdown 后后端显存记账归零（没有漏 vkAllocateMemory）");
    {
        binui::BinUIVK g2;
        if (g2.init(hwnd, 320, 200, false)) {
            for (int i = 0; i < 30; i++) {
                Pump();
                g2.resize(320 + i, 200 + i);
                DrawFrame(g2, renderer);
                g2.present();
            }
            g2.shutdown();
        } else {
            Check(false, "第二轮初始化");
        }
    }
    const size_t memAfterSecond = PrivateBytes();
    const double growMB = (static_cast<double>(memAfterSecond) -
                           static_cast<double>(memAfterFirst)) / 1048576.0;
    std::printf("  第一轮 %.1f MB → 第二轮 %.1f MB（差 %+.1f MB）\n",
                memAfterFirst / 1048576.0, memAfterSecond / 1048576.0, growMB);
    Check(growMB < 32.0, "两轮 shutdown 后提交内存无线性增长（≥32MB 视为泄漏）");

    DestroyWindow(hwnd);
    if (gdip) Gdiplus::GdiplusShutdown(gdipToken);
    std::printf("=== vk_stress: %s（%d 项失败）===\n", g_fail ? "FAILED" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
