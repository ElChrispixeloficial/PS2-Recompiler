#pragma once
#include "code_cache.h"
#include "ee_core.h"  // <--- AÑADIDO: Aquí están EE_State, RA, SP, GP
#include "mips_defs.h"
#include <cstdint>
#include <cstddef>

class EE_Recompiler {
public:
    typedef void (*CompiledBlock)(EE_State* state, uint8_t* ram);
    
    EE_Recompiler(CodeCache& cache, EE_State& state, uint8_t* ram);
    void invalidate(uint32_t s, uint32_t e);
    CompiledBlock compile_block(uint32_t pc);

private:
    CodeCache& m_cache;
    EE_State&  m_state;
    uint8_t*   m_ram;
};