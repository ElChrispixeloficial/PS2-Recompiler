#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>

// ─── Registros GIF (Graphics Interface) ──────────────────────────────────────
// El GS recibe paquetes GIF (Graphics Interface) con tags que describen
// primitivas a dibujar. Nosotros interceptamos estos paquetes y los traducimos
// a comandos Vulkan que el GPU ARM ejecuta directamente.

enum GS_Reg : uint8_t {
    GS_PRIM      = 0x00,  // tipo de primitiva (point, line, triangle, sprite)
    GS_RGBAQ     = 0x01,  // color RGBA + Q (perspectiva)
    GS_ST        = 0x02,  // coordenadas de textura S,T
    GS_UV        = 0x03,  // coordenadas UV (enteras)
    GS_XYZF2     = 0x04,  // vértice XYZ + fog (activa dibujado)
    GS_XYZ2      = 0x05,  // vértice XYZ (activa dibujado)
    GS_TEX0_1    = 0x06,  // configuración de textura 0
    GS_TEX0_2    = 0x07,
    GS_CLAMP_1   = 0x08,
    GS_CLAMP_2   = 0x09,
    GS_FOG       = 0x0A,
    GS_XYZF3     = 0x0C,  // vértice sin activar kick
    GS_XYZ3      = 0x0D,
    GS_TEX1_1    = 0x14,
    GS_TEX1_2    = 0x15,
    GS_TEX2_1    = 0x16,
    GS_TEX2_2    = 0x17,
    GS_XYOFFSET_1= 0x18,  // offset para coordenadas (fixed-point 4.4)
    GS_XYOFFSET_2= 0x19,
    GS_PRMODECONT= 0x1A,
    GS_PRMODE    = 0x1B,
    GS_TEXCLUT   = 0x1C,
    GS_SCANMSK   = 0x22,
    GS_MIPTBP1_1 = 0x34,
    GS_MIPTBP1_2 = 0x35,
    GS_MIPTBP2_1 = 0x36,
    GS_MIPTBP2_2 = 0x37,
    GS_TEXA      = 0x3B,
    GS_FOGCOL    = 0x3D,
    GS_TEXFLUSH  = 0x3F,
    GS_SCISSOR_1 = 0x40,
    GS_SCISSOR_2 = 0x41,
    GS_ALPHA_1   = 0x42,
    GS_ALPHA_2   = 0x43,
    GS_DIMX      = 0x44,
    GS_DTHE      = 0x45,
    GS_COLCLAMP  = 0x46,
    GS_TEST_1    = 0x47,
    GS_TEST_2    = 0x48,
    GS_PABE      = 0x49,
    GS_FBA_1     = 0x4A,
    GS_FBA_2     = 0x4B,
    GS_FRAME_1   = 0x4C,  // frame buffer: base, width, pixel format
    GS_FRAME_2   = 0x4D,
    GS_ZBUF_1    = 0x4E,
    GS_ZBUF_2    = 0x4F,
    GS_BITBLTBUF = 0x50,  // transfer: src/dst en VRAM
    GS_TRXPOS    = 0x51,
    GS_TRXREG    = 0x52,
    GS_TRXDIR    = 0x53,
    GS_HWREG     = 0x54,  // datos de transferencia
    GS_SIGNAL    = 0x60,
    GS_FINISH    = 0x61,
    GS_LABEL     = 0x62,
};

// Estado interno del GS
struct GS_State {
    // Registros de dibujo actuales
    uint64_t regs[0x64];

    // VRAM (4 MB en la PS2)
    uint8_t vram[4 * 1024 * 1024];

    // Vértices acumulados para la primitiva actual
    struct Vertex {
        int32_t x, y, z;    // fixed-point 4.4 (dividir por 16 para pixels)
        uint32_t rgba;
        float    s, t, q;
    } vertex_queue[4];
    int vertex_count = 0;

    // Contexto activo (1 o 2)
    int context = 0;
    
    // Variable temporal para ensamblar datos de 64 bits provenientes de la memoria del EE
    uint32_t temp_reg_lo = 0;

    // Pending signal/finish flags (set by GS_SIGNAL / GS_FINISH writes)
    bool signal_pending = false;
    bool finish_pending = false;
};

class GS_Vulkan;

class GS_Core {
public:
    GS_Core();
    ~GS_Core();

    // Inicializar con el surface nativo de Android
    bool init_vulkan(void* native_surface, int width, int height);

    // Procesar un paquete GIF (64-bit registro + 64-bit data)
    void write_reg(uint8_t reg, uint64_t data);

    // Transferencia VRAM→VRAM o host→VRAM
    void transfer_data(const uint8_t* src, size_t size);

    // Señal de VSYNC — presentar el frame al display Android
    void vsync();

    // Para el DMA PATH3 (el canal principal de envío de primitivas)
    void process_gif_packet(const uint8_t* data, size_t size_qwords);
    
    // Puente para recibir datos directos del DMA Controller
    void process_gif(const uint32_t* data, uint32_t qwc);

    GS_State state;

    GS_Vulkan* get_vulkan() { return vulkan.get(); }

private:
    std::unique_ptr<GS_Vulkan> vulkan;
    void kick_primitive();
    void execute_bitblt();
};

// Puntero global para que las funciones externas de C puedan acceder al GS
extern GS_Core* g_gs_ptr;