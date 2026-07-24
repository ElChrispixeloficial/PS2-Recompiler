#include "mips_translator.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <android/log.h>

#define LOG_TAG "MIPS_Translator"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Forward-declared helper functions that the generated C++ code will call.
// These are static helpers provided by the runtime:
//   static uint32_t ee_read32(uint8_t* ram, uint32_t addr);
//   static void     ee_write32(uint8_t* ram, uint32_t addr, uint32_t val);
//   static void     handle_syscall(EE_State& state, uint32_t pc);
//
// The generated code also uses inline comparison helpers:
//   static bool beq(uint64_t a, uint64_t b)  { return a == b; }
//   static bool bne(uint64_t a, uint64_t b)  { return a != b; }
//   static bool blez(uint64_t a)             { return (int64_t)a <= 0; }
//   static bool bgtz(uint64_t a)             { return (int64_t)a > 0; }
//   static bool bltz(uint64_t a)             { return (int64_t)a < 0; }
//   static bool bgez(uint64_t a)             { return (int64_t)a >= 0; }
//   static bool bltzal(uint64_t a)           { return (int64_t)a < 0; }
//   static bool bgezal(uint64_t a)           { return (int64_t)a >= 0; }
//   static bool bc1f(uint32_t fcsr)          { return !(fcsr & 0x800000); }
//   static bool bc1t(uint32_t fcsr)          { return (fcsr & 0x800000); }
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// File-scope utilities
// ---------------------------------------------------------------------------

