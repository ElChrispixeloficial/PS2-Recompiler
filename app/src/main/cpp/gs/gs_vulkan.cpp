#define VK_USE_PLATFORM_ANDROID_KHR
#include "gs_vulkan.h"
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <android/log.h>
#include <vector>
#include <cstring>

extern "C" int g_vulkan_draws;
extern "C" int g_vulkan_presents;

#define TAG "PS2-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define VK_CHECK(x) do { VkResult _r = x; if (_r) { LOGE("VK err %d at %s:%d", _r, __FILE__, __LINE__); return false; } } while(0)
#define VK_CHECK_NORET(x) do { VkResult _r = x; if (_r) { LOGE("VK err %d at %s:%d", _r, __FILE__, __LINE__); return; } } while(0)

struct VulkanTexture {
    VkImage        image    = VK_NULL_HANDLE;
    VkDeviceMemory memory   = VK_NULL_HANDLE;
    VkImageView    view     = VK_NULL_HANDLE;
    uint32_t       width    = 0;
    uint32_t       height   = 0;
};

static std::vector<VulkanTexture> s_textures;
static uint32_t s_bound_texture = 0;

static const uint32_t ps2_vert_spv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x0000002E,
    0x00000017, 0x00020011, 0x00000001, 0x0006000B,
    0x00000009, 0x4C534C47, 0x00000004, 0x00000004,
    0x00000009, 0x00000000, 0x0000000B, 0x00000013,
    0x00000001, 0x0000000E, 0x00000012, 0x00000005,
    0x00000005, 0x00000009, 0x00000000, 0x00050051,
    0x00000009, 0x00000004, 0x00000003, 0x00000001,
    0x00000000, 0x00050051, 0x00000009, 0x00000005,
    0x00000003, 0x00000001, 0x00000001, 0x00050051,
    0x00000009, 0x00000006, 0x00000003, 0x00000001,
    0x00000002, 0x00050051, 0x00000009, 0x00000008,
    0x00000003, 0x00000001, 0x00000003, 0x00030047,
    0x00000009, 0x00000002, 0x00000001, 0x00050048,
    0x00000009, 0x00000000, 0x00000004, 0x00000002,
    0x00000001, 0x00050048, 0x00000009, 0x00000001,
    0x00000005, 0x00000003, 0x00000002, 0x00050048,
    0x00000009, 0x00000002, 0x00000006, 0x00000003,
    0x00000004, 0x00030047, 0x00000009, 0x00000001,
    0x00000003, 0x00020044, 0x0000000B, 0x00000001,
    0x00000004, 0x00020044, 0x0000000B, 0x00000001,
    0x00000008, 0x00010044, 0x0000000B, 0x00000002,
    0x00050048, 0x00000009, 0x00000003, 0x00000007,
    0x00000001, 0x00000000, 0x00050048, 0x00000009,
    0x00000004, 0x00000008, 0x00000000, 0x00000002,
    0x00030047, 0x00000009, 0x00000002, 0x00000001,
    0x00040047, 0x0000000B, 0x0000000B, 0x00000000,
    0x00050044, 0x0000000B, 0x00000001, 0x00000012,
    0x00000000, 0x00020044, 0x0000000B, 0x00000002,
    0x00000004, 0x00050048, 0x00000009, 0x00000006,
    0x00000006, 0x00000003, 0x00000002, 0x00020048,
    0x00000009, 0x00000006, 0x00000007, 0x00040047,
    0x0000000B, 0x00000009, 0x00000001, 0x00050048,
    0x00000009, 0x00000007, 0x00000008, 0x00000007,
    0x00000001, 0x00040047, 0x0000000B, 0x0000000E,
    0x00000000, 0x00050044, 0x0000000B, 0x00000001,
    0x00000017, 0x00000000, 0x00020044, 0x0000000B,
    0x00000002, 0x00000008, 0x00050048, 0x00000009,
    0x00000008, 0x00000006, 0x00000003, 0x00000004,
    0x00020048, 0x00000009, 0x00000008, 0x00000007,
    0x00040047, 0x0000000B, 0x00000013, 0x00000002,
    0x00020044, 0x0000000B, 0x00000001, 0x0000001E,
    0x00050048, 0x00000009, 0x00000009, 0x00000006,
    0x00000003, 0x00000005, 0x00020048, 0x00000009,
    0x00000009, 0x00000007, 0x00040047, 0x0000000B,
    0x00000018, 0x00000000, 0x00050048, 0x00000009,
    0x0000000A, 0x00000008, 0x00000000, 0x00000002,
    0x00030047, 0x00000009, 0x00000002, 0x00000005,
    0x00040047, 0x0000000B, 0x0000001D, 0x00000001,
    0x00050048, 0x00000009, 0x0000000B, 0x00000009,
    0x00000003, 0x00000006, 0x00020048, 0x00000009,
    0x0000000B, 0x00000007, 0x00040047, 0x0000000B,
    0x00000023, 0x00000003, 0x00050044, 0x0000000B,
    0x00000001, 0x00000029, 0x00000000, 0x00020044,
    0x0000000B, 0x00000002, 0x00000006, 0x00050048,
    0x00000009, 0x0000000C, 0x00000006, 0x00000003,
    0x00000007, 0x00020048, 0x00000009, 0x0000000C,
    0x00000007, 0x00040047, 0x0000000B, 0x0000002E,
    0x00000001, 0x00050048, 0x00000009, 0x0000000D,
    0x00000008, 0x00000000, 0x00000002, 0x00030047,
    0x00000009, 0x00000002, 0x00000007, 0x00040047,
    0x0000000B, 0x0000002F, 0x00000002, 0x0004003D,
    0x00000009, 0x00000004, 0x00000004, 0x00050041,
    0x0000000B, 0x0000000D, 0x00000004, 0x00000001,
    0x00000002, 0x0004003D, 0x00000009, 0x00000005,
    0x00000005, 0x00050041, 0x0000000B, 0x0000000E,
    0x00000005, 0x00000001, 0x00000001, 0x00050041,
    0x0000000B, 0x00000013, 0x0000000D, 0x00000002,
    0x00000003, 0x0004003D, 0x00000009, 0x00000006,
    0x00000006, 0x00050041, 0x0000000B, 0x00000018,
    0x00000006, 0x00000001, 0x00000004, 0x0003003E,
    0x00000009, 0x00000003, 0x0000000E, 0x0004003D,
    0x00000009, 0x00000008, 0x00000008, 0x00050041,
    0x0000000B, 0x0000001D, 0x00000008, 0x00000001,
    0x00000005, 0x0004003D, 0x00000009, 0x00000009,
    0x00000009, 0x00050041, 0x0000000B, 0x00000023,
    0x00000009, 0x00000001, 0x00000002, 0x0004003D,
    0x00000009, 0x0000000A, 0x0000000A, 0x00050041,
    0x0000000B, 0x0000002E, 0x0000000A, 0x00000001,
    0x00000004, 0x00050041, 0x0000000B, 0x0000002F,
    0x0000000A, 0x00000001, 0x00000003, 0x0003003E,
    0x00000009, 0x0000000B, 0x0000001D, 0x000100FD,
    0x00010038
};

