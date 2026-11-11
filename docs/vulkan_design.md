# Vulkan 后端设计（P1~P5）

> 上游调研：[`docs/vulkan.md`](vulkan.md)（结论 NO-GO，条件见该文第 9 节）。
> 本文是**真要做时**的落地设计：接口、模块、同步、呈现、阶段门槛、时间线、回滚点。
> 一句话：**先花 2~3 天把三份节点遍历合成一份（P1），Vulkan 后端才变成"新增一个文件"**；
> 之后 Windows-only 首版 **12~17 人日**，跑通后再谈 Linux/Android。

---

## 0. 范围

| 项 | 本期做 | 说明 |
|---|---|---|
| 平台 | **Windows only** | Linux/Android/Metal 一律不做，理由见调研文档 §9 |
| 后端选择 | GL 仍是默认 | Vulkan 用 `GPU=vk` 构建 + `BINUI_GPU=vk` 运行期开关，双轨并存 |
| 核心 | **零改动** | `binui_renderer` / `binui_compiler` / `binui_paint` 一行不改 |
| 功能对等 | 7 个原语 + 4x MSAA + 脏矩形 + 透明窗 + 多实例 + 热重载 | 与 GL 后端逐项对齐 |

---

## 1. 必须守住的六条契约

这六条是"后端可替换"的地基，Vulkan 设计里任何取舍都不能破坏它们：

1. **绘制只走 7 个原语**（`fillRect/fillRounded/fillLine/strokeRounded/drawImage/drawText/drawTextWrapped`）
   + `pushClip/popClipTo`。新后端不许要求渲染器改调用方式。
2. **字符帧**：`beginFrame(clipRect)` → 只在 `clipRect` 内清屏并绘制 → `present()` 整帧贴出。
   **不许**在交换链 image 上做局部重绘（Vulkan spec：交换链内容不保证保留，见调研 §6）。
3. **颜色约定**：`0xAARRGGBB`，着色器输出预乘 alpha，混合 `(ONE, ONE_MINUS_SRC_ALPHA)`，
   不启用 sRGB 帧缓冲。→ 保证与 GDI/GL 像素可比。
4. **失败降级**：初始化失败 → `isReady() == false`，所有原语变 no-op，宿主窗口照常跑（GL 现在就是这样）。
5. **不阻塞**：没有脏区时 CPU 必须睡在消息等待上（GL 的 `MsgWaitForMultipleObjectsEx` 自节拍）。
6. **一个窗口一个呈现 API**：Vulkan 窗口要么走交换链 present，要么走读回贴屏，
   同一时刻只有一个（`VK_KHR_win32_surface` Issue 2：多 API 共用 HWND = UB）。

---

## 2. 阶段总览

| 阶段 | 内容 | 人日 | 验收门槛（可执行） | 回滚点 |
|---|---|---|---|---|
| **P1** | `IBinUIGfx` + 共享节点遍历（GL+D3D11 先并入） | 2~3 | `make pixels` 10/10 零差异；`gl_shot` 与改前逐像素一致（记录 diff 数值）；四套测试全绿 | 纯重构，`git revert` 即可 |
| **P2** | Vulkan 骨架：实例/设备/离屏目标/管线/批次/`fillRect`+`fillRounded`+`present`（读回） | 3~4 | `vk_shot` 渲染出"色卡 + 圆角块"，与 GDI 基准 diff ≤ 阈值；跑 10 分钟不崩 | 不接进 demo，仅工具 |
| **P2b** | 窗口 + 交换链 + 三路 present + 全屏直翻转守卫 + 透明窗读回（`vk_window` 验收工具） | 1~2 | 交换链图像读回与 GL 同尺寸输出 0 差异；resize/全屏/透明三条路径各自可跑 | 删除 swapchain 代码即回到 P2 |
| **P3** | 全原语：线段/文本图集/图片/裁剪栈/MSAA 累积帧（三路 present 归到 P2b） | 4~5 | 10 个像素用例全部出图并给三方 diff 表；`svgdemo`/`features`/`fontshow` 跑通 | demo 仍用 GL，`GPU=vk` 才切 |
| **P4** | 鲁棒性：resize/最大化/最小化/DPI/多显示器/RDP/多实例/热重载/验证层 | 2~3 | 8 个 demo × 3 圈操作无停帧、无 validation error、无泄漏 | 保留 P3 版本 |
| **P5** | 工具与发版：`vk_shot`、`GPU=gl\|vk`、文档、CHANGELOG | 1~2 | `make pixels GPU=vk`、`make check_examples` 全绿 | — |
| | **合计** | **12~17** | | |

依赖关系：P1 → P2 → P3 →（P4、P5 可部分并行）。

---

## 3. P1：`IBinUIGfx` 与共享遍历

### 3.1 接口（新建 `src/backends/binui_gfx.h`）

```cpp
namespace binui {

// 后端绘制契约。任何实现都必须满足 docs/vulkan_design.md §1 的六条。
class IBinUIGfx {
public:
    virtual ~IBinUIGfx() = default;

    // ---- 生命周期 ----
    virtual bool isReady() const = 0;
    virtual int  width() const = 0;
    virtual int  height() const = 0;
    virtual void resize(int width, int height) = 0;

    // ---- 帧 ----
    virtual void beginFrame(const void* clipRect) = 0;   // clipRect = int[4]{l,t,r,b}，nullptr = 全窗
    virtual void present() = 0;

    // ---- 原语（颜色 0xAARRGGBB）----
    virtual void fillRect(float x, float y, float w, float h, uint32_t color) = 0;
    virtual void fillRounded(float x, float y, float w, float h, float radius, uint32_t color) = 0;
    virtual void fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) = 0;
    virtual void strokeRounded(float x, float y, float w, float h, float radius,
                               float thickness, uint32_t color) = 0;
    virtual void drawImage(uint16_t index, float x, float y, float w, float h) = 0;

    // ---- 文本（x/y 为基线；centerH/centerV 时 x/y 为矩形中心）----
    virtual void drawText(float x, float y, float size, int weight,
                          const std::string& utf8, uint32_t color,
                          bool centerH = false, bool centerV = false, bool italic = false) = 0;
    virtual void drawTextWrapped(float x, float y, float maxW, float size, int weight,
                                 const std::string& utf8, uint32_t color, int align = 0,
                                 bool italic = false) = 0;

    // ---- 图片（generation 不一致时重新上传）----
    virtual bool prepareImage(uint16_t index, const uint8_t* data, size_t size,
                              uint32_t generation) = 0;
    virtual bool prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                                 uint32_t generation) = 0;

    // ---- 裁剪栈（与当前 scissor 求交，返回进入前深度）----
    virtual int  pushClip(float x, float y, float w, float h) = 0;
    virtual void popClipTo(int depth) = 0;
};

} // namespace binui
```

**不放进接口的**（留在具体类里）：`init/shutdown/setVsync/setTransparent/presentViaGdi/setFontName/
textWidth/fontMetrics` —— 前四个是窗口/呈现层的事，后三个只有渲染器与命中测试用，遍历用不到。

### 3.2 共享遍历（新建 `src/backends/binui_draw_nodes.cpp`）

把 `drawNodesGLGdi`（[`binui_gl-gdi.cpp:1882`](../src/backends/binui_gl-gdi.cpp)，694 行）与
`drawNodesD3D`（[`binui_d3d11.cpp:1656`](../src/backends/binui_d3d11.cpp)，530 行）合并成一份：

```cpp
void drawNodesBinUI(IBinUIGfx& gfx, const BinUIRenderer& renderer, const void* clipRect,
                    const BinUIDrawHooks& hooks);   // hooks: canvas 回调 / video 回调（可空）
```

两者当初差额 164 行主要是各后端自己的注释与少量保底分支，合并时**以 GL 版为准**（它多 `PATH`/`CANVAS`
细节），逐段比对再定稿。迁移分三步，每步都能编能测：

1. 加接口 + `BinUIGLGdi : public IBinUIGfx`（`override` 全加），内核遍历改成调接口版；
2. `BinUID3D : public IBinUIGfx` 同样接上，删掉它那份遍历；
3. 删掉 GL 那份遍历，`binui_draw_nodes.cpp` 成为唯一实现。

### 3.2a P1 落地记录（2026-11-10，已完成 GL 一半）

- 新增 [`src/backends/binui_gfx.h`](../src/backends/binui_gfx.h)：`IBinUIGfx` 接口全文 + `CanvasFn`。
- 新增 [`src/backends/binui_draw_nodes.h`](../src/backends/binui_draw_nodes.h)：后端无关遍历 `drawNodesBinUI`
  （从 GL 的 694 行原样搬出，只把 `BinUIGLGdi&` 换成 `IBinUIGfx&`、`ScaleAlpha` 换成共享的 `GfxScaleAlpha`）。
  放头文件里是为了**不动 Makefile**（后端对象是按目标逐个链接的，加 .cpp 要改十几条规则）。
- `BinUIGLGdi : public IBinUIGfx`；GL 侧只留一个薄壳 `drawNodesGLGdi`（把具体回调适配成 `CanvasFn`），
  demo/tools 一行没改。
- **像素中性验证**：`make pixels` 10/10 零差异；10 个用例的 `gl_shot` 出图与改前**逐像素 0 差异**；
  四套测试全绿；`check_examples` 11/11。

### 3.2b 发现的意外：D3D11 后端根本没在构建里

P1 复查时发现 `binui_d3d11.cpp`（2600 行）**既不在 Makefile 也不在 CMakeLists** 里 —— mingw32-make 与
CMake 都不编译它，也没有任何 demo/tool 使用 `BinUID3D`。所以：
- 仓库里"三端一致"的说法，在构建层面实际只有 **GDI + GL 两端**；
- 把它并入共享遍历**当前无法验证**（没有出图工具、没有窗口壳），因此 P1 只完成 GL 那一半，
  D3D11 的并入推迟到"它真的重新进入构建"时再做（届时直接用 `drawNodesBinUI` 替换它自己那份）。

### 3.3 GDI 为什么不并进来

`drawNodesGDI`（[`app_win32.cpp:635`](../src/platform/app_win32.cpp)，约 749 行）与 GPU 版**语义不同**：
它按条带自己算 `BlendOver(颜色, 底色)`（没有真 alpha），文字走 `DrawTextW`，裁剪是 `IntersectClipRect`。
硬套接口会把"GDI 的近似混合"塞进 GPU 后端的契约里。**本期不动它**；将来若要并，先给 GDI 写一个
`GdiGfx` 适配器并把混合语义标成 `capabilities` 位，那是另一件事。

