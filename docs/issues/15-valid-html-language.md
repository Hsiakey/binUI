# 15 - 源码成为合法 HTML：把"浏览器预览"变成语言契约

**状态**：**M0 完成、M1 完成（2026-11-21）**——语法定稿见 §2.1；双跑样例
`examples/valid-html/` 已归门禁（18 页 0 告警）。**M1 四项全落地（0.38.8 / 0.38.9）**：
`var(--name, fallback)` 编译期展开、`data-component` 内联（宿主即组件根）、`data-param`
文本覆盖、窗口 `data-*`。**M2 部分有**（`tools/binui_dev.py` 已能热刷新 + 看结构，
缺 `binui-preview.js` 与组件独立视图）；M3 存量迁移、M4 语义对齐未开工。
（2026-11-21 master 定方向：走合法 HTML，路难但收益大）
**类型**：语言 / 工具链 / DX · **严重度**：高（决定"写 UI 是不是像写网页"）
**影响面**：编译器（`binui_compiler.cpp`）、全部存量布局（`examples/`、`components/`、`docs/mockups/`）、
新工具（`binui dev` + 预览运行时）。
**相关**：[14-browser-preview-dx.md](14-browser-preview-dx.md)（镜像页原型，本方案是它的长期解）。

> 一句话：目标不是"生成镜像"，而是**源码本身在浏览器里就能跑**——写完 `.html` 直接打开、
> 改一行刷新即见；binUI 编译**同一份文件**，像素仍以 `bin2d_shot` 为准。
> 代价分两段：**语法迁移**（1~2 周，机械）和**布局语义对齐**（深水区，1~2 月迭代）。

---

## 1. 目标（用户视角）

```sh
binui dev main.html        # 起本地服务 + 监听文件
# 浏览器打开 http://localhost:xxxx/main.html → 看到真实布局（近似像素）
# 改源码保存 → 自动刷新
binui_compile main.html -o main.bui   # 同一份文件，编出发布用二进制
```

三条硬要求：

1. **合法 HTML**：浏览器不装任何 binUI 相关东西也能把页面渲染出来（组件内联除外，见 §4）；
2. **同一份源码**：预览和发布不分叉，不存在"预览用的写法"和"发布用的写法"；
3. **像素以 binUI 为准**：浏览器是"看得对不对"，`bin2d_shot` 是"像素对不对"，两者用同一套源码。

## 2. 四条语法迁移

| # | 现在（编译期方言） | 合法 HTML 写法 | 浏览器行为 | binUI 行为 |
|---|---|---|---|---|
| 1 | `style="width:{{w=132}}"` | `style="width:var(--w,132px)"` | 原生支持 CSS 变量，默认 132px | 解析 `var(--name, fallback)`：宿主给了 `--w` 就用，否则用 fallback |
| 2 | 文本 `{{label=按钮}}` | 直接写默认文本 `按钮` + `data-param="label"` | 显示"按钮" | 宿主有 `data-label="确定"` 时覆盖文本 |
| 3 | 无单位 `width:120` | `width:120px` | 原生（浏览器本来就要求单位） | **今天就收 `px`**（`ParseLenPct`），零改动 |
| 4 | 组件 `<use src="x.bum.html" x="20" y="64" w="120" label="确定"/>` | `<button data-component="button.html" style="position:absolute;left:20px;top:64px;--w:120px" data-label="确定">确定</button>` | 显示宿主元素本身（默认文本/尺寸），JS 负责把组件内容填进来 | `data-component` 走现有 `<use>` 内联管线（同一个 `MergeLayout`） |
| 5 | 根 `<binui width="780" height="600" bg="#F4F7FC">` / `<app_window>` | `<html><body>` + `<div data-binui-window data-width="780" data-height="600" data-bg="#F4F7FC">` | 浏览器按视口渲染，`data-*` 被忽略 | 窗口描述段从 `data-*` 读（M1；今天忽略，尺寸暂由 `<html>/<body>` 的 width/height 兜底，见 §7） |

