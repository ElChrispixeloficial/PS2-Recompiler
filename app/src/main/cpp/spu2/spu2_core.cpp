#include "spu2_core.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <cstring>
#include <cmath>

#define LOG_TAG "PS2-SPU2"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr int NUM_BUFFERS = 4;

static SLObjectItf   sl_engine_obj  = nullptr;
static SLEngineItf   sl_engine      = nullptr;
static SLObjectItf   sl_mix_obj     = nullptr;
static SLObjectItf   sl_player_obj  = nullptr;
static SLPlayItf     sl_play        = nullptr;
static SLAndroidSimpleBufferQueueItf sl_queue = nullptr;

static int16_t audio_buffers[NUM_BUFFERS][SPU2_BUFFER_FRAMES * SPU2_CHANNELS];
static int     buf_idx = 0;

static const int ADPCM_COEF[5][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
};

static uint32_t s_noise_reg = 0x12345678;
static int16_t  s_prev_output[SPU2_VOICES] = {};

static constexpr uint32_t REVERB_BUF_SAMPLES = 128 * 1024;
static constexpr uint32_t REVERB_DELAY        = 4800;
static constexpr float    REVERB_DECAY        = 0.50f;
static constexpr float    REVERB_WET          = 0.25f;
static uint32_t s_reverb_pos = 0;

static SPU2_Core spu2_state;

static int16_t generate_noise() {
    s_noise_reg = (s_noise_reg >> 1) ^ (-(s_noise_reg & 1) & 0xB3000000);
    return (int16_t)(s_noise_reg & 0xFFFF);
}

void SPU2_Core::decode_adpcm_block(SPU2_Voice& v, int16_t out[28]) {
    const uint8_t* block = &ram[v.cur_addr];

    uint8_t shift  = block[0] & 0x0F;
    uint8_t filter = (block[0] >> 4) & 0x07;
    if (filter > 4) filter = 4;

    int f0 = ADPCM_COEF[filter][0];
    int f1 = ADPCM_COEF[filter][1];

    for (int i = 0; i < 28; i++) {
        int nibble = (i & 1) ? (block[2 + i / 2] >> 4) : (block[2 + i / 2] & 0xF);
        int sample = (int16_t)(nibble << 12) >> shift;
        sample = sample + ((v.prev1 * f0 + v.prev2 * f1 + 32) >> 6);

        if (sample >  32767) sample =  32767;
        if (sample < -32768) sample = -32768;

        out[i] = (int16_t)sample;
        v.prev2 = v.prev1;
        v.prev1 = (int16_t)sample;
    }
}

void SPU2_Core::process_adsr(SPU2_Voice& voice) {
    switch (voice.adsr_state) {
        case 0:
            voice.adsr_vol += 0x200;
            if (voice.adsr_vol >= 0x7FFF) {
                voice.adsr_vol = 0x7FFF;
                voice.adsr_state = 1;
            }
            break;
        case 1:
            voice.adsr_vol -= 0x100;
            if (voice.adsr_vol <= 0x3FFF) {
                voice.adsr_vol = 0x3FFF;
                voice.adsr_state = 2;
            }
            break;
        case 2:
            voice.adsr_vol -= 0x20;
            if (voice.adsr_vol < 0) voice.adsr_vol = 0;
            break;
        case 3:
            voice.adsr_vol -= 0x400;
            if (voice.adsr_vol <= 0) {
                voice.adsr_vol = 0;
                voice.key_on = false;
            }
            break;
    }
}

