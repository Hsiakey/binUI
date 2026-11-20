# 迁移计划：binUI 的路径光栅换成 bin2d

> 结论先写（**2026-11-18 更新：开工条件已达标**）：P0 做完了 —— bin2d 0.4.2 的
> `FillPath` 在同口径测量里是现实现的 **0.98 倍耗时**（0.4.1 时是 1.61 倍），
> 开工条件 ≤1.20 倍达标，且 `B / A2 ≤ 1.00` 的目标也到了。下一步是 **P1（立基线）**。
> 记这一段的必要性：先量、再算、后改 —— 顺序反了就会把性能问题搬进 UI，
> 而不是把几何能力搬过来。

## 0. 三句话

1. **目标**：UI 不再自己算条带，`<path>` 的填充/描边走 `bin2d::Path + raster`；
   前端（几何、覆盖率、裁剪、图层）只有一份实现，三个 GPU 后端只负责"把四边形/位图刷上屏"。
2. **判据**：像素（三端 `make pixels` / `pixels-vk` / `pixels-d3d11` 0 diff）+ 性能
   （`make gate-migrate` ≤ 1.20×）+ 一条路径要么走老实现要么走新实现，不许共存。
3. **现在的位置**：像素那条已经有把握（bin2d 与 binGPU 四端逐像素 0 差）；
   性能那条**已达标**（0.98×，见 §1 的后半段），可以进 P1。

## 1. 开工条件的实测（`make gate-migrate`）

场景：800×600 圆角卡片 + 200 个 120px 图标（星形 10 点 / 齿轮 16 齿 / 圆 64 段各 1/3），
目标一块 1280×720 内存 Surface，两边都含清屏、都不含布局/遍历。

| 路线 | 内容 | 0.4.1 实测 | **0.4.2 实测** |
|---|---|---|---|
| A1 | binUI 现状**只建条带**（`BuildPathFillBandsAA`，119 668 条/帧） | 7.30 ms | 7.21 ms |
| A2 | binUI 现状**条带 + 落像素**（同口径；条带填充 2.0 ms） | 9.31 ms | 9.32–9.48 ms |
| B | bin2d `FillPath`（扫描线 + 覆盖率 + 混合一次到底） | **15.03 ms** | **9.22–9.25 ms** |
| — | **B / A2** | **1.61×** ❌ | **0.98×** ✅ |

分解（0.4.1）：15.03 ms 里卡片（480k px）约 1.95 ms、200 个图标约 12.5 ms
（**62 µs/个**）、清屏 0.11 ms。bin2d 在大形状上是 1.45 ns/px（`make bench` 的 440px
星形），小形状却付了 **5 倍单价** —— 钱花在**每形状固定开销**上。

**根因（0.4.2 用消融实验修正过）**：不全是"内部像素被乘了 8 倍"。抄一份 `raster.cpp`
做对照，三项分别是整像素段逐像素累加 5.07 ms（55%）、每条子扫描线重建交叉点并排序
4.62 ms（这一项被高估）、求交+建边 1.88 ms（20%）。所以 0.4.2 只干掉了能"不动语义"
的那一项（整像素段改**差分 + 行尾前缀和**，像素值逐位不变），拿到 1.63×。
剩下那 ~2× 需要"每行只求 2 次交 + 梯形解析面积"，会改竖直方向的 AA 语义
（8 次采样 → 精确面积），是**新能力**，不混在性能轮里（记为 bin2d roadmap ① 的开放项）。

## 2. 阶段

### P0：bin2d 扫描线性能轮 —— ✅ 已完成（bin2d 0.4.2）

**结果**：`B / A2` 1.61× → **0.98×**（B 15.03 → 9.22 ms）；`make test` 125 项全过、
`make quality` 七项数字与改前**逐项相同**（像素值不变）、`make bench` 六项合计
42.9 → 38.9 ns/px。原始计划与"为什么没走解析覆盖率"的复盘见 bin2d
`CHANGELOG.md` 的 0.4.2 条与 `docs/perf.md` 的「迁移开工条件那一轮」。

下面是当时写的计划原文（留着对照，别当成待办）：

