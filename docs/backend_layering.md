# 后端分层：谁定功能、谁定怎么画

> 2026-11-20 · master 的口径：
> 「让 binGPU 决定吧，`binUI > bin2D/bin3D > binGPU: vk(GPU/CPU)/gl(GPU/CPU)/D3D...`。
> 应该是 binGPU 来决定怎么绘制、用什么绘制来保证绘制性能和质量吧？binUI、bin2D/3D 决定的是功能。」

## 1. 正确分工

| 层 | 只决定 | 不该出现的东西 |
|---|---|---|
| **binUI** | 功能：节点树 / CSS / 布局 / 事件 / 语义原语（`fillRounded`、`strokeRounded`、`drawText`、`pushLayer`…） | **一行图形 API**；也不该有"用哪个后端画"的多个实现 |
| **bin2d / bin3d** | 2D/3D 的**绘制语义**：几何（圆角、路径、覆盖率口径）、材质、混合、文字排版、图层与模糊的语义 | 不该自己挑设备，也不该直接拿 GL/VK/D3D |
| **binGPU** | **怎么画、用什么画**：API（VK / GL / D3D11）× 执行体（**GPU / CPU**）× 质量控制（MSAA/采样/AA 方式），以及"这笔活 GPU 不划算就回落到 CPU" | 不该了解 UI 语义 |

也就是说：`vk(GPU/CPU)` / `gl(GPU/CPU)` / `d3d(GPU/CPU)` 是 **binGPU 内部的矩阵**，
不是 binUI 的后端清单。

## 2. 我 0.33.0 做错了什么（记在这里免得再犯）

0.33.0 我加了 `BinUIAuto`：在 binUI 层按 `VK > GL > D3D11 > CPU` 挑一个**binUI 自己的
后端实现**。这条路快（见 §4），但**层次是错的**：

- 它把"怎么画"重新塞回了 binUI —— 那四份 `IBinUIGfx` 实现（`binui_vk` / `binui_gl` /
  `binui_d3d11` / `binui_bin2d`）**各自都是一套完整的 2D 光栅**，正是 0.29.0 那条迁移要删的东西。
- 于是"像素权威"重新分裂成四份；`BINUI_GPU=` 这个环境变量也就成了 binUI 在代替 binGPU 做决定。

结论：**0.33.0 的 `BinUIAuto` 是过渡接法，不是目标架构**，最终要删掉；
它的性能必须由 binGPU 内部的"GPU 执行模式"提供，而不是由 binUI 换后端实现提供。

## 3. 目标结构（三步，每步单独可验收）

### 第一步：binGPU 长出一层"2D 图元 + 执行模式"

新增两件东西，都只属于 binGPU：

1. **执行计划** `bingpu_render_plan()`：返回「用哪个 API + GPU 还是 CPU + 质量档」。
   决策依据是设备能力（有没有 GPU、驱动能不能建上下文、MSAA 上限）与"这笔活在哪更快"
   （例如超大填充与高斯模糊在 GPU、几十个小圆角在 CPU 可能更划算）。
   这就是 master 说的"binGPU 决定怎么绘制、用什么绘制来保证性能和质量"的落点。
2. **2D 图元**（不是 UI 语义，是几何基元）：圆角矩形/边框/外阴影、四边形批处理、
   线性渐变四边形、纹理四边形。**用同一套解析式（SDF）定义**，GPU 用着色器求值、
   CPU 用同一个公式逐像素求值 —— 于是"换执行体不换像素"，质量由同一份数学保证，
   而不是靠两套光栅去对齐。

### 第二步：bin2d 的 Canvas 变成"只描述语义"，执行交给 binGPU

`Canvas::fillRounded(...)` 等翻译成 binGPU 的图元调用；CPU 覆盖光栅保留，但**降级成
binGPU 的 CPU 执行模式**（bin2d 不再自己决定在哪算）。文字：bin2d 的字形图集直接产出
纹理（binGPU 已有纹理上传），不再走"CPU 逐个字形合成"。
这一步做完，bin2d 的像素口径与 binGPU 的图元口径合一。
**文字归属的拆分与迁移顺序见 [issues/12-font-layer-ownership.md](issues/12-font-layer-ownership.md)**
（bin2d 侧三处文档现在和本条相反，先统一再动代码）。

### 第三步：删掉 binUI 的三份 GPU 后端

`binui_vk` / `binui_gl` / `binui_d3d11` 删除，`BinUIAuto` 一起删。
binUI 只剩一条路：`binUI ──► bin2d（+bin3d）──► binGPU ──► VK/GL/D3D11 × GPU/CPU`。
这才是 0.29.0 那条迁移的真正收尾。

## 4. 过渡期为什么先留着 0.33.0

同一页实测（1020×684，窗口置顶 + High 优先级，200 帧）：

| 实现 | 帧耗时 | fps |
|---|---|---|
| Vulkan（binUI 自己的 VK 后端） | 8.50 ms | 112 |
| D3D11 | 0.75 ms | 113 |
| OpenGL | 1.48 ms | 105 |
| bin2d（CPU 覆盖光栅） | 12.11 ms | 56 |

差的 12~16 倍是真钱。所以在把"GPU 执行模式"做进 binGPU 之前，0.33.0 作为**过渡基线**
留着（它会自己后面被第一步+第二步取代）。**它不是一个我要维护的第二条永久路径。**

## 5. 进展（0.35.0，2026-11-20）

