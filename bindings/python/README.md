# binUI × Python（ctypes）

零依赖，只用标准库 `ctypes`，不用编译任何 Python 扩展。

## 跑起来

```bash
# 1. 仓库根目录产出 C ABI
mingw32-make SHELL=sh capi      # -> build-mingw/bin/binui_c.dll

# 2. 跑演示
python bindings/python/demo.py
```

`binui.py` 会按顺序找 `binui_c.dll`：`$BINUI_C_DLL` → 本目录/`bin/` → 上级目录 →
仓库内 `build-mingw/bin`。所以仓库里直接跑不用设 PATH。

## 最小用法

```python
import binui

layout = binui.compile_source(open("ui.html", encoding="utf-8").read())
with binui.App(layout) as app:
    app.open(title="我的窗口")
    count_id = app.find("count")
    n = 0
    while app.pump():                       # 8ms 超时，空闲不烧 CPU
        while (ev := app.poll()) is not None:
            if ev.type == binui.CLICK and ev.node_id == app.find("inc"):
                n += 1
                app.set_text(count_id, str(n))
```

## API 一览

| 分组 | 接口 |
|---|---|
| 编译 | `compile_file(src, out)`、`compile_source(text) -> bytes` |
| 生命周期 | `App(layout, asset_dir=None)`、`open(w=0, h=0, title=..., flags=RESIZABLE)`、`pump(ms=8) -> bool`、`poll() -> Event\|None`、`run(callback)`、`close()` |
| 查询 | `nodes()`、`node(id)`、`find(name)`、`text(id)`、`size()`、`hit_test(x, y)` |
| 修改 | `set_text(id, s)`、`set_visible(id, b)`、`set_checked(id, b)`、`invalidate(id)`、`invalidate_all()` |
| 窗口 | `minimize()`、`maximize()`、`request_exit()`、`begin_window_drag()` |
| 资源 | `set_image_rgba(index, w, h, bytes)`、`set_image(index, png_bytes)`、`mount_components(dir)` |

`Event` 的 `type` 用模块常量比较：`CLICK` / `INPUT` / `HOVER` / `RESIZE` / `KEY` /
`CHAR` / `WHEEL` / `CLOSE`；`INPUT` 与 `CHAR` 的 `text` 已经是 `str`（库内 UTF-8 → Python str）。

`Node` 是 dataclass，带 `type_name` / `visible` / `interactive` 便捷属性。

## 注意

- 控制台可能是 GBK：`print` 中文前先
  `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`（demo 里就是这么干的）。
- 窗口要在同一线程创建和 `pump`；`App` 不要跨线程用。
- `pump()` 会阻塞最多 `timeout_ms`，所以在里面放长耗时逻辑会掉帧 —— 大活儿拆到
  自己的循环里，别塞进事件回调。
