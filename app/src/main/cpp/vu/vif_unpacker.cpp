#include "vif_unpacker.h"
#include "vu_core.h"
#include <cstring>
#include <android/log.h>

#define TAG "VIF"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static constexpr int VU1_DATA_SIZE = 16 * 1024;

static VIF_Unpacker g_vif0;
static VIF_Unpacker g_vif1;

void VIF_Unpacker::reset() {
    vif0_stat = 0;
    vif1_stat = 0;
    fifo_pos = 0;
    fifo_size = 0;
    memset(vif1_fifo, 0, sizeof(vif1_fifo));
    itops[0] = itops[1] = 0;
    top[0] = top[1] = 0;
    r0[0] = r0[1] = 0;
}

uint32_t VIF_Unpacker::read_stat(int vif_idx) {
    return vif_idx == 0 ? g_vif0.vif0_stat : g_vif1.vif1_stat;
}

void VIF_Unpacker::write_stat(int vif_idx, uint32_t val) {
    if (vif_idx == 0) g_vif0.vif0_stat = val;
    else g_vif1.vif1_stat = val;
}

uint32_t VIF_Unpacker::read_fifo(int vif_idx) {
    VIF_Unpacker& vif = vif_idx == 0 ? g_vif0 : g_vif1;
    if (vif.fifo_pos >= vif.fifo_size) {
        LOGW("VIF%d FIFO read underflow", vif_idx);
        return 0;
    }
    return vif.vif1_fifo[vif.fifo_pos++];
}

void VIF_Unpacker::write_top(int vif_idx, uint32_t val) {
    if (vif_idx == 0) g_vif0.top[0] = val;
    else g_vif1.top[1] = val;
}

uint32_t VIF_Unpacker::read_top(int vif_idx) {
    return vif_idx == 0 ? g_vif0.top[0] : g_vif1.top[1];
}

bool VIF_Unpacker::feed_packet(uint32_t tag, const uint32_t* data, int qwc, VU_Core& vu, int vif_idx) {
    uint32_t nloop = tag & 0x7FFF;
    bool e_bit = (tag >> 15) & 1;
    uint32_t cmd = (tag >> 24) & 0xFF;
    uint32_t imm = (tag >> 16) & 0xFF;
    bool flg = (tag >> 22) & 1;
    (void)flg;

    LOGD("VIF%d feed: tag=0x%08X nloop=%u cmd=0x%02X imm=0x%02X qwc=%d",
         vif_idx, tag, nloop, cmd, imm, qwc);

    if (cmd >= 0x60 && cmd <= 0x7F) {
        unpack_data(cmd, data, qwc * 4, vu, vif_idx);
        return true;
    }

    switch (cmd) {
        case 0x00: // NOP
            LOGD("VIF%d NOP", vif_idx);
            break;

        case 0x20: // FLUSHA
            LOGD("VIF%d FLUSHA", vif_idx);
            fifo_pos = 0;
            fifo_size = 0;
            break;

        case 0x21: // FLUSH
            LOGD("VIF%d FLUSH", vif_idx);
            fifo_pos = 0;
            fifo_size = 0;
            break;

        case 0x34: // MSCAL - Micro Subroutine Call
            LOGD("VIF%d MSCAL addr=0x%03X", vif_idx, nloop * 8);
            if (!vu.is_running(vif_idx)) {
                vu.start(vif_idx, nloop * 8);
            }
            break;

        case 0x35: // MSCNT - Micro Subroutine Call (decrement counter)
            LOGD("VIF%d MSCNT", vif_idx);
            if (!vu.is_running(vif_idx)) {
                uint32_t addr = vu.vi[vif_idx == 0 ? 0 : 1][15]; // VI15 = TOP used as pc hint
                vu.start(vif_idx, addr);
            }
            break;

        case 0x37: // MSCALL - Micro Subroutine Call (fixed addr)
            LOGD("VIF%d MSCALL", vif_idx);
            if (!vu.is_running(vif_idx)) {
                vu.start(vif_idx, 0);
            }
            break;

        case 0x30: // DIRECT
        case 0x31: // DIRECTHL
            LOGD("VIF%d DIRECT%s qwc=%d",
                 vif_idx, cmd == 0x31 ? "HL" : "", qwc);
            // DIRECT transfers data directly to VU memory via GIF path
            // For now, store in FIFO for later processing
            if (data && qwc > 0) {
                int words = qwc * 4;
                if (fifo_pos + words <= 256) {
                    memcpy(&vif1_fifo[fifo_pos], data, words * sizeof(uint32_t));
                    fifo_size = fifo_pos + words;
                    fifo_pos = 0;
                }
            }
            break;

        case 0x33: // ITOP - Set ITOPS register
            LOGD("VIF%d ITOP val=0x%04X", vif_idx, nloop & 0x7FFF);
            itops[vif_idx] = nloop & 0x7FFF;
            break;

        case 0x01: // STCYCL - Set Cycle
            LOGD("VIF%d STCYCL cl=%u wl=%u", vif_idx, imm & 0xFF, (imm >> 8) & 0xFF);
            break;

        case 0x02: // OFFSET - Set OFFSET
            LOGD("VIF%d OFFSET val=%u", vif_idx, nloop & 0x7FFF);
            break;

        case 0x03: // BASE - Set BASE
            LOGD("VIF%d BASE val=%u", vif_idx, nloop & 0x7FFF);
            break;

        case 0x10: // UNPACK V4-32 (legacy, same as 0x70)
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
            LOGD("VIF%d UNPACK (legacy 0x%02X) nloop=%u", vif_idx, cmd, nloop);
            unpack_data(0x70 | (cmd & 0x1F), data, qwc * 4, vu, vif_idx);
            break;

        default:
            LOGW("VIF%d unknown cmd 0x%02X (tag=0x%08X)", vif_idx, cmd, tag);
            break;
    }

    return true;
}

