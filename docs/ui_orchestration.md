# 多页面 · 多组件 · 编排（appshow）

> `examples/appshow` 是这个主题的活例子。这份文档解释它**为什么这么写**，
> 以及在当前引擎能力下，"多个页面/组件一起动"到底能做到什么程度。

## 1. 一句话原则

**微动画写在组件里，进出场写在容器/宿主里，先后顺序由宿主记账。**

引擎侧只有"单个节点的属性在时间轴上插值"这一种原语（`@keyframes` 关键帧、
`transition` 过渡、运行时 `setStyleFloat/setStyleColor`），**没有跨节点编排**：
没有动画组、没有时间轴、没有完成事件。所以"配合"只能靠**职责切分 + 相位差 + 宿主的
时间片**三件事拼出来。

## 2. 三层职责

| 层 | 该写什么 | 不该写什么 | appshow 里的位置 |
|---|---|---|---|
| **组件**（`.bum.html`） | 自己被操作时的反馈：hover/press/focus 的 `transition`；常驻的呼吸/脉冲 `@keyframes` | 进出场、页面级时序、业务状态 | `appshow-card` 的 `as-breathe`、`appshow-navitem` 的 `:hover` |
| **容器/壳**（`main.html`） | 页面容器、遮罩、弹窗、提示条的**几何与层级**；`transition` 声明（让宿主能驱动） | 页面切换的先后顺序（它不知道"谁在切"） | `#page-0..3`、`#veil`、`#toast`、`.page`/`.row` 的样式 |
| **宿主**（`main.cpp`） | 状态机：切页/开窗/交错/自动关闭；逐帧 tween；把结果写回引擎 | 具体像素、具体元素外观 | `Shell::goPage / openDialog / toast / replayRows / tickFades` |

## 3. 当前引擎的四条硬约束（决定了实现手法）

1. **`opacity` / `scale` 是节点自身属性，不向后代继承。**
   所以"整页淡出"必须由宿主对**子树里每个节点**设值。`appshow` 启动时把每页子树
   `subtree(pageId)` 拍平成数组，tween 时一次性 `setStyleFloat(id, ANIM_OPACITY, v)`。
   代价：一页几十个节点就是几十次调用 + 几十个失效矩形；换来的是不依赖新功能。
2. **没有"动画完成事件"。**
   所以"旧页退完再进场"不能等回调，只能**宿主自己记时间**（`sh.now` 与 `Fade::start/dur`
   对齐）。这也是为什么 `appshow` 里有自己的 `Fade` 调度器。
3. **一个节点只有一条 `animation`。**
   想让一个元素同时滑动 + 淡入 + 旋转，必须合并成**同一条 `@keyframes`**，让各属性在
   各自的关键帧上插值（`animshow` 的 `combo`/`compound` 就是这么写的）。
4. **`press` / `release` 的命中口径必须一致**（0.12.3 修）。
   `press` 用 `hitTestEx(allowStyled=true)`（"声明了状态样式的普通元素"也算可点），
   而 `release` 早先用的是严格 `hitTest`（只认 `BINUI_INTERACTIVE`）——
   于是"只写了 `:hover` 的普通 div"按下去有视觉反馈、松手却永远不产生 CLICK，
   点击被**静默吞掉**。现在两处同口径，且 `allowStyled` 接受任意一种状态样式
   （只写 `:hover` 也算）。
5. **`@keyframes` 是全局命名空间。**
   `<use src>` 是编译期内联到同一个文档、`kfGroups` 按名字全局匹配且**先到先用**，
   两个组件都定义 `@keyframes fade` 会静默串味。约定：`@keyframes <组件名>-<用途>`。

另外两条容易被坑的：

- **`drawOpacity` 先看动画结果、后看 `floatTargets`**：给一个"自己正在跑 opacity 关键帧"
  的节点设 `ANIM_OPACITY` 是**无效**的。所以常驻微动画不要动 `opacity`
  （`appshow-card` 的呼吸只动 `scale`，就是为了让宿主的淡出始终有效）。
- **CSS 规则覆盖内联 style**（本引擎的既定口径），所以组件参数尽量走 `{{name=默认}}`
  而不是在调用处写内联 style 去盖。

## 4. 编排模式：相位差 + 交错

