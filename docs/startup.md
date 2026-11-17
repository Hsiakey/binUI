# 启动耗时：实测分段与量法

"双击后要等一会才出画面"这类问题，**先量，再改**。这份文档记录 v0.18.0 起的测量工具、
本机实测数字、以及一条踩过的坑（用错工具量出来的数字假到能带偏结论）。

## 1. 怎么量

三个环境变量，互相独立：

| 变量 | 作用 |
|---|---|
| `BINUI_STARTUP=1` | 打开分段探针：每个里程碑往 stderr 打一行 `[startup] <阶段> <累计 ms>` |
| `BINUI_STARTUP_FILE=<path>` | 探针改写到这个文件（**推荐**，见 §4 的坑）。设了它就等于同时打开了探针 |
| `BINUI_FRAME_EXIT=<n>` | 画满 n 帧后**正常退出**（静态页面本来只画 1 帧，所以要 n 帧时外壳会强制重绘），自动化量测不必外部强杀 |
| `BINUI_PERF=1` | 已有的帧统计：`绘制 / 呈现 / 间隔` 的 avg·max；配合 `BINUI_FRAME_EXIT` 短跑也会打印 |

`BINUI_BOOT_EXIT=1` 是给示例用的：走到事件循环前就返回，只量"启动路径"（不要首帧）。

示例（PowerShell，量 `examples/flexshow`）：

```powershell
$env:BINUI_STARTUP_FILE='D:\projects\binUI\build-mingw\tmp\startup.log'
$env:BINUI_FRAME_EXIT='1'
$sw=[System.Diagnostics.Stopwatch]::StartNew()
$p=[System.Diagnostics.Process]::Start('D:\projects\binUI\build-mingw\bin\examples\flexshow.exe')
$p.WaitForExit(); $sw.Elapsed.TotalMilliseconds
Get-Content build-mingw\tmp\startup.log
```

## 2. 本机实测（flexshow，1100×720，RTX 4060 Ti / 120Hz / GL 后端）

累计耗时（从 `main()` 起算），三次运行一致：

| 里程碑 | 累计 ms | 本段 ms | 归属 |
|---|---|---|---|
| `compile + load layout` | 3.5 | 3.5 | **binUI**（HTML→布局） |
| `UiApp ctor` | 3.7 | 0.2 | **binUI** |
| `open: DPI+class` | 4.0 | 0.3 | 系统（DPI/RegisterClass） |
| `open: SPI_GETWORKAREA` | 4.6 | 0.6 | 系统 |
| `open: CreateWindowEx` | 205 | **200** | 系统（**一次性**，见 §3） |
| `GL context (WGL 3.3)` | +145 | **145** | 显卡驱动（一次性） |
| `GL shaders` | +15 | 15 | 驱动编译 GLSL |
| `GL state+framebuffer` | +2 | 2 | binUI |
| `font atlas initFonts` | +1 | 1 | binUI（FreeType 懒加载，不是瓶颈） |
| `open done (window visible)` | 392 | 6 | 系统（前台化/焦点） |
| `frame1: draw_ (verts)` | +31 | **31** | **binUI**（布局遍历 + 顶点上传） |
| `frame1: present done` | **662** | **227** | 驱动/DWM（一次性，见 §3） |

**结论：到第一帧 662 ms，其中 binUI 自己的代码只占 ~35 ms（5%）**，剩下 95% 是窗口创建、
GL 上下文、第一次 SwapBuffers 三项一次性系统/驱动开销。连续帧稳态是 `绘制 0.58 ms /
呈现 11.7 ms`（120Hz + 垂直同步），所以"启动慢"和"用起来卡"是两件互不相干的事。

退出也一样：`BINUI_FRAME_EXIT=1` 的进程墙钟 660 ms 画完，**墙钟还要再多 220~265 ms**
才是进程消失 —— 那是 `wglDeleteContext` + 驱动卸载 + CRT 退出，不是画面没画完。

## 3. 那三项一次性开销能不能砍？——基本不能

用 [tools/win_create_time.cpp](../tools/win_create_time.cpp) 做最小对照实验（一个裸
`CreateWindowExW`，不涉及 binUI）：

```
x86_64-w64-mingw32-g++ -O2 -static-libgcc -static-libstdc++ \
    -o build-mingw/tmp/win_create_time.exe tools/win_create_time.cpp -luser32 -lgdi32
./build-mingw/tmp/win_create_time.exe
```

| 实验 | CreateWindowEx 耗时 |
|---|---|
| 进程里**第一个**窗口（裸类，无 CS_OWNDC） | **156 ms** |
| 第二个窗口（带 CS_OWNDC） | 23 ms |
| 第二个窗口（不 WS_VISIBLE） | 2.9 ms |
| 标准 WS_OVERLAPPEDWINDOW | 15 ms |
| **再一个** popup+visible | 19 ms |

即"第一个窗口 150 ms"是**进程一次性**成本（user32/DWM/主题第一次初始化），和 binUI 的
样式、`CS_OWNDC`、`WS_VISIBLE` 都无关。GL 上下文 145 ms、第一次 SwapBuffers 227 ms 同理
（`[perf]` 里能看到 `呈现 max=227.18` 而 `avg=11.67` —— 只有第一帧异常）。

所以这里**没有可优化项**：剩下的唯一"优化"是把一次性成本挪个位置（比如先隐藏建窗、
GL 就绪后再显示），总时长不变，只是把"白窗等待"换成"晚 500 ms 出窗"，体验更差。

真要再快，方向在别处（都不属于当前范围）：把 GL 上下文与首帧放在独立线程预热、
或者干脆不用 GL 给静态页（GDI 后端首帧只要几毫秒）。

## 4. 坑：**别用 PowerShell 的 `-RedirectStandardError` 量启动**

同一份二进制、同一行代码，只改重定向方式：

| 方式 | `UiApp ctor` 里程碑读数 |
|---|---|
| 控制台直连（`& exe`） | **3.5 ms** |
| `Start-Process -RedirectStandardError file` | **1207 ms** |

差别不在程序，在**每次 `fflush(stderr)`**：子进程 stderr 被 PowerShell 重定向到文件后，
这台机器上每次 flush 要 ~1.1 s（写穿透 + 杀软逐次扫描），三段日志就凭空多出 3 秒多。
所以：

* 量启动**必须**用 `BINUI_STARTUP_FILE`（探针自己 `fopen` 追加写，一次 ~0.1 ms）；
* 想用管道/文件重定向收集 stderr 时，**任何**带 `fflush` 的诊断都会失真 —— 这条对
  `[perf]`、`BINUI_GL_VERTS`、`BINUI_BLUR_TRACE` 同样成立。

探针本身也做了两件事保证读数可信：时间戳在写日志**之前**取（日志开销不计入本段）；
`BINUI_STARTUP_FILE` 路径用 `fopen(..., "ab") + fwrite + fclose`，不依赖 CRT 缓冲。
