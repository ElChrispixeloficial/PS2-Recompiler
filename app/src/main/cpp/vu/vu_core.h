#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// ─── Vector Units (VU0 / VU1) - Implementación completa PS2 ─────────────────
// Los VUs son procesadores SIMD de 128 bits (4×float32) que operan en
// paralelo sobre vectores 4D. Son el corazón del pipeline gráfico de PS2.
//
// VU0: 4KB micro-mem, 4KB data-mem, modo macro (COP2) + micro
// VU1: 16KB micro-mem, 16KB data-mem, modo micro solamente
//
// Cada instrucción VU tiene 64 bits divididos en:
//   Upper (32-bit): operaciones FP (ADD/SUB/MUL/MADD/MINA/MAXA/etc)
//   Lower (32-bit): operaciones enteras, load/store, branch, transfer

// ─── Estructuras de datos ─────────────────────────────────────────────────────

// Registro VU de 128 bits (4 × float32)
struct VU_Reg {
    float x, y, z, w;  // Componentes del vector (x,y,z,w) = (f[0],f[1],f[2],f[3])
    
    // Constructor por defecto: VF0 = (0,0,0,1)
    VU_Reg() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
    
    // Constructor con 4 floats
    VU_Reg(float X, float Y, float Z, float W) : x(X), y(Y), z(Z), w(W) {}
    
    // Broadcast: llena los 4 campos con el mismo valor
    static VU_Reg broadcast(float val) {
        return VU_Reg(val, val, val, val);
    }
    
    // Acceso indexado (para loops)
    float& operator[](int i) {
        return (&x)[i];
    }
    const float& operator[](int i) const {
        return (&x)[i];
    }
    
    // Acceso como enteros (para operaciones lógicas)
    int32_t& i(int idx) {
        return reinterpret_cast<int32_t*>(this)[idx];
    }
    const int32_t& i(int idx) const {
        return reinterpret_cast<const int32_t*>(this)[idx];
    }
    
    // Operadores SIMD elementales
    VU_Reg operator+(const VU_Reg& o) const {
        return VU_Reg(x + o.x, y + o.y, z + o.z, w + o.w);
    }
    VU_Reg operator-(const VU_Reg& o) const {
        return VU_Reg(x - o.x, y - o.y, z - o.z, w - o.w);
    }
    VU_Reg operator*(const VU_Reg& o) const {
        return VU_Reg(x * o.x, y * o.y, z * o.z, w * o.w);
    }
    VU_Reg operator/(const VU_Reg& o) const {
        return VU_Reg(x / o.x, y / o.y, z / o.z, w / o.w);
    }
    
    // Operaciones con broadcast (escalar × vector)
    VU_Reg operator*(float s) const {
        return VU_Reg(x * s, y * s, z * s, w * s);
    }
    VU_Reg operator+(float s) const {
        return VU_Reg(x + s, y + s, z + s, w + s);
    }
};
static_assert(sizeof(VU_Reg) == 16, "VU_Reg must be 128-bit");

// Estado completo de una VU
struct VU_State {
    // Registros de propósito general
    VU_Reg   vf[32];           // VF0-VF31: registros float 128-bit
                               // VF0 = (0,0,0,1) siempre (read-only)
    int16_t  vi[16];           // VI0-VI15: registros enteros 16-bit CON SIGNO
    
    // Acumulador y flags
    VU_Reg   acc;              // Acumulador de operaciones MADD/MSUB
    float    p, q;             // Acumuladores P, Q (para EFU - Elementary Function Unit)
    uint32_t mac;              // MAC flag (resultado de comparaciones)
    uint32_t clip;             // Clip flag (resultado de clip tests)
    uint32_t status;           // Status register:
                               //   bit 0: Z (zero)
                               //   bit 1: S (sign)
                               //   bit 2: U (underflow)
                               //   bit 3: O (overflow)
                               //   bit 4: I (invalid operation)
                               //   bit 5: D (divide by zero)
                               //   bit 19-20: D-bit, T-bit
                               //   bit 31: running
    
    // Control de ejecución
    uint32_t pc;               // Program counter (en micro-memoria)
    uint32_t cmsar0;           // CMSAR0: call/return stack address
    uint32_t cmsar1;           // CMSAR1: branch delay slot address
    uint32_t cycle_count;      // Contador de ciclos
    bool     running;          // Flag de ejecución
    bool     branch_pending;   // Branch en delay slot pendiente
    uint32_t branch_target;    // Dirección del branch pendiente
    
    void reset() {
        memset(vf, 0, sizeof(vf));
        memset(vi, 0, sizeof(vi));
        // VF0 es siempre (0,0,0,1) - se fuerza en cada escritura
        vf[0] = VU_Reg(0.0f, 0.0f, 0.0f, 1.0f);
        acc = VU_Reg::broadcast(0.0f);
        p = q = 0.0f;
        mac = clip = status = 0;
        pc = cmsar0 = cmsar1 = 0;
        cycle_count = 0;
        running = false;
        branch_pending = false;
        branch_target = 0;
    }
};

