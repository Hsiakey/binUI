// test_css.cpp - CSS 属性 / 选择器 / 编译器回归（原 test_compiler）
//
// HTML5 style 段 -> binUI 布局：每个 CSS 属性落到节点字段的口径
// 选择器（单段）、伪元素 ::before/::after、结构伪类
// 编译器告警（未支持属性 / 非法值），以及 docs/issues 的回归用例
//
// 由原 tests/test_compiler.cpp 合并而来，逐条用例代码未改（见下面每个 namespace 的分节注释）。

#include "binui_compiler.h"
#include "binui_renderer.h"
#include "binui_format.h"
#include "binui_paint.h"     // BuildFillBands / PaintBand（渐变角度用例）
#include "binui_mip.h"       // BuildMipChainBGRA（v0.16.20：raw 纹理的 mip 链）
#include "binui_filter.h"    // ColorMatrix / ApplyColorMatrixBGRA（v26：CSS filter）
#include "binui_srcmap.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

// ---------------------------------------------------------------------------
// 原 tests/test_compiler.cpp（代码逐字保留，仅包进 namespace 并改名 run()）
// ---------------------------------------------------------------------------
#undef CHECK
#undef TEST
namespace suite_compiler {

static int failed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); \
        failed++; \
    } else { \
        printf("PASS: %s\n", name); \
    } \
} while(0)

using namespace binui;

static bool ParseInto(BinUIRenderer& r, const std::vector<uint8_t>& binui) {
    return !binui.empty() && r.loadLayout(binui.data(), binui.size());
}

// ============================================================
// docs/issues 回归：03 display:none 子树 / 04 按钮默认样式 /
//                    05 scrollbar-* 告警 / 06 heightAuto 标记
// ============================================================
static const BinUIRenderer::NodeInfo* NodeById(BinUIRenderer& r, uint16_t id) {
    for (const auto& n : r.getNodes())
        if (n.id == id) return &n;
    return nullptr;
}

static void TestIssueRegressions() {
    // ---- 0.16.13：linear-gradient 的角度（P1-6）----
    // 以前只认 `to right/bottom` 这类关键字，数字角度一律当默认（= 垂直），于是
    // `linear-gradient(135deg,…)` 被画成上下渐变。这里钉住三件事：
    //   1. 角度进布局（reserve[1] = 角度 + 1），轴对齐的仍走老三方向（像素不变）；
    //   2. 条带生成真的沿渐变轴变（同一行内颜色不同 = 斜向）；
    //   3. 单位换算（turn / rad）与 `to <corner>` 的映射。
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014">
<body>
  <div id="diag"   style="position:absolute; left:10; top:10;  width:200; height:120; background:linear-gradient(135deg, #FF0000, #0000FF)"></div>
  <div id="right"  style="position:absolute; left:10; top:150; width:200; height:60;  background:linear-gradient(90deg, #FF0000, #0000FF)"></div>
  <div id="down"   style="position:absolute; left:10; top:220; width:200; height:60;  background:linear-gradient(180deg, #FF0000, #0000FF)"></div>
  <div id="corner" style="position:absolute; left:230; top:10; width:150; height:100; background:linear-gradient(to bottom right, #FF0000, #0000FF)"></div>
  <div id="turn"   style="position:absolute; left:230; top:150; width:150; height:60; background:linear-gradient(0.5turn, #FF0000, #0000FF)"></div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.13: 布局可解析", ParseInto(r, bin));
        auto paintOfName = [&](const char* name) -> const BinUIPaintNode* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.paintOf(id) : nullptr;
        };
        const BinUIPaintNode* diag = paintOfName("diag");
        TEST("16.13: 135deg 写进 reserve[1]", diag && diag->reserve[1] == 136);
        TEST("16.13: 135deg 标记为渐变", diag && (diag->flags & PAINT_GRADIENT));
        const BinUIPaintNode* right = paintOfName("right");
        TEST("16.13: 90deg 走老的水平路径（reserve[1] == 0）",
             right && right->reserve[1] == 0 && right->gradDir == BINUI_GRAD_H);
        const BinUIPaintNode* down = paintOfName("down");
        TEST("16.13: 180deg 走老的垂直路径（像素不变）",
             down && down->reserve[1] == 0 && down->gradDir == BINUI_GRAD_V &&
             down->gradFrom == 0xFFFF0000u && down->gradTo == 0xFF0000FFu);
        const BinUIPaintNode* corner = paintOfName("corner");
        TEST("16.13: to bottom right = 135deg", corner && corner->reserve[1] == 136);
        const BinUIPaintNode* turn = paintOfName("turn");
        // 0.5turn = 180deg：和 `180deg` 完全等价（归一化时"角度 -180° + 两端色互换"
        // 走两步正好抵消），所以落到的还是老垂直路径、颜色顺序不变
        TEST("16.13: 0.5turn = 180deg → 老垂直路径",
             turn && turn->reserve[1] == 0 && turn->gradDir == BINUI_GRAD_V &&
             turn->gradFrom == 0xFFFF0000u && turn->gradTo == 0xFF0000FFu);

        // 条带：135deg 的同一行里颜色必须随 x 变（以前是整行一个色）
        if (diag) {
            PaintBand bands[kAngleGradBands];
            const int n = BuildFillBands(diag, 10, 10, 200, 120, 0, diag->gradFrom, bands,
                                         kAngleGradBands);
            TEST("16.13: 对角渐变条带数合理", n > 100 && n <= kAngleGradBands);
            bool rowHasHorizontalVariation = false;
            for (int i = 0; i + 1 < n && i < 400; i++) {
                const PaintBand& a = bands[i];
                const PaintBand& b = bands[i + 1];
                if (std::fabs(a.y - b.y) < 0.01f && a.x != b.x && a.color != b.color)
                    rowHasHorizontalVariation = true;
            }
            TEST("16.13: 同一行内颜色随 x 变（真斜向）", rowHasHorizontalVariation);
            // 左上角应接近起点色、右下角接近终点色
            uint32_t topLeft = 0, botRight = 0;
            float bestTL = 1e9f, bestBR = -1e9f;
            for (int i = 0; i < n; i++) {
                const PaintBand& b = bands[i];
                const float score = b.x + b.y;
                if (score < bestTL) { bestTL = score; topLeft = b.color; }
                if (score > bestBR) { bestBR = score; botRight = b.color; }
            }
            const int rTL = (int)((topLeft >> 16) & 0xFF), bTL = (int)(topLeft & 0xFF);
            const int rBR = (int)((botRight >> 16) & 0xFF), bBR = (int)(botRight & 0xFF);
            TEST("16.13: 左上角偏红", rTL > 150 && bTL < 110);
            TEST("16.13: 右下角偏蓝", bBR > 150 && rBR < 110);
        }
    }

    // ---- 0.16.14：box-shadow（v23 阴影段，P1-5）----
    // 钉住四件事：
    //   1. 解析落进阴影段（偏移/模糊/展开/颜色），颜色缺省 = 当前文字色；
    //   2. 多条阴影按"绘制顺序"存（作者先写的在最后 = 压在上面）；
    //   3. inset 只告警、不写出（不是画成外阴影）；
    //   4. 几何：高斯覆盖率 → 同心圆角环，环数合理、中心覆盖率到 1、分布单调。
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014">
<body>
  <div id="one"  style="position:absolute; left:10; top:10; width:120; height:80; border-radius:12px;
                         box-shadow: 0 8px 18px rgba(0,0,0,0.28)"></div>
  <div id="two"  style="position:absolute; left:150; top:10; width:120; height:80;
                         box-shadow: 0 2px 4px rgba(0,0,0,0.5), 4px 6px 0 #22FF8800"></div>
  <div id="cur"  style="position:absolute; left:290; top:10; width:120; height:80; color:#FF3366;
                         box-shadow: 2px 3px 6px"></div>
  <div id="ins"  style="position:absolute; left:10; top:150; width:120; height:80;
                         box-shadow: inset 0 2px 6px rgba(0,0,0,0.4)"></div>
  <div id="none" style="position:absolute; left:150; top:150; width:120; height:80;
                         box-shadow: none"></div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.14: 阴影布局可解析", ParseInto(r, bin));
        auto shadowsOfName = [&](const char* name, int& n) -> const BinUIShadow* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.shadowsOf(id, n) : nullptr;
        };
        int n1 = 0;
        const BinUIShadow* s1 = shadowsOfName("one", n1);
        TEST("16.14: 单条阴影", s1 && n1 == 1);
        if (s1) {
            TEST("16.14: 偏移/模糊解析", std::fabs(s1->offsetX) < 0.01f &&
                                         std::fabs(s1->offsetY - 8.0f) < 0.01f &&
                                         std::fabs(s1->blur - 18.0f) < 0.01f &&
                                         std::fabs(s1->spread) < 0.01f);
            TEST("16.14: 颜色与 alpha 解析",
                 ((s1->color >> 24) & 0xFF) == 71 && ((s1->color >> 16) & 0xFF) == 0);
        }
        int n2 = 0;
        const BinUIShadow* s2 = shadowsOfName("two", n2);
        TEST("16.14: 多条阴影各自成条", s2 && n2 == 2);
        if (s2 && n2 == 2) {
            // 作者先写的是"贴边硬影"（blur 4），它应该画在**后面**（数组末尾）
            TEST("16.14: 先写的压在上面（数组末尾）",
                 std::fabs(s2[1].blur - 4.0f) < 0.01f && std::fabs(s2[0].blur) < 0.01f &&
                 std::fabs(s2[1].offsetY - 2.0f) < 0.01f);
            TEST("16.14: 第二条的 0 模糊 + #AARRGGBB 颜色",
                 std::fabs(s2[0].offsetX - 4.0f) < 0.01f && ((s2[0].color >> 24) & 0xFF) == 0x22);
        }
        int nc = 0;
        const BinUIShadow* sc = shadowsOfName("cur", nc);
        TEST("16.14: 不写颜色 = 当前文字色（#FF3366）",
             sc && nc == 1 && ((sc->color >> 16) & 0xFF) == 0xFF &&
             ((sc->color >> 8) & 0xFF) == 0x33 && (sc->color & 0xFF) == 0x66);
        int ni = 0;
        TEST("16.14: inset 不写出（不画成外阴影）", shadowsOfName("ins", ni) == nullptr && ni == 0);
        int nn = 0;
        TEST("16.14: none = 无阴影", shadowsOfName("none", nn) == nullptr && nn == 0);

        // 几何：高斯覆盖率 → 环
        BinUIShadow g{};
        g.offsetY = 8.0f;
        g.blur = 18.0f;
        g.color = 0x47000000u;                         // 0.28 黑
        PaintShadowRing rings[kMaxShadowRings];
        const int rn = BuildShadowRings(g, 0.0f, 0.0f, 150.0f, 96.0f, 12.0f, 1.0f, rings);
        TEST("16.14: 环数在合理区间", rn >= 4 && rn <= kMaxShadowRings);
        if (rn > 0) {
            TEST("16.14: 中心环覆盖率 = 1", rings[rn - 1].cover > 0.999f);
            TEST("16.14: 最外环覆盖率很小", rings[0].cover < 0.05f);
            bool monotone = true;
            for (int i = 1; i < rn; i++)
                if (rings[i].cover < rings[i - 1].cover - 1e-4f) monotone = false;
            TEST("16.14: 由外向内覆盖率单调递增", monotone);
            // 累计 alpha 必须收敛到 A（校正过的 alpha 逐环叠加 = 目标覆盖率）
            float acc = 0.0f;
            for (int i = 0; i < rn; i++) {
                const float a = (float)((rings[i].color >> 24) & 0xFFu) / 255.0f;
                acc = a + acc * (1.0f - a);
            }
            TEST("16.14: 叠加后中心 alpha ≈ 0.28", std::fabs(acc - 0.28f) < 0.02f);
        }
        // 无模糊 = 一环硬边
        BinUIShadow hard{};
        hard.offsetX = 8.0f; hard.offsetY = 8.0f;
        hard.color = 0x80000000u;
        const int hn = BuildShadowRings(hard, 0.0f, 0.0f, 50.0f, 40.0f, 0.0f, 1.0f, rings);
        TEST("16.14: 0 模糊 = 一环", hn == 1 && std::fabs(rings[0].x - 8.0f) < 0.01f &&
                                     rings[0].color >> 24 == 0x80);
    }

    // ---- 0.16.15：图片能力（P1-8：圆角图片 / filter: brightness / mip 的几何部分）----
    // 钉住：
    //   1. `filter: brightness(0.5)` 落到 paint.reserve[2]（只乘暗，>1 忽略）；
    //   2. 不认识的滤镜只告警、不影响其它绘制；
    //   3. 圆角图片的横条几何：直角 1 条（像素中性）、圆角 2r+1 条、
    //      每条的内缩与 UV 子矩形一致（圆角区取源图内侧那一段，**不是**拉伸）；
    //   4. 条带在纵向无缝铺满（首尾相接、累计高度 = 图高）。
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014">
<body>
  <img id="imgA" src="a.png" style="position:absolute; left:10; top:10; width:96; height:96; border-radius:24" />
  <img id="imgB" src="a.png" style="position:absolute; left:120; top:10; width:96; height:96; filter:brightness(0.5)" />
  <img id="imgC" src="a.png" style="position:absolute; left:230; top:10; width:96; height:96; filter:brightness(180%)" />
  <img id="imgD" src="a.png" style="position:absolute; left:10; top:120; width:96; height:96; filter:grayscale(1) brightness(0.25)" />