static uint32_t read_u32_be(const uint8_t* base, uint32_t offset) {
    const uint8_t* p = base + offset;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline std::string hex32(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

// ---------------------------------------------------------------------------
// MIPS_Translator implementation
// ---------------------------------------------------------------------------

MIPS_Translator::MIPS_Translator() : m_error() {}

// ---------------------------------------------------------------------------
// Register name helpers
// ---------------------------------------------------------------------------

std::string MIPS_Translator::reg_name(int r) {
    static const char* names[32] = {
        "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
        "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
        "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
        "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"
    };
    if (r >= 0 && r < 32) return names[r];
    return "$?";
}

std::string MIPS_Translator::cop0_name(int r) {
    static const char* names[32] = {
        "Index",     "Random",    "EntryLo0",  "EntryLo1",
        "Context",   "PageMask",  "Wired",     "Reserved0",
        "BadVAddr",  "Count",     "EntryHi",   "Compare",
        "Status",    "Cause",     "EPC",       "PRid",
        "Config",    "LLAddr",    "WatchLo",   "WatchHi",
        "XContext",  "Reserved1", "Reserved2", "Reserved3",
        "Reserved4", "Reserved5", "ParityErr", "CacheErr",
        "TagLo",     "TagHi",     "ErrorEPC",  "DESave"
    };
    if (r >= 0 && r < 32) return names[r];
    return "cop0_?";
}

// ---------------------------------------------------------------------------
// Small helper: GPR accessor expression for register index r
// ---------------------------------------------------------------------------

static inline std::string gpr(int r) {
    return "state.gpr_lo[" + std::to_string(r) + "]";
}

static inline std::string fpu_reg(int r) {
    return "state.fpu[" + std::to_string(r) + "]";
}

static inline std::string cop0_reg(int r) {
    return "state.cop0[" + std::to_string(r) + "]";
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_op(const std::string& op,
                                        const std::string& rd,
                                        const std::string& rs,
                                        const std::string& rt) {
    return "    " + rd + " = (int64_t)(int32_t)(" + op + "((uint32_t)" + rs +
           ", (uint32_t)" + rt + "));\n";
}

std::string MIPS_Translator::format_load(const std::string& op,
                                          const std::string& rt,
                                          int16_t imm,
                                          const std::string& rs) {
    char buf[128];
    if (imm == 0) {
        snprintf(buf, sizeof(buf), "    %s = %s(ram, (uint32_t)%s);\n",
                 rt.c_str(), op.c_str(), rs.c_str());
    } else {
        snprintf(buf, sizeof(buf), "    %s = %s(ram, (uint32_t)(%s + %d));\n",
                 rt.c_str(), op.c_str(), rs.c_str(), (int)imm);
    }
    return buf;
}

std::string MIPS_Translator::format_store(const std::string& op,
                                           const std::string& rt,
                                           int16_t imm,
                                           const std::string& rs) {
    char buf[256];
    if (imm == 0) {
        snprintf(buf, sizeof(buf), "    %s(ram, (uint32_t)%s, (uint32_t)%s);\n",
                 op.c_str(), rs.c_str(), rt.c_str());
    } else {
        snprintf(buf, sizeof(buf), "    %s(ram, (uint32_t)(%s + %d), (uint32_t)%s);\n",
                 op.c_str(), rs.c_str(), (int)imm, rt.c_str());
    }
    return buf;
}

std::string MIPS_Translator::format_branch(const std::string& op,
                                            const std::string& rs,
                                            const std::string& rt,
                                            int16_t imm,
                                            uint32_t pc) {
    uint32_t target = pc + 4 + ((int32_t)imm << 2);
    std::string label = "label_" + hex32(target);
    char buf[256];
    snprintf(buf, sizeof(buf), "    if (%s(%s, %s)) goto %s;\n",
             op.c_str(), rs.c_str(), rt.c_str(), label.c_str());
    return buf;
}

std::string MIPS_Translator::format_branch_z(const std::string& op,
                                              const std::string& rs,
                                              int16_t imm,
                                              uint32_t pc) {
    uint32_t target = pc + 4 + ((int32_t)imm << 2);
    std::string label = "label_" + hex32(target);
    char buf[256];
    snprintf(buf, sizeof(buf), "    if (%s(%s)) goto %s;\n",
             op.c_str(), rs.c_str(), label.c_str());
    return buf;
}

std::string MIPS_Translator::format_syscall(uint32_t pc, uint32_t /*code*/) {
    char buf[128];
    snprintf(buf, sizeof(buf), "    handle_syscall(state, 0x%08x);\n", pc);
    return buf;
}

std::string MIPS_Translator::resolve_label(uint32_t target,
                                            const ELF_Analyzer& analyzer) {
    auto it = m_func_names.find(target);
    if (it != m_func_names.end()) return it->second;
    std::string lbl = analyzer.get_label(target);
    if (!lbl.empty()) return lbl;
    return "sub_" + hex32(target);
}

std::string MIPS_Translator::format_lui(int rt, uint16_t imm) {
    char buf[64];
    snprintf(buf, sizeof(buf), "    state.gpr_lo[%d] = (int64_t)(int32_t)0x%04x0000;\n",
             rt, imm);
    return buf;
}

// ---------------------------------------------------------------------------
// format_special – opcode 0x00 (SPECIAL)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_special(uint32_t pc, uint32_t instr) {
    int rs    = (instr >> 21) & 0x1F;
    int rt    = (instr >> 16) & 0x1F;
    int rd    = (instr >> 11) & 0x1F;
    int shamt = (instr >> 6) & 0x1F;
    int funct = instr & 0x3F;

    std::string line;

    switch (funct) {

    // ---- Shifts --------------------------------------------------------
    case 0x00: // SLL
        if (rd == 0) return "    /* NOP */\n";
        if (shamt == 0)
            line = "    " + gpr(rd) + " = (int64_t)(int32_t)" + gpr(rt) + ";\n";
        else
            line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
                   gpr(rt) + " << " + std::to_string(shamt) + ");\n";
        return line;

    case 0x02: // SRL
        if (shamt == 0)
            line = "    " + gpr(rd) + " = (int64_t)(int32_t)(uint32_t)" + gpr(rt) + ";\n";
        else
            line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
                   gpr(rt) + " >> " + std::to_string(shamt) + ");\n";
        return line;

    case 0x03: // SRA
        if (shamt == 0)
            line = "    " + gpr(rd) + " = (int64_t)(int32_t)" + gpr(rt) + ";\n";
        else
            line = "    " + gpr(rd) + " = (int64_t)(int32_t)((int32_t)" +
                   gpr(rt) + " >> " + std::to_string(shamt) + ");\n";
        return line;

    case 0x04: // SLLV
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rt) + " << ((uint32_t)" + gpr(rs) + " & 0x1F));\n";
        return line;

    case 0x06: // SRLV
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rt) + " >> ((uint32_t)" + gpr(rs) + " & 0x1F));\n";
        return line;

    case 0x07: // SRAV
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((int32_t)" +
               gpr(rt) + " >> ((uint32_t)" + gpr(rs) + " & 0x1F));\n";
        return line;

    // ---- Jump ----------------------------------------------------------
    case 0x08: { // JR
        uint32_t fall = pc + 8;
        line = "    { uint32_t jtarget = (uint32_t)" + gpr(rs) + ";\n";
        line += "      state.pc = jtarget;\n";
        line += "      goto tail_" + hex32(fall) + "; }\n";
        return line;
    }
    case 0x09: { // JALR
        uint32_t fall = pc + 8;
        line = "    " + gpr(31) + " = (int64_t)0x" + hex32(fall) + "LL;\n";
        line += "    { uint32_t jtarget = (uint32_t)" + gpr(rs) + ";\n";
        line += "      state.pc = jtarget;\n";
        line += "      goto tail_" + hex32(fall) + "; }\n";
        return line;
    }

    // ---- SYSCALL / BREAK ----------------------------------------------
    case 0x0C: // SYSCALL
        return format_syscall(pc, (instr >> 6) & 0xFFFFF);

    case 0x0D: // BREAK
        return "    state.halted = true;\n    return;\n";

    // ---- HI / LO moves -------------------------------------------------
    case 0x10: // MFHI
        line = "    " + gpr(rd) + " = state.hi;\n";
        return line;
    case 0x11: // MTHI
        line = "    state.hi = " + gpr(rs) + ";\n";
        return line;
    case 0x12: // MFLO
        line = "    " + gpr(rd) + " = state.lo;\n";
        return line;
    case 0x13: // MTLO
        line = "    state.lo = " + gpr(rs) + ";\n";
        return line;

    // ---- Multiply / Divide ---------------------------------------------
    case 0x18: { // MULT  (rs * rt -> HI:LO, 32-bit signed)
        line = "    { int64_t __res = (int64_t)(int32_t)" + gpr(rs) +
               " * (int64_t)(int32_t)" + gpr(rt) + ";\n";
        line += "      state.lo = (int64_t)(int32_t)(uint32_t)__res;\n";
        line += "      state.hi = (int64_t)(int32_t)(uint32_t)(__res >> 32); }\n";
        return line;
    }
    case 0x19: { // MULTU
        line = "    { uint64_t __res = (uint64_t)(uint32_t)" + gpr(rs) +
               " * (uint64_t)(uint32_t)" + gpr(rt) + ";\n";
        line += "      state.lo = (int64_t)(int32_t)(uint32_t)__res;\n";
        line += "      state.hi = (int64_t)(int32_t)(uint32_t)(__res >> 32); }\n";
        return line;
    }
    case 0x1A: { // DIV
        line = "    { int32_t __n = (int32_t)" + gpr(rs) + ";\n";
        line += "      int32_t __d = (int32_t)" + gpr(rt) + ";\n";
        line += "      if (__d != 0) {\n";
        line += "          state.lo = (int64_t)(int32_t)(__n / __d);\n";
        line += "          state.hi = (int64_t)(int32_t)(__n % __d);\n";
        line += "      } }\n";
        return line;
    }
    case 0x1B: { // DIVU
        line = "    { uint32_t __n = (uint32_t)" + gpr(rs) + ";\n";
        line += "      uint32_t __d = (uint32_t)" + gpr(rt) + ";\n";
        line += "      if (__d != 0) {\n";
        line += "          state.lo = (int64_t)(int32_t)(__n / __d);\n";
        line += "          state.hi = (int64_t)(int32_t)(__n % __d);\n";
        line += "      } }\n";
        return line;
    }
    case 0x1C: { // DMULT
        line = "    { __int128 __res = (__int128)(int64_t)" + gpr(rs) +
               " * (__int128)(int64_t)" + gpr(rt) + ";\n";
        line += "      state.lo = (uint64_t)__res;\n";
        line += "      state.hi = (uint64_t)(__res >> 64); }\n";
        return line;
    }
    case 0x1D: { // DMULTU
        line = "    { unsigned __int128 __res = (unsigned __int128)(uint64_t)" + gpr(rs) +
               " * (unsigned __int128)(uint64_t)" + gpr(rt) + ";\n";
        line += "      state.lo = (uint64_t)__res;\n";
        line += "      state.hi = (uint64_t)(__res >> 64); }\n";
        return line;
    }
    case 0x1E: { // DDIV
        line = "    { int64_t __n = (int64_t)" + gpr(rs) + ";\n";
        line += "      int64_t __d = (int64_t)" + gpr(rt) + ";\n";
        line += "      if (__d != 0) {\n";
        line += "          state.lo = __n / __d;\n";
        line += "          state.hi = __n % __d;\n";
        line += "      } }\n";
        return line;
    }
    case 0x1F: { // DDIVU
        line = "    { uint64_t __n = (uint64_t)" + gpr(rs) + ";\n";
        line += "      uint64_t __d = (uint64_t)" + gpr(rt) + ";\n";
        line += "      if (__d != 0) {\n";
        line += "          state.lo = __n / __d;\n";
        line += "          state.hi = __n % __d;\n";
        line += "      } }\n";
        return line;
    }

    // ---- ADD / SUB (with overflow – R5900 traps) -----------------------
    case 0x20: // ADD
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " + (uint32_t)" + gpr(rt) + ");\n";
        return line;
    case 0x21: // ADDU
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " + (uint32_t)" + gpr(rt) + ");\n";
        return line;
    case 0x22: // SUB
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " - (uint32_t)" + gpr(rt) + ");\n";
        return line;
    case 0x23: // SUBU
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " - (uint32_t)" + gpr(rt) + ");\n";
        return line;

    // ---- Logical -------------------------------------------------------
    case 0x24: // AND
        line = "    " + gpr(rd) + " = (int64_t)(int64_t)(" + gpr(rs) +
               " & " + gpr(rt) + ");\n";
        return line;
    case 0x25: // OR
        line = "    " + gpr(rd) + " = (int64_t)(" + gpr(rs) +
               " | " + gpr(rt) + ");\n";
        return line;
    case 0x26: // XOR
        line = "    " + gpr(rd) + " = (int64_t)(" + gpr(rs) +
               " ^ " + gpr(rt) + ");\n";
        return line;
    case 0x27: // NOR
        line = "    " + gpr(rd) + " = ~((" + gpr(rs) +
               " | " + gpr(rt) + ")) & 0xFFFFFFFFFFFFFFFFULL;\n";
        return line;

    // ---- Set on less than ----------------------------------------------
    case 0x2A: // SLT  (signed)
        line = "    " + gpr(rd) + " = ((int64_t)" + gpr(rs) +
               " < (int64_t)" + gpr(rt) + ") ? 1 : 0;\n";
        return line;
    case 0x2B: // SLTU (unsigned)
        line = "    " + gpr(rd) + " = ((uint64_t)" + gpr(rs) +
               " < (uint64_t)" + gpr(rt) + ") ? 1 : 0;\n";
        return line;

    // ---- DADD / DADDU / DSUB / DSUBU (PS2 R5900 64-bit) ---------------
    case 0x2C: // DADD
        line = "    " + gpr(rd) + " = " + gpr(rs) + " + " + gpr(rt) + ";\n";
        return line;
    case 0x2D: // DADDU
        line = "    " + gpr(rd) + " = " + gpr(rs) + " + " + gpr(rt) + ";\n";
        return line;
    case 0x2E: // DSUB
        line = "    " + gpr(rd) + " = " + gpr(rs) + " - " + gpr(rt) + ";\n";
        return line;
    case 0x2F: // DSUBU
        line = "    " + gpr(rd) + " = " + gpr(rs) + " - " + gpr(rt) + ";\n";
        return line;

    // ---- TGE / TGEU / TLT / TLTU / TEQ / TNE -------------------------
    case 0x30: // TGE  (trap if rs >= rt, signed)
        line = "    if ((int64_t)" + gpr(rs) + " >= (int64_t)" + gpr(rt) +
               ") { state.halted = true; /* TGE */ return; }\n";
        return line;
    case 0x31: // TGEU (trap if rs >= rt, unsigned)
        line = "    if ((uint64_t)" + gpr(rs) + " >= (uint64_t)" + gpr(rt) +
               ") { state.halted = true; /* TGEU */ return; }\n";
        return line;
    case 0x32: // TLT  (signed)
        line = "    if ((int64_t)" + gpr(rs) + " < (int64_t)" + gpr(rt) +
               ") { state.halted = true; /* TLT */ return; }\n";
        return line;
    case 0x33: // TLTU (unsigned)
        line = "    if ((uint64_t)" + gpr(rs) + " < (uint64_t)" + gpr(rt) +
               ") { state.halted = true; /* TLTU */ return; }\n";
        return line;
    case 0x34: // TEQ
        line = "    if (" + gpr(rs) + " == " + gpr(rt) +
               ") { state.halted = true; /* TEQ */ return; }\n";
        return line;
    case 0x36: // TNE
        line = "    if (" + gpr(rs) + " != " + gpr(rt) +
               ") { state.halted = true; /* TNE */ return; }\n";
        return line;

    default:
        break;
    }

    LOGE("Unhandled SPECIAL funct=0x%02x at pc=0x%08x", funct, pc);
    return "    /* TODO: SPECIAL funct=0x" + hex32(funct) + " */\n";
}

// ---------------------------------------------------------------------------
// format_regimm – opcode 0x01 (REGIMM)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_regimm(uint32_t instr, uint32_t pc) {
    int rt   = (instr >> 16) & 0x1F;
    int rs   = (instr >> 21) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);
    uint32_t target = pc + 4 + ((int32_t)imm << 2);
    std::string label = "label_" + hex32(target);
    std::string line;

    switch (rt) {
    case 0x00: // BLTZ
        line = "    if ((int64_t)" + gpr(rs) + " < 0) goto " + label + ";\n";
        return line;
    case 0x01: // BGEZ
        line = "    if ((int64_t)" + gpr(rs) + " >= 0) goto " + label + ";\n";
        return line;
    case 0x10: // BLTZAL
        line = "    " + gpr(31) + " = (int64_t)0x" + hex32(pc + 8) + "LL;\n";
        line += "    if ((int64_t)" + gpr(rs) + " < 0) goto " + label + ";\n";
        return line;
    case 0x11: // BGEZAL
        line = "    " + gpr(31) + " = (int64_t)0x" + hex32(pc + 8) + "LL;\n";
        line += "    if ((int64_t)" + gpr(rs) + " >= 0) goto " + label + ";\n";
        return line;
    default:
        break;
    }

    LOGE("Unhandled REGIMM rt=0x%02x at pc=0x%08x", rt, pc);
    return "    /* TODO: REGIMM rt=0x" + hex32(rt) + " */\n";
}

