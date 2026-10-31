# binUI 语言绑定

引擎是 C++ 的；这里的三份绑定都只对着一层东西编程：`include/binui_c.h` 的 C ABI。

| 语言 | 目录 | 机制 | 状态 |
|---|---|---|---|
| **Python** | [`python/`](python/README.md) | `ctypes`（标准库，零依赖） | ✅ 已跑通 |
| **Go** | [`go/`](go/README.md) | `cgo` + import lib | ✅ 已跑通 |
| **Rust** | [`rust/`](rust/README.md) | 手写 `extern "C"`（不用 bindgen） | ✅ 已跑通 |
| C / C++ | `include/binui_c.h` | 直接 include + `libbinui_c.a` | ✅ `tests/test_capi.cpp` |

![Python 绑定演示](https://raw.githubusercontent.com/Hsiakey/binUI/main/docs/img/python-binding.png)

## 共同前提

```bash
mingw32-make SHELL=sh capi
# build-mingw/libbinui_c.a          静态库
# build-mingw/bin/binui_c.dll       动态库
# build-mingw/bin/libbinui_c.dll.a  动态库的 import lib
```

## 三个语言，同一套形状

```python
# Python
app = binui.App(layout); app.open(title="…")
while app.pump():
    while (ev := app.poll()) is not None: …
```

```go
// Go
app, _ := binui.New(layout, ""); app.Open(0, 0, binui.OpenResizable, "…")
app.Run(8, func(ev binui.Event) { … })
```

```rust
// Rust
let app = binui::App::new(&layout, None)?;
app.open(0, 0, binui::OPEN_RESIZABLE, "…")?;
app.run(8, |app, ev| …);
```

三者都遵守 `构建布局 → 建窗 → 循环 { pump; poll }` 这一条主线，因为 C ABI 就是这么
设计的（没有跨语言回调，见 [`docs/bindings.md`](../docs/bindings.md) 的三条约束）。

## 要不要再加一门语言

先看 [`docs/bindings.md`](../docs/bindings.md) 末尾的"新增一门语言要做的"。判断标准
不是"有没有人用"，而是**这类语言在真实项目里是否真的会碰桌面 UI**：

- 值得：C# / .NET（企业桌面）、Delphi（Windows 原生工具链）、Node + koffi（Electron 主进程）
- 能跑但不必出官方示例：LuaJIT（`ffi.cdef` 贴头文件）、Zig（`@cImport`）、
  Nim（`importc`）、PHP（FFI）、Ruby（fiddle）、Perl（FFI::Platypus）
- 不建议：Swift（Windows 生态不成熟）、Dart/Flutter（自带自绘 UI，和 binUI 是竞争关系）
