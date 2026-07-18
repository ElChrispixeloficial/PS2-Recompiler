#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <android/log.h>

#define TAG "GS-Rast"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct GS_RastPixel {
    uint8_t r, g, b, a;
};

struct GS_RastVertex {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
    float q;
};

static inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static inline void unpack_rgba(uint32_t c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (c >> 0)  & 0xFF;
    g = (c >> 8)  & 0xFF;
    b = (c >> 16) & 0xFF;
    a = (c >> 24) & 0xFF;
}

static inline float edge_function(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static inline float min3(float a, float b, float c) {
    return std::min(a, std::min(b, c));
}

static inline float max3(float a, float b, float c) {
    return std::max(a, std::max(b, c));
}

static inline int clamp_i(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

void gs_rasterize_triangle(
    uint32_t* fb, int fb_w, int fb_h,
    int x0, int y0, int x1, int y1, int x2, int y2,
    uint32_t colour)
{
    if (!fb || fb_w <= 0 || fb_h <= 0) return;

    float fx0 = (float)x0, fy0 = (float)y0;
    float fx1 = (float)x1, fy1 = (float)y1;
    float fx2 = (float)x2, fy2 = (float)y2;

    int min_x = clamp_i((int)min3(fx0, fx1, fx2), 0, fb_w - 1);
    int max_x = clamp_i((int)max3(fx0, fx1, fx2), 0, fb_w - 1);
    int min_y = clamp_i((int)min3(fy0, fy1, fy2), 0, fb_h - 1);
    int max_y = clamp_i((int)max3(fy0, fy1, fy2), 0, fb_h - 1);

    float area = edge_function(fx0, fy0, fx1, fy1, fx2, fy2);
    if (fabsf(area) < 0.001f) return;

    uint8_t cr, cg, cb, ca;
    unpack_rgba(colour, cr, cg, cb, ca);

    for (int py = min_y; py <= max_y; py++) {
        for (int px = min_x; px <= max_x; px++) {
            float cx = px + 0.5f;
            float cy = py + 0.5f;

            float w0 = edge_function(fx1, fy1, fx2, fy2, cx, cy);
            float w1 = edge_function(fx2, fy2, fx0, fy0, cx, cy);
            float w2 = edge_function(fx0, fy0, fx1, fy1, cx, cy);

            if (area > 0) {
                if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                    fb[py * fb_w + px] = colour;
                }
            } else {
                if (w0 <= 0 && w1 <= 0 && w2 <= 0) {
                    fb[py * fb_w + px] = colour;
                }
            }
        }
    }
}

void gs_rasterize_sprite(
    uint32_t* fb, int fb_w, int fb_h,
    int x0, int y0, int x1, int y1, uint32_t colour)
{
    if (!fb) return;

    int lx = std::min(x0, x1);
    int rx = std::max(x0, x1);
    int ty = std::min(y0, y1);
    int by = std::max(y0, y1);

    lx = clamp_i(lx, 0, fb_w - 1);
    rx = clamp_i(rx, 0, fb_w - 1);
    ty = clamp_i(ty, 0, fb_h - 1);
    by = clamp_i(by, 0, fb_h - 1);

    for (int y = ty; y <= by; y++) {
        uint32_t* row = &fb[y * fb_w + lx];
        int count = rx - lx + 1;
        for (int i = 0; i < count; i++) {
            row[i] = colour;
        }
    }
}

uint32_t gs_decode_psm32(const uint8_t* vram, uint32_t offset) {
    uint32_t val;
    memcpy(&val, vram + offset * 4, 4);
    return val;
}

uint32_t gs_decode_psm24(const uint8_t* vram, uint32_t offset) {
    uint32_t addr = offset * 3;
    uint32_t b0 = vram[addr + 0];
    uint32_t b1 = vram[addr + 1];
    uint32_t b2 = vram[addr + 2];
    return b0 | (b1 << 8) | (b2 << 16) | 0xFF000000;
}

uint32_t gs_decode_psm16(const uint8_t* vram, uint32_t offset) {
    uint16_t val;
    memcpy(&val, vram + offset * 2, 2);

    uint32_t r5 = (val >>  0) & 0x1F;
    uint32_t g5 = (val >>  5) & 0x1F;
    uint32_t b5 = (val >> 10) & 0x1F;
    uint32_t a1 = (val >> 15) & 0x01;

    uint32_t r8 = (r5 * 255 + 15) / 31;
    uint32_t g8 = (g5 * 255 + 15) / 31;
    uint32_t b8 = (b5 * 255 + 15) / 31;
    uint32_t a8 = a1 ? 255 : 0;

    return r8 | (g8 << 8) | (b8 << 16) | (a8 << 24);
}

uint32_t gs_decode_psm8(const uint8_t* vram, uint32_t offset, const uint32_t* clut) {
    uint8_t index = vram[offset];
    if (clut) {
        return clut[index & 0xFF];
    }
    uint8_t g = index;
    return g | (g << 8) | (g << 16) | 0xFF000000;
}

uint32_t gs_decode_psm4(const uint8_t* vram, uint32_t pixel_index, const uint32_t* clut) {
    uint32_t byte_offset = pixel_index / 2;
    uint8_t nibble = (pixel_index & 1) ? (vram[byte_offset] >> 4) : (vram[byte_offset] & 0x0F);

    if (clut) {
        return clut[nibble];
    }
    uint8_t g = (uint8_t)(nibble * 17);
    return g | (g << 8) | (g << 16) | 0xFF000000;
}

uint32_t* gs_decode_clut(const uint8_t* vram, uint32_t base_offset, uint32_t entries) {
    if (entries == 0 || entries > 1024) return nullptr;

    uint32_t* clut = new uint32_t[entries];
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t addr = (base_offset + i) * 4;
        memcpy(&clut[i], vram + addr, 4);
    }
    return clut;
}

