#include "binui_renderer.h"
#include <cstring>
#include <cmath>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <array>

namespace binui {

namespace {

// ---------- UTF-8 工具 ----------
bool Utf8IsCont(uint8_t c) { return (c & 0xC0) == 0x80; }

size_t Utf8CharLen(uint8_t lead) {
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// 把偏移量钳制到字符边界（向前回退连续字节）
size_t ClampToCharBoundary(const std::string& s, size_t off) {
    while (off > 0 && off < s.size() && Utf8IsCont(static_cast<uint8_t>(s[off]))) --off;
    return off;
}

// 按 UTF-8 字节计数移动偏移量
size_t MoveByChars(const std::string& s, size_t off, int count) {
    while (count > 0) {
        if (off >= s.size()) break;
        off += Utf8CharLen(static_cast<uint8_t>(s[off]));
        --count;
    }
    while (count < 0) {
        if (off == 0) break;
        size_t prev = off - 1;
        while (prev > 0 && Utf8IsCont(static_cast<uint8_t>(s[prev]))) --prev;
        off = prev;
        ++count;
    }
    return off;
}

// 前 n 个 UTF-16 代码单元对应的字节偏移
size_t Utf16IndexToBytes(const std::string& s, size_t i16) {
    size_t bytes = 0, count = 0;
    while (bytes < s.size() && count < i16) {
        bytes += Utf8CharLen(static_cast<uint8_t>(s[bytes]));
        ++count;
    }
    return bytes;
}

// 按 min/max 尺寸约束钳制（% 相对父内容盒 base；-1 = 未指定）
float ClampSize(float v, const BinUISpecNode* s, float base,
                uint16_t mnFlag, uint16_t mxFlag, float mn, float mx) {
    if (mn >= 0) { float m = (s->flags & mnFlag) ? mn * base : mn; if (v < m) v = m; }
    if (mx >= 0) { float m = (s->flags & mxFlag) ? mx * base : mx; if (v > m) v = m; }
    return v;
}

// ============================================================
// v7 动画引擎辅助
// ============================================================
double AnimNow() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// 贝塞尔曲线 x(t) 求 y：二分/牛顿解 t 使 x(t)=p
float CubicBezierY(float p, float x1, float y1, float x2, float y2) {
    float t = p;
    for (int i = 0; i < 12; i++) {
        float cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
        float xt = ((ax * t + bx) * t + cx) * t;
        float err = xt - p;
        if (std::fabs(err) < 1e-5f) break;
        float dx = (3 * ax * t + 2 * bx) * t + cx;
        if (std::fabs(dx) < 1e-6f) break;
        t -= err / dx;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    float cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    return ((ay * t + by) * t + cy) * t;
}

float EaseTime(uint8_t easing, float t, const float* pts) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (easing) {
        case BINUI_EASE_LINEAR: return t;
        case BINUI_EASE_IN:     return t * t;
        case BINUI_EASE_OUT:    return 1 - (1 - t) * (1 - t);
        case BINUI_EASE_IN_OUT: return t < 0.5f ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2.0f;
        case BINUI_EASE_CUBIC:
            return CubicBezierY(t, pts ? pts[0] : .4f, pts ? pts[1] : 0,
                                pts ? pts[2] : .2f, pts ? pts[3] : 1);
        default: // ease（cubic-bezier(.25,.1,.25,1)）
            return CubicBezierY(t, .25f, .1f, .25f, 1.0f);
    }
}

// 颜色按通道插值（AARRGGBB）；注意用浮点减法避免 uint32 下溢（y<x 时）
uint32_t LerpColor(uint32_t a, uint32_t b, float t) {
    uint32_t aa = (a >> 24) & 0xFF, ba = (b >> 24) & 0xFF;
    uint32_t ar = (a >> 16) & 0xFF, br = (b >> 16) & 0xFF;
    uint32_t ag = (a >> 8) & 0xFF,  bg = (b >> 8) & 0xFF;
    uint32_t ab = a & 0xFF,         bb = b & 0xFF;
    auto l = [&](uint32_t x, uint32_t y) {
        return (uint32_t)((float)x + ((float)y - (float)x) * t);
    };
    return (l(aa, ba) << 24) | (l(ar, br) << 16) | (l(ag, bg) << 8) | l(ab, bb);
}

// 悬停/按下亮暗（与后端 TintColor 语义一致）
uint32_t TintNodeColor(uint32_t c, float t) {
    uint32_t a = c & 0xFF000000;
    float r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    if (t >= 0) {
        r += (255 - r) * t;
        g += (255 - g) * t;
        b += (255 - b) * t;
    } else {
        float k = 1.0f + t;
        r *= k; g *= k; b *= k;
    }
    return a | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8)
           | static_cast<uint32_t>(b);
}

} // namespace

// 进度条按下区域的时间换算（x → 秒；命中测试/拖动 scrub 共用）
static double VideoBarTimeAt(const BinUIRenderer::NodeInfo& n,
                             const BinUIRenderer::VideoBarGeo& g, float x,
                             const BinUIRenderer::VideoState& v);

BinUIRenderer::BinUIRenderer() {}

BinUIRenderer::~BinUIRenderer() {
    shutdown();
}

void BinUIRenderer::shutdown() {
    nodes_.clear();
    nodeIndexById_.clear();
    images_.clear();
    fonts_.clear();
    edit_ = EditState{};
    hoverId_ = -1;
    pressedId_ = -1;
    keyboardId_ = -1;
    scroll_.clear();
    scrollDragNode_ = -1;
    specOf_.clear();
    flexOf_.clear();
    specData_.clear();
    hasSpecs_ = false;
    keyGroups_.clear();
    animDefs_.clear();
    transitions_.clear();
    animState_.clear();
    animResult_.clear();
    animChanged_.clear();
    animTimeInit_ = false;
}

bool BinUIRenderer::loadLayout(const uint8_t* data, size_t size) {
    if (size < sizeof(BinUIHeader)) {
        std::cerr << "[binui] Invalid binUI data: too small" << std::endl;
        return false;
    }

    const auto* header = reinterpret_cast<const BinUIHeader*>(data);
    if (header->magic != BINUI_MAGIC && header->magic != BINUI_HTML_MAGIC) {
        std::cerr << "[binui] Invalid binUI magic: 0x" << std::hex << header->magic << std::dec << std::endl;
        return false;
    }
    if (header->version != BINUI_VERSION) {
        std::cerr << "[binui] Unsupported binUI version: " << header->version << std::endl;
        return false;
    }

    const size_t expectedSize = sizeof(BinUIHeader)
                              + header->nodeCount * sizeof(BinUINode)
                              + header->stringSize;
    if (size < expectedSize) {
        std::cerr << "[binui] binUI data truncated" << std::endl;
        return false;
    }

    // 留存布局字节：mountComponent() 在它上面做组件合并
    layoutBytes_.assign(data, data + size);

    nodes_.clear();
    nodeIndexById_.clear();
    mountedAnchors_.clear();
    specOf_.clear();
    flexOf_.clear();
    specData_.clear();
    hasSpecs_ = false;
    videos_.clear();
    videoIdxByNode_.clear();
    vidClock_.clear();
    vidPressNode_ = -1;
    vidPressRegion_ = 0;
    vidScrubbing_ = false;
    vidAutoplayDone_ = false;

    const auto* srcNodes = reinterpret_cast<const BinUINode*>(data + sizeof(BinUIHeader));
    const char* stringTable = reinterpret_cast<const char*>(srcNodes + header->nodeCount);

    for (uint32_t i = 0; i < header->nodeCount; i++) {
        const auto& src = srcNodes[i];
        NodeInfo node;
        node.id        = src.id;
        node.parentId  = src.parentId;
        node.type      = src.type;
        node.flags     = src.flags;
        node.x         = src.x;
        node.y         = src.y;
        node.width     = src.width;
        node.height    = src.height;
        node.color     = src.color;
        node.imageIndex = src.imageIndex;
        node.fontSize  = src.fontSize;
        node.padding   = src.padding;
        node.radius    = src.radius;
        node.zIndex    = src.zIndex;

        if (src.textOffset < header->stringSize) {
            node.text = stringTable + src.textOffset;
        }

        nodes_.push_back(node);
        nodeIndexById_[node.id] = i;
    }

    // ---- 缩放规格段（可选；v3 编译器输出，手写/默认布局可缺省） ----
    specOf_.assign(nodes_.size(), nullptr);
    flexOf_.clear();
    specData_.clear();
    hasSpecs_ = false;
    const uint8_t* sp = reinterpret_cast<const uint8_t*>(stringTable) + header->stringSize;
    size_t rem = size - (sp - data);
    uint32_t specCount = 0, flexCount = 0;
    const uint8_t* specEnd = sp; // 缩放规格段结束处（窗口描述段起点）
    if (rem >= sizeof(uint32_t)) {
        memcpy(&specCount, sp, sizeof(uint32_t));
        sp += sizeof(uint32_t);
        rem -= sizeof(uint32_t);
        if (specCount <= nodes_.size() && rem >= (size_t)specCount * sizeof(BinUISpecNode)) {
            const uint8_t* fp = sp + (size_t)specCount * sizeof(BinUISpecNode);
            size_t remFlex = rem - (size_t)specCount * sizeof(BinUISpecNode);
            if (remFlex >= sizeof(uint32_t)) {
                memcpy(&flexCount, fp, sizeof(uint32_t));
                if (flexCount > remFlex / sizeof(BinUISpecFlex)) flexCount = 0; // 防御
            }
            specEnd = fp + sizeof(uint32_t) + (size_t)flexCount * sizeof(BinUISpecFlex);
            // 先按完整容量分配，避免 resize 重定位使指针失效
            specData_.resize((size_t)specCount * sizeof(BinUISpecNode)
                             + (size_t)flexCount * sizeof(BinUISpecFlex));
            memcpy(specData_.data(), sp, (size_t)specCount * sizeof(BinUISpecNode));
            if (flexCount) {
                memcpy(specData_.data() + (size_t)specCount * sizeof(BinUISpecNode),
                       fp + sizeof(uint32_t), (size_t)flexCount * sizeof(BinUISpecFlex));
            }
            for (uint32_t i = 0; i < specCount; i++) {
                const auto& e = *reinterpret_cast<const BinUISpecNode*>(
                    specData_.data() + (size_t)i * sizeof(BinUISpecNode));
                auto it = nodeIndexById_.find(e.nodeId);
                if (it != nodeIndexById_.end()) specOf_[it->second] = &e;
            }
            for (uint32_t i = 0; i < flexCount; i++) {
                const auto& f = *reinterpret_cast<const BinUISpecFlex*>(
                    specData_.data() + (size_t)specCount * sizeof(BinUISpecNode)
                    + (size_t)i * sizeof(BinUISpecFlex));
                flexOf_[f.nodeId] = &f;
            }
            hasSpecs_ = true;
        }
    }

    // ---- 窗口描述段（v5，位于缩放规格段之后；无则为 0） ----
    winDesc_ = WindowDesc{};
    {
        const uint8_t* wend = specEnd;
        size_t wrem = size - (wend - data);
        if (wrem >= sizeof(uint32_t)) {
            uint32_t wbytes = 0;
            memcpy(&wbytes, wend, sizeof(uint32_t));
            if (wbytes >= sizeof(BinUIWindowDesc) && wrem >= sizeof(uint32_t) + wbytes) {
                const auto* wd = reinterpret_cast<const BinUIWindowDesc*>(wend + sizeof(uint32_t));
                winDesc_.present = true;
                winDesc_.width = wd->width;
                winDesc_.height = wd->height;
                winDesc_.flags = wd->flags;
                winDesc_.aspect = wd->aspect;
                winDesc_.bgColor = wd->bgColor;
                size_t tl = wd->titleLen;
                if (tl <= wbytes - sizeof(BinUIWindowDesc)) {
                    const char* ts = reinterpret_cast<const char*>(wd) + sizeof(BinUIWindowDesc);
                    winDesc_.title.assign(ts, tl);
                }
            }
        }
    }

    // ---- 动画段（v7，位于窗口描述段之后；animBytes=0 或缺失 = 无动画） ----
    keyGroups_.clear();
    animDefs_.clear();
    transitions_.clear();
    animState_.clear();
    animResult_.clear();
    animTimeInit_ = false;
    {
        const uint8_t* ap = specEnd;
        if (winDesc_.present) {
            uint32_t wb = 0;
            memcpy(&wb, specEnd, sizeof(uint32_t));
            ap = specEnd + sizeof(uint32_t) + wb;
        }
        size_t arem = size - (ap - data);
        if (arem >= sizeof(uint32_t)) {
            uint32_t abytes = 0;
            memcpy(&abytes, ap, sizeof(uint32_t));
            if (abytes > 0 && arem >= sizeof(uint32_t) + abytes) {
                const uint8_t* aend = ap + sizeof(uint32_t) + abytes;
                ap += sizeof(uint32_t);
                uint32_t gc = 0;
                memcpy(&gc, ap, sizeof(uint32_t)); ap += sizeof(uint32_t);
                keyGroups_.reserve(gc);
                for (uint32_t g = 0; g < gc; g++) {
                    if (ap + (int)sizeof(uint32_t) > aend) break;
                    uint32_t kc = 0;
                    memcpy(&kc, ap, sizeof(uint32_t)); ap += sizeof(uint32_t);
                    KeyGroup kg;
                    if (kc && ap + (size_t)kc * sizeof(BinUIKeyFrame) <= aend) {
                        kg.kfs.assign(reinterpret_cast<const BinUIKeyFrame*>(ap),
                                      reinterpret_cast<const BinUIKeyFrame*>(ap) + kc);
                        ap += (size_t)kc * sizeof(BinUIKeyFrame);
                    } else if (kc) { break; }
                    std::sort(kg.kfs.begin(), kg.kfs.end(),
                              [](const BinUIKeyFrame& a, const BinUIKeyFrame& b) { return a.percent < b.percent; });
                    keyGroups_.push_back(std::move(kg));
                }
                if (ap + (int)sizeof(uint32_t) <= aend) {
                    uint32_t ac = 0;
                    memcpy(&ac, ap, sizeof(uint32_t)); ap += sizeof(uint32_t);
                    if (ac && ap + (size_t)ac * sizeof(BinUIAnimDef) <= aend) {
                        animDefs_.assign(reinterpret_cast<const BinUIAnimDef*>(ap),
                                         reinterpret_cast<const BinUIAnimDef*>(ap) + ac);
                        ap += (size_t)ac * sizeof(BinUIAnimDef);
                    }
                }
                if (ap + (int)sizeof(uint32_t) <= aend) {
                    uint32_t tc = 0;
                    memcpy(&tc, ap, sizeof(uint32_t)); ap += sizeof(uint32_t);
                    if (tc && ap + (size_t)tc * sizeof(BinUITransition) <= aend) {
                        transitions_.assign(reinterpret_cast<const BinUITransition*>(ap),
                                            reinterpret_cast<const BinUITransition*>(ap) + tc);
                    }
                }
            }
        }
    }

    // ---- 视频描述段（v9，恒定写出，位于动画段之后；vidBytes=0 = 无视频） ----
    {
        const uint8_t* dp = specEnd;
        if (winDesc_.present) {
            uint32_t wb = 0;
            memcpy(&wb, dp, sizeof(uint32_t));
            dp += sizeof(uint32_t) + wb;
        }
        size_t drem = size - (dp - data);
        if (drem >= sizeof(uint32_t)) { // 跳过动画段
            uint32_t ab = 0;
            memcpy(&ab, dp, sizeof(uint32_t));
            if (drem >= sizeof(uint32_t) + ab) dp += sizeof(uint32_t) + ab;
        }
        drem = size - (dp - data);
        if (drem >= sizeof(uint32_t)) {
            uint32_t vb = 0;
            memcpy(&vb, dp, sizeof(uint32_t));
            if (vb >= sizeof(uint32_t) && drem >= sizeof(uint32_t) + vb) {
                const uint8_t* q = dp + sizeof(uint32_t);
                const uint8_t* qend = q + vb;
                uint32_t vc = 0;
                memcpy(&vc, q, sizeof(uint32_t));
                q += sizeof(uint32_t);
                for (uint32_t i = 0; i < vc && q + sizeof(BinUIVideoDesc) <= qend; i++) {
                    BinUIVideoDesc d;
                    memcpy(&d, q, sizeof(d));
                    q += sizeof(d);
                    if (!nodeIndexById_.count(d.nodeId)) continue; // 悬空描述防御
                    VideoState vs;
                    vs.nodeId = d.nodeId;
                    vs.imageIndex = d.imageIndex;
                    vs.flags = d.flags;
                    vs.volume = d.volume;
                    if (d.srcOffset < header->stringSize)
                        vs.src = stringTable + d.srcOffset;
                    videoIdxByNode_[vs.nodeId] = videos_.size();
                    videos_.push_back(std::move(vs));
                }
            }
        }
    }

    // ---- 状态样式段（v11，恒定写出，位于视频段之后；stateBytes=0 = 无状态样式） ----
    stateCSS_.clear();
    checkedNodes_.clear();
    {
        const uint8_t* dp = specEnd;
        if (winDesc_.present) {
            uint32_t wb = 0;
            memcpy(&wb, dp, sizeof(uint32_t));
            dp += sizeof(uint32_t) + wb;
        }
        size_t drem = size - (dp - data);
        if (drem >= sizeof(uint32_t)) { // 跳过动画段
            uint32_t ab = 0;
            memcpy(&ab, dp, sizeof(uint32_t));
            if (drem >= sizeof(uint32_t) + ab) dp += sizeof(uint32_t) + ab;
        }
        drem = size - (dp - data);
        if (drem >= sizeof(uint32_t)) { // 跳过视频段
            uint32_t vb = 0;
            memcpy(&vb, dp, sizeof(uint32_t));
            if (drem >= sizeof(uint32_t) + vb) dp += sizeof(uint32_t) + vb;
        }
        drem = size - (dp - data);
        if (drem >= sizeof(uint32_t)) {
            uint32_t sb = 0;
            memcpy(&sb, dp, sizeof(uint32_t));
            if (sb >= sizeof(uint32_t) && drem >= sizeof(uint32_t) + sb) {
                const uint8_t* q = dp + sizeof(uint32_t);
                const uint8_t* qend = q + sb;
                uint32_t sc = 0;
                memcpy(&sc, q, sizeof(uint32_t));
                q += sizeof(uint32_t);
                for (uint32_t i = 0; i < sc && q + sizeof(BinUIStateStyle) <= qend; i++) {
                    BinUIStateStyle s;
                    memcpy(&s, q, sizeof(s));
                    q += sizeof(s);
                    const int idx = (s.state == BINUI_ST_HOVER)   ? 0
                                  : (s.state == BINUI_ST_ACTIVE)  ? 1
                                  : (s.state == BINUI_ST_FOCUS)   ? 2
                                  : (s.state == BINUI_ST_CHECKED) ? 3 : -1;
                    if (idx < 0 || !nodeIndexById_.count(s.nodeId)) continue; // 悬空条目防御
                    StateStyle& t = stateCSS_[s.nodeId][idx];
                    t.fields = s.fields;
                    if (s.fields & BINUI_STF_COLOR)  t.color = s.color;
                    if (s.fields & BINUI_STF_SCALE)  t.scale = s.scale;
                    if (s.fields & BINUI_STF_RADIUS) t.radius = s.radius;
                }
            }
        }
    }

    // ---- 名称段（v8，位于资源段/组件引用段之后；缺失 = 布局无命名节点） ----
    // 段序（v12）：窗口描述 / 动画 / 视频 / 状态样式 / 资源 / 组件引用 / 名称，
    // 除名称段外都恒定写出，所以按游标顺序跳段即可。
    auto sectionAt = [&](int skip) -> const uint8_t* {
        const uint8_t* q = specEnd;
        size_t rem = size - static_cast<size_t>(q - data);
        for (int k = 0; k < skip; k++) {
            if (rem < sizeof(uint32_t)) return nullptr;
            uint32_t n = 0;
            memcpy(&n, q, sizeof(n));
            if (rem < sizeof(uint32_t) + n) return nullptr;
            q += sizeof(uint32_t) + n;
            rem -= sizeof(uint32_t) + n;
        }
        return rem >= sizeof(uint32_t) ? q : nullptr;
    };

    // ---- 资源段（v12）：imageIndex -> 图片路径（布局自描述） ----
    imagePaths_.clear();
    if (const uint8_t* ip = sectionAt(4)) {
        uint32_t ib = 0;
        memcpy(&ib, ip, sizeof(ib));
        if (ib >= sizeof(uint32_t) && size - static_cast<size_t>(ip - data) >= sizeof(uint32_t) + ib) {
            const uint8_t* payload = ip + sizeof(uint32_t);   // 段数据起点
            uint32_t ic = 0;
            memcpy(&ic, payload, sizeof(ic));
            for (uint32_t i = 0; i < ic; i++) {
                const size_t at = sizeof(uint32_t) + (size_t)i * sizeof(uint32_t); // 含计数字段
                if (at + sizeof(uint32_t) > ib) break;
                uint32_t off = 0;
                memcpy(&off, payload + at, sizeof(off));
                imagePaths_.emplace_back(off < header->stringSize ? stringTable + off : "");
            }
        }
    }

    // ---- 组件引用段（v12）：<use ... external> 的锚点与组件路径 ----
    componentRefs_.clear();
    if (const uint8_t* cp = sectionAt(5)) {
        uint32_t cb = 0;
        memcpy(&cb, cp, sizeof(cb));
        if (cb >= sizeof(uint32_t) && size - static_cast<size_t>(cp - data) >= sizeof(uint32_t) + cb) {
            const uint8_t* q = cp + sizeof(uint32_t);
            uint32_t cc = 0;
            memcpy(&cc, q, sizeof(cc));
            q += sizeof(cc);
            const uint8_t* qend = cp + sizeof(uint32_t) + cb;
            for (uint32_t i = 0; i < cc && q + sizeof(BinUIComponentRef) <= qend; i++) {
                BinUIComponentRef ref;
                memcpy(&ref, q, sizeof(ref));
                q += sizeof(ref);
                ComponentRef cr;
                cr.anchorId = ref.anchorId;
                cr.src = ref.srcOffset < header->stringSize ? stringTable + ref.srcOffset : "";
                componentRefs_.push_back(std::move(cr));
            }
        }
    }

    // ---- 扩展绘制段（v13）：paintBytes + paintCount + BinUIPaintNode[] ----
    // 段序（v13）：窗口描述 / 动画 / 视频 / 状态样式 / 资源 / 组件引用 / 扩展绘制 / 名称，
    // 除名称段外都恒定写出，所以按游标顺序跳段即可。
    paints_.clear();
    paintIdx_.clear();
    clipOnly_.clear();
    if (const uint8_t* pp = sectionAt(6)) {
        uint32_t pb = 0;
        memcpy(&pb, pp, sizeof(pb));
        if (pb >= sizeof(uint32_t) && size - static_cast<size_t>(pp - data) >= sizeof(uint32_t) + pb) {
            const uint8_t* q = pp + sizeof(uint32_t);
            uint32_t pc = 0;
            memcpy(&pc, q, sizeof(pc));
            q += sizeof(pc);
            const uint8_t* qend = pp + sizeof(uint32_t) + pb;
            for (uint32_t i = 0; i < pc && q + sizeof(BinUIPaintNode) <= qend; i++) {
                BinUIPaintNode pn;
                memcpy(&pn, q, sizeof(pn));
                q += sizeof(pn);
                if (!nodeIndexById_.count(pn.nodeId)) continue;   // 悬空条目防御
                paintIdx_[pn.nodeId] = paints_.size();
                if (pn.flags & PAINT_CLIP_ONLY) clipOnly_.insert(pn.nodeId);
                paints_.push_back(pn);
            }
        }
    }

    nameToId_.clear();
    if (const uint8_t* np = sectionAt(7)) {
        uint32_t nbytes = 0;
        memcpy(&nbytes, np, sizeof(uint32_t));
        if (nbytes > 0 && size - static_cast<size_t>(np - data) >= sizeof(uint32_t) + nbytes) {
            const uint8_t* q = np + sizeof(uint32_t);
            const uint8_t* qend = q + nbytes;
            if (q + sizeof(uint32_t) <= qend) {
                uint32_t nc = 0;
                memcpy(&nc, q, sizeof(uint32_t));
                q += sizeof(uint32_t);
                for (uint32_t i = 0; i < nc && q + 8 <= qend; i++) {
                    uint32_t nodeId = 0, nameOff = 0;
                    memcpy(&nodeId, q, sizeof(uint32_t));
                    memcpy(&nameOff, q + sizeof(uint32_t), sizeof(uint32_t));
                    q += 8;
                    if (nameOff < header->stringSize && nodeId <= 0xFFFF)
                        nameToId_[std::string(stringTable + nameOff)] = (uint16_t)nodeId;
                }
            }
        }
    }

    // 初始化关键帧动画实例（基准 = 加载时刻 + 各自 delay）
    animT0_ = AnimNow();
    animNow_ = animT0_;
    animTimeInit_ = true;
    for (const auto& d : animDefs_)
        startKeyframeAnim(d.nodeId, d);
    stepAnimations(); // 预计算初始可视结果（首帧即从关键帧起点绘制）

    // ui_scale="proportional"：记录设计尺寸与坐标快照；relayout 改为等比缩放（不重排）
    proportionalScale_ = winDesc_.scaleProportional();
    if (proportionalScale_ && !nodes_.empty()) {
        designW_ = nodes_[0].width;
        designH_ = nodes_[0].height;
        designNodes_ = nodes_;
        scaleX_ = scaleY_ = 1.0f;
    }

    std::cout << "[binui] Loaded layout: " << header->nodeCount << " nodes"
              << ", " << header->stringSize << " bytes of strings"
              << (hasSpecs_ ? " (+resize spec)" : "")
              << (winDesc_.present ? " (+window desc)" : "")
              << (!animDefs_.empty() || !transitions_.empty() ? " (+anim)" : "")
              << (!videos_.empty() ? " (+video x" + std::to_string(videos_.size()) + ")" : "")
              << (imagePaths_.empty() ? std::string()
                                      : " (+imgs x" + std::to_string(imagePaths_.size()) + ")")
              << (componentRefs_.empty() ? std::string()
                                         : " (+mounts x" + std::to_string(componentRefs_.size()) + ")")
              << std::endl;
    return true;
}

// ============================================================
// 组件挂载（外挂 .bum）：编译期同款链接器，运行期现场合并
// ============================================================
bool BinUIRenderer::mountComponent(const uint8_t* data, size_t size, uint16_t anchorId) {
    if (!data || size == 0) {
        std::cerr << "[binui] mountComponent: 组件数据为空" << std::endl;
        return false;
    }
    if (layoutBytes_.empty()) {
        std::cerr << "[binui] mountComponent: 还没有布局（先 loadLayout）" << std::endl;
        return false;
    }
    std::vector<uint8_t> merged = layoutBytes_;   // 拷贝：loadLayout 会重写 layoutBytes_
    std::string err;
    if (!MergeLayout(merged, data, size, anchorId, nullptr, nullptr, &err)) {
        std::cerr << "[binui] mountComponent 失败：" << err << std::endl;
        return false;
    }
    if (!loadLayout(merged.data(), merged.size())) return false;
    mountedAnchors_.insert(anchorId);
    std::cout << "[binui] Mounted component under node " << anchorId << std::endl;
    return true;
}

void BinUIRenderer::loadResources(const std::unordered_map<std::string, std::vector<uint8_t>>& resources) {
    for (const auto& [path, data] : resources) {
        std::string ext;
        size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            ext = path.substr(dot);
            for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        }

        if (ext == ".ttf" || ext == ".otf") {
            FontResource font;
            font.data = data;
            fonts_[path] = std::move(font);
        } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp") {
            ImageResource img;
            img.data = data;
            images_[path] = std::move(img);
        }
    }

    std::cout << "[binui] Loaded " << images_.size() << " images, "
              << fonts_.size() << " fonts" << std::endl;
}