// ─── Pipeline state (para recompilación) ─────────────────────────────────────
enum class VU_Mode { IDLE, RUNNING, STALLED, BRANCH_DELAY };

// ─── Estructura de instrucción decodificada ──────────────────────────────────
struct VU_Instr {
    // Upper word (FP operations)
    uint8_t  opcode;      // bits 26-31 (6 bits)
    int      ft;          // bits 16-20
    int      fs;          // bits 11-15
    int      fd;          // bits 6-10
    uint8_t  dest_field;  // bits 21-24 (WZYX write mask)
    uint8_t  bc;          // bits 0-1 (broadcast control)
    uint8_t  fmt;         // bits 2-5 (format/opcode extension)
    
    // Lower word (Integer/Transfer operations)
    uint8_t  lopcode;     // bits 25-31
    int      it;          // bits 16-24
    int      is;          // bits 8-15
    int      id;          // bits 0-7
    uint16_t imm11;       // bits 0-10 (immediate)
    uint16_t imm15;       // bits 0-14 (immediate extendido)
    
    // Flag E-bit (end of microprogram)
    bool     e_bit;       // bit 30 of upper
};

// ─── Clase principal VU_Core ─────────────────────────────────────────────────
class VU_Core {
public:
    VU_Core();
    ~VU_Core();

    // No copiable (demasiado estado)
    VU_Core(const VU_Core&) = delete;
    VU_Core& operator=(const VU_Core&) = delete;
    
    // Movible (para contenedores)
    VU_Core(VU_Core&&) = default;
    VU_Core& operator=(VU_Core&&) = default;

    // Inicialización
    void reset();
    
    // Upload de microcódigo a memoria VU
    void upload_micro(int vu_idx, uint32_t dest, const uint8_t* src, uint32_t size);
    void upload_data(int vu_idx, uint32_t dest, const uint8_t* src, uint32_t size);
    
    // Control de ejecución
    void start(int vu_idx, uint32_t pc);
    void stop(int vu_idx);
    bool is_running(int vu_idx) const;
    
    // Ejecución
    void step(int vu_idx);                    // 1 instrucción (2 ciclos)
    void run_cycles(int vu_idx, uint32_t cycles);
    void run_frame(int vu_idx);               // Ejecuta hasta E-bit
    
    // VU0 modo macro (accesible como COP2 desde EE)
    void execute_macro_instr(uint32_t upper, uint32_t lower);
    
    // Acceso a memoria de datos (para transferencias DMA/COP2)
    uint32_t read_data(int vu_idx, uint32_t addr) const;
    void     write_data(int vu_idx, uint32_t addr, uint32_t val);
    
    // Estados públicos (para el recompiler JIT)
    VU_State vu0, vu1;
    
    // Constantes
    static constexpr size_t VU0_MICRO_SIZE = 4 * 1024;
    static constexpr size_t VU0_DATA_SIZE  = 4 * 1024;
    static constexpr size_t VU1_MICRO_SIZE = 16 * 1024;
    static constexpr size_t VU1_DATA_SIZE  = 16 * 1024;

    // Memoria auxiliar para load/store (PÚBLICAS para el JIT)
    uint8_t* get_data_mem(int vu_idx);
    const uint8_t* get_data_mem(int vu_idx) const;
    uint8_t* get_micro_mem(int vu_idx);

private:
    // ─── Micro-memoria y data-memoria ─────────────────────────────────────────
    alignas(16) uint8_t vu0_micro[VU0_MICRO_SIZE];
    alignas(16) uint8_t vu0_data[VU0_DATA_SIZE];
    alignas(16) uint8_t vu1_micro[VU1_MICRO_SIZE];
    alignas(16) uint8_t vu1_data[VU1_DATA_SIZE];
    
    // ─── Decodificación ──────────────────────────────────────────────────────
    VU_Instr decode(uint32_t upper, uint32_t lower) const;
    
