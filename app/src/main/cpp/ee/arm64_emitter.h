#include <android/log.h>
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ARM64-EMIT", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ARM64-EMIT", __VA_ARGS__)
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <sys/mman.h>

// ─── Registros ARM64 ──────────────────────────────────────────────────────────
enum ARM64Reg : uint8_t {
    X0=0,  X1,  X2,  X3,  X4,  X5,  X6,  X7,
    X8,    X9,  X10, X11, X12, X13, X14, X15,
    X16,   X17, X18, X19, X20, X21, X22, X23,
    X24,   X25, X26, X27, X28, X29, X30,
    XZR = 31,  // zero register
    SP  = 31,  // stack pointer (contexto-dependiente)

    // Alias para claridad en el recompilador
    // Registros caller-saved (libres para usar sin preservar):
    SCRATCH0 = X9,
    SCRATCH1 = X10,
    SCRATCH2 = X11,
    SCRATCH3 = X12,
    // Registros callee-saved (el recompilador los usa para mapeo de MIPS GPRs):
    EE_STATE = X19,  // puntero al struct EE_State completo
    EE_MEM   = X20,  // puntero base a la RAM del EE (32MB)
    EE_PC    = X21,  // program counter actual
};

// Registro W (32-bit view de Xn)
enum ARM64RegW : uint8_t {
    W0=0,W1,W2,W3,W4,W5,W6,W7,W8,W9,W10,W11,W12,W13,W14,W15,
    W16,W17,W18,W19,W20,W21,W22,W23,W24,W25,W26,W27,W28,W29,W30,WZR=31
};

// ─── Emisor de código ARM64 ───────────────────────────────────────────────────
// Escribe instrucciones ARM64 directamente en un buffer ejecutable.
// Cada método emit_* codifica exactamente una instrucción a 4 bytes (fixed-width).
class ARM64Emitter {
public:
    uint32_t* buf;     // buffer de código (mmap ejecutable)
    size_t    pos;     // índice actual en words
    size_t    cap;     // capacidad total en words

    ARM64Emitter(void* memory, size_t size_bytes)
        : buf(static_cast<uint32_t*>(memory))
        , pos(0)
        , cap(size_bytes / 4)
    {}

    uint8_t* current_ptr() const { return reinterpret_cast<uint8_t*>(buf + pos); }
    size_t   code_size()   const { return pos * 4; }

    // ── Data Processing (inmediatos) ─────────────────────────────────────────

    // MOV Xd, imm16 (con shift opcional: 0,16,32,48)
    void emit_movz(ARM64Reg rd, uint16_t imm, int shift = 0) {
        // MOVZ: 1 10 100101 hw[2] imm16[16] Rd[5]
        uint32_t hw = (shift / 16) & 3;
        emit((1u<<31)|(0b10<<29)|(0b100101<<23)|(hw<<21)|(imm<<5)|(rd));
    }

    // MOVK Xd, imm16, LSL #shift  (mantiene otros bits)
    void emit_movk(ARM64Reg rd, uint16_t imm, int shift = 0) {
        uint32_t hw = (shift / 16) & 3;
        emit((1u<<31)|(0b11<<29)|(0b100101<<23)|(hw<<21)|(imm<<5)|(rd));
    }

    // MOV Xd, Xn  (alias de ORR Xd, XZR, Xn)
    void emit_mov(ARM64Reg rd, ARM64Reg rn) {
        emit_orr_reg(rd, ARM64Reg::XZR, rn);
    }

    // MOV Wd, Wn (32-bit)
    void emit_mov32(ARM64RegW rd, ARM64RegW rn) {
        // ORR Wd, WZR, Wn
        emit((0b00101010u<<24)|(0<<21)|(rn<<16)|(0<<10)|(ARM64RegW::WZR<<5)|(rd));
    }

    // ── Aritmética ───────────────────────────────────────────────────────────