// ---------------------------------------------------------------------------
// format_cop0 – opcode 0x10 (COP0)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_cop0(uint32_t instr, uint32_t pc) {
    uint32_t sub = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    std::string line;

    switch (sub) {
    case 0x00: // MFC0
        line = "    " + gpr(rt) + " = (int64_t)(int32_t)" + cop0_reg(rd) + ";\n";
        return line;
    case 0x04: // MTC0
        line = "    " + cop0_reg(rd) + " = (uint32_t)" + gpr(rt) + ";\n";
        return line;
    case 0x10: { // RFE  (Return From Exception)
        uint32_t funct = instr & 0x3F;
        if (funct == 0x10) {
            return "    { uint32_t __st = " + cop0_reg(12) +
                   ";\n      " + cop0_reg(12) +
                   " = (__st & 0xFFFFFFF0) | ((__st >> 2) & 0x0F); }\n";
        }
        break;
    }
    default:
        break;
    }

    // Check for ERET (cop0 sub=0x10, funct=0x18)
    if (sub == 0x10) {
        uint32_t funct = instr & 0x3F;
        if (funct == 0x18) {
            return "    { state.pc = " + cop0_reg(14) +
                   "; /* ERET */\n"
                   "      " + cop0_reg(12) + " &= ~2u;\n"
                   "      return; }\n";
        }
    }

    LOGE("Unhandled COP0 sub=0x%02x at pc=0x%08x", sub, pc);
    return "    /* TODO: COP0 sub=0x" + hex32(sub) + " */\n";
}

