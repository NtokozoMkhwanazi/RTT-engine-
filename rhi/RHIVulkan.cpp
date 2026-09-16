/**
 * Vulkan RHI backend.
 *
 * Full Vulkan rendering backend: instance/device/swapchain, pipeline management,
 * command buffers, UBOs, textures (with PBR multi-texture support), descriptor
 * sets, render passes, offscreen rendering, swapchain scene rendering, and
 * ImGui integration.
 */
#include "RHI.h"
#include "RHIMath.h"
#include "lighting/LightData.h"   // shared std430 GPULightData / LightUBO (suggestions.txt #1)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include "imgui_impl_vulkan.h"

// Vulkan Memory Allocator (VMA) — single-header, integrated for sub-allocation
// and persistent mapping convenience.  Implementation lives in this TU.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>

namespace RHI {
namespace {

// ---------------------------------------------------------------------------
// Vertex struct — matches GLSL `struct Vertex { vec3 pos; vec3 normal; vec2 uv; }`
// Lays out as 8 floats (32 bytes) per vertex: pos(3) + normal(3) + uv(2).
// This feeds the global vertex SSBO used by the bindless GLSL 4.6 shaders.
// ---------------------------------------------------------------------------
struct VulkanVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(VulkanVertex) == 32, "VulkanVertex must be 32 bytes (8 floats)");

// ---------------------------------------------------------------------------
// GPU cluster structs — mirror meshSystem/Mesh.h's GPUClusterBounds /
// GPUClusterCommand.  Kept local here because Mesh.h pulls in <glad/glad.h>
// which conflicts with Vulkan headers.  Layout must match cluster_cull.comp:
//   ClusterBounds  = 3 × vec4 (std140, 48 bytes)
//   ClusterCommand = 4 × uint  (std430, 16 bytes)
// ---------------------------------------------------------------------------
struct VkClusterBounds {
    glm::vec4 sphereCenterRadius;
    glm::vec4 minBounds;
    glm::vec4 maxBounds;
};
struct VkClusterCommand {
    uint32_t firstVertex;
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t instanceId;
};
static_assert(sizeof(VkClusterBounds) == 48, "VkClusterBounds must be 48 bytes");
static_assert(sizeof(VkClusterCommand) == 16, "VkClusterCommand must be 16 bytes");

// Per-mesh upload metadata: tracks offsets into the global SSBOs so the
// draw loop can issue a single vkCmdDrawIndexed with the right firstVertex /
// firstInstance / firstIndex.
struct MeshUploadInfo {
    uint32_t firstVertex = 0;    // offset (in vertices) in the global vertex SSBO
    uint32_t firstIndex  = 0;    // offset (in indices) in the global index buffer
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    uint32_t firstInstance = 0;  // offset (in instances) in the global instance SSBO
    uint32_t instanceCount = 0;
    int      textureId = -1;     // index into the bindless texture array (-1 = white default)
    uint64_t vertexVersion = 0;
    uint64_t instanceVersion = 0;
    // --- Virtualized Geometry (Phase 2) ---
    uint32_t firstCluster = 0;   // offset into the cluster bounds/commands buffers
    uint32_t clusterCount = 0;   // number of clusters for this mesh (0 = legacy draw)
};

// Per-mesh texture resources — image view registered in the bindless array
struct MeshTexResources {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int viewIndex = -1;  // slot in the bindless texture descriptor array
};

// ---------------------------------------------------------------------------
// Shared graphics API state
// ---------------------------------------------------------------------------
extern GraphicsAPI g_activeApi;  // defined in RHIGL.cpp
constexpr const char* kApiConfigFile = "graphics_api.cfg";

// ---------------------------------------------------------------------------
// Vulkan function loading (vkGetInstanceProcAddr / vkGetDeviceProcAddr)
// ---------------------------------------------------------------------------
#define VK_FUNC_LIST \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkCreateInstance) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) \
    X(vkDestroyDevice) \
    X(vkDestroySurfaceKHR) \
    X(vkGetDeviceQueue) \
    X(vkQueueWaitIdle) \
    X(vkDeviceWaitIdle) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkFlushMappedMemoryRanges) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkGetBufferMemoryRequirements2KHR) \
    X(vkGetImageMemoryRequirements2KHR) \
    X(vkBindBufferMemory2KHR) \
    X(vkBindImageMemory2KHR) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkWaitSemaphores) \
    X(vkSignalSemaphore) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdDrawIndexedIndirect) \
    X(vkCmdDrawIndexedIndirectCount) \
    X(vkFreeDescriptorSets) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyBuffer) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCmdPushConstants) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkGetQueryPoolResults) \
    X(vkCmdWriteTimestamp) \
    X(vkCmdResetQueryPool) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage) \
    X(vkCmdBlitImage) \
    X(vkCmdResolveImage) \
    X(vkCmdDispatch) \
    X(vkCreateComputePipelines)

#define X(name) static PFN_##name name = nullptr;
VK_FUNC_LIST
#undef X

void loadVulkanFunctions(VkInstance inst, VkDevice dev) {
    PFN_vkGetInstanceProcAddr gpa =
        (PFN_vkGetInstanceProcAddr)glfwGetInstanceProcAddress(inst, "vkGetInstanceProcAddr");
    if (!gpa) { std::cerr << "[RHI-Vk] Failed to load vkGetInstanceProcAddr\n"; return; }
    #define X(name) name = (PFN_##name)gpa(inst, #name);
    VK_FUNC_LIST
    #undef X
    if (dev) {
        PFN_vkGetDeviceProcAddr dpa =
            (PFN_vkGetDeviceProcAddr)gpa(inst, "vkGetDeviceProcAddr");
        #define X(name) if (!name) name = (PFN_##name)dpa(dev, #name);
        VK_FUNC_LIST
        #undef X
        // On some drivers (RADV), the instance-level proc addr for Vulkan 1.1+
        // promoted KHR functions returns a non-null but non-functional pointer
        // for device-level calls. Override these with device-level proc addr,
        // preferring the non-KHR core name for Vulkan 1.1+.
        if (dpa) {
            PFN_vkGetBufferMemoryRequirements2KHR fp =
                (PFN_vkGetBufferMemoryRequirements2KHR)dpa(dev, "vkGetBufferMemoryRequirements2KHR");
            if (!fp) fp = (PFN_vkGetBufferMemoryRequirements2KHR)dpa(dev, "vkGetBufferMemoryRequirements2");
            if (fp) vkGetBufferMemoryRequirements2KHR = fp;
            PFN_vkGetImageMemoryRequirements2KHR fp2 =
                (PFN_vkGetImageMemoryRequirements2KHR)dpa(dev, "vkGetImageMemoryRequirements2KHR");
            if (!fp2) fp2 = (PFN_vkGetImageMemoryRequirements2KHR)dpa(dev, "vkGetImageMemoryRequirements2");
            if (fp2) vkGetImageMemoryRequirements2KHR = fp2;
            PFN_vkBindBufferMemory2KHR fp3 =
                (PFN_vkBindBufferMemory2KHR)dpa(dev, "vkBindBufferMemory2KHR");
            if (!fp3) fp3 = (PFN_vkBindBufferMemory2KHR)dpa(dev, "vkBindBufferMemory2");
            if (fp3) vkBindBufferMemory2KHR = fp3;
            PFN_vkBindImageMemory2KHR fp4 =
                (PFN_vkBindImageMemory2KHR)dpa(dev, "vkBindImageMemory2KHR");
            if (!fp4) fp4 = (PFN_vkBindImageMemory2KHR)dpa(dev, "vkBindImageMemory2");
            if (fp4) vkBindImageMemory2KHR = fp4;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::vector<char> loadSpirv(const char* path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) return {};
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

VkShaderModule createShaderModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &mod);
    return mod;
}

void FlipRows(unsigned char* rgba, int w, int h) {
    std::vector<unsigned char> row(w * 4);
    for (int y = 0; y < h / 2; ++y) {
        unsigned char* top = rgba + y * w * 4;
        unsigned char* bot = rgba + (h - 1 - y) * w * 4;
        std::memcpy(row.data(), top, w * 4);
        std::memcpy(top, bot, w * 4);
        std::memcpy(bot, row.data(), w * 4);
    }
}

bool copyBufferToImage(VkDevice dev, VkCommandBuffer cmd, VkBuffer buf,
                        VkImage img, uint32_t w, uint32_t h) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return true;
}

