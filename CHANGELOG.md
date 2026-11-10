# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
`【格式 vN】` 指 binUI 二进制布局的 `BINUI_VERSION`（`include/binui_format.h`），
与语义化版本不是一回事——**格式号只在二进制布局变化时递增**。

## [Unreleased]

### 新增 — Vulkan 后端 P2b：窗口 + 交换链 + 三路 present

`BinUIVK` 现在能开真窗口了：`init(hwnd, w, h)` 建 surface + 交换链，`present()` 按条件选路。
渲染目标仍是那张 MSAA + resolve 离屏图（与离屏模式完全同一条路径），present 时才把 resolve 图
blit 进交换链 —— 所以**窗口模式与离屏模式的像素必然一致**。

- 交换链：首选 `B8G8R8A8_UNORM`（与渲染目标同格式，blit 是纯拷贝）；`vsync=true` → FIFO，
  `false` → MAILBOX/IMMEDIATE 优先；`setVsync` 改呈现模式时重建交换链。
- 三路 present：交换链常规路径；透明窗口 → 读回 + `UpdateLayeredWindow`；客户区铺满显示器
  （borderless 全屏，直翻转风险）→ 读回 + `SetDIBitsToDevice`；`acquire/present` 返回
  `OUT_OF_DATE`/`SUBOPTIMAL` 时重建。
- 降级链：交换链创建失败、或驱动没暴露 WSI 时，自动退回"读回 + GDI 贴屏"，不会整体不可用。
- 新工具 `tools/vk_window.cpp`（+ `make vk-window`）：开真窗口跑 N 帧，把**交换链图像本身**
  读回写 PNG，与 `gl_shot` 同尺寸输出逐像素比。
- 新文件 `tools/save_png.h`：BGRA → PNG，抓图工具共用（vk_shot 的本地副本已删）。

#### 实测（`make vk-window`）

| 场景 | 结果 |
|---|---|
| probe / circle（窗口 + 交换链） | 与 GL **0 像素差异（阈值 0）** |
| svgpath（斜边 AA） | 611px（0.560%），最大通道差 116（与离屏同源） |
| 强制 GDI 路径 / 透明窗口 | 均 0 差异 |
| resize 600×260 → 760×340（交换链重建） | 与 GL 在 760×340 下 **0 差异** |
| 全屏守卫 2560×1440 | 自动切 `GDI 读回`，11.4 fps，无停帧 |
| 交换链帧率（vsync=false, MAILBOX, 600×260） | 1435 fps |

#### 踩到的坑

- **`VK_USE_PLATFORM_WIN32_KHR` 必须在 `#include "vulkan/vulkan.h"` 之前定义**（vendored 头
  按平台宏裁剪），否则 `VkWin32SurfaceCreateInfoKHR` 之类直接不存在。
- WSI / 交换链函数必须**单独加载且允许为空**：离屏模式没有 surface 扩展，用统一加载会让离屏
  初始化直接失败。

### 新增 — Vulkan 后端 P3：全原语（文本 / 图片 / 线段），离屏

`BinUIVK` 现在 7 个原语全通，并且**文本与图片的实现与 GL 共用一份代码**，
所以在像素回归里两个 GPU 后端对轴对齐几何逐位相同。格式号不变（无二进制布局改动）。

- 新文件 `src/backends/binui_glyph_atlas.h`：字形图集 + 文本排版（UTF-8 迭代、emoji 簇、
  逐字形回退、图集多代、COLR 彩色 emoji、换行/对齐）。GL 与 Vulkan 都只实现一个
  `IGlyphTexHost`（位图 → 纹理）并消费 `GlyphQuad` 回调 —— 后端不再有自己的排版代码。
  落地方式：把 GL 里的实现整块搬出来，改完与搬迁前逐像素对比（阈值 0，3 个用例 **0 差异**）。
- 新文件 `src/backends/binui_win_image.h`：GDI+ 图片解码（原本 GL 与 D3D11 各一份拷贝）。
- Vulkan 纹理系统重写：上传**可以发生在帧内**（staging 拷进本帧命令缓冲，帧末等 fence 后释放），
  帧外的上传排队到下一个 `beginFrame` 开头。**因此不再需要预热帧** —— `vk_shot` 从渲两帧
  改成渲一帧，结果不变；P2 的"首帧缺圆角"瑕疵随之消失。
- 图片：`prepareImage`（PNG/JPEG 解码）/ `prepareImageRaw`（原始 RGBA，CPU 侧换 R/B 通道）/
  `drawImage`（线性采样），`generation` 不变则复用。
- 采样器拆成 nearest / linear 两个（原先每张纹理建一个 sampler，白建白删）。
- descriptor pool 加 `FREE_DESCRIPTOR_SET` 并在纹理销毁时回收，池满自动再开一个。

### 修复 — `vkCmdClearColorImage` 在 render pass 内调用会丢掉整帧

文本首次光栅化发生在帧内，于是"清零新图集"落在 render pass 里。Vulkan 不允许，
NVIDIA 驱动**静默丢掉整条命令缓冲** —— 现象是"只要画面里有文字，整帧只剩底色"。
改为用全零 staging 上传清零（`zeroTex()`），一次性 4MB/图集。

### 新增 — `make pixels-vk`

14 个用例（10 个像素卡片 + 4 个真实示例：`features` / `fontshow` / `svgdemo` / `texturedemo`）
跑 Vulkan ↔ OpenGL 逐像素对比。判据：不同像素 < 1.5%、单通道 ≤ 128（`pixel_diff` 新增第 5 个
参数）。为什么不是 0：4x MSAA 在**非轴对齐边缘**上，GL 与 Vulkan 的采样归属会差一个点
（实测逐像素正负各半、净偏 ≈ 0），这是实现差异而非绘制 bug；两类后端与 GDI 基准的距离相同
（svgjoin：VK↔GDI 7494 vs GL↔GDI 7505，共 109200 像素）。

实测：6 个纯轴对齐用例 + 4 个真实示例 **0 差异**（`fontshow` 最大通道差 0），
4 个 SVG 描边用例残差 0.2%~1.0%，全部 ≤ 128 通道。

## [0.9.1] - 2026-11-10

### 修复 — 窗口最大化后画面停住、内容割裂

- **现象**：`svgdemo` 的 `<canvas>` 频谱条在窗口最大化后不动了，还出现上下割裂；切回默认尺寸就恢复。
- **根因有两层**：
  1. **呈现**：客户区铺满整块显示器（borderless 全屏 / 最大化）时，DWM 会把 `SwapBuffers` 提升成
     直翻转。实测（NVIDIA + 远程桌面/虚拟显示器）应用仍在 ~100fps 绘制、`glBlitFramebuffer` 也正常，
     但屏幕停在旧帧 —— 所以看到的是"动画停了"。切回普通尺寸时直翻转取消，画面立刻恢复。
     修法：新增 `clientCoversMonitor()`（客户区宽高 ≥ 所在显示器），命中时和拖拽 resize 期间一样走
     `presentViaGdi`（读回离屏 → `StretchDIBits`），完全绕开驱动 present 路径；判定在
     `applyPendingResize()` 里重算，尺寸变回来自动切回 `SwapBuffers`。
  2. **失效矩形**（demo 侧）：`svgdemo` 把 `<canvas>` 的矩形在布局后缓存起来，窗口一改大小
     （`ui_scale="proportional"` 会把它挪走并缩放），缓存矩形失效 → 每帧只重绘旧位置那一小块，
     画布其余部分永远停在上一帧 → 割裂。修法：改成 `renderer.invalidRect(id)` 现取（`invalidateNode`），
     并把该坑写进 demo 注释。
- **实测**：修复后最大化状态画布区两帧差异 15.7 万像素（动画正常）；修复前 0。窗口 700/1200/1600/2000px
  宽正常，只有正好铺满显示器（2560×1440）触发，与报告一致。

## [0.9.0] - 2026-11-10

本轮主题：**SVG 填充的边缘抗锯齿**（【格式 v19】）。破坏性：`BINUI_VERSION` 18 → 19，
旧 `.bui` / `.bum` 需重编（新增 `PATH_AA` 标志位）。

### 新增 — `shape-rendering` 开启边缘抗锯齿

- `shape-rendering="auto" | "optimizeQuality"` → 该子树里的**路径填充**边缘抗锯齿；
  `crispEdges` / `optimizeSpeed` / 没写 → 硬边（**默认，与 0.8.x 完全一致**）。
  可继承（写在 `<svg>` / `<g>` 上对整个子树生效），不认识的值告警。
- 不做 MSAA/stencil：三个后端只有 `fillRect`，所以把 AA 做进共用扫描线填充器 ——
  每行纵向 4 次子采样得到**逐像素覆盖率**，同覆盖率的连续像素合并成一条带，颜色 alpha 乘覆盖率。
- 宽 > 4096px 或条带数超上限时自动退回硬边（宁可锯齿也不崩）。

### 修复 — 覆盖率定点精度

第一版累加器用整数 `255/4 = 63` 做单位，满覆盖只剩 `252/255`，整块面积被系统性低估 1.2%
（三角形实测 4940 / 解析 5000）。改 1024 定点后实测 5000.2。

### 实测

- 面积守恒：100×100 盒里的直角三角形，AA alpha 加权面积 5000.2（解析 5000），硬边 5048。
- 条带数：三角形 AA 199 条（100 条半透明边缘），硬边 98 条（约 2×）。
- 三端：AA 填充在**纯色背景**上 GDI 与 GL **逐像素一致（0 差异）**；叠在渐变上时 1.58% 的像素
  有差异（GDI 用近似底色做 alpha 混合的既有限制，非 AA 的半透明填充叠渐变同样有 20% 差异）。
- 像素基准新增 `svgaa`（420×260），`make pixels` **10/10 零差异**；`svgdemo` 星形图标改用
  `shape-rendering="auto"` 作为长期样例。

### 明确没做

描边的抗锯齿（GDI 的 `LineTo` 是硬边，GL/D3D 的 `fillLine` 自带 AA —— 既存差异，需要重写描边通路）；
默认开 AA（会改所有既有 UI 的观感并让 10 个基准全部失效）。

## [0.8.2] - 2026-11-10

### 修复 — 折线被当成闭合多边形填充

- 0.8.0 把 `<polyline>`/`<polygon>` 改成编路径后，**没写 `fill` 的折线会按 SVG 默认填成黑色**
  （`svgdemo` 的走势图就变成了两块黑疙瘩）。这符合规范、浏览器同样如此（SVG 的经典坑），
  但对"以为自己在画线"的人是一记闷棍。
- 现在：`<polyline>`/`<polygon>` 有 `stroke` 却没提过 `fill` 时**明确告警**并给出写法
  （`fill="none"`）；同时把 `svgdemo` 里的折线/轮廓补上 `fill="none"`（素材本身不干净）。
- 注意"没写 fill"和"写了 fill=none"是两回事：前者是默认黑填充，后者是不填 —— 告警只在
  **从没提过 fill** 时触发（`fill="none"` 算提过）。

### 验证

- `make all tools` 0 warning；四套测试全绿；`make pixels` 9/9 零差异；
  `check_examples` 11 个 UI 0 问题；`svgdemo` 截图重出（走势图恢复成纯折线）。

## [0.8.1] - 2026-11-10

本轮主题：**SVG 收尾的两件漏项**（无格式变化，`BINUI_VERSION` 仍是 18）。

### 修复 — `userSpaceOnUse` 下的百分比改为按视口折算

- 以前把 `x1="50%"` 当成 0.5 个用户单位（差一个数量级，画出来几乎看不见），只能告警让作者改写。
  现在按 SVG 规范折算：`x` 方向 × 视口宽度、`y` 方向 × 视口高度、`r` × `sqrt((vpW²+vpH²)/2)`。
- 视口尺寸由编译器 `Ctx.vpW/vpH` 带下去：进 `<svg>` 取 viewBox 宽高（无 viewBox 退回 `width/height`），
  `<g>` 与普通容器继承。`objectBoundingBox`（默认）语义不变。
- 实测：viewBox 100×50 + `x1="0%" y1="0%" x2="100%" y2="100%"` → 归一化端点 `(0,0)→(1,1)`。

### 新增 — 路径填充条带的跨帧缓存

- 小图标上填充条带本来可以忽略（22 条带 ≈ 0.018 ms），但大图形不是：300px 的环 332 条带
  ≈ 0.218 ms/次，几个就吃掉一帧的预算。
- 缓存条目 `PathBandCacheEntry` 放在 `binui_paint.h`，由渲染器按 nodeId 持有（**一份实现，
  三端共用**，三端一致性不靠各端自觉）；key 混合了节点盒尺寸、填充/描边色、不透明度、
  两条渐变指针与 flags —— 任何影响条带颜色的东西变了 key 就变，不用逐项追踪失效；
  布局变化（`loadLayout`/`relayout`/`mountComponent`）整表作废并 ++gen。
- 两条预算：单节点条带 > 1024 不缓存、缓存条目 > 256 不再新建（大图不会吃内存）。
- 实测（2000 次同一节点）：24px 图标 0.018 → 0.0002 ms/次（~86×）；
  300px 环形 0.218 → 0.0002 ms/次（~1051×）。
- 正确性：单测用"篡改缓存里的颜色再画一次"证明真的命中了缓存；换颜色必须重算并写回；
  `nullptr`（不缓存）照常绘制。`make pixels` 9/9 零差异 —— 缓存没有改变任何像素。

### 验证

- `make all tools`：0 warning；四套测试全绿（新增 `TestPathBandCache` 与 userSpace 百分比断言）。
- `make pixels` 9/9 零差异；`check_examples` 11 个 UI 0 问题。

## [0.8.0] - 2026-11-10

本轮主题：**SVG 收尾**（【格式 v18】）—— 元素类型齐了、坐标系闭合了、文字锚点对了、
渐变的偏心焦点真算了。破坏性：`BINUI_VERSION` 17 → 18，旧 `.bui` / `.bum` 需重编。

### 新增 — `<polygon>` / `<polyline>` 编成路径

- 以前两者被拆成一堆 `BINUI_LINE`，**`fill` 完全无效**（多边形填充是最常见的写法，这是真 bug），
  渐变描边/虚线/连接方式也表达不了。现在有颜料就编成 `BINUI_PATH`（`polygon` 闭合、
  `polyline` 开放），填充/描边/虚线/linecap/linejoin/渐变全部复用路径通道。
- 既没 `fill` 也没 `stroke` 的裸 `<polyline>` 仍然不产生节点（不凭空多一个黑块）。

### 新增 — `<line>` 按需升级成路径

`<line>` 保持轻量 `BINUI_LINE`，但写了 `stroke-dasharray` / 渐变描边 / `stroke-linecap` /
`stroke-linejoin` 时自动升级成两点开放路径 —— 这些能力 LINE 节点表达不了（以前是静默失效）。

### 新增 — 文字与虚线

- `text-anchor="start|middle|end"`：`x` 从"盒子左边"变成"锚点"（用编译期字宽估算折算出盒子左边）。
- `stroke-dashoffset`：编译期把虚线相位往前推（负值取模）。实测 pattern `10 10`、路径 0→100 时，
  offset 0/5/10 的首段实线分别是 `[0,10]` / `[0,5]` / `[10,20]`。

### 新增 — `preserveAspectRatio` 的 `slice` 与 9 宫格对齐

`xMinYMin … xMaxYMax` 九种对齐 + `meet`/`slice` + 可选 `defer` 全部支持；
`slice` 取较大缩放（铺满、超出由 `<svg>` 裁剪）。

### 修复 — 嵌套 `<svg>` 的三处偏移（以前是"整块空白"）

内层 `<svg>` 的 `x/y/width/height` 是父用户坐标，而普通元素的 left/top 是容器像素。三处必须同时
对齐：① 节点盒要过父变换取包围盒；② viewBox 要映射到**设备尺寸**的内容盒（映射到用户尺寸会把
父变换算两遍）；③ 运行期重排的 `relX/relY` 必须是相对父内容盒的偏移（写绝对坐标会在 `setSize`
后再挪一次 —— 这也是为什么 GL 离屏先 `setSize` 时"只有 GL 露馅"）。修完后嵌套 svg 在 GDI 与 GL
下逐像素一致（`0 diff`）。

### 新增 — 径向焦点 `fx/fy`（SVG 锥形语义）

从焦点 `F` 经 `P` 的射线打到单位圆于 `Q`，`t = |P-F| / |Q-F|`；实现是归一化后解
`|F + s·(P-F)| = 1` 取正根、`t = 1/s`。焦点在圆外时规范未定义 → 告警并按同心圆画。
（踩坑记录：二次项符号写反时圆心处 `t` 会变成 1.0，手算应为 `4/9 = 0.444`。）

### 变更 — 格式 v18

- `BinUIPathGradient` 增加 `fx, fy`（92 → 100 字节）；其余能力（polygon/polyline、`<line>` 升级、
  `text-anchor`、`dashoffset`、`slice`、嵌套 svg）都不改格式。

### 验证

- `make all tools`：0 warning；四套测试全绿。
- `suite_svgpath` 新增：polygon/polyline → PATH（含"裸 polygon 不产生节点"）、`<line>` 按需升级、
  text-anchor 盒子左移、虚线三相位的首段区间、`BuildViewBoxMatrix` 的 meet/slice/9 宫格矩阵、
  径向焦点四个 t 值、嵌套 svg 盒随 `<g>` 缩放平移。
- 像素基准新增 `svgfinish`（420×260，覆盖上述全部）；`make pixels` **9/9 零差异**。
- 三端一致性（`gl_shot` GL 离屏 vs GDI 基准）：嵌套 svg 修完后该区差异只剩 125/4096（圆角与描边
  AA），纯填充区（polygon 星形、径向焦点球）**0 差异**。
- `check_examples`：11 个 UI 0 问题。

## [0.7.0] - 2026-11-10

本轮主题：**描边能力补完 + 精确命中**（【格式 v17】）。
破坏性：`BINUI_VERSION` 16 → 17，旧 `.bui` / `.bum` 需重编。

### 新增 — 渐变描边 `stroke="url(#id)"`

- 以前只按首个色标纯色画（还带告警），现在真的渐变：**每段线按自己的中点取色**。
  折线本来就是曲线细分出来的（24 单位图标 ≈ 0.1px 容差），逐段取色等价于逐像素取色，
  不需要给后端加渐变画刷，三端仍然只画 `fillLine` / `fillRect`。
- 线性 / 径向、`spreadMethod`、`gradientTransform`、`objectBoundingBox` / `userSpaceOnUse`、
  2–8 个色标、`href` 继承 —— 与填充渐变同一套能力，坐标折算也**共用同一个函数**
  （只有 alpha 键不同：`stroke-opacity` 而不是 `fill-opacity`）。
- 填充与描边可以同时是渐变：路径记录里两条可选渐变记录各占 92 字节，互不影响。
- 继承：`<svg stroke="url(#g)">` / `<g stroke="url(#g)">` 全子树生效；子元素写纯色 `stroke`
  会清掉继承来的渐变。
- `<line>` / `<polyline>` / `<polygon>` 表达不了渐变描边（它们编成线段节点），明确告警并建议改用 `<path>`。

### 新增 — `stroke-linecap="square"`

- 方头 = 端点沿切线各外延半个线宽；转角几何仍用未外延的原始端点（否则拐角会多出一截）。
  实测：线宽 10、路径 0→100 时 butt 是 `(0,0)-(100,0)`，square 是 `(-5,0)-(105,0)`。

### 新增 — 精确命中（P3 首项）

