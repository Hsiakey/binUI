# binUI 问题清单（用 Go + C ABI 做 GUI 时踩到的）

> **状态：01–08 已修复、10 已修复**（改动见 [CHANGELOG.md](../../CHANGELOG.md)）；
> **09 不修（WON'T FIX）**：v0.16.17 起 GDI 后端标记为**遗留（OLD）后端**，只保证不崩/不阻塞/能出基准图，
> 新特性只做 GL / Vulkan / D3D11 三端；
> **11 = box-shadow 多层环（性能，试做已回退，等金标准策略）；12 = 字体/字形归属提案（待拍板，未实施）；
> 13 = 绘制侧还剩哪些该交 GPU（审计：先换执行体，再搬三件每像素数学）**。

复盘环境，所有条目都在同一套环境里复现：

```
binUI 0.2.0 · BINUI_VERSION=14 · commit 88931ee
Windows 11 · MinGW-w64 gcc · Go 1.26（cgo）
绘制后端：OpenGL 3.3（binui_app_open 用的那个）；对照用 GDI（offscreen_render）
```

复现用的宿主：`C:\aaa_mydata\projects\ovoagent\ovoagent\gui\`（Go + `binui_c.dll`，
一份 DSH 风格的聊天界面布局）。截图里那张界面就是它。

## 清单

| # | 标题 | 类型 | 严重度 | 一眼结论 |
|---|---|---|---|---|
| 01 | [圆角接近「短边一半」时退化，胶囊/正圆画不出来](01-gl-radius-degrades.md) | bug | **高** | 九宫格拼接在中段宽度变负时失效；`<circle>` 正好踩中 |
| 02 | [`binui_app_destroy()` 在窗口仍打开时崩（约 50%）](02-app-destroy-crash.md) | bug → **归因更正** | P0 | 复验：旧码 + 宿主加锁 16/16 不崩 → 真因是跨线程（见 08），`close()` 的顺序只是隐患 |
| 03 | [`display:none` 只跳过元素本身，子树被重挂到根](03-display-none-subtree-leak.md) | bug | 高 | 子元素跑到左上角，且**零警告** |
| 04 | [按钮默认底色写死 `#222222` + 文字强制居中/粗体](04-button-default-bg-and-text-align.md) | bug | 中 | 浅色主题下按钮就是黑块；左对齐只能拿空按钮当底 |
| 05 | [滚动条颜色/宽度写死](05-scrollbar-hardcoded.md) | bug | 中 | 浅色主题右侧一条黑杠，内容还被压住 8px —— 配色与 gutter 都已修 |
| 06 | [`setText` 不重算文本高度](06-settext-no-reflow.md) | enhancement | 中 | 运行期换长文本就压到下一个元素 |
| 07 | [C ABI 缺 `scrollTo` / `setNodeBounds` / `addNode`](07-cabi-runtime-apis.md) | enhancement | 中 | 渲染器里都有，只是没导出；Go 绑定还漏了 `set_image*` |
| 08 | [Go 绑定文档说错「主 goroutine 天然固定线程」](08-go-binding-thread-affinity.md) | documentation | **高（踩了就必现）** | 宿主不 `LockOSThread` → 跨线程调 Win32/WGL → 随机崩或退出挂死；**02 的崩溃就是这条** |
| 09 | [GDI 的文字矩形比节点盒小一圈，大字号被上下裁掉](09-gdi-text-clip.md) | bug（观感）→ **不修** | 低（GDI 已降级为遗留后端） | `DrawTextW` 按文字矩形裁剪；GPU 三端完整。留作记录 |
| 10 | [局部重绘剔除判据写反了一侧（hover 后冒出"影子/边框"）](10-partial-redraw-cull.md) | bug | **高** | 四端都有；只出现在**局部重绘**路径，整帧像素基准看不见；判据收成 `PaintNodeInClip` 一份实现 + `make partial` 对拍 |
| 11 | [box-shadow 被近似成多层环，吃掉 64% 的圆角填充像素](11-shadow-rings.md) | perf | 中（不影响正确性，挡 120 fps） | 每帧 fillRounded 的 63.8% 像素是阴影环，且环是**整块**画的（同一块 cache line 写 5 遍）。环带化实测**性能平账、像素必变**（±2 缝隙 / 页面 1.3%）→ 已回退，等金标准策略 |
| 12 | [字体/字形处理该归谁：bin2d 收字形，binUI 留排版语义](12-font-layer-ownership.md) | 提案（架构 / 分层归属） | 中（不阻塞第 2 步，但挡"bin2d 能画字"） | 字形光栅化 / 整形 / 图集该进 bin2d（建议 `libbin2d_text` 可选模块）；CSS 排版语义留 binUI；平台字体发现由外壳注入。**动手前先统一两份互相矛盾的 layering 文档** |
| 13 | [绘制侧还剩哪些该交给 GPU：先换执行体，再搬三件每像素数学](13-gpu-migration-candidates.md) | 审计 + 提案（performance / 架构） | 中（不阻塞，决定下一轮优化方向） | 实测 CPU 绘制只剩 36 / 19 µs/帧，**再搬任务是省微秒**；大头是 bin2d 整条 CPU 光栅换 binGPU 执行体（12.11 → 0.75 ms，16×）；真该搬的三件：颜色矩阵滤镜、SDF 阴影、SDF 线段（圆角模板 SDF 已就绪只差接线）；搬前先统一 AA 定义 |

