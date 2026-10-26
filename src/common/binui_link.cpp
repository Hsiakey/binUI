// binui_link.cpp - 布局链接器实现（段解析 / 合并 / 重新序列化）
#include "binui_link.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace binui {
namespace {

// ============================================================
// 布局文档：把二进制拆成可合并的结构，再按 v12 段序重新写出
// ============================================================
struct Doc {
    uint32_t magic = BINUI_MAGIC;
    std::vector<BinUINode> nodes;
    std::string strings;             // 字符串表（'\0' 分隔，偏移即字节位置）

    std::vector<BinUISpecNode> specNodes;
    std::vector<BinUISpecFlex> specFlex;

    bool winPresent = false;
    BinUIWindowDesc win{};
    std::string winTitle;

    std::vector<std::vector<BinUIKeyFrame>> kfGroups;
    std::vector<BinUIAnimDef> animDefs;
    std::vector<BinUITransition> trans;

    std::vector<BinUIVideoDesc> videos;
    std::vector<BinUIStateStyle> states;
    std::vector<std::string> images;
    std::vector<ComponentRef> comps;
    std::vector<std::pair<uint16_t, std::string>> names;
};

const char* StrAt(const std::string& table, uint32_t off) {
    return off < table.size() ? table.c_str() + off : "";
}

// 字符串表内联：已存在则复用偏移，否则追加（'\0' 结尾）
uint32_t Intern(std::string& table, const std::string& s) {
    uint32_t off = 0;
    while (off < table.size()) {
        const char* p = table.c_str() + off;
        if (s == p) return off;
        off += static_cast<uint32_t>(std::strlen(p)) + 1;
    }
    const uint32_t at = static_cast<uint32_t>(table.size());
    table.append(s);
    table.push_back('\0');
    return at;
}

// ============================================================
// 解析：逐段读，遇到截断/不合法的段就当作"后面没有了"
// （手写 / GenerateDefaultBinUI 产出的布局可能缺段，渲染器同样容忍）
// ============================================================
class Reader {
public:
    Reader(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}
    size_t left() const { return static_cast<size_t>(end_ - p_); }
    const uint8_t* cur() const { return p_; }
    bool u32(uint32_t& v) {
        if (left() < sizeof(uint32_t)) return false;
        std::memcpy(&v, p_, sizeof(v));
        p_ += sizeof(v);
        return true;
    }
    bool blob(size_t n, const uint8_t*& out) {
        if (n > left()) return false;
        out = p_;
        p_ += n;
        return true;
    }
    void rewind(const uint8_t* to) { p_ = to; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

bool ParseDoc(const void* data, size_t size, Doc& d) {
    if (size < sizeof(BinUIHeader)) return false;
    const uint8_t* base = static_cast<const uint8_t*>(data);
    BinUIHeader h;
    std::memcpy(&h, base, sizeof(h));
    if (h.magic != BINUI_MAGIC && h.magic != BINUI_HTML_MAGIC) return false;
    if (h.version != BINUI_VERSION) return false;
    const size_t body = (size_t)h.nodeCount * sizeof(BinUINode) + (size_t)h.stringSize;
    if (size < sizeof(BinUIHeader) + body) return false;

    d.magic = h.magic;
    const uint8_t* np = base + sizeof(BinUIHeader);
    d.nodes.resize(h.nodeCount);
    if (h.nodeCount) std::memcpy(d.nodes.data(), np, (size_t)h.nodeCount * sizeof(BinUINode));
    const char* tbl = reinterpret_cast<const char*>(np + (size_t)h.nodeCount * sizeof(BinUINode));
    d.strings.assign(tbl, h.stringSize);

    Reader r(reinterpret_cast<const uint8_t*>(tbl) + h.stringSize, base + size);

    // ---- 缩放规格段 ----
    {
        const uint8_t* mark = r.cur();
        uint32_t specCount = 0, flexCount = 0;
        bool ok = false;
        if (r.u32(specCount) && specCount <= d.nodes.size()) {
            const uint8_t* sp = nullptr;
            if (r.blob((size_t)specCount * sizeof(BinUISpecNode), sp) && r.u32(flexCount)) {
                const uint8_t* fp = nullptr;
                if (r.blob((size_t)flexCount * sizeof(BinUISpecFlex), fp)) {
                    d.specNodes.assign(reinterpret_cast<const BinUISpecNode*>(sp),
                                       reinterpret_cast<const BinUISpecNode*>(sp) + specCount);
                    if (flexCount)
                        d.specFlex.assign(reinterpret_cast<const BinUISpecFlex*>(fp),
                                          reinterpret_cast<const BinUISpecFlex*>(fp) + flexCount);
                    ok = true;
                }
            }
        }
        if (!ok) r.rewind(mark); // 不是规格段（旧布局/手写），回退
    }

    // ---- 窗口描述段 ----
    {
        const uint8_t* mark = r.cur();
        uint32_t bytes = 0;
        bool ok = false;
        if (r.u32(bytes)) {
            if (bytes >= sizeof(BinUIWindowDesc)) {
                const uint8_t* wp = nullptr;
                if (r.blob(bytes, wp)) {
                    std::memcpy(&d.win, wp, sizeof(BinUIWindowDesc));
                    size_t tl = bytes - sizeof(BinUIWindowDesc);
                    if (d.win.titleLen < tl) tl = d.win.titleLen; // 防御：titleLen 不可信
                    d.winPresent = true;
                    d.winTitle.assign(reinterpret_cast<const char*>(wp) + sizeof(BinUIWindowDesc), tl);
                    ok = true;
                }
            } else if (bytes == 0) {
                ok = true; // 恒定写出的空段
            }
        }
        if (!ok) r.rewind(mark);
    }

    // ---- 动画段 ----
    {
        const uint8_t* mark = r.cur();
        uint32_t bytes = 0;
        bool ok = false;
        if (r.u32(bytes)) {
            if (bytes == 0) {
                ok = true;
            } else {
                const uint8_t* ap = nullptr;
                if (r.blob(bytes, ap)) {
                    Reader a(ap, ap + bytes);
                    uint32_t gc = 0;
                    bool good = a.u32(gc);
                    for (uint32_t g = 0; good && g < gc; g++) {
                        uint32_t kc = 0;
                        if (!a.u32(kc)) { good = false; break; }
                        const uint8_t* kp = nullptr;
                        if (!a.blob((size_t)kc * sizeof(BinUIKeyFrame), kp)) { good = false; break; }
                        std::vector<BinUIKeyFrame> kfs(reinterpret_cast<const BinUIKeyFrame*>(kp),
                                                       reinterpret_cast<const BinUIKeyFrame*>(kp) + kc);
                        d.kfGroups.push_back(std::move(kfs));
                    }
                    uint32_t ac = 0;
                    if (good && a.u32(ac)) {
                        const uint8_t* dp = nullptr;
                        if (ac == 0 || a.blob((size_t)ac * sizeof(BinUIAnimDef), dp)) {
                            if (ac)
                                d.animDefs.assign(reinterpret_cast<const BinUIAnimDef*>(dp),
                                                  reinterpret_cast<const BinUIAnimDef*>(dp) + ac);
                            uint32_t tc = 0;
                            if (a.u32(tc)) {
                                const uint8_t* tp = nullptr;
                                if (tc == 0 || a.blob((size_t)tc * sizeof(BinUITransition), tp)) {
                                    if (tc)
                                        d.trans.assign(reinterpret_cast<const BinUITransition*>(tp),
                                                       reinterpret_cast<const BinUITransition*>(tp) + tc);
                                    ok = true;
                                }
                            }
                        }
                    }
                }
                if (!ok) {
                    d.kfGroups.clear(); d.animDefs.clear(); d.trans.clear();
                    r.rewind(mark);
                }
            }
        }
        if (!ok) r.rewind(mark);
    }

    // ---- 视频描述段 ----
    {
        const uint8_t* mark = r.cur();
        uint32_t bytes = 0;
        bool ok = false;
        if (r.u32(bytes)) {
            if (bytes == 0) {
                ok = true;
            } else {
                const uint8_t* vp = nullptr;
                if (r.blob(bytes, vp)) {
                    Reader v(vp, vp + bytes);
                    uint32_t vc = 0;
                    if (v.u32(vc) && v.left() >= (size_t)vc * sizeof(BinUIVideoDesc)) {
                        const uint8_t* dp = nullptr;
                        if (v.blob((size_t)vc * sizeof(BinUIVideoDesc), dp)) {
                            if (vc)
                                d.videos.assign(reinterpret_cast<const BinUIVideoDesc*>(dp),
                                                reinterpret_cast<const BinUIVideoDesc*>(dp) + vc);
                            ok = true;
                        }
                    }
                }
                if (!ok) { d.videos.clear(); r.rewind(mark); }
            }
        }
        if (!ok) r.rewind(mark);
    }

    // ---- 状态样式段 ----
    {
        const uint8_t* mark = r.cur();
        uint32_t bytes = 0;
        bool ok = false;
        if (r.u32(bytes)) {
            if (bytes == 0) {
                ok = true;
            } else {
                const uint8_t* sp = nullptr;
                if (r.blob(bytes, sp)) {
                    Reader s(sp, sp + bytes);
                    uint32_t sc = 0;
                    if (s.u32(sc) && s.left() >= (size_t)sc * sizeof(BinUIStateStyle)) {
                        const uint8_t* ep = nullptr;
                        if (s.blob((size_t)sc * sizeof(BinUIStateStyle), ep)) {
                            if (sc)
                                d.states.assign(reinterpret_cast<const BinUIStateStyle*>(ep),
                                                reinterpret_cast<const BinUIStateStyle*>(ep) + sc);
                            ok = true;
                        }
                    }
                }
                if (!ok) { d.states.clear(); r.rewind(mark); }
            }
        }
        if (!ok) r.rewind(mark);
    }

    // ---- 资源段（v12） ----
    {
        const uint8_t* mark = r.cur();
        uint32_t bytes = 0;
        bool ok = false;
        if (r.u32(bytes)) {
            if (bytes == 0) {
                ok = true;
            } else {
                const uint8_t* ip = nullptr;
                if (r.blob(bytes, ip)) {
                    Reader i(ip, ip + bytes);
                    uint32_t ic = 0;
                    if (i.u32(ic) && i.left() >= (size_t)ic * sizeof(uint32_t)) {
                        ok = true;
                        for (uint32_t k = 0; k < ic; k++) {
                            uint32_t off = 0;
                            if (!i.u32(off)) { ok = false; break; }
                            d.images.emplace_back(StrAt(d.strings, off));
                        }
                    }
                }
                if (!ok) { d.images.clear(); r.rewind(mark); }
            }
        }
        if (!ok) r.rewind(mark);
    }

    // ---- 组件引用段（v12） ----
    {
        const uint8_t* mark = r.cur();
        uint32_t bytes = 0;
        bool ok = false;
        if (r.u32(bytes)) {
            if (bytes == 0) {
                ok = true;
            } else {
                const uint8_t* cp = nullptr;
                if (r.blob(bytes, cp)) {
                    Reader c(cp, cp + bytes);
                    uint32_t cc = 0;
                    if (c.u32(cc) && c.left() >= (size_t)cc * sizeof(BinUIComponentRef)) {
                        ok = true;
                        for (uint32_t k = 0; k < cc; k++) {
                            const uint8_t* ep = nullptr;
                            if (!c.blob(sizeof(BinUIComponentRef), ep)) { ok = false; break; }
                            BinUIComponentRef ref;
                            std::memcpy(&ref, ep, sizeof(ref));
                            ComponentRef cr;
                            cr.anchorId = ref.anchorId;
                            cr.src = StrAt(d.strings, ref.srcOffset);
                            d.comps.push_back(std::move(cr));
                        }
                    }
                }
                if (!ok) { d.comps.clear(); r.rewind(mark); }
            }
        }
        if (!ok) r.rewind(mark);
    }

    // ---- 名称段（v8，缺失 = 无命名节点） ----
    {
        uint32_t bytes = 0;
        if (r.u32(bytes) && bytes > 0) {
            const uint8_t* npp = nullptr;
            if (r.blob(bytes, npp)) {
                Reader n(npp, npp + bytes);
                uint32_t nc = 0;
                if (n.u32(nc)) {
                    for (uint32_t k = 0; k < nc && n.left() >= 8; k++) {
                        uint32_t id = 0, off = 0;
                        if (!n.u32(id) || !n.u32(off)) break;
                        d.names.emplace_back(static_cast<uint16_t>(id & 0xFFFF), StrAt(d.strings, off));
                    }
                }
            }
        }
    }
    return true;
}

// ============================================================
// 序列化：按 v12 段序写出（可选段恒定写出，名称段非空才写）
// ============================================================
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}
void PutRaw(std::vector<uint8_t>& b, const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    b.insert(b.end(), q, q + n);
}
void PutSection(std::vector<uint8_t>& b, const std::vector<uint8_t>& payload) {
    PutU32(b, static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) b.insert(b.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> EmitDoc(Doc& d) {
    // 资源 / 组件路径 / 名称需要落到字符串表里，先补齐表再算偏移
    std::vector<uint32_t> imgOffsets;
    imgOffsets.reserve(d.images.size());
    for (const auto& p : d.images) imgOffsets.push_back(Intern(d.strings, p));

    std::vector<uint32_t> compSrcOffsets;
    compSrcOffsets.reserve(d.comps.size());
    for (const auto& c : d.comps) compSrcOffsets.push_back(Intern(d.strings, c.src));

    std::vector<std::pair<uint32_t, uint32_t>> namePairs;
    namePairs.reserve(d.names.size());
    for (const auto& pr : d.names) namePairs.emplace_back(pr.first, Intern(d.strings, pr.second));

    BinUIHeader h;
    h.magic = d.magic;
    h.version = BINUI_VERSION;
    h.nodeCount = static_cast<uint32_t>(d.nodes.size());
    h.stringSize = static_cast<uint32_t>(d.strings.size());

    std::vector<uint8_t> b;
    b.reserve(sizeof(BinUIHeader) + d.nodes.size() * sizeof(BinUINode) + d.strings.size() + 256);
    PutRaw(b, &h, sizeof(h));
    if (!d.nodes.empty()) PutRaw(b, d.nodes.data(), d.nodes.size() * sizeof(BinUINode));
    if (!d.strings.empty()) PutRaw(b, d.strings.data(), d.strings.size());

    // 缩放规格段（注意：这段没有长度前缀，靠 count 自描述，与编译器/渲染器一致）
    {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(d.specNodes.size()));
        if (!d.specNodes.empty()) PutRaw(p, d.specNodes.data(), d.specNodes.size() * sizeof(BinUISpecNode));
        PutU32(p, static_cast<uint32_t>(d.specFlex.size()));
        if (!d.specFlex.empty()) PutRaw(p, d.specFlex.data(), d.specFlex.size() * sizeof(BinUISpecFlex));
        b.insert(b.end(), p.begin(), p.end());
    }
    // 窗口描述段
    {
        std::vector<uint8_t> p;
        if (d.winPresent) {
            BinUIWindowDesc w = d.win;
            w.titleLen = static_cast<uint16_t>(std::min<size_t>(d.winTitle.size(), 0xFFFF));
            PutRaw(p, &w, sizeof(w));
            if (w.titleLen) PutRaw(p, d.winTitle.data(), w.titleLen);
        }
        PutSection(b, p);
    }
    // 动画段
    {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(d.kfGroups.size()));
        for (const auto& g : d.kfGroups) {
            PutU32(p, static_cast<uint32_t>(g.size()));
            if (!g.empty()) PutRaw(p, g.data(), g.size() * sizeof(BinUIKeyFrame));
        }
        PutU32(p, static_cast<uint32_t>(d.animDefs.size()));
        if (!d.animDefs.empty()) PutRaw(p, d.animDefs.data(), d.animDefs.size() * sizeof(BinUIAnimDef));
        PutU32(p, static_cast<uint32_t>(d.trans.size()));
        if (!d.trans.empty()) PutRaw(p, d.trans.data(), d.trans.size() * sizeof(BinUITransition));
        PutSection(b, p);
    }
    // 视频描述段
    {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(d.videos.size()));
        if (!d.videos.empty()) PutRaw(p, d.videos.data(), d.videos.size() * sizeof(BinUIVideoDesc));
        PutSection(b, p);
    }
    // 状态样式段
    {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(d.states.size()));
        if (!d.states.empty()) PutRaw(p, d.states.data(), d.states.size() * sizeof(BinUIStateStyle));
        PutSection(b, p);
    }
    // 资源段
    {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(imgOffsets.size()));
        for (uint32_t off : imgOffsets) PutU32(p, off);
        PutSection(b, p);
    }
    // 组件引用段
    {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(d.comps.size()));
        for (size_t k = 0; k < d.comps.size(); k++) {
            BinUIComponentRef ref{};
            ref.anchorId = d.comps[k].anchorId;
            ref.srcOffset = compSrcOffsets[k];
            PutRaw(p, &ref, sizeof(ref));
        }
        PutSection(b, p);
    }
    // 名称段（非空才写）
    if (!namePairs.empty()) {
        std::vector<uint8_t> p;
        PutU32(p, static_cast<uint32_t>(namePairs.size()));
        for (const auto& pr : namePairs) {
            PutU32(p, pr.first);
            PutU32(p, pr.second);
        }
        PutSection(b, p);
    }
    return b;
}

} // namespace

