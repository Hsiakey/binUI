// binui_vk.cpp - Vulkan 绘制后端（v19，P4：离屏 + 窗口 + 鲁棒性）
//
// 设计（docs/vulkan_design.md §4、§5.9、§5.10）：
//   · 运行期只 LoadLibrary("vulkan-1.dll") + vkGetInstanceProcAddr，不链 vulkan-1.lib；
//   · 每帧画进一张**持久 MSAA 累积图**（loadOp = LOAD），脏矩形靠 scissor + vkCmdClearAttachments；
//   · present：resolve(MSAA → 单采样) 之后二选一 ——
//       (A) blit 进交换链图像 + vkQueuePresentKHR（窗口常规路径）
//       (B) 读回 buffer + SetDIBitsToDevice / UpdateLayeredWindow（透明窗 / 全屏守卫 / 降级）
//     交换链图像**不是**渲染目标，只是显示用副本，所以窗口与离屏像素必然一致；
//   · 顶点 20 字节，与 GL 的 GLVertex 逐位相同；颜色走 B8G8R8A8_UNORM，
//     让 AARRGGBB 的 R/G/B 落到正确的通道（见 MakeVertexFormat 注释）；
//   · 文本/图片与 GL 共用一份实现（binui_glyph_atlas.h / binui_win_image.h），
//     所以像素回归里两个 GPU 后端对轴对齐几何是**逐位相同**的。
#include "backends/binui_vk.h"

#ifndef _WIN32
#error "binui_vk is Windows-only for now (Win32 surface / WSI)"
#endif

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/binui_vk_shaders.h"
#include "binui_paint.h"   // RoundTemplateSize / BuildRoundedNineSlice / PaintQuad
#include "backends/binui_glyph_atlas.h"   // 字形图集 + 排版（与 GL 后端共用）
#include "backends/binui_win_image.h"     // GDI+ 图片解码（与 GL 后端共用）

// 不链接 vulkan-1.lib：全部函数指针自己取（与 binui_gl_ext_min.h 同思路，但这里用官方头，
// 只靠 VK_NO_PROTOTYPES 关掉原型声明）。
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR   // vulkan_win32.h：VkWin32SurfaceCreateInfoKHR / PFN_vkCreateWin32SurfaceKHR
#include "vulkan/vulkan.h"

namespace binui {
namespace {

// QPC 毫秒（帧耗时拆分用；不做缓存的系统调用之外的开销也很小）
double VkNowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ============================================================
// 加载 vulkan-1.dll（进程内只需一次）
// ============================================================
struct VkApi {
    HMODULE lib = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;

#define BINUI_VK_FN(name) PFN_##name name = nullptr;
    BINUI_VK_FN(vkCreateInstance)
    BINUI_VK_FN(vkDestroyInstance)
    BINUI_VK_FN(vkEnumeratePhysicalDevices)
    BINUI_VK_FN(vkGetPhysicalDeviceProperties)
    BINUI_VK_FN(vkGetPhysicalDeviceQueueFamilyProperties)
    BINUI_VK_FN(vkGetPhysicalDeviceMemoryProperties)
    BINUI_VK_FN(vkCreateDevice)
    BINUI_VK_FN(vkDestroyDevice)
    BINUI_VK_FN(vkGetDeviceQueue)
    BINUI_VK_FN(vkDeviceWaitIdle)
    BINUI_VK_FN(vkCreateCommandPool)
    BINUI_VK_FN(vkDestroyCommandPool)
    BINUI_VK_FN(vkAllocateCommandBuffers)
    BINUI_VK_FN(vkBeginCommandBuffer)
    BINUI_VK_FN(vkEndCommandBuffer)
    BINUI_VK_FN(vkResetCommandBuffer)
    BINUI_VK_FN(vkQueueSubmit)
    BINUI_VK_FN(vkQueueWaitIdle)
    BINUI_VK_FN(vkCreateFence)
    BINUI_VK_FN(vkDestroyFence)
    BINUI_VK_FN(vkWaitForFences)
    BINUI_VK_FN(vkResetFences)
    BINUI_VK_FN(vkCreateBuffer)
    BINUI_VK_FN(vkDestroyBuffer)
    BINUI_VK_FN(vkGetBufferMemoryRequirements)
    BINUI_VK_FN(vkBindBufferMemory)
    BINUI_VK_FN(vkMapMemory)
    BINUI_VK_FN(vkUnmapMemory)
    BINUI_VK_FN(vkAllocateMemory)
    BINUI_VK_FN(vkFreeMemory)
    BINUI_VK_FN(vkCreateImage)
    BINUI_VK_FN(vkDestroyImage)
    BINUI_VK_FN(vkGetImageMemoryRequirements)
    BINUI_VK_FN(vkBindImageMemory)
    BINUI_VK_FN(vkCreateImageView)
    BINUI_VK_FN(vkDestroyImageView)
    BINUI_VK_FN(vkCreateSampler)
    BINUI_VK_FN(vkDestroySampler)
    BINUI_VK_FN(vkCreateRenderPass)
    BINUI_VK_FN(vkDestroyRenderPass)
    BINUI_VK_FN(vkCreateFramebuffer)
    BINUI_VK_FN(vkDestroyFramebuffer)
    BINUI_VK_FN(vkCreateShaderModule)
    BINUI_VK_FN(vkDestroyShaderModule)
    BINUI_VK_FN(vkCreateDescriptorSetLayout)
    BINUI_VK_FN(vkDestroyDescriptorSetLayout)
    BINUI_VK_FN(vkCreateDescriptorPool)
    BINUI_VK_FN(vkDestroyDescriptorPool)
    BINUI_VK_FN(vkAllocateDescriptorSets)
    BINUI_VK_FN(vkUpdateDescriptorSets)
    BINUI_VK_FN(vkFreeDescriptorSets)
    BINUI_VK_FN(vkCreatePipelineLayout)
    BINUI_VK_FN(vkDestroyPipelineLayout)
    BINUI_VK_FN(vkCreateGraphicsPipelines)
    BINUI_VK_FN(vkDestroyPipeline)
    BINUI_VK_FN(vkCmdBeginRenderPass)
    BINUI_VK_FN(vkCmdEndRenderPass)
    BINUI_VK_FN(vkCmdBindPipeline)
    BINUI_VK_FN(vkCmdBindVertexBuffers)
    BINUI_VK_FN(vkCmdBindDescriptorSets)
    BINUI_VK_FN(vkCmdSetViewport)
    BINUI_VK_FN(vkCmdSetScissor)
    BINUI_VK_FN(vkCmdPushConstants)
    BINUI_VK_FN(vkCmdDraw)
    BINUI_VK_FN(vkCmdClearAttachments)
    BINUI_VK_FN(vkCmdPipelineBarrier)
    BINUI_VK_FN(vkCmdCopyImageToBuffer)
    BINUI_VK_FN(vkCmdCopyBufferToImage)
    BINUI_VK_FN(vkCmdClearColorImage)

    // ---- WSI（只在窗口模式加载；离屏模式下这些指针必须允许为空）----
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR =
        nullptr;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
    PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
#undef BINUI_VK_FN

    bool load() {
        if (lib) return true;
        lib = ::LoadLibraryA("vulkan-1.dll");
        if (!lib) {
            std::fprintf(stderr, "[vk] 找不到 vulkan-1.dll（未安装 Vulkan 运行库/驱动）\n");
            return false;
        }
        getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                reinterpret_cast<void*>(::GetProcAddress(lib, "vkGetInstanceProcAddr")));
        if (!getInstanceProcAddr) {
            std::fprintf(stderr, "[vk] vulkan-1.dll 里没有 vkGetInstanceProcAddr\n");
            return false;
        }
        // 设备级函数：vkGetInstanceProcAddr 对设备函数同样可用（loader 会给 trampoline）。
        // 注意：这些函数表由**实例持有**（loadInstanceFns 起各自分配），全局只留 vkGetInstanceProcAddr
        // —— 同一个进程里两个 BinUIVK 实例并存时不会互相覆盖（多实例鲁棒性）。
        // 实例层枚举是全局的（实例创建之前就要用），只能在 VK_NULL_HANDLE 上取。
        vkEnumerateInstanceLayerProperties =
            reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
                getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
        return true;
    }

    bool loadInstanceFns(VkInstance inst) {
        auto get = [&](const char* n) { return getInstanceProcAddr(inst, n); };
        vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(get("vkDestroyInstance"));
        vkEnumeratePhysicalDevices =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get("vkEnumeratePhysicalDevices"));
        vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            get("vkGetPhysicalDeviceProperties"));
        vkGetPhysicalDeviceQueueFamilyProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                get("vkGetPhysicalDeviceQueueFamilyProperties"));
        vkGetPhysicalDeviceMemoryProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                get("vkGetPhysicalDeviceMemoryProperties"));
        vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(get("vkCreateDevice"));
        vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(get("vkGetDeviceQueue"));
        return vkEnumeratePhysicalDevices && vkCreateDevice && vkGetDeviceQueue;
    }

    bool loadDeviceFns(VkInstance inst) {
        auto get = [&](const char* n) { return getInstanceProcAddr(inst, n); };
        // 设备级函数用 vkGetDeviceProcAddr 更规范，但 loader 的 instance trampoline 也行；
        // 这里统一走 vkGetInstanceProcAddr（Vulkan 1.0 起即保证可用）。
#define BINUI_VK_LOAD(name)                                                    \
    name = reinterpret_cast<PFN_##name>(get(#name));                            \
    if (!name) {                                                               \
        std::fprintf(stderr, "[vk] 取不到 %s\n", #name);                        \
        return false;                                                          \
    }
        BINUI_VK_LOAD(vkDestroyDevice)
        BINUI_VK_LOAD(vkDeviceWaitIdle)
        BINUI_VK_LOAD(vkCreateCommandPool)
        BINUI_VK_LOAD(vkDestroyCommandPool)
        BINUI_VK_LOAD(vkAllocateCommandBuffers)
        BINUI_VK_LOAD(vkBeginCommandBuffer)
        BINUI_VK_LOAD(vkEndCommandBuffer)
        BINUI_VK_LOAD(vkResetCommandBuffer)
        BINUI_VK_LOAD(vkQueueSubmit)
        BINUI_VK_LOAD(vkQueueWaitIdle)
        BINUI_VK_LOAD(vkCreateFence)
        BINUI_VK_LOAD(vkDestroyFence)
        BINUI_VK_LOAD(vkWaitForFences)
        BINUI_VK_LOAD(vkResetFences)
        BINUI_VK_LOAD(vkCreateBuffer)
        BINUI_VK_LOAD(vkDestroyBuffer)
        BINUI_VK_LOAD(vkGetBufferMemoryRequirements)
        BINUI_VK_LOAD(vkBindBufferMemory)
        BINUI_VK_LOAD(vkMapMemory)
        BINUI_VK_LOAD(vkUnmapMemory)
        BINUI_VK_LOAD(vkAllocateMemory)
        BINUI_VK_LOAD(vkFreeMemory)
        BINUI_VK_LOAD(vkCreateImage)
        BINUI_VK_LOAD(vkDestroyImage)
        BINUI_VK_LOAD(vkGetImageMemoryRequirements)
        BINUI_VK_LOAD(vkBindImageMemory)
        BINUI_VK_LOAD(vkCreateImageView)
        BINUI_VK_LOAD(vkDestroyImageView)
        BINUI_VK_LOAD(vkCreateSampler)
        BINUI_VK_LOAD(vkDestroySampler)
        BINUI_VK_LOAD(vkCreateRenderPass)
        BINUI_VK_LOAD(vkDestroyRenderPass)
        BINUI_VK_LOAD(vkCreateFramebuffer)
        BINUI_VK_LOAD(vkDestroyFramebuffer)
        BINUI_VK_LOAD(vkCreateShaderModule)
        BINUI_VK_LOAD(vkDestroyShaderModule)
        BINUI_VK_LOAD(vkCreateDescriptorSetLayout)
        BINUI_VK_LOAD(vkDestroyDescriptorSetLayout)
        BINUI_VK_LOAD(vkCreateDescriptorPool)
        BINUI_VK_LOAD(vkDestroyDescriptorPool)
        BINUI_VK_LOAD(vkAllocateDescriptorSets)
        BINUI_VK_LOAD(vkUpdateDescriptorSets)
        BINUI_VK_LOAD(vkCreatePipelineLayout)
        BINUI_VK_LOAD(vkDestroyPipelineLayout)
        BINUI_VK_LOAD(vkCreateGraphicsPipelines)
        BINUI_VK_LOAD(vkDestroyPipeline)
        BINUI_VK_LOAD(vkCmdBeginRenderPass)
        BINUI_VK_LOAD(vkCmdEndRenderPass)
        BINUI_VK_LOAD(vkCmdBindPipeline)
        BINUI_VK_LOAD(vkCmdBindVertexBuffers)
        BINUI_VK_LOAD(vkCmdBindDescriptorSets)
        BINUI_VK_LOAD(vkCmdSetViewport)
        BINUI_VK_LOAD(vkCmdSetScissor)
        BINUI_VK_LOAD(vkCmdPushConstants)
        BINUI_VK_LOAD(vkCmdDraw)
        BINUI_VK_LOAD(vkCmdClearAttachments)
        BINUI_VK_LOAD(vkCmdPipelineBarrier)
        BINUI_VK_LOAD(vkCmdCopyImageToBuffer)
        BINUI_VK_LOAD(vkCmdCopyBufferToImage)
        BINUI_VK_LOAD(vkCmdClearColorImage)
        BINUI_VK_LOAD(vkFreeDescriptorSets)
#undef BINUI_VK_LOAD
        return true;
    }

    // ---- WSI 函数：只在窗口模式取，取不到就退回离屏（不把整条后端判死）----
    bool loadSurfaceFns(VkInstance inst) {
        auto get = [&](const char* n) { return getInstanceProcAddr(inst, n); };
#define BINUI_VK_TRY(name) name = reinterpret_cast<PFN_##name>(get(#name))
        BINUI_VK_TRY(vkDestroySurfaceKHR);
        BINUI_VK_TRY(vkCreateWin32SurfaceKHR);
        BINUI_VK_TRY(vkGetPhysicalDeviceSurfaceSupportKHR);
        BINUI_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        BINUI_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR);
        BINUI_VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR);
