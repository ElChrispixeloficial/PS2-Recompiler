#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/log.h>
#include "gs_vulkan.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <cstring>

extern "C" int g_vulkan_draws;
extern "C" int g_vulkan_presents;

#define TAG "GS_Vulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define VK_CHECK(x) do { VkResult err = x; if (err) { LOGE("Vulkan error %d at %s:%d", err, __FILE__, __LINE__); return false; } } while(0)
#define VK_CHECK_NORET(x) do { VkResult err = x; if (err) { LOGE("Vulkan error %d at %s:%d", err, __FILE__, __LINE__); return; } } while(0)

static uint32_t find_mem_type(VkPhysicalDevice d, uint32_t f, VkMemoryPropertyFlags p) {
    VkPhysicalDeviceMemoryProperties m; vkGetPhysicalDeviceMemoryProperties(d,&m);
    for(uint32_t i=0;i<m.memoryTypeCount;i++) if((f&(1<<i))&&(m.memoryTypes[i].propertyFlags&p)==p) return i;
    return 0;
}

VkShaderModule GS_Vulkan::create_shader_module(const uint32_t* code, size_t size_bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size_bytes;
    ci.pCode = code;
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

bool GS_Vulkan::init(ANativeWindow* window) {
    LOGI("Init Vulkan...");
    VkApplicationInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.pApplicationName="PR2"; ai.apiVersion=VK_API_VERSION_1_1;
    const char* exts[]={"VK_KHR_surface","VK_KHR_android_surface"};
    VkInstanceCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ii.pApplicationInfo=&ai; ii.enabledExtensionCount=2; ii.ppEnabledExtensionNames=exts;
    VK_CHECK(vkCreateInstance(&ii,nullptr,&m_instance));
    VkAndroidSurfaceCreateInfoKHR si{}; si.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR; si.window=window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(m_instance,&si,nullptr,&m_surface));
    uint32_t gc=0; vkEnumeratePhysicalDevices(m_instance,&gc,nullptr);
    if(gc==0){LOGE("No GPU");return false;}
    std::vector<VkPhysicalDevice> gpus(gc); vkEnumeratePhysicalDevices(m_instance,&gc,gpus.data()); m_phys_dev=gpus[0];
    float p=1.0f; VkDeviceQueueCreateInfo qi{}; qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex=0; qi.queueCount=1; qi.pQueuePriorities=&p;
    VkDeviceCreateInfo di{}; di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    VK_CHECK(vkCreateDevice(m_phys_dev,&di,nullptr,&m_device));
    vkGetDeviceQueue(m_device,0,0,&m_gfx_queue); m_present_queue=m_gfx_queue;
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_phys_dev,m_surface,&caps);
    m_sc_extent=caps.currentExtent;
    VkSwapchainCreateInfoKHR si2{}; si2.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR; si2.surface=m_surface; si2.minImageCount=2;
    si2.imageFormat=VK_FORMAT_B8G8R8A8_UNORM; si2.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; si2.imageExtent=m_sc_extent;
    si2.imageArrayLayers=1; si2.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; si2.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    si2.preTransform=caps.currentTransform; si2.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; si2.presentMode=VK_PRESENT_MODE_FIFO_KHR; si2.clipped=VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(m_device,&si2,nullptr,&m_swapchain));
    uint32_t ic=0; vkGetSwapchainImagesKHR(m_device,m_swapchain,&ic,nullptr); m_sc_images.resize(ic); vkGetSwapchainImagesKHR(m_device,m_swapchain,&ic,m_sc_images.data());
    m_sc_views.resize(ic);
    for(uint32_t i=0;i<ic;i++){VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image=m_sc_images[i]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_B8G8R8A8_UNORM; vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1; VK_CHECK(vkCreateImageView(m_device,&vi,nullptr,&m_sc_views[i]));}
    VkAttachmentDescription ad{}; ad.format=VK_FORMAT_B8G8R8A8_UNORM; ad.samples=VK_SAMPLE_COUNT_1_BIT; ad.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp=VK_ATTACHMENT_STORE_OP_STORE; ad.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ar{}; ar.attachment=0; ar.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sp{}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount=1; sp.pColorAttachments=&ar;
    VkSubpassDependency dep{}; dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0; dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rp.attachmentCount=1; rp.pAttachments=&ad; rp.subpassCount=1; rp.pSubpasses=&sp; rp.dependencyCount=1; rp.pDependencies=&dep;
    VK_CHECK(vkCreateRenderPass(m_device,&rp,nullptr,&m_render_pass));
    m_framebuffers.resize(ic);
    for(uint32_t i=0;i<ic;i++){VkFramebufferCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass=m_render_pass; fi.attachmentCount=1; fi.pAttachments=&m_sc_views[i]; fi.width=m_sc_extent.width; fi.height=m_sc_extent.height; fi.layers=1; VK_CHECK(vkCreateFramebuffer(m_device,&fi,nullptr,&m_framebuffers[i]));}
    VkCommandPoolCreateInfo cp{}; cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; VK_CHECK(vkCreateCommandPool(m_device,&cp,nullptr,&m_cmd_pool));
    m_cmd_bufs.resize(ic); VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ca.commandPool=m_cmd_pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=ic; VK_CHECK(vkAllocateCommandBuffers(m_device,&ca,m_cmd_bufs.data()));
    VkSemaphoreCreateInfo sc{}; sc.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; VK_CHECK(vkCreateSemaphore(m_device,&sc,nullptr,&m_img_available)); VK_CHECK(vkCreateSemaphore(m_device,&sc,nullptr,&m_render_done));
    VkFenceCreateInfo fc{}; fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fc.flags=VK_FENCE_CREATE_SIGNALED_BIT; VK_CHECK(vkCreateFence(m_device,&fc,nullptr,&m_in_flight));
    create_pipeline();
    create_vertex_buffer(1024 * 1024);
    m_ready=true; LOGI("Vulkan OK"); return true;
}

