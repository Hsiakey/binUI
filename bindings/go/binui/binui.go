// Package binui 是 binUI 的 Go 绑定（cgo）。
//
// 引擎是 C++ 库，跨语言那层是 include/binui_c.h 的 C ABI；这里只做
// "句柄 + 轮询"的翻译，不引入任何回调，所以 Goroutine 调度、GC 保活
// 都不会碰到引擎。
//
// 用法:
//
//	layout, err := binui.CompileString(html)
//	app, err := binui.New(layout, "ui/")
//	defer app.Close()
//	if err := app.Open(0, 0, binui.OpenResizable, "Go 驱动"); err != nil { ... }
//	for app.Pump(8) {
//		for ev, ok := app.Poll(); ok; ev, ok = app.Poll() {
//			if ev.Type == binui.Click && app.Text(ev.NodeID) == "离开" {
//				app.RequestExit()
//			}
//		}
//	}
//
// 注意：窗口必须在同一个 OS 线程上创建和 pump。主 goroutine 天然固定线程，
// 所以别把 Pump 丢进子 goroutine —— 那不是线程安全问题，是 Win32 的规则
// （子 goroutine 上要自己 runtime.LockOSThread()）。
//
// 构建（仓库根目录）:
//
//	mingw32-make SHELL=sh capi
//	cd bindings/go && go build ./...
package binui

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include
#cgo LDFLAGS: -L${SRCDIR}/../../../build-mingw/bin -lbinui_c
#include <stdlib.h>
#include "binui_c.h"
*/
import "C"

import (
	"errors"
	"unsafe"
)

// ABI 版本：与 C 头里的 BINUI_C_ABI_VERSION 一致
const ABI = 1

// 事件类型
const (
	Click  = int32(C.BINUI_EV_CLICK)
	Input  = int32(C.BINUI_EV_INPUT)
	Hover  = int32(C.BINUI_EV_HOVER)
	Resize = int32(C.BINUI_EV_RESIZE)
	Key    = int32(C.BINUI_EV_KEY)
	Char   = int32(C.BINUI_EV_CHAR)
	Wheel  = int32(C.BINUI_EV_WHEEL)
	Close  = int32(C.BINUI_EV_CLOSE)
)

// 节点类型
const (
	NodeRoot   = int32(C.BINUI_NODE_ROOT)
	NodeLabel  = int32(C.BINUI_NODE_LABEL)
	NodeImage  = int32(C.BINUI_NODE_IMAGE)
	NodeButton = int32(C.BINUI_NODE_BUTTON)
	NodeInput  = int32(C.BINUI_NODE_INPUT)
	NodeList   = int32(C.BINUI_NODE_LIST)
	NodeLine   = int32(C.BINUI_NODE_LINE)
	NodeSVG    = int32(C.BINUI_NODE_SVG)
	NodeCanvas = int32(C.BINUI_NODE_CANVAS)
	NodeVideo  = int32(C.BINUI_NODE_VIDEO)
)

// 节点标志位
const (
	FlagVisible     = uint32(C.BINUI_NF_VISIBLE)
	FlagDisabled    = uint32(C.BINUI_NF_DISABLED)
	FlagInteractive = uint32(C.BINUI_NF_INTERACTIVE)
	FlagScrollable  = uint32(C.BINUI_NF_SCROLLABLE)
	FlagAbsolute    = uint32(C.BINUI_NF_ABSOLUTE)
	FlagTitlebar    = uint32(C.BINUI_NF_TITLEBAR)
)

// Open 的选项
const (
	OpenResizable   = uint32(C.BINUI_OPEN_RESIZABLE)
	OpenTransparent = uint32(C.BINUI_OPEN_TRANSPARENT)
	OpenNoVSync     = uint32(C.BINUI_OPEN_NO_VSYNC)
)

// 虚拟键码（BINUI_EV_KEY 的 Key 字段）
const (
	KeyBack   = int32(C.BINUI_KEY_BACK)
	KeyTab    = int32(C.BINUI_KEY_TAB)
	KeyReturn = int32(C.BINUI_KEY_RETURN)
	KeyEscape = int32(C.BINUI_KEY_ESCAPE)
	KeyLeft   = int32(C.BINUI_KEY_LEFT)
	KeyUp     = int32(C.BINUI_KEY_UP)
	KeyRight  = int32(C.BINUI_KEY_RIGHT)
	KeyDown   = int32(C.BINUI_KEY_DOWN)
	KeyDelete = int32(C.BINUI_KEY_DELETE)
)

// Version 返回库版本串。
func Version() string { return C.GoString(C.binui_version()) }

// ABIVersion 返回库的 ABI 版本（应与 ABI 相等）。
func ABIVersion() int32 { return int32(C.binui_abi_version()) }

// LastError 返回最近一次失败的说明。
func LastError() string { return C.GoString(C.binui_last_error()) }

// —— 编译 ——

