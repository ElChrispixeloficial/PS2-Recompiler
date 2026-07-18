#include "gs_core.h"
#include "gs_vulkan.h"
#include <android/native_window.h>
#include <android/log.h>
#include <cstring>
#include <cmath>

extern "C" int g_gs_writes;
extern "C" int g_gs_kicks;
extern "C" uint64_t g_last_gs_reg;
extern "C" uint8_t  g_last_gs_addr;

#define LOG_TAG "PS2-GS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

GS_Core* g_gs_ptr = nullptr;

struct GS_DrawState {
    uint32_t tex0[2][2];
    uint32_t frame[2][2];
    uint32_t zbuf[2][2];
    uint32_t alpha[2][2];
    uint32_t test[2][2];
    uint32_t scissor[2][2];
    uint32_t prim;
    uint32_t rgbaq;
    uint32_t uv;
    uint32_t xyzf2, xyz2;
    uint32_t bitbltbuf[2];
    uint32_t trxpos[2];
    uint32_t trxreg[2];
    uint32_t trxdir;
    uint8_t  textransfer[256 * 1024];
    int      textransfer_size;
};

static GS_DrawState s_draw;

static void decode_tex0(uint32_t ctx, uint64_t data) {
    s_draw.tex0[ctx][0] = (uint32_t)(data & 0xFFFFFFFF);
    s_draw.tex0[ctx][1] = (uint32_t)(data >> 32);
}

static void decode_frame(uint32_t ctx, uint64_t data) {
    s_draw.frame[ctx][0] = (uint32_t)(data & 0xFFFFFFFF);
    s_draw.frame[ctx][1] = (uint32_t)(data >> 32);
}

static void decode_zbuf(uint32_t ctx, uint64_t data) {
    s_draw.zbuf[ctx][0] = (uint32_t)(data & 0xFFFFFFFF);
    s_draw.zbuf[ctx][1] = (uint32_t)(data >> 32);
}

static void decode_alpha(uint32_t ctx, uint64_t data) {
    s_draw.alpha[ctx][0] = (uint32_t)(data & 0xFFFFFFFF);
    s_draw.alpha[ctx][1] = (uint32_t)(data >> 32);
}

static void decode_test(uint32_t ctx, uint64_t data) {
    s_draw.test[ctx][0] = (uint32_t)(data & 0xFFFFFFFF);
    s_draw.test[ctx][1] = (uint32_t)(data >> 32);
}

static void decode_scissor(uint32_t ctx, uint64_t data) {
    s_draw.scissor[ctx][0] = (uint32_t)(data & 0xFFFFFFFF);
    s_draw.scissor[ctx][1] = (uint32_t)(data >> 32);
}

GS_Core::GS_Core() {
    memset(&state, 0, sizeof(state));
    memset(&s_draw, 0, sizeof(s_draw));
    vulkan = std::make_unique<GS_Vulkan>();
    g_gs_ptr = this;
    LOGI("GS Core initialized");
}

GS_Core::~GS_Core() = default;

bool GS_Core::init_vulkan(void* s, int w, int h) {
    return vulkan->init(static_cast<ANativeWindow*>(s));
}