### 3.4 像素中性验证（P1 的硬门槛）

```sh
mingw32-make -j8 all tools
mingw32-make pixels                                  # 期望 10/10 零差异（GDI 路径）
build-mingw/bin/tests/test_elements.exe              # 期望全绿
build-mingw/bin/gl_shot.exe docs/issues/assets/svgfinish.html /tmp/after.png 420 260
build-mingw/bin/pixel_diff.exe docs/pixels/svgfinish.png /tmp/after.png 0.5   # 与改前记录的数值一致
```
改前先把 `gl_shot` 的 10 个用例结果留一份（`build-mingw/tmp/p1_before/*.png`），改后逐个比，
**不允许出现任何新增差异**——P1 是纯重构，一像素都不该动。

---

## 4. P2：Vulkan 骨架

### 4.1 文件与模块

| 文件 | 职责 | 预估行数 |
|---|---|---|
| `src/backends/binui_vk_ext_min.h` | `VK_NO_PROTOTYPES` 下的枚举/结构体/18~24 个函数指针 | 250~400 |
| `src/backends/binui_vk.cpp` | `BinUIVK : IBinUIGfx`（设备/交换链/管线/批次/原语/呈现） | 1800~2400 |
| `src/backends/binui_vk.h` | `BinUIVK` + `BinUIAppVK`（窗口/事件/自节拍，镜像 GL 版） | 250~300 |

内部对象（都在 `BinUIVK::Impl` 里，PIMPL 与 GL 版一致）：

```
Impl
├── VkInst    : instance, debug messenger(仅开发), physical, device, queue, queueFamily
├── VkSwap    : swapchain, images[], views[], format, presentMode, extent, imageUsage
├── VkFrame[2]: cmdPool, cmd, imageAvailableSem, fence            // frames-in-flight = 2
├── VkImgSem[n]: renderFinishedSem（per swapchain image）
├── VkAccum   : msaaImage + msaaView（持久，LOAD_OP_LOAD）, resolveImage(单采样离屏，可选)
├── VkBatch   : 顶点环 3×256KB（每帧一块，host-visible）+ draw 列表 + 当前 scissor
├── VkAtlas   : 字形图集 image（R8）+ 白纹理 + 图片 image[]
└── VkDs      : descriptorSetLayout / pool / 每纹理 set
```

### 4.2 同步模型（最容易出 bug 的地方，先定死）

> **as-built（v0.10.1）**：实际落地的是"CPU 双槽 + GPU 单帧"—— 渲染目标帧间共享，
> GPU 上只允许一帧在跑，所以提交前等上一帧、录制与执行重叠。见 §7.2。
> 下面这段是最初按通用 frames-in-flight 写的设计，`renderFinished` 按 image 索引那条
> 我们没采用（每槽一对信号量即可），保留原文备查。

```
frames-in-flight = 2                      // 同时最多 2 帧在 GPU 上
imageAvailableSem  : nFrames=2            // 每帧一个
renderFinishedSem  : nImages               // ★ 按交换链 image 数，不是按帧
fence              : nFrames=2            // 每帧一个，present 后等它再复用 cmd/顶点环
```

规则（照 ImGui 2024-01-19 的修复，见调研 §2）：
- `vkAcquireNextImageKHR` 的等待信号量 = `imageAvailable[frame]`，**总数要比 frames-in-flight 多**，
  否则 validation 报 "semaphore is already in use"；
- `renderFinished` 必须按 image 索引取，因为同一帧的两个 present 可能在不同 image 上；
- `vkQueuePresentKHR` 之后**不要** `vkDeviceWaitIdle`（会把 CPU 挂住，违背契约 5）；
- 顶点环按帧复用：`vkWaitForFences(fence[frame])` → `reset` → 映射写入。

伪代码（每帧）：

```cpp
beginFrame(clip):
    waitForFences(fence[frame]); resetFence(fence[frame]);
    acquire(swap, imageAvailable[frame], &imageIndex);      // OUT_OF_DATE/SUBOPTIMAL → 重建后重试
    reset(cmd);
    beginRendering(msaaView, loadOp = firstUse ? CLEAR : LOAD, scissor = clip);
    if (clip) vkCmdClearAttachments(cmd, scissor=clip);      // ★ 不能用 vkCmdClearColorImage

present():
    flushBatches();  endRendering();
    resolve(msaaView → target);                              // 交换链 image（若支持）或离屏，再 blit
    submit(cmd, wait=imageAvailable[frame], signal=renderFinished[imageIndex], fence=fence[frame]);
    present(swap, imageIndex, wait=renderFinished[imageIndex]);
```

### 4.3 交换链协商表

| 参数 | 取值 | 理由 |
|---|---|---|
| format | `VK_FORMAT_B8G8R8A8_UNORM` 优先；`VK_FORMAT_R8G8B8A8_UNORM` 次之；**SRGB 一律不用** | binUI 不做 sRGB 转换（GL/GDI 同）；若只有 SRGB → 走读回贴屏并告警 |
| colorSpace | `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` | 与 DDR/DWM 一致 |
| presentMode | vsync ? `FIFO` : (`IMMEDIATE`（支持 `ALLOW_TEARING` 才开）否则 `MAILBOX` 否则 `FIFO`) | 对齐 `setVsync(bool)` |
| compositeAlpha | `OPAQUE`（不支持则依次试 `INHERIT/ PRE_MULTIPLIED`） | 透明窗口走读回，不用交换链 alpha |
| imageUsage | 优先 `COLOR_ATTACHMENT\|TRANSFER_DST`，必须是 `supportedUsageFlags` 子集 | resolve 直进交换链要 `TRANSFER_DST`；不支持就 resolve 到离屏再 blit |
| imageCount | `minImageCount + 1`，上限 `maxImageCount` | 双缓冲 + 1 张在飞 |
| preTransform | 用 `currentTransform` | Windows 上一般是 IDENTITY |
| 重建 | `oldSwapchain` 复用；`currentExtent == (0,0)` 或最小化 → 跳过本轮 | 最小化时 `maxImageExtent` 可能是 0，spec 明说此时无法创建 |

### 4.4 颜色与格式（第一条验收线）

顶点结构与 GL **逐位相同**，20 字节：

```cpp
struct VkVertex { float x, y; uint32_t color; float u, v; };   // 与 GLVertex 同
// attribute 0: R32G32_SFLOAT  @0
// attribute 1: R8G8B8A8_UNORM  @8   ← 与 GL 的 glVertexAttribPointer(4,GL_UNSIGNED_BYTE,GL_TRUE) 同解释
// attribute 2: R32G32_SFLOAT  @12
```

> **字节序陷阱（必须先用色卡验证再往下做）**：`0xAARRGGBB` 在小端内存里是 `BB GG RR AA`，
> 按 `R8G8B8A8_UNORM` 读出来 r 通道拿到的是原蓝值。GL 那边靠"渲染到 RGBA8 FBO + 用 `GL_BGRA` 读回"
> 把这个交换抵消掉才显示正确。Vulkan 侧必须选定一组（目标格式 + 读回格式 + 是否需要 shader swizzle）
> 让**屏幕像素**与 GDI 一致，而不是"照抄看起来对"。
>
> **P2 的第一条门槛**就是一张三色卡 UI（纯 `#FF0000` / `#00FF00` / `#0000FF` 三块 + 半透明白叠加），
> 用 `vk_shot` 出图，与 `offscreen_render`（GDI 基准）逐像素比，diff 必须为 0 或可解释的 AA 边缘。

### 4.5 最小声明头的函数清单（P2 先接这些）

`vkGetInstanceProcAddr` 拿到的函数（约 22 个）：
`vkCreateInstance / vkDestroyInstance / vkEnumeratePhysicalDevices / vkGetPhysicalDeviceProperties /
vkGetPhysicalDeviceQueueFamilyProperties / vkCreateDevice / vkDestroyDevice / vkGetDeviceQueue /
vkGetPhysicalDeviceSurfaceSupportKHR / vkGetPhysicalDeviceSurfaceCapabilitiesKHR /
vkGetPhysicalDeviceSurfaceFormatsKHR / vkGetPhysicalDeviceSurfacePresentModesKHR /
vkCreateWin32SurfaceKHR / vkDestroySurfaceKHR / vkCreateSwapchainKHR / vkDestroySwapchainKHR /
vkGetSwapchainImagesKHR / vkAcquireNextImageKHR / vkQueuePresentKHR / vkQueueSubmit / vkQueueWaitIdle`，
外加 `vkCreate*Pipeline/ShaderModule/DescriptorSet*`、`vkCmd*` 一批（P3 再补）。

### 4.5a P2 落地记录（2026-11-11，已完成）

代码：`src/backends/binui_vk.h` + `.cpp`（离屏核心，约 1.5k 行）、`tools/vk_shot.cpp`、
`tools/vk_shaders/{quad.vert,quad.frag}` + 生成的 `src/backends/binui_vk_shaders.h`、
`third_party/vulkan/include/`（官方头，Apache-2.0，取自 Android NDK 27 的 `vulkan/` 与 `vk_video/`）。

**踩到并修掉的四个坑**（都已写进代码注释）：

1. **fence 必须建出来就是 signaled**。`beginFrame` 第一步是等本帧 fence；建造成 unsignaled 会让
   第一帧死等（实测挂死、CPU 0%）。
2. **帧内不许上传纹理**。`uploadTexture` 会 reset/提交命令缓冲——如果在录制中调用，
   等于把正在录的 cmd 掀掉（实测访问违例 0xC0000005）。改成"缺资源就登记，下一帧 beginFrame 之前补建"，
   因此首帧是预热帧：`vk_shot` 渲两帧，第 2 帧出图。
3. **批次的 scissor 要取"批次开始那一刻"的裁剪区**。第一版在关闭批次时才写 scissor，
   于是整页底色矩形被裁成一小块、四周露白（`vkcolor` 用例一眼看出）。
4. **Vulkan 的 NDC y 向下**，顶点着色器不能照抄 GL 的 `1 - y/h*2`，否则整幅画面上下颠倒。

**颜色字节序**：顶点色属性用 `VK_FORMAT_B8G8R8A8_UNORM`（0xAARRGGBB 的小端内存序正好是 BB GG RR AA），
目标图也用 `B8G8R8A8_UNORM`，两头一致 → 屏幕像素与 GDI/GL 一致；三色卡用例一次通过。

**验收实测**（`docs/issues/assets/vkcolor.html`，420×260，含纯色卡、半透明、SVG 矩形/圆/多边形/线、
圆角边框、胶囊）：

