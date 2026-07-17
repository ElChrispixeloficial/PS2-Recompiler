#pragma once
#include <cstdint>
#include <cstddef>

struct PS2_LoadResult {
    bool     success;
    uint32_t entry_point;          // dirección EE del punto de entrada (ELF entry)
    char     game_id[32];          // ej: "SLUS_123.45"
};

class ISO_Loader {
public:
    // Carga un juego de PS2 desde una ISO en la RAM del EE.
    // ee_ram debe apuntar a un buffer de al menos 32 MB.
    static PS2_LoadResult load(const char* iso_path,
                                uint8_t*    ee_ram,
                                size_t      ee_ram_size);
};
