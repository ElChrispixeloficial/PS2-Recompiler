#include "vu_core.h"
#include <android/log.h>
#include <cstring>
#include <cmath>
#include <algorithm>

#define TAG "VU_Core"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── Constantes de flags ──────────────────────────────────────────────────────
static constexpr uint32_t STATUS_Z = 1 << 0;  // Zero
static constexpr uint32_t STATUS_S = 1 << 1;  // Sign
static constexpr uint32_t STATUS_U = 1 << 2;  // Underflow
static constexpr uint32_t STATUS_O = 1 << 3;  // Overflow
static constexpr uint32_t STATUS_I = 1 << 4;  // Invalid
static constexpr uint32_t STATUS_D = 1 << 5;  // Divide by zero

// ─── Broadcast constants (para fs/ft) ─────────────────────────────────────────
// bc field:
//   0: x  (broadcast x component)
//   1: y  (broadcast y component)
//   2: z  (broadcast z component)
//   3: w  (broadcast w component)

// ─── Implementación VU_Core ───────────────────────────────────────────────────

VU_Core::VU_Core() {
    memset(vu0_micro, 0, sizeof(vu0_micro));
    memset(vu0_data, 0, sizeof(vu0_data));
    memset(vu1_micro, 0, sizeof(vu1_micro));
    memset(vu1_data, 0, sizeof(vu1_data));
    reset();
}

VU_Core::~VU_Core() {
    LOGI("VU_Core destroyed");
}

void VU_Core::reset() {
    vu0.reset();
    vu1.reset();
    LOGI("VU_Core reset complete");
}

// ─── Acceso a memorias ────────────────────────────────────────────────────────

uint8_t* VU_Core::get_micro_mem(int vu_idx) {
    return vu_idx == 0 ? vu0_micro : vu1_micro;
}

uint8_t* VU_Core::get_data_mem(int vu_idx) {
    return vu_idx == 0 ? vu0_data : vu1_data;
}

const uint8_t* VU_Core::get_data_mem(int vu_idx) const {
    return vu_idx == 0 ? vu0_data : vu1_data;
}

// ─── Upload ───────────────────────────────────────────────────────────────────

void VU_Core::upload_micro(int vu_idx, uint32_t dest, const uint8_t* src, uint32_t size) {
    uint8_t* mem = get_micro_mem(vu_idx);
    size_t max_size = (vu_idx == 0) ? VU0_MICRO_SIZE : VU1_MICRO_SIZE;
    
    if (dest + size > max_size) {
        LOGW("VU%d micro upload out of range (0x%X + %u > 0x%X)", 
             vu_idx, dest, size, (unsigned)max_size);
        size = max_size - dest;
    }
    
    memcpy(mem + dest, src, size);
}

void VU_Core::upload_data(int vu_idx, uint32_t dest, const uint8_t* src, uint32_t size) {
    uint8_t* mem = get_data_mem(vu_idx);
    size_t max_size = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    
    if (dest + size > max_size) {
        LOGW("VU%d data upload out of range (0x%X + %u > 0x%X)", 
             vu_idx, dest, size, (unsigned)max_size);
        size = max_size - dest;
    }
    
    memcpy(mem + dest, src, size);
}

// ─── Control de ejecución ─────────────────────────────────────────────────────

void VU_Core::start(int vu_idx, uint32_t pc) {
    VU_State& vu = (vu_idx == 0) ? vu0 : vu1;
    vu.pc = pc;
    vu.running = true;
    vu.branch_pending = false;
    LOGI("VU%d started at PC=0x%04X", vu_idx, pc);
}

void VU_Core::stop(int vu_idx) {
    VU_State& vu = (vu_idx == 0) ? vu0 : vu1;
    vu.running = false;
}

bool VU_Core::is_running(int vu_idx) const {
    const VU_State& vu = (vu_idx == 0) ? vu0 : vu1;
    return vu.running;
}

// ─── Acceso a datos ───────────────────────────────────────────────────────────

uint32_t VU_Core::read_data(int vu_idx, uint32_t addr) const {
    const uint8_t* mem = get_data_mem(vu_idx);
    size_t max_size = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    addr &= (max_size - 1);
    
    uint32_t val;
    memcpy(&val, mem + addr, 4);
    return val;
}

void VU_Core::write_data(int vu_idx, uint32_t addr, uint32_t val) {
    uint8_t* mem = get_data_mem(vu_idx);
    size_t max_size = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    addr &= (max_size - 1);
    
    memcpy(mem + addr, &val, 4);
}

// ─── Decodificación de instrucción ────────────────────────────────────────────

VU_Instr VU_Core::decode(uint32_t upper, uint32_t lower) const {
    VU_Instr instr;
    
    // Upper word decoding
    instr.opcode     = (upper >> 26) & 0x3F;
    instr.ft         = (upper >> 16) & 0x1F;
    instr.fs         = (upper >> 11) & 0x1F;
    instr.fd         = (upper >>  6) & 0x1F;
    instr.dest_field = (upper >> 21) & 0xF;
    instr.bc         = (upper >>  0) & 0x3;
    instr.fmt        = (upper >>  2) & 0xF;
    instr.e_bit      = (upper >> 30) & 1;
    
    // Lower word decoding
    instr.lopcode    = (lower >> 25) & 0x7F;
    instr.it         = (lower >> 16) & 0x1FF;
    instr.is         = (lower >>  8) & 0xFF;
    instr.id         = (lower >>  0) & 0xFF;
    instr.imm11      = (lower >>  0) & 0x7FF;
    instr.imm15      = (lower >>  0) & 0x7FFF;
    
    return instr;
}

// ─── Funciones auxiliares ─────────────────────────────────────────────────────