void transitionImageLayout(VkCommandBuffer cmd, VkImage img,
                            VkImageLayout oldLayout, VkImageLayout newLayout) {
    // Modern synchronization (Vulkan 1.3 sync2) — replaces legacy
    // vkCmdPipelineBarrier with a single vkCmdPipelineBarrier2 call.
    VkImageMemoryBarrier2 barrier2{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier2.image = img;
    barrier2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier2.oldLayout = oldLayout;
    barrier2.newLayout = newLayout;

    if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier2.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier2.srcAccessMask = 0;
    } else {
        barrier2.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier2.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier2.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier2.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier2.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    } else {
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        barrier2.dstAccessMask = 0;
    }

    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier2;
    vkCmdPipelineBarrier2(cmd, &depInfo);
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                         VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((typeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

// ---------------------------------------------------------------------------
// RHIVulkan — Full Vulkan rendering backend
// ---------------------------------------------------------------------------
class RHIVulkan final : public IRHI {
public:
    GraphicsAPI api() const override { return GraphicsAPI::Vulkan; }

    bool available() const override {
        // glfwVulkanSupported() requires GLFW to have been initialized first
        // (it checks an internal _glfw.initialized flag). Initialize GLFW as a
        // clean probe, then tear it down so the caller's own glfwInit() in
        // initialize() is not affected by an extra reference count.
        if (!glfwInit()) return false;
        bool ok = glfwVulkanSupported();
        if (!ok) {
            glfwTerminate();
            return false;
        }
        // Load vkEnumerateInstanceExtensionProperties via GLFW (no instance needed)
        PFN_vkEnumerateInstanceExtensionProperties enumerateFn =
            (PFN_vkEnumerateInstanceExtensionProperties)
            glfwGetInstanceProcAddress(nullptr, "vkEnumerateInstanceExtensionProperties");
        if (!enumerateFn) {
            glfwTerminate();
            return false;
        }
        uint32_t count = 0;
        enumerateFn(nullptr, &count, nullptr);
        glfwTerminate();
        return count > 0;
    }

    GLFWwindow* window() const override { return m_window; }
    int width() const override { return m_width; }
    int height() const override { return m_height; }

    // =====================================================================
    // INITIALIZATION
    // =====================================================================
    bool initialize(int w, int h, const char* title) override {
        if (m_window) return true;

        // glfwInit() must be called BEFORE glfwVulkanSupported() — the latter
        // checks an internal initialized flag and returns false otherwise.
        if (!glfwInit()) {
            std::cerr << "[RHI-Vk] Failed to initialize GLFW\n";
            return false;
        }

        if (!glfwVulkanSupported()) {
            std::cerr << "[RHI-Vk] glfwVulkanSupported() returned false\n";
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        m_window = glfwCreateWindow(w, h, title, nullptr, nullptr);
        if (!m_window) { std::cerr << "[RHI-Vk] Failed to create window\n"; return false; }

        m_width = w;
        m_height = h;

        // Pre-load Vulkan functions via GLFW (before any Vulkan calls).
        // glfwGetInstanceProcAddress(NULL, ...) resolves vkGetInstanceProcAddr
        // and instance-level functions without needing a VkInstance.
        PFN_vkGetInstanceProcAddr gpaInit =
            (PFN_vkGetInstanceProcAddr)glfwGetInstanceProcAddress(nullptr, "vkGetInstanceProcAddr");
        if (!gpaInit) { std::cerr << "[RHI-Vk] Failed to load vkGetInstanceProcAddr via GLFW\n"; return false; }

        // vkGetInstanceProcAddr(VK_NULL_HANDLE, ...) only guarantees valid
        // pointers for a few core functions (vkCreateInstance, etc.).
        // Load those first, then create the instance, then reload ALL
        // instance-level function pointers with the real instance.
        #define X(name) if (!name) name = (PFN_##name)gpaInit(VK_NULL_HANDLE, #name);
        VK_FUNC_LIST
        #undef X

        // Instance — target Vulkan 1.3 baseline for dynamic rendering + sync2
        uint32_t extCount = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&extCount);
        if (!exts || extCount == 0) {
            std::cerr << "[RHI-Vk] No required instance extensions available\n"; return false;
        }
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pEngineName = "3D Game Engine";
        appInfo.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instCI{};
        instCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instCI.pApplicationInfo = &appInfo;
        instCI.enabledExtensionCount = extCount;
        instCI.ppEnabledExtensionNames = exts;
        if (vkCreateInstance(&instCI, nullptr, &m_instance) != VK_SUCCESS) {
            std::cerr << "[RHI-Vk] Failed to create instance\n"; return false;
        }

        // Now that we have a real VkInstance, reload ALL instance-level
        // function pointers so calls like vkEnumeratePhysicalDevices and
        // vkCreateWindowSurface don't crash on a NULL pointer.
        #define X(name) name = (PFN_##name)gpaInit(m_instance, #name);
        VK_FUNC_LIST
        #undef X

        // Surface
        if (glfwCreateWindowSurface(m_instance, m_window, nullptr, &m_surface) != VK_SUCCESS) {
            std::cerr << "[RHI-Vk] Failed to create surface\n"; return false;
        }

        // Physical device
        uint32_t devCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &devCount, nullptr);
        std::vector<VkPhysicalDevice> physDevs(devCount);
        vkEnumeratePhysicalDevices(m_instance, &devCount, physDevs.data());
        m_physical = physDevs[0];

        VkPhysicalDeviceProperties physProps;
        vkGetPhysicalDeviceProperties(m_physical, &physProps);
        std::cout << "[RHI-Vk] GPU: " << physProps.deviceName << std::endl;

        // Queue family — look for BOTH a graphics+present family AND a
        // dedicated compute-only (ACE) family so we can run Nanite LOD
        // selection, cluster culling and FSR3 temporal resolve on a separate
        // async compute queue that overlaps graphics rendering.
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qfCount, qfProps.data());
        for (uint32_t i = 0; i < qfCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_physical, i, m_surface, &present);
            if ((qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                m_queueFamily = i;
            }
            // Look for a dedicated compute-only queue: COMPUTE but NOT GRAPHICS.
            // On AMD RDNA this maps to a second ACE lane that can execute
            // compute work in parallel with the main graphics queue.
            if ((qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                !(qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                m_computeQueueFamily = i;
            }
        }
        // Fallback: if no dedicated compute-only family exists, reuse the
        // graphics family — work still runs, just not in parallel.
        if (m_computeQueueFamily == UINT32_MAX)
            m_computeQueueFamily = m_queueFamily;

        // Device — enable Vulkan 1.3 features: dynamic rendering + sync2
        // Also enable Vulkan 1.2 bindless-descriptor features required by the
        // bindless architecture (Step 2): runtimeDescriptorArray,
        // descriptorBindingPartiallyBound, and the *UpdateAfterBind variants so
        // the descriptor set can be updated at any time (including while bound).
        const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        features12.timelineSemaphore = VK_TRUE;
        features13.pNext = &features12;
        VkDeviceCreateInfo devCI{};
        devCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        devCI.pNext = &features13;

        // Request the graphics+present queue and (if distinct) the async
        // compute-only queue so we can submit compute work concurrently.
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        float qPri = 1.0f;
        VkDeviceQueueCreateInfo gCI{};
        gCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        gCI.queueFamilyIndex = m_queueFamily;
        gCI.queueCount = 1;
        gCI.pQueuePriorities = &qPri;
        queueCreateInfos.push_back(gCI);
        if (m_computeQueueFamily != m_queueFamily) {
            VkDeviceQueueCreateInfo cCI{};
            cCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            cCI.queueFamilyIndex = m_computeQueueFamily;
            cCI.queueCount = 1;
            cCI.pQueuePriorities = &qPri;
            queueCreateInfos.push_back(cCI);
        }
        devCI.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();
        devCI.pQueueCreateInfos = queueCreateInfos.data();
        devCI.enabledExtensionCount = 1;
        devCI.ppEnabledExtensionNames = devExts;
        if (vkCreateDevice(m_physical, &devCI, nullptr, &m_device) != VK_SUCCESS) {
            std::cerr << "[RHI-Vk] Failed to create device\n"; return false;
        }

        loadVulkanFunctions(m_instance, m_device);
        vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
        // Grab the async compute queue handle (may alias m_queue if no
        // dedicated family — the submit path is identical either way).
        vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);

        PFN_vkGetDeviceProcAddr dpa = (PFN_vkGetDeviceProcAddr)glfwGetInstanceProcAddress(m_instance, "vkGetDeviceProcAddr");

        // --- VMA allocator (Step 5: Advanced Memory Management) ---
        VmaVulkanFunctions vfuncs{};
        vfuncs.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)glfwGetInstanceProcAddress(m_instance, "vkGetInstanceProcAddr");
        vfuncs.vkGetDeviceProcAddr = dpa;
        // Populate VMA's internal function pointers from our loaded proc-addresses.
        // Note: On some drivers (RADV), vkGetDeviceProcAddr won't find the KHR-suffixed
        // names of Vulkan 1.1+ promoted functions. Load the non-KHR core names explicitly
        // and assign them to the KHR fields — the function signatures are identical.
        vfuncs.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
        vfuncs.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
        vfuncs.vkAllocateMemory = vkAllocateMemory;
        vfuncs.vkFreeMemory = vkFreeMemory;
        vfuncs.vkMapMemory = vkMapMemory;
        vfuncs.vkUnmapMemory = vkUnmapMemory;
        vfuncs.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
        vfuncs.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
        vfuncs.vkBindBufferMemory = vkBindBufferMemory;
        vfuncs.vkBindImageMemory = vkBindImageMemory;
        vfuncs.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
        vfuncs.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
        vfuncs.vkCreateBuffer = vkCreateBuffer;
        vfuncs.vkDestroyBuffer = vkDestroyBuffer;
        vfuncs.vkCreateImage = vkCreateImage;
        vfuncs.vkDestroyImage = vkDestroyImage;
        vfuncs.vkCmdCopyBuffer = vkCmdCopyBuffer;
        // Vulkan 1.1+ promoted functions — on RADV, the KHR-suffixed name loaded
        // via instance-level vkGetInstanceProcAddr doesn't work with VkDevice.
        // Force-load via device-level proc addr with the non-KHR core name.
        if (dpa) {
            PFN_vkGetBufferMemoryRequirements2KHR fp =
                (PFN_vkGetBufferMemoryRequirements2KHR)dpa(m_device, "vkGetBufferMemoryRequirements2KHR");
            if (!fp) fp = (PFN_vkGetBufferMemoryRequirements2KHR)dpa(m_device, "vkGetBufferMemoryRequirements2");
            vfuncs.vkGetBufferMemoryRequirements2KHR = fp ? fp : vkGetBufferMemoryRequirements2KHR;

            PFN_vkGetImageMemoryRequirements2KHR fp2 =
                (PFN_vkGetImageMemoryRequirements2KHR)dpa(m_device, "vkGetImageMemoryRequirements2KHR");
            if (!fp2) fp2 = (PFN_vkGetImageMemoryRequirements2KHR)dpa(m_device, "vkGetImageMemoryRequirements2");
            vfuncs.vkGetImageMemoryRequirements2KHR = fp2 ? fp2 : vkGetImageMemoryRequirements2KHR;

            PFN_vkBindBufferMemory2KHR fp3 =
                (PFN_vkBindBufferMemory2KHR)dpa(m_device, "vkBindBufferMemory2KHR");
            if (!fp3) fp3 = (PFN_vkBindBufferMemory2KHR)dpa(m_device, "vkBindBufferMemory2");
            vfuncs.vkBindBufferMemory2KHR = fp3 ? fp3 : vkBindBufferMemory2KHR;

            PFN_vkBindImageMemory2KHR fp4 =
                (PFN_vkBindImageMemory2KHR)dpa(m_device, "vkBindImageMemory2KHR");
            if (!fp4) fp4 = (PFN_vkBindImageMemory2KHR)dpa(m_device, "vkBindImageMemory2");
            vfuncs.vkBindImageMemory2KHR = fp4 ? fp4 : vkBindImageMemory2KHR;
        }

        VmaAllocatorCreateInfo allocCI{};
        allocCI.vulkanApiVersion = VK_API_VERSION_1_3;
        allocCI.instance = m_instance;
        allocCI.physicalDevice = m_physical;
        allocCI.device = m_device;
        allocCI.pVulkanFunctions = &vfuncs;
        if (vmaCreateAllocator(&allocCI, &m_vmaAllocator) != VK_SUCCESS) {
            std::cerr << "[RHI-Vk] Failed to create VMA allocator\n"; return false;
        }

        // Swapchain
        if (!createSwapchain()) return false;

        // Timeline semaphore replaces m_fences + binary sync: a single integer
        // counter that both the CPU and GPU wait on/sign. On AMD RDNA this
        // eliminates the fence-to-binary-semaphore handoff stall that caused
        // bubbles when the async compute queue waited on graphics fences.
        VkSemaphoreTypeCreateInfo timelineCI{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        timelineCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineCI.initialValue = 0;
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sci.pNext = &timelineCI;
        vkCreateSemaphore(m_device, &sci, nullptr, &m_frameTimeline);
        m_timelineValue = 0;
        // Binary semaphores for swapchain acquire/present (VK_KHR_swapchain
        // requires binary semaphores; timeline semaphores work here too on
        // Vulkan 1.3 but binary is the simplest correct path).
        VkSemaphoreCreateInfo bsci{};
        bsci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(m_device, &bsci, nullptr, &m_imageAvailSem);
        vkCreateSemaphore(m_device, &bsci, nullptr, &m_renderDoneSem);

        std::cout << "[RHI-Vk] Vulkan device ready (swapchain "
                  << m_swapImages.size() << " images)" << std::endl;
        return true;
    }

    void shutdown() override {
        if (!m_device) return;
        vkDeviceWaitIdle(m_device);

        // Shut down ImGui Vulkan backend before destroying the device
        if (m_imguiInitialized) {
            ImGui_ImplVulkan_Shutdown();
            m_imguiInitialized = false;
        }

        destroyScene();
        destroyEasu();
        destroySwapchain();

        // Light SSBO (destroy while the VMA allocator is still alive)
        if (m_lightBuf) { vmaDestroyBuffer(m_vmaAllocator, m_lightBuf, m_lightAlloc);
                            m_lightBuf = VK_NULL_HANDLE; m_lightAlloc = VK_NULL_HANDLE; }

        // Destroy VMA allocator before device teardown
        if (m_vmaAllocator) {
            vmaDestroyAllocator(m_vmaAllocator);
            m_vmaAllocator = VK_NULL_HANDLE;
        }

        if (m_frameTimeline) { vkDestroySemaphore(m_device, m_frameTimeline, nullptr); m_frameTimeline = VK_NULL_HANDLE; }
        if (m_imageAvailSem) { vkDestroySemaphore(m_device, m_imageAvailSem, nullptr); m_imageAvailSem = VK_NULL_HANDLE; }
        if (m_renderDoneSem) { vkDestroySemaphore(m_device, m_renderDoneSem, nullptr); m_renderDoneSem = VK_NULL_HANDLE; }
        if (m_device) { vkDestroyDevice(m_device, nullptr); m_device = VK_NULL_HANDLE; }
        if (m_surface) { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
        if (m_instance) { vkDestroyInstance(m_instance, nullptr); m_instance = VK_NULL_HANDLE; }
        if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
        glfwTerminate();  // reset GLFW global hints (e.g. GLFW_CLIENT_API=GLFW_NO_API)
                          // so a subsequent GL backend's glfwCreateWindow gets an OpenGL context
    }

    // =====================================================================
    // FRAME LIFECYCLE
    // =====================================================================
    bool beginFrame() override {
        // CPU waits for the GPU to finish frame (m_frame - kMaxFrames + 1) so
        // we don't overrun the per-frame command buffer ring. Timeline
        // semaphores replace vkWaitForFences — the CPU sleeps efficiently via
        // the driver's internal wait without the fence-to-semaphore hop.
        if (m_frame >= kMaxFrames) {
            uint64_t waitValue = m_timelineValue - kMaxFrames + 1;
            VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            waitInfo.semaphoreCount = 1;
            waitInfo.pSemaphores = &m_frameTimeline;
            waitInfo.pValues = &waitValue;
            vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX);
        }
        VkResult res = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                              m_imageAvailSem, VK_NULL_HANDLE, &m_imageIndex);
        if (res != VK_SUCCESS) return false;
        m_frameRecording = false;
        return true;
    }

    void endFrame() override {
        if (!m_frameRecording) return;

        // Transition the swapchain image from COLOR_ATTACHMENT_OPTIMAL (left
        // there by the last vkCmdEndRendering in renderFrameScene/renderFrameImGui)
        // to PRESENT_SRC_KHR so the present operation in vkQueuePresentKHR sees
        // the correct layout.  This runs once per frame, after ALL scene + ImGui
        // rendering is finished.
        if (m_swapImages.size() > m_imageIndex) {
            VkImageMemoryBarrier2 presBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            presBarrier.image = m_swapImages[m_imageIndex];
            presBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            presBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            presBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            presBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            presBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            presBarrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            presBarrier.dstAccessMask = 0;
            VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers = &presBarrier;
            vkCmdPipelineBarrier2(m_cmdBufs[m_frame], &depInfo);
        }

        vkEndCommandBuffer(m_cmdBufs[m_frame]);
        VkSubmitInfo sub{};
        sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waits[] = {m_imageAvailSem};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        sub.waitSemaphoreCount = 1;
        sub.pWaitSemaphores = waits;
        sub.pWaitDstStageMask = waitStages;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &m_cmdBufs[m_frame];
        VkSemaphore signals[] = {m_renderDoneSem, m_frameTimeline};
        sub.signalSemaphoreCount = 2;
        sub.pSignalSemaphores = signals;
        VkTimelineSemaphoreSubmitInfo timelineInfo{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timelineInfo.waitSemaphoreValueCount = 0;  // no waits on this submit
        timelineInfo.signalSemaphoreValueCount = 1;
        uint64_t signalVal = m_timelineValue + 1;
        timelineInfo.pSignalSemaphoreValues = &signalVal;
        sub.pNext = &timelineInfo;
        vkQueueSubmit(m_queue, 1, &sub, VK_NULL_HANDLE);
        m_timelineValue = signalVal;

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = signals;
        VkSwapchainKHR swaps[] = {m_swapchain};
        present.swapchainCount = 1;
        present.pSwapchains = swaps;
        present.pImageIndices = &m_imageIndex;
        VkResult pr = vkQueuePresentKHR(m_queue, &present);
        (void)pr;
        m_frame = (m_frame + 1) % kMaxFrames;
        m_frameRendered = false;
    }

    // =====================================================================
    // OFFSCREEN TRIANGLE (headless verification)
    // =====================================================================
    bool renderOffscreenTriangle(int w, int h, unsigned char* outRGBA) override {
        ensureTriangleResources();
        if (!m_triPipeline) return false;

        // Create offscreen image + FBO for this render
        VkImage offImg; VmaAllocation offMem; VkImageView offView;
        if (!createOffscreenTarget(w, h, offImg, offMem, offView))
            return false;

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_cmdPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        transitionImageLayout(cmd, offImg, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = offView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{0.1f, 0.1f, 0.1f, 1.0f}};
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAtt;
        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_triPipeline);
        // Push triangle vertex positions via push constants (triangle.vert has no VB)
        struct TriPC { float pos[2]; } triPc[3] = {
            {{-0.5f, -0.5f}}, {{0.5f, -0.5f}}, {{0.0f, 0.5f}}
        };
        vkCmdPushConstants(cmd, m_triLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(float) * 6, &triPc[0]);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);

        transitionImageLayout(cmd, offImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        // Readback
        VkBuffer readBuf; VmaAllocation readMem;
        createBuffer(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readBuf, readMem);

        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyImageToBuffer(cmd, offImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                readBuf, 1, &bic);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);

        void* data;
        vmaMapMemory(m_vmaAllocator, readMem, &data);
        std::memcpy(outRGBA, data, w * h * 4);
        vmaUnmapMemory(m_vmaAllocator, readMem);

        FlipRows(outRGBA, w, h);

        vmaDestroyBuffer(m_vmaAllocator, readBuf, readMem);
        vkDestroyImageView(m_device, offView, nullptr);
        vmaDestroyImage(m_vmaAllocator, offImg, offMem);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
        return true;
    }

    // =====================================================================
    // OFFSCREEN 3D SCENE
    // =====================================================================
    bool renderOffscreenScene(int w, int h, const OffscreenScene& scene,
                              unsigned char* outRGBA,
                              float* outVelocityRG = nullptr) override {
        if (scene.instances.empty() && scene.meshes.empty()) return false;
        ensureSceneResources(w, h);
        ensureTriangleResources();

        uploadSceneUBO(scene, w, h);
        uploadInstanceData(scene);   // cube instances at offset 0
        uploadMeshData(scene);       // mesh data appended after

        VkImage offImg; VmaAllocation offMem; VkImageView offView;
        VkImage offDepthImg; VmaAllocation offDepthMem; VkImageView offDepthView;
        if (!createOffscreenTarget(w, h, offImg, offMem, offView,
                                   true, &offDepthImg, &offDepthMem, &offDepthView))
            return false;

        // Phase 3b: second color attachment (RG32f) for screen-space motion
        // vectors. Color (loc0) output is untouched, so GL<->Vulkan color
        // parity is unaffected; the velocity attachment is written only by the
        // mesh/pbr fragment shaders (cubes leave it cleared to 0).
        VkImage velImg; VmaAllocation velMem; VkImageView velView;
        {
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R32G32_SFLOAT;
            ici.extent = {(uint32_t)w, (uint32_t)h, 1};
            ici.mipLevels = 1; ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &ici, &aci, &velImg, &velMem, nullptr);
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = velImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R32G32_SFLOAT;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &vci, nullptr, &velView);
        }

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_cmdPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        transitionImageLayout(cmd, offImg, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transitionImageLayout(cmd, offDepthImg, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        transitionImageLayout(cmd, velImg, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // Dynamic rendering (Vulkan 1.3) — no render pass/framebuffer needed
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = offView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{scene.clearColor[0], scene.clearColor[1],
                                      scene.clearColor[2], scene.clearColor[3]}};

        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = offDepthView;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.clearValue.depthStencil = {1.0f, 0};

        // Phase 3b: velocity render target (cleared to 0 = "no motion").
        VkRenderingAttachmentInfo velAtt{};
        velAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        velAtt.imageView = velView;
        velAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        velAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        velAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        velAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        VkRenderingAttachmentInfo colorAtts[2] = { colorAtt, velAtt };
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 2;
        renderingInfo.pColorAttachments = colorAtts;
        renderingInfo.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Instanced cubes — fetched from global SSBOs, no vertex buffer binding
        if (!scene.instances.empty() && m_scenePipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                    0, 1, &m_bindlessDescSet, 0, nullptr);
            // Push textureId = 0 (white texture) for cubes
            uint32_t whiteTexId = 0;
            vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(uint32_t), &whiteTexId);
            vkCmdDraw(cmd, m_cubeVertexCount, (uint32_t)scene.instances.size(),
                      m_cubeFirstVertex, m_cubeFirstInstance);
        }

        // Generic meshes — PBR path (pbr_mesh.*) when scene.pbrEnabled,
        // otherwise the lambert mesh pipeline. Both share the same vertex
        // layout, texture push-constant (textureId) and bindless descriptor
        // set, so only the shader differs (Cook-Torrance+ACES vs lambert).
        // The cube path above stays solid-color on both backends.
        drawMeshes(cmd, scene,
                   scene.pbrEnabled ? m_pbrMeshPipeline : m_meshPipeline);

        vkCmdEndRendering(cmd);

        transitionImageLayout(cmd, offImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionImageLayout(cmd, velImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        // Readback (color and/or velocity). Either buffer may be requested;
        // when a buffer is not requested (nullptr) we skip its copy+map. The
        // offscreen image is still rendered to for both attachments, but only
        // the requested buffers are transferred to host memory.
        VkBuffer readBuf = VK_NULL_HANDLE; VmaAllocation readMem = VK_NULL_HANDLE;
        VkBuffer velReadBuf = VK_NULL_HANDLE; VmaAllocation velReadMem = VK_NULL_HANDLE;
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        if (outRGBA) {
            createBuffer(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         readBuf, readMem);
            vkCmdCopyImageToBuffer(cmd, offImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readBuf, 1, &bic);
        }
        if (outVelocityRG) {
            createBuffer(w * h * 2 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         velReadBuf, velReadMem);
            vkCmdCopyImageToBuffer(cmd, velImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   velReadBuf, 1, &bic);
        }

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);

        if (outRGBA) {
            void* data;
            vmaMapMemory(m_vmaAllocator, readMem, &data);
            std::memcpy(outRGBA, data, w * h * 4);
            vmaUnmapMemory(m_vmaAllocator, readMem);
            FlipRows(outRGBA, w, h);
        }
        if (outVelocityRG) {
            void* vdata;
            vmaMapMemory(m_vmaAllocator, velReadMem, &vdata);
            std::memcpy(outVelocityRG, vdata, w * h * 2 * sizeof(float));
            vmaUnmapMemory(m_vmaAllocator, velReadMem);
            RHI::FlipRows(outVelocityRG, w, h);
        }

        if (readMem) vmaDestroyBuffer(m_vmaAllocator, readBuf, readMem);
        if (velReadMem) vmaDestroyBuffer(m_vmaAllocator, velReadBuf, velReadMem);
        vkDestroyImageView(m_device, offView, nullptr);
        vmaDestroyImage(m_vmaAllocator, offImg, offMem);
        vkDestroyImageView(m_device, velView, nullptr);
        vmaDestroyImage(m_vmaAllocator, velImg, velMem);
        if (offDepthView) { vkDestroyImageView(m_device, offDepthView, nullptr); offDepthView = VK_NULL_HANDLE; }
        if (offDepthImg || offDepthMem) vmaDestroyImage(m_vmaAllocator, offDepthImg, offDepthMem);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
        return true;
    }

    // =====================================================================
    // FSR3 EASU SPATIAL UPSCALE (Phase 4) — headless, no swapchain required
    // =====================================================================
    // EASU/RCAS constants are now in RHI::makeEasuCon / RHI::makeRcasCon
    // (RHIMath.h) so they are unit-testable from the test runner without a
    // Vulkan device.  The local copies here were removed to avoid drift.
    void ensureEasuResources() {
        if (m_easuPipeline) return;

        // Linear-clamp sampler (s_LinearClamp @1000) for the low-res input.
        // FIX: negative mipmap bias — the low-res render target is half the
        // swapchain resolution, so the GPU's automatic LOD selection picks
        // lower-resolution mipmaps, making the base scene blurry before EASU.
        // A -0.5 bias forces the GPU to clamp toward the highest-resolution
        // mip level, feeding sharp detail into the edge-directed upscaler.
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.mipLodBias = -0.5f;
        sci.minLod = 0.0f;
        vkCreateSampler(m_device, &sci, nullptr, &m_easuSampler);

        // Nearest-clamp sampler for depth reads in the temporal resolve shader.
        // Linear filtering on depth across geometry edges produces phantom Z
        // values that corrupt reprojection; point sampling gives exact depth.
        VkSamplerCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        dsci.magFilter = VK_FILTER_NEAREST;
        dsci.minFilter = VK_FILTER_NEAREST;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(m_device, &dsci, nullptr, &m_depthPointSampler);

        // Descriptor set: binding 0 SAMPLED_IMAGE (low-res color), 1000 SAMPLER,
        // 2001 STORAGE_IMAGE (upscaled output; internal@2000 aliased to 2001),
        // 3000 UNIFORM_BUFFER (EASU constants, std140, 80B).
        VkDescriptorSetLayoutBinding bl[4]{};
        bl[0].binding = 0;   bl[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bl[0].descriptorCount = 1; bl[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bl[1].binding = 1000; bl[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bl[1].descriptorCount = 1; bl[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bl[2].binding = 2001; bl[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bl[2].descriptorCount = 1; bl[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bl[3].binding = 3000; bl[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bl[3].descriptorCount = 1; bl[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dlci{};
        dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dlci.bindingCount = 4; dlci.pBindings = bl;
        vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_easuDescLayout);

        VkDescriptorPoolSize dps[4]{};
        dps[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   dps[0].descriptorCount = 1;
        dps[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;         dps[1].descriptorCount = 1;
        dps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;    dps[2].descriptorCount = 1;
        dps[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;   dps[3].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 4; dpci.pPoolSizes = dps;
        vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_easuDescPool);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = m_easuDescPool;
        dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_easuDescLayout;
        vkAllocateDescriptorSets(m_device, &dsai, &m_easuDescSet);

        // RCAS gets its OWN descriptor set (allocated from a dedicated pool, same layout)
        // so it does NOT stomp m_easuDescSet while EASU's recorded binds still reference it
        // within the same command buffer. RCAS binds 0/2001/3000; the 1000 sampler slot is
        // present in the layout but unused by the float path.
        VkDescriptorPoolCreateInfo rcasPool{};
        rcasPool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        rcasPool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        rcasPool.maxSets = 1; rcasPool.poolSizeCount = 4; rcasPool.pPoolSizes = dps;
        vkCreateDescriptorPool(m_device, &rcasPool, nullptr, &m_rcasDescPool);
        VkDescriptorSetAllocateInfo rcasDsai{};
        rcasDsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        rcasDsai.descriptorPool = m_rcasDescPool;
        rcasDsai.descriptorSetCount = 1; rcasDsai.pSetLayouts = &m_easuDescLayout;
        vkAllocateDescriptorSets(m_device, &rcasDsai, &m_rcasDescSet);

        // Pipeline layout (compute set only, with push constants).
        // FIX: Open up push constant visibility so EASU can read the viewport size metrics
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(float) * 4; // 16 bytes allocation capacity

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; 
        plci.pSetLayouts = &m_easuDescLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &plci, nullptr, &m_easuPipelineLayout);

        // Compute pipeline (canonical AMD EASU, compiled by the Makefile .comp rule).
        auto spv = loadSpirv("build/rhi/fsr3_easu.comp.spv");
        if (spv.empty()) { std::cerr << "[RHI-Vk] EASU: missing build/rhi/fsr3_easu.comp.spv\n"; return; }
        VkShaderModule sm = createShaderModule(m_device, spv);
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm; cpci.stage.pName = "main";
        cpci.layout = m_easuPipelineLayout;
        VkResult pr = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_easuPipeline);
        vkDestroyShaderModule(m_device, sm, nullptr);
        if (pr != VK_SUCCESS) { std::cerr << "[RHI-Vk] EASU compute pipeline FAILED: " << pr << "\n"; return; }

        // UBO (std140: 5 x uvec4 = 80B), persistently mapped. EASU writes its own
        // const0..const3 here and RCAS writes its own rcas con into a SEPARATE
        // m_rcasConUBO (see below) — sharing one buffer across EASU+RCAS in the
        // same command buffer would let the GPU read RCAS's constants when
        // executing EASU (all host writes happen during recording, before
        // submit, so the final write wins at dispatch-execution time). Same
        // reasoning gives temporal its own UBO.
        createBuffer(sizeof(uint32_t) * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_easuConUBO, m_easuConUBOMem);
        vmaMapMemory(m_vmaAllocator, m_easuConUBOMem, &m_easuConMapped);

        // RCAS UBO (const3.x = sharpness; see makeRcasCon). Separate so the
        // RCAS const upload can't clobber the EASU consts during a single
        // frame's command-buffer recording.
        createBuffer(sizeof(uint32_t) * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_rcasConUBO, m_rcasConUBOMem);
        vmaMapMemory(m_vmaAllocator, m_rcasConUBOMem, &m_rcasConMapped);

        // RCAS compute pipeline. Reuses the EASU descriptor set layout (bindings
        // 0/1000/2001/3000 — RCAS aliases its output UAV to 2001 and reads its
        // input from binding 0) so no second set layout/pool/set is needed.
        // (The RCAS descriptor SET is separate from the EASU one — see below.)
        auto rcasSpv = loadSpirv("build/rhi/fsr3_rcas.comp.spv");
        if (rcasSpv.empty()) { std::cerr << "[RHI-Vk] RCAS: missing build/rhi/fsr3_rcas.comp.spv\n"; return; }
        VkShaderModule rsm = createShaderModule(m_device, rcasSpv);
        VkComputePipelineCreateInfo rcpci{};
        rcpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        rcpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        rcpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; rcpci.stage.module = rsm; rcpci.stage.pName = "main";
        rcpci.layout = m_easuPipelineLayout;
        VkResult rcpr = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &rcpci, nullptr, &m_rcasPipeline);
        vkDestroyShaderModule(m_device, rsm, nullptr);
        if (rcpr != VK_SUCCESS) { std::cerr << "[RHI-Vk] RCAS compute pipeline FAILED: " << rcpr << "\n"; return; }

        // --- Temporal resolve (Phase 5c): reproject + accumulate. ---
        // Dedicated set layout (7 bindings: 3 sampled images + 2 samplers +
        // 1 storage image + UBO) — NOT shared with EASU/RCAS (extra sampled
        // views, a second depth sampler, and a different UBO struct). Pipeline
        // layout has one compute set.
        VkDescriptorSetLayoutBinding tb[7]{};
        tb[0].binding = 0;   tb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        tb[0].descriptorCount = 1; tb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[1].binding = 1;   tb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        tb[1].descriptorCount = 1; tb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[2].binding = 2;   tb[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        tb[2].descriptorCount = 1; tb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[3].binding = 1000; tb[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        tb[3].descriptorCount = 1; tb[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[4].binding = 1001; tb[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        tb[4].descriptorCount = 1; tb[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[5].binding = 2000; tb[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tb[5].descriptorCount = 1; tb[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[6].binding = 3000; tb[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tb[6].descriptorCount = 1; tb[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo tdlci{};
        tdlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        tdlci.bindingCount = 7; tdlci.pBindings = tb;
        vkCreateDescriptorSetLayout(m_device, &tdlci, nullptr, &m_temporalDescLayout);

        VkDescriptorPoolSize tpools[4]{};
        tpools[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   tpools[0].descriptorCount = 3;
        tpools[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;         tpools[1].descriptorCount = 2;  // linear + depth point
        tpools[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;   tpools[2].descriptorCount = 1;
        tpools[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;  tpools[3].descriptorCount = 1;
        VkDescriptorPoolCreateInfo tdpci{};
        tdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        tdpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        tdpci.maxSets = 1; tdpci.poolSizeCount = 4; tdpci.pPoolSizes = tpools;
        vkCreateDescriptorPool(m_device, &tdpci, nullptr, &m_temporalDescPool);
        VkDescriptorSetAllocateInfo tdsai{};
        tdsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        tdsai.descriptorPool = m_temporalDescPool;
        tdsai.descriptorSetCount = 1; tdsai.pSetLayouts = &m_temporalDescLayout;
        vkAllocateDescriptorSets(m_device, &tdsai, &m_temporalDescSet);

        VkPipelineLayoutCreateInfo tplci{};
        tplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        tplci.setLayoutCount = 1; tplci.pSetLayouts = &m_temporalDescLayout;
        vkCreatePipelineLayout(m_device, &tplci, nullptr, &m_temporalPipelineLayout);

        auto tSpv = loadSpirv("build/rhi/fsr3_temporal.comp.spv");
        if (tSpv.empty()) { std::cerr << "[RHI-Vk] Temporal: missing build/rhi/fsr3_temporal.comp.spv\n"; return; }
        VkShaderModule tsm = createShaderModule(m_device, tSpv);
        VkComputePipelineCreateInfo tcpci{};
        tcpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        tcpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tcpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; tcpci.stage.module = tsm; tcpci.stage.pName = "main";
        tcpci.layout = m_temporalPipelineLayout;
        VkResult tpr = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &tcpci, nullptr, &m_temporalPipeline);
        vkDestroyShaderModule(m_device, tsm, nullptr);
        if (tpr != VK_SUCCESS) { std::cerr << "[RHI-Vk] Temporal compute pipeline FAILED: " << tpr << "\n"; return; }

        // Temporal UBO (std140: mat4 reproject + float feedback + float hist_valid
        // + vec2 depth_uv_scale + vec2 jitterOffset = 88B, padded to 96B by
        // std140 rules). Persistently mapped, separate from EASU/RCAS UBOs.
        createBuffer(sizeof(float) * 24, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_temporalConUBO, m_temporalConUBOMem);
        vmaMapMemory(m_vmaAllocator, m_temporalConUBOMem, &m_temporalConMapped);

        // --- Velocity dilation compute pass (pre-EASU, pre-temporal) ---
        // Dilsates foreground velocity over silhouette edges using a 3x3
        // depth search. Needs the half-res depth texture and a half-res
        // velocity image (RG32F) from the low-res G-buffer pass.
        {
            VkDescriptorSetLayoutBinding vb[5]{};
            vb[0].binding = 0;   vb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;   // inVelocity (RG32F)
            vb[0].descriptorCount = 1; vb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            vb[1].binding = 1;   vb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  // sceneDepth (D32)
            vb[1].descriptorCount = 1; vb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            vb[2].binding = 1000; vb[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;         // s_linear_clamp
            vb[2].descriptorCount = 1; vb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            vb[3].binding = 2;   vb[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;   // outDilated (RG32F)
            vb[3].descriptorCount = 1; vb[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            vb[4].binding = 3000; vb[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;    // cbDilate (vec2 depthUvScale)
            vb[4].descriptorCount = 1; vb[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo vdlci{};
            vdlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            vdlci.bindingCount = 5; vdlci.pBindings = vb;
            vkCreateDescriptorSetLayout(m_device, &vdlci, nullptr, &m_velocityDilateDescLayout);

            VkDescriptorPoolSize vdPools[4]{};
            vdPools[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; vdPools[0].descriptorCount = 2;
            vdPools[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; vdPools[1].descriptorCount = 1;
            vdPools[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;      vdPools[2].descriptorCount = 1;
            vdPools[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; vdPools[3].descriptorCount = 1;
            VkDescriptorPoolCreateInfo vdpci{};
            vdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            vdpci.maxSets = 1; vdpci.poolSizeCount = 4; vdpci.pPoolSizes = vdPools;
            vkCreateDescriptorPool(m_device, &vdpci, nullptr, &m_velocityDilateDescPool);
            VkDescriptorSetAllocateInfo vdsai{};
            vdsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            vdsai.descriptorPool = m_velocityDilateDescPool;
            vdsai.descriptorSetCount = 1; vdsai.pSetLayouts = &m_velocityDilateDescLayout;
            VkResult vdAllocR = vkAllocateDescriptorSets(m_device, &vdsai, &m_velocityDilateDescSet);
            if (vdAllocR != VK_SUCCESS) {
                std::cerr << "[RHI-Vk] VelocityDilate descriptor set allocation FAILED: " << vdAllocR << "\n";
            }

            VkPipelineLayoutCreateInfo vdplci{};
            vdplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            vdplci.setLayoutCount = 1; vdplci.pSetLayouts = &m_velocityDilateDescLayout;
            vkCreatePipelineLayout(m_device, &vdplci, nullptr, &m_velocityDilatePipelineLayout);

            auto vdSpv = loadSpirv("build/rhi/velocity_dilate.comp.spv");
            if (vdSpv.empty()) { std::cerr << "[RHI-Vk] VelocityDilate: missing build/rhi/velocity_dilate.comp.spv\n"; return; }
            VkShaderModule vsm = createShaderModule(m_device, vdSpv);
            VkComputePipelineCreateInfo vdCpci{};
            vdCpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            vdCpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vdCpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; vdCpci.stage.module = vsm; vdCpci.stage.pName = "main";
            vdCpci.layout = m_velocityDilatePipelineLayout;
            VkResult vdPr = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &vdCpci, nullptr, &m_velocityDilatePipeline);
            vkDestroyShaderModule(m_device, vsm, nullptr);
            if (vdPr != VK_SUCCESS) { std::cerr << "[RHI-Vk] VelocityDilate compute pipeline FAILED: " << vdPr << "\n"; return; }
        }
    }

    void destroyEasu() {
        if (m_velocityDilatePipeline) { vkDestroyPipeline(m_device, m_velocityDilatePipeline, nullptr); m_velocityDilatePipeline = VK_NULL_HANDLE; }
        if (m_velocityDilatePipelineLayout) { vkDestroyPipelineLayout(m_device, m_velocityDilatePipelineLayout, nullptr); m_velocityDilatePipelineLayout = VK_NULL_HANDLE; }
        if (m_velocityDilateDescSet) m_velocityDilateDescSet = VK_NULL_HANDLE;
        if (m_velocityDilateDescPool) { vkDestroyDescriptorPool(m_device, m_velocityDilateDescPool, nullptr); m_velocityDilateDescPool = VK_NULL_HANDLE; }
        if (m_velocityDilateDescLayout) { vkDestroyDescriptorSetLayout(m_device, m_velocityDilateDescLayout, nullptr); m_velocityDilateDescLayout = VK_NULL_HANDLE; }
        if (m_velocityDilateUBOMem) {
            if (m_velocityDilateMapped) vmaUnmapMemory(m_vmaAllocator, m_velocityDilateUBOMem);
            m_velocityDilateMapped = nullptr;
            vmaDestroyBuffer(m_vmaAllocator, m_velocityDilateUBO, m_velocityDilateUBOMem);
            m_velocityDilateUBO = VK_NULL_HANDLE; m_velocityDilateUBOMem = VK_NULL_HANDLE;
        }
        if (m_rcasPipeline) { vkDestroyPipeline(m_device, m_rcasPipeline, nullptr); m_rcasPipeline = VK_NULL_HANDLE; }
        if (m_easuPipeline) { vkDestroyPipeline(m_device, m_easuPipeline, nullptr); m_easuPipeline = VK_NULL_HANDLE; }
        if (m_easuPipelineLayout) { vkDestroyPipelineLayout(m_device, m_easuPipelineLayout, nullptr); m_easuPipelineLayout = VK_NULL_HANDLE; }
        if (m_easuDescSet) m_easuDescSet = VK_NULL_HANDLE;
        if (m_easuDescPool) { vkDestroyDescriptorPool(m_device, m_easuDescPool, nullptr); m_easuDescPool = VK_NULL_HANDLE; }
        if (m_rcasDescSet) m_rcasDescSet = VK_NULL_HANDLE;
        if (m_rcasDescPool) { vkDestroyDescriptorPool(m_device, m_rcasDescPool, nullptr); m_rcasDescPool = VK_NULL_HANDLE; }
        if (m_easuDescLayout) { vkDestroyDescriptorSetLayout(m_device, m_easuDescLayout, nullptr); m_easuDescLayout = VK_NULL_HANDLE; }
        if (m_easuSampler) { vkDestroySampler(m_device, m_easuSampler, nullptr); m_easuSampler = VK_NULL_HANDLE; }
        if (m_depthPointSampler) { vkDestroySampler(m_device, m_depthPointSampler, nullptr); m_depthPointSampler = VK_NULL_HANDLE; }
        if (m_easuConUBOMem) {
            if (m_easuConMapped) vmaUnmapMemory(m_vmaAllocator, m_easuConUBOMem);
            m_easuConMapped = nullptr;
            vmaDestroyBuffer(m_vmaAllocator, m_easuConUBO, m_easuConUBOMem);
            m_easuConUBO = VK_NULL_HANDLE; m_easuConUBOMem = VK_NULL_HANDLE;
        }
        if (m_rcasConUBOMem) {
            if (m_rcasConMapped) vmaUnmapMemory(m_vmaAllocator, m_rcasConUBOMem);
            m_rcasConMapped = nullptr;
            vmaDestroyBuffer(m_vmaAllocator, m_rcasConUBO, m_rcasConUBOMem);
            m_rcasConUBO = VK_NULL_HANDLE; m_rcasConUBOMem = VK_NULL_HANDLE;
        }
        if (m_temporalPipeline) { vkDestroyPipeline(m_device, m_temporalPipeline, nullptr); m_temporalPipeline = VK_NULL_HANDLE; }
        if (m_temporalPipelineLayout) { vkDestroyPipelineLayout(m_device, m_temporalPipelineLayout, nullptr); m_temporalPipelineLayout = VK_NULL_HANDLE; }
        if (m_temporalDescSet) m_temporalDescSet = VK_NULL_HANDLE;
        if (m_temporalDescPool) { vkDestroyDescriptorPool(m_device, m_temporalDescPool, nullptr); m_temporalDescPool = VK_NULL_HANDLE; }
        if (m_temporalDescLayout) { vkDestroyDescriptorSetLayout(m_device, m_temporalDescLayout, nullptr); m_temporalDescLayout = VK_NULL_HANDLE; }
        if (m_temporalConUBOMem) {
            if (m_temporalConMapped) vmaUnmapMemory(m_vmaAllocator, m_temporalConUBOMem);
            m_temporalConMapped = nullptr;
            vmaDestroyBuffer(m_vmaAllocator, m_temporalConUBO, m_temporalConUBOMem);
            m_temporalConUBO = VK_NULL_HANDLE; m_temporalConUBOMem = VK_NULL_HANDLE;
        }
    }

    // ---------------------------------------------------------------------
    // Shared EASU dispatch (Phase 4 headless + Phase 5a swapchain). Records the
    // const upload, descriptor set write-binds, UNDEFINED->GENERAL barrier on
    // the storage output and the 8x8-workgroup vkCmdDispatch — the EXACT
    // sequence renderOffscreenSceneEasu used inline. Factoring it out lets the
    // swapchain path reuse the bit-identical EASU math (bit-identical to the
    // headless path, pinned by RHI.Fsr3EasuUpscalesLowResScene).
    void dispatchEasu(VkCommandBuffer cmd, VkImageView lowView,
                      uint32_t rW, uint32_t rH,
                      VkImage outImg, VkImageView outView,
                      uint32_t outW, uint32_t outH,
                      float jitterX, float jitterY) {
        // Upload EASU constants (jitter cancelled: frame 0 -> {0,0} on headless;
        // non-zero on the swapchain path so the upsample lines up with jittered
        // low-res geometry exactly as AMD's temporal path intends).
        const RHI::EasuCon con = RHI::makeEasuCon(rW, rH, outW, outH, jitterX, jitterY);
        if (m_easuConMapped) std::memcpy(m_easuConMapped, con.c, sizeof(con.c));

        VkDescriptorImageInfo inDii{};
        inDii.imageView = lowView;
        inDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo outDii{};
        outDii.imageView = outView;
        outDii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // STORAGE_IMAGE write
        VkDescriptorBufferInfo conInfo{};
        conInfo.buffer = m_easuConUBO; conInfo.offset = 0; conInfo.range = sizeof(uint32_t) * 16;
        VkDescriptorImageInfo samDii{};
        samDii.sampler = m_easuSampler;
        VkWriteDescriptorSet writes[4]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_easuDescSet; writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &inDii;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_easuDescSet; writes[1].dstBinding = 1000;
        writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &samDii;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_easuDescSet; writes[2].dstBinding = 2001;
        writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &outDii;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_easuDescSet; writes[3].dstBinding = 3000;
        writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[3].pBufferInfo = &conInfo;
        vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_easuPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_easuPipelineLayout,
                                0, 1, &m_easuDescSet, 0, nullptr);
                                
                                
// FIX: Pack the push constant layout matching cluster_cull's uniform block spacing
// totalClusters [uint32] -> viewportW [float] -> viewportH [float] -> lodPixelError [float]
/*float pushData[4] = { 
            0.0f,               // totalClusters (Unused by EASU, acts as structural padding)
            (float)outW,        // viewportW (Full-resolution target width)
            (float)outH,        // viewportH (Full-resolution target height)
            1.0f                // lodPixelError (Trailing configuration padding)
        };  

        // Dispatch the data alignment mapping to the GPU
        vkCmdPushConstants(cmd, m_easuPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushData), pushData);*/

        // The output storage image starts UNDEFINED (or TRANSFER_SRC from the
        // previous frame — UNDEFINED as oldLayout discards and we overwrite every
        // pixel in EASU, so no layout bookkeeping is required). Put it in GENERAL
        // so the imageStore() writes are defined.
        VkImageMemoryBarrier2 inBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        inBar.image = outImg;
        inBar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        inBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        inBar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        inBar.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        inBar.srcAccessMask = 0;
        inBar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        inBar.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo inDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        inDep.imageMemoryBarrierCount = 1; inDep.pImageMemoryBarriers = &inBar;
        vkCmdPipelineBarrier2(cmd, &inDep);
        // AMD's EASU: 64-thread 1D workgroup via ffxRemapForQuad (called inside
        // the canonical EASU() function). Each group covers a 16x16 output tile.
        // Dispatch (outW+15)/16 x (outH+15)/16.
        const uint32_t groupX = (outW + 15u) / 16u;
        const uint32_t groupY = (outH + 15u) / 16u;
        vkCmdDispatch(cmd, groupX, groupY, 1);
    }

    // RCAS dispatch: reads the EASU float output (SRV@0, SHADER_READ_ONLY via
    // texelFetch) and writes a sharpened full-res float image (UAV@2001,
    // GENERAL). Reuses the EASU descriptor set layout/pipeline layout/set —
    // only bindings 0/SRV, 2001/STORAGE and 3000/CB are re-bound; the sampler
    // (1000) is left as-is (unused by the float path). RCAS is a 64-thread 1D
    // group tiling 16x16, so dispatch is (outW+15)/16 x (outH+15)/16 (same as EASU).
    void dispatchRcas(VkCommandBuffer cmd, VkImageView easuView,
                      VkImage easuImg, VkImageView outView,
                      VkImage outImg, uint32_t outW, uint32_t outH,
                      float sharpnessStops) {
        const RHI::EasuCon rcon = RHI::makeRcasCon(sharpnessStops);
        if (m_rcasConMapped) std::memcpy(m_rcasConMapped, rcon.c, sizeof(rcon.c));

        VkDescriptorImageInfo inDii{};
        inDii.imageView = easuView;
        inDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // texelFetch source
        VkDescriptorImageInfo outDii{};
        outDii.imageView = outView;
        outDii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;                  // STORAGE_IMAGE write
        VkDescriptorBufferInfo conInfo{};
        conInfo.buffer = m_rcasConUBO; conInfo.offset = 0; conInfo.range = sizeof(uint32_t) * 16;
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_rcasDescSet; writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &inDii;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_rcasDescSet; writes[1].dstBinding = 2001;
        writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &outDii;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_rcasDescSet; writes[2].dstBinding = 3000;
        writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &conInfo;
        vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_rcasPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_easuPipelineLayout,
                                0, 1, &m_rcasDescSet, 0, nullptr);
        // Barriers for this dispatch are recorded inline by the caller (renderFrameScene)
        // so EASU/RCAS barriers stay explicit and visible — no hidden layout transitions.
        const uint32_t groupX = (outW + 15u) / 16u;
        const uint32_t groupY = (outH + 15u) / 16u;
        vkCmdDispatch(cmd, groupX, groupY, 1);
        (void)easuImg; (void)outImg;
    }

    // Temporal resolve constants (std140, mirrored by cbTemporal_t in
    // rhi/shaders/fsr3_temporal.comp). The CPU precomputes the single
    // reprojection matrix R = prevViewProj * inverse(viewProj) so the shader
    // only multiplies once per pixel. depth_uv_scale maps a full-resolution
    // framebuffer uv into the half-res depth texture's uv (here: identity,
    // because the half-res depth image is sampled with 0..1 uvs and
    // hardware-bilinear-upscaled by the GPU). jitterOffset is the current
    // frame's Halton sub-pixel jitter (NDC units) — the temporal shader
    // subtracts it from the pixel uv before reprojection to cancel the jitter
    // applied to the camera projection.
    struct TemporalCon { Mat4 reproject; float feedback; float histValid; Vec2 depthUvScale; Vec2 jitterOffset; };
    static TemporalCon makeTemporalCon(const Mat4& currViewProj, const Mat4& prevViewProj,
                                       bool histValid, float feedback, const Vec2& jitter) {
        const Mat4 invVP = mat4Inverse(currViewProj);
        return TemporalCon{ mat4Multiply(prevViewProj, invVP), feedback,
                            histValid ? 1.0f : 0.0f, Vec2{1.0f, 1.0f}, jitter };
    }

    // RCAS reads currColor (SRV@0) + prev history (SRV@1) + half-res depth
    // (SRV@2); the linear sampler@1000 is for color/history, the depth point
    // sampler@1001 is for the half-res depth texture. Writes next history
    // (STORAGE_IMAGE@2000). Reproject uses R (prevViewProj*inv(viewProj)) so the
    // history accumulates the PREVIOUS frame reprojected onto the CURRENT
    // frame — stabilizing the jittered low-res render across samples. The
    // caller (renderFrameScene) records the layout barriers around this call so
    // the curr/hist read and hist-write transitions stay explicit.
    void dispatchTemporal(VkCommandBuffer cmd, VkImageView currView, VkImageView prevHistView,
                          VkImageView depthView, VkImageView nextHistView,
                          uint32_t outW, uint32_t outH, const TemporalCon& con) {
        if (m_temporalConMapped) std::memcpy(m_temporalConMapped, &con, sizeof(con));

        VkDescriptorImageInfo currDii{};   currDii.imageView = currView;   currDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo histDii{};   histDii.imageView = prevHistView; histDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo depthDii{};  depthDii.imageView = depthView; depthDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo outDii{};    outDii.imageView = nextHistView; outDii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // STORAGE_IMAGE write
        VkDescriptorImageInfo samDii{};    samDii.sampler = m_easuSampler;
        VkDescriptorImageInfo depthSamDii{}; depthSamDii.sampler = m_depthPointSampler;
        VkDescriptorBufferInfo conInfo{};  conInfo.buffer = m_temporalConUBO; conInfo.offset = 0; conInfo.range = sizeof(TemporalCon);
        VkWriteDescriptorSet writes[6]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_temporalDescSet; writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &currDii;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_temporalDescSet; writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &histDii;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_temporalDescSet; writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2].pImageInfo = &depthDii;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_temporalDescSet; writes[3].dstBinding = 1000;
        writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[3].pImageInfo = &samDii;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = m_temporalDescSet; writes[4].dstBinding = 1001;   // depth point sampler
        writes[4].descriptorCount = 1; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[4].pImageInfo = &depthSamDii;
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = m_temporalDescSet; writes[5].dstBinding = 2000;
        writes[5].descriptorCount = 1; writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[5].pImageInfo = &outDii;
        VkWriteDescriptorSet wUBO{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wUBO.dstSet = m_temporalDescSet; wUBO.dstBinding = 3000;
        wUBO.descriptorCount = 1; wUBO.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wUBO.pBufferInfo = &conInfo;
        const VkWriteDescriptorSet allWrites[] = { writes[0], writes[1], writes[2], writes[3], writes[4], writes[5], wUBO };
        vkUpdateDescriptorSets(m_device, 7, allWrites, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipelineLayout,
                                0, 1, &m_temporalDescSet, 0, nullptr);
        const uint32_t groupX = (outW + 7u) / 8u;
        const uint32_t groupY = (outH + 7u) / 8u;
        vkCmdDispatch(cmd, groupX, groupY, 1);
    }

    // Velocity dilation compute pass: dilates foreground velocity vectors
    // over silhouette edges using a 3x3 depth search. Must be called AFTER
    // the low-res G-buffer has been rendered and AFTER velocity+depth have
    // been transitioned to SHADER_READ_ONLY. Barriers are the caller's
    // responsibility (renderFrameScene records them inline).
    void dispatchVelocityDilation(VkCommandBuffer cmd,
                                  VkImageView inVelView, VkImageView depthView,
                                  VkImageView outVelView,
                                  uint32_t w, uint32_t h) {
        VkDescriptorImageInfo inDii{};
        inDii.imageView = inVelView;
        inDii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // STORAGE_IMAGE read
        VkDescriptorImageInfo depthDii{};
        depthDii.imageView = depthView;
        depthDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo outDii{};
        outDii.imageView = outVelView;
        outDii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // STORAGE_IMAGE write
        VkDescriptorImageInfo samDii{};
        samDii.sampler = m_easuSampler;  // linear, clamp
        VkDescriptorBufferInfo conInfo{};
        conInfo.buffer = m_velocityDilateUBO; conInfo.offset = 0; conInfo.range = sizeof(Vec2);

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_velocityDilateDescSet; writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &inDii;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_velocityDilateDescSet; writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &depthDii;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_velocityDilateDescSet; writes[2].dstBinding = 1000;
        writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[2].pImageInfo = &samDii;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_velocityDilateDescSet; writes[3].dstBinding = 2;
        writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo = &outDii;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = m_velocityDilateDescSet; writes[4].dstBinding = 3000;
        writes[4].descriptorCount = 1; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &conInfo;
        vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_velocityDilatePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_velocityDilatePipelineLayout,
                                0, 1, &m_velocityDilateDescSet, 0, nullptr);
        const uint32_t groupX = (w + 15u) / 16u;
        const uint32_t groupY = (h + 15u) / 16u;
        vkCmdDispatch(cmd, groupX, groupY, 1);
    }
    // full-res EASU float scratch image). Reused every frame, rebuilt when the
    // swapchain changes size.
    void ensureEasuFrameResources() {
        const int wantW = (int)m_swapWidth / 2, wantH = (int)m_swapHeight / 2;
        if (m_easuFloatImage && m_rcasFloatImage && m_lowColorImage && m_lowDepthImage &&
            m_histColorImage[0] && m_histColorImage[1] &&
            m_lowVelocityImage &&
            m_lowScenePipeline && m_lowMeshPipeline && m_lowPbrMeshPipeline &&
            m_velocityDilatePipeline &&
            m_lowW == wantW && m_lowH == wantH)
            return;
        destroyEasuFrameResources();
        m_lowW = wantW ? wantW : 1;
        m_lowH = wantH ? wantH : 1;

        // Low-res color target (RGBA8_UNORM, sampled by EASU).
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)m_lowW, (uint32_t)m_lowH, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &ici, &aci,
                       &m_lowColorImage, &m_lowColorAlloc, nullptr);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = m_lowColorImage; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &m_lowColorView);

        // Low-res depth (D32_SFLOAT).
        VkImageCreateInfo dici{};
        dici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = VK_FORMAT_D32_SFLOAT;
        dici.extent = {(uint32_t)m_lowW, (uint32_t)m_lowH, 1};
        dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;   // SAMPLED: Phase 5c temporal resolve reads this half-res depth
        VmaAllocationCreateInfo daci{}; daci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &dici, &daci,
                       &m_lowDepthImage, &m_lowDepthAlloc, nullptr);
        VkImageViewCreateInfo dvci{};
        dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvci.image = m_lowDepthImage; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = VK_FORMAT_D32_SFLOAT;
        dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &dvci, nullptr, &m_lowDepthView);

        // Half-res velocity target (RG32F) — written by the velocity-aware
        // low-res scene/mesh/pbr pipelines, read by the velocity dilation pass.
        VkImageCreateInfo vlici{};
        vlici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        vlici.imageType = VK_IMAGE_TYPE_2D;
        vlici.format = VK_FORMAT_R32G32_SFLOAT;
        vlici.extent = {(uint32_t)m_lowW, (uint32_t)m_lowH, 1};
        vlici.mipLevels = 1; vlici.arrayLayers = 1;
        vlici.samples = VK_SAMPLE_COUNT_1_BIT;
        vlici.tiling = VK_IMAGE_TILING_OPTIMAL;
        vlici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_STORAGE_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vlici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo vlaci{}; vlaci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &vlici, &vlaci,
                       &m_lowVelocityImage, &m_lowVelocityAlloc, nullptr);
        VkImageViewCreateInfo vlvci{};
        vlvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vlvci.image = m_lowVelocityImage; vlvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vlvci.format = VK_FORMAT_R32G32_SFLOAT;
        vlvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vlvci, nullptr, &m_lowVelocityView);

        // Dilated velocity output (RG32F, half-res) — written by the dilation
        // compute pass, available for the temporal resolver.
        VkImageCreateInfo dvdci{};
        dvdci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        dvdci.imageType = VK_IMAGE_TYPE_2D;
        dvdci.format = VK_FORMAT_R32G32_SFLOAT;
        dvdci.extent = {(uint32_t)m_lowW, (uint32_t)m_lowH, 1};
        dvdci.mipLevels = 1; dvdci.arrayLayers = 1;
        dvdci.samples = VK_SAMPLE_COUNT_1_BIT;
        dvdci.tiling = VK_IMAGE_TILING_OPTIMAL;
        dvdci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        dvdci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo dvdaci{}; dvdaci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &dvdci, &dvdaci,
                       &m_dilatedVelocityImage, &m_dilatedVelocityAlloc, nullptr);
        VkImageViewCreateInfo dvdvci{};
        dvdvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvdvci.image = m_dilatedVelocityImage; dvdvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvdvci.format = VK_FORMAT_R32G32_SFLOAT;
        dvdvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &dvdvci, nullptr, &m_dilatedVelocityView);

        // UBO for the dilation pass: depthUvScale = {1,1} (half-res depth is
        // sampled at 0..1 UVs, same size as the velocity image).
        createBuffer(8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_velocityDilateUBO, m_velocityDilateUBOMem);
        vmaMapMemory(m_vmaAllocator, m_velocityDilateUBOMem, &m_velocityDilateMapped);
        if (m_velocityDilateMapped) {
            Vec2 scale{1.0f, 1.0f};
            std::memcpy(m_velocityDilateMapped, &scale, sizeof(scale));
        }

        // Full-res EASU float scratch (R32G32B32A32_SFLOAT, storage -> blit src).
        // ALIAS_BIT: RCAS float image aliases this allocation (they never overlap
        // in time — RCAS reads EASU's output via texelFetch then writes its own
        // output to the aliased block).
        VkImageCreateInfo fici{};
        fici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        fici.flags = VK_IMAGE_CREATE_ALIAS_BIT;
        fici.imageType = VK_IMAGE_TYPE_2D;
        fici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        fici.extent = {(uint32_t)m_swapWidth, (uint32_t)m_swapHeight, 1};
        fici.mipLevels = 1; fici.arrayLayers = 1;
        fici.samples = VK_SAMPLE_COUNT_1_BIT;
        fici.tiling = VK_IMAGE_TILING_OPTIMAL;
        fici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT;  // SAMPLED: RCAS reads EASU output via texelFetch
        fici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo fci{}; fci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &fici, &fci,
                       &m_easuFloatImage, &m_easuFloatAlloc, nullptr);
        VkImageViewCreateInfo fvci{};
        fvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        fvci.image = m_easuFloatImage; fvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fvci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        fvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &fvci, nullptr, &m_easuFloatView);

        // Second full-res R32G32B32A32 float scratch: the RCAS output. RCAS is a
        // 3x3-neighborhood sharpen (not in-place safe), so it reads the EASU
        // float output (SRV@0) and writes here; the blit to the swapchain is then
        // sourced from this RCAS output. Same extent/usage as m_easuFloatImage.
        // ALIAS_BIT: this image shares the SAME VMA allocation as
        // m_easuFloatImage — they are never live at the same time, so aliasing
        // halves the VRAM footprint of the upscaling scratch pool.
        VkImageCreateInfo rcici{};
        rcici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        rcici.flags = VK_IMAGE_CREATE_ALIAS_BIT;
        rcici.imageType = VK_IMAGE_TYPE_2D;
        rcici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        rcici.extent = {(uint32_t)m_swapWidth, (uint32_t)m_swapHeight, 1};
        rcici.mipLevels = 1; rcici.arrayLayers = 1;
        rcici.samples = VK_SAMPLE_COUNT_1_BIT;
        rcici.tiling = VK_IMAGE_TILING_OPTIMAL;
        rcici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;  // SAMPLED: temporal shader reads RCAS output via texture()
        rcici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Create the image without allocating (separate memory) — we bind
        // it to the EASU allocation below.
        vkCreateImage(m_device, &rcici, nullptr, &m_rcasFloatImage);
        vmaBindImageMemory(m_vmaAllocator, m_easuFloatAlloc, m_rcasFloatImage);
        VkImageViewCreateInfo rcvci{};
        rcvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        rcvci.image = m_rcasFloatImage; rcvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        rcvci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        rcvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &rcvci, nullptr, &m_rcasFloatView);

        // Full-res history color (RGBA16F, ping-pong) for Phase 5c temporal.
        // m_histWriteIdx is the WRITE target (next history); m_histWriteIdx^1 is
        // the READ target (prev history). RGBA16F matches RCAS output precision
        // and is blitted (color-converted) to the swapchain after resolve.
        for (int i = 0; i < 2; ++i) {
            VkImageCreateInfo hici{};
            hici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            hici.imageType = VK_IMAGE_TYPE_2D;
            hici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            hici.extent = {(uint32_t)m_swapWidth, (uint32_t)m_swapHeight, 1};
            hici.mipLevels = 1; hici.arrayLayers = 1;
            hici.samples = VK_SAMPLE_COUNT_1_BIT;
            hici.tiling = VK_IMAGE_TILING_OPTIMAL;
            hici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            hici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo hci{}; hci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &hici, &hci,
                           &m_histColorImage[i], &m_histColorAlloc[i], nullptr);
            VkImageViewCreateInfo hvci{};
            hvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            hvci.image = m_histColorImage[i]; hvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            hvci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            hvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &hvci, nullptr, &m_histColorView[i]);
        }

        // FIX 1: Clear both ping-pong history images to a known state.
        // On a swapchain rebuild or engine restart, m_histColorImage[1] was
        // never written by any compute shader and contains raw VRAM garbage.
        // Sampling that garbage even once produces NaN or extreme negative
        // floats in the temporal feedback loop, which compound into
        // black-pixel pollution across the entire viewport.  Clear both images
        // to a safe background color so the sampler always reads valid,
        // finite values.  Uses a one-time command buffer (ensureEasuFrameResources
        // has no caller-provided cmd buffer).
        {
            VkClearColorValue clearBlack = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = m_cmdPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            VkCommandBuffer initCmd;
            if (vkAllocateCommandBuffers(m_device, &allocInfo, &initCmd) == VK_SUCCESS) {
                VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(initCmd, &beginInfo);

                for (int i = 0; i < 2; ++i) {
                    transitionImageLayout(initCmd, m_histColorImage[i],
                                          VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    vkCmdClearColorImage(initCmd, m_histColorImage[i],
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         &clearBlack, 1, &range);
                    transitionImageLayout(initCmd, m_histColorImage[i],
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }

                vkEndCommandBuffer(initCmd);
                VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &initCmd;
                vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
                vkQueueWaitIdle(m_queue);
                vkFreeCommandBuffers(m_device, m_cmdPool, 1, &initCmd);
            }
        }

        // Dedicated low-resolution scene pipelines (swap/2). Reuse the canonical
        // scene shaders compiled at swap/2 so the baked viewport matches the
        // m_low* render target. m_pbrPipelineLayout is m_sceneLayout.
        const int lW = (int)m_swapWidth / 2, lH = (int)m_swapHeight / 2;
        createInstancedPipeline(m_sceneLayout, lW ? lW : 1, lH ? lH : 1,
                                VK_SAMPLE_COUNT_1_BIT, m_lowScenePipeline,
                                VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                VK_FORMAT_R32G32_SFLOAT);
        createMeshPipeline(m_sceneLayout, lW ? lW : 1, lH ? lH : 1,
                           VK_SAMPLE_COUNT_1_BIT, m_lowMeshPipeline,
                           VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                           VK_FORMAT_R32G32_SFLOAT);
        createPBRMeshPipeline(m_pbrPipelineLayout, lW ? lW : 1, lH ? lH : 1,
                              VK_SAMPLE_COUNT_1_BIT, m_lowPbrMeshPipeline,
                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                              VK_FORMAT_R32G32_SFLOAT);
    }

    void destroyEasuFrameResources() {
        if (m_lowPbrMeshPipeline) { vkDestroyPipeline(m_device, m_lowPbrMeshPipeline, nullptr); m_lowPbrMeshPipeline = VK_NULL_HANDLE; }
        if (m_lowMeshPipeline) { vkDestroyPipeline(m_device, m_lowMeshPipeline, nullptr); m_lowMeshPipeline = VK_NULL_HANDLE; }
        if (m_lowScenePipeline) { vkDestroyPipeline(m_device, m_lowScenePipeline, nullptr); m_lowScenePipeline = VK_NULL_HANDLE; }
        if (m_easuFloatView) { vkDestroyImageView(m_device, m_easuFloatView, nullptr); m_easuFloatView = VK_NULL_HANDLE; }
        if (m_easuFloatImage) { vmaDestroyImage(m_vmaAllocator, m_easuFloatImage, m_easuFloatAlloc); m_easuFloatImage = VK_NULL_HANDLE; m_easuFloatAlloc = VK_NULL_HANDLE; }
        if (m_rcasFloatView) { vkDestroyImageView(m_device, m_rcasFloatView, nullptr); m_rcasFloatView = VK_NULL_HANDLE; }
        if (m_rcasFloatImage) { vkDestroyImage(m_device, m_rcasFloatImage, nullptr); m_rcasFloatImage = VK_NULL_HANDLE; }
        // Note: m_rcasFloatAlloc is NOT separately freed — the RCAS image aliases
        // the EASU allocation (m_easuFloatAlloc), which is destroyed below.
        for (int i = 0; i < 2; ++i) {
            if (m_histColorView[i]) { vkDestroyImageView(m_device, m_histColorView[i], nullptr); m_histColorView[i] = VK_NULL_HANDLE; }
            if (m_histColorImage[i]) { vmaDestroyImage(m_vmaAllocator, m_histColorImage[i], m_histColorAlloc[i]); m_histColorImage[i] = VK_NULL_HANDLE; m_histColorAlloc[i] = VK_NULL_HANDLE; }
        }
        if (m_lowDepthView) { vkDestroyImageView(m_device, m_lowDepthView, nullptr); m_lowDepthView = VK_NULL_HANDLE; }
        if (m_lowDepthImage) { vmaDestroyImage(m_vmaAllocator, m_lowDepthImage, m_lowDepthAlloc); m_lowDepthImage = VK_NULL_HANDLE; m_lowDepthAlloc = VK_NULL_HANDLE; }
        if (m_dilatedVelocityView) { vkDestroyImageView(m_device, m_dilatedVelocityView, nullptr); m_dilatedVelocityView = VK_NULL_HANDLE; }
        if (m_dilatedVelocityImage) { vmaDestroyImage(m_vmaAllocator, m_dilatedVelocityImage, m_dilatedVelocityAlloc); m_dilatedVelocityImage = VK_NULL_HANDLE; m_dilatedVelocityAlloc = VK_NULL_HANDLE; }
        if (m_lowVelocityView) { vkDestroyImageView(m_device, m_lowVelocityView, nullptr); m_lowVelocityView = VK_NULL_HANDLE; }
        if (m_lowVelocityImage) { vmaDestroyImage(m_vmaAllocator, m_lowVelocityImage, m_lowVelocityAlloc); m_lowVelocityImage = VK_NULL_HANDLE; m_lowVelocityAlloc = VK_NULL_HANDLE; }
        if (m_lowColorView) { vkDestroyImageView(m_device, m_lowColorView, nullptr); m_lowColorView = VK_NULL_HANDLE; }
        if (m_lowColorImage) { vmaDestroyImage(m_vmaAllocator, m_lowColorImage, m_lowColorAlloc); m_lowColorImage = VK_NULL_HANDLE; m_lowColorAlloc = VK_NULL_HANDLE; }
        m_lowW = 0; m_lowH = 0;
        m_histWriteIdx = 0; m_histValid = false;
    }

    // Render the scene at HALF the output resolution (low-res G-buffer) and upscale
    // it with AMD FSR3 EASU to (outW x outH). outRGBA receives outW*outH*4 bytes,
    // row 0 = TOP. Headless: no swapchain/surface needed.
    bool renderOffscreenSceneEasu(int outW, int outH, const OffscreenScene& scene,
                                  unsigned char* outRGBA) override {
        if (!m_device || !m_vmaAllocator) return false;
        if (!outRGBA || (scene.instances.empty() && scene.meshes.empty())) return false;
        if (!(outW > 1 && outH > 1)) return false;

        ensureTriangleResources();
        const int rW = outW / 2, rH = outH / 2;           // low-res render G-buffer
        ensureSceneResources(rW ? rW : 1, rH ? rH : 1);
        ensureEasuResources();
        if (!m_easuPipeline) return false;

        uploadSceneUBO(scene, rW, rH);
        uploadInstanceData(scene);
        uploadMeshData(scene);

        // Low-res color target (RGBA8 + SAMPLED, so EASU can read it) + depth.
        VkImage lowImg; VmaAllocation lowMem; VkImageView lowView;
        VkImage lowDepthImg = VK_NULL_HANDLE; VmaAllocation lowDepthMem = VK_NULL_HANDLE; VkImageView lowDepthView = VK_NULL_HANDLE;
        {
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent = {(uint32_t)rW, (uint32_t)rH, 1};
            ici.mipLevels = 1; ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &ici, &aci, &lowImg, &lowMem, nullptr);
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = lowImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &vci, nullptr, &lowView);

            VkImageCreateInfo dici{};
            dici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            dici.imageType = VK_IMAGE_TYPE_2D;
            dici.format = VK_FORMAT_D32_SFLOAT;
            dici.extent = {(uint32_t)rW, (uint32_t)rH, 1};
            dici.mipLevels = 1; dici.arrayLayers = 1;
            dici.samples = VK_SAMPLE_COUNT_1_BIT;
            dici.tiling = VK_IMAGE_TILING_OPTIMAL;
            dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            VmaAllocationCreateInfo daci{}; daci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &dici, &daci, &lowDepthImg, &lowDepthMem, nullptr);
            VkImageViewCreateInfo dvci{};
            dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dvci.image = lowDepthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            dvci.format = VK_FORMAT_D32_SFLOAT;
            dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &dvci, nullptr, &lowDepthView);
        }

        // Full-res EASU output image: the shader writes rgba32f -> R32G32B32A32.
        VkImage outImg; VmaAllocation outMem; VkImageView outView;
        {
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            ici.extent = {(uint32_t)outW, (uint32_t)outH, 1};
            ici.mipLevels = 1; ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &ici, &aci, &outImg, &outMem, nullptr);
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &vci, nullptr, &outView);
        }

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_cmdPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Low-res scene render (color + depth, no velocity — EASU is spatial only).
        transitionImageLayout(cmd, lowImg, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transitionImageLayout(cmd, lowDepthImg, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = lowView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{scene.clearColor[0], scene.clearColor[1],
                                      scene.clearColor[2], scene.clearColor[3]}};
        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = lowDepthView;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {(uint32_t)rW, (uint32_t)rH}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAtt;
        renderingInfo.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(cmd, &renderingInfo);
        VkViewport vp{0, 0, (float)rW, (float)rH, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, {(uint32_t)rW, (uint32_t)rH}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (!scene.instances.empty() && m_scenePipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                    0, 1, &m_bindlessDescSet, 0, nullptr);
            uint32_t whiteTexId = 0;
            vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(uint32_t), &whiteTexId);
            vkCmdDraw(cmd, m_cubeVertexCount, (uint32_t)scene.instances.size(),
                      m_cubeFirstVertex, m_cubeFirstInstance);
        }
        drawMeshes(cmd, scene, scene.pbrEnabled ? m_pbrMeshPipeline : m_meshPipeline);
        vkCmdEndRendering(cmd);

        // low-res color -> SHADER_READ_ONLY for the EASU sampler.
        VkImageMemoryBarrier2 lb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        lb.image = lowImg;
        lb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        lb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        lb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        lb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        lb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        lb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        lb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo ldep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        ldep.imageMemoryBarrierCount = 1; ldep.pImageMemoryBarriers = &lb;
        vkCmdPipelineBarrier2(cmd, &ldep);

        // Upload EASU constants + bind resources + barrier + dispatch.
        // Frame 0 on the headless path has no jitter (jitterOrigin {0,0}), so the
        // EASU sample positions line up with the (non-jittered) low-res render.
        dispatchEasu(cmd, lowView, (uint32_t)rW, (uint32_t)rH,
                     outImg, outView, (uint32_t)outW, (uint32_t)outH,
                     0.0f, 0.0f);

        // output (GENERAL after the compute write) -> TRANSFER_SRC + readback into RGBA8.
        VkImageMemoryBarrier2 ob{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        ob.image = outImg;
        ob.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        ob.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ob.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ob.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        ob.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        ob.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        ob.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo odep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        odep.imageMemoryBarrierCount = 1; odep.pImageMemoryBarriers = &ob;
        vkCmdPipelineBarrier2(cmd, &odep);

        // Readback the R32G32B32A32 (rgba32f in shader) output as floats, pack to RGBA8.
        VkBuffer readBuf; VmaAllocation readMem;
        createBuffer(outW * outH * 4 * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readBuf, readMem);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {(uint32_t)outW, (uint32_t)outH, 1};
        vkCmdCopyImageToBuffer(cmd, outImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readBuf, 1, &bic);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);

        void* data;
        vmaMapMemory(m_vmaAllocator, readMem, &data);
        const float* fp = (const float*)data;
        for (int i = 0; i < outW * outH; ++i) {
            const float r = fp[i * 4 + 0], g = fp[i * 4 + 1], b = fp[i * 4 + 2];
            outRGBA[i * 4 + 0] = (unsigned char)(255 * std::min(1.0f, std::max(0.0f, r)));
            outRGBA[i * 4 + 1] = (unsigned char)(255 * std::min(1.0f, std::max(0.0f, g)));
            outRGBA[i * 4 + 2] = (unsigned char)(255 * std::min(1.0f, std::max(0.0f, b)));
            outRGBA[i * 4 + 3] = 255;
        }
        vmaUnmapMemory(m_vmaAllocator, readMem);
        FlipRows(outRGBA, outW, outH);

        // Free transient EASU + low-res resources (keep persistent pipeline/UBO).
        vmaDestroyBuffer(m_vmaAllocator, readBuf, readMem);
        vkDestroyImageView(m_device, outView, nullptr);
        vmaDestroyImage(m_vmaAllocator, outImg, outMem);
        vkDestroyImageView(m_device, lowView, nullptr);
        vmaDestroyImage(m_vmaAllocator, lowImg, lowMem);
        if (lowDepthView) { vkDestroyImageView(m_device, lowDepthView, nullptr); lowDepthView = VK_NULL_HANDLE; }
        if (lowDepthImg || lowDepthMem) vmaDestroyImage(m_vmaAllocator, lowDepthImg, lowDepthMem);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
        return true;
    }


    bool renderFrameScene(const OffscreenScene& scene) override {
        if (!m_device || !m_swapchain || (scene.instances.empty() && scene.meshes.empty())) return false;
        // ensureSceneResources(m_width,m_height) sets up m_sceneLayout + the
        // bindless SSBO ring (cached for the frame, no command-pool rebuild) — it
        // MUST run before ensureSwapchainScene(), which builds the swapchain
        // scene pipelines against m_sceneLayout.
        ensureSceneResources(m_width, m_height);
        ensureSwapchainScene();
        ensureEasuResources();
        ensureEasuFrameResources();

        VkCommandBuffer cmd = m_cmdBufs[m_frame];
        if (!m_frameRecording) {
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;
            m_frameRecording = true;
        }

        // -----------------------------------------------------------------
        // Phase 5a (VK swapchain EASU): render the scene at HALF resolution
        // into a persistent RGBA8+D32 G-buffer, run AMD FSR3 EASU to upscale
        // that low-res image to full swapchain resolution in a float scratch
        // image, then blit (color-convert) the float scratch into the
        // presentable swapchain image. The present layout handoff
        // (COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR) still happens in
        // endFrame(); ImGui renders on top via LOAD after this returns.
        // Jitter (Halton(2,3)) sub-pixel-shifts the low-res render; the SAME
        // jitter is fed to makeEasuCon() so EASU samples the low-res image at
        // the positions geometry was actually drawn (temporal reprojection is
        // wired in Phase 5b; for now it cancels to a correct spatial upscale).
        // -----------------------------------------------------------------
        if (m_easuPipeline && m_lowColorImage && m_easuFloatImage &&
            m_rcasPipeline && m_rcasFloatImage &&
            m_temporalPipeline && m_histColorImage[0] && m_histColorImage[1] &&
            m_lowColorView && m_easuFloatView && m_lowDepthView && m_rcasFloatView &&
            m_histColorView[0] && m_histColorView[1] &&
            m_lowDepthImage && m_lowVelocityView && m_velocityDilatePipeline &&
            m_lowScenePipeline && m_lowMeshPipeline && m_lowPbrMeshPipeline) {

            const int rW = (int)m_swapWidth / 2, rH = (int)m_swapHeight / 2;

            // firstFrame is hoisted here so the depth-barrier state machine
            // (ldBar) and the temporal history (tNext) both see the same value.
            const bool firstFrame = !m_histValid;

            // Same jitter the low-res render below will use — captured BEFORE
            // uploadSceneUBO advances m_offscreenFrame.
            const Vec2 j = ndcJitter((int)m_offscreenFrame, rW, rH);

            // Capture the previous frame's viewProj BEFORE uploadSceneUBO
            // (which overwrites m_prevViewProj with the current frame's) so the
            // temporal reproject R = prevViewProj * inverse(viewProj) lines up the
            // PREVIOUS history onto the CURRENT frame.
            const Mat4 prevVP = m_prevViewProj;
            uploadSceneUBO(scene, rW, rH);   // embeds jitter, advances m_prevViewProj to frame N
            const Mat4 currVP = m_prevViewProj;   // current frame's viewProj (just set above)
            uploadInstanceData(scene);   // cube instances at offset 0
            uploadMeshData(scene);       // mesh data appended after

            // --- low-res G-buffer render (color + depth) ---
            // Color target is discarded each frame (cleared by vkCmdBeginRendering).
            VkImageMemoryBarrier2 lcBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            lcBar.image = m_lowColorImage;
            lcBar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            lcBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            lcBar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            lcBar.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            lcBar.srcAccessMask = 0;
            lcBar.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            lcBar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            // Depth: PRESERVE layout state across frames.  On the first frame the
            // depth image is freshly allocated (UNDEFINED).  On every subsequent
            // frame it was left in SHADER_READ_ONLY_OPTIMAL by the prior frame's
            // temporal-resolve barrier (tdBar).  Transitioning from the ACTUAL
            // previous layout (instead of blindly UNDEFINED) keeps the AMD GPU's
            // HTILE/depth-compression metadata valid — UNDEFINED discards the
            // compression state every frame, so the later texture() on the depth
            // in the temporal resolve returns fallback 0.0 values, corrupting
            // the reprojection and producing black pixel pollution.
            VkImageMemoryBarrier2 ldBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            ldBar.image = m_lowDepthImage;
            ldBar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            ldBar.oldLayout = firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ldBar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            ldBar.srcStageMask = firstFrame ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ldBar.srcAccessMask = firstFrame ? 0 : VK_ACCESS_2_SHADER_READ_BIT;
            ldBar.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            ldBar.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkImageMemoryBarrier2 lvBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            lvBar.image = m_lowVelocityImage;
            lvBar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            lvBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            lvBar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            lvBar.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            lvBar.srcAccessMask = 0;
            lvBar.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            lvBar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

            VkImageMemoryBarrier2 barsIn[3] = { lcBar, lvBar, ldBar };
            VkDependencyInfo depIn{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depIn.imageMemoryBarrierCount = 3; depIn.pImageMemoryBarriers = barsIn;
            vkCmdPipelineBarrier2(cmd, &depIn);

            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView = m_lowColorView;
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.clearValue.color = {{scene.clearColor[0], scene.clearColor[1],
                                          scene.clearColor[2], scene.clearColor[3]}};
            VkRenderingAttachmentInfo depthAtt{};
            depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAtt.imageView = m_lowDepthView;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAtt.clearValue.depthStencil = {1.0f, 0};
            VkRenderingAttachmentInfo velAtt{};
            velAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            velAtt.imageView = m_lowVelocityView;
            velAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            velAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            velAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            velAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

            VkRenderingAttachmentInfo colorAtts[2] = { colorAtt, velAtt };
            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = {{0, 0}, {(uint32_t)rW, (uint32_t)rH}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 2;
            renderingInfo.pColorAttachments = colorAtts;
            renderingInfo.pDepthAttachment = &depthAtt;
            vkCmdBeginRendering(cmd, &renderingInfo);
            VkViewport vp{0, 0, (float)rW, (float)rH, 0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scissor{{0, 0}, {(uint32_t)rW, (uint32_t)rH}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            if (!scene.instances.empty() && m_lowScenePipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lowScenePipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                        0, 1, &m_bindlessDescSet, 0, nullptr);
                uint32_t whiteTexId = 0;
                vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(uint32_t), &whiteTexId);
                vkCmdDraw(cmd, m_cubeVertexCount, (uint32_t)scene.instances.size(),
                          m_cubeFirstVertex, m_cubeFirstInstance);
            }
            drawMeshes(cmd, scene,
                       scene.pbrEnabled ? m_lowPbrMeshPipeline : m_lowMeshPipeline);
            vkCmdEndRendering(cmd);

            // --- low-res color -> SHADER_READ_ONLY for the EASU sampler ---
            VkImageMemoryBarrier2 lb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            lb.image = m_lowColorImage;
            lb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            lb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            lb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            lb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            lb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            lb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            lb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkDependencyInfo ldep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            ldep.imageMemoryBarrierCount = 1; ldep.pImageMemoryBarriers = &lb;
            vkCmdPipelineBarrier2(cmd, &ldep);

            // --- Velocity dilation (pre-EASU, pre-temporal) ---
            // Dilates foreground velocity vectors over silhouette edges so FSR3
            // reprojects using the dominant surface motion at boundaries.
            // Velocity: COLOR_ATTACHMENT_OPTIMAL -> GENERAL (storage read).
            // Depth:    DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY (sample in dilation).
            // Dilated:  UNDEFINED -> GENERAL (storage write).
            VkImageMemoryBarrier2 vb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            vb.image = m_lowVelocityImage;
            vb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            vb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            vb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            vb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            vb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkImageMemoryBarrier2 vd{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            vd.image = m_lowDepthImage;
            vd.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vd.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            vd.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vd.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            vd.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            vd.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            vd.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkImageMemoryBarrier2 vdo{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            vdo.image = m_dilatedVelocityImage;
            vdo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vdo.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            vdo.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            vdo.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            vdo.srcAccessMask = 0;
            vdo.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            vdo.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            VkImageMemoryBarrier2 vdBars[3] = { vb, vd, vdo };
            VkDependencyInfo vdDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            vdDep.imageMemoryBarrierCount = 3; vdDep.pImageMemoryBarriers = vdBars;
            vkCmdPipelineBarrier2(cmd, &vdDep);

            dispatchVelocityDilation(cmd, m_lowVelocityView, m_lowDepthView,
                                     m_dilatedVelocityView,
                                     (uint32_t)rW, (uint32_t)rH);

            // Dilated velocity: GENERAL -> SHADER_READ_ONLY (for temporal resolve).
            VkImageMemoryBarrier2 vdr{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            vdr.image = m_dilatedVelocityImage;
            vdr.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vdr.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            vdr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vdr.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            vdr.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            vdr.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            vdr.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkDependencyInfo vdDep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            vdDep2.imageMemoryBarrierCount = 1; vdDep2.pImageMemoryBarriers = &vdr;
            vkCmdPipelineBarrier2(cmd, &vdDep2);

            // --- EASU: low-res(sampler) -> full-res R32G32B32A32 scratch (storage) ---
            dispatchEasu(cmd, m_lowColorView, (uint32_t)rW, (uint32_t)rH,
                         m_easuFloatImage, m_easuFloatView,
                         (uint32_t)m_swapWidth, (uint32_t)m_swapHeight,
                         j.x, j.y);
            // After EASU, m_easuFloatImage is in GENERAL (storage write finished).
            // RCAS samples it via texelFetch (needs SHADER_READ_ONLY) and writes m_rcasFloatImage.

            // --- barrier: EASU output GENERAL -> SHADER_READ_ONLY (RCAS texelFetch src) ---
            //            + RCAS output UNDEFINED -> GENERAL (storage write) ---
            VkImageMemoryBarrier2 e2rBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            e2rBar.image = m_easuFloatImage;
            e2rBar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            e2rBar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            e2rBar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            e2rBar.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            e2rBar.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            e2rBar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            e2rBar.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkImageMemoryBarrier2 r2gBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            r2gBar.image = m_rcasFloatImage;
            r2gBar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            r2gBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // discard (RCAS overwrites all pixels)
            r2gBar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            r2gBar.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            r2gBar.srcAccessMask = 0;
            r2gBar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            r2gBar.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            VkImageMemoryBarrier2 e2rBars[2] = { e2rBar, r2gBar };
            VkDependencyInfo e2rDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            e2rDep.imageMemoryBarrierCount = 2; e2rDep.pImageMemoryBarriers = e2rBars;
            vkCmdPipelineBarrier2(cmd, &e2rDep);

            // --- RCAS: sharpen the EASU output (rcasConfig in const3.x) -> m_rcasFloatImage ---
            dispatchRcas(cmd, m_easuFloatView, m_easuFloatImage,
                         m_rcasFloatView, m_rcasFloatImage,
                         (uint32_t)m_swapWidth, (uint32_t)m_swapHeight, m_rcasSharpness);

            // --- Phase 5c: temporal resolve between RCAS and the swapchain blit ---
            // RCAS left m_rcasFloatImage in GENERAL (currColor source for temporal).
            // Reproject: history = feedback*curr + (1-feedback)*prevHistory(prevViewProj·inv(viewProj)).
            const int histWriteIdx = (int)m_histWriteIdx;
            const int histReadIdx  = histWriteIdx ^ 1;

            // currColor (rcasFloat) GENERAL -> SHADER_READ_ONLY (temporal samples it).
            VkImageMemoryBarrier2 tCurr{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            tCurr.image = m_rcasFloatImage;
            tCurr.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            tCurr.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            tCurr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tCurr.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tCurr.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            tCurr.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tCurr.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;

            // prev history (read source): UNDEFINED first frame (unread, discard ok),
            // else SHADER_READ_ONLY (written + transitioned ready by the prior frame).
            VkImageMemoryBarrier2 tPrev{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            tPrev.image = m_histColorImage[histReadIdx];
            tPrev.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            tPrev.oldLayout = firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tPrev.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tPrev.srcStageMask = firstFrame ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tPrev.srcAccessMask = firstFrame ? 0 : VK_ACCESS_2_SHADER_WRITE_BIT;
            tPrev.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tPrev.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;

            // next history (write target): UNDEFINED -> GENERAL (discard + storage).
            VkImageMemoryBarrier2 tNext{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            tNext.image = m_histColorImage[histWriteIdx];
            tNext.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            tNext.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // discard (overwritten by resolve)
            tNext.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            tNext.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            tNext.srcAccessMask = 0;
            tNext.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tNext.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;

            // --- low-res depth: DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY ---
            // The temporal shader samples m_lowDepthView (SRV@2). Without this
            // barrier the depth image stays in DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            // which is an INVALID layout for texture() — the GPU returns 0.0 for
            // every texel.  Zero depth → reprojection maps to invalid history UV
            // (often outside [0,1]) → black border texels are sampled → black
            // pixels overlay the scene.  (First frame the depth was never
            // transitioned out of DEPTH_STENCIL_ATTACHMENT_OPTIMAL either, so
            // the same corruption affects frame 0.)
            // --- low-res depth: already SHADER_READ_ONLY (from velocity dilation
            // barriers above) -> keep it there for the temporal shader's depth
            // sampling. This barrier just ensures the depth-write (render pass)
            // and the prior compute-shader read (velocity dilation) are
            // available before the temporal resolve reads depth.
            VkImageMemoryBarrier2 tdBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            tdBar.image = m_lowDepthImage;
            tdBar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            tdBar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tdBar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tdBar.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            tdBar.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            tdBar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tdBar.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;

            VkImageMemoryBarrier2 tBars[4] = { tCurr, tPrev, tNext, tdBar };
            VkDependencyInfo tDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            tDep.imageMemoryBarrierCount = 4; tDep.pImageMemoryBarriers = tBars;
            vkCmdPipelineBarrier2(cmd, &tDep);

            const TemporalCon tcon = makeTemporalCon(currVP, prevVP, firstFrame, m_temporalFeedback, j);
            dispatchTemporal(cmd, m_rcasFloatView,
                             m_histColorView[histReadIdx], m_lowDepthView,
                             m_histColorView[histWriteIdx],
                             (uint32_t)m_swapWidth, (uint32_t)m_swapHeight, tcon);

            // next history GENERAL -> TRANSFER_SRC (blit source to swapchain).
            VkImageMemoryBarrier2 tXfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            tXfer.image = m_histColorImage[histWriteIdx];
            tXfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            tXfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            tXfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            tXfer.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tXfer.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            tXfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            tXfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo xdep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            xdep.imageMemoryBarrierCount = 1; xdep.pImageMemoryBarriers = &tXfer;
            vkCmdPipelineBarrier2(cmd, &xdep);

            // --- swapchain image: discard -> TRANSFER_DST (blit destination) ---
            VkImageMemoryBarrier2 sb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            sb.image = m_swapImages[m_imageIndex];
            sb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            sb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // discard last frame's present
            sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            sb.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            sb.srcAccessMask = 0;
            sb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            sb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo sdep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            sdep.imageMemoryBarrierCount = 1; sdep.pImageMemoryBarriers = &sb;
            vkCmdPipelineBarrier2(cmd, &sdep);

            // --- blit temporal history (RGBA16F) -> swapchain (R8G8B8A8, linear) ---
            VkImageBlit blit{};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {(int)m_swapWidth, (int)m_swapHeight, 1};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {(int)m_swapWidth, (int)m_swapHeight, 1};
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            vkCmdBlitImage(cmd, m_histColorImage[histWriteIdx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_swapImages[m_imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            // next history -> SHADER_READ_ONLY (so it's the prev-history source next frame).
            VkImageMemoryBarrier2 tRd{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            tRd.image = m_histColorImage[histWriteIdx];
            tRd.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            tRd.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            tRd.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tRd.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            tRd.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            tRd.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tRd.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkDependencyInfo rdep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            rdep.imageMemoryBarrierCount = 1; rdep.pImageMemoryBarriers = &tRd;
            vkCmdPipelineBarrier2(cmd, &rdep);

            m_histWriteIdx = (uint8_t)((m_histWriteIdx + 1) & 1);
            m_histValid = true;

            // --- swapchain: TRANSFER_DST -> COLOR_ATTACHMENT_OPTIMAL (endFrame present + ImGui LOAD) ---
            VkImageMemoryBarrier2 cb2{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            cb2.image = m_swapImages[m_imageIndex];
            cb2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            cb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            cb2.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            cb2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            cb2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            cb2.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            cb2.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo cdep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            cdep.imageMemoryBarrierCount = 1; cdep.pImageMemoryBarriers = &cb2;
            vkCmdPipelineBarrier2(cmd, &cdep);

            m_frameRendered = true;
            m_frameHasScene = true;
            return true;
        }

        // -----------------------------------------------------------------
        // Fallback: EASU resources unavailable — render the scene full
        // resolution directly into the swapchain (pre-Phase 5 behavior), so the
        // editor viewport stays live on constrained builds.
        // -----------------------------------------------------------------
        ensureSceneResources(m_width, m_height);
        uploadSceneUBO(scene, m_width, m_height);
        uploadInstanceData(scene);   // cube instances at offset 0
        uploadMeshData(scene);       // mesh data appended after

        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (m_swapchainSamples == VK_SAMPLE_COUNT_1_BIT) {
            colorAtt.imageView = m_swapImageViews[m_imageIndex];
        } else {
            colorAtt.imageView = m_swapchainMsaaViews[m_imageIndex];
            colorAtt.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorAtt.resolveImageView = m_swapImageViews[m_imageIndex];
            colorAtt.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{scene.clearColor[0], scene.clearColor[1],
                                      scene.clearColor[2], scene.clearColor[3]}};

        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = m_swapchainDepthViews[m_imageIndex];
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {(uint32_t)m_swapWidth, (uint32_t)m_swapHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAtt;
        renderingInfo.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport vp{0, 0, (float)m_swapWidth, (float)m_swapHeight, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, {(uint32_t)m_swapWidth, (uint32_t)m_swapHeight}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Instanced cubes — fetched from global SSBOs, no vertex buffer binding
        if (!scene.instances.empty() && m_scenePipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                    0, 1, &m_bindlessDescSet, 0, nullptr);
            uint32_t whiteTexId = 0;
            vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(uint32_t), &whiteTexId);
            vkCmdDraw(cmd, m_cubeVertexCount, (uint32_t)scene.instances.size(),
                      m_cubeFirstVertex, m_cubeFirstInstance);
        }

        // Generic meshes
        drawMeshes(cmd, scene, m_swapchainMeshPipeline ? m_swapchainMeshPipeline : m_swapchainPipeline);

        vkCmdEndRendering(cmd);

        m_frameRendered = true;
        m_frameHasScene = true;
        return true;
    }

    // =====================================================================
    // IMGUI (Vulkan integration)
    // =====================================================================
    bool initializeImGui() override {
        if (!m_device || !m_swapchain) return false;
        ensureSceneResources(m_width, m_height);
        ensureSwapchainScene();

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = m_instance;
        info.PhysicalDevice = m_physical;
        info.Device = m_device;
        info.QueueFamily = m_queueFamily;
        info.Queue = m_queue;
        info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
        info.MinImageCount = (uint32_t)m_swapImages.size();
        info.ImageCount = (uint32_t)m_swapImages.size();
        // ImGui — use dynamic rendering (Vulkan 1.3) instead of render pass.
        // This eliminates the need for ImGui's internal render pass / framebuffers
        // and lets us inline a single-color-attachment rendering scope.
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        static VkFormat imguiColorFmt = VK_FORMAT_UNDEFINED;
        imguiColorFmt = m_swapFormat;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &imguiColorFmt;
        info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        info.CheckVkResultFn = [](VkResult r) {
            if (r != VK_SUCCESS) std::cerr << "[RHI-Vk] ImGui Vulkan error: " << r << std::endl;
        };
        if (!ImGui_ImplVulkan_Init(&info)) return false;
        m_imguiInitialized = true;
        return true;
    }

    bool renderFrameImGui(ImDrawData* drawData) override {
        if (!m_imguiInitialized || !drawData) return false;
        if (!m_frameRecording) {
            vkResetCommandBuffer(m_cmdBufs[m_frame], 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (vkBeginCommandBuffer(m_cmdBufs[m_frame], &begin) != VK_SUCCESS) return false;
            m_frameRecording = true;
        }

        // Begin dynamic rendering scope on the currently acquired swapchain
        // image. The scene was rendered by renderFrameScene; ImGui renders
        // on top with LOAD op (preserves scene content). The result ends in
        // PRESENT_SRC_KHR for presentation.
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = m_swapImageViews[m_imageIndex];
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // preserve scene
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{0, 0, 0, 0}};
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {(uint32_t)m_swapWidth, (uint32_t)m_swapHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAtt;
        vkCmdBeginRendering(m_cmdBufs[m_frame], &renderingInfo);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplVulkan_RenderDrawData(drawData, m_cmdBufs[m_frame]);
        vkCmdEndRendering(m_cmdBufs[m_frame]);
        return true;
    }

private:
    // =====================================================================
    // SWAPCHAIN
    // =====================================================================
    static constexpr int kMaxFrames = 3;
    // Upper bound for the bindless texture descriptor array (binding 3).
    // PARTIALLY_BOUND ensures unused slots don't cause errors.
    static constexpr uint32_t kMaxBindlessTextures = 1024;

    bool createSwapchain() {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps);

        uint32_t fmtCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &fmtCount, fmts.data());
        m_swapFormat = fmts[0].format;
        m_colorSpace = fmts[0].colorSpace;

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &presentModeCount, modes.data());
        m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { m_presentMode = m; break; }

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR scCI{};
        scCI.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        scCI.surface = m_surface;
        scCI.minImageCount = imageCount;
        scCI.imageFormat = m_swapFormat;
        scCI.imageColorSpace = m_colorSpace;
        scCI.imageExtent = caps.currentExtent;
        scCI.imageArrayLayers = 1;
        // Base usage (COLOR_ATTACHMENT) + TRANSFER_DST/TRANSFER_SRC so the
        // Phase 5a FSR3 path can blit the EASU float scratch image into the
        // presentable swapchain image (and read it back for headless gating).
        scCI.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        scCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        scCI.preTransform = caps.currentTransform;
        scCI.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        scCI.presentMode = m_presentMode;
        scCI.clipped = VK_TRUE;

        if (vkCreateSwapchainKHR(m_device, &scCI, nullptr, &m_swapchain) != VK_SUCCESS)
            return false;

        uint32_t imgCount;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, nullptr);
        m_swapImages.resize(imgCount);
        m_swapImageViews.resize(imgCount);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, m_swapImages.data());

        for (uint32_t i = 0; i < imgCount; ++i) {
            VkImageViewCreateInfo ivCI{};
            ivCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivCI.image = m_swapImages[i];
            ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivCI.format = m_swapFormat;
            ivCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &ivCI, nullptr, &m_swapImageViews[i]);
        }

        m_swapWidth = caps.currentExtent.width;
        m_swapHeight = caps.currentExtent.height;
        std::cout << "[RHI-Vk] PresentMode="
                  << (m_presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO")
                  << " size=" << m_swapWidth << "x" << m_swapHeight << std::endl;
        return true;
    }

    void destroySwapchain() {
        for (auto v : m_swapImageViews) if (v) vkDestroyImageView(m_device, v, nullptr);
        m_swapImageViews.clear();
        m_swapImages.clear();
        if (m_swapchain) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
    }

    // =====================================================================
    // OFFSCREEN TARGET (for headless renders)
    // =====================================================================
    bool createOffscreenTarget(int w, int h, VkImage& img, VmaAllocation& mem,
                               VkImageView& view, bool withDepth = false,
                               VkImage* depthImg = nullptr, VmaAllocation* depthMem = nullptr,
                               VkImageView* depthView = nullptr) {
        // Color image
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)w, (uint32_t)h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &ici, &aci, &img, &mem, nullptr);

        // View
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &view);

        // Optional depth image (for render-pass compatibility with m_sceneRenderPass)
        if (withDepth) {
            VkImageCreateInfo dici{};
            dici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            dici.imageType = VK_IMAGE_TYPE_2D;
            dici.format = VK_FORMAT_D32_SFLOAT;
            dici.extent = {(uint32_t)w, (uint32_t)h, 1};
            dici.mipLevels = 1; dici.arrayLayers = 1;
            dici.samples = VK_SAMPLE_COUNT_1_BIT;
            dici.tiling = VK_IMAGE_TILING_OPTIMAL;
            dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

            VmaAllocationCreateInfo daci{};
            daci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &dici, &daci, depthImg, depthMem, nullptr);

            VkImageViewCreateInfo dvci{};
            dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dvci.image = *depthImg;
            dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            dvci.format = VK_FORMAT_D32_SFLOAT;
            dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &dvci, nullptr, depthView);
        }

        // Render pass + framebuffer removed — dynamic rendering (vkCmdBeginRenderPass2/vkCmdEndRendering)
        return true;
    }

    // =====================================================================
    // SCENE RESOURCES (UBO, pipelines, descriptor sets, vertex/index buffers)
    // =====================================================================
        void ensureSceneResources(int w, int h) {
        if (m_scenePipeline && m_sceneW == w && m_sceneH == h) return;
        destroyScene();
        m_sceneW = w; m_sceneH = h;

        // Command pool
        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = m_queueFamily;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmdPool);

        // Command buffers (one per frame in flight)
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = m_cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = kMaxFrames;
        vkAllocateCommandBuffers(m_device, &cbai, m_cmdBufs);

        // Scene UBO (persistently mapped — eliminates per-frame map/unmap)
        createBuffer(sizeof(CameraUBOData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_sceneUbo, m_sceneUboMem);
        vmaMapMemory(m_vmaAllocator, m_sceneUboMem, &m_sceneUboMapped);

        // ── Global SSBOs ──
        const VkDeviceSize kInitVB  = 16 * 1024 * 1024;
        const VkDeviceSize kInitIB  = 8  * 1024 * 1024;
        const VkDeviceSize kInitInst= 4  * 1024 * 1024;

        // Triple-buffer the skinned vertex SSBO (binding 1). Each frame the host
        // writes fresh verts into slice[m_frame]; beginFrame()'s 3-frame-in-flight
        // fences guarantee the GPU has drained slice[m_frame] before we reuse it,
        // so the host never overwrites vertices the GPU is still drawing — this
        // kills the Vulkan read/write race that stretched/vibrated the feet.
        m_globalVBSliceSize = kInitVB;
        createBuffer(kInitVB * kMaxFrames, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_globalVB, m_globalVBMem);
        vmaMapMemory(m_vmaAllocator, m_globalVBMem, &m_globalVBMapped);
        m_globalVBCapacity = kInitVB * kMaxFrames;

        createBuffer(kInitIB, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_globalIB, m_globalIBMem);
        vmaMapMemory(m_vmaAllocator, m_globalIBMem, &m_globalIBMapped);
        m_globalIBCapacity = kInitIB;

        // Triple-buffer the instance SSBO (binding 2) — instance/model matrices
        // (cube + skinned character) change every frame, so a single buffer would
        // tear them exactly like the vertex SSBO above.
        m_globalInstSliceSize = kInitInst;
        createBuffer(kInitInst * kMaxFrames, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_globalInstBuf, m_globalInstMem);
        vmaMapMemory(m_vmaAllocator, m_globalInstMem, &m_globalInstMapped);
        m_globalInstCapacity = kInitInst * kMaxFrames;

        // ── Bindless descriptor set ──
        VkDescriptorSetLayoutBinding bl[4]{};
        bl[0].binding = 0; bl[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bl[0].descriptorCount = 1;
        bl[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bl[1].binding = 1; bl[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bl[1].descriptorCount = 1; bl[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bl[2].binding = 2; bl[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bl[2].descriptorCount = 1; bl[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bl[3].binding = 3; bl[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bl[3].descriptorCount = kMaxBindlessTextures;
        bl[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // Update-after-bind on ALL bindings so we can re-write buffer handles
        // (VB/IB/UBO) after a resize without rebinding the set, and add/update
        // textures at any time.  UPDATE_AFTER_BIND lets us modify descriptors
        // even after the set has been bound to a command buffer.
        VkDescriptorBindingFlags bflags[4] = {};
        bflags[0] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        bflags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        bflags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        bflags[3] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfcInfo{};
        bfcInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bfcInfo.bindingCount = 4;
        bfcInfo.pBindingFlags = bflags;

        VkDescriptorSetLayoutCreateInfo dlci{};
        dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dlci.pNext = &bfcInfo;
        dlci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        dlci.bindingCount = 4;
        dlci.pBindings = bl;
        vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_bindlessDescLayout);

        VkDescriptorPoolSize dps[3]{};
        dps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;        dps[0].descriptorCount = 1;
        dps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;       dps[1].descriptorCount = 2;
        dps[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; dps[2].descriptorCount = kMaxBindlessTextures;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 3; dpci.pPoolSizes = dps;
        vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_bindlessDescPool);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = m_bindlessDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_bindlessDescLayout;
        vkAllocateDescriptorSets(m_device, &dsai, &m_bindlessDescSet);

        // White texture (textureId 0 in the bindless array)
        ensureWhiteTexture();
        m_bindlessImageInfos.clear();
        VkDescriptorImageInfo whiteDii{};
        whiteDii.sampler = m_whiteTexSampler;
        whiteDii.imageView = m_whiteTexView;
        whiteDii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_bindlessImageInfos.push_back(whiteDii);

        // Write UBO + SSBOs to the bindless set (uses helper so buffers can be
        // re-bound after resize without duplicating the write logic).
        updateBindlessBufferDescriptors();

        // Write white texture at binding 3 (textureId 0)
        VkWriteDescriptorSet texWds{};
        texWds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        texWds.dstSet = m_bindlessDescSet;
        texWds.dstBinding = 3;
        texWds.dstArrayElement = 0;
        texWds.descriptorCount = 1;
        texWds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texWds.pImageInfo = &whiteDii;
        vkUpdateDescriptorSets(m_device, 1, &texWds, 0, nullptr);

        // ── Cube vertices → global vertex SSBO ──
        m_cubeVertexCount = 36;
        m_cubeFirstVertex = 0;
        m_cubeFirstInstance = 0;
        m_haveCubeVerts = true;
        for (int i = 0; i < 36; ++i) {
            int s = i * 3;
            int d = i * 8;
            m_cubeVerts[d + 0] = kCubeVerts[s + 0];
            m_cubeVerts[d + 1] = kCubeVerts[s + 1];
            m_cubeVerts[d + 2] = kCubeVerts[s + 2];
            m_cubeVerts[d + 3] = 0.0f;
            m_cubeVerts[d + 4] = 1.0f;
            m_cubeVerts[d + 5] = 0.0f;
            m_cubeVerts[d + 6] = 0.0f;
            m_cubeVerts[d + 7] = 0.0f;
        }
        // Publish the static cube verts into EVERY ring slice so rotating the
        // active slice (m_frame) never leaves a slice without cube data.
        if (m_globalVBMapped)
            for (int f = 0; f < kMaxFrames; ++f)
                std::memcpy((char*)m_globalVBMapped + (VkDeviceSize)f * m_globalVBSliceSize,
                            m_cubeVerts, sizeof(m_cubeVerts));
        m_globalVBSize = sizeof(m_cubeVerts);

        // ── Pipeline layout (bindless set + push constants) ──
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0; pcr.size = 16;
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1; pl.pSetLayouts = &m_bindlessDescLayout;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &pl, nullptr, &m_sceneLayout);
        m_pbrPipelineLayout = m_sceneLayout;

        m_meshUploadInfos.clear();
        m_meshTex.clear();
        m_bindlessImageInfos.resize(1);  // keep white texture at slot 0

        // --- Virtualized Geometry: Cluster Culling Pipeline (Phase 2) ---
        // Lazily created only when a mesh with clusters is uploaded. We pre-create
        // the descriptor set layout and pipeline layout here so they're ready.
        {
            VkDescriptorSetLayoutBinding clB[5]{};
            clB[0].binding = 0; clB[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            clB[0].descriptorCount = 1; clB[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            clB[1].binding = 1; clB[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            clB[1].descriptorCount = 1; clB[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            clB[2].binding = 2; clB[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            clB[2].descriptorCount = 1; clB[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            clB[3].binding = 3; clB[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            clB[3].descriptorCount = 1; clB[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            clB[4].binding = 4; clB[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            clB[4].descriptorCount = 1; clB[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo clDlcInfo{};
            clDlcInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            clDlcInfo.bindingCount = 5; clDlcInfo.pBindings = clB;
            vkCreateDescriptorSetLayout(m_device, &clDlcInfo, nullptr, &m_clusterDescLayout);

            VkPushConstantRange pcCR{};
            pcCR.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcCR.offset = 0; pcCR.size = 16;  // totalNodes + viewportW/H + lodPixelError
            VkPipelineLayoutCreateInfo clPlInfo{};
            clPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            clPlInfo.setLayoutCount = 1; clPlInfo.pSetLayouts = &m_clusterDescLayout;
            clPlInfo.pushConstantRangeCount = 1; clPlInfo.pPushConstantRanges = &pcCR;
            vkCreatePipelineLayout(m_device, &clPlInfo, nullptr, &m_clusterCullLayout);

            auto spv = loadSpirv("build/rhi/cluster_cull.comp.spv");
            if (!spv.empty()) {
                VkShaderModule sm = createShaderModule(m_device, spv);
                if (sm) {
                    VkComputePipelineCreateInfo cpci{};
                    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    cpci.stage.module = sm;
                    cpci.stage.pName = "main";
                    cpci.layout = m_clusterCullLayout;
                    vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_clusterCullPipeline);
            vkDestroyShaderModule(m_device, sm, nullptr);
            m_clusterCullInitialized = true;
            std::cout << "[RHI-Vk] Cluster cull pipeline created\n";

            // --- Phase 3: Nanite LOD selection pipeline ---
            auto lodSpv = loadSpirv("build/rhi/nanite_lod.comp.spv");
            if (!lodSpv.empty()) {
                VkShaderModule lodSm = createShaderModule(m_device, lodSpv);
                if (lodSm) {
                    VkComputePipelineCreateInfo lodCpci{};
                    lodCpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                    lodCpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    lodCpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    lodCpci.stage.module = lodSm;
                    lodCpci.stage.pName = "main";
                    lodCpci.layout = m_clusterCullLayout;  // shared layout (same bindings)
                    vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &lodCpci, nullptr, &m_naniteLodPipeline);
                    vkDestroyShaderModule(m_device, lodSm, nullptr);
                    std::cout << "[RHI-Vk] Nanite LOD pipeline created\n";
                }
            }
                }
            }
        }
        // Buffers for cluster data are created lazily in uploadMeshData().

        // Pipelines (dynamic rendering — no render pass, no vertex input)
        createInstancedPipeline(m_sceneLayout, m_sceneW, m_sceneH,
                                VK_SAMPLE_COUNT_1_BIT, m_scenePipeline,
                                VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
        createMeshPipeline(m_sceneLayout, m_sceneW, m_sceneH,
                           VK_SAMPLE_COUNT_1_BIT, m_meshPipeline,
                           VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
        createPBRMeshPipeline(m_sceneLayout, m_sceneW, m_sceneH,
                              VK_SAMPLE_COUNT_1_BIT, m_pbrMeshPipeline,
                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    }
    void destroyScene() {
        // Swapchain scene resources (pipelines + MSAA/depth images) reference
        // m_sceneLayout, so they must be destroyed before the layout below.
        destroySwapchainScene();

        for (auto& r : m_meshTex) destroyMeshTex(r);
        m_meshTex.clear();
        m_bindlessImageInfos.clear();
        if (m_triPipeline) { vkDestroyPipeline(m_device, m_triPipeline, nullptr); m_triPipeline = VK_NULL_HANDLE; }
        if (m_triLayout) { vkDestroyPipelineLayout(m_device, m_triLayout, nullptr); m_triLayout = VK_NULL_HANDLE; }
        if (m_pbrMeshPipeline && m_pbrMeshPipeline != m_meshPipeline) { vkDestroyPipeline(m_device, m_pbrMeshPipeline, nullptr); m_pbrMeshPipeline = VK_NULL_HANDLE; }
        if (m_scenePipeline) { vkDestroyPipeline(m_device, m_scenePipeline, nullptr); m_scenePipeline = VK_NULL_HANDLE; }
        if (m_meshPipeline) { vkDestroyPipeline(m_device, m_meshPipeline, nullptr); m_meshPipeline = VK_NULL_HANDLE; }
        if (m_sceneLayout) { vkDestroyPipelineLayout(m_device, m_sceneLayout, nullptr); m_sceneLayout = VK_NULL_HANDLE; }
        if (m_bindlessDescPool) { vkDestroyDescriptorPool(m_device, m_bindlessDescPool, nullptr); m_bindlessDescPool = VK_NULL_HANDLE; }
        if (m_bindlessDescLayout) { vkDestroyDescriptorSetLayout(m_device, m_bindlessDescLayout, nullptr); m_bindlessDescLayout = VK_NULL_HANDLE; }
        if (m_sceneUbo || m_sceneUboMem) {
            if (m_sceneUboMapped) vmaUnmapMemory(m_vmaAllocator, m_sceneUboMem);
            m_sceneUboMapped = nullptr;
            vmaDestroyBuffer(m_vmaAllocator, m_sceneUbo, m_sceneUboMem);
            m_sceneUbo = VK_NULL_HANDLE;
            m_sceneUboMem = VK_NULL_HANDLE;
        }
        if (m_globalVB || m_globalVBMem) {
            if (m_globalVBMapped) { vmaUnmapMemory(m_vmaAllocator, m_globalVBMem); m_globalVBMapped = nullptr; }
            vmaDestroyBuffer(m_vmaAllocator, m_globalVB, m_globalVBMem);
            m_globalVB = VK_NULL_HANDLE; m_globalVBMem = VK_NULL_HANDLE;
        }
        if (m_globalIB || m_globalIBMem) {
            if (m_globalIBMapped) { vmaUnmapMemory(m_vmaAllocator, m_globalIBMem); m_globalIBMapped = nullptr; }
            vmaDestroyBuffer(m_vmaAllocator, m_globalIB, m_globalIBMem);
            m_globalIB = VK_NULL_HANDLE; m_globalIBMem = VK_NULL_HANDLE;
        }
        if (m_globalInstBuf || m_globalInstMem) {
            if (m_globalInstMapped) { vmaUnmapMemory(m_vmaAllocator, m_globalInstMem); m_globalInstMapped = nullptr; }
            vmaDestroyBuffer(m_vmaAllocator, m_globalInstBuf, m_globalInstMem);
            m_globalInstBuf = VK_NULL_HANDLE; m_globalInstMem = VK_NULL_HANDLE;
        }
        if (m_cmdPool) { vkDestroyCommandPool(m_device, m_cmdPool, nullptr); m_cmdPool = VK_NULL_HANDLE; }
        // White texture resources (image + memory + view + sampler)
        if (m_whiteTexView) { vkDestroyImageView(m_device, m_whiteTexView, nullptr); m_whiteTexView = VK_NULL_HANDLE; }
        if (m_whiteTex || m_whiteTexMem) {
            vmaDestroyImage(m_vmaAllocator, m_whiteTex, m_whiteTexMem);
            m_whiteTex = VK_NULL_HANDLE; m_whiteTexMem = VK_NULL_HANDLE;
        }
        if (m_whiteTexSampler) { vkDestroySampler(m_device, m_whiteTexSampler, nullptr); m_whiteTexSampler = VK_NULL_HANDLE; }
        m_sceneW = m_sceneH = 0;
        m_globalVBCapacity = 0; m_globalIBCapacity = 0; m_globalInstCapacity = 0;
        m_globalVBSize = 0;
        m_meshUploadInfos.clear();

        // Cluster culling resources (Phase 2)
        if (m_clusterBoundsBuf || m_clusterBoundsMem) {
            vmaDestroyBuffer(m_vmaAllocator, m_clusterBoundsBuf, m_clusterBoundsMem);
            m_clusterBoundsBuf = VK_NULL_HANDLE; m_clusterBoundsMem = VK_NULL_HANDLE;
        }
        if (m_clusterCmdBuf || m_clusterCmdMem) {
            vmaDestroyBuffer(m_vmaAllocator, m_clusterCmdBuf, m_clusterCmdMem);
            m_clusterCmdBuf = VK_NULL_HANDLE; m_clusterCmdMem = VK_NULL_HANDLE;
        }
        if (m_indirectDrawBuf || m_indirectDrawMem) {
            vmaDestroyBuffer(m_vmaAllocator, m_indirectDrawBuf, m_indirectDrawMem);
            m_indirectDrawBuf = VK_NULL_HANDLE; m_indirectDrawMem = VK_NULL_HANDLE;
        }
        if (m_clusterCounterBuf || m_clusterCounterMem) {
            if (m_clusterCounterMapped) {
                vmaUnmapMemory(m_vmaAllocator, m_clusterCounterMem);
                m_clusterCounterMapped = nullptr;
            }
            vmaDestroyBuffer(m_vmaAllocator, m_clusterCounterBuf, m_clusterCounterMem);
            m_clusterCounterBuf = VK_NULL_HANDLE; m_clusterCounterMem = VK_NULL_HANDLE;
        }
        if (m_clusterCullPipeline) {
            vkDestroyPipeline(m_device, m_clusterCullPipeline, nullptr);
            m_clusterCullPipeline = VK_NULL_HANDLE;
        }
        if (m_naniteLodPipeline) {
            vkDestroyPipeline(m_device, m_naniteLodPipeline, nullptr);
            m_naniteLodPipeline = VK_NULL_HANDLE;
        }
        if (m_clusterCullLayout) {
            vkDestroyPipelineLayout(m_device, m_clusterCullLayout, nullptr);
            m_clusterCullLayout = VK_NULL_HANDLE;
        }
        if (m_clusterDescSet) {
            vkFreeDescriptorSets(m_device, m_clusterDescPool, 1, &m_clusterDescSet);
            m_clusterDescSet = VK_NULL_HANDLE;
        }
        if (m_clusterDescPool) {
            vkDestroyDescriptorPool(m_device, m_clusterDescPool, nullptr);
            m_clusterDescPool = VK_NULL_HANDLE;
        }
        if (m_clusterDescLayout) {
            vkDestroyDescriptorSetLayout(m_device, m_clusterDescLayout, nullptr);
            m_clusterDescLayout = VK_NULL_HANDLE;
        }
        m_clusterCullInitialized = false;
    }

    // =====================================================================
    // WHITE TEXTURE
    // =====================================================================
    void ensureWhiteTexture() {
        if (m_whiteTexView) return;

        // Sampler
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(m_device, &sci, nullptr, &m_whiteTexSampler);

        // Image
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {1, 1, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo waci{};
        waci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &ici, &waci, &m_whiteTex, &m_whiteTexMem, nullptr);

        // View
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = m_whiteTex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &m_whiteTexView);

        // Upload pixel
        VkBuffer staging; VmaAllocation stagingMem;
        createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging, stagingMem);
        void* data;
        vmaMapMemory(m_vmaAllocator, stagingMem, &data);
        const uint8_t white[4] = {255, 255, 255, 255};
        std::memcpy(data, white, 4);
        vmaUnmapMemory(m_vmaAllocator, stagingMem);

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = m_cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &ai, &cmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        transitionImageLayout(cmd, m_whiteTex, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(m_device, cmd, staging, m_whiteTex, 1, 1);
        transitionImageLayout(cmd, m_whiteTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
        vmaDestroyBuffer(m_vmaAllocator, staging, stagingMem);
    }

    // =====================================================================
    // MESH TEXTURES (PBR: albedo + normal + roughness + AO)
    // =====================================================================
    struct MeshTexResources {
        VkImage image = VK_NULL_HANDLE; VmaAllocation mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
        uint32_t textureId = 0;  // index into m_bindlessImageInfos
        int w = 0, h = 0;
    };
    std::vector<MeshTexResources> m_meshTex;

    void destroyMeshTex(MeshTexResources& r) {
        if (r.view) { vkDestroyImageView(m_device, r.view, nullptr); r.view = VK_NULL_HANDLE; }
        if (r.image || r.mem) vmaDestroyImage(m_vmaAllocator, r.image, r.mem); r.image = VK_NULL_HANDLE; r.mem = VK_NULL_HANDLE;
        r.textureId = 0;
        r.w = r.h = 0;
    }

    // Register a texture in the bindless array (returns textureId).
    // Writes the descriptor into binding 3 of the global bindless set at the
    // returned array index.  With UPDATE_AFTER_BIND we can update descriptors
    // at any time, even after the set has been bound to a command buffer.
    uint32_t registerTexture(VkImageView view, VkSampler sampler) {
        VkDescriptorImageInfo dii{};
        dii.sampler = sampler;
        dii.imageView = view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        uint32_t id = (uint32_t)m_bindlessImageInfos.size();
        m_bindlessImageInfos.push_back(dii);
        // Bind this texture into the bindless descriptor array at index `id`
        if (m_bindlessDescSet) {
            VkWriteDescriptorSet texWds{};
            texWds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            texWds.dstSet = m_bindlessDescSet;
            texWds.dstBinding = 3;               // bindless texture array binding
            texWds.dstArrayElement = id;          // write at the specific array slot
            texWds.descriptorCount = 1;
            texWds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            texWds.pImageInfo = &dii;
            vkUpdateDescriptorSets(m_device, 1, &texWds, 0, nullptr);
        }
        return id;
    }

    // Re-write the buffer descriptors (UBO + vertex SSBO + instance SSBO) in
    // the bindless set.  Called after buffer creation or resize so the
    // descriptor set always points to the LIVE buffer handle, and after
    // destroyScene/ensureSceneResources rebuild the set.
    void updateBindlessBufferDescriptors() {
        if (!m_bindlessDescSet || !m_sceneUbo || !m_globalVB || !m_globalInstBuf) return;
        VkDescriptorBufferInfo uboInfo{m_sceneUbo, 0, sizeof(CameraUBOData)};
        // Ring slices: the host just wrote this frame's verts/instances into
        // slice[m_frame]; rebind THAT slice so the shader reads fresh data
        // instead of a slice still in flight on the GPU.
        VkDeviceSize frameVBBase   = (VkDeviceSize)m_frame * m_globalVBSliceSize;
        VkDeviceSize frameInstBase = (VkDeviceSize)m_frame * m_globalInstSliceSize;
        VkDescriptorBufferInfo vtxInfo{m_globalVB,
                                       m_globalVBSliceSize ? frameVBBase : 0,
                                       m_globalVBSliceSize ? m_globalVBSliceSize : VK_WHOLE_SIZE};
        VkDescriptorBufferInfo instInfo{m_globalInstBuf,
                                        m_globalInstSliceSize ? frameInstBase : 0,
                                        m_globalInstSliceSize ? m_globalInstSliceSize : VK_WHOLE_SIZE};
        VkWriteDescriptorSet wds[3]{};
        wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[0].dstSet = m_bindlessDescSet; wds[0].dstBinding = 0;
        wds[0].descriptorCount = 1; wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wds[0].pBufferInfo = &uboInfo;
        wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[1].dstSet = m_bindlessDescSet; wds[1].dstBinding = 1;
        wds[1].descriptorCount = 1; wds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wds[1].pBufferInfo = &vtxInfo;
        wds[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[2].dstSet = m_bindlessDescSet; wds[2].dstBinding = 2;
        wds[2].descriptorCount = 1; wds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wds[2].pBufferInfo = &instInfo;
        vkUpdateDescriptorSets(m_device, 3, wds, 0, nullptr);
    }

    bool createMeshTexture(const OffscreenMesh& mesh, MeshTexResources& res) {
        // White texture must exist (bindless texture 0 = white)
        ensureWhiteTexture();

        uint32_t w = mesh.textureWidth, h = mesh.textureHeight;
        if (w == 0 || h == 0) return false;

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo maci{};
        maci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_vmaAllocator, &ici, &maci, &res.image, &res.mem, nullptr);

        // View
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = res.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &res.view);

        // Upload via staging
        VkBuffer staging; VmaAllocation stagingMem;
        VkDeviceSize sz = (VkDeviceSize)w * h * 4;
        createBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging, stagingMem);
        void* data;
        vmaMapMemory(m_vmaAllocator, stagingMem, &data);
        std::memcpy(data, mesh.texturePixels.data(), sz);
        vmaUnmapMemory(m_vmaAllocator, stagingMem);

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = m_cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &ai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        transitionImageLayout(cmd, res.image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(m_device, cmd, staging, res.image, w, h);
        transitionImageLayout(cmd, res.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
        vmaDestroyBuffer(m_vmaAllocator, staging, stagingMem);

        // Register in bindless texture array — no per-mesh descriptor set
        res.textureId = registerTexture(res.view, m_whiteTexSampler);
        res.w = w; res.h = h;
        return true;
    }

    // =====================================================================
    // SCENE UBO UPLOAD
    // =====================================================================
    void uploadSceneUBO(const OffscreenScene& scene, int w, int h) {
        CameraUBOData d{};
        const Mat4 proj = mat4Perspective(scene.camera.fovDeg, (float)w / (float)h,
                                          0.1f, scene.camera.farPlane);
        const Mat4 projVk = mat4GlToVulkanProj(proj);
        const Mat4 view = mat4LookAt(scene.camera.eye, scene.camera.target, scene.camera.up);
        d.viewProj = mat4Multiply(projVk, view);
        // FSR3 history (Phase 1: staged, inert — shaders don't read these yet).
        d.prevViewProj = m_prevViewProj;                                   // reprojection
        const Vec2 j = ndcJitter((int)m_offscreenFrame, w, h);
        d.jitterOffset[0] = j.x; d.jitterOffset[1] = j.y;
        d.camPos[0] = scene.camera.eye[0]; d.camPos[1] = scene.camera.eye[1]; d.camPos[2] = scene.camera.eye[2];
        d.fogDensity = scene.fogDensity;
        d.fogColor[0] = scene.fogColor[0]; d.fogColor[1] = scene.fogColor[1]; d.fogColor[2] = scene.fogColor[2];
        // Advance frame history for the next frame's reprojection/jitter.
        m_prevViewProj = d.viewProj;
        ++m_offscreenFrame;  // advance the Halton jitter sequence (offscreen path)
        // Persistently mapped — direct memcpy, no map/unmap overhead per frame
        if (m_sceneUboMapped)
            std::memcpy(m_sceneUboMapped, &d, sizeof(d));
    }

    // =====================================================================
    // MESH DATA UPLOAD (global SSBOs, version-aware)
    // =====================================================================
    void uploadMeshData(const OffscreenScene& scene) {
        // Per-frame ring slice bases. The host writes current-frame data into
        // slice[m_frame]; beginFrame()'s fences ensure it's safe to overwrite.
        const VkDeviceSize frameVBBase   = (VkDeviceSize)m_frame * m_globalVBSliceSize;
        const VkDeviceSize frameInstBase = (VkDeviceSize)m_frame * m_globalInstSliceSize;
        // Rebind this frame's vertex (binding 1) + instance (binding 2) slices.
        updateBindlessBufferDescriptors();
        if (scene.meshes.empty()) return;

        // Ensure texture resources exist for each mesh (bindless registration)
        while (m_meshTex.size() < scene.meshes.size()) {
            m_meshTex.emplace_back();
        }
        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            const auto& mesh = scene.meshes[i];
            if (mesh.textureWidth > 0 && mesh.textureHeight > 0 && !mesh.texturePixels.empty()) {
                if (m_meshTex[i].w != mesh.textureWidth || m_meshTex[i].h != mesh.textureHeight
                    || m_meshTex[i].image == VK_NULL_HANDLE) {
                    destroyMeshTex(m_meshTex[i]);
                    createMeshTexture(mesh, m_meshTex[i]);
                }
            }
        }

        // Compute total sizes and resize global buffers if needed.
        // totalV includes the cube verts (cube lives at the slice base, meshes append).
        VkDeviceSize totalV = sizeof(m_cubeVerts), totalI = 0, totalInst = 0;
        for (const auto& mesh : scene.meshes) {
            if (mesh.vertices.empty() || mesh.indices.empty()) continue;
            totalV    += mesh.vertices.size() * sizeof(float);
            totalI    += mesh.indices.size() * sizeof(uint32_t);
            totalInst += mesh.instances.size() * sizeof(OffscreenInstance);
        }

        if (!resizeGlobalVB(totalV) || !resizeGlobalIB(totalI) || !resizeGlobalInst(totalInst)) {
            std::cerr << "[RHI-Vk] Failed to resize global SSBOs\n";
            return;
        }

        // Triple-buffered ring: slices rotate with m_frame, so the current slice
        // must be (re)populated every frame — otherwise it would hold a different
        // frame's data. (Skinned verts/indices are already full-reuploaded every
        // frame via mesh.version; forcing fullReupload keeps static meshes correct
        // across all slices too.)
        bool fullReupload = true;
        m_meshUploadInfos.resize(scene.meshes.size());
        // Cube verts sit at the base of every slice (pre-published); reset the
        // LOCAL (in-slice) cursors so meshes append after the cube within this slice.
        m_globalVBSize  = sizeof(m_cubeVerts);
        m_globalIBSize  = 0;   // cube is non-indexed


        // Upload vertex + index + instance data into global SSBOs
        // Mesh data is appended AFTER cube data (which starts at offset 0)
        VkDeviceSize vOff = m_globalVBSize, iOff = m_globalIBSize;
        // Instance data: cubes at [0, cubeCount), meshes after
        VkDeviceSize instOff = m_globalInstSize;

        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            const auto& mesh = scene.meshes[i];
            if (mesh.vertices.empty() || mesh.indices.empty()) continue;

            VkDeviceSize vBytes = mesh.vertices.size() * sizeof(float);
            VkDeviceSize iBytes = mesh.indices.size() * sizeof(uint32_t);
            VkDeviceSize instBytes = mesh.instances.size() * sizeof(OffscreenInstance);

            // Bounds check (absolute offset into the 3x ring buffer)
            if (frameVBBase + vOff + vBytes > m_globalVBCapacity) {
                std::cerr << "[RHI-Vk] uploadMeshData: global VB overflow\n";
                break;
            }
            if (iOff + iBytes > m_globalIBCapacity) {
                std::cerr << "[RHI-Vk] uploadMeshData: global IB overflow\n";
                break;
            }
            if (frameInstBase + instOff + instBytes > m_globalInstCapacity) {
                std::cerr << "[RHI-Vk] uploadMeshData: global instance buffer overflow\n";
                break;
            }

            if (fullReupload) {
                if (m_globalVBMapped)
                    std::memcpy((char*)m_globalVBMapped + frameVBBase + vOff,
                                mesh.vertices.data(), vBytes);
                if (m_globalIBMapped)
                    std::memcpy((char*)m_globalIBMapped + iOff, mesh.indices.data(), iBytes);
            }

            if (fullReupload || m_meshUploadInfos[i].instanceVersion != mesh.instanceVersion) {
                if (instBytes > 0 && m_globalInstMapped)
                    std::memcpy((char*)m_globalInstMapped + frameInstBase + instOff,
                                mesh.instances.data(), instBytes);
            }

            // firstVertex/firstIndex/firstInstance are LOCAL to this frame's slice;
            // the ring offset is applied via the descriptor (VB/Inst @ slice base)
            // and vkCmdBindIndexBuffer offset. firstVertex is in VERTEX units (not
            // float units) — it becomes the vkCmdDrawIndexed vertexOffset.
            m_meshUploadInfos[i].firstVertex = (uint32_t)(vOff / sizeof(VulkanVertex));
            m_meshUploadInfos[i].firstIndex = (uint32_t)(iOff / sizeof(uint32_t));
            m_meshUploadInfos[i].vertexCount = (uint32_t)(mesh.vertices.size() / 8);  // 8 floats/vertex
            m_meshUploadInfos[i].indexCount = (uint32_t)mesh.indices.size();
            m_meshUploadInfos[i].firstInstance = (uint32_t)(instOff / sizeof(OffscreenInstance));
            m_meshUploadInfos[i].instanceCount = (uint32_t)mesh.instances.size();
            m_meshUploadInfos[i].textureId = m_meshTex[i].textureId;
            m_meshUploadInfos[i].vertexVersion = mesh.version;
            m_meshUploadInfos[i].instanceVersion = mesh.instanceVersion;

            vOff += vBytes;
            iOff += iBytes;
            instOff += instBytes;
        }

        // Update global sizes after upload
        m_globalVBSize = vOff;
        m_globalIBSize = iOff;
        m_globalInstSize = instOff;

        // --- Virtualized Geometry: upload cluster data (Phase 2) ---
        // Aggregate all clusters across meshes into contiguous GPU buffers.
        size_t totalClusters = 0;
        for (const auto& mesh : scene.meshes) {
            totalClusters += mesh.clusters.size();
        }

        if (totalClusters == 0) return;  // no clustered meshes — legacy path

        // Lazily create/recreate cluster buffers
        if (!m_clusterBoundsBuf || !m_clusterCmdBuf || !m_indirectDrawBuf || !m_clusterCounterBuf) {
            VkDeviceSize boundsSize = totalClusters * sizeof(VkClusterBounds);
            VkDeviceSize cmdSize    = totalClusters * sizeof(VkClusterCommand);
            VkDeviceSize indirSize  = totalClusters * sizeof(VkDrawIndexedIndirectCommand);
            VkDeviceSize counterSize = sizeof(uint32_t);

            createBuffer(boundsSize > 0 ? boundsSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_clusterBoundsBuf, m_clusterBoundsMem);
            createBuffer(cmdSize > 0 ? cmdSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_clusterCmdBuf, m_clusterCmdMem);
            createBuffer(indirSize > 0 ? indirSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_indirectDrawBuf, m_indirectDrawMem);
            createBuffer(counterSize,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_clusterCounterBuf, m_clusterCounterMem);
            vmaMapMemory(m_vmaAllocator, m_clusterCounterMem, &m_clusterCounterMapped);
        }

        // Upload all cluster bounds and commands
        std::vector<VkClusterBounds>  allBounds;
        std::vector<VkClusterCommand> allCmds;
        allBounds.reserve(totalClusters);
        allCmds.reserve(totalClusters);

        uint32_t clusterIdx = 0;
        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            const auto& mesh = scene.meshes[i];
            m_meshUploadInfos[i].firstCluster = clusterIdx;
            m_meshUploadInfos[i].clusterCount = (uint32_t)mesh.clusters.size();

            for (const auto& cr : mesh.clusters) {
                VkClusterBounds b;
                b.sphereCenterRadius = glm::vec4(cr.center[0], cr.center[1], cr.center[2], cr.radius);
                b.minBounds = glm::vec4(0, 0, 0, 1);  // conservative; could refine from vertices
                b.maxBounds = glm::vec4(0, 0, 0, 1);

                VkClusterCommand cmd;
                // Store ABSOLUTE offsets into global buffers (vertexOffset and
                // firstIndex are relative to the zero-bound index/vertex buffers).
                const auto& mi = m_meshUploadInfos[i];
                cmd.firstVertex = mi.firstVertex + cr.firstVertex;
                cmd.firstIndex  = mi.firstIndex  + cr.firstIndex;
                cmd.indexCount  = cr.indexCount;
                cmd.instanceId  = mi.firstInstance;  // all clusters share mesh's instance data

                allBounds.push_back(b);
                allCmds.push_back(cmd);
                ++clusterIdx;
            }
        }

        // Store total cluster count for the render phase
        m_totalClusterCount = (uint32_t)totalClusters;

        if (!allBounds.empty()) {
            void* boundsPtr = nullptr;
            vmaMapMemory(m_vmaAllocator, m_clusterBoundsMem, &boundsPtr);
            std::memcpy(boundsPtr, allBounds.data(), allBounds.size() * sizeof(VkClusterBounds));
            vmaUnmapMemory(m_vmaAllocator, m_clusterBoundsMem);
        }
        if (!allCmds.empty()) {
            void* cmdPtr = nullptr;
            vmaMapMemory(m_vmaAllocator, m_clusterCmdMem, &cmdPtr);
            std::memcpy(cmdPtr, allCmds.data(), allCmds.size() * sizeof(VkClusterCommand));
            vmaUnmapMemory(m_vmaAllocator, m_clusterCmdMem);
        }

        // Allocate / write the cluster descriptor set (set 0 for the compute pipeline)
        if (m_clusterDescPool == VK_NULL_HANDLE) {
            VkDescriptorPoolSize poolSizes[4]{};
            poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;    poolSizes[0].descriptorCount = 1;
            poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   poolSizes[1].descriptorCount = 1;
            poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   poolSizes[2].descriptorCount = 1;
            poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;    poolSizes[3].descriptorCount = 2;
            VkDescriptorPoolCreateInfo dpci{};
            dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpci.maxSets = 1; dpci.poolSizeCount = 4; dpci.pPoolSizes = poolSizes;
            vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_clusterDescPool);
        }
        if (m_clusterDescSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = m_clusterDescPool;
            dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_clusterDescLayout;
            vkAllocateDescriptorSets(m_device, &dsai, &m_clusterDescSet);
        }

        // Write descriptors: scene UBO (b0), bounds SSBO (b1), cmds SSBO (b2),
        // indirect buffer (b3), counter SSBO (b4)
        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_sceneUbo; uboInfo.offset = 0; uboInfo.range = sizeof(CameraUBOData);
        VkDescriptorBufferInfo boundsInfo{};
        boundsInfo.buffer = m_clusterBoundsBuf; boundsInfo.offset = 0;
        boundsInfo.range = totalClusters * sizeof(VkClusterBounds);
        VkDescriptorBufferInfo cmdInfo{};
        cmdInfo.buffer = m_clusterCmdBuf; cmdInfo.offset = 0;
        cmdInfo.range = totalClusters * sizeof(VkClusterCommand);
        VkDescriptorBufferInfo indirInfo{};
        indirInfo.buffer = m_indirectDrawBuf; indirInfo.offset = 0;
        indirInfo.range = totalClusters * sizeof(VkDrawIndexedIndirectCommand);
        VkDescriptorBufferInfo counterInfo{};
        counterInfo.buffer = m_clusterCounterBuf; counterInfo.offset = 0;
        counterInfo.range = sizeof(uint32_t);

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_clusterDescSet; writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &uboInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_clusterDescSet; writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &boundsInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_clusterDescSet; writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &cmdInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_clusterDescSet; writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &indirInfo;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = m_clusterDescSet; writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &counterInfo;
        vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
    }

    // Upload cube instance data to the global instance SSBO (cubes come first)
    void uploadInstanceData(const OffscreenScene& scene) {
        const VkDeviceSize frameInstBase = (VkDeviceSize)m_frame * m_globalInstSliceSize;
        if (scene.instances.empty()) {
            m_globalInstSize = 0;   // no cube instances this frame
            m_cubeFirstInstance = 0;
            m_cubeInstanceCount = 0;
            return;
        }
        VkDeviceSize needed = scene.instances.size() * sizeof(OffscreenInstance);
        if (!resizeGlobalInst(needed)) {
            std::cerr << "[RHI-Vk] Failed to resize global instance SSBO\n";
            return;
        }
        if (m_globalInstMapped) {
            // Cube instances land at the base of THIS frame's instance-slice.
            std::memcpy((char*)m_globalInstMapped + frameInstBase, scene.instances.data(), (size_t)needed);
            m_globalInstSize = needed;
        }
        m_cubeFirstInstance = 0;
        m_cubeInstanceCount = (uint32_t)scene.instances.size();
    }

    // =====================================================================
    // DRAW MESHES (bindless textures + global SSBOs + push constants)
    // =====================================================================
    void drawMeshes(VkCommandBuffer cmd, const OffscreenScene& scene,
                    VkPipeline pipeline) {
        if (scene.meshes.empty()) return;
        if (!m_globalVB || !m_globalIB || !m_globalInstBuf) return;

        bool anyClustered = false;
        for (const auto& mesh : scene.meshes) {
            if (!mesh.clusters.empty()) { anyClustered = true; break; }
        }

        // If any mesh has clusters, use GPU-driven culling + indirect draws
        if (anyClustered && m_clusterCullInitialized && m_clusterCullPipeline
            && m_totalClusterCount > 0) {
            drawMeshesClustered(cmd, scene);
            // Also draw non-clustered meshes in the same pass via legacy path
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                    0, 1, &m_bindlessDescSet, 0, nullptr);
            (void)m_cubeFirstInstance;
        } else {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                    0, 1, &m_bindlessDescSet, 0, nullptr);
            (void)m_cubeFirstInstance;  // cubes always start at instance 0
        }

        // Legacy per-mesh draw for meshes WITHOUT cluster data
        for (size_t i = 0; i < scene.meshes.size() && i < m_meshUploadInfos.size(); ++i) {
            const auto& mesh = scene.meshes[i];
            const auto& info = m_meshUploadInfos[i];
            if (mesh.vertices.empty() || mesh.indices.empty() || info.instanceCount == 0) continue;
            if (!mesh.clusters.empty()) continue;  // already drawn via cluster path

            uint32_t texId = info.textureId;
            vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(uint32_t), &texId);

            vkCmdBindIndexBuffer(cmd, m_globalIB,
                                 info.firstIndex * sizeof(uint32_t),
                                 VK_INDEX_TYPE_UINT32);

            vkCmdDrawIndexed(cmd, info.indexCount, info.instanceCount,
                             0, info.firstVertex, info.firstInstance);
        }
    }

    // =====================================================================
    // DRAW CLUSTERS — GPU-driven frustum culling + indirect draws (Phase 2)
    // =====================================================================
    void drawMeshesClustered(VkCommandBuffer cmd, const OffscreenScene& scene) {
        if (!m_clusterCullInitialized || !m_clusterCullPipeline || m_totalClusterCount == 0)
            return;

        // 1. Reset draw-count counter to 0
        if (m_clusterCounterMapped) {
            *(uint32_t*)m_clusterCounterMapped = 0;
        }

        // 2. Bind compute pipeline + cluster descriptor set
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_clusterCullPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 m_clusterCullLayout, 0, 1, &m_clusterDescSet, 0, nullptr);

        // 3. Push constants: totalClusters, viewportW, viewportH, lodPixelError
        struct PushData { uint32_t total; float vw, vh, err; } pd{};
        pd.total = m_totalClusterCount;
        pd.vw = (float)m_sceneW; pd.vh = (float)m_sceneH;
        pd.err = 1.0f;  // 1-pixel target error
        vkCmdPushConstants(cmd, m_clusterCullLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PushData), &pd);

        // 4. Dispatch compute shader (64 threads/workgroup)
        uint32_t groupCount = (m_totalClusterCount + 63) / 64;
        vkCmdDispatch(cmd, groupCount, 1, 1);

        // 5. Barrier: compute writes → indirect draw reads
        VkMemoryBarrier memBarrier{};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 1, &memBarrier, 0, nullptr, 0, nullptr);

        // 6. Bind graphics pipeline + bindless descriptors (same as legacy path)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                 0, 1, &m_bindlessDescSet, 0, nullptr);

        // 7. Bind index buffer at offset 0 (cluster commands carry absolute indices)
        vkCmdBindIndexBuffer(cmd, m_globalIB, 0, VK_INDEX_TYPE_UINT32);

        // 8. Issue indirect draws — count is read from GPU counter buffer
        vkCmdDrawIndexedIndirectCount(cmd,
            m_indirectDrawBuf, 0,
            m_clusterCounterBuf, 0,
            sizeof(VkDrawIndexedIndirectCommand),
            m_totalClusterCount);  // maxDrawCount
    }

    // =====================================================================
    // PIPELINE CREATION
    // =====================================================================
    bool createInstancedPipeline(VkPipelineLayout layout, int w, int h,
                                  VkSampleCountFlagBits samples,
                                  VkPipeline& out, VkFormat colorFmt,
                                  VkFormat depthFmt,
                                  VkFormat velocityFmt = VK_FORMAT_R32G32_SFLOAT) {
        auto vsSpv = loadSpirv("build/rhi/scene.vert.spv");
        auto fsSpv = loadSpirv("build/rhi/scene.frag.spv");
        if (vsSpv.empty() || fsSpv.empty()) return false;
        VkShaderModule vs = createShaderModule(m_device, vsSpv);
        VkShaderModule fs = createShaderModule(m_device, fsSpv);
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vs2{};
        vs2.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vs2.viewportCount = 1; vs2.pViewports = &vp;
        vs2.scissorCount = 1; vs2.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = samples;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;

        const bool kSceneHasVelocity = (velocityFmt != VK_FORMAT_UNDEFINED);
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].colorWriteMask = 0xF; cba[0].blendEnable = VK_FALSE;
        cba[1].colorWriteMask = 0xF; cba[1].blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = kSceneHasVelocity ? 2 : 1;
        cb.pAttachments = cba;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = nullptr;  // SSBO vertex fetching — no vertex attributes
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vs2;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.layout = layout;
        VkPipelineRenderingCreateInfo prci{};
        prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        VkFormat sceneColorFormats[2] = { colorFmt, velocityFmt };
        prci.colorAttachmentCount = kSceneHasVelocity ? 2 : 1;
        prci.pColorAttachmentFormats = kSceneHasVelocity ? sceneColorFormats : &colorFmt;
        prci.depthAttachmentFormat = depthFmt;
        pci.pNext = &prci;
        pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out);

        vkDestroyShaderModule(m_device, vs, nullptr);
        vkDestroyShaderModule(m_device, fs, nullptr);
        return out != VK_NULL_HANDLE;
    }

    bool createMeshPipeline(VkPipelineLayout layout, int w, int h,
                            VkSampleCountFlagBits samples,
                            VkPipeline& out, VkFormat colorFmt,
                            VkFormat depthFmt,
                            VkFormat velocityFmt = VK_FORMAT_R32G32_SFLOAT) {
        auto vsSpv = loadSpirv("build/rhi/mesh.vert.spv");
        auto fsSpv = loadSpirv("build/rhi/mesh.frag.spv");
        if (vsSpv.empty() || fsSpv.empty()) return false;
        VkShaderModule vs = createShaderModule(m_device, vsSpv);
        VkShaderModule fs = createShaderModule(m_device, fsSpv);
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vs2{};
        vs2.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vs2.viewportCount = 1; vs2.pViewports = &vp;
        vs2.scissorCount = 1; vs2.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = samples;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;

        const bool kSceneHasVelocity = (velocityFmt != VK_FORMAT_UNDEFINED);
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].colorWriteMask = 0xF; cba[0].blendEnable = VK_FALSE;
        cba[1].colorWriteMask = 0xF; cba[1].blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = kSceneHasVelocity ? 2 : 1;
        cb.pAttachments = cba;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = nullptr;  // SSBO vertex fetching — no vertex attributes
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vs2;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.layout = layout;
        VkPipelineRenderingCreateInfo prci2{};
        prci2.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        VkFormat sceneColorFormats[2] = { colorFmt, velocityFmt };
        prci2.colorAttachmentCount = kSceneHasVelocity ? 2 : 1;
        prci2.pColorAttachmentFormats = kSceneHasVelocity ? sceneColorFormats : &colorFmt;
        prci2.depthAttachmentFormat = depthFmt;
        pci.pNext = &prci2;
        pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out);

        vkDestroyShaderModule(m_device, vs, nullptr);
        vkDestroyShaderModule(m_device, fs, nullptr);
        return out != VK_NULL_HANDLE;
    }

    // PBR mesh pipeline (pbr_mesh.vert / pbr_mesh.frag)
    bool createPBRMeshPipeline(VkPipelineLayout layout, int w, int h,
                                VkSampleCountFlagBits samples,
                                VkPipeline& out, VkFormat colorFmt,
                                VkFormat depthFmt,
                            VkFormat velocityFmt = VK_FORMAT_R32G32_SFLOAT) {
        auto vsSpv = loadSpirv("build/rhi/pbr_mesh.vert.spv");
        auto fsSpv = loadSpirv("build/rhi/pbr_mesh.frag.spv");
        if (vsSpv.empty() || fsSpv.empty()) return false;
        VkShaderModule vs = createShaderModule(m_device, vsSpv);
        VkShaderModule fs = createShaderModule(m_device, fsSpv);
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vs2{};
        vs2.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vs2.viewportCount = 1; vs2.pViewports = &vp;
        vs2.scissorCount = 1; vs2.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = samples;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;

        const bool kSceneHasVelocity = (velocityFmt != VK_FORMAT_UNDEFINED);
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].colorWriteMask = 0xF; cba[0].blendEnable = VK_FALSE;
        cba[1].colorWriteMask = 0xF; cba[1].blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = kSceneHasVelocity ? 2 : 1;
        cb.pAttachments = cba;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = nullptr;  // SSBO vertex fetching
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vs2;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.layout = layout;
        VkPipelineRenderingCreateInfo prci2{};
        prci2.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        VkFormat sceneColorFormats[2] = { colorFmt, velocityFmt };
        prci2.colorAttachmentCount = kSceneHasVelocity ? 2 : 1;
        prci2.pColorAttachmentFormats = kSceneHasVelocity ? sceneColorFormats : &colorFmt;
        prci2.depthAttachmentFormat = depthFmt;
        pci.pNext = &prci2;
        pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out);

        vkDestroyShaderModule(m_device, vs, nullptr);
        vkDestroyShaderModule(m_device, fs, nullptr);
        return out != VK_NULL_HANDLE;
    }


    // =====================================================================
    // TRIANGLE RESOURCES
    // =====================================================================
    void ensureTriangleResources() {
        if (m_triPipeline) return;
        // ensureSceneResources creates m_cmdPool, but renderOffscreenTriangle
        // may be called without it (device-only, no scene). Create it here if
        // it doesn't exist yet so command buffers can be allocated.
        if (!m_cmdPool) {
            VkCommandPoolCreateInfo cpci{};
            cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cpci.queueFamilyIndex = m_queueFamily;
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmdPool);

            VkCommandBufferAllocateInfo cbai{};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = m_cmdPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = kMaxFrames;
            vkAllocateCommandBuffers(m_device, &cbai, m_cmdBufs);
        }
        auto vsSpv = loadSpirv("build/rhi/triangle.vert.spv");
        auto fsSpv = loadSpirv("build/rhi/triangle.frag.spv");
        if (vsSpv.empty() || fsSpv.empty()) return;

        VkShaderModule vs = createShaderModule(m_device, vsSpv);
        VkShaderModule fs = createShaderModule(m_device, fsSpv);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport vp{0, 0, 64, 64, 0, 1};
        VkRect2D sc{{0,0},{64,64}};
        VkPipelineViewportStateCreateInfo vsci{};
        vsci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vsci.viewportCount = 1; vsci.pViewports = &vp;
        vsci.scissorCount = 1; vsci.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        // Dynamic rendering — no render pass needed
        VkPipelineRenderingCreateInfo prciTri{};
        prciTri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        prciTri.colorAttachmentCount = 1;
        VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
        prciTri.pColorAttachmentFormats = &colorFmt;

        // Pipeline layout: push constants only (triangle.vert uses push constants
        // for vertex positions — no descriptor sets, no vertex buffers).
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset = 0; pcr.size = 16;  // {vec2 pos0, pos1, pos2} = 12 bytes, padded to 16
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &pl, nullptr, &m_triLayout);

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.pNext = &prciTri;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = nullptr;  // push constants for vertex data
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vsci;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pColorBlendState = &cb;
        pci.layout = m_triLayout;
        pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_triPipeline);

        vkDestroyShaderModule(m_device, vs, nullptr);
        vkDestroyShaderModule(m_device, fs, nullptr);
    }


    void ensureSwapchainScene() {
        if (m_swapchainPipeline || m_swapchainMeshPipeline) return;

        // Determine MSAA sample count. On software rasterizers (lavapipe/llvmpipe),
        // MSAA is pure CPU overhead — 4x the pixel work with no visual benefit.
        VkPhysicalDeviceProperties pdp;
        vkGetPhysicalDeviceProperties(m_physical, &pdp);
        std::string gpuName = std::string(pdp.deviceName);
        bool isSoftware = (gpuName.find("llvmpipe") != std::string::npos ||
                           gpuName.find("lavapipe") != std::string::npos ||
                           gpuName.find("softpin") != std::string::npos);
        VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
        if (!isSoftware) {
            VkSampleCountFlags counts = pdp.limits.framebufferColorSampleCounts &
                                        pdp.limits.framebufferDepthSampleCounts;
            for (auto s : {VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT}) {
                if (counts & s) { msaaSamples = s; break; }
            }
        }
        if (isSoftware)
            std::cout << "[RHI-Vk] Software rasterizer detected — MSAA disabled for performance\n";
        m_swapchainSamples = msaaSamples;

        // MSAA color + depth images per swap chain image (dynamic rendering — no FB)
        for (size_t i = 0; i < m_swapImages.size(); ++i) {
            // MSAA color
            VkImage msaaColor; VmaAllocation msaaColorMem; VkImageView msaaColorView;
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = m_swapFormat;
            ici.extent = {m_swapWidth, m_swapHeight, 1};
            ici.mipLevels = 1; ici.arrayLayers = 1;
            ici.samples = msaaSamples;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (msaaSamples == VK_SAMPLE_COUNT_1_BIT)
                ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo macci{};
            macci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &ici, &macci, &msaaColor, &msaaColorMem, nullptr);
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = msaaColor; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = m_swapFormat;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &vci, nullptr, &msaaColorView);

            // Depth
            VkImage depthImg; VmaAllocation depthMem; VkImageView depthView;
            VkImageCreateInfo dci{};
            dci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            dci.imageType = VK_IMAGE_TYPE_2D;
            dci.format = VK_FORMAT_D32_SFLOAT;
            dci.extent = {m_swapWidth, m_swapHeight, 1};
            dci.mipLevels = 1; dci.arrayLayers = 1;
            dci.samples = msaaSamples;
            dci.tiling = VK_IMAGE_TILING_OPTIMAL;
            dci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            VmaAllocationCreateInfo daciswap{};
            daciswap.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &dci, &daciswap, &depthImg, &depthMem, nullptr);
            VkImageViewCreateInfo dvci{};
            dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dvci.image = depthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            dvci.format = VK_FORMAT_D32_SFLOAT;
            dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &dvci, nullptr, &depthView);

            m_swapchainMsaaImages.push_back(msaaColor);
            m_swapchainMsaaMems.push_back(msaaColorMem);
            m_swapchainMsaaViews.push_back(msaaColorView);
            m_swapchainDepthImages.push_back(depthImg);
            m_swapchainDepthMems.push_back(depthMem);
            m_swapchainDepthViews.push_back(depthView);
        }

        // Pipelines for swapchain scene (dynamic rendering — no render pass)
        createInstancedPipeline(m_sceneLayout, m_swapWidth, m_swapHeight,
                                msaaSamples, m_swapchainPipeline, m_swapFormat, VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED);
        createMeshPipeline(m_sceneLayout, m_swapWidth, m_swapHeight,
                           msaaSamples, m_swapchainMeshPipeline, m_swapFormat, VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED);
    }

    void destroySwapchainScene() {
        if (m_swapchainMeshPipeline) { vkDestroyPipeline(m_device, m_swapchainMeshPipeline, nullptr); m_swapchainMeshPipeline = VK_NULL_HANDLE; }
        if (m_swapchainPipeline) { vkDestroyPipeline(m_device, m_swapchainPipeline, nullptr); m_swapchainPipeline = VK_NULL_HANDLE; }
        for (auto v : m_swapchainMsaaViews) if (v) vkDestroyImageView(m_device, v, nullptr);
        m_swapchainMsaaViews.clear();
        for (size_t i = 0; i < m_swapchainMsaaImages.size(); ++i)
            vmaDestroyImage(m_vmaAllocator, m_swapchainMsaaImages[i],
                i < m_swapchainMsaaMems.size() ? m_swapchainMsaaMems[i] : VK_NULL_HANDLE);
        m_swapchainMsaaImages.clear();
        m_swapchainMsaaMems.clear();
        for (auto v : m_swapchainDepthViews) if (v) vkDestroyImageView(m_device, v, nullptr);
        m_swapchainDepthViews.clear();
        for (size_t i = 0; i < m_swapchainDepthImages.size(); ++i)
            vmaDestroyImage(m_vmaAllocator, m_swapchainDepthImages[i],
                i < m_swapchainDepthMems.size() ? m_swapchainDepthMems[i] : VK_NULL_HANDLE);
        m_swapchainDepthImages.clear();
        m_swapchainDepthMems.clear();
        // Phase 5a: the persistent EASU frame images are swapchain-sized and
        // must be torn down when the swapchain is (re)built so they're recreated
        // at the new extent on the next renderFrameScene.
        destroyEasuFrameResources();
    }

    // =====================================================================
    // GLOBAL SSBO HELPERS (VMA-backed)
    // =====================================================================
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer& buf, VmaAllocation& alloc) {
        if (!m_vmaAllocator) {
            std::cerr << "[RHI-Vk] createBuffer: m_vmaAllocator is NULL!\n";
            return false;
        }
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        }
        VkResult r = vmaCreateBuffer(m_vmaAllocator, &bci, &aci, &buf, &alloc, nullptr);
        if (r != VK_SUCCESS) {
            std::cerr << "[RHI-Vk] createBuffer FAILED: size=" << size << " usage=" << usage
                      << " result=" << r << "\n";
        }
        return r == VK_SUCCESS;
    }

    // Resize global vertex SSBO if needed
    bool resizeGlobalVB(VkDeviceSize newSize) {
        // newSize is the per-frame vertex footprint (cube + skinned mesh verts);
        // grow the slice and reallocate 3x for triple-buffering.
        if (newSize <= m_globalVBSliceSize) return true;
        if (m_globalVB) { vmaDestroyBuffer(m_vmaAllocator, m_globalVB, m_globalVBMem); }
        m_globalVB = VK_NULL_HANDLE; m_globalVBMem = VK_NULL_HANDLE; m_globalVBMapped = nullptr;
        m_globalVBSliceSize = newSize;
        if (!createBuffer(m_globalVBCapacity = newSize * kMaxFrames,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          m_globalVB, m_globalVBMem))
            return false;
        vmaMapMemory(m_vmaAllocator, m_globalVBMem, &m_globalVBMapped);
        if (m_haveCubeVerts && m_globalVBMapped)
            for (int f = 0; f < kMaxFrames; ++f)
                std::memcpy((char*)m_globalVBMapped + (VkDeviceSize)f * m_globalVBSliceSize,
                            m_cubeVerts, sizeof(m_cubeVerts));
        updateBindlessBufferDescriptors();
        return true;
    }

    // Resize global index SSBO if needed
    bool resizeGlobalIB(VkDeviceSize newSize) {
        if (newSize <= m_globalIBCapacity) return true;
        if (m_globalIB) { vmaDestroyBuffer(m_vmaAllocator, m_globalIB, m_globalIBMem); }
        m_globalIB = VK_NULL_HANDLE; m_globalIBMem = VK_NULL_HANDLE; m_globalIBMapped = nullptr;
        if (!createBuffer(m_globalIBCapacity = newSize,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          m_globalIB, m_globalIBMem))
            return false;
        vmaMapMemory(m_vmaAllocator, m_globalIBMem, &m_globalIBMapped);
        updateBindlessBufferDescriptors();
        return true;
    }

    // Resize global instance SSBO if needed
    bool resizeGlobalInst(VkDeviceSize newSize) {
        if (newSize <= m_globalInstSliceSize) return true;
        if (m_globalInstBuf) { vmaDestroyBuffer(m_vmaAllocator, m_globalInstBuf, m_globalInstMem); }
        m_globalInstBuf = VK_NULL_HANDLE; m_globalInstMem = VK_NULL_HANDLE; m_globalInstMapped = nullptr;
        m_globalInstSliceSize = newSize;
        if (!createBuffer(m_globalInstCapacity = newSize * kMaxFrames,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          m_globalInstBuf, m_globalInstMem))
            return false;
        vmaMapMemory(m_vmaAllocator, m_globalInstMem, &m_globalInstMapped);
        updateBindlessBufferDescriptors();
        return true;
    }

    // ---- Light buffer (suggestions.txt #1, Vulkan mirror of DeferredRenderer's
    //      single glBufferSubData of LightUBO). Consumes the SAME std430
    //      GPULightData array that buildGpuLightData() produces, so GL and VK
    //      see identical light data; only the transport differs (vma map+memcpy
    //      here vs glBufferSubData on the GL backend). The VK lighting pass
    //      (descriptor set @ set=0, binding=0) calls this once per frame once
    //      wired into renderOffscreenScene/renderFrameScene. ----
    bool updateLightBuffer(const LightUBO& ubo) {
        if (!m_vmaAllocator) return false;
        if (m_lightBuf == VK_NULL_HANDLE) {
            if (!createBuffer(sizeof(LightUBO),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              m_lightBuf, m_lightAlloc))
                return false;
        }
        void* p = nullptr;
        if (vmaMapMemory(m_vmaAllocator, m_lightAlloc, &p) != VK_SUCCESS || !p)
            return false;
        std::memcpy(p, &ubo, sizeof(LightUBO));
        vmaUnmapMemory(m_vmaAllocator, m_lightAlloc);
        return true;
    }

    // =====================================================================
    // MEMBER VARIABLES
    // =====================================================================
    GLFWwindow* m_window = nullptr;
    int m_width = 0, m_height = 0;
    int m_swapWidth = 0, m_swapHeight = 0;
    int m_sceneW = 0, m_sceneH = 0;
    uint32_t m_frame = 0;
    uint32_t m_imageIndex = 0;
    // FSR3 (Phase 1): previous frame's viewProj for temporal reprojection.
    RHI::Mat4 m_prevViewProj{};
    // Offscreen-only frame counter for the Halton jitter. Vulkan's m_frame is
    // the *swapchain image index* (cmdBuf/framebuffer/fence ring), so it must
    // NOT be advanced from uploadSceneUBO (the offscreen path never presents
    // and never runs beginFrame/present, so m_frame would stay 0 and the
    // offscreen jitter would be frozen at {0,0}). m_offscreenFrame advances
    // once per uploadSceneUBO call on BOTH the offscreen (renderOffscreenScene)
    // and swapchain (renderFrameScene) paths, so the sub-pixel jitter marches
    // in lockstep with the GL backend. The swapchain image-index semantics of
    // m_frame are unchanged.
    uint32_t m_offscreenFrame = 0;
    bool m_frameRecording = false;
    bool m_frameRendered = false;
    bool m_frameHasScene = false;
    bool m_imguiInitialized = false;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    // Dedicated async compute queue family (AMD ACE lane). Falls back to
    // m_queueFamily when the driver exposes no compute-only family, so all
    // compute dispatches still work — just not in parallel with graphics.
    uint32_t m_computeQueueFamily = UINT32_MAX;
    VkQueue m_computeQueue = VK_NULL_HANDLE;

    // VMA allocator (Step 5: Advanced Memory Management)
    VmaAllocator m_vmaAllocator = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = {};
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> m_swapImages;
    std::vector<VkImageView> m_swapImageViews;

    // Sync — timeline semaphore replaces per-frame VkFence ring. The counter
    // monotonically increases; beginFrame waits for (m_timelineValue - kMaxFrames)
    // and endFrame signals (m_timelineValue + 1).
    VkSemaphore m_frameTimeline = VK_NULL_HANDLE;
    uint64_t m_timelineValue = 0;
    VkSemaphore m_imageAvailSem = VK_NULL_HANDLE;
    VkSemaphore m_renderDoneSem = VK_NULL_HANDLE;

    // Command buffers
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmdBufs[kMaxFrames] = {};

    // Triangle (headless)
    VkPipelineLayout m_triLayout = VK_NULL_HANDLE;
    VkPipeline m_triPipeline = VK_NULL_HANDLE;

    // Scene (offscreen + swapchain) — bindless
    VkDescriptorSetLayout m_bindlessDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessDescSet = VK_NULL_HANDLE;
    VkPipelineLayout m_sceneLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pbrPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_scenePipeline = VK_NULL_HANDLE;
    VkPipeline m_meshPipeline = VK_NULL_HANDLE;
    VkPipeline m_pbrMeshPipeline = VK_NULL_HANDLE;
    VkPipeline m_swapchainPipeline = VK_NULL_HANDLE;
    VkPipeline m_swapchainMeshPipeline = VK_NULL_HANDLE;

    // Push constant range: 16 bytes {uint textureId; uint pad[3];}
    // Shared by m_sceneLayout (mesh/pipeline) — triangle layout has its own.

    // UBO
    VkBuffer m_sceneUbo = VK_NULL_HANDLE;
    VmaAllocation m_sceneUboMem = VK_NULL_HANDLE;
    void* m_sceneUboMapped = nullptr;  // persistently mapped UBO

    // Global SSBOs (vertex data, index data, instance data — single allocation each)
    VkBuffer m_globalVB = VK_NULL_HANDLE;
    VmaAllocation m_globalVBMem = VK_NULL_HANDLE;
    void* m_globalVBMapped = nullptr;
    VkDeviceSize m_globalVBCapacity = 0;
    VkDeviceSize m_globalVBSize = 0;
    VkDeviceSize m_globalVBSliceSize = 0;   // per-frame ring slice (3x capacity)

    VkBuffer m_globalIB = VK_NULL_HANDLE;
    VmaAllocation m_globalIBMem = VK_NULL_HANDLE;
    void* m_globalIBMapped = nullptr;
    VkDeviceSize m_globalIBCapacity = 0;
    VkDeviceSize m_globalIBSize = 0;

    VkBuffer m_globalInstBuf = VK_NULL_HANDLE;
    VmaAllocation m_globalInstMem = VK_NULL_HANDLE;
    void* m_globalInstMapped = nullptr;

    // Light SSBO (std430 LightUBO — suggestions.txt #1). Vulkan mirror of the
    // GL DeferredRenderer light SSBO; host-visible so updateLightBuffer() is a
    // single map+memcpy (no staging cmd buffer).
    VkBuffer m_lightBuf = VK_NULL_HANDLE;
    VmaAllocation m_lightAlloc = VK_NULL_HANDLE;
    VkDeviceSize m_globalInstCapacity = 0;
    VkDeviceSize m_globalInstSize = 0;
    VkDeviceSize m_globalInstSliceSize = 0;   // per-frame ring slice (3x capacity)

    // Cube metadata in the global buffers
    uint32_t m_cubeVertexCount = 0;
    uint32_t m_cubeFirstVertex = 0;
    uint32_t m_cubeFirstInstance = 0;
    uint32_t m_cubeInstanceCount = 0;
    alignas(4) float m_cubeVerts[36 * 8] = {};   // static cube verts (re-published per ring slice)
    bool m_haveCubeVerts = false;

    // Per-mesh upload metadata (offsets into the global SSBOs)
    std::vector<MeshUploadInfo> m_meshUploadInfos;

    // Bindless texture array (image infos at binding 3, indexed by textureId)
    std::vector<VkDescriptorImageInfo> m_bindlessImageInfos;

    // White texture
    VkImage m_whiteTex = VK_NULL_HANDLE;
    VmaAllocation m_whiteTexMem = VK_NULL_HANDLE;
    VkImageView m_whiteTexView = VK_NULL_HANDLE;
    VkSampler m_whiteTexSampler = VK_NULL_HANDLE;

    // Swapchain scene (MSAA + depth — dynamic rendering, no render pass)
    VkSampleCountFlagBits m_swapchainSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkImage> m_swapchainMsaaImages;
    std::vector<VmaAllocation> m_swapchainMsaaMems;
    std::vector<VkImageView> m_swapchainMsaaViews;
    std::vector<VkImage> m_swapchainDepthImages;
    std::vector<VmaAllocation> m_swapchainDepthMems;
    std::vector<VkImageView> m_swapchainDepthViews;

    // FSR3 EASU (Phase 4): AMD FSR1 spatial upscaler as a Vulkan compute shader.
    // Built once (resolution-independent) and reused for every EASU dispatch; the
    // per-call render targets (low-res color input, full-res output) are transient.
    VkDescriptorSetLayout m_easuDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_easuDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_easuDescSet = VK_NULL_HANDLE;
    VkPipelineLayout m_easuPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_easuPipeline = VK_NULL_HANDLE;
    VkSampler m_easuSampler = VK_NULL_HANDLE;   // linear, clamp-to-edge (s_LinearClamp @1000)
    VkSampler m_depthPointSampler = VK_NULL_HANDLE;  // NEAREST, clamp-to-edge (depth read in temporal)
    VkBuffer m_easuConUBO = VK_NULL_HANDLE;     // std140: const0..const3 + sample0 (80B)
    VmaAllocation m_easuConUBOMem = VK_NULL_HANDLE;
    void* m_easuConMapped = nullptr;           // persistently mapped UBO

    // RCAS constant UBO (const3.x = sharpness). SEPARATE from m_easuConUBO so the
    // RCAS host upload can't clobber EASU's constants when both run in the same
    // command-buffer recording (see dispatchRcas / the UBO race note above).
    VkBuffer m_rcasConUBO = VK_NULL_HANDLE;
    VmaAllocation m_rcasConUBOMem = VK_NULL_HANDLE;
    void* m_rcasConMapped = nullptr;

    // FSR3 Phase 5 (VK swapchain EASU): persistent, swapchain-sized frame
    // targets reused every frame so we don't allocate per-frame on the hot path.
    //  - low-res half-swap G-buffer (RGBA8 color + D32 depth): rendered at
    //    m_swapWidth/2 x m_swapHeight/2, then sampled by the EASU compute pass.
    //  - full-swap R32G32B32A32_SFLOAT scratch: the EASU compute output, then
    //    blitted (color-converted) into the presentable swapchain image.
    int m_lowW = 0, m_lowH = 0;
    VkImage m_lowColorImage = VK_NULL_HANDLE;
    VmaAllocation m_lowColorAlloc = VK_NULL_HANDLE;
    VkImageView m_lowColorView = VK_NULL_HANDLE;
    VkImage m_lowDepthImage = VK_NULL_HANDLE;
    VmaAllocation m_lowDepthAlloc = VK_NULL_HANDLE;
    VkImageView m_lowDepthView = VK_NULL_HANDLE;
    VkImage m_easuFloatImage = VK_NULL_HANDLE;
    VmaAllocation m_easuFloatAlloc = VK_NULL_HANDLE;
    VkImageView m_easuFloatView = VK_NULL_HANDLE;

    // Phase 5b (RCAS): AMD FSR1 Contrast-Adaptive Sharpen, run after EASU.
    // RCAS is a 3x3-neighborhood sharpen (FsrRcasF) — NOT in-place safe, so it
    // reads the EASU float output (SRV) and writes a second full-res float
    // scratch, which is then linearly blitted (color-converted) to the
    // swapchain. Reuses the EASU set LAYOUT (bindings 0/2001/3000) but a
    // SEPARATE descriptor set so it doesn't clobber m_easuDescSet while EASU's
    // encoded binds still reference it in the same command buffer.
    VkDescriptorPool m_rcasDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_rcasDescSet = VK_NULL_HANDLE;
    VkPipeline m_rcasPipeline = VK_NULL_HANDLE;
    float m_rcasSharpness = 0.25f;   // stops of sharpness reduction (0 = sharpest)
    VkImage m_rcasFloatImage = VK_NULL_HANDLE;
    VmaAllocation m_rcasFloatAlloc = VK_NULL_HANDLE;
    VkImageView m_rcasFloatView = VK_NULL_HANDLE;

    // Dedicated low-resolution (swap/2) scene pipelines for the Phase 5a
    // half-res G-buffer render. Created against m_sceneLayout (same bindless
    // set + push-constant range as the full-res scene pipelines) at the low
    // resolution so the baked viewport matches m_low* — and crucially NOT via
    // ensureSceneResources(swap/2), which would rebuild the command pool
    // mid-frame. velocityFmt=VK_FORMAT_UNDEFINED => single-color attachment
    // (the low-res pass writes color only; no velocity yet).
    VkPipeline m_lowScenePipeline = VK_NULL_HANDLE;
    VkPipeline m_lowMeshPipeline = VK_NULL_HANDLE;
    VkPipeline m_lowPbrMeshPipeline = VK_NULL_HANDLE;

    // Half-res velocity target for the low-res G-buffer pass (RG32F).
    // Written by the velocity-aware low-res scene/mesh/pbr pipelines,
    // consumed by the velocity dilation compute pass.
    VkImage m_lowVelocityImage = VK_NULL_HANDLE;
    VmaAllocation m_lowVelocityAlloc = VK_NULL_HANDLE;
    VkImageView m_lowVelocityView = VK_NULL_HANDLE;

    // Dilated velocity output (RG32F, half-res). Written by
    // velocity_dilate.comp, available for the temporal resolve.
    VkImage m_dilatedVelocityImage = VK_NULL_HANDLE;
    VmaAllocation m_dilatedVelocityAlloc = VK_NULL_HANDLE;
    VkImageView m_dilatedVelocityView = VK_NULL_HANDLE;

    // UBO for velocity dilation (vec2 depthUvScale).
    VkBuffer m_velocityDilateUBO = VK_NULL_HANDLE;
    VmaAllocation m_velocityDilateUBOMem = VK_NULL_HANDLE;
    void* m_velocityDilateMapped = nullptr;

    // Velocity dilation compute pass (pre-EASU, pre-temporal).
    // Dilsates foreground velocity vectors over silhouette edges so FSR3
    // reprojects using the dominant surface motion at boundaries.
    VkDescriptorSetLayout m_velocityDilateDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_velocityDilateDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_velocityDilateDescSet = VK_NULL_HANDLE;
    VkPipelineLayout m_velocityDilatePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_velocityDilatePipeline = VK_NULL_HANDLE;

    // Phase 5c (temporal): ping-pong full-res RGBA16F history (RCAS read/write
    // across frames). m_histWriteIdx is the current WRITE target;
    // m_histWriteIdx^1 is the READ (previous) target. m_histValid flips true
    // after the first frame so the reproject reads real history instead of
    // the frame-0 copy.
    VkImage m_histColorImage[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VmaAllocation m_histColorAlloc[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView m_histColorView[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    uint8_t m_histWriteIdx = 0;
    bool m_histValid = false;

    // Temporal resolver (reproject+accumulate). Separate pipeline/layout/desc
    // from EASU/RCAS so the per-frame descriptor writes (curr @0, prev @1,
    // depth @2, linear sampler @1000, depth point sampler @1001, out @2000,
    // UBO @3000) never race the EASU/RCAS descriptor set in the same command
    // buffer recording.
    VkDescriptorSetLayout m_temporalDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_temporalDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_temporalDescSet = VK_NULL_HANDLE;
    VkPipelineLayout m_temporalPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_temporalPipeline = VK_NULL_HANDLE;
    VkBuffer m_temporalConUBO = VK_NULL_HANDLE;
    VmaAllocation m_temporalConUBOMem = VK_NULL_HANDLE;
    void* m_temporalConMapped = nullptr;
    float m_temporalFeedback = 0.9f;   // 0..1 blend weight on the current frame

    // =====================================================================
    // Virtualized Geometry — Cluster Culling (Phase 2)
    // =====================================================================
    // All NULL when no clustered meshes are present — the cluster path is
    // lazily initialized on the first mesh with clusters.
    VkBuffer m_clusterBoundsBuf  = VK_NULL_HANDLE;  // GPUClusterBounds[]
    VmaAllocation m_clusterBoundsMem = VK_NULL_HANDLE;
    VkBuffer m_clusterCmdBuf = VK_NULL_HANDLE;     // GPUClusterCommand[]
    VmaAllocation m_clusterCmdMem  = VK_NULL_HANDLE;
    VkBuffer m_indirectDrawBuf = VK_NULL_HANDLE;   // VkDrawIndexedIndirectCommand[]
    VmaAllocation m_indirectDrawMem = VK_NULL_HANDLE;
    VkBuffer m_clusterCounterBuf = VK_NULL_HANDLE; // atomic draw-count counter
    VmaAllocation m_clusterCounterMem = VK_NULL_HANDLE;
    void* m_clusterCounterMapped = nullptr;

    VkDescriptorSetLayout m_clusterDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_clusterDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_clusterDescSet = VK_NULL_HANDLE;
    VkPipelineLayout m_clusterCullLayout = VK_NULL_HANDLE;
    VkPipeline m_clusterCullPipeline = VK_NULL_HANDLE;
    VkPipeline m_naniteLodPipeline = VK_NULL_HANDLE;

    bool m_clusterCullInitialized = false;
    uint32_t m_totalClusterCount = 0;
};

} // namespace

// Factory
std::unique_ptr<IRHI> createVulkanRHI() {
    return std::make_unique<RHIVulkan>();
}

} // namespace RHI
