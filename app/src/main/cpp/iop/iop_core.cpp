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

// IOP BIOS HLE function addresses (physical, unmapped region 0x1FC00000+)
static constexpr uint32_t IOP_BIOS_ENTRY_RESET     = 0xBFC00000;
static constexpr uint32_t IOP_BIOS_ENTRY_EXCEPTION  = 0x80000180;
static constexpr uint32_t IOP_BIOS_ENTRY_SYSCALL    = 0x80000170;

// Common IOP BIOS function offsets (from IOP BIOS disassembly)
static constexpr uint32_t IOP_FN_QueryBootMode     = 0x00000520;
static constexpr uint32_t IOP_FN_SetAlarm          = 0x00000D20;
static constexpr uint32_t IOP_FN_CancelAlarm       = 0x00000D30;
static constexpr uint32_t IOP_FN_ReferIntCounter   = 0x00000CD0;
static constexpr uint32_t IOP_FN_DisableIntc       = 0x00000010;
static constexpr uint32_t IOP_FN_EnableIntc        = 0x00000020;
static constexpr uint32_t IOP_FN_DisableDmac       = 0x00000040;
static constexpr uint32_t IOP_FN_EnableDmac        = 0x00000050;
static constexpr uint32_t IOP_FN_SetSifInit        = 0x00001000;
static constexpr uint32_t IOP_FN_SetGsCrt          = 0x00001500;
static constexpr uint32_t IOP_FN_SetVTLBRefillHandler = 0x00001620;
static constexpr uint32_t IOP_FN_SetVCommonHandler = 0x00001600;
static constexpr uint32_t IOP_FN_ExecPS2           = 0x00001D50;
static constexpr uint32_t IOP_FN_LoadExecPS2       = 0x00001D20;
static constexpr uint32_t IOP_FN_SetGsCrt2         = 0x00002000;
static constexpr uint32_t IOP_FN_SifLoadModule     = 0x00002D10;
static constexpr uint32_t IOP_FN_SifExecModuleBuffer = 0x00002D80;
static constexpr uint32_t IOP_FN_AddSifDmaHandler  = 0x00001008;
static constexpr uint32_t IOP_FN_Sync              = 0x00003540;
static constexpr uint32_t IOP_FN_FFlush            = 0x00003620;
static constexpr uint32_t IOP_FN_FRead             = 0x00003640;
static constexpr uint32_t IOP_FN_FWrite            = 0x00003660;
static constexpr uint32_t IOP_FN_Open              = 0x00003720;
static constexpr uint32_t IOP_FN_Close             = 0x00003740;
static constexpr uint32_t IOP_FN_Lseek             = 0x00003760;
static constexpr uint32_t IOP_FN_Exit              = 0x00001C40;
static constexpr uint32_t IOP_FN_SetMemMode        = 0x00001C00;
static constexpr uint32_t IOP_FN_GsInitGraph       = 0x00002000;
static constexpr uint32_t IOP_FN_GsSetCRTMode      = 0x00002100;
static constexpr uint32_t IOP_FN_GsResetGraph      = 0x00001640;
static constexpr uint32_t IOP_FN_DmaExecute        = 0x00002A50;

