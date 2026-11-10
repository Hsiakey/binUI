# SVG 支持方案（`<path>` 起步）

> 状态：**P1 / P2 / 渐变（P3 首项）已实现**。
> P1 在 0.4.0 落地（布局格式 `BINUI_VERSION` 14 → 15），
> 渐变在 0.5.0 落地（14 → 16 分两步：15 = 路径段、16 = 路径渐变）。
> 下面的第 1–3 节保留方案原貌（含最初的判断依据），第 4 节之后是落地情况与实现后记；
> P2/P3 的待办见第 6 节，渐变的设计与实测见第 8 节。
> P1 分四次提交：A 几何内核 → B 格式与三端渲染 → C SVG 语义（viewBox/transform/继承）
> → D 验收（示例、像素基准、文档）。

---

## 1. 现状（实测，不是设计意图）

| 写法 | 今天的结果 | 证据 |
|---|---|---|
| `<svg width height>` | 透明容器节点，**不裁剪子元素**、GDI 端连底色都不画 | `src/platform/app_win32.cpp:898` 的 switch 没有 SVG case；`src/renderer/binui_renderer.cpp:2911-2925` 只把 `SCROLLABLE` 祖先当裁剪链 |
| `<line>` | 编成 `BINUI_LINE`（x,y=起点，width,height=终点，radius=线宽） | `binui_compiler.cpp:4718-4728` |
| `<circle>` | 编成 `BINUI_LIST`（圆角=半径） | `binui_compiler.cpp:4729-4732` |
| `<rect rx>` | 同上，`rx` → 圆角 | `binui_compiler.cpp:4733-4737` |
| `<polyline>/<polygon>` | 拆成多条 `BINUI_LINE` | `binui_compiler.cpp:4234-4278` |
| `<path d="…">` | **不画**，只报一条"不支持" | `binui_compiler.cpp:4012` |
| `<ellipse>` | 不画 | `binui_compiler.cpp:4014` |
| `viewBox` / `transform` | 忽略，各报一条"不生效" | `binui_compiler.cpp:4056-4063` |
| `fill` + `stroke` 并存 | **没有通道**：一个节点只能带一个颜色 | 填充塞进 `BinUINode.color`，描边塞进 `radius`（4730-4737） |

三个绘制后端可用的原语只有：
`fillRect` / `fillRounded` / `fillLine` / `strokeRounded` / `drawImage` / `pushClip`
（GDI 见 `app_win32.cpp:609-634`、GL 见 `src/backends/binui_gl-gdi.cpp:1541-1794`、
D3D 见 `src/backends/binui_d3d11.cpp:1280-1565`）。
**没有任何一端能填任意多边形**——全仓库唯一的 `Polygon()` 是播放按钮那个硬编码三角形
（`app_win32.cpp:1240`），GL/D3D 连 stencil 都没开。

所以"支持 SVG"不是给某个后端加一个能力，而是：**新增一种节点 + 一段路径数据 + 一个三端共用的
扫描线填充器**。

---

## 2. 目标与非目标

**P1 目标**（做完这些，Feather / Lucide / Material 这类图标可以直接粘进 `.html` 用）：

- `<path d>` 完整命令集：`M L H V C S Q T A Z`，绝对与相对，隐式重复命令
- `fill` + `stroke` 并存，`fill-rule="evenodd|nonzero"`，`fill-opacity` / `stroke-opacity`
- `<ellipse>`（`<circle>` 走 path 还是沿用现状见 3.7）
- `viewBox` + `preserveAspectRatio`（`meet` / `none`）
- `<g>` 与 `transform`（`translate/scale/rotate/matrix`，可叠加）
- 颜料属性**从祖先继承**（`<svg fill="currentColor">` 管子元素）
- 重排 / `ui_scale="proportional"` / `ui_scale="fixed"` 子树下形状不走样
- `<svg>` 正确裁剪子元素（顺手修掉 format.h 里那句错误的注释）
- 命中测试与现有节点一致（包围盒口径）

**明确不做（P1）**：`filter` / `mask` / `clipPath` / `pattern` / `marker` / `symbol` /
`<use>` / 渐变 / `stroke-dasharray` / SMIL 动画 / `<text>` / `<image>`。
这些继续按"不支持 + 说清楚换什么"告警（`kShimTags`，`binui_compiler.cpp:3988-4026`）。

**架构上不做**：GPU 三角化、stencil 填充、每端自己的路径数学。
理由：三端像素一致是这套库的既定原则（`include/binui_paint.h:1-27` 整段头注释就是这个意思），
一旦分端实现，GDI 和 GL 的边缘开始不一样，以后每个 bug 都要修三遍。

---

## 3. 设计

### 3.1 节点

新增 `BINUI_PATH = 10`：

- `include/binui_format.h:17-28`（枚举）、`include/binui_c.h:134-143`（C ABI 镜像 `BINUI_NODE_PATH`）
- 节点字段全部复用现有语义：`x,y,width,height` = **几何包围盒**（有描边时外扩 `strokeW/2`，
  这样描边不会溢出节点盒、命中区刚好盖住笔画）
- 因为 `width/height` 是**真尺寸**（不是 `BINUI_LINE` 那种"终点绝对坐标"约定），
  运行期重排**不需要给 PATH 加特判**：`relayout` 里 `relX/relY/relW/relH` 就是通用尺寸语义
  （`binui_renderer.cpp:1078/1081`）。这一条很重要——`BINUI_LINE` 的端点约定在
  `binui_renderer.cpp:1112-1123` 和 `973-983` 各要一支特判，别再复制。
- 边框 / 焦点环 / z 排序 / 脏矩形 / 动画偏移与缩放全部落在三端 switch **之后**的统一代码里
  （`app_win32.cpp:1270-1316`、`gl-gdi.cpp:2376-2419`、`d3d11.cpp:2158-2196`），不用动。

### 3.2 路径段（v15）：存折线，不存命令

```
uint32 pathBytes + uint32 pathCount
每条路径：
  BinUIPathEntry            定长 32 字节
  uint32 contourOffsets[contourCount]   // 点下标，第 i 条子路径起点；末条终点 = pointCount
  uint16 points[pointCount * 2]         // 归一化坐标，0..65535 对应包围盒内 0..1
```

```c
struct BinUIPathEntry {      // 32 字节
    uint16_t nodeId;
    uint16_t flags;          // PATH_FILL / PATH_STROKE / PATH_EVENODD
    uint32_t fillColor;      // AARRGGBB（已折入 fill-opacity / opacity）
    uint32_t strokeColor;    // 同上
    float    designW, designH;  // 编译期包围盒尺寸：运行期用它算描边缩放系数
    uint16_t pointCount;
    uint16_t contourCount;
    uint8_t  strokeW;        // 设计值（像素），运行期乘缩放系数
    uint8_t  join;           // 0=round（默认）1=miter 2=bevel —— 三者都已实现
    uint8_t  cap;            // 0=butt 1=round 2=square
    uint8_t  reserve;
};
```

