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
#include <array>
#include <cstring>
#include <cstdlib>   // getenv (RTT_DEFERRED_SCENE swapchain-deferred toggle)
#include <cmath>
#include <algorithm>
#include <string>

// Lightweight Vulkan-result verifier used by the bloom descriptor/image setup
// lambdas. Logs failures and continues; the bloom path is a guarded no-op when
// pipelines fail to build, so a hard abort would be inappropriate.
#define VK_VERIFY(x) do { VkResult _vk_r = (x); if (_vk_r != VK_SUCCESS) std::cerr << "[RHI-Vk] VK_VERIFY failed (code " << _vk_r << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } while (0)

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
//   ClusterBounds  = 4 × vec4 (padded to 64 bytes — one Vega cache-line)
//   ClusterCommand = 4 × uint  (std430, 16 bytes — already cache-aligned)
// ---------------------------------------------------------------------------
struct VkClusterBounds {
    glm::vec4 sphereCenterRadius;
    glm::vec4 minBounds;
    glm::vec4 maxBounds;
    glm::vec4 pad;                  // 64-byte cache-line alignment (UMA/APU)
};
struct VkClusterCommand {
    uint32_t firstVertex;
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t instanceId;
};
static_assert(sizeof(VkClusterBounds) == 64, "VkClusterBounds must be 64 bytes (4 x vec4, APU cache-line aligned)");
static_assert(sizeof(VkClusterCommand) == 16, "VkClusterCommand must be 16 bytes");

// ---------------------------------------------------------------------------
// Phase 3 (nanite_lod.comp) per-cluster LOD record. Matches the GLSL
// `LODCluster` block (64 bytes = one Vega cache-line). cluster_cull only reads
// .sphereCenterRadius; the error/parent fields drive the SSE LOD selector.
// With a flat single-level cluster set (parentId = 0, errorSelf = 0) every
// frustum-visible cluster renders — functionally identical to Phase 2 culling
// but through the NaNITE LOD codepath, ready for a real error/parent hierarchy.
// ---------------------------------------------------------------------------
struct VkLODCluster {
    glm::vec4 sphereCenterRadius;   // offset 0  (center.xyz, radius)
    float     errorSelf;            // offset 16 (this cluster's geometric error)
    float     errorParent;          // offset 20 (parent's error; 0 = no hierarchy yet)
    uint32_t  firstVertex;          // offset 24
    uint32_t  firstIndex;           // offset 28
    uint32_t  indexCount;           // offset 32
    uint32_t  instanceId;           // offset 36
    uint32_t  parentId;             // offset 40 (0 = root)
    uint32_t  pad0;                 // offset 44
    glm::vec4 extraPadding;         // offset 48 (16 B -> total 64, Vega cache-line)
};
static_assert(sizeof(VkLODCluster) == 64, "VkLODCluster must be 64 bytes (4 x vec4) to match GLSL LODCluster");

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
    int      layerTexId[4] = {-1, -1, -1, -1};  // 4-layer splat layer ids (terrain)
    uint64_t vertexVersion = 0;
    uint64_t instanceVersion = 0;
    // --- Skinning (4-bone LBS) ---
    uint32_t boneOffset = 0;   // mat4 index into the mesh's bone palette
    uint32_t boneCount  = 0;   // joint count for this mesh
    VkDescriptorSet skinDescSet = VK_NULL_HANDLE;  // set=1 skin VB + palette
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

