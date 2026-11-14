// binui_draw_nodes.h - 节点树遍历（后端无关，v19 起统一）
//
// 为什么在这里：这段代码以前在 GL 与 D3D11 后端里各有一份（694 / 530 行），
// 改一处 UI 语义要改两遍（v13 扩展绘制段就是这样漏的）。抽出 IBinUIGfx 后它只留一份，
// 新后端（Vulkan / Metal）只要实现接口即可复用同一份遍历。
//
// 约定（见 docs/vulkan_design.md §1）：只通过 IBinUIGfx 的 7 个原语 + 裁剪栈绘制，
// 不碰任何后端私有状态；clipRect 是脏矩形，遍历内部所有绘制都裁在它之内。
#ifndef BINUI_DRAW_NODES_H
#define BINUI_DRAW_NODES_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "binui_gfx.h"
#include "binui_paint.h"     // PaintBand / BuildFillBands（路径与渐变条带）
#include "binui_renderer.h"
#include "binui_text_estimate.h"   // 折行行数估算（与布局同一份口径）

namespace binui {

// 按透明度缩放 alpha（0xAARRGGBB；RGB 保持直线色，预乘时统一缩放）。
// GL/D3D11 后端各自都有同名实现，这里是遍历用的共享版本。
inline uint32_t GfxScaleAlpha(uint32_t c, float op) {
    if (op >= 1.0f) return c;
    if (op <= 0.0f) return c & 0x00FFFFFF;
    uint32_t a = static_cast<uint32_t>(((c >> 24) & 0xFF) * op);
    if (a > 255) a = 255;
    return (c & 0x00FFFFFF) | (a << 24);
}

// 光标闪烁相位（500ms 周期，绘制与 C ABI 的光标位置共用同一相位）
inline bool GfxCaretBlinkOn() {
    using namespace std::chrono;
    static auto last = steady_clock::now();
    static bool on = true;
    const auto now = steady_clock::now();
    if (duration_cast<milliseconds>(now - last).count() >= 500) {
        on = !on;
        last = now;
    }
    return on;
}

// ============================================================
// drawNodesGLGdi：与 drawNodesGDI/D3D 语义一致的节点树 GL 绘制
// ============================================================
// ============================================================
// 视频节点绘制辅助（v9，与 D3D11 后端同构）
// ============================================================

static void FormatVideoTime(double sec, char* buf, size_t n) {
    if (sec < 0) sec = 0;
    int s = (int)(sec + 0.5);
    snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

// 控制条：底条 + 进度轨道/已播放/滑块 + 播放暂停图标 + 时间文本
template <typename Gfx>
static void DrawVideoControlsTmpl(Gfx& gfx, const BinUIRenderer& renderer,
                                  const BinUIRenderer::NodeInfo& node,
                                  float x, float y, float nw, float opacity) {
    (void)y;   // 控制条几何按节点矩形算，纵向不用外部传入
    BinUIRenderer::VideoBarGeo g;
    if (!BinUIRenderer::VideoBarLayout(node, g)) return;
    const auto* v = renderer.findVideo(node.id);
    if (!v) return;

    uint32_t barBg   = GfxScaleAlpha(0xB4000000, opacity);
    uint32_t trackC  = GfxScaleAlpha(0x4DFFFFFF, opacity);
    uint32_t solidC  = GfxScaleAlpha(0xFFFFFFFF, opacity);
    uint32_t timeC   = GfxScaleAlpha(0xD9FFFFFF, opacity);

    // 底条（覆盖到视频底缘）
    gfx.fillRect(x, g.y, nw, g.h, barBg);

    double dur = v->duration, pos = v->position;
    float ratio = (dur > 0) ? (float)std::min(std::max(pos / dur, 0.0), 1.0) : 0.0f;

    // 进度轨道 + 已播放段
    gfx.fillRounded(g.trX, g.trY, g.trW, g.trH, g.trH * 0.5f, trackC);
    float playedW = g.trW * ratio;
    if (playedW >= 2.0f)
        gfx.fillRounded(g.trX, g.trY, playedW, g.trH, g.trH * 0.5f, solidC);

    // 滑块（圆）
    float tcx = g.trX + playedW;
    float tcy = g.trY + g.trH * 0.5f;
    gfx.fillRounded(tcx - g.thumbR, tcy - g.thumbR,
                    g.thumbR * 2, g.thumbR * 2, g.thumbR, solidC);

    // 播放/暂停图标（按钮中心）
    float bx = g.btnX + g.btnS * 0.5f;
    float by = g.btnY + g.btnS * 0.5f;
    if (v->playing) { // ⏸ 两根竖条
        gfx.fillRect(bx - 5, by - 7, 3.5f, 14.0f, solidC);
        gfx.fillRect(bx + 1.5f, by - 7, 3.5f, 14.0f, solidC);
    } else {          // ▶ 行扫描三角形（宽边在左、顶点朝右）
        for (int j = 0; j < 7; j++) {
            float frac = 1.0f - std::fabs(j - 3) / 4.0f;
            float wl = 13.0f * frac;
            if (wl >= 1.0f) gfx.fillRect(bx - 5.5f, by - 7 + j * 2.0f, wl, 2.0f, solidC);
        }
    }

    // 时间文本（右对齐）：m:ss / m:ss
    char cur[16] = "0:00", tot[16] = "0:00";
    FormatVideoTime(pos, cur, sizeof(cur));
    if (dur > 0) FormatVideoTime(dur, tot, sizeof(tot));
    std::string label = std::string(cur) + " / " + tot;
    float tw = gfx.textWidth(12.0f, 400, label);
    int asc = 0, lh = 0;
    gfx.fontMetrics(12.0f, 400, asc, lh);
    gfx.drawText(x + nw - 10 - tw, g.y + (g.h - lh) * 0.5f + asc,
                 12.0f, 400, label, timeC);
}

// ============================================================
// 遍历分段计时（BINUI_DRAW_PROF=1 → 每 60 帧往 stderr 打一行）
// ------------------------------------------------------------
// 只统计"遍历本身"的 CPU 时间（各节点分支 + 前置索引构建），不含后端 present。
// 用它回答"CPU 侧还剩多少、在哪个节点类型上"，而不是靠猜。
// 关掉时是一根可预测的空分支（static const bool，只查一次环境变量）。
// ============================================================
struct DrawProfBucket {
    double us = 0;
    int n = 0;
};
struct DrawProf {
    DrawProfBucket index;                    // byId / scrolls / gutter 前置
    DrawProfBucket byType[32];               // 节点分支（按 BinUINodeType）
    DrawProfBucket boundsCull;               // 视口/脏矩形剔除掉、没进 switch 的
    int nodes = 0;
    int frames = 0;
    double frameUs = 0;
};

inline double DrawProfNowUs() {
    using namespace std::chrono;
    return duration<double, std::micro>(steady_clock::now().time_since_epoch()).count();
}

inline DrawProf& DrawProfState() {
    static DrawProf p;
    return p;
}

inline bool DrawProfEnabled() {
    static const bool on = std::getenv("BINUI_DRAW_PROF") != nullptr;
    return on;
}

inline void DrawProfFlush() {
    DrawProf& p = DrawProfState();
    const double n = p.frames > 0 ? static_cast<double>(p.frames) : 1.0;
    std::fprintf(stderr,
                 "[draw][prof] %d 帧: 遍历 %.3f ms/帧，节点 %d/帧（索引 %.3f / 剔除 %.3f）\n",
                 p.frames, p.frameUs / n / 1000.0, static_cast<int>(p.nodes / n),
                 p.index.us / n / 1000.0, p.boundsCull.us / n / 1000.0);
    for (int t = 0; t < 32; t++) {
        if (p.byType[t].n == 0) continue;
        std::fprintf(stderr, "[draw][prof]   type %2d: %.3f ms/帧（%d 个/帧，%d 次）\n", t,
                     p.byType[t].us / n / 1000.0, static_cast<int>(p.byType[t].n / n),
                     p.byType[t].n);
    }
    std::fflush(stderr);
    p = DrawProf{};
}

// ============================================================
// 任意角度渐变底色的条带缓存（v0.16.13）
// ------------------------------------------------------------
// 斜向渐变的条带是"逐行横向切段"来的，千级（180×140 的 135° 卡片约 2.3k 条、
// 256×256 约 2.3k 条）。逐帧重算是纯浪费：静态页面上这些输入一个字节都没变。
// 与路径条带缓存（renderer 里的 PathBandCacheEntry）同一个思路，只是键放在这里：
//   id + 盒(4) + 圆角 + 两端色 + 角度
// 任意一项变了就重算，所以不需要额外的失效入口。
// 内存上限 2 MB，超了就淘汰最久没用的那条（正常页面就几条）。
// ============================================================
struct BgBandsCache {
    struct Entry {
        uint16_t nodeId = 0;
        uint32_t sig = 0;
        uint64_t usedFrame = 0;
        std::vector<PaintBand> bands;
    };
    std::vector<Entry> items;
    uint64_t frame = 0;
    size_t bytes = 0;
};

inline BgBandsCache& BgBandsCacheState() {
    static thread_local BgBandsCache c;
    return c;
}

inline uint32_t BgBandsSig(float x, float y, float w, float h, float radius,
                           uint32_t from, uint32_t to, int angle) {
    auto mix = [](uint32_t hsh, uint32_t v) {
        hsh ^= v + 0x9E3779B9u + (hsh << 6) + (hsh >> 2);
        return hsh;
    };
    uint32_t s = 0x811C9DC5u;
    const float vals[5] = { x, y, w, h, radius };
    for (float f : vals) {
        uint32_t b = 0;
        std::memcpy(&b, &f, sizeof(b));
        s = mix(s, b);
    }
    s = mix(s, from);
    s = mix(s, to);
    s = mix(s, static_cast<uint32_t>(angle));
    return s;
}

// 取（或建）这个节点的条带。返回的指针在下次调用前有效。
inline const std::vector<PaintBand>* BgBandsGet(uint16_t nodeId, uint32_t sig,
                                                const BinUIPaintNode* p, float x, float y,
                                                float w, float h, float radius) {
    BgBandsCache& c = BgBandsCacheState();
    constexpr size_t kMaxBytes = 2u << 20;
    BgBandsCache::Entry* slot = nullptr;
    for (BgBandsCache::Entry& e : c.items) {
        if (e.nodeId == nodeId) { slot = &e; break; }
    }
    if (!slot) {
        if (c.items.size() >= 64) {                    // 条目数也设个上限
            size_t oldest = 0;
            for (size_t i = 1; i < c.items.size(); i++)
                if (c.items[i].usedFrame < c.items[oldest].usedFrame) oldest = i;
            c.bytes -= c.items[oldest].bands.size() * sizeof(PaintBand);
            c.items[oldest] = c.items.back();
            c.items.pop_back();
        }
        c.items.emplace_back();
        slot = &c.items.back();
        slot->nodeId = nodeId;
        slot->sig = 0;                                  // 强制重建
    }
    slot->usedFrame = c.frame;
    if (slot->sig != sig || slot->bands.empty()) {
        const int cap = kAngleGradBands;
        c.bytes -= slot->bands.size() * sizeof(PaintBand);
        slot->bands.resize(static_cast<size_t>(cap));
        const int an = BuildFillBandsAngleGrad(p->gradFrom, p->gradTo,
                                               static_cast<float>(p->reserve[1] - 1),
                                               x, y, w, h, radius, slot->bands.data(), cap);
        slot->bands.resize(an > 0 ? static_cast<size_t>(an) : 0);
        c.bytes += slot->bands.size() * sizeof(PaintBand);
        slot->sig = sig;
        // 预算超了就整体丢一半最旧的条目（正常页面碰不到这条路径）
        if (c.bytes > kMaxBytes && c.items.size() > 1) {
            for (size_t i = 0; i < c.items.size();) {
                if (&c.items[i] != slot && c.items[i].usedFrame + 1 < c.frame) {
                    c.bytes -= c.items[i].bands.size() * sizeof(PaintBand);
                    c.items.erase(c.items.begin() + static_cast<long>(i));
                } else {
                    i++;
                }
            }
        }
    }
    return &slot->bands;
}

inline void drawNodesBinUI(IBinUIGfx& gfx, const BinUIRenderer& renderer, const void* clipRect,
                 const CanvasFn& canvas) {
    const auto& nodes = renderer.getNodes();
    int winW = gfx.width();
    int winH = gfx.height();
    const bool prof = DrawProfEnabled();
    DrawProf& pr = DrawProfState();
    const double tFrame0 = prof ? DrawProfNowUs() : 0.0;
    ++BgBandsCacheState().frame;   // 背景条带缓存的"最近使用"时钟

    // ★ 环境变量**只查一次**：下面两处调试打印原本在每个节点身上调 std::getenv，
    // 而 UCRT 的 getenv 每次都要遍历环境表（≈0.5 µs/次）。一个几千节点的页面光这一项
    // 就要 1 ms+；同样的坑在 Vulkan 后端的 fillRect 里也踩到过（见 binui_vk.cpp）。
    static const bool dbgClip = std::getenv("BINUI_DEBUG_CLIP") != nullptr;

    int clipL = 0, clipT = 0, clipR = winW, clipB = winH;
    if (clipRect) {
        const int* c = static_cast<const int*>(clipRect);
        int l = c[0], t = c[1], r = c[2], b = c[3];
        if (r > l && b > t && l >= 0 && t >= 0 && r <= winW && b <= winH) {
            clipL = l; clipT = t; clipR = r; clipB = b;
        }
    }

    const double tIndex0 = prof ? DrawProfNowUs() : 0.0;
    // 节点 id → 下标。以前每帧先建一张 unordered_map（80 个节点 5 µs，还要 malloc），
    // 再在每个祖先上查它；其实编译器分配的 id 是 0..N-1 稠密递增、与下标同序，
    // 一次数组直读 + id 比对就够了。只有运行期增删过节点（顺序被打乱）的页面
    // 才会真的去建那张表 —— 而且只建一次，不是每帧。
    const size_t nodeCount = nodes.size();
    std::unordered_map<uint16_t, size_t> byId;
    bool byIdBuilt = false;
    auto indexOf = [&](uint16_t id) -> size_t {
        const size_t k = static_cast<size_t>(id);
        if (k < nodeCount && nodes[k].id == id) return k;
        if (!byIdBuilt) {
            byId.reserve(nodeCount);
            for (size_t i = 0; i < nodeCount; i++) byId[nodes[i].id] = i;
            byIdBuilt = true;
        }
        auto it = byId.find(id);
        return it == byId.end() ? static_cast<size_t>(-1) : it->second;
    };

    std::unordered_map<uint16_t, binui::BinUIRenderer::ScrollState> scrolls;
    // 真的要出滚动条的容器 → 给它留 gutter（右侧 / 底部扣掉滚动条宽度）。
    // 以前滚动条直接压在内容上：最右一列文字/气泡会被盖住一截（docs/issues/05）。
    std::unordered_map<uint16_t, int32_t> gutterR, gutterB;
    const int32_t sbW = static_cast<int32_t>(renderer.scrollbarStyle().width);
    for (const auto& node : nodes) {
        if (node.flags & BINUI_SCROLLABLE) {
            if (const auto* s = renderer.scrollState(node.id)) scrolls[node.id] = *s;
            // gutter 只为"真的会画滚动条"的容器预留（overflow:auto/scroll）；
            // overflow:hidden 的只裁剪容器不出滚动条，留 gutter 等于白裁掉一条内容
            if (renderer.isUserScrollable(node.id)) {
                if (renderer.contentHeight(node.id) > node.height + 0.5f) gutterR[node.id] = sbW;
                if (renderer.contentWidth(node.id) > node.width + 0.5f) gutterB[node.id] = sbW;
            }
        }
    }

    // 预计算：父传子祖先索引 / 隐藏 / 滚动偏移 / 最内层滚动视口
    std::vector<int32_t> parentIdx(nodes.size(), -1);
    std::vector<uint8_t> hidden(nodes.size(), 0);
    std::vector<float> offX(nodes.size(), 0), offY(nodes.size(), 0);
    std::vector<int32_t> vpL(nodes.size(), 0), vpT(nodes.size(), 0),
                         vpR(nodes.size(), 0), vpB(nodes.size(), 0);
    std::vector<uint8_t> hasVp(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); i++) {
        const auto& node = nodes[i];
        if (node.parentId == 0xFFFF) continue;
        const size_t pi = indexOf(node.parentId);
        if (pi == static_cast<size_t>(-1)) continue;
        parentIdx[i] = static_cast<int32_t>(pi);
        if (hidden[pi] || !(nodes[pi].flags & BINUI_VISIBLE)) { hidden[i] = 1; continue; }
        offX[i] = offX[pi];
        offY[i] = offY[pi];
        const auto& par = nodes[pi];
        if (par.flags & BINUI_SCROLLABLE) {
            auto sit = scrolls.find(par.id);
            if (sit != scrolls.end()) { offX[i] += sit->second.x; offY[i] += sit->second.y; }
            // 视口矩形是"容器在屏幕上的位置"：容器自己可能在被滚动的内容里
            // （页面里的 #pool 就在滚动的页带里），所以要减掉**它祖先**的滚动偏移。
            // 以前用布局坐标，嵌套裁剪容器一进滚动视口就把子节点全裁没了。
            hasVp[i] = 1;
            vpL[i] = static_cast<int32_t>(par.x - offX[pi]);
            vpT[i] = static_cast<int32_t>(par.y - offY[pi]);
            vpR[i] = static_cast<int32_t>(par.x - offX[pi] + par.width);
            vpB[i] = static_cast<int32_t>(par.y - offY[pi] + par.height);
            // 出滚动条时右侧/底部留 gutter：内容裁在滚动条内侧，不再被它压住
            auto gitR = gutterR.find(par.id);
            if (gitR != gutterR.end()) vpR[i] -= gitR->second;
            auto gitB = gutterB.find(par.id);
            if (gitB != gutterB.end()) vpB[i] -= gitB->second;
        } else {
            hasVp[i] = hasVp[pi];
            vpL[i] = vpL[pi]; vpT[i] = vpT[pi];
            vpR[i] = vpR[pi]; vpB[i] = vpB[pi];
        }
    }

    // 根节点背景（填充绘制范围；RGBA 画布按 alpha 与底色合成）
    for (const auto& node : nodes) {
        if (node.type == BINUI_ROOT && (node.flags & BINUI_VISIBLE)) {
            gfx.fillRect(static_cast<float>(clipL), static_cast<float>(clipT),
                         static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT),
                         node.color);
            break;
        }
    }
    if (prof) {
        pr.index.us += DrawProfNowUs() - tIndex0;
        pr.frames++;
    }

