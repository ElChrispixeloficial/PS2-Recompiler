// gs/gs_rasterizer.cpp
// Software rasterizer fallback for GS primitives (Phase 2).
// Phase 1: Vulkan path handles all rendering. This is compiled but unused.
#include <cstdint>
#include <android/log.h>

#define TAG "GS_Rast"

// Rasterise a flat-colour triangle into a 32-bit RGBA framebuffer.
// (stub — fill in Phase 2 for accurate per-pixel GS effects)
void gs_rasterize_triangle(
    uint32_t* fb, int fb_w, int fb_h,
    int x0, int y0, int x1, int y1, int x2, int y2,
    uint32_t colour)
{
    (void)fb; (void)fb_w; (void)fb_h;
    (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2;
    (void)colour;
    // TODO: Phase 2 — scanline rasteriser with GS alpha/depth rules
}

void gs_rasterize_sprite(
    uint32_t* fb, int fb_w, int fb_h,
    int x0, int y0, int x1, int y1, uint32_t colour)
{
    if (!fb) return;
    int lx = x0 < x1 ? x0 : x1;
    int rx = x0 < x1 ? x1 : x0;
    int ty = y0 < y1 ? y0 : y1;
    int by = y0 < y1 ? y1 : y0;
    for (int y = ty; y < by && y < fb_h; y++)
        for (int x = lx; x < rx && x < fb_w; x++)
            fb[y * fb_w + x] = colour;
}