static const uint32_t ps2_frag_spv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x0000001C,
    0x00000017, 0x00020011, 0x00000001, 0x0006000B,
    0x00000009, 0x4C534C47, 0x00000004, 0x00000004,
    0x00000009, 0x00000000, 0x0000000B, 0x00000013,
    0x00000001, 0x0000000E, 0x00000012, 0x00000005,
    0x00000005, 0x00000009, 0x00000000, 0x00050051,
    0x00000009, 0x00000004, 0x00000003, 0x00000001,
    0x00000000, 0x00050051, 0x00000009, 0x00000005,
    0x00000003, 0x00000001, 0x00000001, 0x00030047,
    0x00000009, 0x00000002, 0x00000001, 0x00040047,
    0x00000009, 0x00000004, 0x00000001, 0x00040047,
    0x00000009, 0x00000005, 0x00000000, 0x00020044,
    0x0000000B, 0x00000001, 0x00000004, 0x00020044,
    0x0000000B, 0x00000001, 0x00000008, 0x00010044,
    0x0000000B, 0x00000002, 0x00050044, 0x0000000B,
    0x00000001, 0x00000012, 0x00000000, 0x00020044,
    0x0000000B, 0x00000002, 0x00000004, 0x00040047,
    0x0000000B, 0x0000000B, 0x00000000, 0x00050048,
    0x00000009, 0x00000004, 0x00000006, 0x00000003,
    0x00000002, 0x00020048, 0x00000009, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x00000008,
    0x00000000, 0x00050048, 0x00000009, 0x00000004,
    0x00000009, 0x00000003, 0x00000004, 0x00020048,
    0x00000009, 0x00000004, 0x0000000A, 0x00040047,
    0x00000009, 0x0000000D, 0x00000001, 0x00020044,
    0x0000000B, 0x00000001, 0x00000017, 0x00050048,
    0x00000009, 0x00000005, 0x0000000B, 0x00000003,
    0x00000005, 0x00020048, 0x00000009, 0x00000005,
    0x0000000C, 0x00040047, 0x0000000B, 0x0000001C,
    0x00000000, 0x0004003D, 0x00000009, 0x00000004,
    0x00000004, 0x0004003D, 0x00000009, 0x00000005,
    0x00000005, 0x00050041, 0x0000000B, 0x0000000B,
    0x00000004, 0x00000001, 0x00000002, 0x00050041,
    0x0000000B, 0x0000000D, 0x0000000B, 0x00000002,
    0x00000003, 0x0004003D, 0x00000009, 0x00000008,
    0x00000008, 0x00050085, 0x00000009, 0x00000008,
    0x00000005, 0x00000004, 0x00050041, 0x0000000B,
    0x0000001C, 0x00000008, 0x00000001, 0x00000001,
    0x0003003E, 0x00000009, 0x00000006, 0x0000000D,
    0x000100FD, 0x00010038
};

