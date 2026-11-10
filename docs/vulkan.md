# Vulkan 后端调研（2026-11）

> 结论先行：**现在不做后端，先做 2~3 天的 `binui_gl_common` 拆分**。Vulkan 在 binUI 这个场景
> （7 个绘制原语 / 一张字形图集 / 一个 scissor / 一张持久累积帧）性能收益≈0，复杂度收益是负的；
> 它唯一的价值是**平台策略**（Android 长期、显式同步、未来接外部 GPU 纹理）。而"一套 GPU 后端
> 覆盖全平台"这个目标在 2026 年反而更难了：Khronos 已把 MoltenVK 标记为 deprecated。
> 真要做，首版 20~30 人日、成熟度 40+ 人日，且**不会比 GL 跨平台路线更便宜**（同一量级）。

本文是把"要不要上 Vulkan"这件事查到底的记录，含本机实测、仓库现状盘点、Windows 呈现的真实坑、
原语映射表、工作量与风险。**GO/NO-GO 条件在第 9 节**，不想看细节可以直接跳。

---

## 1. 本机可验证的事实

| 项 | 实测 |
|---|---|
| Vulkan loader | `C:\Windows\System32\vulkan-1.dll` 1.4.341（另有 `vulkan-1-999-0-0-0.dll`） |
| `vulkaninfo.exe` | 存在，系统自带，`Instance Version: 1.4.341` |
| GPU | GPU0 = NVIDIA GeForce RTX 4060 Ti，`apiVersion = 1.4.341`，`driverName = NVIDIA`，driverVersion 610.88 |
| 实例扩展 | `VK_KHR_surface`(25)、`VK_KHR_win32_surface`(6)、`VK_EXT_swapchain_colorspace`、`VK_KHR_portability_enumeration` 等 17 个 |
| Vulkan SDK | **没有**（`VULKAN_SDK` 未设，`C:\VulkanSDK` 不存在） |
| MinGW Vulkan 头 | **没有**（`C:\ubin\mingw64\include\vulkan\*` 不存在） |

**可行性结论**：可以**不装 SDK、不链 `vulkan-1.lib`** 接进来 —— 全库 `LoadLibrary("vulkan-1.dll")` +
`vkGetInstanceProcAddr` + `#define VK_NO_PROTOTYPES` + 一份最小声明头。这与仓库现有
[`src/backends/binui_gl_ext_min.h`](../src/backends/binui_gl_ext_min.h)（47 行，GL 扩展存根）是同一路子，
预计最小声明头 250~400 行（枚举/结构体/16~24 个函数指针）。**这条不是障碍**。

---

## 2. 仓库现状：真正的成本在"三份遍历"，不在 Vulkan

| 后端 | 文件 | 总行数 | 节点树遍历（`drawNodes*`） |
|---|---|---|---|
| GDI | [`src/platform/app_win32.cpp`](../src/platform/app_win32.cpp) | 1384 | `drawNodesGDI` @635 → 约 749 行，7 个 case |
| D3D11 | [`src/backends/binui_d3d11.cpp`](../src/backends/binui_d3d11.cpp) | 2600 | `drawNodesD3D` @1656，**530 行**，9 个 case |
| OpenGL | [`src/backends/binui_gl-gdi.cpp`](../src/backends/binui_gl-gdi.cpp) | 3455 | `drawNodesGLGdi` @1882，**694 行**，9 个 case |

两份 GPU 遍历的 case 标签完全一致（`LABEL/BUTTON/INPUT/LIST/IMAGE/VIDEO/SVG/CANVAS/PATH`），
调用的原语次数也基本一致（`fillRect` 12 vs 9、`drawText` 4 vs 4、`pushClip` 7 vs 6……）。

而两个 gfx 类的**公开方法签名逐条相同**（[`binui_gl-gdi.h`](../src/backends/binui_gl-gdi.h) 32~78 行 vs
[`binui_d3d11.h`](../src/backends/binui_d3d11.h) 28~70 行）：

```cpp
bool init(void* hwnd, int width, int height, bool vsync = true);
void resize(int w, int h);  void beginFrame(const void* clipRect);  void present();
void fillRect/fillRounded/fillLine/strokeRounded/drawImage/drawText/drawTextWrapped(...);
int  pushClip(...);  void popClipTo(int depth);
```

