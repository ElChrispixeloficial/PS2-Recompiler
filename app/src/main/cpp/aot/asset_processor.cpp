#include "asset_processor.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <android/log.h>

#define LOG_TAG "Asset_Processor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#pragma pack(push, 1)

struct GS_TEX0_Reg {
    uint64_t tex_base     : 14;
    uint64_t psm          : 6;
    uint64_t tw           : 4;
    uint64_t th           : 4;
    uint64_t tex_cc       : 1;
    uint64_t tex_function : 2;
    uint64_t clut_base   : 14;
    uint64_t clut_psm    : 6;
    uint64_t clut_mode   : 4;
    uint64_t csm          : 1;
    uint64_t clut_offset : 5;
    uint64_t clut_load   : 1;
    uint64_t pad          : 3;
};

struct WAVHeader {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
};

#pragma pack(pop)

static constexpr uint32_t PS2_GS_PSMCT32  = 0x00;
static constexpr uint32_t PS2_GS_PSMCT24  = 0x01;
static constexpr uint32_t PS2_GS_PSMCT16  = 0x02;
static constexpr uint32_t PS2_GS_PSMCT8   = 0x13;
static constexpr uint32_t PS2_GS_PSMCT4   = 0x14;

static constexpr size_t PS2_SPU2_RAM_SIZE = 0x200000;
static constexpr size_t PS2_EE_RAM_SIZE   = 0x2000000;
static constexpr size_t PS2_VRAM_SIZE     = 0x400000;
static constexpr int    SPU2_NUM_VOICES   = 24;

static constexpr float PS2_ADPCM_TABLE[5][2] = {
    { 0.0f,     0.0f },
    { 0.9375f,  0.0f },
    { 1.796875f, -0.8125f },
    { 1.53125f, -0.859375f },
    { 1.90625f,  0.0f },
};

static bool write_file(const std::string& path, const void* data, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGE("Failed to open file for writing: %s", path.c_str());
        return false;
    }
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

static uint32_t log2_ceil(uint32_t v) {
    if (v == 0) return 0;
    uint32_t r = 0;
    while ((1u << r) < v) r++;
    return r;
}

static uint16_t swap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0x000000FF) |
           ((v >> 8)  & 0x0000FF00) |
           ((v << 8)  & 0x00FF0000) |
           ((v << 24) & 0xFF000000);
}

static inline uint8_t read_u8(const uint8_t* base, size_t offset) {
    return base[offset];
}

static inline uint16_t read_u16(const uint8_t* base, size_t offset) {
    uint16_t v;
    memcpy(&v, base + offset, sizeof(uint16_t));
    return v;
}

static inline uint32_t read_u32(const uint8_t* base, size_t offset) {
    uint32_t v;
    memcpy(&v, base + offset, sizeof(uint32_t));
    return v;
}

static inline uint64_t read_u64(const uint8_t* base, size_t offset) {
    uint64_t v;
    memcpy(&v, base + offset, sizeof(uint64_t));
    return v;
}

void Asset_Processor::process_all(const uint8_t* vram, size_t vram_size,
                                  const uint8_t* spu2_ram, size_t spu2_size,
                                  const uint8_t* ee_ram, size_t ee_size,
                                  const std::string& out_dir) {
    LOGI("process_all: starting asset extraction");

    auto textures = extract_textures(vram, vram_size);
    LOGI("process_all: found %zu textures", textures.size());

    auto audio = extract_audio(spu2_ram, spu2_size);
    LOGI("process_all: found %zu audio assets", audio.size());

    auto models = extract_models(ee_ram, ee_size);
    LOGI("process_all: found %zu models", models.size());

    for (size_t i = 0; i < textures.size(); i++) {
        std::string path = out_dir + "/" + textures[i].filename;
        write_texture_png(textures[i], path);
    }

    for (size_t i = 0; i < audio.size(); i++) {
        std::string path = out_dir + "/" + audio[i].filename;
        write_audio_wav(audio[i], path);
    }

    for (size_t i = 0; i < models.size(); i++) {
        std::string path = out_dir + "/" + models[i].filename;
        write_model_obj(models[i], path);
    }

    LOGI("process_all: extraction complete");
}