static uint32_t find_mem_type(VkPhysicalDevice dev, uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(dev, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

VkShaderModule GS_Vulkan::create_shader_module(const uint32_t* code, size_t size_bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size_bytes;
    ci.pCode    = code;
    VkShaderModule mod;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) {
        LOGE("Failed to create shader module");
        return VK_NULL_HANDLE;
    }
    return mod;
}

uint32_t GS_Vulkan::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    return find_mem_type(m_phys_dev, type_filter, props);
}

bool GS_Vulkan::create_instance() {
    VkApplicationInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName   = "PS2-Recompiler";
    ai.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    ai.pEngineName        = "PS2";
    ai.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    ai.apiVersion         = VK_API_VERSION_1_1;

    const char* exts[] = { "VK_KHR_surface", "VK_KHR_android_surface" };

    VkInstanceCreateInfo ii{};
    ii.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo        = &ai;
    ii.enabledExtensionCount   = 2;
    ii.ppEnabledExtensionNames = exts;

    VkResult r = vkCreateInstance(&ii, nullptr, &m_instance);
    if (r != VK_SUCCESS) { LOGE("vkCreateInstance failed: %d", r); return false; }
    return true;
}

bool GS_Vulkan::create_surface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window;
    VkResult r = vkCreateAndroidSurfaceKHR(m_instance, &ci, nullptr, &m_surface);
    if (r != VK_SUCCESS) { LOGE("vkCreateAndroidSurfaceKHR failed: %d", r); return false; }
    return true;
}

bool GS_Vulkan::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) { LOGE("No Vulkan GPUs found"); return false; }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    m_phys_dev = devices[0];

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys_dev, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_props(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys_dev, &queue_count, queue_props.data());

    bool found_gfx = false;
    bool found_present = false;
    for (uint32_t i = 0; i < queue_count; i++) {
        if (queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_gfx_family = i;
            found_gfx = true;
        }
        VkBool32 present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_phys_dev, i, m_surface, &present_support);
        if (present_support) {
            m_present_family = i;
            found_present = true;
        }
        if (found_gfx && found_present) break;
    }

    if (!found_gfx || !found_present) {
        LOGE("Suitable queue family not found");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_phys_dev, &props);
    LOGI("GPU: %s", props.deviceName);
    return true;
}

