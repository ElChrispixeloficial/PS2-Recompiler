#pragma once
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <cstdint>
#include <cstddef>

constexpr int SPU2_VOICES       = 24;
constexpr int SPU2_SAMPLE_RATE  = 48000;
constexpr int SPU2_CHANNELS     = 2;
constexpr int SPU2_BUFFER_FRAMES= 1024;

struct SPU2_Voice {
    uint32_t start_addr;    /* dirección ADPCM en SPU2 RAM */
    uint32_t loop_addr;
    uint32_t cur_addr;

    uint32_t pitch;         /* 0x1000 = 44100 Hz */
    uint32_t adsr_state;    /* Attack/Decay/Sustain/Release */
    uint32_t adsr_vol;      /* volumen ADSR actual (0-0x7FFF) */
    uint32_t vol_l;
    uint32_t vol_r;

    int16_t  prev1, prev2;  /* predicción ADPCM */
    float    frac;          /* fracción de sample para resampling */

    bool     key_on;
    bool     key_off;
    bool     loop_end;
};

class SPU2_Core {
public:
    SPU2_Core();
    ~SPU2_Core();

    void reset();

    /* Acceso registro SPU2 (mapeado en memoria IOP) */
    uint16_t read_reg(uint32_t addr);
    void     write_reg(uint32_t addr, uint16_t val);

    /* DMA: recibir bloques ADPCM desde IOP */
    void dma_write(const uint16_t* data, size_t words);

    /* Mezcla SPU2_BUFFER_FRAMES muestras estéreo en out[] */
    void mix(int16_t* out, int frames);

    SPU2_Voice voices[SPU2_VOICES];
    uint8_t    ram[2 * 1024 * 1024]; /* 2 MB SPU2 RAM */

private:
    void decode_adpcm_block(SPU2_Voice& v, int16_t out[28]);
    void process_adsr(SPU2_Voice& v);

    uint32_t m_core0_regs[0x200];
    uint32_t m_core1_regs[0x200];
};

/* Audio output usando OpenSL ES */
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    bool init(SPU2_Core* spu2);
    void shutdown();
    bool is_running() const { return m_running; }

private:
    static void audio_callback(SLAndroidSimpleBufferQueueItf queue, void* ctx);
    void* m_sl_engine   = nullptr;
    void* m_sl_player   = nullptr;
    bool  m_running     = false;
    SPU2_Core* m_spu2   = nullptr;
};
// ─── Funciones de inicialización del SPU2 ────────────────────────────────────
bool SPU2_init();
void SPU2_key_on(uint32_t voice_mask);
void SPU2_key_off(uint32_t voice_mask);
