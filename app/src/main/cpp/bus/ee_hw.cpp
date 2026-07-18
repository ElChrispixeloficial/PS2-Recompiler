#include "ee_hw.h"
#include <android/log.h>
#include <cstring>

#define TAG "PS2-HW"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

// Timer base addresses
static constexpr uint32_t TIMER_BASE     = 0x10001000;
static constexpr uint32_t TIMER_SPACING  = 0x100;

// Timer register offsets
static constexpr uint32_t TMR_COUNT_OFF  = 0x00;
static constexpr uint32_t TMR_MODE_OFF   = 0x10;
static constexpr uint32_t TMR_TARGET_OFF = 0x20;

// Timer 5 has different base: 0x10001800
static constexpr uint32_t TIMER5_BASE    = 0x10001800;

// INTC registers
static constexpr uint32_t INTC_STAT_ADDR = 0x1000F000;
static constexpr uint32_t INTC_MASK_ADDR = 0x1000F010;

// MECNIFO register
static constexpr uint32_t MECNIFO_ADDR   = 0x1000F430;

// Mode register bit positions
static constexpr uint32_t TMR_TE    = (1u << 0);   // Timer Enable
static constexpr uint32_t TMR_TM0   = (1u << 1);   // Timer Mode
static constexpr uint32_t TMR_CMP   = (1u << 2);   // Compare mode
static constexpr uint32_t TMR_IE    = (1u << 8);   // Interrupt Enable
static constexpr uint32_t TMR_OVFL  = (1u << 14);  // Internal overflow flag (bit 14 unused in hardware mode reg)

// Clock dividers for CKS bits 10-11
// 00 = bus clk / 16
// 01 = bus clk / 256
// 10 = bus clk (unclocked)
// 11 = external clock
static int get_clock_divider(uint32_t mode) {
    uint32_t cks = (mode >> 10) & 3;
    switch (cks) {
        case 0: return 16;
        case 1: return 256;
        case 2: return 1;
        case 3: return 1; // external, treat as 1
        default: return 16;
    }
}

// Resolve timer index from address
// Timer 0-3: 0x10001000, 0x10001100, 0x10001200, 0x10001300
// Timer 4:   0x10001400
// Timer 5:   0x10001800
static int resolve_timer_index(uint32_t addr, uint32_t& offset) {
    if (addr >= TIMER_BASE && addr < TIMER_BASE + 4 * TIMER_SPACING) {
        uint32_t idx = (addr - TIMER_BASE) / TIMER_SPACING;
        offset = (addr - TIMER_BASE) % TIMER_SPACING;
        if (idx < 4) return idx;
    }
    // Timer 4
    if (addr >= 0x10001400 && addr < 0x10001400 + TIMER_SPACING) {
        offset = addr - 0x10001400;
        return 4;
    }
    // Timer 5
    if (addr >= TIMER5_BASE && addr < TIMER5_BASE + TIMER_SPACING) {
        offset = addr - TIMER5_BASE;
        return 5;
    }
    return -1;
}

// Update interrupt status based on compare/overflow
static void timer_check_interrupt(EE_HW& hw, PS2_Timer& t, int timer_idx) {
    if ((t.mode & TMR_OVFL) && (t.mode & TMR_IE)) {
        hw.i_stat |= (1u << (16 + timer_idx)); // Timers map to bits 16-21 of I_STAT
    }
}

static void timer_tick(EE_HW& hw, PS2_Timer& t, int timer_idx, int cycles) {
    if (!(t.mode & TMR_TE)) return;

    int divider = get_clock_divider(t.mode);
    uint32_t increment = (uint32_t)(cycles / divider);
    if (increment == 0 && cycles > 0) increment = 1;

    bool had_overflow = false;

    if (t.mode & TMR_TM0) {
        // Target mode: count up to target, then zero-return or stop
        uint64_t new_count = (uint64_t)t.count + increment;
        if (t.target > 0 && new_count >= t.target) {
            t.count = 0;
            had_overflow = true;
        } else {
            t.count = (uint32_t)new_count;
        }
    } else {
        // Free-run mode: count wraps at 0xFFFFFFFF
        uint64_t new_count = (uint64_t)t.count + increment;
        if (new_count > 0xFFFFFFFF) {
            had_overflow = true;
            t.count = (uint32_t)(new_count & 0xFFFFFFFF);
        } else {
            t.count = (uint32_t)new_count;
        }
    }

    if (had_overflow) {
        t.mode |= TMR_OVFL;
        timer_check_interrupt(hw, t, timer_idx);
    }
}

void ee_hw_init(EE_HW& hw) {
    memset(&hw, 0, sizeof(EE_HW));
    LOGI("EE_HW initialized");
}

uint32_t ee_hw_read32(EE_HW& hw, uint32_t phys_addr) {
    if (phys_addr == INTC_STAT_ADDR) {
        return hw.i_stat;
    }
    if (phys_addr == INTC_MASK_ADDR) {
        return hw.i_mask;
    }
    if (phys_addr == MECNIFO_ADDR) {
        return 0;
    }

    uint32_t offset;
    int idx = resolve_timer_index(phys_addr, offset);
    if (idx >= 0 && idx < PS2_TIMER_COUNT) {
        PS2_Timer& t = hw.timers[idx];
        if (offset == TMR_COUNT_OFF)  return t.count;
        if (offset == TMR_MODE_OFF)   return t.mode;
        if (offset == TMR_TARGET_OFF) return t.target;
        return 0;
    }

    LOGW("EE_HW read32 unhandled addr=0x%08X", phys_addr);
    return 0;
}

void ee_hw_write32(EE_HW& hw, uint32_t phys_addr, uint32_t val) {
    if (phys_addr == INTC_STAT_ADDR) {
        // Write-1-to-clear
        hw.i_stat &= ~val;
        return;
    }
    if (phys_addr == INTC_MASK_ADDR) {
        hw.i_mask = val;
        return;
    }
    if (phys_addr == MECNIFO_ADDR) {
        return; // Read-only, ignore writes
    }

    uint32_t offset;
    int idx = resolve_timer_index(phys_addr, offset);
    if (idx >= 0 && idx < PS2_TIMER_COUNT) {
        PS2_Timer& t = hw.timers[idx];
        if (offset == TMR_COUNT_OFF) {
            t.count = 0; // Writing count resets it
        } else if (offset == TMR_MODE_OFF) {
            uint32_t old_mode = t.mode;
            t.mode = (val & ~TMR_OVFL) | (old_mode & TMR_OVFL); // preserve internal overflow bit
            // If timer was just enabled, reset count
            if ((val & TMR_TE) && !(old_mode & TMR_TE)) {
                t.count = 0;
                LOGI("Timer %d enabled, count=0", idx);
            }
        } else if (offset == TMR_TARGET_OFF) {
            t.target = val;
        }
        return;
    }

    LOGW("EE_HW write32 unhandled addr=0x%08X val=0x%08X", phys_addr, val);
}

bool ee_hw_tick(EE_HW& hw, int cycles) {
    for (int i = 0; i < PS2_TIMER_COUNT; i++) {
        timer_tick(hw, hw.timers[i], i, cycles);
    }
    return (hw.i_stat & hw.i_mask) != 0;
}
