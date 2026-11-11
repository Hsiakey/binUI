// appshow/main.cpp - binUI 应用壳演示（多页面 · 多级组件 · 编排在宿主）
//
// 这个例子要回答的是"多个页面/组件一起动时怎么配合"，所以它只用了**当前已有**的 API：
//   · 组件内的微动画     → 组件自己的 @keyframes（appshow-card 里那个呼吸圆点）
//   · 页面/弹窗的进出场   → 宿主逐帧 setStyleFloat(OPACITY / TRANSLATE_Y) 推 tween
//   · 先后与交错         → 宿主维护时间片（delay/dur），不用 CSS 的长 delay 假装编排
//   · 两态切换           → hover/press 走 CSS transition（引擎自动插值）
//
// 为什么不直接给页面容器写 animation：binUI 的 opacity/translate 是**节点自身**的属性，
// 不向后代继承，也没有"整棵子树一起动"的通道；所以整页动效必须由宿主对子树里的每个
// 节点设值。这是当前状态的真实成本，写在注释里而不是藏起来。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "binui_compiler.h"
#include "binui_paths.h"
#include "binui_renderer.h"
#include "backends/binui_ui.h"   // 编译期选后端：默认 GL，-DBINUI_GPU_VK 走 Vulkan
#include <windows.h>

using namespace binui;

namespace {

constexpr int kPages = 4;
constexpr int kRows = 6;
constexpr int kSwitch = 4;
constexpr double kPageH = 676.0;     // 内容视口高度 = 一"格"页面的高度
constexpr double kPush = 0.28;       // 整块推走/顶进来的时长（两页同时，硬连在一起）

const char* TypeName(uint8_t t) {
    switch (t) {
        case BINUI_ROOT:   return "root";
        case BINUI_LABEL:  return "label";
        case BINUI_IMAGE:  return "image";
        case BINUI_BUTTON: return "button";
        case BINUI_INPUT:  return "input";
        case BINUI_LIST:   return "list";
        case BINUI_LINE:   return "line";
        case BINUI_SVG:    return "svg";
        case BINUI_CANVAS: return "canvas";
        case BINUI_VIDEO:  return "video";
        case BINUI_PATH:   return "path";
        default:           return "?";
    }
}

float EaseOut(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t);
}

// 一条 tween：把一组节点的某个通道（opacity / translateY）从 from 插到 to。
// start/dur 是宿主的编排时间片；hold = start 之前也把 from 写下去（交错时"占住 0"）。
struct Fade {
    std::vector<uint16_t> ids;
    uint16_t prop = ANIM_OPACITY;
    float from = 1.0f, to = 1.0f;
    double start = 0.0, dur = 0.2;
    bool eased = true;
    bool easeInOut = false;
    bool hold = false;
    std::function<void()> onDone;
    bool done = false;
};

struct Shell {
    Shell(BinUIRenderer& renderer, UiApp& ui) : r(renderer), app(ui) {}

    BinUIRenderer& r;
    UiApp& app;

    double now = 0.0;          // 宿主时钟（秒）
    int curPage = 0;
    bool busy = false;         // 切页进行中：屏蔽重复点击
    double busyUntil = 0.0;
    bool modalOpen = false;
    bool confirmMode = false;
    bool sw[kSwitch] = {true, false, true, false};
    int colorIdx = 0;
    uint32_t palette[5] = {0xFF3E8BFF, 0xFF2FBF9A, 0xFFF2B544, 0xFFFF8A65, 0xFF38BDF8};

    std::vector<Fade> fades;
    std::unordered_map<uint16_t, uint16_t> parentOf;
    std::vector<std::vector<uint16_t>> pageSub;
    std::vector<uint16_t> dlgSub, toastSub;
    std::vector<uint16_t> navIds, indIds, rowIds, trIds, kfIds, knIds;
    std::vector<uint16_t> pageIds = std::vector<uint16_t>(kPages, 0);

    uint16_t nid(const char* name) { return r.findNodeByName(name); }

