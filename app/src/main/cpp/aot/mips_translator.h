#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include "elf_analyzer.h"

struct MIPSToken {
    enum Type {
        REG, IMM, MEM, LABEL, CONST, OP, PAREN, COMMA, LPAREN, RPAREN
    };
    Type type;
    std::string text;
};

struct TranslatedFunction {
    std::string name;
    uint32_t mips_start;
    uint32_t mips_end;
    std::string cpp_code;
    std::string arm64_code;
    std::vector<std::string> includes;
    std::vector<std::string> dependencies;
    uint32_t stack_size;
};

class MIPS_Translator {
public:
    MIPS_Translator();
    
    TranslatedFunction translate_function(
        uint32_t addr, const uint8_t* code, uint32_t size,
        const ELF_Analyzer& analyzer, uint8_t* ee_ram);

    std::string translate_instruction(uint32_t pc, uint32_t instr);
    std::string generate_register_declarations();
    std::string generate_state_struct();

    const std::string& last_error() const { return m_error; }

private:
    std::string m_error;
    std::unordered_map<uint32_t, std::string> m_func_names;

    std::string reg_name(int r);
    std::string cop0_name(int r);
    std::string format_op(const std::string& op, const std::string& rd,
                          const std::string& rs, const std::string& rt);
    std::string format_load(const std::string& op, const std::string& rt,
                            int16_t imm, const std::string& rs);
    std::string format_store(const std::string& op, const std::string& rt,
                             int16_t imm, const std::string& rs);
    std::string format_branch(const std::string& op, const std::string& rs,
                              const std::string& rt, int16_t imm, uint32_t pc);
    std::string format_branch_z(const std::string& op, const std::string& rs,
                                int16_t imm, uint32_t pc);
    std::string format_syscall(uint32_t pc, uint32_t code);
    std::string format_special(uint32_t pc, uint32_t instr);
    std::string format_regimm(uint32_t instr, uint32_t pc);
    std::string format_cop0(uint32_t instr, uint32_t pc);
    std::string format_cop1(uint32_t instr, uint32_t pc);
    std::string format_cop2(uint32_t instr, uint32_t pc);
    std::string format_special2(uint32_t instr, uint32_t pc);
    std::string format_special3(uint32_t instr, uint32_t pc);
    std::string format_mmi(uint32_t instr, uint32_t pc);
    std::string format_ll_sc(const std::string& op, uint32_t instr, uint32_t pc);
    std::string format_lui(int rt, uint16_t imm);
    std::string resolve_label(uint32_t target, const ELF_Analyzer& analyzer);
};
