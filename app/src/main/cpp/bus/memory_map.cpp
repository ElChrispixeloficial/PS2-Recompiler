// memory_map.cpp — Central EE bus dispatcher for hardware register I/O
// Routes reads/writes to the appropriate subsystem: timers, INTC, SIF, DMA, etc.

#include "ee_hw.h"
#include "sif_bus.h"
#include "dma_controller.h"
#include "../gs/gs_core.h"
#include "../ee/ee_core.h"
#include <android/log.h>

#define LOG_TAG "PS2-BUS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

EE_HW    g_ee_hw;
SIF_Bus  g_sif_bus;

extern DMA_Controller* g_dma_ptr;
extern uint32_t g_iop_intc_stat;

uint32_t hw_read32(EE_Core& ee, uint32_t addr) {
    // EE Timers — 0x10001000..0x100018FF
    if (addr >= 0x10001000 && addr <= 0x100018FF) {
        return ee_hw_read32(g_ee_hw, addr);
    }

    // DMA Controller — 0x10008000..0x1000EFF0
    if (addr >= 0x10008000 && addr <= 0x1000EFF0) {
        if (g_dma_ptr) return g_dma_ptr->read_reg(addr);
        return 0;
    }

    // INTC — 0x1000F000..0x1000F020 (dispatched through ee_hw)
    if (addr >= 0x1000F000 && addr <= 0x1000F020) {
        return ee_hw_read32(g_ee_hw, addr);
    }

    // SIF — 0x1000F200..0x1000F2FF
    if (addr >= 0x1000F200 && addr <= 0x1000F2FF) {
        return sif_read32(g_sif_bus, addr);
    }

    // MECNIFO — 0x1000F430
    if (addr == 0x1000F430) {
        return ee_hw_read32(g_ee_hw, addr);
    }

    // Stub MBOX / SIF registers — 0x1000F520..0x1000F580
    if (addr >= 0x1000F520 && addr <= 0x1000F580) {
        return 0;
    }

    LOGW("hw_read32: unhandled addr 0x%08X", addr);
    return 0;
}

void hw_write32(EE_Core& ee, uint32_t addr, uint32_t val) {
    // EE Timers — 0x10001000..0x100018FF
    if (addr >= 0x10001000 && addr <= 0x100018FF) {
        ee_hw_write32(g_ee_hw, addr, val);
        return;
    }

    // DMA Controller — 0x10008000..0x1000EFF0
    if (addr >= 0x10008000 && addr <= 0x1000EFF0) {
        if (g_dma_ptr) g_dma_ptr->write_reg(addr, val);
        return;
    }

    // INTC — 0x1000F000..0x1000F020
    if (addr >= 0x1000F000 && addr <= 0x1000F020) {
        ee_hw_write32(g_ee_hw, addr, val);
        return;
    }

    // SIF — 0x1000F200..0x1000F2FF
    if (addr >= 0x1000F200 && addr <= 0x1000F2FF) {
        sif_write32(g_sif_bus, addr, val);
        return;
    }

    // MECNIFO — 0x1000F430
    if (addr == 0x1000F430) {
        ee_hw_write32(g_ee_hw, addr, val);
        return;
    }

    // Stub MBOX / SIF registers — 0x1000F520..0x1000F580
    if (addr >= 0x1000F520 && addr <= 0x1000F580) {
        return;
    }

    LOGW("hw_write32: unhandled addr 0x%08X val=0x%08X", addr, val);
}

bool hw_tick(int cycles) {
    bool irq = ee_hw_tick(g_ee_hw, cycles);
    
    // ─── IOP VBlank interrupt: fire VBlank bit (bit 4 = 0x10) in IOP INTC_STAT
    //     periodically so IOP modules polling for VBlank can progress.
    //     EE runs at 4915200/60 ≈ 81920 cycles per frame, IOP at 1/8 = 10240
    static int iop_vblank_divider = 0;
    iop_vblank_divider += cycles;
    if (iop_vblank_divider >= 10240) {
        iop_vblank_divider = 0;
        // Directly set the VBlank bit in the IOP INTC_STAT variable
        extern uint32_t g_iop_intc_stat;
        g_iop_intc_stat |= 0x10; // IOP VBlank interrupt (bit 4)
    }

    return irq;
}