void BinUIRenderer::setText(uint16_t nodeId, const std::string& text) {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return;
    nodes_[it->second].text = text;
}

void BinUIRenderer::setVisible(uint16_t nodeId, bool visible) {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return;

    if (visible) {
        nodes_[it->second].flags |= BINUI_VISIBLE;
    } else {
        nodes_[it->second].flags &= ~BINUI_VISIBLE;
    }
}

uint16_t BinUIRenderer::findNodeByName(const std::string& name) const {
    auto it = nameToId_.find(name);
    return (it == nameToId_.end()) ? 0 : it->second;
}

uint16_t BinUIRenderer::addNode(uint16_t parentId, uint8_t type,
                                float x, float y, float w, float h,
                                uint32_t color, uint16_t radius, uint16_t fontSize) {
    if (!nextNodeId_) {  // 惰性初始化：从已加载节点分配后续 id
        for (const auto& n : nodes_)
            if ((uint32_t)n.id + 1 > nextNodeId_) nextNodeId_ = (uint16_t)(n.id + 1);
    }
    NodeInfo node;
    node.id = nextNodeId_++;
    node.parentId = parentId;
    node.type = type;
    node.flags = BINUI_VISIBLE | BINUI_ABSOLUTE;
    node.x = x; node.y = y; node.width = w; node.height = h;
    node.color = color;
    node.radius = radius;
    node.zIndex = 0;
    node.fontSize = fontSize;
    node.padding = 0;
    node.imageIndex = 0;
    nodes_.push_back(node);
    nodeIndexById_[node.id] = nodes_.size() - 1;
    if (specOf_.size() == nodes_.size() - 1) specOf_.push_back(nullptr);
    return node.id;
}