| 对比 | 差异 |
|---|---|
| **Vulkan ↔ OpenGL** | **0 像素（0.000%）** —— 两个 GPU 后端逐位一致 |
| Vulkan ↔ GDI 基准 | 537 像素（0.492%），全部在 AA 边缘 |
| OpenGL ↔ GDI 基准（改前既有） | **537 像素**，同一区域 —— 也就是说 Vulkan 与 GL 的差异**等于** GL 与 GDI 的既有 AA 差异 |

结论：Vulkan 后端复用了同一份几何与遍历后，像素行为与 GL 完全对齐，没有引入新的渲染差异。

### 4.6 P2 验收

```sh
build-mingw/bin/vk_shot.exe docs/issues/assets/svgpath.html /tmp/vk_p2.png 420 260
build-mingw/bin/pixel_diff.exe docs/pixels/svgpath.png /tmp/vk_p2.png 0.5   # 允许 AA 边缘差异，逐条解释
BINUI_PERF=1 build-mingw/bin/examples/svgdemo_vk.exe                        # 帧耗时分解可读、跑 10 分钟不崩
```

---

## 5. P3：全原语

### 5.1 批次与 draw 列表

与 GL 同构：每个四边形 6 顶点；切批键 = `(descriptorSet, scissor)`。
顶点环 3 块（每帧一块，host-visible，PERSISTENT 映射），耗尽时 `vkWaitForFences` 回收最老那块
（**不许 `vkQueueWaitIdle`**）。

### 5.2 字形图集

- 图集 image：`R8_UNORM`（FreeType 灰度），着色时用顶点色染色；白纹理 1×1 供纯色四边形。
- 增量上传：staging buffer → `vkCmdCopyBufferToImage` 子区域 → barrier
  `TRANSFER → FRAGMENT_SHADER`；**跨帧延迟销毁**：第 N 帧上传的图集代，第 N+2 帧才能被使用方释放。
- 采样器：`NEAREST`（与 GL 的 `linear=false` 默认一致；图片用 `LINEAR`，与 `linear=true` 分支对齐）。

### 5.3 图片与 descriptor

每张图一个 `VkImage` + 一个 `VkDescriptorSet`；`generation` 变化 → 重建 image + set，
旧 set 延迟两帧释放。pool 尺寸 = `图片数 + 图集代数×2 + 1`。

### 5.4 MSAA 累积帧 + 局部清屏

- 累积目标是一张**持久** 4x MSAA image（`framebufferColorSampleCounts` 取 4，不支持退 1 并告警）；
- 每帧 `loadOp = LOAD`，只对脏矩形 `vkCmdClearAttachments`（scissor 生效）；
- `resize` 才重建；
- present：`vkCmdResolveImage(msaa → target)`。

### 5.5 三路 present

| 场景 | 走法 |
|---|---|
| 普通窗口 | resolve → 交换链 image → `vkQueuePresentKHR` |
| 交换链不支持 `TRANSFER_DST` | resolve → 持久单采样离屏 → `vkCmdBlitImage` → 交换链 |
| 客户区盖满显示器（IF 风险）/ 透明窗口 / 拖拽 resize 期 | **读回**：`vkCmdCopyImageToBuffer` + fence 等待 → `StretchDIBits` 或 `UpdateLayeredWindow` |

IF 守卫复用现有 `clientCoversMonitor()` 判定（[`binui_gl-gdi.cpp`](../src/backends/binui_gl-gdi.cpp) 0.9.1 新增），
逻辑照搬，规则相同：铺满整屏就切读回。

### 5.6 透明窗口

离屏 premultiplied BGRA 读回 → DIB 双/三缓冲 → `UpdateLayeredWindow`（与 GL 同构，见调研 §4）。
**不做**交换链 alpha（Windows 上只有 33% 设备支持 `PRE_MULTIPLIED`）。

### 5.7 裁剪栈

`pushClip`：与当前 scissor 求交（浮点，`floor/ceil` 取整对齐 GL 的行为）；`vkCmdSetScissor` 前
**clamp 到 viewport**（ImGui 踩过：越界 scissor 直接 validation 报错）。

### 5.8 P3 验收

```sh
mingw32-make GPU=vk -j8 all tools        # 产出 *_vk 与 vk_shot
mingw32-make pixels GPU=vk               # 10 个用例，输出三方 diff 表（GDI 基准 / GL / VK）
```
期望：纯色与渐变填充区 **0 差异**；描边/文字边缘允许 AA 级差异，逐条写进文档；
`svgdemo_vk` / `features_vk` / `fontshow_vk` 目视一致、`BINUI_PERF=1` 帧耗时与 GL 同量级。

### 5.9 P3 落地记录（2026-11-11，已完成）

代码：`src/backends/binui_glyph_atlas.h`（新，GL/VK 共用的排版 + 图集）、
`src/backends/binui_win_image.h`（新，GDI+ 解码共用）、`binui_vk.cpp` 的纹理/文本/图片三块重写、
`tools/vk_shot.cpp`（图片资源加载、单帧出图）、`pixel_diff` 新增单通道阈值参数、
`Makefile` 新增 `pixels-vk`。

#### 5.9.1 关键决定：文本排版抽成共享头（与 P1 同一条理由）

文本是 UI 里几何最碎的部分。如果 GL 与 VK 各写一份"格子边长 / advance / 基线 / 图集代"，
换行位置和字间距会慢慢分叉，像素回归就只能靠容差遮着。所以把**排版 + 光栅化 + 缓存**整份搬进
`binui_glyph_atlas.h`，后端只实现 `IGlyphTexHost`（位图 → 纹理）并消费 `GlyphQuad` 回调。

代价与验证：GL 侧是纯搬迁，改完用 HEAD 版本逐像素对比，**3 个用例（probe / svgjoin / circle）
在阈值 0 下 0 差异**，随后 `make pixels` 10/10 依然零差异 —— 搬迁没有副作用。

#### 5.9.2 与设计的三处偏差（都是实测后的选择，不是偷懒）

1. **图集不是 `R8_UNORM`，而是 `BGRA8`**（§5.2 原计划）。理由：GL 图集历来是"白字预乘 BGRA"，
   共用一份光栅化代码时换格式就得给系数加分支。P3 的验收线是"GL↔VK 零差异"，
   统一 BGRA 让两端取样公式完全一致，一次就过。单通道 R8 能省 3/4 图集显存（4MB → 1MB/风格），
   这属于内存优化，需要第二条着色器分支，放 P4。
2. **不再有"预热帧"，也没有跨帧延迟销毁**（§5.2 原计划）。改为：上传**可以发生在帧内**
   （staging 拷进本帧命令缓冲，帧末等 fence 后统一释放），帧外产生的上传（排版阶段光栅化的字形、
   `prepareImage`）排进队列，在下一个 `beginFrame` 的开头、render pass 之前补录。
   于是"先上传后绘制"在同一帧内天然成立 —— `vk_shot` 从渲两帧改成渲一帧，结果不变。
   P2 的 `pendingRounds` 机制与它的"首帧缺圆角"瑕疵一起删掉了。
3. **顶点仍是一块常驻映射缓冲 + 每帧等 fence**（§5.1 原计划的 3 块环形缓冲没做）。
   等 fence 意味着没有 CPU/GPU 并行，但 P2 的离屏验收不需要；P4 再做多帧并行。
   → **v0.10.1 已完成**（双槽：每槽一块顶点缓冲 + 提交前等上一帧，见 §7.2）。

#### 5.9.3 踩到并修掉的新坑

1. **`vkCmdClearColorImage` 不允许出现在 render pass 内**。文本第一次光栅化就发生在帧内，
   于是"清零新图集"这条命令落在 render pass 里 —— 实测症状极具迷惑性：**只要画面里有文字，
   整帧变成纯底色**（命令缓冲被驱动整个丢掉，且 Vulkan 默认不报错）。
   改为用一块全零 staging 上传（`zeroTex()`），4MB/图集、一次性，代码路径反而更少。
   这条写进了 `stageUpload` 的上方注释，防止以后有人"优化"回去。
2. **新建图像内容不定**。GL 的 `glTexImage2D(data=nullptr)` 在实践中是零，Vulkan 不保证。
   图集里的"空格子"（空格、控制符）会被当真字形画出来（advance 有效、像素应全透明），
   不清零就会在文字中间出现噪点。所以 `Tex::needClear` 是必须的，不是保险。
3. **批次的纹理要在 draw 那一刻才切到 `SHADER_READ_ONLY`**。绘制是 `present()` 里 `flush()`
   才录进命令缓冲的，而上传在遍历期就录了 —— 若在 emit 时就切只读，后到的同图集字形还会
   再切回 `TRANSFER_DST`，最终 draw 会读到错误布局。
4. **`prepareImageRaw` 的 RGBA→BGRA 通道交换**。GL 用 `GL_RGBA` 直传，VK 的图统一
   `B8G8R8A8_UNORM`，所以原始像素要在 CPU 侧换一次 R/B（代价是一次 `w*h` 循环，可忽略）。

#### 5.9.4 P3 验收实测（14 个用例，全部通过）

命令：`make pixels-vk`（判据：不同像素 < 1.5%，单通道容差 128 —— 见下表说明）。

| 用例 | VK ↔ GL（阈值 0 的原始数字） | 最大通道差 | VK ↔ GDI 基准 | GL ↔ GDI 基准 |
|---|---|---|---|---|
| circle（圆 + 文本） | **0 (0.000%)** | 0 | 1130 | 1130 |
| display-none2 | **0 (0.000%)** | 0 | 5219 | 5219 |
| probe | **0 (0.000%)** | 0 | 4004 | 4004 |
| radius-matrix | **0 (0.000%)** | 0 | 7665 | 7665 |
| svgpath | 611 (0.560%) | 116 | 6071 | 6080 |
| svggrad | 223 (0.204%) | 113 | 4605 | 4609 |
| svgjoin | 1122 (1.027%) | 71 | 7494 | 7505 |
| svgstroke | 713 (0.653%) | 115 | 7174 | 7208 |
| svgfinish | 438 (0.401%) | 85 | 6949 | 6939 |
| svgaa | **0 (0.000%)** | 5 | 6670 | 6671 |
| demo/fontshow（纯文本整页） | **0（最大通道差 0）** | 0 | — | — |
| demo/features | **0** | 43 | — | — |
| demo/svgdemo | **0** | 56 | — | — |
| demo/texturedemo（5 张图片） | **0** | 9 | — | — |