bool GS_Vulkan::create_logical_device() {
    float priority = 1.0f;

    uint32_t unique_families[] = { m_gfx_family, m_present_family };
    uint32_t family_count = (m_gfx_family == m_present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo queue_infos[2]{};
    for (uint32_t i = 0; i < family_count; i++) {
        queue_infos[i].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[i].queueFamilyIndex = unique_families[i];
        queue_infos[i].queueCount       = 1;
        queue_infos[i].pQueuePriorities = &priority;
    }

    const char* dev_exts[] = { "VK_KHR_swapchain" };

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = family_count;
    ci.pQueueCreateInfos       = queue_infos;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = dev_exts;

    VkResult r = vkCreateDevice(m_phys_dev, &ci, nullptr, &m_device);
    if (r != VK_SUCCESS) { LOGE("vkCreateDevice failed: %d", r); return false; }

    vkGetDeviceQueue(m_device, m_gfx_family, 0, &m_gfx_queue);
    vkGetDeviceQueue(m_device, m_present_family, 0, &m_present_queue);
    return true;
}

bool GS_Vulkan::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_phys_dev, m_surface, &caps);
    m_sc_extent = caps.currentExtent;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = m_surface;
    ci.minImageCount    = 2;
    ci.imageFormat      = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent      = m_sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    VkResult r = vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain);
    if (r != VK_SUCCESS) { LOGE("vkCreateSwapchainKHR failed: %d", r); return false; }

    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_sc_images.resize(image_count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_sc_images.data());

    m_sc_views.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = m_sc_images[i];
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vi.format                          = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.layerCount     = 1;
        VkResult rv = vkCreateImageView(m_device, &vi, nullptr, &m_sc_views[i]);
        if (rv != VK_SUCCESS) { LOGE("vkCreateImageView failed: %d", rv); return false; }
    }

    m_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
    LOGI("Swapchain: %ux%u, %u images", m_sc_extent.width, m_sc_extent.height, image_count);
    return true;
}

bool GS_Vulkan::create_render_pass() {
    VkAttachmentDescription attachment{};
    attachment.format         = m_sc_format;
    attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &attachment;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies   = &dependency;

    VkResult r = vkCreateRenderPass(m_device, &rp, nullptr, &m_render_pass);
    if (r != VK_SUCCESS) { LOGE("vkCreateRenderPass failed: %d", r); return false; }
    return true;
}

bool GS_Vulkan::create_pipeline() {
    VkShaderModule vert_mod = create_shader_module(ps2_vert_spv, sizeof(ps2_vert_spv));
    VkShaderModule frag_mod = create_shader_module(ps2_frag_spv, sizeof(ps2_frag_spv));
    if (!vert_mod || !frag_mod) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(PS2_Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PS2_Vertex, x) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PS2_Vertex, r) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(PS2_Vertex, u) };

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.width    = (float)m_sc_extent.width;
    vp.height   = (float)m_sc_extent.height;
    vp.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = m_sc_extent;

    VkPipelineViewportStateCreateInfo vs{};
    vs.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.pViewports    = &vp;
    vs.scissorCount  = 1;
    vs.pScissors     = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = 16;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;

    VkResult r = vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipeline_layout);
    if (r != VK_SUCCESS) { LOGE("vkCreatePipelineLayout failed: %d", r); return false; }

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vs;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState   = &ms;
    pci.pColorBlendState    = &cb;
    pci.layout              = m_pipeline_layout;
    pci.renderPass          = m_render_pass;

    r = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline);
    if (r != VK_SUCCESS) { LOGE("vkCreateGraphicsPipelines failed: %d", r); return false; }

    vkDestroyShaderModule(m_device, vert_mod, nullptr);
    vkDestroyShaderModule(m_device, frag_mod, nullptr);
    LOGI("Pipeline created");
    return true;
}

bool GS_Vulkan::create_framebuffers() {
    m_framebuffers.resize(m_sc_views.size());
    for (size_t i = 0; i < m_sc_views.size(); i++) {
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = m_render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments    = &m_sc_views[i];
        fi.width           = m_sc_extent.width;
        fi.height          = m_sc_extent.height;
        fi.layers          = 1;
        VkResult r = vkCreateFramebuffer(m_device, &fi, nullptr, &m_framebuffers[i]);
        if (r != VK_SUCCESS) { LOGE("vkCreateFramebuffer failed: %d", r); return false; }
    }
    return true;
}

