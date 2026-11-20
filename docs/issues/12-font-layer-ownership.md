# 12 - 字体 / 字形处理该归谁：bin2d 收字形，binUI 留排版语义

**状态**：**提案，待拍板**（第 2 步已于 0.37.0 完成，可以开工设计；文字实现仍全在 binUI）。
**类型**：架构 / 分层归属 · **严重度**：中（不阻塞第 2 步，但挡住"bin2d 能画字"这件事）
**影响面**：binUI `src/backends/binui_glyph_atlas.h`（1169 行）+ `src/font/binui_font_renderer.cpp`；
bin2d 的依赖声明与 `make layers` 边界；`docs/backend_layering.md` 第 2 步的文字部分。
**前置**：无（第 2 步已完成，见下）。

> 一句话：**字形光栅化、整形、图集缓存该搬进 bin2d（建议做成可选模块 `libbin2d_text`）；
> CSS 的排版语义留在 binUI；平台字体发现由外壳注入。** 动手前先把两份互相矛盾的
> layering 文档统一成一份。

## 0.38.0 复核（2026-11-21）

- **第 2 步已完成**（binUI 0.37.0：`BinUI2D` 把 canvas 挂上 `bin2d::GpuExec`）—— 本文原先的
  "第 2 步之前不动"自动解除，现在是"可以开工设计"的状态。
- **文字实现仍全在 binUI**：`binui_glyph_atlas.h`、`binui_font_renderer.cpp` 都还在，
  `drawText` / `drawTextWrapped` 仍由 binUI 排版 + FreeType 光栅。
- **一个对本文有利的进展**：bin2D 0.22.3 开了**外部纹理通道**（`GpuTexCreate` / `GpuTexUpload`
  / `GpuTexDestroy` / `GpuDrawTexture`，走 `canvas_gpu.h` 的窄接口），binUI 的字形图集已经改用它
  （`binui_bin2d.cpp` 的 `GlyphHost2D` + `deviceTextures`，只上传新格子那块矩形）。
  也就是说 §5 第 2 步"接口先定"**事实上发生了** —— 只是落在 bin2d 的通用纹理接口里，
  还没有"从字符串到字形"的那一层。
- **bin2D 侧的三处说法没变**（`layering.md:78/96/187` 仍写"文本归 binUI"），所以 §1 的矛盾**仍然存在**，
  而且现在更微妙：bin2d 已经能接收外部字形纹理，但"字形从哪来"仍在上层。

---

## 1. 现状：两份文档互相矛盾（这是最该先处理的一条）

同一套设计，两个仓库现在写的是相反的话：

| 出处 | 原话 | 结论 |
|---|---|---|
| `bin2D/docs/layering.md:78` | "文本排版 / 字形图集 → **binUI**（字形是几千个小位图，走 UI 的图集与 hinting；bin2d 只画它给的位图）" | 留上层 |
| `bin2D/docs/layering.md:96` | `binui_glyph_atlas.h`（1169 行）→ **留**（bin2d 只接收位图） | 留上层 |
| `bin2D/docs/layering.md:187` | "明确不做：文本排版 / 字形光栅" | 留上层 |
| `bin2D/README.md` | "**纯 CPU、无第三方依赖**"、"不排版文字（字形图集与整形属于上层库）" | 留上层 |
| `binUI/docs/backend_layering.md:48` | "文字：**bin2d 的字形图集直接产出纹理**（binGPU 已有纹理上传），不再走'CPU 逐个字形合成'" | **归 bin2d** |
| `binUI/docs/migrate-to-bin2d.md:167` | "`binui_glyph_atlas.h` 只换 `IGlyphTexHost` 的实现" | 留上层（过渡期做法） |

两份都是 2026-11-20 前后写的。**不统一，第 2 步做到文字那一段就会撞车**：一边说"bin2d 不出字形"，
一边说"bin2d 的字形图集直接产出纹理"。

## 2. 建议的拆分

判据沿用 `docs/backend_layering.md` 那一条：**这段代码认不认 CSS / 节点 / 布局？** 认 → 上层；不认 → 绘制语义层。

