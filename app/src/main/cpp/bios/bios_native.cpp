// bios/bios_native.cpp
// PS2 BIOS HLE (High-Level Emulation) with full 4MB ROM support
// Supports both HLE boot (direct to game) and LLE boot (BIOS ROM execution)
#include "bios_native.h"
#include "../ee/ee_core.h"
#include "../iop/iop_core.h"
#include <android/log.h>
#include <cstring>

#define TAG "PS2-BIOS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static EE_Core* g_ee_native = nullptr;
static IOP_Core* g_iop_native = nullptr;
static bool g_bios_initialized = false;
static bool g_lle_mode = false;  // Low-Level Emulation (real BIOS execution)
static uint32_t g_game_entry_point = 0x00100000;
static uint8_t g_bios_scratchpad[16 * 1024];

// ─── BIOS ROM function table (HLE stubs for common BIOS calls) ───────────────
// These replace BIOS ROM functions so the EE can boot without executing the full BIOS.
// When LLE mode is active, the real BIOS ROM is executed instead.

// PS2 BIOS ROM entry points (from BIOS disassembly)
static constexpr uint32_t BIOS_ENTRY_RESET     = 0xBFC00000;
static constexpr uint32_t BIOS_ENTRY_EXCEPTION = 0x80000180;
static constexpr uint32_t BIOS_ENTRY_SYSCALL   = 0x80000170;
static constexpr uint32_t BIOS_ENTRY_INTERRUPT  = 0x80000200;

// Known BIOS function addresses (for HLE interception)
static constexpr uint32_t BIOS_FN_SifInit         = 0x00001000;
static constexpr uint32_t BIOS_FN_SetGsCrt        = 0x00001500;
static constexpr uint32_t BIOS_FN_GsResetGraph    = 0x00001640;
static constexpr uint32_t BIOS_FN_Exit            = 0x00001C40;
static constexpr uint32_t BIOS_FN_LoadExecPS2     = 0x00001D20;
static constexpr uint32_t BIOS_FN_GetRomFileName   = 0x00001F30;
static constexpr uint32_t BIOS_FN_GsInitGraph     = 0x00002000;
static constexpr uint32_t BIOS_FN_GsSetCRTMode    = 0x00002100;
static constexpr uint32_t BIOS_FN_SifLoadModule   = 0x00002D10;
static constexpr uint32_t BIOS_FN_SifExecModuleBuffer = 0x00002D80;
static constexpr uint32_t BIOS_FN_Sync            = 0x00003540;
static constexpr uint32_t BIOS_FN_FFlush          = 0x00003620;
static constexpr uint32_t BIOS_FN_FRead           = 0x00003640;
static constexpr uint32_t BIOS_FN_FWrite          = 0x00003660;
static constexpr uint32_t BIOS_FN_Open            = 0x00003720;
static constexpr uint32_t BIOS_FN_Close           = 0x00003740;
static constexpr uint32_t BIOS_FN_Lseek           = 0x00003760;
static constexpr uint32_t BIOS_FN_DmaExecute      = 0x00002A50;
static constexpr uint32_t BIOS_FN_ExecPS2         = 0x00001D50;
static constexpr uint32_t BIOS_FN_SetVTLBRefillHandler = 0x00001620;
static constexpr uint32_t BIOS_FN_SetVCommonHandler = 0x00001600;
static constexpr uint32_t BIOS_FN_AddSifDmaHandler = 0x00001008;

void PS2_BIOS::init() {
    g_bios_initialized = false;
    g_lle_mode = false;
    LOGI("BIOS HLE system initialized.");
}

void PS2_BIOS::set_ee_core(EE_Core* ee) { g_ee_native = ee; }
void PS2_BIOS::set_iop_core(IOP_Core* iop) { g_iop_native = iop; }

void PS2_BIOS::set_game_entry(uint32_t entry) {
    g_game_entry_point = entry;
    LOGI("Game entry point: 0x%08X", entry);
}

void PS2_BIOS::set_lle_mode(bool lle) {
    g_lle_mode = lle;
    LOGI("BIOS mode: %s", lle ? "LLE (real BIOS)" : "HLE (fast boot)");
}

void PS2_BIOS::reset() {
    g_bios_initialized = false;
}

// ─── HLE BIOS execution ──────────────────────────────────────────────────────
void PS2_BIOS::execute() {
    if (g_bios_initialized) return;
    if (!g_ee_native) return;

    if (g_lle_mode) {
        execute_lle();
    } else {
        execute_hle();
    }
}

