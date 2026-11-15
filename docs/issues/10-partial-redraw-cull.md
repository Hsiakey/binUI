# 10 - 局部重绘的剔除判据写反了一侧：hover 之后沿脏矩形冒出一圈"影子 / 边框"

**状态**：已修（v0.16.32）。
**现象**：鼠标扫过按钮（`:hover`）之后，按钮附近出现一块**边缘笔直**的浅色块 ——
像影子、像边框、又像"卡片缺了一角"；鼠标移开或在别处动一下，有时会自己消失。
**影响面**：所有走局部重绘的路径（窗口每帧只重画变化的节点）+ GL / Vulkan / D3D11 / GDI **四端都有**；
阴影 / 圆角卡片 / 相邻元素越多越明显。

## 复现

`examples/buttonshow`，鼠标停在 ②「尺寸与图标」里的 **大号** 按钮上（窗口 1020×684 时约 (278, 222)），
等 `transform: scale(1.04)` 过渡跑完，看按钮下方跨两个卡片的缝隙：

```
build-mingw/bin/partial_shot.exe examples/buttonshow/main.html build-mingw/tmp/before.png 278 222 40
```

左 = 修前（浅色块 + 硬边），右 = 修后：

![局部重绘剔除：修前 / 修后](assets/partial-redraw-before-after.png)

## 根因：剔除判据的四条边里，两条写成了"节点的边进入了 clip 的边 + pad 以内"

绘制遍历在每个节点上做一次"这块要不要画"的快速判定（`binui_draw_nodes.h`，GDI 侧另有一份
等价实现）。正确语义只有一种：**节点的（含阴影外扩的）盒子与脏矩形不相交才剔除**。当时的写法：

```cpp
// v0.16.31 及以前（错的）
if (x - shPadX + nw <= clipL || x + shPadX >= clipR ||
    y - shPadY + nh <= clipT || y + shPadY >= clipB) continue;
```

左/上两条是 `右边 - pad <= clip 左边`、`下边 - pad <= clip 上边` —— 偏保守（少剔除，安全）；
右/下两条却是 `左边 + pad >= clip 右边`、`上边 + pad >= clip 下边` —— 判的是
**"节点的左/上边进入 clip 右/下边 pad 以内"**，于是：

> 紧贴脏矩形右/下边缘、**明明还有几列/几行像素可见**的节点，被整块丢掉了。

丢掉的是这个节点画的所有东西——底色、边框、**阴影**。而脏矩形内部已经被 `beginFrame(clip)`
清成了画布底色（GL 是白色，其余后端各自等价），于是那块区域显示"底色 + 邻居的阴影"，
和周围"正确内容"的边界正好是脏矩形的直边 —— 看起来就是一圈硬边亮块。

**为什么以前没被发现**：整帧重绘时 `clip` = 整屏，四条边的边界情况碰不到任何节点
（节点都在屏幕内），所以 16 张像素基准、三端 diff 一直是 0。这条轨道只在"clip 是几百像素的
脏矩形"时才会踩到，而脏矩形只在**局部重绘**时出现。

**为什么 hover 才冒出来**：hover 会改节点尺寸 → 外壳调 `invalidRect` + `InvalidateRect(脏矩形)` →
`WM_PAINT` 把 clip 传给绘制。没有 hover（或鼠标没落在可 hover 的节点上）时，外壳整屏重绘，看不到。

## 修法

1. 判据收成**一份**实现（`include/binui_paint.h`）：

   ```cpp
   inline bool PaintNodeInClip(float x, float y, float w, float h,
                               float padX, float padY,
                               float clipL, float clipT, float clipR, float clipB) {
       if (x + w + padX <= clipL) return false;   // 整个在左
       if (x - padX >= clipR) return false;       // 整个在右
       if (y + h + padY <= clipT) return false;   // 整个在上
       if (y - padY >= clipB) return false;       // 整个在下
       return true;
   }
   ```

   三个 GPU 后端（同一份 `binui_draw_nodes.h`）+ GDI（`app_win32.cpp`）都改成调它；
   文字阴影的外扩（`tsPadX/tsPadY`）也一起并进 pad（以前只算 box-shadow，文字阴影溢出的
   节点在脏矩形边缘同样会被剔掉）。

2. **脏矩形要把阴影算进去**：`invalidRect` 以前完全没算 `box-shadow` / `text-shadow` 的外扩
   （只有 `pad=2`），于是 hover 时"新长出来的阴影"没有重绘 → 阴影边缘留一圈旧像素。
   现在它调与绘制侧**同一份**外扩公式（`PaintShadowPad` / `PaintTextShadowPad`，含缩放动画的
   `drawScale`），有阴影时再多给 1px 余量，并把结果向外取整到整像素 —— 调用方 `(int)` 截断
   从此是无操作，脏矩形永远保守。

## 回归

- 单测（`tests/test_elements.cpp` 的 `suite_hover_dirty`）：`PaintNodeInClip` 的 9 个边界用例
  （含"紧贴右/下边界、有一列/一行可见"这两个回归点）+ `invalidRect` 含阴影外扩的 4 条断言。
- 工具（`tools/partial_shot.cpp`，`make partial`）：**同一帧画面的两条路径对拍** ——
  整帧渲染 vs "只清 + 只画一个脏矩形"。4 个 hover 点全部通过；阈值 `diff<=64 且 max<=1`
  （残差是 MSAA 解析在脏矩形边界上的 1 单位噪声）。修前同一用例是 `diff=5502 max=16`。
- 整帧像素基准（`make pixels` 16/16、`pixels-vk`、`pixels-d3d11`）不受影响 —— 它们走整屏重绘。

## 教训

**"整帧渲染对"与"局部重绘对"是两条不同的代码路径，必须各自有基准。**
这个 bug 从 0.16.9（引入局部重绘）一直活到 0.16.31，期间三端像素 diff 全绿、示例跑得好好的，
原因就是所有基准都在整帧那一条路上。现在 `make partial` 把另一条路也钉住了，
而它的比较对象不是"金标准图"，是"同一状态下的整帧渲染" —— 不需要维护基准图，
也就不会因为样式改动而失效。
