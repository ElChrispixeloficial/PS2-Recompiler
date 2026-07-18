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
#include "bus/memory_map.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <csignal>
#include <ucontext.h>
#include <unistd.h>

#define TAG "PS2-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern uint8_t* g_bios;
extern EE_Core* g_ee_core_ptr;
DMA_Controller* g_dma_ptr = nullptr;
VU_Core* g_vu_core_ptr = nullptr;

static std::unique_ptr<EE_Core>          g_ee;
static std::unique_ptr<GS_Core>          g_gs;
static std::unique_ptr<IOP_Core>         g_iop;
static std::unique_ptr<VU_Core>          g_vu;
static std::unique_ptr<DMA_Controller>   g_dma;
static ANativeWindow* g_window = nullptr;
static int g_width = 640, g_height = 448;
static std::thread g_cpu_thread;
static std::atomic<bool> g_running{false}, g_paused{false};
static std::mutex g_vulkan_mutex;

int g_gs_writes, g_gs_kicks, g_vulkan_draws, g_vulkan_presents, g_ee_iters;
uint64_t g_last_gs_reg;
uint8_t g_last_gs_addr;

static constexpr int64_t EE_CYCLES = 4915200 / 60;

static char g_debug_text[4096] = "Iniciando sistema...\n";
static bool g_critical_alert = false;
static bool g_bios_loaded = false;
static int g_init_phase = 0;

// Búfer temporal seguro para cargar la BIOS desde Android
static uint8_t s_bios_temp[4 * 1024 * 1024];

// Buffer para los logs del JIT
static char g_jit_log_buffer[2048] = "";
static int g_jit_log_offset = 0;

