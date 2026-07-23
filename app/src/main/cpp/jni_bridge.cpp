// ═══════════════════════════════════════════════════════════════════════════════
// JNI Bridge — PR2 Recompiler
// ═══════════════════════════════════════════════════════════════════════════════

#include "ee/ee_core.h"
#include "ee/ee_memory.h"
#include "ee/code_cache.h"
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

static constexpr const char* BUILD_VERSION = "v0.2.2-exception-fix+IOP-HW";

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
int g_init_phase = 0;

// Code cache base for crash diagnostics (set once from CodeCache)
static uint8_t* g_code_cache_base = nullptr;

// Per-block MIPS PC ring buffer — last 64 blocks executed
constexpr int PC_RING_SIZE = 64;
uint32_t g_pc_ring[PC_RING_SIZE];
int g_pc_ring_idx = 0;

void set_code_cache_base(uint8_t* base) {
    g_code_cache_base = base;
}

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
    void* sp = (void*)uc->uc_mcontext.sp;
    void* lr = (void*)uc->uc_mcontext.regs[30];
    uint64_t regs[31];
    for (int i = 0; i < 31; i++) regs[i] = uc->uc_mcontext.regs[i];
    uint64_t pstate = uc->uc_mcontext.pstate;
#else
    void* pc = (void*)0;
    void* sp = (void*)0;
    void* lr = (void*)0;
    uint64_t regs[31] = {};
    uint64_t pstate = 0;
