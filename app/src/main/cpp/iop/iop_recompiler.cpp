#include "iop_recompiler.h"
#include "iop_core.h"
#include "../ee/code_cache.h"
#include <android/log.h>
#include <cstring>
#include <cstddef>

#define TAG "IOP_JIT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" {
    uint32_t iop_bus_read32(uint32_t a);
    void     iop_bus_write32(uint32_t a, uint32_t v);
    uint16_t iop_bus_read16(uint32_t a);
    void     iop_bus_write16(uint32_t a, uint16_t v);
    uint8_t  iop_bus_read8 (uint32_t a);
    void     iop_bus_write8(uint32_t a, uint8_t v);
}
extern uint8_t* g_iop_ram_ptr;

static constexpr uint32_t OFF_GPR  = offsetof(IOP_State, gpr);
static constexpr uint32_t OFF_PC   = offsetof(IOP_State, pc);
static constexpr uint32_t OFF_HI   = offsetof(IOP_State, hi);
static constexpr uint32_t OFF_LO   = offsetof(IOP_State, lo);
static constexpr uint32_t OFF_COP0 = offsetof(IOP_State, cop0);
static inline uint32_t gpr_off(unsigned r) { return OFF_GPR + r * 4; }

struct E {
    uint8_t* p;
    void u32(uint32_t v) { memcpy(p, &v, 4); p += 4; }
    void movz(unsigned Wd, uint16_t i, unsigned s) { u32(0x52800000u | (s<<21) | (uint32_t(i)<<5) | (Wd&31)); }
    void movk(unsigned Wd, uint16_t i, unsigned s) { u32(0x72800000u | (s<<21) | (uint32_t(i)<<5) | (Wd&31)); }
    void movi32(unsigned Wd, uint32_t v) { movz(Wd, v & 0xFFFF, 0); if (v >> 16) movk(Wd, (v>>16)&0xFFFF, 1); }
    void movi64(unsigned Xd, uint64_t v) {
        u32(0xD2800000u | (uint32_t(v & 0xFFFF) << 5) | (Xd&31));
        if ((v>>16)&0xFFFF) u32(0xF2A00000u | (uint32_t((v>>16)&0xFFFF)<<5) | (Xd&31));
        if ((v>>32)&0xFFFF) u32(0xF2C00000u | (uint32_t((v>>32)&0xFFFF)<<5) | (Xd&31));
        if ((v>>48)&0xFFFF) u32(0xF2E00000u | (uint32_t((v>>48)&0xFFFF)<<5) | (Xd&31));
    }
    void movr(unsigned Wd, unsigned Wm) { u32(0x2A0003E0u | ((Wm&31)<<16) | (Wd&31)); }
    void ldr32(unsigned Wt, unsigned Xn, uint32_t im) { u32(0xB9400000u | ((im>>2)<<10) | ((Xn&31)<<5) | (Wt&31)); }
    void str32(unsigned Wt, unsigned Xn, uint32_t im) { u32(0xB9000000u | ((im>>2)<<10) | ((Xn&31)<<5) | (Wt&31)); }
    void load_gpr(unsigned Wd, unsigned r) { if (!r) { u32(0x2A1F03E0u | (Wd&31)); return; } ldr32(Wd, 0, gpr_off(r)); }
    void store_gpr(unsigned Ws, unsigned r) { if (!r) return; str32(Ws, 0, gpr_off(r)); }
    void add(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x0B000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void sub(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x4B000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void andr(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x0A000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void orr(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x2A000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void eor(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x4A000000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void mvn(unsigned Wd, unsigned Wm){ u32(0x2A2003E0u | ((Wm&31)<<16) | (Wd&31)); }
    void lsli(unsigned Wd, unsigned Wn, unsigned s){ u32(0x53000000u | ((s&63)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void lsri(unsigned Wd, unsigned Wn, unsigned s){ u32(0x53000000u | ((s&63)<<16) | ((Wn&31)<<5) | (Wd&31) | (1<<22)); }
    void asri(unsigned Wd, unsigned Wn, unsigned s){ u32(0x13000000u | ((s&63)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void lslv(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC02000u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void lsrv(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC02400u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void asrv(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC02800u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void cmp(unsigned Wn, unsigned Wm){ u32(0x6B00001Fu | ((Wm&31)<<16) | (Wn&31)); }
    void cset(unsigned Wd, unsigned cond){ u32(0x1A9F07E0u | ((cond&15)<<12) | (Wd&31)); }
    void smull(unsigned Xd, unsigned Wn, unsigned Wm){ u32(0x9B207C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Xd&31)); }
    void umull(unsigned Xd, unsigned Wn, unsigned Wm){ u32(0x9BA07C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Xd&31)); }
    void sdiv(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC00C00u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void udiv(unsigned Wd, unsigned Wn, unsigned Wm){ u32(0x1AC00800u | ((Wm&31)<<16) | ((Wn&31)<<5) | (Wd&31)); }
    void msub(unsigned Wd, unsigned Wn, unsigned Wm, unsigned Wa){ u32(0x1B008000u | ((Wm&31)<<16) | ((Wa&31)<<10) | ((Wn&31)<<5) | (Wd&31)); }
    void blr(unsigned Xn){ u32(0xD63F0000u | ((Xn&31)<<5)); }
    void sbfx(unsigned Wd, unsigned Wn, unsigned lsb, unsigned width) { u32(0x13000000u | (((width-1)&0x3F)<<16) | ((lsb&0x3F)<<10) | ((Wn&31)<<5) | (Wd&31)); }
    void ubfx(unsigned Wd, unsigned Wn, unsigned lsb, unsigned width) { u32(0x53000000u | (((width-1)&0x3F)<<16) | ((lsb&0x3F)<<10) | ((Wn&31)<<5) | (Wd&31)); }
    void sxtw(unsigned Xd, unsigned Wn) { u32(0x93407C00u | ((Wn&31)<<5) | (Xd&31)); }
    void csel(unsigned Wd, unsigned Wn, unsigned Wm, unsigned cond) { u32(0x1A800000u | ((Wm&31)<<16) | ((cond&0xF)<<12) | ((Wn&31)<<5) | (Wd&31)); }
    void smulh(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0x9B407C00u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }
    void umulh(unsigned Xd, unsigned Xn, unsigned Xm) { u32(0x9BC07C00u | ((Xm&31)<<16) | ((Xn&31)<<5) | (Xd&31)); }

    // Usamos x21 y x22 para guardar state y ram temporalmente durante un BLR
    void call(void* fn) {
        u32(0xAA0003F5u); u32(0xAA0103F6u);         // mov x21, x0; mov x22, x1
        movi64(16, reinterpret_cast<uintptr_t>(fn));
        blr(16);
        u32(0xAA1503E0u); u32(0xAA1603E1u);         // mov x0, x21; mov x1, x22
    }

    void prologue() {
        u32(0xA9BF7BFDu);        // STP x29, x30, [sp, #-16]!
        u32(0x910003FDu);        // MOV x29, sp
        u32(0xA9BF53F3u);        // STP x19, x20, [sp, #-16]!
        u32(0xA9BF5BF5u);        // STP x21, x22, [sp, #-16]!
    }
    void epilogue() {
        u32(0xA8C15BF5u);        // LDP x21, x22, [sp], #16
        u32(0xA8C153F3u);        // LDP x19, x20, [sp], #16
        u32(0xA8C17BFDu);        // LDP x29, x30, [sp], #16
        u32(0xD65F03C0u);        // RET
    }
};

static bool is_term(uint32_t in) {
    uint32_t op=(in>>26)&0x3F;
    if (op == 0x02 || op == 0x03) return true;
    if (op >= 0x04 && op <= 0x07) return true;
    if (op == 0x01) return true;
    if (op == 0x00) { uint32_t f = in & 0x3F; if (f==0x08||f==0x09||f==0x0C||f==0x0D) return true; }
    return false;
}

IOP_Recompiler::IOP_Recompiler(CodeCache& c, IOP_State& s, uint8_t* ram)
    : m_cache(c), m_state(s), m_ram(ram) { g_iop_ram_ptr = ram; }
void IOP_Recompiler::invalidate(uint32_t s, uint32_t e) { m_cache.invalidate_range(s, e); }

// Usamos x19 para el destino del salto, y x20 para la condición (1 tomado, 0 no tomado)
static bool emit_r3k(E& e, uint32_t in, uint32_t pc) {
    uint32_t op=(in>>26)&0x3F, rs=(in>>21)&0x1F, rt=(in>>16)&0x1F, rd=(in>>11)&0x1F;
    uint32_t sa=(in>>6)&0x1F, fn=in&0x3F;
    int32_t  imm=int16_t(in&0xFFFF);
    uint32_t tgt=(pc&0xF0000000u)|((in&0x3FFFFFFu)<<2);
    int32_t  boff=imm<<2;

    switch (op) {
    case 0x00: switch (fn) {
        case 0x00: e.load_gpr(9,rt); e.lsli(9,9,sa); e.store_gpr(9,rd); return false; // SLL
        case 0x02: e.load_gpr(9,rt); e.lsri(9,9,sa); e.store_gpr(9,rd); return false; // SRL
        case 0x03: e.load_gpr(9,rt); e.asri(9,9,sa); e.store_gpr(9,rd); return false; // SRA
        case 0x04: e.load_gpr(9,rt); e.load_gpr(10,rs); e.ubfx(11,10,0,5); e.lslv(9,9,11); e.store_gpr(9,rd); return false; // SLLV
        case 0x06: e.load_gpr(9,rt); e.load_gpr(10,rs); e.ubfx(11,10,0,5); e.lsrv(9,9,11); e.store_gpr(9,rd); return false; // SRLV
        case 0x07: e.load_gpr(9,rt); e.load_gpr(10,rs); e.ubfx(11,10,0,5); e.asrv(9,9,11); e.store_gpr(9,rd); return false; // SRAV
        case 0x08: e.load_gpr(19,rs); e.movi32(20,1); return true; // JR
        case 0x09: e.load_gpr(19,rs); e.movi32(9,pc+8); e.store_gpr(9,rd?rd:31); e.movi32(20,1); return true; // JALR
        case 0x0C: e.movi32(19, 0x80000080u); e.movi32(20,1); return true; // SYSCALL
        case 0x0D: e.movi32(19, 0x80000080u); e.movi32(20,1); return true; // BREAK
        case 0x10: e.ldr32(9, 0, OFF_HI); e.store_gpr(9,rd); return false; // MFHI
        case 0x11: e.load_gpr(9,rs); e.str32(9, 0, OFF_HI); return false; // MTHI
        case 0x12: e.ldr32(9, 0, OFF_LO); e.store_gpr(9,rd); return false; // MFLO
        case 0x13: e.load_gpr(9,rs); e.str32(9, 0, OFF_LO); return false; // MTLO
        case 0x18: { // MULT: {HI,LO} = rs * rt (signed)
            e.load_gpr(9,rs); e.load_gpr(10,rt); e.sxtw(19,9); e.sxtw(20,10);
            e.smulh(11,19,20); e.mul(12,9,10);
            e.str32(12, 0, OFF_LO); e.str32(11, 0, OFF_HI);
            return false;
        }
        case 0x19: { // MULTU
            e.load_gpr(9,rs); e.load_gpr(10,rt);
            e.umull(18,9,10); // X18 = 64-bit result
            e.str32(18, 0, OFF_LO);
            e.u32(0xD360FC12u); // LSR X12, X18, #32
            e.str32(12, 0, OFF_HI);
            return false;
        }
        case 0x1A: { // DIV (signed)
            e.load_gpr(9,rs); e.load_gpr(10,rt);
            // If divisor == 0: LO = 0xFFFFFFFF, HI = rs
            e.u32(0x6B1F001Fu); // CMP W10, #0
            e.sdiv(11,9,10);
            e.msub(12,11,10,9); // W12 = 9 - 11*10 = remainder
            // If W10 == 0, select 0xFFFFFFFF for LO and rs for HI
            e.movi32(13, 0xFFFFFFFF);
            e.csel(11, 13, 11, 0x0); // If div==0, LO=0xFFFFFFFF, else LO=result
            e.csel(12, 9, 12, 0x0);  // If div==0, HI=rs, else HI=remainder
            e.str32(11, 0, OFF_LO); e.str32(12, 0, OFF_HI);
            return false;
        }
        case 0x1B: { // DIVU (unsigned)
            e.load_gpr(9,rs); e.load_gpr(10,rt);
            e.u32(0x6B1F001Fu); // CMP W10, #0
            e.udiv(11,9,10);
            e.msub(12,11,10,9);
            e.movi32(13, 0xFFFFFFFF);
            e.csel(11, 13, 11, 0x0); // If div==0, LO=0xFFFFFFFF
            e.csel(12, 9, 12, 0x0);  // If div==0, HI=rs
            e.str32(11, 0, OFF_LO); e.str32(12, 0, OFF_HI);
            return false;
        }
        case 0x20: e.load_gpr(9,rs); e.load_gpr(10,rt); e.add(9,9,10); e.store_gpr(9,rd); return false; // ADD
        case 0x21: e.load_gpr(9,rs); e.load_gpr(10,rt); e.add(9,9,10); e.store_gpr(9,rd); return false; // ADDU
        case 0x22: e.load_gpr(9,rs); e.load_gpr(10,rt); e.sub(9,9,10); e.store_gpr(9,rd); return false; // SUB
        case 0x23: e.load_gpr(9,rs); e.load_gpr(10,rt); e.sub(9,9,10); e.store_gpr(9,rd); return false; // SUBU
        case 0x24: e.load_gpr(9,rs); e.load_gpr(10,rt); e.andr(9,9,10); e.store_gpr(9,rd); return false; // AND
        case 0x25: e.load_gpr(9,rs); e.load_gpr(10,rt); e.orr(9,9,10); e.store_gpr(9,rd); return false; // OR
        case 0x26: e.load_gpr(9,rs); e.load_gpr(10,rt); e.eor(9,9,10); e.store_gpr(9,rd); return false; // XOR
        case 0x27: e.load_gpr(9,rs); e.load_gpr(10,rt); e.orr(9,9,10); e.mvn(9,9); e.store_gpr(9,rd); return false; // NOR
        case 0x2A: e.load_gpr(9,rs); e.load_gpr(10,rt); e.cmp(9,10); e.cset(9,0xB); e.store_gpr(9,rd); return false; // SLT
        case 0x2B: e.load_gpr(9,rs); e.load_gpr(10,rt); e.cmp(9,10); e.cset(9,0x3); e.store_gpr(9,rd); return false; // SLTU
        }
        return false;
    case 0x01: {
        e.load_gpr(9,rs); e.cmp(9,31); e.cset(20, (rt&1)?0xA:0xB);
        e.movi32(19, pc+4+boff);
        if (rt & 0x10) { e.movi32(9,pc+8); e.store_gpr(9,31); }
        return true;
    }
    case 0x02: e.movi32(19,tgt); e.movi32(20,1); return true;
    case 0x03: e.movi32(9,pc+8); e.store_gpr(9,31); e.movi32(19,tgt); e.movi32(20,1); return true;
    case 0x04: e.load_gpr(9,rs); e.load_gpr(10,rt); e.cmp(9,10); e.cset(20,0x0); e.movi32(19,pc+4+boff); return true;
    case 0x05: e.load_gpr(9,rs); e.load_gpr(10,rt); e.cmp(9,10); e.cset(20,0x1); e.movi32(19,pc+4+boff); return true;
    case 0x06: e.load_gpr(9,rs); e.cmp(9,31); e.cset(20,0xD); e.movi32(19,pc+4+boff); return true;
    case 0x07: e.load_gpr(9,rs); e.cmp(9,31); e.cset(20,0xC); e.movi32(19,pc+4+boff); return true;
    case 0x08: case 0x09: e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10); e.store_gpr(9,rt); return false;
    case 0x0A: e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.cmp(9,10); e.cset(9,0xB); e.store_gpr(9,rt); return false;
    case 0x0B: e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.cmp(9,10); e.cset(9,0x3); e.store_gpr(9,rt); return false;
    case 0x0C: e.load_gpr(9,rs); e.movi32(10, uint16_t(imm)); e.andr(9,9,10); e.store_gpr(9,rt); return false;
    case 0x0D: e.load_gpr(9,rs); e.movi32(10, uint16_t(imm)); e.orr(9,9,10);  e.store_gpr(9,rt); return false;
    case 0x0E: e.load_gpr(9,rs); e.movi32(10, uint16_t(imm)); e.eor(9,9,10);  e.store_gpr(9,rt); return false;
    case 0x0F: e.movi32(9, uint32_t(uint16_t(imm)) << 16); e.store_gpr(9,rt); return false;
    case 0x20: case 0x24: case 0x21: case 0x25: case 0x23: {
        e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10); e.movr(0,9);
        void* fn = (op==0x20||op==0x24)?(void*)&iop_bus_read8
                 : (op==0x21||op==0x25)?(void*)&iop_bus_read16
                 :                       (void*)&iop_bus_read32;
        e.call(fn);
        if      (op==0x20) e.u32(0x13001C09u | (0<<5));
        else if (op==0x21) e.u32(0x13003C09u);
        else               e.movr(9,0);
        if (op==0x24) { e.movi32(10,0xFF); e.andr(9,9,10); }
        if (op==0x25) { e.movi32(10,0xFFFF); e.andr(9,9,10); }
        e.store_gpr(9, rt);
        return false;
    }
    case 0x28: case 0x29: case 0x2B: { // SB, SH, SW
        e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10);
        e.load_gpr(10,rt); e.movr(0,9); e.movr(1,10);
        void* fn = (op==0x28)?(void*)&iop_bus_write8
                 : (op==0x29)?(void*)&iop_bus_write16
                 :             (void*)&iop_bus_write32;
        e.call(fn); return false;
    }
    case 0x2A: { // SWL: store word left (unimplemented, fallback to SW)
        e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10);
        e.load_gpr(10,rt); e.movr(0,9); e.movr(1,10);
        e.call((void*)&iop_bus_write32); return false;
    }
    case 0x2E: { // SWR: store word right (unimplemented, fallback to SW)
        e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10);
        e.load_gpr(10,rt); e.movr(0,9); e.movr(1,10);
        e.call((void*)&iop_bus_write32); return false;
    }
    case 0x22: { // LWL: load word left (unimplemented, fallback to LW)
        e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10); e.movr(0,9);
        e.call((void*)&iop_bus_read32); e.movr(9,0);
        e.store_gpr(9, rt); return false;
    }
    case 0x26: { // LWR: load word right (unimplemented, fallback to LW)
        e.load_gpr(9,rs); e.movi32(10,uint32_t(imm)); e.add(9,9,10); e.movr(0,9);
        e.call((void*)&iop_bus_read32); e.movr(9,0);
        e.store_gpr(9, rt); return false;
    }
    case 0x10: { // COP0
        uint32_t sub = (in >> 21) & 0x1F;
        uint32_t rd = (in >> 11) & 0x1F;
        if (sub == 0x00) { // MFC0: rt = COP0[rd]
            e.ldr32(9, 0, OFF_COP0 + rd * 4);
            e.store_gpr(9, rt); return false;
        }
        if (sub == 0x04) { // MTC0: COP0[rd] = rt
            e.load_gpr(9, rt);
            e.str32(9, 0, OFF_COP0 + rd * 4); return false;
        }
        if (fn == 0x10) { // RFE: Return From Exception
            // RFE restores the bottom 2 bits (mode) of Status from SR
            // COP0[12] = SR, COP0[13] = Cause, COP0[14] = EPC
            e.ldr32(9, 0, OFF_COP0 + 12 * 4);  // Load SR
            e.ldr32(10, 0, OFF_COP0 + 13 * 4);  // Load Cause
            // Mode = SR[mode] >> 2, then shift SR mode bits right by 2 (KUp→KUo pattern)
            // In R3000: SR[1:0] = mode, we shift right by 2 to get previous mode
            e.ubfx(11, 9, 2, 2);   // W11 = SR.mode_prev (bits 2-3)
            e.andr(9, 9, 10);       // Clear mode bits from SR (keep others)
            e.movi32(10, 0x3);
            e.mvn(10, 10);          // W10 = ~0x3
            e.andr(9, 9, 10);       // W9 = SR & ~0x3
            e.orr(9, 9, 11);        // W9 = (SR & ~0x3) | mode_prev
            e.str32(9, 0, OFF_COP0 + 12 * 4);  // Store back to SR
            return false;
        }
        return false;
    }
    case 0x11: case 0x12: return false; // COP1/COP2 - skip
    case 0x32: case 0x3A: return false; // LWC2/SWC2 - skip
    case 0x2F: return false; // LWC0/SWC0 - skip
    default: LOGE("IOP op %02X @%08X", op, pc); return false;
    }
}

IOP_Recompiler::CompiledBlock IOP_Recompiler::compile_block(uint32_t pc) {
    constexpr size_t MAX_CODE = 4096;
    uint8_t* code = static_cast<uint8_t*>(m_cache.alloc(MAX_CODE));
    if (!code) { m_cache.flush(); code = static_cast<uint8_t*>(m_cache.alloc(MAX_CODE)); }
    if (!code) { LOGE("cache lleno"); return nullptr; }

    E e{code};
    e.prologue();

    constexpr uint32_t MAX_INSNS = 32;
    uint32_t current_pc = pc;
    bool terminated = false;
    uint32_t branch_pc = 0;

    for (uint32_t i = 0; i < MAX_INSNS; ++i) {
        uint32_t instr = iop_bus_read32(current_pc);
        bool term = emit_r3k(e, instr, current_pc);
        
        if (term && !terminated) {
            terminated = true;
            branch_pc  = current_pc;
            current_pc += 4;
            continue; // Ejecutar delay slot
        }
        current_pc += 4;
        if (terminated) break; // Bloque terminado
    }

    if (!terminated) {
        // Si no hubo branch, el PC simplemente avanza
        e.movi32(9, current_pc);
        e.str32(9, 0, OFF_PC);
    } else {
        // Si hubo branch, decidir si se tomó o no usando x19 y x20
        e.movi32(10, branch_pc + 8); // Destino si NO se toma
        e.cmp(20, 31);               // Comparar x20 con XZR
        
        // CSEL W9, W19, W10, NE -> Si x20 != 0, W9 = W19, si no W9 = W10
        e.u32(0x1A800000u | (10 << 16) | (0x1 << 12) | (19 << 5) | 9);
        e.str32(9, 0, OFF_PC);
    }

    e.epilogue();

    size_t code_size = size_t(e.p - code);
    if (code_size > MAX_CODE) { LOGE("¡overrun! %zu > %zu", code_size, MAX_CODE); return nullptr; }

    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(e.p));

    auto fn = reinterpret_cast<CompiledBlock>(code);
    m_cache.register_block(pc, reinterpret_cast<CodeCache::BlockFn>(fn), code_size);
    return fn;
}