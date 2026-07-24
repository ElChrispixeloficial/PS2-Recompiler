#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "elf_analyzer.h"

struct HWModule {
    std::string name;
    std::string header_code;
    std::string source_code;
    std::vector<std::string> dependencies;
};

class HW_ModuleGen {
public:
    void generate_all(const ELF_Analyzer& analyzer, const std::string& out_dir);

    HWModule generate_ee_module(const ELF_Analyzer& analyzer);
    HWModule generate_iop_module(const ELF_Analyzer& analyzer);
    HWModule generate_gs_module(const ELF_Analyzer& analyzer);
    HWModule generate_dma_module(const ELF_Analyzer& analyzer);
    HWModule generate_vu_module(const ELF_Analyzer& analyzer);

    void write_file(const std::string& path, const std::string& content);

private:
    std::string generate_ee_header(const ELF_Analyzer& analyzer);
    std::string generate_ee_source(const ELF_Analyzer& analyzer);
    std::string generate_iop_header();
    std::string generate_iop_source();
    std::string generate_gs_header();
    std::string generate_gs_source();
    std::string generate_dma_header();
    std::string generate_dma_source();
    std::string generate_vu_header();
    std::string generate_vu_source();
};