- `BINUI_PATH` 有填充时，包围盒命中之后再按"点在路径内"判一次（`PointInPathNorm`，
  与 `PointInPath` 同一套绕数/奇偶逻辑，直接吃 `uint16` 归一化点），顺带取 ±1 设备像素容差。
  **环的洞、星形的凹口不再误命中**；描边-only 的路径仍按包围盒（细线本来就不好点）。

### 修复 — `fill-opacity` / `stroke-opacity` 作为表现属性时被静默忽略

- `fade()` 以前只查 `style`（内联 style / CSS 规则），没查元素属性，所以
  `fill-opacity="0.5"`、`stroke-opacity="0.5"` 都不生效。现在按 `style > 属性` 取值 ——
  渐变描边要折 `stroke-opacity`，正好踩在这条路上。

### 变更 — 格式 v17

- 路径记录在填充渐变之后可再追加一条 `BinUIPathGradient`（`PATH_STROKE_GRAD`）。
- `stroke-linecap="square"` 落进既有的 `BinUIPathEntry.cap` 字段（`BINUI_CAP_SQUARE`）。
- 渲染器 / 链接器 / 三端 `BINUI_PATH` 分支都带上描边渐变：`PaintPathNode` 的颜料参数
  收拢成 `PathPaintSpec`（填充色/描边色/两条渐变/不透明度），描边回调从"固定颜色"改成"逐段取色"；
  GDI 的画笔缓存改成按"宽度 + 颜色"缓存（否则渐变描边会一直用第一段颜色）。

### 验证

- `make all tools`：0 warning；四套测试全绿。`suite_svgpath` 新增：方头外延、渐变描边逐段取色
  （纯色同色 / 渐变首末段色差）、精确命中（环带命中、洞心不命中、盒外不命中）、
  `linecap=square` 落库且不报警告。
- 像素基准新增 `svgstroke`（420×260）：渐变描边四样（线性/径向/三色标/虚线）+ 线帽三连 +
  方头虚线 + 三个真图标；`svggrad` 基准重出（`stroke="url(#…)"` 现在是真的渐变）。
  `make pixels` **8/8 零差异**。
- 三端一致性（`gl_shot` GL 离屏 vs GDI 基准）：渐变填充区内部像素 **20694 个 0 差异**；
  `svgjoin` 内部 702 个中 5 个、`svgstroke` 内部 444 个中 12 个不同 —— 差异全在线段边缘
  （GDI `LineTo` 与 GL `fillLine` 的取整/抗锯齿，与渐变无关）。
- `check_examples`：11 个 UI 0 问题；`examples/svgdemo` 的心形图标改成三色标渐变描边 + 方头。

## [0.6.0] - 2026-11-10

本轮主题：**`stroke-linejoin` 的 miter / bevel 补齐**（SVG P2 收尾，格式仍是 v16，
**不破坏兼容** —— 旧二进制照旧能读，没有 `stroke-linejoin` 的路径仍然是圆角连接）。

### 新增 — `stroke-linejoin`: round / miter / bevel + `stroke-miterlimit`

- 圆角（`round`，binUI 的历史默认）、斜接（`miter`）、斜面（`bevel`）三种连接都实现了：
  - `miter` 补四边形 `(p1, m, p2, b)`，`bevel` 补三角形 `(p1, b, p2)`；
  - `ratio = 1/sin(θ/2)` 超过 `stroke-miterlimit`（SVG 默认 4）时按规范退回 bevel；
  - 转角 < 15° 的微折点仍然不补（折线是曲线细分出来的，缺口只有零点几像素）。
- **实现方式**：后端没有"填多边形"的原语，缺口复用路径填充那套扫描线条带
  （`BuildPathFillBands` → `fillRect`），三端只多接一个"填条带"回调，几何只写一份。
  一个 72px 图标加 10 个斜接尖角，合计只多 **9 条条带**。
- **斜接尖角会伸出几何包围盒 `(ratio-1)·半线宽`**，而三个后端都把每个节点裁剪到自己的矩形 ——
  不处理的话 `miterlimit` 调大也看不出变化（尖角画出来又被裁掉）。现在编译期用运行时
  同一个 `PathJoinAt` 逐顶点算交点、把节点盒撑开（五角星 72px 盒 / 2px 描边 /
  `miterlimit=10`：盒子 **88.00×84.08 → 97.19×92.43**），尖角不再被切平。
- `stroke-miterlimit` 只认数字（<1 告警并按 1 处理）；藏在路径记录的空闲字节
  `BinUIPathEntry.reserve` 里（0 = 运行期按默认 4），**所以格式版本不需要升**。
- 认不出的 `stroke-linejoin` 值明确告警（可选 round / miter / bevel），不再静默。
- `stroke-linecap="square"` 仍未实现（按 butt 画并告警，要方头把两端各延长半个线宽）。

### 验证

- `make all tools`：0 warning；四套测试全绿。`suite_svgpath` 新增：
  - 运行期几何 9 组断言（斜面三角形顶点、斜接四边形顶点、`miterlimit` 超限退回斜面、
    尖刺真斜接、共线不补、圆角不产生多边形）；
  - 编译端到端 7 组断言（三种 join 落库、`stroke-miterlimit` 落库、尖角超限时盒子不撑开、
    尖角未超限时盒子被撑开、未知值告警、`linecap=square` 告警）。
- 像素基准新增 `svgjoin`（420×260）：五角星 round / miter / bevel 三连 + 粗折线尖角 +
  `miterlimit=4` vs `12` 的尖刺对照 + star / chevron / triangle 真图标。
  `make pixels` **7/7 零差异**。
- 三端一致性（GDI 离屏 vs GL 窗口）：离描边边缘 ≥4px 的内部像素 702 个中 5 个不同（0.7%），
  差异全在线段的端点/AA 边缘（GDI `LineTo` 与 GL `fillLine` 的取整差异，与连接方式无关）。
- `check_examples`：11 个 UI 0 问题。

## [0.5.0] - 2026-11-10

本轮主题：**SVG 渐变**（`<linearGradient>` / `<radialGradient>`，【格式 v16】）。
破坏性：`BINUI_VERSION` 15 → 16，旧 `.bui` / `.bum` 需重编（`binui.res` 里是源码，重编即可）。

### 新增 — SVG 渐变填充

- `fill="url(#id)"` 引用 `<linearGradient>` / `<radialGradient>`，现在能直接粘现成素材了：
  - **线性**任意角度（`x1/y1/x2/y2`）、**径向椭圆**（`cx/cy/r`，包围盒非正方时两轴半径不同，
    符合 SVG 语义；`rx/ry` 分开存就是为了这个）
  - **2–8 个色标**：`offset`（支持 `"50%"`）、`stop-color`、`stop-opacity`，也认
    `style="stop-color:#fff;stop-opacity:.5"`；缺 `offset` 按等距插值、整体非递减夹紧；
    `stop-color="currentColor"` 沿祖先链取 `color`
  - `spreadMethod="pad|repeat|reflect"`
  - `gradientTransform`（走既有 `ParseSvgTransform`）、`gradientUnits="objectBoundingBox"`（默认）
    与 `"userSpaceOnUse"`
  - `href` / `xlink:href` 继承另一个渐变的色标与未写明的属性（深度 ≤8 + 环检测）
  - `fill` 的继承链继续生效：`<svg fill="url(#g)">` / `<g fill="url(#g)">` 全子树带渐变
- **实现方式**：渐变在编译期折成**包围盒归一化坐标**（与折线点同一套映射，缩放节点盒时渐变
  跟着缩放），运行期把每条扫描线条带的颜色从常量换成 `t` 的函数 —— 不需要着色器、不需要纹理、
  不需要每端各写一份采样数学，三端仍然只画 `fillRect`。两处关键取舍：
  - **量化**：`t` 量化到 `levels` 级（每 ~2 设备像素一级，8..96）、级内取级中心色，同色行才能
    继续合并成高条带 —— 否则每条带颜色都不同，400px 高的形状就是 400 条带；
  - **按方向切段**：颜色沿 x 变时按"走一级要几个像素"把这一行切细，纯垂直渐变一段都不切。
    结果横竖两个方向的条带数都只跟级数同量级：100×100 垂直/水平渐变各 **50 条**，
    24×24 图标尺寸对角渐变 **20 条**，400×400 对角渐变 **2352 条一次画完**（不降级）。
- `<circle>` / `<rect>` 的**渐变填充**也会自动降级成路径（轻量映射只能表达一个颜色）。
- 三端一致性实测：同一份 UI 的 11 个渐变区域（横/竖/45°/径向/宽盒径向/repeat/rotate/窄条/
  盾牌/圆角矩形/`<g>` 继承）**GDI 与 GL 逐像素完全一致（最大通道差 0）**；差异只落在既有的
  描边边缘（GL `fillLine` 自带 AA）与文字抗锯齿上。

### 新增 — 不支持的渐变写法一律告警（都带替代方案）

`stroke="url(#…)"`（渐变描边，按首个色标纯色画）、`>8` 个色标（截断）、径向焦点 `fx/fy`
（按同心圆画）、径向 + `gradientTransform` 含旋转/斜切（按轴对齐椭圆近似）、
`userSpaceOnUse` 下的百分比（按用户单位折算，不是 viewport 比例）、
`<text>` / `<polyline>` / `<polygon>` 的渐变填充（降级纯色）、`url(#不存在)`（退回纯色）。
`<linearGradient>` / `<radialGradient>` / `<stop>` 也不再出现在"不支持标签"表里。

### 变更 — 格式 v16

- 路径记录在定长头 `BinUIPathEntry` 之后可选追加 `BinUIPathGradient`（92 字节：
  类型 / spread / 2 个端点或圆心 + 椭圆两轴半径 / 内联 8 个色标），只有 `PATH_FILL_GRAD`
  置位时才写 —— 纯色路径的字节布局与 v15 完全一样，多出来的成本只落在渐变节点上。
- 单条路径的条带上限 `kMaxPathBands` 2048 → 4096（渐变的条带比纯色多，见上）。
- 渲染器 / 链接器 / 三端 `BINUI_PATH` 分支都带上了渐变；`PathRecord` 新增 `hasGrad` + `grad`。

### 验证

- `make all tools`：0 warning。
- 四套测试全绿；`suite_svgpath` 新增 `TestGradientBands`（量化/切段/合并/spread/退化/透明度
  共 9 组数值断言）与 `TestGradientCompile`（默认 oBB 端点、`userSpaceOnUse` 折算、
  `href` 继承、径向默认 50%、缺定义告警 4 组端到端断言）。
- `check_examples`：11 个 UI 0 问题；`examples/svgdemo` 图标条新增四个渐变样例并重出截图。
- `make pixels`：**6/6 零差异**（新增基准 `svggrad`，420×260）。

## [0.4.0] - 2026-11-10

本轮主题：**SVG 从"五个图元"升级成"能直接粘现成素材"**（【格式 v15】），
外加打包清单 `binui.res`、`binui_export` 解包工具，以及一批"写了没效果"的告警清理。
破坏性：`BINUI_VERSION` 14 → 15，旧 `.bui` / `.bum` 需重编（`binui.res` 里是源码，重编即可）。

### 新增 — SVG P2-4：`stroke-linecap="round"`

- 开放子路径的两端可以补圆点了（复用 `PaintPathStroke` 已有的 `roundCap` 参数，
  格式里 `BinUIPathEntry.cap` 这个字节一直在，只是编译器没填）。
- `stroke-linecap="square"` 未实现：按 butt 画并明确告警（要方头把路径两端各延长半个线宽）。
- `stroke-linejoin="miter"/"bevel"` 未实现：按圆角连接画并告警（尖角图标会比标准 SVG 略圆）。
  以前这两个属性在"已消费属性"表里，写了就是**静默无效** —— 现在至少说出来。

### 新增 — SVG P2-3：`<text>` 与 `<image>`

- `<text x y font-size fill>` 编成普通 `LABEL`：坐标走 viewBox/transform，`fill` 当文字色，
  `font-size` 直接生效；SVG 的 `y` 是**基线**，按"盒顶上移 0.8 个字号"近似
  （没有富文本能力，`<tspan>` 仍告警）。
- `<image x y width height href>` 编成普通 `IMAGE`：`href` / `xlink:href` 进资源表
  （路径同样相对 UI 文件），尺寸跟着 viewBox 缩放；`data:` 内联图片明确告警（不支持）。
- 修一个"属性名走错通道"的坑：SVG 用 `font-size` 属性，而编译器取字号读的是 `size`
  属性 + `font-size` CSS —— 只写 SVG 属性时字号被忽略（实测落库 14 而不是 20）。
- 验证：`suite_svgpath` 新增 text/image 端到端断言（文本、颜色、基线近似、字号、
  imageIndex 与资源表路径）；四套测试全绿、`check_examples` 0 问题。

### 新增 — SVG P2-2：`<defs>` / `<symbol>` 模板 + `<use href="#id">` 引用

- 图标素材里常见的"一份模板多处引用"现在能用：`<defs>`/`<symbol>` 里的元素是**模板**
  （自己永远不画），`<use href="#id">`（也认 `xlink:href`）在引用处克隆一份。
- 语义对齐 SVG：`<use x y>` 平移；引用 `<symbol>`/`<svg>` 且给了 `width`/`height` 时，
  目标的 `viewBox` 按**等比居中**缩放到这个尺寸；克隆体去掉 `id`（避免重复 id），
  根元素改写成 `<g>`（`<g>` 不建立坐标系，viewBox 由 transform 承担）。
- 与组件内联不冲突：`<use src="x.bum">` 是**组件**（编译期内联/运行期挂载），
  `<use href="#id">` 是 **SVG 引用** —— 按有没有 `src` 分流，两者可以同时存在于一份 UI 里。
- 容错：引用不存在的 id、循环引用（引自己的祖先）、外链 `href` 都会明确告警，
  并且不留空盒子（以前会退化成一个 300px 宽的空标签）。
- 验证：`suite_svgpath` 新增 `<defs>` 模板引用两次 + `<symbol viewBox>` 按 40px 缩放的
  端到端断言；像素基准 `svgpath` 增加第三行（三个 badge 克隆 + 两个不同尺寸的
  symbol 三角形）；`make pixels` 5/5 零差异、`check_examples` 11 个 UI 0 问题。

### 新增 — SVG P2-1：带描边的圆/矩形自动走路径 + `stroke-dasharray`

- **`<circle>` / `<rect>` 带描边时自动降级成 `BINUI_PATH`**：以前它们只能走轻量映射
  （`LIST` + 圆角 = 一个颜色），`fill` 与 `stroke` 并存直接告警、描边被丢掉。
  现在只要写了 `stroke`（且线宽 > 0）就编成路径，圆角矩形的 `rx` 也会变成真正的
  四分圆弧（新 `AppendRoundedRect`）。没有描边时**继续走轻量映射** —— 九宫格填充
  比扫描线条带更干净，不该为了统一而变差。
- **`stroke-dasharray` 支持**：编译期按弧长把折线切成"实线段"子路径
  （新 `ApplyDashPattern`），运行期照常画折线 —— 三端零改动，曲线（虚线圆）也对。
  长度按用户单位再乘坐标变换系数，所以放大后虚线间距一起放大（SVG 语义）。
  奇数个值按规范重复一遍（`3` → `3 3`）；全 0 视为不画并告警。
- 仍不支持：`stroke-linecap`（一律 butt）、`stroke-linejoin`（一律圆角连接，转角 > 15° 补圆点）、
  渐变、`mask`/`clipPath`/`pattern`/`marker`、`<defs>/<use>/<symbol>`、`<text>`、`<image>`。
- 验证：`suite_svgpath` 增加圆角矩形几何、虚线切段（10 单位 + `2 2` → 3 段、实线总长 6）、
  "带描边的圆/矩形必须编成路径且 fill+stroke 都在"的端到端断言；像素基准 `svgpath`
  第二行补上四种新写法（描边圆 / 描边圆角矩形 / 虚线 / 虚线圆），5/5 零差异。

### 新增 — SVG `<path>` / `<ellipse>`：真正的矢量路径（【格式 v15】）

- **现在能直接粘现成图标**（Lucide / Feather / Material 那一套 `<path d="M…">`），
  不再是"五个图元 + 路径不支持"。命令集 `M L H V C S Q T A Z` 全支持（绝对/相对、
  隐式重复、紧贴负数、弧标志紧贴写法），`A` 在编译期转三次贝塞尔后统一细分。
- **格式**：新增节点类型 `BINUI_PATH = 10`（`BINUI_NODE_PATH`）。节点 `x/y/w/h`
  就是几何包围盒（有描边时外扩 `strokeW/2`，笔画不溢出、命中区盖得住），
  折线放在新的**路径段**里（定长头 + 子路径表 + `uint16` 归一化点），
  段序在扩展绘制段之后、名称段之前。
- **为什么存折线**：运行期只做"归一化点 × 节点盒"的两次乘加 —— 不解析、不缓存、
  重排/等比缩放下形状自动跟随；曲线细分在编译期一次做完（容差 0.1 用户单位，
  实测半径 10 的圆弧离真圆最大偏差 0.0196px）。代价是极限放大能看到折线。
- **三端共用一份几何**（`include/binui_paint.h`）：扫描线求交 → 轴对齐条带 →
  `fillRect`，相邻同宽行自动合并；撞条带上限时按行步长降级（1→2→4→8）保证画完
  （400px 的圆：cap=16 触发截断，cap=256 + 步长 8 → 47 条完整画完）。
  描边逐段画线、转角超过 15° 补一个圆点（圆角连接）。
- **SVG 语义**：`viewBox`（等比 `meet` 居中，`preserveAspectRatio="none"` 直接拉伸）、
  `<g transform>`（`translate/scale/rotate/skewX/skewY/matrix` 可串联，且 `<g>` 不建立坐标系）、
  `fill`/`stroke`/`stroke-width`/`fill-rule` 变成**可继承**属性（含 `currentColor`），
  `stroke-width` 跟着坐标变换一起缩放（放大后线不会还是 2px）。
  **SVG 颜料属性按规范走 CSS > 表现属性 > 继承 > 默认**（与 binUI 其它属性"属性盖 CSS"
  相反，单独一条 `pickSvgPaint`）。
- **顺手修掉一个真 bug**：`<svg>` 的注释从第一条就说"绘制裁剪到容器"，但三端都没实现，
  子图形一直能画到容器外面。现在 `clippingAncestors()` 把 `<svg>` 也算裁剪祖先。
- **破坏性**：`BINUI_VERSION` 14 → 15，旧 `.bui` / `.bum` 一律拒载，需重编
  （与历史每次 bump 一致；`binui.res` 里存的是源码，重编即可）。
- 仍不支持（有明确告警）：渐变 `linearGradient/radialGradient`、`stroke-dasharray`、
  `mask`/`clipPath`/`pattern`/`marker`、`<defs>/<use>/<symbol>`、`<text>`、`<image>`。
  `<circle>` / `<rect>` 仍是轻量映射（不能 fill 与 stroke 并存），需要描边请用 `<path>`。
- 验证：`test_elements` 新增 `suite_svgpath`（解析/精度/容差/错误/上限、面积与采样
  双向验证填充、evenodd 与 nonzero 差异、条带合并、描边接缝、viewBox/g transform/继承/
  CSS 优先级）；像素基准新增 `docs/pixels/svgpath.png`；GDI 与 GL 对照 —— 填充 0 像素差，
  描边只在边缘因 GL 的 `fillLine` 抗锯齿不同。

### 修复 — SVG 不支持的标签/属性从"静默无效"改成明确告警

- 现象：写 `<path d="…">` / `<ellipse>` / `<text>` / `<g transform="translate(…)">` / `viewBox`
  编译**零提示**，画面上那一段就是空的，只能靠自己猜是写错了还是不支持。