    // 绘制顺序：按 z-index 分层（v10）由渲染器统一给出，同层流式先于 absolute、DOM 顺序
    // 裁剪链缓存：每个节点复用，避免逐节点分配
    std::vector<uint16_t> clipAncs;
    const std::vector<uint32_t> order = renderer.drawOrder();

    // ---- 组透明度（v0.16.9）：把半透明祖先的子树包进离屏图层 ----
    // CSS 的 opacity 是"组"语义：整棵子树先合成、再整体乘一次 alpha。0.16.8 用"逐元素乘"
    // 近似（子元素重叠处会叠深），这里在有能力的后端上换成真正的离屏合成：
    //   · 只对**绘制顺序连续**的子树建层（父先于子、同 z 连续 —— 普通布局都满足）；
    //     被 z-index 打散的子树退回逐元素乘法，只是近似，不会画错顺序；
    //   · 层数受后端限制（GL 池子 4 层），pushLayer 返回 false 就退回逐元素模式；
    //   · 进了层就把这些祖先的 alpha 从子节点颜色里"除回来"（层在 pop 时整体乘一次）。
    struct LayerNode { size_t idx; int lastPos; float op; };
    std::vector<LayerNode> layerStack;
    float layerAccum = 1.0f;          // 已经由图层承受掉的 alpha 乘积
    // 每个节点的"自己"的 opacity（静态 × 动画），以及父下标
    std::vector<float> ownOp;
    std::vector<int32_t> pIdx;
    std::unordered_map<size_t, std::pair<int, int>> ownerSpan;   // 层候选：首/末位置
    std::unordered_map<size_t, int> ownerCount;                  // 层候选：子树里画了几个节点
    {
        ownOp.resize(nodeCount, 1.0f);
        pIdx.assign(nodeCount, -1);
        bool anyOp = false;
        for (size_t i = 0; i < nodeCount; i++) {
            ownOp[i] = renderer.ownOpacity(nodes[i].id);
            if (ownOp[i] < 0.999f) anyOp = true;
            const uint16_t pid = nodes[i].parentId;
            if (pid != 0xFFFF && pid != nodes[i].id) {
                const size_t pi = indexOf(pid);
                if (pi != static_cast<size_t>(-1)) pIdx[i] = static_cast<int32_t>(pi);
            }
        }
        if (anyOp) {
            // 一次遍历统计每个半透明祖先覆盖的绘制位置（首、末、个数）
            for (size_t pos = 0; pos < order.size(); pos++) {
                const size_t i = order[pos];
                if (!(nodes[i].flags & BINUI_VISIBLE) || hidden[i]) continue;
                // 从**节点自己**开始往上走：层作用域必须把 owner 自己的底色/边框也包进去。
                // 只统计后代的话，父元素底会被留在主目标上、只有子元素进层 —— 子元素半透明
                // 合成时就会拿"已经被淡过的父底"当背景，像素比浏览器的组语义更暗。
                int32_t a = static_cast<int32_t>(i);
                for (int guard = 0; a >= 0 && guard < 512; guard++) {
                    const size_t ai = static_cast<size_t>(a);
                    if (ownOp[ai] < 0.999f) {
                        auto it = ownerSpan.find(ai);
                        if (it == ownerSpan.end()) ownerSpan[ai] = { static_cast<int>(pos), static_cast<int>(pos) };
                        else it->second.second = static_cast<int>(pos);
                        ownerCount[ai]++;
                    }
                    a = pIdx[ai];
                }
            }
            // 只保留"连续"的候选（跨度长度 == 覆盖的节点数），并且至少覆盖 2 个节点 ——
            // 一个孤零零的半透明叶子不需要图层（逐元素乘 alpha 就是精确值），建层反而要多一次 flush。
            std::unordered_map<size_t, std::pair<int, int>> contig;
            for (const auto& kv : ownerSpan) {
                const int first = kv.second.first, last = kv.second.second;
                const int cnt = ownerCount[kv.first];
                if (cnt >= 2 && last - first + 1 == cnt) contig[kv.first] = kv.second;
            }
            ownerSpan.swap(contig);
        }
    }
    // 一个节点该不该在"进层/出层"的边界上（按位置索引）

