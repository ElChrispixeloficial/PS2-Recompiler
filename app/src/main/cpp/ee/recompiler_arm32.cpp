// ee/recompiler_arm32.cpp
// ARM32 (Thumb2) recompiler — stub for 32-bit Android targets.
// The primary target is ARM64; this file compiles but emits nothing.
#include <cstdint>
#include <android/log.h>

#define TAG "ARM32_Recomp"

uint8_t* arm32_recompile_block(uint32_t guest_pc, const uint8_t* /*ps2_ram*/) {
    __android_log_print(ANDROID_LOG_WARN, TAG,
        "ARM32 recompiler stub called for PC=%08X — not implemented", guest_pc);
    return nullptr;
}
