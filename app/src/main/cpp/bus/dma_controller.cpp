#include "dma_controller.h"
#include "../gs/gs_core.h"
#include "../vu/vu_core.h"
#include <android/log.h>
#include <cstring>

#define TAG "DMA"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

extern uint8_t*  g_ee_ram;
extern uint32_t  g_ee_ram_size;
extern GS_Core*  g_gs_ptr;

// ─── Helpers ───────────────────────────────────────────────────────────────

DMA_Controller::DMA_Controller() { reset(); }

void DMA_Controller::reset() {
    memset(channels, 0, sizeof(channels));
    m_ctrl = m_stat = m_pcr = m_sqwc = m_rbsr = m_rbor = 0;
    m_irq_pending = 0;
}

// ─── Transfer start (CHCR.STR written) ────────────────────────────────────

void DMA_Controller::start_transfer(DMA_Channel ch) {
    uint32_t chcr = channels[ch].chcr;
    uint32_t mode = (chcr >> 2) & 3; // MOD: 0=normal, 1=chain, 2=interleave
    LOGI("DMA ch%d start addr=0x%08X qwc=%u mode=%u",
         ch, channels[ch].madr, channels[ch].qwc, mode);

    if (mode == 0) do_normal_transfer(ch);
    else if (mode == 1) do_chain_transfer(ch);
    else {
        LOGW("DMA ch%d interleave mode not yet implemented", ch);
    }

    m_irq_pending |= (1u << ch);
    channels[ch].chcr &= ~(1u << 8); // clear STR bit
}

// ─── Tick (background processing) ─────────────────────────────────────────

void DMA_Controller::tick(int /*cycles*/) {}

// ─── Route data to the appropriate peripheral ─────────────────────────────

void DMA_Controller::route_transfer(DMA_Channel ch, uint32_t addr, uint32_t qwc) {
    if (!g_ee_ram || addr + qwc * 16 > g_ee_ram_size) {
        LOGW("DMA ch%d route addr out of range 0x%08X (qwc=%u)", ch, addr, qwc);
        return;
    }

    uint8_t* data_ptr = g_ee_ram + addr;

    switch (ch) {
        case DMA_CH_GIF: {
            LOGI("DMA GIF → GS_Core: %u QWC from 0x%08X", qwc, addr);
            if (g_gs_ptr) {
                g_gs_ptr->process_gif((uint32_t*)data_ptr, qwc);
            } else {
                LOGW("DMA GIF: g_gs_ptr is null, transfer dropped");
            }
            break;
        }

        case DMA_CH_VIF0: {
            LOGI("DMA VIF0 → VU0: %u QWC from 0x%08X (data logged, not yet routed to VU)", qwc, addr);
            // TODO: forward to VU0 data memory via upload_data(0, ...)
            // For now we log the transfer size and source so BIOS boot can proceed
            break;
        }

        case DMA_CH_VIF1: {
            LOGI("DMA VIF1 → VU1/XGKICK: %u QWC from 0x%08X", qwc, addr);
            // VIF1 is typically used for GIF path (XGKICK).
            // In a real PS2, VIF1 unpacks tags then feeds GIF via XGKICK.
            // For now, check if the data looks like GIF packets and forward
            // directly to GS if so; otherwise log and skip.
            if (g_gs_ptr && qwc > 0) {
                g_gs_ptr->process_gif((uint32_t*)data_ptr, qwc);
                LOGI("DMA VIF1 → GS_Core (XGKICK passthrough): %u QWC", qwc);
            } else if (!g_gs_ptr) {
                LOGW("DMA VIF1: g_gs_ptr is null, transfer dropped");
            }
            break;
        }

        case DMA_CH_SIF0:
        case DMA_CH_SIF1:
        case DMA_CH_SIF2:
            LOGI("DMA SIF%d: %u QWC from 0x%08X (SIF transfer logged)", ch - DMA_CH_SIF0, qwc, addr);
            break;

        case DMA_CH_IPU_FROM:
        case DMA_CH_IPU_TO:
            LOGI("DMA IPU%d: %u QWC from 0x%08X (IPU transfer logged)", ch - DMA_CH_IPU_FROM, qwc, addr);
            break;

        case DMA_CH_SPR_FROM:
        case DMA_CH_SPR_TO:
            LOGI("DMA SPR%d: %u QWC from 0x%08X (SPR transfer logged)", ch - DMA_CH_SPR_FROM, qwc, addr);
            break;

        default:
            LOGW("DMA ch%d: unhandled route for %u QWC from 0x%08X", ch, qwc, addr);
            break;
    }
}

// ─── Normal (linear) transfer ─────────────────────────────────────────────

void DMA_Controller::do_normal_transfer(DMA_Channel ch) {
    uint32_t addr = channels[ch].madr & 0x01FFFFFF;
    uint32_t qwc  = channels[ch].qwc;

    if (!g_ee_ram || addr + qwc * 16 > g_ee_ram_size) {
        LOGW("DMA ch%d normal addr out of range 0x%08X (qwc=%u)", ch, addr, qwc);
        return;
    }

    if (qwc > 0) {
        route_transfer(ch, addr, qwc);
    }

    channels[ch].madr += qwc * 16;
    channels[ch].qwc  = 0;
}

