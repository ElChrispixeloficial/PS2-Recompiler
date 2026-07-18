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
    void     ee_mem_read128(uint32_t addr, uint8_t* out);
    void     ee_mem_write128(uint32_t addr, const uint8_t* in);
    uint32_t ee_lwl(uint32_t addr, uint32_t mem_val);
    uint32_t ee_lwr(uint32_t addr, uint32_t mem_val);
    uint32_t ee_ldl(uint32_t addr, uint64_t mem_val);
    uint32_t ee_ldr(uint32_t addr, uint64_t mem_val);
    void     ee_swl(uint32_t addr, uint32_t reg_val);
    void     ee_swr(uint32_t addr, uint32_t reg_val);
    void     ee_sdl(uint32_t addr, uint64_t reg_val);
    void     ee_sdr(uint32_t addr, uint64_t reg_val);
}