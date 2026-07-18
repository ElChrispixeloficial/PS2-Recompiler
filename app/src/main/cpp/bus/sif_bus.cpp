#include "sif_bus.h"
#include <android/log.h>
#include <cstring>

#define TAG "SIF"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

static constexpr uint32_t SIF_MSCNT   = 0x1000F200;
static constexpr uint32_t SIF_M0CNT   = 0x1000F210;
static constexpr uint32_t SIF_M1CNT   = 0x1000F220;
static constexpr uint32_t SIF_SBUSCNT = 0x1000F230;
static constexpr uint32_t SIF_SBUSDCT = 0x1000F240;
static constexpr uint32_t SIF_MSFLG   = 0x1000F260;
static constexpr uint32_t SIF_SMFLG   = 0x1000F2C0;

void sif_init(SIF_Bus& sif) {
    memset(&sif, 0, sizeof(SIF_Bus));
    LOGI("SIF bus initialized");
}

uint32_t sif_read32(SIF_Bus& sif, uint32_t addr) {
    switch (addr) {
        case SIF_MSCNT:   return sif.mscnt;
        case SIF_M0CNT:   return sif.m0cnt;
        case SIF_M1CNT:   return sif.m1cnt;
        case SIF_SBUSCNT: return sif.sbuscnt;
        case SIF_SBUSDCT: return sif.sbusdct;
        case SIF_MSFLG:   return sif.msflg;
        case SIF_SMFLG:   return sif.smflg;
        default:
            LOGW("SIF read32 unhandled addr=0x%08X", addr);
            return 0;
    }
}

void sif_write32(SIF_Bus& sif, uint32_t addr, uint32_t val) {
    switch (addr) {
        case SIF_MSCNT:
            sif.mscnt = val;
            LOGI("SIF MSCNT = 0x%08X", val);
            break;
        case SIF_M0CNT:
            sif.m0cnt = val;
            LOGI("SIF M0CNT = 0x%08X", val);
            break;
        case SIF_M1CNT:
            sif.m1cnt = val;
            LOGI("SIF M1CNT = 0x%08X", val);
            break;
        case SIF_SBUSCNT:
            sif.sbuscnt = val;
            break;
        case SIF_SBUSDCT:
            sif.sbusdct = val;
            break;
        case SIF_MSFLG:
            // Writing 1 to a bit clears it (master flags)
            sif.msflg &= ~val;
            break;
        case SIF_SMFLG:
            // Slave flags set by IOP; EE writes clear
            sif.smflg &= ~val;
            break;
        default:
            LOGW("SIF write32 unhandled addr=0x%08X val=0x%08X", addr, val);
            break;
    }
}
