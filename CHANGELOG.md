# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
`【格式 vN】` 指 binUI 二进制布局的 `BINUI_VERSION`（`include/binui_format.h`），
与语义化版本不是一回事——**格式号只在二进制布局变化时递增**。

## [Unreleased]

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
