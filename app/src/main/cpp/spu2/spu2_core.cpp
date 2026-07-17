// ─── SPU2 → Android AudioTrack ───────────────────────────────────────────────
// El SPU2 (Sound Processing Unit 2) de PS2 mezcla hasta 48 voces PCM
// y produce 2 canales estéreo a 48000 Hz.
//
// En lugar de simular el DSP completo, procesamos los registros SPU2
// y enviamos el audio PCM directamente al AudioTrack de Android
// via OpenSL ES / AAudio, que lo pasa directo al hardware de audio.
// Resultado: audio del juego → bocina/auriculares sin capa de simulación.

#include "spu2_core.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <cstring>
#include <cmath>

#define LOG_TAG "PS2-SPU2"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr int NUM_BUFFERS   = 4;

// ─── OpenSL ES ────────────────────────────────────────────────────────────────
static SLObjectItf   sl_engine_obj  = nullptr;
static SLEngineItf   sl_engine      = nullptr;
static SLObjectItf   sl_mix_obj     = nullptr;
static SLObjectItf   sl_player_obj  = nullptr;
static SLPlayItf     sl_play        = nullptr;
static SLAndroidSimpleBufferQueueItf sl_queue = nullptr;

static int16_t audio_buffers[NUM_BUFFERS][SPU2_BUFFER_FRAMES * SPU2_CHANNELS];
static int     buf_idx = 0;

// ─── Decodificador ADPCM de PS2 ───────────────────────────────────────────────
static const int ADPCM_COEF[5][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
};

void SPU2_Core::decode_adpcm_block(SPU2_Voice& v, int16_t out[28]) {
    const uint8_t* block = &ram[v.cur_addr];
    
    uint8_t shift = block[0] & 0x0F;
    uint8_t filter= (block[0] >> 4) & 0x07;
    if (filter > 4) filter = 4;

    int f0 = ADPCM_COEF[filter][0];
    int f1 = ADPCM_COEF[filter][1];

    for (int i = 0; i < 28; i++) {
        int nibble = (i & 1) ? (block[2 + i/2] >> 4) : (block[2 + i/2] & 0xF);
        int sample = (int16_t)(nibble << 12) >> shift;
        sample = sample + ((v.prev1 * f0 + v.prev2 * f1 + 32) >> 6);

        if (sample >  32767) sample =  32767;
        if (sample < -32768) sample = -32768;

        out[i] = (int16_t)sample;
        v.prev2 = v.prev1;
        v.prev1 = (int16_t)sample;
    }
}

// ─── Mezcla de voces → buffer PCM estéreo ────────────────────────────────────
void SPU2_Core::mix(int16_t* output, int frames) {
    memset(output, 0, frames * SPU2_CHANNELS * sizeof(int16_t));

    for (int v = 0; v < SPU2_VOICES; v++) {
        SPU2_Voice& voice = voices[v];
        if (!voice.key_on) continue;

        int decoded_pos = 0;
        uint32_t last_addr = 0xFFFFFFFF;
        int16_t decoded[28];

        for (int i = 0; i < frames; i++) {
            uint32_t addr = voice.cur_addr;
            if (addr + 16 >= sizeof(ram)) {
                voice.key_on = false;
                break;
            }

            if (addr != last_addr || decoded_pos >= 28) {
                decode_adpcm_block(voice, decoded);
                last_addr   = addr;
                decoded_pos = 0;
            }

            int16_t sample = decoded[decoded_pos];

            process_adsr(voice);
            int32_t env = voice.adsr_vol >> 15;
            sample = (sample * env) >> 15;

            int32_t l = (int32_t)output[i*2  ] + ((sample * (int32_t)voice.vol_l) >> 15);
            int32_t r = (int32_t)output[i*2+1] + ((sample * (int32_t)voice.vol_r) >> 15);

            output[i*2  ] = (l >  32767) ?  32767 : (l < -32768) ? -32768 : (int16_t)l;
            output[i*2+1] = (r >  32767) ?  32767 : (r < -32768) ? -32768 : (int16_t)r;

            decoded_pos++;
            if (decoded_pos >= 28) {
                uint8_t flags = ram[addr + 1];
                bool loop_end = (flags & 0x01) != 0;

                if (loop_end) {
                    if (voice.loop_end) {
                        voice.cur_addr = voice.loop_addr;
                    } else {
                        voice.key_on = false;
                    }
                } else {
                    voice.cur_addr += 16;
                }
                decoded_pos = 0;
            }
        }
    }
}