// ---------------------------------------------------------------------------
// format_cop1 – opcode 0x11 (COP1 – FPU)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_cop1(uint32_t instr, uint32_t pc) {
    uint32_t sub = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int fs = (instr >> 11) & 0x1F;
    int fd = (instr >> 6) & 0x1F;
    uint32_t funct = instr & 0x3F;
    std::string line;

    // BC1x (branch)
    if (sub == 0x08) {
        int16_t imm = (int16_t)(instr & 0xFFFF);
        uint32_t target = pc + 4 + ((int32_t)imm << 2);
        std::string label = "label_" + hex32(target);
        switch (rt) {
        case 0x00: // BC1F
            return "    if (bc1f(state.fcsr)) goto " + label + ";\n";
        case 0x01: // BC1T
            return "    if (bc1t(state.fcsr)) goto " + label + ";\n";
        default:
            break;
        }
    }

    switch (sub) {
    case 0x00: // MFC1  (move from FPU GPR to CPU GPR)
        line = "    { uint32_t __v; memcpy(&__v, &" + fpu_reg(fs) +
               ", sizeof(__v));\n";
        line += "      " + gpr(rt) + " = (int64_t)(int32_t)__v; }\n";
        return line;
    case 0x04: // MTC1  (move from CPU GPR to FPU GPR)
        line = "    { uint32_t __v = (uint32_t)" + gpr(rt) +
               "; memcpy(&" + fpu_reg(fs) + ", &__v, sizeof(__v)); }\n";
        return line;

    case 0x01: { // CFC1  (move FPU control reg to CPU GPR)
        int fc = fs;
        if (fc == 0) {
            line = "    " + gpr(rt) + " = (int64_t)(int32_t)state.fcsr;\n";
        } else {
            line = "    " + gpr(rt) + " = 0; /* CFC1 fc=" + std::to_string(fc) + " */\n";
        }
        return line;
    }
    case 0x05: { // CTC1  (move CPU GPR to FPU control reg)
        int fc = fs;
        if (fc == 0) {
            line = "    state.fcsr = (uint32_t)" + gpr(rt) + ";\n";
        } else {
            line = "    /* CTC1 fc=" + std::to_string(fc) + " */\n";
        }
        return line;
    }

    // Single-precision arithmetic (funct field)
    case 0x02: // Single instructions use funct
        break;  // fall through to funct switch below
    default:
        // Some COP1 ops are encoded differently – also check funct for sub==0x02
        if (sub != 0x02) {
            LOGE("Unhandled COP1 sub=0x%02x at pc=0x%08x", sub, pc);
            return "    /* TODO: COP1 sub=0x" + hex32(sub) + " */\n";
        }
        break;
    }

    // COP1 single-precision funct codes
    switch (funct) {
    case 0x00: // ADD.S
        line = "    " + fpu_reg(fd) + " = " + fpu_reg(fs) + " + " + fpu_reg(rt) + ";\n";
        return line;
    case 0x01: // SUB.S
        line = "    " + fpu_reg(fd) + " = " + fpu_reg(fs) + " - " + fpu_reg(rt) + ";\n";
        return line;
    case 0x02: // MUL.S
        line = "    " + fpu_reg(fd) + " = " + fpu_reg(fs) + " * " + fpu_reg(rt) + ";\n";
        return line;
    case 0x03: // DIV.S
        line = "    " + fpu_reg(fd) + " = " + fpu_reg(fs) + " / " + fpu_reg(rt) + ";\n";
        return line;
    case 0x04: // SQRT.S
        line = "    " + fpu_reg(fd) + " = sqrtf(" + fpu_reg(fs) + ");\n";
        return line;
    case 0x05: // ABS.S
        line = "    " + fpu_reg(fd) + " = fabsf(" + fpu_reg(fs) + ");\n";
        return line;
    case 0x06: // MOV.S
        line = "    " + fpu_reg(fd) + " = " + fpu_reg(fs) + ";\n";
        return line;
    case 0x07: // NEG.S
        line = "    " + fpu_reg(fd) + " = -" + fpu_reg(fs) + ";\n";
        return line;
    case 0x08: // ROUND.W.S
        line = "    { float __tmp = " + fpu_reg(fs) + ";\n";
        line += "      uint32_t __bits;\n";
        line += "      memcpy(&__bits, &__tmp, 4);\n";
        line += "      /* stub round */ memcpy(&" + fpu_reg(fd) + ", &__tmp, 4); }\n";
        return line;
    case 0x09: // TRUNC.W.S
        line = "    { int32_t __i = (int32_t)" + fpu_reg(fs) + ";\n";
        line += "      memcpy(&" + fpu_reg(fd) + ", &__i, 4); }\n";
        return line;
    case 0x0A: // CEIL.W.S
        line = "    { float __v = ceilf(" + fpu_reg(fs) + ");\n";
        line += "      memcpy(&" + fpu_reg(fd) + ", &__v, 4); }\n";
        return line;
    case 0x0B: // FLOOR.W.S
        line = "    { float __v = floorf(" + fpu_reg(fs) + ");\n";
        line += "      memcpy(&" + fpu_reg(fd) + ", &__v, 4); }\n";
        return line;
    case 0x24: // CVT.W.S  (convert single to word)
        line = "    { int32_t __i = (int32_t)" + fpu_reg(fs) + ";\n";
        line += "      memcpy(&" + fpu_reg(fd) + ", &__i, 4); }\n";
        return line;
    case 0x20: // CVT.S.W  (convert word to single)
        line = "    { int32_t __i; memcpy(&__i, &" + fpu_reg(fs) + ", 4);\n";
        line += "      float __f = (float)__i;\n";
        line += "      memcpy(&" + fpu_reg(fd) + ", &__f, 4); }\n";
        return line;
    case 0x30: // C.F.S
        return "    state.fcsr &= ~0x800000u;\n";
    case 0x31: // C.EQ.S
        line = "    if (" + fpu_reg(fs) + " == " + fpu_reg(rt) +
               ") state.fcsr |= 0x800000u; else state.fcsr &= ~0x800000u;\n";
        return line;
    case 0x32: // C.OLT.S
        line = "    if (" + fpu_reg(fs) + " < " + fpu_reg(rt) +
               ") state.fcsr |= 0x800000u; else state.fcsr &= ~0x800000u;\n";
        return line;
    case 0x33: // C.OLE.S
        line = "    if (" + fpu_reg(fs) + " <= " + fpu_reg(rt) +
               ") state.fcsr |= 0x800000u; else state.fcsr &= ~0x800000u;\n";
        return line;
    case 0x34: // C.ULT.S
        line = "    if (" + fpu_reg(fs) + " < " + fpu_reg(rt) +
               ") state.fcsr |= 0x800000u; else state.fcsr &= ~0x800000u;\n";
        return line;
    case 0x35: // C.ULE.S
        line = "    if (" + fpu_reg(fs) + " <= " + fpu_reg(rt) +
               ") state.fcsr |= 0x800000u; else state.fcsr &= ~0x800000u;\n";
        return line;
    case 0x3C: // C.SF.S
        return "    state.fcsr &= ~0x800000u;\n";
    case 0x3E: // C.NGE.S
        line = "    if (" + fpu_reg(fs) + " < " + fpu_reg(rt) +
               ") state.fcsr |= 0x800000u; else state.fcsr &= ~0x800000u;\n";
        return line;
    default:
        break;
    }

    LOGE("Unhandled COP1 funct=0x%02x at pc=0x%08x", funct, pc);
    return "    /* TODO: COP1 funct=0x" + hex32(funct) + " */\n";
}

// ---------------------------------------------------------------------------
// format_cop2 – opcode 0x12 (COP2 / VU macro mode)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_cop2(uint32_t instr, uint32_t pc) {
    uint32_t sub = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    std::string line;

    // QMFC2 – quadword move from VU I/F regs to CPU GPRs (128-bit)
    if (sub == 0x00) {
        line = "    /* QMFC2: load 128-bit VU reg into gpr_lo[" + std::to_string(rd) +
               "]..[" + std::to_string((rd + 3) & 31) + "] */\n";
        line += "    /* TODO: implement VU register read */\n";
        return line;
    }
    // QMTC2 – quadword move from CPU GPRs to VU I/F regs
    if (sub == 0x04) {
        line = "    /* QMTC2: store gpr_lo[" + std::to_string(rd) +
               "]..[" + std::to_string((rd + 3) & 31) +
               "] into VU reg */\n";
        line += "    /* TODO: implement VU register write */\n";
        return line;
    }
    // MFC2
    if (sub == 0x01) {
        line = "    /* MFC2: move VU reg to gpr_lo[" + std::to_string(rt) + "] */\n";
        line += "    /* TODO: implement VU register read */\n";
        return line;
    }
    // MTC2
    if (sub == 0x05) {
        line = "    /* MTC2: move gpr_lo[" + std::to_string(rt) + "] to VU reg */\n";
        line += "    /* TODO: implement VU register write */\n";
        return line;
    }

    // VU cop2 operations: the bulk of VU integer ops are encoded in the
    // lower bits.  For the AOT translator we emit stubs with comments
    // noting the operation; a full implementation would decode the
    // function field (bits 5:0) and the dest field (bits 15:10).
    uint32_t funct = instr & 0x3F;
    uint32_t dest  = (instr >> 6) & 0xF;

    // VADDx, VSUBx, VMULx, VADDa, VSUBa, VMADDx, VMSUBx, etc.
    line = "    /* COP2 op: sub=0x" + hex32(sub) + " funct=0x" + hex32(funct) +
           " dest=" + std::to_string(dest) + " */\n";
    line += "    /* TODO: VU macro-mode instruction */\n";
    LOGE("Unhandled COP2 at pc=0x%08x (sub=0x%02x, funct=0x%02x)", pc, sub, funct);
    return line;
}

