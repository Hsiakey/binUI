# binUI 问题清单（用 Go + C ABI 做 GUI 时踩到的）

> **状态：7 条全部已修复**（改动见 [CHANGELOG.md](../../CHANGELOG.md) 的 Unreleased 段）。
> 下面保留原始记录，方便回溯复现方式与当时的实测边界。

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
| 02 | [`binui_app_destroy()` 在窗口仍打开时崩（约 50%）](02-app-destroy-crash.md) | bug | **P0 崩溃** | 先 `gfx_.shutdown()` 再 `DestroyWindow()`，销毁途中的重绘踩到已拆的 GL |
| 03 | [`display:none` 只跳过元素本身，子树被重挂到根](03-display-none-subtree-leak.md) | bug | 高 | 子元素跑到左上角，且**零警告** |
| 04 | [按钮默认底色写死 `#222222` + 文字强制居中/粗体](04-button-default-bg-and-text-align.md) | bug | 中 | 浅色主题下按钮就是黑块；左对齐只能拿空按钮当底 |
| 05 | [滚动条颜色/宽度写死](05-scrollbar-hardcoded.md) | bug | 中 | 浅色主题右侧一条黑杠，内容还被压住 8px |
| 06 | [`setText` 不重算文本高度](06-settext-no-reflow.md) | enhancement | 中 | 运行期换长文本就压到下一个元素 |
| 07 | [C ABI 缺 `scrollTo` / `setNodeBounds` / `addNode`](07-cabi-runtime-apis.md) | enhancement | 中 | 渲染器里都有，只是没导出；Go 绑定还漏了 `set_image*` |

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
