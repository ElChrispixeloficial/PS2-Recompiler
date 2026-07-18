// bus/cdvd.cpp — Complete CDVD hardware register emulation (IOP side)
// Registers at 0x1F402000.  Handles N-Command (data) and S-Command (control)
// paths, disc status, sector reads, and version information.

#include "cdvd.h"
#include <android/log.h>
#include <cstring>

#define TAG "CDVD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

static constexpr uint32_t CDVD_BASE = 0x1F402000u;

// N-Command status bits
static constexpr uint8_t NSTAT_BUSY  = 0x80;
static constexpr uint8_t NSTAT_READY = 0x40;
static constexpr uint8_t NSTAT_ERROR = 0x01;

// S-Command status bits
static constexpr uint8_t SSTAT_BUSY  = 0x80;
static constexpr uint8_t SSTAT_READY = 0x40;

// I_STAT bits
static constexpr uint8_t ISTAT_SM    = 0x02; // S-Command complete
static constexpr uint8_t ISTAT_NMODE = 0x04; // N-Command complete

// CDVD N-Command opcodes
static constexpr uint8_t N_CMD_READN      = 0x01;
static constexpr uint8_t N_CMD_PLAY       = 0x02;
static constexpr uint8_t N_CMD_SEEK       = 0x09;
static constexpr uint8_t N_CMD_GETTOC     = 0x06;
static constexpr uint8_t N_CMD_GETSTAT    = 0x0E;
static constexpr uint8_t N_CMD_GETARGN    = 0x0F;
static constexpr uint8_t N_CMD_GETQ       = 0x11;
static constexpr uint8_t N_CMD_READDVD    = 0x14;
static constexpr uint8_t N_CMD_READDVDDUAL= 0x19;

// CDVD S-Command opcodes
static constexpr uint8_t S_CMD_READNOP    = 0x20;
static constexpr uint8_t S_CMD_TRAYREQ    = 0x22;
static constexpr uint8_t S_CMD_READSUBCH  = 0x40;

// Global state (singleton, same pattern as g_gs_ptr)
static CDVD_State* g_cdvd_ptr = nullptr;

void cdvd_init(CDVD_State& state, bool has_disc) {
    memset(&state, 0, sizeof(CDVD_State));
    state.version      = 0x20;
    state.disc_present = has_disc;
    state.read_progress = -1;
    state.n_status     = has_disc ? NSTAT_READY : 0;
    state.s_status     = SSTAT_READY;
    state.n_result_len = 0;
    state.n_result_pos = 0;
    state.s_result_len = 0;
    state.s_result_pos = 0;
    state.n_arg_len    = 0;
    state.i_stat       = 0;
    state.i_mask       = 0;
    g_cdvd_ptr = &state;
    LOGI("CDVD initialized, disc=%s", has_disc ? "present" : "absent");
}

// ─── N-Command handling ────────────────────────────────────────────────────

static void push_n_result(CDVD_State& s, uint8_t val) {
    if (s.n_result_len < 16) {
        s.n_result[s.n_result_len++] = val;
    }
}

static void execute_n_command(CDVD_State& s, uint8_t cmd) {
    s.n_arg_len = 0;
    s.n_result_len = 0;
    s.n_result_pos = 0;

    switch (cmd) {
        case N_CMD_READN:
        case N_CMD_READDVD:
        case N_CMD_READDVDDUAL:
            LOGI("N-Command CdlReadN/CdlReadDVD sector=%u", s.sector);
            if (s.disc_present) {
                s.read_progress = 2048;
                s.n_status = NSTAT_READY;
            } else {
                s.n_status = NSTAT_ERROR;
            }
            break;

        case N_CMD_PLAY:
            LOGI("N-Command CdlPlay");
            s.n_status = s.disc_present ? NSTAT_READY : NSTAT_ERROR;
            break;

        case N_CMD_SEEK:
            LOGI("N-Command CdlSeek sector=%u", s.sector);
            s.n_status = s.disc_present ? NSTAT_READY : NSTAT_ERROR;
            break;

        case N_CMD_GETTOC:
            LOGI("N-Command CdlGetToc");
            push_n_result(s, 0x00); // TOC not available
            push_n_result(s, 0x00);
            push_n_result(s, 0x00);
            push_n_result(s, 0x00);
            s.n_status = NSTAT_READY;
            break;

        case N_CMD_GETSTAT: {
            LOGI("N-Command CdlGetStat");
            uint8_t stat = 0;
            if (s.disc_present) {
                stat |= 0x80; // bit 7 = disc present
                stat |= 0x02; // bits 0-2 = ready state
            }
            stat |= 0x04; // media type: CD-ROM
            push_n_result(s, stat);
            s.n_status = NSTAT_READY;
            break;
        }

        case N_CMD_GETARGN:
            LOGI("N-Command CdlGetArgN");
            push_n_result(s, (uint8_t)s.n_arg_len);
            s.n_status = NSTAT_READY;
            break;

        case N_CMD_GETQ: {
            LOGI("N-Command CdlGetQ");
            // Dummy Q subchannel: all zeros with track/indices at 1
            for (int i = 0; i < 16; i++) {
                push_n_result(s, 0x00);
            }
            // Q channel format: ctrl(1) + addr(1) + track(1) + index(1) + ...
            if (s.n_result_len >= 4) {
                s.n_result[0] = 0x14; // ctrl: audio track, pre-emphasis
                s.n_result[1] = 0x01; // adr mode 1
                s.n_result[2] = 0x01; // track number
                s.n_result[3] = 0x01; // index number
            }
            s.n_status = NSTAT_READY;
            break;
        }

        default:
            LOGW("N-Command unhandled opcode 0x%02X", cmd);
            s.n_status = NSTAT_READY;
            break;
    }

    // Raise N-Command complete interrupt if mask enabled
    s.i_stat |= ISTAT_NMODE;
}

