// binui_vk.cpp - Vulkan 绘制后端（v19，P2：离屏核心）
//
// 设计（docs/vulkan_design.md §4）：
//   · 运行期只 LoadLibrary("vulkan-1.dll") + vkGetInstanceProcAddr，不链 vulkan-1.lib；
//   · 每帧画进一张**持久 MSAA 累积图**（loadOp = LOAD），脏矩形靠 scissor + vkCmdClearAttachments；
//   · present = resolve(MSAA → 单采样) → vkCmdCopyImageToBuffer → 等 fence → 交给读回方；
//   · 顶点 20 字节，与 GL 的 GLVertex 逐位相同；颜色走 B8G8R8A8_UNORM，
//     让 AARRGGBB 的 R/G/B 落到正确的通道（见 MakeVertexFormat 注释）。
//
// P2 只做离屏 + 4 个几何原语；文本/图片是桩（P3）；窗口交换链在 P2b。
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
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/binui_vk_shaders.h"
#include "binui_paint.h"   // RoundTemplateSize / BuildRoundedNineSlice / PaintQuad

// 不链接 vulkan-1.lib：全部函数指针自己取（与 binui_gl_ext_min.h 同思路，但这里用官方头，
// 只靠 VK_NO_PROTOTYPES 关掉原型声明）。
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"