// ---------------------------------------------------------------------------
// format_special2 – opcode 0x1C (SPECIAL2)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_special2(uint32_t instr, uint32_t pc) {
    int rs   = (instr >> 21) & 0x1F;
    int rt   = (instr >> 16) & 0x1F;
    int rd   = (instr >> 11) & 0x1F;
    int funct = instr & 0x3F;
    std::string line;

    switch (funct) {
    case 0x02: // MUL (rd = (rs * rt)[31:0])
        line = "    " + gpr(rd) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " * (uint32_t)" + gpr(rt) + ");\n";
        return line;
    case 0x00: // MADD (HI:LO += rs * rt, signed 32-bit)
        line = "    { int64_t __prod = (int64_t)(int32_t)" + gpr(rs) +
               " * (int64_t)(int32_t)" + gpr(rt) + ";\n";
        line += "      int64_t __acc = ((int64_t)(int32_t)state.hi << 32) | "
                "(uint64_t)(uint32_t)state.lo;\n";
        line += "      __acc += __prod;\n";
        line += "      state.lo = (int64_t)(int32_t)(uint32_t)__acc;\n";
        line += "      state.hi = (int64_t)(int32_t)(uint32_t)(__acc >> 32); }\n";
        return line;
    case 0x01: // MADDU (HI:LO += rs * rt, unsigned 32-bit)
        line = "    { uint64_t __prod = (uint64_t)(uint32_t)" + gpr(rs) +
               " * (uint64_t)(uint32_t)" + gpr(rt) + ";\n";
        line += "      uint64_t __acc = ((uint64_t)(uint32_t)state.hi << 32) | "
                "(uint64_t)(uint32_t)state.lo;\n";
        line += "      __acc += __prod;\n";
        line += "      state.lo = (int64_t)(int32_t)(uint32_t)__acc;\n";
        line += "      state.hi = (int64_t)(int32_t)(uint32_t)(__acc >> 32); }\n";
        return line;
    case 0x04: // MSUB
        line = "    { int64_t __prod = (int64_t)(int32_t)" + gpr(rs) +
               " * (int64_t)(int32_t)" + gpr(rt) + ";\n";
        line += "      int64_t __acc = ((int64_t)(int32_t)state.hi << 32) | "
                "(uint64_t)(uint32_t)state.lo;\n";
        line += "      __acc -= __prod;\n";
        line += "      state.lo = (int64_t)(int32_t)(uint32_t)__acc;\n";
        line += "      state.hi = (int64_t)(int32_t)(uint32_t)(__acc >> 32); }\n";
        return line;
    case 0x05: // MSUBU
        line = "    { uint64_t __prod = (uint64_t)(uint32_t)" + gpr(rs) +
               " * (uint64_t)(uint32_t)" + gpr(rt) + ";\n";
        line += "      uint64_t __acc = ((uint64_t)(uint32_t)state.hi << 32) | "
                "(uint64_t)(uint32_t)state.lo;\n";
        line += "      __acc -= __prod;\n";
        line += "      state.lo = (int64_t)(int32_t)(uint32_t)__acc;\n";
        line += "      state.hi = (int64_t)(int32_t)(uint32_t)(__acc >> 32); }\n";
        return line;

    default:
        break;
    }

    LOGE("Unhandled SPECIAL2 funct=0x%02x at pc=0x%08x", funct, pc);
    return "    /* TODO: SPECIAL2 funct=0x" + hex32(funct) + " */\n";
}

// ---------------------------------------------------------------------------
// format_special3 – opcode 0x1F (SPECIAL3 – R5900 extensions)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_special3(uint32_t instr, uint32_t pc) {
    int rs   = (instr >> 21) & 0x1F;
    int rt   = (instr >> 16) & 0x1F;
    int rd   = (instr >> 11) & 0x1F;
    int shamt = (instr >> 6) & 0x1F;
    int funct = instr & 0x3F;
    std::string line;

    switch (funct) {
    case 0x00: { // EXT  (Extract Bit Field)
        int pos = shamt;
        int size = (instr >> 11) & 0x1F;
        int lsbsz = pos + 1;
        if (lsbsz == 0) lsbsz = 32;
        uint32_t mask = (lsbsz >= 32) ? 0xFFFFFFFFu : ((1u << lsbsz) - 1);
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    %s = (int64_t)(int32_t)((%s >> %d) & 0x%Xu);\n",
                 gpr(rt).c_str(), gpr(rs).c_str(), pos, mask);
        return buf;
    }
    case 0x04: { // INS  (Insert Bit Field)
        int pos = shamt;
        int size = (rd - shamt + 1);
        if (size <= 0 || size > 32) size = 32;
        uint32_t mask = (size >= 32) ? 0xFFFFFFFFu : ((1u << size) - 1);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __m = 0x%Xu << %d;\n"
                 "      %s = (%s & ~(uint64_t)__m) | "
                 "(((uint64_t)(%s & 0x%Xu) << %d)); }\n",
                 mask, pos,
                 gpr(rt).c_str(), gpr(rt).c_str(),
                 gpr(rs).c_str(), mask, pos);
        return buf;
    }
    case 0x20: { // BSHFL
        int bshfl_op = (instr >> 6) & 0x1F;
        switch (bshfl_op) {
        case 0x02: // WSBH  (Word Swap Bytes Within Halfwords)
            line = "    { uint32_t __v = (uint32_t)" + gpr(rt) + ";\n";
            line += "      __v = ((__v & 0xFF00FF00) >> 8) | "
                    "((__v & 0x00FF00FF) << 8);\n";
            line += "      " + gpr(rd) + " = (int64_t)(int32_t)__v; }\n";
            return line;
        case 0x10: // SEB  (Sign-Extend Byte)
            line = "    " + gpr(rd) + " = (int64_t)(int8_t)(uint8_t)" +
                   gpr(rt) + ";\n";
            return line;
        case 0x18: // SEH  (Sign-Extend Halfword)
            line = "    " + gpr(rd) + " = (int64_t)(int16_t)(uint16_t)" +
                   gpr(rt) + ";\n";
            return line;
        case 0x30: // BITREV (Bit Reverse) – R5900 specific
            line = "    { uint32_t __v = (uint32_t)" + gpr(rt) + ";\n";
            line += "      __v = ((__v & 0x55555555) << 1) | "
                    "((__v & 0xAAAAAAAA) >> 1);\n";
            line += "      __v = ((__v & 0x33333333) << 2) | "
                    "((__v & 0xCCCCCCCC) >> 2);\n";
            line += "      __v = ((__v & 0x0F0F0F0F) << 4) | "
                    "((__v & 0xF0F0F0F0) >> 4);\n";
            line += "      __v = ((__v & 0x00FF00FF) << 8) | "
                    "((__v & 0xFF00FF00) >> 8);\n";
            line += "      __v = ((__v & 0x0000FFFF) << 16) | "
                    "((__v & 0xFFFF0000) >> 16);\n";
            line += "      " + gpr(rd) + " = (int64_t)(int32_t)__v; }\n";
            return line;
        case 0x24: // BYTEALIGN (PS2-specific)
            line = "    /* BSHFL BYTEALIGN: stub */\n";
            return line;
        default:
            break;
        }
        break;
    }
    case 0x3B: { // RDHWR  (Read Hardware Register)
        int hwr = (instr >> 11) & 0x1F;
        if (hwr == 29) { // TP – thread pointer, typically 0
            line = "    " + gpr(rt) + " = 0;\n";
        } else {
            line = "    /* RDHWR hwr=" + std::to_string(hwr) + " */\n";
            line += "    " + gpr(rt) + " = 0;\n";
        }
        return line;
    }
    default:
        break;
    }

    LOGE("Unhandled SPECIAL3 funct=0x%02x at pc=0x%08x", funct, pc);
    return "    /* TODO: SPECIAL3 funct=0x" + hex32(funct) + " */\n";
}

