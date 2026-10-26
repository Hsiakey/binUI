# 参与贡献

先看 [README](README.md) 的「组成」与「UI 描述格式」，再看 [docs/](docs/README.md)。
本文件只讲**怎么改得进得来**。

## 目录结构

```
include/                # 公共头（格式 / 编译器 / 渲染器 / 字体 / 图表 / 视频接口
                        #   + binui_paths.h / binui_hot_reload.h 两个 header-only 应用工具）
src/compiler/           # XML、HTML5 → binUI 编译器（含 CSS 解析与级联）
src/renderer/           # binUI 解析、节点状态、命中测试、事件、动画、滚动、文本编辑
src/font/               # FreeType + HarfBuzz 字体服务（字形图集）
src/backends/           # binui_gl-gdi.*（OpenGL 3.3）/ binui_d3d11.*（D3D11）绘制后端
src/platform/           # app_win32.*：Win32 窗口外壳 + GDI 绘制（演示与工具共用）
examples/               # 演示程序（每个子目录 = 一个 demo：main.cpp + main.html）
examples/assets/        # 演示共用图片（构建时拷到各 demo 目录）
tests/                  # 测试（自写的轻量断言，无第三方框架）
tools/                  # 调试工具：离屏渲染、窗口抓取、窗口枚举、示例 UI 检查
docs/                   # 设计文档、图片、设计稿（mockups/）
scripts/                # fetch-deps.sh 等辅助脚本
third_party/            # FreeType / HarfBuzz 源码（不入库，脚本拉取）
```

## 构建与测试

只支持 **C++17**。依赖 FreeType + HarfBuzz，首次构建先拉源码：

```bash
sh scripts/fetch-deps.sh        # 或 make deps
```

**CMake（推荐，跨平台）**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Makefile（MinGW / MSYS2）**

```bash
mingw32-make SHELL=sh all      # 库 + 测试 + 演示
mingw32-make SHELL=sh test
```

> **Windows + Git bash 的坑**：用 Git 自带的 `sh.exe` 当 SHELL 时，Git 的 `mingw64/bin`
> 会排到 PATH 前面，测试程序于是加载 Git 带的旧 `libstdc++-6.dll`，表现为
> `STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139)` 或直接段错误。把目标工具链目录
> （如 `C:\ubin\mingw64\bin`）放到 PATH 最前再跑 `make test`。

## 改代码的硬规矩

1. **核心库不依赖平台 API**。`src/compiler`、`src/renderer`、`src/font`、`include/*`（除后端头）里
   不许出现 `windows.h`、`<GL/…>`、`<d3d11.h>`；平台相关的都放在 `src/backends/binui_gl-gdi.*`、
   `src/backends/binui_d3d11.*`、`src/platform/app_win32.*`。
2. **改二进制布局必须升 `BINUI_VERSION`**，并在 `include/binui_format.h` 顶部注释、
   README 的「binUI 二进制布局」、CHANGELOG 三处同步。新增可选段一律「恒定写出 + 长度前缀」，
   让解析链保持确定（参考视频段、状态样式段）。
3. **"写了没效果"是 bug**。编译器遇到认识但不支持的属性/单位/选择器，必须走 `Warn()`
   打一条编译警告（`CompileResult::warnings` 里也能读到），不许静默忽略。
4. **注释写"为什么"**，尤其是绕过限制的地方（比如 `advance - 1` 的取舍、`hitTestEx` 为什么存在）。
   本项目注释与文档用中文，标识符用英文。
5. **不引入新依赖**。字体只认 FreeType + HarfBuzz，视频解码只认可选的 FFmpeg 模块
   （`BINUI_ENABLE_VIDEO`，默认 OFF），其余一律手写。
6. 4 空格缩进、不加 BOM、文件末尾留换行（见 `.editorconfig`）。

## 测试

- 每个测试是独立可执行文件（`tests/test_*.cpp`），`main` 返回失败数；`make test` / `ctest` 全绿才算过。
- 修 bug 请先补一条能复现的断言，再修。
- 改编译器/渲染器行为时，跑一遍演示 UI 的编译检查（`warnings` 必须为 0）：

  ```bash
  build-mingw/bin/offscreen_render examples/singlewindow/main.html /tmp/x.png   # 顺带验证能被真实后端画出来
  ```

## 新增一个演示

三处一起改，缺一个 master 分支就构建不出这个 demo：

1. `examples/<name>/main.cpp` + `examples/<name>/main.html`；
2. `Makefile`：加入 `DEMO_BINS` 并写链接规则（记得 `cmd /c copy` 把 HTML 拷到 `bin/<name>/`）；
3. `CMakeLists.txt`：`add_executable` + `target_link_libraries(... gl_shell)` + `POST_BUILD` 拷 HTML。

## 提交与 PR

- 一次提交一件事；信息格式 `<范围>: <做了什么>`，例如
  `compiler: 支持 CSS 运行时伪类 → v11 状态样式段`、`renderer: 修复 :hover 对普通元素不生效`。
- PR 描述里写清：**动机 / 改动点 / 验证方式**（跑了哪些测试、哪些演示 UI、warnings 是否为 0）。
- 涉及二进制布局的 PR 请在描述里标注格式版本变化。

## 许可证

提交即表示同意以 [MIT](LICENSE) 授权你的贡献；第三方组件的许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