#undef BINUI_VK_TRY
        const bool ok = vkDestroySurfaceKHR && vkCreateWin32SurfaceKHR &&
                        vkGetPhysicalDeviceSurfaceSupportKHR &&
                        vkGetPhysicalDeviceSurfaceCapabilitiesKHR &&
                        vkGetPhysicalDeviceSurfaceFormatsKHR &&
                        vkGetPhysicalDeviceSurfacePresentModesKHR;
        if (!ok) std::fprintf(stderr, "[vk] WSI 函数不完整（驱动没暴露 surface 扩展）\n");
        return ok;
    }

    bool loadSwapchainFns(VkInstance inst) {
        auto get = [&](const char* n) { return getInstanceProcAddr(inst, n); };
#define BINUI_VK_TRY(name) name = reinterpret_cast<PFN_##name>(get(#name))
        BINUI_VK_TRY(vkCreateSwapchainKHR);
        BINUI_VK_TRY(vkDestroySwapchainKHR);
        BINUI_VK_TRY(vkGetSwapchainImagesKHR);
        BINUI_VK_TRY(vkAcquireNextImageKHR);
        BINUI_VK_TRY(vkQueuePresentKHR);
        BINUI_VK_TRY(vkCmdBlitImage);
        BINUI_VK_TRY(vkCreateSemaphore);
        BINUI_VK_TRY(vkDestroySemaphore);
#undef BINUI_VK_TRY
        const bool ok = vkCreateSwapchainKHR && vkDestroySwapchainKHR &&
                        vkGetSwapchainImagesKHR && vkAcquireNextImageKHR && vkQueuePresentKHR &&
                        vkCmdBlitImage && vkCreateSemaphore && vkDestroySemaphore;
        if (!ok) std::fprintf(stderr, "[vk] 交换链函数不完整（驱动不支持 VK_KHR_swapchain）\n");
        return ok;
    }

};

VkApi g_vk;   // 进程内单例（loader 只加载一次）

const char* VkResultText(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VK_RESULT_?";
    }
}

#define VK_CHECK(expr, what)                                                        \
    do {                                                                            \
        const VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                      \
            std::fprintf(stderr, "[vk] %s 失败: %s (%d)\n", (what),                  \
                         VkResultText(_r), (int)_r);                                 \
            return false;                                                            \
        }                                                                            \
    } while (0)

// 顶点：与 GL 的 GLVertex 逐位相同（20 字节）。
// 颜色属性用 **B8G8R8A8_UNORM**：0xAARRGGBB 在小端内存里是 BB GG RR AA，
// 按 BGRA 解释正好得到 (R,G,B,A)，与作者的直觉一致；目标图也用 B8G8R8A8_UNORM，
// 读写两头一致 → 屏幕上就是作者写的颜色（GL 那边靠 FBO+GL_BGRA 读回抵消，路子不同结论相同）。
struct VkVertex {
    float x, y;
    uint32_t color;
    float u, v;
};
static_assert(sizeof(VkVertex) == 20, "顶点必须与 GLVertex 同 20 字节");

constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr size_t kMaxVertices = 100000;      // 与 GL 的 kMaxVerts 一致（约 16.6k 四边形）
constexpr int kMaxBatches = 2048;

uint32_t ScaleAlphaU32(uint32_t c, float op) {
    if (op >= 1.0f) return c;
    if (op <= 0.0f) return c & 0x00FFFFFFu;
    uint32_t a = static_cast<uint32_t>(((c >> 24) & 0xFF) * op);
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | (a << 24);
}

void Premult(uint32_t& c) {
    const uint32_t a = (c >> 24) & 0xFF;
    if (a == 255) return;
    const uint32_t r = ((c >> 16) & 0xFF) * a / 255;
    const uint32_t g = ((c >> 8) & 0xFF) * a / 255;
    const uint32_t b = (c & 0xFF) * a / 255;
    c = (a << 24) | (r << 16) | (g << 8) | b;
}

} // namespace

// ============================================================
// Impl
// ============================================================
struct BinUIVK::Impl : binui::IGlyphTexHost {
    // ---- 设备 ----
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;

