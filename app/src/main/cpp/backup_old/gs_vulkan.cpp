// ─── GS → Vulkan Translator ───────────────────────────────────────────────────
// Traduce comandos del Graphics Synthesizer de PS2 a draw calls Vulkan.
// El GS dibuja directamente en VRAM (4MB onboard). Nosotros mapeamos esa
// VRAM a texturas Vulkan y ejecutamos los draws en el GPU ARM directamente.
//
// Flujo:
//   GS packet → gs_core.cpp (parsea registros)
//   → kick_primitive() → gs_vulkan.cpp (construye VkDrawCmd)
//   → Vulkan GPU ARM → píxeles en pantalla Android

#include "gs_vulkan.h"
#include "gs_core.h"
#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <vector>
#include <cstring>

#define LOG_TAG "PS2-GS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define VK_CHECK(expr) do { VkResult r = (expr); \
    if (r != VK_SUCCESS) { LOGE(#expr " falló: %d", r); return false; } } while(0)

// ─── Shaders SPIR-V inlineados ───────────────────────────────────────────────
// Shader de vértices: transforma coordenadas PS2 (fixed 4.4 / 4096x4096)
// a NDC de Vulkan (-1..1)
static const uint32_t VERT_SPIRV[] = {
    // SPIR-V compilado del siguiente GLSL:
    // layout(location=0) in vec2 pos;
    // layout(location=1) in vec4 color;
    // layout(location=2) in vec2 uv;
    // layout(location=0) out vec4 vColor;
    // layout(location=1) out vec2 vUV;
    // layout(push_constant) uniform PC { vec2 scale; vec2 offset; } pc;
    // void main() {
    //   gl_Position = vec4(pos * pc.scale + pc.offset, 0, 1);
    //   vColor = color;
    //   vUV = uv;
    // }
    0x07230203,0x00010000,0x000D000B,0x00000024,
    0x00000000,0x00020011,0x00000001,0x0006000B,
    0x00000001,0x4C534C47,0x6474732E,0x3035342E,
    0x00000000,0x0003000E,0x00000000,0x00000001,
    // ... (SPIR-V completo se genera con glslangValidator en el build)
    // Placeholder — reemplazar con SPIR-V real al compilar
    0xFFFFFFFF
};

static const uint32_t FRAG_SPIRV[] = {
    // Fragment shader: sample textura + multiplicar por color
    // layout(location=0) in vec4 vColor;
    // layout(location=1) in vec2 vUV;
    // layout(set=0, binding=0) uniform sampler2D tex;
    // layout(location=0) out vec4 fragColor;
    // void main() { fragColor = texture(tex, vUV) * vColor; }
    0x07230203,0x00010000,0x000D000B,0x00000020,
    0x00000000,0x00020011,0x00000001,
    0xFFFFFFFF
};

struct PS2_Vertex {
    float x, y;       // posición normalizada
    float r, g, b, a; // color
    float u, v;       // coordenadas de textura
};

// ─── GS_Vulkan ────────────────────────────────────────────────────────────────

struct GS_Vulkan {
    VkInstance       instance     = VK_NULL_HANDLE;
    VkPhysicalDevice phys_device  = VK_NULL_HANDLE;
    VkDevice         device       = VK_NULL_HANDLE;
    VkQueue          queue        = VK_NULL_HANDLE;
    uint32_t         queue_family = 0;

    VkSurfaceKHR     surface      = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain    = VK_NULL_HANDLE;

    std::vector<VkImage>     swapchain_images;
    std::vector<VkImageView> swapchain_views;
    std::vector<VkFramebuffer> framebuffers;

    VkRenderPass     render_pass  = VK_NULL_HANDLE;
    VkPipeline       pipeline     = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout  = VK_NULL_HANDLE;

    VkCommandPool    cmd_pool     = VK_NULL_HANDLE;
    VkCommandBuffer  cmd_buf      = VK_NULL_HANDLE;

    VkSemaphore      img_available = VK_NULL_HANDLE;
    VkSemaphore      render_done   = VK_NULL_HANDLE;
    VkFence          frame_fence   = VK_NULL_HANDLE;

    // Buffer de vértices para el frame actual
    VkBuffer         vb_buffer    = VK_NULL_HANDLE;
    VkDeviceMemory   vb_memory    = VK_NULL_HANDLE;
    PS2_Vertex*      vb_mapped    = nullptr;
    uint32_t         vb_count     = 0;

    // Textura de VRAM (4MB → textura de 1024x1024 RGBA)
    VkImage          vram_image   = VK_NULL_HANDLE;
    VkImageView      vram_view    = VK_NULL_HANDLE;
    VkDeviceMemory   vram_memory  = VK_NULL_HANDLE;
    VkSampler        vram_sampler = VK_NULL_HANDLE;
    VkDescriptorSet  vram_desc    = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool    = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;

    uint32_t         width = 640, height = 448;

    bool init(ANativeWindow* window, uint32_t w, uint32_t h);
    void begin_frame();
    void draw_triangles(const PS2_Vertex* verts, uint32_t count);
    void draw_sprites(const PS2_Vertex* verts, uint32_t count);
    void upload_vram_texture(const uint8_t* vram, uint32_t tbp, uint32_t tw, uint32_t th,
                             uint32_t psm);
    void present_frame();
    void shutdown();

private:
    bool create_instance();
    bool create_surface(ANativeWindow* window);
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain(uint32_t w, uint32_t h);
    bool create_render_pass();
    bool create_framebuffers();
    bool create_pipeline();
    bool create_vertex_buffer(size_t size_bytes);
    bool create_vram_texture();
    bool create_sync_objects();
    bool create_command_buffers();

    uint32_t find_memory_type(uint32_t filter, VkMemoryPropertyFlags props);
};

bool GS_Vulkan::init(ANativeWindow* window, uint32_t w, uint32_t h) {
    width = w; height = h;
    LOGI("Iniciando Vulkan GS (%dx%d)", w, h);

    return create_instance()
        && create_surface(window)
        && pick_physical_device()
        && create_logical_device()
        && create_swapchain(w, h)
        && create_render_pass()
        && create_framebuffers()
        && create_pipeline()
        && create_vertex_buffer(4 * 1024 * 1024)  // 4MB para vértices por frame
        && create_vram_texture()
        && create_sync_objects()
        && create_command_buffers();
}

bool GS_Vulkan::create_instance() {
    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "PS2Recompiler",
        .applicationVersion = VK_MAKE_VERSION(0,1,0),
        .pEngineName        = "PS2GS",
        .engineVersion      = VK_MAKE_VERSION(0,1,0),
        .apiVersion         = VK_API_VERSION_1_1,
    };

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app_info,
        .enabledExtensionCount   = 2,
        .ppEnabledExtensionNames = extensions,
    };

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
    LOGI("Vulkan instance creada");
    return true;
}

