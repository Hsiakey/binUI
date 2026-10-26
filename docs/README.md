# 文档索引

| 文档 | 内容 |
|---|---|
| [../README.md](../README.md) | 总览：组成、构建、UI 描述格式（含 CSS 支持矩阵）、二进制布局、图表 API、视频模块 |
| [app_layout.md](app_layout.md) | **应用散装形态**：exe 真实路径解析、UI 文件 6 级查找顺序、`src` 相对规则、`.bex` 容器的映射契约 |
| [components.md](components.md) | **组件化**：`<use src="x.bum.xml">` 编译期内联、参数/合并/CSS 作用域规则、`.bum` 二进制与运行时挂载的规划 |
| [bex_container.md](bex_container.md) | **`.bex` 单文件容器规范（设计归档，暂不实现）**：ELF 自定义段 / PE 资源节承载 binUI 布局、节布局与 trailer |
| [img/](img/) | 文档用图：默认布局渲染、字体渲染对比、伪类演示 |
| [mockups/](mockups/) | 演示的**设计稿/对照稿**（HTML，不参与构建，允许出现未支持的写法）：`galgamechat.html`、`singlewindow.html` |

## 图片

| 文件 | 说明 |
|---|---|
| [img/default-layout.png](img/default-layout.png) | 内置默认布局（`GenerateDefaultBinUI()`，480×800）在 GDI 后端下的渲染结果 |
| [img/cssshow.png](img/cssshow.png) | `examples/cssshow` 的离屏渲染（伪类/伪元素演示；静态呈现，`:hover` 等需真实交互） |
| [img/components.png](img/components.png) | `examples/components` 的离屏渲染（组件内联：标题栏 / 按钮 / 组合卡片） |
| [font_rendering_before_after.png](font_rendering_before_after.png) | 字体渲染修正前后对比（像素对齐 + gamma + 轻量 hinting + advance 修正） |
| [font_hinting_modes.png](font_hinting_modes.png) | `BINUI_FONT_HINT=0|1|2` 三种 hinting 模式对比 |

## 相关外部文档

- [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) —— 本仓库 CHANGELOG 格式
- [Contributor Covenant v2.1](https://www.contributor-covenant.org/zh-cn/version/2/1/code_of_conduct/) —— 行为准则
