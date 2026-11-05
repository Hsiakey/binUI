> **状态：已修复（机制）** —— close() 改为“摘窗口过程 → 隐藏 → 拆 GL → DestroyWindow”，并给 WM_PAINT 加就绪守卫，销毁途中的同步消息不再进入本库代码、也不再有悬垂 this。
> 说明：原宿主（Go + cgo 聊天界面）未入库，本地用最小 C 复现程序跑 12 次未复现出 50% 概率，所以这条依据的是代码路径分析而不是现场复现。

# `binui_app_destroy()` 在窗口仍打开时崩（约 50%，AV @ null+0x1528）

> 建议标签：`bug` · 严重度：崩溃 · 面积：C ABI / 绘制后端（GL）
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 平台：Windows 11 + MinGW-w64 gcc（Go 1.26 cgo 宿主）
> 绘制后端：OpenGL 3.3（`binui_gl-gdi`）
> 编译警告：无

## 现象

按 `include/binui_c.h` 顶部写的最小流程跑：

```c
app = binui_app_create(layout, len, NULL);
binui_app_open(app, 0, 0, BINUI_OPEN_RESIZABLE, "…");
while (binui_pump(app, 8) == 0) { /* 排空事件 */ }
binui_app_destroy(app);        // ← 这里炸，约 50%
```

崩溃特征（Go 宿主打印的运行时异常）：

```
Exception 0xc0000005 0x0 0x1528 0x7ffd…        ← 写地址 0x1528（空指针 + 偏移）
signal arrived during external code execution
ovoagent-gui/binui._Cfunc_binui_app_destroy(...)
main.main.deferwrap1()                        ← 我的 defer app.Close()
```

写地址 `0x1528` 每次一样（不同构建之间保持一致），说明不是内存踩踏，而是
**某个本该非空的成员在偏移 0x1528 处被写**。

## 关键：触发条件已二分出来

| 退出方式 | 结果 |
|---|---|
| **A. 循环里主动 `break`，此时窗口还开着**，然后 `destroy` | **4 / 8 崩**（约 50%） |
| B. 发 `WM_CLOSE` 让窗口先关掉 → `pump` 返回 1 → 再 `destroy` | 6 / 6 干净 |
| C. 完全没有窗口交互，`pump` 1 秒后 `destroy`（等价于 A） | 2 / 6 崩 |
| D. 项目自带 `binui_c.dll` + Go 绑定演示（`build-mingw/bin/go-demo.exe`），用 `WM_CLOSE` 退出 | 6 / 6 干净 |
| E. 项目自带 GDI 演示 `build-mingw/bin/features.exe`，用 `WM_CLOSE` 退出 | 6 / 6 干净 |

也就是说：**窗口已经关掉时 `destroy` 是安全的；窗口还开着时 `destroy` 有一半概率崩。**
B/C 的差别只在"关窗是否已经走过 `pump`"。

## 最小复现

C 语言侧就是上面那 5 行；宿主要做的是"开窗 → pump 若干帧 → 不关窗直接 destroy"。

Go（cgo）侧最省事：

```go
app, _ := binui.New(layout, "")
app.Open(0, 0, binui.OpenResizable, "repro")
for i := 0; i < 120; i++ {     // 约 1 秒
    if !app.Pump(8) { break }
    for { if _, ok := app.Poll(); !ok { break } }
}
defer app.Close()              // 等价 binui_app_destroy，崩在这里
```

跑 8 次，观察退出码（0 = 正常，2 = Go 捕获到的 AV）：

```powershell
1..8 | % { Start-Process .\repro.exe -Wait -PassThru | % { $_.ExitCode } }
```

我的宿主实测同一份二进制：4/8 崩、4/8 干净，换机器/换负载比例会有波动。

## 根因线索

`src/binui_c.cpp:492 binui_app_destroy()` → `a->shell->close()`。

`src/backends/binui_gl-gdi.cpp:2719 close()`：

```cpp
void BinUIAppGLGdi::close() {
    running_ = false;
    gfx_.shutdown();        // 先拆 GL（含 ReleaseDC）—— 注释说"必须在窗口销毁前"
    if (hwnd_) {
        DestroyWindow(static_cast<HWND>(hwnd_));   // ← 窗口还活着时，这里会同步派发消息
        hwnd_ = nullptr;
    }
    if (!childMode_) { MSG msg = {}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {} }
    childMode_ = false;
}
```

最可能的过程：窗口还开着时，`DestroyWindow()` 会**同步**派发 `WM_PAINT` /
`WM_NCDESTROY` 给 `wndProc`，而 GL 上下文已经被 `gfx_.shutdown()` 拆掉了；
`wndProc` 里若有绘制/`impl_->…` 访问，就会写到已析构（或置空）的成员上 ——
和"写地址 0x1528 恒定"吻合。

窗口已关的路径（B/D/E）恰好绕开了这一步：`WM_CLOSE` 的处理器自己 `DestroyWindow`
并把 `hwnd_` 置空，`close()` 里 `if (hwnd_)` 不成立，不再二次销毁，所以干净。

> 这一段是推断。确认方法：在 `close()` 里 `gfx_.shutdown()` 与 `DestroyWindow()`
> 之间插一条 `OutputDebugString`，并在 `wndProc` 的 `WM_PAINT` / `WM_NCDESTROY` 分支
> 各插一条，看崩溃前最后一条是哪条；或者直接 `WinDbg` 抓一次（本机没有调试器，
> 所以我只做到了行为层面）。

## 建议修法

任选其一（1、2 都改最稳）：

1. **拆 GL 之前先让窗口不成立**：先 `SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, DefWindowProcW)`
   或 `ShowWindow(hwnd_, SW_HIDE)` 再 `gfx_.shutdown()`，最后 `DestroyWindow`；
   隐藏窗口可以让 `DestroyWindow` 不再产生一次真实的绘制。
2. **绘制路径加就绪判断**：`wndProc` 的 `WM_PAINT` 分支和所有绘制入口先判
   `if (!impl_->ready) { ValidateRect(hwnd_, nullptr); return; }`。
   `fillRounded()` 已经这么判了（`:1383`），说明这个防御本来就有，只是没覆盖全。
3. 顺带：`binui_c.h` 说 destroy "内部会先关窗口"，那就该保证这条语义成立；
   或者反过来在文档里明确"必须先收到窗口关闭（`pump` 返回 1）再 destroy"，
   但这和"最小可用流程"那段示例矛盾，建议按 1/2 修，别改文档。

另外：我的宿主里只要改成"抓完图 `os.Exit()` 不调 destroy"就 6/6 稳定，
可作为"确实卡在 destroy"的旁证。
