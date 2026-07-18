#define VK_USE_PLATFORM_ANDROID_KHR
#pragma once
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <vector>
#include <cstdint>

/* Vértice PS2 unificado para el pipeline Vulkan */
struct PS2_Vertex {
    float x, y, z, w;   /* posición */
    float r, g, b, a;   /* color RGBA 0-1 */
    float u, v;          /* coordenadas de textura */
};

class GS_Vulkan {
public:
    /* Ciclo de vida */
    bool init(ANativeWindow* window);
    void shutdown();

    /* Renderizado */
    bool begin_frame();
    void end_frame();
    void draw_primitive(const PS2_Vertex* verts, uint32_t count, uint32_t prim_type);
    void present_frame();

    /* Gestión de texturas */
    uint32_t upload_texture(const uint8_t* data, uint32_t width, uint32_t height,
                            uint32_t psm);
    void bind_texture(uint32_t tex_id);

    /* Estado */
    bool is_ready() const { return m_ready; }
    uint32_t width()  const { return m_width;  }
    uint32_t height() const { return m_height; }

private:
    /* ---- helpers internos ---- */
    bool create_instance();
    bool create_surface(ANativeWindow* window);
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain();
    bool create_render_pass();
    bool create_pipeline();
    bool create_framebuffers();
    bool create_command_pool();
    bool create_command_buffers();
    bool create_sync_objects();
    bool create_vertex_buffer(VkDeviceSize size);

    VkShaderModule create_shader_module(const uint32_t* code, size_t size_bytes);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);

    void cleanup_swapchain();

    /* ---- objetos Vulkan ---- */
    VkInstance               m_instance          = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface            = VK_NULL_HANDLE;
    VkPhysicalDevice         m_phys_dev           = VK_NULL_HANDLE;
    VkDevice                 m_device             = VK_NULL_HANDLE;
    VkQueue                  m_gfx_queue          = VK_NULL_HANDLE;
    VkQueue                  m_present_queue      = VK_NULL_HANDLE;

    VkSwapchainKHR           m_swapchain          = VK_NULL_HANDLE;
    std::vector<VkImage>     m_sc_images;
    std::vector<VkImageView> m_sc_views;
    VkFormat                 m_sc_format          = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_sc_extent          = {0, 0};

    VkRenderPass             m_render_pass        = VK_NULL_HANDLE;
    VkPipelineLayout         m_pipeline_layout    = VK_NULL_HANDLE;
    VkPipeline               m_pipeline           = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> m_framebuffers;
    VkCommandPool            m_cmd_pool           = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_cmd_bufs;

    VkSemaphore              m_img_available      = VK_NULL_HANDLE;
    VkSemaphore              m_render_done        = VK_NULL_HANDLE;
    VkFence                  m_in_flight          = VK_NULL_HANDLE;

    VkBuffer                 m_vb                 = VK_NULL_HANDLE;
    VkDeviceMemory           m_vb_mem             = VK_NULL_HANDLE;
    VkDeviceSize             m_vb_capacity        = 0;
    void*                    m_vb_mapped          = nullptr;
    uint32_t                 m_vb_vertex_offset   = 0;

    uint32_t                 m_gfx_family         = 0;
    uint32_t                 m_present_family     = 0;
    uint32_t                 m_current_image      = 0;

    uint32_t                 m_width              = 640;
    uint32_t                 m_height             = 448;
    bool                     m_ready              = false;
};