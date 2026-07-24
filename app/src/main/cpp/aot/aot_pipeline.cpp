#include "aot_pipeline.h"
#include <android/log.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

#define LOG_TAG "AOT_Pipeline"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr size_t GS_VRAM_OFFSET  = 0x02000000;
static constexpr size_t GS_VRAM_SIZE    = 0x00400000;
static constexpr size_t SPU2_RAM_OFFSET = 0x02400000;
static constexpr size_t SPU2_RAM_SIZE   = 0x00200000;

static void mkdirs_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
    }
}

AOT_Pipeline::AOT_Pipeline() = default;
AOT_Pipeline::~AOT_Pipeline() = default;

AOT_Result AOT_Pipeline::run(const char* game_path, const char* output_dir,
                             uint8_t* ee_ram, size_t ram_size,
                             ProgressCallback progress) {
    m_result = AOT_Result{};
    std::string out_dir(output_dir ? output_dir : "");

    LOGI("AOT Pipeline starting: game=%s output=%s ram_size=%zu",
         game_path ? game_path : "(null)", out_dir.c_str(), ram_size);

    mkdirs_p(out_dir);

    auto report = [&](int phase, const std::string& msg, int pct) {
        LOGI("Phase %d [%d%%]: %s", phase, pct, msg.c_str());
        if (progress) progress(phase, msg, pct);
    };

    // --- Phase 1: Analyze ELF ---
    report(1, "Analyzing ELF...", 0);
    if (!phase_1_analyze(game_path, ee_ram, ram_size)) {
        if (m_result.error.empty()) m_result.error = "Phase 1: ELF analysis failed";
        LOGE("Phase 1 failed: %s", m_result.error.c_str());
        return m_result;
    }
    report(1, "ELF analysis complete", 100);

    // --- Phase 2: Translate MIPS functions ---
    report(2, "Translating MIPS functions...", 0);
    std::vector<TranslatedFunction> functions;
    const auto& detected_funcs = m_analyzer.functions();

    if (!detected_funcs.empty() && !m_elf.code_data.empty()) {
        functions.reserve(detected_funcs.size());
        uint32_t total = static_cast<uint32_t>(detected_funcs.size());

        for (uint32_t i = 0; i < total; i++) {
            const auto& func = detected_funcs[i];
            if (func.size == 0) continue;

            uint32_t offset = func.start - m_elf.code_start;
            if (offset + func.size > static_cast<uint32_t>(m_elf.code_data.size())) {
                LOGI("Function 0x%08X exceeds code bounds (offset=%u size=%u code_sz=%zu), skipping",
                     func.start, offset, func.size, m_elf.code_data.size());
                continue;
            }

            const uint8_t* code_ptr = m_elf.code_data.data() + offset;
            TranslatedFunction tf = m_translator.translate_function(
                func.start, code_ptr, func.size, m_analyzer, ee_ram);

            if (!tf.cpp_code.empty()) {
                functions.push_back(std::move(tf));
            }

            int pct = static_cast<int>((i + 1) * 100 / total);
            if (pct != static_cast<int>(i * 100 / total)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Translated %u/%u functions", i + 1, total);
                report(2, msg, pct);
            }
        }
    }

    m_result.functions_translated = static_cast<uint32_t>(functions.size());
    report(2, "Translation complete", 100);

    // --- Phase 3: Generate native hardware modules ---
    report(3, "Generating native hardware modules...", 0);
    std::string native_dir = out_dir + "/native";
    if (!phase_3_generate_native(m_analyzer, functions)) {
        if (m_result.error.empty()) m_result.error = "Phase 3: HW module generation failed";
        LOGE("Phase 3 failed: %s", m_result.error.c_str());
        return m_result;
    }
    report(3, "Native modules generated", 100);

    // --- Phase 4: Process assets ---
    report(4, "Processing assets...", 0);

    const uint8_t* vram_ptr = nullptr;
    size_t vram_size = 0;
    const uint8_t* spu2_ptr = nullptr;
    size_t spu2_size = 0;

    if (ram_size >= GS_VRAM_OFFSET + GS_VRAM_SIZE) {
        vram_ptr = ee_ram + GS_VRAM_OFFSET;
        vram_size = GS_VRAM_SIZE;
    } else {
        LOGI("RAM buffer (0x%zx) too small for VRAM region at 0x%zx, skipping VRAM extraction",
             ram_size, GS_VRAM_OFFSET);
    }

    if (ram_size >= SPU2_RAM_OFFSET + SPU2_RAM_SIZE) {
        spu2_ptr = ee_ram + SPU2_RAM_OFFSET;
        spu2_size = SPU2_RAM_SIZE;
    } else {
        LOGI("RAM buffer (0x%zx) too small for SPU2 region at 0x%zx, skipping SPU2 extraction",
             ram_size, SPU2_RAM_OFFSET);
    }

    if (!phase_4_process_assets(m_analyzer, vram_ptr, vram_size,
                                spu2_ptr, spu2_size, ee_ram, ram_size)) {
        if (m_result.error.empty()) m_result.error = "Phase 4: Asset processing failed";
        LOGE("Phase 4 failed: %s", m_result.error.c_str());
        return m_result;
    }
    report(4, "Assets processed", 100);

    // --- Phase 5: Generate Android project ---
    report(5, "Generating Android project...", 0);

    std::vector<TextureAsset> textures = m_asset_proc.extract_textures(vram_ptr, vram_size);
    std::vector<AudioAsset>   audio    = m_asset_proc.extract_audio(spu2_ptr, spu2_size);
    std::vector<ModelAsset>   models   = m_asset_proc.extract_models(ee_ram, ram_size);

    m_result.project_dir = out_dir;
    if (!phase_5_generate_project(m_analyzer, functions, textures, audio, models)) {
        if (m_result.error.empty()) m_result.error = "Phase 5: Project generation failed";
        LOGE("Phase 5 failed: %s", m_result.error.c_str());
        return m_result;
    }
    report(5, "Project generated", 100);

    // --- Phase 6: Build APK ---
    report(6, "Building APK...", 0);
    if (!phase_6_build_apk(out_dir)) {
        LOGI("Phase 6: APK build skipped or failed (non-fatal)");
    }
    report(6, "Build complete", 100);

    m_result.success = true;
    m_result.project_dir = out_dir;
    m_result.apk_path = out_dir + "/app/build/outputs/apk/debug/app-debug.apk";

    LOGI("AOT Pipeline completed successfully");
    LOGI("  Functions translated: %u", m_result.functions_translated);
    LOGI("  Textures extracted:   %u", m_result.textures_extracted);
    LOGI("  Audio clips:          %u", m_result.audio_clips_extracted);
    LOGI("  Models extracted:     %u", m_result.models_extracted);

    return m_result;
}

