// ee/recompiler_arm64.cpp
// Dynamic recompiler MIPS R5900 → ARM64 (AArch64) — 100% Nativo

#include "recompiler_arm64.h"
#include "ee_memory.h"
#include <android/log.h>
#include <cstring>
#include <cstddef>

#define TAG "EE_JIT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static constexpr uint32_t OFF_GPR_LO = offsetof(EE_State, gpr_lo);
static constexpr uint32_t OFF_HI     = offsetof(EE_State, hi);
static constexpr uint32_t OFF_LO     = offsetof(EE_State, lo);
static constexpr uint32_t OFF_PC     = offsetof(EE_State, pc);
static constexpr uint32_t OFF_COP0   = offsetof(EE_State, cop0);

static inline uint32_t gpr_off(uint32_t r) { return OFF_GPR_LO + r * 8; }

struct Emitter {
    uint8_t* p;
    void u32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }

    void movz64(unsigned Xd, uint16_t imm, unsigned shift16) { u32(0xD2800000u | (shift16 << 21) | (uint32_t(imm) << 5) | (Xd & 31)); }
    void movk64(unsigned Xd, uint16_t imm, unsigned shift16) { u32(0xF2800000u | (shift16 << 21) | (uint32_t(imm) << 5) | (Xd & 31)); }
    void mov_imm64(unsigned Xd, uint64_t v) {
        movz64(Xd, v & 0xFFFF, 0);
        if ((v >> 16) & 0xFFFF) movk64(Xd, (v >> 16) & 0xFFFF, 1);
        if ((v >> 32) & 0xFFFF) movk64(Xd, (v >> 32) & 0xFFFF, 2);
        if ((v >> 48) & 0xFFFF) movk64(Xd, (v >> 48) & 0xFFFF, 3);
    }
    void mov_imm32(unsigned Wd, uint32_t v) {
        u32(0x52800000u | ((v & 0xFFFF) << 5) | (Wd & 31));
        if (v >> 16) u32(0x72A00000u | (((v >> 16) & 0xFFFF) << 5) | (Wd & 31));
    }
    void mov_reg64(unsigned Xd, unsigned Xm) { u32(0xAA0003E0u | ((Xm & 31) << 16) | (Xd & 31)); }

    void ldr64(unsigned Xt, unsigned Xn, uint32_t imm) { u32(0xF9400000u | ((imm >> 3) << 10) | ((Xn & 31) << 5) | (Xt & 31)); }
    void str64(unsigned Xt, unsigned Xn, uint32_t imm) { u32(0xF9000000u | ((imm >> 3) << 10) | ((Xn & 31) << 5) | (Xt & 31)); }
    void ldr32(unsigned Wt, unsigned Xn, uint32_t imm) { u32(0xB9400000u | ((imm >> 2) << 10) | ((Xn & 31) << 5) | (Wt & 31)); }
    void str32(unsigned Wt, unsigned Xn, uint32_t imm) { u32(0xB9000000u | ((imm >> 2) << 10) | ((Xn & 31) << 5) | (Wt & 31)); }

    void load_gpr(unsigned Xd, unsigned mreg) {
        if (mreg == 0) { u32(0xAA1F03E0u | (Xd & 31)); return; }
        ldr64(Xd, 19, gpr_off(mreg));
    }
    void store_gpr(unsigned Xs, unsigned mreg) {
        if (mreg == 0) return;
        str64(Xs, 19, gpr_off(mreg));
    }

    void add64(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0x8B000000u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void sub64(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0xCB000000u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void and64(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0x8A000000u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void orr64(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0xAA000000u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void eor64(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0xCA000000u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void mvn64(unsigned Xd, unsigned Xm)              { u32(0xAA2003E0u | ((Xm&31)<<16) | (Xd&31)); }
    void add32(unsigned Wd, unsigned Wn, unsigned Wm) { u32(0x0B000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void sub32(unsigned Wd, unsigned Wn, unsigned Wm) { u32(0x4B000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void mul32(unsigned Wd, unsigned Wn, unsigned Wm) { u32(0x1B007C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void umull(unsigned Xd, unsigned Wn, unsigned Wm) { u32(0x9BA07C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Xd&31)); }
    void smull(unsigned Xd, unsigned Wn, unsigned Wm) { u32(0x9B207C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Xd&31)); }
    void sdiv32(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC00C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void udiv32(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC00800u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void msub32(unsigned Wd, unsigned Wn, unsigned Wm, unsigned Wa) { u32(0x1B008000u | ((Wm&31)<<16) | ((Wa&31)<<10) | ((Wn&31)<<5) | (Wd&31)); }

    void lslv32(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC02000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void lsrv32(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC02400u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void asrv32(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC02800u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void lslv64(unsigned Xd, unsigned Xn, unsigned Xm){ u32(0x9AC02000u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void lsrv64(unsigned Xd, unsigned Xn, unsigned Xm){ u32(0x9AC02400u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void asrv64(unsigned Xd, unsigned Xn, unsigned Xm){ u32(0x9AC02800u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }

    void lsl_imm32(unsigned Wd, unsigned Wn, unsigned imm) {
        unsigned immr = (32 - imm) & 31, imms = 31 - imm;
        u32(0x53000000u | (immr << 16) | (imms << 10) | ((Wn&31) << 5) | (Wd&31));
    }
    void lsr_imm32(unsigned Wd, unsigned Wn, unsigned imm) { u32(0x53007C00u | (imm << 16) | ((Wn&31) << 5) | (Wd&31)); }
    void asr_imm32(unsigned Wd, unsigned Wn, unsigned imm) { u32(0x13007C00u | (imm << 16) | ((Wn&31) << 5) | (Wd&31)); }

    void lsl_imm64(unsigned Xd, unsigned Xn, unsigned imm) {
        unsigned immr = (64 - imm) & 63, imms = 63 - imm;
        u32(0xD3400000u | (immr << 16) | (imms << 10) | ((Xn&31) << 5) | (Xd&31));
    }
    void lsr_imm64(unsigned Xd, unsigned Xn, unsigned imm) { u32(0xD340FC00u | (imm << 16) | ((Xn&31) << 5) | (Xd&31)); }
    void asr_imm64(unsigned Xd, unsigned Xn, unsigned imm) { u32(0x9340FC00u | (imm << 16) | ((Xn&31) << 5) | (Xd&31)); }

    void sxtw(unsigned Xd, unsigned Wn)               { u32(0x93407C00u | ((Wn&31)<<5) | (Xd&31)); }
    void uxtw(unsigned Xd, unsigned Wn)               { u32(0x2A0003E0u | ((Wn&31)<<16) | (Xd&31)); }

    void cmp64(unsigned Xn, unsigned Xm) { u32(0xEB00001Fu | ((Xm&31)<<16) | ((Xn&31)<<5)); }
    void cset64(unsigned Xd, unsigned cond) { u32(0x9A9F07E0u | (cond << 12) | (Xd & 31)); }

    void blr(unsigned Xn) { u32(0xD63F0000u | ((Xn&31) << 5)); }

    void call(void* fn) {
        mov_imm64(16, reinterpret_cast<uintptr_t>(fn));
        blr(16);
    }

    void prologue() {
        u32(0xA9BF7BFDu);        // STP x29, x30, [sp, #-16]!
        u32(0x910003FDu);        // MOV x29, sp
        u32(0xA9BF53F3u);        // STP x19, x20, [sp, #-16]!
        u32(0xA9BF5BF5u);        // STP x21, x22, [sp, #-16]!
        mov_reg64(19, 0);        // x19 = state
        mov_reg64(20, 1);        // x20 = ram
    }
    void epilogue() {
        u32(0xA8C15BF5u);        // LDP x21, x22, [sp], #16
        u32(0xA8C153F3u);        // LDP x19, x20, [sp], #16
        u32(0xA8C17BFDu);        // LDP x29, x30, [sp], #16
        u32(0xD65F03C0u);        // RET
    }
};

extern "C" {
    uint32_t ee_mem_read32(uint32_t addr);
    void     ee_mem_write32(uint32_t addr, uint32_t val);
    uint16_t ee_mem_read16(uint32_t addr);
    void     ee_mem_write16(uint32_t addr, uint16_t val);
    uint8_t  ee_mem_read8 (uint32_t addr);
    void     ee_mem_write8(uint32_t addr, uint8_t val);
}
static uint64_t ee_mem_read64(uint32_t a) { return uint64_t(ee_mem_read32(a)) | (uint64_t(ee_mem_read32(a+4)) << 32); }
static void     ee_mem_write64(uint32_t a, uint64_t v) { ee_mem_write32(a, uint32_t(v)); ee_mem_write32(a+4, uint32_t(v>>32)); }

static void emit_ill_insn(Emitter& e, uint32_t pc, uint32_t insn) {
    LOGE("Instruccion no traducida @%08X (insn=%08X) -> FORZANDO EXC_RI", pc, insn);
    e.mov_imm32(9, pc); e.str32(9, 19, OFF_COP0 + 14*4); 
    e.ldr32(9, 19, OFF_COP0 + 13*4); e.mov_imm32(10, ~0x7Cu);
    e.and64(9, 9, 10); 
    e.mov_imm32(10, 0x40u); // 0x10 (RI) << 2 = 0x40
    e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 13*4);
    e.ldr32(9, 19, OFF_COP0 + 12*4); e.mov_imm32(10, 2);
    e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 12*4);
    e.mov_imm32(21, 0x80000180u);
    e.mov_imm32(20, 1);
}

static bool emit_mips(Emitter& e, uint32_t insn, uint32_t pc) {
    MIPSInstr in{insn};
    uint32_t op = in.r.op, rs = in.r.rs, rt = in.r.rt, rd = in.r.rd, sa = in.r.sa;
    int32_t  imm  = int16_t(in.i.imm);
    uint32_t tgt  = (pc & 0xF0000000u) | (in.j.target << 2);
    int32_t  boff = imm << 2;

    switch (op) {
    case OP_SPECIAL:
        switch (in.r.func) {
        case FUNC_SLL:  e.load_gpr(9, rt); e.lsl_imm32(9, 9, sa); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SRL:  e.load_gpr(9, rt); e.lsr_imm32(9, 9, sa); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SRA:  e.load_gpr(9, rt); e.asr_imm32(9, 9, sa); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SLLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lslv32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SRLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lsrv32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SRAV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.asrv32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_ADD: case FUNC_ADDU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.add32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SUB: case FUNC_SUBU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.sub32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_AND: e.load_gpr(9, rs); e.load_gpr(10, rt); e.and64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_OR:  e.load_gpr(9, rs); e.load_gpr(10, rt); e.orr64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_XOR: e.load_gpr(9, rs); e.load_gpr(10, rt); e.eor64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_NOR: e.load_gpr(9, rs); e.load_gpr(10, rt); e.orr64(9, 9, 10); e.mvn64(9, 9); e.store_gpr(9, rd); return false;
        case FUNC_SLT: e.load_gpr(9, rs); e.load_gpr(10, rt); e.cmp64(9, 10); e.cset64(9, 0xB); e.store_gpr(9, rd); return false;
        case FUNC_SLTU:e.load_gpr(9, rs); e.load_gpr(10, rt); e.cmp64(9, 10); e.cset64(9, 0x3); e.store_gpr(9, rd); return false;
        case FUNC_DADD: case FUNC_DADDU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.add64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_DSUB: case FUNC_DSUBU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.sub64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_DSLL:  e.load_gpr(9, rt); e.lsl_imm64(9, 9, sa);       e.store_gpr(9, rd); return false;
        case FUNC_DSRL:  e.load_gpr(9, rt); e.lsr_imm64(9, 9, sa);       e.store_gpr(9, rd); return false;
        case FUNC_DSRA:  e.load_gpr(9, rt); e.asr_imm64(9, 9, sa);       e.store_gpr(9, rd); return false;
        case FUNC_DSLL32:e.load_gpr(9, rt); e.lsl_imm64(9, 9, sa + 32);  e.store_gpr(9, rd); return false;
        case FUNC_DSRL32:e.load_gpr(9, rt); e.lsr_imm64(9, 9, sa + 32);  e.store_gpr(9, rd); return false;
        case FUNC_DSRA32:e.load_gpr(9, rt); e.asr_imm64(9, 9, sa + 32);  e.store_gpr(9, rd); return false;
        case FUNC_DSLLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lslv64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_DSRLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lsrv64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_DSRAV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.asrv64(9, 9, 10); e.store_gpr(9, rd); return false;
        case FUNC_MULT: {
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.smull(11, 9, 10);
            e.sxtw(9, 11);          e.str64(9, 19, OFF_LO);
            e.asr_imm64(9, 11, 32); e.sxtw(9, 9); e.str64(9, 19, OFF_HI);
            if (rd) { e.ldr64(9, 19, OFF_LO); e.store_gpr(9, rd); }
            return false;
        }
        case FUNC_MULTU: {
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.umull(11, 9, 10);
            e.sxtw(9, 11);          e.str64(9, 19, OFF_LO);
            e.lsr_imm64(9, 11, 32); e.sxtw(9, 9); e.str64(9, 19, OFF_HI);
            if (rd) { e.ldr64(9, 19, OFF_LO); e.store_gpr(9, rd); }
            return false;
        }
        case FUNC_DIV: {
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.sdiv32(11, 9, 10); e.msub32(12, 11, 10, 9);
            e.sxtw(11, 11); e.str64(11, 19, OFF_LO);
            e.sxtw(12, 12); e.str64(12, 19, OFF_HI);
            return false;
        }
        case FUNC_DIVU: {
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.udiv32(11, 9, 10); e.msub32(12, 11, 10, 9);
            e.sxtw(11, 11); e.str64(11, 19, OFF_LO);
            e.sxtw(12, 12); e.str64(12, 19, OFF_HI);
            return false;
        }
        case FUNC_MFHI: e.ldr64(9, 19, OFF_HI); e.store_gpr(9, rd); return false;
        case FUNC_MFLO: e.ldr64(9, 19, OFF_LO); e.store_gpr(9, rd); return false;
        case FUNC_MTHI: e.load_gpr(9, rs); e.str64(9, 19, OFF_HI); return false;
        case FUNC_MTLO: e.load_gpr(9, rs); e.str64(9, 19, OFF_LO); return false;

        case FUNC_JR:
            e.load_gpr(21, rs);
            e.mov_imm32(20, 1);
            return true;
        case FUNC_JALR:
            e.load_gpr(21, rs);
            e.mov_imm32(9, pc + 8); e.sxtw(9, 9); e.store_gpr(9, rd ? rd : RA);
            e.mov_imm32(20, 1);
            return true;

        case FUNC_SYSCALL: {
            e.mov_imm32(9, pc); e.str32(9, 19, OFF_COP0 + 14*4);
            e.ldr32(9, 19, OFF_COP0 + 12*4); e.mov_imm32(10, 2);
            e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 12*4);
            e.mov_imm32(21, 0x80000180u);
            e.mov_imm32(20, 1);
            return true;
        }
        
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36:
            return false; 

        case FUNC_SYNC: return false;

        default:
            emit_ill_insn(e, pc, insn);
            return true; 
        }

    case OP_REGIMM: {
        if (rt <= 0x11) {
            e.load_gpr(9, rs); e.cmp64(9, 31);
            unsigned cond = (rt & 1) ? 0xA : 0xB;
            e.cset64(20, cond);
            e.mov_imm32(21, pc + 4 + boff);
            if (rt & 0x10) { e.mov_imm32(9, pc + 8); e.sxtw(9, 9); e.store_gpr(9, RA); }
            return true;
        }
        return false;
    }

    case OP_J:
        e.mov_imm32(21, tgt); e.mov_imm32(20, 1); return true;
    case OP_JAL:
        e.mov_imm32(9, pc + 8); e.sxtw(9, 9); e.store_gpr(9, RA);
        e.mov_imm32(21, tgt); e.mov_imm32(20, 1); return true;

    case OP_BEQ: case OP_BNE:
    case OP_BLEZ: case OP_BGTZ: {
        e.load_gpr(9, rs);
        if (op == OP_BEQ || op == OP_BNE) { e.load_gpr(10, rt); e.cmp64(9, 10); }
        else                              { e.cmp64(9, 31); }
        unsigned cond;
        switch (op) {
            case OP_BEQ:  cond = 0x0; break;
            case OP_BNE:  cond = 0x1; break;
            case OP_BLEZ: cond = 0xD; break;
            case OP_BGTZ: cond = 0xC; break;
            default:      cond = 0x0;
        }
        e.cset64(20, cond);
        e.mov_imm32(21, pc + 4 + boff);
        return true;
    }

    case OP_ADDI: case OP_ADDIU:
        if (rt == 0x00) return false;
        e.load_gpr(9, rs); e.mov_imm32(10, uint32_t(imm)); e.add32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rt); return false;
    case OP_SLTI:
        if (rt == 0x00) return false;
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.cmp64(9, 10); e.cset64(9, 0xB); e.store_gpr(9, rt); return false;
    case OP_SLTIU:
        if (rt == 0x00) return false;
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.cmp64(9, 10); e.cset64(9, 0x3); e.store_gpr(9, rt); return false;
    case OP_ANDI: e.load_gpr(9, rs); e.mov_imm32(10, uint16_t(imm)); e.and64(9, 9, 10); e.store_gpr(9, rt); return false;
    case OP_ORI:  e.load_gpr(9, rs); e.mov_imm32(10, uint16_t(imm)); e.orr64(9, 9, 10); e.store_gpr(9, rt); return false;
    case OP_XORI: e.load_gpr(9, rs); e.mov_imm32(10, uint16_t(imm)); e.eor64(9, 9, 10); e.store_gpr(9, rt); return false;
    case OP_LUI:  e.mov_imm32(9, uint32_t(uint16_t(imm)) << 16); e.sxtw(9, 9); e.store_gpr(9, rt); return false;
    case OP_DADDI: case OP_DADDIU:
        if (rt == 0x00) return false;
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10); e.store_gpr(9, rt); return false;

    case OP_LB:  case OP_LBU: case OP_LH:  case OP_LHU:
    case OP_LW:  case OP_LWU: case OP_LD: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        void* fn = (op == OP_LB || op == OP_LBU) ? (void*)&ee_mem_read8
                : (op == OP_LH || op == OP_LHU) ? (void*)&ee_mem_read16
                : (op == OP_LD)                 ? (void*)&ee_mem_read64
                :                                 (void*)&ee_mem_read32;
        e.call(fn);
        if      (op == OP_LB)  { e.u32(0x93401C00u); }
        else if (op == OP_LH)  { e.u32(0x93403C00u); }
        else if (op == OP_LW)  { e.sxtw(0, 0); }
        else if (op == OP_LBU) { e.u32(0x92401C00u); }
        else if (op == OP_LHU) { e.u32(0x92403C00u); }
        else if (op == OP_LWU) { e.uxtw(0, 0); }
        e.store_gpr(0, rt);
        return false;
    }

    case OP_SB: case OP_SH: case OP_SW: case OP_SD: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.load_gpr(10, rt);
        e.mov_reg64(0, 9); e.mov_reg64(1, 10);
        void* fn = (op == OP_SB) ? (void*)&ee_mem_write8
                : (op == OP_SH) ? (void*)&ee_mem_write16
                : (op == OP_SD) ? (void*)&ee_mem_write64
                :                 (void*)&ee_mem_write32;
        e.call(fn); return false;
    }

    case OP_COP0:
        if (rs == 0x00) {
            e.ldr32(9, 19, OFF_COP0 + rd*4); e.sxtw(9, 9); e.store_gpr(9, rt); return false;
        } else if (rs == 0x04) {
            e.load_gpr(9, rt); e.str32(9, 19, OFF_COP0 + rd*4); return false;
        } else if (rs == 0x10) {
            if (in.r.func == 0x18) { // ERET
                e.ldr32(21, 19, OFF_COP0 + 14*4);
                e.sxtw(21, 21);
                e.ldr32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(10, ~0x2u); 
                e.and64(9, 9, 10);
                e.str32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(20, 1); 
                return true;
            } else if (in.r.func == 0x16) { // DI
                e.ldr32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(10, ~0x1u); 
                e.and64(9, 9, 10);
                e.str32(9, 19, OFF_COP0 + 12*4);
                return false;
            } else if (in.r.func == 0x1B) { // EI
                e.ldr32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(10, 0x1);
                e.orr64(9, 9, 10);
                e.str32(9, 19, OFF_COP0 + 12*4);
                return false;
            }
        }
        emit_ill_insn(e, pc, insn);
        return true;

    case OP_CACHE: case OP_PREF: return false;

    default:
        emit_ill_insn(e, pc, insn);
        return true;
    }
}

EE_Recompiler::EE_Recompiler(CodeCache& c, EE_State& s, uint8_t* ram)
    : m_cache(c), m_state(s), m_ram(ram) {}

void EE_Recompiler::invalidate(uint32_t s, uint32_t e) { m_cache.invalidate_range(s, e); }

EE_Recompiler::CompiledBlock EE_Recompiler::compile_block(uint32_t guest_pc) {
    // ESPIA DE RECOMPILACIÓN
    static int compile_log_count = 0;
    if (compile_log_count < 30) {
        uint32_t first_instr = ee_mem_read32(guest_pc);
        LOGI("JIT Compilando PC: 0x%08X | Instruccion: 0x%08X", guest_pc, first_instr);
        compile_log_count++;
    }

    constexpr size_t MAX_CODE = 4096;
    uint8_t* code = static_cast<uint8_t*>(m_cache.alloc(MAX_CODE));
    if (!code) { m_cache.flush(); code = static_cast<uint8_t*>(m_cache.alloc(MAX_CODE)); }
    if (!code) { LOGE("cache lleno"); return nullptr; }

    Emitter e{code};
    e.prologue();

    constexpr uint32_t MAX_INSNS = 128;
    uint32_t pc = guest_pc;
    bool terminated = false;
    uint32_t branch_pc = 0;

    for (uint32_t i = 0; i < MAX_INSNS; ++i) {
        uint32_t instr = ee_mem_read32(pc);
        bool term = emit_mips(e, instr, pc);
        
        if (term && !terminated) {
            terminated = true;
            branch_pc  = pc;
            pc += 4;
            continue; // Seguimos al siguiente ciclo para ejecutar el DELAY SLOT
        }
        
        pc += 4;
        if (terminated) break;
    }

    if (!terminated) {
        e.mov_imm32(9, pc); e.str32(9, 19, OFF_PC);
    } else {
        e.mov_imm32(10, branch_pc + 8);
        e.cmp64(20, 31);
        // CSEL X9, X10, X21, EQ -> Si x20 == 0, X9 = X10 (no tomado), si no, X9 = X21 (tomado)
        e.u32(0x9A800000u | (21 << 16) | (0x0 << 12) | (10 << 5) | 9);
        e.str32(9, 19, OFF_PC);
    }

    e.epilogue();

    size_t code_size = size_t(e.p - code);
    if (code_size > MAX_CODE) { LOGE("¡overrun! %zu > %zu", code_size, MAX_CODE); return nullptr; }

    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(e.p));

    auto fn = reinterpret_cast<CompiledBlock>(code);
    m_cache.register_block(guest_pc, reinterpret_cast<CodeCache::BlockFn>(fn), code_size);
    return fn;
}