</body></html>
</app_window>)HTML";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.15: 图片布局可解析", ParseInto(r, res.layout));
        auto paintOfName = [&](const char* name) -> const BinUIPaintNode* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.paintOf(id) : nullptr;
        };
        const BinUIPaintNode* pb = paintOfName("imgB");
        TEST("16.15: brightness(0.5) 进 reserve[2] = 128", pb && pb->reserve[2] == 128);
        const BinUIPaintNode* pc = paintOfName("imgC");
        TEST("16.15: brightness(180%) 不提亮（reserve[2] 保持 0）", pc == nullptr || pc->reserve[2] == 0);
        const BinUIPaintNode* pd = paintOfName("imgD");
        TEST("16.15: grayscale 忽略但 brightness 仍生效", pd && pd->reserve[2] == 64);
        // 只写了 filter 的节点也必须进扩展绘制段（否则渲染器读不到系数）
        TEST("16.15: 只有 filter 也生成绘制条目", pb != nullptr);

        // ---- 圆角图片的横条几何 ----
        PaintImageBand bands[kMaxImageBands];
        int n = BuildImageBands(0, 0, 96, 96, 0, bands);
        TEST("16.15: 直角 = 一条整图", n == 1 && bands[0].u0 == 0.0f && bands[0].u1 == 1.0f &&
                                        bands[0].v0 == 0.0f && bands[0].v1 == 1.0f);
        n = BuildImageBands(10, 20, 96, 96, 24, bands);
        TEST("16.15: 圆角条带数 = 2r+1", n == 49);
        if (n > 1) {
            TEST("16.15: 首条在下圆角区：有内缩 + UV 取内侧",
                 bands[0].x > 10.0f && bands[0].w < 96.0f &&
                 bands[0].u0 > 0.0f && std::fabs(bands[0].u1 - (1.0f - bands[0].u0)) < 1e-5f);
            TEST("16.15: 首条从图顶开始", std::fabs(bands[0].y - 20.0f) < 1e-4f);
            bool middleFull = false;      // 中间那条是整宽
            for (int i = 0; i < n; i++)
                if (std::fabs(bands[i].w - 96.0f) < 1e-4f && bands[i].u0 == 0.0f)
                    middleFull = true;
            TEST("16.15: 直边区一条整宽（u 0..1）", middleFull);
            float sum = 0.0f, lastEnd = bands[0].y;
            bool contiguous = true;
            for (int i = 0; i < n; i++) {
                if (std::fabs(bands[i].y - lastEnd) > 1e-4f) contiguous = false;
                lastEnd = bands[i].y + bands[i].h;
                sum += bands[i].h;
            }
            TEST("16.15: 纵向首尾相接", contiguous);
            TEST("16.15: 纵向铺满整图高", std::fabs(sum - 96.0f) < 0.01f);
        }
        // 半径大到装不下时不能"只画一半"
        n = BuildImageBands(0, 0, 40, 40, 200, bands);
        float sum2 = 0.0f;
        for (int i = 0; i < n; i++) sum2 += bands[i].h;
        TEST("16.15: 超大半径仍铺满", n > 0 && n <= kMaxImageBands && std::fabs(sum2 - 40.0f) < 0.01f);
    }

    // ---- 0.16.16：text-shadow（v24 文字阴影段，P1-11 的配套）----
    // 钉住六件事：
    //   1. 三个长度按顺序落进 offset-x / offset-y / blur（没有 spread、没有 inset）；
    //   2. 颜色缺省 = 当前文字色（currentColor 两种写法都认）；
    //   3. **可继承**：容器上写一次，子树里真正画字的节点各有一条（值相同）；
    //   4. `text-shadow: none` 从那里开始清掉（子树不再有）；
    //   5. 只有真会画字的节点才写条目（空节点/容器不写，段保持稀疏）；
    //   6. 一层上限 + 负 blur 收敛到 0（含 4 个长度时多出来的忽略）。
    {
        const char* html = R"HTML(<app_window width="420" height="320" bg="#101014">
<html width="420" height="320" bg="#101014">
<body>
  <span id="one"   style="position:absolute; left:10; top:10; font-size:16px;
                          text-shadow: 0 2px 6px rgba(0,0,0,0.55)">hello</span>
  <span id="cur"   style="position:absolute; left:10; top:40; font-size:16px; color:#FF3366;
                          text-shadow: 1px 1px">current</span>
  <span id="curkw" style="position:absolute; left:10; top:70; font-size:16px; color:#22CC88;
                          text-shadow: 0 3px 4px currentColor">keyword</span>
  <div id="card" style="position:absolute; left:10; top:100; width:400; height:120;
                        text-shadow: 0 2px 5px rgba(0,0,0,0.40)">
    <span id="kid" style="position:absolute; left:8; top:8; font-size:16px">inherited</span>
    <span id="kidnone" style="position:absolute; left:8; top:40; font-size:16px;
                              text-shadow: none">cleared</span>
    <div id="emptybox" style="position:absolute; left:8; top:70; width:60; height:30"></div>
  </div>
  <span id="four"  style="position:absolute; left:10; top:240; font-size:16px;
                          text-shadow: 2px 3px 4px 9px rgba(0,0,0,0.5)">four</span>
  <span id="neg"   style="position:absolute; left:120; top:240; font-size:16px;
                          text-shadow: 2px 3px -5px #000000">neg</span>
  <span id="multi" style="position:absolute; left:220; top:240; font-size:16px;
                          text-shadow: 0 1px 2px #000000, 0 4px 10px #FF0000">multi</span>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.16: 文字阴影布局可解析", ParseInto(r, bin));
        auto tsOfName = [&](const char* name, int& n) -> const BinUITextShadow* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.textShadowsOf(id, n) : nullptr;
        };
        int n1 = 0;
        const BinUITextShadow* s1 = tsOfName("one", n1);
        TEST("16.16: 单条文字阴影", s1 && n1 == 1);
        if (s1) {
            TEST("16.16: offset/blur 解析", std::fabs(s1->offsetX) < 0.01f &&
                                            std::fabs(s1->offsetY - 2.0f) < 0.01f &&
                                            std::fabs(s1->blur - 6.0f) < 0.01f);
            TEST("16.16: 颜色与 alpha 解析",
                 ((s1->color >> 24) & 0xFF) == 140 && ((s1->color >> 16) & 0xFF) == 0);
        }
        int nc = 0;
        const BinUITextShadow* sc = tsOfName("cur", nc);
        TEST("16.16: 不写颜色 = 当前文字色（#FF3366）",
             sc && nc == 1 && ((sc->color >> 16) & 0xFF) == 0xFF &&
             ((sc->color >> 8) & 0xFF) == 0x33 && (sc->color & 0xFF) == 0x66);
        TEST("16.16: 只给两个长度 = 硬阴影（blur 0）", sc && std::fabs(sc->blur) < 0.01f);
        int nk = 0;
        const BinUITextShadow* sk = tsOfName("curkw", nk);
        TEST("16.16: currentColor 关键字 = 自身文字色",
             sk && nk == 1 && ((sk->color >> 16) & 0xFF) == 0x22 &&
             ((sk->color >> 8) & 0xFF) == 0xCC && (sk->color & 0xFF) == 0x88);
        // 继承：card 上有，kid 上也必须有同样的值
        int ncard = 0, nkid = 0;
        const BinUITextShadow* scard = tsOfName("card", ncard);
        const BinUITextShadow* skid = tsOfName("kid", nkid);
        TEST("16.16: 容器本身没文字 → 不写条目", scard == nullptr && ncard == 0);
        TEST("16.16: 子元素继承父级文字阴影", skid && nkid == 1);
        if (skid && s1) {
            TEST("16.16: 继承值一致", std::fabs(skid->offsetY - 2.0f) < 0.01f &&
                                      std::fabs(skid->blur - 5.0f) < 0.01f);
        }
        int nn = 0;
        TEST("16.16: none 清掉继承的阴影", tsOfName("kidnone", nn) == nullptr && nn == 0);
        int ne = 0;
        TEST("16.16: 没有文字的节点不写条目", tsOfName("emptybox", ne) == nullptr && ne == 0);
        int n4 = 0;
        const BinUITextShadow* s4 = tsOfName("four", n4);
        TEST("16.16: 多余的第四个长度被忽略（blur 取第三个）",
             s4 && n4 == 1 && std::fabs(s4->blur - 4.0f) < 0.01f &&
             std::fabs(s4->offsetX - 2.0f) < 0.01f);
        int ng = 0;
        const BinUITextShadow* sg = tsOfName("neg", ng);
        TEST("16.16: 负 blur 收敛到 0", sg && ng == 1 && std::fabs(sg->blur) < 0.01f);
        int nm = 0;
        const BinUITextShadow* sm = tsOfName("multi", nm);
        // v0.16.50：多层不再截断。段的数组顺序 = 绘制顺序，所以作者写的第二层
        // （红、blur 10）在下标 0，第一层（黑、blur 2）在后 —— 详细断言见下面的 16.50 段。
        TEST("16.50: 两层都进段（段序 = 绘制顺序）",
             sm && nm == 2 && std::fabs(sm[0].blur - 10.0f) < 0.01f &&
             std::fabs(sm[1].blur - 2.0f) < 0.01f);
    }

    // ---- 0.16.50：多层 text-shadow（P1 表里最后一条功能尾巴）----
    // 钉住六件事：
    //   1. 三层按"书写顺序的逆序"进段（段序 = 绘制顺序，第一个写的最后画 = 在最上面）；
    //   2. **可继承**：容器写两层，子树里的文字节点各拿到同样两层（顺序也一致）；
    //   3. `none` 从该元素起清空整棵子树；
    //   4. 层数上限 BINUI_MAX_TEXT_SHADOWS：第 5 层起点名告警并忽略；
    //   5. 全透明的层根本不占条目（省一份模糊图集 / 一张 GDI 位图）；
    //   6. 外扩量按**最大**的一层算（裁剪与脏矩形都用它，少算了会留残影）。
    {
        const char* html = R"HTML(<app_window width="420" height="320" bg="#101014">
<html width="420" height="320" bg="#101014">
<body>
  <span id="three" style="position:absolute; left:10; top:10; font-size:16px; color:#FFFFFF;
        text-shadow: 0 0 2px #FF0000, 0 0 6px #00FF00, 0 0 12px #0000FF">three</span>
  <div id="card2" style="position:absolute; left:10; top:40; width:400; height:120;
        text-shadow: 0 1px 2px rgba(0,0,0,0.60), 0 4px 9px rgba(255,0,0,0.50)">
    <span id="kid2" style="position:absolute; left:8; top:8; font-size:16px">inherit</span>
    <span id="kid2none" style="position:absolute; left:8; top:40; font-size:16px;
          text-shadow:none">none</span>
  </div>
  <span id="five" style="position:absolute; left:10; top:170; font-size:16px;
        text-shadow: 0 1px 1px #010101, 0 2px 2px #020202, 0 3px 3px #030303, 0 4px 4px #040404, 0 5px 5px #050505">five</span>
  <span id="ghost" style="position:absolute; left:220; top:170; font-size:16px; color:#FFFFFF;
        text-shadow: 0 0 4px rgba(0,0,0,0), 0 2px 3px #22CC88, 0 1px 0 currentColor">ghost</span>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.50: 多层文字阴影布局可解析", ParseInto(r, res.layout));
        auto tsOfName = [&](const char* name, int& n) -> const BinUITextShadow* {
            const uint16_t id = r.findNodeByName(name);
            n = 0;
            return id ? r.textShadowsOf(id, n) : nullptr;
        };
        // 1) 三层：逆序进段
        int n3 = 0;
        const BinUITextShadow* s3 = tsOfName("three", n3);
        TEST("16.50: 三层都进段", s3 && n3 == 3);
        if (s3 && n3 == 3) {
            TEST("16.50: 段序 = 书写顺序的逆序（blur 12 / 6 / 2）",
                 std::fabs(s3[0].blur - 12.0f) < 0.01f &&
                 std::fabs(s3[1].blur - 6.0f) < 0.01f &&
                 std::fabs(s3[2].blur - 2.0f) < 0.01f);
            TEST("16.50: 每层颜色各自独立（蓝 / 绿 / 红）",
                 (s3[0].color & 0x00FFFFFFu) == 0x0000FFu &&
                 (s3[1].color & 0x00FFFFFFu) == 0x00FF00u &&
                 (s3[2].color & 0x00FFFFFFu) == 0xFF0000u);
            // 6) 外扩按最大层：blur 12 → σ=6 → 6*3+1 = 19
            float px = 0.0f, py = 0.0f;
            for (int k = 0; k < n3; k++) binui::PaintTextShadowPad(s3[k], px, py);
            TEST("16.50: 外扩量取最大层（19px）",
                 std::fabs(px - 19.0f) < 0.01f && std::fabs(py - 19.0f) < 0.01f);
        }
        // 2) 继承两层
        int nc2 = 0, nk2 = 0;
        const BinUITextShadow* sc2 = tsOfName("card2", nc2);
        const BinUITextShadow* sk2 = tsOfName("kid2", nk2);
        TEST("16.50: 容器没文字 → 不写条目", sc2 == nullptr && nc2 == 0);
        TEST("16.50: 子元素继承两层", sk2 && nk2 == 2);
        if (sk2 && nk2 == 2) {
            TEST("16.50: 继承的两层顺序与值都一致",
                 std::fabs(sk2[0].blur - 9.0f) < 0.01f &&
                 std::fabs(sk2[0].offsetY - 4.0f) < 0.01f &&
                 ((sk2[0].color >> 24) & 0xFFu) == 128 &&
                 std::fabs(sk2[1].blur - 2.0f) < 0.01f &&
                 std::fabs(sk2[1].offsetY - 1.0f) < 0.01f &&
                 ((sk2[1].color >> 24) & 0xFFu) == 153);
        }
        // 3) none 清空
        int nkn = 0;
        TEST("16.50: none 清掉继承的多层", tsOfName("kid2none", nkn) == nullptr && nkn == 0);
        // 4) 层数上限
        int n5 = 0;
        const BinUITextShadow* s5 = tsOfName("five", n5);
        TEST("16.50: 超过上限截到 4 层", s5 && n5 == BINUI_MAX_TEXT_SHADOWS);
        if (s5 && n5 >= 4) {
            // authored 前四层 = blur 1..4，逆序进段 → 段首是 blur 4
            TEST("16.50: 保留的是**前**四层（blur 4 / 3 / 2 / 1）",
                 std::fabs(s5[0].blur - 4.0f) < 0.01f && std::fabs(s5[1].blur - 3.0f) < 0.01f &&
                 std::fabs(s5[2].blur - 2.0f) < 0.01f && std::fabs(s5[3].blur - 1.0f) < 0.01f);
        }
        bool warnedCap = false;
        for (const auto& w : res.warnings)
            if (w.find("最多支持 4 层") != std::string::npos) warnedCap = true;
        TEST("16.50: 第 5 层点名告警", warnedCap);
        // 5) 全透明的层不占条目；currentColor 逐层解析
        int ng = 0;
        const BinUITextShadow* sg = tsOfName("ghost", ng);
        TEST("16.50: 全透明的层被丢掉（3 层写法 → 2 层条目）", sg && ng == 2);
        if (sg && ng == 2) {
            TEST("16.50: 剩下的两层顺序正确（硬阴影 currentColor 在前）",
                 std::fabs(sg[0].blur) < 0.01f &&
                 (sg[0].color & 0x00FFFFFFu) == 0xFFFFFFu &&
                 std::fabs(sg[1].blur - 3.0f) < 0.01f &&
                 (sg[1].color & 0x00FFFFFFu) == 0x22CC88u);
        }
    }

    // ---- 0.16.19：九宫格图片（v25 绘制附加段，P1-8 的尾巴）----
    // 钉住四件事：
    //   1. `image-slice: <n>` 落进段里（renderer.imageSliceOf），与 border-radius 互斥
    //      （九宫格优先，圆角被清成 0），百分比忽略、超 255 钳制、四值不等取第一个；
    //   2. 切分几何：9 块、四角保尺寸（= 内缩）、四边与中心拉伸、UV 与目标块一一对应；
    //   3. 内缩被**两端**钳制（源图半张 / 目标盒半宽），不会切出负宽度；
    //   4. 没有 `image-slice` 的图片完全走老路径（像素中性）。
    {
        const char* html = R"HTML(<app_window width="420" height="320" bg="#101014">
<html width="420" height="320" bg="#101014">
<body>
  <img id="plain" src="a.png" style="position:absolute; left:10; top:10; width:180; height:70" />
  <img id="nine"  src="a.png" style="position:absolute; left:10; top:90; width:180; height:70; image-slice:12" />
  <img id="both"  src="a.png" style="position:absolute; left:10; top:170; width:180; height:70;
                                     image-slice:12; border-radius:20" />
  <img id="pct"   src="a.png" style="position:absolute; left:10; top:250; width:180; height:70;
                                     image-slice:10%" />
  <img id="big"   src="a.png" style="position:absolute; left:200; top:10; width:180; height:70;
                                     image-slice:900" />
  <img id="four"  src="a.png" style="position:absolute; left:200; top:90; width:180; height:70;
                                     image-slice:8 12 16 20" />
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.19: 九宫格布局可解析", ParseInto(r, bin));
        auto sliceOfName = [&](const char* name) -> int {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.imageSliceOf(id) : -1;
        };
        TEST("16.19: 没写 = 0（普通拉伸）", sliceOfName("plain") == 0);
        TEST("16.19: image-slice:12 → 12", sliceOfName("nine") == 12);
        TEST("16.19: 与 border-radius 互斥（九宫格优先）", sliceOfName("both") == 12);
        {
            const uint16_t bid = r.findNodeByName("both");
            int rad = -1;
            for (const auto& ni : r.getNodes())
                if (ni.id == bid) rad = static_cast<int>(ni.radius);
            TEST("16.19: 同时写圆角时圆角被清成 0", bid != 0 && rad == 0);
        }
        TEST("16.19: 百分比忽略 → 0", sliceOfName("pct") == 0);
        TEST("16.19: 超过 255 钳制", sliceOfName("big") == 255);
        TEST("16.19: 四值不等取第一个", sliceOfName("four") == 8);

        // ---- 切分几何 ----
        float q[9][8];
        int n = binui::BuildNineSlice(0.0f, 0.0f, 180.0f, 70.0f, 48, 48, 12.0f, q);
        TEST("16.19: 9 块", n == 9);
        if (n == 9) {
            // 0=左上 1=上中 2=右上 3=左中 4=中心 5=右中 6=左下 7=下中 8=右下
            TEST("16.19: 左上角保尺寸（12×12，UV 0..0.25）",
                 std::fabs(q[0][4]) < 1e-4f && std::fabs(q[0][5]) < 1e-4f &&
                 std::fabs(q[0][6] - 12.0f) < 1e-4f && std::fabs(q[0][7] - 12.0f) < 1e-4f &&
                 std::fabs(q[0][0]) < 1e-6f && std::fabs(q[0][2] - 0.25f) < 1e-5f &&
                 std::fabs(q[0][3] - 0.25f) < 1e-5f);
            TEST("16.19: 中心块拉伸（180-24 × 70-24，UV 0.25..0.75）",
                 std::fabs(q[4][4] - 12.0f) < 1e-4f && std::fabs(q[4][6] - 156.0f) < 1e-4f &&
                 std::fabs(q[4][5] - 12.0f) < 1e-4f && std::fabs(q[4][7] - 46.0f) < 1e-4f &&
                 std::fabs(q[4][0] - 0.25f) < 1e-5f && std::fabs(q[4][2] - 0.75f) < 1e-5f);
            // 每一行的宽度之和 = 盒宽；每一列的 UV 上下相邻不重叠
            bool widthsOk = true, uvsOk = true;
            for (int row = 0; row < 3; row++) {
                float sum = 0.0f;
                for (int c = 0; c < 3; c++) sum += q[row * 3 + c][6];
                if (std::fabs(sum - 180.0f) > 1e-3f) widthsOk = false;
            }
            for (int row = 0; row < 3; row++) {
                if (std::fabs(q[row * 3 + 0][2] - q[row * 3 + 1][0]) > 1e-6f) uvsOk = false;
                if (std::fabs(q[row * 3 + 1][2] - q[row * 3 + 2][0]) > 1e-6f) uvsOk = false;
            }
            TEST("16.19: 每行宽度之和 = 盒宽", widthsOk);
            TEST("16.19: 横向 UV 首尾相接", uvsOk);
            TEST("16.19: 首块 UV 从 0 开始、末块到 1",
                 std::fabs(q[2][2] - 1.0f) < 1e-6f && std::fabs(q[8][3] - 1.0f) < 1e-6f);
        }
        // 内缩超过半张图：按半张钳制（两块角不能重叠）
        n = binui::BuildNineSlice(0.0f, 0.0f, 100.0f, 100.0f, 20, 20, 99.0f, q);
        TEST("16.19: 内缩钳到源图半张", n > 0 && std::fabs(q[0][6] - 50.0f) < 1e-4f);
        // 目标盒比内缩还小：按半盒钳制，仍是 9 块且不出现负宽
        n = binui::BuildNineSlice(0.0f, 0.0f, 10.0f, 8.0f, 48, 48, 12.0f, q);
        bool positive = n > 0;
        for (int i = 0; i < n; i++) if (q[i][6] <= 0.0f || q[i][7] <= 0.0f) positive = false;
        TEST("16.19: 盒比内缩小时不出现负宽", positive && n <= 9);
        // 退化输入
        TEST("16.19: 退化输入返回 0", binui::BuildNineSlice(0, 0, 0, 70, 48, 48, 12, q) == 0 &&
                                      binui::BuildNineSlice(0, 0, 180, 70, 0, 48, 12, q) == 0 &&
                                      binui::BuildNineSlice(0, 0, 180, 70, 48, 48, 0, q) == 0);
    }

    // ---- 0.16.49：九宫格中段平铺（CSS border-image-repeat）----
    // 钉住四件事：
    //   1. 两轴各两位落进 BinUIPaintEx::flags（renderer.imageSliceRepeatOf）；
    //   2. stretch 与老行为**逐字节相同**（块数/顺序/数值都不变）；
    //   3. repeat：按"自然尺寸 = 内缩"平铺，末块裁切且**源 UV 同比例裁**；
    //   4. round：块数取整、块长等分（不留裁切边）。
    {
        const char* html = R"HTML(<app_window width="420" height="240" bg="#101014">
<html width="420" height="240" bg="#101014">
<body>
  <img id="st" src="a.png" style="position:absolute; left:10; top:10; width:180; height:70; image-slice:12" />
  <img id="rx" src="a.png" style="position:absolute; left:10; top:90; width:180; height:70; image-slice:12;
                                      border-image-repeat:repeat" />
  <img id="ry" src="a.png" style="position:absolute; left:10; top:170; width:180; height:70; image-slice:12;
                                      border-image-repeat:stretch round" />
  <img id="rb" src="a.png" style="position:absolute; left:210; top:10; width:180; height:70; image-slice:12;
                                      border-image-repeat:repeat round" />
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.49: 九宫格布局可解析", ParseInto(r, bin));
        auto repOf = [&](const char* name) -> int {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.imageSliceRepeatOf(id) : -1;
        };
        TEST("16.49: 没写 = stretch/stretch(0)", repOf("st") == 0);
        TEST("16.49: 一个值两轴都生效", repOf("rx") == (1 | (1 << 2)));
        TEST("16.49: 两个值分别管 x/y", repOf("ry") == (0 | (2 << 2)));
        TEST("16.49: repeat/round 混写", repOf("rb") == (1 | (2 << 2)));

        // ---- 几何：stretch 与老的三段调用逐字节相同 ----
        float a9[9][8], b9[9][8];
        const int na = binui::BuildNineSlice(0, 0, 180, 70, 48, 48, 12, a9);
        const int nb = binui::BuildNineSlice(0, 0, 180, 70, 48, 48, 12, 0, 0, &b9[0][0], 9);
        bool same = (na == nb && na == 9);
        for (int i = 0; same && i < na; i++)
            for (int k = 0; k < 8; k++)
                if (a9[i][k] != b9[i][k]) same = false;
        TEST("16.49: stretch 与老行为逐字节相同", same);

        // ---- repeat：180 宽、内缩 12 → 中段 156 = 13×12（无末块）
        // 4 角 + 上下各 13 + 左右各 1 + 中心 13 = 45
        static float q[binui::kMaxNineSliceQuads * 8];
        int n = binui::BuildNineSlice(0, 0, 180, 70, 48, 48, 12, 1, 0, q, binui::kMaxNineSliceQuads);
        TEST("16.49: 横向 repeat 的块数", n == 45);
        bool topOk = false;
        for (int i = 0; i < n; i++) {
            const float* o = q + i * 8;
            // 上边第一条：x=12、12×12、UV 中间段的开头 0.25..0.5
            if (std::fabs(o[4] - 12.0f) < 1e-4f && std::fabs(o[5]) < 1e-4f &&
                std::fabs(o[6] - 12.0f) < 1e-4f && std::fabs(o[7] - 12.0f) < 1e-4f &&
                std::fabs(o[0] - 0.25f) < 1e-5f && std::fabs(o[2] - 0.75f) < 1e-5f) topOk = true;
        }
        TEST("16.49: repeat 的块是自然尺寸，UV 对应同一段", topOk);

        // 盒宽 175 → 中段 151 = 12×12 + 末块 7（UV 只取 7/12）
        n = binui::BuildNineSlice(0, 0, 175, 70, 48, 48, 12, 1, 0, q, binui::kMaxNineSliceQuads);
        bool tailOk = false;
        for (int i = 0; i < n; i++) {
            const float* o = q + i * 8;
            if (std::fabs(o[6] - 7.0f) < 1e-3f && std::fabs(o[7] - 12.0f) < 1e-3f &&
                std::fabs((o[2] - o[0]) - (0.5f * 7.0f / 12.0f)) < 1e-5f) tailOk = true;
        }
        TEST("16.49: repeat 末块按比例裁切（目标与源 UV 一起裁）", tailOk);

        // ---- round：盒宽 170 → 中段 146 → 12 块（146/12=12.17 取整），块长等分
        n = binui::BuildNineSlice(0, 0, 170, 70, 48, 48, 12, 2, 0, q, binui::kMaxNineSliceQuads);
        int topRow = 0;
        for (int i = 0; i < n; i++) {
            const float* o = q + i * 8;
            if (std::fabs(o[5]) < 1e-4f && std::fabs(o[7] - 12.0f) < 1e-4f) topRow++;
        }
        TEST("16.49: round 的块数按取整分配（顶行 1+12+1）", topRow == 14);
        // 预算护栏：极窄的自然宽度不该炸出成百上千块
        n = binui::BuildNineSlice(0, 0, 400, 400, 48, 48, 4, 1, 1, q, binui::kMaxNineSliceQuads);
        TEST("16.49: 双轴 repeat 不超过预算", n > 0 && n <= binui::kMaxNineSliceQuads);
    }

    // ---- 0.16.51：九宫格 space（border-image-repeat 的最后一个模式）----
    // 钉住五件事：
    //   1. `space` 落成模式 3（两轴、单值/双值都认）；
    //   2. 块数 = floor(中段 / 自然尺寸)，块**不缩放、不裁切**（块长恒等于内缩）；
    //   3. 余数均分成 **块数 + 1** 个间隙（两端各一个 —— 规范原话 "distributed around
    //      the tiles"），所以首块不吃着角、末块也不贴着角；
    //   4. 中段比一块还窄时没有余数可均分 → 唯一一块缩到中段长（"不被裁切"）；
    //   5. 与 round 的区别：round 改块长（等分、无缝隙），space 改缝隙（块长不动）。
    {
        const char* html = R"HTML(<app_window width="420" height="240" bg="#101014">
<html width="420" height="240" bg="#101014">
<body>
  <img id="sp" src="a.png" style="position:absolute; left:10; top:10; width:180; height:70; image-slice:12;
                                      border-image-repeat:space" />
  <img id="sp2" src="a.png" style="position:absolute; left:10; top:90; width:180; height:70; image-slice:12;
                                       border-image-repeat:space round" />
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.51: space 布局可解析", ParseInto(r, bin));
        auto repOf = [&](const char* name) -> int {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.imageSliceRepeatOf(id) : -1;
        };
        TEST("16.51: space 两轴都生效（模式 3）", repOf("sp") == (3 | (3 << 2)));
        TEST("16.51: space / round 混写",
             repOf("sp2") == (3 | (2 << 2)));

        // ---- 单轴数学：中段 [0,356)、自然 12 → 29 块 + 30 个缝（每个 8/30）----
        binui::PaintNineTile tiles[binui::kNineMaxTiles];
        const float mid0 = 0.0f, mid1 = 356.0f, nat = 12.0f;
        const int cnt = binui::SplitNineAxis(3, mid0, mid1, nat, tiles, binui::kNineMaxTiles);
        TEST("16.51: space 的块数 = floor(中段/自然)", cnt == 29);
        if (cnt == 29) {
            const float gap = (mid1 - mid0 - static_cast<float>(cnt) * nat) / static_cast<float>(cnt + 1);
            bool ok = true;
            for (int i = 0; i < cnt; i++) {
                if (std::fabs((tiles[i].t1 - tiles[i].t0) - nat) > 1e-4f) ok = false;   // 块长不动
                if (std::fabs(tiles[i].f0) > 1e-6f || std::fabs(tiles[i].f1 - 1.0f) > 1e-6f) ok = false;
            }
            TEST("16.51: 每块都是自然尺寸、UV 取整段", ok);
            TEST("16.51: 两端各留一个缝（首块不贴着起点）",
                 std::fabs(tiles[0].t0 - gap) < 1e-4f &&
                 std::fabs((mid1 - tiles[cnt - 1].t1) - gap) < 1e-4f);
            TEST("16.51: 块间缝隙等宽",
                 std::fabs((tiles[1].t0 - tiles[0].t1) - gap) < 1e-4f &&
                 std::fabs((tiles[cnt - 1].t0 - tiles[cnt - 2].t1) - gap) < 1e-4f);
        }
        // ---- 中段比一块还窄：[0,6) / 自然 12 → 1 块缩到 6（不裁切、不越界）----
        const int cnt2 = binui::SplitNineAxis(3, 0.0f, 6.0f, nat, tiles, binui::kNineMaxTiles);
        TEST("16.51: 中段比一块窄时只放一块",
             cnt2 == 1 && std::fabs(tiles[0].t0) < 1e-5f && std::fabs(tiles[0].t1 - 6.0f) < 1e-5f);
        // ---- 与 round 的对照：同样 [0,146) / 12，round 是 12 块各 12.17，space 是 12 块各 12 + 13 个缝 ----
        const int nr = binui::SplitNineAxis(2, 0.0f, 146.0f, nat, tiles, binui::kNineMaxTiles);
        float rLast = nr > 0 ? tiles[nr - 1].t1 : -1.0f;
        TEST("16.51: round 用等分块长填满（末块正好到边界）",
             nr == 12 && std::fabs(rLast - 146.0f) < 1e-4f);
        const int ns = binui::SplitNineAxis(3, 0.0f, 146.0f, nat, tiles, binui::kNineMaxTiles);
        TEST("16.51: space 块长仍是自然尺寸、靠缝填满（末块不到边界）",
             ns == 12 && std::fabs((tiles[ns - 1].t1 - tiles[ns - 1].t0) - nat) < 1e-4f &&
             (146.0f - tiles[ns - 1].t1) > 1e-4f &&
             std::fabs((146.0f - tiles[ns - 1].t1) - tiles[0].t0) < 1e-4f);

        // ---- 整盒几何：180×70 / 内缩 12 / 双轴 space ----
        // 中段 156 → x 轴 13 块（156 = 13×12，缝隙 0）；中段 46 → y 轴 3 块 + 余 10 均分 4 缝
        // 四边形数 = (13+2) × (3+2) = 75
        static float q[binui::kMaxNineSliceQuads * 8];
        const int n = binui::BuildNineSlice(0, 0, 180, 70, 48, 48, 12, 3, 3, q,
                                           binui::kMaxNineSliceQuads);
        TEST("16.51: 双轴 space 的四边形数", n == 75);
        bool inside = true;
        for (int i = 0; i < n; i++) {
            const float* o = q + i * 8;
            if (o[4] < -1e-4f || o[5] < -1e-4f || o[4] + o[6] > 180.0f + 1e-3f ||
                o[5] + o[7] > 70.0f + 1e-3f) inside = false;                 // 不越界
            if (o[6] > 12.0f + 1e-3f || o[7] > 12.0f + 1e-3f) {
                // 只有四角与两端是 12×12；中段块也不该超过自然尺寸
                if (o[6] > 12.0f + 1e-3f) inside = false;
            }
        }
        TEST("16.51: space 的所有块都在盒内且不超过自然尺寸", inside);
        // y 轴的缝隙 = (46 - 36) / 4 = 2.5：顶行 y=0、中心行第一块 y = 12 + 2.5
        bool rowOk = false;
        for (int i = 0; i < n; i++) {
            const float* o = q + i * 8;
            if (std::fabs(o[4] - 12.0f) < 1e-3f && std::fabs(o[5] - 14.5f) < 1e-3f &&
                std::fabs(o[7] - 12.0f) < 1e-3f) rowOk = true;
        }
        TEST("16.51: y 轴余数均分成缝（中心行下移 2.5px）", rowOk);
    }

    // ---- 0.16.20：程序化纹理/视频帧的 mip 链（binui_mip.h）----
    // 钉住三件事：
    //   1. 层数：一直减半到 1×1，最多 16 级；
    //   2. 第 l 级 = 上一级 2×2 的四舍五入平均（(a+b+c+d+2)/4）—— 这就是各后端
    //      "硬件 GenerateMipmap 与 VK 的 CPU 盒式滤波等价"的唯一口径；
    {
        TEST("16.20: mip 层数（1×1 / 2×2 / 256 / 1920×1080）",
             binui::MipLevels(1, 1) == 1 && binui::MipLevels(2, 2) == 2 &&
             binui::MipLevels(256, 256) == 9 && binui::MipLevels(1920, 1080) == 11);
        // 4×4 棋盘（2px 方格）：间隔取 0 / 255
        std::vector<uint8_t> checker(4 * 4 * 4, 0);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                const uint8_t v = (((x / 2) + (y / 2)) & 1) ? 255 : 0;
                for (int c = 0; c < 4; c++) checker[(y * 4 + x) * 4 + c] = v;
            }
        auto chain = binui::BuildMipChainBGRA(checker.data(), 4, 4);
        TEST("16.20: 4×4 出 3 级（4×4 / 2×2 / 1×1）", chain.size() == 3);
        if (chain.size() == 3) {
            TEST("16.20: 第 0 级原样", chain[0].size() == 4 * 4 * 4 && chain[0][0] == checker[0]);
            // 2×2 棋盘 → 每级都应该是 127 或 128（(0+0+0+255+2)/4 = 64? 不 —— 每个 2×2 块
            // 恰好是同一色，所以第 1 级仍是 0/255；第 2 级 1×1 = (0+255+255+0+2)/4 = 128）
            TEST("16.20: 同级色块保留", chain[1].size() == 2 * 2 * 4 &&
                                        (chain[1][0] == 0 || chain[1][0] == 255));
            TEST("16.20: 1×1 = 四舍五入平均（128）", chain[2].size() == 4 && chain[2][0] == 128);
        }
        // 3×3 奇数：第 1 级 1×1 = 左上 2×2 + 边缘夹取后的平均
        std::vector<uint8_t> odd(3 * 3 * 4, 10);
        for (int c = 0; c < 4; c++) odd[(1 * 3 + 1) * 4 + c] = 250;   // 中心 250
        auto chain2 = binui::BuildMipChainBGRA(odd.data(), 3, 3);
        TEST("16.20: 3×3 出 2 级（3×3 / 1×1）", chain2.size() == 2);
        if (chain2.size() == 2) {
            // 降采样点 = (0,0)=10 (1,0)=10 (0,1)=10 (1,1)=250 → (280+2)/4 = 70
            TEST("16.20: 3×3 → 1×1 的 2×2 采样（70）", chain2[1].size() == 4 && chain2[1][0] == 70);
        }
        auto empty = binui::BuildMipChainBGRA(nullptr, 4, 4);
        auto bad = binui::BuildMipChainBGRA(checker.data(), 0, 4);
        TEST("16.20: 退化输入返回空", empty.empty() && bad.empty());
    }

    // ---- 16.21：仿射级联的矩阵工具（transform 作用于整棵子树） ----
    {
        auto mapX = [](const float m[6], float x, float y) { return m[0] * x + m[1] * y + m[2]; };
        auto mapY = [](const float m[6], float x, float y) { return m[3] * x + m[4] * y + m[5]; };
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        const float id[6] = {1, 0, 0, 0, 1, 0};

        // 单位元：复合后不变
        float r1[6], r2[6];
        binui::AffineMul(id, id, r1);
        TEST("16.21: I∘I = I", near(r1[0], 1) && near(r1[1], 0) && near(r1[2], 0) &&
                               near(r1[3], 0) && near(r1[4], 1) && near(r1[5], 0));

        // 平移 ∘ 绕原点 90°：(1,0) → 转 (0,1) → 平移 (10,0) → (10,1)
        float rot[6], tr[6];
        binui::AffineRotScaleAbout(0.0f, 0.0f, 90.0f, 1.0f, 1.0f, rot);
        const float t10[6] = {1, 0, 10, 0, 1, 0};
        (void)tr;
        binui::AffineMul(t10, rot, r2);
        TEST("16.21: 平移∘旋转：点 (1,0) → (10,1)",
             near(mapX(r2, 1, 0), 10.0f) && near(mapY(r2, 1, 0), 1.0f));

        // 顺序不可交换：旋转 ∘ 平移 → (1,0)+10 → (11,0) → 转 90° → (0,11)
        binui::AffineMul(rot, t10, r2);
        TEST("16.21: 旋转∘平移 与上一条不同（顺序有意义）",
             near(mapX(r2, 1, 0), 0.0f) && near(mapY(r2, 1, 0), 11.0f));

        // 绕中心旋转：中心是不动点；右边的点转到下方
        float about[6];
        binui::AffineRotScaleAbout(50.0f, 20.0f, 90.0f, 1.0f, 1.0f, about);
        TEST("16.21: 绕 (50,20) 转 90°：中心不动",
             near(mapX(about, 50, 20), 50.0f) && near(mapY(about, 50, 20), 20.0f));
        TEST("16.21: 绕 (50,20) 转 90°：(60,20) → (50,30)",
             near(mapX(about, 60, 20), 50.0f) && near(mapY(about, 60, 20), 30.0f));

        // 非等比缩放：x 拉伸 2 倍、y 不动，中心不动
        float sq[6];
        binui::AffineRotScaleAbout(0.0f, 0.0f, 0.0f, 2.0f, 1.0f, sq);
        TEST("16.21: scale(2,1)：(5,7) → (10,7)",
             near(mapX(sq, 5, 7), 10.0f) && near(mapY(sq, 5, 7), 7.0f));

        // 级联 = 父矩阵 ∘ 平移(子原点) ：子原点在父局部里 (5,0)，父转 90° → 屏幕 (0,5)
        float child[6];
        const float t5[6] = {1, 0, 5, 0, 1, 0};
        binui::AffineMul(rot, t5, child);
        TEST("16.21: 父转 90° + 子偏移 5px → 子原点在 (0,5)",
             near(mapX(child, 0, 0), 0.0f) && near(mapY(child, 0, 0), 5.0f));

        // 别名安全：out 与 m0/m1 同一个数组
        float ali[6] = {1, 0, 10, 0, 1, 0};
        binui::AffineMul(ali, rot, ali);
        TEST("16.21: 复合允许输出与输入同一数组",
             near(mapX(ali, 1, 0), 10.0f) && near(mapY(ali, 1, 0), 1.0f));
    }

    // ---- 16.22：CSS filter 的颜色矩阵（v26 图片滤镜段）----
    // 钉住四件事：
    //   1. 单个函数的矩阵与 CSS 规范一致（grayscale/sepia/saturate/invert/contrast）；
    //   2. 合成顺序 = 书写顺序（先写的先作用），非交换；
    //   3. 逐像素应用：颜色被变换、alpha 不动、越界钳制；
    //   4. 编译器落点与缓存键（brightness 走 reserve[2]，其余进滤镜段）。
    {
        auto near = [](float a, float b) { return std::fabs(a - b) < 0.002f; };
        // 定点到 0..1 的 RGB 应用矩阵，方便断言
        auto apply = [&](const binui::ColorMatrix& c, float r, float g, float b, float out[3]) {
            out[0] = c.m[0] * r + c.m[1] * g + c.m[2] * b + c.o[0];
            out[1] = c.m[3] * r + c.m[4] * g + c.m[5] * b + c.o[1];
            out[2] = c.m[6] * r + c.m[7] * g + c.m[8] * b + c.o[2];
        };
        float o[3];

        apply(binui::FilterGrayscale(1.0f), 1.0f, 0.0f, 0.0f, o);
        TEST("16.22: grayscale(1) 红色 → 0.2126 灰",
             near(o[0], 0.2126f) && near(o[1], 0.2126f) && near(o[2], 0.2126f));
        apply(binui::FilterGrayscale(0.0f), 0.3f, 0.6f, 0.9f, o);
        TEST("16.22: grayscale(0) = 原样",
             near(o[0], 0.3f) && near(o[1], 0.6f) && near(o[2], 0.9f));

        apply(binui::FilterSepia(1.0f), 0.5f, 0.5f, 0.5f, o);
        // 行和：0.393+0.769+0.189 = 1.351；0.349+0.686+0.168 = 1.203；0.272+0.534+0.131 = 0.937
        TEST("16.22: sepia(1) 灰 0.5 → (0.6755, 0.6015, 0.4685)",
             near(o[0], 0.6755f) && near(o[1], 0.6015f) && near(o[2], 0.4685f));

        apply(binui::FilterSaturate(0.0f), 1.0f, 0.0f, 0.0f, o);
        TEST("16.22: saturate(0) = grayscale(1)",
             near(o[0], 0.2126f) && near(o[1], 0.2126f) && near(o[2], 0.2126f));
        apply(binui::FilterSaturate(2.0f), 1.0f, 0.0f, 0.0f, o);
        TEST("16.22: saturate(2) 纯红保持纯红（增益不改变已饱和通道）",
             near(o[0], 0.2126f + (1.0f - 0.2126f) * 2.0f) && near(o[1], 0.2126f - 0.2126f * 2.0f));

        apply(binui::FilterInvert(1.0f), 0.2f, 0.5f, 0.9f, o);
        TEST("16.22: invert(1) 反相", near(o[0], 0.8f) && near(o[1], 0.5f) && near(o[2], 0.1f));
        apply(binui::FilterInvert(0.5f), 0.2f, 0.5f, 0.9f, o);
        // a = 0.5 → 系数 1-2a = 0，偏移 a = 0.5：所有颜色都被拉到中灰
        TEST("16.22: invert(0.5) 全变成 0.5 灰",
             near(o[0], 0.5f) && near(o[1], 0.5f) && near(o[2], 0.5f));

        apply(binui::FilterContrast(0.0f), 0.1f, 0.7f, 0.9f, o);
        TEST("16.22: contrast(0) 全变成 0.5 灰", near(o[0], 0.5f) && near(o[2], 0.5f));
        apply(binui::FilterContrast(2.0f), 0.5f, 0.6f, 0.25f, o);
        TEST("16.22: contrast(2) 绕 0.5 拉伸", near(o[0], 0.5f) && near(o[1], 0.7f) && near(o[2], 0.0f));

        // hue-rotate(360°) ≈ 单位矩阵
        {
            const binui::ColorMatrix hr = binui::FilterHueRotate(360.0f);
            apply(hr, 0.2f, 0.6f, 0.9f, o);
            TEST("16.22: hue-rotate(360) ≈ 原样", near(o[0], 0.2f) && near(o[1], 0.6f) && near(o[2], 0.9f));
        }
        // 120° 把红搬到另一个通道族（R 行的权重不再让纯红留在 R）
        {
            const binui::ColorMatrix hr = binui::FilterHueRotate(120.0f);
            apply(hr, 1.0f, 0.0f, 0.0f, o);
            TEST("16.22: hue-rotate(120) 纯红 → 不再只有红通道有值",
                 o[0] < 0.99f || o[1] > 0.01f || o[2] > 0.01f);
        }

        // 合成顺序：先 grayscale 后 invert ≠ 先 invert 后 grayscale
        {
            const binui::ColorMatrix g = binui::FilterGrayscale(1.0f);
            const binui::ColorMatrix inv = binui::FilterInvert(1.0f);
            float a[3], b[3];
            apply(binui::ColorMatrixMul(inv, g), 1.0f, 0.0f, 0.0f, a);   // 先灰后反相
            TEST("16.22: 灰→反相：纯红 → 亮度取反（0.7874）", near(a[0], 1.0f - 0.2126f));
            // sepia 与 invert 不交换：这两种顺序结果必须不同（否则"按书写顺序合成"就没意义）
            const binui::ColorMatrix sp = binui::FilterSepia(1.0f);
            apply(binui::ColorMatrixMul(inv, sp), 0.5f, 0.2f, 0.8f, a);
            apply(binui::ColorMatrixMul(sp, inv), 0.5f, 0.2f, 0.8f, b);
            TEST("16.22: sepia→invert ≠ invert→sepia", std::fabs(a[0] - b[0]) > 0.05f);
        }

        // 逐像素应用：alpha 不动、越界钳制
        {
            std::vector<uint8_t> px = {
                0, 0, 255, 128,     // BGRA：纯红，半透明
                255, 255, 255, 0,   // 全透明（应原样跳过）
                200, 100, 50, 255,  // 随机色
            };
            const binui::ColorMatrix inv = binui::FilterInvert(1.0f);
            binui::ApplyColorMatrixBGRA(px.data(), 3, 1, inv);
            TEST("16.22: 像素反相（红 → 青）", px[0] == 255 && px[1] == 255 && px[2] == 0);
            TEST("16.22: alpha 不变", px[3] == 128);
            TEST("16.22: 全透明像素跳过", px[4] == 255 && px[5] == 255 && px[6] == 255 && px[7] == 0);
            TEST("16.22: 反相 (50,100,200) → (205,155,55)",
                 px[8] == 55 && px[9] == 155 && px[10] == 205 && px[11] == 255);

            // 越界钳制：contrast(4) 作用在 0.9 上会冲到 2.1 → 钳到 255
            std::vector<uint8_t> px2 = { 230, 230, 230, 255 };
            binui::ApplyColorMatrixBGRA(px2.data(), 1, 1, binui::FilterContrast(4.0f));
            TEST("16.22: 越界钳制到 255", px2[0] == 255 && px2[1] == 255 && px2[2] == 255);
        }

        // 缓存键：无滤镜 = 0；不同矩阵不同键；单位矩阵（= 不写段）也返回 0
        {
            BinUIImageFilter f = {};
            f.nodeId = 7; f.flags = 1;
            const binui::ColorMatrix g = binui::FilterGrayscale(1.0f);
            for (int i = 0; i < 9; i++) f.m[i] = g.m[i];
            const uint64_t k1 = binui::ImageFilterKey(&f);
            TEST("16.22: 滤镜键非 0", k1 != 0);
            TEST("16.22: nullptr → 0", binui::ImageFilterKey(nullptr) == 0);
            f.nodeId = 9;   // nodeId 不参与哈希（同一张图两个节点共用一份纹理）
            TEST("16.22: nodeId 不进键", binui::ImageFilterKey(&f) == k1);
            f.m[0] = 0.999f;
            TEST("16.22: 矩阵变 → 键变", binui::ImageFilterKey(&f) != k1);
        }

        // 编译器落点：filter 各函数 → 滤镜段；brightness 仍走 reserve[2]
        {
            const char* html = R"HTML(<app_window width="420" height="320" bg="#101014">
<html width="420" height="320" bg="#101014">
<body>
  <img id="gray"  src="a.png" style="position:absolute; left:10; top:10; width:100; height:60;
                                     filter: grayscale(100%)" />
  <img id="multi" src="a.png" style="position:absolute; left:10; top:80; width:100; height:60;
                                     filter: sepia(50%) saturate(150%) hue-rotate(90deg)" />
  <img id="inv"   src="a.png" style="position:absolute; left:10; top:150; width:100; height:60;
                                     filter: invert(1)" />
  <img id="bright" src="a.png" style="position:absolute; left:10; top:220; width:100; height:60;
                                     filter: brightness(0.5)" />
  <img id="none"  src="a.png" style="position:absolute; left:10; top:290; width:100; height:60" />
  <img id="blur"  src="a.png" style="position:absolute; left:200; top:10; width:100; height:60;
                                     filter: blur(2px)" />
</body></html>
</app_window>)HTML";
            const auto bin = CompileHTML(html);
            BinUIRenderer r;
            TEST("16.22: filter 布局可解析", ParseInto(r, bin));
            auto filtOf = [&](const char* name) -> const BinUIImageFilter* {
                const uint16_t id = r.findNodeByName(name);
                return id ? r.imageFilterOf(id) : nullptr;
            };
            const BinUIImageFilter* fg = filtOf("gray");
            TEST("16.22: grayscale(100%) 进滤镜段", fg != nullptr);
            if (fg) TEST("16.22: grayscale 矩阵首行 = 亮度权重",
                         std::fabs(fg->m[0] - 0.2126f) < 0.002f &&
                         std::fabs(fg->m[1] - 0.7152f) < 0.002f);
            TEST("16.22: 多函数合成进一条", filtOf("multi") != nullptr);
            const BinUIImageFilter* fi = filtOf("inv");
            TEST("16.22: invert(1) 的偏移 = 1",
                 fi && std::fabs(fi->offset[0] - 1.0f) < 0.002f &&
                 std::fabs(fi->m[0] + 1.0f) < 0.002f);
            TEST("16.22: brightness 不进滤镜段（走 reserve[2]）", filtOf("bright") == nullptr);
            TEST("16.22: 没写 filter → 无条目", filtOf("none") == nullptr);
            TEST("16.22: blur() 不进段（离屏合成暂无）", filtOf("blur") == nullptr);
        }
    }

    // ---- 16.23：SVG <marker>（v0.16.23，编译期展开成路径子路径） ----
    // 钉住四件事：
    //   1. marker-end / mid / start 都在编译期展开：几何变成引用路径的**兄弟节点**
    //      （BINUI_PATH + PATH_FILL），运行期/后端零改动；
    //   2. 引用路径 fill:none 时不会被填上（第一版把开放折线整块填了色，实拍抓到的）；
    //   3. 带 marker 的 <line> 升级成路径（BINUI_LINE 只有两个端点，画不了 marker）；
    //   4. 没有 marker 的 SVG 完全不受影响（节点数不变）。
    {
        const char* html = R"HTML(<app_window width="300" height="200" bg="#101014">
<html width="300" height="200" bg="#101014">
<body>
  <svg width="280" height="180" style="position:absolute; left:0; top:0">
    <defs>
      <marker id="arr" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
        <path d="M0,0 L8,4 L0,8 z" fill="#0B5FA5" />
      </marker>
      <marker id="dot" markerWidth="6" markerHeight="6" refX="3" refY="3" orient="auto">
        <circle cx="3" cy="3" r="2.5" fill="#0F766E" />
      </marker>
    </defs>
    <path id="p1" d="M10,20 L120,20" fill="none" stroke="#0B5FA5" stroke-width="2"
          marker-end="url(#arr)" />
    <polyline id="p2" points="10,60 60,40 110,60" fill="none" stroke="#0F766E"
              stroke-width="2" marker-mid="url(#dot)" />
    <line id="l1" x1="10" y1="100" x2="120" y2="100" stroke="#C2410C" stroke-width="2"
          marker-end="url(#arr)" />
    <path id="plain" d="M10,140 L120,140" fill="none" stroke="#888888" stroke-width="2" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.23: marker 布局可解析", ParseInto(r, bin));

        auto countPaths = [&](void) {
            int n = 0;
            for (const auto& ni : r.getNodes())
                if (ni.type == BINUI_PATH) n++;
            return n;
        };
        // 4 条图形：p1 / p2 / l1（line 升级）/ plain；marker 每个引用处多一个子节点：
        //   p1 → 1（end）、p2 → 1（mid 两个点但合成一个节点）、l1 → 1 = 共 3 个
        TEST("16.23: marker 展开成额外路径节点（4 + 3）", countPaths() == 7);

        const uint16_t id1 = r.findNodeByName("p1");
        TEST("16.23: 引用路径仍然存在", id1 != 0);
        int childCount = 0;
        for (const auto& ni : r.getNodes())
            if (ni.parentId == id1 && ni.type == BINUI_PATH) childCount++;
        TEST("16.23: marker 节点挂在引用路径下（子节点 = 后画 = 压在线上）", childCount == 1);

        // 引用路径自己 fill:none → 不能因为 marker 被填上
        {
            const auto* pr = r.pathOf(id1);
            TEST("16.23: 引用路径仍是 fill:none（没有被 marker 的填充污染）",
                 pr && !(pr->head.flags & PATH_FILL));
        }
        // marker 节点是纯填充
        {
            const BinUIRenderer::PathRecord* mr = nullptr;
            for (const auto& ni : r.getNodes())
                if (ni.parentId == id1 && ni.type == BINUI_PATH) mr = r.pathOf(ni.id);
            TEST("16.23: marker 节点是纯填充（PATH_FILL，无描边）",
                 mr && (mr->head.flags & PATH_FILL) && !(mr->head.flags & PATH_STROKE));
            // 三角形 3 个点（细分后仍闭合）
            TEST("16.23: marker 几何至少 3 个点且闭合",
                 mr && mr->pts.size() >= 6 && !mr->closed.empty() && mr->closed[0] == 1);
        }
        // 带 marker 的 <line> 必须升级成路径
        {
            const uint16_t lid = r.findNodeByName("l1");
            bool isPath = false;
            for (const auto& ni : r.getNodes())
                if (ni.id == lid) isPath = (ni.type == BINUI_PATH);
            TEST("16.23: 带 marker 的 <line> 升级成路径", lid != 0 && isPath);
        }
        // 没有 marker 的路径不受影响：没有子节点
        {
            const uint16_t pid = r.findNodeByName("plain");
            int kids = 0;
            for (const auto& ni : r.getNodes())
                if (ni.parentId == pid) kids++;
            TEST("16.23: 没写 marker 的路径不生成子节点", pid != 0 && kids == 0);
        }
    }

    // ---- 16.52：patternTransform 的旋转/斜切（v0.16.52）----
    // 钉住四件事：
    //   1. `rotate()` 真的转：内容是一条 40×4 的横条，转 90° 之后展开节点的盒子变成
    //      4×40（宽高互换）—— 老实现按轴对齐近似，这里会仍是 40×4；
    //   2. `skewX()` 真的斜：横条被剪成 40 + 4·tan30° ≈ 42.31 宽；
    //   3. 旋转是**相似变换** → 描边宽度精确、不该报"斜切"的告警；斜切才告警；
    //   4. 不可逆（scale(0)）→ 告警并退回纯色实填（不是画错，也不是什么都不画）。
    {
        const char* html = R"HTML(<app_window width="320" height="200" bg="#101014">
<html width="320" height="200" bg="#101014">
<body>
  <svg width="300" height="190" style="position:absolute; left:0; top:0">
    <defs>
      <pattern id="rot" width="200" height="200" patternTransform="rotate(90)">
        <rect x="0" y="0" width="40" height="4" fill="#0B5FA5" />
      </pattern>
      <pattern id="skw" width="200" height="200" patternTransform="skewX(30)"
               patternUnits="userSpaceOnUse">
        <rect x="0" y="0" width="40" height="4" fill="#0F766E" stroke="#22CC88"
              stroke-width="2" />
      </pattern>
      <pattern id="plain" width="200" height="200">
        <rect x="0" y="0" width="40" height="4" fill="#C2410C" />
      </pattern>
      <pattern id="sing" width="200" height="200" patternTransform="scale(0)">
        <rect x="0" y="0" width="40" height="4" fill="#777777" />
      </pattern>
    </defs>
    <rect id="a1" x="-8" y="0" width="16" height="60" fill="url(#rot)" />
    <rect id="a2" x="0" y="40" width="50" height="8" fill="url(#skw)" />
    <rect id="a3" x="0" y="80" width="10" height="10" fill="url(#plain)" />
    <rect id="a4" x="0" y="120" width="10" height="10" fill="url(#sing)" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.52: patternTransform 布局可解析", ParseInto(r, res.layout));
        // 展开出来的几何：一个节点里可能有多个 tile 的轮廓（都挂在同一个 PathGroup 上），
        // 所以取**第一条子路径**的包围盒 —— 每一块形状都一样，转没转一眼就能看出来。
        auto firstContourBox = [&](const char* name, float& bw, float& bh) -> bool {
            const uint16_t pid = r.findNodeByName(name);
            for (const auto& ni : r.getNodes()) {
                if (ni.parentId != pid || ni.type != BINUI_PATH) continue;
                const auto* pr = r.pathOf(ni.id);
                if (!pr || pr->first.empty() || pr->count.empty()) continue;
                const uint32_t s0 = pr->first[0], c0 = pr->count[0];
                float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                for (uint32_t i = s0; i < s0 + c0 && i < pr->pts.size() / 2; i++) {
                    const float u = pr->pts[i * 2] / 65535.0f;
                    const float v = pr->pts[i * 2 + 1] / 65535.0f;
                    u0 = std::min(u0, u); u1 = std::max(u1, u);
                    v0 = std::min(v0, v); v1 = std::max(v1, v);
                }
                bw = (u1 - u0) * (float)ni.width;
                bh = (v1 - v0) * (float)ni.height;
                return true;
            }
            return false;
        };
        float w1 = 0, h1 = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
        TEST("16.52: rotate(90) 后 40×4 的横条变成 4×40（宽高互换）",
             firstContourBox("a1", w1, h1) && std::fabs(w1 - 4.0f) <= 1.5f &&
             std::fabs(h1 - 40.0f) <= 1.5f);
        TEST("16.52: 没写 patternTransform 的仍按老行为（40×4）",
             firstContourBox("a3", w3, h3) && std::fabs(w3 - 40.0f) <= 1.5f &&
             std::fabs(h3 - 4.0f) <= 1.5f);
        TEST("16.52: skewX(30) 把横条剪宽（40 + 4·tan30 ≈ 42.3）",
             firstContourBox("a2", w2, h2) && std::fabs(w2 - 42.31f) <= 1.5f &&
             std::fabs(h2 - 4.0f) <= 1.5f);
        bool rotatedWarn = false, skewWarn = false, singularWarn = false;
        for (const auto& w : res.warnings) {
            if (w.find("旋转/斜切不支持") != std::string::npos) rotatedWarn = true;
            if (w.find("斜切：描边按几何平均宽度画") != std::string::npos) skewWarn = true;
            if (w.find("不可逆") != std::string::npos) singularWarn = true;
        }
        TEST("16.52: 不再报\"旋转/斜切不支持\"", !rotatedWarn);
        TEST("16.52: 斜切 + 描边 → 告警（椭圆笔只存一个线宽）", skewWarn);
        TEST("16.52: scale(0) → 告警不可逆", singularWarn);
        {
            // 不可逆 → fallback：唯一一个节点的形状是**元素自己**（纯色实填），盒子 = 元素盒
            const uint16_t pid4 = r.findNodeByName("a4");
            int n4 = 0;
            float w4 = 0, h4 = 0;
            for (const auto& ni : r.getNodes()) {
                if (ni.parentId != pid4 || ni.type != BINUI_PATH) continue;
                n4++;
                w4 = (float)ni.width; h4 = (float)ni.height;
            }
            TEST("16.52: 不可逆时退回纯色实填（元素盒 10×10 一块）", n4 == 1 &&
                 std::fabs(w4 - 10.0f) <= 1.5f && std::fabs(h4 - 10.0f) <= 1.5f);
        }
        // 求逆与相似判定的单元检查
        {
            binui::PathMatrix m = binui::PathRotate(37.0f);
            m = binui::PathMul(binui::PathTranslate(11.0f, -7.0f), binui::PathScale(2.0f, 3.0f));
            binui::PathMatrix inv;
            const bool okInv = binui::PathInvert(m, inv);
            const binui::PathPointF back =
                binui::PathApply(inv, binui::PathApply(m, 5.0f, 9.0f).x,
                                      binui::PathApply(m, 5.0f, 9.0f).y);
            TEST("16.52: PathInvert 往返回到原点",
                 okInv && std::fabs(back.x - 5.0f) < 1e-3f && std::fabs(back.y - 9.0f) < 1e-3f);
            binui::PathMatrix zero = binui::PathScale(0.0f, 1.0f);
            TEST("16.52: PathInvert 对退化矩阵返回 false",
                 !binui::PathInvert(zero, inv));
            TEST("16.52: 旋转+等比缩放是相似变换（描边精确）",
                 binui::PathIsSimilarity(binui::PathRotate(23.0f)));
            binui::PathMatrix sk = { 1.0f, 0.0f, 0.577f, 1.0f, 0.0f, 0.0f };
            TEST("16.52: skewX 不是相似变换（要告警）", !binui::PathIsSimilarity(sk));
        }
    }

    // ---- 16.24：SVG <clipPath> / clip-path（v27 裁剪路径段） ----
    // 钉住四件事：
    //   1. clip-path: url(#id) 落进裁剪段（renderer.clipPathOf），包围盒是**设备空间**的；
    //   2. clipPathUnits=objectBoundingBox 把 0..1 坐标放大到元素盒；
    //   3. 没写 / 写了但不存在的 id → 没有条目（不画坏，只是不裁）；
    //   4. 裁剪路径的归一化点都在 [0,65535] 且闭合（运行期补集擦除要它闭）。
    {
        const char* html = R"HTML(<app_window width="300" height="200" bg="#101014">
<html width="300" height="200" bg="#101014">
<body>
  <svg width="290" height="190" style="position:absolute; left:0; top:0">
    <defs>
      <clipPath id="circ"><circle cx="60" cy="60" r="40" /></clipPath>
      <clipPath id="obb" clipPathUnits="objectBoundingBox">
        <rect x="0" y="0" width="1" height="1" rx="0.25" />
      </clipPath>
    </defs>
    <rect id="c1" x="0" y="0" width="120" height="120" fill="#0B5FA5" clip-path="url(#circ)" />
    <rect id="c2" x="150" y="0" width="100" height="100" fill="#C2410C" clip-path="url(#obb)" />
    <rect id="c3" x="0" y="130" width="100" height="50" fill="#0F766E" />
    <rect id="c4" x="150" y="130" width="100" height="50" fill="#0F766E" clip-path="url(#nope)" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("16.24: clip-path 布局可解析", ParseInto(r, bin));
        auto clipOfName = [&](const char* name) -> const BinUIRenderer::ClipRecord* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.clipPathOf(id) : nullptr;
        };
        const BinUIRenderer::ClipRecord* c1 = clipOfName("c1");
        TEST("16.24: circle 裁剪进段（有条目）", c1 != nullptr);
        if (c1) {
            TEST("16.24: 包围盒 = 圆的外接方块（40 半径 → 80×80）",
                 std::fabs(c1->head.w - 80.0f) < 1.5f && std::fabs(c1->head.h - 80.0f) < 1.5f);
            TEST("16.24: 包围盒是设备空间（x≈20, y≈20）",
                 std::fabs(c1->head.x - 20.0f) < 1.5f && std::fabs(c1->head.y - 20.0f) < 1.5f);
            bool allIn = !c1->pts.empty();
            for (uint16_t v : c1->pts) (void)v;   // uint16 天然在范围内，这里只验非空
            TEST("16.24: 点表非空且闭合", allIn && !c1->closed.empty() && c1->closed[0] == 1);
        }
        const BinUIRenderer::ClipRecord* c2 = clipOfName("c2");
        TEST("16.24: objectBoundingBox 裁剪进段", c2 != nullptr);
        if (c2) {
            TEST("16.24: objectBoundingBox 放大到元素盒（100×100）",
                 std::fabs(c2->head.w - 100.0f) < 2.0f && std::fabs(c2->head.h - 100.0f) < 2.0f);
        }
        TEST("16.24: 没写 clip-path → 无条目", clipOfName("c3") == nullptr);
        TEST("16.24: 引用不存在的 id → 无条目（不裁也不报错画坏）",
             clipOfName("c4") == nullptr);
    }

    // ---- 16.25：SVG <pattern>（v0.16.25，编译期平铺展开） ----
    // 钉住五件事：
    //   1. 引用 pattern 的元素**自己不再填充**（填充交给展开出来的 tile 节点）——
    //      否则下面会垫一整块实色，图案就白铺了；
    //   2. tile 展开成子路径节点，同色同笔合成一个（100 块 10×10 的 tile 只出一个节点）；
    //   3. 每个展开节点都挂一条"元素自身轮廓"的裁剪记录（靠 v27 的补集擦除兜住溢出）；
    //   4. patternUnits=objectBoundingBox 时 tile 尺寸按元素盒比例放大；
    //   5. 引用了不存在的 id / width=0 → 告警，并且**不留空白**（退回纯色 / 明确不填）。
    {
        const char* html = R"HTML(<app_window width="300" height="200" bg="#101014">
<html width="300" height="200" bg="#101014">
<body>
  <svg width="290" height="190" style="position:absolute; left:0; top:0">
    <defs>
      <pattern id="p1" width="10" height="10" patternUnits="userSpaceOnUse">
        <rect x="0" y="0" width="5" height="10" fill="#1D4ED8" />
      </pattern>
      <pattern id="p2" width="0.5" height="0.5" patternUnits="objectBoundingBox"
               patternContentUnits="objectBoundingBox">
        <rect x="0" y="0" width="0.5" height="0.5" fill="#B45309" />
      </pattern>
      <pattern id="pzero" width="0" height="10">
        <rect x="0" y="0" width="4" height="4" fill="#0F766E" />
      </pattern>
    </defs>
    <rect id="a1" x="0" y="0" width="100" height="100" fill="url(#p1)" />
    <rect id="a2" x="110" y="0" width="100" height="100" fill="url(#p2)" />
    <rect id="a3" x="0" y="120" width="100" height="50" fill="url(#nope)" />
    <rect id="a4" x="110" y="120" width="100" height="50" fill="url(#pzero)" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.25: pattern 布局可解析", ParseInto(r, res.layout));

        auto kidsOf = [&](const char* name) {
            std::vector<uint16_t> out;
            const uint16_t pid = r.findNodeByName(name);
            for (const auto& ni : r.getNodes())
                if (ni.parentId == pid && ni.type == BINUI_PATH) out.push_back(ni.id);
            return out;
        };
        // 1) 元素自己变成路径节点但不再填充
        const uint16_t a1 = r.findNodeByName("a1");
        TEST("16.25: 引用 pattern 的 <rect> 升级成路径", a1 != 0 && r.pathOf(a1) != nullptr);
        {
            const auto* pr = r.pathOf(a1);
            TEST("16.25: 元素自身不填充（填充交给 tile）",
                 pr && !(pr->head.flags & PATH_FILL));
        }
        // 2) tile 展开：100 个 10×10 的同色 tile → 一个子路径节点
        {
            const auto kids = kidsOf("a1");
            TEST("16.25: 同色 tile 合成一个子节点", kids.size() == 1);
            if (kids.size() == 1) {
                const auto* kp = r.pathOf(kids[0]);
                TEST("16.25: tile 节点是纯填充", kp && (kp->head.flags & PATH_FILL) &&
                     !(kp->head.flags & PATH_STROKE));
                // 每块 1 个矩形子路径 → 100 块 → 至少 100 个点
                TEST("16.25: 100 块 tile 都铺上了（点数 >= 100）",
                     kp && kp->head.pointCount >= 100);
                // 3) 裁剪记录：包围盒 = 元素盒（设备空间），闭合
                const auto* cr = r.clipPathOf(kids[0]);
                TEST("16.25: tile 节点挂了元素轮廓的裁剪记录", cr != nullptr);
                if (cr) {
                    TEST("16.25: 裁剪盒 = 元素盒（100×100）",
                         std::fabs(cr->head.w - 100.0f) < 2.0f &&
                         std::fabs(cr->head.h - 100.0f) < 2.0f);
                    TEST("16.25: 裁剪点闭合且够用",
                         !cr->pts.empty() && !cr->closed.empty() && cr->closed[0] == 1);
                }
            }
        }
        // 4) objectBoundingBox：tile = 元素盒的 50% → 4 块，且几何被放大到 50×50
        {
            const auto kids = kidsOf("a2");
            TEST("16.25: objectBoundingBox 的 tile 也展开了", kids.size() == 1);
            if (kids.size() == 1) {
                const auto* kp = r.pathOf(kids[0]);
                // 4 块 × 4 点 = 16 点；如果没放大就是 0.5 单位的小点，点数一样但节点盒会很小
                TEST("16.25: 4 块 tile（点数 >= 16）", kp && kp->head.pointCount >= 16);
                const auto* ni = NodeById(r, kids[0]);
                TEST("16.25: OBB 的 tile 被放大到元素盒尺度（>= 40px）",
                     ni && ni->width >= 40 && ni->height >= 40);
            }
        }
        // 5) 引用不存在的 id：告警 + 退回纯色（不留空白）
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("nope") != std::string::npos) warned = true;
            TEST("16.25: 引用不存在的 pattern 要告警", warned);
            // 没有渐变/描边的 <rect> 走轻量映射（不是路径节点），所以两种形态都算"看得见"
            const uint16_t a3id = r.findNodeByName("a3");
            const auto* pr = r.pathOf(a3id);
            const auto* na = NodeById(r, a3id);
            TEST("16.25: 找不到 pattern 时退回纯色（元素仍然可见）",
                 (pr && (pr->head.flags & PATH_FILL)) || (na && na->color != 0u));
        }
        // 6) width=0：告警，并且不凭空填色
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("width/height") != std::string::npos) warned = true;
            TEST("16.25: pattern 尺寸为 0 要告警", warned);
            TEST("16.25: pattern 尺寸为 0 → 不填充", kidsOf("a4").empty());
        }
    }

    // ---- 16.26：SVG <mask>（v0.16.26，格式 v28 遮罩段） ----
    // 钉住六件事：
    //   1. mask="url(#id)" 落进遮罩段（renderer.maskOf），每个条目带"亮度折成的 alpha"；
    //   2. 亮度口径（白 = 255、0xB4 = 180），纯黑形状直接不参与（= 全透明）；
    //   3. 区域默认是 bbox 的 -10%..120%，maskUnits=objectBoundingBox 时按元素盒取；
    //   4. 条目几何是**设备空间**包围盒（圆 → 10..90）；
    //   5. 引用不存在的 id / 内容全黑 → 告警且没有条目（不会把元素画没）；
    //   6. 没有 mask 的元素不受影响。
    {
        const char* html = R"HTML(<app_window width="300" height="200" bg="#101014">
<html width="300" height="200" bg="#101014">
<body>
  <svg width="290" height="190" style="position:absolute; left:0; top:0">
    <defs>
      <mask id="mw"><circle cx="50" cy="50" r="40" fill="#FFFFFF" /></mask>
      <mask id="mg"><rect x="0" y="0" width="50" height="100" fill="#B4B4B4" /></mask>
      <mask id="mobb" maskUnits="objectBoundingBox" x="0" y="0" width="1" height="1">
        <rect x="0" y="0" width="1" height="1" fill="#FFFFFF" />
      </mask>
      <mask id="mblack"><rect x="0" y="0" width="10" height="10" fill="#000000" /></mask>
    </defs>
    <rect id="k1" x="0" y="0" width="100" height="100" fill="#0B5FA5" mask="url(#mw)" />
    <rect id="k2" x="110" y="0" width="100" height="100" fill="#C2410C" mask="url(#mg)" />
    <rect id="k3" x="0" y="120" width="100" height="50" fill="#0F766E" mask="url(#mobb)" />
    <rect id="k4" x="110" y="120" width="100" height="50" fill="#0F766E" mask="url(#nope)" />
    <rect id="k5" x="220" y="0" width="60" height="60" fill="#0F766E" mask="url(#mblack)" />
    <rect id="k6" x="220" y="70" width="60" height="60" fill="#0F766E" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.26: mask 布局可解析", ParseInto(r, res.layout));

        auto masksOfName = [&](const char* name) -> std::vector<BinUIRenderer::MaskRecord> {
            const uint16_t id = r.findNodeByName(name);
            if (!id) return {};
            return r.maskOf(id);
        };
        // 1) 白圆遮罩：一条条目，alpha = 255，几何盒 = 圆的外接方块
        {
            auto ms = masksOfName("k1");
            TEST("16.26: 白圆遮罩进段（1 条）", ms.size() == 1);
            if (ms.size() == 1) {
                TEST("16.26: 白色 → alpha 255", ((ms[0].head.color >> 24) & 0xFFu) == 255);
                TEST("16.26: 条目几何是设备空间的圆外接盒（80×80）",
                     std::fabs(ms[0].head.w - 80.0f) < 1.5f &&
                     std::fabs(ms[0].head.h - 80.0f) < 1.5f);
                TEST("16.26: 条目几何原点在 (10,10)",
                     std::fabs(ms[0].head.x - 10.0f) < 1.5f &&
                     std::fabs(ms[0].head.y - 10.0f) < 1.5f);
                // 2) 区域默认 = bbox 的 -10%..120%
                TEST("16.26: 默认区域 = bbox 外扩 10%（120×120 @ -10,-10）",
                     std::fabs(ms[0].head.rx + 10.0f) < 1.5f &&
                     std::fabs(ms[0].head.ry + 10.0f) < 1.5f &&
                     std::fabs(ms[0].head.rw - 120.0f) < 1.5f &&
                     std::fabs(ms[0].head.rh - 120.0f) < 1.5f);
                TEST("16.26: 点表非空且闭合",
                     !ms[0].pts.empty() && !ms[0].closed.empty() && ms[0].closed[0] == 1);
            }
        }
        // 2) 灰 = 半透明
        {
            auto ms = masksOfName("k2");
            TEST("16.26: 灰阶遮罩进段（1 条）", ms.size() == 1);
            if (ms.size() == 1) {
                const int a = (int)((ms[0].head.color >> 24) & 0xFFu);
                TEST("16.26: 0xB4 的亮度 → alpha 180（±2）", a >= 178 && a <= 182);
            }
        }
        // 3) objectBoundingBox 的区域 = 元素盒本身
        {
            auto ms = masksOfName("k3");
            TEST("16.26: objectBoundingBox 遮罩进段", ms.size() == 1);
            if (ms.size() == 1) {
                TEST("16.26: 区域 = 元素盒（0,120 100×50）",
                     std::fabs(ms[0].head.rx - 0.0f) < 1.5f &&
                     std::fabs(ms[0].head.ry - 120.0f) < 1.5f &&
                     std::fabs(ms[0].head.rw - 100.0f) < 1.5f &&
                     std::fabs(ms[0].head.rh - 50.0f) < 1.5f);
            }
        }
        // 4) 引用不存在的 id：告警 + 无条目
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("nope") != std::string::npos) warned = true;
            TEST("16.26: 引用不存在的 mask 要告警", warned);
            TEST("16.26: 引用不存在的 mask → 无条目（元素照常画）",
                 masksOfName("k4").empty());
        }
        // 5) 内容全黑：告警 + 无条目（不把元素画没）
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("没有可见形状") != std::string::npos) warned = true;
            TEST("16.26: 掩码内容全黑要告警", warned);
            TEST("16.26: 掩码内容全黑 → 无条目", masksOfName("k5").empty());
        }
        // 6) 没写 mask 的元素不受影响
        TEST("16.26: 没写 mask → 无条目", masksOfName("k6").empty());
    }

    // ---- 16.53：遮罩的渐变内容（v0.16.53，格式 v34）----
    // 钉住五件事：
    //   1. `fill="url(#grad)"` 不再降级成"中间色"，而是生成一条**带渐变**的遮罩记录
    //      （BinUIMaskEntry.flags bit0 + 紧跟一条 BinUIPathGradient）；
    //   2. 色标的 alpha = **亮度**（白 → 255、黑 → 0），所以运行期那句 dst *= srcA
    //      逐点乘的就是渐变亮度；
    //   3. 渐变坐标是"本组几何包围盒归一化"空间：横向渐变的端点落在 x=0 / x=1；
    //   4. fill-opacity 折进色标 alpha；渐变形状独占一条记录（不和纯色形状合并）；
    //   5. 没有渐变的 mask 内容不受影响（仍是一条纯色记录，flags 不置位）。
    {
        const char* html = R"HTML(<app_window width="320" height="200" bg="#101014">
<html width="320" height="200" bg="#101014">
<body>
  <svg width="300" height="190" style="position:absolute; left:0; top:0">
    <defs>
      <linearGradient id="gm" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" stop-color="#FFFFFF" />
        <stop offset="1" stop-color="#000000" />
      </linearGradient>
      <linearGradient id="gm2" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#808080" />
        <stop offset="1" stop-color="#FFFFFF" />
      </linearGradient>
      <mask id="mg" maskUnits="userSpaceOnUse" x="0" y="0" width="200" height="120">
        <rect x="0" y="0" width="200" height="120" fill="url(#gm)" />
      </mask>
      <mask id="mg2" maskUnits="userSpaceOnUse" x="0" y="0" width="200" height="120">
        <rect x="0" y="0" width="100" height="120" fill="#FFFFFF" />
        <rect x="100" y="0" width="100" height="120" fill="url(#gm2)" fill-opacity="0.5" />
      </mask>
      <mask id="mflat" maskUnits="userSpaceOnUse" x="0" y="0" width="200" height="120">
        <rect x="0" y="0" width="200" height="120" fill="#FFFFFF" />
      </mask>
    </defs>
    <rect id="g1" x="10" y="10" width="190" height="100" fill="#3B82F6" mask="url(#mg)" />
    <rect id="g2" x="10" y="10" width="190" height="100" fill="#3B82F6" mask="url(#mg2)" />
    <rect id="g3" x="10" y="10" width="190" height="100" fill="#3B82F6" mask="url(#mflat)" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.53: 渐变遮罩布局可解析", ParseInto(r, res.layout));
        bool warnedGrad = false;
        for (const auto& w : res.warnings)
            if (w.find("不支持渐变") != std::string::npos) warnedGrad = true;
        TEST("16.53: 不再报\"内容不支持渐变\"", !warnedGrad);
        auto masksOfName = [&](const char* name) -> std::vector<BinUIRenderer::MaskRecord> {
            const uint16_t id = r.findNodeByName(name);
            if (!id) return {};
            return r.maskOf(id);
        };
        // 1) 一条渐变记录
        const auto m1 = masksOfName("g1");
        TEST("16.53: 渐变 mask 生成一条记录", m1.size() == 1);
        if (m1.size() == 1) {
            const auto& rec = m1[0];
            TEST("16.53: flags 置位 MASK_FLAG_FILL_GRAD", (rec.head.flags & MASK_FLAG_FILL_GRAD) != 0);
            TEST("16.53: 读到了渐变本体", rec.hasGrad);
            TEST("16.53: 线性渐变、两个色标",
                 rec.grad.type == binui::PATH_GRAD_LINEAR && rec.grad.stopCount == 2);
            // 2) 色标 alpha = 亮度：白 → 255、黑 → 0
            TEST("16.53: 白端 alpha 255、黑端 alpha 0",
                 ((rec.grad.stops[0].color >> 24) & 0xFFu) >= 250 &&
                 ((rec.grad.stops[1].color >> 24) & 0xFFu) <= 2);
            // 3) 归一化坐标：横向渐变的端点在 x=0 / x=1（y 同一条线）
            TEST("16.53: 渐变归一化到本组几何盒（x 0 → 1）",
                 std::fabs(rec.grad.x1 - 0.0f) < 0.02f && std::fabs(rec.grad.x2 - 1.0f) < 0.02f &&
                 std::fabs(rec.grad.y1 - rec.grad.y2) < 0.02f);
        }
        // 4) 纯色形状 + 渐变形状：两条记录，渐变那条独占；fill-opacity 折进 alpha
        const auto m2 = masksOfName("g2");
        TEST("16.53: 纯色 + 渐变 → 两条记录", m2.size() == 2);
        if (m2.size() == 2) {
            int solid = 0, grad = 0;
            for (const auto& rec : m2) {
                if (rec.hasGrad) {
                    grad++;
                    // 灰 #808080 的亮度 ≈ 0.502 → ×0.5 → ≈64；白 → 0.5 → ≈128
                    const int a0 = (int)((rec.grad.stops[0].color >> 24) & 0xFFu);
                    const int a1 = (int)((rec.grad.stops[1].color >> 24) & 0xFFu);
                    TEST("16.53: 渐变色标 alpha = 亮度 × fill-opacity",
                         std::abs(a0 - 64) <= 3 && std::abs(a1 - 128) <= 3);
                } else {
                    solid++;
                }
            }
            TEST("16.53: 纯色记录仍是一条、渐变不与之合并", solid == 1 && grad == 1);
        }
        // 4b) 区域宽高（maskUnits="userSpaceOnUse"）不再被乘元素盒 —— v0.16.53 修的老 bug：
        //     190×60 的元素 + 200×120 的 mask 区域以前会得到 38000×7200 的图层框。
        if (m1.size() == 1) {
            // 宽高不再被元素盒放大（修之前是 200×190 = 38000、120×60 = 7200）；
            // rx/ry 是"元素盒左上 + 用户坐标"（非路径元素以元素原点为用户空间原点）
            TEST("16.53: userSpaceOnUse 的 mask 区域宽高就是 200×120（不再乘元素盒）",
                 std::fabs(m1[0].head.rw - 200.0f) < 1.0f &&
                 std::fabs(m1[0].head.rh - 120.0f) < 1.0f &&
                 std::fabs(m1[0].head.rx - 10.0f) < 1.0f &&
                 std::fabs(m1[0].head.ry - 10.0f) < 1.0f);
        }
        // 5) 没有渐变的 mask 不受影响
        const auto m3 = masksOfName("g3");
        TEST("16.53: 纯色 mask 的 flags 不置位",
             m3.size() == 1 && (m3[0].head.flags & MASK_FLAG_FILL_GRAD) == 0 && !m3[0].hasGrad);
    }

    // ---- 16.54：mask-type（v0.16.54）----
    // 钉住五件事：
    //   1. 默认 / mask-type="luminance"：颜色**亮度**当 alpha（#808080 → ≈128）；
    //   2. mask-type="alpha"：亮度不参与，只取内容自己的 alpha（#808080 → 255）；
    //   3. alpha 型下 fill-opacity / opacity 仍然乘（0.5 → ≈128）；
    //   4. 渐变内容同样跟着变（色标 alpha 不再被亮度压暗）；
    //   5. 认不出的值、写在非 <mask> 元素上，都要点名告警。
    {
        const char* html = R"HTML(<app_window width="320" height="200" bg="#101014">
<html width="320" height="200" bg="#101014">
<body>
  <svg width="300" height="190" style="position:absolute; left:0; top:0">
    <defs>
      <mask id="lum" maskUnits="userSpaceOnUse" x="0" y="0" width="120" height="60">
        <rect x="0" y="0" width="120" height="60" fill="#808080" />
      </mask>
      <mask id="alp" mask-type="alpha" maskUnits="userSpaceOnUse" x="0" y="0" width="120" height="60">
        <rect x="0" y="0" width="120" height="60" fill="#808080" />
      </mask>
      <mask id="alpHalf" mask-type="alpha" maskUnits="userSpaceOnUse" x="0" y="0" width="120" height="60">
        <rect x="0" y="0" width="120" height="60" fill="#FF0000" opacity="0.5" />
      </mask>
      <mask id="lumExplicit" mask-type="luminance" maskUnits="userSpaceOnUse" x="0" y="0" width="120" height="60">
        <rect x="0" y="0" width="120" height="60" fill="#808080" />
      </mask>
      <linearGradient id="g54" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" stop-color="#808080" />
        <stop offset="1" stop-color="#404040" />
      </linearGradient>
      <mask id="gradAlp" mask-type="alpha" maskUnits="userSpaceOnUse" x="0" y="0" width="120" height="60">
        <rect x="0" y="0" width="120" height="60" fill="url(#g54)" />
      </mask>
      <mask id="badType" mask-type="foo" maskUnits="userSpaceOnUse" x="0" y="0" width="120" height="60">
        <rect x="0" y="0" width="120" height="60" fill="#808080" />
      </mask>
    </defs>
    <rect id="e1" x="10" y="10" width="100" height="40" fill="#3B82F6" mask="url(#lum)" />
    <rect id="e2" x="10" y="10" width="100" height="40" fill="#3B82F6" mask="url(#alp)" />
    <rect id="e3" x="10" y="10" width="100" height="40" fill="#3B82F6" mask="url(#alpHalf)" />
    <rect id="e4" x="10" y="10" width="100" height="40" fill="#3B82F6" mask="url(#lumExplicit)" />
    <rect id="e5" x="10" y="10" width="100" height="40" fill="#3B82F6" mask="url(#gradAlp)" />
    <rect id="e6" x="10" y="10" width="100" height="40" fill="#3B82F6" mask="url(#badType)" />
    <rect id="e7" x="10" y="10" width="100" height="40" fill="#3B82F6" mask-type="alpha" />
  </svg>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.54: mask-type 布局可解析", ParseInto(r, res.layout));
        auto alphaOf = [&](const char* name) -> int {
            const uint16_t id = r.findNodeByName(name);
            if (!id) return -1;
            const auto& ms = r.maskOf(id);
            if (ms.empty()) return -1;
            return (int)((ms[0].head.color >> 24) & 0xFFu);
        };
        // 1) 默认亮度：#808080 的亮度 ≈ 0.502 → ≈128
        TEST("16.54: 默认（luminance）灰色 → ≈128", std::abs(alphaOf("e1") - 128) <= 2);
        TEST("16.54: 显式 mask-type=\"luminance\" 同默认",
             std::abs(alphaOf("e4") - 128) <= 2);
        // 2) alpha 型：亮度不参与 → 255
        TEST("16.54: mask-type=alpha 灰色 → 255", alphaOf("e2") == 255);
        // 3) alpha 型仍乘 opacity：0.5 → ≈128（颜色不重要，红色亮度本来只有 ~0.21）
        TEST("16.54: alpha 型仍乘 opacity（0.5 → ≈128）",
             std::abs(alphaOf("e3") - 128) <= 2);
        // 4) 渐变内容：alpha 型下色标 alpha 不再被亮度压暗
        {
            const uint16_t id = r.findNodeByName("e5");
            std::vector<BinUIRenderer::MaskRecord> ms;
            if (id) ms = r.maskOf(id);
            TEST("16.54: 渐变内容沿用 mask-type=alpha",
                 ms.size() == 1 && ms[0].hasGrad &&
                 ((ms[0].grad.stops[0].color >> 24) & 0xFFu) >= 250 &&
                 ((ms[0].grad.stops[1].color >> 24) & 0xFFu) >= 250);
        }
        // 5) 告警：认不出的值 + 写在非 <mask> 元素上
        {
            bool warnBad = false, warnPlace = false;
            for (const auto& w : res.warnings) {
                if (w.find("mask-type 只认 luminance / alpha") != std::string::npos) warnBad = true;
                if (w.find("mask-type 只对 <mask> 元素自身生效") != std::string::npos) warnPlace = true;
            }
            TEST("16.54: 认不出的 mask-type 值要告警", warnBad);
            TEST("16.54: mask-type 写在普通元素上要告警", warnPlace);
        }
    }

    // ---- 16.55：描边几何走 CPU 覆盖率（v0.17.1）----
    // 背景：描边以前直接发细长四边形，靠 GPU 的 4x MSAA 出 AA；但采样子像素位置由
    // API 决定，GL 与 Vulkan 在斜线上覆盖率差一档（实测浅斜线整条差 62/255、圆描边
    // 差 320 个像素），"三端逐像素相同"在斜线上根本不成立。改成 CPU 条带覆盖率高
    // 三端必然一致（像素门禁用 layer-stroke 钉住）。这里钉住几何与契约：
    //   1. 覆盖率加权面积 = 线长 x 线宽（不丢也不重）；
    //   2. 条带保持 RGB、alpha 有效、尺寸为正；
    //   3. 同一条线段重复调用逐位相同（无隐藏状态）；
    //   4. 45 度与 -45 度加权面积一致（对称性 —— 采样位置问题就出在这里）；
    //   5. 退化输入不产生条带；缓存 key 区分线宽与颜色。
    {
        using binui::PaintBand;
        std::vector<PaintBand> lb(binui::kMaxPathBands);
        std::vector<uint16_t> lcov;
        auto build = [&](float x0, float y0, float x1, float y1, float w, uint32_t c) {
            return binui::BuildLineBandsAA(x0, y0, x1, y1, w, c, lb.data(),
                                           static_cast<int>(lb.size()), lcov);
        };
        auto warea = [&](int n) {
            double s = 0;
            for (int i = 0; i < n; i++) {
                const double a = static_cast<double>((lb[i].color >> 24) & 0xFFu) / 255.0;
                s += static_cast<double>(lb[i].w) * static_cast<double>(lb[i].h) * a;
            }
            return s;
        };
        const uint32_t col = 0xFFB45309u;
        // 1) 水平线：长 100、宽 3 -> 300（AA 边缘按覆盖率加权，所以不是整像素数）
        const int nH = build(10.0f, 50.0f, 110.0f, 50.0f, 3.0f, col);
        TEST("16.55: 水平线的覆盖率加权面积 = 长 x 宽",
             nH > 0 && std::fabs(warea(nH) - 300.0) < 3.0);
        // 2) 45 度、长 141.42 -> 424.26
        const int nD = build(10.0f, 10.0f, 110.0f, 110.0f, 3.0f, col);
        TEST("16.55: 45 度线的覆盖率加权面积 = 长 x 宽",
             nD > 0 && std::fabs(warea(nD) - 424.26) < 4.25);
        // 3) 浅斜线（就是实测差 62/255 的那条）：长 121.66 -> 364.97
        const int nS = build(20.0f, 100.0f, 140.0f, 120.0f, 3.0f, col);
        TEST("16.55: 浅斜线的覆盖率加权面积 = 长 x 宽",
             nS > 0 && std::fabs(warea(nS) - 364.97) < 3.65);
        // 4) 颜色契约：RGB 保持、alpha 不超过输入、尺寸为正
        {
            bool ok = nS > 0;
            for (int i = 0; i < nS && ok; i++) {
                if ((lb[i].color & 0x00FFFFFFu) != (col & 0x00FFFFFFu)) ok = false;
                if (((lb[i].color >> 24) & 0xFFu) > 0xFFu) ok = false;
                if (!(lb[i].w > 0.0f) || !(lb[i].h > 0.0f)) ok = false;
            }
            TEST("16.55: 条带保持 RGB、alpha 有效、尺寸为正", ok);
        }
        // 5) 确定性：同一条线段两次算出来逐位相同
        {
            const std::vector<PaintBand> first(lb.begin(), lb.begin() + nS);
            const int n2 = build(20.0f, 100.0f, 140.0f, 120.0f, 3.0f, col);
            bool same = (n2 == nS);
            for (int i = 0; i < n2 && same; i++) {
                same = first[i].x == lb[i].x && first[i].y == lb[i].y &&
                       first[i].w == lb[i].w && first[i].h == lb[i].h &&
                       first[i].color == lb[i].color;
            }
            TEST("16.55: 同一条线段重复计算逐位相同", same);
        }
        // 6) 对称性：45 度与 -45 度加权面积一致
        const int nM = build(10.0f, 110.0f, 110.0f, 10.0f, 3.0f, col);
        TEST("16.55: 45 度与 -45 度面积一致",
             nM > 0 && std::fabs(warea(nM) - warea(nD)) < 0.5);
        // 7) 退化输入
        TEST("16.55: 零长度线段没有条带",
             build(50.0f, 50.0f, 50.0f, 50.0f, 3.0f, col) == 0);
        TEST("16.55: 零宽度线段没有条带",
             build(10.0f, 50.0f, 110.0f, 50.0f, 0.0f, col) == 0);
        // 8) 缓存 key
        const uint64_t k1 = binui::MakeLineBandKey(10.0f, 50.0f, 110.0f, 50.0f, 3.0f, col);
        const uint64_t k2 = binui::MakeLineBandKey(10.0f, 50.0f, 110.0f, 50.0f, 3.0f, col);
        const uint64_t k3 = binui::MakeLineBandKey(10.0f, 50.0f, 110.0f, 50.0f, 4.0f, col);
        const uint64_t k4 =
            binui::MakeLineBandKey(10.0f, 50.0f, 110.0f, 50.0f, 3.0f, 0xFFB4530Au);
        TEST("16.55: 缓存 key 相同输入相同", k1 == k2 && k1 != 0);
        TEST("16.55: 缓存 key 区分线宽与颜色", k1 != k3 && k1 != k4);
    }

    // ---- 16.56：pattern 内容裁到 tile 框（v0.17.2）----
    // ClipShapeConvex 是凸窗口的 Sutherland-Hodgman（<pattern> 的 tile 在设备空间可能是
    // 矩形，也可能是 patternTransform 旋转/斜切后的平行四边形 —— 两者都凸）。钉住四件事：
    //   1. 完全在窗口里的形状：ShapeInsideConvex 为真、裁剪不改点数（正常写法零变化）；
    //   2. 跨窗口：裁剪后点全在窗口里，且面积 = 交集面积；
    //   3. 完全在外：裁完为空；
    //   4. 平行四边形窗口同样成立，且裁剪后面积不超过窗口面积。
    {
        using binui::PathPointF;
        using binui::PathShape;
        auto makeRect = [](float x, float y, float w, float h) {
            PathShape s;
            const float xs[4] = { x, x + w, x + w, x };
            const float ys[4] = { y, y, y + h, y + h };
            for (int i = 0; i < 4; i++) s.pts.push_back({ xs[i], ys[i] });
            s.contours.push_back({ 0, 4, true });
            s.RecomputeBounds();
            return s;
        };
        auto areaOf = [](const PathShape& s) {
            double a = 0.0;
            for (const auto& c : s.contours) {
                double sum = 0.0;
                for (uint32_t i = 0; i < c.count; i++) {
                    const PathPointF& p = s.pts[c.first + i];
                    const PathPointF& q = s.pts[c.first + (i + 1) % c.count];
                    sum += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
                }
                a += sum * 0.5;
            }
            return a < 0 ? -a : a;
        };
        const PathPointF winSq[4] = { { 0, 0 }, { 40, 0 }, { 40, 40 }, { 0, 40 } };
        // 1) 完全在窗口内：判定为内，裁剪不改点数
        {
            PathShape s = makeRect(10, 10, 20, 20);
            const bool inside = binui::ShapeInsideConvex(s, winSq);
            const size_t before = s.pts.size();
            binui::ClipShapeConvex(s, winSq);
            TEST("16.56: 完全在 tile 内的内容判定为内、点数不变",
                 inside && s.pts.size() == before);
        }
        // 2) 跨窗口：面积 = 交集（[0,20]x[0,20] = 400）
        {
            PathShape s = makeRect(-10, -10, 30, 30);
            TEST("16.56: 越出 tile 的内容判定为越界", !binui::ShapeInsideConvex(s, winSq));
            binui::ClipShapeConvex(s, winSq);
            bool allInside = !s.Empty();
            for (const PathPointF& p : s.pts) {
                if (p.x < -0.01f || p.x > 40.01f || p.y < -0.01f || p.y > 40.01f)
                    allInside = false;
            }
            TEST("16.56: 裁剪后所有点都在 tile 内", allInside);
            TEST("16.56: 裁剪后面积等于交集面积", std::fabs(areaOf(s) - 400.0) < 0.5);
        }
        // 3) 完全在外：裁完为空
        {
            PathShape s = makeRect(50, 50, 20, 20);
            binui::ClipShapeConvex(s, winSq);
            TEST("16.56: 完全在 tile 外的形状裁完为空", s.Empty());
        }
        // 4) 平行四边形窗口（旋转 30 度再平移，模拟 patternTransform 的格）
        {
            const float ang = 3.14159265f / 6.0f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            auto rot = [&](float x, float y) {
                return PathPointF{ ca * x - sa * y + 100.0f, sa * x + ca * y + 100.0f };
            };
            const PathPointF winP[4] = { rot(0, 0), rot(40, 0), rot(40, 40), rot(0, 40) };
            const PathPointF center = rot(20, 20);
            const PathShape small = makeRect(center.x - 12, center.y - 12, 24, 24);
            TEST("16.56: 平行四边形窗口内的小块判定为内",
                 binui::ShapeInsideConvex(small, winP));
            PathShape big = makeRect(70, 70, 60, 60);
            binui::ClipShapeConvex(big, winP);
            const double bigArea = areaOf(big);
            TEST("16.56: 平行四边形窗口裁剪后非空且面积不超过窗口",
                 !big.Empty() && bigArea > 0.0 && bigArea <= 1600.5);
        }
    }

    // ---- 16.57：column 滚动列表里的行不收缩（v0.17.4）----
    // flexshow 的"② flex:1 + overflow:auto"卡在窗口拉矮时，12 行条目被压到十几像素，
    // 行里 16px 的文字却还是自己的字号 —— 溢出到相邻行，看上去像被纵向切掉一半；更怪的是
    // 滚动条照样出现（滚动范围按声明的 32px 算）。根因不是排版偏差：CSS 里 flex 子项默认
    // `flex-shrink: 1`，容器不够高就会收缩。这里钉住两条路径（编译期布局 + 运行期 setSize）：
    //   ① 写了 `flex-shrink: 0` 的行在溢出时**不缩**；
    //   ② 没写的行照 CSS 缩（对照组 —— 防止"把收缩整个关掉"这种糊涂修法）。
    // `min-height` 也能达到同样效果（实测两种写法渲染逐字节相同），但"这一项不许缩"的
    // 语义用 `flex-shrink: 0` 表达最直接。
    {
        const char* html = R"HTML(<html width="300" height="200"><body>
  <div id="list" style="display:flex; flex-direction:column; height:40; overflow:auto">
    <div id="a" style="width:100%; height:32; flex-shrink:0"></div>
    <div id="b" style="width:100%; height:32"></div>
  </div>
</body></html>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.57: 滚动列表页可解析",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto heightOf = [&](const char* name) -> float {
            const uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes())
                if (nd.id == nid) return nd.height;
            return -1.0f;
        };
        TEST("16.57: flex-shrink:0 的行溢出时不缩（保持 32）",
             std::fabs(heightOf("a") - 32.0f) < 0.5f);
        TEST("16.57: 没写 shrink 的行按 CSS 缩（对照组）", heightOf("b") < 32.0f);
        // 运行期改窗口尺寸：同一条规则必须继续成立（重排走的是同一份 flex 布局）
        r.setSize(300, 300);
        TEST("16.57: setSize 之后 flex-shrink:0 的行仍不缩",
             std::fabs(heightOf("a") - 32.0f) < 0.5f);
    }

    // ---- 16.27：毛玻璃 backdrop-filter（v0.16.27，格式 v29 毛玻璃段） ----
    // 钉住五件事：
    //   1. backdrop-filter: blur(Npx) 落进毛玻璃段（renderer.backdropOf），带半径与合成圆角；
    //   2. -webkit- 前缀同样认；
    //   3. none / blur(0) → 没有条目（元素照常画，只是不模糊）；
    //   4. 不支持的函数（saturate 等）→ 告警，但 blur 仍然生效；
    //   5. 没写的元素不受影响。
    {
        const char* html = R"HTML(<app_window width="300" height="240" bg="#101014">
<html width="300" height="240" bg="#101014">
<body>
  <div id="a1" style="position:absolute; left:20; top:20; width:120; height:80;
       border-radius: 16; backdrop-filter: blur(24px)"></div>
  <div id="a2" style="position:absolute; left:160; top:20; width:120; height:80;
       -webkit-backdrop-filter: blur(8px)"></div>
  <div id="a3" style="position:absolute; left:20; top:120; width:120; height:60;
       backdrop-filter: none"></div>
  <div id="a4" style="position:absolute; left:160; top:120; width:120; height:60;
       backdrop-filter: blur(0px)"></div>
  <div id="a5" style="position:absolute; left:20; top:190; width:120; height:60;
       backdrop-filter: blur(12px) saturate(1.4)"></div>
  <div id="a6" style="position:absolute; left:160; top:190; width:120; height:60"></div>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.27: 毛玻璃布局可解析", ParseInto(r, res.layout));

        auto bdOfName = [&](const char* name) -> const BinUIBackdropEntry* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.backdropOf(id) : nullptr;
        };
        // 1) blur(24px) + 圆角
        {
            const BinUIBackdropEntry* b = bdOfName("a1");
            TEST("16.27: blur(24px) 进段", b != nullptr);
            if (b) {
                TEST("16.27: 模糊半径 = 24", std::fabs(b->blur - 24.0f) < 0.01f);
                TEST("16.27: 合成圆角 = 节点圆角（16）",
                     std::fabs(b->radius - 16.0f) < 0.01f);
            }
        }
        // 2) -webkit- 前缀
        {
            const BinUIBackdropEntry* b = bdOfName("a2");
            TEST("16.27: -webkit-backdrop-filter 也认", b && std::fabs(b->blur - 8.0f) < 0.01f);
        }
        // 3) none / blur(0) 不产生条目
        TEST("16.27: backdrop-filter: none → 无条目", bdOfName("a3") == nullptr);
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("blur(0)") != std::string::npos) warned = true;
            TEST("16.27: blur(0) 要告警", warned);
            TEST("16.27: blur(0) → 无条目", bdOfName("a4") == nullptr);
        }
        // 4) 不支持的函数告警，blur 仍生效
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("backdrop-filter 只支持 blur()") != std::string::npos) warned = true;
            TEST("16.27: saturate() 要告警（只支持 blur）", warned);
            const BinUIBackdropEntry* b = bdOfName("a5");
            TEST("16.27: 混写时 blur 仍生效", b && std::fabs(b->blur - 12.0f) < 0.01f);
        }
        // 5) 没写的不受影响
        TEST("16.27: 没写 backdrop-filter → 无条目", bdOfName("a6") == nullptr);
    }

    // ---- 16.28：材质预设 material: glass/smoked/thin + 颗粒 grain（v0.16.28） ----
    // 预设只补"作者没写"的声明（可局部覆盖），颗粒强度打包进毛玻璃记录的 flags。
    {
        const char* html = R"HTML(<app_window width="300" height="300" bg="#101014">
<html width="300" height="300" bg="#101014">
<body>
  <div id="g1" style="position:absolute; left:20; top:20; width:120; height:80;
       material: glass"></div>
  <div id="g2" style="position:absolute; left:160; top:20; width:120; height:80;
       material: smoked; border-radius: 40"></div>
  <div id="g3" style="position:absolute; left:20; top:120; width:120; height:80;
       material: nope"></div>
  <div id="g4" style="position:absolute; left:160; top:120; width:120; height:80;
       backdrop-filter: blur(9px) grain(0.5)"></div>
  <div id="g5" style="position:absolute; left:20; top:220; width:120; height:60;
       backdrop-filter: blur(6px)"></div>
</body></html>
</app_window>)HTML";
        const auto res = binui::CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("16.28: 材质预设布局可解析", ParseInto(r, res.layout));

        auto bdOf = [&](const char* name) -> const BinUIBackdropEntry* {
            const uint16_t id = r.findNodeByName(name);
            return id ? r.backdropOf(id) : nullptr;
        };
        auto strengthOf = [](const BinUIBackdropEntry* b) -> int {
            return b ? static_cast<int>((b->flags >> 8) & 0xFFu) : -1;
        };
        // 1) glass 预设：blur 24 + 圆角 24 + 颗粒 0.7
        {
            const BinUIBackdropEntry* b = bdOf("g1");
            TEST("16.28: material: glass 带出毛玻璃", b != nullptr);
            if (b) {
                TEST("16.28: 预设模糊 = 24", std::fabs(b->blur - 24.0f) < 0.01f);
                TEST("16.28: 预设圆角 = 24", std::fabs(b->radius - 24.0f) < 0.01f);
                TEST("16.28: 预设开颗粒（bit0）", (b->flags & 1u) != 0);
                TEST("16.28: 颗粒强度 ≈ 0.7（179±2）", strengthOf(b) >= 177 && strengthOf(b) <= 181);
            }
            // 预设还补了渐变底 + 边框 + 投影
            const uint16_t id = r.findNodeByName("g1");
            const BinUIPaintNode* pn = r.paintOf(id);
            TEST("16.28: 预设补了渐变底", pn && (pn->flags & PAINT_GRADIENT));
            TEST("16.28: 预设补了 1px 边框", pn && (pn->flags & PAINT_BORDER));
        }
        // 2) 局部覆盖：只有 border-radius 被改成 40，其余沿用预设
        {
            const BinUIBackdropEntry* b = bdOf("g2");
            TEST("16.28: smoked 预设存在", b != nullptr);
            if (b) {
                TEST("16.28: 作者写的 border-radius 覆盖预设（40）",
                     std::fabs(b->radius - 40.0f) < 0.01f);
                TEST("16.28: smoked 的模糊 = 20", std::fabs(b->blur - 20.0f) < 0.01f);
                TEST("16.28: smoked 颗粒 ≈ 0.5（128±2）",
                     strengthOf(b) >= 126 && strengthOf(b) <= 130);
            }
        }
        // 3) 未知预设：告警 + 不产生毛玻璃
        {
            bool warned = false;
            for (const auto& w : res.warnings)
                if (w.find("不是支持的材质预设") != std::string::npos) warned = true;
            TEST("16.28: 未知材质预设要告警", warned);
            TEST("16.28: 未知预设 → 无毛玻璃", bdOf("g3") == nullptr);
        }
        // 4) 手写 grain(0.5)
        {
            const BinUIBackdropEntry* b = bdOf("g4");
            TEST("16.28: grain(0.5) 进段", b && std::fabs(b->blur - 9.0f) < 0.01f);
            TEST("16.28: grain(0.5) 强度 ≈ 128", strengthOf(b) >= 126 && strengthOf(b) <= 130);
        }
        // 5) 没写 grain：flags = 0（不撒颗粒）
        {
            const BinUIBackdropEntry* b = bdOf("g5");
            TEST("16.28: 没写 grain → flags=0", b && b->flags == 0);
        }
    }

    // ---- 03：display:none 必须连整棵子树一起不生成 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#101014">
