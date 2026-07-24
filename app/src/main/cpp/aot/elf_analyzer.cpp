#include "elf_analyzer.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "ELF_Analyzer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#pragma pack(push, 1)

struct ELF32_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF32_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};

struct ELF32_Sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

struct ELF32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

#pragma pack(pop)

static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static bool is_little_endian_elf(const uint8_t* data) {
    if (data[4] == 1) return true;
    if (data[4] == 2) return false;
    return false;
}

#define ELF32_ST_TYPE(i) ((i)&0xf)
#define ELF32_ST_BIND(i) ((i) >> 4)

static uint32_t elf32_sym_type(uint8_t info) { return ELF32_ST_TYPE(info); }
static uint32_t elf32_sym_bind(uint8_t info) { return ELF32_ST_BIND(info); }

static constexpr uint32_t SHT_SYMTAB  = 2;
static constexpr uint32_t SHT_STRTAB  = 3;
static constexpr uint32_t SHT_NOBITS  = 8;
static constexpr uint16_t SHN_UNDEF   = 0;
static constexpr uint32_t STT_FUNC    = 2;
static constexpr uint32_t STT_OBJECT  = 1;

static bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOGE("Failed to open ELF: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return rd == out.size();
}

bool ELF_Analyzer::analyze(const char* elf_path, uint8_t* ee_ram, size_t ram_size) {
    std::vector<uint8_t> buf;
    if (!read_file(elf_path, buf)) return false;
    if (!analyze_buffer(buf.data(), buf.size(), ee_ram, ram_size)) return false;
    analyze_code(ee_ram);
    return true;
}