void SPU2_Core::mix(int16_t* output, int frames) {
    memset(output, 0, frames * SPU2_CHANNELS * sizeof(int16_t));

    int16_t* reverb_buf = (int16_t*)(ram + (2 * 1024 * 1024 - REVERB_BUF_SAMPLES * 2));

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
                if (addr + 16 < sizeof(ram)) {
                    decode_adpcm_block(voice, decoded);
                }
                last_addr   = addr;
                decoded_pos = 0;
            }

            int16_t sample;

            if (voice.pitch & 0x1000000) {
                sample = generate_noise();
            } else {
                sample = decoded[decoded_pos];
            }

            if (voice.pitch > 0x8000 && !(voice.pitch & 0x1000000)) {
                float delta = s_prev_output[v] * 0.0001f;
                uint32_t new_pitch = voice.pitch + (uint32_t)(delta);
                if (new_pitch < 0x100)  new_pitch = 0x100;
                if (new_pitch > 0xFFFF) new_pitch = 0xFFFF;
                voice.pitch = (voice.pitch & 0xFF0000) | new_pitch;
            }

            process_adsr(voice);
            int32_t env = voice.adsr_vol >> 15;
            sample = (int16_t)((sample * env) >> 15);

            int32_t l = (sample * (int32_t)voice.vol_l) >> 15;
            int32_t r = (sample * (int32_t)voice.vol_r) >> 15;

            uint32_t rev_write = (s_reverb_pos + i) & (REVERB_BUF_SAMPLES - 1);
            reverb_buf[rev_write] = (int16_t)((l + r) >> 1);

            int32_t ol = (int32_t)output[i * 2]     + l;
            int32_t or_ = (int32_t)output[i * 2 + 1] + r;

            output[i * 2]     = (ol >  32767) ?  32767 : (ol < -32768) ? -32768 : (int16_t)ol;
            output[i * 2 + 1] = (or_ >  32767) ?  32767 : (or_ < -32768) ? -32768 : (int16_t)or_;

            s_prev_output[v] = sample;

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

    for (int i = 0; i < frames; i++) {
        uint32_t read_pos = (s_reverb_pos + i + REVERB_BUF_SAMPLES - REVERB_DELAY)
                            & (REVERB_BUF_SAMPLES - 1);
        float rev = reverb_buf[read_pos] * REVERB_DECAY;
        int32_t rl = (int32_t)output[i * 2]     + (int32_t)(rev * REVERB_WET);
        int32_t rr = (int32_t)output[i * 2 + 1] + (int32_t)(rev * REVERB_WET);
        output[i * 2]     = (rl >  32767) ?  32767 : (rl < -32768) ? -32768 : (int16_t)rl;
        output[i * 2 + 1] = (rr >  32767) ?  32767 : (rr < -32768) ? -32768 : (int16_t)rr;
    }
    s_reverb_pos = (s_reverb_pos + frames) & (REVERB_BUF_SAMPLES - 1);
}

void AudioOutput::audio_callback(SLAndroidSimpleBufferQueueItf queue, void*) {
    int16_t* buf = audio_buffers[buf_idx];
    spu2_state.mix(buf, SPU2_BUFFER_FRAMES);
    (*queue)->Enqueue(queue, buf, SPU2_BUFFER_FRAMES * SPU2_CHANNELS * sizeof(int16_t));
    buf_idx = (buf_idx + 1) % NUM_BUFFERS;
}

SPU2_Core::SPU2_Core() { reset(); }
SPU2_Core::~SPU2_Core() {}

void SPU2_Core::reset() {
    memset(voices, 0, sizeof(voices));
    memset(ram, 0, sizeof(ram));
    memset(m_core0_regs, 0, sizeof(m_core0_regs));
    memset(m_core1_regs, 0, sizeof(m_core1_regs));
    s_noise_reg = 0x12345678;
    memset(s_prev_output, 0, sizeof(s_prev_output));
    s_reverb_pos = 0;
}

uint16_t SPU2_Core::read_reg(uint32_t addr) {
    if (addr >= 0x1F801C00 && addr < 0x1F801C00 + SPU2_VOICES * 16) {
        uint32_t off   = addr - 0x1F801C00;
        uint32_t voice = off / 16;
        uint32_t reg   = off % 16;

        switch (reg) {
            case 0x00: return voices[voice].vol_l;
            case 0x02: return voices[voice].vol_r;
            case 0x04: return voices[voice].pitch & 0xFFFF;
            case 0x06: return m_core0_regs[voice * 4 + 1];
            case 0x08: return m_core0_regs[voice * 4 + 2];
            case 0x0A: return (uint16_t)(voices[voice].adsr_vol & 0x7FFF);
            case 0x0C: return (voices[voice].loop_addr >> 6) & 0xFFFF;
            case 0x0E: return (voices[voice].start_addr >> 6) & 0xFFFF;
        }
    }

    if (addr >= 0x1F801D80 && addr < 0x1F801E00) {
        uint32_t reg = (addr - 0x1F801D80) / 2;
        return m_core1_regs[reg & 0x1FF];
    }

    if (addr == 0x1F801DA0) return 0;
    if (addr == 0x1F801DA2) return 0;
    if (addr == 0x1F801DA4) return 0;

    if (addr >= 0x1F801F00 && addr < 0x1F801F20) {
        uint32_t reg = (addr - 0x1F801F00) / 2;
        return m_core0_regs[0x100 + (reg & 0xFF)];
    }

    return 0;
}

