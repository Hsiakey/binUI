> **状态：已实施（0.25.0）**，且文中讨论的三个后端已在 0.37.x **整份删除**
> （见 [`backend_layering.md`](backend_layering.md) 第 3c 步）。
> 本文件保留为决策记录，里面的行号/行数与 `binui_gl.cpp` / `binui_vk.cpp` /
> `binui_d3d11.cpp` 的文件引用都已过期（那些文件不存在了）；
> 实际改动、实测数字与验收见 [CHANGELOG 0.25.0](../CHANGELOG.md) 与
> [migrate-to-bin2d.md](migrate-to-bin2d.md) 的 P3 段。

# P3b：圆角 / 描边的"九宫格计划"收口到 bin2d

> **结论先行**：三个 GPU 后端里，`fillRounded` / `strokeRounded` / `fillRoundedXf` 一共
> **393 行**是同一件事的三份写法（GL 123 / VK 90 / D3D11 141），其中 D3D11 的 **66 行**
> 是把共用数学（`BuildRoundedNineSlice`）又手抄了一遍。提案把它换成一个 bin2d 的
> **按层出 9 片**的几何函数（`BuildRoundedLayer` + `RoundedLayerCount`），三端净减
> **约 150–190 行**，像素 **0 变化**——**唯一例外**是 VK 的 `strokeRounded` 漏了多层
> 循环（现在**就是**三端分岔，只是门禁没覆盖），修它必须单独一步 + 新增一个用例。
>
> 本文只做调研与计划，**没有改任何源码**。所有行号以 2026-11-19 的 `D:\projects\binUI`
> 工作区为准。

---

## 0. 先看数字

| 项 | 值 | 出处 |
|---|---|---|
| 三端圆角相关代码 | **393 行** | GL 26+32+65、VK 20+27+43、D3D11 35+47+59 |
| 其中"又抄一遍共用数学" | **66 行**（D3D11 `fillRounded` 21 + `fillRoundedXf` 26 + `strokeRounded` 19） | `binui_d3d11.cpp:1913-1933, 1962-1987, 2024-2042` |
| 已共享、**不算**重复 | `BuildRoundedNineSlice`（30 行）、`RoundTemplateSize`、`BuildGradientQuads`、`BuildNineSlice`（图片九宫格切分） | `include/binui_paint.h:1252-1335, 1125-1196` |
| 三端 `BuildRoundedNineSlice` 调用点 | GL 3 处、VK 3 处、**D3D11 0 处** | 见 §1 |
| 真实三端分岔 | **2 处**（VK 描边单层、`rf<=0` 回退不同） | 见 §2 |
| 门禁覆盖缺口 | 圆角**描边**只有 thickness=1 被覆盖；thickness=2 只在"编辑中焦点环"出现，**无任何用例** | 见 §2.3 |
| 提案新增 API | `bin2d::RoundedLayerCount` + `bin2d::BuildRoundedLayer` | §3 |
| 预计改动 | bin2d +~120 行（含单测）；binUI 三端 393 → ~218 行（净减 ~175），再加 `binui_paint.h` 净减 ~27 → **共净减 ~200 行** | §3.3 / §4 |

---

## 1. 台账：重复到底在哪

### 1.1 总表