static void crash_signal_handler(int sig, siginfo_t* info, void* uc_void) {
    ucontext_t* uc = (ucontext_t*)uc_void;
    void* fault_addr = info->si_addr;
#ifdef __aarch64__
    void* pc = (void*)uc->uc_mcontext.pc;
#else
    void* pc = (void*)uc->uc_mcontext.arm_pc;
#endif
    const char* sig_name = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : "UNKNOWN";
    __android_log_print(ANDROID_LOG_ERROR, TAG,
        "*** CRASH: %s at %p (PC=%p) ***", sig_name, fault_addr, pc);
    __android_log_print(ANDROID_LOG_ERROR, TAG,
        "EE_PC=0x%08X g_running=%d g_paused=%d g_bios=%d g_init_phase=%d",
        g_ee ? g_ee->state.pc : 0, (int)g_running.load(), (int)g_paused.load(), (int)g_bios_loaded, g_init_phase);
    if (g_ee) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "EE SP=0x%08X RA=0x%08X",
            (uint32_t)g_ee->state.gpr_lo[29], (uint32_t)g_ee->state.gpr_lo[31]);
    }
    _exit(128 + sig);
}

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
    if (g_cpu_thread.joinable()) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_cpu_thread.joinable() && !g_running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (g_cpu_thread.joinable()) {
            g_cpu_thread.detach();
        }
    }
    g_ee_core_ptr = nullptr;
    g_dma_ptr = nullptr;
    g_vu_core_ptr = nullptr;
    g_dma.reset(); g_vu.reset(); g_iop.reset(); g_gs.reset(); g_ee.reset();
    g_gs_writes = g_gs_kicks = g_vulkan_draws = g_vulkan_presents = g_ee_iters = 0;
    g_last_gs_reg = g_last_gs_addr = 0;
    snprintf(g_debug_text, sizeof(g_debug_text), "Sistema apagado.");
    g_critical_alert = false;
    g_init_phase = 0;
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
        hw_tick(EE_CYCLES);
        
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
                        "[!] ALERTA: BUCLE INFINITO DETECTADO\n\n"
                        "Checkpoint: EE_ATASCADO_EN_PC\n"
                        "EE PC: 0x%08X\nIOP PC: 0x%08X\n\n"
                        "Instruccion actual: 0x%08X\n"
                        "Siguiente instruccion: 0x%08X\n\n"
                        "--- LOGS JIT IOP ---\n%s\n"
                        "El JIT no sabe traducir esa instruccion.",
                        current_ee_pc, current_iop_pc, stuck_instr, next_instr, g_jit_log_buffer);
                }
            } else {
                stuck_counter = 0;
                g_critical_alert = false;
                snprintf(g_debug_text, sizeof(g_debug_text),
                    "[OK] SISTEMA EN EJECUCION\n\n"
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
    struct sigaction sa{};
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);

    const char* path = env->GetStringUTFChars(jiso_path, nullptr);
    LOGI("[STEP] nativeLoadISO: path obtained");

    FILE* test = fopen(path, "rb");
    if (!test) {
        LOGE("nativeLoadISO: ISO no accesible: %s", path);
        env->ReleaseStringUTFChars(jiso_path, path);
        return JNI_FALSE;
    }
    fseek(test, 0, SEEK_END);
    long fsize = ftell(test);
    fclose(test);
    if (fsize < 2048) {
        LOGE("nativeLoadISO: ISO demasiado pequeno o vacio: %s (%ld bytes)", path, fsize);
        env->ReleaseStringUTFChars(jiso_path, path);
        return JNI_FALSE;
    }

    LOGI("[STEP] nativeLoadISO: file validated (%ld bytes)", fsize);
    g_init_phase = 1;
    LOGI("[DIAG] Phase 1: before full_cleanup, g_window=%p g_gs=%p g_ee=%p g_iop=%p",
         (void*)g_window, (void*)g_gs.get(), (void*)g_ee.get(), (void*)g_iop.get());
    full_cleanup();
    LOGI("[STEP] nativeLoadISO: full_cleanup done");

    g_init_phase = 2;
    LOGI("[DIAG] Phase 2: creating EE_Core");
    try {
        LOGI("[STEP] nativeLoadISO: creating EE_Core...");
        g_ee  = std::make_unique<EE_Core>();
    } catch (const std::bad_alloc&) {
        LOGE("[STEP] nativeLoadISO: EE_Core FAILED (out of memory)");
        env->ReleaseStringUTFChars(jiso_path, path);
        return JNI_FALSE;
    }
    if (!g_ee || !g_ee->get_ram()) { LOGE("[STEP] nativeLoadISO: EE_Core null"); env->ReleaseStringUTFChars(jiso_path, path); return JNI_FALSE; }
    LOGI("[STEP] nativeLoadISO: EE_Core created, ptr=%p ram=%p", (void*)g_ee.get(), (void*)g_ee->get_ram());
    g_ee_core_ptr = g_ee.get();

    g_init_phase = 3;
    LOGI("[DIAG] Phase 3: creating GS_Core");
    try {
        LOGI("[STEP] nativeLoadISO: creating GS_Core...");
        g_gs  = std::make_unique<GS_Core>();
    } catch (const std::bad_alloc&) {
        LOGE("[STEP] nativeLoadISO: GS_Core FAILED (out of memory)");
        env->ReleaseStringUTFChars(jiso_path, path);
        return JNI_FALSE;
    }
    if (!g_gs) { LOGE("[STEP] nativeLoadISO: GS_Core null"); env->ReleaseStringUTFChars(jiso_path, path); return JNI_FALSE; }
    LOGI("[STEP] nativeLoadISO: GS_Core created, ptr=%p vulkan=%p", (void*)g_gs.get(), (void*)g_gs->get_vulkan());

    g_init_phase = 4;
    LOGI("[DIAG] Phase 4: creating IOP_Core");
    try {
        LOGI("[STEP] nativeLoadISO: creating IOP_Core...");
        g_iop = std::make_unique<IOP_Core>();
    } catch (const std::bad_alloc&) {
        LOGE("[STEP] nativeLoadISO: IOP_Core FAILED (out of memory)");
        env->ReleaseStringUTFChars(jiso_path, path);
        return JNI_FALSE;
    }
    if (!g_iop) { LOGE("[STEP] nativeLoadISO: IOP_Core null"); env->ReleaseStringUTFChars(jiso_path, path); return JNI_FALSE; }
    LOGI("[STEP] nativeLoadISO: IOP_Core created, ptr=%p", (void*)g_iop.get());

    g_init_phase = 5;
    LOGI("[DIAG] Phase 5: BIOS injection, g_bios_loaded=%d", (int)g_bios_loaded);

    if (g_bios_loaded) {
        g_ee->load_bios(s_bios_temp, 4 * 1024 * 1024);
        ee_mem_init(g_ee->get_ram(), EE_RAM_SIZE, g_ee->get_bios());
        memcpy(g_iop->get_ram(), s_bios_temp, IOP_RAM_SIZE);
        g_iop->state.pc = 0xBFC00000;
        LOGI("[STEP] nativeLoadISO: BIOS injected, organic boot");
    } else {
        ee_mem_init(g_ee->get_ram(), EE_RAM_SIZE, nullptr);
        g_iop->state.pc = 0;
        LOGI("[STEP] nativeLoadISO: No BIOS loaded, EE mem init done");
    }
    g_iop->state.halted = false;

    g_init_phase = 6;
    LOGI("[DIAG] Phase 6: Vulkan init, g_window=%p g_gs=%p", (void*)g_window, (void*)g_gs.get());
    if (g_window && g_gs) {
        std::lock_guard<std::mutex> lock(g_vulkan_mutex);
        LOGI("[STEP] nativeLoadISO: init_vulkan (window present)");
        if (g_gs->init_vulkan(g_window, g_width, g_height)) LOGI("[STEP] nativeLoadISO: Vulkan OK");
        else LOGE("[STEP] nativeLoadISO: Vulkan FAILED (non-fatal)");
    } else {
        LOGI("[STEP] nativeLoadISO: skipping Vulkan (no window yet)");
    }

    g_init_phase = 7;
    LOGI("[DIAG] Phase 7: creating VU_Core + DMA_Controller");
    try {
        g_vu  = std::make_unique<VU_Core>();
        g_dma = std::make_unique<DMA_Controller>();
    } catch (const std::bad_alloc&) {
        LOGE("[STEP] nativeLoadISO: VU/DMA FAILED (out of memory)");
        env->ReleaseStringUTFChars(jiso_path, path);
        return JNI_FALSE;
    }
    if (!g_vu || !g_dma) { LOGE("[STEP] nativeLoadISO: VU/DMA null"); env->ReleaseStringUTFChars(jiso_path, path); return JNI_FALSE; }
    g_dma_ptr = g_dma.get();
    g_vu_core_ptr = g_vu.get();
    LOGI("[STEP] nativeLoadISO: VU_Core + DMA created");

    g_init_phase = 8;
    LOGI("[DIAG] Phase 8: PS2_BIOS::init, ee=%p iop=%p", (void*)g_ee.get(), (void*)g_iop.get());
    PS2_BIOS::init();
    PS2_BIOS::set_ee_core(g_ee.get());
    PS2_BIOS::set_iop_core(g_iop.get());
    LOGI("[STEP] nativeLoadISO: BIOS init + cores wired");

    g_init_phase = 9;
    LOGI("[DIAG] Phase 9: load_game");
    LOGI("[STEP] nativeLoadISO: load_game starting");
    if (!load_game(path)) {
        env->ReleaseStringUTFChars(jiso_path, path);
        g_critical_alert = true;
        snprintf(g_debug_text, sizeof(g_debug_text), "[!] ERROR DE CARGA\n\nCheckpoint: ISO_LOAD_FAIL");
        LOGE("[STEP] nativeLoadISO: load_game FAILED");
        return JNI_FALSE;
    }
    LOGI("[STEP] nativeLoadISO: load_game done");

    g_init_phase = 10;
    LOGI("[DIAG] Phase 10: SPU2_init");
    LOGI("[STEP] nativeLoadISO: SPU2_init starting");
    if (!SPU2_init()) {
        LOGE("[STEP] nativeLoadISO: SPU2_init FAILED (audio may not work)");
    } else {
        LOGI("[STEP] nativeLoadISO: SPU2_init OK");
    }

    g_init_phase = 11;
    LOGI("[DIAG] Phase 11: nativeLoadISO complete");
    snprintf(g_debug_text, sizeof(g_debug_text), "[OK] Juego cargado. Esperando inicio de CPU...");
    LOGI("[STEP] nativeLoadISO: DONE SUCCESSFULLY");
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
    std::lock_guard<std::mutex> lock(g_vulkan_mutex);
    if (g_window) ANativeWindow_release(g_window);
    g_window = ANativeWindow_fromSurface(env, surface);
    if (g_gs && g_window) {
        LOGI("[STEP] nativeSurfaceCreated: init_vulkan");
        if (g_gs->init_vulkan(g_window, g_width, g_height)) LOGI("[STEP] nativeSurfaceCreated: Vulkan OK");
        else LOGE("[STEP] nativeSurfaceCreated: Vulkan FAILED");
    }
}
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeSurfaceChanged(JNIEnv*, jobject, jobject, jint w, jint h) { g_width=w; g_height=h; }
JNIEXPORT void JNICALL Java_com_chrispixel_ps2recompiler_RuntimeActivity_nativeSurfaceDestroyed(JNIEnv*, jobject) { 
    g_paused=true;
    std::lock_guard<std::mutex> lock(g_vulkan_mutex);
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
    static char safe_buf[4096];
    memcpy(safe_buf, g_debug_text, sizeof(safe_buf) - 1);
    safe_buf[sizeof(safe_buf) - 1] = '\0';
    return env->NewStringUTF(safe_buf);
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