// ---------------------------------------------------------------------------
// format_mmi – opcode 0x1C primary + SPECIAL funct (R5900 MMI)
// The MMI instructions are reached through SPECIAL with rs field encoding
// the sub-opcode.  This helper is called when format_special detects
// rs >= 0x10 for SPECIAL opcode 0.
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_mmi(uint32_t instr, uint32_t pc) {
    int rs   = (instr >> 21) & 0x1F;
    int rt   = (instr >> 16) & 0x1F;
    int rd   = (instr >> 11) & 0x1F;
    int funct = instr & 0x3F;
    std::string line;

    // MMI is actually a separate encoding in R5900:
    // opcode = 0x1C, SPECIAL2 field uses rs bits as sub-opcode
    // For simplicity we just stub all MMI sub-instructions.

    switch (funct) {
    case 0x10: // MMI0 sub-opcode varies by rd
        line = "    /* MMI0: rd=" + std::to_string(rd) + " */\n";
        line += "    /* TODO: MMI0 sub-instruction */\n";
        return line;
    case 0x12: // MMI1
        line = "    /* MMI1: rd=" + std::to_string(rd) + " */\n";
        line += "    /* TODO: MMI1 sub-instruction */\n";
        return line;
    case 0x14: // MMI2
        line = "    /* MMI2: rd=" + std::to_string(rd) + " */\n";
        line += "    /* TODO: MMI2 sub-instruction */\n";
        return line;
    case 0x16: // MMI3
        line = "    /* MMI3: rd=" + std::to_string(rd) + " */\n";
        line += "    /* TODO: MMI3 sub-instruction */\n";
        return line;
    case 0x28: // PMFHL
        line = "    /* PMFHL */\n";
        line += "    " + gpr(rd) + " = 0; /* TODO */\n";
        return line;
    case 0x29: // PMTHL
        line = "    /* PMTHL */\n";
        return line;
    default:
        break;
    }

    LOGE("Unhandled MMI funct=0x%02x at pc=0x%08x", funct, pc);
    return "    /* TODO: MMI funct=0x" + hex32(funct) + " */\n";
}

// ---------------------------------------------------------------------------
// format_ll_sc – Load Linked / Store Conditional (R5900 LL/SC)
// ---------------------------------------------------------------------------

std::string MIPS_Translator::format_ll_sc(const std::string& op,
                                           uint32_t instr, uint32_t pc) {
    int base = (instr >> 21) & 0x1F;
    int rt   = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (op == "LL") {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    %s = ee_read32(ram, (uint32_t)(%s + %d));\n"
                 "    /* TODO: LL linked address tracking */\n",
                 gpr(rt).c_str(), gpr(base).c_str(), (int)imm);
        return buf;
    }
    if (op == "SC") {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    ee_write32(ram, (uint32_t)(%s + %d), (uint32_t)%s);\n"
                 "    %s = 1; /* SC always succeeds in AOT */\n",
                 gpr(base).c_str(), (int)imm, gpr(rt).c_str(), gpr(rt).c_str());
        return buf;
    }

    return "    /* " + op + " stub */\n";
}

// ---------------------------------------------------------------------------
// translate_instruction – dispatch on primary opcode
// ---------------------------------------------------------------------------

