#include <android/log.h>
#include "gs_core.h"
#include "gs_vulkan.h"
#include <android/native_window.h>
#include <cstring>

extern "C" int g_gs_writes;
extern "C" int g_gs_kicks;
extern "C" uint64_t g_last_gs_reg;
extern "C" uint8_t g_last_gs_addr;

#define LOG_TAG "PS2-GS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

GS_Core* g_gs_ptr = nullptr;

GS_Core::GS_Core() { 
    memset(&state, 0, sizeof(state)); 
    vulkan = std::make_unique<GS_Vulkan>(); 
    g_gs_ptr = this; 
    LOGI("GS Core iniciado"); 
}
GS_Core::~GS_Core() = default;

bool GS_Core::init_vulkan(void* s, int w, int h) { return vulkan->init(static_cast<ANativeWindow*>(s)); }

void GS_Core::write_reg(uint8_t reg, uint64_t data) {
    g_gs_writes++;
    g_last_gs_reg = data;
    g_last_gs_addr = reg;
    if (reg < 0x64) state.regs[reg] = data;
    switch (reg) {
    case GS_XYZF2: case GS_XYZ2: {
        auto& v = state.vertex_queue[state.vertex_count];
        v.x=(int32_t)((data>>0)&0xFFFF); v.y=(int32_t)((data>>16)&0xFFFF);
        v.z=(int32_t)((data>>32)&0xFFFFFF); v.rgba=(uint32_t)state.regs[GS_RGBAQ];
        uint64_t uv=state.regs[GS_UV]; v.s=(float)((uv>>0)&0x3FFF)/16.0f; v.t=(float)((uv>>16)&0x3FFF)/16.0f;
        state.vertex_count++;
        kick_primitive(); 
        break;
    }
    case GS_XYZF3: case GS_XYZ3: {
        auto& v = state.vertex_queue[state.vertex_count%4];
        v.x=(int32_t)((data>>0)&0xFFFF); v.y=(int32_t)((data>>16)&0xFFFF);
        v.z=(int32_t)((data>>32)&0xFFFFFF); v.rgba=(uint32_t)state.regs[GS_RGBAQ];
        state.vertex_count++;
        break;
    }
    default: break;
    }
}

void GS_Core::kick_primitive() {
    g_gs_kicks++;
    uint64_t prim = state.regs[GS_PRIM];
    uint8_t pt = prim & 7; 
    int needed = 3; 
    switch(pt){
        case 0: needed=1; break;
        case 1: case 2: needed=2; break;
        case 6: needed=4; break;
    }
    if(state.vertex_count < needed) return;
    
    uint64_t xyoff = state.regs[GS_XYOFFSET_1 + state.context];
    int32_t ox = (xyoff >> 0) & 0xFFFF; 
    int32_t oy = (xyoff >> 16) & 0xFFFF;
    
    if(pt==6) {
        // Sprite
    } else if(pt>=3 && pt<=5) {
        // Triángulo
        PS2_Vertex verts[3];
        for(int i=0; i<3; i++) {
            auto& v = state.vertex_queue[i];
            verts[i] = {
                (v.x - ox) / 16.0f, 
                (v.y - oy) / 16.0f, 
                ((v.rgba >> 0) & 0xFF) / 128.0f, 
                ((v.rgba >> 8) & 0xFF) / 128.0f, 
                ((v.rgba >> 16) & 0xFF) / 128.0f, 
                ((v.rgba >> 24) & 0xFF) / 128.0f, 
                v.s, v.t
            };
        }
        vulkan->draw_primitive(verts, 3, 0);
    }
    state.vertex_count = (pt==4 || pt==5) ? 1 : 0;
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

void GS_Core::process_gif(const uint32_t* data, uint32_t qwc) {
    for (uint32_t i = 0; i < qwc; i++) {
        uint32_t tag_lo = data[i * 4 + 0];
        uint32_t tag_hi = data[i * 4 + 1];
        uint32_t reg_lo = data[i * 4 + 2];
        uint32_t reg_hi = data[i * 4 + 3];

        uint8_t nloop = tag_lo & 0x7FFF;
        uint32_t reg_addr = tag_hi & 0xF; 

        if (nloop > 0 && reg_addr != 0) {
            uint64_t reg_data = (uint64_t)reg_lo | ((uint64_t)reg_hi << 32);
            write_reg(reg_addr, reg_data);
        }
    }
}

void GS_Core::process_gif_packet(const uint8_t* d, size_t s) {}
void GS_Core::transfer_data(const uint8_t* s, size_t z) {}

void GS_Core::vsync() { 
    vulkan->present_frame(); 
    vulkan->begin_frame(); 
}