    void mapTree() {
        parentOf.clear();
        for (const auto& n : r.getNodes()) parentOf[n.id] = n.parentId;
    }
    std::vector<uint16_t> subtree(uint16_t root) {
        std::unordered_map<uint16_t, std::vector<uint16_t>> kids;
        for (const auto& n : r.getNodes())
            if (n.id != root && n.parentId != 0xFFFF) kids[n.parentId].push_back(n.id);
        std::vector<uint16_t> out{root};
        for (size_t i = 0; i < out.size(); i++) {
            auto it = kids.find(out[i]);
            if (it == kids.end()) continue;
            for (uint16_t c : it->second) out.push_back(c);
        }
        return out;
    }
    // 点到了子节点（文字/图标）时，沿父链找到"真正被点的那一件"
    uint16_t resolve(uint16_t id, const std::vector<uint16_t>& targets) {
        for (int guard = 0; id != 0xFFFF && guard < 64; guard++) {
            if (std::find(targets.begin(), targets.end(), id) != targets.end()) return id;
            auto it = parentOf.find(id);
            if (it == parentOf.end()) break;
            id = it->second;
        }
        return 0xFFFF;
    }

    void touch(uint16_t id) {
        float bx, by, bw, bh;
        if (r.invalidRect(id, bx, by, bw, bh, 4.0f))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    }
    void touchAll(const std::vector<uint16_t>& ids) {
        for (uint16_t id : ids) touch(id);
    }
    void setValue(const std::vector<uint16_t>& ids, uint16_t prop, float v) {
        for (uint16_t id : ids) r.setStyleFloat(id, prop, v);
    }

    void fade(std::vector<uint16_t> ids, float from, float to, double delay, double dur,
              std::function<void()> done = nullptr, uint16_t prop = ANIM_OPACITY,
              bool hold = false, bool inOut = false) {
        Fade f;
        f.ids = std::move(ids);
        f.prop = prop;
        f.from = from;
        f.to = to;
        f.start = now + delay;
        f.dur = dur;
        f.eased = true;
        f.easeInOut = inOut;
        f.hold = hold;
        f.onDone = std::move(done);
        if (delay <= 0.0) setValue(f.ids, f.prop, from);   // 起点立刻到位
        fades.push_back(std::move(f));
    }

    void tickFades() {
        for (auto& f : fades) {
            if (f.done) continue;
            if (now < f.start) {
                if (f.hold) { setValue(f.ids, f.prop, f.from); touchAll(f.ids); }
                continue;
            }
            const double k = f.dur <= 0.0 ? 1.0 : std::min(1.0, (now - f.start) / f.dur);
            float e = f.eased ? EaseOut((float)k) : (float)k;
            if (f.easeInOut)
                e = (float)k * (float)k * (3.0f - 2.0f * (float)k);   // smoothstep
            setValue(f.ids, f.prop, f.from + (f.to - f.from) * e);
            touchAll(f.ids);
            if (k >= 1.0) {
                f.done = true;
                if (f.onDone) f.onDone();
            }
        }
        fades.erase(std::remove_if(fades.begin(), fades.end(),
                                   [](const Fade& f) { return f.done; }),
                    fades.end());
    }

    void setStatus(const std::string& s) {
        uint16_t id = nid("status");
        if (!id) return;
        r.setText(id, s);
        touch(id);
    }

    void applySwitch(int i) {
        const bool on = sw[i];
        if (trIds[i]) r.setStyleColor(trIds[i], ANIM_BG_COLOR, on ? 0xFF3E8BFF : 0xFFDCE5F2);
        if (kfIds[i]) r.setVisible(kfIds[i], !on);
        if (knIds[i]) r.setVisible(knIds[i], on);
        for (uint16_t id : {trIds[i], kfIds[i], knIds[i]}) if (id) touch(id);
    }

