#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>

constexpr size_t EE_RAM_SIZE = 32 * 1024 * 1024;
constexpr size_t BIOS_ROM_SIZE = 4 * 1024 * 1024;
constexpr size_t SCRATCHPAD_SIZE = 16 * 1024;

constexpr uint32_t SP = 29;
constexpr uint32_t RA = 31;
constexpr uint32_t GP = 28;

struct EE_State {
    uint64_t gpr_lo[32];
    uint64_t gpr_hi[32];
    uint64_t hi, lo;
    uint32_t pc;
    uint32_t cop0[32];
    float    fpu[32];
    uint32_t fcsr;
    bool halted;
    bool interrupt_pending;
    bool branch_delay;
    uint32_t branch_target;
};

class CodeCache;
class EE_Recompiler;

class EE_Core {
public:
    EE_Core();
    ~EE_Core();

    void run_cycles(int64_t cycles);
    void load_bios(const uint8_t* bios_data, size_t size);
    void load_game(uint32_t entry_point);
    void handle_interrupt();
    void raise_interrupt(int irq);
    
    uint32_t read32(uint32_t addr);
    void write32(uint32_t addr, uint32_t val);
    
    uint8_t* get_ram() { return ee_ram.get(); }
    uint8_t* get_bios() { return bios_rom.get(); }
    size_t ram_size() { return EE_RAM_SIZE; }
    
    EE_State state;

    void interpret_single_instruction();

private:
    std::unique_ptr<CodeCache> cache;
    std::unique_ptr<EE_Recompiler> recompiler;
    std::unique_ptr<uint8_t[]> ee_ram;
    std::unique_ptr<uint8_t[]> bios_rom;
    std::unique_ptr<uint8_t[]> scratchpad;
    bool halted = false;
};