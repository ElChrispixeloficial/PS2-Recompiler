#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include "elf_analyzer.h"
#include "mips_translator.h"
#include "hw_module_gen.h"
#include "asset_processor.h"
#include "project_generator.h"

struct AOT_Result {
    bool success;
    std::string project_dir;
    std::string apk_path;
    std::string error;
    uint32_t functions_translated;
    uint32_t textures_extracted;
    uint32_t audio_clips_extracted;
    uint32_t models_extracted;
};

class AOT_Pipeline {
public:
    using ProgressCallback = std::function<void(int phase, const std::string& msg, int pct)>;

    AOT_Pipeline();
    ~AOT_Pipeline();

    AOT_Result run(const char* game_path, const char* output_dir,
                   uint8_t* ee_ram, size_t ram_size,
                   ProgressCallback progress = nullptr);

    const AOT_Result& last_result() const { return m_result; }

private:
    AOT_Result m_result;
    AOT_ELFInfo m_elf;
    ELF_Analyzer m_analyzer;
    MIPS_Translator m_translator;
    HW_ModuleGen m_hw_gen;
    Asset_Processor m_asset_proc;
    Project_Generator m_proj_gen;

    bool phase_1_analyze(const char* game_path, uint8_t* ee_ram, size_t ram_size);
    bool phase_2_translate(const ELF_Analyzer& analyzer, uint8_t* ee_ram);
    bool phase_3_generate_native(const ELF_Analyzer& analyzer,
                                 const std::vector<TranslatedFunction>& functions);
    bool phase_4_process_assets(const ELF_Analyzer& analyzer,
                                const uint8_t* vram, size_t vram_size,
                                const uint8_t* spu2_ram, size_t spu2_size,
                                const uint8_t* ee_ram, size_t ee_size);
    bool phase_5_generate_project(const ELF_Analyzer& analyzer,
                                  const std::vector<TranslatedFunction>& functions,
                                  const std::vector<TextureAsset>& textures,
                                  const std::vector<AudioAsset>& audio,
                                  const std::vector<ModelAsset>& models);
    bool phase_6_build_apk(const std::string& project_dir);

    std::string make_package_name(const std::string& game_id);
};