void GS_Core::write_reg(uint8_t reg, uint64_t data) {
    g_gs_writes++;
    g_last_gs_reg = data;
    g_last_gs_addr = reg;

    if (reg < 0x64) state.regs[reg] = data;

    uint32_t ctx = state.context;

    switch (reg) {
        case GS_PRIM:
            s_draw.prim = (uint32_t)data;
            break;

        case GS_RGBAQ:
            s_draw.rgbaq = (uint32_t)data;
            break;

        case GS_UV:
            s_draw.uv = (uint32_t)data;
            break;

        case GS_ST:
            break;

        case GS_TEX0_1:
            decode_tex0(0, data);
            break;
        case GS_TEX0_2:
            decode_tex0(1, data);
            break;

        case GS_CLAMP_1:
        case GS_CLAMP_2:
            break;

        case GS_FOG:
            break;

        case GS_XYOFFSET_1:
        case GS_XYOFFSET_2:
            break;

        case GS_PRMODECONT:
        case GS_PRMODE:
            break;

        case GS_TEXCLUT:
            break;

        case GS_SCANMSK:
            break;

        case GS_TEXA:
            break;

        case GS_FOGCOL:
            break;

        case GS_TEXFLUSH:
            break;

        case GS_SCISSOR_1:
            decode_scissor(0, data);
            break;
        case GS_SCISSOR_2:
            decode_scissor(1, data);
            break;

        case GS_ALPHA_1:
            decode_alpha(0, data);
            break;
        case GS_ALPHA_2:
            decode_alpha(1, data);
            break;

        case GS_DIMX:
        case GS_DTHE:
        case GS_COLCLAMP:
            break;

        case GS_TEST_1:
            decode_test(0, data);
            break;
        case GS_TEST_2:
            decode_test(1, data);
            break;

        case GS_PABE:
        case GS_FBA_1:
        case GS_FBA_2:
            break;

        case GS_FRAME_1:
            decode_frame(0, data);
            break;
        case GS_FRAME_2:
            decode_frame(1, data);
            break;

        case GS_ZBUF_1:
            decode_zbuf(0, data);
            break;
        case GS_ZBUF_2:
            decode_zbuf(1, data);
            break;

        case GS_BITBLTBUF:
            s_draw.bitbltbuf[0] = (uint32_t)(data & 0xFFFFFFFF);
            s_draw.bitbltbuf[1] = (uint32_t)(data >> 32);
            break;

        case GS_TRXPOS:
            s_draw.trxpos[0] = (uint32_t)(data & 0xFFFFFFFF);
            s_draw.trxpos[1] = (uint32_t)(data >> 32);
            break;

        case GS_TRXREG:
            s_draw.trxreg[0] = (uint32_t)(data & 0xFFFFFFFF);
            s_draw.trxreg[1] = (uint32_t)(data >> 32);
            break;

        case GS_TRXDIR:
            s_draw.trxdir = (uint32_t)(data & 0xFFFFFFFF);
            break;

        case GS_HWREG:
            transfer_data((const uint8_t*)&data, 8);
            break;

        case GS_SIGNAL:
        case GS_FINISH:
        case GS_LABEL:
            break;

        case GS_XYZF2:
        case GS_XYZ2: {
            auto& v = state.vertex_queue[state.vertex_count % 4];
            v.x = (int32_t)((data >> 0) & 0xFFFF);
            v.y = (int32_t)((data >> 16) & 0xFFFF);
            v.z = (int32_t)((data >> 32) & 0xFFFFFF);
            v.rgba = s_draw.rgbaq;
            uint64_t uv_reg = s_draw.uv;
            v.s = (float)((uv_reg >> 0) & 0x3FFF) / 16.0f;
            v.t = (float)((uv_reg >> 16) & 0x3FFF) / 16.0f;
            state.vertex_count++;
            kick_primitive();
            break;
        }

        case GS_XYZF3:
        case GS_XYZ3: {
            auto& v = state.vertex_queue[state.vertex_count % 4];
            v.x = (int32_t)((data >> 0) & 0xFFFF);
            v.y = (int32_t)((data >> 16) & 0xFFFF);
            v.z = (int32_t)((data >> 32) & 0xFFFFFF);
            v.rgba = s_draw.rgbaq;
            state.vertex_count++;
            break;
        }

        default:
            break;
    }
}

