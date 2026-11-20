# 13 - 绘制侧还剩哪些该交给 GPU：先换执行体，再搬三件每像素数学

**状态**：**审计结论 + 提案**（部分已排期：`docs/backend_layering.md` 第 2/3 步）。
**类型**：performance / 架构 · **严重度**：中（不阻塞，但决定下一轮优化往哪使劲）
**影响面**：`binui_bin2d.cpp`（整条 CPU 光栅）、三份 GPU 后端的原语实现、`binui_filter.h`、
`binui_paint.h` 的覆盖率几何（条带 / 环 / 模板）。
**测量环境**：Windows 11 + MinGW-w64，本机 `build-mingw`（2026-11-20 构建），
`draw_bench` 200 帧、`--frames` 默认预热；GPU 数字取自 `docs/backend_layering.md` §4 同页实测。

> 一句话：**GPU 路径上，绘制侧的 CPU 任务已经只剩 ~40 µs/帧，没有值得再搬的任务了。**
> 性能大头是"把 bin2d 的整条 CPU 光栅换成 binGPU 执行体"（12.11 → 0.75 ms，16×），
> 不是再搬某个环节。真正还该搬进片元着色器的只有三件**每像素数学**：颜色矩阵滤镜、
> SDF 阴影、SDF 线段；圆角模板那条 binGPU 已经做好了，只差接线。

---

## 1. 实测：CPU 绘制遍历已经很小

`build-mingw/bin/draw_bench.exe`（只遍历、只计数、不上屏；不含后端侧模板生成/字形光栅/顶点上传）：

```sh
build-mingw\bin\draw_bench.exe examples\buttonshow\main.html --frames 200
build-mingw\bin\draw_bench.exe examples\svgdemo\main.html    --frames 200
```

| 页面 | 遍历 µs/帧（p50 / p95） | 原语 | ≈四边形 | 顶点数据 |
|---|---|---|---|---|
| buttonshow（66 节点，1020×684） | **36.3**（36.0 / 37.0） | 339 | 2913 | 341.4 KB |
| svgdemo（65 节点，960×560） | **19.2**（19.0 / 20.0） | 4404 | 4997 | 585.6 KB |

对照同一页的**整帧**耗时（`docs/backend_layering.md` §4，1020×684、窗口置顶、200 帧）：

| 执行体 | 帧耗时 | fps |
|---|---|---|
| D3D11（GPU） | **0.75 ms** | 113 |
| OpenGL（GPU） | 1.48 ms | 105 |
| Vulkan（GPU，present 在等刷新） | 8.50 ms | 112 |
| **bin2d（CPU 光栅）** | **12.11 ms** | 56 |

**读数**：GPU 路径上一帧只要 0.5–1.0 ms（binGPU CHANGELOG 0.9.0：VK vsync=0 时 966–991 fps），
而绘制侧 CPU 只花 36 µs —— 连 1% 都不到。**再往 GPU 搬"任务"，省的是微秒。**

## 2. 真正的 16× 不在任务上，在执行体上

bin2d 后端是**整条覆盖率光栅跑在 CPU 上**（`binui_bin2d.cpp` → `bin2d::Canvas`），
它不是"少搬了一个任务"，是"整条路没上 GPU"。这正是 `docs/backend_layering.md` 第 2/3 步要收的：

> 第 2 步：bin2d 的 `Canvas` 变成"只描述语义"，执行交给 binGPU；
> 第 3 步：删掉 binui_vk / binui_gl / binui_d3d11 与 `BinUIAuto`。

**这一步是当前唯一量级正确的性能动作**：12.11 → 0.75 ms。其余都是它的配套。

## 3. 还该搬进片元着色器的：只有"每像素数学"

按收益排（只在 GPU 路径上评估；CPU 光栅路径另算）：

