#pragma once
#include <cstdint>

namespace PS2_BIOS {

// BIOS del IOP ultra-simple: solo NOPs y branches
// No usa lw/sw - no llama a call() - no puede crashear
static const uint32_t IOP_BIOS[] = {
    0x00000000, // NOP
    0x00000000, // NOP  
    0x00000000, // NOP
    0x00000000, // NOP
    0x08000000, // j 0 (bucle infinito)
    0x00000000, // NOP (delay slot)
};

static const size_t IOP_BIOS_SIZE = sizeof(IOP_BIOS);

} // namespace PS2_BIOS