- 根因：`<path>` 等标签走"未知 HTML 标签，已按普通容器处理"，对 SVG 作者毫无信息量；
  `transform` 属性更糟 —— 它在 `UsedHtmlAttrs()` 里，被当成"已消费"直接放行，**一条告警都没有**。
- 修复：`<path>` / `<ellipse>` / `<text>` / `<tspan>` / `<g>` / `<defs>` / `<linearGradient>` /
  `<radialGradient>` / `<mask>` / `<clipPath>` / `<pattern>` / `<marker>` / `<symbol>` / `<image>`
  都给出"不支持 + 换成什么"的专用提示；`transform` 属性与 `viewBox` 单独告警并说明替代写法。
- README 的 `<svg>` 条目补一份**不支持清单**（原来只写了支持什么）。
- `check_examples` 11 个 UI 仍 0 问题（没有误伤现有写法）。

### 新增 — 打包清单 `binui.res` + `binui_export`：**内嵌的包也能解包回打包前的目录结构**

- 之前文档里写"从 `.bui` 反推源码树做不到"——那是**没有清单**时的结论。现在补上了：
  编译时在产物目录写一份 `binui.res`（清单 + 原路径），记录主 UI 与每个被内联/外挂组件的
  **完整原始文件内容** + 相对 appRootDir 的路径；`binui_export` 按它逐字节还原，
  不是"节点树反推 HTML"那种有损转换，也不是偏移量记录（倒出来就是原文件本身）。
- **默认不产出，要显式开**：清单里带着源码，不该是默认行为 —— `binui_compile --res` 才生成。
  它是旁挂文件，布局里没有任何东西引用它。实测删掉后同一份 `.bui` 的离屏渲染
  **逐像素不变**（0 差异），只是不能再解包。
- **不动二进制布局**：`BINUI_VERSION` 保持 14，`.bui` 一个字节都不变，加载路径与启动性能
  零影响。想保护源码就别带这个文件（默认也不产出，要 `--res` 才有）。
- **一目录一份**：多个 `.bui` 编到同一目录时清单**合并**（同路径以本次为准）。
- 路径口径：先记绝对路径，最后统一折算成**相对所有文件共同祖先**的相对路径。所以
  `main.bui.html` 在 `examples/features/`、组件用 `../../components/x` 引用的工程，
  解出来的就是 `examples/features/main.html` + `components/x.bum.html`，与原树一致。
- 新增 `/lib` 侧：`CompileOptions::emitSrcMap`（默认 false）、`CompileResult::srcMap`、
  header-only 的 `include/binui_srcmap.h`（`BuildSrcMap` / `ParseSrcMap`）。
- 新增工具 **`binui_export`**：`binui_export main.bui -o restore/`，支持 `--list`。
  `binui_compile` 加 `--res` 才产出 `binui.res`（默认不产出）；旧产物 `<x.bui>.srcmap`
  仍能读取。
- 实测闭环（examples/features：源码组件 + 二进制组件 + 外挂组件）：
  编出 `main.bui` → `binui_export` → 6 个文件**逐字节 SHA256 全等** →
  解包出来的树再编一次 → `.bui` **逐字节相同**（同一 SHA256）。
- 顺带：`binui_compile -o` 指向不存在的目录时自动建父目录（以前直接失败）。
- 测试：`test_css` 新增 9 条清单断言（开关产出 / 可解析 / 第 0 条是主 UI /
  与原文件逐字节一致 / 多实例只记一份 / 嵌套组件在清单 / 读不到的组件不进清单 /
  路径都是相对 / 默认不产出）。

### 新增 — 文档：目录结构（单目录 / `binui/` 树 + 分类子目录；内嵌 vs 外挂的取舍）

- `docs/components.md` 新增第十节「目录结构」：
  - 几个组件 → **全放一个目录**（`main.bui`、`main.bui.html`、`button.bum(.html)`、图片）；
  - 几十上百个 → **`binui/` 树 + `components/<分类>/`**，带资源的组件（图标/字体/贴图）
    单独一个文件夹（`components/Button/primary.bum.html` + `star.png`），纯样式组件平铺即可。
  - 实测多级目录：宿主 `<use src="components/Button/primary.bum.html">`，组件内
    `<img src="../Icon/star.png">` 与嵌套 `<use src="../Label/tag.bum.html">`，
    编译产出 `736 字节，资源 1 项`，资源段里记的是宿主的 `components/Icon/star.png`。
- 内嵌 vs 外挂列了对照表并给出结论：**发行优先内嵌**（运行期零文件读取），外挂换来的是
  可替换性（皮肤/插件/按需加载），不是性能。
- 分发随意：只给 `.bum`（保护源码）/ 只给源码（可改、就地编译）/ 都给（二进制优先）。
- 已知限制补一条：**从 `.bui` 反向导出组件源码树现在做不到**（内联抹平了组件边界），
  要做需要"组件区间段 + 节点树→HTML 序列化器"两步，v15 起才可能。
- `README.md`、`components/README.md`、`docs/README.md` 同步一句指引；原「十、已知限制」顺延为十一。

### 变更 — 组件源码统一改名为 `.bum.html`（标准组件库 11 个 + examples/features 3 个）

- 事实：**组件源码的扩展名从来不影响任何行为**。编译器按**内容**选解析器
  （`CompileBinUIEx`：只有首个标签是 `<binui>` / `<root>`，或裸 `<app_window>` 且正文不含
  `DOCTYPE`/`<html>`/`<body>`/`<head>`，才走 legacy XML 路径）；组件内联更是直接交给 HTML
  元素解析器（`ExpandComponents` → `ParseElements`）。所以 `.bum.xml` 里的 "xml" 是一个
  不产生任何能力的后缀。
- 但它会误导：旧 README 写着"`.bui.xml` / `.bum.xml` 同样按 XML 解析"（**错的**），
  作者也会以为存在两种组件格式。主 UI 一直是 `main.html`，组件却叫 `.bum.xml`，两边对不上。
- 改名：`components/*.bum.xml`（11 个）与 `examples/features/*.bum.xml`（3 个）→ `.bum.html`；
  示例 UI、文档、Makefile / CMakeLists、`binui_compile` / `check_examples`、测试同步。
- **`.bum.xml` 仍然识别**（`ProbeAsset` 候选表里排在 `.bum.html` 之后），旧引用不会突然失效；
  只是文档和新代码不再这么写。裸名字候选顺序相应改为
  `.bum` → `.bum.html` → `.bum.xml` → `.bui.html` → `.bui.xml` → `.html` → `.xml`。
- 顺带修一个被改名暴露的 bug：`binui_compile x.bum.html` 的默认输出名算成了 **`x.bum.bui`**
  （`.html` 分支直接把后缀换成 `.bui`，没先判断是不是组件名）。现在正确输出 `x.bum`。
- 验证：`check_examples` 11 个 UI 0 问题；9 张离屏渲染图与改名前逐像素对比，8 张**零差异**，
  features 只差那三颗演示 `<use>` 写法的 tag（文案同时改成 `xxx.bum.html` / `xxx.bum` / `xxx`）；
  `make all` 0 警告，四套测试全绿。

### 修复 — 多根组件让编译器崩溃（`<use>` 展开时 `cremap[SIZE_MAX]` 越界）

- 现象：组件文件里写了**两个顶层元素**、宿主 `<use src="…">` 引用它，编译直接崩
  （MSVC/MinGW release 下是静默挂错父节点或整棵子树消失，debug 下 `_GLIBCXX_ASSERTIONS`
  报 `vector::operator[]: __n < this->size()` 失败后 `abort`，0xC0000005 / 0xC0000409）。
- 根因：展开组件时只把**第一个**顶层元素当作"组件根"替换掉，其余顶层元素的 `parentIdx`
  仍是 `SIZE_MAX`（无父哨兵），却被拿来当数组下标查 `cremap` → 越界读。
- 修复：多根组件改为用一个**合成的 `<div>` 承载所有顶层元素**，与二进制组件的链接器口径
  对齐（`binui_link` 本来就把组件内所有无父节点挂到锚点节点下）。合成容器不带组件任何属性
  （属性归宿主 `<use>`），并给一条编译期提示"有 N 个顶层元素，已用隐式 `<div>` 容器包裹"。
  单根组件的行为逐字节不变。
- 顺带：`<include>` / `<import>` 现在会明确告警"不生效，组件内联请用 `<use src=…>`"
  （以前只是一句"未知 HTML 标签"）。
- 回归测试：`test_css` component 段新增 4 条（多根编译成功 / 顶层元素一个不丢 /
  都挂在合成容器下 / 有提示）。

### 新增 — 元素级 `ui_scale="fixed"`：等比缩放窗口里保住"不许变形的 chrome"（标题栏按钮）

- 现象：`ui_scale="proportional"` 的示例最大化后，标题栏三颗控制按钮被拉成矩形（29×29 → 82×55），
  图标也跟着扁了。
- 根因：等比缩放按「新窗口 ÷ 设计尺寸」**分别**算 x/y 比例（2560/900 = 2.84、1440/760 = 1.89），
  方形元素必然被拉成矩形；字号只按纵向比例缩放，所以文字看着正常、盒子却走了形。
- 新增元素属性 **`ui_scale="fixed"`**（编译成 `SPEC_FIXED_SCALE`，复用规格段空闲位，不升格式版本）：
  该子树不参与等比拉伸，内部**统一按纵向比例缩放**（形状不变、字号一致），位置按设计留白贴最近
  的边（左右）或居中（上下），并取整到整像素避免 1px 描边发糊。`<titlebar>` 组件根节点默认带上，
  于是最大化后按钮仍是正方形、贴着右边缘、垂直居中。
- **描边也按同一个比例缩放**：边框宽度（`BinUIPaintNode::borderW`，加了一份编译期快照，每次重排
  从快照重算，不会累乘）和线段线宽（`BINUI_LINE` 的 `radius`）在固定子树里同样乘 `fs`。否则就会
  出现"放大后最小化那条横线变成 2px、最大化方框边框还是 1px"的错觉——两边的描边必须一起变粗。
  设计尺寸下 `k=1`，渲染与改动前逐像素一致。
- 回归测试：`test_elements` 9e —— 设计尺寸下 10×10；`setSize` 到 x:2 / y:1.5 后仍是正方形且按
  纵向比例变成 15×15。
- 验证：`gl_shot` 把 fontshow 渲到 2560×1440，三颗按钮 55×55、图标 17×17、留白 19/19/19/19；
  设计尺寸 900×760 下渲染与改前逐像素一致（`MIN 9×1`、`MAX/CLOSE 9×9`，留白 10/10/14/14）。
- README / components/README 补了 `ui_scale="fixed"` 的用法说明。

### 修复 — 窗口 resize / 最小化恢复后，标题栏的 ✕ 被拉成一条横线（svg 线段端点错乱）

- 现象：窗口改变尺寸（或最小化再打开触发重排）后，标题栏关闭按钮里的 ✕ 变成一条横贯窗口的
  粉色直线，图标像是被"抹开"了。
- 根因：`BINUI_LINE` 节点的 `width/height` 不是尺寸而是**终点绝对坐标**（起点 `x/y`，
  终点 `width/height`，`radius` 是线宽）。但编译期写重排规格时按普通节点处理：
  `sp.relW = baseW`（= `x2` 端点值，被当成"宽度"）。运行期 `relayout` 于是把起点算成
  "父内容盒原点 + x1"、终点直接用 `x2`（svg 局部坐标）—— 父容器一挪位置（标题栏按钮右对齐，
  窗口变宽按钮右移），起点跟着走、终点留在原处，线段就被拉成一条横线。
- 修复：编译期给线段存**起点偏移 + 终点增量**（`relX/relY = x1/y1`，`relW/relH = x2-x1 / y2-y1`），
  运行期按"起点 + 增量"还原终点，线段整体跟随父容器；`<polyline>` / `<polygon>` 展开出来的
  线段原来连规格都没有（父容器一动就完全不跟随），也一并补上。
- 顺带：`examples/flexshow` 的标题栏原来写死 `h="44"`（唯一显式指定高度的示例），29 高的按钮
  在 44 里居中到 y=7.5，图标又落回半像素；改成 `h="45"`。
- 回归测试：`test_elements` 9d 三条 —— 线段初始增量、重排后增量不变、跟随父容器。
- 验证：`gl_shot` 把 flexshow 渲染到 1300×760（走非等比重排），三颗图标 `MIN 9×1`（`L10 R10
  T14 B14`）、`MAX / CLOSE 9×9`（`L10 R10 T10 B10`）；文档图全部重出。

### 示例 — 标题栏左侧图标不再呼吸闪烁；三颗控制图标改成元素绘制（终于对齐了）

- `components/titlebar.bum.xml` 的左侧图标挂着 `animation: bui-tb-icon-pulse 2s infinite`
  （scale 1→1.15、opacity 1→0.75 来回），整条标题栏一直在"晃"。删掉动画属性和 `@keyframes`
  定义，图标从此是静止的；样式的 `.bui-tb-icon` 颜色/字号不变。
- 三颗控制按钮本来就是同尺寸（`bs` = 28×28），但**字形墨迹大小不一致**：`−` 7×2、`□` 7×7、
  `✕` 10×10，墨迹中心还差 1~1.5px —— 字形在字身框里的位置和大小由字体度量决定，换字号只能
  东拼西凑。所以**不再用字形，改成用元素画**（按钮里的绝对定位子节点，不参与命中测试：点图标
  等于点按钮，悬停底色照常从图标后面透出来）：
  - 最小化：`9×1` 的实心 div（`.bui-tb-bar`），落在按钮第 14 行；
  - 最大化：`9×9` 的空心 div，`border:1px solid #66768C`（边框落在第 10 / 18 行）；
  - 关闭：两条 `<line>` 从 (10.5,10.5)→(18.5,18.5)、(18.5,10.5)→(10.5,18.5)，`stroke-width:1`
    （`<path>` / `viewBox` binUI 还不支持，`<line>` 支持）。
- **整套尺寸改成奇数，才是真正把"半像素"从根上去掉**（这是这轮反复的最终结论）：
  - 原来 44 高标题栏 + 28×28 按钮 + 10×10 图标：按钮中线落在第 21.5 行（两行交界），1px 线
    无论放 21 还是 22 都差半像素，放 13.5 又会被 GL 渲成"半覆盖一行"（灰且偏高）；
  - 现在 **标题栏 45 + 按钮 29×29 + 图标 9×9**：按钮中线正好是第 14 行，横线就落在这一行；
    方框边框落在第 10 / 18 行，中心线也是第 14 行；左右各留 10 列。三者全部整数对齐。
  - `h` 默认 44 → 45、`bs` 默认 28 → 29（45-29)/2 = 8，按钮垂直位置仍是整数。
- 脚本量像素（`build-mingw/tmp/measure_titlebar_icons.py`：找按钮底色 bbox，再量图标墨迹四边留白）：
  GL 真机截屏三颗都是 `L10 R10 T14 B14`（最小化）/ `L10 R10 T10 B10`（最大化、关闭），`dx=dy=0`。
  GDI 离屏的关闭图标因为 `LineTo` 取整是 `L11 R9 T11 B10`（偏右下 1px），只影响静态文档图。
- 半透明叠加验证（`build-mingw/tmp/overlay_buttons.py`，三颗按钮 42% / 只叠图标 62%）：圆角底完全
  重合；横线正好穿过 ✕ 的交叉点；▢ 与 ✕ 的墨迹范围逐像素重合 —— 之前那条"偏高半像素"的横线
  在叠加图里能直接看出来，改完就没有了。
- 组件参数 `bsym` / `bsymmin`（字形字号）随之删除：图标几何写死在按钮的 29×29 坐标系里，
  `bs` 仍是按钮边长（**改 `bs` 时要同步改图标坐标**，文件里注释写了）。
- `test_css` 的 titlebar 用例重写：三颗按钮同宽高、按钮文案为空（不再有字形）、图标子节点存在
  且都不可交互、**三颗按钮中心用 `hitTest` 都命中按钮自己**（图标不挡点击）。
- 文档图按新标题栏重出：`flexshow` / `buttonshow` / `features` / `animshow` / `fontshow` /
  `std-components` 走离屏渲染；`svgdemo` / `cubedemo` / `texturedemo` 重新真机截屏。
- 验证：`gl_shot` 离屏 GL 渲染与真机窗口在未改动区域逐像素一致（maxdiff 0），所以半像素修正
  是拿真机同一个光栅器验的；相隔 1.6s 两帧标题栏 diff 最大 0（图标不再动）；悬停最大化按钮底色变
  `#DCE5F2` 且图标仍在最上层；点最大化窗口铺满 2560×1440、点关闭进程退出（点击路由没被
  图标挡住）；Tab 焦点环正常套在按钮上。
- ⚠ 组件在**应用启动时**才编译加载：改完 `titlebar.bum.xml` 要重启示例进程才能看到新像素
  （跑着的旧实例会一直是旧的 ✕ 和横线）。

### 修复 — 鼠标点输入框也画出"编辑中"蓝环

- `focusRingOf()` 原来只看 `edit_.active`：一旦进入编辑态就返回 `Editing`，不管编辑是键盘
  Enter 进来的还是鼠标点进来的。于是点一下输入框，周围立刻套一圈 2px 蓝框 —— 和"焦点框
  只代表键盘在操作"的设计矛盾。
- 现在区分来源：`focus(nodeId, viaKeyboard)`，只有 `activateKeyboardFocus()`（Enter）传
  `true` 才画编辑环；鼠标点击 / 宿主直接调 `focus(id)` 进入的编辑有光标、能打字、`:focus`
  样式照旧，但不画环。退出编辑时同源判断：键盘进来的回到虚线待编辑环，鼠标进来的不画环。
- 实测（fontshow 普通输入框，客户区 900×760）：鼠标点进去 0 个蓝像素（光标在）；
  Esc 退出后 0 个；同一框再按 Enter 进来 850 个 `RGB(62,139,255)` 像素、边框两像素宽 ——
  说明只是"鼠标进来的不画"，不是把编辑环关掉了。
- `test_elements` 9c 补 6 条：鼠标点击进编辑且有 `:focus` 但不画环、Esc 退出后仍不画、
  键盘重新 Enter 后编辑环保留。文档与 C ABI 注释同步（`docs/keyboard_focus.md` /
  `docs/bindings.md` / `README.md` / `include/binui_c.h`）。

### 文档 — 尚未实现的下潜/上浮方案改称「层级焦点导航」

- 原来叫 "z 序焦点"，容易和窗口 Z 序（`binui_win32_foreground.h` 里的置顶）混淆；它讲的其实是
  焦点在元素层级树里的下潜/上浮，和渲染 Z 序无关。
- 改名范围：`docs/keyboard_focus.md` 末节标题改为「未来开发计划：层级焦点导航（下潜 / 上浮，未实现）」，
  `docs/README.md` 索引同步；`[0.3.0]` 里那条记录也换成新名字（语义没变，仍未实现）。

### README 顶部的示例图换成当前示例；补 flexshow / buttonshow 截图

- 顶部三张图原来是「内置默认布局 / features / animshow」：第三张标着"组件化（`<use>`）"却放的
  动画截图，而"默认布局"只是引擎兜底布局（不是示例），观感像很久以前的东西。
- 换成 8 组示例里最有代表性的三张：**flexshow**（自适应布局）、**buttonshow**（按钮与状态样式）、
  **animshow**（动画与过渡），全部用 `tools/offscreen_render` 从**当前源码**重出。
- 新增 `docs/img/flexshow.png`、`docs/img/buttonshow.png`；顺手重出 animshow / 组件库预览；
  `docs/README.md` 图片表补上这两行，并加了"改完示例 UI 怎么重出这些图"的命令清单
  （`make tools` 后一张一条）。
