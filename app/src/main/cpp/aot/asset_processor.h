#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct TextureAsset {
    uint32_t vram_addr;
    uint16_t width;
    uint16_t height;
    uint32_t format;
    std::vector<uint8_t> rgba_data;
    std::string filename;
};

struct AudioAsset {
    uint32_t spu2_addr;
    uint32_t size;
    uint16_t sample_rate;
    uint16_t channels;
    std::vector<int16_t> pcm_data;
    std::string filename;
};

struct ModelAsset {
    uint32_t vu1_addr;
    uint32_t vertex_count;
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<uint32_t> indices;
    std::string filename;
};

class Asset_Processor {
public:
    void process_all(const uint8_t* vram, size_t vram_size,
                     const uint8_t* spu2_ram, size_t spu2_size,
                     const uint8_t* ee_ram, size_t ee_size,
                     const std::string& out_dir);

    std::vector<TextureAsset> extract_textures(const uint8_t* vram, size_t vram_size);
    std::vector<AudioAsset> extract_audio(const uint8_t* spu2_ram, size_t spu2_size);
    std::vector<ModelAsset> extract_models(const uint8_t* ee_ram, size_t ee_size);

    void write_texture_png(const TextureAsset& tex, const std::string& path);
    void write_audio_wav(const AudioAsset& audio, const std::string& path);
    void write_model_obj(const ModelAsset& model, const std::string& path);

private:
    void decode_ps2_texture(const uint8_t* src, uint16_t w, uint16_t h,
                           uint32_t format, std::vector<uint8_t>& out_rgba);
    void decode_adpcm_block(const int16_t* input, int16_t* output, int count);
    void write_wav_header(FILE* f, uint32_t data_size, uint16_t channels,
                          uint32_t sample_rate, uint16_t bits);
};