    for (size_t oi = 0; oi < order.size(); oi++) {
        const size_t i = order[oi];
        const auto& node = nodes[i];
        // 层边界先处理（放在所有 continue 之前，保证顺序不依赖本节点画不画）
        while (!layerStack.empty() && static_cast<int>(oi) > layerStack.back().lastPos) {
            gfx.popLayer();
            layerAccum = (layerStack.back().op > 0.0001f) ? layerAccum / layerStack.back().op : 1.0f;
            layerStack.pop_back();
        }
        for (const auto& kv : ownerSpan) {
            if (kv.second.first != static_cast<int>(oi)) continue;
            // 最外层先 push：按祖先链深度排序（**从节点自己**开始，owner 自己的位置也要建层）
            std::vector<size_t> chain;
            {
                int32_t a = static_cast<int32_t>(i);
                for (int g = 0; a >= 0 && g < 512; g++) {
                    if (ownerSpan.count(static_cast<size_t>(a))) chain.push_back(static_cast<size_t>(a));
                    a = pIdx[static_cast<size_t>(a)];
                }
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const size_t oi2 = *it;
                if (layerStack.size() && layerStack.back().idx == oi2) continue;
                const auto& on = nodes[oi2];
                const auto s2 = renderer.drawState(on.id);
                const float bx = on.x - offX[oi2] + s2.offX;
                const float by = on.y - offY[oi2] + s2.offY;
                if (gfx.pushLayer(bx, by, on.width, on.height, ownOp[oi2])) {
                    layerStack.push_back({ oi2, ownerSpan[oi2].second, ownOp[oi2] });
                    layerAccum *= ownOp[oi2];
                }
            }
        }
        if (!(node.flags & BINUI_VISIBLE) || hidden[i]) continue;
        if (node.type == BINUI_ROOT) continue;

        // 分段计时的 RAII：节点里有 continue，靠析构统一归账
        struct ProfScope {
            DrawProf* p;
            int type;
            double t0;
            ProfScope(DrawProf* pp, int t) : p(pp), type(t & 31), t0(pp ? DrawProfNowUs() : 0) {}
            ~ProfScope() {
                if (!p) return;
                p->byType[type].us += DrawProfNowUs() - t0;
                p->byType[type].n++;
                p->nodes++;
            }
        } profScope(prof ? &pr : nullptr, static_cast<int>(node.type));

        // 几何基准（v0.11.7）：**有动画时保留亚像素**，静态节点仍走原来的整型截断。
        // 为什么必须分开：以前这里是 `int x = (int)node.x - (int)offX; ... x += (int)dx;`，
        // 动画平移/缩放全被截断到整像素。`animshow` 的 `bob` 是 16px/1.6s ≈ 0.08px/帧，
        // 截断后就是"每 12 帧跳 1px"；`slideX` 200px/1.4s 也会有 1~2px 的台阶 ——
        // 这就是"像阶梯、不丝滑"的直接原因（帧率再高也没用，精度先被扔掉了）。
        // 静态路径保持整型截断是为了像素门禁：一像素都不许变。
        // BINUI_LINE：x,y = 起点；width,height = 终点；radius = 线宽
        if (node.type == BINUI_LINE) {
            float x1 = node.x - offX[i], y1 = node.y - offY[i];
            float x2 = node.width - offX[i], y2 = node.height - offY[i];
            const BinUIRenderer::DrawState st = renderer.drawState(node.id);
            uint32_t dc = st.color;
            float op = st.opacity;
            // 进了图层的祖先 alpha 由层在 pop 时整体乘，这里除回来（否则乘两次）
            if (layerAccum > 0.0001f && layerAccum < 0.999f) {
                op /= layerAccum;
                if (op > 1.0f) op = 1.0f;
            }
            float ddx = st.offX, ddy = st.offY;
            x1 += ddx; y1 += ddy; x2 += ddx; y2 += ddy;
            if (op < 1.0f) dc = GfxScaleAlpha(dc, op);
            float mnx = std::min(x1, x2), mny = std::min(y1, y2);
            float mxx = std::max(x1, x2), mxy = std::max(y1, y2);
            // 线宽扩展：水平/垂直线某维跨度为 0，若按零面积 clip 会在 flush 时被
            // 批次的 scissor 检查（r<=l || bot<=t）整批丢弃——必须外扩线宽保证非零面积
            float lw = (float)node.radius;
            mnx -= lw; mny -= lw; mxx += lw; mxy += lw;
            if (mxx <= clipL || mnx >= clipR || mxy <= clipT || mny >= clipB) continue;
            if (hasVp[i] && (mxx <= vpL[i] || mnx >= vpR[i] || mxy <= vpT[i] || mny >= vpB[i])) continue;
            int savedDepth = gfx.pushClip(mnx, mny, mxx - mnx, mxy - mny);
            gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                         static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
            bool draw = true;
            renderer.clippingAncestors(node.id, clipAncs);
            for (uint16_t ancId : clipAncs) {
                const size_t ai = indexOf(ancId);
                if (ai == static_cast<size_t>(-1)) continue;
                const auto& par = nodes[ai];
                if (!(par.flags & BINUI_VISIBLE)) { draw = false; break; }
                if (dbgClip)
                std::fprintf(stderr, "        anc node=%u type=%u clip=(%.1f,%.1f %.1fx%.1f)\n",
                             (unsigned)par.id, (unsigned)par.type, (double)par.x,
                             (double)par.y, (double)par.width, (double)par.height);
            gfx.pushClip(par.x - offX[ai], par.y - offY[ai], par.width, par.height);
            }
            if (draw) gfx.fillLine(x1, y1, x2, y2, (float)node.radius, dc);
            gfx.popClipTo(savedDepth);
            continue;
        }

        // v7 动画：颜色（含 hover/按下 tint 与过渡）、透明度、平移、缩放、圆角
        // v20：一次取齐（以前这里散着 7 次查询 = 每节点 ~17 次哈希探测）
        const BinUIRenderer::DrawState st = renderer.drawState(node.id);
        uint32_t dc = st.color;
        // v13 扩展绘制段：文字色 / 边框 / 渐变 / 字重 / 行高 / 文字装饰
        const BinUIPaintNode* pt = st.paint;
        const bool hasBorder = pt && (pt->flags & PAINT_BORDER);
        const bool hasGrad = pt && (pt->flags & PAINT_GRADIENT);
        float opacity = st.opacity;
        // 进了图层的祖先 alpha 由层在 pop 时整体乘，这里除回来（否则乘两次）
        if (layerAccum > 0.0001f && layerAccum < 0.999f) {
            opacity /= layerAccum;
            if (opacity > 1.0f) opacity = 1.0f;
        }
        // v14 禁用：整块压到 55%（颜色/文字/图片一起淡，不引入额外合成通道）
        if (st.disabled) opacity *= 0.55f;
        float dx = st.offX;
        float dy = st.offY;
        float scale = st.scale;
        float animRadius = st.radius;
        if (opacity < 1.0f) dc = GfxScaleAlpha(dc, opacity);
        // 几何：有平移/缩放时用浮点（亚像素），否则沿用整型截断（像素中性）
        const bool animGeo = (dx != 0.0f || dy != 0.0f || scale != 1.0f);
        // 文字：静态节点按整像素落位（最锐利），有动画时保留小数 + LINEAR 采样。
        // 不这样的话，文字每 2 帧跳 1px —— 填充/描边都在连续滑，只有字在跳，很显眼。
        gfx.setTextSubpixel(animGeo);
        // v20：旋转 / 非等比缩放走**仿射**路径（旋转的矩形不是轴对齐的，画不了九宫格直块）。
        // 仿射只作用在"底/边框"上：文字与图片不参与（字形要对齐像素，旋转只会糊）。
        const bool affine = st.needsAffine();
        float x, y, nw, nh;                 // 轴对齐几何（仿射时 = 变换后的 AABB）
        float xf[6] = {1, 0, 0, 0, 1, 0};   // 局部 → 屏幕：[a c e; b d f]
        float lw = 0.0f, lh = 0.0f, lr = 0.0f;   // 仿射路径下的局部尺寸/圆角
        const float effRadius0 = animRadius >= 0 ? animRadius : static_cast<float>(node.radius);
        if (affine) {
            lw = static_cast<float>(node.width);
            lh = static_cast<float>(node.height);
            lr = effRadius0;   // 圆角跟着矩阵一起拉伸 = CSS 的语义，不再单独乘缩放
            const float bx = static_cast<float>(static_cast<int>(node.x) - static_cast<int>(offX[i]));
            const float by = static_cast<float>(static_cast<int>(node.y) - static_cast<int>(offY[i]));
            const float cx = bx + lw * 0.5f + dx;
            const float cy = by + lh * 0.5f + dy;
            const float rad = st.rotateDeg * 3.14159265358979323846f / 180.0f;
            const float cs = std::cos(rad), sn = std::sin(rad);
            const float kx = st.scaleX, ky = st.scaleY;
            const float a = cs * kx, b = sn * kx, c = -sn * ky, d = cs * ky;
            xf[0] = a; xf[1] = c; xf[2] = cx - (a * lw * 0.5f + c * lh * 0.5f);
            xf[3] = b; xf[4] = d; xf[5] = cy - (b * lw * 0.5f + d * lh * 0.5f);
            // 变换后四角的 AABB（给剔除、裁剪、文字定位用）
            float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
            const float lx[4] = {0.0f, lw, 0.0f, lw};
            const float ly[4] = {0.0f, 0.0f, lh, lh};
            for (int v = 0; v < 4; v++) {
                const float sx = xf[0] * lx[v] + xf[1] * ly[v] + xf[2];
                const float sy = xf[3] * lx[v] + xf[4] * ly[v] + xf[5];
                mnx = std::min(mnx, sx); mxx = std::max(mxx, sx);
                mny = std::min(mny, sy); mxy = std::max(mxy, sy);
            }
            x = mnx; y = mny; nw = mxx - mnx; nh = mxy - mny;
        } else if (animGeo) {
            nw = static_cast<float>(node.width) * scale;
            nh = static_cast<float>(node.height) * scale;
            // 绕矩形中心缩放，再叠加平移（与旧整型算法的顺序一致，只是不截断）
            const float cx = node.x - offX[i] + static_cast<float>(node.width) * 0.5f + dx;
            const float cy = node.y - offY[i] + static_cast<float>(node.height) * 0.5f + dy;
            x = cx - nw * 0.5f;
            y = cy - nh * 0.5f;
        } else {
            x = static_cast<float>(static_cast<int>(node.x) - static_cast<int>(offX[i]));
            y = static_cast<float>(static_cast<int>(node.y) - static_cast<int>(offY[i]));
            nw = static_cast<float>(static_cast<int>(node.width));
            nh = static_cast<float>(static_cast<int>(node.height));
        }
        // 圆角：**浮点**。缩放动画里半径要和 w/h 一起连续变化 ——
        // 40px 盒子放大 1.45× 时半径也必须乘 1.45（CSS 的 transform: scale 就是这样），
        // 而且圆/胶囊（radius = 短边一半）在缩放时如果半径取整，2r 与 w 会周期性错开
        // 1px，九宫格中间那几块在 0/1/2px 之间跳，圆看起来"一胀一缩"（VK/D3D11 上最明显）。
        // 后端的角片模板仍按最近的整数半径取（模板是位图，只能按整数建）。
        float effRadius = effRadius0;
        if (!affine && animGeo && scale != 1.0f) effRadius *= scale;

        // v23 阴影（box-shadow）：阴影露在盒子**外面**，所以剔除与裁剪都得把外扩算进去。
        // 外扩 = |spread| + 3σ + 1（与 BuildShadowRings 裁断的距离一致），再叠上偏移。
        int shN = 0;
        float shScale = 1.0f;                  // 等比缩放动画下阴影整体跟着缩放
        const BinUIShadow* shs = renderer.shadowsOf(node.id, shN);
        if (animGeo && scale > 0.0f) shScale = scale;
        float shPadX = 0.0f, shPadY = 0.0f;
        for (int k = 0; k < shN; k++) {
            const float sig = shs[k].blur > 0.0f ? shs[k].blur * 0.5f : 0.0f;
            const float pad = (std::fabs(shs[k].spread) + sig * 3.0f + 1.0f) * shScale;
            shPadX = std::max(shPadX, pad + std::fabs(shs[k].offsetX) * shScale);
            shPadY = std::max(shPadY, pad + std::fabs(shs[k].offsetY) * shScale);
        }

        if (x - shPadX + nw <= clipL || x + shPadX >= clipR ||
            y - shPadY + nh <= clipT || y + shPadY >= clipB) continue;
        if (hasVp[i] && (x - shPadX + nw <= vpL[i] || x + shPadX >= vpR[i] ||
                         y - shPadY + nh <= vpT[i] || y + shPadY >= vpB[i])) continue;

        // 裁剪链：自身 + 祖先 + 本次重绘范围（scissor 求交）。
        // 关键：持久离屏上节点重绘若不做脏矩形裁剪，会把整个节点矩形重涂，
        // 覆盖掉脏区以外（未重绘）的子元素/兄弟元素 → 悬停时内部元素消失。
        // 节点自身的裁剪矩形：亚像素几何要外扩到"覆盖到的像素格"。
        // 后端把浮点 scissor 截断（GL/D3D11 是 (int)，VK 是 floor/ceil），x=549.2 宽 22
        // 的盒子右边界在 571.2，被截成 571 就吃掉 0.2px —— 边界随动画每帧变，肉眼又是台阶。
        // 静态路径原样传（像素中性）。
        float cl = x, ct = y, cw = nw, ch = nh;
        if (animGeo || affine) {
            const float fx0 = std::floor(x), fy0 = std::floor(y);
            cl = fx0; ct = fy0;
            cw = std::ceil(x + nw) - fx0;
            ch = std::ceil(y + nh) - fy0;
        }
        if (dbgClip) {
            std::fprintf(stderr,
                         "[clip] node=%u type=%u own=(%.1f,%.1f %.1fx%.1f) dx=%.1f dy=%.1f"
                         " r=%.1f k=%.2fx%.2f rot=%.1f aff=%d\n",
                         (unsigned)node.id, (unsigned)node.type, (double)x, (double)y,
                         (double)nw, (double)nh, (double)dx, (double)dy, (double)effRadius,
                         (double)st.scaleX, (double)st.scaleY, (double)st.rotateDeg,
                         affine ? 1 : 0);
        }
        // 祖先裁剪容器是否可见（不复位 gfx 状态：后面阴影那一趟还要再压一遍同样的矩形）
        bool draw = true;
        renderer.clippingAncestors(node.id, clipAncs);
        for (uint16_t ancId : clipAncs) {
            const size_t ai = indexOf(ancId);
            if (ai == static_cast<size_t>(-1)) continue;
            if (!(nodes[ai].flags & BINUI_VISIBLE)) { draw = false; break; }
        }

        // ---- v23 box-shadow：先画阴影，再画自身底色 ----
        // 阴影属于元素自己的背景层（CSS：box-shadow 画在 background 之下），所以在
        // "进节点自身裁剪之前"单独开一段裁剪 —— 自身盒子会把阴影整块切掉。
        // 祖先裁剪（overflow 容器 / 滚动视口）照旧，阴影同样被滚动视口裁掉。
        if (draw && shN > 0 && opacity > 0.002f) {
            const float ex = x - shPadX, ey = y - shPadY;
            int sd = gfx.pushClip(ex, ey, nw + 2.0f * shPadX, nh + 2.0f * shPadY);
            gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                         static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
            for (uint16_t ancId : clipAncs) {
                const size_t ai = indexOf(ancId);
                if (ai == static_cast<size_t>(-1)) continue;
                const auto& par = nodes[ai];
                gfx.pushClip(par.x - offX[ai], par.y - offY[ai], par.width, par.height);
            }
            binui::PaintShadowRing rings[binui::kMaxShadowRings];
            for (int k = 0; k < shN; k++) {
                binui::BinUIShadow s2 = shs[k];
                // 缩放动画：阴影（偏移/模糊/展开）跟着整体缩放，与 CSS transform 语义一致
                if (shScale != 1.0f) {
                    s2.offsetX *= shScale; s2.offsetY *= shScale;
                    s2.blur *= shScale;    s2.spread *= shScale;
                }
                if (affine) {
                    // 旋转/斜切：环画在**局部空间**里，再整体过仿射矩阵（整块阴影跟着转）
                    const int rn = binui::BuildShadowRings(s2, 0.0f, 0.0f, lw, lh, lr, opacity,
                                                           rings);
                    for (int q = 0; q < rn; q++) {
                        const binui::PaintShadowRing& rr = rings[q];
                        if (rr.w <= 0.0f || rr.h <= 0.0f) continue;
                        if (((rr.color >> 24) & 0xFFu) == 0) continue;
                        // 环的局部原点不在 (0,0)：把平移量折进仿射矩阵的 e/f
                        const float ox = rr.x, oy = rr.y;
                        const float xf2[6] = {
                            xf[0], xf[1], xf[0] * ox + xf[1] * oy + xf[2],
                            xf[3], xf[4], xf[3] * ox + xf[4] * oy + xf[5],
                        };
                        gfx.fillRoundedXf(rr.w, rr.h, rr.radius, false, 0.0f, rr.color, xf2);
                    }
                } else {
                    const int rn = binui::BuildShadowRings(s2, x, y, nw, nh, effRadius, opacity,
                                                           rings);
                    for (int q = 0; q < rn; q++) {
                        const binui::PaintShadowRing& rr = rings[q];
                        if (rr.w <= 0.0f || rr.h <= 0.0f) continue;
                        if (((rr.color >> 24) & 0xFFu) == 0) continue;   // 最外圈可能已淡到 0
                        if (rr.radius > 0.5f)
                            gfx.fillRounded(rr.x, rr.y, rr.w, rr.h, rr.radius, rr.color);
                        else
                            gfx.fillRect(rr.x, rr.y, rr.w, rr.h, rr.color);
                    }
                }
            }
            gfx.popClipTo(sd);
        }

        int savedDepth = gfx.pushClip(cl, ct, cw, ch);
        gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                     static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
        // 裁剪链交给渲染器判定（clippingAncestors）：只有 overflow:auto/scroll/hidden
        // 的祖先才裁子元素。普通 div（含组件自己的根 div）按 CSS 语义不裁。
        for (uint16_t ancId : clipAncs) {
            const size_t ai = indexOf(ancId);
            if (ai == static_cast<size_t>(-1)) continue;
            const auto& par = nodes[ai];
            // 裁剪矩形 = 容器在屏幕上的矩形（减掉它祖先的滚动偏移）
            gfx.pushClip(par.x - offX[ai], par.y - offY[ai], par.width, par.height);
        }

        if (draw) {
            uint32_t cc = dc;
            uint32_t textCol = GfxScaleAlpha(0xFFFFFFFF, opacity);
            uint32_t accentCol = GfxScaleAlpha(0xFFBB86FC, opacity);
            int pad = node.padding;
            int fs = node.fontSize ? node.fontSize : 14;
            // v14：斜体 / 占位符（同一份 textFlags 在三端语义一致）
            const bool italic = pt && (pt->textFlags & TEXT_ITALIC) != 0;
            const bool placeholder = pt && (pt->textFlags & TEXT_PLACEHOLDER) != 0;

            // ---- v13 扩展绘制：底色/边框的统一入口（几何来自 binui_paint.h） ----
            // GL/D3D11 有 alpha 混合，条带直接画即可，不需要像 GDI 那样自己先混合底色
            // 透明度对**渐变底色与边框**同样生效（v0.12.8）：
            // 以前只有单色底、文字、图片被 opacity 缩放，渐变底和边框直接拿编译期的
            // 颜色画 —— 于是宿主淡入淡出一块渐变卡片/描边圆环时，底和边纹丝不动。
            // 这里在进条带构建前把 alpha 折进去；不透明时 ptScaled == *pt，逐位等价。
            BinUIPaintNode ptScaled;
            const BinUIPaintNode* ptGrad = pt;
            const BinUIPaintNode* ptBord = pt;
            if (pt && opacity < 1.0f) {
                bool need = hasGrad || hasBorder;
                if (need) {
                    ptScaled = *pt;
                    ptScaled.gradFrom = GfxScaleAlpha(pt->gradFrom, opacity);
                    ptScaled.gradTo = GfxScaleAlpha(pt->gradTo, opacity);
                    for (int e = 0; e < 4; e++)
                        ptScaled.borderColor[e] = GfxScaleAlpha(pt->borderColor[e], opacity);
                    ptGrad = &ptScaled;
                    ptBord = &ptScaled;
                }
            }
            auto fillNodeBg = [&](uint32_t color, float radius) {
                if (affine) {
                    // 仿射路径：整块底一次性交给后端（渐变条带在旋转下会被撕开，
                    // 这里退回单色底；文字/图片本来就不参与仿射）
                    gfx.fillRoundedXf(lw, lh, lr, false, 0.0f, color, xf);
                    return;
                }
                binui::PaintBand bands[binui::kMaxPaintBands];
                binui::PaintBand* bp = bands;
                int bcap = binui::kMaxPaintBands;
                const std::vector<binui::PaintBand>* cached = nullptr;
                // 任意角度渐变（v0.16.13）：条带是千级，走"按节点缓存 + 堆缓冲"。
                // 缓存键覆盖了影响条带的全部输入，所以坐标/颜色一变就自动重算。
                if (ptGrad && (ptGrad->flags & binui::PAINT_GRADIENT) && ptGrad->reserve[1] != 0) {
                    const uint32_t sig = binui::BgBandsSig(static_cast<float>(x), static_cast<float>(y),
                                                           static_cast<float>(nw), static_cast<float>(nh),
                                                           radius, ptGrad->gradFrom, ptGrad->gradTo,
                                                           ptGrad->reserve[1] - 1);
                    cached = binui::BgBandsGet(node.id, sig, ptGrad, static_cast<float>(x),
                                               static_cast<float>(y), static_cast<float>(nw),
                                               static_cast<float>(nh), radius);
                    if (cached && !cached->empty()) { bp = nullptr; bcap = 0; }
                }
                int bn = 0;
                if (cached && !cached->empty()) {
                    bn = static_cast<int>(cached->size());
                    for (int k = 0; k < bn; k++) {
                        const binui::PaintBand& b = (*cached)[static_cast<size_t>(k)];
                        if (b.w <= 0.0f || b.h <= 0.0f) continue;
                        gfx.fillRect(b.x, b.y, b.w, b.h, b.color);
                    }
                    return;
                }
                bn = binui::BuildFillBands(ptGrad, static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(nw), static_cast<float>(nh),
                                           radius, color, bp, bcap);
                for (int k = 0; k < bn; k++) {
                    const binui::PaintBand& b = bp[k];
                    if (b.w <= 0.0f || b.h <= 0.0f) continue;
                    if (b.round && b.radius > 0.0f)
                        gfx.fillRounded(b.x, b.y, b.w, b.h, b.radius, b.color);
                    else
                        gfx.fillRect(b.x, b.y, b.w, b.h, b.color);
                }
            };
            auto drawNodeBorder = [&]() {
                if (!hasBorder) return;
                if (affine) {
                    // 旋转/形变下的边框：走仿射圆环（四边等宽单色；不等宽的边框在旋转下
                    // 需要四条独立仿射带，暂不支持，这里按最宽的那条画）
                    uint8_t maxW = 1;
                    for (int e = 0; e < 4; e++) maxW = std::max(maxW, pt->borderW[e]);
                    gfx.fillRoundedXf(lw, lh, lr, true, static_cast<float>(maxW),
                                      ptBord->borderColor[0], xf);
                    return;
                }
                // 等宽 1px 边框 + 圆角：走九宫格圆环（有解析式 AA），不用逐行矩形拼。
                // 逐行拼的条带在 45° 处每行 1.4~2px 宽、且只有亚像素混色没有 AA，
                // 圆角看起来比直边"更粗更亮"，和旁边 AA 过的填充不搭。
                const uint8_t* bw = pt->borderW;
                const bool uniform1px = bw[0] == 1 && bw[1] == 1 && bw[2] == 1 && bw[3] == 1 &&
                                        ptBord->borderColor[0] == ptBord->borderColor[1] &&
                                        ptBord->borderColor[0] == ptBord->borderColor[2] &&
                                        ptBord->borderColor[0] == ptBord->borderColor[3];
                if (uniform1px && effRadius > 0.5f) {
                    // 圆角走九宫格圆环（解析式 AA）：弧线是渐变而不是 1px 一格的台阶，
                    // 直边段模板覆盖率已修成满值，颜色和作者给的一致。
                    gfx.strokeRounded(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(nw), static_cast<float>(nh),
                                      static_cast<float>(effRadius), 1.0f, ptBord->borderColor[0]);
                    return;
                }
                binui::PaintBand bb[binui::kMaxBorderBands];
                const int bn = binui::BuildBorderBands(ptBord, static_cast<float>(x), static_cast<float>(y),
                                                       static_cast<float>(nw), static_cast<float>(nh),
                                                       static_cast<float>(effRadius), bb);
                for (int k = 0; k < bn; k++)
                    if (bb[k].w > 0.0f && bb[k].h > 0.0f)
                        gfx.fillRect(bb[k].x, bb[k].y, bb[k].w, bb[k].h, bb[k].color);
            };
            // 文字装饰线（下划线/删除线/上划线）：一条细矩形，位置/粗细由 binui_format.h
            // 统一给（三端一致）；超出节点的部分由压在上面的 scissor 裁掉
            auto drawTextRules = [&](float left, float top, float w) {
                binui::PaintBand rules[3];
                const int rn = binui::BuildTextRuleBands(pt, left, top, w,
                                                         static_cast<float>(fs), rules);
                for (int k = 0; k < rn; k++)
                    if (rules[k].w > 0.0f)
                        gfx.fillRect(rules[k].x, rules[k].y, rules[k].w, rules[k].h, rules[k].color);
            };

            switch (node.type) {
                case BINUI_LABEL: {
                    // 文字色 / 字重（v13）：作者显式写了 color / font-weight 就用它，
                    // 否则沿用节点自身颜色与常规字重（旧行为不变）
                    const bool paintText = pt && (pt->flags & PAINT_TEXT_COLOR);
                    const uint32_t lcol = paintText ? GfxScaleAlpha(pt->textColor, opacity) : cc;
                    const int wt = binui::PaintWeightOf(pt);
                    // LABEL 现在也可以有底色：只在有渐变、或显式给了不透明底色时铺底。
                    // 纯文字标签满屏都是，透明底再画一层只是白烧一个四边形。
                    if (hasGrad || (paintText && ((node.color >> 24) & 0xFF)))
                        fillNodeBg(dc, static_cast<float>(node.radius));
                    if (node.text.empty()) break;

                    int asc = 0, lh = 0;
                    gfx.fontMetrics(static_cast<float>(fs), wt, asc, lh, italic);
                    const float availW = static_cast<float>(nw - 2 * pad);
                    const int align = (node.flags & BINUI_TEXT_CENTER) ? 1
                                    : (node.flags & BINUI_TEXT_RIGHT) ? 2 : 0;
                    // 装饰线要落在文字上：逐行跟着实际画文字的位置走
                    const bool hasRule = pt && (pt->flags & (PAINT_UNDERLINE | PAINT_STRIKE | PAINT_OVERLINE));
                    if (node.text.find('\n') != std::string::npos) {
                        // 多行文本（<br> / '\n'）：整块垂直居中，逐行换行绘制（按 align 对齐）
                        size_t lines = 1;
                        for (char c : node.text) if (c == '\n') lines++;
                        // 行距：CSS line-height 优先（v13），未声明时用字体行高（旧行为）。
                        // drawTextWrapped 内部按字体行高推进，跨 '\n' 的段间距只能在这里补
                        const float lstep = (pt && (pt->flags & PAINT_LINE_HEIGHT))
                                          ? binui::PaintLineHeight(pt, static_cast<float>(fs))
                                          : static_cast<float>(lh);
                        float blockH = static_cast<float>(lines) * lstep;
                        float top = static_cast<float>(y) + (nh - blockH) / 2.0f;
                        if (top < static_cast<float>(y)) top = static_cast<float>(y);
                        const float lineLeft = static_cast<float>(x + pad);
                        float ly = top;
                        size_t p0 = 0;
                        while (p0 <= node.text.size()) {
                            const size_t nl = node.text.find('\n', p0);
                            const std::string seg = node.text.substr(
                                p0, (nl == std::string::npos) ? std::string::npos : nl - p0);
                            gfx.drawTextWrapped(lineLeft, ly, availW > 1.0f ? availW : 1.0f,
                                                static_cast<float>(fs), wt, seg, lcol, align, italic);
                            // 装饰线逐行画：只画一条的话，多行文本就只有第一行有下划线
                            if (hasRule && !seg.empty()) {
                                const float tw = gfx.textWidth(static_cast<float>(fs), wt, seg, italic);
                                float rx = lineLeft;
                                if (align == 1) rx += (availW - tw) * 0.5f;
                                else if (align == 2) rx += availW - tw;
                                drawTextRules(rx, ly, tw);
                            }
                            if (nl == std::string::npos) break;
                            ly += lstep;
                            p0 = nl + 1;
                        }
                    } else {
                        // 量一次宽度：既要判"放不放得下"，对齐/装饰线也用它
                        // （字宽走字形图集的 run 缓存，稳定后只是一次查表）
                        const float tw = gfx.textWidth(static_cast<float>(fs), wt, node.text, italic);
                        // 自然排版（v21）：一行放不下就按盒子宽度折行。节点高度由布局按
                        // 同一份估算撑开（BinUIRenderer::autoTextHeight），这里是它的绘制侧：
                        // 行数估算 → 块高 → 垂直居中 → drawTextWrapped 逐行画。
                        if (tw > availW + 0.5f && availW > 1.0f) {
                            const int nlines = binui::EstimateLineCount(
                                node.text, static_cast<float>(fs), availW);
                            const float lstep = (pt && (pt->flags & PAINT_LINE_HEIGHT))
                                              ? binui::PaintLineHeight(pt, static_cast<float>(fs))
                                              : static_cast<float>(lh);
                            const float blockH = static_cast<float>(nlines) * lstep;
                            float top = static_cast<float>(y) + (nh - blockH) * 0.5f;
                            if (top < static_cast<float>(y)) top = static_cast<float>(y);
                            gfx.drawTextWrapped(static_cast<float>(x + pad), top, availW,
                                                static_cast<float>(fs), wt, node.text, lcol,
                                                align, italic);
                            // 装饰线只画在首行（折行文本的下划线按块近似）
                            if (hasRule)
                                drawTextRules(static_cast<float>(x + pad), top,
                                              std::min(tw, availW));
                            break;
                        }
                        float base = static_cast<float>(y) + (nh - lh) / 2.0f + asc;
                        float tx = static_cast<float>(x + pad);
                        if (align != 0) {
                            tx += (align == 1) ? (availW - tw) / 2.0f : (availW - tw);
                            if (tx < static_cast<float>(x + pad)) tx = static_cast<float>(x + pad);
                        }
                        gfx.drawText(tx, base, static_cast<float>(fs), wt, node.text, lcol,
                                     false, false, italic);
                        if (hasRule)
                            drawTextRules(tx, static_cast<float>(y) + (nh - lh) / 2.0f, tw);
                    }
                    break;
                }
                case BINUI_BUTTON: {
                    fillNodeBg(dc, static_cast<float>(effRadius));
                    if (!node.text.empty()) {
                        // 文字色：v13 显式文字色优先，否则保持白字（旧行为）
                        const uint32_t tcol = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                                  ? GfxScaleAlpha(pt->textColor, opacity) : textCol;
                        int bs = node.fontSize ? node.fontSize : 14;
                        // 字重：作者声明了 font-weight 就按声明（与 LABEL 同一套
                        // PaintWeightOf，归一化到 400/700）；没声明才回落 700 ——
                        // 按钮的旧观感是粗体，不能让老布局突然变细。
                        // textWidth / fontMetrics / drawText 三处必须同一个值。
                        const int wt = (pt && (pt->flags & PAINT_WEIGHT))
                                     ? binui::PaintWeightOf(pt) : 700;
                        const float fs = static_cast<float>(bs);
                        float tw = gfx.textWidth(fs, wt, node.text, italic);
                        int asc = 0, lh = 0;
                        gfx.fontMetrics(fs, wt, asc, lh, italic);
                        // 水平对齐：与上面 BINUI_LABEL 分支完全同一套算法
                        //（都不置位 = 左对齐 / CENTER / RIGHT）。以前写死居中，
                        // text-align:left 与 padding-left 写了都没效果。
                        const float boxW = static_cast<float>(nw - 2 * pad);
                        const float availW = boxW > 1.0f ? boxW : 1.0f;
                        const int align = (node.flags & BINUI_TEXT_CENTER) ? 1
                                        : (node.flags & BINUI_TEXT_RIGHT) ? 2 : 0;
                        float tx = static_cast<float>(x + pad);
                        if (align != 0) {
                            tx += (align == 1) ? (availW - tw) / 2.0f : (availW - tw);
                            // 文字比盒子宽时别越过左内边距（与 LABEL 一致）
                            if (tx < static_cast<float>(x + pad)) tx = static_cast<float>(x + pad);
                        }
                        float base = y + (nh - lh) / 2.0f + asc;
                        gfx.drawText(tx, base, fs, wt, node.text, tcol, false, false, italic);
                    }
                    break;
                }
                case BINUI_INPUT: {
                    fillNodeBg(dc, static_cast<float>(node.radius));
                    // 输入框的强调色：作者给了 textColor 就用它，否则回退内置紫
                    //（以前写死 #BB86FC，浅色主题下没法跟着配色走）
                    const uint32_t inAccent = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                                  ? GfxScaleAlpha(pt->textColor, opacity) : accentCol;
                    // 底部强调线是"控件可供性"，不是正文：压到 35% 免得在浅底上成一道黑杠。
                    // 作者给了 border 就不再画 —— 那条线是直角 Material 下划线的遗留，
                    // 横穿圆角输入框时会在框底多出一道悬空横杠（v13 有边框后已多余）
                    if (!hasBorder) {
                        gfx.fillRect(static_cast<float>(x), static_cast<float>(y + nh - 2),
                                     static_cast<float>(nw), 2, GfxScaleAlpha(inAccent, 0.35f));
                    }

                    bool editing = renderer.isFocused(node.id);
                    std::string text = node.text;
                    size_t caret = 0, selStart = 0;
                    if (editing) {
                        text = renderer.edit().value;
                        caret = renderer.edit().caret;
                        selStart = renderer.edit().selStart;
                    }

                    // 文字基线/行高在"有没有文本"之外也要算：空输入框同样要画光标
                    int asc = 0, lh = 0;
                    gfx.fontMetrics(static_cast<float>(fs), 400, asc, lh, italic);
                    if (lh <= 0) lh = fs + 2;
                    const float base = static_cast<float>(y) + (nh - lh) / 2.0f + asc;
                    const float tx = static_cast<float>(x + pad + 4);
                    // 光标/选区的竖直范围 = **文字行高**，不是整条节点高度。
                    // 以前用的是 nh-4：36px 高的输入框里光标有 32px，比 13px 的字高出一倍，
                    // 看着像根柱子。
                    const float lineTop = static_cast<float>(y) + (nh - lh) / 2.0f;
                    const float lineH = static_cast<float>(lh);

                    if (!text.empty()) {
                        // 占位符按 45% 色画（不是真实内容，视觉上要和已输入文本区分开）
                        const uint32_t inCol = (placeholder && !editing)
                                                   ? GfxScaleAlpha(inAccent, 0.55f) : inAccent;
                        gfx.drawText(tx, base, static_cast<float>(fs), 400, text, inCol,
                                     false, false, italic);
                    }

                    if (editing) {
                        size_t a = std::min(caret, selStart);
                        size_t b = std::max(caret, selStart);
                        if (a != b) {
                            float wa = gfx.textWidth(static_cast<float>(fs), 400, text.substr(0, a));
                            float wb = gfx.textWidth(static_cast<float>(fs), 400, text.substr(0, b));
                            gfx.fillRect(tx + wa, lineTop, wb - wa, lineH,
                                         (inAccent & 0x00FFFFFFu) | 0x66000000u);
                        }
                        if (GfxCaretBlinkOn()) {
                            float wc = gfx.textWidth(static_cast<float>(fs), 400, text.substr(0, caret));
                            // 光标画满行高（上端贴字顶、下端贴字底），1.5px 宽
                            gfx.fillRect(tx + wc, lineTop, 1.5f, lineH, inAccent);
                        }
                    }
                    break;
                }
                case BINUI_LIST: {
                    fillNodeBg(dc, static_cast<float>(effRadius));
                    if (!node.text.empty()) {
                        // 文字色 / 字重（v13）：显式写法优先；否则按自身底色亮度选深浅
                        //（以前一律白字，浅底容器里的文字直接看不见）
                        const uint32_t tcol = (pt && (pt->flags & PAINT_TEXT_COLOR))
                                                  ? GfxScaleAlpha(pt->textColor, opacity)
                                                  : GfxScaleAlpha(binui::PaintContrastText(dc), opacity);
                        const int wt = binui::PaintWeightOf(pt);
                        const int align = (node.flags & BINUI_TEXT_CENTER) ? 1
                                        : (node.flags & BINUI_TEXT_RIGHT) ? 2 : 0;
                        gfx.drawTextWrapped(static_cast<float>(x + pad + 12),
                                            static_cast<float>(y + pad + 8),
                                            static_cast<float>(nw - 2 * pad - 24),
                                            static_cast<float>(fs), wt, node.text, tcol, align, italic);
                    }
                    break;
                }
                case BINUI_IMAGE: {
                    const ImageResource* img = renderer.getImage(node.imageIndex);
                    bool drawn = false;
                    if (img && !img->data.empty()) {
                        uint32_t gen = renderer.imageGeneration(node.imageIndex);
                        if (img->rawPixels) {
                            if (gfx.prepareImageRaw(node.imageIndex, img->width, img->height,
                                                    img->data.data(), gen))
                                gfx.drawImage(node.imageIndex, static_cast<float>(x),
                                              static_cast<float>(y),
                                              static_cast<float>(nw), static_cast<float>(nh));
                            drawn = true;
                        } else if (gfx.prepareImage(node.imageIndex, img->data.data(),
                                                    img->data.size(), gen)) {
                            gfx.drawImage(node.imageIndex, static_cast<float>(x),
                                          static_cast<float>(y),
                                          static_cast<float>(nw), static_cast<float>(nh));
                            drawn = true;
                        }
                    }
                    if (!drawn) {
                        fillNodeBg(dc, static_cast<float>(effRadius));
                        if (!node.text.empty()) {
                            int asc = 0, lh = 0;
                            gfx.fontMetrics(24.0f, 300, asc, lh);
                            gfx.drawText(x + nw / 2.0f, y + nh / 2.0f - lh / 2.0f + asc,
                                         24.0f, 300, node.text, textCol, true, false);
                        }
                    }
                    break;
                }
                case BINUI_VIDEO: {
                    // 底色：无解码帧时显示（深色海报底）
                    fillNodeBg(dc, static_cast<float>(effRadius));

                    // 解码帧（伪图片槽，rawPixels）：contain-fit letterbox
                    const ImageResource* img = renderer.getImage(node.imageIndex);
                    if (img && img->rawPixels && !img->data.empty() &&
                        img->width > 0 && img->height > 0) {
                        uint32_t gen = renderer.imageGeneration(node.imageIndex);
                        if (gfx.prepareImageRaw(node.imageIndex, img->width, img->height,
                                                img->data.data(), gen)) {
                            float ar = static_cast<float>(img->width) / img->height;
                            float boxAr = static_cast<float>(nw) / nh;
                            float fw, fh;
                            if (ar > boxAr) { fw = static_cast<float>(nw); fh = fw / ar; }
                            else            { fh = static_cast<float>(nh); fw = fh * ar; }
                            gfx.drawImage(node.imageIndex,
                                          x + (nw - fw) * 0.5f, y + (nh - fh) * 0.5f, fw, fh);
                        }
                    }

                    // 浏览器式控制条（暂停常显/播放中悬停显示）
                    if (renderer.videoControlsVisible(node.id))
                        DrawVideoControlsTmpl(gfx, renderer, node,
                                              static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(nw), opacity);
                    break;
                }
                case BINUI_SVG:
                case BINUI_CANVAS: {
                    // 容器矩形：有底色则绘制（svg/canvas 默认透明）
                    fillNodeBg(dc, static_cast<float>(effRadius));
                    if (node.type == BINUI_CANVAS && canvas)
                        canvas(gfx, node.id, static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(nw), static_cast<float>(nh));
                    break;
                }
                case BINUI_PATH: {
                    // v15 SVG 路径：填充 + 描边，几何全在 binui_paint.h（三端共用）
                    const auto* pr = renderer.pathOf(node.id);
                    if (!pr || pr->first.empty()) break;
                    static thread_local binui::PathRenderBuffers pbuf;
                    binui::PathPaintSpec pspec;
                    pspec.fillColor = GfxScaleAlpha(pr->head.fillColor, opacity);
                    pspec.strokeColor = GfxScaleAlpha(pr->head.strokeColor, opacity);
                    pspec.fillGrad = pr->hasGrad ? &pr->grad : nullptr;
                    pspec.strokeGrad = pr->hasStrokeGrad ? &pr->strokeGrad : nullptr;
                    pspec.opacity = opacity;
                    binui::PaintPathNode(
                        pr->head, pr->pts.data(), pr->first.data(), pr->count.data(),
                        pr->closed.data(), static_cast<int>(pr->first.size()),
                        static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(nw), static_cast<float>(nh), pspec,
                        renderer.pathBandCache(node.id), renderer.pathCacheGen(),
                        renderer.pathBandsBudget(), pbuf,
                        [&](const binui::PaintBand* b, int n) {
                            for (int k = 0; k < n; k++)
                                gfx.fillRect(b[k].x, b[k].y, b[k].w, b[k].h, b[k].color);
                        },
                        [&](float x0, float y0, float x1, float y1, float lw, uint32_t lc) {
                            gfx.fillLine(x0, y0, x1, y1, lw, lc);
                        },
                        [&](float rx, float ry, float rw, float rh, float rr, uint32_t dc) {
                            gfx.fillRounded(rx, ry, rw, rh, rr, dc);
                        });
                    // 缓存记账：这一节点这帧实际占了多少条带（全局预算 pathBandsBudget 用）
                    renderer.commitPathBands(node.id);
                    break;
                }
                default:
                    break;
            }

            // 边框（v13）：画在节点内侧，四边可不同宽，压在底色之上
            drawNodeBorder();

            // 焦点环三态：输入元素待编辑 = 虚线 + 对比色；按钮等普通焦点 = 实线 + 对比色
            // （两者都 2 秒无操作自动消失）；编辑中 = 实线加粗 + 强调蓝（常亮）。
            // 线型 + 颜色双编码，见 docs/keyboard_focus.md。
            {
                const BinUIRenderer::FocusRing ring = renderer.focusRingOf(node.id);
                if (ring != BinUIRenderer::FocusRing::None) {
                    // 焦点环必须跟随节点自己的圆角：以前写死半径 10、内缩 2，
                    // 圆角 8 的按钮上就会套出一个半径不同的环，看着像"双线框"。
                    const bool editing = ring == BinUIRenderer::FocusRing::Editing;
                    const bool dashed = ring == BinUIRenderer::FocusRing::Dashed;
                    const float thick = editing ? 2.0f : 1.0f;
                    uint32_t ringCol = textCol;
                    if (editing) {
                        ringCol = GfxScaleAlpha(BinUIRenderer::kFocusRingEditColor, opacity);
                    } else if (((dc >> 24) & 0xFFu) >= 0x80u) {
                        // 对比色：写死白色在浅色主题上（白底白环）等于没画
                        const float bgLum = (0.2126f * static_cast<float>((dc >> 16) & 0xFFu) +
                                             0.7152f * static_cast<float>((dc >> 8) & 0xFFu) +
                                             0.0722f * static_cast<float>(dc & 0xFFu)) / 255.0f;
                        if (bgLum > 0.7f) ringCol = GfxScaleAlpha(0xFF1A1A1Au, opacity);
                    }
                    const float fr = effRadius > 2 ? static_cast<float>(effRadius) - 1.0f : 0.0f;
                    const float fx = static_cast<float>(x) + 1.0f, fy = static_cast<float>(y) + 1.0f;
                    const float fw = static_cast<float>(nw) - 2.0f, fh = static_cast<float>(nh) - 2.0f;
                    if (dashed) {
                        // 虚线环：沿圆角路径取线段（几何在 binui_paint.h，三端共用）
                        PaintSegment segs[kMaxFocusDashSegments];
                        const int n = BuildDashedRoundRing(fx, fy, fw, fh, fr, 6.0f, 4.0f,
                                                           segs, kMaxFocusDashSegments);
                        for (int si = 0; si < n; si++)
                            gfx.fillLine(segs[si].x0, segs[si].y0, segs[si].x1, segs[si].y1,
                                         1.0f, ringCol);
                    } else if (fr >= 1.0f) {
                        gfx.strokeRounded(fx, fy, fw, fh, fr, thick, ringCol);
                    } else {
                        gfx.fillLine(fx, fy, fx + fw, fy, thick, ringCol);
                        gfx.fillLine(fx, fy + fh, fx + fw, fy + fh, thick, ringCol);
                        gfx.fillLine(fx, fy, fx, fy + fh, thick, ringCol);
                        gfx.fillLine(fx + fw, fy, fx + fw, fy + fh, thick, ringCol);
                    }
                }
            }
        }

        gfx.popClipTo(savedDepth);
    }
    // 收尾：图层必须成对弹出（正常不会剩下，防御性兜底）
    while (!layerStack.empty()) {
        gfx.popLayer();
        layerStack.pop_back();
    }