## 修复顺序建议

1. **02**（崩溃）—— 唯一的稳定性问题，而且触发条件是"文档示例里的标准写法"。
2. **01**（圆角/圆形）—— 观感上限，且它同时是 `<circle>` 失效的原因。
3. **03**（display:none）—— 静默错位，最难被发现。
4. **04 / 05**（默认样式）—— 各几十行，浅色主题的门槛。
5. **06 / 07**（运行期能力）—— 设计决策，改动面大，建议一起排期。

实际按这个顺序修完了；07 里"不升 `BINUI_C_ABI_VERSION`"的判断成立（纯增量新增）。

## 复现资产（`assets/`）

| 文件 | 用在哪条 | 说明 |
|---|---|---|
| `radius-matrix.html` / `-gl.png` / `-gdi.png` | 01 | 200×200 六档半径 + 八种小盒子，GL 与 GDI 对照 |
| `radius-threshold.html` / `-gl.png` | 01 | 100×100 扫 r=30..50，看凹口从哪一档开始 |
| `circle.html` / `circle-gl.png` | 01 | `<svg><circle>` 六种直径，全是方块 |
| `display-none2.html` / `-gl.png` | 03 | 内联 / class 两种 `display:none`，子块都跑到左上角 |
| `probe.html` / `probe.png` | 04 | 三个按钮的 `text-align:left` 全部无效 |
| `scrollbar-light.png` | 05 | 浅色主题下右侧那条黑杠 |
| `gui-dark-chat.png` | 全部 | 完整界面（深色），说明这些限制在真实布局里的表现 |
| `gdi-text-clip.html` / `-gl.png` / `-gdi.png` | 09 | 同一页 `<div>`（被裁）与 `<span>`（完整）的 GDI / GL 对照 |

> `assets/` 里的 HTML 都是 self-contained 的，可以直接
> `build-mingw/bin/offscreen_render.exe <html> out.png` 跑 GDI 结果；
> GL 结果需要用 C ABI 建窗（我就是用上面那个 Go 宿主加载的）。
> `docs/issues/` 不在 `check_examples` 的扫描范围（它只扫 `examples/`），不会影响 CI。

## 已排除：这些不是 binUI 的问题

留一份，免得重复排查：

- **截图拿到空位图 / `GetDIBits` 返回 0** —— 我自己的抓屏代码（应该用
  `CreateDIBSection` 直接读内存，或者注意 `GetDIBits` 要求位图不能还选在 DC 里）。
  `PrintWindow` 对 GL 窗口抓到全黑也是预期：GL 画面不在 GDI 表面里。
- **`SendKeys` 打进输入框后空格消失** —— 宿主开着中文输入法，按键被 IME 吃掉了；
  用 IME 无关的方式注入（`WM_CHAR`）就正常。不是 `BINUI_EV_INPUT` 的问题。
- **`go vet` 报 `possible misuse of unsafe.Pointer`** —— 在 `bindings/go/binui/binui.go`
  的上游代码里，见 07 的最后一段（顺手提了，算绑定侧的小活）。