void VU_Core::write_dest(VU_State& vu, const VU_Reg& result, int fd, uint8_t field) {
    // VF0 es read-only: siempre (0,0,0,1)
    if (fd == 0) return;
    
    // field bits: W Z Y X (bit 3=W, bit 2=Z, bit 1=Y, bit 0=X)
    // 0xF = xyzw, 0x8 = x, 0x4 = y, 0x2 = z, 0x1 = w
    if (field & 0x8) vu.vf[fd].x = result.x;
    if (field & 0x4) vu.vf[fd].y = result.y;
    if (field & 0x2) vu.vf[fd].z = result.z;
    if (field & 0x1) vu.vf[fd].w = result.w;
}

VU_Reg VU_Core::broadcast_source(const VU_State& vu, int fs, int ft, uint8_t bc) const {
    // fs siempre se lee como vector completo
    const VU_Reg& vs = vu.vf[fs];
    
    // ft se lee con broadcast según bc
    const VU_Reg& vt = vu.vf[ft];
    float broadcast_val;
    switch (bc) {
        case 0: broadcast_val = vt.x; break;
        case 1: broadcast_val = vt.y; break;
        case 2: broadcast_val = vt.z; break;
        case 3: broadcast_val = vt.w; break;
        default: broadcast_val = vt.x; break;
    }
    
    return VU_Reg::broadcast(broadcast_val);
}

void VU_Core::update_mac(VU_State& vu, const VU_Reg& result, uint8_t field) {
    // MAC flag: se actualiza por componentes según field mask
    // MAC = 0 si todos los componentes escritos son 0
    uint32_t mac = 0;
    if (field & 0x8 && result.x == 0.0f) mac |= 1;
    if (field & 0x4 && result.y == 0.0f) mac |= 2;
    if (field & 0x2 && result.z == 0.0f) mac |= 4;
    if (field & 0x1 && result.w == 0.0f) mac |= 8;
    vu.mac = mac;
}

void VU_Core::update_clip(VU_State& vu, const VU_Reg& result) {
    // Clip flag: compara componentes con ±w
    uint32_t clip = 0;
    
    // X > +W → bit 0, X < -W → bit 1
    if (result.x > +fabsf(result.w)) clip |= (1 << 0);
    if (result.x < -fabsf(result.w)) clip |= (1 << 1);
    
    // Y > +W → bit 2, Y < -W → bit 3
    if (result.y > +fabsf(result.w)) clip |= (1 << 2);
    if (result.y < -fabsf(result.w)) clip |= (1 << 3);
    
    // Z > +W → bit 4, Z < -W → bit 5
    if (result.z > +fabsf(result.w)) clip |= (1 << 4);
    if (result.z < -fabsf(result.w)) clip |= (1 << 5);
    
    vu.clip = clip;
}

void VU_Core::update_status(VU_State& vu, const VU_Reg& result) {
    vu.status &= ~(STATUS_Z | STATUS_S | STATUS_U | STATUS_O);
    
    // Zero check: todos los componentes son 0
    if (result.x == 0.0f && result.y == 0.0f && result.z == 0.0f && result.w == 0.0f) {
        vu.status |= STATUS_Z;
    }
    
    // Sign check: bit de signo del resultado
    if (result.x < 0.0f || result.y < 0.0f || result.z < 0.0f || result.w < 0.0f) {
        vu.status |= STATUS_S;
    }
}

// ─── Ejecución principal ──────────────────────────────────────────────────────

void VU_Core::step(int vu_idx) {
    VU_State& vu = (vu_idx == 0) ? vu0 : vu1;
    if (!vu.running) return;
    
    // Fetch instrucción de 64 bits (2 × 32-bit words)
    uint8_t* micro = get_micro_mem(vu_idx);
    size_t max_micro = (vu_idx == 0) ? VU0_MICRO_SIZE : VU1_MICRO_SIZE;
    
    if (vu.pc + 8 > max_micro) {
        vu.running = false;
        return;
    }
    
    uint32_t upper, lower;
    memcpy(&upper, micro + vu.pc, 4);
    memcpy(&lower, micro + vu.pc + 4, 4);
    
    VU_Instr instr = decode(upper, lower);
    
    // Avanzar PC (antes de ejecutar por los delay slots)
    uint32_t next_pc = vu.pc + 8;
    
    // Ejecutar upper y lower en paralelo (arquitectura VU real)
    execute_upper(vu, instr);
    execute_lower(vu, vu_idx, instr);
    
    // Actualizar PC
    if (vu.branch_pending) {
        vu.pc = vu.branch_target;
        vu.branch_pending = false;
    } else {
        vu.pc = next_pc;
    }
    
    // Verificar E-bit (end of microprogram)
    if (instr.e_bit) {
        vu.running = false;
    }
    
    vu.cycle_count += 2;  // Cada instrucción VU toma 2 ciclos (upper + lower)
}

void VU_Core::run_cycles(int vu_idx, uint32_t cycles) {
    VU_State& vu = (vu_idx == 0) ? vu0 : vu1;
    uint32_t target_cycle = vu.cycle_count + cycles;
    
    while (vu.running && vu.cycle_count < target_cycle) {
        step(vu_idx);
    }
}

void VU_Core::run_frame(int vu_idx) {
    VU_State& vu = (vu_idx == 0) ? vu0 : vu1;
    while (vu.running) {
        step(vu_idx);
    }
}

// ─── VU0 Modo Macro ───────────────────────────────────────────────────────────

void VU_Core::execute_macro_instr(uint32_t upper, uint32_t lower) {
    VU_Instr instr = decode(upper, lower);
    execute_upper(vu0, instr);
    // En modo macro, el lower word no se ejecuta igual que en micro
    // Se maneja a través de COP2 directamente
}

// ─── Ejecución Upper (FP ALU) ─────────────────────────────────────────────────