std::string MIPS_Translator::translate_instruction(uint32_t pc, uint32_t instr) {
    int opcode = (instr >> 26) & 0x3F;
    int rs     = (instr >> 21) & 0x1F;
    int rt     = (instr >> 16) & 0x1F;
    int rd     = (instr >> 11) & 0x1F;
    int shamt  = (instr >> 6) & 0x1F;
    int16_t imm16 = (int16_t)(instr & 0xFFFF);
    uint16_t uimm16 = instr & 0xFFFF;
    uint32_t jtarget = ((pc + 4) & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
    std::string line;

    switch (opcode) {

    case 0x00: // SPECIAL
        return format_special(pc, instr);

    case 0x01: // REGIMM
        return format_regimm(instr, pc);

    case 0x02: { // J
        line = "    state.pc = 0x" + hex32(jtarget) + ";\n";
        line += "    goto label_" + hex32(jtarget) + ";\n";
        return line;
    }
    case 0x03: { // JAL
        line = "    " + gpr(31) + " = (int64_t)0x" + hex32(pc + 8) + "LL;\n";
        line += "    state.pc = 0x" + hex32(jtarget) + ";\n";
        line += "    goto label_" + hex32(jtarget) + ";\n";
        return line;
    }
    case 0x04: // BEQ
        return format_branch("beq", gpr(rs), gpr(rt), imm16, pc);
    case 0x05: // BNE
        return format_branch("bne", gpr(rs), gpr(rt), imm16, pc);
    case 0x06: // BLEZ
        return format_branch_z("blez", gpr(rs), imm16, pc);
    case 0x07: // BGTZ
        return format_branch_z("bgtz", gpr(rs), imm16, pc);

    // ---- Arithmetic Immediate -------------------------------------------
    case 0x08: // ADDI
        line = "    " + gpr(rt) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " + (uint32_t)" + std::to_string((int)imm16) + ");\n";
        return line;
    case 0x09: // ADDIU
        line = "    " + gpr(rt) + " = (int64_t)(int32_t)((uint32_t)" +
               gpr(rs) + " + (uint32_t)" + std::to_string((int)imm16) + ");\n";
        return line;
    case 0x0A: // SLTI
        line = "    " + gpr(rt) + " = ((int64_t)" + gpr(rs) +
               " < (int64_t)" + std::to_string((int)imm16) + ") ? 1 : 0;\n";
        return line;
    case 0x0B: // SLTIU
        line = "    " + gpr(rt) + " = ((uint64_t)" + gpr(rs) +
               " < (uint64_t)(int64_t)" + std::to_string((int)imm16) + ") ? 1 : 0;\n";
        return line;
    case 0x0C: // ANDI
        line = "    " + gpr(rt) + " = " + gpr(rs) +
               " & (int64_t)(uint64_t)" + std::to_string((int)uimm16) + "LL;\n";
        return line;
    case 0x0D: // ORI
        line = "    " + gpr(rt) + " = " + gpr(rs) +
               " | (int64_t)(uint64_t)" + std::to_string((int)uimm16) + "LL;\n";
        return line;
    case 0x0E: // XORI
        line = "    " + gpr(rt) + " = " + gpr(rs) +
               " ^ (int64_t)(uint64_t)" + std::to_string((int)uimm16) + "LL;\n";
        return line;
    case 0x0F: // LUI
        return format_lui(rt, uimm16);

    // ---- COP0 ----------------------------------------------------------
    case 0x10:
        return format_cop0(instr, pc);

    // ---- COP1 (FPU) ----------------------------------------------------
    case 0x11:
        return format_cop1(instr, pc);

    // ---- COP2 (VU macro) -----------------------------------------------
    case 0x12:
        return format_cop2(instr, pc);

    // ---- SPECIAL2 (opcode 0x1C) ----------------------------------------
    case 0x1C:
        return format_special2(instr, pc);

    // ---- SPECIAL3 (opcode 0x1F) ----------------------------------------
    case 0x1F:
        return format_special3(instr, pc);

    // ---- Load instructions ---------------------------------------------
    case 0x20: // LB
        line = "    " + gpr(rt) + " = (int64_t)(int8_t)ee_read8(ram, (uint32_t)(" +
               gpr(rs) + " + " + std::to_string((int)imm16) + "));\n";
        return line;
    case 0x21: // LH
        line = "    " + gpr(rt) + " = (int64_t)(int16_t)ee_read16(ram, (uint32_t)(" +
               gpr(rs) + " + " + std::to_string((int)imm16) + "));\n";
        return line;
    case 0x22: { // LWL  (Load Word Left – unaligned)
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __addr = (uint32_t)(%s + %d);\n"
                 "      uint32_t __shift = (__addr & 3) * 8;\n"
                 "      uint32_t __mem = ee_read32(ram, __addr & ~3u);\n"
                 "      uint32_t __cur = (uint32_t)%s;\n"
                 "      %s = (int64_t)(int32_t)(((__cur << (32 - __shift)) & "
                 "0xFFFFFFFF) | (__mem >> __shift)); }\n",
                 gpr(rs).c_str(), (int)imm16, gpr(rt).c_str(), gpr(rt).c_str());
        return buf;
    }
    case 0x23: // LW
        return format_load("ee_read32", gpr(rt), imm16, gpr(rs));
    case 0x24: // LBU
        line = "    " + gpr(rt) + " = (int64_t)(uint32_t)ee_read8(ram, (uint32_t)(" +
               gpr(rs) + " + " + std::to_string((int)imm16) + "));\n";
        return line;
    case 0x25: // LHU
        line = "    " + gpr(rt) + " = (int64_t)(uint32_t)ee_read16(ram, (uint32_t)(" +
               gpr(rs) + " + " + std::to_string((int)imm16) + "));\n";
        return line;
    case 0x26: { // LWR  (Load Word Right – unaligned)
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __addr = (uint32_t)(%s + %d);\n"
                 "      uint32_t __shift = (__addr & 3) * 8;\n"
                 "      uint32_t __mem = ee_read32(ram, __addr & ~3u);\n"
                 "      uint32_t __cur = (uint32_t)%s;\n"
                 "      %s = (int64_t)(int32_t)((__cur >> __shift) | "
                 "((__mem << (32 - __shift)) & 0xFFFFFFFF)); }\n",
                 gpr(rs).c_str(), (int)imm16, gpr(rt).c_str(), gpr(rt).c_str());
        return buf;
    }
    case 0x27: // LWU  (Load Word Unsigned – R5900)
        line = "    " + gpr(rt) + " = (int64_t)(uint64_t)(uint32_t)ee_read32(ram, (uint32_t)(" +
               gpr(rs) + " + " + std::to_string((int)imm16) + "));\n";
        return line;

    // ---- Store instructions --------------------------------------------
    case 0x28: { // SB
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __addr = (uint32_t)(%s + %d);\n"
                 "      ee_write8(ram, __addr, (uint8_t)%s); }\n",
                 gpr(rs).c_str(), (int)imm16, gpr(rt).c_str());
        return buf;
    }
    case 0x29: { // SH
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __addr = (uint32_t)(%s + %d);\n"
                 "      ee_write16(ram, __addr, (uint16_t)%s); }\n",
                 gpr(rs).c_str(), (int)imm16, gpr(rt).c_str());
        return buf;
    }
    case 0x2A: { // SWL  (Store Word Left)
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __addr = (uint32_t)(%s + %d);\n"
                 "      uint32_t __shift = (__addr & 3) * 8;\n"
                 "      uint32_t __mem = ee_read32(ram, __addr & ~3u);\n"
                 "      uint32_t __val = (uint32_t)%s;\n"
                 "      __mem = (__mem >> (32 - __shift)) | "
                 "((__val << __shift) & 0xFFFFFFFF);\n"
                 "      ee_write32(ram, __addr & ~3u, __mem); }\n",
                 gpr(rs).c_str(), (int)imm16, gpr(rt).c_str());
        return buf;
    }
    case 0x2B: // SW
        return format_store("ee_write32", gpr(rt), imm16, gpr(rs));
    case 0x2C: // SDL
        line = "    /* SDL: stub */\n";
        return line;
    case 0x2D: // SDR
        line = "    /* SDR: stub */\n";
        return line;
    case 0x2E: { // SWR  (Store Word Right)
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __addr = (uint32_t)(%s + %d);\n"
                 "      uint32_t __shift = (__addr & 3) * 8;\n"
                 "      uint32_t __mem = ee_read32(ram, __addr & ~3u);\n"
                 "      uint32_t __val = (uint32_t)%s;\n"
                 "      __mem = (__mem >> __shift) | "
                 "((__val << (32 - __shift)) & 0xFFFFFFFF);\n"
                 "      ee_write32(ram, __addr & ~3u, __mem); }\n",
                 gpr(rs).c_str(), (int)imm16, gpr(rt).c_str());
        return buf;
    }

    // ---- Load linked / Store conditional --------------------------------
    case 0x30: // LL
        return format_ll_sc("LL", instr, pc);
    case 0x38: // SC
        return format_ll_sc("SC", instr, pc);

    // ---- LWC1 / LWC2 / LQC2 -------------------------------------------
    case 0x31: { // LWC1
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __v = ee_read32(ram, (uint32_t)(%s + %d));\n"
                 "      memcpy(&state.fpu[%d], &__v, 4); }\n",
                 gpr(rs).c_str(), (int)imm16, rt);
        return buf;
    }
    case 0x32: { // LWC2
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    /* LWC2: load word to VU reg[%d] from addr=%s + %d */\n"
                 "    /* TODO */\n",
                 rt, gpr(rs).c_str(), (int)imm16);
        return buf;
    }
    case 0x37: { // LQC2  (Load Quadword from COP2 – 128-bit)
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    /* LQC2: load 128-bit to VU reg[%d] from addr=%s + %d */\n"
                 "    /* TODO */\n",
                 rt, gpr(rs).c_str(), (int)imm16);
        return buf;
    }

    // ---- SWC1 / SWC2 / SQC2 -------------------------------------------
    case 0x39: { // SWC1
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    { uint32_t __v; memcpy(&__v, &state.fpu[%d], 4);\n"
                 "      ee_write32(ram, (uint32_t)(%s + %d), __v); }\n",
                 rt, gpr(rs).c_str(), (int)imm16);
        return buf;
    }
    case 0x3A: { // SWC2
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    /* SWC2: store VU reg[%d] word to addr=%s + %d */\n"
                 "    /* TODO */\n",
                 rt, gpr(rs).c_str(), (int)imm16);
        return buf;
    }
    case 0x3F: { // SQC2  (Store Quadword from COP2 – 128-bit)
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "    /* SQC2: store 128-bit VU reg[%d] to addr=%s + %d */\n"
                 "    /* TODO */\n",
                 rt, gpr(rs).c_str(), (int)imm16);
        return buf;
    }

    default:
        break;
    }

    LOGE("Unhandled opcode=0x%02x at pc=0x%08x", opcode, pc);
    return "    /* TODO: opcode=0x" + hex32(opcode) + " */\n";
}

// ---------------------------------------------------------------------------
// translate_function – produce a complete C++ function for a MIPS function
// ---------------------------------------------------------------------------