    // ---- 命令（P6：双槽流水线）----
    // 每槽一套"命令缓冲 + fence + acquire/present 信号量 + 顶点缓冲 + staging 待释放表"，
    // 于是 CPU 录第 N+1 帧时可以不管 GPU 还在跑第 N 帧。
    //
    // 为什么不是 3 槽 / 无限制 in-flight：渲染目标（4x MSAA 累积图 + resolve 图）和纹理布局
    // 是**帧间共享**的，GPU 上永远只允许一帧在跑（累积帧 loadOp=LOAD，两帧并发会互相踩）。
    // 所以在 vkQueueSubmit 之前必须等上一帧的 fence。双槽刚好够把"录制"和"执行"重叠一步，
    // 再多的槽只会多占显存、不会多出并行度。见 docs/vulkan_design.md §7.2。
    static constexpr int kSlots = 2;
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore semAvail = VK_NULL_HANDLE;   // 图像可用
        VkSemaphore semDone = VK_NULL_HANDLE;    // 渲染完成
        VkBuffer vbuf = VK_NULL_HANDLE;
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkVertex* vmap = nullptr;
        std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;   // 等本槽 fence 后释放
        bool submitted = false;   // 已提交、fence 还没等（只有一帧会处于这个状态）
    };
    Slot slots[kSlots];
    int slotIdx = 0;      // 当前录制槽
    int prevSlot = -1;    // 上一帧提交的槽：提交新一帧前必须先等它
    long long frameNo = 0;
    // 当前槽的快捷别名：绘制/上传代码只认这三个，beginFrame 里换成新槽
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    VkVertex* vmap = nullptr;
    // ---- 帧耗时拆分（BINUI_VK_PERF=1：每 60 帧一行）----
    // 判断瓶颈在 CPU 录制还是 GPU 执行：录制 ≫ 等 GPU 说明是 CPU 侧（路径细分/排版），
    // 等 GPU ≫ 录制说明是填充率/带宽（大窗口 + 4x MSAA / 读回）。
    bool perfOn = std::getenv("BINUI_VK_PERF") != nullptr;
    double perfTBegin = 0.0;      // beginFrame 进入时刻
    double perfWaitFrame = 0.0;   // 本帧等 fence 的累计
    double perfRec = 0.0, perfWait = 0.0, perfPres = 0.0;
    size_t perfVerts = 0;        // 60 帧窗口内的顶点累计（给出"每帧提交了多少几何"）
    long long perfN = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool recording = false;

    // ---- 目标 ----
    int w = 0, h = 0;
    // 最近一次读回的实际尺寸（GDI 路径 = w×h；交换链捕获 = swapExtent）
    int capW = 0, capH = 0;
    VkImage msaaImage = VK_NULL_HANDLE;
    VkDeviceMemory msaaMem = VK_NULL_HANDLE;
    VkImageView msaaView = VK_NULL_HANDLE;
    VkImage resolveImage = VK_NULL_HANDLE;
    VkDeviceMemory resolveMem = VK_NULL_HANDLE;
    VkImageView resolveView = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;
    void* readbackMapped = nullptr;

    // ---- 窗口 / 交换链（hwnd != nullptr 时才有）----
    // 设计选择：渲染目标始终是 MSAA + resolve 这套离屏图（与离屏模式完全同一条路径），
    // present 时才把 resolve 图 blit 到交换链图像。这样窗口模式与离屏模式的像素必然一致，
    // 也省掉"每个交换链图像一套 framebuffer + render pass"的复杂度。
    HWND hwnd = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = kColorFormat;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    bool windowMode = false;
    bool useSwapchain = false;               // false = 只能读回 + GDI 贴屏
    bool transparent = false;                // 分层窗口（UpdateLayeredWindow）
    bool forceGdi = false;                   // 拖拽 resize 期由上层置位
    bool coversMonitor = false;              // 客户区铺满整块显示器 → 绕开直翻转
    bool capture = false;                    // 每帧把交换链图像读回（工具/诊断用）
    bool lastPresentWasGdi = false;

    // GDI 贴屏/分层窗口用：32bit BGRA，自上而下
    HBITMAP dib = nullptr;
    HDC dibDC = nullptr;
    void* dibBits = nullptr;
    int dibW = 0, dibH = 0;

    // ---- 管线 ----
    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dsPool = VK_NULL_HANDLE;

    // ---- 采样器 ----
    VkSampler samplerNearest = VK_NULL_HANDLE;
    VkSampler samplerLinear = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> dsPools;   // 池满就再加一个（不回收 descriptor set 池本身）

    // ---- 纹理 ----
    // 句柄：Tex* 直接当 IGlyphTexHost 的 void* 用（unordered_map 的节点地址稳定）。
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        int w = 0, h = 0;
        bool linear = false;
        bool needClear = true;      // 新建图像内容未定义：第一次用之前必须清零
        // 已经**记录进命令缓冲**的布局（每帧一开始 cmd 重置，但上一帧提交时
        // 我们就等到 fence 了，所以这个字段始终等于图像的真实布局）
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    Tex white;
    struct RoundTex {
        Tex tex;
        int t = 0;
    };
    std::unordered_map<int, RoundTex> rounds;   // key = 半径（描边用负值，与 GL 同）
    std::unordered_map<int, Tex> images;        // 图片（按 index）
    std::unordered_map<int, uint32_t> imageGen; // 换图 generation：不匹配时重新上传

    // 帧外产生的上传（测量文本时会光栅化字形，那时命令缓冲还没开始录制）。
    // 这些先攒着，在下一个 beginFrame 里、render pass 开始之前一次性录进去 ——
    // 所以**不需要预热帧**：同一帧内先上传后绘制，像素就在这一帧里。
    struct PendingUpload {
        Tex* tex = nullptr;
        int x = 0, y = 0, w = 0, h = 0;
        int rowBytes = 0;
        std::vector<uint8_t> data;
    };
    std::vector<PendingUpload> pendingUploads;
    bool cmdOpen = false;   // 命令缓冲正在录制（帧内，或一次性的初始化提交）

    // ---- 文本（排版/图集在共享头里，本类只当纹理宿主）----
    GlyphAtlas atlas;
    std::vector<Tex*> glyphTexes;   // 图集纹理（shutdown 时统一释放）

    // v20：顶点先攒在**预分配好的** CPU 缓冲里，提交时整块 memcpy 进映射的槽缓冲。
    // 实测过"直接写映射内存省掉拷贝"这条路：映射来的内存是写合并（WC）的，
    // 逐顶点 20 字节赋值吃的是非对齐 PCIe 写惩罚，录制从 0.070 ms 涨到 **1.00 ms**
    //（`features` 8472 顶点/帧，169 KB 走了 0.93 ms ≈ 180 MB/s）。整块 memcpy
    // 用的是 rep movs/流式写，同样的数据只要 ~10 µs。所以拷贝留着，去掉的是
    // std::vector 的逐顶点 push_back（容量检查 + size 递增）：缓冲一次 resize 到位，
    // 写顶点就是一个指针 + 偏移。
    std::vector<VkVertex> verts;                 // 本帧顶点（预分配到 kMaxVertices）
    size_t vcount = 0;                           // 本帧已写顶点数
    size_t frameVerts = 0;                       // 上一帧最终提交的顶点数（perf 用）

    struct Batch {
        uint32_t first = 0;
        uint32_t count = 0;
        Tex* tex = nullptr;      // 采样纹理（nullptr = 纯白 1×1，纯色四边形）
        int sx = 0, sy = 0, sw = 0, sh = 0;
    };
    std::vector<Batch> batches;
    Batch cur{};
    int clipL = 0, clipT = 0, clipR = 0, clipB = 0;
    int scissorStack[32] = {};
    int scissorDepth = 0;
    bool vsync = true;
    bool ready = false;

    // ---- 显存记账（P4：区分"我们漏了"和"驱动池化"）----
    // 只统计 vkAllocateMemory/vkFreeMemory 这一对，swapchain 图像由驱动持有，不在账上。
    std::unordered_map<VkDeviceMemory, VkDeviceSize> memSizes;
    size_t memBytes = 0;

    void trackMem(VkDeviceMemory m, VkDeviceSize bytes) {
        if (!m) return;
        memSizes[m] = bytes;
        memBytes += static_cast<size_t>(bytes);
    }

    void freeMem(VkDeviceMemory& m) {
        if (!m) return;
        auto it = memSizes.find(m);
        if (it != memSizes.end()) {
            memBytes -= static_cast<size_t>(it->second);
            memSizes.erase(it);
        }
        g_vk.vkFreeMemory(device, m, nullptr);
        m = VK_NULL_HANDLE;
    }

    // ============================================================
    // 双槽流水线：槽的建立 / 等待 / 回收
    // ============================================================
    bool initSlot(Slot& s) {
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = cmdPool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        if (g_vk.vkAllocateCommandBuffers(device, &cbi, &s.cmd) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] initSlot: vkAllocateCommandBuffers 失败\n");
            return false;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // ★ 必须一开始就是 signaled：beginFrame 的第一步是 wait 这一槽的 fence，
        // 建出来是 unsignaled 的话第一帧直接死等（P2 实测挂死，CPU 0%）。
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (g_vk.vkCreateFence(device, &fci, nullptr, &s.fence) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] initSlot: vkCreateFence 失败\n");
            return false;
        }
        return true;
    }

    // 交换链信号量：与交换链同生共死。
    // 它们在 WSI 那一组函数里（只有 loadSwapchainFns 之后才非空），所以离屏模式没有这两个对象。
    bool createSlotSemaphores() {
        if (!g_vk.vkCreateSemaphore) return false;
        VkSemaphoreCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (int i = 0; i < kSlots; i++) {
            if (g_vk.vkCreateSemaphore(device, &smi, nullptr, &slots[i].semAvail) != VK_SUCCESS ||
                g_vk.vkCreateSemaphore(device, &smi, nullptr, &slots[i].semDone) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] 信号量创建失败，退回读回呈现\n");
                return false;
            }
        }
        return true;
    }

    void destroySlotSemaphores() {
        for (int i = 0; i < kSlots; i++) {
            Slot& s = slots[i];
            if (s.semAvail && g_vk.vkDestroySemaphore)
                g_vk.vkDestroySemaphore(device, s.semAvail, nullptr);
            if (s.semDone && g_vk.vkDestroySemaphore)
                g_vk.vkDestroySemaphore(device, s.semDone, nullptr);
            s.semAvail = s.semDone = VK_NULL_HANDLE;
        }
    }

    // 等 fence（BINUI_VK_PERF 时把等待时间记进本帧）
    void waitFence(VkFence f) {
        if (perfOn) {
            const double t = VkNowMs();
            g_vk.vkWaitForFences(device, 1, &f, VK_TRUE, UINT64_MAX);
            perfWaitFrame += VkNowMs() - t;
            return;
        }
        g_vk.vkWaitForFences(device, 1, &f, VK_TRUE, UINT64_MAX);
    }

    void freeStaging(Slot& s) {
        for (auto& kv : s.staging) {
            g_vk.vkDestroyBuffer(device, kv.first, nullptr);
            freeMem(kv.second);
        }
        s.staging.clear();
    }

    // 等这一槽的 GPU 工作跑完，并回收它的 staging
    void waitSlot(Slot& s) {
        if (!s.submitted) return;
        waitFence(s.fence);
        s.submitted = false;
        freeStaging(s);
    }

    Slot& cs() { return slots[slotIdx]; }

    // ============================================================
    // 帧耗时拆分（BINUI_VK_PERF=1）
    // ============================================================
    void perfFrameAdd(double rec, double wait, double pres, size_t nverts) {
        perfRec += rec;
        perfWait += wait;
        perfPres += pres;
        perfVerts += nverts;
        if (++perfN % 60 != 0) return;
        const double n = 60.0;
        std::fprintf(stderr,
                     "[vk][perf] %lld 帧: 平均 %.3f ms/帧 = 录制 %.3f + 等 GPU %.3f + 呈现 %.3f"
                     "（顶点 %zu/帧）\n",
                     perfN, (perfRec + perfWait + perfPres) / n, perfRec / n, perfWait / n,
                     perfPres / n, static_cast<size_t>(perfVerts / 60));
        perfRec = perfWait = perfPres = 0.0;
        perfVerts = 0;
    }

    void perfBeginFrame() {
        if (!perfOn) return;
        perfTBegin = VkNowMs();
        perfWaitFrame = 0.0;
    }

    void perfEndFrame(double tPresent) {
        if (!perfOn) return;
        const double now = VkNowMs();
        perfFrameAdd(tPresent - perfTBegin, perfWaitFrame, now - tPresent - perfWaitFrame,
                     frameVerts);
    }

    // ============================================================
    // 小工具
    // ============================================================
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want)
                return i;
        }
        return UINT32_MAX;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (g_vk.vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        g_vk.vkGetBufferMemoryRequirements(device, buf, &req);
        const uint32_t type = findMemoryType(req.memoryTypeBits, props);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (g_vk.vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        trackMem(mem, req.size);
        return g_vk.vkBindBufferMemory(device, buf, mem, 0) == VK_SUCCESS;
    }

    bool createImage2D(int iw, int ih, VkFormat fmt, VkImageUsageFlags usage,
                       VkSampleCountFlagBits sampleCount, VkImage& img,
                       VkDeviceMemory& mem, VkImageAspectFlags aspect) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = sampleCount;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (g_vk.vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] createImage2D: vkCreateImage 失败 %dx%d\n", iw, ih);
            return false;
        }
        VkMemoryRequirements req{};
        g_vk.vkGetImageMemoryRequirements(device, img, &req);
        const uint32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            std::fprintf(stderr, "[vk] createImage2D: 找不到 DEVICE_LOCAL 内存类型 (bits=0x%x)\n",
                         req.memoryTypeBits);
            return false;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkResult ar = g_vk.vkAllocateMemory(device, &ai, nullptr, &mem);
        if (ar != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] createImage2D: vkAllocateMemory 失败 %s\n", VkResultText(ar));
            return false;
        }
        trackMem(mem, req.size);
        ar = g_vk.vkBindImageMemory(device, img, mem, 0);
        if (ar != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] createImage2D: vkBindImageMemory 失败 %s\n", VkResultText(ar));
            return false;
        }
        (void)aspect;   // view 由调用点用 makeView() 建
        return true;
    }

    VkImageView makeView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        if (g_vk.vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        return view;
    }

    void barrier(VkImage img, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        g_vk.vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ============================================================
    // 设备/目标
    // ============================================================
    bool pickPhysical() {
        uint32_t n = 0;
        g_vk.vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (n == 0) {
            std::fprintf(stderr, "[vk] 没有可用 Vulkan 物理设备\n");
            return false;
        }
        std::vector<VkPhysicalDevice> devs(n);
        g_vk.vkEnumeratePhysicalDevices(instance, &n, devs.data());
        // 优先独立显卡
        VkPhysicalDevice best = VK_NULL_HANDLE;
        int bestScore = -1;
        for (VkPhysicalDevice d : devs) {
            VkPhysicalDeviceProperties p{};
            g_vk.vkGetPhysicalDeviceProperties(d, &p);
            uint32_t qn = 0;
            g_vk.vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            g_vk.vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, q.data());
            int gfx = -1;
            for (uint32_t i = 0; i < qn; i++) {
                if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                // 窗口模式：队列还要能往这块 surface 上呈现（Win32 上通常是同一个队列）
                if (surface) {
                    VkBool32 sup = VK_FALSE;
                    g_vk.vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &sup);
                    if (!sup) continue;
                }
                gfx = static_cast<int>(i);
                break;
            }
            if (gfx < 0) continue;
            int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 100 : 10;
            if (score > bestScore) {
                bestScore = score;
                best = d;
                queueFamily = static_cast<uint32_t>(gfx);
                // 4x MSAA 不是所有实现都支持，取最小值
                const VkSampleCountFlags sc = p.limits.framebufferColorSampleCounts;
                samples = (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
                        : (sc & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
                                                       : VK_SAMPLE_COUNT_1_BIT;
                std::fprintf(stderr, "[vk] 设备: %s (%s, API %u.%u.%u, MSAA %dx)\n",
                             p.deviceName,
                             p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "独显" : "其他",
                             VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                             VK_VERSION_PATCH(p.apiVersion),
                             static_cast<int>(samples));
            }
        }
        if (!best) {
            std::fprintf(stderr, "[vk] 没有带图形队列的 Vulkan 设备\n");
            return false;
        }
        physical = best;
        g_vk.vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
        return true;
    }

    bool createDevice() {
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (windowMode) {
            dci.enabledExtensionCount = 1;
            dci.ppEnabledExtensionNames = devExt;
        }
        VK_CHECK(g_vk.vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        g_vk.vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (!g_vk.loadDeviceFns(instance)) return false;
        return true;
    }

    void destroyTargets() {
        if (framebuffer) { g_vk.vkDestroyFramebuffer(device, framebuffer, nullptr); framebuffer = VK_NULL_HANDLE; }
        if (renderPass) { g_vk.vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }
        if (msaaView) { g_vk.vkDestroyImageView(device, msaaView, nullptr); msaaView = VK_NULL_HANDLE; }
        if (resolveView) { g_vk.vkDestroyImageView(device, resolveView, nullptr); resolveView = VK_NULL_HANDLE; }
        if (msaaImage) { g_vk.vkDestroyImage(device, msaaImage, nullptr); msaaImage = VK_NULL_HANDLE; }
        if (resolveImage) { g_vk.vkDestroyImage(device, resolveImage, nullptr); resolveImage = VK_NULL_HANDLE; }
        freeMem(msaaMem);
        freeMem(resolveMem);
        if (readback) { g_vk.vkDestroyBuffer(device, readback, nullptr); readback = VK_NULL_HANDLE; }
        freeMem(readbackMem);
    readbackMapped = nullptr;
    }

    bool createTargets() {
        destroyTargets();
        auto fail = [](const char* step) {
            std::fprintf(stderr, "[vk] createTargets 失败于: %s\n", step);
            return false;
        };

        // MSAA 累积图 + 单采样 resolve 图（present 时拷到 readback buffer）
        if (!createImage2D(w, h, kColorFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           samples, msaaImage, msaaMem, VK_IMAGE_ASPECT_COLOR_BIT))
            return fail("msaa image");
        if (!createImage2D(w, h, kColorFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_SAMPLE_COUNT_1_BIT, resolveImage, resolveMem, VK_IMAGE_ASPECT_COLOR_BIT))
            return fail("resolve image");
        msaaView = makeView(msaaImage, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        resolveView = makeView(resolveImage, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!msaaView || !resolveView) return fail("image views");

        // render pass：attachment0 = MSAA 颜色（LOAD，present 前不 swap），
        // attachment1 = resolve 目标（DONT_CARE 加载、STORE、末尾转 TRANSFER_SRC）
        VkAttachmentDescription att[2]{};
        att[0].format = kColorFormat;
        att[0].samples = samples;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;      // 累积帧：跨帧保留
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = kColorFormat;
        att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference resolveRef{};
        resolveRef.attachment = 1;
        resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;
        sub.pResolveAttachments = (samples == VK_SAMPLE_COUNT_1_BIT) ? nullptr : &resolveRef;

        VkSubpassDependency dep[2]{};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask = 0;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 2;
        rpi.pAttachments = att;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        rpi.dependencyCount = 2;
        rpi.pDependencies = dep;
        VK_CHECK(g_vk.vkCreateRenderPass(device, &rpi, nullptr, &renderPass), "vkCreateRenderPass");

        VkImageView views[2] = {msaaView, resolveView};
        VkFramebufferCreateInfo fbi{};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = renderPass;
        fbi.attachmentCount = 2;
        fbi.pAttachments = views;
        fbi.width = static_cast<uint32_t>(w);
        fbi.height = static_cast<uint32_t>(h);
        fbi.layers = 1;
        VK_CHECK(g_vk.vkCreateFramebuffer(device, &fbi, nullptr, &framebuffer), "vkCreateFramebuffer");

        // 读回 buffer（host visible）
        // 尺寸取"渲染目标"与"交换链"两者的较大者：DPI 缩放/跨显示器移动时 swapchain 的
        // currentExtent 可能比客户区尺寸大，捕获路径按 swapExtent 拷，缓冲区必须装得下
        //（否则就是一次越界写 —— P4 复查这块时发现的隐患）。
        const size_t capArea = std::max(static_cast<size_t>(w) * h,
                                        static_cast<size_t>(swapExtent.width) *
                                            swapExtent.height);
        if (!createBuffer(static_cast<VkDeviceSize>(capArea) * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          readback, readbackMem))
            return fail("readback buffer");
        if (g_vk.vkMapMemory(device, readbackMem, 0, VK_WHOLE_SIZE, 0, &readbackMapped) != VK_SUCCESS)
            return fail("map readback");

        // 首帧前把两张图转进 render pass 需要的布局
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(g_vk.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        barrier(msaaImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        barrier(resolveImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VK_CHECK(g_vk.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(g_vk.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(layout)");
        g_vk.vkQueueWaitIdle(queue);
        return true;
    }

    // ============================================================
    // 交换链（窗口模式）
    // ============================================================
    void destroySwapchain() {
        destroySlotSemaphores();
        for (VkImageView v : swapViews)
            if (v) g_vk.vkDestroyImageView(device, v, nullptr);
        swapViews.clear();
        swapImages.clear();
        if (swapchain) {
            g_vk.vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    // 首选 B8G8R8A8_UNORM（与渲染目标同格式，blit 是纯拷贝）；没有就退回驱动给的第一种
    VkSurfaceFormatKHR pickSurfaceFormat() {
        uint32_t n = 0;
        g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
        VkSurfaceFormatKHR out{};
        out.format = kColorFormat;
        out.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        if (n == 0) return out;
        std::vector<VkSurfaceFormatKHR> fs(n);
        g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, fs.data());
        for (const auto& f : fs) {
            if (f.format == kColorFormat && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
        }
        if (n == 1 && fs[0].format == VK_FORMAT_UNDEFINED) return out;   // 任意格式
        return fs[0];
    }

    // vsync=false 时优先 MAILBOX（不撕裂、不阻塞），否则 IMMEDIATE；都没有就 FIFO
    VkPresentModeKHR pickPresentMode(bool vsync) {
        uint32_t n = 0;
        g_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr);
        if (n == 0) return VK_PRESENT_MODE_FIFO_KHR;
        std::vector<VkPresentModeKHR> ms(n);
        g_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, ms.data());
        auto has = [&](VkPresentModeKHR m) {
            for (VkPresentModeKHR x : ms) if (x == m) return true;
            return false;
        };
        if (vsync) return has(VK_PRESENT_MODE_FIFO_KHR) ? VK_PRESENT_MODE_FIFO_KHR : ms[0];
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    bool createSwapchain() {
        if (!windowMode || !surface) return false;
        destroySwapchain();

        VkSurfaceCapabilitiesKHR caps{};
        if (g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 取不到 surface capabilities\n");
            return false;
        }
        const VkSurfaceFormatKHR sf = pickSurfaceFormat();
        // currentExtent 是 0xFFFFFFFF 时由应用决定（Win32 上通常直接给出客户区尺寸）
        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu || ext.height == 0xFFFFFFFFu) {
            ext.width = static_cast<uint32_t>(std::max(1, w));
            ext.height = static_cast<uint32_t>(std::max(1, h));
        }
        ext.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, ext.width));
        ext.height =
            std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, ext.height));
        if (ext.width == 0 || ext.height == 0) return false;

        uint32_t imageCount = caps.minImageCount + 1;   // 多要一张，避免等待
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha)) {
            // 透明窗口不走交换链（Windows 上 PRE_MULTIPLIED 只有 ~1/3 设备支持，见设计 §5.6），
            // 但驱动连 OPAQUE 都不给时要挑一个能用的
            const VkCompositeAlphaFlagBitsKHR all[] = {
                VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
            for (auto a : all)
                if (caps.supportedCompositeAlpha & a) { alpha = a; break; }
        }

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface;
        ci.minImageCount = imageCount;
        ci.imageFormat = sf.format;
        ci.imageColorSpace = sf.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        // TRANSFER_DST：把 resolve 图 blit 上去；TRANSFER_SRC：工具/诊断时读回交换链内容
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                              ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                              : caps.currentTransform;
        ci.compositeAlpha = alpha;
        ci.presentMode = pickPresentMode(vsync);
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = VK_NULL_HANDLE;

        const VkResult cr = g_vk.vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain);
        if (cr != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] vkCreateSwapchainKHR 失败: %s (%d)\n", VkResultText(cr),
                         (int)cr);
            swapchain = VK_NULL_HANDLE;
            return false;
        }
        uint32_t imgN = 0;
        g_vk.vkGetSwapchainImagesKHR(device, swapchain, &imgN, nullptr);
        swapImages.resize(imgN);
        g_vk.vkGetSwapchainImagesKHR(device, swapchain, &imgN, swapImages.data());
        swapViews.resize(imgN, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < imgN; i++) {
            swapViews[i] = makeView(swapImages[i], sf.format, VK_IMAGE_ASPECT_COLOR_BIT);
            if (!swapViews[i]) {
                std::fprintf(stderr, "[vk] 交换链图像 %u 建 view 失败\n", i);
                destroySwapchain();
                return false;
            }
        }
        swapFormat = sf.format;
        swapExtent = ext;
        // 信号量与交换链同生共死：重建交换链时一起重建（此刻设备已 idle，见 recreateSwapchain）
        if (!createSlotSemaphores()) {
            destroySwapchain();
            return false;
        }
        // 拖边框时尺寸每帧变一次、交换链每帧重建一次：默认不打印，排查用 BINUI_VK_TRACE=1
        if (std::getenv("BINUI_VK_TRACE"))
            std::fprintf(stderr, "[vk] 交换链 %ux%u，%u 张，%s，presentMode=%d\n", ext.width,
                         ext.height, imgN, VkFormatName(sf.format), (int)ci.presentMode);
        return true;
    }

    // resize 后交换链可能失效（0 尺寸 / 超出 caps），重建一次即可
    bool recreateSwapchain() {
        g_vk.vkDeviceWaitIdle(device);
        return createSwapchain();
    }

    static const char* VkFormatName(VkFormat f) {
        switch (f) {
            case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
            case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
            case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
            case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
            default: return "其他格式";
        }
    }

    // 丢帧（最小化 / 交换链过期 / acquire 失败）时也必须把命令缓冲**提交**掉：
    // beginFrame 每帧开头等这一槽的 fence，而 fence 只在 vkQueueSubmit 时才会置位 ——
    // 漏提交就是"下一帧永远等不到"的死锁。压力测试第一轮最小化就抓到了这个（挂死）。
    //
    // 丢帧是异常路径，直接同步掉（提交 + 等完），不留"在飞的一帧"给下一帧收拾。
    void submitNoPresent() {
        Slot& s = cs();
        // 正常帧的串行化点在 present()：丢帧也一样，提交前先等上一帧
        if (prevSlot >= 0 && prevSlot != slotIdx) waitSlot(slots[prevSlot]);
        prevSlot = -1;
        g_vk.vkEndCommandBuffer(s.cmd);
        recording = false;
        cmdOpen = false;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        if (g_vk.vkQueueSubmit(queue, 1, &si, s.fence) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] submitNoPresent: vkQueueSubmit 失败，后端转为未就绪\n");
            ready = false;
            return;
        }
        s.submitted = true;
        waitSlot(s);
    }
    // 客户区是否铺满整块显示器（borderless 全屏）。
    // 与 GL 后端 0.9.1 的 clientCoversMonitor() 同一判定与同一理由：这种状态下 DWM 会把
    // 呈现提升成直翻转，实测在远程桌面/虚拟显示器上画面会停住（应用仍在满帧绘制）。
    bool clientCoversMonitor() const {
        if (!hwnd) return false;
        RECT cr;
        if (!GetClientRect(hwnd, &cr)) return false;
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
        const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
        const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        return (cr.right - cr.left) >= mw && (cr.bottom - cr.top) >= mh;
    }

    bool ensureDib(int dw, int dh) {
        if (dib && dibW == dw && dibH == dh) return true;
        if (dib) DeleteObject(dib);
        if (dibDC) DeleteDC(dibDC);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = dw;
        bi.bmiHeader.biHeight = -dh;   // 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        dibDC = CreateCompatibleDC(nullptr);
        dibW = dw;
        dibH = dh;
        return dib && dibBits && dibDC;
    }

    // 把读回缓冲贴到窗口（非透明）/ 合成到桌面（透明）
    bool blitToWindow(const uint8_t* px, int pw, int ph) {
        if (!hwnd || !px) return false;
        if (transparent) {
            if (!ensureDib(pw, ph)) return false;
            std::memcpy(dibBits, px, static_cast<size_t>(pw) * ph * 4);
            HDC screen = GetDC(nullptr);
            HGDIOBJ old = SelectObject(dibDC, dib);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            POINT dst = {wr.left, wr.top};
            SIZE sz = {pw, ph};
            POINT src = {0, 0};
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(hwnd, screen, &dst, &sz, dibDC, &src, 0, &bf, ULW_ALPHA);
            SelectObject(dibDC, old);
            ReleaseDC(nullptr, screen);
            return true;
        }
        // 读回缓冲本身是自上而下的 BGRA，直接当 DIB 位用（SetDIBitsToDevice 不缩放）
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = pw;
        bi.bmiHeader.biHeight = -ph;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(hwnd);
        SetDIBitsToDevice(dc, 0, 0, pw, ph, 0, 0, 0, ph, px, &bi, DIB_RGB_COLORS);
        ReleaseDC(hwnd, dc);
        return true;
    }

    // ============================================================
    // 纹理
    // ------------------------------------------------------------
    // 与 P2 的差别（P2 是"上传不能发生在帧内"）：
    //   · 上传**可以发生在帧内**：staging 拷进本帧命令缓冲，帧末等 fence 后释放，
    //     所以不需要预热帧，也不会 reset 正在录制的命令缓冲；
    //   · 帧外产生的上传（排版阶段光栅化字形、prepareImage）先排进 pendingUploads，
    //     下一个 beginFrame 里在 render pass 开始之前一次性录进去 ——
    //     同一帧内先上传后绘制，像素就在这一帧里；
    //   · 图像内容不会被驱动清零，新建图像第一次用之前必须 clear：
    //     图集里的"空格子"（空格）会被当真字形画，脏显存就是噪点。
    // ============================================================

    // 建纹理（图像 + view + descriptor set），不带数据
    bool createTexImage(Tex& t, int tw, int th, bool linear) {
        if (!createImage2D(tw, th, kColorFormat,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_SAMPLE_COUNT_1_BIT, t.image, t.mem, VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        t.view = makeView(t.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.view) {
            std::fprintf(stderr, "[vk] createTexImage: vkCreateImageView 失败\n");
            return false;
        }
        t.w = tw;
        t.h = th;
        t.linear = linear;
        t.needClear = true;
        t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return allocTexDs(t);
    }

    // 池满就再开一个（纹理数量随图片/图集代数增长，固定容量迟早会满）
    bool growPool() {
        VkDescriptorPoolSize sz{};
        sz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sz.descriptorCount = 256;
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 256;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &sz;
        VkDescriptorPool p = VK_NULL_HANDLE;
        if (g_vk.vkCreateDescriptorPool(device, &pi, nullptr, &p) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] growPool: vkCreateDescriptorPool 失败\n");
            return false;
        }
        dsPools.push_back(p);
        dsPool = p;
        return true;
    }

    bool allocTexDs(Tex& t) {
        for (int attempt = 0; attempt < 2; attempt++) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = dsPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &dsLayout;
            if (g_vk.vkAllocateDescriptorSets(device, &ai, &t.ds) == VK_SUCCESS) {
                writeTexDs(t);
                return true;
            }
            if (!growPool()) return false;
        }
        std::fprintf(stderr, "[vk] allocTexDs: descriptor set 分配失败\n");
        return false;
    }

    void writeTexDs(Tex& t) {
        VkDescriptorImageInfo dii{};
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dii.imageView = t.view;
        dii.sampler = t.linear ? samplerLinear : samplerNearest;
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = t.ds;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &dii;
        g_vk.vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
    }

    // 释放一个纹理（图像/view/descriptor set）。调用前它不能再被本帧的顶点引用。
    void freeTex(Tex& t) {
        // 队列里还没录进去的上传：跟着作废（否则会在下一帧写进已销毁的图像）
        for (size_t i = 0; i < pendingUploads.size();) {
            if (pendingUploads[i].tex == &t)
                pendingUploads.erase(pendingUploads.begin() + static_cast<long>(i));
            else
                i++;
        }
        if (t.ds) g_vk.vkFreeDescriptorSets(device, dsPool, 1, &t.ds);
        if (t.view) g_vk.vkDestroyImageView(device, t.view, nullptr);
        if (t.image) g_vk.vkDestroyImage(device, t.image, nullptr);
        freeMem(t.mem);
        t = Tex{};
    }

    // ---------- IGlyphTexHost：字形图集的纹理宿主 ----------
    // 句柄就是 Tex*（unordered_map 的节点地址稳定，图集代不会被搬走）。
    void* createTex(int tw, int th) override {
        Tex* t = new Tex();
        if (!createTexImage(*t, tw, th, false)) {
            freeTex(*t);
            delete t;
            return nullptr;
        }
        glyphTexes.push_back(t);
        return t;
    }

    void updateTex(void* tex, int x, int y, int w, int h, const uint8_t* bgra) override {
        Tex* t = static_cast<Tex*>(tex);
        if (t) uploadRegion(*t, x, y, w, h, bgra, w * 4);
    }

    void destroyTex(void* tex) override {
        Tex* t = static_cast<Tex*>(tex);
        if (!t) return;
        freeTex(*t);
        glyphTexes.erase(std::remove(glyphTexes.begin(), glyphTexes.end(), t), glyphTexes.end());
        delete t;
    }

    // 记录一次布局转换（cmd 必须在录制中）
    void setTexLayout(Tex& t, VkImageLayout dst) {
        if (t.layout == dst || !cmdOpen || !t.image) return;
        VkAccessFlags srcAccess = 0, dstAccess = 0;
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        switch (t.layout) {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
                srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                srcAccess = VK_ACCESS_SHADER_READ_BIT;
                srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                break;
            default:   // UNDEFINED：内容丢弃，无需等待
                break;
        }
        switch (dst) {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
                dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                dstAccess = VK_ACCESS_SHADER_READ_BIT;
                dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                break;
            default:
                break;
        }
        barrier(t.image, t.layout, dst, srcAccess, dstAccess, srcStage, dstStage);
        t.layout = dst;
    }

    // 把一块 BGRA 像素拷进纹理的 (x,y,w,h)：staging → vkCmdCopyBufferToImage
    bool stageCopy(Tex& t, int x, int y, int w, int h, const void* src, int rowBytes) {
        if (!cmdOpen || !t.image || w <= 0 || h <= 0) return false;
        if (rowBytes <= 0) rowBytes = w * 4;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(rowBytes) * h;
        VkBuffer stage = VK_NULL_HANDLE;
        VkDeviceMemory stageMem = VK_NULL_HANDLE;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stage, stageMem)) {
            std::fprintf(stderr, "[vk] stageCopy: staging buffer 失败 (%llu 字节)\n",
                         (unsigned long long)bytes);
            return false;
        }
        void* mapped = nullptr;
        if (g_vk.vkMapMemory(device, stageMem, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] stageCopy: 映射 staging 失败\n");
            g_vk.vkDestroyBuffer(device, stage, nullptr);
            freeMem(stageMem);
            return false;
        }
        std::memcpy(mapped, src, static_cast<size_t>(bytes));
        g_vk.vkUnmapMemory(device, stageMem);

        setTexLayout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {x, y, 0};
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        g_vk.vkCmdCopyBufferToImage(cmd, stage, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1, &region);
        // staging 的生命期到本槽的 fence 为止（拷贝是异步的，现在还不能释放）
        cs().staging.emplace_back(stage, stageMem);
        return true;
    }

    // 把纹理清成全透明。
    //
    // 注意：这里**不能**用 vkCmdClearColorImage —— 该命令不允许在 render pass 内调用，
    // 而文本的第一次光栅化就发生在帧内（遍历到一个新字形时）。实测症状：只要画面里
    // 有文字，整帧变成纯底色（NVIDIA 直接丢掉这条命令缓冲）。
    // 改为用一块全零的 staging 上传：一次性代价 4MB/图集，换来"帧内建纹理"这条简单路径。
    bool zeroTex(Tex& t) {
        if (!cmdOpen || !t.image) return false;
        const size_t bytes = static_cast<size_t>(t.w) * t.h * 4;
        static std::vector<uint8_t> zeros;   // 零页复用，避免每次 4MB 分配
        if (zeros.size() < bytes) zeros.assign(bytes, 0);
        return stageCopy(t, 0, 0, t.w, t.h, zeros.data(), t.w * 4);
    }

    bool stageUpload(Tex& t, int x, int y, int w, int h, const void* src, int rowBytes) {
        if (t.needClear) {
            t.needClear = false;
            // 整张覆盖的上传可以免掉清零
            if (!(x == 0 && y == 0 && w == t.w && h == t.h && rowBytes == t.w * 4))
                if (!zeroTex(t)) return false;
        }
        return stageCopy(t, x, y, w, h, src, rowBytes);
    }

    // 上传入口：帧内直接录，帧外排队（下一个 beginFrame 里补）
    bool uploadRegion(Tex& t, int x, int y, int w, int h, const void* src, int rowBytes) {
        if (!t.image || w <= 0 || h <= 0 || !src) return false;
        if (rowBytes <= 0) rowBytes = w * 4;
        if (cmdOpen) return stageUpload(t, x, y, w, h, src, rowBytes);
        PendingUpload pu;
        pu.tex = &t;
        pu.x = x; pu.y = y; pu.w = w; pu.h = h;
        pu.rowBytes = rowBytes;
        const auto* p = static_cast<const uint8_t*>(src);
        pu.data.assign(p, p + static_cast<size_t>(rowBytes) * h);
        pendingUploads.push_back(std::move(pu));
        return true;
    }

    void drainPendingUploads() {
        if (pendingUploads.empty() || !cmdOpen) return;
        std::vector<PendingUpload> q;
        q.swap(pendingUploads);
        for (const PendingUpload& pu : q)
            if (pu.tex) stageUpload(*pu.tex, pu.x, pu.y, pu.w, pu.h, pu.data.data(), pu.rowBytes);
    }

    // 一次性提交：初始化期（白纹理）没有帧，用它把像素/清零送上去。
    // 固定借用 0 号槽：这时还没有任何帧在录制（init 里调用），槽是空闲的。
    bool oneShot(const std::function<void()>& fn) {
        const int savedIdx = slotIdx;
        const VkCommandBuffer savedCmd = cmd;
        slotIdx = 0;
        Slot& s = slots[0];
        cmd = s.cmd;
        bool ok = true;
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (g_vk.vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) ok = false;
        if (ok && g_vk.vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) ok = false;
        if (ok) {
            cmdOpen = true;
            fn();
            cmdOpen = false;
            if (g_vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) ok = false;
        }
        if (ok) {
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            if (g_vk.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] oneShot: vkQueueSubmit 失败\n");
                ok = false;
            } else if (g_vk.vkQueueWaitIdle(queue) != VK_SUCCESS) {
                ok = false;
            }
        }
        freeStaging(s);
        cmd = savedCmd;
        slotIdx = savedIdx;
        return ok;
    }

    bool createSamplers() {
        auto make = [this](bool linear, VkSampler& out) {
            VkSamplerCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = 0.0f;
            if (g_vk.vkCreateSampler(device, &si, nullptr, &out) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] createSamplers: vkCreateSampler 失败 (linear=%d)\n",
                             (int)linear);
                return false;
            }
            return true;
        };
        return make(false, samplerNearest) && make(true, samplerLinear);
    }

    bool createWhite() {
        if (!createTexImage(white, 1, 1, false)) return false;
        const uint32_t px = 0xFFFFFFFFu;   // 预乘白：纯色四边形 × 1.0
        return oneShot([&] {
            white.needClear = false;       // 整张上传，不用再清零
            stageUpload(white, 0, 0, 1, 1, &px, 4);
        });
    }

    // 圆角模板：与 GL 的 roundFor 同一份解析式覆盖率（像素级同源）
    static RoundTex& emptyRound() {
        static RoundTex e;
        return e;
    }

    // 建模板：现在可以在帧内直接建（上传走命令缓冲）
    void buildRound(int radius, bool stroke) {
        const int r = std::max(1, radius);
        const int key = stroke ? -r : r;
        const int t = RoundTemplateSize(r);
        std::vector<uint32_t> px(static_cast<size_t>(t) * t);
        const float half = static_cast<float>(t) / 2.0f;
        const float inset = half - static_cast<float>(r);
        for (int i = 0; i < t; i++) {
            for (int j = 0; j < t; j++) {
                const float fx = std::fabs(static_cast<float>(j) + 0.5f - half) - inset;
                const float fy = std::fabs(static_cast<float>(i) + 0.5f - half) - inset;
                const float dx = fx > 0 ? fx : 0.0f;
                const float dy = fy > 0 ? fy : 0.0f;
                const float dist = std::sqrt(dx * dx + dy * dy) - static_cast<float>(r);
                float cov = stroke ? std::min(0.5f - dist, dist + 1.5f) : 0.5f - dist;
                cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
                const uint32_t lum = static_cast<uint32_t>(cov * 255.0f + 0.5f);
                px[static_cast<size_t>(i) * t + j] = (lum << 24) | (lum << 16) | (lum << 8) | lum;
            }
        }
        auto old = rounds.find(key);
        if (old != rounds.end()) {
            freeTex(old->second.tex);
            rounds.erase(old);
        }
        RoundTex& rt = rounds[key];
        rt.t = t;
        if (!createTexImage(rt.tex, t, t, false)) {
            rounds.erase(key);
            return;
        }
        rt.tex.needClear = false;   // 整张覆盖
        if (!uploadRegion(rt.tex, 0, 0, t, t, px.data(), t * 4)) {
            freeTex(rt.tex);
            rounds.erase(key);
        }
    }

    RoundTex& roundFor(int radius, bool stroke) {
        const int r = std::max(1, radius);
        const int key = stroke ? -r : r;
        auto it = rounds.find(key);
        if (it != rounds.end() && it->second.tex.view) return it->second;
        buildRound(r, stroke);
        it = rounds.find(key);
        return it != rounds.end() ? it->second : emptyRound();
    }

    // ============================================================
    // 管线
    // ============================================================
    bool createPipeline() {
        // descriptor set layout：set0 binding0 = combined image sampler
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = 1;
        dli.pBindings = &b;
        VK_CHECK(g_vk.vkCreateDescriptorSetLayout(device, &dli, nullptr, &dsLayout),
                 "vkCreateDescriptorSetLayout");

        VkDescriptorPoolSize psz{};
        psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        psz.descriptorCount = 256;
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // 纹理销毁时单独回收它那一套 set（图集换代 / 换图都走这里），
        // 否则池子会被"每换一代图集就泄漏一套"慢慢吃穿
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 256;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &psz;
        VK_CHECK(g_vk.vkCreateDescriptorPool(device, &pi, nullptr, &dsPool),
                 "vkCreateDescriptorPool");
        dsPools.push_back(dsPool);

        // push constant：vec2 viewSize
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.size = sizeof(float) * 2;
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &dsLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VK_CHECK(g_vk.vkCreatePipelineLayout(device, &pli, nullptr, &pipeLayout),
                 "vkCreatePipelineLayout");

        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = sizeof(kVkQuadVertSpv);
        smi.pCode = kVkQuadVertSpv;
        VK_CHECK(g_vk.vkCreateShaderModule(device, &smi, nullptr, &vs), "vkCreateShaderModule(vs)");
        smi.codeSize = sizeof(kVkQuadFragSpv);
        smi.pCode = kVkQuadFragSpv;
        VK_CHECK(g_vk.vkCreateShaderModule(device, &smi, nullptr, &fs), "vkCreateShaderModule(fs)");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkVertexInputBindingDescription bind{};
        bind.binding = 0;
        bind.stride = sizeof(VkVertex);
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attr[3]{};
        attr[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VkVertex, x)};
        // 颜色：BGRA 解释（0xAARRGGBB 的内存序）→ 着色器拿到 true (R,G,B,A)
        attr[1] = {1, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(VkVertex, color)};
        attr[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VkVertex, u)};
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3;
        vin.pVertexAttributeDescriptions = attr;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = samples;
        ms.minSampleShading = 1.0f;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;              // 预乘 alpha
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{};
        dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dy;
        gp.layout = pipeLayout;
        gp.renderPass = renderPass;
        gp.subpass = 0;
        const VkResult pr = g_vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp,
                                                          nullptr, &pipeline);
        g_vk.vkDestroyShaderModule(device, vs, nullptr);
        g_vk.vkDestroyShaderModule(device, fs, nullptr);
        if (pr != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] vkCreateGraphicsPipelines 失败: %s (%d)\n",
                         VkResultText(pr), (int)pr);
            return false;
        }
        return true;
    }

    // ============================================================
    // 绘制
    // ============================================================
    // 顶点缓冲按槽建（懒建：off-screen 只画一帧时也只建一个槽的）
    void ensureVertBuf(Slot& s) {
        if (s.vbuf) return;
        if (!createBuffer(kMaxVertices * sizeof(VkVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          s.vbuf, s.vmem)) {
            std::fprintf(stderr, "[vk] ensureVertBuf: 顶点缓冲创建失败\n");
            return;
        }
        void* mapped = nullptr;
        if (g_vk.vkMapMemory(device, s.vmem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
            s.vmap = static_cast<VkVertex*>(mapped);
        verts.resize(kMaxVertices);
        batches.reserve(kMaxBatches);
    }

    // 顶点写入口：返回 nullptr = 缓冲已满（这一批图元直接丢弃，
    // 与旧的"超过 kMaxVertices 就别再 push"语义一致）。
    VkVertex* vertRoom(size_t n) {
        if (vcount + n > verts.size()) return nullptr;
        return verts.data() + vcount;
    }

    void switchBatch(Tex* tex) {
        if (cur.tex == tex && cur.sx == clipL && cur.sy == clipT && cur.sw == clipR - clipL &&
            cur.sh == clipB - clipT)
            return;
        if (cur.count > 0 && batches.size() < static_cast<size_t>(kMaxBatches)) {
            // ★ 不要在这里覆盖 scissor：批次的 scissor 是**开始那一刻**的裁剪区，
            // 覆盖成"关闭那一刻"的会让先前 emitted 的顶点跑到别的裁剪区里
            // （实测症状：整页底色矩形被裁成一小块，四周露白）。
            batches.push_back(cur);
        }
        cur = Batch{};
        cur.first = static_cast<uint32_t>(vcount);
        cur.tex = tex;
        cur.sx = clipL; cur.sy = clipT; cur.sw = clipR - clipL; cur.sh = clipB - clipT;
    }

    void emitQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  uint32_t color, Tex* tex = nullptr) {
        VkVertex* o = vertRoom(6);
        if (!o) return;
        switchBatch(tex);
        o[0] = VkVertex{x0, y0, color, u0, v0};
        o[1] = VkVertex{x1, y0, color, u1, v0};
        o[2] = VkVertex{x0, y1, color, u0, v1};
        o[3] = o[2];
        o[4] = o[1];
        o[5] = VkVertex{x1, y1, color, u1, v1};
        vcount += 6;
        cur.count += 2;
    }

    void flush() {
        // 每帧一次也要提成 static：UCRT 的 getenv 要遍历环境表，~0.5 µs/次，
        // 和本文件 fillRect 里同一个坑（§7.3）。
        static const bool trace = std::getenv("BINUI_VK_TRACE") != nullptr;
        if (trace) {
            std::fprintf(stderr, "[vk][trace] flush: verts=%zu batches=%zu cur.count=%u\n",
                         vcount, batches.size(), cur.count);
        }
        if (batches.empty()) {
            if (cur.count > 0) batches.push_back(cur);
            cur = Batch{};
        } else if (cur.count > 0) {
            batches.push_back(cur);
            cur = Batch{};
        }
        // 顶点攒在 CPU 缓冲里（见 verts 的注释），这里整块拷进映射的槽缓冲。
        if (vcount == 0 || !vmap) return;
        std::memcpy(vmap, verts.data(), vcount * sizeof(VkVertex));

        g_vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDeviceSize off = 0;
        g_vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
        for (const Batch& b : batches) {
            if (b.count == 0) continue;
            VkRect2D sc{};
            sc.offset = {static_cast<int32_t>(std::max(0, b.sx)),
                         static_cast<int32_t>(std::max(0, b.sy))};
            sc.extent = {static_cast<uint32_t>(std::max(0, std::min(b.sw, w - b.sx))),
                         static_cast<uint32_t>(std::max(0, std::min(b.sh, h - b.sy)))};
            if (sc.extent.width == 0 || sc.extent.height == 0) continue;
            Tex* tex = b.tex ? b.tex : &white;
            // 上传留下的 TRANSFER_DST 必须在这里转成只读：绘制是 flush 时才录进
            // 命令缓冲的，所以转换点只能是 draw 之前（不是 emit 那一刻）。
            setTexLayout(*tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_vk.vkCmdSetScissor(cmd, 0, 1, &sc);
            g_vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1,
                                         &tex->ds, 0, nullptr);
            g_vk.vkCmdDraw(cmd, b.count * 3, 1, b.first, 0);   // count 是三角形数，first 是顶点索引
        }
        batches.clear();
        frameVerts = vcount;
        vcount = 0;
    }
};

// ============================================================
// 对外接口
// ============================================================
BinUIVK::BinUIVK() : impl_(new Impl()) {}
BinUIVK::~BinUIVK() {
    shutdown();
    delete impl_;
}

bool BinUIVK::init(void* hwnd, int width, int height, bool vsync) {
    Impl& d = *impl_;
    if (width <= 0 || height <= 0) return false;
    d.w = width;
    d.h = height;
    d.vsync = vsync;
    d.hwnd = static_cast<HWND>(hwnd);
    d.windowMode = (d.hwnd != nullptr);

    if (!g_vk.load()) return false;

    // ---- 实例 ----
    // 离屏不需要任何扩展；窗口模式要 surface + win32 surface 这两条实例扩展。
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "binUI";
    app.applicationVersion = VK_MAKE_VERSION(0, 11, 4);
    app.pEngineName = "binUI";
    app.engineVersion = VK_MAKE_VERSION(0, 11, 4);
    app.apiVersion = VK_API_VERSION_1_0;
    const char* instExt[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (d.windowMode) {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = instExt;
    }

    // ---- 校验层（可选）----
    // BINUI_VK_LAYERS=1      → 打印本机可用的实例层
    // BINUI_VK_VALIDATION=1  → 若装了 VK_LAYER_KHRONOS_validation 就启用
    // 这台机器没有 Vulkan SDK（没有层 json 注册），所以默认这条路径是空操作；
    // P4 的"0 validation error"因此只能靠压力测试 + 静态复查兜底，见 docs §6。
    const char* layerName = nullptr;
    if (g_vk.vkEnumerateInstanceLayerProperties) {
        uint32_t ln = 0;
        g_vk.vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> ls(ln);
        if (ln) g_vk.vkEnumerateInstanceLayerProperties(&ln, ls.data());
        const bool listLayers = std::getenv("BINUI_VK_LAYERS") != nullptr;
        if (listLayers) std::fprintf(stderr, "[vk] 可用实例层 %u 个\n", ln);
        for (uint32_t i = 0; i < ln; i++) {
            if (listLayers) std::fprintf(stderr, "[vk]   层: %s\n", ls[i].layerName);
            if (std::strcmp(ls[i].layerName, "VK_LAYER_KHRONOS_validation") == 0 &&
                std::getenv("BINUI_VK_VALIDATION"))
                layerName = "VK_LAYER_KHRONOS_validation";
        }
        if (std::getenv("BINUI_VK_VALIDATION") && !layerName)
            std::fprintf(stderr, "[vk] 要求校验层但本机没装（无 Vulkan SDK）\n");
    }
    if (layerName) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layerName;
        std::fprintf(stderr, "[vk] 校验层已启用: %s\n", layerName);
    }
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_vk.getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) {
        std::fprintf(stderr, "[vk] 取不到 vkCreateInstance\n");
        return false;
    }
    VK_CHECK(createInstance(&ici, nullptr, &d.instance), "vkCreateInstance");
    if (!g_vk.loadInstanceFns(d.instance)) return false;

    // ---- surface（窗口模式）----
    if (d.windowMode) {
        if (!g_vk.loadSurfaceFns(d.instance)) return false;
        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = d.hwnd;
        const VkResult sr = g_vk.vkCreateWin32SurfaceKHR(d.instance, &sci, nullptr, &d.surface);
        if (sr != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] vkCreateWin32SurfaceKHR 失败: %s (%d)\n",
                         VkResultText(sr), (int)sr);
            d.surface = VK_NULL_HANDLE;
            return false;
        }
    }

    if (!d.pickPhysical()) return false;
    if (!d.createDevice()) return false;

    // 交换链函数（窗口模式）：取不到就不起窗口失败，而是降级为"读回 + GDI 贴屏"
    if (d.windowMode && g_vk.loadSwapchainFns(d.instance)) {
        if (d.createSwapchain()) {
            d.useSwapchain = true;
        } else {
            std::fprintf(stderr, "[vk] 交换链创建失败，退回读回呈现（性能会差很多）\n");
        }
    }
    if (d.windowMode && !d.useSwapchain)
        std::fprintf(stderr, "[vk] 窗口模式：present 走读回 + GDI 贴屏\n");

    // 命令池 + 双槽（命令缓冲 / fence / 信号量）
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = d.queueFamily;
    VK_CHECK(g_vk.vkCreateCommandPool(d.device, &cpi, nullptr, &d.cmdPool), "vkCreateCommandPool");
    for (int i = 0; i < Impl::kSlots; i++) {
        if (!d.initSlot(d.slots[i])) {
            std::fprintf(stderr, "[vk] 命令槽 %d 初始化失败\n", i);
            return false;
        }
    }
    d.cmd = d.slots[0].cmd;   // 离屏/单帧模式下 oneShot 与 beginFrame 都从 0 号槽开始

    d.clipL = 0; d.clipT = 0; d.clipR = width; d.clipB = height;
    if (!d.createTargets()) return false;
    if (!d.createPipeline()) return false;
    if (!d.createSamplers()) return false;

    // 建白纹理（一次性提交，此时还没有帧在录制）
    if (!d.createWhite()) { std::fprintf(stderr, "[vk] createWhite 返回 false\n"); return false; }

    // 字形图集：排版/光栅化在共享头里，本类自己当纹理宿主
    d.atlas.setHost(&d);
    d.atlas.initFonts();

    d.ready = true;
    d.coversMonitor = d.windowMode ? d.clientCoversMonitor() : false;
    std::fprintf(stderr, "[vk] %s就绪 %dx%d（MSAA %dx%s）\n", d.windowMode ? "窗口" : "离屏",
                 width, height, static_cast<int>(d.samples),
                 d.windowMode ? (d.useSwapchain ? "，交换链呈现" : "，读回呈现") : "");
    return true;
}