- **3D / 纹理 / SVG 画布三张改成 GL 真机截屏**：cubedemo 的立方体、texturedemo 的程序化纹理、
  svgdemo 的 canvas 频谱都是应用在 `drawFn` 里逐帧画的，离屏渲染只能得到空画布 —— 之前
  `docs/img/cubedemo.png` 就是一张"画布空白"的图，README 顶部也补了第四格 3D 线框（真机截屏并注明）。
- flexshow 状态栏的占位文案从「窗口 —」改成默认窗口尺寸（运行时仍由 resize 事件覆盖），
  离屏图/截图不再出现"—"。
- 修默认布局（`GenerateDefaultBinUI()`）按钮文字变左对齐：手写节点漏了 `BINUI_TEXT_CENTER`，
  后端改成按 flags 画文字之后就露出来了；`test_css` 加一条守护（手写布局按钮也必须居中）。

## [0.3.0] - 2026-11-10

首个正式按 `MAJOR.MINOR.PATCH` 发布的版本（此前只有 0.0.1 / 0.0.2 两个归档）。
版本号规则见 [CONTRIBUTING.md](CONTRIBUTING.md) 的「版本号与发布」；二进制布局格式号仍独立递增（当前 v14）。

### 修宽输入框的虚线焦点环只画了一半；`strokeRounded` 真正支持线宽

- `flex:1` 拉满窗口的输入框（flexshow 的搜索框，800+px 宽）周长超过虚线段数上限，
  只画到左上→右上→右下就停了，表现为"只有上边和右边有虚线"。现在上限从 96 提到 256，
  且超限时按周长**等比拉长实线/间隔**（而不是画一半就停），任何宽度都整圈画完；
  单测覆盖 1200px 与 3000px 宽的环（`BuildDashedRoundRing`）。
- GL/D3D 的 `strokeRounded` 之前是 `(void)thickness` —— 参数被直接丢掉，所有描边都只有
  1px（"2px 编辑环"实际是 1px）。现在按厚度画多层同心圆角环，编辑环真的是 2px
  （真机取样：`RGB(62,139,255)` 连续两像素）；其它传 1px 的调用点行为不变。
- 验证：flexshow 默认窗口 Tab 到搜索框 → 虚线整圈；fontshow 点输入框 → 2px 蓝环；
  `make all` 0 警告，`make test` 四套全绿。

### 键盘焦点系统重做：WASD 移焦点、方向键滚页面、回车两段式、焦点环 2 秒自动消失

以前方向键在可交互节点间移动焦点、焦点环画上就一直在、8 个示例各自 `switch (vk)` 复制一份
编辑键处理（`features` 干脆没有编辑分支）。现在键盘输入只有一条入口
`renderer.handleKey(vk, mods)`，焦点 / 滚动 / 编辑的优先级由渲染器统一决定：

```cpp
app.setKeyFn([&](int vk) {
    uint32_t mods = 0;
    if (::GetKeyState(VK_SHIFT)   & 0x8000) mods |= BinUIRenderer::KEYMOD_SHIFT;
    if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= BinUIRenderer::KEYMOD_CTRL;
    if (::GetKeyState(VK_MENU)    & 0x8000) mods |= BinUIRenderer::KEYMOD_ALT;
    const auto kr = renderer.handleKey(vk, mods);
    for (uint16_t id : kr.changed) invalidateNode(id);
    if (!kr.handled && vk == VK_ESCAPE) app.requestExit();   // 库不消费 Esc
});
```

三种状态与按键：

| 状态 | 键 | 行为 |
|---|---|---|
| 焦点控制 | `WASD` | 上/左/下/右移动焦点（Tab 仍按文档顺序循环） |
| 焦点控制 | 方向键 | **滚页面**：离焦点最近的滚动容器；焦点不在滚动容器里时，滚视口内面积最大的那个 |
| 焦点控制 | PgUp/PgDn、Home/End | 按视口 0.9 倍翻页、到顶/到底 |
| 焦点控制 | Enter | `<input>` 进入文本编辑；`<video>` 播放/暂停；其余派发 `CLICK` |
| 焦点控制 | 空格 | 有焦点时同 Enter；没有焦点时向下翻页（浏览器语义） |
| 文本编辑 | Enter / Esc | 提交并退出编辑，**回到焦点控制** |
| 文本编辑 | Tab / Shift+Tab | 退出编辑并跳到下一个 / 上一个控件 |
| 文本编辑 | 退格/删除/左右/Home/End | 光标与选区（字母归输入法，**WASD 不抢键**） |

焦点环不再是"一条常亮白线"，而是把状态画出来（`renderer.focusRingOf(id)` →
`None/Dashed/Solid/Editing`）：

| 状态 | 环 | 说明 |
|---|---|---|
| 待编辑（**只有输入元素**） | **虚线** 1px + 底色对比色 | 键盘选中输入框、还没按 Enter；`:focus-visible` 语义：鼠标点击不画、2 秒无操作自动熄灭 |
| 普通焦点（按钮等） | **实线** 1px + 底色对比色 | 键盘选中非输入元素；同样 2 秒无操作自动熄灭 |
| 编辑中（输入元素） | **实线** 2px + 强调蓝 `#3E8BFF` | 正在编辑的输入框；常亮（编辑是持续状态，不能自己消失） |
| 无 | 不画 | 鼠标操作 / 超时 / 窗口失焦 / 没有焦点 |

虚线只用来标记输入元素的"待编辑"状态，不是通用焦点样式 —— 否则按钮上也套虚线，满屏看起来
像"所有东西都在等你编辑"，和禁用/占位之类的虚线语义也容易混。

线型 + 颜色双编码：只靠颜色区分对色弱用户不友好，虚线/实线一眼能分。虚线沿圆角矩形
路径按弧长采样（`binui_paint.h` 的 `BuildDashedRoundRing`，三个后端共用几何、只负责 fillLine）。

Enter/Esc 的语义也按状态分开、不混：待编辑态 Enter = 激活（输入框进编辑 / 其余 CLICK）；
编辑态 Enter = 确认（提交并退出，单行输入没有"换行"，要"发送"的宿主在 `INPUT`/`FOCUS`
事件里做）；编辑态 Esc = 退出编辑回到待编辑态。焦点环熄灭不需要宿主定时 ——
`stepAnimations()` 会在熄灭的那一帧把焦点节点放进返回值，宿主照常 `invalidateNode()` 擦掉。
窗口失焦时宿主调 `renderer.hideFocusRing()`。

#### 中文输入法：非编辑态把 IME 摘掉，否则 WASD 一个键都收不到

这一步不做的话上面的 WASD 在中文环境里等于没接。中文输入法默认挂在窗口上，按 `d` 时
Windows 会把 `WM_KEYDOWN` 的 `wParam` 换成 `VK_PROCESSKEY`（229）交给输入法组字。真机日志：

```
keydown vk=229 composing=0     ← 按 d，输入法把键吃掉
keydown vk=229 composing=1
```

于是外壳加 `setImeActiveFn`：返回 false 时把 IME 上下文从窗口上摘下来（`ImmAssociateContextEx(hwnd, nullptr, 0)`，
摘之前先 `cancelImeComposition()`），返回 true 再挂回默认上下文（组字/候选框/光标定位照旧）。
每帧在 `renderFrame()` 里同步，宿主只需一行：

```cpp
app.setImeActiveFn([&] { return renderer.isEditing(); });
```

8 个示例与 C ABI 都接了；不接 = 保持老行为（IME 常挂），此时中文输入法下 WASD 无效、方向键照常。
另外 `BINUI_IME_LOG=1` 的日志现在会打每一条 `keydown vk=...`，这类"键被谁吃了"的问题一眼能看出来。

组字中的 Esc 归输入法：实测微软拼音组字时按 Esc 只发 `WM_IME_ENDCOMPOSITION` 取消候选、
不再放行 `WM_KEYDOWN`，编辑态原样保留；对会放行 Esc 的输入法，外壳把"组字结束后 200ms 内"
到达的第一个 Esc 吞掉一次（`composeEndMs_`），再按一次才退出编辑 —— "Esc 取消拼音"和
"Esc 退出编辑"不会互相误伤。

实现要点：

- `handleKey` 返回 `{ handled, changed }`：`handled` 只表示"库消费了这个键"（宿主别再用它当
  快捷键），`changed` 是焦点环/光标/滚动容器的变化集合，宿主按它做局部重绘。
- 方向键的目标容器 `scrollTargetNode()`：先沿焦点节点向上找最近的 `overflow:auto/scroll`
  祖先；没有则取视口中心所在的可滚动容器；再没有取视口内面积最大者。只有真有两轴溢出的
  容器才算数，否则方向键不被消费（宿主可以自己用）。
- 鼠标点击只切 `:focus` 状态（`commitFocus(id, false)`），不点亮焦点环；环正亮着时点一下
  也会立刻收掉（`release()` 里清 `focusRing_`）。`setKeyboardFocus` / `moveKeyboardFocus` /
  `cycleKeyboardFocus` / `handleKey` 都会亮环并重置 2 秒计时。
- 每节点环态由 `focusRingOf(nodeId)` 决定（`None/Dashed/Solid/Editing`）：输入元素待编辑给
  `Dashed`、其余元素给 `Solid`、正在编辑的输入框给 `Editing`。三个后端只负责画：
  实线用 `strokeRounded(2px)` / 四条 2px 线，虚线用 `binui_paint.h` 的
  `BuildDashedRoundRing` 沿圆角路径按弧长采样出线段再 `fillLine`。编辑环颜色统一取
  `BinUIRenderer::kFocusRingEditColor`（#3E8BFF），不会三端各调一个蓝。
- 编辑键不再由宿主复制：`features` 的输入框以前按退格/方向键无效，现在和其它示例一致。
- C ABI 也切到同一条路径（剪贴板四件套仍留在宿主侧，因为系统剪贴板 I/O 在平台层）。

验收：

- `tests/test_elements.cpp` 新增 9c 节 35 条用例：WASD 移焦点、方向键不移动焦点且无滚动容器时
  不被消费、回车激活/进出编辑、编辑中 WASD 忽略、编辑中 Tab 换控件、方向键滚焦点容器、
  PgUp/PgDn/Home/End、无焦点时空格翻页、焦点环三态（输入框待编辑 `Dashed` / 按钮 `Solid` /
  编辑中 `Editing`，Esc 后回到 `Dashed`）、虚线环几何（线段数 / 端点不出界 / 覆盖周长）、环超时熄灭并回报该节点、
  鼠标点击只设焦点不亮环。
- `make all` 0 警告；`make test` 四套全绿；`check_examples` 11 个 UI 0 问题。
- 真机键盘验收（截图逐帧比对，均值像素差）：
  - `buttonshow`：WASD 后出现**虚线**焦点环 → 2.3 秒后消失 → 再按又出现；鼠标点按钮只出行状态、
    不亮环。
  - `flexshow`：连按 4 次方向键，右侧列表区差 19.1、左边图表区 0.0（焦点没动、页面只滚了列表）；
    再按 `a` 焦点环出现在第一颗可聚焦节点。
  - `fontshow`：点输入框进编辑 → 输入框上是**实线蓝环** + 光标 → 数字 “123” → 回车退出
    （环变虚线、光标消失）→ 按 `x` 无变化（编辑外的字母被忽略）→ 再回车进编辑 → 按 `4` 插入。
    组字 “ni'hao” 时按 Esc：只取消候选，编辑态与实线蓝环保留；再按一次 Esc 才回到虚线待编辑态。
    `BINUI_IME_LOG=1` 的日志同步显示 `ime detached` / `ime attached` 跟着编辑态切换。

层级焦点导航（非输入框按 Enter 下潜到子元素、多层可下沉，`Delete` 上浮）按 master 的要求
先记下设计，见 [`docs/keyboard_focus.md`](docs/keyboard_focus.md) 末节，本次未实现。

### 示例 — 标题栏的 − □ ✕ 三颗按钮全部接线，固定布局示例补 `ui_scale="proportional"`

`components/titlebar.bum.xml` 一直只有外观，动作归宿主；之前 8 个示例里：
`buttonshow` / `flexshow` 完全没接，其余几个是按**字形文本**匹配（`n.text == "−"`）接的 —— 改个字号、
换成别的图标就悄悄失效，而且它挂在 release 回调里，和 CLICK 各走一半。现在统一改成**按组件声明的
id** 接（`idmin`/`idmax`/`idclose` 默认 `win-min`/`win-max`/`win-close`）：

```cpp
const uint16_t idWinMin   = renderer.findNodeByName("win-min");
const uint16_t idWinMax   = renderer.findNodeByName("win-max");
const uint16_t idWinClose = renderer.findNodeByName("win-close");
renderer.setEventCallback([&](const Event& ev) {
    if (ev.type != EventType::CLICK) return;
    if (ev.nodeId == idWinMin)   { app.minimize(); return; }
    if (ev.nodeId == idWinMax)   { app.maximize(); return; }   // 最大化 / 还原
    if (ev.nodeId == idWinClose) { app.requestExit(); return; }
});
```

顺带解决"最大化之后缩在左上角"：固定像素布局的 7 个示例加 `ui_scale="proportional"`，
按 □ 最大化时整棵节点树按「新窗口 ÷ 设计尺寸」缩放，而不是留一片空白。`window_resize="disable"`
仍然生效（不能拖边），两者不冲突。

验收（真机，逐个示例点按钮）：

| 示例 | − 最小化 | □ 最大化 / 还原 | ✕ 关闭 |
|---|---|---|---|
| animshow / buttonshow / cubedemo / texturedemo | iconic=True | 2560×1440 → 原尺寸 | 进程退出 |
| features（900×932，纵横比不同） | iconic=True | 2560×1440 → 原尺寸（x/y 分别缩放） | 进程退出 |
| fontshow / svgdemo / flexshow | iconic=True | 2560×1440 → 原尺寸 | 进程退出 |

`make all` 0 警告；`check_examples` 11 个 UI 0 问题。

### 修复 — column flex 里 `flex:1` 的"撑满剩余高度"塌成内容高

`<div style="display:flex;flex-direction:column">` 里写 `flex:1` 的子容器，高度**位置**按剩余空间
算对了（`720−44−48−28=600`），节点**高度**却被它自己的"未声明高度 → 按内容撑开"逻辑改回内容高：

```
id=15 #main   y=92   h=40     ← flex:1 的列内项，高度没被撑开
id=23 #status y=692           ← 但排位置时按 #main=600 算
```

于是"撑满剩余高度"的容器塌成一条、后面的兄弟压上来（重叠），`flex:1 + overflow:auto` 的滚动视口
也变成 0 高（永远不出滚动条）。修法：column 父级下的 `flex:N` 主项，高度由父级分配，**不许**再用
内容高度覆盖（编译器与渲染器 `relayout` 两处同口径；`autoH` 也一并置 0，否则运行期重排又会塌回去）。

顺带修了一条同源问题：**视口 = 窗口**。重排后根节点不再被内容撑大（以前内容比窗口高时根会"长"出
窗口，脏矩形/命中/绘制全落到窗口外，自适应布局一缩窗口就错位）。

回归用例（`tests/test_css.cpp`）：`flex column fill`（列内 flex:1 容器 = 528、嵌套 flex:1 = 528、
兄弟位置跟随、`setSize(400,400)` 后 = 328）+ `flex scroll viewport`（`flex:1 + overflow:auto`
视口高 = 剩余 260 且带 `BINUI_SCROLLABLE`）。

### 示例 — 7 个固定布局示例固定宽高 + 新增自适应布局示例 `examples/flexshow`

- 固定像素布局的 7 个示例统一在 `<app_window>` 上写 `window_resize="disable"`：以前窗口能拖边，
  一拖就错位（绝对定位不会跟着重排）。
- 新增 `examples/flexshow`：整份 UI **没有一个绝对定位**，全靠百分比 / `flex:1` / `min·max-width` /
  `width:content` / `flex:1 + overflow:auto` 撑开；状态栏由 C++ 在 `RESIZE` 事件里写窗口尺寸，
  一眼看出"重排用的是新尺寸"。默认 1100×720，实测 820×560 / 1100×720 / 1400×900 三档都正确重排
  （卡片宽度、柱状图百分比高度、列表滚动视口全部跟随，窗口缩小到 820 时第三张卡被 `min-width:240` 钳住）。
- 顺带补了这个示例的**滚轮接线**：`overflow:auto` 只负责画滚动条与裁剪，滚轮要外壳自己
  `renderer.scrollAt(x, y, dy)`（拖条子走的是 press/setHover 那条路，滚轮是另一条）。
  第一版漏了它，表现就是"滚动条画出来了、滚轮和拖条都不动"。

### 性能 — 拖窗口实时重排：按刷新率出帧、动画不停、空闲不再吃满单核

- **拖拽期动画不再冻结**：resize 循环以前只调 `renderOnce()`，`update_` 被跳过，拖边时所有动画停住；
  现在每帧都走 `renderFrame()`（`update_ + 重排 + 呈现`）。
- **按显示器刷新率出帧**：以前拖拽循环是"每轮重排 + 整屏呈现 + `Sleep(1)`"，一秒几百帧、CPU 打满、
  消息队列堆在后面，反而一顿一顿；现在用 `GetDeviceCaps(VREFRESH)` 定额（拿不到按 60Hz），
  边框 `SetWindowPos` 仍立刻跟手，内容按刷新率出帧。GL 与 D3D11 两个外壳同构。
- **空闲节流**：`pump()` 以前只要有脏区就满速出帧（一个有常驻小动画的界面能吃掉 ~80% 单核），
  现在按同一套帧时钟等到下一帧点再出帧、其余时间睡在 `MsgWaitForMultipleObjectsEx` 上。
  实测 `buttonshow`（标题栏图标常驻呼吸动画）空闲 CPU **82% → ~12%**，6 秒内 413 帧（≈69fps，含启动）。
- GDI 呈现路径把 `StretchDIBits` 换成 1:1 的 `SetDIBitsToDevice`（不做缩放采样）。
- 新增 `BINUI_PERF=1`：每 600 帧 + 退出时打印分段耗时（重排 / 绘制 / 呈现，呈现再拆
  解析 / DIB / 读回 / 贴屏），拖拽结束打印本次拖拽的帧数与平均/最大帧耗时。

### 示例 — `buttonshow` 补上标准标题栏，7 个示例现在外观统一

之前 6 个示例（features / animshow / fontshow / svgdemo / cubedemo / texturedemo）都用
`components/titlebar.bum.xml`，只有 `buttonshow` 自己拿两个 `<span>`（24px 大标题 + 副标题 +
右上角一颗 chip）拼了个"头部"：窗口没有可拖动的标题栏、右上角没有窗口按钮，和其余示例摆在一起
一眼就看出不是一套东西（`examples/README.md` 里"都用标准标题栏"那句当时是假的）。

改成和别的示例同构：

```html
<use src="../../components/titlebar.bum.xml" x="0" y="0" w="1020" icon="▣" iconw="22"
     title="按钮样式一览" sub="语义 / 尺寸 / 圆角 / 底色 / 状态 / 形态 —— 全部是 CSS，没有一行绘制代码"/>
```

- 原来那个 24px 大标题与副标题被标题栏的 `title` / `sub` 取代，`.title` / `.sub` / `.chip`
  三条样式随之删掉（不留死样式）；chip 里的"主色 #3E8BFF"由副标题覆盖，信息没丢。
- 内容整体上移 28px：卡片序列从 `84 / 192 / 300 / 476 / 618` 变成 `56 / 164 / 272 / 448 / 590`，
  首块距标题栏 12px、末块距底 20px，窗口高度 712 → 684（和其他示例"标题栏 44 + 内容从 56 起"一致）。

验收：