| 东西 | 归谁 | 现状在哪 | 理由 |
|---|---|---|---|
| 字体枚举 / 发现（`EnumFontFamiliesEx`、fontconfig、CoreText） | **外壳注入回调** | binUI `binui_font_renderer.cpp` 内部 | 平台 API。bin2d 不碰平台，这是它的既有规矩，别破 |
| face 加载与生命周期（`FT_New_Face`、回退链、豆腐块） | **bin2d 文字模块** | 同上 | 只认"家族名 + 样式"，不认识节点 |
| 字形光栅化（hinting、gamma、亚像素定位、COLR/CPAL 彩色字形） | **bin2d 文字模块** | `binui_font_renderer.cpp` + `binui_glyph_atlas.h` | 与路径展平、覆盖率光栅同一性质：纯绘制语义 |
| 图集打包与缓存（含 `text-shadow` 的模糊图集） | **bin2d 文字模块** | `binui_glyph_atlas.h` | 同上；"几千个小位图怎么进一张纹理"是绘制实现，不是 UI 语义 |
| shaping（HarfBuzz：连字、复杂脚本、簇切分、逐字形回退） | **bin2d 文字模块**（建议） | `binui_font_renderer.cpp` | 只认"脚本 + 字符串"，不认识 DOM；与字形紧邻，分开维护成本更高 |
| `line-height` / `white-space` / `text-align` / `text-decoration` / 换行策略 / `text-overflow: ellipsis` / bidi 方向 | **binUI** | `binui_draw_nodes.h` + `binui_glyph_atlas.h` | 这些是 CSS 盒模型语义，bin2d 不该认识 |
| "文本节点 → 布局盒"（换行、折行、行高、基线在盒子里的位置） | **binUI** | `binui_draw_nodes.h` | 依赖盒模型 |

边界一句话：**"给一段字符串 + 一个 face + 字号 → 还你一批字形位图、advance 和图集纹理"这半截归 bin2d；
"这段字在哪个盒子里、行高多少、怎么折、对齐到哪"归 binUI。**

## 3. 为什么这么切

1. **`DisplayList` 要能记文字，字形就必须在 bin2d 一侧可执行。** 现在的 `DisplayList` 只记几何；
   如果字形留在 binUI，文字进列表只能写成"一张位图 + 一堆 quad"，binUI 每帧仍要做字形工作，
   等于第 2 步只分出去一半。反之，列表里记 `(face, size, baseline, string, color)`，执行体自己去光栅化 /
   上传，才是"先记语义、再交给执行体"。

2. **字形是"多后端共享的一份数学"，搬走不丢像素。** 三端文字能 0 差，靠的就是共用
   `binui_glyph_atlas.h`（见该文件头注释）。bin2d 成为唯一执行体后，这份代码跟着 bin2d 走是顺的；
   留在 binUI 反而要在"binUI 遍历"和"bin2d Canvas"之间再抄一份。

3. **bin2d 不能画字是能力缺口。** 它的定位是"保留模式 2D 画布、输出预乘 RGBA8 位图"。
   一个不能画字的 2D 画布，拿给 binUI 以外的人用就是半成品；而且 `bin2d` 的
   `scene/display_list/quality` 三套工具（含 `bin2d_quality` 出图取证）都没法覆盖文字。

4. **和 SDF 那轮是同一个方向。** 覆盖率数学从"CPU 位图模板"搬到片元着色器，是"表现层数学归执行层"；
   字形光栅化是同一类东西，只是执行层从 GPU 换成了 bin2d 的 CPU 光栅。

## 4. 代价（必须明说，别当免费）

1. **破 `bin2d` 的"零第三方依赖"定位。** 要引 FreeType（FTL，需在 `THIRD_PARTY_NOTICES` 署名），
   建议再引 HarfBuzz（MIT）。**建议做成可选模块**：`libbin2d` 核心保持零依赖，
   文字单独 `libbin2d_text`（依赖 FreeType + HarfBuzz 静态库），binUI 链它。
   这样"只想画图形的人"不被强制装字体库。退一步的替代方案是独立仓库 `binfont`，与 bin2d/bin3d 平级，
   但那要多维护一个仓库。