void BinUIVK::shutdown() {
    Impl& d = *impl_;
    if (!d.device) return;
    g_vk.vkDeviceWaitIdle(d.device);
    if (d.dib) { DeleteObject(d.dib); d.dib = nullptr; }
    if (d.dibDC) { DeleteDC(d.dibDC); d.dibDC = nullptr; }
    d.dibBits = nullptr;
    d.dibW = d.dibH = 0;
    d.destroySwapchain();
    if (d.surface) g_vk.vkDestroySurfaceKHR(d.instance, d.surface, nullptr);
    d.surface = VK_NULL_HANDLE;
    d.useSwapchain = false;
    d.windowMode = false;
    d.hwnd = nullptr;
    // 图集纹理要先释放（自己 new 出来的 Tex）
    d.atlas.clear();
    d.atlas.setHost(nullptr);
    for (Impl::Tex* t : d.glyphTexes) {
        if (t) { d.freeTex(*t); delete t; }
    }
    d.glyphTexes.clear();
    for (auto& kv : d.rounds) d.freeTex(kv.second.tex);
    d.rounds.clear();
    for (auto& kv : d.images) d.freeTex(kv.second);
    d.images.clear();
    d.imageGen.clear();
    d.pendingUploads.clear();
    d.freeTex(d.white);
    for (VkDescriptorPool p : d.dsPools)
        if (p) g_vk.vkDestroyDescriptorPool(d.device, p, nullptr);
    d.dsPools.clear();
    d.dsPool = VK_NULL_HANDLE;
    if (d.samplerNearest) g_vk.vkDestroySampler(d.device, d.samplerNearest, nullptr);
    if (d.samplerLinear) g_vk.vkDestroySampler(d.device, d.samplerLinear, nullptr);
    d.samplerNearest = d.samplerLinear = VK_NULL_HANDLE;
    if (d.pipeline) g_vk.vkDestroyPipeline(d.device, d.pipeline, nullptr);
    if (d.pipeLayout) g_vk.vkDestroyPipelineLayout(d.device, d.pipeLayout, nullptr);
    if (d.dsLayout) g_vk.vkDestroyDescriptorSetLayout(d.device, d.dsLayout, nullptr);
    // 双槽：每槽自己一套顶点缓冲 / fence / 信号量
    for (int i = 0; i < Impl::kSlots; i++) {
        Impl::Slot& s = d.slots[i];
        if (s.vbuf) g_vk.vkDestroyBuffer(d.device, s.vbuf, nullptr);
        // 先解映射再释放：freeMem 会把句柄置空，之后 vkUnmapMemory 拿到空句柄就直接崩
        //（P4 压力测试第一次跑就抓到 —— 以前只是"顺序不对但侥幸没崩"）
        if (s.vmem) g_vk.vkUnmapMemory(d.device, s.vmem);
        d.freeMem(s.vmem);
        d.freeStaging(s);
        if (s.fence) g_vk.vkDestroyFence(d.device, s.fence, nullptr);
        if (s.semAvail) g_vk.vkDestroySemaphore(d.device, s.semAvail, nullptr);
        if (s.semDone) g_vk.vkDestroySemaphore(d.device, s.semDone, nullptr);
        s = Impl::Slot{};   // 命令缓冲由命令池统一释放
    }
    d.cmd = VK_NULL_HANDLE;
    d.vbuf = VK_NULL_HANDLE;
    d.vmap = nullptr;
    d.vmem = VK_NULL_HANDLE;
    d.slotIdx = 0;
    d.prevSlot = -1;
    d.frameNo = 0;
    d.destroyTargets();
    if (d.cmdPool) g_vk.vkDestroyCommandPool(d.device, d.cmdPool, nullptr);
    g_vk.vkDestroyDevice(d.device, nullptr);
    if (d.instance) g_vk.vkDestroyInstance(d.instance, nullptr);
    d.device = VK_NULL_HANDLE;
    d.instance = VK_NULL_HANDLE;
    d.ready = false;
}