**做法（按预期收益排序）**：

1. **逐行解析覆盖率**（替代 8 条子采样）：一条直边在一像素行内的覆盖面积是**梯形解析式**
   （端点 y 分量与斜率决定），内部像素覆盖率恰为 1。这样每行只做 1 次求交 +
   边界像素各算一次面积，行宽方向的累加从 8× 降到 1×。预期 **3~5×**，
   而且**质量变好**（今天的 1/8 竖直量化消失，边缘位置误差仍为 0，覆盖率误差 → 浮点级）。
2. **边界像素才做细算**：内部段直接写满（已有"整像素段"优化，现在被 8 条子采样绕开了）。
3. **每形状固定开销**：边表构建 / 展平 / 包围盒裁剪各一次，检查有没有按行重算的东西。
4. **缓存折叠**：同一个 `Path` + 同一个变换在连续帧重复绘制时，展平结果可缓存
   （`Path::flatten` 已经是"按容差+缩放"，天然是键）；先不做，等 1~3 落地后再看还差多少。

**验收**：
- `make gate-migrate` ≤ 1.00×（200 图标与 50 图标两个规模都测）；
- `make test` 125 项全过（覆盖率断言会**变好**：圆面积误差从 0.24% 收到更小；
  精确值断言必须仍然精确 —— 整像素对齐时覆盖率还是恰好 1）；
- `make bench` 六项不退化（±5% 内），大形状上应该顺带更快；
- **不许**为了快而退化到硬边：`docs/raster_rules.md` 的覆盖率语义不变。

**风险**：解析覆盖率在 `nonzero` 自交路径、描边的"并集取 max"、以及细长三角形
（一行内两次穿越同一像素）上要单独验。对策：保留今天的 8 条子采样实现作为**测试参照**
（只在测试里用，不进生产路径），对它做逐像素容差对拍（≤1 级）。

### P1：像素与哈希门禁先立起来 —— ✅ 已完成（binUI 0.20.3）

**产出**：`tests/baseline/manifest.txt`（提交进仓库）—— 每个用例、每一端一行
FNV-1a 64 指纹（算在原始 ARGB 上，PNG 编码器换了也不影响），外加 `draw_bench --hash`
的原语流哈希与 bin2d `make quality` 的数字。生成/校验工具 `tools/baseline_snapshot.cpp`。

```sh
mingw32-make SHELL=sh baseline          # 重生成快照
mingw32-make SHELL=sh baseline-check    # 报告：哪一端、哪个用例动了
mingw32-make SHELL=sh baseline-strict   # 要求这一步逐位相同时用
```

**为什么不是"再跑一遍 make pixels 就行了"**：`pixel_diff` 只回答"在容差内吗"
（0.5% 像素 / 通道差 8）。一个真实退化完全可以躲在这条线内 —— 比如整页文字从抗锯齿
变成硬边只影响 0.3% 的像素，容差照样绿。所以 P1 补的是另一半能力：
**迁移每步都能回答"动的正好是该动的那些吗"**，而不是"要不要接受"。

判据分两级，写死在 `tests/baseline/README.md`：硬门禁仍是 `pixel_diff`；
快照管"动了什么"—— 用例增删/尺寸变直接失败，哈希变化默认只报告（迁移本来就该动几个
像素），要求逐位相同时才 `--strict`。允许重生成快照的条件也写在那里（能逐条解释 +
CHANGELOG 写明 + 硬门禁先绿）。

用例集合 = `pixel_diff` 的 18 个用例（四端）+ 4 个整页示例 + 28 个动画中途帧用例
（GL/VK/D3D11；GDI 侧本来就没有这些基准图），合计 **168 项像素指纹 + 9 项原语流哈希 +
11 行画质数字**。`animshow` 不进原语流哈希：它的原语流由动画时钟决定，自己就会变。

### P2：`<path>` 的填充 + 描边换到 bin2d —— ✅ 已完成（填充 0.21.0 / bin2d 0.5.0，描边 0.22.0 / bin2d 0.6.0）

