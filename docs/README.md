# 文档索引

| 文档 | 内容 |
|---|---|
| [../README.md](../README.md) | 总览：组成、构建、UI 描述格式（含 CSS 支持矩阵）、二进制布局、图表 API、视频模块、C ABI |
| [bindings.md](bindings.md) | **C ABI 与语言绑定**：三条设计约束（定长 POD / 无回调 / UTF-8）、最小流程、线程与窗口规则、键盘与中文输入、未覆盖能力、新增语言步骤 |
| [keyboard_focus.md](keyboard_focus.md) | **键盘焦点系统**：焦点控制 / 文本编辑 / 鼠标三态与按键表、WASD 移焦点、方向键滚哪个容器、回车两段式、焦点环（:focus-visible + 2 秒自动熄灭）、宿主接法；末尾是层级焦点导航（下潜 / 上浮）的未来开发计划（未实现） |
| [app_layout.md](app_layout.md) | **应用散装形态**：exe 真实路径解析、UI 文件 6 级查找顺序、`src` 相对规则、`.bex` 容器的映射契约 |
| [components.md](components.md) | **组件化**：`<use src="x.bum.html">` 编译期内联、参数/合并/CSS 作用域规则、`.bum` 二进制与运行时挂载、目录结构 |
| [svg.md](svg.md) | **SVG 支持（P1 已实现）**：现状实测、`BINUI_PATH` 节点 + v15 路径段的格式、编译期 viewBox/变换/继承、三端共用扫描线填充、四阶段拆分与验收 |
| [vulkan.md](vulkan.md) | **Vulkan 后端调研（结论：现在 NO-GO）**：本机实测（loader 1.4.341 / RTX 4060 Ti）、三份节点遍历的重复（749/694/530 行）与 `IBinUIGfx` 抽取方案、Windows 独立翻转对 GL/Vulkan 同样生效、透明窗口只能读回、局部重绘必须用持久累积帧、原语映射表、20~30 人日估算、GO/NO-GO 条件 |
| [vulkan_design.md](vulkan_design.md) | **Vulkan 后端设计（P1~P5）**：六条后端契约、`IBinUIGfx` 接口全文与三份遍历的合并步骤、Vulkan 模块/同步模型/交换链协商表、字节序与色卡验收线、三路 present、鲁棒性清单、12~17 人日时间线与回滚点；P3 落地记录（共享字形图集、帧内上传、`vkCmdClearColorImage` 陷阱、14 个用例的 VK↔GL↔GDI 三方 diff 表）见 §5.9 |
| [bex_container.md](bex_container.md) | **`.bex` 单文件容器规范（设计归档，暂不实现）**：ELF 自定义段 / PE 资源节承载 binUI 布局、节布局与 trailer |
| [img/](img/) | 文档用图：默认布局渲染、字体渲染对比、伪类演示、语言绑定演示 |
| [../tests/README.md](../tests/README.md) | **测试用例**：四个主题套件（CSS / 元素 / 字体与输入法 / C ABI）、无窗口能测什么、历史合并记录 |
| [mockups/](mockups/) | 演示的**设计稿/对照稿**（HTML，不参与构建，允许出现未支持的写法）：`galgamechat.html`、`singlewindow.html`。以前把它们临时编成 demo（`galgamechat`/`singlewindow`/`multiwindow`/`videodemo`）的草稿程序与产物已删除，看设计稿直接开 HTML |
| [issues/INDEX.md](issues/INDEX.md) | **用 Go + C ABI 做 GUI 时踩到的问题清单**（8 条，附复现资产与根因定位）：圆角/圆形退化、`app_destroy` 偶发崩溃（复验后更正为宿主跨线程）、`display:none` 子树泄漏、按钮默认样式、滚动条写死、`setText` 不重排、C ABI 运行期能力缺口、Go 绑定线程亲和文档纠错 |

## 图片