| # | 功能块 | GL | VK | D3D11 | 行数 | 逐位相同？ | 差异 |
|---|---|---|---|---|---|---|---|
| L1 | `fillRounded` | `binui_gl.cpp:1765-1790`（26） | `binui_vk.cpp:4098-4117`（20） | `binui_d3d11.cpp:1902-1936`（35） | **81** | **像素相同** | D3D11 不调 `BuildRoundedNineSlice`，自己内联 21 行；退化块（中段宽/高为 0）GL/VK 跳过、D3D11 照样发（像素一样，顶点数不同） |
| L2 | `strokeRounded` | `binui_gl.cpp:1792-1823`（32） | `binui_vk.cpp:4214-4240`（27） | `binui_d3d11.cpp:2000-2046`（47） | **106** | **不同（见 D1/D2）** | VK 只画 1 层；`rf<=0` 回退 GL/D3D11 = `fillRect` 整块、VK = 四条矩形；D3D11 内联 19 行 |
| L3 | `fillRoundedXf` | `binui_gl.cpp:1699-1763`（65） | `binui_vk.cpp:3730-3772`（43） | `binui_d3d11.cpp:1940-1998`（59） | **167** | **像素相同** | GL/VK 除 emit 与变量名外逐字相同；D3D11 内联 26 行 + `curLinear/curSrv` 状态；GL 多 15 行 `BINUI_XF_DUMP` 诊断 |
| L4 | `drawImageNine`（图片九宫格） | `binui_gl.cpp:2081-2093`（13） | `binui_vk.cpp:4443-4455`（13） | `binui_d3d11.cpp:2323-2335`（13） | **39** | 相同（切分数学已共享） | 只差"图片尺寸从哪张表查"和 `drawImageEx` 的调用 |
| — | 合计 | 123 | 90 | 141 | **393** | | |

### 1.2 逐块细节

**L1 `fillRounded`（81 行）** —— 三端的语义完全一致：

```
半径钳制 → 无效则 fillRect → 取模板 roundFor(r, false) → 九宫格 → 发 9 个四边形
```

GL（`binui_gl.cpp:1765-1790`）与 VK（`binui_vk.cpp:4098-4117`）都调
`BuildRoundedNineSlice`；D3D11（`binui_d3d11.cpp:1902-1936`）在 1913-1933 行把同一份
数学（`cu = c/t`、`strip0 = 0.5f`、`strip1 = 0.5f + 1/t`、9 次 `quad(...)`）又写了一遍。

**L2 `strokeRounded`（106 行）** —— 这是**唯一有真实行为差异**的一块：

| | GL（1792-1823） | VK（4214-4240） | D3D11（2000-2046） |
|---|---|---|---|
| 层数 | `round(max(1,thickness))` 层同心环 | **1 层**（没有循环） | `round(max(1,thickness))` 层 |
| 每层几何 | `ex=x+k, ew=w-2k, rfk=rf-k` | 直接用 `x,y,w,h,rf` | 同 GL，外加 `cr=min(rf-k, min(ew,eh)*0.5)`（该钳制**永不生效**，见下） |
| 九宫格 | `BuildRoundedNineSlice`（1815） | `BuildRoundedNineSlice`（4233） | 内联 19 行（2024-2042） |
| `rf<=0` | `fillRect` 整块 | **四条 `fillRect` 画框** | `fillRect` 整块 |

- `cr` 的钳制永不生效：因为 `rf <= min(w,h)/2`，所以 `rf-k <= min(w-2k,h-2k)/2` 恒成立。
  它是一行**冗余差异**，不影响像素，但说明三份代码已经在各自演化。
- VK 的注释 `binui_vk.cpp:4218` 自己写着"P2 先按四边 + 四角的九宫格模板画"——
  即 v0.11.8 给 GL/D3D11 加的"多层同心环"**从没补到 VK 上**。

**L3 `fillRoundedXf`（167 行）** —— 结构三端一致：

```
层数 = stroke ? round(max(1,thickness)) : 1
每层：lw=w-2k, lh=h-2k, rk=rf-k → 模板 → 九宫格 → 每片四角过 2×3 矩阵 → emitQuadUV
兜底：emitted==0 时发一个 (0,0,w,h) 的整块（UV 取模板正中 0.5）
```

GL 与 VK 除 emit 调用与局部变量名以外逐字相同（GL 用 `impl_->emitQuadUV(sp, uv, color)`，
VK 用 `d.emitQuadUV(sp, uv, color, &rt.tex)`）。D3D11 是同一套逻辑 + 内联数学 +
`curLinear/curSrv` 的切换。GL 额外有约 16 行 `BINUI_XF_DUMP=1` 的诊断打印（`binui_gl.cpp:1708-1711, 1736-1746, 1750`）。

**L4 `drawImageNine`（39 行）** —— 每个 13 行，形状都是：

```cpp
查图片尺寸 → BuildNineSlice(...)（**已共享**，include/binui_paint.h:1125）
           → 失败退 drawImageEx 整图 → 否则逐块 drawImageEx
```

