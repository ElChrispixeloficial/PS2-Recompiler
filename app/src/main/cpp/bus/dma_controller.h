#pragma once
#include <cstdint>

// PS2 DMA Controller — EE-side DMA channels (0x1000_8000 block).
// Routes GIF transfers to GS_Core, VIF0/VIF1 to VU_Core via XGKICK.
// Supports normal and linked-list chain modes per channel.

enum DMA_Channel {
    DMA_CH_VIF0     = 0,
    DMA_CH_VIF1     = 1,
    DMA_CH_GIF      = 2,
    DMA_CH_IPU_FROM = 3,
    DMA_CH_IPU_TO   = 4,
    DMA_CH_SIF0     = 5,
    DMA_CH_SIF1     = 6,
    DMA_CH_SIF2     = 7,
    DMA_CH_SPR_FROM = 8,
    DMA_CH_SPR_TO   = 9,
    DMA_CH_COUNT    = 10
};

struct DMA_ChannelRegs {
    uint32_t chcr;   // control
    uint32_t madr;   // memory address
    uint32_t qwc;    // quad-word count
    uint32_t tadr;   // tag address
    uint32_t asr[2]; // address saved
    uint32_t sadr;   // scratchpad address
};

class DMA_Controller {
public:
    DMA_Controller();
    void reset();

    // Called when CHCR.STR=1 is written — initiates a transfer
    void start_transfer(DMA_Channel ch);

    // Called every ~8 EE cycles for background processing
    void tick(int cycles);

    // Register access (physical address 0x1000_8000 + offset)
    uint32_t read_reg(uint32_t addr);
    void     write_reg(uint32_t addr, uint32_t val);

    // IRQ interface
    bool has_irq() const { return m_irq_pending != 0; }
    uint32_t irq_mask() const { return m_irq_pending; }
    void clear_irq(uint32_t mask) { m_irq_pending &= ~mask; }

    DMA_ChannelRegs channels[DMA_CH_COUNT];

private:
    uint32_t m_ctrl;        // 0x1000E000 — DMA control
    uint32_t m_stat;        // 0x1000E010 — STAT (write-1-to-clear busy bits)
    uint32_t m_pcr;         // 0x1000E020 — Priority Control Register (priority per channel)
    uint32_t m_sqwc;        // 0x1000E030 — Stub speed queue control
    uint32_t m_rbsr;        // 0x1000E040 — Ring buffer size register
    uint32_t m_rbor;        // 0x1000E050 — Ring buffer base address register
    uint32_t m_irq_pending; // pending IRQ bitmask (1 bit per channel)

    void do_normal_transfer(DMA_Channel ch);
    void do_chain_transfer(DMA_Channel ch);
    void route_transfer(DMA_Channel ch, uint32_t addr, uint32_t qwc);
};
