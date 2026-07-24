#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "elf_analyzer.h"
#include "mips_translator.h"
#include "hw_module_gen.h"
#include "asset_processor.h"

struct ProjectConfig {
    std::string game_name;
    std::string game_id;
    std::string package_name;
    std::string out_dir;
    uint32_t min_sdk = 24;
    uint32_t target_sdk = 35;
    uint32_t ndk_version = 29;
    std::string cmake_version = "3.22.1";
};

class Project_Generator {
public:
    bool generate(const ELF_Analyzer& analyzer,
                  const std::vector<TranslatedFunction>& functions,
                  const std::vector<TextureAsset>& textures,
                  const std::vector<AudioAsset>& audio,
                  const std::vector<ModelAsset>& models,
                  const ProjectConfig& config);

    const std::string& last_error() const { return m_error; }

private:
    std::string m_error;

    void generate_cmake(const ProjectConfig& config);
    void generate_build_gradle(const ProjectConfig& config);
    void generate_manifest(const ProjectConfig& config);
    void generate_jni_bridge(const ProjectConfig& config,
                            const ELF_Analyzer& analyzer);
    void generate_main_activity(const ProjectConfig& config);
    void generate_game_cpp(const ProjectConfig& config,
                          const ELF_Analyzer& analyzer,
                          const std::vector<TranslatedFunction>& functions);
    void generate_ee_state(const ELF_Analyzer& analyzer);
    void generate_iop_state();
    void generate_gs_renderer();
    void generate_dma_controller();
    void generate_vu_processor();
    void generate_memory_map(const ELF_Analyzer& analyzer);
    void generate_assets_index(const std::vector<TextureAsset>& textures,
                              const std::vector<AudioAsset>& audio,
                              const std::vector<ModelAsset>& models);

    void write_file(const std::string& path, const std::string& content);
    std::string package_to_path(const std::string& pkg);
};
