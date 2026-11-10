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
| **P2** | Vulkan 骨架：实例/设备/交换链/管线/批次/`fillRect`+`fillRounded`+`present` | 3~4 | `vk_shot` 渲染出"色卡 + 圆角块"，与 GDI 基准 diff ≤ 阈值；跑 10 分钟不崩 | 不接进 demo，仅工具 |
| **P3** | 全原语：线段/文本图集/图片/裁剪栈/MSAA 累积帧/三路 present | 4~5 | 10 个像素用例全部出图并给三方 diff 表；`svgdemo`/`features`/`fontshow` 跑通 | demo 仍用 GL，`GPU=vk` 才切 |
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

---

## 7. P5：工具与发版

- `tools/vk_shot.cpp`：离屏 Vulkan 出图（镜像 `gl_shot`，复用同一套 `offscreen` 目标，输出 PNG）。
- Makefile：`GPU=gl|vk`（默认 gl）切换 `*_vk` 目标与 `vk_shot`；`pixels GPU=vk` 用 `vk_shot` 代替 `offscreen_render`。
- 运行期开关：`BINUI_GPU=vk|gl`（demo 里读环境变量选 `typedef`），默认 gl。
- 文档：`docs/vulkan.md` 补"已实现"章节、README 后端矩阵加一行、`CHANGELOG` 记 **0.10.0**（新后端 = MINOR）。
- 发版门槛同现有规矩：干净重编 0 warning、四套测试全绿、`make pixels` 10/10、`check_examples` 11/11。

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