void BinUIRenderer::removeNode(uint16_t nodeId) {
    auto it = nodeIndexById_.find(nodeId);
    if (it != nodeIndexById_.end()) nodes_[it->second].flags &= ~BINUI_VISIBLE;
}

bool BinUIRenderer::isNodeVisible(uint16_t nodeId) const {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return false;

    // 沿祖先链向上：自身与所有祖先都必须可见
    size_t idx = it->second;
    while (true) {
        const auto& node = nodes_[idx];
        if (!(node.flags & BINUI_VISIBLE)) return false;
        if (node.parentId == 0xFFFF) return true;

        auto pit = nodeIndexById_.find(node.parentId);
        if (pit == nodeIndexById_.end()) return true; // 父缺失，按可见处理
        idx = pit->second;
    }
}

// 绘制顺序（v10）：z-index 稳定分层，返回节点下标，越靠后绘制越靠上。
//
// 按 CSS 的绘制模型**沿树遍历**：父先于子（容器的底色不能盖住自己的内容），
// 同一父节点下按 (z-index, 流式先于 absolute, DOM 顺序) 排。
// 早先的实现是对全部节点做一次全局排序 —— 那会让 absolute 容器排到它自己的
// 流式子节点之后，容器底色把子内容整片盖掉（普通元素 + 卡片的常见写法直接不可见）。
std::vector<uint32_t> BinUIRenderer::drawOrder() const {
    const size_t n = nodes_.size();
    std::vector<std::vector<uint32_t>> kids(n);
    std::vector<uint32_t> roots;
    roots.reserve(8);
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t pid = nodes_[i].parentId;
        auto it = (pid == 0xFFFF) ? nodeIndexById_.end() : nodeIndexById_.find(pid);
        if (it == nodeIndexById_.end() || it->second >= n || it->second == i)
            roots.push_back(i);            // 无父 / 悬空父 / 自指：当根处理
        else
            kids[it->second].push_back(i);
    }
    auto byLayer = [this](uint32_t a, uint32_t b) {
        const int za = nodes_[a].zIndex, zb = nodes_[b].zIndex;
        if (za != zb) return za < zb;
        const bool aa = (nodes_[a].flags & BINUI_ABSOLUTE) != 0;
        const bool ab = (nodes_[b].flags & BINUI_ABSOLUTE) != 0;
        if (aa != ab) return !aa;  // 同层：流式先于 absolute
        return a < b;              // 同层同型：DOM 顺序
    };
    for (auto& k : kids) if (k.size() > 1) std::stable_sort(k.begin(), k.end(), byLayer);
    if (roots.size() > 1) std::stable_sort(roots.begin(), roots.end(), byLayer);

    // 迭代式先序遍历（深树不爆栈）；seen 防御父链成环的畸形布局
    std::vector<uint32_t> order;
    order.reserve(n);
    std::vector<uint8_t> seen(n, 0);
    std::vector<uint32_t> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        if (i >= n || seen[i]) continue;
        seen[i] = 1;
        order.push_back(i);
        const auto& ch = kids[i];
        for (auto r = ch.rbegin(); r != ch.rend(); ++r) stack.push_back(*r);
    }
    // 成环导致漏掉的节点：按原下标补在后面（至少画出来，不至于凭空消失）
    for (uint32_t i = 0; i < n; i++)
        if (!seen[i]) order.push_back(i);
    return order;
}

void BinUIRenderer::setImageOrder(const std::vector<std::string>& paths) {
    imageOrder_ = paths;
    imageGen_.assign(imageOrder_.size(), 0);
}

const ImageResource* BinUIRenderer::getImage(uint16_t index) const {
    if (index >= imageOrder_.size()) return nullptr;
    auto it = images_.find(imageOrder_[index]);
    return (it == images_.end()) ? nullptr : &it->second;
}

bool BinUIRenderer::updateImage(uint16_t index, const uint8_t* data, size_t size) {
    if (index >= imageOrder_.size()) return false;
    ImageResource img;
    if (data && size > 0) img.data.assign(data, data + size);
    img.rawPixels = false;
    images_[imageOrder_[index]] = std::move(img);
    imageGen_[index]++;
    return true;
}

bool BinUIRenderer::updateImageRaw(uint16_t index, int width, int height, const uint8_t* rgba) {
    if (index >= imageOrder_.size() || width <= 0 || height <= 0 || !rgba) return false;
    ImageResource img;
    size_t n = static_cast<size_t>(width) * height * 4;
    img.data.assign(rgba, rgba + n);
    img.width = width;
    img.height = height;
    img.channels = 4;
    img.rawPixels = true;
    images_[imageOrder_[index]] = std::move(img);
    imageGen_[index]++;
    return true;
}

uint32_t BinUIRenderer::imageGeneration(uint16_t index) const {
    return index < imageGen_.size() ? imageGen_[index] : 0;
}

void BinUIRenderer::setSize(int width, int height) {
    relayout(width, height); // 有规格段时重排布局；无规格段仅同步根尺寸
    Event ev;
    ev.type = EventType::RESIZE;
    ev.nodeId = 0;
    ev.resize.width = width;
    ev.resize.height = height;
    if (eventCallback_) eventCallback_(ev);
}