bool BinUIVK::isReady() const { return impl_->ready; }
int  BinUIVK::width() const { return impl_->w; }
int  BinUIVK::height() const { return impl_->h; }

void BinUIVK::resize(int width, int height) {
    Impl& d = *impl_;
    if (width <= 0 || height <= 0 || (width == d.w && height == d.h)) return;
    g_vk.vkDeviceWaitIdle(d.device);
    d.w = width;
    d.h = height;
    // 先重建交换链：readback buffer 要按"渲染目标与交换链的较大者"分配，
    // 反过来（先建目标后建交换链）会让 DPI 缩放时缓冲区偏小（P4 修的隐患）。
    if (d.windowMode && d.useSwapchain) {
        if (!d.createSwapchain()) std::fprintf(stderr, "[vk] resize: 交换链重建失败\n");
    }
    if (!d.createTargets()) {
        d.ready = false;
        std::fprintf(stderr, "[vk] resize 后重建目标失败\n");
        return;
    }
    d.coversMonitor = d.windowMode ? d.clientCoversMonitor() : false;
}

void BinUIVK::setVsync(bool on) {
    Impl& d = *impl_;
    if (d.vsync == on) return;
    d.vsync = on;
    // present mode 在交换链里定死：改 vsync 要重建
    if (d.windowMode && d.useSwapchain) d.recreateSwapchain();
}

