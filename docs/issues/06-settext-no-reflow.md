> **状态：已修复：规格段带 heightAuto + autoBase，setText 按与编译期同一口径重算高度；宿主也可用 binui_set_node_bounds 自己撑高。**

# 运行期 `setText` 不重算文本高度：换成长文本就溢出/压到下一个元素

> 建议标签：`enhancement`（也可以当 `bug` 看：C ABI 里 `set_text` 是主要用法）· 面积：渲染器 + C ABI
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 平台 / 后端：无关（三个后端都按节点矩形绘制）
> 编译警告：无

## 现象

文本节点"未声明高度时按估算行数撑开高度"这一步**只发生在编译期**。
运行期用 `binui_set_text()` 换成更长的文本后：

- 节点 `height` 不变；
- 文本按节点宽度重新折行，第二行及以后**画到节点盒外**，压到下一个元素身上
  （或者被父级的裁剪吃掉，看有没有 `overflow`）。

C ABI 里没有任何"改高度/改坐标"的接口，宿主也没法自己救。

## 复现（概念层面，代码即证据）

```html
<!-- 编译期文本很短，高度被估成 1 行 -->
<span id="log" style="position:absolute; left:20; top:20; width:300; font-size:13px">OK</span>
<!-- 下面这个元素紧挨着它 -->
<div style="position:absolute; left:20; top:44; width:300; height:40; background:#F1F3F5"></div>
```

```c
binui_set_text(app, find("log"), "很长的一句……", -1);   // 变成 3 行
// 结果：第 2、3 行盖在下面那个 div 上
```

GDI/GL/D3D11 表现一致（都按 `node.height` 裁剪或直接压图层）。

## 根因线索

- 估算只在编译期做：`src/compiler/binui_compiler.cpp:4116`
  「文本节点未声明高度时估算行数并撑开高度」——注释里就写了
  「三个后端都按节点矩形绘制，谁也没办法把节点撑高」。
- 运行期 `setText` 只赋文本：`src/renderer/binui_renderer.cpp:688`

  ```cpp
  void BinUIRenderer::setText(uint16_t nodeId, const std::string& text) {
      ...
      nodes_[it->second].text = text;     // 没有 height 重算、没有重排、没有失效通知
  }
  ```

- 渲染器里也没有任何"文本度量/行数"的运行时实现（`textWidth` / `fontMetrics`
  都只存在于各绘制后端，核心库里没有）。

## 建议修法

按投入排序：

1. **给"高度是自动估的"这件事留个标记**。现在编译器算完就把值写死进 `height`，
   运行时分不清"作者写的"和"估算的"。用 flags 的空闲位加一个
   `BINUI_NF_HEIGHT_AUTO`（`include/binui_c.h` 里 `BINUI_NF_*` 只用到 bit 7，还有余量），
   `set_text` 时对这个标记的节点重新估算高度。
   估算需要一个度量接口 —— 可以给 `BinUIRenderer` 注入一个 `measureText` 回调，
   或者沿用编译器那套"CJK 1.0×、拉丁 0.52×"的粗估（够用，且不引入字体依赖）。
2. **给 C ABI 补 `binui_set_node_bounds(app, id, x, y, w, h)`**。哪怕不做自动估算，
   宿主也能自己按新文本算高度再撑开 —— 这条其实也是
   [07-cabi-runtime-apis.md](07-cabi-runtime-apis.md) 里的一项。
3. 如果两条都不做，至少要在 `binui_set_text` 的文档里写清楚"不会重排，
   文本长度变化请自己预留高度"，并且给出"预留槽位"的示例 ——
   否则用 C ABI 写列表/日志流的人一定会踩。

（我这边是靠"布局里预留固定高度的槽位 + 只往里填等长文本"绕过去的，
体验上就是"消息一长就得出界"，很别扭。）
