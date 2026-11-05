> **状态：已修复：编译器增加一趟预处理，按级联后的 style 标出 display:none 的整棵子树并整体跳过。**

# `display:none` 只跳过元素本身，子树被重挂到最近可见祖先

> 建议标签：`bug` · 面积：编译器（HTML/CSS 解析）
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 平台 / 后端：无关（编译器问题，GL 与 GDI 表现一致）
> 编译警告：无（**这是最坏的一点：没有任何提示**）

## 现象

`README.md` 的样式章节承诺「`display:none` 经 class 生效（整棵子树不生成节点）」，
实际行为是：**元素本身不生成节点，但它的子元素照样生成，并被挂到最近的可见祖先上**。

后果是子元素的坐标为按"新父级"重新解析 —— 绝对定位的子元素会跑到左上角/错位，
而且没有任何编译警告。这在"页面切换 / 面板隐藏"这种用法上会直接穿帮。

## 最小复现

`assets/display-none2.html`，渲染结果 `assets/display-none2-gl.png`：

```html
<style>
  .hidden { display: none; }
  .card { background: #2C2C2E; border-radius: 8; }
</style>

<!-- A：内联 display:none，子块应一起隐藏 -->
<div class="card" style="position:absolute; left:600; top:200; width:120; height:80; display:none">
  <div style="position:absolute; left:20;  top:20; width:60; height:30; background:#F25A5A"></div>
  <span style="position:absolute; left:20; top:56; width:90; height:16; color:#F25A5A">A 子块</span>
</div>

<!-- B：经 class 生效，子块应一起隐藏 -->
<div class="card hidden" style="position:absolute; left:640; top:200; width:120; height:80">
  <div style="position:absolute; left:200; top:20; width:60; height:30; background:#F59E0B"></div>
  <span style="position:absolute; left:200; top:56; width:90; height:16; color:#F59E0B">B 子块</span>
</div>

<!-- C：对照组 -->
<div class="card" style="position:absolute; left:160; top:200; width:300; height:80">
  <div style="position:absolute; left:20; top:20; width:60; height:30; background:#4ED17E"></div>
</div>
```

期望：只有 C 出现。
实际：A、B 的**容器都不见了，但两个子块都出现了**，而且跑到画布左上角附近
（A 的红块 + 文字在 `(20,20)`、B 的橙块 + 文字在 `(200,20)` —— 就是它们相对容器
的原始坐标，被当成相对根节点解析了）。截图里上排那两个色块就是这么来的。

GDI 与 GL 结果一致，因为这一段是编译器干的。

## 根因线索

`src/compiler/binui_compiler.cpp:4010`：

```cpp
std::string display = pick(e.attrs, style, "display", "display", "");
if (display == "none") continue;      // ← 只 skip 当前元素
```

而子元素的父级是**运行时往上找最近的已生成祖先**（`:3659-3665`）：

```cpp
std::vector<uint32_t> hIdxToNodeId(elems.size(), 0xFFFF);
auto resolveParentId = [&](size_t idx) -> uint32_t {
    while (idx != SIZE_MAX && idx < elems.size()) {
        if (hIdxToNodeId[idx] != 0xFFFF) return hIdxToNodeId[idx];   // 跳过没生成的
        idx = elems[idx].parentIdx;
    }
    return 0xFFFF;
};
```

`display:none` 的容器正好"没生成"，`resolveParentId` 于是继续往上跳到 `<body>`/根，
子元素带着自己的绝对坐标挂进去 → 位置错。

顺带一提，flex 那一趟（`:3689`）对 `display:none` 的子项处理是对的（会 `continue`），
只有"元素本身跳过后没有连带子树"这件事没做。

## 建议修法

在元素循环之前加一趟预处理，把 `display:none` 的**整棵子树**标记出来：

```cpp
// elems 已按深度优先序排好，父索引恒小于子索引
std::vector<char> hidden(elems.size(), 0);
for (size_t i = 0; i < elems.size(); i++) {
    // 注意：display 可能来自 class 规则，必须用级联后的 style，而不是只看内联
    auto st = styleOf(elems[i], i);
    std::string d = lowerStr(pick(elems[i].attrs, st, "display", "display", ""));
    if (d == "none") hidden[i] = 1;
    else if (elems[i].parentIdx < elems.size() && hidden[elems[i].parentIdx]) hidden[i] = 1;
}
```

然后主循环里 `if (hidden[ei]) continue;` 代替现在的单点判断。

两个配套点：

- `:2863` 附近注释说「`display:none` 的元素仍被计数」是 `:nth-child` 的口径差异，
  与这条不冲突，别一起改。
- 修完加一条测试：隐藏容器里的绝对定位子节点，坐标不得出现在根坐标系里。
  （`tests/` 里现有断言风格就够用。）