<html width="400" height="300" bg="#101014">
<head><style>
  .hidden { display: none; }
  .card { background: #2C2C2E; border-radius: 8; }
</style></head>
<body>
  <div class="card" id="inlineHide" style="position:absolute; left:600; top:200; width:120; height:80; display:none">
    <div id="childA" style="position:absolute; left:20; top:20; width:60; height:30; background:#F25A5A"></div>
  </div>
  <div class="card hidden" id="classHide" style="position:absolute; left:640; top:200; width:120; height:80">
    <div id="childB" style="position:absolute; left:200; top:20; width:60; height:30; background:#F59E0B"></div>
  </div>
  <div class="card" id="visibleOne" style="position:absolute; left:160; top:200; width:300; height:80">
    <div id="childC" style="position:absolute; left:20; top:20; width:60; height:30; background:#4ED17E"></div>
  </div>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("03: 编译成功", ParseInto(r, bin));
        TEST("03: 内联 display:none 的容器不生成", r.findNodeByName("inlineHide") == 0);
        TEST("03: 内联 display:none 的子节点也不生成", r.findNodeByName("childA") == 0);
        TEST("03: class display:none 的容器不生成", r.findNodeByName("classHide") == 0);
        TEST("03: class display:none 的子节点也不生成", r.findNodeByName("childB") == 0);
        TEST("03: 可见容器仍在", r.findNodeByName("visibleOne") != 0);
        TEST("03: 可见容器的子节点仍在", r.findNodeByName("childC") != 0);
    }

    // ---- 04：没写 background 的 button / input 默认透明；按钮默认居中 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#FFFFFF">
