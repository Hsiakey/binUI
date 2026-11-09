# 键盘焦点系统

binUI 的键盘输入**只有一条入口**：`renderer.handleKey(vk, mods)`。宿主把 Win32 的
`WM_KEYDOWN`（`VK_*`）原样丢进来，再按返回的 `changed` 节点做局部重绘即可；焦点、
页面滚动、文本框编辑三件事的优先级全在渲染器内部定好，8 个示例、C ABI 走的是同一套。

## 三种状态

| 状态 | 进入方式 | 键盘归谁 |
|---|---|---|
| **焦点控制**（默认） | 启动即此态；文本编辑按 Enter/Esc/Tab 退出后回到此态 | 导航键：WASD / Tab / 方向键 / PgUp·PgDn / Home·End / Enter / 空格 |
| **文本编辑** | 焦点落在 `<input>` 上时按 Enter（鼠标点输入框也直接进入） | 编辑键：退格 / 删除 / 左右 / Home·End / Shift 选区；字母数字归输入法（`WM_CHAR`） |
| **鼠标操作** | 点击 | 只切 `:focus` 样式，**不点亮焦点环** |

### 焦点控制中

| 键 | 行为 |
|---|---|
| `W` / `A` / `S` / `D` | 上 / 左 / 下 / 右移动焦点（同行同列优先，其次加权距离；跳过隐藏/禁用节点） |
| `Tab` / `Shift+Tab` | 按文档顺序在可交互节点间循环（环绕） |
| `↑` `↓` `←` `→` | **滚页面**，不移动焦点。目标 = 离焦点最近的滚动容器；焦点不在任何滚动容器里时，取视口内面积最大的那个 |
| `PageUp` / `PageDown` | 按视口 0.9 倍翻页 |
| `Home` / `End` | 滚到顶 / 到底 |
| `Enter` | 激活焦点：`<input>` → 进入文本编辑；`<video>` → 播放/暂停；其余 → 派发 `CLICK` |
| `空格` | 有焦点时同 Enter；没有焦点时向下翻页（浏览器语义） |
| `Esc` | **不消费**（宿主可以拿它关窗口）。文本编辑中按 Esc 才是"退出编辑" |

方向键的目标容器按这个顺序找（`scrollTargetNode()`）：

1. 从焦点节点沿 `parentId` 往上，第一个 `overflow:auto/scroll`（且不是 `overflow:hidden`
   的只裁剪容器）的祖先；
2. 没有 → 视口中心落入的那个可滚动容器；
3. 还没有 → 视口内面积最大的可滚动容器；
4. 全没有（例如整页固定布局）→ 返回 `-1`，方向键**不被消费**，宿主可以自己用。

只有真有两轴溢出的容器才算目标（`contentHeight/contentWidth` 大于视口），
否则方向键会永远被"消费"掉、宿主收不到。

### 文本编辑中

| 键 | 行为 |
|---|---|
| `←` `→` | 按字符移动光标（`Shift` 扩展选区） |
| `Home` / `End` | 光标到行首 / 行尾 |
| `Backspace` / `Delete` | 删除选区或光标前 / 后一个字符 |
| `Enter` | **确认**：提交值、结束编辑、回到待编辑态（环变回虚线）。单行输入没有"换行"；需要"发送"的宿主在 `INPUT`/`FOCUS` 事件里做，别把 Enter 当两种意思 |
| `Esc` | 退出编辑、回到待编辑态，同样提交值（不丢用户已输入的内容）；组字中先取消候选，见下面"输入法"一节 |
| `Tab` / `Shift+Tab` | 提交并退出编辑，然后跳到下一个 / 上一个控件（浏览器行为） |
| `Ctrl+C/X/V/A` | 不归渲染器：剪贴板 I/O 在平台层（C ABI 内部接好了，C++ 示例没接） |
| 字母、数字、中文 | 走 `WM_CHAR` → 宿主转 `renderer.insertText()`（IME 组字同理） |

