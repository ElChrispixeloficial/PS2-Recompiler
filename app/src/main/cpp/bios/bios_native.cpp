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

    // 1. Clear kernel area (first 64KB) — MUST be done first
    memset(g_ee_native->get_ram(), 0, 0x00010000);

    // 2. Setup PS2 kernel data area (0x00-0xFF)
    // Exception handler vectors (copied from BIOS ROM by real boot)
    ee_ram[0x00 >> 2] = 0x08000060;  // j 0x80000180 (General exception)
    ee_ram[0x04 >> 2] = 0x00000000;  // nop
    ee_ram[0x08 >> 2] = 0x08000080;  // j 0x80000200 (Interrupt handler)
    ee_ram[0x0C >> 2] = 0x00000000;  // nop

    // Kernel data area — game reads from here (s1=0x10, polls addr 0x14)
    ee_ram[0x10 >> 2] = 0x00000001;  // Kernel initialized flag
    ee_ram[0x14 >> 2] = 0x00000001;  // "IOP ready" / kernel status
    ee_ram[0x18 >> 2] = 0x00000001;  // Module count / boot stage
    ee_ram[0x1C >> 2] = 0x00000001;  // Additional init flag

    // Thread control block area (typical values)
    ee_ram[0x20 >> 2] = 0x00000001;  // TCBS count
    ee_ram[0x24 >> 2] = 0x00100000;  // Kernel heap start

    // 3. Initialize SIF mailbox to wake EE from kernel loop
    ee_ram[0x0000F200 >> 2] = 0x00010000;
    ee_ram[0x0000F240 >> 2] = 0x00000001; // SIF.SIF_RPCInitialize
    ee_ram[0x0000F260 >> 2] = 0x00000001; // SIF control flag

    // 4. Setup COP0 registers
    // Status: BEV=1, ERL=1 (initial state after reset)
    g_ee_native->state.cop0[12] = 0x00000004; // Status: BEV=1
    // PRId: R5900 revision (COP0 reg15 — note: EBASE is separate via Select register)
    g_ee_native->state.cop0[15] = 0x00002E20;

    // 5. Setup TLB (minimal: map KSEG0/KSEG1 as unmapped)
    // These are handled by the address masking in ee_memory.h

    // 6. Setup Stack Pointer and Frame Pointer
    g_ee_native->state.gpr_lo[SP] = 0x01FFFFF0;
    g_ee_native->state.gpr_lo[GP] = 0; // GP will be set by game

    // 7. Clear return address
    g_ee_native->state.gpr_lo[RA] = 0;

    // 8. Enable interrupts
    g_ee_native->state.cop0[12] = 0x00010401; // Status: IE=1, EXL=0, KSU=0

    // 9. Jump to game entry point
    g_ee_native->state.pc = g_game_entry_point;
    LOGI("HLE boot: EE jumping to 0x%08X", g_game_entry_point);

    // 10. Start IOP from BIOS ROM entry — it will run BIOS HLE stubs
    //    (SetSifInit, GsInitGraph, etc.) and halt via Exit function.
    //    CRITICAL: IOP must be alive for SIF communication with the game.
    if (g_iop_native) {
        g_iop_native->state.pc = 0xBFC00000;
        g_iop_native->state.halted = false;
        // Enable IOP interrupts: IE=1, IM[2]=1 (INTC), IM[3]=1 (DMAC)
        g_iop_native->state.cop0[12] = 0x0000080C; // IE=1, IM2=1(INTC), IM3=1(DMAC)
        g_iop_native->state.cop0[13] = 0; // Cause
        // Enable IOP INTC for SIF (bit 7) so IOP can process SIF RPCs
        extern uint32_t g_iop_intc_mask;
        g_iop_intc_mask = (1u << 7); // Enable SIF interrupt on IOP
        LOGI("HLE boot: IOP started at 0xBFC00000 (BIOS HLE active, interrupts enabled)");

        // Write IOP exception handler at offset 0x180 in IOP RAM.
        // When IOP takes an interrupt (PC jumps to 0x80000180 = phys 0x180),
        // this handler processes SIF0 interrupts and signals completion to EE.
        //
        // Assembled MIPS R3000A code:
        //   addiu $sp, $sp, -8
        //   sw    $t0, 0($sp)
        //   sw    $t1, 4($sp)
        //   lui   $t0, 0x1F80          ; INTC base
        //   lw    $t1, 0x1070($t0)     ; $t1 = INTC_STAT
        //   andi  $t2, $t1, 0x80       ; $t2 = SIF bit (bit 7)
        //   beq   $t2, $zero, skip     ; if no SIF, skip
        //   nop
        //   sw    $t2, 0x1070($t0)     ; clear SIF from INTC_STAT (W1C)
        //   lui   $t0, 0x1000          ; SIF base
        //   ori   $t0, $t0, 0xF2C0     ; $t0 = &SMFLG
        //   lw    $t1, 0($t0)
        //   ori   $t1, $t1, 1
        //   sw    $t1, 0($t0)          ; SMFLG |= 1
        //   lui   $t0, 0x1000
        //   ori   $t0, $t0, 0xF260     ; $t0 = &MSFLG
        //   lw    $t1, 0($t0)
        //   ori   $t1, $t1, 1
        //   sw    $t1, 0($t0)          ; MSFLG |= 1
        // skip:
        //   lw    $t0, 0($sp)
        //   lw    $t1, 4($sp)
        //   addiu $sp, $sp, 8
        //   eret
        //   nop
        uint32_t* iop = (uint32_t*)(g_iop_native->get_ram() + 0x180);
        int p = 0;
        iop[p++] = 0x27BDFFF8; // addiu $sp, $sp, -8
        iop[p++] = 0xAF880000; // sw $t0, 0($sp)
        iop[p++] = 0xAF890004; // sw $t1, 4($sp)
        iop[p++] = 0x3C081F80; // lui $t0, 0x1F80
        iop[p++] = 0x8D091070; // lw $t1, 0x1070($t0)  — INTC_STAT
        iop[p++] = 0x312A0080; // andi $t2, $t1, 0x80  — SIF bit
        iop[p++] = 0x1140000B; // beq $t2, $zero, skip (+11)
        iop[p++] = 0x00000000; // nop (delay slot)
        iop[p++] = 0xAD0A1070; // sw $t2, 0x1070($t0)  — clear SIF
        iop[p++] = 0x3C081000; // lui $t0, 0x1000
        iop[p++] = 0x3508F2C0; // ori $t0, $t0, 0xF2C0 — &SMFLG
        iop[p++] = 0x8D090000; // lw $t1, 0($t0)
        iop[p++] = 0x35290001; // ori $t1, $t1, 1
        iop[p++] = 0xAD090000; // sw $t1, 0($t0)       — SMFLG |= 1
        iop[p++] = 0x3C081000; // lui $t0, 0x1000
        iop[p++] = 0x3508F260; // ori $t0, $t0, 0xF260 — &MSFLG
        iop[p++] = 0x8D090000; // lw $t1, 0($t0)
        iop[p++] = 0x35290001; // ori $t1, $t1, 1
        iop[p++] = 0xAD090000; // sw $t1, 0($t0)       — MSFLG |= 1
        // skip:
        iop[p++] = 0x8FA80000; // lw $t0, 0($sp)
        iop[p++] = 0x8FA90004; // lw $t1, 4($sp)
        iop[p++] = 0x27BD0008; // addiu $sp, $sp, 8
        iop[p++] = 0x42000010; // eret
        iop[p++] = 0x00000000; // nop
        LOGI("HLE boot: IOP exception handler written at 0x180 (%d words)", p);
    }

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
    // Check if PC is in BIOS address range:
    //   KSEG1 BIOS ROM:  0xBFC00000-0xBFCFFFFF  (mapped from physical 0x1FC00000)
    //   KSEG0 BIOS kernel in RAM: 0x80000000-0x803FFFFF (copied from ROM during real boot)
    //
    // NOTE: We do NOT intercept addresses < 0x00400000 because that range is
    // EE RAM where game code is loaded (e.g., ELF at 0x00100000). Treating
    // game code as BIOS functions would NOP every instruction.
    uint32_t bios_offset = 0;
    if (pc >= 0xBFC00000u && pc < 0xC0000000u) {
        bios_offset = pc - 0xBFC00000u;
    } else if (pc >= 0x80000000u && pc < 0x80400000u) {
        bios_offset = pc - 0x80000000u;
    } else {
        return false;
    }

    switch (bios_offset) {
    // ── R5900 Exception vectors (KSEG0 offsets) ──────────────────────────
    // Only intercept these when EXL bit is set (actual exception in progress).
    // If EXL=0, the game jumped here intentionally — let the JIT execute.
    case 0x000: // TLB Refill handler at 0x80000000
    case 0x170: { // TLB Refill (alternate)
        if (!(g_ee_native->state.cop0[12] & 0x2u)) return false; // EXL not set
        LOGI("BIOS HLE: TLB Refill (offset 0x%03X) -> ERET to EPC=0x%08X", bios_offset, g_ee_native->state.cop0[14]);
        g_ee_native->state.cop0[12] &= ~0x2u; // Clear EXL
        new_pc = g_ee_native->state.cop0[14]; // Return to EPC
        return true;
    }
    case 0x180: { // General Exception handler at 0x80000180
        if (!(g_ee_native->state.cop0[12] & 0x2u)) return false; // EXL not set
        uint32_t cause = g_ee_native->state.cop0[13];
        uint32_t exc_code = (cause >> 2) & 0x1F;
        uint32_t epc = g_ee_native->state.cop0[14];
        LOGI("BIOS HLE: General Exception ExcCode=%u EPC=0x%08X", exc_code, epc);
        switch (exc_code) {
        case 0: { // Interrupt
            uint32_t status = g_ee_native->state.cop0[12];
            uint32_t pending = (cause >> 8) & (status >> 8) & 0xFF;
            cause &= ~(pending << 8);
            g_ee_native->state.cop0[13] = cause;
            g_ee_native->state.cop0[12] = status & ~0x2u;
            new_pc = epc;
            break;
        }
        case 3:  // Address Error (Load) — skip instruction
        case 4:  // Address Error (Store) — skip instruction
        case 5:  // Reserved Instruction — skip instruction
        case 8:  // Syscall — advance past SYSCALL instruction
        case 9:  // Break — advance past BREAK instruction
        case 10: // Watch
            g_ee_native->state.cop0[12] &= ~0x2u;
            new_pc = epc + 4;
            break;
        default: // For any other exception, skip instruction to avoid infinite loop
            LOGI("BIOS HLE: Unhandled ExcCode=%u at EPC=0x%08X, skipping", exc_code, epc);
            g_ee_native->state.cop0[12] &= ~0x2u;
            new_pc = epc + 4;
            break;
        }
        return true;
    }
    case 0x200: { // Interrupt handler at 0x80000200
        if (!(g_ee_native->state.cop0[12] & 0x2u)) return false; // EXL not set
        uint32_t cause = g_ee_native->state.cop0[13];
        uint32_t status = g_ee_native->state.cop0[12];
        uint32_t pending = (cause >> 8) & (status >> 8) & 0xFF;
        cause &= ~(pending << 8);
        g_ee_native->state.cop0[13] = cause;
        g_ee_native->state.cop0[12] = status & ~0x2u;
        new_pc = g_ee_native->state.cop0[14];
        return true;
    }

    // ── Known BIOS library functions (offsets same in ROM and RAM copy) ──
    case BIOS_FN_SetGsCrt:
        LOGI("BIOS HLE: SetGsCrt -> NOP");
        new_pc = pc + 8;
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
        g_ee_native->state.gpr_lo[2] = 1;
        new_pc = pc + 8;
        return true;
    case BIOS_FN_SifExecModuleBuffer:
        LOGI("BIOS HLE: SifExecModuleBuffer -> NOP (return 1)");
        g_ee_native->state.gpr_lo[2] = 1;
        new_pc = pc + 8;
        return true;
    case BIOS_FN_SetVTLBRefillHandler:
        LOGI("BIOS HLE: SetVTLBRefillHandler -> NOP");
        new_pc = pc + 8;
        return true;
    case BIOS_FN_SetVCommonHandler:
        LOGI("BIOS HLE: SetVCommonHandler -> NOP");
        new_pc = pc + 8;
        return true;
    case BIOS_FN_GsInitGraph:
        LOGI("BIOS HLE: GsInitGraph -> NOP");
        new_pc = pc + 8;
        return true;
    case BIOS_FN_AddSifDmaHandler:
        LOGI("BIOS HLE: AddSifDmaHandler -> NOP (return 0)");
        g_ee_native->state.gpr_lo[2] = 0;
        new_pc = pc + 8;
        return true;
    default:
        // NOP out any BIOS function in the kernel ROM/RAM area (up to 256KB)
        if (bios_offset < 0x40000) {
            LOGI("BIOS HLE: Unknown function at offset 0x%04X (PC=0x%08X) -> NOP", bios_offset, pc);
            new_pc = pc + 8;
            return true;
        }
        break;
    }
    return false;
}