**填充（0.21.0）**：走 `bin2d::RasterizeCoverage`（bin2d 0.5.0 新导出的覆盖率查询，
与 `FillPath` 同一个光栅器），binUI 只保留"覆盖率 → 条带"。删掉约 380 行老扫描线
（`PaintScanEdge` / `BuildPaintScanEdges` / `ActivateScanEdges` / 4 条子采样 + 1024 定点
累加器 / rowStep 降级 / 4096px 兜底）。实测 18 个 GDI 基准 0 个不同像素、三端 0 个不同像素、
快照只动 35 项且全是路径用例。细节见 `CHANGELOG.md` 0.21.0。

**描边（0.22.0）**：走 `bin2d::RasterizeStrokeCoverage`（bin2d 0.6.0）—— 每段四边形 + 拐点
连接块 + 端帽**逐块栅格化、同一像素取 max**（并集），binUI 侧只剩"并集覆盖率 → 条带"。
删掉 ~80 行 `PaintPathStroke`（逐段 fillLine + 拐点圆/多边形回调），并顺手修掉两处老问题：
接缝逐段混合两次（半透明描边每个接缝深一条）、拐点/端帽走后端 `fillRounded`（4× MSAA
采样位置在 GL/VK 上互为镜像 → 三端本来就分岔）。GDI 端也从"整数 HPEN + Ellipse"换成同一份
条带，于是**四端第一次真正用同一条路画描边**：GDI 与 GL 在 `svgjoin` / `svgstroke` 上
从 1284 / 1016 个不同像素降到 **0**。细节见 `CHANGELOG.md` 0.22.0。

原计划（保留作对照）：
- **改**：`binui_draw_nodes.h` 里 `<path>` 的绘制分支 → 构造 `bin2d::Path` + `FillPath/StrokePath`。
- **删**：`include/binui_path.h` 的扫描线填充。
- **留**："节点 → 路径"的那层壳（`BinUIPath*` 结构体是 ABI，不能动）。
- **验收**：三端像素 0 diff（图标/图表/圆角全在同一张基准里）；
  `gate-migrate` 与 `bench-draw` 都不退化；SVG 示例（`svgdemo`）的 819 KB 顶点量**下降**。
- **回滚条件**：任何一端出现 >0 像素差，或单帧退化 >20%。

### P3：圆角 / 投影 / 九宫格 —— ✅ 已落地（0.23.0 + 0.25.0）

**已落地（0.23.0）**：三个 GPU 后端的**圆角模板内容**收口到 bin2d
（`RasterizeRoundedTileCoverage`）。GL / VK / D3D11 原本各写一份同样的解析式距离循环，
现在只有一份。判据是"像素一位不动"：`baseline-strict` 188 项**完全一致**（含三端各 50 个
用例的原始 ARGB 逐字节比对）、`pixels` 18/18 零差异、`pixels-vk` / `pixels-d3d11` 零差异、
全量重编 0 警告。几何部分（`BuildRoundedNineSlice`）本来就在 `binui_paint.h` 里共用。

**还没做**：GDI 端的圆角/投影/九宫格 —— **已取消**：GDI 渲染后端整体移除（0.24.0），
所以"把 GDI 的圆角换成 bin2d 的 AA 覆盖率"这件事没有对象了。像素基准图改由 GL 出，
三端一致的判据不变。

- ~~`fillRounded` → `bin2d::FillRoundedRect`；假投影 → `bin2d::Shadow`；
  九宫格 → `bin2d::DrawNineSlice`~~（不迁：这三个 API 的语义是"落 CPU 像素"，
  而三端要的是四边形；真正该收口的是"几何与层数"，见下一条）
- ~~**删**：三端各自的圆角模板与描边层叠实现~~ —— **第二半已落地（0.25.0）**：
  `RoundedLayerCount / BuildRoundedLayer / BuildRoundedLayerQuads / ...Xf`
  进了 `binui_paint.h`，三端的填充/描边/仿射圆角都只调这一份。
  D3D11 那 66 行手抄的九宫格 UV 数学删掉；VK 的 `strokeRounded` 补上多层循环
  （此前 `thickness` 被丢弃，>1px 也只画 1px —— 唯一一处三端分岔）。
  后端代码净减 **114 行**（+123 / −237），共享实现 +94 行（只有一份）。
