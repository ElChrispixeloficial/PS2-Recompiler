// ─── Emotion Engine Core — Dispatcher principal ───────────────────────────────
// Bucle principal de ejecución: busca o compila bloques, los ejecuta,
// y maneja interrupciones / excepciones del EE.

#include "ee_core.h"
#include "recompiler_arm64.h"
#include "code_cache.h"
#include "ee_memory.h"
#include "mips_defs.h"
#include "../bios/bios_native.h"
#include <android/log.h>
#include <cstring>

extern void set_code_cache_base(uint8_t* base);

// Per-block MIPS PC ring buffer (defined in jni_bridge.cpp)
constexpr int PC_RING_SIZE = 64;
extern uint32_t g_pc_ring[];
extern int g_pc_ring_idx;

#define LOG_TAG "PS2-EE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

EE_Core::EE_Core()
    : cache(std::make_unique<CodeCache>())
    , ee_ram(new uint8_t[EE_RAM_SIZE]())
    , bios_rom(new uint8_t[BIOS_ROM_SIZE]())
    , scratchpad(new uint8_t[SCRATCHPAD_SIZE]())
{
    // Inicializamos la memoria global (ee_memory.cpp)
    ee_mem_init(ee_ram.get(), EE_RAM_SIZE, bios_rom.get());
    
    memset(&state, 0, sizeof(state));
    ee_hw_init(hw);
    state.pc = 0xBFC00000u;  // Reset vector: inicio de BIOS
    set_code_cache_base(cache->code_base());
    LOGI("EE Core iniciado. RAM: %zu MB, CodeCache base: %p", EE_RAM_SIZE / (1024*1024), cache->code_base());
}

EE_Core::~EE_Core() = default;

void EE_Core::load_bios(const uint8_t* bios_data, size_t size) {
    size_t copy_size = size < BIOS_ROM_SIZE ? size : BIOS_ROM_SIZE;
    memcpy(bios_rom.get(), bios_data, copy_size);
    LOGI("BIOS cargada en ROM interna: %zu KB", copy_size / 1024);
}

void EE_Core::load_game(uint32_t entry_point) {
    state.pc = entry_point;
    // Stack pointer inicial (convención PS2)
    state.gpr_lo[SP] = 0x01FFFFF0u;
    state.gpr_lo[GP] = 0x0u;
    LOGI("Juego listo. PC = 0x%08X", entry_point);
}