| # | 任务 | 现在在哪 | 搬法 | 预期收益 | 优先级 |
|---|---|---|---|---|---|
| 1 | **颜色矩阵滤镜**（`grayscale`/`sepia`/`saturate`/`hue-rotate`/`invert`/`contrast`） | **解码期 CPU 逐像素**（v26，`binui_filter.h`） | 片元着色器里对采样结果乘 4×5 矩阵 | 大图/视频帧**每帧**都在 CPU 上过一遍；搬完这条成本归零 | **高** |
| 2 | **`box-shadow` 环** | CPU 每帧生成 N 个同心环几何（`binui_paint.h:850 BuildShadowRings`），每个环一次全块填充；buttonshow 里占 **63.8%** 填充像素（issue 11） | 圆角盒 SDF + `erfc` 衰减，一片元一次画完 | GPU 路径：省 overdraw 与 5× 四边形；**CPU 光栅路径**：fillRounded 6.56 → 2 ms 量级 | 高（先修 §5 的口径） |
| 3 | **斜线 `fillLine`** | CPU 覆盖率条带（`binui_gl.cpp:1687` / `binui_vk.cpp:4173` / `binui_d3d11.cpp:1891`），svgdemo 约 6985 条带/帧 | capsule SDF（点到线段距离场） | 条带 → 四边形，顶点少一个量级；svgdemo 的 0.136 ms 回落到 ~0.08 ms | 中（被 MSAA 相位挡着） |
| 4 | **圆角 / 描边模板** | CPU 生成（`binui_gl.cpp:1034 BuildRoundedTileRGBA` / `BuildRoundedRingRGBA`），每半径一次 + 纹理上传 | SDF（binGPU 0.12.0 已有 `BGPU_VERTEX_SHAPE`，GL+CPU 参考实现完成，VK/D3D11 待补） | 去模板缓存/纹理/半径键；**连续相位**修掉 `box(8.5,8.5)` 差 416 px | 中（质量 > 性能） |
| 5 | **文字阴影模糊** | CPU 可分离高斯 + 独立模糊图集（`binui_glyph_atlas.h`） | 复用 GPU 的 blur 链路（`binui_gl.cpp:389` 的 `BlurSet`） | 大字号 / 多层阴影时明显；单层小字几乎无感 | 中低 |
| 6 | **VK 的 mip 链** | CPU 2×2 盒式（`binui_vk.cpp:36` 引 `binui_mip.h`；GL/D3D11 是硬件生成） | `vkCmdBlitImage` 逐级 / compute | 一次性上传成本，非每帧 | 低 |
| 7 | **顶点上传** | 每帧全量 341–586 KB（41 MB/s @120Hz） | 持久缓冲 + 只传脏节点 | CPU 带宽；不是 GPU 任务，顺手收 | 低 |

## 4. 明确不该搬的

- **路径填充条带**（`BuildPathFillBands`）：GPU 路径光栅化要 stencil-and-cover 或 compute 扫描线，
  跨平台与像素可证性都变差，收益不匹配；现状由 bin2d 统一覆盖率已经够快。
- **文字光栅化**：GPU 不做字形栅格化；唯一值得的是 MSDF（解决 0.16.18 记的"仿射下位图旋转发软"），
  那是质量优化，不是性能。
- **遍历 / 布局 / 剔除 / 命中测试**：永远 CPU，实测 19–36 µs。

## 5. 前置：AA 定义必须先统一，否则每搬一条就多一套口径

上面 #2/#3/#4 本质都是"把覆盖率数学搬进片元着色器"。但 binUI 现在的像素权威是
**bin2d 的 8 子采样扫描线**（0.34.4 刚把三端模板统一到它），而 SDF 是**另一套定义**
（弧边差 48 电平）。所以顺序必须是：

1. **定权威**：按 `docs/backend_layering.md` §3 的原话 —— 同一套解析式，GPU 着色器求值、
   CPU 同一公式逐像素求值 —— **SDF 当唯一权威**，bin2d 的 `Canvas::fillRounded` 也改成 SDF 求值；
2. 再补 binGPU 的 VK/D3D11 SHAPE 实现（glslc / fxc 构建期生成、产物入库，见 12 号文档同一节的构建规矩）；
3. 最后逐条接线。

**不许出现第四套 AA 定义**：现状已经是"扫描线 / 模板(扫描线) / SDF"三套并存，再搬一条就分叉。

## 6. 顺序（建议）

| 轮次 | 做什么 | 为什么先做 |
|---|---|---|
| 0 | 换执行体：layering 第 2 步（`Canvas` → binGPU） | 16×，其余都是零头 |
| 1 | 颜色矩阵滤镜搬片元着色器 | 纯赚、零像素风险、不依赖 §5 |
| 2 | 统一 AA 口径（SDF 当权威）+ 补 binGPU VK/D3D11 SHAPE | 后面三条的闸门 |
| 3 | SDF 阴影、SDF 线段接线 | 填充率与条带数 |
| 4 | 文字阴影模糊 / VK mip / 脏节点顶点 | 收尾 |

## 7. 验收线

1. 每搬一条，`make pixels` / `pixels-vk` / `pixels-d3d11` / `pixels-bin2d` 四套基准要么 **0 差**，
   要么明确"预期变、已重生基准"并给出变在哪（沿用 0.34.4 的做法）；
2. 性能给"搬前/搬后 + 同负载"的数字，不给 fps 口号：
   buttonshow 在 GPU 路径上给 CPU 侧 ms/帧与 GPU 填充率两项；
3. `draw_bench --hash` 的**原语流哈希**允许变（换了执行体），但必须记录变了多少条，
   否则"少发了几千个条带"这种收益看不出来。

## 8. 复现（本机的两组数）

```sh
# CPU 遍历（只计数、不上屏；不含模板/字形/上传）
build-mingw\bin\draw_bench.exe examples\buttonshow\main.html --frames 200
build-mingw\bin\draw_bench.exe examples\svgdemo\main.html    --frames 200

# 整帧（四执行体对照，见 docs/backend_layering.md §4）
build-mingw\bin\bin2d_window.exe examples\buttonshow\main.html --frames 200
```

`draw_bench` 的口径边界（重要，别当成"后端绘制成本"）：它在遍历期把 `fillLine` 的**真实条带数**
也数了（所以 svgdemo 的 4404 个原语里有 4365 个是条带），但 **不含**后端侧的圆角模板生成、
字形光栅、图片解码与顶点上传 —— 那几项由缓存决定，不在这个数字里。
