# tools —— 调试与验收工具

都是独立小程序，不参与库构建。统一在仓库根目录执行（工具里的相对路径按仓库根解析）。

## check_examples —— 示例 UI 零警告检查

把 `examples/` 下所有 `*.html`、`*.binui.xml`（`mockup*.html` 设计稿除外）逐个编译，
**编译为空或产生任何编译警告都算失败**。CMake 里已注册成 ctest 用例（CI 会跑）。

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
build-mingw/bin/offscreen_render examples/cssshow/main.html docs/img/cssshow.png
build-mingw/bin/offscreen_render examples/singlewindow/main.html /tmp/a.png --scrollTo 120
build-mingw/bin/offscreen_render examples/vision_monitor/main.binui.xml /tmp/b.png --setText log1="hello"
```

注意：离屏渲染只呈现**静态状态**——`::before/::after`、结构伪类、基础样式都能看到，
但 `:hover`/`:focus`/`:checked` 这类运行时状态需要真实交互才生效（渲染器当前状态决定）。

## window_capture —— 抓取指定窗口内容为 PNG

```bash
g++ -std=c++17 -O2 tools/window_capture.cpp -lgdiplus -lgdi32 -luser32 -o build-mingw/bin/window_capture.exe
build-mingw/bin/window_capture "CSS" docs/img/shot-cssshow.png    # 参数是窗口标题子串
```

用 `PrintWindow` 抓，**对 OpenGL/D3D 窗口通常返回空白**（GPU 内容不在 GDI 表面里），
抓 GDI 后端窗口（如 `vision_monitor`、`offscreen_render` 的结果）才可靠。

## enum_windows —— 列出可见窗口标题

```bash
g++ -std=c++17 -O2 tools/enum_windows.cpp -luser32 -o build-mingw/bin/enum_windows.exe
build-mingw/bin/enum_windows.exe
```

找窗口标题用（配合 `window_capture` 的标题子串参数）。

## verify_shots.py —— 截图颜色校验

对 PNG 采样若干坐标并比对期望色，用于检查"某处到底是不是这个颜色"。

```bash
python tools/verify_shots.py docs/img/cssshow.png
```

需要 Pillow（`pip install pillow`）。
