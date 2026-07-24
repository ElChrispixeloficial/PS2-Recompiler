#pragma once
#include <cstdint>

#define PS2_TIMER_COUNT 6

struct PS2_Timer {
    uint32_t count;
    uint32_t mode;
    uint32_t target;
};

struct EE_HW {
    uint32_t i_stat;
    uint32_t i_mask;
    PS2_Timer timers[PS2_TIMER_COUNT];
    uint32_t mecnifo;
};

void ee_hw_init(EE_HW& hw);
uint32_t ee_hw_read32(EE_HW& hw, uint32_t phys_addr);
void ee_hw_write32(EE_HW& hw, uint32_t phys_addr, uint32_t val);
bool ee_hw_tick(EE_HW& hw, int cycles);
void ee_hw_raise_interrupt(EE_HW& hw, uint32_t bit);
void ee_hw_clear_interrupt(EE_HW& hw, uint32_t bit);