- 离屏渲染：把改前 `y=84..692` 与改后 `y=56..664` 两块裁出来逐像素比对，**完全一致**
  （说明只是整体位移，没有别的重排）；标题栏条带与其余 6 个示例逐像素同规格
  （白底、`y=43` 处 `#E6EDF7` 分割线、右端 `#FFE9EC` 关闭钮）。
- 真机 GL：标题栏可见；从标题栏按下拖动，窗口矩形 `(770,358) → (870,398)`，位移正好等于鼠标
  位移 `(+100,+40)`；悬停最小化钮，底色 `#EEF3FA → #DCE5F2`（走 `.bui-tb-btn:hover`）；
  点「主操作」按钮，状态行更新为"最后点击：主操作 / 点击次数 1"。
- `check_examples` 10 个 UI 0 问题；`build-mingw/bin/examples/` 下 7 份 main.html 与源树哈希一致。

### 重构 — `examples/features` 的按钮组件去掉多余的根 div

`examples/features/button.bum.xml` 以前写成 `<div class="cmp-wrap"><button …></div>`：这层 div
与按钮完全重合（宿主 `<use x y w>` 落到它身上，按钮再 absolute 定位到 0,0），除了给按钮
多一个定位基准与一个祖先之外什么都不做。它还带来一个隐性瑕疵——宿主传的 `bg` 参数会作为属性
落到 div 上，div 画出一个**方角**底色块，盖住按钮的圆角（离屏渲染里那颗绿按钮四角是方的）。

改成**组件根就是 button**：位置/宽度由宿主 `<use x=… y=… w=…>` 给，参数只写内联 style。
`docs/components.md` 的示例同步，并补一句"别为了有个容器再套一层 div"。

顺带把 Makefile 里 features 的规则补上 `examples/features/button.bum.xml` 依赖 —— 以前只认
`main.html`，改组件不会重新拷到 `bin/examples/features/`，跑起来还是旧组件。

验收：8 个示例离屏渲染逐像素比对，只有那颗 `bg` 按钮的四个角变了（方角 → 圆角），其余全同；
`check_examples` 10 个 UI 0 问题；`make test` 四套全绿。

### 修复 — 组件里的按钮 hover 放大时被"关在盒子里"（裁剪链按每个祖先矩形无边裁）

组件自己的根 **`div`** 与按钮**等大**（`examples/features/button.bum.xml` 里就写着
`<div class="cmp-wrap" style="height:40">` 包着按钮，`<use>` 展开后根 div 就是
190×40，和按钮一模一样）。三个后端绘制节点时，
除了自身与脏矩形之外，还会**无条件**沿祖先链逐层裁剪：

```cpp
while (draw) { ... gfx.pushClip(par.x, par.y, par.width, par.height); }   // 以前
```

于是组件按钮 `:hover { transform:scale(1.04) }` 放大出来的那圈（每边 3.8px）被这个等大的根 div
切掉：按钮看着还是原尺寸、四角变成硬边 —— 就是"被困在一个容器里放大，超出的部分被隐藏"。

CSS 语义里只有 `overflow:auto/scroll/hidden` 的容器才裁子元素。修法：

- 渲染器新增 `clippingAncestors(nodeId, out)`：从近到远返回**带 `BINUI_SCROLLABLE`** 的祖先
  （编译期由 `overflow:auto/scroll/hidden` 标出），普通 div（含组件根 div）不入链。
- 三个后端（GL、D3D11、GDI）的绘制循环都改成调它，删掉各自手写的祖先遍历；
  每帧复用同一个 `std::vector<uint16_t>` 缓冲，不产生逐节点分配。
- 祖先不可见仍然跳过绘制（`hidden[]` 预计算已经覆盖，这里只补裁剪容器自身的可见性判断）。

验收：

- 真机：悬停「字体与输入法 / 组件化」卡片里的「普通按钮」—— 像素量出按钮从 190px 变成
  195–197px（`190 × 1.04 = 197.6`，两侧各带 1px AA），圆角完好；修复前恒为 190px 且是硬边。
- 静态回归：8 个示例 + 标准组件预览的离屏渲染，改造前后**逐像素零差异**（说明没有哪份布局
  是靠"祖先裁剪"苟着的）。
- `tests/test_elements.cpp` 新增 `suite_clip_chain`：`overflow:hidden` 的祖先必须在链上，
  普通 div 祖先必须不在链上。三个后端现在共用同一个判定，这条用例钉住的就是后端的行为口径。
- `make all` 0 警告，`make test` 四套全绿，D3D11 后端 `-fsyntax-only` 通过。

### 修复 — 按钮 hover 放大再缩回，边缘留下一道 1px 残影

按钮没写 `:hover` 样式时，引擎给的内置悬停效果是**放大 1.05 倍 + 提亮 12%**（`refreshNodeStyle`），
而且**没有过渡**：缩放一帧跳上去、一帧跳回来。缩回那一帧的失效矩形按"当前值"算正好等于布局矩形，
放大时在最外沿画过的像素就没人擦 —— 在新布局里表现为「按钮文字居中」这种**宽按钮**右边留一道
1px 竖线（152px 宽放大 1.05 会多出 3.8px，超过 `invalidRect` 的 `pad = 2`，所以只有宽按钮露馅；
窄按钮的 1px 溢出被 pad 顺手盖住了，这也是之前一直没发现的原因）。

根因有两层，缺一不可：

- **没有"扫过的极值"**：`invalidRect()` 只并当前缩放/位移的包围盒。新增 `animExtent_`
  （每节点记缩放 min/max、位移 min/max），`computeAnimResult()` 每帧更新，`invalidRect()`
  一并并进去 —— 缩回那一帧就仍然覆盖放大时画过的范围。布局重载时清空。
- **内置悬停的缩放进不了 `AnimResult`**：`drawScale()` 有"无过渡时回落到 `floatTargets`"的兜底，
  但 `computeAnimResult()` 没做同样的折入，所以这种"无过渡"的缩放从来没被记录过，
  上面那个极值也就永远是空的。现在 `computeAnimResult()` 末尾按同样的口径兜底
  （scale / translate），绘制结果不变，但极值能记上了。

回归用例：`tests/test_elements.cpp` 新增 `suite_hover_dirty` —— 200px 宽按钮、悬停后移开，
断言"缩回那一帧的失效矩形仍覆盖放大过的范围"。**这条用例在打补丁前是红的**（就是上面那条 FAIL），
打完变绿。

真机验收（`字体与输入法` 示例，像素比对）：光标移开后再截图，故按钮右侧 868~879 列与启动时
**逐像素相同（差异 0）**；补丁前同一位置差 19 个像素，即那道 1px 竖线。

### 重构 — `imeshow` 并进 `fontshow`：示例收成「字体与输入法」一组

输入法和字体共用同一条度量链（组字字体跟字号、光标高度跟行高），分成两个 demo 反而要来回切窗口看，
而且 `imeshow` 是全仓库唯一的深色底示例，和"统一浅色主题"的约定本来就打架。

- `examples/fontshow` 扩成**字体与输入法**：①字号阶梯 ②字重/斜体/装饰 ③行高 ④混排与符号不变，
  ⑤ 换成输入法区（24px 大号输入框 + 13px 普通输入框 + 上屏回显 + 码点/字符数统计），
  窗口 900×640 → 900×760。
- 外壳并入 `imeshow` 的 IME 接线：`setImeCaretFn`（组字窗与候选框贴光标）、`setCharFn`
  （只有选词上屏的字符才进来）、组字期间按键不抢、编辑键（退格/方向/Home/End/回车/Tab）。
  另外补了热重载后的节点 id 重解析（换树后 `findNodeByName` 要重跑，否则回显会写到空气里）。
- 删除 `examples/imeshow/`；`Makefile` / `CMakeLists.txt` 的示例列表 8 -> 7。
- 文档：README 示例表与运行清单、`examples/README.md`、`docs/README.md`（图片说明）、
  `docs/app_layout.md`、`tests/README.md`（IME 手测入口改到 `examples/fontshow`）同步；
  `docs/img/fontshow.png` 按新布局重新生成。

验收（真机 + `BINUI_IME_LOG=1`）：

- 点 24px 输入框打 `nihao`：组字串 `n` 内联在光标处、候选框（1 你 2 呢 3 那…）贴在输入框下方；
  日志 `ime place (45,600) h=32 reset=1` —— 光标高 32 = 24px 字号的行高，与节点几何吻合。
- 空格选词：`[fontshow] char: 你  U+4F60`（`resultstr bytes=2 cps=1`），输入框变「你」，
  回显行显示"最后收到的字符：你 / 码点：U+4F60（3 字节 UTF-8） / 输入框① 3 字符 · 输入框② 0 字符"。

### 重构 — 测试与示例收拢：9 个测试二进制合成 4 个，产物按用途分目录

一根鸡毛一个 `test_xxx.exe` 的老问题积到现在：`tests/` 里躺着 9 个互不相干的可执行文件，
`build-mingw/bin` 里测试、示例、工具、以及几份**既没有源码也没有构建规则**的残留产物
（`galgamechat` / `cssshow` / `singlewindow` / `multiwindow` / `videodemo` / `minwin` /
`ime_probe` …）混作一团，双击有的还打不开。

- `tests/` 改成**按主题**分四个套件，代码逐字保留（只做结构重排：每个旧文件的 `main()` 包进
  `namespace suite_xxx` 并改名为 `run()`，新 TU 的 `main()` 逐个调用并汇总失败数）：
  - `test_css` ← 原 `test_compiler`（CSS 属性 / 选择器 / 伪元素 / 编译器告警）
  - `test_elements` ← 原 `test_binui` + `test_renderer` + `test_renderer_real` + `test_paint`
    + `test_anim` + `test_hot_reload`（元素 / 布局 / 命中 / 交互 / 绘制几何 / 动画 / 热重载）
  - `test_font` ← 原 `test_font`，并补上**输入法（IME）**用例
  - `test_capi` 不变（C ABI 契约）
  合并时用输出逐条比对确认用例没丢：元素组 359 条、CSS 组 373 条，两侧完全一致。
- `test_font` 新增 IME 桥接用例：UTF-16 索引 → UTF-8 光标字节偏移、全角字、代理对（emoji）、
  上屏插入（`insertText`）、失焦提交（`blur`）、`setCaret` 夹到字符边界。这段不依赖字体，
  没有系统字体的环境也会跑（以前整支测试会直接 skip 掉）。
- **修 `Utf16IndexToBytes()` 把 UTF-16 代码单元当成码点数**
  （`src/renderer/binui_renderer.cpp`）：补充平面字符（emoji、罕见汉字）在 UTF-16 里是
  **代理对、占 2 个单元**，旧实现按 1 个算 —— 输入法给的 `GCS_CURSORPOS` 索引在含 emoji 的
  输入框里逐字符偏一位，索引还可能落进代理对中间，组字窗就挂在半个 emoji 的位置上。
  现在按单元推进，落在代理对中间时夹到该字符起点（鼠标定位、双击选词走的是同一个转换）。
- 产物按用途分目录，Makefile 与 CMake 保持一致：
  - `build-mingw/bin/tests/` —— 四个套件
  - `build-mingw/bin/examples/` —— 8 组示例（exe + 同名 UI 子目录）
  - `build-mingw/bin/` —— 库 / DLL / 命令行工具
  示例 UI 里的 `../../components/xxx.bum.xml` 相应同步到 `bin/components/`；另加 `make examples`
  只编示例、`make test` 只编跑测试。
- 删掉 7 个旧测试源文件、`build-mingw/bin` 里的残留产物，以及 tests/README 里说明的边界
  （哪些必须开窗手测：组字窗/候选框定位、组字期间点击被 IME 吞掉、GL/D3D 真实像素）。
- 文档同步：新增 [`tests/README.md`](tests/README.md)（套件划分 + 能测/不能测 + 合并记录）；
  `README.md` / `examples/README.md`（6 组 → 8 组，路径改到 `bin/examples/`）/
  `tools/README.md` / `docs/{README,app_layout,components,pixels/README}.md` 里的失效路径一并修正
  （`examples/cssshow`、`examples/singlewindow`、`examples/components` 这些早就搬走/删掉的老路径）。

### 修复 — 圆角盒子的边框只画到顶部约 64px 就断了

`BuildBorderBands()`（`include/binui_paint.h`，GDI / OpenGL / D3D11 三个后端共用）的圆角分支
逐行生成边框条带，每行左右各 push 一条；合并逻辑只看 `out[n-1]`，而左右两条是**交替** push 的，
永远合不上 —— 条带数退化成 O(高度)，撞到 `kMaxPaintBands`（128）就在第 64 行截断。

表现：所有带 `border` 的圆角盒子，左边框/右边框只有**上半截**，下面整段没有；截断位置与盒子
高度无关（圆角 14 的卡片实测 52 行，加上顶部圆角恰好 64），所以看起来像"固定长度的一条 1px 线"，
极易误判成刻意画的装饰线。三个后端 + 所有示例都中招。

- 合并改成**按边记住上一次的条带下标**（上/下/左/右各一个），直边段逐行延伸成一条高条 ——
  条带数只跟半径有关（≈4r+常数），与盒子高度无关。
- 边框条带上限单独提为 `kMaxBorderBands = 512`（半径大的胶囊也装得下）；装不下时**退回直角
  四条**，宁可方角也不能整段缺边。
- `tests/test_elements.cpp` 的绘制几何小节补回归用例：600px 高卡片的边框必须画到底边、盒子拉高一倍条带数不变、
  半径超过上限时退回四条、直角仍是四条。
- 顺带修了圆角**转角比直边更粗更亮**的问题：1px 等宽边框的圆角以前是逐行矩形拼的（45° 处每行
  1.4~2px、只有亚像素混色没有 AA），而紧挨着的填充是 AA 的，转角看起来发虚发粗。现在这种边框的
  圆角改走九宫格 AA 圆环（模板覆盖率也修成了满值，直边颜色和作者写的一致），直边不再需要额外补矩形。
  GL 与 D3D11 一致；GDI 画布后端没有圆环原语，仍走逐行条带（已经不截断）。
- **焦点环画成了灰线**：圆环模板以前是"骑在外侧边界上"画的，靠内那半个像素只有 50% 覆盖率 ——
  文档写的是"白色焦点环"，实际渲染出来是 #858990 的灰线，浅色主题上（白底白环）完全看不见。
  现在环整体挪到边界内侧满覆盖，白环就是纯白 1px；颜色再按节点底色取对比色（浅底画深灰
  #1A1A1A），浅色主题也看得见。GL 与 D3D11 一致。

### 新增 — 按钮样式示例（`examples/buttonshow`）

按钮能力以前散在 features 里，没有一个"照着抄就行"的集中示例。新增 buttonshow，**纯 CSS**
（外壳只接事件，没有任何绘制代码），六组摆在一起：

- 语义色（主操作 / 次要 / 成功 / 警告 / 危险 / 信息 / 链接）、尺寸（11/12/14px）、
  圆角（2/9/16/胶囊）、底色（纯色 / 渐变 / 描边 / 幽灵）、状态、形态（图标 / 纯文本 / 块级整行）。
- 运行时状态全部演示到位：`:hover`（提亮 + `transform:scale`）、`:active`（压暗 + 回弹）、
  `:checked`（外壳 `setNodeChecked` 维护）、`disabled`（引擎压暗 55% 并退出 Tab 链），
  并在页面上标注真实优先级 `:active > :focus > :checked > :hover`。
- 底部状态栏接 CLICK 事件（鼠标与键盘 Enter/Space 同一条路径），显示"最后点击了哪个按钮 +
  点击次数"；Tab / 方向键移动焦点、焦点环可直接验。
- 浅色主题，与 features 同一套令牌；`Makefile` / `CMakeLists.txt` 已接线。
- README 示例表补上 buttonshow 与 imeshow（之前漏了），并更正"6 组示例 / 全部浅色"的说法。

### 修复 — IME 组字窗/候选框停在旧位置，打字时屏幕上什么都没有

自绘窗口（Cicero-unaware）上，输入法的组字窗和候选框位置**只有第一次生效**：
`ImmSetCompositionWindow` / `ImmSetCandidateWindow` 之后一律返回 TRUE 却不理。
后果是两层：

- 宿主没接 `setImeCaretFn` 时，输入法拿不到光标，组字窗落到**窗口外**
  （实测窗口在屏幕 (900,376)，组字窗跑到 (1030,151)）。现象是"打拼音屏幕上什么
  都没有"，但选词照样上屏，极易误判成输入法没工作。
- 接了钩子也不够：从第一个输入框切到第二个输入框、或在同一个框里接着打下一个词，
  组字串画对了、候选框还赖在旧位置。

修法：

- 新增 `imeCaretFromRenderer()`（`backends/binui_gl-gdi.h`）：按渲染器编辑态算光标
  客户区坐标与行高，算式与 `drawNodesGLGdi` 里 BINUI_INPUT 的绘制同源
  （`padding+4` + `textWidth` + 同一行高），不会差半格。C++ 宿主一行接上：
  `app.setImeCaretFn([&](int& x, int& y, int& h){ return imeCaretFromRenderer(renderer, app.gfx(), x, y, h); });`
- 新增 `BinUIAppGLGdi::applyImePlacement()`：组字开始时**重挂一次 IME 上下文**
  （`ImmAssociateContext(hwnd, nullptr)` 再挂回），强制输入法丢掉旧 UI、重新采纳
  位置；位置没变就跳过（每次按键都重设会让输入法 UI 闪）。
- 新增公开方法 `BinUIAppGLGdi::cancelImeComposition()`，`WM_KILLFOCUS` 改用它
  （同时让位置缓存作废，回来时重新摆）。
- `WM_IME_COMPOSITION` 拿到 `GCS_RESULTSTR` 后**仍然交给 DefWindowProc**：
  早期直接 `return 0`，但它要看这条消息才能收尾；结果串已自己插过，随后生成的
  逐字节 `WM_IME_CHAR` 会被本库吞掉，不会重复上屏。
- `examples/imeshow`（新增示例）补上 `setImeCaretFn` —— 之前漏了，正好是"打拼音
  看不见任何东西"的复现。

已知限制（不是本库能修的）：组字期间鼠标点击会被输入法吃掉（它先把拼音当结果串
提交，点击不送到窗口，连点"退出"按钮都没反应）。要让点击不被吃，得给窗口实现
TSF 文本存储（`ITextStoreACP`）。

### 修复 — 字形图集满了会静默丢字

`rasterGlyph()` / `rasterColorCluster()` 里 `if (a->next >= total) return;` —— 图集满了
新字形**直接不出现**，而且一句日志都没有。表现是"某些字时有时无"，最难查的一类。
单张图集 1024²，按 ~1.4×字号切格，一个字号 + 字体面约 2600 格 —— 中文界面跑久了
（聊天记录、动态列表）真的会撞到。

- **图集分代**：同一个 key（字号 × 字重 × 字体面 × 斜体）现在可以有多张。
  `getAtlasForCp()` 优先返回"已经含有该字形"的那一代（UV 必须和纹理对得上），
  没有就找还有空格的，都满了再开新一代（上限 8 代）。**不再丢字**。
- 绘制路径（`drawText` 的灰度分支）改用按码点取图集；测量路径保持取主图集
  （只要 advance，不碰纹理）。
- 真的到 8 代还满（说明字号/字重组合失控）时，两处仍保留"只喊一次"的 stderr 警告。
- 彩色图集（emoji）暂未分代，满时告警并退回灰度。
- 验证：改动前后同一次 `gl_shot` 抓图**逐像素 0 差异**（320×240，probe.html）。

### 变更 — XML 格式（`<binui>` / `<root>`）正式降级为 legacy