- **验收**：`make pixels` 18/18 零差异、`baseline-strict` 172 项完全一致
  （三端各 150 个像素用例的原始 ARGB 逐字节比对）、三端在"2px/3px 圆角环"临时页面上
  0 diff、`test_elements` 新增 35 条几何断言（v49）。
- **待办**：聚焦编辑环（`thickness=2` 的唯一可达路径）没有像素用例 —— 它需要运行时
  焦点态，静态出图工具拍不到；目前靠共享计划单测 + 三端代码收口兜底。

### P4：整份后端交给 bin2d —— 🔶 第一半已落地（0.26.0）

P3 之前的迁移都是"把某个原语的**几何**收口到共用头文件"，后端仍然是三份各自发射四边形。
P4 换了个层次：**`IBinUIGfx` 直接由 bin2d 实现**，binUI 不再写图形 API。

```
binUI  ──►  bin2d / bin3d  ──►  binGPU  ──►  GL / Vulkan / D3D11
```

**已落地（0.26.0）**：`binui::BinUI2D`（`src/backends/binui_bin2d.{h,cpp}`，~700 行），
`IBinUIGfx` 的 30 个原语全部映射到 `bin2d::Canvas`。文件里一行 `gl*` / `vk*` / D3D 调用都没有。

复用（一行没改）：

| 复用 | 说明 |
|---|---|
| `binui_glyph_atlas.h` | 只换 `IGlyphTexHost` 的实现：`glTexSubImage2D` → 写进 `bin2d::Image` |
| `binui_paint.h` | 九宫格切分、圆角层几何，同一份数学 |
| `binui_win_image.h` | 同一个 GDI+ 解码，BGRA 直通 → 预乘 `bin2d::Image` |
| `drawNodesBinUI()` | 节点树遍历 |

代价（如实记账）——

- **覆盖率 AA 与 GPU 不同**：bin2d 用 8× 子采样解析覆盖率，GPU 用 1px 模板环 / 条带。
  18 个像素用例里 **99.4%–100% 的差异像素落在 1px 边缘上**（`circle` 99.9%、
  `image-round` / `image-mip` 100%）。这是换实现的标准差，不是布局或颜色错。
  所以 `make pixels-bin2d` 用**容差 0 的自回归**（对照 `tests/pixels-bin2d/`），
  与 GL 基准的差异当年用 `make bin2d-vs-gl` **只报告不判失败**
  （那个目标随 GL 后端在 0.37.x 删除；`docs/pixels` 现在只作为 `pixels-bin2d-gpu` 的参照）。
- **发现 GL 基准一处伪影**：`grad-angle` 第三个面板（180° 垂直渐变）在 GL 输出里 y=43
  这一行是 `(202,0,53)`，上下两行是 `(234,0,21)` / `(191,0,64)` —— 单调渐变中间插了一行
  偏暗的接缝。bin2d 输出 `(206,14,63)`，单调。**这条 bin2d 更正确。**
- **已知缺口**：`setImageFilter` 未实现（`svg-filter` 用例 13.8% 差异）；
  `pushLayerEx` 的 `drop-shadow` 未实现（blur 已走 `pushOffscreen` + `bin2d::Blur`）。

**踩出来的 bin2d bug（已在 0.10.1 修）**：`Canvas::clipRect` 用裸 `state().xf` 映射，
落像素却用 `deviceTransform()`（xf 减层原点）—— 在图层里差一个 origin，层内第一次
`clipRect` 就把内容裁成 0×0。`opacity` 用例 48.9% → 0%。回归测试见
`bin2D/tests/test_composite.cpp::TestClipRectInsideLayer`。