namespace binui {
namespace {

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
        // 设备级函数：vkGetInstanceProcAddr 对设备函数同样可用（loader 会给 trampoline）
#define BINUI_VK_LOAD(name)                                                   \
    name = reinterpret_cast<PFN_##name>(                                       \
        getInstanceProcAddr(instance_, #name));                                \
    if (!name) {                                                               \
        std::fprintf(stderr, "[vk] 取不到 %s\n", #name);                        \
        return false;                                                          \
    }
        // instance_ 在 load 之后才创建，这里分两段：先取实例函数，再调 loadDeviceFns()
        (void)0;
#undef BINUI_VK_LOAD
        return true;
    }

    VkInstance instance_ = VK_NULL_HANDLE;

    bool loadInstanceFns() {
        auto get = [&](const char* n) { return getInstanceProcAddr(instance_, n); };
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

    bool loadDeviceFns() {
        auto get = [&](const char* n) { return getInstanceProcAddr(instance_, n); };
        VkDevice d = device_;
        (void)d;
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
#undef BINUI_VK_LOAD
        return true;
    }

    VkDevice device_ = VK_NULL_HANDLE;
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
struct BinUIVK::Impl {
    // ---- 设备 ----
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;

    // ---- 命令 ----
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool recording = false;

    // ---- 目标 ----
    int w = 0, h = 0;
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
    std::vector<uint8_t> pixels;              // 读回后的 BGRA（自上而下）

    // ---- 管线 ----
    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dsPool = VK_NULL_HANDLE;
    VkSampler samplerNearest = VK_NULL_HANDLE;

    // ---- 纹理（白 + 圆角模板 + 每纹理 descriptor set）----
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        int size = 0;
    };
    Tex white;
    struct RoundTex {
        Tex tex;
        int t = 0;
    };
    std::unordered_map<int, RoundTex> rounds;   // key = 半径（描边用负值，与 GL 同）
    // 帧内不许做纹理上传（uploadTexture 会重置正在录制的命令缓冲）。
    // 缺失的圆角模板记在这里，下一帧 beginFrame 之前补上；这一帧调用方退回 fillRect。
    std::vector<std::pair<int, bool>> pendingRounds;
    std::unordered_map<int, Tex> images;        // 图片（P3）

    // ---- 顶点环 ----
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    VkVertex* vmap = nullptr;
    std::vector<VkVertex> verts;                 // 本帧顶点（提交时拷进 vmap）

    struct Batch {
        uint32_t first = 0;
        uint32_t count = 0;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        int sx = 0, sy = 0, sw = 0, sh = 0;
    };
    std::vector<Batch> batches;
    Batch cur{};
    int clipL = 0, clipT = 0, clipR = 0, clipB = 0;
    int scissorStack[32] = {};
    int scissorDepth = 0;
    bool vsync = true;
    bool ready = false;

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
            for (uint32_t i = 0; i < qn; i++)
                if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = static_cast<int>(i); break; }
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
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VK_CHECK(g_vk.vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        g_vk.device_ = device;
        g_vk.vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (!g_vk.loadDeviceFns()) return false;
        return true;
    }

    void destroyTargets() {
        if (framebuffer) { g_vk.vkDestroyFramebuffer(device, framebuffer, nullptr); framebuffer = VK_NULL_HANDLE; }
        if (renderPass) { g_vk.vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }
        if (msaaView) { g_vk.vkDestroyImageView(device, msaaView, nullptr); msaaView = VK_NULL_HANDLE; }
        if (resolveView) { g_vk.vkDestroyImageView(device, resolveView, nullptr); resolveView = VK_NULL_HANDLE; }
        if (msaaImage) { g_vk.vkDestroyImage(device, msaaImage, nullptr); msaaImage = VK_NULL_HANDLE; }
        if (resolveImage) { g_vk.vkDestroyImage(device, resolveImage, nullptr); resolveImage = VK_NULL_HANDLE; }
        if (msaaMem) { g_vk.vkFreeMemory(device, msaaMem, nullptr); msaaMem = VK_NULL_HANDLE; }
        if (resolveMem) { g_vk.vkFreeMemory(device, resolveMem, nullptr); resolveMem = VK_NULL_HANDLE; }
        if (readback) { g_vk.vkDestroyBuffer(device, readback, nullptr); readback = VK_NULL_HANDLE; }
        if (readbackMem) { g_vk.vkFreeMemory(device, readbackMem, nullptr); readbackMem = VK_NULL_HANDLE; readbackMapped = nullptr; }
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
        if (!createBuffer(static_cast<VkDeviceSize>(w) * h * 4,
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
    // 纹理
    // ============================================================
    bool uploadTexture(Tex& t, int tw, int th, const uint32_t* bgra, bool linear) {
        t.size = tw;
        std::fprintf(stderr, "[vk] uploadTexture %dx%d linear=%d\n", tw, th, (int)linear);
        if (!createImage2D(tw, th, kColorFormat,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_SAMPLE_COUNT_1_BIT, t.image, t.mem, VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        t.view = makeView(t.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.view) { std::fprintf(stderr, "[vk] uploadTexture: view 失败\n"); return false; }

        // staging buffer → image
        VkBuffer stage = VK_NULL_HANDLE;
        VkDeviceMemory stageMem = VK_NULL_HANDLE;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tw) * th * 4;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stage, stageMem)) {
            std::fprintf(stderr, "[vk] uploadTexture: staging buffer 失败 (%llu 字节)\n",
                         (unsigned long long)bytes);
            return false;
        }
        void* mapped = nullptr;
        if (g_vk.vkMapMemory(device, stageMem, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] uploadTexture: 映射 staging 失败\n");
            return false;
        }
        std::memcpy(mapped, bgra, static_cast<size_t>(bytes));
        g_vk.vkUnmapMemory(device, stageMem);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        g_vk.vkResetCommandBuffer(cmd, 0);
        VK_CHECK(g_vk.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(upload)");
        barrier(t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};
        g_vk.vkCmdCopyBufferToImage(cmd, stage, t.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        VK_CHECK(g_vk.vkEndCommandBuffer(cmd), "vkEndCommandBuffer(upload)");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(g_vk.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(upload)");
        g_vk.vkQueueWaitIdle(queue);
        g_vk.vkDestroyBuffer(device, stage, nullptr);
        g_vk.vkFreeMemory(device, stageMem, nullptr);

        // descriptor set（每纹理一个，与 ImGui 的做法一致）
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = dsPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsLayout;
        const VkResult dsr = g_vk.vkAllocateDescriptorSets(device, &ai, &t.ds);
        if (dsr != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] uploadTexture: descriptor set 失败 %s\n", VkResultText(dsr));
            return false;
        }

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        if (g_vk.vkCreateSampler(device, &sci, nullptr, &sampler) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] uploadTexture: vkCreateSampler 失败\n");
            return false;
        }

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
        g_vk.vkDestroySampler(device, sampler, nullptr);
        return true;
    }

    bool createWhite() {
        uint32_t px = 0xFFFFFFFFu;
        return uploadTexture(white, 1, 1, &px, false);
    }

    // 圆角模板：与 GL 的 roundFor 同一份解析式覆盖率（像素级同源）
    static RoundTex& emptyRound() {
        static RoundTex e;
        return e;
    }

    // 真正建模板（只在 beginFrame 之前调用：此时没有正在录制的命令缓冲）
    void buildRound(int radius, bool stroke) {
        const int r = std::max(1, radius);
        const int key = stroke ? -r : r;
        if (rounds.count(key)) return;
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
        RoundTex rt;
        rt.t = t;
        if (!uploadTexture(rt.tex, t, t, px.data(), false)) return;   // 失败：下一帧还会重登记
        rounds.emplace(key, rt);
    }

    RoundTex& roundFor(int radius, bool stroke) {
        const int r = std::max(1, radius);
        const int key = stroke ? -r : r;
        auto it = rounds.find(key);
        if (it != rounds.end()) return it->second;
        // 本帧没有：登记到下一帧补建，这次让调用方退回 fillRect（宁可方角，不要脏内存）
        pendingRounds.emplace_back(r, stroke);
        return emptyRound();
    }

    void drainPendingRounds() {
        if (pendingRounds.empty()) return;
        auto q = pendingRounds;
        pendingRounds.clear();
        for (const auto& pr : q) buildRound(pr.first, pr.second);
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
        pi.maxSets = 256;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &psz;
        VK_CHECK(g_vk.vkCreateDescriptorPool(device, &pi, nullptr, &dsPool),
                 "vkCreateDescriptorPool");

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
    void ensureVertBuf() {
        if (vbuf) return;
        if (!createBuffer(kMaxVertices * sizeof(VkVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          vbuf, vmem))
            return;
        void* mapped = nullptr;
        if (g_vk.vkMapMemory(device, vmem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
            vmap = static_cast<VkVertex*>(mapped);
        verts.reserve(kMaxVertices);
        batches.reserve(kMaxBatches);
    }

    void switchBatch(VkDescriptorSet ds) {
        if (cur.ds == ds && cur.sx == clipL && cur.sy == clipT && cur.sw == clipR - clipL &&
            cur.sh == clipB - clipT)
            return;
        if (cur.count > 0 && batches.size() < static_cast<size_t>(kMaxBatches)) {
            // ★ 不要在这里覆盖 scissor：批次的 scissor 是**开始那一刻**的裁剪区，
            // 覆盖成"关闭那一刻"的会让先前 emitted 的顶点跑到别的裁剪区里
            // （实测症状：整页底色矩形被裁成一小块，四周露白）。
            batches.push_back(cur);
        }
        cur = Batch{};
        cur.first = static_cast<uint32_t>(verts.size());
        cur.ds = ds;
        cur.sx = clipL; cur.sy = clipT; cur.sw = clipR - clipL; cur.sh = clipB - clipT;
    }

    void emitQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  uint32_t color, VkDescriptorSet ds) {
        if (verts.size() + 6 > kMaxVertices) return;
        switchBatch(ds);
        const VkVertex a{x0, y0, color, u0, v0};
        const VkVertex b{x1, y0, color, u1, v0};
        const VkVertex c{x0, y1, color, u0, v1};
        const VkVertex d{x1, y1, color, u1, v1};
        verts.push_back(a); verts.push_back(b); verts.push_back(c);
        verts.push_back(c); verts.push_back(b); verts.push_back(d);
        cur.count += 2;
    }

    void flush() {
        if (std::getenv("BINUI_VK_TRACE")) {
            std::fprintf(stderr, "[vk][trace] flush: verts=%zu batches=%zu cur.count=%u\n",
                         verts.size(), batches.size(), cur.count);
        }
        if (batches.empty()) {
            if (cur.count > 0) batches.push_back(cur);
            cur = Batch{};
        } else if (cur.count > 0) {
            batches.push_back(cur);
            cur = Batch{};
        }
        if (verts.empty() || !vmap) return;
        std::memcpy(vmap, verts.data(), verts.size() * sizeof(VkVertex));

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
            g_vk.vkCmdSetScissor(cmd, 0, 1, &sc);
            g_vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1,
                                         &b.ds, 0, nullptr);
            g_vk.vkCmdDraw(cmd, b.count * 3, 1, b.first, 0);   // count 是三角形数，first 是顶点索引
        }
        batches.clear();
        verts.clear();
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
    if (hwnd) {
        std::fprintf(stderr, "[vk] 窗口 + 交换链在 P2b 实现（当前只支持离屏）；返回未就绪\n");
        return false;
    }
    if (width <= 0 || height <= 0) return false;
    d.w = width;
    d.h = height;
    d.vsync = vsync;

    if (!g_vk.load()) return false;

    // ---- 实例（Vulkan 1.0，无层、无扩展：离屏不需要 WSI）----
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "binUI";
    app.applicationVersion = VK_MAKE_VERSION(0, 9, 2);
    app.pEngineName = "binUI";
    app.engineVersion = VK_MAKE_VERSION(0, 9, 2);
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_vk.getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) {
        std::fprintf(stderr, "[vk] 取不到 vkCreateInstance\n");
        return false;
    }
    VK_CHECK(createInstance(&ici, nullptr, &d.instance), "vkCreateInstance");
    g_vk.instance_ = d.instance;
    if (!g_vk.loadInstanceFns()) return false;

    if (!d.pickPhysical()) return false;
    if (!d.createDevice()) return false;

    // 命令池 / 命令缓冲 / fence
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = d.queueFamily;
    VK_CHECK(g_vk.vkCreateCommandPool(d.device, &cpi, nullptr, &d.cmdPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = d.cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_CHECK(g_vk.vkAllocateCommandBuffers(d.device, &cbi, &d.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // ★ 必须一开始就是 signaled：beginFrame 的第一步是 wait 这一帧的 fence，
    // 建出来是 unsignaled 的话第一帧直接死等（实测挂死，CPU 0%）。
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(g_vk.vkCreateFence(d.device, &fci, nullptr, &d.fence), "vkCreateFence");

    d.clipL = 0; d.clipT = 0; d.clipR = width; d.clipB = height;
    std::fprintf(stderr, "[vk] step: createTargets\n");
    if (!d.createTargets()) return false;
    std::fprintf(stderr, "[vk] step: createPipeline\n");
    if (!d.createPipeline()) return false;
    std::fprintf(stderr, "[vk] step: createWhite\n");

    // 建白纹理（uploadTexture 内部会用 cmd 做一次独立提交，这里不能处于 recording）
    if (!d.createWhite()) { std::fprintf(stderr, "[vk] createWhite 返回 false\n"); return false; }
    std::fprintf(stderr, "[vk] step: createWhite done\n");

    d.ready = true;
    std::fprintf(stderr, "[vk] 离屏就绪 %dx%d（MSAA %dx）\n", width, height,
                 static_cast<int>(d.samples));
    return true;
}

void BinUIVK::shutdown() {
    Impl& d = *impl_;
    if (!d.device) return;
    g_vk.vkDeviceWaitIdle(d.device);
    for (auto& kv : d.rounds) {
        Impl::Tex& t = kv.second.tex;
        if (t.view) g_vk.vkDestroyImageView(d.device, t.view, nullptr);
        if (t.image) g_vk.vkDestroyImage(d.device, t.image, nullptr);
        if (t.mem) g_vk.vkFreeMemory(d.device, t.mem, nullptr);
    }
    d.rounds.clear();
    for (auto& kv : d.images) {
        Impl::Tex& t = kv.second;
        if (t.view) g_vk.vkDestroyImageView(d.device, t.view, nullptr);
        if (t.image) g_vk.vkDestroyImage(d.device, t.image, nullptr);
        if (t.mem) g_vk.vkFreeMemory(d.device, t.mem, nullptr);
    }
    d.images.clear();
    if (d.white.view) g_vk.vkDestroyImageView(d.device, d.white.view, nullptr);
    if (d.white.image) g_vk.vkDestroyImage(d.device, d.white.image, nullptr);
    if (d.white.mem) g_vk.vkFreeMemory(d.device, d.white.mem, nullptr);
    d.white = Impl::Tex{};
    if (d.dsPool) g_vk.vkDestroyDescriptorPool(d.device, d.dsPool, nullptr);
    if (d.pipeline) g_vk.vkDestroyPipeline(d.device, d.pipeline, nullptr);
    if (d.pipeLayout) g_vk.vkDestroyPipelineLayout(d.device, d.pipeLayout, nullptr);
    if (d.dsLayout) g_vk.vkDestroyDescriptorSetLayout(d.device, d.dsLayout, nullptr);
    if (d.vbuf) g_vk.vkDestroyBuffer(d.device, d.vbuf, nullptr);
    if (d.vmem) g_vk.vkFreeMemory(d.device, d.vmem, nullptr);
    g_vk.vkUnmapMemory(d.device, d.vmem);
    d.destroyTargets();
    if (d.fence) g_vk.vkDestroyFence(d.device, d.fence, nullptr);
    if (d.cmdPool) g_vk.vkDestroyCommandPool(d.device, d.cmdPool, nullptr);
    g_vk.vkDestroyDevice(d.device, nullptr);
    if (d.instance) g_vk.vkDestroyInstance(d.instance, nullptr);
    g_vk.device_ = VK_NULL_HANDLE;
    g_vk.instance_ = VK_NULL_HANDLE;
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
    if (!d.createTargets()) {
        d.ready = false;
        std::fprintf(stderr, "[vk] resize 后重建目标失败\n");
    }
}

void BinUIVK::setVsync(bool on) { impl_->vsync = on; }
void BinUIVK::setTransparent(bool) { /* 离屏模式无意义；窗口模式在 P2b */ }
void BinUIVK::setFontName(const char*) { /* P3：字形图集 */ }

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
    d.drainPendingRounds();   // 必须在开始录制之前（内部会 begin/end/submit 命令缓冲）

    g_vk.vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
    g_vk.vkResetFences(d.device, 1, &d.fence);
    g_vk.vkResetCommandBuffer(d.cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    g_vk.vkBeginCommandBuffer(d.cmd, &bi);
    d.recording = true;

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

    d.ensureVertBuf();
    d.verts.clear();
    d.batches.clear();
    d.cur = Impl::Batch{};
    d.switchBatch(d.white.ds);
}

void BinUIVK::present() {
    Impl& d = *impl_;
    if (!d.ready || !d.recording) return;
    d.flush();
    g_vk.vkCmdEndRenderPass(d.cmd);

    // resolve 目标已经是 TRANSFER_SRC_OPTIMAL（render pass finalLayout），直接拷到 buffer
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h), 1};
    g_vk.vkCmdCopyImageToBuffer(d.cmd, d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                d.readback, 1, &region);
    // 拷完把 resolve 图转回 render pass 的写入布局（下一帧 subpass 会再 resolve 进去）
    d.barrier(d.resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_vk.vkEndCommandBuffer(d.cmd);
    d.recording = false;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d.cmd;
    if (g_vk.vkQueueSubmit(d.queue, 1, &si, d.fence) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] present: vkQueueSubmit 失败\n");
        return;
    }
    g_vk.vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
}

const uint8_t* BinUIVK::readbackPixels(int& outW, int& outH) {
    Impl& d = *impl_;
    if (!d.ready || !d.readbackMapped) return nullptr;
    outW = d.w;
    outH = d.h;
    // GPU 侧已是自上而下写入（region 的 offset=0 对应图像第 0 行 = 顶部）
    return static_cast<const uint8_t*>(d.readbackMapped);
}

// ---------- 原语 ----------
void BinUIVK::fillRect(float x, float y, float w, float h, uint32_t color) {
    Impl& d = *impl_;
    if (std::getenv("BINUI_VK_TRACE")) {
        static int n = 0;
        if (n < 4) std::fprintf(stderr, "[vk][trace] fillRect #%d (%.0f,%.0f %.0fx%.0f) color=%08X\n",
                                n++, (double)x, (double)y, (double)w, (double)h, color);
    }
    if (!d.ready || w <= 0 || h <= 0) return;
    uint32_t c = ScaleAlphaU32(color, 1.0f);
    Premult(c);
    d.emitQuad(x, y, x + w, y + h, 0.5f, 0.5f, 0.5f, 0.5f, c, d.white.ds);
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
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color, rt.tex.ds);
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
    d.switchBatch(d.white.ds);
    if (d.verts.size() + 6 > kMaxVertices) return;
    const VkVertex a{x0 + nx, y0 + ny, c, 0.5f, 0.5f};
    const VkVertex b{x1 + nx, y1 + ny, c, 0.5f, 0.5f};
    const VkVertex cc{x1 - nx, y1 - ny, c, 0.5f, 0.5f};
    const VkVertex dd{x0 - nx, y0 - ny, c, 0.5f, 0.5f};
    d.verts.push_back(a); d.verts.push_back(b); d.verts.push_back(cc);
    d.verts.push_back(a); d.verts.push_back(cc); d.verts.push_back(dd);
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
                   q[i].u0, q[i].v0, q[i].u1, q[i].v1, color, rt.tex.ds);
}

void BinUIVK::drawImage(uint16_t, float, float, float, float) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr, "[vk] 图片绘制在 P3 实现（当前忽略）\n");
    }
}

void BinUIVK::drawText(float, float, float, int, const std::string&, uint32_t, bool, bool, bool) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr, "[vk] 文本绘制在 P3 实现（当前忽略）\n");
    }
}

void BinUIVK::drawTextWrapped(float, float, float, float, int, const std::string&, uint32_t, int,
                              bool) {}

float BinUIVK::textWidth(float, int, const std::string&, bool) { return 0.0f; }

void BinUIVK::fontMetrics(float, int, int& ascent, int& lineHeight, bool) {
    ascent = 0;
    lineHeight = 0;
}

bool BinUIVK::prepareImage(uint16_t, const uint8_t*, size_t, uint32_t) { return false; }
bool BinUIVK::prepareImageRaw(uint16_t, int, int, const uint8_t*, uint32_t) { return false; }

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
    d.switchBatch(d.cur.ds);
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
    d.switchBatch(d.cur.ds);
}

} // namespace binui