**关键决定**：**宿主元素就是组件根**（不再"组件根标签决定语义"）。作者写 `<button data-component=…>`，
语义由宿主标签给，浏览器里也不用改标签名；组件文件只提供**内部结构 + 样式**，不再自带根元素。

### 2.1 组件契约（M0 定稿，可直接给 M1 当规格）

样例：`examples/valid-html/button.html`（组件）+ `examples/valid-html/main.html`（宿主页）。
| # | 规范 | 反例（旧写法 / 不合法写法） |
|---|---|---|
| 1 | 组件文件是**片段**：没有 `<html>/<head>/<body>`，也**不写组件自己的根标签** | `<button class="bui-btn {{kind=primary}}">…</button>`（自带根 + 模板参数） |
| 2 | **宿主元素即组件根**：位置 / 尺寸 / 底色 / 字号由宿主的 `style`（和 `--var`）给，语义由宿主标签名给 | `<use src="button.bum.html" x="20" y="64" w="120" h="38" label="确定"/>` |
| 3 | 组件只提供：① 内部子结构 ② **只作用于内部**的选择器（`.btn__label`，不碰宿主） | 组件里写 `.bui-btn{width:132px}`（把盒子写死在组件里） |
| 4 | 参数默认值 = **宿主标签里的文本**（浏览器直接可见）；宿主 `data-<param>="…"` 时覆盖 | 文本 `{{label=按钮}}`（浏览器把它当普通文字或丢掉） |
| 5 | 数值参数用 CSS 变量 `var(--name, fallback)`，逐实例覆盖就在宿主的 `style` 里写 `--name:…` | `style="width:{{w=132}}"` |
| 6 | 长度一律带 `px`；`%` / `auto` / `content` 照常用 | `width:120` |
| 7 | 窗口：`<div data-binui-window data-width data-height data-bg>` 包住页面内容 | `<binui width…>` / `<app_window width…>` |
| 8 | 组件实例的参数以 `data-*` 传递（`data-label`），不占普通属性名 | `<use label="确定">` 的裸属性 |

**同一实例的两种写法对照**（M0 样例里都跑过）：

```html
<!-- ① 目标语法：宿主即根，宽度走 var()，参数用 data-* -->
<button class="btn" data-component="button.html" data-param="label" data-label="确定"
        style="position:absolute;left:152px;top:128px;width:var(--w,120px);height:38px">次按钮</button>

<!-- ② M0 等价写法：只把宽度换成字面 px —— 这是 M1 的像素参照（两组必须逐像素相同） -->
<button class="btn" data-component="button.html" data-param="label" data-label="确定"
        style="position:absolute;left:152px;top:204px;width:120px;height:38px">次按钮</button>
```


## 3. 编译器改动（小，且每一条都能单测）

| 改动 | 位置 | 说明 |
|---|---|---|
| `var(--name, fallback)` 解析 | `ParseLenPct` / 颜色解析 | 今天告警并回退，且**回退到 0px 而不是 fallback**（宽度塌成 0，见 §7 实测）。需要：查宿主元素的同名 CSS 变量 → 取值 → **否则用 fallback 递归解析**；长度与颜色两个位置都要覆盖；顺带把"一处声明两条告警"（`width：` + `长度：`）合成一条 |
| `data-component` 内联 | 现有 `<use src>` 分支 | 与 `<use>` 共用一套：定位由宿主 `style` 给（不再是 `x/y/w/h` 属性），参数由 `--var` / `data-*` 给 |
| `data-param` 文本覆盖 | 组件参数替换 | 默认文本留在源码里（浏览器可见），宿主给了才覆盖 |
| 窗口/根 | 根尺寸段 | 从 `data-binui-window` 的 `data-*` 读 |
| 旧语法兼容 | 同上 | `{{}}` / `<use>` / 无单位继续接受，打 legacy 告警；`--strict-html` 时拒收 |

