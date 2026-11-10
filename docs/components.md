# 组件化：`.bui` / `.bum` / `.bum.html`

> 一句话：**一个界面 = 一个 `.bui`（单文件，组件全内联）；想拆开就拆成 `.bui` + 若干 `.bum`，
> 二进制和源码可以混放，加载时二进制优先、源码用于改和热更新。**

![组件化三种形态](img/features.png)

示例：[`examples/features/`](../examples/features/)（上图就是它渲染出来的）。
标题栏 `titlebar.bum`（二进制）、按钮 `button.bum.html`（源码 + 参数）、卡片 `card.bum`（二进制，
内部又引用过源码组件）、状态栏 `dock.bum`（`external`，运行期挂载）。

## 标准组件库（`components/`）

`examples/` 里的组件是"组件机制"的演示；**通用组件**放在仓库根的 `components/`，
不含任何 C++ 代码，用 `<use src="…/xxx.bum.html" .../>` 编译期内联：

| 组件 | 根标签 | 参数 |
|---|---|---|
| `titlebar` | `<titlebar>` | `title` `titlew`(默认 `content`=按文字宽度) `sub` `icon` `iconw` `h` `pad` `padr` `bs` `gap` `idmin` `idmax` `idclose`（左：图标/标题/副标题；右：最小化— 最大化▢ 关闭✕，三颗图标都是元素画的 10×10 图形） |
| `button` | `<button>` | `label` `kind`(primary/secondary/ghost/danger) `w` `h` `fs` `r` |
| `field` | `<input>` | `ph` `val` `w` `h` `fs` `pad` |
| `badge` | `<span>` | `text` `kind`(blue/mint/amber/red/gray) `w` |
| `toggle` | `<button>` | `label` `w` `h` |
| `panel` | `<div>` | `title` `pad` `w` `h`（宿主子元素成为面板内容） |
| `dialogue` | `<div>` | `name` `text` `ok` `cancel` `w` `h` |
| `stat` | `<div>` | `k` `v` `w` `h` |
| `avatar` | `<img>` | `src` `size` |
| `divider` | `<div>` | `len` `color` |
| `tag` | `<span>` | `text` `kind`(blue/mint/amber/gray) `w` `fs` |
| `statusbar` | `<div>` | `text` `b1` `b2` `h` `bw` |

三条约定（细节见 [`components/README.md`](../components/README.md)）：

1. **参数别写进组件的 `<style>`** —— 组件样式表会并入宿主且是全局的，多实例互相覆盖；
   可调值一律走内联 `style`。
2. **组件根标签决定节点语义** —— `<use>` 会把根标签搬过来：根写 `<button>` 才是真按钮，
   根写 `<input>` 才有占位符/编辑，根写 `<titlebar>` 才能拖动窗口。以前替换节点一律当
   `div`，组件里的"按钮"其实不可点击。
3. **宿主子元素落进组件根容器**（流式末尾），`panel` 靠这条支持"带内容的卡片"。
4. **一个组件只该有一个根元素** —— 写了多个顶层元素也能用（会被一个合成的 `<div>` 包起来，
   与二进制组件的链接器口径一致），但编译期会告警：那层容器不是你写的，`class` 也没处给，
   样式只能落到 `<use>` 属性上。宁可自己写根容器，看得见才好调。
5. **没有 `<include>`** —— 组件内联一律是 `<use src="…">`（标签名别猜：`<include>` 会按未知标签
   告警，什么都不做）。`<use>` 是**实例化**：搬根标签、能传参、二进制组件走链接器/运行期挂载；
   纯文本拼接式的"包含"不提供，因为 binUI 的每个元素都是真实布局盒，拼接和实例化混在一起
   就没法保证根语义。


## 一、命名与分发形态

| 文件 | 含义 | 谁读 |
|---|---|---|
| `main.bui` | 完整界面布局（**应用入口**，`loadLayout` 直接吃） | 外壳 |
| `main.bui.xml` | 上面那份的源码（也接受 `main.html`） | 编译器 |
| `titlebar.bum` | 组件二进制布局 | 链接器（编译期内联）/ 渲染器（运行期挂载） |
| `titlebar.bum.html` | 组件源码（可读、可编辑、可热重载） | 编译器 |
| `.binui` | `.bui` 的旧名，仍然识别（兼容历史示例） | 同 `.bui` |

三种分发方式都成立，作者按需选：