void GS_Core::kick_primitive() {
    g_gs_kicks++;
    uint32_t prim_type = s_draw.prim & 0x7;
    int needed = 3;
    switch (prim_type) {
        case 0: needed = 1; break;
        case 1: case 2: needed = 2; break;
        case 3: case 4: case 5: needed = 3; break;
        case 6: needed = 4; break;
        case 7: needed = 0; break;
    }
    if (needed == 0 || state.vertex_count < needed) return;

    uint64_t xyoff = state.regs[GS_XYOFFSET_1 + state.context];
    int32_t ox = (int32_t)((xyoff >> 0) & 0xFFFF);
    int32_t oy = (int32_t)((xyoff >> 16) & 0xFFFF);

    if (prim_type == 6) {
        auto& v0 = state.vertex_queue[0];
        auto& v1 = state.vertex_queue[1];
        PS2_Vertex verts[6];
        float x0 = (v0.x - ox) / 16.0f;
        float y0 = (v0.y - oy) / 16.0f;
        float x1 = (v1.x - ox) / 16.0f;
        float y1 = (v1.y - oy) / 16.0f;
        float r = ((v0.rgba >> 0)  & 0xFF) / 128.0f;
        float g = ((v0.rgba >> 8)  & 0xFF) / 128.0f;
        float b = ((v0.rgba >> 16) & 0xFF) / 128.0f;
        float a = ((v0.rgba >> 24) & 0xFF) / 128.0f;
        float u0 = v0.s, v0t = v0.t;
        float u1 = v1.s, v1t = v1.t;
        verts[0] = {x0, y0, 0, 1, r, g, b, a, u0, v0t};
        verts[1] = {x1, y0, 0, 1, r, g, b, a, u1, v0t};
        verts[2] = {x1, y1, 0, 1, r, g, b, a, u1, v1t};
        verts[3] = {x0, y0, 0, 1, r, g, b, a, u0, v0t};
        verts[4] = {x1, y1, 0, 1, r, g, b, a, u1, v1t};
        verts[5] = {x0, y1, 0, 1, r, g, b, a, u0, v1t};
        vulkan->draw_primitive(verts, 6, prim_type);
    } else if (prim_type >= 3 && prim_type <= 5) {
        PS2_Vertex verts[3];
        for (int i = 0; i < 3; i++) {
            auto& v = state.vertex_queue[i];
            verts[i] = {
                (v.x - ox) / 16.0f,
                (v.y - oy) / 16.0f,
                0, 1,
                ((v.rgba >> 0)  & 0xFF) / 128.0f,
                ((v.rgba >> 8)  & 0xFF) / 128.0f,
                ((v.rgba >> 16) & 0xFF) / 128.0f,
                ((v.rgba >> 24) & 0xFF) / 128.0f,
                v.s, v.t
            };
        }
        vulkan->draw_primitive(verts, 3, prim_type);
    } else if (prim_type == 1 || prim_type == 2) {
        PS2_Vertex verts[2];
        for (int i = 0; i < 2; i++) {
            auto& v = state.vertex_queue[i];
            verts[i] = {
                (v.x - ox) / 16.0f,
                (v.y - oy) / 16.0f,
                0, 1,
                ((v.rgba >> 0)  & 0xFF) / 128.0f,
                ((v.rgba >> 8)  & 0xFF) / 128.0f,
                ((v.rgba >> 16) & 0xFF) / 128.0f,
                ((v.rgba >> 24) & 0xFF) / 128.0f,
                v.s, v.t
            };
        }
        vulkan->draw_primitive(verts, 2, prim_type);
    }

    state.vertex_count = (prim_type == 4 || prim_type == 5) ? 1 : 0;
}

void GS_Core::process_gif_packet(const uint8_t* data, size_t size_qwords) {
    const uint64_t* qwords = reinterpret_cast<const uint64_t*>(data);

    size_t idx = 0;
    while (idx < size_qwords) {
        uint64_t gif_tag = qwords[idx++];

        uint16_t nloop = (uint16_t)(gif_tag & 0x7FFF);
        bool     eod   = (gif_tag >> 15) & 1;
        uint8_t  flg   = (uint8_t)((gif_tag >> 16) & 0x3);
        uint8_t  nreg  = (uint8_t)((gif_tag >> 18) & 0x3F);
        if (nreg == 0) nreg = 1;

        if (flg == 2) {
            uint32_t image_words = nloop * (nreg > 0 ? nreg : 1);
            const uint8_t* img_data = reinterpret_cast<const uint8_t*>(&qwords[idx]);
            transfer_data(img_data, image_words * 8);
            idx += image_words;
        } else {
            uint8_t regs[16];
            if (nreg <= 16) {
                uint64_t reg_field = (gif_tag >> 28);
                for (int i = 0; i < nreg; i++) {
                    regs[i] = (uint8_t)((reg_field >> (i * 4)) & 0xF);
                }
            }

            for (uint16_t loop = 0; loop < nloop; loop++) {
                if (idx >= size_qwords) break;
                uint64_t reg_data = qwords[idx++];

                if (flg == 0) {
                    for (int r = 0; r < nreg; r++) {
                        write_reg(regs[r], reg_data);
                    }
                } else if (flg == 1) {
                    uint32_t reg_word = (uint32_t)(reg_data & 0xFFFFFFFF);
                    uint8_t reg_addr = (uint8_t)(reg_word & 0x7F);
                    write_reg(reg_addr, reg_data);
                }
            }
        }

        if (eod) break;
    }
}