bool GS_Vulkan::create_pipeline() {
    VkShaderModule vert_mod = create_shader_module(ps2_vert_spv, sizeof(ps2_vert_spv));
    VkShaderModule frag_mod = create_shader_module(ps2_frag_spv, sizeof(ps2_frag_spv));
    if (!vert_mod || !frag_mod) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(PS2_Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PS2_Vertex, x)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PS2_Vertex, r)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PS2_Vertex, u)};

    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{}; vp.width = (float)m_sc_extent.width; vp.height = (float)m_sc_extent.height; vp.maxDepth = 1.0f;
    VkRect2D scissor{}; scissor.extent = m_sc_extent;

    VkPipelineViewportStateCreateInfo vs{}; vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1; vs.pViewports = &vp; vs.scissorCount = 1; vs.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.size = 16;

    VkPipelineLayoutCreateInfo plci{}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipeline_layout));

    VkGraphicsPipelineCreateInfo pci{}; pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2; pci.pStages = stages;
    pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vs; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pColorBlendState = &cb;
    pci.layout = m_pipeline_layout; pci.renderPass = m_render_pass;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_device, vert_mod, nullptr);
    vkDestroyShaderModule(m_device, frag_mod, nullptr);
    LOGI("Pipeline created OK");
    return true;
}

bool GS_Vulkan::create_vertex_buffer(VkDeviceSize size) {
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_device, &bi, nullptr, &m_vb));

    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(m_device, m_vb, &mr);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_device, &ai, nullptr, &m_vb_mem));
    vkBindBufferMemory(m_device, m_vb, m_vb_mem, 0);
    m_vb_capacity = size;
    vkMapMemory(m_device, m_vb_mem, 0, size, 0, &m_vb_mapped);
    LOGI("Vertex buffer created: %zu bytes", (size_t)size);
    return true;
}

void GS_Vulkan::shutdown() {
    if(!m_ready) return;
    vkDeviceWaitIdle(m_device);
    if(m_vb)vkDestroyBuffer(m_device,m_vb,nullptr);
    if(m_vb_mem)vkFreeMemory(m_device,m_vb_mem,nullptr);
    if(m_pipeline)vkDestroyPipeline(m_device,m_pipeline,nullptr);
    if(m_pipeline_layout)vkDestroyPipelineLayout(m_device,m_pipeline_layout,nullptr);
    if(m_in_flight)vkDestroyFence(m_device,m_in_flight,nullptr);
    if(m_render_done)vkDestroySemaphore(m_device,m_render_done,nullptr);
    if(m_img_available)vkDestroySemaphore(m_device,m_img_available,nullptr);
    if(m_cmd_pool)vkDestroyCommandPool(m_device,m_cmd_pool,nullptr);
    for(auto&f:m_framebuffers)if(f)vkDestroyFramebuffer(m_device,f,nullptr);
    if(m_render_pass)vkDestroyRenderPass(m_device,m_render_pass,nullptr);
    for(auto&iv:m_sc_views)if(iv)vkDestroyImageView(m_device,iv,nullptr);
    if(m_swapchain)vkDestroySwapchainKHR(m_device,m_swapchain,nullptr);
    if(m_device)vkDestroyDevice(m_device,nullptr);
    if(m_surface)vkDestroySurfaceKHR(m_instance,m_surface,nullptr);
    if(m_instance)vkDestroyInstance(m_instance,nullptr);
    m_ready=false;
}