| 步 | 状态 | 具体做了什么 |
|---|---|---|
| 1. binGPU 长出 2D 图元 + GPU/CPU 两种执行体 | **已完成（圆角这一半）** | `bingpu_render_plan()`（0.8.0）给 backend/exec/msaa；`bingpu_pace_*`（0.9.0）收"什么时候画"。**圆角的 AA 数学已经搬进设备层**：binGPU 0.12.0 / ABI 4 新增 `BGPU_VERTEX_SHAPE` + `BgpuShapeVertex`（36 字节）+ `bingpu_cmd_draw_shapes()`，片元里算有符号距离场（`cov = clamp(0.5 - d, 0, 1)`），四个后端（CPU 参考 / GL / VK / D3D11）都实现、端间最大差 1。旧的两条九宫格入口（0.10.0 / 0.11.0）留着，只给不支持 `BGPU_CAP_SHAPE` 的后端兜底。执行体那一轴仍然只有 GPU：binGPU 的 CPU 后端是参考实现，实测比 bin2d 的光栅慢 14× |
| 2. bin2d `Canvas` 把执行交给 binGPU | **已完成（圆角这一半）** | bin2D 0.21.0：有 `BGPU_CAP_SHAPE` 时 `drawRoundedRect(s)/drawRoundedStroke/drawRoundedGradient` 直接发 `draw_shapes`，**不再生成也不上传任何覆盖率模板**；没有这项能力就原路退回 0.20.0 的模板那条路。`DisplayList`（0.17.0 → 0.19.0）仍然覆盖矩形 / 圆角 / 圆角描边 / 线性渐变 / 圆角渐变 / 染色贴图 / 矩形裁剪。**但 `Canvas` 本身还是纯 CPU 光栅**：它的圆角目前是 8 子采样扫描线，与 GPU 的解析解在弧上最多差 41 级 —— 下一步应当把 CPU 光栅也换成同一条解析式（会动所有像素基线，单独一版做） |
| 2b. **binUI 自己的三端** | **已完成（圆角这一半）** | 0.35.0：`binui_gl` / `binui_vk` / `binui_d3d11` 的 `fillRounded` / `strokeRounded` 换成同一段 SDF，三条顶点流各自 36 字节、**不再采样圆角模板**。于是三端的跨后端像素门禁从 `PIXEL_VK_TOL 1.5%` / `PIXEL_D3D_TOL 3%` 收到 **0%**（单通道容差 128 → 8）。这一条是迫不得已的先手：三端各自实现，只改一端会让"后端互拍"门禁永久红 |
| 3. 删掉 binui_vk / binui_gl / binui_d3d11 与 BinUIAuto | 未开始 | 三份实现仍在；`BinUIAuto` 仍然是选后端的那层薄壳。仍然卡在第 2 步（`Canvas` 与 binUI 的绘制入口还没改成"先记列表再执行"） |

**卡点只剩一个了**：把 binUI 换成"先记列表 → 交给执行体"会让圆角弧边整体挪
（`docs/pixels` 的 GL 基准已经换成解析 SDF、`tests/pixels-bin2d` 还是扫描线，两者差 41 个电平）。
**0.35.0 之后 binUI 的三端已经全是解析 SDF 了**，所以这一步的前置条件比之前好：剩下的分歧只在
bin2d 的 CPU 光栅那一边。

这一轮唯一动的是**节拍**：它原本也在错的那一层（外壳自己算网格 + 滞回 + 猜 present
有没有在等垂直同步），现在按同一套"判据在设备层"的道理挪进了 binGPU。
可见的后果是 master 直接能感觉到的：GPU 路径从 55/31/100 fps 变成
**unlimited 下 2116/1893/702 fps、standard 下 114/116/108 fps**。

第 3 步仍然卡在第 1 步：bin2d 的 GPU 执行模式没做完之前删掉那三份实现，等于把
GPU 绘制能力直接扔掉（CPU 光栅 9 ms/帧装不下一个刷新周期）。
### 圆角 AA 已经统一了（0.34.4，master 2026-11-20 定"用质量最好的那一套"）

| 路径 | AA 做法 | 谁在用 |
|---|---|---|
| `Canvas::fillRounded` | 8 子采样扫描线 | binUI 的 bin2d 后端、离屏出图 —— **唯一的一份** |
| `RasterizeRoundedTileCoverage` | ~~解析 SDF~~ → **调上面那份扫描线**（0.20.0） | binUI 的 GL / VK / D3D11 三端 |

以前同一个圆角矩形的弧边差到 **48 个电平**，一直没被发现，是因为两个 golden 是分开的
（`docs/pixels` 对 GL、`tests/pixels-bin2d` 对 bin2d 后端），**"逐像素相同"从来没跨过
实现比**。0.20.0 把模板内容换成 CPU 光栅自己的扫描线覆盖率之后：

- bin2d 的 `test_raster` 钉住"角片与 `Canvas::fillRounded` 逐像素相同（r=1..40）"，**max 0**；
- bin2d 的 `test_gpu_bridge` 钉住"九宫格 vs 直填"，四端 **max 0**；
- binUI 侧 `make pixels-bin2d` 一个像素没动（语义没变），而 GL 的基准图按新实现重生成了。

**仍未统一的一处**：盒子落在小数像素上时，位图模板（整像素相位）与连续相位的扫描线
仍会差 —— 实测 `box(8.5,8.5)` 差 416 像素、最大 119。这不是"两套定义"，是模板这个
表示方式的固有代价，要消掉得让模板带相位（每 1/8 像素一张，或用 SDF+导数着色器）。