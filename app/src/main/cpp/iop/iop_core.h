#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include "../ee/ee_memory.h"

// Tamaño de la RAM del IOP (2MB) si no está definido en ee_memory.h
#ifndef IOP_RAM_SIZE
#define IOP_RAM_SIZE 0x200000
#endif

struct IOP_State {
    uint32_t gpr[32];      // Registros de propósito general
    uint32_t pc, hi, lo;   // Contador de programa y registros HI/LO
    uint32_t cop0[32];     // Registros del Coprocesador 0
    bool     halted;       // Bandera de detención
    bool     branch_delay; // Bandera de Branch Delay Slot
    uint32_t branch_target;// Dirección de destino del salto diferido
};

// Declaraciones hacia adelante
class CodeCache;
class IOP_Recompiler;

class IOP_Core {
public:
    IOP_Core();
    ~IOP_Core();
    
    void run_cycles(int64_t cycles);
    void interpret_single_instruction();
    uint32_t read32(uint32_t addr);
    void write32(uint32_t addr, uint32_t val);
    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    void write8(uint32_t addr, uint8_t val);
    void write16(uint32_t addr, uint16_t val);
    uint8_t read_pad(int port, int byte);
    uint8_t* get_ram() { return iop_ram; }
    
    IOP_State state;
    
private:
    std::unique_ptr<CodeCache> cache;
    std::unique_ptr<IOP_Recompiler> jit;
    uint8_t iop_ram[IOP_RAM_SIZE];
};