这一块**没有数学**，只有"图片元数据查询 + 转发"。收口收益 ~20 行，风险=0，但收益也很低。

### 1.3 已经共享的部分（**不要**把它们算成"待收口"）

| 共享件 | 位置 | 三端使用情况 |
|---|---|---|
| `RoundTemplateSize`（模板边长 `max(8, 2r+4)`） | `include/binui_paint.h:1252-1255` | 三端 `roundFor` 都用它 |
| `BuildRoundedNineSlice`（九宫格几何） | `include/binui_paint.h:1269-1298` | GL 3 处、VK 3 处、**D3D11 0 处** |
| `BuildRoundedTileRGBA`（模板内容，来自 bin2d） | `include/binui_paint.h:1265` / 实现 `src/backends/binui_paint.cpp:216-227` | 三端 `roundFor` 各调一次（`binui_gl.cpp:1025`、`binui_vk.cpp:2015`、`binui_d3d11.cpp:538`） |
| `BuildGradientQuads`（渐变 + 圆角） | `include/binui_paint.h:1307-1335` | 三端 `fillGradientQuad`（VK `binui_vk.cpp:4136`） |
| `BuildNineSlice`（图片九宫格切分） | `include/binui_paint.h:1125-1196` | 三端 `drawImageNine` 各调一次 |
| `bin2d::RasterizeRoundedTileCoverage` | `bin2d/include/bin2d/raster.h:315-325` | 三端模板内容的唯一来源（P3 前半，0.23.0） |

所以 P3b 的靶子**不是**"几何数学"（那已经在 binui_paint.h 里一份了），而是
**"每层怎么取模板、九宫格怎么发、退化怎么兜底"这层计划逻辑**——它在三端各写了一遍，
而且 D3D11 连数学都抄走了。

---

## 2. 调研中发现的真实分岔（比"重复"更值得先修）

### D1（**现存 bug**）VK 的 `strokeRounded` 不画多层

`binui_vk.cpp:4214-4240` 没有层循环。可达路径只有一条：

- `binui_draw_nodes.h:1538` 的焦点环：`thick = editing ? 2.0f : 1.0f`（`binui_draw_nodes.h:1515`）。
  即"输入框编辑中"的焦点环是 **2px**——GL/D3D11 画 2 层同心环，**VK 只画 1 层**。
- 普通边框走 `binui_draw_nodes.h:1107`，`thickness` 恒为 `1.0f`；非 1px 的边框
  （用例里的 `border:2px`）走 `BuildBorderBands + fillRect`（`binui_draw_nodes.h:1112-1118`），
  根本不进 `strokeRounded`。

**当前门禁抓不到**：`PIXEL_CASES`（Makefile:605）与 `ANIM_CASES`（Makefile:679）里
没有任何用例带"编辑中焦点环"，`docs/issues/assets/*.html` 里 `focus` 的两处命中是
渐变 id（`svgfinish.html:10,53`），不是焦点环。见 §2.3。

### D2 `rf <= 0` 的回退语义不同（**当前不可达**）

GL/D3D11 `fillRect` 整块（等于"把描边画成实心"），VK 画四条矩形（真正的直角边框）。
但从调用点看 `rf<=0` 不可达：边框路径要求 `effRadius > 0.5f`（`binui_draw_nodes.h:1104`），
焦点环要求 `fr >= 1.0f`（`binui_draw_nodes.h:1537`）。**收口时按 GL/D3D11 的语义定死即可**，
但要写进 API 注释，免得将来有人去掉那个 guard。

### D3 D3D11 把共用数学抄了 3 份（潜在分岔）

`binui_d3d11.cpp:1913-1933 / 1962-1987 / 2024-2042`。目前与 `BuildRoundedNineSlice`
逐字节等价（我按表达式顺序对过：`c = r`、`cu = c/T`、`strip0/1`、9 片的坐标与 UV 一致），
但**没有机制保证以后一致**——P3a 的教训（三份解析式距离循环漂移）就是同一类。

### D4 门禁覆盖表

