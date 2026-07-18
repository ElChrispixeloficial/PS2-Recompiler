// vu/vif_unpacker.h
// VIF (VIFIFO Interface) Unpacker - decompresses VIF1 packets into VU1 micro/mem data
#pragma once
#include <cstdint>
#include <cstddef>

class VU_Core;

struct VIF_Unpacker {
    uint32_t vif0_stat;
    uint32_t vif1_stat;
    uint32_t vif1_fifo[256];
    int     fifo_pos;
    int     fifo_size;

    uint32_t itops[2];
    uint32_t top[2];
    uint32_t r0[2];

    void reset();
    bool feed_packet(uint32_t tag, const uint32_t* data, int qwc, VU_Core& vu, int vif_idx);
    void unpack_data(uint32_t cmd, const uint32_t* data, int size, VU_Core& vu, int vif_idx);

    static uint32_t read_stat(int vif_idx);
    static void write_stat(int vif_idx, uint32_t val);
    static uint32_t read_fifo(int vif_idx);
    static void write_top(int vif_idx, uint32_t val);
    static uint32_t read_top(int vif_idx);
};

// VIF mode types for UNPACK command
enum VIF_PackMode : uint8_t {
    VIF_PACK_S32   = 0x00,
    VIF_PACK_S16   = 0x01,
    VIF_PACK_S8    = 0x02,
    VIF_PACK_V3_32 = 0x04,
    VIF_PACK_V2_32 = 0x05,
    VIF_PACK_V1_32 = 0x06,
    VIF_PACK_V3_16 = 0x08,
    VIF_PACK_V3_12 = 0x09,
    VIF_PACK_V2_16 = 0x0A,
    VIF_PACK_V2_12 = 0x0B,
    VIF_PACK_V1_16 = 0x0C,
    VIF_PACK_V4_32 = 0x10,
    VIF_PACK_V4_16 = 0x14,
    VIF_PACK_V4_12 = 0x15,
    VIF_PACK_V4_8  = 0x18,
    VIF_PACK_V3_8  = 0x19,
    VIF_PACK_V2_8  = 0x1A,
    VIF_PACK_V1_8  = 0x1B,
};
