#pragma once

#include "mips_defs.h"
#include "code_cache.h"
#include "arm64_emitter.h"

#include <cstdint>
#include <vector>

class EE_Recompiler;

// -------------------------------------------------
// Contexto de emisión ARM64
// -------------------------------------------------

struct EmitContext {
    uint32_t current_pc = 0;
    ARM64Emitter* emitter = nullptr;
};

// -------------------------------------------------
// Contexto de ejecución del JIT
// -------------------------------------------------

struct RuntimeContext {
    EE_State* state = nullptr;
    uint8_t* ram = nullptr;
    EE_Recompiler* jit = nullptr;
};

// -------------------------------------------------
// Asignador de registros
// -------------------------------------------------

struct RegisterAllocator {
    void reset();
};

// -------------------------------------------------
// Recompilador del Emotion Engine
// -------------------------------------------------

class EE_Recompiler {

public:

    using CompiledBlock = void(*)(EE_State*, uint8_t*);

    struct Statistics {
        uint64_t blocks_compiled = 0;
        uint64_t instructions_compiled = 0;
    };

    EE_Recompiler(
        CodeCache& cache,
        EE_State& state,
        uint8_t* ram
    );

    ~EE_Recompiler();

    void reset();

    CompiledBlock compile_block(
        uint32_t pc
    );

    const Statistics& get_statistics() const;

private:

    CodeCache& m_cache;
    EE_State& m_state;
    uint8_t* m_ram;

    EmitContext m_emit_ctx;
    RuntimeContext m_runtime;
    RegisterAllocator m_registers;

    Statistics stats;
};