void SPU2_Core::write_reg(uint32_t addr, uint16_t val) {
    if (addr >= 0x1F801C00 && addr < 0x1F801C00 + SPU2_VOICES * 16) {
        uint32_t off   = addr - 0x1F801C00;
        uint32_t voice = off / 16;
        uint32_t reg   = off % 16;

        if (voice >= SPU2_VOICES) return;

        switch (reg) {
            case 0x00: voices[voice].vol_l = val; break;
            case 0x02: voices[voice].vol_r = val; break;
            case 0x04: voices[voice].pitch = (voices[voice].pitch & 0xFF0000) | val; break;
            case 0x06: m_core0_regs[voice * 4 + 1] = val; break;
            case 0x08: m_core0_regs[voice * 4 + 2] = val; break;
            case 0x0C:
                voices[voice].loop_addr = ((uint32_t)val << 6) & 0xFFFFF;
                break;
            case 0x0E:
                voices[voice].start_addr = ((uint32_t)val << 6) & 0xFFFFF;
                break;
        }
    } else if (addr >= 0x1F801D80 && addr < 0x1F801E00) {
        uint32_t reg = (addr - 0x1F801D80) / 2;
        m_core1_regs[reg & 0x1FF] = val;
    } else if (addr >= 0x1F801F00 && addr < 0x1F801F20) {
        uint32_t reg = (addr - 0x1F801F00) / 2;
        m_core0_regs[0x100 + (reg & 0xFF)] = val;
    }
}

void SPU2_Core::dma_write(const uint16_t* data, size_t words) {
    size_t bytes = words * 2;
    if (bytes > sizeof(ram)) bytes = sizeof(ram);
    memcpy(ram, data, bytes);
}

AudioOutput::AudioOutput() {
    m_sl_engine = nullptr;
    m_sl_player = nullptr;
    m_running = false;
    m_spu2 = nullptr;
}

AudioOutput::~AudioOutput() { shutdown(); }

bool AudioOutput::init(SPU2_Core* spu2) {
    m_spu2 = spu2;

    SLEngineOption opts[] = {{ SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE }};
    if (slCreateEngine(&sl_engine_obj, 1, opts, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) {
        LOGE("slCreateEngine failed");
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
        LOGE("CreateAudioPlayer failed");
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

    LOGI("SPU2 started: %d Hz, %d voices, reverb %d ms delay",
         SPU2_SAMPLE_RATE, SPU2_VOICES, REVERB_DELAY * 1000 / SPU2_SAMPLE_RATE);
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

bool SPU2_init() {
    static AudioOutput audio_output;
    return audio_output.init(&spu2_state);
}

void SPU2_key_on(uint32_t voice_mask) {
    for (int v = 0; v < SPU2_VOICES; v++) {
        if (voice_mask & (1u << v)) {
            spu2_state.voices[v].key_on     = true;
            spu2_state.voices[v].cur_addr   = spu2_state.voices[v].start_addr;
            spu2_state.voices[v].adsr_vol   = 0;
            spu2_state.voices[v].adsr_state = 0;
            spu2_state.voices[v].prev1      = 0;
            spu2_state.voices[v].prev2      = 0;
            s_prev_output[v] = 0;
        }
    }
}

void SPU2_key_off(uint32_t voice_mask) {
    for (int v = 0; v < SPU2_VOICES; v++) {
        if (voice_mask & (1u << v)) {
            spu2_state.voices[v].adsr_state = 3;
        }
    }
}
