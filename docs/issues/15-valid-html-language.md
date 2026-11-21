# 15 - 源码成为合法 HTML：把"浏览器预览"变成语言契约

**状态**：**方案（2026-11-21 master 定方向：走合法 HTML，路难但收益大）**。未开工。
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
| 5 | 根 `<binui width="780" height="600" bg="#F4F7FC">` / `<app_window>` | `<html><body>` + `<div data-binui-window data-width="780" data-height="600" data-bg="#F4F7FC">` | 浏览器按视口渲染，`data-*` 被忽略 | 窗口描述段从 `data-*` 读 |

**关键决定**：**宿主元素就是组件根**（不再"组件根标签决定语义"）。作者写 `<button data-component=…>`，
语义由宿主标签给，浏览器里也不用改标签名；组件文件只提供**内部结构 + 样式**，不再自带根元素。

## 3. 编译器改动（小，且每一条都能单测）

| 改动 | 位置 | 说明 |
|---|---|---|
| `var(--name, fallback)` 解析 | `ParseLenPct` / 颜色解析 | 今天会告警并回退（见 §7 探针）。需要：查宿主元素的同名 CSS 变量 → 取值 → 否则用 fallback 递归解析 |
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

| 差异点 | 现状 | 对策 |
|---|---|---|
| `box-sizing` | binUI 的 `width` 含 padding/border（border-box 语义） | 预览样式统一 `box-sizing:border-box`，写进语言规范 |
| `%` 高度基准 | binUI 编译期对父内容盒解析 | 浏览器要求父元素有确定高度；规则同 binUI，写进规范 + 预览脚本补 `height:100%` 兜底 |
| flex 细节 | binUI 是子集（`flex:1`、`gap`、`align-items`…） | 只用共同子集；差异项进"不推荐写法"表 |
| 文字度量 | binUI 自己的字体栈与行高 | 预览注入同一份字体文件 + 固定 `line-height`；接受 ±1 行的换行差 |
| 裁剪 / `overflow` | binUI 默认按祖先矩形裁剪 | 预览脚本给定位元素加 `overflow:hidden`，与 binUI 对齐 |
| `z-index` / 层叠 | binUI 按树遍历 + 数值排序 | 只用 `z-index` 数值，不依赖 stacking context 细节 |
| 阴影 / 渐变 / 圆角 | binUI 按 CSS 子集实现 | 浏览器原生更准；以浏览器为"看起来"的参照，像素仍以 binUI 为准 |

**工具**：`binui parity <page>` —— 同一页 浏览器截图（Edge headless / puppeteer 均可）vs `bin2d_shot`，
出 diff 图与差异率；每修一条差异就钉一页。这与 `make pixels-bin2d` 是两套口径：后者守"binUI 自身不回归"，
前者守"浏览器与 binUI 不跑偏"。

## 6. 分阶段

| 阶段 | 做什么 | 验收 | 估算 |
|---|---|---|---|
| M0 | 语法定稿 + 一个"双跑"样例（`button` 组件用新语法重写：浏览器能看、`binui_compile` 能编） | 同一份 `button.html` 两处都成立 | 2~3 天 |
| M1 | 编译器：`var()`、`data-component`、`data-param`、窗口 `data-*`、legacy 告警 | 新语法样例编出的 `.bui` 与旧语法**像素完全一致**（`pixels-bin2d` 0 容差） | 3~5 天 |
| M2 | `binui dev` + `binui-preview.js` + 热刷新 + **组件独立视图（居中 / 1x~8x / 盒边界 / 像素放大）** | 改一行保存 → 浏览器 2 秒内刷新且布局变化可见；单组件能 4× 看清 1px 描边 | 1~2 周 |
| M3 | codemod：`{{}}`→`var()`/`data-param`、`<use>`→`data-component`、补 `px` | 存量 `examples/` + `components/` 全量迁移后像素不变 | 3~5 天 |
| M4 | 语义对齐迭代（§5 表逐条）+ `binui parity` 门禁 | 示例页差异率收敛到"文字换行以内" | 1~2 月，持续 |
| M5 | 删 legacy 语法（`{{}}`、`<use>`、无单位） | 编译器只剩一套写法 | 收尾 |

## 7. 可行性探针（2026-11-21 实测）

```html
<!-- out/probe/valid-btn.html：合法 HTML + var() + data-* -->
<button data-component="button" data-param="label"
        style="position:absolute;left:20px;top:64px;width:var(--w,120px);height:38px">主按钮</button>
```

| 今天的行为 | 结论 |
|---|---|
| 合法 HTML（`<html><body>` + 标准标签 + `px`） | **编译器直接收**（487 字节 `.bui`，无报错） |
| `width:var(--w,120px)` | 告警"不支持函数值，已回退默认值" → 这就是 M1 要补的那一处 |
| `data-component` / `data-param` | 目前被当普通属性忽略（无告警）→ M1 接线即可 |

也就是说：**语法迁移没有拦路虎，真正的工程量在 §4 的预览运行时和 §5 的语义对齐。**

## 8. 风险与不做

- **不做**："浏览器像素 == binUI 像素"的承诺。字体与 AA 的差异是固有的，浏览器只保证"结构、盒子、层级、配色"。
- **不做**：为兼容浏览器引入 JS 框架/打包器。运行时只有一个小脚本 + 一个静态服务器。
- **不做**：让 binUI 去解析任意 HTML（`<table>`、`<form>` 等）；语言仍是**子集 + 白名单**，
  越界写法给告警（沿用现在"未知标签只告警"的策略）。
- **风险**：存量页面迁移会碰到"从未在浏览器里看过的写法"（例如窄盒里的自动换行），
  M3 的 codemod 之后必须逐页过 `pixels-bin2d`，像素不许变。