uint32_t gs_apply_alpha_blend(uint32_t src, uint32_t dst,
                               uint8_t a_factor, uint8_t b_factor,
                               uint8_t c_factor, uint8_t d_factor) {
    uint8_t sr, sg, sb, sa;
    uint8_t dr, dg, db, da;
    unpack_rgba(src, sr, sg, sb, sa);
    unpack_rgba(dst, dr, dg, db, da);

    auto blend_ch = [&](uint8_t s, uint8_t d, uint8_t a, uint8_t b, uint8_t c, uint8_t dd) -> uint8_t {
        float fs = s * (a / 128.0f);
        float fd = d * (b / 128.0f);
        float fc = a * (c / 128.0f);
        float result = fs + fd - fc + d * (dd / 128.0f);
        int v = (int)result;
        return (uint8_t)clamp_i(v, 0, 255);
    };

    uint8_t rr = blend_ch(sr, dr, a_factor, b_factor, c_factor, d_factor);
    uint8_t rg = blend_ch(sg, dg, a_factor, b_factor, c_factor, d_factor);
    uint8_t rb = blend_ch(sb, db, a_factor, b_factor, c_factor, d_factor);
    uint8_t ra = clamp_i(sa + da, 0, 255);

    return pack_rgba(rr, rg, rb, ra);
}

bool gs_alpha_test(uint8_t pixel_alpha, uint8_t test_alpha, uint8_t alpha_method) {
    switch (alpha_method) {
        case 0: return false;
        case 1: return pixel_alpha == test_alpha;
        case 2: return pixel_alpha >= test_alpha;
        case 3: return pixel_alpha >  test_alpha;
        case 4: return pixel_alpha <  test_alpha;
        case 5: return pixel_alpha <= test_alpha;
        case 6: return pixel_alpha != test_alpha;
        case 7: return true;
        default: return false;
    }
}

bool gs_depth_test(uint32_t pixel_z, uint32_t buffer_z, uint8_t z_method) {
    switch (z_method) {
        case 0: return false;
        case 1: return true;
        case 2: return pixel_z <= buffer_z;
        case 3: return pixel_z <  buffer_z;
        case 4: return pixel_z >= buffer_z;
        case 5: return pixel_z >  buffer_z;
        case 6: return pixel_z == buffer_z;
        case 7: return pixel_z != buffer_z;
        default: return false;
    }
}