// ─── Procesamiento ADSR simplificado ──────────────────────────────────────────
void SPU2_Core::process_adsr(SPU2_Voice& voice) {
    if (voice.adsr_vol < 0x7FFF) {
        voice.adsr_vol += 0x100;
    }
}

// ─── Callback de OpenSL ES ────────────────────────────────────────────────────
static SPU2_Core spu2_state;

void AudioOutput::audio_callback(SLAndroidSimpleBufferQueueItf queue, void*) {
    int16_t* buf = audio_buffers[buf_idx];
    
    spu2_state.mix(buf, SPU2_BUFFER_FRAMES);

    (*queue)->Enqueue(queue, buf, SPU2_BUFFER_FRAMES * SPU2_CHANNELS * sizeof(int16_t));
    buf_idx = (buf_idx + 1) % NUM_BUFFERS;
}

// ─── Implementación de SPU2_Core ──────────────────────────────────────────────
SPU2_Core::SPU2_Core() {
    reset();
}

SPU2_Core::~SPU2_Core() {}

void SPU2_Core::reset() {
    memset(voices, 0, sizeof(voices));
    memset(ram, 0, sizeof(ram));
    memset(m_core0_regs, 0, sizeof(m_core0_regs));
    memset(m_core1_regs, 0, sizeof(m_core1_regs));
}

uint16_t SPU2_Core::read_reg(uint32_t addr) {
    if (addr >= 0x1F801C00 && addr < 0x1F801C00 + SPU2_VOICES * 16) {
        uint32_t off   = addr - 0x1F801C00;
        uint32_t voice = off / 16;
        uint32_t reg   = off % 16;

        switch (reg) {
            case 0: return voices[voice].vol_l;
            case 2: return voices[voice].vol_r;
            case 4: return voices[voice].pitch;
            case 6: return m_core0_regs[voice * 4 + 1];
            case 8: return m_core0_regs[voice * 4 + 2];
        }
    }
    return 0;
}

void SPU2_Core::write_reg(uint32_t addr, uint16_t val) {
    if (addr >= 0x1F801C00 && addr < 0x1F801C00 + SPU2_VOICES * 16) {
        uint32_t off   = addr - 0x1F801C00;
        uint32_t voice = off / 16;
        uint32_t reg   = off % 16;

        switch (reg) {
            case 0: voices[voice].vol_l  = val; break;
            case 2: voices[voice].vol_r  = val; break;
            case 4: voices[voice].pitch  = val; break;
            case 6: m_core0_regs[voice * 4 + 1] = val; break;
            case 8: m_core0_regs[voice * 4 + 2] = val; break;
        }
    }
}

void SPU2_Core::dma_write(const uint16_t* data, size_t words) {
    memcpy(ram, data, words * 2);
}

