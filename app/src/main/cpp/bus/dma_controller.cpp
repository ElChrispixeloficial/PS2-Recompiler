#include "dma_controller.h"
// Ajuste de la ruta relativa correcta
#include "../gs/gs_core.h" 
#include <android/log.h>
#include <cstring>

#define TAG "DMA"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

extern uint8_t* g_ee_ram;      
extern uint32_t g_ee_ram_size;

DMA_Controller::DMA_Controller() { reset(); }

void DMA_Controller::reset() {
    memset(channels, 0, sizeof(channels));
    m_ctrl = m_stat = m_pcr = m_sqwc = m_rbsr = m_rbor = 0;
    m_irq_pending = 0;
}

void DMA_Controller::start_transfer(DMA_Channel ch) {
    uint32_t chcr = channels[ch].chcr;
    uint32_t mode = (chcr >> 2) & 3; /* MOD: 0=normal, 1=chain, 2=interleave */
    LOGI("DMA ch%d start addr=0x%08X qwc=%u mode=%u", ch, channels[ch].madr, channels[ch].qwc, mode);
    
    if (mode == 0) do_normal_transfer(ch);
    else if (mode == 1) do_chain_transfer(ch); 

    m_irq_pending |= (1u << ch);
    channels[ch].chcr &= ~(1u << 8);
}

void DMA_Controller::tick(int /*cycles*/) {}

void DMA_Controller::do_normal_transfer(DMA_Channel ch) {
    uint32_t addr = channels[ch].madr & 0x01FFFFFF;
    uint32_t qwc  = channels[ch].qwc;
    if (!g_ee_ram || addr + qwc * 16 > g_ee_ram_size) {
        LOGW("DMA ch%d addr out of range 0x%08X", ch, addr);
        return;
    }
    
    if (ch == DMA_CH_GIF) {
        LOGI(">>> DMA GIF Normal: %u QWC desde 0x%08X (¡Enviando a GS!)", qwc, addr);
        // g_gs_core.process_gif((uint32_t*)(g_ee_ram + addr), qwc); // Descomenta esto cuando ajustes el nombre de tu clase
    }
    
    channels[ch].madr += qwc * 16;
    channels[ch].qwc   = 0;
}

void DMA_Controller::do_chain_transfer(DMA_Channel ch) {
    uint32_t addr = channels[ch].tadr & 0x01FFFFFF;
    int safety_counter = 0; 

    while (true) {
        if (safety_counter++ > 100000) {
            LOGW("DMA ch%d Chain mode bucle infinito detectado.", ch);
            break;
        }

        if (!g_ee_ram || addr + 16 > g_ee_ram_size) {
            LOGW("DMA ch%d chain addr out of range 0x%08X", ch, addr);
            break;
        }

        uint32_t* tag_ptr = (uint32_t*)(g_ee_ram + addr);
        uint32_t tag_word = tag_ptr[0];
        
        uint32_t qwc = tag_word & 0xFFFF;
        uint8_t id = (tag_word >> 28) & 0x7; 
        uint32_t next_addr = tag_ptr[1] & 0x01FFFFFF; 

        if (qwc > 0) {
            uint32_t data_addr = (id == 2 || id == 3) ? next_addr : addr; 
            
            if (ch == DMA_CH_GIF) {
                LOGI(">>> DMA GIF Chain: %u QWC desde 0x%08X (¡Enviando a GS!)", qwc, data_addr);
                // g_gs_core.process_gif((uint32_t*)(g_ee_ram + data_addr), qwc); // Descomenta esto cuando ajustes el nombre de tu clase
            }
        }

        channels[ch].madr = addr + 16; 
        channels[ch].tadr = next_addr;

        switch (id) {
            case 0: return; // REFE
            case 1: addr = next_addr; break; // CNT
            case 2: addr = next_addr; break; // NEXT
            case 3: addr = next_addr; break; // REF/REFS
            case 5: addr = next_addr; break; // CALL
            case 6: return; // RET
            case 7: return; // END
            default: return;
        }
    }
    
    channels[ch].qwc = 0;
}

uint32_t DMA_Controller::read_reg(uint32_t addr) {
    uint32_t ch_base = 0x10008000;
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
    if (addr == 0x1000E000) return m_ctrl;
    if (addr == 0x1000E010) return m_stat;
    return 0;
}

void DMA_Controller::write_reg(uint32_t addr, uint32_t val) {
    uint32_t ch_base = 0x10008000;
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
        }
        return;
    }
    if (addr == 0x1000E000) m_ctrl = val;
    if (addr == 0x1000E010) m_stat &= ~val; 
}