**为什么存折线、不存命令**：

- 存命令要在运行期做曲线细分 + 按缩放缓存（`setSize` 每次都变，缓存失效策略比细分本身更麻烦）；
  存折线则运行期只做"归一化点 × 节点盒"的两次乘加，没有解析、没有缓存。
- 一个 24px 图标的典型 path 细分后 40–120 个点 → 160–480 字节；50 个图标 10–24KB，可接受。
- 细分精度在编译期就锁死：容差取 `max(0.1px, viewBox 对角 / 2000)`，
  已知 `<svg width>` 时按设备像素算，未知（`width="100%"`）时按 viewBox 比例算。
  250px 内放大误差 < 0.3px，肉眼看不出。
- 代价：把图标放到 1000px 高会看到折线。P3 再决定要不要"命令 + 运行时再细分"（格式留了 reserve，
  届时加 `PATH_HAS_CMDS` 位即可，不破坏兼容）。

**为什么坐标归一化到包围盒**：节点盒在重排/等比缩放下会变，归一化点自动跟着变，
路径数据一次写入永不重算。`uint16` 精度 = 1/65535 盒宽（24px 盒 → 0.0004px）。
描边宽度不归一化（它要按同一系数缩放），所以额外存 `designW/designH`。

### 3.3 编译期

1. **`d` 独立 tokenizer**。不能复用 `ParseLenPct`：`binui_compiler.cpp:275-319` 遇到
   `M10 10` 这种首字符非数字会报"无法解析长度值"并回退 0（291-298）。
   `d` 要自己按命令字母切分，支持 `1e-3`、`.5`、`1.5.5`（隐式小数点分隔，SVG 允许）、
   命令间无空格（`M10 10L20 20`）、隐式重复（`M` 后续坐标对按 `L` 处理）。
2. **命令归一化**：`H/V → L`、`S → C`、`T → Q`、`A → C`（每段 ≤90°，误差 < 0.1%），
   运行期只剩 `M / L / C / Q / Z`。
3. **flatten**：按容差递归细分；点数上限 4096/条，超限时容差翻倍重来（宁可略圆，不要截断）。
4. **viewBox → 内容盒仿射**：`scale = min(W/vw, H/vh)`（`meet`，默认 `xMidYMid`）+ 居中偏移；
   `preserveAspectRatio="none"` 时 `sx/sy` 分开算。**编译期一次性映射完**，运行期不知道 viewBox 存在。
   限制写进文档：`<svg>` 尺寸在运行期被 CSS 改变时，viewBox 的等比逻辑不重算（按包围盒拉伸）。
5. **变换栈**：`<g transform>` / 图形自身的 `transform` 叠成 2×3 仿射，作用在映射后的坐标上。
   `translate/scale/rotate/matrix` 全支持，`skew` 不支持（告警一次）。
6. **颜料继承**：`Ctx` 加 `fillColor/fillSet, strokeColor/strokeWidth/strokeSet, fillRule`，
   三段式求值（自身声明 → 父 `pc` 继承 → SVG 默认：黑填充、无描边），
   照文字色那套写法（`binui_compiler.cpp:4567-4571` 求值、`5021` 压栈；`popAndPropagate`
   `3768-3777` 只是 pop，天然回退）。
7. **透明度**：`opacity` / `fill-opacity` / `stroke-opacity` 在编译期折进 alpha（与现有
   `binui_compiler.cpp:4670-4687` 一致），不做离屏合成。
8. **`<svg>` 缺省尺寸**：有 `viewBox` → 用 viewBox 的 `w/h`；都没有 → 300×150（浏览器默认）。
   现在落到 `defaultHeight` 的通用 40（`binui_compiler.cpp:3091-3106`），图标会矮一半。
9. **标签与属性表**：`kKnownTags` 加 `path`/`ellipse`（3985），删 `kShimTags` 里对应两条
   （4012/4014），`UsedHtmlAttrs` 加 `d`/`fill-rule`/`stroke-linecap`/`stroke-linejoin`/
   `fill-opacity`/`stroke-opacity`（2305-2326），`tagToType` 加 `path/ellipse → BINUI_PATH`
   （3077-3089，否则落到 LIST）。
10. **颜料属性优先级**：`pick()`（3162-3171）让 XML 属性盖住 CSS，与 SVG 规范（呈现属性优先级最低）
    相反。只对 SVG 颜料属性用一个新的 `PickSvgPaint()`：内联 style > CSS 规则 > 呈现属性 > 继承 > 默认。
    其余属性沿用 `pick()`，不扩大改动面。

### 3.4 运行期

- **扫描线填充**（新增在 `include/binui_paint.h`，三端共用）：
  逐整数设备行求交 → 按 `nonzero`/`evenodd` 配对 → 取整成 span →
  **垂直合并相邻同 `x0/x1` 的行**（照 `BuildBorderBands` 的 `pushRun` 写法 `binui_paint.h:206-225`）。
  合并之后，一个实心图标通常是"边缘几十条 + 中间一条"，不是"每行一条"。
- **上限与降级**：`kMaxPathSpans`（初值 2048）。撞上限时行步长翻倍重算，
  保证画完而不是画一半（`BuildDashedRoundRing` 的 `binui_paint.h:435-447` 是同一套思路）。
- **描边**：折线逐段画线，转角按 `stroke-linejoin` 补缺口：圆角补一个直径 `strokeW` 的圆点，
  斜接/斜面填一个多边形（见第 9 节）。细分够密时这些补片看不出来。
  这段用模板函数写在 `binui_paint.h` 里，三端传进去的只有"画线 / 画圆 / 填多边形"，签名三端一致。
- **缩放**：`k = node.height / entry.designH`（整形钳制到 [0.05, 64]），`strokeW*k` 四舍五入。
  与边框/线宽的既有做法一致（`binui_renderer.cpp:966-972` 的 `designBorderW_`）。
  非等比拉伸时用 `height` 方向的比例——和 `fontSize` 只跟 `scaleY` 是同一处取舍，
  文档里写清楚。
- **`ui_scale="fixed"` 子树**：`binui_renderer.cpp:973-983` 那个分支要补 PATH——
  路径点靠归一化自动跟随，只需要保证 `strokeW` 跟着 `k` 走。
- **`proportional` 常规分支**（`binui_renderer.cpp:1008-1013`）只改 `x/y/w/h`，
  PATH 自动正确，不用加代码。
- **裁剪**：`clippingAncestors`（`binui_renderer.cpp:2911-2925`）把 `BINUI_SVG` 也算成裁剪祖先。
  这是修 bug，不是新功能——`binui_format.h:25` 的注释从一开始就说"绘制裁剪到容器"，
  但三端都没实现，子图形一直能画到容器外面。
- **命中**：沿用矩形判断（`binui_renderer.cpp:1311-1312`）。图标要的就是"整块可点"，
  精确命中（点在路径内）反而会让图标边角的点击失效。P3 再提供可选开关。

