> **状态：已修复：角片边长改为 r、模板不再内缩、半径到短边一半时九块精确铺满（GL + D3D11）。见 include/binui_paint.h `BuildRoundedNineSlice`。**

# 圆角在接近「短边一半」时退化：胶囊/正圆画不出来（GL 后端）

> 建议标签：`bug` · 面积：绘制后端（GL）/ 渲染器
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 平台：Windows 11 + MinGW-w64 gcc
> 绘制后端：OpenGL 3.3（`binui_gl-gdi`），GDI 后端作对照
> 编译警告：无

## 现象

同一份布局，GDI 后端（`tools/offscreen_render`）画得对，GL 后端（C ABI 建窗）画不对：

- 圆角半径接近盒子短边一半时，**四边中部先出现凹口**；
- 半径到达短边一半时，**完全退化成直角**，也就是胶囊和正圆都画不出来；
- 半径再大一点，角部会向外凸出一块（切片重叠的痕迹）。

这不是"圆角不够圆"的观感问题：`border-radius: 999` 想画个圆形头像/圆形发送键，
在 GL 后端会得到一个正方形。

## 最小复现

`assets/radius-threshold.html`（GL 渲染结果 `assets/radius-threshold-gl.png`）：

```html
<!-- 100x100 盒子，半径 30/35/38/40/42/44/46/48/50 -->
<div style="position:absolute; left:20;  top:40; width:100; height:100; background:#2C2C2E; border-radius:30"></div>
<div style="position:absolute; left:140; top:40; width:100; height:100; background:#2C2C2E; border-radius:35"></div>
<div style="position:absolute; left:260; top:40; width:100; height:100; background:#2C2C2E; border-radius:38"></div>
<!-- … 一直到 border-radius:50（正好短边一半） -->
```

跑：

```powershell
# GL（问题在这里）
build-mingw/bin/xxx.exe            # 或任意 C ABI 宿主加载这份布局
# GDI（对照，正确）
build-mingw/bin/offscreen_render.exe assets/radius-threshold.html out.png
```

另外两份额外数据：

- `assets/radius-matrix.html`：上排 200×200 盒子 r=10/20/30/40/60/90；下排 34×34 r17、
  34×34 r12、26×26 r13、26×26 r9、42×42 r21、42×42 r16、80×80 r40、80×80 r20。
  GL 结果 `-gl.png`，GDI 结果 `-gdi.png`。
- `assets/circle.html`：`<svg><circle r="30/17/13/8/4/3">`，GL 结果 `assets/circle-gl.png`
  **六个圆全是方块**。

## 实测边界（100×100 盒子扫半径）

| 半径 | 半径/短边 | GL 结果 |
|---|---|---|
| 30 | 0.30 | 正常 |
| 35 | 0.35 | 正常 |
| 38 | 0.38 | 四边出现凹口 |
| 40 | 0.40 | 凹口明显 |
| 42–48 | 0.42–0.48 | 越来越方正 + 凹口 |
| 50 | 0.50 | **纯直角** |

其它盒子吻合同一规律：34×34 r17、26×26 r13、42×42 r21、80×80 r40（都是比例 0.50）
全变直角；80×80 r20、26×26 r9（比例 0.25–0.35）正常；200×200 r90（比例 0.45）出凹口。

## 根因线索

`src/backends/binui_gl-gdi.cpp:1382 fillRounded()` 用九宫格拼接：

```cpp
// :925  模板边长
int t = std::max(8, r * 4);
ensureRasterDC(t, t);              // :926 返回值没检查
rt.corner = r * 2 + 1;             // :956  c = 2r+1

// :1404-1410 四角 + 中段
quad(x + w - c, y, c, c, 1 - cu, 0, 1, cu);
quad(x + w - c, y + c, c, h - 2 * c, 1 - cu, strip0, 1, strip1);   // 高度 h-2c 可能为负
```

当 `2c = 4r + 2` 超过盒子短边时：

1. 左右/上下的角片互相重叠，重叠区**把对方的"内角圆弧"露出来** → 四边中部的凹口；
2. 半径再大，中段宽度 `w - 2c` / 高度 `h - 2c` 变为负数，四个角片直接覆盖整块，
   每片自带的弧把整块裁掉 → 直角。

`<circle>` 走的是同一处：编译器把 `<circle>` 编成 `BINUI_LIST` 节点，尺寸 `2r×2r`、
`radius = r`（`src/compiler/binui_compiler.cpp:4528-4531`），正好是"半径 = 短边一半"，
所以是这条 bug 的最严重表现。

顺带两个小问题：

- `ensureRasterDC(t, t)` 的返回值没检查（`:926`），失败后 `memset(rasterBits, 0, t*t*4)`
  会按新尺寸写到旧缓冲上；
- `rounds` 缓存按半径存纹理、没有上限，r=90 时单片 360×360，`border-radius` 取值一多就积压显存。

## 建议修法

按改动量从小到大：

1. **钳中段**：`float c = std::min({r*2.0f+1.0f, w/2, h/2})`，中段宽高 `max(0, w-2c)`，
   同时四角位置改成 `x + w - c` 与 `x + c` 分离。能消掉凹口，但 `r == 短边/2` 时中段为 0，
   四角仍是四个 1/4 圆拼出来的，需要角片本身就能覆盖半张脸才不会看到接缝。
2. **半径到顶时走专用路径**：`2r >= min(w,h)` 时直接用"两端半圆 + 中间矩形"（胶囊）
   或一次解析式绘制，不走九宫格。这条最能解决实际需求（胶囊按钮、圆形头像）。
3. **终极解**：`fillRounded` 改成一个 SDF（`abs(p)-half+r` 的圆角矩形距离场）
   fragment shader，半径作为 uniform 传，任意尺寸/半径都精确，还省掉纹理缓存。
   代价是多一个 shader + 一次 uniform 上传。

无论选哪条，`ensureRasterDC` 的返回值都要检查（`CONTRIBUTING.md` 说的"写了没效果是 bug"）。

## 复刻方的临时绕法（供参考）

在修好之前，把半径压在 `r ≤ 短边 × 0.35` 以内可以稳定得到圆角；需要正圆的地方
只能用 `binui_set_image_raw` 传一张圆形位图。