// CompileFile 把 UI 源码（.html / .xml）编成二进制布局文件。
func CompileFile(src, out string) error {
	cs, co := C.CString(src), C.CString(out)
	defer C.free(unsafe.Pointer(cs))
	defer C.free(unsafe.Pointer(co))
	if C.binui_compile_file(cs, co) != 1 {
		return errors.New(LastError())
	}
	return nil
}

// CompileString 把 UI 源码字符串编成布局字节。
func CompileString(src string) ([]byte, error) {
	cs := C.CString(src)
	defer C.free(unsafe.Pointer(cs))
	var data unsafe.Pointer
	var size C.int32_t
	if C.binui_compile_memory(cs, C.int32_t(len(src)), &data, &size) != 1 {
		return nil, errors.New(LastError())
	}
	defer C.binui_free(data)
	return C.GoBytes(data, size), nil
}

// —— 数据 ——

// Event 是一次输入/状态变化。
type Event struct {
	Type     int32
	NodeID   int32
	X, Y, Dy float32
	Key      int32
	Text     string // INPUT / CHAR 的 UTF-8 文本
	Width    int32
	Height   int32
}

// Node 是节点的定长快照。
type Node struct {
	ID         int32
	ParentID   int32
	Type       int32
	Flags      uint32
	X, Y       float32
	Width      float32
	Height     float32
	Color      uint32 // 0xAARRGGBB
	ImageIndex int32
	FontSize   int32
	Padding    int32
	Radius     int32
	ZIndex     int32
}

// Visible 报告节点是否可见。
func (n Node) Visible() bool { return n.Flags&FlagVisible != 0 }

// Interactive 报告节点是否可交互。
func (n Node) Interactive() bool { return n.Flags&FlagInteractive != 0 }

// —— 应用 ——

// App 是一个 binUI 窗口。
type App struct {
	h unsafe.Pointer
}

// New 加载布局字节，返回应用句柄（还没建窗）。assetDir 是图片/外挂组件的基准目录，可空。
func New(layout []byte, assetDir string) (*App, error) {
	if len(layout) == 0 {
		return nil, errors.New("布局为空")
	}
	var dir *C.char
	if assetDir != "" {
		dir = C.CString(assetDir)
		defer C.free(unsafe.Pointer(dir))
	}
	h := C.binui_app_create(unsafe.Pointer(&layout[0]), C.int32_t(len(layout)), dir)
	if h == nil {
		return nil, errors.New(LastError())
	}
	return &App{h: h}, nil
}

// Open 建窗口（width / height <= 0 时用布局自带的窗口描述）。
func (a *App) Open(width, height int, flags uint32, title string) error {
	ct := C.CString(title)
	defer C.free(unsafe.Pointer(ct))
	if C.binui_app_open(a.h, C.int32_t(width), C.int32_t(height),
		C.uint32_t(flags), ct) != 1 {
		return errors.New(LastError())
	}
	return nil
}

// Pump 推进一帧；返回 false 表示窗口已关闭。
func (a *App) Pump(timeoutMs int) bool {
	return C.binui_pump(a.h, C.int32_t(timeoutMs)) == 0
}

// OpenChild 以子窗口形式把 binUI 挂进宿主窗口 parent 的客户区（CWnd / QWidget 的 HWND）。
//
// 与 Open 的差别：不碰进程 DPI 策略、不抢焦点、**不自己收消息** —— 宿主
// DispatchMessage 会把鼠标/键盘投递给本窗口，事件照旧从 Poll 出来。
// 因此子窗口模式每帧调 Render 而不是 Pump。宿主窗口记得带 WS_CLIPCHILDREN。
func (a *App) OpenChild(parent uintptr, x, y, width, height int) error {
	if C.binui_app_open_child(a.h, unsafe.Pointer(parent), C.int32_t(x), C.int32_t(y),
		C.int32_t(width), C.int32_t(height), 0) != 1 {
		return errors.New(LastError())
	}
	return nil
}

// Render 只出一帧（推进动画 + 绘制脏区），不碰宿主的消息队列。
// 返回 false 表示窗口已关闭（宿主销毁了父窗口）。
func (a *App) Render() bool {
	return C.binui_render(a.h) == 0
}

// Resize 宿主改了本区域尺寸时调用（等价于收到 WM_SIZE）。
func (a *App) Resize(width, height int) {
	C.binui_resize(a.h, C.int32_t(width), C.int32_t(height))
}

// HWND 返回本窗口句柄（子窗口模式下宿主布局要用它；未开窗返回 0）。
func (a *App) HWND() uintptr {
	return uintptr(C.binui_app_hwnd(a.h))
}