bool ELF_Analyzer::analyze_buffer(const uint8_t* data, size_t size, uint8_t* ee_ram, size_t ram_size) {
    if (size < sizeof(ELF32_Ehdr)) { LOGE("ELF too small"); return false; }

    uint8_t ei_class = data[4];
    if (ei_class != 1) {
        LOGI("ELF64 detected, delegating to parse_elf64");
        return parse_elf64(data, size);
    }

    bool le = is_little_endian_elf(data);

    const auto* ehdr = reinterpret_cast<const ELF32_Ehdr*>(data);
    uint16_t e_machine = le ? ehdr->e_machine : be16(ehdr->e_machine);
    uint32_t e_entry   = le ? ehdr->e_entry   : be32(ehdr->e_entry);
    uint32_t e_shoff   = le ? ehdr->e_shoff   : be32(ehdr->e_shoff);
    uint16_t e_shnum   = le ? ehdr->e_shnum   : be16(ehdr->e_shnum);
    uint16_t e_shstrndx= le ? ehdr->e_shstrndx: be16(ehdr->e_shstrndx);

    if (e_machine != 8) {
        LOGE("Not a MIPS ELF (machine=%u)", e_machine);
    }

    LOGI("ELF32 entry=0x%08x shoff=0x%x shnum=%u", e_entry, e_shoff, e_shnum);

    m_elf.entry_point = e_entry;
    m_elf.load_address = 0;
    m_elf.code_start = 0;
    m_elf.code_end = 0;
    m_elf.data_start = 0;
    m_elf.data_end = 0;
    m_elf.bss_start = 0;
    m_elf.bss_size = 0;

    if (e_shoff == 0 || e_shnum == 0) return false;
    if (e_shoff + (size_t)e_shnum * sizeof(ELF32_Shdr) > size) { LOGE("Section headers out of bounds"); return false; }

    auto r16 = [&](uint16_t v) -> uint16_t { return le ? v : be16(v); };
    auto r32 = [&](uint32_t v) -> uint32_t { return le ? v : be32(v); };

    const auto* shdrs = reinterpret_cast<const ELF32_Shdr*>(data + e_shoff);

    uint32_t shstrtab_off = 0, shstrtab_sz = 0;
    if (e_shstrndx < e_shnum) {
        shstrtab_off = r32(shdrs[e_shstrndx].sh_offset);
        shstrtab_sz  = r32(shdrs[e_shstrndx].sh_size);
    }

    auto get_shstr = [&](uint32_t name_off) -> std::string {
        if (shstrtab_off == 0 || shstrtab_off + name_off >= size) return "";
        const char* s = reinterpret_cast<const char*>(data + shstrtab_off + name_off);
        size_t maxlen = std::min((size_t)64, size - shstrtab_off - name_off);
        size_t len = strnlen(s, maxlen);
        return std::string(s, len);
    };

    const ELF32_Shdr* symtab_shdr = nullptr;
    const ELF32_Shdr* strtab_shdr = nullptr;
    const ELF32_Shdr* text_shdr   = nullptr;
    const ELF32_Shdr* data_shdr   = nullptr;
    const ELF32_Shdr* bss_shdr    = nullptr;

    for (uint16_t i = 0; i < e_shnum; i++) {
        std::string name = get_shstr(r32(shdrs[i].sh_name));
        uint32_t type = r32(shdrs[i].sh_type);
        uint32_t flags = r32(shdrs[i].sh_flags);
        uint32_t addr = r32(shdrs[i].sh_addr);
        uint32_t off  = r32(shdrs[i].sh_offset);
        uint32_t sz   = r32(shdrs[i].sh_size);

        LOGI("Section[%u] '%s' type=%u flags=0x%x addr=0x%08x off=0x%x size=0x%x", i, name.c_str(), type, flags, addr, off, sz);

        if (name == ".text" && !text_shdr) text_shdr = &shdrs[i];
        if (name == ".data" && !data_shdr) data_shdr = &shdrs[i];
        if (name == ".bss"  && !bss_shdr)  bss_shdr  = &shdrs[i];
        if (type == SHT_SYMTAB && !symtab_shdr) symtab_shdr = &shdrs[i];
        if (type == SHT_STRTAB && name != ".shstrtab" && !strtab_shdr) strtab_shdr = &shdrs[i];
    }

    if (text_shdr) {
        m_elf.code_start = r32(text_shdr->sh_addr);
        m_elf.code_end = m_elf.code_start + r32(text_shdr->sh_size);
        uint32_t off = r32(text_shdr->sh_offset);
        uint32_t sz  = r32(text_shdr->sh_size);
        if (off + sz <= size) {
            m_elf.code_data.assign(data + off, data + off + sz);
        }
        LOGI("Code: 0x%08x - 0x%08x (%u bytes)", m_elf.code_start, m_elf.code_end, sz);
    }

    if (data_shdr) {
        m_elf.data_start = r32(data_shdr->sh_addr);
        m_elf.data_end = m_elf.data_start + r32(data_shdr->sh_size);
        uint32_t off = r32(data_shdr->sh_offset);
        uint32_t sz  = r32(data_shdr->sh_size);
        if (off + sz <= size) {
            m_elf.data_data.assign(data + off, data + off + sz);
        }
    }

    if (bss_shdr) {
        m_elf.bss_start = r32(bss_shdr->sh_addr);
        m_elf.bss_size  = r32(bss_shdr->sh_size);
    }

    if (!m_elf.load_address && m_elf.code_start) {
        m_elf.load_address = m_elf.code_start;
    }

    if (m_elf.load_address == 0 && m_elf.data_start) {
        m_elf.load_address = m_elf.data_start;
    }

    if (symtab_shdr && strtab_shdr) {
        uint32_t sym_off = r32(symtab_shdr->sh_offset);
        uint32_t sym_sz  = r32(symtab_shdr->sh_size);
        uint32_t sym_ent = r32(symtab_shdr->sh_entsize);
        uint32_t str_off = r32(strtab_shdr->sh_offset);
        uint32_t str_sz  = r32(strtab_shdr->sh_size);

        if (sym_ent == 0) sym_ent = sizeof(ELF32_Sym);
        uint32_t count = sym_sz / sym_ent;

        const char* strtab = reinterpret_cast<const char*>(data + str_off);
        const ELF32_Sym* syms = reinterpret_cast<const ELF32_Sym*>(data + sym_off);

        LOGI("Symbol table: %u symbols, strtab size %u", count, str_sz);

        for (uint32_t i = 0; i < count; i++) {
            uint32_t name_idx = r32(syms[i].st_name);
            uint32_t value    = r32(syms[i].st_value);
            uint32_t s_size   = r32(syms[i].st_size);
            uint8_t  info     = syms[i].st_info;
            uint16_t shndx    = r16(syms[i].st_shndx);

            if (shndx == SHN_UNDEF) continue;

            std::string sym_name;
            if (name_idx < str_sz) {
                size_t maxlen = (str_sz - name_idx < (size_t)256) ? str_sz - name_idx : (size_t)256;
                sym_name = std::string(strtab + name_idx, strnlen(strtab + name_idx, maxlen));
            }

            if (sym_name.empty()) continue;

            uint32_t stype = elf32_sym_type(info);
            uint32_t sbind = elf32_sym_bind(info);

            AOT_Symbol sym;
            sym.address  = value;
            sym.size     = s_size;
            sym.type     = stype;
            sym.binding  = sbind;
            sym.name     = sym_name;
            sym.section  = "";

            if (shndx < e_shnum) {
                sym.section = get_shstr(r32(shdrs[shndx].sh_name));
            }

            m_symbols.push_back(sym);
            m_labels[value] = sym_name;

            if (stype == STT_FUNC) {
                m_function_starts.insert(value);
            }
        }
    }

    return true;
}