| 门禁 | 覆盖什么 | 与圆角/描边的关系 |
|---|---|---|
| `make pixels`（Makefile:611） | 18 个用例，GL vs 金标 PNG（`PIXEL_TOL=0.5`） | `radius-matrix`（纯填充，6 种半径 + 胶囊）、`radius-threshold`、`image-round`（圆角图 + 2px 边框，走条带）、`round-in-layer`、`layer-stroke` |
| `make pixels-vk`（Makefile:685） | 同上 18 + 4 demo，**GL vs VK**（`PIXEL_VK_TOL=1.5`） | 同左；**没有任何 thickness≥2 的 `strokeRounded` 用例** |
| `make pixels-d3d11`（Makefile:723） | 同上，**GL vs D3D11**（`PIXEL_D3D_TOL=3`） | 同左 |
| `make baseline-strict`（Makefile:818） | `tests/baseline/manifest.txt` 176 行：px 哈希 + `draw` 原语流哈希 + `qa` 文本，`--strict` 要求哈希完全相同 | `draw` 段的哈希来自 `tools/draw_bench.cpp` 的**接口调用流**（`strokeRounded` 记 `9*round(th)` 个四边形，`draw_bench.cpp:130-135`），与后端内部实现无关 → 三端内部改造**不会**改它 |
| `make gate-migrate`（Makefile:576） | bin2d 路径光栅 vs 老实现的耗时比 | 与圆角计划无关；但改造若引入每帧额外计算，这里看得见 |

结论：**P3b 的像素判据对 GL/D3D11 是硬的**（0 diff 可证），**对 VK 的厚描边是空的**——
D1 现在没被抓住，正是因为门禁没这一格。

---

## 3. 提案

### 3.1 为什么是"四边形计划"，不是"覆盖率"

三条路都试过口径上的比较：

| 方案 | 评价 |
|---|---|
| **（选定）四边形计划** | 三端现有管线就是"模板纹理 × 9 片四边形"，计划是纯几何，零像素风险；GPU 上仍是 9 片，不增不减 |
| 覆盖率（每像素） | 会把圆角从"1 张模板 + 9 片"变成"每帧重算 t×t 覆盖率 + 条带下发"，像素**一定变**（模板采样 vs 覆盖率采样在弧上是两种 AA 口径），且慢 |
| 整块几何（4 个角各一个三角形扇） | 需要把弧做成折线，等于放弃模板（P3a 刚把模板内容收口到 bin2d），收益为负 |

所以：**几何计划归 bin2d，模板内容仍归 bin2d，纹理创建/缓存留后端**。

### 3.2 API（加在 `bin2d/include/bin2d/raster.h`，随 0.10.0 发）

```cpp
// ---- 圆角九宫格计划（0.10.0）：给"自己有四边形管线"的宿主用 ----
//
// 语义与 binUI 0.16 起的三端实现逐位等价（同序同式，见 tests 的逐位比较）：
//   · fill   = 1 层；
//   · stroke = round(max(1,thickness)) 层同心环，每层"缩 1px、半径减 1"，
//              层在 (w-2k <= 1 || h-2k <= 1 || rf-k <= 0) 处停止；
//   · 每层 9 片，顺序固定 TL,TR,BL,BR,上,下,左,右,中（与 BuildRoundedNineSlice 一致），
//     中段宽/高为 0 的片**不输出**；
//   · 半径 <= 0 时返回 0：调用方按自己的老回退走（binUI 是 fillRect）。
//
// 输出布局沿用 BuildNineSlice：每 8 个 float 一片 = (u0,v0,u1,v1, x,y,w,h)。

// 层数（fill 恒为 1；半径无效返回 0）
int RoundedLayerCount(float w, float h, float radius, bool stroke, float thickness);

// 第 layer 层的 9 片；outRadius/outTile 是这一层要取的模板（radius 已钳到 >=1，
// tile = RoundedTileSize(radius)）。返回片数（0..9），0 = 该层不存在。
int BuildRoundedLayer(float x, float y, float w, float h, float radius, bool stroke,
                      int layer, float out[9][8], int* outRadius, int* outTile);
```

