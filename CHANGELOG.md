# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
`【格式 vN】` 指 binUI 二进制布局的 `BINUI_VERSION`（`include/binui_format.h`），
与语义化版本不是一回事——**格式号只在二进制布局变化时递增**。

## [Unreleased]

### 新增 — 组件化（`<use src="x.bum.xml">` 编译期内联）

- 把 UI 片段抽成组件：`<use src="标题栏.bum.xml" x="0" y="0" w="720" title="…"/>`，
  编译器在展开阶段把组件源码拼进宿主元素树 —— **运行时仍是普通节点树**，二进制格式不变，
  命中测试/动画/`--watch` 热重载全部照旧（详见 [docs/components.md](docs/components.md)）。
- 参数：组件源码里的 `{{name}}` 用 `<use>` 上的同名属性替换；`{{name=默认值}}` 让参数可选
  且不报警告；参数值按**纯文本注入**（转义 `& < >`），传进来的标签不会变成真元素。
- 合并：组件根元素属性作为默认值、`<use>` 属性覆盖；`style`/`class` 叠加而非替换；
  组件内可再引用组件（深度上限 8，超出判为循环引用并警告）。
- 组件内的相对资源路径（`<img src>`/`<video src>`）自动改写成宿主相对路径。
- 新演示 `examples/components`（`components.exe`）：标题栏 / 按钮 / 组合卡片三个组件，
  含嵌套引用与宿主改色（`bg` 参数）。

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
