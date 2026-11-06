# binUI × Go（cgo）

## 跑起来

```bash
# 1. 仓库根目录产出 C ABI（DLL + import lib）
mingw32-make SHELL=sh capi

# 2. 构建并运行
cd bindings/go
CGO_ENABLED=1 go run ./demo
```

需要 cgo 能找到 gcc（本仓库用 MinGW：把 `C:\ubin\mingw64\bin` 放进 PATH）。

运行期要有 `binui_c.dll`：cgo 只链接了 import lib（`libbinui_c.dll.a`），DLL 本身要在
`PATH` 上或 exe 同目录。演示里可以：

```bash
cp ../../build-mingw/bin/binui_c.dll .
go run ./demo
```

## 最小用法

```go
layout, _ := binui.CompileString(html)
app, _ := binui.New(layout, "")          // 第二参数：资源基准目录，可空
defer app.Close()
app.Open(0, 0, binui.OpenResizable, "Go 驱动")

count := 0
app.Run(8, func(ev binui.Event) {
    if ev.Type == binui.Click && ev.NodeID == app.Find("inc") {
        count++
        app.SetText(app.Find("count"), fmt.Sprint(count))
    }
})
```

## 包结构

- `bindings/go/binui/` —— 绑定本体（`package binui`）
- `bindings/go/demo/` —— 演示：`ui.html` 用 `//go:embed` 编进二进制

## 注意

- **建窗和消息循环必须锁在一个 OS 线程上，`main` 也要锁**。Win32 窗口和 WGL 上下文
  都有线程亲和，而 Go 的 main goroutine 只在启动时位于主线程，运行期可以被调度到
  别的 M —— 所以"别把 `Pump` 丢进子 goroutine"并不够。用 `binui.RunMain`：

  ```go
  func main() { os.Exit(binui.RunMain(run)) }

  func run() int { /* New / Open / for Pump ... / Close */ return 0 }
  ```

  不锁的症状是**随机崩溃或退出时挂死**，日志里没有任何提示。绑定没有在内部起线程，
  也不会替你锁（锁是 goroutine 级的，得在你要跑循环的那个 goroutine 上做）。
- cgo 每次调用有固定开销（百纳秒级），别在"逐节点每帧"的循环里反复跨 ——
  `Nodes()` 一次性拿全部快照就是这个原因。
- 字符串走 `C.CString` + `C.free`，`GoStringN` 取回时按 `text_len` 而不是等 NUL。