bool GS_Vulkan::create_command_pool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_gfx_family;
    VkResult r = vkCreateCommandPool(m_device, &ci, nullptr, &m_cmd_pool);
    if (r != VK_SUCCESS) { LOGE("vkCreateCommandPool failed: %d", r); return false; }
    return true;
}

bool GS_Vulkan::create_command_buffers() {
    m_cmd_bufs.resize(m_sc_images.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = m_cmd_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)m_cmd_bufs.size();
    VkResult r = vkAllocateCommandBuffers(m_device, &ai, m_cmd_bufs.data());
    if (r != VK_SUCCESS) { LOGE("vkAllocateCommandBuffers failed: %d", r); return false; }
    return true;
}

bool GS_Vulkan::create_sync_objects() {
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkResult r1 = vkCreateSemaphore(m_device, &sci, nullptr, &m_img_available);
    VkResult r2 = vkCreateSemaphore(m_device, &sci, nullptr, &m_render_done);
    if (r1 != VK_SUCCESS || r2 != VK_SUCCESS) { LOGE("Semaphore creation failed"); return false; }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkResult r3 = vkCreateFence(m_device, &fci, nullptr, &m_in_flight);
    if (r3 != VK_SUCCESS) { LOGE("Fence creation failed"); return false; }
    return true;
}

bool GS_Vulkan::create_vertex_buffer(VkDeviceSize size) {
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = size;
    bi.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer(m_device, &bi, nullptr, &m_vb);
    if (r != VK_SUCCESS) { LOGE("vkCreateBuffer failed: %d", r); return false; }

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, m_vb, &mr);

    VkMemoryAllocateInfo ai{};
    ai.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize   = mr.size;
    ai.memoryTypeIndex  = find_memory_type(mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    r = vkAllocateMemory(m_device, &ai, nullptr, &m_vb_mem);
    if (r != VK_SUCCESS) { LOGE("vkAllocateMemory failed: %d", r); return false; }

    vkBindBufferMemory(m_device, m_vb, m_vb_mem, 0);
    m_vb_capacity = size;
    vkMapMemory(m_device, m_vb_mem, 0, size, 0, &m_vb_mapped);
    LOGI("Vertex buffer: %zu bytes", (size_t)size);
    return true;
}

bool GS_Vulkan::init(ANativeWindow* window) {
    LOGI("Initializing Vulkan...");

    if (!create_instance())          return false;
    if (!create_surface(window))     return false;
    if (!pick_physical_device())     return false;
    if (!create_logical_device())    return false;
    if (!create_swapchain())         return false;
    if (!create_render_pass())       return false;
    if (!create_pipeline())          return false;
    if (!create_framebuffers())      return false;
    if (!create_command_pool())      return false;
    if (!create_command_buffers())   return false;
    if (!create_sync_objects())      return false;
    if (!create_vertex_buffer(4 * 1024 * 1024)) return false;

    m_ready = true;
    LOGI("Vulkan initialized OK");
    return true;
}

void GS_Vulkan::cleanup_swapchain() {
    if (!m_device) return;

    for (auto& fb : m_framebuffers) {
        if (fb) vkDestroyFramebuffer(m_device, fb, nullptr);
    }
    m_framebuffers.clear();

    for (auto& view : m_sc_views) {
        if (view) vkDestroyImageView(m_device, view, nullptr);
    }
    m_sc_views.clear();

    if (m_swapchain) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_sc_images.clear();
}

bool GS_Vulkan::begin_frame() {
    if (!m_ready) return false;

    vkWaitForFences(m_device, 1, &m_in_flight, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_in_flight);

    VkResult ar = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                         m_img_available, VK_NULL_HANDLE, &m_current_image);
    if (ar != VK_SUCCESS) { return false; }

    VkCommandBuffer cb = m_cmd_bufs[m_current_image];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass  = m_render_pass;
    rpbi.framebuffer = m_framebuffers[m_current_image];
    rpbi.renderArea  = { {0, 0}, m_sc_extent };
    VkClearValue cv = {{{0.0f, 0.0f, 0.2f, 1.0f}}};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues    = &cv;

    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    if (m_pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    }

    m_vb_vertex_offset = 0;
    return true;
}