### 3.5 三端落点

一个节点类型只加一个 case，位置固定：

| 端 | 文件 | switch | 备注 |
|---|---|---|---|
| GDI | `src/platform/app_win32.cpp` | `:898` | 填充走 `FillPaintBands`（`:609`，自带 alpha 混合） |
| GL | `src/backends/binui_gl-gdi.cpp` | `:2120` | `fillRect` 是 `emitQuad`（`:1208`） |
| D3D11 | `src/backends/binui_d3d11.cpp` | `:1886` | `fillRect` 是 `emitQuad`（`:1024`） |

三端 case 里只写一行 `PaintPathNode(gfx, node, path, 1.0f)`（模板在 `binui_paint.h`），
其余（底色/边框/焦点环/透明度/动画）都在 switch 前后的统一代码里。
**注意**：`default: break` 是静默的——新类型忘了加 case 不会报错，只会屏幕空白。

### 3.6 链接器与挂载（最容易漏）

路径段按 `nodeId` 索引，所以组件合并时必须跟着重映射：

| 位置 | 改什么 |
|---|---|
| `src/common/binui_link.cpp:20-42` | `Doc` 加路径段成员 |
| `:352`（扩展绘制段之后、名称段 `:354` 之前） | `ParseDoc` 加一段解析 |
| `:713-720`（照 paint 段） | `MergeLayout` 按 `idMap` 重映射 `path.nodeId` |
| `:484-490` 之后 | `EmitDoc` 写出新段 |
| `src/renderer/binui_renderer.cpp:592` | `sectionAt(7)` → `sectionAt(8)`；**漏了这行，`findNodeByName` 会静默全空** |
| `binui_renderer.cpp:561-580` 之后 | 解析路径段 + `pathIdx_` 查找表（照 `paints_/paintIdx_`，`binui_renderer.h:457-458`）+ 访问器（照 `paintOf`，`:103-106`） |

### 3.7 已支持的五个图形怎么办

`<line>/<circle>/<rect>/<polyline>/<polygon>` 继续走现有映射（快，且圆角矩形走九宫格比扫描线更干净）。
只有它们带上 path 才支持的能力（渐变/虚线/fill 与 stroke 并存）时，P2 再自动降级成 PATH 编出来。
P1 里这个降级不做，但文档要写清"圆/矩形没有独立描边"。

### 3.8 版本

`BINUI_VERSION` 14 → 15。三个拒载点：`binui_renderer.cpp:208`、`binui_link.cpp:96`、`:514`；
五个写出点：`binui_link.cpp:407`、`binui_compiler.cpp:1710/5103/5196`；
测试里两处硬编码 `BINUI_VERSION == 14` 要一起改（`tests/test_css.cpp:1631/1787`）。
**旧 `.bui` 一律拒载**（历史每次 bump 都这样），`binui.res` 与 `.bum` 需一并重编——
这是升级到 0.4.0 唯一的破坏性变更，CHANGELOG 要写在最前面。

---

## 4. 阶段、提交与验收

| 阶段 | 内容 | 验收 | 估量 |
|---|---|---|---|
| **A 几何内核** | `include/binui_path.h`（d 解析 → 归一化 → 细分 → bbox → 点在路径内）；`binui_paint.h` 加扫描线 + 描边模板；独立单测 | 单测全绿；不碰格式、不碰后端，可单独提交 | ~450 行 |
| **B 格式与运行期** | v15、`BINUI_PATH`、路径段写/读、`sectionAt` 序号、link 重映射、三端 case、fixed 子树与描边缩放、`<svg>` 裁剪 | 手写一段 `.bui` 能画出图标；三端截图一致；link 合并后 id 正确 | ~400 行 |
| **C 编译器** | `<path>`/`<ellipse>`、viewBox、变换栈、颜料继承、缺省尺寸、标签/属性表、告警文案改成"已支持" | `examples/svgdemo` 用真实图标；`check_examples` 0 warning | ~500 行 |
| **D 验收与文档** | 像素基准（`Makefile` 的 `pixels` 目标 + `docs/pixels`）、README/CHANGELOG/本文档转正、C ABI 常量 | `make all tools check` 全绿；像素基准通过 | ~200 行 |

每阶段结束的标准动作（沿用本仓库惯例）：
`mingw32-make all tools -j8` **0 warning** → 四套测试全绿
（`test_elements` / `test_css` / `test_capi` / `test_font`）→ `check_examples` 11 个 UI 0 warning
→ GDI/GL/D3D 三端截图 + `pixel_diff` 比对。

---

## 5. 风险与取舍

1. **折线化的极限**：图标放大到 3 倍内基本无感，再大能看到直边。这是"运行期零解析"的代价，
   P3 可以用格式里预留的位升级成"命令 + 运行时再细分"，不破坏旧文件。
2. **性能**：填充成本 = 行数 × 每条边求交。24px 图标约 24 行；400px 高的实心图形约 400 行，
   垂直合并后条带数远小于行数。真正的大面积矢量图（比如整窗背景）不适合 `<path>`，
   留在 `<canvas>`。P3 视需要加"包围盒+颜色不变则复用 span"的缓存。
3. **属性优先级偏差**：`pick()` 让属性盖 CSS 是既有行为，本次只给 SVG 颜料属性纠回规范顺序，
   不动其它属性——避免一次改出大面积不可预期的样式变化。
4. **三端像素一致**：扫描线取整统一在 `binui_paint.h` 做，GDI 端的 `LineTo` 有历史 0.5px 偏移
   （已知问题，`docs/` 里记录在案），像素基准要按"扫描线填充走 fillRect 不受影响"来设阈值。
5. **告警翻转**：`<path>` 从"不支持"变成"支持"会翻转现有断言，动阶段 C 之前先 grep
   `tests/` 和 `examples/` 里所有 `<path>`/`viewBox` 相关用例。

---

## 6. 后续（P2 / P3）

- **P2 已完成**：`stroke-dasharray`（编译期按弧长切段，三端零改动）、
  圆/矩形带描边时自动编成路径（`AppendRoundedRect`）、
  `<defs>`/`<symbol>` 模板 + `<use href="#id">` 克隆引用（`ExpandSvgUse`；
  支持 `xlink:href`、按 x/y 平移、symbol 的 viewBox 按 width/height 缩放）。
- **P2 已完成**：`<text>` → `BINUI_LABEL`（y 当基线近似）、`<image href>` → `BINUI_IMAGE`、
  `stroke-linecap`（butt/round）。
- **P2 已完成**：`linearGradient` / `radialGradient`（格式 v16，路径记录带渐变；
  量化条带逐带取色，不需要着色器与纹理 —— 设计与实测见第 8 节）。
- **P2 已完成**：`stroke-linejoin`（round / miter / bevel + `stroke-miterlimit`，
  缺口填多边形走同一套条带填充 —— 设计与实测见第 9 节）。
- **P2 已完成**：渐变描边（`stroke="url(#…)"`，逐段取色）、`stroke-linecap="square"`
  —— 见第 10 节。