## 4. 浏览器一侧（"边看边写"）

标准 HTML 没有 include，CSS 也做不到组件内联，所以需要**一个小运行时**（不是框架）：

| 东西 | 职责 | 规模 |
|---|---|---|
| `binui dev` | 静态服务器 + 文件监听 + 热刷新（SSE）+ 自动注入运行时脚本 | ~300 行 C++（复用已有的文件改动检测思路） |
| `binui-preview.js` | 展开 `data-component`（fetch 组件文件 → 注入 `<style>` → 填入内部结构）、应用 `--var`（浏览器原生）、注入统一基础样式（`box-sizing:border-box`、默认字体/行高） | ~400 行 JS |
| 无 JS 退化 | 宿主元素仍然可见（默认文本、尺寸由 `--w` 的 fallback 给），只是没有内部结构 —— 比"乱成一团"好得多 | 0 |

为什么必须走 `binui dev` 而不是 `file://`：`fetch` 组件文件在 `file://` 下被 CORS 拦。
这也顺带解决了热刷新。

### 组件独立视图（居中 + 缩放，看细节）

单个组件在整页里太小，看不出一像素的描边、圆角弧、阴影，所以 `binui dev` 要给一条独立路由：

```
/__binui/component/components/button.bum.html?zoom=4&grid=1
```

| 元素 | 行为 |
|---|---|
| 底板 | 棋盘格（看得到透明区域）+ 组件居中 |
| 工具栏 | `1x / 2x / 4x / 8x`、`盒边界`（虚线描出组件包围盒）、`尺寸读数`（如 `400% · 528×40`） |
| 键盘 | `+ / -` 逐级缩放，`0` 复位 |
| 状态保持 | `zoom` / `grid` 写进 URL query；热刷新后不丢 |
| 进页面 | 默认按"组件自然尺寸"居中，不拉伸 |

**两种缩放是两件事，别混**：

1. **视觉放大**（DOM `transform: scale`）：浏览器会按新尺寸重新光栅化文字与矢量 —— 用于看
   阴影/圆角/渐变/配色的**效果**，文字依然清晰。
2. **像素放大**（对 `bin2d_shot` 出的 PNG 做 4× 最近邻）：用于看 **binUI 的像素**（AA 边缘、
   1px 线、字形 hinting）—— 这时看到的是真实出图，不是浏览器的矢量重绘。

前者是 `binui dev` 里点一下就有；后者在工具栏上是一个 `像素` 按钮（调一次 `bin2d_shot` 再放大）。
两条都留着，因为它们回答不同问题："看起来对不对" vs "像素对不对"。

## 5. 布局语义对齐（深水区，决定成败）

语法迁移是表层；**"浏览器里看到的就是 binUI 出图的样子"** 才是这个方向的真正成本。已知差异清单：

> **实测方法（2026-11-21）**：探针页 `build-mingw/tmp/probe-semantics.html`（七项一页）与
> `probe-wrap.html`（换行）、`probe-boxsizing.html`（box-sizing），两端各出一张 PNG：
> ```sh
> build-mingw\bin\bin2d_shot.exe build-mingw\tmp\probe-semantics.html build-mingw\tmp\probe-binui.png
> msedge.exe --headless --disable-gpu --hide-scrollbars --force-device-scale-factor=1 ^
>            --window-size=780,600 --screenshot=build-mingw\tmp\probe-browser.png ^
>            file:///D:/projects/binUI/build-mingw/tmp/probe-semantics.html
> ```
> 数字全部来自 `System.Drawing` 逐像素扫描（颜色包围盒 / 采样点 / 行数），不是推测。

