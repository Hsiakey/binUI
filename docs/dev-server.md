# binUI dev 服务：结构查看器 + 真实渲染 + 保存即刷新

`tools/binui_dev.py`。把"看布局"从三条命令变成"存一下就变"：

```sh
python tools/binui_dev.py examples/buttonshow/main.html
```

stdout **第一行**就是最终 URL（端口被占用会自动 +1，脚本可以直接抓这一行）：

```
http://127.0.0.1:8765/
```

浏览器打开就是两栏：**左边结构查看器**（`draw_bench --dump-nodes` + `tools/binui_nodes.py`），
**右边 `bin2d_shot` 的真实渲染 PNG**。改页面/组件/样式，保存 → 自动重建 → 页面自己刷新。

## 用法

```sh
python tools/binui_dev.py <page.html> [--port 8765] [--shot-only] [--no-initial-build]
```

| 参数 | 含义 |
|---|---|
| `<page.html>` | 页面路径（相对当前目录或绝对路径） |
| `--port N` | 起始端口，默认 8765；被占用就 +1 重试（最多 20 次） |
| `--shot-only` | 只出 `bin2d_shot` 的 PNG，不做结构查看器（页面上就是一张大图） |
| `--no-initial-build` | 启动时先不重建，等第一次请求 |

产物统一落 `out/dev/`（已在 `.gitignore` 里，服务**不写源码树**）：

```
out/dev/<name>.png         bin2d_shot 的真实渲染
out/dev/<name>.json        draw_bench --dump-nodes 的节点表
out/dev/<name>.view.html   binui_nodes 合成的结构查看器（服务以 /view 提供）
```

## 设置页背后的三条命令

服务本身不解析布局，只是把现成二进制串起来（和手敲一样）：

```sh
build-mingw\bin\bin2d_shot.exe <page> out\dev\<name>.png
build-mingw\bin\draw_bench.exe <page> --frames 2 --dump-nodes out\dev\<name>.json
python tools\binui_nodes.py <json> <png> -o out\dev\<name>.view.html
```

首屏一次完整重建约 **250~600 ms**（buttonshow、1020×684：shot ≈200ms + 节点 ≈10ms + 合成 ≈35ms），
之后只有文件变了才重建。

## 监听范围（mtime 每秒轮询，不引 watchdog）

1. 页面文件本身；
2. 它引用的组件：`<use src="...">` / `data-component="..."`（组件里再引组件会继续跟，最多 3 层）；
3. 页面里被引用的 `.css` / `.js`，以及**页面同目录**下的样式/脚本文件。

改了构件清单（比如新加一个 `<use>`）也不用重启，监听集合每次轮询会重算。

## 热刷新怎么工作（SSE）

- `/events` 是一条 SSE 流；服务端发现文件变化 → 重建 → 推一条 `data: reload`；
- 页面里的 `EventSource` 收到就重载 iframe 和 PNG（带 `?t=时间戳` 破缓存），顶栏显示第几次刷新；
- 断线自动重连（服务端发 `retry: 1000`，浏览器原生重连），断线时顶栏圆点变红并显示"自动重连中"；
- 心跳 `: ping` 每 15 s 一次，避免长连接被中间设备掐断。

其他路由：`/view`（结构查看器 HTML）、`/shot.png`（PNG）、`/api/rebuild`（强制重建，页面右上角
「重新生成」按钮用它）、`/api/state`（JSON：状态/重建次数/在线客户端数，便于脚本断言）。

## 自己验证一次

```powershell
# 1) 起服务，抓第一行 URL
$p = Start-Process python -ArgumentList "tools\binui_dev.py","examples/buttonshow/main.html" -NoNewWindow -PassThru -RedirectStandardOutput out\dev\s.log
Start-Sleep 6; Get-Content out\dev\s.log -TotalCount 1

# 2) reload 是否真的推到客户端：挂一个 SSE 客户端，再 touch 源文件
python -c "import urllib.request;[print(l) for l in urllib.request.urlopen('http://127.0.0.1:8765/events',timeout=20) if b'reload' in l][:1]"
(Get-Item components\titlebar.bum.html).LastWriteTime = Get-Date   # 改组件也算
# 服务端日志会出现： [watch] 变化：components\titlebar.bum.html → [sse] 推送 'reload' → 1 个客户端
```

浏览器侧就更容易：把窗口摆着，改一下 `examples/buttonshow/main.html` 里任意一个数值保存，
右栏图、左栏树都会自己换，顶栏时间戳 +1。

## 已知边界

- **不要**把它当真实浏览器预览：左栏是结构树 + `bin2d_shot` 的位图，不是网页本身
  （语法接近 HTML 但语义不同，见 `tools/binui_preview.py` 的说明）。
- 编辑器"保存即触发"可能连发两次（临时文件 + 重命名），服务会重建两次；无副作用，只是多花 ~250 ms。
- 重建是串行的：正在重建时又改文件，这一轮会以最新内容为准（下一轮再触发一次）。
- 二进制被占用（`Permission denied`，通常是在 `make`）时自动重试一次并打日志。
- 只监听 `127.0.0.1`，不对外网暴露。
