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

### 第三步：删掉 binUI 的三份 GPU 后端（**已完成 2026-11-20**）

`binui_vk` / `binui_gl` / `binui_d3d11` 删除，`binui_gpu`（设备层那份）与 `BinUIAuto` 一起删。
binUI 只剩一条路：`binUI ──► bin2d（+bin3d）──► binGPU ──► VK/GL/D3D11 × GPU/CPU`。
这才是 0.29.0 那条迁移的真正收尾。落地清单见 §5 的 3c 行。

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

绘制侧"还剩哪些该交 GPU"的实测与候选清单见
[issues/13-gpu-migration-candidates.md](issues/13-gpu-migration-candidates.md)
（结论：GPU 路径上 CPU 绘制只剩 36 / 19 µs 一帧，再搬任务是省微秒；量级正确的动作就是本节的第 2/3 步）。

## 5. 进展（0.36.0，2026-11-20）

| 步 | 状态 | 具体做了什么 |
|---|---|---|
| 1. binGPU 长出 2D 图元 + GPU/CPU 两种执行体 | **已完成（圆角这一半）** | `bingpu_render_plan()`（0.8.0）给 backend/exec/msaa；`bingpu_pace_*`（0.9.0）收"什么时候画"。**圆角的 AA 数学已经搬进设备层**：binGPU 0.12.0 / ABI 4 新增 `BGPU_VERTEX_SHAPE` + `BgpuShapeVertex`（36 字节）+ `bingpu_cmd_draw_shapes()`，片元里算有符号距离场（`cov = clamp(0.5 - d, 0, 1)`），四个后端（CPU 参考 / GL / VK / D3D11）都实现、端间最大差 1。旧的两条九宫格入口（0.10.0 / 0.11.0）留着，只给不支持 `BGPU_CAP_SHAPE` 的后端兜底。执行体那一轴仍然只有 GPU：binGPU 的 CPU 后端是参考实现，实测比 bin2d 的光栅慢 14× |
| 2. bin2d `Canvas` 把执行交给 binGPU | **已完成（圆角这一半）** | bin2D 0.21.0：有 `BGPU_CAP_SHAPE` 时 `drawRoundedRect(s)/drawRoundedStroke/drawRoundedGradient` 直接发 `draw_shapes`，**不再生成也不上传任何覆盖率模板**；没有这项能力就原路退回 0.20.0 的模板那条路。`DisplayList`（0.17.0 → 0.19.0）仍然覆盖矩形 / 圆角 / 圆角描边 / 线性渐变 / 圆角渐变 / 染色贴图 / 矩形裁剪。**但 `Canvas` 本身还是纯 CPU 光栅**：它的圆角目前是 8 子采样扫描线，与 GPU 的解析解在弧上最多差 41 级 —— 下一步应当把 CPU 光栅也换成同一条解析式（会动所有像素基线，单独一版做） |
| 2b. binUI 自己的三端 | 已完成 → **已被 3a 取代** | 0.35.0：`binui_gl` / `binui_vk` / `binui_d3d11` 的 `fillRounded` / `strokeRounded` 换成同一段 SDF，三条顶点流各自 36 字节、**不再采样圆角模板**。三端的跨后端像素门禁从 `PIXEL_VK_TOL 1.5%` / `PIXEL_D3D_TOL 3%` 收到 **0%**（单通道容差 128 → 8）。这一条是迫不得已的先手：三端各自实现，只改一端会让"后端互拍"门禁永久红。**代价是同一段 SDF 在 binUI 里躺了 3 份**（全栈 7 份拷贝），master 当场指出落错了层 —— 于是有了 3a |
| **3a. binUI 的设备层换成 binGPU（GL 先行）** | **已完成（0.36.0）** | 新增 `src/backends/binui_gpu.{h,cpp}`：`BinUIGpu` 实现同一份 `IBinUIGfx`，但**不建自己的设备** —— 设备/交换链/管线/采样器/命令缓冲/节拍全部来自 binGPU（`bingpu_device_create` / `bingpu_swapchain_create` / `bingpu_cmd_*` / `bingpu_pace_*`）。绘制目标是一张窗口大小的持久 `canvas` 纹理，`present()` 时贴到交换链目标上；于是"抓背景做毛玻璃"与"图层合成"都退化成"把纹理的某块画成四边形"，**不需要给 binGPU 加任何拷贝命令**。结果：binUI 这条路径上**一份 AA 数学都没有** —— 圆角 / 描边 / 圆角渐变全走 `bingpu_cmd_draw_shapes`。两处口径对齐（顶点色必须是直通色、纹理必须声明 `BGPU_TEXTURE_PREMULTIPLIED`）之后，18 个像素基准里 **16 个与老 GL 后端逐像素相同**，`appshow` 整页（1280×800）0 差异、最大通道差 7 |
| 3b. 同一份实现搬到 VK / D3D11 | **已完成（binGPU 0.13.0 / binUI 0.36.2）** | `binui_gpu.cpp` 里没有一行为某个 API 特判（命令流完全相同），差的只是 binGPU 各后端自己。VK 后端在"一帧里多次 begin/end render、把刚当渲染目标的纹理再当采样源"这条路上还不对：`circle` / `opacity` 0 差异，但 `svg-filter` 87%、`appshow` 13.7%。修在 binGPU，修好后 3a 里那颗"先钉 GL"的钉子去掉即可 |
| 3c. 删掉 binui_vk / binui_gl / binui_d3d11 / binui_gpu 与 BinUIAuto | **已完成（2026-11-20）** | 前置两件（binGPU 的 **MSAA** 与**真 mip 链**）在 binGPU 0.15.0 补齐，`image-round` / `image-mip` 两页对 `docs/pixels` 归零，于是这几份可以整份退场。**删掉的源文件**（~12k 行、含 4 份 GLSL/HLSL）：`src/backends/` 的 `binui_gl.{h,cpp}`（2250 行）、`binui_vk.{h,cpp}`（4560 + SPIR-V 常量表 `binui_vk_shaders.h`）、`binui_d3d11.{h,cpp}`（2457 行）、`binui_gpu.{h,cpp}`（1714 行）、`binui_auto.{h,cpp}`（327 行），以及只被它们 include 的 `binui_gl_ext_min.h`；**删掉的工具**：`gl_shot` / `vk_shot` / `vk_window` / `vk_stress` / `d3d11_shot` / `gpu_shot` 与 `tools/vk_shaders/`（GLSL 源 + `.spv`）；**退场的门禁**：`pixels` / `pixels-vk` / `pixels-d3d11` / `pixels-gpu`（那几条是"三个后端互拍"，后端没了就没有执行体）、`bench-gl` / `vk-window` / `vk-stress` / `resize-bench` / `baseline-*`、`demos-vk` / `demos-d3d11` 与 `-DBINUI_GPU_VK\|GL\|D3D11\|2D` 四个编译期宏（`BINUI_GPU=` 环境变量一起退场）。**剩下的路径只有一条**：`src/backends/binui_bin2d.{h,cpp}`（`BinUI2D` / `BinUIApp2D`）+ `binui_app.h`（外壳）+ `binui_draw_nodes.h`（遍历）+ `binui_glyph_atlas.h`（排版/图集）+ `binui_paint.cpp`（覆盖率条带），像素门禁只剩 `pixels-bin2d`（CPU 权威，0 容差）与 `pixels-bin2d-gpu`（唯一 GPU 门禁，对 `docs/pixels`） |
| **4. binUI 只留一条路（master 2026-11-20：「把整个 backends 目录砍掉，直接接 bin2D/3D」）** | **已完成接线（binUI 0.37.0 / bin2D 0.22.2）** | `BinUI2D` 把 canvas 挂上 `bin2d::GpuExec`：binUI 这一层不再有像素数学、不再有图形 API 调用，圆角 / 圆角描边 / 渐变 / 字形 / 九宫格 / 图层 / 毛玻璃全部由 bin2d 的语义翻成 binGPU 的图元。降级链三级（执行体 → `GpuCompositor` 贴屏 → GDI），`presentBackendName()` 如实报当前一级。帧时 `buttonshow` 1020×684 9.07 → **0.68 ms**（13.3×）、`appshow` 1280×800 8.88 → **0.74 ms**（12.0×），与 binUI 自带的 `binui_gpu`（0.64 / 0.69）**同量级** —— 可见"把 2D 语义往下一层搬"没有代价，只是少了一千八百行。为此 bin2D 0.22.2 补了三条原语：`Canvas::strokeRounded`（否则所有圆角边框整片消失）、`imageChanged`（字形图集原地改像素 ⇒ 一直采样旧内容）、`backdropBlur`（CPU 侧明确返回 false），外加 `GpuExec` 的节拍 |