**4 个 SVG 用例的残差是什么**：差值只出现在**非轴对齐的 MSAA 边缘**上，逐像素的方向是对称的
（`probe-line` 探针：GL 更亮的 96 像素、VK 更亮的 96 像素、净偏差 ≈ 0），幅度恰好是"一个采样点"
的覆盖率（4x MSAA 下 25%）。同一张显卡上，GL 与 Vulkan 走的是两套驱动前端，斜边上的
采样归属会差一个点 —— 这是**实现差异**而不是绘制 bug。判据因此写成"不同像素 < 1.5% 且单通道
≤ 128"，`pixel_diff` 为此新增了单通道阈值参数。

**更有说服力的一列**：4 个 SVG 用例里 VK↔GDI 与 GL↔GDI 的差异像素数相差 ≤ 34（例如 svgjoin
7494 vs 7505，共 109200 像素）—— 两个 GPU 后端与 GDI 基准**等距**，谁都不比谁更"错"。
轴对齐几何（圆、圆角、矩形、渐变、文本、图片）则是**逐位相同**。

### 5.10 P2b 落地记录：窗口 + 交换链 + 三路 present（2026-11-11，已完成）

代码：`binui_vk.cpp` 的 `surface/swapchain` 两块、`tools/vk_window.cpp`（新）、
`tools/save_png.h`（新，抓图工具共用的 PNG 写出）、`Makefile` 新增 `vk-window` 目标。

#### 5.10.1 关键决定：渲染目标不变，present 时才 blit 到交换链

设计里 §5.5 列了三种 present 走法，落地时选了一个更省事、也更容易保证一致性的结构：

```
[每帧] MSAA 累积图 --resolve--> 单采样 resolve 图
                                   ├─(A) vkCmdBlitImage → 交换链图像 → vkQueuePresentKHR
                                   └─(B) vkCmdCopyImageToBuffer → SetDIBitsToDevice / UpdateLayeredWindow
```

也就是说**交换链图像从来不是渲染目标**，只是一个"显示用的最终副本"。好处：

- 窗口模式与离屏模式跑的是**同一条渲染路径**，像素必然一致 —— `vk-shot`（离屏）与
  `vk-window`（窗口，读回交换链图像）在 probe / circle 上都是 **0 差异**；
- 不需要"每个交换链图像一套 framebuffer + render pass"，也不用考虑交换链格式与
  `kColorFormat` 不同时 render pass 要重建；
- 交换链图像只需要 `TRANSFER_DST`（blit 目标）+ `TRANSFER_SRC`（验收读回）两种用途。

代价是每帧多一次全屏 blit（1440p 上 < 0.1ms，实测满帧 1435fps 无压力），P4 若要极限优化
再改成"直接 resolve 进交换链"。

#### 5.10.2 三路 present 的实际分工

| 条件 | 走法 | 理由 |
|---|---|---|
| 交换链可用、非透明、非全屏 | (A) blit + `vkQueuePresentKHR` | 正常路径，最快 |
| 透明窗口（`setTransparent(true)`） | (B) 读回 + `UpdateLayeredWindow` | 交换链 alpha 在 Windows 上只有约 1/3 设备可信（调研 §4），与 GL 同构 |
| 客户区铺满显示器（borderless 全屏） | (B) 读回 + `SetDIBitsToDevice` | 与 GL 0.9.1 的直翻转守卫同一判定、同一理由 |
| 上层强制（拖拽 resize 期调 `presentViaGdi()`） | (B) | 绕开驱动在 resize+present 组合下的不确定性 |
| 交换链创建失败 / 驱动不给 WSI | (B) 自动降级 | 后端不能因为 WSI 缺失就整体不可用 |

`coversMonitor` 在 `init` 与 `resize` 时重算（与 GL 的 `applyPendingResize` 同节奏）；
`acquire/present` 返回 `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` 时重建交换链。

#### 5.10.3 踩到的坑

1. **`VK_USE_PLATFORM_WIN32_KHR` 必须在 `#include "vulkan/vulkan.h"` 之前定义**，否则
   `VkWin32SurfaceCreateInfoKHR` / `PFN_vkCreateWin32SurfaceKHR` 根本不存在（vendored 头按
   平台宏裁剪）。报错看起来像"头文件不全"，其实是少了这个宏。
2. **WSI 与交换链函数必须单独加载、且允许为空**：离屏模式下 `vkCreateWin32SurfaceKHR`
   之类的入口本来就不存在（没开 surface 扩展），用统一 `BINUI_VK_LOAD` 会让离屏初始化直接失败。
   所以拆成 `loadSurfaceFns()` / `loadSwapchainFns()`，失败只降级不致命。
3. **`--fullscreen` 的真实代价**：2560×1440 走读回 + GDI 贴屏只有 **11.4 fps**（全屏读回
   138MB/帧）。这与 GL 的守卫路径同量级，但说明"全屏 + 读回"只能当兜底，不能当常规路径。

#### 5.10.4 P2b 验收实测（`make vk-window`）

| 场景 | 结果 |
|---|---|
| probe / circle（窗口 + 交换链，读回**交换链图像**） | 与 GL 同尺寸输出 **0 像素差异（阈值 0）** |
| svgpath（斜边 AA） | 611px（0.560%），最大通道差 116 —— 与离屏模式的 4 个 SVG 残差同源 |
| 强制 GDI 路径（`--gdi`） | 0 差异（probe/circle），11fps→223fps 量级正常 |
| 透明窗口（`--transparent`，分层窗口） | 0 差异，无崩溃 |
| resize（600×260 → 760×340，交换链重建） | 重建后捕获帧与 GL 在 760×340 下的输出 **0 差异** |
| 全屏守卫（2560×1440 铺满显示器） | 自动切 `present=GDI 读回`，11.4 fps，无停帧/撕裂现象 |
| 帧率晴雨表（交换链，vsync=false，MAILBOX） | 1435 fps（600×260）→ 说明 present 没有额外阻塞 |

**未做（留到后面）**：交换链 alpha（透明仍走读回）、~~frames-in-flight 并行（仍是单命令缓冲 +
每帧等 fence）~~（v0.10.1 已做，见 §7.2）、输入/IME 事件循环（P5 的 app 类）、
`GPU=vk` 构建开关（P5）。

---

## 6. P4：鲁棒性清单

| # | 场景 | 判据 |
|---|---|---|
| 1 | 拖边框 resize（每帧 `SetWindowPos`） | 无花屏、无 validation error、内容跟手 |
| 2 | 最大化 / 还原 | 画面持续更新（`clientCoversMonitor` → 读回路径生效）；对比 0.9.1 的 GL 修法 |
| 3 | 最小化 → 还原 | 不崩、不黑、交换链正确重建（`maxImageExtent == 0` 分支） |
| 4 | 跨显示器拖动（DPI 100%↔150%） | `WM_DPICHANGED` 后布局与像素正确 |
| 5 | 远程桌面 / 虚拟显示器（向日葵 IDD 在场） | 画面持续更新（IDD 是最容易暴露 IF 停帧的环境） |
| 6 | 多实例（同进程两个 GPU 面板） | 各自画面正确（GL 踩过"全画到后建那块"，Vulkan 每实例独立 device 即可） |
| 7 | 热重载（`--watch` + 改 HTML） | 不崩、图集/图片正确换代 |
| 8 | 验证层（`VK_LAYER_KHRONOS_validation`） | 8 个 demo 各跑 60 秒，**0 error 0 warning**（不含 perf warning） |

### 6.1 P4 落地记录（2026-11-11，已完成可测部分）

代码：`binui_vk.cpp` 的鲁棒性加固（see below）、`tools/vk_stress.cpp`（新，压力测试）、
`Makefile` 新增 `vk-stress` 目标；`pixel_diff`/工具链无改动。

#### 6.1.1 加固了什么

1. **丢帧也要提交命令缓冲**（`submitNoPresent()`）。最小化（surface extent = 0）、
   交换链过期、`acquire` 失败这三条早退路径以前直接 `vkEndCommandBuffer` 就返回，
   而 `beginFrame` 每帧开头都在等 fence —— **漏提交就是死锁**。压力测试第一轮最小化就挂死了，
   这是本轮抓到的最严重的 bug。
2. **解映射必须在释放之前**。`freeMem()` 会把句柄置空，原来 `vkFreeMemory` 之后紧跟
   `vkUnmapMemory(device, vmem)` —— 以前只是"顺序不对但侥幸没崩"，改完记账后立刻变成
   访问违例（`0xC0000005`）。