bool ELF_Analyzer::parse_elf32(const uint8_t* data, size_t size) {
    return false;
}

bool ELF_Analyzer::parse_elf64(const uint8_t* data, size_t size) {
    if (size < 64) return false;

    uint32_t e_entry  = __builtin_bswap32(*(const uint32_t*)(data + 24));
    uint64_t e_shoff  = __builtin_bswap64(*(const uint64_t*)(data + 40));
    uint16_t e_shnum  = __builtin_bswap16(*(const uint16_t*)(data + 60));
    uint16_t e_shstrndx = __builtin_bswap16(*(const uint16_t*)(data + 62));

    m_elf.entry_point = e_entry;
    LOGI("ELF64 entry=0x%08x (partial support)", e_entry);
    return true;
}

void ELF_Analyzer::analyze_code(uint8_t* ee_ram) {
    if (m_elf.code_data.empty()) {
        LOGI("No code data to analyze");
        return;
    }

    build_function_list();
    detect_syscalls(ee_ram);
    detect_iop_access(ee_ram);
    detect_memory_patterns(ee_ram);

    LOGI("Analysis complete: %zu functions, %zu syscalls, %zu IOP deps, %zu mem accesses",
         m_functions.size(), m_syscalls.size(), m_iop_deps.size(), m_mem_accesses.size());
}

static bool is_branch_mips(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    uint32_t func = opcode & 0x3F;

    if (op == 0x04 || op == 0x05) return true;
    if (op == 0x06 || op == 0x07) return true;

    if (op == 0x01) {
        uint32_t rt = (opcode >> 16) & 0x1F;
        if (rt == 0x00 || rt == 0x01 || rt == 0x02 || rt == 0x03) return true;
        if (rt == 0x10 || rt == 0x11 || rt == 0x12 || rt == 0x13) return true;
    }

    if (op == 0x00 && func == 0x08) return true;
    if (op == 0x00 && func == 0x09) return true;

    if (op == 0x1C && func == 0x02) return true;

    return false;
}

static bool is_jump_mips(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    return (op == 0x02 || op == 0x03);
}

static bool is_jal_mips(uint32_t opcode) {
    return ((opcode >> 26) & 0x3F) == 0x03;
}

static bool is_jalr_mips(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    uint32_t func = opcode & 0x3F;
    return (op == 0x00 && func == 0x09);
}

static bool is_syscall_mips(uint32_t opcode) {
    return (opcode == 0x0000000C);
}

static bool is_jr_mips(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    uint32_t func = opcode & 0x3F;
    uint32_t rd = (opcode >> 11) & 0x1F;
    return (op == 0x00 && func == 0x08 && rd == 0);
}

static bool is_jalx_mips(uint32_t opcode) {
    return ((opcode >> 26) & 0x3F) == 0x1D;
}

