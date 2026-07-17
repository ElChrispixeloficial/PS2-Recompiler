#pragma once
#include <cstdint>

class EE_Core;
class IOP_Core;

class PS2_BIOS {
public:
    static void init();
    static void set_ee_core(EE_Core* ee);
    static void set_iop_core(IOP_Core* iop);
    static void reset();
    static void execute();
    static void set_game_entry(uint32_t entry); // <--- AÑADIDO
};