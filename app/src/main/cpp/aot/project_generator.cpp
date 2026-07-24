#include "project_generator.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <android/log.h>

#define LOG_TAG "ProjectGen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::string s_out_dir;

static void mkdirs_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
    }
}

void Project_Generator::write_file(const std::string& path, const std::string& content) {
    size_t sl = path.rfind('/');
    if (sl != std::string::npos) mkdirs_p(path.substr(0, sl));
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { LOGE("Failed to create: %s", path.c_str()); return; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    LOGI("Wrote %zu bytes: %s", content.size(), path.c_str());
}

std::string Project_Generator::package_to_path(const std::string& pkg) {
    std::string r = pkg;
    std::replace(r.begin(), r.end(), '.', '/');
    return r;
}

bool Project_Generator::generate(const ELF_Analyzer& analyzer,
                                 const std::vector<TranslatedFunction>& functions,
                                 const std::vector<TextureAsset>& textures,
                                 const std::vector<AudioAsset>& audio,
                                 const std::vector<ModelAsset>& models,
                                 const ProjectConfig& config) {
    LOGI("Generating project: %s in %s", config.game_name.c_str(), config.out_dir.c_str());
    s_out_dir = config.out_dir;
    generate_cmake(config);
    generate_build_gradle(config);
    generate_manifest(config);
    generate_jni_bridge(config, analyzer);
    generate_main_activity(config);
    generate_game_cpp(config, analyzer, functions);
    generate_ee_state(analyzer);
    generate_iop_state();
    generate_gs_renderer();
    generate_dma_controller();
    generate_vu_processor();
    generate_memory_map(analyzer);
    generate_assets_index(textures, audio, models);
    LOGI("Project generation complete");
    return true;
}

void Project_Generator::generate_cmake(const ProjectConfig& config) {
    std::string path = config.out_dir + "/app/src/main/cpp/CMakeLists.txt";
    std::ostringstream o;
    o << "cmake_minimum_required(VERSION 3.22.1)\n";
    o << "project(\"" << config.game_name << "\" LANGUAGES CXX)\n\n";
    o << "set(CMAKE_CXX_STANDARD 17)\n";
    o << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    o << "add_library(native-lib SHARED\n";
    o << "    native-lib.cpp\n";
    o << "    game_core.cpp\n";
    o << "    ee_state.cpp\n";
    o << "    iop_state.cpp\n";
    o << "    gs_renderer.cpp\n";
    o << "    dma_controller.cpp\n";
    o << "    vu_processor.cpp\n";
    o << "    memory_map.cpp\n";
    o << ")\n\n";
    o << "target_compile_options(native-lib PRIVATE\n";
    o << "    -march=armv8-a+simd\n";
    o << "    -O2\n";
    o << "    -ffast-math\n";
    o << "    -Wall -Wextra\n";
    o << ")\n\n";
    o << "find_library(log-lib log)\n";
    o << "find_library(android-lib android)\n";
    o << "find_library(vulkan-lib vulkan)\n";
    o << "find_library(opensles-lib opensles)\n\n";
    o << "target_link_libraries(native-lib\n";
    o << "    ${log-lib}\n";
    o << "    ${android-lib}\n";
    o << "    ${vulkan-lib}\n";
    o << "    ${opensles-lib}\n";
    o << ")\n";
    write_file(path, o.str());
}

void Project_Generator::generate_build_gradle(const ProjectConfig& config) {
    std::string path = config.out_dir + "/app/build.gradle.kts";
    std::ostringstream o;
    o << "plugins {\n";
    o << "    id(\"com.android.application\")\n";
    o << "    id(\"org.jetbrains.kotlin.android\")\n";
    o << "}\n\n";
    o << "android {\n";
    o << "    namespace = \"" << config.package_name << "\"\n";
    o << "    compileSdk = 35\n\n";
    o << "    defaultConfig {\n";
    o << "        applicationId = \"" << config.package_name << "\"\n";
    o << "        minSdk = " << config.min_sdk << "\n";
    o << "        targetSdk = " << config.target_sdk << "\n";
    o << "        versionCode = 1\n";
    o << "        versionName = \"1.0\"\n\n";
    o << "        ndk {\n";
    o << "            abiFilters += setOf(\"arm64-v8a\")\n";
    o << "        }\n\n";
    o << "        externalNativeBuild {\n";
    o << "            cmake {\n";
    o << "                cppFlags += \"\"\n";
    o << "                arguments += \"-DANDROID_STL=c++_shared\"\n";
    o << "            }\n";
    o << "        }\n";
    o << "    }\n\n";
    o << "    externalNativeBuild {\n";
    o << "        cmake {\n";
    o << "            path = file(\"src/main/cpp/CMakeLists.txt\")\n";
    o << "            version = \"" << config.cmake_version << "\"\n";
    o << "        }\n";
    o << "    }\n\n";
    o << "    buildTypes {\n";
    o << "        release {\n";
    o << "            isMinifyEnabled = false\n";
    o << "            proguardFiles(\n";
    o << "                getDefaultProguardFile(\"proguard-android-optimize.txt\"),\n";
    o << "                \"proguard-rules.pro\"\n";
    o << "            )\n";
    o << "        }\n";
    o << "    }\n\n";
    o << "    compileOptions {\n";
    o << "        sourceCompatibility = JavaVersion.VERSION_17\n";
    o << "        targetCompatibility = JavaVersion.VERSION_17\n";
    o << "    }\n\n";
    o << "    kotlinOptions {\n";
    o << "        jvmTarget = \"17\"\n";
    o << "    }\n\n";
    o << "    sourceSets {\n";
    o << "        getByName(\"main\") {\n";
    o << "            java.srcDirs(\"src/main/kotlin\")\n";
    o << "        }\n";
    o << "    }\n";
    o << "}\n\n";
    o << "dependencies {\n";
    o << "    implementation(\"androidx.core:core-ktx:1.13.1\")\n";
    o << "    implementation(\"androidx.appcompat:appcompat:1.7.0\")\n";
    o << "    implementation(\"com.google.android.material:material:1.12.0\")\n";
    o << "}\n";
    write_file(path, o.str());
}

void Project_Generator::generate_manifest(const ProjectConfig& config) {
    std::string path = config.out_dir + "/app/src/main/AndroidManifest.xml";
    std::string pkg_path = package_to_path(config.package_name);
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    o << "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">\n\n";
    o << "    <uses-permission android:name=\"android.permission.INTERNET\" />\n\n";
    o << "    <uses-feature android:glEsVersion=\"0x00030000\" android:required=\"true\" />\n";
    o << "    <uses-feature android:name=\"android.hardware.vulkan.level\" android:required=\"true\" />\n";
    o << "    <uses-feature android:name=\"android.hardware.vulkan.version\" android:required=\"true\" />\n\n";
    o << "    <application\n";
    o << "        android:allowBackup=\"true\"\n";
    o << "        android:label=\"" << config.game_name << "\"\n";
    o << "        android:supportsRtl=\"true\"\n";
    o << "        android:theme=\"@style/Theme.AppCompat.NoActionBar\"\n";
    o << "        android:hasCode=\"true\"\n";
    o << "        android:hardwareAccelerated=\"true\">\n\n";
    o << "        <activity\n";
    o << "            android:name=\"" << config.package_name << ".MainActivity\"\n";
    o << "            android:exported=\"true\"\n";
    o << "            android:configChanges=\"orientation|screenSize|keyboardHidden\"\n";
    o << "            android:screenOrientation=\"landscape\">\n";
    o << "            <intent-filter>\n";
    o << "                <action android:name=\"android.intent.action.MAIN\" />\n";
    o << "                <category android:name=\"android.intent.category.LAUNCHER\" />\n";
    o << "            </intent-filter>\n";
    o << "        </activity>\n\n";
    o << "        <activity\n";
    o << "            android:name=\"" << config.package_name << ".RuntimeActivity\"\n";
    o << "            android:exported=\"false\"\n";
    o << "            android:configChanges=\"orientation|screenSize|keyboardHidden\"\n";
    o << "            android:screenOrientation=\"landscape\"\n";
    o << "            android:theme=\"@android:style/Theme.NoTitleBar.Fullscreen\">\n";
    o << "            <meta-data\n";
    o << "                android:name=\"android.app.lib_name\"\n";
    o << "                android:value=\"native-lib\" />\n";
    o << "        </activity>\n\n";
    o << "    </application>\n\n";
    o << "</manifest>\n";
    write_file(path, o.str());
}

void Project_Generator::generate_jni_bridge(const ProjectConfig& config,
                                            const ELF_Analyzer& analyzer) {
    std::string path = config.out_dir + "/app/src/main/cpp/native-lib.cpp";
    std::string jp = package_to_path(config.package_name);
    const AOT_ELFInfo& info = analyzer.elf_info();

    std::ostringstream o;
    o << "#include <jni.h>\n";
    o << "#include <string>\n";
    o << "#include <cstring>\n";
    o << "#include <atomic>\n";
    o << "#include <thread>\n";
    o << "#include <chrono>\n";
    o << "#include <android/native_activity.h>\n";
    o << "#include <android/native_window.h>\n";
    o << "#include <android/native_window_jni.h>\n";
    o << "#include <android/log.h>\n";
    o << "#include <vulkan/vulkan.h>\n";
    o << "#include \"game_core.h\"\n\n";
    o << "#define LOG_TAG \"NativeBridge\"\n";
    o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n";
    o << "#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)\n\n";
    o << "static JavaVM* g_jvm = nullptr;\n";
    o << "static ANativeWindow* g_window = nullptr;\n";
    o << "static std::atomic<bool> g_running{false};\n";
    o << "static std::thread g_cpu_thread;\n";
    o << "static std::string g_elf_path;\n\n";

    // Vulkan state
    o << "static VkInstance g_vk_instance = VK_NULL_HANDLE;\n";
    o << "static VkSurfaceKHR g_vk_surface = VK_NULL_HANDLE;\n";
    o << "static VkPhysicalDevice g_vk_phys = VK_NULL_HANDLE;\n";
    o << "static VkDevice g_vk_dev = VK_NULL_HANDLE;\n";
    o << "static uint32_t g_vk_gfx_queue = 0;\n";
    o << "static VkSwapchainKHR g_vk_swap = VK_NULL_HANDLE;\n";
    o << "static VkFormat g_vk_fmt = VK_FORMAT_B8G8R8A8_UNORM;\n";
    o << "static VkExtent2D g_vk_ext = {0, 0};\n";
    o << "static std::vector<VkImage> g_vk_images;\n";
    o << "static std::vector<VkImageView> g_vk_views;\n";
    o << "static VkRenderPass g_vk_rp = VK_NULL_HANDLE;\n";
    o << "static std::vector<VkFramebuffer> g_vk_fbs;\n";
    o << "static VkCommandPool g_vk_pool = VK_NULL_HANDLE;\n";
    o << "static std::vector<VkCommandBuffer> g_vk_cmds;\n";
    o << "static VkSemaphore g_vk_sem_img = VK_NULL_HANDLE;\n";
    o << "static VkSemaphore g_vk_sem_rnd = VK_NULL_HANDLE;\n";
    o << "static VkFence g_vk_fence = VK_NULL_HANDLE;\n\n";

    // init_vulkan
    o << "static bool init_vulkan() {\n";
    o << "    if (!g_window) return false;\n";
    o << "    VkApplicationInfo ai{};\n";
    o << "    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;\n";
    o << "    ai.pApplicationName = \"" << config.game_name << "\";\n";
    o << "    ai.applicationVersion = VK_MAKE_VERSION(1,0,0);\n";
    o << "    ai.pEngineName = \"PS2Recompiler\";\n";
    o << "    ai.engineVersion = VK_MAKE_VERSION(1,0,0);\n";
    o << "    ai.apiVersion = VK_API_VERSION_1_1;\n";
    o << "    VkInstanceCreateInfo ci{};\n";
    o << "    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;\n";
    o << "    ci.pApplicationInfo = &ai;\n";
    o << "    if (vkCreateInstance(&ci, nullptr, &g_vk_instance) != VK_SUCCESS) {\n";
    o << "        LOGE(\"Vulkan instance failed\"); return false;\n";
    o << "    }\n";
    o << "    VkAndroidSurfaceCreateInfoKHR sci{};\n";
    o << "    sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;\n";
    o << "    sci.window = g_window;\n";
    o << "    if (vkCreateAndroidSurfaceKHR(g_vk_instance, &sci, nullptr, &g_vk_surface) != VK_SUCCESS) {\n";
    o << "        LOGE(\"Surface failed\"); return false;\n";
    o << "    }\n";
    o << "    uint32_t dc = 0;\n";
    o << "    vkEnumeratePhysicalDevices(g_vk_instance, &dc, nullptr);\n";
    o << "    if (!dc) { LOGE(\"No GPU\"); return false; }\n";
    o << "    std::vector<VkPhysicalDevice> devs(dc);\n";
    o << "    vkEnumeratePhysicalDevices(g_vk_instance, &dc, devs.data());\n";
    o << "    g_vk_phys = devs[0];\n";
    o << "    uint32_t qfc = 0;\n";
    o << "    vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys, &qfc, nullptr);\n";
    o << "    std::vector<VkQueueFamilyProperties> qf(qfc);\n";
    o << "    vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys, &qfc, qf.data());\n";
    o << "    g_vk_gfx_queue = 0;\n";
    o << "    for (uint32_t i = 0; i < qfc; i++) {\n";
    o << "        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vk_gfx_queue = i; break; }\n";
    o << "    }\n";
    o << "    float qp = 1.0f;\n";
    o << "    VkDeviceQueueCreateInfo dqci{};\n";
    o << "    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;\n";
    o << "    dqci.queueFamilyIndex = g_vk_gfx_queue;\n";
    o << "    dqci.queueCount = 1;\n";
    o << "    dqci.pQueuePriorities = &qp;\n";
    o << "    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};\n";
    o << "    VkDeviceCreateInfo dci{};\n";
    o << "    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;\n";
    o << "    dci.queueCreateInfoCount = 1;\n";
    o << "    dci.pQueueCreateInfos = &dqci;\n";
    o << "    dci.enabledExtensionCount = 1;\n";
    o << "    dci.ppEnabledExtensionNames = exts;\n";
    o << "    if (vkCreateDevice(g_vk_phys, &dci, nullptr, &g_vk_dev) != VK_SUCCESS) {\n";
    o << "        LOGE(\"Device failed\"); return false;\n";
    o << "    }\n";
    o << "    LOGI(\"Vulkan OK\");\n";
    o << "    return true;\n";
    o << "}\n\n";

    // create_swapchain
    o << "static bool create_swapchain() {\n";
    o << "    if (!g_vk_dev || !g_vk_surface || !g_window) return false;\n";
    o << "    ANativeWindow_Buffer buf;\n";
    o << "    ANativeWindow_getBuffers(g_window, &buf);\n";
    o << "    g_vk_ext = {(uint32_t)buf.width, (uint32_t)buf.height};\n";
    o << "    VkSwapchainCreateInfoKHR sci{};\n";
    o << "    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;\n";
    o << "    sci.surface = g_vk_surface;\n";
    o << "    sci.minImageCount = 3;\n";
    o << "    sci.imageFormat = g_vk_fmt;\n";
    o << "    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;\n";
    o << "    sci.imageExtent = g_vk_ext;\n";
    o << "    sci.imageArrayLayers = 1;\n";
    o << "    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;\n";
    o << "    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;\n";
    o << "    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;\n";
    o << "    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;\n";
    o << "    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;\n";
    o << "    sci.clipped = VK_TRUE;\n";
    o << "    if (vkCreateSwapchainKHR(g_vk_dev, &sci, nullptr, &g_vk_swap) != VK_SUCCESS) {\n";
    o << "        LOGE(\"Swapchain failed\"); return false;\n";
    o << "    }\n";
    o << "    uint32_t ic = 0;\n";
    o << "    vkGetSwapchainImagesKHR(g_vk_dev, g_vk_swap, &ic, nullptr);\n";
    o << "    g_vk_images.resize(ic);\n";
    o << "    vkGetSwapchainImagesKHR(g_vk_dev, g_vk_swap, &ic, g_vk_images.data());\n";
    o << "    g_vk_views.resize(ic);\n";
    o << "    for (uint32_t i = 0; i < ic; i++) {\n";
    o << "        VkImageViewCreateInfo vi{};\n";
    o << "        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;\n";
    o << "        vi.image = g_vk_images[i];\n";
    o << "        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;\n";
    o << "        vi.format = g_vk_fmt;\n";
    o << "        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;\n";
    o << "        vi.subresourceRange.levelCount = 1;\n";
    o << "        vi.subresourceRange.layerCount = 1;\n";
    o << "        vkCreateImageView(g_vk_dev, &vi, nullptr, &g_vk_views[i]);\n";
    o << "    }\n";
    o << "    VkAttachmentDescription ca{};\n";
    o << "    ca.format = g_vk_fmt;\n";
    o << "    ca.samples = VK_SAMPLE_COUNT_1_BIT;\n";
    o << "    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;\n";
    o << "    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;\n";
    o << "    ca.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;\n";
    o << "    ca.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;\n";
    o << "    VkAttachmentReference cr{};\n";
    o << "    cr.attachment = 0;\n";
    o << "    cr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;\n";
    o << "    VkSubpassDescription sp{};\n";
    o << "    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;\n";
    o << "    sp.colorAttachmentCount = 1;\n";
    o << "    sp.pColorAttachments = &cr;\n";
    o << "    VkSubpassDependency dep{};\n";
    o << "    dep.srcSubpass = VK_SUBPASS_EXTERNAL;\n";
    o << "    dep.dstSubpass = 0;\n";
    o << "    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;\n";
    o << "    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;\n";
    o << "    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;\n";
    o << "    VkRenderPassCreateInfo rpi{};\n";
    o << "    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;\n";
    o << "    rpi.attachmentCount = 1;\n";
    o << "    rpi.pAttachments = &ca;\n";
    o << "    rpi.subpassCount = 1;\n";
    o << "    rpi.pSubpasses = &sp;\n";
    o << "    rpi.dependencyCount = 1;\n";
    o << "    rpi.pDependencies = &dep;\n";
    o << "    vkCreateRenderPass(g_vk_dev, &rpi, nullptr, &g_vk_rp);\n";
    o << "    g_vk_fbs.resize(ic);\n";
    o << "    for (uint32_t i = 0; i < ic; i++) {\n";
    o << "        VkFramebufferCreateInfo fbi{};\n";
    o << "        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;\n";
    o << "        fbi.renderPass = g_vk_rp;\n";
    o << "        fbi.attachmentCount = 1;\n";
    o << "        fbi.pAttachments = &g_vk_views[i];\n";
    o << "        fbi.width = g_vk_ext.width;\n";
    o << "        fbi.height = g_vk_ext.height;\n";
    o << "        fbi.layers = 1;\n";
    o << "        vkCreateFramebuffer(g_vk_dev, &fbi, nullptr, &g_vk_fbs[i]);\n";
    o << "    }\n";
    o << "    VkCommandPoolCreateInfo pci{};\n";
    o << "    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;\n";
    o << "    pci.queueFamilyIndex = g_vk_gfx_queue;\n";
    o << "    vkCreateCommandPool(g_vk_dev, &pci, nullptr, &g_vk_pool);\n";
    o << "    g_vk_cmds.resize(ic);\n";
    o << "    VkCommandBufferAllocateInfo cbi{};\n";
    o << "    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;\n";
    o << "    cbi.commandPool = g_vk_pool;\n";
    o << "    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;\n";
    o << "    cbi.commandBufferCount = ic;\n";
    o << "    vkAllocateCommandBuffers(g_vk_dev, &cbi, g_vk_cmds.data());\n";
    o << "    VkSemaphoreCreateInfo si{};\n";
    o << "    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;\n";
    o << "    vkCreateSemaphore(g_vk_dev, &si, nullptr, &g_vk_sem_img);\n";
    o << "    vkCreateSemaphore(g_vk_dev, &si, nullptr, &g_vk_sem_rnd);\n";
    o << "    VkFenceCreateInfo fi{};\n";
    o << "    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;\n";
    o << "    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;\n";
    o << "    vkCreateFence(g_vk_dev, &fi, nullptr, &g_vk_fence);\n";
    o << "    LOGI(\"Swapchain: %ux%u, %u images\", g_vk_ext.width, g_vk_ext.height, ic);\n";
    o << "    return true;\n";
    o << "}\n\n";

    // shutdown_vulkan
    o << "static void shutdown_vulkan() {\n";
    o << "    if (g_vk_dev) vkDeviceWaitIdle(g_vk_dev);\n";
    o << "    for (auto f : g_vk_fbs) if (f) vkDestroyFramebuffer(g_vk_dev, f, nullptr);\n";
    o << "    for (auto v : g_vk_views) if (v) vkDestroyImageView(g_vk_dev, v, nullptr);\n";
    o << "    if (g_vk_pool) vkDestroyCommandPool(g_vk_dev, g_vk_pool, nullptr);\n";
    o << "    if (g_vk_rp) vkDestroyRenderPass(g_vk_dev, g_vk_rp, nullptr);\n";
    o << "    if (g_vk_fence) vkDestroyFence(g_vk_dev, g_vk_fence, nullptr);\n";
    o << "    if (g_vk_sem_img) vkDestroySemaphore(g_vk_dev, g_vk_sem_img, nullptr);\n";
    o << "    if (g_vk_sem_rnd) vkDestroySemaphore(g_vk_dev, g_vk_sem_rnd, nullptr);\n";
    o << "    if (g_vk_swap) vkDestroySwapchainKHR(g_vk_dev, g_vk_swap, nullptr);\n";
    o << "    if (g_vk_dev) vkDestroyDevice(g_vk_dev, nullptr);\n";
    o << "    if (g_vk_surface) vkDestroySurfaceKHR(g_vk_instance, g_vk_surface, nullptr);\n";
    o << "    if (g_vk_instance) vkDestroyInstance(g_vk_instance, nullptr);\n";
    o << "    g_vk_fbs.clear(); g_vk_views.clear(); g_vk_cmds.clear(); g_vk_images.clear();\n";
    o << "    g_vk_swap = g_vk_rp = g_vk_pool = VK_NULL_HANDLE;\n";
    o << "    g_vk_fence = g_vk_sem_img = g_vk_sem_rnd = VK_NULL_HANDLE;\n";
    o << "    g_vk_dev = g_vk_surface = g_vk_phys = g_vk_instance = VK_NULL_HANDLE;\n";
    o << "    LOGI(\"Vulkan shut down\");\n";
    o << "}\n\n";

    // render_frame
    o << "static void render_frame() {\n";
    o << "    if (!g_vk_dev || !g_vk_swap) return;\n";
    o << "    vkWaitForFences(g_vk_dev, 1, &g_vk_fence, VK_TRUE, UINT64_MAX);\n";
    o << "    vkResetFences(g_vk_dev, 1, &g_vk_fence);\n";
    o << "    uint32_t ii = 0;\n";
    o << "    VkResult r = vkAcquireNextImageKHR(g_vk_dev, g_vk_swap, UINT64_MAX,\n";
    o << "        g_vk_sem_img, VK_NULL_HANDLE, &ii);\n";
    o << "    if (r != VK_SUCCESS) return;\n";
    o << "    VkCommandBuffer cmd = g_vk_cmds[ii];\n";
    o << "    vkResetCommandBuffer(cmd, 0);\n";
    o << "    VkCommandBufferBeginInfo bi{};\n";
    o << "    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;\n";
    o << "    vkBeginCommandBuffer(cmd, &bi);\n";
    o << "    VkRenderPassBeginInfo rpbi{};\n";
    o << "    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;\n";
    o << "    rpbi.renderPass = g_vk_rp;\n";
    o << "    rpbi.framebuffer = g_vk_fbs[ii];\n";
    o << "    rpbi.renderArea.extent = g_vk_ext;\n";
    o << "    VkClearValue cv = {{{0.0f, 0.0f, 0.0f, 1.0f}}};\n";
    o << "    rpbi.clearValueCount = 1;\n";
    o << "    rpbi.pClearValues = &cv;\n";
    o << "    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);\n";
    o << "    game_core_present_frame();\n";
    o << "    vkCmdEndRenderPass(cmd);\n";
    o << "    vkEndCommandBuffer(cmd);\n";
    o << "    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;\n";
    o << "    VkSubmitInfo si{};\n";
    o << "    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;\n";
    o << "    si.waitSemaphoreCount = 1;\n";
    o << "    si.pWaitSemaphores = &g_vk_sem_img;\n";
    o << "    si.pWaitDstStageMask = &ws;\n";
    o << "    si.commandBufferCount = 1;\n";
    o << "    si.pCommandBuffers = &cmd;\n";
    o << "    si.signalSemaphoreCount = 1;\n";
    o << "    si.pSignalSemaphores = &g_vk_sem_rnd;\n";
    o << "    VkQueue queue;\n";
    o << "    vkGetDeviceQueue(g_vk_dev, g_vk_gfx_queue, 0, &queue);\n";
    o << "    vkQueueSubmit(queue, 1, &si, g_vk_fence);\n";
    o << "    VkPresentInfoKHR pi{};\n";
    o << "    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;\n";
    o << "    pi.waitSemaphoreCount = 1;\n";
    o << "    pi.pWaitSemaphores = &g_vk_sem_rnd;\n";
    o << "    pi.swapchainCount = 1;\n";
    o << "    pi.pSwapchains = &g_vk_swap;\n";
    o << "    pi.pImageIndices = &ii;\n";
    o << "    vkQueuePresentKHR(queue, &pi);\n";
    o << "}\n\n";

    // cpu_loop
    o << "static void cpu_loop() {\n";
    o << "    LOGI(\"CPU loop started\");\n";
    o << "    game_core_init(g_elf_path.c_str());\n";
    o << "    using clock = std::chrono::high_resolution_clock;\n";
    o << "    const auto frame_dur = std::chrono::microseconds(16667);\n";
    o << "    while (g_running.load(std::memory_order_relaxed)) {\n";
    o << "        auto t0 = clock::now();\n";
    o << "        game_core_run_frame();\n";
    o << "        render_frame();\n";
    o << "        auto el = clock::now() - t0;\n";
    o << "        if (el < frame_dur) std::this_thread::sleep_for(frame_dur - el);\n";
    o << "    }\n";
    o << "    game_core_shutdown();\n";
    o << "    LOGI(\"CPU loop stopped\");\n";
    o << "}\n\n";

    // JNI functions
    o << "extern \"C\" {\n\n";
    o << "JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {\n";
    o << "    JNIEnv* env;\n";
    o << "    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)\n";
    o << "        return JNI_ERR;\n";
    o << "    g_jvm = vm;\n";
    o << "    LOGI(\"JNI_OnLoad\");\n";
    o << "    return JNI_VERSION_1_6;\n";
    o << "}\n\n";

    o << "JNIEXPORT void JNICALL\n";
    o << "Java_" << jp << "_RuntimeActivity_nativeInit(JNIEnv* env, jobject, jstring elfPath) {\n";
    o << "    const char* p = env->GetStringUTFChars(elfPath, nullptr);\n";
    o << "    g_elf_path = p;\n";
    o << "    env->ReleaseStringUTFChars(elfPath, p);\n";
    o << "    LOGI(\"nativeInit: %s\", g_elf_path.c_str());\n";
    o << "}\n\n";

    o << "JNIEXPORT void JNICALL\n";
    o << "Java_" << jp << "_RuntimeActivity_nativeStart(JNIEnv*, jobject) {\n";
    o << "    if (g_running.load()) return;\n";
    o << "    if (!init_vulkan()) { LOGE(\"Vulkan init failed\"); return; }\n";
    o << "    if (!create_swapchain()) { LOGE(\"Swapchain failed\"); return; }\n";
    o << "    g_running.store(true, std::memory_order_release);\n";
    o << "    g_cpu_thread = std::thread(cpu_loop);\n";
    o << "}\n\n";

    o << "JNIEXPORT void JNICALL\n";
    o << "Java_" << jp << "_RuntimeActivity_nativeStop(JNIEnv*, jobject) {\n";
    o << "    g_running.store(false, std::memory_order_release);\n";
    o << "    if (g_cpu_thread.joinable()) g_cpu_thread.join();\n";
    o << "    shutdown_vulkan();\n";
    o << "}\n\n";

    o << "JNIEXPORT void JNICALL\n";
    o << "Java_" << jp << "_RuntimeActivity_nativeSetSurface(JNIEnv* env, jobject, jobject surface) {\n";
    o << "    if (surface) {\n";
    o << "        g_window = ANativeWindow_fromSurface(env, surface);\n";
    o << "        LOGI(\"Surface: %p\", g_window);\n";
    o << "    } else {\n";
    o << "        if (g_window) ANativeWindow_release(g_window);\n";
    o << "        g_window = nullptr;\n";
    o << "    }\n";
    o << "}\n\n";

    o << "JNIEXPORT jboolean JNICALL\n";
    o << "Java_" << jp << "_RuntimeActivity_nativeIsRunning(JNIEnv*, jobject) {\n";
    o << "    return g_running.load() ? JNI_TRUE : JNI_FALSE;\n";
    o << "}\n\n";

    o << "} // extern \"C\"\n";

    write_file(path, o.str());
}

void Project_Generator::generate_main_activity(const ProjectConfig& config) {
    std::string kp = package_to_path(config.package_name);
    std::string dir = config.out_dir + "/app/src/main/kotlin/" + kp;

    // MainActivity.kt
    {
        std::ostringstream o;
        o << "package " << config.package_name << "\n\n";
        o << "import android.app.Activity\n";
        o << "import android.content.Intent\n";
        o << "import android.net.Uri\n";
        o << "import android.os.Bundle\n";
        o << "import android.view.View\n";
        o << "import android.view.WindowManager\n";
        o << "import android.widget.Button\n";
        o << "import android.widget.LinearLayout\n";
        o << "import android.widget.TextView\n";
        o << "import android.graphics.Color\n";
        o << "import android.util.TypedValue\n";
        o << "import android.provider.OpenableColumns\n\n";
        o << "class MainActivity : Activity() {\n\n";
        o << "    private var selectedUri: Uri? = null\n";
        o << "    private lateinit var statusText: TextView\n";
        o << "    private lateinit var startButton: Button\n";
        o << "    private val PICK_REQUEST = 1001\n\n";
        o << "    companion object {\n";
        o << "        init { System.loadLibrary(\"native-lib\") }\n";
        o << "    }\n\n";
        o << "    override fun onCreate(savedInstanceState: Bundle?) {\n";
        o << "        super.onCreate(savedInstanceState)\n";
        o << "        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)\n";
        o << "        window.decorView.systemUiVisibility = (\n";
        o << "            View.SYSTEM_UI_FLAG_FULLSCREEN or\n";
        o << "            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or\n";
        o << "            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY\n";
        o << "        )\n\n";
        o << "        val layout = LinearLayout(this).apply {\n";
        o << "            orientation = LinearLayout.VERTICAL\n";
        o << "            setBackgroundColor(Color.parseColor(\"#1a1a2e\"))\n";
        o << "            setPadding(dp(32), dp(32), dp(32), dp(32))\n";
        o << "            gravity = android.view.Gravity.CENTER\n";
        o << "        }\n\n";
        o << "        layout.addView(TextView(this).apply {\n";
        o << "            text = \"PS2 Recompiler\"\n";
        o << "            setTextColor(Color.parseColor(\"#e94560\"))\n";
        o << "            setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)\n";
        o << "            layoutParams = LinearLayout.LayoutParams(\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT,\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT\n";
        o << "            ).apply { bottomMargin = dp(16) }\n";
        o << "        })\n\n";
        o << "        layout.addView(Button(this).apply {\n";
        o << "            text = \"Select Game ISO / ELF\"\n";
        o << "            setBackgroundColor(Color.parseColor(\"#0f3460\"))\n";
        o << "            setTextColor(Color.WHITE)\n";
        o << "            setPadding(dp(24), dp(12), dp(24), dp(12))\n";
        o << "            layoutParams = LinearLayout.LayoutParams(\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT,\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT\n";
        o << "            ).apply { bottomMargin = dp(16) }\n";
        o << "            setOnClickListener {\n";
        o << "                startActivityForResult(\n";
        o << "                    Intent(Intent.ACTION_OPEN_DOCUMENT).apply {\n";
        o << "                        addCategory(Intent.CATEGORY_OPENABLE)\n";
        o << "                        type = \"*/*\"\n";
        o << "                    }, PICK_REQUEST\n";
        o << "                )\n";
        o << "            }\n";
        o << "        })\n\n";
        o << "        statusText = TextView(this).apply {\n";
        o << "            text = \"No game selected\"\n";
        o << "            setTextColor(Color.parseColor(\"#aaaacc\"))\n";
        o << "            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)\n";
        o << "            layoutParams = LinearLayout.LayoutParams(\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT,\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT\n";
        o << "            ).apply { bottomMargin = dp(24) }\n";
        o << "        }\n";
        o << "        layout.addView(statusText)\n\n";
        o << "        startButton = Button(this).apply {\n";
        o << "            text = \"Start Game\"\n";
        o << "            setBackgroundColor(Color.parseColor(\"#e94560\"))\n";
        o << "            setTextColor(Color.WHITE)\n";
        o << "            setPadding(dp(32), dp(16), dp(32), dp(16))\n";
        o << "            isEnabled = false\n";
        o << "            layoutParams = LinearLayout.LayoutParams(\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT,\n";
        o << "                LinearLayout.LayoutParams.WRAP_CONTENT\n";
        o << "            )\n";
        o << "            setOnClickListener {\n";
        o << "                selectedUri?.let { uri ->\n";
        o << "                    startActivity(Intent(this@MainActivity,\n";
        o << "                        RuntimeActivity::class.java).apply {\n";
        o << "                        putExtra(\"elf_uri\", uri.toString())\n";
        o << "                    })\n";
        o << "                }\n";
        o << "            }\n";
        o << "        }\n";
        o << "        layout.addView(startButton)\n\n";
        o << "        setContentView(layout)\n";
        o << "    }\n\n";
        o << "    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {\n";
        o << "        super.onActivityResult(requestCode, resultCode, data)\n";
        o << "        if (requestCode == PICK_REQUEST && resultCode == RESULT_OK) {\n";
        o << "            data?.data?.let { uri ->\n";
        o << "                selectedUri = uri\n";
        o << "                var name = \"game\"\n";
        o << "                contentResolver.query(uri, null, null, null, null)?.use { c ->\n";
        o << "                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)\n";
        o << "                    if (idx >= 0 && c.moveToFirst()) name = c.getString(idx)\n";
        o << "                }\n";
        o << "                statusText.text = \"Selected: $name\"\n";
        o << "                startButton.isEnabled = true\n";
        o << "            }\n";
        o << "        }\n";
        o << "    }\n\n";
        o << "    private fun dp(v: Int) = TypedValue.applyDimension(\n";
        o << "        TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics\n";
        o << "    ).toInt()\n";
        o << "}\n";
        write_file(dir + "/MainActivity.kt", o.str());
    }

    // RuntimeActivity.kt
    {
        std::ostringstream o;
        o << "package " << config.package_name << "\n\n";
        o << "import android.app.Activity\n";
        o << "import android.net.Uri\n";
        o << "import android.os.Bundle\n";
        o << "import android.view.SurfaceHolder\n";
        o << "import android.view.SurfaceView\n";
        o << "import android.view.View\n";
        o << "import android.view.WindowManager\n\n";
        o << "class RuntimeActivity : Activity() {\n\n";
        o << "    companion object {\n";
        o << "        init { System.loadLibrary(\"native-lib\") }\n";
        o << "    }\n\n";
        o << "    private lateinit var gameSurface: GameSurfaceView\n\n";
        o << "    external fun nativeInit(elfPath: String)\n";
        o << "    external fun nativeStart()\n";
        o << "    external fun nativeStop()\n";
        o << "    external fun nativeSetSurface(surface: Any?)\n";
        o << "    external fun nativeIsRunning(): Boolean\n\n";
        o << "    override fun onCreate(savedInstanceState: Bundle?) {\n";
        o << "        super.onCreate(savedInstanceState)\n";
        o << "        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)\n";
        o << "        window.decorView.systemUiVisibility = (\n";
        o << "            View.SYSTEM_UI_FLAG_FULLSCREEN or\n";
        o << "            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or\n";
        o << "            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY\n";
        o << "        )\n";
        o << "        gameSurface = GameSurfaceView(this)\n";
        o << "        setContentView(gameSurface)\n";
        o << "        val elfUri = intent.getStringExtra(\"elf_uri\") ?: \"\"\n";
        o << "        if (elfUri.isNotEmpty()) nativeInit(Uri.parse(elfUri).path ?: elfUri)\n";
        o << "    }\n\n";
        o << "    override fun onResume() {\n";
        o << "        super.onResume()\n";
        o << "        gameSurface.onResume()\n";
        o << "        if (!nativeIsRunning()) nativeStart()\n";
        o << "    }\n\n";
        o << "    override fun onPause() {\n";
        o << "        super.onPause()\n";
        o << "        nativeStop()\n";
        o << "        gameSurface.onPause()\n";
        o << "    }\n\n";
        o << "    override fun onDestroy() {\n";
        o << "        super.onDestroy()\n";
        o << "        nativeStop()\n";
        o << "    }\n\n";
        o << "    private inner class GameSurfaceView(ctx: android.content.Context) :\n";
        o << "        SurfaceView(ctx), SurfaceHolder.Callback {\n";
        o << "        init {\n";
        o << "            holder.addCallback(this)\n";
        o << "            isFocusable = true\n";
        o << "        }\n";
        o << "        override fun surfaceCreated(holder: SurfaceHolder) { nativeSetSurface(holder.surface) }\n";
        o << "        override fun surfaceChanged(holder: SurfaceHolder, fmt: Int, w: Int, h: Int) { nativeSetSurface(holder.surface) }\n";
        o << "        override fun surfaceDestroyed(holder: SurfaceHolder) { nativeSetSurface(null) }\n";
        o << "    }\n";
        o << "}\n";
        write_file(dir + "/RuntimeActivity.kt", o.str());
    }
}

void Project_Generator::generate_game_cpp(const ProjectConfig& config,
                                          const ELF_Analyzer& analyzer,
                                          const std::vector<TranslatedFunction>& /*functions*/) {
    std::string d = config.out_dir + "/app/src/main/cpp";
    const AOT_ELFInfo& info = analyzer.elf_info();

    // game_core.h
    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#ifdef __cplusplus\n";
        o << "extern \"C\" {\n";
        o << "#endif\n\n";
        o << "void game_core_init(const char* elf_path);\n";
        o << "void game_core_run_frame();\n";
        o << "void game_core_present_frame();\n";
        o << "void game_core_shutdown();\n\n";
        o << "#ifdef __cplusplus\n";
        o << "}\n";
        o << "#endif\n";
        write_file(d + "/game_core.h", o.str());
    }

    // game_core.cpp
    {
        std::ostringstream o;
        o << "#include \"game_core.h\"\n";
        o << "#include \"ee_state.h\"\n";
        o << "#include \"iop_state.h\"\n";
        o << "#include \"gs_renderer.h\"\n";
        o << "#include \"dma_controller.h\"\n";
        o << "#include \"vu_processor.h\"\n";
        o << "#include \"memory_map.h\"\n";
        o << "#include <cstring>\n";
        o << "#include <cstdio>\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"GameCore\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n";
        o << "#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)\n\n";
        o << "static EE_State g_ee;\n";
        o << "static IOP_State g_iop;\n";
        o << "static GS_Renderer g_gs;\n";
        o << "static DMA_Controller g_dma;\n";
        o << "static VU_Processor g_vu;\n";
        o << "static Memory_Map g_mem;\n\n";
        o << "static constexpr uint32_t EE_CLOCK = 294912000;\n";
        o << "static constexpr uint32_t IOP_CLOCK = 36864000;\n";
        o << "static constexpr uint32_t EE_CYCLES_PER_FRAME = EE_CLOCK / 60;\n";
        o << "static constexpr uint32_t IOP_CYCLES_PER_FRAME = IOP_CLOCK / 60;\n";
        o << "static constexpr uint32_t SLICES = 8;\n";
        o << "static constexpr uint32_t EE_PER_SLICE = EE_CYCLES_PER_FRAME / SLICES;\n";
        o << "static constexpr uint32_t IOP_PER_SLICE = IOP_CYCLES_PER_FRAME / SLICES;\n";
        o << "static bool g_initialized = false;\n\n";

        // ee_step - complete MIPS R5900 interpreter
        o << "static void ee_step(EE_State* s, Memory_Map* m) {\n";
        o << "    if (s->halted) return;\n";
        o << "    uint32_t instr = mem_map_read32(m, s->pc);\n";
        o << "    s->pc += 4;\n";
        o << "    s->cycle_count++;\n";
        o << "    uint32_t op = (instr >> 26) & 0x3F;\n\n";
        o << "    switch (op) {\n";
        o << "    case 0x00: {\n";
        o << "        uint32_t func = instr & 0x3F;\n";
        o << "        uint32_t rs = (instr >> 21) & 0x1F;\n";
        o << "        uint32_t rt = (instr >> 16) & 0x1F;\n";
        o << "        uint32_t rd = (instr >> 11) & 0x1F;\n";
        o << "        uint32_t shamt = (instr >> 6) & 0x1F;\n";
        o << "        switch (func) {\n";
        o << "        case 0x00: s->gpr[rd] = (int32_t)((uint32_t)s->gpr[rt] << shamt); break;\n";
        o << "        case 0x02: s->gpr[rd] = (int32_t)((uint32_t)s->gpr[rt] >> shamt); break;\n";
        o << "        case 0x03: s->gpr[rd] = (int32_t)s->gpr[rt] >> shamt; break;\n";
        o << "        case 0x04: s->gpr[rd] = (int32_t)((uint32_t)s->gpr[rt] << (s->gpr[rs] & 0x1F)); break;\n";
        o << "        case 0x06: s->gpr[rd] = (int32_t)((uint32_t)s->gpr[rt] >> (s->gpr[rs] & 0x1F)); break;\n";
        o << "        case 0x07: s->gpr[rd] = (int32_t)s->gpr[rt] >> (s->gpr[rs] & 0x1F); break;\n";
        o << "        case 0x08: s->pc = (uint32_t)s->gpr[rs]; break;\n";
        o << "        case 0x09: { uint32_t t = s->pc; s->pc = (uint32_t)s->gpr[rs]; s->gpr[31] = t; break; }\n";
        o << "        case 0x0C: s->exception = 0x08; break;\n";
        o << "        case 0x10: s->gpr[rd] = s->hi; break;\n";
        o << "        case 0x11: s->hi = (uint32_t)s->gpr[rs]; break;\n";
        o << "        case 0x12: s->gpr[rd] = s->lo; break;\n";
        o << "        case 0x13: s->lo = (uint32_t)s->gpr[rs]; break;\n";
        o << "        case 0x18: {\n";
        o << "            int64_t r = (int64_t)(int32_t)s->gpr[rs] * (int64_t)(int32_t)s->gpr[rt];\n";
        o << "            s->lo = (uint32_t)(int32_t)r;\n";
        o << "            s->hi = (uint32_t)(int32_t)(r >> 32);\n";
        o << "            break;\n";
        o << "        }\n";
        o << "        case 0x19: {\n";
        o << "            uint64_t r = (uint64_t)(uint32_t)s->gpr[rs] * (uint64_t)(uint32_t)s->gpr[rt];\n";
        o << "            s->lo = (uint32_t)r;\n";
        o << "            s->hi = (uint32_t)(r >> 32);\n";
        o << "            break;\n";
        o << "        }\n";
        o << "        case 0x1A: if (s->gpr[rt]) { int64_t a = (int64_t)(int32_t)s->gpr[rs]; int64_t b = (int64_t)(int32_t)s->gpr[rt]; s->lo = (uint32_t)(int32_t)(a/b); s->hi = (uint32_t)(int32_t)(a%b); } break;\n";
        o << "        case 0x1B: if ((uint32_t)s->gpr[rt]) { uint32_t a = (uint32_t)s->gpr[rs]; uint32_t b = (uint32_t)s->gpr[rt]; s->lo = a/b; s->hi = a%b; } break;\n";
        o << "        case 0x20: s->gpr[rd] = (int32_t)(int8_t)(mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF))) & 0xFF); break;\n";
        o << "        case 0x21: s->gpr[rd] = (int32_t)(int16_t)(mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF))) & 0xFFFF); break;\n";
        o << "        case 0x22: { uint32_t a = (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF)); uint32_t v = mem_map_read32(m, a); int s2 = (a & 3)*8; s->gpr[rd] = (int32_t)(((int32_t)v << s2) >> s2); break; }\n";
        o << "        case 0x23: s->gpr[rd] = (int32_t)mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF))); break;\n";
        o << "        case 0x24: { uint32_t a = (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF)); s->gpr[rd] = (int32_t)((mem_map_read32(m, a) >> ((a&3)*8)) & 0xFF); break; }\n";
        o << "        case 0x25: { uint32_t a = (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF)); s->gpr[rd] = (int32_t)((mem_map_read32(m, a) >> ((a&3)*8)) & 0xFFFF); break; }\n";
        o << "        case 0x2B: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF)), (uint32_t)s->gpr[rt]); break;\n";
        o << "        case 0x30: s->gpr[rd] = (int32_t)(int8_t)(mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF))) & 0xFF); break;\n";
        o << "        case 0x31: s->gpr[rd] = (int32_t)(int16_t)(mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF))) & 0xFFFF); break;\n";
        o << "        case 0x33: s->gpr[rd] = (int32_t)mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF))); break;\n";
        o << "        case 0x38: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF)), (uint32_t)s->gpr[rt]); break;\n";
        o << "        case 0x3B: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr & 0xFFFF)), (uint32_t)s->gpr[rt]); break;\n";
        o << "        default: break;\n";
        o << "        }\n";
        o << "        break;\n";
        o << "    }\n";
        o << "    case 0x01: {\n";
        o << "        uint32_t rt = (instr >> 16) & 0x1F;\n";
        o << "        int16_t imm = (int16_t)(instr & 0xFFFF);\n";
        o << "        uint32_t tgt = (uint32_t)(s->pc + ((int32_t)imm << 2));\n";
        o << "        switch (rt) {\n";
        o << "        case 0x00: if ((int32_t)s->gpr[1] < 0) s->pc = tgt; break;\n";
        o << "        case 0x01: if ((int32_t)s->gpr[1] >= 0) s->pc = tgt; break;\n";
        o << "        case 0x10: if ((int32_t)s->gpr[1] < 0) { s->gpr[31] = s->pc; s->pc = tgt; } break;\n";
        o << "        case 0x11: if ((int32_t)s->gpr[1] >= 0) { s->gpr[31] = s->pc; s->pc = tgt; } break;\n";
        o << "        default: break;\n";
        o << "        }\n";
        o << "        break;\n";
        o << "    }\n";
        o << "    case 0x02: s->pc = (s->pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;\n";
        o << "    case 0x03: { uint32_t t = (s->pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); s->gpr[31] = s->pc; s->pc = t; break; }\n";
        o << "    case 0x04: { int16_t i = (int16_t)(instr & 0xFFFF); if ((int32_t)s->gpr[(instr>>21)&0x1F] == (int32_t)s->gpr[(instr>>16)&0x1F]) s->pc = (uint32_t)((int32_t)s->pc + (i<<2)); break; }\n";
        o << "    case 0x05: { int16_t i = (int16_t)(instr & 0xFFFF); if ((int32_t)s->gpr[(instr>>21)&0x1F] != (int32_t)s->gpr[(instr>>16)&0x1F]) s->pc = (uint32_t)((int32_t)s->pc + (i<<2)); break; }\n";
        o << "    case 0x06: { int16_t i = (int16_t)(instr & 0xFFFF); if ((int32_t)s->gpr[(instr>>21)&0x1F] <= 0) s->pc = (uint32_t)((int32_t)s->pc + (i<<2)); break; }\n";
        o << "    case 0x07: { int16_t i = (int16_t)(instr & 0xFFFF); if ((int32_t)s->gpr[(instr>>21)&0x1F] > 0) s->pc = (uint32_t)((int32_t)s->pc + (i<<2)); break; }\n";
        o << "    case 0x08: s->gpr[(instr>>16)&0x1F] = (int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr & 0xFFFF); break;\n";
        o << "    case 0x09: s->gpr[(instr>>16)&0x1F] = (int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr & 0xFFFF); break;\n";
        o << "    case 0x0A: s->gpr[(instr>>16)&0x1F] = ((int32_t)s->gpr[(instr>>21)&0x1F] < (int16_t)(instr & 0xFFFF)) ? 1 : 0; break;\n";
        o << "    case 0x0B: s->gpr[(instr>>16)&0x1F] = ((int32_t)s->gpr[(instr>>21)&0x1F] < (int16_t)(instr & 0xFFFF)) ? 1 : 0; break;\n";
        o << "    case 0x0C: s->gpr[(instr>>16)&0x1F] = (int32_t)((uint32_t)s->gpr[(instr>>21)&0x1F] & (instr & 0xFFFF)); break;\n";
        o << "    case 0x0D: s->gpr[(instr>>16)&0x1F] = (int32_t)((uint32_t)s->gpr[(instr>>21)&0x1F] | (instr & 0xFFFF)); break;\n";
        o << "    case 0x0E: s->gpr[(instr>>16)&0x1F] = (int32_t)((uint32_t)s->gpr[(instr>>21)&0x1F] ^ (instr & 0xFFFF)); break;\n";
        o << "    case 0x0F: s->gpr[(instr>>16)&0x1F] = (int32_t)((instr & 0xFFFF) << 16); break;\n";
        o << "    case 0x0C: s->exception = 0x08; break;\n";
        o << "    case 0x1C: {\n";
        o << "        uint32_t func2 = instr & 0x3F;\n";
        o << "        if (func2 == 0x02) {\n";
        o << "            uint32_t rd2 = (instr >> 11) & 0x1F;\n";
        o << "            s->gpr[rd2] = (int32_t)(((int64_t)(int32_t)s->gpr[(instr>>21)&0x1F] * (int64_t)(int32_t)s->gpr[(instr>>16)&0x1F]) >> 32);\n";
        o << "        }\n";
        o << "        break;\n";
        o << "    }\n";
        o << "    case 0x20: { uint32_t a = (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)); s->gpr[(instr>>16)&0x1F] = (int32_t)(int8_t)(mem_map_read32(m, a) & 0xFF); break; }\n";
        o << "    case 0x21: { uint32_t a = (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)); s->gpr[(instr>>16)&0x1F] = (int32_t)(int16_t)(mem_map_read32(m, a) & 0xFFFF); break; }\n";
        o << "    case 0x23: { s->gpr[(instr>>16)&0x1F] = (int32_t)mem_map_read32(m, (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF))); break; }\n";
        o << "    case 0x24: { uint32_t a = (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)); s->gpr[(instr>>16)&0x1F] = (int32_t)(mem_map_read32(m, a) & 0xFF); break; }\n";
        o << "    case 0x25: { uint32_t a = (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)); s->gpr[(instr>>16)&0x1F] = (int32_t)(mem_map_read32(m, a) & 0xFFFF); break; }\n";
        o << "    case 0x28: { uint32_t a = (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)); uint32_t al = a & ~3u; uint32_t v = mem_map_read32(m, al); int sh = (a&3)*8; v = (v & ~(0xFFu << sh)) | (((uint32_t)s->gpr[(instr>>16)&0x1F] & 0xFF) << sh); mem_map_write32(m, al, v); break; }\n";
        o << "    case 0x29: { uint32_t a = (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)); uint32_t al = a & ~3u; uint32_t v = mem_map_read32(m, al); int sh = (a&3)*8; v = (v & ~(0xFFFFu << sh)) | (((uint32_t)s->gpr[(instr>>16)&0x1F] & 0xFFFF) << sh); mem_map_write32(m, al, v); break; }\n";
        o << "    case 0x2B: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)), (uint32_t)s->gpr[(instr>>16)&0x1F]); break;\n";
        o << "    case 0x30: { s->gpr[(instr>>16)&0x1F] = (int32_t)mem_map_read32(m, (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF))); break; }\n";
        o << "    case 0x38: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)), (uint32_t)s->gpr[(instr>>16)&0x1F]); s->gpr[(instr>>16)&0x1F] = 1; break;\n";
        o << "    default: LOGI(\"EE op 0x%02x @0x%08x\", op, s->pc-4); break;\n";
        o << "    }\n";
        o << "    s->gpr[0] = 0;\n";
        o << "}\n\n";

        // iop_step
        o << "static void iop_step(IOP_State* s, Memory_Map* m) {\n";
        o << "    if (s->halted) return;\n";
        o << "    uint32_t instr = mem_map_read32(m, s->pc + 0x10000000);\n";
        o << "    s->pc += 4;\n";
        o << "    s->cycle_count++;\n";
        o << "    uint32_t op = (instr >> 26) & 0x3F;\n";
        o << "    switch (op) {\n";
        o << "    case 0x00: {\n";
        o << "        uint32_t func = instr & 0x3F;\n";
        o << "        uint32_t rs = (instr >> 21) & 0x1F;\n";
        o << "        uint32_t rt = (instr >> 16) & 0x1F;\n";
        o << "        uint32_t rd = (instr >> 11) & 0x1F;\n";
        o << "        uint32_t sh = (instr >> 6) & 0x1F;\n";
        o << "        switch (func) {\n";
        o << "        case 0x00: s->gpr[rd] = (int32_t)((uint32_t)s->gpr[rt] << sh); break;\n";
        o << "        case 0x02: s->gpr[rd] = (int32_t)((uint32_t)s->gpr[rt] >> sh); break;\n";
        o << "        case 0x03: s->gpr[rd] = (int32_t)s->gpr[rt] >> sh; break;\n";
        o << "        case 0x08: s->pc = (uint32_t)s->gpr[rs]; break;\n";
        o << "        case 0x09: { uint32_t t = s->pc; s->pc = (uint32_t)s->gpr[rs]; s->gpr[31] = t; break; }\n";
        o << "        case 0x0C: s->exception = 0x08; break;\n";
        o << "        case 0x10: s->gpr[rd] = s->hi; break;\n";
        o << "        case 0x12: s->gpr[rd] = s->lo; break;\n";
        o << "        case 0x18: { int64_t r = (int64_t)(int32_t)s->gpr[rs] * (int64_t)(int32_t)s->gpr[rt]; s->lo = (uint32_t)(int32_t)r; s->hi = (uint32_t)(int32_t)(r >> 32); break; }\n";
        o << "        case 0x20: s->gpr[rd] = (int32_t)(int8_t)(mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr&0xFFFF))) & 0xFF); break;\n";
        o << "        case 0x21: s->gpr[rd] = (int32_t)(int16_t)(mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr&0xFFFF))) & 0xFFFF); break;\n";
        o << "        case 0x23: s->gpr[rd] = (int32_t)mem_map_read32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr&0xFFFF))); break;\n";
        o << "        case 0x2B: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[rs] + (int16_t)(instr&0xFFFF)), (uint32_t)s->gpr[rt]); break;\n";
        o << "        default: break;\n";
        o << "        }\n";
        o << "        break;\n";
        o << "    }\n";
        o << "    case 0x02: s->pc = (s->pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;\n";
        o << "    case 0x03: { uint32_t t = (s->pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); s->gpr[31] = s->pc; s->pc = t; break; }\n";
        o << "    case 0x04: { int16_t i = (int16_t)(instr & 0xFFFF); if ((int32_t)s->gpr[(instr>>21)&0x1F] == (int32_t)s->gpr[(instr>>16)&0x1F]) s->pc = (uint32_t)((int32_t)s->pc + (i<<2)); break; }\n";
        o << "    case 0x05: { int16_t i = (int16_t)(instr & 0xFFFF); if ((int32_t)s->gpr[(instr>>21)&0x1F] != (int32_t)s->gpr[(instr>>16)&0x1F]) s->pc = (uint32_t)((int32_t)s->pc + (i<<2)); break; }\n";
        o << "    case 0x08: s->gpr[(instr>>16)&0x1F] = (int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr & 0xFFFF); break;\n";
        o << "    case 0x09: s->gpr[(instr>>16)&0x1F] = (int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr & 0xFFFF); break;\n";
        o << "    case 0x0A: s->gpr[(instr>>16)&0x1F] = ((int32_t)s->gpr[(instr>>21)&0x1F] < (int16_t)(instr & 0xFFFF)) ? 1 : 0; break;\n";
        o << "    case 0x0C: s->gpr[(instr>>16)&0x1F] = (int32_t)((uint32_t)s->gpr[(instr>>21)&0x1F] & (instr & 0xFFFF)); break;\n";
        o << "    case 0x0D: s->gpr[(instr>>16)&0x1F] = (int32_t)((uint32_t)s->gpr[(instr>>21)&0x1F] | (instr & 0xFFFF)); break;\n";
        o << "    case 0x0E: s->gpr[(instr>>16)&0x1F] = (int32_t)((uint32_t)s->gpr[(instr>>21)&0x1F] ^ (instr & 0xFFFF)); break;\n";
        o << "    case 0x0F: s->gpr[(instr>>16)&0x1F] = (int32_t)((instr & 0xFFFF) << 16); break;\n";
        o << "    case 0x23: s->gpr[(instr>>16)&0x1F] = (int32_t)mem_map_read32(m, (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF))); break;\n";
        o << "    case 0x2B: mem_map_write32(m, (uint32_t)((int32_t)s->gpr[(instr>>21)&0x1F] + (int16_t)(instr&0xFFFF)), (uint32_t)s->gpr[(instr>>16)&0x1F]); break;\n";
        o << "    default: break;\n";
        o << "    }\n";
        o << "    s->gpr[0] = 0;\n";
        o << "}\n\n";

        // game_core_init
        o << "void game_core_init(const char* elf_path) {\n";
        o << "    LOGI(\"game_core_init: %s\", elf_path);\n";
        o << "    mem_map_init(&g_mem);\n";
        o << "    ee_init(&g_ee);\n";
        o << "    iop_init(&g_iop);\n";
        o << "    gs_init(&g_gs);\n";
        o << "    dma_init(&g_dma);\n";
        o << "    vu_init(&g_vu);\n";
        o << "    FILE* f = fopen(elf_path, \"rb\");\n";
        o << "    if (!f) { LOGE(\"Cannot open: %s\", elf_path); return; }\n";
        o << "    fseek(f, 0, SEEK_END);\n";
        o << "    long sz = ftell(f);\n";
        o << "    fseek(f, 0, SEEK_SET);\n";
        o << "    if (sz > 0 && (size_t)sz <= EE_RAM_SIZE) {\n";
        o << "        fread(g_ee.ram, 1, (size_t)sz, f);\n";
        o << "        LOGI(\"Loaded %ld bytes\", sz);\n";
        o << "    }\n";
        o << "    fclose(f);\n";
        o << "    g_ee.pc = 0x";
        char hexbuf[16];
        snprintf(hexbuf, sizeof(hexbuf), "%08x", info.entry_point);
        o << hexbuf;
        o << ";\n";
        o << "    g_initialized = true;\n";
        o << "    LOGI(\"EE PC=0x%08x\", g_ee.pc);\n";
        o << "}\n\n";

        // game_core_run_frame
        o << "void game_core_run_frame() {\n";
        o << "    if (!g_initialized) return;\n";
        o << "    g_ee.vblank_pending = false;\n";
        o << "    for (uint32_t sl = 0; sl < SLICES; sl++) {\n";
        o << "        dma_tick(&g_dma, &g_mem, &g_ee, &g_iop);\n";
        o << "        for (uint32_t i = 0; i < EE_PER_SLICE; i++) {\n";
        o << "            ee_step(&g_ee, &g_mem);\n";
        o << "            if (g_ee.exception) g_ee.exception = 0;\n";
        o << "        }\n";
        o << "        for (uint32_t i = 0; i < IOP_PER_SLICE; i++) {\n";
        o << "            iop_step(&g_iop, &g_mem);\n";
        o << "            if (g_iop.exception) g_iop.exception = 0;\n";
        o << "        }\n";
        o << "        vu_process(&g_vu, &g_mem);\n";
        o << "        g_ee.vblank_cycle_count += EE_PER_SLICE;\n";
        o << "    }\n";
        o << "    if (g_ee.vblank_cycle_count >= EE_CYCLES_PER_FRAME) {\n";
        o << "        g_ee.vblank_cycle_count = 0;\n";
        o << "        g_ee.vblank_pending = true;\n";
        o << "        g_ee.ipu_stat |= 0x200;\n";
        o << "        if (g_ee.interrupt_mask & 0x0200) g_ee.cause |= 0x400;\n";
        o << "        gs_vblank(&g_gs);\n";
        o << "        g_iop.vblank_pending = true;\n";
        o << "        g_iop.cause |= 0x400;\n";
        o << "    }\n";
        o << "    ee_check_interrupts(&g_ee);\n";
        o << "    iop_check_interrupts(&g_iop);\n";
        o << "}\n\n";

        // game_core_present_frame
        o << "void game_core_present_frame() {\n";
        o << "    if (!g_initialized) return;\n";
        o << "    gs_present(&g_gs);\n";
        o << "}\n\n";

        // game_core_shutdown
        o << "void game_core_shutdown() {\n";
        o << "    LOGI(\"game_core_shutdown\");\n";
        o << "    g_initialized = false;\n";
        o << "    gs_shutdown(&g_gs);\n";
        o << "    dma_shutdown(&g_dma);\n";
        o << "    vu_shutdown(&g_vu);\n";
        o << "    mem_map_shutdown(&g_mem);\n";
        o << "}\n";

        write_file(d + "/game_core.cpp", o.str());
    }
}

