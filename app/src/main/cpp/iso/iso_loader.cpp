// ─── PS2 ISO Loader ───────────────────────────────────────────────────────────
#include "iso_loader.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <android/log.h>
#include <cerrno>

#define LOG_TAG "PS2-ISO"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr int SECTOR_SIZE = 2048;
static constexpr int PVD_SECTOR  = 16;

#pragma pack(push, 1)
struct ISO9660_DirRecord {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint32_t lba_le, lba_be;
    uint32_t size_le, size_be;
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  unit_size;
    uint8_t  gap_size;
    uint16_t vol_seq_le, vol_seq_be;
    uint8_t  name_len;
    char     name[1];
};

struct ISO9660_PVD {
    uint8_t  type;
    char     id[5];
    uint8_t  version;
    uint8_t  unused1;
    char     sys_id[32];
    char     vol_id[32];
    uint8_t  unused2[8];
    uint32_t vol_size_le, vol_size_be;
    uint8_t  unused3[32];
    uint16_t vol_set_le, vol_set_be;
    uint16_t vol_seq_le, vol_seq_be;
    uint16_t logical_block_le, logical_block_be;
    uint32_t path_table_size_le, path_table_size_be;
    uint32_t path_table_lba_le;
    uint32_t opt_path_table_lba_le;
    uint32_t path_table_lba_be;
    uint32_t opt_path_table_lba_be;
    ISO9660_DirRecord root_dir;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct ELF32_Header {
    uint8_t  magic[4];
    uint8_t  cls, data, version;
    uint8_t  padding[9];
    uint16_t type, machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct ELF32_ProgramHeader {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
#pragma pack(pop)

PS2_LoadResult ISO_Loader::load(const char* iso_path, uint8_t* ee_ram, size_t ee_ram_size) {
    PS2_LoadResult result = {};
    result.success = false;

    FILE* f = fopen(iso_path, "rb");
    if (!f) {
        LOGE("ERROR: No se puede abrir ISO: %s (errno=%d: %s)", iso_path, errno, strerror(errno));
        LOGE("  ¿El archivo existe? ¿Tienes permisos de lectura?");
        return result;
    }

    ISO9660_PVD pvd;
    if (fseek(f, PVD_SECTOR * SECTOR_SIZE, SEEK_SET) != 0 ||
        fread(&pvd, 1, sizeof(pvd), f) < sizeof(pvd)) {
        LOGE("ERROR: No se puede leer PVD (sector 16). ¿Archivo corrupto?");
        fclose(f); return result;
    }

    if (memcmp(pvd.id, "CD001", 5) != 0) {
        LOGE("ERROR: No es una ISO 9660 valida. ID='%.5s'", pvd.id);
        fclose(f); return result;
    }

    LOGI("ISO: %.32s", pvd.vol_id);

    uint32_t root_lba  = pvd.root_dir.lba_le;
    uint32_t root_size = pvd.root_dir.size_le;

    std::vector<uint8_t> dir_data(root_size);
    fseek(f, root_lba * SECTOR_SIZE, SEEK_SET);
    fread(dir_data.data(), 1, root_size, f);

    uint32_t cnf_lba = 0, cnf_size = 0;
    uint32_t exe_lba = 0, exe_size = 0;
    char exe_name[256] = {};

    size_t off = 0;
    while (off < root_size) {
        auto* rec = reinterpret_cast<ISO9660_DirRecord*>(dir_data.data() + off);
        if (rec->length == 0) { off = (off + SECTOR_SIZE) & ~(SECTOR_SIZE-1); continue; }
        std::string name(rec->name, rec->name_len);
        auto sc = name.find(';');
        if (sc != std::string::npos) name = name.substr(0, sc);
        if (name == "SYSTEM.CNF") { cnf_lba = rec->lba_le; cnf_size = rec->size_le; }
        off += rec->length;
    }

    if (!cnf_lba) { LOGE("SYSTEM.CNF no encontrado"); fclose(f); return result; }

    std::vector<char> cnf(cnf_size + 1, 0);
    fseek(f, cnf_lba * SECTOR_SIZE, SEEK_SET);
    fread(cnf.data(), 1, cnf_size, f);
    LOGI("SYSTEM.CNF:\n%s", cnf.data());

    const char* boot2 = strstr(cnf.data(), "BOOT2");
    if (!boot2) { LOGE("BOOT2 no encontrado"); fclose(f); return result; }
    const char* slash = strchr(boot2, '\\');
    if (!slash) slash = strchr(boot2, '/');
    if (!slash) { LOGE("Ruta invalida en BOOT2"); fclose(f); return result; }
    slash++;
    const char* end = slash;
    while (*end && *end != ';' && *end != '\r' && *end != '\n' && *end != ' ') end++;
    strncpy(exe_name, slash, end - slash);
    exe_name[end-slash] = 0;
    LOGI("Ejecutable: %s", exe_name);
    strncpy(result.game_id, exe_name, sizeof(result.game_id)-1);

    off = 0;
    while (off < root_size) {
        auto* rec = reinterpret_cast<ISO9660_DirRecord*>(dir_data.data() + off);
        if (rec->length == 0) { off = (off + SECTOR_SIZE) & ~(SECTOR_SIZE-1); continue; }
        std::string name(rec->name, rec->name_len);
        auto sc = name.find(';');
        if (sc != std::string::npos) name = name.substr(0, sc);
        if (name == exe_name) { exe_lba = rec->lba_le; exe_size = rec->size_le; break; }
        off += rec->length;
    }

    if (!exe_lba) { LOGE("EXE %s no encontrado", exe_name); fclose(f); return result; }

    std::vector<uint8_t> elf_data(exe_size);
    fseek(f, exe_lba * SECTOR_SIZE, SEEK_SET);
    fread(elf_data.data(), 1, exe_size, f);
    fclose(f);

    ELF32_Header* elf = reinterpret_cast<ELF32_Header*>(elf_data.data());
    if (elf->magic[0] != 0x7F || elf->magic[1] != 'E' || elf->magic[2] != 'L' || elf->magic[3] != 'F') {
        LOGE("No es un ELF valido"); return result;
    }
    if (elf->machine != 8) { LOGE("ELF no es MIPS (machine=%d)", elf->machine); return result; }

    LOGI("ELF entry: 0x%08X, %d program headers", elf->entry, elf->phnum);

    auto* phdrs = reinterpret_cast<ELF32_ProgramHeader*>(elf_data.data() + elf->phoff);
    for (int i = 0; i < elf->phnum; i++) {
        auto& ph = phdrs[i];
        if (ph.type != 1) continue;
        uint32_t dest = ph.vaddr & 0x1FFFFFFFu;
        if (dest + ph.memsz > ee_ram_size) {
            LOGE("Segmento %d fuera de RAM: 0x%08X + 0x%X", i, dest, ph.memsz);
            continue;
        }
        if (ph.filesz > 0) memcpy(ee_ram + dest, elf_data.data() + ph.offset, ph.filesz);
        if (ph.memsz > ph.filesz) memset(ee_ram + dest + ph.filesz, 0, ph.memsz - ph.filesz);
        LOGI("Segmento %d: 0x%08X -> RAM+0x%08X (%u bytes)", i, ph.vaddr, dest, ph.filesz);
    }

    result.entry_point = elf->entry;
    result.success = true;
    LOGI("Juego cargado. Entry: 0x%08X", result.entry_point);
    return result;
}
