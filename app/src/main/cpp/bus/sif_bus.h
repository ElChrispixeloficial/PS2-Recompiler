#pragma once
#include <cstdint>

// PS2 System Interface (SIF) — EE↔IOP communication bus.
// The SIF provides a mailbox-style register interface plus DMA channels
// (SIF0/SIF1/SIF2) for bulk data transfer between the two processors.

struct SIF_Bus {
    uint32_t mscnt;     // 0x1000F200 — SIF master control
    uint32_t m0cnt;     // 0x1000F210 — SIF EE master 0 control
    uint32_t m1cnt;     // 0x1000F220 — SIF EE master 1 control
    uint32_t sbuscnt;   // 0x1000F230 — SIF slave bus control
    uint32_t sbusdct;   // 0x1000F240 — SIF slave bus data control
    uint32_t msflg;     // 0x1000F260 — SIF master flags (EE side)
    uint32_t smflg;     // 0x1000F2C0 — SIF slave flags (IOP side)
    uint32_t i_stat_mirror; // mirror INTC I_STAT writes from IOP side
};

void     sif_init(SIF_Bus& sif);
uint32_t sif_read32(SIF_Bus& sif, uint32_t addr);
void     sif_write32(SIF_Bus& sif, uint32_t addr, uint32_t val);
void     sif_signal_rpc_complete(SIF_Bus& sif);
