# 在其他语言里使用 binUI

binUI 引擎是 C++ 库（`namespace binui`、`std::function`、异常语义），直接跨 FFI
会踩一串语言各自的坑。所以真正对外的门只有一扇：**`include/binui_c.h` 的 C ABI**。

```
        .html / .xml
             │  binui_compile / binui_compile_file()（构建期或运行期都行）
             ▼
        .bui 二进制布局  ←── 纯数据，任何语言都能读写、能随包分发
             │  binui_app_create() / binui_app_open()
             ▼
   ┌─────────────────────────────────────────┐
   │ binui_c.dll  (Windows / OpenGL 3.3)     │
   │   布局解析 · 命中测试 · 滚动 · 动画      │
   │   文本编辑 · 组件挂载 · 脏矩形重绘       │
   └─────────────────────────────────────────┘
             ▲  binui_pump / binui_poll_event / binui_set_text
             │
   Python (ctypes) · Go (cgo) · Rust (extern "C") · C · 任何能调 C 的语言
```

## 三条设计约束

改 ABI 之前先读这三条 —— 绑定能不能"一个模子"写出来全靠它们：

1. **结构体只用定长类型**：`int32_t` / `uint32_t` / `float`。没有 `bool`（C 是
   1 字节、C# 默认 4 字节）、没有 `enum`（宽度随编译器）、没有 `size_t` / `long`
   （32/64 位会变）。所有结构体天然 4 字节对齐，不需要 `#pragma pack`。
   已用 `tests/test_capi.cpp` 把 `sizeof` / `offsetof` 钉死。
2. **没有跨语言回调**。事件全部走 `binui_poll_event()` 轮询 —— 宿主不进引擎的
   调用栈，就没有线程亲和、GC 保活、重入这些问题；PHP/Ruby 这类没有稳定回调
   机制的语言也能用。
3. **字符串一律 UTF-8**。取文本用 `(buf, cap)` 两段式（返回所需长度，和
   `snprintf` 一个习惯），不返回需要宿主释放的指针。

## 最小流程

```c
void* app = binui_app_create(layout_bytes, layout_len, "ui_dir/");  // 加载 .bui
binui_app_open(app, 0, 0, BINUI_OPEN_RESIZABLE, "标题");            // 建窗（0=用布局自带的尺寸）
while (binui_pump(app, 8) == 0) {          // 8ms 超时：空闲不烧 CPU
    BinUIEvent ev;
    while (binui_poll_event(app, &ev)) {
        if (ev.type == BINUI_EV_CLICK) { /* ev.node_id 是谁被点了 */ }
    }
}
binui_app_destroy(app);
```

`binui_pump()` 内部做四件事：收 Win32 消息 → 推进动画/视频时钟 → 重绘脏矩形 →
返回。宿主不需要知道 GL、不需要知道消息循环，也不需要自己算脏区。

## 各语言

| 语言 | 目录 | 机制 | 状态 |
|---|---|---|---|
| C / C++ | `include/binui_c.h` | 直接 include + `libbinui_c.a` | ✅ `tests/test_capi.cpp` 覆盖 |
| **Python** | [`bindings/python/`](../bindings/python/README.md) | `ctypes`（标准库，零依赖） | ✅ 跑通（本机 3.12） |
| **Go** | [`bindings/go/`](../bindings/go/README.md) | `cgo` + import lib | ✅ 跑通 |
| **Rust** | [`bindings/rust/`](../bindings/rust/README.md) | `extern "C"` + 手写声明（不依赖 bindgen） | ✅ 见下 |

三个绑定的 API 形状是一样的：`New/compile → open → loop { pump; poll }`，
返回值/错误处理各自本地化。要看最完整的例子直接读
[`bindings/python/binui.py`](../bindings/python/binui.py)。

## 构建 C ABI 产物