bool AOT_Pipeline::phase_1_analyze(const char* game_path, uint8_t* ee_ram, size_t ram_size) {
    LOGI("Phase 1: Analyzing ELF at %s", game_path ? game_path : "(null)");

    if (!game_path || !ee_ram || ram_size == 0) {
        m_result.error = "Phase 1: Invalid arguments (null path or ee_ram, or zero ram_size)";
        return false;
    }

    if (!m_analyzer.analyze(game_path, ee_ram, ram_size)) {
        m_result.error = "Phase 1: ELF_Analyzer::analyze() failed for " + std::string(game_path);
        return false;
    }

    m_elf = m_analyzer.elf_info();

    LOGI("Phase 1 complete:");
    LOGI("  Game ID:    %s", m_elf.game_id.c_str());
    LOGI("  Game Title: %s", m_elf.game_title.c_str());
    LOGI("  Entry:      0x%08X", m_elf.entry_point);
    LOGI("  Code:       0x%08X - 0x%08X (%zu bytes)",
         m_elf.code_start, m_elf.code_end, m_elf.code_data.size());
    LOGI("  Data:       0x%08X - 0x%08X (%zu bytes)",
         m_elf.data_start, m_elf.data_end, m_elf.data_data.size());
    LOGI("  Functions:  %zu", m_analyzer.functions().size());
    LOGI("  Syscalls:   %zu", m_analyzer.syscalls().size());
    LOGI("  IOP deps:   %zu", m_analyzer.iop_deps().size());

    return true;
}

