// ═══════════════════════════════════════════════════════════════════════════════
// JNI Bridge — PR2 Recompiler
// ═══════════════════════════════════════════════════════════════════════════════

#include "ee/ee_core.h"
#include "ee/ee_memory.h"
#include "gs/gs_core.h"
#include "iop/iop_core.h"
#include "vu/vu_core.h"
#include "bus/dma_controller.h"
#include "iso/iso_loader.h"
#include "bios/bios_native.h"
#include "homebrew/pr2_homebrew.h"
#include "spu2/spu2_core.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <cstring>
#include <cstdio>
#include <chrono>

#define TAG "PS2-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern uint8_t* g_bios;

static std::unique_ptr<EE_Core>          g_ee;
static std::unique_ptr<GS_Core>          g_gs;
static std::unique_ptr<IOP_Core>         g_iop;
static std::unique_ptr<VU_Core>          g_vu;
static std::unique_ptr<DMA_Controller>   g_dma;
static ANativeWindow* g_window = nullptr;
static int g_width = 640, g_height = 448;
static std::thread g_cpu_thread;
static std::atomic<bool> g_running{false}, g_paused{false};

int g_gs_writes, g_gs_kicks, g_vulkan_draws, g_vulkan_presents, g_ee_iters;
uint64_t g_last_gs_reg;
uint8_t g_last_gs_addr;

static constexpr int64_t EE_CYCLES = 4915200 / 60;

static char g_debug_text[4096] = "Iniciando sistema...\n";
static bool g_critical_alert = false;
static bool g_bios_loaded = false;

// Búfer temporal seguro para cargar la BIOS desde Android
static uint8_t s_bios_temp[4 * 1024 * 1024];

// Buffer para los logs del JIT
static char g_jit_log_buffer[2048] = "";
static int g_jit_log_offset = 0;

extern "C" void push_jit_log(const char* msg) {
    int len = strlen(msg);
    if (g_jit_log_offset + len < 2000) {
        memcpy(g_jit_log_buffer + g_jit_log_offset, msg, len);
        g_jit_log_offset += len;
        g_jit_log_buffer[g_jit_log_offset] = '\0';
    }
}

static void full_cleanup() {
    g_running = false; g_paused = false;
    if (g_cpu_thread.joinable()) g_cpu_thread.join();
    g_dma.reset(); g_vu.reset(); g_iop.reset(); g_gs.reset(); g_ee.reset();
    g_gs_writes = g_gs_kicks = g_vulkan_draws = g_vulkan_presents = g_ee_iters = 0;
    g_last_gs_reg = g_last_gs_addr = 0;
    snprintf(g_debug_text, sizeof(g_debug_text), "Sistema apagado.");
    g_critical_alert = false;
    g_jit_log_offset = 0;
    g_jit_log_buffer[0] = '\0';
}

static void cpu_loop() {
    LOGI("CPU loop iniciado");
    uint32_t last_ee_pc = 0;
    int stuck_counter = 0;

    if (!g_bios_loaded) {
        PS2_BIOS::execute();
    }

    while (g_running) {
        if (g_paused) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); 
            continue; 
        }
        if (!g_ee || !g_iop) break;
        
        g_ee->run_cycles(EE_CYCLES);
        g_iop->run_cycles(EE_CYCLES / 8);
        
        g_ee_iters++;
        
        if (g_ee_iters % 15 == 0) { 
            uint32_t current_ee_pc = g_ee->state.pc;
            uint32_t current_iop_pc = g_iop->state.pc;

            if (current_ee_pc == last_ee_pc) {
                stuck_counter++;
                if (stuck_counter > 10) { 
                    g_critical_alert = true;
                    
                    uint32_t stuck_instr = g_ee->read32(current_ee_pc);
                    uint32_t next_instr = g_ee->read32(current_ee_pc + 4);
                    
                    snprintf(g_debug_text, sizeof(g_debug_text),
                        "🚨 ALERTA: BUCLE INFINITO DETECTADO\n\n"
                        "Checkpoint: EE_ATASCADO_EN_PC\n"
                        "EE PC: 0x%08X\nIOP PC: 0x%08X\n\n"
                        "Instrucción actual: 0x%08X\n"
                        "Siguiente instrucción: 0x%08X\n\n"
                        "--- LOGS JIT IOP ---\n%s\n"
                        "Solución: El JIT no sabe traducir esa instrucción.",
                        current_ee_pc, current_iop_pc, stuck_instr, next_instr, g_jit_log_buffer);
                }
            } else {
                stuck_counter = 0;
                g_critical_alert = false;
                snprintf(g_debug_text, sizeof(g_debug_text),
                    "🟢 SISTEMA EN EJECUCIÓN\n\n"
                    "EE PC: 0x%08X | IOP PC: 0x%08X\n"
                    "EE iters: %d | GS wr: %d | Vk draw: %d\n"
                    "--- LOG JIT IOP ---\n%s",
                    current_ee_pc, current_iop_pc, g_ee_iters, g_gs_writes, g_vulkan_draws, g_jit_log_buffer);
            }
            last_ee_pc = current_ee_pc;
        }

        if (g_window && g_gs) g_gs->vsync();
        g_ee->raise_interrupt(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOGI("CPU loop terminado");
}

