# 文档索引

| 文档 | 内容 |
|---|---|
| [../README.md](../README.md) | 总览：组成、构建、UI 描述格式（含 CSS 支持矩阵）、二进制布局、图表 API、视频模块、C ABI |
| [bindings.md](bindings.md) | **C ABI 与语言绑定**：三条设计约束（定长 POD / 无回调 / UTF-8）、最小流程、线程与窗口规则、键盘与中文输入、未覆盖能力、新增语言步骤 |
| [app_layout.md](app_layout.md) | **应用散装形态**：exe 真实路径解析、UI 文件 6 级查找顺序、`src` 相对规则、`.bex` 容器的映射契约 |
| [components.md](components.md) | **组件化**：`<use src="x.bum.xml">` 编译期内联、参数/合并/CSS 作用域规则、`.bum` 二进制与运行时挂载的规划 |
| [bex_container.md](bex_container.md) | **`.bex` 单文件容器规范（设计归档，暂不实现）**：ELF 自定义段 / PE 资源节承载 binUI 布局、节布局与 trailer |
| [img/](img/) | 文档用图：默认布局渲染、字体渲染对比、伪类演示、语言绑定演示 |
| [mockups/](mockups/) | 演示的**设计稿/对照稿**（HTML，不参与构建，允许出现未支持的写法）：`galgamechat.html`、`singlewindow.html` |

## 图片

| 文件 | 说明 |
|---|---|
| [img/default-layout.png](img/default-layout.png) | 内置默认布局（`GenerateDefaultBinUI()`，480×800）在 GDI 后端下的渲染结果 |
| [img/features.png](img/features.png) | `examples/features` 的离屏渲染（伪类/扩展绘制/HTML 语义/组件化/数据驱动） |
| [img/std-components.png](img/std-components.png) | `components/preview.html` 的离屏渲染（标准组件库全览） |
| [img/animshow.png](img/animshow.png) | `examples/animshow` 的离屏渲染（关键帧 / 过渡；静态呈现，动态需真实运行） |
| [img/fontshow.png](img/fontshow.png) | `examples/fontshow` 的离屏渲染（字号 / 字重 / 斜体 / 行高 / 文字装饰） |
| [img/svgdemo.png](img/svgdemo.png) | `examples/svgdemo` 的离屏渲染（`<svg>` 矢量；`<canvas>` 需真实 GL 窗口才有内容） |
| [img/cubedemo.png](img/cubedemo.png) | `examples/cubedemo` 的离屏渲染（线框立方体；三角面同样需真实 GL 窗口） |
| [img/texturedemo.png](img/texturedemo.png) | `examples/texturedemo` 的离屏渲染（程序化纹理多尺寸采样） |
| [img/python-binding.png](img/python-binding.png) | Python 绑定演示界面（`bindings/python/demo_ui.html`） |
| [img/go-binding.png](img/go-binding.png) | Go 绑定演示界面（`bindings/go/demo/ui.html`） |
| [img/rust-binding.png](img/rust-binding.png) | Rust 绑定演示界面（`bindings/rust/ui.html`） |
| [font_rendering_before_after.png](font_rendering_before_after.png) | 字体渲染修正前后对比（像素对齐 + gamma + 轻量 hinting + advance 修正） |
| [font_hinting_modes.png](font_hinting_modes.png) | `BINUI_FONT_HINT=0\|1\|2` 三种 hinting 模式对比 |

> 绑定演示图为什么是离屏渲染：OpenGL 窗口的内容抓不到（`PrintWindow` 对双缓冲 GL
> 窗口返回全黑，`BitBlt` 只能抓被遮挡后合成到桌面的部分）。所以演示图一律走
> `tools/offscreen_render` 的 GDI 路径 —— 同一份 HTML、同一套布局引擎，界面一样，
> 但 `<canvas>` 里由应用自己画的内容不会出现。

## 相关外部文档

- [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) —— 本仓库 CHANGELOG 格式
- [Contributor Covenant v2.1](https://www.contributor-covenant.org/zh-cn/2/1/code_of_conduct/) —— 行为准则