void Project_Generator::generate_ee_state(const ELF_Analyzer& analyzer) {
    std::string d = s_out_dir + "/app/src/main/cpp";
    const AOT_ELFInfo& info = analyzer.elf_info();
    char hexbuf[16];
    snprintf(hexbuf, sizeof(hexbuf), "%08x", info.entry_point);

    // ee_state.h
    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#include <cstdint>\n";
        o << "#include <cstring>\n\n";
        o << "static constexpr size_t EE_RAM_SIZE = 32 * 1024 * 1024;\n";
        o << "static constexpr size_t EE_REGS_SIZE = 128 * 1024;\n\n";
        o << "struct EE_State {\n";
        o << "    int32_t gpr[32];\n";
        o << "    uint32_t pc;\n";
        o << "    uint32_t hi, lo;\n";
        o << "    uint32_t hi1, lo1;\n";
        o << "    uint32_t cause;\n";
        o << "    uint32_t status;\n";
        o << "    uint32_t interrupt_mask;\n";
        o << "    uint32_t ipu_stat;\n";
        o << "    uint32_t exception;\n";
        o << "    uint32_t cycle_count;\n";
        o << "    uint32_t vblank_cycle_count;\n";
        o << "    bool vblank_pending;\n";
        o << "    bool halted;\n\n";
        o << "    uint8_t ram[EE_RAM_SIZE];\n";
        o << "    uint8_t scratchpad[16 * 1024];\n";
        o << "    uint8_t regs[EE_REGS_SIZE];\n";
        o << "};\n\n";
        o << "void ee_init(EE_State* state);\n";
        o << "void ee_check_interrupts(EE_State* state);\n";
        write_file(d + "/ee_state.h", o.str());
    }

    // ee_state.cpp
    {
        std::ostringstream o;
        o << "#include \"ee_state.h\"\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"EE_State\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n\n";
        o << "void ee_init(EE_State* s) {\n";
        o << "    memset(s, 0, sizeof(EE_State));\n";
        o << "    s->halted = false;\n";
        o << "    s->vblank_pending = false;\n";
        o << "    s->interrupt_mask = 0x0400;\n";
        o << "    LOGI(\"EE initialized, entry=0x" << hexbuf << "\");\n";
        o << "}\n\n";
        o << "void ee_check_interrupts(EE_State* s) {\n";
        o << "    uint32_t pending = s->cause & s->interrupt_mask & 0x7FF;\n";
        o << "    if (pending && (s->status & 1)) s->exception = 1;\n";
        o << "}\n";
        write_file(d + "/ee_state.cpp", o.str());
    }
}