static bool iop_intercept_bios_call(IOP_Core* iop, uint32_t pc, uint32_t& new_pc) {
    // Check if PC is in BIOS ROM area (KSEG1: 0xBFC00000-0xBFC03FFF)
    // or physical BIOS ROM area (0x1FC00000-0x1FC03FFF mapped via bus)
    uint32_t bios_offset = 0;
    if (pc >= 0xBFC00000u && pc < 0xBFC04000u) {
        bios_offset = pc - 0xBFC00000u;
    } else if (pc >= 0x1FC00000u && pc < 0x1FC04000u) {
        bios_offset = pc - 0x1FC00000u;
    } else {
        return false;
    }

    switch (bios_offset) {
    case IOP_FN_SetGsCrt:
    case IOP_FN_SetGsCrt2: // same offset as GsInitGraph on some BIOS versions
        LOGI("IOP BIOS HLE: SetGsCrt/GsInitGraph -> NOP");
        new_pc = pc + 8;
        return true;
    case IOP_FN_SetSifInit:
        LOGI("IOP BIOS HLE: SetSifInit -> NOP");
        new_pc = pc + 8;
        return true;
    case IOP_FN_QueryBootMode:
        LOGI("IOP BIOS HLE: QueryBootMode -> return 0 (full boot)");
        iop->state.gpr[2] = 0; // return 0 (normal boot mode)
        new_pc = pc + 8;
        return true;
    case IOP_FN_DisableIntc:
    case IOP_FN_EnableIntc:
    case IOP_FN_DisableDmac:
    case IOP_FN_EnableDmac:
        LOGI("IOP BIOS HLE: INTC/DMAC control -> NOP (return 0)");
        iop->state.gpr[2] = 0;
        new_pc = pc + 8;
        return true;
    case IOP_FN_SetAlarm:
    case IOP_FN_CancelAlarm:
        LOGI("IOP BIOS HLE: Alarm -> NOP");
        new_pc = pc + 8;
        return true;
    case IOP_FN_SifLoadModule:
    case IOP_FN_SifExecModuleBuffer:
        LOGI("IOP BIOS HLE: SifLoadModule/SifExecModuleBuffer -> return 1");
        iop->state.gpr[2] = 1; // success
        new_pc = pc + 8;
        return true;
    case IOP_FN_AddSifDmaHandler:
        LOGI("IOP BIOS HLE: AddSifDmaHandler -> return 0");
        iop->state.gpr[2] = 0;
        new_pc = pc + 8;
        return true;
    case IOP_FN_SetVTLBRefillHandler:
    case IOP_FN_SetVCommonHandler:
        LOGI("IOP BIOS HLE: SetVHandler -> NOP");
        new_pc = pc + 8;
        return true;
    case IOP_FN_LoadExecPS2:
    case IOP_FN_ExecPS2:
        LOGI("IOP BIOS HLE: ExecPS2 -> NOP");
        new_pc = pc + 8;
        return true;
    case IOP_FN_Sync:
        LOGI("IOP BIOS HLE: Sync -> NOP (return 0)");
        iop->state.gpr[2] = 0;
        new_pc = pc + 8;
        return true;
    case IOP_FN_ReferIntCounter:
        LOGI("IOP BIOS HLE: ReferIntCounter -> return 0");
        iop->state.gpr[2] = 0;
        new_pc = pc + 8;
        return true;
    case IOP_FN_Open:
    case IOP_FN_Close:
    case IOP_FN_FFlush:
    case IOP_FN_FRead:
    case IOP_FN_FWrite:
    case IOP_FN_Lseek:
        LOGI("IOP BIOS HLE: File I/O fn 0x%03X -> return 0", bios_offset);
        iop->state.gpr[2] = 0;
        new_pc = pc + 8;
        return true;
    case IOP_FN_GsSetCRTMode:
    case IOP_FN_GsResetGraph:
        LOGI("IOP BIOS HLE: GS fn -> NOP");
        new_pc = pc + 8;
        return true;
    case IOP_FN_Exit:
        LOGI("IOP BIOS HLE: Exit -> halt IOP");
        iop->state.halted = true;
        return true;
    case IOP_FN_DmaExecute:
        LOGI("IOP BIOS HLE: DmaExecute -> return 0");
        iop->state.gpr[2] = 0;
        new_pc = pc + 8;
        return true;
    case IOP_FN_SetMemMode:
        LOGI("IOP BIOS HLE: SetMemMode -> NOP");
        new_pc = pc + 8;
        return true;
    default:
        if (bios_offset < 0x4000) {
            LOGI("IOP BIOS HLE: Unknown function at offset 0x%04X (PC=0x%08X) -> NOP", bios_offset, pc);
            new_pc = pc + 8;
            return true;
        }
        break;
    }
    return false;
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
    if (!cache || !cache->is_valid()) {
        interpret_single_instruction();
        return;
    }
    int64_t cycles_run = 0;
    
    while (cycles_run < cycles) {
        // El PC del IOP puede dar la vuelta si llega al final de la RAM
        if (state.pc >= 0x20000000 && state.pc < 0x70000000) {
            state.pc = 0xBFC00000;
        }

        // IOP BIOS HLE interception
        uint32_t bios_new_pc = 0;
        if (iop_intercept_bios_call(this, state.pc, bios_new_pc)) {
            state.pc = bios_new_pc;
            cycles_run += 1;
            continue;
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
    uint32_t sa = (instr >> 6) & 0x1F;
    uint32_t funct = instr & 0x3F;
    uint16_t imm = instr & 0xFFFF;
    int32_t simm = (int16_t)imm;
    uint32_t uimm = imm;

    state.gpr[0] = 0; 

    switch (opcode) {
        case 0x00: // SPECIAL
            switch (funct) {
                case 0x00: state.gpr[rd] = (int32_t)((uint32_t)state.gpr[rt] << sa); break; // SLL
                case 0x02: state.gpr[rd] = (int32_t)((uint32_t)state.gpr[rt] >> sa); break; // SRL
                case 0x03: state.gpr[rd] = (int32_t)((int32_t)state.gpr[rt] >> sa); break; // SRA
                case 0x04: state.gpr[rd] = (int32_t)((uint32_t)state.gpr[rt] << (state.gpr[rs] & 0x1F)); break; // SLLV
                case 0x06: state.gpr[rd] = (int32_t)((uint32_t)state.gpr[rt] >> (state.gpr[rs] & 0x1F)); break; // SRLV
                case 0x07: state.gpr[rd] = (int32_t)((int32_t)state.gpr[rt] >> (state.gpr[rs] & 0x1F)); break; // SRAV
                case 0x08: will_branch = true; next_pc = state.gpr[rs]; break; // JR
                case 0x09: state.gpr[rd ? rd : 31] = state.pc + 8; will_branch = true; next_pc = state.gpr[rs]; break; // JALR
                case 0x0C: break; // SYSCALL - TODO: proper exception handling
                case 0x0D: break; // BREAK
                case 0x10: state.gpr[rd] = (int32_t)state.hi; break; // MFHI
                case 0x11: state.hi = (int32_t)state.gpr[rs]; break; // MTHI
                case 0x12: state.gpr[rd] = (int32_t)state.lo; break; // MFLO
                case 0x13: state.lo = (int32_t)state.gpr[rs]; break; // MTLO
                case 0x18: { // MULT
                    int64_t result = (int64_t)(int32_t)state.gpr[rs] * (int64_t)(int32_t)state.gpr[rt];
                    state.lo = (int32_t)(result & 0xFFFFFFFF);
                    state.hi = (int32_t)((result >> 32) & 0xFFFFFFFF);
                    break;
                }
                case 0x19: { // MULTU
                    uint64_t result = (uint64_t)(uint32_t)state.gpr[rs] * (uint64_t)(uint32_t)state.gpr[rt];
                    state.lo = (int32_t)(result & 0xFFFFFFFF);
                    state.hi = (int32_t)((result >> 32) & 0xFFFFFFFF);
                    break;
                }
                case 0x1A: { // DIV
                    int32_t n = (int32_t)state.gpr[rs];
                    int32_t d = (int32_t)state.gpr[rt];
                    if (d != 0) { state.lo = n / d; state.hi = n % d; }
                    break;
                }
                case 0x1B: { // DIVU
                    uint32_t n = (uint32_t)state.gpr[rs];
                    uint32_t d = (uint32_t)state.gpr[rt];
                    if (d != 0) { state.lo = (int32_t)(n / d); state.hi = (int32_t)(n % d); }
                    break;
                }
                case 0x20: state.gpr[rd] = (int32_t)(state.gpr[rs] + state.gpr[rt]); break; // ADD
                case 0x21: state.gpr[rd] = (int32_t)(state.gpr[rs] + state.gpr[rt]); break; // ADDU
                case 0x22: state.gpr[rd] = (int32_t)(state.gpr[rs] - state.gpr[rt]); break; // SUB
                case 0x23: state.gpr[rd] = (int32_t)(state.gpr[rs] - state.gpr[rt]); break; // SUBU
                case 0x24: state.gpr[rd] = state.gpr[rs] & state.gpr[rt]; break; // AND
                case 0x25: state.gpr[rd] = state.gpr[rs] | state.gpr[rt]; break; // OR
                case 0x26: state.gpr[rd] = state.gpr[rs] ^ state.gpr[rt]; break; // XOR
                case 0x27: state.gpr[rd] = ~(state.gpr[rs] | state.gpr[rt]); break; // NOR
                case 0x2A: state.gpr[rd] = (state.gpr[rs] < state.gpr[rt]) ? 1 : 0; break; // SLT
                case 0x2B: state.gpr[rd] = ((uint32_t)state.gpr[rs] < (uint32_t)state.gpr[rt]) ? 1 : 0; break; // SLTU
            }
            break;
        case 0x01: { // REGIMM
            int32_t offset = simm << 2;
            bool taken = false;
            switch (rt) {
                case 0x00: taken = (int32_t)state.gpr[rs] < 0; break;  // BLTZ
                case 0x01: taken = (int32_t)state.gpr[rs] >= 0; break; // BGEZ
                case 0x10: taken = (int32_t)state.gpr[rs] < 0;         // BLTZAL
                           state.gpr[31] = state.pc + 8; break;
                case 0x11: taken = (int32_t)state.gpr[rs] >= 0;        // BGEZAL
                           state.gpr[31] = state.pc + 8; break;
            }
            if (taken) { will_branch = true; next_pc = state.pc + 4 + offset; }
            break;
        }
        case 0x02: will_branch = true; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break; // J
        case 0x03: state.gpr[31] = state.pc + 8; will_branch = true; next_pc = (state.pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break; // JAL
        case 0x04: if (state.gpr[rs] == state.gpr[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break; // BEQ
        case 0x05: if (state.gpr[rs] != state.gpr[rt]) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break; // BNE
        case 0x06: if ((int32_t)state.gpr[rs] <= 0) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break; // BLEZ
        case 0x07: if ((int32_t)state.gpr[rs] > 0) { will_branch = true; next_pc = state.pc + 4 + (simm << 2); } break;  // BGTZ
        case 0x08: state.gpr[rt] = (int32_t)(state.gpr[rs] + simm); break; // ADDI
        case 0x09: state.gpr[rt] = (int32_t)(state.gpr[rs] + simm); break; // ADDIU
        case 0x0A: state.gpr[rt] = ((int32_t)state.gpr[rs] < simm) ? 1 : 0; break; // SLTI
        case 0x0B: state.gpr[rt] = ((uint32_t)state.gpr[rs] < (uint32_t)(int32_t)simm) ? 1 : 0; break; // SLTIU
        case 0x0C: state.gpr[rt] = state.gpr[rs] & uimm; break; // ANDI
        case 0x0D: state.gpr[rt] = state.gpr[rs] | uimm; break; // ORI
        case 0x0E: state.gpr[rt] = state.gpr[rs] ^ uimm; break; // XORI
        case 0x0F: state.gpr[rt] = (int32_t)(uimm << 16); break; // LUI
        case 0x10: // COP0
            switch (rs) {
                case 0x00: state.gpr[rt] = (int32_t)state.cop0[rd]; break; // MFC0
                case 0x04: state.cop0[rd] = state.gpr[rt]; break; // MTC0
                case 0x10: // CO sub-op
                    switch (funct) {
                        case 0x18: // RFE
                            state.cop0[12] = (state.cop0[12] & 0x3F) | ((state.cop0[12] >> 2) & 0xF);
                            break;
                    }
                    break;
            }
            break;
        case 0x20: { int8_t v = (int8_t)read8(state.gpr[rs] + simm); state.gpr[rt] = (int32_t)v; break; } // LB
        case 0x21: { int16_t v = (int16_t)read16(state.gpr[rs] + simm); state.gpr[rt] = (int32_t)v; break; } // LH
        case 0x22: { // LWL
            uint32_t addr = state.gpr[rs] + simm;
            uint32_t aligned = read32(addr & ~3u);
            uint32_t shift = (addr & 3) * 8;
            state.gpr[rt] = (state.gpr[rt] & (0xFFFFFFFF >> (32 - shift))) | (aligned << shift);
            break;
        }
        case 0x23: state.gpr[rt] = (int32_t)read32(state.gpr[rs] + simm); break; // LW
        case 0x24: state.gpr[rt] = (uint32_t)read8(state.gpr[rs] + simm); break; // LBU
        case 0x25: state.gpr[rt] = (uint32_t)(int16_t)read16(state.gpr[rs] + simm); break; // LHU
        case 0x26: { // LWR
            uint32_t addr = state.gpr[rs] + simm;
            uint32_t aligned = read32(addr & ~3u);
            uint32_t shift = (addr & 3) * 8;
            state.gpr[rt] = (state.gpr[rt] & (0xFFFFFFFF << shift)) | (aligned >> (32 - shift));
            break;
        }
        case 0x28: write8(state.gpr[rs] + simm, (uint8_t)state.gpr[rt]); break; // SB
        case 0x29: write16(state.gpr[rs] + simm, (uint16_t)state.gpr[rt]); break; // SH
        case 0x2A: { // SWL
            uint32_t addr = state.gpr[rs] + simm;
            uint32_t old = read32(addr & ~3u);
            uint32_t shift = (addr & 3) * 8;
            uint32_t val = (old & (0xFFFFFFFF << shift)) | ((uint32_t)state.gpr[rt] >> (32 - shift));
            write32(addr & ~3u, val);
            break;
        }
        case 0x2B: write32(state.gpr[rs] + simm, (uint32_t)state.gpr[rt]); break; // SW
        case 0x2E: { // SWR
            uint32_t addr = state.gpr[rs] + simm;
            uint32_t old = read32(addr & ~3u);
            uint32_t shift = (addr & 3) * 8;
            uint32_t val = (old & (0xFFFFFFFF >> (32 - shift))) | ((uint32_t)state.gpr[rt] << shift);
            write32(addr & ~3u, val);
            break;
        }
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

uint8_t IOP_Core::read8(uint32_t addr) {
    return iop_bus_read8(addr);
}

uint16_t IOP_Core::read16(uint32_t addr) {
    return iop_bus_read16(addr);
}

void IOP_Core::write8(uint32_t addr, uint8_t val) {
    iop_bus_write8(addr, val);
}

void IOP_Core::write16(uint32_t addr, uint16_t val) {
    iop_bus_write16(addr, val);
}

uint8_t IOP_Core::read_pad(int port, int byte) {
    return 0;
}