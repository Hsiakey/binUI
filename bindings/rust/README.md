# binUI × Rust（extern "C"）

不依赖 bindgen：`src/lib.rs` 里手写了一遍 `include/binui_c.h` 的声明，外加一层安全
封装。理由是这个 ABI 很小（约 40 个函数、2 个结构体），手写的声明比生成的可读，
也不会因为换台机器就要重跑 bindgen。

## 跑起来

```bash
# 1. 仓库根目录产出 C ABI（DLL + import lib）
mingw32-make SHELL=sh capi

# 2. 构建并运行（用 GNU 工具链，和 MinGW 同一套链接器）
cd bindings/rust
rustup toolchain install stable-x86_64-pc-windows-gnu --profile minimal   # 只需一次
cargo +stable-x86_64-pc-windows-gnu run --example demo
```

`build.rs` 按这个顺序找 `binui_c`：

1. 环境变量 `BINUI_C_LIB_DIR`
2. `bindings/rust/../../build-mingw/bin`（仓库内默认位置）

运行期要有 `binui_c.dll` 在 `PATH` 上或 exe 同目录。

> MSVC 工具链（`stable-x86_64-pc-windows-msvc`）也能 `cargo check`，但链接要装
> VS Build Tools 的 `link.exe`。用 GNU 工具链可以直接复用仓库已有的 MinGW。

## 最小用法

```rust
let layout = binui::compile_str(HTML)?;
let app = binui::App::new(&layout, None)?;
app.open(0, 0, binui::OPEN_RESIZABLE, "Rust 驱动")?;

let id_count = app.find("count");
let id_inc = app.find("inc");
app.run(8, |app, ev| {
    if ev.kind == binui::EventKind::Click && ev.node_id == id_inc {
        let _ = app.set_text(id_count, "被点了");
    }
});
```

## 设计取舍

- **`App` 故意不实现 `Send`/`Sync`**：Win32 窗口必须在创建它的线程上 `pump`。
  不给 `Send` 就是让编译器替你挡住"丢进线程池"的写法。
- **不做回调**：和 C ABI 一致，`pump()` + `poll()` 轮询。所以没有
  `FnMut` 跨 FFI、没有生命周期纠缠。
- **`_assert_layout()`**：把 `sizeof(RawNode) == 56`、`offsetof(color) == 32`、
  `sizeof(RawEvent) == 48`、`offsetof(text) == 24` 断言进去 —— 和
  `tests/test_capi.cpp` 用的是同一组数字，任何一边改了另一边就炸。
- MSRV：`std::mem::offset_of!` 需要 Rust ≥ 1.77。

## 目录

```
bindings/rust/
├── Cargo.toml
├── build.rs          给 rustc 指定 -L 与 -l
├── src/lib.rs        raw extern 声明 + 安全封装
├── ui.html           演示界面（include_str! 编进二进制）
└── examples/demo.rs  演示逻辑
```