TranslatedFunction MIPS_Translator::translate_function(
    uint32_t addr, const uint8_t* code, uint32_t size,
    const ELF_Analyzer& analyzer, uint8_t* /*ee_ram*/) {

    TranslatedFunction result;
    result.mips_start = addr;
    result.mips_end   = addr + size;
    result.stack_size = 0;

    // Build function name
    char fname[64];
    snprintf(fname, sizeof(fname), "func_%08x", addr);
    result.name = fname;

    // Collect branch targets for label generation
    std::unordered_map<uint32_t, bool> labels_needed;
    uint32_t num_insns = size / 4;

    for (uint32_t i = 0; i < num_insns; i++) {
        uint32_t pc_i = addr + i * 4;
        uint32_t instr = read_u32_be(code, i * 4);
        int opcode = (instr >> 26) & 0x3F;
        int16_t imm16 = (int16_t)(instr & 0xFFFF);
        int rs = (instr >> 21) & 0x1F;

        switch (opcode) {
        case 0x00: { // SPECIAL
            int funct = instr & 0x3F;
            if (funct == 0x08 || funct == 0x09) { // JR, JALR
                labels_needed[pc_i + 8] = true;
            }
            break;
        }
        case 0x01: { // REGIMM
            int rt = (instr >> 16) & 0x1F;
            if (rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11) {
                uint32_t target = pc_i + 4 + ((int32_t)imm16 << 2);
                labels_needed[target] = true;
            }
            break;
        }
        case 0x02: case 0x03: { // J, JAL
            uint32_t jtarget = ((pc_i + 4) & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
            labels_needed[jtarget] = true;
            break;
        }
        case 0x04: case 0x05: { // BEQ, BNE
            uint32_t target = pc_i + 4 + ((int32_t)imm16 << 2);
            labels_needed[target] = true;
            break;
        }
        case 0x06: case 0x07: { // BLEZ, BGTZ
            uint32_t target = pc_i + 4 + ((int32_t)imm16 << 2);
            labels_needed[target] = true;
            break;
        }
        case 0x11: { // COP1 – BC1F / BC1T
            int sub = (instr >> 21) & 0x1F;
            int rt = (instr >> 16) & 0x1F;
            if (sub == 0x08 && (rt == 0 || rt == 1)) {
                uint32_t target = pc_i + 4 + ((int32_t)imm16 << 2);
                labels_needed[target] = true;
            }
            break;
        }
        default:
            break;
        }
    }

    // ------------------------------------------------------------------
    // Build the C++ function string
    // ------------------------------------------------------------------
    std::ostringstream out;

    // Static helper declarations for the generated code
    out << "static inline uint32_t ee_read8(uint8_t* ram, uint32_t addr) {\n";
    out << "    return (uint32_t)ram[addr];\n";
    out << "}\n";
    out << "static inline uint32_t ee_read16(uint8_t* ram, uint32_t addr) {\n";
    out << "    uint16_t v;\n";
    out << "    memcpy(&v, ram + addr, 2);\n";
    out << "    return (uint32_t)v;\n";
    out << "}\n";
    out << "static inline uint32_t ee_read32(uint8_t* ram, uint32_t addr) {\n";
    out << "    uint32_t v;\n";
    out << "    memcpy(&v, ram + addr, 4);\n";
    out << "    return v;\n";
    out << "}\n";
    out << "static inline void ee_write8(uint8_t* ram, uint32_t addr, uint8_t val) {\n";
    out << "    ram[addr] = val;\n";
    out << "}\n";
    out << "static inline void ee_write16(uint8_t* ram, uint32_t addr, uint16_t val) {\n";
    out << "    memcpy(ram + addr, &val, 2);\n";
    out << "}\n";
    out << "static inline void ee_write32(uint8_t* ram, uint32_t addr, uint32_t val) {\n";
    out << "    memcpy(ram + addr, &val, 4);\n";
    out << "}\n";
    out << "\n";

    // Branch comparison helpers
    out << "static inline bool beq(uint64_t a, uint64_t b)  { return a == b; }\n";
    out << "static inline bool bne(uint64_t a, uint64_t b)  { return a != b; }\n";
    out << "static inline bool blez(uint64_t a)             { return (int64_t)a <= 0; }\n";
    out << "static inline bool bgtz(uint64_t a)             { return (int64_t)a > 0; }\n";
    out << "static inline bool bltz(uint64_t a)             { return (int64_t)a < 0; }\n";
    out << "static inline bool bgez(uint64_t a)             { return (int64_t)a >= 0; }\n";
    out << "static inline bool bc1f(uint32_t fcsr)          { return !(fcsr & 0x800000); }\n";
    out << "static inline bool bc1t(uint32_t fcsr)          { return (fcsr & 0x800000) != 0; }\n";
    out << "\n";

    // handle_syscall forward declaration
    out << "static void handle_syscall(EE_State& state, uint32_t pc);\n";
    out << "\n";

    // Function signature
    out << "static void " + result.name + "(EE_State& state, uint8_t* ram) {\n";

    // Local variables
    out << "    uint32_t pc = 0x" << std::hex << addr << std::dec << ";\n";
    out << "\n";

    // Emit label for the entry point
    out << "label_" << std::hex << addr << ":" << std::dec << "\n";

    // Translate each instruction
    for (uint32_t i = 0; i < num_insns; i++) {
        uint32_t pc_i = addr + i * 4;
        uint32_t instr = read_u32_be(code, i * 4);

        // Check if we need a label at this address
        if (labels_needed.count(pc_i) && pc_i != addr) {
            out << "label_" << std::hex << pc_i << ":" << std::dec << "\n";
        }

        // Emit the translated instruction
        std::string cpp = translate_instruction(pc_i, instr);
        out << cpp;
    }

    // Tail label for the fall-through (for JR/JALR that set pc and jump)
    {
        uint32_t tail = addr + num_insns * 4;
        out << "tail_" << std::hex << tail << ":" << std::dec << "\n";
        out << "    return;\n";
    }

    out << "}\n";

    result.cpp_code = out.str();
    result.includes.push_back("cstdint");
    result.includes.push_back("cstring");

    LOGI("Translated function at 0x%08x (%u insns) -> %zu bytes of C++",
         addr, num_insns, result.cpp_code.size());

    return result;
}

// ---------------------------------------------------------------------------
// generate_register_declarations – emit C++ code for EE register declarations
// ---------------------------------------------------------------------------

std::string MIPS_Translator::generate_register_declarations() {
    std::ostringstream o;
    o << "// EE Register Declarations\n";
    o << "// All 32 GPRs are stored as int64_t for sign-extension on 32-bit ops\n";
    o << "// state.gpr_lo[0] ($zero) is hardwired to 0 by the hardware\n";
    for (int i = 0; i < 32; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "// state.gpr_lo[%2d] = %s\n", i, reg_name(i).c_str());
        o << buf;
    }
    o << "// state.hi / state.lo – multiply/divide results\n";
    o << "// state.cop0[N] – COP0 coprocessor registers\n";
    o << "// state.fpu[N]  – FPU single-precision registers (IEEE 754 float)\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// generate_state_struct – emit the EE_State struct definition
// ---------------------------------------------------------------------------

std::string MIPS_Translator::generate_state_struct() {
    std::ostringstream o;
    o << "#pragma once\n\n";
    o << "#include <cstdint>\n";
    o << "#include <cstring>\n\n";
    o << "struct EE_State {\n";
    o << "    int64_t  gpr_lo[32];\n";
    o << "    uint64_t hi, lo;\n";
    o << "    uint32_t pc;\n";
    o << "    uint32_t cop0[32];\n";
    o << "    float    fpu[32];\n";
    o << "    uint32_t fcsr;\n";
    o << "    bool     halted;\n";
    o << "    bool     interrupt_pending;\n";
    o << "    bool     branch_delay;\n";
    o << "    uint32_t branch_target;\n";
    o << "\n";
    o << "    void reset() {\n";
    o << "        memset(this, 0, sizeof(EE_State));\n";
    o << "        halted = false;\n";
    o << "    }\n";
    o << "};\n";
    return o.str();
}