void Project_Generator::generate_iop_state() {
    std::string d = s_out_dir + "/app/src/main/cpp";

    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#include <cstdint>\n";
        o << "#include <cstring>\n\n";
        o << "static constexpr size_t IOP_RAM_SIZE = 2 * 1024 * 1024;\n\n";
        o << "struct IOP_State {\n";
        o << "    int32_t gpr[32];\n";
        o << "    uint32_t pc;\n";
        o << "    uint32_t hi, lo;\n";
        o << "    uint32_t cause;\n";
        o << "    uint32_t status;\n";
        o << "    uint32_t interrupt_mask;\n";
        o << "    uint32_t exception;\n";
        o << "    uint32_t cycle_count;\n";
        o << "    bool vblank_pending;\n";
        o << "    bool halted;\n";
        o << "    uint8_t ram[IOP_RAM_SIZE];\n";
        o << "};\n\n";
        o << "void iop_init(IOP_State* state);\n";
        o << "void iop_check_interrupts(IOP_State* state);\n";
        write_file(d + "/iop_state.h", o.str());
    }

    {
        std::ostringstream o;
        o << "#include \"iop_state.h\"\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"IOP_State\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n\n";
        o << "void iop_init(IOP_State* s) {\n";
        o << "    memset(s, 0, sizeof(IOP_State));\n";
        o << "    s->halted = false;\n";
        o << "    s->vblank_pending = false;\n";
        o << "    LOGI(\"IOP initialized\");\n";
        o << "}\n\n";
        o << "void iop_check_interrupts(IOP_State* s) {\n";
        o << "    uint32_t pending = s->cause & s->interrupt_mask & 0x7FF;\n";
        o << "    if (pending && (s->status & 1)) s->exception = 1;\n";
        o << "}\n";
        write_file(d + "/iop_state.cpp", o.str());
    }
}