bool GS_Vulkan::begin_frame() { 
    if(!m_ready) return false; 
    
    vkWaitForFences(m_device, 1, &m_in_flight, VK_TRUE, UINT64_MAX); 
    vkResetFences(m_device, 1, &m_in_flight); 
    
    if(vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_img_available, VK_NULL_HANDLE, &m_current_image) != VK_SUCCESS) {
        return false;
    }

    VkCommandBuffer cb = m_cmd_bufs[m_current_image];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if(vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;

    VkRenderPassBeginInfo rp{}; 
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; 
    rp.renderPass = m_render_pass; 
    rp.framebuffer = m_framebuffers[m_current_image]; 
    rp.renderArea = {{0,0}, m_sc_extent};
    VkClearValue cv = {{{0.0f, 0.0f, 0.2f, 1.0f}}}; 
    rp.clearValueCount = 1; 
    rp.pClearValues = &cv;
    
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    
    if (m_pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    }
    
    return true; 
}

void GS_Vulkan::end_frame() {}

void GS_Vulkan::draw_primitive(const PS2_Vertex* verts, uint32_t count, uint32_t) {
    if(!m_ready || count == 0 || !m_pipeline) return;
    g_vulkan_draws++;

    VkCommandBuffer cb = m_cmd_bufs[m_current_image];

    uint32_t offset = m_vb_vertex_offset;
    uint32_t needed = count * sizeof(PS2_Vertex);
    if (offset + needed > m_vb_capacity) {
        offset = 0;
        m_vb_vertex_offset = 0;
    }

    memcpy((uint8_t*)m_vb_mapped + offset, verts, needed);
    VkDeviceSize buf_offset = offset;
    vkCmdBindVertexBuffers(cb, 0, 1, &m_vb, &buf_offset);

    float scale[2] = {2.0f / m_sc_extent.width, -2.0f / m_sc_extent.height};
    float offset2[2] = {-1.0f, 1.0f};
    vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, scale);
    vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 8, 8, offset2);

    vkCmdDraw(cb, count, 1, 0, 0);

    m_vb_vertex_offset += needed;
}

void GS_Vulkan::present_frame() {
    if(!m_ready) return; 
    
    VkCommandBuffer cb = m_cmd_bufs[m_current_image];
    vkCmdEndRenderPass(cb); 
    
    if(vkEndCommandBuffer(cb) != VK_SUCCESS) {
        return;
    }

    g_vulkan_presents++;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{}; 
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; 
    si.waitSemaphoreCount = 1; 
    si.pWaitSemaphores = &m_img_available; 
    si.pWaitDstStageMask = &ws; 
    si.commandBufferCount = 1; 
    si.pCommandBuffers = &m_cmd_bufs[m_current_image]; 
    si.signalSemaphoreCount = 1; 
    si.pSignalSemaphores = &m_render_done;
    
    if(vkQueueSubmit(m_gfx_queue, 1, &si, m_in_flight) != VK_SUCCESS) {
        return;
    }
    
    VkPresentInfoKHR pi{}; 
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; 
    pi.waitSemaphoreCount = 1; 
    pi.pWaitSemaphores = &m_render_done; 
    pi.swapchainCount = 1; 
    pi.pSwapchains = &m_swapchain; 
    pi.pImageIndices = &m_current_image;
    
    vkQueuePresentKHR(m_present_queue, &pi);
}

void GS_Vulkan::cleanup_swapchain() {}
uint32_t GS_Vulkan::upload_texture(const uint8_t*,uint32_t,uint32_t,uint32_t){return 0;}
void GS_Vulkan::bind_texture(uint32_t){}
