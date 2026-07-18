#pragma once
#include <cstdint>

class EE_Core;

uint32_t hw_read32(EE_Core& ee, uint32_t addr);
void     hw_write32(EE_Core& ee, uint32_t addr, uint32_t val);
bool     hw_tick(int cycles);
