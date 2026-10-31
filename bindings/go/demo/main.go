// Go 绑定演示：整窗 UI 由 binUI 渲染，业务逻辑全在 Go 里。
//
// 构建前先在仓库根目录产出 C ABI：
//
//	mingw32-make SHELL=sh capi
//
// 然后：
//
//	cd bindings/go
//	go run ./demo
//
// 运行期需要 binui_c.dll 在 exe 同目录或 PATH 上（cgo 只是链接了 import lib）。
package main

import (
	_ "embed"
	"fmt"
	"os"

	"binui/binui"
)

//go:embed ui.html
var uiHTML string

func main() {
	fmt.Println("binui_c", binui.Version(), "abi", binui.ABIVersion())
	if binui.ABIVersion() != binui.ABI {
		fmt.Fprintf(os.Stderr, "警告：ABI 版本不一致（库 %d / 绑定 %d）\n",
			binui.ABIVersion(), binui.ABI)
	}

	layout, err := binui.CompileString(uiHTML)
	if err != nil {
		fmt.Fprintf(os.Stderr, "编译失败：%v\n", err)
		os.Exit(1)
	}
	fmt.Printf("ui.html -> %d 字节布局\n", len(layout))

	app, err := binui.New(layout, "")
	if err != nil {
		fmt.Fprintf(os.Stderr, "加载失败：%v\n", err)
		os.Exit(1)
	}
	defer app.Close()

	if err := app.Open(0, 0, binui.OpenResizable, "binUI · Go 绑定"); err != nil {
		fmt.Fprintf(os.Stderr, "建窗失败：%v\n", err)
		os.Exit(1)
	}

	ids := map[string]int32{}
	for _, name := range []string{"count", "inc", "reset", "toggle", "quit", "name", "echo"} {
		ids[name] = app.Find(name)
		if ids[name] < 0 {
			fmt.Fprintf(os.Stderr, "节点没找到：%s\n", name)
		}
	}

	count := 0
	toggled := false

	app.Run(8, func(ev binui.Event) {
		switch ev.Type {
		case binui.Click:
			switch ev.NodeID {
			case ids["inc"]:
				count++
				app.SetText(ids["count"], fmt.Sprint(count))
				fmt.Println("click: inc ->", count)
			case ids["reset"]:
				count = 0
				app.SetText(ids["count"], "0")
				fmt.Println("click: reset")
			case ids["toggle"]:
				toggled = !toggled
				app.SetChecked(ids["toggle"], toggled)
				label := "开关"
				if toggled {
					label = "已开"
				}
				app.SetText(ids["toggle"], label)
				fmt.Println("click: toggle ->", toggled)
			case ids["quit"]:
				fmt.Println("click: quit")
				app.RequestExit()
			default:
				fmt.Println("click: node", ev.NodeID)
			}
		case binui.Input:
			fmt.Printf("input: %q\n", ev.Text)
			echo := "…"
			if ev.Text != "" {
				echo = "输入框内容：" + ev.Text
			}
			app.SetText(ids["echo"], echo)
		case binui.Close:
			fmt.Println("窗口关闭")
		}
	})

	fmt.Println("退出")
}
