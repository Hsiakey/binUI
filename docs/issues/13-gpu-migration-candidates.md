# 13 - 绘制侧还剩哪些该交给 GPU：执行体已换完，剩三件每像素数学 + 一处结构欠账

**状态**：**审计（2026-11-21 复核，binUI 0.38.0 / bin2D 0.22.6 / binGPU 0.15.1 之后）**。
上一版写于 0.34.x（三份 GPU 后端还在的时候），其中"先换执行体"这条**已经落地**
（binUI 0.37.0：13.3× / 12.0×），文中引用的 `binui_gl.cpp` / `binui_vk.cpp` / `binui_d3d11.cpp`
路径**已随 0.38.0 删除**。本版按现状重写。
**类型**：performance / 架构 · **严重度**：中（不阻塞功能，决定下一轮优化方向）
**影响面**：bin2d 的 GPU 执行体（`src/gpu_exec.cpp`、`canvas_gpu.h`）、binUI 的
`binui_bin2d.cpp`（字形 / 滤镜 / 阴影 / 线）、binGPU 的离屏层与质量档。

> 一句话：**"把绘制交给 GPU"这件事在 0.37.0 已经做完了**（9.07 → 0.68 ms，13.3×），
> binUI 现在一帧的 CPU 绘制只剩 **37.6 / 19.9 µs**。剩下的不是"再搬任务"，而是三件事：
> ① ~~一处结构欠账（`svg-filter` 那类重滤镜页 11.4 ms/帧）~~ —— **已修（bin2D 0.22.8：11.30 → 3.38 ms）**，而根因**不是这里猜的"整窗层纹理 + 2r+1 次高斯"**，是模糊链池 `kMaxContentBlur = 12` 被一页 14 条链顶撞后每帧重建渲染目标（详见本文 §复核）；
> ② **三件每像素数学**（颜色矩阵滤镜、SDF 阴影、SDF 线段）；
> ③ **AA 口径**（CPU 扫描线 vs GPU 解析 SDF，弧差 41 级；统一会动 `pixels-bin2d` 基线，要拍板）。

---

## 1. 现状：一帧里 CPU 还剩多少（2026-11-21 复测）

`build-mingw/bin/draw_bench.exe`（只遍历、只计数、不上屏；2026-11-21 00:26 构建）：

```sh
build-mingw\bin\draw_bench.exe examples\buttonshow\main.html --frames 200
build-mingw\bin\draw_bench.exe examples\svgdemo\main.html    --frames 200
```

| 页面 | 遍历 µs/帧（p50 / p95） | 原语 | ≈四边形 | 顶点数据 |
|---|---|---|---|---|
| buttonshow（66 节点，1020×684） | **37.6**（37.0 / 39.0） | 339 | 2913 | 341.4 KB |
| svgdemo（65 节点，960×560） | **19.9**（20.0 / 21.0） | 4404 | 4997 | 585.6 KB |

对照整帧（binUI 0.37.0 / bin2D 0.22.4，vsync 关，60 帧中位）：

| 页面 | CPU 光栅（0.36.x） | bin2d GPU 执行体（1×） | GPU + 4× MSAA |
|---|---|---|---|
| buttonshow 1020×684 | 9.07 ms | **0.68 ms**（13.3×） | 0.94 ms |
| appshow 1280×800 | 8.88 ms | **0.74 ms**（12.0×） | 1.15 ms |

**读数**：GPU 路径上 CPU 一帧只花 20–38 µs（不到整帧的 5%），**再搬"任务"省的是微秒**。
14× 的收益已经在 0.37.0 拿到，它来自"换执行体"，不是"多搬几件事"。

## 2. 已经完成的（上一版列的候选，状态更新）

| 上一版候选 | 现状 | 证据 |
|---|---|---|
| ① 换执行体（bin2d Canvas → binGPU 执行体） | **已完成（binUI 0.37.0 / bin2D 0.22.2）** | 9.07 → 0.68 ms（13.3×），与 binUI 自带 `binui_gpu` 的 0.64 ms 同量级 |
| ② 圆角 / 描边模板 → SDF | **已完成（bin2d 0.21.0 + binGPU ABI 4）** | `BGPU_CAP_SHAPE` 已是**四端能力**（CPU / GL / VK / D3D11 都实现）；bin2d 有 cap 时直接发 `draw_shapes`，不再生成/上传覆盖率模板 |
| ③ VK 的真 mip 链 | **已完成（binGPU 0.14.0，ABI 5）** | `image-mip` 页对 `docs/pixels` 从 37% 收到 0.118%，0.37.2 起 0 |
| ④ 三份老 GPU 后端各自持有一套 AA | **已完成（0.38.0）** | 26 个文件、-15,121 行删掉，`binUI → bin2D → binGPU` 一条路 |
| ⑤ 斜线 `fillLine` 的 CPU 条带 | **GPU 路径已绕开** | `binui_bin2d.cpp:1299` GPU 模式换成"平移 + 旋转 + `fillRect`"一个四边形；CPU 路径仍走 `strokePath`（像素权威不动） |