3. **帧前 surface 复查**：每帧查 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`，
   `currentExtent` 与交换链不一致（跨显示器 / DPI 变化 / 用户改尺寸）就重建；
   `currentExtent` 为 0×0（最小化）则丢弃本帧。
4. **读回缓冲按"渲染目标与交换链的较大者"分配**。DPI 缩放时 swapchain 可能比客户区大，
   捕获路径按 `swapExtent` 拷 —— 原来按 `w*h` 分配会越界写。
5. **多实例安全**：去掉全局 `instance_`/`device_`（原先被最后一个实例覆盖，
   先销毁的实例会把还活着的实例的函数加载路径弄脏），加载函数改为显式传 `VkInstance`。
6. **设备丢失**：`VK_ERROR_DEVICE_LOST` 时把后端置为未就绪，不再继续提交。
7. **显存记账** `deviceMemoryBytes()`：区分"我们自己漏了"和"驱动把释放的显存留在池里"
   （P4 判泄漏时唯一可靠的账）。
8. **校验层开关**：`BINUI_VK_LAYERS=1` 列出实例层，`BINUI_VK_VALIDATION=1` 在装了就启用
   Khronos 校验层（本机没装，见 6.1.3）。
9. 去掉 `[vk] 交换链 …` 的默认打印（拖边框时每帧重建会刷屏），改由 `BINUI_VK_TRACE=1` 打开。

#### 6.1.2 压力测试结果（`make vk-stress`）

`tools/vk_stress.cpp` 一轮 8 个阶段，全部 `[ok]`：

| 阶段 | 判据 | 结果 |
|---|---|---|
| 1. resize 抖动（120 次，尺寸含 1/7/33/1200px、奇数、极小值） | 后端仍就绪、记录尺寸与请求一致 | ok |
| 2. 同尺寸反复 resize（100 次） | 走早退路径不破坏后端 | ok |
| 3. 最小化 → 恢复 | 0×0 被忽略、恢复后继续绘制、不死锁 | ok |
| 4. 透明窗来回切（40 次） | 读回路径与交换链路径互切不崩 | ok |
| 5. 强制 GDI 呈现 / 交换链交替（40 次） | 同上 | ok |
| 6. init/shutdown ×20（同一 HWND） | 不崩、提交内存不涨（105.5 → 105.5 MB） | ok |
| 7. 双实例并存 | 两个实例各自出图；销毁第二个后第一个仍可用 | ok |
| 8. 泄漏复跑 | `shutdown` 后**后端显存记账 = 0.0 MB**；两轮之间提交内存 −7.0 MB（无线性增长） | ok |

关于那 160MB 的提交内存：阶段 1 之后 ~105MB、结束 ~165MB，但**后端自己的 `vkAllocateMemory`
账在 shutdown 后是 0**，两轮同流程之间不涨 —— 那是驱动对交换链图像/显存块的池化
（NVIDIA 常见行为），不是后端泄漏。判据因此写成"记账归零 + 不随轮数线性上涨"，
而不是"进程提交内存回到基线"。

像素侧：压力测试结束时的画面与 GL 同尺寸输出 **0 差异（阈值 0）**（probe / vkcolor 两个用例）。

#### 6.1.3 本机测不了的部分（明确写清楚，不冒充已验证）

| 项 | 状态 | 原因 |
|---|---|---|
| 跨显示器拖动（DPI 100%↔150%） | 未实测 | 本机只有 **1 台显示器**（2560×1440，`Screen.AllScreens.Count = 1`）；代码里的 `currentExtent` 复查与读回缓冲尺寸修正是对应防护，但缺现场证据 |
| 远程桌面 / 向日葵 IDD | 未实测 | 本机没有 RDP 会话与虚拟显示器；IF 停帧守卫（`clientCoversMonitor`）已在 2560×1440 全屏下验证（自动切读回、无停帧） |
| 验证层 0 error | 不可用 | 没装 Vulkan SDK。实测 `BINUI_VK_LAYERS=1` 只枚举到 **5 个第三方/驱动层**
（`VK_LAYER_NV_optimus` / `NV_present` / `VALVE_steam_overlay` / `VALVE_steam_fossilize` / `OBS_HOOK`），
没有 `VK_LAYER_KHRONOS_validation`；`BINUI_VK_VALIDATION=1` 会明确打印"本机没装" |
| 热重载（`--watch` + 改 HTML） | 未实测 | 热重载在 app 层（`binui_hot_reload`），Vulkan 侧只涉及"重新 `setSize` + 图集换代"，
那两条路径已被 resize 抖动与 atlas 换代覆盖；完整跑起来要等 P5 的 `GPU=vk` app |

---

## 7. P5：工具与发版

- `tools/vk_shot.cpp`：离屏 Vulkan 出图（镜像 `gl_shot`，复用同一套 `offscreen` 目标，输出 PNG）。
- Makefile：`GPU=gl|vk`（默认 gl）切换 `*_vk` 目标与 `vk_shot`；`pixels GPU=vk` 用 `vk_shot` 代替 `offscreen_render`。
- 运行期开关：`BINUI_GPU=vk|gl`（demo 里读环境变量选 `typedef`），默认 gl。
- 文档：`docs/vulkan.md` 补"已实现"章节、README 后端矩阵加一行、`CHANGELOG` 记 **0.10.0**（新后端 = MINOR）。
- 发版门槛同现有规矩：干净重编 0 warning、四套测试全绿、`make pixels` 10/10、`check_examples` 11/11。

### 7.1 P5 落地记录（2026-11-11，已完成，v0.10.0）

#### 7.1.1 关键决定：把"窗口外壳"也抽成共享模板，而不是给 Vulkan 再写一份

`BinUIAppGLGdi` 原来整块长在 `binui_gl-gdi.cpp` 里（**1134 行**）。做 `GPU=vk` 时发现它只调用后端的
8 个方法：`init / shutdown / resize / beginFrame / present / presentViaGdi / setTransparent / isReady`
—— 跟 GL 一点关系都没有。所以：

- 新文件 `src/backends/binui_app.h`：`template <class Gfx> class BinUIAppT`（整块搬过来，机械替换），
  连同 `FramePerf`/`Perf()`/`NowMs()`/`caretBlinkOn()`/`isTitlebarDrag()`/`imeCaretFromRenderer()`/
  `placeCaretAtClick()` 一起变成共享代码；
- `BinUIAppGLGdi = BinUIAppT<BinUIGLGdi>`、`BinUIAppVK = BinUIAppT<BinUIVK>`；
- 后端只需要多提供一个 `static const wchar_t* appClassName()`（同进程两个后端并存时窗口类不冲突）。
- 新文件 `src/backends/binui_ui.h`：示例/工具的统一入口，`UiBackend` / `UiApp` / `drawUi()`，
  由 `-DBINUI_GPU_VK` 选后端；8 个示例改成这套名字（`BinUIAppGLGdi` → `UiApp` 等），
  逻辑一行没动。

结果：时间轴/输入/IME/无边框拖拽/帧节流/脏矩形/直翻转守卫这些"边角逻辑"两个后端**只有一份**，
不会再各自演进。

#### 7.1.2 与设计的一处偏差：编译期开关，没做运行期 `BINUI_GPU`

设计里写的是"`GPU=gl|vk` 构建开关 + `BINUI_GPU` 运行期开关"。实际只做了**编译期**：

- `make demos-vk` 产出 `examples/*_vk.exe`（同一份源码 + `-DBINUI_GPU_VK`，链接 `binui_vk.o`），
  与 GL 版并存，便于 A/B 对比；
- 运行期切换要在同一个进程里装两套后端，而示例的 `UiBackend&` 是**具体类型**（`<canvas>` 回调里要调
  原语），要么再加一层类型擦除、要么每个示例写两份 switch —— 收益不抵复杂度。像素/压力/窗口三套
  判据已经分别覆盖两个后端，运行期切换属于"能选但不必须"。

#### 7.1.3 验收实测（v0.10.0）

| 项 | 结果 |
|---|---|
| 干净重编 | **0 warning**（`-Wall -Wextra`） |
| 四套测试 | `test_elements` / `test_css` / `test_font` / `test_capi` 全绿 |
| `make pixels`（GDI 基准） | 10/10 **零差异** |
| `make pixels-vk`（VK ↔ GL，14 用例） | 14/14 通过（6 个用例 0 差异，4 个 SVG 描边 ≤1 个 MSAA 采样） |
| `make vk-window`（窗口 5 种走法 × 3 用例） | 12 项阈值 128 全过；**脏矩形重绘 vs 全量重绘 3/3 逐位相同**（阈值 0） |
| `make vk-stress`（8 阶段压力 + 泄漏复跑） | 全部 `[ok]`，shutdown 后后端显存记账 0.0 MB |
| `make demos-vk` | 8 个 `*_vk.exe` 链接成功；`svgdemo` / `svgdemo_vk` 各跑 5 秒无崩溃，`BINUI_PERF` 两边输出一致（120Hz → budget 8.33ms） |
| `check_examples` | 11/11 无问题 |
| 版本 | `binui_version()` → `0.10.0`（新后端 = MINOR） |

#### 7.1.4 这轮之后仍然没做的

- 运行期 `BINUI_GPU`（见 7.1.2）；
- ~~frames-in-flight 并行（仍是单命令缓冲 + 每帧等 fence）~~ → **v0.10.1 补上了**，见 §7.2；
  设计 §5.1 的"环形顶点缓冲"仍是未来的可选项（当前每槽一块整缓冲就够用）。
- 跨显示器 DPI / RDP / 验证层 0 error：条件不具备（见 §6.1.3）；
- 把 D3D11 后端并进共享遍历（它至今不在构建里，见 §3.2b）。

### 7.2 P6 落地记录：双槽流水线（2026-11-11，v0.10.1）

#### 7.2.1 改的是什么

改之前每帧的因果链是**串行**的：

```
beginFrame → [CPU 遍历/排版/路径细分/填顶点] → endRP → submit → vkWaitForFences(本帧)
           → blitToWindow/读回 → 返回
```

`submit → fence 置位` 这个往返（驱动提交 + GPU 起跑 + MSAA resolve + blit）虽然只有
**75~150 µs**，但它整个压在每帧关键路径上，而且和下一帧的 CPU 录制完全无法重叠。
现在改成双槽（`Impl::Slot`）：命令缓冲 / fence / 两个信号量 / 顶点缓冲 / staging 待释放表
各自一套，**等待点从 beginFrame 挪到"提交前等上一帧"**：

```
beginFrame  : slotIdx = frameNo % 2；等本槽（两帧前的，通常已置位）→ 复位 fence →
              绑定本槽 cmd/vbuf/vmap → 开录
present     : 录完 → 等 prevSlot（上一帧）→ 提交本槽 → [仅 GDI/capture] 等本槽 → present
```

于是 `present(N)` 返回后，GPU 还在跑第 N 帧的期间 CPU 已经把 N+1 帧录完了。

#### 7.2.2 为什么只双槽，为什么必须留一次等待

渲染目标（4x MSAA 累积图 + resolve 图）和纹理布局是**帧间共享**的，累积帧 `loadOp=LOAD`，
两帧并发会互相踩；所以"上一帧的 fence"这个等待**不能删**，只能挪位置 —— 挪到录制之后、
提交之前，正好把录制时间塞进 GPU 执行时间里。槽数超过 2 不会增加任何并行度，只多占
2 MB/槽 顶点缓冲 + 一套命令对象。

顶点缓冲必须每槽一块：写第 N+1 帧顶点时，第 N 帧可能还在读它自己的那块。

#### 7.2.3 踩到的坑：WSI 函数在离屏模式下是空指针

`vkCreateSemaphore` / `vkDestroySemaphore` 被归在 WSI 那一组（`loadSwapchainFns` 之后才非空），
而双槽把信号量从"窗口模式才建"改成了"每槽都建"，于是 **`vk_shot` 一跑就 SIGSEGV**
（`gdb` 定位到 `Impl::initSlot`，`#0 0x0000000000000000`）。修法：信号量与交换链**同生共死**
—— `createSwapchain()` 成功后建、`destroySwapchain()` 里毁；离屏模式没有这两个对象，而 GDI 呈现
路径本来也不用它们。

#### 7.2.4 A/B 实测（同机同工具，7 次取中位，300 帧，vsync=off，capture 关闭）

| 用例（窗口尺寸） | v0.10.0（串行） | v0.10.1（双槽） | 提升 |
|---|---|---|---|
| `features` 900×932 | 1375.9 fps (0.727 ms/帧) | **1722.3 fps** (0.581 ms/帧) | **+25.2%** |
| `svgdemo` 960×560 | 618.4 fps (1.617 ms/帧) | **682.2 fps** (1.466 ms/帧) | **+10.3%** |
| `probe` 600×260 | 4711.2 fps (0.212 ms/帧) | **7277.4 fps** (0.137 ms/帧) | **+54.5%** |
| `singlewindow` 1280×720 | 2604.7 fps (0.384 ms/帧) | **3642.6 fps** (0.274 ms/帧) | **+39.8%** |

每帧净省 **75~150 µs**，与"fence 往返"的量级吻合；窗口越小、CPU 每帧越短，省下的固定开销占比越大
（所以 `probe` 提升最猛、`svgdemo` 最小）。

#### 7.2.5 拆分数据（`BINUI_VK_PERF=1`，每 60 帧一行）

```
svgdemo      1.447 ms/帧 = 录制 1.244 + 等 GPU 0.052 + 呈现 0.151
features     0.470 ms/帧 = 录制 0.266 + 等 GPU 0.089 + 呈现 0.115
singlewindow 0.167 ms/帧 = 录制 0.062 + 等 GPU 0.013 + 呈现 0.092
```

结论很明确：**GPU 几乎全程空闲**，这台机器（RTX 4060 Ti）上 900×932 / 4x MSAA 的一帧只要几十微秒。
剩下的瓶颈全在 CPU 录制侧 —— `svgdemo` 86% 的时间花在遍历 + 路径细分 + 填顶点上。
所以再往上加 GPU 并行度（3 槽、无 fence）不会有任何收益；下一步的性能目标应该是
**绘制遍历与 SVG 细分本身**（缓存细分结果 / 顶点环上传），这跟 GL 后端是共用收益。

→ 顺着这条线查下去，**真正的元凶不是细分，而是热路径里的 `std::getenv`**，见 §7.3
（`svgdemo` 录制 1.24 ms → 0.059 ms）。

#### 7.2.6 验收（v0.10.1）

| 项 | 结果 |
|---|---|
| 干净重编 | **0 warning**（`-Wall -Wextra`） |
| 四套测试 | `test_elements` / `test_css` / `test_font` / `test_capi` 全绿 |
| `make pixels-vk` | 14/14 通过，**14 个用例"不同像素 0"** |
| `make vk-window` | 12 项阈值 128 全过；脏矩形 vs 全量重绘 3/3 逐位相同（阈值 0） |
| `make vk-stress` | 两轮 8 阶段全 `[ok]`，shutdown 后后端显存记账 0.0 MB |
| `make demos-vk` | 8 个 `*_vk.exe` 链接成功，`svgdemo_vk` 跑 5 秒无崩溃 |
| `check_examples` | 11/11 无问题 |

---

## 7.3 P6b 落地记录：热路径里的 `std::getenv`（2026-11-11，v0.10.2）

### 7.3.1 怎么找到的（一条可复核的证据链）

P6 的 `BINUI_VK_PERF` 拆分说 `svgdemo` 960×560 每帧 1.447 ms 里 **1.244 ms 在录制**、GPU 只等 0.052 ms。
按"录制 = 遍历 + 路径细分 + 填顶点"的直觉，先怀疑 SVG 细分，于是用临时开关逐项排除
（开关只在本地量测时加，没有提交）：

| 实验 | 录制耗时 | 结论 |
|---|---|---|
| 原状 | 1.183 ms | — |
| 关掉描边绘制（`PATH_STROKE` 分支短路） | 1.174 ms | **不是描边**（只占 0.009 ms） |
| 整条路径绘制短路（`PaintPathNode` 直接 return） | 0.120 ms | **就是路径绘制**，占 90% |
| 把条带缓存的 `cache` 参数传 `nullptr`（等于每帧重算条带） | 1.196 ms | **不是细分**：缓存开关几乎没差别 → 瓶颈在"重放"不在"计算" |
| 把 `fillRect` 里的 `std::getenv("BINUI_VK_TRACE")` 提成 `static const bool` | 0.138 ms | **命中**：省了 1.04 ms |
| 再把 `drawNodesBinUI` 里逐节点的 `std::getenv("BINUI_DEBUG_CLIP")` 提成 static | **0.059 ms** | 再省 0.045 ms |

一帧 `svgdemo` 有 **2109 条条带**（`flush: verts=12654` = 2109 个四边形），也就是 `fillRect`
每帧被调用 2109 次；每次调一次 `getenv`，在 UCRT 里要遍历环境表（≈0.5 µs）→ 约 1.05 ms。

### 7.3.2 改了什么

| 位置 | 改动 |
|---|---|
| `src/backends/binui_vk.cpp` `fillRect` | `std::getenv` → 函数内 `static const bool`（只查一次） |
| `src/backends/binui_draw_nodes.h` `drawNodesBinUI` | `BINUI_DEBUG_CLIP` 提到函数开头查一次，两处打印共用 |

第二处在**共享遍历**里，所以 GL 后端同等受益（GL 的图元函数本身没有 getenv，审计过）。

### 7.3.3 实测（同机同工具，7 次取中位，300 帧，vsync=off）

| 用例 | 0.10.0 | 0.10.1 | **0.10.2** | 对 0.10.0 |
|---|---|---|---|---|
| `features` 900×932 | 1375.9 fps | 1722.3 fps | **2657.6 fps** | **+93.2%** |
| `svgdemo` 960×560 | 618.4 fps | 682.2 fps | **3857.6 fps** | **+523.8%（6.2×）** |
| `probe` 600×260 | 4711.2 fps | 7277.4 fps | **9087.4 fps** | **+92.9%** |
| `singlewindow` 1280×720 | 2604.7 fps | 3642.6 fps | **4387.7 fps** | **+68.5%** |

`svgdemo` 的录制耗时 1.183 → **0.059 ms**（20×）—— 它本来就是"路径最多"的页面，所以最能体现差别。
`BINUI_VK_PERF` 现在的分工：`svgdemo` 0.128 ms/帧 = 录制 0.059 + 等 GPU 0.003 + 呈现 0.066，
**呈现那一侧反过来成了最大头**（提交 + resolve blit + present）。

### 7.3.4 教训（写进规矩）

- `std::getenv` 是**加锁 + 遍历环境表**的操作，在本机 UCRT 上 ≈0.5 µs/次。任何
  "每个图元/每个节点查一次"的写法都是自杀：几千次就是毫秒级。
- 调试开关的正确形态：`static const bool on = std::getenv("X") != nullptr;` —— 进程内只查一次，
  关掉时是一根可预测的空分支。**不要**在循环或热路径里直接调。
- 审计结论（本仓库）：其余 `getenv` 都在初始化/一次性路径（`binui_app.h` 的 `BINUI_PERF`、
  `BINUI_IME_LOG`、`binui_paths.h` 的 `BINUI_UI`、VK 的层/校验/`TRACE` 初始化分支），热路径已清零。

---

## 7.4 P7 落地记录：D3D11 并入共享代码并进构建（2026-11-11，v0.11.0）

### 7.4.1 起因

§3.2b 记过一笔：D3D11 后端（`server/backends/binui_d3d11.cpp`，2804 行）**根本不在构建里**。
它自带一整套重复：自己的节点树遍历（`drawNodesD3D`，622 行 + 视频控制条）、自己的字形图集
（`Atlas`/`ColorAtlas` 格子打包，与 `binui_glyph_atlas.h` 同功能）、自己的图片解码
（`DecodeImagePixels`，与 `binui_win_image.h` 同源）、自己的光标定位/闪烁/标题栏判定、
自己的无边框窗口外壳（`BinUIAppD3D`，约 440 行）。Vulkan 做完后，这些重复第一次有了
可比对的"标准答案"，于是把它并进来。

### 7.4.2 做了什么

| 项 | 改动 |
|---|---|
| 接口 | `class BinUID3D : public IBinUIGfx`，全部原语标 `override`（之前只是签名碰巧相同） |
| 遍历 | 删掉 `drawNodesD3D` + `DrawVideoControlsTmpl`/`FormatVideoTime`；改走 `drawNodesBinUI` |
| 外壳 | 删掉 `BinUIAppD3D`（含 `wndProc`/`staticResize`/事件分发）；`using BinUIAppD3D = BinUIAppT<BinUID3D>` |
| 输入辅助 | 删掉 `placeCaretAtClickD3D` / `caretBlinkOn` / `isTitlebarDrag`；用 `binui_app.h` 的共享模板 |
| 呈现 | 新增 `presentViaGdi()` + `Impl::presentReadback()`（透明窗 / 全屏直翻转守卫 / 拖拽期），与 GL/Vulkan 同一套触发条件 |
| 入口 | `binui_ui.h` 加 `BINUI_GPU_D3D11` 分支；`drawUi()` 三个后端统一调 `drawNodesBinUI`（GL 原来的 `drawNodesGLGdi` 就是这个 wrapper，行为不变） |
| 构建 | `Makefile`：`binui_d3d11.o` 规则 + `D3D11_LIBS`（d3d11/dxgi/d3dcompiler/dwmapi）+ `make demos-d3d11` → `examples/*_d3d11.exe`（8 个） |

净效果：`binui_d3d11.cpp` **2804 → 1615 行**（-42%），`binui_d3d11.h` 192 → 106 行。
D3D11 第一次进构建，并且**白拿**了 v13~v19 的全部特性（边框/渐变/文字装饰/SVG 路径/
脏矩形/帧节流/IME），因为遍历和外壳都是共享的。

### 7.4.3 验收工具：`d3d11_shot`（隐藏窗口 + 读回）

D3D11 的交换链必须绑窗口，没有"无窗口离屏"这条腿，所以 `tools/d3d11_shot.cpp` 开一个
**隐藏窗口**（`WS_POPUP`、不 `ShowWindow`）跑 N 帧，然后从**离屏纹理**读回像素
（新增 `BinUID3D::readbackPixels`）。这一点很关键：先用屏幕截屏（`window_capture` 的
`PrintWindow`）时抓到的是一张**纯白图** —— flip-model 交换链的内容不在 GDI 表面上，
窗口被别的窗口（这个 GUI）盖住时 DWM 也没有可复制的内容。改成后端自己读回之后，
窗口被遮挡/隐藏都照样抓得到真实像素。

### 7.4.4 与 GL 的逐像素比对（`make pixels-d3d11`，14 用例）

**P7a 之后**（D3D11 自己的圆角模板 + 单采样离屏）：

| 用例 | 不同像素 |
|---|---|
| `probe` / `radius-matrix` / `texturedemo` | **0** |
| `svggrad` / `svgfinish` / `svgjoin` / `display-none2` | 8 → 0 / 3 / 57 / 168 |
| `svgaa` / `svgpath` / `svgstroke` | 272 / 284 / 305 |
| `fontshow` / `svgdemo` / `features` | 336 / 116 / 2492 |
| `circle` | 1200 (**2.041%**) |

**P7b 之后**（换成共享圆角模板 + 4× MSAA 离屏）：

| 用例 | 不同像素 | 变化 |
|---|---|---|
| `probe` / `radius-matrix` / `texturedemo` | **0** | — |
| `display-none2` / `svggrad` / `svgfinish` | **0** | 168/8/3 → 0（圆角模板对上了） |
| `svgjoin` / `svgpath` | 4 / 25 | 57/284 → 4/25 |
| `fontshow` / `features` / `svgdemo` | 8 / 132 / 104 | 336/2492/116 → 8/132/104 |
| `svgstroke` / `svgaa` / `circle` | 247 / 272 / 1200 | 基本不变 |
| 脏矩形重绘 vs 全量重绘（阈值 0） | **0** | 持久 MSAA 累积帧没被破坏 |
| 强制读回 + GDI 贴屏 vs 交换链（阈值 0） | **0** | 同一张 offTex |

两处改动各自的贡献是**可分离**的：

1. **圆角模板换成共享的**（`RoundTemplateSize` + 铺满 `[0,t]` 的覆盖率公式，不再 `max(8,4r)`
   且不再内缩 1px）→ `display-none2` 168→0、`svggrad` 8→0、`features` 2492→132。
   根因很明确：模板边长差 1~2r px、形状又在缩小过的盒子里铺，九宫格的角片就对不上弧长。
2. **离屏改成 4× MSAA + ResolveSubresource**（与 GL 的 `msFbo→offTex`、Vulkan 的
   `msaa→resolve` 同一条路线）→ `svgpath` 92→25、`svgjoin` 16→4、`svgfinish` 3→0。

剩下的差异（`circle` 2.04%、`svgaa` 0.25%、`svgstroke` 0.23%）全部在**曲线的边缘一圈**
（diff 图上就是圆的一圈红绿描边）—— 是两套 API 的 4× MSAA **采样位置**不同导致的覆盖率
量化差，与 VK↔GL 在斜边上"一个采样点"的差同理。所以 `make pixels-d3d11` 的判据暂定
**3%**（`PIXEL_D3D_TOL`），比 `pixels-vk` 的 1.5% 宽；除 `circle` 外全部用例都在 0.25% 以内。

### 7.4.5 验收（v0.11.0）

| 项 | 结果 |
|---|---|
| 干净重编 | **0 warning**（`-Wall -Wextra`，三个后端 + 8 个 d3d11 demo + d3d11_shot） |
| 四套测试 | 全绿 |
| `make pixels`（GDI 基准） | 10/10 零差异（共享遍历改动后不变） |
| `make pixels-vk` | 14/14（不同像素 0） |
| `make pixels-d3d11`（新） | 14 用例全过（判据 3%，差异集中圆角/文本，见 §7.4.4）；脏矩形 vs 全量、GDI vs 交换链 **两条阈值 0 全过** |
| `make demos-d3d11` | 8 个 `*_d3d11.exe` 链接成功；`svgdemo_d3d11` 跑 3 秒无崩溃、`d3d11_shot` 出图为完整页面 |
| `make demos-vk` / `check_examples` | 8 个 / 11-11 无问题 |

### 7.4.6 仍未做的

- ~~文本/图片仍是 D3D11 自己的实现~~ → **v0.11.2 已换成共享的** `binui_glyph_atlas.h` /
  `binui_win_image.h`（`binui_d3d11.cpp` 1216 行，与 GL 后端同量级；像素数字不变，
  因为两套图集光栅化出的位图本来就相同）；
- 曲线边缘与 GL 的 4× MSAA 采样位置差异（见 §7.4.4，量级 0.02~0.25%，`circle` 2.04%）；
- 跨显示器 DPI / RDP / 验证层 0 error：条件不具备（见 §6.1.3）。

---

## 7.5 预留接口与未来计划（2026-11-11，v0.11.3）

三件"本机条件不具备、但接口先留好"的事。原则：**能模拟的就必须能测**，只写文档的下场是
下次接手时又得从头查一遍。

### 7.5.1 跨显示器 DPI

- **接口**：窗口外壳处理 `WM_DPICHANGED`（[binui_app.h](../src/backends/binui_app.h)）：
  按系统建议的矩形摆窗口 → 走与 `WM_SIZE` 相同的"挂起 → 帧内应用"路径（重排 + 后端
  `resize` + 全量重绘）→ 记下新 DPI → 回调 `setDpiChangedFn(dpi)`。`dpi()` 可读当前值。
- **模拟**：`BINUI_FORCE_DPI=120|144|168` 覆盖 DPI 取值，单显示器机器上也能把这条路径跑通。
- **验证**：`BINUI_FORCE_DPI=120` 跑 `svgdemo`，启动即打印
  `[dpi] BINUI_FORCE_DPI=120（模拟换屏后的 DPI）`，窗口正常出图。
- **仍缺**：真正的跨屏缩放取决于**应用**是否把进程设成 per-monitor-v2
  （`SetProcessDpiAwarenessContext`）。库不改进程 DPI 策略（这是越界，见 `openChild` 注释），
  所以"拖到另一块 DPI 不同的屏上"这一步只能在双屏机器上验收。

### 7.5.2 远程会话（RDP / 虚拟显示器）

- **判定**：`binui::RemoteSession()`（[binui_app.h](../src/backends/binui_app.h)）：
  `GetSystemMetrics(SM_REMOTESESSION)`，`BINUI_FORCE_REMOTE=1` 可模拟（本机没有 RDP 会话）。
- **行为**：远程会话一律走"读回 + GDI 贴屏"，绕开直翻转。三个后端与外壳共用这一处判定
  （外壳置 `fullscreenPresent_`，VK/D3D11 在 `present()` 里也各自兜一层，因为工具不走外壳）。
- **证据**（本机模拟，本次实测）：

  | 后端 | 常规 | `BINUI_FORCE_REMOTE=1` | 像素 |
  |---|---|---|---|
  | Vulkan（`vk_window`，600×260，10 帧） | 631.7 fps，`present=交换链` | 184.4 fps，`present=GDI 读回` | **0 差异（阈值 0）** |
  | D3D11（`d3d11_shot`，420×260） | 交换链 | 读回贴屏 | **0 差异（阈值 0）** |

  也就是读回的代价是**吞吐**（631.7 → 184.4 fps，全屏拷贝 + `SetDIBitsToDevice`），
  像素不受影响 —— 这正是"远程环境宁可慢也不能停帧"的取舍。

### 7.5.3 运行期 `BINUI_GPU=vk|gl|d3d11`

- **现状**：编译期选（`-DBINUI_GPU_VK` / `-DBINUI_GPU_D3D11`），一份源码编三个 exe。
- **已清掉的前置障碍**（这是本轮实际做的事）：
  1. `BinUIAppT<Gfx>::DrawFn` 与 `<canvas>` 回调都改成吃 `IBinUIGfx&`，8 个示例的
     `setDrawFn` / `canvasCb` 一并改成接口类型 —— 调用方不再依赖具体后端类型；
  2. 遍历、字形图集、图片解码、窗口外壳全是共享的（§7.4），后端本身只剩 ~1200 行；
  3. `binui_ui.h` 里留了 `UiBackendName()` 与"接哪里"的注释。
- **还缺**：一个 `IBinUIApp`（`run/pump/退出/setXXX`）抽象 + 工厂
  `CreateUiApp(BackendId)`（按 `getenv("BINUI_GPU")` 返回某个 `BinUIAppT<Gfx>`），
  并把三个后端的 `.o` 同时链进同一个 exe。评估：约半天，纯加法，不改现有接口。

---

## 7.6 CPU 侧绘制：整形缓存（2026-11-11，v0.11.4）

### 7.6.1 先量再改：`BINUI_DRAW_PROF=1`

在共享遍历 `drawNodesBinUI` 里加了 env 开关的分段计时（RAII 作用域，节点分支里的
`continue` 也能正确归账），每 60 帧往 stderr 打一行：总遍历耗时 + 每个节点类型的耗时/个数。
关掉时是一根 `static const bool` 空分支（`std::getenv` 只查一次 —— 上一轮 §7.3 的教训）。

`features`（900×932，80 个节点/帧）的读数：

```
[draw][prof] 60 帧: 遍历 0.087 ms/帧，节点 80/帧（索引 0.002）
[draw][prof]   type  1（LABEL）: 0.058 ms/帧（43 个/帧，≈1.35 µs/个）
[draw][prof]   type  3（BUTTON）: 0.010 ms/帧
[draw][prof]   type  5（DIV）: 0.008 ms/帧
```

**文本占 67%**，不是遍历/裁剪/圆角。原因很具体：`GlyphAtlas::drawText` 每次都要
UTF-8 解码（还带一次 vector 分配）→ 逐簇 `faceIdxForCp`（字体 cmap 查表）→
两级哈希查图集格子 → 发射四边形。而静态页面上同一串文本每帧的结果**完全不变**。

### 7.6.2 改动：按 (文本, 字号, 字重, 斜体, 字体面) 缓存整形结果

`GlyphAtlas` 里加 `GlyphRun` 缓存（最多 1024 条，LRU）：命中时只按预先算好的四边形表
发射；`textWidth` 命中时直接返回缓存的总前进宽（连解码 + 逐簇度量都省掉）。
缓存条目带**图集代数**：纹理新建/销毁、`clear()`、换字体面都会 `++atlasGen_` 并清空缓存 ——
句柄与 UV 因此不会指向已销毁的图集页。彩色字形（emoji）在缓存里记 `white=true`
（白顶点不染色），灰字形按调用方的颜色染色，颜色本身不进 key（同一个 run 可换色复用）。

### 7.6.3 实测（同一台机器，`BINUI_DRAW_PROF` + 300 帧 × 7 次取中位）

| 用例 | 遍历耗时（改前 → 改后） | 端到端 fps（0.11.3 → 0.11.4） |
|---|---|---|
| `features` 900×932（43 个标签） | 0.087 → **0.043 ms/帧**（2.0×） | 2657.6 → **2997.6**（+12.8%） |
| `svgdemo` 960×560 | ≈0.059 → **0.040 ms/帧** | 3857.6 → **3950.2** |
| `fontshow` 900×760（整页文字） | 0.030（改前未单独记） | 2676.4 |
| `singlewindow` 1280×720 | 0.030 → **0.017** | 3642.6 → **4296.3**（+17.9%） |

三套像素门禁全部保持原值（`pixels` 10/10 零差异、`pixels-vk` 14/14 不同像素 0、
`pixels-d3d11` 全过）—— 缓存只改了"怎么算出来"，没改"画成什么"。

### 7.6.4 顺手修掉的构建问题：工具的 `-MMD` 依赖从来没生效

工具 exe 的 recipe 里是"编译 + 链接"两条命令，于是 `-MMD` 生成的
`build-mingw/vk_window.d` 没有任何目标依赖它 —— **改了共享头但工具不重编**，
直接链旧 `.o`。本轮就在 `BINUI_DRAW_PROF` 上踩到：加了计时、`make tools` 显示"已最新"、
跑出来的二进制里根本没有这段代码。修法：把 `vk_shot / vk_window / vk_stress / d3d11_shot`
的 `.o` 拆成独立规则（`$(BUILD_DIR)/xxx.o: tools/xxx.cpp`），exe 依赖 `.o`。

### 7.6.5 下一步还能啃的（0.11.4 当时的清单）

- `features` 剩下的 0.043 ms：LABEL 已经没有整形成本了，余下是 80 个节点的
  `renderer.drawColor/drawOpacity/clippingAncestors` 这类逐节点哈希查询
  （≈0.5 µs/节点）；`DIV` 的渐变底色是否也该按节点缓存条带（现在是每帧算）；
- `svgdemo` 的 0.040 ms 里 PATH（type 10）仍是 0.019 ms —— 条带缓存已命中，剩的是重放
  （每帧 2000+ 个 `fillRect`）；
- 顶点上传：GL/VK/D3D11 每帧都 `memcpy` 一次全量顶点（`features` 12654 个 ≈ 253 KB），
  环形缓冲 + 偏移可以省掉这份拷贝（设计 §5.1 一直留着）。

上面三条在 0.11.5 全部咬了一遍，结论见 §7.6.6~§7.6.9。

---

## 7.6' CPU 侧绘制第二轮（2026-11-11，v0.11.5）

### 7.6.6 先把"顶点预算"量出来：`BINUI_VK_PERF` 加顶点计数

`[vk][perf]` 行尾现在会带 `（顶点 8472/帧）`。同时确认了分段口径：
`vk_window` 的 `录制` = `beginFrame → present`，**包含整个共享遍历 + 全部顶点写入**；
`呈现` 从 `present` 开始算，包含 `flush` 那次 `memcpy` 与命令缓冲录制。

一行读数说明问题（`features` 900×932，稳态）：

```
[vk][perf] 360 帧: 平均 0.543 ms/帧 = 录制 0.054 + 等 GPU 0.376 + 呈现 0.115（顶点 8472/帧）
[draw][prof] 60 帧: 遍历 0.039 ms/帧，节点 80/帧（索引 0.001）
```

也就是说：**遍历 0.039 ms 里 8472 个顶点 × 20 B ≈ 169 KB 的写入是主项**
（4.7 ns/顶点），而 `等 GPU` 是 CPU 全套（0.054 + 0.115 = 0.169 ms）的 **2.2 倍**。
这两个数决定了本轮该干什么、以及该在哪里停手。

### 7.6.7 改动一：每节点状态查询合并成一次（共享代码，三后端同享）

遍历里原先对每个节点散着调 `drawColor / drawOpacity / drawOffsetX / drawOffsetY /
drawScale / drawRadius / paintOf / isDisabled` 八个查询，每个内部都要查
`animResult_` + `animState_` + `nodeIndexById_` —— 一个**没有动画**的节点也要 17 次哈希探测。

`BinUIRenderer::drawState(id)` 把这一帧要用的可视属性一次取齐（颜色 / 透明度 / 平移 /
缩放 / 圆角 / 禁用 / 扩展绘制段指针），语义与逐个查询逐条对齐（动画结果优先、样式目标兜底）：

- 新增 `nodeIndexOf(id)`：`nodes_[id].id == id` 直接返回下标（编译器的 id 是 0..N-1 稠密递增），
  哈希表只作兜底；
- 两张动画表**空就直接跳过**（静态页面正是这种情况），不再做必然失败的探测；
- `clippingAncestors` 的祖先链也改走 `nodeIndexOf`；
- 共享遍历里原本每帧重建一张 `unordered_map<uint16_t,size_t> byId`（80 个节点 ≈ 5 µs/帧
  + 一次 malloc），改成"稠密 id 直读 + 必要时懒建表"，索引段 0.005 → **0.001 ms/帧**。

### 7.6.8 改动二：顶点写入改成"预分配 + 指针偏移"（GL / Vulkan / D3D11 各一处）

三个后端原来是 `verts.insert(verts.end(), v, v+6)` / 逐顶点 `push_back`：每条图元都要
做一次容量检查 + 一次 `size` 递增。改成缓冲一次 `resize` 到 `kMaxVerts/kMaxVertices`，
之后每帧只复位计数、按 `data() + vcount` 写，`flush` 时一次 `memcpy` 进设备缓冲。
GL 的 `beginFrame` 也**不再** `verts.clear()`（否则每帧把 2 MB 析构再由写入口扩容一遍）。

**这里踩了一个坑，值得记下来**：第一版是"直接把顶点写进映射过的槽缓冲，省掉拷贝"。
结果 `录制` 从 0.070 ms 涨到 **1.00 ms**（15 倍）—— 映射来的内存是**写合并（WC）**的，
逐顶点 20 字节赋值吃的是非对齐 PCIe 写惩罚：169 KB 走了 0.93 ms（≈180 MB/s），
而同一份数据整块 `memcpy` 只要 ~10 µs（`rep movs`/流式写）。所以拷贝留着，
砍掉的是 `std::vector` 的逐顶点记账。这条注释也写进了 `binui_vk.cpp`。

另外 `PaintLerpArgb` 里 4 通道 × 每帧几十个条带的 `std::lround` 换成 `(v + 0.5f)` 截断
（通道值恒 ≥ 0，逐位等价）。

### 7.6.9 实测（新旧二进制交替跑 15 轮取中位，`vk_window --frames 300`）

| 用例 | 遍历耗时 | 录制耗时（CPU 侧全集） |
|---|---|---|
| `features` 900×932（80 节点 / 8472 顶点） | 0.044 → **0.039 ms/帧**（-11.4%） | 0.044 → **0.040**（-9.1%） |
| `svgdemo` 960×560（64 节点 / 12654 顶点） | 0.041 → **0.038 ms/帧**（-7.3%） | 0.042 → **0.038**（-9.5%） |

同一协议下的**端到端 fps**：`features` 2618.4 → 2634.3（+0.6%）、`svgdemo`
3545.3 → 3609.0（+1.8%）。增益这么小不是没优化到，而是**这两个页面已经 GPU 受限**
（§7.6.6 的 0.376 ms 等 GPU）—— **CPU 侧再抠 10 µs，帧率不动**。这正是本轮停手的依据。

### 7.6.10 剩下的是什么（要再快只能改渲染架构）

`features` 的 0.039 ms 除以 8472 个顶点 = **4.7 ns/顶点**，其中大头是顶点数据本身的
两次写入（CPU 缓冲 + `memcpy`）。要再降一档只能减少"每图元 6 个顶点"这件事本身：

- 索引缓冲：4 顶点 + 6 个 `uint16` 索引，每图元 120 B → 92 B（-23%）；
- 四边形实例化：一个四边形一条记录（x,y/xy/uv/color ≈ 32 B），6 个顶点在着色器里
  按 `gl_VertexIndex` 生成，120 B → 32 B（-73%）。**这条最值**，但要改造三个后端的
  顶点布局 + 着色器，属于渲染架构改动，不在"顺手"范围内；
- 渐变 / 路径条带的**条带数**是像素级近似（圆角区 1px 一条），减条带就等于改像素，不做。

`singlewindow` 那类"节点少、每节点 19 个四边形"的页面，每节点 1.3 µs 里有 0.5 µs 是
`fillRounded/strokeRounded` 的九宫格发射（各 9 个四边形），其余尚未定位到更细的归因。


---

## 8. 风险与回滚

| 风险 | 触发 | 缓解 | 回滚 |
|---|---|---|---|
| 同步错（信号量） | 多帧飞行 | 先按 §4.2 定死数量；开发期常开 validation | 回 P2 骨架 |
| 交换链重建 | resize/DPI/远程会话 | 只记录 `WM_SIZE` 每帧应用、`oldSwapchain` 复用、0 尺寸跳过 | 回 P3 |
| IF 停帧 | 铺满整屏 + 远程/采集 | 读回旁路（与 GL 0.9.1 同策略） | 强制读回 |
| 字节序错 | 首帧颜色 | P2 色卡门槛先钉死 | 回 P2 |
| 图集/图片竞争 | 增量上传 | barrier + 延迟两帧释放 | 全量重建图集 |
| 性能不如 GL | 每帧 barrier/fence | P3 用 `BINUI_PERF` 分段对比，超过 GL 1.3× 就查 | 保持 GL 默认 |
| 范围膨胀 | 想顺手做 Linux | **明确不做**，见调研 §9 | — |

---

## 9. 时间线

```
周 1   P1（2~3d）── 像素中性验证 ──┬── P2 骨架（3~4d）
                                   │
周 2/3 P3 全原语（4~5d）───────────┴── 三方 diff 表
                                   │
周 3   P4 鲁棒性（2~3d）
周 4   P5 工具/文档/0.10.0（1~2d）
```

累计 **12~17 人日**（不含 Linux/Android，不含"见够 8 个坑"的成熟期 —— 那还要 20+ 人日）。

---

## 10. 明确不做

- Linux X11 / Wayland / Android / macOS（macOS 走 Metal；Vulkan 的 macOS 腿等于 MoltenVK，已被 Khronos 标记 deprecated）。
- `VK_KHR_incremental_present`（是呈现带宽提示，不解决内容保留）。
- `VK_EXT_full_screen_exclusive`（更彻底绕开 DWM，采集/远程更糟）。
- VMA（`vk_mem_alloc.h` 17k 行，对 ~50MB 的占用属过度工程；自写 ~200 行线性分配器）。
- dynamic rendering（Vulkan 1.3）：能省 render pass，但抬高门槛；**首版用 1.0 + `VK_KHR_swapchain`**，将来再评估。
- 把 GDI 后端并入 `IBinUIGfx`（语义不同，见 §3.3）。
