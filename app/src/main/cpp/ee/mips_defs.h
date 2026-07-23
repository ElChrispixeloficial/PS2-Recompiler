#pragma once
#ifndef MIPS_DEFS_H
#define MIPS_DEFS_H

#include <cstdint>

// Las constantes GP, SP, RA y el struct EE_State se omiten aquí 
// intencionadamente. Ya están definidos en ee_core.h y causaban conflictos.

// ─── Opcodes Principales (Bits 31-26) ───────────────────────────────────────
enum MipsOpcode : uint8_t {
    OP_SPECIAL = 0x00,
    OP_REGIMM  = 0x01,
    OP_J       = 0x02,
    OP_JAL     = 0x03,
    OP_BEQ     = 0x04,
    OP_BNE     = 0x05,
    OP_BLEZ    = 0x06,
    OP_BGTZ    = 0x07,
    OP_ADDI    = 0x08,
    OP_ADDIU   = 0x09,
    OP_SLTI    = 0x0A,
    OP_SLTIU   = 0x0B,
    OP_ANDI    = 0x0C,
    OP_ORI     = 0x0D,
    OP_XORI    = 0x0E,
    OP_LUI     = 0x0F,
    OP_COP0    = 0x10,
    OP_COP1    = 0x11,
    OP_COP2    = 0x12,
    OP_BEQL    = 0x14,
    OP_BNEL    = 0x15,
    OP_BLEZL   = 0x16,
    OP_BGTZL   = 0x17,
    OP_DADDI   = 0x18,
    OP_DADDIU  = 0x19,
    OP_LDL     = 0x1A,
    OP_LDR     = 0x1B,
    OP_MMI     = 0x1C,
    OP_LQ       = 0x1E,
    OP_SQ       = 0x1F,
    OP_LB      = 0x20,
    OP_LH      = 0x21,
    OP_LWL     = 0x22,
    OP_LW      = 0x23,
    OP_LBU     = 0x24,
    OP_LHU     = 0x25,
    OP_LWR     = 0x26,
    OP_LWU     = 0x27,
    OP_SB      = 0x28,
    OP_SH      = 0x29,
    OP_SWL     = 0x2A,
    OP_SW      = 0x2B,
    OP_SDL     = 0x2C,
    OP_SDR     = 0x2D,
    OP_SWR     = 0x2E,
    OP_CACHE   = 0x2F,
    OP_LL      = 0x30,
    OP_LWC1    = 0x31,
    OP_LWC2    = 0x32,
    OP_PREF    = 0x33,
    OP_LDC1    = 0x35,
    OP_LDC2    = 0x36,
    OP_LD      = 0x37,
    OP_SC      = 0x38,
    OP_SWC1    = 0x39,
    OP_SWC2    = 0x3A,
    OP_SDC1    = 0x3D,
    OP_SDC2    = 0x3E,
    OP_SD      = 0x3F
};

