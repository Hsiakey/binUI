# examples/ —— 8 组使用示例

一个组一组示例，一组只演示一件事。所有示例共用同一套浅色主题令牌
（页面底 `#F4F7FC` / 白卡片 + 1px 边框 / 主字 `#1F2A37` / 次字 `#66768C` /
主色 `#3E8BFF`），并且都用 `components/` 的标准标题栏。

**窗口尺寸分两类**：除 `flexshow` 外的 7 个示例都是**固定像素布局**（绝对定位排版），
所以统一写 `<app_window … window_resize="disable">` —— 能拖边但没有重排能力，拖了只会错位；
`flexshow` 是唯一开窗可缩放的示例（百分比 / flex 自适应），用来验证 resize 实时重排。

| 目录 | 组 | 演示什么 | 后端 |
|---|---|---|---|
| [`features/`](features/) | **功能** | 运行时伪类、v13 扩展绘制（边框/渐变/字重/行高/装饰）、v14 HTML 元素默认语义、组件化三形态（源码 `.bum.xml` / 二进制 `.bum` / `external` 外挂）、数据驱动 `setText` + 局部重绘 | GL |
| [`animshow/`](animshow/) | **动画** | 缓动曲线、方向与次数、时长、fill 模式、infinite、悬停过渡、运行时 `setColorOf` API | GL |
| [`fontshow/`](fontshow/) | **字体与输入法** | 字号阶梯 12–32、字重 400–800、斜体、行高 100%/170%、下划线/删除线/上划线、中英混排与符号（含缺字回退），以及 IME：组字串内联在光标处、候选框跟着光标跳、选词上屏回显码点、光标高度跟行高走（`BINUI_IME_LOG=1` 出消息日志） | GL |
| [`buttonshow/`](buttonshow/) | **按钮样式** | 语义色 / 尺寸与图标 / 圆角 / 纯色·渐变·描边·幽灵底 / `:hover`·`:active`·`:checked`·`disabled` / 图标·文本·块级形态（纯 CSS，可直接照抄的按钮色板） | GL |
| [`flexshow/`](flexshow/) | **自适应布局** | 百分比宽度、`flex:1` 分配剩余空间、`min-width`/`max-width` 钳制、`width:content` 按文字宽、`flex:1` + `overflow:auto` 的可变视口；整份 UI 无绝对定位，**拖窗口边框实时重排**（唯一可缩放窗口的示例） | GL |
| [`svgdemo/`](svgdemo/) | **2D 渲染** | `<svg>` 声明式矢量（line / polyline / polygon / rect / circle）与 `<canvas>` 命令式绘制面 | GL |
| [`cubedemo/`](cubedemo/) | **3D 渲染** | 引擎不做 3D：顶点变换 / 透视投影 / 深度调色全在应用里算，只用 `gfx.fillLine` 画线框立方体 | GL |
| [`texturedemo/`](texturedemo/) | **纹理** | `updateImageRaw` 运行期上传程序化纹理，同一张纹理多尺寸采样；与视频帧同一条通路 | GL |

> 这些是**使用示例**（怎么用引擎），不是测试用例。测试用例在 [`tests/`](../tests/)（4 个主题套件，
> 产物落 `build-mingw/bin/tests/`）；界面设计稿/原型（如 `galgamechat`、`singlewindow`）在
> [`docs/mockups/`](../docs/mockups/)，只是 HTML，不参与构建。

## 构建与运行

```sh
mingw32-make all          # 产物在 build-mingw/bin/examples/<name>.exe
build-mingw/bin/examples/features.exe
```

CMake 构建同理（`cmake --build build`，产物同样落 `build/bin/examples/`）。每个示例的 UI 源码在构建后
会被拷到 `<exe 目录>/<name>/main.html`，程序默认在那里找；加 `--ui <path>` 或 `--watch`
可以直接盯源码树（保存即热重载）。

示例 UI 用 `../../components/xxx.bum.xml` 引用标准组件 —— 相对 `bin/examples/<name>/` 往上两级
就是 `bin/components/`，构建期会从仓库根 `components/` 同步过去，所以单拷 exe 出去要连
`components/` 一起带。

## 写新示例的约定

1. 目录名 = 可执行文件名 = UI 子目录名（`examples/<name>/main.html`）。
2. 布局用绝对定位的示例要写 `window_resize="disable"`（照抄现有示例的 `<app_window>`）；
   要做自适应就参考 `flexshow`，注意 `<html>`/`<body>` 不是节点，flex 容器要自己包一层
   `width:100%;height:100%` 的 div。
3. 标题栏用标准组件（`icon` + `iconw="22"` 给个字形，`title` / `sub` 写标题与一句话说明）：

   ```html
   <use src="../../components/titlebar.bum.xml" x="0" y="0" w="<窗口宽>" icon="◆" iconw="22"
        title="…" sub="…"/>
   ```

   它占顶部 44px（白底 + 1px 分割线 + 右上三颗窗口按钮），窗口高度要把它算进去；
   内容块从 `y=56` 起（44 + 12 间距），末块距底留 20px 左右。
4. 颜色照抄上面的令牌，别另发明一套。
5. 加进 `Makefile` 的 `DEMO_BINS` 与 `CMakeLists.txt` 的 `add_gl_demo(...)`，
   并保证 `check_examples` 跑完 0 问题（CI 会把警告也算成问题）。
6. 一个特性顺手写个 demo 就够，别为一个属性再开一组；**要验证行为请写进 `tests/` 的对应套件**，
   示例只负责"让人看见"。
