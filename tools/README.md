# tools —— 调试与验收工具

都是独立小程序，不参与库构建。产物在 `build-mingw/bin/`（和库、DLL 放一起；测试在
`bin/tests/`，示例在 `bin/examples/`，三者不混）。统一在仓库根目录执行（工具里的相对路径按仓库根解析）。

## binui_compile —— UI / 组件源码编译成二进制

```bash
make tools && build-mingw/bin/binui_compile.exe components/titlebar.bum.html
#   -> components/titlebar.bum
build-mingw/bin/binui_compile.exe components/preview.html -o build/main.bui
build-mingw/bin/binui_compile.exe dist/titlebar.bum -o dist2/titlebar.bum   # 已是二进制 -> 原样拷贝
```

约定：`.bui` = 单个界面（应用入口），`.bum` = 组件；两者都可以只发源码
（`.bum.html`、旧写法 `.bum.xml`）、只发二进制，或两种都发（加载时二进制优先）。
`make components` / CMake 的 `components_bums` 目标会在构建期生成示例组件二进制。
详见 [docs/components.md](../docs/components.md)。

加 `--res` 才在产物**所在目录**写一份 **打包清单** `binui.res`（清单 + 原路径：主 UI + 每个
组件的原始字节与相对路径）。**默认不产出** —— 清单里带着源码，不该是默认行为。
`binui_export` 靠它解包；它可有可无，删掉不影响运行，只是这个包不能再解包。
多个 `.bui` 编到同一目录时条目会合并。

## binui_export —— 把 .bui 还原成打包前的目录结构

```bash
make tools && build-mingw/bin/binui_export.exe build/main.bui -o restore/
#   restore/ 里就是打包时用到的那些文件，逐字节一致
build-mingw/bin/binui_export.exe build/main.bui --list    # 只看清单，不写文件
```

读的是同目录那份 `binui.res`。存的是**原始文件字节 + 相对 appRootDir 的路径**（不是偏移量，
倒出来就是原文件），
所以是逐字节还原，不是"节点树反推 HTML"。没带 `binui.res` 的包不可解包（这正是"不想泄露源码就别带"的开关）。解出来的树能再编一次，产出同一个字节的 `.bui`。

## check_examples —— 示例 UI 零警告检查

把 `examples/` 下所有 `*.html`、`*.bum.html`、`*.bui.xml`、`*.binui.xml`
（`mockup*.html` 设计稿除外）逐个编译，**编译为空或产生任何编译警告都算失败**；
此外还校验 `<use ... external>` 引用的外挂组件是否真的能解析到（二进制或源码）。
CMake 里已注册成 ctest 用例（CI 会跑）。

```bash
make tools && build-mingw/bin/check_examples.exe examples
# 输出：OK/WARN/FAIL + 末尾 "N 个示例 UI 检查完毕，M 处问题"
```

## offscreen_render —— 离屏渲染真实布局到 PNG

把 UI 源文件真正编译 + 布局 + 用 GDI 后端画出来存成 PNG，**不需要开窗口**，适合
CI 截图回归和"改完样式看一眼"。

```bash
# 构建（MinGW；把 PATH 里的工具链放在最前）
g++ -std=c++17 -O2 -Iinclude -Isrc \
    tools/offscreen_render.cpp src/platform/app_win32.cpp \
    build-mingw/libbinui.a -lgdiplus -lgdi32 -luser32 -lole32 \
    -Lthird_party/freetype/build -lfreetype \
    -Lthird_party/harfbuzz/build-static/src -lharfbuzz \
    -o build-mingw/bin/offscreen_render.exe

# 用法
# 内置默认布局（GenerateDefaultBinUI，无 UI 文件时的兜底界面）
build-mingw/bin/offscreen_render --default docs/img/default-layout.png

# 普通用法
build-mingw/bin/offscreen_render examples/features/main.html /tmp/a.png
build-mingw/bin/offscreen_render build-mingw/bin/examples/features/main.html /tmp/b.png   # 验证 bin/ 下的资源布局
build-mingw/bin/offscreen_render tests/fixtures/vision.binui.xml /tmp/c.png --setText log1="hello"
```

注意：离屏渲染只呈现**静态状态**——`::before/::after`、结构伪类、基础样式都能看到，
但 `:hover`/`:focus`/`:checked` 这类运行时状态需要真实交互才生效（渲染器当前状态决定）。

## window_capture —— 抓取指定窗口内容为 PNG

```bash
g++ -std=c++17 -O2 tools/window_capture.cpp -lgdiplus -lgdi32 -luser32 -o build-mingw/bin/window_capture.exe
build-mingw/bin/window_capture "功能演示" /tmp/shot-features.png    # 参数是窗口标题子串
```

用 `PrintWindow` 抓，**对 OpenGL/D3D 窗口通常返回空白**（GPU 内容不在 GDI 表面里），
抓 GDI 后端窗口（如 `offscreen_render` 的结果）才可靠。

## enum_windows —— 列出可见窗口标题

```bash
g++ -std=c++17 -O2 tools/enum_windows.cpp -luser32 -o build-mingw/bin/enum_windows.exe
build-mingw/bin/enum_windows.exe
```

找窗口标题用（配合 `window_capture` 的标题子串参数）。

## verify_shots.py —— 截图颜色校验

对 PNG 采样若干坐标并比对期望色，用于检查"某处到底是不是这个颜色"。

```bash
python tools/verify_shots.py docs/img/features.png
```

需要 Pillow（`pip install pillow`）。

## partial_shot —— 局部重绘一致性（hover 脏矩形 vs 整帧）