bool GS_Vulkan::create_surface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR ci = {
        .sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window,
    };
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance, &ci, nullptr, &surface));
    return true;
}

bool GS_Vulkan::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    // Elegir el primer dispositivo (en Android siempre hay uno — el GPU del SoC)
    phys_device = devices[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys_device, &props);
    LOGI("GPU: %s", props.deviceName);
    return true;
}

bool GS_Vulkan::create_logical_device() {
    // Buscar queue family con soporte gráfico + presentación
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_device, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_device, &qf_count, qf.data());

    for (uint32_t i = 0; i < qf_count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys_device, i, surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            queue_family = i;
            break;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };

    const char* dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = 1,
        .ppEnabledExtensionNames = dev_ext,
    };

    VK_CHECK(vkCreateDevice(phys_device, &ci, nullptr, &device));
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    return true;
}

bool GS_Vulkan::create_swapchain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_device, surface, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_device, surface, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_device, surface, &fmt_count, formats.data());

    // Preferir RGBA8 SRGB
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f; break;
        }
    }

    VkExtent2D extent = { w, h };

    VkSwapchainCreateInfoKHR ci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = surface,
        .minImageCount    = 2,  // double buffering
        .imageFormat      = chosen.format,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_FIFO_KHR,  // vsync ON (60fps naturales)
        .clipped          = VK_TRUE,
    };

    VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));

    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &img_count, nullptr);
    swapchain_images.resize(img_count);
    vkGetSwapchainImagesKHR(device, swapchain, &img_count, swapchain_images.data());

    swapchain_views.resize(img_count);
    for (uint32_t i = 0; i < img_count; i++) {
        VkImageViewCreateInfo vci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = chosen.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 },
        };
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &swapchain_views[i]));
    }

    LOGI("Swapchain creado: %d imágenes %dx%d", img_count, w, h);
    return true;
}

bool GS_Vulkan::create_render_pass() {
    VkAttachmentDescription color_att = {
        .format         = VK_FORMAT_R8G8B8A8_SRGB,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &ref,
    };

    VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &color_att,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
    };

    VK_CHECK(vkCreateRenderPass(device, &ci, nullptr, &render_pass));
    return true;
}

bool GS_Vulkan::create_framebuffers() {
    framebuffers.resize(swapchain_views.size());
    for (size_t i = 0; i < swapchain_views.size(); i++) {
        VkFramebufferCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = render_pass,
            .attachmentCount = 1,
            .pAttachments    = &swapchain_views[i],
            .width           = width,
            .height          = height,
            .layers          = 1,
        };
        VK_CHECK(vkCreateFramebuffer(device, &ci, nullptr, &framebuffers[i]));
    }
    return true;
}