void BinUIVK::setTransparent(bool t) {
    // 透明窗口**不走交换链**（Windows 上只有约 1/3 设备支持 PRE_MULTIPLIED），
    // 走读回 + UpdateLayeredWindow，与 GL 后端同一套做法（设计 §5.6）
    impl_->transparent = t;
}

void BinUIVK::setCaptureReadback(bool on) { impl_->capture = on; }

bool BinUIVK::swapchainActive() const { return impl_->useSwapchain && !impl_->lastPresentWasGdi; }

bool BinUIVK::presentUsedGdi() const { return impl_->lastPresentWasGdi; }

size_t BinUIVK::deviceMemoryBytes() const { return impl_->memBytes; }

void BinUIVK::setFontName(const char* name) { impl_->atlas.setFontName(name); }

void BinUIVK::beginFrame(const void* clipRect) {
    Impl& d = *impl_;
    if (!d.ready) return;
    int l = 0, t = 0, r = d.w, b = d.h;
    if (clipRect) {
        const int* c = static_cast<const int*>(clipRect);
        if (c[2] > c[0] && c[3] > c[1] && c[0] >= 0 && c[1] >= 0 && c[2] <= d.w && c[3] <= d.h) {
            l = c[0]; t = c[1]; r = c[2]; b = c[3];
        }
    }
    d.clipL = l; d.clipT = t; d.clipR = r; d.clipB = b;
    d.scissorDepth = 0;

    // ---- 换到本帧的槽 ----
    // 这一槽上次提交是**两帧前**，present() 的串行化点已经等过一帧，这里通常立即返回；
    // 只有丢帧路径（submitNoPresent 已同步）或第一帧会真等。等完后 fence 一定是置位的，
    // 于是可以安全地复位给本帧的提交用。
    d.slotIdx = static_cast<int>(d.frameNo++ % Impl::kSlots);
    Impl::Slot& slot = d.slots[d.slotIdx];
    d.waitSlot(slot);
    g_vk.vkResetFences(d.device, 1, &slot.fence);
    d.ensureVertBuf(slot);
    d.cmd = slot.cmd;          // 当前槽别名：绘制/上传代码只认 d.cmd / d.vbuf / d.vmap
    d.vbuf = slot.vbuf;
    d.vmem = slot.vmem;
    d.vmap = slot.vmap;
    d.perfBeginFrame();
    g_vk.vkResetCommandBuffer(slot.cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    g_vk.vkBeginCommandBuffer(slot.cmd, &bi);
    d.recording = true;
    d.cmdOpen = true;
    // 帧外攒下的上传（排版阶段光栅化的字形、prepareImage）在这里补录：
    // 放在 render pass 之前，于是"先上传后绘制"在同一帧内天然成立，不需要预热帧。
    d.drainPendingUploads();

    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = d.renderPass;
    rpb.framebuffer = d.framebuffer;
    rpb.renderArea.offset = {0, 0};
    rpb.renderArea.extent = {static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h)};
    rpb.clearValueCount = 0;
    g_vk.vkCmdBeginRenderPass(d.cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width = static_cast<float>(d.w);
    vp.height = static_cast<float>(d.h);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    g_vk.vkCmdSetViewport(d.cmd, 0, 1, &vp);
    float vs[2] = {static_cast<float>(d.w), static_cast<float>(d.h)};
    if (d.pipeLayout) g_vk.vkCmdPushConstants(d.cmd, d.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, vs);

    // 脏矩形清成底色（与 GL 的 beginFrame 同语义：不透明画布清白）
    VkClearAttachment ca{};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    ca.clearValue.color.float32[0] = 1.0f;
    ca.clearValue.color.float32[1] = 1.0f;
    ca.clearValue.color.float32[2] = 1.0f;
    ca.clearValue.color.float32[3] = 1.0f;
    VkClearRect cr{};
    cr.rect.offset = {l, t};
    cr.rect.extent = {static_cast<uint32_t>(r - l), static_cast<uint32_t>(b - t)};
    cr.baseArrayLayer = 0;
    cr.layerCount = 1;
    g_vk.vkCmdClearAttachments(d.cmd, 1, &ca, 1, &cr);

    d.ensureVertBuf(d.slots[d.slotIdx]);
    d.vcount = 0;
    d.batches.clear();
    d.cur = Impl::Batch{};
    d.switchBatch(&d.white);
}

// present() 会不会自己阻塞到垂直同步：走交换链 + FIFO 才算。其余路径（读回 + GDI 贴屏、
// 透明窗、强制读回）都不阻塞，外壳必须自己按刷新率节流。
bool BinUIVK::presentPacesFrames() const {
    const Impl& d = *impl_;
    return d.ready && d.vsync && d.useSwapchain && !d.transparent && !d.forceGdi &&
           !RemoteSession();
}

void BinUIVK::present() {
    Impl& d = *impl_;
    if (!d.ready || !d.recording) return;
    const double tPresent = d.perfOn ? VkNowMs() : 0.0;
    Impl::Slot& slot = d.slots[d.slotIdx];
    d.flush();
    g_vk.vkCmdEndRenderPass(d.cmd);

    // ---- 呈现前的 surface 复查（P4：DPI 变化 / 跨显示器移动 / 最小化）----
    // caps.currentExtent 由 surface 决定，客户区尺寸只是我们的"期望"；两者不一致说明
    // 窗口刚换过显示器/DPI 或用户改过尺寸，交换链必须先重建才能继续。每帧查一次
    // （vkGetPhysicalDeviceSurfaceCapabilitiesKHR 是纯 CPU 侧查询，代价可忽略）。
    if (d.windowMode && d.useSwapchain) {
        VkSurfaceCapabilitiesKHR caps{};
        if (g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, d.surface, &caps) ==
            VK_SUCCESS) {
            const VkExtent2D ce = caps.currentExtent;
            if (ce.width == 0 || ce.height == 0) {
                // 最小化：没有可呈现的像素，本帧丢掉但不报错（恢复时会重建）
                d.submitNoPresent();
                d.perfEndFrame(tPresent);
                return;
            }
            if (ce.width != 0xFFFFFFFFu &&
                (ce.width != d.swapExtent.width || ce.height != d.swapExtent.height)) {
                if (!d.recreateSwapchain()) d.useSwapchain = false;
            }
        }
    }

    // 呈现路线（设计 §5.5）：
    //   A. 交换链可用且不需要绕开直翻转 → resolve → blit 到交换链图像 → vkQueuePresentKHR
    //   B. 其余情况（透明窗口 / 客户区铺满显示器 / 远程会话 / 上层要求拖拽期走 GDI /
    //      无交换链）→ resolve → 读回 buffer → SetDIBitsToDevice 或 UpdateLayeredWindow
    // 两条路线用的是**同一张 resolve 图**，所以窗口模式与离屏模式的像素完全一致。
    // 远程会话（RDP）走 B 是实测结论：虚拟显示器上 A 会停帧（GL 0.9.1 同样处理）。
    const bool gdiPath = !d.useSwapchain || d.transparent || d.forceGdi ||
                         (d.windowMode && d.coversMonitor) || RemoteSession();
    d.lastPresentWasGdi = gdiPath;

    uint32_t imageIndex = 0;
    VkImage swapImg = VK_NULL_HANDLE;
    if (!gdiPath) {
        const VkResult ar = g_vk.vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX,
                                                       slot.semAvail, VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            d.recreateSwapchain();
            d.submitNoPresent();   // 本帧仍然提交掉，下帧用新交换链
            d.perfEndFrame(tPresent);
            return;
        }
        if (ar != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] vkAcquireNextImageKHR 失败: %s (%d)\n", VkResultText(ar),
                         (int)ar);
            if (ar == VK_ERROR_DEVICE_LOST) d.ready = false;   // 设备丢了就别再画
            d.useSwapchain = false;
            d.submitNoPresent();
            d.perfEndFrame(tPresent);
            return;
        }
        swapImg = d.swapImages[imageIndex];
    }

    if (gdiPath) {
        // resolve 目标此刻是 TRANSFER_SRC_OPTIMAL（render pass finalLayout），直接拷到 buffer
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h), 1};
        d.capW = d.w;
        d.capH = d.h;
        g_vk.vkCmdCopyImageToBuffer(d.cmd, d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    d.readback, 1, &region);
    } else {
        // 交换链图像：UNDEFINED → TRANSFER_DST，blit 上去，再转 PRESENT_SRC
        d.barrier(swapImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {d.w, d.h, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(d.swapExtent.width),
                              static_cast<int32_t>(d.swapExtent.height), 1};
        g_vk.vkCmdBlitImage(d.cmd, d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                            VK_FILTER_NEAREST);
        VkImageLayout after = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (d.capture) {
            // 诊断/验收：把**交换链图像**读回（证明真的画进了交换链，而不只是 resolve 图）
            d.barrier(swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {static_cast<uint32_t>(d.swapExtent.width),
                                  static_cast<uint32_t>(d.swapExtent.height), 1};
            d.capW = static_cast<int>(d.swapExtent.width);
            d.capH = static_cast<int>(d.swapExtent.height);
            g_vk.vkCmdCopyImageToBuffer(d.cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        d.readback, 1, &region);
            d.barrier(swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, after,
                      VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else {
            d.barrier(swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, after,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
    }
    // 拷完把 resolve 图转回 render pass 的写入布局（下一帧 subpass 会再 resolve 进去）
    d.barrier(d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_vk.vkEndCommandBuffer(d.cmd);
    d.recording = false;
    d.cmdOpen = false;

    // ---- 串行化点 ----
    // 提交新一帧之前必须等上一帧的 fence：渲染目标（MSAA 累积图 / resolve 图）和纹理布局
    // 是帧间共享的，GPU 上不能同时有两帧在跑。**但录制已经并行了** —— beginFrame 到
    // 这里之间的 CPU 工作（遍历、排版、路径细分、顶点填充）都发生在上一帧还在 GPU 上跑的时候，
    // 这正是双槽换来的收益。放在提交前而不是 beginFrame，是为了把重叠窗口拉到最大。
    if (d.prevSlot >= 0 && d.prevSlot != d.slotIdx) d.waitSlot(d.slots[d.prevSlot]);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore sig = VK_NULL_HANDLE;
    if (!gdiPath) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &slot.semAvail;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &slot.semDone;
        sig = slot.semDone;
    }
    if (g_vk.vkQueueSubmit(d.queue, 1, &si, slot.fence) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] present: vkQueueSubmit 失败，后端转为未就绪\n");
        d.ready = false;
        d.perfEndFrame(tPresent);
        return;
    }
    slot.submitted = true;
    d.prevSlot = d.slotIdx;

    // 像素要拿回 CPU 的两条路必须同步等：GDI 贴屏（接下来就要读这块内存）和
    // capture 读回（工具在帧循环结束后立刻取像素）。交换链那条路把等待留给下一帧 ——
    // 这就是流水线的全部意义。
    if (gdiPath || d.capture) d.waitSlot(slot);

    if (gdiPath) {
        d.blitToWindow(static_cast<const uint8_t*>(d.readbackMapped), d.w, d.h);
        d.perfEndFrame(tPresent);
        return;
    }
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = sig ? 1 : 0;
    pi.pWaitSemaphores = &sig;
    pi.swapchainCount = 1;
    pi.pSwapchains = &d.swapchain;
    pi.pImageIndices = &imageIndex;
    const VkResult pr = g_vk.vkQueuePresentKHR(d.queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        d.recreateSwapchain();
    } else if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vkQueuePresentKHR 失败: %s (%d)\n", VkResultText(pr), (int)pr);
        if (pr == VK_ERROR_DEVICE_LOST) d.ready = false;
        d.useSwapchain = false;
    }
    d.perfEndFrame(tPresent);
}

// 强制读回 + GDI 贴屏（拖拽 resize 期由上层调用；全屏直翻转守卫由 present() 内部处理）
void BinUIVK::presentViaGdi() {
    Impl& d = *impl_;
    if (!d.ready) return;
    d.forceGdi = true;
    present();
    d.forceGdi = false;
}


const uint8_t* BinUIVK::readbackPixels(int& outW, int& outH) {
    Impl& d = *impl_;
    if (!d.ready || !d.readbackMapped) return nullptr;
    outW = d.capW > 0 ? d.capW : d.w;
    outH = d.capH > 0 ? d.capH : d.h;
    // GPU 侧已是自上而下写入（region 的 offset=0 对应图像第 0 行 = 顶部）
    return static_cast<const uint8_t*>(d.readbackMapped);
}

// ---------- 原语 ----------
void BinUIVK::fillRect(float x, float y, float w, float h, uint32_t color) {
    Impl& d = *impl_;
    // ★ 环境变量只查一次：fillRect 是每帧被调用几千次的热路径，UCRT 的 getenv 每次都要
    // 走一遍环境表 + 加锁，实测在 2100 条带/帧的页面上能耗掉 0.5 ms（P6 量测时抓到）。
    static const bool trace = std::getenv("BINUI_VK_TRACE") != nullptr;
    if (trace) {
        static int n = 0;
        if (n < 4) std::fprintf(stderr, "[vk][trace] fillRect #%d (%.0f,%.0f %.0fx%.0f) color=%08X\n",
                                n++, (double)x, (double)y, (double)w, (double)h, color);
    }
    if (!d.ready || w <= 0 || h <= 0) return;
    uint32_t c = ScaleAlphaU32(color, 1.0f);
    Premult(c);
    d.emitQuad(x, y, x + w, y + h, 0.5f, 0.5f, 0.5f, 0.5f, c, &d.white);
}

void BinUIVK::fillRounded(float x, float y, float w, float h, float radius, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    int r = static_cast<int>(radius);
    r = std::max(0, std::min(r, static_cast<int>(std::min(w, h) / 2)));
    if (r <= 0) { fillRect(x, y, w, h, color); return; }
    Impl::RoundTex& rt = d.roundFor(r, false);
    if (!rt.tex.image) { fillRect(x, y, w, h, color); return; }
    PaintQuad q[9];
    const int n = BuildRoundedNineSlice(x, y, w, h, static_cast<float>(r),
                                       static_cast<float>(rt.t), q, 9);
    if (n <= 0) { fillRect(x, y, w, h, color); return; }
    Premult(color);
    for (int i = 0; i < n; i++)
        d.emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color, &rt.tex);
}

void BinUIVK::fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || width <= 0) return;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float nx = -dy / len * width * 0.5f;
    const float ny = dx / len * width * 0.5f;
    uint32_t c = color;
    Premult(c);
    // 旋转四边形：两个三角形，法向偏移半线宽
    d.switchBatch(&d.white);
    VkVertex* o = d.vertRoom(6);
    if (!o) return;
    o[0] = VkVertex{x0 + nx, y0 + ny, c, 0.5f, 0.5f};
    o[1] = VkVertex{x1 + nx, y1 + ny, c, 0.5f, 0.5f};
    o[2] = VkVertex{x1 - nx, y1 - ny, c, 0.5f, 0.5f};
    o[3] = o[0];
    o[4] = o[2];
    o[5] = VkVertex{x0 - nx, y0 - ny, c, 0.5f, 0.5f};
    d.vcount += 6;
    d.cur.count += 2;
}

