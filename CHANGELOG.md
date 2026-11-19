# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
`【格式 vN】` 指 binUI 二进制布局的 `BINUI_VERSION`（`include/binui_format.h`），
与语义化版本不是一回事——**格式号只在二进制布局变化时递增**。


## [0.30.0] - 2026-11-19 【格式 v34 不变】

**原语级探针** —— 让"这一帧的钱花在哪"变成可读的数字，而不是拿别的基准反推等价解。

### Added —— `BINUI2D_PROF=1`

在 `BinUI2D` 的 14 个原语入口上挂探针（两次 `steady_clock::now()`，约 20 ns/次，
关闭时只多一次 bool 判断），退出时按原语分类打一张表：

```
[bin2d-prof] bin2d 后端原语分解 frames=40 原语总耗时 22.13 ms/帧
[bin2d-prof]   fillRounded         53.0 次/帧    261.41 us/次   13.855 ms/帧   62.6%   4078 kpx/帧   2.68 ns/px
[bin2d-prof]   fillRect            89.0 次/帧     53.82 us/次    4.790 ms/帧   21.6%   1158 kpx/帧   0.52 ns/px
```

带**盒面积累计**：只报"次/帧"的话，一个 900×932 的圆角底和一个 12×14 的圆点在同一
行上是同一个数，看不出钱花在哪种面积上；有了 kpx/帧 与 ns/px，可以直接和画布地板
（`memset` 0.07 ns/px）比。

### Changed —— 后端跟着 bin2D 0.13.0 的栅格优化

`fillRounded` / `fillRect` 换成 bin2d 的新快速路径（像素逐字节不变）。
features 页全屏帧时 **22.13 → 12.74 ms/帧**。

### 这一轮量出来的东西（记在这里，省得下次再猜）

| 事实 | 数字 |
|---|---|
| 全屏 23.6 ms 不是"像素的钱" | 1.6 M px 按实测原语只值 2.2–8.3 ms |
| 钱在按调用计的固定成本 | 1px 圆角描边 60–126 µs/次、卡片 236 µs/张、满屏组图层 1.77 ms/层 |
| 替代花法 | 折算成 ≈2100 个小圆角节点 / ≈100 张卡片 / ≈437 个图元都能对上 23.6 ms |
| 过绘制 | 838 800 px 的画布上每帧画 4.08 M px 的圆角底 = **4.9× 过绘制** |
| 带宽 | 唯一贴地板的是 memset（1.0×）；填充类 11×（大面积圆角/路径）到 41×（②查表）—— **全部计算受限** |

附带发现两条（都已修或已记账）：`fillRect` 的"解析覆盖率快路径"曾比走路径**慢 1.7 倍**；
`Canvas::clear(Color::trans())` 是 **no-op**（`Surface::fillRect` 对 `a==0` 早退）。
## [0.29.0] - 2026-11-19 【格式 v34 不变】

**默认后端就是 bin2d** —— master 那条分层从这一版起是**默认行为**，不再是并列的一条路：

```
binUI ──► bin2d / bin3d ──► binGPU ──► GL / Vulkan / D3D11
                                      └► 系统贴屏（无 GPU 时）
```

三份 GPU 后端没删，但降级成**可选的对照构建**：`-DBINUI_GPU_GL` / `_VK` / `_D3D11`
（`make demos-vk` / `demos-d3d11`），以及 `make pixels` 那条"像素参考实现"的路。
理由写在 `src/backends/binui_ui.h` 顶部：binUI 这一层不该再自己拿图形 API ——
以前圆角/渐变/字形要在三个后端各写一遍，改一处漏两处（VK 的 `strokeRounded` 就漏过
`thickness`，见 0.25.0）。

### Changed —— 默认后端

- `src/backends/binui_ui.h`：默认分支从 GL 改成 **bin2d**；`-DBINUI_GPU_GL` 变成显式
  选项（以前是默认）。三个 `-DBINUI_GPU_*` 名字不变，`UiBackend` / `UiApp` 一处切换。
- `Makefile`：9 个默认示例改成链 `binui_bin2d.o` + `$(BINGPU_LNK)`，产物名**不再带
  `_2d` 后缀**（`appshow.exe` 而不是 `appshow_2d.exe` —— 用户是要双击那个的）。
  `make demos-2d` 保留成别名，脚本/文档里那批命令继续可用。
- `src/binui_c.cpp`：C ABI 的默认后端也从 `BinUIAppGL` 换成 `BinUIApp2D`，
  `libbinui_c.a` / `binui_c.dll` 的构建依赖与链接跟着改（`Makefile` 两处 + `test_capi`）。
  C ABI 只用外壳 API 与 `gfx()` 返回的 `IBinUIGfx&`，所以换后端只是换一个类型别名。

### Changed —— 核心库不再依赖任何具体后端

- `include/binui_charts.h` 的 `BinUIChart::draw(BinUIGL&)` → `draw(IBinUIGfx&)`
  （`LineChart` 只用到 `fillRounded` / `fillLine`，本来就是接口方法）。
  顺带**去掉了 `#ifdef _WIN32` 分叉** —— 以前非 Windows 构建要靠一个空实现占住
  vtable 符号；现在核心库在任何平台都不链接图形后端。

### Fixed —— VK / D3D11 的 `filter: blur(Npx)` 在 N < 4 时没糊

- 与 0.28.0 修的 GL 同一处病：降采样链函数在 `levels <= 0` 时**提前 return**，
  于是 `gaussBlurLevel` 一次都没跑。改成 GL 的判据（链只包 `levels > 0`，
  高斯留在块外，0 级也跑）。
- 取证：把 VK 的提前返回临时放回去重渲，**VK vs GL = 17791 像素 (5.735%)，
  最大通道差 100，26 个连通区域**（正好是页面上那些 blur/阴影块）；修复后 **0**。
  这个 bug 之所以一直没被跨后端门禁抓到，是因为**三端一起没糊，互比是 0 差异**
  —— 只有 CPU 参考（bin2d）能照出来。

### 验收

| 项 | 结果 |
|---|---|
| `make all` | 0 warning，默认示例全部链 bin2d |
| `make test` | 6 个套件全绿（含 `test_shell` 38 项、`test_capi`） |
| `make pixels`（GL 金标准，19 例） | 通过，`svg-filter` 差异 0 像素 |
| `make pixels-bin2d`（容差 0） | 18/18 像素一致 |
| `make pixels-vk` / `pixels-d3d11` | 通过（`svg-filter` 0 像素 / 最大通道差 2） |
| 真窗口 | 默认 `appshow.exe` 截图：整壳（侧栏/卡片/弹窗/页脚）正常，`present 可阻塞垂直同步=否` 确认走的是 bin2d |

## [0.28.0] - 2026-11-19 【格式 v34 不变】

**把三份 GPU 后端剩下的能力缺口补齐** —— 0.27.0 之后"bin2d 能当唯一后端"只剩三处
功能差：透明分层窗、图片滤镜、图层外阴影。这一版全部补上，并顺带修掉一个**三端共有
的模糊 bug**。

```
binUI ──► bin2d ──► binGPU / 系统贴屏
                     └─ 透明分层窗：UpdateLayeredWindow（不经过交换链）
```

### Added —— 透明分层窗（`bin2d+layered`）

- `BinUI2D::present()` 的第一支现在是透明判断：`transparent && hwnd` → Surface
  **只换字节序**（premultiplied RGBA → premultiplied BGRA）→ `UpdateLayeredWindow`。
  不反预乘：`ULW_ALPHA` 要的就是预乘值，反预乘只服务 `SRCCOPY` 贴屏。
- compositor 会被拆掉（交换链没有逐像素 alpha），窗口没设 `WS_EX_LAYERED` 时退回贴屏。
- `presentBackendName()` 多一个 `bin2d+layered`。
- 这是删三份 GPU 后端前唯一"用户可见"的缺口：以前 `setTransparent(true)` 在 bin2d
  后端只存了个标志，分层窗会直接不上屏。

### Added —— `setImageFilter`（v26 图片滤镜）

- `BinUIImageFilter`（3×3 + 3 偏移、作用在直通 RGB）逐位搬进 bin2d 的
  `ColorMatrix`（4×5、同样作用在直通空间），alpha 行取恒等。
- 按 **`(图, 矩阵)` 缓存过滤变体**，键用 `binui::ImageFilterKey` —— 与三份 GPU 后端
  同一个函数（那边是把哈希编进纹理缓存键，这边是一份 Image）。换图时变体作废。

### Added —— 图层 `drop-shadow`

- `popLayer` 里按三份 GPU 后端的同一套算法补上：纯色预乘 → 乘层 alpha 得**上色轮廓**
  → 按 σ 模糊 → 按取整偏移合成到本体**下面**。书写顺序里靠前的在上层，所以逆序画。
- 已知口径（不是笔误）：阴影不乘组 alpha，四端一致；要改就四端一起改。

### Fixed —— `filter: blur(Npx)` 在 N < 4 时三端**完全没糊**

- 根因：`compositeThroughLevels()` 里那句分离高斯被关在 `if (levels > 0)` 里面，而
  `PaintFilterBlurLevel(N)` 在 N<4 时返回 0 —— 整个块被跳过，只剩"抓层内容 → 1:1
  拷回去"。`filter: blur(3px)` / SVG `stdDeviation="3"` 因此一点没糊。
- GL 改成"0 级也跑卷积"，判据变成"有没有 σ"而不是"降了几级"。VK / D3D11 同款，另行补齐。
- 这个 bug 是靠 bin2d 后端**逐像素比对**发现的：两个后端对同一个 svg-filter 用例差
  13.8%，把 bin2d 的模糊改成 σ 语义后差到 8.2%，再修掉这条 → **2.446%**，
  剩下的差异全在文字行（已知的字形采样类）。

### Changed —— 模糊改走 bin2d 0.12.0 的 σ 语义

`bin2d::Blur(radius)` 的 `radius` 是**盒式核半宽**，而被喂进来的数字全是 σ
（CSS `blur(Npx)` / SVG `stdDeviation`）。σ=3 的盒式实际只有 σ≈2.0，所以 bin2d 这一侧
一直"糊得不够"。改用 0.12.0 新增的 `bin2d::BlurSigma(surface, sigmaPx)`（三趟盒式近似
高斯，实测 10–90% 过渡宽度 2.50–2.67σ，理想值 2.563σ）：
`filter: blur`、`drop-shadow`、`backdropBlur` 三处全换。

### 验收

| 项 | 结果 |
|---|---|
| `make pixels`（GL 金标准，19 例） | 通过（只有 `svg-filter` 一例更新） |
| `make pixels-bin2d`（bin2d 自回归，容差 0） | 18/18 像素一致 |
| `make bin2d-vs-gl` 的 `svg-filter` | 13.8% → 8.2% → **2.446%**（余下全在文字行） |
| `tests/test_shell` | 38 项通过（新增透明分层窗 3 项） |
| `make test` | 6 个套件全绿 |

### 还没做

三份 GPU 后端仍不能删：`IBinUIGfx` 之外还剩 4 处编译期硬依赖（`binui_charts.h` 的
`draw(BinUIGL&)`、C ABI 的 `BinUIAppGL`、`binui_ui.h` 默认分支、CMake 没有 bin2d 目标）。
能力缺口这一栏现在是空的 —— 见 `docs/migrate-to-bin2d.md`。

## [0.27.0] - 2026-11-19 【格式 v34 不变】

**bin2d 后端接管整个应用外壳** —— 0.26.1 只证明了"bin2d 能把像素送到屏幕上"；这一版
证明的是"它能顶替三份 GPU 后端当**唯一的后端**"。差别在于：上屏只是一帧的最后一脚，
输入 / IME / 拖边框 / DPI / 脏区这些外壳行为从来没在 bin2d 上跑过。

``` 
binUI 外壳（BinUIAppT<Gfx>，与图形 API 无关）
   └── BinUI2D ──► bin2d ──► binGPU / 系统贴屏
```

### Added

- `make demos-2d`：同一份示例源码 + `-DBINUI_GPU_2D`，产出 9 个
  `build-mingw/bin/examples/*_2d.exe`（含 `appshow_2d` —— 多页 / 弹窗 / 拖拽 / 滚动 /
  输入框全在那个演示里）。与 `demos-vk` / `demos-d3d11` 并列，四端可并存。
- `BinUIApp2D = BinUIAppT<BinUI2D>`（`src/backends/binui_bin2d.h`）：与
  `BinUIAppGL` / `BinUIAppVK` / `BinUIAppD3D` 同一份模板。头文件里补上
  `backends/binui_app.h` 的包含（`#ifdef _WIN32` 守卫 —— `binui_bin2d.h` 也被
  `bin2d_shot` 这类无窗口工具包含，不该被拖上 windows.h）。
- `tests/test_shell`（38 项断言，进 `make test`）：外壳 × bin2d 后端的**接口契约**
  回归。子窗口模式跑在隐藏父窗口下，可进 CI。覆盖 12 组：
  1. `openChild` 参数校验 / 建窗 / 类名（`BinUIApp2DWindow`，与三份 GPU 后端不冲突）
  2. 首帧：脏矩形 = 整窗、像素尺寸 = w·h·4、颜色正确
  3. **脏区语义**：`invalidateRect(10,20,30,40)` → 脏矩形 `{10,20,40,60}`；脏区外的
     像素保留上一帧的值（不是被清成透明）—— 这条是"窗口抖动时整屏闪"的防线
  4. 组透明度：`pushLayer(α=0.5)` 的合成结果 `(25,51,102,255)`
  5. 裁剪栈：`pushClip` 真的挡住填充，`popClipTo` 真的恢复
  6. 输入：鼠标移动/按下/抬起/双击/右中键/中键/离开/滚轮（屏幕坐标→客户区坐标）、
     `WM_CHAR`（ASCII / 中文三字节 / 代理对四字节 / 控制字符被丢弃）、`WM_KEYDOWN`、
     `WM_SETCURSOR` 光标查询
  7. **输入法**：`imeActive=false` → `ImmGetContext` 返回 null（上下文被摘下）；
     `true` → 非 null（挂回）。不是"调了没炸"，是真的查了上下文句柄
  8. `WM_SIZE` → 挂起到帧内应用（回调 + 后端尺寸 + 像素缓冲都跟着变）
  9. `WM_DPICHANGED` → `dpiChanged` 回调 + `dpi()`
  10. 跨线程 `pump` 返回 -1（线程亲和自检）
  11. 同进程双实例并存，像素互不干扰
  12. `requestExit` / `close` 幂等

### Changed

- `docs/migrate-to-bin2d.md` §2 的"三份 GPU 后端的去留"更新：原先列的两条留用理由
  里，①（输入/IME/拖拽只在 GL/VK 上真跑过）**已作废** —— 本条由 `make demos-2d` +
  `tests/test_shell` 兑现；②（性能账）与"能不能删"无关，是另一件事。

### 验收（本轮实测）

| 项 | 结果 |
|---|---|
| `make demos-2d` | 9 个 exe，0 warning |
| `tests/test_shell` | 38/38 通过 |
| `make test`（6 个套件） | 全绿 |
| `make pixels-bin2d` | 18/18 像素一致（容差 0） |

### 测试期踩到的两处口径（写在这里省得下次再踩）

- **外壳有帧节拍闸门**：`renderFrame` 里 `if (!paced) { if (now < nextFrameTime_) return; }`。
  bin2d 后端的 `present` 不阻塞垂直同步 → `paced=false` → 节拍完全由外壳按刷新率推算。
  测试若不等这一拍就 `pump()`，读到的是"上一帧还挂着"，脏区断言会全部落空。所以
  测试的 `PumpOnce` 要先睡过一个帧预算。
- **`PostMessage(WM_DPICHANGED)` 会被系统丢掉**（收不到），必须用 `SendMessage`。
  这类"消息发出去没反应"的坑，只能靠断言"回调真的被调了"发现。

### 还没做

- 三份 GPU 后端仍不能删。审计（本轮实测）结论：`IBinUIGfx` 之外的三份后端还留着
  4 处编译期硬依赖 + 3 项 bin2d 没有的能力 —— 见 `docs/migrate-to-bin2d.md` 新增的
  「删后端的清单」一节。其中**透明分层窗**（`UpdateLayeredWindow`）是唯一"用户可见"的
  缺口：`setTransparent(true)` 在 bin2d 后端只存了个标志，`WS_EX_LAYERED` 窗口会不上屏。

## [0.26.1] - 2026-11-19 【格式 v34 不变】


**bin2d 后端接窗口呈现** —— 0.26.0 的 `BinUI2D` 只会离屏出图；这一版让它能当真正的
窗口后端用，于是"binUI 不写图形 API"这件事从"注释"变成"可运行的程序"。

```
binUI ──► bin2d ──► binGPU ──► GL / Vulkan / D3D11
                 └► 系统贴屏（没有可用 GPU 后端时）
```

### Added

- `BinUI2D::init(hwnd, w, h, vsync)` 真的用上了 `hwnd`：建一台
  `bin2d::GpuCompositor`（窗口版），按 **GL → Vulkan → D3D11** 的顺序找第一个
  能起来的后端。
- `present()` 按三级降级：
  1. `bin2d::GpuCompositor::presentSurface` → bingpu 交换链（GPU 上屏）；
  2. 起不来 → Surface 反预乘成 BGRA → `StretchDIBits` 贴屏（**无显卡的机器靠这条**）；
  3. 没有窗口（`bin2d_shot` 那类离屏调用）→ 空操作，像素留在 Surface 上。
  `presentBackendName()` 报出实际走的那条。
  一个后端都起不来**不是失败**：窗口照样能开，只是少了 GPU 合成。
- `BINUI2D_NO_GPU=1` 强制走系统贴屏（验收兜底路径 / 隔离"慢在 CPU 还是慢在 GPU"）。
- `tools/bin2d_window`：真窗口验证工具 —— 建窗、跑 N 帧、分段计时（光栅 vs 呈现）、
  可选抓图。`--frames N` / `--shot png` / `--headless` / `--gdi` / `--clip x,y,w,h`。
- `binui_ui.h` 加 `BINUI_GPU_2D` 分支：`UiBackend = BinUI2D`、`UiApp =
  BinUIAppT<BinUI2D>`、`UiBackendName() == "bin2d"`。示例源码一行不改就能编出
  bin2d 版本。
- `BinUI2D::presentViaGdi()` / `appClassName()`：`BinUIAppT` 外壳要求的两个成员。

### Fixed —— bin2d 侧（0.11.0）

- `bin2d::DrawSurface` 新增**图集 1:1 快路径**：UV 子矩形落在整纹素边界、且与目标
  矩形 1:1 时，采样点在纹素中心，双线性退化成取一个纹素。这既是性能（字形从 25 µs
  降到几个 µs），也修掉了一个精度问题 —— 原来的 `u*(sw-1)` 口径在 1:1 时把采样点
  放在"纹素边界 + 0.4 纹素"上，所以文字是糊的。

  实测（18 个像素用例对 GL 基准，阈值 8）：

  | 用例 | 修前 | 修后 |
  |---|---|---|
  | circle | 1.823% / max 82 | **1.617% / max 37** |
  | display-none2 | 1.955% / 114 | **1.446% / 56** |
  | probe | 2.478% / 149 | **1.603% / 63** |
  | radius-matrix | 0.638% / 157 | **0.485% / 63** |
  | text-shadow | 11.832% / 171 | **6.473% / 79** |
  | text-shadow-multi | 19.483% / 175 | **15.807% / 148** |

  （`image-mip` / `svg-filter` 不变：前者是缩放预滤波 vs mip 链，后者仍未实现。）

### 性能 —— 诚实的一页

features 页（900×932，900 个节点级别）实测：

| 场景 | 光栅+遍历 | 呈现 | 每帧 |
|---|---|---|---|
| 全屏重画 + GL 上屏 | 23.6 ms | 9.41 ms | 33.0 ms |
| 全屏重画 + 系统贴屏 | 23.6 ms | 3.22 ms | 26.8 ms |
| 脏区 120×40 | 3.56 ms | — | 3.56 ms |
| 脏区 900×60 | 1.13 ms | — | 1.13 ms |

对照 GL 后端同一页：遍历 + 发射 **0.071 ms**（栅格化在 GPU 上）。

所以差距**不在"bin2d 慢"，而在"谁来栅格化"**：GPU 后端把 160 万像素的填充面积
交给显卡，bin2d 在 CPU 上做。正常 UI 走脏矩形（外壳 `BinUIAppT` 本来就有这套机制，
`beginFrame(rc)`），成本 1~3.5 ms；只有整屏重画（滚动 / resize / 全屏动画）才付
23.6 ms 的全价。

这一版**没有**为性能做架构改动。真要压全屏那 23.6 ms，正确的路是接 bin2d 的
`Scene` 缓存（静态组只贴图，bin2d 实测 8.2×），那是 P4 第二半的事。

### 验证

- `tools/bin2d_window` 在 features 页上三条路都跑通：`present=gl`（binGPU 的 GL 后端）
  / `present=bin2d+gdi` / `present=bin2d+offscreen`；抓图三张内容一致；
- `make pixels-bin2d`：18/18 逐像素相同（基准已按新采样口径重生成）；
- `make pixels`：18/18 零差异（三份 GPU 后端一位没动）；
- `make baseline-strict`：172 项完全一致；
- `make test` 全绿；`make all` 0 警告。

## [0.26.0] - 2026-11-19 【格式 v34 不变】

**bin2d 后端：`IBinUIGfx` 的一份 CPU 实现**（`src/backends/binui_bin2d.{h,cpp}`）。
这是 master 2026-11-19 那条分层的第一个落点：

```
binUI  ──►  bin2d / bin3d  ──►  binGPU  ──►  GL / Vulkan / D3D11
```

在此之前 binUI 有三份各自直写图形 API 的后端（GL 1985 行 / VK 4244 行 / D3D11 2193 行），
任何一次绘制语义调整都要在三处同时改对（0.25.0 修的 VK `thickness` 就是漏改的结果）。
`BinUI2D` 把这层边界**下移**：binUI 只负责"节点树 → 原语调用"，几何交给 bin2d 的
覆盖率光栅（CPU 像素权威），将来上屏交给 bin2d 的 `GpuCompositor` → binGPU。

### Added

- `binui::BinUI2D`（`src/backends/binui_bin2d.h/.cpp`，~700 行）：`IBinUIGfx` 的完整实现。
  30 个原语全部覆盖 —— 矩形/圆角/仿射圆角/线段/渐变/圆角描边、图片（UV 子矩形 +
  九宫格 + tint）、文本（排版/整形/阴影）、裁剪栈、图层（组透明 + 离屏 + blur）、
  混合模式（擦除/保留）、仿射状态、`backdropBlur`（毛玻璃）。
  **一行图形 API 都没有**：没有 `gl*`、没有 `vk*`、没有 D3D 调用。
- `tools/bin2d_shot.cpp`：把 UI 渲染成 PNG 的工具，**无窗口、无驱动、无 GPU**。
  `gl_shot` / `vk_shot` / `d3d11_shot` 都要先建窗口（交换链绑窗口）、再靠 GDI 抓
  DC（交换链图像抓不到）；bin2d 后端没有交换链，像素直接落在 `bin2d::Surface` 上，
  写完就存 PNG —— 一台没有显卡的机器也能跑。
- `make pixels-bin2d`（**容差 0，逐字节比对**）/ `pixels-bin2d-update` /
  `bin2d-vs-gl`（只报告不判失败）。基准在 `tests/pixels-bin2d/`（18 个用例）。
  为什么不和 `docs/pixels`（GL 基准）比：两条路的覆盖率 AA 实现**本质不同**
  （bin2d 用 8× 子采样解析覆盖率，GPU 用 1px 模板环/条带），边缘必然差几级 ——
  那是换实现的标准差，不是 bug。

### 复用而不是重写

文字与图片这两块**几乎没写代码**，因为它们本来就是后端无关的：

| 复用 | 原来做什么 | bin2d 后端做什么 |
|---|---|---|
| `binui_glyph_atlas.h`（1169 行） | `IGlyphTexHost` → `glTexSubImage2D` / staging + copy | `IGlyphTexHost` → 写进 `bin2d::Image` |
| `binui_paint.h` | 九宫格切分、圆角层几何 | 直接用（同一份切分数学） |
| `binui_win_image.h` | GDI+ 解码 → BGRA 直通 | 同一份解码，转成预乘 `bin2d::Image` |
| `drawNodesBinUI()` | —— | 节点树遍历，一行没改 |

排版 / 字形光栅 / 整形缓存 / 文字阴影全部走同一份 `GlyphAtlas`，所以**字形位图逐字节
相同**，差异只可能来自后面的采样（见下面的"与 GPU 基准的差异"）。

### Fixed（bin2d 侧，0.10.1）

- **`bin2d::Canvas::clipRect` 在图层里算错坐标系**：它用裸 `state().xf` 映射局部矩形，
  而落像素用 `deviceTransform()`（= xf **再减掉层原点**）。两者在图层里差一个 origin，
  结果是"层内第一次 `clipRect` 就把内容裁成 0×0"，整个层空白。
  `clipPath` 一直用的是 `deviceTransform`，只有 `clipRect` 漏了 —— 已在 bin2d 0.10.1 修，
  并补了回归测试 `test_composite::TestClipRectInsideLayer`（4 项断言）。
  影响面：`opacity` 用例 48.9% → **0%**，`svg-filter` 45.3% → 13.8%。

### 与 GPU 基准（`docs/pixels`）的差异 —— 如实记账

`make bin2d-vs-gl`（18 个用例，阈值 8）：

| 用例 | 不同像素 | 最大通道差 | 性质 |
|---|---|---|---|
| opacity | **0.000%** | 0 | 一致 |
| shadow | 0.039% | 19 | 边缘 |
| grad-angle | 0.216% | 21 | **GL 金样有伪影**（见下） |
| radius-matrix | 0.638% | 157 | 边缘 |
| circle | 1.823% | 82 | 边缘 |
| display-none2 | 1.955% | 114 | 边缘 |
| image-round | 2.214% | 82 | 边缘 |
| probe | 2.478% | 149 | 边缘 |
| svgpath / svggrad / svgstroke | 3.2–3.6% | 75–82 | 边缘 + 文字 |
| svgjoin / svgaa / svgfinish | 4.1–5.0% | 78–143 | 边缘 + 文字 |
| image-mip | 10.677% | 120 | 缩放预滤波 vs mip 链 |
| text-shadow / -multi | 11.8% / 19.5% | 171 / 175 | 字形采样口径 |
| svg-filter | 13.780% | 204 | **未实现**（`setImageFilter`） |

按"差异像素是否落在边缘"做过分桶：**除 grad-angle / text-shadow* / svg-filter 外，
其余全部 ≥ 99.4% 的差异像素在 1px 边缘上**（circle 99.9%、image-round 100%、
image-mip 100%）。这是两种 AA 实现的固有差，不是布局/颜色错。

两处值得单独说：

- **grad-angle：bin2d 更正确**。第三个面板（180° 垂直渐变）在 GL 基准里 y=43 这一行
  是 `(202,0,53)`，而上下两行是 `(234,0,21)` / `(191,0,64)` —— 单调渐变中间插了一行
  偏暗的接缝（GL 条带/顶点插值的量化伪影）。bin2d 输出 `(206,14,63)`，单调递减。
- **文字更锐**：bin2d 的 `DrawSurface` 用 `u*(sw-1)` 做纹素中心映射，GPU 硬件用
  `u*sw-0.5` —— 半个纹素的采样偏移。视觉上 bin2d 的字略细、GL 的字略实。
  这是两条实现的标准差；要统一就得改一方的采样口径（会影响 bin2d 现有图片测试），
  本轮不动。

### 验证

- `make pixels-bin2d`：18/18 用例**逐字节相同**（自回归，容差 0）；
- `make test`：全绿（test_css / test_elements / test_font / test_capi）；
- bin2d `make test`：**全部通过**（9 个套件，含新增的层内裁剪回归）；
- `make all`：0 警告。

### 未做（下一轮）

- **窗口呈现**：`present()` 现在是空操作，像素留在 Surface 上。接法是把这个 Surface
  交给 `bin2d::GpuCompositor::composeOne` → binGPU 交换链，外壳（`BinUIAppT`）再加一个
  模板参数即可 —— 那时 binUI 的"呈现"也完全离开 GL/VK/D3D11。
- `setImageFilter`（图片滤镜，`svg-filter` 用例）—— bin2d 有 `ColorMatrix`，
  但"按 (图, 矩阵) 分键缓存"还没接。
- `pushLayerEx` 的 `drop-shadow` —— blur 已经走 `pushOffscreen` + `bin2d::Blur`，
  阴影那半边还是空的。

## [0.25.0] - 2026-11-19 【格式 v34】

**P3 第二半：三端的"圆角图层计划"收口到一份**（`binui_paint.h`）。在它之前，填充/描边/
仿射圆角各端的差异只是"还没被发现"，不是"不存在"。

### Changed

- 新增共享实现（`include/binui_paint.h`）：
  `RoundedLayerCount` / `BuildRoundedLayer` / `BuildRoundedLayerQuads` / `BuildRoundedLayerQuadsXf`。
  它管两件以前三端各写一份的事：**层数**（描边 = round(thickness) 层同心环、每层往里缩
  1px 且半径减 1）与**一层的九宫格几何**（四角 + 四段 + 中心，UV 与模板边长绑定）。
  分"层数 + 取一层"而不是一次算完：层数上界是短边一半（400+ 层 × 9 块），一次算完要几百 KB。
- GL / VK / D3D11 的 `fillRounded` / `strokeRounded` / `fillRoundedXf` 全部改成调这份实现。
  D3D11 有 **66 行**是把九宫格 UV 数学手抄一遍（`cu = r/t`、`strip = 0.5 + 1/t`），删掉；
  后端合计 **−114 行**（+123 / −237），共享实现 +94 行（只有一份）。
- 回退语义统一：半径 ≤ 0 时三端都退回 `fillRect`（以前 VK 画四条矩形、GL/D3D 画整块）。

### Fixed

- **VK 的 `strokeRounded` 丢掉 `thickness`**：模板是 1px 环，GL/D3D11 按 `round(thickness)`
  叠同心层，VK 只画 1 层 —— 2px 的"编辑中焦点环"在 VK 上是 1px。收口后三端同一份代码。
  这是本轮**唯一会改像素**的点，且只影响 `thickness > 1` 的圆角描边。
- 该状态（聚焦编辑环）**没有像素用例**：它需要运行时焦点态，静态出图工具拍不到。
  已列进 `docs/migrate-to-bin2d.md` 的待办；目前靠共享计划单测 + 三端代码收口兜底。

### 验证

- `make pixels`：18/18 用例 **0 不同像素**（对照 GL 基准）；
- `make baseline-strict`：**172 项完全一致**（三端各 150 个像素用例的原始 ARGB 逐字节比对，
  消失 0 / 新增 0 / 尺寸变 0 / 哈希变 0 / 画质变 0）；
- 三端在"2px + 3px 圆角环"临时页面（1280×720）上逐像素 **0 diff**（pixel_diff 容差 0）；
- `make test`：全绿；`test_elements` 新增 **35 条** `v49` 断言（层数/每层几何/九宫格 UV
  手算值/仿射单位变换逐位相同/平移缩放）。

## [0.24.1] - 2026-11-19 【格式 v34】

**只动台账与取证，不动渲染**（像素 / 原语流零变化：`make baseline-strict` 172 项全等）。

### Changed

- `make quality` 的原文台账现在只收 **stdout**：bin2d 0.9.1 起把计时行（ms/µs）
  写到 stderr，而台账是逐字比较的 —— 收进来就会因机器噪声每次红一次。
- 台账新增 bin2d 的两条**确定性**画质数字（qa 11/12）：mip 满链 6 级 / 5460 字节、
  预算 5000 字节 → 1 级 / 4096 字节。`qa` 从 11 条变 13 条，合计 170 → **172**。

### 验证

- `make baseline-strict`：172 项完全一致（消失 0 / 新增 0 / 尺寸变 0 / 哈希变 0 / 画质变 0）；
- bin2d 侧本轮升到 0.9.1（滤镜 / 投影 alpha 罩 / 九宫格自动边界 / mip 上限 / 子树缓存），
  binUI 的像素一位没动 —— 这三端的基线就是证据。

## [0.24.0] - 2026-11-19 【格式 v34】

**移除 GDI 渲染后端**。`src/platform/app_win32.*`（Win32 外壳 + `drawNodesGDI`，约 2600 行）
与 `tools/offscreen_render` 一并删除；像素基准图改由 **GL** 出（`gl_shot`，隐藏窗口 +
离屏 FBO）。三个 GPU 后端之间的逐像素约束不变。

### 为什么删（先量，再删）

它是 `docs/pixels/` 基准图的生成者。把它的基准图和 GL 渲的同一批页面逐像素比一遍，
差距大得没法当参照（阈值 8，即"肉眼能看出"的差异）：

| 用例 | 旧 GDI 基准 vs GL | 用例 | 旧 GDI 基准 vs GL |
|---|---|---|---|
| `opacity` | **25.00%** | `image-round` | 6.05% |
| `text-shadow-multi` | 15.57% | `svgjoin` | 5.36% |
| `svg-filter` | 14.02% | `svgstroke` | 4.77% |
| `text-shadow` | 13.95% | `svggrad` / `svgpath` | 4.29% / 4.26% |
| `image-mip` | 8.12% | `probe`／`display-none2`／`circle` | 2.57% / 2.29% / 1.92% |
| `svgfinish` / `svgaa` | 6.99% / 6.86% | `radius-matrix`／`grad-angle`／`shadow` | 0.88% / 0.72% / 0.40% |

也就是说：**基准图量的不是"真实渲染长什么样"，而是"最省的那条近似路径长什么样"**。
`pixels` 一直是绿的，只是因为 gold 与它同源；`pixels-vk`/`pixels-d3d11` 比的又是 GL，
所以 GDI 从来没进过"三端一致"这条判据。它缺 alpha 合成、缺字形图集、缺 AA 填充，
每补一项都是照着 GPU 端再实现一遍 —— 而它唯一的独有价值（无 GPU 也能出图）已经由
`presentViaGdi` + GL 离屏覆盖。

### 现在谁出基准图 / 门禁怎么变

- `docs/pixels/*.png`（18 张）**全部由 `gl_shot` 重新生成**，`make pixels` 变成
  "GL 自回归"，`pixels-update` 也走 `gl_shot`。
- `pixels-vk` / `pixels-d3d11`（与 GL 逐像素比）不变，仍是"三端一致"的硬门禁。
- `baseline_snapshot` 的后端列表收成 `gl / vk / d3d11`：快照 188 → **170 项**
  （少 18 条 `px gdi`）。`make baseline-render` 现在先 `rm -rf` 渲染目录 ——
  删掉一个后端之后，旧的 `gdi/` 产物本来会被下一次 `make baseline` 悄悄扫进来
  （这次就踩了：第一次重生成仍是 188 项）。
- `CMakeLists.txt` 删掉 `app_shell` 库与已经没人调用的 `add_binui_demo()`。

### 保留的 GDI 用法（不是渲染）

`IBinUIGfx::presentViaGdi()`（读回离屏像素 → `SetDIBitsToDevice`）保留：拖拽 resize、
远程会话、全屏直翻转时的**呈现**路径，规避 NVIDIA 驱动在 "resize + SwapBuffers" 下崩溃。
它不参与渲染，因此不影响任何像素判据。

### 验证

| 门禁 | 结果 |
|---|---|
| `make pixels`（新 GL 基准） | 18/18 **0 个不同像素** |
| `make pixels-vk` / `pixels-d3d11` | 全部 **0 个不同像素**（`anim/affine-text` 6 px 为既有差异） |
| `make baseline-strict`（170 项） | **完全一致** |
| `make test` | 全绿 |

---

## [0.23.0] - 2026-11-19 【格式 v34】

**P3 第一半：三个 GPU 后端的圆角模板收口到 bin2d**（`bin2d::RasterizeRoundedTileCoverage`，
bin2d 0.7.0）。GL / VK / D3D11 原本**各写一份**一模一样的解析式距离循环（30 行 × 3 份）
去生成"九宫格角片"那张灰度模板，现在只有 bin2d 里一份，binUI 只做"覆盖率 → 灰度 RGBA"。

### 这一步的判据是"像素一位不动"

| 门禁 | 结果 |
|---|---|
| `make baseline-strict`（188 项） | **完全一致**（像素 / 原语流 / 画质数字全同） |
| `make pixels`（18 个 GDI 基准） | 18 个用例 **0 个不同像素** |
| `make pixels-vk` / `pixels-d3d11` | 全部 **0 个不同像素**（`anim/affine-text` 6 px 为该用例既有差异） |
| 全量重编（清了 .o 之后） | **0 警告** |

`baseline-strict` 是这一刻最有说服力的那条：它把 GL/VK/D3D11 各 50 个用例的原始 ARGB
逐字节比了一遍 —— 三个后端渲染出的圆角边缘一位都没变。所以这次删掉两份实现是**纯减法**。

### Changed

- 删掉 GL / D3D11 的 `roundFor` 里的解析式循环与 VK `buildRound` 里的同名循环，改为
  `binui::BuildRoundedTileRGBA(radius, stroke, out)`（实现在 `src/backends/binui_paint.cpp`）。
- `BuildRoundedTileRGBA` 会核对 `RoundTemplateSize(radius)` 与 bin2d 的 `RoundedTileSize`
  是否一致 —— 不一致就返回 0 让调用方退回 `fillRect`，而不是悄悄把模板拉伸成错的大小
  （那会让圆角边缘整体偏一圈像素，而且只在某些半径上出现）。
- 顺手修掉 GL 后端 `drawDropShadow` 的 `unused parameter 'layerFbo'` 警告（它在本次改动
  之前就存在，只是那个翻译单元很久没有重编，全量重编才露出来）。现在全量重编 0 警告。

### 为什么"圆角模板"也算 bin2d 的活

它和 P2/P2b 的填充、描边是同一类东西：**覆盖率**。宿主要的是"这个像素被盖住多少"，
不是已经混好的像素 —— 三个后端各自把这张灰度图当纹理去 9 宫格拉伸（角片边长 = r、
中段取正中 1px），几何部分（`BuildRoundedNineSlice`）早就在 `binui_paint.h` 里共用了，
只有"模板内容"漂着三份。

注意它**不是**多边形光栅化：bin2d 也提供 `RasterizeCoverage`，但拿它拼一个圆角路径会得到
另一张图（折线圆弧 + 8 条子采样），三端像素就不再相同 —— 这条写在 bin2d 的头文件注释里。

---

## [0.22.0] - 2026-11-19 【格式 v34】

**P2b：`<path>` 的描边也换成 bin2d**（迁移的最后一块几何）。描边不再是"逐段 fillLine +
拐点补圆/多边形"，而是一张**并集覆盖率**：`bin2d::RasterizeStrokeCoverage`（bin2d 0.6.0）
出覆盖率，binUI 只把覆盖率拼成条带。

### 为什么必须是一张并集（不是三次优化里可选的那一次）

| 老做法 | 后果 |
|---|---|
| 逐段各自混合 | 相邻段在接缝处各混一次 → **半透明描边的每个接缝深一条**（真实存在的画质 bug） |
| 拐点补片走后端 `fillRounded` | 4× MSAA 采样位置在 GL/VK 上互为镜像 → 拐点/端帽本来就分岔，只是没被单独测过 |
| 斜接/斜面走硬边条带 | 尖角是硬边的，旁边的填充却是 AA 的 → 尖角像被切过 |

现在统一成"逐块栅格化、同一像素取 max、只混一次"。顺带把 GDI 端从"整数 HPEN + `Ellipse`"
换成同一份条带 —— **四端第一次真正用同一条路画描边**。

### 结果（同口径实测）

| 指标 | 迁移前 | 迁移后 |
|---|---|---|
| GDI vs GL（`svgjoin`） | 1284 个不同像素 | **0** |
| GDI vs GL（`svgstroke`） | 1016 个不同像素 | **0** |
| GDI vs GL（`svgpath` / `svggrad` / `svgfinish` / `svgaa`） | 843 / 102 / 545 / 105 | 87 / 58 / 265 / 88 |
| `make pixels`（新基准） | — | 18 个用例 **0 个不同像素** |
| `make pixels-vk` / `pixels-d3d11` | — | 全部 **0 个不同像素**（`anim/affine-text` 6 px / 0.002%，与本改动无关） |
| `make baseline-check` | 188 项 | 变化 **40 项**（GDI 6 + 三端各 11），全部是路径/描边用例 |
| `make gate-migrate` B / A2 | 0.98× | **0.98×**（这个场景不含描边，没有变化） |

快照变化的 40 项：GDI 端 6 个（`svgpath` `svggrad` `svgjoin` `svgstroke` `svgfinish` `svgaa`），
三个 GPU 端各 11 个（那 6 个 + `demo-svgdemo` 与 4 个带描边的动画用例
`svg-marker` `svg-pattern` `pattern-clip` `layer-stroke`）。文字/图片/圆角/阴影/九宫格**一个都没动**。
另外 `demo-svgdemo` 在 GL 与 VK 上**迁移前的哈希就不一样**（67bb… vs 8f81…），迁移后两端
收敛到同一个哈希（6f45…）—— 这条是"并集覆盖率把两端拉到同一条路"的直接证据。

### 基准图重生成（6 张，`make pixels-update`）

`svgpath` `svggrad` `svgjoin` `svgstroke` `svgfinish` `svgaa` —— 这 6 张基准图以前是被
**GDI 自己的 bug** 画出来的，迁移后修掉的两处：

- **圆头/圆角连接**：老 GDI 用 `Ellipse` 配错色画成**黑块**（`svgstroke` 圆头端点外 1px 处
  实测 (0,0,0)）；现在与 GPU 端同样的覆盖率。
- **方头虚线**：老 GDI 的 HPEN 是圆端帽，一段 28px 的虚线被两端各撑 5px → 缝隙被吃掉，
  4 段虚线画成一条连续横线；现在按 SVG 语义留出 2px 缝（`18 + 2×5` 对 `18+12` 周期）。

### Changed

- **删掉** `PaintPathStroke`（~80 行：逐段 `fillLine` + 拐点圆点 + 斜接/斜面多边形 + 方头
  端点外延）与它的三参数回调契约；`PaintPathNode` 的 `line` / `dot` 两个回调参数一并删除
  （四处调用点同步收口）。几何搬进 bin2d，binUI 只留 `BuildPathStrokeBands`
  （实现在 `src/backends/binui_paint.cpp`）。
- **描边条带有了跨帧缓存**：`PathBandCacheEntry` 加 `strokeBands`，与填充共用 key（同一节点、
  同一批输入），各存一份列表 —— 某一份超预算就不缓存那一份，不牵连另一份。以前描边靠
  `LineBandCache`（4096 条的逐段表，按线段几何做键），节点一多就会互相挤掉；现在按节点缓存。
- **撞条带上限的兜底从"行步长降级"改成"放大缓冲重试"**（`kMaxPathBands 4096 → 65536`
  按需翻倍）：P2 删掉老扫描线后 `rowStep` 已经没人用了，那条降级实际是空转（撞上限就只画
  一半）；现在按需翻倍，画不完才会真的截断。填充与描边都用这一条。
- 清理 P2 留下的两个死常量：`kMaxPathCrossings` / `kMaxRowSpans`。
- GDI 端（`src/platform/app_win32.cpp`）删掉描边的 HPEN/Ellipse 机器（`penFor` / `penW` /
  `penC`），改走同一份条带。

### 量出来的代价（不藏）

`make bench-path` 新增描边一节（并集口径 vs 老口径，纯 CPU）：

- 120px 三个图标（星形10点 / 齿轮16齿 / 圆64段，线宽 4）：新 **356.6 µs** / 旧 **147.8 µs** = **2.41×**；
  50 个图标 = 5.94 ms vs 2.46 ms。
- 成本来自"逐块各走一次行循环"：64 段圆描边 = 64 个四边形 + 64 个连接圆 = 128 块，
  而老口径只有 64 段（拐点是后端免费画的一个圆角矩形）。这是"不重叠、只混一次"的价格。
- 命中节点缓存的帧**没有变化**（纯重放），动画描边每帧付这笔钱。

### 新增测试

- bin2d：`test_raster` 30 → 43 项，新增 13 条描边覆盖率断言（平头直线导出矩形、square 端帽
  外延半线宽、round 端帽只多出亚像素、斜接尖角 vs 斜面/超限、**与 `StrokePath` 覆盖率
  逐像素一致（实测最大差 0 级）**、半透明接缝正好 50%、clip 生效）。
- binUI：`TestStroke` 从"断言回调几何（线段数/圆点数/多边形顶点）"改成**像素级覆盖率断言**
  （13 项），并新增"条带互不重叠"（= 并集，每个像素只混一次）与渐变描边"颜色按像素递增
  > 20 种"（老口径是逐段 ≤ 11 种）；`TestPathBandCache` 新增描边缓存命中/篡改重放两项。

### 附带修掉的一处历史差异

`docs/issues/assets/svgstroke.html` 里那两条虚线（`stroke-dasharray` + `square` 线帽）
以前 GDI 与 GPU 端画得不一样（见上"方头虚线"），基准图一直靠 0.5% 容差把它吞掉。
现在两端都按 SVG 语义画，容差可以不再负责这件事。

---

## [0.21.0] - 2026-11-18 【格式 v34】

**P2：`<path>` 的填充光栅化换成 bin2d**（迁移的主体）。binUI 不再自己扫线 ——
`bin2d::RasterizeCoverage`（bin2d 0.5.0，与 `FillPath` 同一个光栅器）出覆盖率，
binUI 只保留自己特有的那半段：**覆盖率 → 条带**（GPU 后端的 `fillRect` 四边形）。

### 结果（同口径实测）

| 指标 | 迁移前 | 迁移后 |
|---|---|---|
| `make pixels` 18 个 GDI 基准 | — | **0 个不同像素**（`svgaa` 最大通道差 1） |
| `make pixels-vk` / `pixels-d3d11` | — | **0 个不同像素**（D3D11 的 `shadow` 6 px / 0.002%，既有差异） |
| `make baseline-check` | 188 项 | 变化 **35 项**，全部是路径相关用例（见下） |
| `make gate-migrate` B / A2 | 0.98× | **0.91×** |
| `make bench-draw` svgdemo | — | 1431 原语 / ≈5924 四边形 / 694 KB（比迁移前低） |

快照变化的 35 项**全部**是 SVG 路径用例：`svgpath` `svggrad` `svgstroke` `svgjoin`
`svgfinish` `svgaa` `demo-svgdemo` 与四个带路径的动画用例（`svg-marker` `svg-pattern`
`pattern-clip` `layer-stroke`），每一端（GDI/GL/VK/D3D11）都是同一组。文字、图片、圆角、
阴影、九宫格等 **一个都没动** —— 这正是 P1 那套快照存在的意义。

### 为什么几乎逐像素不变（三个刻意的对齐点）

1. **竖直子采样数照抄**：老实现 AA 是 4 条子扫描线、硬边 1 条（行中心）。迁移后显式传
   `subSamples = 4 / 1`，采样位置逐点相同；
2. **硬边走 `CoverageStyle::HardEdge`**：端点先四舍五入到整像素再铺满 —— 不能在调用方
   把 0..255 自己二值化，否则亚像素宽的细条会消失（老实现是 round(端点) 铺整像素）；
3. **覆盖率 → alpha 的量化**：`round(cov × 255)` 与老实现 `(cov1024 × 255 + 512) / 1024`
   在 1024 定点上逐值相等，实测只在 `svgaa` 出现 1 级通道差（8bit 舍入在 .5 边界上的
   翻转），远在判据（通道差 8）之内。

### Changed

- **删掉**（约 380 行，随老扫描线一起走）：`PaintScanEdge` / `BuildPaintScanEdges` /
  `ActivateScanEdges` / 两条填充器的求交与累加 / 4 条子扫描线 + 1024 定点累加器
  (`covAcc`) / `rowStep` 降级 / `kMaxPathAAWidth = 4096` 的"太宽退回硬边"兜底。
  光栅化从此只有一份实现，在 bin2d 里。
- `BuildPathFillBands` / `BuildPathFillBandsAA` 变成**声明**，实现在
  `src/backends/binui_paint.cpp`（唯一依赖 bin2d 的翻译单元）。
- `BuildLineBandsAA` 与三个后端的 `fillLine` 调用点去掉 `covAcc` 实参。
- **bin2d 成了硬依赖**：`make` 时缺 `../bin2D/build-mingw/libbin2d.a` 会明确报错并提示
  `make -C ../bin2D lib`，不再静默少符号。

### 能力变化（都是变强）

- **不再有 4096px 宽度上限**：超宽路径以前退回硬边（锯齿），现在照常按 AA 画完。
  测试相应从"太宽必须返回 -1"改成"5000px 宽也按 AA 画完"。
- **条带上限撞到时的降级**：`rowStep` 参数保留只为签名兼容（bin2d 的覆盖率没有条带上限），
  实际不再走降级分支。

### 代价（诚实记账）

- **未命中条带缓存的最坏情况**（每帧全部形状重算）比迁移前慢 ~12%：老实现是"边扫一遍
  顺便合并条带"，现在是"bin2d 出覆盖率 → binUI 再走一遍拼条带"，多一次区域读写。
  真实帧里条带按节点缓存（key = 盒 + 颜料），命中路径完全不受影响。

### 版本

- 0.20.3 → 0.21.0（结构变化：一条渲染路径换了实现并删掉了旧路径）。

## [0.20.3] - 2026-11-18 【格式 v34】

**P1：迁移基线快照立起来了**（只加门禁与工具，**渲染代码一行没动**）。

### Added

- `tools/baseline_snapshot.cpp` + `make baseline` / `baseline-check` / `baseline-strict`：
  三端各渲一遍（GL / VK / D3D11 各 50 个用例 = 18 个像素用例 + 4 个整页示例 + 28 个
  动画中途帧用例；GDI 只有那 18 个用例，基准图只建了这些），每个用例每一端一个
  **FNV-1a 64**（算在原始 ARGB 上，PNG 编码器换了也不影响），连同
  `draw_bench --frames 1 --warm 0 --hash` 的 9 个原语流哈希、bin2d `make quality`
  的 11 行画质数字，压成 `tests/baseline/manifest.txt`（**188 项**，9.3 KB）提交进仓库。
- `tests/baseline/README.md`：口径、两级判据、什么情况下允许重生成快照。

### 为什么不是"再跑一遍 make pixels 就行了"

`pixel_diff` 只回答"在容差内吗"（0.5% 像素 / 通道差 8）。**一个真实退化完全可以躲在这条
线内** —— 整页文字从抗锯齿变成硬边只影响 0.3% 的像素，容差照样绿。P1 补的是另一半能力：
迁移每一步都能回答"动的正好是该动的那些吗"。

判据分两级：硬门禁仍是 `pixel_diff`；快照管"动了什么"—— 用例增删 / 尺寸变化直接失败
（结构变化），哈希变化默认只报告（迁移本来就该动几个像素），要求逐位相同时才 `--strict`。
三种失败模式都实测过：尺寸变 → exit 1，用例消失 → exit 1，只有哈希变 → 报告且 exit 0，
加 `--strict` → exit 1。

### 踩到的坑（写进 Makefile 注释了）

**recipe 的命令文本必须 ASCII。** mingw32-make 调 Git 的 sh 时，某些中文字符
（实测「没构建设」「（）」）会让 sh 报 `unexpected EOF while looking for matching '"'`，
整行命令解析失败、目标什么都不做，报错却只指向 Makefile 行号 —— 看起来像 Makefile 语法
错误，实际上是字符集问题。中文只放注释里，命令里的诊断信息改成 ASCII。

**原语流哈希不能收"自己会变"的页面。** `animshow` 的原语流由动画时钟（wall clock）决定，
同一份代码连跑三次给出三个不同哈希 —— 拿它当基线只会每次都报"变了"。已从哈希集合里
排除（像素快照那边没这问题：渲染工具是定帧的，两次全渲 168 项逐位相同）。

## [0.20.2] - 2026-11-18 【格式 v34】

**只有文档**：bin2d 0.4.2 把迁移开工条件做到了，本仓记录裁决与更新后的数字
（binUI 的渲染代码一行没动）。

### 结论（P0 关闭，进 P1）

```
场景：800x600 圆角卡片 + 200 个 120px 图标（119 668 条带/帧）
                     0.4.1 实测     0.4.2 实测
  A1 只建条带          7.30 ms        7.21 ms
  A2 条带 + 落像素      9.31 ms        9.32 ms
  B  bin2d FillPath    15.03 ms        9.22 ms
  B / A2               1.61× ❌        0.98× ✅（开工条件 ≤1.20×，目标 ≤1.00×）
```

### 文档

- `docs/migrate-to-bin2d.md`：§0 的结论改成"开工条件已达标、下一步 P1"；§1 换成两列
  对照表，并**修正根因描述** —— 消融实验（抄一份 bin2d 的 `raster.cpp` 做对照）显示
  "内部像素被乘了 8 倍"只占覆盖率开销的 1/3，真正的大头是"每条子扫描线给每条活动边
  各算一次 x"；P0 段落标为已完成并保留原计划原文作对照；时间线表更新。
- `docs/future.md`：bin2d 指针块补上达标后的数字。

### 为什么这轮没动 binUI 的渲染代码

规矩是"一条路径要么老实现要么新实现，不许共存"（`bin2D/docs/layering.md` §4），
而 P1 本身不改渲染（只立三端像素/哈希/质量基线快照）。开工条件达标不等于可以跳过基线：
P2 之后每步都要拿它比对，先立门禁再动手。

## [0.20.1] - 2026-11-18 【格式 v34】

**只有文档 + 一个测量工具**：把"binUI → bin2d"的迁移计划独立成文，并把**开工条件**量出来。

### Added

- `docs/migrate-to-bin2d.md`：迁移计划（P0 扫描线性能轮 → P1 基线快照 → P2 `<path>` 迁移
  → P3 圆角/投影/九宫格 → P4 图层收口 → P5 mip 收敛），每阶段都写了验收、删除条件与回滚线。
- `tools/migrate_gate.cpp` + `make gate-migrate`：**同口径**对拍 —— 同一份设备空间折线、
  同一块 1280×720 内存 Surface、都含清屏、都不含布局遍历：
  A1 只建条带、A2 条带+落像素、B bin2d `FillPath`。

### 结论（这条决定了下一步做什么）

```
场景：800x600 圆角卡片 + 200 个 120px 图标（119 668 条带/帧）
  A1 binUI 只建条带                 :  7.30 ms
  A2 binUI 条带 + 落像素（同口径）  :  9.31 ms
  B  bin2d FillPath                 : 15.03 ms
  B / A2 = 1.61×  → 未达标（开工条件 ≤ 1.20×）
```

所以**不开始迁 UI**，先进 bin2d 的扫描线性能轮。根因已定位：每个像素行跑 8 条子扫描线并对
整行宽累加覆盖率，内部像素（覆盖率恰为 1）也被乘了 8 倍；120px 图标单价 62 µs，是大形状
（1.45 ns/px）的 5 倍。改法是逐行解析覆盖率（直边在一行内的覆盖面积是梯形解析式）
+ 只在边界像素上细算。

### 文档

- `docs/future.md` 的 bin2d 一节指向新计划，并记下这条实测数字。

## [0.20.0] - 2026-11-18 【格式 v34】

**GL 后端改名：`binui_gl-gdi.*` → `binui_gl.*`。** 旧名从 0.6 时代留下来，那时真有一个 GDI
绘制/呈现后端挂在这个文件里；现在文件里**绘制全是 GPU 四边形**（GL 3.3 core + 顶点批次 +
字形图集 + 九宫格/FBO），字形光栅化是 FreeType + HarfBuzz，GDI 只剩三处**呈现**回退：
透明窗 `UpdateLayeredWindow`、拖拽 resize 的读回 + `SetDIBitsToDevice`、远程会话贴屏。
纯 GDI 后端一直是**另一份**实现（`src/platform/app_win32.*`：`BinUIApp` + `drawNodesGDI`，
遗留），所以 `gl-gdi` 这个复合名会让人误以为它是 GDI 后端。

改名清单（源码级；C ABI 不变，绑定不受影响）：

| 旧 | 新 |
|---|---|
| `src/backends/binui_gl-gdi.{h,cpp}` | `src/backends/binui_gl.{h,cpp}` |
| `BinUIGLGdi` | `BinUIGL` |
| `BinUIAppGLGdi` | `BinUIAppGL` |
| `drawNodesGLGdi` | `drawNodesGL` |
| 窗口类名 `L"BinUIAppGLGdiWindow"` | `L"BinUIAppGLWindow"` |
| `placeCaretAtClickGLGdi` | `placeCaretAtClickGL` |

- `BinUIGLGdi` / `BinUIAppGLGdi` 留了 `using` 别名（源码兼容一版，新代码用新名）；
  `drawNodesGLGdi` 不留别名——带默认参数，包装一层不值，直接改名。
- 构建侧：对象 `binui_gl.o`（Makefile 各 demo 链接规则 / CMake `gl_shell`）、`gl_shot`、
  `partial_shot`、`binui_ui.h` 的后端选择、`binui_charts.h` 的前置声明与
  `LineChart::draw(BinUIGL&)`。
- 文档侧：README（目录树 / 后端表 / 拖拽 present 说明顺带更正为 `SetDIBitsToDevice`）、
  CONTRIBUTING、`docs/bindings.md`、`docs/future.md`、`docs/svg.md`、`docs/vulkan*.md`、
  `docs/startup.md`（resize 表里 `GDI/GL` → `GL`）、`docs/keyboard_focus.md`（原来写
  "IME 开关只有 GL/GDI 外壳有"，实际三端共用 `BinUIAppT`，一并改正）、issue 模板、
  `tools/gl_shot.cpp` 注释。
- `CHANGELOG.md` 与 `docs/issues/*` 里的**历史条目保持原样**（当时的文件名就是那个）。

理由一句话：名字要能预测"改哪里"。想改 GL 呈现的人不该先去 `app_win32.cpp` 里找 GDI 代码。


## [0.19.2] - 2026-11-18 【格式 v34】

**记录一次"看起来该省、实测不省"的优化：VK 拖拽期不重建交换链。**（无行为变更）

VK 的 `gfx.resize` 一直是 4.2 ms，GL 0.33 / D3D11 0.34。拆开量（`BINUI_RESIZE_TRACE=1`）：

| VK resize 内部分段 | ms |
|---|---|
| `vkDeviceWaitIdle` | 0.02 |
| surface 查询（caps / formats / presentModes，已缓存） | 0.02 |
| **`vkCreateSwapchainKHR`（带 oldSwapchain 复用）** | **2.8~3.3** |
| 旧链销毁 | ~0.2 |
| **重建 MSAA 4× / resolve / readback 三张图** | **1.2~1.5** |

GL / D3D11 便宜是因为它们**原地换尺寸**：GL 的 `glTexImage2D` / FBO 复用已有存储，
DXGI 的 `ResizeBuffers` 是延迟分配，都不需要重新协商"呈现目标"。

于是试了标准做法：拖拽期间不重建交换链，只重建渲染目标，present 时把新尺寸的图**缩放**
blit 进旧链（松手后再精确重建一次）。`gfx.resize` 确实从 3.95 ms 降到 **1.58 ms** ——

**但整帧没有变快**：`呈现` 从 11.4 ms 涨到 13.6 ms，整帧仍是 16.0 ms / 60 fps。
省下的 3 ms 全跑到 present 上了（交换链尺寸与窗口不符时，DXGI 的呈现路径更贵）。
所以这条优化**没有合入**，代码回到"每帧重建"，只把这个结论写在这里，省得下次再试一遍。

顺带留下的诊断（`BINUI_RESIZE_TRACE=1`）：`[vk][targets] 重建 WxH（swap WxH）`、
`[vk][gdi] 读回 WxH`、`[vk][blit] WxH ret=… 客户区=…`、`[vk][resize] waitIdle / swapchain / targets`。

结论：**VK 拖拽 resize 的 4.2 ms 就是"每帧重建交换链"的价格，跑在 60 fps**（稳态 120 fps
不受影响，因为只有 resize 帧才付这个钱）。要更快只能改呈现策略（例如拖拽期显示缩放版），
实测不划算。


## [0.19.1] - 2026-11-18 【格式 v34】

**VK：窗口拉到"铺满显示器"后画面停在上一帧、其余全黑（0.19.0 漏掉的第三种 resize）。**

### 现象与定位

VK 窗口 resize 到刚好盖住整块显示器（2560×1440）后画面不更新、新增区域全黑；同一尺寸
GL 与 D3D11 都正常。0.19.0 只验证到 820×560 与 2500×1350（都没到"铺满"），所以漏了。

打点证明整条链都是对的：

```
[app][resize] apply 2560x1440（gfx ready=1）
[fx][resize]  setSize(2560,1440) -> root=2560x1440
[vk][targets] 重建 2560x1440（swap 2560x1440）
[vk][gdi]     读回 2560x1440
[vk][blit]    2560x1440 ret=1440（全部扫描线成功）客户区=2560x1440
```

画对了、读回来了、blit 也返回成功 —— **屏幕上就是不更新**。

### 根因

Windows 上 VK 的 WSI 一旦为窗口建过交换链，这个窗口就是 flipping 窗口，DWM 只合成
交换链里的图；GDI 往窗口 DC 画的内容根本不上屏。而 `clientCoversMonitor()` 这条守卫
（本意是避免 DWM 直翻转停帧）恰好把 VK 从交换链**踢到了**"读回 + GDI 贴屏"，
于是画得再对也看不见。GL 没有交换链，它的 GDI 路径一直是好的 —— 这就是"只有 VK 出问题"。

### 修法

* `present()` 的 `gdiPath` 去掉 `coversMonitor` 条件；
* `presentViaGdi()` 在有交换链、非透明、非远程会话时直接走交换链；
* 远程会话仍走读回（虚拟显示器上交换链会停帧，是 P4 的实测结论），透明窗本来没有交换链。

判断标准是**这个窗口当前是不是由交换链在呈现**，不是"哪个后端"。

### 验收

* VK 全屏 2560×1440 截图与 GL / D3D11 逐像素对比：除任务栏时钟（截图时刻不同）外一致；
* `make vk-window`（`--fullscreen` 现在报告 `present=交换链`）、`make vk-stress` 全过；
* `pixels` 18/18、`pixels-vk` 0 差、`pixels-d3d11` 仅既有 `anim/affine-text` 6 px、
  `partial`、`check_examples` 16/0、`check-guard`、`capi`、`resize-bench` 全绿。
* 新增诊断（均受 `BINUI_RESIZE_TRACE=1` 控制）：`[vk][targets] 重建 WxH`、
  `[vk][gdi] 读回 WxH`、`[vk][blit] WxH ret=… 客户区=…` —— 这次就是靠它们才分清
  "画错了 / 读回错了 / 贴屏没上屏"。


## [0.19.0] - 2026-11-17 【格式 v34】

**拖边框 resize：VK 每次 26 ms、D3D11 的交换链从来没真换过（还连带着视口不换）。**

本版不动渲染语义：`BINUI_VERSION` 仍 v34，三个 GPU 后端的离屏输出逐像素不变。

### 一、先给"拖边框"一个能重复的尺子

新增 `BINUI_RESIZE_BENCH=n`（共享外壳 `binui_app.h`）与 `make resize-bench`：每帧用
`SetWindowPos` 把窗口宽度抖 ±40px，逼出"每个 `WM_SIZE` 都重建交换链/后备缓冲"这条最坏
路径，画满 n 帧后**布局耗时**与**后端 `gfx.resize` 耗时**分开打印，然后正常退出。
本机 1100×720、60 次：

| 后端 | 修前 `gfx.resize` avg | 修后 avg | max |
|---|---|---|---|
| GDI/GL | 0.46 ms | 0.46 ms | 0.76 |
| **Vulkan** | **26.67 ms** | **4.68 ms** | 17.31 |
| **D3D11** | 0.09 ms（假的，见下） | 0.31 ms | 0.48 |

### 二、D3D11：`ResizeBuffers` 每次都失败（`0x887A0001`），交换链永远停在旧尺寸

失败是**瞬间返回**的，所以耗时看起来最漂亮（0.09 ms），实际什么都没做：画面被系统拉伸
或裁切。根因是调用时我们还持有着后备缓冲的 RTV，而且它仍绑在管线上。修法：先
`OMSetRenderTargets(0,…)` → 释放 RTV → `Flush()`（把在飞的绘制踢完）→ `ResizeBuffers`。
修后实测后备缓冲确实变成新尺寸（`BINUI_RESIZE_TRACE=1` 打印 `后备缓冲 820x560（期望
820x560）`）。

### 三、D3D11：resize 后**视口没换**，画面被放大 1.34 倍并裁掉右/下

`RSSetViewports()` 只在 `createState()` 里设过一次，`BinUID3D::resize()` 没有重置它。
窗口变小后 NDC 仍被映射到旧尺寸，于是整幅画面按 `1100/820≈1.34` 放大、右/下被裁 ——
看起来像"布局没重排"，实际布局是对的。**GDI / Vulkan 都没有这个毛病**（各自在 resize
里换视口），所以只跑 `pixels-vk` 抓不到。修法：resize 里换视口，`beginFrame` 每帧再对一次
（层渲染/模糊临时通道会改它）。

### 四、Vulkan：resize 从 26.6 ms 降到 4.68 ms

`createSwapchain()` 原来是"先销毁再新建"，且每次都重新枚举 surface caps / formats /
presentModes。拆开量：查询 3.3~4.4 ms、`vkCreateSwapchainKHR` 3~5 ms、销毁旧链 ~4 ms。
三处一起改：

* 用 `ci.oldSwapchain = 旧链`（Vulkan 推荐的 resize 路径，驱动可复用底层资源），创建
  成功后再自行销毁旧链；失败路径用 RAII 保证旧链一定被销毁，不漏；
* surface 能力（caps / formats / presentModes）**缓存**，只在建 surface 与重建失败时重查；
* 交换链尺寸改用"我们要的窗口尺寸"，不再照抄缓存里的 `caps.currentExtent`。
  照抄缓存会按旧尺寸建链、随后 present 前发现不一致再建一次，一帧建两条链（实测踩到）。

顺带：present 前那次**每帧**的 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 实测 ~0.7 ms
（120Hz 帧预算的 8%），改成先比 `GetClientRect`（~0.01 ms），只在真不一致时才查 caps。

### 五、验收

* `make resize-bench`（新增门禁）：跑三个后端，任何 `ResizeBuffers hr=` / `交换链重建失败`
  / `重建目标失败` / `Present hr=` 直接失败；
* 三个后端的窗口都改成 820×560 后截图互比：VK↔GL 80 px、VK↔D3D 80 px（都是同一行 1px
  的滚动条位置），修前 VK↔D3D 是 **19.4%**；
* 原有门禁：`pixels` 18/18、`pixels-vk` 0 差、`pixels-d3d11`（仅既有 `anim/affine-text`
  6 px）、`partial`、`check_examples` 16/0、`check-guard`、`capi`。


## [0.18.0] - 2026-11-17 【格式 v34】

**启动耗时：把"首帧之前"逐段量出来，并修好一把会骗人的尺子。**

（本版没有渲染改动：`BINUI_VERSION` 仍是 v34，三个 GPU 后端的输出与 0.17.6 逐像素相同。）

### 一、修尺子：PowerShell 重定向 stderr 会让每次 `fflush` 花掉 1.1 秒

0.17.6 报给用户的段数是**用 `Start-Process -RedirectStandardError` 量出来的**，同一份
二进制只换收集方式：

| 收集方式 | `UiApp ctor` 读数 |
|---|---|
| 控制台直连 | **3.5 ms** |
| `Start-Process -RedirectStandardError file` | **1207 ms** |

差别不在程序，在子进程 stderr 被重定向后**每次 `fflush(stderr)` 要 ~1.1 s**（写穿透 +
杀软逐次扫描）：三段日志凭空多出 3 秒。所以探针改成两条路：

* `BINUI_STARTUP=1` 仍写 stderr（交互式看方便）；
* `BINUI_STARTUP_FILE=<path>` 用**自己的 `fopen("ab") + fwrite + fclose`** 写文件（一次
  ~0.1 ms），量启动一律走这条；
* 时间戳在写日志**之前**取，日志开销不计入本段。

### 二、分段：窗口创建 / GL 上下文 / 首帧各占多少

新增里程碑（`open: DPI+class`、`open: SPI_GETWORKAREA`、`open: CreateWindowEx`、
`frame1: begin / beginFrame / draw_ / present done`，GL 后端再加 `GL context / LoadGL /
shaders / state+framebuffer / font atlas`）。本机 flexshow 实测到首帧 **662 ms**：

| 段 | ms | 归属 |
|---|---|---|
| 编译 + 装载布局 | 3.5 | binUI |
| `CreateWindowEx` | **200** | 系统（进程内第一个窗口的一次性成本） |
| GL 上下文 | **145** | 显卡驱动 |
| GLSL 编译 | 15 | 驱动 |
| GL 状态 + 帧缓冲 + 字体图集 | 3 | binUI |
| 首帧遍历 + 顶点上传 | 31 | binUI |
| 首帧 `SwapBuffers` | **227** | 驱动 / DWM |

**binUI 自己只占 ~35 ms（5%）**。三项大头是**一次性**的：最小对照实验里，裸
`CreateWindowExW` 在同一个进程里第一个窗口 156 ms、第二个 23 ms；`[perf]` 显示
`呈现 avg=11.67 max=227.18`（只有第一帧异常）。所以这里没有可优化项 —— 把一次性成本
挪位置（先隐藏建窗、GL 就绪再显示）总时长不变，只是把"白窗等待"换成"晚 500 ms 出窗"。
对照实验留在 `tools/win_create_time.cpp`，完整数据见 [docs/startup.md](docs/startup.md)。

### 三、`BINUI_FRAME_EXIT=n`：画满 n 帧自己退

自动化量测以前靠 `Stop-Process -Force` 强杀，缓冲里的诊断会连带丢掉。现在：

* 设了它就直接走 `requestExit()`，**正常退出**（`wglDeleteContext` + CRT 退出都跑完）；
* 静态页面本来只画 1 帧（没脏区不出帧），所以 n>1 时外壳会 `invalidateAll()` 强制重绘，
  "n 帧"才真的会发生；
* `BINUI_PERF=1` 的短跑也会在 `close()` 里 `PerfFlush()` 打印一份统计（不再只有 exit 一行）。

已知的一个读数陷阱也写进文档：设 `BINUI_FRAME_EXIT=1` 时，进程墙钟比"首帧画完"多
220~265 ms —— 那是驱动卸载与 CRT 退出，不是画面没画完。

## [0.17.6] - 2026-11-17 【格式 v34】

**flexshow 拖窗口变矮时"缩成一坨"，以及启动慢的分段测量。**

### 一、resize 塌陷：`flex:1` 的项会被压到 0

实测（`Renderer::setSize` 后 dump 节点尺寸，1100×720 → 360）：

| 节点 | 720 时 | 360 时 | 问题 |
|---|---|---|---|
| `#main`（图表行） | 437 | 77 | 整块被压 |
| `#chart`（柱子容器） | 379 | **19** | 柱子按百分比，38% 只剩 7px |
| `#list`（滚动视口） | 367 | **7** | 一行都看不见 |

根因**不是**排版偏差，而是与浏览器的一处真实差异：浏览器里 flex 项的 `min-height` 默认是
`auto`（= 不小于自己的内容），所以容器变矮最多把项压到内容高度、再多就溢出；**binUI 没实现
这个 auto**，默认无下限 —— 于是"窗口一矮，所有 `flex:1` 的区域一起被压穿"。
注意滚动容器（`overflow:auto`）按规范本就该 `min-height:auto = 0`，所以 `#list` 被压到 7px
是符合规范的；真正该兜住的是 `#chart` 这类非滚动区域。

**修法（示例侧，不改引擎）**：给必须留出可用高度的区域写死下限 ——
`#main` 280 / `#chart` 130 / `#list` 120。实测 720×1100 下**布局一位不变**（分配 437 > 280），
360/240 下不再继续压（`#main` 保持 280、`#chart` 222、列表视口 210），窗口再矮就整页溢出、
底部被窗口裁掉 —— 比"每块都压成一坨"好。实现 `min-height:auto` 需要两遍布局（先量子项内容高
再分配），已记进 `docs/future.md` 待办。

新增 `test_css` 16.58 段（4 条断言）钉住这个差异：没写 `min-height` 的 `flex:1` 项在空间不足时
被压到 0；写了 `min-height:50` 的项守住下限（容器因此溢出）；`setSize` 之后两条仍成立 ——
将来实现 auto 时这两条会改成"不小于内容高"，改动点因此是显式的。

### 二、启动慢：分段探针 `BINUI_STARTUP=1`

`flexshow` 双击后约 **540 ms** 才出窗口。新加的分段探针（`binui::ex::StartupNowMs` /
`StartupMark`，在 `include/binui_paths.h`；`BinUIApp::run` 里也埋了"建窗/首帧"两点）实测：

| 阶段 | 耗时 |
|---|---|
| `main()` 进到 `UiApp` 构造完成（编译 + 装载布局 + 构造） | **3.4 ms** |
| 整个进程（`BINUI_BOOT_EXIT=1`，不进事件循环） | 76 ms |
| 窗口真正可见 | **540 ms** |

也就是说 **~460 ms 花在 `run()` 里"建窗 → 首帧"这一段**（GL 上下文创建 + 字形图集首次上传 +
首帧绘制），main 里的编译与布局完全不是瓶颈。示例里还加了 `BINUI_BOOT_EXIT=1`（停在事件循环前，
只量启动路径）。

## [0.17.5] - 2026-11-17 【格式 v34】

**把 v0.17.4 那条滚动列表收缩规则钉成单元测试，并补上 `min-height` 的等价关系。**

v0.17.4 只改了示例写法（`.row { flex-shrink: 0 }`），没有测试兜底 —— 而这条规则涉及
"编译期布局"和"运行期 `setSize` 重排"两条路径，任一条漏掉都会重现"行被压扁、文字溢出"。

### 新增 16.57 段（4 条断言）

一个 `display:flex; flex-direction:column; height:40; overflow:auto` 的容器里放两个
`height:32` 的行（一个写 `flex-shrink:0`、一个不写）：

- 写了的那行在溢出时**保持 32**；
- 没写的行照 CSS 缩（对照组 —— 防止"把收缩整个关掉"这种糊涂修法）；
- `setSize(300,300)` 之后两条规则仍然成立。

### 文档补充

`docs/ui_orchestration.md` 补一句：`min-height: 32` 与 `flex-shrink: 0` **等价** —— 收缩会
"扣到 min 为止"，同一页两种写法实测渲染出来**逐字节相同**（sha256 一致）；语义上
"这一项不许缩"用 `flex-shrink: 0`，"至少保留这么高"用 `min-height`。

顺带纠正我自己的一次误判：中途怀疑过"CSS 规则块里的 `/* */` 注释会把后面的声明吃掉"，
实测**不成立**（`CssStripComments` 在解析前就剥掉了，带注释与不带注释的页面逐像素相同），
真正的差异只在 `flex-shrink` 那一行。

### 验证

`make test` 全 PASS（16.57 段 4/4）。

## [0.17.4] - 2026-11-17 【格式 v34】

**修 flexshow 的滚动列表：`flex:1 + overflow:auto` 里的行被压扁、文字溢出。**

现象：窗口拉矮时，那张"② flex:1 + overflow:auto"卡里 12 行条目被压到十几像素，
而行里的 16px 文字没跟着缩 —— 文字溢出到相邻行，看上去像被纵向切掉一半；更怪的是
**滚动条仍然出现**（滚动范围按未压缩的 32px 算），于是"既压扁又出滚动条"。

根因不是排版引擎的偏差：CSS 里 flex 子项默认 `flex-shrink: 1`，容器高度不足就会收缩。
`include/binui_flex_layout.h` 一直按 `shrink × basis` 加权收缩（这行为是对的），
问题是示例没写 `flex-shrink: 0` —— 于是行被压扁，而滚动范围又按声明高度算。

### 做法

- `examples/flexshow/main.html` 的 `.row` 加 `flex-shrink: 0`：行保持 32px，视口外的部分
  靠滚动，这才是滚动列表该有的样子。
- 顺手改掉示例头部那条**过期提醒**：原文写"flex-shrink 未实现：加 min-width 保护"，
  而它 v22 起就实现了（`docs/ui_orchestration.md` 里一直写着"默认 1、真参与收缩计算"）——
  这条错提醒正是这次踩坑的原因。
- `docs/ui_orchestration.md` 补一条规则：`display:flex; flex-direction:column; overflow:auto`
  的滚动列表**必须显式关掉收缩**，并写清"压扁 + 仍有滚动条"这个组合症状。

### 验证

- 新判据页 `docs/issues/assets/flex-scroll-shrink.html`（同尺寸左右对照：左边不写
  `flex-shrink:0`、右边写了）：三端 **0 不同像素、最大通道差 0**；
- `examples/flexshow` 在 1100×720 与 341×274 两个尺寸下重渲染确认（后者就是原来塌掉的尺寸）：
  行恢复 32px、文字完整、滚动条正常；
- `check_examples` 16 例 0 问题。

### 同时修掉的度量缺口（bench）

`tools/draw_bench.cpp` 以前把 `fillLine` 记成 **1 个四边形**，而三个 GPU 后端自 v0.17.1 起
会把一段线拆成 2~8 个 CPU 覆盖率条带 —— svgdemo 估 2456、实测 **6985**（2.8 倍），
cubedemo 估 329、实测 **2081**（6.3 倍）。现在它调同一份 `binui::BuildLineBandsAA` 数条带
并复用同一套条带缓存（命中只重放），"遍历 µs"与真实后端一致（svgdemo 18.5 µs，
不加缓存会是 178 µs）。`docs/future.md` 的两张性能表按实测重写。

## [0.17.3] - 2026-11-17 【格式 v34】

**毛玻璃 / `filter: blur` 的降采样链按尺寸池化。** 以前三个后端都只维护**一套**链，
第 0 级尺寸由第一个毛玻璃节点决定；同帧出现第二个不同尺寸的节点就把整条链
（8 级纹理 + 目标/视图）删了重建 —— 而"大玻璃卡 + 小玻璃按钮 + 模糊浮层"同页很常见，
等于每帧几十次纹理创建/销毁。零格式改动、零像素变化。

### 做法

- 三端各加一个 `BlurSet` 池：key 是**第 0 级（抓取）尺寸**，命中直接复用，最多留
  `kMaxBlurSets = 16` 套，超出淘汰最久未用（VK 的淘汰走既有的 `retiredBlurTex` 两代
  延迟释放 —— 命令缓冲到 present 才提交，当场删图就是悬空引用）。
- 所有建 level0 的地方先 `selectBlurSet(rw, rh)`；`ensureBlurLevel` 与 `gaussBlurLevel`
  改成操作"当前活动套"。GL/D3D11 的临时链清理、VK 的纹理布局记账都跟着套走。
- **踩到的坑**：VK 的 `blurBackdrop` 里那处 `ensureBlurLevel(0,...)` 带花括号（失败要
  `resumeTarget()`），批量替换的正则只匹配了"单行 return"的写法，于是 VK 漏掉
  `selectBlurSet` → `activeBlur` 一直是 -1 → 第一个玻璃节点直接返回 false，
  `anim/glass` 里 11374 个像素没糊（GL/D3D11 正常）。教训：**跨三端改同一处调用约定时，
  别用文本替换，逐个点确认**。

### 验证

- 新增判据页 `anim/glass-multi-size`（一页 4 种尺寸毛玻璃 + 2 种尺寸 `filter:blur` +
  一个 grain 玻璃，背板是渐变加条纹，糊没糊一眼可见）：三端 **0 不同像素、最大通道差 1**；
- **量测**（`BINUI_BLUR_TRACE=1` 数 `ensureBlurLevel` 里真正创建纹理的次数，`--bench 60`）：
  该页 60 帧共 **31 次**创建（只有第一帧建 7 套链），改前是每帧 ~34 次（60 帧 2046 次）
  —— **66 倍**；
- `pixels-vk` 全部用例 0 个超容差像素（含既有的 `anim/glass`）、`pixels` 18/18 零差异、
  `pixels-d3d11` 只剩既有的 `anim/affine-text` 6 像素。
- 剩余边界：`gaussBlurLevel` 的 7 号暂存仍按"当前级别尺寸"复用，同一套链里不同级别的
  节点交替时它会重建一次（`anim/glass` 实测仍有 ~4.7 次/帧，与池化前一致）。
- 证据图：`docs/issues/assets/glass-multi-size.png`。

## [0.17.2] - 2026-11-17 【格式 v34】

**`<pattern>` 的内容现在裁到自己的 tile 框（SVG 规范：tile 框外不画）。**
以前是"内容的形状别越界"，越界的部分会盖到相邻 tile 上、拼出规范之外的图案。零格式改动。

### 做法

- 编译期做**凸窗口裁剪**（Sutherland-Hodgman，`ClipShapeConvex` 放在 `include/binui_path.h`
  与其它路径工具一起）：tile 框在设备空间要么是矩形（轴对齐）、要么是平行四边形
  （`patternTransform` 带旋转/斜切），两者都凸，所以一套算法够。
- tile 框的变换取 `svgX · [tf] · T(格原点)` —— 不含内容映射 `cb`，因为 `cb` 正是把内容
  映射**进** `(0,0)-(txa,tya)` 的那一段。
- **只对真的越界的内容裁**（`ShapeInsideConvex` 先判一次）：正常写法（内容不越出 tile）
  走的是原路，像素一位不变 —— `pixels` 里既有的 `svg-pattern` 基准图零差异。

### 已知代价（写进 `docs/svg.md` 14.3）

- 这是**几何**裁剪、不是渲染后裁剪：被切出来的新边会跟着描边画出来，tile 边界上多一条线。
  所以"越界 + 有描边"会点名告警。
- 凹轮廓被切出来之后沿窗口边界会有零宽的"桥"（面积 0）：nonzero 填充下不可见，
  even-odd 下与"自交轮廓"是同一类问题。

### 验证

- `test_css` 新增 16.56 段（7 条断言）：完全在内 -> 判定为内且点数不变；跨窗口 -> 点全在内、
  面积等于交集（400）；完全在外 -> 裁完为空；平行四边形窗口 -> 落在窗口里的小块判定为内、
  越界大块裁后面积不超过窗口面积（1600）；
- `pixels-vk` 新增 `anim/pattern-clip`（越界圆 / 不越界圆 / 越界横条 / `rotate(30)` 越界圆 /
  旋转对照）：**三端 0 不同像素、最大通道差 0**；
- `pixels` 18/18 零差异（既有 pattern 用例内容不越界，走原路径）、`pixels-d3d11` 只剩既有的
  `anim/affine-text` 6 像素。
- 证据图：`docs/issues/assets/pattern-clip.png`（左边每组的越界内容已经只画 tile 内那部分，
  右边是不越界的对照）。

## [0.17.1] - 2026-11-17 【格式 v34】

**收掉文档里挂着的两条"未定位差异"，根因是同一个：4x MSAA 的采样子像素位置由 API 决定，
OpenGL 与 Vulkan 在同一块 GPU 上互为镜像 —— 斜向的细长四边形覆盖率会差一档（25%）。**
描边几何以前就是"一根细长四边形靠 GPU 出 AA"，于是：

- `<circle>` 描边（圆是折线，整圈都是斜的）在**不清裁剪、不套层**时就已经差
  320 个像素、最大通道差 123；
- 浅斜线（120x20，9.5 度）整条差 62/255，正好一档 4x MSAA；
- 轴对齐线、45 度线、斜三角形填充、圆角矩形、小数坐标矩形都是 **0 差异** ——
  它们的覆盖数在两种采样位置下恰好相同。这也解释了为什么文档里那条一直叫
  "裁剪层里的**奇数宽度**描边"：真正相关的是"边缘斜穿像素"，而裁剪层和奇数宽度
  只是当时凑巧能复现的组合。

### 做法

- 三个 GPU 后端的 `fillLine` 改成 **CPU 覆盖率条带**：复用填充那套
  `BuildPathFillBandsAA`（每像素行 4 次纵向子采样，覆盖率相同的连续像素合并成一条），
  三端画的是同一份条带。采样位置不可设置（OpenGL 没有对应 API），这是唯一能让
  "三端逐像素相同"在斜线上也成立的办法。
- 仿射（rotate/scale）下的描边**仍走原来的四边形**：条带是轴对齐矩形，过矩阵后
  相邻条带之间会露缝，质量反而更差；仿射路径另有像素门禁用例兜底。
- 轴对齐**也没有**留 GPU 快路径 —— 本来想当然，实测翻车：竖线 `stroke-width=3` 落在
  x=150、边缘恰好在 148.5 / 151.5 这两条**像素边界**上时，GL 与 VK 在
  (148,68) / (151,68) 差 55。
- 代价靠**条带缓存**压回去：`MakeLineBandKey`（几何 + 颜色）配一张 thread_local 表，
  静态页面每帧命中、动画每帧 miss（动画本来每帧都要重算）；表满 4096 项整体清空。

### 验证

- `test_css` 新增 16.55 段（10 条断言）：覆盖率加权面积 = 线长 x 线宽（水平 / 45 度 /
  浅斜线三种）、条带保持 RGB 与 alpha 有效、同一线段重复计算逐位相同、
  45 度与 -45 度面积一致（对称性 —— 采样位置问题正出在这里）、退化输入不产生条带、
  缓存 key 区分线宽与颜色；
- `pixels-vk` 新增 `anim/layer-stroke`（w1/w2/w3/w5/w7、整数与小数 y、斜线、折线、圆、
  自定义路径裁剪、嵌套裁剪，外加同款不裁剪对照）与 `anim/layer-shadow`（组透明度图层里的
  box-shadow，复刻原始三条触发条件：半透明卡底 + 大模糊低 alpha 阴影 + 渐变底）：
  **三端 0 不同像素、最大通道差 0**（改前 `layer-stroke` 场景最大通道差 119、
  圆描边单例 320 个像素差 / 通道 123）；
- `pixels` 18/18 零差异（GDI 是遗留后端，描边实现没动，所以基准图不用重出）；
- `pixels-d3d11` 只剩既有的 `anim/affine-text` 6 像素；
- `partial` OK、`check_examples` 16 例 0 问题、`make check-guard` 0 报告；
- 性能（`gl_shot --bench`）：svgdemo **0.078 -> 0.136 ms/帧**（不加缓存是 0.335，
  纯 CPU 覆盖率 4.3 倍，缓存压掉六成）；不含描边的 features / appshow / animshow
  与改前一致（0.070 / 0.079 / 0.092 ms）。
- 证据图：`docs/issues/assets/layer-stroke-before.png`（同一页面在改前二进制下的 GL 结果）
  与 `layer-stroke.png`（改后，三端逐像素相同）。

## [0.17.0] - 2026-11-17 【格式 v34】

**把 `0.16.46` – `0.16.54` 这九次宣读收编成一次正式发布，同时纠正版本号的用法。**
那九次里八次是**新增能力**（filter 原语链、九宫格四种平铺、多层 `text-shadow`、
`patternTransform`、遮罩渐变、`mask-type`……），按 [CONTRIBUTING.md](CONTRIBUTING.md)
的表格它们全属于 `MINOR` 而不是 `PATCH`。压在 `PATCH` 位的后果是
`0.16.0 → 0.16.54`：四天 55 个号、其间 47 条是功能，版本号于是不再表示"这次变了多大"。
从本版起回到规矩：**新能力进 `MINOR`、只有纯修复才动 `PATCH`**，同一天的多项改动攒成一个
发布，不再一个改动一个号。

### 收编进本版的能力（原 `0.16.46` – `0.16.54`）

| 原版本 | 内容 |
|---|---|
| 0.16.46 | 修 `check_examples` 偶发 `0xC0000005`（编译器布局栈里一处悬空引用）；新增 `make check-guard` 电篱笆分配器门禁 |
| 0.16.47 | SVG `filter="url(#f)"`：原语链在编译期归约成"整组模糊 + 身后的彩色影子"，运行期与格式零改动 |
| 0.16.48 | 扫描线填充改用活动边表（AET）：50 个 120px 图标 AA 填充 3.29 → 1.35 ms、硬边 0.83 → 0.30 ms，输出逐位不变 |
| 0.16.49 | 九宫格图片中段平铺 `stretch / repeat / round`（末块按比例裁切、源 UV 同裁） |
| 0.16.50 | 多层 `text-shadow`（上限 4 层）：每层独立偏移/模糊/颜色，发射顺序"层在外字在内" |
| 0.16.51 | 九宫格 `space` 平铺（四种模式补齐）：块不缩放不裁切，余数均分成 `cnt+1` 个缝隙 |
| 0.16.52 | `<pattern>` 的 `patternTransform` 整份生效（旋转/斜切不再按轴对齐近似）；预算改数"真的会画的块" |
| 0.16.53 | 遮罩内容支持渐变（格式 v34）；顺带修 v28 起 `maskUnits="userSpaceOnUse"` 的 width/height 被误乘元素盒 |
| 0.16.54 | 遮罩 `mask-type="luminance \| alpha"`：alpha 型不折亮度、只取内容自己的 alpha |

### 版本号一致性

- `binui_version()` 与 `CMakeLists.txt` 的 `project(... VERSION)` 本版对齐到 `0.17.0`：
  CMake 那边此前一直停在 `0.9.1`，比库自报的版本落后七个 `MINOR`——发版清单第 1 项漏改，
  而漏改的检测方式是"看 CMake 的 `STATUS "binUI v..."` 输出"，没人看就没人发现。
- `bindings/rust/Cargo.toml` 的 `version` 是 Rust 绑定的**自有版本**，不跟随库版本
  （既有约定，见"版本号与发布"）；此项不属于本次不一致。

### 验证

九次改动各自的断言与门禁结果仍留在下面各条的"验证"小节（本版为版本号整理，未改渲染代码）。
重跑：`make test` 全 PASS、`pixels` 18/18 零差异、`pixels-vk` 全部用例 0 个超容差像素、
`pixels-d3d11` 只剩既有的 `anim/affine-text` 6 像素、`partial` OK、
`check_examples` 16 例 0 问题、`make check-guard` 0 报告；`binui_version()` 实测返回 `0.17.0`。

## [0.16.54] - 2026-11-17 【格式 v34】

**遮罩补上 `mask-type="luminance | alpha"`。** 这是 CSS Masking 里"用亮度还是用内容自己的
alpha 当遮罩"的开关；以前一律按亮度，于是"白色半透明圆"这种素材会被当成"白色 = 全保留"。
零格式改动 —— `BinUIMaskEntry.color` 的 alpha 本来就存最终取值，只是编译期少折一个系数。

### 做法

- 编译期只差一个乘数：`luminance`（默认，SVG 1.1 语义）折入
  `0.2126R + 0.7152G + 0.0722B`；`alpha` 型**不折**，取颜色 alpha × `fill-opacity` × `opacity`。
  渐变内容的色标同理（`alpha` 型下 `#808080 → #FF7A00` 的渐变整条保持不透明）。
- 于是同一份 `#808080` 内容：`luminance` 得 ≈128（半透）、`alpha` 得 255（全保留）——
  单测与证据图都按这个差额钉。
- 认不出的值（`mask-type="foo"`）点名告警并按 luminance 处理；写在**非 `<mask>` 元素**上
  也点名告警（它只描述 `<mask>` 自己，写了别指望生效）。
- 顺手把这几个属性补进 HTML 属性白名单（`mask-type`，含 `masktype` 的驼峰写法），
  否则会被当成"未实现属性"告警并丢弃 —— 这一条踩过：先只改了编译器、没改白名单，
  渲染出来两种语义一模一样，`gl_shot` 的日志里才看到那句"未实现，已忽略"。

### 验证

- `test_css` 新增 16.54 段（8 条断言）：默认 luminance 灰色 ≈128、显式 `luminance` 同默认、
  `alpha` 灰色 = 255、`alpha` 型仍乘 `opacity`（0.5 → ≈128）、渐变内容沿用 `alpha` 型
  （两个色标都 ≥250）、认不出的值要告警、写在普通元素上要告警；
- `pixels` 18/18 零差异；`pixels-vk` 全部用例 0 个超容差像素；`anim/svg-mask` 扩到 660×1090
  （新增 luminance / alpha / alpha+opacity / alpha 型渐变四例），**GL/VK 0 像素差、通道差 0**；
  `pixels-d3d11` 只剩既有的 `anim/affine-text` 6 像素；
- `make test` 全 PASS、`partial` OK、`check_examples` 16 例 0 问题、`make check-guard` 0 报告。
- 证据图：`docs/issues/assets/svg-mask-type.png`（同一份 #808080 内容，左半透、右全保留）。

## [0.16.53] - 2026-11-17 【格式 v34】

**遮罩内容可以用渐变了（`<mask>` 里的 `fill="url(#grad)"`）。**
P2 表里"掩码内容渐变按中间色"这条收掉，顺手修了一个 v28 就存在的区域缩放 bug。

### 做法

- **不再"取中间色"**：渐变形状生成**一条独立**的遮罩记录 ——
  `BinUIMaskEntry.flags` 的 bit0（`MASK_FLAG_FILL_GRAD`）置位，定长头后面紧跟一条
  `BinUIPathGradient`（100 字节）。这就是格式从 v33 → **v34** 的唯一原因。
- **关键的一步：色标的 alpha 存的是亮度**（白 → 255、黑 → 0，再乘 `fill-opacity` 与
  `opacity`）。于是运行期保留趟那句 `dst *= srcA` 逐点乘的就是渐变亮度 ——
  渐变蒙版天然成立，**运行期只多两行**（`mspec.fillGrad = grad` + 置 `PATH_FILL_GRAD`），
  三端零着色器改动、零新管线。
- **渐变坐标**折进"本组几何包围盒归一化"空间：`N ∘ Mc ∘ [kid 盒] ∘ gradientTransform`
  （与 v16 路径渐变同一套口径，radial 的 rx/ry 按 M 两列范数放大）。
  径向渐变在旋转/斜切下仍按轴对齐椭圆近似并告警（路径渐变同样的限制）。
- **纯色与渐变不合并**：同亮度的纯色形状照旧合成一条，渐变形状独占一条 —— 否则
  一条记录只能有一个常量颜色。
- **顺手修的老 bug（v28 起就有）**：`maskUnits="userSpaceOnUse"` 时 `width/height`
  被当成 objectBoundingBox 又乘了一遍元素盒（190×60 的元素 + 200×120 的 mask 区域
  → **38000×7200** 的图层框）。默认单位是 objectBoundingBox，所以平时看不出来；
  写显式 `maskUnits` 的人会得到一张巨大的离屏层（还可能撞到图层预算）。
  现在宽高按用户坐标算，单测钉住 200×120。

### 验证

- `test_css` 新增 16.53 段（13 条断言）：渐变记录生成、flags 置位、渐变本体可读、
  线性 + 两个色标、**白端 alpha 255 / 黑端 alpha 0**、归一化到本组几何盒（x 0 → 1）、
  纯色 + 渐变 → 两条记录且不合并、色标 alpha = 亮度 × fill-opacity（灰 #808080 × 0.5 ≈ 64、
  白 × 0.5 ≈ 128）、纯色记录的 flags 不置位、以及区域宽高 200×120 的回归断言；
- `pixels` 18/18 零差异（GDI 基准图不含 mask，这一例钉的是"格式改动没波及别的段"）；
- `pixels-vk` 全部用例（实测 **45 例** = 18 像素 + 4 示例 UI + 23 动画帧）0 个超容差像素；
  `anim/svg-mask` 扩到 660×700（新增横向线性、竖向三色标、径向聚光三例），
  **GL/VK 0 像素差、最大通道差 0**；`pixels-d3d11` 全部用例（实测 **47 例**）里
  只有既有的 `anim/affine-text` 那 6 像素；
- `make test` 全 PASS、`partial` OK、`check_examples` 16 例 0 问题、`make check-guard` 0 报告。
- 证据图：`docs/issues/assets/svg-mask-gradient.png`（下半部分是三个渐变蒙版）。

## [0.16.52] - 2026-11-17 【格式 v33】

**`<pattern>` 的 `patternTransform` 整份生效了：旋转 / 斜切也不再按轴对齐近似。**
P2 表里"pattern 只支持平移+缩放"那条收掉。零格式改动、零运行期改动 —— 平铺仍然是
编译期展开成普通路径，这次改的只是**展开时用的矩阵与枚举范围**。

### 做法

- **两种情形分开**，为了老页面逐位不变：
  - 轴对齐（`b = c = 0`）：继续走原来那套"用户空间网格"（格原点先过 T、步长乘 `a/d`），
    表达式一个字没动。A/B 实测：同一张 pattern 页，旧编译器 vs 新编译器 **0 个不同像素、
    最大通道差 0**；
  - 有旋转/斜切：平铺网格是**平行四边形格**，枚举改到 **tile 空间**（施加 `patternTransform`
    之前的坐标系）做 —— 把元素包围盒的四角用 `T⁻¹` 变换回去取 aabb 当枚举范围，
    每块按 `M = svgX · T · T(格原点) · 内容映射` 合成，剔除用"这一块在用户空间的外接 aabb"。
- **预算数"真的会画的块"**：枚举范围（`T⁻¹` 的 aabb）比实际覆盖区域大 √2 倍量级，
  第一版直接拿它估预算 → `rotate(30)` 的 178 块被估成 420 块、超过 400 上限，
  整张图案退回纯色。改成先用同一个可见性判定数一遍，再决定是否降级。
- **描边的诚实边界**：旋转/等比缩放是**相似变换**，圆笔仍是圆 → 线宽精确；`skewX/skewY`
  下圆笔应为椭圆，而路径段只存一个 `strokeW` → 按几何平均宽度画，并对"斜切 + 有描边"的
  pattern 告警（点名是哪一张）。`patternTransform` 不可逆（行列式 0）→ 告警并退回"首色实填"。
- 新增通用工具 `binui_path.h` 的 `PathInvert`（带退化判定）与 `PathIsSimilarity`
  （判断"圆笔是否还是圆"），都有单测。

### 验证

- `test_css` 新增 16.52 段（12 条断言）：`rotate(90)` 把 40×4 的横条变成 4×40（宽高互换）、
  无 transform 的仍是 40×4、`skewX(30)` 剪成 42.3×4、不再报"旋转/斜切不支持"、
  斜切+描边要告警、`scale(0)` 告警不可逆并退回元素盒实填、`PathInvert` 往返与退化、
  `PathIsSimilarity` 对旋转/斜切的正反判定；
- **逐位不变**的 A/B：`offscreen_render` 分别用 v0.16.51 与新编译器渲染同一张旧 pattern 页
  → `不同像素 0，最大通道差 0`（GDI 基准不受影响，`pixels` 18/18 零差异）；
- `pixels-vk` 全部用例 0 个超容差像素；`anim/svg-pattern` 扩到 660×820（新增 `rotate(30)` 的
  斜条、`rotate(45)` 的菱形格、`skewX(20)` 的斜砖、`rotate(-20) scale(1.4)` 的圆点阵），
  GL/VK **0 像素差、最大通道差 0**；`pixels-d3d11` 只有既有的 `anim/affine-text` 那 6 像素；
- `make test` 全 PASS、`partial` OK、`check_examples` 16 例 0 问题、`make check-guard` 0 报告。
- 证据图：`docs/issues/assets/svg-pattern-transform.png`（同一页的上半部分是老的轴对齐用例，
  下半部分是旋转/斜切 —— 直接和"轴对齐近似"的旧结果对比就知道差在哪）。

## [0.16.51] - 2026-11-17 【格式 v33】

**九宫格中段的最后一个填充模式补齐：`border-image-repeat: space`。**
P1-8 的尾巴（v0.16.49 时 `space` 是"按 repeat 处理并告警"的降级）。零新管线、零格式改动：
还是 `SplitNineAxis` 里多一个分支，三个 GPU 后端共用同一份数学。

### 做法

- **语义**：块尺寸 = 自然尺寸（= 内缩，**不缩放也不裁切**），块数 = `floor(中段 / 自然)`，
  余数均分成 **`cnt + 1` 个间隙** —— 两端各一个、块间各一个，全部等宽。CSS 的原话是
  "the extra space is distributed **around** the tiles"，所以首块不贴着角、末块也不贴边
  （这一点和 `repeat` 正好相反：`repeat` 是贴边铺、末块裁切）。
- **一处例外**：中段比一块还窄（`cnt == 1` 且 `自然尺寸 > 中段`）时没有"余数"可均分，
  把唯一一块缩到中段长。CSS 对 `space` 的硬要求是"不被裁切"，缩到刚好放得下正是这个意思。
- **与 round 的区别**（单测里钉住了）：同样 146px 中段、12px 自然尺寸 —— `round` 是 12 块
  各 12.17px（末块正好抵到边界、没有缝），`space` 是 12 块各 12px + 13 个 0.154px 的缝。
- **模式值 3** 进 `BinUIPaintEx::flags` 的两位（`BINUI_SLICE_SPACE`），旧二进制的 0
  仍是 stretch → **`BINUI_VERSION` 不变**。预算护栏照旧（两轴都 space 时块数是乘积，
  超过 256 个四边形就退成等分）。

### 验证

- `test_css` 新增 16.51 段（13 条断言）：模式落段（单值/双值/与 round 混写）、
  块数 = `floor(中段/自然)`、每块都是自然尺寸且 UV 取整段、**两端各留一个缝**、块间缝等宽、
  中段比一块窄时缩成一块、与 round 的对照、整盒双轴 space 的四边形数（75）与
  "所有块都在盒内、不超过自然尺寸"、y 轴余数均分（中心行下移 2.5px）；
- `pixels-vk` 全部用例 0 个超容差像素（`anim/nine-slice` 已扩成 640×860 的十个用例：
  横/竖/双轴 repeat、round、space、单块居中、末块裁切对照、中段过窄…），
  `pixels-d3d11` 只有既有的 `anim/affine-text` 那 6 像素；
- `pixels` 18/18 零差异（九宫格不参与 GDI 基准图，这一例钉的是"没碰别的路径"）；
- `make test` 全 PASS、`partial` OK、`check_examples` 16 例 0 问题、`make check-guard` 0 报告。
- 证据图：`docs/issues/assets/nine-slice-space.png`（space 那一行垫了深色底 ——
  间隙露出来的就是深色，"两端也留缝"才看得见）。

## [0.16.50] - 2026-11-17 【格式 v33】

**文字阴影支持多层了：`text-shadow: <层>, <层>, …`（上限 4 层）。**
P1 表里最后一条"只支持一层"的尾巴收掉。零格式改动、零新管线：段的数组本来就是
"一个节点连续好几条"，改的全是运行时与解析。

### 做法

- **解析**：多层不再"第二层起告警忽略"，全部进段。CSS 的绘制顺序是**写在前面的在最上面**，
  而段的数组顺序 = 绘制顺序（下标小的先画），所以按"书写顺序的逆序"写出 ——
  与 `box-shadow` 用同一套约定，读段的人不用记两套。
- **上限 4 层**（`BINUI_MAX_TEXT_SHADOWS`）：每层在 GPU 端都要为每个字形多烘一份
  "模糊覆盖率"图集格子，在 GDI 端要多一张位图。4 层覆盖了现实里的所有写法
  （描边式 3 层 + 1 层光晕、霓虹 2~3 层），再多对观感几乎没有增量。第 5 层起点名告警。
  全透明的层直接不占条目（省一份图集 / 一张位图）。
- **发射顺序 = 层在外、字在内**：整条阴影按层铺完，最后画清晰字形。若改成逐字交错，
  后一个字的阴影会压到前一个字上（半透明文字会看出脏边）—— 这条约束 v0.16.16 就有了，
  多层只是把"一条阴影"换成"若干条阴影"。
- **每层的阴影格挂在同一个字形四边形上**（`GlyphRunQuad::s[BINUI_MAX_TEXT_SHADOWS]`），
  所以同一句话的多层阴影仍然只有一份整形结果；`blur = 0` 的层复用字形自己那一格
  （"阴影 = 偏移一份字形"，零额外纹理），`blur > 0` 的层各取自己模糊半径的图集。
- **顺手修了两个既有 bug**（根因同一个：模糊半径没进整形缓存 key）：
  1. 同一句话写在两个不同模糊半径的节点上，第二个节点会默默画成第一个的模糊半径
     （烘进四边形的阴影 UV 被复用了）；
  2. 同一句话先被**无阴影**节点排版、再被有阴影的节点复用 → 命中缓存直接重放，
     那条阴影整条丢失（四边形是在整形时烘的）。
  现在层数 + 每层的量化模糊半径都进 key（偏移/颜色是发射期叠加的，不进位图）。
- **外扩/脏矩形按最大的一层**：`PaintTextShadowPad` 逐层跑一遍取最大，绘制侧（自身裁剪、
  剔除）与失效侧（`invalidRect`）用的还是同一个函数，两边不会各算一套。
- **GDI 遗留后端**：`g_gdiTextShadow` 从单个结构变成数组，缓存键里带上"是哪一层"，
  按数组顺序逐层 `AlphaBlend` 再画字；自身裁剪外扩也改成按最大层（公式与 GPU 端一致）。

### 接口变化

- `IBinUIGfx::setTextShadow(dx,dy,blur,color)` → **`setTextShadows(const BinUITextShadow*, int count)`**
  （一次给一整批，层序 = 绘制顺序，实现内部不再反转）。三个 GPU 后端都只是把它转给
  共享的 `GlyphAtlas::setTextShadows`；GDI 直接调的那批工具（`tools/draw_bench.cpp`）已同步。

### 验证

- `pixels` 18/18 零差异（新增的 `text-shadow-multi` 基准图由 GDI 生成，其余 17 张
  **逐字节未变** —— 说明单层的 GDI 输出没被这次改动碰到）；
- `pixels-vk` 全部用例：VK 与 GL **0 个超过通道容差的像素**，新用例 `text-shadow-multi`
  的最大通道差是 **0**（三端共用同一份图集代码，多层也不该有差）；
- `pixels-d3d11` 全部用例：只有既有的 `anim/affine-text` 6 像素（D3D11 自己的文本光栅化），
  两个 text-shadow 用例都是 0 像素 0 通道差；
- `test_css` 新增 16.50 段（15 条断言）：三层逆序进段、每层颜色独立、
  **继承两层**（顺序与值都一致）、`none` 清空、第 5 层截断 + 点名告警、
  全透明层被丢掉、外扩取最大层（blur 12 → 19px）；
- `make test` 全 PASS、`partial` OK、`check_examples` 16 例 0 问题、`make check-guard` 0 报告。
- 证据图：`docs/issues/assets/text-shadow-multi.png`（霓虹 3 层 / 描边 4 层 / 双色偏移 /
  继承 / `currentColor` / 贴边大模糊 / 三个不同模糊半径同时活跃）。

## [0.16.49] - 2026-11-16 【格式 v33】

**九宫格图片的中段也能平铺了：`border-image-repeat: stretch | repeat | round`。**
P1-8 那条「九宫格只有 stretch」的尾巴收掉。零新管线：切分数学还是三端共用的
`BuildNineSlice`，只是"中段一块拉伸"变成"中段按模式切若干块"。

### 做法

- **模式存哪**：`BinUIPaintEx` 本来就有一个写着"保留（目前恒 0）"的 `flags` 字节 —— 拿它存
  "两轴各 2 位"（0=stretch 1=repeat 2=round）。旧产物的 0 正好等于 stretch，所以
  **`BINUI_VERSION` 不必升**（与 v0.16.38 复用 flags 空闲位同一个先例）。
- **`repeat`**：按"自然尺寸 = 内缩像素"铺；末块不是丢弃而是**裁切** —— 目标宽度缩到剩余部分，
  **源 UV 也按同一比例裁**（`f1 = rest / natural`），否则末块会被整段压扁。
- **`round`**：块数 = `lround(中段长 / 自然尺寸)`，块长 = 中段长 / 块数，不留裁切边。
- **预算护栏**：两轴都平铺时块数是乘积（中心也会摊成网格），超了就把块数按比例压小
  （退成等分，看起来接近 round），单次最多 256 个四边形（栈上 8 KB）。
- 三个 GPU 后端只是把 `repX/repY` 透给共享切分函数；GDI 遗留后端仍然退回整图拉伸
  （`drawImageNine` 默认实现）。

### 验证

- `pixels-vk` / `pixels-d3d11` 全绿：`anim/nine-slice` 这个用例已经扩成 5 组
  （横 repeat / 竖 repeat / 双轴 repeat / round / space→repeat），VK 与 D3D11 对 GL
  **0 个超过通道容差的像素**；
- `pixels` 17/17 零差异（GDI 不参与九宫格，这一例钉的是"没碰别的路径"）；
- `test_css` 新增 16.49 段：两轴模式落段、stretch **与老实现逐字节相同**、
  repeat 的块数与自然尺寸、末块裁切（目标与源 UV 一起裁）、round 的取整分配、预算护栏；
- `make test` 全 PASS、`partial` OK、`check_examples` 0 问题、`make check-guard` 0 报告。
- 证据图：`docs/issues/assets/nine-slice-repeat.png`（同一张 48×48 贴图，每条边中间有色点，
  平铺后这些点会重复出现 —— 一眼能看出到底铺没铺）。

## [0.16.48] - 2026-11-16 【格式 v33】

**P0-3 收口：扫描线填充改用活动边表（AET）—— 50 个 120px 图标 3.29 ms → 1.35 ms（AA）。**
这条路没走多线程（那是线程亲和 + GL/VK 上下文风险那条路），而是把算法本身修对：以前**每行都重扫
路径的全部边**（AA 还要 ×4 子采样），行内工作量 = 行数 × 边数；现在边只摊平一次、按 y 起点排序，
每行只保留"跨过这一行"的几条。

### 做法

- 新增 `PaintScanEdge` + `BuildPaintScanEdges`（`include/binui_paint.h`）：把折线摊成边，
  **跳过水平边**（水平边不产生交点），按 `yb` 升序；
- 新增 `ActivateScanEdges`：行内一步"激活 + 去活"（游标只前进；上一行留下但已整条在上方的边就地
  丢弃），AA 的 4 条子扫描线共用同一份候选边；
- 交点表达式**逐字沿用** `t = (yc - ay) / (by - ay)`（同一浮点式子 → 输出逐位不变，不是"看着一样"）。

### 量测（`make bench-path`，单线程 best-of-5，每轮 20 次；表里是 24/48/120/300 四档里的 120px）

| 形状（120px） | 硬边 前 → 后 | AA 前 → 后 |
|---|---|---|
| 星形 10 点 | 6.30 → 5.00 µs | 29.4 → 22.6 µs |
| 齿轮 16 齿 | 18.5 → 7.65 µs | 89.7 → 33.3 µs |
| 圆 64 段 | 24.9 → 5.10 µs | 78.5 → 25.4 µs |
| **三个合计** | **49.7 → 17.8 µs** | **197.6 → 81.2 µs** |
| **50 个图标（验收口径）** | **0.83 → 0.30 ms** | **3.29 → 1.35 ms**（目标 ≤2 ms ✅） |

300×300 三档：硬边 15.5/47.1/43.4 → 12.8/19.7/12.5 µs，AA 118/288/230 → 110/180/111 µs ——
大尺寸的 AA 已经主要花在"每行填覆盖率 + 出条带"上，不再是判交（所以再优化得动那两块，收益有限）。

### 验证

- `pixels` **17/17 零差异**：GDI 基准直接吃这套条带，等于"输出逐位不变"的硬证据；
- `pixels-vk` / `pixels-d3d11` 全绿（没有超过通道容差的像素）；`make test` 全 PASS；
  `partial` OK、`check_examples` 0 问题、`make check-guard` 0 报告；
- `make bench-draw` 的遍历数字不变 —— 条带缓存的稳态本来就不重算，这条改动只影响"重算那一次"
  （首帧 / resize / 几何在变的动画）；
- 新工具 `make bench-path`：纯 CPU、无窗口、确定性，硬边与 AA 分开报，换机器也能复现同一口径。

## [0.16.47] - 2026-11-16 【格式 v33】

**`filter: url(#f)`（SVG `<filter>`）支持了 —— 编译期归约，运行期与二进制格式零改动。**
这是 `docs/future.md` 里"图 8 / 图 10"剩下的最后一条 SVG 缺口；上一版把它记成"需要第二套
绘制管线"，实际动手发现**不需要**：后端离屏图层已经能表达两种原子效果，把原语链在编译期
折成它们的组合就够了。

### 做法：两种原子效果 + 一条归约规则

运行期只有"整组模糊"（`paint.blurPx`，v32）与"身后的彩色影子"
（`BinUIFilterDropShadowEntry`，v33）—— 这正是 CSS 侧 `blur()` / `drop-shadow()` 的落点。
于是 `<filter>` 的归约规则就是"把链折成这两样"：

| SVG 原语链 | 归约成 |
|---|---|
| `feGaussianBlur(SourceGraphic)` | 整组模糊（σ = `stdDeviation`） |
| `feDropShadow` | 一条影子 |
| `feGaussianBlur(SourceAlpha) → feOffset → feFlood → feComposite(operator="in") → feMerge[影子, 源]` | 一条带模糊的影子（最常见的手写版本） |
| 同上去掉 `feGaussianBlur` | 一条硬边影子（σ=0） |
| `feMerge` 里多条影子 | 多条影子（SVG 先写的在下面；阴影段口径是"下标 0 在最上面"→ 倒着落段） |

`filter` 现在既认 CSS 写法（`style="filter:url(#f)"`）也认 **SVG 表现属性**
（`<g filter="url(#f)">`）。归约不了的原语逐条点名告警：`feColorMatrix` / `feBlend` /
`feBoxBlur` / `feMorphology` / `feTurbulence` / `feImage` / `feTile` / `feDisplacementMap` /
光照（逐像素运算要么改着色器、要么每帧同步读回，两条都是本项目明确不做的）。
另外这些也告警：连续两层模糊（运行期只做一趟）、各向异性 `stdDeviation`（按 σx）、
`feOffset` 直接作用在源图形上（= 整体平移）、`feMerge` 多源图形、影子写在源图形之后、
滤镜区域比默认 `-10%..110%` 更窄（我们不按它裁剪）。

### 顺手修的一个坑：套滤镜的 `<g>` 内容整个消失

`<g>` / `<svg>` 这类容器自己的盒子常常是 **0×0**（子形状是绝对定位的独立节点），而离屏图层
框 = 节点框外扩 3σ —— 框是空的就把层里的内容**全裁掉**。第一版实测：套了
`filter="url(#f)"` 的 `<g>` 整块消失，而 `<div>` 套同样的滤镜正常。
修法：编译期给"带滤镜且盒子为空"的节点算一个**子树包围盒**撑大盒子（**不动 x/y**，免得把
流式光标和兄弟带偏），并把结果写进重排规格（`relW/relH` + 清 `SPEC_AUTO_W`）——否则运行期
`setSize` 会按"自动宽度"把它算回 0×0（这一条是实测出来的：编译期补好了盒子，`gl_shot`
里还是 0×0）。

### 验证

- `pixels` 新增 `svg-filter`（660×470，16 → 17 例）：GDI 基准 **0 差异**（GDI 是遗留后端，
  本来就不画阴影，这一例钉的是"内容不会被盒子裁没 + 几何/颜色不变"）。
- `anim/svg-filter` 进跨后端比对：VK vs GL **0 不同像素**（最大通道差 2）、D3D11 vs GL
  **0 不同像素**（最大 2）—— 与既有的 `drop-shadow` / `blur` 用例同级。
- `test_elements` 新增 v47 段（`tests/test_elements.cpp`）：五原语链归约、硬阴影、`feDropShadow`、
  单 `feGaussianBlur`、两条影子的叠放顺序、`feColorMatrix` 告警且不产生效果、引用不存在 id 的
  告警、带滤镜的 `<g>` 盒子被撑到覆盖子树但**不**铺满父容器。`make test` 全 PASS。
- 其余门禁：`partial` OK、`check_examples` 16 个 UI 0 问题、`make check-guard`（电篱笆）0 报告。
- 证据图：`docs/issues/assets/svg-filter.png`（GL）、`docs/pixels/svg-filter.png`（GDI 基准）。

## [0.16.46] - 2026-11-16 【格式 v33】

**定位并修掉 `check_examples` 那条"约 30% 概率 `0xC0000005`、一个字都不输出"的旧问题**
（v0.16.38 就记进 `docs/future.md` 的未定位清单）。根因不是当初猜的"堆越界写"，而是
**编译器布局栈里的一处悬空引用**：`Ctx& pc = stack.back()` 拿到的引用，在
`stack.push_back()` 把 `stack` 扩容之后**指向已释放的内存**，后面还在读
`pc.ox / pc.oy / pc.contentW / pc.vpW / pc.vpH`。读到的是旧垃圾 —— 绝大多数时候只是几个
无害的陈旧数值，偶尔凑成病态值才炸。这也解释了"崩不崩取决于二进制布局：只往头文件加几个
零调用的 inline 函数，就从 0/30 变成 9/30"。

### 定位方法 — 本机没有 libasan / page heap，自己造了一个

- 新增 `tools/guard_alloc.cpp` + `make check-guard`：把每个 `operator new` 块放到
  **"末尾紧贴一个 `PAGE_NOACCESS` 页"**的位置（Electric Fence 精简版），释放时立刻
  `VirtualFree` → **越界写、读已释放内存都当场 0xC0000005**，不再靠布局运气。末尾因对齐
  留下的 ≤15 字节空档填哨兵，`free` 时校验并打印**这块内存的分配栈**（知道是哪个缓冲区
  被写坏）。只接管 `operator new/delete`、不动 `malloc`（CRT 堆必须自洽）。
- 效果：老代码在它下面不是"30% 崩"，而是**编译第一个示例就 100% 崩**，fault PC 每次都落在
  同一条指令（`CompileHTML` 里读 `pc.vpW`）→ 20 分钟收敛。
- `docs/future.md` 里那条"需要 ASan 或 page heap 才能定位"的结论改成"用 `make check-guard`"。

### 修

- `CompileHTML`：布局栈一次 `stack.reserve(elems.size() + 1)`（栈深上限就是元素数+1），
  `push_back` 从此不扩容；同时在 `push_back` **之前**把父上下文的 5 个字段拷成局部变量
  （`pcOx/pcOy/pcContentW/pcVpW/pcVpH`），后面不再经过 `pc` —— 不让正确性依赖"reserve 过
  就一定不扩容"这种事。

### 验证

- 老二进制改前记录过 17/60、12/40、41/120（4 倍目录 = 每进程 64 次编译才容易踩到）；
  修后 **200 次 × 每进程 64 次编译，0 崩**。
  （诚实标注：旧二进制在本次会话后段重跑也变成 0/60 —— 这个概率本身依赖系统当时的堆状态，
  所以"统计对比"不能单独当证据；**真正的证据是电篱笆下老代码 100% 必现、新代码 0 报告**。）
- `make check-guard`：16 个示例、每进程 64 次编译 × 6 轮，0 报告 0 崩溃。
- 像素门禁全绿（这条修复不改变任何可见输出）：`pixels` 16 例 0 差异；`pixels-vk` 全部
  **0 不同像素**（最大通道差 ≤2）；`pixels-d3d11` 0 不同像素（≤1）；`partial` OK；
  `make test` 全 PASS；`check_examples` 16 个 UI 0 问题。

## [0.16.45] - 2026-11-16 【格式 v33】

**修一个从 v0.16.12 就在的三端差异**：VK 后端在**离屏图层里**用的是 **4× MSAA 的主管线**，
而层目标是**单采样**的 —— 采样数状态与附件不一致。整数对齐的图元看不出差别，但**圆角图片**
是按行切细条的（`BuildImageBands`，每条的左右边都是小数），弧线上**每行差一个像素**
（17 个像素、单通道差 200+）。复现只要 `opacity:0.98` 包一张 `border-radius:14px` 的 `<img>`，
和模糊、和投影都无关；GL 与 D3D11 一直是对的。

### 修 — `pushLayer` 里管线选得太早

- VK 的 `pushLayer` 顺序是"`beginLayerPass` → 记录层框 → `layerStack.push_back`"。而
  `refreshPipeline()` 是按 `!layerStack.empty()` 判断"在不在层里"的（层内用单采样变体），
  `beginLayerPass` 里调它的时候栈**还是空的** → 选成 4× 主管线。修法是一行：`push_back`
  之后**再选一次**（并在旁边写清为什么不能省）。
- 只有**最外层**图层会踩到（嵌套层的 `beginLayerPass` 时栈非空，本来就选对），所以以前那些
  "层里的文字/圆角卡片"用例全是好的 —— 只有"层里的圆角图片"暴露，且必须在**单采样**目标里
  才看得见（主目标有 4× MSAA，覆盖率的判定被平均掉了）。
- 影响面比"模糊质量"宽：`opacity` 套圆角图片在 v0.16.12 之后一直有三端差异，只是门禁用例里
  没有这个组合。

### 连带效果（同一批门禁数字）

- `pixels-vk`：`filter-blur` 的最大通道差 **20 → 1**，`drop-shadow` **58 → 2**，
  `blur-quality` **48 → 1** —— 之前那些"几十"的差值里，有相当一部分就是这里来的。
- **v0.16.44 的临时让步撤销**：模糊的最小降采样级数从 1 放开回 **0**（`σ<2` 时全分辨率处理，
  省一趟填充），因为当初定成 1 只是为了把上面这个差异 2×2 平均掉。现在
  `filter: blur(0.5~3px)` 对圆角图片也是 0 差异。

### 新门禁用例 — `anim/round-in-layer`

`docs/issues/assets/round-in-layer.html`：层内 r14 / 层内 r46（大弧线）/ 层内**小数坐标** r8 /
层内 `blur(2px)` r20 / 不套层 r14（对照），另加层里的圆、渐变圆角、1~2px 细线、胶囊，以及
**嵌套两层**（外层 opacity 0.9 里再套 opacity 0.6 的圆角图片）。三端 0 不同像素、最大通道差 0。

### 门禁

- `make test`：0 失败（`PaintFilterBlurLevel` 的期望值跟着改成允许 0 级）。
- `pixels-vk` / `pixels-d3d11` / `pixels`：全部 0 不同像素（GDI 基准 16 例 0 差异）。
- `partial`：OK；`check_examples`：16 个 UI / 0 问题。
- 证据图：`docs/issues/assets/round-in-layer.png`。

## [0.16.44] - 2026-11-16 【格式 v33】

**模糊质量：把"降采样 ≈ 高斯"换成"降采样 + 真正的分离高斯"**。三条模糊路径
（`backdrop-filter` 毛玻璃 / `filter: blur` / `filter: drop-shadow`）共用这一份算法，
所以它们一起变平滑。**没有格式变化**（v33 沿用），纯绘制期改动。

### 改 — 模糊的算法（观感：斜坡从"梯形/平台"变成真正的高斯 S 形）

- **旧实现的病根**：逐级 2× 降采样，最后把**最粗那一级**双线性放大回原位。半径一大，
  最粗级只剩 5~7 个纹素 —— 形状信息在那一步就没了，放大回来是一条"斜坡 + 折点"。
  最直观的例子：14px 的圆点配 `blur(12px)` 糊出来是个**十字**；6px 的细条糊出来是个方块；
  100px 的卡片边缘是一条 10px 宽的直线斜坡（而 σ=18 本该有 100px 宽的过渡带）。
- **新实现两条**：
  1. 降采样只降到 **σ 还剩 ≥2 纹素**的那一级（`PaintFilterBlurLevel`，0..5 级）；
  2. 在那一级上跑**横/纵两趟加权卷积**（`PaintFilterGaussTaps`）：抽头**每个纹素一个**、
     覆盖 ±3σ，顶点色 = 白×权重，用**加法混合**（GL `ONE/ONE`、VK 与 D3D11 各新加一条
     ADD 管线/混合状态）把抽头累加进已清空的目标 —— 权重和正好 255，累加结果就是
     **预乘色的加权平均**（真卷积）。没有片元着色器可写，加法混合是唯一能"求和"的混合方式。
  3. 两趟往返 `level → 暂存 → level`（同一个目标不能既读又写；暂存复用图池末号索引）。
- **口径统一到 CSS**：σ = 半径。所以 `blur(18px)` 现在是 σ=18 的高斯（10~90% 过渡带
  实测 39px），外扩恰好是 3σ = 3N，与既有的 `PaintFilterBlurPad` 一致 —— 以前那条链的
  等效 σ 只有半径的 1/4 左右，**同一份 CSS 现在会明显更糊，这是修正不是回归**。
- 抽头数由 σ 决定（σ∈[1,4) → 7~25 个），同一个批次、一次 draw；两侧抽头权重为 0 时
  仍然照画（顶点色 alpha=0，代价可忽略），保持代码路径单一。
- 级数规则与抽头算法提到 `include/binui_paint.h`（`PaintFilterBlurLevel` /
  `PaintFilterGaussTaps`）：三端 + 单元测试**共用一份实现**（以前是三份拷贝，
  "改一处漏两处"正是这轮踩到的坑）。

### 修 — 量测中挖出来的三个缺陷

- **GL/VK/D3D11：抽头按 ±kσ 布点会留空档**（第一版就是 7 个抽头、间距正好 σ）：
  σ=2.5 纹素时抽头之间空 2.5 纹素，卷积核成了**梳状函数**，输出是一圈圈等距条纹
  （21×21 的级上肉眼就是"糊出来带栅栏"）。改成整纹素密采样后核才完备。
- **权重被截断成整数**（GL 第一版）：浮点权重直接写进 `uint32_t` 数组，`0.9` 截断成 0，
  7 个抽头里只剩中心那个 255 —— 整条核退化成 delta，"模糊完全没生效"（比旧实现还清晰）。
  现在先算浮点再量化，并把量化误差补在中心抽头上保证和为 255。
- **`ensureBlurLevel` 会 `resize` 级池**：函数里抓的 `BlurLevel&` 引用会因此悬空，
  读到的 w/h 是已释放内存。三端都改成"先取标量、之后按下标取"。

### 已知问题（既有，本轮只是让它不再被触发）

- **圆角图片在 1× 离屏层里，GL 与 VK 的圆角模板相位差 1px**（17 个像素、单通道差 200+）。
  与模糊无关：只要 `opacity:0.98` 包一个 `border-radius` 的 `<img>` 就能复现。
  本轮把模糊的**最小降采样级数定为 1**（`PaintFilterBlurLevel` 注释里写了原因），
  于是合成不再是"1:1 拷贝"，这个相位差被 2×2 平均摊平成网格量化级别（实测最大通道差
  回到 59 以下）。等圆角模板对齐后这一级可以放开到 0。
- 模糊的填充成本随 σ 下降：每级一趟是"整级面积 × 抽头数"，一个 1280×720 的毛玻璃背板配
  `blur(4px)` 是最坏情形（两次 13 抽头 ≈ 6M 次写）。演示页量级（300×200 区域、σ≥10）实测
  没有可测的帧时间变化。

### 门禁

- `make test`：新增 18 条（级数档位/边界、抽头个数、权重和 255、左右对称、单峰、
  尾巴截断、`maxN` 收窄），0 失败。
- `pixels-vk`：**全部 0 不同像素**；本轮新增 `anim/blur-quality`（小元素大半径 + 边缘剖面 +
  文字 + 三档毛玻璃），最大通道差 48。`filter-blur` 的最大通道差从 59 降到 **20**。
- `pixels-d3d11`：无 FAIL；blur 三条用例最大通道差 1~2。
- `pixels`（GDI 基准 16 例）：0 差异；`partial`：OK；`check_examples`：16 个 UI / 0 问题。
- 证据图：`docs/issues/assets/blur-quality.png`（改前 `blur-quality-before.png` 的圆点是十字）。
  量测脚本：`tools/profile_probe.py`（沿一条水平线取亮度剖面与一阶差分）。

## [0.16.43] - 2026-11-16 【格式 v33】

**`filter: drop-shadow(...)`**（元素轮廓的投影）。到这里 CSS `filter` 家族就只剩
`url(#f)`（引用 SVG 滤镜链，需要第二套绘制管线）没做了。

### 加 — `filter: drop-shadow(dx dy [blur] [color])`（格式 v33：外阴影段，20 字节/条）

- **影子取的是元素自己的 alpha（轮廓），不是盒子** —— 这是它跟 `box-shadow` 的本质区别：
  星形图标的影子是星形、一句话的影子是字的形状、一张透明边缘的图的影子跟着图的轮廓走。
  `box-shadow` 是"圆角矩形的影子"，无论内容长什么样。
- 所以它跟 `filter: blur` 走同一套离屏设施（同一份图层）：`pop` 时对每一条阴影
  ① 把阴影色（预乘）铺满整块区域；② 用**保留混合**（`dst *= srcA`）把层内容画一遍 ——
  于是得到 `(c.rgb·c.a·a, c.a·a)`：**上色的轮廓**；③ 按阴影半径走同一条降采样链；
  ④ 按偏移（**取整**，三端同一套整数矩形）叠在元素下面。最后才把本体（清晰或模糊）画上去。
- **多条阴影**：按 CSS 的书写顺序落段，绘制期逆序（写在前面的在上层）；一条里两个偏移
  必填，第三个是可选的模糊，颜色可以写在任意位置、缺省 `currentColor`。
- 与 `filter: blur` 同写时明确告警：我们的离屏层只做一趟，阴影与本体都取自同一张
  （未糊的）层内容 —— CSS 里 `blur() drop-shadow()` 的阴影本该取自"已经糊过"的轮廓。
- 图层外扩口径统一到 `PaintLayerPad`（`binui_paint.h`）：自身 3σ 与每条阴影的
  `3σ + |偏移|` 取最大，图层框与合成矩形都按它外扩并取整。
- `invalidRect` 同步（模糊那一版已经在算"自己的 + 祖先的"外扩，阴影沿用同一处）。
- 为什么单开一段而不是复用 `box-shadow` 的阴影段：两者的输入根本不同（盒子几何 vs
  离屏 alpha），而且 drop-shadow 每条要 20 字节（偏移×2 + 半径 + 颜色），稀疏存储最省。

### 修 — 两个在量测中挖出来的既有缺陷（都是"三端不一致"的根因）

- **GL：轮廓采样的 V 方向写反了**（v0.16.42 的 `filter: blur` 不受影响，因为它只抓不管
  方向；但阴影是"取 alpha 上色"，方向一反就是一个上下镜像的轮廓 —— 对称元素看不出来，
  星形/文字一眼能看出"影子长在元素上方"）。同一个函数里两处 UV 的方向约定不同，
  这回把注释写死在代码旁边。
- **D3D11：降采样链的 scissor 没有还原**（v0.16.42 引入）：每级降采样都把批次矩形换成
  "小图局部"，退出时忘了还原，于是 scissor 停在最后那一级的大小（比如 18×17）上 ——
  **之后画的所有东西都被裁在那块里**，表现为"模糊/阴影元素之后的文字整行消失"。
  GL/VK 没有这个问题（它们不用 scissor 表达这件事），所以三端比对才暴露出来。

### 门禁

`make test` 新增 12 条（一条/多条落段、硬阴影缺省 blur、负偏移、颜色前置、
`drop-shadow(3px 3px)` 合法、`currentColor` 缺省、外扩口径 20/18）；
`docs/issues/assets/drop-shadow.html` 进跨后端比对（星形轮廓 / 圆角渐变 / 圆形 /
两条阴影叠加 / 硬阴影 / 文字轮廓 / 子树一个轮廓 / 负偏移 / 颜色前置 / opacity 同用 /
贴窗口边缘 / blur+shadow 同写 / 方角图片）：**GL vs VK 0 不同像素（最大通道差 58）、
D3D11 vs GL 0 不同像素（最大通道差 2）**；`make pixels` 16/16 0 差异；`make partial` 通过；
`check_examples` 16/16。证据图 `docs/issues/assets/drop-shadow.png`。

### 记 — 一个已知的观感边界（不影响门禁）

多级 2× 降采样链是"用缩图近似高斯"，不是真高斯：半径大而元素小的时候（例如 100px 的块
配 18px 半径），低级数纹理只有 5~7 个纹素，残余会呈**梯形/平台状**而不是平滑的钟形。
`backdrop-filter: blur` 一直是这个特性（它的输入通常是平滑背景，看不出来），
drop-shadow 的输入是硬轮廓，所以更容易被眼睛抓到。要更平滑需要"区域分辨率上的多趟
可分离高斯"（多几个四边形 + 一张临时图），留给下一轮，见 `docs/future.md`。

## [0.16.42] - 2026-11-16 【格式 v32】

**`filter: blur(Npx)`**（元素自身 + 整棵子树的离屏模糊）。这是 `filter` 家族里最后一块
"需要离屏合成"的能力：颜色矩阵（v26）做得到逐像素变换，但模糊必须先把整组画面
合成到一张离屏图、再糊掉、再叠回来。以前写了只换来一条"需要离屏合成，已忽略"的告警。

### 加 — `filter: blur()`（格式 v32：绘制段 52 → 56 字节）

- **组语义**：`filter` 在 CSS 里作用于"元素 + 它的整棵子树"合成后的画面，不是逐图元。
  落法和 `opacity` 完全一样 —— 绘制期把该节点连同子树包进离屏图层，`pop` 时多跑一趟
  "把层内容糊掉再合成"。所以两者**共用同一条图层**：同时写 `opacity` 和 `filter` 的元素
  只建一层、只合成一次。
- **模糊 = 抓层纹理 → 多级 2× 降采样 → 正常 alpha 合成**：与 `backdrop-filter: blur`
  （毛玻璃）**同一条链、同一套级数公式**（级数 = 让 2^levels ≥ 半径的最小值，1..6）。
  差别只有两处，也就是"糊自己"与"糊身后"的全部区别：
  抓取源是**层纹理**（不是当前目标）；合成用**正常 alpha 混合**（不是 replace）。
- **溢出处理**：模糊会把画面摊到盒子外面，所以图层框与合成矩形都按 3σ（`PaintFilterBlurPad`）
  外扩并**取整** —— 取整是刻意的：抓取/回写矩形在三端都是整像素，合成四边形因此恒落在
  像素边界上，三端逐像素一致（小数坐标的元素也一样：内容照旧按原坐标画进层里，只是层框变大）。
- **半径上限 64px**：降采样链最多 6 级，再大的半径只是把已经糊成一片的画面继续糊，
  却要多扫几遍整块区域 —— 超了钳制并告警。百分比对 CSS `blur()` 无效（那是 SVG
  `stdDeviation` 的语法），按 0 处理并告警。`blur(0)` = 不建层、像素与以前完全一致。
- **`-webkit-filter` 别名**一并支持（同一段解析逻辑）。`drop-shadow()` / `url(#f)` 仍不支持，
  告警文案已改成只提它们两个（不再把 blur 列进去）。
- **为什么升格式号**：半径要跟着节点走（`float`，小数有意义），`reserve` 三个字节与
  `flags` 16 位都已占满 —— 直接加 `float blurPx`（52 → 56 字节），`BINUI_VERSION` 31 → 32。
- **脏矩形也跟着外扩**：模糊画到盒子外面，所以 `invalidRect` 现在会把"自己的 3σ"和
  **祖先的 3σ** 都算进失效矩形 —— 后者容易被忽略：模糊卡片里的按钮 hover 一下，
  卡片那圈模糊后的边上也会变，不把祖先的 pad 算进来的话，局部重绘会在卡片边缘留一圈
  没人擦的旧模糊（与 box-shadow 当年踩的坑同一个形状）。
- **GDI 遗留路径不模糊**：`pushLayerBlurred` 返回 false → 调用方退回"不模糊"（若该节点还写了
  `opacity`，那一半照旧走普通图层）。与 `backdrop-filter` 在 GDI 上的处理一致。

### 门禁

`make test` 新增 17 条（半径落段、`blur(0)`/未写不建层、`-webkit-filter` 别名、与颜色矩阵
同写、200px 钳到 64px 并告警、百分比告警并按 0、外扩口径 0/6/8/192）；
`docs/issues/assets/filter-blur.html` 进跨后端比对（5 档半径 / 子树模糊 / 渐变圆角 /
opacity+blur 合层 / 嵌套两层模糊 / 小数坐标 / `-webkit-` / 贴窗口边缘 / blur 与不支持的
函数同写）：**GL vs VK 0 不同像素（最大通道差 59）、D3D11 vs GL 0 不同像素（最大通道差 2）**；
`make pixels` 16/16 0 差异；`make partial` 通过；`check_examples` 16/16。
证据图 `docs/issues/assets/filter-blur.png`。

### 记 — 量测中撞到的一个**既有**差异（不是本版引入）

### 限定 — 局部重绘（脏矩形）必须覆盖"元素框 + 外扩"

`vk_window --dirty`（只画四分之一屏、四帧轮完一整幅）在含模糊的页面上与整帧渲染不一致
（本页 5.5%，`glass` 毛玻璃页 7.5%）—— 这不是本版引入的：**离屏特效的输入范围就是
它自己的作图范围**，只画四分之一屏时模糊只能看到那四分之一的内容，天然凑不出整帧的结果。
`docs/issues/assets/glass.html`（毛玻璃）一直如此，模糊只是同一条限制的第二个成员。
真正要紧的是**外壳的脏矩形**（`renderer.invalidRect`）：那里现在把外扩算全了，
所以 hover/动画驱动的局部重绘是逐位一致的（`make partial` 通过）。

顺带查清了一件事并写进 `docs/future.md` 的"工具链已知问题"：**放进离屏图层的
"圆角图片"在三端有 17~22 个像素不一致**（GL == D3D11 逐像素相同，VK 不同）。
隔离结论：`opacity:0.98` + 圆角 `<img>`（**完全没有 `filter`**）就能复现，所以与本版无关；
根因是层目标是**单采样**的，而圆角图片是"横向细条"拼出来的亚像素几何 —— 1× 目标里
没有 MSAA 兜底，条带边界那条像素的取舍在三端各自为政（一像素级的 0/1 翻转），
再被模糊摊开成几十的差。门禁判据（不同像素通道差 > 128 才算不同）仍然通过，
但要追平需要"层内也做解析式 AA"，属于另一件事。

## [0.16.41] - 2026-11-16 【格式 v31】

**`text-overflow: ellipsis`**（单行截断 + 省略号）。列表项名字太长、表格某一列放不下，
以前只能靠 `overflow:hidden` 硬裁（字被切一半，很难看）。

### 加 — `text-overflow: ellipsis`（`TEXT_ELLIPSIS`，格式不变，仍 v31）

- **截断发生在绘制前，用真实字形宽度**：`IBinUIGfx::ellipsize()`（默认实现，四个后端共用）
  先量整串，放得下就原样返回；放不下就用**二分**找"最长的、加上 … 仍不超宽"的 UTF-8
  前缀（回退到码点边界，绝不切半个汉字/emoji），补 `…`。
- 好处是"量出来的"与"画出来的"天然一致（都用 `textWidth` + 同一份 run 缓存），
  而且对**运行期 `setText`** 换进来的长文本同样生效 —— 不需要重新编译。
- 走的是 `textFlags` 的空闲位（`TEXT_ELLIPSIS = 1u << 4`），不新增字段、不升格式号。
- **多行不适用**：CSS 里 `text-overflow` 只对单行生效，含换行的文本照旧按多行画，
  编译期明确告警（不静默）；`clip` / `auto` 按"不截断"处理。
- 顺手把 `text-overflow` 从"已知不生效"清单里移除（否则会同时打一条自相矛盾的告警）。

### 门禁

`make test` 新增 4 条（标记落段、未写不截断、`clip` 不截断、多行 + ellipsis 告警）；
`docs/issues/assets/text-overflow.html` 进跨后端比对：**GL vs VK、D3D11 vs GL 均
0 不同像素**（同一个字符串在三种机器上截断位置完全一致）；`make pixels` 16/16 0 差异；
`make partial` 通过；`check_examples` 16/16。证据图
`docs/issues/assets/text-overflow.png`（600/360/200/120px 四个宽度 + 中文 + 比 … 还窄的盒子）。

## [0.16.40] - 2026-11-16 【格式 v31】

**`letter-spacing`**（字间距）。设计稿里"标题加一点字距"是最常见的排版要求之一，
以前写了只换来一条告警。

### 加 — `letter-spacing`（格式 v31：绘制段 48 → 52 字节）

- **像素、可负、可继承**：`normal` / `0` 复位；`em` 按字号折算。CSS 里这是**可继承属性**，
  所以走编译器的 `Ctx` 链（和 `color` 一样），子元素不写就跟着祖先走。
- **实现是后端图集里的一个排版状态**（`GlyphAtlas::setLetterSpacing` + `IBinUIGfx`
  同名虚函数）：`textWidth` / `drawText` / `drawTextWrapped`（折行位置 + 居中/右对齐）
  全都走同一套度量，所以**量出来的宽与画出来的宽永远相等** —— 这正是不给 `drawText`
  加参数、而用"状态"的原因（否则总有一处忘了传）。整形缓存的 key 带上它
  （同一个字符串在 0 / 2px 间距下是两份 run），字形位图本身不变。
- **编译期估算同步**：`EstimateTextWidth / EstimateLineCount / EstimateTextHeight` 都加了
  字间距参数（每个字符后加一次，含末尾那个 —— 与浏览器一致），于是"自动撑高/自动换行"
  在编译期与运行期（`autoTextHeight`）用同一口径，不会出现"编译期放得下、运行期画宽一截"。
- **为什么升格式号**：`BinUIPaintNode` 的三个 `reserve` 字节已被静态 opacity、渐变角度、
  `filter: brightness` 占满，`flags` 的 16 位也用满。字间距需要小数与负值，塞不进去，
  所以直接加 `float letterSpacing`（48 → 52 字节），`BINUI_VERSION` 30 → 31。

### 门禁

`make test` 新增 9 条（2px/1.5px/负值原样落段、可继承、`normal` 复位、50px 不设人为上限、
估算宽度按字符数加间距、CJK 同样每字符一次、间距参与换行行数）；三端渲染
`docs/issues/assets/letter-spacing.html`：**GL vs VK、D3D11 vs GL 均 0 不同像素**、
最大通道差 0；`make pixels` 16/16 0 差异；`make partial` 通过。证据图
`docs/issues/assets/letter-spacing.png`（0 / 2px / 6px / 继承 4px / 负 -1px 五行）。

## [0.16.39] - 2026-11-16 【格式 v30】

**`position: sticky`**（第三个"写常见界面"的缺口）。分组标题、表头、筛选栏滚到顶就钉住、
本组滚完后被下一个标题顶走 —— 桌面端列表/日志/设置页最常见的那套交互，现在写 CSS 就有。

### 加 — `position: sticky`（`ANCHOR_STICKY`，格式不变，仍 v30）

- **留在文档流里**：布局位置照常由流式光标给（不是 absolute），所以兄弟照旧排在它后面、
  高度照旧参与父容器撑高。粘滞只发生在**绘制期**。
- **钳制公式只有一处**（`binui_paint.h::PaintStickyShiftY`）：
  `顶 = 滚动视口上沿 + top`，`底 = 父节点盒下沿 − 自身高`；元素被滚出上沿就钉住，
  父容器滚走时被带走（CSS 的"永不离开包含块"）。位移折进绘制用的 `offY`，于是
  **整棵子树**跟着一起动、后代不用各算一次。
- **命中测试同一套数学**：`hitTestEx` 的祖先链累加里带上同样的钳制，不会出现
  "看得见却点不到"。测试里用"滚 300 后仍在顶部命中 / 滚到底后被父盒带走"钉住这一对。
- **层级按"定位元素"排**：`drawOrder` 把 sticky 与 absolute/fixed 一样放进"流式之后"的
  定位阶段。不这么做的话，钉住的表头会被紧随其后的流式内容**盖住**（表现为"钉住了但看不见"，
  这一条是实测抓出来的）。
- **百分比**按滚动视口高解析（`top:10%`）。**只支持纵向**：`left/right/bottom` 与"没写 top"
  都明确告警（静默忽略等于骗作者）。
- `anchorB` 借来存 top 偏移（它与 absolute 的 bottom 互斥），渲染器重排里那段落位公式
  对 sticky 显式跳过 —— 否则一 resize 表头会跳到容器底部。

### 门禁

`make test`（新增 10 条：`isStickyPositioned` 标记、滚 300 后仍在顶部命中、粘滞不改布局
坐标、`top:10%` 按视口高解析、滚到底被父盒带走、resize 后位置仍由流式决定、两种误用告警）、
`make pixels` 16/16 **0 差异**（没有老页面用 sticky，所以基准图一位没动）、
`make pixels-vk` / `pixels-d3d11` 全 0 不同像素（新增跨后端用例 `sticky`）、
`make partial` diff ≤56/max 1。证据图 `docs/issues/assets/sticky-scroll.png`（滚 0 / 100 / 250）。

## [0.16.38] - 2026-11-16 【格式 v30】

**渐变改为在预乘空间插值**（CSS Color 4 / 浏览器口径）。这不是"精度优化"，是一条
**肉眼可见的观感 bug**：两端 alpha 不同的渐变（`rgba(255,120,80,1)` → `transparent`
这种淡出）以前是"先在直色空间插值、再预乘"，中间段颜色会**一起变暗**；浏览器只让
alpha 变。实测一条淡出渐变的中点：改前 `(71,39,31)`，改后 `(135,69,51)`，
浏览器解析值 `(135,70,52)`（底色 `#101418` 上）—— 见 `docs/issues/assets/grad-fade-space.png`
（上=改前，下=改后）。

### 改 — 插值空间（`PaintLerpArgbGrad` / `PaintGradColorAtPoint` / `PathGradColorAt`）

- 新增 `PaintLerpArgbGrad`：两端 alpha **不同**时在预乘空间插值再除回直色；
  **相同**时原路返回老函数 —— 这一支刻意保证逐位不变，既有像素基准一位都没动
  （16 张 GDI 基准 0 差异）。
- 条带三条通路（任意角度 `BuildFillBandsAngleGrad`、轴对齐逐行/抖动、SVG `PathGradColorAt`）
  全部换成它；抖动版 `PaintLerpArgbGradDither` 在 alpha 不同的那一支**不抖**
  （误差扩散定义在直色通道上，换成预乘空间后硬抖会出彩边）。
- `PaintGradColorAtPoint` 直接算**预乘色**（顶点色要的就是它）：预乘空间里的线性渐变
  仍是屏幕坐标的仿射函数，所以三角形插值依旧精确。

### 顺带 — 淡出渐变也走"四角带色四边形"（`BuildGradientQuads` 去掉 alpha 相同的门槛）

以前为了"两条路插值空间不同"只能让淡出渐变留在条带上。现在两边同口径，**所有线性
渐变（含淡出、含圆角）都是一块四边形**（圆角 9 块九宫格）。12 张 300×240、圆角 14 的
135° 淡出卡片实测：**42,889 条原语 / 5,026 KB / 遍历 76.7 µs → 13 条 / 12.8 KB / 2.7 µs**
（28× 原语、27× 遍历）；`grad-quad` 用例的淡出行也从"每行一条"变成整块插值，
圆弧边缘是模板 AA，不再有逐行台阶。

### 门禁

`make test`（新增：alpha 相同 → 预乘插值与直插逐位等价；淡出中点 alpha=128 且颜色不变；
四边形与条带两条路在同一点必须算出同一个颜色）、`make pixels` 16/16 **0 差异**、
`make pixels-vk` / `make pixels-d3d11` 全 0 不同像素、`make partial` diff ≤56/max 1、
`check_examples` 16 个示例 0 问题。`docs/issues/assets/grad-quad.html` 的注释同步更新
（淡出行不再是"与 GDI 不同"的那一类）。

## [0.16.37] - 2026-11-16 【格式 v30】

写常见界面的第二个门槛：**`position: fixed` 是真的固定定位**（0.16.36 还只是"按 absolute
处理 + 告警"）。标题栏、工具条、右下角悬浮按钮、页面角标这类"不随内容滚动"的 chrome，现在
写 CSS 就能得到浏览器里的那个行为，不再需要"把它塞进一个不滚动的层里"这种绕法。

### 加 — `position: fixed`（`ANCHOR_FIXED`，格式不变，仍 v30）

四条语义一起对上 CSS：

- **含定位盒 = 视口**：`left/top/right/bottom` 与百分比都按**根节点内容盒**解析，不再按父容器。
  编译期静态布局与运行期重排（`setSize` / 局部重排）走同一份口径 —— 规格段复用 0.16.36 的
  `SPEC_ANCHOR_R` / `ANCHOR_B`，只多一个 `ANCHOR_FIXED` 位（`anchorFlags` 的空闲位，字节数
  不变，所以不升格式号）。
- **不吃祖先滚动偏移**：`drawNodesBinUI` 里 `offX/offY` 的继承链在 fixed 节点处**归零**
  （`hasVp` 一并清掉，祖先的裁剪视口也到此为止）。实测（`docs/issues/assets/fixed.html`，
  `offscreen_render --scrollTo 200`）：两帧差异的包围盒正好是滚动容器本身
  `(20,30)-(338,250)`，五个 fixed 元素逐字节不变 —— 证据图
  `docs/issues/assets/fixed-scroll.png`（左=未滚、中=差异像素、右=滚 200）。
- **不被祖先 `overflow` / `<svg>` 裁剪**：`clippingAncestors()` 遇到 fixed 节点就停；fixed
  节点自己也直接返回空链（它只受根视口约束）。命中测试同步：fixed 节点的祖先链只查可见性，
  不累加滚动偏移、不判祖先视口。
- **层级提到根**：`drawOrder()` 把 fixed 节点挂到**根节点的子级**参与排序（子树仍挂在它
  自己下面）。依据是 CSS 的绘制模型：同一个层叠上下文里所有 positioned 元素都画在**流式
  内容之后**，于是"卡片里声明的 fixed 按钮"不会被后面声明的流式兄弟盖住；同为定位元素
  的兄弟仍按 DOM 顺序（要压过它得写 `z-index`，与浏览器一致）。

### 顺带 — 三处配套修正（不改这三处就等于没做）

- `contentHeight()` / `contentWidth()` / `scrollTo()` / `clampScrollOffsets()` 都**跳过
  fixed 子节点**：否则右下角一个 `fixed` 悬浮按钮会把滚动容器的可滚动内容多算出一截
  （滚动条凭空变长、能滚到一片空白）。
- **flex 容器不把 fixed 子项当 flex 项**：`layoutFlex` 的活跃子项里滤掉 `position:fixed`
  （不滤的话，按钮行里一个 fixed 角标会凭空挤出它的宽度），编译期与重排两侧都挡。
- GDI 遗留后端只补最小的一条（`offX/offY` 归零）：visible 位置一致，其余（层级 / 裁剪链）
  本来就在渲染器侧共享。

### 像素门禁

`make test` 新增 12 条断言（落位按视口 640×420、百分比按视口、`isFixedPositioned` 标记、
不算进滚动内容高、`clippingAncestors` 为空、滚动后命中仍在原位、绘制顺序提到流式之后、
`setSize(800,500)` 后重新贴边）。新用例 `docs/issues/assets/fixed.html` 进跨后端比对：
GL vs VK、D3D11 vs GL 均 **0 不同像素**；GDI 基准 16/16 零差异；`make partial` diff 56 /
max 1 通过；`check_examples` 16 个示例 0 问题。

性能：`drawOrder` 里按**下标**直接读规格段（不走 id → 下标映射），实测 `features` 27.0 µs
（改前 27.6）、`appshow` 46.5~47.9 µs（改前 47.4），GL 一帧 0.064 / 0.072 ms —— 均在噪声内。

## [0.16.36] - 2026-11-16 【格式 v30】

写常见界面的第一个门槛：**贴右 / 贴下定位**（`right` / `bottom` / `inset`）。以前只有
`left`/`top`，所以"右下角的浮标""右上角的关闭按钮"只能用硬编码坐标拼，窗口一变就错位。

### 加 — `right` / `bottom` / `inset`（`SPEC_ANCHOR_R` / `ANCHOR_B`，格式 v30）

- **语义**：位置 = 父内容盒边 − 偏移 − 自身尺寸；`%` 相对父内容盒宽/高（`right:20%` 是
  "离右边 20% 宽"）。`inset` 支持 1/2/4 值简写，等价于 `top/right/bottom/left`，
  **长属性优先**（作者同时写了 `right` 和 `inset` 时 `right` 赢，与 CSS 层叠一致）。
- **两处都要算**：编译期静态布局算一次落位；运行期 `setSize` / 局部重排再按规格段的
  `SPEC_ANCHOR_R` / `ANCHOR_B` 重算 —— 只改编译期的话，静态页面对，窗口一拉宽"贴右"
  的元素就留在左边。测试里专门钉了这一对（编译期 640 宽 → `x=500`；`setSize(800)` 后
  → `x=660`）。
- **冲突与误用都明确告警**，不静默二选一：`left`+`right` 同时写（按 left，与 CSS 定宽
  元素一致）、没有 `position:absolute` 却写 `right`、`top`+`bottom` 同时写。
- `right`/`bottom`/`inset` 从"已知不生效"清单里移除。

### 修 — `position: fixed` 现在是**明确告警**，不再静默当成 absolute

它一直按 `absolute` 处理（位置以父容器为基准、并且跟着容器滚动）—— 这条路本身没错，
但"静默"是错的：作者写 fixed 就是想要"不随滚动移动"。现在编译期直接说清楚，并给替代
方案（放进不滚动的层里）。真做 fixed 需要"包含块换成窗口 + 绘制时不吃祖先滚动偏移"，
记进 `docs/future.md` 待办（`sticky` 排在它后面）。

### 顺带 — README 的"不生效 CSS"清单纠错

清单里还挂着 `flex-wrap` / `flex-shrink` / `align-self` / `order` —— 它们在 v22 就已经
实现了（`include/binui_flex_layout.h` 支持换行/收缩/逐项对齐/排序），实测
`display:flex;flex-wrap:wrap` 能正常换行。清单按编译器**实际**告警的集合重写：
`letter-spacing`、`word-break`、`visibility`、`float`、`outline`、`cursor`、
`grid-template-columns`、`text-overflow`、`object-fit`、静态 `transform`、`aspect-ratio`。

### 格式 v30

`BinUISpecNode` 84 → 92 →（补齐对齐）**96 字节**：末尾加 `anchorR` / `anchorB` /
`anchorFlags` / `reserve2[3]`，`SPEC_ANCHOR_R` / `SPEC_ANCHOR_R_PCT` 占用 flags 的第 14/15 位，
bottom 的两个标记放在 `anchorFlags` 里（flags 16 位已经用满）。UI 全在运行时从 HTML 编译，
没有兼容负担。

### 门禁

`make test`（新增 12 条断言：贴右/贴下/两者/百分比/inset/left+right 冲突 + 5 条 resize
重算）、`make pixels`（16 张 GDI 基准 0 差异）、`make pixels-vk`、`make pixels-d3d11`、
`make partial`、`check_examples`。

## [0.16.35] - 2026-11-16

**P0-1「层缓存」实测关闭**（它是欠缺清单里最老的一条），顺带把那几毫秒里真正的大头修掉：
GL 后端在每次 `pushLayer` 里做 `glGetIntegerv(GL_FRAMEBUFFER_BINDING)` —— 一次**同步查询**。

### 量 — 先量，再决定做不做

搭了一个"图层压力页"（4 个 `opacity: 0.6` 的卡片组，每组 420×280，内含渐变卡 + 5 条色块 +
一个按钮），再用同内容、去掉 `opacity` 的版本做对照（走逐元素模式）：

| | 遍历 | GL 绘制(遍历+发射) |
|---|---|---|
| 4 个 opacity 组（改前） | 7.0 µs | **0.097 ms** |
| 4 个 opacity 组（改后） | 7.0 µs | **0.013 ms** |
| 逐元素（无图层） | 5.8 µs | 0.009 ms |

结论有两条：

1. **层缓存的价值上限是 4 µs/帧**（0.013 − 0.009）。层缓存要引入"跨帧纹理复用 + 失效出口"，
   换 4 µs 不划算 —— P0-1 关闭（理由与数字进了 `docs/future.md`，免得以后又从头讨论）。
2. **0.097 → 0.013 ms（7.5×）** 是另一个 bug：`pushLayer` 每层都要问一次"当前绑的是哪个
   FBO"，而 `glGetIntegerv` 会让驱动把排队的命令处理完再回答（drain 流水线）。4 个图层
   × 60 帧 = 每秒 240 次同步点。毛玻璃那条路（`backdropBlur`）里还有一次同样的查询，
   一并改成用自己维护的值（`Impl::curFbo` + `bindFbo()`）；顺带把同处的
   `glGetIntegerv(GL_VIEWPORT)` 也去掉了（那一刻视口恒为 `(0,0,winW,winH)`）。
   **像素零变化**（纯状态跟踪），`pixels-vk` / `pixels-d3d11` 仍逐像素 0 差异，
   `anim/glass`（毛玻璃）也是 0 差异。

### 记 — 一条新规矩（写进 `docs/future.md`）

> GL / D3D 的后端代码里不许出现**同步状态查询**（`glGetIntegerv` / `GetData` / `Map(READ)`）。
> 需要知道什么就自己维护 —— 一次同步就是一次流水线排空，在每帧路径上等于几十微秒。

这一条同样适用于"以后要不要做 dirty 上传/顶点块缓存"的判断：先量 (9)，别先量 (7)。

### 门禁

`make test`、`make pixels`（16 张 GDI 基准 0 差异）、`make pixels-vk`、`make pixels-d3d11`、
`make partial`、`check_examples` 全过。`BINUI_VERSION` 仍 29（纯后端状态管理，不动格式）。

## [0.16.34] - 2026-11-16

任意角度渐变从"逐行 1px 条带"换成**四角带色四边形**（P1-6 剩下的那半，也是 v0.16.33
量出来的唯一一处"每帧上万四边形"）。12 张 135° 渐变卡从 **43,409 条/帧** 变成 **12 个四边形**。

### 加 — `IBinUIGfx::fillGradientQuad`（四角带色 + 圆角九宫格）

- **为什么能一个四边形搞定**：线性渐变在屏幕空间是**仿射函数**（颜色 = `lerp(from,to,
  ((p-s)·a)/len)`），而仿射函数被三角形上的线性插值**精确复现** —— 三个顶点的颜色定死一个
  仿射函数。所以"四角各取一次色"是精确解，不是近似。以前那条路（0.16.13 起）要把一张
  320×180 的 135° 卡片切成 3,694 条 1px 条带，纯粹是"条带只能靠切得足够细逼近渐变"的锅。
- **圆角**走九宫格：角片仍然借用后端那张圆角模板位图（模板 alpha × 顶点色），每块四角
  各自取渐变在**屏幕坐标**上的颜色。与 `fillRounded` 同一套角片，所以弧线 AA 也一致 ——
  顺带修掉了老路径的圆角台阶（条带是把每行横向内缩，硬边；模板是真 AA）。
  见 `docs/issues/assets/grad-quad-aa.png`（胶囊卡片左上角放大 6 倍：左 = 条带，右 = 四角带色）。
- **两端 alpha 不同就不走这条路**（`rgba(...,1) → transparent` 这类淡出）：整套渐变现在
  都是"在直线色空间插值、再预乘"，而四角带色是让 GPU 在预乘后的顶点色空间插值 ——
  只有 alpha 不变时两者逐像素等价。所以 `BuildGradientQuads` 在 alpha 不同时返回 0，
  调用方退回条带，**淡出渐变的像素一位不变**（"插值空间该用哪个"单独记在
  `docs/future.md`，那是一条会影响所有后端 + 基准图的改动，不混在这一版里）。
- **三端共用一份几何**：`include/binui_paint.h` 的 `BuildGradientQuads` 一次算好 1 或 9 个
  四边形（含四角**预乘**颜色、模板 UV），三个后端只负责写顶点 —— 顶点色是逐顶点属性，
  着色器一行都不用改，也没有新着色器。
- **不支持就退回**：`fillGradientQuad` 默认返回 `false`（GDI 遗留路径、或圆角模板缺失），
  调用方落到原来的条带路径 —— 所以 GDI 的像素一位不变（`pixels` 16 个基准全 0 差异）。
  `BINUI_NO_GRAD_QUAD=1` 可强制走老路径做 A/B。
- 顺带把渐变轴算式抽成 `PaintGradAxisOf`（条带与四边形两条路共用），盒子/角度不会两处
  算得不一样。

### 量 — 效果（本机，`make bench-draw` / `make bench-gl`）

| bench-grad（12 张 320×180 的 135° 卡） | 改前 | 改后 |
|---|---|---|
| 原语/帧 | 43,409 | **13** |
| ≈顶点/帧 | 5,087 KB | **12.8 KB** |
| 遍历 µs/帧 | 76.6 | **2.8** |
| GL 绘制(遍历+发射) | 0.288 ms | **0.007 ms** |
| GL 上传+绘制调用 | 0.138 ms | **0.003 ms** |

其它页面不变（features 27.6 µs、appshow 47.4 µs 上下）。也就是说：**135° 渐变卡片的绘制
成本从"每帧 0.43 ms + 5 MB 顶点"变成"0.01 ms + 13 KB"**，而且是连续色阶（不再有 96 级
量化台阶），圆角边缘还多了真 AA。

### 门禁

- `make test`：新增 22 条断言直接钉住数学 —— 角度语义（0/90/180deg 的四个角必须是
  `from`/`to`）、对角角度下四角投影到渐变线两端、**盒内任一点按三角形插值还原的颜色与
  解析渐变差 ≤ 1 LSB**、九宫格面积和 = 盒子面积、UV 不越界、缺模板返回 0、半透明角色已预乘。
- `make pixels-vk` / `make pixels-d3d11`：GL / VK / D3D11 **逐像素 0 差异**（含新用例
  `grad-quad`：135/45/225/315°、圆角 0/8/24/胶囊、小数坐标、端点 alpha 1→0 的淡出）。
- `make pixels`（GDI，仍走条带）：16 个基准全 0 差异。

### 已知差异（记录，不是回归）

- 圆角弧线：老路径是"每行横向内缩"，弧是**硬边台阶**；新路径用圆角模板，是真 AA。
  所以同一张卡片的圆角边缘像素会变（最多 ~90/255 的覆盖率差），肉眼就是弧线顺了。
  这是修对了，`docs/issues/assets/grad-quad-aa.png` 有 6 倍放大的对照。
- GDI 仍是条带（遗留后端），所以 GDI 与 GPU 在圆角边缘不再逐像素相同；跨后端比对
  （GL/VK/D3D11）仍是 0 差异。`pixels` 的 16 张 GDI 基准全 0 差异（基准图由 GDI 生成）。

## [0.16.33] - 2026-11-16

"缓存 + 脏节点"（P0-1/P0-4）的第一步：**先把一帧的 CPU 绘制变成可测的东西，再按量出来的
数字决定做什么**。结论和预期不一样 —— 最大的单笔不是条带生成，而是节点循环里的一处
`std::getenv`（实测 **900 ns/次**，每个节点一次）；拔掉之后中等页面的遍历只剩几十微秒。
"把上千条条带合成一次调用"这个方向也试了，实测 3.7%（噪声内），已撤回 —— 真还在
"上万四边形"级别的只剩对角渐变的条带本身，去向写进 `docs/future.md`。

### 加 — `tools/draw_bench`：绘制遍历的 CPU 基准 + 原语流哈希

- 一个**只计数不上屏**的 `IBinUIGfx` 实现 + 真布局（`CompileBinUIFileEx` → `loadLayout`），
  量"遍历一帧"的 µs、各类原语次数、≈四边形数、≈顶点 KB；无 GPU、无窗口、确定性，
  所以同一个数字在 GL / VK / D3D11 三端之间可比。
- `--hash`：把整条原语流（顺序、坐标、颜色、UV、裁剪、批次边界）做成 64 位哈希。
  像素基准只能证明"看起来一样"，这个能证明"发出去的每个四边形都一样" ——
  做绘制缓存时这是唯一能证明"没有因为复用而画错一帧"的判据。
- 配套 `--clip`（模拟脏矩形）/ `--scrollTo` / `--hover` / `--text` / `--json`。
- 新目标 `make bench-draw`（全部示例 + 条带爆炸用例）与 `make bench-gl`
  （真实 GL 后端，遍历+发射 / 上传+绘制调用 分开报）。后者靠 `gl_shot --bench N`，
  不动窗口、不出图、同一进程同一个 GL 上下文。
- 新增基准资产 `docs/issues/assets/bench-grad.html`：12 张 320×180 的 135° 渐变卡，
  专门压"条带爆炸"这一唯一上万四边形的场景。

改前那些数字（1052 µs/帧、9800 个四边形）是手测的口口相传，现在已经能一条命令复现，
基线表落在 `docs/future.md`「绘制成本现状」。

### 修 — 节点循环里的 `std::getenv`（遍历 2.5~4.5×）

`clip-path`（0.16.24）和 `mask`（0.16.26）的调试打印写在**节点循环里**：
`if (std::getenv("BINUI_DEBUG_CLIPMASK") && ...)`。UCRT 的 `getenv` 要遍历环境表，
本机实测 **896~928 ns/次** —— 每个节点白烧近 1 µs，正好是当时的全部"每节点固定开销"
（同一个坑文件顶部 0.16.0 就记过一次，那次是 `BINUI_DEBUG_CLIP`）。
现在和 `dbgClip` 一样只查一次（`static const bool`）。像素零变化（开关未设时行为相同）。

| 页面 | 节点 | 改前 | 改后 | |
|---|---|---|---|---|
| features | 85 | 101.8 µs | 27.6 µs | 3.7× |
| buttonshow | 66 | 92.6 µs | 37.6 µs | 2.5× |
| animshow | 132 | 152.3 µs | 38.4 µs | 4.0× |
| appshow | 308 | 125.2 µs | 47.4 µs | 2.6× |
| svgdemo | 65 | 68.0 µs | 15.0 µs | 4.5× |
| texturedemo | 40 | 53.7 µs | 21.3 µs | 2.5× |

### 试过并撤回 — 条带批量原语（`fillBands`）

第一版还想顺手给后端加一条"一次发 N 个条带"的原语：渐变底/边框在 CPU 侧被切成几百到
几千条 1px 矩形，逐条 `fillRect` 每次都要过虚调用 + 容量检查 + 批次检查，看着像白白浪费。
实现了 `IBinUIFillQuad` + 默认循环 + GL 快路径（容量/批次检查提到循环外，循环体只剩
premultiply + 写 6 个顶点），四端像素逐位相同 —— 然后 A/B 量了一下：

| bench-grad（43,409 条/帧） | 绘制(遍历+发射) | 上传+绘制调用 |
|---|---|---|
| 逐条 `fillRect` | 0.299 ms | 0.161 ms |
| `fillBands` 批量 | 0.288 ms | 0.138 ms |

**3.7%，在噪声内。** 因为条带贵在"写那 6 个顶点"，不贵在虚调用 —— 后端两家（D3D11/VK）
的 `fillRect` 也都是三步就进 `emitQuad`，没有额外的状态切换可省。所以**撤回**，不留一个
没有收益的契约面（改动只在 `binui_gfx.h` / `binui_format.h` / `binui_paint.h` / GL 后端，
`git` 里已复原）。结论写进 `docs/future.md`：要压这一页，只能减**四边形数量本身**
（四角带色四边形，或整块做成图层纹理），"减少调用次数"这个方向不用再试。

### 加 — `gl_shot --bench N` 与 `BinUIGLGdi::flushFrame()`

量真实后端的一帧要把两段分开：「遍历+发射」和「上传+绘制调用」。后者走新加的
`flushFrame()`（只 `flushAll`，不做 MSAA resolve、不进交换链）—— 一旦 `present()`
进交换链，窗口可见时会被合成器节流到 **8.2 ms/帧**（半个刷新周期），量到的就不是
CPU 成本了（第一版就踩了这个坑，数字差 20 倍）。

### 门禁

`make test`（含 C ABI）、`make pixels`（16 个 GDI 基准，全部 0 差异）、
`make pixels-vk`（0 差异，`anim/affine-text` 6 px 为已知容差）、
`make pixels-d3d11`（同上）、`make partial`（4 个 hover 点 diff ≤ 56 / max 1）、
`check_examples`（16 个示例 0 问题）、`make bench-draw` / `make bench-gl` 全过。

## [0.16.17] - 2026-11-15

后端支持级别定案：**GDI = 遗留（OLD）后端，后面只做 Vulkan / GL / D3D11**（master 决定）。
这不是代码改动，是一次"把边界写清楚"的收口 —— 顺手把 GDI 最后一个已知问题记录归档。

### 定 — 后端两级：主线三端 / GDI 遗留

| 级别 | 后端 | 承诺 |
|---|---|---|
| 主线 | OpenGL / Vulkan / D3D11 | 新特性只在这里做；三端共用 `binui_draw_nodes.h` + `binui_glyph_atlas.h`，**逐像素相同**是硬约束 |
| 遗留（OLD） | GDI（`drawNodesGDI`） | 只保证 ① 不崩 ② 不阻塞（无 GPU / 远程会话 / 全屏直翻转的后备呈现） ③ 能出**像素基准图**。不再新增特性、不再修观感类问题 |

**为什么**：GDI 的实际职责只有两件 —— 后备呈现、生成 `docs/pixels/` 基准（无 GPU 依赖、
确定性最好，这条要留着它）；而它的能力缺口是结构性的（没有 alpha 合成 / 字形图集 / AA 填充 /
离屏图层），每加一个特性都得为它另写一条路（`text-shadow` 就是 270 行烘位图 + `AlphaBlend`），
换不来临界收益。**例外照旧修**：崩溃、卡死、泄漏，以及"静默画错到没法看"（例如 0.16.14 的
`RoundRect` 右/下各短一行导致环与环之间漏一圈亮线、透明条带把兄弟阴影擦成白块，0.16.16 的
GDI 文字阴影 `AlphaBlend` 源位图未选中导致阴影整块不出现）—— 那些不修，GDI 基准图就失去意义。

### 记录 — GDI 最后一个已知问题（不修）

`docs/issues/09-gdi-text-clip.md`：GDI 的 LABEL 文字矩形是 `{x+pad, y, x+nw-pad, y+nh}`
（纵向也扣了 padding），`DrawTextW` 按矩形裁剪 —— `div` 默认 `padding:8`，字号一大
（比如 22px 字塞进 34px 高、padding 8 的盒子 → 文字矩形只剩 18px）上下就被切掉；GPU 三端
只裁节点盒，所以完整。复现资产 `docs/issues/assets/gdi-text-clip.html` + GL/GDI 两张对照图。
同文档末尾还列了其余 GDI 已知差异（阴影环吸整像素、组透明度逐元素近似、无 ¼ 像素相位）。

### 同步 — 文档/注释里的"支持级别"要能读到

- README：后端表里 GDI 标成 **遗留（OLD）**，新增「后端支持级别（v0.16.17 起）」一节；
  文件地图与 `src/platform/app_win32.*` 一行同步标注。
- `docs/future.md`：新增「后端支持级别（v0.16.17 定案）：GDI = 遗留（OLD）」一节 ——
  两级承诺、"为什么这样定"（三条理由）、"遗留不等于放任"（哪类问题照旧必修）、
  已知差异表、"对欠缺清单的影响"（P0/P1 里提到 GDI 的条目按"只做主线三端"读）。
- `CONTRIBUTING.md`：硬规矩第 1 条补一句"新特性只做 GL/Vulkan/D3D11 三端"，
  文件树里 GDI 标成遗留。
- 代码：`src/platform/app_win32.h` 的 `drawNodesGDI` 声明与 `app_win32.cpp` 的实现处
  都加了 OLD 横幅（后来者改到这个函数第一眼就能看到边界）。
- `docs/issues/INDEX.md`：新增 09 一行（WON'T FIX），状态从"7 条全部已修复"改成
  "01–08 已修复 + 09 不修（遗留后端）"。

## [0.16.18] - 2026-11-15

文字/图片参与仿射（P1-9）。以前旋转只转"底/边框"：卡片转 20°，里面的字和图标还是竖的 ——
这是三端统一之后最明显的观感缺口。**格式不变**（纯接口 + 绘制层，不动 `.bui`）。

### 加 — `IBinUIGfx::setAffine(xf)` 状态（不是逐四边形参数）

- 语义：设置之后**接下来所有原语**的四边形都过同一个 2×3 矩阵（屏幕 = `a*x + c*y + e`,
  `b*x + d*y + f`），传 `nullptr` 关闭。为什么是状态：文字是几百个字形四边形、图片带圆角时
  是几十条横带，逐四边形传矩阵不现实。
- 三个 GPU 后端各改**一处**：`emitQuad` 里判到仿射就改用已有的 `emitQuadUV`（任意四角 +
  每角 UV），`emitQuadRot`（`fillLine` 的旋转四边形）同理；`fillRoundedXf` 自带矩阵，
  显式**不**过状态（否则圆角底会被变换两次）。
- `drawNodes`：仿射节点里整段改用**节点局部坐标**（`x=y=0, nw=lw, nh=lh`），矩阵负责转到
  屏幕 —— 于是文字、图片、文字阴影、边框、渐变条带、复选框这类"附带几何"全部一起变换；
  裁剪/剔除仍用变换后的 AABB（先算，再换坐标系）。
- 能力探测：`setAffine` 返回 `false` 的后端（GDI 没有任意四边形）→ 完全退回 0.16.17 的
  "只转盒子"行为，坐标保持 AABB。所以**老页面像素一位不变**（16 条 GDI 基准 0 差异）。
- 代价写清楚：字形是**位图旋转**（双线性采样），大角度/大字号下边缘略软；真"矢量旋转"要
  GPU 栅格化字形（`docs/future.md` P2 已把"文字次像素渲染"划为不做，同源）。

### 验收

- 新用例 `docs/issues/assets/affine-text.html`（0°/20°/-12°/`scale(1.4,0.7)` + 卡片内文字、
  图片、文字阴影、卡片阴影、28px 粗体标题），进 `ANIM_CASES`：**GL vs VK / GL vs D3D11
  0 不同像素**（640×380，容差 0%，最大通道差 18）。
- `make pixels` 16/16 零差异（老页面逐位不变）、`pixels-vk` / `pixels-d3d11` 全 0、
  `make test` 全绿、`check_examples` 16 示例 0 问题。

## [0.16.19] - 2026-11-15 【格式 v25】

九宫格图片（P1-8 的尾巴）：`image-slice: <内缩像素>`。**格式升到 v25**（新增 4 字节/条的
「绘制附加段」）。UI 里"带边框/带圆角的贴图容器"（按钮底、卡片框、气泡）都靠它 ——
一张 48×48 的框拉伸到 300×70，普通整图拉伸会把 2px 边框拉成 20px、圆角压扁。

### 加 — 切分数学在共享头里，切分点在后端算

- `BuildNineSlice(x, y, w, h, srcW, srcH, slice, out[9][8])`（`include/binui_paint.h`）：
  输出 9 个 `{u0,v0,u1,v1,x,y,w,h}`。三端共用这一份数学 ⇒ 九宫格像素必须一致。
  内缩被**两端同时**钳制（源图方向不超过半张图、目标盒方向不超过半盒），所以
  "盒比内缩还小"不会切出负宽度，只是中间那块被压扁。
- 为什么切分点放在后端（`IBinUIGfx::drawImageNine`）：切分要**源图像素尺寸**，只有解码过
  图片的后端知道（UV 是归一化的）。默认实现退回整图拉伸 —— GDI 这类遗留后端不必为此改，
  不支持时表现为"框被拉粗"而不是"什么都不画"。
- `BinUIPaintEx`（v25，4 字节：nodeId + imageSlice + flags）单独成段理由同 v23/v24：
  `BinUIPaintNode` 的 `reserve[3]` 三个字节已经全部用完（opacity / 渐变角度 / 图片亮度），
  48 字节的结构无法再加字段。段序：… / 路径 / 阴影 / 文字阴影 / **绘制附加** / 名称。
- 编译期：`image-slice` / `nine-slice` 两种写法；只支持**四边同宽**、上限 255px、
  百分比忽略、四值不等取第一个（都走 `Warn()`）；与 `border-radius` 互斥 —— 九宫格优先、
  圆角清 0 并告警（九宫格的圆角通常已经烘进贴图，再裁一层只会把四角切掉）。

### 验收

- 新用例 `docs/issues/assets/nine-slice.html`（48×48 九宫格源图，7 组对比：整图拉伸 vs
  slice 12/20/16、100×100 方盒、560×36 通栏、带 `brightness(0.55)`），进 `ANIM_CASES`：
  **GL vs VK 0 不同像素（最大通道差 1）、GL vs D3D11 0 不同像素（最大通道差 0）**。
- `make pixels` 16/16 零差异、`pixels-vk` / `pixels-d3d11` 全 0、`make test` 全绿、
  `check_examples` 16 示例 0 问题。
- `test_css` 新增 17 条断言：`imageSliceOf` 解析（缺省 0 / 12 / 与圆角互斥且圆角清 0 /
  百分比 / 钳到 255 / 四值取第一）、9 块、四角保尺寸且 UV 0..0.25、中心块拉伸且 UV 0.25..0.75、
  每行宽度之和 = 盒宽、横向 UV 首尾相接、首末块 UV 到 0/1、内缩钳到源图半张、
  盒比内缩小不出现负宽、退化输入返回 0。

## [0.16.32] - 2026-11-16

**修的 bug：hover 之后沿脏矩形冒出一圈"影子 / 边框"**（GL / Vulkan / D3D11 / GDI 四端都有）。
完整体检报告与复现资产见 [`docs/issues/10-partial-redraw-cull.md`](docs/issues/10-partial-redraw-cull.md)；
格式不变（`BINUI_VERSION` 仍是 29）。

### 修 — 局部重绘的剔除判据写反了一侧

绘制遍历里"这个节点要不要画"的快速判据，四条边只有左/上两条是对的。右/下两条写成了
`x + pad >= clipR` / `y + pad >= clipB`，判的是**"节点的左/上边进入脏矩形右/下边 pad 以内"**，
于是**紧贴脏矩形边缘、明明还有几列/几行可见**的节点被整块丢掉（底色 + 边框 + 阴影全没）。

脏矩形内部已被 `beginFrame(clip)` 清成画布底色，于是那块区域变成"底色 + 邻居的阴影"，
边界正好是脏矩形的直边 —— 视觉上就是一圈硬边的浅色块，像影子又像边框。

**为什么一直没被发现**：整帧重绘时 `clip` = 整屏，四条边的边界情况碰不到任何节点，所以
16 张像素基准、三端 diff 一路是 0；只有**局部重绘**（`invalidRect` → `InvalidateRect(脏矩形)`
→ `WM_PAINT` 把 clip 传进绘制）才会踩到，而 hover 正是最常见的触发场景。

### 改 — 判据收成一份实现 + 脏矩形把阴影算进去

- `include/binui_paint.h` 新增 **`PaintNodeInClip()`**：四边一律"完全在外才剔除"。三个 GPU
  后端（共用 `binui_draw_nodes.h`）与 GDI（`app_win32.cpp`）都改成调它，不再各写一套；
  文字阴影外扩（`tsPadX/tsPadY`）也并进 pad（以前只算 box-shadow）。
- 新增 **`PaintShadowPad()` / `PaintTextShadowPad()`**：阴影外扩公式只留一处 —— 绘制（裁剪/剔除）
  与失效（脏矩形多大）用同一份数字。
- `BinUIRenderer::invalidRect()`：**把 box-shadow / text-shadow 的外扩算进脏矩形**（以前只加
  `pad=2`，hover 时"新长出来的阴影"根本没重绘 → 阴影边缘留一圈旧像素）；外扩跟着
  `drawScale` 一起缩放（过渡途中也够），有阴影时再 +1px 余量，并把结果**向外取整到整像素**
  （调用方 `(int)` 截断从此是无操作）。

### 加 — 局部重绘一致性检查（`make partial`）

新工具 [`tools/partial_shot.cpp`](tools/partial_shot.cpp)：先整帧画一帧 → hover 指定坐标 → 推进过渡
→ 只重画 `invalidRect` 算出的脏矩形 → 再把整帧重画一遍，**逐像素对拍**同一状态下的两条路径。
非 0 退出 = 不一致。阈值 `diff<=64 且 max<=1`（残差是 MSAA 解析在脏矩形边界上的 1 单位噪声）。

```
make partial      # 4 个 hover 点：大号 / 图标按钮 / 主操作 / 链接按钮
```

修前同一用例：`diff=5502 max=16`（成片的底色/阴影差十几）；修后：`diff=4~56 max=1`。

### 加 — 单测

`tests/test_elements.cpp` 的 `suite_hover_dirty` 增加 13 条断言：
`PaintNodeInClip` 的 9 个边界用例（含"紧贴右/下边界、有一列/一行可见 → 必须画"这两个回归点）
+ `invalidRect` 含阴影外扩的 4 条（宽/高/左/上）。

### 记录 — 教训

**"整帧渲染对"与"局部重绘对"是两条不同代码路径，必须各自有基准。** 这个 bug 从 0.16.9 引入
局部重绘起活到 0.16.31，期间三端像素 diff 全绿、示例一切正常 —— 因为所有基准都只覆盖整帧。
`make partial` 现在把另一条路也钉住了，而且它的比较对象是"同一状态下的整帧渲染"而不是基准图，
样式改动不会让它失效。

## [0.16.31] - 2026-11-15

三层的**性能轮**（binUI 本体仍然一行代码没改，只同步文档与版本号）：`binGPU` → **0.2.1**、
`bin2D` → **0.1.1**、`bin3D` → **0.1.1**。三条都是"同机前后对比 + 行为不变"的口径，各自的
`docs/perf.md` 有完整数字与"试过但放弃"的记录。

### 快 — binGPU 0.2.1（CPU 参考后端，像素裁判提速）

| 场景（1024²） | 0.2.0 | 0.2.1 | 加速 |
|---|---|---|---|
| 纯色 · 预乘 | 18.13 ms | **2.50 ms** | **7.3×** |
| 纯色 · replace | 16.95 | **2.56** | 6.6× |
| 纯色 · keep / erase / additive | 18.1 / 18.2 / 17.7 | **3.76 / 3.65 / 3.82** | 4.6~5.0× |
| 纹理 · nearest / linear | 29.2~53.2 | **16.0~27.6** | 1.6~2.3× |
| scissor 内小批 | 7.19 | **1.10** | 6.5× |
| texture_read | 3.62 | **2.20** | 1.6× |

纯色填充 **57.8 → 419.9 MPix/s**。做法：分路径（纯色不算 UV、不测深度不插 Z）+ 混合模式模板特化
+ 均匀色三角形**混合查表**（4×256 置换表，表项与逐像素同一条公式）+ 采样器状态外提/内联 floor/
REPEAT 取模快路径 + 反预乘查表 + 只给 CPU 后端开 `-O3`。**13 个 FNV-1a 校验和与 0.2.0 逐位相同**。

### 快 — bin2D 0.1.1（光栅与合成）

六项 512² 基准合计 **190.6 → 43.1 ns/px（4.43×）**：圆角纯色 12.25→2.51（**4.9×**）、星形路径
5.07→1.50（3.4×）、线性渐变 18.05→6.42（2.8×）、位图 2× 放大 35.21→12.24（2.9×）、组透明度
图层合成 41.42→4.73（**8.8×**）、盒式模糊 r=8 78.56→15.66（5.0×）。做法：扫描线改成"边数组 +
活动边表 + span 三拆（左端/整像素段/右端）"、合成分派到模板特化循环、位图 1:1 直拷与缩放查表、
模糊前缀和 O(1)/像素、裁剪遮罩惰性分配。`/255` 换移位实现前**穷举验证与 /255 逐值相等**。
顺带修掉一个观感 bug：**1:1 的 `drawSurface`/`drawImage` 原来走双线性**，等于整体偏半纹素 +
轻微模糊（图层内容其实一直被糊过一遍），现在整数直拷。

### 快 — bin3D 0.1.1（软光栅）

| 分辨率 | 0.1.0 | 0.1.1 | 加速 |
|---|---|---|---|
| 320×240 | 2.19 ms | **1.03 ms** | 2.1× |
| 640×480 | 8.50 | **3.91** | 2.2× |
| 1280×720 | 24.8 | **10.1** | 2.4× |

27 → **11~12 ns/像素**（91 MPix/s @720p）。起点数据表明瓶颈不在几何而在**着色（71%）**，且 bbox
要试 34 个像素才产出一个 → 最大两笔是「按行解 x 区间」（−1.91 ms）与「常量法线光照提升」
（−1.68 ms），其余是模板分流、深度先判、32 位清屏等。**demo 图 SHA-256 与 0.1.0 完全相同**、
164 条断言一字未改。

### 记录 — 这轮主动放弃的"更快写法"

优化轮最值得记下来的不是增速，是**为守住"行为不变"放弃了什么**：

- bin3D 试过「仿射平面求值」：640×480 能再快 17%，但深度差 1 ulp → 地面与网格线共面处"谁在上"
  翻转，**3714 个像素可见差异（最大通道差 44）**；又试过「顶点色三顶点相同提升」（−0.56 ms），
  575 个像素差 1。两条都写进 `docs/perf.md` 的"试过但放弃"，免得下一轮有人再走一遍。
- binGPU 放弃了「边函数增量递推」与 SIMD：浮点累加/重排会改边界像素，与"逐位不变"冲突。
- 这类取舍的判据写在三个仓库的 roadmap 里：**2D/CPU 路径守逐位一致；3D 要动验收规则（覆盖率/
  轮廓容差）之后才允许这类改写**。

### 修 — 顺带的安全问题

- binGPU：`R8` 纹理当渲染目标会**写越界**（`px_store`/clear 恒写 4 字节，R8 每像素 1 字节），
  现在 `texture_create` 明确拒绝 `R8 + RENDER_TARGET`（R8 仍可作采样遮罩）。这是内存安全问题，
  不是像素语义。

## [0.16.30] - 2026-11-15

**语言分工修订**：`binGPU` 的实现从 C11 改成 **C++17**，对外的 ABI **一位未动**（仍是 C 兼容的
扁平函数，`BINGPU_ABI_VERSION` 还是 1）。binGPU 到 **v0.2.0**；binUI 本版仍然只动文档 +
版本号，`.bui` 布局与 `BINUI_VERSION`（29）不变。

### 改 — 为什么把"语言"和"ABI"拆成两件事

之前定的分工是"最底层用 C"，理由是"底下三套 API 本来就是 C + 要给别人绑"。写下来发现前两条
理由站不住：

- **绑定的理由是 ABI 的，不是语言的**：Python / Go / Rust 吃的是 C 符号，而"用 C++ 实现、导出
  C 符号"是 Vulkan loader / HarfBuzz / bgfx / Skia 的常规做法 —— 两者根本不冲突；
- **这一层要的全是 C++ 的主场**：句柄表、容器、生命周期、错误串拼接。用 C 写等于手写
  `std::vector`（realloc + 容量字段）和两遍析构路径，收益为零、维护成本压在每一处；
- **"C 更快"早测过不成立**（`docs/future.md` 里的实测：同一份混合内核 gcc/g++ 生成的汇编逐行相同，
  差别只在能不能内联取样器）。

于是现在：**四层实现全是 C++17**；唯一保留的边界是 `bingpu.h` 的 **C 兼容 ABI** —— 它必须能被
纯 C 编译器单独编过（`make abi-check` 用 `gcc -x c` 编），因为别人要绑的就是这一圈。

### 加 — binGPU 0.2.0 实际改了什么

- **后端变成 C++ 类**：`class Backend` 纯虚接口 + `BackendBase`（两个后端共用的状态机/校验/计数）
  + `NullBackend` / `CpuBackend`。共用一份状态机意味着两个后端**结构上不可能**校验不一致 ——
  这是 0.1.0 里靠"人肉对齐"的地方。
- **RAII 对象表**：`vector<unique_ptr<ObjectBase>>`，析构管资源，`destroy` 只负责摘槽；代数失效、
  设备连带清理、销毁前清空别人指向它的裸指针都收到一处。
- **错误槽换 `thread_local std::string`**；错误串的 `file:line` 现在指向真正的失败点（后端 .cpp）。
- **异常绝不越过 ABI**：头是冻结的 C ABI（声明里不能写 `noexcept`），所以每个入口用顶层
  try/catch 达成同一语义：`bad_alloc` → `BGPU_ERR_OUT_OF_MEMORY`，其余 → `BGPU_ERR_BACKEND_FAILED`
  并把 `what()` 写进 `bingpu_last_error()`。
- **代价写清楚**：静态库会带 libstdc++（构建侧 `-static-libstdc++` 静态链进去，产物自包含）。
  用 C ABI 绑它的语言有三条路：用 C++ 驱动链接 / 自己加 `-lstdc++` / 只 include 头（头本身零
  C++ 依赖）。`make abi-check` 新增第三条正是这个场景的冒烟：**C 目标文件 + C++ 静态库 → g++
  链接 → 跑起来打印 `0.2.0/ABI 1`**。
- **断言数与像素行为一条没少**：`make test` 仍是 **239 + 334** 条全绿；bin2D 的 GPU 往返用例
  对着新库重跑仍是**最大通道差 0**。

### 修 — 顺带暴露出来的 0.1.0 实现 bug（都是"重写时才发现旧写法兜不住"）

1. NULL 后端 `stats.bytes_live` 只加不减（析构漏回写）；
2. 帧中途 `destroy` 管线/纹理是 use-after-free（cmd 存裸引用）→ 现在退化成 `BGPU_ERR_STATE`；
3. `draw_indexed` 索引越界会"画一半再报错" → 先整段校验，要么全画要么不动；
4. `vertex_offset` 的对齐校验只在 NULL 分支里有；
5. 交换链销毁后，借出去的帧目标纹理还持有悬空别名 → 现在一起失效；
6. 错误串的 `file:line` 指向上层入口而不是真正的失败点。

### 同步 — 文档

- `docs/future.md`：「语言分工」整节重写（ABI 与实现分开、硬约束按 ABI 面/实现面分列、libstdc++
  这条代价），binGPU 章节的语言描述、四层关系图、落地清单的语言列一起改；
- `README.md`：生态表里 binGPU 的语言改成 "C++17 / C 兼容 ABI"；
- `docs/README.md`：索引行同步。

## [0.16.29] - 2026-11-15

生态地基：`bin2d` / `bin3d` / `binGPU` 三个项目**落地到 v0.1.0**（都在 binUI 的**同级目录**、
各自独立仓库、各自版本号）。**本版 binUI 一行代码都没改**：`.bui` 布局与 `BINUI_VERSION`（29）
不变，只有文档同步 + 版本号推进。

### 加 — 三个仓库的地基（各自仓库，不在本仓库里）

| 项目 | 语言 | 已有 | 验收（本机实测） |
|---|---|---|---|
| `../binGPU` | C11 / C ABI | `bingpu.h` 定稿 ABI（`uint64_t` 句柄、`struct_size` 版本化 POD、无回调、`bingpu_poll` 轮询）+ NULL 后端（只校验+计数）+ **CPU 参考后端**（五种混合、采样器、scissor/viewport/affine、离屏、float 深度、索引网格、反预乘回读）+ 后端注册表（VK/D3D12/GL 登记为未实现） | `make test` **573 条断言**、`make abi-check`（纯 C 编译器单独编头 + 禁 C++ 设施）、零警告 |
| `../bin2D` | C++17 | 四个 header-only 地基头（core/surface/transform/png）+ paint/path/raster（覆盖率 AA 填充与描边、圆角、渐变、位图、九宫格、盒式模糊）/canvas（变换栈 + 裁剪栈 + 图层栈 + 5 种混合）/scene（Group/Shape/ImageShape）+ binGPU 桥 | **94 条断言**、`make demo` 出图、**与 binGPU CPU 后端往返逐像素 0 差**（最大通道差 0）、`make layers` 边界检查 |
| `../bin3D` | C++17 | math（列主序 Mat4 / 右手系）+ camera + mesh（**36 字节顶点 = `BgpuMeshVertex` 逐字段同布局**）+ scene（节点级联 / 材质 / 平行光 + 环境光）+ SoftRenderer（六平面齐次裁剪、1/w 透视校正、z-buffer、Lambert）+ GPU 阶段的桩 | **164 条断言**、三格并排出图、1280×720 一帧 ≈22.6 ms（≈24 ns/像素，瓶颈在像素不在三角形）、`make layers` |

### 记录 — 验收口径必须分家

binGPU / bin2D 是确定性 CPU 路径，继续用"逐像素 0 差"；**bin3D 不能用 0 diff**（浮点光栅化
顺序不同，z 竞争处必然差 1 个像素）—— 容差规则写死在 bin3D 的 roadmap 里：轮廓容差 ≤1px、
8×8 分块覆盖率 ±2%、平坦区色差 ±2、禁止在 z 竞争处取样。这条不写死的话，"三端像素一致"
会把 3D 拖成永远合不进去的分支。

### 记录 — 迁移还没开始（别误读成"binUI 已经用上 bin2d"）

binUI 现在**仍然**用自己那份光栅数学（`binui_paint.h` 的条带 + `binui_path.h` 的几何）。
bin2d 的第②步才是"binUI 改吃 bin2d"，验收线照旧：搬完必须 GL / VK / D3D11 **三端像素基准
0 diff** + 示例无回归，功能扩展另开一版。两边暂时各有一份填充/描边代码的原因写进了
`docs/future.md`：binUI 那份绑着 `BinUIPaintNode`，是节点的产物；bin2d 那份是节点无关的画布接口
—— 合并时删掉的是**条带生成**那一层，不是像素语义。

### 同步 — 文档与徽章

- `docs/future.md`：从"未实现的方向性记录"改成"**地基已落地**"，新增「地基落地清单（v0.1.0）」
  一节（三个项目已有的东西 / 验收数字 / 迁移状态 / 验收口径差异）；
- `docs/README.md`：把 `future.md` 收进索引（以前**故意**不收，因为没有一行代码）；
- `README.md`：新增「生态：bin2d / bin3d / binGPU」一节；顺手修掉两枚**过期徽章**
  （version 写着 `0.9.1`、format 写着 `v19`，实际早就是 0.16.x / v29）。

## [0.16.28] - 2026-11-15

毛玻璃的两件"苹果级"收口：**材质预设**与**颗粒**。格式不变（颗粒强度复用 v29 毛玻璃
记录的 `flags` 高字节）。

### 加 — `material: glass / smoked / thin`

一句声明就是一块玻璃：

| 预设 | 模糊 | 底色 | 边 | 颗粒 |
|---|---|---|---|---|
| `glass` | 24px | 白 56%→26% 竖向渐变 | 1px 白 62% | 0.7 |
| `smoked` | 20px | 深灰 58%→36% | 1px 白 20% | 0.5 |
| `thin` | 12px | 白 34%→16% | 1px 白 42% | 0.4 |

规则是 CSS 的"层叠"直觉：**只补作者没写的声明**，所以 `material: glass; border-radius: 40`
就是"预设 + 改圆角"，`backdrop-filter` 也可以整条覆盖。预设另外补 `border-radius: 24` 与
一枚柔和投影（浮起来才像玻璃）。未知预设名告警忽略。

### 加 — `grain(<0..1>)`（`backdrop-filter` 里的扩展记号）

模糊面在渐变背景上会有 banding；撒一层极淡的颗粒就压住了。做法：

- 64×64 噪声，**确定性 LCG**（固定种子，三端同一张图）；
- **一个噪声纹素 = 一个屏幕像素**（NEAREST + REPEAT，UV = `x/64`），**屏幕锚定** ——
  元素移动时颗粒不跟着滑，这才是"材质"而不是"贴图"；
- 顶点色 alpha = `grain × 3/255`，最大约 1.2% 白，是"加一层"而不是"盖一层"。

### 踩到的两个坑（都是实拍 / 像素比对抓到的）

1. **颗粒必须画在"恢复预乘混合"之后**：第一版颗粒趟还在"覆盖混合（ONE/ZERO）"状态下，
   整块玻璃被噪声**替换**掉了 —— RGB 看着还行、alpha 掉到 0.45 左右，半透明底一叠就发灰。
   `pixel_diff` 把 alpha 通道也计入，所以是被像素比对抓出来的（RGB 全对、alpha 差一大截）。
2. **UV 不要加半个纹素**：光栅化取像素中心（`x+0.5`），`u = x/64` 时像素中心正好落在纹素
   中心；再加 `0.5/64` 就落到纹素**边界**上，NEAREST 取哪个纹素由实现决定 ——
   GL 与 D3D11 整块不一致（25% 像素不同）。
3. 顺带：`grain(0.7)` 里的括号参数最初被词法器当成"未知函数"报了"只支持 blur()"，
   改成先把 `grain(...)` 从串里抽出来再解析其余函数。

### 验收

- `test_css` 新增 16.28（17 条断言）：`glass` 带出毛玻璃且 blur/圆角/颗粒正确、
  预设补了渐变底与 1px 边、作者写的 `border-radius` 覆盖预设、smoked 的模糊与颗粒、
  未知预设告警且不产生毛玻璃、手写 `grain(0.5)` 强度 ≈128、没写 grain 时 `flags=0`。
- 用例 `anim/glass` 改成"预设为主"的新版（glass / glass+覆盖 / smoked / thin /
  圆形按钮 / 手写无颗粒对照）：`pixels-d3d11` **0 不同像素（最大通道差 2）**、
  `pixels-vk` **0 不同像素**。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题。

### 已知边界

`saturate()` / `brightness()` 仍未做（要片元运算：改 SPIR-V 或 CPU 读回，后者每帧同步
不划算）；仿射节点不模糊；颗粒是屏幕锚定 + 1:1，所以页面缩放时颗粒不缩放（视觉上像
"屏幕质感"，严格说不是 CSS 语义）。

## [0.16.27] - 2026-11-15 【格式 v29】

毛玻璃材质（`backdrop-filter: blur`）：苹果那种"背景被糊掉、上面压一层半透明白 + 1px 高光边"
的卡片。**不动着色器**（VK 的 SPIR-V 一行没改），三端逐像素相同。

### 做 — 离屏抓背景 + 多级降采样

着色器只做"纹理 × 顶点色"，所以模糊不能靠片元采样，而是走**降采样链**：

1. 把元素背后那块背景（当前目标的矩形区域）抓成 level0 纹理；
2. 反复缩小一半：每级都是"覆盖整个小目标、双线性采样上一级"⇒ 一次 2×2 盒式模糊；
   级数 = 让 `2^levels >= 半径` 的最小值（1..6 级），链起来逼近高斯（20px 就 4~5 级）；
3. 合成：`BuildImageBands` 的圆角条带 + 双线性放大，以 **replace（ONE/ZERO）** 混合画回原位
   —— 背景被"替换成模糊版"，而不是再叠一层模糊；随后元素自己的半透明底色/边框照常叠上去。

于是三端只需要一种新的混合状态（GL `glBlendFunc(GL_ONE, GL_ZERO)`、D3D11 第四个 blend state、
VK 一对管线变体），模糊本身全靠固定管线。编译期只写一条 12 字节记录（`BinUIBackdropEntry`：
节点、模糊半径、合成圆角），段序 … / 裁剪 / 遮罩 / **毛玻璃** / 名称。

### 踩到的坑

1. **降采样趟被"批次 scissor"整块裁掉**（D3D11/VK）：三端的批次矩形都取自当前 scissor
   （= 节点的裁剪盒），把小目标画进 60×40 的纹理时 scissor 还是 120×80 的节点盒 → 画的
   全在目标外 → 结果全透明 → 合成出来是纯黑（半透明白叠黑 = 灰卡片）。修法：降采样那几趟
   临时把 scissor 设成 (0,0,dw,dh)，合成趟再换回来。
2. **GL 的 FBO 行序**：GL 的 FBO 第 0 行是屏幕底部，抓下来的纹理要**翻转 V** 才是"区域顶部"；
   D3D11/VK 是自上而下，不能翻。三端只有这一处必须不同。
3. **MSAA 主目标**：抓之前得先解析（GL `glBlitFramebuffer` / D3D11 `ResolveSubresource` /
   VK `vkCmdResolveImage`），否则抓到的是未解析内容。
4. `<div>` 的圆角要写 `border-radius`：`radius:` 是 HTML 属性写法，写在 `style` 里不生效
   （第一版实拍发现毛玻璃卡片是直角）。

### 验收

- `test_css` 新增 16.27（11 条断言）：`blur(24px)` 进段且半径/圆角正确、`-webkit-` 前缀、
  `none` 与 `blur(0)` 无条目并告警、`saturate()` 告警但 blur 仍生效、没写的元素无条目。
- 新用例 `anim/glass`（渐变 + 大字背景下四块玻璃：blur 24 / blur 8 / 圆形玻璃按钮 /
  深色烟熏 + 半透明底 + 1px 高光边）：`pixels-vk` / `pixels-d3d11` **0 不同像素**。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题、
  `vk-window` / `vk-stress` 通过。

### 已知边界

只支持 `blur()`（`saturate()` / `brightness()` 等需要片元运算，告警忽略）；仿射（旋转）节点
不模糊；GDI 遗留后端不参与（照常画半透明底色，不糊背景）；每个毛玻璃节点 = 一次抓取 +
若干次半尺寸绘制，页面上大量毛玻璃节点会有可见开销（后续可以做"同帧同区域只抓一次"）。

## [0.16.26] - 2026-11-15 【格式 v28】

SVG `<mask>`（P1-10 最后一条）：遮罩。白圆遮罩、灰阶半透明、阶梯衰减、遮罩 + 图片、
遮罩 + `clip-path` 嵌套现在都能做了，而且**不动着色器**、三端逐像素相同。

### 做 — 图层里两趟混合

遮罩是"逐像素乘一次 alpha"。做法与 `clipPath` 同一套离屏图层，只是多一趟：

1. **保留趟**：把掩码几何按 alpha 用 `dst *= srcA` 画一遍（白 = 保留、灰 = 压暗、黑 = 通透）；
2. **补集趟**：把"图层矩形 ∪ 全部掩码几何（even-odd）"用 `dst *= 1-srcA` 画一遍 ——
   掩码没盖到的像素被清零（SVG 语义：掩码内容之外 = 黑 = 透明）。

- 为什么必须两趟：`dst *= srcA` **只作用在画了东西的像素**上，掩码外的像素根本不会被碰到
  —— 单靠它整层都留着（0.16.25 做 pattern 时踩过一次，这次直接按两趟写）。
- 亮度在**编译期**折进 alpha（sRGB 系数 0.2126/0.7152/0.0722 × `fill-opacity` × `opacity`），
  所以布局里每条只要"几何 + 一个 32 位颜色"：新段 `BinUIMaskEntry`（44 字节：节点、颜色、
  图层区域 rx/ry/rw/rh、本组几何盒 x/y/w/h、点数/子路径数 + 归一化点）。
- 同一个节点可以有多条（按亮度分组，每组一次 draw）；和 `clip-path` 嵌套时外层裁剪层、
  内层遮罩层 —— 掩码作用在内容上、裁剪作用在合成结果上，与 SVG 语义一致。
- 后端只多一种混合状态：GL `glBlendFunc(GL_ZERO, GL_SRC_ALPHA)`、D3D11 第三个 blend state、
  VK 两条管线变体（`pipelineKeep` / `pipelineLayerKeep`）。**SPIR-V 一行没动**。

### 踩到的坑

1. **`<mask>` 还在 shim 表里**：标签支持了但告警还写着"SVG `<mask>` 不支持"，
   而且 `mask` / `maskunits` / `maskcontentunits` 没进 `UsedHtmlAttrs()` 白名单。
2. **`objectBoundingBox` 的内容容差**：和 `clipPath` 同一个坑（0..1 空间里 0.1 的细分容差
   把 `r=0.45` 的圆切成八边形），内容单位是 OBB 时容差缩到 0.002。
3. **区域默认值**：没写 `x/y/width/height` 时要按 SVG 默认的 -10%..120%（相对元素盒），
   不能当成"用户单位 0"—— 第一版直接落成 0.1 用户单位，遮罩几乎把元素裁没了。

### 验收

- `test_css` 新增 16.26（16 条断言）：条目存在、白 → alpha 255、0xB4 → alpha 180、
  几何是设备空间的圆外接盒、默认区域 = bbox 外扩 10%、点表闭合、OBB 区域 = 元素盒、
  引用不存在的 id 要告警且无条目、内容全黑要告警且无条目、没写 mask 无条目。
- 新用例 `anim/svg-mask`（白圆 / 灰阶三档 / 阶梯衰减六段 / 遮罩 + 图片 / mask+clip-path
  嵌套 / objectBoundingBox）：`pixels-vk` / `pixels-d3d11` **0 不同像素（最大通道差 0）**。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题、
  `vk-window` / `vk-stress` 通过。

### 已知边界

掩码内容不支持渐变（按中间色；要渐变请拆成多条不同亮度的形状）；内容里的 `<g>` 不展开；
**重叠形状会被乘两次**（重叠区更暗，补集趟的 even-odd 也会把重叠区当成"外"）；
`mask-type="alpha"` 未做（一律按亮度）；GDI 遗留后端不参与（照常画，不遮罩）。
到这里 P1-10（SVG 的 `mask` / `clipPath` / `pattern` / `marker`）四条全部落地。

## [0.16.25] - 2026-11-15

SVG `<pattern>`（P1-10 第三条）：图案填充。之前 `fill="url(#p)"` 命中 pattern 时只会告警
"不支持"，现在条纹、棋盘、点阵这类图案能填进任意形状，而且**格式不变、三个后端零改动**。

### 做 — 编译期平铺展开 + 复用 v27 裁剪

pattern 的内容是**静态几何**：铺几块、每块落在哪，编译期就能算死。所以整件事在编译期做完：

1. 元素照常编成路径节点，但**自己不再填充**（填充交给 tile）；`<rect>`/`<circle>` 因为
   `fill="url(#…)"` 自动升级成路径（轻量映射只能表达一个颜色）。
2. 按**用户空间**扫网格：`width/height` 是 tile 尺寸（`patternUnits="objectBoundingBox"`
   时按元素盒比例放大），整块落在元素包围盒外的 tile 跳过。
3. 每块内容过 `M = svgX · T(tile 原点) · [viewBox meet 居中] · [OBB 缩放]` 落到设备空间。
4. **同色同笔的 tile 合成一个路径节点** —— 一块一个节点的话，100 块就是 100 次
   "进图层 → 擦除 → 出图层"，而图层池只有 4 层。每个展开节点再挂一条"元素自身轮廓"的
   裁剪记录（复用 v0.16.24 的补集擦除），所以图案**不会溢出**到形状外（星形、曲线都能填）。

支持：`width/height/x/y`、`patternUnits`、`patternContentUnits`、`viewBox`（等比 meet 居中）、
`patternTransform`（平移 + 缩放）；内容支持 `path`/`rect`/`circle`/`ellipse`/`polygon`/
`polyline`/`line` 各自的 `fill`/`stroke`/`stroke-width`/`stroke-linecap`/`stroke-linejoin`。

### 踩到的坑

1. **`<pattern>` 的属性全被当成"未实现"告警**：`patternunits` / `patterncontentunits` /
   `patterntransform` 没进 `UsedHtmlAttrs()` 白名单。
2. **补集擦除只认 even-odd 区域**：五角星那种 10 点**自交**写法，重叠区会被当成"外"，
   图案中间空一块。试过把擦除改成 `dst *= srcA`（不受填充规则影响），但那个混合只作用在
   "画了东西的像素"上，路径外根本不会被清零（整层都留下来了）—— 结论是**补集几何是必需的**，
   于是把边界写进文档：轮廓按 even-odd 解释，星形请写不交叉的 10 点。
3. **裁剪层里的奇数宽度描边**：`anim/svg-pattern` 第一版 GL vs VK 差 496 像素（0.16%），
   定位到"`stroke-width=3` 的线条，VK 每段末端多 1px"（GL 与 D3D11 逐像素相同，偶数宽度
   零差异）。图案线条改用偶数宽度后 **0 不同像素**；根因未定位，按已知差异记录在
   `docs/future.md`。

### 验收

- `test_css` 新增 16.25（15 条断言）：元素自身不填、同色 tile 合成一个节点、tile 节点是纯填充、
  100 块都铺上、每个展开节点挂了裁剪记录且裁剪盒 = 元素盒、`objectBoundingBox` 的 tile 放大到
  元素盒尺度、引用了不存在的 id 要告警且退回纯色、`width=0` 要告警且不填。
- 新用例 `anim/svg-pattern`（竖条 / 棋盘 / 星形 + viewBox / objectBoundingBox /
  `patternTransform` / 开放曲线）：`pixels-vk` / `pixels-d3d11` **0 不同像素（最大通道差 0）**。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题。
- 格式仍是 **v27**（pattern 只复用裁剪段，没有新段）。

### 已知边界

pattern 内容**不裁到 tile 框**（tile 里的形状别越界，否则会盖到隔壁 tile）；`<g>` 不展开；
内容渐变按中间色；平铺块数 > 400 或点数超预算时退回"首色实填"并告警；
`mask` 仍未做（要片元级采样）。

## [0.16.24] - 2026-11-15 【格式 v27】

SVG `clipPath` / `clip-path`（P1-10 第二条）：任意形状裁剪。之前只有矩形 scissor，圆角、
圆形、多边形裁剪都做不到；现在能裁了，而且三端逐像素相同。

### 做 — 离屏图层 + even-odd 补集擦除

裁剪的本质是"保留路径内的像素"。三端一致的实现只有一条：把节点先画进**离屏图层**
（组透明度那套现成的），再用"图层矩形 ∪ 裁剪路径"的 **even-odd 补集**在**擦除混合**
（`dst *= 1-srcA`）下画一遍 —— 路径外被清零、边缘按覆盖率淡出。于是：

- 混合状态是唯一的新能力：GL 换 `glBlendFunc`、D3D11 多建一个 blend state、VK **建两条
  管线变体**（混合是管线状态，SPIR-V 一行没动）。
- 几何在**编译期**折算：`clipPath` 的子形状过 `svgX`（或 `objectBoundingBox` 缩放）后
  写进新的**裁剪路径段**（v27，`BinUIClipEntry` + 归一化点），运行期不需要 SVG 变换链。
- 图层里画的内容包括底色/边框/文字/图片/SVG 路径/阴影 —— 它们都是四边形式条带，
  擦除一并覆盖（所以文字裁剪、图片裁剪、双层色块裁剪都对）。

### 踩到的三个坑

1. **`pushLayer` 原来拒绝 `alpha = 1`**（组透明度的早退优化："不需要变淡就不建层"）。
   裁剪用的层是纯容器，必须放开 → 三个后端改成只在 `alpha <= 0` 时早退。
2. **补集要按图层矩形重新归一化**，第一次我把绝对坐标当成了相对坐标，整层被擦光
   （实拍是一张全空白的页面）。
3. **`objectBoundingBox` 的容差**：0..1 的空间里用 0.1 的细分容差，圆被切成十边形。
   容差按 units 缩放（0.002）后才是圆。

### 验收

- `test_css` 新增 16.24（9 条断言）：条目存在、包围盒是设备空间、objectBoundingBox 放大到
  元素盒、没写/引用不存在时无条目。
- 新用例 `anim/svg-clip`（圆形裁图片 / 圆角矩形裁色块 / 多边形裁双层色块 / 裁文字 /
  objectBoundingBox）：`pixels-vk` / `pixels-d3d11` **0 不同像素**（D3D11 最大通道差 0）。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题、
  `vk-stress` 通过。

### 已知边界

图层池 4 层（与组透明度共用），所以"裁剪 + 半透明组"嵌套超过 4 层时退回不裁（不会画坏）；
GDI 遗留后端不参与；`mask` / `pattern` 仍未做。

## [0.16.23] - 2026-11-15


SVG `<marker>`（P1-10 的第一条）：`marker-start` / `marker-mid` / `marker-end` 从"告警忽略"
变成真支持。箭头、圆点、刻度这类"路径上的装饰"终于能画了。

### 做 — 编译期展开，运行期零改动

marker 是**静态几何**：每个实例只取决于顶点位置与切线方向。所以整件事在编译期做完 ——
按 `M = T(顶点) · R(切角) · S(markerUnits) · T(-refPoint) · V(viewBox)` 把 marker 内容
（path/polygon/polyline/line/rect/circle/ellipse）换算成设备空间，**追加成引用路径的子节点**
（纯填充的普通 `BINUI_PATH`）。三个后端与格式段一行没改，`anim/svg-marker` 三端 0 差异。

支持：`viewBox` + `refX`/`refY` 对齐、`markerWidth`/`markerHeight`、`markerUnits`
（`strokeWidth` 默认 / `userSpaceOnUse`）、`orient`（`auto` / `auto-start-reverse` / 固定角度）、
CSS 简写 `marker`。带 marker 的 `<line>` 自动升级成路径（`BINUI_LINE` 只有两个端点）。

### 踩到并修掉的两个坑

1. **不能并进引用路径自己的节点**：第一版把 marker 几何追加进 `svgPathShape` 并强行打开
   `PATH_FILL`，于是 `fill:none` 的折线被**整条填成实心块**（实拍图里那支"箭头线"变成了
   一大片蓝色三角形）。一个节点只有一个填充色 —— marker 必须单独成节点，颜色取引用路径的
   描边色（SVG 的 `context-stroke` 语义）。
2. **`<line>` 陷阱**：`<line>` 平时编成 `BINUI_LINE`（两个端点、省节点），marker 展开只
   发生在路径分支里 —— 所以"线上加箭头"最初什么都不出现。现在带 marker 的 `<line>` 和虚线/
   渐变描边一样自动升级成路径。

### 验收

- `test_css` 新增 16.23（9 条断言）：marker 展开成额外节点、挂在引用路径下、引用路径仍是
  `fill:none`、marker 节点是纯填充且几何闭合、带 marker 的 `<line>` 升级成路径、没写 marker
  的路径不生成子节点。
- 新用例 `anim/svg-marker`（5 组：折线箭头 / 两端 auto-start-reverse / 顶点圆点 / viewBox +
  orient=90 / markerUnits 随线宽缩放）：`pixels-vk` / `pixels-d3d11` **0 不同像素**。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题、
  `vk-window` / `vk-stress` 通过。

## [0.16.22] - 2026-11-15 【格式 v26】


`filter` 补齐（P1-8 的最后一条）：`grayscale` / `sepia` / `saturate` / `hue-rotate` /
`invert` / `contrast` 六个函数从"告警忽略"变成真支持，三端逐像素相同。

### 加 — 颜色矩阵：编译期合成，解码时逐像素应用

这六个函数本质都是"RGB 上的一次仿射变换"（3×3 矩阵 + 偏移），顶点色乘不出来；而改片元
着色器这条路对 VK 不通（SPIR-V 是仓库里预编译好的常量数组，没有构建期 glslang）。所以走
**CPU 变体纹理**：

- 编译期按**书写顺序**把函数合成成 3×3 + 偏移（数学在 `include/binui_filter.h`，与 CSS
  Filter Effects L1 同一口径：sRGB 空间、非预乘、逐函数左乘），写进新的**图片滤镜段**
  （`BinUIImageFilter`，52 字节/节点，`uint32 bytes + uint32 count + entries`）。
- 后端解码图片后、建 mip 链之前跑一遍矩阵，纹理按 `(imageIndex, 矩阵哈希)` 缓存变体 ——
  同一张图配两种滤镜是两份纹理，**之后每帧零开销**；没有滤镜时键退化为 imageIndex，
  老路径的指针/行为一位不变。
- `brightness()` 不动：它等价于"乘一个灰度顶点色"，0.16.15 起就走 `paint.reserve[2]`，
  GDI 也能吃。矩阵里刻意不含它，避免双重压暗。
- 视频帧也吃（每帧跑一遍是 CSS 语义要求的真开销，值为先）。

### 改 — 三端图片缓存键：imageIndex → (imageIndex, 滤镜哈希)

GL / VK / D3D11 的 `images` / `imageGen` / `imageDims` / `rawMips` 四张表统一换成
`uint64_t` 键（`index | 哈希 << 16`）。VK 原来那张是 `unordered_map<int, Tex>`，
**必须**一起换成 64 位，否则哈希被截断，两种滤镜可能撞同一份纹理。

### 验收

- `test_css` 新增 16.22（28+ 条断言）：grayscale/sepia/saturate/invert/contrast 的矩阵值、
  hue-rotate 的 360° 恒等与 120° 换色、合成顺序非交换（sepia→invert ≠ invert→sepia）、
  逐像素应用（alpha 不动 / 全透明跳过 / 越界钳制）、缓存键（nodeId 不进键、矩阵变则键变）、
  编译器落点（六个函数进段、`brightness` 不进段、`blur()` 不进段）。
- 新用例 `anim/img-filter`（12 组：单函数、圆角、3 函数合成、同图两份纹理）：
  `pixels-vk` / `pixels-d3d11` **0 不同像素**（最大通道差 1）。
- `make pixels` 16/16 零差异、`make test` 全绿、`check_examples` 16 示例 0 问题、
  `vk-window` / `vk-stress` 通过。

## [0.16.21] - 2026-11-15

仿射**级联**：`transform` 终于作用于**整棵子树**。0.16.18 只让"节点自己画的东西"过矩阵，
子元素还是正的 —— master 一眼就看出来了："浏览器是整个元素一起转的"。

### 改 — 一个节点一个矩阵，沿祖先链复合

浏览器的 `transform` 是在元素边界处**换一套坐标系**：后代照旧在局部坐标里布局，整棵子树由
同一个矩阵落到屏幕。所以转一张卡片，卡片里的文字、图标、子元素、子元素的旋转全都跟着转。

- 绘制前按节点下标顺序（父必先于子）预计算 `M[i]`：节点局部坐标 → 屏幕：
  `M[i] = M[父] ∘ 平移(子原点在父局部) ∘ 自身旋转/缩放（绕自身中心，含动画平移）`。
  根节点缺省 `M[父] = I`，平移量 = 盒子左上角（各减自己的滚动偏移）。
- 有了 `M[i]` 就**不用改后端**：`drawNodes` 把节点的几何整体挪到局部 `(0,0,lw,lh)`，
  一次 `setAffine(M)` 同时管住底色/边框/文字/图片/阴影和**所有后代**（包括嵌套 `transform`
  —— 两个矩阵相乘，子元素不是各自绕自己的屏幕中心转）。
- `<line>`（分隔线）也吃级联：局部端点是 `(0,0)` → `(w-x, h-y)`，退出时清状态。
- 快速开关 `renderer.anyTransformAnim()`：静态页面两张动画表都是空的，一次哈希探测都不花，
  复合整段直接跳过（像素与性能都不受影响）。
- 矩阵数学抽进 `include/binui_paint.h`（`AffineMul` / `AffineRotScaleAbout`）并单测 ——
  复合顺序、别名安全、绕中心不变点这些都是"少乘一项就整页挤到左上角"的雷（本轮第一版就漏了
  根节点的平移项，实拍立刻塌成一团，所以补了 8 条断言）。

### 验收

- `affine-text` 用例扩到嵌套变换（父 -8° / 子 +12°）+ 非等比缩放的整棵子树拉伸。
- `make pixels` 16/16 零差异（老页面逐位不变）、`pixels-vk` **全 0 不同像素**、
  `pixels-d3d11` 仅 6 个像素超阈值（0.002%，都是旋转字形边缘的 MSAA 采样，容差内）。
- `make test` 全绿（新增 `test_css` 16.21：8 条矩阵断言）、`check_examples` 16 示例 0 问题、
  `vk-window` / `vk-stress` 通过。

## [0.16.20] - 2026-11-15

程序化纹理 / 视频帧的 mip（P1-8 的最后一条）。以前 `prepareImageRaw`（`<video>` 帧、
`updateImageRaw` 推上来的棋盘/噪声纹理）**故意不做 mip** —— 视频帧每帧换内容，每帧
`GenerateMipmap` 太贵。但"程序化纹理"是静态的，缩小时同样满屏摩尔纹。

### 加 — "第二次见到同一个 generation" 才补 mip 链

判据只有一句：`prepareImageRaw(index, …, generation)` 被**同一个 generation** 调第二次，
就说明这份像素不再逐帧变了 —— 视频帧的 generation 每次都变，永远走不到那条分支；程序化
纹理从第二帧起命中，补一次 mip 链后一直复用。三端各自落地：

- **GL**：命中时 `glGenerateMipmap` + `MIN_FILTER = LINEAR_MIPMAP_LINEAR`（`rawMips` 标记只做一次）。
- **D3D11**：mip 链不能在创建后追加，所以命中时**重建**成 `MipLevels=0 + GENERATE_MIPS +
  RENDER_TARGET` 的纹理 → `UpdateSubresource` 第 0 级 → `GenerateMips`。
- **Vulkan**：链在 CPU 上做（后端没有"每帧重传"的余量，也不能 blit），命中时用传入的像素
  建整条链、逐级上传。
- 视频帧路径**完全不变**（不生成、不重传），零额外开销。

### 抽 — `include/binui_mip.h`：mip 链的 CPU 盒式滤波只有一份

三端"同一张图缩小时必须看着一样"，而硬件 `GenerateMipmap` 改不了，所以 CPU 这一侧的公式
写死并单测：第 l 级 = 上一级 **2×2 四舍五入平均**（`(a+b+c+d+2)/4`），奇数尺寸边缘夹取。
`BuildMipChainBGRA` / `MipLevels` 同时被 VK 的两条路径（图片 + raw 纹理）复用 —— 以前这两段
各写了一遍同样的循环，正是"两份实现慢慢分叉"的温床。

### 验收

- `test_css` 新增 8 条断言：层数（1×1 / 2×2 / 256×256=9 / 1920×1080=11）、4×4 出 3 级、
  第 0 级原样、同级色块保留、1×1 = 128（四舍五入平均）、3×3 → 1×1 = 70（边缘夹取）、
  退化输入返回空。
- `make pixels` 16/16 零差异、`pixels-vk` / `pixels-d3d11` 全 0（VK 的链改走共享实现后与
  GL 的硬件生成仍逐像素一致）、`make test` 全绿、`check_examples` 16 示例 0 问题。
- `texturedemo`（30Hz `updateImageRaw`）跑 3 秒无崩溃 —— 视频帧路径确认没被这条改动影响。

## [Unreleased]

### 文档 — 补一份「欠缺清单」：13 条，带实测数字、优先级和"谁来填"

master："把你说的欠缺的功能列出来，感觉都得填上"。先能测的都测了（本机单线程 `-O2`，
直接把 `include/binui_paint.h` 的条带生成拉出来跑）：

| 形状 | 条带数 | 生成耗时 |
|---|---|---|
| 纯色圆角卡 320×180 r16 | 1 | ~0 |
| 渐变圆角卡 320×180 r16 | 37 | 0.52 µs |
| 半透明渐变卡 320×180 r16 | 240 | 4.03 µs |
| 边框 borderW=2 r16 | 62 | 0.91 µs |
| 路径圆 r60（AA，120×120） | 473 | 64.1 µs |
| 路径五角星（AA） | 510 | 25.0 µs |
| 路径大圆 r150（AA，300×300） | **1173** | **199.0 µs** |

跑出来两个比"慢"更值得写进文档的发现：

- **路径条带缓存的 1024 条硬上限是真悬崖**：`kMaxCachedBands` 超了就不缓存，300×300 图标
  （1173 条 / 199 µs）几何不变也**每帧重算**；240×240（935 条）刚好还在缓存里。
- **顶点预算 16,666 个四边形，打满后静默丢几何**：三端都是 `kMaxVerts = 100000`（≈2 MB），
  `vertRoom()` 返回 nullptr 后 `emitQuad` 直接 return，不报错。按实测 473 四边形/图标算，
  **34 个图标就把整帧打满**，后面的内容不画。这条是"画不出来"级，优先级高于所有性能项。

清单分三档：P0 五条（层缓存/增量上传、路径缓存上限、路径单线程、每帧全量上传、顶点预算静默
丢弃）、P1 七条（`box-shadow`、**对角渐变是按垂直近似的**、**组透明度其实是错的**、图像无 mip/
圆角/tint、文字图片不参与仿射、SVG mask/clipPath/pattern/marker、字形无 ¼ 像素变体）、
P2 四条明确不填（GPU 路径光栅、文字次像素、完整 SVG 滤镜栈、通用图形 API 化）——理由都写了。

也给了建议顺序：离屏合成（同时结掉组透明度和层缓存）→ 顶点预算别静默丢 → 去 1024 上限 →
路径多线程 → 其余按示例需要。不主张"13 条一起填"：其中 3 条是同一套设施，4 条已决定不做。

### 文档 — 记下三个**未公开、未实现**的未来计划：bin2d / bin3d / binGPU

- 新增 `docs/future.md`：**bin2d**（把 `include/binui_path.h` / `include/binui_paint.h` 那层
  光栅数学抽成独立 2D 库 —— 场景图 / 图层 / 离屏合成 / 滤镜 / 导出，binUI 转为它的消费者）、
  **bin3d**（界面里嵌 3D 视口，先 CPU 软光栅 + `BINUI_3DVIEW` 节点、不动后端；撑不住再给
  `IBinUIGfx` 加 `drawMesh` + 深度目标）、**binGPU**（三个后端下面那层设备 / 交换链 / 管线 /
  呈现的抽象：先只做薄的固定管线，VK 作第一实现、D3D12 作第二实现，GL 明确**不迁**留作兜底）。
- **语言分工（master 定）**：bin3d = C++、bin2d = C++、**binGPU = C**。bin2d/bin3d 跟 binUI 同为
  C++17；只有最底层 binGPU 是 C11、导出 C ABI（定长 POD + `uint64_t` 句柄 + 扁平函数，无回调 /
  无异常 / 不依赖 libstdc++）。理由记在文档里：它下面那三套 API 自己就是 C（`vulkan.h` / COM /
  GL），它又是最可能被 Python/Go/Rust 直接绑的一层，形状上恰好只传数字和句柄。
- 也记了 C 的代价：`binui_vk.cpp` 的 2805 行 C++ 搬过去是"按 C 重写"而不是移文件（容器、fence
  RAII、错误串都得自己来），销毁顺序由 C++ 侧的 RAII 壳收在一处，device lost 用显式轮询不搞回调。
- 顺手实测了 master 问的"纯 C 会不会更快"（`docs/future.md`「语言分工 → 附」）：
  - 同一份 alpha 混合内核交给 `gcc -O2` 与 `g++ -O2`，生成的 `.text` **逐行相同**（只差符号名）——
    语言本身不带来速度；
  - 照条带发射写对照：窄条带下"取样器经函数指针"比纯铺 +11.0%，"取样器作模板参数（可内联）"
    只有 +1.5%；而 `std::function` 每行一次类型擦除是 **+138.4%**，渐变取样数学是 **+177.8%**。
    宽行（1920×1080）时全部落在 ±3% 噪音里（那会儿瓶颈是写内存带宽）。
  - 结论写进文档：语言按**表达力 / ABI** 选，不按速度选；想给别人直接 `#include` 的那一层才导出
    C ABI。于是 binGPU = C，bin2d/bin3d = C++17 写、需要时再挂 `extern "C"` 的 C 入口。
- **故意不写进 [`docs/README.md`](docs/README.md) 索引、也不在 README 提**：三条都还没有一行代码，
  属于方向性记录，不是承诺、没有日期。文档里同时写了"现在为什么不做的原因"和"什么条件才开工"，
  免得下次又从零讨论一遍。
- 顺带记下了几个后面一定会踩的坑：3D 的像素基准不能沿用"0 diff"（浮点光栅化顺序不同，z 缓冲
  边缘必然差 1px，容差规则要改写成覆盖率/轮廓容差）；bin2d 的复杂度成本主要在场景图的缓存
  生命周期，而 binUI 现在**故意**没有这份状态；binGPU 的收益是"少写第四份实现"、代价是所有
  像素 bug 多一层要排查 —— 所以在没有第四份后端之前是纯亏。
- binGPU 的现状数字也一起记了（同一件事写三遍的证据）：GL 1327 / D3D11 1340 / VK 2805 行；
  `wglCreateContext` 9 处、`vkCreateSwapchainKHR` 5 处、`CreateSwapChainForHwnd` 2 处；
  `glCreateProgram` 4 / `vkCreateGraphicsPipelines` 4 / `D3DCompile` 3；着色器源码三套
  （GLSL 内联 / `binui_vk_shaders.h` 的 SPIR-V 常量 / HLSL 内联）。

### 文档 — 图层设施设计稿（P0-1 / P1-7 的收口）

0.16.8 的组透明度是"逐元素乘 alpha"的近似，重叠区会叠深；层缓存（内容不变不重画）也还没做。
两件事都要同一套离屏图层，所以在 `docs/future.md` 里把接口（`pushLayer`/`popLayer`，默认返回
false = 后端退回逐元素模式）、渲染器侧的分组规则、三端实现要点（GL/D3D11 直接做，VK 要改主
render pass 的 loadOp 所以排最后）、层缓存的失效条件与验收线先写清楚，动手时照这个来。

## [0.16.16] - 2026-11-15 【格式 v24】

文字能力补齐（P1-11）：**¼ 像素字形相位** + **`text-shadow`**。前者治"动画时文字发虚"，
后者是卡片/标题一直缺的那层层次。**格式升到 v24**（新增文字阴影段，20 字节/条）。

### 加 — ¼ 像素字形相位：动画文字不再靠 LINEAR 采样"糊一层"

0.16.4 为了不让文字在动画里每 2 帧跳 1px，让有动画的节点保留小数位置、图集切 LINEAR 采样。
代价是**整个字形被过滤糊掉**（不只是位移那 ¼ 像素）。现在换成业界做法：

- x 的小数部分量化成 **¼ 像素相位**（0.37 → 相位 1；0.9 → 进位到下一像素、相位 0；
  最坏位置误差 0.125px），光栅化时用 `FT_Set_Transform` 的 delta 把轮廓先右移 `phase/4`
  像素 —— **位图自己带上那 ¼ 像素**，于是字形照样画在整数坐标上，1:1 采样、不再被过滤。
- 图集格子按 `(码点, 相位)` 缓存、**懒加载**：静态页面永远只有相位 0（与旧版逐像素相同），
  动画里用到哪个相位才造哪个。整形缓存 key 也含相位。
- `advance` 保持整数 → `textWidth` / 换行位置 / 布局一字不变（这条很重要：改小数 advance
  会让所有老页面的文字整体移位）。
- **只做横向**：纵向亚像素要 4×4=16 份变体（图集体积×4）且与纵向 hinting 冲突，
  DirectWrite / Chrome 同样只做横向、纵向按整像素吸附。
- A/B 开关 `BINUI_TEXT_PHASE_OFF=1` 退回 0.16.15 的 LINEAR 行为。

实测（`docs/issues/assets/text-subpixel.html`，同一页同一帧，只切开关）：
墨量总和 974168 vs 974387（差 0.02%，**粗细不变**）、**单位墨量的边缘梯度能量 1.249 vs 1.161
（+7.6%，更锐利）**、中间调像素 6835 vs 7743（**−12%，发虚的过渡像素更少**）。

### 加 — `text-shadow`（v24 文字阴影段）

`text-shadow: <offset-x> <offset-y> [blur] [color]`，**可继承**、`currentColor` 缺省、
`blur < 0.5px` = 硬阴影、**只支持一层**（第二层起明确告警）、没有 `spread`/`inset`。

- 为什么再开一段而不是复用 v23 阴影段：一条 `box-shadow` 有 5 个几何量（多 `spread`）、
  一条 `text-shadow` 只有 3 个，而且绘制位置完全不同（盒子轮廓的环 vs 字形位图的模糊）。
  段序：… / 扩展绘制 / 路径 / 阴影 / **文字阴影** / 名称。
- 继承走编译器的 `Ctx`（和 `color`/`fill` 一样沿树传），所以**卡片上写一次、卡片里所有
  文字都带**，`none` 从那一层开始清掉；只有**真会画字**的节点才写条目（容器/空节点不写，
  段保持稀疏）。
- 实现：字形覆盖率 → 可分离高斯（σ = blur/2、截断 3σ、核按权重和归一化）→ 按
  `(字号, 字重, 斜体, 字体面, 模糊半径)` 存进**独立的模糊图集**（格子四边按 `3σ+1` 外扩，
  边长封顶 192px）。画的时候**先整条阴影、再整条字形**（逐字交错会让后一个字的阴影
  压到前一个字上），阴影四边形按 `sPad` 外扩、颜色乘顶点色。`blur = 0` 直接复用字形格子
  —— "阴影 = 偏移一份字形"，零额外纹理、逐像素最精确。
- 后端契约**没有新增原语**：只加了 `IBinUIGfx::setTextShadow(dx,dy,blur,color)` 这个状态
  （`drawText` 不变），所以三个 GPU 后端各一行、像素逐位相同。
- 文字阴影会溢出节点盒（CSS 也不裁），所以**节点自身裁剪矩形按外扩量放大**
  （`|offset| + 3σ + 1`），否则贴边文字的阴影会被切出一道直边。
- **emoji 不带阴影**：彩色格子里每像素自带颜色，乘阴影色会糊成彩色噪团而不是剪影 ——
  与其画错，不如明确不画（`docs/future.md` 记着）。

### GDI — 覆盖率烘位图 + `AlphaBlend`（与 GPU 侧同一套数学）

GDI 没有字形图集（系统 `DrawTextW` 直接光栅化），所以另走一条等价路：用**同一字体、
同一矩形、同一套 `DrawTextW` 参数**把文字画进 32bpp DIB（覆盖率藏在 RGB 里，ClearType 的
彩色边取三通道平均）→ 同一套高斯（σ = blur/2）→ 按阴影色预乘 → `AlphaBlend` 贴到
`(矩形左上 − pad + offset)`。不用 GDI+ `DrawString`：阴影必须与正文**逐像素同源**，
两套排版器会差出半个字宽。位图按"文字 + 字体 + 矩形尺寸 + 排版标志 + 阴影参数"缓存
（与屏幕位置无关，滚动/动画时换位置仍命中）。`BINUI_TEXTSHADOW_OFF=1` 可关掉做 A/B。

### 验收

- `make pixels` **16/16 零差异**（15 条老基准逐位不变 —— 静态文字相位恒为 0，这就是
  "不碰老页面"的证明；新基准 `text-shadow` 由 GDI 生成）。
- `pixels-vk` / `pixels-d3d11` **全部 0 不同像素**，含新增的 `text-shadow`（640×400）与
  跨后端比对里唯一覆盖非 0 相位的 `anim/text-subpixel`（640×280，容差 0%）。
- `vk-window` / `vk-stress` 通过（显存记账归零）、`make test` 全绿、`check_examples` 16 示例 0 问题。
- 新增断言：`test_css` **15 条**（段解析 / currentColor 两种写法 / 继承 / none 清除 /
  空节点不写 / 第三、四个长度 / 负 blur / 多层只取第一层）、`test_font` **6 条**
  （四相位可光栅化 / advance 与相位无关 / 重心单调右移 / 每步 ≈0.25px：实测
  0.189·0.377·0.199 / 墨量不变 <5% / 相位 4 回绕到 0）。
- 新像素用例 `docs/issues/assets/text-shadow.html`（偏移/模糊/硬影/大偏移/负偏移/继承/
  none/currentColor/emoji/贴边）与 `text-subpixel.html`（相位 1/2/3 + 纵向 + 阴影组合）。


## [0.16.15] - 2026-11-15

图片能力补齐（P1-8）：**圆角裁剪 / mip / 染色**。以前 `<img>` 只有"一张方图双线性拉进盒子"，
圆角头像会露出方块、大图缩小满屏摩尔纹、也没有任何染色的手段。**格式不变**（`BINUI_VERSION`
仍是 23：亮度复用了扩展绘制段一直为 0 的 `reserve[2]`）。

### 加 — 圆角图片：横条 + UV 子矩形

- 后端没有"圆角裁剪"原语，所以把图片按行切成横条：上下两个圆角区各 1px 一条、中间直边区
  一条整宽，每条只画该行的 `[内缩, w-内缩]`。形状就是解析式圆弧（`PaintRoundInset`），
  边缘交给后端的 MSAA。条带数 = 2r+1，与图片高度无关（96px 高的圆角图 49 条、正圆头像也是）；
  半径大到装不下时放大行高，保证画完而不是画一半（`BuildImageBands`，`binui_paint.h`）。
- **UV 跟着内缩走**：圆角区取源图 `[in/w, 1-in/w]` 那一段。图片仍然"按整个盒子缩放"，
  被圆角裁掉的是两侧 —— 与 CSS 的 `border-radius` 语义一致；若 UV 恒取 `0..1`，圆角区的
  图片会被横向拉伸（鱼眼），实测一眼能看出来。
- 直角时仍然一条整图，**像素与 0.16.14 完全相同**。
- GDI 用 GDI+ 的圆角路径裁剪（自带解析 AA）。

### 加 — 染色：`filter: brightness(0..1)`

- 三个 GPU 后端画图片本来就是"纹理 × 顶点色"，所以"乘暗"是免费的：系数写进扩展绘制段的
  `reserve[2]`（1..255，0 = 没声明）。GDI 用 GDI+ 的 color matrix（**注意对角线第四维
  `m[3][3]` 必须置 1**，否则 alpha 归零、整张图消失 —— 实测踩到）。
- `brightness(>1)` 不提亮（顶点色只能乘），`blur`/`grayscale`/`saturate` 仍不支持并告警。
- 只写了 `filter` 的 `<img>` 也必须进扩展绘制段 —— 编译器的"要不要出 paint 条目"条件补上
  `reserve[2]`（`PAINT_*` 的 16 位早就用满了，没有空位可用）。

### 加 — mip（大图缩小的质量）

- GL：`glGenerateMipmap` + `GL_LINEAR_MIPMAP_LINEAR`（新增 `glGenerateMipmap` 装载）。
- D3D11：`MipLevels=0` + `RENDER_TARGET` + `GENERATE_MIPS`，`UpdateSubresource` 写第 0 级再
  `GenerateMips`；采样器从 `MIN_MAG_LINEAR_MIP_POINT` 换成 `MIN_MAG_MIP_LINEAR`。
  **注意**：带 mip 的纹理不能在 `CreateTexture2D` 里只喂第 0 级（D3D11 要求
  `pInitialData` 覆盖整条链，否则创建失败、图片整块消失 —— 实测踩到，先建空纹理再更新）。
- VK：mip 链在 **CPU** 上按 2×2 平均生成再逐级上传（`vkCmdCopyBufferToImage` 带
  `mipLevel`），采样器线性档改 `MIPMAP_MODE_LINEAR` + `maxLod = VK_LOD_CLAMP_NONE`。
  不走 `vkCmdBlitImage` 的理由写在后端注释里：VK 这边是"一张图一个布局状态"的记账模型，
  blit 链要 `TRANSFER_SRC` + 逐级布局切换，比"多传几张小图"更容易出错；而链只在
  换 generation 时生成一次。
- `prepareImageRaw`（视频帧 / 程序化纹理）**不做** mip：它们每帧换内容，生成 mip 不划算，
  通常也是 1:1 或放大显示。

### 修 — 三个测试工具一直没在真正渲染图片

`gl_shot` / `vk_shot` / `d3d11_shot` / `vk_window` / `vk_stress` 加载了图片字节却**没调
`setImageOrder`** —— 渲染器的 `getImage()` 走的是 `imageOrder_`，于是这些工具画出来的
`<img>` 全是空白（`pixels-vk` 里的 `texturedemo` 用例因此一直是"没有图片的在跑"）。
现在加载资源前先 `setImageOrder(imagePaths())`。

### 测试 / 文档

- 新像素用例 `image-round`（直角/16/圆形/胶囊/竖长条/带边框 + 3 档 brightness）与
  `image-mip`（512×512 的 2px 棋盘按 4 种比例缩小，另加圆形与圆角），基准由 GDI 生成：
  `make pixels` 15/15 零差异、`pixels-vk` / `pixels-d3d11` 全 0 差异（VK 与 GL 的 mip 结果
  逐像素相同）。
- 新的接口 `drawImageEx(index, u0,v0,u1,v1, x,y,w,h, tint)`（`IBinUIGfx`），`drawImage` 变成
  它的便捷包装 —— 三个 GPU 后端都只需几行改动（顶点色/UV 本来就有）。
- `tests/test_css.cpp` 新增 0.16.15 一组 14 条断言：`brightness(0.5)` → `reserve[2] == 128`、
  `brightness(180%)` 不提亮、`grayscale(1) brightness(0.25)` 只吃到 0.25、只有 filter 也出
  paint 条目、直角 = 1 条整图、圆角 = 2r+1 条、圆角区 UV 取内侧、直边区一条整宽、
  纵向首尾相接且铺满整图高、超大半径仍铺满。
- README 的图片一节与"不生效属性"清单同步；`docs/future.md` P1-8 标记完成（剩下的：
  九宫格 nine-slice 图片、`filter` 的其它函数、视频帧的 mip）。



## [0.16.14] - 2026-11-15 【格式 v23】

`box-shadow` 能用了（P1-5）。这是"卡片浮起来"缺的那一层：以前卡片只能靠 1px 边框 + 色差
做层次，柔影全是半透明圆角假造。**格式升到 v23**，新增一段阴影数据。

### 加 — 阴影段（v23）与三种画法

- **编译器**：`box-shadow: <offset-x> <offset-y> [blur] [spread] [color] [, …]*`，颜色可以写
  在任意位置、不写就用 `currentColor`；`inset` 明确告警并忽略（后端只有 `fillRect`/`fillRounded`，
  画不出"挖内孔"的几何，画成外阴影比不画更错）；`none` = 清空；百分比偏移告警。
  **多条阴影按"绘制顺序"存**（作者先写的在上面 ⇒ 数组里放在末尾），渲染器顺序画一遍即可。
  数据落新的一段：`uint32 bytes + uint32 count + BinUIShadow[]`（24 字节/条，紧随路径段之后，
  名称段之前，恒定写出）。XML 路径与默认布局同样写空段，保证解析链确定。
- **几何（`include/binui_paint.h` 的 `BuildShadowRings`）**：阴影 = 边框盒按 `spread` 外扩后的
  圆角矩形做高斯模糊（CSS 的 blur-radius = 2σ）。模糊不走 CPU 卷积，而是**解析算覆盖率**：
  盒边外扩 d 处的覆盖率 `0.5·erfc(d/σ√2)`，于是整块阴影是一串同心圆角矩形环（外扩 0/0.5σ/…
  /3σ，按"相邻两环透明度台阶 ≤ 3/255"定环数，最多 32 环）。**由外向内画**，后画的环整块盖住
  先画的 ⇒ 每个像素最后拿到"包住它的最内环"的覆盖率，不需要后端提供环形几何。
- **GPU 三端**（GL / Vulkan / D3D11）：走真 alpha 混合，所以环的 alpha 做了**累计校正**
  `a_i = (T_i−T_{i−1})/(1−T_{i−1})` —— 直接按覆盖率给会让重叠区叠深（0.28 的阴影内部变成 0.38，
  实测抓到的）。校正后逐环源覆盖正好得到目标覆盖率，多条阴影叠加也天然是 `1−(1−a)(1−b)`。
  三端**逐像素相同**（`pixels-vk` / `pixels-d3d11` 全部 0 差异，最大通道差 1）。
- **GDI 后备后端**：没有 alpha 合成，改成把整条阴影**烘成一张预乘 alpha 位图**，`AlphaBlend`
  一次贴上去。位图里"覆盖率"先编码成灰度（GDI 往 32 位 DIB 填色不写 alpha 字节），填完再一趟
  CPU 扫描换成预乘像素。缓存键 = 盒尺寸 + 圆角 + 偏移/模糊/展开 + 颜色（与位置、不透明度无关，
  `SourceConstantAlpha` 承担 op 级联），同款卡片共用一张 ⇒ 每帧只有一次位图贴图。
- **合成位置**：阴影画在元素自身底色之下（CSS 的 background 层语义），并且在**节点自身盒子
  裁剪之外**单独开一段裁剪（祖先的 overflow/滚动视口照旧裁它）；剔除用的外接框也按阴影外扩，
  否则贴着视口边缘的卡片一滚就把阴影削掉。旋转/非等比缩放（仿射）的节点把环画在局部空间里
  再整体过矩阵，阴影跟着一起转。

### 修 — 顺手抓到的三个 GDI 问题

- **`FillPaintBands` 会把"完全透明"的填充画成不透明**：`BlendOver(fg=alpha 0, bg)` 返回的是
  **bg**，于是透明节点等于"把父底色重涂一遍"，压在它下面的兄弟/阴影被擦掉。现在 alpha 为 0
  的条带直接跳过（透明元素 + 阴影的用例一眼就看出来：阴影被自身盖成一片白）。
- **`RoundRect` 的填充范围右/下各收 1px**（实测 `[x0,x1−1)×[y0,y1−1)`，上/左却是含的）：
  阴影位图里每一环都短一行，环与环之间漏出一条没画的缝。补上 +1。
- **`-lmsimg32`**：`AlphaBlend` 不在 `imm32` 里，链接行补上（`GL_LIBS`）。

### 测试 / 文档

- 新像素用例 `docs/issues/assets/shadow.html` + 基准 `docs/pixels/shadow.png`（六种形态：柔影/
  硬影/大展开/直角/多层/负 spread/透明元素/双向偏移）。`make pixels` 13 个用例全 0 差异，
  **其余 12 张基准逐字节未变**（说明旧页面一位像素都没动）。
- `tests/test_css.cpp` 新增 0.16.14 一组 16 条断言：解析（偏移/模糊/展开/颜色/`currentColor`/
  `none`/`inset` 忽略）、多层顺序、环数/单调性/中心覆盖率 = 1、校正后累计 alpha 收敛到 A、0 模糊 = 一环。
- 示例：`appshow`（面板/弹窗提示条/按钮）、`animshow`（卡片）、`features`（卡片）、
  `buttonshow`（卡片 + 全部按钮）都加上了柔影 —— 这几处以前都写着"没有 box-shadow（引擎不支持）"。
- 文档：README 的样式一节加了"阴影段"条目，并从"不生效属性"清单里划掉 `box-shadow`；
  `docs/future.md` 的 P1-5 标为已做（`text-shadow` 仍留）。


`linear-gradient` 的角度终于**按角度**画了（P1-6）。以前 `135deg` / `90deg` / `0.5turn`
一律被当成默认方向，所有斜向、横向渐变都画成上下渐变 —— 一眼能看出假。

### 改 — 解析角度，并按角度生成条带

- **编译期**：数字角度（`deg` / `turn` / `rad` / `grad`）与 8 个 `to <side|corner>` 关键字都
  按 CSS 规范折成角度，再归一化到 `[0,180)`（θ 与 θ-180 只差"两端颜色互换"，线性渐变上等价）。
  角度写进 `paint.reserve[1] = 角度 + 1`（0 = 没声明）。**不加 flag 位、不升 BINUI_VERSION**：
  `PAINT_*` 的 16 位已经用满，而旧布局这一位恒为 0、行为与以前完全一致（与 0.16.8 的
  `reserve[0]` = 静态 opacity 同一个先例）。
- **轴对齐的仍是老路径**：`180deg`/`to bottom` 与 `90deg`/`to right`（以及它们的反向：靠两端
  颜色互换表达）继续走原来的逐行/逐列条带 —— 历史页面像素一位都不变，
  `make pixels` 的 11 个基准 0 差异。
- **其余角度**走新的 `BuildFillBandsAngleGrad`（`include/binui_paint.h`）：逐像素行取圆角矩形
  的横跨区间（`PaintRoundInset` 解析式内缩，圆角仍是连续弧），再按"沿 x 走一个色阶需要几个
  像素"把这一行横向切段，段色量化到级别中心后**纵向合并**。这就是 SVG 路径渐变
  （`BuildPathFillBands`）的同一套算法，只是形状用解析式圆角矩形。
  条带仍是轴对齐矩形，所以**后端契约不变**（各端只画 `fillRect`），GL / Vulkan / D3D11 逐像素相同。
- 不做误差扩散抖动：抖动会让相邻行颜色永不相等，纵向合并直接失效 —— 实测 180×140 的 135°
  卡片从 ~260 条涨到 7700+ 条、撞上限退回"垂直近似"（等于没修）。级内恒定的代价是能看出色阶，
  这与 SVG 渐变是同一条底线。

### 加 — 背景条带缓存（斜向渐变的条带是千级）

`binui_draw_nodes.h` 里加了 `BgBandsCache`：键 = 节点 id + 盒(4) + 圆角 + 两端色 + 角度，
任一项变了就重算，所以不需要额外失效入口；上限 2 MB / 64 条，LRU 淘汰。
texturedemo（5 个 135° 占位背景，256²~400×96）实测 **2.95 ms/帧 → 0.26 ms/帧**
（顶点数 48570/帧不变，省的是逐帧重算）。

### 修 — `offscreen_render`（GDI 参考后端）改了共享头不重编

它的 Makefile 规则把编译写进了 exe 的 recipe，**没有头依赖**：改了
`include/binui_paint.h` / `src/backends/binui_draw_nodes.h` 之后跑的还是旧二进制。
这轮差点被它骗过去（改完渐变却在比较旧图，白折腾一轮）—— 与 §7.6.4 记的那次是同一个坑。
现在改成先编 `.o`（带 `-MMD -MP`）再链接，`examples/app_win32.o` 那条规则也补上了 `-MMD`。

### 加 — 像素基准用例 `grad-angle`

`docs/issues/assets/grad-angle.html` + `docs/pixels/grad-angle.png`，进 `PIXEL_CASES`。
六个角度：135° / 90° / 180° / 45° / 225° / 135°+圆角。判据是"颜色沿渐变轴变"，
老实现六块全是上下渐变。

### 验收

- `make pixels`（GDI 基准，11 用例）：11/11 **0 差异**；
- `make pixels-vk` / `pixels-d3d11`（15 用例 + 4 demo + 3 anim）：全部 0 差异；
- `make vk-window` / `vk-stress`：0 差异、显存记账归零；
- `make test` 全绿（新增 11 条角度断言：角度入布局、轴对齐走老路、单位换算、
  `to <corner>` 映射、同一行内颜色随 x 变、左上偏红右下偏蓝）；
- `check_examples` 16 示例 0 问题。

## [0.16.12] - 2026-11-14

Vulkan 的组透明度打通：**整条渲染路径从 render pass 换成 dynamic rendering**，
图层在三个 GPU 后端上终于齐了；顺带抓到一个"一帧多次 flush 时顶点缓冲被覆盖"的真 bug。

### 改 — Vulkan 改用动态渲染（`vkCmdBeginRendering` / `vkCmdEndRendering`）

图层要求"一帧内切渲染目标"，而 render pass 那条路在这台驱动上已经证明走不通（0.16.11 记录：
主 pass 重启后 `LOAD_OP_LOAD` 没保住内容、层里 owner 那一笔不出现）。动态渲染没有
pass/framebuffer 对象，切目标就是两条命令，附件布局全由显式 barrier 记账 —— 两条现象同时消失。

- 设备创建时查询并启用 `VkPhysicalDeviceVulkan13Features::dynamicRendering`；实例版本协商到 1.3，
  老 loader（没有 `vkEnumerateInstanceVersion`）上 `useDynamic=false`，退回原 render pass 路径，
  **像素与原实现逐位相同**，只是没有图层。
- `BINUI_VK_DYNRENDER=0` 可强制关掉，用来做 A/B 对照（两条路径实测逐像素相同）。
- 两条管线：主目标（4x MSAA）+ 层目标（单采样），除"采样数 + 目标形式"外状态完全一致。
- MSAA resolve 改回设计里写的显式 `vkCmdResolveImage`（一帧一次）。原来靠 render pass 的
  `finalLayout` 顺带 resolve，接上图层后每进出一次主 pass 就隐式 resolve 一遍全屏。
- 合成四边形取**层框本身**（浮点框，不取整），UV 用像素坐标 ÷ 窗口尺寸：采样严格 1:1（屏幕
  像素中心折算过去正好是纹素中心），组边界的小数部分又交给主目标的 4x MSAA 覆盖率做抗锯齿。
  第一版写成"整窗四边形 + UV 0..1"，1:1 成立但丢了边界的小数部分，`translate(10.5px)` 的
  动画用例一比就现形（组边界变 1px 台阶，差 447 像素）。
- 就绪日志多一项：「动态渲染开（组透明度可用）/关（组透明度退回逐元素近似）」。

### 修 — 一帧多次 flush 会把顶点缓冲覆盖掉（图层一上线就满屏白）

`flush()` 原来每次从 0 重拷顶点到映射的槽缓冲，因为**以前一帧只 flush 一次**（在 `present()` 里）
所以从没暴露。图层要在一帧内 flush 多次，后一次 memcpy 把前面所有 draw 的顶点覆盖成最后一批 ——
现象是整帧只剩最后画的那几个四边形。改成 `vcount` 跨 flush 累加、只追加 `[flushBase, vcount)`；
帧内扩容淘汰的旧顶点缓冲挂进 `Slot::retiredVb`，等本槽 fence 之后再释放。

### 修 — D3D11 嵌套图层漏乘外层 alpha

`pushLayer` 一律把 `prevTarget` 记成主目标，嵌套时内层直接合成到主目标上，外层那一次 alpha 就
漏了（6 层嵌套用例差 25% 像素）。改成"父目标 = 外层图层的 RT"。

### 记 — 层目标是单采样的（已知边界，不是 bug）

组**边界**落在小数坐标时，三端都靠合成四边形的 4x MSAA 覆盖率抗锯齿，结果一致；但组**内部**
的小数几何没有 MSAA，边缘比铺在主目标上硬一档，各后端在这类边缘的采样归属也会差：实测
`scale(0.77)` + `opacity` 的用例里 VK 与 D3D11 逐像素相同，GL 差 128 个边缘像素。彻底消掉只能
把层目标做成 4x MSAA + resolve（显存 ×4，4K 上单层 132 MB），取舍与证据记在
`docs/vulkan_design.md` §7.13.7。

### 加 — 组透明度的像素基准用例 `opacity`

`docs/issues/assets/opacity.html` + `docs/pixels/opacity.png`，进 `PIXEL_CASES`。四块场景：
重叠子块、嵌套 0.5×0.5、`overflow:hidden` 里的组、`transform:rotate` + 组透明度。
判据是"重叠区必须是上层子块 × 组 alpha over 背景"，逐元素近似会在这里暗一档、偏色。

实测（420×320 三层页面）：只红区 `(255,127,127)`、红蓝重叠 `(127,127,255)`、父底色 `(127,255,127)`；
逐元素近似是 `(191,127,63)` / `(95,63,159)`。

### 性能 — 动态渲染本身不慢，每层有固定成本

`BINUI_VK_PERF=1`，`vk_window --frames 300`：

| 页面 | 基线 | 结果 |
|---|---|---|
| features 900×932（本来没有 opacity） | render pass 0.182 ms/帧 | 动态渲染 0.170 ms/帧（持平） |
| 420×320，3 个层 | 0.111 ms/帧 | 0.148 ms/帧 |
| 1280×720，12 个层（最坏合成场景） | 0.180 ms/帧 | 0.39 ms/帧（占 16.7 ms 预算 2.4%） |

每层的固定代价主要是主 pass 进出各一次带来的 4x MSAA 附件 LOAD/STORE（1280×720 一次 ≈14.7 MB
带宽），不是层本身。层的 `renderArea` 只取层框，所以 clear 的代价跟着元素面积走。

### 验收

- `make pixels`（GDI 基准，现在 10 个用例）：10/10 0 差异；
- `make pixels-vk` / `pixels-d3d11`（14 用例 + 4 demo + 3 anim）：全部 0 差异；
- `make vk-window`（交换链 / GDI 读回 / 透明窗 / 脏矩形 / resize / 全屏守卫）：全部 0 差异；
- `make vk-stress`（含用 `opacity` 页面单独压一次）：显存记账归零、无泄漏、像素 0 差异；
- `make test` 全绿，`check_examples` 16 示例 0 问题。

细节与实测表：[`docs/vulkan_design.md`](docs/vulkan_design.md) §7.13。

## [0.16.11] - 2026-11-14

把 0.16.9/0.16.10 的图层作用域修对：**owner 自己的底色/边框也必须进层**。

### 修 — 层作用域从"子节点"改成"节点自己"

0.16.9 里判断"层覆盖哪些绘制位置"时，祖先链是从 `pIdx[i]`（父节点）开始走的，
`pushLayer` 那一刻的链也是从 `pIdx[i]` 开始 —— 于是**半透明元素自己那一笔（底色/边框）留在主目标上，
只有子元素进了层**。后果是合成时拿"已经被淡过一次的父底"当背景，结果比浏览器暗一档：

| 区域 | 修前（父底在层外） | 修后（父底在层内，= CSS 组语义） |
|---|---|---|
| 父底 #202020 @50% 压白 | `(143,143,143)` | `(143,143,143)` |
| 只有红 | `(199,71,71)` ❌ 暗一档 | **`(255,127,127)`** ✅ |
| 只有蓝 | `(71,71,199)` ❌ | **`(127,127,255)`** ✅ |
| 红蓝重叠 | `(99,35,163)` ❌（因为层其实没建） | **`(127,127,255)`** ✅ 纯蓝 @50% |

两处各改一行：统计 owner 覆盖位置时从**节点自己**开始往上走；进入层时构造的祖先链也从
**节点自己**开始 —— 这样 owner 的位置在 span 里、链里都有，`pushLayer` 才会真的在它之前发生。

`check_examples` 16 示例 0 问题；`make test` 全绿；三端像素基准 0 个不同像素（基准里没有
"半透明父 + 子节点"的场景，所以像素中性）。

### 记 — Vulkan 的图层实现这轮没打通，代码已回退

试过"复用主 render pass + 层自己的 framebuffer（MSAA + resolve）"这条路，症状是：合成后只剩
最后一个子块且不透明、父元素背景消失。已定位一半（就是上面那个 owner 位置的坑），另一半
（主 pass 重启后内容丢失 / 合成 alpha 失效）没查完，先把代码回退，Vulkan 继续走 `pushLayer`
的 false 分支（0.16.8 的逐元素近似）。下一步建议给层单独写**单附件** render pass + 对应
pipeline，避开 resolve 附件的 `initialLayout = UNDEFINED`。

## [0.16.10] - 2026-11-14

D3D11 后端也接上离屏图层（0.16.9 只做了 GL）。

### 加 — `BinUID3D::pushLayer/popLayer`

和 GL 同一份设计：池化**窗口大小**的 RGBA 目标（texture + RTV + SRV 三件套），上限 4 层；
绘制侧的分组规则完全共用（`binui_draw_nodes.h`），后端只管"换个渲染目标"：

- `pushLayer`：`flushAll()` → 存下当前 `msTarget` → `OMSetRenderTargets` 切到层 RTV → 清成透明；
- `popLayer`：`flushAll()` → 切回原 RTV → 用层 SRV 发一个带预乘 alpha 的四边形 → `flushAll()`；
- `flushAll` 顺手做两件事：按层框裁剪每条批次的 scissor；**画完清零顶点/批次**（否则 present 会把
  合成四边形再画一遍 —— GL 那边踩过这个坑，表现是组透明度彻底失效）。

D3D 的纹理原点在左上、和屏幕坐标一致，所以**不需要**像 GL 那样翻转 V 轴。层内是单采样的
（内容全是无 MSAA 的 quad，圆角/描边本来就是解析式几何）。

### 实测（同一份 optest3.html：父 opacity:0.5 深灰底 + 红块/蓝块部分重叠）

| 后端 | 父底 | 只有红 | 重叠区 |
|---|---|---|---|
| GL（0.16.9） | `(143,143,143)` | `(199,71,71)` | `(71,71,199)` 纯蓝 @50% |
| **D3D11（本次）** | `(143,143,143)` | `(199,71,71)` | **`(71,71,199)`** 逐位相同 |
| Vulkan（还没做） | 同上 | 同上 | `(99,35,163)` 逐元素近似 |

### 验收

`make test` 全绿；`check_examples` 16 示例 0 问题；三端像素基准 **0 个不同像素**（Vulkan 走
`pushLayer` 的 false 分支，像素中性）。

**还没做**：Vulkan 的图层 —— 它的主 render pass 已经是 `LOAD_OP_LOAD`，中途切回来不会清屏，
所以可行；但要为层单独建 render pass + framebuffer，并在 pop 时把层 image 从
`COLOR_ATTACHMENT` 转到 `SHADER_READ_ONLY` 再当纹理采样，比 GL/D3D11 多一层资源与布局管理。
以及层缓存（P0-1）。

## [0.16.9] - 2026-11-14

P1-7 的后半截：**真正的组透明度**（离屏图层，先做 GL）。

0.16.8 只是把父元素的 alpha 乘到子元素身上，子元素互相重叠时重叠区会叠深。这一版在
GL 后端上真的"先渲进图层、再整体乘一次"：

### 加 — `IBinUIGfx::pushLayer/popLayer`（默认不实现，后端零改动）

接口默认返回 `false` → 调用方退回 0.16.8 的逐元素乘 alpha。**没实现的后端（Vulkan / D3D11 /
GDI）在像素上不会有任何变化**，这是加这一层的全部意义。

GL 实现：池化 **窗口大小** 的 RGBA 纹理 + FBO（坐标不用换算、UV 直接是像素坐标 ÷ 窗口尺寸），
上限 4 层，超了退回逐元素。绘制侧只对**绘制顺序连续**、且覆盖 ≥2 个节点的半透明子树建层
（`binui_draw_nodes.h`）——被 z-index 打散的子树继续用近似模式，不会画错顺序。

### 实测（父 `opacity:0.5` 深灰底，红块 + 蓝块部分重叠）

| 区域 | GL（图层） | VK（逐元素，未实现图层） |
|---|---|---|
| 父底 | `(143,143,143)` = #202020 @50% 白底 | 同左 |
| 只有红 | `(199,71,71)` | 同左 |
| **红蓝重叠** | **`(71,71,199)` = 纯蓝 @50%**（CSS 语义） | `(99,35,163)` —— 红光透上来了 |

### 落这个功能时踩到的三个坑（都记在代码注释里）

1. **`flushAll()` 必须清零顶点/批次**：以前一帧只 flush 一次，所以没人注意；有了图层，flush 会
   在同一帧里发生好几次，不清零就会在 present 时把"合成四边形"再画一遍 —— 表现是组透明度
   完全失效（画面全不透明）。
2. **层纹理的 V 轴是反的**：同一套投影渲进 FBO 后，纹理在内存里上下颠倒（GL 纹理原点在左下）。
   UV 不翻转的话，层内容会整体镜像到框的另一半。
3. **合成四边形的顶点色必须预乘**：着色器算的是 `frag = (t.rgb * vcol.rgb, t.a * vcol.a)`，
   rgb 不预乘的话通道会被整块加进去（蓝通道直接爆到 255）。

### 验收

`make test` 全绿；`check_examples` 16 示例 0 问题；`pixels` / `pixels-vk` / `pixels-d3d11`
三端 **0 个不同像素**（没有层的后端走 false 分支，逐像素不变）；appshow 自检帧耗时
16.6~16.7 ms，与改动前逐条相同。

**还没做**：D3D11 / Vulkan 的图层实现（VK 要先改主 render pass 的 `loadOp`），以及层缓存
（P0-1，内容不变就重放图层纹理）——两者的设计都在 `docs/future.md`。

## [0.16.8] - 2026-11-14

缺口清单里的 P1-7：**父元素的 `opacity` 根本不影响子元素**。

### 修 — 组透明度按祖先链级联（不再是"只淡自己"）

先复现：父 `<div>` 写 `opacity:0.25` + 白底，子 `<div>` 纯蓝（不写 opacity），黑底上抓像素 ——
父底 `(255,191,191)`（25% 白压白），**子块 `(0,0,255)` 一点没淡**。也就是说一个卡片做淡出动画时，
背景在淡、文字和子块纹丝不动，和 CSS 的组语义正好相反。

两个根因：

1. **静态 opacity 在编译期就折进了颜色**（`binui_compiler.cpp` 里那段 `fade()`，注释还写着
   "binUI 没有合成组，子节点不受影响"）—— 信息丢了，运行期想级联也没数据可用；
2. **运行时只查了元素自己**（`drawState().opacity` / `drawOpacity()` 都是单节点）。

改法（**不升 `BINUI_VERSION`**，格式字节数没变）：

- 静态 opacity 不再折进颜色，改存进 `BinUIPaintNode` 早就留着的 `reserve[0]`（0..255），
  用新标志位 `PAINT_OPACITY`（`1<<15`，旧二进制这一位恒为 0、同时旧颜色里已经折过 → 行为不变，
  这正是当初留 3 字节 reserve 的用途）；
- 渲染器新增 `staticOpacity()` / `animatedOpacity()` / `ownOpacity()` / `ancestorOpacity()`：
  `drawOpacity()` 与 `drawState().opacity` 现在都返回**级联后**的值（自己 × 所有祖先），
  三个 GPU 后端本来就拿 `st.opacity` 去乘颜色，所以一处改完全都生效；
- 没有静态 opacity、也没有动画的页面走常数快路径（`anyStaticOpacity_` 标志），
  不会给每帧凭空多出几千次哈希探测；
- GDI 后备路径（`app_win32.cpp`，离屏渲染/文档图用它）没有 alpha 合成，改成把级联 opacity
  折进颜色再与底色 `BlendOver`；父元素自己的"看起来的颜色"也按它的 opacity 先算一遍，
  否则子元素会拿父元素的饱和原色去混，明显偏深。

**实测**（同一份 `optest2.html`，蓝子块应变成 `0.25×蓝 + 0.75×父底`）：

| 后端 | 子块像素 |
|---|---|
| GL / Vulkan / D3D11 | `(191,143,207)` ✅ 与解析值逐位相同 |
| GDI（离屏） | `(191,143,207)` ✅ 修前是 `(191,0,64)`（按父元素原色混的） |

### 已知差异（写进 `docs/future.md` 的 P1-7）

这是**逐元素乘 alpha** 的近似：子元素互相重叠时，重叠区会比浏览器"先渲进图层、再整体乘一次"
略深一点。真正的组合成要离屏合成 —— 和 P0-1（层缓存/增量上传）是同一套设施，留给 bin2d 那一轮。

### 验收

`make test` 全绿（`test_css` 里那条"opacity 折进 alpha"改成断言新语义 + 新增"父 0.5 → 子 0.5"级联用例）；
`check_examples` 16 个示例 0 问题；`pixels` / `pixels-vk` / `pixels-d3d11` 三端 **0 个不同像素**
（现有基准里没有"半透明父元素带子元素"的场景，所以像素中性）；appshow 自检帧耗时 16.6~16.7 ms，
与改动前逐条相同（这一阶段是"20 块 × 5 层 × 4 通道设值"的重负载页，不是回归）。

## [0.16.7] - 2026-11-14

缺口清单（`docs/future.md`）里最要紧的两条，P0-5 和 P0-2。

### 修 — 顶点缓冲满了不再**静默丢几何**（P0-5）

三个后端都是 `kMaxVerts = 100000`（≈16,666 个四边形 / 2 MB）的硬上限，写满之后
`vertRoom()` 返回 `nullptr`、`emitQuad` 直接 `return` —— **后面的内容凭空消失，不报错、不降级**。
按实测 61 个四边形/图标（110px AA 圆），一屏图标超过约 270 个就中招；换成 300px 的图标是 34 个。

改成**按需扩容**：初始仍是 10 万顶点，装不下就翻倍（GL 的 CPU 缓冲 / D3D11 动态 VB / Vulkan 的
映射槽缓冲各自扩容，D3D11/Vulkan 重建缓冲时同步重绑），硬上限 100 万顶点（≈17 万四边形）。
只有撞到硬上限才丢图元，而且**只告警一次**（`[gl]` / `[d3d11]` / `[vk]` 各一行 stderr）。

实测（64 个 110px 图标，`gl_shot` 离屏抓）：
- 把上限压到 1,200 个四边形做对照 → 只画出 **20 / 64** 个图标，并打印上限告警；
- 恢复扩容后 → **64 / 64**，三端（GL / Vulkan / D3D11）逐像素 **0 差**（连最大通道差都是 0）。

### 修 — 路径条带缓存的 1024 条悬崖（P0-2）

`kMaxCachedBands` 是单节点缓存上限，1024 条。条带数 ≈ 节点高度 × 1~3，于是 240×240 的图标
（935 条）刚好能缓存，300×300 的（1173 条）就**永不缓存、每帧重算 199 µs** —— 阈值卡在两个相邻
尺寸之间，等于随机。

- 单节点上限放宽到 **16384 条**（≈384 KB）；
- 全局再加一道预算：渲染器记总条带数，**65536 条**（≈1.5 MB）封顶，超预算的节点退回"不缓存"
  （少缓存只影响速度，不影响画面），不会出现"整表反复清空"的抖动；
- `PaintPathNode` 多一个 `cacheBudget` 参数，`路径` 分支画完调 `commitPathBands()` 记账；
  GDI 平台路径（`app_win32.cpp`）也接了同一套。

测试同步：`test_elements` 里那条"条带超上限不缓存"改成"400px 圆现在**应该**缓存"，并新增
"预算给 0 时必须不缓存但照常画"的用例。

### 验收

`make test` 全绿；`check_examples` 16 个示例 0 问题；`pixels` / `pixels-vk` / `pixels-d3d11`
三端全部 **0 个不同像素**（格式仍是 v22，不动 `.bui` 布局）。

## [0.16.6] - 2026-11-14

master："页切换动画感觉不丝滑了" —— 是 0.16.5 那条 **60Hz 封顶**带来的。

整页横移 1058px 在 200ms 里走完：120Hz 是 2.6px/帧、60Hz 是 5.3px/帧，位移动画对帧率的
敏感度远高于别的效果，一眼就能看出"一顿一顿"。**撤销默认封顶，回到跟显示器刷新率同步**；
`BINUI_FPS=<n>` 仍可手动封顶（想省电自己设）。

实测（`BINUI_APPSHOW_AUTO=1` 自检跑完整套页切换/子页滑动/弹窗，读它自己报的 `帧 X ms`）：
封顶时 17.2ms（58fps），撤销后 **8.8ms（≈114fps）**。

0.16.5 的另外三条（pump 空闲超时、`update()` 节拍闸门、最小化短路）都保留 —— 它们修的是
"没动画时的空转"，不影响活跃帧率：后台被盖住 ~3%、最小化 0%。

## [0.16.5] - 2026-11-14

master 在任务管理器里看到 appshow 放后台还"一会 7%、一会 0.6%"。查出来是**帧循环**的三处
毛病，跟渲染无关（每帧 update+draw 只有 0.28ms，时间是烧在等 vsync 和空转上）：

### 修 — 最小化/后台不再空转，并给宿主的 update 加节拍闸门

1. **pump 的 1ms 空转**：`run()` 以前是 `while (running_) pump(1)`，而 `renderFrame()`
   第一件事就是 `update_(dt)` —— 闲着的时候宿主每秒被推进 **1000 次**（每次都要算一遍
   漂移/重排判定），这就是 0.5% 起的底噪。现在 pump 的空闲超时 = 一个帧预算，并且
   `renderFrame` 里加了闸门：没有脏区时最多每帧预算跑一次 update。
2. **自旋**：闸门命中时如果不清 `lastDrew_`，pump 会一直认为"还有活"→ present 又不阻塞
   → 空转吃满一个核（实测 41%）。这一条是这个版本最大头。
3. **最小化**：`IsIconic` 时直接返回（同时清脏区/`lastDrew_`），不再推进宿主时钟、不再画。
   dt 超过 0.1s 会被钳到 1/60，恢复时动画不会跳。

### 改 — 帧预算默认封顶 60Hz（`BINUI_FPS=<n>` 可改，0 = 跟显示器）

这台机器的显示器报 120Hz，于是"每帧都出"= 每秒 120 次 present，每次都要等一次垂直同步：
池子页实测 **12.5% 一个核**，其中画面本身只占 0.3ms。UI 没有必须 120fps 的场景（动画本身
是亚像素抗锯齿的），封顶 60 后同样的画面 **5.5%**。同时把 `nextFrameTime_` 统一按
"本拍开始 + 预算"定，pump 用高精度定时器精确等（present 只等到下一次刷新，封顶后还差
半拍要自己补，否则会按系统计时器 15.6ms 的精度多睡，掉到 ~50fps 且节奏抖）。

实测（24 核机器，池子页 20 块半透明组件，`cpu = 一个核的百分比`）：

| 状态 | 改前 | 改后 |
|---|---|---|
| 前台池子页 | 12.5%（118fps） | 5.5%（58fps，间隔 16~17ms） |
| 后台被别的窗口盖住 | 15.5% | 1.7% |
| 最小化 | 13.7% | 0.0% |
| 完全没动画的页面 | ~0.5% | ~0.1% |

## [0.16.4] - 2026-11-14

填充/描边能连续滑了，字还在抖。master 问得对："泡泡在形变、文字又不变，它只是跟随移动，
怎么会抖？难道 flex 重排了？" —— **没有重排**（气泡的标签是 `position:absolute` + 百分比，
`ANIM_TRANSLATE/SCALE` 只改绘制态的位移/缩放，不动布局）。抖的是**字形落位**：

`GlyphAtlas::drawText` 里有一句 `floor(x + 0.5f)`（v13 起就有的"像素对齐"，字形位图
1:1 落在半像素上会糊），于是"平滑移动的小数坐标"被吸成整像素 —— 0.5px/帧时每 2 帧跳
1px。旁边填充和描边都在连续滑，只有字在跳，反而更显眼。

### 加 — 文字亚像素定位（只给动画节点）

- `GlyphAtlas` 加 `snapText` 开关 + `IBinUIGfx::setTextSubpixel(bool)`（默认 no-op，
  三个后端都实现）；绘制层按 `animGeo`（有位移/缩放）逐节点设。
- 字形图集纹理 NEAREST → **LINEAR**（GL / VK 各自建纹理处；D3D11 是按批次选采样器，
  改成 `curLinear = subpixelText`）。整数位置下 1:1 采样点正好落在纹素中心，
  **逐像素与 NEAREST 相同** —— 静态页面不受影响，`pixels` / `pixels-vk` / `pixels-d3d11`
  全 0 差异。

结果是：不动的字仍然锐利，动起来的字连续滑（代价是亚像素位置的笔边会被插值，略软 ——
要好只能给图集做 ¼ 像素多份字形，那是下一步）。

## [0.16.3] - 2026-11-14

0.16.2 的示例侧改动**方向错了**：把泡泡位移取整成整像素，内部确实不再互相错位，但整块
变成 30Hz 步进（≈0.5px/帧 → 每 2 帧挪 1 行），master 一眼看出"更抖了，移动不顺滑"。
根因不在宿主取整，而在**填充边缘没有抗锯齿**。

### 修 — 半透明渐变的填充边缘也抗锯齿

`BuildFillBands` 的半透明分支改成**逐行光栅化 + 四边部分覆盖**（新增 `PaintEmitAa`）：

- 一行一条，颜色取**像素中心**处的渐变值（1D 抖动照旧按行累积）；
- 行的纵向覆盖率按形状边界算：首行/末行各是半行（`alpha × 覆盖率`），中间整覆盖；
- 行的左右边界各拆一条部分列（覆盖率 = 小数部分），圆角区逐行内缩照旧。

于是填充边缘和 1px 圆角描边（解析式 AA）同一个口径，亚像素位移时两者一起连续滑。
整数坐标（静态页面）时覆盖率恒为 1 ⇒ 发出去的矩形与旧版逐像素相同，`pixels` /
`pixels-vk` / `pixels-d3d11` 全 0 差异。

条数上限 512 → 768（半透明一条最多拆 3 块，覆盖到 ~253px 高的元素；更高退回硬边条带）。

### 撤 — 0.16.2 里示例侧的整像素取整与"去掉呼吸"

`examples/appshow` 恢复亚像素 `translate` 和每块的呼吸（连续改 `scale`）—— 填充 AA 之后
连续尺寸变化也不会抖。示例/文档里"按整像素取整"的说法一并改掉。

仍然整像素落位的只剩**文字**（字形图集 NEAREST，亚像素采样会糊）：泡泡里那两行标签
还会 1px 跳，这是当前最后一个已知的"不顺滑"来源。

## [0.16.2] - 2026-11-13

master 看着池子说"泡泡移动时在抖动"——不是故意加的，是真抖。逐帧抓图量出来的：泡泡往上飘
（≈0.5px/帧）时，顶边每隔一帧就在"1 行满覆盖的亮边"和"2 行各半"之间跳，并且每隔一帧顶上
多浮出 1 行纯填充色（18 级亮度差）。

### 诊断 — 填充边缘和 AA 边框各吸各的像素

一块泡泡是三层叠加：半透明渐变填充（**纯色矩形条带，没有抗锯齿**）、1px 圆角描边（解析式
AA，九宫格模板）、文字（字形只能整像素落位）。亚像素位移时三者的取整规则不一样：

- 填充的上边界被 `BuildFillBands` 的 `std::round` 吸到整像素；
- 边框是九宫格按小数 y 光栅化的，自己按像素中心取整；
- 两者每隔一帧就差 1px ⇒ 边框的 AA 有时压在填充上、有时压在池子底上，亮度来回跳。

同样的错位在水平方向也有，只是泡泡的横向速度（≈0.2px/帧）把它摊成了 5 帧一个周期，看不出来。

### 修 — 条带切点不再吸整像素

`BuildFillBands` 的半透明分支去掉 `std::round`：切点保留小数、条带首尾**严格相邻**（不重叠，
所以不会双混合），取整交给后端光栅化 —— 和边框同一个规则，错位就没了。
静态页面（整数坐标）逐像素不变，`pixels` / `pixels-vk` / `pixels-d3d11` 全 0 差异。

### 改 — 示例里泡泡的位移按整像素取整，并去掉"呼吸"

引擎有亚像素能力（v0.11.7，`animshow` 的滑动靠它），但填充边缘没有抗锯齿时，亚像素位移
换来的不是"更顺"而是"内部互相错位"。`examples/appshow` 的泡泡改成 `round(dx/dy)`：整块
（填充 + 描边 + 文字）一起按整像素走，内部不再抖。

顺手去掉了每块泡泡的**呼吸**（连续改 `scale`）。同一个病：尺寸带小数 ⇒ 描边按小数光栅化、
填充按像素取整，顶边亮边在 85 ↔ 109 之间跳（一次呼吸能跳好几回）。爆开那一下的缩放留着
（只有 0.24s，一闪而过看不出来）。引擎的 `ANIM_SCALE` 没动，只是这一页不再连续改尺寸 ——
要"呼吸"得先让填充边缘也抗锯齿。

实测（顶边同一段 x270..300，逐帧取一行像素剖面，17ms 间隔连拍 10 帧）：

| | 改前 | 改后 |
|---|---|---|
| 顶边剖面 | 偶数帧 `rim` 分两行 39/60，奇数帧 `rim` 单行 + 上方多一行纯填充 | 恒定 `18 / 108 / 37 / 36 / 37`，每 2 帧整体挪 1 行 |
| 亮边亮度 | 85 ↔ 109 来回跳 | 恒 108~109 |
| 泡内位移 | 填充边缘与描边错开 1px | 整块刚体平移，内部零相对位移 |

## [0.16.1] - 2026-11-13

接着上一版：抖动把池子底板的 24px 色带揉平之后，底板反而"看不见"了（渐变只跨 12 个色阶 +
1px 边框几乎同色，远看就是一片黑），而**半透明**的泡泡还是粗条带 —— 判定阈值只看不透明
渐变的色阶步长，alpha 0.2~0.3 的渐变直接走了粗路径。

### 修 — 半透明渐变也细分 + 抖动

`BuildFillBands` 的半透明判定提前，`bandPx` 的条件加上 `translucent`：alpha 渐变（泡泡本体）
同样按 1px 切条带并做误差扩散。实测 块 11 的内填充从"每 24px 一道硬边"变成逐行 ±1。

### 改 — 示例池子的底板对比度

抖动负责"平滑"，对比度负责"这块画布在"。`examples/appshow` 的 `.pool` 渐变从
`#0B1424 → #17263F` 拉宽到 `#0A1220 → #22375A`、边框 `#22314A → #34496C`：
底板重新看得出来，渐变本身仍然没有色阶。

## [0.16.0] - 2026-11-13

本轮主题：**深色渐变的色阶**。实机看「半透明重叠」池子时背景有一条条横带 —— 这不是 8-bit
显示/截图链路的问题，是**我们自己切的纯色条带**：`BuildFillBands` 把渐变直边区按 24~32px
一条均分，而 `#0B1424 → #17263F` 整个渐变只跨 12 个色阶（R），一块 40px 高的色带当然看得见。

### 修 — 渐变条带细分 + 一维抖动

- 直边区按"每个色阶占多少像素"决定细分：台阶 > ~6px 就切 **1px 一条**（原来 24~32px）；
  台阶本来就细（颜色变化快的渐变）保持粗条带，省顶点/绘制次数。
- 条带颜色对 R/G/B 做**沿渐变方向的误差扩散**（1D 抖动）：条带是纯色矩形，没法像素级抖动，
  但切到 1px 后按行累积取整误差，相邻行就在 floor/ceil 之间交替 —— 把 40px 的台阶摊成
  1px 细噪点。**alpha 不抖**：半透明叠加上的抖动会变成可见噪点。
- `kMaxPaintBands` 128 → 512（1px 一条要够长；`PaintBand` 都在栈上，量级 +2KB）。
- 实测（appshow 池子背景，同一列 285 行）：改前是"亮 30 行 + 跳 1 个色阶"，改后逐行在
  ±1 之间交替（285 行里 240 次变化），肉眼看是平滑的。GDI 后端同一条路径，0 条黑缝。

### 测试

`test_paint` 里"半径超过条带上限时退回直角四条"改成显式给小 cap（原来拿 `kMaxPaintBands`
当输入，常量一调大这条断言就失效了）。三端像素金标 0 差异（抖动只动 ±1 个色阶，低于阈值 8）。

## [0.15.0] - 2026-11-13

本轮主题：**中键**（以及把按键编号换成 DOM 的约定）。上一版只有 `PRESS`/`RELEASE`/`CONTEXT_MENU`，
中键按下去会被当成左键走到 `CLICK`，而 `pointer.button` 又是 `1=右 2=中` 这套自创编号 —— 从 Web
迁过来的人一定会把中键当右键。

### 新增 — `AUX_CLICK`（中键）

- `EventType::AUX_CLICK` / `BINUI_EV_AUX_CLICK`：非主按钮的点击，对应 DOM 的 `auxclick`。
  触发出 `PRESS` → `RELEASE` → `AUX_CLICK`；**不产生 `CLICK`、不参与双击判定、不改键盘焦点**
  （浏览器里中键点击就是"旁路点击"）。
- 右键仍走 `CONTEXT_MENU`（菜单请求，`hitTestAny` 命中口径，不进按下态）。
- **非主按钮的命中口径跟右键一致**（`hitTestAny`：点到哪报哪，普通 div / 文字也算）——主按钮
  仍只认"可交互 / 带状态样式"的节点，否则整页元素都会变成可点。刚开始漏了这条，中键点在
  概览标题的文字上什么都收不到（实机验证时才发现）。

### 改动 — `button` 编号与 DOM 对齐（**语义变更**）

0=左 1=中 2=右（原来 1=右 2=中）。涉及 `renderer.press/release` 的 `button` 参数、`Event::pointer.button`、
外壳的 `setButtonFn`、C ABI 的 `BinUIEvent::button`。`BINUI_C_ABI_VERSION` 3 → 4：结构体没变，
但字段语义变了 —— 旧宿主会把中键当右键，所以同样要重编。

映射表：`WM_LBUTTON*` → 0、`WM_MBUTTON*` → 1、`WM_RBUTTON*` → 2。

### 示例 / 测试 / 文档

- appshow：中键任意控件 → 弹窗显示"中键走 AUX_CLICK"，与右键的 `CONTEXT_MENU` 弹窗对照。
- 测试：中键三段事件、`AUX_CLICK` 的 `button == 1`、"中键不进双击判定"；右键断言改为 `button == 2`。
- 文档：README 事件表 + bindings.md 事件表补 `AUX_CLICK` 与编号约定。

## [0.14.1] - 2026-11-13

修一个"右键没反应"的实机问题：`CONTEXT_MENU` 原来用 `hitTestEx(allowStyled=true)` 取节点，
那套口径只认 `BINUI_INTERACTIVE` 节点和"声明了 `:hover/:active`"的普通元素 —— 在普通
`div`/文字上右键一律拿到 -1，宿主按"没有节点就不弹菜单"处理，看起来就是右键无效。

- 新增 `hitTestAny(x, y)`：纯按绘制顺序取**光标下最上层可见节点**（不限可交互），
  `CONTEXT_MENU` 改走它 —— 和 DOM `contextmenu` 的 target 语义一致。
- appshow：右键任何位置都会弹窗报"点到了哪个节点"，空白区域也报（`nodeId = 0`），
  不再有"点了没动静"这种无法区分的情况；顺带在 stdout 打一行 `[appshow] contextmenu …`。
- 测试：新增"普通 div 上右键也报 CONTEXT_MENU"（无状态样式的节点）。

## [0.14.0] - 2026-11-13

本轮主题：**把事件补齐**（上一版补了动画完成事件，这一版补指针/焦点/编辑/滚动/拖拽/加载）。
判定标准是"宿主不轮询、不猜时间、不重复实现引擎已经知道的事"——凡是要宿主自己算一遍的，
都算缺口。**冒泡 / 事件委托 / `Event` 对象 / `CustomEvent` 仍然不做**：binUI 不是浏览器，
组件↔宿主靠属性 + `setText` + 宿主编排（真要组件通信应该是具名事件通道，不是复制 DOM）。

### 新增 — 事件（引擎 `EventType` 从 11 种到 25 种）

| 事件 | 之前 | 现在 |
|---|---|---|
| 指针 | 只有 `PRESS` | `PRESS` / `RELEASE` / `MOVE` / `DBL_CLICK` / `CONTEXT_MENU`，都带 `button` 与坐标 |
| 悬停 | 只有进入（`HOVER`） | 加 `HOVER_LEAVE`（含鼠标移出窗口） |
| 焦点 | `FOCUS` 一个类型两义（进入/退出编辑），**键盘焦点变化完全没有事件** | `FOCUS`（键盘焦点，带 `focused` + `prevId`）+ `EDIT`（进入/退出编辑）分离；Tab/WASD/点击/API 换焦点都会报 |
| 文本 | `INPUT` 同时是"逐字"和"提交" | `INPUT` = 逐字（含 IME 上屏），`CHANGE` = 提交（失焦或 Enter 退出且有改动） |
| 滚动 | `SCROLL` 只有 `nodeId` | `SCROLL` 带新位置 + 本次增量；新增 `WHEEL`（光标位置 + 增量） |
| 元素拖拽 | 只能轮询 `isResizing()` / `takeResizeDirty()` | `RESIZE_START` / `RESIZE_END`（END 带最终矩形） |
| 加载 | 只有 `loadLayout` 的 bool 返回值 | `LOAD` 事件（回调需在 `loadLayout` 之前设好） |

要点：

- **双击由引擎判**（同节点、400ms 内、位移 < 4px），不依赖 `WM_LBUTTONDBLCLK`——自绘窗口里
  那条路常因 `SetCapture`/焦点变化丢事件；三连击不会再报一次双击。
- **`RELEASE` 只在"确实按下过"时报**，`nodeId` 用按下时命中的节点（鼠标可能已经移开），
  宿主据此收按住态不会漏。
- **右键不抢焦点、不进按下态**：`press(x,y,1)` 直接派发 `CONTEXT_MENU`，菜单长什么样归宿主。
- 想要节点 id 就需要把按键号传到引擎：外壳新增 `setButtonFn`（`WM_RBUTTONDOWN/UP`、
  `WM_MBUTTONDOWN/UP`），appshow 已接上（右键任意控件 → 弹窗显示"点到了谁"）。

### 改动 — C ABI 升到 v3

`BinUIEvent` 追加 `dx` / `button` / `focused` / `editing` / `prev_id`；新增
`BINUI_EV_PRESS/RELEASE/MOVE/DBL_CLICK/CONTEXT_MENU/HOVER_LEAVE/FOCUS/EDIT/CHANGE/SCROLL/RESIZE_START/RESIZE_END/LOAD`。
结构体又变大一次，`BINUI_C_ABI_VERSION` 2 → 3，**宿主必须按新头文件重编**。C 侧用法不变，
仍是同一个 `binui_poll_event` 队列。

### 测试

`tests/test_elements.cpp` 新增 24 条事件断言：LOAD、`MOVE` 的命中节点、`HOVER`/`HOVER_LEAVE`、
`PRESS→RELEASE→CLICK` 顺序与 `button`、双击判定与"三连击不再报"、右键只发 `CONTEXT_MENU`、
键盘焦点获得/失去、`EDIT` 进入与退出、`INPUT`↔`CHANGE` 分离、`WHEEL`/`SCROLL` 载荷、
`RESIZE_START` 与 `RESIZE_END` 的最终矩形实测值。

## [0.13.1] - 2026-11-13

本轮主题：**动画完成事件**（`animationend` / `animationiteration` / `transitionend`）。这是
"事件相比浏览器还差什么"里最碍事的一条：`stepAnimations()` 只说"这帧哪些节点变了"，
**没人告诉你动画结束了**，所以宿主要串下一段编排只能自己按 `durationMs` 定时 —— 等于把
引擎的时间轴重写一遍，`delay`/`iteration`/`direction` 一变就对不上。

### 新增 — 动画事件

```cpp
for (const auto& e : renderer.takeAnimEvents()) {   // 每帧 stepAnimations() 之后取一次
    // e.nodeId / e.kind（ANIM_END · ANIM_ITERATION · TRANSITION_END）/ e.prop / e.iteration
}
```

- **是"取"不是"回调"**：事件在 `stepAnimations()` 内部产生，宿主常在同一条路径上改样式/
  删节点，回调会就地重入 —— 和 `takeResizeDirty()` 同一个理由。
- 迭代事件同一帧跨多次只报一条（带当前次数），卡顿不刷屏；`infinite` 不报结束；
  `fill-mode` 不影响事件；`restartAnimations()` 丢掉排队中的旧事件。
- 判据是"时钟 ÷ 单次时长"，与 `EvalKeyFrameAnim` 同源，所以 delay / 有限迭代都算得准。

### 改动 — C ABI 升到 v2

`BinUIEvent` 尾部追加 `prop` / `iteration`，新增 `BINUI_EV_ANIM_END` / `BINUI_EV_ANIM_ITER` /
`BINUI_EV_TRANS_END`。结构体变大就是 ABI 破坏，所以 `BINUI_C_ABI_VERSION` 1 → 2：**宿主必须
按新头文件重编**（旧头文件 + 新 DLL 会被写出界）。C 侧用法不变 —— 库把这三类事件排进同一个
队列，照旧 `binui_poll_event` 轮询。

### 测试

`tests/test_elements.cpp` 动画段新增 6 条：1 次迭代的关键帧报 `ANIM_END`（`iteration == 1`）、
`infinite` 动画永不报结束但报迭代、hover 的 `transform` / `background-color` 两条过渡各报
`TRANSITION_END`、以及与 `prop` 对得上、**结束事件只报一次**。

## [0.13.0] - 2026-11-13

本轮主题：**flex 的属性补齐**。上一版查"flex 子项为什么不换行"时发现三件事：`flex:1`
是"纯切分、不看内容"（CSS 正确），但 `flex-wrap`/`flex-shrink`/`align-self`/`align-content`/
`order` 全都落在"接受但无效"的清单里，而且 **`flex` 简写只读第一个数字**——`flex:0 0 120px`
被静默当成 `flex:1` 等分。这一版把这一族属性全部做完。

### 新增 — flex 属性（`BINUI_VERSION` 21 → 22）

| 属性 | 之前 | 现在 |
|---|---|---|
| `flex` 简写 | 只读第一个数，`flex:0 0 120px` → 等分（静默错） | `grow [shrink] [basis]` + `auto`/`none`/`initial` |
| `flex-grow` / `flex-shrink` / `flex-basis` | 不支持（只认简写） | 支持；`flex-shrink` 真参与收缩计算 |
| `flex-wrap` | 忽略 | `nowrap` / `wrap` / `wrap-reverse` |
| `align-content` | 忽略 | 多行时按 start/center/end/between/around/evenly/stretch 分行间距 |
| `align-self` | 忽略 | 逐项覆盖 `align-items` |
| `order` | 忽略 | 主轴排序（同值保持文档顺序） |
| `justify-content: space-evenly` | 不支持 | 支持 |
| `align-items: stretch` | 不支持 | 支持（未声明交叉轴尺寸的项拉伸占满整行） |

- **一份实现**：`include/binui_flex_layout.h`（header-only）。编译期布局与运行期
  `setSize`/`relayoutSubtree` 从此调同一个函数 —— 原来两边各写一份，加特性必然改出
  "编译期和运行期不一样"。
- **溢出收缩**：按 CSS 的缩放收缩因子（`shrink × basis`）加权，`flex-shrink` 默认 1。
  两个 150 挤 200 的容器现在各缩到 100（以前固定宽项根本不缩、直接溢出）。
- **折行 + auto 高**：`wrap` 时按假设主轴尺寸贪心装行；容器没声明 `height` 时 auto 高 =
  各行交叉轴之和 + 行间距，并且**把流式父级的光标补上差值**（编/运两端各一处）——
  不补的话折出来的第二行会压住后面的兄弟。
- **拖拽优先**：宿主用 `resizeNodeBounds` 钉住的盒子不再被 flex 的 auto 高覆盖。
- 三处明确偏离 CSS（都为了不破坏既有布局，写进 README）：`align-items` 默认 `flex-start`、
  row 里"宽和 flex 都没写"按 `flex:1`、`baseline` 按 `flex-start`。

### 改动 — 示例与文档

- `examples/appshow` 子页面 ①：四个 `flex:1` 色块改成 `flex:1 1 120; min-width:56` +
  容器 `flex-wrap:wrap`，并加一行读数（`flex N 行 · 子项宽 N`）。拖窄卡片时能直接看到
  "基准 120 装不下就换行，容器 auto 高变高，下面的百分比色条跟着下移"。
- `docs/ui_orchestration.md` §10 补 flex 属性表与折行/收缩的规则；README 的 flexbox 条目重写。
- 截图更新：`docs/img/appshow-subpage.png`（640 宽，1 行）、`appshow-subpage-drag.png`
  （380 宽，2 行）。

### 测试

- `tests/test_elements.cpp` 新增 18 条：flex 简写（`flex:0 0 120px` 固定 120 不复现旧 bug、
  余量归 `flex:1`）、`flex-shrink` 默认 1 与 `:0`、`flex-wrap` 折行 + auto 高 70 +
  **运行期改窄重新折行**（编译期/运行期一致）、`align-self`、`order`、`align-content`、
  `flex-basis` 长写、`flex:0 0 auto` 按内容宽。
- `tests/test_css.cpp` 的"flex overflow 不许负间距"改为断言新语义（400 挤 300 → 各 150）。

## [0.12.14] - 2026-11-13

本轮主题：**把"布局与重排"做成子页面（从右边切入）** —— 四块演示塞在概览页里太挤，
单开一页；入口只留一张卡，点「进入」时视口横向滚动 1058px，子页面从右边顶进来。

### 新增 — 子页面切换（视口 X 滚动）

和上下切页**同一个 `scrollTo`，只是换了轴**：竖页带排在 x 0..1058、子页面排在 1058..2116，
滚到 1058 就是"竖页带滑出去、子页面顶进来"。宿主只管时间：`subX` 从 0 缓动到 1058
（ease-out 300ms），每帧把 `(subX, scrollPos)` 一起提交给视口，返回按钮反向（260ms）。

两个要点（都写进 `docs/ui_orchestration.md` §10）：

1. **宿主自己的脏矩形要减横向偏移**：`ScreenX(layoutX) = layoutX − subX`。引擎那侧
   （命中/裁剪/滚动）本来就按"减滚动偏移"算，宿主漏了这条，读数刷新就会落在窗口外。
2. **切页优先**：在子页面里点侧栏导航时先把横向偏移瞬间归零，再走上下滚动 ——
   否则两条动画同时改同一个视口会互相拉扯。

### 新增 — 中文避头点（折行时的标点规则）

自动折行时，行首不能是 `，。、；：！？）》」』】…` 这类标点（CSS `line-break: normal`
的常见规则）：遇到就把该标点并回上一行末尾，允许极轻微超出可用宽度。不这么做，
窄卡片里的中文段落会出现"行首孤零零一个逗号"。

### 变更 — appshow 概览页瘦身

原来的三张小卡片 + 提示行换成一张"布局与重排 · 子页面"入口卡；四块演示搬进子页面，
空间够了（① 流式+折行+flex 640×296 ② min/max 钳制 344×296 ③ 百分比/auto ④ 只开对角）。

### 验收

0 warning；四套测试全绿；`pixels` 全 0 差异；`pixels-vk` 17/17、`pixels-d3d11` 全过；
`check_examples` 16 个文件 0 问题；`appshow` 自检报
`sub-open ... 滚动=(1058,0)（子页 开）` → 拖卡片 `子页面卡片 380×231（拖动前 640×296）… 正文高 32 → 49 · flex 子项宽 142 → 78`
→ `back-in ... 滚动=(0,0)（子页 关）`；真实窗口截图见 `docs/img/appshow-entry.png` /
`appshow-subpage.png` / `appshow-subpage-drag.png`。

## [0.12.13] - 2026-11-13

本轮主题：**自然内容重排** —— 0.12.12 的局部重排只管"位置按新盒子走"，文字高度还是编译期
算的那份。这一版把"高度也跟着文字走"补上：盒子变窄，文字折行、节点变高、后面的兄弟让位。

### 新增 — 文字自然折行（布局 + 绘制两侧）

| 侧 | 做了什么 |
|---|---|
| 布局 | `autoTextHeight(idx, width)` 抽出来，`setText`（换文本）与 `relayoutFrom`（换宽度）共用；`heightAuto` 且作者没写 `height` 的 LABEL/LIST/BUTTON 按当前宽度重算行数 → 高度 = 行数 × `line-height`（未声明 1.35×字号），并受 `min/max-height` 钳制 |
| 绘制 | LABEL 一行放不下（`textWidth > 内容宽`）时走 `drawTextWrapped` 折行：行数用同一份估算、块高按 `line-height`、垂直居中；放得下仍是原来的单行路径（像素基准不变） |
| GDI 后备 | 折行阈值从"超宽 20% 才折"改成"放不下就折"，与 GPU 后端对齐（以前同一份 UI 在 GDI 里单行裁掉、在 GL 里折行） |

顺序很关键：重算高度必须在**流式光标推进之前**做完，否则后面的兄弟按旧高度落位
（现象是"文字折行了，但下面的元素没让开"）。flex 子项**不套用**文字自然高度 ——
它的高度由父容器分配（`flex:1` 撑满剩余），文字高度只是 flex-basis 的参考。

### 变更 — 基准图更新

`docs/pixels` 的 `svgaa` / `svgfinish` 两张换新：页面底部那行说明文字原来被裁掉，现在按
盒子宽度折成两行（这是新的正确行为，不是回归）。其余 8 张逐字节未变。

### 验收

0 warning；`pixels` 全 0 差异、`pixels-vk` 17/17、`pixels-d3d11` 全过；四套测试全绿 ——
`test_elements` 新增 11 条"自然重排"断言（压窄后正文变高、兄弟被顶下去、正文宽度跟内容盒、
flex 子项重新等分、拉宽后高度回落）；`check_examples` 16 个文件 0 问题；`appshow` 自检报
`自然重排：正文高 30 → 45（换行变高）· flex 子项宽 139 → 69`；真实窗口截图三行正文 +
三个色块重分 + 百分比色条跟着窄。

## [0.12.12] - 2026-11-13

本轮主题：**局部重排** —— 上一版拖尺寸只改那一个盒子，子节点留在原地（当时写成"还没有
运行时重排子树的接口"）；这一版把那个接口补上，拖完尺寸内容跟着排。

### 新增 — `relayoutSubtree` / `resizeNodeBounds` / `clearBoundsOverride`

窗口 resize 那套重排（百分比 / `width:100%` / 流式光标 / flex 主次轴 / min·max /
线段端点）本来就是"从根往下走一遍"。这版把它拆成 `relayoutFrom(startIdx)`：**同一个
算法，换个起点，且不把起点钉回窗口尺寸**。对外三个口子：

| 接口 | 用途 |
|---|---|
| `relayoutSubtree(id)` | 盒子没变，只按当前盒子重排这棵子树（宿主换文本撑高后调用） |
| `resizeNodeBounds(id,x,y,w,h)` | 改矩形 + 钉住尺寸覆盖 + 重排；缩放拖拽内部就用它 |
| `clearBoundsOverride(id)` | 解除钉住：下次窗口重排回到作者声明的尺寸 |

三条规则：

1. **钉住的尺寸在流式光标推进之前生效** —— 否则后面的兄弟会按旧高度落位（流式光标
   是按 `h` 累加的），表现是"拖高手柄、下面的兄弟不动"。
2. **节点在流式/flex 父容器里时重排父容器**（兄弟一起让位），否则只重排自己这棵子树。
3. **绝对定位的子节点仍不动**，流式 / 百分比 / 自动宽的子节点跟着走 —— 与 CSS 直觉一致。

`setNodeBounds` 保持原语义（只改一个盒子、不动内容，宿主自己排版时用），没有改成重排。

### 变更 — `appshow` 去掉 `ui_scale="proportional"`

等比缩放模式下整棵树按窗口比例缩放，没有"内容盒重排"这回事（`relayoutSubtree` 在那种
窗口里返回 false）。`appshow` 本来就是 `window_resize="disable"`、用不上等比缩放，属性去掉；
第 1 页三块可缩放卡片的子节点改成**流式 + 百分比**（62% / 40% / 52% 的色条），拖尺寸时
能直接看到内容跟着重排。

### 修复 — 缩放拖拽的脏矩形按"子树包围盒"取并集

局部重排会挪动子节点；原来只按盒子本身失效，缩小时被挤出去的那一圈会留残影。现在
`subtreeScreenBounds()` 把整棵子树（含祖先滚动偏移）的包围盒取出来，旧新两个并集进
`takeResizeDirty()`。

### 验收

0 warning；`pixels` 全 0 差异、`pixels-vk` 17/17、`pixels-d3d11` 全过；四套测试全绿 ——
`test_elements` 新增 16 条局部重排断言（百分比/自动宽跟着盒子变、`relayoutSubtree` 与
`resizeNodeBounds` 等价、流式兄弟让位、钉住尺寸在窗口重排后保持、解除后回到声明尺寸）；
`check_examples` 16 个文件 0 问题；`appshow` 自检报
`可缩放面板 540×258（拖动前 470×212）… 62% 色条 312（拖动前 269）`；真实窗口里拖大两块后
卡片内三条色条同步变宽、只开对角的那张同样跟着重排。

## [0.12.11] - 2026-11-13

本轮主题：**元素的边/角拖拽缩放**（CSS `resize`，【格式 v21】）—— 把"窗口改大小"
搬进 UI：贴到元素开着的边上，光标变成系统 resize 形状，按住拖动直接改这个盒子。

### 新增 — `resize` 属性（引擎接管拖拽，宿主只读尺寸）

`resize: both`（四边 + 四角）/ `horizontal` / `vertical` / `left|right|top|bottom` /
`top-left|top-right|bottom-left|bottom-right`，也接受 `left-resize` 这种拼法。
边和角**分开记位**：只写两个角时，两条边的中间段不响应（比"有左边就自动有角"更贴近
作者的意图）。

| 引擎负责 | 宿主负责 |
|---|---|
| 热区判定（贴边 ±4px、四角 12×12、最上层优先、禁用节点/视口外不算） | 接一行 `app.setCursorFn`（9 个示例 + C ABI 都接了） |
| 拖拽：快照原矩形 + 祖先滚动偏移，拖哪条边哪条动、对边不动，最小 8px | `takeResizeDirty()` 取"旧 ∪ 新"脏矩形做局部重绘 |
| 4 种系统 resize 光标（`SizeWE/SizeNS/SizeNWSE/SizeNESW`） | 读回节点矩形（`appshow` 的尺寸读数） |
| 不产生 `PRESS`/`CLICK`；滚动条优先 | 运行期锁死尺寸：`setResizeMask(id, 0)` |

格式：`BinUIPaintNode` 追加 `resizeMask`（44 → 48 字节，含 3 字节预留）+
`PAINT_RESIZE` 标志位，`BINUI_VERSION` 20 → 21 —— 旧二进制需重新编译。

### 修复 — C++ 示例里光标形状从来没生效过

`setCursorFn` 只有 C ABI 接了，9 个 C++ 示例一个都没接：输入框不变 I 型、按钮没有
手型、滚动条没有上下箭头。这一版全部接上（`app.setCursorFn([&](x,y){ return
(int)renderer.cursorAt(x,y); })`），`CursorKind` 同时扩了两种对角光标。

### 已知边界（写进 README 与 docs）

- **只改这一个盒子**：子节点不重排（没有运行时重排子树的接口），超出部分由 `overflow`
  决定裁不裁；"内容跟着缩"需要宿主自己按新尺寸排子节点。
- 可滚动容器的边上**滚动条优先**（`press()` 先判滚动条），想拖尺寸就开在没有滚动条的边上。
- 脏矩形必须是"旧 ∪ 新"：只失效新矩形，缩小后会留一圈旧边框残影。

### 验收

0 warning；`pixels` 10/10、`pixels-vk` 17/17、`pixels-d3d11` 全过；四套测试全绿
（`test_elements` 新增 26 条 resize 断言：位图解析 / 热区光标 / 四边与四角拖拽 /
最小尺寸 / 脏矩形 / 运行期开关）；`check_examples` 16 个文件 0 问题；`appshow`
自检脚本报「可缩放面板 540×258（拖动前 470×212）光标 右边缘=4 右下角=5 进入缩放态=1」；
真实窗口里逐点查 `GetCursorInfo`：左右 `SIZEWE`、上下 `SIZENS`、左上/右下 `SIZENWSE`、
右上/左下 `SIZENESW`，只开对角的卡片边中间是箭头、只开左右的卡片上边是箭头。

## [0.12.10] - 2026-11-12

第 3 页在真实窗口里是**空的**（静态离屏图却正常）。根因是引擎的一个几何 bug：
**滚动容器里的裁剪容器，裁剪矩形没跟着滚动偏移走**。修完这一条，"半透明组件重叠"
才真正在窗口里显示出来。

### 修复 — 嵌套裁剪容器在滚动内容里裁错位置

`overflow:hidden` 的容器（比如页面里的 `#pool`）本身排在被滚动的页带里：
它在屏幕上的位置 = 布局位置 − 祖先已滚掉的距离。但三条路径都直接拿布局坐标当
屏幕矩形用：

| 位置 | 症状 |
|---|---|
| 遍历的视口矩形（`vpL/vpT/...`，用于剔除） | 子节点全被判成"在视口外"，整块内容一个都不画 |
| 遍历的祖先裁剪链（`pushClip(par.x, par.y, ...)`） | 裁剪矩形留在屏幕外 → 子节点被整片裁掉（**"啥也没有"就是这条**） |
| `hitTestEx` 的视口判断 | 点得中布局位置，点不中屏幕上看到的那个 |

三处统一改成 `par.x − offX[该容器]`（`offX/offY` 是"祖先滚动偏移"的预计算）。
GDI 遍历（`drawNodesGDI`）同样修了节点分支与线段分支。顺带把 `appshow` 自己的
坐标口径理清：脏矩形与命中在**屏幕坐标**，组件状态在**布局坐标**，
`ScreenY()` 一处换算（以前脏矩形全在窗口外，等于每帧整窗重绘）。

### 新增 — 文档

`docs/ui_orchestration.md` §9 补了"坐标换算"这一条；新增「叠在一起」状态的截图
`docs/img/appshow-overlap-clump.png`（真实窗口，20 块叠在池心）。

### 验收

0 warning；`pixels` 10/10、`pixels-vk` 17/17、`pixels-d3d11` 全过（全零差异）；
四套测试全绿；`check_examples` 16 个文件 0 问题；真实窗口截图里 20 块正常显示、
点击戳破、`叠在一起` 后叠成一坨。

## [0.12.9] - 2026-11-12

第 3 页改成看**半透明组件重叠**：元素回到"半透明色块"，20 块在同一个池子里飘，
重点是两块叠在一起时 alpha 合成出来的颜色，而不是"像不像泡泡"。

### 改动 — 组件从 8 层泡泡简化成 5 层半透明卡片

- `appshow-bubble.bum.html`：半透明双色渐变 + 1px 边光 + 内圈细框 + 标题/副标题 +
  底部色条（宿主每帧改色）。参数改为 `w / h / r`，尺寸不再只有直径。
- 渐变与描边都是**带 alpha 的颜色**（`rgba(...,0.30)` 一档），所以重叠区就是普通
  alpha 合成：后面的透过来 70%，不是"越叠越亮"；叠的顺序 = 文档顺序。
- 宿主侧：命中从"圆心 + 半径"改成"当前矩形"；新增「叠在一起」（全部挪到池中心，
  重叠面积最大化，方便盯合成结果）；读数/状态文案统一成"块"。
- 自检输出跟着改（块 0 的位置/速度/scale/opacity、被戳破那块的 popT）。

### 说明

- 这一版**没有引擎改动**：0.12.8 的半透明渐变条带 / `opacity` 作用渐变与边框 /
  圆角模板 LRU / 批次表下标四处修复原样保留，像素基准不变。
- 文档 `docs/ui_orchestration.md` §9 重写；截图换成 `docs/img/appshow-overlap.png`。

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`check_examples` 16 个文件 0 问题；
`BINUI_APPSHOW_AUTO=1` 自检：20 块逐帧位移、点破一块（4.53s scale 1.49 / op 0.34）→
5.10s 已在池底重生。

## [0.12.8] - 2026-11-12

appshow 的第 3 页换成「泡泡测试」：20 颗泡泡，每颗是一个 8 图层的微组件，
位置 / 呼吸 / 色相 / 戳破重生全部由宿主逐帧设值。为了让"彩色透明"真的画得出来，
这一版顺手修掉四处渲染缺陷 —— 都是这套演示逼出来的。

### 新增 — appshow 泡泡池（组件 + 宿主编排的极端例子）

- `examples/appshow/appshow-bubble.bum.html`：一颗泡泡 = 薄膜渐变 + 1px 边光 +
  两道虹彩环 + 主高光 + 反光点 + 下/侧两团**会换色**的反光，共 8 个普通 div 图层。
  参数只有 `bid / x / y / d / c1 / c2 / rim / iris1 / iris2 / glow1 / glow2`。
- 宿主（`main.cpp`）维护每颗泡泡的位置/速度/相位/色相/爆开进度：
  上浮、正弦横漂、左右碰壁反弹、飘出池顶沉底重生；点一下戳破（胀大 + 淡出）再重生；
  「吹一口气」给全体加速、「暂停漂浮」（暂停期间已戳破的泡泡仍然演完）、「重排池子」。
- 20 颗 × 7 层 × (translateX/Y + scale + opacity) + 2 次 `setStyleColor` ≈ 600 次设值/帧；
  只在本页**且页带不在滚动**时推进（滚动中宿主的脏矩形会与滚动偏移错位）。
- 命中由宿主按圆心算：引擎 `hitTest` 用的是**布局**矩形，不跟随动画位移。
- 文档：`docs/ui_orchestration.md` §9；截图 `docs/img/appshow-bubbles.png`。

### 修复 — 半透明渐变会出现横向条纹

渐变底色是"多条纯色横带"拼的，相邻条带各多 0.5px 防露缝。这个重叠对**不透明**色无害
（同色叠同色），但条带是 alpha 混合的：同一条像素行被两条各盖一次 = 混合两次，于是
每两条之间浮出一条亮纹。泡泡的薄膜（`rgba(...0.28)` → `rgba(...0.14)`）在深色底上一眼可见。

现在**半透明渐变**把切点吸到整像素、条带首尾相接不重叠；不透明的老路径逐位不变
（`pixels` 10/10 全零差异）。判据：`docs/issues/assets/roundtex-lru.html` 之外的
泡泡池截图与 `pixels` 门禁。

### 修复 — 节点透明度不作用于渐变底与边框

`drawNodes` 只把 `opacity` 折进单色底、文字、图片，渐变条带直接用编译期的
`gradFrom/gradTo`、边框直接用 `borderColor` —— 于是宿主淡入淡出一块渐变卡片或一圈
描边时，底和边纹丝不动（泡泡的出生/爆开淡出就完全失效）。现在两者都按节点透明度
缩放 alpha（不透明时逐位等价）。

### 修复 — 圆角模板 LRU 在帧内删除仍被批次引用的纹理（GL / D3D11）

圆角模板按半径缓存在 48 条 LRU 里，超了当场删纹理。但本帧**已经录进批次**的四边形
还记着那个纹理名：删掉之后新模板复用同一个 GL 名字，先前那些片就会采样到别人的内容。
表现：某几块圆角盒子/圆环被**整块涂成一个纯色圆**，且只在半径种类数跨过 48 时出现
（泡泡池一页 >48 种半径，第 11 颗泡泡开始炸）。现在淘汰只把名字挪进
`pendingTexDelete` / `pendingRoundRelease`，等 `flushAll` 画完再删。

回归用例：`docs/issues/assets/roundtex-lru.html`（60 个格子、半径 3..62），
已加进 `ANIM_CASES`，进 `pixels-vk` / `pixels-d3d11` 的跨后端零差异比对。

### 修复 — GL/D3D11 批次表存的是 `vector` 元素指针

`curBatch` 原本是 `Batch*`，`push_back` 扩容后指向已释放内存，之后 `count++`
写的是野指针。改成存下标（Vulkan 后端本来就是按值累积，无此问题）。

### 验收

0 warning；四套测试全绿；`pixels` 10/10（全零差异）；`pixels-vk` 17/17；
`pixels-d3d11` 全过（含新增 `anim/roundtex-lru` 零差异）；`vk-window` 全过；
`check_examples` 24 个文件 0 问题；`BINUI_APPSHOW_AUTO=1` 自检打印泡泡位移/缩放/
被戳破那颗的 `popT`·`scale`·`opacity`（4.52s：scale 1.47 / op 0.37 → 5.10s 已重生于池底）。

## [0.12.7] - 2026-11-12

`overflow:hidden` 的容器现在真的"不出滚动条但能编程滚动"，也不再白留 gutter。

### 修复 — 只裁剪容器不该画滚动条、不该留 gutter

`overflow:hidden` 编译成 `BINUI_SCROLLABLE`（所以 `scrollTo` 能滚、子节点会跟着偏移），
但遍历里两处只判了 SCROLLABLE，把"只裁剪"容器当成普通滚动容器处理了：

| 位置 | 症状 |
|---|---|
| 滚动条绘制 | 给 `overflow:hidden` 的容器也画了轨道 + 滑块 —— 用户**拖不动**（`isUserScrollable` 为假），却在视觉上暗示可以拖 |
| gutter 预留 | 右侧/底部白扣掉 `sbW`（默认 8px）—— 内容被裁掉一条，而滚动条根本不存在 |

两处都改成先问 `renderer.isUserScrollable(node.id)`（= SCROLLABLE 且非 clip-only）。
这也是 CSS 的语义：`overflow:hidden` 永远不显示滚动条。

appshow 的页带视口正是这种容器：`scrollTo` 驱动切页、无滚动条、滚轮不生效，
右侧内容也不再被那条假 gutter 裁掉。

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`pixels-vk` 16/16；`pixels-d3d11` 全过；
`vk-window` 全过；`check_examples` 15 个文件 0 问题；半滚动静态帧确认右侧已无滚动条痕迹。


## [0.12.6] - 2026-11-12

`appshow` 的切页改成**竖页带滚动**（浏览器里点锚点跳页那样），中间隔几页都能看到它们从眼前过去。

### 改进 — 页带 + 引擎滚动，而不是逐节点位移

- 四页在视口 `#viewport`（高 676、`overflow:hidden` → 只裁剪）里排成
  `top = i*676` 的竖带；
- 切页 = `renderer.scrollTo(viewport, 0, i*676)`：**用引擎自己的滚动状态**，
  滚动偏移由遍历累加到子节点、命中测试与裁剪也认它；
- 时长按跨页数算：`0.16 + 0.085 × 跨了几页`（上限 0.55s），ease-out；
  可打断 —— 滚到一半再点一次就从当前位置接着滚；
- 数据页 6 行的交错挪到"滚完之后"再起。

为什么不用上一版那套"给子树每个节点设同一个 `translateY`"：那只是**视觉上**对，
`translateY` 不参与命中测试，滚完之后点到的是布局位置上那一页 —— 画面和点击会错位。
换成引擎滚动后，这套问题全部消失。

顺带记两个坑（都写进文档 §3.4）：
1. `overflow:hidden` 的容器**在名称表里可能没有名字**（`findNodeByName("viewport")`
   返回 0），宿主只能靠"第 1 页的 parentId"定位它；启动时做了自检告警；
2. 自检的报告要从 `scrollState(viewport)` 读真实滚动量（之前读 `drawState().offY`，
   而这个值不含容器的滚动偏移，会一直报 0）。

自检实测（`BINUI_APPSHOW_AUTO=1`）：

```
[appshow] 页带视口 #36，内容高 2704（视口高 676）
[demo] nav-1      滚动=0    （看第1页）
[demo] push-40%   滚动=561  （看第2页）   ← 滚到一半
[demo] push-end   滚动=676  （看第2页）
[demo] nav-3      滚动=676  → slide-end 滚动=2028（看第4页）  ← 中间两页从眼前过去
[demo] nav-0      滚动=2028 → 回到第 1 页
```

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`pixels-vk` 16/16；`pixels-d3d11` 全过；
`check_examples` 15 个文件 0 问题；另用"半滚动"静态帧确认了视口裁剪与两页接缝。


## [0.12.5] - 2026-11-12

修"页面切换没有动画、只是延时一下"：`drawState()` 漏了单轴平移通道。

### 修复 — 平移的取值有两个出口，只修了一个

`drawOffsetX/Y()` 和 `drawState()` 各自维护"从哪取平移"的逻辑：

| 来源 | `drawOffsetX/Y` | `drawState()`（绘制与命中实际读的） |
|---|---|---|
| 过渡/关键帧的合并平移 `offSet` | ✅ | ✅ |
| 单轴 `offXSet/offYSet`（0.12.3 新加） | ✅ | ❌ 漏了 |
| 样式目标 `ANIM_TRANSLATE` | ✅ | ✅ |
| 样式目标 `ANIM_TRANSLATE_X/Y` | ✅ | ❌ 漏了 |

于是 `drawOffsetY()` 明明返回 676→0，绘制却拿 `st.offY = 0` —— 页面**不动**，只在新页
`setVisible(true)` / 旧页隐藏那一帧"跳"一下，看起来就是"只是延时了一下"。
修复：`drawState()` 的平移改成与 `drawOffsetX/Y` 完全同源（三路来源逐个对齐）。

顺带把自检的报告从 `drawOffsetY()` 改成 `drawState().offY` —— 和绘制、命中共用同一个
出口，这类"两套口径"以后不会再被自检放过：

```
[demo] nav-1     dy=[+0  +676 +0 +0] vis=1100
[demo] push-40%  dy=[-314 +362 +0 +0] vis=1100    ← 之前这里读 drawState 全是 0
[demo] nav-3     dy=[+0  +0   +0 +676] vis=0101
[demo] nav-0     dy=[-676 +0  +0 +0] vis=1001
```

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`pixels-vk` 16/16；`pixels-d3d11` 全过；
`check_examples` 15 个文件 0 问题。文档 §3.5 记下"同一个动画值只能有一个出口"。


## [0.12.4] - 2026-11-12

`appshow` 的页面切换换成**整块推**（推上去 / 顶下来），像胶片走一格的观感。

### 改进 — 整块推的切页

之前是"旧页滑 22px + 淡出、新页从 +26 滑入 + 淡入"，本质还是淡入淡出。现在：

- **一"格" = 视口高度 676px**：旧页 `translateY 0 → -dir*676`、新页 `dir*676 → 0`，
  **同一条时间轴、同一条缓动**（smoothstep，280ms）—— 两页首尾相接、间距恒定，
  看起来就是被手推走了一格，中间不会露底；
- **方向跟着导航顺序**：往后翻（next > prev）是"推上去"，往前翻是"顶下来"；
- 页面外面包一层 `overflow:hidden` 的视口容器 `#viewport`（边框/圆角也挪到视口上，
  页面本身是白底方块）：`translate` 不继承，整页推走时元素会飞出内容区，
  没有这层裁剪就会盖到标题栏和侧栏上，还会在视口边缘露出 4 个角缺口；
- 数据页的行交错挪到**推完之后**（280ms 起，45ms 一档），否则会和整页位移打架。

自检实测（`BINUI_APPSHOW_AUTO=1`，报告挪到动画步进之后才读到本帧的值）：

```
[demo] nav-1     dy=[+0  +676 +0 +0] vis=1100       ← 新页先在视口下方待命
[demo] push-40%  dy=[-284 +392 +0 +0] vis=1100       ← 两页一起走，间距恒等于 676
[demo] push-end  dy=[+0  +0   +0 +0] vis=0100        ← 旧页隐藏、归位
[demo] nav-3     dy=[+0  +0   +0 +676] vis=0101      ← 继续往下翻
[demo] nav-0     dy=[-676 +0  +0 +0] vis=1001        ← 反方向：新页从上方顶下来
```

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`pixels-vk` 16/16；`pixels-d3d11` 全过；
`check_examples` 16 个文件 0 问题。文档 `docs/ui_orchestration.md` §4 换成推的时序，
§3.4 记下"整页位移必须配 overflow:hidden 视口"这条。


## [0.12.3] - 2026-11-12

修两个"点了没反应"的真 bug，给页面切换换成上下切入切出。

### 修复 — `press` / `release` 命中口径不一致，`:hover` 元素永远点不动

`press()` 用 `hitTestEx(allowStyled=true)`（"声明了状态样式的普通元素"也算可点目标），
`release()` 却用严格的 `hitTest()`（只认 `BINUI_INTERACTIVE`）。于是**只写了 `:hover`
的普通 div**：按下去有视觉反馈，松手时 `release` 拿不到同一个节点 → 直接返回 -1 →
CLICK 被静默吞掉。appshow 的侧栏导航项和弹窗按钮全是这种写法，表现就是
"页面切不了、弹窗关不掉"。

- `release()` 改用与 `press()` 相同的口径（`hitTestEx(x,y,true,ACTIVE)`）；
- `hitTestEx` 的 `allowStyled` 分支放宽成"命中任意一种状态样式"（只写 `:hover` 也算），
  否则 `:hover`-only 的元素在 ACTIVE 查询下仍然落空。

### 新增 — 单轴平移通道 `ANIM_TRANSLATE_X` / `ANIM_TRANSLATE_Y`

`ANIM_TRANSLATE` 只能把 x 和 y 设成同一个值，"只上下滑 26px"做不到。现在有独立轴通道：
过渡、`setStyleFloat`、`drawOffsetX/Y`、动画极值（脏矩形）都支持；`transition: transform`
的 prop 掩码同时打开 X/Y，`translateX`/`translateY` 也各自可声明。

### 改进 — `appshow` 页面切换改为上下切入切出

- 旧页 `translateY 0→-22` + 淡出 160ms，滑完才隐藏；
- 新页先摆到 `+26px + 全透明`，延迟 60ms 后 `+26→0` 上滑 + 淡入 220ms；
- 弹窗面板也自下抬起 12px（比遮罩晚 40ms），关闭时反向；
- 修 demo 侧的三处配合问题：点击解析表补全（导航项/选中条/弹窗子树/遮罩/标题栏按钮）、
  navitem 组件补 `id="{{nid=}}"`、隐藏页的 opacity 初始化压到 0（否则首帧会闪一下满不透明）；
- 启动时做一次"节点 id 自检"，缺 id 直接告警（组件忘写 id 是这类"点了没反应"的常见来源）；
- 自检模式改成**按坐标真点**（`setHover → press → release`），覆盖导航冒泡/弹窗按钮/遮罩。

自检实测：

```
[demo] nav-1     p0=1.00 p1=0.00 dy1=+26.0            ← 点导航文字，冒泡到 nav-1
[demo] slide-mid p0=0.00 p1=1.00 dy1=+0.0 rows=0.95 0.80 0.55 0.20 0.00 0.00
[demo] dlg-in    veil=1.00
[demo] dlg-cancel → [demo] dlg-out veil=0.00          ← 点弹窗"取消"真的能关
[demo] nav-0 → [demo] final p0=1.00
```

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`pixels-vk` 16/16；`pixels-d3d11` 全过；
`check_examples` 15 个文件 0 问题。文档 `docs/ui_orchestration.md` 增补命中口径这条坑
（§3.4）与新的切页时序（§4）。


## [0.12.2] - 2026-11-12

新增 `examples/appshow`：**多页面 · 多级组件 · 编排在宿主**的完整应用壳，用来回答
"多个页面/组件一起动时怎么配合"。引擎能力没动，只把现有 API 能到的天花板演示清楚。

### 新增示例 — `examples/appshow`（4 页 / 153 节点 / 12 个组件实例）

- 壳：标题栏组件 + 侧栏（4 个导航项组件 + 选中条）+ 内容区（4 个叠放的页面容器）；
- 组件：`appshow-navitem`（带 hover 过渡）、`appshow-card`（**组件级呼吸动画**，
  只动 scale 不动 opacity）、`appshow-dialog`（标题/正文/按钮，正文走 `setText`）；
- 页面：概览（4 张指标卡）/ 数据（6 行 + 查询·重播按钮）/ 组件（badge·按钮·换色）/ 设置（4 个开关 + 危险操作）；
- 宿主编排：切页 = 旧页淡出 160ms → 新页延 60ms 淡入 220ms；数据页行按 45ms 交错；
  弹窗 = 遮罩 0ms / 面板 +40ms；提示条 1.7s 后自动淡出；开关状态迁移；
  模态期只放行弹窗与遮罩的点击；
- 被动查询：底部状态栏实时显示页号/节点数/悬停节点（id·类型·文本），
  「查询悬停节点」把命中节点的矩形/父节点/文本写进弹窗；
- 自检：`BINUI_APPSHOW_AUTO=1` 按脚本走一遍并打印每步**实际不透明度**，没有窗口抓图
  也能验编排（`page1=1.00 rows=0.94 0.78 0.52 0.16 0.00 0.00` 就是 45ms 交错在生效）。

### 文档 — `docs/ui_orchestration.md`

三层职责（组件/容器/宿主）、引擎的四条硬约束（opacity 不继承、无完成事件、
单节点单 animation、keyframes 全局命名空间）、相位差与交错的写法、`Fade::hold`
的必要性（否则交错的行会被整页淡入提前点亮）、被动查询清单，以及"缺什么才叫真正配合"
（单节点重播 / 完成事件 / 多 animation / 交错原语）。

### 验收

0 warning；四套测试全绿；`pixels` 10/10；`pixels-vk` 16/16；`pixels-d3d11` 全过；
`vk-window` / `vk-stress` 全过；`check_examples` 15 个文件 0 问题（含新增 4 个）。


## [0.12.1] - 2026-11-12

「形变」样例的圆角看着变直了：**不是 bug，是 CSS 语义**——`transform: scale(sx, sy)`
会把 `border-radius` 一起拉伸成椭圆角（浏览器同样如此）。这一版不加补偿，只把
"怎么证明它不是错位"固化成用例与诊断。

### 新增 — 仿射圆角矩阵用例（进跨后端门禁）

`docs/issues/assets/affine-corners.html`：6 种变换（`scale(1.28,0.72)` /
`scale(0.72,1.28)` / `scale(1.8,0.5)` / `rotate(20deg)` / `rotate(20deg) scale(1.3,0.7)` /
`scale(0.6)`）× 4 种半径（4 / 10 / 22 / 30），全部用 `from = to` 固定在目标状态。
已加入 `ANIM_CASES`，`pixels-vk`、`pixels-d3d11` 都逐像素比对：差异 0（最大通道差 56，
斜边 4× MSAA 采样归属差异）。

### 新增 — `BINUI_XF_DUMP` 诊断

`=1` 时打印前两次仿射圆角调用的九格 `local / uv / screen`，用来判断"角不对"是分格错位、
UV 没镜像、还是采样不是 1:1。

### 文档 — 五步自查法（`docs/vulkan_design.md` §7.12.6）

同一形状 A/B（`rotate(0)` vs `rotate(0.001deg)`）、圆的边界半径误差、像素点反变换、
分格/UV 自洽、半透明内部接缝。本次全部通过：A/B 逐像素一致；半径误差两条路径同值
（−1.75…+0.25）；反变换越界 0 像素；`opacity=0.55` 的仿射形状内部只有一种颜色。

### 验收

0 warning；`pixels` 10/10 零差异；`pixels-vk` 16/16（新增 affine-corners 用例，差异 0）；
`pixels-d3d11` 全过；四套测试全绿；`check_examples` 11/11。


## [0.12.0] - 2026-11-11

动画系统补上**旋转 + 非均匀缩放**（仿射绘制），`animshow` 重新编排并新增"压力测试"版块。
**格式号 `BINUI_VERSION` 19 → 20**（关键帧结构变长，旧的 `.bui` 需重编）。

### 新增 — `rotate()` / `scale(sx, sy)` 可动画，绘制走仿射

之前的动画只能表达 `translate / scale(等比) / opacity / border-radius / background-color`，
HTML 元素也不支持静态 `transform`。这次补齐：

- 属性位 `ANIM_SCALE_X` / `ANIM_SCALE_Y` / `ANIM_ROTATE`；`BinUIKeyFrame` 加
  `scaleX/scaleY/rotateDeg`（28 → 40 字节）。
- 写法：`scale(s)` 等比（老语义）、`scale(sx, sy)` 非等比、`rotate(deg)` 度数（顺时针正）。
- 顺带修掉编译器一个真 bug：`AnimFnNum(s, fn, arg)` 的 `arg` 是"第几次出现这个函数"，
  取 `translate(12px, 0)` 的第二个值永远拿到 0。新增 `AnimFnArgs()` 按参数表解析。
- 渲染：`DrawState` 暴露 `kx/ky/rot`，`needsAffine()` = 旋转≠0 或 kx≠ky；共享遍历用
  `[a c e; b d f]`（绕中心：缩放→旋转→平移）把局部坐标映射到屏幕，并用它算 AABB
  做剔除/裁剪。**只有底与边框走仿射**，文字/图片/路径不参与（字形要像素对齐）。
- 新原语 `IBinUIGfx::fillRoundedXf(...)`：九宫格每片按仿射变换后发射；三个后端各加
  一个 `emitQuadUV()`（任意四角 + 每角 UV）。九宫格几何与轴对齐版本同源，所以
  不旋转时逐像素一致 —— 非仿射路径**一行没动**。

### 新增 — 小数几何/旋转的跨后端回归用例

`anim-midphase.html` 扩成五条：小数等比缩放、37° 旋转圆角矩形、-21° 旋转 + 1px 仿射边框、
`scale(1.4, 0.68)` 形变、旋转 12° + 形变的圆。全部用同一个 from/to 保证**首帧就是目标状态**。

- `vk_shot` vs `gl_shot`：不同像素 **0**（最大通道差 56，斜边 4× MSAA 采样归属差异）；
- `d3d11_shot` vs `gl_shot`：不同像素 **0**；
- 已进 `pixels-vk`（阈值 0 + 通道阈值 128）与 `pixels-d3d11`。

### 改进 — `animshow` 重新编排（1000×680 → 1000×900）

- 缩放类动画集中到 640 宽的「**缩放 · 形变**」版块：5 个 `scalePulse`（五种缓动）+ 脉冲
  + 呼吸（opacity×scale）+ 形变 `scale(1.28, 0.72)` + 圆角形变；「缓动曲线」版块改用
  `translateY` 演示，不再重复缩放。
- 新增 640 宽的「**压力测试**」版块：
  1. 环绕轴心 + 自转 360° + 变色（同一条 keyframes 里 translate/rotate/background-color）；
  2. 锯齿波（`translateX` linear+alternate）× 非等比形变 × 变色；
  3. 位移 × 旋转 × 形变 × 圆角 × 透明度 五属性同帧。

### 验收

0 warning；四套测试全绿；`pixels` 10/10 零差异；`pixels-vk` 15/15（动画用例不同像素 0）；
`pixels-d3d11` 全过（含动画用例）；`vk-window` / `vk-stress` 全过；`demos-vk` /
`demos-d3d11` 各 8 个；`check_examples` 11/11（animshow 23949 字节）。详见
`docs/vulkan_design.md` §7.12。


## [0.11.10] - 2026-11-11

只剩 Vulkan 的"放大/缩小抖、平移不抖"：**圆角模板纹理在 VK 上是 NEAREST 采样**。
静态画面逐像素不变。

### 修复 — VK 的圆角模板改用线性采样（与 GL / D3D11 一致）

三个后端都用"圆角模板位图 + 九宫格"画圆角矩形，但模板纹理的过滤器不一致：

| 后端 | 模板纹理 |
|---|---|
| GL | LINEAR（`CreateTexture2D(..., true)`） |
| D3D11 | 线性采样器（`curLinear = 1`） |
| **Vulkan** | **NEAREST**（`createTexImage(rt.tex, t, t, false)`） |

静态帧看不出：角片边长 `rf == r`、模板 `t = 2r+4`，九宫格按 1 texel : 1 像素映射，
NEAREST 与 LINEAR 逐像素相同（三套门禁因此一直没抓到）。**缩放动画里 `rf` 是小数**，
九宫格把模板非整数拉伸，NEAREST 把弧线量化成 texel 台阶、每帧变一次 → 圆角在抖；
平移不改变 `rf`，所以平移稳定。

改成 `createTexImage(rt.tex, t, t, true)`。

### 新增回归用例 — 小数几何

`docs/issues/assets/anim-midphase.html`：`from { transform: scale(0.77) }`，渲染第一帧
就落在**小数缩放**上（不用等动画跑起来）：

- 改之前：`vk_shot` vs `gl_shot` **差异区域 252×156**，容差 0 失败；
- 改之后：**不同像素 0、最大通道差 1**，容差 0 通过。

已进 `pixels-vk`（阈值 0）和 `pixels-d3d11`（3%）的清单；其余静态用例读数一位没变
（`features` 最大通道差 43、`svgdemo` 56 …）。

### 验收

0 warning；四套测试全绿；`pixels` 10/10 零差异；`pixels-vk` 15/15（新增 anim 用例，
不同像素 0）；`pixels-d3d11` 全过（新增 anim 用例）；`vk-window` / `vk-stress` 全过；
`demos-vk` / `demos-d3d11` 各 8 个；`check_examples` 11/11。详见
`docs/vulkan_design.md` §7.11。


## [0.11.9] - 2026-11-11

Vulkan 独占的动画抖动：**present 不等"这一帧真的上屏"**。无像素变化。

### 修复 — VK_KHR_present_wait（GL/D3D11 已丝滑，只剩 VK 抖）

§7.9 之后 GL / D3D11 都稳了，只有 `animshow_vk` 还抖。给 `BINUI_VK_PERF` 加**调用级**
拆分后才看清：

```
[vk][perf] 1200 帧: 平均 6.102 ms/帧 = 录制 0.071 + 等 GPU 5.933 + 呈现 0.098
          （顶点 2931/帧；其中 acquire 0.005 + present 调用 0.024）
```

`vkQueuePresentKHR` 只花 0.024ms —— FIFO 下它**从不阻塞**，交换链图只在合成分解那一刻
才被放掉（于是 8ms 落在 `waitSlot` 等 GPU 侧的 blit 上）。结果是 CPU 提前把下一帧排进
队列，合成器某些周期拿到两帧、某些周期一帧都没有：帧率照样 120，肉眼是抖。

- present 时链一个 `VkPresentIdKHR`（帧号），提交后
  `vkWaitForPresentKHR(swapchain, 帧号, 100ms)` 阻塞到该帧**被显示**为止 ——
  与 `SwapBuffers(vsync)` 语义对齐，"一帧一次合成"成为硬约束。
- 可选扩展：设备创建时探测 `VK_KHR_present_id` + `VK_KHR_present_wait` 并链上对应 feature；
  任一环节缺失就不启用，退回 v0.11.8 的 "present + DwmFlush"。
  启动日志会打 `[vk] 交换链 N 图，呈现模式 FIFO(vsync)，present_wait 可用（等真上屏）`。
- 超时 100ms（最小化/被遮挡时合成器可能长时间不合成）；交换链图数回到 `minImageCount+1`。
- 外壳侧无需改动：present 现在自己阻塞 ~8.2ms，§7.9 的 `lastPresentMs_` 判定会跳过
  `DwmFlush`，不会双重等待。

| 后端 | fps | 掉拍 | 一帧一次合成 |
|---|---|---|---|
| Vulkan | 120.1 | 0/600 | 是（硬约束） |
| GL | 120.0 | 0/600 | 是（DwmFlush） |
| D3D11 | 120.0 | 0/600 | 是（DwmFlush） |

### 验收

0 warning；四套测试全绿；`pixels` 10/10 零差异；`pixels-vk` 14/14（不同像素 0）；
`pixels-d3d11` 全过；`vk-window` / `vk-stress` 全过；`demos-vk` / `demos-d3d11` 各 8 个；
`check_examples` 11/11。详见 `docs/vulkan_design.md` §7.10。


## [0.11.8] - 2026-11-11

呈现相位修正：同一套共享遍历下 **GL 看着稳、Vulkan/D3D11 看着抖**。无像素变化
（三套门禁原值通过）。

### 修复 — 帧点唤醒退回 15.6ms + 合成节拍与显示器相位错开

`BINUI_PERF` 新增 1ms 一档的**帧间隔直方图**（平均值看不出"两张快一张慢"的拍子），
一测就出来了：

| 后端 | 直方图（600 帧） | fps |
|---|---|---|
| GL | **15.6ms**（14ms:140 15ms:380 16ms:62） | 63.7 |
| Vulkan | 7ms:163 8ms:353 9ms:84 | 120.2 |
| D3D11 | 15.6ms | 64.9 |

- **无脏区分支**按 `timeoutMs`（1ms）粗睡 → OS 合并到 15.6ms 计时器节拍。上一帧画完
  `dirty_` 就清了，"有没有下一帧"要等下一次 `renderFrame` 里的 `update_` 才知道，
  于是每次都在帧点之前先睡掉 15.6ms（GL/D3D11 掉回 64fps）。
  改成也睡到 `nextFrameTime_`（高精度定时器，输入仍可唤醒）。
- **present 之后 `DwmFlush()`**（仅 `pacedByPresent()` 为真、即真走交换链时）：与桌面
  合成器的合成节拍对齐，一帧一次合成。不 Flush 的话合成节拍器按名义 8.33±1ms 出帧，
  落在刷新边界另一侧的那些帧会被多留一个周期 —— 平均 120fps 但肉眼是抖。

| 后端 | 改后 fps | 直方图 |
|---|---|---|
| GL | **120.0** | 8ms:428/600 |
| Vulkan | **120.0** | **8ms:487/600**（9ms 只剩 32） |
| D3D11 | **120.0** | **8ms:560/600** |

`掉拍` 0~4/600；静态页空闲 CPU 与改动前持平（0.81s → 0.86s / 8s）。

### 修复 — 圆角半径改成浮点几何（缩放/圆形动画"一胀一缩"的第二个来源）

上一版（0.11.7）只把 x/y/w/h 变成浮点，**半径仍然是整数**（`lround(r * scale)`）。
圆形是 `radius = 短边/2`：半径取整后 `2r` 会周期性比 `w` 大/小 1px，九宫格中间那几块
就在 0/1/2px 之间跳，圆看起来一胀一缩 —— 缩放类动画里最扎眼的一处。

现在半径的**几何**用浮点（跟着 w/h 连续变化），只有"角片模板"按最近整数取
（模板是位图，只能按整数建），三个后端的 `fillRounded` / `strokeRounded` 统一：

```
id=65（pulse 圆）  w: 49.7, 48.5, 48, 47.9, 47.8, 47.5, 47.4, 47.2
                  r: 24.8, 24.2, 24, 23.9, 23.9, 23.7, 23.7, 23.6
              2r-w: -0.1, -0.1,  0, -0.1,    0, -0.1,    0,    0
```

静态页面传进来的半径本来就是整数，`lround(rf)` 与原值相同、九宫格几何一致 →
三套像素门禁逐像素未变。

### 验收

0 warning；四套测试全绿；`pixels` 10/10 零差异；`pixels-vk` 14/14（不同像素 0）；
`pixels-d3d11` 全过；`vk-window` / `vk-stress` 全过；`demos-vk` / `demos-d3d11` 各 8 个；
`check_examples` 11/11。详见 `docs/vulkan_design.md` §7.9。


## [0.11.7] - 2026-11-11

动画几何改**亚像素**：修掉"帧率已经 120fps，动画仍是阶梯式移动"。无 API 改动；
静态画面逐像素不变（三套像素门禁原值通过）。

### 修复 — 遍历把动画平移/缩放截断成整像素

`binui_draw_nodes.h` 里节点几何是三段整型截断：`x = (int)node.x - (int)offX`、
`nw = (int)(nw*scale)`、`x += (int)dx`。`animshow` 的 `bob` 是 16px/1.6s = 10px/s，
120fps 下每帧只该走 **0.083px** —— 截断后就是"停 11 帧、跳 1 像素"；`slideX`
（200px/1.4s = 143px/s）也有 1~2px 的台阶。精度在进绘制之前就被扔掉了。

- **只有正在动画的节点走浮点几何**（`dx/dy/scale` 非默认值时）：绕中心缩放 + 平移
  全程 float，直接交给后端已有的浮点原语；静态节点保持原来的整型截断，所以
  `pixels` / `pixels-vk` / `pixels-d3d11` 逐像素未变。
- **圆角随缩放**：`effRadius = lround(effRadius * scale)`（CSS 的 `transform: scale`
  连圆角一起放大，否则 40px 盒子放大 1.45× 后圆角看起来变方）。
- **自身裁剪矩形外扩到像素格**：`floor(x)/ceil(x+w)`。GL/D3D11 的 `pushClip` 是
  `(int)` 向内截断，x=549.2 宽 22 的盒子右边界 571.2 会被截成 571、吃掉 0.2px，
  而这个小数每帧都在变 → 又是台阶。
- `BINUI_DEBUG_CLIP` 的 `own=(...)` 改成 `%.1f`，以便观察亚像素坐标。

证据（`BINUI_DEBUG_CLIP=1`，同一节点相邻帧）：

```
node=44 own=(822.0,148.7 ...) dy=-1.3
node=44 own=(822.0,148.1 ...) dy=-1.9
node=44 own=(822.0,147.4 ...) dy=-2.6
```

文本仍按整像素对齐（字形是图集采样，亚像素只会糊），移动容器里的文字可能比容器
晚半像素对齐 —— UI 库常规取舍。已知未修两条记在 `docs/vulkan_design.md` §7.8.5：
父节点 transform 不传递给子树、三后端 scissor 取整不一致。

### 验收

0 warning；四套测试全绿；`pixels` 10/10 零差异；`pixels-vk` 14/14（不同像素 0）；
`pixels-d3d11` 全过；`vk-window` / `vk-stress` 全过；`demos-vk` / `demos-d3d11` 各 8 个；
`check_examples` 11/11。详见 `docs/vulkan_design.md` §7.8。


## [0.11.6] - 2026-11-11

帧节拍修正：动画在 120Hz 屏上"感觉只有 60fps / 有微抖"。无 API 破坏（只新增一个
带默认实现的后端查询），无像素变化。

### 修复 — 拿毫秒级 `MsgWait` 当帧节拍器（GL 64fps / D3D11 64fps / VK 105fps 的真因）

`binui_app.h` 的帧节流是"算到下一帧差几毫秒就睡那么久"，而
`MsgWaitForMultipleObjectsEx` 的毫秒超时按**系统计时器精度（默认 15.6ms）**兑现：
想睡 8.33ms 实际睡 15.6ms，一帧跨两个刷新 → 120Hz 屏上只有 64fps。再叠上
`nextFrameTime_ = now + budget`（把每帧耗时也算进周期，累积漂移）和一个 `+1ms` 保险。

- **固定网格**：`nextFrameTime_ += frameBudget_`，落后超过 2 个预算才重新对齐。
- **高精度等待** `waitForFrame()`：`CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`
  + `MsgWaitForMultipleObjectsEx` 一起等消息（精度 ~0.5ms，不改全局电源策略）；
  拖拽 resize 循环同用。
- **空闲分支保持毫秒粗等待**：静态页 `pump(1)` 的空闲唤醒若改高精度会从 64 次/秒变成
  1000 次/秒（实测空闲 CPU 1% → 10%），所以只给"帧点"用精确等待。
- 新增 `IBinUIGfx::presentPacesFrames()`（默认 false，GL/VK/D3D11 各自 override）：
  说明该后端在 vsync 交换链上可能阻塞到垂直同步；外壳据此打一行启动诊断，
  两种情况下都保留 `nextFrameTime_` 闸门。
- `BINUI_PERF` 增加帧间隔统计：`间隔 max=`、`掉拍=`（>1.5 预算的次数），单次 >100ms
  的间隔单独打一行。

| 后端（`animshow`，120Hz，16s 取稳态 600 帧窗口） | 改前 | 改后 |
|---|---|---|
| GL | 62.9 fps | **120.1 fps**（呈现 avg 0.90 → 6.77ms，确实在等 vsync），掉拍 0~1/600 |
| D3D11 | 64.0 fps | **120.1 fps**，掉拍 0~1/600 |
| Vulkan | 105~106 fps | **119~120 fps**，掉拍 1~3/600 |

帧间隔：GL 15.6ms/帧 → `max=9.5~11.4ms`（预算 8.33ms）；VK `max=27ms` → `9.3~17.6ms`。

### 验收

0 warning；四套测试全绿；`pixels` 10/10 零差异；`pixels-vk` 14/14（不同像素 0）；
`pixels-d3d11` 全过；`vk-window` / `vk-stress` 全过；`demos-vk` / `demos-d3d11` 各 8 个；
`check_examples` 11/11。静态页空闲 CPU 与 0.11.5 持平（0.81s → 0.77s / 8s）。
详见 `docs/vulkan_design.md` §7.7。


## [0.11.5] - 2026-11-11

CPU 侧绘制（第二轮）：**每节点状态查询合并成一次** + **顶点写入路径**。无 API / 布局改动，
无像素变化，三个后端共享同一份遍历与同一套写顶点方式。

### 性能 — 遍历：把 8 个查询合成 1 个

`features` 一帧 80 个节点里，每个节点原先要散着调 `drawColor / drawOpacity / drawOffsetX /
drawOffsetY / drawScale / drawRadius / paintOf / isDisabled`：每个内部都查
`animResult_ + animState_ + nodeIndexById_`，**没有动画**的节点也要 17 次哈希探测。

- `BinUIRenderer::drawState(id)`：一次取齐颜色 / 透明度 / 平移 / 缩放 / 圆角 / 禁用 /
  扩展绘制段指针，语义与逐个查询逐条对齐（动画结果优先、样式目标兜底）；两张动画表
  **空就跳过**（静态页面正是如此）。
- `nodeIndexOf(id)`：稠密 id 走 `nodes_[id].id == id` 数组直读，哈希表只兜底；
  `clippingAncestors` 的祖先链同步改用它。
- 共享遍历里每帧重建的 `unordered_map byId`（80 节点 ≈ 5 µs + 一次 malloc）改成
  "直读 + 懒建表"：索引段 **0.005 → 0.001 ms/帧**。
- `PaintLerpArgb` 的 `std::lround` 换成 `(v + 0.5f)` 截断（通道值恒 ≥ 0，逐位等价）。

### 性能 — 三个后端的顶点写入：预分配 + 指针偏移

GL / Vulkan / D3D11 原来都是 `verts.insert(verts.end(), v, v+6)` 或逐顶点 `push_back`，
每条图元都要做容量检查 + `size` 递增。改成缓冲一次 `resize` 到上限、每帧只复位计数、
按 `data() + vcount` 写；GL 的 `beginFrame` 也不再 `clear()`（否则每帧把 2 MB
析构再扩容一遍）。

**试过并否掉的方案**：顶点直接写进映射过的槽缓冲以省掉 `memcpy` —— 映射内存是**写合并**
的，逐顶点 20 字节赋值吃非对齐 PCIe 写惩罚，`录制` 从 0.070 ms 涨到 **1.00 ms**（15 倍）：
169 KB 走了 0.93 ms（≈180 MB/s），而整块 `memcpy` 只要 ~10 µs。代码里留了注释，
别再走一遍。

### 实测（新旧二进制交替跑 15 轮取中位，`vk_window --frames 300`）

| 用例 | 遍历耗时 | 录制耗时（CPU 侧全集） |
|---|---|---|
| `features` 900×932（80 节点 / 8472 顶点） | 0.044 → **0.039 ms/帧**（-11.4%） | 0.044 → **0.040**（-9.1%） |
| `svgdemo` 960×560（64 节点 / 12654 顶点） | 0.041 → **0.038 ms/帧**（-7.3%） | 0.042 → **0.038**（-9.5%） |

同一协议的端到端 fps：`features` 2618.4 → 2634.3（+0.6%）、`svgdemo` 3545.3 → 3609.0
（+1.8%）。读数能解释为什么只有这点：`BINUI_VK_PERF` 一帧 = 录制 0.054 + **等 GPU 0.376**
+ 呈现 0.115，**CPU 侧已经不是瓶颈**（GPU 4× MSAA 累积 + resolve 才是）。

`BINUI_VK_PERF` 的读数行尾新增 `（顶点 N/帧）`，从此能直接看"这一帧提交了多少几何"。

### 验收

0 warning；四套测试全绿；`make pixels` 10/10 零差异；`make pixels-vk` 14/14（不同像素 0）；
`make pixels-d3d11` 全过（残差与 0.11.2 记录值逐项一致：`svgaa` 0.249%、`svgstroke` 0.226%、
`features` 132 像素，均为 4× MSAA 采样位置差异）；`vk-window` / `vk-stress` 全过；
`demos-vk` / `demos-d3d11` 各 8 个；`check_examples` 11/11。详见
`docs/vulkan_design.md` §7.6.6~§7.6.10。


## [0.11.4] - 2026-11-11

CPU 侧绘制优化：**字形整形结果缓存**。无 API / 布局改动，无像素变化。

### 性能 — `GlyphAtlas` 里按 (文本, 字号, 字重, 斜体, 字体面) 缓存整形结果

先量再改：新增 `BINUI_DRAW_PROF=1`（共享遍历分段计时，每 60 帧一行）。读数显示
`features` 一帧 0.087 ms 的遍历里 **0.058 ms（67%）是文本**（43 个 LABEL ≈1.35 µs/个）——
每次 `drawText` 都要 UTF-8 解码 + 逐簇查字体面 + 两级哈希查图集格子，而静态页面上这些
结果每帧完全不变。

- 新增 `GlyphRun` 缓存（1024 条 LRU）：命中时只按预存的四边形表发射；
  `textWidth` 命中时直接返回缓存的前进宽（连解码与度量都省）。
- 条目带**图集代数**：纹理新建/销毁、`clear()`、换字体面都会 `++atlasGen_` 并清空缓存，
  句柄/UV 不会指向已销毁的图集页。彩色字形在缓存里记 `white=true`，灰字形按调用方颜色染色
  （颜色不进 key，同一个 run 换色可复用）。

| 用例 | 遍历耗时 | 端到端 fps |
|---|---|---|
| `features` 900×932（43 个标签） | 0.087 → **0.043 ms/帧** | 2657.6 → **2997.6**（+12.8%） |
| `singlewindow` 1280×720 | 0.030 → **0.017 ms/帧** | 3642.6 → **4296.3**（+17.9%） |
| `svgdemo` 960×560 | ≈0.059 → **0.040 ms/帧** | 3857.6 → 3950.2 |

三套像素门禁保持原值（缓存只改"怎么算"，不改"画成什么"）。

### 修复 — 工具 exe 的 `-MMD` 依赖从未生效

工具 recipe 里是"编译 + 链接"两条命令，生成的 `build-mingw/vk_window.d` 没有目标依赖它，
于是**改了共享头但工具不重编**、链的是旧 `.o`（本轮在 `BINUI_DRAW_PROF` 上踩到）。
`vk_shot / vk_window / vk_stress / d3d11_shot` 的 `.o` 已拆成独立规则。

### 验收

0 warning；四套测试全绿；`make pixels` 10/10 零差异；`make pixels-vk` 14/14；
`make pixels-d3d11` 全过；`vk-window` / `vk-stress` 全过；`demos-vk` / `demos-d3d11` 各 8 个；
`check_examples` 11/11。详见 `docs/vulkan_design.md` §7.6。


## [0.11.3] - 2026-11-11

三件"本机条件不具备"的事先留接口，其中两件连**模拟开关**都做了（能模拟就能测）。
详见 `docs/vulkan_design.md` §7.5。

### 新增 — 跨显示器 DPI 接口 + 模拟开关

- 窗口外壳处理 `WM_DPICHANGED`：按系统建议的矩形摆窗口 → 走与 `WM_SIZE` 相同的
  "挂起 → 帧内应用"路径 → 记录新 DPI → 回调 `setDpiChangedFn(dpi)`；新增 `dpi()` 读数。
- `BINUI_FORCE_DPI=120|144|168` 在单显示器机器上模拟换屏后的 DPI（启动打印一行确认）。
- 库不改进程 DPI 策略（那是应用的 `SetProcessDpiAwarenessContext`），所以"真的拖到另一块
  屏上"仍需双屏机器验收。

### 新增 — 远程会话（RDP / 虚拟显示器）判定

- `binui::RemoteSession()`：`GetSystemMetrics(SM_REMOTESESSION)`，`BINUI_FORCE_REMOTE=1` 可模拟。
- 远程会话一律走"读回 + GDI 贴屏"，绕开直翻转（原来只有"客户区铺满显示器"这一个触发条件）。
  三个后端与外壳共用这一处判定。

实测（`BINUI_FORCE_REMOTE=1`）：Vulkan `vk_window` 600×260 从 `present=交换链` 631.7 fps
切到 `present=GDI 读回` 184.4 fps，**像素 0 差异（阈值 0）**；D3D11 `d3d11_shot` 同样 0 差异。
代价是吞吐，换的是"远程环境不停帧"。

### 变更 — 为运行期 `BINUI_GPU` 清掉前置障碍

- `BinUIAppT<Gfx>::DrawFn` 与 `<canvas>` 回调改成吃 `IBinUIGfx&`，8 个示例的 `setDrawFn` /
  `canvasCb` 一并改成接口类型 —— 调用方不再依赖具体后端类型；`drawUi()` 不再需要把接口
  往下转型。`binui_ui.h` 留了 `UiBackendName()` 和"接哪里"的注释，还缺的只有
  `IBinUIApp` 抽象 + 工厂（约半天，纯加法）。

### 验收

0 warning（三个后端 + 全部 demo）；四套测试全绿；`make pixels` 10/10 零差异；
`make pixels-vk` 14/14；`make pixels-d3d11` 全过；`demos-vk` / `demos-d3d11` 各 8 个；
`check_examples` 11/11。


## [0.11.2] - 2026-11-11

D3D11 最后一块重复实现并入共享代码：**文本/图片改用 `binui_glyph_atlas.h` /
`binui_win_image.h`**。无 API / 布局改动，无像素变化。

### 重构 — 三端同一份文本实现

- 实现 `IGlyphTexHost`（`createTex` / `updateTex` / `destroyTex`）：句柄用 SRV 指针，纹理通过
  `glyphTexOf` 反查；新建纹理必须给零缓冲（图集里的空格子会被当真字形画）。
- `drawText` / `drawTextWrapped` / `textWidth` / `fontMetrics` 变成 `GlyphAtlas` 的一行调用；
  emoji（COLR/CPAL 簇合成）、逐字形回退、hinting/gamma/像素对齐这些细节从后端消失。
- 删掉 `Atlas` / `ColorAtlas` / `Glyph` / `ColorGlyph` / `IsEmojiCp` / `EmojiClusterLen` /
  `EmojiClusterKey` / `getAtlas` / `rasterGlyph` / `glyphFor` / `getColorAtlas` /
  `rasterColorCluster` / `colorGlyphFor` / `faceIdxForCp` / `emojiFaceIdx`，以及本地
  `DecodeImagePixels` 与 `gWrappedLineStep`。
- 图片解码改调 `binui::DecodeImagePixels`（与 GL / Vulkan 同一份）。

**尺寸**：`binui_d3d11.cpp` 2804（0.10.x）→ 1693（0.11.0）→ **1216 行**，与 GL 后端（1186 行）
同量级。三个后端现在是同一个形状：薄后端 + 共享遍历 / 图集 / 图片解码 / 窗口外壳。

### 像素

`make pixels-d3d11` 的 14 个用例数字与 0.11.1 **完全一致**（`probe` / `radius-matrix` /
`texturedemo` / `display-none2` / `svggrad` / `svgfinish` 全 0；`features` 132、`fontshow` 8、
`svgdemo` 104、`circle` 1200、`svgaa` 272、`svgstroke` 247）—— 说明两套图集光栅化出的位图
本来就相同，这次改的是结构不是像素。

### 验收

0 warning；四套测试全绿；`make pixels` 10/10 零差异；`make pixels-vk` 14/14；
`make pixels-d3d11` 全过（含两条阈值 0）；`demos-d3d11` / `demos-vk` 各 8 个；`check_examples` 11/11。


## [0.11.1] - 2026-11-11

D3D11 像素对齐补丁：**圆角模板换成共享实现** + **离屏改 4× MSAA**。无 API / 布局改动。

### 修复 — 两处"D3D11 还没换成共享实现"

- `roundFor` 改用 `binui_paint.h` 的 `RoundTemplateSize(r)`，覆盖率公式也改成"形状铺满
  `[0,t]`、圆弧包围盒正好 `[0,r]`"（旧代码 `t = max(8, 4r)` 且形状在内缩 1px 的盒子里铺，
  九宫格角片与弧长对不上）。顺带补了 GL 后端早有的 **48 张 LRU 上限**（半径种类多时会一路
  涨显存）。
- 离屏帧缓冲从单采样改成 **4× MSAA 绘制目标 + `ResolveSubresource` 到单采样纹理**，
  与 GL 的 `msFbo → offTex`、Vulkan 的 `msaa → resolve` 同一条路线；脏矩形累积仍留在
  MSAA 目标上（阈值 0 的"脏矩形 vs 全量"回归验证没被破坏）。驱动不支持 4× 时自动退回单采样。

### 与 GL 的逐像素差异变化（`make pixels-d3d11`，阈值 0 的两条照旧全 0）

| 用例 | 0.11.0 | 0.11.1 |
|---|---|---|
| `display-none2` | 168 | **0** |
| `svggrad` | 8 | **0** |
| `svgfinish` | 3 | **0** |
| `svgjoin` | 57 | 4 |
| `svgpath` | 284 | 25 |
| `features` | 2492 | **132** |
| `fontshow` | 336 | 8 |
| `svgdemo` | 116 | 104 |
| `circle`（曲线边缘） | 1200 (2.041%) | 1200 (2.041%) |
| `probe` / `radius-matrix` / `texturedemo` | 0 | 0 |

剩下的是曲线边缘一圈的覆盖率量化差（两套 API 的 4× MSAA 采样位置不同），与 VK↔GL 在斜边上
"一个采样点"的差同理，因此判据保持 3%。详见 `docs/vulkan_design.md` §7.4.4。

### 验收

0 warning；四套测试全绿；`make pixels` 10/10 零差异；`make pixels-vk` 14/14；
`make pixels-d3d11` 全过（含两条阈值 0）；`make demos-d3d11` 8 个；`check_examples` 11/11。


## [0.11.0] - 2026-11-11

D3D11 后端**并入共享代码并进入构建**。新后端参与构建 = MINOR；无二进制布局改动
（`BINUI_VERSION` 仍 19）。

### 新增 — D3D11 成为第三个共享后端

`src/backends/binui_d3d11.cpp` 原来是个**不在 `Makefile` 里**的独立实现（2804 行），
自带一份节点树遍历、字形图集、图片解码、光标定位和无边框窗口外壳。现在：

- `class BinUID3D : public IBinUIGfx`（之前只是签名碰巧相同），全部原语 `override`；
- 节点树遍历改走共享的 `drawNodesBinUI`（`binui_draw_nodes.h`），删掉 `drawNodesD3D`
  与视频控制条那份拷贝；
- 窗口外壳改走共享模板 `BinUIAppT<Gfx>`（`binui_app.h`），删掉整块 `BinUIAppD3D`；
- 光标定位 / 光标闪烁 / 标题栏判定用 `binui_app.h` 的共享实现；
- 新增 `presentViaGdi()` + 读回贴屏路径，与 GL/Vulkan 同一套触发条件（透明窗 /
  客户区铺满显示器 / 拖拽 resize 期）；
- `binui_ui.h` 加 `BINUI_GPU_D3D11` 分支，`drawUi()` 三个后端统一调 `drawNodesBinUI`；
- `Makefile`：`binui_d3d11.o` + `D3D11_LIBS` + **`make demos-d3d11`** → 8 个
  `examples/*_d3d11.exe`。

净效果：`binui_d3d11.cpp` 2804 → **1615 行**，`binui_d3d11.h` 192 → 106 行；
D3D11 白拿 v13~v19 的全部特性（边框 / 渐变 / 文字装饰 / SVG 路径 / 脏矩形 / 帧节流 / IME）。

### 验证：新增 `d3d11_shot` + `make pixels-d3d11`

D3D11 的交换链必须绑窗口，所以 `tools/d3d11_shot.cpp` 开**隐藏窗口**跑 N 帧，再从离屏纹理
读回像素（新增 `BinUID3D::readbackPixels`）。之前用屏幕截屏（`PrintWindow`）抓到的是纯白图
—— flip-model 交换链的内容不在 GDI 表面上。有了它，`make pixels-d3d11` 才能自动化跑：

| 用例 | 不同像素 |
|---|---|
| `probe` / `radius-matrix` / `texturedemo` | **0** |
| `svgdemo` / `fontshow` / `features` | 116 (0.022%) / 336 (0.049%) / 2492 (0.297%) |
| `circle`（最大） | 1200 (2.041%) |
| 脏矩形重绘 vs 全量重绘（阈值 0） | **0** |
| 强制读回 + GDI 贴屏 vs 交换链（阈值 0） | **0** |

差异根因是两处"还没换成共享实现"：D3D11 自己的**圆角模板**（`max(8,4r)` + 自己的覆盖率算法
vs `binui_paint.h` 的 `RoundTemplateSize`）和**文本光栅化**（`binui_font_renderer` 自带图集）。
所以 `make pixels-d3d11` 的判据先放宽到 3%（`PIXEL_D3D_TOL`），P7b 之后收紧到 1.5% → 0。

### 验收

0 warning（三个后端 + 8 个 d3d11 demo + d3d11_shot）；四套测试全绿；`make pixels` 10/10 零差异；
`make pixels-vk` 14/14；`make pixels-d3d11` 14 用例 + 两条阈值 0 全过；`make demos-d3d11` 8 个；
`make demos-vk` 8 个；`check_examples` 11/11。


## [0.10.2] - 2026-11-11

性能修复：**热路径里的 `std::getenv`**。无 API / 二进制布局改动（`BINUI_VERSION` 仍 19）。

### 修复 — 每帧几千次的 getenv

- `src/backends/binui_vk.cpp` 的 `fillRect` 里有一句 `if (std::getenv("BINUI_VK_TRACE"))` 只为打印
  前 4 次调用 —— 但它在**每个图元**上执行。`svgdemo` 一帧 2109 个条带（=2109 次 getenv），
  UCRT 的 getenv 每次遍历环境表约 0.5 µs，合计 **1.04 ms/帧**。改成函数内 `static const bool`。
- `src/backends/binui_draw_nodes.h` 的 `drawNodesBinUI` 里，`BINUI_DEBUG_CLIP` 在**每个节点**上查
  两次。提到函数开头查一次（这一处在共享遍历里，**GL 后端同等受益**）。
- 结果：`svgdemo` 录制耗时 1.183 → **0.059 ms**，窗口吞吐 682 → **3857.6 fps**。

### 定位过程（可复核）

`BINUI_VK_PERF` 显示 `svgdemo` 每帧 1.447 ms 里 1.244 ms 在 CPU 录制侧。逐项排除：关描边 1.183→1.174
（不是描边）、整条路径绘制短路 1.183→0.120（就是路径绘制）、把条带缓存传 `nullptr` 1.183→1.196
（**不是细分**、是"重放"）、把 getenv 提成 static 1.183→0.138 → **真凶确认**。详见
`docs/vulkan_design.md` §7.3。

### 实测（同机同工具，7 次取中位，300 帧，vsync=off）

| 用例 | 0.10.1 | 0.10.2 | 对 0.10.0 |
|---|---|---|---|
| `features` 900×932 | 1722.3 fps | **2657.6 fps** | +93.2% |
| `svgdemo` 960×560 | 682.2 fps | **3857.6 fps** | +523.8%（6.2×） |
| `probe` 600×260 | 7277.4 fps | **9087.4 fps** | +92.9% |
| `singlewindow` 1280×720 | 3642.6 fps | **4387.7 fps** | +68.5% |

### 验收

0 warning；四套测试全绿；`make pixels` 10/10 零差异；`make pixels-vk` 14/14（不同像素 0）；
`make vk-window` 全过；`make vk-stress` 两轮 8 阶段 0 失败；`check_examples` 11/11。


## [0.10.1] - 2026-11-11

Vulkan 后端性能补丁：**双槽流水线**（帧录制与 GPU 执行重叠）。无 API / 二进制布局改动
（`BINUI_VERSION` 仍 19），旧 `.bui`/`.bum` 不用重编。

### 修复 / 性能 — Vulkan：每帧不再同步等自己的 fence

- 改动前每帧都是 `submit → vkWaitForFences(本帧) → present`，把 submit→fence 的往返
  （75~150 µs）整个压在关键路径上，且下一帧的 CPU 录制无法与 GPU 执行重叠。
- 现在 `Impl` 里是 **2 个槽**（命令缓冲 / fence / 两个信号量 / 顶点缓冲 / staging 待释放表各一套）：
  等待点从 `beginFrame` 挪到"提交前等上一帧"，`beginFrame → present` 之间的 CPU 工作
  （遍历、排版、SVG 路径细分、填顶点）发生在上一帧还在 GPU 上跑的时候。
- 只做 2 槽的原因：渲染目标（4x MSAA 累积图 + resolve）帧间共享，`loadOp=LOAD` 要求 GPU 上
  永远只有一帧在跑；再多槽位不增加并行度，只多占内存。
- 顶点缓冲改为每槽一块（写第 N+1 帧顶点时第 N 帧可能还在读）。
- 修一个随之暴露的坑：`vkCreateSemaphore`/`vkDestroySemaphore` 属于 WSI 那组函数
  （`loadSwapchainFns` 之后才非空），离屏模式下建信号量 = 调空指针 → `vk_shot` SIGSEGV。
  改为信号量与交换链同生共死。
- `tools/vk_window`：`--out` 不给就不开每帧交换链读回（读回是一次全屏拷贝，会掩盖真实吞吐）；
  验收用例都带 `--out`，判据不变。
- 新环境变量 `BINUI_VK_PERF=1`：每 60 帧打印"录制 / 等 GPU / 呈现"三段耗时。

### 实测（同机同工具，7 次取中位，300 帧，vsync=off）

| 用例 | 0.10.0 | 0.10.1 | 提升 |
|---|---|---|---|
| `features` 900×932 | 1375.9 fps | 1722.3 fps | +25.2% |
| `svgdemo` 960×560 | 618.4 fps | 682.2 fps | +10.3% |
| `probe` 600×260 | 4711.2 fps | 7277.4 fps | +54.5% |
| `singlewindow` 1280×720 | 2604.7 fps | 3642.6 fps | +39.8% |

`BINUI_VK_PERF` 拆分显示 GPU 几乎全程空闲（`svgdemo` 1.447 ms = 录制 1.244 + 等 GPU 0.052 +
呈现 0.151），剩余瓶颈在 CPU 录制侧，见 `docs/vulkan_design.md` §7.2.5。

### 验收

0 warning；四套测试全绿；`make pixels-vk` 14/14（14 个用例不同像素 0）；`make vk-window`
全部通过（含脏矩形 vs 全量重绘逐位相同）；`make vk-stress` 两轮 8 阶段 0 失败、shutdown 后
后端显存 0.0 MB；`make demos-vk` 8 个；`check_examples` 11/11。


## [0.10.0] - 2026-11-11

本轮主题：**Vulkan 绘制后端（P1~P5 全部落地）**。新后端 = MINOR；`BINUI_VERSION` 仍是 19
（无二进制布局改动，旧 `.bui`/`.bum` 不用重编）。

### 新增 — Vulkan 后端 P5：共享窗口外壳 + `GPU=vk` 示例

- **窗口外壳也抽成共享模板**：`BinUIAppGLGdi` 原来整块（1134 行）长在 GL 后端里，实际只调用
  后端的 8 个方法（`init/shutdown/resize/beginFrame/present/presentViaGdi/setTransparent/isReady`）。
  搬到 `src/backends/binui_app.h` 变成 `template <class Gfx> class BinUIAppT`：
  `BinUIAppGLGdi = BinUIAppT<BinUIGLGdi>`、`BinUIAppVK = BinUIAppT<BinUIVK>`。
  时间轴/输入/IME/无边框拖拽/帧节流/脏矩形/直翻转守卫两个后端只有一份；
  `FramePerf`/`BINUI_PERF` 统计一起共享（Vulkan 侧同样可读）。
- 后端只多提供一个 `static const wchar_t* appClassName()`：同进程两个后端并存时窗口类不冲突。
- 新文件 `src/backends/binui_ui.h`：示例/工具的统一入口（`UiBackend` / `UiApp` / `drawUi()` /
  `placeCaretAtClick()`），由 `-DBINUI_GPU_VK` 选后端；8 个示例改成这套名字，逻辑不变
  （`placeCaretAtClick` 也从 GL 后端搬进共享模板）。
- `make demos-vk` → `examples/*_vk.exe`（同一份源码 + `-DBINUI_GPU_VK`，链接 `binui_vk.o`），
  与 GL 版并存便于 A/B。
- 版本：`binui_version()` → **0.10.0**。

#### 与设计的一处偏差

设计里写的是"构建开关 + 运行期 `BINUI_GPU`"。实际只做了编译期：运行期切换要在同一进程装两套
后端，而示例的 `UiBackend&` 是**具体类型**（`<canvas>` 回调要调原语），要么再加一层类型擦除、
要么每个示例写两份 switch —— 收益不抵复杂度。像素/压力/窗口三套判据已分别覆盖两个后端。

#### 验收（v0.10.0）

| 项 | 结果 |
|---|---|
| 干净重编 | **0 warning**（`-Wall -Wextra`） |
| 四套测试 | `test_elements` / `test_css` / `test_font` / `test_capi` 全绿 |
| `make pixels` | 10/10 零差异 |
| `make pixels-vk` | 14/14 通过（6 个用例 0 差异，4 个 SVG 描边 ≤1 个 MSAA 采样） |
| `make vk-window` | 12 项阈值 128 全过；脏矩形重绘 vs 全量重绘 **3/3 逐位相同（阈值 0）** |
| `make vk-stress` | 8 阶段全 `[ok]`；shutdown 后后端显存记账 0.0 MB |
| `make demos-vk` | 8 个 `*_vk.exe`；`svgdemo`/`svgdemo_vk` 各跑 5 秒无崩溃，`BINUI_PERF` 输出一致 |
| `check_examples` | 11/11 |

### 新增 — Vulkan 后端 P4：鲁棒性加固 + 压力测试工具

`tools/vk_stress.cpp`（+ `make vk-stress`）：一轮 8 个阶段 —— resize 抖动（120 次，尺寸含 1px/奇数/
极大值）、同尺寸反复 resize（100 次）、最小化→恢复、透明窗来回切、GDI/交换链交替、init/shutdown
循环 ×20、双实例并存、泄漏复跑。每阶段都要"不崩、不死锁、最终像素正确"。

#### 修掉的三个真 bug（都由压力测试暴露）

1. **丢帧后不提交命令缓冲 → 下一帧死锁**。最小化（surface extent = 0）、交换链过期、
   `acquire` 失败三条早退路径以前直接 `vkEndCommandBuffer` 就返回；而 `beginFrame` 每帧开头
   都在等 fence，fence 只在提交时置位 → 挂死。改走 `submitNoPresent()`。
   这是本机第一次最小化就复现的（挂死、CPU 0%）。
2. **`vkUnmapMemory` 用了已释放的句柄**。显存记账把 `freeMem()` 的句柄置空后，原来紧跟其后的
   `vkUnmapMemory(device, vmem)` 拿到空句柄 → 访问违例 `0xC0000005`。改为先解映射再释放。
3. **读回缓冲在 DPI 缩放下可能偏小**。捕获路径按 `swapExtent` 拷，原来按 `w*h` 分配；
   DPI 缩放时 swapchain 可能比客户区大 → 越界写。现在按两者较大者分配。

#### 其他加固

- 每帧复查 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`：`currentExtent` 变了就重建交换链
  （跨显示器/DPI），为 0×0 则丢弃本帧（最小化）。
- 多实例安全：去掉全局 `instance_`/`device_`（会被最后创建的实例覆盖），加载函数改为显式传实例。
- `VK_ERROR_DEVICE_LOST` → 后端置未就绪，不再继续提交。
- 新增 `deviceMemoryBytes()`：区分"后端漏显存"和"驱动池化"。
- 校验层开关：`BINUI_VK_LAYERS=1` 列出实例层、`BINUI_VK_VALIDATION=1` 装了就启用。
- 交换链创建不再默认刷日志（拖边框时每帧重建），改由 `BINUI_VK_TRACE=1` 打开。

#### 实测（`make vk-stress`，probe / vkcolor）

- 8 个阶段全部 `[ok]`；结束画面与 GL 同尺寸输出 **0 差异（阈值 0）**；
- `shutdown` 后后端显存记账 **0.0 MB**，两轮之间提交内存 −7.0 MB（无线性增长）；
- init/shutdown ×20 后提交内存 105.5 → 105.5 MB。

**本机测不了的**（已在 `docs/vulkan_design.md` §6.1.3 写清）：跨显示器 DPI 切换
（只有 1 台显示器）、RDP/虚拟显示器、验证层 0 error（没装 Vulkan SDK，只枚举到 5 个第三方层）。

### 新增 — Vulkan 后端 P2b：窗口 + 交换链 + 三路 present

`BinUIVK` 现在能开真窗口了：`init(hwnd, w, h)` 建 surface + 交换链，`present()` 按条件选路。
渲染目标仍是那张 MSAA + resolve 离屏图（与离屏模式完全同一条路径），present 时才把 resolve 图
blit 进交换链 —— 所以**窗口模式与离屏模式的像素必然一致**。

- 交换链：首选 `B8G8R8A8_UNORM`（与渲染目标同格式，blit 是纯拷贝）；`vsync=true` → FIFO，
  `false` → MAILBOX/IMMEDIATE 优先；`setVsync` 改呈现模式时重建交换链。
- 三路 present：交换链常规路径；透明窗口 → 读回 + `UpdateLayeredWindow`；客户区铺满显示器
  （borderless 全屏，直翻转风险）→ 读回 + `SetDIBitsToDevice`；`acquire/present` 返回
  `OUT_OF_DATE`/`SUBOPTIMAL` 时重建。
- 降级链：交换链创建失败、或驱动没暴露 WSI 时，自动退回"读回 + GDI 贴屏"，不会整体不可用。
- 新工具 `tools/vk_window.cpp`（+ `make vk-window`）：开真窗口跑 N 帧，把**交换链图像本身**
  读回写 PNG，与 `gl_shot` 同尺寸输出逐像素比。
- 新文件 `tools/save_png.h`：BGRA → PNG，抓图工具共用（vk_shot 的本地副本已删）。

#### 实测（`make vk-window`）

| 场景 | 结果 |
|---|---|
| probe / circle（窗口 + 交换链） | 与 GL **0 像素差异（阈值 0）** |
| svgpath（斜边 AA） | 611px（0.560%），最大通道差 116（与离屏同源） |
| 强制 GDI 路径 / 透明窗口 | 均 0 差异 |
| resize 600×260 → 760×340（交换链重建） | 与 GL 在 760×340 下 **0 差异** |
| 全屏守卫 2560×1440 | 自动切 `GDI 读回`，11.4 fps，无停帧 |
| 交换链帧率（vsync=false, MAILBOX, 600×260） | 1435 fps |

#### 踩到的坑

- **`VK_USE_PLATFORM_WIN32_KHR` 必须在 `#include "vulkan/vulkan.h"` 之前定义**（vendored 头
  按平台宏裁剪），否则 `VkWin32SurfaceCreateInfoKHR` 之类直接不存在。
- WSI / 交换链函数必须**单独加载且允许为空**：离屏模式没有 surface 扩展，用统一加载会让离屏
  初始化直接失败。

### 新增 — Vulkan 后端 P3：全原语（文本 / 图片 / 线段），离屏

`BinUIVK` 现在 7 个原语全通，并且**文本与图片的实现与 GL 共用一份代码**，
所以在像素回归里两个 GPU 后端对轴对齐几何逐位相同。格式号不变（无二进制布局改动）。

- 新文件 `src/backends/binui_glyph_atlas.h`：字形图集 + 文本排版（UTF-8 迭代、emoji 簇、
  逐字形回退、图集多代、COLR 彩色 emoji、换行/对齐）。GL 与 Vulkan 都只实现一个
  `IGlyphTexHost`（位图 → 纹理）并消费 `GlyphQuad` 回调 —— 后端不再有自己的排版代码。
  落地方式：把 GL 里的实现整块搬出来，改完与搬迁前逐像素对比（阈值 0，3 个用例 **0 差异**）。
- 新文件 `src/backends/binui_win_image.h`：GDI+ 图片解码（原本 GL 与 D3D11 各一份拷贝）。
- Vulkan 纹理系统重写：上传**可以发生在帧内**（staging 拷进本帧命令缓冲，帧末等 fence 后释放），
  帧外的上传排队到下一个 `beginFrame` 开头。**因此不再需要预热帧** —— `vk_shot` 从渲两帧
  改成渲一帧，结果不变；P2 的"首帧缺圆角"瑕疵随之消失。
- 图片：`prepareImage`（PNG/JPEG 解码）/ `prepareImageRaw`（原始 RGBA，CPU 侧换 R/B 通道）/
  `drawImage`（线性采样），`generation` 不变则复用。
- 采样器拆成 nearest / linear 两个（原先每张纹理建一个 sampler，白建白删）。
- descriptor pool 加 `FREE_DESCRIPTOR_SET` 并在纹理销毁时回收，池满自动再开一个。

### 修复 — `vkCmdClearColorImage` 在 render pass 内调用会丢掉整帧

文本首次光栅化发生在帧内，于是"清零新图集"落在 render pass 里。Vulkan 不允许，
NVIDIA 驱动**静默丢掉整条命令缓冲** —— 现象是"只要画面里有文字，整帧只剩底色"。
改为用全零 staging 上传清零（`zeroTex()`），一次性 4MB/图集。

### 新增 — `make pixels-vk`

14 个用例（10 个像素卡片 + 4 个真实示例：`features` / `fontshow` / `svgdemo` / `texturedemo`）
跑 Vulkan ↔ OpenGL 逐像素对比。判据：不同像素 < 1.5%、单通道 ≤ 128（`pixel_diff` 新增第 5 个
参数）。为什么不是 0：4x MSAA 在**非轴对齐边缘**上，GL 与 Vulkan 的采样归属会差一个点
（实测逐像素正负各半、净偏 ≈ 0），这是实现差异而非绘制 bug；两类后端与 GDI 基准的距离相同
（svgjoin：VK↔GDI 7494 vs GL↔GDI 7505，共 109200 像素）。

实测：6 个纯轴对齐用例 + 4 个真实示例 **0 差异**（`fontshow` 最大通道差 0），
4 个 SVG 描边用例残差 0.2%~1.0%，全部 ≤ 128 通道。

## [0.9.1] - 2026-11-10

### 修复 — 窗口最大化后画面停住、内容割裂

- **现象**：`svgdemo` 的 `<canvas>` 频谱条在窗口最大化后不动了，还出现上下割裂；切回默认尺寸就恢复。
- **根因有两层**：
  1. **呈现**：客户区铺满整块显示器（borderless 全屏 / 最大化）时，DWM 会把 `SwapBuffers` 提升成
     直翻转。实测（NVIDIA + 远程桌面/虚拟显示器）应用仍在 ~100fps 绘制、`glBlitFramebuffer` 也正常，
     但屏幕停在旧帧 —— 所以看到的是"动画停了"。切回普通尺寸时直翻转取消，画面立刻恢复。
     修法：新增 `clientCoversMonitor()`（客户区宽高 ≥ 所在显示器），命中时和拖拽 resize 期间一样走
     `presentViaGdi`（读回离屏 → `StretchDIBits`），完全绕开驱动 present 路径；判定在
     `applyPendingResize()` 里重算，尺寸变回来自动切回 `SwapBuffers`。
  2. **失效矩形**（demo 侧）：`svgdemo` 把 `<canvas>` 的矩形在布局后缓存起来，窗口一改大小
     （`ui_scale="proportional"` 会把它挪走并缩放），缓存矩形失效 → 每帧只重绘旧位置那一小块，
     画布其余部分永远停在上一帧 → 割裂。修法：改成 `renderer.invalidRect(id)` 现取（`invalidateNode`），
     并把该坑写进 demo 注释。
- **实测**：修复后最大化状态画布区两帧差异 15.7 万像素（动画正常）；修复前 0。窗口 700/1200/1600/2000px
  宽正常，只有正好铺满显示器（2560×1440）触发，与报告一致。

## [0.9.0] - 2026-11-10

本轮主题：**SVG 填充的边缘抗锯齿**（【格式 v19】）。破坏性：`BINUI_VERSION` 18 → 19，
旧 `.bui` / `.bum` 需重编（新增 `PATH_AA` 标志位）。

### 新增 — `shape-rendering` 开启边缘抗锯齿

- `shape-rendering="auto" | "optimizeQuality"` → 该子树里的**路径填充**边缘抗锯齿；
  `crispEdges` / `optimizeSpeed` / 没写 → 硬边（**默认，与 0.8.x 完全一致**）。
  可继承（写在 `<svg>` / `<g>` 上对整个子树生效），不认识的值告警。
- 不做 MSAA/stencil：三个后端只有 `fillRect`，所以把 AA 做进共用扫描线填充器 ——
  每行纵向 4 次子采样得到**逐像素覆盖率**，同覆盖率的连续像素合并成一条带，颜色 alpha 乘覆盖率。
- 宽 > 4096px 或条带数超上限时自动退回硬边（宁可锯齿也不崩）。

### 修复 — 覆盖率定点精度

第一版累加器用整数 `255/4 = 63` 做单位，满覆盖只剩 `252/255`，整块面积被系统性低估 1.2%
（三角形实测 4940 / 解析 5000）。改 1024 定点后实测 5000.2。

### 实测

- 面积守恒：100×100 盒里的直角三角形，AA alpha 加权面积 5000.2（解析 5000），硬边 5048。
- 条带数：三角形 AA 199 条（100 条半透明边缘），硬边 98 条（约 2×）。
- 三端：AA 填充在**纯色背景**上 GDI 与 GL **逐像素一致（0 差异）**；叠在渐变上时 1.58% 的像素
  有差异（GDI 用近似底色做 alpha 混合的既有限制，非 AA 的半透明填充叠渐变同样有 20% 差异）。
- 像素基准新增 `svgaa`（420×260），`make pixels` **10/10 零差异**；`svgdemo` 星形图标改用
  `shape-rendering="auto"` 作为长期样例。

### 明确没做

描边的抗锯齿（GDI 的 `LineTo` 是硬边，GL/D3D 的 `fillLine` 自带 AA —— 既存差异，需要重写描边通路）；
默认开 AA（会改所有既有 UI 的观感并让 10 个基准全部失效）。

## [0.8.2] - 2026-11-10

### 修复 — 折线被当成闭合多边形填充

- 0.8.0 把 `<polyline>`/`<polygon>` 改成编路径后，**没写 `fill` 的折线会按 SVG 默认填成黑色**
  （`svgdemo` 的走势图就变成了两块黑疙瘩）。这符合规范、浏览器同样如此（SVG 的经典坑），
  但对"以为自己在画线"的人是一记闷棍。
- 现在：`<polyline>`/`<polygon>` 有 `stroke` 却没提过 `fill` 时**明确告警**并给出写法
  （`fill="none"`）；同时把 `svgdemo` 里的折线/轮廓补上 `fill="none"`（素材本身不干净）。
- 注意"没写 fill"和"写了 fill=none"是两回事：前者是默认黑填充，后者是不填 —— 告警只在
  **从没提过 fill** 时触发（`fill="none"` 算提过）。

### 验证

- `make all tools` 0 warning；四套测试全绿；`make pixels` 9/9 零差异；
  `check_examples` 11 个 UI 0 问题；`svgdemo` 截图重出（走势图恢复成纯折线）。

## [0.8.1] - 2026-11-10

本轮主题：**SVG 收尾的两件漏项**（无格式变化，`BINUI_VERSION` 仍是 18）。

### 修复 — `userSpaceOnUse` 下的百分比改为按视口折算

- 以前把 `x1="50%"` 当成 0.5 个用户单位（差一个数量级，画出来几乎看不见），只能告警让作者改写。
  现在按 SVG 规范折算：`x` 方向 × 视口宽度、`y` 方向 × 视口高度、`r` × `sqrt((vpW²+vpH²)/2)`。
- 视口尺寸由编译器 `Ctx.vpW/vpH` 带下去：进 `<svg>` 取 viewBox 宽高（无 viewBox 退回 `width/height`），
  `<g>` 与普通容器继承。`objectBoundingBox`（默认）语义不变。
- 实测：viewBox 100×50 + `x1="0%" y1="0%" x2="100%" y2="100%"` → 归一化端点 `(0,0)→(1,1)`。

### 新增 — 路径填充条带的跨帧缓存

- 小图标上填充条带本来可以忽略（22 条带 ≈ 0.018 ms），但大图形不是：300px 的环 332 条带
  ≈ 0.218 ms/次，几个就吃掉一帧的预算。
- 缓存条目 `PathBandCacheEntry` 放在 `binui_paint.h`，由渲染器按 nodeId 持有（**一份实现，
  三端共用**，三端一致性不靠各端自觉）；key 混合了节点盒尺寸、填充/描边色、不透明度、
  两条渐变指针与 flags —— 任何影响条带颜色的东西变了 key 就变，不用逐项追踪失效；
  布局变化（`loadLayout`/`relayout`/`mountComponent`）整表作废并 ++gen。
- 两条预算：单节点条带 > 1024 不缓存、缓存条目 > 256 不再新建（大图不会吃内存）。
- 实测（2000 次同一节点）：24px 图标 0.018 → 0.0002 ms/次（~86×）；
  300px 环形 0.218 → 0.0002 ms/次（~1051×）。
- 正确性：单测用"篡改缓存里的颜色再画一次"证明真的命中了缓存；换颜色必须重算并写回；
  `nullptr`（不缓存）照常绘制。`make pixels` 9/9 零差异 —— 缓存没有改变任何像素。

### 验证

- `make all tools`：0 warning；四套测试全绿（新增 `TestPathBandCache` 与 userSpace 百分比断言）。
- `make pixels` 9/9 零差异；`check_examples` 11 个 UI 0 问题。

## [0.8.0] - 2026-11-10

本轮主题：**SVG 收尾**（【格式 v18】）—— 元素类型齐了、坐标系闭合了、文字锚点对了、
渐变的偏心焦点真算了。破坏性：`BINUI_VERSION` 17 → 18，旧 `.bui` / `.bum` 需重编。

### 新增 — `<polygon>` / `<polyline>` 编成路径

- 以前两者被拆成一堆 `BINUI_LINE`，**`fill` 完全无效**（多边形填充是最常见的写法，这是真 bug），
  渐变描边/虚线/连接方式也表达不了。现在有颜料就编成 `BINUI_PATH`（`polygon` 闭合、
  `polyline` 开放），填充/描边/虚线/linecap/linejoin/渐变全部复用路径通道。
- 既没 `fill` 也没 `stroke` 的裸 `<polyline>` 仍然不产生节点（不凭空多一个黑块）。

### 新增 — `<line>` 按需升级成路径

`<line>` 保持轻量 `BINUI_LINE`，但写了 `stroke-dasharray` / 渐变描边 / `stroke-linecap` /
`stroke-linejoin` 时自动升级成两点开放路径 —— 这些能力 LINE 节点表达不了（以前是静默失效）。

### 新增 — 文字与虚线

- `text-anchor="start|middle|end"`：`x` 从"盒子左边"变成"锚点"（用编译期字宽估算折算出盒子左边）。
- `stroke-dashoffset`：编译期把虚线相位往前推（负值取模）。实测 pattern `10 10`、路径 0→100 时，
  offset 0/5/10 的首段实线分别是 `[0,10]` / `[0,5]` / `[10,20]`。

### 新增 — `preserveAspectRatio` 的 `slice` 与 9 宫格对齐

`xMinYMin … xMaxYMax` 九种对齐 + `meet`/`slice` + 可选 `defer` 全部支持；
`slice` 取较大缩放（铺满、超出由 `<svg>` 裁剪）。

### 修复 — 嵌套 `<svg>` 的三处偏移（以前是"整块空白"）

内层 `<svg>` 的 `x/y/width/height` 是父用户坐标，而普通元素的 left/top 是容器像素。三处必须同时
对齐：① 节点盒要过父变换取包围盒；② viewBox 要映射到**设备尺寸**的内容盒（映射到用户尺寸会把
父变换算两遍）；③ 运行期重排的 `relX/relY` 必须是相对父内容盒的偏移（写绝对坐标会在 `setSize`
后再挪一次 —— 这也是为什么 GL 离屏先 `setSize` 时"只有 GL 露馅"）。修完后嵌套 svg 在 GDI 与 GL
下逐像素一致（`0 diff`）。

### 新增 — 径向焦点 `fx/fy`（SVG 锥形语义）

从焦点 `F` 经 `P` 的射线打到单位圆于 `Q`，`t = |P-F| / |Q-F|`；实现是归一化后解
`|F + s·(P-F)| = 1` 取正根、`t = 1/s`。焦点在圆外时规范未定义 → 告警并按同心圆画。
（踩坑记录：二次项符号写反时圆心处 `t` 会变成 1.0，手算应为 `4/9 = 0.444`。）

### 变更 — 格式 v18

- `BinUIPathGradient` 增加 `fx, fy`（92 → 100 字节）；其余能力（polygon/polyline、`<line>` 升级、
  `text-anchor`、`dashoffset`、`slice`、嵌套 svg）都不改格式。

### 验证

- `make all tools`：0 warning；四套测试全绿。
- `suite_svgpath` 新增：polygon/polyline → PATH（含"裸 polygon 不产生节点"）、`<line>` 按需升级、
  text-anchor 盒子左移、虚线三相位的首段区间、`BuildViewBoxMatrix` 的 meet/slice/9 宫格矩阵、
  径向焦点四个 t 值、嵌套 svg 盒随 `<g>` 缩放平移。
- 像素基准新增 `svgfinish`（420×260，覆盖上述全部）；`make pixels` **9/9 零差异**。
- 三端一致性（`gl_shot` GL 离屏 vs GDI 基准）：嵌套 svg 修完后该区差异只剩 125/4096（圆角与描边
  AA），纯填充区（polygon 星形、径向焦点球）**0 差异**。
- `check_examples`：11 个 UI 0 问题。

## [0.7.0] - 2026-11-10

本轮主题：**描边能力补完 + 精确命中**（【格式 v17】）。
破坏性：`BINUI_VERSION` 16 → 17，旧 `.bui` / `.bum` 需重编。

### 新增 — 渐变描边 `stroke="url(#id)"`

- 以前只按首个色标纯色画（还带告警），现在真的渐变：**每段线按自己的中点取色**。
  折线本来就是曲线细分出来的（24 单位图标 ≈ 0.1px 容差），逐段取色等价于逐像素取色，
  不需要给后端加渐变画刷，三端仍然只画 `fillLine` / `fillRect`。
- 线性 / 径向、`spreadMethod`、`gradientTransform`、`objectBoundingBox` / `userSpaceOnUse`、
  2–8 个色标、`href` 继承 —— 与填充渐变同一套能力，坐标折算也**共用同一个函数**
  （只有 alpha 键不同：`stroke-opacity` 而不是 `fill-opacity`）。
- 填充与描边可以同时是渐变：路径记录里两条可选渐变记录各占 92 字节，互不影响。
- 继承：`<svg stroke="url(#g)">` / `<g stroke="url(#g)">` 全子树生效；子元素写纯色 `stroke`
  会清掉继承来的渐变。
- `<line>` / `<polyline>` / `<polygon>` 表达不了渐变描边（它们编成线段节点），明确告警并建议改用 `<path>`。

### 新增 — `stroke-linecap="square"`

- 方头 = 端点沿切线各外延半个线宽；转角几何仍用未外延的原始端点（否则拐角会多出一截）。
  实测：线宽 10、路径 0→100 时 butt 是 `(0,0)-(100,0)`，square 是 `(-5,0)-(105,0)`。

### 新增 — 精确命中（P3 首项）

- `BINUI_PATH` 有填充时，包围盒命中之后再按"点在路径内"判一次（`PointInPathNorm`，
  与 `PointInPath` 同一套绕数/奇偶逻辑，直接吃 `uint16` 归一化点），顺带取 ±1 设备像素容差。
  **环的洞、星形的凹口不再误命中**；描边-only 的路径仍按包围盒（细线本来就不好点）。

### 修复 — `fill-opacity` / `stroke-opacity` 作为表现属性时被静默忽略

- `fade()` 以前只查 `style`（内联 style / CSS 规则），没查元素属性，所以
  `fill-opacity="0.5"`、`stroke-opacity="0.5"` 都不生效。现在按 `style > 属性` 取值 ——
  渐变描边要折 `stroke-opacity`，正好踩在这条路上。

### 变更 — 格式 v17

- 路径记录在填充渐变之后可再追加一条 `BinUIPathGradient`（`PATH_STROKE_GRAD`）。
- `stroke-linecap="square"` 落进既有的 `BinUIPathEntry.cap` 字段（`BINUI_CAP_SQUARE`）。
- 渲染器 / 链接器 / 三端 `BINUI_PATH` 分支都带上描边渐变：`PaintPathNode` 的颜料参数
  收拢成 `PathPaintSpec`（填充色/描边色/两条渐变/不透明度），描边回调从"固定颜色"改成"逐段取色"；
  GDI 的画笔缓存改成按"宽度 + 颜色"缓存（否则渐变描边会一直用第一段颜色）。

### 验证

- `make all tools`：0 warning；四套测试全绿。`suite_svgpath` 新增：方头外延、渐变描边逐段取色
  （纯色同色 / 渐变首末段色差）、精确命中（环带命中、洞心不命中、盒外不命中）、
  `linecap=square` 落库且不报警告。
- 像素基准新增 `svgstroke`（420×260）：渐变描边四样（线性/径向/三色标/虚线）+ 线帽三连 +
  方头虚线 + 三个真图标；`svggrad` 基准重出（`stroke="url(#…)"` 现在是真的渐变）。
  `make pixels` **8/8 零差异**。
- 三端一致性（`gl_shot` GL 离屏 vs GDI 基准）：渐变填充区内部像素 **20694 个 0 差异**；
  `svgjoin` 内部 702 个中 5 个、`svgstroke` 内部 444 个中 12 个不同 —— 差异全在线段边缘
  （GDI `LineTo` 与 GL `fillLine` 的取整/抗锯齿，与渐变无关）。
- `check_examples`：11 个 UI 0 问题；`examples/svgdemo` 的心形图标改成三色标渐变描边 + 方头。

## [0.6.0] - 2026-11-10

本轮主题：**`stroke-linejoin` 的 miter / bevel 补齐**（SVG P2 收尾，格式仍是 v16，
**不破坏兼容** —— 旧二进制照旧能读，没有 `stroke-linejoin` 的路径仍然是圆角连接）。

### 新增 — `stroke-linejoin`: round / miter / bevel + `stroke-miterlimit`

- 圆角（`round`，binUI 的历史默认）、斜接（`miter`）、斜面（`bevel`）三种连接都实现了：
  - `miter` 补四边形 `(p1, m, p2, b)`，`bevel` 补三角形 `(p1, b, p2)`；
  - `ratio = 1/sin(θ/2)` 超过 `stroke-miterlimit`（SVG 默认 4）时按规范退回 bevel；
  - 转角 < 15° 的微折点仍然不补（折线是曲线细分出来的，缺口只有零点几像素）。
- **实现方式**：后端没有"填多边形"的原语，缺口复用路径填充那套扫描线条带
  （`BuildPathFillBands` → `fillRect`），三端只多接一个"填条带"回调，几何只写一份。
  一个 72px 图标加 10 个斜接尖角，合计只多 **9 条条带**。
- **斜接尖角会伸出几何包围盒 `(ratio-1)·半线宽`**，而三个后端都把每个节点裁剪到自己的矩形 ——
  不处理的话 `miterlimit` 调大也看不出变化（尖角画出来又被裁掉）。现在编译期用运行时
  同一个 `PathJoinAt` 逐顶点算交点、把节点盒撑开（五角星 72px 盒 / 2px 描边 /
  `miterlimit=10`：盒子 **88.00×84.08 → 97.19×92.43**），尖角不再被切平。
- `stroke-miterlimit` 只认数字（<1 告警并按 1 处理）；藏在路径记录的空闲字节
  `BinUIPathEntry.reserve` 里（0 = 运行期按默认 4），**所以格式版本不需要升**。
- 认不出的 `stroke-linejoin` 值明确告警（可选 round / miter / bevel），不再静默。
- `stroke-linecap="square"` 仍未实现（按 butt 画并告警，要方头把两端各延长半个线宽）。

### 验证

- `make all tools`：0 warning；四套测试全绿。`suite_svgpath` 新增：
  - 运行期几何 9 组断言（斜面三角形顶点、斜接四边形顶点、`miterlimit` 超限退回斜面、
    尖刺真斜接、共线不补、圆角不产生多边形）；
  - 编译端到端 7 组断言（三种 join 落库、`stroke-miterlimit` 落库、尖角超限时盒子不撑开、
    尖角未超限时盒子被撑开、未知值告警、`linecap=square` 告警）。
- 像素基准新增 `svgjoin`（420×260）：五角星 round / miter / bevel 三连 + 粗折线尖角 +
  `miterlimit=4` vs `12` 的尖刺对照 + star / chevron / triangle 真图标。
  `make pixels` **7/7 零差异**。
- 三端一致性（GDI 离屏 vs GL 窗口）：离描边边缘 ≥4px 的内部像素 702 个中 5 个不同（0.7%），
  差异全在线段的端点/AA 边缘（GDI `LineTo` 与 GL `fillLine` 的取整差异，与连接方式无关）。
- `check_examples`：11 个 UI 0 问题。

## [0.5.0] - 2026-11-10

本轮主题：**SVG 渐变**（`<linearGradient>` / `<radialGradient>`，【格式 v16】）。
破坏性：`BINUI_VERSION` 15 → 16，旧 `.bui` / `.bum` 需重编（`binui.res` 里是源码，重编即可）。

### 新增 — SVG 渐变填充

- `fill="url(#id)"` 引用 `<linearGradient>` / `<radialGradient>`，现在能直接粘现成素材了：
  - **线性**任意角度（`x1/y1/x2/y2`）、**径向椭圆**（`cx/cy/r`，包围盒非正方时两轴半径不同，
    符合 SVG 语义；`rx/ry` 分开存就是为了这个）
  - **2–8 个色标**：`offset`（支持 `"50%"`）、`stop-color`、`stop-opacity`，也认
    `style="stop-color:#fff;stop-opacity:.5"`；缺 `offset` 按等距插值、整体非递减夹紧；
    `stop-color="currentColor"` 沿祖先链取 `color`
  - `spreadMethod="pad|repeat|reflect"`
  - `gradientTransform`（走既有 `ParseSvgTransform`）、`gradientUnits="objectBoundingBox"`（默认）
    与 `"userSpaceOnUse"`
  - `href` / `xlink:href` 继承另一个渐变的色标与未写明的属性（深度 ≤8 + 环检测）
  - `fill` 的继承链继续生效：`<svg fill="url(#g)">` / `<g fill="url(#g)">` 全子树带渐变
- **实现方式**：渐变在编译期折成**包围盒归一化坐标**（与折线点同一套映射，缩放节点盒时渐变
  跟着缩放），运行期把每条扫描线条带的颜色从常量换成 `t` 的函数 —— 不需要着色器、不需要纹理、
  不需要每端各写一份采样数学，三端仍然只画 `fillRect`。两处关键取舍：
  - **量化**：`t` 量化到 `levels` 级（每 ~2 设备像素一级，8..96）、级内取级中心色，同色行才能
    继续合并成高条带 —— 否则每条带颜色都不同，400px 高的形状就是 400 条带；
  - **按方向切段**：颜色沿 x 变时按"走一级要几个像素"把这一行切细，纯垂直渐变一段都不切。
    结果横竖两个方向的条带数都只跟级数同量级：100×100 垂直/水平渐变各 **50 条**，
    24×24 图标尺寸对角渐变 **20 条**，400×400 对角渐变 **2352 条一次画完**（不降级）。
- `<circle>` / `<rect>` 的**渐变填充**也会自动降级成路径（轻量映射只能表达一个颜色）。
- 三端一致性实测：同一份 UI 的 11 个渐变区域（横/竖/45°/径向/宽盒径向/repeat/rotate/窄条/
  盾牌/圆角矩形/`<g>` 继承）**GDI 与 GL 逐像素完全一致（最大通道差 0）**；差异只落在既有的
  描边边缘（GL `fillLine` 自带 AA）与文字抗锯齿上。

### 新增 — 不支持的渐变写法一律告警（都带替代方案）

`stroke="url(#…)"`（渐变描边，按首个色标纯色画）、`>8` 个色标（截断）、径向焦点 `fx/fy`
（按同心圆画）、径向 + `gradientTransform` 含旋转/斜切（按轴对齐椭圆近似）、
`userSpaceOnUse` 下的百分比（按用户单位折算，不是 viewport 比例）、
`<text>` / `<polyline>` / `<polygon>` 的渐变填充（降级纯色）、`url(#不存在)`（退回纯色）。
`<linearGradient>` / `<radialGradient>` / `<stop>` 也不再出现在"不支持标签"表里。

### 变更 — 格式 v16

- 路径记录在定长头 `BinUIPathEntry` 之后可选追加 `BinUIPathGradient`（92 字节：
  类型 / spread / 2 个端点或圆心 + 椭圆两轴半径 / 内联 8 个色标），只有 `PATH_FILL_GRAD`
  置位时才写 —— 纯色路径的字节布局与 v15 完全一样，多出来的成本只落在渐变节点上。
- 单条路径的条带上限 `kMaxPathBands` 2048 → 4096（渐变的条带比纯色多，见上）。
- 渲染器 / 链接器 / 三端 `BINUI_PATH` 分支都带上了渐变；`PathRecord` 新增 `hasGrad` + `grad`。

### 验证

- `make all tools`：0 warning。
- 四套测试全绿；`suite_svgpath` 新增 `TestGradientBands`（量化/切段/合并/spread/退化/透明度
  共 9 组数值断言）与 `TestGradientCompile`（默认 oBB 端点、`userSpaceOnUse` 折算、
  `href` 继承、径向默认 50%、缺定义告警 4 组端到端断言）。
- `check_examples`：11 个 UI 0 问题；`examples/svgdemo` 图标条新增四个渐变样例并重出截图。
- `make pixels`：**6/6 零差异**（新增基准 `svggrad`，420×260）。

## [0.4.0] - 2026-11-10

本轮主题：**SVG 从"五个图元"升级成"能直接粘现成素材"**（【格式 v15】），
外加打包清单 `binui.res`、`binui_export` 解包工具，以及一批"写了没效果"的告警清理。
破坏性：`BINUI_VERSION` 14 → 15，旧 `.bui` / `.bum` 需重编（`binui.res` 里是源码，重编即可）。

### 新增 — SVG P2-4：`stroke-linecap="round"`

- 开放子路径的两端可以补圆点了（复用 `PaintPathStroke` 已有的 `roundCap` 参数，
  格式里 `BinUIPathEntry.cap` 这个字节一直在，只是编译器没填）。
- `stroke-linecap="square"` 未实现：按 butt 画并明确告警（要方头把路径两端各延长半个线宽）。
- `stroke-linejoin="miter"/"bevel"` 未实现：按圆角连接画并告警（尖角图标会比标准 SVG 略圆）。
  以前这两个属性在"已消费属性"表里，写了就是**静默无效** —— 现在至少说出来。

### 新增 — SVG P2-3：`<text>` 与 `<image>`

- `<text x y font-size fill>` 编成普通 `LABEL`：坐标走 viewBox/transform，`fill` 当文字色，
  `font-size` 直接生效；SVG 的 `y` 是**基线**，按"盒顶上移 0.8 个字号"近似
  （没有富文本能力，`<tspan>` 仍告警）。
- `<image x y width height href>` 编成普通 `IMAGE`：`href` / `xlink:href` 进资源表
  （路径同样相对 UI 文件），尺寸跟着 viewBox 缩放；`data:` 内联图片明确告警（不支持）。
- 修一个"属性名走错通道"的坑：SVG 用 `font-size` 属性，而编译器取字号读的是 `size`
  属性 + `font-size` CSS —— 只写 SVG 属性时字号被忽略（实测落库 14 而不是 20）。
- 验证：`suite_svgpath` 新增 text/image 端到端断言（文本、颜色、基线近似、字号、
  imageIndex 与资源表路径）；四套测试全绿、`check_examples` 0 问题。

### 新增 — SVG P2-2：`<defs>` / `<symbol>` 模板 + `<use href="#id">` 引用

- 图标素材里常见的"一份模板多处引用"现在能用：`<defs>`/`<symbol>` 里的元素是**模板**
  （自己永远不画），`<use href="#id">`（也认 `xlink:href`）在引用处克隆一份。
- 语义对齐 SVG：`<use x y>` 平移；引用 `<symbol>`/`<svg>` 且给了 `width`/`height` 时，
  目标的 `viewBox` 按**等比居中**缩放到这个尺寸；克隆体去掉 `id`（避免重复 id），
  根元素改写成 `<g>`（`<g>` 不建立坐标系，viewBox 由 transform 承担）。
- 与组件内联不冲突：`<use src="x.bum">` 是**组件**（编译期内联/运行期挂载），
  `<use href="#id">` 是 **SVG 引用** —— 按有没有 `src` 分流，两者可以同时存在于一份 UI 里。
- 容错：引用不存在的 id、循环引用（引自己的祖先）、外链 `href` 都会明确告警，
  并且不留空盒子（以前会退化成一个 300px 宽的空标签）。
- 验证：`suite_svgpath` 新增 `<defs>` 模板引用两次 + `<symbol viewBox>` 按 40px 缩放的
  端到端断言；像素基准 `svgpath` 增加第三行（三个 badge 克隆 + 两个不同尺寸的
  symbol 三角形）；`make pixels` 5/5 零差异、`check_examples` 11 个 UI 0 问题。

### 新增 — SVG P2-1：带描边的圆/矩形自动走路径 + `stroke-dasharray`

- **`<circle>` / `<rect>` 带描边时自动降级成 `BINUI_PATH`**：以前它们只能走轻量映射
  （`LIST` + 圆角 = 一个颜色），`fill` 与 `stroke` 并存直接告警、描边被丢掉。
  现在只要写了 `stroke`（且线宽 > 0）就编成路径，圆角矩形的 `rx` 也会变成真正的
  四分圆弧（新 `AppendRoundedRect`）。没有描边时**继续走轻量映射** —— 九宫格填充
  比扫描线条带更干净，不该为了统一而变差。
- **`stroke-dasharray` 支持**：编译期按弧长把折线切成"实线段"子路径
  （新 `ApplyDashPattern`），运行期照常画折线 —— 三端零改动，曲线（虚线圆）也对。
  长度按用户单位再乘坐标变换系数，所以放大后虚线间距一起放大（SVG 语义）。
  奇数个值按规范重复一遍（`3` → `3 3`）；全 0 视为不画并告警。
- 仍不支持：`stroke-linecap`（一律 butt）、`stroke-linejoin`（一律圆角连接，转角 > 15° 补圆点）、
  渐变、`mask`/`clipPath`/`pattern`/`marker`、`<defs>/<use>/<symbol>`、`<text>`、`<image>`。
- 验证：`suite_svgpath` 增加圆角矩形几何、虚线切段（10 单位 + `2 2` → 3 段、实线总长 6）、
  "带描边的圆/矩形必须编成路径且 fill+stroke 都在"的端到端断言；像素基准 `svgpath`
  第二行补上四种新写法（描边圆 / 描边圆角矩形 / 虚线 / 虚线圆），5/5 零差异。

### 新增 — SVG `<path>` / `<ellipse>`：真正的矢量路径（【格式 v15】）

- **现在能直接粘现成图标**（Lucide / Feather / Material 那一套 `<path d="M…">`），
  不再是"五个图元 + 路径不支持"。命令集 `M L H V C S Q T A Z` 全支持（绝对/相对、
  隐式重复、紧贴负数、弧标志紧贴写法），`A` 在编译期转三次贝塞尔后统一细分。
- **格式**：新增节点类型 `BINUI_PATH = 10`（`BINUI_NODE_PATH`）。节点 `x/y/w/h`
  就是几何包围盒（有描边时外扩 `strokeW/2`，笔画不溢出、命中区盖得住），
  折线放在新的**路径段**里（定长头 + 子路径表 + `uint16` 归一化点），
  段序在扩展绘制段之后、名称段之前。
- **为什么存折线**：运行期只做"归一化点 × 节点盒"的两次乘加 —— 不解析、不缓存、
  重排/等比缩放下形状自动跟随；曲线细分在编译期一次做完（容差 0.1 用户单位，
  实测半径 10 的圆弧离真圆最大偏差 0.0196px）。代价是极限放大能看到折线。
- **三端共用一份几何**（`include/binui_paint.h`）：扫描线求交 → 轴对齐条带 →
  `fillRect`，相邻同宽行自动合并；撞条带上限时按行步长降级（1→2→4→8）保证画完
  （400px 的圆：cap=16 触发截断，cap=256 + 步长 8 → 47 条完整画完）。
  描边逐段画线、转角超过 15° 补一个圆点（圆角连接）。
- **SVG 语义**：`viewBox`（等比 `meet` 居中，`preserveAspectRatio="none"` 直接拉伸）、
  `<g transform>`（`translate/scale/rotate/skewX/skewY/matrix` 可串联，且 `<g>` 不建立坐标系）、
  `fill`/`stroke`/`stroke-width`/`fill-rule` 变成**可继承**属性（含 `currentColor`），
  `stroke-width` 跟着坐标变换一起缩放（放大后线不会还是 2px）。
  **SVG 颜料属性按规范走 CSS > 表现属性 > 继承 > 默认**（与 binUI 其它属性"属性盖 CSS"
  相反，单独一条 `pickSvgPaint`）。
- **顺手修掉一个真 bug**：`<svg>` 的注释从第一条就说"绘制裁剪到容器"，但三端都没实现，
  子图形一直能画到容器外面。现在 `clippingAncestors()` 把 `<svg>` 也算裁剪祖先。
- **破坏性**：`BINUI_VERSION` 14 → 15，旧 `.bui` / `.bum` 一律拒载，需重编
  （与历史每次 bump 一致；`binui.res` 里存的是源码，重编即可）。
- 仍不支持（有明确告警）：渐变 `linearGradient/radialGradient`、`stroke-dasharray`、
  `mask`/`clipPath`/`pattern`/`marker`、`<defs>/<use>/<symbol>`、`<text>`、`<image>`。
  `<circle>` / `<rect>` 仍是轻量映射（不能 fill 与 stroke 并存），需要描边请用 `<path>`。
- 验证：`test_elements` 新增 `suite_svgpath`（解析/精度/容差/错误/上限、面积与采样
  双向验证填充、evenodd 与 nonzero 差异、条带合并、描边接缝、viewBox/g transform/继承/
  CSS 优先级）；像素基准新增 `docs/pixels/svgpath.png`；GDI 与 GL 对照 —— 填充 0 像素差，
  描边只在边缘因 GL 的 `fillLine` 抗锯齿不同。

### 修复 — SVG 不支持的标签/属性从"静默无效"改成明确告警

- 现象：写 `<path d="…">` / `<ellipse>` / `<text>` / `<g transform="translate(…)">` / `viewBox`
  编译**零提示**，画面上那一段就是空的，只能靠自己猜是写错了还是不支持。
- 根因：`<path>` 等标签走"未知 HTML 标签，已按普通容器处理"，对 SVG 作者毫无信息量；
  `transform` 属性更糟 —— 它在 `UsedHtmlAttrs()` 里，被当成"已消费"直接放行，**一条告警都没有**。
- 修复：`<path>` / `<ellipse>` / `<text>` / `<tspan>` / `<g>` / `<defs>` / `<linearGradient>` /
  `<radialGradient>` / `<mask>` / `<clipPath>` / `<pattern>` / `<marker>` / `<symbol>` / `<image>`
  都给出"不支持 + 换成什么"的专用提示；`transform` 属性与 `viewBox` 单独告警并说明替代写法。
- README 的 `<svg>` 条目补一份**不支持清单**（原来只写了支持什么）。
- `check_examples` 11 个 UI 仍 0 问题（没有误伤现有写法）。

### 新增 — 打包清单 `binui.res` + `binui_export`：**内嵌的包也能解包回打包前的目录结构**

- 之前文档里写"从 `.bui` 反推源码树做不到"——那是**没有清单**时的结论。现在补上了：
  编译时在产物目录写一份 `binui.res`（清单 + 原路径），记录主 UI 与每个被内联/外挂组件的
  **完整原始文件内容** + 相对 appRootDir 的路径；`binui_export` 按它逐字节还原，
  不是"节点树反推 HTML"那种有损转换，也不是偏移量记录（倒出来就是原文件本身）。
- **默认不产出，要显式开**：清单里带着源码，不该是默认行为 —— `binui_compile --res` 才生成。
  它是旁挂文件，布局里没有任何东西引用它。实测删掉后同一份 `.bui` 的离屏渲染
  **逐像素不变**（0 差异），只是不能再解包。
- **不动二进制布局**：`BINUI_VERSION` 保持 14，`.bui` 一个字节都不变，加载路径与启动性能
  零影响。想保护源码就别带这个文件（默认也不产出，要 `--res` 才有）。
- **一目录一份**：多个 `.bui` 编到同一目录时清单**合并**（同路径以本次为准）。
- 路径口径：先记绝对路径，最后统一折算成**相对所有文件共同祖先**的相对路径。所以
  `main.bui.html` 在 `examples/features/`、组件用 `../../components/x` 引用的工程，
  解出来的就是 `examples/features/main.html` + `components/x.bum.html`，与原树一致。
- 新增 `/lib` 侧：`CompileOptions::emitSrcMap`（默认 false）、`CompileResult::srcMap`、
  header-only 的 `include/binui_srcmap.h`（`BuildSrcMap` / `ParseSrcMap`）。
- 新增工具 **`binui_export`**：`binui_export main.bui -o restore/`，支持 `--list`。
  `binui_compile` 加 `--res` 才产出 `binui.res`（默认不产出）；旧产物 `<x.bui>.srcmap`
  仍能读取。
- 实测闭环（examples/features：源码组件 + 二进制组件 + 外挂组件）：
  编出 `main.bui` → `binui_export` → 6 个文件**逐字节 SHA256 全等** →
  解包出来的树再编一次 → `.bui` **逐字节相同**（同一 SHA256）。
- 顺带：`binui_compile -o` 指向不存在的目录时自动建父目录（以前直接失败）。
- 测试：`test_css` 新增 9 条清单断言（开关产出 / 可解析 / 第 0 条是主 UI /
  与原文件逐字节一致 / 多实例只记一份 / 嵌套组件在清单 / 读不到的组件不进清单 /
  路径都是相对 / 默认不产出）。

### 新增 — 文档：目录结构（单目录 / `binui/` 树 + 分类子目录；内嵌 vs 外挂的取舍）

- `docs/components.md` 新增第十节「目录结构」：
  - 几个组件 → **全放一个目录**（`main.bui`、`main.bui.html`、`button.bum(.html)`、图片）；
  - 几十上百个 → **`binui/` 树 + `components/<分类>/`**，带资源的组件（图标/字体/贴图）
    单独一个文件夹（`components/Button/primary.bum.html` + `star.png`），纯样式组件平铺即可。
  - 实测多级目录：宿主 `<use src="components/Button/primary.bum.html">`，组件内
    `<img src="../Icon/star.png">` 与嵌套 `<use src="../Label/tag.bum.html">`，
    编译产出 `736 字节，资源 1 项`，资源段里记的是宿主的 `components/Icon/star.png`。
- 内嵌 vs 外挂列了对照表并给出结论：**发行优先内嵌**（运行期零文件读取），外挂换来的是
  可替换性（皮肤/插件/按需加载），不是性能。
- 分发随意：只给 `.bum`（保护源码）/ 只给源码（可改、就地编译）/ 都给（二进制优先）。
- 已知限制补一条：**从 `.bui` 反向导出组件源码树现在做不到**（内联抹平了组件边界），
  要做需要"组件区间段 + 节点树→HTML 序列化器"两步，v15 起才可能。
- `README.md`、`components/README.md`、`docs/README.md` 同步一句指引；原「十、已知限制」顺延为十一。

### 变更 — 组件源码统一改名为 `.bum.html`（标准组件库 11 个 + examples/features 3 个）

- 事实：**组件源码的扩展名从来不影响任何行为**。编译器按**内容**选解析器
  （`CompileBinUIEx`：只有首个标签是 `<binui>` / `<root>`，或裸 `<app_window>` 且正文不含
  `DOCTYPE`/`<html>`/`<body>`/`<head>`，才走 legacy XML 路径）；组件内联更是直接交给 HTML
  元素解析器（`ExpandComponents` → `ParseElements`）。所以 `.bum.xml` 里的 "xml" 是一个
  不产生任何能力的后缀。
- 但它会误导：旧 README 写着"`.bui.xml` / `.bum.xml` 同样按 XML 解析"（**错的**），
  作者也会以为存在两种组件格式。主 UI 一直是 `main.html`，组件却叫 `.bum.xml`，两边对不上。
- 改名：`components/*.bum.xml`（11 个）与 `examples/features/*.bum.xml`（3 个）→ `.bum.html`；
  示例 UI、文档、Makefile / CMakeLists、`binui_compile` / `check_examples`、测试同步。
- **`.bum.xml` 仍然识别**（`ProbeAsset` 候选表里排在 `.bum.html` 之后），旧引用不会突然失效；
  只是文档和新代码不再这么写。裸名字候选顺序相应改为
  `.bum` → `.bum.html` → `.bum.xml` → `.bui.html` → `.bui.xml` → `.html` → `.xml`。
- 顺带修一个被改名暴露的 bug：`binui_compile x.bum.html` 的默认输出名算成了 **`x.bum.bui`**
  （`.html` 分支直接把后缀换成 `.bui`，没先判断是不是组件名）。现在正确输出 `x.bum`。
- 验证：`check_examples` 11 个 UI 0 问题；9 张离屏渲染图与改名前逐像素对比，8 张**零差异**，
  features 只差那三颗演示 `<use>` 写法的 tag（文案同时改成 `xxx.bum.html` / `xxx.bum` / `xxx`）；
  `make all` 0 警告，四套测试全绿。

### 修复 — 多根组件让编译器崩溃（`<use>` 展开时 `cremap[SIZE_MAX]` 越界）

- 现象：组件文件里写了**两个顶层元素**、宿主 `<use src="…">` 引用它，编译直接崩
  （MSVC/MinGW release 下是静默挂错父节点或整棵子树消失，debug 下 `_GLIBCXX_ASSERTIONS`
  报 `vector::operator[]: __n < this->size()` 失败后 `abort`，0xC0000005 / 0xC0000409）。
- 根因：展开组件时只把**第一个**顶层元素当作"组件根"替换掉，其余顶层元素的 `parentIdx`
  仍是 `SIZE_MAX`（无父哨兵），却被拿来当数组下标查 `cremap` → 越界读。
- 修复：多根组件改为用一个**合成的 `<div>` 承载所有顶层元素**，与二进制组件的链接器口径
  对齐（`binui_link` 本来就把组件内所有无父节点挂到锚点节点下）。合成容器不带组件任何属性
  （属性归宿主 `<use>`），并给一条编译期提示"有 N 个顶层元素，已用隐式 `<div>` 容器包裹"。
  单根组件的行为逐字节不变。
- 顺带：`<include>` / `<import>` 现在会明确告警"不生效，组件内联请用 `<use src=…>`"
  （以前只是一句"未知 HTML 标签"）。
- 回归测试：`test_css` component 段新增 4 条（多根编译成功 / 顶层元素一个不丢 /
  都挂在合成容器下 / 有提示）。

### 新增 — 元素级 `ui_scale="fixed"`：等比缩放窗口里保住"不许变形的 chrome"（标题栏按钮）

- 现象：`ui_scale="proportional"` 的示例最大化后，标题栏三颗控制按钮被拉成矩形（29×29 → 82×55），
  图标也跟着扁了。
- 根因：等比缩放按「新窗口 ÷ 设计尺寸」**分别**算 x/y 比例（2560/900 = 2.84、1440/760 = 1.89），
  方形元素必然被拉成矩形；字号只按纵向比例缩放，所以文字看着正常、盒子却走了形。
- 新增元素属性 **`ui_scale="fixed"`**（编译成 `SPEC_FIXED_SCALE`，复用规格段空闲位，不升格式版本）：
  该子树不参与等比拉伸，内部**统一按纵向比例缩放**（形状不变、字号一致），位置按设计留白贴最近
  的边（左右）或居中（上下），并取整到整像素避免 1px 描边发糊。`<titlebar>` 组件根节点默认带上，
  于是最大化后按钮仍是正方形、贴着右边缘、垂直居中。
- **描边也按同一个比例缩放**：边框宽度（`BinUIPaintNode::borderW`，加了一份编译期快照，每次重排
  从快照重算，不会累乘）和线段线宽（`BINUI_LINE` 的 `radius`）在固定子树里同样乘 `fs`。否则就会
  出现"放大后最小化那条横线变成 2px、最大化方框边框还是 1px"的错觉——两边的描边必须一起变粗。
  设计尺寸下 `k=1`，渲染与改动前逐像素一致。
- 回归测试：`test_elements` 9e —— 设计尺寸下 10×10；`setSize` 到 x:2 / y:1.5 后仍是正方形且按
  纵向比例变成 15×15。
- 验证：`gl_shot` 把 fontshow 渲到 2560×1440，三颗按钮 55×55、图标 17×17、留白 19/19/19/19；
  设计尺寸 900×760 下渲染与改前逐像素一致（`MIN 9×1`、`MAX/CLOSE 9×9`，留白 10/10/14/14）。
- README / components/README 补了 `ui_scale="fixed"` 的用法说明。

### 修复 — 窗口 resize / 最小化恢复后，标题栏的 ✕ 被拉成一条横线（svg 线段端点错乱）

- 现象：窗口改变尺寸（或最小化再打开触发重排）后，标题栏关闭按钮里的 ✕ 变成一条横贯窗口的
  粉色直线，图标像是被"抹开"了。
- 根因：`BINUI_LINE` 节点的 `width/height` 不是尺寸而是**终点绝对坐标**（起点 `x/y`，
  终点 `width/height`，`radius` 是线宽）。但编译期写重排规格时按普通节点处理：
  `sp.relW = baseW`（= `x2` 端点值，被当成"宽度"）。运行期 `relayout` 于是把起点算成
  "父内容盒原点 + x1"、终点直接用 `x2`（svg 局部坐标）—— 父容器一挪位置（标题栏按钮右对齐，
  窗口变宽按钮右移），起点跟着走、终点留在原处，线段就被拉成一条横线。
- 修复：编译期给线段存**起点偏移 + 终点增量**（`relX/relY = x1/y1`，`relW/relH = x2-x1 / y2-y1`），
  运行期按"起点 + 增量"还原终点，线段整体跟随父容器；`<polyline>` / `<polygon>` 展开出来的
  线段原来连规格都没有（父容器一动就完全不跟随），也一并补上。
- 顺带：`examples/flexshow` 的标题栏原来写死 `h="44"`（唯一显式指定高度的示例），29 高的按钮
  在 44 里居中到 y=7.5，图标又落回半像素；改成 `h="45"`。
- 回归测试：`test_elements` 9d 三条 —— 线段初始增量、重排后增量不变、跟随父容器。
- 验证：`gl_shot` 把 flexshow 渲染到 1300×760（走非等比重排），三颗图标 `MIN 9×1`（`L10 R10
  T14 B14`）、`MAX / CLOSE 9×9`（`L10 R10 T10 B10`）；文档图全部重出。

### 示例 — 标题栏左侧图标不再呼吸闪烁；三颗控制图标改成元素绘制（终于对齐了）

- `components/titlebar.bum.xml` 的左侧图标挂着 `animation: bui-tb-icon-pulse 2s infinite`
  （scale 1→1.15、opacity 1→0.75 来回），整条标题栏一直在"晃"。删掉动画属性和 `@keyframes`
  定义，图标从此是静止的；样式的 `.bui-tb-icon` 颜色/字号不变。
- 三颗控制按钮本来就是同尺寸（`bs` = 28×28），但**字形墨迹大小不一致**：`−` 7×2、`□` 7×7、
  `✕` 10×10，墨迹中心还差 1~1.5px —— 字形在字身框里的位置和大小由字体度量决定，换字号只能
  东拼西凑。所以**不再用字形，改成用元素画**（按钮里的绝对定位子节点，不参与命中测试：点图标
  等于点按钮，悬停底色照常从图标后面透出来）：
  - 最小化：`9×1` 的实心 div（`.bui-tb-bar`），落在按钮第 14 行；
  - 最大化：`9×9` 的空心 div，`border:1px solid #66768C`（边框落在第 10 / 18 行）；
  - 关闭：两条 `<line>` 从 (10.5,10.5)→(18.5,18.5)、(18.5,10.5)→(10.5,18.5)，`stroke-width:1`
    （`<path>` / `viewBox` binUI 还不支持，`<line>` 支持）。
- **整套尺寸改成奇数，才是真正把"半像素"从根上去掉**（这是这轮反复的最终结论）：
  - 原来 44 高标题栏 + 28×28 按钮 + 10×10 图标：按钮中线落在第 21.5 行（两行交界），1px 线
    无论放 21 还是 22 都差半像素，放 13.5 又会被 GL 渲成"半覆盖一行"（灰且偏高）；
  - 现在 **标题栏 45 + 按钮 29×29 + 图标 9×9**：按钮中线正好是第 14 行，横线就落在这一行；
    方框边框落在第 10 / 18 行，中心线也是第 14 行；左右各留 10 列。三者全部整数对齐。
  - `h` 默认 44 → 45、`bs` 默认 28 → 29（45-29)/2 = 8，按钮垂直位置仍是整数。
- 脚本量像素（`build-mingw/tmp/measure_titlebar_icons.py`：找按钮底色 bbox，再量图标墨迹四边留白）：
  GL 真机截屏三颗都是 `L10 R10 T14 B14`（最小化）/ `L10 R10 T10 B10`（最大化、关闭），`dx=dy=0`。
  GDI 离屏的关闭图标因为 `LineTo` 取整是 `L11 R9 T11 B10`（偏右下 1px），只影响静态文档图。
- 半透明叠加验证（`build-mingw/tmp/overlay_buttons.py`，三颗按钮 42% / 只叠图标 62%）：圆角底完全
  重合；横线正好穿过 ✕ 的交叉点；▢ 与 ✕ 的墨迹范围逐像素重合 —— 之前那条"偏高半像素"的横线
  在叠加图里能直接看出来，改完就没有了。
- 组件参数 `bsym` / `bsymmin`（字形字号）随之删除：图标几何写死在按钮的 29×29 坐标系里，
  `bs` 仍是按钮边长（**改 `bs` 时要同步改图标坐标**，文件里注释写了）。
- `test_css` 的 titlebar 用例重写：三颗按钮同宽高、按钮文案为空（不再有字形）、图标子节点存在
  且都不可交互、**三颗按钮中心用 `hitTest` 都命中按钮自己**（图标不挡点击）。
- 文档图按新标题栏重出：`flexshow` / `buttonshow` / `features` / `animshow` / `fontshow` /
  `std-components` 走离屏渲染；`svgdemo` / `cubedemo` / `texturedemo` 重新真机截屏。
- 验证：`gl_shot` 离屏 GL 渲染与真机窗口在未改动区域逐像素一致（maxdiff 0），所以半像素修正
  是拿真机同一个光栅器验的；相隔 1.6s 两帧标题栏 diff 最大 0（图标不再动）；悬停最大化按钮底色变
  `#DCE5F2` 且图标仍在最上层；点最大化窗口铺满 2560×1440、点关闭进程退出（点击路由没被
  图标挡住）；Tab 焦点环正常套在按钮上。
- ⚠ 组件在**应用启动时**才编译加载：改完 `titlebar.bum.xml` 要重启示例进程才能看到新像素
  （跑着的旧实例会一直是旧的 ✕ 和横线）。

### 修复 — 鼠标点输入框也画出"编辑中"蓝环

- `focusRingOf()` 原来只看 `edit_.active`：一旦进入编辑态就返回 `Editing`，不管编辑是键盘
  Enter 进来的还是鼠标点进来的。于是点一下输入框，周围立刻套一圈 2px 蓝框 —— 和"焦点框
  只代表键盘在操作"的设计矛盾。
- 现在区分来源：`focus(nodeId, viaKeyboard)`，只有 `activateKeyboardFocus()`（Enter）传
  `true` 才画编辑环；鼠标点击 / 宿主直接调 `focus(id)` 进入的编辑有光标、能打字、`:focus`
  样式照旧，但不画环。退出编辑时同源判断：键盘进来的回到虚线待编辑环，鼠标进来的不画环。
- 实测（fontshow 普通输入框，客户区 900×760）：鼠标点进去 0 个蓝像素（光标在）；
  Esc 退出后 0 个；同一框再按 Enter 进来 850 个 `RGB(62,139,255)` 像素、边框两像素宽 ——
  说明只是"鼠标进来的不画"，不是把编辑环关掉了。
- `test_elements` 9c 补 6 条：鼠标点击进编辑且有 `:focus` 但不画环、Esc 退出后仍不画、
  键盘重新 Enter 后编辑环保留。文档与 C ABI 注释同步（`docs/keyboard_focus.md` /
  `docs/bindings.md` / `README.md` / `include/binui_c.h`）。

### 文档 — 尚未实现的下潜/上浮方案改称「层级焦点导航」

- 原来叫 "z 序焦点"，容易和窗口 Z 序（`binui_win32_foreground.h` 里的置顶）混淆；它讲的其实是
  焦点在元素层级树里的下潜/上浮，和渲染 Z 序无关。
- 改名范围：`docs/keyboard_focus.md` 末节标题改为「未来开发计划：层级焦点导航（下潜 / 上浮，未实现）」，
  `docs/README.md` 索引同步；`[0.3.0]` 里那条记录也换成新名字（语义没变，仍未实现）。

### README 顶部的示例图换成当前示例；补 flexshow / buttonshow 截图

- 顶部三张图原来是「内置默认布局 / features / animshow」：第三张标着"组件化（`<use>`）"却放的
  动画截图，而"默认布局"只是引擎兜底布局（不是示例），观感像很久以前的东西。
- 换成 8 组示例里最有代表性的三张：**flexshow**（自适应布局）、**buttonshow**（按钮与状态样式）、
  **animshow**（动画与过渡），全部用 `tools/offscreen_render` 从**当前源码**重出。
- 新增 `docs/img/flexshow.png`、`docs/img/buttonshow.png`；顺手重出 animshow / 组件库预览；
  `docs/README.md` 图片表补上这两行，并加了"改完示例 UI 怎么重出这些图"的命令清单
  （`make tools` 后一张一条）。
- **3D / 纹理 / SVG 画布三张改成 GL 真机截屏**：cubedemo 的立方体、texturedemo 的程序化纹理、
  svgdemo 的 canvas 频谱都是应用在 `drawFn` 里逐帧画的，离屏渲染只能得到空画布 —— 之前
  `docs/img/cubedemo.png` 就是一张"画布空白"的图，README 顶部也补了第四格 3D 线框（真机截屏并注明）。
- flexshow 状态栏的占位文案从「窗口 —」改成默认窗口尺寸（运行时仍由 resize 事件覆盖），
  离屏图/截图不再出现"—"。
- 修默认布局（`GenerateDefaultBinUI()`）按钮文字变左对齐：手写节点漏了 `BINUI_TEXT_CENTER`，
  后端改成按 flags 画文字之后就露出来了；`test_css` 加一条守护（手写布局按钮也必须居中）。

## [0.3.0] - 2026-11-10

首个正式按 `MAJOR.MINOR.PATCH` 发布的版本（此前只有 0.0.1 / 0.0.2 两个归档）。
版本号规则见 [CONTRIBUTING.md](CONTRIBUTING.md) 的「版本号与发布」；二进制布局格式号仍独立递增（当前 v14）。

### 修宽输入框的虚线焦点环只画了一半；`strokeRounded` 真正支持线宽

- `flex:1` 拉满窗口的输入框（flexshow 的搜索框，800+px 宽）周长超过虚线段数上限，
  只画到左上→右上→右下就停了，表现为"只有上边和右边有虚线"。现在上限从 96 提到 256，
  且超限时按周长**等比拉长实线/间隔**（而不是画一半就停），任何宽度都整圈画完；
  单测覆盖 1200px 与 3000px 宽的环（`BuildDashedRoundRing`）。
- GL/D3D 的 `strokeRounded` 之前是 `(void)thickness` —— 参数被直接丢掉，所有描边都只有
  1px（"2px 编辑环"实际是 1px）。现在按厚度画多层同心圆角环，编辑环真的是 2px
  （真机取样：`RGB(62,139,255)` 连续两像素）；其它传 1px 的调用点行为不变。
- 验证：flexshow 默认窗口 Tab 到搜索框 → 虚线整圈；fontshow 点输入框 → 2px 蓝环；
  `make all` 0 警告，`make test` 四套全绿。

### 键盘焦点系统重做：WASD 移焦点、方向键滚页面、回车两段式、焦点环 2 秒自动消失

以前方向键在可交互节点间移动焦点、焦点环画上就一直在、8 个示例各自 `switch (vk)` 复制一份
编辑键处理（`features` 干脆没有编辑分支）。现在键盘输入只有一条入口
`renderer.handleKey(vk, mods)`，焦点 / 滚动 / 编辑的优先级由渲染器统一决定：

```cpp
app.setKeyFn([&](int vk) {
    uint32_t mods = 0;
    if (::GetKeyState(VK_SHIFT)   & 0x8000) mods |= BinUIRenderer::KEYMOD_SHIFT;
    if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= BinUIRenderer::KEYMOD_CTRL;
    if (::GetKeyState(VK_MENU)    & 0x8000) mods |= BinUIRenderer::KEYMOD_ALT;
    const auto kr = renderer.handleKey(vk, mods);
    for (uint16_t id : kr.changed) invalidateNode(id);
    if (!kr.handled && vk == VK_ESCAPE) app.requestExit();   // 库不消费 Esc
});
```

三种状态与按键：

| 状态 | 键 | 行为 |
|---|---|---|
| 焦点控制 | `WASD` | 上/左/下/右移动焦点（Tab 仍按文档顺序循环） |
| 焦点控制 | 方向键 | **滚页面**：离焦点最近的滚动容器；焦点不在滚动容器里时，滚视口内面积最大的那个 |
| 焦点控制 | PgUp/PgDn、Home/End | 按视口 0.9 倍翻页、到顶/到底 |
| 焦点控制 | Enter | `<input>` 进入文本编辑；`<video>` 播放/暂停；其余派发 `CLICK` |
| 焦点控制 | 空格 | 有焦点时同 Enter；没有焦点时向下翻页（浏览器语义） |
| 文本编辑 | Enter / Esc | 提交并退出编辑，**回到焦点控制** |
| 文本编辑 | Tab / Shift+Tab | 退出编辑并跳到下一个 / 上一个控件 |
| 文本编辑 | 退格/删除/左右/Home/End | 光标与选区（字母归输入法，**WASD 不抢键**） |

焦点环不再是"一条常亮白线"，而是把状态画出来（`renderer.focusRingOf(id)` →
`None/Dashed/Solid/Editing`）：

| 状态 | 环 | 说明 |
|---|---|---|
| 待编辑（**只有输入元素**） | **虚线** 1px + 底色对比色 | 键盘选中输入框、还没按 Enter；`:focus-visible` 语义：鼠标点击不画、2 秒无操作自动熄灭 |
| 普通焦点（按钮等） | **实线** 1px + 底色对比色 | 键盘选中非输入元素；同样 2 秒无操作自动熄灭 |
| 编辑中（输入元素） | **实线** 2px + 强调蓝 `#3E8BFF` | 正在编辑的输入框；常亮（编辑是持续状态，不能自己消失） |
| 无 | 不画 | 鼠标操作 / 超时 / 窗口失焦 / 没有焦点 |

虚线只用来标记输入元素的"待编辑"状态，不是通用焦点样式 —— 否则按钮上也套虚线，满屏看起来
像"所有东西都在等你编辑"，和禁用/占位之类的虚线语义也容易混。

线型 + 颜色双编码：只靠颜色区分对色弱用户不友好，虚线/实线一眼能分。虚线沿圆角矩形
路径按弧长采样（`binui_paint.h` 的 `BuildDashedRoundRing`，三个后端共用几何、只负责 fillLine）。

Enter/Esc 的语义也按状态分开、不混：待编辑态 Enter = 激活（输入框进编辑 / 其余 CLICK）；
编辑态 Enter = 确认（提交并退出，单行输入没有"换行"，要"发送"的宿主在 `INPUT`/`FOCUS`
事件里做）；编辑态 Esc = 退出编辑回到待编辑态。焦点环熄灭不需要宿主定时 ——
`stepAnimations()` 会在熄灭的那一帧把焦点节点放进返回值，宿主照常 `invalidateNode()` 擦掉。
窗口失焦时宿主调 `renderer.hideFocusRing()`。

#### 中文输入法：非编辑态把 IME 摘掉，否则 WASD 一个键都收不到

这一步不做的话上面的 WASD 在中文环境里等于没接。中文输入法默认挂在窗口上，按 `d` 时
Windows 会把 `WM_KEYDOWN` 的 `wParam` 换成 `VK_PROCESSKEY`（229）交给输入法组字。真机日志：

```
keydown vk=229 composing=0     ← 按 d，输入法把键吃掉
keydown vk=229 composing=1
```

于是外壳加 `setImeActiveFn`：返回 false 时把 IME 上下文从窗口上摘下来（`ImmAssociateContextEx(hwnd, nullptr, 0)`，
摘之前先 `cancelImeComposition()`），返回 true 再挂回默认上下文（组字/候选框/光标定位照旧）。
每帧在 `renderFrame()` 里同步，宿主只需一行：

```cpp
app.setImeActiveFn([&] { return renderer.isEditing(); });
```

8 个示例与 C ABI 都接了；不接 = 保持老行为（IME 常挂），此时中文输入法下 WASD 无效、方向键照常。
另外 `BINUI_IME_LOG=1` 的日志现在会打每一条 `keydown vk=...`，这类"键被谁吃了"的问题一眼能看出来。

组字中的 Esc 归输入法：实测微软拼音组字时按 Esc 只发 `WM_IME_ENDCOMPOSITION` 取消候选、
不再放行 `WM_KEYDOWN`，编辑态原样保留；对会放行 Esc 的输入法，外壳把"组字结束后 200ms 内"
到达的第一个 Esc 吞掉一次（`composeEndMs_`），再按一次才退出编辑 —— "Esc 取消拼音"和
"Esc 退出编辑"不会互相误伤。

实现要点：

- `handleKey` 返回 `{ handled, changed }`：`handled` 只表示"库消费了这个键"（宿主别再用它当
  快捷键），`changed` 是焦点环/光标/滚动容器的变化集合，宿主按它做局部重绘。
- 方向键的目标容器 `scrollTargetNode()`：先沿焦点节点向上找最近的 `overflow:auto/scroll`
  祖先；没有则取视口中心所在的可滚动容器；再没有取视口内面积最大者。只有真有两轴溢出的
  容器才算数，否则方向键不被消费（宿主可以自己用）。
- 鼠标点击只切 `:focus` 状态（`commitFocus(id, false)`），不点亮焦点环；环正亮着时点一下
  也会立刻收掉（`release()` 里清 `focusRing_`）。`setKeyboardFocus` / `moveKeyboardFocus` /
  `cycleKeyboardFocus` / `handleKey` 都会亮环并重置 2 秒计时。
- 每节点环态由 `focusRingOf(nodeId)` 决定（`None/Dashed/Solid/Editing`）：输入元素待编辑给
  `Dashed`、其余元素给 `Solid`、正在编辑的输入框给 `Editing`。三个后端只负责画：
  实线用 `strokeRounded(2px)` / 四条 2px 线，虚线用 `binui_paint.h` 的
  `BuildDashedRoundRing` 沿圆角路径按弧长采样出线段再 `fillLine`。编辑环颜色统一取
  `BinUIRenderer::kFocusRingEditColor`（#3E8BFF），不会三端各调一个蓝。
- 编辑键不再由宿主复制：`features` 的输入框以前按退格/方向键无效，现在和其它示例一致。
- C ABI 也切到同一条路径（剪贴板四件套仍留在宿主侧，因为系统剪贴板 I/O 在平台层）。

验收：

- `tests/test_elements.cpp` 新增 9c 节 35 条用例：WASD 移焦点、方向键不移动焦点且无滚动容器时
  不被消费、回车激活/进出编辑、编辑中 WASD 忽略、编辑中 Tab 换控件、方向键滚焦点容器、
  PgUp/PgDn/Home/End、无焦点时空格翻页、焦点环三态（输入框待编辑 `Dashed` / 按钮 `Solid` /
  编辑中 `Editing`，Esc 后回到 `Dashed`）、虚线环几何（线段数 / 端点不出界 / 覆盖周长）、环超时熄灭并回报该节点、
  鼠标点击只设焦点不亮环。
- `make all` 0 警告；`make test` 四套全绿；`check_examples` 11 个 UI 0 问题。
- 真机键盘验收（截图逐帧比对，均值像素差）：
  - `buttonshow`：WASD 后出现**虚线**焦点环 → 2.3 秒后消失 → 再按又出现；鼠标点按钮只出行状态、
    不亮环。
  - `flexshow`：连按 4 次方向键，右侧列表区差 19.1、左边图表区 0.0（焦点没动、页面只滚了列表）；
    再按 `a` 焦点环出现在第一颗可聚焦节点。
  - `fontshow`：点输入框进编辑 → 输入框上是**实线蓝环** + 光标 → 数字 “123” → 回车退出
    （环变虚线、光标消失）→ 按 `x` 无变化（编辑外的字母被忽略）→ 再回车进编辑 → 按 `4` 插入。
    组字 “ni'hao” 时按 Esc：只取消候选，编辑态与实线蓝环保留；再按一次 Esc 才回到虚线待编辑态。
    `BINUI_IME_LOG=1` 的日志同步显示 `ime detached` / `ime attached` 跟着编辑态切换。

层级焦点导航（非输入框按 Enter 下潜到子元素、多层可下沉，`Delete` 上浮）按 master 的要求
先记下设计，见 [`docs/keyboard_focus.md`](docs/keyboard_focus.md) 末节，本次未实现。

### 示例 — 标题栏的 − □ ✕ 三颗按钮全部接线，固定布局示例补 `ui_scale="proportional"`

`components/titlebar.bum.xml` 一直只有外观，动作归宿主；之前 8 个示例里：
`buttonshow` / `flexshow` 完全没接，其余几个是按**字形文本**匹配（`n.text == "−"`）接的 —— 改个字号、
换成别的图标就悄悄失效，而且它挂在 release 回调里，和 CLICK 各走一半。现在统一改成**按组件声明的
id** 接（`idmin`/`idmax`/`idclose` 默认 `win-min`/`win-max`/`win-close`）：

```cpp
const uint16_t idWinMin   = renderer.findNodeByName("win-min");
const uint16_t idWinMax   = renderer.findNodeByName("win-max");
const uint16_t idWinClose = renderer.findNodeByName("win-close");
renderer.setEventCallback([&](const Event& ev) {
    if (ev.type != EventType::CLICK) return;
    if (ev.nodeId == idWinMin)   { app.minimize(); return; }
    if (ev.nodeId == idWinMax)   { app.maximize(); return; }   // 最大化 / 还原
    if (ev.nodeId == idWinClose) { app.requestExit(); return; }
});
```

顺带解决"最大化之后缩在左上角"：固定像素布局的 7 个示例加 `ui_scale="proportional"`，
按 □ 最大化时整棵节点树按「新窗口 ÷ 设计尺寸」缩放，而不是留一片空白。`window_resize="disable"`
仍然生效（不能拖边），两者不冲突。

验收（真机，逐个示例点按钮）：

| 示例 | − 最小化 | □ 最大化 / 还原 | ✕ 关闭 |
|---|---|---|---|
| animshow / buttonshow / cubedemo / texturedemo | iconic=True | 2560×1440 → 原尺寸 | 进程退出 |
| features（900×932，纵横比不同） | iconic=True | 2560×1440 → 原尺寸（x/y 分别缩放） | 进程退出 |
| fontshow / svgdemo / flexshow | iconic=True | 2560×1440 → 原尺寸 | 进程退出 |

`make all` 0 警告；`check_examples` 11 个 UI 0 问题。

### 修复 — column flex 里 `flex:1` 的"撑满剩余高度"塌成内容高

`<div style="display:flex;flex-direction:column">` 里写 `flex:1` 的子容器，高度**位置**按剩余空间
算对了（`720−44−48−28=600`），节点**高度**却被它自己的"未声明高度 → 按内容撑开"逻辑改回内容高：

```
id=15 #main   y=92   h=40     ← flex:1 的列内项，高度没被撑开
id=23 #status y=692           ← 但排位置时按 #main=600 算
```

于是"撑满剩余高度"的容器塌成一条、后面的兄弟压上来（重叠），`flex:1 + overflow:auto` 的滚动视口
也变成 0 高（永远不出滚动条）。修法：column 父级下的 `flex:N` 主项，高度由父级分配，**不许**再用
内容高度覆盖（编译器与渲染器 `relayout` 两处同口径；`autoH` 也一并置 0，否则运行期重排又会塌回去）。

顺带修了一条同源问题：**视口 = 窗口**。重排后根节点不再被内容撑大（以前内容比窗口高时根会"长"出
窗口，脏矩形/命中/绘制全落到窗口外，自适应布局一缩窗口就错位）。

回归用例（`tests/test_css.cpp`）：`flex column fill`（列内 flex:1 容器 = 528、嵌套 flex:1 = 528、
兄弟位置跟随、`setSize(400,400)` 后 = 328）+ `flex scroll viewport`（`flex:1 + overflow:auto`
视口高 = 剩余 260 且带 `BINUI_SCROLLABLE`）。

### 示例 — 7 个固定布局示例固定宽高 + 新增自适应布局示例 `examples/flexshow`

- 固定像素布局的 7 个示例统一在 `<app_window>` 上写 `window_resize="disable"`：以前窗口能拖边，
  一拖就错位（绝对定位不会跟着重排）。
- 新增 `examples/flexshow`：整份 UI **没有一个绝对定位**，全靠百分比 / `flex:1` / `min·max-width` /
  `width:content` / `flex:1 + overflow:auto` 撑开；状态栏由 C++ 在 `RESIZE` 事件里写窗口尺寸，
  一眼看出"重排用的是新尺寸"。默认 1100×720，实测 820×560 / 1100×720 / 1400×900 三档都正确重排
  （卡片宽度、柱状图百分比高度、列表滚动视口全部跟随，窗口缩小到 820 时第三张卡被 `min-width:240` 钳住）。
- 顺带补了这个示例的**滚轮接线**：`overflow:auto` 只负责画滚动条与裁剪，滚轮要外壳自己
  `renderer.scrollAt(x, y, dy)`（拖条子走的是 press/setHover 那条路，滚轮是另一条）。
  第一版漏了它，表现就是"滚动条画出来了、滚轮和拖条都不动"。

### 性能 — 拖窗口实时重排：按刷新率出帧、动画不停、空闲不再吃满单核

- **拖拽期动画不再冻结**：resize 循环以前只调 `renderOnce()`，`update_` 被跳过，拖边时所有动画停住；
  现在每帧都走 `renderFrame()`（`update_ + 重排 + 呈现`）。
- **按显示器刷新率出帧**：以前拖拽循环是"每轮重排 + 整屏呈现 + `Sleep(1)`"，一秒几百帧、CPU 打满、
  消息队列堆在后面，反而一顿一顿；现在用 `GetDeviceCaps(VREFRESH)` 定额（拿不到按 60Hz），
  边框 `SetWindowPos` 仍立刻跟手，内容按刷新率出帧。GL 与 D3D11 两个外壳同构。
- **空闲节流**：`pump()` 以前只要有脏区就满速出帧（一个有常驻小动画的界面能吃掉 ~80% 单核），
  现在按同一套帧时钟等到下一帧点再出帧、其余时间睡在 `MsgWaitForMultipleObjectsEx` 上。
  实测 `buttonshow`（标题栏图标常驻呼吸动画）空闲 CPU **82% → ~12%**，6 秒内 413 帧（≈69fps，含启动）。
- GDI 呈现路径把 `StretchDIBits` 换成 1:1 的 `SetDIBitsToDevice`（不做缩放采样）。
- 新增 `BINUI_PERF=1`：每 600 帧 + 退出时打印分段耗时（重排 / 绘制 / 呈现，呈现再拆
  解析 / DIB / 读回 / 贴屏），拖拽结束打印本次拖拽的帧数与平均/最大帧耗时。

### 示例 — `buttonshow` 补上标准标题栏，7 个示例现在外观统一

之前 6 个示例（features / animshow / fontshow / svgdemo / cubedemo / texturedemo）都用
`components/titlebar.bum.xml`，只有 `buttonshow` 自己拿两个 `<span>`（24px 大标题 + 副标题 +
右上角一颗 chip）拼了个"头部"：窗口没有可拖动的标题栏、右上角没有窗口按钮，和其余示例摆在一起
一眼就看出不是一套东西（`examples/README.md` 里"都用标准标题栏"那句当时是假的）。

改成和别的示例同构：

```html
<use src="../../components/titlebar.bum.xml" x="0" y="0" w="1020" icon="▣" iconw="22"
     title="按钮样式一览" sub="语义 / 尺寸 / 圆角 / 底色 / 状态 / 形态 —— 全部是 CSS，没有一行绘制代码"/>
```

- 原来那个 24px 大标题与副标题被标题栏的 `title` / `sub` 取代，`.title` / `.sub` / `.chip`
  三条样式随之删掉（不留死样式）；chip 里的"主色 #3E8BFF"由副标题覆盖，信息没丢。
- 内容整体上移 28px：卡片序列从 `84 / 192 / 300 / 476 / 618` 变成 `56 / 164 / 272 / 448 / 590`，
  首块距标题栏 12px、末块距底 20px，窗口高度 712 → 684（和其他示例"标题栏 44 + 内容从 56 起"一致）。

验收：

- 离屏渲染：把改前 `y=84..692` 与改后 `y=56..664` 两块裁出来逐像素比对，**完全一致**
  （说明只是整体位移，没有别的重排）；标题栏条带与其余 6 个示例逐像素同规格
  （白底、`y=43` 处 `#E6EDF7` 分割线、右端 `#FFE9EC` 关闭钮）。
- 真机 GL：标题栏可见；从标题栏按下拖动，窗口矩形 `(770,358) → (870,398)`，位移正好等于鼠标
  位移 `(+100,+40)`；悬停最小化钮，底色 `#EEF3FA → #DCE5F2`（走 `.bui-tb-btn:hover`）；
  点「主操作」按钮，状态行更新为"最后点击：主操作 / 点击次数 1"。
- `check_examples` 10 个 UI 0 问题；`build-mingw/bin/examples/` 下 7 份 main.html 与源树哈希一致。

### 重构 — `examples/features` 的按钮组件去掉多余的根 div

`examples/features/button.bum.xml` 以前写成 `<div class="cmp-wrap"><button …></div>`：这层 div
与按钮完全重合（宿主 `<use x y w>` 落到它身上，按钮再 absolute 定位到 0,0），除了给按钮
多一个定位基准与一个祖先之外什么都不做。它还带来一个隐性瑕疵——宿主传的 `bg` 参数会作为属性
落到 div 上，div 画出一个**方角**底色块，盖住按钮的圆角（离屏渲染里那颗绿按钮四角是方的）。

改成**组件根就是 button**：位置/宽度由宿主 `<use x=… y=… w=…>` 给，参数只写内联 style。
`docs/components.md` 的示例同步，并补一句"别为了有个容器再套一层 div"。

顺带把 Makefile 里 features 的规则补上 `examples/features/button.bum.xml` 依赖 —— 以前只认
`main.html`，改组件不会重新拷到 `bin/examples/features/`，跑起来还是旧组件。

验收：8 个示例离屏渲染逐像素比对，只有那颗 `bg` 按钮的四个角变了（方角 → 圆角），其余全同；
`check_examples` 10 个 UI 0 问题；`make test` 四套全绿。

### 修复 — 组件里的按钮 hover 放大时被"关在盒子里"（裁剪链按每个祖先矩形无边裁）

组件自己的根 **`div`** 与按钮**等大**（`examples/features/button.bum.xml` 里就写着
`<div class="cmp-wrap" style="height:40">` 包着按钮，`<use>` 展开后根 div 就是
190×40，和按钮一模一样）。三个后端绘制节点时，
除了自身与脏矩形之外，还会**无条件**沿祖先链逐层裁剪：

```cpp
while (draw) { ... gfx.pushClip(par.x, par.y, par.width, par.height); }   // 以前
```

于是组件按钮 `:hover { transform:scale(1.04) }` 放大出来的那圈（每边 3.8px）被这个等大的根 div
切掉：按钮看着还是原尺寸、四角变成硬边 —— 就是"被困在一个容器里放大，超出的部分被隐藏"。

CSS 语义里只有 `overflow:auto/scroll/hidden` 的容器才裁子元素。修法：

- 渲染器新增 `clippingAncestors(nodeId, out)`：从近到远返回**带 `BINUI_SCROLLABLE`** 的祖先
  （编译期由 `overflow:auto/scroll/hidden` 标出），普通 div（含组件根 div）不入链。
- 三个后端（GL、D3D11、GDI）的绘制循环都改成调它，删掉各自手写的祖先遍历；
  每帧复用同一个 `std::vector<uint16_t>` 缓冲，不产生逐节点分配。
- 祖先不可见仍然跳过绘制（`hidden[]` 预计算已经覆盖，这里只补裁剪容器自身的可见性判断）。

验收：

- 真机：悬停「字体与输入法 / 组件化」卡片里的「普通按钮」—— 像素量出按钮从 190px 变成
  195–197px（`190 × 1.04 = 197.6`，两侧各带 1px AA），圆角完好；修复前恒为 190px 且是硬边。
- 静态回归：8 个示例 + 标准组件预览的离屏渲染，改造前后**逐像素零差异**（说明没有哪份布局
  是靠"祖先裁剪"苟着的）。
- `tests/test_elements.cpp` 新增 `suite_clip_chain`：`overflow:hidden` 的祖先必须在链上，
  普通 div 祖先必须不在链上。三个后端现在共用同一个判定，这条用例钉住的就是后端的行为口径。
- `make all` 0 警告，`make test` 四套全绿，D3D11 后端 `-fsyntax-only` 通过。

### 修复 — 按钮 hover 放大再缩回，边缘留下一道 1px 残影

按钮没写 `:hover` 样式时，引擎给的内置悬停效果是**放大 1.05 倍 + 提亮 12%**（`refreshNodeStyle`），
而且**没有过渡**：缩放一帧跳上去、一帧跳回来。缩回那一帧的失效矩形按"当前值"算正好等于布局矩形，
放大时在最外沿画过的像素就没人擦 —— 在新布局里表现为「按钮文字居中」这种**宽按钮**右边留一道
1px 竖线（152px 宽放大 1.05 会多出 3.8px，超过 `invalidRect` 的 `pad = 2`，所以只有宽按钮露馅；
窄按钮的 1px 溢出被 pad 顺手盖住了，这也是之前一直没发现的原因）。

根因有两层，缺一不可：

- **没有"扫过的极值"**：`invalidRect()` 只并当前缩放/位移的包围盒。新增 `animExtent_`
  （每节点记缩放 min/max、位移 min/max），`computeAnimResult()` 每帧更新，`invalidRect()`
  一并并进去 —— 缩回那一帧就仍然覆盖放大时画过的范围。布局重载时清空。
- **内置悬停的缩放进不了 `AnimResult`**：`drawScale()` 有"无过渡时回落到 `floatTargets`"的兜底，
  但 `computeAnimResult()` 没做同样的折入，所以这种"无过渡"的缩放从来没被记录过，
  上面那个极值也就永远是空的。现在 `computeAnimResult()` 末尾按同样的口径兜底
  （scale / translate），绘制结果不变，但极值能记上了。

回归用例：`tests/test_elements.cpp` 新增 `suite_hover_dirty` —— 200px 宽按钮、悬停后移开，
断言"缩回那一帧的失效矩形仍覆盖放大过的范围"。**这条用例在打补丁前是红的**（就是上面那条 FAIL），
打完变绿。

真机验收（`字体与输入法` 示例，像素比对）：光标移开后再截图，故按钮右侧 868~879 列与启动时
**逐像素相同（差异 0）**；补丁前同一位置差 19 个像素，即那道 1px 竖线。

### 重构 — `imeshow` 并进 `fontshow`：示例收成「字体与输入法」一组

输入法和字体共用同一条度量链（组字字体跟字号、光标高度跟行高），分成两个 demo 反而要来回切窗口看，
而且 `imeshow` 是全仓库唯一的深色底示例，和"统一浅色主题"的约定本来就打架。

- `examples/fontshow` 扩成**字体与输入法**：①字号阶梯 ②字重/斜体/装饰 ③行高 ④混排与符号不变，
  ⑤ 换成输入法区（24px 大号输入框 + 13px 普通输入框 + 上屏回显 + 码点/字符数统计），
  窗口 900×640 → 900×760。
- 外壳并入 `imeshow` 的 IME 接线：`setImeCaretFn`（组字窗与候选框贴光标）、`setCharFn`
  （只有选词上屏的字符才进来）、组字期间按键不抢、编辑键（退格/方向/Home/End/回车/Tab）。
  另外补了热重载后的节点 id 重解析（换树后 `findNodeByName` 要重跑，否则回显会写到空气里）。
- 删除 `examples/imeshow/`；`Makefile` / `CMakeLists.txt` 的示例列表 8 -> 7。
- 文档：README 示例表与运行清单、`examples/README.md`、`docs/README.md`（图片说明）、
  `docs/app_layout.md`、`tests/README.md`（IME 手测入口改到 `examples/fontshow`）同步；
  `docs/img/fontshow.png` 按新布局重新生成。

验收（真机 + `BINUI_IME_LOG=1`）：

- 点 24px 输入框打 `nihao`：组字串 `n` 内联在光标处、候选框（1 你 2 呢 3 那…）贴在输入框下方；
  日志 `ime place (45,600) h=32 reset=1` —— 光标高 32 = 24px 字号的行高，与节点几何吻合。
- 空格选词：`[fontshow] char: 你  U+4F60`（`resultstr bytes=2 cps=1`），输入框变「你」，
  回显行显示"最后收到的字符：你 / 码点：U+4F60（3 字节 UTF-8） / 输入框① 3 字符 · 输入框② 0 字符"。

### 重构 — 测试与示例收拢：9 个测试二进制合成 4 个，产物按用途分目录

一根鸡毛一个 `test_xxx.exe` 的老问题积到现在：`tests/` 里躺着 9 个互不相干的可执行文件，
`build-mingw/bin` 里测试、示例、工具、以及几份**既没有源码也没有构建规则**的残留产物
（`galgamechat` / `cssshow` / `singlewindow` / `multiwindow` / `videodemo` / `minwin` /
`ime_probe` …）混作一团，双击有的还打不开。

- `tests/` 改成**按主题**分四个套件，代码逐字保留（只做结构重排：每个旧文件的 `main()` 包进
  `namespace suite_xxx` 并改名为 `run()`，新 TU 的 `main()` 逐个调用并汇总失败数）：
  - `test_css` ← 原 `test_compiler`（CSS 属性 / 选择器 / 伪元素 / 编译器告警）
  - `test_elements` ← 原 `test_binui` + `test_renderer` + `test_renderer_real` + `test_paint`
    + `test_anim` + `test_hot_reload`（元素 / 布局 / 命中 / 交互 / 绘制几何 / 动画 / 热重载）
  - `test_font` ← 原 `test_font`，并补上**输入法（IME）**用例
  - `test_capi` 不变（C ABI 契约）
  合并时用输出逐条比对确认用例没丢：元素组 359 条、CSS 组 373 条，两侧完全一致。
- `test_font` 新增 IME 桥接用例：UTF-16 索引 → UTF-8 光标字节偏移、全角字、代理对（emoji）、
  上屏插入（`insertText`）、失焦提交（`blur`）、`setCaret` 夹到字符边界。这段不依赖字体，
  没有系统字体的环境也会跑（以前整支测试会直接 skip 掉）。
- **修 `Utf16IndexToBytes()` 把 UTF-16 代码单元当成码点数**
  （`src/renderer/binui_renderer.cpp`）：补充平面字符（emoji、罕见汉字）在 UTF-16 里是
  **代理对、占 2 个单元**，旧实现按 1 个算 —— 输入法给的 `GCS_CURSORPOS` 索引在含 emoji 的
  输入框里逐字符偏一位，索引还可能落进代理对中间，组字窗就挂在半个 emoji 的位置上。
  现在按单元推进，落在代理对中间时夹到该字符起点（鼠标定位、双击选词走的是同一个转换）。
- 产物按用途分目录，Makefile 与 CMake 保持一致：
  - `build-mingw/bin/tests/` —— 四个套件
  - `build-mingw/bin/examples/` —— 8 组示例（exe + 同名 UI 子目录）
  - `build-mingw/bin/` —— 库 / DLL / 命令行工具
  示例 UI 里的 `../../components/xxx.bum.xml` 相应同步到 `bin/components/`；另加 `make examples`
  只编示例、`make test` 只编跑测试。
- 删掉 7 个旧测试源文件、`build-mingw/bin` 里的残留产物，以及 tests/README 里说明的边界
  （哪些必须开窗手测：组字窗/候选框定位、组字期间点击被 IME 吞掉、GL/D3D 真实像素）。
- 文档同步：新增 [`tests/README.md`](tests/README.md)（套件划分 + 能测/不能测 + 合并记录）；
  `README.md` / `examples/README.md`（6 组 → 8 组，路径改到 `bin/examples/`）/
  `tools/README.md` / `docs/{README,app_layout,components,pixels/README}.md` 里的失效路径一并修正
  （`examples/cssshow`、`examples/singlewindow`、`examples/components` 这些早就搬走/删掉的老路径）。

### 修复 — 圆角盒子的边框只画到顶部约 64px 就断了

`BuildBorderBands()`（`include/binui_paint.h`，GDI / OpenGL / D3D11 三个后端共用）的圆角分支
逐行生成边框条带，每行左右各 push 一条；合并逻辑只看 `out[n-1]`，而左右两条是**交替** push 的，
永远合不上 —— 条带数退化成 O(高度)，撞到 `kMaxPaintBands`（128）就在第 64 行截断。

表现：所有带 `border` 的圆角盒子，左边框/右边框只有**上半截**，下面整段没有；截断位置与盒子
高度无关（圆角 14 的卡片实测 52 行，加上顶部圆角恰好 64），所以看起来像"固定长度的一条 1px 线"，
极易误判成刻意画的装饰线。三个后端 + 所有示例都中招。

- 合并改成**按边记住上一次的条带下标**（上/下/左/右各一个），直边段逐行延伸成一条高条 ——
  条带数只跟半径有关（≈4r+常数），与盒子高度无关。
- 边框条带上限单独提为 `kMaxBorderBands = 512`（半径大的胶囊也装得下）；装不下时**退回直角
  四条**，宁可方角也不能整段缺边。
- `tests/test_elements.cpp` 的绘制几何小节补回归用例：600px 高卡片的边框必须画到底边、盒子拉高一倍条带数不变、
  半径超过上限时退回四条、直角仍是四条。
- 顺带修了圆角**转角比直边更粗更亮**的问题：1px 等宽边框的圆角以前是逐行矩形拼的（45° 处每行
  1.4~2px、只有亚像素混色没有 AA），而紧挨着的填充是 AA 的，转角看起来发虚发粗。现在这种边框的
  圆角改走九宫格 AA 圆环（模板覆盖率也修成了满值，直边颜色和作者写的一致），直边不再需要额外补矩形。
  GL 与 D3D11 一致；GDI 画布后端没有圆环原语，仍走逐行条带（已经不截断）。
- **焦点环画成了灰线**：圆环模板以前是"骑在外侧边界上"画的，靠内那半个像素只有 50% 覆盖率 ——
  文档写的是"白色焦点环"，实际渲染出来是 #858990 的灰线，浅色主题上（白底白环）完全看不见。
  现在环整体挪到边界内侧满覆盖，白环就是纯白 1px；颜色再按节点底色取对比色（浅底画深灰
  #1A1A1A），浅色主题也看得见。GL 与 D3D11 一致。

### 新增 — 按钮样式示例（`examples/buttonshow`）

按钮能力以前散在 features 里，没有一个"照着抄就行"的集中示例。新增 buttonshow，**纯 CSS**
（外壳只接事件，没有任何绘制代码），六组摆在一起：

- 语义色（主操作 / 次要 / 成功 / 警告 / 危险 / 信息 / 链接）、尺寸（11/12/14px）、
  圆角（2/9/16/胶囊）、底色（纯色 / 渐变 / 描边 / 幽灵）、状态、形态（图标 / 纯文本 / 块级整行）。
- 运行时状态全部演示到位：`:hover`（提亮 + `transform:scale`）、`:active`（压暗 + 回弹）、
  `:checked`（外壳 `setNodeChecked` 维护）、`disabled`（引擎压暗 55% 并退出 Tab 链），
  并在页面上标注真实优先级 `:active > :focus > :checked > :hover`。
- 底部状态栏接 CLICK 事件（鼠标与键盘 Enter/Space 同一条路径），显示"最后点击了哪个按钮 +
  点击次数"；Tab / 方向键移动焦点、焦点环可直接验。
- 浅色主题，与 features 同一套令牌；`Makefile` / `CMakeLists.txt` 已接线。
- README 示例表补上 buttonshow 与 imeshow（之前漏了），并更正"6 组示例 / 全部浅色"的说法。

### 修复 — IME 组字窗/候选框停在旧位置，打字时屏幕上什么都没有

自绘窗口（Cicero-unaware）上，输入法的组字窗和候选框位置**只有第一次生效**：
`ImmSetCompositionWindow` / `ImmSetCandidateWindow` 之后一律返回 TRUE 却不理。
后果是两层：

- 宿主没接 `setImeCaretFn` 时，输入法拿不到光标，组字窗落到**窗口外**
  （实测窗口在屏幕 (900,376)，组字窗跑到 (1030,151)）。现象是"打拼音屏幕上什么
  都没有"，但选词照样上屏，极易误判成输入法没工作。
- 接了钩子也不够：从第一个输入框切到第二个输入框、或在同一个框里接着打下一个词，
  组字串画对了、候选框还赖在旧位置。

修法：

- 新增 `imeCaretFromRenderer()`（`backends/binui_gl-gdi.h`）：按渲染器编辑态算光标
  客户区坐标与行高，算式与 `drawNodesGLGdi` 里 BINUI_INPUT 的绘制同源
  （`padding+4` + `textWidth` + 同一行高），不会差半格。C++ 宿主一行接上：
  `app.setImeCaretFn([&](int& x, int& y, int& h){ return imeCaretFromRenderer(renderer, app.gfx(), x, y, h); });`
- 新增 `BinUIAppGLGdi::applyImePlacement()`：组字开始时**重挂一次 IME 上下文**
  （`ImmAssociateContext(hwnd, nullptr)` 再挂回），强制输入法丢掉旧 UI、重新采纳
  位置；位置没变就跳过（每次按键都重设会让输入法 UI 闪）。
- 新增公开方法 `BinUIAppGLGdi::cancelImeComposition()`，`WM_KILLFOCUS` 改用它
  （同时让位置缓存作废，回来时重新摆）。
- `WM_IME_COMPOSITION` 拿到 `GCS_RESULTSTR` 后**仍然交给 DefWindowProc**：
  早期直接 `return 0`，但它要看这条消息才能收尾；结果串已自己插过，随后生成的
  逐字节 `WM_IME_CHAR` 会被本库吞掉，不会重复上屏。
- `examples/imeshow`（新增示例）补上 `setImeCaretFn` —— 之前漏了，正好是"打拼音
  看不见任何东西"的复现。

已知限制（不是本库能修的）：组字期间鼠标点击会被输入法吃掉（它先把拼音当结果串
提交，点击不送到窗口，连点"退出"按钮都没反应）。要让点击不被吃，得给窗口实现
TSF 文本存储（`ITextStoreACP`）。

### 修复 — 字形图集满了会静默丢字

`rasterGlyph()` / `rasterColorCluster()` 里 `if (a->next >= total) return;` —— 图集满了
新字形**直接不出现**，而且一句日志都没有。表现是"某些字时有时无"，最难查的一类。
单张图集 1024²，按 ~1.4×字号切格，一个字号 + 字体面约 2600 格 —— 中文界面跑久了
（聊天记录、动态列表）真的会撞到。

- **图集分代**：同一个 key（字号 × 字重 × 字体面 × 斜体）现在可以有多张。
  `getAtlasForCp()` 优先返回"已经含有该字形"的那一代（UV 必须和纹理对得上），
  没有就找还有空格的，都满了再开新一代（上限 8 代）。**不再丢字**。
- 绘制路径（`drawText` 的灰度分支）改用按码点取图集；测量路径保持取主图集
  （只要 advance，不碰纹理）。
- 真的到 8 代还满（说明字号/字重组合失控）时，两处仍保留"只喊一次"的 stderr 警告。
- 彩色图集（emoji）暂未分代，满时告警并退回灰度。
- 验证：改动前后同一次 `gl_shot` 抓图**逐像素 0 差异**（320×240，probe.html）。

### 变更 — XML 格式（`<binui>` / `<root>`）正式降级为 legacy

`CompileXML` 路径没有 `<style>` 级联、不认 `<use>` 组件、也没有 v13/v14 的扩展绘制段，
但一直没有任何提示。现在编译 XML 会得到一条警告，建议改写为 HTML。
（`examples/features/*.bum.xml` 这类组件片段走的是 HTML 解析，不受影响。）

### 新增 — 输入法候选窗定位（IME）

以前完全没处理 `WM_IME_*`：中文能上字（`WM_CHAR` 会到），但**候选窗不受控**，
基本飘在屏幕角落；组字期间也没有下划线。

- GL 外壳处理 `WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION`：
  `ImmSetCompositionWindow(CFS_POINT)` 把组字窗口摆到光标处，
  `ImmSetCandidateWindow(CFS_CANDIDATEPOS)` 把候选窗列在光标下方一行。
  定位完继续交给 `DefWindowProc` —— 组字内容本身是系统画的，不抢。
- C ABI 提供 `setImeCaretFn`：按"输入框左内边距 + 光标前缀文本宽度"算出光标客户区坐标，
  字体度量复用 GL 后端的 `textWidth` / `fontMetrics`（与绘制同一套，不会错位）。
- 构建加 `-limm32`（Makefile 的 `GL_LIBS` 与 CMake 的 `gl_shell`）。

### 新增 — 鼠标拖选与双击选词

以前文本选区只能靠 Shift+方向键，鼠标用户会以为输入框坏了。

- 渲染器：`setCaretByUtf16Index(idx, extend)` 多了 extend 参数 —— extend 时只挪 caret、
  保留选区起点（按下时的锚点），这就是拖选；`selectWordByUtf16Index()` 按 UTF-16 索引
  选中所在的"词"（ASCII 字母数字下划线 / 连续非 ASCII 当一个词；点在空白或标点上时
  退化成"连到该处的那段非空白"）。
- 后端：`placeCaretAtClick*()` 多了 extend 参数并返回换算出的 UTF-16 索引（双击复用
  同一次换算，不会因为两次测量的舍入差半格）。
- 窗口外壳：注册窗口类加 `CS_DBLCLKS`（没有它系统根本不发 `WM_LBUTTONDBLCLK`），
  新增 `WM_LBUTTONDBLCLK` → `setDoubleClickFn`；C ABI 里把它接到"选词"，
  并在 `setMoveFn` 里检测左键按下 → 扩展选区。

### 新增 — 剪贴板：Ctrl+C / X / V / A

以前输入框能打字但不能复制粘贴 —— 等于半残。

- 渲染器补三个"文本层动作"：`selectedText()` / `deleteSelection()` / `selectAll()`
  （只读框可以全选和复制，但删不掉内容）。
- C ABI 的按键处理接上 Win32 剪贴板（`CF_UNICODETEXT`），UTF-8 ↔ UTF-16 往返；
  粘贴时去掉 `\r\n`（输入框是单行，换行粘进来会画到框外）；剪贴板被别的程序占着
  时静默放弃（粘贴不成功比崩掉好）。
- 只在 C ABI 这一层做：核心库保持平台无关，`examples/` 里的演示用自己的按键处理，
  没接这个（它们不是产品面）。

### 修复 — IME 组字结果根本不进输入框（"打了字什么都没出来"）

上一版只做了候选窗定位，仍然输不进中文。根因是**我们不是 EDIT 控件**：组字结果串
（`GCS_RESULTSTR`）必须由窗口自己从 IME 上下文取出来插进文本，`DefWindowProc` 不会替
应用做这件事。另外默认的 `WM_IME_SETCONTEXT` 行为是"应用程序自己画组字窗口"—— 系统
以为我们会画，于是它也不画，表现就是打字后屏幕上什么都没有。

- `WM_IME_SETCONTEXT`：清掉 `ISC_SHOWUICOMPOSITIONWINDOW` 再交给 `DefWindowProc`，
  让输入法自己画组字窗口。
- `WM_IME_COMPOSITION`：带 `GCS_RESULTSTR` 时用 `ImmGetCompositionStringW` 取结果串，
  UTF-8 转好交给宿主插入（新增 `setImeResultFn`），并返回 0 防止重复。
- `WM_IME_CHAR`：结果串已插入，这条**必须吞掉**，否则 `DefWindowProc` 会把它转成
  `WM_CHAR`，同一个字进两次。
- `WM_IME_REQUEST` / `IMR_CANDIDATEWINDOW`：新式 IME 走这条问候选窗位置，比
  `CANDIDATEFORM` 可靠。
- 顺带设置组字字体高度跟随输入框行高 —— 不设的话系统用默认 UI 字体，13px 输入框里会
  冒出一行明显更大的组字文本，看着像"位置不对"。

### 新增 — 光标形状（I 型 / 手型 / 拖动箭头）

整个库以前只加载过 `IDC_ARROW`：文本框里不会变 I 型、按钮上没有手型 —— 用起来第一眼
就"不像原生控件"。

- 渲染器新增 `cursorAt(x, y)`，按光标下是什么节点给形状：输入框 → I 型、可交互节点
  （按钮等）→ 手型、滚动条边缘 → 竖直/水平箭头、其余（含禁用节点）→ 箭头。
- GL 与 D3D11 两个窗口外壳加 `WM_SETCURSOR` 处理（非客户区仍交回系统）。
- 顺带修了 `hitScrollBar()` 的水平分支：它拿 `contentHeight` 判断水平条，应该用
  `contentWidth`。

### 修复 — 收尾三项：跨线程 destroy 改为拒绝、圆角模板改 LRU、水平滚动条

- `binui_app_destroy` 在非建窗线程上**拒绝执行**（以前只打警告再硬做，仍会崩）：
  跨线程拆 Win32/WGL 是未定义行为，现在的取舍是"泄漏窗口 + GL 上下文，进程退出时
  一起回收"，并把原因写进 `binui_last_error`。App 句柄不会失效，宿主可以直接
  `os.Exit`。
- 圆角模板缓存满了改成 **LRU**（以前淘汰"最大的一张"，而大的那张往往正是最热的主
  卡片圆角）。
- 滚动条补上**水平方向**：内容比容器宽时在底边画水平条，并给内容的底部留 gutter。
  竖直/水平两条在右下角不重叠（水平轨道扣掉竖直条那一格）。GL / D3D11 / GDI 三端一致。
  渲染器顺带补了 `contentWidth()`（`contentHeight()` 的横向对偶，`scrollTo` 本来就
  按它钳制）。

### 修复 — 圆角半径到"短边一半"就退化成直角（胶囊 / 正圆画不出来，GL 与 D3D11）

九宫格拼接把角片边长写死成 `2r+1`（模板 4r×4r，形状内缩 1px）。角片比四分之一圆弧
本身需要的 `r` 大一倍多，于是 `2×角片` 很快超过盒子短边：两个角片互相重叠（四边中部
露出凹口），中段宽高转负（整块被角片自带的弧裁掉）—— `border-radius:999` 的圆形头像、
胶囊按钮、`<svg><circle>` 全都命中，半径到短边一半时直接变**直角**。

- 几何统一抽到 `include/binui_paint.h` 的 `BuildRoundedNineSlice()`：角片边长 = `r`
  （四分之一圆弧的包围盒正好是 r×r），模板改成"形状铺满 [0,t]"（不再内缩 1px），
  模板边长从 `4r` 降到 `2r+4`（r=200 时 2.5MB → 653KB）。
- 半径钳到 `min(w,h)/2` 后九块**精确铺满**：`2r == 短边` 时中段宽高为 0，四个 1/4 圆
  正好拼成正圆 / 胶囊。
- `ensureRasterDC()` 的返回值以前没检查（失败后按新尺寸 memset 会踩坏堆），现在检查；
  圆角模板缓存加了上限（超过 48 张淘汰最大的一张）。
- GL 与 D3D11 两个后端同步；GDI 走 `RoundRect` 本来就是对的。

### 修复 — `binui_app_destroy()` 在窗口仍打开时可能崩（**结论有更正**）

> 后续用原宿主复验发现：这条崩溃**不是库的缺陷**，是宿主跨线程调用（Go 的 main
> goroutine 没 `runtime.LockOSThread()` 会迁移，`DestroyWindow` / `gfx_.shutdown()`
> 落到非建窗线程）。旧代码 + 宿主锁线程 16/16 不崩。详见 `docs/issues/02` 的
> 「复验」一节与 `docs/issues/08`。下面的加固保留（顺序确实更稳），但它没有"修好"
> 那个崩溃，因为崩溃本来不在库里 —— 真正补上的是下一条的线程自检。

`close()` 以前是"先 `gfx_.shutdown()` 再 `DestroyWindow()`"。窗口还活着时
`DestroyWindow` 会**同步**把 `WM_KILLFOCUS` / `WM_NCDESTROY` 派发回窗口过程，
而 GL 上下文这时已经拆掉了 —— 重入路径踩到已析构的成员。

- 顺序改成：先把窗口过程摘回 `DefWindowProc` 并清掉 `GWLP_USERDATA`（之后任何消息
  都不进我们的代码，也不会有悬垂 `this`）→ `ShowWindow(SW_HIDE)` → 拆 GL →
  `DestroyWindow`。
- `WM_PAINT` 加就绪守卫：GL 没就绪时 `ValidateRect` 掉，别落到 `DefWindowProc` 去真画。

### 新增 — 线程自检：跨线程调 `pump` / `render` 明确报错（docs/issues/08）
窗口、消息循环、WGL 上下文都有线程亲和。以前跨线程调用**不报错**，症状是随机 AV
或退出时挂死，日志里一片空白 —— 连"是不是我写错了"都看不出来。

- `open()` / `openChild()` 记下建窗线程；`binui_pump` / `binui_render` 在非建窗线程上
  调用时返回 **-1**（原语义只有 0/1，扩一位向后兼容），并往 stderr 打一行说明；
  `binui_app_destroy` 仍会执行（没法返回错误），但先打警告。
- Go 绑定：新增 `binui.RunMain(fn func() int) int`（替宿主 `runtime.LockOSThread`）与
  `PumpOrErr`（把"线程不对"单独报成 `ErrWrongThread`）；demo 改用 `RunMain`。
- **文档纠错**：`bindings/go/README.md`、`binui.go` 顶部注释、`docs/bindings.md`
  原先都写着「主 goroutine 天然固定线程」—— 这句是错的，main goroutine 会在安全点被
  调度到别的 M，`main` 里也必须锁线程。

### 修复 — `display:none` 只跳过元素本身，子树被重挂到最近的可见祖先

隐藏容器里的绝对定位子元素会按"新父级"重新解析坐标（跑到左上角），而且**零警告**。

- 编译器新增一趟预处理：按**级联后的 style**（class 规则也算）标出 `display:none`
  的整棵子树，主循环按标记跳过。沿 `parentIdx` 链向上查，对元素顺序没有要求
  （`::before` 合成节点的父索引可能大于自身下标）。
- `:nth-child` 的口径（`display:none` 的元素仍计入兄弟序号）不变。

### 修复 — `<button>` / `<input>` 默认底色写死 `#222222`，按钮文字强制居中 + 700 粗体

浅色主题下"不写 background 的按钮"就是一坨黑块；`text-align:left` 与 `font-weight`
双双失效（三个后端各有一份写死的绘制代码）。

- 默认底色：`BINUI_BUTTON` / `BINUI_INPUT` 也改成透明，与 v13 起"容器默认透明"一致。
- 文字对齐：三个后端都改成读 `BINUI_TEXT_CENTER` / `BINUI_TEXT_RIGHT`，算法与
  `BINUI_LABEL` 分支完全一致。`<button>` 的 UA 默认居中由**编译器**补（浏览器如此），
  所以没写 `text-align` 的按钮观感不变。
- 字重：读扩展绘制段的 `font-weight`（`PAINT_WEIGHT`），没声明时回落 700。

### 修复 — Makefile：`test_capi` 从不重链，跑的是旧二进制

`test_capi` 的规则把 `$(BINUI_C_A)` 写在变量定义之前 —— GNU Make 立即展开成空，
于是这个目标只依赖 `.cpp`，库更新后不重链。改了 C ABI 之后跑出来的崩溃其实是
"新旧两套目标文件混链"。现在直接写 `$(BUILD_DIR)/libbinui_c.a` 并补上 `$(LIB_A)`。

### 修复 — 滚动条颜色/宽度写死，浅色主题右边一条黑杠

轨道 `#2E2E2E` / 滑块 `#5A5A5A` / 宽 8px 全是字面量，没有任何配置入口。

- 渲染器新增 `BinUIScrollbarStyle` + `setScrollbarStyle()` / `resetScrollbarStyleToDefault()`；
  **默认按根节点底色亮度派生**（浅底浅色、深底深灰）。
- C ABI 新增 `binui_set_scrollbar_style()` / `binui_reset_scrollbar_style()`，
  三个绑定同步。三个后端都改成从 `renderer.scrollbarStyle()` 取色与宽度。
- CSS 的 `scrollbar-color` / `scrollbar-width` 仍然不支持，但**不再静默忽略**：
  编译期会告警并指向上面的接口（否则作者会以为改主题生效了）。
- **补上 gutter**：真的要出竖直滚动条时，滚动容器的内容视口右侧扣掉滚动条宽度，
  内容不再被滚动条压住（GL / D3D11 / GDI 三端一致）。

### 新增 — 运行期能力：`scroll_to` / `set_node_bounds` / `add_node` 等

核心渲染器里一直有，只是没导出。列表 / 聊天流绕不过去的能力：

- `binui_scroll_to` / `binui_scroll_by` / `binui_content_height`
- `binui_set_node_bounds`（宿主自己排版 / 撑高）
- `binui_add_node` / `binui_remove_node`（动态节点；`remove_node` 只是隐藏，id 不复用）
- 纯增量新增，不动既有结构体 / 签名，`BINUI_C_ABI_VERSION` 保持 1。
- Python / Go / Rust 三个绑定同步补齐（Go 还补了 `SetImageRaw` / `SetImage` /
  `MountComponents`，并用 `unsafe.SliceData` 修掉了 `go vet` 的
  `possible misuse of unsafe.Pointer`）。

### 新增 — `set_text` 换长文本后自动重算高度

以前"未声明高度就按文本估算撑开"这一步**只在编译期**做，运行期 `set_text` 换成长文本
会画到节点盒外、压到下一个元素身上。

- 规格段用旧布局的 `reserve` 字节承载 `heightAuto` + `autoBase`（基线高度，即估算前的
  元素默认盒高）。旧二进制这两位恒为 0 = 不重算，**不升 `BINUI_VERSION`**
  （先例：`BINUI_TEXT_CENTER` 复用 flags 的空闲位）。
- 估算口径抽成 header-only 的 `include/binui_text_estimate.h`，编译器与渲染器共用一份
  （否则"编译期刚好放下、换文本就压字"）。重算取 `max(基线高度, 估算高度)`，
  与编译期完全一致。

### 修复 — 同一线程多实例时 GL 上下文不切换，先建的实例画成纯黑

WGL 的 current context 是**线程级**的。`BinUIGLGdi::init()` 里 `wglMakeCurrent`
只做一次，之后没人再切回来 —— 于是同一线程里有两个后端实例（子窗口模式嵌两块面板、
或者多窗口）时，所有绘制都跑到最后 init 的那个上下文里，先建的那块**整个窗口纯黑**，
而它的事件/命中测试一切正常（所以很难察觉是渲染问题）。

- `Impl::makeCurrent()`：先 `wglGetCurrentContext()` 比对，不是自己才
  `wglMakeCurrent`；`beginFrame()` / `present()` / `presentViaGdi()` 三处入口统一调用。
- 这条是"子窗口模式"的必修项：嵌入场景天然每帧多个实例交替绘制。

### 新增 — `tools/gl_shot`：GL 后端的离屏抓图（终于能看见 GL 输出）

OpenGL 窗口抓不到图 —— `PrintWindow` 对双缓冲 GL 窗口返回全黑，`BitBlt` 只能抓合成
到桌面的部分；所以文档图一直只有 GDI 离屏渲染，而 GDI 自带字体链接，"GL 到底画成
什么样"谁都没验证过。

`tools/gl_shot <ui.html> <out.png> [w] [h]`：建一个窗口 → GL 上下文 → 渲染一帧 →
`presentViaGdi()` 读回离屏像素并 `StretchDIBits` 到窗口 DC → `BitBlt` 存 PNG。
**窗口必须先 `ShowWindow`**：GDI 对隐藏窗口的 `StretchDIBits` 直接丢弃（不报错），
读回会得到一片空白 —— 这也是排查时踩过的坑。

产物：[docs/img/glyph-fallback.png](docs/img/glyph-fallback.png)（逐字形回退 + 豆腐块）。

### 修复 — 缺字留白改成豆腐块：逐字形回退（问字体的 cmap，不按码点区间猜）

GL / D3D 后端选字体一直是"按码点区间猜"：`cp >= 0x80` → 微软雅黑，落进 emoji 区间 →
Segoe UI Emoji，否则 Segoe UI。猜中的面里没有这个字形时，`FT_Get_Char_Index` 返回 0，
`renderGlyph` 直接 `return false`，光栅化不缓存、绘制时 `if (!g) return;` —— **渲染成一片
空白**。用户看到的不是"这个字体没有该字符"，而是"引擎没画"。

为什么猜本身是错的：Unicode 只规定"码点 ↔ 抽象字符"，字形归谁由**每个字体文件自报的
cmap** 决定；Block 只是管理分组，标准明确说不能用来选字体。按区间猜会同时错两次 ——
缺字（空白），以及风格错（`⌘⌫⎋` 这类黑白符号被丢给 emoji 字体，画成彩色图形）。

- `FontEngine` 新增**逐字形回退**：`setFallbackChain({…})` / `faceIndexFor(cp)` /
  `faceName(idx)`。`faceIndexFor` 扫候选链、查的正是 `FT_Get_Char_Index`（字体 cmap），
  命中即停、结果按码点缓存；候选面的 `FT_Face` 也按需加载（前面的面命中就不读后面的，
  中文界面不会因为链尾有泰文候选就多读一个字体文件）。
- 新增 `renderMissingGlyph(size, …)`：**空心方框（豆腐块）**，几何只有一份，两个 GPU
  后端共用。缺字时画方框而不是留白 —— 留白像引擎 bug，方框是"当前字体链里没有这个
  字符"的明确信号。测宽与绘制用同一份度量，顺带修掉"占着 0.6em 宽度却不画"的错位。
- 设计上不可见的码点（C0 控制符、ZWJ/ZWNJ、变体选择符 VS1–256、键帽组合符）不画方框，
  否则一串 emoji 里会凭空多出方块。
- `registerSystemFonts` 把候选面补齐并排好顺序（Windows）：Segoe UI → 微软雅黑 →
  **Segoe UI Symbol** → Segoe UI Emoji → 宋体/黑体 → MS Gothic → Malgun Gothic →
  Nirmala UI → Leelawadee UI → Arial。符号面排在 emoji 面之前：`⌘⌫✕` 现在走
  Segoe UI Symbol 画**黑白**符号，只有真 emoji（`😀`、ZWJ 序列）才走 COLR 彩色路径。
- 两个 GPU 后端的字形图集 key：低 6 位从 `cjk|emoji` 两个布尔换成**面下标**
  （同一码点解析到不同字体时必须分图集，否则字形会串）。
- `test_font` 新增断言：回退链已填、`setFallbackChain` 过滤未注册名 + 去重、
  `faceIndexFor` 对 `A` / `中` / `−` 命中且**命中面确实画得出该字**、
  `U+10FFFD`（私用区末尾）判定为无字形、`⌘` 解析到 Segoe UI Symbol、
  豆腐块 12/14/32px 的四边有墨 + 中心空心 + 边框不透明。

> 顺带说明为什么这个 bug 一直没在文档图里暴露：GDI 离屏渲染走 `DrawTextW`，系统有
> **字体链接**（font linking）自己挑字体补上；GL/D3D 后端自己管字形图集，缺就是缺。
> 所以 `docs/img/*.png` 全对，真程序里才有空白。

### 新增 — 子窗口模式：把 binUI 当成控件嵌进宿主窗口

之前只有"binUI 自己当应用"（顶层 `WS_POPUP` 无边框窗 + 自己的消息循环）。要把它塞进
WPF / Qt / WinForms 的某个面板里，缺的是这一层：`WS_CHILD` 子窗口，宿主管消息循环、
布局、DPI，binUI 只管那一块画布。

- `BinUIAppGLGdi` 新增 `openChild(parent, x, y, w, h)` / `renderFrame()` /
  `setClientSize(w, h)` / `windowHandle() / isChildMode()`；`pump()` 在子窗口模式下
  只出帧、**不 `PeekMessage`**（碰了就是把宿主的消息吃掉）。
- C ABI 新增 `binui_app_open_child` / `binui_render` / `binui_resize` / `binui_app_hwnd`。
- 窗口类注册顺带修正：`hInstance` 改为取**本模块**（`GetModuleHandleExW` +
  `FROM_ADDRESS`）而不是 `GetModuleHandleW(nullptr)`。DLL 里后者拿到的是宿主 exe 的
  实例，把窗口类注册进别人的 atom 表是越界行为——顶层窗侥幸能用，子窗口模式不行。
- 子窗口模式下刻意不做的事：`SetProcessDPIAware`（进程级策略归宿主）、`SetFocus`
  （不抢宿主焦点）、`WS_EX_LAYERED`（对子窗口无效）、标题栏拖动/边框缩放/最大化。
- 生命周期：宿主 `DestroyWindow` 时子窗口的 `WM_DESTROY` 当场收摊（关 GL、标记关闭），
  且**不发 `PostQuitMessage`**（那会把宿主的消息循环干掉）；之后 `binui_render` 返回 1。
- Python 绑定加 `open_child` / `render` / `resize` / `hwnd`，并给了活例子
  `bindings/python/demo_child.py`：用 ctypes 建一个普通 Win32 宿主窗口，里面嵌两块
  独立 binUI 面板。

**踩到的坑（已写进文档）**：宿主窗口必须带 `WS_CLIPCHILDREN`。不加的话宿主的
`WM_PAINT` 会把整个客户区刷一遍、把 GL 子窗口那块一起刷掉——GL 子窗口没有留存面，
表现就是"面板是白的、点得到但看不见"。加与不加是"整块空白"和"两块面板各就各位"的区别。

验证：点击两块面板的按钮 → 事件回到 Python、两块面板同时更新；输入框聚焦 + 输入中文；
点"关闭宿主" → 干净退出（退出码 0）。参数校验进 `tests/test_capi.cpp`，CI 可跑。
真机截屏见 [docs/img/child-window.png](docs/img/child-window.png)：普通 Win32 宿主窗口，
左边 220px 定宽面板、右边自适应面板，两块独立实例同时正确渲染。

### 新增 — C ABI（`include/binui_c.h`）与 Python / Go / Rust 绑定

引擎一直是纯 C++（`namespace binui`、`std::function`、`std::vector` 返回），其他语言
根本接不上：全仓库没有 `__declspec(dllexport)`，CMake 只有 `STATIC` 库。这次补了对外
的唯一一扇门，并把三门语言跑通。

设计上只守三条约束，绑定写法就统一了：

1. **定长 POD** —— 只用 `int32_t` / `uint32_t` / `float`。没有 `bool`（C 是 1 字节、
   C# 默认 4 字节）、没有 `enum`（宽度随编译器）、没有 `size_t`（32/64 位会变）。
   所以结构体天然 4 字节对齐，**不需要任何 `#pragma pack`**。
2. **没有跨语言回调** —— 事件全部 `binui_poll_event()` 轮询。宿主不进引擎的调用栈，
   就没有线程亲和、GC 保活、重入这些问题；PHP/Ruby 这类没有稳定回调的语言也能用。
3. **UTF-8 + (buf, cap) 两段式取文本** —— 不返回需要宿主释放的指针。

新增文件与改动：

- `include/binui_c.h` / `src/binui_c.cpp`：约 40 个函数。编译（`binui_compile_file` /
  `binui_compile_memory`）、应用生命周期（`app_create` / `app_open` / `pump` /
  `poll_event` / `app_destroy`）、节点查询与修改、窗口动作、图片与组件挂载。
- `BinUIAppGLGdi` 拆出 **`open()` / `pump(timeoutMs)` / `close()`**（`run()` 行为不变，
  等价于三者串起来）。宿主语言需要"自己拥有消息循环"，原来的 `run()` 把循环锁死在里面。
  `pump` 在空闲时用 `MsgWaitForMultipleObjectsEx` 阻塞到有输入，不空转烧 CPU。
- 事件队列 + 内建交互：悬停重绘、标题栏拖动、输入框光标定位、Tab 循环焦点、
  Enter 激活、滚轮滚动、动画/视频时钟推进 —— 宿主只管 `pump` + `poll` + 改文本。
- 构建：`Makefile` 加 `capi` 目标（`libbinui_c.a` 静态库 + `binui_c.dll` 动态库 +
  它俩的 import lib），`CMakeLists.txt` 加 `binui_c` / `binui_c_shared`（仅 Windows）。
  DLL 自包含：`-static-libgcc -static-libstdc++` + FreeType/HarfBuzz 静态库，除系统 DLL
  外不用附带任何运行时。
- 绑定：`bindings/python/`（ctypes，零第三方依赖）、`bindings/go/`（cgo）、
  `bindings/rust/`（手写 `extern "C"` + 安全封装，不用 bindgen）。三份都能编 UI、
  建窗、收点击、改文本，且都验证过中文与 emoji 输入。
- 测试：`tests/test_capi.cpp`（无窗口，CI 可跑）把 ABI 钉死 ——
  `sizeof(BinUINode) == 56`、`offsetof(color) == 32`、`sizeof(BinUIEvent) == 48`、
  `offsetof(text) == 24`，外加编译/加载/两段式取文本/截断/NULL 防护/错误路径。
- 文档：`docs/bindings.md`（约束、流程、线程规则、未覆盖能力、新增语言步骤）、
  `bindings/README.md`（总表）、三份子 README；README 加「在别的语言里用」一节。

### 修复 — ANSI 窗口类导致中文输入变乱码（三个外壳全中）

三个窗口外壳（`binui_gl-gdi` / `app_win32` / `binui_d3d11`）都用 `RegisterClassA` +
`DispatchMessageA` 建窗。在 CP936 系统上，Windows 会把**一个中文字符拆成两个
`WM_CHAR`**（DBCS 字节），`handleChar` 逐字节转 UTF-8 就得到乱码：输入框里打「中」
显示「ÖÐ」，打「鱼」显示「Óã」。IME 输入同理。

改成 Unicode 窗口（`RegisterClassW` + `DispatchMessageW` + `CreateWindowExW` +
`DefWindowProcW`）后正常，代理对（emoji）也走通了 —— 原本写好的 `pendingHigh_`
代理对逻辑以前根本收不到数据，因为 ANSI 窗口不会给 16 位码元。

这条 bug 只在"真在别的语言里打一遍字"时暴露（C++ demo 一直是中文标签、没人打中文），
算是本次做绑定最实在的收获。

### 修复 — 输入框底部强调线与作者边框重复

三个后端都给每个 `<input>` 硬画一条 2px 底部强调线（老深色主题的 Material 下划线）。
v13 有 `border` 之后作者已经用 `border: 1px solid #E1E9F5; border-radius: 10` 画了
真正的框，那条线就变成横穿圆角的悬空横杠。改为**作者声明了 `PAINT_BORDER` 时不画**，
没声明的旧界面行为不变。

### 修复 — 点击输入框中部不定位光标

`placeCaretAtClick` / `placeCaretAtClickGLGdi` / `placeCaretAtClickD3D` 三个函数写了
但**从来没有调用点**（README 里却写着"点击光标定位"）。C ABI 的 press 里接上了；
`examples/features` 的 press 回调也补了这一行。

### 修复 — 库内调试输出污染宿主 stdout

`BinUiRenderer` 里有 4 处 `std::cout`（加载统计、组件挂载、点击日志、资源加载）。
作为嵌入式库，这会把噪声塞进宿主语言的 stdout。改为默认静默、`BINUI_VERBOSE=1`
打到 stderr（`std::cerr` 的错误输出保留）。

### 新增 — 组件源码支持 `.bum.html`

组件一直支持两种源码写法，但只有 `.bum.xml` 在文档里出现过。`ProbeAsset` 的候选表里
漏了 `.bum.html`：写 `src="x.bum.html"`（显式扩展名）一直能内联，写 `src="x.bum"`
（裸名字）就找不到 `x.bum.html`，只会安静地留一个空容器。

- `include/binui_asset.h`：候选表补上 `.bum.html` / `.bui.html` / `.binui.html`；
  裸名字顺序 `<名字>.bum` → `.bum.xml` → `.bum.html` → …（二进制优先，规则不变）。
- 标准组件库新增 `components/tag.bum.html`：库里第一个 HTML 写法的组件，
  `examples/features` 里用三种 `<use>` 写法（显式 `.bum.html` / 裸名字）指向同一个组件。
- `test_compiler` 新增 5 条断言：显式扩展名内联、参数替换、`kind` 换肤、裸名字命中。
- 文档：`components/README.md` 补「两种写法」一节，`docs/components.md` 清单加 `tag`。

**注意**：组件只在 HTML 解析路径下展开。`CompileXML`（`.binui.xml` 老路径）不处理
`<use>`，那种源文件里写组件不会生效。

### 变更 — 示例砍成 6 组：功能 / 动画 / 字体 / 2D 渲染 / 3D 渲染 / 纹理

原来 9 个示例各演各的，彼此重叠又都不完整（cssshow 讲伪类、components 讲组件化、
vision_monitor 讲数据更新，三套脚手架几乎一样）。现在收敛成 **6 组各一个示例**，
一组只演示一件事，全部用标准标题栏 + 同一套浅色令牌：

| 目录 | 组 | 来源 |
|---|---|---|
| `examples/features` | 功能 | **新合并**：cssshow + components + vision_monitor 的数据驱动 |
| `examples/animshow` | 动画 | 保留 |
| `examples/fontshow` | 字体 | **新建** |
| `examples/svgdemo` | 2D 渲染 | 保留 |
| `examples/cubedemo` | 3D 渲染 | **新建** |
| `examples/texturedemo` | 纹理 | **新建** |

删掉的：`videodemo` / `singlewindow` / `multiwindow` / `galgamechat`（git 历史里都在）。
`examples/assets/noela.png` 一并删除（只有被删的立绘示例用）。

三个新示例的看点：

- **fontshow**：字号 12–32、字重 400–800、斜体、行高 100%/170%、三种文字装饰、
  中英混排与 UI 符号。顺带把"字形图集怎么分类、缺字为什么是空白"写进卡片注释。
- **cubedemo**：引擎里没有 3D —— 顶点变换、透视投影、逐边按深度调色全在应用里做，
  最后只用 `gfx.fillLine` 画 12 条棱。这是"引擎只管 UI"这条边界的活示范。
- **texturedemo**：布局里写一张不存在的图 `<img src="tex/proc.png">`（只为占一个图片槽），
  运行期用 `updateImageRaw` 每帧生成 RGBA 塞进去；同一张纹理被四个尺寸采样。
  和 `<video>` 帧走的是同一条通路。

配套改动：

- `examples/README.md` 新增：6 组对照表 + 写新示例的三条约定。
- 原来 `examples/vision_monitor/main.binui.xml` 是唯一的 XML 源示例，随示例一起删掉；
  它作为 **XML 解析的回归夹具** 移到 `tests/fixtures/vision.binui.xml`，
  `test_compiler` / `test_renderer_real` 指过去（XML 的编译与运行时更新覆盖没丢）。
- `test_hot_reload` 的"源码树兜底"改用 `examples/features/NOTES.md`。
- Makefile / CMake 的 demo 目标改成 6 个（`add_gl_demo(name)` 统一处理拷贝）。

### 新增 — flex 支持 `width:content`（按文字宽度排）

flex 里没有"宽度自适应"：声明像素要么留一条大缝、要么把字裁掉，`width:auto` 又会被
当成弹性项去平分剩余空间。新增 `width:content` —— 用编译器自己的字宽估算（和换行同一套
度量）×1.08 + 4px 作为固定宽度。标题栏的图标/标题/副标题就靠它紧挨着排，不用再手调像素。

### 修复

- **标题栏最小化按钮是空的**：`─`(U+2500 制表符) 只有中文字体里有，Segoe UI 没有；
  GL/D3D 的字形图集按"非 ASCII → 中文字体"分类，该字形笔画极淡，肉眼就是个空按钮。
  换成 `−`(U+2212)，三颗按钮的字号统一由 `bsym` 控制（默认 13）。
  `tests/test_font.cpp` 新增一条断言：四个 UI 符号（− □ ✕ ◆）至少有一个候选字体画得出笔画。
- **标题栏副标题离标题太远**：`titlew` 原来是手填像素，各示例按字数猜，猜大了就是一条缝。
  现在默认 `content`，副标题紧贴标题；单窗口示例那个超长标题顺手缩短，细节挪到副标题。

### 新增 — 标准组件库 `components/`（标题栏、按钮、输入框……）

`examples/` 里的组件是"组件机制"的演示；真正能拿来用的通用组件之前一个都没有 ——
每个示例都手写一遍标题栏（6 份几乎一样的 8 行 span/button）。新增仓库根的
`components/`，11 个纯 HTML+CSS 组件，`<use src="…">` 编译期内联、运行期零依赖：

`titlebar` `button` `field` `badge` `toggle` `panel` `dialogue` `stat` `avatar`
`divider` `statusbar`

- `titlebar` 用 **flex 排版**，窗口多宽都不用宿主算按钮坐标（`[图标][标题 flex:1][说明][─][✕]`）。
- `panel` 支持"带内容"：宿主的子元素会落进组件根容器，按流式排在标题下面。
- 文档：`components/README.md`（参数表 + 三条使用约定 + 限制）、`docs/components.md` 新增一节。

### 修复

- **`<use>` 丢掉了组件根的标签**：替换节点恒为 `div`，于是
  · 以 `<button>` 为根的组件实例变成普通容器（不可点击、文字不居中、不吃 `:hover`）；
  · 以 `<input>` 为根的实例丢掉占位符与编辑语义（输入框只剩一个空盒子）；
  · 以 `<span>` 为根的实例变成块级 `LIST`（文字带 12px 内缩、顶对齐，小胶囊里的字被裁）。
  现在把根标签原样搬过来 —— 上面三种症状一次全消。标准组件库正是靠它才能成立。

### 变更

- 示例统一改用标准 `titlebar`（animshow / svgdemo / cssshow / galgamechat /
  singlewindow / multiwindow·stage·dialog），并删掉各自那份手写的标题栏 CSS；
  `examples/components` 新增第四张卡片展示标准组件库。
- `examples/cssshow` 的主程序补上"─ 最小化"分支（以前只有 ✕）。

### 变更 — 示例 UI 全面换成清新浅色主题

9 个示例（animshow / cssshow / components / svgdemo / videodemo / singlewindow /
multiwindow / galgamechat / vision_monitor）与引擎内置默认布局，从"深色赛博"统一改成
**浅色清爽风**：页面底 `#F4F7FC`、白卡片 + 1px `#E6EDF7` 边框、主字 `#1F2A37` /
次字 `#66768C` / 弱字 `#94A3B8`、主色 `#3E8BFF`、薄荷 `#2FBF9A`。层次只靠 1px 边框与
色差（`box-shadow` 引擎不支持），令牌约定写在每个示例 `<style>` 开头的注释里。

- 按钮拆成"主色填充 + 白字 / 浅蓝底 + 主色字"两套，各自带悬停/按下；把 `background`
  从行内 `style` 搬进 class —— 行内样式会压掉 `:hover`，这是浅色改造最容易踩的坑。
- 几何布局基本不动（只有 svgdemo 收掉多余的窗口高度、animshow 的"方向·次数"卡片
  第 4 行原本就溢出卡片、videodemo 的装饰圆原本悬在卡片外，一并修正）。
- 示例里硬编码的颜色同步换新（animshow 的色块调色板、svgdemo 的频谱条、singlewindow
  的换背景色循环）。
- `docs/img/{default-layout,cssshow,components}.png` 全部重渲染。

### 修复

- **GDI 后端不画 `BINUI_LINE`**：`<line>`/`<polyline>`/`<polygon>` 在 GDI 画布上一根都
  没有（GL/D3D 正常），三端不一致。现在按端点包围盒单独裁剪后画线。
- **容器文字恒为白字**：`BINUI_LIST` 没声明文字色时一律白 —— 浅底容器里（尤其运行期
  `addNode` 出来的列表行）文字看不见。改为按节点自身底色亮度选深浅（`PaintContrastText`）。
- **输入框强调色写死紫色**：文字/光标/底部强调线都是 `#BB86FC`，浅色主题没法配色。
  改为优先用作者声明的文字色，强调线压到 35%。
- **内置默认布局的文字全是错乱的**：`GenerateDefaultBinUI()` 的字符串偏移是手写魔数
  （0/5/18/29/66/95/106/115/130/135），文案一改就整体错位，渲染出来的标题、按钮、
  底部导航全部串到别的节点上；而且容器的 `textOffset = 0` 现在指向 "9:41"（应为空串）。
  改成按字符串长度累加算出偏移，并在表首放空串。默认布局同时换成浅色。
- `tools/offscreen_render` 新增 `--default`：直接渲染内置默认布局，这张 docs 图终于
  可以一条命令重现（以前只能手工截图，所以一直和代码对不上）。

### 新增 — 元素语义与默认样式：UA 样式表、行内标签、表单属性（【格式 v14】）

> 上一轮把"格式"和"CSS 表达力"补完了；这一轮补的是**元素这一侧**。体检时发现的共同
> 根因是两条：① 没有"元素 → 默认样式"这一层（浏览器 UA 样式表的等价物），于是 `<u>`
> `<mark>` `<hr>` `<b>` 这些标签写了等于没写；② 表单属性（`placeholder` / `value` /
> `disabled` / `readonly` / `checked` / `hidden`）被**静默忽略或错误消费**，而且整份 HTML
> 编译下来一条警告都没有 —— 作者没有任何线索。

- **【格式 v14】`BinUIPaintNode` 补 `textFlags`**（40 → 44 字节）：斜体、占位符、只读、
  初始勾选。四个都是"少数节点才需要"的布尔量，跟着稀疏的扩展绘制段走，比往定长
  `BinUINode` 里塞字段便宜；`flags` 的 16 位已经用满。
- **内置 UA 默认样式表**（最低优先级的级联层，作者任何声明都覆盖它）：
  `h1~h6` 加粗 + 字号梯度 28/24/20/18/16/14，`b/strong/th` 700，`i/em/cite/var/dfn` 斜体，
  `u/ins` 下划线，`s/del/strike` 删除线，`mark` 浅黄底深字，`hr` 一条 2px 中灰线，
  `a` 下划线，`pre` 保留原始换行与缩进，`sub/sup` 0.75em 行内小字（**无基线偏移**）。
- **默认文字色随根底色自适应**：根底色亮度 > 0.5 取 `#1A1A1A`，否则白。之前默认写死白色，
  而根底色默认也是白 —— **不写 `color` 的浅色页面整页白底白字**，只有到处写 `color`
  才看得见；深色主题亮度低，观感不变。
- **行内标签不再掉进块级分支**：`u/s/strike/del/ins/mark/code/kbd/samp/var/abbr/cite/q/`
  `dfn/time/sub/sup/dt/dd/figcaption/summary/caption` 等补进 `BINUI_LABEL` 白名单
  （以前默认落到 `BINUI_LIST`，把一句话里的几个字打成独立整行块）。
- **`font-style: italic`**：FreeType `FT_GlyphSlot_Oblique` 合成倾斜（只切变轮廓、不改
  advance，所以测宽/绘制仍一致），GDI 走 `CreateFontA` 的 italic 参数，三端一致。
- **`white-space: pre|pre-wrap`**：解析阶段同时保留"折叠文本"与"原样文本"，按 CSS 取用；
  `<pre>` 由 UA 样式给出 `white-space:pre`。以前代码块的缩进被当普通空白折叠掉。
- **表单属性真生效**：
  `placeholder` 列为占位符（55% 色绘制、首次输入清空、失焦留空恢复，
  `renderer.inputValue()` 取真实值）；`value` 只在文本类 `input` 上是真实值；
  `disabled` → `BINUI_DISABLED`（不进焦点链、不响应悬停/点击、拒绝编辑、三端 55% 淡）；
  `readonly` → 可聚焦可选中但不显示光标、拒绝内容变更；
  `<input type=hidden>` 不生成节点；`checkbox/radio` 编成 18×18 方块/圆点，
  `checked` 播种为运行时勾选态；`<a href>` 变成可命中节点；`<img alt>` 写进文本，
  图片加载失败时后端居中显示。
- **HTML 标签/属性体检**：未识别的标签、"写得进去但没实现"的属性（`name`/`for`/
  `required`/`maxlength`/`pattern`/`tabindex`/`target`/`rel`…）各警告一次；`<table>` /
  `<select>` / `<textarea>` / `<details>` / `<progress>` 这类"认得但只能糊弄"的标签会
  明说它会变成什么。HTML 模式生效，XML 允许自定义标签不查。

### 修复

- **GDI 后端的字号比 GPU 后端小一号**：`CreateFontA` 的高度传了正值（字符单元高度，
  含内部行距），比 em 小约 15%，同一份 UI 在 GDI 里字号/换行位置和 GL/D3D 对不上。
  改成负值（em 高度，与 `FT_Set_Pixel_Sizes` 同口径）。
- **GDI 后端把窄盒子里的短标签折成两行**：单行文本只要超宽就走 `DT_WORDBREAK`，而 GPU
  后端画单行文本从不换行。改成超宽 20% 以上才换行。
- **GDI 后端的裸 `'\n'` 不换行**：`DrawTextW` 只认 CRLF，`<br>` / `<pre>` 存的都是单个
  `'\n'`，整段会连成一行。绘制前统一补 `'\r'`。
- **GDI 后端把"透明 + 有文字"的节点整块跳过**：`<pre>` / `<div>文字</div>`（没有底色）
  直接消失。跳过条件补上"没有文本"。
- **`<hr>` 从 v13 起完全不可见**：容器默认透明化的副作用 —— 既没高度也没颜色。
- **`placeholder` 被当成真实文本**：点进去就能编辑提示语、退格删掉还触发 `INPUT`。
- **`value` 在 `checkbox`/`radio` 上被画成可见文字**；`<input type=hidden>` 变成可见输入框。
- **`examples/videodemo` 用 `name=` 取节点**（`findNodeByName` 只认 `id`），改 `id=`。
- **`<use>` 的参数键被当成属性挂到替换节点上**：`<use label="…">` 会在结果里留下
  一个无效的 `label` 属性。现在只挂"有语义的属性"（宽高/样式/类等）。
- **`examples/components` / `cssshow` / `svgdemo` 的文字盒子按新字号重排**（正文变大后
  几处说明文字被裁掉）。

### 新增 — 元素与 CSS：底色/文字色分家、边框、渐变、字重、行高（【格式 v13】）

- **【格式 v13】扩展绘制段**：`BinUIPaintNode`（40 字节，稀疏）——文字色、四边边框
  （宽度与颜色各自独立）、`linear-gradient` 底色、字重、行高、文字装饰线、`overflow:hidden`
  的"只裁剪"标记。不往定长的 `BinUINode` 里加字段，旧布局语义不变；链接器合并组件时同样迁移。
- **底色与文字色分家**（本轮最重要的一条）：以前只有一个颜色字段兼任两职，于是
  `div { color:#333 }` 会被当成底色用掉、`span { background:transparent; color:#333 }`
  直接让文字变透明、按钮文字恒白。现在 `background*` 是底色、`color` 是文字色，
  **并且沿元素树继承**（`body{color:#333}` 对整棵树生效）—— 浅色主题终于能写。
- **边框**：`border` / `border-width` / `border-color`（1~4 值展开）与
  `border-top/right/bottom/left`，四边各自宽度与颜色，画在节点内侧不参与布局。
  圆角元素走"环形边框"：按圆弧逐行铺薄条（直边区自动合并成一条），
  不再用 4 条轴对齐矩形去凑 —— 那样圆角按钮上会露出方框的角。
- **渐变底色**：`linear-gradient(...)`（`to <side>` / `135deg`，取首尾两色）。条带按
  "圆角区 1px 一条 + 直边区 ~32px 一条"切分，圆角渐变不会出现阶梯。
- **排版**：`font-weight`（FreeType `FT_GlyphSlot_Embolden` 合成加粗，GDI/GL/D3D 三端一致，
  测宽与绘制同步加宽）、`line-height`（倍数 / `%` / `px`）、`text-decoration`
  （`underline` / `line-through` / `overline`）。
- **选择器（补齐最大的一块表达力缺口）**：复合类 `.btn.primary`、属性选择器
  `[type=text]` / `[href^=http]` / `[class~=x]` / `[lang|=zh]`、后代/子代/相邻兄弟/后续兄弟
  组合子（`.card .title`、`.card > .title`、`h2 + p`、`h2 ~ p`）。组合子按 CSS 从右往左匹配，
  兄弟序号只统计可视元素（`head/meta/script/style` 与伪元素合成盒不算）。
- **容器默认透明**：`div/section` 等容器不再默认填 `#222222`（浅色主题的拦路虎）；
  `<button>` / `<input>` 仍保留控件底色，`<rect>`/`<circle>` 仍是 SVG 语义的黑。
- **文本节点高度估算**：未声明 `height` 的文本节点按估算行数撑开（CJK 1.0×字号、其余
  0.52×字号，行距取 `line-height`）——以前段落按单行高排，第二行就压到下一个元素身上。
- **容器撑高会推动后续兄弟**：自动高度的容器弹出时把"被内容撑高"的增量补到父容器的
  流光标上，卡片里放几行字不会再把后面的元素压住。

### 修复

- `LABEL` 写了 `background`（哪怕 `transparent`）就把 `color` 当背景色解析，导致文字消失。
- `overflow:hidden` 被当成可滚动容器（会出滚动条、响应滚轮）；现在只裁剪不滚动。
- GDI 画布后端的长文本不换行（硬套 `DT_SINGLELINE`），与 GPU 后端表现不一致。
- `binui_compile` 把每条编译警告打印两遍（编译器自己写 stderr + 工具再打 warnings）。
- `offscreen_render` 保存 PNG 后以 `0xC0000005` 退出：GDI+ 的 `Bitmap(HBITMAP,…)` 不拷贝像素，
  却让 `Bitmap` 活过了 `DeleteObject(dib)`；现在先析构再删。
- **hover 放大时边框被"削掉一半"**：`invalidateNode` 只按布局矩形失效，`transform:scale()`
  长出来的那一圈（边框/底色/焦点环）永远不在重绘区里，于是只留下缩回前的残影。
  新增 `renderer.invalidRect()`（布局矩形 ∪ 动画缩放/平移包围盒 + pad），9 个示例统一改用它。
- **键盘焦点环的圆角写死成 10**（三个后端都是），圆角 8 的按钮上会套出一个半径不同的环，
  看着像"双线框"；现在跟随节点自身圆角与内缩。
- Makefile 的头文件依赖只覆盖了部分输出目录，`backends/`、`platform/`、`common/`、`bin/`
  改了头文件不会重编；补齐后 `make` 的结果才可信。

### 新增 — 组件化：`.bui` / `.bum` / `.bum.xml`（【格式 v12】）

- **命名与分发**：一个界面 = 一个 `.bui`（组件全内联，运行期零依赖）；要拆开就是
  `.bui` + 若干 `.bum`。组件可以是二进制 `.bum`、源码 `.bum.xml`，也可以两种都放；
  作者还能按组件混放。旧名 `.binui` 仍被识别。
- **加载优先级**：二进制优先；**源码 mtime 更新时改用源码并告警**——改源码保存后
  `--watch` 直接生效，不会因为忘了重编 `.bum` 而“改了没反应”。规则只在
  `include/binui_asset.h` 里实现，编译器与运行期加载器共用。
- **布局链接器**（`include/binui_link.h` / `src/common/binui_link.cpp`）：把组件布局并进宿主——
  节点 id 重编号、坐标平移到锚点内容原点、占位节点按组件包围盒撑大、字符串表后移重算偏移、
  缩放规格/状态样式/动画/视频/资源/组件引用/名称段一并迁移；失败时宿主保持不变。
- **编译期内联（默认）**：`<use src="x.bum.xml">` 走源码展开（可传参），
  `<use src="x.bum">` 由链接器离线合并（不暴露源码）。两者产出同一个 `.bui`。
- **运行期外挂**：`<use src="x.bum" external>` 只留占位节点 + 一条组件引用记录；
  新增 `binui::ex::MountAll(renderer, dir, layout)` 与 `renderer.mountComponent()`，
  换组件不用重编主 `.bui`（皮肤/插件面板）。挂载幂等，`isComponentMounted()` 可查。
- **【格式 v12】两段新段（恒定写出，位于状态样式段之后、名称段之前）**：
  - **资源段**：`imageIndex` → 图片路径，布局自描述。外壳不必再自己保存编译期的图片列表
    （`renderer.imagePaths()` / `ex::LoadLayoutImages()`）；组件合并时资源下标在这里整体重排。
  - **组件引用段**：`external` 组件的占位节点 id 与相对路径。
- **参数**：`{{name}}` / `{{name=默认值}}`；单独编 `.bum` 时按默认值代入（裸 `{{name}}`
  留给内联替换），所以同一份 `.bum.xml` 既能被 `<use>` 传参，也能自己编成 `.bum`。
  未替换的参数占位不再触发“无法解析长度/颜色”的假警告。
- **`<use>` 对二进制组件的覆盖**：`w` / `h` / `bg` / `radius` 仍可覆盖组件根
  （其余参数在编 `.bum` 时已固化）。
- **新工具 `binui_compile`**：`.bum.xml` → `.bum`、`main.html` → `main.bui`；
  传入已是二进制的文件则原样拷贝，方便流水线统一。构建期由 `make components` /
  CMake 的 `components_bums` 目标生成，产物不落源码树。
- **`check_examples`** 覆盖 `.bum.xml` / `.bui.xml`，并校验外挂组件引用是否真的能解析到。
- 演示 `examples/components` 重做：标题栏 `titlebar.bum`、卡片 `card.bum`（二进制内联）、
  按钮 `button.bum.xml`（源码传参）、状态栏 `dock.bum`（`external` 运行期挂载），
  三种形态同屏（见 [docs/components.md](docs/components.md)）。

### 新增 — CSS 伪类 / 伪元素（【格式 v11】）

- **运行时伪类** `:hover` / `:active` / `:focus` / `:checked`：编译进新的**状态样式段**
  （`BinUIStateStyle`，16 字节/条），渲染器在指针/焦点/勾选变化时切换目标颜色 / `scale` /
  `border-radius`；节点声明了 `transition` 就走现成的过渡通道，否则瞬时。
  同时命中时的优先级：`:active` > `:focus` > `:checked` > `:hover`。
- **结构伪类**（编译期求值，零运行时开销）：`:first-child`、`:last-child`、`:only-child`、
  `:nth-child(An+B|odd|even|n)`、`:nth-last-child()`、`:first-of-type`、`:last-of-type`、
  `:only-of-type`、`:nth-of-type()`、`:nth-last-of-type()`、`:not(简单选择器)`、`:root`。
- **伪元素** `::before` / `::after`（需 `content`）：内联模式把内容并入宿主文本
  （`li::before{content:"▸ "}` 这类项目符号，同时抑制自动项目符号）；盒模式
  （带 `width/height/background*/…`）合成一个 `span` 子节点。`content` 支持字符串、
  转义（`\25B8`、`\"`、`\n`）与 `attr(属性名)`。
- 选择器特异性按 CSS 计权：伪类 +10（按 class），伪元素 +1（按元素）。
- 不支持的伪类/伪元素（`:visited`、`::first-line`、`::selection` 等）**明确打编译警告**
  并跳过，不再静默失效。
- 渲染器新增 `stateStyle(id, state)` / `activeStateStyle(id)` / `setNodeChecked(id, on)` /
  `isNodeChecked(id)` / `hitTestEx(...)`；`:hover`/`:active` 现在对**任何元素**生效
  （命中测试把「声明了该状态样式的普通节点」纳入），事件仍只对可交互节点派发。
- 新演示 `examples/cssshow`（`cssshow.exe`，支持 `--watch`）。

### 修复

- **绘制顺序改为按树遍历**（父先于子）：原先是对全部节点做一次全局排序，`position:absolute`
  的容器会被排到它自己的流式子节点**之后**，容器底色把内容整片盖掉——`<div style="position:absolute">`
  里放普通 `<span>` 这种最常见的写法直接看不见内容。现在同一父节点下按
  `(z-index, 流式先于 absolute, DOM 顺序)` 排，跨子树的 z-index 仍按层叠上下文处理。
- **GDI 后端（`src/platform/app_win32.cpp`）**：
  - 按钮/输入框/图片文本改用 `DrawTextW`（原来是 `DrawTextA`，UTF-8 被按系统代码页解释，
    中文直接叠成一团乱码）；
  - 节点颜色改为 `renderer.drawColor()`（原来自己按 hover/按下做 tint）——
    CSS 状态样式、`transition`、`@keyframes` 现在在 GDI 后端同样生效，与 GL/D3D11 一致；
  - 全透明节点不再画成黑色块（GDI 没有 alpha 合成；图片与视频帧例外）。
- **`test_renderer` 里一处逗号运算符误用**：`TEST("click empty keeps focus", (a, b))` 只检查了
  后半段，改成 `&&` 后断言才真正生效（当前行为是正确的，属于测试自身的 bug）。
- **非 Windows 平台拿不到任何字体**：`registerSystemFonts` 只有 Windows 分支，默认字形名还写死
  `Segoe UI` → Linux/macOS 上所有绘制测试都无字形。补了常见路径探测（Noto CJK / WenQuanYi /
  DejaVu / macOS PingFang·Helvetica）并把 `Segoe UI` 别名到实际可用字体。
- **`binui_charts.cpp` 无条件 include 后端头**（含 `windows.h`）导致核心库在非 Windows 编不过；
  现在 `LineChart::draw` 的后端绘制按 `_WIN32` 隔离，其他平台给空实现（vtable 仍完整）。
- **盒模式伪元素改用 `div`（`BINUI_LIST`）**：`LABEL` 在三个后端里都只画文字，
  `content:""` 的纯装饰盒子（分隔条/色块）会完全看不见；未声明 `background` 的盒模式
  伪元素按透明处理并警告（否则会冒出默认灰底）。
- **`:hover` 之类的选择器之前永远不会触发**：命中测试只返回 `BINUI_INTERACTIVE` 节点，
  普通 `div`/`span`/`p` 进不了 hover 状态机 —— 新增 `hitTestEx`，把声明了状态样式的
  普通节点纳入命中。
- **状态样式被无伪类规则污染**：提取状态目标时未校验「规则是否真的带该伪类」，
  导致 `.row:nth-child(odd)` 这类规则在四个状态位下全部命中，把基础 `border-radius`
  也写成状态目标（按钮 hover 时圆角被重置）。修法：只有带对应状态伪类的规则参与。

### 项目结构

- 补齐开源项目骨架：`LICENSE`（MIT）、`CHANGELOG.md`、`CONTRIBUTING.md`、
  `SECURITY.md`、`CODE_OF_CONDUCT.md`、`.editorconfig`、`.gitattributes`；
- `.github/`：CI（Linux GCC + Windows MinGW-w64）、issue 表单、PR 模板；
- **目录分层**（原来后端与工具散在 `src/`、`examples/` 根目录）：
  - `src/backends/`：`binui_gl-gdi.*`（OpenGL 3.3）、`binui_d3d11.*`（D3D11）、GL 扩展头；
  - `src/platform/`：`app_win32.*`——Win32 窗口外壳 + GDI 绘制（演示与 `tools/` 共用）；
  - `include/binui_paths.h`、`include/binui_hot_reload.h`：两个 header-only 应用工具从
    `examples/` 提升为公共头（可直接被第三方外壳 `#include`）；
  - `examples/assets/noela.png`：1.65 MB 演示立绘从 `tests/` 移出（测试目录不该放素材）；
  - `docs/mockups/`：两套设计稿从 `examples/*/mockup.html` 移到文档区，不再混在演示目录里；
  - 演示资源一律 `examples/assets/` → 构建时 `cp` 到各 demo 目录（按原名引用）。
- **修复 `make` 的资源拷贝静默失败**：`cmd /c copy /Y examples\x\y build-mingw\bin\x\y`
  在 `SHELL=sh`（README 里给的构建命令）下反斜杠被 sh 当成转义吃掉，cmd 只打印 banner 就退出——
  演示 UI 其实从未被拷到 `build-mingw/bin/<demo>/`，程序一直回退到内置默认布局。
  改为 `cp -f` + 正斜杠路径（`xcopy` → `cp -rf`）。
- `tools/`：离屏渲染 / 窗口抓取 / 窗口枚举 / 示例 UI 零警告检查（`check_examples`，已进 ctest）；
- `scripts/fetch-deps.sh` + `make deps`：第三方源码不入库（约 130 MB），一条命令拉取；
- `docs/img/`：文档用图（默认布局、`cssshow` 离屏渲染、字体渲染对比）。

### 变更

- `BINUI_VERSION` 10 → 11（新增状态样式段，恒定写出，位于视频段之后、名称段之前）。
  所有 `.binui` 需重新编译（本项目的 UI 均在运行时从 HTML 编译，无兼容负担）。
- README 补齐 CSS 支持矩阵与伪类/伪元素语义说明。

## [0.0.2]

- 增加 `examples/` 下多套演示（animation / svg / canvas / video / 多窗口 / galgame 前端）。
- 抽出 `include/binui_paths.h`（exe 真实路径 + UI 查找顺序 + 资源加载）与
  `include/binui_hot_reload.h`（`--watch` 热重载，保留窗口/滚动/焦点/输入内容）。
- 二进制布局演进：【v6】`min/max` 尺寸约束、【v7】动画段、【v8】名称表、【v9】视频描述段、【v10】`zIndex`。

## [0.0.1]

- 首个可运行内核：XML / HTML5 → binUI → 节点树 → GDI / D3D11 / OpenGL 三个绘制后端。
- 纯 C++17 核心（编译 / 解析 / 命中测试 / 事件分发无平台 API 依赖）。
- FreeType + HarfBuzz 字体服务：灰度字形图集、COLR/CPAL 彩色 emoji、HarfBuzz 整形。
- 二进制布局：【v3】缩放规格段（`setSize` 重排）、【v4】节点 `radius` 圆角、【v5】窗口描述段。
- 高层图表 API（`binui_charts.h`）。