**WASD 在编辑态不移动焦点** —— 字母要留给输入。这与"用鼠标点开输入框就能打字"的
习惯一致，也是"回车两段式"存在的意义：先 WASD 选中，回车进编辑，打完再回车出来。

Enter 在**两个状态下是两件事**，这是有意的：待编辑态 Enter = 激活（进编辑 / 点按钮），
编辑态 Enter = 确认（只提交，不再触发外边那个元素的点击）。

### Enter / Esc 语义速查

| 键 | 待编辑（Browsing） | 编辑中（Editing） |
|---|---|---|
| `Enter` | 输入框 → 进入编辑；按钮等 → `CLICK` | 确认：提交并退出编辑 |
| `Esc` | 不消费（宿主可以关窗口） | 退出编辑（提交）→ 回到待编辑 |

## 焦点环：输入元素才有虚线，其余是实线

焦点环不是"有焦点就一条线"，而是把"现在处于什么状态"画出来。每个节点单独判
（`renderer.focusRingOf(nodeId)` → `None` / `Dashed` / `Solid` / `Editing`）：

| 状态 | 环 | 什么时候 |
|---|---|---|
| 待编辑（**只有输入元素**） | **虚线** 1px，颜色 = 节点底色的对比色（深底浅线 / 浅底深线） | 键盘选中输入框、还没按 Enter —— 提示"再按一下 Enter 才能打字" |
| 普通焦点（按钮等其它元素） | **实线** 1px，对比色 | 键盘选中按钮 / 开关 / 链接 / 视频… |
| 编辑中（输入元素） | **实线** 2px，强调蓝 `kFocusRingEditColor`（#3E8BFF） | 键盘 Enter 进入的编辑（有光标），常亮；**鼠标点进来的编辑不画环** |
| 无 | 不画 | 鼠标操作 / 2 秒无操作 / 窗口失焦 / 没有焦点 |

**虚线只给输入元素当状态标记**，不是"焦点样式"：按钮、卡片上的可点元素被键盘选中时是
普通实线环，否则满屏虚线焦点看起来像"所有东西都在等你编辑"，和禁用/占位之类的虚线语义
也容易混。线型 + 颜色双编码同时对色弱用户友好。

- 虚线沿圆角矩形路径按弧长采样（`binui_paint.h` 的 `BuildDashedRoundRing`），三个后端
  共用同一份几何、各自 `fillLine`。**宽输入框（flex 拉满窗口）会按周长自动拉长虚线**，
  不会因为段数上限把环画成半圈；编辑环 2px 由 `strokeRounded` 的多层同心环叠出来。
- **编辑环只给键盘**：鼠标点进输入框会直接进入编辑（浏览器行为，有光标、能打字、`:focus` 生效），
  但**不画那圈蓝环**；只有从焦点控制态按 Enter 进来的编辑才画（`focus(id, viaKeyboard=true)`）。
  退出编辑时同理：键盘进来的回到虚线/实线待编辑环，鼠标点进来的退出后不画环。
  这也让"焦点框"始终只代表"现在键盘在操作这里"，鼠标用户看光标就够了。
- 虚线和普通实线（待编辑 / 普通焦点）都是 `:focus-visible` 语义：键盘导航才亮；**鼠标点击不亮**
  （`handleKey` / `setKeyboardFocus` 会亮，`release` 不会）。环正亮着时鼠标点控件会立刻收掉。
- 这两态 **2 秒没有操作自动消失**（`kFocusRingIdleSec`，`setFocusRingIdleTime()` 可改）：
  每按一次导航键都会重置计时；消失不需要宿主定时 —— `stepAnimations()` 会在熄灭的那一帧
  把焦点节点放进返回值，宿主照常 `invalidateNode()` 就擦掉。
- **编辑环常亮**：编辑是持续状态，不能 2 秒自己消失（那时用户在打字，环消失会像"焦点丢了"）。
  退出编辑（Enter/Esc/Tab）后，键盘进来的那次回到虚线的待编辑态。
