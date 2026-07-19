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
    LOGI("EE Core iniciado. RAM: %zu MB", EE_RAM_SIZE / (1024*1024));
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
    uint32_t funct = instr & 0x3F;
    uint16_t imm = instr & 0xFFFF;
    int32_t simm = (int16_t)imm;

    state.gpr_lo[0] = 0;

    switch (opcode) {
        case 0x00:
            switch (funct) {
                case 0x08: will_branch = true; next_pc = state.gpr_lo[rs]; break;
                case 0x21: state.gpr_lo[rd] = state.gpr_lo[rs] + state.gpr_lo[rt]; break;
                case 0x25: state.gpr_lo[rd] = state.gpr_lo[rs] | state.gpr_lo[rt]; break;
            }
            break;
        case 0x02: will_branch = true; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;
        case 0x03: will_branch = true; state.gpr_lo[31] = state.pc + 8; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;
        case 0x04: if (state.gpr_lo[rs] == state.gpr_lo[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break;
        case 0x05: if (state.gpr_lo[rs] != state.gpr_lo[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break;
        case 0x09: state.gpr_lo[rt] = state.gpr_lo[rs] + simm; break;
        case 0x0D: state.gpr_lo[rt] = state.gpr_lo[rs] | imm; break;
        case 0x23: state.gpr_lo[rt] = read32(state.gpr_lo[rs] + simm); break;
        case 0x2B: write32(state.gpr_lo[rs] + simm, state.gpr_lo[rt]); break;
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

    if (!(status & 1)) return;

    uint32_t pending = (cause >> 8) & (status >> 8) & 0xFF;
    if (!pending) { state.interrupt_pending = false; return; }

    state.cop0[14] = state.pc;
    state.cop0[13] = (cause & ~0x7C) | (0 << 2);
    state.cop0[12] = status | 0x2;

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