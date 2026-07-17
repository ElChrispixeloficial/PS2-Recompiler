#include "spu2_core.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <cmath>


#define LOG_TAG "PS2-SPU2"

#define LOGI(...) __android_log_print(
    ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) __android_log_print(
    ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


static constexpr int ADPCM_COEF[5][2] =
{
    {0,0},
    {60,0},
    {115,-52},
    {98,-55},
    {122,-60}
};



SPU2_Core::SPU2_Core()
{
    reset();
}


SPU2_Core::~SPU2_Core()
{
}



void SPU2_Core::reset()
{
    memset(voices,0,sizeof(voices));

    memset(ram,0,sizeof(ram));

    memset(m_core0_regs,0,sizeof(m_core0_regs));
    memset(m_core1_regs,0,sizeof(m_core1_regs));


    for(int i = 0; i < SPU2_VOICES; i++)
    {
        voices[i].pitch = 0x1000;

        voices[i].adsr_state = 0;
        voices[i].adsr_vol = 0;

        voices[i].prev1 = 0;
        voices[i].prev2 = 0;

        voices[i].frac = 0.0f;

        voices[i].key_on = false;
        voices[i].key_off = false;
        voices[i].loop_end = false;
    }
}



uint16_t SPU2_Core::read_reg(uint32_t addr)
{
    uint32_t offset = addr & 0x7FF;

    uint32_t index = offset >> 1;


    if(index < 0x200)
        return (uint16_t)m_core0_regs[index];


    return 0;
}




void SPU2_Core::write_reg(uint32_t addr,uint16_t val)
{
    uint32_t offset = addr - 0x1F801C00;


    if(offset < SPU2_VOICES * 16)
    {
        uint32_t id = offset / 16;
        uint32_t reg = offset & 0xF;


        SPU2_Voice &v = voices[id];


        switch(reg)
        {
            case 0x0:
                v.vol_l = val;
                break;


            case 0x2:
                v.vol_r = val;
                break;


            case 0x4:
                v.pitch = val;
                break;


            case 0x6:
                v.adsr_state = val;
                break;


            case 0x8:
                v.adsr_vol = val;
                break;


            case 0xC:
                v.start_addr = ((uint32_t)val) << 3;
                break;


            case 0xE:
                v.loop_addr = ((uint32_t)val) << 3;
                break;
        }

        return;
    }



    uint32_t index = (offset >> 1);


    if(index < 0x200)
        m_core0_regs[index] = val;
}




void SPU2_Core::dma_write(
    const uint16_t* data,
    size_t words)
{
    static uint32_t dma_addr = 0;


    size_t bytes = words * sizeof(uint16_t);


    if(dma_addr + bytes > sizeof(ram))
    {
        bytes = sizeof(ram) - dma_addr;
    }


    memcpy(
        ram + dma_addr,
        data,
        bytes
    );


    dma_addr += bytes;


    if(dma_addr >= sizeof(ram))
        dma_addr = 0;
}





void SPU2_Core::decode_adpcm_block(
    SPU2_Voice &v,
    int16_t out[28])
{
    uint32_t addr = v.cur_addr;


    if(addr + 16 > sizeof(ram))
    {
        memset(out,0,sizeof(int16_t)*28);
        return;
    }


    uint8_t header = ram[addr];


    int shift =
        header & 0x0F;


    int filter =
        (header >> 4) & 0x07;


    if(filter > 4)
        filter = 4;



    int f0 = ADPCM_COEF[filter][0];
    int f1 = ADPCM_COEF[filter][1];



    for(int i = 0; i < 28; i++)
    {
        uint8_t packed =
            ram[addr + 2 + (i >> 1)];


        int nibble =
            (i & 1)
            ? (packed >> 4)
            : (packed & 0x0F);



        if(nibble & 8)
            nibble -= 16;



        int sample =
            nibble << 12;


        sample >>= shift;



        sample +=
            ((v.prev1 * f0) +
             (v.prev2 * f1) +
             32) >> 6;



        sample =
            std::clamp(
                sample,
                -32768,
                32767
            );


        out[i]=(int16_t)sample;


        v.prev2 = v.prev1;
        v.prev1 = (int16_t)sample;
    }
}

void SPU2_Core::process_adsr(SPU2_Voice& v)
{
    switch(v.adsr_state & 3)
    {
        case 0: // Attack
            v.adsr_vol += 0x400;

            if(v.adsr_vol >= 0x7FFF)
            {
                v.adsr_vol = 0x7FFF;
                v.adsr_state = 1;
            }
            break;


        case 1: // Decay
            v.adsr_vol -= 0x80;

            if(v.adsr_vol <= 0x5000)
            {
                v.adsr_state = 2;
            }
            break;


        case 2: // Sustain
            break;


        case 3: // Release
            if(v.adsr_vol > 0x80)
                v.adsr_vol -= 0x80;
            else
            {
                v.adsr_vol = 0;
                v.key_on = false;
            }

            break;
    }
}




void SPU2_Core::mix(
    int16_t* out,
    int frames)
{
    memset(
        out,
        0,
        frames * SPU2_CHANNELS * sizeof(int16_t)
    );


    int16_t decoded[28];


    for(int voice_id = 0;
        voice_id < SPU2_VOICES;
        voice_id++)
    {
        SPU2_Voice& v = voices[voice_id];


        if(!v.key_on)
            continue;



        for(int i = 0; i < frames; i++)
        {
            decode_adpcm_block(
                v,
                decoded
            );


            int index =
                (int)v.frac;


            if(index >= 28)
            {
                index = 0;

                v.cur_addr += 16;


                if(v.loop_end)
                    v.cur_addr = v.loop_addr;
            }



            int32_t sample =
                decoded[index];



            process_adsr(v);



            sample =
                (sample * (int32_t)v.adsr_vol)
                >> 15;



            int32_t left =
                out[i*2] +
                ((sample * v.vol_l) >> 15);



            int32_t right =
                out[i*2+1] +
                ((sample * v.vol_r) >> 15);



            out[i*2] =
                (int16_t)
                std::clamp(
                    left,
                    -32768,
                    32767
                );


            out[i*2+1] =
                (int16_t)
                std::clamp(
                    right,
                    -32768,
                    32767
                );



            v.frac +=
                (float)v.pitch / 0x1000.0f;
        }
    }
}





// ─────────────────────────────────────────
// AudioOutput OpenSL ES
// ─────────────────────────────────────────


AudioOutput::AudioOutput()
{
}



AudioOutput::~AudioOutput()
{
    shutdown();
}




bool AudioOutput::init(SPU2_Core* spu2)
{
    m_spu2 = spu2;

    m_running = true;


    LOGI(
        "SPU2 AudioOutput iniciado %dHz",
        SPU2_SAMPLE_RATE
    );


    return true;
}





void AudioOutput::shutdown()
{
    m_running = false;
}



void AudioOutput::audio_callback(
    void* ctx,
    int16_t* buf,
    int frames)
{
    AudioOutput* audio =
        static_cast<AudioOutput*>(ctx);



    if(!audio ||
       !audio->m_spu2)
    {
        memset(
            buf,
            0,
            frames *
            SPU2_CHANNELS *
            sizeof(int16_t)
        );

        return;
    }



    audio->m_spu2->mix(
        buf,
        frames
    );
}