- 窗口失焦时宿主调 `renderer.hideFocusRing()`（编辑中还会 `blur()`），别把环留在别人的窗口上。

`setKeyboardFocus(-1)` / 窗口失焦 / 按 Esc 关窗都会清掉焦点；焦点节点被隐藏、禁用后
`stepAnimations` 不会重报，宿主按自己的失效逻辑重绘即可。

## 中文输入法与 WASD（必须接的一行）

中文输入法**默认挂在窗口上**：按 `d` 时 Windows 会把 `WM_KEYDOWN` 的 `wParam` 改成
`VK_PROCESSKEY`（229），字母交给输入法组字。这样 WASD 焦点导航一个键都收不到 ——
实测窗口日志：

```
keydown vk=229 composing=0     ← 按 d，输入法把键吃了
keydown vk=229 composing=1
```

所以外壳提供 `setImeActiveFn`：非编辑态把 IME 上下文从窗口上摘下来（字母原样到达），
进了输入框再挂回去（组字/候选框/光标定位照旧）：

```cpp
app.setImeActiveFn([&] { return renderer.isEditing(); });
```

摘掉/挂回发生在每帧的 `renderFrame()` 里（`syncImeContext()`），组件不可见的开关
不需要宿主自己判断；摘之前会 `cancelImeComposition()`，不会把半截拼音留成结果串。
不接这一行 = 保持老行为（IME 常挂），此时中文输入法下 WASD 无效，方向键照常。

**组字中的 Esc 归输入法，不归焦点系统。** 实测微软拼音：组字时按 Esc → IME 先发
`WM_IME_ENDCOMPOSITION` 取消候选，然后不再把 Esc 放行，编辑态原样保留（日志里能看到
`msg=WM_IME_ENDCOMPOSITION` 后面没有 `keydown vk=27`）。有的输入法会把 Esc 放行，
外壳为此兜了一道：组字结束 200ms 内到达的那个 Esc 被窗口吞掉一次（`composeEndMs_`），
再按一次才退出编辑。所以"Esc 取消拼音"和"Esc 退出编辑"不会互相误伤（`BINUI_IME_LOG=1`
时日志里有 `esc delta=…` 可查）。

## 宿主接法

```cpp
app.setKeyFn([&](int vk) {
    uint32_t mods = 0;
    if (::GetKeyState(VK_SHIFT)   & 0x8000) mods |= BinUIRenderer::KEYMOD_SHIFT;
    if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= BinUIRenderer::KEYMOD_CTRL;
    if (::GetKeyState(VK_MENU)    & 0x8000) mods |= BinUIRenderer::KEYMOD_ALT;
    const auto kr = renderer.handleKey(vk, mods);
    for (uint16_t id : kr.changed) invalidateNode(id);
    if (!kr.handled && vk == VK_ESCAPE) app.requestExit();   // 库不消费 Esc，宿主决定
});
app.setFocusFn([&](bool focused) {
    if (!focused) {
        renderer.cancelPress(); renderer.clearHover(); renderer.blur();
        renderer.hideFocusRing();
        app.invalidateAll();
    }
});
// 非编辑态摘掉输入法，否则中文输入法会把 WASD 吃成拼音（见上一节）
app.setImeActiveFn([&] { return renderer.isEditing(); });
```

`KeyResult::handled` 只表示"库消费了这个键"；宿主自己的全局快捷键应当在
`handled == false` 时再处理，否则会和编辑/导航抢键。

## 已知取舍

- 单击输入框直接进入编辑（浏览器行为）；"回车两段式"只约束键盘导航，鼠标用户不用多按一次。
  鼠标进入的编辑不画焦点环（见上一节），所以"点了输入框却没有框"是预期行为，不是丢了焦点。
