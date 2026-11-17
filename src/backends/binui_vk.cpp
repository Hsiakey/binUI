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
#include "binui_paint.h"
#include "binui_filter.h"      // v26：CSS filter 颜色矩阵（解码后逐像素应用）   // RoundTemplateSize / BuildRoundedNineSlice / PaintQuad
#include "backends/binui_glyph_atlas.h"   // 字形图集 + 排版（与 GL 后端共用）
#include "binui_mip.h"                  // v0.16.20：mip 链的 CPU 2×2 盒式滤波（与 GL/D3D 的硬件生成等价）
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
    // Vulkan 1.3 动态渲染（可选）：拿不到就退回 render pass 路径。
    // 图层必须靠它 —— 见 Impl::useDynamic 的说明。
    BINUI_VK_FN(vkCmdBeginRendering)
    BINUI_VK_FN(vkCmdEndRendering)
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
    BINUI_VK_FN(vkCmdCopyImage)
    BINUI_VK_FN(vkCmdResolveImage)
    BINUI_VK_FN(vkCmdClearColorImage)

    // ---- WSI（只在窗口模式加载；离屏模式下这些指针必须允许为空）----
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
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
    // VK_KHR_present_wait（可选）：等"这一帧真的被合成器显示了"再返回。
    // 这是唯一能精确对齐显示器刷新的手段 —— DWM 只在合成那一刻放掉交换链图，
    // 而 present 本身（FIFO）并不阻塞。拿不到就退回"present + DwmFlush"。
    PFN_vkWaitForPresentKHR vkWaitForPresentKHR = nullptr;
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
        // 可选：动态渲染的特性查询（1.1 起才有；拿不到就不启用动态渲染）
        vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            get("vkGetPhysicalDeviceFeatures2"));
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
        BINUI_VK_LOAD(vkCmdCopyImage)
        BINUI_VK_LOAD(vkCmdResolveImage)
        BINUI_VK_LOAD(vkCmdClearColorImage)
        BINUI_VK_LOAD(vkFreeDescriptorSets)