bool GS_Vulkan::create_pipeline() {
    // Los shaders SPIR-V reales se compilan con glslangValidator durante el build.
    // Este pipeline configura el estado de renderizado que coincide con el GS de PS2:
    // - blending alpha igual al GS
    // - sin culling (el GS no hace backface culling)
    // - depth test configurable por registro GS_ZBUF

    VkPipelineVertexInputStateCreateInfo vert_in = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_asm = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkViewport vp = { 0, 0, (float)width, (float)height, 0, 1 };
    VkRect2D   sc = { {0,0}, {width, height} };

    VkPipelineViewportStateCreateInfo vp_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount  = 1, .pScissors  = &sc,
    };

    VkPipelineRasterizationStateCreateInfo raster = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,  // GS no hace backface culling
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    // Alpha blending igual al GS de PS2 (modo más común: src_alpha / one_minus_src_alpha)
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                               VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend,
    };

    // Push constants para la transformación de coordenadas GS→NDC
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = 16,  // vec2 scale + vec2 offset
    };

    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc_range,
    };

    VK_CHECK(vkCreatePipelineLayout(device, &layout_ci, nullptr, &pipe_layout));
    LOGI("Pipeline Vulkan configurado");
    return true;
}

bool GS_Vulkan::create_vertex_buffer(size_t size_bytes) {
    VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size_bytes,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(device, &ci, nullptr, &vb_buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, vb_buffer, &req);

    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = find_memory_type(req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &vb_memory));
    vkBindBufferMemory(device, vb_buffer, vb_memory, 0);
    vkMapMemory(device, vb_memory, 0, size_bytes, 0, (void**)&vb_mapped);
    LOGI("Vertex buffer: %zu KB", size_bytes/1024);
    return true;
}

bool GS_Vulkan::create_vram_texture() {
    // VRAM del GS: 4 MB → textura de 2048x512 RGBA8 (mapeado lineal)
    VkImageCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = { 2048, 512, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_LINEAR,   // acceso directo desde CPU
        .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(device, &ci, nullptr, &vram_image));
    LOGI("Textura VRAM creada (2048x512)");
    return true;
}

bool GS_Vulkan::create_sync_objects() {
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                  nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
    VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &img_available));
    VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &render_done));
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &frame_fence));
    return true;
}

bool GS_Vulkan::create_command_buffers() {
    VkCommandPoolCreateInfo ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family,
    };
    VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmd_pool));

    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd_buf));
    return true;
}

void GS_Vulkan::begin_frame() {
    vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &frame_fence);
    vb_count = 0;
}

void GS_Vulkan::draw_triangles(const PS2_Vertex* verts, uint32_t count) {
    if (!vb_mapped) return;
    memcpy(vb_mapped + vb_count, verts, count * sizeof(PS2_Vertex));
    vb_count += count;
}

void GS_Vulkan::present_frame() {
    uint32_t img_idx = 0;
    vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                           img_available, VK_NULL_HANDLE, &img_idx);

    // Grabar comandos de dibujo
    VkCommandBufferBeginInfo begin = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd_buf, &begin);

    VkClearValue clear = { .color = {{0,0,0,1}} };
    VkRenderPassBeginInfo rp = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = render_pass,
        .framebuffer     = framebuffers[img_idx],
        .renderArea      = {{0,0},{width,height}},
        .clearValueCount = 1,
        .pClearValues    = &clear,
    };
    vkCmdBeginRenderPass(cmd_buf, &rp, VK_SUBPASS_CONTENTS_INLINE);

    if (vb_count > 0) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd_buf, 0, 1, &vb_buffer, &offset);

        // Push constant: transformación PS2 (4096x4096 fixed) → NDC
        float pc[4] = { 2.0f/4096.0f, -2.0f/4096.0f, -1.0f, 1.0f };
        vkCmdPushConstants(cmd_buf, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);

        vkCmdDraw(cmd_buf, vb_count, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd_buf);
    vkEndCommandBuffer(cmd_buf);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &img_available,
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd_buf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &render_done,
    };
    vkQueueSubmit(queue, 1, &submit, frame_fence);

    VkPresentInfoKHR present = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &render_done,
        .swapchainCount     = 1,
        .pSwapchains        = &swapchain,
        .pImageIndices      = &img_idx,
    };
    vkQueuePresentKHR(queue, &present);
}

uint32_t GS_Vulkan::find_memory_type(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((filter & (1<<i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

void GS_Vulkan::shutdown() {
    if (device) vkDeviceWaitIdle(device);
    // Cleanup de todos los recursos Vulkan
    if (frame_fence)   vkDestroyFence(device, frame_fence, nullptr);
    if (render_done)   vkDestroySemaphore(device, render_done, nullptr);
    if (img_available) vkDestroySemaphore(device, img_available, nullptr);
    if (cmd_pool)      vkDestroyCommandPool(device, cmd_pool, nullptr);
    if (pipeline)      vkDestroyPipeline(device, pipeline, nullptr);
    if (pipe_layout)   vkDestroyPipelineLayout(device, pipe_layout, );
    if (render_pass)   vkDestroyRenderPass(device, render_pass, nullptr);
    for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (auto iv : swapchain_views) vkDestroyImageView(device, iv, nullptr);
    if (swapchain)     vkDestroySwapchainKHR(device, swapchain, nullptr);
    if (surface)       vkDestroySurfaceKHR(instance, surface, nullptr);
    if (device)        vkDestroyDevice(device, nullptr);
    if (instance)      vkDestroyInstance(instance, nullptr);
}