- **P3 已完成**：有填充的路径按"点在路径内"命中（环的洞 / 星形凹口不再误命中）。
- **P3 已完成（v18）**：`<polygon>`/`<polyline>` 编成路径、`<line>` 按需升级、`text-anchor`、
  `stroke-dashoffset`、`preserveAspectRatio` 的 `slice` 与 9 宫格对齐、嵌套 `<svg>` 继承祖先变换、
  径向焦点 `fx/fy`、`userSpaceOnUse` 下的百分比 —— 见第 11 节。
- **P3 已完成（v19）**：`userSpaceOnUse` 下的百分比按视口折算、路径条带跨帧缓存 —— 见第 12 节。
- **P4 已完成（v19）**：`shape-rendering` 的**边缘抗锯齿**（opt-in，默认仍是硬边）—— 见第 13 节。
- **明确不做**：`mask` / `clipPath` / `pattern` / `marker` / `filter` / SMIL 动画 /
  `<tspan>` 富文本 / 字形描边（`<text stroke>`）。这些都有明确告警，不会静默失效。
- **P3**：精确命中（点在路径内，可选）、span 缓存、命令级存储 + 运行时再细分、可选灰度抗锯齿
  （只对 path 边缘，全库仍走硬边）。

---

## 7. 实现后记（落地时才知道的）

### 7.1 实际交付

| 阶段 | 提交 | 要点 |
|---|---|---|
| A | `d970e1a` | `include/binui_path.h`（`d` 解析 / 弧转贝塞尔 / 细分 / 椭圆 / 矩阵 / 变换解析 / viewBox 矩阵 / 点在路径内 / `uint16` 归一化）；`binui_paint.h` 加 `BuildPathFillBands` / `PaintPathStroke` / `PaintPathNode`；`test_elements` 新增 `suite_svgpath` |
| B | `fd1e0ee` | v15 + `BINUI_PATH` + 路径段读写 + `sectionAt` 序号 + 链接器四件套 + 三端 case + `<svg>` 裁剪 |
| C | `63abc2a` | `Ctx` 增加 `svgXform` 与颜料继承；viewBox / `preserveAspectRatio` / `<g transform>` / `currentColor` / CSS 优先级；`stroke-width` 随缩放 |
| D | 本次 | svgdemo 图标条（6 个 Lucide 图标）+ 真实 GL 截屏；像素基准 `svgpath`；README/CHANGELOG/本文档 |

### 7.2 实测数据

- 圆弧细分精度：半径 10 的四分之一圆，容差 0.05 时**离真圆最大偏差 0.0196px**；
  椭圆归一化往返误差 **0.000076px**（`uint16` 归一化够用）。
- 填充正确性：直角三角形面积 820 vs 解析 800（1px 行取整的正常误差）；
  环的 evenodd 挖洞 4 条带、nonzero 整块 1 条带（垂直合并生效）。
- 条带上限降级：400px 的圆 `cap=16` 触发截断；`cap=256` + 行步长 8 → 47 条完整画完。
- 三端一致性：同一份 UI，GDI 与 GL **填充 0 像素差**；193 个差异像素全部落在描边边缘
  （GL 的 `fillLine` 自带抗锯齿，是既有行为，不是路径层引入的）。

### 7.3 两个踩过的坑（写进代码注释了）

1. **双重偏移**：`<svg>` 的子坐标映射一开始把 `T(内容盒原点)` 也乘进了 `svgXform`，
   而 `Ctx.ox` 本来就是内容盒原点 —— 结果整棵图形右下漂 `left+top` 那么多。
   现在的约定写死了：`绝对坐标 = (ox, oy) + svgXform × 用户坐标`，`svgXform` 里**不含位移**。
2. **`<g>` 不是布局盒子**：binUI 里任何容器节点都会把子元素原点是它自己的内容盒挪走，
   而 SVG 的 `<g>` 不建立坐标系。处理方式：`<g>` 编成零尺寸绝对定位节点，
   并把子上下文的内容盒原点**继承父容器的**（`child.ox = pc.ox`），只往下传 `transform`。

### 7.4 已知取舍

- **描边连接**：round / miter / bevel 都有了（miter 超 `stroke-miterlimit` 按规范退回 bevel）；
  转角 < 15° 的微折点不补缺口（折线是曲线细分出来的，这种缺口只有零点几像素，补了只会让
  draw call 翻倍）。
- **两条线之间**没有抗锯齿：硬边与整个库一致；GL 端 `fillLine` 的边缘是它自己的 AA，
  所以"填充逐像素一致、描边边缘略有差异"。
- **`<circle>` / `<rect>` 默认仍走轻量映射**（九宫格填充比扫描线干净）：只有写了 `stroke`
  才自动编成路径（`AppendRoundedRect` 把 `rx` 变成真圆弧），所以"圆没有独立描边"这条限制已经没了。
- **`stroke-linecap` 三种都支持**（butt / round 补半圆 / **square 两端各外延半线宽**）；
  **`stroke-linejoin` 三种都支持**（round 补圆点、miter/bevel 填多边形，miter 受
  `stroke-miterlimit` 限制），尖角图标（五角星）终于是尖的。
- **渐变描边**每段线按自己的中点取色（折线是细分出来的，逐段取色就是逐像素取色）；
  代价是 `stroke="url(#…)"` 的路径在 8px 粗描边下会有 ~2px 的色阶。
- **嵌套 `<svg>`**：祖先变换会叠加（v18 起），内层 svg 的节点盒按父变换折算成轴对齐包围盒
  （旋转时盒子取 bbox，内容仍按真变换画）。


---

## 8. 渐变（v16，0.5.0）

### 8.1 为什么是"逐条带取色"，而不是位图或着色器

三个后端的绘制原语只有 `fillRect` / `fillRounded` / `fillLine` / `strokeRounded`，
路径填充本来就是"扫描线 → 轴对齐条带"（第 4 节）。渐变有两条备选路：

- **编译期烘一张渐变位图**：要新增资源段/纹理、每端接采样器、缩放时重采样；GDI 那端还得
  逐像素填，等于在三端各写一份采样数学 —— 与"几何只写一份"的既定原则冲突。
- **把每条带的颜色从常量换成 t 的函数**（本方案）：扫描线、合并、降级、三端 `fillRect`
  全部复用，后端只多传一个 `const BinUIPathGradient*`。

所以渐变这一版在**运行期只多了一个函数**（`PathShade::At`），三端唯一的改动是把渐变指针
传进 `PaintPathNode`。

### 8.2 数据模型

`BinUIPathGradient`（92 字节）挂在路径记录里，`PATH_FILL_GRAD` 置位时才写：