    // 滚动条（轨道 + 滑块）：同样裁剪到本次重绘范围，避免滚动条整条重涂
    int sbDepth = gfx.pushClip(static_cast<float>(clipL), static_cast<float>(clipT),
                               static_cast<float>(clipR - clipL), static_cast<float>(clipB - clipT));
    for (const auto& node : nodes) {
        if (!(node.flags & BINUI_SCROLLABLE) || !(node.flags & BINUI_VISIBLE)) continue;
        // overflow:hidden = "只裁剪"容器：可以编程滚动（scrollTo），但不出滚动条、
        // 不响应滚轮。用户滚不动的东西就别给它画一条让人以为能拖的滑块。
        if (!renderer.isUserScrollable(node.id)) continue;
        float ch = renderer.contentHeight(node.id);
        float cw = renderer.contentWidth(node.id);
        auto sit = scrolls.find(node.id);
        float sx = sit != scrolls.end() ? sit->second.x : 0;
        float sy = sit != scrolls.end() ? sit->second.y : 0;
        int x = static_cast<int>(node.x), y = static_cast<int>(node.y);
        int nw = static_cast<int>(node.width), nh = static_cast<int>(node.height);
        const bool needV = ch > nh + 0.5f;
        const bool needH = cw > nw + 0.5f;
        if (!needV && !needH) continue;

        // 滚动条配色/宽度由渲染器提供（C ABI 的 binui_set_scrollbar_style 落到同一处）。
        // 以前这里写死 8px + #2E2E2E / #5A5A5A，浅色主题下就是右边一条黑杠，
        // 而且没有任何入口能覆盖。
        const auto& sb = renderer.scrollbarStyle();
        const int barW = sb.width;
        const float thumbThick = static_cast<float>(std::max(2, barW / 2));

        if (needV) {
            gfx.fillRect(static_cast<float>(x + nw - barW), static_cast<float>(y),
                         static_cast<float>(barW), static_cast<float>(nh), sb.track);
            float thumbH = std::max(24.0f, static_cast<float>(nh) * nh / ch);
            float maxScroll = ch - nh;
            // 位置算法（贴右、按滚动比例）保持原样；只有宽度跟着轨道换算
            float thumbY = maxScroll > 0 ? sy / maxScroll * (nh - thumbH) : 0;
            // 滑块宽度沿用"轨道一半"的老比例（轨道 8 → 滑块 4），左右各留 (barW-thumbW)/2
            const float thumbX = static_cast<float>(x + nw - barW) + (barW - thumbThick) * 0.5f;
            gfx.fillRect(thumbX, static_cast<float>(y) + thumbY, thumbThick, thumbH, sb.thumb);
        }
        if (needH) {
            // 水平条：贴在底边，宽度扣掉竖直条占的那一格，避免两条在右下角打架
            const int trackW = nw - (needV ? barW : 0);
            gfx.fillRect(static_cast<float>(x), static_cast<float>(y + nh - barW),
                         static_cast<float>(trackW), static_cast<float>(barW), sb.track);
            float thumbW = std::max(24.0f, static_cast<float>(nw) * nw / cw);
            float maxScroll = cw - nw;
            float thumbX = maxScroll > 0 ? sx / maxScroll * (nw - thumbW) : 0;
            const float thumbY = static_cast<float>(y + nh - barW) + (barW - thumbThick) * 0.5f;
            gfx.fillRect(static_cast<float>(x) + thumbX, thumbY, thumbW, thumbThick, sb.thumb);
        }
    }
    gfx.popClipTo(sbDepth);

    if (prof) {
        pr.frameUs += DrawProfNowUs() - tFrame0;
        if (pr.frames >= 60) DrawProfFlush();
    }
}

} // namespace binui

#endif // BINUI_DRAW_NODES_H