| 文件 | 说明 |
|---|---|
| [img/default-layout.png](img/default-layout.png) | 内置默认布局（`GenerateDefaultBinUI()`，480×800）在 GDI 后端下的渲染结果 |
| [img/flexshow.png](img/flexshow.png) | `examples/flexshow` 的离屏渲染（百分比 / `flex:1` / `min·max-width` / `overflow:auto`；拖窗实时重排是运行时行为，静态图看不出） |
| [img/buttonshow.png](img/buttonshow.png) | `examples/buttonshow` 的离屏渲染（语义色 / 尺寸 / 圆角 / 底色 / 形态，以及 `:hover`·`:active`·`:checked`·`disabled` 状态） |
| [img/features.png](img/features.png) | `examples/features` 的离屏渲染（伪类/扩展绘制/HTML 语义/组件化/数据驱动） |
| [img/std-components.png](img/std-components.png) | `components/preview.html` 的离屏渲染（标准组件库全览） |
| [img/animshow.png](img/animshow.png) | `examples/animshow` 的离屏渲染（关键帧 / 过渡；静态呈现，动态需真实运行） |
| [img/fontshow.png](img/fontshow.png) | `examples/fontshow` 的离屏渲染（字号 / 字重 / 斜体 / 行高 / 文字装饰 + ⑤ 输入法区；组字串与候选框是运行时行为，静态图里看不到） |
| [img/svgdemo.png](img/svgdemo.png) | `examples/svgdemo` 的**GL 真机截屏**（`<svg>` 矢量 + 6 个现成 `<path>` 图标 + `<canvas>` 频谱；canvas 内容离屏渲染看不到） |
| [img/cubedemo.png](img/cubedemo.png) | `examples/cubedemo` 的**GL 真机截屏**（应用自己算顶点/投影、用 `fillLine` 画的线框立方体；离屏渲染只有空画布） |
| [img/texturedemo.png](img/texturedemo.png) | `examples/texturedemo` 的**GL 真机截屏**（`updateImageRaw` 每帧上传的程序化纹理多尺寸采样；离屏渲染拿不到运行时纹理） |
| [img/python-binding.png](img/python-binding.png) | Python 绑定演示界面（`bindings/python/demo_ui.html`） |
| [img/go-binding.png](img/go-binding.png) | Go 绑定演示界面（`bindings/go/demo/ui.html`） |
| [img/rust-binding.png](img/rust-binding.png) | Rust 绑定演示界面（`bindings/rust/ui.html`） |
| [img/child-window.png](img/child-window.png) | 子窗口模式真机截屏：普通 Win32 宿主窗口里嵌两块独立 binUI 面板 |
| [img/glyph-fallback.png](img/glyph-fallback.png) | 逐字形回退与豆腐块（**GL 后端真实像素**，`tools/gl_shot.exe` 离屏抓取） |
| [font_rendering_before_after.png](font_rendering_before_after.png) | 字体渲染修正前后对比（像素对齐 + gamma + 轻量 hinting + advance 修正） |
| [font_hinting_modes.png](font_hinting_modes.png) | `BINUI_FONT_HINT=0\|1\|2` 三种 hinting 模式对比 |

> 绑定演示图为什么是离屏渲染：OpenGL 窗口的内容抓不到（`PrintWindow` 对双缓冲 GL
> 窗口返回全黑，`BitBlt` 只能抓被遮挡后合成到桌面的部分）。所以大部分演示图走
> `tools/offscreen_render` 的 GDI 路径 —— 同一份 HTML、同一套布局引擎，界面一样，
> 但 `<canvas>` 里由应用自己画的内容不会出现。
>
> 因此 **svgdemo / cubedemo / texturedemo 三张改用 GL 真机截屏**：它们的画布内容是应用在
> `drawFn` 里逐帧画的（立方体、频谱、程序化纹理），离屏渲染只能得到空画布。截屏用
> `tools/window_capture` 或 `build-mingw/tmp/drive.ps1 -Action shot`，抓窗口矩形即可。

改完示例 UI 后重出这些图（`make tools` 之后，一张一条命令）：

```sh
B=build-mingw/bin/offscreen_render.exe
$B examples/flexshow/main.html docs/img/flexshow.png
$B examples/buttonshow/main.html docs/img/buttonshow.png
$B examples/features/main.html docs/img/features.png
$B examples/animshow/main.html docs/img/animshow.png
$B examples/fontshow/main.html docs/img/fontshow.png
$B components/preview.html docs/img/std-components.png
$B --default docs/img/default-layout.png
# svgdemo / cubedemo / texturedemo 需要启动示例后真机截屏（见上面说明）
```

## 相关外部文档

- [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) —— 本仓库 CHANGELOG 格式
- [Contributor Covenant v2.1](https://www.contributor-covenant.org/zh-cn/2/1/code_of_conduct/) —— 行为准则