void VU_Core::execute_upper(VU_State& vu, const VU_Instr& instr) {
    switch (instr.opcode) {
        case 0x00: break; // NOP
        case 0x01: vu_add(vu, instr);     break; // ADDbc
        case 0x02: vu_addi(vu, instr);    break; // ADDi bc
        case 0x03: vu_sub(vu, instr);     break; // SUBbc
        case 0x04: vu_subi(vu, instr);    break; // SUBi bc
        case 0x05: vu_mul(vu, instr);     break; // MULbc
        case 0x06: vu_muli(vu, instr);    break; // MULi bc
        case 0x07: vu_madd(vu, instr);    break; // MADDbc
        case 0x08: vu_maddi(vu, instr);   break; // MADDi bc
        case 0x09: vu_msub(vu, instr);    break; // MSUBbc
        case 0x0A: vu_msubi(vu, instr);   break; // MSUBi bc
        case 0x0B: vu_max(vu, instr);     break; // MAXbc
        case 0x0C: vu_maxi(vu, instr);    break; // MAXi bc
        case 0x0D: vu_min(vu, instr);     break; // MINbc
        case 0x0E: vu_mini(vu, instr);    break; // MINi bc
        case 0x0F: break; // NOP
        
        case 0x10: vu_itof0(vu, instr);   break; // ITOF0
        case 0x11: vu_itof4(vu, instr);   break; // ITOF4
        case 0x12: vu_itof12(vu, instr);  break; // ITOF12
        case 0x13: vu_itof15(vu, instr);  break; // ITOF15
        case 0x14: vu_ftoi0(vu, instr);   break; // FTOI0
        case 0x15: vu_ftoi4(vu, instr);   break; // FTOI4
        case 0x16: vu_ftoi12(vu, instr);  break; // FTOI12
        case 0x17: vu_ftoi15(vu, instr);  break; // FTOI15
        
        case 0x1C: vu_fcset(vu, instr);   break; // FCSET
        case 0x1D: vu_fmand(vu, instr);   break; // FMAND
        case 0x1E: vu_fseq(vu, instr);    break; // FSEQ
        case 0x1F: vu_fsor(vu, instr);    break; // FSOR
        case 0x20: vu_fsset(vu, instr);   break; // FSSET
        case 0x21: vu_fsub(vu, instr);    break; // FSUB
        case 0x22: vu_fmadd(vu, instr);   break; // FMADD
        case 0x23: vu_abs(vu, instr);     break; // ABS
        case 0x24: break; // NOP
        case 0x25: break; // NOP
        case 0x26: vu_clip(vu, instr);    break; // CLIP
        
        case 0x28: // Extended opcodes (fmt field selects)
            switch (instr.fmt) {
                case 0x0: vu_add(vu, instr);     break; // ADD
                case 0x1: vu_addi(vu, instr);    break; // ADDi
                case 0x2: vu_sub(vu, instr);     break; // SUB
                case 0x3: vu_subi(vu, instr);    break; // SUBi
                case 0x4: vu_mul(vu, instr);     break; // MUL
                case 0x5: vu_muli(vu, instr);    break; // MULi
                case 0x6: vu_madd(vu, instr);    break; // MADD
                case 0x7: vu_maddi(vu, instr);   break; // MADDi
                case 0x8: vu_msub(vu, instr);    break; // MSUB
                case 0x9: vu_msubi(vu, instr);   break; // MSUBi
                case 0xA: vu_max(vu, instr);     break; // MAX
                case 0xB: vu_maxi(vu, instr);    break; // MAXi
                case 0xC: vu_min(vu, instr);     break; // MIN
                case 0xD: vu_mini(vu, instr);    break; // MINi
                case 0xE: vu_opmula(vu, instr);  break; // OPMULA
                case 0xF: vu_opmsub(vu, instr);  break; // OPMSUB
                default: break;
            }
            break;
        
        default: break; // NOP for unimplemented
    }
}

// ─── Ejecución Lower (Integer/Transfer) ───────────────────────────────────────

void VU_Core::execute_lower(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    switch (instr.lopcode) {
        case 0x00: vu_lq(vu, vu_idx, instr);     break; // LQ
        case 0x01: vu_sq(vu, vu_idx, instr);     break; // SQ
        case 0x04: vu_ilw(vu, vu_idx, instr);    break; // ILW
        case 0x05: vu_isw(vu, vu_idx, instr);    break; // ISW
        case 0x08: vu_iaddiu(vu, instr);         break; // IADDIU
        case 0x09: vu_isub(vu, instr);           break; // ISUB
        case 0x0A: vu_iadd(vu, instr);           break; // IADD
        case 0x0B: vu_iand(vu, instr);           break; // IAND
        case 0x0C: vu_ior(vu, instr);            break; // IOR
        case 0x10: vu_move(vu, instr);           break; // MOVE
        case 0x11: vu_mr32(vu, instr);           break; // MR32
        case 0x12: vu_mfir(vu, instr);           break; // MFIR
        case 0x13: vu_mtil(vu, instr);           break; // MTIL
        case 0x14: vu_ibeq(vu, instr);           break; // IBEQ
        case 0x15: vu_ibne(vu, instr);           break; // IBNE
        case 0x16: vu_ibltz(vu, instr);          break; // IBLTZ
        case 0x17: vu_ibgtz(vu, instr);          break; // IBGTZ
        case 0x18: vu_iblez(vu, instr);          break; // IBLEZ
        case 0x19: vu_ibgez(vu, instr);          break; // IBGEZ
        case 0x1A: vu_jr(vu, instr);             break; // JR
        case 0x1B: vu_jalr(vu, instr);           break; // JALR
        case 0x1C: break; // NOP
        case 0x1D: vu_xgkick(vu, instr);         break; // XGKICK
        case 0x1E: vu_xitop(vu, instr);          break; // XITOP
        case 0x1F: vu_xtop(vu, instr);           break; // XTOP
        default: break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// UPPER OPCODES - Operaciones de Punto Flotante
// ═══════════════════════════════════════════════════════════════════════════════

void VU_Core::vu_nop(VU_State& vu) {
    (void)vu; // No operation
}

void VU_Core::vu_add(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = vs.x + vt.x;
    result.y = vs.y + vt.y;
    result.z = vs.z + vt.z;
    result.w = vs.w + vt.w;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
    update_status(vu, result);
}

void VU_Core::vu_addi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = vs.x + vt.x;
    result.y = vs.y + vt.y;
    result.z = vs.z + vt.z;
    result.w = vs.w + vt.w;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
    update_status(vu, result);
}

void VU_Core::vu_sub(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = vs.x - vt.x;
    result.y = vs.y - vt.y;
    result.z = vs.z - vt.z;
    result.w = vs.w - vt.w;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
    update_status(vu, result);
}

void VU_Core::vu_subi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = vs.x - vt.x;
    result.y = vs.y - vt.y;
    result.z = vs.z - vt.z;
    result.w = vs.w - vt.w;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
    update_status(vu, result);
}