窗口只重画变化的节点时，同一帧画面由**两条路径**产生：整帧重绘、以及"只清 + 只画一个脏矩形"。
两条路必须逐像素一致 —— 不一致就是 hover 之后沿脏矩形冒出来的那圈"影子 / 边框"
（见 [docs/issues/10](../docs/issues/10-partial-redraw-cull.md)）。

```bash
make tools
# 出图：先整帧画一帧，再 hover (278,222)，只重画它的脏矩形，把结果存下来
build-mingw/bin/partial_shot.exe examples/buttonshow/main.html build-mingw/tmp/before.png 278 222 40
# 对拍（非 0 退出 = 两条路不一致）
build-mingw/bin/partial_shot.exe --check examples/buttonshow/main.html - 278 222 40
# 一键跑 4 个 hover 点
make partial
```

参数：`<ui> <out.png|-> <x> <y> [frames] [W] [H]`。`frames` 是 hover 后推进动画的步数
（每步 8ms，40 步覆盖 0.16s 过渡）。

阈值：`diff<=64 且 max<=1`。残差是 MSAA 解析在脏矩形边界上的 1 单位噪声；
真出问题（判据写错、脏矩形漏算阴影）时差的是整块区域的底色/阴影，`diff` 上千、`max` 十几，
一眼就能分开（v0.16.32 修的就是这一类，修前同一用例 `diff=5502 max=16`）。

为什么它对拍的不是"基准图"：比较对象是**同一状态下的整帧渲染**，所以不需要维护金标准图，
样式改了也不会失效。

## draw_bench —— 绘制遍历的 CPU 基准（+ 原语流哈希）

回答一个问题：**一帧的 CPU 绘制到底花在哪**。没有 GPU、没有窗口、没有字体光栅化 ——
遍历只认 `IBinUIGfx`，工具里就地实现一个只计数的后端，所以同一个数字在
GL / VK / D3D11 三端之间可比（后端差异另用 `gl_shot --bench` 量）。

```bash
make tools
# 单个页面：遍历 µs/帧、原语分类、≈四边形数、≈KB/帧
build-mingw/bin/draw_bench.exe examples/features/main.html --frames 120 --warm 10
# 只看脏矩形（模拟 hover/动画时的局部重绘）
build-mingw/bin/draw_bench.exe examples/buttonshow/main.html --clip 278,220,160,80
# 原语流的 64 位哈希：缓存 / 重构前后必须一模一样
build-mingw/bin/draw_bench.exe examples/features/main.html --hash
# 一键跑全部示例 + 条带爆炸用例
make bench-draw
```

为什么要有 `--hash`：像素基准只能证明"看起来一样"，原语流哈希能证明"发出去的每个
四边形都一样"（顺序、坐标、颜色、UV、裁剪、批次边界）。做绘制缓存时，这是唯一能证明
"没有因为复用而画错一帧"的判据。

`--json` 输出一行 JSON（CI 里好抓）。`--hover x,y` / `--scrollTo y` / `--text id=...`
用来把页面推进到某个状态再量。

后端的开销用另一个工具量（同一台机器、同一个上下文）：

```bash
# 真实 GL 后端一帧：绘制(遍历+发射) 与 上传+绘制调用 分开报
build-mingw/bin/gl_shot.exe examples/appshow/main.html - --bench 60
make bench-gl
```

注意它**不含** MSAA resolve / 交换链：窗口一进交换链，present 会被合成器节流到
8.2 ms/帧（半个刷新周期），量到的就不是 CPU 成本了。所以 bench 模式走
`flushFrame()`（只 `flushAll`）。

两者相减 = 后端顶点发射/上传的开销。v0.16.33 的实测结论（见
[docs/future.md](../docs/future.md) 的 P0-1/P0-4）：中等页面遍历 15~47 µs、
GL「遍历+发射」0.07~0.10 ms、上传 0.01 ms。`bench-grad`（12 张 135° 渐变卡）在
v0.16.34 之前是 43,409 条/帧、≈5 MB 顶点（绘制 0.29 ms + 上传 0.14 ms）；四角带色四边形
之后是 **13 个原语、12.8 KB、0.01 ms/帧** —— 这条用例就是那次的判据，留着防止回退。

## baseline_snapshot —— 迁移基线快照（三端像素 + 原语流哈希 + 画质数字）

```sh
# 生成快照（提交进仓库的那个文件）：三端各渲一遍 + draw_bench 哈希 + bin2d 画质
mingw32-make SHELL=sh baseline

# 对快照（每步迁移之后跑一次）：报告哪一端、哪个用例动了
mingw32-make SHELL=sh baseline-check

# 要求"这一步必须逐位相同"时用：
mingw32-make SHELL=sh baseline-strict
```

它解决的是**像素回归回答不了的那半个问题**：`pixel_diff` 只说"在容差内吗"，
说不出"哪个用例、哪一端动了"。迁移 `<path>` 到 bin2d 时像素一定会动一点点，那时最需要的
恰恰是"动的正好是路径相关的用例，别的一个都没动"。

口径：

- 每个用例每端一个 **FNV-1a 64**，算在**原始 ARGB** 上（`WxH` + 逐行像素），
  PNG 编码器换了也不影响；
- `draw_bench --hash` 固定 `--frames 1 --warm 0`（哈希跨帧累加，帧数不同值不同 ——
  实测 `--frames 1/2/5` 给出三个不同的值）；
- bin2d `make quality` 的数字原样收进来，当"画质参照"，不是门禁。

判据分两级，别混：**硬门禁仍是 `pixel_diff` 的容差判据**（它管"能不能接受"）；
快照管"动了什么"—— 用例增删 / 尺寸变化直接失败；哈希变化默认只报告，
要求逐位相同时才加 `--strict`。
