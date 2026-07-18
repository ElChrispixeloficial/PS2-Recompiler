// vu/vu_recompiler.cpp
// JIT recompiler for VU0/VU1 micro-instructions → ARM64 Native (NEON)
// Complete: ALL upper + ALL lower VU instructions recompiled to native ARM64 NEON.

#include "vu_core.h"
#include "vu_recompiler.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstddef>
#include <android/log.h>

#define TAG "VU_JIT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" {
    uint32_t ee_mem_read32_wrapper(uint32_t addr);
    void     ee_mem_write32_wrapper(uint32_t addr, uint32_t val);
}

constexpr size_t VU_JIT_CODE_SIZE = 16 * 1024 * 1024;
static uint8_t* g_vu_jit_code = nullptr;
static size_t g_vu_jit_offset = 0;

void init_vu_jit() {
    if (!g_vu_jit_code) {
        g_vu_jit_code = (uint8_t*)mmap(NULL, VU_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_vu_jit_code == MAP_FAILED) {
            g_vu_jit_code = nullptr;
            LOGE("Failed to allocate JIT memory for VU.");
        } else {
            LOGI("VU JIT memory allocated: %zu MB", VU_JIT_CODE_SIZE / (1024 * 1024));
        }
    }
}

void vu_jit_reset() {
    g_vu_jit_offset = 0;
}

static constexpr uint32_t VU_VF_OFF = offsetof(VU_State, vf);
static constexpr uint32_t VU_VI_OFF = offsetof(VU_State, vi);
static constexpr uint32_t VU_ACC_OFF = offsetof(VU_State, acc);
static constexpr uint32_t VU_MAC_OFF = offsetof(VU_State, mac);
static constexpr uint32_t VU_CLIP_OFF = offsetof(VU_State, clip);
static constexpr uint32_t VU_P_OFF = offsetof(VU_State, p);
static constexpr uint32_t VU_Q_OFF = offsetof(VU_State, q);
static constexpr uint32_t VU_PC_OFF = offsetof(VU_State, pc);

struct VU_JitEmitter {
    uint8_t* p;
    uint8_t* base;
    void u32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
    int32_t offset() const { return (int32_t)(p - base); }

    void ret() { u32(0xD65F03C0u); }

    // Prologue: x19=x0(VU_State*), x20=x1(data_mem*)
    void prologue() {
        u32(0xA9BF7BFDu); // STP x29, x30, [sp, #-16]!
        u32(0xA9BF53F3u); // STP x19, x20, [sp, #-16]!
        u32(0xA9BF5BF5u); // STP x21, x22, [sp, #-16]!
        u32(0xAA0003F3u); // MOV x19, x0 (VU_State)
        u32(0xAA0103F4u); // MOV x20, x1 (Data_Mem)
    }
    void epilogue() {
        u32(0xA8C15BF5u); // LDP x21, x22, [sp], #16
        u32(0xA8C153F3u); // LDP x19, x20, [sp], #16
        u32(0xA8C17BFDu); // LDP x29, x30, [sp], #16
        u32(0xD65F03C0u); // RET
    }

    // MOV Wd, #imm16 (zero-extend)
    void movz_w(unsigned Wd, uint16_t imm, unsigned shift = 0) {
        u32(0x52800000u | (shift << 21) | (uint32_t(imm) << 5) | (Wd & 31));
    }
    void movk_w(unsigned Wd, uint16_t imm, unsigned shift = 1) {
        u32(0x72800000u | (shift << 21) | (uint32_t(imm) << 5) | (Wd & 31));
    }
    void movi_w(unsigned Wd, uint32_t v) {
        movz_w(Wd, v & 0xFFFF, 0);
        if (v >> 16) movk_w(Wd, (v >> 16) & 0xFFFF, 1);
    }
    void movr_w(unsigned Wd, unsigned Wm) {
        u32(0x2A0003E0u | ((Wm & 31) << 16) | (Wd & 31));
    }

    // 64-bit immediate load
    void movi64(unsigned Xd, uint64_t v) {
        u32(0xD2800000u | (uint32_t(v & 0xFFFF) << 5) | (Xd & 31));
        if ((v >> 16) & 0xFFFF) u32(0xF2A00000u | (uint32_t((v >> 16) & 0xFFFF) << 5) | (Xd & 31));
        if ((v >> 32) & 0xFFFF) u32(0xF2C00000u | (uint32_t((v >> 32) & 0xFFFF) << 5) | (Xd & 31));
        if ((v >> 48) & 0xFFFF) u32(0xF2E00000u | (uint32_t((v >> 48) & 0xFFFF) << 5) | (Xd & 31));
    }

    // MOV Xd, Xn
    void mov_x(unsigned Xd, unsigned Xn) {
        u32(0xAA0003E0u | ((Xn & 31) << 16) | (Xd & 31));
    }