| 差异点 | 现状 | 对策 | 实测（2026-11-21） |
|---|---|---|---|
| `box-sizing` | binUI 恒为 **border-box**（`width` 含 padding/border），且**忽略**作者写的 `box-sizing`（无告警、无效果） | 预览注入 `box-sizing:border-box`，写进语言规范 | `width:100px;padding:10px;border:2px`：binUI 外框 100×40（蓝面 96×36），浏览器默认 content-box 外框 **124×64**（蓝面 120×60）。显式写 `box-sizing:border-box` 后两端都是 100×40；显式写 `content-box` 时 binUI 蓝面 **3,456 px** vs 浏览器 **7,200 px**（2.08×）→ **必须注入，且禁止作者写 content-box** |
| `%` 高度基准 | binUI 编译期对**父内容盒**解析 | 规则同 binUI，写进规范；预览脚本补 `height:100%` 兜底 | 父 120×100（高度确定）+ 子 `height:50%`：两端**逐像素一致**（子 120×50）。差只出现在父高度不确定时：浏览器把 `%` 当 auto 塌陷，binUI 仍按编译期父内容盒算 |
| flex 细节 | binUI 是子集（`flex:1`、`gap`、`align-items`…） | 只用共同子集；差异项进"不推荐写法"表 | `display:flex;gap:8px;align-items:center` + 3×`flex:1` 子项：宽 **61/61/61**（binUI）vs **61/62/61**（浏览器），间距两端都是 **8/8**，`align-items:center` 落点相同（首项 y=40..59）→ 差在**分配余数舍入**（±1px/项），不在语义 |
| 文字度量 | binUI 自己的字体栈与行高 | 预览注入同一份字体 + 固定 `line-height`；接受 ±1 行换行差 | 同字体 13px `Hello 中文 Ag`：binUI 宽 **78px**（x=562..639, y=25..38），浏览器宽 **82px**（x=561..642, y=22..35）→ 宽差 4px、**基线低 3px**。换行：120px 宽中文段 binUI **4 行** vs 浏览器 **3 行**；200px 宽英文段两端都 3 行（±1 行坐实） |
| 裁剪 / `overflow` | **（修正）binUI 默认不按祖先矩形裁剪**，只有 `overflow:hidden/auto/scroll` 才裁 | 预览**不需要**给定位元素加 `overflow:hidden`（原对策是错的）；作者要裁剪就显式写 `overflow` | `overflow:hidden` 的 100×40 容器里塞 200×100 子块：被裁到 x=119（两端一致）；不写 `overflow` 的对照容器，子块溢出到盒外——binUI **8,431 px**、浏览器 **8,540 px** 落在容器外，两端都**不裁** |
| `z-index` / 层叠 | binUI 按树遍历 + 数值排序 | 只用 `z-index` 数值，不依赖 stacking context 细节 | 两个重叠盒 `z-index:1/2`：重叠点采样两端都是 **#00FFFF**（z=2 的在上）→ 数值语义一致；差在 binUI 无 stacking context（父 `z-index` 不建立新层叠上下文） |
| 阴影 / 渐变 / 圆角 | binUI 按 CSS 子集实现 | 浏览器原生更准；以浏览器为"看起来"的参照，像素仍以 binUI 为准 | 圆角 20px：角点 (160,240) 两端都是纯白（弧外）。`to right` 渐变三点 binUI **#59A4F6/#49B2D4/#39BDAF** vs 浏览器 **#5DA1FE/#48B2D3/#34C1A7**（最大单通道差 **9/255**）。`box-shadow:0 8px 24px rgba(0,0,0,.35)`：盒下 8px 处最暗 binUI **R=165** vs 浏览器 **R=201** → **binUI 阴影更浓（差 36/255）**，这是目前视觉上最明显的一条 |

**结论**：七条里 4 条**语义一致**（`%`、flex 分配规则、z-index 数值、圆角/渐变），3 条**有实质差异**
（box-sizing 未定义时差 24px、文字宽 4px/基线 3px/换行 ±1 行、阴影浓度 36/255）。
预览脚本能消掉 box-sizing 与字体（注入 CSS），消不掉阴影浓度与换行的**边缘情况**。