2. **工作量约 1500 行**：`binui_glyph_atlas.h` 1169 行 + `binui_font_renderer.cpp`（FreeType + HarfBuzz +
   COLR/CPAL + 回退链 + 豆腐块 + 文字阴影模糊图集）。比 SDF 那条"加一条管线"重一个量级。
3. **`make layers` 的判据要跟着改。** 现在它 grep `Node`/`layout`/`dom`/`binui::`；文字模块进来后，
   要在检查里加一条"只能被上层注入 face 与平台回调，不许反向依赖 binui"。
4. **文档面同步**：`bin2D/README.md` 的"不是什么"、`bin2D/docs/layering.md` §3/§4/§7、
   `binUI/docs/backend_layering.md` §3 第二步、`binUI/docs/migrate-to-bin2d.md` 台账，四处要一起改。

## 5. 路怎么走（顺序不能乱）

| 步 | 做什么 | 冒烟线 |
|---|---|---|
| 0 | **统一文档**：以 `binUI/docs/backend_layering.md:48` 为准（文字归 bin2d），把 bin2d 侧三处改成"待迁" | 两份文档说法一致 |
| 1 | **先做 layering 第 2 步**（`Canvas` → binGPU 执行体，拿回 12~16× 性能差） | 与文字无关，先拿收益 |
| 2 | **接口先定**：把 `IGlyphTexHost` 从 binUI 后端接口提到 bin2d 的 `gpu.h` 旁边（"字形位图 → `BgpuTexture` 上传"），binUI 的 `binui_glyph_atlas.h` 先改成薄壳，只留 CSS 语义 | 三端像素不变 |
| 3 | **搬实现**：`libbin2d_text`（face 加载 + 光栅化 + 图集 + shaping），binUI 只注入字幕发现回调 | `make pixels` / `pixels-vk` / `pixels-d3d11` / `pixels-bin2d` 全 0 差 |
| 4 | **删**：`binui_glyph_atlas.h` 与 `binui_font_renderer.cpp` 删掉，不许留开关（沿用"替换不并存"） | 构建绿、测试全过 |

**为什么不是现在搬**：字体迁移**不改变任何一个像素**，纯粹是搬家；而第 2 步的收益（GPU 执行、
原生批处理）马上能拿。反过来先搬字体，会把第 2 步堵在"等字体模块就绪"上。

## 6. 需要拍板的三件事

- [ ] **形态**：`libbin2d_text` 可选模块 / 独立 `binfont` 仓库 / 直接并入 bin2d 核心（不推荐，破坏零依赖）
- [ ] **shaping 进不进**：HarfBuzz 跟着字形一起进 bin2d，还是留在 binUI（留在上层则 bin2d 只收"字形序列"）
- [ ] **字体发现接口**：bin2d 只定义"注入 face + 平台回调"的契约，具体枚举由 binUI 外壳实现（推荐），
      还是 bin2d 自带 fontconfig/Win32 两套实现

## 7. 验收线（搬完必须全绿）

1. 三端（GL / VK / D3D11）文字像素与现在**0 差**，`bin2d` 后端同样 0 差；
2. 字号 / 字宽 / 换行位置 / 基线 / 行高与现在**逐字节一致**（这是最容易悄悄变的东西）；
3. `bin2d` 的 `make layers` 仍绿，且在没有 FreeType 的机器上核心库仍能构建；
4. 彩色字形（COLR/CPAL）、豆腐块、逐字形回退、多层 `text-shadow` 四个用例不缺；
5. binUI 侧不再出现 FreeType / HarfBuzz 的 include。

## 8. 明确不做（免得改的时候跑偏）

- **不把 CSS 排版搬进 bin2d**：`line-height` / `white-space` / 对齐 / 装饰 / 省略号 / bidi 方向，一条都不搬。
- **不让 bin2d 碰平台 API**：字体枚举、DPI、窗口，全部由外壳注入。
- **不在第 2 步之前动代码**（文档可以先统一）。
- **不为迁移保留双路径开关**：搬完就删旧实现，不留 `BINUI_USE_BIN2D_TEXT` 这类开关。