// 运行时重排：与编译器布局算法一致的流式/百分比/flex 语义。
// 规格段缺省（静态布局）时仅把根节点尺寸同步为窗口尺寸。
void BinUIRenderer::relayout(int width, int height) {
    if (nodes_.empty()) return;

    // 等比 UI 缩放模式（ui_scale="proportional"）：整棵节点树随窗口缩放，
    // 坐标写回 nodes_（命中/绘制/滚动自动一致），不做流式/百分比/flex 重排。
    if (proportionalScale_) {
        scaleX_ = designW_ > 0 ? (float)width / designW_ : 1.0f;
        scaleY_ = designH_ > 0 ? (float)height / designH_ : 1.0f;
        float fs = scaleY_ > 0 ? scaleY_ : 1.0f;
        // 静态节点：按设计坐标快照等比缩放（坐标/尺寸/圆角/字号）
        for (size_t i = 0; i < designNodes_.size(); i++) {
            const NodeInfo& d = designNodes_[i];
            nodes_[i].x = d.x * scaleX_;
            nodes_[i].y = d.y * scaleY_;
            nodes_[i].width = d.width * scaleX_;
            nodes_[i].height = d.height * scaleY_;
            nodes_[i].radius = (uint16_t)(d.radius * fs);
            nodes_[i].fontSize = (uint16_t)std::max(1.0f, d.fontSize * fs);
        }
        // 运行时动态节点（addNode 追加在快照之后）：保持当前像素坐标（图表应用不启用缩放模式）
        nodes_[0].x = 0; nodes_[0].y = 0;
        nodes_[0].width = static_cast<float>(width);
        nodes_[0].height = static_cast<float>(height);
        // 滚动偏移随窗口等比缩放
        for (auto& kv : scroll_) {
            kv.second.x *= scaleX_;
            kv.second.y *= scaleY_;
            if (kv.second.x < 0) kv.second.x = 0;
            if (kv.second.y < 0) kv.second.y = 0;
        }
        return;
    }

    nodes_[0].x = 0;
    nodes_[0].y = 0;
    nodes_[0].width = static_cast<float>(width);
    nodes_[0].height = static_cast<float>(height);
    if (!hasSpecs_) return;

    // 容器上下文：内容盒原点/尺寸 + 独立流光标（与编译器 Ctx 一致）
    struct RCtx {
        uint16_t nodeId;
        size_t   nodeIdx;
        float    ox, oy;
        float    cw, ch;
        float    cursorY;
    };
    std::vector<RCtx> stack;
    float rootPad = nodes_[0].padding;
    stack.push_back({ 0, 0, rootPad, rootPad,
                      (float)width - 2 * rootPad, (float)height - 2 * rootPad, 0 });

    // 容器高度不足时扩展到容纳流式内容（滚动容器保持固定视口）
    auto growContainer = [&](RCtx& c) {
        NodeInfo& cn = nodes_[c.nodeIdx];
        if (cn.flags & BINUI_SCROLLABLE) return;
        float need = c.cursorY + cn.padding * 2;
        if (need > cn.height) cn.height = need;
    };

    // flex 子项布局结果：childId -> {x, y, w, h}（相对父内容盒）
    std::unordered_map<uint16_t, std::array<float, 4>> flexPos;

    for (size_t i = 1; i < nodes_.size(); i++) {
        NodeInfo& node = nodes_[i];
        const BinUISpecNode* spec = specOf_[i];

        while (stack.size() > 1 && stack.back().nodeId != node.parentId) {
            growContainer(stack.back());
            stack.pop_back();
        }
        RCtx& c = stack.back();

        // ---- 尺寸 ----
        float w, h;
        auto fp = flexPos.find(node.id);
        if (spec && (spec->flags & SPEC_IN_FLEX) && fp != flexPos.end()) {
            w = fp->second[2];
            h = fp->second[3];
        } else {
            if (spec && (spec->flags & SPEC_PCT_W)) w = spec->pctW * c.cw;
            else if (spec && (spec->flags & SPEC_AUTO_W)) w = c.cw - spec->ml - spec->mr;
            else if (spec && spec->relW >= 0) w = spec->relW;
            else w = node.width;
            if (spec && (spec->flags & SPEC_PCT_H)) h = spec->pctH * c.ch;
            else if (spec && spec->relH >= 0) h = spec->relH;
            else h = node.height;
            if (spec) {
                w = ClampSize(w, spec, c.cw, SPEC_MINW_PCT, SPEC_MAXW_PCT, spec->minW, spec->maxW);
                h = ClampSize(h, spec, c.ch, SPEC_MINH_PCT, SPEC_MAXH_PCT, spec->minH, spec->maxH);
            }
        }

        // ---- 位置 ----
        float x, y;
        if (spec && (spec->flags & SPEC_IN_FLEX) && fp != flexPos.end()) {
            x = c.ox + fp->second[0];
            y = c.oy + fp->second[1];
        } else if (spec && (spec->flags & SPEC_FLOW_Y)) {
            // 流式：y 由父容器流光标决定，x 为编译期锚点（margin 左缩进）
            y = c.oy + c.cursorY + spec->mt;
            x = c.ox + spec->relX;
            c.cursorY += spec->mt + h + spec->mb;
        } else if (spec) {
            // 显式定位（absolute/explicit）：百分比或编译期锚点
            x = (spec->flags & SPEC_PCT_X) ? c.ox + spec->pctX * c.cw : c.ox + spec->relX;
            y = (spec->flags & SPEC_PCT_Y) ? c.oy + spec->pctY * c.ch : c.oy + spec->relY;
        } else {
            x = c.ox + (node.x - c.ox); // 无规格：保持原位
            y = c.oy + (node.y - c.oy);
        }
        node.x = x;
        node.y = y;
        node.width = w;
        node.height = h;

        // ---- flex 容器：重算子项位置/尺寸（与编译器 layoutFlex 一致） ----
        auto fit = flexOf_.find(node.id);
        if (fit != flexOf_.end()) {
            const BinUISpecFlex& f = *fit->second;
            float cw = w - 2 * node.padding;
            float ch = h - 2 * node.padding;
            if (cw < 0) cw = 0;
            if (ch < 0) ch = 0;
            bool row = f.dir == 0;

            std::vector<size_t> kids; // 子节点下标（id 序）
            for (size_t k = i + 1; k < nodes_.size(); k++) {
                if (nodes_[k].parentId != node.id) continue;
                const BinUISpecNode* ks = specOf_[k];
                if (!ks || !(ks->flags & SPEC_IN_FLEX)) continue;
                kids.push_back(k);
            }
            size_t n = kids.size();
            std::vector<float> wv(n, 0), hv(n, 0), wt(n, 0);
            float weightSum = 0, fixedSum = 0;
            for (size_t k = 0; k < n; k++) {
                const BinUISpecNode* ks = specOf_[kids[k]];
                if (!ks) continue;
                // 交叉轴先按声明值（百分比跟随新内容盒，其余保持编译期尺寸）
                if (ks->flags & SPEC_PCT_W) wv[k] = ks->pctW * cw;
                else if (!row && (ks->flags & SPEC_AUTO_W)) wv[k] = cw;   // column：未声明宽 = 拉伸
                else wv[k] = nodes_[kids[k]].width;
                hv[k] = (ks->flags & SPEC_PCT_H) ? ks->pctH * ch : nodes_[kids[k]].height;

                // 主轴：row 分宽、column 分高；固定尺寸（含百分比）计入 fixedSum
                if (row) {
                    if (ks->flags & SPEC_PCT_W) { fixedSum += wv[k]; }
                    else if (ks->flexWeight > 0) { wt[k] = ks->flexWeight; weightSum += wt[k]; }
                    else fixedSum += wv[k];
                } else {
                    if (ks->flexWeight > 0) { wt[k] = ks->flexWeight; weightSum += wt[k]; }
                    else fixedSum += hv[k];
                }
            }
            float totalGap = (n > 1 ? (float)(n - 1) : 0) * f.gap;
            const float mainLen = row ? cw : ch;
            float remain = mainLen - fixedSum - totalGap;
            if (remain < 0) remain = 0;
            float extra = weightSum > 0 ? remain / weightSum : 0;
            for (size_t k = 0; k < n; k++) {
                if (wt[k] <= 0) continue;
                const float v = extra * wt[k];
                if (row) wv[k] = v; else hv[k] = v;
            }

            // 弹性分配后按子项 min/max 约束钳制（与编译器 layoutFlex 一致）
            for (size_t k = 0; k < n; k++) {
                const BinUISpecNode* ks = specOf_[kids[k]];
                if (!ks) continue;
                wv[k] = ClampSize(wv[k], ks, cw, SPEC_MINW_PCT, SPEC_MAXW_PCT, ks->minW, ks->maxW);
                hv[k] = ClampSize(hv[k], ks, ch, SPEC_MINH_PCT, SPEC_MAXH_PCT, ks->minH, ks->maxH);
            }

            float mainTotal = 0;
            for (size_t k = 0; k < n; k++) mainTotal += row ? wv[k] : hv[k];
            float cross = row ? ch : cw;
            float main = row ? cw : ch; // 主轴可用长度

            // 自由空间：溢出钳到 0（与编译器一致，避免负间距/反向堆叠）
            float freeMain = main - mainTotal - totalGap;
            if (freeMain < 0) freeMain = 0;

            float leading = 0, gap2 = 0;
            switch (f.justify) {
                case 1: leading = freeMain; break;
                case 2: leading = freeMain / 2.0f; break;
                case 3: gap2 = n > 1 ? freeMain / (float)(n - 1) : 0; break;
                case 4: {
                    leading = n > 0 ? freeMain / (2.0f * (float)n) : 0;
                    gap2 = n > 0 ? freeMain / (float)n : 0;
                    break;
                }
                default: break;
            }
            float pos = leading;
            float maxCross = 0;
            for (size_t k = 0; k < n; k++) {
                size_t kidIdx = kids[k];
                float cpos = 0;
                if (f.align == 1) cpos = (cross - (row ? hv[k] : wv[k])) / 2;
                else if (f.align == 2) cpos = cross - (row ? hv[k] : wv[k]);
                flexPos[nodes_[kidIdx].id][0] = row ? pos : cpos;
                flexPos[nodes_[kidIdx].id][1] = row ? cpos : pos;
                flexPos[nodes_[kidIdx].id][2] = wv[k];
                flexPos[nodes_[kidIdx].id][3] = hv[k];
                maxCross = std::max(maxCross, row ? hv[k] : wv[k]);
                pos += (row ? wv[k] : hv[k]) + f.gap + gap2;
            }
            if (f.autoH) {
                float need = row ? maxCross : mainTotal + totalGap;
                node.height = need + 2 * node.padding;
                if (spec) // auto 高同样受 min/max-height 约束钳制
                    node.height = ClampSize(node.height, spec, c.ch, SPEC_MINH_PCT, SPEC_MAXH_PCT, spec->minH, spec->maxH);
            }
        }

        stack.push_back({ node.id, i, x + node.padding, y + node.padding,
                          w - 2 * node.padding, h - 2 * node.padding, 0 });
    }

    // 收尾：弹出剩余容器并自动扩展，最后扩展根（与编译器一致）
    while (stack.size() > 1) {
        growContainer(stack.back());
        stack.pop_back();
    }
    growContainer(stack[0]);

    // 滚动偏移按新视口钳制
    for (auto& [id, s] : scroll_) {
        auto it = nodeIndexById_.find(id);
        if (it == nodeIndexById_.end()) continue;
        const auto& n = nodes_[it->second];
        float maxY = contentHeight(id) - n.height;
        if (maxY < 0) maxY = 0;
        float maxX = 0;
        for (const auto& ch : nodes_) {
            if (ch.parentId == id)
                maxX = std::max(maxX, ch.x + ch.width - n.x);
        }
        maxX -= n.width;
        if (maxX < 0) maxX = 0;
        s.x = std::min(std::max(s.x, 0.0f), maxX);
        s.y = std::min(std::max(s.y, 0.0f), maxY);
    }
}

int BinUIRenderer::hitTest(float x, float y) const {
    return hitTestEx(x, y, false, 0);
}

// 命中测试的扩展：除 BINUI_INTERACTIVE 节点外，还命中"声明了该状态样式"的普通节点
// （CSS 里 :hover/:active 对任何元素都有效，不限于控件）
int BinUIRenderer::hitTestEx(float x, float y, bool allowStyled, uint8_t state) const {
    // 按绘制顺序逆序遍历：最上层（最后绘制者）优先——即 z-index 高者、
    // 同层 absolute 高于流式、同型后声明者（子/后辈）优先。
    const std::vector<uint32_t> order = drawOrder();
    for (size_t oi = order.size(); oi-- > 0;) {
        const size_t i = order[oi];
        const auto& node = nodes_[i];
        if (!(node.flags & BINUI_VISIBLE)) continue;
        bool hittable = (node.flags & BINUI_INTERACTIVE) != 0;
        if (!hittable && allowStyled) hittable = stateStyle(node.id, state) != nullptr;
        if (!hittable) continue;

        // 沿祖先链：不可见子树跳过；滚动容器裁剪视口 + 累加滚动偏移（容器自身不偏移）
        float ox = 0, oy = 0;
        size_t idx = i;
        bool hidden = false;
        while (true) {
            const auto& anc = nodes_[idx];
            if (anc.parentId == 0xFFFF) break;
            auto pit = nodeIndexById_.find(anc.parentId);
            if (pit == nodeIndexById_.end()) break;
            idx = pit->second;
            const auto& par = nodes_[idx];
            if (!(par.flags & BINUI_VISIBLE)) { hidden = true; break; }
            if (par.flags & BINUI_SCROLLABLE) {
                // 视口外内容不可命中
                if (x < par.x || x > par.x + par.width || y < par.y || y > par.y + par.height) {
                    hidden = true;
                    break;
                }
                auto sit = scroll_.find(par.id);
                if (sit != scroll_.end()) { ox += sit->second.x; oy += sit->second.y; }
            }
        }
        if (hidden) continue;

        if (x >= node.x - ox && x <= node.x - ox + node.width &&
            y >= node.y - oy && y <= node.y - oy + node.height) {
            return node.id;
        }
    }
    return -1;
}

void BinUIRenderer::setEventCallback(EventCallback cb) {
    eventCallback_ = std::move(cb);
}

int BinUIRenderer::dispatchClick(float x, float y) {
    press(x, y);
    return release(x, y);
}

// ============ 滚动容器 ============

namespace {
constexpr float kScrollBarW = 8.0f; // 滚动条宽度/厚度
}

bool BinUIRenderer::isScrollable(uint16_t nodeId) const {
    auto it = nodeIndexById_.find(nodeId);
    return it != nodeIndexById_.end() && (nodes_[it->second].flags & BINUI_SCROLLABLE);
}

// 用户可滚动 = SCROLLABLE 且不是 overflow:hidden 的"只裁剪"容器
bool BinUIRenderer::isUserScrollable(uint16_t nodeId) const {
    return isScrollable(nodeId) && clipOnly_.count(nodeId) == 0;
}

// 节点文字色：扩展绘制段优先，否则回退节点自带颜色（LABEL 的旧约定）
uint32_t BinUIRenderer::textColorOf(uint16_t nodeId) const {
    if (const BinUIPaintNode* p = paintOf(nodeId))
        if (p->flags & PAINT_TEXT_COLOR) return p->textColor;
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return 0xFFFFFFFFu;
    return nodes_[it->second].color;
}

float BinUIRenderer::contentHeight(uint16_t nodeId) const {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return 0;
    const auto& c = nodes_[it->second];
    float bottom = 0;
    for (const auto& n : nodes_) {
        if (n.parentId == nodeId)
            bottom = std::max(bottom, n.y + n.height - c.y);
    }
    return bottom;
}

// 滚轮：滚动光标处最顶层的滚动容器（命中节点向上找最近滚动祖先；无交互节点时直接找容器）
int BinUIRenderer::scrollAt(float x, float y, float dy) {
    int id = hitTest(x, y);
    if (id < 0) {
        for (size_t i = nodes_.size(); i-- > 0;) {
            const auto& n = nodes_[i];
            if (!isUserScrollable(n.id) || !(n.flags & BINUI_VISIBLE)) continue;
            if (x >= n.x && x <= n.x + n.width && y >= n.y && y <= n.y + n.height) {
                id = n.id;
                break;
            }
        }
        if (id < 0) return -1;
    } else {
        size_t idx = nodeIndexById_.at(static_cast<uint16_t>(id));
        while (nodes_[idx].parentId != 0xFFFF) {
            auto pit = nodeIndexById_.find(nodes_[idx].parentId);
            if (pit == nodeIndexById_.end()) break;
            idx = pit->second;
            if (isUserScrollable(nodes_[idx].id)) { id = nodes_[idx].id; break; }
        }
    }
    scrollBy(static_cast<uint16_t>(id), 0, -dy);
    return id;
}