## 3. 还剩什么（按收益排）

### 3.1 结构欠账：重滤镜页的离屏层按"整窗"分配（**现在最大的一处**）

`svg-filter` 660×470 在窗口路径上 **~11.4 ms/帧**（被删的 `binui_gpu` 约 2.9 ms，同页 CPU 路径
13.9 ms —— 出自 bin2D 0.22.5 / binUI 0.37.4）。原因不是"没交给 GPU"，而是 GPU 上做得低效：

- 内容层纹理是**整窗尺寸**且带设备级 4× MSAA，每层一次全尺寸 clear（≈5 MB 写 + 1.2 MB resolve），
  一页 10 层 + 14 条阴影 ≈ **60 次全尺寸清屏**；
- 分离高斯是"2r+1 个整矩形累加"，4× 再乘 4。

两条解法（bin2D 0.22.5 记的）：
1. **层纹理按层框尺寸分配**（等于把 CPU 的 `originX_` 搬到 GPU；顺手修 CPU `pushLayer` 嵌套原点
   不累积那个 bug）；
2. **binGPU 开放逐目标采样数**（层用 1×，最终画布用 4×）。

这不是"搬任务"，是"把已经搬过去的那条路修顺"。**优先级最高**：它决定"重滤镜页能不能留在
GPU 路径上"。在它修好之前，`svg-filter` 这类页在 4× 下比老实现慢 4 倍。

### 3.2 颜色矩阵滤镜：仍在 CPU 逐像素

`binui_bin2d.cpp:689` 附近的 `ApplyColorMatrix(&surface, matrix)`：图片**解码后**在 CPU 上
逐像素乘 4×5 矩阵，再整张上传。静态图有 `filterKey` 缓存（付一次），**视频帧每帧都付**。

搬法：片元着色器里对采样结果做矩阵（`filter.h` 的 `ColorMatrix` 直接进 `BgpuVertex` 的
扩展或管线状态）。收益：大图/视频帧的 CPU 逐像素归零；风险：像素由"CPU 8 位乘法"换成
"GPU 浮点 + 舍入"，需要 `pixels-bin2d-gpu` 容差确认（现有口径 0.5% / 单通道 8）。

### 3.3 `box-shadow`：环还是环

`binui_draw_nodes.h:881/896` 的 `BuildShadowRings` 每帧在 CPU 生成 N 个同心环，
每个环调一次 `fillRounded`（现在走 SDF 四边形）。GPU 上**overdraw 仍在**：buttonshow 的
5 层环 = 5 次全块覆盖，占 fillRounded 像素 **63.8%**（issue 11）。

搬法：圆角盒 SDF + `erfc` 衰减，一片元一次画完（解析解，和 `0.5·erfc(d/σ√2)` 那条现成公式同源）。
收益：GPU overdraw 与 CPU 环生成都降；**但会改像素**（issue 11 已裁定"环带化/解析阴影必变像素"），
要 master 点头。

### 3.4 `fillLine`：CPU 条带已绕开，剩 AA 口径

GPU 路径现在是"旋转矩形"（1 个四边形），CPU 路径是 `strokePath` 的覆盖率条带。
两条路的 AA 不同：旋转矩形靠 4× MSAA 的采样点，CPU 靠 8 子采样扫描线 —— 与 0.16.18 记的
"仿射下位图旋转"同类问题。真要统一，**capsule SDF**（点到线段距离场）是正解：
`cov = clamp(0.5 - d)`，一片元、1px 解析过渡，与圆角同一套数学。收益是质量与口径，不是性能。

### 3.5 文字阴影模糊：CPU 可分离高斯

`binui_glyph_atlas.h` 里文字阴影的模糊图集是 CPU 算的（按"字号/字重/斜体/面/模糊半径"缓存，
新组合才重算）。GPU 侧已有 blur 链路（bin2d 的内容层 + 降采样），搬法是"字形覆盖率图集 +
GPU 模糊 pass"。优先级低：缓存命中的常规页面付 0。

### 3.6 顶点上传 / 脏节点

仍是每帧全量 341–586 KB（41 MB/s @120Hz）。`draw_nodes` 已有脏矩形与节点级缓存，
但顶点流没有"只传脏节点"。这不是 GPU 任务，是 CPU 侧优化，顺手收。

