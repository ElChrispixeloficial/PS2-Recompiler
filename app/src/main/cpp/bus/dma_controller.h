#pragma once
#include <cstdint>

/* Canales DMA del EE (PS2) */
enum DMA_Channel {
    DMA_CH_VIF0  = 0,
    DMA_CH_VIF1  = 1,
    DMA_CH_GIF   = 2,
    DMA_CH_IPU_FROM = 3,
    DMA_CH_IPU_TO   = 4,
    DMA_CH_SIF0  = 5,
    DMA_CH_SIF1  = 6,
    DMA_CH_SIF2  = 7,
    DMA_CH_SPR_FROM = 8,
    DMA_CH_SPR_TO   = 9,
    DMA_CH_COUNT = 10
};

struct DMA_ChannelRegs {
    uint32_t chcr;   /* control */
    uint32_t madr;   /* dirección memoria */
    uint32_t qwc;    /* quad-word count */
    uint32_t tadr;   /* tag address */
    uint32_t asr[2]; /* address saved */
    uint32_t sadr;   /* scratchpad address */
};

/* Forward */
struct EE_State;

class DMA_Controller {
public:
    DMA_Controller();
    void reset();

    /* Activado por escritura en CHCR con STR=1 */
    void start_transfer(DMA_Channel ch);

    /* Llamado cada ~8 ciclos EE para procesar un slice */
    void tick(int cycles);

    /* Acceso registros (0x1000_8000 + offset) */
    uint32_t read_reg(uint32_t addr);
    void     write_reg(uint32_t addr, uint32_t val);

    /* Interrupciones pendientes */
    bool has_irq() const { return m_irq_pending != 0; }
    uint32_t irq_mask() const { return m_irq_pending; }
    void clear_irq(uint32_t mask) { m_irq_pending &= ~mask; }

    DMA_ChannelRegs channels[DMA_CH_COUNT];

private:
    uint32_t m_ctrl;
    uint32_t m_stat;
    uint32_t m_pcr;
    uint32_t m_sqwc;
    uint32_t m_rbsr;
    uint32_t m_rbor;
    uint32_t m_irq_pending;

    void do_normal_transfer(DMA_Channel ch);
    void do_chain_transfer(DMA_Channel ch);
};