**踩出来的 bin2d 后端 bug（已在 0.31.1 修）**：字形位图从 `binui_glyph_atlas.h`
出来时已经是预乘白色（`p[0..3] = 覆盖率`），而 `GlyphHost2D::updateTex` 走了
`BlitBgraStraightInto` 又预乘了一次 —— 纹素变成 `(a²/255, a²/255, a²/255, a)`，
覆盖率在 RGB 上被平方。症状：**白字压浅底出现一圈灰边**（白底上字形核心只有 191
而不是 255），整体墨量偏少，小字号拉丁发虚，与汉字放一起就像"有的粗有的细"。
三份 GPU 后端把字节原样上纹理，所以只有 bin2d 这条有。改成 `BlitBgraPremulInto`
（只换字节序）之后，白字压蓝底的文本像素与 GL **0 差异**，`svg-filter` 与 GL 的
差异 2.446% → 0.513%。

> 定位方法记一笔：把同一个字形分别压在**黑 / 白 / 灰**三种底色上。只有"源 rgb 比
> 正确值暗"这一种解释能同时对上三组数（s=243/a=249、s=85/a=149，两组都等于 a²/255）。
> 只压在一种底色上会误判成"阴影"或"字体粗细"。


**还没做（P4 第二半）**：

- ~~窗口呈现~~ —— **已落地（0.26.1 + bin2d 0.11.0）**：`BinUI2D::init(hwnd,…)` 建一台
  `bin2d::GpuCompositor`（窗口版），`present()` → `presentSurface()` → bingpu 交换链。
  后端按 GL → Vulkan → D3D11 找第一个能起来的；一个都没有就退回"CPU 出图 +
  `StretchDIBits` 贴屏"。`tools/bin2d_window` 实测三条路都通（`present=gl` /
  `present=bin2d+gdi` / `present=bin2d+offscreen`），抓图内容一致。
- 三份 GPU 后端的去留：窗口路径通了，但**现在还不能删** ——
  ① ~~`BinUIAppT` 的输入/IME/拖拽只在 GL/VK 上真跑过~~ —— **已作废（0.27.0）**：
  `make demos-2d` 产出 9 个 `*_2d.exe`（含 `appshow_2d` 这个外壳演示），
  `tests/test_shell` 用 38 项断言把外壳与后端之间的契约钉住（消息 / IME 上下文挂载 /
  脏区只清脏区 / 图层 / 裁剪栈 / 尺寸 / DPI / 线程亲和 / 双实例）。外壳只认
  `IBinUIGfx` 的 8 个方法，`BinUI2D` 全部实现 → 这条理由不再成立；
  ② 性能账还没结（见下）—— 与"能不能删"无关，是另一件事。

#### 删后端的清单（0.27.0 审计：`IBinUIGfx` 之外还差什么）

> **⚠ 历史记录（0.27.0 时的审计）**：这份清单在 0.29.0 ~ 0.37.x 之间逐条还清了，
> 所列的 `binui_gl.*` / `binui_vk.*` / `binui_d3d11.*` 与 `BinUIGL` / `BinUIAppGL`
> **已于 2026-11-20 整份删除**（清单见 [`backend_layering.md`](backend_layering.md) 第 3c 步）。
> 下面的表格读作"当时还差什么、后来怎么补的"，不是待办。

规模：`binui_gl.*` 111,793 B + `binui_vk.*` 233,825 B + `binui_d3d11.*` 117,275 B
≈ 452 KB。删文件本身不难，难的是下面两类。

**A. 编译期硬依赖（不补就编不过）** —— 下面四行已全部完成（`binui_charts.h` 改成 `IBinUIGfx&`、
`src/binui_c.cpp` 改成 `BinUIApp2D`、`binui_ui.h` 只剩 bin2d、`Makefile` 只链 `binui_bin2d.o`）：

| 位置 | 现状（0.27.0 当时） | 要改成 |
|---|---|---|
| `include/binui_charts.h:14,31,79` | `virtual void draw(BinUIGL&)` | ✅ 已改 `IBinUIGfx&`（内部只用到 `fillLine`，本就是接口方法） |
| `src/binui_c.cpp:52,211,216` | `std::unique_ptr<BinUIAppGL>` | ✅ 已改 `BinUIApp2D`（0.37.x 的 `BinUIAuto` 中间态也一并退场） |
| `src/backends/binui_ui.h:21-23,36-38,71-81` | 默认分支是 GL | ✅ 已改：**只剩 bin2d**，按后端名分发的分支全删 |
| `Makefile:212-293 / 469`、`CMakeLists.txt:82-84,114-146` | 9 个 demo 全链 `binui_gl.o`；**CMake 里没有 bin2d 目标** | ✅ 已改：`BACKEND_OBJS` 只有 `binui_bin2d.o` |