**卡点只剩一个了**（历史记录：这句写于 3c 之前，卡点后来由 binGPU 0.15.0 收掉）：
把 binUI 换成"先记列表 → 交给执行体"会让圆角弧边整体挪
（`docs/pixels` 的 GL 基准已经换成解析 SDF、`tests/pixels-bin2d` 还是扫描线，两者差 41 个电平）。
**0.35.0 之后 binUI 的三端已经全是解析 SDF 了**，所以这一步的前置条件比之前好：剩下的分歧只在
bin2d 的 CPU 光栅那一边。

这一轮唯一动的是**节拍**：它原本也在错的那一层（外壳自己算网格 + 滞回 + 猜 present
有没有在等垂直同步），现在按同一套"判据在设备层"的道理挪进了 binGPU。
可见的后果是 master 直接能感觉到的：GPU 路径从 55/31/100 fps 变成
**unlimited 下 2116/1893/702 fps、standard 下 114/116/108 fps**。

**卡点只剩一件 binGPU 的事了**（VK 多目标那条已被 binGPU 0.13.0 收掉）：

1. ~~VK 后端的多目标来回切换（3b 的唯一阻塞）~~ —— **已完成**（binGPU 0.13.0：顶点流扩容改成
   retire-then-free、采样纹理用完重新 transition；`appshow` 1280×800 在 VK 上 0 差异、最大通道差 7，
   于是 0.36.0 那颗"先钉 GL"的钉子也拔了）；