void PS2_BIOS::execute_hle() {
    LOGI("HLE boot: Fast-booting directly to game...");

    uint32_t* ee_ram = (uint32_t*)g_ee_native->get_ram();

    // 1. Initialize SIF mailbox to wake EE from kernel loop
    ee_ram[0x0000F200 >> 2] = 0x00010000;
    ee_ram[0x0000F240 >> 2] = 0x00000001; // SIF.SIF_RPCInitialize

    // 2. Setup kernel structures in EE RAM
    // Clear stack area
    memset(g_ee_native->get_ram() + 0x00100000, 0, 0x00080000);

    // 3. Setup COP0 registers
    // Status: BEV=1, ERL=1 (initial state after reset)
    g_ee_native->state.cop0[12] = 0x00000004; // Status: BEV=1
    // PRId: R5900 revision
    g_ee_native->state.cop0[15] = 0x00002E20;
    // EBASE: exception base address
    g_ee_native->state.cop0[15] = 0x80000000;

    // 4. Setup TLB (minimal: map KSEG0/KSEG1 as unmapped)
    // These are handled by the address masking in ee_memory.h

    // 5. Setup Stack Pointer and Frame Pointer
    g_ee_native->state.gpr_lo[SP] = 0x01FFFFF0;
    g_ee_native->state.gpr_lo[28] = 0x01FFFFF0; // GP (will be set by game)

    // 6. Clear return address
    g_ee_native->state.gpr_lo[RA] = 0;

    // 7. Enable interrupts
    g_ee_native->state.cop0[12] = 0x00010401; // Status: IE=1, EXL=0, KSU=0

    // 8. Jump to game entry point
    g_ee_native->state.pc = g_game_entry_point;
    LOGI("HLE boot: EE jumping to 0x%08X", g_game_entry_point);

    g_bios_initialized = true;
}

// ─── LLE BIOS execution (real BIOS ROM) ──────────────────────────────────────
void PS2_BIOS::execute_lle() {
    LOGI("LLE boot: Starting real BIOS execution...");

    if (!g_ee_native || !g_iop_native) {
        LOGE("LLE boot: EE or IOP core not set!");
        return;
    }

    uint32_t* ee_ram = (uint32_t*)g_ee_native->get_ram();

    // 1. Setup initial COP0 state (as if BIOS just reset)
    g_ee_native->state.cop0[12] = 0x00000004; // Status: BEV=1, ERL=1
    g_ee_native->state.cop0[15] = 0x00002E20; // PRId

    // 2. Setup stack and pointers
    g_ee_native->state.gpr_lo[SP] = 0x01FFFFF0;
    g_ee_native->state.gpr_lo[GP] = 0;

    // 3. IOP starts at BIOS ROM entry
    g_iop_native->state.pc = 0xBFC00000;
    g_iop_native->state.halted = false;

    // 4. EE starts at BIOS ROM entry (mapped to KSEG1)
    g_ee_native->state.pc = 0xBFC00000;

    // 5. Initialize SIF for EE↔IOP communication
    ee_ram[0x0000F200 >> 2] = 0;

    LOGI("LLE boot: Both EE and IOP starting from BIOS ROM entry.");
    g_bios_initialized = true;
}

// ─── BIOS function interception ──────────────────────────────────────────────
bool PS2_BIOS::intercept_bios_call(uint32_t pc, uint32_t& new_pc) {
    // Check if PC is in BIOS function area (KSEG1: 0xBFC00000-0xBFC03FFF)
    // or physical BIOS ROM area (0x00000000-0x00003FFF)
    uint32_t bios_offset = 0;
    if (pc >= 0xBFC00000u && pc < 0xBFC04000u) {
        bios_offset = pc - 0xBFC00000u;
    } else if (pc < 0x00004000u) {
        bios_offset = pc;
    } else {
        return false;
    }

    switch (bios_offset) {
    case BIOS_FN_SetGsCrt:
        LOGI("BIOS HLE: SetGsCrt -> NOP");
        new_pc = pc + 8; // Skip delay slot
        return true;
    case BIOS_FN_GsResetGraph:
        LOGI("BIOS HLE: GsResetGraph -> NOP");
        new_pc = pc + 8;
        return true;
    case BIOS_FN_Exit:
        LOGI("BIOS HLE: Exit -> halt");
        g_ee_native->state.halted = true;
        return true;
    case BIOS_FN_LoadExecPS2:
        LOGI("BIOS HLE: LoadExecPS2");
        new_pc = g_ee_native->state.gpr_lo[4]; // a0 = entry point
        return true;
    case BIOS_FN_Sync:
        LOGI("BIOS HLE: Sync -> NOP");
        new_pc = pc + 8;
        return true;
    case BIOS_FN_SifLoadModule:
        LOGI("BIOS HLE: SifLoadModule -> NOP (return 1)");
        g_ee_native->state.gpr_lo[2] = 1; // return success
        new_pc = pc + 8;
        return true;
    case BIOS_FN_SifExecModuleBuffer:
        LOGI("BIOS HLE: SifExecModuleBuffer -> NOP (return 1)");
        g_ee_native->state.gpr_lo[2] = 1;
        new_pc = pc + 8;
        return true;
    default:
        if (bios_offset < 0x4000) {
            LOGI("BIOS HLE: Unknown function at offset 0x%04X (PC=0x%08X) -> NOP", bios_offset, pc);
            new_pc = pc + 8;
            return true;
        }
        break;
    }
    return false;
}