<html width="400" height="300" bg="#FFFFFF"><body>
  <button id="plainBtn" style="position:absolute; left:20; top:20; width:120; height:32">按钮</button>
  <button id="bgBtn" style="position:absolute; left:20; top:60; width:120; height:32; background:#3E8BFF">按钮</button>
  <input id="plainInput" style="position:absolute; left:20; top:100; width:120; height:32"/>
  <button id="leftBtn" style="position:absolute; left:20; top:140; width:200; height:32; text-align:left">左</button>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("04: 编译成功", ParseInto(r, bin));
        const auto* plain = NodeById(r, r.findNodeByName("plainBtn"));
        const auto* bgb = NodeById(r, r.findNodeByName("bgBtn"));
        const auto* input = NodeById(r, r.findNodeByName("plainInput"));
        const auto* left = NodeById(r, r.findNodeByName("leftBtn"));
        TEST("04: 无 background 的按钮默认透明", plain && plain->color == 0u);
        TEST("04: 写了 background 的按钮不受影响", bgb && bgb->color == 0xFF3E8BFFu);
        TEST("04: 无 background 的输入框默认透明", input && input->color == 0u);
        // 后端从"写死居中"改成"读 flags"之后，默认居中必须由编译器补，
        // 否则所有没写 text-align 的按钮会突然变左对齐（演示观感全变）。
        TEST("04: 未写 text-align 的按钮默认居中", plain && (plain->flags & BINUI_TEXT_CENTER));
        TEST("04: text-align:left 的按钮不置居中/右对齐位",
             left && !(left->flags & (BINUI_TEXT_CENTER | BINUI_TEXT_RIGHT)));
    }

    // ---- 05：scrollbar-color / scrollbar-width 必须告警（不能静默忽略）----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#FFFFFF">