## 4. 明确不该搬的

- **路径填充条带**（`BuildPathFillBands` → bin2d 的 `Path`）：GPU 路径光栅化要
  stencil-and-cover 或 compute 扫描线，跨平台与像素可证性都变差；现状由 bin2d 统一覆盖率，
  而且 binUI 的 `<path>` 本来就少。
- **文字光栅化**：GPU 不做字形栅格；可选的是 MSDF（质量优化，不是性能），见
  [12-font-layer-ownership.md](12-font-layer-ownership.md)。
- **遍历 / 布局 / 剔除 / 命中测试**：永远 CPU，实测 20–38 µs。

## 5. AA 口径：现在只剩两条路的差，且要 master 拍板

| 路径 | AA 做法 |
|---|---|
| bin2d CPU 光栅（`pixels-bin2d` 的权威，0 容差） | 8 子采样扫描线 |
| bin2d GPU 执行体（`pixels-bin2d-gpu`） | 解析 SDF + 4× MSAA（VK）/ 1×（其余） |

弧边最多差 **41 个电平**（`docs/backend_layering.md` §5 记的数），现有门禁靠
`0.5% / 单通道 8` + `PIXEL2D_GPU_KNOWN := text-shadow-multi` 兜着。要真正收口只有一条路：
**让 CPU 光栅也用同一条 SDF 公式**（会改 `tests/pixels-bin2d` 全部基线，属于"改像素"，
按 0.34.4 的规矩要单独一版 + master 点头）。在那之前，**不许再引入第三套 AA 定义**。

另外，bin2D 0.22.5 发现的 CPU `Canvas::pushLayer` **嵌套原点不累积**（外层 (200,100) 里套
内层 (212,112) ⇒ 内层 0 个非透明像素）也归在这一块：设备路径没有这个问题，所以
`filter-blur.html` 那种"滤镜套滤镜"页 GPU / CPU 差 2.6%。修它会动 CPU 像素，和 AA 统一可以
放同一版。

## 6. 顺序（建议）

| 轮次 | 做什么 | 为什么先做 |
|---|---|---|
| 1 | ~~层纹理按层框分配~~ ✅ **已做（bin2D 0.22.8）** | 实测层纹理按层框分配只值 0.03 ms；真正修掉 11.4 ms 的是模糊链池上限 12→32。现在 `svg-filter` 3.38 ms（主路径 buttonshow 0.82 / appshow 1.14 ms） |
| 2 | 颜色矩阵滤镜搬片元着色器 | 纯赚、不依赖像素拍板 |
| 3 | AA 口径统一（CPU 光栅换 SDF）+ CPU `pushLayer` 嵌套 bug | 改像素，需要单独一版与 master 点头 |
| 4 | SDF 阴影、capsule SDF 线段 | 依赖第 3 步的口径 |
| 5 | 文字阴影 GPU 模糊 / 脏节点顶点 | 收尾 |

## 7. 验收线（按现在的门禁口径）

1. `make pixels-bin2d`（CPU 权威，**容差 0**）—— 不搬则必须逐像素不动；
2. `make pixels-bin2d-gpu`（GPU 唯一门禁，容差 0.5% / 单通道 8，`PIXEL2D_GPU_KNOWN = text-shadow-multi`）
   —— 搬了要给出"新的 known 名单 + 理由"，不许悄悄放宽；
3. `make partial`（脏矩形重绘对拍，**0 容差**）—— 任何图层/裁剪相关的搬动都要过这条；
4. 性能给"搬前/搬后 + 同负载"的数字（`probe_cost` 或 `bin2d_shot --frames`），不给 fps 口号；
5. `draw_bench --hash` 允许变（换了执行体），但要记录原语数变化（条带 → 四边形这类收益）。

## 8. 复现

```sh
# CPU 遍历（只计数、不上屏；不含模板/字形/上传）
build-mingw\bin\draw_bench.exe examples\buttonshow\main.html --frames 200
build-mingw\bin\draw_bench.exe examples\svgdemo\main.html    --frames 200

# 整帧（离屏；GPU 执行体 vs CPU 光栅）
build-mingw\bin\bin2d_shot.exe examples\buttonshow\main.html  out.png
set BINUI2D_GPU=1 && build-mingw\bin\bin2d_shot.exe examples\buttonshow\main.html out-gpu.png
```

口径边界：`draw_bench` 在遍历期会数 `fillLine` 的真实条带数（CPU 口径），但 **不含**后端侧
圆角模板生成（新路径已无模板）、字形光栅、图片解码、顶点上传与 GPU 侧填充 —— 那几项由缓存
与设备决定，不在这个数字里。
