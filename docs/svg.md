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
- **P2 待做**：`stroke-linecap="square"`、渐变描边、径向焦点 `fx/fy`、
  `userSpaceOnUse` 下的百分比（目前按用户单位折算并告警）。
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
- **`stroke-linecap` 支持 butt/round**（round 在两端补圆点，square 按 butt 画并告警）；
  **`stroke-linejoin` 三种都支持**（round 补圆点、miter/bevel 填多边形，miter 受
  `stroke-miterlimit` 限制），尖角图标（五角星）终于是尖的。
- **嵌套 `<svg>`**：内层 svg 自己的 `transform` 与 viewBox 会重新起算，祖先的变换不叠加
  （嵌套 svg 在图标素材里极少见，暂不处理）。


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