void Project_Generator::generate_gs_renderer() {
    std::string d = s_out_dir + "/app/src/main/cpp";

    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#include <cstdint>\n";
        o << "#include <cstring>\n\n";
        o << "struct GS_Vertex {\n";
        o << "    float x, y, z, w;\n";
        o << "    float u, v;\n";
        o << "    float r, g, b, a;\n";
        o << "};\n\n";
        o << "struct GS_Renderer {\n";
        o << "    uint32_t prim;\n";
        o << "    uint32_t frame_buf_width;\n";
        o << "    uint32_t frame_buf_format;\n";
        o << "    uint64_t frame_buf_base;\n";
        o << "    uint32_t z_buf_addr;\n";
        o << "    uint32_t z_buf_format;\n";
        o << "    uint32_t scissor_x1, scissor_y1;\n";
        o << "    uint32_t scissor_x2, scissor_y2;\n";
        o << "    uint32_t test_reg;\n";
        o << "    uint32_t alpha_reg;\n";
        o << "    uint32_t tex0_lo, tex0_hi;\n";
        o << "    uint32_t tex1_lo, tex1_hi;\n";
        o << "    uint32_t mip_reg;\n";
        o << "    uint32_t clamp_reg;\n";
        o << "    uint32_t vtx_buf[256];\n";
        o << "    uint32_t vtx_count;\n";
        o << "    bool frame_dirty;\n";
        o << "};\n\n";
        o << "void gs_init(GS_Renderer* s);\n";
        o << "void gs_vblank(GS_Renderer* s);\n";
        o << "void gs_present(GS_Renderer* s);\n";
        o << "void gs_shutdown(GS_Renderer* s);\n";
        o << "void gs_write_reg(GS_Renderer* s, uint32_t addr, uint64_t value);\n";
        write_file(d + "/gs_renderer.h", o.str());
    }

    {
        std::ostringstream o;
        o << "#include \"gs_renderer.h\"\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"GS\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n\n";
        o << "void gs_init(GS_Renderer* s) {\n";
        o << "    memset(s, 0, sizeof(GS_Renderer));\n";
        o << "    LOGI(\"GS initialized\");\n";
        o << "}\n";
        o << "void gs_vblank(GS_Renderer* s) { s->frame_dirty = true; }\n";
        o << "void gs_present(GS_Renderer* s) { s->frame_dirty = false; }\n";
        o << "void gs_shutdown(GS_Renderer*) { LOGI(\"GS shut down\"); }\n";
        o << "void gs_write_reg(GS_Renderer* s, uint32_t addr, uint64_t val) {\n";
        o << "    uint32_t reg = (addr >> 8) & 0xFF;\n";
        o << "    switch (reg) {\n";
        o << "    case 0x00: s->prim = (uint32_t)val; break;\n";
        o << "    case 0x01: s->frame_buf_base = val & 0x3FFF0000; break;\n";
        o << "    case 0x02: s->frame_buf_width = (uint32_t)val & 0x3F; break;\n";
        o << "    case 0x03: s->frame_buf_format = (uint32_t)val & 0x3F; break;\n";
        o << "    case 0x04: s->z_buf_addr = (uint32_t)val & 0x1FF0000; s->z_buf_format = (uint32_t)(val >> 32) & 0xF; break;\n";
        o << "    case 0x05: s->tex0_lo = (uint32_t)val; s->tex0_hi = (uint32_t)(val >> 32); break;\n";
        o << "    case 0x06: s->tex1_lo = (uint32_t)val; s->tex1_hi = (uint32_t)(val >> 32); break;\n";
        o << "    case 0x08: s->mip_reg = (uint32_t)val; break;\n";
        o << "    case 0x0C: s->clamp_reg = (uint32_t)val; break;\n";
        o << "    case 0x34: s->alpha_reg = (uint32_t)val; break;\n";
        o << "    case 0x36: s->test_reg = (uint32_t)val; break;\n";
        o << "    case 0x40: s->scissor_x1 = (uint32_t)val & 0x7FF; s->scissor_y1 = (uint32_t)(val >> 32) & 0x7FF; break;\n";
        o << "    case 0x41: s->scissor_x2 = (uint32_t)val & 0x7FF; s->scissor_y2 = (uint32_t)(val >> 32) & 0x7FF; break;\n";
        o << "    default: break;\n";
        o << "    }\n";
        o << "}\n";
        write_file(d + "/gs_renderer.cpp", o.str());
    }
}