```
单个 UI（内嵌，一个文件）        main.bui
组件式（外挂）                  main.bui + titlebar.bum + button.bum
混放（部分二进制、部分源码）     main.bui + titlebar.bum + button.bum.html
双份（二进制优先，源码可改）     main.bui + button.bum + button.bum.html
```

“组件全内联”的 `main.bui` 运行时**不需要任何外部文件**（图片仍在外面，路径记在布局自带的资源段里）。

## 二、三条组装路线

| 写法 | 组装时机 | 用什么 | 特点 |
|---|---|---|---|
| `<use src="x.bum.html">` | 编译期 | 源码内联（文本替换 + 元素合并） | 支持 `{{参数}}`；宿主能主题化组件 |
| `<use src="x.bum">` | 编译期 | 链接器离线合并 | 不暴露源码；参数在编 `.bum` 时已固化 |
| `<use src="x.bum" external>` | **运行期** | `binui::ex::MountAll()` | 换组件不用重编主 `.bui`（皮肤/插件） |

前两条产出同一个 `.bui`，运行时零开销、零依赖；第三条在布局里只留一个占位节点 +
一条“组件引用”记录（v12 组件引用段），外壳启动时挂上去。

## 三、基本用法

```html
<!-- 宿主 main.html（编译成 main.bui） -->
<use src="titlebar.bum"    x="0" y="0" w="720"/>                        <!-- 二进制，编译期内联 -->
<use src="button.bum.html"  x="16" y="44" w="132" label="确定"/>          <!-- 源码，可传参 -->
<use src="card.bum"        x="16" y="96" w="304"/>                      <!-- 二进制，编译期内联 -->
<use src="dock.bum" external x="0" y="412" w="720"/>                    <!-- 外挂，运行期挂载 -->
```

```html
<!-- button.bum.html：参数 + 自己的 :hover 过渡 -->
<style>
    .cmp-btn { color:#F5F5F7; border-radius:10; transition: background-color .18s ease; }
    .cmp-btn:hover { background:#3B2F63; }
</style>
<!-- 组件根就是按钮本身：位置/宽度交给宿主 <use x=… y=… w=…>，参数只写内联 style -->
<button class="cmp-btn"
        style="position:absolute; width:{{w=132}}; height:40;
               background:{{bg=#23232E}}">{{label=按钮}}</button>
```

> 别为了"有个容器"再套一层 `<div>`：那层 div 与按钮完全重合，只会多一个定位基准与祖先
> （宿主传给组件的 `bg` 之类属性还会落到它身上，画出个方块角），没有实际作用。

```cpp
// 外挂组件：布局里只有占位节点，这一步把它们挂上（幂等，可重复调用）
renderer.loadLayout(bytes.data(), bytes.size());
binui::ex::MountAll(renderer, binui::ex::DirOf(uiPath), renderer.layoutBytes());
```

`MountAll` 会按第二节的规则解析每个引用：`.bum` 直接读、只有 `.bum.html` 时就地编译，
并打印挂载统计。`--watch` 热重载后重新调用一次即可（示例就是这么做的）。

## 四、加载优先级（二进制优先，源码优先更新）

规则只在一个地方实现：[`include/binui_asset.h`](../include/binui_asset.h)，
编译器与运行期加载器共用同一份。

| 情况 | 用哪个 | 说明 |
|---|---|---|
| 只有 `.bum` | 二进制 | 直接合并 |
| 只有 `.bum.html` | 源码 | 编译期内联（可传参） |
| 两者都在，`.bum` 不旧 | **二进制** | “优先加载二进制的” |
| 两者都在，源码 mtime 更新 | **源码** + 告警 | 防止“改了源码没重编 `.bum` → 改了没反应” |
| 写 `src="x"`（无扩展名） | 依次探测 | `x.bum` → `x.bum.html` → `x.bum.xml`（旧写法）→ `x.bui.html` → `x.html` → `x.xml` |

扩展名只作提示，最终按文件头 magic（`BINU` / `XHTM`）判定——把 `.bum` 改名成 `.xml`
也不会误把二进制当源码解析。**组件源码的推荐写法是 `.bum.html`**（和主 UI 的 `main.html`
同一套语法、同一个元素解析器）；`.bum.xml` 是历史写法，能力完全一样、继续识别。

> **想绝对走二进制（发行版）**：只发 `.bum`/`.bui` 就行。源码候选不存在时，mtime 比较
> 根本不会发生（`ProbeAsset` 里 `src2` 为空直接返回二进制）；上面那条"源码更新则用源码"
> 只对**开发期两样都在**生效，为的是避免"改了源码忘了重编 → 改了没反应"。