void BinUIRenderer::scrollTo(uint16_t nodeId, float x, float y) {
    if (!isScrollable(nodeId)) return;
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return;
    const auto& c = nodes_[it->second];

    float maxY = contentHeight(nodeId) - c.height;
    float maxX = 0;
    for (const auto& n : nodes_) {
        if (n.parentId == nodeId)
            maxX = std::max(maxX, n.x + n.width - c.x);
    }
    maxX -= c.width;
    if (maxY < 0) maxY = 0;
    if (maxX < 0) maxX = 0;

    auto& s = scroll_[nodeId];
    float nx = std::min(std::max(x, 0.0f), maxX);
    float ny = std::min(std::max(y, 0.0f), maxY);
    if (nx == s.x && ny == s.y) return;
    s.x = nx;
    s.y = ny;

    if (eventCallback_) {
        Event ev;
        ev.type = EventType::SCROLL;
        ev.nodeId = nodeId;
        eventCallback_(ev);
    }
}

void BinUIRenderer::scrollBy(uint16_t nodeId, float dx, float dy) {
    auto sit = scroll_.find(nodeId);
    scrollTo(nodeId, (sit != scroll_.end() ? sit->second.x : 0) + dx,
                      (sit != scroll_.end() ? sit->second.y : 0) + dy);
}

const BinUIRenderer::ScrollState* BinUIRenderer::scrollState(uint16_t nodeId) const {
    auto sit = scroll_.find(nodeId);
    return sit == scroll_.end() ? nullptr : &sit->second;
}

// 滚动条几何：返回容器 id 与拇指矩形（thumb 在 viewport 内坐标），无滚动条时返回 false
static bool ScrollThumbRect(const BinUIRenderer::NodeInfo& c, float contentH,
                            float scrollY, bool vertical, float& thumbPos, float& thumbSize) {
    float viewport = vertical ? c.height : c.width;
    if (contentH <= viewport + 0.5f) return false;
    thumbSize = std::max(24.0f, viewport * viewport / contentH);
    float maxScroll = contentH - viewport;
    if (maxScroll <= 0) return false;
    float track = viewport - thumbSize;
    thumbPos = (track > 0 ? scrollY / maxScroll * track : 0);
    return true;
}

int BinUIRenderer::hitScrollBar(float x, float y, bool& vertical) const {
    for (size_t i = nodes_.size(); i-- > 0;) {
        const auto& n = nodes_[i];
        if (!isUserScrollable(n.id) || !(n.flags & BINUI_VISIBLE)) continue;
        float ch = contentHeight(n.id);
        const ScrollState* s = scrollState(n.id);
        float sy = s ? s->y : 0;
        // 垂直滚动条（右缘）
        float tp, ts;
        if (ScrollThumbRect(n, ch, sy, true, tp, ts)) {
            if (x >= n.x + n.width - kScrollBarW && x <= n.x + n.width &&
                y >= n.y && y <= n.y + n.height) {
                vertical = true;
                return n.id;
            }
        }
        // 水平滚动条（下缘）
        if (ScrollThumbRect(n, ch, sy, false, tp, ts)) {
            if (y >= n.y + n.height - kScrollBarW && y <= n.y + n.height &&
                x >= n.x && x <= n.x + n.width) {
                vertical = false;
                return n.id;
            }
        }
    }
    return -1;
}

bool BinUIRenderer::scrollBarHitTest(float x, float y) const {
    bool v = true;
    return hitScrollBar(x, y, v) >= 0;
}

// ============ 指针状态（hover / 按下） ============

void BinUIRenderer::applyFocus(int nodeId) {
    auto it = nodeIndexById_.find(static_cast<uint16_t>(nodeId));
    if (it != nodeIndexById_.end() && nodes_[it->second].type == BINUI_INPUT) {
        if (!isFocused(static_cast<uint16_t>(nodeId))) focus(static_cast<uint16_t>(nodeId));
    } else {
        blur();
    }
}

void BinUIRenderer::setHover(float x, float y) {
    // 拖动滚动条时更新滚动位置，不更新悬停
    if (scrollDragNode_ >= 0) {
        dragScroll(x, y);
        return;
    }
    // 拖动视频进度条：持续定位，不更新悬停（浏览器拖动行为）
    if (vidScrubbing_ && vidPressNode_ >= 0 && vidPressRegion_ == 2) {
        auto it = nodeIndexById_.find(static_cast<uint16_t>(vidPressNode_));
        if (it != nodeIndexById_.end()) {
            const NodeInfo& n = nodes_[it->second];
            VideoBarGeo g;
            if (VideoBarLayout(n, g)) {
                const VideoState* v = findVideo((uint16_t)vidPressNode_);
                if (v) seekVideo((uint16_t)vidPressNode_, VideoBarTimeAt(n, g, x, *v));
            }
        }
        return;
    }
    int id = hitTestEx(x, y, true, BINUI_ST_HOVER);
    if (id == hoverId_) return;
    int old = hoverId_;
    hoverId_ = id;
    refreshNodeStyle(old);
    refreshNodeStyle(id);
    if (id >= 0 && eventCallback_) {
        Event ev;
        ev.type = EventType::HOVER;
        ev.nodeId = static_cast<uint16_t>(id);
        eventCallback_(ev);
    }
}

void BinUIRenderer::clearHover() {
    int old = hoverId_;
    hoverId_ = -1;
    refreshNodeStyle(old);
}

void BinUIRenderer::dragScroll(float px, float py) {
    auto it = nodeIndexById_.find(static_cast<uint16_t>(scrollDragNode_));
    if (it == nodeIndexById_.end()) return;
    const auto& n = nodes_[it->second];
    float ch = contentHeight(n.id);
    const ScrollState* s = scrollState(n.id);
    float viewport = scrollDragVert_ ? n.height : n.width;
    float tp, ts;
    if (!ScrollThumbRect(n, ch, 0, scrollDragVert_, tp, ts)) return;
    float maxScroll = ch - viewport;
    if (maxScroll <= 0) return;
    float pos = (scrollDragVert_ ? py - n.y : px - n.x) - scrollDragGrab_;
    float track = viewport - ts;
    float v = track > 0 ? pos / track * maxScroll : 0;
    scrollTo(n.id, scrollDragVert_ ? (s ? s->x : 0) : v, scrollDragVert_ ? v : (s ? s->y : 0));
}

int BinUIRenderer::press(float x, float y) {
    // 滚动条拖动优先于节点按下
    scrollDragNode_ = -1;
    bool vert = true;
    int sb = hitScrollBar(x, y, vert);
    if (sb >= 0) {
        scrollDragNode_ = sb;
        scrollDragVert_ = vert;
        auto it = nodeIndexById_.find(static_cast<uint16_t>(sb));
        if (it != nodeIndexById_.end()) {
            const auto& n = nodes_[it->second];
            float ch = contentHeight(n.id);
            float tp, ts;
            if (ScrollThumbRect(n, ch, 0, vert, tp, ts))
                scrollDragGrab_ = (vert ? y - n.y : x - n.x) - tp;
        }
        return sb;
    }

    int id = hitTestEx(x, y, true, BINUI_ST_ACTIVE);
    pressedId_ = id;
    applyFocus(id);
    refreshNodeStyle(id);

    // 视频节点：记录控制条按下区域（按钮/进度条/画面体）
    if (id >= 0) {
        auto nit = nodeIndexById_.find(static_cast<uint16_t>(id));
        if (nit != nodeIndexById_.end() && nodes_[nit->second].type == BINUI_VIDEO)
            videoPress(x, y, id);
    }

    // PRESS 事件只对可交互节点派发（带状态样式的普通元素只做视觉反馈）
    bool interactive = false;
    if (id >= 0) {
        auto nit = nodeIndexById_.find(static_cast<uint16_t>(id));
        if (nit != nodeIndexById_.end())
            interactive = (nodes_[nit->second].flags & BINUI_INTERACTIVE) != 0;
    }
    if (id >= 0 && !interactive) return -1;
    if (id >= 0 && eventCallback_) {
        Event ev;
        ev.type = EventType::PRESS;
        ev.nodeId = static_cast<uint16_t>(id);
        ev.click.x = x;
        ev.click.y = y;
        eventCallback_(ev);
    }
    return id;
}

int BinUIRenderer::release(float x, float y) {
    if (scrollDragNode_ >= 0) {
        scrollDragNode_ = -1; // 滚动条松开：不产生点击
        return -1;
    }
    // 视频控制条交互（播放/暂停/进度）优先于普通 CLICK
    if (vidPressNode_ >= 0)
        return videoRelease(x, y, vidPressNode_) ? vidPressNode_ : -1;

    int id = pressedId_;
    pressedId_ = -1;
    refreshNodeStyle(id);
    if (id < 0) return -1;
    if (hitTest(x, y) != id) return -1;

    std::cout << "[binui] Click on node #" << id << " at (" << x << "," << y << ")" << std::endl;

    if (eventCallback_) {
        Event ev;
        ev.type = EventType::CLICK;
        ev.nodeId = static_cast<uint16_t>(id);
        ev.click.x = x;
        ev.click.y = y;
        eventCallback_(ev);
    }
    commitFocus(id);        // 点击过的节点成为键盘焦点（:focus 状态样式随之切换）
    return id;
}

void BinUIRenderer::cancelPress() {
    int old = pressedId_;
    pressedId_ = -1;
    scrollDragNode_ = -1;
    vidPressNode_ = -1;
    vidPressRegion_ = 0;
    vidScrubbing_ = false;
    refreshNodeStyle(old);
}

// ============ 键盘导航（方向键） ============

// 键盘焦点变更（同时刷新受影响节点的状态样式：:focus）
void BinUIRenderer::commitFocus(int newId) {
    const int old = keyboardId_;
    keyboardId_ = newId;
    if (old == newId) return;
    refreshNodeStyle(old);
    refreshNodeStyle(newId);
}

void BinUIRenderer::setKeyboardFocus(int nodeId) {
    if (nodeId < 0) { commitFocus(-1); return; }
    auto it = nodeIndexById_.find(static_cast<uint16_t>(nodeId));
    if (it == nodeIndexById_.end()) { commitFocus(-1); return; }
    if (!(nodes_[it->second].flags & BINUI_INTERACTIVE) || !isNodeVisible(static_cast<uint16_t>(nodeId))) {
        commitFocus(-1);
        return;
    }
    commitFocus(nodeId);
}

// 方向键移动焦点：候选须在指定方向一侧；优先同行/列（轴带重叠）的最近者，
// 无同行/列时按加权距离（垂直于主方向加权重）取最近
int BinUIRenderer::moveKeyboardFocus(KeyDirection dir) {
    const NodeInfo* cur = nullptr;
    if (keyboardId_ >= 0) {
        auto it = nodeIndexById_.find(static_cast<uint16_t>(keyboardId_));
        if (it != nodeIndexById_.end()) cur = &nodes_[it->second];
    }

    // 无焦点：选中第一个可交互节点
    if (!cur) {
        for (const auto& n : nodes_) {
            if ((n.flags & BINUI_INTERACTIVE) && isNodeVisible(n.id)) {
                commitFocus(n.id);
                return keyboardId_;
            }
        }
        return -1;
    }

    float cx = cur->x + cur->width / 2.0f;
    float cy = cur->y + cur->height / 2.0f;
    float curTop = cur->y, curBot = cur->y + cur->height;
    float curLft = cur->x, curRgt = cur->x + cur->width;

    const NodeInfo* sameAxis = nullptr; // 同行/列候选
    float sameAxisDist = 1e30f;
    const NodeInfo* any = nullptr;      // 任意方向候选（加权距离）
    float anyScore = 1e30f;

    for (const auto& n : nodes_) {
        if (n.id == cur->id) continue;
        if (!(n.flags & BINUI_INTERACTIVE)) continue;
        if (!isNodeVisible(n.id)) continue;

        float dx = (n.x + n.width / 2.0f) - cx;
        float dy = (n.y + n.height / 2.0f) - cy;
        bool inDir = false;
        switch (dir) {
            case KeyDirection::RIGHT: inDir = dx > 0; break;
            case KeyDirection::LEFT:  inDir = dx < 0; break;
            case KeyDirection::DOWN:  inDir = dy > 0; break;
            case KeyDirection::UP:    inDir = dy < 0; break;
        }
        if (!inDir) continue;

        if (dir == KeyDirection::RIGHT || dir == KeyDirection::LEFT) {
            float overlap = std::min(curBot, n.y + n.height) - std::max(curTop, n.y);
            if (overlap > 0 && std::fabs(dx) < sameAxisDist) {
                sameAxisDist = std::fabs(dx);
                sameAxis = &n;
            }
            float score = std::fabs(dx) + std::fabs(dy) * 3.0f;
            if (score < anyScore) { anyScore = score; any = &n; }
        } else {
            float overlap = std::min(curRgt, n.x + n.width) - std::max(curLft, n.x);
            if (overlap > 0 && std::fabs(dy) < sameAxisDist) {
                sameAxisDist = std::fabs(dy);
                sameAxis = &n;
            }
            float score = std::fabs(dy) + std::fabs(dx) * 3.0f;
            if (score < anyScore) { anyScore = score; any = &n; }
        }
    }

    if (sameAxis) commitFocus(static_cast<int>(sameAxis->id));
    else if (any) commitFocus(static_cast<int>(any->id));
    return keyboardId_; // 均无候选：焦点不变
}

