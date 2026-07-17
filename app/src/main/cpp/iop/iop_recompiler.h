#pragma once
#include "iop_core.h"
#include "../ee/code_cache.h"
#include <cstdint>
#include <cstddef>

extern "C" {
    uint32_t iop_bus_read32(uint32_t a);
    void     iop_bus_write32(uint32_t a, uint32_t v);
    uint16_t iop_bus_read16(uint32_t a);
    void     iop_bus_write16(uint32_t a, uint16_t v);
    uint8_t  iop_bus_read8 (uint32_t a);
    void     iop_bus_write8(uint32_t a, uint8_t v);
}
extern uint8_t* g_iop_ram_ptr;

class IOP_Recompiler {
public:
    using CompiledBlock = void (*)();
    
    IOP_Recompiler(CodeCache& c, IOP_State& s, uint8_t* ram);
    void invalidate(uint32_t s, uint32_t e);
    CompiledBlock compile_block(uint32_t pc);

private:
    CodeCache& m_cache;
    IOP_State& m_state;
    uint8_t*   m_ram;
};