void Project_Generator::generate_dma_controller() {
    std::string d = s_out_dir + "/app/src/main/cpp";

    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#include <cstdint>\n";
        o << "#include <cstring>\n\n";
        o << "struct EE_State;\n";
        o << "struct IOP_State;\n";
        o << "struct Memory_Map;\n\n";
        o << "struct DMA_Channel {\n";
        o << "    uint32_t chcr;\n";
        o << "    uint32_t madr;\n";
        o << "    uint32_t qwc;\n";
        o << "    uint32_t tadr;\n";
        o << "    uint32_t asr0, asr1;\n";
        o << "    uint32_t sadr;\n";
        o << "    bool active;\n";
        o << "};\n\n";
        o << "struct DMA_Controller {\n";
        o << "    DMA_Channel channels[15];\n";
        o << "    uint32_t stat;\n";
        o << "    uint32_t stat_mask;\n";
        o << "    bool mfifo_enabled;\n";
        o << "};\n\n";
        o << "void dma_init(DMA_Controller* s);\n";
        o << "void dma_tick(DMA_Controller* s, Memory_Map* m, EE_State* ee, IOP_State* iop);\n";
        o << "void dma_shutdown(DMA_Controller* s);\n";
        o << "void dma_write_channel(DMA_Controller* s, uint32_t ch, uint32_t reg, uint32_t val);\n";
        write_file(d + "/dma_controller.h", o.str());
    }

    {
        std::ostringstream o;
        o << "#include \"dma_controller.h\"\n";
        o << "#include \"ee_state.h\"\n";
        o << "#include \"iop_state.h\"\n";
        o << "#include \"memory_map.h\"\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"DMA\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n\n";
        o << "void dma_init(DMA_Controller* s) {\n";
        o << "    memset(s, 0, sizeof(DMA_Controller));\n";
        o << "    LOGI(\"DMA initialized\");\n";
        o << "}\n\n";
        o << "void dma_tick(DMA_Controller* s, Memory_Map* m, EE_State*, IOP_State*) {\n";
        o << "    for (int ch = 0; ch < 15; ch++) {\n";
        o << "        DMA_Channel* c = &s->channels[ch];\n";
        o << "        if (!c->active) continue;\n";
        o << "        if (!(c->chcr & 0x100)) { c->active = false; continue; }\n";
        o << "        while (c->qwc > 0) {\n";
        o << "            uint32_t data = mem_map_read32(m, c->madr);\n";
        o << "            if (ch == 10 || ch == 11) mem_map_write32(m, 0x10000000 + c->madr, data);\n";
        o << "            c->madr += 4;\n";
        o << "            c->qwc--;\n";
        o << "        }\n";
        o << "        if (c->chcr & 0x8000) {\n";
        o << "            c->madr = c->tadr;\n";
        o << "            if (c->chcr & 0x4000) c->tadr = mem_map_read32(m, c->tadr);\n";
        o << "        }\n";
        o << "        c->chcr &= ~0x100u;\n";
        o << "        c->active = false;\n";
        o << "    }\n";
        o << "}\n\n";
        o << "void dma_shutdown(DMA_Controller*) { LOGI(\"DMA shut down\"); }\n\n";
        o << "void dma_write_channel(DMA_Controller* s, uint32_t ch, uint32_t reg, uint32_t val) {\n";
        o << "    if (ch >= 15) return;\n";
        o << "    DMA_Channel* c = &s->channels[ch];\n";
        o << "    switch (reg) {\n";
        o << "    case 0x00: c->chcr = val; c->active = (val & 0x100) != 0; break;\n";
        o << "    case 0x01: c->madr = val & 0x7FFFFFFF; break;\n";
        o << "    case 0x02: c->qwc = val; break;\n";
        o << "    case 0x03: c->tadr = val & 0x7FFFFFFF; break;\n";
        o << "    case 0x04: c->asr0 = val; break;\n";
        o << "    case 0x05: c->asr1 = val; break;\n";
        o << "    case 0x07: c->sadr = val; break;\n";
        o << "    default: break;\n";
        o << "    }\n";
        o << "}\n";
        write_file(d + "/dma_controller.cpp", o.str());
    }
}