```sh
mingw32-make SHELL=sh capi
# build-mingw/libbinui_c.a         静态库：C / C++ / Rust 直接链
# build-mingw/bin/binui_c.dll      动态库：Python ctypes / Go cgo
# build-mingw/bin/libbinui_c.dll.a DLL 的 import lib（Go/Rust 链接用）
```

CMake 侧对应 `binui_c`（静态）与 `binui_c_shared`（动态）两个目标，仅 Windows 构建。

**产物是自包含的**：`Makefile` 里 `RTLINK := -static-libgcc -static-libstdc++`，
FreeType / HarfBuzz 也都走静态库（`third_party/*/build-static`），所以除系统 DLL
（`gdiplus` / `opengl32` / `dwmapi` / `user32` / `gdi32`）外不用附带任何第三方运行时。
Python 用户不会撞上经典的 `OSError: [WinError 126] 找不到指定模块`。

⚠️ 链接时别误用 `third_party/harfbuzz/build/src/libharfbuzz.dll.a` —— 那套会拖进
`libharfbuzz-0.dll`。用 `build-static` 那份。

⚠️ DLL 被宿主进程加载期间**无法重新链接**（Windows 文件锁）。改完引擎先关掉宿主
程序再 `make capi`，否则报 `cannot open output file binui_c.dll: Permission denied`。

## 线程与窗口

- 窗口必须在**创建它的那个线程**上 `pump`。Win32 规则，不是线程安全问题。
  Python：主循环天然满足；Go：别把 `Pump` 丢进子 goroutine（要丢就
  `runtime.LockOSThread()`）；Rust：绑定刻意不给 `App` 实现 `Send`。
- 目前只出**独立顶层窗口**，不支持把 binUI 嵌成子窗口（`attach(HWND)` 还没做）。

## 键盘与中文输入

文本框的编辑**由库内部处理**：退格 / 删除 / 方向键 / Home / End / Tab 循环焦点 /
Enter 激活，宿主不用重写一遍，结果通过 `BINUI_EV_INPUT` 推出（编辑过程中逐字推，
失焦时提交）。

> 2026-10 修的一个真 bug：三个窗口外壳原来用 ANSI 窗口类（`RegisterClassA` +
> `DispatchMessageA`），在 CP936 系统上 Windows 会把一个中文字符拆成**两个**
> `WM_CHAR`（DBCS 字节），于是输入框里打「中」得到「ÖÐ」。改成 Unicode 窗口
> （`RegisterClassW` + `DispatchMessageW`）后，中文与代理对（emoji）都正常。
> 这条也算"为什么绑定值得做"的注脚 —— 光靠 C++ demo 发现不了，得真在别的语言里
> 打一遍字。

## 没有跨过去的部分（明确不支持，不是 bug）

| 能力 | 原因 / 替代方案 |
|---|---|
| `<canvas>` 绘制回调 | 需要跨语言回调，与约束 2 冲突；宿主自己在窗口上画，或改用 `set_image_rgba` 推像素 |
| `<video>` 播放 | 解码模块 `BINUI_ENABLE_VIDEO=ON` 才编进去，另行挂载；帧可以走 `binui_set_image_raw` |
| 窗口嵌入子窗口 | 待做（底层 `BinUIGLGdi::init(hwnd)` 已能接外部句柄，缺外壳那层） |
| 非 Windows 平台 | 核心库跨平台，**外壳不是**：GL-GDI 是 Win32 实现。要跨平台先补平台层 |

## 新增一门语言要做的

1. 照着 `include/binui_c.h` 写一遍声明（结构体字段顺序别动）。
2. 字符串用 `(ptr, len)` 而不是只看 NUL。
3. `binui_last_error()` 转成本地错误类型。
4. 写一个 demo：编译 UI → 建窗 → 循环 pump/poll → 点一个按钮改一行字。
5. 如果这门语言有 struct 布局检查能力，把 `sizeof(BinUINode) == 56`、
   `sizeof(BinUIEvent) == 48` 断言进去（Rust 的 `_assert_layout()` 就是干这个的）。