void GS_Vulkan::end_frame() {}

void GS_Vulkan::draw_primitive(const PS2_Vertex* verts, uint32_t count, uint32_t prim_type) {
    if (!m_ready || count == 0 || !m_pipeline) return;
    g_vulkan_draws++;

    VkCommandBuffer cb = m_cmd_bufs[m_current_image];

    uint32_t needed = count * sizeof(PS2_Vertex);
    if (m_vb_vertex_offset + needed > m_vb_capacity) {
        m_vb_vertex_offset = 0;
    }

    memcpy((uint8_t*)m_vb_mapped + m_vb_vertex_offset, verts, needed);

    VkDeviceSize buf_offset = m_vb_vertex_offset;
    vkCmdBindVertexBuffers(cb, 0, 1, &m_vb, &buf_offset);

    float scale[2] = { 2.0f / m_sc_extent.width, -2.0f / m_sc_extent.height };
    float offset[2] = { -1.0f, 1.0f };
    vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, scale);
    vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 8, 8, offset);

    vkCmdDraw(cb, count, 1, 0, 0);

    m_vb_vertex_offset += needed;
}

void GS_Vulkan::present_frame() {
    if (!m_ready) return;

    VkCommandBuffer cb = m_cmd_bufs[m_current_image];
    vkCmdEndRenderPass(cb);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return;

    g_vulkan_presents++;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &m_img_available;
    si.pWaitDstStageMask    = &wait_stage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_cmd_bufs[m_current_image];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_render_done;

    if (vkQueueSubmit(m_gfx_queue, 1, &si, m_in_flight) != VK_SUCCESS) return;

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_render_done;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_swapchain;
    pi.pImageIndices      = &m_current_image;

    vkQueuePresentKHR(m_present_queue, &pi);
}