两个函数、无分配、无回调、可直接单测。为什么拆成"层数 + 取一层"而不是"一次返回整份计划"：
层数上界是 `min(w,h)/2`（可以是 400+），整份计划要 400×9×8 float ≈ 115 KB 的栈/堆；
拆开之后调用方每层只拿 9 片，天然分页。

**也要一起挪的**：`BuildRoundedNineSlice`（`include/binui_paint.h:1269-1298`）可以由
`BuildRoundedLayer` 直接取代，`binui_paint.h` 里只留一个 3 行的兼容包装（或直接删，
调用点见 §4）。`BuildGradientQuads`（同文件 1307-1335）改成调 `BuildRoundedLayer(..., 0, ...)`。

### 3.3 三端各自改成什么样

**GL `fillRounded`（26 → 16 行）**：

```cpp
void BinUIGL::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    if (!impl_->ready || w <= 0 || h <= 0) return;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) { fillRect(x, y, w, h, color); return; }
    float q[9][8]; int rr = 0, tile = 0;
    const int n = bin2d::BuildRoundedLayer(x, y, w, h, rf, false, 0, q, &rr, &tile);
    RoundTex& rt = impl_->roundFor(rr, false);
    if (n <= 0 || !rt.tex) { fillRect(x, y, w, h, color); return; }
    Premult(color);
    impl_->curTex = rt.tex;
    for (int i = 0; i < n; i++)
        impl_->emitQuad(q[i][4], q[i][5], q[i][4] + q[i][6], q[i][5] + q[i][7],
                        q[i][0], q[i][1], q[i][2], q[i][3], color);
    impl_->curTex = impl_->whiteTex;
}
```

**GL/VK/D3D11 `strokeRounded`（32/27/47 → ~20 行）**：

```cpp
    const int layers = bin2d::RoundedLayerCount(w, h, rf, true, thickness);
    for (int k = 0; k < layers; k++) {
        float q[9][8]; int rr = 0, tile = 0;
        const int n = bin2d::BuildRoundedLayer(x, y, w, h, rf, true, k, q, &rr, &tile);
        if (n <= 0) break;
        RoundTex& rt = roundFor(rr, true);            // 各端自己的模板
        if (!rt.tex) break;
        /* 各端 9 次 emitQuad */
    }
```

**`fillRoundedXf`（65/43/59 → ~40/30/38 行）**：同一份计划 + 每片四角过矩阵，
`layer` 循环保留（`stroke` 时层数来自 `RoundedLayerCount`）。GL 的 `BINUI_XF_DUMP` 诊断保留。

**逐端预计行数变化**：

| 端 | fillRounded | strokeRounded | fillRoundedXf | 合计（前 → 后） |
|---|---|---|---|---|
| GL | 26 → 16 | 32 → 20 | 65 → 40 | 123 → **76** |
| VK | 20 → 15 | 27 → 20 | 43 → 32 | 90 → **67** |
| D3D11 | 35 → 16 | 47 → 21 | 59 → 38 | 141 → **75** |
| 合计 | | | | **393 → ~218**（净减 ~175） |

另外 `include/binui_paint.h` 删 `BuildRoundedNineSlice` 30 行、`BuildGradientQuads` 改 3 行
（净减 ~27）。**总计净减约 200 行**。

---

## 4. 分步改造（每步都能单独回滚）