static inline void sign_extend_16(int16_t& val) {
    // already int16_t, no-op
}

static inline int32_t sign_extend_8(int8_t val) {
    return static_cast<int32_t>(val);
}

static inline int32_t sign_extend_12(int32_t val) {
    val &= 0xFFF;
    if (val & 0x800) val |= ~0xFFF;
    return val;
}

static inline int32_t sign_extend_10(int32_t val) {
    val &= 0x3FF;
    if (val & 0x200) val |= ~0x3FF;
    return val;
}

static inline int32_t sign_extend_7(int32_t val) {
    val &= 0x7F;
    if (val & 0x40) val |= ~0x7F;
    return val;
}

static inline float int_to_float(int32_t v) {
    return static_cast<float>(v);
}

static inline float fixed_4_to_float(int32_t v) {
    return static_cast<float>(v) / 16.0f;
}

static inline float fixed_12_to_float(int32_t v) {
    return static_cast<float>(v) / 4096.0f;
}

void VIF_Unpacker::unpack_data(uint32_t cmd, const uint32_t* data, int size, VU_Core& vu, int vif_idx) {
    uint32_t mode = cmd & 0x1F;
    int vu_num = vif_idx;
    uint8_t* mem = vu.get_data_mem(vu_num);
    int16_t* vi_regs = vu.vi[vu_num];
    uint32_t& top_reg = top[vif_idx];

    LOGD("VIF%d UNPACK mode=0x%02X nloop_cmd=0x%02X size=%d top=%u",
         vif_idx, mode, cmd, size, top_reg);

    int data_pos = 0;

    for (int i = 0; i < size && data_pos < size; ) {
        uint32_t result[4] = {0, 0, 0, 0};

        switch (mode) {
            case VIF_PACK_S32: {
                result[0] = (data_pos < size) ? data[data_pos++] : 0;
                break;
            }
            case VIF_PACK_S16: {
                if (data_pos < size) {
                    uint32_t w = data[data_pos++];
                    int16_t lo = static_cast<int16_t>(w & 0xFFFF);
                    int16_t hi = static_cast<int16_t>((w >> 16) & 0xFFFF);
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(lo));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(hi));
                }
                break;
            }
            case VIF_PACK_S8: {
                if (data_pos < size) {
                    uint32_t w = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(w & 0xFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 8) & 0xFF)));
                    result[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 16) & 0xFF)));
                    result[3] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 24) & 0xFF)));
                }
                break;
            }
            case VIF_PACK_V4_32: {
                for (int c = 0; c < 4; c++) {
                    result[c] = (data_pos < size) ? data[data_pos++] : 0;
                }
                break;
            }
            case VIF_PACK_V3_32: {
                for (int c = 0; c < 3; c++) {
                    result[c] = (data_pos < size) ? data[data_pos++] : 0;
                }
                result[3] = 0x3F800000; // 1.0f
                break;
            }
            case VIF_PACK_V2_32: {
                for (int c = 0; c < 2; c++) {
                    result[c] = (data_pos < size) ? data[data_pos++] : 0;
                }
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V1_32: {
                result[0] = (data_pos < size) ? data[data_pos++] : 0;
                result[1] = 0;
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V4_16: {
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w0 & 0xFFFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((w0 >> 16) & 0xFFFF)));
                }
                if (data_pos < size) {
                    uint32_t w1 = data[data_pos++];
                    result[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w1 & 0xFFFF)));
                    result[3] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((w1 >> 16) & 0xFFFF)));
                }
                break;
            }
            case VIF_PACK_V3_16: {
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w0 & 0xFFFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((w0 >> 16) & 0xFFFF)));
                }
                if (data_pos < size) {
                    uint32_t w1 = data[data_pos++];
                    result[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w1 & 0xFFFF)));
                }
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V2_16: {
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w0 & 0xFFFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((w0 >> 16) & 0xFFFF)));
                }
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V1_16: {
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w0 & 0xFFFF)));
                }
                result[1] = 0;
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V3_12: {
                // Three 12-bit signed values packed in 36 bits (3 words used)
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    int32_t v0 = sign_extend_12(w0 & 0xFFF);
                    int32_t v1 = sign_extend_12((w0 >> 12) & 0xFFF);
                    int32_t v2 = sign_extend_12((w0 >> 24) & 0xF | ((data_pos < size ? data[data_pos] : 0) & 0xFF) << 4);
                    result[0] = static_cast<uint32_t>(v0);
                    result[1] = static_cast<uint32_t>(v1);
                    result[2] = static_cast<uint32_t>(v2);
                    if (data_pos < size) data_pos++;
                }
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V2_12: {
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    int32_t v0 = sign_extend_12(w0 & 0xFFF);
                    int32_t v1 = sign_extend_12((w0 >> 12) & 0xFFF);
                    result[0] = static_cast<uint32_t>(v0);
                    result[1] = static_cast<uint32_t>(v1);
                }
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V4_12: {
                if (data_pos < size) {
                    uint32_t w0 = data[data_pos++];
                    int32_t v0 = sign_extend_12(w0 & 0xFFF);
                    int32_t v1 = sign_extend_12((w0 >> 12) & 0xFFF);
                    result[0] = static_cast<uint32_t>(v0);
                    result[1] = static_cast<uint32_t>(v1);
                    if (data_pos < size) {
                        uint32_t w1 = data[data_pos++];
                        int32_t v2 = sign_extend_12(w1 & 0xFFF);
                        int32_t v3 = sign_extend_12((w1 >> 12) & 0xFFF);
                        result[2] = static_cast<uint32_t>(v2);
                        result[3] = static_cast<uint32_t>(v3);
                    }
                }
                break;
            }
            case VIF_PACK_V4_8: {
                if (data_pos < size) {
                    uint32_t w = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(w & 0xFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 8) & 0xFF)));
                    result[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 16) & 0xFF)));
                    result[3] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 24) & 0xFF)));
                }
                break;
            }
            case VIF_PACK_V3_8: {
                if (data_pos < size) {
                    uint32_t w = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(w & 0xFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 8) & 0xFF)));
                    result[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 16) & 0xFF)));
                }
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V2_8: {
                if (data_pos < size) {
                    uint32_t w = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(w & 0xFF)));
                    result[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((w >> 8) & 0xFF)));
                }
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            case VIF_PACK_V1_8: {
                if (data_pos < size) {
                    uint32_t w = data[data_pos++];
                    result[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(w & 0xFF)));
                }
                result[1] = 0;
                result[2] = 0;
                result[3] = 0x3F800000;
                break;
            }
            default:
                LOGW("VIF%d unknown pack mode 0x%02X", vif_idx, mode);
                data_pos++;
                break;
        }

        uint32_t addr = (top_reg * 16) % VU1_DATA_SIZE;
        memcpy(&mem[addr], result, 16);
        top_reg++;
    }

    LOGD("VIF%d UNPACK done, new top=%u", vif_idx, top_reg);
}
