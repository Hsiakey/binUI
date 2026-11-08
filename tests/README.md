# tests/ —— 四个主题套件

按**主题**分文件，不再按"哪个特性加了一次测试"分文件。一个套件 = 一个二进制，
内部按小节组织；新增用例往对应文件的注释分节下面加，别再开 `test_xxx.cpp`。

| 套件 | 覆盖什么 | 依赖 |
|---|---|---|
| [`test_css.cpp`](test_css.cpp) | CSS 属性落到节点字段的口径、选择器（单段）、伪元素 `::before/::after`、结构伪类、编译器告警、`docs/issues` 的编译器侧回归 | 核心库 |
| [`test_elements.cpp`](test_elements.cpp) | 二进制格式往返、元素树与布局、命中测试与事件、伪元素绘制几何、圆角/边框条带、滚动、动画与过渡、热重载 | 核心库 |
| [`test_font.cpp`](test_font.cpp) | 字体发现 / 灰度光栅化 / 整形度量 / 豆腐块，以及**输入法（IME）的 UTF-16 光标桥接**（全角字、代理对 emoji、上屏插入、失焦提交） | 核心库 + FreeType/HarfBuzz |
| [`test_capi.cpp`](test_capi.cpp) | C ABI 的字段顺序/返回值语义/两段式取文本（不开窗口） | 核心库 + GL 后端对象 |

`fixtures/` 放测试用的固定 UI（例如 `vision.binui.xml`，原 `vision_monitor` 示例的布局）。

## 跑

```sh
mingw32-make SHELL='sh' test          # 编 + 跑四个套件
build-mingw/bin/tests/test_elements.exe   # 单独跑一个（工作目录须为仓库根）
ctest --test-dir build --output-on-failure
```

`test_elements` 里的热重载用例会往 `build-mingw/tmp/` 写临时副本，不碰仓库；
所以**必须在仓库根目录执行**（相对路径按仓库根解析，和 `examples/` 里的示例一致）。

## 无窗口能测什么、不能测什么

能：编译器、布局、命中、编辑会话、动画曲线、绘制几何、字体光栅化、热重载状态机。

不能（需要真窗口/真输入法，靠示例手测）：

- 组字窗与候选框的屏幕位置 —— `examples/fontshow` 的 ⑤ 输入法小节（`BINUI_IME_LOG=1` 看 `ime place` 行）
- 组字期间点击被 IME 吞掉（非 TSF 窗口的平台限制，需要 `ITextStoreACP` 才能接管）
- GL / D3D 后端的实际像素 —— 靠 `docs/pixels/` 的像素回归（`make pixels`，只在本机可信）

历史上 `test_binui / test_renderer / test_renderer_real / test_compiler / test_anim /
test_hot_reload / test_paint` 七个二进制已合并进上面四个，用例逐条保留（合并时用输出
比对确认：元素组 359 条、CSS 组 373 条，一条没丢）。
