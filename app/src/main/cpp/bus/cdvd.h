#pragma once
#include <cstdint>
#include <cstddef>

// CDVD hardware register emulation — IOP-side registers at 0x1F402000.
// Handles N-commands (data path), S-commands (control path),
// disc status, sector reads, and drive version reporting.

struct CDVD_State {
    // N-Command path (offsets 0x00-0x06)
    uint8_t n_cmd;        // 0x00 — N-Command data input (write triggers command)
    uint8_t n_datain;     // 0x01 — N-Command data input (secondary)
    uint8_t n_status;     // 0x04 — N-Command status (bit 6 = ready)
    uint8_t n_dataout;    // 0x05 — N-Command result data (read to dequeue)

    // Interrupts (offsets 0x07-0x08)
    uint8_t i_stat;       // 0x07 — Interrupt status (write-1-to-clear)
    uint8_t i_mask;       // 0x08 — Interrupt mask

    // S-Command path (offsets 0x0F-0x13)
    uint8_t s_cmd;        // 0x13 — S-Command register (write triggers command)
    uint8_t s_datain;     // 0x0F — S-Command data in
    uint8_t s_dataout;    // 0x10 — S-Command data out
    uint8_t s_status;     // 0x11 — S-Command status (bit 7=busy, bit 6=ready)

    // Drive status (offset 0x0A)
    uint8_t status;       // Drive status byte

    // CDXA volume (offsets 0x14-0x15)
    uint8_t cdxa_vol_l;   // 0x14 — CDXA volume left
    uint8_t cdxa_vol_r;   // 0x15 — CDXA volume right

    // Auto-adjust (offset 0x16)
    uint8_t auto_adjust;  // 0x16

    // Version (offset 0x17)
    uint8_t version;      // 0x17 — returns 0x20 for PS2 compatibility

    // BIOS version info (offset 0x18)
    uint8_t bios_version; // 0x18

    // Sector tracking
    uint32_t sector;       // Current read sector LBA
    bool     disc_present; // Is a disc inserted?
    int32_t  read_progress;// -1 = not reading, 0 = read complete

    // N-Command result buffer (simple FIFO, up to 16 bytes)
    uint8_t  n_result[16];
    int      n_result_len;
    int      n_result_pos;

    // S-Command result buffer
    uint8_t  s_result[16];
    int      s_result_len;
    int      s_result_pos;

    // Argument tracking for N-Commands
    uint8_t  n_arg_buf[16];
    int      n_arg_len;
};

void     cdvd_init(CDVD_State& state, bool has_disc);
uint8_t  cdvd_bus_read8(uint32_t addr);
void     cdvd_bus_write8(uint32_t addr, uint8_t val);
uint32_t cdvd_bus_read32(uint32_t addr);
void     cdvd_bus_write32(uint32_t addr, uint32_t val);
void     cdvd_tick(CDVD_State& state, int cycles);