| 步 | 改什么 | 出口判据（命令） | 回滚 |
|---|---|---|---|
| **S0** | bin2d 加 `RoundedLayerCount` + `BuildRoundedLayer` + `tests/test_raster.cpp` 里**逐位比较**测试（见 §5.1） | `make -C ../bin2D test` 全绿；`make -C ../bin2D quality` 的 `[九宫格]` 行不变 | 只动 bin2d，binUI 不重建就无关 |
| **S1** | GL 三函数换计划（纯重构） | `make pixels`（18/18 零差异）、`make baseline-strict`（GL px 哈希不变）、`make gate-migrate`（不劣化 >1.2×） | 单 commit，revert 即回 |
| **S2** | D3D11 三函数换计划（删掉 66 行内联数学） | `make pixels-d3d11`（18 例 + 4 demo 零差异）、`make baseline-strict` | 同上 |
| **S3** | VK `fillRounded` + `fillRoundedXf` 换计划 | `make pixels-vk`（零差异）、`make baseline-strict` | 同上 |
| **S4** | **（唯一改像素）** VK `strokeRounded` 补多层；同时把 `round-stroke-thick` 用例加进 `PIXEL_CASES` | 改前：新用例在 `pixels-vk` 上**红**（记录 diff）；改后：`pixels-vk` 绿、`pixels` 先 `make pixels-update` 出金标、`baseline-strict` 里 VK 该用例哈希变化其余不变 | 单独 commit；回滚 = 退回单层（同时撤用例） |
| **S5** | `binui_paint.h` 删 `BuildRoundedNineSlice`；`BuildGradientQuads` 改调 `BuildRoundedLayer` | `make pixels` + `pixels-vk` + `pixels-d3d11` 三绿 + `baseline-strict` | 同上 |
| **S6**（可选） | `drawImageNine` 三端合流（39 → ~15 行） | 三端像素零差异 | 收益低，建议单独评估后再做，默认**不做** |

顺序理由：先 GL（金标就是 GL 出的，最容易证 0 变化）→ 再 D3D11（删最多行，风险最高，
放在 GL 之后有参照）→ 再 VK（只换几何，不碰描边层数）→ 最后才动"会改像素"的 S4。

---

## 5. "像素 0 变化"的验证步骤

### 5.1 bin2d 侧：逐位等价测试（**这是 0 变化的地基**）

`tests/test_raster.cpp` 里对参数网格逐位比较：

```
w,h ∈ {8,9,10,17,64,101,200,201} × r ∈ {1,2,3,7,16,33}（含 r > min(w,h)/2 的钳制）
× stroke ∈ {false,true} × thickness ∈ {1,2,3,5,8}
```

比较方式：把现在 `include/binui_paint.h:1269-1298` 的算法原样抄进测试作为参照，
`std::memcmp(a, b, sizeof(float)*8*n) == 0`。**要求逐位，不是近似**——因为新旧实现
只要表达式顺序相同（`c=r; cu=c/T; s0=0.5f; s1=0.5f+1/T`），结果就必然逐位相同。
这条测试一旦绿，S1~S3 的像素 0 变化就是结构性的，而不是靠运气。

### 5.2 binUI 侧：三道门禁 + 一次快照

```
make pixels          # GL vs 金标，18/18，容差 0.5 —— 期望：零差异
make pixels-vk       # GL vs VK，22 例，容差 1.5 —— S1~S3 后零差异；S4 后仍零差异
make pixels-d3d11    # GL vs D3D11，22 例，容差 3   —— S2 后零差异
make baseline-strict # 176 行清单，哈希必须完全一致（px + draw + qa）
```

`baseline-strict` 的 `qa` 段是 bin2d `make quality` 的**stdout 原文**——如果 bin2d 的新
API 让 quality 工具的输出多一行/少一行，这一条会红（bin2d 0.9.1 刚为此把计时行挪到
stderr）。所以 S0 里**不要**给 quality 工具加任何新输出。

### 5.3 S4 的"改前红、改后绿"

新用例 `docs/issues/assets/round-stroke-thick.html`：两行三列，分别是
1px 圆角边框 / 2px 圆角边框（走 `BuildBorderBands`，对照组）/ 编辑中焦点环（2px，
走 `strokeRounded`）+ 一个 radius=0 的直角边框（覆盖 D2 的回退分支，虽然不可达）。

流程：先只加用例不改代码 → `make pixels-vk` 应当红（VK 的 2px 环只有 1px 宽）；
再改 S4 → 三端绿。**这条红是证据**：它证明"改前就分岔"，而不是重构引入的。

---