static bool is_load_mips(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    return (op == 0x20 || op == 0x21 || op == 0x22 || op == 0x23 ||
            op == 0x24 || op == 0x25 || op == 0x26 || op == 0x30 ||
            op == 0x38);
}

static bool is_store_mips(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    return (op == 0x28 || op == 0x29 || op == 0x2A || op == 0x2B ||
            op == 0x3C || op == 0x3E);
}

static uint32_t get_branch_target(uint32_t pc, uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;

    if (op == 0x00) {
        uint32_t func = opcode & 0x3F;
        if (func == 0x08 || func == 0x09) {
            return 0;
        }
    }

    int32_t imm = (int32_t)(int16_t)(opcode & 0xFFFF);
    return (pc + 4) + (imm << 2);
}

static uint32_t get_jump_target(uint32_t pc, uint32_t opcode) {
    uint32_t target = opcode & 0x03FFFFFF;
    return (pc & 0xF0000000) | (target << 2);
}

static int32_t get_load_store_offset(uint32_t opcode) {
    return (int32_t)(int16_t)(opcode & 0xFFFF);
}

static uint32_t get_rs(uint32_t opcode) { return (opcode >> 21) & 0x1F; }
static uint32_t get_rt(uint32_t opcode) { return (opcode >> 16) & 0x1F; }

static uint32_t get_register_value(const uint32_t* regs, uint32_t idx) {
    if (idx < 32) return regs[idx];
    return 0;
}

void ELF_Analyzer::build_function_list() {
    m_functions.clear();

    std::vector<uint32_t> starts(m_function_starts.begin(), m_function_starts.end());
    std::sort(starts.begin(), starts.end());

    if (starts.empty() && m_elf.code_start) {
        starts.push_back(m_elf.code_start);
    }

    const uint32_t* code32 = reinterpret_cast<const uint32_t*>(m_elf.code_data.data());
    size_t code_words = m_elf.code_data.size() / 4;

    for (size_t si = 0; si < starts.size(); si++) {
        uint32_t fstart = starts[si];
        uint32_t fend;

        if (si + 1 < starts.size()) {
            fend = starts[si + 1];
        } else if (m_elf.code_end) {
            fend = m_elf.code_end;
        } else {
            fend = fstart + (uint32_t)m_elf.code_data.size();
        }

        if (fstart < m_elf.code_start || fstart >= m_elf.code_end) continue;
        if (fend > m_elf.code_end) fend = m_elf.code_end;
        if (fend <= fstart) continue;

        uint32_t off_start = (fstart - m_elf.code_start) / 4;
        uint32_t off_end   = (fend - m_elf.code_start) / 4;

        AOT_Function func;
        func.start = fstart;
        func.end = fend;
        func.size = fend - fstart;
        func.has_return = false;

        auto it = m_labels.find(fstart);
        if (it != m_labels.end()) {
            func.name = it->second;
        } else {
            func.name = "sub_" + [&]() -> std::string {
                char buf[16];
                snprintf(buf, sizeof(buf), "%08x", fstart);
                return std::string(buf);
            }();
        }

        uint32_t regs[32] = {};

        for (uint32_t i = off_start; i < off_end && i < (uint32_t)code_words; i++) {
            uint32_t opcode = code32[i];
            uint32_t pc = fstart + (i - off_start) * 4;

            if (is_jal_mips(opcode) || is_jalx_mips(opcode)) {
                uint32_t target = get_jump_target(pc, opcode);
                if (std::find(func.call_targets.begin(), func.call_targets.end(), target) == func.call_targets.end()) {
                    func.call_targets.push_back(target);
                    m_function_starts.insert(target);
                }
            } else if (is_jalr_mips(opcode)) {
                uint32_t rd = (opcode >> 11) & 0x1F;
                if (rd == 31) {
                    uint32_t rs_val = get_register_value(regs, get_rs(opcode));
                    if (rs_val >= m_elf.code_start && rs_val < m_elf.code_end) {
                        if (std::find(func.call_targets.begin(), func.call_targets.end(), rs_val) == func.call_targets.end()) {
                            func.call_targets.push_back(rs_val);
                            m_function_starts.insert(rs_val);
                        }
                    }
                }
            }

            if (is_branch_mips(opcode)) {
                uint32_t target = get_branch_target(pc, opcode);
                if (target >= fstart && target < fend) {
                    func.branches.push_back(target);
                }
            }

            if (is_jump_mips(opcode)) {
                uint32_t target = get_jump_target(pc, opcode);
                if (target >= fstart && target < fend) {
                    func.branches.push_back(target);
                }
            }

            if (is_syscall_mips(opcode)) {
                func.syscalls.push_back(pc);
            }

            if (is_load_mips(opcode) || is_store_mips(opcode)) {
                int32_t offset = get_load_store_offset(opcode);
                uint32_t base_val = get_register_value(regs, get_rs(opcode));
                uint32_t mem_addr = base_val + (uint32_t)offset;

                AOT_MemoryAccess ma;
                ma.instr_addr = pc;
                ma.mem_addr   = mem_addr;
                ma.reg_base   = get_rs(opcode);
                ma.offset     = offset;
                ma.is_store   = is_store_mips(opcode);

                uint32_t op = (opcode >> 26) & 0x3F;
                switch (op) {
                    case 0x20: case 0x28: ma.size = 1; break;
                    case 0x21: case 0x29: ma.size = 2; break;
                    case 0x22: case 0x23: case 0x2A: case 0x2B: ma.size = 4; break;
                    case 0x24: case 0x25: ma.size = 1; break;
                    case 0x26: ma.size = 3; break;
                    default: ma.size = 4; break;
                }

                m_mem_accesses.push_back(ma);

                if (ma.is_store) {
                    func.memory_writes.push_back(mem_addr);
                } else {
                    func.memory_reads.push_back(mem_addr);
                }
            }

            if (is_jr_mips(opcode)) {
                func.has_return = true;
            }
        }

        m_functions.push_back(func);
    }

    for (auto& func : m_functions) {
        auto it = m_labels.find(func.start);
        if (it != m_labels.end()) {
            func.name = it->second;
        }
    }

    LOGI("Built %zu functions", m_functions.size());
}

