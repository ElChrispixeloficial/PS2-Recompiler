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
static constexpr uint32_t OFF_GPR_HI = offsetof(EE_State, gpr_hi);
static constexpr uint32_t OFF_HI     = offsetof(EE_State, hi);
static constexpr uint32_t OFF_LO     = offsetof(EE_State, lo);
static constexpr uint32_t OFF_PC     = offsetof(EE_State, pc);
static constexpr uint32_t OFF_COP0   = offsetof(EE_State, cop0);
static constexpr uint32_t OFF_FPU    = offsetof(EE_State, fpu);
static constexpr uint32_t OFF_FCSR   = offsetof(EE_State, fcsr);

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
    void cmp32(unsigned Wn, unsigned Wm) { u32(0x6B00001Fu | ((Wm&31)<<16) | ((Wn&31)<<5)); }
    void cset64(unsigned Xd, unsigned cond) { u32(0x9A9F07E0u | (cond << 12) | (Xd & 31)); }

    void blr(unsigned Xn) { u32(0xD63F0000u | ((Xn&31) << 5)); }
    void br(unsigned Xn) { u32(0xD61F0000u | ((Xn&31) << 5)); }

    void b_cond(int16_t offset, unsigned cond) {
        u32(0x54000000u | (uint32_t(uint16_t(offset)) << 5) | (cond & 0xF));
    }

    void cbz64(unsigned Xt, int32_t imm19) {
        u32(0xB4000000u | ((uint32_t(imm19) & 0x7FFFF) << 5) | (Xt & 31));
    }
    void cbnz64(unsigned Xt, int32_t imm19) {
        u32(0xB5000000u | ((uint32_t(imm19) & 0x7FFFF) << 5) | (Xt & 31));
    }

    void ldr_q(unsigned Qt, unsigned Xn, uint32_t imm) {
        u32(0x3DC00000u | ((imm >> 4) << 10) | ((Xn & 31) << 5) | (Qt & 31));
    }
    void str_q(unsigned Qt, unsigned Xn, uint32_t imm) {
        u32(0x3D800000u | ((imm >> 4) << 10) | ((Xn & 31) << 5) | (Qt & 31));
    }
    void dup_4s(unsigned Vd, unsigned Wn) {
        u32(0x4E040C00u | ((Wn & 31) << 5) | (Vd & 31));
    }
    void umov_w(unsigned Wd, unsigned Vn, unsigned idx) {
        u32(0x0E043C00u | (idx << 17) | ((Vn & 31) << 5) | (Wd & 31));
    }
    void smov_w(unsigned Wd, unsigned Vn, unsigned idx) {
        u32(0x0E042C00u | (idx << 17) | ((Vn & 31) << 5) | (Wd & 31));
    }
    void fmov_s(unsigned Sd, unsigned Wn) {
        u32(0x1E270000u | ((Wn & 31) << 5) | (Sd & 31));
    }
    void fmov_w(unsigned Wd, unsigned Sn) {
        u32(0x1E260000u | ((Sn & 31) << 5) | (Wd & 31));
    }
    void fadd_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E202800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fsub_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E203800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fmul_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E200800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fdiv_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E201800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fsqrt_s(unsigned Sd, unsigned Sn) {
        u32(0x1E21C800u | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fabs_s(unsigned Sd, unsigned Sn) {
        u32(0x1E20C800u | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fneg_s(unsigned Sd, unsigned Sn) {
        u32(0x1E214800u | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fmadd_s(unsigned Sd, unsigned Sn, unsigned Sm, unsigned Sa) {
        u32(0x1F000800u | ((Sm & 31) << 16) | ((Sa & 31) << 10) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fmsub_s(unsigned Sd, unsigned Sn, unsigned Sm, unsigned Sa) {
        u32(0x1F001800u | ((Sm & 31) << 16) | ((Sa & 31) << 10) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fcvt_w_s(unsigned Wd, unsigned Sn, unsigned rounding) {
        u32(0x1E380000u | (rounding << 13) | ((Sn & 31) << 5) | (Wd & 31));
    }
    void fcvt_s_w(unsigned Sd, unsigned Wn) {
        u32(0x1E220000u | ((Wn & 31) << 5) | (Sd & 31));
    }
    void fcmp_s(unsigned Sn, unsigned Sm, unsigned cond) {
        u32(0x1E202000u | ((Sm & 31) << 16) | (cond << 14) | ((Sn & 31) << 5));
    }
    void ldr_s(unsigned St, unsigned Xn, uint32_t imm) {
        u32(0xBD400000u | ((imm >> 2) << 10) | ((Xn & 31) << 5) | (St & 31));
    }
    void str_s(unsigned St, unsigned Xn, uint32_t imm) {
        u32(0xBD000000u | ((imm >> 2) << 10) | ((Xn & 31) << 5) | (St & 31));
    }
    void ldr_d(unsigned Dt, unsigned Xn, uint32_t imm) {
        u32(0xFD400000u | ((imm >> 3) << 10) | ((Xn & 31) << 5) | (Dt & 31));
    }
    void str_d(unsigned Dt, unsigned Xn, uint32_t imm) {
        u32(0xFD000000u | ((imm >> 3) << 10) | ((Xn & 31) << 5) | (Dt & 31));
    }
    void fcvt_d_s(unsigned Dd, unsigned Sn) {
        u32(0x1E22C000u | ((Sn & 31) << 5) | (Dd & 31));
    }
    void fcvt_s_d(unsigned Sd, unsigned Dn) {
        u32(0x1E624000u | ((Dn & 31) << 5) | (Sd & 31));
    }
    void fcvt_w_d(unsigned Wd, unsigned Dn, unsigned rounding) {
        u32(0x1E780000u | (rounding << 13) | ((Dn & 31) << 5) | (Wd & 31));
    }
    void fcvt_d_w(unsigned Dd, unsigned Wn) {
        u32(0x1E620000u | ((Wn & 31) << 5) | (Dd & 31));
    }
    void fadd_d(unsigned Dd, unsigned Dn, unsigned Dm) {
        u32(0x1E602800u | ((Dm & 31) << 16) | ((Dn & 31) << 5) | (Dd & 31));
    }
    void fsub_d(unsigned Dd, unsigned Dn, unsigned Dm) {
        u32(0x1E603800u | ((Dm & 31) << 16) | ((Dn & 31) << 5) | (Dd & 31));
    }
    void fmul_d(unsigned Dd, unsigned Dn, unsigned Dm) {
        u32(0x1E600800u | ((Dm & 31) << 16) | ((Dn & 31) << 5) | (Dd & 31));
    }
    void fdiv_d(unsigned Dd, unsigned Dn, unsigned Dm) {
        u32(0x1E601800u | ((Dm & 31) << 16) | ((Dn & 31) << 5) | (Dd & 31));
    }
    void fcmp_d(unsigned Dn, unsigned Dm, unsigned cond) {
        u32(0x1E602000u | ((Dm & 31) << 16) | (cond << 14) | ((Dn & 31) << 5));
    }

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
static void     ee_mem_read128_wrapper(uint32_t a, uint8_t* out) { ee_mem_read128(a, out); }
static void     ee_mem_write128_wrapper(uint32_t a, const uint8_t* in) { ee_mem_write128(a, in); }
static uint64_t ee_movz(uint64_t rs_val, uint64_t rt_val, uint64_t rd_val) { return rt_val == 0 ? rs_val : rd_val; }
static uint64_t ee_movn(uint64_t rs_val, uint64_t rt_val, uint64_t rd_val) { return rt_val != 0 ? rs_val : rd_val; }

static void emit_ill_insn(Emitter& e, uint32_t pc, uint32_t insn) {
    LOGE("Instruccion no traducida @%08X (insn=%08X) -> FORZANDO EXC_RI", pc, insn);
    e.mov_imm32(9, pc); e.str32(9, 19, OFF_COP0 + 14*4); 
    e.ldr32(9, 19, OFF_COP0 + 13*4); e.mov_imm32(10, ~0x7Cu);
    e.and64(9, 9, 10); 
    e.mov_imm32(10, 0x14u); // ExcCode=5 (RI) << 2 = 0x14
    e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 13*4);
    e.ldr32(9, 19, OFF_COP0 + 12*4); e.mov_imm32(10, 2);
    e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 12*4);
    e.mov_imm32(21, 0x80000180u);
    e.mov_imm32(20, 1);
}

// Returns: 0=normal, 1=branch (delay slot always executes), 2=branch-likely (delay slot skipped if not taken)
static int emit_mips(Emitter& e, uint32_t insn, uint32_t pc) {
    MIPSInstr in{insn};
    uint32_t op = in.r.op, rs = in.r.rs, rt = in.r.rt, rd = in.r.rd, sa = in.r.sa;
    int32_t  imm  = int16_t(in.i.imm);
    uint32_t tgt  = (pc & 0xF0000000u) | (in.j.target << 2);
    int32_t  boff = imm << 2;

    switch (op) {
    case OP_SPECIAL:
        switch (in.r.func) {
        case FUNC_SLL:  e.load_gpr(9, rt); e.lsl_imm32(9, 9, sa); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SRL:  e.load_gpr(9, rt); e.lsr_imm32(9, 9, sa); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SRA:  e.load_gpr(9, rt); e.asr_imm32(9, 9, sa); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SLLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lslv32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SRLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lsrv32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SRAV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.asrv32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_ADD: case FUNC_ADDU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.add32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SUB: case FUNC_SUBU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.sub32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_AND: e.load_gpr(9, rs); e.load_gpr(10, rt); e.and64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_OR:  e.load_gpr(9, rs); e.load_gpr(10, rt); e.orr64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_XOR: e.load_gpr(9, rs); e.load_gpr(10, rt); e.eor64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_NOR: e.load_gpr(9, rs); e.load_gpr(10, rt); e.orr64(9, 9, 10); e.mvn64(9, 9); e.store_gpr(9, rd); return 0;
        case FUNC_SLT: e.load_gpr(9, rs); e.load_gpr(10, rt); e.cmp64(9, 10); e.cset64(9, 0xB); e.store_gpr(9, rd); return 0;
        case FUNC_SLTU:e.load_gpr(9, rs); e.load_gpr(10, rt); e.cmp64(9, 10); e.cset64(9, 0x3); e.store_gpr(9, rd); return 0;
        case FUNC_MOVZ: { // MOVZ rd,rs,rt: if rt==0 then rd=rs, else rd unchanged
            e.load_gpr(0, rs); e.load_gpr(1, rt); e.load_gpr(2, rd);
            e.call((void*)ee_movz);
            e.store_gpr(0, rd); return 0;
        }
        case FUNC_MOVN: { // MOVN rd,rs,rt: if rt!=0 then rd=rs, else rd unchanged
            e.load_gpr(0, rs); e.load_gpr(1, rt); e.load_gpr(2, rd);
            e.call((void*)ee_movn);
            e.store_gpr(0, rd); return 0;
        }
        case FUNC_DADD: case FUNC_DADDU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.add64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_DSUB: case FUNC_DSUBU:
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.sub64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_DSLL:  e.load_gpr(9, rt); e.lsl_imm64(9, 9, sa);       e.store_gpr(9, rd); return 0;
        case FUNC_DSRL:  e.load_gpr(9, rt); e.lsr_imm64(9, 9, sa);       e.store_gpr(9, rd); return 0;
        case FUNC_DSRA:  e.load_gpr(9, rt); e.asr_imm64(9, 9, sa);       e.store_gpr(9, rd); return 0;
        case FUNC_DSLL32:e.load_gpr(9, rt); e.lsl_imm64(9, 9, sa + 32);  e.store_gpr(9, rd); return 0;
        case FUNC_DSRL32:e.load_gpr(9, rt); e.lsr_imm64(9, 9, sa + 32);  e.store_gpr(9, rd); return 0;
        case FUNC_DSRA32:e.load_gpr(9, rt); e.asr_imm64(9, 9, sa + 32);  e.store_gpr(9, rd); return 0;
        case FUNC_DSLLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lslv64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_DSRLV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.lsrv64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_DSRAV: e.load_gpr(9, rt); e.load_gpr(10, rs); e.asrv64(9, 9, 10); e.store_gpr(9, rd); return 0;
        case FUNC_MULT: {
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.smull(11, 9, 10);
            e.sxtw(9, 11);          e.str64(9, 19, OFF_LO);
            e.asr_imm64(9, 11, 32); e.sxtw(9, 9); e.str64(9, 19, OFF_HI);
            if (rd) { e.ldr64(9, 19, OFF_LO); e.store_gpr(9, rd); }
            return 0;
        }
        case FUNC_MULTU: {
            e.load_gpr(9, rs); e.load_gpr(10, rt); e.umull(11, 9, 10);
            e.sxtw(9, 11);          e.str64(9, 19, OFF_LO);
            e.lsr_imm64(9, 11, 32); e.sxtw(9, 9); e.str64(9, 19, OFF_HI);
            if (rd) { e.ldr64(9, 19, OFF_LO); e.store_gpr(9, rd); }
            return 0;
        }
        case FUNC_DIV: {
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.sdiv32(11, 9, 10); e.msub32(12, 11, 10, 9);
            e.sxtw(11, 11); e.str64(11, 19, OFF_LO);
            e.sxtw(12, 12); e.str64(12, 19, OFF_HI);
            return 0;
        }
        case FUNC_DIVU: {
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.udiv32(11, 9, 10); e.msub32(12, 11, 10, 9);
            e.sxtw(11, 11); e.str64(11, 19, OFF_LO);
            e.sxtw(12, 12); e.str64(12, 19, OFF_HI);
            return 0;
        }
        case FUNC_MFHI: e.ldr64(9, 19, OFF_HI); e.store_gpr(9, rd); return 0;
        case FUNC_MFLO: e.ldr64(9, 19, OFF_LO); e.store_gpr(9, rd); return 0;
        case FUNC_MTHI: e.load_gpr(9, rs); e.str64(9, 19, OFF_HI); return 0;
        case FUNC_MTLO: e.load_gpr(9, rs); e.str64(9, 19, OFF_LO); return 0;

        case FUNC_JR:
            e.load_gpr(21, rs);
            e.mov_imm32(20, 1);
            return 1;
        case FUNC_JALR:
            e.load_gpr(21, rs);
            e.mov_imm32(9, pc + 8); e.sxtw(9, 9); e.store_gpr(9, rd ? rd : RA);
            e.mov_imm32(20, 1);
            return 1;

        case FUNC_SYSCALL: {
            e.mov_imm32(9, pc); e.str32(9, 19, OFF_COP0 + 14*4);
            e.ldr32(9, 19, OFF_COP0 + 12*4); e.mov_imm32(10, 2);
            e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 12*4);
            e.mov_imm32(21, 0x80000180u);
            e.mov_imm32(20, 1);
            return 1;
        }
        case FUNC_BREAK: {
            e.mov_imm32(9, pc); e.str32(9, 19, OFF_COP0 + 14*4);
            e.ldr32(9, 19, OFF_COP0 + 12*4); e.mov_imm32(10, 4);
            e.orr64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 12*4);
            e.ldr32(9, 19, OFF_COP0 + 13*4); e.mov_imm32(10, ~0x7Cu);
            e.and64(9, 9, 10); e.str32(9, 19, OFF_COP0 + 13*4);
            e.mov_imm32(21, 0x80000180u);
            e.mov_imm32(20, 1);
            return 1;
        }
        
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36:
            return 0; 

        case FUNC_SYNC: return 0;

        default:
            emit_ill_insn(e, pc, insn);
            return 1; 
        }

    case OP_REGIMM: {
        e.load_gpr(9, rs); e.cmp64(9, 31);
        unsigned cond = (rt & 1) ? 0xA : 0xB;
        e.cset64(20, cond);
        e.mov_imm32(21, pc + 4 + boff);
        if (rt & 0x10) { e.mov_imm32(9, pc + 8); e.sxtw(9, 9); e.store_gpr(9, RA); }
        // BLTZL(2), BGEZL(3), BLTZALL(18), BGEZALL(19) are branch-likely
        bool likely = (rt == 2 || rt == 3 || rt == 18 || rt == 19);
        return likely ? 2 : 1;
    }

    case OP_J:
        e.mov_imm32(21, tgt); e.mov_imm32(20, 1); return 1;
    case OP_JAL:
        e.mov_imm32(9, pc + 8); e.sxtw(9, 9); e.store_gpr(9, RA);
        e.mov_imm32(21, tgt); e.mov_imm32(20, 1); return 1;

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
        return 1;
    }

    case OP_BEQL: case OP_BNEL:
    case OP_BLEZL: case OP_BGTZL: {
        e.load_gpr(9, rs);
        if (op == OP_BEQL || op == OP_BNEL) { e.load_gpr(10, rt); e.cmp64(9, 10); }
        else                                { e.cmp64(9, 31); }
        unsigned cond;
        switch (op) {
            case OP_BEQL:  cond = 0x0; break;
            case OP_BNEL:  cond = 0x1; break;
            case OP_BLEZL: cond = 0xD; break;
            case OP_BGTZL: cond = 0xC; break;
            default:       cond = 0x0;
        }
        e.cset64(20, cond);
        e.mov_imm32(21, pc + 4 + boff);
        return 2;
    }

    case OP_ADDI: case OP_ADDIU:
        if (rt == 0x00) return 0;
        e.load_gpr(9, rs); e.mov_imm32(10, uint32_t(imm)); e.add32(9, 9, 10); e.sxtw(9, 9); e.store_gpr(9, rt); return 0;
    case OP_SLTI:
        if (rt == 0x00) return 0;
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.cmp64(9, 10); e.cset64(9, 0xB); e.store_gpr(9, rt); return 0;
    case OP_SLTIU:
        if (rt == 0x00) return 0;
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.cmp64(9, 10); e.cset64(9, 0x3); e.store_gpr(9, rt); return 0;
    case OP_ANDI: e.load_gpr(9, rs); e.mov_imm32(10, uint16_t(imm)); e.and64(9, 9, 10); e.store_gpr(9, rt); return 0;
    case OP_ORI:  e.load_gpr(9, rs); e.mov_imm32(10, uint16_t(imm)); e.orr64(9, 9, 10); e.store_gpr(9, rt); return 0;
    case OP_XORI: e.load_gpr(9, rs); e.mov_imm32(10, uint16_t(imm)); e.eor64(9, 9, 10); e.store_gpr(9, rt); return 0;
    case OP_LUI:  e.mov_imm32(9, uint32_t(uint16_t(imm)) << 16); e.sxtw(9, 9); e.store_gpr(9, rt); return 0;
    case OP_DADDI: case OP_DADDIU:
        if (rt == 0x00) return 0;
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10); e.store_gpr(9, rt); return 0;

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
        return 0;
    }

    case OP_SB: case OP_SH: case OP_SW: case OP_SD: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.load_gpr(10, rt);
        e.mov_reg64(0, 9); e.mov_reg64(1, 10);
        void* fn = (op == OP_SB) ? (void*)&ee_mem_write8
                : (op == OP_SH) ? (void*)&ee_mem_write16
                : (op == OP_SD) ? (void*)&ee_mem_write64
                :                 (void*)&ee_mem_write32;
        e.call(fn); return 0;
    }

    case OP_LQ: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.mov_imm32(10, gpr_off(rt)); e.add64(10, 19, 10);
        e.mov_reg64(1, 10);
        e.call((void*)ee_mem_read128_wrapper);
        e.ldr64(9, 19, gpr_off(rt));
        e.str64(9, 19, OFF_GPR_HI + rt * 8);
        return 0;
    }
    case OP_SQ: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.mov_imm32(10, gpr_off(rt)); e.add64(10, 19, 10);
        e.mov_reg64(1, 10);
        e.call((void*)ee_mem_write128_wrapper);
        return 0;
    }

    case OP_LWL: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_lwl);
        e.store_gpr(0, rt);
        return 0;
    }
    case OP_LWR: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_lwr);
        e.sxtw(0, 0);
        e.store_gpr(0, rt);
        return 0;
    }
    case OP_SWL: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_swl);
        return 0;
    }
    case OP_SWR: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_swr);
        return 0;
    }

    case OP_LDL: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_ldl);
        e.store_gpr(0, rt);
        return 0;
    }
    case OP_LDR: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_ldr);
        e.sxtw(0, 0);
        e.store_gpr(0, rt);
        return 0;
    }
    case OP_SDL: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_sdl);
        return 0;
    }
    case OP_SDR: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.load_gpr(1, rt);
        e.call((void*)ee_sdr);
        return 0;
    }

    case OP_COP0:
        if (rs == 0x00) {
            e.ldr32(9, 19, OFF_COP0 + rd*4); e.sxtw(9, 9); e.store_gpr(9, rt); return 0;
        } else if (rs == 0x04) {
            e.load_gpr(9, rt); e.str32(9, 19, OFF_COP0 + rd*4); return 0;
        } else if (rs == 0x10) {
            if (in.r.func == 0x18) { // ERET
                e.ldr32(21, 19, OFF_COP0 + 14*4);
                e.sxtw(21, 21);
                e.ldr32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(10, ~0x2u); 
                e.and64(9, 9, 10);
                e.str32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(20, 1); 
                return 1;
            } else if (in.r.func == 0x16) { // DI
                e.ldr32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(10, ~0x1u); 
                e.and64(9, 9, 10);
                e.str32(9, 19, OFF_COP0 + 12*4);
                return 0;
            } else if (in.r.func == 0x1B) { // EI
                e.ldr32(9, 19, OFF_COP0 + 12*4);
                e.mov_imm32(10, 0x1);
                e.orr64(9, 9, 10);
                e.str32(9, 19, OFF_COP0 + 12*4);
                return 0;
            }
        }
        emit_ill_insn(e, pc, insn);
        return 1;

    case OP_COP1:
        if (rs == 0x00) { // MFC1
            e.ldr32(9, 19, OFF_FPU + rd * 4);
            e.sxtw(9, 9);
            e.store_gpr(9, rt);
            return 0;
        } else if (rs == 0x04) { // MTC1
            e.load_gpr(9, rt);
            e.str32(9, 19, OFF_FPU + rd * 4);
            return 0;
        } else if (rs == 0x10) { // COP1 sub-ops
            switch (in.r.func) {
            case 0x00: // ADD.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.ldr_s(1, 19, OFF_FPU + rt * 4);
                e.fadd_s(0, 0, 1);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x01: // SUB.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.ldr_s(1, 19, OFF_FPU + rt * 4);
                e.fsub_s(0, 0, 1);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x02: // MUL.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.ldr_s(1, 19, OFF_FPU + rt * 4);
                e.fmul_s(0, 0, 1);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x03: // DIV.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.ldr_s(1, 19, OFF_FPU + rt * 4);
                e.fdiv_s(0, 0, 1);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x04: // SQRT.S
                e.ldr_s(0, 19, OFF_FPU + rt * 4);
                e.fsqrt_s(0, 0);
                e.str_s(0, 19, OFF_FPU + 0); // result to FPR[0]
                return 0;
            case 0x05: // ABS.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fabs_s(0, 0);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x06: // MOV.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x07: // NEG.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fneg_s(0, 0);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x08: // ROUND.W.S
            case 0x09: // TRUNC.W.S
            case 0x0A: // CEIL.W.S
            case 0x0B: // FLOOR.W.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fcvt_w_s(9, 0, 0);
                e.str32(9, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x20: // CVT.W.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fcvt_w_s(9, 0, 0);
                e.str32(9, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x24: // CVT.W.PL  (treat as CVT.W.S for now)
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fcvt_w_s(9, 0, 0);
                e.str32(9, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x25: // CVT.W.PH (treat as CVT.W.S)
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fcvt_w_s(9, 0, 0);
                e.str32(9, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x28: // CVT.S.PL (treat as no-op)
                return 0;
            case 0x29: // CVT.S.PH (treat as no-op)
                return 0;
            case 0x30: // C.F.S
            case 0x31: // C.UN.S
            case 0x32: // C.EQ.S
            case 0x33: // C.UEQ.S
            case 0x34: // C.OLT.S
            case 0x35: // C.ULT.S
            case 0x36: // C.OLE.S
            case 0x37: // C.ULE.S
            case 0x38: // C.SF.S
            case 0x39: // C.NGLE.S
            case 0x3A: // C.SEQ.S
            case 0x3B: // C.NGL.S
            case 0x3C: // C.LT.S
            case 0x3D: // C.NGE.S
            case 0x3E: // C.LE.S
            case 0x3F: // C.NGT.S
                return 0; // FPU compare flags (simplified)
            case 0x49: // ABS.PS
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fabs_s(0, 0);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x50: // C.ABS.S
            case 0x51: // MOVF
            case 0x52: // MOVT
                return 0;
            case 0x53: // MOVZ.F
            case 0x54: // MOVN.F
                return 0;
            case 0x5C: // NEG.PS
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fneg_s(0, 0);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x5E: // MADD.S
                e.ldr_s(0, 19, OFF_FPU + (sa) * 4);
                e.ldr_s(1, 19, OFF_FPU + rs * 4);
                e.ldr_s(2, 19, OFF_FPU + rt * 4);
                e.fmadd_s(0, 1, 2, sa);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x5F: // MSUB.S
                e.ldr_s(0, 19, OFF_FPU + (sa) * 4);
                e.ldr_s(1, 19, OFF_FPU + rs * 4);
                e.ldr_s(2, 19, OFF_FPU + rt * 4);
                e.fmsub_s(0, 1, 2, sa);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            default:
                emit_ill_insn(e, pc, insn);
                return 1;
            }
        } else if (rs == 0x11) { // COP1.D (Double-precision)
            switch (in.r.func) {
            case 0x00: // ADD.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.ldr_d(1, 19, OFF_FPU + rt * 8);
                e.fadd_d(0, 0, 1);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x01: // SUB.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.ldr_d(1, 19, OFF_FPU + rt * 8);
                e.fsub_d(0, 0, 1);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x02: // MUL.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.ldr_d(1, 19, OFF_FPU + rt * 8);
                e.fmul_d(0, 0, 1);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x03: // DIV.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.ldr_d(1, 19, OFF_FPU + rt * 8);
                e.fdiv_d(0, 0, 1);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x05: // ABS.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.fabs_s(0, 0);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x06: // MOV.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x07: // NEG.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.fneg_s(0, 0);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x20: // CVT.S.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.fcvt_s_d(0, 0);
                e.str_s(0, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x21: // CVT.D.S
                e.ldr_s(0, 19, OFF_FPU + rs * 4);
                e.fcvt_d_s(0, 0);
                e.str_d(0, 19, OFF_FPU + rd * 8);
                return 0;
            case 0x24: // CVT.W.D
                e.ldr_d(0, 19, OFF_FPU + rs * 8);
                e.fcvt_w_d(9, 0, 0);
                e.str32(9, 19, OFF_FPU + rd * 4);
                return 0;
            case 0x30: // C.F.D
            case 0x32: // C.EQ.D
            case 0x3C: // C.LT.D
            case 0x3E: // C.LE.D
                return 0; // Double-precision compare
            default:
                emit_ill_insn(e, pc, insn);
                return 1;
            }
        }
        emit_ill_insn(e, pc, insn);
        return 1;

    case OP_COP2:
        if (rs == 0x00) { // QMFC2
            e.load_gpr(9, rt); e.str64(9, 19, OFF_GPR_HI + rt * 8);
            return 0;
        } else if (rs == 0x04) { // QMTC2
            e.load_gpr(9, rt); e.str64(9, 19, OFF_GPR_HI + rt * 8);
            return 0;
        }
        emit_ill_insn(e, pc, insn);
        return 1;

    case OP_LWC1: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.call((void*)ee_mem_read32);
        e.sxtw(0, 0);
        e.str32(0, 19, OFF_FPU + rt * 4);
        return 0;
    }
    case OP_SWC1: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.ldr32(1, 19, OFF_FPU + rt * 4);
        e.mov_reg64(0, 9);
        e.call((void*)ee_mem_write32);
        return 0;
    }
    case OP_LDC1: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.call((void*)ee_mem_read64);
        e.str64(0, 19, OFF_FPU + rt * 8);
        return 0;
    }
    case OP_SDC1: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.ldr64(1, 19, OFF_FPU + rt * 8);
        e.mov_reg64(0, 9);
        e.call((void*)ee_mem_write64);
        return 0;
    }
    case OP_LWC2: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.mov_reg64(0, 9);
        e.call((void*)ee_mem_read32);
        e.sxtw(0, 0);
        e.store_gpr(0, rt);
        return 0;
    }
    case OP_SWC2: {
        e.load_gpr(9, rs); e.mov_imm64(10, uint64_t(int64_t(imm))); e.add64(9, 9, 10);
        e.load_gpr(1, rt);
        e.mov_reg64(0, 9);
        e.call((void*)ee_mem_write32);
        return 0;
    }

    case OP_MMI: {
        switch (in.r.func & 0x3F) {
        case 0x00: // PMFHL.LW
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.add32(9, 9, 10);
            e.sxtw(9, 9);
            e.store_gpr(9, rd);
            return 0;
        case 0x01: // PMTHL.LW
            e.load_gpr(9, rs);
            e.str64(9, 19, OFF_HI);
            return 0;
        case 0x04: // PSLLVW
        case 0x05: // PSRAVW
            return 0;
        case 0x08: // PMADDW
        case 0x09: // PMADDU.W
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.add64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x0C: // PMAXW
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.cmp64(9, 10);
            e.cset64(11, 0xB);
            e.store_gpr(11, rd);
            return 0;
        case 0x0D: // PMAXH
            return 0;
        case 0x0E: // PCGTB
            return 0;
        case 0x0F: // PCGTH
            return 0;
        case 0x10: // PCGTW
            return 0;
        case 0x11: // PMINW
            return 0;
        case 0x12: // PMINH
            return 0;
        case 0x13: // PADDB
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.add32(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x14: // PADDH
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.add32(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x15: // PADDW
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.add64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x16: // PSUBB
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.sub32(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x17: // PSUBH
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.sub32(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x18: // PSUBW
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.sub64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x1C: // PEXTLB
        case 0x1D: // PEXTLH
        case 0x1E: // PEXTLW
        case 0x20: // PEXTUB
        case 0x21: // PEXTUH
        case 0x22: // PEXTUW
            return 0;
        case 0x24: // PADDQ
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.add64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x25: // PSUBQ
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.sub64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x2C: // PAND
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.and64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x2D: // PXOR
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.eor64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x2E: // POR
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.orr64(9, 9, 10);
            e.store_gpr(9, rd);
            return 0;
        case 0x2F: // PNOR
            e.load_gpr(9, rs); e.load_gpr(10, rt);
            e.orr64(9, 9, 10);
            e.mvn64(9, 9);
            e.store_gpr(9, rd);
            return 0;
        default:
            emit_ill_insn(e, pc, insn);
            return 1;
        }
    }

    case OP_CACHE: case OP_PREF: return 0;

    default:
        emit_ill_insn(e, pc, insn);
        return 1;
    }
}

EE_Recompiler::EE_Recompiler(CodeCache& c, EE_State& s, uint8_t* ram)
    : m_cache(c), m_state(s), m_ram(ram) {}

void EE_Recompiler::invalidate(uint32_t s, uint32_t e) { m_cache.invalidate_range(s, e); }

EE_Recompiler::CompiledBlock EE_Recompiler::compile_block(uint32_t guest_pc) {
    // ESPIA DE RECOMPILACIÓN
    static int compile_log_count = 0;
    if (compile_log_count < 50) {
        uint32_t first_instr = ee_mem_read32(guest_pc);
        bool is_bios = (guest_pc >= 0xBFC00000u && guest_pc < 0xC0000000u);
        LOGI("JIT Compilar[%d] PC: 0x%08X%s | Instr: 0x%08X",
             compile_log_count, guest_pc, is_bios ? " (BIOS)" : "", first_instr);
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
    uint8_t* cbz_patch = nullptr;

    for (uint32_t i = 0; i < MAX_INSNS; ++i) {
        uint32_t instr = ee_mem_read32(pc);
        int term = emit_mips(e, instr, pc);
        
        if (term && !terminated) {
            terminated = true;
            branch_pc  = pc;

            if (term == 2) {
                cbz_patch = e.p;
                e.cbz64(20, 0);
            }

            pc += 4;
            continue;
        }
        
        pc += 4;
        if (terminated) break;
    }

    if (cbz_patch) {
        int32_t skip = int32_t(e.p - cbz_patch);
        int32_t imm19 = skip / 4;
        uint32_t* insn = reinterpret_cast<uint32_t*>(cbz_patch);
        *insn = 0xB4000000u | ((uint32_t(imm19) & 0x7FFFF) << 5) | 20;
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

    static int compile_done_count = 0;
    if (compile_done_count < 50) {
        LOGI("JIT OK PC=0x%08X code=%p size=%zu", guest_pc, (void*)fn, code_size);
        compile_done_count++;
    }

    return fn;
}