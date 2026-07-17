#pragma once
#include <cstdint>
#include <cstddef>

class VU_Core;

// Inicializa la memoria ejecutable para el JIT de la VU
void init_vu_jit();

// Recompila un bloque de microcódigo VU a código nativo ARM64
// Devuelve un puntero a la función nativa generada
uint8_t* vu_recompile_block(VU_Core& vu_core, int unit, uint32_t micro_pc);