#include "dma_controller.h"
#include "../gs/gs_core.h"
#include "../vu/vu_core.h"
#include "../vu/vif_unpacker.h"
#include "../iop/iop_core.h"
#include <android/log.h>
#include <cstring>

extern uint8_t* g_iop_ram_ptr;

#define TAG "DMA"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

extern uint8_t*  g_ee_ram;
extern uint32_t  g_ee_ram_size;
extern GS_Core*  g_gs_ptr;
extern VU_Core*  g_vu_core_ptr;
static VIF_Unpacker g_dma_vif1;

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
            LOGI("DMA VIF1: %u QWC from 0x%08X", qwc, addr);
            if (!g_ee_ram || qwc == 0) break;
            const uint32_t* words = (const uint32_t*)(g_ee_ram + addr);
            int total_words = qwc * 4;
            int pos = 0;

            while (pos < total_words) {
                uint32_t tag = words[pos++];
                uint32_t cmd = (tag >> 24) & 0xFF;
                uint32_t nloop = tag & 0x7FFF;
                int cmd_qwc = (nloop > 0) ? ((total_words - pos) / 4) : 0;
                if (cmd_qwc > nloop && nloop > 0) cmd_qwc = nloop;

                if (cmd >= 0x60 && cmd <= 0x7F) {
                    int data_words = cmd_qwc * 4;
                    if (pos + data_words > total_words) data_words = total_words - pos;
                    if (g_vu_core_ptr) {
                        g_dma_vif1.feed_packet(tag, &words[pos], data_words / 4, *g_vu_core_ptr, 1);
                    }
                    pos += data_words;
                } else if (cmd == 0x30 || cmd == 0x31) {
                    int data_words = cmd_qwc * 4;
                    if (pos + data_words > total_words) data_words = total_words - pos;
                    if (g_gs_ptr && data_words > 0) {
                        g_gs_ptr->process_gif((const uint32_t*)&words[pos], data_words / 4);
                    }
                    pos += data_words;
                } else {
                    if (g_vu_core_ptr) {
                        g_dma_vif1.feed_packet(tag, nullptr, 0, *g_vu_core_ptr, 1);
                    }
                }
            }
            break;
        }

        case DMA_CH_SIF0: {
            // SIF0: EE → IOP data transfer
            LOGI("DMA SIF0: %u QWC from 0x%08X (EE→IOP)", qwc, addr);
            if (!g_ee_ram || qwc == 0) break;
            // Map EE physical address to IOP RAM (SIF0 transfers from EE to IOP)
            // IOP sees EE addresses in the 0x01000000+ range, map to IOP RAM offset
            extern uint8_t* g_iop_ram_ptr;
            if (g_iop_ram_ptr) {
                uint32_t iop_dest = addr & (IOP_RAM_SIZE - 1);
                if (iop_dest + qwc * 16 <= IOP_RAM_SIZE) {
                    memcpy(g_iop_ram_ptr + iop_dest, g_ee_ram + addr, qwc * 16);
                    LOGI("DMA SIF0: copied %u QWC to IOP RAM+0x%08X", qwc, iop_dest);
                }
            }
            break;
        }

        case DMA_CH_SIF1: {
            // SIF1: IOP → EE data transfer
            LOGI("DMA SIF1: %u QWC from 0x%08X (IOP→EE)", qwc, addr);
            if (!g_ee_ram || qwc == 0) break;
            // SIF1 transfers data from IOP to EE. The MADR in EE space is the dest.
            // Read source from IOP RAM via SIF FIFO (simplified: use MADR as EE dest)
            extern uint8_t* g_iop_ram_ptr;
            if (g_iop_ram_ptr) {
                // In practice, IOP writes data to its SIF FIFO, EE reads it.
                // For HLE: just copy from the EE MADR area (IOP already placed data there via SIF)
                LOGI("DMA SIF1: %u QWC transfer (IOP→EE) — data already in EE RAM", qwc);
            }
            break;
        }

        case DMA_CH_SIF2: {
            // SIF2: bidirectional (typically debug/unused in games)
            LOGI("DMA SIF2: %u QWC from 0x%08X (bidirectional)", qwc, addr);
            break;
        }

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