// ─── S-Command handling ────────────────────────────────────────────────────

static void push_s_result(CDVD_State& s, uint8_t val) {
    if (s.s_result_len < 16) {
        s.s_result[s.s_result_len++] = val;
    }
}

static void execute_s_command(CDVD_State& s, uint8_t cmd) {
    s.s_result_len = 0;
    s.s_result_pos = 0;

    switch (cmd) {
        case S_CMD_READNOP:
            LOGI("S-Command SCECdReadNop");
            push_s_result(s, s.status);
            break;

        case S_CMD_TRAYREQ:
            LOGI("S-Command SCECdTrayRequest val=0x%02X", s.s_datain);
            if (s.s_datain == 0x00) {
                // Open tray
                s.disc_present = false;
                s.n_status = 0;
                LOGI("Tray opened, disc removed");
            } else if (s.s_datain == 0x01) {
                // Close tray
                s.disc_present = true;
                s.n_status = NSTAT_READY;
                LOGI("Tray closed, disc inserted");
            }
            push_s_result(s, 0x00); // success
            break;

        case S_CMD_READSUBCH:
            LOGI("S-Command SCECdReadSubchannel");
            // Dummy subchannel data (24 bytes of zeros)
            for (int i = 0; i < 24; i++) {
                push_s_result(s, 0x00);
            }
            break;

        default:
            LOGW("S-Command unhandled opcode 0x%02X", cmd);
            push_s_result(s, 0xFF); // error
            break;
    }

    s.s_status |= SSTAT_READY;
    s.s_status &= ~SSTAT_BUSY;
    s.i_stat |= ISTAT_SM;
}

// ─── Bus access functions ──────────────────────────────────────────────────

uint8_t cdvd_bus_read8(uint32_t addr) {
    CDVD_State& s = *g_cdvd_ptr;
    uint32_t reg = addr & 0xFF;

    switch (reg) {
        case 0x00: // CDVD_N_DATAIN — read back last N-cmd data in
            return s.n_datain;

        case 0x04: // CDVD_N_STATUS
            return s.n_status;

        case 0x05: { // CDVD_N_DATAOUT — dequeue from N-result buffer
            uint8_t val = 0xFF;
            if (s.n_result_pos < s.n_result_len) {
                val = s.n_result[s.n_result_pos++];
                if (s.n_result_pos >= s.n_result_len) {
                    s.n_result_pos = 0;
                    s.n_result_len = 0;
                }
            }
            return val;
        }

        case 0x07: // CDVD_I_STAT
            return s.i_stat;

        case 0x08: // CDVD_I_MASK
            return s.i_mask;

        case 0x0A: // CDVD_STATUS — drive status
            return s.status;

        case 0x0F: // CDVD_S_DATAIN — read back last S-cmd data in
            return s.s_datain;

        case 0x10: { // CDVD_S_DATAOUT — dequeue from S-result buffer
            uint8_t val = 0xFF;
            if (s.s_result_pos < s.s_result_len) {
                val = s.s_result[s.s_result_pos++];
                if (s.s_result_pos >= s.s_result_len) {
                    s.s_result_pos = 0;
                    s.s_result_len = 0;
                }
            }
            return val;
        }

        case 0x11: // CDVD_S_STATUS
            return s.s_status;

        case 0x13: // CDVD_S_COMMAND
            return s.s_cmd;

        case 0x14: // CDVD_CDXA_VOL_L
            return s.cdxa_vol_l;

        case 0x15: // CDVD_CDXA_VOL_R
            return s.cdxa_vol_r;

        case 0x16: // CDVD_AUTO_ADJUST
            return s.auto_adjust;

        case 0x17: // CDVD_VER
            return s.version;

        case 0x18: // CDVD_IF — BIOS version info
            return s.bios_version;

        default:
            LOGW("CDVD read8 0x%08X unhandled reg=0x%02X", addr, reg);
            return 0xFF;
    }
}

