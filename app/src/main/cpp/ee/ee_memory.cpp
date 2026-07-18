#include <android/log.h>
#include <cstring>
#include <cstdio>
#include "ee_memory.h"

#define TAG "EE-MEM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

uint8_t* g_ee_ram = nullptr;
uint32_t g_ee_ram_size = 32*1024*1024;

// El puntero g_bios ahora apuntará al array bios_rom de 4MB del EE_Core
uint8_t* g_bios = nullptr; 

uint8_t s_scratchpad[16*1024];
extern uint32_t g_sif_buffer[];

extern "C" void gs_write_reg(uint32_t addr, uint32_t val);
extern "C" void gs_write_priv(uint32_t addr, uint32_t val);

static uint32_t vmap(uint32_t addr) {
    if (addr >= 0x80000000 && addr < 0xC0000000) return addr & 0x1FFFFFFF;
    if (addr >= 0x70000000 && addr < 0x70010000) return 0x70000000 | (addr & 0x3FFF);
    return addr;
}

extern "C" uint32_t ee_mem_read32(uint32_t addr) {
    uint32_t phys = vmap(addr);
    
    // RAM Principal (32MB)
    if (g_ee_ram && phys < g_ee_ram_size) return *(uint32_t*)(g_ee_ram + phys);
    
    // BIOS ROM (4MB mapeados en 0x1FC00000)
    if (phys >= 0x1FC00000 && phys < 0x20000000 && g_bios) {
        return *(uint32_t*)(g_bios + (phys - 0x1FC00000));
    }
    
    if ((phys & 0xFFFF0000) == 0x70000000) return *(uint32_t*)(s_scratchpad + (phys & 0x3FFF));
    if ((phys >= 0x10000000 && phys < 0x10010000)) return g_sif_buffer[0]; 

    return 0;
}

extern "C" void ee_mem_write32(uint32_t addr, uint32_t val) {
    uint32_t phys = vmap(addr);
    
    if (g_ee_ram && phys < g_ee_ram_size) { 
        *(uint32_t*)(g_ee_ram + phys) = val; 
        return; 
    }
    if ((phys & 0xFFFF0000) == 0x70000000) { 
        *(uint32_t*)(s_scratchpad + (phys & 0x3FFF)) = val; 
        return; 
    }
    
    if (phys >= 0x12000000 && phys < 0x12010000) {
        gs_write_reg(phys, val); 
        return;
    }
    if (phys >= 0x12001000 && phys < 0x12002000) {
        gs_write_priv(phys, val); 
        return;
    }

    if ((phys >= 0x10000000 && phys < 0x10010000)) {
        g_sif_buffer[0] = val;
        return;
    }
}

void ee_mem_init(uint8_t* ram, uint32_t size, uint8_t* bios) { 
    g_ee_ram = ram; 
    g_ee_ram_size = size; 
    g_bios = bios; // Guardamos el puntero a la ROM
}

extern "C" uint8_t ee_mem_read8(uint32_t addr) {
    uint32_t phys = vmap(addr);
    if (g_ee_ram && phys < g_ee_ram_size) return g_ee_ram[phys];
    if (phys >= 0x1FC00000 && phys < 0x20000000 && g_bios) return g_bios[phys - 0x1FC00000];
    if ((phys & 0xFFFF0000) == 0x70000000) return s_scratchpad[phys & 0x3FFF];
    return 0;
}
extern "C" uint16_t ee_mem_read16(uint32_t addr) {
    uint32_t phys = vmap(addr);
    if (g_ee_ram && phys + 1 < g_ee_ram_size) return *(uint16_t*)(g_ee_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x20000000 && g_bios) return *(uint16_t*)(g_bios + (phys - 0x1FC00000));
    if ((phys & 0xFFFF0000) == 0x70000000) return *(uint16_t*)(s_scratchpad + (phys & 0x3FFF));
    return 0;
}
extern "C" void ee_mem_write8(uint32_t addr, uint8_t val) {
    uint32_t phys = vmap(addr);
    if (g_ee_ram && phys < g_ee_ram_size) g_ee_ram[phys] = val;
    else if ((phys & 0xFFFF0000) == 0x70000000) s_scratchpad[phys & 0x3FFF] = val;
}
extern "C" void ee_mem_write16(uint32_t addr, uint16_t val) {
    uint32_t phys = vmap(addr);
    if (g_ee_ram && phys + 1 < g_ee_ram_size) *(uint16_t*)(g_ee_ram + phys) = val;
    else if ((phys & 0xFFFF0000) == 0x70000000) *(uint16_t*)(s_scratchpad + (phys & 0x3FFF)) = val;
}