`CompileXML` 路径没有 `<style>` 级联、不认 `<use>` 组件、也没有 v13/v14 的扩展绘制段，
但一直没有任何提示。现在编译 XML 会得到一条警告，建议改写为 HTML。
（`examples/features/*.bum.xml` 这类组件片段走的是 HTML 解析，不受影响。）

### 新增 — 输入法候选窗定位（IME）

以前完全没处理 `WM_IME_*`：中文能上字（`WM_CHAR` 会到），但**候选窗不受控**，
基本飘在屏幕角落；组字期间也没有下划线。

- GL 外壳处理 `WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION`：
  `ImmSetCompositionWindow(CFS_POINT)` 把组字窗口摆到光标处，
  `ImmSetCandidateWindow(CFS_CANDIDATEPOS)` 把候选窗列在光标下方一行。
  定位完继续交给 `DefWindowProc` —— 组字内容本身是系统画的，不抢。
- C ABI 提供 `setImeCaretFn`：按"输入框左内边距 + 光标前缀文本宽度"算出光标客户区坐标，
  字体度量复用 GL 后端的 `textWidth` / `fontMetrics`（与绘制同一套，不会错位）。
- 构建加 `-limm32`（Makefile 的 `GL_LIBS` 与 CMake 的 `gl_shell`）。

### 新增 — 鼠标拖选与双击选词

以前文本选区只能靠 Shift+方向键，鼠标用户会以为输入框坏了。

- 渲染器：`setCaretByUtf16Index(idx, extend)` 多了 extend 参数 —— extend 时只挪 caret、
  保留选区起点（按下时的锚点），这就是拖选；`selectWordByUtf16Index()` 按 UTF-16 索引
  选中所在的"词"（ASCII 字母数字下划线 / 连续非 ASCII 当一个词；点在空白或标点上时
  退化成"连到该处的那段非空白"）。
- 后端：`placeCaretAtClick*()` 多了 extend 参数并返回换算出的 UTF-16 索引（双击复用
  同一次换算，不会因为两次测量的舍入差半格）。
- 窗口外壳：注册窗口类加 `CS_DBLCLKS`（没有它系统根本不发 `WM_LBUTTONDBLCLK`），
  新增 `WM_LBUTTONDBLCLK` → `setDoubleClickFn`；C ABI 里把它接到"选词"，
  并在 `setMoveFn` 里检测左键按下 → 扩展选区。

### 新增 — 剪贴板：Ctrl+C / X / V / A

以前输入框能打字但不能复制粘贴 —— 等于半残。

- 渲染器补三个"文本层动作"：`selectedText()` / `deleteSelection()` / `selectAll()`
  （只读框可以全选和复制，但删不掉内容）。
- C ABI 的按键处理接上 Win32 剪贴板（`CF_UNICODETEXT`），UTF-8 ↔ UTF-16 往返；
  粘贴时去掉 `\r\n`（输入框是单行，换行粘进来会画到框外）；剪贴板被别的程序占着
  时静默放弃（粘贴不成功比崩掉好）。
- 只在 C ABI 这一层做：核心库保持平台无关，`examples/` 里的演示用自己的按键处理，
  没接这个（它们不是产品面）。

### 修复 — IME 组字结果根本不进输入框（"打了字什么都没出来"）

上一版只做了候选窗定位，仍然输不进中文。根因是**我们不是 EDIT 控件**：组字结果串
（`GCS_RESULTSTR`）必须由窗口自己从 IME 上下文取出来插进文本，`DefWindowProc` 不会替
应用做这件事。另外默认的 `WM_IME_SETCONTEXT` 行为是"应用程序自己画组字窗口"—— 系统
以为我们会画，于是它也不画，表现就是打字后屏幕上什么都没有。

- `WM_IME_SETCONTEXT`：清掉 `ISC_SHOWUICOMPOSITIONWINDOW` 再交给 `DefWindowProc`，
  让输入法自己画组字窗口。
- `WM_IME_COMPOSITION`：带 `GCS_RESULTSTR` 时用 `ImmGetCompositionStringW` 取结果串，
  UTF-8 转好交给宿主插入（新增 `setImeResultFn`），并返回 0 防止重复。
- `WM_IME_CHAR`：结果串已插入，这条**必须吞掉**，否则 `DefWindowProc` 会把它转成
  `WM_CHAR`，同一个字进两次。
- `WM_IME_REQUEST` / `IMR_CANDIDATEWINDOW`：新式 IME 走这条问候选窗位置，比
  `CANDIDATEFORM` 可靠。
- 顺带设置组字字体高度跟随输入框行高 —— 不设的话系统用默认 UI 字体，13px 输入框里会
  冒出一行明显更大的组字文本，看着像"位置不对"。

### 新增 — 光标形状（I 型 / 手型 / 拖动箭头）

整个库以前只加载过 `IDC_ARROW`：文本框里不会变 I 型、按钮上没有手型 —— 用起来第一眼
就"不像原生控件"。

- 渲染器新增 `cursorAt(x, y)`，按光标下是什么节点给形状：输入框 → I 型、可交互节点
  （按钮等）→ 手型、滚动条边缘 → 竖直/水平箭头、其余（含禁用节点）→ 箭头。
- GL 与 D3D11 两个窗口外壳加 `WM_SETCURSOR` 处理（非客户区仍交回系统）。
- 顺带修了 `hitScrollBar()` 的水平分支：它拿 `contentHeight` 判断水平条，应该用
  `contentWidth`。

### 修复 — 收尾三项：跨线程 destroy 改为拒绝、圆角模板改 LRU、水平滚动条

- `binui_app_destroy` 在非建窗线程上**拒绝执行**（以前只打警告再硬做，仍会崩）：
  跨线程拆 Win32/WGL 是未定义行为，现在的取舍是"泄漏窗口 + GL 上下文，进程退出时
  一起回收"，并把原因写进 `binui_last_error`。App 句柄不会失效，宿主可以直接
  `os.Exit`。
- 圆角模板缓存满了改成 **LRU**（以前淘汰"最大的一张"，而大的那张往往正是最热的主
  卡片圆角）。
- 滚动条补上**水平方向**：内容比容器宽时在底边画水平条，并给内容的底部留 gutter。
  竖直/水平两条在右下角不重叠（水平轨道扣掉竖直条那一格）。GL / D3D11 / GDI 三端一致。
  渲染器顺带补了 `contentWidth()`（`contentHeight()` 的横向对偶，`scrollTo` 本来就
  按它钳制）。

### 修复 — 圆角半径到"短边一半"就退化成直角（胶囊 / 正圆画不出来，GL 与 D3D11）

九宫格拼接把角片边长写死成 `2r+1`（模板 4r×4r，形状内缩 1px）。角片比四分之一圆弧
本身需要的 `r` 大一倍多，于是 `2×角片` 很快超过盒子短边：两个角片互相重叠（四边中部
露出凹口），中段宽高转负（整块被角片自带的弧裁掉）—— `border-radius:999` 的圆形头像、
胶囊按钮、`<svg><circle>` 全都命中，半径到短边一半时直接变**直角**。

- 几何统一抽到 `include/binui_paint.h` 的 `BuildRoundedNineSlice()`：角片边长 = `r`
  （四分之一圆弧的包围盒正好是 r×r），模板改成"形状铺满 [0,t]"（不再内缩 1px），
  模板边长从 `4r` 降到 `2r+4`（r=200 时 2.5MB → 653KB）。
- 半径钳到 `min(w,h)/2` 后九块**精确铺满**：`2r == 短边` 时中段宽高为 0，四个 1/4 圆
  正好拼成正圆 / 胶囊。
- `ensureRasterDC()` 的返回值以前没检查（失败后按新尺寸 memset 会踩坏堆），现在检查；
  圆角模板缓存加了上限（超过 48 张淘汰最大的一张）。
- GL 与 D3D11 两个后端同步；GDI 走 `RoundRect` 本来就是对的。

### 修复 — `binui_app_destroy()` 在窗口仍打开时可能崩（**结论有更正**）

> 后续用原宿主复验发现：这条崩溃**不是库的缺陷**，是宿主跨线程调用（Go 的 main
> goroutine 没 `runtime.LockOSThread()` 会迁移，`DestroyWindow` / `gfx_.shutdown()`
> 落到非建窗线程）。旧代码 + 宿主锁线程 16/16 不崩。详见 `docs/issues/02` 的
> 「复验」一节与 `docs/issues/08`。下面的加固保留（顺序确实更稳），但它没有"修好"
> 那个崩溃，因为崩溃本来不在库里 —— 真正补上的是下一条的线程自检。

`close()` 以前是"先 `gfx_.shutdown()` 再 `DestroyWindow()`"。窗口还活着时
`DestroyWindow` 会**同步**把 `WM_KILLFOCUS` / `WM_NCDESTROY` 派发回窗口过程，
而 GL 上下文这时已经拆掉了 —— 重入路径踩到已析构的成员。

- 顺序改成：先把窗口过程摘回 `DefWindowProc` 并清掉 `GWLP_USERDATA`（之后任何消息
  都不进我们的代码，也不会有悬垂 `this`）→ `ShowWindow(SW_HIDE)` → 拆 GL →
  `DestroyWindow`。
- `WM_PAINT` 加就绪守卫：GL 没就绪时 `ValidateRect` 掉，别落到 `DefWindowProc` 去真画。

### 新增 — 线程自检：跨线程调 `pump` / `render` 明确报错（docs/issues/08）
窗口、消息循环、WGL 上下文都有线程亲和。以前跨线程调用**不报错**，症状是随机 AV
或退出时挂死，日志里一片空白 —— 连"是不是我写错了"都看不出来。

- `open()` / `openChild()` 记下建窗线程；`binui_pump` / `binui_render` 在非建窗线程上
  调用时返回 **-1**（原语义只有 0/1，扩一位向后兼容），并往 stderr 打一行说明；
  `binui_app_destroy` 仍会执行（没法返回错误），但先打警告。
- Go 绑定：新增 `binui.RunMain(fn func() int) int`（替宿主 `runtime.LockOSThread`）与
  `PumpOrErr`（把"线程不对"单独报成 `ErrWrongThread`）；demo 改用 `RunMain`。
- **文档纠错**：`bindings/go/README.md`、`binui.go` 顶部注释、`docs/bindings.md`
  原先都写着「主 goroutine 天然固定线程」—— 这句是错的，main goroutine 会在安全点被
  调度到别的 M，`main` 里也必须锁线程。

### 修复 — `display:none` 只跳过元素本身，子树被重挂到最近的可见祖先

隐藏容器里的绝对定位子元素会按"新父级"重新解析坐标（跑到左上角），而且**零警告**。

- 编译器新增一趟预处理：按**级联后的 style**（class 规则也算）标出 `display:none`
  的整棵子树，主循环按标记跳过。沿 `parentIdx` 链向上查，对元素顺序没有要求
  （`::before` 合成节点的父索引可能大于自身下标）。
- `:nth-child` 的口径（`display:none` 的元素仍计入兄弟序号）不变。

### 修复 — `<button>` / `<input>` 默认底色写死 `#222222`，按钮文字强制居中 + 700 粗体

浅色主题下"不写 background 的按钮"就是一坨黑块；`text-align:left` 与 `font-weight`
双双失效（三个后端各有一份写死的绘制代码）。

- 默认底色：`BINUI_BUTTON` / `BINUI_INPUT` 也改成透明，与 v13 起"容器默认透明"一致。
- 文字对齐：三个后端都改成读 `BINUI_TEXT_CENTER` / `BINUI_TEXT_RIGHT`，算法与
  `BINUI_LABEL` 分支完全一致。`<button>` 的 UA 默认居中由**编译器**补（浏览器如此），
  所以没写 `text-align` 的按钮观感不变。
- 字重：读扩展绘制段的 `font-weight`（`PAINT_WEIGHT`），没声明时回落 700。

### 修复 — Makefile：`test_capi` 从不重链，跑的是旧二进制

`test_capi` 的规则把 `$(BINUI_C_A)` 写在变量定义之前 —— GNU Make 立即展开成空，
于是这个目标只依赖 `.cpp`，库更新后不重链。改了 C ABI 之后跑出来的崩溃其实是
"新旧两套目标文件混链"。现在直接写 `$(BUILD_DIR)/libbinui_c.a` 并补上 `$(LIB_A)`。

### 修复 — 滚动条颜色/宽度写死，浅色主题右边一条黑杠

轨道 `#2E2E2E` / 滑块 `#5A5A5A` / 宽 8px 全是字面量，没有任何配置入口。

- 渲染器新增 `BinUIScrollbarStyle` + `setScrollbarStyle()` / `resetScrollbarStyleToDefault()`；
  **默认按根节点底色亮度派生**（浅底浅色、深底深灰）。
- C ABI 新增 `binui_set_scrollbar_style()` / `binui_reset_scrollbar_style()`，
  三个绑定同步。三个后端都改成从 `renderer.scrollbarStyle()` 取色与宽度。
- CSS 的 `scrollbar-color` / `scrollbar-width` 仍然不支持，但**不再静默忽略**：
  编译期会告警并指向上面的接口（否则作者会以为改主题生效了）。
- **补上 gutter**：真的要出竖直滚动条时，滚动容器的内容视口右侧扣掉滚动条宽度，
  内容不再被滚动条压住（GL / D3D11 / GDI 三端一致）。

### 新增 — 运行期能力：`scroll_to` / `set_node_bounds` / `add_node` 等

核心渲染器里一直有，只是没导出。列表 / 聊天流绕不过去的能力：

- `binui_scroll_to` / `binui_scroll_by` / `binui_content_height`
- `binui_set_node_bounds`（宿主自己排版 / 撑高）
- `binui_add_node` / `binui_remove_node`（动态节点；`remove_node` 只是隐藏，id 不复用）
- 纯增量新增，不动既有结构体 / 签名，`BINUI_C_ABI_VERSION` 保持 1。
- Python / Go / Rust 三个绑定同步补齐（Go 还补了 `SetImageRaw` / `SetImage` /
  `MountComponents`，并用 `unsafe.SliceData` 修掉了 `go vet` 的
  `possible misuse of unsafe.Pointer`）。

### 新增 — `set_text` 换长文本后自动重算高度

以前"未声明高度就按文本估算撑开"这一步**只在编译期**做，运行期 `set_text` 换成长文本
会画到节点盒外、压到下一个元素身上。

- 规格段用旧布局的 `reserve` 字节承载 `heightAuto` + `autoBase`（基线高度，即估算前的
  元素默认盒高）。旧二进制这两位恒为 0 = 不重算，**不升 `BINUI_VERSION`**
  （先例：`BINUI_TEXT_CENTER` 复用 flags 的空闲位）。
- 估算口径抽成 header-only 的 `include/binui_text_estimate.h`，编译器与渲染器共用一份
  （否则"编译期刚好放下、换文本就压字"）。重算取 `max(基线高度, 估算高度)`，
  与编译期完全一致。

### 修复 — 同一线程多实例时 GL 上下文不切换，先建的实例画成纯黑

WGL 的 current context 是**线程级**的。`BinUIGLGdi::init()` 里 `wglMakeCurrent`
只做一次，之后没人再切回来 —— 于是同一线程里有两个后端实例（子窗口模式嵌两块面板、
或者多窗口）时，所有绘制都跑到最后 init 的那个上下文里，先建的那块**整个窗口纯黑**，
而它的事件/命中测试一切正常（所以很难察觉是渲染问题）。

- `Impl::makeCurrent()`：先 `wglGetCurrentContext()` 比对，不是自己才
  `wglMakeCurrent`；`beginFrame()` / `present()` / `presentViaGdi()` 三处入口统一调用。
- 这条是"子窗口模式"的必修项：嵌入场景天然每帧多个实例交替绘制。

### 新增 — `tools/gl_shot`：GL 后端的离屏抓图（终于能看见 GL 输出）

OpenGL 窗口抓不到图 —— `PrintWindow` 对双缓冲 GL 窗口返回全黑，`BitBlt` 只能抓合成
到桌面的部分；所以文档图一直只有 GDI 离屏渲染，而 GDI 自带字体链接，"GL 到底画成
什么样"谁都没验证过。

`tools/gl_shot <ui.html> <out.png> [w] [h]`：建一个窗口 → GL 上下文 → 渲染一帧 →
`presentViaGdi()` 读回离屏像素并 `StretchDIBits` 到窗口 DC → `BitBlt` 存 PNG。
**窗口必须先 `ShowWindow`**：GDI 对隐藏窗口的 `StretchDIBits` 直接丢弃（不报错），
读回会得到一片空白 —— 这也是排查时踩过的坑。

产物：[docs/img/glyph-fallback.png](docs/img/glyph-fallback.png)（逐字形回退 + 豆腐块）。

### 修复 — 缺字留白改成豆腐块：逐字形回退（问字体的 cmap，不按码点区间猜）

GL / D3D 后端选字体一直是"按码点区间猜"：`cp >= 0x80` → 微软雅黑，落进 emoji 区间 →
Segoe UI Emoji，否则 Segoe UI。猜中的面里没有这个字形时，`FT_Get_Char_Index` 返回 0，
`renderGlyph` 直接 `return false`，光栅化不缓存、绘制时 `if (!g) return;` —— **渲染成一片
空白**。用户看到的不是"这个字体没有该字符"，而是"引擎没画"。

为什么猜本身是错的：Unicode 只规定"码点 ↔ 抽象字符"，字形归谁由**每个字体文件自报的
cmap** 决定；Block 只是管理分组，标准明确说不能用来选字体。按区间猜会同时错两次 ——
缺字（空白），以及风格错（`⌘⌫⎋` 这类黑白符号被丢给 emoji 字体，画成彩色图形）。

- `FontEngine` 新增**逐字形回退**：`setFallbackChain({…})` / `faceIndexFor(cp)` /
  `faceName(idx)`。`faceIndexFor` 扫候选链、查的正是 `FT_Get_Char_Index`（字体 cmap），
  命中即停、结果按码点缓存；候选面的 `FT_Face` 也按需加载（前面的面命中就不读后面的，
  中文界面不会因为链尾有泰文候选就多读一个字体文件）。
- 新增 `renderMissingGlyph(size, …)`：**空心方框（豆腐块）**，几何只有一份，两个 GPU
  后端共用。缺字时画方框而不是留白 —— 留白像引擎 bug，方框是"当前字体链里没有这个
  字符"的明确信号。测宽与绘制用同一份度量，顺带修掉"占着 0.6em 宽度却不画"的错位。
- 设计上不可见的码点（C0 控制符、ZWJ/ZWNJ、变体选择符 VS1–256、键帽组合符）不画方框，
  否则一串 emoji 里会凭空多出方块。
- `registerSystemFonts` 把候选面补齐并排好顺序（Windows）：Segoe UI → 微软雅黑 →
  **Segoe UI Symbol** → Segoe UI Emoji → 宋体/黑体 → MS Gothic → Malgun Gothic →
  Nirmala UI → Leelawadee UI → Arial。符号面排在 emoji 面之前：`⌘⌫✕` 现在走
  Segoe UI Symbol 画**黑白**符号，只有真 emoji（`😀`、ZWJ 序列）才走 COLR 彩色路径。
- 两个 GPU 后端的字形图集 key：低 6 位从 `cjk|emoji` 两个布尔换成**面下标**
  （同一码点解析到不同字体时必须分图集，否则字形会串）。
- `test_font` 新增断言：回退链已填、`setFallbackChain` 过滤未注册名 + 去重、
  `faceIndexFor` 对 `A` / `中` / `−` 命中且**命中面确实画得出该字**、
  `U+10FFFD`（私用区末尾）判定为无字形、`⌘` 解析到 Segoe UI Symbol、
  豆腐块 12/14/32px 的四边有墨 + 中心空心 + 边框不透明。

