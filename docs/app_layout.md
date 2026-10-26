# 应用布局（散装形态）—— 当前采纳的形态

> **状态：已采纳，先记下不做实现。**
> `.bex` 单文件容器方案已归档为备选（见 [`bex_container.md`](bex_container.md)）。
> 本文只记录"现在按什么形态长"，以及为了将来不返工而必须固定的几条契约。

---

## 1. 决策

**应用 = 原生平台可执行文件（`exe` / ELF / Mach-O）+ 同级散件资源，运行时外部加载。**

不做容器打包。理由三条：

1. **容器解决的是分发，当前痛点是开发循环。** 改一行 UI 就要重编译 C++ 或手工搬文件，这个摩擦比"目录里有几个散文件"重得多。
2. **散装与"UI 即应用"的哲学一致。** 一份 UI 文本 + 一个资源目录就定义一个应用，二进制只是解释器；换 UI 不用碰二进制。
3. **散装是容器的子集。** 整个目录将来可以原样打进 `.bex`（布局 + 资源 + 元数据），所以现在不阻塞将来——容器只是最后一步"把目录压成一个文件"。

**明确记下"不做"的清单**（避免反复讨论）：

- `binui-pack` / `binui-inspect` 打包与检查工具
- `.desktop` / AppStream metainfo 生成、`--install` / `--uninstall`
- 图标多尺寸生成、SVG 栅格化
- `libbinui.so`：默认维持静态链接。**若**将来要给"不能重编译宿主"的第三方内嵌，才做 C ABI 版（`include/binui_c.h`，句柄式，零 STL 跨边界）；直接导出现在的 C++ 头（`std::string`/`std::vector`/`unordered_map` 参数）会掉进 C++ ABI 地狱，不做。
- 镜像级加密/混淆/签名

---

## 2. 目录形态（两种，等价）

**A. 平铺**（小应用，资源少）

```
myapp.exe            # 或 Linux 的 myapp / myapp.bex 之外的原生 ELF
main.html            # UI 源；或 main.binui（已编译产物，跳过编译）
logo.png
```

**B. 子目录**（资源多 / 多屏 / 现状 demo 形态）

```
myapp.exe
myapp/
  main.html
  dialog.html
  images/hero.png
```

两者**资源写法完全一样**，因为规则只有一条：

> **`src=` 一律相对 UI 文件所在目录解析。**（与 `<video src>` 现有约定一致）

所以 A 里写 `src="logo.png"`、B 里写 `src="images/hero.png"`，搬目录不用改 UI。

---

## 3. 路径解析规范

把现在 7 个 demo 各自复制的一份 `ExeDir + ResolveUI` 收敛成一条规则（现状见 `examples/svgdemo/main.cpp:18-33`、`examples/singlewindow/main.cpp:27-75`）。

**UI 文件查找顺序**：

```
1. --ui <path>                        显式指定（最高优先级）
2. $BINUI_UI 环境变量
3. <exeDir>/<exeName>/main.html        子目录形态
4. <exeDir>/main.html                  平铺形态
5. <exeDir>/<exeName>/main.bui         已编译产物（跳过编译；旧名 .binui 也认）
6. <repo>/examples/<name>/main.html    仅 debug/未安装构建（源码树内直接跑）
7. 全部失败 → GenerateDefaultBinUI() + stderr 警告（保留现有行为，不黑屏退出）
```

**两条硬规定**：

1. **`exeDir` 必须用 `GetModuleFileNameW(nullptr, …)`（Windows）/ `readlink("/proc/self/exe")`（Linux）拿真实路径，禁止用 `argv[0]`。** 现在 7 个 demo 全用 `ExeDir(argv[0])`——通过 PATH 启动、软链、被改名时 `argv[0]` 根本不是路径，资源就找不到。
2. **禁止依赖 CWD。** 桌面环境双击启动时 CWD 通常是 `$HOME` 或 `/`，一切相对路径都会失效。

**图片资源查找**：对 `CompileResult::images` 里每个 `p`，依次在

```
<a> UI 文件所在目录 / p
<b> exeDir / p
<c> exeDir / <exeName> / p
```

查找，命中即用；缺失时 stderr 打 `MISS image "<p>"`，并按渲染器现有降级（`README` 的"无资源时回退为圆形/纯色块"）继续启动，**不中断**。

**键的契约**：`p` 字符串是渲染器查表的键（`setImageOrder(res.images)` + `loadResources(map)`），**逐字节使用，不归一化**（不转小写、不合并 `./`、不补全大小写）。归一化交给编译器，容器/加载层只做搬运——两侧归一化规则一旦不一致，就会出现"文件明明在却 MISS"。

**实现状态：✅ 已落地。** 规则实现在 [`include/binui_paths.h`](../include/binui_paths.h)（header-only），全部 7 个 demo 已改为共用：

