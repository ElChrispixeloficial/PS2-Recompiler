#pragma once
#ifndef MIPS_DEFS_H
#define MIPS_DEFS_H

#include <cstdint>

// Eliminamos GP, SP, RA y EE_State de aquí.
// Ya están definidos perfectamente en ee_core.h y causaban conflictos.

// Definiciones de opcodes MIPS (estas no causan conflicto)
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
    OP_CACHE   = 0x2F,
    OP_LB      = 0x20,
    OP_LH      = 0x21,
    OP_LWL     = 0x22,
    OP_LW      = 0x23,
    OP_LBU     = 0x24,
    OP_LHU     = 0x25,
    OP_LWR     = 0x26,
    OP_SB      = 0x28,
    OP_SH      = 0x29,
    OP_SWL     = 0x2A,
    OP_SW      = 0x2B,
    OP_SWR     = 0x2E,
    OP_PREF    = 0x33,
    OP_LWU     = 0x27,
    OP_LD      = 0x37,
    OP_SD      = 0x3F,
    OP_DADDI   = 0x18,
    OP_DADDIU  = 0x19
};

// Funciones especiales (op=0)
enum MipsFunc : uint8_t {
    FUNC_SLL    = 0x00,
    FUNC_SRL    = 0x02,
    FUNC_SRA    = 0x03,
    FUNC_SLLV   = 0x04,
    FUNC_SRLV   = 0x06,
    FUNC_SRAV   = 0x07,
    FUNC_JR     = 0x08,
    FUNC_JALR   = 0x09,
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
    FUNC_DSLLV  = 0x14, // <--- AÑADIDO
    FUNC_DSRLV  = 0x16, // <--- AÑADIDO
    FUNC_DSRAV  = 0x17, // <--- AÑADIDO
    FUNC_DSLL   = 0x38,
    FUNC_DSRL   = 0x3A,
    FUNC_DSRA   = 0x3B,
    FUNC_DSLL32 = 0x3C,
    FUNC_DSRL32 = 0x3E,
    FUNC_DSRA32 = 0x3F,
    FUNC_SYNC   = 0x0F,
    FUNC_SYSCALL= 0x0C
};

// Estructura para decodificar instrucciones de 32 bits
union MIPSInstr {
    uint32_t raw;
    struct {
        uint32_t op : 6;
        uint32_t rs : 5;
        uint32_t rt : 5;
        uint32_t rd : 5;
        uint32_t sa : 5;
        uint32_t func : 6;
    } r;
    struct {
        uint32_t op : 6;
        uint32_t rs : 5;
        uint32_t rt : 5;
        uint32_t imm : 16;
    } i;
    struct {
        uint32_t op : 6;
        uint32_t target : 26;
    } j;
    
    MIPSInstr(uint32_t v) : raw(v) {}
};

#endif // MIPS_DEFS_H