<html width="400" height="300" bg="#FFFFFF">
<head><style> .sc { scrollbar-color: #C7CBD1 #E5E7EB; scrollbar-width: thin; } </style></head>
<body><div class="sc" style="position:absolute; left:0; top:0; width:200; height:100; overflow:auto"></div></body>
</html></app_window>)HTML";
        const CompileResult res = CompileBinUIEx(html);
        bool sawScrollbar = false, sawApi = false;
        for (const auto& w : res.warnings) {
            if (w.find("scrollbar") != std::string::npos) sawScrollbar = true;
            if (w.find("binui_set_scrollbar_style") != std::string::npos) sawApi = true;
        }
        TEST("05: scrollbar-* 有编译警告", sawScrollbar);
        TEST("05: 警告里点了 binui_set_scrollbar_style", sawApi);
    }

    // ---- 06：未声明高度的文本节点要带 heightAuto 标记 ----
    {
        const char* html = R"HTML(<app_window width="400" height="300" bg="#FFFFFF">
<html width="400" height="300" bg="#FFFFFF"><body>
  <span id="autoH" style="position:absolute; left:20; top:20; width:300; font-size:13px">OK</span>
  <span id="fixedH" style="position:absolute; left:20; top:60; width:300; height:40; font-size:13px">OK</span>
</body></html>
</app_window>)HTML";
        const auto bin = CompileHTML(html);
        BinUIRenderer r;
        TEST("06: 编译成功", ParseInto(r, bin));
        const uint16_t a = r.findNodeByName("autoH");
        const uint16_t f = r.findNodeByName("fixedH");
        TEST("06: 未声明高度的文本节点 heightAuto=1", a != 0 && r.specHeightAuto(a));
        TEST("06: 显式声明高度的节点 heightAuto=0", f != 0 && !r.specHeightAuto(f));
    }
}

