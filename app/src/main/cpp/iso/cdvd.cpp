// iso/cdvd.cpp
// CDVD hardware register emulation — needed by IOP CDVD driver.
// Full implementation in Phase 2; Phase 1 uses iso_loader for direct ELF load.
#include <cstdint>
#include <android/log.h>

#define TAG "CDVD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

// CDVD register base: 0x1F402000 (IOP bus)
static constexpr uint32_t CDVD_BASE = 0x1F402000u;

// CDVD N-command / S-command registers (simplified)
static uint8_t s_ncmd    = 0;
static uint8_t s_ndata   = 0;
static uint8_t s_nready  = 0x40; // NREADY bit: 1 = ready
static uint8_t s_istat   = 0;

uint8_t cdvd_read8(uint32_t addr) {
    switch (addr & 0xFF) {
        case 0x04: return s_nready; // N status
        case 0x05: return s_ndata;  // N result data
        case 0x0A: return s_istat;  // interrupt status
        default:
            LOGW("CDVD read8 %08X unimplemented", addr);
            return 0xFF;
    }
}

void cdvd_write8(uint32_t addr, uint8_t val) {
    switch (addr & 0xFF) {
        case 0x04: s_ncmd  = val; LOGI("CDVD N-CMD %02X", val); break;
        case 0x05: s_ndata = val; break;
        case 0x07: s_istat &= ~val; break; // write 1 clears
        default:
            LOGW("CDVD write8 %08X = %02X unimplemented", addr, val);
            break;
    }
}

// Called by IOP bus dispatcher
uint8_t cdvd_bus_read8 (uint32_t addr) { return cdvd_read8(addr);  }
void    cdvd_bus_write8(uint32_t addr, uint8_t val) { cdvd_write8(addr, val); }