| 字段 | 含义 |
|---|---|
| `type` | `PATH_GRAD_LINEAR` / `PATH_GRAD_RADIAL` |
| `spread` | `pad`（默认）/ `repeat` / `reflect` |
| `stopCount` | 2..8 |
| `x1,y1,x2,y2` | 线性：起点→终点；径向：圆心（`x1,y1`） |
| `rx,ry` | 径向椭圆两轴半径 |
| `stops[8]` | `{float offset; uint32 color}`，颜色已折入 `stop-opacity` / `fill-opacity` / `opacity` |

坐标是**包围盒归一化坐标**（设备坐标 = 节点盒左上 + 归一化值 × 节点盒尺寸），与折线点
同一套映射 —— 缩放节点盒时渐变跟着缩放，这正是 SVG `objectBoundingBox` 的语义；
`userSpaceOnUse` 在编译期就被折算进这套坐标。

径向用**两个半径**而不是一个：包围盒非正方时 SVG 的径向渐变本来就是椭圆，一个半径
表达不了（`userSpaceOnUse` 下两轴缩放不同也会走到这条路）。焦点 `fx/fy` 没做（告警）。

### 8.3 编译期：两套坐标折算

`N = Scale(1/盒宽, 1/盒高) ∘ Translate(-盒原点)`，`G` = `gradientTransform`：

- `objectBoundingBox`（默认）：`M = N ∘ Translate(fillMin) ∘ Scale(fillW, fillH) ∘ G`
  （`fill` 盒是**不含描边**的几何盒：SVG 的 objectBoundingBox 不含描边宽度）
- `userSpaceOnUse`：`M = N ∘ svgX ∘ G`（`svgX` 就是 viewBox/`<g transform>` 那一套）
- 线性：两个端点各自 `PathApply`；径向：圆心 `PathApply`，半径乘 `M` 两列的**列范数**
  （正好还原仿射变换下椭圆的轴长；含旋转/斜切时按轴对齐近似并告警）

色标按 SVG 规范清洗：缺 `offset` 时在相邻色标间**等距插值**、整体**非递减夹紧**（后者小于前者
就抬平）、`>8` 个截断并告警、单色标复制成 2 个（格式要求 ≥2）、0 色标按"找不到定义"处理。
`href` / `xlink:href` 可以继承另一个渐变的色标与未写明的属性（深度 ≤8 + 环检测）。

### 8.4 运行期：量化 → 横向切段 → 纵向合并

只做两件事，都是为了"条带数不随面积增长"：

1. **量化**：`t` 先量化到 `levels` 级、取级中心色。级内相邻行颜色**完全相等**，才能继续走
   "上下合并成高条带"那条老路 —— 否则每条带颜色都不同，400px 高的形状就是 400 条带。
   `levels` 按渐变轴在设备上的长度取：每 ~2 设备像素一级，夹在 8..96。
2. **按方向切**：颜色沿 x 变时（水平/斜向/径向），按"沿 x 走一级要几个像素"
   （`1/(levels·|∂t/∂x|)`）把这一行切成几段；纯垂直渐变 `∂t/∂x≈0`，一段都不切。
   于是横、竖两个方向的条带数都只跟 `levels` 同量级。

撞条带上限时沿用老办法按行步长降级（1→2→4→8），保证画完而不是画一半；
v16 把单条路径的条带上限从 2048 提到 4096，就是为了让 400px 的对角渐变一次画完。

### 8.5 实测（这台机器，`-O2`）

| 场景 | levels | 条带数 | 条带形状 |
|---|---|---|---|
| 100×100 垂直渐变 | 50 | **50** | 100×2（合并成 2px 高） |
| 100×100 水平渐变 | 50 | **50** | 2×100（合并成整列高） |
| 100×100 对角渐变 | 35 | 324 | 5~6 × 2~6 |
| 24×24 对角渐变（图标尺寸） | 8 | **20** | 6 × 3~6 |
| 64×64 径向 | 16 | 564 | 2 × 1~24 |
| 100×100 径向 | 25 | 1334 | 2 × 1~40 |
| 400×400 对角 | 96 | 2352（一次画完，无降级） | 5~6 × 1~4 |

像素证据（同一份 `docs/issues/assets/svggrad.html`，GDI 离屏 vs GL 窗口）：

- 11 个渐变区域（横/竖/45°/径向/宽盒径向/repeat/rotate/窄条/盾牌/圆角矩形/`<g>` 继承）
  **逐像素完全一致**：超阈值 0 像素、最大通道差 **0**。
- 差异只出现在两类既存问题处：**描边边缘**（GL 的 `fillLine` 自带抗锯齿：圆描边 116px、
  盾牌描边 212px）与**文字抗锯齿**（两行说明文字 2024 + 2249 px，最大通道差 117）。
- 像素基准新增 `svggrad`（420×260，三行：线性四样 / 径向·repeat·gradientTransform·userSpace /
  真实图标 + 渐变描边兜底），`make pixels` **6/6 零差异**。

### 8.6 已知取舍

- **色阶可见**：每级约 2 设备像素，放大到 400px 的高饱和渐变能看出台阶；级数上限 96。
  这与"不做抗锯齿、只用 fillRect"是同一条底线。
- **径向的大图条带多**：径向的色带是弧形的，横向切段逐行都不同，无法纵向合并 ——
  100px 的径向约 1334 条（真机图标尺寸 64px 是 564 条）。仍在 4096 上限内，但这是
  "CPU 软填充 + fillRect"的固有成本。
- **渐变描边不支持**（`stroke="url(#…)"` 按首个色标纯色画并告警）：描边是逐段画线，
  沿线的渐变要按弧长取色，是另一套几何。
- **径向 + `gradientTransform` 旋转/斜切**按轴对齐椭圆近似（告警）。
- **`userSpaceOnUse` + 百分比**：按"用户单位"折算（不是 viewport 比例），命中即告警并给出改法。
- **`fx/fy` 焦点**忽略（告警）；`<text>` / `<polyline>` / `<polygon>` 的渐变填充降级成纯色并告警
  （前者的颜料走文字色通道，后者编成线段节点，都没有填充通路）。

---

## 9. 描边连接（linejoin，0.6.0）

### 9.1 难在哪：后端没有"填多边形"

转角缺口是个三角形/四边形，而三端只有 `fillRect`/`fillLine`/`fillRounded`。
这一版没给后端加任何原语，而是**复用路径填充那套扫描线条带**：把缺口多边形喂给
`BuildPathFillBands`，出来的还是 `fillRect`。于是三端的改动只有一句 ——
把描边色和"填条带"的回调接进 `PaintPathStroke`。

### 9.2 几何（`PathJoinAt` 放在 `binui_path.h`）

- 归一化两段方向 `u`、`v`，叉积定"转角外侧"，两侧各偏移半线宽得 `p1`、`p2`；
- 斜接点 `m` = 过 `p1` 沿 `u` 的直线 ∩ 过 `p2` 沿 `v` 的直线；
- `ratio = |m-b| / half = 1/sin(θ/2)`，`θ` 是顶点的内角；
- **bevel** 填三角形 `(p1, b, p2)`；**miter** 填四边形 `(p1, m, p2, b)`。
  两个多边形都只和线段"擦边"，不重复覆盖 —— 半透明描边不会被叠加两次。