void Asset_Processor::decode_ps2_texture(const uint8_t* src, uint16_t w, uint16_t h,
                                          uint32_t format, std::vector<uint8_t>& out_rgba) {
    size_t pixel_count = static_cast<size_t>(w) * h;
    out_rgba.resize(pixel_count * 4);

    switch (format) {
        case PS2_GS_PSMCT32: {
            for (size_t i = 0; i < pixel_count; i++) {
                uint32_t c = read_u32(src, i * 4);
                out_rgba[i * 4 + 0] = c & 0xFF;
                out_rgba[i * 4 + 1] = (c >> 8) & 0xFF;
                out_rgba[i * 4 + 2] = (c >> 16) & 0xFF;
                out_rgba[i * 4 + 3] = (c >> 24) & 0xFF;
            }
            break;
        }

        case PS2_GS_PSMCT24: {
            for (size_t i = 0; i < pixel_count; i++) {
                uint32_t c = read_u32(src, i * 3);
                c &= 0x00FFFFFF;
                out_rgba[i * 4 + 0] = c & 0xFF;
                out_rgba[i * 4 + 1] = (c >> 8) & 0xFF;
                out_rgba[i * 4 + 2] = (c >> 16) & 0xFF;
                out_rgba[i * 4 + 3] = 0xFF;
            }
            break;
        }

        case PS2_GS_PSMCT16: {
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t c = read_u16(src, i * 2);
                uint8_t r = ((c >> 0) & 0x1F) << 3;
                uint8_t g = ((c >> 5) & 0x1F) << 3;
                uint8_t b = ((c >> 10) & 0x1F) << 3;
                uint8_t a = (c & 0x8000) ? 0xFF : 0x00;
                out_rgba[i * 4 + 0] = r;
                out_rgba[i * 4 + 1] = g;
                out_rgba[i * 4 + 2] = b;
                out_rgba[i * 4 + 3] = a;
            }
            break;
        }

        case PS2_GS_PSMCT8: {
            const uint8_t* pal = src + pixel_count;
            for (size_t i = 0; i < pixel_count; i++) {
                uint8_t idx = src[i];
                uint16_t entry = read_u16(pal, idx * 2);
                out_rgba[i * 4 + 0] = ((entry >> 0) & 0x1F) << 3;
                out_rgba[i * 4 + 1] = ((entry >> 5) & 0x1F) << 3;
                out_rgba[i * 4 + 2] = ((entry >> 10) & 0x1F) << 3;
                out_rgba[i * 4 + 3] = (entry & 0x8000) ? 0xFF : 0x00;
            }
            break;
        }

        case PS2_GS_PSMCT4: {
            const uint8_t* pal = src + (pixel_count / 2);
            for (size_t i = 0; i < pixel_count; i++) {
                uint8_t byte = src[i / 2];
                uint8_t idx = (i & 1) ? (byte >> 4) : (byte & 0x0F);
                uint16_t entry = read_u16(pal, idx * 2);
                out_rgba[i * 4 + 0] = ((entry >> 0) & 0x1F) << 3;
                out_rgba[i * 4 + 1] = ((entry >> 5) & 0x1F) << 3;
                out_rgba[i * 4 + 2] = ((entry >> 10) & 0x1F) << 3;
                out_rgba[i * 4 + 3] = (entry & 0x8000) ? 0xFF : 0x00;
            }
            break;
        }

        default: {
            LOGE("decode_ps2_texture: unknown format 0x%X", format);
            memset(out_rgba.data(), 0xFF, out_rgba.size());
            break;
        }
    }
}

std::vector<TextureAsset> Asset_Processor::extract_textures(const uint8_t* vram, size_t vram_size) {
    std::vector<TextureAsset> textures;

    if (!vram || vram_size < 64) {
        LOGE("extract_textures: invalid VRAM");
        return textures;
    }

    size_t scan_limit = (vram_size > 0x400000) ? 0x400000 : vram_size;

    for (size_t off = 0; off + 64 <= scan_limit; off += 64) {
        uint64_t raw = read_u64(vram, off);
        if (raw == 0) continue;

        GS_TEX0_Reg tex0;
        memcpy(&tex0, &raw, sizeof(uint64_t));

        uint32_t psm = tex0.psm;
        if (psm > 0x14) continue;

        bool valid_psm = (psm == PS2_GS_PSMCT32 || psm == PS2_GS_PSMCT24 ||
                          psm == PS2_GS_PSMCT16 || psm == PS2_GS_PSMCT8 ||
                          psm == PS2_GS_PSMCT4);
        if (!valid_psm) continue;

        if (tex0.tw == 0 || tex0.th == 0 || tex0.tw > 10 || tex0.th > 10) continue;

        uint16_t w = 1u << tex0.tw;
        uint16_t h = 1u << tex0.th;

        uint32_t tex_base_addr = tex0.tex_base * 256;
        if (tex_base_addr + w * h * 4 > scan_limit) continue;

        TextureAsset ta;
        ta.vram_addr = tex_base_addr;
        ta.width = w;
        ta.height = h;
        ta.format = psm;

        decode_ps2_texture(vram + tex_base_addr, w, h, psm, ta.rgba_data);

        char fname[64];
        snprintf(fname, sizeof(fname), "tex_%08X.png", tex_base_addr);
        ta.filename = fname;

        textures.push_back(std::move(ta));
    }

    LOGI("extract_textures: scanned VRAM, found %zu texture(s)", textures.size());
    return textures;
}

