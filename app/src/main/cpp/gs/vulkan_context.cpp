// gs/vulkan_context.cpp
// Shared Vulkan context helpers (extensions, layers, debug utils).
// In Phase 1 the GS_Vulkan class is self-contained; this file is a
// placeholder for future context-sharing between GS and VU1 XGKICK.
#include <vulkan/vulkan.h>
#include <android/log.h>

#define TAG "VulkanCtx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// List available instance extensions (diagnostic helper)
void vk_log_instance_extensions() {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    LOGI("%u Vulkan instance extensions available", count);
}
