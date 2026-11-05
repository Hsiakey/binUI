> **状态：已修复：button/input 默认底色改透明；三个后端改为读 BINUI_TEXT_CENTER/RIGHT 与 PAINT_WEIGHT；button 的 UA 默认居中由编译器补。**

# `<button>` / `<input>` 的默认底色写死 `#222222`，且按钮文字强制居中 + 700 粗体

> 建议标签：`bug` · 面积：编译器（默认样式）+ 绘制后端（三个后端各有一份）
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 平台：Windows（GL / D3D11 / GDI 三个后端都有）
> 编译警告：无
> 相关：这条是"浅色主题下按钮变黑块"的根因

## 现象

1. **不写 `background` 的 `<button>` / `<input>` 一律是 `#222222` 深灰底**（不是透明）。
   深色主题下看不出来，浅色主题里就是一坨黑块 —— 表现为"这个按钮/标签怎么是黑的"。
2. **按钮文字永远水平居中**，`text-align: left` 被忽略；`BINUI_TEXT_CENTER` /
   `BINUI_TEXT_RIGHT` 两位 flags 对 `BINUI_BUTTON` 无效。
3. **按钮文字永远是 700 粗体**，`font-weight` 被忽略。

合起来的实际后果：想做一个"左侧图标 + 左对齐文字"的列表行（侧栏导航、会话列表那种），
只能写成"空 `<button>` 当底（要显式写 `background`），文字用绝对定位的 `<span>` 盖上去"。
这是一个能忍但很别扭的约定，而且新人一定会踩。

## 最小复现

`assets/probe.html` / `assets/probe.png`：

```html
<style>
  .b1 { background:#2C2C2E; border-radius:12; font-size:14px; }
  .b2 { background:#2C2C2E; border-radius:12; font-size:14px; text-align:left; }
  .b3 { background:#2C2C2E; border-radius:12; font-size:14px; text-align:left; padding-left:14px; }
</style>

<button class="b1" style="position:absolute; left:20; top:20;  width:252; height:36">默认按钮 alignment</button>
<button class="b2" style="position:absolute; left:20; top:70;  width:252; height:36">左对齐 text-align:left</button>
<button class="b3" style="position:absolute; left:20; top:120; width:252; height:36">左对齐 + padding</button>

<!-- 对照：div + span 是左对齐的 -->
<div  style="position:absolute; left:20; top:180; width:252; height:36; background:#2C2C2E; border-radius:12"></div>
<span style="position:absolute; left:34; top:188; width:200; height:20; font-size:14px">div 行内文字</span>
```

三个按钮的文字都在正中（`text-align` 与 `padding-left` 都不生效），只有底部的 div 是左对齐。

默认底色复现更简单：任意 `<button>` 不写 `background`，放到浅色底上
（`<html bg="#FFFFFF">`）——会看到一个 `#222222` 的方块。

## 根因线索

**默认底色**（`src/compiler/binui_compiler.cpp:4352`）：

```cpp
uint32_t defBg = (type == BINUI_LABEL)  ? 0xFFFFFFFFu
               : (type == BINUI_SVG || type == BINUI_CANVAS) ? 0x00000000u
               : (type == BINUI_VIDEO) ? 0xFF101010u
               : (type == BINUI_BUTTON || type == BINUI_INPUT) ? 0xFF222222u   // ← 这里
               : 0x00000000u;   // v13：容器默认透明（以前是 #222222，浅色主题直接被吃掉）
if (bgRaw.empty()) n.color = defBg;
```

有意思的是下一行的注释：容器已经被改成透明了（理由正是"浅色主题直接被吃掉"），
但 button / input 这一档没跟着改。

（`BINUI_LABEL` 那一档的 `0xFFFFFFFF` 是个摆设，不会真画出白块：LABEL 只在"显式给了
文字色或渐变"时才铺底，见 `src/backends/binui_gl-gdi.cpp:1938`。所以能说"只有
button / input 这一档真的会画出默认底色"。）

**文字排版**（GL 后端 `src/backends/binui_gl-gdi.cpp:1999-2011`）：

```cpp
int bs = node.fontSize ? node.fontSize : 14;
float tw = gfx.textWidth(static_cast<float>(bs), 700, node.text, italic);   // ← 权重写死 700
int asc = 0, lh = 0;
gfx.fontMetrics(static_cast<float>(bs), 700, asc, lh, italic);
float tx = x + pad + (nw - 2 * pad - tw) / 2.0f;    // ← 强制水平居中
gfx.drawText(tx, base, static_cast<float>(bs), 700, node.text, tcol, false, false, italic);
```

完全没读 `node.flags` 里的 `BINUI_TEXT_CENTER` / `BINUI_TEXT_RIGHT`，也没读条目里
作者声明的 `font-weight`。对照：同一个文件里 `LABEL` 分支是读 flags 的
（`src/backends/binui_gl-gdi.cpp:1945` 起的 `align` 计算），所以这不是"flag 没做"，
就是 `BUTTON` 分支漏了。同样的写法在另外两个后端各有一份：

- `src/backends/binui_d3d11.cpp` 的 `case BINUI_BUTTON:`（`:1929`）
- `src/platform/app_win32.cpp` 的 `case BINUI_BUTTON:`（`:965`，用的是 `FW_BOLD`）

## 建议修法

1. **默认底色**：`BINUI_BUTTON` / `BINUI_INPUT` 也改成 `0x00000000`（透明），
   让"没写就是透明"这条 v13 已经对容器生效的规则一致。按钮想要可见底色，
   示例与组件库里本来就都写了 `background`，不受影响。
   如果担心破坏老用户的观感（老布局依赖 `#222222`），可以退一步：
   默认值跟随根节点的 `color`（`body{color:…}` 已经是继承起点）或显式的 UA 样式表，
   至少让浅色主题能覆盖掉。
2. **文字对齐**：按钮绘制时读 flags——

   ```cpp
   float tx = x + pad;
   if (node.flags & BINUI_TEXT_CENTER) tx = x + (nw - tw) / 2.0f;
   else if (node.flags & BINUI_TEXT_RIGHT) tx = x + nw - pad - tw;
   ```
   （语义与 `LABEL` 的 `text-align` 保持一致即可。）
3. **字重**：`textWidth` / `fontMetrics` / `drawText` 三处改用条目里声明的
   `font-weight`（`pt` 里已经有这个信息的那条路径，与 LABEL 共用），默认 400 还是 700
   建议保持 700 以免破坏现有示例，但允许被覆盖。

配套：改完在 `examples/features/main.html` 的 ③ 区加一个"左对齐按钮"用例，
`check_examples` 会把它纳入编译检查。