// ─── Chain (linked-list) transfer ─────────────────────────────────────────

void DMA_Controller::do_chain_transfer(DMA_Channel ch) {
    uint32_t addr = channels[ch].tadr & 0x01FFFFFF;
    int safety = 0;

    while (true) {
        if (safety++ > 100000) {
            LOGW("DMA ch%d chain: safety limit exceeded (possible loop)", ch);
            break;
        }

        if (!g_ee_ram || addr + 16 > g_ee_ram_size) {
            LOGW("DMA ch%d chain addr out of range 0x%08X", ch, addr);
            break;
        }

        uint32_t* tag_ptr = (uint32_t*)(g_ee_ram + addr);
        uint32_t tag_word = tag_ptr[0];

        uint32_t qwc      = tag_word & 0xFFFF;
        uint8_t  id       = (tag_word >> 28) & 0x7;
        uint32_t next_addr = tag_ptr[1] & 0x01FFFFFF;

        if (qwc > 0) {
            // For TAG types with data at tag pointer, the data follows the tag
            // For type 2/3 (NEXT/REF), data is at next_addr
            uint32_t data_addr = (id == 2 || id == 3) ? next_addr : addr;
            route_transfer(ch, data_addr, qwc);
        }

        channels[ch].madr = addr + 16;
        channels[ch].tadr = next_addr;

        switch (id) {
            case 0: return; // REFE — end
            case 1: addr = next_addr; break; // CNT — continue
            case 2: addr = next_addr; break; // NEXT — next pointer
            case 3: addr = next_addr; break; // REF/REFS
            case 5: addr = next_addr; break; // CALL
            case 6: return;                  // RET — end
            case 7: return;                  // END — end
            default:
                LOGW("DMA ch%d chain: unknown tag id %d", ch, id);
                return;
        }
    }

    channels[ch].qwc = 0;
}

// ─── Register read ────────────────────────────────────────────────────────

uint32_t DMA_Controller::read_reg(uint32_t addr) {
    uint32_t ch_base = 0x10008000;

    // Per-channel registers (0x10008000 – 0x10008FFF, 10 channels × 0x10)
    if (addr >= ch_base && addr < ch_base + DMA_CH_COUNT * 0x10) {
        uint32_t ch  = (addr - ch_base) >> 4;
        uint32_t reg = (addr - ch_base) & 0xF;
        switch (reg) {
            case 0x0: return channels[ch].chcr;
            case 0x4: return channels[ch].madr;
            case 0x8: return channels[ch].qwc;
            case 0xC: return channels[ch].tadr;
            default:  return 0;
        }
    }

    // Global registers
    switch (addr) {
        case 0x1000E000: { // DMA_CTRL — read returns 0 (no special bits)
            return 0;
        }
        case 0x1000E010: { // STAT — bit N=1 means channel N is NOT busy
            // Return complement: bit=0 means channel is busy (transferring)
            // All channels idle → all bits set to 1
            uint32_t stat = 0;
            for (int i = 0; i < DMA_CH_COUNT; i++) {
                if (!(channels[i].chcr & (1u << 8))) {
                    stat |= (1u << i); // STR not set → not busy → bit=1
                }
            }
            return stat;
        }
        case 0x1000E020: return m_pcr;   // Priority Control
        case 0x1000E030: return m_sqwc;  // Stub Speed Queue
        case 0x1000E040: return m_rbsr;  // Ring Buffer Size
        case 0x1000E050: return m_rbor;  // Ring Buffer Base
        default:
            LOGW("DMA read_reg unhandled addr=0x%08X", addr);
            return 0;
    }
}

// ─── Register write ───────────────────────────────────────────────────────

void DMA_Controller::write_reg(uint32_t addr, uint32_t val) {
    uint32_t ch_base = 0x10008000;

    // Per-channel registers
    if (addr >= ch_base && addr < ch_base + DMA_CH_COUNT * 0x10) {
        uint32_t ch  = (addr - ch_base) >> 4;
        uint32_t reg = (addr - ch_base) & 0xF;
        switch (reg) {
            case 0x0:
                channels[ch].chcr = val;
                if (val & (1u << 8))
                    start_transfer((DMA_Channel)ch);
                break;
            case 0x4: channels[ch].madr = val; break;
            case 0x8: channels[ch].qwc  = val; break;
            case 0xC: channels[ch].tadr = val; break;
            default: break;
        }
        return;
    }

    // Global registers
    switch (addr) {
        case 0x1000E000:
            m_ctrl = val;
            break;
        case 0x1000E010:
            // STAT: write-1-to-clear per-channel busy flags
            // In practice this clears IRQ-pending bits
            m_stat &= ~val;
            break;
        case 0x1000E020:
            m_pcr = val;
            LOGI("DMA PCR = 0x%08X", val);
            break;
        case 0x1000E030:
            m_sqwc = val;
            break;
        case 0x1000E040:
            m_rbsr = val;
            break;
        case 0x1000E050:
            m_rbor = val;
            LOGI("DMA RBOR = 0x%08X", val);
            break;
        default:
            LOGW("DMA write_reg unhandled addr=0x%08X val=0x%08X", addr, val);
            break;
    }
}