// Poll 取一个事件；第二个返回值为 false 表示队列空。
func (a *App) Poll() (Event, bool) {
	var ev C.BinUIEvent
	if C.binui_poll_event(a.h, &ev) != 1 {
		return Event{}, false
	}
	out := Event{
		Type:   int32(ev._type),
		NodeID: int32(ev.node_id),
		X:      float32(ev.x),
		Y:      float32(ev.y),
		Dy:     float32(ev.dy),
		Key:    int32(ev.key),
		Width:  int32(ev.width),
		Height: int32(ev.height),
	}
	if ev.text != nil && ev.text_len > 0 {
		out.Text = C.GoStringN(ev.text, ev.text_len)
	}
	return out, true
}

// Run 跑标准主循环：pump 一帧，排空事件，交给回调（回调为 nil 时只 pump）。
func (a *App) Run(timeoutMs int, fn func(Event)) {
	for a.Pump(timeoutMs) {
		for {
			ev, ok := a.Poll()
			if !ok {
				break
			}
			if fn != nil {
				fn(ev)
			}
		}
	}
}

// Close 销毁窗口与应用。
func (a *App) Close() {
	if a.h != nil {
		C.binui_app_destroy(a.h)
		a.h = nil
	}
}

// Nodes 返回全部节点快照（文档顺序）。
func (a *App) Nodes() []Node {
	n := int(C.binui_node_count(a.h))
	out := make([]Node, 0, n)
	for i := 0; i < n; i++ {
		var c C.BinUINode
		if C.binui_node_at(a.h, C.int32_t(i), &c) == 1 {
			out = append(out, nodeOf(c))
		}
	}
	return out
}

// NodeByID 取单个节点快照。
func (a *App) NodeByID(id int32) (Node, bool) {
	var c C.BinUINode
	if C.binui_node_by_id(a.h, C.int32_t(id), &c) != 1 {
		return Node{}, false
	}
	return nodeOf(c), true
}

// Find 按 HTML id / XML name 查节点 id；找不到返回 -1。
func (a *App) Find(name string) int32 {
	cn := C.CString(name)
	defer C.free(unsafe.Pointer(cn))
	return int32(C.binui_find_node(a.h, cn))
}

// Text 取节点文本（节点不存在返回空串）。
func (a *App) Text(id int32) string {
	need := int(C.binui_node_text(a.h, C.int32_t(id), nil, 0))
	if need <= 0 {
		return ""
	}
	buf := make([]byte, need+1)
	C.binui_node_text(a.h, C.int32_t(id), (*C.char)(unsafe.Pointer(&buf[0])),
		C.int32_t(len(buf)))
	return string(buf[:need])
}

// Size 返回当前窗口尺寸。
func (a *App) Size() (w, h int) {
	var cw, ch C.int32_t
	C.binui_app_size(a.h, &cw, &ch)
	return int(cw), int(ch)
}

// HitTest 返回坐标处的节点 id，未命中 -1。
func (a *App) HitTest(x, y float32) int32 {
	return int32(C.binui_hit_test(a.h, C.float(x), C.float(y)))
}

// SetText 改文本（会自动重绘该节点）。
func (a *App) SetText(id int32, text string) error {
	ct := C.CString(text)
	defer C.free(unsafe.Pointer(ct))
	if C.binui_set_text(a.h, C.int32_t(id), ct, C.int32_t(len(text))) != 1 {
		return errors.New("set_text 失败：节点不存在")
	}
	return nil
}

// SetVisible 显隐节点。
func (a *App) SetVisible(id int32, visible bool) {
	v := C.int32_t(0)
	if visible {
		v = 1
	}
	C.binui_set_visible(a.h, C.int32_t(id), v)
}

// SetChecked 设置 :checked 状态。
func (a *App) SetChecked(id int32, checked bool) {
	v := C.int32_t(0)
	if checked {
		v = 1
	}
	C.binui_set_checked(a.h, C.int32_t(id), v)
}

// Invalidate 只重绘一个节点。
func (a *App) Invalidate(id int32) { C.binui_invalidate_node(a.h, C.int32_t(id)) }

// InvalidateAll 全窗重绘。
func (a *App) InvalidateAll() { C.binui_invalidate_all(a.h) }

// Minimize / Maximize / RequestExit / BeginWindowDrag 是窗口动作。
func (a *App) Minimize()        { C.binui_minimize(a.h) }
func (a *App) Maximize()        { C.binui_maximize(a.h) }
func (a *App) RequestExit()     { C.binui_request_exit(a.h) }
func (a *App) BeginWindowDrag() { C.binui_begin_window_drag(a.h) }

func nodeOf(c C.BinUINode) Node {
	return Node{
		ID:         int32(c.id),
		ParentID:   int32(c.parent_id),
		Type:       int32(c._type),
		Flags:      uint32(c.flags),
		X:          float32(c.x),
		Y:          float32(c.y),
		Width:      float32(c.width),
		Height:     float32(c.height),
		Color:      uint32(c.color),
		ImageIndex: int32(c.image_index),
		FontSize:   int32(c.font_size),
		Padding:    int32(c.padding),
		Radius:     int32(c.radius),
		ZIndex:     int32(c.z_index),
	}
}