| API | 作用 |
|-----|------|
| `ex::ExePath()` / `ex::ExeDir()` | `GetModuleFileNameW` / `readlink("/proc/self/exe")` 取真实路径（替掉过去的 `ExeDir(argv[0])`） |
| `ex::ParseArgs(argc, argv)` | 解析 `--ui <path>` / `--watch` / 位置参数 |
| `ex::ResolveUI(args, appName, fileName)` | 上面 7 级查找，返回真实路径（空 = 没找到） |
| `ex::LoadImages(uiPath, images, appName, extraDirs)` | 按上面的顺序加载图片字节，键 = 编译器给的 `src` 原样字符串 |

> 修掉的真实 bug：7 个 demo 过去各自复制一份 `ExeDir(argv[0])`。通过 PATH 启动、软链调用、二进制被改名时 `argv[0]` 根本不是路径，图片全 MISS。现在统一走系统 API。


---

## 4. 元数据归属（待办）

散装形态没有容器头，元数据（程序名/开发者/版本/图标/AppID）的自然落点是 **`<app_window>` 属性**——UI 文件即应用描述：

```html
<app_window name="数据面板" dev="Dev" version="1.4.2"
            app_id="io.github.dev.datapanel" icon="logo.png"
            title="数据面板" width="1280" height="720">
```

现状：`BinUIWindowDesc`（v5，`include/binui_format.h:132`）已有 `width/height/flags/aspect/bgColor/title`；上面标 **粗** 的那 5 个属性**还没有**。

低成本落地路径（不必升 v11）：走已有的 `extra` 键值段（`name=` / `dev=` / `version=` / `app_id=` / `icon=`），`BinUIWindowDesc` 结构一个字节都不用动。

---

## 5. UI 热重载（开发期）—— ✅ 已实现

```bash
build-mingw/bin/svgdemo.exe --watch                                   # 用 exe 同级的 UI
build-mingw/bin/svgdemo.exe --watch --ui examples/svgdemo/main.html   # 直接盯源码树
```

保存 UI 后自动重编译并换树，不用重启进程。实现：[`include/binui_hot_reload.h`](../include/binui_hot_reload.h)（header-only）；7 个 demo 全部接入。

| 决策 | 取值与理由 |
|------|-----------|
| 变化检测 | **轮询 mtime+size**，采样节流 100ms。不用 inotify/`ReadDirectoryChangesW`：省掉两套平台分支，5 次/秒 stat 的开销可忽略 |
| 防抖 | 文件**稳定 250ms** 才动手（编辑器写一半时读到半截文件会编译出垃圾） |
| 编译失败 | **保留旧 UI**，只在 stderr 报错。热重载绝不能把界面刷成黑的 |
| 状态恢复 | 窗口尺寸 / 滚动偏移 / 键盘焦点 / 输入框内容（`focus()` 重建编辑会话 + `setCaret`）。按 **节点 id** 恢复——id 由编译器按 DOM 顺序分配，改动不影响编号时不丢 |
| 动态节点 | `addNode` 造的行（galgamechat 的 backlog、图表柱条）换树后消失，属预期；galgamechat 因此选择"重载即重开对话" |
| 通知后端 | `Tick()` 返回 `Reloaded` 后由 demo 自己 `app.invalidateAll()` + 刷新本 demo 缓存的节点几何（shell 的失效是应用侧的事，库不该知道） |

回归测试：[`tests/test_hot_reload.cpp`](../tests/test_hot_reload.cpp)（无窗口，`build-mingw/bin/test_hot_reload.exe`，工作目录须为仓库根）—— 覆盖路径解析 6 级顺序、未改动不触发、保存即重载、滚动/焦点跨重载保留、坏 UI 不刷空、文件消失不误触发。

> 未做：只 watch **单个 UI 文件**的 mtime。同目录换图（`updateImage`）仍需自己重新读文件（singlewindow 的"重新载入"按钮就是干这个的）。


---

## 6. 与 `.bex` 容器的衔接契约

将来若启用容器，**目录内容原样映射**，不改任何 UI 与资源写法：

| 散装文件 | → 容器条目 |
|---|---|
| `main.html` | `BEX_KIND_LAYOUT`（编译后）或 `BEX_KIND_SOURCE`（`--embed-source`） |
| `*.png` / `*.jpg` | `BEX_KIND_IMAGE`，逻辑路径 = 相对 UI 目录的路径（**与 §3 的键契约逐字节一致**） |
| 其它附带文件 | `BEX_KIND_DATA` |
| `<app_window>` 的 name/dev/version/app_id/icon | `BinUIAppMeta` 对应字段 |
| 图标多尺寸 | `BEX_KIND_ICON`，`nameOff = "icon/64.png"` 等 |

**这一节就是两者唯一需要对齐的契约**：只要 §3 的"src 相对 UI 目录、逐字节作键"不变，将来接容器是纯增量，零返工。