唯一的"薄壳"：`drawNodesGL`（`binui_gl.cpp:2104`，6 行，等于 `drawNodesBinUI`）、
`flushFrame`（只服务 `make bench-gl`）、`BinUIGLGdi/BinUIAppGLGdi` 别名（零调用者）。
`tests/` 目录对四份后端**零引用** —— 测试层早就后端无关了（`tests/baseline/` 里的三端指纹
随后端删除成为历史存档）。

**B. bin2d 还缺的能力（补了才谈得上等价替换）**

| 能力 | 三份 GPU 后端（**已删除**） | bin2d 现状 | 影响 |
|---|---|---|---|
| **透明分层窗** | GL `glReadPixels`→`UpdateLayeredWindow`（gl.cpp:1453-1503）/ VK 1599 / D3D 1445 | ✅ **0.28.0 已补**（`present()` 第一支 → `bin2d+layered`） | 无 |
| `setImageFilter` | 三端都有 | ✅ **0.28.0 已补**（`ColorMatrix` + 按 (图,矩阵) 缓存变体） | 无 |
| 图层 `drop-shadow` | 三端都有 | ✅ **0.28.0 已补**（上色轮廓 → σ 模糊 → 逆序合成到本体下面） | 无 |
| `readbackPixels` | VK 4062 / D3D 2327（GL 没有，靠 BitBlt） | `pixels()` / `savePng()`（预乘 RGBA，语义不同） | 只影响 `vk_shot` / `d3d11_shot` 等工具 —— **已随后端删除** |
| VK 诊断四项 | `setCaptureReadback` / `swapchainActive` / `presentUsedGdi` / `deviceMemoryBytes` | 无 | 只影响 `vk_window` / `vk_stress` —— **已随后端删除** |
| `presentPacesFrames` | 三端都有真判据 | 恒 false（保守） | 只影响帧相位，不挂功能 |

MSAA 解析 / DPI / `RemoteSession` / `fullscreenPresent_` **都不是后端的事**：
MSAA 是 GPU 的做法，bin2d 在覆盖率光栅里解析掉了；后三项在外壳（`binui_app.h`）。

#### 顺带量出来的两个 backend 口径问题（0.28.0）

1. **`filter: blur(Npx)` 在 N < 4 时三端完全没糊。** `compositeThroughLevels()` 里那句
   分离高斯被关在 `if (levels > 0)` 里面，而 `PaintFilterBlurLevel(N)` 在 N<4 时返回 0，
   整块被跳过 —— 只剩"抓层内容 → 1:1 拷回去"。GL 已修（0 级也跑卷积）。
2. **模糊的 σ 语义。** `bin2d::Blur(radius)` 的 radius 是盒式核半宽，而喂进来的数字
   全是 σ（CSS `blur(Npx)` / SVG `stdDeviation`）。σ=3 的盒式实际只有 σ≈2.0。
   bin2d 0.12.0 新增 `BlurSigma(surface, sigmaPx)`：三趟盒式近似高斯，实测
   10–90% 过渡宽度 2.50–2.67σ（理想 2.563σ）。binUI 三处（`filter: blur` /
   `drop-shadow` / `backdropBlur`）全换过去了。

两条都靠**逐像素比对**发现：`svg-filter` 与 GL 的差异 13.8% → （σ 修正）8.2%
→（0 级卷积修正）**2.446%**，余下差异全部落在文字行（已知的字形采样类）。

- 性能：features 页 900×932，全屏重画 23.6 ms（脏区 120×40 只要 3.56 ms）。
  全屏那 23.6 ms 主要是大面积圆角/渐变的覆盖率栅格，不是字形。要压下去得接
  bin2d 的 `Scene` 缓存（静态组只贴图），那是下一步；不做也行 —— 正常 UI 有脏矩形。