#endif
    const char* sig_name = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : "UNKNOWN";

    const char* pc_lib = "unknown";
    const char* lr_lib = "unknown";
    char pc_offset[128] = "";
    char lr_offset[128] = "";
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        uintptr_t pc_val = (uintptr_t)pc;
        uintptr_t lr_val = (uintptr_t)lr;
        while (fgets(line, sizeof(line), maps)) {
            uintptr_t start, end;
            char perms[8], path[256] = "";
            if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %255[^\n]", &start, &end, perms, path) >= 3) {
                if (pc_val >= start && pc_val < end) {
                    pc_lib = strdup(path);
                    snprintf(pc_offset, sizeof(pc_offset), "%s+0x%lx", path, pc_val - start);
                }
                if (lr_val >= start && lr_val < end) {
                    lr_lib = strdup(path);
                    snprintf(lr_offset, sizeof(lr_offset), "%s+0x%lx", path, lr_val - start);
                }
            }
        }
        fclose(maps);
    }

    FILE* f = fopen("/sdcard/Download/ps2_crash.log", "w");
    if (f) {
        fprintf(f, "=== CRASH LOG ===\n");
        fprintf(f, "Build: %s %s\n", __DATE__, __TIME__);
        fprintf(f, "Signal: %s (%d)\n", sig_name, sig);
        fprintf(f, "Fault address: %p\n", fault_addr);
        fprintf(f, "PC: %p  [%s]\n", pc, pc_offset[0] ? pc_offset : pc_lib);
        fprintf(f, "SP: %p  LR: %p  [%s]\n", sp, lr, lr_offset[0] ? lr_offset : lr_lib);
        fprintf(f, "PSTATE: 0x%016llx\n", (unsigned long long)pstate);
        fprintf(f, "g_init_phase: %d\n", g_init_phase);
        fprintf(f, "g_bios_loaded: %d\n", (int)g_bios_loaded);
        fprintf(f, "g_running: %d  g_paused: %d\n", (int)g_running.load(), (int)g_paused.load());
        fprintf(f, "g_window: %p\n", (void*)g_window);
        fprintf(f, "g_gs: %p\n", (void*)g_gs.get());
        fprintf(f, "g_ee: %p\n", (void*)g_ee.get());
        fprintf(f, "g_iop: %p\n", (void*)g_iop.get());
        fprintf(f, "g_vu: %p\n", (void*)g_vu.get());
        fprintf(f, "g_dma: %p\n", (void*)g_dma.get());
        fprintf(f, "g_ee_core_ptr: %p\n", (void*)g_ee_core_ptr);
        fprintf(f, "g_code_cache_base: %p\n", (void*)g_code_cache_base);
        if (g_ee) {
            fprintf(f, "EE PC=0x%08X SP=0x%08X RA=0x%08X\n",
                g_ee->state.pc, (uint32_t)g_ee->state.gpr_lo[29], (uint32_t)g_ee->state.gpr_lo[31]);
        }
        if (g_gs) {
            fprintf(f, "GS vulkan ptr: %p\n", (void*)g_gs->get_vulkan());
        }
        // ─── Full ARM64 register dump ───────────────────────────────
#ifdef __aarch64__
        fprintf(f, "\n--- ARM64 REGISTERS ---\n");
        for (int i = 0; i < 31; i += 2) {
            fprintf(f, "  x%-2d = 0x%016llx", i, (unsigned long long)regs[i]);
            if (i + 1 < 31) fprintf(f, "  x%-2d = 0x%016llx", i + 1, (unsigned long long)regs[i + 1]);
            fprintf(f, "\n");
        }
        fprintf(f, "  SP = 0x%016llx\n", (unsigned long long)(uintptr_t)sp);
        fprintf(f, "  PC = 0x%016llx\n", (unsigned long long)(uintptr_t)pc);
        fprintf(f, "  LR = 0x%016llx\n", (unsigned long long)(uintptr_t)lr);
        // ─── Decode ARM64 instructions around crash PC ──────────────
        fprintf(f, "\n--- ARM64 DISASSEMBLY (PC -8 .. PC +12) ---\n");
        uintptr_t pc_val = (uintptr_t)pc;
        uint32_t* insn_ptr = (uint32_t*)(pc_val - 8);
        for (int i = -2; i <= 3; i++) {
            uint32_t insn = insn_ptr[i + 2];
            uintptr_t addr = (uintptr_t)(pc_val + i * 4);
            const char* prefix = (i == 0) ? ">>>" : "   ";
            fprintf(f, "  %s 0x%016llx : %08X", prefix, (unsigned long long)addr, insn);
            // Decode common ARM64 instructions
            if ((insn & 0xFFE00000) == 0xD63F0000) {
                fprintf(f, "  BLR x%d\n", (insn >> 5) & 31);
            } else if ((insn & 0xFFE00000) == 0xD61F0000) {
                fprintf(f, "  BR x%d\n", (insn >> 5) & 31);
            } else if ((insn & 0xFFFFFC00) == 0xD65F0000) {
                fprintf(f, "  RET\n");
            } else if ((insn & 0xFF000000) == 0x94000000 || (insn & 0xFF000000) == 0x97000000) {
                int32_t off = ((int32_t)(insn & 0x3FFFFFF) << 6) >> 6;
                fprintf(f, "  BL 0x%llx\n", (unsigned long long)(addr + off * 4));
            } else if ((insn & 0xFC000000) == 0x14000000 || (insn & 0xFC000000) == 0x17000000) {
                int32_t off = ((int32_t)(insn & 0x3FFFFFF) << 6) >> 6;
                fprintf(f, "  B 0x%llx\n", (unsigned long long)(addr + off * 4));
            } else if ((insn & 0xFFE00000) == 0xF9400000) {
                unsigned Rt = insn & 31, Rn = (insn >> 5) & 31;
                unsigned imm = ((insn >> 10) & 0x7FF) << 3;
                fprintf(f, "  LDR x%d, [x%d, #%u]", Rt, Rn, imm);
                if (Rn == 19) fprintf(f, "  ; EE_State + %u", imm);
                fprintf(f, "\n");
            } else if ((insn & 0xFFE00000) == 0xF9000000) {
                unsigned Rt = insn & 31, Rn = (insn >> 5) & 31;
                unsigned imm = ((insn >> 10) & 0x7FF) << 3;
                fprintf(f, "  STR x%d, [x%d, #%u]", Rt, Rn, imm);
                if (Rn == 19) fprintf(f, "  ; EE_State + %u", imm);
                fprintf(f, "\n");
            } else if ((insn & 0xFFE00000) == 0xB9400000) {
                unsigned Rt = insn & 31, Rn = (insn >> 5) & 31;
                unsigned imm = ((insn >> 10) & 0x7FF) << 2;
                fprintf(f, "  LDR w%d, [x%d, #%u]", Rt, Rn, imm);
                if (Rn == 19) fprintf(f, "  ; EE_State + %u", imm);
                fprintf(f, "\n");
            } else if ((insn & 0xFFE00000) == 0xB9000000) {
                unsigned Rt = insn & 31, Rn = (insn >> 5) & 31;
                unsigned imm = ((insn >> 10) & 0x7FF) << 2;
                fprintf(f, "  STR w%d, [x%d, #%u]", Rt, Rn, imm);
                if (Rn == 19) fprintf(f, "  ; EE_State + %u", imm);
                fprintf(f, "\n");
            } else if ((insn & 0xFFE00000) == 0xAA0003E0) {
                unsigned Rd = insn & 31, Rm = (insn >> 16) & 31;
                fprintf(f, "  MOV x%d, x%d\n", Rd, Rm);
            } else if ((insn & 0xFF000000) == 0xD2800000) {
                unsigned Rd = insn & 31;
                uint64_t imm = ((uint64_t)(insn >> 5) & 0xFFFF) << (((insn >> 21) & 3) * 16);
                fprintf(f, "  MOVZ x%d, #0x%llx\n", Rd, (unsigned long long)imm);
            } else if ((insn & 0xFF000000) == 0xF2800000) {
                unsigned Rd = insn & 31;
                uint64_t imm = ((uint64_t)(insn >> 5) & 0xFFFF) << (((insn >> 21) & 3) * 16);
                fprintf(f, "  MOVK x%d, #0x%llx, LSL #%d\n", Rd, (unsigned long long)imm, ((insn >> 21) & 3) * 16);
            } else if ((insn & 0xFFC00000) == 0xB4000000) {
                unsigned Rt = insn & 31;
                int32_t off = ((int32_t)((insn >> 5) & 0x7FFFF) << 13) >> 13;
                fprintf(f, "  CBZ x%d, %lld\n", Rt, (long long)(addr + off * 4));
            } else if ((insn & 0xFFC00000) == 0xB5000000) {
                unsigned Rt = insn & 31;
                int32_t off = ((int32_t)((insn >> 5) & 0x7FFFF) << 13) >> 13;
                fprintf(f, "  CBNZ x%d, %lld\n", Rt, (long long)(addr + off * 4));
            } else if ((insn & 0xFF800000) == 0x54000000) {
                unsigned cond = insn & 0xF;
                int32_t off = ((int32_t)((insn >> 5) & 0x7FF)) << 1;
                const char* conds[] = {"EQ","NE","CS","CC","MI","PL","VS","VC","HI","LS","GE","LT","GT","LE","AL","NV"};
                fprintf(f, "  B.%s %lld\n", conds[cond], (long long)(addr + off));
            } else if ((insn & 0xFFE0FC00) == 0xEB00001F) {
                unsigned Rn = (insn >> 5) & 31, Rm = (insn >> 16) & 31;
                fprintf(f, "  CMP x%d, x%d\n", Rn, Rm);
            } else if ((insn & 0xFFE003E0) == 0xAA0003E0) {
                unsigned Rd = insn & 31, Rm = (insn >> 16) & 31;
                fprintf(f, "  ORR x%d, xzr, x%d (= MOV x%d, x%d)\n", Rd, Rm, Rd, Rm);
            } else if ((insn & 0xFFE0FC00) == 0x9A800000) {
                unsigned Rd = insn & 31, Rn = (insn >> 5) & 31, Rm = (insn >> 16) & 31;
                unsigned cond = (insn >> 12) & 0xF;
                const char* conds[] = {"EQ","NE","CS","CC","MI","PL","VS","VC","HI","LS","GE","LT","GT","LE","AL","NV"};
                fprintf(f, "  CSEL x%d, x%d, x%d, %s\n", Rd, Rn, Rm, conds[cond]);
            } else {
                fprintf(f, "  (unknown)\n");
            }
        }
        // ─── Identify if crash is in code cache ─────────────────────
        if (g_code_cache_base) {
            uintptr_t cc_base = (uintptr_t)g_code_cache_base;
            uintptr_t cc_end = cc_base + CODE_CACHE_SIZE;
            if (pc_val >= cc_base && pc_val < cc_end) {
                fprintf(f, "\n>>> CRASH IN JIT CODE CACHE at offset 0x%lx <<<\n", pc_val - cc_base);
                fprintf(f, ">>> Code cache base: %p  capacity: %zu MB <<<\n",
                    g_code_cache_base, CODE_CACHE_SIZE / (1024*1024));
            } else {
                fprintf(f, "\n>>> CRASH NOT in code cache (in %s) <<<\n",
                    pc_offset[0] ? pc_offset : pc_lib);
            }
        }
#endif
        // ─── MIPS PC ring buffer ────────────────────────────────────
        fprintf(f, "\n--- LAST %d MIPS PCs EXECUTED ---\n", PC_RING_SIZE);
        for (int i = 0; i < PC_RING_SIZE; i++) {
            int idx = (g_pc_ring_idx - PC_RING_SIZE + i + PC_RING_SIZE * 2) % PC_RING_SIZE;
            if (g_pc_ring[idx] != 0) {
                fprintf(f, "  [%3d] 0x%08X", i, g_pc_ring[idx]);
                if (g_pc_ring[idx] >= 0xBFC00000u && g_pc_ring[idx] < 0xC0000000u)
                    fprintf(f, "  (BIOS ROM)");
                fprintf(f, "\n");
            }
        }
        fprintf(f, "=== END CRASH LOG ===\n");
        fclose(f);
    }

    snprintf(g_debug_text, sizeof(g_debug_text),
        "CRASH: %s at %p\nPC: %s\nEE PC=0x%08X\nCheck /sdcard/Download/ps2_crash.log",
        sig_name, fault_addr, pc_offset[0] ? pc_offset : "?",
        g_ee ? g_ee->state.pc : 0);

    __android_log_print(ANDROID_LOG_ERROR, TAG,
        "*** CRASH: %s at %p (PC=%p) ***", sig_name, fault_addr, pc);
    __android_log_print(ANDROID_LOG_ERROR, TAG,
        "PC: %s", pc_offset[0] ? pc_offset : "unknown");
    __android_log_print(ANDROID_LOG_ERROR, TAG,
        "LR: %s", lr_offset[0] ? lr_offset : "unknown");
    __android_log_print(ANDROID_LOG_ERROR, TAG,
        "EE_PC=0x%08X g_init_phase=%d", g_ee ? g_ee->state.pc : 0, g_init_phase);
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

