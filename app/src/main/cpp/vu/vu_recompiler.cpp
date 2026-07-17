// vu/vu_recompiler.cpp
// JIT recompiler for VU0/VU1 micro-instructions → ARM64 Native (NEON)
// Traduce microcódigo MIPS VU a código nativo ARM64 usando registros NEON de 128 bits.

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

// Tamaño de la memoria para el código JIT de la VU (16MB)
constexpr size_t VU_JIT_CODE_SIZE = 16 * 1024 * 1024;
static uint8_t* g_vu_jit_code = nullptr;
static size_t g_vu_jit_offset = 0;

// Inicializar memoria ejecutable para VU JIT
void init_vu_jit() {
    if (!g_vu_jit_code) {
        g_vu_jit_code = (uint8_t*)mmap(NULL, VU_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_vu_jit_code == MAP_FAILED) {
            g_vu_jit_code = nullptr;
            LOGE("Fallo al reservar memoria JIT para VU (mmap).");
        } else {
            LOGI("Memoria JIT para VU reservada: %zu MB", VU_JIT_CODE_SIZE / (1024 * 1024));
        }
    }
}

// Offset de los registros VF dentro de VU_State
static constexpr uint32_t VU_VF_OFF = offsetof(VU_State, vf);

// ─── Estructura para emitir código ARM64 ──────────────────────────────────────
struct VU_Emitter {
    uint8_t* p;
    void u32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }

    void ret() { u32(0xD65F03C0u); }
    
    // LDR/STR de 128 bits (NEON) - Carga/Guarda registros VU
    // LDR Qt, [Xn, #imm]
    void ldr_q(unsigned Qt, unsigned Xn, uint32_t imm) {
        u32(0x3DC00000u | ((imm >> 4) << 10) | ((Xn & 31) << 5) | (Qt & 31));
    }
    // STR Qt, [Xn, #imm]
    void str_q(unsigned Qt, unsigned Xn, uint32_t imm) {
        u32(0x3D800000u | ((imm >> 4) << 10) | ((Xn & 31) << 5) | (Qt & 31));
    }

    // Operaciones SIMD de punto flotante (NEON) - 4x32bit (4S)
    // FADD Vd.4S, Vn.4S, Vm.4S
    void fadd_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4E20D400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    // FSUB Vd.4S, Vn.4S, Vm.4S
    void fsub_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x4EA0D400u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    // FMUL Vd.4S, Vn.4S, Vm.4S
    void fmul_v4s(unsigned Vd, unsigned Vn, unsigned Vm) {
        u32(0x6E20DC00u | ((Vm & 31) << 16) | ((Vn & 31) << 5) | (Vd & 31));
    }
    // FABS Vd.4S, Vn.4S
    void fabs_v4s(unsigned Vd, unsigned Vn) {
        u32(0x4EA0F800u | ((Vn & 31) << 5) | (Vd & 31));
    }

    // Prólogo y Epílogo (x19 = VU_State, x20 = Data_Mem)
    void prologue() {
        u32(0xA9BF7BFDu); // STP x29, x30, [sp, #-16]!
        u32(0xA9BF53F3u); // STP x19, x20, [sp, #-16]!
        u32(0xAA0003F3u); // MOV x19, x0 (VU_State)
        u32(0xAA0103F4u); // MOV x20, x1 (Data_Mem)
    }
    void epilogue() {
        u32(0xA8C153F3u); // LDP x19, x20, [sp], #16
        u32(0xA8C17BFDu); // LDP x29, x30, [sp], #16
        u32(0xD65F03C0u); // RET
    }
};

// ─── Función principal de recompilación ───────────────────────────────────────
uint8_t* vu_recompile_block(VU_Core& vu_core, int unit, uint32_t micro_pc) {
    if (!g_vu_jit_code) init_vu_jit();
    if (!g_vu_jit_code) return nullptr;

    if (g_vu_jit_offset + 4096 > VU_JIT_CODE_SIZE) {
        g_vu_jit_offset = 0; // Reset simple si se llena
    }

    uint8_t* code = g_vu_jit_code + g_vu_jit_offset;
    VU_Emitter e{code};
    
    e.prologue();
    
    VU_State& vu = (unit == 0) ? vu_core.vu0 : vu_core.vu1;
    uint8_t* micro_mem = vu_core.get_micro_mem(unit);
    size_t max_micro = (unit == 0) ? VU_Core::VU0_MICRO_SIZE : VU_Core::VU1_MICRO_SIZE;
    
    // Compilar un bloque de hasta 32 instrucciones o hasta encontrar E-bit
    uint32_t pc = micro_pc;
    int instr_count = 0;
    
    while (instr_count < 32 && pc + 8 <= max_micro) {
        uint32_t upper = *(uint32_t*)(micro_mem + pc);
        uint32_t lower = *(uint32_t*)(micro_mem + pc + 4);
        
        // Decodificación básica de la parte Upper (FP)
        uint8_t opcode = (upper >> 26) & 0x3F;
        int fs = (upper >> 11) & 0x1F;
        int ft = (upper >> 16) & 0x1F;
        int fd = (upper >> 6) & 0x1F;
        bool e_bit = (upper >> 30) & 1;
        
        // Cargar vf[fs] y vf[ft] en los registros NEON v0 y v1
        e.ldr_q(0, 19, VU_VF_OFF + fs * 16);
        e.ldr_q(1, 19, VU_VF_OFF + ft * 16);
        
        // Traducir instrucción VU a ARM64 NEON
        switch (opcode) {
            case 0x00: // NOP
                break;
            case 0x01: // ADDbc (usaremos op vectorial completa por ahora)
            case 0x02: // ADDi
                e.fadd_v4s(2, 0, 1); // v2 = v0 + v1
                e.str_q(2, 19, VU_VF_OFF + fd * 16); // vf[fd] = v2
                break;
            case 0x03: // SUBbc
            case 0x04: // SUBi
                e.fsub_v4s(2, 0, 1); // v2 = v0 - v1
                e.str_q(2, 19, VU_VF_OFF + fd * 16); // vf[fd] = v2
                break;
            case 0x05: // MULbc
            case 0x06: // MULi
                e.fmul_v4s(2, 0, 1); // v2 = v0 * v1
                e.str_q(2, 19, VU_VF_OFF + fd * 16); // vf[fd] = v2
                break;
            case 0x23: // ABS
                e.fabs_v4s(2, 0); // v2 = abs(v0)
                e.str_q(2, 19, VU_VF_OFF + ft * 16); // vf[ft] = v2
                break;
            default:
                // Por ahora, las instrucciones no implementadas se omiten (NOP)
                // En el futuro, aquí añadiremos el resto (MADD, MSUB, etc.)
                break;
        }
        
        if (e_bit) break; // Fin del microprograma
        
        pc += 8;
        instr_count++;
    }
    
    e.epilogue();
    
    g_vu_jit_offset += 4096;
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(e.p));

    LOGI("VU%d JIT: Bloque recompilado en mPC=0x%04X (%d instrucciones)", unit, micro_pc, instr_count);
    return code;
}