- `ratio > stroke-miterlimit`（默认 4）时按 bevel 画，这就是规范的"削角"行为；
- 转角 < 15° 的微折点不补（折线是从曲线细分出来的，这种缺口只有零点几像素，
  每个都补会让 draw call 翻倍）。

### 9.3 踩到的坑：斜接尖角会被自己的节点盒切平

节点盒按"半线宽"外扩几何包围盒（SVG 的 bbox 本来就不含描边），但**斜接尖角可以伸出
`(ratio-1)·半线宽`**，而三个后端都把每个节点裁剪到自己的矩形（GDI 是
`IntersectClipRect(node rect)`）。结果：`miterlimit` 调到 12 也看不出变化 ——
尖角画出来了，又被盒子的裁剪矩形削掉。

解法是让**编译期把包围盒按实际尖角撑开**：用与运行期同一个 `PathJoinAt` 逐顶点算交点，
超限（按 bevel 画）的不算，把 `minX/maxX/minY/maxY` 撑到交点。运行期条带裁剪盒再多给
`limit·半线宽` 的余量吸收取整误差。实测：五角星（72px 盒、2px 描边、`miterlimit=10`）
节点盒从 **88.00×84.08 涨到 97.19×92.43**，尖角不再被切。
`objectBoundingBox` 渐变的"几何盒"因此不能再拿"节点盒减 2·半线宽"反推（撑盒是不对称的），
改成先用局部变量记下外扩前的几何盒。

### 9.4 实测

| 场景 | 线段 | 补片 | 条带（合成后） |
|---|---|---|---|
| 五角星 round，w=2 | 10 | 10 个圆点 | — |
| 五角星 miter，w=2 | 10 | 10 个四边形 | **9 条** |
| 五角星 bevel，w=2 | 10 | 10 个三角形 | **4 条** |
| 五角星 miter，w=6 | 10 | 10 个四边形 | 33 条 |
| 五角星 bevel，w=6 | 10 | 10 个三角形 | 20 条 |

一个 72px 图标加 10 个斜接尖角，合计只多 9 条条带（每个尖角一条），开销可以忽略。

视觉与像素：

- 像素基准新增 `svgjoin`（420×260）：同一颗五角星的 round / miter / bevel 三连、粗折线
  尖角、`miterlimit=4` vs `12` 的尖刺对照、以及 star / chevron / triangle 三个真图标。
  `make pixels` **7/7 零差异**。
- 尖刺对照的量化：`miterlimit=4`（兜底斜面）最高点 y=123；`12`（真斜接）最高点 **y=103**，
  也就是尖角多伸出 20px（1/sin 换算的理论值 24.7px，差的是取整与 15° 阈值）。
- 三端一致性（GDI 离屏 vs GL 窗口）：离描边边缘 ≥4px 的内部像素 702 个中 5 个不同
  （0.7%），差异全在线段的端点/AA 边缘（GDI `LineTo` 与 GL `fillLine` 的端点取整本来
  就差 1px，与连接方式无关）；连接多边形本身逐像素一致。

### 9.5 仍然没做的

- `stroke-linecap="square"`：仍按 butt 画并告警（要方头把路径两端各延长半个线宽）。
- `miter-clip`：按普通 miter 处理（SVG2 的这个变体只在超限时把尖角改成"平头"，没人用）。

---

## 10. 渐变描边 + 方头线帽 + 精确命中（v17，0.7.0）

### 10.1 渐变描边：颜色跟着线走

描边是"逐段画线"，所以渐变描边的做法很自然：**每段线按自己的中点取一个颜色**。
折线本来就是从曲线细分出来的（24 单位的图标 ≈ 0.1px 容差），逐段取色等价于逐像素取色，
不需要给后端加渐变画刷。

- 格式：路径记录里再追加一条可选的 `BinUIPathGradient`（`PATH_STROKE_GRAD`），
  在填充渐变之后、变长表之前；两条记录各自独立，填充与描边可以同时是渐变。
- 编译期：坐标折算与填充**共用同一个** `toNormSpace()`（`N ∘ …`、同一个几何盒），
  唯一差别是 alpha 键（`stroke-opacity` 而不是 `fill-opacity`）。
- 运行期：`PaintPathStroke` 从"固定颜色"改成"回调取色"（`colorAt(x,y)`），
  于是线段、圆角连接的圆点、斜接/斜面的多边形三者颜色都一致。
  GDI 端的画笔缓存因此要从"按宽度"改成"按宽度 + 颜色"，否则渐变描边会一直用第一段颜色。
- 继承：`stroke` 与 `fill` 一样沿 `<svg>`/`<g>` 继承渐变；子元素写纯色 `stroke` 会把
  继承来的渐变清掉（两条链都要清，否则会画出"渐变 + 纯色"的混合）。

### 10.2 `stroke-linecap="square"`

方头 = 端点沿切线方向各外延半个线宽。实现只有几行（首段起点、末段终点各偏 half），
**但转角几何必须用未外延的原始端点** —— 否则拐角处会多出一小截。

### 10.3 精确命中（P3 首项）

`BINUI_PATH` 有填充时，包围盒命中之后再按"点在路径内"（`PointInPathNorm`，与
`PointInPath` 同一套绕数/奇偶逻辑，直接吃 `uint16` 归一化点）判一次，顺带取
±1 设备像素的容差：环的洞、星形的凹口不再误命中。描边-only 的路径仍用包围盒
（1–2px 的线本来就不好点）。

### 10.4 顺手修掉的既有缺陷

`fill-opacity` / `stroke-opacity` 写在**表现属性**上时以前是静默忽略的（`fade()` 只查
`style`，没查 `attrs`）。渐变描边正好踩在这条路上：`stroke-opacity="0.5"` 必须折进
色标 alpha，所以一起修了 —— 现在按 `style > 属性` 取值。

### 10.5 实测

| 项 | 数值 |
|---|---|
| 渐变描边取色 | 10 段折线 → 首段 `FFF2000D`、末段 `FF0D00F2`（红→蓝逐段过渡） |
| 方头外延 | 线宽 10、路径 0→100：butt 段 `(0,0)-(100,0)`；square 段 `(-5,0)-(105,0)` |
| `stroke-opacity=.5` 折入 | 色标 `80FF0000` / `800000FF`，而 `fill-opacity=.25` 只影响 `fillColor`（`0x40`） |
| 精确命中 | 环带上的点命中、洞心 `hitTest` 返回 -1 |
| 像素基准 | 新增 `svgstroke`（420×260）：渐变描边四样 + 线帽三连 + 方头虚线 + 三个真图标；`make pixels` **8/8 零差异** |
| 三端一致性 | `gl_shot`（GL 后端正向渲染）vs GDI 基准：渐变填充区内部像素 **20694 个 0 差异**；`svgjoin` 内部 702 个中 5 个、`svgstroke` 内部 444 个中 12 个不同，差异全在线段边缘（GDI `LineTo` 与 GL `fillLine` 的取整/抗锯齿，与渐变无关） |