void ELF_Analyzer::detect_syscalls(uint8_t* ee_ram) {
    m_syscalls.clear();

    for (const auto& func : m_functions) {
        for (uint32_t addr : func.syscalls) {
            AOT_SyscallInfo si;
            si.instr_addr = addr;
            si.function_id = 0;
            memset(si.arg_regs, 0, sizeof(si.arg_regs));

            uint32_t code_offset = addr - m_elf.code_start;
            if (code_offset + 4 <= m_elf.code_data.size()) {
                uint32_t opcode = __builtin_bswap32(
                    *reinterpret_cast<const uint32_t*>(m_elf.code_data.data() + code_offset));
                si.function_id = (opcode >> 6) & 0xFFFFF;
            }

            if (ee_ram && addr < m_elf.code_end) {
                uint32_t ram_offset = addr;
                if (ram_offset + 16 <= m_elf.code_data.size() + m_elf.code_start) {
                    uint32_t prev_off = addr - 4 - m_elf.code_start;
                    for (int j = 0; j < 4 && prev_off + 4 <= m_elf.code_data.size(); j++) {
                        uint32_t prev_op = __builtin_bswap32(
                            *reinterpret_cast<const uint32_t*>(m_elf.code_data.data() + prev_off));

                        uint32_t op = (prev_op >> 26) & 0x3F;
                        if (op == 0x0C) break;

                        if (op == 0x09 || op == 0x0D) {
                            uint32_t rt = (prev_op >> 16) & 0x1F;
                            si.arg_regs[j] = rt;
                        } else if (op == 0x00 && ((prev_op & 0x3F) == 0x21 || (prev_op & 0x3F) == 0x25)) {
                            uint32_t rt = (prev_op >> 16) & 0x1F;
                            si.arg_regs[j] = rt;
                        }

                        prev_off -= 4;
                    }
                }
            }

            m_syscalls.push_back(si);
        }
    }

    LOGI("Detected %zu syscalls", m_syscalls.size());
}

