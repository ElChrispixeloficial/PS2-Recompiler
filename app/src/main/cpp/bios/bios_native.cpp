#include "bios_native.h"
#include "ee/ee_core.h"
#include "iop/iop_core.h"
#include <android/log.h>
#include <cstring>

#define TAG "PS2-BIOS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static EE_Core* g_ee_native = nullptr;
static IOP_Core* g_iop_native = nullptr;
static bool g_bios_initialized = false;
static uint32_t g_game_entry_point = 0x00100000; // Punto de entrada por defecto

void PS2_BIOS::init() {
    g_bios_initialized = false;
    LOGI("BIOS Nativa (HLE) inicializada.");
}

void PS2_BIOS::set_ee_core(EE_Core* ee) { g_ee_native = ee; }
void PS2_BIOS::set_iop_core(IOP_Core* iop) { g_iop_native = iop; }

void PS2_BIOS::set_game_entry(uint32_t entry) {
    g_game_entry_point = entry;
    LOGI("Punto de entrada del juego establecido: 0x%08X", entry);
}

void PS2_BIOS::reset() {
    g_bios_initialized = false;
}

void PS2_BIOS::execute() {
    if (g_bios_initialized) return;
    if (!g_ee_native) return;

    LOGI("Ejecutando arranque HLE nativo (Salto directo al juego)...");

    // 1. Despertar al EE del bucle de espera (SIF Mailbox)
    uint32_t* ee_ram = (uint32_t*)g_ee_native->get_ram();
    ee_ram[0x0000F200 >> 2] = 0x00010000; 

    // 2. Configurar Stack Pointer (SP) y registros básicos
    g_ee_native->state.gpr_lo[29] = 0x01FFFFF0; // SP
    g_ee_native->state.gpr_lo[28] = 0x01FFFFF0; // FP
    g_ee_native->state.gpr_lo[31] = 0x00000000; // RA
    
    // 3. Habilitar interrupciones en el COP0
    g_ee_native->state.cop0[12] = 0x00010401; // Status: IE=1, EXL=0, KSU=0
    
    // 4. ¡SALTAR DIRECTAMENTE AL JUEGO!
    // Evitamos que el EE ejecute el Kernel vacío y lo mandamos al juego.
    g_ee_native->state.pc = g_game_entry_point;
    LOGI("EE saltando al juego en PC: 0x%08X", g_ee_native->state.pc);
    
    g_bios_initialized = true;
}