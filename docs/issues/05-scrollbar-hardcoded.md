> **状态：已修复（配色 + gutter）** —— 滚动条样式来自 `BinUIScrollbarStyle`，默认按根节点底色亮度派生，C ABI 可用 `binui_set_scrollbar_style()` 覆盖；CSS `scrollbar-*` 会告警。**gutter 也补上了**：真的要出竖直滚动条时，滚动容器右侧扣掉滚动条宽度，内容不再被压住（三个后端一致）。

# 滚动条颜色/宽度写死在渲染里，浅色主题下是一条黑杠

> 建议标签：`bug`（或 `enhancement`，看是否接受"这是故意不做"）· 面积：绘制后端（GL）
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 平台 / 后端：Windows + OpenGL 3.3（`binui_gl-gdi`）
> 编译警告：无

## 现象

`overflow: auto` 的滚动容器，滚动条由渲染器自己画：

- 轨道固定 `#2E2E2E`、宽 **8px**；
- 滑块固定 `#5A5A5A`、宽 **4px**；
- 没有任何 CSS 属性能覆盖，也没有 C API 能改。

深色主题下不显眼；浅色主题下就是右边一条纯黑竖杠，和"清新浅色"整体风格冲突。
另外滚动条是**直接压在内容上的**（不预留 gutter），最右侧的文字/气泡会被盖住一部分。

截图：`assets/scrollbar-light.png`（浅色界面右侧那条黑杠；同一份布局在深色下几乎看不出来）。

## 最小复现

任意浅色底 + 一个 `overflow:auto` 的容器，内容高度超过视口：

```html
<app_window width="600" height="400" bg="#FFFFFF">
<html width="600" height="400" bg="#FFFFFF">
<body>
  <div style="position:absolute; left:20; top:20; width:400; height:300; overflow:auto; background:#F9FAFB">
    <div style="position:absolute; left:0; top:0; width:400; height:900; background:#F1F3F5"></div>
  </div>
</body></html>
</app_window>
```

右侧出现一条 8px 的黑条。

## 根因线索

`src/backends/binui_gl-gdi.cpp:2190-2204`（滚动条绘制，与节点绘制同处一个函数）：

```cpp
int barW = 8;
gfx.fillRect(x + nw - barW, y, barW, nh, 0xFF2E2E2E);           // 轨道：写死深灰
float thumbH = std::max(24.0f, nh * nh / ch);
gfx.fillRect(x + nw - barW + 2, y + thumbY, 4.0f, thumbH, 0xFF5A5A5A);   // 滑块：写死中灰
```

颜色是字面量，宽度也是字面量，没有任何配置入口。

## 建议修法

两个层次，建议都做：

1. **可配色**：接受标准 CSS `scrollbar-color: <thumb> <track>`（以及 `scrollbar-width`），
   在编译器里落到节点属性上；C ABI 侧再加一个兜底：
   `binui_set_scrollbar_style(void* app, uint32_t track_argb, uint32_t thumb_argb, int32_t width)`。
   按 `CONTRIBUTING.md` 的规矩：如果暂时不实现，就要在编译期 `Warn()`，
   而不是让作者写了没效果。
2. **预留 gutter**：`overflow:auto/scroll` 的容器把 `kScrollBarW` 从内容宽度里扣掉
   （现在 `viewport` 计算里没有扣），这样最右侧内容不会被滚动条压住。
   这一条会和布局/命中测试相关，改动面稍大，但能在 `BinUISpecNode` 里带一个
   `gutter` 标记落地。

如果确定"滚动条样式不做"，那也建议把颜色改成**从节点文字色派生**
（例如 `mix(node.textColor, transparent 40%)`），至少浅色主题不会出现黑条。