void VU_Core::vu_mul(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = vs.x * vt.x;
    result.y = vs.y * vt.y;
    result.z = vs.z * vt.z;
    result.w = vs.w * vt.w;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_muli(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = vs.x * vt.x;
    result.y = vs.y * vt.y;
    result.z = vs.z * vt.z;
    result.w = vs.w * vt.w;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_madd(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = vu.acc.x + vs.x * vt.x;
    result.y = vu.acc.y + vs.y * vt.y;
    result.z = vu.acc.z + vs.z * vt.z;
    result.w = vu.acc.w + vs.w * vt.w;
    
    // Actualizar acumulador
    vu.acc = result;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_maddi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = vu.acc.x + vs.x * vt.x;
    result.y = vu.acc.y + vs.y * vt.y;
    result.z = vu.acc.z + vs.z * vt.z;
    result.w = vu.acc.w + vs.w * vt.w;
    
    vu.acc = result;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_msub(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = vu.acc.x - vs.x * vt.x;
    result.y = vu.acc.y - vs.y * vt.y;
    result.z = vu.acc.z - vs.z * vt.z;
    result.w = vu.acc.w - vs.w * vt.w;
    
    vu.acc = result;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_msubi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = vu.acc.x - vs.x * vt.x;
    result.y = vu.acc.y - vs.y * vt.y;
    result.z = vu.acc.z - vs.z * vt.z;
    result.w = vu.acc.w - vs.w * vt.w;
    
    vu.acc = result;
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_max(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = std::max(vs.x, vt.x);
    result.y = std::max(vs.y, vt.y);
    result.z = std::max(vs.z, vt.z);
    result.w = std::max(vs.w, vt.w);
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_maxi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = std::max(vs.x, vt.x);
    result.y = std::max(vs.y, vt.y);
    result.z = std::max(vs.z, vt.z);
    result.w = std::max(vs.w, vt.w);
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_min(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    VU_Reg result;
    result.x = std::min(vs.x, vt.x);
    result.y = std::min(vs.y, vt.y);
    result.z = std::min(vs.z, vt.z);
    result.w = std::min(vs.w, vt.w);
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_mini(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    
    VU_Reg result;
    result.x = std::min(vs.x, vt.x);
    result.y = std::min(vs.y, vt.y);
    result.z = std::min(vs.z, vt.z);
    result.w = std::min(vs.w, vt.w);
    
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_opmula(VU_State& vu, const VU_Instr& instr) {
    // MUL to ACC only (no write to VF)
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    vu.acc.x = vs.x * vt.x;
    vu.acc.y = vs.y * vt.y;
    vu.acc.z = vs.z * vt.z;
    vu.acc.w = vs.w * vt.w;
}

void VU_Core::vu_opmsub(VU_State& vu, const VU_Instr& instr) {
    // MSUB: ACC = ACC - fs*ft (no write to VF)
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    vu.acc.x = vu.acc.x - vs.x * vt.x;
    vu.acc.y = vu.acc.y - vs.y * vt.y;
    vu.acc.z = vu.acc.z - vs.z * vt.z;
    vu.acc.w = vu.acc.w - vs.w * vt.w;
}

void VU_Core::vu_clip(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    vu.clip = 0;
    
    // PS2 CLIP: X > +W → bit 0, X < -W → bit 1
    //          Y > +W → bit 2, Y < -W → bit 3
    //          Z > +W → bit 4, Z < -W → bit 5
    float w = vt.w;
    if (vs.x >  w) vu.clip |= (1 << 0);
    if (vs.x < -w) vu.clip |= (1 << 1);
    if (vs.y >  w) vu.clip |= (1 << 2);
    if (vs.y < -w) vu.clip |= (1 << 3);
    if (vs.z >  w) vu.clip |= (1 << 4);
    if (vs.z < -w) vu.clip |= (1 << 5);
}

void VU_Core::vu_abs(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.x = fabsf(vs.x);
    result.y = fabsf(vs.y);
    result.z = fabsf(vs.z);
    result.w = fabsf(vs.w);
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_fcset(VU_State& vu, const VU_Instr& instr) {
    // FCSET: copia el campo imm15 al registro de clip
    vu.clip = instr.imm15 & 0x3F;  // 6 bits de clip flags
}

void VU_Core::vu_fmand(VU_State& vu, const VU_Instr& instr) {
    // FMAND: AND lógico entre VI y VF (como enteros)
    int32_t vi_val = vu.vi[instr.it];
    int32_t vf_val = vu.vf[instr.is].i(0);
    
    int32_t result = vi_val & vf_val;
    vu.vi[instr.id] = static_cast<int16_t>(result);
}

void VU_Core::vu_fseq(VU_State& vu, const VU_Instr& instr) {
    // FSEQ: compara vs == vt componente a componente
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    
    uint32_t mask = 0;
    if (vs.x == vt.x) mask |= 0x1;
    if (vs.y == vt.y) mask |= 0x2;
    if (vs.z == vt.z) mask |= 0x4;
    if (vs.w == vt.w) mask |= 0x8;
    
    vu.vi[instr.id] = static_cast<int16_t>(mask);
}

void VU_Core::vu_fsor(VU_State& vu, const VU_Instr& instr) {
    // FSOR: OR lógico entre VI y VF
    int32_t vi_val = vu.vi[instr.it];
    int32_t vf_val = vu.vf[instr.is].i(0);
    
    int32_t result = vi_val | vf_val;
    vu.vi[instr.id] = static_cast<int16_t>(result);
}

void VU_Core::vu_fsset(VU_State& vu, const VU_Instr& instr) {
    // FSSET: copia imm12 al registro de status
    vu.status = (vu.status & 0xFC0) | (instr.imm11 & 0x3F);
}

void VU_Core::vu_fsub(VU_State& vu, const VU_Instr& instr) {
    // FSUB: resta con actualización de status extendida
    vu_sub(vu, instr);
    // Status ya actualizado en vu_sub
}

void VU_Core::vu_fmadd(VU_State& vu, const VU_Instr& instr) {
    // FMADD: MADD con actualización de status extendida
    vu_madd(vu, instr);
}

// ─── Conversiones ITOF (Integer to Float) ─────────────────────────────────────

void VU_Core::vu_itof0(VU_State& vu, const VU_Instr& instr) {
    // ITOF0: convierte entero a float (0 bits fraccionales = entero puro)
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0));
    result.y = static_cast<float>(vs.i(1));
    result.z = static_cast<float>(vs.i(2));
    result.w = static_cast<float>(vs.i(3));
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_itof4(VU_State& vu, const VU_Instr& instr) {
    // ITOF4: entero con 4 bits fraccionales → float
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0)) / 16.0f;
    result.y = static_cast<float>(vs.i(1)) / 16.0f;
    result.z = static_cast<float>(vs.i(2)) / 16.0f;
    result.w = static_cast<float>(vs.i(3)) / 16.0f;
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_itof12(VU_State& vu, const VU_Instr& instr) {
    // ITOF12: entero con 12 bits fraccionales → float
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0)) / 4096.0f;
    result.y = static_cast<float>(vs.i(1)) / 4096.0f;
    result.z = static_cast<float>(vs.i(2)) / 4096.0f;
    result.w = static_cast<float>(vs.i(3)) / 4096.0f;
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_itof15(VU_State& vu, const VU_Instr& instr) {
    // ITOF15: entero con 15 bits fraccionales → float
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0)) / 32768.0f;
    result.y = static_cast<float>(vs.i(1)) / 32768.0f;
    result.z = static_cast<float>(vs.i(2)) / 32768.0f;
    result.w = static_cast<float>(vs.i(3)) / 32768.0f;
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

// ─── Conversiones FTOI (Float to Integer) ─────────────────────────────────────

void VU_Core::vu_ftoi0(VU_State& vu, const VU_Instr& instr) {
    // FTOI0: float → entero (0 bits fraccionales)
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x);
    result.i(1) = static_cast<int32_t>(vs.y);
    result.i(2) = static_cast<int32_t>(vs.z);
    result.i(3) = static_cast<int32_t>(vs.w);
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_ftoi4(VU_State& vu, const VU_Instr& instr) {
    // FTOI4: float → entero con 4 bits fraccionales
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x * 16.0f);
    result.i(1) = static_cast<int32_t>(vs.y * 16.0f);
    result.i(2) = static_cast<int32_t>(vs.z * 16.0f);
    result.i(3) = static_cast<int32_t>(vs.w * 16.0f);
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_ftoi12(VU_State& vu, const VU_Instr& instr) {
    // FTOI12: float → entero con 12 bits fraccionales
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x * 4096.0f);
    result.i(1) = static_cast<int32_t>(vs.y * 4096.0f);
    result.i(2) = static_cast<int32_t>(vs.z * 4096.0f);
    result.i(3) = static_cast<int32_t>(vs.w * 4096.0f);
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_ftoi15(VU_State& vu, const VU_Instr& instr) {
    // FTOI15: float → entero con 15 bits fraccionales
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x * 32768.0f);
    result.i(1) = static_cast<int32_t>(vs.y * 32768.0f);
    result.i(2) = static_cast<int32_t>(vs.z * 32768.0f);
    result.i(3) = static_cast<int32_t>(vs.w * 32768.0f);
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

// ═══════════════════════════════════════════════════════════════════════════════
// LOWER OPCODES - Operaciones Enteras y Transferencia
// ═══════════════════════════════════════════════════════════════════════════════

void VU_Core::vu_lq(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    // LQ: Load Quadword (128 bits = 4 floats) desde data memory
    uint8_t* data_mem = get_data_mem(vu_idx);
    size_t max_data = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    
    uint32_t addr = (instr.imm11 * 16) & (max_data - 1);
    
    VU_Reg loaded;
    memcpy(&loaded, data_mem + addr, 16);
    
    // Escribir con field mask (permite masked load)
    write_dest(vu, loaded, instr.ft, instr.dest_field);
}

void VU_Core::vu_sq(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    // SQ: Store Quadword (128 bits) to data memory
    uint8_t* data_mem = get_data_mem(vu_idx);
    size_t max_data = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    
    uint32_t addr = (instr.imm11 * 16) & (max_data - 1);
    
    const VU_Reg& vs = vu.vf[instr.fs];
    memcpy(data_mem + addr, &vs, 16);
}

void VU_Core::vu_ilw(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    // ILW: Load Integer Word (32 bits) from data memory → VI register
    uint8_t* data_mem = get_data_mem(vu_idx);
    size_t max_data = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    
    uint32_t addr = (instr.imm11 * 4) & (max_data - 1);
    
    int32_t val;
    memcpy(&val, data_mem + addr, 4);
    vu.vi[instr.it] = static_cast<int16_t>(val);
}

void VU_Core::vu_isw(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    // ISW: Store Integer Word (32 bits) from VI register → data memory
    uint8_t* data_mem = get_data_mem(vu_idx);
    size_t max_data = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    
    uint32_t addr = (instr.imm11 * 4) & (max_data - 1);
    
    int32_t val = static_cast<int32_t>(vu.vi[instr.is]);
    memcpy(data_mem + addr, &val, 4);
}

void VU_Core::vu_iadd(VU_State& vu, const VU_Instr& instr) {
    // IADD: Integer ADD (vi[id] = vi[is] + vi[it])
    int16_t a = vu.vi[instr.is];
    int16_t b = vu.vi[instr.it];
    vu.vi[instr.id] = a + b;
}

void VU_Core::vu_iaddiu(VU_State& vu, const VU_Instr& instr) {
    // IADDIU: Integer ADD Immediate Unsigned
    int16_t a = vu.vi[instr.is];
    vu.vi[instr.it] = a + static_cast<int16_t>(instr.imm15);
}

void VU_Core::vu_iand(VU_State& vu, const VU_Instr& instr) {
    // IAND: vi[id] = vi[is] & vi[it]
    vu.vi[instr.id] = vu.vi[instr.is] & vu.vi[instr.it];
}

void VU_Core::vu_ior(VU_State& vu, const VU_Instr& instr) {
    // IOR: vi[id] = vi[is] | vi[it]
    vu.vi[instr.id] = vu.vi[instr.is] | vu.vi[instr.it];
}

void VU_Core::vu_isub(VU_State& vu, const VU_Instr& instr) {
    // ISUB: vi[id] = vi[is] - vi[it]
    vu.vi[instr.id] = vu.vi[instr.is] - vu.vi[instr.it];
}

void VU_Core::vu_move(VU_State& vu, const VU_Instr& instr) {
    // MOVE: vf[ft] = vf[fs] (copia registro float completo)
    write_dest(vu, vu.vf[instr.fs], instr.ft, 0xF);  // xyzw mask = 0xF
}

void VU_Core::vu_mr32(VU_State& vu, const VU_Instr& instr) {
    // MR32: Move Rotate Right 32 bits (intercambia high/low words)
    const VU_Reg& vs = vu.vf[instr.fs];
    
    VU_Reg result;
    // Rota los componentes: x←y, y←z, z←w, w←x
    result.x = vs.y;
    result.y = vs.z;
    result.z = vs.w;
    result.w = vs.x;
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_mfir(VU_State& vu, const VU_Instr& instr) {
    // MFIR: Move From Integer Register → vf[ft].x (broadcast a x)
    int16_t val = vu.vi[instr.is];
    
    VU_Reg result;
    result.x = static_cast<float>(val);
    result.y = static_cast<float>(val);
    result.z = static_cast<float>(val);
    result.w = static_cast<float>(val);
    
    write_dest(vu, result, instr.ft, instr.dest_field);
}

void VU_Core::vu_mtil(VU_State& vu, const VU_Instr& instr) {
    // MTIL: Move To Integer Low (copia low 16 bits de vf[fs].x → vi[it])
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.vi[instr.it] = static_cast<int16_t>(vs.i(0) & 0xFFFF);
}

void VU_Core::vu_ibeq(VU_State& vu, const VU_Instr& instr) {
    // IBEQ: Branch if vi[is] == vi[it]
    if (vu.vi[instr.is] == vu.vi[instr.it]) {
        vu.branch_pending = true;
        vu.branch_target = instr.imm11 * 8;  // imm11 es offset en instrucciones
    }
}

void VU_Core::vu_ibne(VU_State& vu, const VU_Instr& instr) {
    // IBNE: Branch if vi[is] != vi[it]
    if (vu.vi[instr.is] != vu.vi[instr.it]) {
        vu.branch_pending = true;
        vu.branch_target = instr.imm11 * 8;
    }
}

void VU_Core::vu_ibltz(VU_State& vu, const VU_Instr& instr) {
    // IBLTZ: Branch if vi[is] < 0 (signed)
    if (vu.vi[instr.is] < 0) {
        vu.branch_pending = true;
        vu.branch_target = instr.imm11 * 8;
    }
}

void VU_Core::vu_ibgtz(VU_State& vu, const VU_Instr& instr) {
    // IBGTZ: Branch if vi[is] > 0 (signed)
    if (vu.vi[instr.is] > 0) {
        vu.branch_pending = true;
        vu.branch_target = instr.imm11 * 8;
    }
}

void VU_Core::vu_iblez(VU_State& vu, const VU_Instr& instr) {
    // IBLEZ: Branch if vi[is] <= 0 (signed)
    if (vu.vi[instr.is] <= 0) {
        vu.branch_pending = true;
        vu.branch_target = instr.imm11 * 8;
    }
}

void VU_Core::vu_ibgez(VU_State& vu, const VU_Instr& instr) {
    // IBGEZ: Branch if vi[is] >= 0 (signed)
    if (vu.vi[instr.is] >= 0) {
        vu.branch_pending = true;
        vu.branch_target = instr.imm11 * 8;
    }
}

void VU_Core::vu_jr(VU_State& vu, const VU_Instr& instr) {
    // JR: Jump Register (pc = vi[is] * 8)
    vu.branch_pending = true;
    vu.branch_target = (vu.vi[instr.is] & 0xFFFF) * 8;
}

void VU_Core::vu_jalr(VU_State& vu, const VU_Instr& instr) {
    // JALR: Jump And Link Register
    // Guarda return address en vi[it], salta a vi[is]
    vu.vi[instr.it] = static_cast<int16_t>((vu.pc + 16) / 8);  // PC después del delay slot
    
    vu.branch_pending = true;
    vu.branch_target = (vu.vi[instr.is] & 0xFFFF) * 8;
}

void VU_Core::vu_xgkick(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_xitop(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_xtop(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_div(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    if (vt.w != 0.0f) {
        vu.p = vs.w / vt.w;
    } else {
        vu.p = (vs.w < 0.0f) ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    }
    if (vt.w != 0.0f) {
        vu.q = 1.0f / vt.w;
    } else {
        vu.q = (vt.w < 0.0f) ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    }
}

void VU_Core::vu_sqrt(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vt = vu.vf[instr.ft];
    vu.q = sqrtf(fabsf(vt.w));
}

void VU_Core::vu_rsqrt(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    if (vt.w != 0.0f) {
        vu.q = vs.w / sqrtf(fabsf(vt.w));
    } else {
        vu.q = 0.0f;
    }
}

void VU_Core::vu_waitq(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_rnext(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_geti(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_next(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_ilwr(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    uint8_t* data_mem = get_data_mem(vu_idx);
    size_t max_data = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    uint32_t addr = (vu.vi[instr.is] * 16) & (max_data - 1);
    const VU_Reg& vs = vu.vf[instr.fs];
    memcpy(data_mem + addr, &vs, 16);
}

void VU_Core::vu_iswr(VU_State& vu, int vu_idx, const VU_Instr& instr) {
    uint8_t* data_mem = get_data_mem(vu_idx);
    size_t max_data = (vu_idx == 0) ? VU0_DATA_SIZE : VU1_DATA_SIZE;
    uint32_t addr = (vu.vi[instr.it] * 16) & (max_data - 1);
    const VU_Reg& vs = vu.vf[instr.fs];
    memcpy(data_mem + addr, &vs, 16);
}

void VU_Core::vu_loi(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fcget(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fceq(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fcor(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fmeq(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fmfir(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fmor(VU_State& vu, const VU_Instr& instr) {
    (void)vu;
    (void)instr;
}

void VU_Core::vu_fmove(VU_State& vu, const VU_Instr& instr) {
    vu_move(vu, instr);
}

void VU_Core::vu_fmul(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    VU_Reg result;
    result.x = vs.x * vt.x;
    result.y = vs.y * vt.y;
    result.z = vs.z * vt.z;
    result.w = vs.w * vt.w;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_fmulq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vs.x * vu.q;
    result.y = vs.y * vu.q;
    result.z = vs.z * vu.q;
    result.w = vs.w * vu.q;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_fmulai(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    float q = vu.q;
    VU_Reg result;
    result.x = vu.acc.x + vs.x * q;
    result.y = vu.acc.y + vs.y * q;
    result.z = vu.acc.z + vs.z * q;
    result.w = vu.acc.w + vs.w * q;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_faddq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vs.x + vu.q;
    result.y = vs.y + vu.q;
    result.z = vs.z + vu.q;
    result.w = vs.w + vu.q;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_fmaddq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x + vs.x * vu.q;
    result.y = vu.acc.y + vs.y * vu.q;
    result.z = vu.acc.z + vs.z * vu.q;
    result.w = vu.acc.w + vs.w * vu.q;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_faddi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vs.x + vt.x;
    result.y = vs.y + vt.y;
    result.z = vs.z + vt.z;
    result.w = vs.w + vt.w;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_faddai(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vt.x;
    result.y = vu.acc.y + vs.y + vt.y;
    result.z = vu.acc.z + vs.z + vt.z;
    result.w = vu.acc.w + vs.w + vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vaddq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vs.x + vu.p;
    result.y = vs.y + vu.p;
    result.z = vs.z + vu.p;
    result.w = vs.w + vu.p;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vaddai(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vt.x;
    result.y = vu.acc.y + vs.y + vt.y;
    result.z = vu.acc.z + vs.z + vt.z;
    result.w = vu.acc.w + vs.w + vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsubai(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vt.x;
    result.y = vu.acc.y - vs.y - vt.y;
    result.z = vu.acc.z - vs.z - vt.z;
    result.w = vu.acc.w - vs.w - vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vaddaq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vu.q;
    result.y = vu.acc.y + vs.y + vu.q;
    result.z = vu.acc.z + vs.z + vu.q;
    result.w = vu.acc.w + vs.w + vu.q;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsubaq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vu.q;
    result.y = vu.acc.y - vs.y - vu.q;
    result.z = vu.acc.z - vs.z - vu.q;
    result.w = vu.acc.w - vs.w - vu.q;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmaddaq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vu.q;
    result.y = vu.acc.y + vs.y + vu.q;
    result.z = vu.acc.z + vs.z + vu.q;
    result.w = vu.acc.w + vs.w + vu.q;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmsubaq(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vu.q;
    result.y = vu.acc.y - vs.y - vu.q;
    result.z = vu.acc.z - vs.z - vu.q;
    result.w = vu.acc.w - vs.w - vu.q;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vadda(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vt.x;
    result.y = vu.acc.y + vs.y + vt.y;
    result.z = vu.acc.z + vs.z + vt.z;
    result.w = vu.acc.w + vs.w + vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsuba(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vt.x;
    result.y = vu.acc.y - vs.y - vt.y;
    result.z = vu.acc.z - vs.z - vt.z;
    result.w = vu.acc.w - vs.w - vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmadda(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    VU_Reg result;
    result.x = vu.acc.x + vs.x * vt.x;
    result.y = vu.acc.y + vs.y * vt.y;
    result.z = vu.acc.z + vs.z * vt.z;
    result.w = vu.acc.w + vs.w * vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmsuba(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    const VU_Reg& vt = vu.vf[instr.ft];
    VU_Reg result;
    result.x = vu.acc.x - vs.x * vt.x;
    result.y = vu.acc.y - vs.y * vt.y;
    result.z = vu.acc.z - vs.z * vt.z;
    result.w = vu.acc.w - vs.w * vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vitof0(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0));
    result.y = static_cast<float>(vs.i(1));
    result.z = static_cast<float>(vs.i(2));
    result.w = static_cast<float>(vs.i(3));
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vitof4(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0)) / 16.0f;
    result.y = static_cast<float>(vs.i(1)) / 16.0f;
    result.z = static_cast<float>(vs.i(2)) / 16.0f;
    result.w = static_cast<float>(vs.i(3)) / 16.0f;
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vitof12(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0)) / 4096.0f;
    result.y = static_cast<float>(vs.i(1)) / 4096.0f;
    result.z = static_cast<float>(vs.i(2)) / 4096.0f;
    result.w = static_cast<float>(vs.i(3)) / 4096.0f;
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vitof15(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = static_cast<float>(vs.i(0)) / 32768.0f;
    result.y = static_cast<float>(vs.i(1)) / 32768.0f;
    result.z = static_cast<float>(vs.i(2)) / 32768.0f;
    result.w = static_cast<float>(vs.i(3)) / 32768.0f;
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vftoi0(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x);
    result.i(1) = static_cast<int32_t>(vs.y);
    result.i(2) = static_cast<int32_t>(vs.z);
    result.i(3) = static_cast<int32_t>(vs.w);
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vftoi4(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x * 16.0f);
    result.i(1) = static_cast<int32_t>(vs.y * 16.0f);
    result.i(2) = static_cast<int32_t>(vs.z * 16.0f);
    result.i(3) = static_cast<int32_t>(vs.w * 16.0f);
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vftoi12(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x * 4096.0f);
    result.i(1) = static_cast<int32_t>(vs.y * 4096.0f);
    result.i(2) = static_cast<int32_t>(vs.z * 4096.0f);
    result.i(3) = static_cast<int32_t>(vs.w * 4096.0f);
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vftoi15(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.i(0) = static_cast<int32_t>(vs.x * 32768.0f);
    result.i(1) = static_cast<int32_t>(vs.y * 32768.0f);
    result.i(2) = static_cast<int32_t>(vs.z * 32768.0f);
    result.i(3) = static_cast<int32_t>(vs.w * 32768.0f);
    write_dest(vu, result, instr.fd, instr.dest_field);
}

void VU_Core::vu_vadda_bc(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vt.x;
    result.y = vu.acc.y + vs.y + vt.y;
    result.z = vu.acc.z + vs.z + vt.z;
    result.w = vu.acc.w + vs.w + vt.w;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsuba_bc(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vt.x;
    result.y = vu.acc.y - vs.y - vt.y;
    result.z = vu.acc.z - vs.z - vt.z;
    result.w = vu.acc.w - vs.w - vt.w;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmadda_bc(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x + vs.x * vt.x;
    result.y = vu.acc.y + vs.y * vt.y;
    result.z = vu.acc.z + vs.z * vt.z;
    result.w = vu.acc.w + vs.w * vt.w;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmsuba_bc(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x - vs.x * vt.x;
    result.y = vu.acc.y - vs.y * vt.y;
    result.z = vu.acc.z - vs.z * vt.z;
    result.w = vu.acc.w - vs.w * vt.w;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vadda_i(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x + vs.x;
    result.y = vu.acc.y + vs.y;
    result.z = vu.acc.z + vs.z;
    result.w = vu.acc.w + vs.w;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsuba_i(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x - vs.x;
    result.y = vu.acc.y - vs.y;
    result.z = vu.acc.z - vs.z;
    result.w = vu.acc.w - vs.w;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vadda_q(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vu.q;
    result.y = vu.acc.y + vs.y + vu.q;
    result.z = vu.acc.z + vs.z + vu.q;
    result.w = vu.acc.w + vs.w + vu.q;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsuba_q(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vu.q;
    result.y = vu.acc.y - vs.y - vu.q;
    result.z = vu.acc.z - vs.z - vu.q;
    result.w = vu.acc.w - vs.w - vu.q;
    vu.acc = result;
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vaddax(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.x += vs.x;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vadday(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.y += vs.y;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vaddaz(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.z += vs.z;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vaddaw(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.w += vs.w;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vsubax(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.x -= vs.x;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vsubay(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.y -= vs.y;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vsubaz(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.z -= vs.z;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vsubaw(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    vu.acc.w -= vs.w;
    update_mac(vu, vu.acc, instr.dest_field);
}

void VU_Core::vu_vaddi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vs.x + vt.x;
    result.y = vs.y + vt.y;
    result.z = vs.z + vt.z;
    result.w = vs.w + vt.w;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vsubi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vs.x - vt.x;
    result.y = vs.y - vt.y;
    result.z = vs.z - vt.z;
    result.w = vs.w - vt.w;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmaddi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x + vs.x + vt.x;
    result.y = vu.acc.y + vs.y + vt.y;
    result.z = vu.acc.z + vs.z + vt.z;
    result.w = vu.acc.w + vs.w + vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmsubi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = vu.acc.x - vs.x - vt.x;
    result.y = vu.acc.y - vs.y - vt.y;
    result.z = vu.acc.z - vs.z - vt.z;
    result.w = vu.acc.w - vs.w - vt.w;
    vu.acc = result;
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmaxbc(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = std::max(vs.x, vt.x);
    result.y = std::max(vs.y, vt.y);
    result.z = std::max(vs.z, vt.z);
    result.w = std::max(vs.w, vt.w);
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmini(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = std::min(vs.x, vt.x);
    result.y = std::min(vs.y, vt.y);
    result.z = std::min(vs.z, vt.z);
    result.w = std::min(vs.w, vt.w);
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}

void VU_Core::vu_vmaxi(VU_State& vu, const VU_Instr& instr) {
    const VU_Reg& vs = vu.vf[instr.fs];
    VU_Reg vt = broadcast_source(vu, instr.fs, instr.ft, instr.bc);
    VU_Reg result;
    result.x = std::max(vs.x, vt.x);
    result.y = std::max(vs.y, vt.y);
    result.z = std::max(vs.z, vt.z);
    result.w = std::max(vs.w, vt.w);
    write_dest(vu, result, instr.fd, instr.dest_field);
    update_mac(vu, result, instr.dest_field);
}