### 10.6 仍然没做的

- `<text stroke>`：连纯色描边都没有通路（LABEL 只画字，不画描边）。
- `<line>` / `<polyline>` / `<polygon>` 的渐变描边：它们编成线段节点，没有"颜色沿路径变化"
  的通道 —— 明确告警并建议改用 `<path>`。
- 径向焦点 `fx/fy`、`userSpaceOnUse` 下的百分比（见第 8 节）。

---

## 11. 收尾（v18，0.8.0）

这一版把 SVG 剩下的"能用但半截"的地方补齐，一句话概括：**元素类型齐了、坐标系闭合了、
文字锚点对了、渐变的偏心焦点真算了**。

### 11.1 `<polygon>` / `<polyline>` 编成路径

以前它们被拆成一堆 `BINUI_LINE`：`fill` 完全无效（这是真 bug —— 多边形填充是最常见的写法），
渐变描边也表达不了。现在只要**有颜料**（fill 不是 none，或写了 stroke）就编成 `BINUI_PATH`：
`points` 解析成折线点、`polygon` 闭合、`polyline` 开放，填充/描边/虚线/连接方式/渐变全部复用
路径那条通道。既没 fill 也没 stroke 的裸 `<polyline>` 依旧不产生节点（不凭空多一个黑块）。

### 11.2 `<line>` 按需升级

`<line>` 保持轻量 `BINUI_LINE`（省节点、命中简单），但写了 `stroke-dasharray` /
渐变描边 / `stroke-linecap` / `stroke-linejoin` 时自动升级成两点开放路径 —— 这些能力 LINE 节点
表达不了。

### 11.3 `text-anchor` 与 `stroke-dashoffset`

- `text-anchor="start|middle|end"`：`x` 从"盒子左边"变成"锚点"。用编译期的字宽估算把盒子左边
  左移（middle 移一半、end 移整段）。估算宽度与真实排版有小偏差（PDF 的 `textLength` 没有），
  所以锚定是"够用级"的精确。
- `stroke-dashoffset`：编译期把虚线相位往前推 offset 个弧长（`ApplyDashPattern` 多了第三个参数）。
  实测：pattern `10 10`、路径 0→100 时，offset 0/5/10 → 首段实线分别是 `[0,10]` / `[0,5]` / `[10,20]`。

### 11.4 `preserveAspectRatio` 的 `slice` 与对齐

`BuildViewBoxMatrix` 现在自己解析 `xMinYMin … xMaxYMax` + `meet`/`slice` + 可选 `defer`：
`slice` 取**较大**的缩放（铺满、超出部分由 `<svg>` 容器裁剪），对齐前缀决定多余空间往哪边靠。
实测（viewBox 100×50 塞进 100×100 盒）：`xMidYMid slice` → 缩放 2、`tx=-50`；
`xMinYMin slice` → `tx=ty=0`；`xMaxYMax slice` → `tx=-100`。

### 11.5 嵌套 `<svg>`：三处偏移的连环坑

内层 `<svg>` 的 `x/y/width/height` 是**父用户坐标**，而 binUI 里普通元素的 left/top 是容器像素。
三处必须同时对齐，少一处就是"图形飘一截"或"整块空白"：

1. **节点盒**：内层 svg 的矩形要过父变换取包围盒（否则子图形按变换画、盒子留在原位，全被裁掉）。
2. **内容盒 = 设备尺寸**：viewBox 映射到**节点盒**（已含父变换的缩放），不能映射到声明的用户尺寸
   —— 否则父变换被算两遍。
3. **运行期重排**：`relX/relY` 必须是"相对父内容盒"的偏移。运行期 `relayout()` 会做
   `x = parent.ox + relX`；编译期若把绝对坐标写进 relX，窗口 resize 后就会再挪一次
   （实测：`<svg x="20">` 里的方块跑到 40 —— GL 离屏会先 `setSize`，所以只有 GL 那边露馅，
   GDI 离屏直接用编译结果看不出来）。

定位这个坑的现场记录：GDI 渲染对、GL 渲染错 10px（正是内层 svg 的 x），
在 GL 的裁剪循环里加一行临时打印，看到 `node.id=3 own=(40,0 …)` —— 立刻指向"运行期重排把
2 次 x 加了起来"。

### 11.6 径向焦点的锥形语义（v18）

`fx/fy` 不再"按同心圆画并告警"：按 SVG 规范，从焦点 `F` 出发经过 `P` 的射线打到单位圆于 `Q`，
`t = |P-F| / |Q-F|`。实现是把坐标按 `(rx,ry)` 归一化后解二次方程 `|F + s·(P-F)| = 1`，取正根、
`t = 1/s`。

> 踩到的坑：二次项 `b` 的符号写反了，解出来的根是"反方向"的两个，表现是圆心处的 `t` 变成 1.0
> （应该 0.444）。判据是手算：焦点在 0.1、圆心 0.5、r=0.5 时，圆心处的 t 必须等于
> `(0.5-0.1)/(0.5+0.5-0.1) = 4/9`；修正后实测正好 0.444。

焦点落在圆外时规范没定义，编译期告警并按同心圆画。

### 11.7 实测与边界

- 像素基准新增 `svgfinish`（420×260）：polygon 填充、polygon+描边、polyline 渐变描边、
  `<line>` 升级（虚线+方头+渐变）、text-anchor 三连、`slice`、径向焦点、嵌套 svg ——
  `make pixels` **9/9 零差异**。
- 单测新增：polygon/polyline → PATH（2 个路径、0 条线段、裸 polygon 不产生节点）、
  `<line>` 仅虚线那条升级、text-anchor 的盒子左边依次左移、虚线三个相位的首段实线区间、
  `BuildViewBoxMatrix` 的 meet/slice/9 宫格矩阵、径向焦点的 4 个 t 值、嵌套 svg 的盒随
  `<g>` 缩放/平移。
- 三端一致性（`gl_shot` vs GDI 基准）：修复后嵌套 svg 区差异 125/4096（全是圆角与描边 AA），
  纯填充区（polygon 星形、径向焦点球）**0 差异**。

### 11.8 到此为止的部分（以及为什么）

`mask` / `clipPath` / `pattern` / `marker` / `filter`、SMIL 动画、`<tspan>` 富文本、
字形描边、边缘抗锯齿 —— 这些要么需要第二套绘制管线（mask/filter）、要么需要字体轮廓
（`<text stroke>`）、要么与"三端像素一致 + 硬边"的底线冲突（抗锯齿）。全部保持
"明确告警 + 说清楚换什么"，不静默。

---

## 12. 用户空间百分比 + 条带缓存（v19，0.8.1）

### 12.1 `userSpaceOnUse` 下的百分比（从"告警"变成"真算"）

