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
constexpr int kBubbles = 20;
constexpr int kSwitch = 4;
constexpr double kPageH = 676.0;     // 一页的高度 = 视口高度（竖页带每格正好一屏）
// 滚动时长按"跨了几页"算：跨得越多滚得越久，跟浏览器平滑滚动一样
inline double ScrollDur(int steps) { return std::min(0.55, 0.16 + 0.085 * std::abs(steps)); }

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

// HSV → 0xAARRGGBB：组件里那两团反光每帧换色，直接算色相环比在 HTML 里写死一堆颜色省事
uint32_t HsvArgb(float h, float s, float v, uint8_t a) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const float c = v * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float rr = 0.0f, gg = 0.0f, bb = 0.0f;
    switch (static_cast<int>(hp) % 6) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    const float m = v - c;
    auto ch = [&](float f) {
        const int i = static_cast<int>(std::lround((f + m) * 255.0f));
        return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    return (static_cast<uint32_t>(a) << 24) | (ch(rr) << 16) | (ch(gg) << 8) | ch(bb);
}

// 一块组件的宿主侧状态：漂移速度 / 相位 / 色相 / 爆开进度。
// 组件（appshow-bubble）只画外观；"它会怎么飘"全在这里 —— 每颗的参数都不一样。
struct Bubble {
    uint16_t root = 0, body = 0, ring = 0, t = 0, sub = 0, tint = 0;
    float baseX = 0.0f, baseY = 0.0f, w = 0.0f, h = 0.0f;   // 布局位置与尺寸（宿主只做相对平移/缩放）
    float x = 0.0f, y = 0.0f;                    // 当前左上角（窗口坐标）
    float px = 0.0f, py = 0.0f;                  // 上一帧（脏矩形取新旧并集）
    float vx = 0.0f, vy = 0.0f;                  // px/s（vy < 0 = 上浮）
    float phase = 0.0f, breath = 0.03f, hz = 0.3f;
    float op = 0.9f;
    float hue = 0.0f, hueSpd = 12.0f;            // 色相漂移（度/秒）
    float popT = -1.0f;                          // <0 = 活着；>=0 = 戳破后经过的时间
    float popDur = 0.24f;
};

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
    uint16_t idViewport = 0;   // 竖页带所在的滚动视口
    double scrollPos = 0.0;    // 当前滚动偏移（像素，向下为正）
    float  pageH = (float)kPageH;   // 一页高度（= 视口高度），自检打印用
    double scrollFrom = 0.0, scrollTo = 0.0, scrollT0 = 0.0, scrollDur = 0.0;
    bool scrolling = false;
    // 子页面（布局与重排）：视口 **X** 滚动 0 ↔ kSubW，按钮触发、从右边切入
    static constexpr float kSubW = 1058.0f;
    uint16_t idSubPage = 0, idBtnSub = 0, idBtnBack = 0;
    bool   subOpen = false, subSliding = false;
    double subX = 0.0, subFrom = 0.0, subTo = 0.0, subT0 = 0.0, subDur = 0.0;
    bool modalOpen = false;
    bool confirmMode = false;
    bool sw[kSwitch] = {true, false, true, false};

    std::vector<Fade> fades;
    std::unordered_map<uint16_t, uint16_t> parentOf;
    std::vector<std::vector<uint16_t>> pageSub;
    std::vector<uint16_t> dlgSub, toastSub;
    std::vector<uint16_t> navIds, indIds, rowIds, trIds, kfIds, knIds;
    std::vector<uint16_t> pageIds = std::vector<uint16_t>(kPages, 0);

    // ---- 组件池（第 3 页）：状态全在宿主，组件只负责外观 ----
    std::vector<Bubble> bubs;
    uint16_t idPool = 0, idBtnPause = 0, idBubRead = 0;
    float poolL = 0.0f, poolT = 0.0f, poolR = 0.0f, poolB = 0.0f;   // 池内容盒（窗口坐标）
    bool bubblesPaused = false;
    int popped = 0;
    int lastPop = -1;      // 最近被戳破的组件下标（自检报告用）
    // 第 1 页的可缩放面板：自检时把"光标 / 缩放态 / 脏矩形"记下来报告
    int rsCursorEdge = 0, rsCursorCorner = 0;
    bool rsWasResizing = false;
    float rsDirty = 0.0f;
    float rsParaBefore = 0.0f, rsChipBefore = 0.0f;   // 拖动前的正文高度 / flex 子项宽度
    uint32_t rng = 0x2545F491u;
    double readT = 0.0;
    float frameMs = 16.0f;

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

    // ---- 组件池 ----
    // 宿主自己算命中：组件是被 translate/scale 挪走的，引擎的命中用的是**布局**矩形
    // （drawState 里的偏移不参与 hitTest），所以不能指望 renderer.hitTest。这里按圆心+半径算，
    // 后用"后画的在上面"的顺序取最上面那颗 —— 和绘制顺序一致。
    float rnd() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xFFFFFFu) / 16777215.0f;
    }
    float rndRange(float a, float b) { return a + (b - a) * rnd(); }

    void collectBubbles() {
        idBtnPause = nid("btn-pause");
        idBubRead = nid("bub-read");
        bubs.clear();
        for (int i = 0; i < kBubbles; i++) {
            const std::string p = "b" + std::to_string(i);
            Bubble b;
            b.root = nid(p.c_str());
            if (!b.root) continue;
            b.body = nid((p + "-body").c_str());
            b.ring = nid((p + "-ring").c_str());
            b.t = nid((p + "-t").c_str());
            b.sub = nid((p + "-s").c_str());
            b.tint = nid((p + "-tint").c_str());
            for (const auto& n : r.getNodes()) {
                if (n.id != b.root) continue;
                b.baseX = n.x; b.baseY = n.y; b.w = n.width; b.h = n.height;
                b.x = b.px = n.x;
                b.y = b.py = n.y;
                break;
            }
            if (b.w <= 0.0f || b.h <= 0.0f) continue;
            b.vx = rndRange(-9.0f, 9.0f);
            b.vy = -rndRange(10.0f, 30.0f);
            b.phase = rndRange(0.0f, 6.2831853f);
            b.breath = rndRange(0.018f, 0.045f);
            b.hz = rndRange(0.16f, 0.40f);
            b.op = rndRange(0.74f, 1.0f);
            b.hue = rndRange(0.0f, 360.0f);
            b.hueSpd = rndRange(8.0f, 24.0f) * (rnd() < 0.5f ? -1.0f : 1.0f);
            bubs.push_back(b);
        }
        if (bubs.empty()) return;
        auto pit = parentOf.find(bubs[0].root);
        if (pit == parentOf.end()) return;
        idPool = pit->second;
        for (const auto& n : r.getNodes()) {
            if (n.id != idPool) continue;
            poolL = n.x; poolT = n.y;
            poolR = n.x + n.width; poolB = n.y + n.height;
            break;
        }
    }

    // 组件的坐标是**布局**坐标（页带里第 3 页的那一段），屏幕坐标要减掉页带滚动量。
    // 宿主自己算脏矩形和命中时都必须换算，否则 ut 的不是同一套坐标。
    float ScreenY(float layoutY) const { return layoutY - static_cast<float>(scrollPos); }
    float ScreenX(float layoutX) const { return layoutX - static_cast<float>(subX); }

    // 子页面四张卡片的读数：引擎拖的是节点矩形，宿主每帧读回（不改任何状态）
    void updateSubReadout() {
        struct Row { uint16_t card; uint16_t label; };
        static Row rows[4] = {};
        if (!rows[0].card) {
            rows[0] = { nid("pg-card"),   nid("pg-sz")  };
            rows[1] = { nid("pg-clamp"),  nid("pg-sz2") };
            rows[2] = { nid("pg-pct"),    nid("pg-sz3") };
            rows[3] = { nid("pg-corner"), nid("pg-sz4") };
        }
        static std::string cache[4];
        for (int i = 0; i < 4; i++) {
            if (!rows[i].card || !rows[i].label) continue;
            const BinUIRenderer::NodeInfo* n = nullptr;
            for (const auto& k : r.getNodes())
                if (k.id == rows[i].card) { n = &k; break; }
            if (!n) continue;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.0f × %.0f", (double)n->width, (double)n->height);
            if (cache[i] == buf) continue;
            cache[i] = buf;
            r.setText(rows[i].label, cache[i]);
            // 脏矩形在屏幕坐标：布局矩形 − 视口滚动偏移（子页面是 X=1058）
            float bx, by, bw, bh;
            if (r.invalidRect(rows[i].card, bx, by, bw, bh, 4.0f))
                app.invalidateRect((int)ScreenX(bx), (int)ScreenY(by), (int)bw, (int)bh);
        }

        // ① 的 flex 行：数一下子项排成了几行（同一 y 算一行）+ 第一个子项的宽。
        // 拖窄卡片时 flex-wrap 会折行、容器 auto 高跟着长，下面那行读数就是证据。
        static uint16_t flexBox = 0, flexLabel = 0;
        static std::string flexCache;
        if (!flexBox) { flexBox = nid("pg-chips"); flexLabel = nid("pg-flex"); }
        if (flexBox && flexLabel) {
            float lineY[16];
            int lines = 0;
            float w0 = -1.0f;
            for (const auto& k : r.getNodes()) {
                if (k.parentId != flexBox) continue;
                bool seen = false;
                for (int j = 0; j < lines; j++)
                    if (std::fabs(lineY[j] - k.y) < 2.0f) seen = true;
                if (!seen && lines < 16) lineY[lines++] = k.y;
                if (w0 < 0.0f) w0 = k.width;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "flex %d 行 · 子项宽 %.0f", lines, (double)w0);
            if (flexCache != buf) {
                flexCache = buf;
                r.setText(flexLabel, flexCache);
                float fx, fy, fw, fh;
                if (r.invalidRect(flexBox, fx, fy, fw, fh, 6.0f))
                    app.invalidateRect((int)ScreenX(fx), (int)ScreenY(fy), (int)fw, (int)fh);
            }
        }
    }

    void touchBubble(const Bubble& b) {
        const float pad = 26.0f;
        float l = std::min(b.px, b.x) - pad;
        float t = ScreenY(std::min(b.py, b.y)) - pad;
        float rr = std::max(b.px, b.x) + b.w + pad;
        float bb = ScreenY(std::max(b.py, b.y)) + b.h + pad;
        l = std::max(l, poolL); t = std::max(t, ScreenY(poolT));
        rr = std::min(rr, poolR); bb = std::min(bb, ScreenY(poolB));
        if (rr <= l || bb <= t) return;
        app.invalidateRect((int)l, (int)t, (int)(rr - l), (int)(bb - t));
    }

    // 一块组件的所有图层都吃同一组 (dx, dy, scale, opacity)：图层各自是独立节点，
    // translate/scale 不向后代继承，所以必须逐层设值（这就是 binUI 当前的成本）。
    void applyBubble(Bubble& b, float t) {
        const float k = b.popT >= 0.0f ? std::min(1.0f, b.popT / b.popDur) : 0.0f;
        float s = 1.0f + b.breath * std::sin(t * b.hz * 6.2831853f + b.phase);
        float op = b.op * (0.94f + 0.06f * std::sin(t * 1.9f + b.phase * 1.7f));
        if (b.popT >= 0.0f) {
            s += 0.65f * EaseOut(k);      // 爆开：先胀再淡出
            op *= (1.0f - k);
        }
        const float dx = b.x - b.baseX, dy = b.y - b.baseY;
        auto set = [&](uint16_t id, float o) {
            if (!id) return;
            r.setStyleFloat(id, ANIM_TRANSLATE_X, dx);
            r.setStyleFloat(id, ANIM_TRANSLATE_Y, dy);
            r.setStyleFloat(id, ANIM_SCALE, s);
            r.setStyleFloat(id, ANIM_OPACITY, o);
        };
        set(b.body, op);
        set(b.ring, op * 0.95f);
        set(b.t, op);
        set(b.sub, op * 0.9f);
        set(b.tint, op * 0.95f);
        // 底部色条的颜色在色相环上跑：同一池子里每块的色相起点不同 → 叠起来才有变化
        if (b.tint) r.setStyleColor(b.tint, ANIM_BG_COLOR, HsvArgb(b.hue, 0.8f, 1.0f, 0x8C));
        touchBubble(b);
    }

    void respawn(Bubble& b) {
        b.x = rndRange(poolL + 8.0f, std::max(poolL + 9.0f, poolR - b.w - 8.0f));
        b.y = poolB + rndRange(6.0f, 120.0f);     // 从池底外面进场
        b.px = b.x; b.py = b.y;
        b.vx = rndRange(-9.0f, 9.0f);
        b.vy = -rndRange(10.0f, 30.0f);
        b.phase = rndRange(0.0f, 6.2831853f);
        b.hz = rndRange(0.16f, 0.40f);
        b.breath = rndRange(0.018f, 0.045f);
        b.hue = rndRange(0.0f, 360.0f);
        b.popT = -1.0f;
    }

    void updateBubbles(double dt) {
        if (bubs.empty()) return;
        const float t = static_cast<float>(now);
        for (auto& b : bubs) {
            b.px = b.x; b.py = b.y;
            if (b.popT >= 0.0f) {
                // 爆开是"已经发生的事"，暂停也让它演完 —— 否则暂停时戳破的组件会僵在
                // popT=0、永远不重生
                b.popT += static_cast<float>(dt);
                if (b.popT >= b.popDur) respawn(b);
                applyBubble(b, t);
                continue;
            }
            if (bubblesPaused) continue;   // 暂停：不推进漂移 / 呼吸 / 色相
            b.hue += b.hueSpd * static_cast<float>(dt);
            b.y += b.vy * static_cast<float>(dt);
            b.x += (b.vx + std::sin(t * 0.8f + b.phase) * 16.0f) * static_cast<float>(dt);
            // 池子左右是硬墙：蹭到就反弹（上浮方向不变）
            if (b.x < poolL) { b.x = poolL; b.vx = std::fabs(b.vx); }
            if (b.x + b.w > poolR) { b.x = poolR - b.w; b.vx = -std::fabs(b.vx); }
            if (b.y + b.h < poolT - 6.0f) respawn(b);   // 整块飘出顶 → 沉底重生
            applyBubble(b, t);
        }
    }

    bool tryPopBubble(float x, float y) {
        const float ly = y + static_cast<float>(scrollPos);   // 屏幕 → 布局
        if (x < poolL || x > poolR || ly < poolT || ly > poolB) return false;
        for (size_t i = bubs.size(); i-- > 0;) {
            Bubble& b = bubs[i];
            if (b.popT >= 0.0f) continue;
            if (x < b.x || x > b.x + b.w || ly < b.y || ly > b.y + b.h) continue;
            b.popT = 0.0f;
            ++popped;
            lastPop = static_cast<int>(i);
            setStatus("戳破一块：命中是宿主按矩形算的（引擎命中用布局矩形，跟随不了动画位移）");
            applyBubble(b, static_cast<float>(now));
            return true;
        }
        return false;
    }

    void gustBubbles() {
        for (auto& b : bubs) {
            if (b.popT >= 0.0f) continue;
            b.vy -= rndRange(28.0f, 66.0f);
            b.vx += rndRange(-42.0f, 42.0f);
        }
    }

    // 全部挪到池子中心：重叠面积最大化，方便看"半透明叠起来是什么颜色"
    void clumpBubbles() {
        const float cx = (poolL + poolR) * 0.5f, cy = (poolT + poolB) * 0.5f;
        for (auto& b : bubs) {
            if (b.popT >= 0.0f) continue;
            b.x = cx + rndRange(-70.0f, 70.0f) - b.w * 0.5f;
            b.y = cy + rndRange(-60.0f, 60.0f) - b.h * 0.5f;
            b.px = b.x; b.py = b.y;
            b.vx = rndRange(-4.0f, 4.0f);
            b.vy = -rndRange(3.0f, 10.0f);
            applyBubble(b, static_cast<float>(now));
        }
    }

    void resetBubbles() {
        const float span = std::max(1.0f, (poolR - poolL) - 16.0f);
        const float step = span / static_cast<float>(std::max<size_t>(1, bubs.size()));
        for (size_t i = 0; i < bubs.size(); i++) {
            Bubble& b = bubs[i];
            b.x = poolL + 8.0f + step * static_cast<float>(i) + rndRange(-10.0f, 10.0f);
            b.y = poolT + 8.0f + rndRange(0.0f, (poolB - poolT) * 0.55f);
            b.x = std::min(b.x, poolR - b.w - 8.0f);
            b.px = b.x; b.py = b.y;
            b.vx = rndRange(-9.0f, 9.0f);
            b.vy = -rndRange(10.0f, 30.0f);
            b.phase = rndRange(0.0f, 6.2831853f);
            b.popT = -1.0f;
            applyBubble(b, static_cast<float>(now));
        }
    }

    void updateBubbleReadout(double dt, bool force) {
        readT += dt;
        if (!force && readT < 0.25) return;
        readT = 0.0;
        if (!idBubRead) return;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "块 %d · 已戳破 %d · %s · %.1f ms/帧",
                      (int)bubs.size(), popped, bubblesPaused ? "暂停" : "漂浮中",
                      (double)frameMs);
        r.setText(idBubRead, buf);
        touch(idBubRead);
    }

    // ---- 页面编排：竖页带滚动（像浏览器里点锚点跳页）----
    // 四页在视口里是一条 4×676 的竖向页带，切页 = 把视口滚到第 N 页的偏移。
    // 跨几页就滚多久，中间那几页会在滚的过程中从眼前过去。
    void goPage(int next) {
        if (next < 0 || next >= kPages || next == curPage) return;
        // 在子页面里点侧栏：先把横向偏移瞬间归零（切页优先），再走上下滚动
        if (subOpen || subX != 0.0) {
            subOpen = false;
            subSliding = false;
            subX = 0.0;
        }
        const int prev = curPage;
        curPage = next;

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

        // 可打断：滚到一半再点一次，就从当前位置接着滚（浏览器行为）
        scrollFrom = scrollPos;
        scrollTo = next * kPageH;
        scrollT0 = now;
        scrollDur = ScrollDur(next - prev);
        scrolling = true;

        if (next == 1) replayRows(ScrollDur(next - prev) + 0.02);   // 到了再让 6 行逐个进场
        setStatus("页 " + std::to_string(next + 1) + "/" + std::to_string(kPages) +
                  " · 竖向页带滚动 " + std::to_string((int)std::lround(scrollDur * 1000)) +
                  "ms（跨 " + std::to_string(std::abs(next - prev)) + " 页）");
    }

    // ---- 子页面：视口横向滚动切入/退回 ----
    // 和上下切页共用同一个 scrollTo：竖页带在 x 0..1058，子页面排在 x 1058..2116。
    // 滚到 1058 时竖页带整条滑出去、子页面从右边顶进来；返回就滚回 0。
    void openSub() {
        if (subOpen) return;
        subOpen = true;
        curPage = 0;
        subFrom = subX;
        subTo = kSubW;
        subT0 = now;
        subDur = 0.30;
        subSliding = true;
        setStatus("子页面 · 布局与重排：视口 X 滚动 1058px（从右边切入）");
    }
    void closeSub() {
        if (!subOpen) return;
        subOpen = false;
        subFrom = subX;
        subTo = 0.0;
        subT0 = now;
        subDur = 0.26;
        subSliding = true;
        setStatus("返回概览（滑回 0）");
    }
    void stepSub() {
        if (!subSliding || !idViewport) return;
        const double k = subDur <= 0.0 ? 1.0 : std::min(1.0, (now - subT0) / subDur);
        const float e = 1.0f - (1.0f - (float)k) * (1.0f - (float)k);   // ease-out
        subX = subFrom + (subTo - subFrom) * e;
        if (k >= 1.0) {
            subSliding = false;
            subX = subTo;
        }
        applyScroll();
    }
    // 视口滚动 = (subX, scrollPos)：两条轴各归各的动画管，写回时一起提交
    void applyScroll() {
        if (!idViewport) return;
        r.scrollTo(idViewport, (float)subX, (float)scrollPos);
        float bx, by, bw, bh;
        if (r.invalidRect(idViewport, bx, by, bw, bh, 2.0f))
            app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
    }

    // 每帧推进滚动：只改引擎的滚动偏移，命中测试/裁剪/脏矩形都由引擎自己算
    void stepScroll() {
        if (!scrolling || !idViewport) return;
        const double k = scrollDur <= 0.0 ? 1.0
                       : std::min(1.0, (now - scrollT0) / scrollDur);
        const float e = 1.0f - (1.0f - (float)k) * (1.0f - (float)k);   // ease-out
        scrollPos = scrollFrom + (scrollTo - scrollFrom) * e;
        if (k >= 1.0) {
            scrolling = false;
            scrollPos = scrollTo;
        }
        applyScroll();
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
    sh.collectBubbles();   // 组件池：每块组件的 5 个图层 + 池子矩形
    if (!sh.bubs.empty())
        std::printf("[appshow] 半透明组件池：%d 块 · 池子 (%.0f,%.0f %.0fx%.0f)\n",
                    (int)sh.bubs.size(), (double)sh.poolL, (double)sh.poolT,
                    (double)(sh.poolR - sh.poolL), (double)(sh.poolB - sh.poolT));
    else
        std::fprintf(stderr, "[appshow] 警告：没找到组件节点（b0..b%d）\n", kBubbles - 1);

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
    // 初始状态：页带里四页都在（靠视口裁剪），视口滚到第 1 页；遮罩/弹窗/提示条隐藏
    // 视口 id 用 "page-0 的父节点" 定位：比依赖 id 名字稳（overflow:hidden 的容器在
    // 名称表里不一定有名字，见下面的告警）
    for (const auto& n : renderer.getNodes())
        if (n.id == sh.pageIds[0]) { sh.idViewport = n.parentId; break; }
    if (!sh.idViewport) sh.idViewport = sh.nid("viewport");
    for (int i = 0; i < kPages; i++) {
        if (sh.pageIds[i]) renderer.setVisible(sh.pageIds[i], true);
        if (sh.indIds[i]) renderer.setVisible(sh.indIds[i], i == 0);
        if (sh.navIds[i])
            renderer.setStyleColor(sh.navIds[i], ANIM_BG_COLOR, i == 0 ? 0xFFE8F1FF : 0xFFFFFFFF);
    }
    if (sh.idViewport) renderer.scrollTo(sh.idViewport, 0.0f, 0.0f);
    if (!sh.idViewport || !renderer.isScrollable(sh.idViewport))
        std::fprintf(stderr, "[appshow] 警告：页带视口不可滚动（idViewport=%u），切页不会滚\n",
                     (unsigned)sh.idViewport);
    else
        std::printf("[appshow] 页带视口 #%u，内容高 %.0f（视口高 %.0f）\n",
                    (unsigned)sh.idViewport, (double)renderer.contentHeight(sh.idViewport),
                    (double)sh.pageH);
    for (uint16_t id : {idDlgRoot, idVeil, sh.nid("toast")})
        if (id) renderer.setVisible(id, false);
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
    // 子页面入口 / 返回（都排在视口里，命中走引擎）
    sh.idBtnSub = sh.nid("btn-sub");
    sh.idBtnBack = sh.nid("btn-back");
    addTarget(sh.idBtnSub);
    addTarget(sh.idBtnBack);

    auto patch = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
        for (const auto& n : renderer.getNodes())
            if (n.id == id) return &n;
        return nullptr;
    };

    app.setDrawFn([&](IBinUIGfx& gfx, const void* clip) { drawUi(gfx, renderer, clip); });

    app.setMoveFn([&](float x, float y) {
        const int old = renderer.hoveredNode();
        renderer.setHover(x, y);
        // CSS resize 拖拽：引擎给出"旧∪新"脏矩形（屏幕坐标），照它重绘即可
        {
            float rx = 0, ry = 0, rw = 0, rh = 0;
            if (renderer.takeResizeDirty(rx, ry, rw, rh))
                app.invalidateRect((int)rx - 2, (int)ry - 2, (int)(rw + 4), (int)(rh + 4));
        }
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

    std::function<void(float, float)> onPress = [&](float x, float y) {
        if (isTitlebarDrag(renderer, x, y)) { app.beginWindowDrag(); return; }
        // 组件池：先自己判一次（组件被动画挪走了，引擎命中用的是布局矩形）
        if (sh.curPage == 2 && sh.tryPopBubble(x, y)) return;
        renderer.press(x, y);
        const int n = renderer.hitTest(x, y);
        if (n >= 0) {
            float bx, by, bw, bh;
            if (renderer.invalidRect((uint16_t)n, bx, by, bw, bh, 4.0f))
                app.invalidateRect((int)bx, (int)by, (int)bw, (int)bh);
        }
    };
    app.setPressFn([&](float x, float y) { onPress(x, y); });

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
        // 子页面：进入 / 返回（从右边切入、滑回去）
        if (id == sh.idBtnSub) { sh.openSub(); return; }
        if (id == sh.idBtnBack) { sh.closeSub(); return; }
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
        if (id == sh.nid("btn-gust")) {
            sh.gustBubbles();
            sh.setStatus("吹一口气：每块的浮速 +28~66 px/s、横向随机加速（宿主直接改速度）");
            return;
        }
        if (id == sh.nid("btn-clump")) {
            sh.clumpBubbles();
            sh.setStatus("叠在一起：20 块全挪到池子中心 —— 重叠区就是普通 alpha 合成的结果");
            return;
        }
        if (id == sh.nid("btn-pause")) {
            sh.bubblesPaused = !sh.bubblesPaused;
            if (sh.idBtnPause) {
                renderer.setText(sh.idBtnPause, sh.bubblesPaused ? "继续漂浮" : "暂停漂浮");
                sh.touch(sh.idBtnPause);
            }
            sh.setStatus(sh.bubblesPaused ? "池子已暂停：宿主只是不再推进自己的时钟"
                                          : "池子继续：位置/色相从暂停处接着走，没有跳变");
            sh.updateBubbleReadout(0.0, true);
            return;
        }
        if (id == sh.nid("btn-reset")) {
            sh.resetBubbles();
            sh.popped = 0;
            sh.setStatus("重排池子：宿主按池宽均分重新撒了一遍（组件一个都不改）");
            sh.updateBubbleReadout(0.0, true);
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
    // 光标形状交给引擎：输入框 I 型、可交互节点手型、可缩放元素的边/角是系统 resize 光标
    app.setCursorFn([&](float x, float y) { return (int)renderer.cursorAt(x, y); });

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
        // 滚动偏移直接问引擎（scrollState 是遍历/命中用的同一份状态）
        double sy = 0.0, sx = 0.0;
        if (const auto* ss = renderer.scrollState(sh.idViewport)) { sy = ss->y; sx = ss->x; }
        const int shown = (int)std::lround(sy / kPageH);
        std::printf("[demo] %-11s t=%.2f 滚动=(%.0f,%.0f)（看第%d页/子页 %s）veil=%.2f rows=",
                    tag, demoT, sx, sy, shown + 1, sh.subOpen ? "开" : "关",
                    (double)renderer.drawOpacity(idVeil));
        for (int i = 0; i < kRows; i++) std::printf("%.2f ", (double)renderer.drawOpacity(sh.rowIds[i]));
        std::printf("\n");
        if (!sh.bubs.empty()) {
            // 组件是宿主逐帧设值的：读回 drawState（= 遍历真正用来画的那份状态）才作数
            const Bubble& a = sh.bubs[0];
            const Bubble& c = sh.bubs[1];
            std::printf("[demo] %-11s 块 0 位置=(%.0f,%.0f) 速度=(%.0f,%.0f) scale=%.2f op=%.2f | "
                        "块 1 scale=%.2f op=%.2f | 已戳破 %d\n",
                        "", (double)a.x, (double)a.y, (double)a.vx, (double)a.vy,
                        (double)renderer.drawScale(a.body), (double)renderer.drawOpacity(a.body),
                        (double)renderer.drawScale(c.body), (double)renderer.drawOpacity(c.body),
                        sh.popped);
            std::printf("[demo] %-11s 帧 %.1f ms（20 块 × 5 层 × 4 个通道的设值）\n", "",
                        (double)sh.frameMs);
            if (sh.lastPop >= 0 && sh.lastPop < (int)sh.bubs.size()) {
                const Bubble& p = sh.bubs[sh.lastPop];
                std::printf("[demo] %-11s 被戳破的 #%d：位置=(%.0f,%.0f) popT=%.2f scale=%.2f op=%.2f\n",
                            "", sh.lastPop, (double)p.x, (double)p.y, (double)p.popT,
                            (double)renderer.drawScale(p.body), (double)renderer.drawOpacity(p.body));
            }
        }
        if (sh.rsCursorCorner > 0) {
            const auto* n = patch(sh.nid("pg-card"));
            const auto* para = patch(sh.nid("pg-para"));
            float chip = 0.0f;
            for (const auto& k : renderer.getNodes())
                if (k.parentId == sh.nid("pg-chips")) { chip = k.width; break; }
            // 光标 kind 与 BinUIRenderer::CursorKind 同值：3=上下 4=左右 5=⤡ 6=⤢
            std::printf("[demo] %-11s 子页面卡片 %.0f×%.0f（拖动前 640×296）"
                        "光标 右边缘=%d 右下角=%d 进入缩放态=%d 脏矩形宽=%.0f\n",
                        "", n ? (double)n->width : 0.0, n ? (double)n->height : 0.0,
                        sh.rsCursorEdge, sh.rsCursorCorner, (int)sh.rsWasResizing, (double)sh.rsDirty);
            std::printf("[demo] %-11s 自然重排：正文高 %.0f → %.0f（换行变高）· flex 子项宽 %.0f → %.0f\n",
                        "", (double)sh.rsParaBefore, para ? (double)para->height : 0.0,
                        (double)sh.rsChipBefore, (double)chip);
        }
    };

    app.setUpdateFn([&](float dt) {
        sh.now += dt;
        if (autoDemo) {
            demoT += dt;
            // 全部走"按坐标点"的真实路径（press → release → 命中 → 冒泡 → 分发）
            auto autoClick = [&](float x, float y) {
                // 和真实鼠标一样：先 move（更新悬停），再 press（走应用层的 press 回调），
                // 再 release。press 必须走 onPress —— 组件的命中在应用层，直接 renderer.press
                // 会绕过它，戳不破。
                renderer.setHover(x, y);
                onPress(x, y);
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
                {3.70, 0,   50.0f,  227.0f, "nav-2"},      // 第 3 项 → 组件/重叠页
                {4.20, 1,    0.0f,    0.0f, "bub-in"},
                {4.40, 2,    0.0f,    0.0f, "bub-pop"},    // 点一块组件（坐标由宿主现算）
                {4.52, 1,    0.0f,    0.0f, "bub-pop-mid"},
                {5.10, 1,    0.0f,    0.0f, "bub-respawn"},
                {5.30, 0,   50.0f,  139.0f, "nav-0-back"},  // 回第 1 页（"布局与重排"入口）
                {5.70, 4,    0.0f,    0.0f, "sub-open"},    // 点"进入"→ 子页面从右边切入
                {6.10, 1,    0.0f,    0.0f, "sub-in"},
                {6.40, 5,    0.0f,    0.0f, "sub-drag"},    // 拖子页面第一张卡的右下角
                {6.55, 1,    0.0f,    0.0f, "sub-drag-done"},
                {6.95, 6,    0.0f,    0.0f, "sub-back"},    // 点"返回"→ 滑回去
                {7.25, 1,    0.0f,    0.0f, "back-in"},
            };
            const int n = (int)(sizeof(steps) / sizeof(steps[0]));
            if (demoStep < n && demoT > steps[demoStep].at) {
                const Step& s = steps[demoStep];
                if (s.kind == 0) autoClick(s.x, s.y);
                if (s.kind == 2) {
                    // 点当前最上面那块活着的组件：位置/直径都是宿主状态里的实时值
                    for (size_t bi = sh.bubs.size(); bi-- > 0;) {
                        const Bubble& b = sh.bubs[bi];
                        if (b.popT >= 0.0f) continue;
                        autoClick(b.x + b.w * 0.5f, sh.ScreenY(b.y) + b.h * 0.5f);
                        break;
                    }
                }
                if (s.kind == 3) {
                    // 拖第 1 页 rs-all 的右下角：整条链路都是引擎的
                    // （光标 → press 命中热区 → move 改矩形 + 局部重排 → release 收尾）
                    const uint16_t id = sh.nid("rs-all");
                    const auto* n = patch(id);
                    if (n) {
                        // 页 0 在页带顶部且没滚动 → 布局坐标 == 窗口坐标
                        const float ex = n->x + n->width - 1.0f;
                        const float ey = n->y + n->height - 1.0f;
                        sh.rsCursorEdge   = (int)renderer.cursorAt(ex, n->y + n->height * 0.5f);
                        sh.rsCursorCorner = (int)renderer.cursorAt(ex, ey);
                        // 记录拖动前的自然内容高度（正文）与 flex 子项宽度
                        if (const auto* p = patch(sh.nid("rs-para"))) sh.rsParaBefore = p->height;
                        for (const auto& k : renderer.getNodes())
                            if (k.parentId == sh.nid("rs-chips")) { sh.rsChipBefore = k.width; break; }
                        renderer.setHover(ex, ey);
                        onPress(ex, ey);
                        sh.rsWasResizing = renderer.isResizing();
                        // 往里收 90×40：盒子变窄 → 正文折行变高，flex 子项变窄
                        renderer.setHover(ex - 210.0f, ey - 50.0f);
                        float dx = 0, dy = 0, dw = 0, dh = 0;
                        sh.rsDirty = renderer.takeResizeDirty(dx, dy, dw, dh) ? dw : 0.0f;
                        onRelease(ex - 210.0f, ey - 50.0f);
                    }
                }
                if (s.kind == 4) {
                    // 点"进入布局与重排"（入口按钮中心按名字现算）
                    const auto* b = patch(sh.nid("btn-sub"));
                    if (b) autoClick(b->x + b->width * 0.5f, b->y + b->height * 0.5f);
                }
                if (s.kind == 5) {
                    // 子页面第一张卡片的右下角：拖拽走引擎，脏矩形/局部重排全自动
                    // 屏幕坐标 = 布局坐标 − 视口滚动(subX, scrollPos)
                    const auto* n = patch(sh.nid("pg-card"));
                    const auto* para = patch(sh.nid("pg-para"));
                    if (n) {
                        sh.rsParaBefore = para ? para->height : 0.0f;
                        for (const auto& k : renderer.getNodes())
                            if (k.parentId == sh.nid("pg-chips")) { sh.rsChipBefore = k.width; break; }
                        const float ex = sh.ScreenX(n->x + n->width - 1.0f);
                        const float ey = sh.ScreenY(n->y + n->height - 1.0f);
                        sh.rsCursorEdge   = (int)renderer.cursorAt(ex, sh.ScreenY(n->y + n->height * 0.5f));
                        sh.rsCursorCorner = (int)renderer.cursorAt(ex, ey);
                        renderer.setHover(ex, ey);
                        onPress(ex, ey);
                        sh.rsWasResizing = renderer.isResizing();
                        renderer.setHover(ex - 260.0f, ey - 60.0f);
                        float dx = 0, dy = 0, dw = 0, dh = 0;
                        sh.rsDirty = renderer.takeResizeDirty(dx, dy, dw, dh) ? dw : 0.0f;
                        onRelease(ex - 260.0f, ey - 60.0f);
                    }
                }
                if (s.kind == 6) {
                    // 点"返回"（在子页面里：屏幕坐标要减 subX）
                    const auto* b = patch(sh.nid("btn-back"));
                    if (b) autoClick(sh.ScreenX(b->x + b->width * 0.5f),
                                     sh.ScreenY(b->y + b->height * 0.5f));
                }
                pendingReport = s.tag;
                demoStep++;
            } else if (demoStep >= n && demoT > steps[n - 1].at + 0.30) {
                demoStep = n + 1;
                app.requestExit();
            }

        }        // 引擎自己的动画时钟（组件内的 @keyframes）走 stepAnimations；
        // 宿主的编排时钟是上面的 sh.now，两者互不干扰。
        sh.frameMs = sh.frameMs * 0.9f + dt * 1000.0f * 0.1f;
        sh.stepScroll();
        sh.stepSub();
        // 子页面四张卡片的尺寸读数（引擎拖、宿主只读，不反向写尺寸）
        if (sh.subOpen && !sh.subSliding) sh.updateSubReadout();
        // 只在这一页、且页带没在滚的时候推进组件：
        //   · 不在这一页 → 省 ~600 次 setStyleFloat/帧
        //   · 正在滚 → 宿主算的脏矩形是"滚定后"的窗口坐标，滚动中会错位（引擎按滚动偏移
        //     自己挪子节点），所以滚动那 200ms 让组件先别动
        if (sh.curPage == 2 && !sh.scrolling) {
            sh.updateBubbles(dt);
            sh.updateBubbleReadout(dt, false);
        }
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