int run() {
    printf("=== test_compiler: XML/HTML5 -> binUI -> parse roundtrip ===\n\n");

    // 1. XML -> binUI
    {
        const char* xml =
            "<binui width=\"480\" height=\"800\" bg=\"#121212\">\n"
            "  <label x=\"16\" y=\"46\" w=\"300\" h=\"36\" color=\"#FFFFFF\" size=\"20\">Material Demo</label>\n"
            "  <button x=\"32\" y=\"276\" w=\"416\" h=\"48\" color=\"#BB86FC\" size=\"14\">CLICK ME</button>\n"
            "  <input x=\"32\" y=\"348\" w=\"416\" h=\"48\" color=\"#1E1E1E\" size=\"14\">Enter text...</input>\n"
            "</binui>\n";

        BinUIRenderer r;
        auto binui = CompileXML(xml);
        TEST("CompileXML produces data", !binui.empty());
        TEST("CompileXML parses", ParseInto(r, binui));
        const auto& nodes = r.getNodes();
        TEST("XML node count == 4", nodes.size() == 4);
        if (nodes.size() == 4) {
            TEST("XML root id 0", nodes[0].id == 0 && nodes[0].type == BINUI_ROOT);
            TEST("XML root size 480x800", nodes[0].width == 480 && nodes[0].height == 800);
            TEST("XML label text", nodes[1].text == "Material Demo");
            TEST("XML button interactive", nodes[2].type == BINUI_BUTTON && (nodes[2].flags & BINUI_INTERACTIVE));
            TEST("XML button text", nodes[2].text == "CLICK ME");
            TEST("XML input text", nodes[3].type == BINUI_INPUT && nodes[3].text == "Enter text...");
        }
    }

    // 2. HTML5 -> binUI
    {
        const char* html =
            "<html width=\"480\" height=\"800\" bg=\"#121212\">\n"
            "<body>\n"
            "  <h3 style=\"color:#FFFFFF\">HTML5 Demo</h3>\n"
            "  <button style=\"background:#BB86FC; height:48\">CLICK ME</button>\n"
            "  <input type=\"text\" value=\"Enter text...\">\n"
            "</body>\n"
            "</html>\n";

        BinUIRenderer r;
        auto binui = CompileHTML(html);
        TEST("CompileHTML produces data", !binui.empty());
        TEST("CompileHTML parses", ParseInto(r, binui));
        const auto& nodes = r.getNodes();
        TEST("HTML node count == 4", nodes.size() == 4);
        if (nodes.size() == 4) {
            TEST("HTML root id 0", nodes[0].id == 0 && nodes[0].type == BINUI_ROOT);
            TEST("HTML h3 label text", nodes[1].type == BINUI_LABEL && nodes[1].text == "HTML5 Demo");
            TEST("HTML button text", nodes[2].type == BINUI_BUTTON && nodes[2].text == "CLICK ME");
            TEST("HTML input value", nodes[3].type == BINUI_INPUT && nodes[3].text == "Enter text...");
        }
    }

    // 3. 自动识别格式（CompileBinUISource）
    {
        BinUIRenderer r;
        TEST("Auto-detect XML", ParseInto(r, CompileBinUISource(
            "<binui width=\"100\" height=\"100\"><label>hi</label></binui>")));
        TEST("Auto-detect HTML", ParseInto(r, CompileBinUISource(
            "<html width=\"100\" height=\"100\"><body><p>hi</p></body></html>")));
        TEST("Auto-detect HTML doctype", ParseInto(r, CompileBinUISource(
            "<!DOCTYPE html><html width=\"100\" height=\"100\"><body><p>hi</p></body></html>")));
    }

    // 4. 实际示例文件
    {
        BinUIRenderer r3;
        auto vision = CompileBinUIFile("tests/fixtures/vision.binui.xml");
        TEST("Compile tests/fixtures/vision.binui.xml", ParseInto(r3, vision));
        TEST("vision_monitor node count == 72", !vision.empty() && r3.getNodes().size() == 72);
    }

    // 4.5 真实父子层级
    {
        // XML 嵌套：card 包含 label，button 是 card 的兄弟
        const char* xml =
            "<binui width=\"200\" height=\"300\">"
            "<card x=\"0\" y=\"0\" w=\"100\" h=\"100\">"
            "<label x=\"5\" y=\"5\" w=\"50\" h=\"20\">child</label>"
            "</card>"
            "<button x=\"0\" y=\"200\" w=\"100\" h=\"30\">sibling</button>"
            "</binui>";
        BinUIRenderer r;
        auto ufl = CompileXML(xml);
        TEST("hier XML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("hier XML node count == 4", n.size() == 4);
        if (n.size() == 4) {
            TEST("XML root parentId == 0xFFFF",
                 n[0].type == BINUI_ROOT && n[0].parentId == 0xFFFF);
            TEST("XML card parentId == root", n[1].parentId == n[0].id);
            TEST("XML label parentId == card",
                 n[2].type == BINUI_LABEL && n[2].parentId == n[1].id);
            TEST("XML button parentId == root",
                 n[3].type == BINUI_BUTTON && n[3].parentId == n[0].id);
        }

        // HTML 嵌套：body 被跳过，div 的子节点应挂到 html 根
        const char* html =
            "<html width=\"200\" height=\"300\">"
            "<body>"
            "<div x=\"0\" y=\"0\" w=\"100\" h=\"100\">"
            "<span x=\"5\" y=\"5\">inner</span>"
            "</div>"
            "</body>"
            "</html>";
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(html);
        TEST("hier HTML parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        TEST("hier HTML node count == 3", m.size() == 3);
        if (m.size() == 3) {
            TEST("HTML root parentId == 0xFFFF",
                 m[0].type == BINUI_ROOT && m[0].parentId == 0xFFFF);
            TEST("HTML div parentId == root", m[1].parentId == m[0].id);
            TEST("HTML span parentId == div", m[2].parentId == m[1].id);
        }
    }

    // 4.7 margin / padding
    {
        // XML padding 存储
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"300\">"
            "<label x=\"0\" y=\"0\" w=\"100\" h=\"30\" padding=\"8\">pad</label>"
            "</binui>");
        TEST("pad XML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("XML padding stored", n.size() == 2 && n[1].padding == 8);

        // HTML：margin 影响流式布局，padding 存入节点
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"400\">"
            "<body>"
            "<div style=\"height:20\">a</div>"
            "<div style=\"height:20; margin:10\">b</div>"
            "<div style=\"height:20; padding:5\">c</div>"
            "</body>"
            "</html>");
        TEST("margin HTML parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        if (m.size() != 4) {
            TEST("margin HTML node count == 4", false);
        } else {
            TEST("flow a y==0", m[1].y == 0 && m[1].height == 20);
            TEST("margin b y==30", m[2].y == 30);
            TEST("margin b x==10", m[2].x == 10);
            TEST("margin b w==180", m[2].width == 180);
            TEST("flow c y==60", m[3].y == 60);
            TEST("padding c == 5", m[3].padding == 5);
        }
    }

    // 4.8 父级相对布局：容器的流式子元素排布在容器内部，容器自动扩展容纳
    {
        BinUIRenderer r;
        auto ufl = CompileHTML(
            "<html width=\"200\" height=\"400\">"
            "<body>"
            "<div style=\"background:#333; height:100\">"
            "<span style=\"height:20\">x</span>"
            "<span style=\"height:20\">y</span>"
            "</div>"
            "</body>"
            "</html>");
        TEST("container layout parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        if (n.size() != 4) {
            TEST("container node count == 4", false);
        } else {
            // root(0), div(1), span1(2), span2(3) —— 注意 span 是 label 类型
            TEST("div is LIST at y==0", n[1].type == BINUI_LIST && n[1].y == 0 && n[1].height == 100);
            TEST("span1 inside div y==0", n[2].type == BINUI_LABEL && n[2].parentId == n[1].id && n[2].y == n[1].y);
            TEST("span2 stacked inside div y==20", n[3].y == n[1].y + 20);
        }

        // 容器自动扩展：显式高度小于流式内容时撑大
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"400\">"
            "<body>"
            "<div style=\"height:10\">"
            "<span style=\"height:20\">a</span>"
            "</div>"
            "</body>"
            "</html>");
        TEST("grow layout parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        TEST("container auto-grow to fit child",
             m.size() == 3 && m[1].height >= 20);
    }

    // 4.9 实体解码
    {
        // XML：文本中的命名/数值实体
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"300\">"
            "<label x=\"0\" y=\"0\" w=\"100\" h=\"30\">Tom &amp; Jerry &#65;&#x42; a&nbsp;b</label>"
            "</binui>");
        TEST("entity XML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("XML text entities decoded",
             n.size() == 2 && n[1].text == "Tom & Jerry AB a\xC2\xA0" "b");

        // HTML：文本与 input value 属性中的实体
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"300\">"
            "<body>"
            "<p>a &lt; b &amp;&amp; c &gt; d</p>"
            "<input type=\"text\" value=\"x &quot;y&quot;\">"
            "</body>"
            "</html>");
        TEST("entity HTML parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        bool textOk = false, attrOk = false;
        for (const auto& nd : m) {
            if (nd.type == BINUI_LABEL && nd.text == "a < b && c > d") textOk = true;
            if (nd.type == BINUI_INPUT && nd.text == "x \"y\"") attrOk = true;
        }
        TEST("HTML text entities decoded", textOk);
        TEST("HTML attr entity decoded", attrOk);

        // 未知实体保留原样
        BinUIRenderer r3;
        auto ufl3 = CompileHTML(
            "<html width=\"100\" height=\"100\"><body><p>&unknown; ok</p></body></html>");
        TEST("unknown entity parses", ParseInto(r3, ufl3));
        bool unkOk = false;
        for (const auto& nd : r3.getNodes())
            if (nd.type == BINUI_LABEL && nd.text == "&unknown; ok") unkOk = true;
        TEST("unknown entity kept as-is", unkOk);
    }

    // 4.10 图片资源引用
    {
        auto res = CompileBinUIEx(
            "<html width=\"200\" height=\"300\">"
            "<body>"
            "<img src=\"a.png\" style=\"position:absolute; left:0; top:0; width:20; height:20\">"
            "<img src=\"b.png\" style=\"position:absolute; left:30; top:0; width:20; height:20\">"
            "<div>x</div>"
            "</body>"
            "</html>");
        TEST("img resources collected",
             res.images.size() == 2 && res.images[0] == "a.png" && res.images[1] == "b.png");

        BinUIRenderer r;
        TEST("img layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        if (r.getNodes().size() < 3) {
            TEST("img node count", false);
        } else {
            TEST("img[0] imageIndex==0",
                 r.getNodes()[1].type == BINUI_IMAGE && r.getNodes()[1].imageIndex == 0);
            TEST("img[1] imageIndex==1",
                 r.getNodes()[2].type == BINUI_IMAGE && r.getNodes()[2].imageIndex == 1);
        }

        // XML 资源引用
        auto res2 = CompileBinUIEx(
            "<binui width=\"100\" height=\"100\">"
            "<image x=\"0\" y=\"0\" w=\"20\" h=\"20\" src=\"icon.png\"/>"
            "</binui>");
        TEST("XML img resource",
             res2.images.size() == 1 && res2.images[0] == "icon.png");

        // 渲染器：imageIndex -> 资源路径 映射
        BinUIRenderer r3;
        std::unordered_map<std::string, std::vector<uint8_t>> resources;
        resources["a.png"] = { 1, 2, 3, 4 };
        r3.setImageOrder({ "a.png" });
        r3.loadResources(resources);
        TEST("getImage returns loaded image",
             r3.getImage(0) != nullptr && r3.getImage(0)->data.size() == 4);
        TEST("getImage out-of-range null", r3.getImage(5) == nullptr);
        TEST("getImage missing path null", r3.getImage(99) == nullptr);
    }

    // 4.11 百分比尺寸（width/height/left/top: N%，相对父容器内容盒）
    {
        // HTML：width/height % 相对父容器内容盒
        BinUIRenderer r;
        auto ufl = CompileHTML(
            "<html width=\"200\" height=\"100\">"
            "<body>"
            "<div style=\"width:50%; height:50%\">x</div>"
            "</body>"
            "</html>");
        TEST("pct HTML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("pct width 50% of 200", n.size() == 2 && std::fabs(n[1].width - 100) < 0.01f);
        TEST("pct height 50% of 100", std::fabs(n[1].height - 50) < 0.01f);

        // 流式 + % 高度：后续子元素按新高度下移
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"200\">"
            "<body>"
            "<div style=\"height:25%\">a</div>"
            "<div style=\"height:25%\">b</div>"
            "</body>"
            "</html>");
        TEST("pct flow parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        TEST("pct flow stacking",
             m.size() == 3 && std::fabs(m[1].height - 50) < 0.01f
             && std::fabs(m[2].y - 50) < 0.01f && std::fabs(m[2].height - 50) < 0.01f);

        // XML：x/y/w/h %
        BinUIRenderer r3;
        auto ufl3 = CompileXML(
            "<binui width=\"200\" height=\"200\">"
            "<card x=\"10%\" y=\"10%\" w=\"50%\" h=\"50%\">c</card>"
            "</binui>");
        TEST("pct XML parses", ParseInto(r3, ufl3));
        const auto& k = r3.getNodes();
        TEST("pct XML geometry",
             k.size() == 2 && std::fabs(k[1].x - 20) < 0.01f
             && std::fabs(k[1].y - 20) < 0.01f
             && std::fabs(k[1].width - 100) < 0.01f && std::fabs(k[1].height - 100) < 0.01f);

        // 根 width/height:100% -> 默认窗口尺寸 1280x720
        BinUIRenderer r4;
        auto ufl4 = CompileHTML("<html style=\"width:100%; height:100%\"><body>x</body></html>");
        TEST("pct root parses", ParseInto(r4, ufl4));
        TEST("pct root default size",
             !r4.getNodes().empty()
             && std::fabs(r4.getNodes()[0].width - 1280) < 0.01f
             && std::fabs(r4.getNodes()[0].height - 720) < 0.01f);

        // 缩放规格段：编译产物应带 resize spec，手写格式不带
        TEST("compiled layout has resize spec", r.hasResizeSpec());
        TEST("default layout has no resize spec",
             [] {
                 BinUIRenderer rr;
                 auto d = GenerateDefaultBinUI();
                 rr.loadLayout(d.data(), d.size());
                 return !rr.hasResizeSpec();
             }());
        // 手写默认布局里的按钮也要带"按钮文字默认居中"这条 UA 语义
        // （后端按 flags 画文字，漏了它就是左对齐）
        TEST("default layout button text centered",
             [] {
                 BinUIRenderer rr;
                 auto d = GenerateDefaultBinUI();
                 rr.loadLayout(d.data(), d.size());
                 for (const auto& n : rr.getNodes())
                     if (n.type == BINUI_BUTTON) return (n.flags & BINUI_TEXT_CENTER) != 0;
                 return false;
             }());
    }

    // 4b. min/max 尺寸约束：编译期即生效（初始布局），HTML 与 XML 均支持
    {
        // HTML：min-width 撑开自动宽节点（容器小于最小值时钳制）
        BinUIRenderer r1;
        auto u1 = CompileHTML(
            "<html width=\"100\" height=\"100\"><body>"
            "<button style=\"min-width:200\">x</button>"
            "</body></html>");
        TEST("html min-width parses", ParseInto(r1, u1));
        TEST("html min-width initial", std::fabs(r1.getNodes()[1].width - 200) < 0.01f);

        // HTML：max-width 钳制百分比宽度
        BinUIRenderer r2;
        auto u2 = CompileHTML(
            "<html width=\"300\" height=\"100\"><body>"
            "<button style=\"width:100%; max-width:120\">x</button>"
            "</body></html>");
        TEST("html max-width parses", ParseInto(r2, u2));
        TEST("html max-width initial", std::fabs(r2.getNodes()[1].width - 120) < 0.01f);

        // HTML：min/max-height 影响流式堆叠
        BinUIRenderer r3;
        auto u3 = CompileHTML(
            "<html width=\"200\" height=\"200\"><body>"
            "<div style=\"min-height:70\">a</div>"
            "<div style=\"height:100; max-height:40\">b</div>"
            "</body></html>");
        TEST("html height constraints parse", ParseInto(r3, u3));
        const auto& k = r3.getNodes();
        TEST("html min-height initial", std::fabs(k[1].height - 70) < 0.01f);
        TEST("html max-height initial", std::fabs(k[2].height - 40) < 0.01f);
        TEST("html flow uses clamped heights", std::fabs(k[2].y - 70) < 0.01f);

        // HTML：% 约束
        BinUIRenderer r4;
        auto u4 = CompileHTML(
            "<html width=\"400\" height=\"200\"><body>"
            "<button style=\"width:60; min-width:50%\">x</button>"
            "</body></html>");
        TEST("html pct constraint parses", ParseInto(r4, u4));
        TEST("html pct min-width initial",
             std::fabs(r4.getNodes()[1].width - 200) < 0.01f);

        // HTML：flex 子项 max-width
        BinUIRenderer r5;
        auto u5 = CompileHTML(
            "<html width=\"600\" height=\"100\"><body>"
            "<div style=\"display:flex; height:50\">"
            "<button style=\"flex:1; max-width:100\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "</div></body></html>");
        TEST("html flex constraint parses", ParseInto(r5, u5));
        const auto& f = r5.getNodes();
        TEST("html flex max-width initial", std::fabs(f[2].width - 100) < 0.01f);
        TEST("html flex sibling initial", std::fabs(f[3].width - 300) < 0.01f);

        // XML：属性形式
        BinUIRenderer r6;
        auto u6 = CompileXML(
            "<binui width=\"300\" height=\"100\">"
            "<button x=\"0\" y=\"0\" w=\"200\" max-width=\"90\">x</button>"
            "</binui>");
        TEST("xml constraint parses", ParseInto(r6, u6));
        TEST("xml max-width initial", std::fabs(r6.getNodes()[1].width - 90) < 0.01f);

        // XML：min-height
        BinUIRenderer r7;
        auto u7 = CompileXML(
            "<binui width=\"300\" height=\"100\">"
            "<card x=\"0\" y=\"0\" w=\"100\" h=\"10\" min-height=\"60\">c</card>"
            "</binui>");
        TEST("xml min-height parses", ParseInto(r7, u7));
        TEST("xml min-height initial", std::fabs(r7.getNodes()[1].height - 60) < 0.01f);
    }

    // 5. 无效输入安全失败
    {
        TEST("Empty source -> empty output", CompileBinUISource("").empty());
        TEST("Random bytes -> no crash", CompileBinUISource("hello world this is not markup").size() < (1u << 20));
    }

    // 6. 运行时图片 API：updateImage / updateImageRaw / imageGeneration
    {
        const char* src =
            "<binui width=\"320\" height=\"240\">"
            "<image x=\"8\" y=\"8\" w=\"64\" h=\"64\" src=\"a.png\"></image>"
            "</binui>";
        auto binui = CompileBinUISource(src);
        BinUIRenderer r;
        TEST("image layout parses", ParseInto(r, binui));
        r.setImageOrder({ "a.png" });
        TEST("image initial generation 0", r.imageGeneration(0) == 0);
        TEST("getImage null before load", r.getImage(0) == nullptr);

        const uint8_t png[] = { 0x89, 0x50, 0x4E, 0x47 }; // 无效 PNG 字节，仅测 API 通路
        TEST("updateImage replaces data", r.updateImage(0, png, sizeof png));
        const ImageResource* img = r.getImage(0);
        TEST("updateImage generation bumped", r.imageGeneration(0) == 1 && img && !img->data.empty());
        TEST("updateImage not raw", img && !img->rawPixels);
        TEST("updateImage data preserved", img && img->data[0] == 0x89 && img->data.size() == 4);

        uint8_t rgba[4 * 2 * 2] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255 };
        TEST("updateImageRaw replaces pixels", r.updateImageRaw(0, 2, 2, rgba));
        const ImageResource* raw = r.getImage(0);
        TEST("updateImageRaw generation bumped", r.imageGeneration(0) == 2 && raw && raw->rawPixels);
        TEST("updateImageRaw dims stored", raw && raw->width == 2 && raw->height == 2 && raw->channels == 4);
        TEST("updateImageRaw pixels copied", raw && raw->data.size() == 16 && raw->data[0] == 255 && raw->data[15] == 255);
    }

    // 7. <app_window> 作根元素：XML 自动合成 ROOT 节点
    {
        auto u = CompileBinUISource(
            "<app_window width=\"800\" height=\"600\" bg=\"#202030\" title=\"Win\">"
            "<label x=\"10\" y=\"10\" w=\"120\" h=\"24\">hi</label>"
            "</app_window>");
        BinUIRenderer r;
        TEST("app_window-root parses", ParseInto(r, u));
        TEST("app_window-root has synthesized ROOT id 0",
             !r.getNodes().empty() && r.getNodes()[0].id == 0 &&
             r.getNodes()[0].type == BINUI_ROOT);
        TEST("app_window-root root size from window",
             std::fabs(r.getNodes()[0].width - 800) < 0.01f &&
             std::fabs(r.getNodes()[0].height - 600) < 0.01f);
        TEST("app_window-root root bg from window", r.getNodes()[0].color == 0xFF202030u);
        TEST("app_window-root children parented to root",
             r.getNodes().size() > 1 && r.getNodes()[1].parentId == 0);
        TEST("app_window-root window desc present", r.windowDesc().present &&
             r.windowDesc().width == 800 && r.windowDesc().height == 600 &&
             r.windowDesc().title == "Win");
    }

    // 8. <app_window> 包裹层 + HTML 正文：仍按 HTML 解析
    {
        auto res = CompileBinUIEx(
            "<!DOCTYPE html><app_window width=\"500\" height=\"400\" bg=\"#101020\" title=\"H\">"
            "<div id=\"box\" x=\"10\" y=\"10\" width=\"200\" height=\"50\">hello</div>"
            "</app_window>");
        BinUIRenderer r;
        TEST("app_window+html doc stays HTML",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t box = r.findNodeByName("box");
        TEST("app_window+html has div child", box != 0);
        TEST("app_window+html window desc present", r.windowDesc().present &&
             r.windowDesc().title == "H");
    }

    // 9. <video> 编译（HTML）：BINUI_VIDEO 节点 + 伪图片槽 + 视频描述段
    {
        const char* html =
            "<html><body style=\"width:640px;height:480px;background:#111111\">"
            "<video id=\"vid\" src=\"media/sample.mp4\" controls autoplay loop "
            "volume=\"0.8\" x=\"20\" y=\"20\" width=\"600\" height=\"400\"></video>"
            "</body></html>";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("video layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t vid = r.findNodeByName("vid");
        TEST("video node found by id", vid != 0);
        bool hasVideoType = false;
        for (const auto& n : r.getNodes())
            if (n.id == vid) { hasVideoType = (n.type == BINUI_VIDEO); break; }
        TEST("video node type BINUI_VIDEO", hasVideoType);
        TEST("one video in descriptor table", r.getVideos().size() == 1);
        const BinUIRenderer::VideoState* v = r.findVideo(vid);
        TEST("findVideo returns state", v != nullptr);
        TEST("video src compiled", v && v->src == "media/sample.mp4");
        TEST("video flags autoplay|loop|controls",
             v && (v->flags & VID_AUTOPLAY) && (v->flags & VID_LOOP) && (v->flags & VID_CONTROLS));
        TEST("video muted flag absent", v && !(v->flags & VID_MUTED));
        TEST("video volume parsed", v && v->volume > 0.79f && v->volume < 0.81f);
        TEST("video pseudo image slot allocated", res.images.size() >= 1 &&
             res.images[0] == "media/sample.mp4" && v && v->imageIndex == 0);
        // loadLayout 内部首帧 stepAnimations 会触发 autoplay，故只断言时钟/时长未动
        TEST("video initial runtime state", v && v->position == 0.0 && v->duration == 0.0 && !v->ended);
    }

    // 10. 无 <video> 布局：视频表为空
    {
        auto res = CompileBinUIEx("<html><body style=\"width:320px;height:240px\"><p>plain</p></body></html>");
        BinUIRenderer r;
        r.loadLayout(res.layout.data(), res.layout.size());
        TEST("no-video layout yields empty table", r.getVideos().empty());
        TEST("no-video findVideo null", r.findVideo(0) == nullptr);
    }

    // 11. <video> 编译（XML）与默认值
    {
        const char* xml =
            "<binui width=\"480\" height=\"360\" bg=\"#101010\">"
            "<video x=\"10\" y=\"10\" w=\"460\" h=\"300\" src=\"a.mp4\" muted></video>"
            "</binui>";
        auto res = CompileBinUIEx(xml);
        BinUIRenderer r;
        TEST("xml video layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::VideoState* v = !r.getVideos().empty() ? &r.getVideos()[0] : nullptr;
        TEST("xml video desc exists", v != nullptr);
        TEST("xml video muted only", v && (v->flags & VID_MUTED) &&
             !(v->flags & VID_AUTOPLAY) && !(v->flags & VID_CONTROLS));
        TEST("xml video default volume", v && v->volume == 1.0f);
    }

    // 12. z-index 层叠（v10）：解析 + 绘制顺序 + 命中测试按层顶优先
    {
        const char* html =
            "<html><body style=\"width:200px;height:200px;background:#000000\">"
            "<button id=\"low\" style=\"position:absolute;left:0;top:0;width:100px;"
            "height:100px;z-index:1\">A</button>"
            "<button id=\"high\" style=\"position:absolute;left:0;top:0;width:100px;"
            "height:100px;z-index:9\">B</button>"
            "</body></html>";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("zindex layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t low = r.findNodeByName("low");
        uint16_t high = r.findNodeByName("high");
        TEST("zindex nodes found", low != 0 && high != 0);

        const auto& nodes = r.getNodes();
        auto zOf = [&](uint16_t id) -> int {
            for (const auto& n : nodes) if (n.id == id) return n.zIndex;
            return -1000;
        };
        TEST("zindex parsed low=1", zOf(low) == 1);
        TEST("zindex parsed high=9", zOf(high) == 9);
        TEST("zindex default 0", zOf(0) == 0); // 根未指定

        // 绘制顺序：z 高者靠后（更靠上）
        auto order = r.drawOrder();
        auto posOf = [&](uint16_t id) -> int {
            for (size_t k = 0; k < order.size(); k++)
                if (nodes[order[k]].id == id) return (int)k;
            return -1;
        };
        TEST("zindex draw order high after low", posOf(high) > posOf(low) && posOf(high) >= 0);

        // 命中测试：重叠区域内层顶（z 高）胜出
        TEST("zindex hitTest topmost", r.hitTest(50, 50) == (int)high);
    }

    // 13. z-index 未指定时：同层 absolute 按 DOM 顺序，后声明者在上
    {
        const char* html =
            "<html><body style=\"width:200px;height:200px\">"
            "<button id=\"first\" style=\"position:absolute;left:0;top:0;width:100px;height:100px\">1</button>"
            "<button id=\"second\" style=\"position:absolute;left:0;top:0;width:100px;height:100px\">2</button>"
            "</body></html>";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("zindex-default parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t first = r.findNodeByName("first");
        uint16_t second = r.findNodeByName("second");
        TEST("zindex-default dom-order hit", r.hitTest(50, 50) == (int)second && first != second);
    }

    // 14. 长度单位：px / 无单位 / % / em / rem / vw / vh
    {
        auto res = CompileBinUIEx(
            "<html><body style=\"width:1280px;height:720px\">"
            "<div id=\"emu\"  style=\"left:0;top:0;width:10em;height:20vh;font-size:14px\"></div>"
            "<div id=\"remu\" style=\"left:0;top:0;width:2rem;height:50%\"></div>"
            "<div id=\"vwu\"  style=\"left:0;top:0;width:25vw;height:10px\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("units: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto boxOf = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* emu = boxOf("emu");
        // 10em × 14px = 140；20vh = 0.2 × 720 = 144
        TEST("units: em resolves against font-size (10em -> 140)",
             emu && std::fabs(emu->width - 140.0f) < 0.5f);
        TEST("units: vh resolves against viewport (20vh -> 144)",
             emu && std::fabs(emu->height - 144.0f) < 0.5f);
        const auto* remu = boxOf("remu");
        // rem 基准 = 根字号 14 -> 2rem = 28
        TEST("units: rem resolves against root font (2rem -> 28)",
             remu && std::fabs(remu->width - 28.0f) < 0.5f);
        const auto* vwu = boxOf("vwu");
        // 25vw = 0.25 × 1280 = 320
        TEST("units: vw resolves against viewport (25vw -> 320)",
             vwu && std::fabs(vwu->width - 320.0f) < 0.5f);
        TEST("units: valid input produces no warnings", res.warnings.empty());
    }

    // 15. 不支持的单位 / calc()：必须报警告，不能静默当 px
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"bad\" style=\"left:0;top:0;width:12pt;height:30\"></div>"
            "<div id=\"fn\"  style=\"left:0;top:40;width:calc(50% - 10px);height:30\"></div>"
            "</body></html>");
        bool sawPt = false, sawCalc = false;
        for (const auto& w : res.warnings) {
            if (w.find("pt") != std::string::npos) sawPt = true;
            if (w.find("calc") != std::string::npos) sawCalc = true;
        }
        TEST("units: pt warns instead of silent px", sawPt);
        TEST("units: calc() warns instead of silent 0", sawCalc);
        BinUIRenderer r;
        TEST("units: bad-unit UI still parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
    }

    // 16. 颜色：命名色 / transparent / !important / rgb() 百分比 / 现代 slash / hsl / #AARRGGBB
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"c1\" style=\"left:0;top:0;width:10px;height:10px;background:red\"></div>"
            "<div id=\"c2\" style=\"left:0;top:0;width:10px;height:10px;background:transparent\"></div>"
            "<div id=\"c3\" style=\"left:0;top:0;width:10px;height:10px;background:#00ff00 !important\"></div>"
            "<div id=\"c4\" style=\"left:0;top:0;width:10px;height:10px;background:rgb(100%, 0%, 0%)\"></div>"
            "<div id=\"c5\" style=\"left:0;top:0;width:10px;height:10px;background:rgb(0 0 255 / 50%)\"></div>"
            "<div id=\"c6\" style=\"left:0;top:0;width:10px;height:10px;background:hsl(120, 100%, 25%)\"></div>"
            "<div id=\"c7\" style=\"left:0;top:0;width:10px;height:10px;background:#80112233\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("colors: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.color;
            return 0xDEADBEEFu;
        };
        TEST("colors: named 'red' -> FFFF0000", colorOf("c1") == 0xFFFF0000u);
        TEST("colors: 'transparent' -> alpha 0", colorOf("c2") == 0x00000000u);
        TEST("colors: '!important' stripped (no alpha loss)", colorOf("c3") == 0xFF00FF00u);
        TEST("colors: rgb() percentage channels", colorOf("c4") == 0xFFFF0000u);
        TEST("colors: modern 'rgb(r g b / a)'", colorOf("c5") == 0x800000FFu);
        TEST("colors: hsl(120,100%,25%) -> green", colorOf("c6") == 0xFF008000u);
        TEST("colors: #AARRGGBB kept as binUI convention", colorOf("c7") == 0x80112233u);

        bool sawArgbNote = false;
        for (const auto& w : res.warnings)
            if (w.find("AARRGGBB") != std::string::npos) sawArgbNote = true;
        TEST("colors: 8-digit hex warns about AARRGGBB vs CSS RRGGBBAA", sawArgbNote);

        // 无法识别的颜色：保留默认色 + 警告
        auto bad = CompileBinUIEx(
            "<html><body><div id=\"cb\" style=\"left:0;top:0;width:10px;height:10px;background:notacolor\"></div>"
            "</body></html>");
        bool sawBad = false;
        for (const auto& w : bad.warnings)
            if (w.find("notacolor") != std::string::npos) sawBad = true;
        TEST("colors: unknown color warns", sawBad);
    }

    // 17. XML 侧同样吃 em/rem/vw/vh 与命名色
    {
        auto res = CompileBinUIEx(
            "<binui width=\"800\" height=\"600\">"
            "<label id=\"xl\" x=\"10\" y=\"10\" w=\"10em\" size=\"20\" color=\"tomato\">t</label>"
            "</binui>");
        BinUIRenderer r;
        TEST("xml-units: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t xl = r.findNodeByName("xl");
        for (const auto& nd : r.getNodes()) {
            if (nd.id != xl) continue;
            TEST("xml-units: 10em with size=20 -> 200", std::fabs(nd.width - 200.0f) < 0.5f);
            TEST("xml-colors: named 'tomato' -> FFFF6347", nd.color == 0xFFFF6347u);
        }
    }

    // 18. flex 主轴分配：百分比刚性项占位 / 溢出不出负偏移 / space-around / column 分高度
    {
        auto res = CompileBinUIEx(
            "<html><body style=\"width:1264px;height:200px\">"
            "<div id=\"row\" style=\"display:flex;height:32px;gap:8px;justify-content:space-between\">"
            "<span id=\"pa\" style=\"width:50%;height:20px\">A</span>"
            "<span id=\"fb\" style=\"flex:1;height:20px\">B</span>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex: mixed parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto nodeOf = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* pa = nodeOf("pa");
        const auto* fb = nodeOf("fb");
        // 回归：百分比宽必须先占位（以前没进 fixedSum，弹性项重叠并溢出容器）
        TEST("flex: percent child keeps 50% (632)", pa && std::fabs(pa->width - 632.0f) < 0.5f);
        TEST("flex: flexible child gets remainder (624)", fb && std::fabs(fb->width - 624.0f) < 0.5f);
        TEST("flex: no overlap (B starts after A + gap)",
             pa && fb && std::fabs(fb->x - (pa->x + pa->width + 8.0f)) < 0.5f);
        TEST("flex: row exactly fills container", fb && std::fabs(fb->x + fb->width - 1264.0f) < 0.5f);
    }
    {
        // 回归：溢出时 space-between 不得算出负间距（曾把第二项推到第一项左边）
        auto res = CompileBinUIEx(
            "<html><body style=\"width:300px;height:100px\">"
            "<div style=\"display:flex;height:40px;justify-content:space-between\">"
            "<div id=\"o1\" style=\"width:200px;height:20px\"></div>"
            "<div id=\"o2\" style=\"width:200px;height:20px\"></div>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex overflow parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t o2 = r.findNodeByName("o2");
        for (const auto& nd : r.getNodes()) {
            if (nd.id != o2) continue;
            // v22：flex-shrink 生效（CSS 默认 1）。两项各 200、容器只有 300 → 各让 50 收缩到 150，
            // 而不是像以前那样"固定宽不缩、直接溢出"。目的仍是"不许出现负间距"。
            TEST("flex overflow: shrink instead of negative spacing (o2.x == 150)",
                 std::fabs(nd.x - 150.0f) < 0.5f);
        }
    }
    {
        // space-around：首尾各半个间隔（CSS 语义），之前首尾给的是整份
        auto res = CompileBinUIEx(
            "<html><body style=\"width:600px;height:100px\">"
            "<div style=\"display:flex;height:40px;justify-content:space-around\">"
            "<div id=\"s1\" style=\"width:100px;height:20px\"></div>"
            "<div id=\"s2\" style=\"width:100px;height:20px\"></div>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex space-around parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        float x1 = -1, x2 = -1;
        for (const auto& nd : r.getNodes()) {
            if (nd.id == r.findNodeByName("s1")) x1 = nd.x;
            if (nd.id == r.findNodeByName("s2")) x2 = nd.x;
        }
        TEST("flex space-around: half gap at edges (100 / 400)",
             std::fabs(x1 - 100.0f) < 0.5f && std::fabs(x2 - 400.0f) < 0.5f);
    }
    {
        // column：flex:1 沿主轴（高度）分配，而不是错作用在宽度上
        auto res = CompileBinUIEx(
            "<html><body style=\"width:300px;height:400px\">"
            "<div style=\"display:flex;flex-direction:column;height:400px;gap:10\">"
            "<div id=\"c1\" style=\"flex:1;width:50px\"></div>"
            "<div id=\"c2\" style=\"flex:1\"></div>"   // 未声明宽 -> 交叉轴拉伸
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex column parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::NodeInfo* c1 = nullptr;
        const BinUIRenderer::NodeInfo* c2 = nullptr;
        for (const auto& nd : r.getNodes()) {
            if (nd.id == r.findNodeByName("c1")) c1 = &nd;
            if (nd.id == r.findNodeByName("c2")) c2 = &nd;
        }
        TEST("flex column: main axis = height ((400-10)/2 = 195)",
             c1 && std::fabs(c1->height - 195.0f) < 0.5f);
        TEST("flex column: second below first + gap",
             c1 && c2 && std::fabs(c2->y - (c1->y + c1->height + 10.0f)) < 0.5f);
        TEST("flex column: declared cross size kept (50)", c1 && std::fabs(c1->width - 50.0f) < 0.5f);
        TEST("flex column: undeclared cross size stretches (300)", c2 && std::fabs(c2->width - 300.0f) < 0.5f);
    }

    // 18b. column 里的 flex:1 容器：高度由父级剩余空间分配，不许被"内容撑高"改写
    //      （回归：撑满剩余高度的容器塌成内容高，后面的兄弟却仍按分配值排 → 错位/重叠；
    //        flex:1 + overflow:auto 的滚动视口也跟着没了）
    {
        auto res = CompileBinUIEx(
            "<html><body style=\"width:400px;height:600px\">"
            "<div id=\"col\" style=\"display:flex;flex-direction:column;height:100%\">"
            "<div id=\"head\" style=\"height:44px\"></div>"
            "<div id=\"mid\" style=\"flex:1;display:flex;flex-direction:column\">"
            "<div id=\"fill\" style=\"flex:1\"></div>"
            "</div>"
            "<div id=\"foot\" style=\"height:28px\"></div>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex column fill parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto nodeOf2 = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* mid  = nodeOf2("mid");
        const auto* fill = nodeOf2("fill");
        const auto* foot = nodeOf2("foot");
        TEST("flex column fill: flexed container keeps remainder (528)",
             mid && std::fabs(mid->height - 528.0f) < 0.5f);
        TEST("flex column fill: nested flex:1 fills it (528)",
             fill && std::fabs(fill->height - 528.0f) < 0.5f);
        TEST("flex column fill: next sibling sits after the flexed box",
             foot && mid && std::fabs(foot->y - (mid->y + mid->height)) < 0.5f);

        // 运行时改窗口尺寸：同一条规则必须继续成立（重排不能又把它塌回内容高）
        r.setSize(400, 400);
        mid = nodeOf2("mid"); fill = nodeOf2("fill"); foot = nodeOf2("foot");
        TEST("flex column fill: after setSize(400,400) remain = 328",
             mid && std::fabs(mid->height - 328.0f) < 0.5f);
        TEST("flex column fill: nested child follows (328)",
             fill && std::fabs(fill->height - 328.0f) < 0.5f);
        TEST("flex column fill: footer follows after resize",
             foot && mid && std::fabs(foot->y - (mid->y + mid->height)) < 0.5f);
    }
    {
        // flex:1 的滚动容器：视口高度 = 分配高度（不是内容高度），否则滚动条永远不出现
        auto res = CompileBinUIEx(
            "<html><body style=\"width:300px;height:300px\">"
            "<div style=\"display:flex;flex-direction:column;height:300px\">"
            "<div id=\"hdr\" style=\"height:40px\"></div>"
            "<div id=\"list\" style=\"flex:1;overflow:auto\">"
            "<div style=\"height:900px\"></div>"
            "</div></div></body></html>");
        BinUIRenderer r;
        TEST("flex scroll viewport parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t lid = r.findNodeByName("list");
        for (const auto& nd : r.getNodes()) {
            if (nd.id != lid) continue;
            TEST("flex scroll viewport: height = remainder (260)", std::fabs(nd.height - 260.0f) < 0.5f);
            TEST("flex scroll viewport: marked scrollable", (nd.flags & BINUI_SCROLLABLE) != 0);
        }
    }

    // 19. 命名实体表 / <br> 真实换行 / input 默认标签 / LIST 文本字号
    {
        auto res = CompileBinUIEx(
            "<html><body><p id=\"e\">A&mdash;B&hellip;C&pi;&rarr;&hearts;&frac12;&epsilon;"
            "&unknownent;</p></body></html>");
        BinUIRenderer r;
        TEST("entities: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        std::string got;
        for (const auto& nd : r.getNodes())
            if (nd.id == r.findNodeByName("e")) got = nd.text;
        // — … π → ♥ ½ ε + 未知实体原样保留
        const std::string want = "A\xE2\x80\x94" "B\xE2\x80\xA6" "C\xCF\x80" "\xE2\x86\x92"
                                 "\xE2\x99\xA5" "\xC2\xBD" "\xCE\xB5" "&unknownent;";
        TEST("entities: named table decodes (mdash/hellip/pi/rarr/hearts/frac12/epsilon)",
             got == want);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body><p id=\"mb\">line1<br>line2<br>line3</p></body></html>");
        BinUIRenderer r;
        TEST("br: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        TEST("br: node count has no placeholder node", r.getNodes().size() == 2);
        const BinUIRenderer::NodeInfo* mb = nullptr;
        for (const auto& nd : r.getNodes())
            if (nd.id == r.findNodeByName("mb")) mb = &nd;
        TEST("br: becomes '\\n' in text", mb && mb->text == "line1\nline2\nline3");
        TEST("br: node grows to fit lines (h > 20*2)", mb && mb->height > 40.0f);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<input id=\"s\" type=\"submit\"><input id=\"r\" type=\"reset\">"
            "<input id=\"b\" type=\"button\"><input id=\"t\" type=\"text\">"
            "</body></html>");
        BinUIRenderer r;
        TEST("input default: parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto nodeOf = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* s = nodeOf("s");
        const auto* rr = nodeOf("r");
        TEST("input default: submit -> BUTTON 'Submit'",
             s && s->type == BINUI_BUTTON && s->text == "Submit");
        TEST("input default: reset -> BUTTON 'Reset'",
             rr && rr->type == BINUI_BUTTON && rr->text == "Reset");
        TEST("input default: text stays INPUT", nodeOf("t") && nodeOf("t")->type == BINUI_INPUT);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body><div id=\"lf\" style=\"height:60px;font-size:20px\">hello</div>"
            "<div id=\"ld\" style=\"height:60px\">list text</div></body></html>");
        BinUIRenderer r;
        TEST("list font: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        for (const auto& nd : r.getNodes()) {
            if (nd.id == r.findNodeByName("lf")) TEST("list font: node.fontSize == 20", nd.fontSize == 20);
            if (nd.id == r.findNodeByName("ld")) TEST("list font: default 14", nd.fontSize == 14);
        }
    }

    // 20. text-align（flags 空闲位，不升格式版本）/ 静态 opacity / <li> 符号 / 不支持属性警告
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<p id=\"al\" style=\"height:20px\">l</p>"
            "<p id=\"ac\" style=\"height:20px;text-align:center\">c</p>"
            "<p id=\"ar\" style=\"height:20px;text-align:right\">r</p>"
            "</body></html>");
        BinUIRenderer r;
        TEST("align: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto flagsOf = [&](const char* name) -> int {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.flags;
            return -1;
        };
        TEST("align: default is left (no bits)",
             flagsOf("al") >= 0 && !(flagsOf("al") & BINUI_TEXT_CENTER) && !(flagsOf("al") & BINUI_TEXT_RIGHT));
        TEST("align: center sets flag", flagsOf("ac") & BINUI_TEXT_CENTER);
        TEST("align: right sets flag", flagsOf("ar") & BINUI_TEXT_RIGHT);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"opwrap\" style=\"height:60px;opacity:0.5\">"
            "  <div id=\"opchild\" style=\"height:20px;background:#0000FF\"></div>"
            "</div>"
            "<div id=\"op\" style=\"height:20px;background:#FF0000;opacity:0.5\"></div>"
            "<div id=\"op0\" style=\"height:20px;background:#00FF00;opacity:0\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("opacity: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.color;
            return 0xDEADBEEFu;
        };
        auto opacityOf = [&](const char* name) -> float {
            return r.drawOpacity(r.findNodeByName(name));
        };
        auto ownOpOf = [&](const char* name) -> float {
            return r.staticOpacity(r.findNodeByName(name));
        };
        // 0.16.8：静态 opacity 不再折进颜色（那样**子节点完全不受影响**），
        // 改成存进扩展绘制段的 reserve[0]，由 drawOpacity() 按祖先链级联。
        TEST("opacity: 颜色保持完全不透明", colorOf("op") == 0xFFFF0000u);
        TEST("opacity: 0.5 存进 reserve[0]=128", std::fabs(ownOpOf("op") - 0.5f) < 0.005f);
        TEST("opacity: 0 存成 0", ownOpOf("op0") <= 0.001f);
        TEST("opacity: drawOpacity 自己是 0.5", std::fabs(opacityOf("op") - 0.5f) < 0.005f);
        // 关键：父元素 opacity:0.5，子元素没有 opacity —— 子元素也要被衰减（CSS 组语义）
        TEST("opacity: 级联到子节点（父 0.5 → 子 0.5）",
             std::fabs(opacityOf("opchild") - 0.5f) < 0.005f);
        TEST("opacity: 子节点自己的静态 opacity 仍是 1", std::fabs(ownOpOf("opchild") - 1.0f) < 0.001f);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body><ul><li id=\"u1\">one</li></ul>"
            "<ol><li id=\"o1\">first</li><li id=\"o2\">second</li></ol></body></html>");
        BinUIRenderer r;
        TEST("li: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto textOf = [&](const char* name) -> std::string {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.text;
            return {};
        };
        TEST("li: ul gets bullet", textOf("u1") == "\xE2\x80\xA2 one");
        TEST("li: ol gets numbering", textOf("o1") == "1. first" && textOf("o2") == "2. second");
    }
    {
        // v13 起 border 与 font-weight 已实现：不再告警，而是落到扩展绘制段
        auto res = CompileBinUIEx(
            "<html><body><div id=\"d1\" style=\"height:20px;border:1px solid red\">a</div>"
            "<div id=\"d2\" style=\"height:20px;border:2px solid blue\">b</div>"
            "<p id=\"pw\" style=\"font-weight:bold\">c</p></body></html>");
        int borderWarns = 0, weightWarns = 0;
        for (const auto& w : res.warnings) {
            if (w.find("border") != std::string::npos) borderWarns++;
            if (w.find("font-weight") != std::string::npos) weightWarns++;
        }
        TEST("css v13: border 不再告警", borderWarns == 0);
        TEST("css v13: font-weight 不再告警", weightWarns == 0);
        BinUIRenderer r;
        r.loadLayout(res.layout.data(), res.layout.size());
        const BinUIPaintNode* p1 = r.paintOf(r.findNodeByName("d1"));
        const BinUIPaintNode* p2 = r.paintOf(r.findNodeByName("d2"));
        TEST("css v13: 两个节点的边框宽度各自生效",
             p1 && p2 && p1->borderW[0] == 1 && p2->borderW[0] == 2 &&
             p1->borderColor[0] == 0xFFFF0000u && p2->borderColor[0] == 0xFF0000FFu);
        const BinUIPaintNode* pw = r.paintOf(r.findNodeByName("pw"));
        TEST("css v13: font-weight:bold -> 700",
             pw && (pw->flags & PAINT_WEIGHT) && pw->weight == 7);
    }

    // 21. <style> 普通 CSS 规则 + 级联（内联 > #id > .class > tag > 同权重后写胜）
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".btn { background: #ff0000; width: 120px; height: 40px; border-radius: 8px; }"
            "</style></head><body><button id=\"b\" class=\"btn\">Go</button></body></html>");
        BinUIRenderer r;
        TEST("css: class rule parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::NodeInfo* b = nullptr;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("b")) b = &nd;
        TEST("css: class rule applies (width/height/bg/radius)",
             b && std::fabs(b->width - 120.0f) < 0.5f && std::fabs(b->height - 40.0f) < 0.5f &&
                 b->color == 0xFFFF0000u && b->radius == 8);
    }
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "div   { background: #111111; }"
            ".card { background: #222222; }"
            "#top  { background: #333333; }"
            "</style></head><body>"
            "<div id=\"a\" class=\"card\" style=\"height:20px\">x</div>"
            "<div id=\"top\" class=\"card\" style=\"height:20px\">y</div>"
            "<div id=\"c\" class=\"card\" style=\"height:20px\">z</div>"
            "<div id=\"t\" style=\"height:20px\">w</div>"
            "<div id=\"inl\" class=\"card\" style=\"height:20px;background:#444444\">v</div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("cascade: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.color;
            return 0xDEADBEEFu;
        };
        TEST("cascade: #id beats .class", colorOf("top") == 0xFF333333u);
        TEST("cascade: .class beats tag", colorOf("a") == 0xFF222222u && colorOf("c") == 0xFF222222u);
        TEST("cascade: tag applies when nothing else", colorOf("t") == 0xFF111111u);
        TEST("cascade: inline beats everything", colorOf("inl") == 0xFF444444u);
    }
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".dup { background: #111111; }"
            ".dup { background: #222222; }"
            "h1, .grp { color: #0000ff; font-size: 20px; }"
            "p.note { display: none; }"
            "</style></head><body>"
            "<div id=\"d\" class=\"dup\" style=\"height:20px\"></div>"
            "<p id=\"g\" class=\"grp\" style=\"height:24px\">group</p>"
            "<p id=\"n\" class=\"note\">hidden</p>"
            "</body></html>");
        BinUIRenderer r;
        TEST("css2: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint32_t dupColor = 0xDEADBEEFu;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("d")) dupColor = nd.color;
        TEST("css2: same specificity -> later wins", dupColor == 0xFF222222u);
        const BinUIRenderer::NodeInfo* g = nullptr;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("g")) g = &nd;
        TEST("css2: selector group applies to .class", g && g->fontSize == 20);
        TEST("css2: tag.class with display:none removes node", r.findNodeByName("n") == 0);
    }
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "div .x { color: red; }"                       // v13 起支持：不该再告警
            ".y:visited { color: blue; }"
            "p::first-line { color: green; }"
            ".a.b { color: green; }"                       // v13 起支持：不该再告警
            "@media (max-width: 600px) { .z { color: green; } }"
            "</style></head><body><div style=\"height:20px\"><span class=\"a b x\">x</span></div>"
            "</body></html>");
        int descendant = 0, pseudo = 0, pelem = 0, compound = 0, media = 0;
        for (const auto& w : res.warnings) {
            if (w.find("div .x") != std::string::npos) descendant++;
            if (w.find(":visited") != std::string::npos) pseudo++;
            if (w.find("::first-line") != std::string::npos) pelem++;
            if (w.find(".a.b") != std::string::npos) compound++;
            if (w.find("@media") != std::string::npos) media++;
        }
        TEST("css warn: 后代组合子不再告警（v13）", descendant == 0);
        TEST("css warn: unsupported pseudo class", pseudo == 1);
        TEST("css warn: unsupported pseudo element", pelem == 1);
        TEST("css warn: 复合类不再告警（v13）", compound == 0);
        TEST("css warn: @media", media == 1);
    }
    {
        // 动画规则仍由动画解析器处理，不能被普通 CSS 解析吃掉
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "@keyframes pulse { 0% { opacity: 0.2; } 100% { opacity: 1; } }"
            ".pulse { animation: pulse 1s infinite; width: 60px; height: 60px; }"
            "</style></head><body><div id=\"p\" class=\"pulse\"></div></body></html>");
        BinUIRenderer r;
        TEST("css+anim: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::NodeInfo* p = nullptr;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("p")) p = &nd;
        TEST("css+anim: static props from class still apply", p && std::fabs(p->width - 60.0f) < 0.5f);
    }

    {
        // ---- v11 运行时伪类：:hover / :active / :focus / :checked ----
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".hov { background: #202020; width: 100px; height: 40px; }"
            ".hov:hover { background: #00ff00; }"
            ".hov:active { background: #ff0000; }"
            ".foc:focus { background: #0000ee; }"
            ".foc:checked { background: #ff00ff; }"
            ".big:hover { transform: scale(1.5); }"
            "</style></head><body>"
            "<div id=\"h\" class=\"hov\"></div>"
            "<button id=\"b\" class=\"foc\" style=\"width:60px;height:24px;background:#111111\">x</button>"
            "<div id=\"s\" class=\"big\" style=\"width:40px;height:20px\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("v11: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const uint16_t hId = r.findNodeByName("h");
        const auto* hs = r.stateStyle(hId, BINUI_ST_HOVER);
        TEST("v11: :hover style compiled",
             hs && (hs->fields & BINUI_STF_COLOR) && hs->color == 0xFF00FF00u);

        r.setHover(10, 10);
        TEST("v11: hover applies author color", r.drawColor(hId) == 0xFF00FF00u);
        r.press(10, 10);
        TEST("v11: :active outranks :hover", r.drawColor(hId) == 0xFFFF0000u);
        r.release(10, 10);
        TEST("v11: release falls back to hover", r.drawColor(hId) == 0xFF00FF00u);
        r.clearHover();
        TEST("v11: leave restores base color", r.drawColor(hId) == 0xFF202020u);

        const uint16_t bId = r.findNodeByName("b");
        r.setNodeChecked(bId, true);
        TEST("v11: :checked applies", r.drawColor(bId) == 0xFFFF00FFu);
        r.setKeyboardFocus(bId);
        TEST("v11: :focus outranks :checked", r.drawColor(bId) == 0xFF0000EEu);
        r.setKeyboardFocus(-1);
        r.setNodeChecked(bId, false);
        TEST("v11: states cleared", r.drawColor(bId) == 0xFF111111u);

        const uint16_t sId = r.findNodeByName("s");
        const auto* ss = r.stateStyle(sId, BINUI_ST_HOVER);
        TEST("v11: transform scale compiled",
             ss && (ss->fields & BINUI_STF_SCALE) && std::fabs(ss->scale - 1.5f) < 0.001f);
    }
    {
        // ---- 结构伪类：兄弟序号在编译期求值 ----
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "li:first-child { color: #ff0000; }"
            "li:last-child { color: #00ff00; }"
            "li:nth-child(even) { color: #0000ff; }"
            "p:first-of-type { font-size: 30px; }"
            "div:not(.skip) { background: #123456; }"
            "</style></head><body>"
            "<ul><li id=\"l1\">a</li><li id=\"l2\">b</li><li id=\"l3\">c</li></ul>"
            "<p id=\"p1\">x</p><p id=\"p2\">y</p>"
            "<div id=\"d1\"></div><div id=\"d2\" class=\"skip\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("struct: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            const uint16_t id = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == id) return nd.color;
            return 0xDEADBEEFu;
        };
        auto fontSizeOf = [&](const char* name) -> int {
            const uint16_t id = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == id) return nd.fontSize;
            return -1;
        };
        TEST("struct: :first-child", colorOf("l1") == 0xFFFF0000u);
        TEST("struct: :nth-child(even)", colorOf("l2") == 0xFF0000FFu);
        TEST("struct: :last-child", colorOf("l3") == 0xFF00FF00u);
        TEST("struct: :first-of-type", fontSizeOf("p1") == 30 && fontSizeOf("p2") != 30);
        TEST("struct: :not(.skip) matches", colorOf("d1") == 0xFF123456u);
        TEST("struct: :not(.skip) skips", colorOf("d2") != 0xFF123456u);
    }
    {
        // ---- 伪元素 ::before / ::after ----
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".bullet::before { content: \"\\25B8 \"; }"
            ".sep::after { content: \"\"; width: 120px; height: 2px; background: #333333; }"
            ".tag::after { content: attr(data-tag); }"
            ".nocon::after { color: #ff0000; }"
            "</style></head><body>"
            "<ul><li id=\"b\" class=\"bullet\">item</li></ul>"
            "<div id=\"s1\" class=\"sep\"></div>"
            "<span id=\"t\" class=\"tag\" data-tag=\"AB\">tag</span>"
            "<span id=\"nc\" class=\"nocon\">nc</span>"
            "</body></html>");
        BinUIRenderer r;
        TEST("pseudo: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));

        std::string bText;
        const uint16_t bId = r.findNodeByName("b");
        for (const auto& nd : r.getNodes()) if (nd.id == bId) bText = nd.text;
        // inline 模式：内容并入宿主文本，且抑制 <li> 自动项目符号
        TEST("pseudo: inline ::before prepends text",
             bText.size() > 3 && bText.rfind("\xE2\x96\xB8 ", 0) == 0);
        TEST("pseudo: no auto bullet when ::before present",
             bText.find("\xE2\x80\xA2") == std::string::npos);

        // box 模式：合成一个子节点（分隔条）
        const uint16_t s1Id = r.findNodeByName("s1");
        const BinUIRenderer::NodeInfo* sep = nullptr;
        for (const auto& nd : r.getNodes())
            if (nd.parentId == s1Id && nd.id != 0 && std::fabs(nd.height - 2.0f) < 0.01f) sep = &nd;
        TEST("pseudo: box ::after creates child node", sep != nullptr);
        TEST("pseudo: box node keeps width/color",
             sep && std::fabs(sep->width - 120.0f) < 0.5f && sep->color == 0xFF333333u);

        std::string tText;
        const uint16_t tId = r.findNodeByName("t");
        for (const auto& nd : r.getNodes()) if (nd.id == tId) tText = nd.text;
        TEST("pseudo: content attr() appends host attribute",
             tText.size() >= 5 && tText.compare(tText.size() - 2, 2, "AB") == 0);

        int noContentWarn = 0;
        for (const auto& w : res.warnings)
            if (w.find("缺少 content") != std::string::npos) noContentWarn++;
        TEST("pseudo: ::after without content warns", noContentWarn == 1);
    }

    {
        // ---- 组件内联：<use src="x.bum.html"> ----
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "binui_test_cmp";
        fs::create_directories(dir);
        auto writeFile = [&](const std::string& name, const std::string& text) {
            std::ofstream f(dir / name, std::ios::binary);
            f << text;
        };

        // 组件：内联参数 + 带默认值的参数
        writeFile("btn.bum.html",
            "<div style=\"height:40\">"
            "<button style=\"position:absolute; left:0; top:0; width:{{w}}; height:40;"
            " background:{{bg=#101010}}\">{{label}}</button></div>");
        // 组合组件：内部再引用 btn.bum.html
        writeFile("card.bum.html",
            "<div style=\"height:80\">"
            "<span style=\"height:20\">{{heading}}</span>"
            "<use src=\"btn.bum.html\" x=\"0\" y=\"24\" w=\"100\" label=\"卡片按钮\"/>"
            "</div>");
        writeFile("host.html",
            "<html><body>"
            "<use src=\"btn.bum.html\" x=\"10\" y=\"10\" w=\"120\" label=\"确定\" bg=\"#FF0000\"/>"
            "<use src=\"btn.bum.html\" x=\"10\" y=\"60\" w=\"120\" label=\"默认底色\"/>"
            "<use src=\"card.bum.html\" x=\"10\" y=\"110\" w=\"200\" heading=\"卡片标题\"/>"
            "<use x=\"10\" y=\"200\"/>"
            "<use src=\"missing.bum.html\" x=\"10\" y=\"240\"/>"
            "</body></html>");

        auto res = CompileBinUIFileEx((dir / "host.html").generic_string());
        BinUIRenderer r;
        TEST("component: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));

        auto findByText = [&](const std::string& text) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.text == text) return &n;
            return nullptr;
        };
        const auto* okBtn = findByText("确定");
        TEST("component: param substituted into text", okBtn != nullptr);
        TEST("component: param substituted into style",
             okBtn && std::fabs(okBtn->width - 120.0f) < 0.5f && okBtn->color == 0xFFFF0000u);
        const auto* dfltBtn = findByText("默认底色");
        TEST("component: default value used when param omitted",
             dfltBtn && dfltBtn->color == 0xFF101010u);
        const auto* cardBtn = findByText("卡片按钮");
        TEST("component: nested <use> expanded", cardBtn != nullptr);
        TEST("component: nested heading substituted", findByText("卡片标题") != nullptr);

        int noSrc = 0, missingFile = 0;
        for (const auto& w : res.warnings) {
            if (w.find("缺少 src") != std::string::npos) noSrc++;
            if (w.find("读不到组件文件") != std::string::npos) missingFile++;
        }
        TEST("component: missing src warns", noSrc == 1);
        TEST("component: missing file warns", missingFile == 1);

        // ---- 多根组件：顶层元素 > 1 → 用合成的 <div> 承载（与二进制组件的链接器口径一致）----
        // 修复前：第二个顶层元素的 parentIdx 是 SIZE_MAX，被当成索引查 cremap → 越界
        // （debug 断言 abort，release 挂到垃圾父节点或整个丢掉）。
        writeFile("multi.bum.html",
            "<div style=\"width:80; height:20\"><span style=\"height:20\">多根甲</span></div>"
            "<div style=\"width:80; height:20\"><span style=\"height:20\">多根乙</span></div>");
        writeFile("host2.html",
            "<html><body>"
            "<use src=\"multi.bum.html\" x=\"10\" y=\"10\" w=\"200\"/>"
            "</body></html>");
        auto res3 = CompileBinUIFileEx((dir / "host2.html").generic_string());
        BinUIRenderer r3;
        TEST("component: multi-root parses",
             !res3.layout.empty() && r3.loadLayout(res3.layout.data(), res3.layout.size()));
        // html / 合成容器 / 甲 + span / 乙 + span
        TEST("component: multi-root keeps all top-level elements", r3.getNodes().size() == 6);
        {
            const auto& nodes3 = r3.getNodes();
            const bool haveBoth = nodes3.size() == 6 &&
                                  nodes3[2].parentId == nodes3[1].id &&
                                  nodes3[4].parentId == nodes3[1].id &&
                                  nodes3[1].parentId == nodes3[0].id;
            TEST("component: multi-root 两个顶层元素都挂在合成容器下", haveBoth);
        }
        {
            int multiWarn = 0;
            for (const auto& w : res3.warnings)
                if (w.find("顶层元素") != std::string::npos) multiWarn++;
            TEST("component: multi-root 给出提示", multiWarn == 1);
        }

        // ---- v12：二进制组件 .bum（编译期由链接器离线合并）----
        writeFile("pack.bum.html",
            "<div style=\"height:30\"><span style=\"height:20; color:#00FF00\">打包组件</span></div>");
        const auto packRes = CompileBinUIFileEx((dir / "pack.bum.html").generic_string());
        TEST("component: binary build produces layout", !packRes.layout.empty());
        {
            std::ofstream f(dir / "pack.bum", std::ios::binary);
            f.write(reinterpret_cast<const char*>(packRes.layout.data()),
                    static_cast<std::streamsize>(packRes.layout.size()));
        }
        writeFile("binhost.html",
            "<html><body>"
            "<use src=\"pack.bum\" x=\"10\" y=\"10\" w=\"140\"/>"
            "<use src=\"pack.bum\" external x=\"10\" y=\"60\" w=\"140\"/>"
            "</body></html>");
        const auto bres = CompileBinUIFileEx((dir / "binhost.html").generic_string());
        BinUIRenderer rb;
        TEST("component: binary host parses",
             !bres.layout.empty() && rb.loadLayout(bres.layout.data(), bres.layout.size()));
        int packed = 0;
        const BinUIRenderer::NodeInfo* packedSpan = nullptr;
        for (const auto& n : rb.getNodes())
            if (n.text == "打包组件") { packed++; packedSpan = &n; }
        TEST("component: binary .bum inlined at compile time", packed == 1);
        const BinUIRenderer::NodeInfo* packedWrap = nullptr;
        if (packedSpan)
            for (const auto& n : rb.getNodes())
                if (n.id == packedSpan->parentId) packedWrap = &n;
        TEST("component: <use w> 覆盖二进制组件根宽",
             packedWrap && std::fabs(packedWrap->width - 140.0f) < 0.5f);
        TEST("component: binary 组件根坐标落在锚点处",
             packedWrap && std::fabs(packedWrap->y - 10.0f) < 0.5f);

        // ---- v12：外挂组件（external）-> 组件引用段 + 运行期挂载 ----
        TEST("component: external <use> 记录一个挂载点", rb.componentRefs().size() == 1);
        TEST("component: 挂载点路径已相对宿主改写",
             !rb.componentRefs().empty() && rb.componentRefs()[0].src.find("pack.bum") != std::string::npos);
        if (!rb.componentRefs().empty()) {
            const uint16_t anchor = rb.componentRefs()[0].anchorId;
            std::vector<uint8_t> packBytes = packRes.layout;
            TEST("component: 运行期挂载成功",
                 rb.mountComponent(packBytes.data(), packBytes.size(), anchor));
            int after = 0;
            for (const auto& n : rb.getNodes())
                if (n.text == "打包组件") after++;
            TEST("component: 挂载后节点数增加", after == packed + 1);
            TEST("component: 已挂载标记幂等", rb.isComponentMounted(anchor));
        }

        // ---- 二进制与源码并存：二进制优先；源码更新则改用源码并告警 ----
        {
            std::ofstream f(dir / "pack.bum.html", std::ios::binary);
            f << "<div style=\"height:30\"><span style=\"height:20\">源码更新版</span></div>";
        }
        std::filesystem::last_write_time(
            dir / "pack.bum.html",
            std::filesystem::last_write_time(dir / "pack.bum") + std::chrono::seconds(5));
        const auto nres = CompileBinUIFileEx((dir / "binhost.html").generic_string());
        bool staleWarn = false;
        for (const auto& w : nres.warnings)
            if (w.find("源码比二进制新") != std::string::npos) staleWarn = true;
        TEST("component: 源码比二进制新时告警", staleWarn);
        BinUIRenderer rn;
        rn.loadLayout(nres.layout.data(), nres.layout.size());
        bool hasNewText = false;
        for (const auto& n : rn.getNodes())
            if (n.text == "源码更新版") hasNewText = true;
        TEST("component: 源码更新版生效（改用源码内联）", hasNewText);

        // ---- v12：资源段（布局自描述：imageIndex -> 图片路径）----
        writeFile("img.html",
            "<html><body><img src=\"noela.png\" style=\"width:40; height:40\"/></body></html>");
        const auto ires = CompileBinUIFileEx((dir / "img.html").generic_string());
        BinUIRenderer ri;
        ri.loadLayout(ires.layout.data(), ires.layout.size());
        TEST("v12: 资源段随布局写出",
             !ires.images.empty() && ri.imagePaths().size() == ires.images.size() &&
             ri.imagePaths()[0] == ires.images[0]);

        // 参数值按纯文本注入：传进来的标签不能变成真节点
        writeFile("inject.bum.html", "<div style=\"height:20\"><span>{{body}}</span></div>");
        writeFile("inject.html",
            "<html><body><use src=\"inject.bum.html\" x=\"0\" y=\"0\" w=\"300\""
            " body=\"a &lt;use src=&quot;btn.bum.html&quot;&gt; b\"/></body></html>");
        auto res2 = CompileBinUIFileEx((dir / "inject.html").generic_string());
        BinUIRenderer r2;
        TEST("component: inject parses", !res2.layout.empty() && r2.loadLayout(res2.layout.data(), res2.layout.size()));
        int textWithMarkup = 0;
        for (const auto& n : r2.getNodes())
            if (n.text.find("<use") != std::string::npos) textWithMarkup = 1;
        TEST("component: param value injected as text, not markup", textWithMarkup == 1);
        TEST("component: injection adds no extra nodes", r2.getNodes().size() == 3);

        // ---- 打包清单（.bui.srcmap）：内嵌的包也能还原打包前的目录结构 ----
        // 存的是"原始字节 + 相对共同祖先的路径"，所以解包是逐字节还原，不是节点反推。
        {
            binui::CompileOptions opt;
            opt.emitSrcMap = true;
            const auto mres = CompileBinUIFileEx((dir / "host.html").generic_string(), opt);
            TEST("srcmap: 开关打开就产出", !mres.srcMap.empty());

            std::vector<binui::SrcMapEntry> ents;
            TEST("srcmap: 可解析", binui::ParseSrcMap(mres.srcMap.data(), mres.srcMap.size(), ents));
            TEST("srcmap: 第 0 条是主 UI",
                 !ents.empty() && ents[0].path.find("host.html") != std::string::npos);

            // 清单里的主 UI 内容必须和磁盘上的原文件逐字节一致
            std::string orig;
            {
                std::ifstream f(dir / "host.html", std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                orig = ss.str();
            }
            const bool same = !ents.empty() &&
                              std::string(ents[0].data.begin(), ents[0].data.end()) == orig;
            TEST("srcmap: 主 UI 与原文件逐字节一致", same);

            // 内联的源码组件与嵌套组件都要在清单里（同一个组件被 use 两次只记一份）
            int btnEntries = 0, cardEntries = 0, missingEntries = 0;
            for (const auto& e : ents) {
                if (e.path.find("btn.bum.html") != std::string::npos) btnEntries++;
                if (e.path.find("card.bum.html") != std::string::npos) cardEntries++;
                if (e.path.find("missing") != std::string::npos) missingEntries++;
            }
            TEST("srcmap: 内联组件在清单里（多实例只记一份）", btnEntries == 1);
            TEST("srcmap: 嵌套组件在清单里", cardEntries == 1);
            TEST("srcmap: 读不到的组件不进清单", missingEntries == 0);

            // 路径必须是相对的（不能把编译机的绝对路径写进包里）
            bool allRelative = true;
            for (const auto& e : ents)
                if (e.path.empty() || e.path[0] == '/' || (e.path.size() > 1 && e.path[1] == ':'))
                    allRelative = false;
            TEST("srcmap: 路径都是相对的", allRelative);

            // 默认（不开开关）一个字节都不产出
            const auto plain = CompileBinUIFileEx((dir / "host.html").generic_string());
            TEST("srcmap: 默认不产出", plain.srcMap.empty());
        }
    }

    // ---- v13：扩展绘制段（文字色/边框/渐变/字重/行高/装饰）+ 底色与文字色分家 ----
    {
        const char* html =
            "<html><body style=\"background:#FFFFFF; color:#333333\">\n"
            "  <div class=\"card\" id=\"card\">\n"
            "    <span class=\"title\" id=\"title\">标题</span>\n"
            "    <span id=\"plain\">继承父色</span>\n"
            "  </div>\n"
            "  <a class=\"link\" id=\"link\">链接</a>\n"
            "  <button class=\"btn\" id=\"btn\">确定</button>\n"
            "  <span class=\"badge\" id=\"badge\">徽标</span>\n"
            "  <div class=\"grad\" id=\"grad\"></div>\n"
            "  <div class=\"clip\" id=\"clip\"></div>\n"
            "  <div class=\"scroll\" id=\"scroll\"></div>\n"
            "</body></html>\n"
            "<style>\n"
            "  .card { padding:8px; background:#FFFFFF; border:1px solid #DDDDDD;"
            "          border-bottom:2px solid #FF0000; border-radius:8px; }\n"
            "  .title { font-size:18px; font-weight:700; line-height:160%; color:#111111; }\n"
            "  .link { font-size:14px; text-decoration:underline; color:#0055FF; }\n"
            "  .btn  { width:100px; height:36px; background:#8B5CF6; color:#101010; }\n"
            "  .badge { background:#FFEEDD; color:#884400; border-radius:4px; }\n"
            "  .grad { width:200px; height:60px; background:linear-gradient(to right, #202020, #404040); }\n"
            "  .clip { width:200px; height:40px; overflow:hidden; }\n"
            "  .scroll { width:200px; height:40px; overflow:auto; }\n"
            "</style>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("v13: 布局可解析", ParseInto(r, res.layout));
        TEST("v13: 版本号 >= 13", BINUI_VERSION >= 13);

        auto byId = [&](const char* name) -> uint16_t { return r.findNodeByName(name); };
        auto nodeOf = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.id == id) return &n;
            return nullptr;
        };

        // 容器默认透明（v13 起不再默认 #222222），底色只来自作者声明
        const uint16_t cardId = byId("card");
        const auto* card = nodeOf(cardId);
        TEST("v13: 卡片找到", card != nullptr);
        if (card) {
            TEST("v13: 卡片底色 = 声明的白色", card->color == 0xFFFFFFFFu);
            const BinUIPaintNode* p = r.paintOf(cardId);
            TEST("v13: 卡片有边框绘制段", p != nullptr && (p->flags & PAINT_BORDER));
            if (p && (p->flags & PAINT_BORDER)) {
                TEST("v13: 边框四边宽 1/1/2/1",
                     p->borderW[0] == 1 && p->borderW[2] == 2 &&
                     p->borderW[1] == 1 && p->borderW[3] == 1);
                TEST("v13: 边框颜色按边生效（上灰下红）",
                     p->borderColor[0] == 0xFFDDDDDDu && p->borderColor[2] == 0xFFFF0000u);
            }
        }

        // 文字色与底色分家：LABEL 有底色时文字色走 textColor，不再吃掉背景
        const uint16_t titleId = byId("title");
        const auto* title = nodeOf(titleId);
        const BinUIPaintNode* tp = r.paintOf(titleId);
        // LABEL 没有底色时沿用旧约定（node.color 存文字色），textColorOf 两条路径都通
        TEST("v13: 标题文字色取到 #111111", r.textColorOf(titleId) == 0xFF111111u);
        // 有底色的 LABEL：底色与文字色必须分家
        const uint16_t badgeId = byId("badge");
        const auto* badge = nodeOf(badgeId);
        const BinUIPaintNode* bd = r.paintOf(badgeId);
        TEST("v13: 有底色的 LABEL 底色正确", badge && badge->color == 0xFFFFEEDDu);
        TEST("v13: 有底色的 LABEL 文字色走 textColor",
             bd && (bd->flags & PAINT_TEXT_COLOR) && bd->textColor == 0xFF884400u);
        (void)title;
        TEST("v13: 字重 700", tp && (tp->flags & PAINT_WEIGHT) && tp->weight == 7);
        TEST("v13: 行高 160%", tp && (tp->flags & PAINT_LINE_HEIGHT) && tp->lineHeight == 160);
        TEST("v13: 字号 18", title && title->fontSize == 18);

        // CSS color 继承：没写颜色的子元素取祖先的 color
        TEST("v13: 文字色可继承", r.textColorOf(byId("plain")) == 0xFF333333u);

        // 下划线
        const BinUIPaintNode* lp = r.paintOf(byId("link"));
        TEST("v13: text-decoration:underline", lp && (lp->flags & PAINT_UNDERLINE) &&
             lp->textColor == 0xFF0055FFu);

        // 按钮文字色（以前恒白，无法指定）
        const BinUIPaintNode* bp = r.paintOf(byId("btn"));
        TEST("v13: 按钮文字色可指定", bp && (bp->flags & PAINT_TEXT_COLOR) &&
             bp->textColor == 0xFF101010u);

        // 渐变
        const BinUIPaintNode* gp = r.paintOf(byId("grad"));
        TEST("v13: 渐变段写出", gp && (gp->flags & PAINT_GRADIENT) &&
             gp->gradFrom == 0xFF202020u && gp->gradTo == 0xFF404040u &&
             gp->gradDir == BINUI_GRAD_H);
        {
            bool colorWarn = false;
            for (const auto& w : res.warnings)
                if (w.find("无法识别的颜色") != std::string::npos) colorWarn = true;
            TEST("v13: 渐变不被当成颜色解析（无假警告）", !colorWarn);
        }

        // overflow：hidden = 只裁剪（不可滚），auto = 可滚
        const uint16_t clipId = byId("clip"), scrollId = byId("scroll");
        TEST("v13: overflow:hidden 仍是裁剪容器", r.isScrollable(clipId));
        TEST("v13: overflow:hidden 不可滚动", !r.isUserScrollable(clipId));
        TEST("v13: overflow:auto 可滚动", r.isUserScrollable(scrollId));
        TEST("v13: overflow:hidden 滚轮无效",
             r.scrollAt(20.0f, nodeOf(clipId) ? nodeOf(clipId)->y + 5.0f : 0.0f, -30.0f) < 0);
    }

    // ---- v13：选择器组合子 / 复合类 / 属性选择器 ----
    {
        const char* html =
            "<html><body>\n"
            "  <div class=\"card\">\n"
            "    <span class=\"title\">A</span>\n"
            "    <span class=\"body\">B</span>\n"
            "  </div>\n"
            "  <div class=\"box\"><span class=\"deep\">C</span></div>\n"
            "  <button class=\"btn primary\">D</button>\n"
            "  <button class=\"btn\">E</button>\n"
            "  <input type=\"text\" value=\"F\">\n"
            "  <input type=\"button\" value=\"G\">\n"
            "  <ul class=\"adj\"><li>H</li><li>I</li></ul>\n"
            "  <ul class=\"gen\"><li>J</li><li>K</li><li>L</li></ul>\n"
            "</body></html>\n"
            "<style>\n"
            "  .card > .title   { color:#AA0000; }\n"          // 子代
            "  .card .body     { color:#00AA00; }\n"           // 后代
            "  .box .deep      { color:#0000AA; }\n"           // 后代（跨层）
            "  .btn.primary    { color:#00AAAA; }\n"           // 复合类
            "  input[type=text]{ color:#AA00AA; }\n"           // 属性选择器
            "  .adj li + li    { color:#AAAA00; }\n"           // 相邻兄弟
            "  .gen li ~ li    { color:#00AA00; }\n"           // 后续兄弟
            "  .body:not(.x)  { background:#111111; }\n"       // :not
            "</style>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("sel: 布局可解析", ParseInto(r, res.layout));

        auto textColor = [&](const std::string& text) -> uint32_t {
            for (const auto& n : r.getNodes()) {
                // <li> 会自动加 "• " / "1. " 前缀，这里按后缀匹配
                if (n.text.size() < text.size()) continue;
                if (n.text.compare(n.text.size() - text.size(), text.size(), text) != 0) continue;
                if (const BinUIPaintNode* p = r.paintOf(n.id))
                    if (p->flags & PAINT_TEXT_COLOR) return p->textColor;
                return n.color;   // LABEL 旧约定
            }
            return 0;
        };
        TEST("sel: 子代组合子 .card > .title", textColor("A") == 0xFFAA0000u);
        TEST("sel: 后代组合子 .card .body", textColor("B") == 0xFF00AA00u);
        TEST("sel: 后代组合子跨层 .box .deep", textColor("C") == 0xFF0000AAu);
        TEST("sel: 复合类 .btn.primary", textColor("D") == 0xFF00AAAAu);
        TEST("sel: 复合类未命中时回落", textColor("E") != 0xFF00AAAAu);
        TEST("sel: 属性选择器 input[type=text]", textColor("F") == 0xFFAA00AAu);
        TEST("sel: 属性选择器不误伤 input[type=button]", textColor("G") != 0xFFAA00AAu);
        TEST("sel: 相邻兄弟 li + li", textColor("I") == 0xFFAAAA00u);
        TEST("sel: 相邻兄弟不命中首项", textColor("H") != 0xFFAAAA00u);
        TEST("sel: 后续兄弟 li ~ li", textColor("K") == 0xFF00AA00u);
        TEST("sel: 后续兄弟同样命中第三项", textColor("L") == 0xFF00AA00u);
        TEST("sel: 后续兄弟不命中首项", textColor("J") != 0xFF00AA00u);
        TEST("sel: :not() 内层复合类", textColor("B") == 0xFF00AA00u);
    }
    // ---- v14：UA 默认样式 / 行内语义 / 表单属性 ----
    {
        const char* html =
            "<app_window width=\"800\" height=\"600\" bg=\"#FFFFFF\"><html><body>\n"
            "  <b id=\"b1\">粗体</b>\n"
            "  <i id=\"i1\">斜体</i>\n"
            "  <u id=\"u1\">下划线</u>\n"
            "  <s id=\"s1\">删除</s>\n"
            "  <mark id=\"m1\">高亮</mark>\n"
            "  <h2 id=\"h2\">标题</h2>\n"
            "  <hr id=\"hr1\">\n"
            "  <pre id=\"pre1\">a\nb\nc</pre>\n"
            "  <input id=\"ph\" type=\"text\" placeholder=\"请输入\">\n"
            "  <input id=\"val\" type=\"text\" value=\"真实值\">\n"
            "  <input id=\"cb\" type=\"checkbox\" checked>\n"
            "  <input id=\"hid\" type=\"hidden\" value=\"x\">\n"
            "  <button id=\"btn\" disabled>禁用</button>\n"
            "  <input id=\"ro\" type=\"text\" readonly value=\"只读\">\n"
            "  <a id=\"lnk\" href=\"https://example.com\">链接</a>\n"
            "  <img id=\"im\" alt=\"替代文字\">\n"
            "</body></html></app_window>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("v14: 布局可解析", ParseInto(r, res.layout));
        TEST("v14: 版本号 >= 14", BINUI_VERSION >= 14);

        auto byId = [&](const char* name) -> uint16_t { return r.findNodeByName(name); };
        auto nodeOf = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.id == id) return &n;
            return nullptr;
        };
        auto paintOf = [&](const char* name) -> const BinUIPaintNode* {
            return r.paintOf(byId(name));
        };

        // <b> 加粗（以前只是普通文字）
        const BinUIPaintNode* pb = paintOf("b1");
        TEST("v14: <b> 走 700 字重", pb && (pb->flags & PAINT_WEIGHT) && pb->weight == 7);
        // <i> 斜体
        const BinUIPaintNode* pi = paintOf("i1");
        TEST("v14: <i> 斜体标记", pi && (pi->textFlags & TEXT_ITALIC));
        // <h2> 默认加粗
        const BinUIPaintNode* ph2 = paintOf("h2");
        TEST("v14: <h2> 默认加粗", ph2 && (ph2->flags & PAINT_WEIGHT) && ph2->weight == 7);
        // <u>/<s> 是行内 LABEL（以前掉到 LIST 变块级），带装饰线
        const auto* u1 = nodeOf(byId("u1"));
        const auto* s1 = nodeOf(byId("s1"));
        TEST("v14: <u> 是 LABEL 不是块级容器", u1 && u1->type == BINUI_LABEL);
        TEST("v14: <u> 带下划线", paintOf("u1") && (paintOf("u1")->flags & PAINT_UNDERLINE));
        TEST("v14: <s> 是 LABEL", s1 && s1->type == BINUI_LABEL);
        TEST("v14: <s> 带删除线", paintOf("s1") && (paintOf("s1")->flags & PAINT_STRIKE));
        // <mark> 黄底深字
        const auto* m1 = nodeOf(byId("m1"));
        TEST("v14: <mark> 有黄底", m1 && ((m1->color >> 24) & 0xFF) && m1->color != 0xFFFFFFFFu);
        TEST("v14: <mark> 深色文字", paintOf("m1") &&
             (paintOf("m1")->flags & PAINT_TEXT_COLOR) && paintOf("m1")->textColor == 0xFF1A1A1Au);
        // <hr> 有高度和底色（v13 容器透明化后曾经完全不可见）
        const auto* hr = nodeOf(byId("hr1"));
        TEST("v14: <hr> 有可见高度", hr && hr->height >= 1.0f && hr->height <= 4.0f);
        TEST("v14: <hr> 有不透明底色", hr && ((hr->color >> 24) & 0xFF) == 0xFF);
        // <pre> 保留换行
        const auto* pre = nodeOf(byId("pre1"));
        TEST("v14: <pre> 保留原始换行", pre && pre->text.find('\n') != std::string::npos);

        // placeholder 不再是真实值
        const uint16_t phId = byId("ph");
        const auto* ph = nodeOf(phId);
        TEST("v14: placeholder 文本仍可显示", ph && ph->text == "请输入");
        TEST("v14: placeholder 有占位标记", r.isPlaceholder(phId));
        TEST("v14: placeholder 的真实值为空", r.inputValue(phId).empty());
        // value 相反
        const uint16_t valId = byId("val");
        TEST("v14: value 是真实值", !r.isPlaceholder(valId) && r.inputValue(valId) == "真实值");

        // checkbox：18x18 方块 + 初始勾选播种
        const uint16_t cbId = byId("cb");
        const auto* cb = nodeOf(cbId);
        TEST("v14: checkbox 是小方块", cb && cb->width <= 20.0f && cb->height <= 20.0f);
        TEST("v14: checkbox checked 播种进渲染器", r.isNodeChecked(cbId));
        // hidden 不生成节点
        TEST("v14: input type=hidden 不生成节点", byId("hid") == 0);
        // disabled / readonly
        TEST("v14: button disabled", r.isDisabled(byId("btn")));
        TEST("v14: input readonly", r.isReadonly(byId("ro")));
        TEST("v14: readonly 不是 disabled", !r.isDisabled(byId("ro")));
        // <a href> 可点
        const uint16_t lnkId = byId("lnk");
        const auto* lnk = nodeOf(lnkId);
        TEST("v14: <a href> 可命中", lnk &&
             r.hitTest(lnk->x + lnk->width * 0.5f, lnk->y + lnk->height * 0.5f) == (int)lnkId);
        // img alt 兜底
        const auto* im = nodeOf(byId("im"));
        TEST("v14: img alt 写进文本（资源缺失时显示）", im && im->text == "替代文字");

        // 默认文字色随根底色自适应：白底 → 深字
        const auto* bold1 = nodeOf(byId("b1"));
        TEST("v14: 白底默认深色文字", bold1 && bold1->color == 0xFF1A1A1Au);

        // 深底 → 白字（现有暗色主题观感不变）
        auto darkRes = CompileBinUIEx(
            "<app_window width=\"200\" height=\"100\" bg=\"#0E0E13\"><html><body>"
            "<span id=\"t\">x</span></body></html></app_window>");
        BinUIRenderer rd;
        TEST("v14: 深底布局可解析", ParseInto(rd, darkRes.layout));
        const uint16_t tid = rd.findNodeByName("t");
        const BinUIRenderer::NodeInfo* tn = nullptr;
        for (const auto& n : rd.getNodes()) if (n.id == tid) tn = &n;
        TEST("v14: 深底默认白色文字", tn && tn->color == 0xFFFFFFFFu);
    }

    // ---- 组件源码的 HTML 写法（.bum.html）：和 .bum.html 等价 ----
    {
        const char* html =
            "<app_window width=\"420\" height=\"200\" bg=\"#F4F7FC\"><html><body>\n"
            "  <use id=\"t1\" src=\"components/tag.bum.html\" x=\"16\" y=\"20\" w=\"90\"\n"
            "       text=\"HTML 写法\" kind=\"mint\"/>\n"
            "  <use id=\"t2\" src=\"components/tag.bum\" x=\"16\" y=\"60\" w=\"90\" text=\"裸名字\"/>\n"
            "</body></html></app_window>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("bum.html: 组件可编译加载", ParseInto(r, res.layout));

        auto nodeOf = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.id == id) return &n;
            return nullptr;
        };
        const auto* t1 = nodeOf(r.findNodeByName("t1"));
        const auto* t2 = nodeOf(r.findNodeByName("t2"));
        // 根是 <span> → LABEL；参数替换 + 类名换肤都要生效
        TEST("bum.html: 显式扩展名内联出组件根（span → LABEL）", t1 && t1->type == BINUI_LABEL);
        TEST("bum.html: 参数 text 替换", t1 && t1->text == "HTML 写法");
        TEST("bum.html: kind=mint 换肤", t1 && t1->color == 0xFFE3F7F1u);
        // src="tag.bum" 的裸名字要能落到 tag.bum.html 上（同一个候选表）
        TEST("bum.html: 裸名字 src 也命中 .bum.html", t2 && t2->text == "裸名字");
    }

    // ---- v15：标准组件库 components/（根标签搬运 / 参数替换 / 插槽）----
    {
        const char* html =
            "<app_window width=\"760\" height=\"420\" bg=\"#F4F7FC\"><html><body>\n"
            "  <use src=\"components/titlebar.bum.html\" x=\"0\" y=\"0\" w=\"760\"\n"
            "       title=\"标题A\" sub=\"提示B\" idmin=\"imin\" idmax=\"imax\" idclose=\"iclose\"/>\n"
            "  <use id=\"btn\" src=\"components/button.bum.html\" x=\"16\" y=\"60\"\n"
            "       w=\"120\" h=\"36\" kind=\"secondary\" label=\"次按钮\"/>\n"
            "  <use id=\"fld\" src=\"components/field.bum.html\" x=\"16\" y=\"110\"\n"
            "       w=\"240\" ph=\"请输入关键字\"/>\n"
            "  <use id=\"bdg\" src=\"components/badge.bum.html\" x=\"300\" y=\"118\"\n"
            "       w=\"60\" text=\"在线\" kind=\"mint\"/>\n"
            "  <use src=\"components/panel.bum.html\" x=\"16\" y=\"170\" w=\"320\" title=\"面板标题\">\n"
            "    <span id=\"inner\" style=\"height:20; color:#66768C; font-size:12\">面板内容</span>\n"
            "  </use>\n"
            "</body></html></app_window>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("std: 标准组件库可编译加载", ParseInto(r, res.layout));

        auto nodeOf = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.id == id) return &n;
            return nullptr;
        };
        auto byText = [&](const char* t) -> uint16_t {
            for (const auto& n : r.getNodes()) if (n.text == t) return n.id;
            return 0xFFFF;
        };

        // 1) titlebar：根标签是 <titlebar>（拖动窗口靠它），参数与按钮 id 都生效
        const uint16_t titleId = byText("标题A");
        const auto* titleNode = nodeOf(titleId);
        TEST("std: titlebar 标题取到参数", titleNode != nullptr);
        TEST("std: titlebar 副标题取到参数", byText("提示B") != 0xFFFF);
        if (titleNode) {
            const auto* tb = nodeOf(titleNode->parentId);
            TEST("std: titlebar 根带 BINUI_TITLEBAR（窗口拖动）",
                 tb != nullptr && (tb->flags & BINUI_TITLEBAR));
        }
        TEST("std: titlebar 三大按钮 id 可定制",
             r.findNodeByName("imin") != 0 && r.findNodeByName("imax") != 0 &&
             r.findNodeByName("iclose") != 0);
        {
            const uint16_t nMin = r.findNodeByName("imin"), nMax = r.findNodeByName("imax");
            const uint16_t nClose = r.findNodeByName("iclose");
            const auto* bMin = nodeOf(nMin); const auto* bMax = nodeOf(nMax);
            const auto* bClose = nodeOf(nClose);
            // 三颗控制按钮：同尺寸，图标是元素画的（按钮自己不再有字形文案）
            TEST("std: titlebar 三颗控制按钮同宽高",
                 bMin && bMax && bClose && bMin->width == bMax->width &&
                 bMax->width == bClose->width && bMin->height == bMax->height &&
                 bMax->height == bClose->height);
            TEST("std: titlebar 按钮不再拿字形当图标（文案为空）",
                 bMin && bMax && bClose && bMin->text.empty() && bMax->text.empty() &&
                 bClose->text.empty());
            // 图标是按钮的绝对定位子节点：不参与命中测试，点在图标上照样落到按钮
            TEST("std: titlebar 图标节点存在且不可交互",
                 bMin && [&] {
                     int iconNodes = 0;
                     int interactiveIcons = 0;
                     for (const auto& n : r.getNodes()) {
                         if (n.parentId != nMin && n.parentId != nMax && n.parentId != nClose) continue;
                         iconNodes++;
                         if (n.flags & BINUI_INTERACTIVE) interactiveIcons++;
                     }
                     return iconNodes >= 3 && interactiveIcons == 0;
                 }());
            TEST("std: titlebar 点按钮中心命中按钮自己",
                 bMin && bMax && bClose &&
                 r.hitTest(bMin->x + bMin->width / 2, bMin->y + bMin->height / 2) == (int)nMin &&
                 r.hitTest(bMax->x + bMax->width / 2, bMax->y + bMax->height / 2) == (int)nMax &&
                 r.hitTest(bClose->x + bClose->width / 2, bClose->y + bClose->height / 2) == (int)nClose);
        }

        // 2) button：根标签搬过来 → 真的是 BINUI_BUTTON（可点击、居中、粗体）
        const uint16_t btnId = r.findNodeByName("btn");
        const auto* btn = nodeOf(btnId);
        TEST("std: button 根是 BINUI_BUTTON", btn && btn->type == BINUI_BUTTON);
        TEST("std: button 可交互（可点击）", btn && (btn->flags & BINUI_INTERACTIVE));
        TEST("std: button kind=secondary 生效", btn && btn->color == 0xFFEEF3FAu);
        TEST("std: button 文案取到参数", btn && btn->text == "次按钮");

        // 3) field：根是 <input> → 保住占位符语义
        const uint16_t fldId = r.findNodeByName("fld");
        const auto* fld = nodeOf(fldId);
        TEST("std: field 根是 BINUI_INPUT", fld && fld->type == BINUI_INPUT);
        TEST("std: field 占位符是占位符而不是真实值", r.isPlaceholder(fldId));
        TEST("std: field 占位符文案取到参数", fld && fld->text == "请输入关键字");

        // 4) badge：根是 <span> → LABEL（居中、无 LIST 的 12px 内缩）
        const uint16_t bdgId = r.findNodeByName("bdg");
        const auto* bdg = nodeOf(bdgId);
        TEST("std: badge 根是 BINUI_LABEL", bdg && bdg->type == BINUI_LABEL);
        TEST("std: badge kind=mint 生效", bdg && bdg->color == 0xFFE3F7F1u);

        // 5) panel：宿主子元素落进组件根容器（无插槽语法下的"内容"）
        const uint16_t innerId = r.findNodeByName("inner");
        const auto* inner = nodeOf(innerId);
        const uint16_t panelTitleId = byText("面板标题");
        const auto* panelTitle = nodeOf(panelTitleId);
        TEST("std: panel 标题取到参数", panelTitle != nullptr);
        TEST("std: panel 里的宿主子元素挂在同一父节点",
             inner && panelTitle && inner->parentId == panelTitle->parentId);
    }

    TestIssueRegressions();

    printf("\n=== Results: %d tests failed ===\n", failed);
    return failed > 0 ? 1 : 0;
}

}  // namespace suite_compiler


// ===========================================================================
// 汇总入口：任一子套件有失败则整体失败
// ===========================================================================
int main() {
    int fails = 0;
    fails += suite_compiler::run();
    std::printf("\n=== test_css: %s ===\n", fails ? "FAILED" : "全部通过");
    return fails ? 1 : 0;
}
