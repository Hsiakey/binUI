> **状态：不修（WON'T FIX）** —— GDI 后端自 v0.16.17 起标记为**遗留（OLD）后端**：
> 只保证"不崩、不阻塞、能出像素基准图"，不再为它新增特性或修这类观感问题。
> 新特性一律只做 GL / Vulkan / D3D11 三端。本文留作记录与复现。

# GDI 的文字矩形比节点盒小一圈，大字号被上下裁掉

> 建议标签：`bug`（观感）· 面积：GDI 后备后端（`src/platform/app_win32.cpp`）
> 版本：binUI 0.16.16，`BINUI_VERSION=24`，commit `bf65f51`
> 平台 / 后端：Windows + GDI（`drawNodesGDI`，窗口后备/离屏基准都走它）；对照 OpenGL 3.3
> 编译警告：无（画出来才知道）

## 现象

同一个盒子、同一个字号，GDI 画的文字**上下各被切掉一条**，GPU 三端（GL / VK / D3D11）完整。
触发条件很普通：**节点有上下 padding**（`div` 默认 `padding: 8`），且字号接近或超过
`盒高 − 2·padding`。这时文字行高（≈ 1.35×字号）比文字矩形还高，GDI 的 `DrawTextW`
按矩形裁剪 —— 于是被切；GPU 后端只裁节点盒，字形从 padding 区溢出到盒内，看起来完整。

复现资产：`assets/gdi-text-clip.html`，两种渲染结果：
`assets/gdi-text-clip-gl.png`（上，GL）/ `assets/gdi-text-clip-gdi.png`（下，GDI）。

同一页里 `<span>`（padding 0）完整、`<div>`（padding 8）被切，说明问题不在字形本身。

## 最小复现

```html
<app_window width="640" height="240" resizable="false">
<html width="640" height="240" bg="#FFFFFF">
  <div  style="position:absolute;left:20px;top:16px;width:600px;height:34px;font-size:22px">div：22px 字 / 盒高 34</div>
  <span style="position:absolute;left:20px;top:58px;width:600px;height:34px;font-size:22px">span：22px 字 / 盒高 34</span>
</html>
</app_window>
```

```
build-mingw/bin/offscreen_render.exe docs/issues/assets/gdi-text-clip.html gdi.png   # 被切
build-mingw/bin/gl_shot.exe          docs/issues/assets/gdi-text-clip.html gl.png    # 完整
```

## 根因

`src/platform/app_win32.cpp` 的 LABEL 分支（`drawNodesGDI` 内）：

```cpp
RECT tr = { x + pad, y, x + nw - pad, y + nh };   // 四边都扣掉了 padding
GdiDrawTextShadowed(hdc, wtext.c_str(), -1, &tr, halign | DT_VCENTER | DT_SINGLELINE);
```

`DrawTextW` 会**按矩形裁剪**（除非 `DT_NOCLIP`；后端没加，也不该加 —— 那样横向也会溢出）。
横向扣 `padding` 是对的（CSS 的 content box），但纵向扣掉之后，行高 > 矩形高就必然被切。
GPU 侧的文字定位是"基线 = 盒子垂直中线 + ascent/2"，只受节点盒裁剪，所以不受影响。

`BINUI_INPUT` / 列表项分支同样用 `{x+pad, y, x+nw-pad, y+nh}`，症状一样；输入框还额外
把 `tr` 换成行高来画光标，所以光标是准的、文字反而被切。

## 如果要修（留给将来）

- 最小改动：纵向不要扣 padding，`RECT tr = { x + pad, y - 4, x + nw - pad, y + nh + 4 }`
  这类外扩；或用 `DrawTextW(... DT_VCENTER | DT_SINGLELINE | DT_NOCLIP)` 再自己按节点盒
  做一次 `IntersectClipRect`（等价于 GPU 的语义）。
- 但这是 GDI 后端的**专属**问题，改它不影响三端像素（`make pixels` 的基准图由 GDI 生成，
  改了要重出基准）。按 v0.16.17 的支持级别决定：不修。

## 相关的 GDI 已知差异（同样不再投入）

| 差异 | 原因 | 影响 |
|---|---|---|
| `box-shadow` 环边界吸整像素 | GDI 没有 AA 填充 | 与 GPU 差几十个边缘像素（`docs/pixels/shadow.png` 就是 GDI 基准） |
| 组透明度是逐元素近似 | GDI 没有离屏合成面 | 重叠区比 GPU 略深（`docs/future.md` P1-7） |
| emoji 无 `text-shadow` | 彩色格子乘阴影色会糊成彩团 | 与 GPU 端一致（GPU 也明确不画） |
| 文字阴影走 `DrawTextW` 烘位图 | 没有共享字形图集 | 阴影与正文同源，但字形光栅化与 GPU 端不同 |
| 本文这条（文字被 padding 裁） | `DrawTextW` 按矩形裁剪 | 大字号 + padding 的标签上下缺字 |
