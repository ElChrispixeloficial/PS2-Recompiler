#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

struct AOT_Symbol {
    uint32_t address;
    uint32_t size;
    uint32_t type;
    uint32_t binding;
    std::string name;
    std::string section;
};

struct AOT_Function {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    std::string name;
    std::vector<uint32_t> call_targets;
    std::vector<uint32_t> branches;
    std::vector<uint32_t> memory_reads;
    std::vector<uint32_t> memory_writes;
    std::vector<uint32_t> syscalls;
    bool has_return;
};

struct AOT_MemoryAccess {
    uint32_t instr_addr;
    uint32_t mem_addr;
    uint32_t reg_base;
    int32_t  offset;
    uint32_t size;
    bool is_store;
};

struct AOT_SyscallInfo {
    uint32_t instr_addr;
    uint32_t function_id;
    uint32_t arg_regs[4];
};

struct AOT_IOPDependency {
    uint32_t instr_addr;
    uint32_t reg_addr;
    uint32_t reg_val;
    std::string operation;
};

struct AOT_ELFInfo {
    std::string game_id;
    std::string game_title;
    uint32_t entry_point;
    uint32_t load_address;
    uint32_t code_start;
    uint32_t code_end;
    uint32_t data_start;
    uint32_t data_end;
    uint32_t bss_start;
    uint32_t bss_size;
    std::vector<uint8_t> code_data;
    std::vector<uint8_t> data_data;
};

class ELF_Analyzer {
public:
    bool analyze(const char* elf_path, uint8_t* ee_ram, size_t ram_size);
    bool analyze_buffer(const uint8_t* data, size_t size, uint8_t* ee_ram, size_t ram_size);

    const AOT_ELFInfo& elf_info() const { return m_elf; }
    const std::vector<AOT_Symbol>& symbols() const { return m_symbols; }
    const std::vector<AOT_Function>& functions() const { return m_functions; }
    const std::vector<AOT_SyscallInfo>& syscalls() const { return m_syscalls; }
    const std::vector<AOT_IOPDependency>& iop_deps() const { return m_iop_deps; }
    const std::unordered_map<uint32_t, std::string>& label_map() const { return m_labels; }

    uint32_t find_function(uint32_t addr) const;
    std::string get_label(uint32_t addr) const;

private:
    AOT_ELFInfo m_elf;
    std::vector<AOT_Symbol> m_symbols;
    std::vector<AOT_Function> m_functions;
    std::vector<AOT_SyscallInfo> m_syscalls;
    std::vector<AOT_IOPDependency> m_iop_deps;
    std::vector<AOT_MemoryAccess> m_mem_accesses;
    std::unordered_map<uint32_t, std::string> m_labels;
    std::unordered_set<uint32_t> m_function_starts;

    bool parse_elf32(const uint8_t* data, size_t size);
    bool parse_elf64(const uint8_t* data, size_t size);
    void analyze_code(uint8_t* ee_ram);
    void build_function_list();
    void detect_syscalls(uint8_t* ee_ram);
    void detect_iop_access(uint8_t* ee_ram);
    void detect_memory_patterns(uint8_t* ee_ram);
};