void GS_Core::process_gif(const uint32_t* data, uint32_t qwc) {
    process_gif_packet(reinterpret_cast<const uint8_t*>(data), qwc);
}

void GS_Core::transfer_data(const uint8_t* src, size_t size) {
    size_t space = sizeof(s_draw.textransfer) - s_draw.textransfer_size;
    size_t copy = (size < space) ? size : space;
    if (copy > 0) {
        memcpy(s_draw.textransfer + s_draw.textransfer_size, src, copy);
        s_draw.textransfer_size += (int)copy;
    }

    if (s_draw.trxdir == 0 || s_draw.trxdir == 1) {
        execute_bitblt();
    }
}

void GS_Core::execute_bitblt() {
    uint32_t sbp  = s_draw.bitbltbuf[0] & 0x3FFF;
    uint32_t sbw  = (s_draw.bitbltbuf[0] >> 16) & 0x3F;
    uint32_t spsm = (s_draw.bitbltbuf[0] >> 24) & 0xF;
    uint32_t dbp  = s_draw.bitbltbuf[1] & 0x3FFF;
    uint32_t dbw  = (s_draw.bitbltbuf[1] >> 16) & 0x3F;
    uint32_t dpsm = (s_draw.bitbltbuf[1] >> 24) & 0xF;

    uint32_t ssax = s_draw.trxpos[0] & 0x3FFF;
    uint32_t ssay = (s_draw.trxpos[0] >> 16) & 0x3FFF;
    uint32_t dsax = s_draw.trxpos[1] & 0x3FFF;
    uint32_t dsay = (s_draw.trxpos[1] >> 16) & 0x3FFF;

    uint32_t rrw = s_draw.trxreg[0] & 0xFFFF;
    uint32_t rrh = s_draw.trxreg[1] & 0xFFFF;
    if (rrw == 0) rrw = 0xFFFF;
    if (rrh == 0) rrh = 0xFFFF;

    if (s_draw.textransfer_size <= 0) return;

    uint32_t bpp;
    switch (dpsm) {
        case 0:  bpp = 32; break;
        case 1:  bpp = 24; break;
        case 2:  bpp = 16; break;
        default: bpp = 32; break;
    }

    uint32_t pixels_per_word = (bpp == 32) ? 1 : (bpp == 24) ? 1 : 2;
    uint32_t dst_x = dsax;
    uint32_t dst_y = dsay;
    uint32_t words_used = s_draw.textransfer_size / 4;
    uint32_t src_idx = 0;

    for (uint32_t y = 0; y < rrh; y++) {
        for (uint32_t x = 0; x < rrw; x += pixels_per_word) {
            if (src_idx >= words_used) goto done;
            uint32_t word_val;
            memcpy(&word_val, s_draw.textransfer + src_idx * 4, 4);

            uint32_t px = dst_x + x;
            uint32_t py = dst_y + y;
            uint32_t addr = ((dbp + py * (dbw ? dbw : 1)) * 64 + px) * (bpp / 8);
            if (addr + 4 <= sizeof(state.vram)) {
                memcpy(state.vram + addr, &word_val, 4);
            }
            if (pixels_per_word == 2 && x + 1 < rrw) {
                uint32_t addr2 = ((dbp + py * (dbw ? dbw : 1)) * 64 + px + 1) * (bpp / 8);
                if (addr2 + 4 <= sizeof(state.vram)) {
                    uint32_t px2 = (bpp == 16) ? (word_val >> 16) : word_val;
                    memcpy(state.vram + addr2, &px2, 4);
                }
            }
            src_idx++;
        }
        dst_x = dsax;
        dst_y++;
    }
done:
    s_draw.textransfer_size = 0;
}

void GS_Core::vsync() {
    vulkan->present_frame();
    vulkan->begin_frame();
}

extern "C" void gs_write_reg(uint32_t addr, uint32_t val) {
    if (!g_gs_ptr) return;
    uint8_t reg = (addr & 0xFFFF) >> 3;
    if ((addr & 0x4) == 0) {
        g_gs_ptr->state.temp_reg_lo = val;
    } else {
        uint64_t full_val = g_gs_ptr->state.temp_reg_lo | ((uint64_t)val << 32);
        g_gs_ptr->write_reg(reg, full_val);
    }
}

extern "C" void gs_write_priv(uint32_t addr, uint32_t val) {
    if (!g_gs_ptr) return;
}