实测六种组合（`ProbeAsset` 直接返回的结果）：

| 目录里有什么 | `src="c.bum"` 选中 | 说明 |
|---|---|---|
| 只有 `a.bum` | 二进制 | 逐字节直接用 |
| 只有 `b.bum.html` | 源码 | 编译期内联 |
| `c.bum` + `c.bum.html`（源码更旧） | **二进制** | 默认路径 |
| `d.bum` + `d.bum.html`（源码更新） | 源码 + 告警 | 开发期热重载 |
| 只有 `e.bum.html`，写 `src="e"` | 源码 | 无扩展名自动探测 |
| 都没有 | 失败（编译期告警 / 运行期挂不上） | — |

## 五、参数

- `{{name}}`：用 `<use>` 上的同名属性替换；未提供 → 空串 + **编译警告**。
- `{{name=默认值}}`：可缺省，未提供时用默认值，不报警告。
- **单独编 `.bum` 时按默认值代入**（裸 `{{name}}` 原样保留）——所以带默认值的组件
  既能被内联传参，也能自己编成 `.bum`，两种形态语义一致。
- **参数值按纯文本注入**（`& < >` 转义）：传 `"内部 <use>"` 只是一段文字，不会变成真元素。
- 参数名大小写不敏感（HTML 属性名本来就小写化）。

二进制组件的参数在编 `.bum` 时就固化了；宿主还能覆盖的是**外框**：`<use>` 上的
`w` / `h` / `bg` / `radius`（链接器改的是组件根节点）。要可变的文字/配色，
要么用源码形式，要么给每种配置编一份 `.bum`。

## 六、链接器做了什么（内联与外挂共用）

[`include/binui_link.h`](../include/binui_link.h) 的 `MergeLayout()` 是唯一实现，
`binui_compile`（离线）和 `renderer.mountComponent()`（在线）都调它：

| 项 | 处理 |
|---|---|
| 节点 id | 重新编号（宿主最大 id + 1 起），整棵挂到锚点节点下 |
| 组件根 | 组件里那个“合成根”（`html/body` 包裹层）丢弃，其子节点改挂锚点 |
| 坐标 | 组件坐标从 (0,0) 起算，整体平移到锚点内容原点（节点坐标是绝对屏幕坐标） |
| 占位节点 | 按组件包围盒撑大——外壳会按祖先矩形裁剪子节点，不撑大就会被裁掉一块 |
| 字符串表 | 整体后移，重算节点文本 / 视频 src / 图片路径 / 名称的偏移 |
| 缩放规格段 | 合并（`%`/flex/auto 在宿主窗宽下重排照旧） |
| 状态样式段 | 合并（组件的 `:hover`/`:active` 在宿主里照常生效） |
| 动画段 | 关键帧组整体后移，动画定义的组下标跟着补 |
| 视频/资源段 | 视频描述合并；资源下标整体重排（`imageIndex` 一起改） |
| 扩展绘制段（v13/v14） | 合并（组件的文字色/边框/渐变/字重/行高/装饰线，以及 v14 的斜体/占位符/只读/勾选标记跟着搬过来，节点 id 重映射） |
| 组件引用段 | 递归合并（组件里还能外挂组件） |
| 名称段 | 合并；同名以宿主为准（组件不能顶掉宿主的 `#id`） |
| 窗口描述段 | 不动（宿主保留自己的标题/尺寸/背景） |

失败时宿主保持不变并给出原因（`err`），不会留下半个组件。

## 七、命令行

```bash
# 组件源码 -> 二进制组件
binui_compile titlebar.bum.html                 # -> titlebar.bum（同目录）
binui_compile main.html -o build/main.bui      # 单文件界面

# 已经是二进制就原样拷贝，方便两条流水线共用一条命令
binui_compile titlebar.bum -o dist/titlebar.bum

# 解包：把 .bui 还原成打包前的目录结构（依赖编译时生成的 .bui.srcmap）
binui_export build/main.bui -o src_restored/   # 逐字节还原，不是节点反推
binui_export build/main.bui --list             # 只看包里有哪些文件
```

构建集成：`make components` 把 `examples/features/*.bum.html` 编成
`build-mingw/assets/components/*.bum`；CMake 里是 `components_bums` 目标
（`cmake --build . --target components_bums`）。产物不落源码树。

## 八、打包清单与解包（`.bui.srcmap`）

**内嵌的包也能还原打包前的目录结构** —— 靠编译时在旁边写的一份打包清单。