void Asset_Processor::decode_adpcm_block(const int16_t* input, int16_t* output, int count) {
    static constexpr int SPU2_ADPCM_FILTERS[5][2] = {
        { 0, 0 },
        { 60, 0 },
        { 115, -52 },
        { 98, -55 },
        { 122, -60 },
    };

    int32_t hist1 = 0;
    int32_t hist2 = 0;

    for (int blk = 0; blk < count; blk++) {
        uint16_t header = static_cast<uint16_t>(input[blk * 16]);
        int filter_idx = (header >> 4) & 0x0F;
        int shift = header & 0x0F;
        if (shift > 12) shift = 12;

        int f0 = SPU2_ADPCM_FILTERS[filter_idx][0];
        int f1 = SPU2_ADPCM_FILTERS[filter_idx][1];

        const uint8_t* samples_bytes = reinterpret_cast<const uint8_t*>(&input[blk * 16 + 1]);

        for (int s = 0; s < 14; s++) {
            uint8_t nybble;
            if (s & 1) {
                nybble = samples_bytes[s >> 1] >> 4;
            } else {
                nybble = samples_bytes[s >> 1] & 0x0F;
            }

            int32_t sample = static_cast<int32_t>(nybble);
            if (sample >= 8) sample -= 16;

            sample = (sample << shift) + ((hist1 * f0 + hist2 * f1 + 32) >> 6);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            hist2 = hist1;
            hist1 = sample;

            output[blk * 14 + s] = static_cast<int16_t>(sample);
        }

        if (header & 0x10) hist1 = 0;
        if (header & 0x20) hist2 = 0;
    }
}

std::vector<AudioAsset> Asset_Processor::extract_audio(const uint8_t* spu2_ram, size_t spu2_size) {
    std::vector<AudioAsset> audio;

    if (!spu2_ram || spu2_size < 256) {
        LOGE("extract_audio: invalid SPU2 RAM");
        return audio;
    }

    size_t scan_limit = (spu2_size > PS2_SPU2_RAM_SIZE) ? PS2_SPU2_RAM_SIZE : spu2_size;

    constexpr uint16_t SPU2_VOICE_STRIDE = 0x200;
    constexpr int MIN_AUDIO_BLOCKS = 4;

    for (int voice = 0; voice < SPU2_NUM_VOICES; voice++) {
        size_t voice_off = static_cast<size_t>(voice) * SPU2_VOICE_STRIDE;
        if (voice_off + SPU2_VOICE_STRIDE > scan_limit) break;

        uint16_t sa = read_u16(spu2_ram, voice_off + 0x0A);
        size_t data_off = static_cast<size_t>(sa) * 8;

        if (data_off == 0 || data_off >= scan_limit) continue;

        size_t remaining = scan_limit - data_off;
        if (remaining < 256) continue;

        int block_count = 0;
        size_t probe = data_off;
        while (probe + 32 <= scan_limit && block_count < 1024) {
            uint16_t hdr = read_u16(spu2_ram, probe);
            int shift = hdr & 0x0F;
            int filt = (hdr >> 4) & 0x0F;
            if (shift > 12 || filt > 4) break;

            bool has_nonzero = false;
            for (size_t b = 2; b < 32; b++) {
                if (spu2_ram[probe + b] != 0) {
                    has_nonzero = true;
                    break;
                }
            }
            if (!has_nonzero && block_count > MIN_AUDIO_BLOCKS) break;

            block_count++;
            probe += 32;
        }

        if (block_count < MIN_AUDIO_BLOCKS) continue;

        size_t bytes_consumed = static_cast<size_t>(block_count) * 32;
        if (data_off + bytes_consumed > scan_limit) continue;

        std::vector<int16_t> pcm(block_count * 14);
        decode_adpcm_block(reinterpret_cast<const int16_t*>(spu2_ram + data_off),
                           pcm.data(), block_count);

        AudioAsset aa;
        aa.spu2_addr = static_cast<uint32_t>(sa);
        aa.size = static_cast<uint32_t>(bytes_consumed);
        aa.sample_rate = 44100;
        aa.channels = 1;
        aa.pcm_data = std::move(pcm);

        char fname[64];
        snprintf(fname, sizeof(fname), "audio_%08X.wav", aa.spu2_addr);
        aa.filename = fname;

        audio.push_back(std::move(aa));
    }

    LOGI("extract_audio: found %zu audio asset(s)", audio.size());
    return audio;
}