bool AOT_Pipeline::phase_2_translate(const ELF_Analyzer& analyzer, uint8_t* ee_ram) {
    (void)analyzer;
    (void)ee_ram;
    LOGI("Phase 2: Translation handled inline by run()");
    return true;
}

bool AOT_Pipeline::phase_3_generate_native(const ELF_Analyzer& analyzer,
                                           const std::vector<TranslatedFunction>& functions) {
    LOGI("Phase 3: Generating native HW modules (%zu translated functions)",
         functions.size());

    std::string native_dir;
    if (!m_elf.game_id.empty()) {
        native_dir = m_elf.game_id;
        std::replace(native_dir.begin(), native_dir.end(), ' ', '_');
        std::replace(native_dir.begin(), native_dir.end(), '/', '_');
    } else {
        native_dir = "game";
    }

    m_hw_gen.generate_all(analyzer, native_dir);
    LOGI("Phase 3 complete: native modules in %s", native_dir.c_str());
    return true;
}

bool AOT_Pipeline::phase_4_process_assets(const ELF_Analyzer& analyzer,
                                           const uint8_t* vram, size_t vram_size,
                                           const uint8_t* spu2_ram, size_t spu2_size,
                                           const uint8_t* ee_ram, size_t ee_size) {
    (void)analyzer;
    LOGI("Phase 4: Processing assets (vram=%zu spu2=%zu ee=%zu)",
         vram_size, spu2_size, ee_size);

    std::string assets_dir;
    if (!m_elf.game_id.empty()) {
        assets_dir = m_elf.game_id;
        std::replace(assets_dir.begin(), assets_dir.end(), ' ', '_');
        std::replace(assets_dir.begin(), assets_dir.end(), '/', '_');
    } else {
        assets_dir = "game";
    }

    mkdirs_p(assets_dir);

    m_asset_proc.process_all(vram, vram_size, spu2_ram, spu2_size,
                             ee_ram, ee_size, assets_dir);

    auto textures = m_asset_proc.extract_textures(vram, vram_size);
    auto audio = m_asset_proc.extract_audio(spu2_ram, spu2_size);
    auto models = m_asset_proc.extract_models(ee_ram, ee_size);

    m_result.textures_extracted = static_cast<uint32_t>(textures.size());
    m_result.audio_clips_extracted = static_cast<uint32_t>(audio.size());
    m_result.models_extracted = static_cast<uint32_t>(models.size());

    LOGI("Phase 4 complete: %u textures, %u audio, %u models",
         m_result.textures_extracted, m_result.audio_clips_extracted,
         m_result.models_extracted);
    return true;
}

bool AOT_Pipeline::phase_5_generate_project(const ELF_Analyzer& analyzer,
                                             const std::vector<TranslatedFunction>& functions,
                                             const std::vector<TextureAsset>& textures,
                                             const std::vector<AudioAsset>& audio,
                                             const std::vector<ModelAsset>& models) {
    LOGI("Phase 5: Generating Android project");

    ProjectConfig config;
    config.game_name = m_elf.game_title.empty() ? "AOT Game" : m_elf.game_title;
    config.game_id   = m_elf.game_id.empty() ? "UNKNOWN" : m_elf.game_id;
    config.package_name = make_package_name(config.game_id);
    config.out_dir = m_result.project_dir.empty() ? "." : m_result.project_dir;

    mkdirs_p(config.out_dir);

    if (!m_proj_gen.generate(analyzer, functions, textures, audio, models, config)) {
        m_result.error = "Phase 5: " + m_proj_gen.last_error();
        return false;
    }

    m_result.project_dir = config.out_dir;
    LOGI("Phase 5 complete: project at %s", config.out_dir.c_str());
    return true;
}