// Tab 焦点循环：按文档顺序在可交互节点间前进/后退（浏览器式，环绕）
int BinUIRenderer::cycleKeyboardFocus(bool forward) {
    std::vector<uint16_t> order;
    for (const auto& n : nodes_) {
        if (!(n.flags & BINUI_INTERACTIVE)) continue;
        if (!isNodeVisible(n.id)) continue;
        order.push_back(n.id);
    }
    if (order.empty()) { commitFocus(-1); return -1; }

    // 当前焦点不在候选链中（无焦点/被隐藏/非交互）→ 视作从头/尾开始
    size_t pos = order.size();
    if (keyboardId_ >= 0) {
        for (size_t i = 0; i < order.size(); i++)
            if (order[i] == static_cast<uint16_t>(keyboardId_)) { pos = i; break; }
    }
    pos = (pos == order.size()) ? (forward ? 0 : order.size() - 1)
                                : forward ? (pos + 1) % order.size()
                                          : (pos + order.size() - 1) % order.size();
    commitFocus(static_cast<int>(order[pos]));
    return keyboardId_;
}

bool BinUIRenderer::activateKeyboardFocus() {
    if (keyboardId_ < 0) return false;
    auto it = nodeIndexById_.find(static_cast<uint16_t>(keyboardId_));
    if (it == nodeIndexById_.end()) return false;
    const auto& n = nodes_[it->second];

    if (n.type == BINUI_INPUT) {
        focus(n.id);
        return true;
    }
    if (n.type == BINUI_VIDEO) {
        toggleVideo(n.id); // Enter/Space：播放/暂停切换（浏览器行为）
        return true;
    }

    if (eventCallback_) {
        Event ev;
        ev.type = EventType::CLICK;
        ev.nodeId = n.id;
        ev.click.x = n.x + n.width / 2.0f;
        ev.click.y = n.y + n.height / 2.0f;
        eventCallback_(ev);
    }
    return true;
}

// ============ 视频（v9） ============

// 控制条几何：渲染器命中测试与绘制后端共用同一布局（见 VideoBarGeo）
constexpr float BinUIRenderer::kVideoBarH;

bool BinUIRenderer::VideoBarLayout(const NodeInfo& n, VideoBarGeo& g) {
    if (n.width < 120 || n.height < 90) return false; // 过小不放控制条
    g.w = n.width;
    g.h = kVideoBarH;
    g.x = n.x;
    g.y = n.y + n.height - kVideoBarH;
    g.btnS = 24.0f;
    g.btnX = n.x + 10.0f;
    g.btnY = g.y + (kVideoBarH - g.btnS) * 0.5f;
    g.trY = g.y + kVideoBarH * 0.5f - 2.0f;
    g.trH = 4.0f;
    g.trX = g.btnX + g.btnS + 12.0f;
    g.thumbR = 6.0f;
    g.trW = n.width - (g.trX - n.x) - 66.0f; // 右侧留时间文本宽度
    return g.trW >= 40.0f;
}

BinUIRenderer::VideoState* BinUIRenderer::findVideoMut(uint16_t nodeId) {
    auto it = videoIdxByNode_.find(nodeId);
    return it == videoIdxByNode_.end() ? nullptr : &videos_[it->second];
}

const BinUIRenderer::VideoState* BinUIRenderer::findVideo(uint16_t nodeId) const {
    auto it = videoIdxByNode_.find(nodeId);
    return it == videoIdxByNode_.end() ? nullptr : &videos_[it->second];
}

void BinUIRenderer::emitVideoEvent(EventType t, uint16_t nodeId) {
    if (eventCallback_) {
        Event ev;
        ev.type = t;
        ev.nodeId = nodeId;
        eventCallback_(ev);
    }
}

bool BinUIRenderer::playVideo(uint16_t nodeId) {
    auto* v = findVideoMut(nodeId);
    if (!v || v->playing) return v != nullptr;
    if (v->duration > 0 && v->position >= v->duration - 1e-4)
        v->position = 0;                       // 播完后再播 → 从头
    v->ended = false;
    v->playing = true;
    vidClock_[nodeId] = { v->position, AnimNow() };
    emitVideoEvent(EventType::VIDEO_PLAY, nodeId);
    return true;
}

bool BinUIRenderer::pauseVideo(uint16_t nodeId) {
    auto* v = findVideoMut(nodeId);
    if (!v || !v->playing) return v != nullptr && !v->playing;
    auto it = vidClock_.find(nodeId);
    if (it != vidClock_.end()) {
        double pos = it->second.first + (AnimNow() - it->second.second);
        if (v->duration > 0) pos = std::min(std::max(pos, 0.0), v->duration);
        v->position = pos;
        vidClock_.erase(it);
    }
    v->playing = false;
    emitVideoEvent(EventType::VIDEO_PAUSE, nodeId);
    return true;
}

bool BinUIRenderer::toggleVideo(uint16_t nodeId) {
    auto* v = findVideo(nodeId);
    if (!v) return false;
    return v->playing ? (pauseVideo(nodeId), false) : (playVideo(nodeId), true);
}

void BinUIRenderer::seekVideo(uint16_t nodeId, double sec, bool quiet) {
    auto* v = findVideoMut(nodeId);
    if (!v) return;
    sec = std::max(0.0, sec);
    if (v->duration > 0) sec = std::min(sec, v->duration);
    v->position = sec;
    v->ended = false;
    if (v->playing) vidClock_[nodeId] = { sec, AnimNow() };
    if (!quiet) emitVideoEvent(EventType::VIDEO_SEEK, nodeId);
}

void BinUIRenderer::setVideoVolume(uint16_t nodeId, float vol) {
    auto* v = findVideoMut(nodeId);
    if (v) v->volume = std::min(std::max(vol, 0.0f), 1.0f);
}

void BinUIRenderer::setVideoMuted(uint16_t nodeId, bool muted) {
    auto* v = findVideoMut(nodeId);
    if (!v) return;
    if (muted) v->flags |= VID_MUTED;
    else       v->flags &= (uint16_t)~VID_MUTED;
}

void BinUIRenderer::setVideoSource(uint16_t nodeId, const std::string& src) {
    auto* v = findVideoMut(nodeId);
    if (v && !v->playing) v->src = src;
}

void BinUIRenderer::setVideoDuration(uint16_t nodeId, double sec) {
    auto* v = findVideoMut(nodeId);
    if (v && sec > 0) v->duration = sec;
}

bool BinUIRenderer::videoControlsVisible(uint16_t nodeId) const {
    const VideoState* v = findVideo(nodeId);
    if (!v || !(v->flags & VID_CONTROLS)) return false;
    if (vidScrubbing_ && vidPressNode_ == (int)nodeId) return true;
    if (!v->playing) return true;   // 暂停时常显
    return hoverId_ == (int)nodeId; // 播放中悬停才显示（浏览器行为）
}

// 进度条按下区域的时间换算（x → 秒）
static double VideoBarTimeAt(const BinUIRenderer::NodeInfo& n,
                             const BinUIRenderer::VideoBarGeo& g, float x,
                             const BinUIRenderer::VideoState& v) {
    float rx = (g.trW > 0) ? (x - g.trX) / g.trW : 0.0f;
    rx = std::min(std::max(rx, 0.0f), 1.0f);
    (void)n;
    return (double)rx * v.duration;
}

void BinUIRenderer::videoPress(float x, float y, int nodeId) {
    vidPressNode_ = nodeId;
    vidPressRegion_ = 0;      // 默认画面体
    (void)x; (void)y;         // 区域判定只用几何；保留坐标参数供扩展
    vidScrubbing_ = false;
    if (nodeId < 0) { vidPressNode_ = -1; return; }
    auto it = nodeIndexById_.find(static_cast<uint16_t>(nodeId));
    if (it == nodeIndexById_.end()) { vidPressNode_ = -1; return; }
    const NodeInfo& n = nodes_[it->second];
    VideoBarGeo g;
    if (n.type != BINUI_VIDEO) return;
    if (!videoControlsVisible((uint16_t)nodeId) || !VideoBarLayout(n, g)) return;

    if (x >= g.btnX - 3 && x <= g.btnX + g.btnS + 3 &&
        y >= g.btnY - 3 && y <= g.btnY + g.btnS + 3) {
        vidPressRegion_ = 1;  // 播放/暂停按钮
        return;
    }
    bool inBar = y >= n.y + n.height - kVideoBarH - 4 && y <= n.y + n.height;
    bool inTrack = x >= g.trX - 8 && x <= g.trX + g.trW + 8;
    if (inBar && inTrack) {
        vidPressRegion_ = 2;  // 进度条：按下即定位并进入拖动
        vidScrubbing_ = true;
        const VideoState* v = findVideo((uint16_t)nodeId);
        if (v) seekVideo((uint16_t)nodeId, VideoBarTimeAt(n, g, x, *v));
    }
}

bool BinUIRenderer::videoRelease(float x, float y, int nodeId) {
    int reg = vidPressRegion_;
    bool scrub = vidScrubbing_;
    vidPressNode_ = -1;
    vidPressRegion_ = 0;
    vidScrubbing_ = false;
    refreshNodeStyle(nodeId);
    // 进度条拖动已在 press/move 中定位；画面体/按钮需松开时仍在节点上
    if (!scrub && reg != 2 && hitTest(x, y) != nodeId) return true;
    if (reg != 2) toggleVideo(static_cast<uint16_t>(nodeId)); // 体点击=播放/暂停切换
    commitFocus(nodeId);
    return true;
}

void BinUIRenderer::stepVideos(double now) {
    bool first = !vidAutoplayDone_;
    vidAutoplayDone_ = true;
    for (auto& v : videos_) {
        bool dirty = false;
        // autoplay：首帧步进触发一次
        if (first && (v.flags & VID_AUTOPLAY) && !v.playing) {
            if (v.duration > 0 && v.position >= v.duration - 1e-4) v.position = 0;
            v.ended = false;
            v.playing = true;
            vidClock_[v.nodeId] = { v.position, now };
            emitVideoEvent(EventType::VIDEO_PLAY, v.nodeId);
            dirty = true;
        }
        auto it = vidClock_.find(v.nodeId);
        if (v.playing && it != vidClock_.end()) {
            double pos = it->second.first + (now - it->second.second);
            if (v.duration > 0 && pos >= v.duration) {
                if (v.flags & VID_LOOP) {
                    pos = std::fmod(pos, v.duration);   // 回绕（不触发 ENDED）
                    it->second = { pos, now };
                } else {
                    pos = v.duration;
                    v.playing = false;
                    v.ended = true;
                    vidClock_.erase(it);
                    emitVideoEvent(EventType::VIDEO_ENDED, v.nodeId);
                }
            }
            v.position = pos;
            dirty = true;     // 播放中每帧都重绘（进度条/画面推进）
        }
        if (dirty) animChanged_.push_back(v.nodeId);
    }
}

void BinUIRenderer::reserveImageSlots(uint16_t count) {
    for (uint16_t i = (uint16_t)imageOrder_.size(); i < count; i++) {
        imageOrder_.push_back("@slot" + std::to_string(i)); // 视频伪槽占位名
        imageGen_.push_back(0);
    }
}

// ============ 文本框编辑 ============

void BinUIRenderer::focus(uint16_t nodeId) {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end() || nodes_[it->second].type != BINUI_INPUT) {
        blur();
        return;
    }
    if (edit_.active && edit_.nodeId != nodeId) blur();

    edit_.active = true;
    edit_.nodeId = nodeId;
    edit_.value = nodes_[it->second].text;
    edit_.caret = edit_.selStart = edit_.value.size();
    refreshNodeStyle(static_cast<int>(nodeId));   // :focus 状态样式（正在编辑也算聚焦）

    if (eventCallback_) {
        Event ev;
        ev.type = EventType::FOCUS;
        ev.nodeId = nodeId;
        eventCallback_(ev);
    }
}

void BinUIRenderer::blur() {
    if (!edit_.active) return;
    uint16_t nodeId = edit_.nodeId;
    std::string finalValue = edit_.value;
    bool changed = false;

    edit_.active = false;
    edit_.value.clear();
    edit_.caret = edit_.selStart = 0;
    refreshNodeStyle(static_cast<int>(nodeId));   // 结束编辑：:focus 状态样式回退

    auto it = nodeIndexById_.find(nodeId);
    if (it != nodeIndexById_.end() && nodes_[it->second].text != finalValue) {
        nodes_[it->second].text = finalValue;
        changed = true;
    }

    if (changed && eventCallback_) {
        Event ev;
        ev.type = EventType::INPUT;
        ev.nodeId = nodeId;
        ev.input.text = finalValue.c_str();
        eventCallback_(ev);
    }
    if (eventCallback_) {
        Event ev;
        ev.type = EventType::FOCUS;
        ev.nodeId = nodeId;
        eventCallback_(ev);
    }
}

void BinUIRenderer::commitTextEvent() {
    if (!eventCallback_ || !edit_.active) return;
    Event ev;
    ev.type = EventType::INPUT;
    ev.nodeId = edit_.nodeId;
    ev.input.text = edit_.value.c_str();
    eventCallback_(ev);
}

