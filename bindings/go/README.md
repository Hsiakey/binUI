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

- **别把 `Pump` 丢进子 goroutine**。Win32 窗口有线程亲和；主 goroutine 天然固定
  线程，子 goroutine 上要自己 `runtime.LockOSThread()`。绑定刻意没有在内部起线程。
- cgo 每次调用有固定开销（百纳秒级），别在"逐节点每帧"的循环里反复跨 ——
  `Nodes()` 一次性拿全部快照就是这个原因。
- 字符串走 `C.CString` + `C.free`，`GoStringN` 取回时按 `text_len` 而不是等 NUL。