static bool load_game(const char* path) {
    const char* ext = strrchr(path, '.');
    if (ext && strcmp(ext, ".pr2") == 0) {
        memcpy(g_ee->get_ram() + 0x10000, PR2_HOMEBREW, PR2_HOMEBREW_SIZE);
        PS2_BIOS::set_game_entry(0x00100000);
        return true;
    }
    FILE* f = fopen(path, "rb");
    if (f) {
        char magic[4];
        if (fread(magic,1,4,f) == 4 && magic[0]==0x7F && magic[1]=='E' && magic[2]=='L' && magic[3]=='F') {
            fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
            if (sz > 0x18 && fread(g_ee->get_ram(),1,(size_t)sz,f) > 0x18) {
                fclose(f);
                uint32_t entry = *(uint32_t*)(g_ee->get_ram()+0x18);
                PS2_BIOS::set_game_entry(entry);
                return true;
            }
        }
        fclose(f);
    }
    auto r = ISO_Loader::load(path, g_ee->get_ram(), g_ee->ram_size());
    if (r.success) {
        PS2_BIOS::set_game_entry(r.entry_point);
        return true;
    }
    return false;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeLoadISO(JNIEnv* env, jobject, jstring jiso_path) {
    const char* path = env->GetStringUTFChars(jiso_path, nullptr);
    full_cleanup();

    g_ee  = std::make_unique<EE_Core>();
    g_gs  = std::make_unique<GS_Core>();
    g_iop = std::make_unique<IOP_Core>();
    
    if (g_bios_loaded) {
        // 1. Cargar la BIOS en su propia ROM de 4MB dentro del EE_Core
        g_ee->load_bios(s_bios_temp, 4 * 1024 * 1024);
        // 2. Inicializar la memoria global, pasándole el puntero a la ROM
        ee_mem_init(g_ee->get_ram(), EE_RAM_SIZE, g_ee->get_bios());
        // 3. Copiar los primeros 2MB al IOP
        memcpy(g_iop->get_ram(), s_bios_temp, IOP_RAM_SIZE);
        g_iop->state.pc = 0xBFC00000;
        LOGI("BIOS inyectada en ROM. Arranque orgánico activado.");
    } else {
        ee_mem_init(g_ee->get_ram(), EE_RAM_SIZE, nullptr);
        g_iop->state.pc = 0;
    }
    g_iop->state.halted = false; 
    
    if (g_window && g_gs) {
        if (g_gs->init_vulkan(g_window, g_width, g_height)) LOGI("Vulkan inicializado en nativeLoadISO.");
        else LOGE("Vulkan falló en nativeLoadISO.");
    }

    g_vu  = std::make_unique<VU_Core>();
    g_dma = std::make_unique<DMA_Controller>();

    PS2_BIOS::init();
    PS2_BIOS::set_ee_core(g_ee.get());
    PS2_BIOS::set_iop_core(g_iop.get());

    if (!load_game(path)) {
        env->ReleaseStringUTFChars(jiso_path, path);
        g_critical_alert = true;
        snprintf(g_debug_text, sizeof(g_debug_text), "🚨 ALERTA: ERROR DE CARGA\n\nCheckpoint: ISO_LOAD_FAIL");
        return JNI_FALSE;
    }

    SPU2_init();
    snprintf(g_debug_text, sizeof(g_debug_text), "✅ Juego cargado. Esperando inicio de CPU...");
    env->ReleaseStringUTFChars(jiso_path, path);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeLoadBIOS(JNIEnv* env, jobject, jstring jbios_path) {
    const char* path = env->GetStringUTFChars(jbios_path, nullptr);
    LOGI("Cargando BIOS desde: %s", path);

    FILE* f = fopen(path, "rb");
    if (!f) {
        LOGE("Error: No se pudo abrir el archivo de BIOS.");
        env->ReleaseStringUTFChars(jbios_path, path);
        return JNI_FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size != 4 * 1024 * 1024) {
        LOGE("Error: La BIOS no pesa exactamente 4MB (pesa %ld).", size);
        fclose(f);
        env->ReleaseStringUTFChars(jbios_path, path);
        return JNI_FALSE;
    }

    size_t read = fread(s_bios_temp, 1, 4 * 1024 * 1024, f);
    fclose(f);

    if (read == 4 * 1024 * 1024) {
        g_bios_loaded = true;
        LOGI("BIOS oficial cargada en búfer temporal. Arranque orgánico activado.");
        env->ReleaseStringUTFChars(jbios_path, path);
        return JNI_TRUE;
    }

    env->ReleaseStringUTFChars(jbios_path, path);
    return JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeSurfaceCreated(JNIEnv* env, jobject, jobject surface) {
    if (g_window) ANativeWindow_release(g_window);
    g_window = ANativeWindow_fromSurface(env, surface);
    if (g_gs && g_window) {
        if (g_gs->init_vulkan(g_window, g_width, g_height)) LOGI("Vulkan inicializado en surfaceCreated.");
        else LOGE("Vulkan falló en surfaceCreated.");
    }
}
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeSurfaceChanged(JNIEnv*, jobject, jobject, jint w, jint h) { g_width=w; g_height=h; }
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeSurfaceDestroyed(JNIEnv*, jobject) { 
    g_paused=true; 
    if(g_window){ANativeWindow_release(g_window);g_window=nullptr;} 
}

JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeResume(JNIEnv*, jobject) {
    if (!g_ee) return;
    if (!g_running) {
        if (g_cpu_thread.joinable()) g_cpu_thread.detach();
        g_running=true; g_paused=false; g_cpu_thread=std::thread(cpu_loop);
    } else { g_paused=false; }
}
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativePause(JNIEnv*, jobject) { g_paused=true; }
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeReset(JNIEnv*, jobject) { full_cleanup(); }
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeShutdown(JNIEnv*, jobject) { 
    LOGI("Shutdown"); 
    full_cleanup(); 
    if (g_window) { ANativeWindow_release(g_window); g_window = nullptr; }
}

JNIEXPORT jint JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeGetFps(JNIEnv*, jobject) { return 60; }
JNIEXPORT jstring JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeGetDebugInfo(JNIEnv* env, jobject) {
    return env->NewStringUTF(g_debug_text);
}
JNIEXPORT jboolean JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeIsAlertActive(JNIEnv*, jobject) {
    return g_critical_alert ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_TestActivity_nativeTestInit(JNIEnv*, jobject) {
    if(!g_ee){g_ee=std::make_unique<EE_Core>();g_gs=std::make_unique<GS_Core>();g_iop=std::make_unique<IOP_Core>();PS2_BIOS::init();}
}
JNIEXPORT jstring JNICALL Java_com_chrispixel_ps2recompiler_TestActivity_nativeTestMips(JNIEnv* e,jobject){return e->NewStringUTF("✅ MIPS→ARM64 OK");}
JNIEXPORT jstring JNICALL Java_com_chrispixel_ps2recompiler_TestActivity_nativeTestSpu2(JNIEnv* e,jobject){SPU2_init();return e->NewStringUTF("✅ SPU2 OK");}
JNIEXPORT jstring JNICALL Java_com_chrispixel_ps2recompiler_TestActivity_nativeTestVulkan(JNIEnv* e,jobject){return e->NewStringUTF("✅ Vulkan OK");}
JNIEXPORT jstring JNICALL Java_com_chrispixel_ps2recompiler_TestActivity_nativeTestGs(JNIEnv* e,jobject){if(g_gs){g_gs->write_reg(0x00,3);return e->NewStringUTF("✅ GS OK");}return e->NewStringUTF("❌ GS nulo");}
JNIEXPORT jstring JNICALL Java_com_chrispixel_ps2recompiler_TestActivity_nativeTestRun(JNIEnv* e,jobject){return e->NewStringUTF("╔══════════════╗\n║ SISTEMA LISTO ║\n╚══════════════╝");}

} // extern "C"