uint32_t GS_Vulkan::upload_texture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t psm) {
    if (!m_device || width == 0 || height == 0) return 0;

    VkFormat format;
    uint32_t bpp;
    switch (psm) {
        case 0:  format = VK_FORMAT_R8G8B8A8_UNORM; bpp = 32; break;
        case 2:  format = VK_FORMAT_R5G5B5A1_UNORM_PACK16; bpp = 16; break;
        default: format = VK_FORMAT_R8G8B8A8_UNORM; bpp = 32; break;
    }

    uint32_t pixel_bytes = bpp / 8;
    VkDeviceSize image_size = (VkDeviceSize)width * height * pixel_bytes;

    VulkanTexture tex{};
    tex.width  = width;
    tex.height = height;

    VkImageCreateInfo image_ci{};
    image_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType     = VK_IMAGE_TYPE_2D;
    image_ci.extent.width  = width;
    image_ci.extent.height = height;
    image_ci.extent.depth  = 1;
    image_ci.mipLevels     = 1;
    image_ci.arrayLayers   = 1;
    image_ci.format        = format;
    image_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkResult r = vkCreateImage(m_device, &image_ci, nullptr, &tex.image);
    if (r != VK_SUCCESS) { LOGE("vkCreateImage failed: %d", r); return 0; }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(m_device, tex.image, &mem_req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize   = mem_req.size;
    alloc.memoryTypeIndex  = find_memory_type(mem_req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    r = vkAllocateMemory(m_device, &alloc, nullptr, &tex.memory);
    if (r != VK_SUCCESS) {
        LOGE("Texture alloc failed: %d", r);
        vkDestroyImage(m_device, tex.image, nullptr);
        return 0;
    }
    vkBindImageMemory(m_device, tex.image, tex.memory, 0);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image                           = tex.image;
    view_ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format                          = format;
    view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.baseMipLevel   = 0;
    view_ci.subresourceRange.levelCount     = 1;
    view_ci.subresourceRange.baseArrayLayer = 0;
    view_ci.subresourceRange.layerCount     = 1;

    r = vkCreateImageView(m_device, &view_ci, nullptr, &tex.view);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateImageView failed: %d", r);
        vkDestroyImage(m_device, tex.image, nullptr);
        vkFreeMemory(m_device, tex.memory, nullptr);
        return 0;
    }

    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;

    VkBufferCreateInfo buf_ci{};
    buf_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size        = image_size;
    buf_ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateBuffer(m_device, &buf_ci, nullptr, &staging_buf);

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(m_device, staging_buf, &buf_req);

    VkMemoryAllocateInfo buf_alloc{};
    buf_alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buf_alloc.allocationSize  = buf_req.size;
    buf_alloc.memoryTypeIndex = find_memory_type(buf_req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(m_device, &buf_alloc, nullptr, &staging_mem);
    vkBindBufferMemory(m_device, staging_buf, staging_mem, 0);

    void* mapped;
    vkMapMemory(m_device, staging_mem, 0, image_size, 0, &mapped);
    if (bpp == 16) {
        const uint16_t* src16 = reinterpret_cast<const uint16_t*>(data);
        uint32_t* dst32 = static_cast<uint32_t*>(mapped);
        for (uint32_t i = 0; i < width * height; i++) {
            uint16_t c = src16[i];
            float r_f = ((c >>  0) & 0x1F) / 31.0f;
            float g_f = ((c >>  5) & 0x1F) / 31.0f;
            float b_f = ((c >> 10) & 0x1F) / 31.0f;
            float a_f = ((c >> 15) & 0x01) ? 1.0f : 0.0f;
            uint8_t rb = (uint8_t)(r_f * 255);
            uint8_t gb = (uint8_t)(g_f * 255);
            uint8_t bb = (uint8_t)(b_f * 255);
            uint8_t ab = (uint8_t)(a_f * 255);
            dst32[i] = rb | (gb << 8) | (bb << 16) | (ab << 24);
        }
    } else {
        memcpy(mapped, data, image_size);
    }
    vkUnmapMemory(m_device, staging_mem);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool        = m_cmd_pool;
    cmd_ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo cmd_bi{};
    cmd_bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cmd_bi);

    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = tex.image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.layerCount     = 1;
    barrier.srcAccessMask                   = 0;
    barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageExtent = { width, height, 1 };

    vkCmdCopyBufferToImage(cmd, staging_buf, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_gfx_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_gfx_queue);

    vkFreeCommandBuffers(m_device, m_cmd_pool, 1, &cmd);
    vkDestroyBuffer(m_device, staging_buf, nullptr);
    vkFreeMemory(m_device, staging_mem, nullptr);

    uint32_t id = (uint32_t)s_textures.size();
    s_textures.push_back(tex);
    LOGI("Texture uploaded: id=%u %ux%u psm=%u", id, width, height, psm);
    return id;
}

void GS_Vulkan::bind_texture(uint32_t tex_id) {
    s_bound_texture = tex_id;
}

void GS_Vulkan::shutdown() {
    if (!m_ready) return;

    vkDeviceWaitIdle(m_device);

    for (auto& tex : s_textures) {
        if (tex.view)   vkDestroyImageView(m_device, tex.view, nullptr);
        if (tex.image)  vkDestroyImage(m_device, tex.image, nullptr);
        if (tex.memory) vkFreeMemory(m_device, tex.memory, nullptr);
    }
    s_textures.clear();

    if (m_vb)      vkDestroyBuffer(m_device, m_vb, nullptr);
    if (m_vb_mem)  vkFreeMemory(m_device, m_vb_mem, nullptr);
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipeline_layout) vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
    if (m_in_flight)  vkDestroyFence(m_device, m_in_flight, nullptr);
    if (m_render_done) vkDestroySemaphore(m_device, m_render_done, nullptr);
    if (m_img_available) vkDestroySemaphore(m_device, m_img_available, nullptr);
    if (m_cmd_pool) vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);

    cleanup_swapchain();

    if (m_render_pass) vkDestroyRenderPass(m_device, m_render_pass, nullptr);
    if (m_device)  vkDestroyDevice(m_device, nullptr);
    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance) vkDestroyInstance(m_instance, nullptr);

    m_ready = false;
    LOGI("Vulkan shutdown complete");
}