// ============================================================
// 公开 API
// ============================================================
bool IsLayoutData(const void* data, size_t size) {
    if (!data || size < sizeof(BinUIHeader)) return false;
    BinUIHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != BINUI_MAGIC && h.magic != BINUI_HTML_MAGIC) return false;
    if (h.version != BINUI_VERSION) return false;
    return size >= sizeof(BinUIHeader) + (size_t)h.nodeCount * sizeof(BinUINode) + (size_t)h.stringSize;
}

uint32_t LayoutVersion(const void* data, size_t size) {
    if (!data || size < sizeof(BinUIHeader)) return 0;
    BinUIHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != BINUI_MAGIC && h.magic != BINUI_HTML_MAGIC) return 0;
    return h.version;
}

std::vector<std::string> ReadImagePaths(const void* data, size_t size) {
    Doc d;
    if (!ParseDoc(data, size, d)) return {};
    return d.images;
}

std::vector<ComponentRef> ReadComponentRefs(const void* data, size_t size) {
    Doc d;
    if (!ParseDoc(data, size, d)) return {};
    return d.comps;
}

bool MergeLayout(std::vector<uint8_t>& host, const void* comp, size_t compSize,
                 uint16_t anchorId, const LinkOptions* opts, LinkStats* stats, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        if (opts && opts->verbose) std::fprintf(stderr, "[binui] 布局合并失败：%s\n", m.c_str());
        return false;
    };
    Doc h, c;
    if (!ParseDoc(host.data(), host.size(), h)) return fail("宿主布局解析失败（magic/版本/长度）");
    if (!ParseDoc(comp, compSize, c)) return fail("组件布局解析失败（magic/版本/长度）");
    if (c.nodes.empty()) return fail("组件布局没有节点");

    size_t anchorIdx = SIZE_MAX;
    uint32_t maxId = 0;
    for (size_t i = 0; i < h.nodes.size(); i++) {
        if (h.nodes[i].id == anchorId) anchorIdx = i;
        if (h.nodes[i].id > maxId) maxId = h.nodes[i].id;
    }
    if (anchorIdx == SIZE_MAX) return fail("锚点节点 " + std::to_string(anchorId) + " 不在宿主布局里");
    if (maxId + c.nodes.size() + 1 > 0xFFF0)
        return fail("节点 id 空间不足（合并后超过 65535）");

    // ---- id 重映射 ----
    std::vector<uint16_t> idMap(c.nodes.size());
    std::unordered_map<uint16_t, size_t> compIdx;
    for (size_t i = 0; i < c.nodes.size(); i++) {
        idMap[i] = static_cast<uint16_t>(maxId + 1 + i);
        compIdx.emplace(c.nodes[i].id, i); // 重复 id 取第一个
    }
    const uint16_t compRootId = c.nodes[0].id;
    const uint16_t anchorPad = h.nodes[anchorIdx].padding;
    // 节点 x/y 是**绝对屏幕坐标**（外壳按 node.x/y 直接绘制，并裁剪到祖先矩形）。
    // 组件在自己的坐标系里从 (0,0) 起算，所以整体平移到锚点内容原点。
    const float originX = h.nodes[anchorIdx].x + anchorPad;
    const float originY = h.nodes[anchorIdx].y + anchorPad;
    (void)anchorPad;

    // ---- 字符串表：整体后移 ----
    const uint32_t strBase = static_cast<uint32_t>(h.strings.size());
    const uint32_t compStrSize = static_cast<uint32_t>(c.strings.size());
    h.strings += c.strings;
    auto shiftOff = [&](uint32_t off) -> uint32_t {
        // 组件里的偏移若越界，按"空文本"处理（0 指向表头）
        return off < compStrSize ? off + strBase : 0;
    };

    // ---- 相对资源路径重定基（组件目录 -> 宿主目录）----
    auto rebase = [&](const std::string& p) -> std::string {
        if (!opts || p.empty() || opts->compDir.empty()) return p;
        if ((p.size() > 1 && (p[0] == '/' || p[1] == ':')) || p[0] == '\\') return p; // 绝对路径
        if (opts->compDir == opts->hostDir) return p;
        std::error_code ec;
        const auto rel = std::filesystem::relative(
            std::filesystem::path(opts->compDir) / p,
            std::filesystem::path(opts->hostDir.empty() ? "." : opts->hostDir), ec);
        return ec ? p : rel.generic_string();
    };

    // ---- 资源下标重排 ----
    const uint16_t imageBase = static_cast<uint16_t>(h.images.size());
    for (auto& p : c.images) h.images.push_back(rebase(p));

    // ---- 节点：丢弃组件合成根，其子节点改挂锚点 ----
    uint16_t mergedRoot = 0;
    uint16_t mergedCount = 0;
    float bboxR = originX, bboxB = originY;
    for (size_t i = 1; i < c.nodes.size(); i++) {
        BinUINode n = c.nodes[i];
        n.id = idMap[i];
        n.textOffset = shiftOff(n.textOffset);
        if (n.type == BINUI_IMAGE || n.type == BINUI_VIDEO)
            n.imageIndex = static_cast<uint16_t>(n.imageIndex + imageBase);

        auto pit = compIdx.find(n.parentId);
        const bool toAnchor = (n.parentId == compRootId) || (pit == compIdx.end()) || (pit->second == 0);
        if (toAnchor) n.parentId = anchorId;
        else n.parentId = idMap[pit->second];

        // 组件坐标系 -> 宿主绝对坐标
        n.x += originX;
        n.y += originY;
        if (n.x + n.width > bboxR) bboxR = n.x + n.width;
        if (n.y + n.height > bboxB) bboxB = n.y + n.height;
        if (mergedCount == 0) mergedRoot = n.id;
        mergedCount++;
        h.nodes.push_back(n);
    }
    if (mergedCount == 0) return fail("组件布局只有合成根，没有可合并的节点");

    // 占位节点按组件包围盒撑大：外壳会按祖先矩形裁剪子节点，
    // 占位节点小于组件内容时组件会被裁掉一块（尤其 <use> 只给了 w 没给 h 的情况）
    {
        BinUINode& anc = h.nodes[anchorIdx];
        const float needW = bboxR - anc.x;
        const float needH = bboxB - anc.y;
        if (needW > anc.width) anc.width = needW;
        if (needH > anc.height) anc.height = needH;
    }

    // ---- 组件根覆盖（<use w=/h=/bg=/radius=>）----
    // 二进制组件的参数在编 .bum 时已固化，宿主还能改的是"外框"：尺寸/底色/圆角。
    if (opts && (opts->hasW || opts->hasH || opts->hasColor || opts->hasRadius)) {
        for (auto& n : h.nodes) {
            if (n.id != mergedRoot) continue;
            if (opts->hasW) n.width = opts->w;
            if (opts->hasH) n.height = opts->h;
            if (opts->hasColor) n.color = opts->color;
            if (opts->hasRadius) n.radius = static_cast<uint16_t>(opts->radius + 0.5f);
            break;
        }
        for (auto& s : h.specNodes) {
            if (s.nodeId != mergedRoot) continue;
            if (opts->hasW) s.relW = opts->w;
            if (opts->hasH) s.relH = opts->h;
            break;
        }
    }

    // ---- 缩放规格段 ----
    for (auto& s : c.specNodes) {
        auto it = compIdx.find(s.nodeId);
        if (it == compIdx.end() || it->second == 0) continue; // 合成根的规格丢弃
        BinUISpecNode s2 = s;
        s2.nodeId = idMap[it->second];
        h.specNodes.push_back(s2);
    }
    for (auto& f : c.specFlex) {
        auto it = compIdx.find(f.nodeId);
        if (it == compIdx.end() || it->second == 0) continue;
        BinUISpecFlex f2 = f;
        f2.nodeId = idMap[it->second];
        h.specFlex.push_back(f2);
    }

    // ---- 动画段：关键帧组整体后移，动画定义的组下标跟着补 ----
    {
        const uint16_t groupBase = static_cast<uint16_t>(h.kfGroups.size());
        for (auto& g : c.kfGroups) h.kfGroups.push_back(g);
        for (auto& d : c.animDefs) {
            auto it = compIdx.find(d.nodeId);
            if (it == compIdx.end() || it->second == 0) continue;
            BinUIAnimDef d2 = d;
            d2.nodeId = idMap[it->second];
            d2.groupIdx = static_cast<uint16_t>(d2.groupIdx + groupBase);
            h.animDefs.push_back(d2);
        }
        for (auto& t : c.trans) {
            auto it = compIdx.find(t.nodeId);
            if (it == compIdx.end() || it->second == 0) continue;
            BinUITransition t2 = t;
            t2.nodeId = idMap[it->second];
            h.trans.push_back(t2);
        }
    }

    // ---- 视频描述段 ----
    for (auto& v : c.videos) {
        auto it = compIdx.find(v.nodeId);
        if (it == compIdx.end() || it->second == 0) continue;
        BinUIVideoDesc v2 = v;
        v2.nodeId = idMap[it->second];
        v2.imageIndex = static_cast<uint16_t>(v2.imageIndex + imageBase);
        v2.srcOffset = Intern(h.strings, rebase(StrAt(c.strings, v.srcOffset)));
        h.videos.push_back(v2);
    }

    // ---- 状态样式段 ----
    for (auto& s : c.states) {
        auto it = compIdx.find(s.nodeId);
        if (it == compIdx.end() || it->second == 0) continue;
        BinUIStateStyle s2 = s;
        s2.nodeId = idMap[it->second];
        h.states.push_back(s2);
    }

    // ---- 组件引用段（组件内再外挂组件时递归） ----
    for (auto& ref : c.comps) {
        ComponentRef r2 = ref;
        r2.src = rebase(ref.src);
        auto it = compIdx.find(ref.anchorId);
        r2.anchorId = (it == compIdx.end())
                          ? anchorId
                          : (it->second == 0 ? anchorId : idMap[it->second]);
        h.comps.push_back(r2);
    }

    // ---- 名称段：同名以宿主为准 ----
    {
        std::unordered_map<std::string, uint16_t> existing;
        for (auto& pr : h.names) existing.emplace(pr.second, pr.first);
        for (auto& pr : c.names) {
            auto it = compIdx.find(pr.first);
            if (it == compIdx.end() || it->second == 0) continue;
            if (existing.count(pr.second)) continue;
            h.names.emplace_back(idMap[it->second], pr.second);
        }
    }

    std::vector<uint8_t> out = EmitDoc(h);
    host.swap(out);

    if (stats) {
        stats->rootId = mergedRoot;
        stats->nodeCount = mergedCount;
        stats->imageCount = static_cast<uint32_t>(h.images.size());
    }
    return true;
}

bool MergeLayoutFile(std::vector<uint8_t>& host, const std::string& compPath,
                     uint16_t anchorId, const LinkOptions* opts, LinkStats* stats, std::string* err) {
    std::ifstream in(compPath, std::ios::binary);
    if (!in) {
        if (err) *err = "读不到组件文件 " + compPath;
        return false;
    }
    std::vector<uint8_t> comp((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (comp.empty()) {
        if (err) *err = "组件文件为空 " + compPath;
        return false;
    }
    LinkOptions local;
    if (opts) local = *opts;
    if (local.compDir.empty())
        local.compDir = std::filesystem::path(compPath).parent_path().generic_string();
    return MergeLayout(host, comp.data(), comp.size(), anchorId, &local, stats, err);
}

} // namespace binui