    void replayRows(double extraDelay) {
        for (int i = 0; i < kRows; i++) {
            if (!rowIds[i]) continue;
            fade({rowIds[i]}, 0.0f, 1.0f, extraDelay + i * 0.045, 0.20, nullptr,
                 ANIM_OPACITY, /*hold=*/true);
            setValue({rowIds[i]}, ANIM_OPACITY, 0.0f);
        }
    }

    // ---- 页面编排：整块推（新页在后面顶上来 / 旧页被推走），方向跟着导航顺序 ----
    // "推上去"：从上层页面切到下层（next > prev），旧页往上走、新页从下面顶进来。
    // "顶下来"：反过来。两页同一条时间轴、同样时长 —— 它们是一节胶片，不能各走各的。
    void goPage(int next) {
        if (next < 0 || next >= kPages || next == curPage) return;
        if (busy && now < busyUntil) return;
        const int prev = curPage;
        const int dir = (next > prev) ? 1 : -1;   // +1 推上去，-1 顶下来
        curPage = next;
        busy = true;
        busyUntil = now + kPush + 0.06;

        for (int i = 0; i < kPages; i++) {
            if (indIds[i]) {
                r.setVisible(indIds[i], i == next);
                touch(indIds[i]);
            }
            if (navIds[i]) {
                r.setStyleColor(navIds[i], ANIM_BG_COLOR, i == next ? 0xFFE8F1FF : 0xFFFFFFFF);
                touch(navIds[i]);
            }
        }

        // 新页先摆到视口外（dir*H），旧页在 0：两页正好首尾相接
        setValue(pageSub[next], ANIM_TRANSLATE_Y, (float)(dir * kPageH));
        setValue(pageSub[next], ANIM_OPACITY, 1.0f);
        r.setVisible(pageIds[next], true);
        touchAll(pageSub[next]);

        // 同一条时长、同一条缓动：旧页 0 → -dir*H，新页 dir*H → 0
        fade(pageSub[prev], 0.0f, (float)(-dir * kPageH), 0.0, kPush, [this, prev]() {
            r.setVisible(pageIds[prev], false);
            setValue(pageSub[prev], ANIM_TRANSLATE_Y, 0.0f);   // 归位，下次直接可用
            touchAll(pageSub[prev]);
        }, ANIM_TRANSLATE_Y, /*hold=*/false, /*inOut=*/true);
        fade(pageSub[next], (float)(dir * kPageH), 0.0f, 0.0, kPush, nullptr,
             ANIM_TRANSLATE_Y, false, true);

        // 数据页的行交错放在推完之后，否则会跟整页位移打架
        if (next == 1) replayRows(kPush + 0.04);

        setStatus("页 " + std::to_string(next + 1) + "/" + std::to_string(kPages) +
                  (dir > 0 ? " · 推上去：旧页上移 676px，新页从下方顶上来 280ms"
                           : " · 顶下来：旧页下移 676px，新页从上方顶进来 280ms") +
                  (next == 1 ? "（随后 6 行按 45ms 交错）" : ""));
    }
    void openDialog(const std::string& title, const std::string& body, bool confirm) {
        if (modalOpen) return;
        modalOpen = true;
        confirmMode = confirm;
        if (uint16_t t = nid("dlg-title")) r.setText(t, title);
        if (uint16_t b = nid("dlg-body")) r.setText(b, body);
        if (uint16_t ok = nid("dlg-ok")) r.setText(ok, confirm ? "确认清空" : "确定");

        setValue(dlgSub, ANIM_OPACITY, 0.0f);
        setValue(dlgSub, ANIM_TRANSLATE_Y, 12.0f);
        setValue({nid("veil")}, ANIM_OPACITY, 0.0f);
        r.setVisible(nid("veil"), true);
        r.setVisible(nid("dlg-root"), true);
        touchAll(dlgSub);
        touch(nid("veil"));
        // 遮罩先起，面板晚 40ms 且自下 12px 抬起 —— 两个组件的相位差
        fade({nid("veil")}, 0.0f, 1.0f, 0.0, 0.16);
        fade(dlgSub, 0.0f, 1.0f, 0.04, 0.18);
        fade(dlgSub, 12.0f, 0.0f, 0.04, 0.18, nullptr, ANIM_TRANSLATE_Y);
        setStatus("弹窗：遮罩 0ms / 面板 +40ms（同一时间轴上的相位差）");
    }