`appshow` 里所有"配合"都是同一条时间轴上的**相位差**：

```
切页（上下切入切出）：
        旧页 translateY 0→-22 + opacity 1→0   [0ms ─────── 160ms] → 隐藏
        新页 translateY +26→0 + opacity 0→1          [60ms ───────── 280ms]
        数据页 6 行                                  [160+i*45ms ─── +200ms]  ← 交错

弹窗：  遮罩 opacity 0→1  [0ms ─── 160ms]
        面板（整棵子树）   [40ms ───── 220ms]                  ← 面板晚 40ms

提示条： 0→1 [0..150ms] → 停 1.7s → 1→0 [..+250ms] → 隐藏
```

`Fade::hold` 是这套机制的关键细节：一轮交错里，**还没轮到的行要保持 0**，
否则会被整页淡入提前点亮（`appshow` 第一版就踩了，日志里表现为后面的行先变成 1.00）。

## 5. 被动查询：编排的状态从哪来

编排要读状态，读状态的方式就是**查询**，不是事件：

- `renderer.hoveredNode()` —— 悬停节点（没有"悬停事件"，移动时直接查）
- `renderer.hitTest / hitTestEx` —— 坐标 → 节点
- `renderer.getNodes()` / `findNodeByName` —— 节点表（`NodeInfo` 带 `parentId`，
  所以能沿父链把"点到了文字"冒泡回"真正的按钮"）
- `renderer.drawOpacity / drawOffsetX` —— 当前动画值（自检模式用它打印）

`appshow` 的底部状态栏就是"被动查询的读数"：页号、节点总数、悬停节点的 id/类型/文本。
页面里的「查询悬停节点」按钮把命中节点的 id/类型/矩形/父节点/文本写进弹窗——整条链路
（命中 → 查询 → 渲染）都不依赖任何事件。

## 6. 自检：没有窗口抓图也能验编排

```bash
BINUI_APPSHOW_AUTO=1 build-mingw/bin/examples/appshow.exe
```

按脚本走一遍切页/弹窗/交错，并打印每步的**实际不透明度**：

```
[demo] ->page1    page0=1.00 page1=0.00 rows=0.00 0.00 0.00 0.00 0.00 0.00
[demo] fade       page0=0.17 page1=0.29 rows=0.00 0.00 0.00 0.00 0.00 0.00
[demo] mid        page0=0.00 page1=1.00 rows=0.94 0.78 0.52 0.16 0.00 0.00   ← 45ms 交错
[demo] dlg-in     veil=1.00
```

这是最便宜的回归方式：编排逻辑错位会立刻显示成不可能的不透明度组合。

## 7. 缺什么才叫"真正配合"

按性价比排序，前两条是这类需求的通用解：

1. **单节点重播**：`restartAnimation(nodeId)`。现在只有全量的 `restartAnimations()`，
   改一个角标就得让整屏动画重来。
2. **动画完成事件**：`onAnimationEnd(nodeId, name)`。有了它，宿主不用猜时长，
   `Fade::start/dur` 那套手工对表可以删掉。
3. **一个节点多条 animation**（逗号列表）：把"滑入 + 淡入 + 缩放"拆开调。
4. **交错原语**：编译期 `--i` 变量或运行时 `setStagger(base, step)`，
   省掉"每行一条 tween"的样板。
5. ~~`setStyleFloat` 的 `ANIM_TRANSLATE` 只能让 x==y~~ → 0.12.3 已加 `ANIM_TRANSLATE_X/Y`
   两个独立通道（过渡与运行时驱动都支持），"只上下滑 26px"现在成立。

## 8. 约定（建议写进团队规范）

- 组件只写"自己被操作时"的动画，命名带组件前缀；
- 组件**不写进出场**，进出场只由容器/宿主决定 → 组件才能被任意容器复用；
- "多个元素对同一事件反应"不让子组件各自监听，**父容器统一切 class / 统一起 tween**；
- 跨页面、跨窗口的时序**一律宿主记账**，别在 CSS 里堆超长 delay 假装编排；
- 页面切换期间要么屏蔽点击、要么让新旧两页的命中区不重叠（`appshow` 在模态期只放行
  弹窗内与遮罩的点击）。