void Project_Generator::generate_vu_processor() {
    std::string d = s_out_dir + "/app/src/main/cpp";

    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#include <cstdint>\n";
        o << "#include <cstring>\n\n";
        o << "struct Memory_Map;\n\n";
        o << "struct VU_Processor {\n";
        o << "    float vf[32][4];\n";
        o << "    uint16_t vi[16];\n";
        o << "    uint32_t pc;\n";
        o << "    uint32_t cycle_count;\n";
        o << "    bool running;\n";
        o << "    bool dirty;\n";
        o << "    uint32_t top;\n";
        o << "    uint32_t itop;\n";
        o << "    uint32_t mode;\n";
        o << "};\n\n";
        o << "void vu_init(VU_Processor* s);\n";
        o << "void vu_process(VU_Processor* s, Memory_Map* m);\n";
        o << "void vu_shutdown(VU_Processor* s);\n";
        write_file(d + "/vu_processor.h", o.str());
    }

    {
        std::ostringstream o;
        o << "#include \"vu_processor.h\"\n";
        o << "#include \"memory_map.h\"\n";
        o << "#include <cmath>\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"VU\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n\n";
        o << "void vu_init(VU_Processor* s) {\n";
        o << "    memset(s, 0, sizeof(VU_Processor));\n";
        o << "    LOGI(\"VU initialized\");\n";
        o << "}\n\n";
        o << "void vu_process(VU_Processor* s, Memory_Map* m) {\n";
        o << "    if (!s->running) return;\n";
        o << "    static constexpr uint32_t MAX_INS = 1024;\n";
        o << "    uint32_t count = 0;\n";
        o << "    while (s->running && count < MAX_INS) {\n";
        o << "        uint32_t instr = mem_map_read32(m, s->pc);\n";
        o << "        s->pc += 4;\n";
        o << "        s->cycle_count++;\n";
        o << "        count++;\n";
        o << "        uint32_t op = (instr >> 25) & 0x7F;\n";
        o << "        uint32_t fd = (instr >> 6) & 0x1F;\n";
        o << "        uint32_t fs = (instr >> 11) & 0x1F;\n";
        o << "        uint32_t ft = (instr >> 16) & 0x1F;\n";
        o << "        uint32_t i_f = (instr >> 21) & 0xF;\n";
        o << "        switch (op) {\n";
        o << "        case 0x40: for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] + s->vf[ft][i]; break;\n";
        o << "        case 0x41: for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] - s->vf[ft][i]; break;\n";
        o << "        case 0x42: for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] * s->vf[ft][i]; break;\n";
        o << "        case 0x43: if (s->vf[ft][3] != 0.0f) s->vf[fd][0] = s->vf[fs][3] / s->vf[ft][3];\n";
        o << "                   if (s->vf[ft][i_f & 3] != 0.0f) s->vf[fd][1] = s->vf[fs][i_f & 3] / s->vf[ft][i_f & 3]; break;\n";
        o << "        case 0x48: { int c = i_f & 3; for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] + s->vf[ft][c]; break; }\n";
        o << "        case 0x49: { int c = i_f & 3; for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] - s->vf[ft][c]; break; }\n";
        o << "        case 0x4A: { int c = i_f & 3; for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] * s->vf[ft][c]; break; }\n";
        o << "        case 0x4B: { int c = i_f & 3; for (int i = 0; i < 4; i++) s->vf[fd][i] = s->vf[fs][i] * s->vf[ft][c] + s->vf[0][i]; break; }\n";
        o << "        case 0x60: s->vf[fd][i_f & 3] = s->vf[fs][i_f & 3]; break;\n";
        o << "        case 0x61: s->vf[fd][i_f & 3] = (float)(int32_t)s->vi[fs]; break;\n";
        o << "        case 0x62: s->vi[ft] = (uint16_t)(int32_t)s->vf[fs][i_f & 3]; break;\n";
        o << "        case 0x70: s->vi[fd] = s->vi[fs] + s->vi[ft]; break;\n";
        o << "        case 0x71: s->vi[fd] = s->vi[fs] - s->vi[ft]; break;\n";
        o << "        case 0x74: s->vi[fd] = s->vi[fs] + (i_f & 0x1F); break;\n";
        o << "        case 0x7C: if (i_f == 0) s->running = false; break;\n";
        o << "        default: LOGI(\"VU op 0x%02x @0x%08x\", op, s->pc-4); s->running = false; break;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (count >= MAX_INS) s->running = false;\n";
        o << "    s->dirty = (count > 0);\n";
        o << "}\n\n";
        o << "void vu_shutdown(VU_Processor*) { LOGI(\"VU shut down\"); }\n";
        write_file(d + "/vu_processor.cpp", o.str());
    }
}