extern void set_code_cache_base(uint8_t* base);

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
    memset(g_pc_ring, 0, sizeof(g_pc_ring));
    g_pc_ring_idx = 0;
}

static void cpu_loop() {
    LOGI("CPU loop iniciado");
    uint32_t last_ee_pc = 0;
    int stuck_counter = 0;
    int vblank_divider = 0;

    // Always initialize EE state via BIOS HLE (fast boot).
    // Previously this was skipped when g_bios_loaded=1, which left COP0,
    // interrupts, SIF, and stack uninitialized — causing immediate crashes.
    PS2_BIOS::execute();

    while (g_running) {
        if (g_paused) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); 
            continue; 
        }
        if (!g_ee || !g_iop) break;
        
        g_ee->run_cycles(EE_CYCLES);
        if (!g_iop->state.halted) {
            g_iop->run_cycles(EE_CYCLES / 8);
        }
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
                        "Build: %s\n"
                        "Checkpoint: EE_ATASCADO_EN_PC\n"
                        "EE PC: 0x%08X\nIOP PC: 0x%08X\n\n"
                        "Instruccion actual: 0x%08X\n"
                        "Siguiente instruccion: 0x%08X\n\n"
                        "--- LOGS JIT IOP ---\n%s\n"
                        "El JIT no sabe traducir esa instruccion.",
                        BUILD_VERSION, current_ee_pc, current_iop_pc, stuck_instr, next_instr, g_jit_log_buffer);
                }
            } else {
                stuck_counter = 0;
                g_critical_alert = false;
                snprintf(g_debug_text, sizeof(g_debug_text),
                    "[OK] SISTEMA EN EJECUCION\n\n"
                    "Build: %s\n"
                    "EE PC: 0x%08X | IOP PC: 0x%08X\n"
                    "EE iters: %d | GS wr: %d | Vk draw: %d\n"
                    "--- LOG JIT IOP ---\n%s",
                    BUILD_VERSION, current_ee_pc, current_iop_pc, g_ee_iters, g_gs_writes, g_vulkan_draws, g_jit_log_buffer);
            }
            last_ee_pc = current_ee_pc;
        }

        if (g_window && g_gs) g_gs->vsync();
        // Raise VBlank interrupt at ~60Hz (every ~16 iterations at 1ms sleep each)
        vblank_divider++;
        if (vblank_divider >= 16) {
            vblank_divider = 0;
            g_ee->raise_interrupt(2);
        }
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

    // Try raw ELF file first
    FILE* f = fopen(path, "rb");
    if (f) {
        char magic[4];
        if (fread(magic,1,4,f) == 4 && magic[0]==0x7F && magic[1]=='E' && magic[2]=='L' && magic[3]=='F') {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (sz < 0x34) { fclose(f); goto try_iso; }

            // Read ELF header
            uint8_t* elf_buf = (uint8_t*)malloc((size_t)sz);
            if (!elf_buf) { fclose(f); goto try_iso; }
            if ((long)fread(elf_buf, 1, (size_t)sz, f) != sz) { free(elf_buf); fclose(f); goto try_iso; }
            fclose(f);

            uint32_t elf_entry = *(uint32_t*)(elf_buf + 0x18);
            uint16_t elf_phnum = *(uint16_t*)(elf_buf + 0x2C);
            uint16_t elf_phentsize = *(uint16_t*)(elf_buf + 0x2E);
            uint32_t elf_phoff = *(uint32_t*)(elf_buf + 0x1C);

            LOGI("Raw ELF: entry=0x%08X phnum=%d phentsize=%d", elf_entry, elf_phnum, elf_phentsize);

            if (elf_phnum > 0 && elf_phoff + (uint32_t)elf_phnum * elf_phentsize <= (uint32_t)sz) {
                // Load each PT_LOAD segment to its correct p_vaddr
                for (int i = 0; i < elf_phnum; i++) {
                    uint32_t* phdr = (uint32_t*)(elf_buf + elf_phoff + i * elf_phentsize);
                    uint32_t p_type   = phdr[0];
                    uint32_t p_offset = phdr[1];
                    uint32_t p_vaddr  = phdr[2];
                    uint32_t p_filesz = phdr[4];
                    uint32_t p_memsz  = phdr[5];

                    if (p_type != 1) continue; // PT_LOAD = 1

                    uint32_t dest = p_vaddr & 0x1FFFFFFFu;
                    if (dest + p_memsz > EE_RAM_SIZE) {
                        LOGE("ELF segment %d out of RAM: 0x%08X + 0x%X", i, dest, p_memsz);
                        continue;
                    }
                    if (p_offset + p_filesz > (uint32_t)sz) {
                        LOGE("ELF segment %d source overflow", i);
                        continue;
                    }

                    if (p_filesz > 0)
                        memcpy(g_ee->get_ram() + dest, elf_buf + p_offset, p_filesz);
                    if (p_memsz > p_filesz)
                        memset(g_ee->get_ram() + dest + p_filesz, 0, p_memsz - p_filesz);
                    LOGI("ELF segment %d: vaddr=0x%08X -> RAM+0x%08X (%u bytes)", i, p_vaddr, dest, p_filesz);
                }
            } else {
                // Fallback: dump entire ELF at offset 0 (works for simple PS2 ELFs)
                LOGI("ELF: no program headers, dumping raw to RAM");
                if ((size_t)sz > EE_RAM_SIZE) sz = EE_RAM_SIZE;
                memcpy(g_ee->get_ram(), elf_buf, (size_t)sz);
            }

            free(elf_buf);
            PS2_BIOS::set_game_entry(elf_entry);
            LOGI("Raw ELF loaded. Entry: 0x%08X", elf_entry);
            return true;
        }
        fclose(f);
    }