void cdvd_bus_write8(uint32_t addr, uint8_t val) {
    CDVD_State& s = *g_cdvd_ptr;
    uint32_t reg = addr & 0xFF;

    switch (reg) {
        case 0x00: // CDVD_N_DATAIN
            s.n_datain = val;
            if (s.n_arg_len < 16) {
                s.n_arg_buf[s.n_arg_len++] = val;
            }
            break;

        case 0x04: // CDVD_N_CMD — writing here triggers the N-command
            LOGI("CDVD N-CMD = 0x%02X (arg count=%d)", val, s.n_arg_len);
            execute_n_command(s, val);
            break;

        case 0x05: // CDVD_N_DATAOUT (write ignored)
            break;

        case 0x07: // CDVD_I_STAT — write-1-to-clear
            s.i_stat &= ~val;
            break;

        case 0x08: // CDVD_I_MASK
            s.i_mask = val;
            break;

        case 0x0A: // CDVD_STATUS (write ignored)
            break;

        case 0x0F: // CDVD_S_DATAIN
            s.s_datain = val;
            break;

        case 0x10: // CDVD_S_DATAOUT (write ignored)
            break;

        case 0x11: // CDVD_S_STATUS (write ignored)
            break;

        case 0x13: // CDVD_S_COMMAND — writing here triggers the S-command
            LOGI("CDVD S-CMD = 0x%02X data=0x%02X", val, s.s_datain);
            s.s_status |= SSTAT_BUSY;
            execute_s_command(s, val);
            break;

        case 0x14: // CDVD_CDXA_VOL_L
            s.cdxa_vol_l = val;
            break;

        case 0x15: // CDVD_CDXA_VOL_R
            s.cdxa_vol_r = val;
            break;

        case 0x16: // CDVD_AUTO_ADJUST
            s.auto_adjust = val;
            break;

        case 0x17: // CDVD_VER (read-only, ignore writes)
            break;

        case 0x18: // CDVD_IF (read-only, ignore writes)
            break;

        default:
            LOGW("CDVD write8 0x%08X = 0x%02X unhandled reg=0x%02X", addr, val, reg);
            break;
    }
}

uint32_t cdvd_bus_read32(uint32_t addr) {
    // 32-bit reads from CDVD registers return the 8-bit value zero-extended
    // in the low byte (hardware behavior for most IOP registers)
    uint32_t reg = addr & 0xFF;
    uint32_t val = 0;

    // For N_DATAOUT and S_DATAOUT, read as full 32-bit from result buffers
    if (reg == 0x05) {
        CDVD_State& s = *g_cdvd_ptr;
        if (s.n_result_pos < s.n_result_len) {
            val = s.n_result[s.n_result_pos++];
            if (s.n_result_pos >= s.n_result_len) {
                s.n_result_pos = 0;
                s.n_result_len = 0;
            }
        }
        return val;
    }
    if (reg == 0x10) {
        CDVD_State& s = *g_cdvd_ptr;
        if (s.s_result_pos < s.s_result_len) {
            val = s.s_result[s.s_result_pos++];
            if (s.s_result_pos >= s.s_result_len) {
                s.s_result_pos = 0;
                s.s_result_len = 0;
            }
        }
        return val;
    }

    return cdvd_bus_read8(addr);
}

void cdvd_bus_write32(uint32_t addr, uint32_t val) {
    // All CDVD registers are 8-bit; 32-bit writes use only the low byte
    cdvd_bus_write8(addr, (uint8_t)(val & 0xFF));
}

// ─── Tick — advance CDVD state ────────────────────────────────────────────

void cdvd_tick(CDVD_State& /*state*/, int cycles) {
    if (!g_cdvd_ptr) return;
    CDVD_State& s = *g_cdvd_ptr;

    // Simulate sector read completion
    if (s.read_progress > 0) {
        s.read_progress -= cycles;
        if (s.read_progress <= 0) {
            s.read_progress = 0;
            s.sector++;
            LOGI("CDVD sector read complete, next sector=%u", s.sector);
            s.read_progress = -1;
        }
    }
}