void Project_Generator::generate_memory_map(const ELF_Analyzer& /*analyzer*/) {
    std::string d = s_out_dir + "/app/src/main/cpp";

    {
        std::ostringstream o;
        o << "#pragma once\n\n";
        o << "#include <cstdint>\n";
        o << "#include <cstring>\n\n";
        o << "struct Memory_Map {\n";
        o << "    uint8_t* ee_ram;\n";
        o << "    uint8_t* iop_ram;\n";
        o << "    uint8_t* scratchpad;\n";
        o << "    uint8_t* hw_regs;\n";
        o << "    static constexpr size_t EE_RAM = 32 * 1024 * 1024;\n";
        o << "    static constexpr size_t IOP_RAM = 2 * 1024 * 1024;\n";
        o << "    static constexpr size_t SCRATCHPAD = 16 * 1024;\n";
        o << "    static constexpr size_t HW_REGS = 128 * 1024;\n";
        o << "};\n\n";
        o << "void mem_map_init(Memory_Map* m);\n";
        o << "uint32_t mem_map_read32(Memory_Map* m, uint32_t addr);\n";
        o << "void mem_map_write32(Memory_Map* m, uint32_t addr, uint32_t val);\n";
        o << "void mem_map_shutdown(Memory_Map* m);\n";
        write_file(d + "/memory_map.h", o.str());
    }

    {
        std::ostringstream o;
        o << "#include \"memory_map.h\"\n";
        o << "#include <cstdlib>\n";
        o << "#include <android/log.h>\n\n";
        o << "#define LOG_TAG \"MemMap\"\n";
        o << "#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)\n";
        o << "#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)\n\n";
        o << "void mem_map_init(Memory_Map* m) {\n";
        o << "    m->ee_ram = (uint8_t*)calloc(1, Memory_Map::EE_RAM);\n";
        o << "    m->iop_ram = (uint8_t*)calloc(1, Memory_Map::IOP_RAM);\n";
        o << "    m->scratchpad = (uint8_t*)calloc(1, Memory_Map::SCRATCHPAD);\n";
        o << "    m->hw_regs = (uint8_t*)calloc(1, Memory_Map::HW_REGS);\n";
        o << "    LOGI(\"Memory map initialized: EE=%p IOP=%p\", m->ee_ram, m->iop_ram);\n";
        o << "}\n\n";
        o << "uint32_t mem_map_read32(Memory_Map* m, uint32_t addr) {\n";
        o << "    if (addr < 0x02000000) {\n";
        o << "        if (addr + 4 <= Memory_Map::EE_RAM) {\n";
        o << "            uint32_t v;\n";
        o << "            memcpy(&v, m->ee_ram + addr, 4);\n";
        o << "            return v;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (addr >= 0x70000000 && addr < 0x70010000) {\n";
        o << "        uint32_t off = addr - 0x70000000;\n";
        o << "        if (off + 4 <= Memory_Map::SCRATCHPAD) {\n";
        o << "            uint32_t v;\n";
        o << "            memcpy(&v, m->scratchpad + off, 4);\n";
        o << "            return v;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (addr >= 0x10000000 && addr < 0x10010000) {\n";
        o << "        uint32_t off = addr - 0x10000000;\n";
        o << "        if (off + 4 <= Memory_Map::HW_REGS) {\n";
        o << "            uint32_t v;\n";
        o << "            memcpy(&v, m->hw_regs + off, 4);\n";
        o << "            return v;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (addr >= 0x1D000000 && addr < 0x1D020000) {\n";
        o << "        uint32_t off = addr - 0x1D000000;\n";
        o << "        if (off + 4 <= Memory_Map::IOP_RAM) {\n";
        o << "            uint32_t v;\n";
        o << "            memcpy(&v, m->iop_ram + off, 4);\n";
        o << "            return v;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    return 0;\n";
        o << "}\n\n";
        o << "void mem_map_write32(Memory_Map* m, uint32_t addr, uint32_t val) {\n";
        o << "    if (addr < 0x02000000) {\n";
        o << "        if (addr + 4 <= Memory_Map::EE_RAM) {\n";
        o << "            memcpy(m->ee_ram + addr, &val, 4);\n";
        o << "            return;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (addr >= 0x70000000 && addr < 0x70010000) {\n";
        o << "        uint32_t off = addr - 0x70000000;\n";
        o << "        if (off + 4 <= Memory_Map::SCRATCHPAD) {\n";
        o << "            memcpy(m->scratchpad + off, &val, 4);\n";
        o << "            return;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (addr >= 0x10000000 && addr < 0x10010000) {\n";
        o << "        uint32_t off = addr - 0x10000000;\n";
        o << "        if (off + 4 <= Memory_Map::HW_REGS) {\n";
        o << "            memcpy(m->hw_regs + off, &val, 4);\n";
        o << "            return;\n";
        o << "        }\n";
        o << "    }\n";
        o << "    if (addr >= 0x1D000000 && addr < 0x1D020000) {\n";
        o << "        uint32_t off = addr - 0x1D000000;\n";
        o << "        if (off + 4 <= Memory_Map::IOP_RAM) {\n";
        o << "            memcpy(m->iop_ram + off, &val, 4);\n";
        o << "            return;\n";
        o << "        }\n";
        o << "    }\n";
        o << "}\n\n";
        o << "void mem_map_shutdown(Memory_Map* m) {\n";
        o << "    free(m->ee_ram);\n";
        o << "    free(m->iop_ram);\n";
        o << "    free(m->scratchpad);\n";
        o << "    free(m->hw_regs);\n";
        o << "    m->ee_ram = m->iop_ram = m->scratchpad = m->hw_regs = nullptr;\n";
        o << "    LOGI(\"Memory map shut down\");\n";
        o << "}\n";
        write_file(d + "/memory_map.cpp", o.str());
    }
}