bool AOT_Pipeline::phase_6_build_apk(const std::string& project_dir) {
    LOGI("Phase 6: Building APK in %s", project_dir.c_str());

    FILE* which = popen("which cmake 2>/dev/null", "r");
    if (!which) {
        LOGI("Phase 6: popen failed, skipping build");
        return false;
    }

    char buf[512];
    bool cmake_found = false;
    while (fgets(buf, sizeof(buf), which)) {
        if (strstr(buf, "cmake")) {
            cmake_found = true;
            break;
        }
    }
    pclose(which);

    if (!cmake_found) {
        LOGI("Phase 6: cmake not available, skipping native build");
        return false;
    }

    std::string build_dir = project_dir + "/build_native";
    mkdirs_p(build_dir);

    std::string cmake_cmd =
        "cd \"" + build_dir + "\" && "
        "cmake \"" + project_dir + "\" "
        "-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake "
        "-DANDROID_ABI=arm64-v8a "
        "-DANDROID_PLATFORM=android-24 "
        "-DCMAKE_BUILD_TYPE=Release "
        "2>&1";

    LOGI("Phase 6: Running cmake configure...");
    FILE* cmake_proc = popen(cmake_cmd.c_str(), "r");
    if (cmake_proc) {
        while (fgets(buf, sizeof(buf), cmake_proc)) {
            buf[strcspn(buf, "\n")] = '\0';
            LOGI("  [cmake] %s", buf);
        }
        int ret = pclose(cmake_proc);
        if (ret != 0) {
            LOGE("Phase 6: cmake configure failed (exit %d)", ret);
            return false;
        }
    } else {
        LOGE("Phase 6: failed to launch cmake");
        return false;
    }

    std::string make_cmd =
        "cd \"" + build_dir + "\" && make -j$(nproc) 2>&1";
    LOGI("Phase 6: Running make...");
    FILE* make_proc = popen(make_cmd.c_str(), "r");
    if (make_proc) {
        while (fgets(buf, sizeof(buf), make_proc)) {
            buf[strcspn(buf, "\n")] = '\0';
            LOGI("  [make] %s", buf);
        }
        int ret = pclose(make_proc);
        if (ret != 0) {
            LOGE("Phase 6: make failed (exit %d)", ret);
            return false;
        }
    } else {
        LOGE("Phase 6: failed to launch make");
        return false;
    }

    std::string gradle_cmd =
        "cd \"" + project_dir + "\" && "
        "chmod +x gradlew 2>/dev/null; "
        "./gradlew assembleDebug 2>&1";
    LOGI("Phase 6: Running gradle assembleDebug...");
    FILE* gradle_proc = popen(gradle_cmd.c_str(), "r");
    if (gradle_proc) {
        while (fgets(buf, sizeof(buf), gradle_proc)) {
            buf[strcspn(buf, "\n")] = '\0';
            LOGI("  [gradle] %s", buf);
        }
        int ret = pclose(gradle_proc);
        if (ret != 0) {
            LOGE("Phase 6: gradle build failed (exit %d)", ret);
            return false;
        }
    } else {
        LOGE("Phase 6: failed to launch gradle");
        return false;
    }

    LOGI("Phase 6 complete: APK built at %s/app/build/outputs/apk/debug/", project_dir.c_str());
    return true;
}

std::string AOT_Pipeline::make_package_name(const std::string& game_id) {
    static const std::string prefix = "com.aotgame.";
    std::string result = prefix;
    result.reserve(prefix.size() + game_id.size());

    for (char c : game_id) {
        if (c >= '0' && c <= '9') {
            result += c;
        } else if (c >= 'A' && c <= 'Z') {
            result += static_cast<char>(c + ('a' - 'A'));
        } else if (c >= 'a' && c <= 'z') {
            result += c;
        }
    }
    return result;
}