// ─── Intérprete puro (para BIOS y manejo seguro) ──────────────────────────────
void EE_Core::interpret_single_instruction() {
    uint32_t instr = read32(state.pc);
    
    bool will_branch = false;
    uint32_t next_pc = state.pc + 4;

    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    uint32_t rd = (instr >> 11) & 0x1F;
    uint32_t sa = (instr >> 6) & 0x1F;
    uint32_t funct = instr & 0x3F;
    uint16_t imm = instr & 0xFFFF;
    int32_t simm = (int16_t)imm;
    uint32_t uimm = imm;

    state.gpr_lo[0] = 0;

    switch (opcode) {
        case 0x00: // SPECIAL
            switch (funct) {
                case 0x00: state.gpr_lo[rd] = (int32_t)((uint32_t)state.gpr_lo[rt] << sa); break; // SLL
                case 0x02: state.gpr_lo[rd] = (int32_t)((uint32_t)state.gpr_lo[rt] >> sa); break; // SRL
                case 0x03: state.gpr_lo[rd] = (int32_t)((int32_t)state.gpr_lo[rt] >> sa); break;  // SRA
                case 0x04: state.gpr_lo[rd] = (int32_t)((uint32_t)state.gpr_lo[rt] << (state.gpr_lo[rs] & 0x1F)); break; // SLLV
                case 0x06: state.gpr_lo[rd] = (int32_t)((uint32_t)state.gpr_lo[rt] >> (state.gpr_lo[rs] & 0x1F)); break; // SRLV
                case 0x07: state.gpr_lo[rd] = (int32_t)((int32_t)state.gpr_lo[rt] >> (state.gpr_lo[rs] & 0x1F)); break;  // SRAV
                case 0x08: will_branch = true; next_pc = state.gpr_lo[rs]; break; // JR
                case 0x09: state.gpr_lo[rd ? rd : 31] = state.pc + 8; will_branch = true; next_pc = state.gpr_lo[rs]; break; // JALR
                case 0x0A: if (state.gpr_lo[rt] == 0) state.gpr_lo[rd] = state.gpr_lo[rs]; break; // MOVZ
                case 0x0B: if (state.gpr_lo[rt] != 0) state.gpr_lo[rd] = state.gpr_lo[rs]; break; // MOVN
                case 0x0C: { // SYSCALL
                    state.cop0[14] = state.pc;
                    state.cop0[13] = (state.cop0[13] & ~0x7C) | (0x03 << 2);
                    state.cop0[12] |= 0x2;
                    will_branch = true; next_pc = 0x80000180;
                    break;
                }
                case 0x0D: { // BREAK
                    state.cop0[14] = state.pc;
                    state.cop0[13] = (state.cop0[13] & ~0x7C) | (0x04 << 2);
                    state.cop0[12] |= 0x2;
                    will_branch = true; next_pc = 0x80000180;
                    break;
                }
                case 0x0F: break; // SYNC - no-op on EE
                case 0x10: state.gpr_lo[rd] = state.hi; break; // MFHI
                case 0x11: state.hi = state.gpr_lo[rs]; break; // MTHI
                case 0x12: state.gpr_lo[rd] = state.lo; break; // MFLO
                case 0x13: state.lo = state.gpr_lo[rs]; break; // MTLO
                case 0x18: { // MULT
                    int64_t result = (int64_t)(int32_t)state.gpr_lo[rs] * (int64_t)(int32_t)state.gpr_lo[rt];
                    state.lo = (int32_t)(result & 0xFFFFFFFF);
                    state.hi = (int32_t)((result >> 32) & 0xFFFFFFFF);
                    if (rd) state.gpr_lo[rd] = state.lo;
                    break;
                }
                case 0x19: { // MULTU
                    uint64_t result = (uint64_t)(uint32_t)state.gpr_lo[rs] * (uint64_t)(uint32_t)state.gpr_lo[rt];
                    state.lo = (int32_t)(result & 0xFFFFFFFF);
                    state.hi = (int32_t)((result >> 32) & 0xFFFFFFFF);
                    if (rd) state.gpr_lo[rd] = state.lo;
                    break;
                }
                case 0x1A: { // DIV
                    int32_t n = (int32_t)state.gpr_lo[rs];
                    int32_t d = (int32_t)state.gpr_lo[rt];
                    if (d != 0) {
                        state.lo = n / d;
                        state.hi = n % d;
                    }
                    break;
                }
                case 0x1B: { // DIVU
                    uint32_t n = (uint32_t)state.gpr_lo[rs];
                    uint32_t d = (uint32_t)state.gpr_lo[rt];
                    if (d != 0) {
                        state.lo = (int32_t)(n / d);
                        state.hi = (int32_t)(n % d);
                    }
                    break;
                }
                case 0x20: state.gpr_lo[rd] = (int32_t)(state.gpr_lo[rs] + state.gpr_lo[rt]); break; // ADD
                case 0x21: state.gpr_lo[rd] = (int32_t)(state.gpr_lo[rs] + state.gpr_lo[rt]); break; // ADDU
                case 0x22: state.gpr_lo[rd] = (int32_t)(state.gpr_lo[rs] - state.gpr_lo[rt]); break; // SUB
                case 0x23: state.gpr_lo[rd] = (int32_t)(state.gpr_lo[rs] - state.gpr_lo[rt]); break; // SUBU
                case 0x24: state.gpr_lo[rd] = state.gpr_lo[rs] & state.gpr_lo[rt]; break; // AND
                case 0x25: state.gpr_lo[rd] = state.gpr_lo[rs] | state.gpr_lo[rt]; break; // OR
                case 0x26: state.gpr_lo[rd] = state.gpr_lo[rs] ^ state.gpr_lo[rt]; break; // XOR
                case 0x27: state.gpr_lo[rd] = ~(state.gpr_lo[rs] | state.gpr_lo[rt]); break; // NOR
                case 0x2A: state.gpr_lo[rd] = (state.gpr_lo[rs] < state.gpr_lo[rt]) ? 1 : 0; break; // SLT
                case 0x2B: state.gpr_lo[rd] = ((uint64_t)state.gpr_lo[rs] < (uint64_t)state.gpr_lo[rt]) ? 1 : 0; break; // SLTU
                case 0x2C: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rs] + (int64_t)state.gpr_lo[rt]; break; // DADD
                case 0x2D: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rs] + (int64_t)state.gpr_lo[rt]; break; // DADDU
                case 0x2E: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rs] - (int64_t)state.gpr_lo[rt]; break; // DSUB
                case 0x2F: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rs] - (int64_t)state.gpr_lo[rt]; break; // DSUBU
                case 0x38: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rt] << sa; break; // DSLL
                case 0x3A: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rt] >> sa; break; // DSRL
                case 0x3B: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rt] >> sa; break; // DSRA
                case 0x3C: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rt] << (sa + 32); break; // DSLL32
                case 0x3E: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rt] >> (sa + 32); break; // DSRL32
                case 0x3F: state.gpr_lo[rd] = (int64_t)state.gpr_lo[rt] >> (sa + 32); break; // DSRA32
            }
            break;
        case 0x01: { // REGIMM
            int32_t offset = (int32_t)(simm) << 2;
            bool taken = false;
            switch (rt) {
                case 0x00: taken = (int64_t)state.gpr_lo[rs] < 0; break;  // BLTZ
                case 0x01: taken = (int64_t)state.gpr_lo[rs] >= 0; break; // BGEZ
                case 0x10: taken = (int64_t)state.gpr_lo[rs] < 0;         // BLTZAL
                           state.gpr_lo[31] = state.pc + 8; break;
                case 0x11: taken = (int64_t)state.gpr_lo[rs] >= 0;        // BGEZAL
                           state.gpr_lo[31] = state.pc + 8; break;
            }
            if (taken) { will_branch = true; next_pc = state.pc + 4 + offset; }
            break;
        }
        case 0x02: will_branch = true; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break; // J
        case 0x03: state.gpr_lo[31] = state.pc + 8; will_branch = true; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break; // JAL
        case 0x04: if (state.gpr_lo[rs] == state.gpr_lo[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break; // BEQ
        case 0x05: if (state.gpr_lo[rs] != state.gpr_lo[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break; // BNE
        case 0x06: if ((int64_t)state.gpr_lo[rs] <= 0) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break; // BLEZ
        case 0x07: if ((int64_t)state.gpr_lo[rs] > 0) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break;  // BGTZ
        case 0x08: state.gpr_lo[rt] = (int32_t)(state.gpr_lo[rs] + simm); break; // ADDI
        case 0x09: state.gpr_lo[rt] = (int32_t)(state.gpr_lo[rs] + simm); break; // ADDIU
        case 0x0A: state.gpr_lo[rt] = ((int64_t)state.gpr_lo[rs] < (int64_t)(int32_t)simm) ? 1 : 0; break; // SLTI
        case 0x0B: state.gpr_lo[rt] = ((uint64_t)state.gpr_lo[rs] < (uint64_t)(uint32_t)(int32_t)simm) ? 1 : 0; break; // SLTIU
        case 0x0C: state.gpr_lo[rt] = state.gpr_lo[rs] & uimm; break; // ANDI
        case 0x0D: state.gpr_lo[rt] = state.gpr_lo[rs] | uimm; break; // ORI
        case 0x0E: state.gpr_lo[rt] = state.gpr_lo[rs] ^ uimm; break; // XORI
        case 0x0F: state.gpr_lo[rt] = (int32_t)(uimm << 16); break; // LUI
        case 0x10: // COP0
            switch (rs) {
                case 0x00: state.gpr_lo[rt] = (int32_t)state.cop0[rd]; break; // MFC0
                case 0x04: state.cop0[rd] = state.gpr_lo[rt]; break; // MTC0
                case 0x10: // CO sub-op
                    switch (funct) {
                        case 0x18: // ERET
                            state.cop0[12] &= ~0x2u;
                            state.pc = state.cop0[14];
                            state.gpr_lo[0] = 0;
                            return; // skip normal PC advance
                    }
                    break;
            }
            break;
        case 0x11: // COP1 (FPU)
            switch (rs) {
                case 0x00: state.gpr_lo[rt] = (int32_t)state.fpu[rd]; break; // MFC1
                case 0x04: state.fpu[rd] = (float)(int32_t)state.gpr_lo[rt]; break; // MTC1
                case 0x10: // COP1.S sub-ops
                    switch (funct) {
                        case 0x00: state.fpu[rd] = state.fpu[rs] + state.fpu[rt]; break; // ADD.S
                        case 0x01: state.fpu[rd] = state.fpu[rs] - state.fpu[rt]; break; // SUB.S
                        case 0x02: state.fpu[rd] = state.fpu[rs] * state.fpu[rt]; break; // MUL.S
                        case 0x03: state.fpu[rd] = state.fpu[rs] / state.fpu[rt]; break; // DIV.S
                        case 0x05: state.fpu[rd] = fabsf(state.fpu[rs]); break; // ABS.S
                        case 0x06: state.fpu[rd] = state.fpu[rs]; break; // MOV.S
                        case 0x07: state.fpu[rd] = -state.fpu[rs]; break; // NEG.S
                    }
                    break;
            }
            break;
        case 0x12: // COP2 (VU0 macro) - stub, skip
            break;
        case 0x1C: // MMI - stub, skip
            break;
        case 0x1E: { // LQ
            uint32_t addr = state.gpr_lo[rs] + simm;
            uint8_t tmp[16];
            ee_mem_read128(addr, tmp);
            uint64_t lo1 = *reinterpret_cast<uint64_t*>(tmp);
            uint64_t hi1 = *reinterpret_cast<uint64_t*>(tmp + 8);
            state.gpr_lo[rt] = lo1;
            state.gpr_hi[rt] = hi1;
            break;
        }
        case 0x1F: { // SQ
            uint32_t addr = state.gpr_lo[rs] + simm;
            uint8_t tmp[16];
            *reinterpret_cast<uint64_t*>(tmp) = state.gpr_lo[rt];
            *reinterpret_cast<uint64_t*>(tmp + 8) = state.gpr_hi[rt];
            ee_mem_write128(addr, tmp);
            break;
        }
        case 0x20: { int8_t v = (int8_t)ee_mem_read8(state.gpr_lo[rs] + simm); state.gpr_lo[rt] = (int32_t)v; break; } // LB
        case 0x21: { int16_t v = (int16_t)ee_mem_read16(state.gpr_lo[rs] + simm); state.gpr_lo[rt] = (int32_t)v; break; } // LH
        case 0x23: state.gpr_lo[rt] = (int32_t)read32(state.gpr_lo[rs] + simm); break; // LW
        case 0x24: state.gpr_lo[rt] = (uint32_t)ee_mem_read8(state.gpr_lo[rs] + simm); break; // LBU
        case 0x25: state.gpr_lo[rt] = (uint32_t)ee_mem_read16(state.gpr_lo[rs] + simm); break; // LHU
        case 0x27: state.gpr_lo[rt] = (uint32_t)read32(state.gpr_lo[rs] + simm); break; // LWU
        case 0x28: ee_mem_write8(state.gpr_lo[rs] + simm, (uint8_t)state.gpr_lo[rt]); break; // SB
        case 0x29: ee_mem_write16(state.gpr_lo[rs] + simm, (uint16_t)state.gpr_lo[rt]); break; // SH
        case 0x2B: write32(state.gpr_lo[rs] + simm, (uint32_t)state.gpr_lo[rt]); break; // SW
        case 0x31: { // LWC1
            uint32_t val = read32(state.gpr_lo[rs] + simm);
            memcpy(&state.fpu[rt], &val, 4);
            break;
        }
        case 0x39: { // SWC1
            uint32_t val;
            memcpy(&val, &state.fpu[rt], 4);
            write32(state.gpr_lo[rs] + simm, val);
            break;
        }
        case 0x37: { // LD
            uint32_t addr = state.gpr_lo[rs] + simm;
            state.gpr_lo[rt] = (uint64_t)read32(addr) | ((uint64_t)read32(addr + 4) << 32);
            break;
        }
        case 0x3F: { // SD
            uint32_t addr = state.gpr_lo[rs] + simm;
            write32(addr, (uint32_t)state.gpr_lo[rt]);
            write32(addr + 4, (uint32_t)(state.gpr_lo[rt] >> 32));
            break;
        }
        case 0x2F: case 0x33: break; // CACHE / PREF - no-op
        case 0x32: case 0x3A: break; // LWC2 / SWC2 - stub
        case 0x35: case 0x36: case 0x3D: case 0x3E: break; // LDC1/LDC2/SDC1/SDC2 - stub
    }

    if (state.branch_delay) {
        state.pc = state.branch_target;
        state.branch_delay = false;
    } else if (will_branch) {
        state.branch_delay = true;
        state.branch_target = next_pc;
        state.pc += 4; 
    } else {
        state.pc = next_pc;
    }
}

// ─── Bucle de ejecución principal ─────────────────────────────────────────────
void EE_Core::run_cycles(int64_t cycles) {
    if (!cache || !cache->is_valid() || !ee_ram) {
        state.halted = true;
        return;
    }
    if (!recompiler) {
        recompiler = std::make_unique<EE_Recompiler>(*cache, state, ee_ram.get());
    }

    int64_t cycles_run = 0;

    while (cycles_run < cycles && !state.halted) {
        uint32_t pc = state.pc;

        // BIOS function interception (HLE)
        uint32_t bios_pc = 0;
        if (PS2_BIOS::intercept_bios_call(pc, bios_pc)) {
            state.pc = bios_pc;
            cycles_run += 2;
            continue;
        }

        // Buscar bloque ya compilado en el cache
        auto fn = reinterpret_cast<EE_Recompiler::CompiledBlock>(cache->lookup(pc));

        if (!fn) {
            fn = recompiler->compile_block(pc);
            if (!fn) {
                LOGE("compile_block falló para PC=0x%08X — halting", pc);
                state.halted = true;
                break;
            }
        }

        // Validate block pointer is within code cache range
        if (!cache->is_in_code(reinterpret_cast<void*>(fn))) {
            LOGE("Block fn=%p out of code cache range for PC=0x%08X — halting", (void*)fn, pc);
            state.halted = true;
            break;
        }

        // Ejecutar el bloque compilado (código ARM64 nativo)
        g_pc_ring[g_pc_ring_idx] = pc;
        g_pc_ring_idx = (g_pc_ring_idx + 1) % PC_RING_SIZE;
        fn(&state, ee_ram.get());

        cycles_run += 64;

        if (state.interrupt_pending) {
            handle_interrupt();
        }
    }
}

void EE_Core::handle_interrupt() {
    uint32_t status = state.cop0[12];
    uint32_t cause  = state.cop0[13];

    if (!(status & 1)) return;       // IE=0: interrupts globally disabled
    if (status & 0x2) return;        // EXL=1: exception in progress, don't re-enter

    uint32_t pending = (cause >> 8) & (status >> 8) & 0xFF;
    if (!pending) { state.interrupt_pending = false; return; }

    state.cop0[14] = state.pc;
    state.cop0[13] = (cause & ~0x7C) | (0 << 2);  // ExcCode=0 (Interrupt)
    state.cop0[12] = status | 0x2;  // Set EXL

    state.pc = 0x80000200u;
    state.interrupt_pending = false;
}

void EE_Core::raise_interrupt(int irq) {
    state.cop0[13] |= (1 << (8 + irq));
    state.interrupt_pending = true;
}

// ─── Acceso a memoria desde fuera del bucle JIT ───────────────────────────────
uint32_t EE_Core::read32(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < EE_RAM_SIZE)
        return *reinterpret_cast<uint32_t*>(ee_ram.get() + phys);
    if (phys >= 0x1FC00000u && phys < 0x20000000u) {
        uint32_t bios_offset = phys - 0x1FC00000u;
        if (bios_offset < BIOS_ROM_SIZE)
            return *reinterpret_cast<uint32_t*>(bios_rom.get() + bios_offset);
    }
    return 0;
}

void EE_Core::write32(uint32_t addr, uint32_t val) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < EE_RAM_SIZE) {
        if (cache && cache->is_valid())
            cache->invalidate_range(phys, phys + 4);
        *reinterpret_cast<uint32_t*>(ee_ram.get() + phys) = val;
    }
}