bool BinUIRenderer::insertText(const std::string& utf8) {
    if (!edit_.active || utf8.empty()) return false;
    auto& v = edit_.value;
    size_t a = std::min(edit_.caret, edit_.selStart);
    size_t b = std::max(edit_.caret, edit_.selStart);
    v.replace(a, b - a, utf8);
    edit_.caret = a + utf8.size();
    edit_.selStart = edit_.caret;
    commitTextEvent();
    return true;
}

bool BinUIRenderer::backspace() {
    if (!edit_.active) return false;
    auto& v = edit_.value;
    size_t a = std::min(edit_.caret, edit_.selStart);
    size_t b = std::max(edit_.caret, edit_.selStart);
    if (b > a) {
        v.erase(a, b - a);
    } else if (a > 0) {
        size_t start = a - 1;
        while (start > 0 && Utf8IsCont(static_cast<uint8_t>(v[start]))) --start;
        v.erase(start, a - start);
        a = start;
    } else {
        return false;
    }
    edit_.caret = edit_.selStart = a;
    commitTextEvent();
    return true;
}

bool BinUIRenderer::deleteForward() {
    if (!edit_.active) return false;
    auto& v = edit_.value;
    size_t a = std::min(edit_.caret, edit_.selStart);
    size_t b = std::max(edit_.caret, edit_.selStart);
    if (b > a) {
        v.erase(a, b - a);
    } else if (a < v.size()) {
        v.erase(a, Utf8CharLen(static_cast<uint8_t>(v[a])));
    } else {
        return false;
    }
    edit_.caret = edit_.selStart = a;
    commitTextEvent();
    return true;
}

bool BinUIRenderer::moveCaret(int deltaChars, bool extend) {
    if (!edit_.active || deltaChars == 0) return false;
    size_t oldCaret = edit_.caret;
    size_t oldSel = edit_.selStart;
    edit_.caret = MoveByChars(edit_.value, oldCaret, deltaChars);
    if (extend) {
        if (oldSel == oldCaret) edit_.selStart = oldCaret; // 首次扩展时锚定起点
    } else {
        edit_.selStart = edit_.caret;
    }
    return edit_.caret != oldCaret || edit_.selStart != oldSel;
}

bool BinUIRenderer::setCaret(size_t byteOffset) {
    if (!edit_.active) return false;
    edit_.caret = std::min(byteOffset, edit_.value.size());
    edit_.caret = ClampToCharBoundary(edit_.value, edit_.caret);
    edit_.selStart = edit_.caret;
    return true;
}

bool BinUIRenderer::setCaretByUtf16Index(size_t utf16Index) {
    if (!edit_.active) return false;
    edit_.caret = Utf16IndexToBytes(edit_.value, utf16Index);
    edit_.selStart = edit_.caret;
    return true;
}

void BinUIRenderer::dispatchInput(uint16_t nodeId, const char* text) {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return;

    auto& node = nodes_[it->second];
    if (node.type != BINUI_INPUT) return;

    node.text = text;

    if (eventCallback_) {
        Event ev;
        ev.type = EventType::INPUT;
        ev.nodeId = nodeId;
        ev.input.text = node.text.c_str();
        eventCallback_(ev);
    }
}

// ============================================================
// v7 动画引擎
// ============================================================

// 把关键帧属性合并进结果（受 mask 限制）
void BinUIRenderer::ApplyKeyFrameProps(const BinUIKeyFrame& kf, uint16_t mask, AnimResult& o) {
    if (mask & ANIM_OPACITY)   { o.opSet = true; o.opacity = kf.opacity; }
    if (mask & ANIM_TRANSLATE) { o.offSet = true; o.offX = kf.tx; o.offY = kf.ty; }
    if (mask & ANIM_SCALE)     { o.scaleSet = true; o.scale = kf.scale; }
    if (mask & ANIM_BG_COLOR)  { o.colorSet = true; o.color = kf.bgColor; }
    if (mask & ANIM_RADIUS)    { o.radiusSet = true; o.radius = kf.radius; }
    if (mask & (ANIM_OPACITY|ANIM_TRANSLATE|ANIM_SCALE|ANIM_BG_COLOR|ANIM_RADIUS)) o.active = true;
}

// 在关键帧序列上按进度 e（0..1）插值；每个属性只在携带该属性的关键帧间插值，
// 未声明的中间帧不参与该属性的插值（CSS 语义）
void BinUIRenderer::InterpKeyFrames(const BinUIKeyFrame* kfs, size_t n, uint16_t mask, float e,
                                    AnimResult& o) {
    if (n == 0) return;
    float target = e * 10000.0f;
    auto floatAt = [&](uint16_t prop, float(*get)(const BinUIKeyFrame&), float def) -> float {
        const BinUIKeyFrame* prev = nullptr;
        const BinUIKeyFrame* next = nullptr;
        for (size_t i = 0; i < n; i++) {
            if (!(kfs[i].propMask & prop)) continue;
            if (kfs[i].percent <= target) prev = &kfs[i];
            else if (!next) next = &kfs[i];
        }
        if (!prev && !next) return def;
        if (!prev) return get(*next);
        if (!next) return get(*prev);
        if (prev->percent == next->percent) return get(*prev);
        float t = (target - prev->percent) / (float)(next->percent - prev->percent);
        return get(*prev) + (get(*next) - get(*prev)) * t;
    };
    if (mask & ANIM_OPACITY) {
        o.opSet = true;
        o.opacity = floatAt(ANIM_OPACITY, [](const BinUIKeyFrame& k) { return k.opacity; }, 1.0f);
    }
    if (mask & ANIM_TRANSLATE) {
        o.offSet = true;
        o.offX = floatAt(ANIM_TRANSLATE, [](const BinUIKeyFrame& k) { return k.tx; }, 0.0f);
        o.offY = floatAt(ANIM_TRANSLATE, [](const BinUIKeyFrame& k) { return k.ty; }, 0.0f);
    }
    if (mask & ANIM_SCALE) {
        o.scaleSet = true;
        o.scale = floatAt(ANIM_SCALE, [](const BinUIKeyFrame& k) { return k.scale; }, 1.0f);
    }
    if (mask & ANIM_RADIUS) {
        o.radiusSet = true;
        o.radius = floatAt(ANIM_RADIUS, [](const BinUIKeyFrame& k) { return k.radius; }, -1.0f);
    }
    if (mask & ANIM_BG_COLOR) {
        const BinUIKeyFrame* prev = nullptr;
        const BinUIKeyFrame* next = nullptr;
        for (size_t i = 0; i < n; i++) {
            if (!(kfs[i].propMask & ANIM_BG_COLOR)) continue;
            if (kfs[i].percent <= target) prev = &kfs[i];
            else if (!next) next = &kfs[i];
        }
        if (prev || next) {
            o.colorSet = true;
            if (!prev) o.color = next->bgColor;
            else if (!next || prev->percent == next->percent) o.color = prev->bgColor;
            else {
                float t = (target - prev->percent) / (float)(next->percent - prev->percent);
                o.color = LerpColor(prev->bgColor, next->bgColor, t);
            }
        }
    }
    if (mask & (ANIM_OPACITY|ANIM_TRANSLATE|ANIM_SCALE|ANIM_BG_COLOR|ANIM_RADIUS)) o.active = true;
}

// 求动画 def 在 now 时刻对可视属性的贡献（含迭代/方向/fill；start = 绝对起始时刻，已含 delay）
void BinUIRenderer::EvalKeyFrameAnim(const BinUIKeyFrame* kfs, size_t n, const BinUIAnimDef& def,
                                     double start, double now, AnimResult& o) {
    if (n == 0) return;
    double dur = def.durationMs / 1000.0;
    if (dur <= 0) dur = 1e-3;
    double elapsed = now - start;
    bool infinite = def.iteration == 0;
    double total = infinite ? 1e18 : dur * def.iteration;
    bool backwards = (def.fillMode == BINUI_ANIM_FILL_BACKWARDS || def.fillMode == BINUI_ANIM_FILL_BOTH);
    bool forwards  = (def.fillMode == BINUI_ANIM_FILL_FORWARDS || def.fillMode == BINUI_ANIM_FILL_BOTH);

    if (elapsed < 0) { if (backwards) ApplyKeyFrameProps(kfs[0], def.propMask, o); return; }
    if (!infinite && elapsed >= total) {
        if (forwards) ApplyKeyFrameProps(kfs[n - 1], def.propMask, o);
        return;
    }
    double iterF = elapsed / dur;
    uint32_t iter = (uint32_t)iterF;
    double local = iterF - (double)iter;
    bool reverse = false;
    switch (def.direction) {
        case BINUI_ANIM_REVERSE:     reverse = true; break;
        case BINUI_ANIM_ALTERNATE:   reverse = (iter % 2) == 1; break;
        case BINUI_ANIM_ALT_REVERSE: reverse = (iter % 2) == 0; break;
        default: break;
    }
    double prog = reverse ? (1.0 - local) : local;
    float e = EaseTime(def.easing, (float)prog, &def.e0x);
    InterpKeyFrames(kfs, n, def.propMask, e, o);
}

bool BinUIRenderer::hasTransition(uint16_t nodeId, uint16_t prop) const {
    for (const auto& t : transitions_)
        if (t.nodeId == nodeId && (t.propMask & prop)) return true;
    return false;
}

void BinUIRenderer::startKeyframeAnim(uint16_t nodeId, const BinUIAnimDef& def) {
    if (def.groupIdx >= keyGroups_.size() || keyGroups_[def.groupIdx].kfs.empty()) return;
    AnimRuntime::KF kf;
    kf.def = &def;
    kf.start = animT0_ + def.delayMs / 1000.0;
    kf.total = def.iteration ? def.durationMs * def.iteration / 1000.0 : 1e18;
    kf.infinite = def.iteration == 0;
    animState_[nodeId].anims.push_back(kf);
}

void BinUIRenderer::startTransition(uint16_t nodeId, uint16_t prop, double now,
                                    float fromX, float fromY, float toX, float toY,
                                    uint32_t fromC, uint32_t toC) {
    for (const auto& tr : transitions_) {
        if (tr.nodeId != nodeId || !(tr.propMask & prop)) continue;
        AnimRuntime& rt = animState_[nodeId];
        for (auto it = rt.trans.begin(); it != rt.trans.end(); )
            if (it->prop == prop) it = rt.trans.erase(it); else ++it;
        AnimRuntime::TR t;
        t.prop = prop;
        t.start = now + tr.delayMs / 1000.0;
        t.end = t.start + tr.durationMs / 1000.0;
        t.easing = tr.easing;
        t.e0x = tr.e0x; t.e0y = tr.e0y; t.e1x = tr.e1x; t.e1y = tr.e1y;
        t.from[0] = fromX; t.from[1] = fromY;
        t.to[0] = toX;     t.to[1] = toY;
        t.fromC = fromC;   t.toC = toC;
        t.isColor = (prop == ANIM_BG_COLOR);
        rt.trans.push_back(t);
        return;
    }
}

// ============ 状态样式（v11：CSS :hover/:active/:focus/:checked） ============

const BinUIRenderer::StateStyle* BinUIRenderer::stateStyle(uint16_t nodeId, uint8_t state) const {
    auto it = stateCSS_.find(nodeId);
    if (it == stateCSS_.end()) return nullptr;
    const int idx = (state == BINUI_ST_HOVER)   ? 0
                  : (state == BINUI_ST_ACTIVE)  ? 1
                  : (state == BINUI_ST_FOCUS)   ? 2
                  : (state == BINUI_ST_CHECKED) ? 3 : -1;
    if (idx < 0) return nullptr;
    const StateStyle& s = it->second[idx];
    return s.fields ? &s : nullptr;
}

// 当前生效的状态样式：按下 > 键盘焦点/编辑 > 勾选 > 悬停。
// CSS 里多个伪类同时命中时由文档序决定，本引擎按"操作强度"排优先级。
const BinUIRenderer::StateStyle* BinUIRenderer::activeStateStyle(uint16_t nodeId) const {
    auto it = stateCSS_.find(nodeId);
    if (it == stateCSS_.end()) return nullptr;
    const int id = (int)nodeId;
    int order[4];
    int n = 0;
    if (pressedId_ == id) order[n++] = 1;
    if (keyboardId_ == id || (edit_.active && edit_.nodeId == nodeId)) order[n++] = 2;
    if (checkedNodes_.count(nodeId)) order[n++] = 3;
    if (hoverId_ == id) order[n++] = 0;
    for (int i = 0; i < n; i++)
        if (it->second[order[i]].fields) return &it->second[order[i]];
    return nullptr;
}

void BinUIRenderer::setNodeChecked(uint16_t nodeId, bool checked) {
    const bool cur = checkedNodes_.count(nodeId) != 0;
    if (cur == checked) return;
    if (checked) checkedNodes_.insert(nodeId);
    else         checkedNodes_.erase(nodeId);
    refreshNodeStyle((int)nodeId);
}