以前：`gradientUnits="userSpaceOnUse"` 下 `x1="50%"` 被当成 0.5 个用户单位，画出来是一条几乎
看不见的渐变，只好告警让作者改写成具体单位。现在按规范折算：

- `x1/x2/cx/fx` 的百分比 × **视口宽度**，`y1/y2/cy/fy` × **视口高度**；
- `r` 的百分比 × `sqrt((vpW² + vpH²)/2)`（SVG 规范里"对角线等效半径"的定义）；
- 视口尺寸由 `Ctx.vpW/vpH` 带下去：进 `<svg>` 时取 viewBox 宽高（没有 viewBox 就退回声明的
  `width/height`），`<g>` 与普通容器原样继承。
- `objectBoundingBox`（默认）不变：百分比就是包围盒比例，`/100` 即正确。

实测（viewBox 100×50、`x1="0%" y1="0%" x2="100%" y2="100%"`）：折到节点归一化坐标后是
`p1=(0,0) → p2=(1,1)`，正是"用户空间的左上角到右下角"。

### 12.2 路径条带缓存

**为什么现在做**：填充条带原本每帧重算。小图标上是噪声（22 条带 ≈ 0.018 ms），但大图形不是
（300px 的环 332 条带 ≈ 0.218 ms/次，5 个就是 1.1 ms/帧）。

**怎么做的**（一份实现，三端共用 → 一致性不靠各端自觉）：

- 条目 `PathBandCacheEntry { uint64_t key, gen; std::vector<PaintBand> bands; }` 放在
  `binui_paint.h`，由 `BinUIRenderer` 按 `nodeId` 持有（`mutable`，因为绘制后端拿到的是 const）；
- key = 节点盒 `x/y/w/h` + 填充色/描边色 + 不透明度 + 两条渐变指针 + `head.flags` 的混合
  —— **任何会影响条带颜色的东西变了 key 就变**，不需要逐项追踪失效；
- 布局一变（`loadLayout` / `relayout` / `mountComponent`）整表作废并 `++gen`，条目里比对代数；
- 两条预算：单节点条带 > `kMaxCachedBands`（1024）不缓存、缓存条目总数 > 256 不再新建
  —— 一个 400px 的渐变大图不会把内存吃掉。

**实测**（`-O2`，2000 次调用同一节点）：

| 场景 | 不缓存 | 命中缓存 | 提升 |
|---|---|---|---|
| 24px 图标（22 条带） | 0.018 ms/次 | ~0.0002 ms/次 | ~86× |
| 300px 环形（332 条带） | 0.218 ms/次 | ~0.0002 ms/次 | ~1051× |

**正确性**：单测用"篡改缓存里的颜色再画一次"证明第二次真的走了缓存（画出来的是篡改后的颜色），
换颜色后 key 变化必须重算并写回；`nullptr` 路径（不缓存）照常绘制。像素基准 9/9 零差异，
说明缓存没有改变任何一个像素。

---

## 13. 边缘抗锯齿（v19，0.9.0）

### 13.1 开关：`shape-rendering`（照抄浏览器语义）

| 写法 | 效果 |
|---|---|
| `crispEdges` / `optimizeSpeed` | 硬边（**binUI 的默认**，与 0.8.x 完全一致） |
| `auto` / `optimizeQuality` | 填充边缘抗锯齿 |
| 没写 | 硬边（注意：这点与浏览器不同 —— 浏览器默认 `auto` 是带 AA 的；binUI 不敢默认翻，否则所有既有 UI 的观感与像素基准一起变） |

`shape-rendering` 是可继承的表现属性，写在 `<svg>` / `<g>` 上会对整棵子树生效（实测覆盖：
`<svg>` 上写 `auto`、`<g>` 上写 `optimizeQuality` 都能让子路径带上 `PATH_AA`）。不认识的值会告警。

### 13.2 怎么做的：覆盖率条带，仍然只画 `fillRect`

不做 MSAA、不做 stencil —— 三个后端只有 `fillRect` 这一种填充原语，所以把抗锯齿做进**共用的
扫描线填充器**：

1. 每个像素行纵向做 `kPathAASubRows = 4` 次子采样，把每条子扫描线覆盖到的像素按重叠比例
   累加进覆盖率缓冲（1024 定点）；
2. 覆盖率相同的**连续像素**合并成一条带，颜色 alpha 乘以"覆盖率/255"；
3. 于是斜边上的像素被部分覆盖，台阶变成渐变 —— 三端各自合成本端自己的 alpha。

> 踩到的坑：第一版累加器用 `255/4 = 63` 做单位（整数除法），满覆盖只有 `252/255`，
> 整块面积被系统性低估 **1.2%**（单测按解析面积对账才发现：三角形实测 4940，理论 5000）。
> 换成 1024 定点后实测 5000.2。

**边界与回退**：覆盖率缓冲宽度上限 `kMaxPathAAWidth = 4096` px，更宽的形状返回 -1 直接退回硬边
（宁可锯齿也不崩）；条带数超过 `kMaxPathBands` 同样退回硬边。

### 13.3 三端一致性（实测）

| 场景 | GDI vs GL |
|---|---|
| AA 填充在**纯色背景**上（`svgaa` 用例：星形/细三角/环/24px 图标） | **0 个像素不同** |
| AA 填充叠在**渐变**上（不透明） | 114 / 7200 px（1.58%）不同，最大通道差 169 |
| 非 AA 的半透明填充叠在渐变上（既有行为，与 AA 无关） | 1440 / 7200 px（20%）不同 |

原因：GDI 没有真 alpha 合成，半透明条带走 `BlendOver(颜色, 底色)`；底色是纯色时结果与真 alpha
完全一致（所以 0 差异），底色是渐变时它只能拿一个近似底色，边缘像素就会偏。
**结论：AA 打开时像素基准只保证"纯色背景下三端零差异"**，渐变叠加属于已知近似。

### 13.4 实测数据

- 面积守恒：`M0 0 L100 0 L0 100 Z` 放到 100×100 盒子里，AA 的 alpha 加权面积 **5000.2**（解析 5000），
  硬边 5048（行取整误差）。
- 条带数：同一三角形 AA 199 条（其中 100 条是半透明边缘带），硬边 98 条 —— 约 2×。
- 像素基准新增 `svgaa`（420×260，crisp/auto 成对出现 + 渐变 + evenodd 洞 + 描边图形），
  `make pixels` **10/10 零差异**。
- `svgdemo` 的星形图标改用 `shape-rendering="auto"`，作为"开与不开"的长期样例。

### 13.5 没做与为什么

- **描边的抗锯齿**：GL/D3D 的 `fillLine` 自带 AA，GDI 的 `LineTo` 是硬边 —— 这是 0.x 起的既存差异
  （`docs/svg.md` §9 就记着），这轮没有统一它，因为 GDI 要靠 `PolyPolyline` + 手写覆盖率才能做到，
  等于重写描边通路。
- **默认开 AA**：会改变所有既有 UI 的观感，也会让 10 个像素基准全部失效，必须由作者显式打开。