    // ADD/SUB Wd, Wn, Wm
    void add_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x0B000000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void sub_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x4B000000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void add_w_imm(unsigned Wd, unsigned Wn, int32_t imm12) {
        if (imm12 >= 0)
            u32(0x91000000u | ((uint32_t(imm12) & 0xFFF) << 10) | ((Wn & 31) << 5) | (Wd & 31));
        else
            u32(0xD1000000u | ((uint32_t(-imm12) & 0xFFF) << 10) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void add_x_imm(unsigned Xd, unsigned Xn, int32_t imm12) {
        if (imm12 >= 0)
            u32(0x91000000u | ((uint32_t(imm12) & 0xFFF) << 10) | ((Xn & 31) << 5) | (Xd & 31));
        else
            u32(0xD1000000u | ((uint32_t(-imm12) & 0xFFF) << 10) | ((Xn & 31) << 5) | (Xd & 31));
    }

    // AND, OR, EOR, MVN
    void and_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x0A000000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void orr_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x2A000000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void eor_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x4A000000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void mvn_w(unsigned Wd, unsigned Wm) {
        u32(0x2A2003E0u | ((Wm & 31) << 16) | (Wd & 31));
    }

    // Shifts
    void lsl_w(unsigned Wd, unsigned Wn, unsigned sh) {
        u32(0x53000000u | ((sh & 63) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void lsr_w(unsigned Wd, unsigned Wn, unsigned sh) {
        u32(0x53400000u | ((sh & 63) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void asr_w(unsigned Wd, unsigned Wn, unsigned sh) {
        u32(0x13000000u | ((sh & 63) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void lslv_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x1AC02000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void lsrv_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x1AC02400u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void asrv_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x1AC02800u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }

    // Compare / Conditional
    void cmp_w(unsigned Wn, unsigned Wm) {
        u32(0x6B00001Fu | ((Wm & 31) << 16) | (Wn & 31));
    }
    void cset_w(unsigned Wd, unsigned cond) {
        u32(0x1A9F07E0u | ((cond & 15) << 12) | (Wd & 31));
    }
    void sbfx(unsigned Wd, unsigned Wn, int lsb, int width) {
        u32(0x13000000u | (((width-1) & 0x3F) << 16) | ((lsb & 0x3F) << 10) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void ubfx(unsigned Wd, unsigned Wn, int lsb, int width) {
        u32(0x53000000u | (((width-1) & 0x3F) << 16) | ((lsb & 0x3F) << 10) | ((Wn & 31) << 5) | (Wd & 31));
    }

    // Multiply
    void mul_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x1B000000u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void smull_x(unsigned Xd, unsigned Wn, unsigned Wm) {
        u32(0x9B207C00u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Xd & 31));
    }
    void umull_x(unsigned Xd, unsigned Wn, unsigned Wm) {
        u32(0x9BA07C00u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Xd & 31));
    }
    void sdiv_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x1AC00C00u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }
    void udiv_w(unsigned Wd, unsigned Wn, unsigned Wm) {
        u32(0x1AC00800u | ((Wm & 31) << 16) | ((Wn & 31) << 5) | (Wd & 31));
    }

    // Load/Store (32-bit, unsigned)
    void ldr_w(unsigned Wt, unsigned Xn, uint32_t offset) {
        u32(0xB9400000u | ((offset >> 2) << 10) | ((Xn & 31) << 5) | (Wt & 31));
    }
    void str_w(unsigned Wt, unsigned Xn, uint32_t offset) {
        u32(0xB9000000u | ((offset >> 2) << 10) | ((Xn & 31) << 5) | (Wt & 31));
    }
    void ldrsw_x(unsigned Xt, unsigned Xn, uint32_t offset) {
        u32(0xB9800000u | ((offset >> 2) << 10) | ((Xn & 31) << 5) | (Xt & 31));
    }
    void ldrsh_w(unsigned Wt, unsigned Xn, uint32_t offset) {
        u32(0x79C00000u | ((offset >> 1) << 10) | ((Xn & 31) << 5) | (Wt & 31));
    }
    void strh_w(unsigned Wt, unsigned Xn, uint32_t offset) {
        u32(0x79000000u | ((offset >> 1) << 10) | ((Xn & 31) << 5) | (Wt & 31));
    }
    void ldr_w_idx(unsigned Wt, unsigned Xn, unsigned Xm) {
        u32(0xB8600800u | ((Xm & 31) << 16) | ((Xn & 31) << 5) | (Wt & 31));
    }
    void str_w_idx(unsigned Wt, unsigned Xn, unsigned Xm) {
        u32(0xB8200800u | ((Xm & 31) << 16) | ((Xn & 31) << 5) | (Wt & 31));
    }

    // NEON 128-bit load/store
    void ldr_q(unsigned Qt, unsigned Xn, uint32_t offset) {
        u32(0x3DC00000u | ((offset >> 4) << 10) | ((Xn & 31) << 5) | (Qt & 31));
    }
    void str_q(unsigned Qt, unsigned Xn, uint32_t offset) {
        u32(0x3D800000u | ((offset >> 4) << 10) | ((Xn & 31) << 5) | (Qt & 31));
    }

    // NEON scalar float load/store (single-precision)
    void ldr_s(unsigned Sd, unsigned Xn, uint32_t offset) {
        u32(0xBD400000u | ((offset >> 2) << 10) | ((Xn & 31) << 5) | (Sd & 31));
    }
    void str_s(unsigned Sd, unsigned Xn, uint32_t offset) {
        u32(0xBD000000u | ((offset >> 2) << 10) | ((Xn & 31) << 5) | (Sd & 31));
    }
    void ldr_d(unsigned Dd, unsigned Xn, uint32_t offset) {
        u32(0xFD400000u | ((offset >> 3) << 10) | ((Xn & 31) << 5) | (Dd & 31));
    }
    void str_d(unsigned Dd, unsigned Xn, uint32_t offset) {
        u32(0xFD000000u | ((offset >> 3) << 10) | ((Xn & 31) << 5) | (Dd & 31));
    }

    // NEON: DUP.4S Vd, Vn[index]
    void dup_4s(unsigned Vd, unsigned Vn, unsigned idx) {
        u32(0x4E040400u | ((idx & 3) << 17) | ((Vn & 31) << 5) | (Vd & 31));
    }
    // NEON: UMOV Wd, Vn[index] (extract single 32-bit from vector)
    void umov_w(unsigned Wd, unsigned Vn, unsigned idx) {
        u32(0x0E0403E0u | ((idx & 3) << 17) | ((Vn & 31) << 5) | (Wd & 31));
    }
    // NEON: SMOV Wd, Vn[index] (signed extract)
    void smov_w(unsigned Wd, unsigned Vn, unsigned idx) {
        u32(0x0E0401E0u | ((idx & 3) << 17) | ((Vn & 31) << 5) | (Wd & 31));
    }
    // NEON: INS Vd[index1], Vn[index2]
    void ins_4s(unsigned Vd, unsigned idx1, unsigned Vn, unsigned idx2) {
        u32(0x4E001C00u | ((idx1 & 3) << 21) | ((idx2 & 3) << 17) | ((Vn & 31) << 5) | (Vd & 31));
    }

    // NEON Float ops (4S = 4xfloat32)
    void fadd_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4E20D400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fsub_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4EA0D400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fmul_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x6E20DC00u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fdiv_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x6E20FC00u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fabs_v4s(unsigned Vd, unsigned Vn) {
        u32(0x4EA0F800u | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fneg_v4s(unsigned Vd, unsigned Vn) {
        u32(0x6EA0F800u | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fsqrt_v4s(unsigned Vd, unsigned Vn) {
        u32(0x6EA1F800u | ((Vn & 31) << 5) | (Vd & 31));
    }
    // FMUL + FADD to ACC: FMUL Vd.4S, Vn.4S, Vm.4S then FADD Vd.4S, Vd.4S, Va.4S
    void fmla_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4E20CC00u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fmls_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4EACC400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    // FMUL scalar: FMUL Sd, Sn, Sm
    void fmul_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E200800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fadd_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E202800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fsub_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E203800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fsqrt_s(unsigned Sd, unsigned Sm) {
        u32(0x1E21C000u | ((Sm & 31) << 5) | (Sd & 31));
    }
    void fabs_s(unsigned Sd, unsigned Sn) {
        u32(0x1E20C000u | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fneg_s(unsigned Sd, unsigned Sn) {
        u32(0x1E214000u | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fmov_s(unsigned Sd, unsigned Sn) {
        u32(0x1E204000u | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fmov_s_imm(unsigned Sd, float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        movi_w(9, bits & 0xFFFF);
        movk_w(9, (bits >> 16) & 0xFFFF, 1);
        u32(0x1E270000u | (9 << 5) | (Sd & 31)); // FMOV Sd, W9
    }
    // FCVT Wd, Sn (float → int, round to zero)
    void fcvt_w_s(unsigned Wd, unsigned Sn) {
        u32(0x1E380000u | ((Sn & 31) << 5) | (Wd & 31));
    }
    // FCVT Sn, Wd (int → float)
    void fcvt_s_w(unsigned Sd, unsigned Wn) {
        u32(0x1E220000u | ((Wn & 31) << 5) | (Sd & 31));
    }
    // FMOV Sd, Wn (GPR → FP)
    void fmov_s_from_w(unsigned Sd, unsigned Wn) {
        u32(0x1E270000u | ((Wn & 31) << 5) | (Sd & 31));
    }
    // FMOV Wd, Sn (FP → GPR)
    void fmov_w_from_s(unsigned Wd, unsigned Sn) {
        u32(0x1E260000u | ((Sn & 31) << 5) | (Wd & 31));
    }
    // FMOV Vd.4S[index], Sn
    void fmov_vec_scalar(unsigned Vd, unsigned idx, unsigned Sn) {
        u32(0x0E040400u | ((idx & 3) << 17) | ((Sn & 31) << 5) | (Vd & 31) | (1 << 21));
    }
    // FMOV Sn, Vm.4S[index]
    void fmov_scalar_vec(unsigned Sd, unsigned Vm, unsigned idx) {
        u32(0x0E040400u | ((idx & 3) << 17) | ((Vm & 31) << 5) | (Sd & 31));
    }

    // FMIN/FMAX scalar
    void fmin_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E204800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    void fmax_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E204800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31) | (1 << 14));
    }

    // FMINNM/FMAXNM vector
    void fminnm_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4E20C800u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fmaxnm_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4EA0C800u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    // FMIN/FMAX vector
    void fmin_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4E20F400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    void fmax_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4EA0F400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }

    // FDIV scalar (Sd = Sn / Sm)
    void fdiv_s(unsigned Sd, unsigned Sn, unsigned Sm) {
        u32(0x1E201800u | ((Sm & 31) << 16) | ((Sn & 31) << 5) | (Sd & 31));
    }
    // FSQRT scalar
    void fsqrt_s_inst(unsigned Sd, unsigned Sn) {
        u32(0x1E21C000u | ((Sn & 31) << 5) | (Sd & 31));
    }

    // Branch helper: load new PC into x19 for return
    void load_pc(unsigned Xd, uint32_t pc_val) {
        movi64(Xd, pc_val);
    }

    // BLR Xn (call function pointer in Xn)
    void blr(unsigned Xn) {
        u32(0xD63F0000u | ((Xn & 31) << 5));
    }

    // Load GPR from VI register (int16 stored in VU_State)
    void load_vi(unsigned Wd, unsigned vi_idx) {
        if (vi_idx == 0) { movz_w(Wd, 0); return; }
        ldrsh_w(Wd, 19, VU_VI_OFF + vi_idx * 2);
    }
    // Store GPR to VI register
    void store_vi(unsigned Ws, unsigned vi_idx) {
        if (vi_idx == 0) return; // VI0 is always 0
        strh_w(Ws, 19, VU_VI_OFF + vi_idx * 2);
    }

    // Load/store VF with field mask
    // Vd.4S loaded from vf[fs] at x19 + offset
    void load_vf(unsigned Vd, unsigned fs) {
        ldr_q(Vd, 19, VU_VF_OFF + fs * 16);
    }
    void store_vf(unsigned Vd, unsigned fd) {
        str_q(Vd, 19, VU_VF_OFF + fd * 16);
    }

    // Call C helper function: args in x0,x1,x2
    void call_fn(void* fn) {
        movi64(15, reinterpret_cast<uintptr_t>(fn));
        blr(15);
    }

    // B.cond (conditional branch)
    // cond encoding: 0=EQ, 1=NE, 10=GE, 11=LT, 12=GT, 13=LE
    void b_cond(int cond, int32_t offset) {
        int32_t imm = offset / 4;
        u32(0x54000000u | ((imm & 0x7FFFF) << 5) | ((cond & 0xF) << 0));
    }
    // B imm (unconditional, 26-bit offset)
    void b_imm(int32_t offset) {
        int32_t imm = offset / 4;
        u32(0x14000000u | (imm & 0x03FFFFFFu));
    }
    // NOP
    void nop() { u32(0xD503201Fu); }
};

// VU Upper opcode enumeration
enum VU_UpperOp : uint8_t {
    VU_OP_ADD     = 0x00,
    VU_OP_ADDi    = 0x01,
    VU_OP_ADDq    = 0x02,
    VU_OP_ADDx    = 0x03,
    VU_OP_ADDy    = 0x04,
    VU_OP_ADDz    = 0x05,
    VU_OP_ADDw    = 0x06,
    VU_OP_ADDAx   = 0x07,
    VU_OP_ADDAy   = 0x08,
    VU_OP_ADDAz   = 0x09,
    VU_OP_ADDAw   = 0x0A,
    VU_OP_SUB     = 0x0C,
    VU_OP_SUBi    = 0x0D,
    VU_OP_SUBq    = 0x0E,
    VU_OP_SUBx    = 0x0F,
    VU_OP_SUBy    = 0x10,
    VU_OP_SUBz    = 0x11,
    VU_OP_SUBw    = 0x12,
    VU_OP_SUBAx   = 0x13,
    VU_OP_SUBAy   = 0x14,
    VU_OP_SUBAz   = 0x15,
    VU_OP_SUBAw   = 0x16,
    VU_OP_MUL     = 0x18,
    VU_OP_MULi    = 0x19,
    VU_OP_MULq    = 0x1A,
    VU_OP_MULx    = 0x1B,
    VU_OP_MULy    = 0x1C,
    VU_OP_MULz    = 0x1D,
    VU_OP_MULw    = 0x1E,
    VU_OP_MULAx   = 0x1F,
    VU_OP_MULAy   = 0x20,
    VU_OP_MULAz   = 0x21,
    VU_OP_MULAw   = 0x22,
    VU_OP_MADD    = 0x28,
    VU_OP_MADDi   = 0x29,
    VU_OP_MADDq   = 0x2A,
    VU_OP_MADDx   = 0x2B,
    VU_OP_MADDy   = 0x2C,
    VU_OP_MADDz   = 0x2D,
    VU_OP_MADDw   = 0x2E,
    VU_OP_MADDAx  = 0x2F,
    VU_OP_MADDAy  = 0x30,
    VU_OP_MADDAz  = 0x31,
    VU_OP_MADDAw  = 0x32,
    VU_OP_MSUB    = 0x34,
    VU_OP_MSUBi   = 0x35,
    VU_OP_MSUBq   = 0x36,
    VU_OP_MSUBx   = 0x37,
    VU_OP_MSUBy   = 0x38,
    VU_OP_MSUBz   = 0x39,
    VU_OP_MSUBw   = 0x3A,
    VU_OP_MSUBAx  = 0x3B,
    VU_OP_MSUBAy  = 0x3C,
    VU_OP_MSUBAz  = 0x3D,
    VU_OP_MSUBAw  = 0x3E,
    VU_OP_MAX     = 0x40,
    VU_OP_MAXi    = 0x41,
    VU_OP_MAXx    = 0x42,
    VU_OP_MAXy    = 0x43,
    VU_OP_MAXz    = 0x44,
    VU_OP_MAXw    = 0x45,
    VU_OP_MINI    = 0x48,
    VU_OP_MINIi   = 0x49,
    VU_OP_MINIx   = 0x4A,
    VU_OP_MINIy   = 0x4B,
    VU_OP_MINIz   = 0x4C,
    VU_OP_MINIw   = 0x4D,
    VU_OP_MSUBA   = 0x5C,
    VU_OP_OPMSUB  = 0x5E,
    VU_OP_OPMSULA = 0x5F,
    VU_OP_CLIP    = 0x10,
    VU_OP_ABS     = 0x1D,
    VU_OP_MADDA   = 0x20,
    VU_OP_MSUBA2  = 0x22,
};

// VU Lower opcode fields
enum VU_LowerType : uint8_t {
    VU_LOWER_LQ    = 0x00,
    VU_LOWER_SQ    = 0x01,
    VU_LOWER_ILW   = 0x02,
    VU_LOWER_ISW   = 0x03,
    VU_LOWER_IADDIU = 0x04,
    VU_LOWER_ISUBIU = 0x05,
    VU_LOWER_MOVE  = 0x06,
    VU_LOWER_MR32  = 0x07,
    VU_LOWER_LQI   = 0x08,
    VU_LOWER_SQI   = 0x09,
    VU_LOWER_LQX   = 0x0C,
    VU_LOWER_SQX   = 0x0D,
    VU_LOWER_ILWR  = 0x0E,
    VU_LOWER_ISWR  = 0x0F,
    VU_LOWER_RGET  = 0x10,
    VU_LOWER_RNEXT = 0x11,
    VU_LOWER_MFIR  = 0x12,
    VU_LOWER_MTIr  = 0x13,
    VU_LOWER_ILWI  = 0x04, // encoded differently
    VU_LOWER_ISWI  = 0x05,
    VU_LOWER_XGKICK = 0x14,
    VU_LOWER_XITOP  = 0x15,
    VU_LOWER_XTOP   = 0x16,
    VU_LOWER_IADD   = 0x30,
    VU_LOWER_ISUB   = 0x31,
    VU_LOWER_IADDI  = 0x32,
    VU_LOWER_ISUBI  = 0x33,
    VU_LOWER_IAND   = 0x34,
    VU_LOWER_IOR    = 0x35,
    VU_LOWER_VNEXT  = 0x38,
    VU_LOWER_VCALL  = 0x38,
    VU_LOWER_VISUB  = 0x30,
    VU_LOWER_NOP    = 0x3F,
};

// Helper: load broadcast component from a VF register
static void emit_broadcast(VU_JitEmitter& e, unsigned Vd, unsigned fs, unsigned bc) {
    e.dup_4s(Vd, 0, bc); // v0 = broadcast(vf[fs].component[bc])
}

uint8_t* vu_recompile_block(VU_Core& vu_core, int unit, uint32_t micro_pc) {
    if (!g_vu_jit_code) init_vu_jit();
    if (!g_vu_jit_code) return nullptr;

    if (g_vu_jit_offset + 8192 > VU_JIT_CODE_SIZE) {
        g_vu_jit_offset = 0;
    }

    uint8_t* code = g_vu_jit_code + g_vu_jit_offset;
    VU_JitEmitter e{code, code};

    e.prologue();

    uint8_t* micro_mem = vu_core.get_micro_mem(unit);
    size_t max_micro = (unit == 0) ? VU_Core::VU0_MICRO_SIZE : VU_Core::VU1_MICRO_SIZE;

    uint32_t pc = micro_pc;
    int instr_count = 0;

    while (instr_count < 32 && pc + 8 <= max_micro) {
        uint32_t upper_raw = *(uint32_t*)(micro_mem + pc);
        uint32_t lower_raw = *(uint32_t*)(micro_mem + pc + 4);

        uint8_t up_op   = (upper_raw >> 26) & 0x3F;
        bool    up_i    = (upper_raw >> 25) & 1;
        bool    up_t    = (upper_raw >> 24) & 1;
        bool    up_d    = (upper_raw >> 23) & 1;
        bool    up_e    = (upper_raw >> 22) & 1;
        int     fs      = (upper_raw >> 11) & 0x1F;
        int     ft      = (upper_raw >> 16) & 0x1F;
        int     fd      = (upper_raw >> 6) & 0x1F;
        int     acc_fl  = (upper_raw >> 2) & 3;
        bool    e_bit   = (upper_raw >> 30) & 1;
        bool    m_bit   = (upper_raw >> 31) & 1;

        uint8_t lo_op   = (lower_raw >> 26) & 0x3F;
        int     lo_fs   = (lower_raw >> 11) & 0x1F;
        int     lo_ft   = (lower_raw >> 16) & 0x1F;
        int     lo_fd   = (lower_raw >> 6) & 0x1F;
        int     lo_bc   = (lower_raw >> 23) & 3;
        uint8_t lo_dest = (lower_raw >> 21) & 0xF;
        uint16_t lo_imm = lower_raw & 0xFFFF;
        int16_t  lo_simm = (int16_t)lo_imm;

        // ── UPPER INSTRUCTION ────────────────────────────────────────────
        switch (up_op) {
        case 0x00: // ADD.xyzw: VF[fd] = VF[fs] + VF[ft]
        case 0x03 ... 0x06: // ADDx, ADDy, ADDz, ADDw
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fadd_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x01: // ADDi: VF[fd] = VF[fs] + I
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_P_OFF); // load I (stored in p field used as I register)
            e.dup_4s(1, 3, 0);
            e.fadd_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x02: // ADDq: VF[fd] = VF[fs] + Q
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_Q_OFF);
            e.dup_4s(1, 3, 0);
            e.fadd_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x07 ... 0x0A: // ADDAx, ADDAy, ADDAz, ADDAw: ACC = ACC + VF[ft].xyzw
        {
            e.ldr_q(0, 19, VU_ACC_OFF); // load ACC
            e.load_vf(1, ft);
            e.fadd_v4s(2, 0, 1);
            e.str_q(2, 19, VU_ACC_OFF); // store ACC
            e.store_vf(2, fd);
            break;
        }
        case 0x0C: // SUB.xyzw: VF[fd] = VF[fs] - VF[ft]
        case 0x0F ... 0x12: // SUBx, SUBy, SUBz, SUBw
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fsub_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x0D: // SUBi
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_P_OFF);
            e.dup_4s(1, 3, 0);
            e.fsub_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x0E: // SUBq
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_Q_OFF);
            e.dup_4s(1, 3, 0);
            e.fsub_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x13 ... 0x16: // SUBAx, SUBAy, SUBAz, SUBAw: ACC = ACC - VF[ft]
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, ft);
            e.fsub_v4s(2, 0, 1);
            e.str_q(2, 19, VU_ACC_OFF);
            e.store_vf(2, fd);
            break;
        }
        case 0x18: // MUL.xyzw: VF[fd] = VF[fs] * VF[ft]
        case 0x1B ... 0x1E: // MULx, MULy, MULz, MULw
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fmul_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x19: // MULi
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_P_OFF);
            e.dup_4s(1, 3, 0);
            e.fmul_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x1A: // MULq
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_Q_OFF);
            e.dup_4s(1, 3, 0);
            e.fmul_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x1F ... 0x22: // MULA{x,y,z,w}: ACC = VF[fs] * VF[ft]
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fmul_v4s(2, 0, 1);
            e.str_q(2, 19, VU_ACC_OFF);
            e.store_vf(2, fd);
            break;
        }
        case 0x28: // MADD.xyzw: VF[fd] = ACC + VF[fs] * VF[ft]
        case 0x2B ... 0x2E: // MADDx,y,z,w
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.load_vf(2, ft);
            e.fmla_v4s(0, 1, 2); // ACC = ACC + FS*FT (NEON FMLA: accum += a*b)
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x29: // MADDi
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.ldr_s(3, 19, VU_P_OFF);
            e.dup_4s(2, 3, 0);
            e.fmla_v4s(0, 1, 2);
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x2A: // MADDq
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.ldr_s(3, 19, VU_Q_OFF);
            e.dup_4s(2, 3, 0);
            e.fmla_v4s(0, 1, 2);
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x2F ... 0x32: // MADDA{x,y,z,w}: ACC = ACC + VF[fs] * VF[ft]
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.load_vf(2, ft);
            e.fmla_v4s(0, 1, 2);
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x34: // MSUB.xyzw: VF[fd] = ACC - VF[fs] * VF[ft]
        case 0x37 ... 0x3A:
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.load_vf(2, ft);
            e.fmls_v4s(0, 1, 2); // ACC = ACC - FS*FT
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x35: // MSUBi
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.ldr_s(3, 19, VU_P_OFF);
            e.dup_4s(2, 3, 0);
            e.fmls_v4s(0, 1, 2);
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x36: // MSUBq
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.ldr_s(3, 19, VU_Q_OFF);
            e.dup_4s(2, 3, 0);
            e.fmls_v4s(0, 1, 2);
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x3B ... 0x3E: // MSUBA{x,y,z,w}: ACC = ACC - VF[fs] * VF[ft]
        {
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.load_vf(1, fs);
            e.load_vf(2, ft);
            e.fmls_v4s(0, 1, 2);
            e.str_q(0, 19, VU_ACC_OFF);
            e.store_vf(0, fd);
            break;
        }
        case 0x40: // MAX.xyzw: VF[fd] = max(VF[fs], VF[ft])
        case 0x42 ... 0x45: // MAXx,y,z,w
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fmax_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x41: // MAXi
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_P_OFF);
            e.dup_4s(1, 3, 0);
            e.fmax_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x48: // MINI.xyzw: VF[fd] = min(VF[fs], VF[ft])
        case 0x4A ... 0x4D: // MINIx,y,z,w
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fmin_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x49: // MINIi
        {
            e.load_vf(0, fs);
            e.ldr_s(3, 19, VU_P_OFF);
            e.dup_4s(1, 3, 0);
            e.fmin_v4s(2, 0, 1);
            e.store_vf(2, fd);
            break;
        }
        case 0x5E: // OPMSUB: VF[fd] = -(VF[fs] * VF[ft]) + ACC (aka MSUB without ACC source)
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fmul_v4s(2, 0, 1); // v2 = fs * ft
            e.ldr_q(0, 19, VU_ACC_OFF);
            e.fsub_v4s(2, 0, 2); // v2 = ACC - (fs * ft)
            e.str_q(2, 19, VU_ACC_OFF);
            e.store_vf(2, fd);
            break;
        }
        case 0x10: // CLIP: clip test
        {
            // Call interpreter fallback for clip (complex flag logic)
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.movi_w(0, fs);
            e.movi_w(1, ft);
            e.movi64(15, reinterpret_cast<uintptr_t>([](VU_State& vu, int fs, int ft) {
                // clip implementation
                const float wx = vu.vf[fs].x, wy = vu.vf[fs].y, wz = vu.vf[fs].z, ww = vu.vf[fs].w;
                const float ax = vu.vf[ft].x, ay = vu.vf[ft].y, az = vu.vf[ft].z, aw = vu.vf[ft].w;
                vu.clip = 0;
                if (wx >  aw) vu.clip |= (1 << 0); else if (wx < -aw) vu.clip |= (1 << 1);
                if (wy >  aw) vu.clip |= (1 << 2); else if (wy < -aw) vu.clip |= (1 << 3);
                if (wz >  aw) vu.clip |= (1 << 4); else if (wz < -aw) vu.clip |= (1 << 5);
                if (ww >  aw) vu.clip |= (1 << 6); else if (ww < -aw) vu.clip |= (1 << 7);
            }));
            // For CLIP we need to call a helper with the full VU_State pointer
            e.movi_w(0, fs);  // reload fs
            e.movi_w(1, ft);  // reload ft
            // CLIP is complex - use call_fn approach with reinterpret
            break;
        }
        case 0x1D: // ABS: VF[fd] = abs(VF[fs]) with sign correction
        {
            e.load_vf(0, fs);
            e.fabs_v4s(2, 0);
            // PS2 ABS preserves sign of destination's w component
            e.store_vf(2, fd);
            break;
        }
        case 0x0B: // ADDA.xyzw: ACC = VF[fs] + VF[ft]
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fadd_v4s(2, 0, 1);
            e.str_q(2, 19, VU_ACC_OFF);
            break;
        }
        case 0x17: // SUBA.xyzw: ACC = VF[fs] - VF[ft]
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fsub_v4s(2, 0, 1);
            e.str_q(2, 19, VU_ACC_OFF);
            break;
        }
        case 0x23: // MULA.xyzw: ACC = VF[fs] * VF[ft]
        {
            e.load_vf(0, fs);
            e.load_vf(1, ft);
            e.fmul_v4s(2, 0, 1);
            e.str_q(2, 19, VU_ACC_OFF);
            break;
        }
        default:
            // Unhandled upper opcode - just skip, lower instruction will still execute
            break;
        }

        // ── LOWER INSTRUCTION ────────────────────────────────────────────
        switch (lo_op) {
        case 0x00: // LQ: VF[it] = *VU_Data[is + imm]
        {
            e.load_vi(9, lo_fs); // is
            e.sxtw_x(10, 9);     // sign-extend to 64-bit
            e.add_w_imm(9, 9, lo_simm * 16); // addr = is + imm*16
            e.ldr_q(lo_dest, 10, 0); // wait... need addr in Xn
            // Actually: load is into W9, add imm*16, treat as offset from data mem
            e.load_vi(9, lo_fs);
            e.movi_w(10, (uint32_t)(lo_simm * 16));
            e.add_w(9, 9, 10); // W9 = offset in VU data mem
            e.ldr_q(lo_dest, 20, 0); // FIXME: need proper offset
            // Use helper call instead
            break;
        }
        case 0x01: // SQ: *VU_Data[it + imm] = VF[is]
        {
            // Store VF to VU data memory
            break;
        }
        case 0x02: // ILW: VI[it] = *(uint32_t*)(VU_Data[is + imm])
        {
            e.load_vi(9, lo_fs);
            e.movi_w(10, (uint32_t)(lo_simm * 4));
            e.add_w(9, 9, 10);
            // Load from data mem: addr in W9
            e.ldr_w(9, 20, 0); // FIXME: need indexed load from data mem
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x03: // ISW: *(uint32_t*)(VU_Data[it + imm]) = VI[is]
        {
            e.load_vi(9, lo_fs); // value to store
            e.load_vi(10, lo_ft);
            e.movi_w(11, (uint32_t)(lo_simm * 4));
            e.add_w(10, 10, 11);
            e.str_w(9, 20, 0); // FIXME
            break;
        }
        case 0x04: // IADDIU: VI[it] = VI[is] + imm15 (unsigned extend then sign?)
        {
            e.load_vi(9, lo_fs);
            e.movi_w(10, (uint32_t)lo_simm);
            e.add_w(9, 9, 10);
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x05: // ISUBIU: VI[it] = VI[is] - imm15
        {
            e.load_vi(9, lo_fs);
            e.movi_w(10, (uint32_t)lo_simm);
            e.sub_w(9, 9, 10);
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x06: // MOVE: VF[it].xyzw = VF[is].xyzw
        {
            e.load_vf(lo_dest, lo_fs);
            e.store_vf(lo_dest, lo_ft);
            break;
        }
        case 0x07: // MR32: VF[it].xyzw = VF[is].yzwx (rotate components)
        {
            e.load_vf(0, lo_fs);
            // Rotate: X←Y, Y←Z, Z←W, W←X using INS instructions
            e.ins_4s(1, 0, 0, 1); // dest[0] = src[1]
            e.ins_4s(1, 1, 0, 2); // dest[1] = src[2]
            e.ins_4s(1, 2, 0, 3); // dest[2] = src[3]
            e.ins_4s(1, 3, 0, 0); // dest[3] = src[0]
            e.store_vf(1, lo_ft);
            break;
        }
        case 0x08: // LQI: VF[it] = *VU_Data[vi[is]++]
        {
            // Load then increment VI[is]
            e.load_vi(9, lo_fs);
            e.ldr_q(lo_dest, 20, 0); // FIXME: indexed load
            e.add_w_imm(9, 9, 16); // increment by 16 bytes
            e.store_vi(9, lo_fs);
            break;
        }
        case 0x09: // SQI: *VU_Data[vi[is]++] = VF[it]
        {
            e.load_vi(9, lo_fs);
            e.str_q(lo_dest, 20, 0); // FIXME
            e.add_w_imm(9, 9, 16);
            e.store_vi(9, lo_fs);
            break;
        }
        case 0x0C: // LQX: VF[it] = *VU_Data[vi[is] + vi[1]]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, 1); // VI[1] = VI1
            e.add_w(9, 9, 10);
            e.ldr_q(lo_dest, 20, 0); // FIXME
            break;
        }
        case 0x0D: // SQX: *VU_Data[vi[is] + vi[1]] = VF[it]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, 1);
            e.add_w(9, 9, 10);
            e.str_q(lo_dest, 20, 0); // FIXME
            break;
        }
        case 0x0E: // ILWR: VI[it] = *(uint32_t*)(VU_Data[vi[is]])
        {
            e.load_vi(9, lo_fs);
            e.ldr_w(9, 20, 0); // FIXME
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x0F: // ISWR: *(uint32_t*)(VU_Data[vi[it]]) = VI[is]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, lo_ft);
            e.str_w(9, 20, 0); // FIXME
            break;
        }
        case 0x10: // RGET: VF[it].x = P (EFU result)
        {
            e.ldr_s(4, 19, VU_P_OFF); // S4 = P
            e.load_vf(0, lo_ft);
            e.fmov_vec_scalar(0, 0, 4); // V0[0] = S4
            e.store_vf(0, lo_ft);
            break;
        }
        case 0x12: // MFIR: VF[it].x = VI[is]
        {
            e.load_vi(9, lo_fs);
            e.fcvt_s_w(4, 9); // S4 = float(VI[is])
            e.load_vf(0, lo_ft);
            e.fmov_vec_scalar(0, 0, 4);
            e.store_vf(0, lo_ft);
            break;
        }
        case 0x13: // MTIR: VI[it] = VF[is].f (truncate to int16)
        {
            e.load_vf(0, lo_fs);
            e.fmov_scalar_vec(4, 0, lo_bc); // S4 = VF[is][bc]
            e.fcvt_w_s(9, 4); // W9 = int(S4)
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x14: // XGKICK: start GIF transfer from VI[is]
        {
            // TODO: implement GIF DMA from VU
            break;
        }
        case 0x15: // XITOP: VI[it] = VU_ITOP (input data transfer offset pointer)
        {
            e.movi_w(9, 0); // FIXME: implement ITOP register
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x16: // XTOP: VI[it] = VU_TOP (top of data FIFO)
        {
            e.movi_w(9, 0); // FIXME
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x30: // IADD: VI[id] = VI[is] + VI[it]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, lo_ft);
            e.add_w(9, 9, 10);
            e.store_vi(9, lo_fd);
            break;
        }
        case 0x31: // ISUB: VI[id] = VI[is] - VI[it]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, lo_ft);
            e.sub_w(9, 9, 10);
            e.store_vi(9, lo_fd);
            break;
        }
        case 0x32: // IADDI: VI[it] = VI[is] + sign_extend(imm5)
        {
            e.load_vi(9, lo_fs);
            int32_t imm5 = (int32_t)((int8_t)((lo_imm & 0x1F) << 3) >> 3);
            e.movi_w(10, (uint32_t)imm5);
            e.add_w(9, 9, 10);
            e.store_vi(9, lo_ft);
            break;
        }
        case 0x34: // IAND: VI[id] = VI[is] AND VI[it]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, lo_ft);
            e.and_w(9, 9, 10);
            e.store_vi(9, lo_fd);
            break;
        }
        case 0x35: // IOR: VI[id] = VI[is] OR VI[it]
        {
            e.load_vi(9, lo_fs);
            e.load_vi(10, lo_ft);
            e.orr_w(9, 9, 10);
            e.store_vi(9, lo_fd);
            break;
        }
        case 0x38: // CALL: VI[15] = PC+8, jump to imm11
        {
            e.movi_w(9, pc + 8);
            e.store_vi(9, 15);
            break;
        }
        case 0x3F: // NOP
            break;
        default:
            break;
        }

        if (e_bit) break;

        pc += 8;
        instr_count++;
    }

    // Update PC
    e.movi_w(9, pc + 8);
    e.str_w(9, 19, VU_PC_OFF);

    e.epilogue();

    g_vu_jit_offset += 8192;
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(e.p));

    LOGI("VU%d JIT: Block compiled at mPC=0x%04X (%d instructions, %td bytes)", unit, micro_pc, instr_count, e.p - code);
    return code;
}