### P5：纹理池 / 预滤波接上 —— ✅ 已完成（0.38.2）

- ~~binUI 的 `binui_mip.h` 是另一套（59 行）——**只留一套**，按"上层不重复实现下层能力"
  删掉 binUI 那份。~~ **已删（0.38.2）**：三份 GPU 后端在 0.38.0 删掉之后，这份只剩一个
  测试在引用它，属于死代码养着一个假靶子；mip 链归 binGPU
  （`BGPU_TEXTURE_MIPMAPS`，0.14.0 起是真链），口径与用例都在 binGPU 的
  `tests/gpu_parity.h`（缩小的棋盘格：1 级偏差 74.11 → 真链 0.00）。
- ~~`image-mip` 用例的 10.7% 差异就是这条：bin2d 走 `sampleSurface` 的盒式预缩小，
  GPU 走 mip 链 + 三线性。P5 要决定留哪一种（**倾向 bin2d**，因为它确定、无驱动差异）。~~
  **取舍结果与当时的倾向相反**：留 **GPU 的 mip 链**。理由是实测 —— binGPU 0.14.0 的真链
  把 `image-mip` 对 `docs/pixels`（当年 GL 4× 的基线）做到 **0.000% / max 0**，
  而盒式预缩小只是近似三线性；"确定、无驱动差异"这个优点被"与基线逐像素相同"压过去了。
  bin2d 的 `sampleSurface` 保留给 CPU 执行体（没有纹理链的那条路仍然需要它）。


## 3. 明确不迁

| 不迁 | 理由 |
|---|---|
| 文本排版 / 字形图集（`binui_glyph_atlas.h` 1169 行、`binui_font_renderer.cpp`） | bin2d 只接收位图，排版是上层的事 |
| 布局 / 事件 / 窗口 / 焦点 | bin2d 不认这些（`make layers` 守住） |
| 编译器（`.bui` → 布局，9332 行） | 与渲染无关 |
| GDI 后端的呈现 | GDI 只剩"贴屏回退"，光栅换成 bin2d 之后它继续只做贴屏 |

## 4. 测量口径的三个坑（今天都踩了一遍）

写进计划是怕下次又踩：

1. **几何必须是设备空间**：`BuildPathFillBandsAA` 要设备坐标 + 设备裁剪框。喂局部坐标
   会得到"圆几乎零条带"的假数字（我第一次量到 2.6 ms，比真值小 3 倍）。
2. **裁剪框给形状自己的 box**，不是整个窗口：传窗口会让条带构建器按窗口宽度分配覆盖率
   累加器，单形状开销放大 10 倍以上（第二次量到 85 ms，比真值大 9 倍）。
3. **两边都要含落像素**：只比"几何构建"会得出 bin2d 慢 4 倍的错误结论（因为 binUI 的
   条带填充成本不在几何里）。

## 5. 时间线（按"每一步都能单独回滚"排）

| 阶段 | 内容 | 出口判据 |
|---|---|---|
| P0 | bin2d 扫描线性能轮（实际做的是"整像素段差分 + 行尾前缀和"） | ✅ `gate-migrate` **0.98×**，125 项断言全过，像素值不变 |
| P1 | 三端像素/哈希基线快照 | ✅ `tests/baseline/manifest.txt` 归档，`make baseline-check` 一键重跑 |
| P2 | `<path>` 填充换到 bin2d + 删旧扫描线 | ✅ 填充完成：18 个 GDI 基准 0 diff、三端 0 diff、快照只动 35 项路径用例；描边留 P2b |
| P3 | 圆角/投影/九宫格 | 三端 0 diff、三端圆角模板删除 |
| P4 | 图层语义收口 | 组透明度像素不变、三份离屏实现退休 |
| P5 | 纹理池/预滤波收敛 | 只剩一套 mip |

P0 已完成，所以现在**可以动 binUI 了**，但起点是 P1（只加门禁与基线快照，不改渲染）。
每进一个阶段前先跑一遍 `make gate-migrate`，掉到 1.2× 以上就回滚那一步 ——
这是本项目自己的规矩（`bin2D/docs/layering.md` §4）。