    // ─── Upper opcodes (FP ALU) ──────────────────────────────────────────────
    void vu_nop(VU_State& vu);
    void vu_add(VU_State& vu, const VU_Instr& instr);
    void vu_addi(VU_State& vu, const VU_Instr& instr);
    void vu_sub(VU_State& vu, const VU_Instr& instr);
    void vu_subi(VU_State& vu, const VU_Instr& instr);
    void vu_mul(VU_State& vu, const VU_Instr& instr);
    void vu_muli(VU_State& vu, const VU_Instr& instr);
    void vu_madd(VU_State& vu, const VU_Instr& instr);
    void vu_maddi(VU_State& vu, const VU_Instr& instr);
    void vu_msub(VU_State& vu, const VU_Instr& instr);
    void vu_msubi(VU_State& vu, const VU_Instr& instr);
    void vu_max(VU_State& vu, const VU_Instr& instr);
    void vu_maxi(VU_State& vu, const VU_Instr& instr);
    void vu_min(VU_State& vu, const VU_Instr& instr);
    void vu_mini(VU_State& vu, const VU_Instr& instr);
    void vu_opmula(VU_State& vu, const VU_Instr& instr);   // MUL to ACC only
    void vu_opmsub(VU_State& vu, const VU_Instr& instr);   // MSUB to ACC only
    void vu_clip(VU_State& vu, const VU_Instr& instr);     // Clip test
    void vu_abs(VU_State& vu, const VU_Instr& instr);
    void vu_fcset(VU_State& vu, const VU_Instr& instr);    // Flag control
    void vu_fmand(VU_State& vu, const VU_Instr& instr);    // Float AND
    void vu_fseq(VU_State& vu, const VU_Instr& instr);     // Float compare ==
    void vu_fsor(VU_State& vu, const VU_Instr& instr);     // Float OR
    void vu_fsset(VU_State& vu, const VU_Instr& instr);    // Float set status
    void vu_fsub(VU_State& vu, const VU_Instr& instr);     // Float SUB (status)
    void vu_fmadd(VU_State& vu, const VU_Instr& instr);    // Float MADD (status)
    void vu_itof0(VU_State& vu, const VU_Instr& instr);    // Int→Float (0-1)
    void vu_itof4(VU_State& vu, const VU_Instr& instr);    // Int→Float (4-bit)
    void vu_itof12(VU_State& vu, const VU_Instr& instr);   // Int→Float (12-bit)
    void vu_itof15(VU_State& vu, const VU_Instr& instr);   // Int→Float (15-bit)
    void vu_ftoi0(VU_State& vu, const VU_Instr& instr);    // Float→Int (0-bit)
    void vu_ftoi4(VU_State& vu, const VU_Instr& instr);    // Float→Int (4-bit)
    void vu_ftoi12(VU_State& vu, const VU_Instr& instr);   // Float→Int (12-bit)
    void vu_ftoi15(VU_State& vu, const VU_Instr& instr);   // Float→Int (15-bit)
    
    // ─── Lower opcodes (Integer ALU / Transfer) ──────────────────────────────
    void vu_lq(VU_State& vu, int vu_idx, const VU_Instr& instr);
    void vu_sq(VU_State& vu, int vu_idx, const VU_Instr& instr);
    void vu_ilw(VU_State& vu, int vu_idx, const VU_Instr& instr);
    void vu_isw(VU_State& vu, int vu_idx, const VU_Instr& instr);
    void vu_iadd(VU_State& vu, const VU_Instr& instr);
    void vu_iaddiu(VU_State& vu, const VU_Instr& instr);
    void vu_iand(VU_State& vu, const VU_Instr& instr);
    void vu_ior(VU_State& vu, const VU_Instr& instr);
    void vu_isub(VU_State& vu, const VU_Instr& instr);
    void vu_move(VU_State& vu, const VU_Instr& instr);
    void vu_mr32(VU_State& vu, const VU_Instr& instr);
    void vu_mfir(VU_State& vu, const VU_Instr& instr);
    void vu_mtil(VU_State& vu, const VU_Instr& instr);
    void vu_ibeq(VU_State& vu, const VU_Instr& instr);
    void vu_ibne(VU_State& vu, const VU_Instr& instr);
    void vu_ibltz(VU_State& vu, const VU_Instr& instr);
    void vu_ibgtz(VU_State& vu, const VU_Instr& instr);
    void vu_iblez(VU_State& vu, const VU_Instr& instr);
    void vu_ibgez(VU_State& vu, const VU_Instr& instr);
    void vu_jr(VU_State& vu, const VU_Instr& instr);
    void vu_jalr(VU_State& vu, const VU_Instr& instr);
    void vu_xgkick(VU_State& vu, const VU_Instr& instr);
    void vu_xitop(VU_State& vu, const VU_Instr& instr);
    void vu_xtop(VU_State& vu, const VU_Instr& instr);
    
    // ─── Funciones auxiliares ────────────────────────────────────────────────
    void write_dest(VU_State& vu, const VU_Reg& result, int fd, uint8_t field);
    VU_Reg broadcast_source(const VU_State& vu, int fs, int ft, uint8_t bc) const;
    void execute_upper(VU_State& vu, const VU_Instr& instr);
    void execute_lower(VU_State& vu, int vu_idx, const VU_Instr& instr);
    void handle_branch(VU_State& vu, int vu_idx, const VU_Instr& instr);
    
    // Flags y condiciones
    void update_mac(VU_State& vu, const VU_Reg& result, uint8_t field);
    void update_clip(VU_State& vu, const VU_Reg& result);
    void update_status(VU_State& vu, const VU_Reg& result);
    
    // Recompilación (to ARM64)
    friend class VU_Recompiler;
};