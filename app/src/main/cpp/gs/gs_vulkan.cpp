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
    VkRenderPassCreateInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rp.attachmentCount=1; rp.pAttachments=&ad; rp.subpassCount=1; rp.pSubpasses=&sp;
    VK_CHECK(vkCreateRenderPass(m_device,&rp,nullptr,&m_render_pass));
    m_framebuffers.resize(ic);
    for(uint32_t i=0;i<ic;i++){VkFramebufferCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass=m_render_pass; fi.attachmentCount=1; fi.pAttachments=&m_sc_views[i]; fi.width=m_sc_extent.width; fi.height=m_sc_extent.height; fi.layers=1; VK_CHECK(vkCreateFramebuffer(m_device,&fi,nullptr,&m_framebuffers[i]));}
    VkCommandPoolCreateInfo cp{}; cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; VK_CHECK(vkCreateCommandPool(m_device,&cp,nullptr,&m_cmd_pool));
    m_cmd_bufs.resize(ic); VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ca.commandPool=m_cmd_pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=ic; VK_CHECK(vkAllocateCommandBuffers(m_device,&ca,m_cmd_bufs.data()));
    VkSemaphoreCreateInfo sc{}; sc.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; VK_CHECK(vkCreateSemaphore(m_device,&sc,nullptr,&m_img_available)); VK_CHECK(vkCreateSemaphore(m_device,&sc,nullptr,&m_render_done));
    VkFenceCreateInfo fc{}; fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fc.flags=VK_FENCE_CREATE_SIGNALED_BIT; VK_CHECK(vkCreateFence(m_device,&fc,nullptr,&m_in_flight));
    m_ready=true; LOGI("Vulkan OK"); return true;
}

void GS_Vulkan::shutdown() {
    if(!m_ready) return;
    vkDeviceWaitIdle(m_device);
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
    
    return true; 
}

void GS_Vulkan::end_frame() {}

void GS_Vulkan::draw_primitive(const PS2_Vertex* verts, uint32_t count, uint32_t) {
    if(!m_ready || count == 0) return;
    g_vulkan_draws++;
    // Aquí en el futuro irán las llamadas a vkCmdBindPipeline y vkCmdDraw
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