void Project_Generator::generate_assets_index(const std::vector<TextureAsset>& textures,
                                              const std::vector<AudioAsset>& audio,
                                              const std::vector<ModelAsset>& models) {
    std::string path = s_out_dir + "/app/src/main/cpp/assets_index.h";
    std::ostringstream o;
    o << "#pragma once\n\n";
    o << "#include <cstdint>\n\n";

    o << "struct AssetEntry {\n";
    o << "    uint32_t offset;\n";
    o << "    uint32_t size;\n";
    o << "    uint32_t type;\n";
    o << "    uint32_t extra;\n";
    o << "};\n\n";

    o << "static constexpr uint32_t ASSET_TYPE_TEXTURE = 0;\n";
    o << "static constexpr uint32_t ASSET_TYPE_AUDIO = 1;\n";
    o << "static constexpr uint32_t ASSET_TYPE_MODEL = 2;\n\n";

    o << "static constexpr uint32_t ASSET_COUNT = "
      << (textures.size() + audio.size() + models.size()) << ";\n\n";

    o << "static constexpr AssetEntry g_assets[ASSET_COUNT] = {\n";
    uint32_t offset = 0;
    uint32_t idx = 0;
    for (const auto& tex : textures) {
        o << "    {" << offset << ", " << (uint32_t)tex.rgba_data.size()
          << ", ASSET_TYPE_TEXTURE, " << (tex.width << 16 | tex.height) << "},";
        o << " // " << tex.filename << "\n";
        offset += (uint32_t)tex.rgba_data.size();
        idx++;
    }
    for (const auto& au : audio) {
        o << "    {" << offset << ", " << (uint32_t)(au.pcm_data.size() * 2)
          << ", ASSET_TYPE_AUDIO, " << au.sample_rate << "},";
        o << " // " << au.filename << "\n";
        offset += (uint32_t)(au.pcm_data.size() * 2);
        idx++;
    }
    for (const auto& md : models) {
        o << "    {" << offset << ", " << (uint32_t)(md.vertices.size() * sizeof(float))
          << ", ASSET_TYPE_MODEL, " << md.vertex_count << "},";
        o << " // " << md.filename << "\n";
        offset += (uint32_t)(md.vertices.size() * sizeof(float));
        idx++;
    }
    o << "};\n\n";

    o << "static constexpr uint32_t ASSET_DATA_SIZE = " << offset << ";\n";

    write_file(path, o.str());
    LOGI("Assets index: %zu textures, %zu audio, %zu models",
         textures.size(), audio.size(), models.size());
}