```bash
binui_compile src/main.bui.html -o out/binui/main.bui
#   -> out/binui/main.bui
#   -> out/binui/main.bui.srcmap     ← 打包清单

binui_export out/binui/main.bui -o restore/
#   restore/
#   ├── examples/features/main.html          ← 和源文件逐字节一致
#   ├── examples/features/button.bum.html
#   └── components/titlebar.bum.html
```

清单里存的是**每个文件的原始字节** + **相对 appRootDir 的路径**（不是编译机的绝对路径，
所以整包搬到哪都能还原）。记录的是"打包时用到的那些文件"：主 UI、每个被内联的组件源码、
每个被链接/外挂的 `.bum`。

三条性质：

1. **逐字节还原**：存的就是原文件内容，不是"节点树反推 HTML"那种有损转换
   （那种会把 CSS 级联、flex、`{{参数}}` 全拍平成绝对值）。
2. **解包出来的树能再编一次，产出同一个字节的 `.bui`**（实测：SHA256 相同）。
3. **不想给就不给**：`.srcmap` 是独立文件，不随 `.bui` 走。发行时别带它、或者编译时
   `--no-srcmap` 不生成，就没有任何源码泄露；主 `.bui` 一个字节都不变。

> 为什么不把清单塞进 `.bui`：那会引入新的格式段、升 `BINUI_VERSION`，还会让包里**一定**
> 带着源码（没得选）。旁挂文件是"要不要暴露"的开关，而布局格式和加载性能完全不动。

```cpp
// 程序化用法：编译时打开开关，自己决定清单写到哪
binui::CompileOptions opt;
opt.emitSrcMap = true;                                   // 默认 false
binui::CompileResult res = binui::CompileBinUIFileEx("main.bui.html", opt);
if (!res.srcMap.empty()) /* 写 main.bui.srcmap */;

// 读回来（header-only）
std::vector<binui::SrcMapEntry> entries;
binui::ParseSrcMap(res.srcMap.data(), res.srcMap.size(), entries);
```

## 九、两个必须知道的坑

1. **组件自带的 `<style>` 是全局的**（并进宿主样式表）：好处是宿主能主题化组件，
   坏处是选择器互相影响——组件内的类名请加前缀（示例统一 `.cmp-`）。
2. **参数化的样式不要写在 `<style>` 里**：样式表全局共享，多个实例会互相覆盖；
   参数化的样式写内联 `style`。

```html
<!-- ❌ 所有实例共用最后一个 bg -->
<style>.cmp-btn { background: {{bg}}; }</style>

<!-- ✅ 每个实例独立 -->
<button class="cmp-btn" style="background: {{bg=#23232E}}">…</button>
```

组件里的 `:hover` / `:active` / `:focus`、`@keyframes`、`::before/::after` 都照常工作。

## 十、资源路径

组件里的相对资源路径（`<img src>`、`<video src>`、嵌套组件的 `src`）在合并时
**改写成宿主相对路径**：

```
components/titlebar.bum.html 里写 <img src="icon.png">
  → 合并进 examples/features/main.html 后是 src="titlebar/icon.png"
```

图片清单还会写进布局的**资源段**（v12），所以外壳不必再额外保存编译期的图片列表：

```cpp
auto imgs = binui::ex::LoadLayoutImages(renderer, uiPath, "components"); // 按布局资源段找文件
renderer.setImageOrder(renderer.imagePaths());
renderer.loadResources(imgs);
```

## 十一、目录结构

两条原则就够了：

1. **`src` 相对"写出这个 `<use>` 的文件"解析** —— 同级引用最省事，嵌套组件再相对组件自己。
2. **相对路径最终按宿主 UI 文件解析** —— 编译期是源码文件，运行期是 `.bui`。所以组件别挪到别处。

### 几个组件：全放一个目录

```
myapp/
├── main.bui.html       # 主结构源码（叫 main.html 也行，都认）
├── main.bui            # 构建产物：binui_compile main.bui.html
├── button.bum.html     # 组件源码：命名与产物对称（xxx.bum ↔ xxx.bum.html）
├── button.bum          # 可选：编好的组件二进制（有就优先）
├── panel.bum.html
└── icon.png            # 图片也搁旁边
```

```html
<!-- main.bui.html -->
<use src="button.bum.html" x="16" y="20" w="160" label="确定"/>
<use src="button.bum"      x="16" y="70" w="160" label="裸名字也能找到"/>  <!-- 自动探测 -->
<use src="dock.bum.html" external x="16" y="120" w="200"/>                <!-- 运行期挂载 -->
```

### 几十上百个：`binui/` 树 + `components/<分类>/`

