# examples/ —— 6 组示例

一个组一个示例，一组只演示一件事。所有示例共用同一套浅色主题令牌
（页面底 `#F4F7FC` / 白卡片 + 1px 边框 / 主字 `#1F2A37` / 次字 `#66768C` /
主色 `#3E8BFF`），并且都用 `components/` 的标准标题栏。

| 目录 | 组 | 演示什么 | 后端 |
|---|---|---|---|
| [`features/`](features/) | **功能** | 运行时伪类、v13 扩展绘制（边框/渐变/字重/行高/装饰）、v14 HTML 元素默认语义、组件化三形态（源码 `.bum.xml` / 二进制 `.bum` / `external` 外挂）、数据驱动 `setText` + 局部重绘 | GL |
| [`animshow/`](animshow/) | **动画** | 缓动曲线、方向与次数、时长、fill 模式、infinite、悬停过渡、运行时 `setColorOf` API | GL |
| [`fontshow/`](fontshow/) | **字体** | 字号阶梯 12–32、字重 400–800、斜体、行高 100%/170%、下划线/删除线/上划线、中英混排与符号（含缺字回退说明） | GL |
| [`svgdemo/`](svgdemo/) | **2D 渲染** | `<svg>` 声明式矢量（line / polyline / polygon / rect / circle）与 `<canvas>` 命令式绘制面 | GL |
| [`cubedemo/`](cubedemo/) | **3D 渲染** | 引擎不做 3D：顶点变换 / 透视投影 / 深度调色全在应用里算，只用 `gfx.fillLine` 画线框立方体 | GL |
| [`texturedemo/`](texturedemo/) | **纹理** | `updateImageRaw` 运行期上传程序化纹理，同一张纹理多尺寸采样；与视频帧同一条通路 | GL |

## 构建与运行

```sh
mingw32-make all          # 产物在 build-mingw/bin/<name>.exe
build-mingw/bin/features.exe
```

CMake 构建同理（`cmake --build build`）。每个示例的 UI 源码在构建后会被拷到
`<exe 目录>/<name>/main.html`，程序默认在那里找；加 `--ui <path>` 或 `--watch`
可以直接盯源码树（保存即热重载）。

## 写新示例的约定

1. 目录名 = 可执行文件名 = UI 子目录名（`examples/<name>/main.html`）。
2. 标题栏用标准组件：`<use src="../../components/titlebar.bum.xml" .../>`。
3. 颜色照抄上面的令牌，别另发明一套。
4. 加进 `Makefile` 的 `DEMO_BINS` 与 `CMakeLists.txt` 的 `add_gl_demo(...)`，
   并保证 `check_examples` 跑完 0 问题（CI 会把警告也算成问题）。