// ─── Funciones Especiales (op=0x00, bits 5-0) ──────────────────────────────
enum MipsFunc : uint8_t {
    FUNC_SLL    = 0x00,
    FUNC_MOVF   = 0x01,
    FUNC_SRL    = 0x02,
    FUNC_SRA    = 0x03,
    FUNC_SLLV   = 0x04,
    FUNC_SRLV   = 0x06,
    FUNC_SRAV   = 0x07,
    FUNC_JR     = 0x08,
    FUNC_JALR   = 0x09,
    FUNC_MOVZ   = 0x0A,
    FUNC_MOVN   = 0x0B,
    FUNC_MFHI   = 0x10,
    FUNC_MTHI   = 0x11,
    FUNC_MFLO   = 0x12,
    FUNC_MTLO   = 0x13,
    FUNC_MULT   = 0x18,
    FUNC_MULTU  = 0x19,
    FUNC_DIV    = 0x1A,
    FUNC_DIVU   = 0x1B,
    FUNC_ADD    = 0x20,
    FUNC_ADDU   = 0x21,
    FUNC_SUB    = 0x22,
    FUNC_SUBU   = 0x23,
    FUNC_AND    = 0x24,
    FUNC_OR     = 0x25,
    FUNC_XOR    = 0x26,
    FUNC_NOR    = 0x27,
    FUNC_SLT    = 0x2A,
    FUNC_SLTU   = 0x2B,
    FUNC_DADD   = 0x2C,
    FUNC_DADDU  = 0x2D,
    FUNC_DSUB   = 0x2E,
    FUNC_DSUBU  = 0x2F,
    FUNC_DSLLV  = 0x14,
    FUNC_DSRLV  = 0x16,
    FUNC_DSRAV  = 0x17,
    FUNC_DSLL   = 0x38,
    FUNC_DSRL   = 0x3A,
    FUNC_DSRA   = 0x3B,
    FUNC_DSLL32 = 0x3C,
    FUNC_DSRL32 = 0x3E,
    FUNC_DSRA32 = 0x3F,
    FUNC_SYNC   = 0x0F,
    FUNC_SYSCALL= 0x0C,
    FUNC_BREAK  = 0x0D,
    FUNC_TGE    = 0x30,
    FUNC_TGEU   = 0x31,
    FUNC_TLT    = 0x32,
    FUNC_TLTU   = 0x33,
    FUNC_TEQ    = 0x34,
    FUNC_TNE    = 0x36
};

// ─── Sub-opcodes para COP0 (op=0x10) ────────────────────────────────────────
enum Cop0Func : uint8_t {
    COP0_MFC0   = 0x00,
    COP0_MTC0   = 0x04,
    COP0_CO     = 0x10,
    COP0_TLBR   = 0x01,
    COP0_TLBWI  = 0x02,
    COP0_TLBWR  = 0x06,
    COP0_TLBP   = 0x08,
    COP0_ERET   = 0x18,
    COP0_DI     = 0x16,
    COP0_EI     = 0x1B
};

// ─── Sub-opcodes para REGIMM (op=0x01, rt field) ────────────────────────────
enum RegImmOp : uint8_t {
    REGIMM_BLTZ   = 0x00,
    REGIMM_BGEZ   = 0x01,
    REGIMM_BLTZL  = 0x02,
    REGIMM_BGEZL  = 0x03,
    REGIMM_TGEI   = 0x08,
    REGIMM_TGEIU  = 0x09,
    REGIMM_TLTI   = 0x0A,
    REGIMM_TLTIU  = 0x0B,
    REGIMM_TEQI   = 0x0C,
    REGIMM_TNEI   = 0x0E,
    REGIMM_BLTZAL = 0x10,
    REGIMM_BGEZAL = 0x11,
    REGIMM_MTSAB  = 0x18,
    REGIMM_MTSAH  = 0x19,
    REGIMM_SYNCI  = 0x1F
};

// ─── Estructura para decodificar instrucciones de 32 bits ───────────────────
// El orden de los campos está invertido respecto a la documentación oficial
// porque C++ en procesadores Little-Endian (como ARM64) invierte el orden
// de los campos de bits definidos en un struct.
union MIPSInstr {
    uint32_t raw;
    struct {
        uint32_t func : 6;  // bits 5-0
        uint32_t sa : 5;    // bits 10-6
        uint32_t rd : 5;    // bits 15-11
        uint32_t rt : 5;    // bits 20-16
        uint32_t rs : 5;    // bits 25-21
        uint32_t op : 6;    // bits 31-26
    } r;
    struct {
        uint32_t imm : 16;  // bits 15-0
        uint32_t rt : 5;    // bits 20-16
        uint32_t rs : 5;    // bits 25-21
        uint32_t op : 6;    // bits 31-26
    } i;
    struct {
        uint32_t target : 26; // bits 25-0
        uint32_t op : 6;      // bits 31-26
    } j;
    
    MIPSInstr(uint32_t v) : raw(v) {}
};

#endif // MIPS_DE