// 状态变化后：重算节点的颜色/缩放/圆角目标并启动过渡。
// 优先用 v11 状态样式（作者声明的 :hover/:active/:focus/:checked 目标值）；
// 没有声明状态样式的按钮仍走旧启发式（悬停微亮 + 放大、按下压暗）。
void BinUIRenderer::refreshNodeStyle(int id) {
    if (id < 0) return;
    auto it = nodeIndexById_.find(static_cast<uint16_t>(id));
    if (it == nodeIndexById_.end()) return;
    const NodeInfo& n = nodes_[it->second];
    const uint16_t nid = static_cast<uint16_t>(id);
    const StateStyle* st = activeStateStyle(nid);
    const bool isButton = (n.type == BINUI_BUTTON);
    const bool hasStates = stateCSS_.count(nid) != 0;
    if (!st && !hasStates && !isButton) return;

    double now = AnimNow();
    animNow_ = now;
    const uint32_t base = n.color;
    uint32_t targetColor = base;
    float targetScale = 1.0f;
    float targetRadius = -1.0f;
    if (st) {
        if (st->fields & BINUI_STF_COLOR)  targetColor  = st->color;
        if (st->fields & BINUI_STF_SCALE)  targetScale  = st->scale;
        if (st->fields & BINUI_STF_RADIUS) targetRadius = st->radius;
    } else if (isButton) {
        if (pressedId_ == id) targetColor = TintNodeColor(base, -0.35f);
        else if (hoverId_ == id) { targetColor = TintNodeColor(base, 0.12f); targetScale = 1.05f; }
    }

    AnimRuntime& rt = animState_[id];
    uint32_t prevC = rt.colorTargets.count(ANIM_BG_COLOR) ? rt.colorTargets[ANIM_BG_COLOR] : base;
    float fromScale = drawScale(nid);
    float fromRadius = drawRadius(nid);
    rt.colorTargets[ANIM_BG_COLOR] = targetColor;
    rt.floatTargets[ANIM_SCALE] = targetScale;
    if (targetRadius >= 0) rt.floatTargets[ANIM_RADIUS] = targetRadius;
    else                   rt.floatTargets.erase(ANIM_RADIUS);

    if (hasTransition(nid, ANIM_BG_COLOR) && prevC != targetColor)
        startTransition(nid, ANIM_BG_COLOR, now, 0, 0, 0, 0, prevC, targetColor);
    if (hasTransition(nid, ANIM_SCALE) && fromScale != targetScale)
        startTransition(nid, ANIM_SCALE, now, fromScale, 0, targetScale, 0, 0, 0);
    if (hasTransition(nid, ANIM_RADIUS) && targetRadius >= 0 && fromRadius != targetRadius)
        startTransition(nid, ANIM_RADIUS, now, fromRadius, 0, targetRadius, 0, 0, 0);
}

bool BinUIRenderer::AnimResultEqual(const AnimResult& a, const AnimResult& b) {
    return a.active == b.active && a.opSet == b.opSet && a.opacity == b.opacity &&
           a.offSet == b.offSet && a.offX == b.offX && a.offY == b.offY &&
           a.scaleSet == b.scaleSet && a.scale == b.scale &&
           a.colorSet == b.colorSet && a.color == b.color &&
           a.radiusSet == b.radiusSet && a.radius == b.radius;
}

void BinUIRenderer::computeAnimResult(uint16_t nodeId) {
    auto it = animState_.find(nodeId);
    if (it == animState_.end()) { animResult_.erase(nodeId); return; }
    AnimRuntime& rt = it->second;
    AnimResult res;

    // 关键帧动画（多个时后写覆盖）
    for (auto& ka : rt.anims) {
        AnimResult overlay;
        if (ka.def->groupIdx < keyGroups_.size()) {
            const auto& kfs = keyGroups_[ka.def->groupIdx].kfs;
            EvalKeyFrameAnim(kfs.data(), kfs.size(), *ka.def, ka.start, animNow_, overlay);
        }
        if (overlay.opSet)     { res.opSet = true; res.opacity = overlay.opacity; }
        if (overlay.offSet)    { res.offSet = true; res.offX = overlay.offX; res.offY = overlay.offY; }
        if (overlay.scaleSet)  { res.scaleSet = true; res.scale = overlay.scale; }
        if (overlay.colorSet)  { res.colorSet = true; res.color = overlay.color; }
        if (overlay.radiusSet) { res.radiusSet = true; res.radius = overlay.radius; }
        if (overlay.active) res.active = true;
    }

    // 过渡（完成即移除；目标已写入 targets，drawX 回落到目标值）
    for (auto t = rt.trans.begin(); t != rt.trans.end(); ) {
        res.active = true;
        double now = animNow_;
        float p = 1.0f;
        if (now < t->start) {
            p = 0.0f; ++t;
        } else {
            if (now >= t->end) { t = rt.trans.erase(t); continue; }
            p = (float)((now - t->start) / (t->end - t->start));
            p = EaseTime(t->easing, p, &t->e0x);
            ++t;
        }
        const AnimRuntime::TR& tr = t[-1];
        if (tr.isColor) {
            res.colorSet = true;
            res.color = LerpColor(tr.fromC, tr.toC, p);
        } else if (tr.prop == ANIM_TRANSLATE) {
            res.offSet = true;
            res.offX = tr.from[0] + (tr.to[0] - tr.from[0]) * p;
            res.offY = tr.from[1] + (tr.to[1] - tr.from[1]) * p;
        } else {
            float v = tr.from[0] + (tr.to[0] - tr.from[0]) * p;
            if (tr.prop == ANIM_OPACITY) { res.opSet = true; res.opacity = v; }
            else if (tr.prop == ANIM_SCALE) { res.scaleSet = true; res.scale = v; }
            else if (tr.prop == ANIM_RADIUS) { res.radiusSet = true; res.radius = v; }
        }
    }

    animResult_[nodeId] = res;
}

std::vector<uint16_t> BinUIRenderer::stepAnimations() {
    animChanged_.clear();
    if (!animTimeInit_) return animChanged_;
    animNow_ = AnimNow();

    stepVideos(animNow_); // 视频播放时钟推进（变化的视频节点 id 一并返回）

    // 有动画状态/过渡/样式目标的节点都要评估
    std::vector<uint16_t> touched;
    for (const auto& kv : animState_) touched.push_back(kv.first);

    for (uint16_t id : touched) {
        auto pit = nodeIndexById_.find(id);
        if (pit == nodeIndexById_.end()) continue;
        AnimResult prev;
        auto pr = animResult_.find(id);
        if (pr != animResult_.end()) prev = pr->second;
        computeAnimResult(id);
        const auto& cur = animResult_[id];
        if (!AnimResultEqual(prev, cur)) animChanged_.push_back(id);
    }
    return animChanged_;
}

void BinUIRenderer::restartAnimations() {
    animT0_ = AnimNow();
    animNow_ = animT0_;
    for (auto& kv : animState_) {
        kv.second.anims.clear();
        kv.second.trans.clear();
    }
    for (const auto& d : animDefs_)
        startKeyframeAnim(d.nodeId, d);
    animChanged_.clear();
    for (const auto& kv : animState_) {
        computeAnimResult(kv.first);
        animChanged_.push_back(kv.first);
    }
}

void BinUIRenderer::setStyleFloat(uint16_t nodeId, uint16_t prop, float value) {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return;
    AnimRuntime& rt = animState_[nodeId];
    double now = AnimNow();
    animNow_ = now;
    if (prop == ANIM_TRANSLATE) {
        float fx = drawOffsetX(nodeId), fy = drawOffsetY(nodeId);
        rt.floatTargets[ANIM_TRANSLATE] = value;
        startTransition(nodeId, prop, now, fx, fy, value, value, 0, 0);
        return;
    }
    float from = (prop == ANIM_OPACITY) ? drawOpacity(nodeId)
               : (prop == ANIM_SCALE)   ? drawScale(nodeId)
               : drawRadius(nodeId);
    if (prop == ANIM_OPACITY) rt.floatTargets[ANIM_OPACITY] = value;
    else if (prop == ANIM_SCALE) rt.floatTargets[ANIM_SCALE] = value;
    else if (prop == ANIM_RADIUS) rt.floatTargets[ANIM_RADIUS] = value;
    startTransition(nodeId, prop, now, from, 0, value, 0, 0, 0);
}

void BinUIRenderer::setStyleColor(uint16_t nodeId, uint16_t prop, uint32_t value) {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end() || prop != ANIM_BG_COLOR) return;
    AnimRuntime& rt = animState_[nodeId];
    rt.colorTargets[ANIM_BG_COLOR] = value;
    double now = AnimNow();
    animNow_ = now;
    startTransition(nodeId, prop, now, 0, 0, 0, 0, drawColor(nodeId), value);
}

uint32_t BinUIRenderer::drawColor(uint16_t nodeId) const {
    auto ar = animResult_.find(nodeId);
    if (ar != animResult_.end() && ar->second.colorSet) return ar->second.color;
    auto it = animState_.find(nodeId);
    if (it != animState_.end()) {
        auto c = it->second.colorTargets.find(ANIM_BG_COLOR);
        if (c != it->second.colorTargets.end()) return c->second;
    }
    auto ni = nodeIndexById_.find(nodeId);
    return (ni == nodeIndexById_.end()) ? 0xFFFFFFFF : nodes_[ni->second].color;
}

float BinUIRenderer::drawOpacity(uint16_t nodeId) const {
    auto ar = animResult_.find(nodeId);
    if (ar != animResult_.end() && ar->second.opSet) return ar->second.opacity;
    auto it = animState_.find(nodeId);
    if (it != animState_.end()) {
        auto f = it->second.floatTargets.find(ANIM_OPACITY);
        if (f != it->second.floatTargets.end()) return f->second;
    }
    return 1.0f;
}

float BinUIRenderer::drawOffsetX(uint16_t nodeId) const {
    auto ar = animResult_.find(nodeId);
    if (ar != animResult_.end() && ar->second.offSet) return ar->second.offX;
    auto it = animState_.find(nodeId);
    if (it != animState_.end()) {
        auto f = it->second.floatTargets.find(ANIM_TRANSLATE);
        if (f != it->second.floatTargets.end()) return f->second;
    }
    return 0.0f;
}

float BinUIRenderer::drawOffsetY(uint16_t nodeId) const {
    auto ar = animResult_.find(nodeId);
    if (ar != animResult_.end() && ar->second.offSet) return ar->second.offY;
    auto it = animState_.find(nodeId);
    if (it != animState_.end()) {
        auto f = it->second.floatTargets.find(ANIM_TRANSLATE);
        if (f != it->second.floatTargets.end()) return f->second;
    }
    return 0.0f;
}

float BinUIRenderer::drawScale(uint16_t nodeId) const {
    auto ar = animResult_.find(nodeId);
    if (ar != animResult_.end() && ar->second.scaleSet) return ar->second.scale;
    auto it = animState_.find(nodeId);
    if (it != animState_.end()) {
        auto f = it->second.floatTargets.find(ANIM_SCALE);
        if (f != it->second.floatTargets.end()) return f->second;
    }
    return 1.0f;
}

float BinUIRenderer::drawRadius(uint16_t nodeId) const {
    auto ar = animResult_.find(nodeId);
    if (ar != animResult_.end() && ar->second.radiusSet) return ar->second.radius;
    auto it = animState_.find(nodeId);
    if (it != animState_.end()) {
        auto f = it->second.floatTargets.find(ANIM_RADIUS);
        if (f != it->second.floatTargets.end()) return f->second;
    }
    return -1.0f;
}

// 失效矩形 = 布局矩形 ∪ 动画（位移/绕中心缩放）后的包围盒 + pad。
// 少了这一步，:hover 放大 1.05 时只有布局矩形那块被重绘：长出来的边框/底色留在
// 旧缓冲区里，看上去就是"两边的边框被削掉一半"；缩回时旧位置还会留残影。
bool BinUIRenderer::invalidRect(uint16_t nodeId, float& x, float& y, float& w, float& h,
                                float pad) const {
    auto it = nodeIndexById_.find(nodeId);
    if (it == nodeIndexById_.end()) return false;
    const auto& n = nodes_[it->second];
    if (!(n.flags & BINUI_VISIBLE)) return false;

    float l = n.x, t = n.y, r = n.x + n.width, b = n.y + n.height;
    const float s = drawScale(nodeId);
    if (s != 1.0f && s > 0.0f) {
        const float cx = n.x + n.width * 0.5f, cy = n.y + n.height * 0.5f;
        const float hw = n.width * s * 0.5f, hh = n.height * s * 0.5f;
        l = std::min(l, cx - hw);
        t = std::min(t, cy - hh);
        r = std::max(r, cx + hw);
        b = std::max(b, cy + hh);
    }
    // 平移动画可能来回，两个方向都覆盖（否则回弹时会留下拖影）
    const float dx = drawOffsetX(nodeId), dy = drawOffsetY(nodeId);
    l += std::min(0.0f, dx);
    r += std::max(0.0f, dx);
    t += std::min(0.0f, dy);
    b += std::max(0.0f, dy);

    l -= pad; t -= pad; r += pad; b += pad;
    x = l; y = t; w = r - l; h = b - t;
    return true;
}

} // namespace binui
