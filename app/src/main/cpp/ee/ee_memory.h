#pragma once
#include <cstdint>
#include <cstddef>

// Las constantes de tamaño (EE_RAM_SIZE, etc.) ya están definidas en ee_core.h.
// No las redefinimos aquí para evitar errores del compilador.

// Inicializar la memoria del EE (RAM + BIOS ROM)
void ee_mem_init(uint8_t* ram, uint32_t size, uint8_t* bios);

extern "C" {
    uint32_t ee_mem_read32(uint32_t addr);
    void     ee_mem_write32(uint32_t addr, uint32_t val);
    uint8_t  ee_mem_read8 (uint32_t addr);
    void     ee_mem_write8(uint32_t addr, uint8_t val);
    uint16_t ee_mem_read16(uint32_t addr);
    void     ee_mem_write16(uint32_t addr, uint16_t val);
}