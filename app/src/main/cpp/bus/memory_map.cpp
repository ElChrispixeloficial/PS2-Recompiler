// memory_map.cpp — Mapa de memoria del sistema PS2
// Las traducciones de dirección virtual → física están inline en ee_memory.h.
// Este archivo contiene helpers para el bus de hardware (registros INTC, TIMER, etc.)

#include "../ee/ee_core.h"
#include <android/log.h>

#define LOG_TAG "PS2-BUS"

// Registros de hardware del EE mapeados en 0x10000000-0x10008FFF
uint32_t hw_read32(EE_Core& ee, uint32_t addr) {
    switch (addr) {
    case 0x1000F130: return 0;          // COP0 PCCR
    case 0x1000F000: return 0xFFFFFFFF; // INTC STAT — no hay interrupciones pendientes
    case 0x1000F010: return 0xFFFFFFFF; // INTC MASK
    default:         return 0;
    }
}

void hw_write32(EE_Core& ee, uint32_t addr, uint32_t val) {
    switch (addr) {
    case 0x1000F000:  // INTC STAT — escribir 1 limpia la interrupción
        break;
    case 0x1000F010:  // INTC MASK
        break;
    }
}