void BinUIVK::strokeRounded(float x, float y, float w, float h, float radius,
                            float thickness, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0 || thickness <= 0) return;
    // P2 先按"四边 + 四角"的九宫格模板画；与 GL 的 strokeRounded 同源（同一套模板与几何）
    int r = static_cast<int>(radius);
    r = std::max(0, std::min(r, static_cast<int>(std::min(w, h) / 2)));
    if (r <= 0) {
        // 直角边框：上/下/左/右四条
        fillRect(x, y, w, thickness, color);
        fillRect(x, y + h - thickness, w, thickness, color);
        fillRect(x, y + thickness, thickness, h - 2 * thickness, color);
        fillRect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
        return;
    }
    Impl::RoundTex& rt = d.roundFor(r, true);
    if (!rt.tex.image) return;
    PaintQuad q[9];
    const int n = BuildRoundedNineSlice(x, y, w, h, static_cast<float>(r),
                                       static_cast<float>(rt.t), q, 9);
    if (n <= 0) return;
    Premult(color);
    for (int i = 0; i < n; i++)
        d.emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color, &rt.tex);
}

// ---------- 文本 ----------
// 排版、字形光栅化、图集代管理全在 binui::GlyphAtlas（与 GL 后端同一份代码）；
// 这里只把字形四边形交给本后端的批次。图集纹理走 IGlyphTexHost 回调（见 Impl）。
void BinUIVK::drawText(float x, float y, float size, int weight, const std::string& utf8,
                       uint32_t color, bool centerH, bool centerV, bool italic) {
    Impl& d = *impl_;
    if (!d.ready || utf8.empty()) return;
    d.atlas.drawText(x, y, size, weight, utf8, color, centerH, centerV, italic,
                     [&d](const GlyphQuad& q) {
                         d.emitQuad(q.x, q.y, q.x + q.w, q.y + q.h, q.u0, q.v0, q.u1, q.v1,
                                    q.color, static_cast<Impl::Tex*>(q.tex));
                     });
}