> 顺带说明为什么这个 bug 一直没在文档图里暴露：GDI 离屏渲染走 `DrawTextW`，系统有
> **字体链接**（font linking）自己挑字体补上；GL/D3D 后端自己管字形图集，缺就是缺。
> 所以 `docs/img/*.png` 全对，真程序里才有空白。

### 新增 — 子窗口模式：把 binUI 当成控件嵌进宿主窗口

之前只有"binUI 自己当应用"（顶层 `WS_POPUP` 无边框窗 + 自己的消息循环）。要把它塞进
WPF / Qt / WinForms 的某个面板里，缺的是这一层：`WS_CHILD` 子窗口，宿主管消息循环、
布局、DPI，binUI 只管那一块画布。

- `BinUIAppGLGdi` 新增 `openChild(parent, x, y, w, h)` / `renderFrame()` /
  `setClientSize(w, h)` / `windowHandle() / isChildMode()`；`pump()` 在子窗口模式下
  只出帧、**不 `PeekMessage`**（碰了就是把宿主的消息吃掉）。
- C ABI 新增 `binui_app_open_child` / `binui_render` / `binui_resize` / `binui_app_hwnd`。
- 窗口类注册顺带修正：`hInstance` 改为取**本模块**（`GetModuleHandleExW` +
  `FROM_ADDRESS`）而不是 `GetModuleHandleW(nullptr)`。DLL 里后者拿到的是宿主 exe 的
  实例，把窗口类注册进别人的 atom 表是越界行为——顶层窗侥幸能用，子窗口模式不行。
- 子窗口模式下刻意不做的事：`SetProcessDPIAware`（进程级策略归宿主）、`SetFocus`
  （不抢宿主焦点）、`WS_EX_LAYERED`（对子窗口无效）、标题栏拖动/边框缩放/最大化。
- 生命周期：宿主 `DestroyWindow` 时子窗口的 `WM_DESTROY` 当场收摊（关 GL、标记关闭），
  且**不发 `PostQuitMessage`**（那会把宿主的消息循环干掉）；之后 `binui_render` 返回 1。
- Python 绑定加 `open_child` / `render` / `resize` / `hwnd`，并给了活例子
  `bindings/python/demo_child.py`：用 ctypes 建一个普通 Win32 宿主窗口，里面嵌两块
  独立 binUI 面板。

**踩到的坑（已写进文档）**：宿主窗口必须带 `WS_CLIPCHILDREN`。不加的话宿主的
`WM_PAINT` 会把整个客户区刷一遍、把 GL 子窗口那块一起刷掉——GL 子窗口没有留存面，
表现就是"面板是白的、点得到但看不见"。加与不加是"整块空白"和"两块面板各就各位"的区别。

验证：点击两块面板的按钮 → 事件回到 Python、两块面板同时更新；输入框聚焦 + 输入中文；
点"关闭宿主" → 干净退出（退出码 0）。参数校验进 `tests/test_capi.cpp`，CI 可跑。
真机截屏见 [docs/img/child-window.png](docs/img/child-window.png)：普通 Win32 宿主窗口，
左边 220px 定宽面板、右边自适应面板，两块独立实例同时正确渲染。

### 新增 — C ABI（`include/binui_c.h`）与 Python / Go / Rust 绑定

引擎一直是纯 C++（`namespace binui`、`std::function`、`std::vector` 返回），其他语言
根本接不上：全仓库没有 `__declspec(dllexport)`，CMake 只有 `STATIC` 库。这次补了对外
的唯一一扇门，并把三门语言跑通。

设计上只守三条约束，绑定写法就统一了：

1. **定长 POD** —— 只用 `int32_t` / `uint32_t` / `float`。没有 `bool`（C 是 1 字节、
   C# 默认 4 字节）、没有 `enum`（宽度随编译器）、没有 `size_t`（32/64 位会变）。
   所以结构体天然 4 字节对齐，**不需要任何 `#pragma pack`**。
2. **没有跨语言回调** —— 事件全部 `binui_poll_event()` 轮询。宿主不进引擎的调用栈，
   就没有线程亲和、GC 保活、重入这些问题；PHP/Ruby 这类没有稳定回调的语言也能用。
3. **UTF-8 + (buf, cap) 两段式取文本** —— 不返回需要宿主释放的指针。

新增文件与改动：

- `include/binui_c.h` / `src/binui_c.cpp`：约 40 个函数。编译（`binui_compile_file` /
  `binui_compile_memory`）、应用生命周期（`app_create` / `app_open` / `pump` /
  `poll_event` / `app_destroy`）、节点查询与修改、窗口动作、图片与组件挂载。
- `BinUIAppGLGdi` 拆出 **`open()` / `pump(timeoutMs)` / `close()`**（`run()` 行为不变，
  等价于三者串起来）。宿主语言需要"自己拥有消息循环"，原来的 `run()` 把循环锁死在里面。
  `pump` 在空闲时用 `MsgWaitForMultipleObjectsEx` 阻塞到有输入，不空转烧 CPU。
- 事件队列 + 内建交互：悬停重绘、标题栏拖动、输入框光标定位、Tab 循环焦点、
  Enter 激活、滚轮滚动、动画/视频时钟推进 —— 宿主只管 `pump` + `poll` + 改文本。
- 构建：`Makefile` 加 `capi` 目标（`libbinui_c.a` 静态库 + `binui_c.dll` 动态库 +
  它俩的 import lib），`CMakeLists.txt` 加 `binui_c` / `binui_c_shared`（仅 Windows）。
  DLL 自包含：`-static-libgcc -static-libstdc++` + FreeType/HarfBuzz 静态库，除系统 DLL
  外不用附带任何运行时。
- 绑定：`bindings/python/`（ctypes，零第三方依赖）、`bindings/go/`（cgo）、
  `bindings/rust/`（手写 `extern "C"` + 安全封装，不用 bindgen）。三份都能编 UI、
  建窗、收点击、改文本，且都验证过中文与 emoji 输入。
- 测试：`tests/test_capi.cpp`（无窗口，CI 可跑）把 ABI 钉死 ——
  `sizeof(BinUINode) == 56`、`offsetof(color) == 32`、`sizeof(BinUIEvent) == 48`、
  `offsetof(text) == 24`，外加编译/加载/两段式取文本/截断/NULL 防护/错误路径。
- 文档：`docs/bindings.md`（约束、流程、线程规则、未覆盖能力、新增语言步骤）、
  `bindings/README.md`（总表）、三份子 README；README 加「在别的语言里用」一节。

### 修复 — ANSI 窗口类导致中文输入变乱码（三个外壳全中）

三个窗口外壳（`binui_gl-gdi` / `app_win32` / `binui_d3d11`）都用 `RegisterClassA` +
`DispatchMessageA` 建窗。在 CP936 系统上，Windows 会把**一个中文字符拆成两个
`WM_CHAR`**（DBCS 字节），`handleChar` 逐字节转 UTF-8 就得到乱码：输入框里打「中」
显示「ÖÐ」，打「鱼」显示「Óã」。IME 输入同理。

改成 Unicode 窗口（`RegisterClassW` + `DispatchMessageW` + `CreateWindowExW` +
`DefWindowProcW`）后正常，代理对（emoji）也走通了 —— 原本写好的 `pendingHigh_`
代理对逻辑以前根本收不到数据，因为 ANSI 窗口不会给 16 位码元。

这条 bug 只在"真在别的语言里打一遍字"时暴露（C++ demo 一直是中文标签、没人打中文），
算是本次做绑定最实在的收获。

### 修复 — 输入框底部强调线与作者边框重复

三个后端都给每个 `<input>` 硬画一条 2px 底部强调线（老深色主题的 Material 下划线）。
v13 有 `border` 之后作者已经用 `border: 1px solid #E1E9F5; border-radius: 10` 画了
真正的框，那条线就变成横穿圆角的悬空横杠。改为**作者声明了 `PAINT_BORDER` 时不画**，
没声明的旧界面行为不变。

### 修复 — 点击输入框中部不定位光标

`placeCaretAtClick` / `placeCaretAtClickGLGdi` / `placeCaretAtClickD3D` 三个函数写了
但**从来没有调用点**（README 里却写着"点击光标定位"）。C ABI 的 press 里接上了；
`examples/features` 的 press 回调也补了这一行。

### 修复 — 库内调试输出污染宿主 stdout

`BinUiRenderer` 里有 4 处 `std::cout`（加载统计、组件挂载、点击日志、资源加载）。
作为嵌入式库，这会把噪声塞进宿主语言的 stdout。改为默认静默、`BINUI_VERBOSE=1`
打到 stderr（`std::cerr` 的错误输出保留）。

### 新增 — 组件源码支持 `.bum.html`

组件一直支持两种源码写法，但只有 `.bum.xml` 在文档里出现过。`ProbeAsset` 的候选表里
漏了 `.bum.html`：写 `src="x.bum.html"`（显式扩展名）一直能内联，写 `src="x.bum"`
（裸名字）就找不到 `x.bum.html`，只会安静地留一个空容器。

- `include/binui_asset.h`：候选表补上 `.bum.html` / `.bui.html` / `.binui.html`；
  裸名字顺序 `<名字>.bum` → `.bum.xml` → `.bum.html` → …（二进制优先，规则不变）。
- 标准组件库新增 `components/tag.bum.html`：库里第一个 HTML 写法的组件，
  `examples/features` 里用三种 `<use>` 写法（显式 `.bum.html` / 裸名字）指向同一个组件。
- `test_compiler` 新增 5 条断言：显式扩展名内联、参数替换、`kind` 换肤、裸名字命中。
- 文档：`components/README.md` 补「两种写法」一节，`docs/components.md` 清单加 `tag`。

**注意**：组件只在 HTML 解析路径下展开。`CompileXML`（`.binui.xml` 老路径）不处理
`<use>`，那种源文件里写组件不会生效。

### 变更 — 示例砍成 6 组：功能 / 动画 / 字体 / 2D 渲染 / 3D 渲染 / 纹理

原来 9 个示例各演各的，彼此重叠又都不完整（cssshow 讲伪类、components 讲组件化、
vision_monitor 讲数据更新，三套脚手架几乎一样）。现在收敛成 **6 组各一个示例**，
一组只演示一件事，全部用标准标题栏 + 同一套浅色令牌：

| 目录 | 组 | 来源 |
|---|---|---|
| `examples/features` | 功能 | **新合并**：cssshow + components + vision_monitor 的数据驱动 |
| `examples/animshow` | 动画 | 保留 |
| `examples/fontshow` | 字体 | **新建** |
| `examples/svgdemo` | 2D 渲染 | 保留 |
| `examples/cubedemo` | 3D 渲染 | **新建** |
| `examples/texturedemo` | 纹理 | **新建** |

删掉的：`videodemo` / `singlewindow` / `multiwindow` / `galgamechat`（git 历史里都在）。
`examples/assets/noela.png` 一并删除（只有被删的立绘示例用）。

三个新示例的看点：

- **fontshow**：字号 12–32、字重 400–800、斜体、行高 100%/170%、三种文字装饰、
  中英混排与 UI 符号。顺带把"字形图集怎么分类、缺字为什么是空白"写进卡片注释。
- **cubedemo**：引擎里没有 3D —— 顶点变换、透视投影、逐边按深度调色全在应用里做，
  最后只用 `gfx.fillLine` 画 12 条棱。这是"引擎只管 UI"这条边界的活示范。
- **texturedemo**：布局里写一张不存在的图 `<img src="tex/proc.png">`（只为占一个图片槽），
  运行期用 `updateImageRaw` 每帧生成 RGBA 塞进去；同一张纹理被四个尺寸采样。
  和 `<video>` 帧走的是同一条通路。

配套改动：

- `examples/README.md` 新增：6 组对照表 + 写新示例的三条约定。
- 原来 `examples/vision_monitor/main.binui.xml` 是唯一的 XML 源示例，随示例一起删掉；
  它作为 **XML 解析的回归夹具** 移到 `tests/fixtures/vision.binui.xml`，
  `test_compiler` / `test_renderer_real` 指过去（XML 的编译与运行时更新覆盖没丢）。
- `test_hot_reload` 的"源码树兜底"改用 `examples/features/NOTES.md`。
- Makefile / CMake 的 demo 目标改成 6 个（`add_gl_demo(name)` 统一处理拷贝）。

### 新增 — flex 支持 `width:content`（按文字宽度排）

flex 里没有"宽度自适应"：声明像素要么留一条大缝、要么把字裁掉，`width:auto` 又会被
当成弹性项去平分剩余空间。新增 `width:content` —— 用编译器自己的字宽估算（和换行同一套
度量）×1.08 + 4px 作为固定宽度。标题栏的图标/标题/副标题就靠它紧挨着排，不用再手调像素。

### 修复

- **标题栏最小化按钮是空的**：`─`(U+2500 制表符) 只有中文字体里有，Segoe UI 没有；
  GL/D3D 的字形图集按"非 ASCII → 中文字体"分类，该字形笔画极淡，肉眼就是个空按钮。
  换成 `−`(U+2212)，三颗按钮的字号统一由 `bsym` 控制（默认 13）。
  `tests/test_font.cpp` 新增一条断言：四个 UI 符号（− □ ✕ ◆）至少有一个候选字体画得出笔画。
- **标题栏副标题离标题太远**：`titlew` 原来是手填像素，各示例按字数猜，猜大了就是一条缝。
  现在默认 `content`，副标题紧贴标题；单窗口示例那个超长标题顺手缩短，细节挪到副标题。

### 新增 — 标准组件库 `components/`（标题栏、按钮、输入框……）

`examples/` 里的组件是"组件机制"的演示；真正能拿来用的通用组件之前一个都没有 ——
每个示例都手写一遍标题栏（6 份几乎一样的 8 行 span/button）。新增仓库根的
`components/`，11 个纯 HTML+CSS 组件，`<use src="…">` 编译期内联、运行期零依赖：

`titlebar` `button` `field` `badge` `toggle` `panel` `dialogue` `stat` `avatar`
`divider` `statusbar`

- `titlebar` 用 **flex 排版**，窗口多宽都不用宿主算按钮坐标（`[图标][标题 flex:1][说明][─][✕]`）。
- `panel` 支持"带内容"：宿主的子元素会落进组件根容器，按流式排在标题下面。
- 文档：`components/README.md`（参数表 + 三条使用约定 + 限制）、`docs/components.md` 新增一节。

### 修复

- **`<use>` 丢掉了组件根的标签**：替换节点恒为 `div`，于是
  · 以 `<button>` 为根的组件实例变成普通容器（不可点击、文字不居中、不吃 `:hover`）；
  · 以 `<input>` 为根的实例丢掉占位符与编辑语义（输入框只剩一个空盒子）；
  · 以 `<span>` 为根的实例变成块级 `LIST`（文字带 12px 内缩、顶对齐，小胶囊里的字被裁）。
  现在把根标签原样搬过来 —— 上面三种症状一次全消。标准组件库正是靠它才能成立。

### 变更

- 示例统一改用标准 `titlebar`（animshow / svgdemo / cssshow / galgamechat /
  singlewindow / multiwindow·stage·dialog），并删掉各自那份手写的标题栏 CSS；
  `examples/components` 新增第四张卡片展示标准组件库。
- `examples/cssshow` 的主程序补上"─ 最小化"分支（以前只有 ✕）。

### 变更 — 示例 UI 全面换成清新浅色主题

9 个示例（animshow / cssshow / components / svgdemo / videodemo / singlewindow /
multiwindow / galgamechat / vision_monitor）与引擎内置默认布局，从"深色赛博"统一改成
**浅色清爽风**：页面底 `#F4F7FC`、白卡片 + 1px `#E6EDF7` 边框、主字 `#1F2A37` /
次字 `#66768C` / 弱字 `#94A3B8`、主色 `#3E8BFF`、薄荷 `#2FBF9A`。层次只靠 1px 边框与
色差（`box-shadow` 引擎不支持），令牌约定写在每个示例 `<style>` 开头的注释里。

- 按钮拆成"主色填充 + 白字 / 浅蓝底 + 主色字"两套，各自带悬停/按下；把 `background`
  从行内 `style` 搬进 class —— 行内样式会压掉 `:hover`，这是浅色改造最容易踩的坑。
- 几何布局基本不动（只有 svgdemo 收掉多余的窗口高度、animshow 的"方向·次数"卡片
  第 4 行原本就溢出卡片、videodemo 的装饰圆原本悬在卡片外，一并修正）。
- 示例里硬编码的颜色同步换新（animshow 的色块调色板、svgdemo 的频谱条、singlewindow
  的换背景色循环）。
- `docs/img/{default-layout,cssshow,components}.png` 全部重渲染。

### 修复

- **GDI 后端不画 `BINUI_LINE`**：`<line>`/`<polyline>`/`<polygon>` 在 GDI 画布上一根都
  没有（GL/D3D 正常），三端不一致。现在按端点包围盒单独裁剪后画线。
- **容器文字恒为白字**：`BINUI_LIST` 没声明文字色时一律白 —— 浅底容器里（尤其运行期
  `addNode` 出来的列表行）文字看不见。改为按节点自身底色亮度选深浅（`PaintContrastText`）。
- **输入框强调色写死紫色**：文字/光标/底部强调线都是 `#BB86FC`，浅色主题没法配色。
  改为优先用作者声明的文字色，强调线压到 35%。
- **内置默认布局的文字全是错乱的**：`GenerateDefaultBinUI()` 的字符串偏移是手写魔数
  （0/5/18/29/66/95/106/115/130/135），文案一改就整体错位，渲染出来的标题、按钮、
  底部导航全部串到别的节点上；而且容器的 `textOffset = 0` 现在指向 "9:41"（应为空串）。
  改成按字符串长度累加算出偏移，并在表首放空串。默认布局同时换成浅色。
- `tools/offscreen_render` 新增 `--default`：直接渲染内置默认布局，这张 docs 图终于
  可以一条命令重现（以前只能手工截图，所以一直和代码对不上）。

### 新增 — 元素语义与默认样式：UA 样式表、行内标签、表单属性（【格式 v14】）

> 上一轮把"格式"和"CSS 表达力"补完了；这一轮补的是**元素这一侧**。体检时发现的共同
> 根因是两条：① 没有"元素 → 默认样式"这一层（浏览器 UA 样式表的等价物），于是 `<u>`
> `<mark>` `<hr>` `<b>` 这些标签写了等于没写；② 表单属性（`placeholder` / `value` /
> `disabled` / `readonly` / `checked` / `hidden`）被**静默忽略或错误消费**，而且整份 HTML
> 编译下来一条警告都没有 —— 作者没有任何线索。

- **【格式 v14】`BinUIPaintNode` 补 `textFlags`**（40 → 44 字节）：斜体、占位符、只读、
  初始勾选。四个都是"少数节点才需要"的布尔量，跟着稀疏的扩展绘制段走，比往定长
  `BinUINode` 里塞字段便宜；`flags` 的 16 位已经用满。
- **内置 UA 默认样式表**（最低优先级的级联层，作者任何声明都覆盖它）：
  `h1~h6` 加粗 + 字号梯度 28/24/20/18/16/14，`b/strong/th` 700，`i/em/cite/var/dfn` 斜体，
  `u/ins` 下划线，`s/del/strike` 删除线，`mark` 浅黄底深字，`hr` 一条 2px 中灰线，
  `a` 下划线，`pre` 保留原始换行与缩进，`sub/sup` 0.75em 行内小字（**无基线偏移**）。
- **默认文字色随根底色自适应**：根底色亮度 > 0.5 取 `#1A1A1A`，否则白。之前默认写死白色，
  而根底色默认也是白 —— **不写 `color` 的浅色页面整页白底白字**，只有到处写 `color`
  才看得见；深色主题亮度低，观感不变。