**工具**：`binui parity <page>` —— 同一页 浏览器截图（Edge headless / puppeteer 均可）vs `bin2d_shot`，
出 diff 图与差异率；每修一条差异就钉一页。这与 `make pixels-bin2d` 是两套口径：后者守"binUI 自身不回归"，
前者守"浏览器与 binUI 不跑偏"。

## 6. 分阶段

| 阶段 | 做什么 | 验收 | 估算 |
|---|---|---|---|
| M0 | 语法定稿 + 一个"双跑"样例（`button` 组件用新语法重写：浏览器能看、`binui_compile` 能编） | 同一份 `button.html` 两处都成立 | **✅ 2026-11-21 完成**：`examples/valid-html/{main,button}.html`；`binui_compile` 2188 B / exit 0，`bin2d_shot` 780×600 / exit 0，Edge headless 截图 4 个按钮全在；语法见 §2.1，实测见 §5/§7 |
| M1 | 编译器：`var()`、`data-component`、`data-param`、窗口 `data-*`、legacy 告警 | 新语法样例编出的 `.bui` 与旧语法**像素完全一致**（`pixels-bin2d` 0 容差） | 3~5 天 |
| — | **M1 完成（0.38.8 + 0.38.9）**：`var()`（0.38.8）、`data-component` 内联 / `data-param` 文本覆盖 / 窗口 `data-*`（0.38.9）。`test_css` 共 17 项新断言；样例两组 `diffpx=0 max=0`；`check_examples examples` 18 页 0 问题。legacy 语法仍在（打告警）→ 归 M5 | | |
| — | **M1 第一步已落地（0.38.8）**：`var(--name, fallback)` 编译期展开（长度 + 颜色；祖先继承 + 自身覆盖 + 未定义无 fallback 时丢声明并告警一次）。验收：`test_css` 7 项新断言；分组对照 `diffpx=0 max=0`；样例 0 告警 | | |
| M2 | `binui dev` + `binui-preview.js` + 热刷新 + **组件独立视图（居中 / 1x~8x / 盒边界 / 像素放大）** | 改一行保存 → 浏览器 2 秒内刷新且布局变化可见；单组件能 4× 看清 1px 描边 | 1~2 周 |
| M3 | codemod：`{{}}`→`var()`/`data-param`、`<use>`→`data-component`、补 `px` | 存量 `examples/` + `components/` 全量迁移后像素不变 | 3~5 天 |
| M4 | 语义对齐迭代（§5 表逐条）+ `binui parity` 门禁 | 示例页差异率收敛到"文字换行以内" | 1~2 月，持续 |
| M5 | 删 legacy 语法（`{{}}`、`<use>`、无单位） | 编译器只剩一套写法 | 收尾 |

## 7. 可行性探针（2026-11-21 实测，M0 样例 `examples/valid-html/main.html`）

样例里刻意放了两组宿主：**① 目标语法**（`width:var(--w,120px)`）与 **② M0 等价写法**（`width:120px`），
其余（组件、文案、坐标）完全相同 —— ② 组是 M1 的像素参照。

```html
<!-- ① 目标语法 -->
<button class="btn" data-component="button.html" data-param="label" data-label="确定"
        style="position:absolute;left:152px;top:128px;width:var(--w,120px);height:38px">次按钮</button>
<!-- ② M0 等价写法（只把宽度换成字面 px） -->
<button class="btn" data-component="button.html" data-param="label" data-label="确定"
        style="position:absolute;left:152px;top:204px;width:120px;height:38px">次按钮</button>
```