**含义**：抽一个 `IBinUIGfx`（9 个纯虚 + 6 个生命周期方法）是**机械工作**，约 2~3 天；
抽完之后 GL/D3D11/Vulkan/Metal 共用同一份遍历，Vulkan 后端就变成"新增一个文件"而不是"再抄 600 行"。
**这笔钱不管做不做 Vulkan 都该花**（现在三份遍历里同一处 UI 语义要改三遍，v13 扩展绘制段就是这样）。

另外两个已有的有利条件：
- 几何数学已经在 [`include/binui_paint.h`](../include/binui_paint.h)（1146 行）里与后端无关：路径填充条带、
  圆角九宫格、渐变取样、描边回调。Vulkan 后端直接复用。
- 三个后端**都不直接画进交换链**，而是画进一张持久离屏图再整帧贴出去（GL：`msFbo → offTex → 默认帧缓冲`；
  D3D11：`offTex → backbuffer`）。所以"脏矩形局部重绘"在 Vulkan 里不是新问题（见 §6）。

---

## 3. Windows 呈现：独立翻转（Independent Flip）会同时咬 GL 和 Vulkan

binUI 刚踩过的坑（0.9.1 修的）：客户区**正好铺满整块显示器**时，DWM 把 GL 的 `SwapBuffers` 提升成
独立翻转，应用仍在 ~100fps 绘制（MSAA 解析正常、脏矩形正确），屏幕却停在旧帧。

调研得到的结论：**这不是 GL 独有的，Vulkan 会以同样方式中招。**

- 独立翻转的触发条件就是"交换链缓冲尺寸 == 屏幕尺寸 + 客户区覆盖整屏"（[MS DXGI flip model 文档](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)：提升后 "DWM can go to sleep"，帧直送扫描输出、绕过合成）。**确定。**
- Vulkan on Windows 是否走 DXGI flip model **由驱动决定**（NVIDIA 面板的 "prefer native" vs "layer on
  DXGI swapchain"），所以"Vulkan 一定走 DXGI flip"不成立。**较可信。**
