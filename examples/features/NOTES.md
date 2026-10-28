# examples/features

功能演示的目录说明（也是热重载测试用的"只在源码树里存在、不会被拷进构建目录"的文件）。

- `main.html` —— 一个窗口里塞进六块：运行时伪类 / 扩展绘制 / HTML 元素默认语义 /
  组件化（源码组件）/ 组件化（二进制 + 外挂）/ 数据驱动。
- `button.bum.xml` —— 源码组件，编译期内联，可传参（`label` / `w` / `bg`）。
- `card.bum.xml` —— 组合组件，内部再引用 `button.bum.xml`；构建时编成 `card.bum`（二进制）。
- `dock.bum.xml` —— 运行期外挂组件，构建时编成 `dock.bum`，由 `ex::MountAll()` 现场挂载。

构建目录里只需要 `main.html` + `button.bum.xml` + `card.bum` + `dock.bum`；
本文件不参与运行，只用来验证"UI 文件解析会在源码树里兜底"。
