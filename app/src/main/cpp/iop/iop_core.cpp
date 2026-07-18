#include "iop_core.h"
#include "iop_recompiler.h"
#include "../ee/code_cache.h"
#include <cstring>
#include <cstdio>
#include <android/log.h>

#define TAG "PS2_IOP_DEBUG"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" void push_jit_log(const char* msg);

uint8_t* g_iop_ram_ptr = nullptr;
uint32_t g_sif_buffer[1024]; 

extern "C" {
    uint8_t iop_bus_read8(uint32_t addr) {
        if (!g_iop_ram_ptr) return 0;
        // Mapear la dirección de la BIOS (0xBFC00000) al inicio de la RAM del IOP
        if (addr >= 0x1FC00000 && addr < 0x20000000) addr = (addr - 0x1FC00000) & (IOP_RAM_SIZE - 1);
        else addr &= (IOP_RAM_SIZE - 1);
        return g_iop_ram_ptr[addr];
    }
    uint16_t iop_bus_read16(uint32_t addr) {
        if (!g_iop_ram_ptr) return 0;
        if (addr >= 0x1FC00000 && addr < 0x20000000) addr = (addr - 0x1FC00000) & (IOP_RAM_SIZE - 1);
        else addr &= (IOP_RAM_SIZE - 1);
        return g_iop_ram_ptr[addr] | (g_iop_ram_ptr[addr + 1] << 8);
    }
    uint32_t iop_bus_read32(uint32_t addr) {
        if (!g_iop_ram_ptr) return 0;
        if (addr >= 0x1FC00000 && addr < 0x20000000) addr = (addr - 0x1FC00000) & (IOP_RAM_SIZE - 1);
        else addr &= (IOP_RAM_SIZE - 1);
        return g_iop_ram_ptr[addr] | 
               (g_iop_ram_ptr[addr + 1] << 8) | 
               (g_iop_ram_ptr[addr + 2] << 16) | 
               (g_iop_ram_ptr[addr + 3] << 24);
    }
    void iop_bus_write8(uint32_t addr, uint8_t val) {
        if (!g_iop_ram_ptr) return;
        addr &= (IOP_RAM_SIZE - 1);
        g_iop_ram_ptr[addr] = val;
    }
    void iop_bus_write16(uint32_t addr, uint16_t val) {
        if (!g_iop_ram_ptr) return;
        addr &= (IOP_RAM_SIZE - 1);
        g_iop_ram_ptr[addr] = val & 0xFF;
        g_iop_ram_ptr[addr + 1] = (val >> 8) & 0xFF;
    }
    void iop_bus_write32(uint32_t addr, uint32_t val) {
        if (!g_iop_ram_ptr) return;
        addr &= (IOP_RAM_SIZE - 1);
        g_iop_ram_ptr[addr]     = val & 0xFF;
        g_iop_ram_ptr[addr + 1] = (val >> 8) & 0xFF;
        g_iop_ram_ptr[addr + 2] = (val >> 16) & 0xFF;
        g_iop_ram_ptr[addr + 3] = (val >> 24) & 0xFF;
    }
}

IOP_Core::IOP_Core() {
    memset(&state, 0, sizeof(IOP_State));
    memset(iop_ram, 0, IOP_RAM_SIZE);
    
    g_iop_ram_ptr = iop_ram;
    
    // El vector de RESET del IOP en la PS2 es 0xBFC00000
    state.pc = 0xBFC00000;
    
    cache = std::make_unique<CodeCache>();
    jit = std::make_unique<IOP_Recompiler>(*cache, state, iop_ram);
}

IOP_Core::~IOP_Core() = default;

void IOP_Core::run_cycles(int64_t cycles) {
    if (state.halted) return;
    int64_t cycles_run = 0;
    
    while (cycles_run < cycles) {
        // El PC del IOP puede dar la vuelta si llega al final de la RAM
        if (state.pc >= 0x20000000 && state.pc < 0x70000000) {
            state.pc = 0xBFC00000;
        }

        static int log_counter = 0;
        log_counter++;
        if (log_counter % 1000 == 0) {
            char log_buf[128];
            snprintf(log_buf, sizeof(log_buf), "IOP Run PC: 0x%08X\n", state.pc);
            push_jit_log(log_buf);
        }

        if (jit && cache) {
            using BlockFn = void (*)(IOP_State*, uint8_t*);
            BlockFn fn = reinterpret_cast<BlockFn>(cache->lookup(state.pc));
            
            if (!fn) {
                fn = reinterpret_cast<BlockFn>(jit->compile_block(state.pc));
            }
            
            if (fn) {
                fn(&state, iop_ram);
                cycles_run += 1; 
                continue;
            }

            // JIT compilation failed — execute single instruction via interpreter to avoid infinite loop
            LOGE("IOP JIT failed at PC=0x%08X, falling back to interpreter", state.pc);
            interpret_single_instruction();
            cycles_run += 1;
        } else {
            // No JIT available — use interpreter
            interpret_single_instruction();
            cycles_run += 1;
        }
    }
}

void IOP_Core::interpret_single_instruction() {
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

    state.gpr[0] = 0; 

    switch (opcode) {
        case 0x00: 
            switch (funct) {
                case 0x08: will_branch = true; next_pc = state.gpr[rs]; break;
                case 0x21: state.gpr[rd] = state.gpr[rs] + state.gpr[rt]; break;
                case 0x25: state.gpr[rd] = state.gpr[rs] | state.gpr[rt]; break;
            }
            break;
        case 0x02: will_branch = true; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;
        case 0x03: will_branch = true; state.gpr[31] = state.pc + 8; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;
        case 0x04: if (state.gpr[rs] == state.gpr[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break;
        case 0x05: if (state.gpr[rs] != state.gpr[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break;
        case 0x09: state.gpr[rt] = state.gpr[rs] + simm; break;
        case 0x0D: state.gpr[rt] = state.gpr[rs] | imm; break;
        case 0x23: state.gpr[rt] = read32(state.gpr[rs] + simm); break;
        case 0x2B: write32(state.gpr[rs] + simm, state.gpr[rt]); break;
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

uint32_t IOP_Core::read32(uint32_t addr) {
    return iop_bus_read32(addr);
}

void IOP_Core::write32(uint32_t addr, uint32_t val) {
    iop_bus_write32(addr, val);
}

uint8_t IOP_Core::read_pad(int port, int byte) {
    return 0;
}