[[maybe_unused]]
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

        // ── DRS: VK_QUERY_TYPE_TIMESTAMP pool for GPU scene-pass timing ──
        // 2 timestamps per ring slot (start/end). Non-fatal if it fails — DRS
        // simply stays at 1.0 scale until a pool is recovered.
        VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = kMaxFrames * 2;
        if (vkCreateQueryPool(m_device, &qpci, nullptr, &m_queryPool) != VK_SUCCESS) {
            std::cerr << "[RHI-Vk] Failed to create DRS timestamp query pool\n";
            m_queryPool = VK_NULL_HANDLE;
        }

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
        // Phase-B B4 skybox cubemaps: final teardown (persistent across renders,
        // destroyed before the VMA allocator goes away).
        for (size_t i = 0; i < m_cubemaps.size(); ++i) destroyCubemapTexture((uint32_t)(i + 1));
        m_cubemaps.clear();

        // Light SSBO (destroy while the VMA allocator is still alive)
        if (m_lightBuf) { vmaDestroyBuffer(m_vmaAllocator, m_lightBuf, m_lightAlloc);
                            m_lightBuf = VK_NULL_HANDLE; m_lightAlloc = VK_NULL_HANDLE; }

        // Destroy VMA allocator before device teardown
        if (m_vmaAllocator) {
            vmaDestroyAllocator(m_vmaAllocator);
            m_vmaAllocator = VK_NULL_HANDLE;
        }

        if (m_queryPool) { vkDestroyQueryPool(m_device, m_queryPool, nullptr); m_queryPool = VK_NULL_HANDLE; }
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

            // ── DRS: read back last frame's scene-pass GPU timestamps on THIS
            // ring slot (written kMaxFrames ago), then nudge the scale toward
            // the frame budget. Skipped for the first 3 frames (no history).
            if (m_queryPool && m_drsEnabled) {
                uint64_t timestamps[2] = { 0, 0 };
                VkResult queryRes = vkGetQueryPoolResults(
                    m_device, m_queryPool,
                    m_frame * 2, 2,
                    sizeof(timestamps), timestamps,
                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

                if (queryRes == VK_SUCCESS && timestamps[0] != 0 && timestamps[1] != 0) {
                    VkPhysicalDeviceProperties pdp;
                    vkGetPhysicalDeviceProperties(m_physical, &pdp);
                    float periodNs = pdp.limits.timestampPeriod;
                    m_gpuTimeMs = (float)(timestamps[1] - timestamps[0]) * periodNs / 1000000.0f;

                    if (m_gpuTimeMs > m_targetFrameTimeMs) {
                        m_drsScaleFactor -= 0.05f;
                        if (m_drsScaleFactor < m_drsMinScale) m_drsScaleFactor = m_drsMinScale;
                    } else if (m_gpuTimeMs < m_targetFrameTimeMs * 0.75f) {
                        m_drsScaleFactor += 0.01f;
                        if (m_drsScaleFactor > m_drsMaxScale) m_drsScaleFactor = m_drsMaxScale;
                    }
                }
            }
        }
        VkResult res = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                              m_imageAvailSem, VK_NULL_HANDLE, &m_imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            handleWindowResize();
            return false;
        }
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return false;
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
        // VK_ERROR_OUT_OF_DATE_KHR / SUBOPTIMAL_KHR => the swapchain no longer
        // matches the surface (window resize / mode change): recreate it
        // cleanly instead of stalling on the next acquire.
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            handleWindowResize();
        }
        m_frame = (m_frame + 1) % kMaxFrames;
        m_frameRendered = false;
    }

    // ── DRS: recreate the swapchain + low-res EASU targets on window resize ──
    // Invoked when vkAcquireNextImageKHR / vkQueuePresentKHR report
    // VK_ERROR_OUT_OF_DATE_KHR (surface no longer matches the swapchain).
    void handleWindowResize() {
        int fbW = m_width, fbH = m_height;
        if (m_window) glfwGetFramebufferSize(m_window, &fbW, &fbH);
        if (fbW == 0 || fbH == 0) return;  // window minimized — skip

        vkDeviceWaitIdle(m_device);
        m_width  = fbW;
        m_height = fbH;
        destroySwapchainScene();   // free low-res pipelines/views/images
        destroySwapchain();        // free the swapchain + image views
        if (!createSwapchain()) {  // re-acquire surface caps + set m_swapWidth/Height
            std::cerr << "[RHI-Vk] Failed to recreate swapchain on resize\n";
            return;
        }
        ensureSwapchainScene();    // rebuild scene pipelines at the new dims
        ensureEasuFrameResources();// re-provision m_lowW/m_lowH (50% of new) targets
        m_drsScaleFactor = 1.0f;   // restart DRS tracking at native resolution
        m_histValid = false;       // invalidate temporal history (no stretching)
        m_frameRecording = false;
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
                              float* outVelocityRG = nullptr,
                              bool deferred = false,
                              unsigned char* outGBufferAlbedo = nullptr) override {
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

        // ---- Deferred G-Buffer lighting pass (offscreen verification) ----
        // Geometry stage writes material properties (albedo + normal/roughness)
        // to a 2-attachment G-Buffer; a fullscreen lighting pass then composites
        // a directional light into offImg (the final color target). This proves
        // the Vulkan RHI can run a deferred (geometry -> lighting) pipeline — the
        // parity gap vs the GL deferred RenderPipeline (test.cpp) — without
        // touching the swapchain renderFrameScene path (which stays forward +
        // FSR3 EASU). The shared setup above (offImg, offDepth, velImg, cmd,
        // transitions) is reused; the forward block below is skipped on return.
        if (deferred) {
            VkImage  gAlbedoImg = VK_NULL_HANDLE, gNrImg = VK_NULL_HANDLE;
            VmaAllocation gAlbedoMem = VK_NULL_HANDLE, gNrMem = VK_NULL_HANDLE;
            VkImageView gAlbedoView = VK_NULL_HANDLE, gNrView = VK_NULL_HANDLE;

            auto makeGB = [&](VkImage& img, VmaAllocation& mem, VkImageView& view,
                              VkFormat fmt, VkImageUsageFlags usage) {
                VkImageCreateInfo ici{};
                ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
                ici.extent = {(uint32_t)w, (uint32_t)h, 1};
                ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
                ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = usage;
                ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                vmaCreateImage(m_vmaAllocator, &ici, &aci, &img, &mem, nullptr);
                VkImageViewCreateInfo vci{};
                vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCreateImageView(m_device, &vci, nullptr, &view);
            };
            makeGB(gAlbedoImg, gAlbedoMem, gAlbedoView, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            makeGB(gNrImg, gNrMem, gNrView, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

            if (!m_gbufferPipeline || !m_lightingPipeline || !m_lightingDescSet) {
                // Deferred shaders unavailable — bail out (no pixels).
                vkDestroyImageView(m_device, gAlbedoView, nullptr); vmaDestroyImage(m_vmaAllocator, gAlbedoImg, gAlbedoMem);
                vkDestroyImageView(m_device, gNrView, nullptr);                   vmaDestroyImage(m_vmaAllocator, gNrImg, gNrMem);
                return false;
            }

            // B1: ensure the WHITE shadow map + identity ShadowUBO exist for the
            // lighting pass below (white compare => fully lit => lit-parity).
            ensureSharedShadowResources();

            // --- G-Buffer geometry pass: albedo + normal/roughness + depth ---
            transitionImageLayout(cmd, gAlbedoImg, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            transitionImageLayout(cmd, gNrImg, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo gAlAtt{}, gNrAtt{}, gDpAtt{};
            gAlAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            gAlAtt.imageView = gAlbedoView; gAlAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            gAlAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; gAlAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            gAlAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            gNrAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            gNrAtt.imageView = gNrView; gNrAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            gNrAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; gNrAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            gNrAtt.clearValue.color = {{0.0f, 0.0f, 1.0f, 1.0f}};  // default normal +Z
            gDpAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            gDpAtt.imageView = offDepthView; gDpAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            gDpAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; gDpAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // STORE: B2 SSAO samples scene depth after geometry
            gDpAtt.clearValue.depthStencil = {1.0f, 0};
            VkRenderingAttachmentInfo gAtts[2] = { gAlAtt, gNrAtt };
            VkRenderingInfo gRI{};
            gRI.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            gRI.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
            gRI.layerCount = 1; gRI.colorAttachmentCount = 2; gRI.pColorAttachments = gAtts;
            gRI.pDepthAttachment = &gDpAtt;
            vkCmdBeginRendering(cmd, &gRI);
            VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {(uint32_t)w, (uint32_t)h}};
            vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
            if (!scene.meshes.empty() && m_gbufferPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                        0, 1, &m_bindlessDescSet, 0, nullptr);
                uint32_t texId = 0;
                vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(uint32_t), &texId);
                drawMeshes(cmd, scene, m_gbufferPipeline);  // generic meshes -> G-Buffer (terrain not used in tests)
            }
            vkCmdEndRendering(cmd);

            // --- B2 SSAO -------------------------------------------------------
            // Geometry done. The G-Buffer (albedo/normal/depth) is written by the
            // SAME geometry pass on GL & Vulkan; the kernel + 4x4 noise come from
            // the shared RHI::generateSsao*() generators (identical bytes); only
            // the NDC-z remap differs (VK z=[0,1] sampled directly, GL remaps
            // z*2-1 in its twin). -> GL<->VK AO parity. The blurred AO is bound to
            // lighting@6; a WHITE fallback (m_whiteTexView) is used when ssao is
            // off, so the 60/60 deferred parity test stays byte-identical.
            //
            // NOTE: the SSAO pipelines + AO images are CREATED BY ensureSsaoResources
            // — so we must NOT gate its call on the pipelines being already non-null
            // (that would deadlock: "if pipelines exist -> create them" never fires).
            // Guard only the dispatch body after creation instead.
            if (scene.ssao) {
                ensureSsaoResources(cmd, w, h);
                uploadSsaoUBO(cmd, scene, w, h);
                if (m_ssaoPipeline && m_ssaoBlurPipeline) {
            // raw AO: (UNDEFINED|SHADER_READ_ONLY) -> GENERAL for the
                // storage-image WRITE that the SSAO compute performs.
                {
                    VkImageMemoryBarrier2 br{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    br.image = m_ssaoAoRawImg;
                    br.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    br.oldLayout = m_ssaoAoRawLayout;
                    br.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    br.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    br.srcAccessMask = m_ssaoAoRawLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       ? VK_ACCESS_2_SHADER_READ_BIT : 0;
                    br.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    br.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &br;
                    vkCmdPipelineBarrier2(cmd, &d);
                }
                m_ssaoAoRawLayout = VK_IMAGE_LAYOUT_GENERAL;

                // offDepth: DEPTH_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY (sampled by SSAO)
                {
                    VkImageMemoryBarrier2 bd{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    bd.image = offDepthImg;
                    bd.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    bd.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    bd.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    bd.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                    bd.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    bd.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    bd.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &bd;
                    vkCmdPipelineBarrier2(cmd, &d);
                }

                VkDescriptorImageInfo dInfo{}, nInfo{}, nzInfo{}, rawAoInfo{};
                dInfo.imageView = offDepthView; dInfo.sampler = m_lightingSampler;
                dInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                nInfo.imageView = gNrView; nInfo.sampler = m_lightingSampler;
                nInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                nzInfo.imageView = m_ssaoNoiseView; nzInfo.sampler = m_lightingSampler;
                nzInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                rawAoInfo.imageView = m_ssaoAoRawView;
                rawAoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;            // SSAO writes raw AO (storage)
                VkDescriptorBufferInfo ssaoUBO{};
                ssaoUBO.buffer = m_ssaoKernelUbo; ssaoUBO.offset = 0;
                ssaoUBO.range = sizeof(SsaoUBOData);
                VkWriteDescriptorSet s0{}, s1{}, s2{}, s3{}, s4{};
                s0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s0.dstSet = m_ssaoDescSet;
                s0.dstBinding = 0; s0.descriptorCount = 1;
                s0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s0.pImageInfo = &dInfo; // sampler2D uDepth
                s1 = s0; s1.dstBinding = 1; s1.pImageInfo = &nInfo;                               // uNormal
                s2 = s0; s2.dstBinding = 2; s2.pImageInfo = &nzInfo;                              // uNoise
                s3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s3.dstSet = m_ssaoDescSet;
                s3.dstBinding = 3; s3.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                s3.descriptorCount = 1; s3.pBufferInfo = &ssaoUBO;
                s4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s4.dstSet = m_ssaoDescSet;
                s4.dstBinding = 4; s4.descriptorCount = 1;
                s4.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; s4.pImageInfo = &rawAoInfo;
                VkWriteDescriptorSet sWrites[] = { s0, s1, s2, s3, s4 };
                vkUpdateDescriptorSets(m_device, 5, sWrites, 0, nullptr);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoPipelineLayout,
                                        0, 1, &m_ssaoDescSet, 0, nullptr);
                const uint32_t gx = (uint32_t)((w + 15) / 16), gy = (uint32_t)((h + 15) / 16);
                vkCmdDispatch(cmd, gx, gy, 1);

                // raw AO: GENERAL -> SHADER_READ_ONLY (sampled by the blur pass)
                {
                    VkImageMemoryBarrier2 ba{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    ba.image = m_ssaoAoRawImg;
                    ba.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    ba.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                    ba.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    ba.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    ba.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    ba.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    ba.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &ba;
                    vkCmdPipelineBarrier2(cmd, &d);
                }
                m_ssaoAoRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                // Blur: raw AO (binding 0, sampled) -> blurred AO (binding 1, storage)
                // blurred AO: (UNDEFINED|SHADER_READ_ONLY) -> GENERAL for the
                // storage-image WRITE the blur compute performs.
                {
                    VkImageMemoryBarrier2 bb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    bb.image = m_ssaoAoBlurImg;
                    bb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    bb.oldLayout = m_ssaoAoBlurLayout;
                    bb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    bb.srcAccessMask = m_ssaoAoBlurLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       ? VK_ACCESS_2_SHADER_READ_BIT : 0;
                    bb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    bb.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &bb;
                    vkCmdPipelineBarrier2(cmd, &d);
                }
                m_ssaoAoBlurLayout = VK_IMAGE_LAYOUT_GENERAL;
                VkDescriptorImageInfo bAoInfo{}, bOutInfo{};
                bAoInfo.imageView = m_ssaoAoRawView; bAoInfo.sampler = m_lightingSampler;
                bAoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bOutInfo.imageView = m_ssaoAoBlurView;
                bOutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;            // blur writes blurred AO
                VkWriteDescriptorSet b0{}, b1{};
                b0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; b0.dstSet = m_ssaoBlurDescSet;
                b0.dstBinding = 0; b0.descriptorCount = 1;
                b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b0.pImageInfo = &bAoInfo;
                b1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; b1.dstSet = m_ssaoBlurDescSet;
                b1.dstBinding = 1; b1.descriptorCount = 1;
                b1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b1.pImageInfo = &bOutInfo;
                VkWriteDescriptorSet bWrites[] = { b0, b1 };
                vkUpdateDescriptorSets(m_device, 2, bWrites, 0, nullptr);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoBlurPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoBlurPipelineLayout,
                                        0, 1, &m_ssaoBlurDescSet, 0, nullptr);
                vkCmdDispatch(cmd, gx, gy, 1);

                // blurred AO: GENERAL -> SHADER_READ_ONLY (sampled by lighting@6)
                {
                    VkImageMemoryBarrier2 bb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    bb.image = m_ssaoAoBlurImg;
                    bb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    bb.oldLayout = m_ssaoAoBlurLayout;
                    bb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    bb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    bb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    bb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &bb;
                    vkCmdPipelineBarrier2(cmd, &d);
                }
                m_ssaoAoBlurLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }

            // --- Lighting pass: fullscreen triangle -> offImg (final color) ---
            transitionImageLayout(cmd, gAlbedoImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            transitionImageLayout(cmd, gNrImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            VkDescriptorImageInfo gAlInfo{}, gNrInfo{}, gShInfo{}, gDpInfo{}, gAoInfo{};
            gAlInfo.imageView = gAlbedoView; gAlInfo.sampler = m_lightingSampler;
            gAlInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            gNrInfo.imageView = gNrView; gNrInfo.sampler = m_lightingSampler;
            gNrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            // B1: offline parity path — WHITE shadow (compare=>1.0 = fully lit)
            // + identity ShadowUBO + white depth tap, so the rewritten
            // lighting.frag resolves to the pre-CSM lit output (60/60 preserved).
            gShInfo.imageView = m_whiteShadowView; gShInfo.sampler = m_shadowSampler;
            gShInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            gDpInfo.imageView = m_whiteTexView; gDpInfo.sampler = m_lightingSampler;
            gDpInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            // B2 SSAO: blurred AO factor bound to uAo (binding 6); white fallback
            // when ssao is off keeps the deferred parity path byte-identical.
            gAoInfo.imageView = (scene.ssao && m_ssaoAoBlurView) ? m_ssaoAoBlurView : m_whiteTexView;
            gAoInfo.sampler   = (scene.ssao && m_ssaoAoBlurView) ? m_lightingSampler : m_lightingSampler;
            gAoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo camUBO{}, shUBO{};
            camUBO.buffer = m_sceneUbo; camUBO.offset = 0; camUBO.range = sizeof(CameraUBOData);
            shUBO.buffer = m_shadowUbo; shUBO.offset = 0; shUBO.range = sizeof(ShadowUBO);
            VkWriteDescriptorSet w0{}, w1{}, w2{}, w3{}, w4{}, w5{}, w6{};
            w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w0.dstSet = m_lightingDescSet;
            w0.dstBinding = 0; w0.dstArrayElement = 0; w0.descriptorCount = 1;
            w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w0.pImageInfo = &gAlInfo;
            w1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w1.dstSet = m_lightingDescSet;
            w1.dstBinding = 1; w1.dstArrayElement = 0; w1.descriptorCount = 1;
            w1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w1.pImageInfo = &gNrInfo;
            w2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w2.dstSet = m_lightingDescSet;
            w2.dstBinding = 2; w2.dstArrayElement = 0; w2.descriptorCount = 1;
            w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w2.pImageInfo = &gShInfo;
            w3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w3.dstSet = m_lightingDescSet;
            w3.dstBinding = 3; w3.dstArrayElement = 0; w3.descriptorCount = 1;
            w3.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w3.pBufferInfo = &camUBO;
            w4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w4.dstSet = m_lightingDescSet;
            w4.dstBinding = 4; w4.dstArrayElement = 0; w4.descriptorCount = 1;
            w4.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w4.pImageInfo = &gDpInfo;
            w5.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w5.dstSet = m_lightingDescSet;
            w5.dstBinding = 5; w5.dstArrayElement = 0; w5.descriptorCount = 1;
            w5.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w5.pBufferInfo = &shUBO;
            w6.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w6.dstSet = m_lightingDescSet;
            w6.dstBinding = 6; w6.dstArrayElement = 0; w6.descriptorCount = 1;
            w6.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w6.pImageInfo = &gAoInfo;
            VkWriteDescriptorSet writes[] = { w0, w1, w2, w3, w4, w5, w6 };
            vkUpdateDescriptorSets(m_device, 7, writes, 0, nullptr);
            VkRenderingAttachmentInfo lAtt{};
            lAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; lAtt.imageView = offView;
            lAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            lAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; lAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo lRI{};
            lRI.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            lRI.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
            lRI.layerCount = 1; lRI.colorAttachmentCount = 1; lRI.pColorAttachments = &lAtt;
            lRI.pDepthAttachment = nullptr;
            vkCmdBeginRendering(cmd, &lRI);
            vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipelineLayout,
                                    0, 1, &m_lightingDescSet, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);   // fullscreen triangle
            vkCmdEndRendering(cmd);

            // --- readback: final lit color (offImg) + optional G-Buffer albedo ---
            transitionImageLayout(cmd, offImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBuffer gReadBuf = VK_NULL_HANDLE; VmaAllocation gReadMem = VK_NULL_HANDLE;
            VkBuffer lReadBuf = VK_NULL_HANDLE; VmaAllocation lReadMem = VK_NULL_HANDLE;
            VkBufferImageCopy bic{};
            bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            if (outRGBA) {
                createBuffer(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             lReadBuf, lReadMem);
                vkCmdCopyImageToBuffer(cmd, offImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       lReadBuf, 1, &bic);
            }
            if (outGBufferAlbedo) {
                transitionImageLayout(cmd, gAlbedoImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                createBuffer(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             gReadBuf, gReadMem);
                vkCmdCopyImageToBuffer(cmd, gAlbedoImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       gReadBuf, 1, &bic);
            }

            vkEndCommandBuffer(cmd);
            VkSubmitInfo sdi{};
            sdi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; sdi.commandBufferCount = 1; sdi.pCommandBuffers = &cmd;
            vkQueueSubmit(m_queue, 1, &sdi, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_queue);

            if (outRGBA && lReadMem) {
                void* data; vmaMapMemory(m_vmaAllocator, lReadMem, &data);
                std::memcpy(outRGBA, data, w * h * 4); vmaUnmapMemory(m_vmaAllocator, lReadMem);
                FlipRows(outRGBA, w, h);
            }
            if (outGBufferAlbedo && gReadMem) {
                void* data; vmaMapMemory(m_vmaAllocator, gReadMem, &data);
                std::memcpy(outGBufferAlbedo, data, w * h * 4); vmaUnmapMemory(m_vmaAllocator, gReadMem);
                FlipRows(outGBufferAlbedo, w, h);
            }

            if (lReadMem) vmaDestroyBuffer(m_vmaAllocator, lReadBuf, lReadMem);
            if (gReadMem) vmaDestroyBuffer(m_vmaAllocator, gReadBuf, gReadMem);
            vkDestroyImageView(m_device, offView, nullptr);     vmaDestroyImage(m_vmaAllocator, offImg, offMem);
            vkDestroyImageView(m_device, velView, nullptr);     vmaDestroyImage(m_vmaAllocator, velImg, velMem);
            if (offDepthView) { vkDestroyImageView(m_device, offDepthView, nullptr); offDepthView = VK_NULL_HANDLE; }
            if (offDepthImg || offDepthMem) vmaDestroyImage(m_vmaAllocator, offDepthImg, offDepthMem);
            vkDestroyImageView(m_device, gAlbedoView, nullptr); vmaDestroyImage(m_vmaAllocator, gAlbedoImg, gAlbedoMem);
            vkDestroyImageView(m_device, gNrView, nullptr);     vmaDestroyImage(m_vmaAllocator, gNrImg, gNrMem);
            vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
            return true;
        }

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

        // Phase-B B4 skybox — rendered before opaque cubes; z=w pins it to the
        // far plane so depth-LESS-or-equal depth-test lets cubes/occluders win.
        drawSkyboxVk(cmd, scene);

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
    struct TemporalCon { Mat4 reproject; float feedback; float histValid; Vec2 depthUvScale; Vec2 jitterOffset; Vec2 prevJitter; };
    static TemporalCon makeTemporalCon(const Mat4& currViewProj, const Mat4& prevViewProj,
                                       bool histValid, float feedback, const Vec2& jitter,
                                       const Vec2& prevJitter) {
        const Mat4 invVP = mat4Inverse(currViewProj);
        return TemporalCon{ mat4Multiply(prevViewProj, invVP), feedback,
                            histValid ? 1.0f : 0.0f, Vec2{1.0f, 1.0f}, jitter, prevJitter };
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
            m_lowGBufAlbedoImage && m_lowGBufNormalImage &&
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

        // Deferred G-Buffer: albedo (RGBA8) + normal/roughness (RGBA16F), half-res,
        // matching renderOffscreenScene's gbuffer geometry pass. Allocated for both
        // forward and deferred; the default forward path ignores them.
        {
            VkImageCreateInfo gici{};
            gici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            gici.imageType = VK_IMAGE_TYPE_2D;
            gici.format = VK_FORMAT_R8G8B8A8_UNORM;
            gici.extent = {(uint32_t)m_lowW, (uint32_t)m_lowH, 1};
            gici.mipLevels = 1; gici.arrayLayers = 1; gici.samples = VK_SAMPLE_COUNT_1_BIT;
            gici.tiling = VK_IMAGE_TILING_OPTIMAL;
            gici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            gici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo gaci{}; gaci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &gici, &gaci,
                           &m_lowGBufAlbedoImage, &m_lowGBufAlbedoAlloc, nullptr);
            VkImageViewCreateInfo gavci{};
            gavci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            gavci.image = m_lowGBufAlbedoImage; gavci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            gavci.format = VK_FORMAT_R8G8B8A8_UNORM;
            gavci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &gavci, nullptr, &m_lowGBufAlbedoView);

            VkImageCreateInfo gnici = gici; gnici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            gnici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            vmaCreateImage(m_vmaAllocator, &gnici, &gaci,
                           &m_lowGBufNormalImage, &m_lowGBufNormalAlloc, nullptr);
            VkImageViewCreateInfo gnvci = gavci; gnvci.image = m_lowGBufNormalImage;
            gnvci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            vkCreateImageView(m_device, &gnvci, nullptr, &m_lowGBufNormalView);
        }

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
        createPBRTerrainPipeline(m_pbrPipelineLayout, lW ? lW : 1, lH ? lH : 1,
                                 VK_SAMPLE_COUNT_1_BIT, m_lowPbrTerrainPipeline,
                                 VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                 VK_FORMAT_R32G32_SFLOAT);
    }

    void destroyEasuFrameResources() {
        if (m_lowPbrMeshPipeline) { vkDestroyPipeline(m_device, m_lowPbrMeshPipeline, nullptr); m_lowPbrMeshPipeline = VK_NULL_HANDLE; }
        if (m_lowPbrTerrainPipeline) { vkDestroyPipeline(m_device, m_lowPbrTerrainPipeline, nullptr); m_lowPbrTerrainPipeline = VK_NULL_HANDLE; }
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
        if (m_lowGBufNormalView) { vkDestroyImageView(m_device, m_lowGBufNormalView, nullptr); m_lowGBufNormalView = VK_NULL_HANDLE; }
        if (m_lowGBufNormalImage) { vmaDestroyImage(m_vmaAllocator, m_lowGBufNormalImage, m_lowGBufNormalAlloc); m_lowGBufNormalImage = VK_NULL_HANDLE; m_lowGBufNormalAlloc = VK_NULL_HANDLE; }
        if (m_lowGBufAlbedoView) { vkDestroyImageView(m_device, m_lowGBufAlbedoView, nullptr); m_lowGBufAlbedoView = VK_NULL_HANDLE; }
        if (m_lowGBufAlbedoImage) { vmaDestroyImage(m_vmaAllocator, m_lowGBufAlbedoImage, m_lowGBufAlbedoAlloc); m_lowGBufAlbedoImage = VK_NULL_HANDLE; m_lowGBufAlbedoAlloc = VK_NULL_HANDLE; }
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
        // Phase B bridge: the editor real-time passes (skybox, CSM shadows,
        // SSAO, SSR, bloom) are threaded through OffscreenScene but not yet
        // implemented on the Vulkan swapchain path. Log ONCE so they are visible
        // (not silently dropped); forward rendering stays bit-identical to the
        // offline parity path (all flags default OFF).
        static bool s_rhiPhaseBNotice = false;
        if (!s_rhiPhaseBNotice && (scene.skyEnabled || scene.shadows || scene.ssao || scene.ssr || scene.bloom)) {
            s_rhiPhaseBNotice = true;
            if (scene.skyEnabled) std::cerr << "[RHI-Vk] skybox  : not yet ported (solid clear) [Phase B B4]\n";
            if (scene.shadows)    std::cerr << "[RHI-Vk] CSM     : not yet ported (forward)    [Phase B B1]\n";
            if (scene.ssao)       std::cerr << "[RHI-Vk] SSAO    : not yet ported              [Phase B B2]\n";
            if (scene.ssr)        std::cerr << "[RHI-Vk] SSR     : not yet ported              [Phase B B3]\n";
            if (scene.bloom)      std::cerr << "[RHI-Vk] bloom   : not yet ported              [Phase B B5]\n";
        }
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

            // DRS: contract the active low-res render region toward 50% of the
            // max low-res buffer. The low-res IMAGES (allocated once at rW x rH
            // in ensureEasuFrameResources) stay fixed; we render into the
            // top-left active sub-region and feed only those dims to EASU/velocity.
            // At 1.0 scale aW==rW/H (a no-op), so DRS is invisible until the GPU
            // exceeds budget and the scale drops below 1.0.
            int aW = (int)((float)rW * m_drsScaleFactor);
            int aH = (int)((float)rH * m_drsScaleFactor);
            if (aW < 1) aW = 1;
            if (aH < 1) aH = 1;
            m_activeLowW = (uint32_t)aW;
            m_activeLowH = (uint32_t)aH;

            // firstFrame is hoisted here so the depth-barrier state machine
            // (ldBar) and the temporal history (tNext) both see the same value.
            const bool firstFrame = !m_histValid;

            // Same jitter the low-res render below will use — captured BEFORE
            // uploadSceneUBO advances m_offscreenFrame.
            const Vec2 j = ndcJitter((int)m_offscreenFrame, aW, aH);

            // Capture the previous frame's viewProj BEFORE uploadSceneUBO
            // (which overwrites m_prevViewProj with the current frame's) so the
            // temporal reproject R = prevViewProj * inverse(viewProj) lines up the
            // PREVIOUS history onto the CURRENT frame.
            const Mat4 prevVP = m_prevViewProj;
            uploadSceneUBO(scene, aW, aH);   // project+jitter use active DRS dims
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

            const bool deferredScene = scene.deferred ||
                (std::getenv("RTT_DEFERRED_SCENE") != nullptr);
            if (deferredScene && m_gbufferPipeline && m_lightingPipeline &&
                m_lightingDescSet && m_lightingSampler &&
                m_lowGBufAlbedoView && m_lowGBufNormalView) {
                // --- Deferred (G-Buffer -> lighting) low-res path. Mirrors
                // renderOffscreenScene(deferred=true) [RHIVulkan.cpp:~1060]: the
                // scene renders into a 2-attachment G-Buffer (albedo + normal/rough)
                // over m_lowDepthView, then a fullscreen lighting pass composites the
                // directional light into m_lowColorView (the EASU input). DEFAULT-OFF:
                // set env RTT_DEFERRED_SCENE=1 to opt in (forward+PBR is the default
                // and is untouched when the flag is absent).
                // GPU-VERIFY CAVEAT (headless lavapipe skips swapchain): gbuffer.vert
                // emits no motion vector, so m_lowVelocityView is explicitly cleared
                // to zero here and FSR3 temporal reprojection reads motionless vectors
                // in deferred mode (no ghosting/NaN, but no parallax reprojection).
                // The lighting pass also omits the START DRX timestamp the forward
                // path brackets — benign at the default DRX scale (1.0 = no-op).
                const VkImageLayout kPrevGB =
                    firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                transitionImageLayout(cmd, m_lowGBufAlbedoImage, kPrevGB,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                transitionImageLayout(cmd, m_lowGBufNormalImage, kPrevGB,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                VkViewport vp{0.0f, 0.0f, (float)m_activeLowW, (float)m_activeLowH, 0.0f, 1.0f};
                VkRect2D sc{{0, 0}, {(uint32_t)m_activeLowW, (uint32_t)m_activeLowH}};
                {
                    VkRenderingAttachmentInfo gAlAtt{}, gNrAtt{}, gDpAtt{};
                    gAlAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    gAlAtt.imageView = m_lowGBufAlbedoView;
                    gAlAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    gAlAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; gAlAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    gAlAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                    gNrAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    gNrAtt.imageView = m_lowGBufNormalView;
                    gNrAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    gNrAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; gNrAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    gNrAtt.clearValue.color = {{0.0f, 0.0f, 1.0f, 1.0f}};  // default normal +Z
                    gDpAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    gDpAtt.imageView = m_lowDepthView;
                    gDpAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    gDpAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; gDpAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    gDpAtt.clearValue.depthStencil = {1.0f, 0};
                    VkRenderingAttachmentInfo gAtts[2] = { gAlAtt, gNrAtt };
                    VkRenderingInfo gRI{};
                    gRI.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                    gRI.renderArea = {{0, 0}, {(uint32_t)m_activeLowW, (uint32_t)m_activeLowH}};
                    gRI.layerCount = 1; gRI.colorAttachmentCount = 2; gRI.pColorAttachments = gAtts;
                    gRI.pDepthAttachment = &gDpAtt;
                    vkCmdBeginRendering(cmd, &gRI);
                    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
                    if (!scene.meshes.empty() && m_gbufferPipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sceneLayout,
                                                0, 1, &m_bindlessDescSet, 0, nullptr);
                        uint32_t texId = 0;
                        vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             0, sizeof(uint32_t), &texId);
                        drawMeshes(cmd, scene, m_gbufferPipeline);  // generic meshes -> G-Buffer
                    }
                    vkCmdEndRendering(cmd);
                    // G-Buffer -> SHADER_READ_ONLY for the lighting pass.
                    transitionImageLayout(cmd, m_lowGBufAlbedoImage,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    transitionImageLayout(cmd, m_lowGBufNormalImage,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                // B1: render the CSM shadow atlas (cascade 0 -> layer 0), then
                // transition it + the depth buffer for the lighting fullscreen
                // pass. ensureSharedShadowResources seeds the WHITE fallback
                // bindings so the pass is still valid when shadows are off.
                ensureSharedShadowResources();
                if (scene.shadows && m_shadowPipeline && m_shadowDescSet && m_shadowDepthView0) {
                    ensureShadowResources();
                    uploadShadowUBO(scene);

                    VkImageMemoryBarrier2 asb{}; asb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    asb.image = m_shadowDepthImage;
                    asb.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
                    asb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; asb.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    asb.srcStageMask = VK_PIPELINE_STAGE_2_NONE; asb.srcAccessMask = 0;
                    asb.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                    asb.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    VkDependencyInfo asdi{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; asdi.imageMemoryBarrierCount = 1; asdi.pImageMemoryBarriers = &asb;
                    vkCmdPipelineBarrier2(cmd, &asdi);

                    VkRenderingAttachmentInfo dAtt{};
                    dAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    dAtt.imageView = m_shadowDepthView0;
                    dAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    dAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    dAtt.clearValue.depthStencil = {1.0f, 0};
                    VkRenderingInfo sri{};
                    sri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                    sri.renderArea = {{0, 0}, {(uint32_t)kShadowSize, (uint32_t)kShadowSize}};
                    sri.layerCount = 1; sri.colorAttachmentCount = 0; sri.pDepthAttachment = &dAtt;
                    vkCmdBeginRendering(cmd, &sri);
                    VkViewport svp{0, 0, (float)kShadowSize, (float)kShadowSize, 0.0f, 1.0f};
                    VkRect2D ssc{{0, 0}, {kShadowSize, kShadowSize}};
                    vkCmdSetViewport(cmd, 0, 1, &svp); vkCmdSetScissor(cmd, 0, 1, &ssc);
                    drawMeshesShadow(cmd, scene, 0);   // cascade 0 -> layer 0
                    vkCmdEndRendering(cmd);

                    VkImageMemoryBarrier2 sb[2]{};
                    sb[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    sb[0].image = m_shadowDepthImage;
                    sb[0].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
                    sb[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; sb[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    sb[0].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                    sb[0].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    sb[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; sb[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                    sb[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    sb[1].image = m_lowDepthImage;
                    sb[1].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    sb[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; sb[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    sb[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                    sb[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    sb[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; sb[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                    VkDependencyInfo sdi{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; sdi.imageMemoryBarrierCount = 2; sdi.pImageMemoryBarriers = sb;
                    vkCmdPipelineBarrier2(cmd, &sdi);
                }

                VkDescriptorImageInfo gAlInfo{}, gNrInfo{}, gShInfo{}, gDpInfo{};
                gAlInfo.imageView = m_lowGBufAlbedoView; gAlInfo.sampler = m_lightingSampler;
                gAlInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                gNrInfo.imageView = m_lowGBufNormalView; gNrInfo.sampler = m_lightingSampler;
                gNrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                // B1: real shadow atlas (when scene.shadows) or white fallback.
                gShInfo.imageView = scene.shadows ? m_shadowDepthView : m_whiteShadowView;
                gShInfo.sampler = m_shadowSampler;
                gShInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                gDpInfo.imageView = m_lowDepthView; gDpInfo.sampler = m_lightingSampler;
                gDpInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkDescriptorBufferInfo camUBO{}, shUBO{};
                camUBO.buffer = m_sceneUbo; camUBO.offset = 0; camUBO.range = sizeof(CameraUBOData);
                shUBO.buffer = m_shadowUbo; shUBO.offset = 0; shUBO.range = sizeof(ShadowUBO);
                VkWriteDescriptorSet w0{}, w1{}, w2{}, w3{}, w4{}, w5{};
                w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w0.dstSet = m_lightingDescSet;
                w0.dstBinding = 0; w0.dstArrayElement = 0; w0.descriptorCount = 1;
                w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w0.pImageInfo = &gAlInfo;
                w1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w1.dstSet = m_lightingDescSet;
                w1.dstBinding = 1; w1.dstArrayElement = 0; w1.descriptorCount = 1;
                w1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w1.pImageInfo = &gNrInfo;
                w2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w2.dstSet = m_lightingDescSet;
                w2.dstBinding = 2; w2.dstArrayElement = 0; w2.descriptorCount = 1;
                w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w2.pImageInfo = &gShInfo;
                w3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w3.dstSet = m_lightingDescSet;
                w3.dstBinding = 3; w3.dstArrayElement = 0; w3.descriptorCount = 1;
                w3.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w3.pBufferInfo = &camUBO;
                w4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w4.dstSet = m_lightingDescSet;
                w4.dstBinding = 4; w4.dstArrayElement = 0; w4.descriptorCount = 1;
                w4.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w4.pImageInfo = &gDpInfo;
                w5.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w5.dstSet = m_lightingDescSet;
                w5.dstBinding = 5; w5.dstArrayElement = 0; w5.descriptorCount = 1;
                w5.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w5.pBufferInfo = &shUBO;
                VkWriteDescriptorSet writes[] = { w0, w1, w2, w3, w4, w5 };
                vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);
                // Lighting pass: fullscreen triangle -> m_lowColorView (EASU input).
                VkRenderingAttachmentInfo lAtt{};
                lAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; lAtt.imageView = m_lowColorView;
                lAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                lAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; lAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo lRI{};
                lRI.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                lRI.renderArea = {{0, 0}, {(uint32_t)m_activeLowW, (uint32_t)m_activeLowH}};
                lRI.layerCount = 1; lRI.colorAttachmentCount = 1; lRI.pColorAttachments = &lAtt;
                lRI.pDepthAttachment = nullptr;
                vkCmdBeginRendering(cmd, &lRI);
                vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipelineLayout,
                                        0, 1, &m_lightingDescSet, 0, nullptr);
                vkCmdDraw(cmd, 3, 1, 0, 0);   // fullscreen triangle
                vkCmdEndRendering(cmd);
                // Deferred G-Buffer emits no velocity: clear m_lowVelocityView to zero
                // so the dilate/temporal resolver reads motionless vectors (no garbage/NaN).
                {
                    VkClearColorValue z{}; z.float32[0] = 0.0f; z.float32[1] = 0.0f;
                    VkImageSubresourceRange sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(cmd, m_lowVelocityImage,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &z, 1, &sr);
                }
            } else {
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
            renderingInfo.renderArea = {{0, 0}, {(uint32_t)aW, (uint32_t)aH}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 2;
            renderingInfo.pColorAttachments = colorAtts;
            renderingInfo.pDepthAttachment = &depthAtt;
            // B1-forward: render the sun's CSM atlas (separate render pass,
            // outside the low-res color rendering). Noop when shadows are off
            // -> existing pbr_mesh forward path is untouched (parity / 60/60).
            if (scene.shadows) renderShadowCascade(cmd, scene);
            vkCmdBeginRendering(cmd, &renderingInfo);

            // DRS: bracket the scene pass with START/END timestamps so
            // beginFrame() can read back the scene-pass GPU time and drive
            // m_drsScaleFactor. (No-op if query-pool creation failed.)
            if (m_queryPool) {
                vkCmdResetQueryPool(cmd, m_queryPool, m_frame * 2, 2);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    m_queryPool, m_frame * 2);
            }

            VkViewport vp{0, 0, (float)aW, (float)aH, 0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scissor{{0, 0}, {(uint32_t)aW, (uint32_t)aH}};
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
            if (scene.shadows && m_shadowPbrPipeline && m_shadowPbrPipelineLayout
                && m_shadowDescSet && m_shadowDepthView) {
                drawMeshes(cmd, scene, m_lowPbrMeshPipeline,
                           m_shadowDescSet, m_shadowPbrPipeline, m_shadowSkinnedPipeline,
                           m_shadowTerrainPipeline);
            } else {
                drawMeshes(cmd, scene,
                           scene.pbrEnabled ? m_lowPbrMeshPipeline : m_lowMeshPipeline);
            }
            vkCmdEndRendering(cmd);
            } // else: forward (color+velocity+depth) low-res scene pass
            //
            // END timestamp: brackets the full scene pass (START above).
            if (m_queryPool) {
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    m_queryPool, (m_frame * 2) + 1);
            }

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
                                     (uint32_t)aW, (uint32_t)aH);

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
            dispatchEasu(cmd, m_lowColorView, (uint32_t)aW, (uint32_t)aH,
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

            const TemporalCon tcon = makeTemporalCon(currVP, prevVP, firstFrame, m_temporalFeedback, j, m_prevJitter);
            m_prevJitter = j;   // current jitter becomes "previous" for next frame's history reprojection
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
            dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT        // B2 SSAO samples scene depth
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // B2 depth readback (debugging)
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
        // 32 bytes: pbr_mesh writes 4B (textureId@0); the terrain splat path
        // writes 5 texture ids (control + 4 layers, 20B) at the same offsets.
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0; pcr.size = 32;
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1; pl.pSetLayouts = &m_bindlessDescLayout;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &pl, nullptr, &m_sceneLayout);
        m_pbrPipelineLayout = m_sceneLayout;

        m_meshUploadInfos.clear();
        m_meshTex.clear();
        m_meshLayerTex.clear();
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
        createPBRTerrainPipeline(m_sceneLayout, m_sceneW, m_sceneH,
                                 VK_SAMPLE_COUNT_1_BIT, m_pbrTerrainPipeline,
                                 VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);

        // --- Deferred G-Buffer lighting pass (offscreen test path) ---------
        // Geometry stage: pbr_mesh.vert + gbuffer.frag writing albedo (RGBA8) to
        // loc0 and normal+roughness (RGBA16f) to loc1 (the "velocity" slot,
        // repurposed as the 2nd G-Buffer attachment). Reuses createPBRMeshPipeline
        // — identical raster/depth/push-constant/sampler state, geometry-rasterizes
        // the same as the forward pbr_mesh path.
        createPBRMeshPipeline(m_sceneLayout, m_sceneW, m_sceneH,
                              VK_SAMPLE_COUNT_1_BIT, m_gbufferPipeline,
                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                              VK_FORMAT_R16G16B16A16_SFLOAT,  // 2nd color = G-Buffer normal+rough
                              "build/rhi/gbuffer.frag.spv");
        createLightingPipeline();

        // --- Skinned-mesh skeleton (set=1) ---
        // set=1 layout: b0 = per-mesh boneIdx/Weight SSBO, b1 = per-mesh joint
        // palette SSBO. Separate from the bindless set=0 so pbr_mesh/terrain/
        // gbuffer pipelines are unaffected.
        VkDescriptorSetLayoutBinding sb[2]{};
        sb[0].binding = 0; sb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sb[0].descriptorCount = 1; sb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        sb[1].binding = 1; sb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sb[1].descriptorCount = 1; sb[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo sdlci{};
        sdlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sdlci.bindingCount = 2; sdlci.pBindings = sb;
        vkCreateDescriptorSetLayout(m_device, &sdlci, nullptr, &m_skinDescSetLayout);
        VkDescriptorPoolSize spd{};
        spd.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; spd.descriptorCount = 256; // generous per-mesh sets
        VkDescriptorPoolCreateInfo sdpci{};
        sdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        sdpci.maxSets = 64; sdpci.poolSizeCount = 1; sdpci.pPoolSizes = &spd;
        vkCreateDescriptorPool(m_device, &sdpci, nullptr, &m_skinDescPool);

        const VkDescriptorSetLayout setLayouts[] = { m_bindlessDescLayout, m_skinDescSetLayout };
        VkPushConstantRange skinPCR{};
        skinPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        skinPCR.offset = 0; skinPCR.size = 32;
        VkPipelineLayoutCreateInfo spl{};
        spl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        spl.setLayoutCount = 2; spl.pSetLayouts = setLayouts;
        spl.pushConstantRangeCount = 1; spl.pPushConstantRanges = &skinPCR;
        vkCreatePipelineLayout(m_device, &spl, nullptr, &m_skinPipelineLayout);

        createPBRSkinnedPipeline(m_skinPipelineLayout, m_sceneW, m_sceneH,
                                 VK_SAMPLE_COUNT_1_BIT, m_pbrSkinnedPipeline,
                                 VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);

        // B1-forward: skinned CSM pipeline (pbr_skinned.vert +
        // pbr_skinned_shadow.frag). set0=bindless, set1=skin, set2=shadow atlas.
        // ensureSharedShadowResources seeds set=2 (m_shadowDescSetLayout) + the
        // white-atlas fallback; renderShadowCascade rebinds the live atlas.
        ensureSharedShadowResources();
        {
            VkDescriptorSetLayout ssl[3] = { m_bindlessDescLayout, m_skinDescSetLayout,
                                             m_shadowDescSetLayout };
            VkPushConstantRange skinPCR{};
            skinPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            skinPCR.offset = 0; skinPCR.size = 32;
            VkPipelineLayoutCreateInfo spl{};
            spl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            spl.setLayoutCount = 3; spl.pSetLayouts = ssl;
            spl.pushConstantRangeCount = 1; spl.pPushConstantRanges = &skinPCR;
            if (vkCreatePipelineLayout(m_device, &spl, nullptr,
                                       &m_shadowSkinnedPipelineLayout) != VK_SUCCESS)
                m_shadowSkinnedPipelineLayout = VK_NULL_HANDLE;
        }
        if (m_shadowSkinnedPipelineLayout)
            createPBRMeshPipeline(m_shadowSkinnedPipelineLayout, m_sceneW, m_sceneH,
                                  VK_SAMPLE_COUNT_1_BIT, m_shadowSkinnedPipeline,
                                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                  VK_FORMAT_R32G32_SFLOAT,
                                  "build/rhi/pbr_skinned_shadow.frag.spv",
                                  "build/rhi/pbr_skinned.vert.spv");
    }
    void destroyScene() {
        // Swapchain scene resources (pipelines + MSAA/depth images) reference
        // m_sceneLayout, so they must be destroyed before the layout below.
        destroySwapchainScene();

        for (auto& r : m_meshTex) destroyMeshTex(r);
        m_meshTex.clear();
        for (auto& arr : m_meshLayerTex) for (auto& r : arr) destroyMeshTex(r);
        m_meshLayerTex.clear();
        m_bindlessImageInfos.clear();
        if (m_triPipeline) { vkDestroyPipeline(m_device, m_triPipeline, nullptr); m_triPipeline = VK_NULL_HANDLE; }
        if (m_triLayout) { vkDestroyPipelineLayout(m_device, m_triLayout, nullptr); m_triLayout = VK_NULL_HANDLE; }
        if (m_pbrMeshPipeline && m_pbrMeshPipeline != m_meshPipeline) { vkDestroyPipeline(m_device, m_pbrMeshPipeline, nullptr); m_pbrMeshPipeline = VK_NULL_HANDLE; }
        if (m_pbrTerrainPipeline) { vkDestroyPipeline(m_device, m_pbrTerrainPipeline, nullptr); m_pbrTerrainPipeline = VK_NULL_HANDLE; }
        if (m_gbufferPipeline) { vkDestroyPipeline(m_device, m_gbufferPipeline, nullptr); m_gbufferPipeline = VK_NULL_HANDLE; }
        if (m_lightingPipeline) { vkDestroyPipeline(m_device, m_lightingPipeline, nullptr); m_lightingPipeline = VK_NULL_HANDLE; }
        if (m_lightingPipelineLayout) { vkDestroyPipelineLayout(m_device, m_lightingPipelineLayout, nullptr); m_lightingPipelineLayout = VK_NULL_HANDLE; }
        if (m_lightingDescPool) { vkDestroyDescriptorPool(m_device, m_lightingDescPool, nullptr); m_lightingDescPool = VK_NULL_HANDLE; }
        if (m_lightingDescSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_lightingDescSetLayout, nullptr); m_lightingDescSetLayout = VK_NULL_HANDLE; }
        if (m_lightingSampler) { vkDestroySampler(m_device, m_lightingSampler, nullptr); m_lightingSampler = VK_NULL_HANDLE; }

        // --- B1: Cascaded Shadow Map resources (white fallback + atlas) ---
        if (m_shadowPipeline) { vkDestroyPipeline(m_device, m_shadowPipeline, nullptr); m_shadowPipeline = VK_NULL_HANDLE; }
        if (m_shadowPipelineLayout) { vkDestroyPipelineLayout(m_device, m_shadowPipelineLayout, nullptr); m_shadowPipelineLayout = VK_NULL_HANDLE; }
        if (m_shadowSkinnedPipeline) { vkDestroyPipeline(m_device, m_shadowSkinnedPipeline, nullptr); m_shadowSkinnedPipeline = VK_NULL_HANDLE; }
        if (m_shadowSkinnedPipelineLayout) { vkDestroyPipelineLayout(m_device, m_shadowSkinnedPipelineLayout, nullptr); m_shadowSkinnedPipelineLayout = VK_NULL_HANDLE; }
        if (m_shadowTerrainPipeline) { vkDestroyPipeline(m_device, m_shadowTerrainPipeline, nullptr); m_shadowTerrainPipeline = VK_NULL_HANDLE; }
        if (m_shadowPbrPipeline) { vkDestroyPipeline(m_device, m_shadowPbrPipeline, nullptr); m_shadowPbrPipeline = VK_NULL_HANDLE; }
        if (m_shadowPbrPipelineLayout) { vkDestroyPipelineLayout(m_device, m_shadowPbrPipelineLayout, nullptr); m_shadowPbrPipelineLayout = VK_NULL_HANDLE; }
        if (m_shadowSkinnedPipeline) { vkDestroyPipeline(m_device, m_shadowSkinnedPipeline, nullptr); m_shadowSkinnedPipeline = VK_NULL_HANDLE; }
        if (m_shadowSkinnedPipelineLayout) { vkDestroyPipelineLayout(m_device, m_shadowSkinnedPipelineLayout, nullptr); m_shadowSkinnedPipelineLayout = VK_NULL_HANDLE; }
        if (m_shadowDescPool) { vkDestroyDescriptorPool(m_device, m_shadowDescPool, nullptr); m_shadowDescPool = VK_NULL_HANDLE; }
        if (m_shadowDescSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_shadowDescSetLayout, nullptr); m_shadowDescSetLayout = VK_NULL_HANDLE; }
        if (m_shadowSampler) { vkDestroySampler(m_device, m_shadowSampler, nullptr); m_shadowSampler = VK_NULL_HANDLE; }
        if (m_shadowUboMapped && m_shadowUboMem) vmaUnmapMemory(m_vmaAllocator, m_shadowUboMem);
        if (m_shadowUbo) { vmaDestroyBuffer(m_vmaAllocator, m_shadowUbo, m_shadowUboMem); m_shadowUbo = VK_NULL_HANDLE; m_shadowUboMem = VK_NULL_HANDLE; }
        m_shadowUboMapped = nullptr;
        if (m_whiteShadowView) { vkDestroyImageView(m_device, m_whiteShadowView, nullptr); m_whiteShadowView = VK_NULL_HANDLE; }
        if (m_whiteShadowImage) { vmaDestroyImage(m_vmaAllocator, m_whiteShadowImage, m_whiteShadowAlloc); m_whiteShadowImage = VK_NULL_HANDLE; m_whiteShadowAlloc = VK_NULL_HANDLE; }
        if (m_shadowDepthView) { vkDestroyImageView(m_device, m_shadowDepthView, nullptr); m_shadowDepthView = VK_NULL_HANDLE; }
        if (m_shadowDepthView0) { vkDestroyImageView(m_device, m_shadowDepthView0, nullptr); m_shadowDepthView0 = VK_NULL_HANDLE; }
        if (m_shadowDepthImage) { vmaDestroyImage(m_vmaAllocator, m_shadowDepthImage, m_shadowDepthAlloc); m_shadowDepthImage = VK_NULL_HANDLE; m_shadowDepthAlloc = VK_NULL_HANDLE; }

        // --- Skinning cleanup ---
        for (auto& r : m_skinMeshRes) {
            if (r.skinVb)  { vmaDestroyBuffer(m_vmaAllocator, r.skinVb,  r.skinVbMem);  }
            if (r.boneBuf) { vmaDestroyBuffer(m_vmaAllocator, r.boneBuf, r.boneBufMem); }
            r = {};
        }
        m_skinMeshRes.clear();
        if (m_pbrSkinnedPipeline)   { vkDestroyPipeline(m_device, m_pbrSkinnedPipeline, nullptr); m_pbrSkinnedPipeline = VK_NULL_HANDLE; }
        if (m_skinPipelineLayout)   { vkDestroyPipelineLayout(m_device, m_skinPipelineLayout, nullptr); m_skinPipelineLayout = VK_NULL_HANDLE; }
        if (m_skinDescPool)         { vkDestroyDescriptorPool(m_device, m_skinDescPool, nullptr); m_skinDescPool = VK_NULL_HANDLE; }
        if (m_skinDescSetLayout)    { vkDestroyDescriptorSetLayout(m_device, m_skinDescSetLayout, nullptr); m_skinDescSetLayout = VK_NULL_HANDLE; }
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
        if (m_skyboxPipeline) { vkDestroyPipeline(m_device, m_skyboxPipeline, nullptr); m_skyboxPipeline = VK_NULL_HANDLE; }
        if (m_skyboxLayout) { vkDestroyPipelineLayout(m_device, m_skyboxLayout, nullptr); m_skyboxLayout = VK_NULL_HANDLE; }
        if (m_skyboxDescSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_skyboxDescSetLayout, nullptr); m_skyboxDescSetLayout = VK_NULL_HANDLE; }
        if (m_skyboxDescPool) { vkDestroyDescriptorPool(m_device, m_skyboxDescPool, nullptr); m_skyboxDescPool = VK_NULL_HANDLE; }
        if (m_skyboxVbo || m_skyboxVboMem) { vmaDestroyBuffer(m_vmaAllocator, m_skyboxVbo, m_skyboxVboMem);
                                             m_skyboxVbo = VK_NULL_HANDLE; m_skyboxVboMem = VK_NULL_HANDLE; }
        // Phase-B B4 skybox cubemaps: persistent user resources, torn down in
        // shutdown() only — NOT here, because destroyScene runs on every render
        // via ensureSceneResources and would kill cubemaps mid-frame.

        // B2 SSAO (Vulkan twin): freed on resize here so ensureSsaoResources
        // rebuilds them at the new resolution; lighting@6 binds the blurred AO
        // (or m_whiteTexView fallback when ssao is off -> 60/60 parity).
        if (m_ssaoPipeline)       { vkDestroyPipeline(m_device, m_ssaoPipeline, nullptr); m_ssaoPipeline = VK_NULL_HANDLE; }
        if (m_ssaoPipelineLayout) { vkDestroyPipelineLayout(m_device, m_ssaoPipelineLayout, nullptr); m_ssaoPipelineLayout = VK_NULL_HANDLE; }
        if (m_ssaoBlurPipeline)       { vkDestroyPipeline(m_device, m_ssaoBlurPipeline, nullptr); m_ssaoBlurPipeline = VK_NULL_HANDLE; }
        if (m_ssaoBlurPipelineLayout) { vkDestroyPipelineLayout(m_device, m_ssaoBlurPipelineLayout, nullptr); m_ssaoBlurPipelineLayout = VK_NULL_HANDLE; }
        if (m_ssaoDescSetLayout)  { vkDestroyDescriptorSetLayout(m_device, m_ssaoDescSetLayout, nullptr); m_ssaoDescSetLayout = VK_NULL_HANDLE; }
        if (m_ssaoDescPool)       { vkDestroyDescriptorPool(m_device, m_ssaoDescPool, nullptr); m_ssaoDescPool = VK_NULL_HANDLE; }
        if (m_ssaoBlurDescSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_ssaoBlurDescSetLayout, nullptr); m_ssaoBlurDescSetLayout = VK_NULL_HANDLE; }
        if (m_ssaoBlurDescPool)      { vkDestroyDescriptorPool(m_device, m_ssaoBlurDescPool, nullptr); m_ssaoBlurDescPool = VK_NULL_HANDLE; }
        if (m_ssaoKernelUboMem) vmaUnmapMemory(m_vmaAllocator, m_ssaoKernelUboMem);
        m_ssaoKernelUboMapped = nullptr;
        if (m_ssaoKernelUbo || m_ssaoKernelUboMem)
            { vmaDestroyBuffer(m_vmaAllocator, m_ssaoKernelUbo, m_ssaoKernelUboMem);
            m_ssaoKernelUbo = VK_NULL_HANDLE; m_ssaoKernelUboMem = VK_NULL_HANDLE; }
        if (m_ssaoNoiseView) { vkDestroyImageView(m_device, m_ssaoNoiseView, nullptr); m_ssaoNoiseView = VK_NULL_HANDLE; }
        if (m_ssaoNoiseImg || m_ssaoNoiseMem)
            { vmaDestroyImage(m_vmaAllocator, m_ssaoNoiseImg, m_ssaoNoiseMem);
            m_ssaoNoiseImg = VK_NULL_HANDLE; m_ssaoNoiseMem = VK_NULL_HANDLE; }
        if (m_ssaoAoRawView) { vkDestroyImageView(m_device, m_ssaoAoRawView, nullptr); m_ssaoAoRawView = VK_NULL_HANDLE; }
        if (m_ssaoAoRawImg || m_ssaoAoRawMem)
            { vmaDestroyImage(m_vmaAllocator, m_ssaoAoRawImg, m_ssaoAoRawMem);
            m_ssaoAoRawImg = VK_NULL_HANDLE; m_ssaoAoRawMem = VK_NULL_HANDLE; }
        if (m_ssaoAoBlurView) { vkDestroyImageView(m_device, m_ssaoAoBlurView, nullptr); m_ssaoAoBlurView = VK_NULL_HANDLE; }
        if (m_ssaoAoBlurImg || m_ssaoAoBlurMem)
            { vmaDestroyImage(m_vmaAllocator, m_ssaoAoBlurImg, m_ssaoAoBlurMem);
            m_ssaoAoBlurImg = VK_NULL_HANDLE; m_ssaoAoBlurMem = VK_NULL_HANDLE; }
        m_ssaoCreated = false;
        m_ssaoW = m_ssaoH = 0;
        m_ssaoAoRawLayout = m_ssaoAoBlurLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        m_sceneW = m_sceneH = 0;
        m_globalVBCapacity = 0; m_globalIBCapacity = 0; m_globalInstCapacity = 0;
        m_globalVBSize = 0;
        m_meshUploadInfos.clear();

        // Cluster culling resources (Phase 2 + Phase 3 LOD)
        if (m_clusterBoundsBuf || m_clusterBoundsMem) {
            vmaDestroyBuffer(m_vmaAllocator, m_clusterBoundsBuf, m_clusterBoundsMem);
            m_clusterBoundsBuf = VK_NULL_HANDLE; m_clusterBoundsMem = VK_NULL_HANDLE;
        }
        if (m_lodHierarchyBuf || m_lodHierarchyMem) {
            vmaDestroyBuffer(m_vmaAllocator, m_lodHierarchyBuf, m_lodHierarchyMem);
            m_lodHierarchyBuf = VK_NULL_HANDLE; m_lodHierarchyMem = VK_NULL_HANDLE;
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
        if (m_lodDescSet) {
            vkFreeDescriptorSets(m_device, m_clusterDescPool, 1, &m_lodDescSet);
            m_lodDescSet = VK_NULL_HANDLE;
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
    // Per-mesh, per-layer texture resources for the 4-layer terrain splat.
    // Parallel to MeshUploadInfo.layerTexId[] / OffscreenMesh::splatLayers.
    std::vector<std::array<MeshTexResources, 4>> m_meshLayerTex;

    void destroyMeshTex(MeshTexResources& r) {
        if (r.view) { vkDestroyImageView(m_device, r.view, nullptr); r.view = VK_NULL_HANDLE; }
        if (r.image || r.mem) { vmaDestroyImage(m_vmaAllocator, r.image, r.mem); r.image = VK_NULL_HANDLE; r.mem = VK_NULL_HANDLE; }
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
    // B2 SSAO (Vulkan deferred offscreen path) — twin of the GL path.
    // The kernel + 4x4 noise come from the shared RHI:: generators (same
    // bytes on GL & Vulkan); viewProj is rebuilt exactly like uploadSceneUBO
    // (mat4GlToVulkanProj + mat4LookAt) so world-pos reconstruction matches
    // the deferred geometry pass -> pixel-parity in the resulting AO factor.
    // =====================================================================
    VkImageLayout m_ssaoAoRawLayout   = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout m_ssaoAoBlurLayout  = VK_IMAGE_LAYOUT_UNDEFINED;

    void ensureSsaoResources(VkCommandBuffer cmd, int w, int h) {
        if (!m_ssaoCreated) {
            // --- 4x4 RG8 noise (host-visible LINEAR; sampled by the kernel) ---
            {
                VkImageCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8_UNORM;
                ici.extent = {4, 4, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
                ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_LINEAR;
                ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                vmaCreateImage(m_vmaAllocator, &ici, &aci,
                               &m_ssaoNoiseImg, &m_ssaoNoiseMem, nullptr);
                void* mp = nullptr;
                if (vmaMapMemory(m_vmaAllocator, m_ssaoNoiseMem, &mp) == VK_SUCCESS && mp) {
                    const auto noise = RHI::generateSsaoNoiseR8G8();
                    std::memcpy(mp, noise.data(), noise.size());
                    vmaFlushAllocation(m_vmaAllocator, m_ssaoNoiseMem, 0,
                                       VK_WHOLE_SIZE);
                    vmaUnmapMemory(m_vmaAllocator, m_ssaoNoiseMem);
                }
                // LINEAR host-tiled image: the prior agent left it in UNDEFINED
                // layout, so the SSAO compute shader's sampler read of uNoise
                // sampled an undefined image -> driver fault. Transition it to
                // SHADER_READ_ONLY so the kernel can safely sample the noise.
                transitionImageLayout(cmd, m_ssaoNoiseImg,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                VkImageViewCreateInfo vci{};
                vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vci.image = m_ssaoNoiseImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = VK_FORMAT_R8G8_UNORM;
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCreateImageView(m_device, &vci, nullptr, &m_ssaoNoiseView);
            }
            // --- Kernel UBO (std140 SsaoUBOData), persistently mapped ---
            createBuffer(sizeof(SsaoUBOData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_ssaoKernelUbo, m_ssaoKernelUboMem);
            if (m_ssaoKernelUboMem)
                vmaMapMemory(m_vmaAllocator, m_ssaoKernelUboMem, &m_ssaoKernelUboMapped);

            // --- SSAO descriptor set layout: b0/b1/b2 COMBINED (sampler2D),
            //      b3 UBO (SsaoUBO), b4 STORAGE_IMAGE (raw AO) ---
            {
                VkDescriptorSetLayoutBinding bl[5]{};
                bl[0].binding = 0; bl[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bl[0].descriptorCount = 1; bl[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bl[1].binding = 1; bl[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bl[1].descriptorCount = 1; bl[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bl[2].binding = 2; bl[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bl[2].descriptorCount = 1; bl[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bl[3].binding = 3; bl[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                bl[3].descriptorCount = 1; bl[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bl[4].binding = 4; bl[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bl[4].descriptorCount = 1; bl[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                VkDescriptorSetLayoutCreateInfo dlci{};
                dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                dlci.bindingCount = 5; dlci.pBindings = bl;
                vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_ssaoDescSetLayout);
                VkDescriptorPoolSize sps[3]{};
                sps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sps[0].descriptorCount = 3;
                sps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;       sps[1].descriptorCount = 1;
                sps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;        sps[2].descriptorCount = 1;
                VkDescriptorPoolCreateInfo dpci{};
                dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                dpci.maxSets = 1; dpci.poolSizeCount = 3; dpci.pPoolSizes = sps;
                vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_ssaoDescPool);
                VkDescriptorSetAllocateInfo dsai{};
                dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                dsai.descriptorPool = m_ssaoDescPool; dsai.descriptorSetCount = 1;
                dsai.pSetLayouts = &m_ssaoDescSetLayout;
                vkAllocateDescriptorSets(m_device, &dsai, &m_ssaoDescSet);
                VkPipelineLayoutCreateInfo plci{};
                plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                plci.setLayoutCount = 1; plci.pSetLayouts = &m_ssaoDescSetLayout;
                vkCreatePipelineLayout(m_device, &plci, nullptr, &m_ssaoPipelineLayout);

                // --- Blur set layout: b0 COMBINED (sampled raw AO), b1 STORAGE (blurred AO) ---
                VkDescriptorSetLayoutBinding bbl[2]{};
                bbl[0].binding = 0; bbl[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bbl[0].descriptorCount = 1; bbl[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bbl[1].binding = 1; bbl[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bbl[1].descriptorCount = 1; bbl[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                VkDescriptorSetLayoutCreateInfo bdli{};
                bdli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                bdli.bindingCount = 2; bdli.pBindings = bbl;
                vkCreateDescriptorSetLayout(m_device, &bdli, nullptr, &m_ssaoBlurDescSetLayout);
                VkDescriptorPoolSize bps[2]{};
                bps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bps[0].descriptorCount = 1;
                bps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;        bps[1].descriptorCount = 1;
                VkDescriptorPoolCreateInfo bdpci{};
                bdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                bdpci.maxSets = 1; bdpci.poolSizeCount = 2; bdpci.pPoolSizes = bps;
                vkCreateDescriptorPool(m_device, &bdpci, nullptr, &m_ssaoBlurDescPool);
                VkDescriptorSetAllocateInfo bdsai{};
                bdsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                bdsai.descriptorPool = m_ssaoBlurDescPool; bdsai.descriptorSetCount = 1;
                bdsai.pSetLayouts = &m_ssaoBlurDescSetLayout;
                vkAllocateDescriptorSets(m_device, &bdsai, &m_ssaoBlurDescSet);
                VkPipelineLayoutCreateInfo bplci{};
                bplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                bplci.setLayoutCount = 1; bplci.pSetLayouts = &m_ssaoBlurDescSetLayout;
                vkCreatePipelineLayout(m_device, &bplci, nullptr, &m_ssaoBlurPipelineLayout);

                // --- Compute pipelines (Makefile .comp -> build/rhi/*.comp.spv) ---
                auto loadCs = [&](const char* name, VkPipeline& out, VkPipelineLayout layout) {
                    auto spv = loadSpirv(name);
                    if (spv.empty()) {
                        std::cerr << "[RHI-Vk] SSAO: missing " << name << "\n"; return;
                    }
                    VkShaderModule sm = createShaderModule(m_device, spv);
                    VkComputePipelineCreateInfo cpci{};
                    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm;
                    cpci.stage.pName = "main"; cpci.layout = layout;
                    vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &out);
                    vkDestroyShaderModule(m_device, sm, nullptr);
                };
                loadCs("build/rhi/ssao.comp.spv",       m_ssaoPipeline,       m_ssaoPipelineLayout);
                loadCs("build/rhi/ssao_blur.comp.spv",  m_ssaoBlurPipeline,   m_ssaoBlurPipelineLayout);
            }
            m_ssaoCreated = true;
        }
        // --- AO images (recreate on size change; start UNDEFINED each time) ---
        if (m_ssaoW != static_cast<uint32_t>(w) || m_ssaoH != static_cast<uint32_t>(h)) {
            auto destroyR8 = [&](VkImage& img, VmaAllocation& mem, VkImageView& view) {
                if (view) { vkDestroyImageView(m_device, view, nullptr); view = VK_NULL_HANDLE; }
                if (img || mem) vmaDestroyImage(m_vmaAllocator, img, mem);
                img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
            };
            destroyR8(m_ssaoAoRawImg,  m_ssaoAoRawMem,  m_ssaoAoRawView);
            destroyR8(m_ssaoAoBlurImg, m_ssaoAoBlurMem, m_ssaoAoBlurView);
            // Create an R8_UNORM (OPTIMAL) image + view with the given usage.
            auto make = [&](VkImage& img, VmaAllocation& mem, VkImageView& view,
                            VkImageUsageFlags usage) {
                VkImageCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8_UNORM;
                ici.extent = {(uint32_t)w, (uint32_t)h, 1}; ici.mipLevels = 1;
                ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
                ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = usage;
                ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                vmaCreateImage(m_vmaAllocator, &ici, &aci, &img, &mem, nullptr);
                VkImageViewCreateInfo vci{};
                vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = VK_FORMAT_R8_UNORM;
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCreateImageView(m_device, &vci, nullptr, &view);
            };
            make(m_ssaoAoRawImg,  m_ssaoAoRawMem,  m_ssaoAoRawView,
                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            make(m_ssaoAoBlurImg, m_ssaoAoBlurMem, m_ssaoAoBlurView,
                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            m_ssaoAoRawLayout = m_ssaoAoBlurLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            m_ssaoW = w; m_ssaoH = h;
        }
    }

    void uploadSsaoUBO(VkCommandBuffer, const OffscreenScene& scene, int w, int h) {
        if (!m_ssaoKernelUboMapped) return;
        const Mat4 proj  = mat4Perspective(scene.camera.fovDeg, (float)w / (float)h,
                                           0.1f, scene.camera.farPlane);
        const Mat4 pvk = mat4GlToVulkanProj(proj);
        const Mat4 view = mat4LookAt(scene.camera.eye, scene.camera.target, scene.camera.up);
        SsaoUBOData d{};
        d.viewProj      = mat4Multiply(pvk, view);
        d.invViewProj   = mat4Inverse(d.viewProj);
        const auto kernel = RHI::generateSsaoSampleKernel(16, /*radius*/0.5f);
        for (int i = 0; i < 16; ++i) {
            d.samples[i][0] = kernel[i * 3 + 0];
            d.samples[i][1] = kernel[i * 3 + 1];
            d.samples[i][2] = kernel[i * 3 + 2];
            d.samples[i][3] = 0.0f;   // std140 vec3 pad (16-byte stride)
        }
        std::memcpy(m_ssaoKernelUboMapped, &d, sizeof(d));
    }

    // =====================================================================
    // B5 Bloom (Vulkan twin of rhi/RHIGL.cpp dispatchBloom + rhi/shaders/bloom_*.comp).
    // Guarded NO-OP when pipelines fail to build or scene.bloom is false, so the
    // 79/79 baseline (no deferred+bloom scene) is never disturbed.
    void ensureBloomResources(int w, int h) {
        if (m_bloomCreated) {
            if ((uint32_t)w != m_bloomW || (uint32_t)h != m_bloomH) { cleanupBloomResources(); m_bloomCreated = false; }
            else return;
        }
        auto loadCs = [&](const char* name, VkPipeline& out, VkPipelineLayout layout) -> bool {
            auto spv = loadSpirv(name);
            if (spv.empty()) { std::cerr << "[RHI-Vk] bloom: missing " << name << "\n"; return false; }
            VkShaderModule sm = createShaderModule(m_device, spv);
            VkComputePipelineCreateInfo cpci{};
            cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm; cpci.stage.pName = "main";
            cpci.layout = layout;
            bool ok = (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &out) == VK_SUCCESS);
            vkDestroyShaderModule(m_device, sm, nullptr);
            return ok;
        };
        auto mkDslPoolSet = [&](VkDescriptorSetLayout& dsl, VkDescriptorPool& pool,
                                VkDescriptorSet& ds, VkPipelineLayout& lay, int cis) {
            // b0..b{cis-1} : combined image/sampler ; b{cis} : UBO ; b{cis+1} : storage image
            std::vector<VkDescriptorSetLayoutBinding> bl;
            VkDescriptorSetLayoutBinding e{}; e.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            for (int i=0;i<cis;i++){ e.binding=(uint32_t)i; e.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; e.descriptorCount=1; bl.push_back(e); }
            e.binding=(uint32_t)cis; e.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; e.descriptorCount=1; bl.push_back(e);
            e.binding=(uint32_t)(cis+1); e.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; e.descriptorCount=1; bl.push_back(e);
            VkDescriptorSetLayoutCreateInfo dlci{}; dlci.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dlci.bindingCount=(uint32_t)bl.size(); dlci.pBindings=bl.data();
            VK_VERIFY(vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &dsl));
            std::vector<VkDescriptorPoolSize> sps(3);
            sps[0].type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sps[0].descriptorCount=(uint32_t)cis;
            sps[1].type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;        sps[1].descriptorCount=1;
            sps[2].type=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;        sps[2].descriptorCount=1;
            VkDescriptorPoolCreateInfo dpci{}; dpci.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpci.maxSets=1;
            dpci.poolSizeCount=(uint32_t)sps.size(); dpci.pPoolSizes=sps.data();
            VK_VERIFY(vkCreateDescriptorPool(m_device, &dpci, nullptr, &pool));
            VkDescriptorSetAllocateInfo dsai{}; dsai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool=pool; dsai.descriptorSetCount=1; dsai.pSetLayouts=&dsl;
            VK_VERIFY(vkAllocateDescriptorSets(m_device, &dsai, &ds));
            VkPipelineLayoutCreateInfo plci{}; plci.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; plci.setLayoutCount=1; plci.pSetLayouts=&dsl;
            VK_VERIFY(vkCreatePipelineLayout(m_device, &plci, nullptr, &lay));
        };
        mkDslPoolSet(m_bloomExtractDSL, m_bloomExtractPool, m_bloomExtractSet, m_bloomExtractLayout, 1);
        mkDslPoolSet(m_bloomBlurDSL,    m_bloomBlurPool,    m_bloomBlurSet,    m_bloomBlurLayout,    1);
        mkDslPoolSet(m_bloomCompositeDSL, m_bloomCompositePool, m_bloomCompositeSet, m_bloomCompositeLayout, 2);
        bool ok = loadCs("build/rhi/bloom_extract.comp.spv",   m_bloomExtractPipeline,   m_bloomExtractLayout);
        ok = loadCs("build/rhi/bloom_blur.comp.spv",          m_bloomBlurPipeline,      m_bloomBlurLayout) && ok;
        ok = loadCs("build/rhi/bloom_composite.comp.spv",      m_bloomCompositePipeline, m_bloomCompositeLayout) && ok;
        if (!ok) { std::cerr << "[RHI-Vk] bloom pipelines failed to build; bloom disabled\n"; cleanupBloomResources(); return; }
        createBuffer(sizeof(float)*2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_bloomUBO, m_bloomUBOMem);
        auto mkImg = [&](VkImage& img, VmaAllocation& mem, VkImageView& view) {
            VkImageCreateInfo ici{}; ici.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType=VK_IMAGE_TYPE_2D; ici.format=VK_FORMAT_R16G16B16A16_SFLOAT;
            ici.extent={(uint32_t)w,(uint32_t)h,1}; ici.mipLevels=1; ici.arrayLayers=1;
            ici.samples=VK_SAMPLE_COUNT_1_BIT; ici.tiling=VK_IMAGE_TILING_OPTIMAL;
            ici.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{}; aci.usage=VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(m_vmaAllocator, &ici, &aci, &img, &mem, nullptr);
            VkImageViewCreateInfo vci{}; vci.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vci.image=img;
            vci.viewType=VK_IMAGE_VIEW_TYPE_2D; vci.format=VK_FORMAT_R16G16B16A16_SFLOAT;
            vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VK_VERIFY(vkCreateImageView(m_device, &vci, nullptr, &view));
        };
        mkImg(m_bloomExtractImg, m_bloomExtractMem, m_bloomExtractView);
        mkImg(m_bloomBlurImg,    m_bloomBlurMem,    m_bloomBlurView);
        mkImg(m_bloomFinalImg,   m_bloomFinalMem,   m_bloomFinalView);
        m_bloomW=(uint32_t)w; m_bloomH=(uint32_t)h; m_bloomFinalLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        m_bloomCreated=true;
    }
    void cleanupBloomResources() {
        // Mirrors the GL backend's cleanupBloomResources(): tear down pipelines,
        // pipeline layouts, descriptor set layouts, pools, the UBO and the
        // extract/blur/final images. Guarded on m_bloomCreated for the pipeline
        // set teardown (recreate path); image/view teardown is unconditional to
        // avoid leaking on the pipeline-build-failure path (m_bloomCreated==false).
        if (m_bloomCreated) {
            if (m_bloomExtractPipeline)   { vkDestroyPipeline(m_device, m_bloomExtractPipeline, nullptr);   m_bloomExtractPipeline = VK_NULL_HANDLE; }
            if (m_bloomBlurPipeline)      { vkDestroyPipeline(m_device, m_bloomBlurPipeline, nullptr);      m_bloomBlurPipeline = VK_NULL_HANDLE; }
            if (m_bloomCompositePipeline) { vkDestroyPipeline(m_device, m_bloomCompositePipeline, nullptr); m_bloomCompositePipeline = VK_NULL_HANDLE; }
            vkDestroyPipelineLayout(m_device, m_bloomExtractLayout, nullptr);    m_bloomExtractLayout = VK_NULL_HANDLE;
            vkDestroyPipelineLayout(m_device, m_bloomBlurLayout, nullptr);       m_bloomBlurLayout = VK_NULL_HANDLE;
            vkDestroyPipelineLayout(m_device, m_bloomCompositeLayout, nullptr);  m_bloomCompositeLayout = VK_NULL_HANDLE;
            vkDestroyDescriptorSetLayout(m_device, m_bloomExtractDSL, nullptr);  m_bloomExtractDSL = VK_NULL_HANDLE;
            vkDestroyDescriptorSetLayout(m_device, m_bloomBlurDSL, nullptr);     m_bloomBlurDSL = VK_NULL_HANDLE;
            vkDestroyDescriptorSetLayout(m_device, m_bloomCompositeDSL, nullptr); m_bloomCompositeDSL = VK_NULL_HANDLE;
            vkDestroyDescriptorPool(m_device, m_bloomExtractPool, nullptr);      m_bloomExtractPool = VK_NULL_HANDLE;
            vkDestroyDescriptorPool(m_device, m_bloomBlurPool, nullptr);         m_bloomBlurPool = VK_NULL_HANDLE;
            vkDestroyDescriptorPool(m_device, m_bloomCompositePool, nullptr);    m_bloomCompositePool = VK_NULL_HANDLE;
            if (m_bloomUBO) { vmaDestroyBuffer(m_vmaAllocator, m_bloomUBO, m_bloomUBOMem); m_bloomUBO = VK_NULL_HANDLE; m_bloomUBOMem = VK_NULL_HANDLE; }
            m_bloomCreated = false;
        }
        if (m_bloomExtractView)  { vkDestroyImageView(m_device, m_bloomExtractView, nullptr); }
        if (m_bloomBlurView)     { vkDestroyImageView(m_device, m_bloomBlurView, nullptr); }
        if (m_bloomFinalView)    { vkDestroyImageView(m_device, m_bloomFinalView, nullptr); }
        if (m_bloomExtractImg  || m_bloomExtractMem)  vmaDestroyImage(m_vmaAllocator, m_bloomExtractImg,  m_bloomExtractMem);
        if (m_bloomBlurImg     || m_bloomBlurMem)     vmaDestroyImage(m_vmaAllocator, m_bloomBlurImg,     m_bloomBlurMem);
        if (m_bloomFinalImg    || m_bloomFinalMem)    vmaDestroyImage(m_vmaAllocator, m_bloomFinalImg,    m_bloomFinalMem);
        m_bloomExtractView = m_bloomBlurView = m_bloomFinalView = VK_NULL_HANDLE;
        m_bloomExtractImg  = m_bloomBlurImg  = m_bloomFinalImg  = VK_NULL_HANDLE;
        m_bloomExtractMem  = m_bloomBlurMem  = m_bloomFinalMem  = VK_NULL_HANDLE;
        m_bloomW = m_bloomH = 0;
    }
    void dispatchBloom(VkCommandBuffer cmd, VkImage sceneColorImg, VkImageView sceneColorView, int w, int h) {
        if (!m_bloomCreated || !m_bloomExtractPipeline || !m_bloomBlurPipeline || !m_bloomCompositePipeline) return;
        const uint32_t gx=(uint32_t)((w+15)/16), gy=(uint32_t)((h+15)/16);
        auto putUbo = [&](const float v[2]){
            void* mp=nullptr; if (m_bloomUBOMem && vmaMapMemory(m_vmaAllocator,m_bloomUBOMem,&mp)==VK_SUCCESS && mp) {
                std::memcpy(mp,v,sizeof(float)*2); vmaFlushAllocation(m_vmaAllocator,m_bloomUBOMem,0,VK_WHOLE_SIZE); vmaUnmapMemory(m_vmaAllocator,m_bloomUBOMem);
            }
        };
        auto barrier=[&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                         VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA,
                         VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA){
            VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.image=img; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.oldLayout=oldL; b.newLayout=newL; b.srcStageMask=srcS; b.srcAccessMask=srcA;
            b.dstStageMask=dstS; b.dstAccessMask=dstA;
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep.imageMemoryBarrierCount=1; dep.pImageMemoryBarriers=&b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        auto memSync=[&]{
            VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;
            mb.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT;
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep.memoryBarrierCount=1; dep.pMemoryBarriers=&mb;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        auto bind=[&](VkPipeline pl,VkPipelineLayout lay,VkDescriptorSet ds){
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, nullptr);
        };
        auto bindUbo=[&](VkDescriptorSet ds,uint32_t binding){
            VkDescriptorBufferInfo bu{}; bu.buffer=m_bloomUBO; bu.offset=0; bu.range=sizeof(float)*2;
            VkWriteDescriptorSet w{}; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=ds; w.dstBinding=binding;
            w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo=&bu;
            vkUpdateDescriptorSets(m_device,1,&w,0,nullptr);
        };
        VkDescriptorImageInfo sceneI{}; sceneI.imageView=sceneColorView; sceneI.sampler=m_lightingSampler;
        sceneI.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo extrW{}; extrW.imageView=m_bloomExtractView; extrW.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo blurW{}; blurW.imageView=m_bloomBlurView; blurW.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo rawR{}; rawR.imageView=m_bloomExtractView; rawR.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo blurR{}; blurR.imageView=m_bloomBlurView; blurR.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo finalW{}; finalW.imageView=m_bloomFinalView; finalW.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        // sceneColor: COLOR_ATTACHMENT -> SHADER_READ_ONLY
        barrier(sceneColorImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        // 1) extract: scene(color) -> bloomExtract(GENERAL)
        barrier(m_bloomExtractImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        { VkWriteDescriptorSet s0{},s2{}; s0.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s0.dstSet=m_bloomExtractSet;
          s0.dstBinding=0; s0.descriptorCount=1; s0.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s0.pImageInfo=&sceneI;
          s2.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s2.dstSet=m_bloomExtractSet; s2.dstBinding=2;
          s2.descriptorCount=1; s2.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; s2.pImageInfo=&extrW;
          bindUbo(m_bloomExtractSet,1); VkWriteDescriptorSet es[]={s0,s2}; vkUpdateDescriptorSets(m_device,2,es,0,nullptr); }
        bind(m_bloomExtractPipeline,m_bloomExtractLayout,m_bloomExtractSet);
        vkCmdDispatch(cmd,gx,gy,1); memSync();
        barrier(m_bloomExtractImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        // 2) blur H: extract -> blur(GENERAL), dir=(1,0)
        { const float d[2]={1.0f,0.0f}; putUbo(d); }
        barrier(m_bloomBlurImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        { VkWriteDescriptorSet s0{},s2{}; s0.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s0.dstSet=m_bloomBlurSet;
          s0.dstBinding=0; s0.descriptorCount=1; s0.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s0.pImageInfo=&rawR;
          s2.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s2.dstSet=m_bloomBlurSet; s2.dstBinding=2;
          s2.descriptorCount=1; s2.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; s2.pImageInfo=&blurW;
          bindUbo(m_bloomBlurSet,1); VkWriteDescriptorSet hs[]={s0,s2}; vkUpdateDescriptorSets(m_device,2,hs,0,nullptr); }
        bind(m_bloomBlurPipeline,m_bloomBlurLayout,m_bloomBlurSet);
        vkCmdDispatch(cmd,gx,gy,1); memSync();
        barrier(m_bloomBlurImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        // 3) blur V: blur -> extract(ping-pong, GENERAL), dir=(0,1)
        { const float d[2]={0.0f,1.0f}; putUbo(d); }
        barrier(m_bloomExtractImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        rawR.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        blurR.imageView=m_bloomExtractView; blurR.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        { VkWriteDescriptorSet s0{},s2{}; s0.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s0.dstSet=m_bloomBlurSet;
          s0.dstBinding=0; s0.descriptorCount=1; s0.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s0.pImageInfo=&blurR;
          s2.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s2.dstSet=m_bloomBlurSet; s2.dstBinding=2;
          s2.descriptorCount=1; s2.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; s2.pImageInfo=&rawR;
          bindUbo(m_bloomBlurSet,1); VkWriteDescriptorSet hs[]={s0,s2}; vkUpdateDescriptorSets(m_device,2,hs,0,nullptr); }
        bind(m_bloomBlurPipeline,m_bloomBlurLayout,m_bloomBlurSet);
        vkCmdDispatch(cmd,gx,gy,1); memSync();
        barrier(m_bloomExtractImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        // 4) composite: scene + blurred-bloom -> bloomFinal(GENERAL)
        barrier(m_bloomFinalImg, m_bloomFinalLayout, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        { VkWriteDescriptorSet s0{},s1{},s2{}; s0.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s0.dstSet=m_bloomCompositeSet;
          s0.dstBinding=0; s0.descriptorCount=1; s0.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s0.pImageInfo=&sceneI;
          s1.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s1.dstSet=m_bloomCompositeSet; s1.dstBinding=1;
          s1.descriptorCount=1; s1.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s1.pImageInfo=&blurR;
          s2.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; s2.dstSet=m_bloomCompositeSet; s2.dstBinding=2;
          s2.descriptorCount=1; s2.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; s2.pImageInfo=&finalW;
          bindUbo(m_bloomCompositeSet,2); VkWriteDescriptorSet cs[]={s0,s1,s2}; vkUpdateDescriptorSets(m_device,3,cs,0,nullptr); }
        bind(m_bloomCompositePipeline,m_bloomCompositeLayout,m_bloomCompositeSet);
        vkCmdDispatch(cmd,gx,gy,1); memSync();
        m_bloomFinalLayout=VK_IMAGE_LAYOUT_GENERAL;
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
            // 4-layer terrain splat: create+register each layer texture into the
            // bindless array. layerTexId[] is filled later in the per-mesh loop
            // below (m_meshUploadInfos isn't resized until then). This mesh's
            // texturePixels is the RGBA control map registered above as texId.
            if (mesh.splatLayers.size() == 4) {
                while (m_meshLayerTex.size() <= i) m_meshLayerTex.emplace_back();
                for (int k = 0; k < 4; ++k) {
                    const auto& layer = mesh.splatLayers[k];
                    if (layer.textureWidth > 0 && layer.textureHeight > 0
                        && !layer.texturePixels.empty()) {
                        if (m_meshLayerTex[i][k].w != layer.textureWidth
                            || m_meshLayerTex[i][k].h != layer.textureHeight
                            || m_meshLayerTex[i][k].image == VK_NULL_HANDLE) {
                            destroyMeshTex(m_meshLayerTex[i][k]);
                            OffscreenMesh lm;
                            lm.texturePixels = layer.texturePixels;
                            lm.textureWidth = layer.textureWidth;
                            lm.textureHeight = layer.textureHeight;
                            createMeshTexture(lm, m_meshLayerTex[i][k]);
                        }
                    }
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
            // Splat layer bindless ids (terrain). Registered in the texture loop
            // above; m_meshLayerTex[i][k].textureId is the bindless slot.
            if (mesh.splatLayers.size() == 4) {
                for (int k = 0; k < 4; ++k)
                    m_meshUploadInfos[i].layerTexId[k] = (int)m_meshLayerTex[i][k].textureId;
            } else {
                for (int k = 0; k < 4; ++k) m_meshUploadInfos[i].layerTexId[k] = -1;
            }

            // Skinning: create/reuse per-mesh skin VB (boneIdx/Weight @32B/vert)
            // + joint-palette buffers, allocate the per-mesh set=1, upload bone
            // data, and record skinning metadata for drawMeshes(). Skin data is
            // per-mesh (local vertex indices), so it is independent of the
            // triple-buffered global vertex ring — gl_VertexIndex - firstVertex
            // (pushed) indexes this mesh's skin VB.
            if (mesh.skinned && mesh.jointCount > 0) {
                while (m_skinMeshRes.size() <= i) m_skinMeshRes.emplace_back();
                SkinMeshRes& res = m_skinMeshRes[i];
                const uint32_t vCount  = (uint32_t)(mesh.vertices.size() / 8);   // 8 floats/vert
                const uint32_t jCount  = (uint32_t)mesh.jointCount;
                const VkDeviceSize skinBytes = (VkDeviceSize)vCount * 32;         // uvec4+vec4
                const VkDeviceSize boneBytes = (VkDeviceSize)jCount * 64;         // mat4
                const bool needAlloc = res.skinVbCapacity < vCount || res.paletteCapacity < jCount;
                if (needAlloc) {
                    if (res.skinVb)  { vmaDestroyBuffer(m_vmaAllocator, res.skinVb,  res.skinVbMem);  res.skinVb = VK_NULL_HANDLE; res.skinVbMem = VK_NULL_HANDLE; }
                    if (res.boneBuf) { vmaDestroyBuffer(m_vmaAllocator, res.boneBuf, res.boneBufMem); res.boneBuf = VK_NULL_HANDLE; res.boneBufMem = VK_NULL_HANDLE; }
                    res.skinVbCapacity  = vCount;
                    res.paletteCapacity = jCount;
                    createBuffer(skinBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 res.skinVb, res.skinVbMem);
                    createBuffer(boneBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 res.boneBuf, res.boneBufMem);
                    if (res.descSet == VK_NULL_HANDLE) {
                        VkDescriptorSetAllocateInfo dsai{};
                        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                        dsai.descriptorPool = m_skinDescPool; dsai.descriptorSetCount = 1;
                        dsai.pSetLayouts = &m_skinDescSetLayout;
                        vkAllocateDescriptorSets(m_device, &dsai, &res.descSet);
                    }
                    VkDescriptorBufferInfo sbi{res.skinVb, 0, skinBytes};
                    VkDescriptorBufferInfo bbi{res.boneBuf, 0, boneBytes};
                    VkWriteDescriptorSet w0{};
                    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w0.dstSet = res.descSet;
                    w0.dstBinding = 0; w0.descriptorCount = 1;
                    w0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w0.pBufferInfo = &sbi;
                    VkWriteDescriptorSet w1{};
                    w1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w1.dstSet = res.descSet;
                    w1.dstBinding = 1; w1.descriptorCount = 1;
                    w1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w1.pBufferInfo = &bbi;
                    VkWriteDescriptorSet ws[2] = { w0, w1 };
                    vkUpdateDescriptorSets(m_device, 2, ws, 0, nullptr);
                }
                // Re-upload bone data every frame (palettes animate; indices/weights static).
                if (res.skinVbMem && mesh.boneIndices.size() >= (size_t)vCount * 4
                                    && mesh.boneWeights.size() >= (size_t)vCount * 4) {
                    void* p; vmaMapMemory(m_vmaAllocator, res.skinVbMem, &p);
                    for (uint32_t vi = 0; vi < vCount; ++vi) {
                        std::memcpy((char*)p + (size_t)vi * 32,            &mesh.boneIndices[vi * 4], 16);
                        std::memcpy((char*)p + (size_t)vi * 32 + 16,       &mesh.boneWeights[vi * 4], 16);
                    }
                    vmaUnmapMemory(m_vmaAllocator, res.skinVbMem);
                }
                if (res.boneBufMem && !mesh.jointPalette.empty()) {
                    void* p; vmaMapMemory(m_vmaAllocator, res.boneBufMem, &p);
                    std::memcpy(p, mesh.jointPalette.data(), boneBytes);
                    vmaUnmapMemory(m_vmaAllocator, res.boneBufMem);
                }
                m_meshUploadInfos[i].skinDescSet = res.descSet;
                m_meshUploadInfos[i].boneOffset  = 0;   // per-mesh palette starts at 0
                m_meshUploadInfos[i].boneCount   = jCount;
            }
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

        // --- Virtualized Geometry: upload cluster data (Phase 2 + Phase 3) ---
        // Aggregate all clusters across meshes into contiguous GPU buffers.
        size_t totalClusters = 0;
        size_t numMeshesWithClusters = 0;
        for (const auto& mesh : scene.meshes) {
            totalClusters += mesh.clusters.size();
            if (!mesh.clusters.empty()) ++numMeshesWithClusters;
        }

        if (totalClusters == 0) return;  // no clustered meshes — legacy path

        // Phase 3 LOD hierarchy: one root node per clustered mesh + one leaf
        // node per cluster (interleaved: [root, leaf0..leafN, root, ...]).
        // totalNodes is the dispatch extent for nanite_lod.comp and caps the
        // indirect-counter / indirect-draw buffer for the LOD path.
        size_t totalNodes = totalClusters + numMeshesWithClusters;

        // Lazily create/recreate cluster buffers. The Phase 3 LOD hierarchy buffer
        // is allocated alongside the Phase 2 bound/command buffers.
        if (!m_clusterBoundsBuf || !m_clusterCmdBuf || !m_indirectDrawBuf
            || !m_clusterCounterBuf || !m_lodHierarchyBuf) {
            VkDeviceSize boundsSize = totalClusters * sizeof(VkClusterBounds);  // Phase 2 leaf bounds
            VkDeviceSize cmdSize    = totalClusters * sizeof(VkClusterCommand);   // Phase 2 leaf cmd
            VkDeviceSize indirSize  = totalNodes  * sizeof(VkDrawIndexedIndirectCommand); // Phase 3 max
            VkDeviceSize counterSize = sizeof(uint32_t);
            VkDeviceSize lodSize    = totalNodes  * sizeof(VkLODCluster);  // Phase 3 root+leaf hierarchy

            createBuffer(boundsSize > 0 ? boundsSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_clusterBoundsBuf, m_clusterBoundsMem);
            createBuffer(cmdSize > 0 ? cmdSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_clusterCmdBuf, m_clusterCmdMem);
            // Indirect draw buffer: GPU-written by cluster_cull.comp (outDraws)
            // and GPU-read by vkCmdDrawIndexedIndirectCount — never host-touched,
            // so bind DEVICE_LOCAL to ride the Vega 10 L2 cache (APU memory
            // matrix: no staging copy across the shared DDR4 bus).
            createBuffer(indirSize > 0 ? indirSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         m_indirectDrawBuf, m_indirectDrawMem);
            createBuffer(counterSize,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_clusterCounterBuf, m_clusterCounterMem);
            vmaMapMemory(m_vmaAllocator, m_clusterCounterMem, &m_clusterCounterMapped);
            // Phase 3 LOD hierarchy (read by nanite_lod.comp binding 1).
            createBuffer(lodSize > 0 ? lodSize : 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_lodHierarchyBuf, m_lodHierarchyMem);
        }

        // Upload all cluster bounds, commands, and Phase 3 LOD records
        std::vector<VkClusterBounds>  allBounds;
        std::vector<VkClusterCommand> allCmds;
        std::vector<VkLODCluster>     allLODs;
        allBounds.reserve(totalClusters);
        allCmds.reserve(totalClusters);
        allLODs.reserve(totalNodes);

        uint32_t nodeIdx = 0;     // index into the interleaved root+leaf node buffers
        uint32_t clusterIdx = 0;  // index into the leaf-only Phase 2 buffers
        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            const auto& mesh = scene.meshes[i];
            const auto& mi   = m_meshUploadInfos[i];
            const uint32_t meshClusterCount = (uint32_t)mesh.clusters.size();
            m_meshUploadInfos[i].firstCluster = clusterIdx;
            m_meshUploadInfos[i].clusterCount = meshClusterCount;
            if (meshClusterCount == 0) continue;  // legacy (non-clustered) mesh: no hierarchy

            // --- Phase 3 root node (coarsest level): draws the WHOLE mesh ---
            // Bounding sphere = smallest enclosing sphere of this mesh's cluster
            // spheres (centroid = mean of cluster centers, radius = cover).
            glm::vec3 rootCenter(0.0f);
            float rootRadius = 0.0f;
            for (const auto& cr : mesh.clusters) {
                rootCenter += glm::vec3(cr.center[0], cr.center[1], cr.center[2]);
            }
            rootCenter /= (float)meshClusterCount;
            for (const auto& cr : mesh.clusters) {
                float d = glm::distance(rootCenter, glm::vec3(cr.center[0], cr.center[1], cr.center[2]));
                rootRadius = glm::max(rootRadius, d + cr.radius);
            }
            rootRadius = glm::max(rootRadius, 1e-4f);  // guard divide-by-zero / NaN

            VkLODCluster rootNode{};
            rootNode.sphereCenterRadius = glm::vec4(rootCenter, rootRadius);
            rootNode.errorSelf   = rootRadius;   // coarse level: whole-mesh error ~= bounds radius
            rootNode.errorParent = 0.0f;        // root has no parent
            rootNode.parentId    = 0;           // root sentinel
            rootNode.firstVertex  = mi.firstVertex;
            rootNode.firstIndex   = mi.firstIndex;
            rootNode.indexCount   = mi.indexCount;
            rootNode.instanceId   = mi.firstInstance;
            allLODs.push_back(rootNode);
            const uint32_t rootNodeIdx = nodeIdx;
            ++nodeIdx;

            // --- Phase 3 leaf nodes (one per cluster; children of the root) ---
            // errorSelf = 0 (exact leaf geometry) => always renderable at near
            // range; errorParent = rootRadius => the selector swaps to the root
            // (single whole-mesh draw) once the mesh is sub-pixel.
            for (const auto& cr : mesh.clusters) {
                VkClusterBounds b;
                b.sphereCenterRadius = glm::vec4(cr.center[0], cr.center[1], cr.center[2], cr.radius);
                b.minBounds = glm::vec4(0, 0, 0, 1);  // conservative; could refine from vertices
                b.maxBounds = glm::vec4(0, 0, 0, 1);

                VkClusterCommand cmd;
                // Store ABSOLUTE offsets into global buffers (vertexOffset and
                // firstIndex are relative to the zero-bound index/vertex buffers).
                cmd.firstVertex = mi.firstVertex + cr.firstVertex;
                cmd.firstIndex  = mi.firstIndex  + cr.firstIndex;
                cmd.indexCount  = cr.indexCount;
                cmd.instanceId  = mi.firstInstance;  // all clusters share mesh's instance data

                VkLODCluster lod{};
                lod.sphereCenterRadius = b.sphereCenterRadius;
                lod.errorSelf   = 0.0f;       // fine leaf: no simplification error
                lod.errorParent = rootRadius; // = parent root's errorSelf (coarser)
                lod.parentId    = rootNodeIdx;
                lod.firstVertex  = cmd.firstVertex;
                lod.firstIndex   = cmd.firstIndex;
                lod.indexCount   = cmd.indexCount;
                lod.instanceId   = cmd.instanceId;
                allLODs.push_back(lod);

                // Phase 2 leaves live in the cull set's leaf-only buffers.
                allBounds.push_back(b);
                allCmds.push_back(cmd);
                ++clusterIdx;
                ++nodeIdx;
            }
        }

        // Node counts for the render phase. Phase 2 dispatches over leaves
        // (m_totalClusterCount); Phase 3 dispatches over root+leaves (m_totalNodeCount).
        m_totalClusterCount = (uint32_t)totalClusters;
        m_totalNodeCount    = (uint32_t)totalNodes;

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
        if (!allLODs.empty()) {
            void* lodPtr = nullptr;
            vmaMapMemory(m_vmaAllocator, m_lodHierarchyMem, &lodPtr);
            std::memcpy(lodPtr, allLODs.data(), allLODs.size() * sizeof(VkLODCluster));
            vmaUnmapMemory(m_vmaAllocator, m_lodHierarchyMem);
        }

        // Allocate / write the cluster descriptor set (set 0 for the compute pipeline)
        if (m_clusterDescPool == VK_NULL_HANDLE) {
            // Two sets: Phase 2 (bounds b1) + Phase 3 (LOD b1). Same buffers are
            // reused for b0/b2/b3/b4 across both sets; only binding 1 differs.
            VkDescriptorPoolSize poolSizes[2]{};
            poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;    poolSizes[0].descriptorCount = 2; // b0 x2
            poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   poolSizes[1].descriptorCount = 8; // b1..b4 x2 sets
            VkDescriptorPoolCreateInfo dpci{};
            dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpci.maxSets = 2; dpci.poolSizeCount = 2; dpci.pPoolSizes = poolSizes;
            vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_clusterDescPool);
        }
        if (m_clusterDescSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = m_clusterDescPool;
            dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_clusterDescLayout;
            vkAllocateDescriptorSets(m_device, &dsai, &m_clusterDescSet);
        }
        if (m_lodDescSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = m_clusterDescPool;
            dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_clusterDescLayout;
            vkAllocateDescriptorSets(m_device, &dsai, &m_lodDescSet);
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
        indirInfo.range = totalNodes * sizeof(VkDrawIndexedIndirectCommand);
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

        // Phase 3 descriptor set: identical to set 0 but binding 1 points at
        // the LOD hierarchy buffer (root+leaf nodes) instead of the bounds
        // buffer. b2 (leaf cmds), b3 (indirect, totalNodes) and b4 (counter)
        // are shared with Phase 2.
        VkDescriptorBufferInfo lodInfo{};
        lodInfo.buffer = m_lodHierarchyBuf; lodInfo.offset = 0;
        lodInfo.range = totalNodes * sizeof(VkLODCluster);
        for (int w = 0; w < 5; ++w) { writes[w].dstSet = m_lodDescSet; }
        writes[1].pBufferInfo = &lodInfo;   // b1 = LOD hierarchy (not bounds)
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
                    VkPipeline pipeline,
                    VkDescriptorSet shadowSet = VK_NULL_HANDLE,
                    VkPipeline shadowPipeline = VK_NULL_HANDLE,
                    VkPipeline shadowSkinnedPipeline = VK_NULL_HANDLE,
                    VkPipeline shadowTerrainPipeline = VK_NULL_HANDLE) {
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

            // Route 4-layer splat (terrain) meshes to the terrain pipeline; the
            // rest stays on pbr_mesh. Both share m_sceneLayout + the bindless
            // descriptor set — only the pipeline + PC payload differ.
            // Skinned meshes (4-bone LBS) route to pbr_skinned.vert, which needs
            // the per-mesh skin set (set=1) bound in addition to the bindless
            // set=0. Terrain splat and the generic pbr path are unchanged.
            const bool isSkinned = mesh.skinned && m_pbrSkinnedPipeline != VK_NULL_HANDLE
                                   && info.skinDescSet != VK_NULL_HANDLE;
            const bool isSplat = !isSkinned
                                 && mesh.splatLayers.size() == 4
                                 && m_pbrTerrainPipeline != VK_NULL_HANDLE
                                 && info.layerTexId[0] >= 0;
            // B1-forward shadow: only the forward swapchain path supplies a
            // shadow set + pipeline (default scene.shadows=false -> defaults null
            // -> this whole block is skipped, preserving parity/60/60). Static
            // meshes route to the CSM PBR pipeline (shadow on set=1); skinned
            // meshes route to the skinned CSM pipeline — skin data stays on
            // set=1, the shadow atlas moves to set=2 (pbr_skinned_shadow.frag).
            const bool shadowActive = (shadowSet != VK_NULL_HANDLE) && (shadowPipeline != VK_NULL_HANDLE);
            const bool useShadowForward = shadowActive
                                          && (isSkinned ? (shadowSkinnedPipeline != VK_NULL_HANDLE) : true)
                                          && (isSplat ? (shadowTerrainPipeline != VK_NULL_HANDLE) : true);
            const bool skinnedShadow = useShadowForward && isSkinned;
            const bool staticShadow = useShadowForward && !isSkinned && !isSplat;
            const bool splatShadow  = useShadowForward && isSplat;
            const VkPipeline meshPipeline = skinnedShadow ? shadowSkinnedPipeline
                                         : staticShadow ? shadowPipeline
                                         : splatShadow ? shadowTerrainPipeline
                                         : isSkinned ? m_pbrSkinnedPipeline
                                         : isSplat ? m_pbrTerrainPipeline : pipeline;
            const VkPipelineLayout pl = skinnedShadow ? m_shadowSkinnedPipelineLayout
                          : (staticShadow || splatShadow) ? m_shadowPbrPipelineLayout
                          : isSkinned ? m_skinPipelineLayout : m_sceneLayout;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                    0, 1, &m_bindlessDescSet, 0, nullptr);
            if (skinnedShadow) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                        1, 1, &info.skinDescSet, 0, nullptr);   // set1 = skin
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                        2, 1, &shadowSet, 0, nullptr);          // set2 = shadow atlas
            } else if (staticShadow || splatShadow) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                        1, 1, &shadowSet, 0, nullptr);          // set1 = shadow atlas
            } else if (isSkinned) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                        1, 1, &info.skinDescSet, 0, nullptr);   // set1 = skin
            }
            if (isSkinned) {
                // textureId@0 (frag), boneOffset@4 (mat4 idx), firstVertex@8 (vert)
                struct { uint32_t textureId; uint32_t boneOffset; uint32_t firstVertex; uint32_t pad; } sp{
                    (uint32_t)info.textureId, (uint32_t)info.boneOffset,
                    (uint32_t)info.firstVertex, 0u };
                vkCmdPushConstants(cmd, pl,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(sp), &sp);
            } else if (isSplat) {
                struct { uint32_t control, layer0, layer1, layer2, layer3; } tp{
                    (uint32_t)info.textureId,
                    (uint32_t)info.layerTexId[0], (uint32_t)info.layerTexId[1],
                    (uint32_t)info.layerTexId[2], (uint32_t)info.layerTexId[3] };
                vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(tp), &tp);
            } else if (staticShadow) {
                uint32_t texId = info.textureId;
                vkCmdPushConstants(cmd, m_shadowPbrPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(uint32_t), &texId);
            } else {
                uint32_t texId = info.textureId;
                vkCmdPushConstants(cmd, m_sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(uint32_t), &texId);
            }

            vkCmdBindIndexBuffer(cmd, m_globalIB,
                                 info.firstIndex * sizeof(uint32_t),
                                 VK_INDEX_TYPE_UINT32);

            vkCmdDrawIndexed(cmd, info.indexCount, info.instanceCount,
                             0, info.firstVertex, info.firstInstance);
        }
    }

    // =====================================================================
    // DRAW CLUSTERS — GPU-driven frustum culling + Nanite LOD (Phase 2 + Phase 3)
    // =====================================================================
    void drawMeshesClustered(VkCommandBuffer cmd, const OffscreenScene& scene) {
        if (!m_clusterCullInitialized || m_totalClusterCount == 0)
            return;

        // 1. Reset draw-count counter to 0
        if (m_clusterCounterMapped) {
            *(uint32_t*)m_clusterCounterMapped = 0;
        }

        // 2. Bind compute pipeline + cluster descriptor set.
        //    Phase 3 (nanite_lod.comp) is the active Nanite LOD path: it reads
        //    the 64-byte LODCluster hierarchy (b1) + ClusterCommand (b2) and
        //    emits SSE-gated indirect draws. Fall back to Phase 2 frustum
        //    culling if the LOD pipeline/buffer failed to initialize.
        VkPipeline      computePipeline = m_naniteLodPipeline;
        VkDescriptorSet clusterDescSet  = m_lodDescSet;
        if (!computePipeline || !m_lodHierarchyBuf) {
            computePipeline = m_clusterCullPipeline;
            clusterDescSet  = m_clusterDescSet;
        }
        if (!computePipeline || !clusterDescSet) return;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 m_clusterCullLayout, 0, 1, &clusterDescSet, 0, nullptr);

        // 3. Push constants: totalNodes, viewportW, viewportH, lodPixelError.
        //    Phase 3 dispatches over root+leaf nodes; Phase 2 over leaf clusters.
        struct PushData { uint32_t total; float vw, vh, err; } pd{};
        pd.total = (computePipeline == m_naniteLodPipeline) ? m_totalNodeCount : m_totalClusterCount;
        // Match the DRS-contracted viewport (m_activeLowW/H) when active so the
        // SSE LOD gate uses the same pixel dims the scene was drawn at; fall back
        // to the swapchain scene dims on the offscreen (no-DRS) path.
        pd.vw = (float)(m_activeLowW ? m_activeLowW : (uint32_t)m_sceneW);
        pd.vh = (float)(m_activeLowH ? m_activeLowH : (uint32_t)m_sceneH);
        pd.err = 1.0f;  // 1-pixel target error (raise to cull more aggressively)
        vkCmdPushConstants(cmd, m_clusterCullLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PushData), &pd);

        // 4. Dispatch compute shader (64 threads/workgroup) — one per node
        //    (root+leaf for Phase 3, leaves for Phase 2).
        uint32_t groupCount = (pd.total + 63) / 64;
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
            pd.total);        // maxDrawCount (dispatched nodes; GPU counter caps actual)
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

    // ─── Phase-B B4 skybox (samplerCube — cannot live in the 2D bindless array) ───
    uint32_t createCubemapTexture(int w, int h, const uint8_t* faces[6]) override {
        if (!m_device || !m_vmaAllocator || w <= 0 || h <= 0) return 0;
        for (int f = 0; f < 6; ++f) if (!faces[f]) return 0;
#ifndef VK_IMAGE_CREATE_CUBE_BIT
#define VK_IMAGE_CREATE_CUBE_BIT 0x10
#endif

        Cubemap cm;
        VkSamplerCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.anisotropyEnable = VK_FALSE; sci.mipLodBias = 0.0f; sci.minLod = 0.0f; sci.maxLod = 0.0f;
        if (vkCreateSampler(m_device, &sci, nullptr, &cm.sampler) != VK_SUCCESS) return 0;

        VkImageCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)w, (uint32_t)h, 1}; ici.mipLevels = 1; ici.arrayLayers = 6;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ici.flags = VK_IMAGE_CREATE_CUBE_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(m_vmaAllocator, &ici, &aci, &cm.image, &cm.alloc, nullptr) != VK_SUCCESS) {
            vkDestroySampler(m_device, cm.sampler, nullptr); return 0;
        }
        VkImageViewCreateInfo vci{}; vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = cm.image; vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vkCreateImageView(m_device, &vci, nullptr, &cm.view);

        // Staging: 6 faces packed row-major (face i at offset i*w*h*4)
        VkBuffer staging; VmaAllocation stagingMem;
        createBuffer((VkDeviceSize)6 * w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging, stagingMem);
        void* p; vmaMapMemory(m_vmaAllocator, stagingMem, &p);
        for (int f = 0; f < 6; ++f)
            std::memcpy((char*)p + (size_t)f * w * h * 4, faces[f], (size_t)w * h * 4);
        vmaUnmapMemory(m_vmaAllocator, stagingMem);

        VkCommandPool      pool; VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = m_queueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(m_device, &pci, nullptr, &pool) != VK_SUCCESS) {
            vmaDestroyBuffer(m_vmaAllocator, staging, stagingMem); return 0;
        }
        VkCommandBuffer cmd; VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &ai, &cmd);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.image = cm.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE; b.srcAccessMask = 0;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        for (int f = 0; f < 6; ++f) {
            VkBufferImageCopy r{}; r.bufferOffset = (VkDeviceSize)f * w * h * 4;
            r.bufferRowLength = w; r.bufferImageHeight = h;
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.imageSubresource.baseArrayLayer = f; r.imageSubresource.layerCount = 1;
            r.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, staging, cm.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }

        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(m_queue);
        vkFreeCommandBuffers(m_device, pool, 1, &cmd);
        vkDestroyCommandPool(m_device, pool, nullptr);
        vmaDestroyBuffer(m_vmaAllocator, staging, stagingMem);

        m_cubemaps.push_back(cm);
        return (uint32_t)m_cubemaps.size();   // 1-indexed (0 == null)
    }

    void destroyCubemapTexture(uint32_t handle) override {
        if (handle == 0 || handle > m_cubemaps.size()) return;
        Cubemap& cm = m_cubemaps[handle - 1];
        if (cm.view) { vkDestroyImageView(m_device, cm.view, nullptr); cm.view = VK_NULL_HANDLE; }
        if (cm.sampler) { vkDestroySampler(m_device, cm.sampler, nullptr); cm.sampler = VK_NULL_HANDLE; }
        if (cm.image || cm.alloc) { vmaDestroyImage(m_vmaAllocator, cm.image, cm.alloc);
                                   cm.image = VK_NULL_HANDLE; cm.alloc = VK_NULL_HANDLE; }
    }

    void ensureSkyboxResources() {
        if (m_skyboxPipeline) return;
        if (!m_device || !m_vmaAllocator || !m_sceneUbo) return;   // camera UBO must exist

        auto vsSpv = loadSpirv("build/rhi/skybox.vert.spv");
        auto fsSpv = loadSpirv("build/rhi/skybox.frag.spv");
        if (vsSpv.empty() || fsSpv.empty()) return;
        VkShaderModule vs = createShaderModule(m_device, vsSpv);
        VkShaderModule fs = createShaderModule(m_device, fsSpv);
        if (!vs || !fs) return;

        // Skybox cube VB (36 pos3 = 432 bytes); bound explicitly, NOT via the global SSBO.
        createBuffer(sizeof(kCubeVerts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_skyboxVbo, m_skyboxVboMem);
        void* pv; vmaMapMemory(m_vmaAllocator, m_skyboxVboMem, &pv);
        std::memcpy(pv, kCubeVerts, sizeof(kCubeVerts));
        vmaUnmapMemory(m_vmaAllocator, m_skyboxVboMem);

        VkPipelineVertexInputStateCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription   vbd{};
        vbd.binding = 0; vbd.stride = 12; vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription vad{};
        vad.location = 0; vad.binding = 0; vad.format = VK_FORMAT_R32G32B32_SFLOAT; vad.offset = 0;
        vci.vertexBindingDescriptionCount = 1; vci.pVertexBindingDescriptions = &vbd;
        vci.vertexAttributeDescriptionCount = 1; vci.pVertexAttributeDescriptions = &vad;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkViewport   dummyVp{0, 0, 1, 1, 0, 1};
        VkRect2D     dummySc{{0, 0}, {1, 1}};
        vp.viewportCount = 1; vp.pViewports = &dummyVp; vp.scissorCount = 1; vp.pScissors = &dummySc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_BACK_BIT;         // twin of GL skybox (BACK cull) for GL<->VK parity
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;   // z=w -> far plane, no write
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo pdyn{};
        pdyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        pdyn.dynamicStateCount = 2; pdyn.pDynamicStates = dyn;

        // set=0: binding0=CameraUBO (mat4 viewProj), binding1=samplerCube (cubemap)
        VkDescriptorSetLayoutBinding sb[2]{};
        sb[0].binding = 0; sb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sb[0].descriptorCount = 1; sb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        sb[1].binding = 1; sb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb[1].descriptorCount = 64; sb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{};
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = 2; slci.pBindings = sb;
        vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_skyboxDescSetLayout);

        VkDescriptorPoolSize spd[2]{};
        spd[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;           spd[0].descriptorCount = 1;
        spd[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  spd[1].descriptorCount = 64;
        VkDescriptorPoolCreateInfo spci{};
        spci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        spci.maxSets = 1; spci.poolSizeCount = 2; spci.pPoolSizes = spd;
        vkCreateDescriptorPool(m_device, &spci, nullptr, &m_skyboxDescPool);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = m_skyboxDescPool; dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_skyboxDescSetLayout;
        vkAllocateDescriptorSets(m_device, &dsai, &m_skyboxDescSet);
        VkDescriptorBufferInfo cbi{}; cbi.buffer = m_sceneUbo; cbi.offset = 0; cbi.range = sizeof(CameraUBOData);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_skyboxDescSet; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &cbi;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);

        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1; pl.pSetLayouts = &m_skyboxDescSetLayout;
        vkCreatePipelineLayout(m_device, &pl, nullptr, &m_skyboxLayout);

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount = 2; pci.pStages = stages; pci.pVertexInputState = &vci;
        pci.pInputAssemblyState = &ia; pci.pViewportState = &vp; pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cb;
        pci.pDynamicState = &pdyn; pci.layout = m_skyboxLayout;
        VkPipelineRenderingCreateInfo prci{};
        prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        const VkFormat kColorFmt = VK_FORMAT_R8G8B8A8_UNORM;
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &kColorFmt;
        prci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        pci.pNext = &prci; pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_skyboxPipeline);

        vkDestroyShaderModule(m_device, vs, nullptr);
        vkDestroyShaderModule(m_device, fs, nullptr);
    }

    // Draw the skybox into the current (forward) rendering pass before opaque
    // geometry so depth-LESS-or-equal z=w=1.0 lets cubes/occluders win.
    void drawSkyboxVk(VkCommandBuffer cmd, const OffscreenScene& scene) {
        if (!scene.skyEnabled || scene.skyboxCubemap == 0 ||
            scene.skyboxCubemap > static_cast<int>(m_cubemaps.size())) return;
        ensureSkyboxResources();
        const Cubemap& cm = m_cubemaps[scene.skyboxCubemap - 1];
        if (!m_skyboxPipeline || !m_skyboxDescSet || !cm.view || !cm.sampler) return;

        VkDescriptorImageInfo di{};
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        di.imageView = cm.view; di.sampler = cm.sampler;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_skyboxDescSet; w.dstBinding = 1; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxLayout,
                                0, 1, &m_skyboxDescSet, 0, nullptr);
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_skyboxVbo, &vbOffset);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }

    // PBR mesh pipeline (pbr_mesh.vert / pbr_mesh.frag)
    bool createPBRMeshPipeline(VkPipelineLayout layout, int w, int h,
                                VkSampleCountFlagBits samples,
                                VkPipeline& out, VkFormat colorFmt,
                                VkFormat depthFmt,
                            VkFormat velocityFmt = VK_FORMAT_R32G32_SFLOAT,
                            const char* fragSpvPath = "build/rhi/pbr_mesh.frag.spv",
                            const char* vertSpvPath = "build/rhi/pbr_mesh.vert.spv") {
        auto vsSpv = loadSpirv(vertSpvPath);
        auto fsSpv = loadSpirv(fragSpvPath);
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

    // Terrain 4-layer splat pipeline: pbr_mesh.vert (reused) + pbr_terrain.frag
    // (4-layer blend by control map + blended-height POM + Cook-Torrance PBR).
    bool createPBRTerrainPipeline(VkPipelineLayout layout, int w, int h,
                                  VkSampleCountFlagBits samples,
                                  VkPipeline& out, VkFormat colorFmt,
                                  VkFormat depthFmt,
                                  VkFormat velocityFmt = VK_FORMAT_UNDEFINED) {
        return createPBRMeshPipeline(layout, w, h, samples, out, colorFmt,
                                     depthFmt, velocityFmt,
                                     "build/rhi/pbr_terrain.frag.spv");
    }

    // Skinned (LBS) pipeline: pbr_skinned.vert (+ bone SSBO in set=1) drives the
    // vertex stage; pbr_mesh.frag is REUSED for the lit composite so the
    // skinned and unskinned paths share identical Cook-Torrance lighting.
    bool createPBRSkinnedPipeline(VkPipelineLayout layout, int w, int h,
                                  VkSampleCountFlagBits samples,
                                  VkPipeline& out, VkFormat colorFmt,
                                  VkFormat depthFmt) {
        return createPBRMeshPipeline(layout, w, h, samples, out, colorFmt,
                                     depthFmt,
                                     VK_FORMAT_R32G32_SFLOAT, // velocity attachment (matches forward rpass)
                                     "build/rhi/pbr_mesh.frag.spv",   // shared lit composite
                                     "build/rhi/pbr_skinned.vert.spv");
    }


    // =====================================================================
    // DEFERRED LIGHTING PIPELINE (offscreen test path)
    // =====================================================================
    // Fullscreen-triangle vertex stage + lighting.frag that samples the
    // =====================================================================
    // B1: Cascaded Shadow Maps (deferred CSM). OFF by default (scene.shadows
    // defaults false) so the forward parity path — and the 60/60 GL<->Vulkan
    // parity tests — are untouched. The offline deferred path binds a WHITE
    // (1.0) shadow map + identity ShadowUBO, so the depth compare returns
    // "fully lit" — pixel-identical to the pre-CSM lighting. Only
    // renderFrameSceneScene's deferred branch (when scene.shadows) renders the
    // atlas with shadow_depth.vert/frag and samples it from lighting.frag.
    // =====================================================================
    static constexpr int kShadowCascades = 3;
    static constexpr int kShadowSize = 2048;
    struct ShadowUBO {
        glm::mat4 lightSpaceMatrices[3]; // column-major, 16 floats each
        glm::vec4 cascadeSplits;         // x=near, y=mid, z=far (normalized cam range)
        glm::vec4 sunDirection;          // world-space sun dir (xyz)
    };
    VkSampler m_shadowSampler = VK_NULL_HANDLE;       // B1: compare LEQUAL, clamp-border
    VkBuffer m_shadowUbo = VK_NULL_HANDLE;            // shared (shadow draw + lighting set)
    VmaAllocation m_shadowUboMem = VK_NULL_HANDLE;
    void* m_shadowUboMapped = nullptr;
    VkImage m_whiteShadowImage = VK_NULL_HANDLE;      // 1x1x3 D32F, cleared to 1.0
    VmaAllocation m_whiteShadowAlloc = VK_NULL_HANDLE;
    VkImageView m_whiteShadowView = VK_NULL_HANDLE;   // 2D_ARRAY, 3 layers
    VkDescriptorSetLayout m_shadowDescSetLayout = VK_NULL_HANDLE; // set=1: ShadowUBO@0 (vert)
    VkDescriptorPool m_shadowDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_shadowDescSet = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;     // set0 bindless + set1 shadow + push
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;                 // shadow_depth.vert/frag
    VkPipelineLayout m_shadowPbrPipelineLayout = VK_NULL_HANDLE;  // pbr_mesh_shadow (set0+set1)
    VkPipeline m_shadowPbrPipeline = VK_NULL_HANDLE;              // forward CSM PBR (low-res)
    VkPipelineLayout m_shadowSkinnedPipelineLayout = VK_NULL_HANDLE; // pbr_skinned_shadow (set0+skin+set2 shadow)
    VkPipeline m_shadowSkinnedPipeline = VK_NULL_HANDLE;          // forward CSM skinned (full-res)
    VkPipeline m_shadowTerrainPipeline = VK_NULL_HANDLE;          // forward CSM splat (full-res, reuses m_shadowPbrPipelineLayout)
    VkImage m_shadowDepthImage = VK_NULL_HANDLE;                  // 2048^2 x 3 D32F atlas
    VmaAllocation m_shadowDepthAlloc = VK_NULL_HANDLE;
    VkImageView m_shadowDepthView = VK_NULL_HANDLE;               // 2D_ARRAY (sampled by lighting.frag)
    VkImageView m_shadowDepthView0 = VK_NULL_HANDLE;              // 2D view of layer 0 (render target)

    // Shared shadow resources (white fallback + UBO + set), needed by every
    // deferred lighting pass so lighting.frag's shadow/depth/camera bindings
    // are always valid. Lazy; created once the lighting pipeline exists.
    void ensureSharedShadowResources() {
        if (m_shadowUbo) return;
        if (!m_device || !m_vmaAllocator) return;

        // White (1.0) 1x1x3 D32F depth array for the offline parity path — the
        // depth compare against 1.0 always passes => fully lit.
        VkImageCreateInfo wci{};
        wci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        wci.imageType = VK_IMAGE_TYPE_2D;
        wci.format = VK_FORMAT_D32_SFLOAT;
        wci.extent = { 1, 1, 1 };
        wci.mipLevels = 1; wci.arrayLayers = kShadowCascades;
        wci.samples = VK_SAMPLE_COUNT_1_BIT;
        wci.tiling = VK_IMAGE_TILING_OPTIMAL;
        wci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        wci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo waci{}; waci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(m_vmaAllocator, &wci, &waci,
                           &m_whiteShadowImage, &m_whiteShadowAlloc, nullptr) != VK_SUCCESS) return;
        VkImageViewCreateInfo wvci{};
        wvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        wvci.image = m_whiteShadowImage; wvci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        wvci.format = VK_FORMAT_D32_SFLOAT;
        wvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
        vkCreateImageView(m_device, &wvci, nullptr, &m_whiteShadowView);
        // Clear to far (1.0) so unoccluded comparisons resolve to 1.0 = no shadow.
        if (m_cmdPool && m_queue != VK_NULL_HANDLE) {
            VkCommandBuffer cmd;
            VkCommandBufferAllocateInfo cbai{};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
            vkAllocateCommandBuffers(m_device, &cbai, &cmd);
            VkCommandBufferBeginInfo cbi{}; cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbi);
            // Clear the white shadow (depth=1.0 => compare always passes =>
            // fully lit) via a depth-only dynamic-rendering pass over all 3
            // layers; loadOp=CLEAR promotes UNDEFINED->DEPTH_ATTACHMENT_OPTIMAL.
            VkRenderingAttachmentInfo dAtt{};
            dAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            dAtt.imageView = m_whiteShadowView;
            dAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            dAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            dAtt.clearValue.depthStencil = { 1.0f, 0 };
            VkRenderingInfo wri{};
            wri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            wri.renderArea = {{0, 0}, {1, 1}};
            wri.layerCount = kShadowCascades; wri.colorAttachmentCount = 0; wri.pDepthAttachment = &dAtt;
            vkCmdBeginRendering(cmd, &wri);
            vkCmdEndRendering(cmd);
            VkImageMemoryBarrier2 wb{}; wb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            wb.image = m_whiteShadowImage;
            wb.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
            wb.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; wb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wb.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            wb.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            wb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; wb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkDependencyInfo wdi{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; wdi.imageMemoryBarrierCount = 1; wdi.pImageMemoryBarriers = &wb;
            vkCmdPipelineBarrier2(cmd, &wdi);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(m_queue);
            vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
        }

        // Compare-sampler for the shadow depth array (LEQUAL, border=1.0).
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.compareEnable = VK_TRUE; sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        vkCreateSampler(m_device, &sci, nullptr, &m_shadowSampler);

        // Shadow UBO (persistently mapped; identity default = fully lit).
        createBuffer(sizeof(ShadowUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, m_shadowUbo, m_shadowUboMem);
        if (m_shadowUbo) vmaMapMemory(m_vmaAllocator, m_shadowUboMem, &m_shadowUboMapped);
        ShadowUBO white{};
        for (int c = 0; c < kShadowCascades; ++c) white.lightSpaceMatrices[c] = glm::mat4(1.0f);
        white.cascadeSplits = { 0.05f, 0.2f, 0.6f, 0.0f };
        white.sunDirection  = { 0.5f, 1.0f, 0.3f, 0.0f };
        if (m_shadowUboMapped) std::memcpy(m_shadowUboMapped, &white, sizeof(white));

        // Shadow set (set=1): ShadowUBO binding@0 (vertex+fragment — shared by
        // shadow_depth.vert and the forward CSM pbr_mesh_shadow.frag) + the CSM
        // depth atlas combined-image-sampler@1 (sampled in the forward PBR pass).
        VkDescriptorSetLayoutBinding slb[2]{};
        slb[0].binding = 0; slb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        slb[0].descriptorCount = 1; slb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        slb[1].binding = 1; slb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        slb[1].descriptorCount = 1; slb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo sdlci{};
        sdlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sdlci.bindingCount = 2; sdlci.pBindings = slb;
        vkCreateDescriptorSetLayout(m_device, &sdlci, nullptr, &m_shadowDescSetLayout);
        VkDescriptorPoolSize sps[2]{};
        sps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sps[0].descriptorCount = 1;
        sps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sps[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo spci{};
        spci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        spci.maxSets = 1; spci.poolSizeCount = 2; spci.pPoolSizes = sps;
        vkCreateDescriptorPool(m_device, &spci, nullptr, &m_shadowDescPool);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = m_shadowDescPool; dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_shadowDescSetLayout;
        vkAllocateDescriptorSets(m_device, &dsai, &m_shadowDescSet);
        VkDescriptorBufferInfo sbi{};
        sbi.buffer = m_shadowUbo; sbi.offset = 0; sbi.range = sizeof(ShadowUBO);
        VkDescriptorImageInfo shi{};
        shi.imageView = m_whiteShadowView; shi.sampler = m_shadowSampler;
        shi.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet sw[2]{};
        sw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sw[0].dstSet = m_shadowDescSet; sw[0].dstBinding = 0; sw[0].descriptorCount = 1;
        sw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[0].pBufferInfo = &sbi;
        sw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sw[1].dstSet = m_shadowDescSet; sw[1].dstBinding = 1; sw[1].descriptorCount = 1;
        sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &shi;
        vkUpdateDescriptorSets(m_device, 2, sw, 0, nullptr);
        std::cout << "[RHI-Vk] B1 shared shadow resources (white atlas + UBO) created\n";
    }

    // Shadow atlas (2048^2 x3 D32F) + depth-only shadow pipeline. Render-only
    // resources — only created when scene.shadows is actually requested.
    void ensureShadowResources() {
        if (m_shadowPipeline) return;
        if (!m_device || !m_vmaAllocator) return;

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_D32_SFLOAT;
        ici.extent = { kShadowSize, kShadowSize, 1 };
        ici.mipLevels = 1; ici.arrayLayers = kShadowCascades;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(m_vmaAllocator, &ici, &aci,
                           &m_shadowDepthImage, &m_shadowDepthAlloc, nullptr) != VK_SUCCESS) return;
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = m_shadowDepthImage; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = VK_FORMAT_D32_SFLOAT;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
        vkCreateImageView(m_device, &vci, nullptr, &m_shadowDepthView);
        VkImageViewCreateInfo vci0 = vci;
        vci0.viewType = VK_IMAGE_VIEW_TYPE_2D; vci0.subresourceRange.layerCount = 1; vci0.subresourceRange.baseArrayLayer = 0;
        vkCreateImageView(m_device, &vci0, nullptr, &m_shadowDepthView0);

        VkPipelineLayoutCreateInfo spl{};
        spl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkDescriptorSetLayout ssets[2] = { m_bindlessDescLayout, m_shadowDescSetLayout };
        spl.setLayoutCount = 2; spl.pSetLayouts = ssets;
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.offset = 0; pcr.size = sizeof(uint32_t);
        spl.pushConstantRangeCount = 1; spl.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &spl, nullptr, &m_shadowPipelineLayout);

        auto vs = loadSpirv("build/rhi/shadow_depth.vert.spv");
        auto fs = loadSpirv("build/rhi/shadow_depth.frag.spv");
        if (vs.empty() || fs.empty()) {
            std::cout << "[RHI-Vk] B1 shadow shaders not glslc-compiled yet (build/rhi/shadow_depth.*.spv)\n"; return;
        }
        VkShaderModule vsm = createShaderModule(m_device, vs);
        VkShaderModule fsm = createShaderModule(m_device, fs);
        if (!vsm || !fsm) return;
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, (float)kShadowSize, (float)kShadowSize, 0.0f, 1.0f};
        VkRect2D vr{{0, 0}, {(int)kShadowSize, (int)kShadowSize}};
        VkPipelineViewportStateCreateInfo vpci{}; vpci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vpci.viewportCount = 1; vpci.pViewports = &vp; vpci.scissorCount = 1; vpci.pScissors = &vr;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
        ds.minDepthBounds = 0.0f; ds.maxDepthBounds = 1.0f;
        VkPipelineRenderingCreateInfo prci{}; prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO; // NO color attachments (depth-only)
        VkGraphicsPipelineCreateInfo pci{}; pci.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stageCount = 2; pci.pStages = st;
        pci.pInputAssemblyState = &ia; pci.pViewportState = &vpci;
        pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
        pci.layout = m_shadowPipelineLayout; pci.pNext = &prci; pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_shadowPipeline);
        vkDestroyShaderModule(m_device, vsm, nullptr); vkDestroyShaderModule(m_device, fsm, nullptr);
        std::cout << "[RHI-Vk] B1 CSM shadow pipeline created (2048x" << kShadowSize << "x" << kShadowCascades << ")\n";

        // B1-forward: forward PBR + CSM. pbr_mesh_shadow.frag is pbr_mesh.frag
        // verbatim + a cascade-0 shadow factor (multiply Lo); it reads vWorldPos
        // (provided by pbr_mesh.vert@11) so NO depth-reconstruction is needed.
        // set0 = bindless(sceneLayout), set1 = {ShadowUBO@0 + atlas CIS@1},
        // push = texId@FRAG. Dynamic viewport -> renders at whatever framebuffer
        // (the low-res swapchain pass) the caller has set.
        {
            VkDescriptorSetLayout psl[2] = { m_bindlessDescLayout, m_shadowDescSetLayout };
            VkPushConstantRange pcr{};
            pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = 32; // 4 (texId) .. 20 (terrain PC)
            VkPipelineLayoutCreateInfo spl{};
            spl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            spl.setLayoutCount = 2; spl.pSetLayouts = psl;
            spl.pushConstantRangeCount = 1; spl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_device, &spl, nullptr, &m_shadowPbrPipelineLayout) != VK_SUCCESS)
                m_shadowPbrPipelineLayout = VK_NULL_HANDLE;
        }
        if (m_shadowPbrPipelineLayout) {
            auto pvs = loadSpirv("build/rhi/pbr_mesh.vert.spv");
            auto pfs = loadSpirv("build/rhi/pbr_mesh_shadow.frag.spv");
            if (!pvs.empty() && !pfs.empty()) {
                VkShaderModule pvsm = createShaderModule(m_device, pvs);
                VkShaderModule pf_sm = createShaderModule(m_device, pfs);
                if (pvsm && pf_sm) {
                    VkPipelineShaderStageCreateInfo pst[2]{};
                    pst[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    pst[0].stage = VK_SHADER_STAGE_VERTEX_BIT; pst[0].module = pvsm; pst[0].pName = "main";
                    pst[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    pst[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; pst[1].module = pf_sm; pst[1].pName = "main";
                    VkPipelineInputAssemblyStateCreateInfo ia{};
                    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    VkPipelineViewportStateCreateInfo vpci{};
                    vpci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                    vpci.viewportCount = 0; vpci.pViewports = nullptr; // dynamic viewport
                    vpci.scissorCount = 0; vpci.pScissors = nullptr;
                    VkPipelineRasterizationStateCreateInfo rs{};
                    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
                    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
                    VkPipelineMultisampleStateCreateInfo ms{};
                    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                    VkPipelineDepthStencilStateCreateInfo ds{};
                    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
                    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                    VkPipelineColorBlendAttachmentState cba[2]{};
                    cba[0].colorWriteMask = 0xF; cba[0].blendEnable = VK_FALSE;
                    cba[1].colorWriteMask = 0xF; cba[1].blendEnable = VK_FALSE;
                    VkPipelineColorBlendStateCreateInfo cb{};
                    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                    cb.attachmentCount = 2; cb.pAttachments = cba;
                    VkFormat pcf[3] = { VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_D32_SFLOAT };
                    VkPipelineRenderingCreateInfo prci{};
                    prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                    prci.colorAttachmentCount = 2; prci.pColorAttachmentFormats = &pcf[0];
                    prci.depthAttachmentFormat = pcf[2];
                    VkGraphicsPipelineCreateInfo pci{};
                    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                    pci.stageCount = 2; pci.pStages = pst;
                    pci.pVertexInputState = nullptr; pci.pInputAssemblyState = &ia;
                    pci.pViewportState = &vpci; pci.pRasterizationState = &rs;
                    pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
                    pci.pColorBlendState = &cb; pci.layout = m_shadowPbrPipelineLayout;
                    pci.pNext = &prci;
                    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                                  &m_shadowPbrPipeline) == VK_SUCCESS)
                        std::cout << "[RHI-Vk] B1-forward shadow-PBR pipeline created\n";
                }
                vkDestroyShaderModule(m_device, pvsm, nullptr);
                vkDestroyShaderModule(m_device, pf_sm, nullptr);
            }
        }

        // B1-forward: terrain-splat CSM pipeline (pbr_terrain_shadow.frag =
        // pbr_terrain.frag + cascade-0 PCF). Reuses pbr_mesh.vert (vWorldPos@11)
        // + m_shadowPbrPipelineLayout (set0 bindless + set1 shadow; 32B push
        // covers the 20-byte terrain PC). Forward-safe (default shadows off).
        if (m_shadowPbrPipelineLayout)
            createPBRMeshPipeline(m_shadowPbrPipelineLayout, m_sceneW, m_sceneH,
                                  VK_SAMPLE_COUNT_1_BIT, m_shadowTerrainPipeline,
                                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                  VK_FORMAT_R32G32_SFLOAT,
                                  "build/rhi/pbr_terrain_shadow.frag.spv",
                                  "build/rhi/pbr_mesh.vert.spv");
    }

    // B1-forward: render the sun's CSM depth atlas (cascade 0) into the
    // 3-layer 2048x2048 image, then transition the WHOLE subresource range to
    // SHADER_READ_ONLY and rebind the atlas view into set=1/binding 1. Forwards
    // sample the atlas with vWorldPos (no depth-reconstruct). Shared by the
    // offline swapchain shadow step when scene.shadows is requested.
    void renderShadowCascade(VkCommandBuffer cmd, const OffscreenScene& scene) {
        if (!scene.shadows || !m_shadowDepthView0 || !m_shadowDescSet || !m_shadowUbo) return;
        ensureSharedShadowResources();
        ensureShadowResources();
        uploadShadowUBO(scene);
        {   VkImageMemoryBarrier2 asb{}; asb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            asb.image = m_shadowDepthImage;
            asb.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
            asb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            asb.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            asb.srcStageMask = VK_PIPELINE_STAGE_2_NONE; asb.srcAccessMask = 0;
            asb.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            asb.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo asdi{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            asdi.imageMemoryBarrierCount = 1; asdi.pImageMemoryBarriers = &asb;
            vkCmdPipelineBarrier2(cmd, &asdi);
        }
        VkRenderingAttachmentInfo dAtt{};
        dAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dAtt.imageView = m_shadowDepthView0;
        dAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        dAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        dAtt.clearValue.depthStencil = { 1.0f, 0 };
        VkRenderingInfo sri{};
        sri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        sri.renderArea = {{0, 0}, {(uint32_t)kShadowSize, (uint32_t)kShadowSize}};
        sri.layerCount = 1; sri.colorAttachmentCount = 0; sri.pDepthAttachment = &dAtt;
        vkCmdBeginRendering(cmd, &sri);
        VkViewport svp{0, 0, (float)kShadowSize, (float)kShadowSize, 0.0f, 1.0f};
        VkRect2D ssc{{0, 0}, {kShadowSize, kShadowSize}};
        vkCmdSetViewport(cmd, 0, 1, &svp);
        vkCmdSetScissor(cmd, 0, 1, &ssc);
        drawMeshesShadow(cmd, scene, 0);
        vkCmdEndRendering(cmd);
        VkImageMemoryBarrier2 b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.image = m_shadowDepthImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades };
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        // Rebind the now-readable atlas into set=1/binding 1 (overwrites the
        // white fallback when shadows are active; white otherwise).
        VkDescriptorImageInfo shi{};
        shi.imageView = m_shadowDepthView; shi.sampler = m_shadowSampler;
        shi.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet sw2{};
        sw2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sw2.dstSet = m_shadowDescSet; sw2.dstBinding = 1; sw2.descriptorCount = 1;
        sw2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw2.pImageInfo = &shi;
        vkUpdateDescriptorSets(m_device, 1, &sw2, 0, nullptr);
    }

    // Render scene meshes into shadow atlas layer 0 (cascade 0) — depth-only.
    // Mirrors drawMeshes but forces m_shadowPipeline + the shadow UBO set=1.
    void drawMeshesShadow(VkCommandBuffer cmd, const OffscreenScene& scene, uint32_t cascadeIdx) {
        if (scene.meshes.empty() || !m_shadowPipeline) return;
        if (!m_globalVB || !m_globalIB || !m_shadowDepthView0) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
        VkDescriptorSet sets[2] = { m_bindlessDescSet, m_shadowDescSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout,
                                0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(uint32_t), &cascadeIdx);
        for (size_t i = 0; i < scene.meshes.size() && i < m_meshUploadInfos.size(); ++i) {
            const auto& mesh = scene.meshes[i];
            const auto& info = m_meshUploadInfos[i];
            if (mesh.vertices.empty() || mesh.indices.empty() || info.instanceCount == 0) continue;
            if (!mesh.clusters.empty()) continue;  // clustered (Nanite) meshes not shadowed yet
            vkCmdBindIndexBuffer(cmd, m_globalIB,
                                 info.firstIndex * sizeof(uint32_t), VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, info.indexCount, info.instanceCount,
                             0, info.firstVertex, info.firstInstance);
        }
    }

    // Copy scene.lightSpaceMatrices[] + sunDirection + cascadeSplits into the
    // (mapped) ShadowUBO. toOffscreenScene populates these from the editor's
    // ShadowMapper; cascade 0 is what cascade 0 render + lighting sample use.
    void uploadShadowUBO(const OffscreenScene& scene) {
        if (!m_shadowUboMapped) return;
        ShadowUBO data{};
        for (int c = 0; c < kShadowCascades; ++c)
            for (int k = 0; k < 16; ++k)
                ((float*)&data.lightSpaceMatrices[c])[k] = scene.lightSpaceMatrices[c][k];
        data.cascadeSplits = { scene.cascadeSplits[0], scene.cascadeSplits[1],
                              scene.cascadeSplits[2], 0.0f };
        data.sunDirection  = { scene.sunDirection[0], scene.sunDirection[1],
                              scene.sunDirection[2], 0.0f };
        std::memcpy(m_shadowUboMapped, &data, sizeof(data));
    }

    // G-Buffer (albedo + normal/roughness) and composites a directional light.
    // No vertex buffers (position/UV derived from gl_VertexIndex), no depth
    // test — the fullscreen triangle covers every pixel once.
    void createLightingPipeline() {
        if (m_lightingPipeline) return;

        // Linear, clamp-to-edge sampler for G-Buffer sampling (matches the
        // EASU low-res sampler convention @1000).
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f; sci.maxLod = 0.0f;
        vkCreateSampler(m_device, &sci, nullptr, &m_lightingSampler);

        // set=0: bindings match the rewritten lighting.frag (B1 CSM added).
        // PARTIALLY_BOUND so the offline parity path can leave the shadow/depth
        // bindings on their WHITE fallback without a validation error — the
        // depth compare then returns 1.0 (fully lit), pixel-identical to the
        // pre-CSM lighting. This ALSO fixes a latent type mismatch (binding 0
        // was declared UNIFORM_BUFFER while the shader declared sampler2D).
        VkDescriptorSetLayoutBinding lb[7]{};
        lb[0].binding = 0; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // albedo
        lb[0].descriptorCount = 1; lb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[1].binding = 1; lb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // normalRough
        lb[1].descriptorCount = 1; lb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[2].binding = 2; lb[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // shadow (2DArrayShadow)
        lb[2].descriptorCount = 1; lb[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[3].binding = 3; lb[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;        // CameraUBO
        lb[3].descriptorCount = 1; lb[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[4].binding = 4; lb[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // depth
        lb[4].descriptorCount = 1; lb[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[5].binding = 5; lb[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         // ShadowUBO
        lb[5].descriptorCount = 1; lb[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[6].binding = 6; lb[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // B2 SSAO: uAo (white fallback)
        lb[6].descriptorCount = 1; lb[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorBindingFlags lbf[7] = {};
        for (auto& f : lbf) f = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo lbci{};
        lbci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        lbci.bindingCount = 7; lbci.pBindingFlags = lbf;
        VkDescriptorSetLayoutCreateInfo ldlci{};
        ldlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ldlci.pNext = &lbci;
        ldlci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        ldlci.bindingCount = 7; ldlci.pBindings = lb;
        vkCreateDescriptorSetLayout(m_device, &ldlci, nullptr, &m_lightingDescSetLayout);

        VkDescriptorPoolSize ps[2]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = 5;  // +1 for B2 uAo
        ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         ps[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
        vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_lightingDescPool);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = m_lightingDescPool; dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_lightingDescSetLayout;
        vkAllocateDescriptorSets(m_device, &dsai, &m_lightingDescSet);

        VkPipelineLayoutCreateInfo lpl{};
        lpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lpl.setLayoutCount = 1; lpl.pSetLayouts = &m_lightingDescSetLayout;
        vkCreatePipelineLayout(m_device, &lpl, nullptr, &m_lightingPipelineLayout);

        auto vs = loadSpirv("build/rhi/lighting.vert.spv");
        auto fs = loadSpirv("build/rhi/lighting.frag.spv");
        if (vs.empty() || fs.empty()) return;
        VkShaderModule vsm = createShaderModule(m_device, vs);
        VkShaderModule fsm = createShaderModule(m_device, fs);
        if (!vsm || !fsm) return;

        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport vp{0, 0, (float)m_sceneW, (float)m_sceneH, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)m_sceneW, (uint32_t)m_sceneH}};
        VkPipelineViewportStateCreateInfo vpci{};
        vpci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vpci.viewportCount = 1; vpci.pViewports = &vp;
        vpci.scissorCount = 1; vpci.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;           // fullscreen tri — don't cull
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkPipelineRenderingCreateInfo prci{};
        prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        const VkFormat kLitFmt = VK_FORMAT_R8G8B8A8_UNORM;  // matches offscreen color target
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &kLitFmt;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount = 2; pci.pStages = st;
        pci.pVertexInputState = nullptr;       // SSBO/gl_VertexIndex — no vertex attrs
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vpci;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pColorBlendState = &cb;
        pci.layout = m_lightingPipelineLayout;
        pci.pNext = &prci; pci.renderPass = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_lightingPipeline);
        std::cout << "[RHI-Vk] Deferred lighting pipeline created\n";

        vkDestroyShaderModule(m_device, vsm, nullptr);
        vkDestroyShaderModule(m_device, fsm, nullptr);
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
            ici.extent = {static_cast<uint32_t>(m_swapWidth), static_cast<uint32_t>(m_swapHeight), 1};
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
            dci.extent = {static_cast<uint32_t>(m_swapWidth), static_cast<uint32_t>(m_swapHeight), 1};
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
    // Phase 5c: previous frame's Halton sub-pixel jitter (NDC), fed to the
    // temporal shader so it can undo the jitter that was baked into history.
    RHI::Vec2 m_prevJitter{};
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

    // ── Dynamic Resolution Scaling (DRS) ──
    VkQueryPool m_queryPool = VK_NULL_HANDLE;     // 2 timestamps/frame slot
    float       m_drsScaleFactor = 1.0f;          // 1.0 = native, drops toward 0.5 under GPU pressure
    uint32_t    m_activeLowW = 0;                 // active DRS low-res width  (0 = inactive)
    uint32_t    m_activeLowH = 0;                 // active DRS low-res height
    float       m_gpuTimeMs = 0.0f;               // previous frame's scene-pass GPU time (ms)
    float       m_targetFrameTimeMs = 14.0f;      // drop DRS past this; recover below 75% of it
    float       m_drsMinScale = 0.5f;             // clamp low: 50% of low-res max
    float       m_drsMaxScale = 1.0f;             // clamp high: 100% (native low-res)
    bool        m_drsEnabled  = true;             // master toggle

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
    VkPipeline m_pbrTerrainPipeline = VK_NULL_HANDLE;   // 4-layer splat (terrain)

    // --- Deferred G-Buffer (geometry -> lighting) — offscreen test path ---
    // The Vulkan RHI was forward-only (pbr_mesh composites lit color in one
    // pass). These close the GL-vs-Vulkan parity gap: gbuffer writes material
    // properties (albedo / normal+roughness) to an MRT G-Buffer; a fullscreen
    // lighting pass then composites a directional light. Only the offscreen
    // renderOffscreenScene(deferred=true) path uses these; the swapchain
    // renderFrameScene is untouched (stays forward + FSR3 EASU).
    VkPipeline  m_gbufferPipeline      = VK_NULL_HANDLE;  // pbr_mesh.vert + gbuffer.frag (MRT)
    VkPipeline  m_lightingPipeline     = VK_NULL_HANDLE;  // lighting.vert/frag (fullscreen tri)
    VkPipelineLayout m_lightingPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lightingDescSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_lightingDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_lightingDescSet = VK_NULL_HANDLE;
    VkSampler m_lightingSampler = VK_NULL_HANDLE;        // linear, clamp-to-edge

    // ---- B2 SSAO (deferred offscreen path) — twin of the GL path. Kernel +
    // noise come from the shared RHI:: generators (identical bytes); viewProj is
    // rebuilt to match CameraUBO (mat4GlToVulkanProj) so world-pos reconstruction
    // matches the geometry pass. Lighting binds the blurred AO at descriptor
    // binding 6 (white fallback when ssao is off -> 60/60 parity preserved).
    bool m_ssaoCreated = false;
    uint32_t m_ssaoW = 0, m_ssaoH = 0;
    VkPipeline      m_ssaoPipeline       = VK_NULL_HANDLE; // ssao.comp    (geometry -> raw AO)
    VkPipelineLayout m_ssaoPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssaoDescSetLayout = VK_NULL_HANDLE; // b0-depth b1-normal b2-noise b3-ubo b4-rawAO(storage)
    VkDescriptorPool      m_ssaoDescPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssaoDescSet      = VK_NULL_HANDLE;
    VkBuffer              m_ssaoKernelUbo    = VK_NULL_HANDLE; // SsaoUBOData (persist-mapped)
    VmaAllocation         m_ssaoKernelUboMem = VK_NULL_HANDLE;
    void*                 m_ssaoKernelUboMapped = nullptr;
    VkImage               m_ssaoNoiseImg     = VK_NULL_HANDLE; VkImageView m_ssaoNoiseView = VK_NULL_HANDLE;
    VmaAllocation         m_ssaoNoiseMem     = VK_NULL_HANDLE;
    VkImage               m_ssaoAoRawImg     = VK_NULL_HANDLE; VkImageView m_ssaoAoRawView = VK_NULL_HANDLE;
    VmaAllocation         m_ssaoAoRawMem     = VK_NULL_HANDLE;
    VkImage               m_ssaoAoBlurImg    = VK_NULL_HANDLE; VkImageView m_ssaoAoBlurView = VK_NULL_HANDLE;
    VmaAllocation         m_ssaoAoBlurMem    = VK_NULL_HANDLE;
    // Blur pass (own set/layout/pool: b0=AO sampled, b1=blurred AO storage).
    VkPipeline      m_ssaoBlurPipeline       = VK_NULL_HANDLE; // ssao_blur.comp
    VkPipelineLayout m_ssaoBlurPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssaoBlurDescSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_ssaoBlurDescPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssaoBlurDescSet      = VK_NULL_HANDLE;

    // B5 Bloom (Vulkan twin of GL dispatchBloom + rhi/shaders/bloom_*.comp).
    // 3-pass: extract -> blur H -> blur V (ping-pong) -> composite. NO-OP when
    // scene.bloom==false or resources missing -> 79/79 baseline untouched.
    bool m_bloomCreated = false; uint32_t m_bloomW=0, m_bloomH=0;
    VkPipeline      m_bloomExtractPipeline = VK_NULL_HANDLE, m_bloomBlurPipeline = VK_NULL_HANDLE;
    VkPipeline      m_bloomCompositePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomExtractLayout = VK_NULL_HANDLE, m_bloomBlurLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomCompositeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bloomExtractDSL = VK_NULL_HANDLE, m_bloomBlurDSL = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bloomCompositeDSL = VK_NULL_HANDLE;
    VkDescriptorPool m_bloomExtractPool = VK_NULL_HANDLE, m_bloomBlurPool = VK_NULL_HANDLE;
    VkDescriptorPool m_bloomCompositePool = VK_NULL_HANDLE;
    VkDescriptorSet m_bloomExtractSet = VK_NULL_HANDLE, m_bloomBlurSet = VK_NULL_HANDLE;
    VkDescriptorSet m_bloomCompositeSet = VK_NULL_HANDLE;
    VkBuffer m_bloomUBO = VK_NULL_HANDLE; VmaAllocation m_bloomUBOMem = VK_NULL_HANDLE;
    VkImage m_bloomExtractImg = VK_NULL_HANDLE, m_bloomBlurImg = VK_NULL_HANDLE, m_bloomFinalImg = VK_NULL_HANDLE;
    VmaAllocation m_bloomExtractMem = VK_NULL_HANDLE, m_bloomBlurMem = VK_NULL_HANDLE, m_bloomFinalMem = VK_NULL_HANDLE;
    VkImageView m_bloomExtractView = VK_NULL_HANDLE, m_bloomBlurView = VK_NULL_HANDLE, m_bloomFinalView = VK_NULL_HANDLE;
    VkImageLayout m_bloomFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;


    // Phase-B B4 skybox — samplerCube cannot live in the 2D bindless array, so the
    // skybox gets its own pipeline / layout / descriptor set (set=0: CameraUBO +
    // cubemap). These must be declared before the skybox methods below.
    struct Cubemap { VkImage image = VK_NULL_HANDLE; VmaAllocation alloc = VK_NULL_HANDLE;
                     VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE; };
    std::vector<Cubemap> m_cubemaps;
    VkBuffer         m_skyboxVbo = VK_NULL_HANDLE; VmaAllocation m_skyboxVboMem = VK_NULL_HANDLE;
    VkPipeline       m_skyboxPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_skyboxLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_skyboxDescSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_skyboxDescPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_skyboxDescSet = VK_NULL_HANDLE;

    // --- Skinning (4-bone LBS) — separate set=1 so the shared bindless layout
    // (set=0, 4 bindings) stays untouched. Only pbr_skinned reads set=1.
    VkDescriptorSetLayout m_skinDescSetLayout = VK_NULL_HANDLE;   // set=1: b0 skinVB SSBO, b1 palette SSBO
    VkDescriptorPool       m_skinDescPool     = VK_NULL_HANDLE;
    VkPipelineLayout       m_skinPipelineLayout = VK_NULL_HANDLE;  // set0 bindless + set1 skin + push
    VkPipeline             m_pbrSkinnedPipeline   = VK_NULL_HANDLE;
    // Per-mesh skin vertex + joint-palette buffers (allocated lazily, reused
    // across frames; only data is re-uploaded per frame). Indexed by mesh i.
    struct SkinMeshRes {
        VkBuffer skinVb = VK_NULL_HANDLE;       // boneIdx/weight (32 bytes/vert)
        VmaAllocation skinVbMem = VK_NULL_HANDLE;
        VkBuffer boneBuf = VK_NULL_HANDLE;      // joint palette (mat4[])
        VmaAllocation boneBufMem = VK_NULL_HANDLE;
        uint32_t skinVbCapacity = 0;            // verts
        uint32_t paletteCapacity = 0;           // mat4 slots
        VkDescriptorSet descSet = VK_NULL_HANDLE; // per-mesh set=1 (skin VB + palette)
    };
    std::vector<SkinMeshRes> m_skinMeshRes;

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
    float m_rcasSharpness = 0.12f;   // RCAS sharpness stops (makeRcasCon: exp2(-s));
                                      // 0 = neutral/passthrough (softest), higher = sharper.
                                      // Lowered from 0.25 to soften half-res EASU edges ("too edged").
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
    VkPipeline m_lowPbrTerrainPipeline = VK_NULL_HANDLE;  // low-res splat (terrain)

    // Half-res velocity target for the low-res G-buffer pass (RG32F).
    // Written by the velocity-aware low-res scene/mesh/pbr pipelines,
    // consumed by the velocity dilation compute pass.
    VkImage m_lowVelocityImage = VK_NULL_HANDLE;
    VmaAllocation m_lowVelocityAlloc = VK_NULL_HANDLE;
    VkImageView m_lowVelocityView = VK_NULL_HANDLE;

    // Low-res G-Buffer attachments for the optional swapchain deferred path
    // (m_lowColorView is the post-lighting output read by EASU). Allocated in
    // ensureEasuFrameResources; unused by the default forward path.
    VkImage m_lowGBufAlbedoImage = VK_NULL_HANDLE;
    VmaAllocation m_lowGBufAlbedoAlloc = VK_NULL_HANDLE;
    VkImageView m_lowGBufAlbedoView = VK_NULL_HANDLE;
    VkImage m_lowGBufNormalImage = VK_NULL_HANDLE;
    VmaAllocation m_lowGBufNormalAlloc = VK_NULL_HANDLE;
    VkImageView m_lowGBufNormalView = VK_NULL_HANDLE;

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
    float m_temporalFeedback = 0.93f;  // 0..1 blend weight on the current frame.
                                       // Raised from 0.9 so history carries more weight,
                                       // shrinking the (1-fb)*prevJitter residual that shows
                                       // up as half-pixel flicker on static/camera motion.

    // =====================================================================
    // Virtualized Geometry — Cluster Culling (Phase 2)
    // =====================================================================
    // All NULL when no clustered meshes are present — the cluster path is
    // lazily initialized on the first mesh with clusters.
    VkBuffer m_clusterBoundsBuf  = VK_NULL_HANDLE;  // GPUClusterBounds[]
    VmaAllocation m_clusterBoundsMem = VK_NULL_HANDLE;
    VkBuffer m_clusterCmdBuf = VK_NULL_HANDLE;     // GPUClusterCommand[]
    VmaAllocation m_clusterCmdMem  = VK_NULL_HANDLE;
    VkBuffer m_lodHierarchyBuf   = VK_NULL_HANDLE;  // Phase 3 VkLODCluster[]
    VmaAllocation m_lodHierarchyMem = VK_NULL_HANDLE;
    VkBuffer m_indirectDrawBuf = VK_NULL_HANDLE;   // VkDrawIndexedIndirectCommand[]
    VmaAllocation m_indirectDrawMem = VK_NULL_HANDLE;
    VkBuffer m_clusterCounterBuf = VK_NULL_HANDLE; // atomic draw-count counter
    VmaAllocation m_clusterCounterMem = VK_NULL_HANDLE;
    void* m_clusterCounterMapped = nullptr;

    VkDescriptorSetLayout m_clusterDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_clusterDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_clusterDescSet = VK_NULL_HANDLE;
    VkDescriptorSet m_lodDescSet = VK_NULL_HANDLE;  // Phase 3 (b1 = LOD buf)
    VkPipelineLayout m_clusterCullLayout = VK_NULL_HANDLE;
    VkPipeline m_clusterCullPipeline = VK_NULL_HANDLE;
    VkPipeline m_naniteLodPipeline = VK_NULL_HANDLE;

    bool m_clusterCullInitialized = false;
    uint32_t m_totalClusterCount = 0;   // Phase 2: leaf cluster count (cull set)
    uint32_t m_totalNodeCount    = 0;   // Phase 3: root+leaf LOD node count
};

} // namespace

// Factory
std::unique_ptr<IRHI> createVulkanRHI() {
    return std::make_unique<RHIVulkan>();
}

} // namespace RHI