    // ADD Xd, Xn, Xm
    void emit_add(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0001011u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(rd));
    }

    // ADD Xd, Xn, #imm12
    void emit_add_imm(ARM64Reg rd, ARM64Reg rn, uint16_t imm12) {
        if (imm12 >= 4096) { LOGW("imm12 overflow: %u", imm12); imm12 = 0; }
        emit((1u<<31)|(0b00100010u<<23)|(0<<22)|(imm12<<10)|(rn<<5)|(rd));
    }

    // ADDS Xd, Xn, Xm  (actualiza flags)
    void emit_adds(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0101011u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(rd));
    }

    // SUB Xd, Xn, Xm
    void emit_sub(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b1001011u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(rd));
    }

    // SUB Xd, Xn, #imm12
    void emit_sub_imm(ARM64Reg rd, ARM64Reg rn, uint16_t imm12) {
        if (imm12 >= 4096) { LOGW("imm12 overflow: %u", imm12); imm12 = 0; }
        emit((1u<<31)|(0b10100010u<<23)|(0<<22)|(imm12<<10)|(rn<<5)|(rd));
    }

    // MUL Xd, Xn, Xm  (= MADD Xd, Xn, Xm, XZR)
    void emit_mul(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011011u<<24)|(0<<21)|(rm<<16)|(0b011111u<<10)|(rn<<5)|(rd));
    }

    // SDIV Xd, Xn, Xm
    void emit_sdiv(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011010110u<<21)|(rm<<16)|(0b000011u<<10)|(rn<<5)|(rd));
    }

    // UDIV Xd, Xn, Xm
    void emit_udiv(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011010110u<<21)|(rm<<16)|(0b000010u<<10)|(rn<<5)|(rd));
    }

    // SMULH Xd, Xn, Xm  (parte alta de MUL con signo — para HI de PS2)
    void emit_smulh(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011011u<<24)|(0b01<<21)|(rm<<16)|(XZR<<10)|(rn<<5)|(rd));
    }

    // UMULH Xd, Xn, Xm
    void emit_umulh(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011011u<<24)|(0b10<<21)|(rm<<16)|(XZR<<10)|(rn<<5)|(rd));
    }

    // ── Lógica ───────────────────────────────────────────────────────────────

    // AND Xd, Xn, Xm
    void emit_and(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0001010u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(rd));
    }

    // ORR Xd, Xn, Xm
    void emit_orr_reg(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0101010u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(rd));
    }

    // EOR Xd, Xn, Xm
    void emit_eor(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b1001010u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(rd));
    }

    // NOR: no hay instrucción nativa — OR + NOT
    void emit_nor(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit_orr_reg(rd, rn, rm);
        emit_mvn(rd, rd);
    }

    // MVN Xd, Xm  (bitwise NOT)
    void emit_mvn(ARM64Reg rd, ARM64Reg rm) {
        emit((1u<<31)|(0b0101010u<<24)|(1<<21)|(rm<<16)|(0<<10)|(XZR<<5)|(rd));
    }

    // ── Shifts ───────────────────────────────────────────────────────────────

    // LSL Xd, Xn, #imm (0..63)
    void emit_lsl_imm(ARM64Reg rd, ARM64Reg rn, uint8_t imm) {
        // UBFM Xd, Xn, #(-imm mod 64), #(63-imm)
        uint8_t immr = (-imm) & 63;
        uint8_t imms = 63 - imm;
        emit_ubfm(rd, rn, immr, imms);
    }

    // LSR Xd, Xn, #imm
    void emit_lsr_imm(ARM64Reg rd, ARM64Reg rn, uint8_t imm) {
        emit_ubfm(rd, rn, imm, 63);
    }

    // ASR Xd, Xn, #imm
    void emit_asr_imm(ARM64Reg rd, ARM64Reg rn, uint8_t imm) {
        emit_sbfm(rd, rn, imm, 63);
    }

    // LSL Xd, Xn, Xm (variable)
    void emit_lslv(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011010110u<<21)|(rm<<16)|(0b001000u<<10)|(rn<<5)|(rd));
    }

    // LSR Xd, Xn, Xm
    void emit_lsrv(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011010110u<<21)|(rm<<16)|(0b001001u<<10)|(rn<<5)|(rd));
    }

    // ASR Xd, Xn, Xm
    void emit_asrv(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b0011010110u<<21)|(rm<<16)|(0b001010u<<10)|(rn<<5)|(rd));
    }

    // UBFM / SBFM (bitfield)
    void emit_ubfm(ARM64Reg rd, ARM64Reg rn, uint8_t immr, uint8_t imms) {
        emit((1u<<31)|(0b10<<29)|(0b100110u<<23)|(1<<22)|(immr<<16)|(imms<<10)|(rn<<5)|(rd));
    }
    void emit_sbfm(ARM64Reg rd, ARM64Reg rn, uint8_t immr, uint8_t imms) {
        emit((1u<<31)|(0b00<<29)|(0b100110u<<23)|(1<<22)|(immr<<16)|(imms<<10)|(rn<<5)|(rd));
    }

    // SXTW Xd, Wn  (sign-extend 32→64 — muy usado en MIPS ADDIU etc.)
    void emit_sxtw(ARM64Reg rd, ARM64RegW rn) {
        emit_sbfm(rd, static_cast<ARM64Reg>(rn), 0, 31);
    }

    // ── Comparaciones ────────────────────────────────────────────────────────

    // CMP Xn, Xm  (= SUBS XZR, Xn, Xm)
    void emit_cmp(ARM64Reg rn, ARM64Reg rm) {
        emit((1u<<31)|(0b1101011u<<24)|(0<<21)|(rm<<16)|(0<<10)|(rn<<5)|(XZR));
    }

    // CMP Xn, #imm12
    void emit_cmp_imm(ARM64Reg rn, uint16_t imm12) {
        emit((1u<<31)|(0b11100010u<<23)|(0<<22)|(imm12<<10)|(rn<<5)|(XZR));
    }

    // CSET Xd, cond  (Xd = (cond) ? 1 : 0)
    void emit_cset(ARM64Reg rd, uint8_t cond) {
        // CSINC Xd, XZR, XZR, invert(cond)
        uint8_t inv = cond ^ 1;
        emit((1u<<31)|(0b0011010100u<<21)|(XZR<<16)|(inv<<12)|(0b01<<10)|(XZR<<5)|(rd));
    }

    // SLT: rd = (rn < rm) ? 1 : 0 (signed)
    void emit_slt(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit_cmp(rn, rm);
        emit_cset(rd, 0b1011); // LT condition
    }

    // SLTU: rd = (rn < rm) ? 1 : 0 (unsigned)
    void emit_sltu(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit_cmp(rn, rm);
        emit_cset(rd, 0b0011); // LO (unsigned lower) condition
    }

    // ── Cargar/guardar ───────────────────────────────────────────────────────

    // LDR Xd, [Xn, #offset]
    void emit_ldr(ARM64Reg rd, ARM64Reg rn, int16_t offset) {
        if (offset >= 0 && (offset & 7) == 0 && offset < 32768) {
            // Unsigned offset (más eficiente)
            uint32_t uoff = offset / 8;
            emit((0b11u<<30)|(0b111000u<<24)|(0b01u<<22)|(uoff<<10)|(rn<<5)|(rd));
        } else {
            // Signed offset
            emit((0b11u<<30)|(0b111000u<<24)|(0b00u<<22)|((offset&0xFFF)<<12)|(0b00u<<10)|(rn<<5)|(rd));
        }
    }

    // LDR Wd, [Xn, #offset]  (32-bit load)
    void emit_ldr32(ARM64RegW rd, ARM64Reg rn, int16_t offset) {
        uint32_t uoff = offset / 4;
        emit((0b10u<<30)|(0b111001u<<24)|(0b01u<<22)|(uoff<<10)|(rn<<5)|(rd));
    }

    // LDR Xd, [Xn, Xm]  (register offset)
    void emit_ldr_reg(ARM64Reg rd, ARM64Reg rn, ARM64Reg rm) {
        emit((0b11u<<30)|(0b111000u<<24)|(0b01u<<22)|(rm<<16)|(0b011u<<13)|(1<<12)|(0b11u<<10)|(rn<<5)|(rd));
    }

    // STR Xd, [Xn, #offset]
    void emit_str(ARM64Reg rd, ARM64Reg rn, int16_t offset) {
        uint32_t uoff = offset / 8;
        emit((0b11u<<30)|(0b111000u<<24)|(0b00u<<22)|(uoff<<10)|(rn<<5)|(rd));
    }

    // STR Wd, [Xn, #offset]
    void emit_str32(ARM64RegW rd, ARM64Reg rn, int16_t offset) {
        uint32_t uoff = offset / 4;
        emit((0b10u<<30)|(0b111001u<<24)|(0b00u<<22)|(uoff<<10)|(rn<<5)|(rd));
    }

    // LDRB Wd, [Xn, #offset]
    void emit_ldrb(ARM64RegW rd, ARM64Reg rn, int16_t offset) {
        emit((0b00u<<30)|(0b111001u<<24)|(0b01u<<22)|(offset<<10)|(rn<<5)|(rd));
    }

    // STRB Wd, [Xn, #offset]
    void emit_strb(ARM64RegW rd, ARM64Reg rn, int16_t offset) {
        emit((0b00u<<30)|(0b111001u<<24)|(0b00u<<22)|(offset<<10)|(rn<<5)|(rd));
    }

    // STP X0, X1, [SP, #-16]!  (push par)
    void emit_stp_pre(ARM64Reg r1, ARM64Reg r2, ARM64Reg base, int16_t offset) {
        int32_t imm7 = (offset / 8) & 0x7F;
        emit((0b10u<<30)|(0b101u<<27)|(0b011u<<23)|(imm7<<15)|(r2<<10)|(base<<5)|(r1));
    }

    // LDP X0, X1, [SP], #16  (pop par)
    void emit_ldp_post(ARM64Reg r1, ARM64Reg r2, ARM64Reg base, int16_t offset) {
        int32_t imm7 = (offset / 8) & 0x7F;
        emit((0b10u<<30)|(0b101u<<27)|(0b011u<<23)|(1<<23)|(imm7<<15)|(r2<<10)|(base<<5)|(r1));
    }

    // ── Branches ─────────────────────────────────────────────────────────────

    // B #offset  (PC-relative, offset en bytes, múltiplo de 4)
    void emit_b(int32_t offset_bytes) {
        int32_t imm26 = (offset_bytes / 4) & 0x3FFFFFF;
        emit((0b000101u<<26)|(imm26));
    }

    // BL #offset  (branch con link — para llamadas)
    void emit_bl(int32_t offset_bytes) {
        int32_t imm26 = (offset_bytes / 4) & 0x3FFFFFF;
        emit((0b100101u<<26)|(imm26));
    }

    // BR Xn  (branch register — para JR/JALR de MIPS)
    void emit_br(ARM64Reg rn) {
        emit((0b1101011u<<25)|(0b0000u<<21)|(0b11111u<<16)|(0b000000u<<10)|(rn<<5)|(0b00000u));
    }

    // BLR Xn
    void emit_blr(ARM64Reg rn) {
        emit((0b1101011u<<25)|(0b0001u<<21)|(0b11111u<<16)|(0b000000u<<10)|(rn<<5)|(0b00000u));
    }

    // RET  (= BR X30)
    void emit_ret() {
        emit_br(X30);
    }

    // B.cond #offset  (branch condicional, offset en bytes)
    void emit_b_cond(uint8_t cond, int32_t offset_bytes) {
        int32_t imm19 = (offset_bytes / 4) & 0x7FFFF;
        emit((0b01010100u<<24)|(imm19<<5)|(0<<4)|(cond));
    }

    // CBZ Xn, #offset  (branch if zero)
    void emit_cbz(ARM64Reg rn, int32_t offset_bytes) {
        int32_t imm19 = (offset_bytes / 4) & 0x7FFFF;
        emit((1u<<31)|(0b011010u<<25)|(0<<24)|(imm19<<5)|(rn));
    }

    // CBNZ Xn, #offset
    void emit_cbnz(ARM64Reg rn, int32_t offset_bytes) {
        int32_t imm19 = (offset_bytes / 4) & 0x7FFFF;
        emit((1u<<31)|(0b011010u<<25)|(1<<24)|(imm19<<5)|(rn));
    }

    // ── Miscelánea ───────────────────────────────────────────────────────────

    // NOP
    void emit_nop() { emit(0xD503201Fu); }

    // BRK #imm16  (breakpoint / trap para instrucciones no implementadas)
    void emit_brk(uint16_t imm) {
        emit((0b11010100001u<<21)|(imm<<5)|(0b00000u));
    }

    // Carga de dirección de 64 bits en registro (4 instrucciones)
    void emit_load_imm64(ARM64Reg rd, uint64_t imm) {
        emit_movz(rd, (imm >>  0) & 0xFFFF, 0);
        if (imm >> 16) emit_movk(rd, (imm >> 16) & 0xFFFF, 16);
        if (imm >> 32) emit_movk(rd, (imm >> 32) & 0xFFFF, 32);
        if (imm >> 48) emit_movk(rd, (imm >> 48) & 0xFFFF, 48);
    }

    // Parcha una instrucción ya emitida (para backpatching de branches)
    void patch(size_t pos_to_patch, uint32_t new_instr) {
        buf[pos_to_patch] = new_instr;
    }

    // Marca la posición actual (para patches de branch forward)
    size_t mark() const { return pos; }

    // Flush de instrucción de caché (necesario antes de ejecutar código generado)
    void flush_icache(void* start, size_t len) {
        __builtin___clear_cache(static_cast<char*>(start),
                                static_cast<char*>(start) + len);
    }

private:
    void emit(uint32_t instr) {
        if (pos >= cap) { LOGE("Code buffer overflow!"); return; }
        buf[pos++] = instr;
    }
};