2. **MSAA + 真正的 mip 采样**（3c 的唯一阻塞，也是现在唯一挡在"删掉五份后端"前面的东西）。
   注意这不是"还没做"，是**四端一致的设计取舍**：`bingpu_cpu/gl/vk/d3d11` 全部硬写
   `msaa_samples = 1`，`BGPU_TEXTURE_MIPMAPS` 一律降级为 1 级 —— 因为 binGPU 的合同是
   "与 CPU 参考逐像素相同"，而 CPU 参考没有 MSAA 也没有 mip。老的三份 binUI 后端恰好
   在合同之外多给了这两项（GL 4×），所以删它们之前得先让 binGPU 把这两项长进合同里，
   否则代价是可量的：`image-mip` 页 37%、`image-round` 页 0.217%（最大通道差 120）。

   **进展（binGPU 0.14.0 / 0.14.1）**：两项都已经是真能力（VK + GL，D3D11 明确不做，
   缺能力位时如实报 1）；合同改成"同档设置下与 CPU 参考逐像素相同，高档是显式质量档"，
   ABI 4 → 5。实测：真 mip 把 `image-mip` 从 37% 收到 0.118%、`svg-filter` 从 1.4% 收到 0。
   **但 MSAA 在多目标 / 图层路径上还会丢内容**（`svg-filter` 页 4× 下整页空白，1× 下 0 差异），
   所以 3c 的阻塞项从"补 MSAA + mip"变成"修多目标 MSAA 这一处"。这个 4× 一旦修通，
   `image-round`（旋转边）与 `image-mip` 会一起变成 0 差异 —— 那才是删老后端的时机。

   两件都在 binGPU 里、也只该在 binGPU 里修 —— 这正是 0.36.0 想证明的分工：
   **binUI 只决定画什么，怎么画（含 AA 质量档）由 binGPU 决定。**
   （历史：这两项已在 **binGPU 0.15.0** 补齐并修通多目标 MSAA，所以 3c 得以落地。）

`binui_gpu.cpp` 曾经是 binUI 唯一的 GPU 绘制实现：它 ~1900 行，其中没有一行着色器，
对比被它取代的三份（`binui_gl` 2250 行 + `binui_vk` 5600 行 + `binui_d3d11` 2900 行，
含 4 份 GLSL/HLSL 着色器）。**这份 `binui_gpu.cpp` 本身也已在 3c 删除** ——
它的职责被 `BinUI2D` + bin2d 的 `GpuExec` 接走（同一条"语义在 bin2d、设备在 binGPU"的边界，
只是再往下挪了一层）。

### 圆角 AA 已经统一了（0.34.4，master 2026-11-20 定"用质量最好的那一套"）

| 路径 | AA 做法 | 谁在用 |
|---|---|---|
| `Canvas::fillRounded` | 8 子采样扫描线 | binUI 的 bin2d 后端、离屏出图 —— **唯一的一份** |
| `RasterizeRoundedTileCoverage` | ~~解析 SDF~~ → **调上面那份扫描线**（0.20.0） | ~~binUI 的 GL / VK / D3D11 三端~~（三端已删除） |

> 历史：以前同一个圆角矩形的弧边差到 **48 个电平**，一直没被发现，是因为两个 golden 是分开的
> （`docs/pixels` 对 GL、`tests/pixels-bin2d` 对 bin2d 后端），**"逐像素相同"从来没跨过
> 实现比**。0.20.0 把模板内容换成 CPU 光栅自己的扫描线覆盖率之后：

- bin2d 的 `test_raster` 钉住"角片与 `Canvas::fillRounded` 逐像素相同（r=1..40）"，**max 0**；
- bin2d 的 `test_gpu_bridge` 钉住"九宫格 vs 直填"，四端 **max 0**；
- binUI 侧 `make pixels-bin2d` 一个像素没动（语义没变），而 GL 的基准图按新实现重生成了。

**仍未统一的一处**：盒子落在小数像素上时，位图模板（整像素相位）与连续相位的扫描线
仍会差 —— 实测 `box(8.5,8.5)` 差 416 像素、最大 119。这不是"两套定义"，是模板这个
表示方式的固有代价，要消掉得让模板带相位（每 1/8 像素一张，或用 SDF+导数着色器）。