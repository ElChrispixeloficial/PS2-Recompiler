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