std::vector<ModelAsset> Asset_Processor::extract_models(const uint8_t* ee_ram, size_t ee_size) {
    std::vector<ModelAsset> models;

    if (!ee_ram || ee_size < 256) {
        LOGE("extract_models: invalid EE RAM");
        return models;
    }

    size_t scan_limit = (ee_size > PS2_EE_RAM_SIZE) ? PS2_EE_RAM_SIZE : ee_size;

    constexpr size_t ALIGN = 16;
    constexpr float MIN_COORD = -10000.0f;
    constexpr float MAX_COORD = 10000.0f;

    for (size_t off = 0; off + 64 <= scan_limit; off += ALIGN) {
        uint32_t tag0 = read_u32(ee_ram, off);
        uint32_t tag1 = read_u32(ee_ram, off + 4);

        bool looks_like_vif = ((tag0 & 0xFF000000) != 0) &&
                              ((tag1 & 0xFF000000) == 0) &&
                              (tag0 != 0xFFFFFFFF);

        bool looks_like_gif = (tag0 == 0x60000000 || tag0 == 0x30000000);

        if (!looks_like_vif && !looks_like_gif) continue;

        std::vector<float> verts;
        std::vector<float> norms;
        std::vector<float> texcs;
        size_t scan_end = off + 2048;
        if (scan_end > scan_limit) scan_end = scan_limit;

        size_t p = off + 8;

        while (p + 12 <= scan_end) {
            float f0, f1, f2;
            memcpy(&f0, ee_ram + p,     sizeof(float));
            memcpy(&f1, ee_ram + p + 4, sizeof(float));
            memcpy(&f2, ee_ram + p + 8, sizeof(float));

            bool valid = true;
            for (float f : {f0, f1, f2}) {
                if (std::isnan(f) || std::isinf(f) || f < MIN_COORD || f > MAX_COORD) {
                    valid = false;
                    break;
                }
            }

            if (valid) {
                uint32_t check = 0;
                memcpy(&check, ee_ram + p, sizeof(uint32_t));
                if (check == 0) {
                    p += ALIGN;
                    continue;
                }

                verts.push_back(f0);
                verts.push_back(f1);
                verts.push_back(f2);
                p += 12;
            } else {
                p += 4;
            }
        }

        if (verts.size() < 9) continue;

        ModelAsset ma;
        ma.vu1_addr = static_cast<uint32_t>(off);
        ma.vertex_count = static_cast<uint32_t>(verts.size() / 3);
        ma.vertices = std::move(verts);

        ma.indices.clear();
        for (uint32_t i = 0; i < ma.vertex_count; i++) {
            ma.indices.push_back(i);
        }

        char fname[64];
        snprintf(fname, sizeof(fname), "model_%08X.obj", off);
        ma.filename = fname;

        models.push_back(std::move(ma));
    }

    LOGI("extract_models: found %zu model(s)", models.size());
    return models;
}

void Asset_Processor::write_texture_png(const TextureAsset& tex, const std::string& path) {
    uint16_t w = tex.width;
    uint16_t h = tex.height;
    size_t rgba_size = static_cast<size_t>(w) * h * 4;

    if (tex.rgba_data.size() != rgba_size) {
        LOGE("write_texture_png: size mismatch for %s", path.c_str());
        return;
    }

    size_t header_size = 256;
    std::vector<uint8_t> file_buf(header_size + rgba_size);
    int off = snprintf(reinterpret_cast<char*>(file_buf.data()), header_size,
                       "RGBA8888 %u %u\n", w, h);
    if (off < 0 || static_cast<size_t>(off) >= header_size) off = header_size - 1;

    memcpy(file_buf.data() + header_size, tex.rgba_data.data(), rgba_size);

    if (!write_file(path, file_buf.data(), header_size + rgba_size)) {
        LOGE("write_texture_png: failed to write %s", path.c_str());
    } else {
        LOGI("write_texture_png: wrote %s (%ux%u)", path.c_str(), w, h);
    }
}