- **行内标签不再掉进块级分支**：`u/s/strike/del/ins/mark/code/kbd/samp/var/abbr/cite/q/`
  `dfn/time/sub/sup/dt/dd/figcaption/summary/caption` 等补进 `BINUI_LABEL` 白名单
  （以前默认落到 `BINUI_LIST`，把一句话里的几个字打成独立整行块）。
- **`font-style: italic`**：FreeType `FT_GlyphSlot_Oblique` 合成倾斜（只切变轮廓、不改
  advance，所以测宽/绘制仍一致），GDI 走 `CreateFontA` 的 italic 参数，三端一致。
- **`white-space: pre|pre-wrap`**：解析阶段同时保留"折叠文本"与"原样文本"，按 CSS 取用；
  `<pre>` 由 UA 样式给出 `white-space:pre`。以前代码块的缩进被当普通空白折叠掉。
- **表单属性真生效**：
  `placeholder` 列为占位符（55% 色绘制、首次输入清空、失焦留空恢复，
  `renderer.inputValue()` 取真实值）；`value` 只在文本类 `input` 上是真实值；
  `disabled` → `BINUI_DISABLED`（不进焦点链、不响应悬停/点击、拒绝编辑、三端 55% 淡）；
  `readonly` → 可聚焦可选中但不显示光标、拒绝内容变更；
  `<input type=hidden>` 不生成节点；`checkbox/radio` 编成 18×18 方块/圆点，
  `checked` 播种为运行时勾选态；`<a href>` 变成可命中节点；`<img alt>` 写进文本，
  图片加载失败时后端居中显示。
- **HTML 标签/属性体检**：未识别的标签、"写得进去但没实现"的属性（`name`/`for`/
  `required`/`maxlength`/`pattern`/`tabindex`/`target`/`rel`…）各警告一次；`<table>` /
  `<select>` / `<textarea>` / `<details>` / `<progress>` 这类"认得但只能糊弄"的标签会
  明说它会变成什么。HTML 模式生效，XML 允许自定义标签不查。

### 修复

- **GDI 后端的字号比 GPU 后端小一号**：`CreateFontA` 的高度传了正值（字符单元高度，
  含内部行距），比 em 小约 15%，同一份 UI 在 GDI 里字号/换行位置和 GL/D3D 对不上。
  改成负值（em 高度，与 `FT_Set_Pixel_Sizes` 同口径）。
- **GDI 后端把窄盒子里的短标签折成两行**：单行文本只要超宽就走 `DT_WORDBREAK`，而 GPU
  后端画单行文本从不换行。改成超宽 20% 以上才换行。
- **GDI 后端的裸 `'\n'` 不换行**：`DrawTextW` 只认 CRLF，`<br>` / `<pre>` 存的都是单个
  `'\n'`，整段会连成一行。绘制前统一补 `'\r'`。
- **GDI 后端把"透明 + 有文字"的节点整块跳过**：`<pre>` / `<div>文字</div>`（没有底色）
  直接消失。跳过条件补上"没有文本"。
- **`<hr>` 从 v13 起完全不可见**：容器默认透明化的副作用 —— 既没高度也没颜色。
- **`placeholder` 被当成真实文本**：点进去就能编辑提示语、退格删掉还触发 `INPUT`。
- **`value` 在 `checkbox`/`radio` 上被画成可见文字**；`<input type=hidden>` 变成可见输入框。
- **`examples/videodemo` 用 `name=` 取节点**（`findNodeByName` 只认 `id`），改 `id=`。
- **`<use>` 的参数键被当成属性挂到替换节点上**：`<use label="…">` 会在结果里留下
  一个无效的 `label` 属性。现在只挂"有语义的属性"（宽高/样式/类等）。
- **`examples/components` / `cssshow` / `svgdemo` 的文字盒子按新字号重排**（正文变大后
  几处说明文字被裁掉）。

### 新增 — 元素与 CSS：底色/文字色分家、边框、渐变、字重、行高（【格式 v13】）

- **【格式 v13】扩展绘制段**：`BinUIPaintNode`（40 字节，稀疏）——文字色、四边边框
  （宽度与颜色各自独立）、`linear-gradient` 底色、字重、行高、文字装饰线、`overflow:hidden`
  的"只裁剪"标记。不往定长的 `BinUINode` 里加字段，旧布局语义不变；链接器合并组件时同样迁移。
- **底色与文字色分家**（本轮最重要的一条）：以前只有一个颜色字段兼任两职，于是
  `div { color:#333 }` 会被当成底色用掉、`span { background:transparent; color:#333 }`
  直接让文字变透明、按钮文字恒白。现在 `background*` 是底色、`color` 是文字色，
  **并且沿元素树继承**（`body{color:#333}` 对整棵树生效）—— 浅色主题终于能写。
- **边框**：`border` / `border-width` / `border-color`（1~4 值展开）与
  `border-top/right/bottom/left`，四边各自宽度与颜色，画在节点内侧不参与布局。
  圆角元素走"环形边框"：按圆弧逐行铺薄条（直边区自动合并成一条），
  不再用 4 条轴对齐矩形去凑 —— 那样圆角按钮上会露出方框的角。
- **渐变底色**：`linear-gradient(...)`（`to <side>` / `135deg`，取首尾两色）。条带按
  "圆角区 1px 一条 + 直边区 ~32px 一条"切分，圆角渐变不会出现阶梯。
- **排版**：`font-weight`（FreeType `FT_GlyphSlot_Embolden` 合成加粗，GDI/GL/D3D 三端一致，
  测宽与绘制同步加宽）、`line-height`（倍数 / `%` / `px`）、`text-decoration`
  （`underline` / `line-through` / `overline`）。
- **选择器（补齐最大的一块表达力缺口）**：复合类 `.btn.primary`、属性选择器
  `[type=text]` / `[href^=http]` / `[class~=x]` / `[lang|=zh]`、后代/子代/相邻兄弟/后续兄弟
  组合子（`.card .title`、`.card > .title`、`h2 + p`、`h2 ~ p`）。组合子按 CSS 从右往左匹配，
  兄弟序号只统计可视元素（`head/meta/script/style` 与伪元素合成盒不算）。
- **容器默认透明**：`div/section` 等容器不再默认填 `#222222`（浅色主题的拦路虎）；
  `<button>` / `<input>` 仍保留控件底色，`<rect>`/`<circle>` 仍是 SVG 语义的黑。
- **文本节点高度估算**：未声明 `height` 的文本节点按估算行数撑开（CJK 1.0×字号、其余
  0.52×字号，行距取 `line-height`）——以前段落按单行高排，第二行就压到下一个元素身上。
- **容器撑高会推动后续兄弟**：自动高度的容器弹出时把"被内容撑高"的增量补到父容器的
  流光标上，卡片里放几行字不会再把后面的元素压住。

### 修复

- `LABEL` 写了 `background`（哪怕 `transparent`）就把 `color` 当背景色解析，导致文字消失。
- `overflow:hidden` 被当成可滚动容器（会出滚动条、响应滚轮）；现在只裁剪不滚动。
- GDI 画布后端的长文本不换行（硬套 `DT_SINGLELINE`），与 GPU 后端表现不一致。
- `binui_compile` 把每条编译警告打印两遍（编译器自己写 stderr + 工具再打 warnings）。
- `offscreen_render` 保存 PNG 后以 `0xC0000005` 退出：GDI+ 的 `Bitmap(HBITMAP,…)` 不拷贝像素，
  却让 `Bitmap` 活过了 `DeleteObject(dib)`；现在先析构再删。
- **hover 放大时边框被"削掉一半"**：`invalidateNode` 只按布局矩形失效，`transform:scale()`
  长出来的那一圈（边框/底色/焦点环）永远不在重绘区里，于是只留下缩回前的残影。
  新增 `renderer.invalidRect()`（布局矩形 ∪ 动画缩放/平移包围盒 + pad），9 个示例统一改用它。
- **键盘焦点环的圆角写死成 10**（三个后端都是），圆角 8 的按钮上会套出一个半径不同的环，
  看着像"双线框"；现在跟随节点自身圆角与内缩。
- Makefile 的头文件依赖只覆盖了部分输出目录，`backends/`、`platform/`、`common/`、`bin/`
  改了头文件不会重编；补齐后 `make` 的结果才可信。

### 新增 — 组件化：`.bui` / `.bum` / `.bum.xml`（【格式 v12】）

- **命名与分发**：一个界面 = 一个 `.bui`（组件全内联，运行期零依赖）；要拆开就是
  `.bui` + 若干 `.bum`。组件可以是二进制 `.bum`、源码 `.bum.xml`，也可以两种都放；
  作者还能按组件混放。旧名 `.binui` 仍被识别。
- **加载优先级**：二进制优先；**源码 mtime 更新时改用源码并告警**——改源码保存后
  `--watch` 直接生效，不会因为忘了重编 `.bum` 而“改了没反应”。规则只在
  `include/binui_asset.h` 里实现，编译器与运行期加载器共用。
- **布局链接器**（`include/binui_link.h` / `src/common/binui_link.cpp`）：把组件布局并进宿主——
  节点 id 重编号、坐标平移到锚点内容原点、占位节点按组件包围盒撑大、字符串表后移重算偏移、
  缩放规格/状态样式/动画/视频/资源/组件引用/名称段一并迁移；失败时宿主保持不变。
- **编译期内联（默认）**：`<use src="x.bum.xml">` 走源码展开（可传参），
  `<use src="x.bum">` 由链接器离线合并（不暴露源码）。两者产出同一个 `.bui`。
- **运行期外挂**：`<use src="x.bum" external>` 只留占位节点 + 一条组件引用记录；
  新增 `binui::ex::MountAll(renderer, dir, layout)` 与 `renderer.mountComponent()`，
  换组件不用重编主 `.bui`（皮肤/插件面板）。挂载幂等，`isComponentMounted()` 可查。
- **【格式 v12】两段新段（恒定写出，位于状态样式段之后、名称段之前）**：
  - **资源段**：`imageIndex` → 图片路径，布局自描述。外壳不必再自己保存编译期的图片列表
    （`renderer.imagePaths()` / `ex::LoadLayoutImages()`）；组件合并时资源下标在这里整体重排。
  - **组件引用段**：`external` 组件的占位节点 id 与相对路径。
- **参数**：`{{name}}` / `{{name=默认值}}`；单独编 `.bum` 时按默认值代入（裸 `{{name}}`
  留给内联替换），所以同一份 `.bum.xml` 既能被 `<use>` 传参，也能自己编成 `.bum`。
  未替换的参数占位不再触发“无法解析长度/颜色”的假警告。
- **`<use>` 对二进制组件的覆盖**：`w` / `h` / `bg` / `radius` 仍可覆盖组件根
  （其余参数在编 `.bum` 时已固化）。
- **新工具 `binui_compile`**：`.bum.xml` → `.bum`、`main.html` → `main.bui`；
  传入已是二进制的文件则原样拷贝，方便流水线统一。构建期由 `make components` /
  CMake 的 `components_bums` 目标生成，产物不落源码树。
- **`check_examples`** 覆盖 `.bum.xml` / `.bui.xml`，并校验外挂组件引用是否真的能解析到。
- 演示 `examples/components` 重做：标题栏 `titlebar.bum`、卡片 `card.bum`（二进制内联）、
  按钮 `button.bum.xml`（源码传参）、状态栏 `dock.bum`（`external` 运行期挂载），
  三种形态同屏（见 [docs/components.md](docs/components.md)）。

### 新增 — CSS 伪类 / 伪元素（【格式 v11】）

- **运行时伪类** `:hover` / `:active` / `:focus` / `:checked`：编译进新的**状态样式段**
  （`BinUIStateStyle`，16 字节/条），渲染器在指针/焦点/勾选变化时切换目标颜色 / `scale` /
  `border-radius`；节点声明了 `transition` 就走现成的过渡通道，否则瞬时。
  同时命中时的优先级：`:active` > `:focus` > `:checked` > `:hover`。
- **结构伪类**（编译期求值，零运行时开销）：`:first-child`、`:last-child`、`:only-child`、
  `:nth-child(An+B|odd|even|n)`、`:nth-last-child()`、`:first-of-type`、`:last-of-type`、
  `:only-of-type`、`:nth-of-type()`、`:nth-last-of-type()`、`:not(简单选择器)`、`:root`。
- **伪元素** `::before` / `::after`（需 `content`）：内联模式把内容并入宿主文本
  （`li::before{content:"▸ "}` 这类项目符号，同时抑制自动项目符号）；盒模式
  （带 `width/height/background*/…`）合成一个 `span` 子节点。`content` 支持字符串、
  转义（`\25B8`、`\"`、`\n`）与 `attr(属性名)`。
- 选择器特异性按 CSS 计权：伪类 +10（按 class），伪元素 +1（按元素）。
- 不支持的伪类/伪元素（`:visited`、`::first-line`、`::selection` 等）**明确打编译警告**
  并跳过，不再静默失效。
- 渲染器新增 `stateStyle(id, state)` / `activeStateStyle(id)` / `setNodeChecked(id, on)` /
  `isNodeChecked(id)` / `hitTestEx(...)`；`:hover`/`:active` 现在对**任何元素**生效
  （命中测试把「声明了该状态样式的普通节点」纳入），事件仍只对可交互节点派发。
- 新演示 `examples/cssshow`（`cssshow.exe`，支持 `--watch`）。

### 修复

- **绘制顺序改为按树遍历**（父先于子）：原先是对全部节点做一次全局排序，`position:absolute`
  的容器会被排到它自己的流式子节点**之后**，容器底色把内容整片盖掉——`<div style="position:absolute">`
  里放普通 `<span>` 这种最常见的写法直接看不见内容。现在同一父节点下按
  `(z-index, 流式先于 absolute, DOM 顺序)` 排，跨子树的 z-index 仍按层叠上下文处理。
- **GDI 后端（`src/platform/app_win32.cpp`）**：
  - 按钮/输入框/图片文本改用 `DrawTextW`（原来是 `DrawTextA`，UTF-8 被按系统代码页解释，
    中文直接叠成一团乱码）；
  - 节点颜色改为 `renderer.drawColor()`（原来自己按 hover/按下做 tint）——
    CSS 状态样式、`transition`、`@keyframes` 现在在 GDI 后端同样生效，与 GL/D3D11 一致；
  - 全透明节点不再画成黑色块（GDI 没有 alpha 合成；图片与视频帧例外）。
- **`test_renderer` 里一处逗号运算符误用**：`TEST("click empty keeps focus", (a, b))` 只检查了
  后半段，改成 `&&` 后断言才真正生效（当前行为是正确的，属于测试自身的 bug）。
- **非 Windows 平台拿不到任何字体**：`registerSystemFonts` 只有 Windows 分支，默认字形名还写死
  `Segoe UI` → Linux/macOS 上所有绘制测试都无字形。补了常见路径探测（Noto CJK / WenQuanYi /
  DejaVu / macOS PingFang·Helvetica）并把 `Segoe UI` 别名到实际可用字体。
- **`binui_charts.cpp` 无条件 include 后端头**（含 `windows.h`）导致核心库在非 Windows 编不过；
  现在 `LineChart::draw` 的后端绘制按 `_WIN32` 隔离，其他平台给空实现（vtable 仍完整）。
- **盒模式伪元素改用 `div`（`BINUI_LIST`）**：`LABEL` 在三个后端里都只画文字，
  `content:""` 的纯装饰盒子（分隔条/色块）会完全看不见；未声明 `background` 的盒模式
  伪元素按透明处理并警告（否则会冒出默认灰底）。
- **`:hover` 之类的选择器之前永远不会触发**：命中测试只返回 `BINUI_INTERACTIVE` 节点，
  普通 `div`/`span`/`p` 进不了 hover 状态机 —— 新增 `hitTestEx`，把声明了状态样式的
  普通节点纳入命中。
- **状态样式被无伪类规则污染**：提取状态目标时未校验「规则是否真的带该伪类」，
  导致 `.row:nth-child(odd)` 这类规则在四个状态位下全部命中，把基础 `border-radius`
  也写成状态目标（按钮 hover 时圆角被重置）。修法：只有带对应状态伪类的规则参与。

### 项目结构

- 补齐开源项目骨架：`LICENSE`（MIT）、`CHANGELOG.md`、`CONTRIBUTING.md`、
  `SECURITY.md`、`CODE_OF_CONDUCT.md`、`.editorconfig`、`.gitattributes`；
- `.github/`：CI（Linux GCC + Windows MinGW-w64）、issue 表单、PR 模板；
- **目录分层**（原来后端与工具散在 `src/`、`examples/` 根目录）：
  - `src/backends/`：`binui_gl-gdi.*`（OpenGL 3.3）、`binui_d3d11.*`（D3D11）、GL 扩展头；
  - `src/platform/`：`app_win32.*`——Win32 窗口外壳 + GDI 绘制（演示与 `tools/` 共用）；
  - `include/binui_paths.h`、`include/binui_hot_reload.h`：两个 header-only 应用工具从
    `examples/` 提升为公共头（可直接被第三方外壳 `#include`）；
  - `examples/assets/noela.png`：1.65 MB 演示立绘从 `tests/` 移出（测试目录不该放素材）；
  - `docs/mockups/`：两套设计稿从 `examples/*/mockup.html` 移到文档区，不再混在演示目录里；
  - 演示资源一律 `examples/assets/` → 构建时 `cp` 到各 demo 目录（按原名引用）。
- **修复 `make` 的资源拷贝静默失败**：`cmd /c copy /Y examples\x\y build-mingw\bin\x\y`
  在 `SHELL=sh`（README 里给的构建命令）下反斜杠被 sh 当成转义吃掉，cmd 只打印 banner 就退出——
  演示 UI 其实从未被拷到 `build-mingw/bin/<demo>/`，程序一直回退到内置默认布局。
  改为 `cp -f` + 正斜杠路径（`xcopy` → `cp -rf`）。
- `tools/`：离屏渲染 / 窗口抓取 / 窗口枚举 / 示例 UI 零警告检查（`check_examples`，已进 ctest）；
- `scripts/fetch-deps.sh` + `make deps`：第三方源码不入库（约 130 MB），一条命令拉取；
- `docs/img/`：文档用图（默认布局、`cssshow` 离屏渲染、字体渲染对比）。

### 变更

- `BINUI_VERSION` 10 → 11（新增状态样式段，恒定写出，位于视频段之后、名称段之前）。
  所有 `.binui` 需重新编译（本项目的 UI 均在运行时从 HTML 编译，无兼容负担）。
- README 补齐 CSS 支持矩阵与伪类/伪元素语义说明。

## [0.0.2]

- 增加 `examples/` 下多套演示（animation / svg / canvas / video / 多窗口 / galgame 前端）。
- 抽出 `include/binui_paths.h`（exe 真实路径 + UI 查找顺序 + 资源加载）与
  `include/binui_hot_reload.h`（`--watch` 热重载，保留窗口/滚动/焦点/输入内容）。
- 二进制布局演进：【v6】`min/max` 尺寸约束、【v7】动画段、【v8】名称表、【v9】视频描述段、【v10】`zIndex`。

## [0.0.1]

- 首个可运行内核：XML / HTML5 → binUI → 节点树 → GDI / D3D11 / OpenGL 三个绘制后端。
- 纯 C++17 核心（编译 / 解析 / 命中测试 / 事件分发无平台 API 依赖）。
- FreeType + HarfBuzz 字体服务：灰度字形图集、COLR/CPAL 彩色 emoji、HarfBuzz 整形。
- 二进制布局：【v3】缩放规格段（`setSize` 重排）、【v4】节点 `radius` 圆角、【v5】窗口描述段。
- 高层图表 API（`binui_charts.h`）。