extern "C" void ee_mem_read128(uint32_t addr, uint8_t* out) {
    uint32_t phys = vmap(addr);
    if (g_ee_ram && phys + 15 < g_ee_ram_size) {
        memcpy(out, g_ee_ram + phys, 16);
    } else {
        uint32_t lo = ee_mem_read32(phys);
        uint32_t hi = ee_mem_read32(phys + 4);
        uint32_t lo2 = ee_mem_read32(phys + 8);
        uint32_t hi2 = ee_mem_read32(phys + 12);
        memcpy(out, &lo, 4);
        memcpy(out + 4, &hi, 4);
        memcpy(out + 8, &lo2, 4);
        memcpy(out + 12, &hi2, 4);
    }
}

extern "C" void ee_mem_write128(uint32_t addr, const uint8_t* in) {
    uint32_t phys = vmap(addr);
    if (g_ee_ram && phys + 15 < g_ee_ram_size) {
        memcpy(g_ee_ram + phys, in, 16);
    } else {
        uint32_t v0, v1, v2, v3;
        memcpy(&v0, in, 4);
        memcpy(&v1, in + 4, 4);
        memcpy(&v2, in + 8, 4);
        memcpy(&v3, in + 12, 4);
        ee_mem_write32(phys, v0);
        ee_mem_write32(phys + 4, v1);
        ee_mem_write32(phys + 8, v2);
        ee_mem_write32(phys + 12, v3);
    }
}

// ─── Unaligned load/store helpers (PS2 big-endian semantics) ──────────────────
// LWL/LWR: Load Word Left/Right — combine bytes from aligned word with register
extern "C" uint32_t ee_lwl(uint32_t addr, uint32_t reg_val) {
    uint32_t phys = vmap(addr);
    uint32_t aligned = phys & ~3u;
    uint32_t shift = (phys & 3) * 8;
    uint32_t mem_val = ee_mem_read32(aligned);
    return (reg_val & ((1u << shift) - 1)) | (mem_val << shift);
}
extern "C" uint32_t ee_lwr(uint32_t addr, uint32_t reg_val) {
    uint32_t phys = vmap(addr);
    uint32_t aligned = phys & ~3u;
    uint32_t shift = (3 - (phys & 3)) * 8;
    uint32_t mem_val = ee_mem_read32(aligned);
    return (reg_val & ~((1u << shift) - 1)) | (mem_val >> shift);
}

// LDL/LDR: Load Doubleword Left/Right
extern "C" uint32_t ee_ldl(uint32_t addr, uint64_t reg_val) {
    uint32_t phys = vmap(addr);
    uint32_t aligned = phys & ~7u;
    uint32_t shift = (phys & 7) * 8;
    uint64_t mem_val = uint64_t(ee_mem_read32(aligned)) | (uint64_t(ee_mem_read32(aligned + 4)) << 32);
    if (shift == 0) return uint32_t(mem_val);
    uint64_t mask = (1uLL << shift) - 1;
    return uint32_t((reg_val & mask) | (mem_val << shift));
}
extern "C" uint32_t ee_ldr(uint32_t addr, uint64_t reg_val) {
    uint32_t phys = vmap(addr);
    uint32_t aligned = phys & ~7u;
    uint32_t shift = (7 - (phys & 7)) * 8;
    uint64_t mem_val = uint64_t(ee_mem_read32(aligned)) | (uint64_t(ee_mem_read32(aligned + 4)) << 32);
    if (shift == 0) return uint32_t(reg_val >> 32);
    uint64_t mask = ~((1uLL << shift) - 1);
    return uint32_t(((reg_val & mask) >> (64 - shift)) | (mem_val >> shift));
}

// SWL/SWR: Store Word Left/Right
extern "C" void ee_swl(uint32_t addr, uint32_t reg_val, uint32_t* mem_ptr) {
    uint32_t shift = (addr & 3) * 8;
    uint32_t mask = ((1u << shift) - 1);
    *mem_ptr = (*mem_ptr & ~mask) | (reg_val >> shift);
}
extern "C" void ee_swr(uint32_t addr, uint32_t reg_val, uint32_t* mem_ptr) {
    uint32_t shift = (3 - (addr & 3)) * 8;
    uint32_t mask = ~((1u << shift) - 1);
    *mem_ptr = (*mem_ptr & mask) | (reg_val << shift);
}

// SDL/SDR: Store Doubleword Left/Right
extern "C" void ee_sdl(uint32_t addr, uint64_t reg_val, uint64_t* mem_ptr) {
    uint32_t shift = (addr & 7) * 8;
    if (shift == 0) { *mem_ptr = reg_val; return; }
    uint64_t mask = (1uLL << shift) - 1;
    *mem_ptr = (*mem_ptr & mask) | (reg_val << shift);
}
extern "C" void ee_sdr(uint32_t addr, uint64_t reg_val, uint64_t* mem_ptr) {
    uint32_t shift = (7 - (addr & 7)) * 8;
    if (shift == 0) { *mem_ptr = reg_val; return; }
    uint64_t mask = ~((1uLL << shift) - 1);
    *mem_ptr = (*mem_ptr & mask) | (reg_val >> (64 - shift));
}