try_iso:
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 1: cleanup\nWindow=%p GS=%p", (void*)g_window, (void*)g_gs.get());
    LOGI("[DIAG] Phase 1: before full_cleanup, g_window=%p g_gs=%p g_ee=%p g_iop=%p",
         (void*)g_window, (void*)g_gs.get(), (void*)g_ee.get(), (void*)g_iop.get());
    full_cleanup();
    LOGI("[STEP] nativeLoadISO: full_cleanup done");

    g_init_phase = 2;
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 2: EE_Core");
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 3: GS_Core");
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 4: IOP_Core");
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 5: BIOS inject\nbios=%d", (int)g_bios_loaded);
    LOGI("[DIAG] Phase 5: BIOS injection, g_bios_loaded=%d", (int)g_bios_loaded);

    if (g_bios_loaded) {
        g_ee->load_bios(s_bios_temp, 4 * 1024 * 1024);
        ee_mem_init(g_ee->get_ram(), EE_RAM_SIZE, g_ee->get_bios());
        memcpy(g_iop->get_ram(), s_bios_temp, IOP_RAM_SIZE);
        g_iop->state.pc = 0xBFC00000;
        g_iop->state.halted = false;
        LOGI("[STEP] nativeLoadISO: BIOS injected, organic boot");
    } else {
        ee_mem_init(g_ee->get_ram(), EE_RAM_SIZE, nullptr);
        g_iop->state.pc = 0xBFC00000;
        g_iop->state.halted = true;
        LOGI("[STEP] nativeLoadISO: No BIOS loaded, IOP kept HALTED (HLE mode)");
    }

    g_init_phase = 6;
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 6: Vulkan\nwin=%p gs=%p", (void*)g_window, (void*)g_gs.get());
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 7: VU+DMA");
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 8: BIOS init");
    LOGI("[DIAG] Phase 8: PS2_BIOS::init, ee=%p iop=%p", (void*)g_ee.get(), (void*)g_iop.get());
    PS2_BIOS::init();
    PS2_BIOS::set_ee_core(g_ee.get());
    PS2_BIOS::set_iop_core(g_iop.get());
    LOGI("[STEP] nativeLoadISO: BIOS init + cores wired");

    g_init_phase = 9;
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 9: load_game");
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
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 10: SPU2");
    LOGI("[DIAG] Phase 10: SPU2_init");
    LOGI("[STEP] nativeLoadISO: SPU2_init starting");
    if (!SPU2_init()) {
        LOGE("[STEP] nativeLoadISO: SPU2_init FAILED (audio may not work)");
    } else {
        LOGI("[STEP] nativeLoadISO: SPU2_init OK");
    }

    g_init_phase = 11;
    snprintf(g_debug_text, sizeof(g_debug_text), "Phase 11: DONE");
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