成品 App 的组件化会拆出很多小零件，一屏放不下。这时候按下面收进一棵 `binui/` 树：

```
appRootDir/
└── binui/
    ├── main.bui                      # 或 main.bui.html（源码）
    ├── titlebar.bum                  # 或 titlebar.bum.html
    └── components/
        ├── button.bum(.html)
        ├── Button/                   # 分类目录：组件 + 它自己的资源放一起
        │   ├── primary.bum.html
        │   └── star.png
        └── Label/
            └── tag.bum.html
```

```html
<!-- binui/main.bui.html -->
<use src="titlebar.bum.html" x="0" y="0" w="960"/>
<use src="components/Button/primary.bum.html" x="20" y="60" w="140"/>
<use src="components/Label/tag.bum.html"      x="20" y="130" w="80"/>
```

```html
<!-- binui/components/Button/primary.bum.html：组件自己的资源与嵌套都相对组件目录 -->
<div style="width:{{w=120}}; height:40; background:#3E8BFF">
    <img src="../Icon/star.png" style="position:absolute; left:8; top:12; width:16; height:16"/>
    <use src="../Label/tag.bum.html" x="32" y="10" w="80" text="内嵌标签"/>
</div>
```

实测这套的编译结果：`736 字节，资源 1 项`，资源段里记的是 **`components/Icon/star.png`**
（组件里的 `../Icon/star.png` 被改写成宿主相对路径），嵌套 `../Label/tag.bum.html` 正常展开。

**为什么要给组件单独一个文件夹**：带资源的组件（图标、字体、贴图）把自己的文件放在自己目录里，
搬运/删除/替换都是整块操作，不会散落在 `components/` 根上。纯样式组件（`button`、`divider`）
放平铺的 `components/x.bum.html` 就够了。

### 内嵌 vs 外挂（发行建议）

| | 内嵌（默认，`<use src>` 不写 `external`） | 外挂（`<use … external>`） |
|---|---|---|
| 运行期开销 | **零**：组件已经并进 `.bui`，一个文件都不读 | 每个组件一次读文件 + 合并 |
| 启动 | 一次 `loadLayout` | `loadLayout` + `MountAll` 逐个挂 |
| 换组件 | 要重编主 `.bui` | 直接替换组件文件 |
| 适用 | **发行版**（默认推荐） | 皮肤 / 插件 / 按需加载的大模块 |

所以"发行优先内嵌"是对的：简单、快、零依赖。组件化换来的是**可替换性**，不是性能 ——
只有当组件多到"一次性全加载太重"或者需要运行期换皮肤时，外挂才划算。

### 分发随你

| 你想 | 就给 | 说明 |
|---|---|---|
| 保护源码 | 只给 `button.bum` | 二进制，发行版该给这个 |
| 让人能改 | 只给 `button.bum.html` | 运行期就地编译（首次启动慢一点） |
| 两样都给 | 都放 | **二进制优先**；源码 mtime 更新时改用源码并告警（改完保存就见效） |

### 唯一要守的：别把组件挪到别处

相对路径最终按**宿主 UI 文件**解析：编译期是 `main.bui.html`（或 `main.html`），运行期是
`main.bui`（`ex::MountAll(renderer, DirOf(uiPath), …)`）。同级放 = 两边都成立，什么都不用管。
真要拆目录，就保持镜像：

```
A  main.bui 放 bin/，组件留在 assets/            mounted=0 failed=1
   [binui] 外挂组件缺失：components/dock.bum.html（锚点 #1）
B  组件按原相对位置放进 bin/components/          mounted=1 failed=0
```

## 十二、已知限制

| 限制 | 现状 |
|---|---|
| 二进制组件的参数 | 编 `.bum` 时固化；宿主只能覆盖 `w/h/bg/radius` |
| `slot` / children 投影 | 未实现（组件里不能“把宿主的孩子插到这里”） |
| 组件作用域 CSS 隔离 | 未实现（CSS 全局，靠类名前缀约定） |
| 组件内 `<script>` | 忽略（没有脚本引擎） |
| 运行期卸载组件 | 未实现（挂载后只增不减，`loadLayout` 重来即可） |
| 从**没有** `.srcmap` 的 `.bui` 反推源码 | 做不到：内联把组件边界抹平了，`.bui` 里只剩一棵扁平节点树。想要能解包就编译时保留打包清单（`binui_compile` 默认产出 `.bui.srcmap`），`binui_export` 靠它逐字节还原；没有清单就只能靠外面保存的源码树。 |