void BinUIVK::drawTextWrapped(float x, float y, float maxW, float size, int weight,
                              const std::string& utf8, uint32_t color, int align, bool italic) {
    Impl& d = *impl_;
    if (!d.ready || utf8.empty()) return;
    d.atlas.drawTextWrapped(x, y, maxW, size, weight, utf8, color, align, italic,
                            [&d](const GlyphQuad& q) {
                                d.emitQuad(q.x, q.y, q.x + q.w, q.y + q.h, q.u0, q.v0, q.u1, q.v1,
                                           q.color, static_cast<Impl::Tex*>(q.tex));
                            });
}

float BinUIVK::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->ready) return 0;
    return impl_->atlas.textWidth(size, weight, utf8, italic);
}

void BinUIVK::fontMetrics(float size, int weight, int& ascent, int& lineHeight, bool italic) {
    impl_->atlas.fontMetrics(size, weight, ascent, lineHeight, italic);
}

// ---------- 图片 ----------
// generation = "这份资源被换了"的代号：相同就复用，不重复解码/上传。
bool BinUIVK::prepareImage(uint16_t index, const uint8_t* data, size_t size,
                           uint32_t generation) {
    Impl& d = *impl_;
    if (!d.ready) return false;
    auto gi = d.imageGen.find(index);
    auto ti = d.images.find(index);
    if (gi != d.imageGen.end() && gi->second == generation && ti != d.images.end())
        return ti->second.view != VK_NULL_HANDLE;

    int iw = 0, ih = 0;
    std::vector<uint8_t> px = DecodeImagePixels(data, size, iw, ih);
    if (px.empty()) {
        d.imageGen[index] = generation;
        return false;
    }
    if (ti != d.images.end()) d.freeTex(ti->second);   // 换图：旧图像必须放掉
    Impl::Tex& t = d.images[index];
    if (!d.createTexImage(t, iw, ih, true)) {
        d.images.erase(index);
        return false;
    }
    t.needClear = false;   // 整张覆盖，不用先清零
    const bool ok = d.uploadRegion(t, 0, 0, iw, ih, px.data(), iw * 4);
    d.imageGen[index] = generation;
    return ok;
}

// 原始像素直传（RGBA 字节序，没有解码）：BGRA 目标格式要换一下 R/B 通道
bool BinUIVK::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                              uint32_t generation) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0 || !rgba) return false;
    auto gi = d.imageGen.find(index);
    auto ti = d.images.find(index);
    if (gi != d.imageGen.end() && gi->second == generation && ti != d.images.end())
        return ti->second.view != VK_NULL_HANDLE;

    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    if (ti != d.images.end()) d.freeTex(ti->second);
    Impl::Tex& t = d.images[index];
    if (!d.createTexImage(t, w, h, true)) {
        d.images.erase(index);
        return false;
    }
    t.needClear = false;
    const bool ok = d.uploadRegion(t, 0, 0, w, h, bgra.data(), w * 4);
    d.imageGen[index] = generation;
    return ok;
}

void BinUIVK::drawImage(uint16_t index, float x, float y, float w, float h) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    auto it = d.images.find(index);
    if (it == d.images.end() || !it->second.view) return;
    d.emitQuad(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, &it->second);
}

int BinUIVK::pushClip(float x, float y, float w, float h) {
    Impl& d = *impl_;
    const int depth = d.scissorDepth;
    const int l = static_cast<int>(std::floor(x));
    const int t = static_cast<int>(std::floor(y));
    const int r = static_cast<int>(std::ceil(x + w));
    const int b = static_cast<int>(std::ceil(y + h));
    d.clipL = std::max(d.clipL, l);
    d.clipT = std::max(d.clipT, t);
    d.clipR = std::min(d.clipR, r);
    d.clipB = std::min(d.clipB, b);
    if (d.clipR < d.clipL) d.clipR = d.clipL;
    if (d.clipB < d.clipT) d.clipB = d.clipT;
    if (d.scissorDepth < 31) d.scissorStack[d.scissorDepth++] = depth;
    // 裁剪变了：下一批要换 scissor
    d.switchBatch(d.cur.tex);
    return depth;
}

void BinUIVK::popClipTo(int depth) {
    Impl& d = *impl_;
    if (depth < 0) depth = 0;
    d.scissorDepth = depth;
    d.clipL = 0;
    d.clipT = 0;
    d.clipR = d.w;
    d.clipB = d.h;
    d.switchBatch(d.cur.tex);
}

} // namespace binui