- 但已有直接实证：[SDL #16497](https://github.com/libsdl-org/SDL/issues/16497)（RTX 3060 / 驱动 617.42 /
  Win11）报告 Vulkan 交换链在"正好铺满显示器的窗口"上 alpha 丢失、"跟 GL 一样"。**较可信。**
- `VK_PRESENT_MODE_FIFO/MAILBOX/IMMEDIATE` 不是决定因素（那是同一条底层交换链的排空策略）。
- 采集/远程桌面层面：独立翻转绕过 DWM 合成，Desktop Duplication / WGC 只能拿到 DWM 合成结果 →
  采集端可能看到停帧或黑帧（[ForceComposedFlip](https://github.com/fernandoenzo/ForceComposedFlip) 这个项目就是为此存在）。**较可信。**
- 远程桌面 / 虚拟显示器（向日葵 IDD、MuMu）下 IF 是否必然停帧：**无直接实证，不确定**。

**规避手段（按性价比）**：
1. 客户区物理尺寸 ≠ 显示器尺寸（少 1~2px、只铺工作表区）；对应 binUI 的 `clientCoversMonitor()` 判定，
   现在已经在"铺满就切 GDI 贴屏"。
2. 读回到 DIB 贴屏（binUI 已有 `presentViaGdi`），Vulkan 需要等价旁路。
3. **不要**用 `VK_EXT_full_screen_exclusive`——它更彻底地绕开 DWM，采集/远程只会更糟。
4. `VK_KHR_present_id/present_wait` 只解决节流与延迟统计，不解决停帧。

**另一个硬约束**：[VK_KHR_win32_surface 的 Issue 2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_win32_surface.html)
明确写"同一个 HWND 被多个图形 API 使用 = 未定义行为"，并且**创建过 DXGI flip-model 交换链的窗口可能再也
建不出 `VkSwapchainKHR`**（反之亦然）。所以 Vulkan 后端必须"一个窗口一个呈现 API"，不能像现在 GL 那样
在同一条窗口上随时在 `SwapBuffers` 与 `StretchDIBits` 之间切（binUI 现在这么做只是因为 GL 的 `SetPixelFormat`
与 D3D/ Vulkan 的冲突还没被触发，属于**已知的技术债**，写进 §8 风险）。

---

## 4. 透明窗口：实用路线只有"读回"，不要押注交换链 alpha

binUI 的透明窗口在 GL 端是：离屏 premultiplied 读回 → `UpdateLayeredWindow`（`WS_EX_LAYERED` + `AC_SRC_ALPHA`）。

| 路线 | 可行性 | 成本/风险 |
|---|---|---|
| ① `vkCmdCopyImageToBuffer` 读回 → DIB → `UpdateLayeredWindow` | **可行，与 GL 同构** | 每帧一次 GPU→CPU 同步；1080p BGRA ≈ 8.3 MB/帧，100fps ≈ 0.8 GB/s；延迟 +1~2 帧 |
| ② `VK_KHR_external_memory_win32` 导出为 D3D11 texture 再配 layered window | **不行** | `UpdateLayeredWindow` 只吃 HDC+DIB，没有接收 DXGI surface 的入口 |
| ③ 交换链 `VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR` + 普通窗口 + `DwmEnableBlurBehindWindow` | 能跑，但**只有 33% 的 Windows 设备报告支持**（[gpuinfo 统计](https://vulkan.gpuinfo.org/listsurfacecompositealphamodes.php?platform=windows)），且铺满屏被 IF 提升后 alpha 照样丢 | 不建议 |
| ④ DirectComposition（`CreateSwapChainForComposition` + `WS_EX_NOREDIRECTIONBITMAP`） | 链路存在但没有 "Vulkan → IDCompositionSurface" 直连，要过 D3D11 | 复杂度高、收益未知 |

**结论**：透明窗口继续走路线 ①。Vulkan 后端要额外实现"读回 + DIB 双缓冲"，约 80~150 行，
但**绝不该为了透明去换架构**。

---

## 5. 原语映射表（7 个原语 + 5 个机制）

| 原语/机制 | GL 现状（`binui_gl-gdi.cpp`） | Vulkan 做法 | 新增复杂度 |
|---|---|---|---|
| `fillRect` | `pushQuad` 进顶点批次（`verts`/`batches`），`flushAll` 一次画 | 同一套顶点数据 → `vkCmdDraw`；顶点缓冲按帧 `vkCmdUpdateBuffer`/映射内存环 | 低（批次逻辑可直译） |
| `fillRounded` / `strokeRounded` | 九宫格模板 + 1 张白纹理 + 顶点色 | 同（同一张图集槽位） | 低 |
| `fillLine` | 旋转四边形（任意角度实心线段） | 同（Vulkan 没有 line 光栅化的 1px 限制问题） | 低 |
| `drawText` / `drawTextWrapped` | FreeType 字形图集 `glTexSubImage2D` 增量上传 | 图集 `VkImage` + staging buffer + `vkCmdCopyBufferToImage` 子区域 + **显式 barrier**（GPU 还在读旧内容时不能盖） | **中**（GL 是隐式同步） |
| `drawImage` / `prepareImage*` | 每张图一个 GL 纹理 | 每张图一个 `VkImage` + 每纹理一个 `VkDescriptorSet` | 中 |
| `pushClip` / `popClipTo` | `glScissor` 求交 + 批次键 | `vkCmdSetScissor`（必须先 clamp 到 viewport，否则 validation 报错） | 低 |
| 批次 flush | 按 scissor/纹理切 batch | 按 pipeline/descriptor/scissor 切 command buffer 记录 | 低 |
| MSAA 4x | `glRenderbufferStorageMultisample` + `glBlitFramebuffer` | MSAA image + `vkCmdResolveImage`（或 dynamic rendering 的 resolve attachment） | 低 |
| 持久累积帧 + 脏矩形 | `beginFrame(clipRect)` → scissor + `glClear` | 同一张 MSAA image 上 `LOAD_OP_LOAD` + `vkCmdClearAttachments` 局部清（**不能用 `vkCmdClearColorImage`，它忽略 scissor**） | 低（架构不变） |
| 交换链 / resize | 没有（默认帧缓冲，`resize()` 只有 12 行） | `vkCreateSwapchainKHR` + 格式/模式/alpha 协商 + `oldSwapchain` 复用 + `OUT_OF_DATE/SUBOPTIMAL` 处理 + frames-in-flight 同步 | **高**（GL 里是 0） |
| 透明窗口 | `glReadPixels` → `UpdateLayeredWindow` | 同构读回（§4 路线 ①） | 中 |
| present 旁路 | `presentViaGdi`（读回 → `StretchDIBits`） | 同构 | 中 |

---

## 6. 局部重绘：架构不变，但有一条 spec 级别的红线

**结论：不要在交换链 image 上做局部重绘。** 继续用"自持持久累积 image + 脏矩形只在这张图里清/画 + present 整帧贴出"。

依据（Vulkan spec）：
- 交换链 image 内容**不保证保留**：layout 转换时 `oldLayout` 要么等于当前布局，要么是
  `VK_IMAGE_LAYOUT_UNDEFINED`，而后者明确"内容无需保留"（[spec, Resources 章](https://docs.vulkan.org/spec/latest/chapters/resources.html)）。
  照抄 tutorial 的"每帧 `UNDEFINED → COLOR_ATTACHMENT`"就等于每帧从不确定内容开始 —— 脏矩形方案必然花屏。
- `clipped = VK_TRUE` 时被遮挡像素"读回内容未定义"，所以"从交换链读上一帧"这条路不可靠。
- 唯一标准化的"保留"机制是 `VK_KHR_swapchain_maintenance1` 的 `vkReleaseSwapchainImagesKHR`，
  但它只解决"不 present 就还回去"，替代不了累积帧。
- `VK_KHR_incremental_present` 是**呈现带宽**提示，不是内容保留机制。

一个额外的工程判断：Android 的 tiled GPU 上，对全屏 attachment 每帧 `LOAD_OP_LOAD` 要把整个 framebuffer
搬回 tile memory，"局部重绘 + LOAD" 很可能比"全量 `DONT_CARE` 重绘"更慢。所以 **Android 默认全量重绘**，
Windows/Linux 默认脏矩形。

---

## 7. 工作量（有依据的区间）

参考实现规模（2026-11-10 抓取）：

| 参考 | 体量 | 说明 |
|---|---|---|
| Dear ImGui `imgui_impl_vulkan.cpp` | **2051 行**（核心渲染器 1564 + 交换链 helper 487） | 只画四边形+贴图+scissor，被 8 年打磨过；changelog 里 20+ 条在修同步/present 坑 |
| SDL3 GPU `SDL_gpu_vulkan.c` | **13702 行** | 统一 GPU 抽象（公共头 `SDL_gpu.h` 4752 行）；`src/gpu` 里已**没有 D3D11 后端** |
| Qt `qrhivulkan.cpp` | **10142 行**（抽象层 `qrhi.cpp` 13848 行） | 四后端（VK/GLES/D3D11/Metal）都还在维护 |
| Skia Graphite `src/gpu/graphite/vk` | ≈1.2 万行（Ganesh `vk` ≈1.9 万行） | Graphite 只有 `mtl/vk/dawn`，GL 仅存于老架构 |

binUI 的估算：

| 阶段 | 人日 | 依据 |
|---|---|---|
| `IBinUIGfx` 抽取（遍历去重，四后端共用） | **2~3** | 接口签名已同形，纯机械 |
| Win32 Vulkan 设备核心（达到 GL 后端功能对等） | **8~12** | GL 设备部分 ~1800 行 → Vulkan ~2500 行；参照 ImGui 2051 行的门槛 |
| 驱动差异稳定化（Intel 老 iGPU / RDP / 虚拟显卡 / DPI / 多显示器 / 最小化） | **+3~5** | 对应 `docs/issues/` 里 GL 已踩过的同类问题 |
| Linux X11 壳 / Wayland 壳 / Android 壳 | **+9~14** | `docs/bex_container.md` §10 的自估（Phase 4/6 各 1~1.5 周） |
| **首版可交付（Win32 + X11 + Android）** | **20~30** | 与 GL 跨平台路线（4~6 周）**同量级** |
| 到"见过 8 个坑"的成熟度 | **40+** | 现有 GL 后端 3455 行里约 1/3 是踩坑产物（NVIDIA 崩溃绕过、帧节流、IME 挂摘、resize 挂起应用） |

---

## 8. 风险清单（概率 × 影响）

| # | 风险 | 触发条件 | 症状 | 缓解 |
|---|---|---|---|---|
| 1 | 同步错（信号量数量/生命周期） | 任何多帧飞行 | 随机黑帧、validation 报错、几分钟后卡死 | acquire 信号量数 = frames-in-flight + 1；present 信号量按 image 索引；先跑 validation 骨架 |
| 2 | 交换链重建路径 | resize/最小化/DPI 变化/远程会话切换/显示器热插拔 | 卡住、花屏、`OUT_OF_DATE` 死循环 | 只记录 `WM_SIZE` 每帧应用一次；`maxImageExtent == (0,0)` 时跳过；`oldSwapchain` 复用 |
| 3 | 独立翻转停帧（§3） | 客户区铺满显示器 + 远程/采集 | 画面停住但进程 100fps | 不铺满整屏 / 读回贴屏旁路 |
| 4 | 图集上传与使用竞争 | 字形图集增量上传 | 偶发字形错乱/闪烁 | barrier + 帧间延迟销毁旧 set |
| 5 | 同一个 HWND 多 API（§3 末尾） | 同窗口既 Vulkan 又 GDI 贴屏 | 交换链建不出来 / 画面整块不更新 | **一个窗口一个呈现 API**；Vulkan 窗口若要贴屏，必须先销毁交换链 |
| 6 | macOS 目标 | 需求含 macOS | MoltenVK 已 deprecated（社区 best-effort，v1.4.2 仍在发版）；Khronos 推荐 KosmicKrisp（2026 仍很新） | macOS 明确走 Metal，不并入 Vulkan |
| 7 | 没有 VMA 时的内存管理 | 图集/图片增多 | 碎片、分配失败 | 自写 ~200 行"每资源独立分配 + staging 环"，够用（总占用 ~50 MB 量级） |

---

## 9. 决策：GO / NO-GO

### 现在：**NO-GO**

三条理由：
1. **收益不成立**：几千个四边形的 2D UI，任何 GPU API 都是微秒级；Vulkan 的显式同步在 binUI 这个规模上
   只会增加 bug 面。
2. **"一套覆盖全平台"不成立**：macOS 那条腿 2026 年比 2024 年更难（MoltenVK deprecated）；
   Linux/Android 用 EGL+GLES3 是二十年的老路。
3. **顺序不对**：binUI 连 Linux 外壳都还没有（README "跨平台方向"自陈只支持 Windows）。**外壳是三条路都必须付的成本，
   后端是可换的**——先付不可替代的那部分。

### 翻转成 GO 的条件（满足 ≥2 条）

1. 出现**硬需求**：目标设备只接受 Vulkan（Android TV / ChromeOS / Vulkan-only 嵌入式），或要接外部 GPU 内存
   （DMA-BUF / 共享纹理 / 视频帧零拷贝合成）—— 这些在 GL 上要绕更远的路。
2. 已在 Linux/Android 用 GL 跑通，并积累 **≥2 个"GL 驱动问题绕不过去"的具体实例**（具体 bug，不是"更现代"）。
3. 动手的人**独立写过** Vulkan 1.3 的 frame-in-flight + swapchain 重建，能自己读 VUID 定位问题。
4. 能接受这 20~30 人日期间 GL 路线不推进，且 macOS 明确走 Metal。

### 无论走哪条路，现在就该做的三件事

1. **抽 `IBinUIGfx`**（2~3 天）：把 §2 那三份遍历合成一份。这是四后端（含将来的 Metal）共用的地基，
   也是"以后接 Vulkan 只加一个文件"的前提。
2. **守住后端契约**：`beginFrame(clipRect)` → 只清/画脏区 → `present()` 整帧；所有后端都不许直接
   在交换链上做局部重绘。契约在，后端就是可替换件。
3. **可选的 3 天验证 spike**（只在想提前去风险时做）：`binui_vk_ext_min.h` + 一个 300 行的
   "开窗 + 清屏 + 画一个圆角矩形"程序，验证 §1 的"loader-only 接入"和 §3 的"铺满屏是否停帧"在本机成立。
   不做也不影响任何现有功能。

---

## 附：本次调研的来源

- 本机：`vulkaninfo --summary`（loader 1.4.341 / GPU0 RTX 4060 Ti / apiVersion 1.4.341）。
- 仓库：`src/backends/*`、`src/platform/app_win32.cpp`、`include/binui_paint.h` 的行号与签名实测。
- 外部：
  [MS DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)、
  [VK_KHR_win32_surface（Issue 2：多 API 共用 HWND）](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_win32_surface.html)、
  [SDL #16497（Vulkan 全屏 alpha/停帧）](https://github.com/libsdl-org/SDL/issues/16497)、
  [SDL #15751（NVIDIA Vulkan layered 呈现）](https://github.com/libsdl-org/SDL/issues/15751)、
  [gpuinfo：Windows 上 PRE_MULTIPLIED 支持率](https://vulkan.gpuinfo.org/listsurfacecompositealphamodes.php?platform=windows)、
  [Vulkan spec：Resources（内容未定义）](https://docs.vulkan.org/spec/latest/chapters/resources.html)、
  [Vulkan spec：WSI](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html)、
  [MoltenVK（deprecation notice）](https://github.com/KhronosGroup/MoltenVK)、
  [VK_KHR_wayland_surface](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_wayland_surface.html)、
  [VK_EXT_metal_surface](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_metal_surface.html)。