void Asset_Processor::write_wav_header(FILE* f, uint32_t data_size, uint16_t channels,
                                        uint32_t sample_rate, uint16_t bits) {
    WAVHeader hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.file_size = 36 + data_size;
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.num_channels = channels;
    hdr.sample_rate = sample_rate;
    hdr.bits_per_sample = bits;
    hdr.byte_rate = sample_rate * channels * (bits / 8);
    hdr.block_align = channels * (bits / 8);
    memcpy(hdr.data, "data", 4);
    hdr.data_size = data_size;

    fwrite(&hdr, sizeof(WAVHeader), 1, f);
}

void Asset_Processor::write_audio_wav(const AudioAsset& audio, const std::string& path) {
    if (audio.pcm_data.empty()) {
        LOGE("write_audio_wav: empty PCM data for %s", path.c_str());
        return;
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGE("write_audio_wav: failed to open %s", path.c_str());
        return;
    }

    uint32_t data_size = static_cast<uint32_t>(audio.pcm_data.size() * sizeof(int16_t));
    write_wav_header(f, data_size, audio.channels, audio.sample_rate, 16);
    fwrite(audio.pcm_data.data(), sizeof(int16_t), audio.pcm_data.size(), f);
    fclose(f);

    LOGI("write_audio_wav: wrote %s (%u samples, %u Hz)",
         path.c_str(), static_cast<uint32_t>(audio.pcm_data.size()), audio.sample_rate);
}

void Asset_Processor::write_model_obj(const ModelAsset& model, const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        LOGE("write_model_obj: failed to open %s", path.c_str());
        return;
    }

    fprintf(f, "# PS2 extracted model\n");
    fprintf(f, "# vertices: %u\n\n", model.vertex_count);

    uint32_t vert_count = model.vertex_count;
    for (uint32_t i = 0; i < vert_count; i++) {
        float x = (i * 3 + 0 < model.vertices.size()) ? model.vertices[i * 3 + 0] : 0.0f;
        float y = (i * 3 + 1 < model.vertices.size()) ? model.vertices[i * 3 + 1] : 0.0f;
        float z = (i * 3 + 2 < model.vertices.size()) ? model.vertices[i * 3 + 2] : 0.0f;
        fprintf(f, "v %f %f %f\n", x, y, z);
    }

    fprintf(f, "\n");

    uint32_t normal_count = static_cast<uint32_t>(model.normals.size() / 3);
    for (uint32_t i = 0; i < normal_count; i++) {
        fprintf(f, "vn %f %f %f\n",
                model.normals[i * 3 + 0],
                model.normals[i * 3 + 1],
                model.normals[i * 3 + 2]);
    }

    if (normal_count > 0) fprintf(f, "\n");

    uint32_t tc_count = static_cast<uint32_t>(model.texcoords.size() / 2);
    for (uint32_t i = 0; i < tc_count; i++) {
        fprintf(f, "vt %f %f\n",
                model.texcoords[i * 2 + 0],
                model.texcoords[i * 2 + 1]);
    }

    if (tc_count > 0) fprintf(f, "\n");

    fprintf(f, "# faces\n");
    for (size_t i = 0; i + 2 < model.indices.size(); i += 3) {
        uint32_t a = model.indices[i] + 1;
        uint32_t b = model.indices[i + 1] + 1;
        uint32_t c = model.indices[i + 2] + 1;
        if (tc_count > 0 && normal_count > 0) {
            fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n",
                    a, a, a, b, b, b, c, c, c);
        } else if (tc_count > 0) {
            fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, b, b, c, c);
        } else if (normal_count > 0) {
            fprintf(f, "f %u//%u %u//%u %u//%u\n", a, a, b, b, c, c);
        } else {
            fprintf(f, "f %u %u %u\n", a, b, c);
        }
    }

    fclose(f);
    LOGI("write_model_obj: wrote %s (%u vertices)", path.c_str(), model.vertex_count);
}