| 今天的行为（原文） | 结论 |
|---|---|
| `binui_compile examples\valid-html\main.html -o …` → `2188 字节`、`exit=0`、**0 条告警** | **合法 HTML 直接收**，无语法错误；产物可被 `bin2d_shot` 加载 |
| `bin2d_shot … examples\valid-html\main.html …` → `saved … (780x600)`、`exit=0` | 出图成立；780×600 来自 `html,body{width:780px;height:600px}`（见下条） |
| `width:var(--w,120px)` —— **0.38.8 起支持**（见 §6 M1） | ① 组与 ② 组的盒子都是 `120×38`，两组 PNG **逐像素 0 差异**（`diffpx=0 max=0`，主按钮与次按钮都比过）。曾经的"回退 0px + 一处声明两条告警"已消失 |
| `color: var(--fg, #FFFFFF)`（组件内部） | 颜色位置同样展开（`textColorOf` 实测取到 fallback）→ M1 的颜色那条已覆盖 |
| `data-component="button.html"` / `data-param="label"` / `data-label="确定"` | **0.38.9 起内联**：宿主标签与尺寸原样保留，组件的片段元素挂成宿主子节点；`data-param="label"` 的内部元素文本取 `data-label`，没有就取宿主标签里的文本（宿主文本被消费后清空，不重复画）。编译产物 2188 → 2761 字节 |
| `data-binui-window data-width="780" data-height="600" data-bg="#F4F7FC"` | **0.38.9 起生效**：`NormalizeHtmlWindow` 把它换成编译期窗口包裹层（不进节点树），`data-width/height/bg/title/resizable/transparent/aspect/window-resize/ui-scale` 翻成同名属性，窗口描述段照旧写；`<html>/<body>` 的 CSS 兜底可以删了 |
| `width:120px;height:38px`（② 组） | **零改动接收**（`ParseLenPct` 本来就认 `px`）→ 语法迁移的第 3 条无成本 |
| `lang="zh-CN"`（曾在样例里） | `warning: HTML 属性 "lang"（<html>）未实现，已忽略` —— 合法 HTML 的通用属性不在白名单；样例因此不带 `lang`（M1 要把 `lang`/`dir`/`role` 这类通用属性加进白名单或静默） |
| 两个宿主各写一次 `var()` | 每类告警只报 **1 条**（去重键 = 整条消息）→ 不会刷屏 |

**门禁影响（已解决）**：`check_examples` 对 `examples/` 下**任何告警**判失败
（`tools/check_examples.cpp:59-62`；`CMakeLists.txt:215` 把它注册成 ctest，Windows CI 跑 ctest
且**没有** `continue-on-error`）。`var()` 归零前，样例一度放在 `docs/spec/`（不在扫描范围）避开
红灯；**0.38.8 让 `var()` 归零之后已搬回 `examples/valid-html/`**，现在 `check_examples examples`
是 **18 页 0 问题**，样例因此受门禁保护。**没有给 `check_examples` 加告警白名单** ——
零告警是既有规矩，为一个尚未支持的语法开口子，后面的告警就都能藏进去。

**语法迁移本身没有拦路虎**（`px` 零成本、`data-*` 静默通过、`var()` 已落地），真正的工程量在
§4 的预览运行时、§5 的语义对齐，以及 M1 剩下的 `data-component` / `data-param` / 窗口 `data-*`。

## 8. 风险与不做

- **不做**："浏览器像素 == binUI 像素"的承诺。字体与 AA 的差异是固有的，浏览器只保证"结构、盒子、层级、配色"。
- **不做**：为兼容浏览器引入 JS 框架/打包器。运行时只有一个小脚本 + 一个静态服务器。
- **不做**：让 binUI 去解析任意 HTML（`<table>`、`<form>` 等）；语言仍是**子集 + 白名单**，
  越界写法给告警（沿用现在"未知标签只告警"的策略）。
- **风险**：存量页面迁移会碰到"从未在浏览器里看过的写法"（例如窄盒里的自动换行），
  M3 的 codemod 之后必须逐页过 `pixels-bin2d`，像素不许变。