- `:focus` 与焦点环是两件事：`release()` 会把点击的节点设为键盘焦点（`:focus` 样式因此生效），
  但环是 focus-visible，鼠标不亮。
- 方向键滚动是"最近滚动容器"而不是"整页滚动"：binUI 布局常常是一整屏卡片，把滚动交给
  焦点所在的卡片更符合预期；整页滚动的布局（根容器 `overflow:auto`）同样成立。
- 没有 `tabindex` / `focus-within`：可聚焦集合 = `BINUI_INTERACTIVE` 且可见、未禁用的节点。
- IME 开关只有 GL/GDI 外壳有（`BinUIAppGLGdi::setImeActiveFn`）：D3D11 外壳本来就没有
  IME 处理（它那边 WASD 不会被输入法吃，但中文输入也不可用）；旧演示外壳 `BinUIApp`
  两者都没有，新代码别再用它。

## 未来开发计划：层级焦点导航（下潜 / 上浮，**未实现**）

目标：非输入框按 Enter 可以"进到子元素里"继续用 WASD 选，多层可下潜；`Delete` 回退
上一层级。用来解决"一个卡片里有 20 个按钮，WASD 在整页里跳来跳去"的问题。

名字叫"层级焦点导航"（不是 Z 序）：移动的是**焦点在元素层级树里的深度**，和渲染顺序 / 窗口
叠放次序无关。当前只有一维焦点列表（按文档顺序），这一版把它变成"焦点 + 层级"两维。

拟定语义：

| 操作 | 行为 |
|---|---|
| `Enter`（非输入框、且有可聚焦子节点） | 焦点下潜到该节点的**第一个子焦点**（按文档顺序）；焦点作用域收窄为这个节点 |
| `Enter`（没有可聚焦子节点） | 维持现状：派发 `CLICK` |
| `WASD` / `Tab` | 只在当前作用域内移动；作用域外节点不可达 |
| `Delete` | 上浮一层；已在顶层则清焦点 |
| `Esc` | 直接回到顶层（不清焦点）；文本编辑中仍是退出编辑 |
| 焦点环 | 作用域边框 + 当前焦点环两级指示（外层细框、内层实心环），或只画内层 + 状态栏提示 |

实现要点（记下来，做的时候照这个走）：

1. 渲染器加 `std::vector<uint16_t> focusScopes_`（根在前）和 `focusDepth()`；
   `moveKeyboardFocus` / `cycleKeyboardFocus` 的候选集合先过滤 `isAncestorOf(scope, node)`。
2. `Enter` 的下潜判定要排在 INPUT/VIDEO 分支之后：输入框仍是"进入编辑"，不再是下潜。
3. `Delete` 与文本编辑的"删除后一个字符"冲突 —— 只在**非编辑态**且 `focusDepth() > 0`
   时才是上浮；编辑态保持删除字符。
4. 下潜目标节点自己要不要成为焦点？建议不：作用域节点只作为容器高亮，焦点落在它的
   第一个可聚焦子节点；没有子节点就点击它（`CLICK`），语义和现在的 Enter 一致。
5. 作用域节点被隐藏/禁用时，`focusScopes_` 自动回退到最近的可见祖先（和 `setVisible`
   一样在每帧检查，代价很小：只在焦点链变化时查）。
6. 与滚动的关系：方向键仍滚"焦点最近的滚动容器"，作用域不改变这条。
7. 对外 API：`focusDepth()`、`focusScope()`、`descendFocus()` / `ascendFocus()`（给
   手柄/自定义键位用），`handleKey` 只是它们的默认绑定。
8. 兼容：现有 `moveKeyboardFocus` 不加作用域参数时行为不变（作用域 = 根），
   这样 C ABI 和旧宿主不用改。

没实现的原因：当前 8 个示例的布局层级都很浅，WASD 已经够用；等真正出现"卡片里几十个
可聚焦元素"的界面再做，避免过早给焦点链加状态（多一层状态就多一处失效路径）。
