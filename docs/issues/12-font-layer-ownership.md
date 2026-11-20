# 12 - 字体 / 字形处理该归谁：bin2d 收字形，binUI 留排版语义

**状态**：**提案，已开工**（第 2 步 0.37.0 完成、§5 步 0 文档口径已于 2026-11-21 统一、步 1/2 已落地；实施中，文字实现仍全在 binUI）。
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
| 图集打包与缓存（含 `text-shadow` 的模糊图集） | **bin2d 文字模块** | `binui_glyph_atlas.h`；0.37.1 起位图经 bin2d 的外部纹理通道（`GpuTexCreate/Upload`）上传 | 同上；"几千个小位图怎么进一张纹理"是绘制实现，不是 UI 语义 |
| shaping（HarfBuzz：连字、复杂脚本、簇切分、逐字形回退） | **bin2d 文字模块**（建议） | `binui_font_renderer.cpp` | 只认"脚本 + 字符串"，不认识 DOM；与字形紧邻，分开维护成本更高 |
| `line-height` / `white-space` / `text-align` / `text-decoration` / 换行策略 / `text-overflow: ellipsis` / bidi 方向 | **binUI** | `binui_draw_nodes.h` + `binui_glyph_atlas.h` | 这些是 CSS 盒模型语义，bin2d 不该认识 |
| "文本节点 → 布局盒"（换行、折行、行高、基线在盒子里的位置） | **binUI** | `binui_draw_nodes.h` | 依赖盒模型 |

边界一句话：**"给一段字符串 + 一个 face + 字号 → 还你一批字形位图、advance 和图集纹理"这半截归 bin2d；
"这段字在哪个盒子里、行高多少、怎么折、对齐到哪"归 binUI。**

## 2b. 接口草案（给实施者，§6 拍板后再定名）

```cpp
// bin2d/text.h（只有 libbin2d_text 才编这一份，核心库零依赖）
namespace bin2d::text {

struct FaceDesc { std::string family; float sizePx; int weight; bool italic; };

// 字体发现由外壳注入：bin2d 不碰 EnumFontFamiliesEx / fontconfig / CoreText
using FaceResolver = std::function<const void*(const FaceDesc&)>;   // 返回 FreeType face 句柄

class Library {
public:
    void setResolver(FaceResolver);        // 注入平台发现
    bool init();                           // FreeType 初始化（失败 = 文字不可用，不致命）
};

// 一次排版请求：字符串 + face + 字号 → 字形序列（shaping 在这里）
struct Run { std::vector<GlyphId> glyphs; float advanceSum; };

// 图集：与 bin2d 的 GpuTexCreate/Upload 同一套句柄，CPU / GPU 两条出口共用
class GlyphAtlas {
public:
    // 返回设备纹理句柄 + 格子矩形 + 提前量；未命中时当场光栅化并只上传那一格
    bool ensure(GlyphId, uint64_t& tex, Rect& uv, float& advance);
    bool invalidate();
};

} // namespace bin2d::text
```

边界一句话：**它不认识 CSS 属性、不认识节点、不认识窗口**；它接 `(face, size, glyph)` 与字符串，
还你位图 + 推进量。`line-height` / 换行 / 对齐仍由 binUI 用这些原语自己算。

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

## 5. 路怎么走（按 2026-11-21 的现状更新）

| 步 | 做什么 | 状态 | 冒烟线 |
|---|---|---|---|
| 0 | **统一文档**：以 `binUI/docs/backend_layering.md:48` 为准（文字归 bin2d），把 bin2d 侧三处改成"待迁" | **未做**（bin2d `layering.md:78/96/187` 仍写"留"） | 两份文档说法一致 |
| 1 | layering 第 2 步（`Canvas` → binGPU 执行体） | **已完成（0.37.0）** | — |
| 2 | 接口先定：字形位图 → 设备纹理的上传通道 | **已完成（bin2D 0.22.3 的外部纹理通道）** | `GpuTexCreate/Upload/Destroy/DrawTexture` 已在用（`binui_bin2d.cpp` 的 `GlyphHost2D`） |
| 3 | 搬实现：`libbin2d_text`（face 加载 + 光栅化 + 图集 + shaping） | **未做** | `pixels-bin2d` 0 容差 + `pixels-bin2d-gpu` 不放宽 |
| 4 | 删 `binui_glyph_atlas.h` 与 `binui_font_renderer.cpp`，不留开关 | **未做** | 构建绿、`make test` 全过 |

**为什么现在可以开工**：第 1、2 步都已落地，迁移不再被别的步堵住；而且第 2 步走的是 bin2d 的通用
纹理通道（不是专门为字形开的接口），说明"文字模块"只需要在它上面加"从字符串到字形"这一层，
不用再挖新管道。

## 6. 需要拍板的三件事

- [ ] **形态**：`libbin2d_text` 可选模块 / 独立 `binfont` 仓库 / 直接并入 bin2d 核心（不推荐，破坏零依赖）
- [ ] **shaping 进不进**：HarfBuzz 跟着字形一起进 bin2d，还是留在 binUI（留在上层则 bin2d 只收"字形序列"）
- [ ] **字体发现接口**：bin2d 只定义"注入 face + 平台回调"的契约，具体枚举由 binUI 外壳实现（推荐），
      还是 bin2d 自带 fontconfig/Win32 两套实现

## 7. 验收线（按现在的门禁口径）

1. `make pixels-bin2d`（CPU 权威，**容差 0**）逐像素不动；
2. `make pixels-bin2d-gpu`（GPU 唯一门禁，0.5% / 单通道 8，`PIXEL2D_GPU_KNOWN = text-shadow-multi`）
   不放宽 —— 搬字体不该新增 known 页；
3. `make partial`（脏矩形重绘对拍，**0 容差**）全绿；
4. 字号 / 字宽 / 换行位置 / 基线 / 行高与现在**逐字节一致**（最容易悄悄变的东西）；
5. 彩色字形（COLR/CPAL）、豆腐块、逐字形回退、多层 `text-shadow` 四个用例不缺；
6. `bin2d` 的 `make layers` 仍绿，且在没有 FreeType 的机器上核心库（不链 `libbin2d_text`）仍能构建；
7. binUI 侧不再出现 FreeType / HarfBuzz 的 include。

## 8. 明确不做（免得改的时候跑偏）

- **不把 CSS 排版搬进 bin2d**：`line-height` / `white-space` / 对齐 / 装饰 / 省略号 / bidi 方向，一条都不搬。
- **不让 bin2d 碰平台 API**：字体枚举、DPI、窗口，全部由外壳注入。
- **不为迁移保留双路径开关**：搬完就删旧实现，不留 `BINUI_USE_BIN2D_TEXT` 这类开关。
- **不顺手改字形像素**（hinting / gamma / 亚像素定位）：这一步只搬位置，像素必须逐字节不变；
  想改画质另开一版，走 `pixels-bin2d` 重生基线。