void ELF_Analyzer::detect_iop_access(uint8_t* ee_ram) {
    m_iop_deps.clear();

    for (const auto& ma : m_mem_accesses) {
        bool is_iop = false;
        std::string op_name;

        if ((ma.mem_addr & 0xFFFF0000) == 0x10000000) {
            is_iop = true;
            op_name = ma.is_store ? "SIF_WRITE" : "SIF_READ";
        } else if ((ma.mem_addr & 0xFFFF0000) == 0x1000F000) {
            is_iop = true;
            op_name = ma.is_store ? "IOP_REG_WRITE" : "IOP_REG_READ";
        } else if ((ma.mem_addr & 0xFF000000) == 0x1D000000) {
            is_iop = true;
            op_name = ma.is_store ? "SPU_WRITE" : "SPU_READ";
        } else if ((ma.mem_addr & 0xFF000000) == 0x1C000000) {
            is_iop = true;
            op_name = ma.is_store ? "SPU2_WRITE" : "SPU2_READ";
        } else if ((ma.mem_addr & 0xFFFF0000) == 0x10002000) {
            is_iop = true;
            op_name = ma.is_store ? "MDEC_WRITE" : "MDEC_READ";
        } else if ((ma.mem_addr & 0xFFFF0000) == 0x10003000) {
            is_iop = true;
            op_name = ma.is_store ? "CDVD_WRITE" : "CDVD_READ";
        } else if ((ma.mem_addr & 0xFFFF0000) == 0x10004000) {
            is_iop = true;
            op_name = ma.is_store ? "SSPR_WRITE" : "SSPR_READ";
        } else if ((ma.mem_addr & 0xFFFF0000) == 0x10005000) {
            is_iop = true;
            op_name = ma.is_store ? "SDR_WRITE" : "SDR_READ";
        } else if ((ma.mem_addr & 0xFFFFF000) == 0x1000F100) {
            is_iop = true;
            op_name = ma.is_store ? "POST_BOOT" : "PRE_BOOT";
        }

        if (is_iop) {
            AOT_IOPDependency dep;
            dep.instr_addr = ma.instr_addr;
            dep.reg_addr   = ma.reg_base;
            dep.reg_val    = 0;
            dep.operation  = op_name;

            if (ee_ram) {
                dep.reg_val = ma.mem_addr;
            }

            m_iop_deps.push_back(dep);
        }
    }

    LOGI("Detected %zu IOP dependencies", m_iop_deps.size());
}

void ELF_Analyzer::detect_memory_patterns(uint8_t* ee_ram) {
    for (const auto& ma : m_mem_accesses) {
        uint32_t addr = ma.mem_addr;

        if (addr >= 0x10000000 && addr < 0x10008000) {
            continue;
        }

        if (addr >= 0x11000000 && addr < 0x11008000) {
            continue;
        }

        if (ee_ram && ma.is_store && addr + ma.size <= 32 * 1024 * 1024) {
            uint32_t val = 0;
            if (addr + 4 <= 32 * 1024 * 1024) {
                val = *reinterpret_cast<uint32_t*>(ee_ram + addr);
            }
            (void)val;
        }
    }
}

uint32_t ELF_Analyzer::find_function(uint32_t addr) const {
    for (const auto& func : m_functions) {
        if (addr >= func.start && addr < func.end) {
            return func.start;
        }
    }

    uint32_t best = 0;
    for (uint32_t s : m_function_starts) {
        if (s <= addr && s > best) best = s;
    }
    if (best != 0) return best;

    return 0;
}

std::string ELF_Analyzer::get_label(uint32_t addr) const {
    auto it = m_labels.find(addr);
    if (it != m_labels.end()) {
        return it->second;
    }

    for (const auto& func : m_functions) {
        if (addr >= func.start && addr < func.end) {
            uint32_t offset = addr - func.start;
            char buf[32];
            snprintf(buf, sizeof(buf), "%s+0x%x", func.name.c_str(), offset);
            return std::string(buf);
        }
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "loc_%08x", addr);
    return std::string(buf);
}