#undef BINUI_VK_LOAD
        // 可选：present_wait（拿不到就留空，改用 present + DwmFlush）
        vkWaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(get("vkWaitForPresentKHR"));
        // 可选：动态渲染。1.3 是核心名，1.1/1.2 的驱动只有 KHR 后缀名 —— 两个都试一次。
        vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(get("vkCmdBeginRendering"));
        if (!vkCmdBeginRendering)
            vkCmdBeginRendering =
                reinterpret_cast<PFN_vkCmdBeginRendering>(get("vkCmdBeginRenderingKHR"));
        vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(get("vkCmdEndRendering"));
        if (!vkCmdEndRendering)
            vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(get("vkCmdEndRenderingKHR"));
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
// 顶点缓冲：初始 10 万顶点（约 1.66 万四边形，与 GL/D3D11 一致），满了按需扩容，
// 硬上限 100 万顶点（约 17 万四边形）。以前满了就丢图元且不报错（docs/future.md P0-5）。
constexpr size_t kMaxVertices = 100000;
constexpr size_t kMaxVerticesCeiling = 1u << 20;
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
        VkDeviceSize vbufBytes = 0;              // vbuf 的容量（扩容后跟着变大）
        VkVertex* vmap = nullptr;
        std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;   // 等本槽 fence 后释放
        std::vector<std::pair<VkBuffer, VkDeviceMemory>> retiredVb; // 帧内扩容淘汰的顶点缓冲
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
    double perfAcquire = 0.0;    // 窗口内 vkAcquireNextImageKHR 的累计耗时（判断它是否在等合成器）
    double perfPresCall = 0.0;   // 窗口内 vkQueuePresentKHR 的累计耗时
    double perfPresentWait = 0.0; // 窗口内 vkWaitForPresentKHR 的累计耗时
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

    // ---- 动态渲染（Vulkan 1.3，v0.16.12）----
    // 为什么整条渲染路径改走 vkCmdBeginRendering / vkCmdEndRendering：
    // 组透明度的图层必须在**一帧内**切换渲染目标（主目标 → 层目标 → 合成 → 回主目标）。
    // 用 render pass 做这件事要重建 pass + framebuffer，v0.16.11 实测两个坑
    // （见 docs/future.md）：主 pass 重启后 LOAD_OP_LOAD 没保住已有内容、层里的第一笔不出现。
    // 动态渲染没有 render pass / framebuffer 对象，切目标就是两条命令，附件布局也不再被
    // 隐式转换（全由我们显式 barrier 管），把那一整类交互一次性绕开。
    // 拿不到 1.3 动态渲染时（老驱动）useDynamic=false，退回原来的 render pass 路径，
    // 只是没有图层 —— 组透明度退回逐元素近似，别的像素完全不变。
    bool useDynamic = false;
    // 实例创建时实际拿到的 API 版本：动态渲染的特性结构体只有 1.3 实例才"名正言顺"，
    // 低版本实例上启用它属于路径未定义，所以上面 useDynamic 还要再看这个值。
    uint32_t instApiVersion = VK_API_VERSION_1_0;

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
    uint32_t swapImageCount = 0;             // 交换链图数（诊断用）
    bool presentWaitAvailable = false;       // VK_KHR_present_wait/-id 可用（等真上屏）
    uint64_t presentSeq = 0;                 // 已提交的 present id
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
    // 层管线：与主管线只差"采样数"（层目标是单采样）和 render pass 绑定方式。
    // flush() 里按 activePipeline 绑，所以同一套 vertex/descriptor 代码两边共用。
    VkPipeline pipelineLayer = VK_NULL_HANDLE;
    // v27：clip-path 的擦除混合管线（dst *= 1-srcA）。混合是管线状态，SPIR-V 不用动 ——
    // 同一份着色器多建两条管线（主目标 / 图层目标各一条）。
    VkPipeline pipelineErase = VK_NULL_HANDLE;
    VkPipeline pipelineLayerErase = VK_NULL_HANDLE;
    // v28 遮罩保留趟（dst *= srcA）：第三种混合状态，同样建两条变体
    VkPipeline pipelineKeep = VK_NULL_HANDLE;
    VkPipeline pipelineLayerKeep = VK_NULL_HANDLE;
    // v29 毛玻璃合成（Src=ONE / Dst=ZERO，即"直接覆盖"）：第四种混合状态。
    // 合成趟把模糊版背景**替换**掉原背景，所以不能走预乘叠加（见 binui_gfx.h）。
    VkPipeline pipelineReplace = VK_NULL_HANDLE;
    VkPipeline pipelineLayerReplace = VK_NULL_HANDLE;
    // v0.16.44 分离高斯的抽头累加（Src=ONE / Dst=ONE）：没有片元着色器可写，这是唯一能"求和"
    // 的混合方式（GL 那边就是 glBlendFunc(GL_ONE, GL_ONE)）。第五种混合状态。
    VkPipeline pipelineAdd = VK_NULL_HANDLE;
    VkPipeline pipelineLayerAdd = VK_NULL_HANDLE;
    bool eraseOn = false;
    bool keepOn = false;
    bool addOn = false;
    VkPipeline activePipeline = VK_NULL_HANDLE;
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
        uint32_t mips = 1;          // v0.16.15：图片有 mip 链（其余纹理是 1）
        bool linear = false;
        bool needClear = true;      // 新建图像内容未定义：第一次用之前必须清零
        // 已经**记录进命令缓冲**的布局（每帧一开始 cmd 重置，但上一帧提交时
        // 我们就等到 fence 了，所以这个字段始终等于图像的真实布局）
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    Tex white;
    // v0.16.28：毛玻璃颗粒（grain）的 64×64 可平铺噪声。确定性 LCG（种子 0x9E3779B9）、
    // 预乘白，**三端同一张图** —— 种子/递推顺序不能改，否则 GL/D3D11/VK 的颗粒点对不上。
    // 采样用 NEAREST + REPEAT（见 samplerRepeat）：一个纹素 = 一个屏幕像素，屏幕锚定。
    Tex noise;
    VkSampler samplerRepeat = VK_NULL_HANDLE;
    struct RoundTex {
        Tex tex;
        int t = 0;
    };
    std::unordered_map<int, RoundTex> rounds;   // key = 半径（描边用负值，与 GL 同）
    std::unordered_map<uint64_t, Tex> images;   // 图片（键 = index | 滤镜哈希 << 16，v26）
    std::unordered_map<uint64_t, uint32_t> imageGen; // 换图 generation：不匹配时重新上传
    // v26：当前节点的图片滤镜（setImageFilter 设、解码时应用、按哈希进缓存键）
    binui::ColorMatrix imgFilter;
    uint64_t imgFilterKey = 0;
    uint64_t imgKey(int index) const {
        return static_cast<uint64_t>(static_cast<uint32_t>(index)) | (imgFilterKey << 16);
    }
    // v0.16.20：原始像素纹理（视频帧/程序化纹理）的 mip 是否已补（见 prepareImageRaw）
    std::unordered_map<uint64_t, uint8_t> rawMips;

    // 帧外产生的上传（测量文本时会光栅化字形，那时命令缓冲还没开始录制）。
    // 这些先攒着，在下一个 beginFrame 里、render pass 开始之前一次性录进去 ——
    // 所以**不需要预热帧**：同一帧内先上传后绘制，像素就在这一帧里。
    struct PendingUpload {
        Tex* tex = nullptr;
        int x = 0, y = 0, w = 0, h = 0;
        int rowBytes = 0;
        int mip = 0;            // v0.16.15：mip 链的第几级
        std::vector<uint8_t> data;
    };
    std::vector<PendingUpload> pendingUploads;
    bool cmdOpen = false;   // 命令缓冲正在录制（帧内，或一次性的初始化提交）

    // ---- 文本（排版/图集在共享头里，本类只当纹理宿主）----
    // v0.16.18 仿射状态：设置后 emitQuad/emitQuadRot 的四角都过这个 2×3 矩阵
    bool affineOn = false;
    float aff[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    void affPoint(float lx, float ly, float& sx, float& sy) const {
        sx = aff[0] * lx + aff[1] * ly + aff[2];
        sy = aff[3] * lx + aff[4] * ly + aff[5];
    }
    void affCorners(float x0, float y0, float x1, float y1, float* p) const {
        affPoint(x0, y0, p[0], p[1]);
        affPoint(x1, y0, p[2], p[3]);
        affPoint(x0, y1, p[4], p[5]);
        affPoint(x1, y1, p[6], p[7]);
    }

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
    size_t vcount = 0;                           // 本帧已写顶点数（跨 flush 累加，不归零）
    size_t flushBase = 0;                        // 本帧已拷进 GPU 缓冲的顶点数（见 flush）
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

    // ---- 图层（v0.16.12）：组透明度的离屏合成 ----
    // 与 GL/D3D11 同一套设计：每层一块**窗口大小**的 RGBA 目标（这里复用 Tex：
    // image + view + descriptor set），所以坐标不用换算、UV 就是 0..1 的整窗映射。
    // 层内容单采样 —— 图元全是不带 MSAA 的 quad（圆角/描边是解析式覆盖率模板），
    // 少这层 MSAA 逐像素没有差别，但省掉一整张 1280×720×4x 的显存和 resolve 带宽。
    // 池子上限 4 层，超了 pushLayer 返回 false，调用方退回逐元素乘 alpha。
    struct LayerTarget { Tex tex; bool inUse = false; };
    std::vector<LayerTarget> layers;
    struct LayerFrame {
        int idx = -1;
        // 元素框 / 层框（浮点）：合成四边形用它，才能在**小数边界**上拿到主目标 4x MSAA 的
        // 覆盖率抗锯齿（取整后再合成就是 1px 台阶）
        float x = 0, y = 0, w = 0, h = 0;
        int l = 0, t = 0, r = 0, b = 0;          // 层框（元素框 ∩ 进入时的裁剪，取整）
        int pl = 0, pt = 0, pr = 0, pb = 0;      // 进入前的裁剪，退出时还原
        float alpha = 1.0f;
        // v0.16.42：filter: blur(Npx) 的半径（0 = 普通 alpha 层）
        float blur = 0.0f;
        // v0.16.43：filter: drop-shadow 列表（书写顺序；绘制期逆序）
        std::vector<BinUIFilterDropShadowEntry> shadows;
    };
    std::vector<LayerFrame> layerStack;
    static constexpr int kMaxLayers = 4;

    // ---- v29 毛玻璃：降采样链（每级 1/2，按需分配；区域尺寸不变就复用）----
    // 与 GL 后端同一套池子语义（binui_gl-gdi.cpp 的 BlurLevel / ensureBlurLevel）：
    // 下标 = 级别，图尺寸 = 该级的 dw×dh，尺寸变了就重建。VK 与 GL 的两处必要差异：
    //   · 主目标是 4x MSAA，vkCmdCopyImage 不能从多重采样图取像素 → 先 resolve 再拷；
    //   · VK 的纹理行 0 是"区域顶部"（GL 的 FBO 行 0 是底部），所以 GL 一路翻 V、
    //     这里翻转全部抵消，UV 直接用 0..1 / 条带的 b.v0（见 blurBackdrop）。
    struct BlurLevel {
        Tex tex;          // image/view/ds：复用 Tex，销毁/描述符/采样器都是现成的
        int w = 0, h = 0; // 这一级的实际尺寸（= 图尺寸）
    };
    // v0.17.3：降采样链按尺寸池化（与 GL 同一套语义）。以前只有一套链，第 0 级尺寸由
    // 第一个毛玻璃节点决定，同帧第二个不同尺寸的节点就把整条链重建成新尺寸 —— 而"大玻璃卡
    // + 小玻璃按钮 + 模糊浮层"同页很常见。淘汰的图走既有的 retiredBlurTex 两代延迟释放。
    struct BlurSet {
        int w0 = 0, h0 = 0;
        int tick = 0;
        std::vector<BlurLevel> lv;   // 0..5 降采样 + 7 暂存
    };
    std::vector<BlurSet> blurSets;
    int activeBlur = -1;
    int blurTick = 0;
    static constexpr size_t kMaxBlurSets = 16;
    // 同一帧里换尺寸会重建，而旧图仍被**已录制但还没提交**的 draw 引用
    // （vkDeviceWaitIdle 救不了 —— 命令还没提交）。所以分两代延迟释放：
    // 本帧淘汰的进 retiredBlurTex，present 里等过上一帧 fence 之后换成 prev，再下一帧才真放。
    std::vector<Tex> retiredBlurTex;
    std::vector<Tex> retiredBlurTexPrev;
    bool ignoreLayerClip = false;   // 降采样趟画进小图时坐标是"小图局部"的，不能再用层框裁
    bool blurReplace = false;       // 这一趟用 replace 混合（毛玻璃合成）
    bool blurForceSingle = false;   // 目标是毛玻璃小图（单采样）→ 强制用层变体管线

    static constexpr int kMaxBlurLevels = 8;

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
        // 帧内扩容淘汰下来的旧顶点缓冲：命令缓冲里已经录好的 draw 还指着它们，
        // 必须等 fence 之后才真释放（见 growVertBuf）
        for (auto& kv : s.retiredVb) {
            g_vk.vkDestroyBuffer(device, kv.first, nullptr);
            freeMem(kv.second);
        }
        s.retiredVb.clear();
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
                     "（顶点 %zu/帧；其中 acquire %.3f + present 调用 %.3f + 等上屏 %.3f）\n",
                     perfN, (perfRec + perfWait + perfPres) / n, perfRec / n, perfWait / n,
                     perfPres / n, static_cast<size_t>(perfVerts / 60),
                     perfAcquire / 60.0, perfPresCall / 60.0, perfPresentWait / 60.0);
        perfRec = perfWait = perfPres = 0.0;
        perfVerts = 0;
        perfAcquire = 0.0;
        perfPresCall = 0.0;
        perfPresentWait = 0.0;
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
                       VkDeviceMemory& mem, VkImageAspectFlags aspect,
                       uint32_t mipLevels = 1) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), 1};
        ci.mipLevels = mipLevels;
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

    VkImageView makeView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect,
                         uint32_t levelCount = 1) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = levelCount;
        vi.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        if (g_vk.vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        return view;
    }

    void barrier(VkImage img, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                 uint32_t levelCount = 1) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = levelCount;
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

    // 设备扩展探测：present_wait / present_id 用来"等这一帧真的上屏"。
    // 只有窗口模式才有意义；离屏路径不涉及交换链。
    bool hasDeviceExt(const char* name) const {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            g_vk.getInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
        if (!enumerate) return false;
        uint32_t n = 0;
        if (enumerate(physical, nullptr, &n, nullptr) != VK_SUCCESS) return false;
        std::vector<VkExtensionProperties> props(n);
        if (enumerate(physical, nullptr, &n, props.data()) != VK_SUCCESS) return false;
        for (const auto& p : props)
            if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    }

    bool createDevice() {
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDevicePresentIdFeaturesKHR presentIdFeat{};
        presentIdFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
        presentIdFeat.presentId = VK_TRUE;
        VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeat{};
        presentWaitFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
        presentWaitFeat.presentWait = VK_TRUE;

        // ---- 动态渲染特性：先问支持，再从 pNext 链里正式启用 ----
        // 光有函数指针不够：Vulkan 要求 dynamicRendering 特性在设备创建时显式打开，
        // 否则 vkCmdBeginRendering 是未定义行为（有的驱动能用、有的直接丢命令）。
        // BINUI_VK_DYNRENDER=0 可强制关掉，用来做 A/B 对比（两条路径必须逐像素相同）。
        //
        // ★ 设备函数表要到 loadDeviceFns（vkCreateDevice 之后）才填，但"要不要启用这个特性"
        // 必须在建设备时决定 —— 所以这里先用实例的 getInstanceProcAddr 单独探一次。
        auto probeDevFn = [&](const char* name, const char* altName) {
            void* p = reinterpret_cast<void*>(g_vk.getInstanceProcAddr(instance, name));
            if (!p && altName) p = reinterpret_cast<void*>(g_vk.getInstanceProcAddr(instance, altName));
            return p;
        };
        const bool hasDynFns = probeDevFn("vkCmdBeginRendering", "vkCmdBeginRenderingKHR") != nullptr &&
                               probeDevFn("vkCmdEndRendering", "vkCmdEndRenderingKHR") != nullptr;
        bool wantDynamic = std::getenv("BINUI_VK_DYNRENDER") == nullptr &&
                           instApiVersion >= VK_API_VERSION_1_3 && hasDynFns;
        VkPhysicalDeviceVulkan13Features vk13{};
        vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        if (wantDynamic && g_vk.vkGetPhysicalDeviceFeatures2) {
            VkPhysicalDeviceVulkan13Features q{};
            q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &q;
            g_vk.vkGetPhysicalDeviceFeatures2(physical, &f2);
            useDynamic = q.dynamicRendering == VK_TRUE;
            if (!useDynamic)
                std::fprintf(stderr, "[vk] 设备不支持 dynamicRendering，图层关闭（用 render pass 路径）\n");
        } else if (wantDynamic) {
            std::fprintf(stderr, "[vk] 取不到 vkGetPhysicalDeviceFeatures2，图层关闭\n");
        }
        if (useDynamic) vk13.dynamicRendering = VK_TRUE;

        std::vector<const char*> exts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const void* dciPNext = nullptr;
        if (windowMode && hasDeviceExt(VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            hasDeviceExt(VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            presentIdFeat.pNext = &presentWaitFeat;
            presentWaitFeat.pNext = nullptr;
            dciPNext = &presentIdFeat;
            presentWaitAvailable = true;
        }
        if (useDynamic) {
            vk13.pNext = const_cast<void*>(dciPNext);
            dciPNext = &vk13;
        }
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pNext = dciPNext;        if (windowMode) {
            dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
            dci.ppEnabledExtensionNames = exts.data();
        }
        VK_CHECK(g_vk.vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        g_vk.vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (!g_vk.loadDeviceFns(instance)) return false;
        // 扩展启用但函数取不到 → 当作不可用，别让它变成空指针调用
        if (presentWaitAvailable && !g_vk.vkWaitForPresentKHR) presentWaitAvailable = false;
        if (useDynamic && (!g_vk.vkCmdBeginRendering || !g_vk.vkCmdEndRendering)) useDynamic = false;
        return true;
    }

    void destroyTargets() {
        destroyBlurLevels();   // v29：毛玻璃降采样图也是按"目标尺寸"走的，一起放
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
        if (std::getenv("BINUI_RESIZE_TRACE"))
            std::fprintf(stderr, "[vk][targets] 重建 %dx%d（swap %ux%u）\n", w, h,
                         swapExtent.width, swapExtent.height);
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
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,   // TRANSFER_DST：动态渲染的显式 resolve
                           VK_SAMPLE_COUNT_1_BIT, resolveImage, resolveMem, VK_IMAGE_ASPECT_COLOR_BIT))
            return fail("resolve image");
        msaaView = makeView(msaaImage, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        resolveView = makeView(resolveImage, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!msaaView || !resolveView) return fail("image views");

        // render pass：attachment0 = MSAA 颜色（LOAD，present 前不 swap），
        // attachment1 = resolve 目标（DONT_CARE 加载、STORE、末尾转 TRANSFER_SRC）
        // 动态渲染路径不需要这两个对象 —— 直接跳过（见 useDynamic 的说明）。
        if (!useDynamic) {
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
        att[1].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // 与下面的首次转换一致
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
        }   // !useDynamic

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

        // 首帧前把两张图转进各自的常驻布局。
        // 动态渲染没有 render pass 帮忙做隐式转换，所以布局全程由我们自己记账：
        //   MSAA 图   → COLOR_ATTACHMENT_OPTIMAL（始终）
        //   resolve 图 → COLOR_ATTACHMENT_OPTIMAL（帧内）↔ TRANSFER_SRC_OPTIMAL（present 拷出）
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(g_vk.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        barrier(msaaImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        if (useDynamic) {
            // 常驻 TRANSFER_DST：每帧末的 vkCmdResolveImage 写它，再转到 TRANSFER_SRC 读回
            barrier(resolveImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        } else {
            barrier(resolveImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
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

    // ---- surface 能力缓存 ----
    // 拖边框时交换链每帧重建，而重建前的**查询**（caps / formats / presentModes 三次枚举）
    // 实测占 3.3~4.4 ms —— 比 vkCreateSwapchainKHR 本身还贵。这三样在一个 surface 上
    // 是不变的（caps.currentExtent 除外，窗口尺寸由我们自己记着），所以只在建 surface
    // 和重建失败时查一次，之后全部走缓存。
    bool surfInfoValid = false;
    VkSurfaceCapabilitiesKHR surfCaps{};
    VkSurfaceFormatKHR surfFmt{};
    std::vector<VkPresentModeKHR> surfModes;

    bool ensureSurfaceInfo() {
        if (surfInfoValid) return true;
        if (!surface || !physical) return false;
        if (g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &surfCaps) !=
            VK_SUCCESS)
            return false;
        // 首选 B8G8R8A8_UNORM（与渲染目标同格式，blit 是纯拷贝）；没有就退回驱动给的第一种
        surfFmt.format = kColorFormat;
        surfFmt.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        uint32_t n = 0;
        g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
        if (n > 0) {
            std::vector<VkSurfaceFormatKHR> fs(n);
            g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, fs.data());
            bool found = false;
            for (const auto& f : fs) {
                if (f.format == kColorFormat && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    surfFmt = f;
                    found = true;
                    break;
                }
            }
            // n==1 且 format==UNDEFINED：驱动表示"任意格式"，保持首选
            if (!found && !(n == 1 && fs[0].format == VK_FORMAT_UNDEFINED)) surfFmt = fs[0];
        }
        n = 0;
        surfModes.clear();
        g_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr);
        if (n > 0) {
            surfModes.resize(n);
            g_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, surfModes.data());
        }
        surfInfoValid = true;
        return true;
    }

    // vsync=false 时优先 MAILBOX（不撕裂、不阻塞），否则 IMMEDIATE；都没有就 FIFO
    // 走缓存里的 presentModes（见 ensureSurfaceInfo：每帧重建时不再枚举）
    VkPresentModeKHR pickPresentMode(bool vsyncWanted) const {
        if (surfModes.empty()) return VK_PRESENT_MODE_FIFO_KHR;
        auto has = [&](VkPresentModeKHR m) {
            for (VkPresentModeKHR x : surfModes)
                if (x == m) return true;
            return false;
        };
        if (vsyncWanted)
            return has(VK_PRESENT_MODE_FIFO_KHR) ? VK_PRESENT_MODE_FIFO_KHR : surfModes[0];
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // reuseOld：Vulkan 推荐的 resize 路径 —— 不先销毁旧链，而是把旧链交给驱动
    // （ci.oldSwapchain），驱动可以复用底层交换链资源；创建成功后再由我们自己销毁旧链。
    // 实测"先销毁再新建"每次 ~26 ms（拖边框每帧一次 = 直接掉到 30 fps），复用后见 docs/startup.md §5。
    bool createSwapchain(bool reuseOld = false) {
        if (!windowMode || !surface) return false;
        const bool swapTrace = std::getenv("BINUI_RESIZE_TRACE") != nullptr;
        const auto tE0 = swapTrace ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        VkSwapchainKHR oldChain = VK_NULL_HANDLE;
        std::vector<VkImageView> oldViews;
        if (reuseOld && swapchain) {
            oldChain = swapchain;
            oldViews.swap(swapViews);
            swapImages.clear();
            swapchain = VK_NULL_HANDLE;
            destroySlotSemaphores();   // 与交换链同生共死；且必须早于 createSlotSemaphores
        } else {
            destroySwapchain();
        }
        // 任何一条失败返回都要把旧链销毁掉，否则泄漏一个 swapchain 和它的图像
        struct OldChainGuard {
            VkDevice dev;
            VkSwapchainKHR chain;
            std::vector<VkImageView> views;
            ~OldChainGuard() {
                if (!chain) return;
                for (VkImageView v : views)
                    if (v) g_vk.vkDestroyImageView(dev, v, nullptr);
                if (g_vk.vkDestroySwapchainKHR) g_vk.vkDestroySwapchainKHR(dev, chain, nullptr);
            }
        } oldGuard{device, oldChain, std::move(oldViews)};

        if (!ensureSurfaceInfo()) {
            std::fprintf(stderr, "[vk] 取不到 surface capabilities\n");
            return false;
        }
        const VkSurfaceCapabilitiesKHR& caps = surfCaps;
        const VkSurfaceFormatKHR& sf = surfFmt;
        // 尺寸用"我们要的窗口尺寸"做起点 —— caps.currentExtent 是缓存里**唯一会变**的
        // 字段，照着缓存建就是"按旧尺寸重建"，随后 present 前发现不一致又重建一次，
        // 每帧空转（实测就是这样，一次 resize 里出现两条建链日志）。
        VkExtent2D ext{};
        ext.width = static_cast<uint32_t>(std::max(1, w));
        ext.height = static_cast<uint32_t>(std::max(1, h));
        // 缓存值与我们要的不一致时（换屏 / DPI / 驱动改写），实时确认一次再决定
        if (caps.currentExtent.width != 0xFFFFFFFFu &&
            (caps.currentExtent.width != ext.width ||
             caps.currentExtent.height != ext.height) &&
            surface && physical) {
            VkSurfaceCapabilitiesKHR live{};
            if (g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &live) ==
                VK_SUCCESS) {
                surfCaps = live;
                if (live.currentExtent.width != 0xFFFFFFFFu) ext = live.currentExtent;
            }
        }
        ext.width = std::max(caps.minImageExtent.width,
                             std::min(caps.maxImageExtent.width, ext.width));
        ext.height = std::max(caps.minImageExtent.height,
                              std::min(caps.maxImageExtent.height, ext.height));
        if (ext.width == 0 || ext.height == 0) return false;

        // 交换链图数：minImageCount+1。真正决定节拍的是下面的 present_wait（等这一帧
        // 上屏），图数只影响"排队能有多深"；多留一张是为了 acquire 不必去等合成器。
        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;
        swapImageCount = imageCount;

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
        ci.oldSwapchain = oldChain;

        const bool swapTrace2 = std::getenv("BINUI_RESIZE_TRACE") != nullptr;
        const auto tS0 = swapTrace2 ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        const VkResult cr = g_vk.vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain);
        const auto tS1 = swapTrace2 ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        if (cr != VK_SUCCESS) {
            surfInfoValid = false;   // 能力/格式可能真的变了（换屏、DPI、驱动重置）：下次数值重查
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
        if (swapTrace2) {
            const auto tS2 = std::chrono::steady_clock::now();
            const auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr,
                         "[vk][swap] %ux%u  queries=%.2f  CreateSwapchain=%.2f  views+sem=%.2f ms\n",
                         ext.width, ext.height, ms(tE0, tS0), ms(tS0, tS1), ms(tS1, tS2));
        }
        return true;
    }

    // resize 后交换链可能失效（0 尺寸 / 超出 caps），重建一次即可
    bool recreateSwapchain() {
        g_vk.vkDeviceWaitIdle(device);
        return createSwapchain(true);   // 复用旧链：驱动能省掉一大半分配/同步
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
        const int r = SetDIBitsToDevice(dc, 0, 0, pw, ph, 0, 0, 0, ph, px, &bi,
                                        DIB_RGB_COLORS);
        // BINUI_RESIZE_TRACE=1：GDI 贴屏失败时**必须**看见（失败就没画面，但不报错）
        static const bool blitTrace = std::getenv("BINUI_RESIZE_TRACE") != nullptr;
        if (blitTrace) {
            static int blitN = 0;
            if (blitN++ < 12) {
                RECT cr = {};
                GetClientRect(hwnd, &cr);
                std::fprintf(stderr, "[vk][blit] %dx%d ret=%d err=%lu 客户区=%dx%d\n", pw, ph, r,
                             (unsigned long)GetLastError(), static_cast<int>(cr.right),
                             static_cast<int>(cr.bottom));
                std::fflush(stderr);
            }
        }
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
    // mips > 1：图片用（见 prepareImage 里的 CPU 盒式滤波 mip 链）
    bool createTexImage(Tex& t, int tw, int th, bool linear, uint32_t mips = 1) {
        if (!createImage2D(tw, th, kColorFormat,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_SAMPLE_COUNT_1_BIT, t.image, t.mem, VK_IMAGE_ASPECT_COLOR_BIT,
                           mips))
            return false;
        t.view = makeView(t.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, mips);
        if (!t.view) {
            std::fprintf(stderr, "[vk] createTexImage: vkCreateImageView 失败\n");
            return false;
        }
        t.w = tw;
        t.h = th;
        t.mips = mips;
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

    // 描述符里的采样器是按纹理挑的（线性/最近 + 平铺方式）。默认按 Tex::linear 选；
    // v0.16.28 的颗粒噪声要 NEAREST + REPEAT，走显式版本。
    void writeTexDsWith(Tex& t, VkSampler sampler) {
        VkDescriptorImageInfo dii{};
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dii.imageView = t.view;
        dii.sampler = sampler;
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = t.ds;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &dii;
        g_vk.vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
    }

    void writeTexDs(Tex& t) {
        writeTexDsWith(t, t.linear ? samplerLinear : samplerNearest);
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
        // LINEAR：动画节点的文字按亚像素落位（见 GlyphAtlas::snapText）。整像素位置下
        // 1:1 采样点正好落在纹素中心，与 NEAREST 逐像素相同，静态页面不受影响。
        if (!createTexImage(*t, tw, th, true)) {
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
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                // v29：毛玻璃的降采样图/合成目标会从"附件（刚被写）"转成只读被采样，
                // 少了这条依赖就是经典的写→读 hazard。
                srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
        barrier(t.image, t.layout, dst, srcAccess, dstAccess, srcStage, dstStage, t.mips);
        t.layout = dst;
    }

    // 把一块 BGRA 像素拷进纹理的 (x,y,w,h)：staging → vkCmdCopyBufferToImage
    bool stageCopy(Tex& t, int x, int y, int w, int h, const void* src, int rowBytes,
                   int mipLevel = 0) {
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
        region.imageSubresource.mipLevel = static_cast<uint32_t>(mipLevel);
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

    bool stageUpload(Tex& t, int x, int y, int w, int h, const void* src, int rowBytes,
                     int mipLevel = 0) {
        if (t.needClear && mipLevel == 0) {
            t.needClear = false;
            // 整张覆盖的上传可以免掉清零
            if (!(x == 0 && y == 0 && w == t.w && h == t.h && rowBytes == t.w * 4))
                if (!zeroTex(t)) return false;
        }
        return stageCopy(t, x, y, w, h, src, rowBytes, mipLevel);
    }

    // 上传入口：帧内直接录，帧外排队（下一个 beginFrame 里补）
    bool uploadRegion(Tex& t, int x, int y, int w, int h, const void* src, int rowBytes,
                      int mipLevel = 0) {
        if (!t.image || w <= 0 || h <= 0 || !src) return false;
        if (rowBytes <= 0) rowBytes = w * 4;
        if (cmdOpen) return stageUpload(t, x, y, w, h, src, rowBytes, mipLevel);
        PendingUpload pu;
        pu.tex = &t;
        pu.x = x; pu.y = y; pu.w = w; pu.h = h;
        pu.rowBytes = rowBytes;
        pu.mip = mipLevel;
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
            if (pu.tex) stageUpload(*pu.tex, pu.x, pu.y, pu.w, pu.h, pu.data.data(), pu.rowBytes,
                                    pu.mip);
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
        auto make = [this](bool linear, VkSamplerAddressMode wrap, VkSampler& out) {
            VkSamplerCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            // v0.16.15：线性档走 mip 线性插值（三线性）+ 放开 LOD 上限。单级纹理
            // （字形图集/圆角模板/视频帧）没有 mip 链，采样会被钳到第 0 级，行为不变。
            si.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                  : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = wrap;
            si.addressModeV = wrap;
            si.addressModeW = wrap;
            si.maxLod = linear ? VK_LOD_CLAMP_NONE : 0.0f;
            if (g_vk.vkCreateSampler(device, &si, nullptr, &out) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] createSamplers: vkCreateSampler 失败 (linear=%d)\n",
                             (int)linear);
                return false;
            }
            return true;
        };
        // 第三个是毛玻璃颗粒专用的 NEAREST + REPEAT（UV = x/64 会超出 0..1，必须平铺）
        return make(false, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, samplerNearest) &&
               make(true, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, samplerLinear) &&
               make(false, VK_SAMPLER_ADDRESS_MODE_REPEAT, samplerRepeat);
    }

    bool createWhite() {
        if (!createTexImage(white, 1, 1, false)) return false;
        const uint32_t px = 0xFFFFFFFFu;   // 预乘白：纯色四边形 × 1.0
        return oneShot([&] {
            white.needClear = false;       // 整张上传，不用再清零
            stageUpload(white, 0, 0, 1, 1, &px, 4);
        });
    }

    // v0.16.28 颗粒噪声：64×64、确定性 LCG、预乘白（rgb == alpha）。
    // 与 GL 的 ensureNoiseTex / D3D11 的 ensureNoise **逐字相同**的像素生成；
    // 唯一差别是采样器：VK 的 REPEAT 要单独建一个 sampler 对象（描述符里带）。
    bool createNoise() {
        const int N = 64;
        if (!createTexImage(noise, N, N, false)) return false;   // NEAREST
        writeTexDsWith(noise, samplerRepeat);                    // + REPEAT
        std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
        uint32_t st = 0x9E3779B9u;   // 固定种子：三端同一张噪声
        for (int i = 0; i < N * N; i++) {
            st = st * 1664525u + 1013904223u;
            const uint8_t a = static_cast<uint8_t>((st >> 16) & 0xFFu);
            const size_t o = static_cast<size_t>(i) * 4;
            px[o + 0] = a;   // 预乘白：rgb 跟着 alpha 走
            px[o + 1] = a;
            px[o + 2] = a;
            px[o + 3] = a;
        }
        return oneShot([&] {
            noise.needClear = false;   // 整张覆盖，不用先清零
            stageUpload(noise, 0, 0, N, N, px.data(), N * 4);
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
        // v0.11.10：圆角模板**必须线性采样**，与 GL / D3D11 一致。
        // 静态帧里"角片边长 == 模板 texel 数"（1:1 对齐），NEAREST 与 LINEAR 逐像素相同；
        // 但缩放动画里角片被非整数拉伸（rf/t ≠ 1），NEAREST 会把弧线量化成一格格台阶，
        // 表现为"只有放大/缩小在抖、平移不抖" —— 正是 VK 独占、另两个后端没有的现象。
        if (!createTexImage(rt.tex, t, t, true)) {
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
        ms.rasterizationSamples = samples;   // 逐管线覆盖（层管线是单采样）
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
        // 动态渲染的管线不绑 render pass，改成用 pNext 声明附件格式
        VkPipelineRenderingCreateInfo prci{};
        VkFormat colorFmt = kColorFormat;
        if (useDynamic) {
            prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            prci.colorAttachmentCount = 1;
            prci.pColorAttachmentFormats = &colorFmt;
            gp.pNext = &prci;
        } else {
            gp.renderPass = renderPass;
            gp.subpass = 0;
        }

        // 两条管线：主目标（可能 4x MSAA）+ 层目标（单采样）。除采样数外状态完全一致，
        // 所以同一份 VkGraphicsPipelineCreateInfo 改一下 ms 就建第二条。
        auto build = [&](VkSampleCountFlagBits sc, VkPipeline& out) {
            ms.rasterizationSamples = sc;
            return g_vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &out);
        };
        VkResult pr = build(samples, pipeline);
        if (pr == VK_SUCCESS && useDynamic) pr = build(VK_SAMPLE_COUNT_1_BIT, pipelineLayer);
        // v27 擦除变体
        if (pr == VK_SUCCESS) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            pr = build(samples, pipelineErase);
            if (pr == VK_SUCCESS && useDynamic)
                pr = build(VK_SAMPLE_COUNT_1_BIT, pipelineLayerErase);
        }
        // v28 遮罩保留趟变体：dst *= srcA
        if (pr == VK_SUCCESS) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            pr = build(samples, pipelineKeep);
            if (pr == VK_SUCCESS && useDynamic)
                pr = build(VK_SAMPLE_COUNT_1_BIT, pipelineLayerKeep);
        }
        // v29 毛玻璃 replace 变体：Src=ONE / Dst=ZERO —— 模糊版背景**覆盖**原背景。
        // 注意 Dst 因子是 ZERO 而不是 ONE_MINUS_SRC_ALPHA：alpha 也一起被替换，
        // 否则半透明区域会残留旧背景（GL 那边就是 glBlendFunc(GL_ONE, GL_ZERO)）。
        if (pr == VK_SUCCESS) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            pr = build(samples, pipelineReplace);
            if (pr == VK_SUCCESS && useDynamic)
                pr = build(VK_SAMPLE_COUNT_1_BIT, pipelineLayerReplace);
        }
        // v0.16.44 加法变体：Src=ONE / Dst=ONE —— 分离高斯的抽头累加（见 gaussBlurLevel）
        if (pr == VK_SUCCESS) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            pr = build(samples, pipelineAdd);
            if (pr == VK_SUCCESS && useDynamic)
                pr = build(VK_SAMPLE_COUNT_1_BIT, pipelineLayerAdd);
        }
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
        s.vbufBytes = static_cast<VkDeviceSize>(kMaxVertices) * sizeof(VkVertex);
        if (verts.size() < kMaxVertices) verts.resize(kMaxVertices);   // 只扩不缩（可能已被 growVertBuf 撑大）
        batches.reserve(kMaxBatches);
    }

    // 顶点写入口：装不下就扩 CPU 侧缓冲（翻倍，上限 kMaxVerticesCeiling）；
    // 只有撞到硬上限才返回 nullptr 并告警一次。
    VkVertex* vertRoom(size_t n) {
        if (vcount + n > verts.size()) {
            size_t cap = verts.size() ? verts.size() : kMaxVertices;
            while (cap < vcount + n && cap < kMaxVerticesCeiling) cap *= 2;
            if (cap > kMaxVerticesCeiling) cap = kMaxVerticesCeiling;
            if (vcount + n > cap) { warnVertsOverflow(); return nullptr; }
            verts.resize(cap);
        }
        return verts.data() + vcount;
    }

    static void warnVertsOverflow() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr,
                     "[vk] 顶点数撞上硬上限（%zu 个四边形/帧），后面的图元被丢弃，"
                     "见 docs/future.md P0-5\n", kMaxVerticesCeiling / 6);
    }

    // 本槽的顶点缓冲装不下本帧顶点数时重建一个更大的。
    // 安全性：绘制命令是 flush 时才录的（此时还没 vkCmdBindVertexBuffers），而且本槽的
    // fence 在 beginFrame 里已经等过 —— 这个缓冲此刻没有任何在飞的命令引用它。
    bool growVertBuf(size_t needVerts) {
        Slot& s = slots[slotIdx];
        const VkDeviceSize needBytes = static_cast<VkDeviceSize>(needVerts) * sizeof(VkVertex);
        if (s.vbuf && s.vbufBytes >= needBytes) return true;
        size_t cap = s.vbufBytes ? static_cast<size_t>(s.vbufBytes / sizeof(VkVertex)) : kMaxVertices;
        while (cap < needVerts && cap < kMaxVerticesCeiling) cap *= 2;
        if (cap > kMaxVerticesCeiling) cap = kMaxVerticesCeiling;
        if (cap < needVerts) { warnVertsOverflow(); return false; }
        if (s.vbuf) {
            // 不能立刻销毁：本帧已经 flush 出去的 draw 还引用着它（一帧内可以多次 flush），
            // 挂到 retiredVb 上等本槽 fence 之后由 freeStaging 统一回收。
            g_vk.vkUnmapMemory(device, s.vmem);
            s.retiredVb.emplace_back(s.vbuf, s.vmem);
            s.vbuf = VK_NULL_HANDLE;
            s.vmem = VK_NULL_HANDLE;
            s.vmap = nullptr;
            s.vbufBytes = 0;
        }
        if (!createBuffer(static_cast<VkDeviceSize>(cap) * sizeof(VkVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          s.vbuf, s.vmem)) {
            std::fprintf(stderr, "[vk] growVertBuf: 顶点缓冲重建失败（%zu 顶点）\n", cap);
            return false;
        }
        void* mapped = nullptr;
        if (g_vk.vkMapMemory(device, s.vmem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return false;
        s.vmap = static_cast<VkVertex*>(mapped);
        s.vbufBytes = static_cast<VkDeviceSize>(cap) * sizeof(VkVertex);
        // 本帧的别名同步（flush 里用的是 d.vbuf / d.vmap）
        vbuf = s.vbuf;
        vmap = s.vmap;
        vmem = s.vmem;
        return true;
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
        if (affineOn) {   // 仿射：四角过矩阵（文字/图片跟着卡片转）
            float p[8], uv[8];
            affCorners(x0, y0, x1, y1, p);
            uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0;
            uv[4] = u0; uv[5] = v1; uv[6] = u1; uv[7] = v1;
            emitQuadUV(p, uv, color, tex);
            return;
        }
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

    // 任意四边形 + 每角 UV（旋转/斜切用），角顺序 0=左上 1=右上 2=左下 3=右下。
    void emitQuadUV(const float* p, const float* uv, uint32_t color, Tex* tex) {
        VkVertex* o = vertRoom(6);
        if (!o) return;
        switchBatch(tex);
        o[0] = VkVertex{p[0], p[1], color, uv[0], uv[1]};
        o[1] = VkVertex{p[2], p[3], color, uv[2], uv[3]};
        o[2] = VkVertex{p[4], p[5], color, uv[4], uv[5]};
        o[3] = o[2];
        o[4] = o[1];
        o[5] = VkVertex{p[6], p[7], color, uv[6], uv[7]};
        vcount += 6;
        cur.count += 2;
    }

    // v27：按 (是否在图层, 是否擦除) 选管线；v29 多一档毛玻璃 replace
    void refreshPipeline() {
        // 毛玻璃的降采样目标是 1 采样的小图（不是图层），所以要能和"是否在图层里"解耦
        const bool single = blurForceSingle || !layerStack.empty();
        if (addOn)        activePipeline = single ? pipelineLayerAdd : pipelineAdd;
        else if (blurReplace)  activePipeline = single ? pipelineLayerReplace : pipelineReplace;
        else if (keepOn)  activePipeline = single ? pipelineLayerKeep : pipelineKeep;
        else if (eraseOn) activePipeline = single ? pipelineLayerErase : pipelineErase;
        else              activePipeline = single ? pipelineLayer : pipeline;
        if (!activePipeline)  activePipeline = pipeline;   // 变体管线建失败就退回普通混合
    }

    void flush() {
        // 每帧一次也要提成 static：UCRT 的 getenv 要遍历环境表，~0.5 µs/次，
        // 和本文件 fillRect 里同一个坑（§7.3）。
        static const bool trace = std::getenv("BINUI_VK_TRACE") != nullptr;
        if (trace) {
            std::fprintf(stderr, "[vk][trace] flush: verts=%zu batches=%zu cur.count=%u\n",
                         vcount, batches.size(), cur.count);
        }
        if (cur.count > 0) batches.push_back(cur);
        cur = Batch{};
        // 顶点攒在 CPU 缓冲里（见 verts 的注释），这里整块拷进映射的槽缓冲。
        //
        // ★ 一帧内可以 flush 多次（图层进出目标时各一次），所以**只能追加，不能从 0 重拷**：
        // 顶点缓冲是帧共享的，前面已经录进命令缓冲的 draw 都指着它。以前一帧只 flush 一次，
        // 从 0 重拷看不出问题；接上图层后，后一次 flush 的 memcpy 会把前面所有 draw 的
        // 数据覆盖成最后一批 —— 现象是"整帧只剩最后画的那几个四边形"（v0.16.12 抓到）。
        if (vcount == flushBase || !vmap) { batches.clear(); return; }
        // 本帧顶点比槽缓冲大：先扩容（扩容会把旧缓冲挂到 staging 上等 fence 后再释放，
        // 否则已经录进命令缓冲的 draw 会指向一张已销毁的缓冲）
        if (!growVertBuf(vcount)) { batches.clear(); return; }
        std::memcpy(vmap + flushBase, verts.data() + flushBase,
                    (vcount - flushBase) * sizeof(VkVertex));

        g_vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              activePipeline ? activePipeline : pipeline);
        VkDeviceSize off = 0;
        g_vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
        // 正在画进图层时，每条批次再和层框求交 —— 层外的内容不该出现在层里。
        // 层内若发生 popClipTo（把裁剪放宽回整窗），这条兜住它，不让图元越出层框。
        const LayerFrame* lf = layerStack.empty() ? nullptr : &layerStack.back();
        for (const Batch& b : batches) {
            if (b.count == 0) continue;
            VkRect2D sc{};
            sc.offset = {static_cast<int32_t>(std::max(0, b.sx)),
                         static_cast<int32_t>(std::max(0, b.sy))};
            sc.extent = {static_cast<uint32_t>(std::max(0, std::min(b.sw, w - b.sx))),
                         static_cast<uint32_t>(std::max(0, std::min(b.sh, h - b.sy)))};
            if (lf && !ignoreLayerClip) {
                const int32_t l = std::max<int32_t>(sc.offset.x, lf->l);
                const int32_t t = std::max<int32_t>(sc.offset.y, lf->t);
                const int32_t r = std::min<int32_t>(sc.offset.x + static_cast<int32_t>(sc.extent.width), lf->r);
                const int32_t bb = std::min<int32_t>(sc.offset.y + static_cast<int32_t>(sc.extent.height), lf->b);
                sc.offset = {l, t};
                sc.extent = {static_cast<uint32_t>(std::max(0, r - l)),
                             static_cast<uint32_t>(std::max(0, bb - t))};
            }
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
        flushBase = vcount;   // 已提交到 GPU 的部分，下一次 flush 只追加后面的
        frameVerts = vcount;
    }

    // ============================================================
    // 渲染目标切换（主目标 / 图层目标）
    // ============================================================
    // 收在一处的两个 helper：动态渲染和 render pass 两种写法的差异只在这里出现。
    // 视角/推入常量每条 pass 开头重设一次（切 pass 后保持，但不靠保持）。

    void setPassState() {
        VkViewport vp{};
        vp.x = 0; vp.y = 0;
        vp.width = static_cast<float>(w);
        vp.height = static_cast<float>(h);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        g_vk.vkCmdSetViewport(cmd, 0, 1, &vp);
        float vs[2] = {static_cast<float>(w), static_cast<float>(h)};
        if (pipeLayout)
            g_vk.vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, vs);
    }

    // 附件写→写/读→写 的显式依赖：动态渲染没有 subpass 依赖表，同一张图在一帧内
    // 被两次 beginRendering 当附件用时（主目标进出图层、外层图层嵌套）必须自己发 barrier。
    void barrierAttachment(VkImage img) {
        const VkAccessFlags rw = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier(img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rw, rw,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    VkRenderingAttachmentInfo makeAttachment(VkImageView view, VkImageLayout layout,
                                            VkAttachmentLoadOp loadOp) {
        VkRenderingAttachmentInfo ca{};
        ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca.imageView = view;
        ca.imageLayout = layout;
        ca.loadOp = loadOp;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        return ca;
    }

    VkRenderingInfo makeRenderingInfo(VkRenderingAttachmentInfo* ca, int l = 0, int t = 0,
                                     int r = -1, int b = -1) {
        if (r < 0) r = w;
        if (b < 0) b = h;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.offset = {l, t};
        ri.renderArea.extent = {static_cast<uint32_t>(std::max(0, r - l)),
                                static_cast<uint32_t>(std::max(0, b - t))};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = ca;
        return ri;
    }

    // resume=true：本帧内第二次进入主目标（从图层回来）—— 内容要保住，加一条依赖
    void beginMainPass(bool resume = false) {
        refreshPipeline();
        if (!useDynamic) {
            VkRenderPassBeginInfo rpb{};
            rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpb.renderPass = renderPass;
            rpb.framebuffer = framebuffer;
            rpb.renderArea.offset = {0, 0};
            rpb.renderArea.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
            g_vk.vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            setPassState();
            return;
        }
        if (resume) {
            barrierAttachment(msaaImage);
        }
        VkRenderingAttachmentInfo ca =
            makeAttachment(msaaView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_ATTACHMENT_LOAD_OP_LOAD);   // 累积帧：进出图层都要保住已有内容
        // ★ 这里**不挂 resolveImageView**：一帧内主 pass 可能进进出出好几次（每个图层两次），
        // 每次 EndRendering 都隐式 resolve 一遍 4x MSAA 全屏，实测 12 个图层的页面里
        // GPU 侧多花 0.17 ms/帧。改成帧末用 vkCmdResolveImage 显式 resolve 一次。
        VkRenderingInfo ri = makeRenderingInfo(&ca);
        g_vk.vkCmdBeginRendering(cmd, &ri);
        setPassState();
    }

    void endMainPass() {
        if (useDynamic) g_vk.vkCmdEndRendering(cmd);
        else g_vk.vkCmdEndRenderPass(cmd);
    }

    // ---- 图层目标 ----
    static void warnLayerLimit() {
        static bool warned = false;
        if (warned) return;
        warned = true;
        std::fprintf(stderr, "[vk] 图层超过 %d 层，多出来的退回逐元素乘 alpha（减少嵌套即可）\n",
                     kMaxLayers);
    }

    int acquireLayer() {
        for (size_t i = 0; i < layers.size(); i++)
            if (!layers[i].inUse) return static_cast<int>(i);
        if (static_cast<int>(layers.size()) >= kMaxLayers) return -1;
        LayerTarget t;
        if (!createImage2D(w, h, kColorFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // v29：毛玻璃要把层里背景拷走
                           VK_SAMPLE_COUNT_1_BIT, t.tex.image, t.tex.mem,
                           VK_IMAGE_ASPECT_COLOR_BIT))
            return -1;
        t.tex.w = w;
        t.tex.h = h;
        // 线性采样：合成四边形是整窗 1:1 映射，两种过滤器结果完全相同；选 LINEAR 是为了
        // 以后若改成"按层框 UV 合成"（省填充率）时不会突然糊掉。
        t.tex.linear = true;
        t.tex.needClear = false;   // 内容由 loadOp=CLEAR 负责，不走上传路径
        t.tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.tex.view = makeView(t.tex.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.tex.view || !allocTexDs(t.tex)) {
            std::fprintf(stderr, "[vk] 图层目标创建失败（%dx%d）\n", w, h);
            freeTex(t.tex);
            return -1;
        }
        layers.push_back(t);
        return static_cast<int>(layers.size() - 1);
    }

    void destroyLayers() {
        for (LayerTarget& t : layers) freeTex(t.tex);
        layers.clear();
        layerStack.clear();
    }

    // ============================================================
    // v29 毛玻璃：降采样图池 + 布局切换
    // ============================================================
    // 尺寸变了就重建（与 GL 的 ensureBlurLevel 同一套语义）。旧图不能立刻销毁：
    // 本帧已经录进命令缓冲的 draw 还引用着它的 image/descriptor set，而命令缓冲
    // 要到 present 才提交 —— vkDeviceWaitIdle 也救不了。所以只挂到 retired 队列。
    void retireBlurTex(Tex& t) {
        retiredBlurTex.push_back(t);
        t = Tex{};
    }

    void destroyBlurLevels() {
        for (BlurSet& s : blurSets)
            for (BlurLevel& b : s.lv) {
                if (b.tex.image) freeTex(b.tex);
                b.w = b.h = 0;
            }
        blurSets.clear();
        activeBlur = -1;
        for (Tex& t : retiredBlurTex) freeTex(t);
        retiredBlurTex.clear();
        for (Tex& t : retiredBlurTexPrev) freeTex(t);
        retiredBlurTexPrev.clear();
    }

    // present 里等过**上一帧**的 fence 之后调用：上一帧淘汰的图现在没人引用了。
    // 本帧淘汰的推到"上一代"，再等一帧才真放（见 retiredBlurTex 的注释）。
    void freeRetiredBlurTex() {
        for (Tex& t : retiredBlurTexPrev) freeTex(t);
        retiredBlurTexPrev.clear();
        retiredBlurTexPrev.swap(retiredBlurTex);
        retiredBlurTex.clear();
    }

    BlurLevel& blurAt(size_t i) {
        static BlurLevel dummy;
        if (activeBlur < 0) return dummy;
        std::vector<BlurLevel>& lv = blurSets[static_cast<size_t>(activeBlur)].lv;
        if (i >= lv.size()) { dummy = BlurLevel(); return dummy; }
        return lv[i];
    }
    // 取一套以 (w0,h0) 为第 0 级的链：命中复用，否则新建（超 kMaxBlurSets 淘汰最久未用）。
    bool selectBlurSet(int w0, int h0) {
        if (w0 <= 0 || h0 <= 0) return false;
        for (size_t i = 0; i < blurSets.size(); i++) {
            if (blurSets[i].w0 == w0 && blurSets[i].h0 == h0) {
                blurSets[i].tick = ++blurTick;
                activeBlur = static_cast<int>(i);
                return true;
            }
        }
        if (blurSets.size() >= kMaxBlurSets) {
            size_t victim = 0;
            for (size_t i = 1; i < blurSets.size(); i++)
                if (blurSets[i].tick < blurSets[victim].tick) victim = i;
            for (BlurLevel& b : blurSets[victim].lv) {
                if (b.tex.image) retireBlurTex(b.tex);
                b.w = b.h = 0;
            }
            blurSets.erase(blurSets.begin() + static_cast<long>(victim));
        }
        BlurSet s;
        s.w0 = w0;
        s.h0 = h0;
        s.tick = ++blurTick;
        s.lv.resize(static_cast<size_t>(kMaxBlurLevels));
        blurSets.push_back(std::move(s));
        activeBlur = static_cast<int>(blurSets.size()) - 1;
        return true;
    }

    bool ensureBlurLevel(size_t i, int bw, int bh) {
        if (i >= static_cast<size_t>(kMaxBlurLevels) || bw <= 0 || bh <= 0) return false;
        if (activeBlur < 0) return false;
        BlurLevel& b = blurAt(i);
        if (b.tex.image && b.w == bw && b.h == bh) return true;
        if (b.tex.image) { retireBlurTex(b.tex); b.w = b.h = 0; }
        // 与层目标同一套格式/采样数（单采样：图元都是解析式覆盖率的 quad，层和
        // 降采样目标都没有 MSAA —— 与 GL 的 FBO 也一致）。多用 TRANSFER_DST：
        // 背景要被 vkCmdCopyImage / vkCmdResolveImage 写进来；再用 SAMPLED 被采样。
        static const bool traceLvl = std::getenv("BINUI_BLUR_TRACE") != nullptr;
        if (traceLvl) std::fprintf(stderr, "[vk][blurlevel] create %zu %dx%d\n", i, bw, bh);
        Tex& t = b.tex;
        if (!createImage2D(bw, bh, kColorFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_SAMPLE_COUNT_1_BIT, t.image, t.mem, VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        t.w = bw;
        t.h = bh;
        t.linear = true;       // 降采样与放大都要双线性（GL 那边的纹理也是 LINEAR）
        t.needClear = false;   // 内容由 LOAD_OP_CLEAR / 那次拷贝负责
        t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.view = makeView(t.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.view || !allocTexDs(t)) {
            std::fprintf(stderr, "[vk] 毛玻璃降采样图创建失败（%dx%d）\n", bw, bh);
            retireBlurTex(t);
            return false;
        }
        b.w = bw;
        b.h = bh;
        return true;
    }

    // 降采样图的三种用途之间的显式依赖（动态渲染没有 subpass 依赖表）：
    // 附件（被写）↔ 只读（被下一级/合成采样）↔ TRANSFER_DST（被拷入背景）
    void blurLevelToAttachment(BlurLevel& b) {
        Tex& t = b.tex;
        if (t.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            barrierAttachment(t.image);
            return;
        }
        VkAccessFlags srcA = 0;
        VkPipelineStageFlags srcS = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            srcA = VK_ACCESS_SHADER_READ_BIT;
            srcS = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (t.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            srcA = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcS = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        barrier(t.image, t.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcA,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, srcS,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    void blurLevelToSampled(BlurLevel& b) {
        Tex& t = b.tex;
        if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
        VkAccessFlags srcA = 0;
        VkPipelineStageFlags srcS = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (t.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            srcA = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcS = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (t.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            srcA = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcS = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        barrier(t.image, t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcA,
                VK_ACCESS_SHADER_READ_BIT, srcS, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    void blurLevelToTransferDst(BlurLevel& b) {
        Tex& t = b.tex;
        if (t.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;
        VkAccessFlags srcA = 0;
        VkPipelineStageFlags srcS = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (t.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            srcA = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcS = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            srcA = VK_ACCESS_SHADER_READ_BIT;
            srcS = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        barrier(t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcA,
                VK_ACCESS_TRANSFER_WRITE_BIT, srcS, VK_PIPELINE_STAGE_TRANSFER_BIT);
        t.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    void copyImageRegion(VkImage src, int sx, int sy, VkImage dst, int cw, int ch) {
        VkImageCopy c{};
        c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.srcSubresource.layerCount = 1;
        c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.dstSubresource.layerCount = 1;
        c.srcOffset = {sx, sy, 0};
        c.dstOffset = {0, 0, 0};
        c.extent = {static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), 1};
        g_vk.vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }

    // 抓背景：当前目标（layerIdx >= 0 = 层目标；否则主目标）的 (rx,ry,rw,rh) → level0。
    // 调用前当前渲染作用域必须已经结束（附件要转成 TRANSFER_SRC）。
    // 主目标 = 4x MSAA 时先 resolve 整帧（与 present 同一条命令）再拷这一小块：
    // GL 的 glBlitFramebuffer(MSAA → 纹理) 在硬件上做的正是同一个"平均 4 个样本"。
    void captureBackdrop(int layerIdx, int rx, int ry, int rw, int rh) {
        BlurLevel& l0 = blurAt(0);
        blurLevelToTransferDst(l0);
        if (layerIdx >= 0) {
            Tex& lt = layers[static_cast<size_t>(layerIdx)].tex;
            barrier(lt.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageRegion(lt.image, rx, ry, l0.tex.image, rw, rh);
            barrier(lt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            lt.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            return;
        }
        if (samples == VK_SAMPLE_COUNT_1_BIT) {
            barrier(msaaImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageRegion(msaaImage, rx, ry, l0.tex.image, rw, rh);
            barrier(msaaImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            return;
        }
        barrier(msaaImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageResolve rgn{};
        rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn.srcSubresource.layerCount = 1;
        rgn.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn.dstSubresource.layerCount = 1;
        rgn.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        g_vk.vkCmdResolveImage(cmd, msaaImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               resolveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
        barrier(msaaImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        // resolve 图的常驻布局是 TRANSFER_DST（present 每帧都往它 resolve），拷完要还原
        barrier(resolveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        copyImageRegion(resolveImage, rx, ry, l0.tex.image, rw, rh);
        barrier(resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    // 降采样目标：整级清透明 + viewport/push constant 换成这一级的尺寸。
    // 与 setPassState 的区别只有尺寸（这里是"小图局部坐标系"）。
    void beginBlurLevelPass(BlurLevel& b) {
        VkRenderingAttachmentInfo ca =
            makeAttachment(b.tex.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_ATTACHMENT_LOAD_OP_CLEAR);
        ca.clearValue.color.float32[0] = 0.0f;
        ca.clearValue.color.float32[1] = 0.0f;
        ca.clearValue.color.float32[2] = 0.0f;
        ca.clearValue.color.float32[3] = 0.0f;
        VkRenderingInfo ri = makeRenderingInfo(&ca, 0, 0, b.w, b.h);
        g_vk.vkCmdBeginRendering(cmd, &ri);
        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(b.w);
        vp.height = static_cast<float>(b.h);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        g_vk.vkCmdSetViewport(cmd, 0, 1, &vp);
        float vs[2] = {static_cast<float>(b.w), static_cast<float>(b.h)};
        if (pipeLayout)
            g_vk.vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, vs);
        refreshPipeline();   // blurReplace + blurForceSingle → 单采样 replace 变体
    }

    // 毛玻璃主流程（算法见 binui_gfx.h 与 GL 后端的同名函数，这里逐条对应：
    //   1) 区域取整裁窗口  2) 抓背景到 level0  3) 逐级 2× 降采样  4) 圆角条带 replace 合成）
    bool blurBackdrop(float x, float y, float fw, float fh, float radius, float blurPx,
                      float grain) {
        if (!ready || !useDynamic || !cmdOpen) return false;
        if (!(blurPx > 0.0f) || fw < 2.0f || fh < 2.0f) return false;
        int rx = static_cast<int>(std::floor(x));
        int ry = static_cast<int>(std::floor(y));
        int rx1 = static_cast<int>(std::ceil(x + fw));
        int ry1 = static_cast<int>(std::ceil(y + fh));
        rx = std::max(0, rx);
        ry = std::max(0, ry);
        rx1 = std::min(w, rx1);
        ry1 = std::min(h, ry1);
        const int rw = rx1 - rx, rh = ry1 - ry;
        if (rw < 2 || rh < 2) return false;

        int levels = 0;
        {
            // v0.16.44：与 filter: blur 同一个取法（降到 σ 还剩 ≥2 纹素那一级，剩下的交给
            // 分离高斯），三端逐字一致
            while (levels < 5 && blurPx / static_cast<float>(1 << (levels + 1)) >= 2.0f) levels++;
        }

        // 手上攒的图元先落到**当前**目标：它们构成"背景"
        flush();

        const bool inLayer = !layerStack.empty();
        const int li = inLayer ? layerStack.back().idx : -1;
        const int sl = clipL, st = clipT, sr = clipR, sb = clipB;
        const bool ignWas = ignoreLayerClip;

        // 结束当前渲染作用域：附件既要当拷贝源，又要把画面切到小图上画降采样。
        // 动态渲染的附件布局全由我们记账，所以这一进一出必须先手工收尾。
        if (inLayer) endLayerPass();
        else endMainPass();

        auto resumeTarget = [&] {
            if (inLayer) beginLayerPass(li, true, layerStack.back());
            else beginMainPass(true);
        };

        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) {
            resumeTarget();
            return false;
        }

        // 1) 抓背景 → level0
        captureBackdrop(inLayer ? li : -1, rx, ry, rw, rh);

        // 2) 降采样链：每级清透明 + 一个覆盖整级的四边形采样上一级（双线性 =
        //    目标像素中心落在上一级的 2×2 中心 → 盒式平均）。VK 纹理行 0 = 区域顶部，
        //    所以 UV 是 (0,0)-(1,1)，不翻转（GL 那边要翻是因为它的 FBO 行 0 在底部）。
        bool chainOk = true;
        blurReplace = true;
        blurForceSingle = true;
        for (int i = 1; i <= levels && chainOk; i++) {
            const size_t pi = static_cast<size_t>(i - 1);
            const int dw = std::max(1, blurAt(pi).w / 2);
            const int dh = std::max(1, blurAt(pi).h / 2);
            if (!ensureBlurLevel(static_cast<size_t>(i), dw, dh)) { chainOk = false; break; }
            blurLevelToSampled(blurAt(pi));          // 上一级 → 只读（在渲染作用域外转）
            blurLevelToAttachment(blurAt(static_cast<size_t>(i)));
            beginBlurLevelPass(blurAt(static_cast<size_t>(i)));
            // 坐标是"小图局部"的：层框/窗口裁剪都不适用（GL 的 ignoreLayerClip 同理），
            // 批次 scissor 必须换成整级 —— 否则会被节点裁剪盒整块裁掉。
            clipL = 0;
            clipT = 0;
            clipR = dw;
            clipB = dh;
            ignoreLayerClip = true;
            emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                     0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu, &blurAt(pi).tex);
            flush();
            g_vk.vkCmdEndRendering(cmd);
            ignoreLayerClip = false;
        }
        blurForceSingle = false;
        clipL = sl;
        clipT = st;
        clipR = sr;
        clipB = sb;

        // v0.16.44：末级上跑横/纵分离高斯（三端同一条算法；失败就保持"只降采样"的老观感）
        if (chainOk) gaussBlurLevel(levels, blurPx);

        // 3) 合成：BuildImageBands 的圆角条带 + 双线性放大 + replace 混合。
        //    这里必须**换回原来的 scissor**（节点自己的裁剪），毛玻璃不能越出节点框。
        if (chainOk) {
            blurLevelToSampled(blurAt(static_cast<size_t>(levels)));
            blurReplace = true;   // gaussBlurLevel 退场时把混合还原了，合成趟要重新置上
            resumeTarget();
            refreshPipeline();   // replace 变体（按当前目标：主目标 4x / 层目标 1x）
            binui::PaintImageBand bands[binui::kMaxImageBands];
            const int nb = binui::BuildImageBands(x, y, fw, fh, radius, bands);
            for (int q = 0; q < nb; q++) {
                const binui::PaintImageBand& b = bands[q];
                if (b.w <= 0.0f || b.h <= 0.0f) continue;
                emitQuad(b.x, b.y, b.x + b.w, b.y + b.h, b.u0, b.v0, b.u1, b.v1,
                         0xFFFFFFFFu, &blurAt(static_cast<size_t>(levels)).tex);
            }
            flush();
        }

        // 4) 先把混合恢复成正常预乘（ONE / ONE_MINUS_SRC_ALPHA）：颗粒是"叠上去"的，
        //    留在 replace 混合里画会把整块玻璃**替换**成噪声（实拍：卡片变成近乎透明的
        //    白点，alpha 掉到 0.45 左右）。GL/D3D11 都是把恢复提前到颗粒趟之前。
        blurReplace = false;
        blurForceSingle = false;
        refreshPipeline();
        ignoreLayerClip = ignWas;
        clipL = sl;
        clipT = st;
        clipR = sr;
        clipB = sb;
        if (!chainOk) {
            resumeTarget();   // 链失败时当前目标还没恢复
        } else if (grain > 0.001f && noise.image) {
            // 5) 颗粒（grain）：一层极淡的确定性噪声，压住大块模糊面的 banding。
            //    屏幕锚定 + 一个纹素 = 一个屏幕像素（u = x/64, v = y/64），元素移动时
            //    颗粒不跟着滑；NEAREST + REPEAT 采样保证三端取到同一个纹素。
            const float k = 1.0f / 64.0f;
            // 顶点色：alpha = grain * 3.0（三端同一个常数），rgb = 白。与 GL / D3D11 的
            // ncol 逐字一致 —— 注意**不做预乘**（两端都是直接 (av<<24)|0x00FFFFFF）。
            const uint32_t av = static_cast<uint32_t>(grain * 3.0f + 0.5f);
            const uint32_t ncol = (av << 24) | 0x00FFFFFFu;
            binui::PaintImageBand gb[binui::kMaxImageBands];
            const int ng = binui::BuildImageBands(x, y, fw, fh, radius, gb);
            for (int q = 0; q < ng; q++) {
                const binui::PaintImageBand& b = gb[q];
                if (b.w <= 0.0f || b.h <= 0.0f) continue;
                // 不要再加 0.5 纹素：光栅化取的是**像素中心**（x+0.5），u = x/64 时像素
                // 中心正好落在纹素中心；再加半个纹素就落到边界上，NEAREST 取哪个纹素
                // 由实现决定（GL 与 D3D11 会整块不一致 —— 实测过）。
                emitQuad(b.x, b.y, b.x + b.w, b.y + b.h, b.x * k, b.y * k,
                         (b.x + b.w) * k, (b.y + b.h) * k, ncol, &noise);
            }
            flush();
        }
        switchBatch(&white);

        static const bool trace = std::getenv("BINUI_VK_TRACE") != nullptr;
        if (trace)
            std::fprintf(stderr,
                         "[vk][trace] backdropBlur: 区域 %d,%d %dx%d 级数 %d 颗粒 %.2f "
                         "结果 %d（%s目标）\n",
                         rx, ry, rw, rh, levels, grain, chainOk ? 1 : 0, inLayer ? "层" : "主");
        return chainOk;
    }

    // 异常路径（上一帧有层没闭合）：层图内容不重要，池子的占用标记必须复位
    void abandonLayers() {
        for (const LayerFrame& f : layerStack)
            if (f.idx >= 0 && f.idx < static_cast<int>(layers.size())) layers[f.idx].inUse = false;
        layerStack.clear();
    }

    // 层图布局：附件（被写）↔ 只读（被合成四边形采样）
    void layerToAttachment(int idx, bool resume) {
        Tex& t = layers[idx].tex;
        if (t.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            if (resume) barrierAttachment(t.image);
            return;
        }
        VkAccessFlags srcA = 0;
        VkPipelineStageFlags srcS = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            srcA = VK_ACCESS_SHADER_READ_BIT;
            srcS = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        barrier(t.image, t.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcA,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, srcS,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    void layerToSampled(int idx) {
        Tex& t = layers[idx].tex;
        if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
        barrier(t.image, t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    void beginLayerPass(int idx, bool resume, const LayerFrame& f) {
        VkRenderingAttachmentInfo ca =
            makeAttachment(layers[idx].tex.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           resume ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR);
        // 层从"全透明"开始：CLEAR 覆盖整个 renderArea（= 层框，见下），
        // 所以层框内的起点一定是透明的 —— 合成时乘 alpha 不会带上脏内容。
        ca.clearValue.color.float32[0] = 0.0f;
        ca.clearValue.color.float32[1] = 0.0f;
        ca.clearValue.color.float32[2] = 0.0f;
        ca.clearValue.color.float32[3] = 0.0f;
        // renderArea 只取层框：clear 的代价跟着**元素面积**走，而不是窗口面积
        //（1280×720 的窗口大小层目标，全窗 clear 每层要多花 ~0.9 Mpx 的填充）。
        // 层框外既没人写也没人采样（合成四边形按层框 scissor），所以不用管它。
        VkRenderingInfo ri = makeRenderingInfo(&ca, f.l, f.t, f.r, f.b);
        g_vk.vkCmdBeginRendering(cmd, &ri);
        refreshPipeline();
        setPassState();
    }

    void endLayerPass() { g_vk.vkCmdEndRendering(cmd); }

    // 组透明度：把"一个节点 + 它的子树"画进离屏层，再整组乘一次 alpha 合成回来。
    // 逐元素乘 alpha 做不到这件事 —— 重叠的孩子会被乘两次（父色暗一档、子色偏色），
    // 那正是 v0.16.8 留下、v0.16.9/v0.16.10 用图层修掉的近似。
    bool pushLayer(float x, float y, float lw, float lh, float alpha, float blur = 0.0f,
                   const BinUIFilterDropShadowEntry* shadows = nullptr, int shadowCount = 0) {
        if (!ready || !useDynamic || !cmdOpen) return false;
        if (!(lw > 0.0f) || !(lh > 0.0f) || alpha <= 0.0f) return false;   // v27：alpha=1 也允许（裁剪用容器层）
        if (static_cast<int>(layerStack.size()) >= kMaxLayers) { warnLayerLimit(); return false; }
        // 顺序很重要：先把主目标上攒着的图元录到**当前**目标，再去切目标 ——
        // 反过来的话它们会画进刚清空的层里，然后被合成四边形整块盖掉。
        flush();
        const int idx = acquireLayer();
        if (idx < 0) { warnLayerLimit(); return false; }
        LayerFrame f;
        f.idx = idx;
        f.alpha = alpha;
        f.blur = blur;
        if (shadows && shadowCount > 0) f.shadows.assign(shadows, shadows + shadowCount);
        f.x = x; f.y = y; f.w = lw; f.h = lh;   // 浮点框：合成四边形用（见 LayerFrame）
        f.pl = clipL; f.pt = clipT; f.pr = clipR; f.pb = clipB;
        // 层框 = 元素框 ∩ 当前裁剪（层外的东西本来也不该显示）
        f.l = std::max(f.pl, static_cast<int>(std::floor(x)));
        f.t = std::max(f.pt, static_cast<int>(std::floor(y)));
        f.r = std::min(f.pr, static_cast<int>(std::ceil(x + lw)));
        f.b = std::min(f.pb, static_cast<int>(std::ceil(y + lh)));
        if (layerStack.empty()) endMainPass();
        else endLayerPass();
        layerToAttachment(idx, false);
        beginLayerPass(idx, false, f);
        clipL = f.l; clipT = f.t; clipR = f.r; clipB = f.b;   // 层内只画层框里的内容
        layers[idx].inUse = true;
        layerStack.push_back(f);
        refreshPipeline();   // ★ 必须在这里再选一次：上面 beginLayerPass 时 layerStack 还是空的，
                             // refreshPipeline 会按"主目标"选 4× MSAA 主管线，而层目标是单采样
        return true;
    }

    // 级数与抽头算法在 include/binui_paint.h（PaintFilterBlurLevel / PaintFilterGaussTaps）——
    // 三端与单元测试共用一份实现，这里只留别名，别再各写一遍（以前三份拷贝，改一处漏两处）。
    static int blurLevelsFor(float blurPx) { return binui::PaintFilterBlurLevel(blurPx); }
    static int gaussTaps(float sigmaTexels, uint32_t* w, int maxN) {
        return binui::PaintFilterGaussTaps(sigmaTexels, w, maxN);
    }

    bool gaussBlurLevel(int level, float blurPx) {
        if (level < 0 || level >= kMaxBlurLevels - 1) return false;   // 末号留给暂存
        const size_t li = static_cast<size_t>(level);
        if (activeBlur < 0) return false;
        const int lw = blurAt(li).w, lh = blurAt(li).h;
        const float sigmaPx = blurPx / static_cast<float>(1 << level);
        if (!blurAt(li).tex.image || lw < 1 || lh < 1 || !(sigmaPx > 0.0f)) return false;
        const size_t ti = static_cast<size_t>(kMaxBlurLevels - 1);
        if (!ensureBlurLevel(ti, lw, lh)) return false;
        uint32_t w[33];
        const int n = gaussTaps(sigmaPx, w, 33);
        const int r = (n - 1) / 2;
        static const bool trace = std::getenv("BINUI_BLUR_TRACE") != nullptr;
        if (trace)
            std::fprintf(stderr, "[vk][gauss] 级 %d %dx%d σ=%.2f 纹素 抽头 %d 个\n",
                         level, lw, lh, sigmaPx, n);
        blurForceSingle = true;   // 小图局部坐标系：强制用单采样变体
        blurReplace = false;
        addOn = true;
        const int sl = clipL, st = clipT, sr = clipR, sb = clipB;
        const bool ignWas = ignoreLayerClip;
        for (int pass = 0; pass < 2; pass++) {
            const size_t di = pass == 0 ? ti : li;
            const size_t fi = pass == 0 ? li : ti;
            // 布局切换必须在渲染作用域外：源 → 只读，目标 → 附件
            blurLevelToSampled(blurAt(fi));
            blurLevelToAttachment(blurAt(di));
            beginBlurLevelPass(blurAt(di));   // 清透明 + 小图视口 + 加法变体
            clipL = 0; clipT = 0; clipR = lw; clipB = lh;
            ignoreLayerClip = true;
            for (int k = -r; k <= r; k++) {
                const float o = static_cast<float>(k);
                uint32_t c = (w[k + r] << 24) | 0x00FFFFFFu;
                Premult(c);
                // VK 的纹理行 0 = 区域顶部，UV 正着来（GL 那边要翻 v）
                if (pass == 0)
                    emitQuad(0.0f, 0.0f, static_cast<float>(lw), static_cast<float>(lh),
                             o / static_cast<float>(lw), 0.0f,
                             1.0f + o / static_cast<float>(lw), 1.0f, c, &blurAt(fi).tex);
                else
                    emitQuad(0.0f, 0.0f, static_cast<float>(lw), static_cast<float>(lh),
                             0.0f, o / static_cast<float>(lh),
                             1.0f, 1.0f + o / static_cast<float>(lh), c, &blurAt(fi).tex);
            }
            flush();
            g_vk.vkCmdEndRendering(cmd);
            ignoreLayerClip = false;
        }
        addOn = false;
        blurForceSingle = false;
        ignoreLayerClip = ignWas;   // 裁剪区还给调用方（合成/颗粒趟还要用）
        clipL = sl; clipT = st; clipR = sr; clipB = sb;
        blurLevelToSampled(blurAt(li));
        return true;
    }

    // 区域 = **层框 ∩ 进入时的裁剪**，也就是 VK 真正清过的那块（beginLayerPass 的
    // renderArea）。不能只按层框算：局部重绘时层框比脏矩形大，框外那块层纹理从没被
    // 清过（VK 只清 renderArea），照抄进来就会把上一帧的残影拖进模糊/阴影里。
    // 全帧绘制时两者相等（裁剪就是窗口），所以像素门禁上三端仍然逐像素一致。
    bool layerRegion(const LayerFrame& f, int& rx, int& ry, int& rw, int& rh) const {
        rx = std::max(0, f.l);
        ry = std::max(0, f.t);
        const int rx1 = std::min(w, f.r);
        const int ry1 = std::min(h, f.b);
        rw = rx1 - rx;
        rh = ry1 - ry;
        return rw >= 2 && rh >= 2;
    }

    // 从 level0 往下建 1..levels 级（与 blurBackdrop 逐字一致：VK 的行 0 = 区域顶部，
    // UV 不翻转），再在末级上跑分离高斯。返回最终可用的级号。
    int buildChain(float blurPx) {
        const int levels = blurLevelsFor(blurPx);
        int finalLevel = 0;
        if (levels <= 0) return 0;
        blurReplace = true;
        blurForceSingle = true;
        for (int i = 1; i <= levels; i++) {
            const size_t pi = static_cast<size_t>(i - 1);
            const int dw = std::max(1, blurAt(pi).w / 2);
            const int dh = std::max(1, blurAt(pi).h / 2);
            if (!ensureBlurLevel(static_cast<size_t>(i), dw, dh)) break;
            blurLevelToSampled(blurAt(pi));
            blurLevelToAttachment(blurAt(static_cast<size_t>(i)));
            beginBlurLevelPass(blurAt(static_cast<size_t>(i)));
            clipL = 0; clipT = 0; clipR = dw; clipB = dh;
            ignoreLayerClip = true;
            emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh),
                     0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu, &blurAt(pi).tex);
            flush();
            g_vk.vkCmdEndRendering(cmd);
            ignoreLayerClip = false;
            finalLevel = i;
        }
        blurReplace = false;
        blurForceSingle = false;
        blurLevelToSampled(blurAt(static_cast<size_t>(finalLevel)));
        // v0.16.44：末级上跑横/纵分离高斯（失败就保持"只降采样"的老观感）
        gaussBlurLevel(finalLevel, blurPx);
        return finalLevel;
    }

    // 回到父目标（还有外层就回外层，否则回主目标），并把裁剪还原成进入前的
    void resumeParent(const LayerFrame& f) {
        if (layerStack.empty()) beginMainPass(true);
        else beginLayerPass(layerStack.back().idx, true, layerStack.back());
        clipL = f.pl; clipT = f.pt; clipR = f.pr; clipB = f.pb;
        refreshPipeline();   // 按当前目标的采样数选变体（主目标 4x / 层目标 1x）
    }

    // 把某一级按**正常 alpha** 合成到当前目标（当前目标必须已经恢复好）
    void compositeLevel(int level, int x, int y, int rw, int rh, uint32_t col) {
        emitQuad(static_cast<float>(x), static_cast<float>(y),
                 static_cast<float>(x + rw), static_cast<float>(y + rh),
                 0.0f, 0.0f, 1.0f, 1.0f, col,
                 &blurAt(static_cast<size_t>(level)).tex);
        flush();
    }

    // filter: blur —— 抓层内容 → 降采样链 → 正常 alpha 合成回原区域。
    // 成功返回 true 并已经把结果合成掉了（作用域保持打开）。
    bool blurLayerContent(const LayerFrame& f) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!layerRegion(f, rx, ry, rw, rh)) return false;
        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) return false;
        static const bool traceBlur = std::getenv("BINUI_BLUR_TRACE") != nullptr;
        if (traceBlur)
            std::fprintf(stderr, "[vk][blur] 层框 %.2f,%.2f %.2fx%.2f 抓取 %d,%d %dx%d 级数 %d\n",
                         f.x, f.y, f.w, f.h, rx, ry, rw, rh, blurLevelsFor(f.blur));
        // 抓层内容 → level0（层纹理此时是只读：popLayer 一进来就 layerToSampled 了）
        {
            Tex& lt = layers[static_cast<size_t>(f.idx)].tex;
            setTexLayout(lt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            blurLevelToTransferDst(blurAt(0));
            copyImageRegion(lt.image, rx, ry, blurAt(0).tex.image, rw, rh);
            setTexLayout(lt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        const int finalLevel = buildChain(f.blur);
        resumeParent(f);
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        uint32_t col = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        Premult(col);
        compositeLevel(finalLevel, rx, ry, rw, rh, col);
        return true;
    }

    // filter: drop-shadow —— 一条外阴影。
    // 没有着色器就取不到"把 RGB 压成单色、只留轮廓"，所以用两趟混合凑出来：
    //   1) 阴影色（预乘）铺满 level0 整块区域；
    //   2) "保留混合"（dst *= srcA）把层内容画一遍 → 得到 (c.rgb*c.a*a, c.a*a)：上色的轮廓，
    //      形状来自元素自己的 alpha（星形图标的影子是星形，不是方块 —— 与 box-shadow 的本质区别）；
    //   3) 按阴影半径建链，再按偏移（取整）合成到元素下面。
    void drawDropShadow(const LayerFrame& f, const BinUIFilterDropShadowEntry& sh) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!layerRegion(f, rx, ry, rw, rh)) return;
        if (!selectBlurSet(rw, rh) || !ensureBlurLevel(0, rw, rh)) return;
        // 1) + 2)：在 level0 里造出"上色的轮廓"
        blurReplace = false;
        blurForceSingle = true;
        blurLevelToAttachment(blurAt(0));
        beginBlurLevelPass(blurAt(0));      // 清透明 + 小图视口 + 单采样变体
        clipL = 0; clipT = 0; clipR = rw; clipB = rh;
        ignoreLayerClip = true;
        {
            uint32_t c = sh.color;
            Premult(c);
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     0.5f, 0.5f, 0.5f, 0.5f, c, &white);
            flush();
            keepOn = true;
            refreshPipeline();
            const float u0 = static_cast<float>(rx) / static_cast<float>(w);
            const float u1 = static_cast<float>(rx + rw) / static_cast<float>(w);
            const float v0 = static_cast<float>(ry) / static_cast<float>(h);
            const float v1 = static_cast<float>(ry + rh) / static_cast<float>(h);
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     u0, v0, u1, v1, 0xFFFFFFFFu, &layers[static_cast<size_t>(f.idx)].tex);
            flush();
            keepOn = false;
        }
        g_vk.vkCmdEndRendering(cmd);
        ignoreLayerClip = false;
        blurForceSingle = false;
        // 3) 偏移取整（合成四边形恒落在像素边界上，三端一致）
        const int sdx = static_cast<int>(std::llround(sh.dx));
        const int sdy = static_cast<int>(std::llround(sh.dy));
        const int finalLevel = buildChain(sh.blur);
        if (std::getenv("BINUI_LV_DUMP")) {
            static int dn = 0;
            if (dn < 2) {
                /* 用读回到 buffer 的方式导出（VK 没有 glReadPixels） */
            }
            dn++;
        }
        resumeParent(f);
        if (std::getenv("BINUI_BLUR_TRACE"))
            std::fprintf(stderr, "[vk][shadow] region %d,%d %dx%d 级数 %d 偏移 %d,%d 层框 %.1f,%.1f %.1fx%.1f\n",
                         rx, ry, rw, rh, finalLevel, sdx, sdy, f.x, f.y, f.w, f.h);
        compositeLevel(finalLevel, rx + sdx, ry + sdy, rw, rh, 0xFFFFFFFFu);
    }

    void popLayer() {
        if (layerStack.empty()) return;
        flush();                       // 层内容落盘（flush 按批次 scissor 裁剪，已在层框内）
        const LayerFrame f = layerStack.back();
        layerStack.pop_back();
        endLayerPass();
        // 层纹理接下来要当采样源（阴影轮廓 + 本体合成都要采它）
        layerToSampled(f.idx);
        // v0.16.43：外阴影先铺（书写顺序里靠前的在上层 → 逆序画）。
        // 每条阴影各自"恢复目标 → 合成 → 关掉作用域"，这样下一条的降采样链才能重新开场。
        for (int i = static_cast<int>(f.shadows.size()) - 1; i >= 0; i--) {
            drawDropShadow(f, f.shadows[static_cast<size_t>(i)]);
            g_vk.vkCmdEndRendering(cmd);
        }
        // v0.16.42：模糊层 —— 糊掉层内容并合成掉；成功就直接收工
        if (f.blur > 0.0f && blurLayerContent(f)) {
            layers[static_cast<size_t>(f.idx)].inUse = false;
            switchBatch(&white);
            return;
        }
        // 回到父目标：还有外层就回外层，否则回主目标
        resumeParent(f);
        // 合成：四边形就取**层框**本身，UV 用像素坐标除以窗口尺寸。
        // 这样两个性质同时成立（GL/D3D11 同一套写法）：
        //   · 采样是严格 1:1 的 —— 屏幕像素中心 i+0.5 折算过去正好是纹素中心 i+0.5，
        //     所以层框落在非整数坐标也不会把内容糊掉；
        //   · 层框的小数边界由**主目标的 4x MSAA 覆盖率**做抗锯齿（整窗四边形就没有这个
        //     边缘了，组边界会变成 1px 台阶 —— v0.16.12 第一版就是这么错的，
        //     用 translate(10.5px) 的动画用例比出来的）。
        const float a = f.alpha < 0.0f ? 0.0f : (f.alpha > 1.0f ? 1.0f : f.alpha);
        // 诊断开关：BINUI_VK_LAYER_OPAQUE=1 时合成按 alpha=1 画，用来直接看"层里到底存了什么"
        static const bool dbgOpaque = std::getenv("BINUI_VK_LAYER_OPAQUE") != nullptr;
        const float aFinal = dbgOpaque ? 1.0f : a;
        uint32_t col = (static_cast<uint32_t>(aFinal * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        // 着色器算 (t.rgb * vcol.rgb, t.a * vcol.a)：层里存的是预乘色，
        // 顶点色不预乘的话 rgb 会被整块加进去（和 GL/D3D11 同一个坑）。
        Premult(col);
        const float fx = f.x, fy = f.y;
        const float fw = f.w, fh = f.h;
        emitQuad(fx, fy, fx + fw, fy + fh, fx / static_cast<float>(w), fy / static_cast<float>(h),
                 (fx + fw) / static_cast<float>(w), (fy + fh) / static_cast<float>(h), col,
                 &layers[static_cast<size_t>(f.idx)].tex);
        flush();
        layers[static_cast<size_t>(f.idx)].inUse = false;
        switchBatch(&white);           // 把当前纹理换回纯白（flush 已清空 cur）
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
    // 实例版本协商：1.3 才允许把 VkPhysicalDeviceVulkan13Features 挂进设备创建链
    //（动态渲染 = 图层的前提）。老 loader 没有 vkEnumerateInstanceVersion，直接留 1.0。
    auto enumInstVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        g_vk.getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumInstVer) {
        uint32_t supported = VK_API_VERSION_1_0;
        if (enumInstVer(&supported) == VK_SUCCESS && supported >= VK_API_VERSION_1_3)
            app.apiVersion = VK_API_VERSION_1_3;
    }
    d.instApiVersion = app.apiVersion;
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
    // v0.16.28 颗粒噪声（一次性提交）。失败不致命：毛玻璃照画，只是没有颗粒。
    if (!d.createNoise()) std::fprintf(stderr, "[vk] createNoise 返回 false（毛玻璃颗粒将缺席）\n");

    // 字形图集：排版/光栅化在共享头里，本类自己当纹理宿主
    d.atlas.setHost(&d);
    d.atlas.initFonts();

    d.ready = true;
    d.coversMonitor = d.windowMode ? d.clientCoversMonitor() : false;
    std::fprintf(stderr, "[vk] %s就绪 %dx%d（MSAA %dx，动态渲染%s）\n", d.windowMode ? "窗口" : "离屏",
                 width, height, static_cast<int>(d.samples),
                 d.useDynamic ? "开（组透明度可用）" : "关（组透明度退回逐元素近似）");
    if (d.windowMode && d.useSwapchain)
        std::fprintf(stderr, "[vk] 交换链 %u 图，呈现模式 %s，present_wait %s\n",
                     d.swapImageCount, d.vsync ? "FIFO(vsync)" : "不等待垂直同步",
                     d.presentWaitAvailable ? "可用（等真上屏）" : "不可用（退回 DwmFlush）");
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
    d.destroyLayers();   // 层图先放（它们的 Tex 也占 descriptor set 和显存）
    d.destroyBlurLevels();   // v29：毛玻璃降采样图同理（必须在描述符池之前放）
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
    d.rawMips.clear();
    d.pendingUploads.clear();
    d.freeTex(d.white);
    d.freeTex(d.noise);   // v0.16.28：颗粒噪声图
    for (VkDescriptorPool p : d.dsPools)
        if (p) g_vk.vkDestroyDescriptorPool(d.device, p, nullptr);
    d.dsPools.clear();
    d.dsPool = VK_NULL_HANDLE;
    if (d.samplerNearest) g_vk.vkDestroySampler(d.device, d.samplerNearest, nullptr);
    if (d.samplerLinear) g_vk.vkDestroySampler(d.device, d.samplerLinear, nullptr);
    if (d.samplerRepeat) g_vk.vkDestroySampler(d.device, d.samplerRepeat, nullptr);
    d.samplerNearest = d.samplerLinear = d.samplerRepeat = VK_NULL_HANDLE;
    if (d.pipeline) g_vk.vkDestroyPipeline(d.device, d.pipeline, nullptr);
    if (d.pipelineLayer) g_vk.vkDestroyPipeline(d.device, d.pipelineLayer, nullptr);
    if (d.pipelineErase) g_vk.vkDestroyPipeline(d.device, d.pipelineErase, nullptr);
    if (d.pipelineLayerErase)
        g_vk.vkDestroyPipeline(d.device, d.pipelineLayerErase, nullptr);
    if (d.pipelineKeep) g_vk.vkDestroyPipeline(d.device, d.pipelineKeep, nullptr);
    if (d.pipelineLayerKeep)
        g_vk.vkDestroyPipeline(d.device, d.pipelineLayerKeep, nullptr);
    if (d.pipelineReplace) g_vk.vkDestroyPipeline(d.device, d.pipelineReplace, nullptr);
    if (d.pipelineLayerReplace)
        g_vk.vkDestroyPipeline(d.device, d.pipelineLayerReplace, nullptr);
    if (d.pipelineAdd) g_vk.vkDestroyPipeline(d.device, d.pipelineAdd, nullptr);
    if (d.pipelineLayerAdd) g_vk.vkDestroyPipeline(d.device, d.pipelineLayerAdd, nullptr);
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
    // BINUI_RESIZE_TRACE=1：拖边框时每帧都重建交换链，得先知道时间花在哪一段
    static const bool trace = std::getenv("BINUI_RESIZE_TRACE") != nullptr;
    const auto tA = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    g_vk.vkDeviceWaitIdle(d.device);
    const auto tB = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    d.w = width;
    d.h = height;
    // 层目标是窗口大小的：尺寸一变就得全放掉，下次 pushLayer 按新尺寸重建
    d.destroyLayers();
    // 先重建交换链：readback buffer 要按"渲染目标与交换链的较大者"分配，
    // 反过来（先建目标后建交换链）会让 DPI 缩放时缓冲区偏小（P4 修的隐患）。
    if (d.windowMode && d.useSwapchain) {
        if (!d.createSwapchain(true)) std::fprintf(stderr, "[vk] resize: 交换链重建失败\n");
    }
    const auto tC = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!d.createTargets()) {
        d.ready = false;
        std::fprintf(stderr, "[vk] resize 后重建目标失败\n");
        return;
    }
    if (trace) {
        const auto tD = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr,
                     "[vk][resize] %dx%d  waitIdle=%.2f swapchain=%.2f targets=%.2f 合计=%.2f ms\n",
                     width, height, ms(tA, tB), ms(tB, tC), ms(tC, tD), ms(tA, tD));
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
    d.abandonLayers();   // 上一帧若有层没闭合（异常路径），池子占用标记在这里复位

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

    // 开始主目标（动态渲染或 render pass，见 beginMainPass）
    d.beginMainPass(false);

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
    d.flushBase = 0;
    d.batches.clear();
    d.cur = Impl::Batch{};
    d.switchBatch(&d.white);
}

// 仿射圆角矩形（v20）：局部九宫格每片按 xf 变换后发射（模板/几何与轴对齐版本同源）
void BinUIVK::fillRoundedXf(float w, float h, float radius, bool stroke, float thickness,
                            uint32_t color, const float xf[6]) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    const int layers = stroke ? std::max(1, static_cast<int>(std::lround(std::max(1.0f, thickness))))
                              : 1;
    auto xform = [&](float lx, float ly, float* out) {
        out[0] = xf[0] * lx + xf[1] * ly + xf[2];
        out[1] = xf[3] * lx + xf[4] * ly + xf[5];
    };
    Premult(color);
    int emitted = 0;
    for (int k = 0; k < layers; k++) {
        const float lx = static_cast<float>(k), ly = static_cast<float>(k);
        const float lw = w - 2.0f * k, lh = h - 2.0f * k;
        if (lw <= 1.0f || lh <= 1.0f) break;
        const float rk = rf - static_cast<float>(k);
        if (rk <= 0.0f) break;
        const int rri = std::max(1, static_cast<int>(std::lround(rk)));
        Impl::RoundTex& rt = d.roundFor(rri, stroke);
        if (!rt.tex.image) break;
        PaintQuad q[9];
        const int n = BuildRoundedNineSlice(lx, ly, lw, lh, rk, rt.t, q, 9);
        for (int i = 0; i < n; i++) {
            const PaintQuad& c = q[i];
            const float corners[8] = { c.x, c.y, c.x + c.w, c.y,
                                       c.x, c.y + c.h, c.x + c.w, c.y + c.h };
            const float uv[8] = { c.u0, c.v0, c.u1, c.v0, c.u0, c.v1, c.u1, c.v1 };
            float sp[8];
            for (int v = 0; v < 4; v++) xform(corners[v * 2], corners[v * 2 + 1], sp + v * 2);
            d.emitQuadUV(sp, uv, color, &rt.tex);
            emitted++;
        }
    }
    if (emitted == 0) {
        const float corners[8] = { 0, 0, w, 0, 0, h, w, h };
        const float uv[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        float sp[8];
        for (int v = 0; v < 4; v++) xform(corners[v * 2], corners[v * 2 + 1], sp + v * 2);
        d.emitQuadUV(sp, uv, color, &d.white);
    }
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
    // 防御：调用方忘了 popLayer 时先把层收干净 —— 否则主 pass 还开着，
    // 后面的 readback/blit 就都录在 pass 里面（整帧作废）。
    while (!d.layerStack.empty()) d.popLayer();
    d.flush();
    d.endMainPass();
    if (d.useDynamic) {
        // 动态渲染没有 render pass 的 finalLayout / 隐式 resolve，这两步自己来：
        //   1) MSAA → 单采样：显式 vkCmdResolveImage（一帧只做一次，不是每个 pass 一次）
        //   2) resolve 图转到 TRANSFER_SRC，交给下面的读回 / blit
        if (d.samples != VK_SAMPLE_COUNT_1_BIT) {
            d.barrier(d.msaaImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageResolve rgn{};
            rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rgn.srcSubresource.layerCount = 1;
            rgn.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rgn.dstSubresource.layerCount = 1;
            rgn.extent = {static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h), 1};
            g_vk.vkCmdResolveImage(d.cmd, d.msaaImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &rgn);
            // MSAA 图转回附件布局给下一帧用；resolve 图转到 TRANSFER_SRC 给拷贝/blit
            d.barrier(d.msaaImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            d.barrier(d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
    }

    // ---- 呈现前的 surface 复查（P4：DPI 变化 / 跨显示器移动 / 最小化）----
    // caps.currentExtent 由 surface 决定，客户区尺寸只是我们的"期望"；两者不一致说明
    // 窗口刚换过显示器/DPI 或用户改过尺寸，交换链必须先重建才能继续。
    //
    // 这里**不能**每帧查 caps：实测 vkGetPhysicalDeviceSurfaceCapabilitiesKHR 约 0.7 ms，
    // 120Hz 的帧预算只有 8.33 ms，光这一句就吃掉 8%。改成先比客户区（GetClientRect
    // 约 0.01 ms）：一致就什么都不做，不一致才去查 caps 确认，然后重建。
    if (d.windowMode && d.useSwapchain) {
        if (d.hwnd && IsIconic(d.hwnd)) {
            // 最小化：没有可呈现的像素，本帧丢掉但不报错（恢复时会重建）
            d.submitNoPresent();
            d.perfEndFrame(tPresent);
            return;
        }
        RECT crc{};
        if (d.hwnd && GetClientRect(d.hwnd, &crc)) {
            const int cw = crc.right - crc.left;
            const int ch = crc.bottom - crc.top;
            if (cw > 0 && ch > 0 &&
                (cw != static_cast<int>(d.swapExtent.width) ||
                 ch != static_cast<int>(d.swapExtent.height))) {
                VkSurfaceCapabilitiesKHR caps{};
                if (g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, d.surface, &caps) ==
                    VK_SUCCESS) {
                    const VkExtent2D ce = caps.currentExtent;
                    if (ce.width == 0 || ce.height == 0) {
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
        }
    }

    // 呈现路线（设计 §5.5）：
    //   A. 交换链可用 → resolve → blit 到交换链图像 → vkQueuePresentKHR
    //   B. 其余情况（透明窗口 / 远程会话 / 上层要求拖拽期走 GDI / 无交换链）→ resolve →
    //      读回 buffer → SetDIBitsToDevice 或 UpdateLayeredWindow
    // 两条路线用的是**同一张 resolve 图**，所以窗口模式与离屏模式的像素完全一致。
    // 远程会话（RDP）走 B 是实测结论：虚拟显示器上 A 会停帧（GL 0.9.1 同样处理）。
    // **"客户区铺满显示器"不再进 B**：那条例外来自"避免 DWM 直翻转停帧"，但 VK 一旦建过
    // 交换链，窗口就是 flipping 窗口，DWM 只合成交换链图像，GDI 贴屏的内容不上屏 ——
    // 表现为 resize 到全屏尺寸后画面停在旧帧 + 黑边（v0.19.1 修，见 docs/startup.md §5.2）。
    const bool gdiPath =
        !d.useSwapchain || d.transparent || d.forceGdi || RemoteSession();
    d.lastPresentWasGdi = gdiPath;

    uint32_t imageIndex = 0;
    VkImage swapImg = VK_NULL_HANDLE;
    if (!gdiPath) {
        // acquire 到底等多久（BINUI_VK_PERF 里分段打出来）：这是判断"present 自己会不会
        // 被合成器卡住"的唯一直接证据 —— 它决定了外壳还要不要 DwmFlush。
        const double ta = d.perfOn ? VkNowMs() : 0.0;
        const VkResult ar = g_vk.vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX,
                                                       slot.semAvail, VK_NULL_HANDLE, &imageIndex);
        if (d.perfOn) d.perfAcquire += VkNowMs() - ta;
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
        static int gdiN = 0;
        if (std::getenv("BINUI_RESIZE_TRACE") && gdiN++ < 8)
            std::fprintf(stderr, "[vk][gdi] 读回 %dx%d（capW/H=%d,%d）\n", d.w, d.h, d.capW, d.capH);
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
    // 拷完把 resolve 图转回下一帧的写入布局。两条路径的"常驻布局"不同：
    //   动态渲染 → TRANSFER_DST_OPTIMAL（下一帧由 vkCmdResolveImage 写它）
    //   render pass → COLOR_ATTACHMENT_OPTIMAL（下一帧 subpass 会再 resolve 进去）
    const VkImageLayout resolveRest =
        d.useDynamic ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    d.barrier(d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolveRest,
              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_vk.vkEndCommandBuffer(d.cmd);
    d.recording = false;
    d.cmdOpen = false;

    // ---- 串行化点 ----
    // 提交新一帧之前必须等上一帧的 fence：渲染目标（MSAA 累积图 / resolve 图）和纹理布局
    // 是帧间共享的，GPU 上不能同时有两帧在跑。**但录制已经并行了** —— beginFrame 到
    // 这里之间的 CPU 工作（遍历、排版、路径细分、顶点填充）都发生在上一帧还在 GPU 上跑的时候，
    // 这正是双槽换来的收益。放在提交前而不是 beginFrame，是为了把重叠窗口拉到最大。
    if (d.prevSlot >= 0 && d.prevSlot != d.slotIdx) d.waitSlot(d.slots[d.prevSlot]);
    // v29：上一帧的 fence 已经过了，上一帧淘汰的毛玻璃降采样图现在没人引用，可以真放。
    d.freeRetiredBlurTex();

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
    // VK_KHR_present_id：给这一帧一个编号，等会儿用来问"它上屏了吗"
    const uint64_t myPresentId = ++d.presentSeq;
    VkPresentIdKHR pid{};
    pid.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    pid.swapchainCount = 1;
    pid.pPresentIds = &myPresentId;
    if (d.presentWaitAvailable) pi.pNext = &pid;
    const double tp = d.perfOn ? VkNowMs() : 0.0;
    const VkResult pr = g_vk.vkQueuePresentKHR(d.queue, &pi);
    if (d.perfOn) d.perfPresCall += VkNowMs() - tp;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        d.recreateSwapchain();
    } else if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vkQueuePresentKHR 失败: %s (%d)\n", VkResultText(pr), (int)pr);
        if (pr == VK_ERROR_DEVICE_LOST) d.ready = false;
        d.useSwapchain = false;
    } else if (d.presentWaitAvailable && d.vsync && !d.forceGdi && !d.transparent &&
               !d.coversMonitor) {
        // v0.11.9：**等这一帧真的被合成器显示**再返回。
        // 为什么需要它：FIFO 的 vkQueuePresentKHR 并不阻塞，交换链图只在合成那一刻才被
        // 放掉。不等的话 CPU 可以提前排好下一帧，合成器在某些周期拿到两帧、某些周期一帧
        // 都没有 —— 帧率照样 120，肉眼却是"抖"（这正是只有 VK 抖、GL/D3D11 靠 DwmFlush
        // 对齐后不抖的原因）。present_wait 把"一帧一次合成"变成硬约束。
        // 超时 100ms：最小化/被遮挡时合成器可能长时间不合成，不能把线程挂死。
        const double tw = d.perfOn ? VkNowMs() : 0.0;
        const VkResult wr = g_vk.vkWaitForPresentKHR(d.device, d.swapchain, myPresentId,
                                                     100ull * 1000ull * 1000ull);
        if (d.perfOn) d.perfPresentWait += VkNowMs() - tw;
        if (wr == VK_ERROR_OUT_OF_DATE_KHR || wr == VK_ERROR_SURFACE_LOST_KHR)
            d.useSwapchain = false;
    }
    d.perfEndFrame(tPresent);
}

// 强制读回 + GDI 贴屏（拖拽 resize 期由上层调用；全屏直翻转守卫由 present() 内部处理）
void BinUIVK::presentViaGdi() {
    Impl& d = *impl_;
    if (!d.ready) return;
    // **交换链存在时不能用 GDI 贴屏。** Windows 上 VK 的 WSI 一旦为窗口建了交换链，这个窗口
    // 就成了 flipping 窗口，DWM 只合成交换链里的图 —— GDI 往窗口 DC 画的东西根本不上屏。
    // 实测（v0.19.x）：resize 到 2560×1440 铺满显示器后，读回 2560×1440、blit 返回全扫描线
    // 成功，屏幕却一直是上一帧 + 黑边；GL（没有交换链）与 D3D11 同尺寸都正常。
    // 所以"铺满显示器改走读回"这条守卫对 VK 只能在没有交换链时用（远程会话/透明窗等）。
    if (d.useSwapchain && !d.transparent && !RemoteSession()) {
        present();
        return;
    }
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
    // v0.11.8：几何半径用浮点，角片模板按最近的整数半径取（模板是位图，只能整数）。
    // 静态页面传进来本来就是整数，逐像素等价；缩放动画里半径必须连续，
    // 否则圆/胶囊的 2r 与 w 会周期性错开 1px，中间那几块一胀一缩。
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) { fillRect(x, y, w, h, color); return; }
    const int r = std::max(1, static_cast<int>(std::lround(rf)));
    Impl::RoundTex& rt = d.roundFor(r, false);
    if (!rt.tex.image) { fillRect(x, y, w, h, color); return; }
    PaintQuad q[9];
    const int n = BuildRoundedNineSlice(x, y, w, h, rf,
                                       static_cast<float>(rt.t), q, 9);
    if (n <= 0) { fillRect(x, y, w, h, color); return; }
    Premult(color);
    for (int i = 0; i < n; i++)
        d.emitQuad(q[i].x, q[i].y, q[i].x + q[i].w, q[i].y + q[i].h,
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color, &rt.tex);
}

// v30：四角带色四边形（任意角度渐变）。顶点写法与 emitQuadUV 一致，只是每个顶点
// 各自一个颜色 —— 顶点色是逐顶点属性，着色器不动（三端的顶点结构都是 20 字节）。
bool BinUIVK::fillGradientQuad(float x, float y, float w, float h, float radius,
                               float angleDeg, uint32_t from, uint32_t to) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0.0f || h <= 0.0f) return false;
    const float r = radius > 0.5f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f;
    Impl::Tex* tex = &d.white;
    int tpl = 0;
    if (r > 0.5f) {
        const int rr = std::max(1, static_cast<int>(std::lround(r)));
        Impl::RoundTex& rt = d.roundFor(rr, false);
        if (!rt.tex.image) return false;        // 没模板：交回条带路径（像素不变）
        tex = &rt.tex;
        tpl = rt.t;
    }
    binui::BinUIGradQuad q[9];
    const int n = binui::BuildGradientQuads(x, y, w, h, r, tpl, angleDeg, from, to, q, 9);
    if (n <= 0) return false;
    for (int i = 0; i < n; i++) {
        VkVertex* o = d.vertRoom(6);
        if (!o) return true;                    // 撞顶点硬上限：已告警，剩下的不发
        d.switchBatch(tex);
        o[0] = VkVertex{ q[i].x, q[i].y, q[i].c[0], q[i].u0, q[i].v0 };
        o[1] = VkVertex{ q[i].x + q[i].w, q[i].y, q[i].c[1], q[i].u1, q[i].v0 };
        o[2] = VkVertex{ q[i].x, q[i].y + q[i].h, q[i].c[2], q[i].u0, q[i].v1 };
        o[3] = o[2];
        o[4] = o[1];
        o[5] = VkVertex{ q[i].x + q[i].w, q[i].y + q[i].h, q[i].c[3], q[i].u1, q[i].v1 };
        d.vcount += 6;
        d.cur.count += 2;
    }
    return true;
}

void BinUIVK::fillLine(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    Impl& d = *impl_;
    if (!d.ready || width <= 0) return;
    // v0.17.1：非仿射时走 CPU 覆盖率条带（见 binui_paint.h；GL 用的是另一套 MSAA
    // 采样位置，斜线覆盖率会差一档，所以三端统一在 CPU 上算）。仿射保留原四边形。
    if (!d.affineOn) {
        const uint64_t lkey = binui::MakeLineBandKey(x0, y0, x1, y1, width, color);
        auto& lcache = binui::LineBandCache();
        auto lit = lcache.find(lkey);
        const std::vector<binui::PaintBand>* lbands = nullptr;
        if (lit != lcache.end()) {
            lbands = &lit->second;
        } else {
            static thread_local std::vector<binui::PaintBand> lineBands;
            static thread_local std::vector<uint16_t> lineCov;
            if (lineBands.size() < static_cast<size_t>(binui::kMaxPathBands))
                lineBands.resize(binui::kMaxPathBands);
            const int n = binui::BuildLineBandsAA(x0, y0, x1, y1, width, color,
                                                  lineBands.data(),
                                                  static_cast<int>(lineBands.size()), lineCov);
            if (lcache.size() >= binui::kLineBandCacheCap) lcache.clear();
            std::vector<binui::PaintBand>& slot = lcache[lkey];
            slot.assign(lineBands.begin(), lineBands.begin() + n);
            lbands = &slot;
        }
        for (size_t i = 0; i < lbands->size(); i++)
            fillRect((*lbands)[i].x, (*lbands)[i].y, (*lbands)[i].w, (*lbands)[i].h,
                     (*lbands)[i].color);
        return;
    }
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float nx = -dy / len * width * 0.5f;
    const float ny = dx / len * width * 0.5f;
    uint32_t c = color;
    Premult(c);
    // 旋转四边形：两个三角形，法向偏移半线宽
    if (d.affineOn) {   // v0.16.18：线段也跟着仿射
        float p[8];
        d.affPoint(x0 + nx, y0 + ny, p[0], p[1]);
        d.affPoint(x1 + nx, y1 + ny, p[2], p[3]);
        d.affPoint(x1 - nx, y1 - ny, p[4], p[5]);
        d.affPoint(x0 - nx, y0 - ny, p[6], p[7]);
        const float uv[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        d.emitQuadUV(p, uv, c, &d.white);
        return;
    }
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
    // v0.11.8：几何半径用浮点（缩放动画要连续），模板按最近整数
    const float rf = std::min(radius, std::min(w, h) * 0.5f);
    if (rf <= 0.0f) {
        // 直角边框：上/下/左/右四条
        fillRect(x, y, w, thickness, color);
        fillRect(x, y + h - thickness, w, thickness, color);
        fillRect(x, y + thickness, thickness, h - 2 * thickness, color);
        fillRect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
        return;
    }
    const int r = std::max(1, static_cast<int>(std::lround(rf)));
    Impl::RoundTex& rt = d.roundFor(r, true);
    if (!rt.tex.image) return;
    PaintQuad q[9];
    const int n = BuildRoundedNineSlice(x, y, w, h, rf,
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

void BinUIVK::setLetterSpacing(float px) {
    // v0.16.40：字间距只影响排版，字形位图不变（整形缓存的 key 里带它）
    impl_->atlas.setLetterSpacing(px);
}
void BinUIVK::setTextSubpixel(bool on) {
    // 图集纹理本身已经是 LINEAR（见 Impl::createTex），这里只切"要不要取整"
    if (impl_) impl_->atlas.snapText = !on;
}

// v24 文字阴影：状态交给共享图集（GL / VK / D3D11 是同一份实现，所以三端逐像素相同）
// v0.16.18 仿射状态：三端语义一致（见 GL 后端同名函数）
// v26 图片滤镜：矩阵只影响"下一次上传"的像素，并按哈希进纹理缓存键。
void BinUIVK::setImageFilter(const BinUIImageFilter* f) {
    if (!impl_) return;
    impl_->imgFilterKey = binui::ImageFilterKey(f);
    if (!impl_->imgFilterKey) return;
    for (int i = 0; i < 9; i++) impl_->imgFilter.m[i] = f->m[i];
    for (int i = 0; i < 3; i++) impl_->imgFilter.o[i] = f->offset[i];
}

// v27 裁剪擦除：先 flush（批次延迟提交），再换管线
bool BinUIVK::setEraseBlend(bool on) {
    if (!impl_ || !impl_->ready) return false;
    impl_->flush();
    impl_->eraseOn = on;
    impl_->refreshPipeline();
    return true;
}

// v28 遮罩保留趟：dst *= srcA（同样先 flush 再换管线）
bool BinUIVK::setKeepBlend(bool on) {
    if (!impl_ || !impl_->ready) return false;
    impl_->flush();
    impl_->keepOn = on;
    impl_->refreshPipeline();
    return true;
}

bool BinUIVK::setAffine(const float* xf) {
    if (!impl_) return false;
    if (!xf || !(xf[0] != 1.0f || xf[1] != 0.0f || xf[2] != 0.0f ||
                 xf[3] != 0.0f || xf[4] != 1.0f || xf[5] != 0.0f)) {
        impl_->affineOn = false;
        return true;
    }
    for (int i = 0; i < 6; i++) impl_->aff[i] = xf[i];
    impl_->affineOn = true;
    return true;
}

void BinUIVK::setTextShadows(const BinUITextShadow* layers, int count) {
    if (impl_) impl_->atlas.setTextShadows(layers, count);
}

// ---------- 图片 ----------
// generation = "这份资源被换了"的代号：相同就复用，不重复解码/上传。
bool BinUIVK::prepareImage(uint16_t index, const uint8_t* data, size_t size,
                           uint32_t generation) {
    Impl& d = *impl_;
    if (!d.ready) return false;
    auto gi = d.imageGen.find(d.imgKey(index));
    auto ti = d.images.find(d.imgKey(index));
    if (gi != d.imageGen.end() && gi->second == generation && ti != d.images.end())
        return ti->second.view != VK_NULL_HANDLE;

    int iw = 0, ih = 0;
    std::vector<uint8_t> px = DecodeImagePixels(data, size, iw, ih);
    // v26：CSS filter 的颜色矩阵在解码后、建 mip 链之前跑一遍（链也是滤过的）
    if (!px.empty() && d.imgFilterKey)
        binui::ApplyColorMatrixBGRA(px.data(), iw, ih, d.imgFilter);
    if (px.empty()) {
        d.imageGen[d.imgKey(index)] = generation;
        return false;
    }
    if (ti != d.images.end()) d.freeTex(ti->second);   // 换图：旧图像必须放掉
    // v0.16.15（P1-8）：图片带完整 mip 链。链在 **CPU 上做盒式滤波**再逐级上传 ——
    // GPU 侧生成要么 blit 链（要 TRANSFER_SRC + 逐级布局切换，跟这里的"一张图一个
    // 布局状态"的记账模型别扭），要么 vkCmdBlit 对格式/线性滤波有要求，都不如
    // "多传几张小图"干脆：一次性代价（图片只在换 generation 时上传），质量与 GPU
    // 生成一致（2×2 平均）。视频帧/图集走 prepareImageRaw/atlas，**不做** mip。
    // v0.16.20：链的 2×2 盒式滤波换成共享实现（binui_mip.h，单测钉住；与 GL/D3D
    // 的硬件 GenerateMipmap/GenerateMips 等价），这里只管逐级上传。
    std::vector<std::vector<uint8_t>> chain = BuildMipChainBGRA(px.data(), iw, ih);
    if (chain.empty()) return false;
    const uint32_t levels = static_cast<uint32_t>(chain.size());
    Impl::Tex& t = d.images[d.imgKey(index)];
    if (!d.createTexImage(t, iw, ih, true, levels)) {
        d.images.erase(d.imgKey(index));
        return false;
    }
    t.needClear = false;   // 整张覆盖，不用先清零
    bool ok = true;
    int cw = iw, ch = ih;
    for (uint32_t l = 0; l < levels && ok; l++) {
        ok = d.uploadRegion(t, 0, 0, cw, ch, chain[l].data(), cw * 4, static_cast<int>(l));
        cw = std::max(1, cw / 2);
        ch = std::max(1, ch / 2);
    }
    d.imageGen[d.imgKey(index)] = generation;
    return ok;
}

// 原始像素直传（RGBA 字节序，没有解码）：BGRA 目标格式要换一下 R/B 通道
bool BinUIVK::prepareImageRaw(uint16_t index, int w, int h, const uint8_t* rgba,
                              uint32_t generation) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0 || !rgba) return false;
    auto gi = d.imageGen.find(d.imgKey(index));
    auto ti = d.images.find(d.imgKey(index));
    // v0.16.20：同一个 generation 又来了一次 = 这份原始像素不再每帧变（视频帧的
    // generation 每次都变，走不到这里）。此时补一条 mip 链：与 prepareImage 同一套
    // **CPU 2×2 盒式滤波**逐级上传 —— 程序化纹理缩小时才有 mip 可用，视频帧不必为
    // 每帧的链付钱。只做一次（rawMips 标记），之后 generation 不变就一直复用。
    if (gi != d.imageGen.end() && gi->second == generation && ti != d.images.end() &&
        ti->second.view && !d.rawMips.count(d.imgKey(index)) && w >= 2 && h >= 2) {
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
            bgra[i * 4 + 0] = rgba[i * 4 + 2];
            bgra[i * 4 + 1] = rgba[i * 4 + 1];
            bgra[i * 4 + 2] = rgba[i * 4 + 0];
            bgra[i * 4 + 3] = rgba[i * 4 + 3];
        }
        // mip 链的 2×2 盒式滤波用共享实现（binui_mip.h，单测钉住；与 GL/D3D 的硬件生成等价）
        std::vector<std::vector<uint8_t>> chain = BuildMipChainBGRA(bgra.data(), w, h);
        if (chain.empty()) return false;
        d.freeTex(ti->second);
        Impl::Tex& t = d.images[d.imgKey(index)];
        if (!d.createTexImage(t, w, h, true, static_cast<uint32_t>(chain.size()))) {
            d.images.erase(d.imgKey(index));
            d.imageGen.erase(d.imgKey(index));
            return false;
        }
        t.needClear = false;
        bool ok = true;
        int cw = w, ch = h;
        for (size_t l = 0; l < chain.size() && ok; l++) {
            ok = d.uploadRegion(t, 0, 0, cw, ch, chain[l].data(), cw * 4, static_cast<int>(l));
            cw = std::max(1, cw / 2);
            ch = std::max(1, ch / 2);
        }
        if (ok) d.rawMips[d.imgKey(index)] = 1;
        return ok;
    }
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
    Impl::Tex& t = d.images[d.imgKey(index)];
    if (!d.createTexImage(t, w, h, true)) {
        d.images.erase(d.imgKey(index));
        return false;
    }
    t.needClear = false;
    const bool ok = d.uploadRegion(t, 0, 0, w, h, bgra.data(), w * 4);
    d.rawMips.erase(d.imgKey(index));
    d.imageGen[d.imgKey(index)] = generation;
    return ok;
}

// v25 九宫格（切分数学见 binui_gfx.h 的 BuildNineSlice，三端共用）
void BinUIVK::drawImageNine(uint16_t index, float x, float y, float w, float h,
                           float slice, int repX, int repY, uint32_t tint) {
    if (!impl_ || !impl_->ready) return;
    auto it = impl_->images.find(impl_->imgKey(index));
    float q[kMaxNineSliceQuads][8];
    const int n = it == impl_->images.end() || !it->second.view
        ? 0 : BuildNineSlice(x, y, w, h, it->second.w, it->second.h, slice,
                             repX, repY, &q[0][0], kMaxNineSliceQuads);
    if (n <= 0) { drawImageEx(index, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w, h, tint); return; }
    for (int i = 0; i < n; i++)
        drawImageEx(index, q[i][0], q[i][1], q[i][2], q[i][3],
                    q[i][4], q[i][5], q[i][6], q[i][7], tint);
}

void BinUIVK::drawImageEx(uint16_t index, float u0, float v0, float u1, float v1,
                          float x, float y, float w, float h, uint32_t tint) {
    Impl& d = *impl_;
    if (!d.ready || w <= 0 || h <= 0) return;
    auto it = d.images.find(d.imgKey(index));
    if (it == d.images.end() || !it->second.view) return;
    d.emitQuad(x, y, x + w, y + h, u0, v0, u1, v1, tint, &it->second);
}

// ---- 组透明度：离屏图层（v0.16.12）----
// 语义与 GL/D3D11 后端逐条一致：层的范围 = 一个节点 + 它的整棵子树，
// 画进窗口大小的离屏目标后按 alpha 一次合成回来。
// 拿不到动态渲染（useDynamic=false，老驱动）时返回 false，调用方退回逐元素乘 alpha。
bool BinUIVK::pushLayer(float x, float y, float w, float h, float alpha) {
    return impl_ ? impl_->pushLayer(x, y, w, h, alpha) : false;
}

// v0.16.42：filter: blur 的模糊层（框已由调用方按 3σ 外扩并取整）
bool BinUIVK::pushLayerEx(float x, float y, float w, float h, const LayerFx& fx) {
    if (!impl_ || (fx.shadowCount > 0 && !fx.shadows)) return false;
    return impl_->pushLayer(x, y, w, h, fx.alpha, fx.blurPx, fx.shadows, fx.shadowCount);
}

void BinUIVK::popLayer() {
    if (impl_) impl_->popLayer();
}

// v29 毛玻璃：完整流程在 Impl::blurBackdrop（与 GL 后端逐条对应）。这里只做转发。
// v0.16.28：多一个 grain 参数（颗粒强度 0..1，0 = 不撒）。
bool BinUIVK::backdropBlur(float x, float y, float w, float h, float radius, float blurPx,
                           float grain) {
    return impl_ ? impl_->blurBackdrop(x, y, w, h, radius, blurPx, grain) : false;
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