## 6. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **浮点不同序**（把 `cu = c/t` 写成 `c/t` 前先钳 radius 等） | 圆角边缘整体差 1 像素级 | §5.1 的逐位比较测试是硬门；S1~S3 每步都跑 |
| `rt.t` 与计划里的 `tile` 不一致（某端 `roundFor` 用了别的模板边长） | UV 缩放错，圆角看起来"偏一圈" | 收口时把三端 `roundFor` 里的 `RoundTemplateSize(r)` 统一改调 `bin2d::RoundedTileSize(r)`（同一个函数，改 3 行） |
| **S4 改像素**（这一轮唯一一处） | VK 的 2px 焦点环变粗 2px | 单独 commit + 先加用例 + 记录"改前红" |
| D3D11 跳过退化片 → 顶点数变化 | 只影响统计，不影响像素 | `baseline-strict` 的 `draw` 哈希来自接口调用流（`draw_bench.cpp`），不随内部顶点数变；但要在 PR 描述里说明 |
| 每帧多一次函数调用（计划构建） | 预计 ≤1% | `gate-migrate` 1.2× 阈值；`bench-draw` 对比 |
| bin2d 版本联动 | binUI 依赖 `../bin2D/build-mingw/libbin2d.a`（Makefile:51-53） | S0 先发 bin2d，再改 binUI；两仓各自 CHANGELOG |

---

## 7. 明确**不**收口（以及为什么）

| 不做 | 理由 |
|---|---|
| **`roundFor`（模板纹理创建 / 上传 / LRU）** GL `binui_gl.cpp:1003`、VK `binui_vk.cpp:2038`、D3D11 `binui_d3d11.cpp:517` | 三端的纹理 API 完全不同（GL tex / VK image+view+sampler / D3D11 SRV+sampler），且已有各自的 LRU 语义（`roundtex-lru` 用例守着）。bin2d 没有 GPU 纹理概念，硬收口会把"上传"也拖进来，属于 P5 的范畴 |
| **`drawDropShadow`**（GL `binui_gl.cpp:653-700`、VK `binui_vk.cpp:3261-3308`、D3D11 `binui_d3d11.cpp:1311-1362`，共 ~150 行） | 它其实是"离屏层 + 模糊链 + 保留混合"的编舞（别名/RT/FBO/render pass/混合状态全在端内），算法同构但**每一步都是后端 API**。要收口得先让 bin2d 拥有"离屏目标 + 多级降采样链"，那是 P4/P5 的事。P3b 不碰 |
| **`drawImageNine` 合流**（39 行） | 里面没有数学（切分已在 `BuildNineSlice`），只剩"查图片尺寸 + 转发"；收益 ~20 行、风险 0，但也不解决任何分岔。列为**可选 S6** |
| `emitQuad` / `emitQuadUV` | 各端顶点布局/批次模型不同（GL 立即提交、VK 攒批次、D3D11 攒顶点），这是后端的本职 |
| 焦点环的"虚线"分支（`BuildDashedRoundRing`，`binui_draw_nodes.h:1532`） | 已经是共享几何 + 逐段 `fillLine`，与圆角模板无关 |

---

## 8. 复核用命令（本文所有数字都来自这些）

```sh
# 台账行号与行数
grep -n "fillRounded\|strokeRounded\|fillRoundedXf\|drawImageNine" src/backends/binui_{gl,vk,d3d11}.cpp
# 三端内联数学（D3D11 独有）
sed -n '1902,2046p' src/backends/binui_d3d11.cpp
# 共用件（不该算重复）
sed -n '1125,1335p' include/binui_paint.h
# 门禁
grep -n "^PIXEL_CASES\|^ANIM_CASES\|^pixels\|^baseline-strict" Makefile
# 用例里有没有厚描边 / 焦点环
grep -rn "border:2px\|focus" docs/issues/assets/*.html
```

---

## 附：与既有计划的关系

- `docs/migrate-to-bin2d.md` P3 的"已落地"段已经写明：
  "圆角模板已在 0.23.0 收口到 bin2d；`strokeRounded` 的 '9 × 线宽层数' 仍在，它的三条实现
  现在都只消费 bin2d 的模板"——**本文就是那半句话的展开**。
- 本文不改 `migrate-to-bin2d.md`（本轮只允许写一个文件）；S0 落地时再回填它的 P3 段。