    void closeDialog() {
        if (!modalOpen) return;
        modalOpen = false;
        fade(dlgSub, 1.0f, 0.0f, 0.0, 0.14, [this]() { r.setVisible(nid("dlg-root"), false); });
        fade(dlgSub, 0.0f, 8.0f, 0.0, 0.14, nullptr, ANIM_TRANSLATE_Y);
        fade({nid("veil")}, 1.0f, 0.0f, 0.04, 0.16, [this]() {
            r.setVisible(nid("veil"), false);
            touch(nid("veil"));
        });
        setStatus("弹窗关闭：面板先走，遮罩晚 40ms 收");
    }

    void toast(const std::string& text) {
        if (uint16_t t = nid("toast-txt")) r.setText(t, text);
        setValue(toastSub, ANIM_OPACITY, 0.0f);
        r.setVisible(nid("toast"), true);
        touchAll(toastSub);
        fade(toastSub, 0.0f, 1.0f, 0.0, 0.15);
        fade(toastSub, 1.0f, 0.0f, 1.7, 0.25, [this]() { r.setVisible(nid("toast"), false); });
    }

    // ---- 被动查询 ----
    std::string describe(uint16_t id) {
        for (const auto& n : r.getNodes()) {
            if (n.id != id) continue;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "节点 #%u（%s）\n矩形 x=%.0f y=%.0f w=%.0f h=%.0f\n父节点 #%u",
                          (unsigned)n.id, TypeName(n.type), (double)n.x, (double)n.y,
                          (double)n.width, (double)n.height, (unsigned)n.parentId);
            std::string s = buf;
            if (!n.text.empty()) s += "\n文本 \"" + n.text + "\"";
            return s;
        }
        return "（节点不存在）";
    }
    std::string hoverProbe() {
        const int h = r.hoveredNode();
        if (h < 0) return "悬停：无";
        for (const auto& n : r.getNodes())
            if (n.id == (uint16_t)h) {
                std::string s = "悬停 #" + std::to_string(h) + " " + TypeName(n.type);
                if (!n.text.empty()) s += " 「" + n.text + "」";
                return s;
            }
        return "悬停：?";
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    binui::ex::LaunchArgs args = binui::ex::ParseArgs(argc, argv);
    if (args.help) {
        std::printf("用法: appshow [--ui <main.html>]\n"
                    "  环境变量 BINUI_APPSHOW_AUTO=1：脚本化自检（打印每步不透明度/位移）\n");
        return 0;
    }

    BinUIRenderer renderer;
    const std::string uiPath = binui::ex::ResolveUI(args, "appshow", "main.html");
    CompileResult res;
    if (!uiPath.empty()) res = CompileBinUIFileEx(uiPath);
    if (res.layout.empty()) {
        std::fprintf(stderr, "[appshow] 未找到 UI 文件（用 --ui <path> 指定）\n");
        res.layout = GenerateDefaultBinUI();
    }
    if (!renderer.loadLayout(res.layout.data(), res.layout.size())) {
        std::fprintf(stderr, "[appshow] 布局加载失败\n");
        return 1;
    }

    const auto& wd = renderer.windowDesc();
    const auto& nodes0 = renderer.getNodes();
    const int width  = wd.present ? (int)wd.width  : (nodes0.empty() ? 1280 : (int)nodes0[0].width);
    const int height = wd.present ? (int)wd.height : (nodes0.empty() ? 800  : (int)nodes0[0].height);

    UiApp app(width, height);
    app.setWindowOptions(wd.present ? wd.resizable() : true, wd.present ? wd.aspect : 0,
                         wd.present && wd.transparent(), wd.title.c_str());

    Shell sh{renderer, app};
    sh.mapTree();

    // ---- 收集节点 id：多级结构在这里被"拍平"成几组，宿主只按组操作 ----
    for (int i = 0; i < kPages; i++) {
        sh.pageIds[i] = sh.nid((std::string("page-") + std::to_string(i)).c_str());
        sh.pageSub.push_back(sh.subtree(sh.pageIds[i]));
        sh.navIds.push_back(sh.nid((std::string("nav-") + std::to_string(i)).c_str()));
        sh.indIds.push_back(sh.nid((std::string("ind-") + std::to_string(i)).c_str()));
    }
    for (int i = 0; i < kRows; i++)
        sh.rowIds.push_back(sh.nid((std::string("row-") + std::to_string(i)).c_str()));
    for (int i = 0; i < kSwitch; i++) {
        sh.trIds.push_back(sh.nid((std::string("tr-") + std::to_string(i)).c_str()));
        sh.kfIds.push_back(sh.nid((std::string("kf-") + std::to_string(i)).c_str()));
        sh.knIds.push_back(sh.nid((std::string("kn-") + std::to_string(i)).c_str()));
    }
    sh.dlgSub = sh.subtree(sh.nid("dlg-root"));
    sh.toastSub = sh.subtree(sh.nid("toast"));

    // id 缺失会静默变成"点了没反应"，这里一次性喊出来（组件忘了写 id="{{nid=}}` 的坑）
    {
        int missing = 0;
        auto check = [&](const char* what, uint16_t id) {
            if (id) return;
            ++missing;
            std::fprintf(stderr, "[appshow] 警告：找不到节点 \"%s\"（点了不会响应）\n", what);
        };
        for (int i = 0; i < kPages; i++) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "page-%d", i); check(buf, sh.pageIds[i]);
            std::snprintf(buf, sizeof(buf), "nav-%d", i);  check(buf, sh.navIds[i]);
            std::snprintf(buf, sizeof(buf), "ind-%d", i);  check(buf, sh.indIds[i]);
        }
        for (int i = 0; i < kRows; i++) {
            char buf[32]; std::snprintf(buf, sizeof(buf), "row-%d", i); check(buf, sh.rowIds[i]);
        }
        if (missing == 0) std::printf("[appshow] 节点 id 自检：全部就位\n");
    }

    const uint16_t idVeil = sh.nid("veil");
    const uint16_t idDlgRoot = sh.nid("dlg-root");

    // 初始状态：只留第 1 页；遮罩/弹窗/提示条全隐藏
    for (int i = 0; i < kPages; i++) {
        if (sh.pageIds[i]) renderer.setVisible(sh.pageIds[i], i == 0);
        if (sh.indIds[i]) renderer.setVisible(sh.indIds[i], i == 0);
        if (sh.navIds[i])
            renderer.setStyleColor(sh.navIds[i], ANIM_BG_COLOR, i == 0 ? 0xFFE8F1FF : 0xFFFFFFFF);
    }
    for (uint16_t id : {idDlgRoot, idVeil, sh.nid("toast")})
        if (id) renderer.setVisible(id, false);
    // 隐藏的东西 opacity 也压到 0：否则 drawOpacity 报 1，第一次滑入会先"闪一下满不透明"
    for (int i = 1; i < kPages; i++) sh.setValue(sh.pageSub[i], ANIM_OPACITY, 0.0f);
    sh.setValue({idVeil}, ANIM_OPACITY, 0.0f);
    sh.setValue(sh.dlgSub, ANIM_OPACITY, 0.0f);
    sh.setValue(sh.toastSub, ANIM_OPACITY, 0.0f);
    for (int i = 0; i < kSwitch; i++) sh.applySwitch(i);

    // 点击路由表：命中测试返回的往往是子节点（文字/图标），沿父链爬到这些"可点目标"。
    std::vector<uint16_t> targets;
    auto addTarget = [&](uint16_t id) { if (id) targets.push_back(id); };
    for (auto& v : sh.pageSub) targets.insert(targets.end(), v.begin(), v.end());
    for (uint16_t id : sh.navIds) addTarget(id);
    for (uint16_t id : sh.indIds) addTarget(id);
    for (uint16_t id : sh.dlgSub) addTarget(id);
    addTarget(idVeil);
    addTarget(sh.nid("toast"));
    addTarget(sh.nid("win-min"));
    addTarget(sh.nid("win-max"));
    addTarget(sh.nid("win-close"));

    auto patch = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
        for (const auto& n : renderer.getNodes())
            if (n.id == id) return &n;
        return nullptr;
    };

    app.setDrawFn([&](IBinUIGfx& gfx, const void* clip) { drawUi(gfx, renderer, clip); });

    app.setMoveFn([&](float x, float y) {
        const int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        const int cur = renderer.hoveredNode();
        auto touchNode = [&](int id) {
            if (id < 0) return;
            float bx, by, bw, bh;
            if (renderer.invalidRect((uint16_t)id, bx, by, bw, bh, 4.0f))
                app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
        };
        if (old != cur) { touchNode(old); touchNode(cur); }
        if (cur != old) sh.setStatus(sh.hoverProbe());   // 被动查询：没有"悬停事件"，直接查
    });

    app.setPressFn([&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        renderer.press(x, y);
        const int n = renderer.hitTest(x, y);
        if (n >= 0) {
            float bx, by, bw, bh;
            if (renderer.invalidRect((uint16_t)n, bx, by, bw, bh, 4.0f))
                app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
        }
    });

    std::function<void(float, float)> onRelease = [&](float x, float y) {
        const int pressed = renderer.pressedNode();
        const int hit = renderer.release(x, y);
        if (pressed >= 0) {
            float bx, by, bw, bh;
            if (renderer.invalidRect((uint16_t)pressed, bx, by, bw, bh, 4.0f))
                app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
        }
        if (hit < 0) return;

        const uint16_t id = sh.resolve((uint16_t)hit, targets);
        if (!id) return;
        const auto* n = patch(id);
        if (!n) return;

        const bool inDialog = std::find(sh.dlgSub.begin(), sh.dlgSub.end(), id) != sh.dlgSub.end();
        const bool isVeil = (id == idVeil);
        const uint16_t idOk = sh.nid("dlg-ok");
        const uint16_t idNo = sh.nid("dlg-cancel");

        // 模态期：只放行弹窗内部与遮罩
        if (sh.modalOpen && !inDialog && !isVeil) return;

        // 标题栏
        if (id == sh.nid("win-min"))   { app.minimize(); return; }
        if (id == sh.nid("win-max"))   { app.maximize(); return; }
        if (id == sh.nid("win-close")) { app.requestExit(); return; }
        // 弹窗按钮 / 遮罩
        if (isVeil || id == idNo) { sh.closeDialog(); return; }
        if (id == idOk) {
            const bool wasConfirm = sh.confirmMode;
            sh.closeDialog();
            sh.toast(wasConfirm ? "已清空（假的）" : "已确认");
            if (wasConfirm) sh.setStatus("危险操作已确认：宿主在同一时间轴上接着排提示条");
            return;
        }
        // 导航
        for (int i = 0; i < kPages; i++)
            if (id == sh.navIds[i]) {
                std::printf("[appshow] nav -> page %d\n", i);
                sh.goPage(i);
                return;
            }
        // 开关
        for (int i = 0; i < kSwitch; i++)
            if (id == sh.trIds[i] || id == sh.kfIds[i] || id == sh.knIds[i]) {
                sh.sw[i] = !sh.sw[i];
                sh.applySwitch(i);
                sh.setStatus(std::string("开关 ") + std::to_string(i + 1) + " → " +
                             (sh.sw[i] ? "开" : "关") + "（状态存在宿主，引擎只给 setVisible）");
                return;
            }
        // 页面里的按钮
        if (id == sh.nid("btn-open")) {
            sh.openDialog("多级组件弹窗",
                          "壳 → 遮罩 → 弹窗组件 → 标题/正文/按钮，四层。\n"
                          "进出场不写在组件里：面板的淡入由宿主编排，\n"
                          "比遮罩晚 40ms 且自下抬起 12px —— 同一时间轴上的相位差。",
                          false);
            return;
        }
        if (id == sh.nid("btn-toast")) { sh.toast("已保存 · 用时 0.3 秒"); return; }
        if (id == sh.nid("btn-query")) {
            const int h = renderer.hoveredNode();
            sh.openDialog("被动查询结果",
                          h >= 0 ? sh.describe((uint16_t)h)
                                 : "当前没有悬停节点。\n把鼠标移到任意控件上再点按钮。",
                          false);
            return;
        }
        if (id == sh.nid("btn-replay")) {
            sh.replayRows(0.0);
            sh.setStatus("行交错重播：0/45/90/135/180/225ms 六档 delay");
            return;
        }
        if (id == sh.nid("btn-color")) {
            sh.colorIdx = (sh.colorIdx + 1) % 5;
            if (uint16_t sw = sh.nid("swatch")) {
                renderer.setStyleColor(sw, ANIM_BG_COLOR, sh.palette[sh.colorIdx]);
                sh.touch(sw);
            }
            sh.setStatus("色块换色：宿主只改目标色，0.3s 插值由引擎的 transition 完成");
            return;
        }
        if (id == sh.nid("btn-danger")) {
            sh.openDialog("确认清空",
                          "这会删除全部本地数据，且不可恢复。\n"
                          "（演示用：同一个弹窗组件，换文案 + 换按钮文字）",
                          true);
            return;
        }
        sh.setStatus("点了 #" + std::to_string(id) + " " + TypeName(n->type));
    };
    app.setReleaseFn([&](float x, float y) { onRelease(x, y); });

    app.setLeaveFn([&]() {
        const int h = renderer.hoveredNode();
        renderer.clearHover();
        if (h >= 0) {
            float bx, by, bw, bh;
            if (renderer.invalidRect((uint16_t)h, bx, by, bw, bh, 4.0f))
                app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
        }
    });

    app.setKeyFn([&](int vk) {
        if (vk >= '1' && vk <= '4') { sh.goPage(vk - '1'); return; }
        if (vk == 'D') {
            if (!sh.modalOpen) sh.openDialog("快捷键弹窗", "D 打开 / ESC 关闭。", false);
            return;
        }
        if (vk == VK_ESCAPE) {
            if (sh.modalOpen) sh.closeDialog();
            else app.requestExit();
            return;
        }
        uint32_t mods = 0;
        if (::GetKeyState(VK_SHIFT)   & 0x8000) mods |= BinUIRenderer::KEYMOD_SHIFT;
        if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= BinUIRenderer::KEYMOD_CTRL;
        const auto kr = renderer.handleKey(vk, mods);
        for (uint16_t id : kr.changed) sh.touch(id);
    });

    app.setImeActiveFn([&] { return renderer.isEditing(); });
    app.setFocusFn([&](bool focused) {
        if (!focused) {
            renderer.cancelPress();
            renderer.clearHover();
            renderer.blur();
            renderer.hideFocusRing();
            app.invalidateAll();
        }
    });
    app.setWheelFn([&](float x, float y, float dy) {
        const int id = renderer.scrollAt(x, y, dy);
        if (id >= 0) sh.touch((uint16_t)id);
    });

    int winW = width, winH = height;
    app.setResizeFn([&](int w, int h) { winW = w; winH = h; renderer.setSize(w, h); });

    // 自检模式（BINUI_APPSHOW_AUTO=1）：按脚本把页面/弹窗/交错过一遍，把每步的
    // 实际值打出来 —— 没有窗口抓图也能验证"宿主编排"真的在动。
    const bool autoDemo = std::getenv("BINUI_APPSHOW_AUTO") != nullptr;
    double demoT = 0.0;
    int demoStep = 0;
    const char* pendingReport = nullptr;
    auto demoReport = [&](const char* tag) {
        std::printf("[demo] %-10s t=%.2f dy=[%+.0f %+.0f %+.0f %+.0f] vis=%d%d%d%d veil=%.2f rows=",
                    tag, demoT,
                    (double)renderer.drawState(sh.pageIds[0]).offY,
                    (double)renderer.drawState(sh.pageIds[1]).offY,
                    (double)renderer.drawState(sh.pageIds[2]).offY,
                    (double)renderer.drawState(sh.pageIds[3]).offY,
                    (int)renderer.isNodeVisible(sh.pageIds[0]),
                    (int)renderer.isNodeVisible(sh.pageIds[1]),
                    (int)renderer.isNodeVisible(sh.pageIds[2]),
                    (int)renderer.isNodeVisible(sh.pageIds[3]),
                    (double)renderer.drawOpacity(idVeil));
        for (int i = 0; i < kRows; i++) std::printf("%.2f ", (double)renderer.drawOpacity(sh.rowIds[i]));
        std::printf("\n");
    };

    app.setUpdateFn([&](float dt) {
        sh.now += dt;
        if (autoDemo) {
            demoT += dt;
            // 全部走"按坐标点"的真实路径（press → release → 命中 → 冒泡 → 分发）
            auto autoClick = [&](float x, float y) {
                // 和真实鼠标一样：先 move（更新悬停），再 press，再 release
                renderer.setHover(x, y);
                renderer.press(x, y);
                onRelease(x, y);
            };
            struct Step { double at; int kind; float x, y; const char* tag; };
            // kind 0 = 点坐标；1 = 只报告
            static const Step steps[] = {
                {0.30, 0,   50.0f,  183.0f, "nav-1"},      // 侧栏第 2 项（点在文字上 → 冒泡到 nav-1）
                {0.44, 1,    0.0f,    0.0f, "push-40%"},
                {0.62, 1,    0.0f,    0.0f, "push-end"},
                {1.10, 0,   50.0f,  271.0f, "nav-3"},      // 第 4 项（推上去）
                {1.60, 1,    0.0f,    0.0f, "slide-end"},
                {1.90, 0,  305.0f,  429.0f, "btn-danger"}, // 第 4 页的"清空全部数据"
                {2.30, 1,    0.0f,    0.0f, "dlg-in"},
                {2.60, 0,  678.0f,  506.0f, "dlg-cancel"}, // 弹窗取消按钮
                {2.90, 1,    0.0f,    0.0f, "dlg-out"},
                {3.10, 0,   50.0f,  139.0f, "nav-0"},      // 回到第 1 项
                {3.50, 1,    0.0f,    0.0f, "final"},
            };
            const int n = (int)(sizeof(steps) / sizeof(steps[0]));
            if (demoStep < n && demoT > steps[demoStep].at) {
                const Step& s = steps[demoStep];
                if (s.kind == 0) autoClick(s.x, s.y);
                pendingReport = s.tag;
                demoStep++;
            } else if (demoStep >= n && demoT > steps[n - 1].at + 0.30) {
                demoStep = n + 1;
                app.requestExit();
            }

        }        // 引擎自己的动画时钟（组件内的 @keyframes）走 stepAnimations；
        // 宿主的编排时钟是上面的 sh.now，两者互不干扰。
        for (uint16_t id : renderer.stepAnimations()) sh.touch(id);
        sh.tickFades();
        // 报告放在动画步进之后：animResult_ 这时才反映本帧的设值（否则读到上一帧的值）
        if (pendingReport) { demoReport(pendingReport); pendingReport = nullptr; }
        (void)winW; (void)winH;
    });

    renderer.setEventCallback([&](const Event& ev) {
        if (ev.type == EventType::CLICK)
            std::printf("[appshow] click node=%u (%.1f, %.1f)\n", (unsigned)ev.nodeId,
                        (double)ev.click.x, (double)ev.click.y);
    });

    std::printf("[appshow] binUI 应用壳演示：4 页 / %d 节点\n", (int)renderer.getNodes().size());
    return app.run();
}