// ─── Implementación de AudioOutput ────────────────────────────────────────────
AudioOutput::AudioOutput() {
    m_sl_engine = nullptr;
    m_sl_player = nullptr;
    m_running = false;
    m_spu2 = nullptr;
}

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::init(SPU2_Core* spu2) {
    m_spu2 = spu2;
    
    SLEngineOption opts[] = {{ SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE }};
    if (slCreateEngine(&sl_engine_obj, 1, opts, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) {
        LOGE("slCreateEngine falló");
        return false;
    }
    (*sl_engine_obj)->Realize(sl_engine_obj, SL_BOOLEAN_FALSE);
    (*sl_engine_obj)->GetInterface(sl_engine_obj, SL_IID_ENGINE, &sl_engine);

    (*sl_engine)->CreateOutputMix(sl_engine, &sl_mix_obj, 0, nullptr, nullptr);
    (*sl_mix_obj)->Realize(sl_mix_obj, SL_BOOLEAN_FALSE);

    SLDataLocator_AndroidSimpleBufferQueue bq_loc = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, (SLuint32)NUM_BUFFERS
    };
    SLDataFormat_PCM pcm = {
        SL_DATAFORMAT_PCM,
        (SLuint32)SPU2_CHANNELS,
        SL_SAMPLINGRATE_48,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
        SL_BYTEORDER_LITTLEENDIAN,
    };
    SLDataSource src = { &bq_loc, &pcm };
    SLDataLocator_OutputMix out_loc = { SL_DATALOCATOR_OUTPUTMIX, sl_mix_obj };
    SLDataSink   sink = { &out_loc, nullptr };

    const SLInterfaceID iids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
    const SLboolean req[] = { SL_BOOLEAN_TRUE };

    if ((*sl_engine)->CreateAudioPlayer(sl_engine, &sl_player_obj, &src, &sink, 
                                       1, iids, req) != SL_RESULT_SUCCESS) {
        LOGE("CreateAudioPlayer falló");
        return false;
    }
    (*sl_player_obj)->Realize(sl_player_obj, SL_BOOLEAN_FALSE);
    (*sl_player_obj)->GetInterface(sl_player_obj, SL_IID_PLAY, &sl_play);
    (*sl_player_obj)->GetInterface(sl_player_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &sl_queue);

    (*sl_queue)->RegisterCallback(sl_queue, audio_callback, nullptr);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        memset(audio_buffers[i], 0, sizeof(audio_buffers[i]));
        (*sl_queue)->Enqueue(sl_queue, audio_buffers[i],
                             SPU2_BUFFER_FRAMES * SPU2_CHANNELS * sizeof(int16_t));
    }

    (*sl_play)->SetPlayState(sl_play, SL_PLAYSTATE_PLAYING);
    m_running = true;
    
    LOGI("SPU2 iniciado: %d Hz, %d voces", SPU2_SAMPLE_RATE, SPU2_VOICES);
    return true;
}

void AudioOutput::shutdown() {
    if (sl_play != nullptr) {
        (*sl_play)->SetPlayState(sl_play, SL_PLAYSTATE_STOPPED);
    }
    if (sl_player_obj != nullptr) {
        (*sl_player_obj)->Destroy(sl_player_obj);
        sl_player_obj = nullptr;
    }
    if (sl_mix_obj != nullptr) {
        (*sl_mix_obj)->Destroy(sl_mix_obj);
        sl_mix_obj = nullptr;
    }
    if (sl_engine_obj != nullptr) {
        (*sl_engine_obj)->Destroy(sl_engine_obj);
        sl_engine_obj = nullptr;
    }
    m_running = false;
}

// ─── Funciones de compatibilidad ──────────────────────────────────────────────

bool SPU2_init() {
    static AudioOutput audio_output;
    return audio_output.init(&spu2_state);
}

void SPU2_key_on(uint32_t voice_mask) {
    for (int v = 0; v < SPU2_VOICES; v++) {
        if (voice_mask & (1 << v)) {
            spu2_state.voices[v].key_on     = true;
            spu2_state.voices[v].cur_addr   = spu2_state.voices[v].start_addr;
            spu2_state.voices[v].adsr_vol   = 0;
            spu2_state.voices[v].adsr_state = 0;
            spu2_state.voices[v].prev1      = 0;
            spu2_state.voices[v].prev2      = 0;
        }
    }
}

void SPU2_key_off(uint32_t voice_mask) {
    for (int v = 0; v < SPU2_VOICES; v++) {
        if (voice_mask & (1 << v)) {
            spu2_state.voices[v].adsr_state = 3;
        }
    }
}
