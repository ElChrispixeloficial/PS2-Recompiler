#include "ee_recompiler.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <android/log.h>

#define LOG_TAG "EE-Recompiler"

#define LOGI(...) __android_log_print(
    ANDROID_LOG_INFO,
    LOG_TAG,
    __VA_ARGS__)

#define LOGE(...) __android_log_print(
    ANDROID_LOG_ERROR,
    LOG_TAG,
    __VA_ARGS__)



// ─────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────


EE_Recompiler::EE_Recompiler(
    CodeCache& cache,
    EE_State& state,
    uint8_t* ram
)
    :
    m_cache(cache),
    m_state(state),
    m_ram(ram)
{

    memset(
        &m_emit_ctx,
        0,
        sizeof(m_emit_ctx)
    );


    m_runtime.state = &state;
    m_runtime.ram   = ram;
    m_runtime.jit   = this;


    LOGI(
        "EE Recompiler inicializado"
    );
}



// ─────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────


EE_Recompiler::~EE_Recompiler()
{

}



// ─────────────────────────────────────────────
// Reset del JIT
// ─────────────────────────────────────────────


void EE_Recompiler::reset()
{

    m_registers.reset();


    memset(
        &m_emit_ctx,
        0,
        sizeof(m_emit_ctx)
    );


    stats = Statistics();


    LOGI(
        "EE Recompiler reset"
    );

}



// ─────────────────────────────────────────────
// Compilación principal
// ─────────────────────────────────────────────


EE_Recompiler::CompiledBlock
EE_Recompiler::compile_block(
    uint32_t pc
)
{

    MIPSBlock block{};


    if (!decode_block(
            pc,
            block))
    {

        LOGE(
            "No se pudo decodificar bloque %08X",
            pc
        );

        return nullptr;
    }



    CompiledBlock fn =
        emit_block(block);



    if (!fn)
    {

        LOGE(
            "Falló generación ARM64 %08X",
            pc
        );

        return nullptr;

    }



    m_cache.register_block(
        pc,
        fn,
        0
    );



    stats.blocks_compiled++;


    stats.instructions_compiled +=
        block.instructions.size();



    LOGI(
        "Bloque compilado PC=%08X instrucciones=%zu",
        pc,
        block.instructions.size()
    );


    return fn;

}



// ─────────────────────────────────────────────
// Lectura de instrucción MIPS
// ─────────────────────────────────────────────


uint32_t EE_Recompiler::read_instruction(
    uint32_t address
)
{

    uint32_t phys =
        address & 0x01FFFFFF;



    uint32_t value;



    memcpy(
        &value,
        m_ram + phys,
        sizeof(uint32_t)
    );



    return value;

}





// ─────────────────────────────────────────────
// Decodificación de bloque MIPS
//
// Lee instrucciones del R5900 hasta encontrar:
// - salto
// - branch
// - syscall
// - instrucción de salida
//
// El delay slot del MIPS se conserva.
// ─────────────────────────────────────────────


bool EE_Recompiler::decode_block(
    uint32_t pc,
    MIPSBlock& block
)
{

    block.start_pc = pc;
    block.end_pc   = pc;
    block.ends_branch = false;
    block.has_delay_slot = false;


    constexpr uint32_t MAX_BLOCK_INSTRUCTIONS = 64;


    uint32_t current_pc = pc;



    for (
        uint32_t i = 0;
        i < MAX_BLOCK_INSTRUCTIONS;
        i++
    )
    {

        uint32_t opcode =
            read_instruction(current_pc);



        MIPSInstruction inst{};


        inst.pc =
            current_pc;


        inst.opcode =
            opcode;


        inst.delay_slot =
            0;



        block.instructions.push_back(
            inst
        );



        block.end_pc =
            current_pc + 4;



        if (is_block_end(opcode))
        {

            block.ends_branch = true;


            block.has_delay_slot = true;



            uint32_t delay_opcode =
                read_instruction(
                    current_pc + 4
                );



            MIPSInstruction delay{};


            delay.pc =
                current_pc + 4;


            delay.opcode =
                delay_opcode;



            delay.delay_slot =
                1;



            block.instructions.push_back(
                delay
            );



            block.end_pc =
                current_pc + 8;



            break;

        }



        current_pc += 4;

    }



    return !block.instructions.empty();

}



// ─────────────────────────────────────────────
// Detectar fin del bloque
// ─────────────────────────────────────────────


bool EE_Recompiler::is_block_end(
    uint32_t opcode
)
{

    uint32_t primary =
        opcode >> 26;



    switch(primary)
    {

        // J
        case 0x02:

        // JAL
        case 0x03:

        // BEQ
        case 0x04:

        // BNE
        case 0x05:

        // BLEZ
        case 0x06:

        // BGTZ
        case 0x07:

            return true;



        case 0x00:
        {

            uint32_t funct =
                opcode & 0x3F;


            switch(funct)
            {

                // JR
                case 0x08:

                // JALR
                case 0x09:

                    return true;


                default:
                    break;

            }

            break;

        }


        default:
            break;

    }



    return false;

}



// ─────────────────────────────────────────────
// Delay slot
// ─────────────────────────────────────────────


bool EE_Recompiler::has_delay_slot(
    uint32_t opcode
)
{

    return is_block_end(opcode);

}





// ─────────────────────────────────────────────
// Generación de bloque ARM64
//
// Convierte el bloque MIPS decodificado en
// instrucciones ARM64 nativas dentro del CodeCache.
// No usa intérprete.
// ─────────────────────────────────────────────


EE_Recompiler::CompiledBlock
EE_Recompiler::emit_block(
    const MIPSBlock& block
)
{

    size_t estimated_size =
        block.instructions.size() * 32;


    uint8_t* code =
        static_cast<uint8_t*>(
            m_cache.alloc(
                estimated_size
            )
        );



    if (!code)
    {

        LOGE(
            "Sin espacio en CodeCache"
        );

        return nullptr;

    }



    ARM64Emitter emitter(
        code,
        estimated_size
    );



    m_emit_ctx.current_pc =
        block.start_pc;


    m_emit_ctx.emitter =
        &emitter;



    for (
        const auto& inst :
        block.instructions
    )
    {

        if (!emit_instruction(
                emitter,
                inst))
        {

            LOGE(
                "No se pudo emitir instrucción MIPS %08X",
                inst.opcode
            );


            return nullptr;

        }

    }



    /*
       Cada bloque termina regresando
       al dispatcher principal.
       
       ARM64:
       ret
    */

    emitter.ret();



    void* fn =
        reinterpret_cast<void*>(code);



    __builtin___clear_cache(
        reinterpret_cast<char*>(code),
        reinterpret_cast<char*>(code + emitter.size())
    );



    return reinterpret_cast<CompiledBlock>(
        fn
    );

}



// ─────────────────────────────────────────────
// Emisión de una instrucción MIPS
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_instruction(
    ARM64Emitter& emitter,
    const MIPSInstruction& inst
)
{

    uint32_t opcode =
        inst.opcode;



    uint32_t primary =
        opcode >> 26;



    switch(primary)
    {


        // SPECIAL
        case 0x00:
        {

            return emit_special(
                emitter,
                opcode
            );

        }



        // ADDI
        case 0x08:
        {

            uint32_t rs =
                (opcode >> 21) & 31;


            uint32_t rt =
                (opcode >> 16) & 31;


            int16_t imm =
                opcode & 0xFFFF;



            return emit_addi(
                emitter,
                rs,
                rt,
                imm
            );

        }



        // ADDIU
        case 0x09:
        {

            uint32_t rs =
                (opcode >> 21) & 31;


            uint32_t rt =
                (opcode >> 16) & 31;


            int16_t imm =
                opcode & 0xFFFF;



            return emit_addiu(
                emitter,
                rs,
                rt,
                imm
            );

        }



        // LW
        case 0x23:
        {

            uint32_t base =
                (opcode >> 21) & 31;


            uint32_t rt =
                (opcode >> 16) & 31;


            int16_t off =
                opcode & 0xFFFF;



            return emit_lw(
                emitter,
                base,
                rt,
                off
            );

        }



        // SW
        case 0x2B:
        {

            uint32_t base =
                (opcode >> 21) & 31;


            uint32_t rt =
                (opcode >> 16) & 31;


            int16_t off =
                opcode & 0xFFFF;



            return emit_sw(
                emitter,
                base,
                rt,
                off
            );

        }



        // J
        case 0x02:
        {

            uint32_t target =
                opcode & 0x03FFFFFF;


            return emit_jump(
                emitter,
                target
            );

        }



        // JAL
        case 0x03:
        {

            uint32_t target =
                opcode & 0x03FFFFFF;


            return emit_jal(
                emitter,
                target
            );

        }



        // BEQ
        case 0x04:
        {

            uint32_t rs =
                (opcode >> 21)&31;


            uint32_t rt =
                (opcode >> 16)&31;


            int16_t off =
                opcode & 0xFFFF;



            return emit_beq(
                emitter,
                rs,
                rt,
                off
            );

        }



        // BNE
        case 0x05:
        {

            uint32_t rs =
                (opcode >> 21)&31;


            uint32_t rt =
                (opcode >> 16)&31;


            int16_t off =
                opcode & 0xFFFF;



            return emit_bne(
                emitter,
                rs,
                rt,
                off
            );

        }



        default:

            // Instrucción no implementada:
            // generar NOP ARM64 para mantener
            // sincronización del bloque.

            emitter.nop();

            return true;

    }


}



// ─────────────────────────────────────────────
// NOP MIPS → NOP ARM64
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_nop(
    ARM64Emitter& emitter
)
{

    emitter.nop();

    return true;

}




// ─────────────────────────────────────────────
// ADDI
// rt = rs + inmediato
// MIPS → ARM64
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_addi(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{
    if (rt == 0)
        return true;

    emitter.load_state_reg(rt, rt);
    emitter.load_state_reg(rs, rs);

    emitter.add_imm(
        rt,
        rs,
        static_cast<uint32_t>(imm)
    );

    emitter.store_state_reg(rt, rt);

    return true;
}



// ─────────────────────────────────────────────
// ADDIU
// rt = rs + inmediato unsigned
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_addiu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.load_state_reg(rt, rt);

    emitter.load_state_reg(rs, rs);


    emitter.add_imm(
        rt,
        rs,
        static_cast<uint32_t>(
            static_cast<int32_t>(imm)
        )
    );


    emitter.store_state_reg(rt, rt);


    return true;
}



// ─────────────────────────────────────────────
// LW
// rt = RAM[rs + offset]
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_lw(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load_state_reg(
        0,
        base
    );


    emitter.add_imm(
        0,
        0,
        offset
    );


    emitter.load_ram32(
        rt,
        0
    );


    emitter.store_state_reg(
        rt,
        rt
    );


    return true;
}



// ─────────────────────────────────────────────
// SW
// RAM[rs + offset] = rt
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_sw(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state_reg(
        0,
        base
    );


    emitter.add_imm(
        0,
        0,
        offset
    );


    emitter.load_state_reg(
        1,
        rt
    );


    emitter.store_ram32(
        1,
        0
    );


    return true;

}



// ─────────────────────────────────────────────
// J
// Salto absoluto MIPS
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_jump(
    ARM64Emitter& emitter,
    uint32_t target
)
{

    uint32_t address =
        (m_emit_ctx.current_pc & 0xF0000000)
        |
        (target << 2);


    emitter.set_pc(
        address
    );


    emitter.ret();


    return true;

}



// ─────────────────────────────────────────────
// JAL
// ra = PC+8
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_jal(
    ARM64Emitter& emitter,
    uint32_t target
)
{

    emitter.set_link(
        m_emit_ctx.current_pc + 8
    );


    uint32_t address =
        (m_emit_ctx.current_pc & 0xF0000000)
        |
        (target << 2);


    emitter.set_pc(
        address
    );


    emitter.ret();


    return true;

}



// ─────────────────────────────────────────────
// BEQ
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_beq(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state_reg(
        0,
        rs
    );


    emitter.load_state_reg(
        1,
        rt
    );


    uint32_t target =
        m_emit_ctx.current_pc +
        4 +
        (offset << 2);



    emitter.branch_equal(
        target
    );


    return true;

}



// ─────────────────────────────────────────────
// BNE
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_bne(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state_reg(
        0,
        rs
    );


    emitter.load_state_reg(
        1,
        rt
    );


    uint32_t target =
        m_emit_ctx.current_pc +
        4 +
        (offset << 2);



    emitter.branch_not_equal(
        target
    );


    return true;

}


// ─────────────────────────────────────────────
// SPECIAL opcode handler
// Funct field MIPS → ARM64
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_special(
    ARM64Emitter& emitter,
    uint32_t opcode
)
{

    uint32_t rs =
        (opcode >> 21) & 31;

    uint32_t rt =
        (opcode >> 16) & 31;

    uint32_t rd =
        (opcode >> 11) & 31;

    uint32_t sa =
        (opcode >> 6) & 31;

    uint32_t funct =
        opcode & 0x3F;



    switch(funct)
    {


        // SLL rd, rt, sa
        case 0x00:
        {

            if (rd == 0)
                return true;


            emitter.load_state_reg(
                rt,
                rt
            );


            emitter.shift_left_imm(
                rd,
                rt,
                sa
            );


            emitter.store_state_reg(
                rd,
                rd
            );


            return true;
        }



        // SRL
        case 0x02:
        {

            if (rd == 0)
                return true;


            emitter.load_state_reg(
                rt,
                rt
            );


            emitter.shift_right_imm(
                rd,
                rt,
                sa
            );


            emitter.store_state_reg(
                rd,
                rd
            );


            return true;

        }



        // SRA
        case 0x03:
        {

            if (rd == 0)
                return true;


            emitter.load_state_reg(
                rt,
                rt
            );


            emitter.shift_arithmetic_right(
                rd,
                rt,
                sa
            );


            emitter.store_state_reg(
                rd,
                rd
            );


            return true;

        }



        // JR rs
        case 0x08:
        {

            emitter.load_state_reg(
                0,
                rs
            );


            emitter.set_pc_from_reg(
                0
            );


            emitter.ret();


            return true;

        }



        // JALR rd, rs
        case 0x09:
        {

            if(rd != 0)
            {
                emitter.set_link_reg(
                    rd,
                    m_emit_ctx.current_pc + 8
                );
            }


            emitter.load_state_reg(
                0,
                rs
            );


            emitter.set_pc_from_reg(
                0
            );


            emitter.ret();


            return true;

        }



        // SYSCALL
        case 0x0C:
        {

            emitter.call_exception(
                8
            );


            return true;

        }



        default:
            break;

    }



    // Parte 2 continúa con operaciones ALU

    emitter.nop();

    return true;

}


// ─────────────────────────────────────────────
// SPECIAL instructions
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_special(
    ARM64Emitter& emitter,
    uint32_t opcode
)
{
    uint32_t funct = opcode & 0x3F;

    uint32_t rs = (opcode >> 21) & 31;
    uint32_t rt = (opcode >> 16) & 31;
    uint32_t rd = (opcode >> 11) & 31;
    uint32_t sa = (opcode >> 6)  & 31;


    switch (funct)
    {

        // SLL
        case 0x00:
        {
            return emit_sll(
                emitter,
                rd,
                rt,
                sa
            );
        }


        // SRL
        case 0x02:
        {
            return emit_srl(
                emitter,
                rd,
                rt,
                sa
            );
        }


        // SRA
        case 0x03:
        {
            return emit_sra(
                emitter,
                rd,
                rt,
                sa
            );
        }


        // ADD
        case 0x20:
        {
            return emit_add(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // ADDU
        case 0x21:
        {
            return emit_addu(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // SUB
        case 0x22:
        {
            return emit_sub(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // SUBU
        case 0x23:
        {
            return emit_subu(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // AND
        case 0x24:
        {
            return emit_and(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // OR
        case 0x25:
        {
            return emit_or(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // XOR
        case 0x26:
        {
            return emit_xor(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // NOR
        case 0x27:
        {
            return emit_nor(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // SLT
        case 0x2A:
        {
            return emit_slt(
                emitter,
                rd,
                rs,
                rt
            );
        }


        // JR
        case 0x08:
        {
            return emit_jr(
                emitter,
                rs
            );
        }


        // JALR
        case 0x09:
        {
            return emit_jalr(
                emitter,
                rd,
                rs
            );
        }


        default:
        {
            LOGE(
                "SPECIAL no soportada funct=%02X",
                funct
            );

            emitter.nop();

            return true;
        }

    }

}



// ─────────────────────────────────────────────
// Registro cero MIPS ($zero)
// siempre devuelve 0
// ─────────────────────────────────────────────

void EE_Recompiler::fix_zero_register()
{
    m_state.gpr_lo[0] = 0;
}



// ─────────────────────────────────────────────
// Actualización de PC
// ─────────────────────────────────────────────

void EE_Recompiler::update_pc(
    uint32_t pc
)
{
    m_state.pc = pc;
}



// ─────────────────────────────────────────────
// Estadísticas
// ─────────────────────────────────────────────

const EE_Recompiler::Statistics&
EE_Recompiler::get_statistics() const
{
    return stats;
}


// ─────────────────────────────────────────────
// Operaciones aritméticas MIPS → ARM64
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_add(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.add_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_addu(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.add_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_sub(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.sub_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_subu(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.sub_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_and(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.and_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_or(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.or_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_xor(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.eor_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_nor(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.nor_reg(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_slt(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{
    if (rd == 0)
        return true;

    emitter.slt_reg(
        rd,
        rs,
        rt
    );

    return true;
}



// ─────────────────────────────────────────────
// Shift operations MIPS → ARM64
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_sll(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{
    if (rd == 0)
        return true;

    emitter.lsl_imm(
        rd,
        rt,
        sa
    );

    return true;
}



bool EE_Recompiler::emit_srl(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{
    if (rd == 0)
        return true;

    emitter.lsr_imm(
        rd,
        rt,
        sa
    );

    return true;
}



bool EE_Recompiler::emit_sra(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{
    if (rd == 0)
        return true;

    emitter.asr_imm(
        rd,
        rt,
        sa
    );

    return true;
}



// ─────────────────────────────────────────────
// Immediate arithmetic
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_addi(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.add_imm(
        rt,
        rs,
        static_cast<uint32_t>(imm)
    );


    return true;
}



bool EE_Recompiler::emit_addiu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.add_imm(
        rt,
        rs,
        static_cast<uint32_t>(
            static_cast<int32_t>(imm)
        )
    );


    return true;
}



// ─────────────────────────────────────────────
// Logical immediate
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_andi(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.and_imm(
        rt,
        rs,
        imm
    );


    return true;
}



bool EE_Recompiler::emit_ori(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.or_imm(
        rt,
        rs,
        imm
    );


    return true;
}



bool EE_Recompiler::emit_xori(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.eor_imm(
        rt,
        rs,
        imm
    );


    return true;
}



// ─────────────────────────────────────────────
// Memory operations MIPS R5900 → ARM64
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lw(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load32(
        rt,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_sw(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    emitter.store32(
        rt,
        base,
        offset
    );


    return true;
}



// ─────────────────────────────────────────────
// Byte loads/stores
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lb(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load8_signed(
        rt,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_lbu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load8_unsigned(
        rt,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_sb(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    emitter.store8(
        rt,
        base,
        offset
    );


    return true;
}



// ─────────────────────────────────────────────
// Halfword loads/stores
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lh(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load16_signed(
        rt,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_lhu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load16_unsigned(
        rt,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_sh(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    emitter.store16(
        rt,
        base,
        offset
    );


    return true;
}



// ─────────────────────────────────────────────
// Dirección efectiva MIPS
//
// ARM64:
// Xbase + offset = dirección física
// ─────────────────────────────────────────────


uint32_t EE_Recompiler::translate_address(
    uint32_t address
)
{
    return address & 0x1FFFFFFF;
}



// ─────────────────────────────────────────────
// Control flow MIPS R5900 → ARM64
// J / JAL / JR / JALR / Branches
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_jump(
    ARM64Emitter& emitter,
    uint32_t target
)
{
    uint32_t address =
        (m_emit_ctx.current_pc & 0xF0000000) |
        (target << 2);


    /*
       El salto MIPS no salta directamente al código ARM64.
       Actualiza PC del estado EE y vuelve al dispatcher.
    */

    emitter.mov_imm(
        ARM64Emitter::REG_TMP0,
        address
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.ret();

    return true;
}



bool EE_Recompiler::emit_jal(
    ARM64Emitter& emitter,
    uint32_t target
)
{
    uint32_t address =
        (m_emit_ctx.current_pc & 0xF0000000) |
        (target << 2);


    // RA = PC + 8 por delay slot

    emitter.mov_imm(
        ARM64Emitter::REG_TMP0,
        m_emit_ctx.current_pc + 8
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[31])
    );


    emitter.mov_imm(
        ARM64Emitter::REG_TMP0,
        address
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.ret();


    return true;
}



bool EE_Recompiler::emit_jr(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    /*
       PC = registro rs
    */


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.ret();


    return true;
}



bool EE_Recompiler::emit_jalr(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs
)
{

    if (rd != 0)
    {
        emitter.mov_imm(
            ARM64Emitter::REG_TMP0,
            m_emit_ctx.current_pc + 8
        );


        emitter.store32_state(
            ARM64Emitter::REG_TMP0,
            offsetof(EE_State, gpr_lo[rd])
        );
    }


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.ret();


    return true;
}



// ─────────────────────────────────────────────
// Branches
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_beq(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t offset
)
{

    uint32_t target =
        m_emit_ctx.current_pc +
        4 +
        (static_cast<int32_t>(offset) << 2);


    emitter.compare_state_regs(
        rs,
        rt
    );


    emitter.branch_equal(
        target
    );


    return true;
}



bool EE_Recompiler::emit_bne(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t offset
)
{

    uint32_t target =
        m_emit_ctx.current_pc +
        4 +
        (static_cast<int32_t>(offset) << 2);


    emitter.compare_state_regs(
        rs,
        rt
    );


    emitter.branch_not_equal(
        target
    );


    return true;
}



// ─────────────────────────────────────────────
// Actualización del PC al terminar bloque
// ─────────────────────────────────────────────

void EE_Recompiler::emit_block_exit(
    ARM64Emitter& emitter,
    uint32_t pc
)
{

    emitter.mov_imm(
        ARM64Emitter::REG_TMP0,
        pc
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.ret();

}



// ─────────────────────────────────────────────
// HI / LO operations - R5900
// MULT, DIV, MFHI, MFLO
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mult(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{
    emitter.mult_signed(
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_multu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{
    emitter.mult_unsigned(
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_div(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{
    emitter.div_signed(
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_divu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{
    emitter.div_unsigned(
        rs,
        rt
    );

    return true;
}



// HI → GPR

bool EE_Recompiler::emit_mfhi(
    ARM64Emitter& emitter,
    uint32_t rd
)
{
    if (rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    emitter.store_gpr32(
        rd,
        ARM64Emitter::REG_TMP0
    );


    return true;
}



// LO → GPR

bool EE_Recompiler::emit_mflo(
    ARM64Emitter& emitter,
    uint32_t rd
)
{
    if (rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    emitter.store_gpr32(
        rd,
        ARM64Emitter::REG_TMP0
    );


    return true;
}



// GPR → HI

bool EE_Recompiler::emit_mthi(
    ARM64Emitter& emitter,
    uint32_t rs
)
{
    emitter.load_gpr32(
        ARM64Emitter::REG_TMP0,
        rs
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    return true;
}



// GPR → LO

bool EE_Recompiler::emit_mtlo(
    ARM64Emitter& emitter,
    uint32_t rs
)
{
    emitter.load_gpr32(
        ARM64Emitter::REG_TMP0,
        rs
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    return true;
}



// ─────────────────────────────────────────────
// COP0 System Control - R5900
// MFC0 / MTC0 / ERET / SYSCALL / BREAK
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mfc0(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint32_t rd
)
{
    if (rt == 0)
        return true;


    // rt = COP0[rd]

    emitter.load_cop0(
        ARM64Emitter::REG_TMP0,
        rd
    );


    emitter.store_gpr32(
        rt,
        ARM64Emitter::REG_TMP0
    );


    return true;
}



bool EE_Recompiler::emit_mtc0(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint32_t rd
)
{

    emitter.load_gpr32(
        ARM64Emitter::REG_TMP0,
        rt
    );


    emitter.store_cop0(
        ARM64Emitter::REG_TMP0,
        rd
    );


    return true;
}



// ─────────────────────────────────────────────
// Return from exception
// COP0.Status EXL = 0
// PC = EPC
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_eret(
    ARM64Emitter& emitter
)
{

    emitter.load_cop0(
        ARM64Emitter::REG_TMP0,
        14       // EPC
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.clear_exception_level();


    emitter.ret();


    return true;
}



// ─────────────────────────────────────────────
// SYSCALL
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_syscall(
    ARM64Emitter& emitter
)
{

    emitter.raise_exception(
        8        // Syscall exception
    );


    emitter.ret();


    return true;
}



// ─────────────────────────────────────────────
// BREAK
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_break(
    ARM64Emitter& emitter
)
{

    emitter.raise_exception(
        9        // Break exception
    );


    emitter.ret();


    return true;
}



// ─────────────────────────────────────────────
// Decode SPECIAL2 (R5900)
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_special2(
    ARM64Emitter& emitter,
    uint32_t opcode
)
{

    uint32_t funct =
        opcode & 0x3F;


    uint32_t rs =
        (opcode >> 21) & 31;


    uint32_t rt =
        (opcode >> 16) & 31;


    uint32_t rd =
        (opcode >> 11) & 31;


    switch(funct)
    {

        // MULT1
        case 0x18:
            return emit_mult(
                emitter,
                rs,
                rt
            );


        // DIV1
        case 0x1A:
            return emit_div(
                emitter,
                rs,
                rt
            );


        default:

            emitter.nop();

            return true;
    }

}



// ─────────────────────────────────────────────
// COP1 FPU - Emotion Engine R5900
// Single precision operations
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mfc1(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint32_t fs
)
{
    if (rt == 0)
        return true;


    emitter.load_fpr32(
        ARM64Emitter::REG_TMP0,
        fs
    );


    emitter.store_gpr32(
        rt,
        ARM64Emitter::REG_TMP0
    );


    return true;
}



bool EE_Recompiler::emit_mtc1(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint32_t fs
)
{

    emitter.load_gpr32(
        ARM64Emitter::REG_TMP0,
        rt
    );


    emitter.store_fpr32(
        ARM64Emitter::REG_TMP0,
        fs
    );


    return true;
}



// ─────────────────────────────────────────────
// FPU arithmetic single precision
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_fpu_add_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs,
    uint32_t ft
)
{

    emitter.fadd_s(
        fd,
        fs,
        ft
    );

    return true;
}



bool EE_Recompiler::emit_fpu_sub_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs,
    uint32_t ft
)
{

    emitter.fsub_s(
        fd,
        fs,
        ft
    );

    return true;
}



bool EE_Recompiler::emit_fpu_mul_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs,
    uint32_t ft
)
{

    emitter.fmul_s(
        fd,
        fs,
        ft
    );

    return true;
}



bool EE_Recompiler::emit_fpu_div_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs,
    uint32_t ft
)
{

    emitter.fdiv_s(
        fd,
        fs,
        ft
    );

    return true;
}



bool EE_Recompiler::emit_fpu_sqrt_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs
)
{

    emitter.fsqrt_s(
        fd,
        fs
    );

    return true;
}



bool EE_Recompiler::emit_fpu_abs_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs
)
{

    emitter.fabs_s(
        fd,
        fs
    );

    return true;
}



bool EE_Recompiler::emit_fpu_mov_s(
    ARM64Emitter& emitter,
    uint32_t fd,
    uint32_t fs
)
{

    emitter.fmov_s(
        fd,
        fs
    );

    return true;
}



// ─────────────────────────────────────────────
// COP1 dispatcher
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_cop1(
    ARM64Emitter& emitter,
    uint32_t opcode
)
{

    uint32_t fmt =
        (opcode >> 21) & 31;

    uint32_t ft =
        (opcode >> 16) & 31;

    uint32_t fs =
        (opcode >> 11) & 31;

    uint32_t fd =
        (opcode >> 6) & 31;

    uint32_t funct =
        opcode & 0x3F;



    // Transferencias

    if (fmt == 0x00)
        return emit_mfc1(
            emitter,
            ft,
            fs
        );


    if (fmt == 0x04)
        return emit_mtc1(
            emitter,
            ft,
            fs
        );



    switch(funct)
    {

        case 0x00:
            return emit_fpu_add_s(
                emitter,
                fd,
                fs,
                ft
            );


        case 0x01:
            return emit_fpu_sub_s(
                emitter,
                fd,
                fs,
                ft
            );


        case 0x02:
            return emit_fpu_mul_s(
                emitter,
                fd,
                fs,
                ft
            );


        case 0x03:
            return emit_fpu_div_s(
                emitter,
                fd,
                fs,
                ft
            );


        case 0x04:
            return emit_fpu_sqrt_s(
                emitter,
                fd,
                fs
            );


        case 0x05:
            return emit_fpu_abs_s(
                emitter,
                fd,
                fs
            );


        case 0x06:
            return emit_fpu_mov_s(
                emitter,
                fd,
                fs
            );


        default:
            emitter.nop();
            return true;

    }

}



// ─────────────────────────────────────────────
// MMI SIMD Instructions - Emotion Engine R5900
// Operaciones vectoriales 128 bits
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mmi_paddw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_add_i32(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_mmi_psubw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_sub_i32(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_mmi_paddh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_add_i16(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_mmi_psubh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_sub_i16(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_mmi_paddb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_add_i8(
        rd,
        rs,
        rt
    );

    return true;
}



bool EE_Recompiler::emit_mmi_psubb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_sub_i8(
        rd,
        rs,
        rt
    );

    return true;
}



// Máximo firmado por palabra

bool EE_Recompiler::emit_mmi_pmaxw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_max_i32(
        rd,
        rs,
        rt
    );

    return true;
}



// Mínimo firmado por palabra

bool EE_Recompiler::emit_mmi_pminw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_min_i32(
        rd,
        rs,
        rt
    );

    return true;
}



// Multiplicación vectorial

bool EE_Recompiler::emit_mmi_pmulw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.simd_mul_i32(
        rd,
        rs,
        rt
    );

    return true;
}



// ─────────────────────────────────────────────
// Dispatcher MMI
// ─────────────────────────────────────────────

bool EE_Recompiler::emit_mmi(
    ARM64Emitter& emitter,
    uint32_t opcode
)
{

    uint32_t funct =
        opcode & 0x3F;


    uint32_t rs =
        (opcode >> 21) & 31;


    uint32_t rt =
        (opcode >> 16) & 31;


    uint32_t rd =
        (opcode >> 11) & 31;



    switch(funct)
    {

        case 0x00:
            return emit_mmi_paddw(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x04:
            return emit_mmi_psubw(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x08:
            return emit_mmi_paddh(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x0C:
            return emit_mmi_psubh(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x10:
            return emit_mmi_paddb(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x14:
            return emit_mmi_psubb(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x28:
            return emit_mmi_pmaxw(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x29:
            return emit_mmi_pminw(
                emitter,
                rd,
                rs,
                rt
            );


        case 0x30:
            return emit_mmi_pmulw(
                emitter,
                rd,
                rs,
                rt
            );


        default:

            emitter.nop();

            return true;
    }

}


// ─────────────────────────────────────────────
// R5900 Extended Memory Operations
// 64-bit + 128-bit PS2 instructions
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_ld(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load64(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_sd(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.store64(
        rt,
        base,
        offset
    );

    return true;
}



// Load quadword PS2
// Registro GPR par/impar


bool EE_Recompiler::emit_lq(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load128(
        rt,
        base,
        offset
    );

    return true;
}



// Store quadword PS2


bool EE_Recompiler::emit_sq(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.store128(
        rt,
        base,
        offset
    );

    return true;
}



// ─────────────────────────────────────────────
// Unaligned loads
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lwl(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_left32(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_lwr(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_right32(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_swl(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.store_left32(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_swr(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.store_right32(
        rt,
        base,
        offset
    );

    return true;
}



// ─────────────────────────────────────────────
// R5900 special immediate operations
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_daddi(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{

    if(rt == 0)
        return true;


    emitter.add_imm64(
        rt,
        rs,
        imm
    );

    return true;
}



bool EE_Recompiler::emit_daddiu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{

    if(rt == 0)
        return true;


    emitter.add_imm64(
        rt,
        rs,
        imm
    );

    return true;
}


// ─────────────────────────────────────────────
// Immediate Arithmetic / Logic Instructions
// R5900 MIPS
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lui(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;

    uint32_t value = ((uint32_t)imm) << 16;

    emitter.mov_imm(
        rt,
        value
    );

    return true;
}



bool EE_Recompiler::emit_ori(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.or_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        imm
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    return true;
}



bool EE_Recompiler::emit_andi(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.and_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        imm
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    return true;
}



bool EE_Recompiler::emit_xori(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.xor_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        imm
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    return true;
}



bool EE_Recompiler::emit_slti(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    int16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.compare_state_imm(
        rs,
        imm
    );


    emitter.set_less_signed(
        rt
    );


    return true;
}



bool EE_Recompiler::emit_sltiu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt,
    uint16_t imm
)
{
    if (rt == 0)
        return true;


    emitter.compare_state_imm_unsigned(
        rs,
        imm
    );


    emitter.set_less_unsigned(
        rt
    );


    return true;
}



// ─────────────────────────────────────────────
// Memory load helpers
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lb(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load8_signed(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_lbu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load8_unsigned(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_lh(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load16_signed(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_lhu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load16_unsigned(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_lwu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load32_unsigned(
        rt,
        base,
        offset
    );

    return true;
}



// ─────────────────────────────────────────────
// Store helpers
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_sb(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    emitter.store8(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_sh(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    emitter.store16(
        rt,
        base,
        offset
    );

    return true;
}



bool EE_Recompiler::emit_sw(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    emitter.store32(
        rt,
        base,
        offset
    );

    return true;
}


// ─────────────────────────────────────────────
// HI / LO Register Operations
// R5900 Integer Multiply / Divide
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mult(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{
    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );

    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.mul_signed32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo)
    );


    emitter.mul_high_signed32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi)
    );


    return true;
}



bool EE_Recompiler::emit_multu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.mul_unsigned32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo)
    );


    emitter.mul_high_unsigned32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi)
    );


    return true;
}




bool EE_Recompiler::emit_div(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.div_signed32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo)
    );


    emitter.mod_signed32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi)
    );


    return true;
}




bool EE_Recompiler::emit_divu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.div_unsigned32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo)
    );


    emitter.mod_unsigned32(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi)
    );


    return true;
}



// ─────────────────────────────────────────────
// Move HI / LO
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mfhi(
    ARM64Emitter& emitter,
    uint32_t rd
)
{
    if(rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rd])
    );


    return true;
}



bool EE_Recompiler::emit_mflo(
    ARM64Emitter& emitter,
    uint32_t rd
)
{
    if(rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rd])
    );


    return true;
}



bool EE_Recompiler::emit_mthi(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    return true;
}



bool EE_Recompiler::emit_mtlo(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    return true;
}


// ─────────────────────────────────────────────
// COP0 Instructions
// Emotion Engine System Control
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mfc0(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint32_t rd
)
{

    if(rt == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, cop0[rd])
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    return true;
}




bool EE_Recompiler::emit_mtc0(
    ARM64Emitter& emitter,
    uint32_t rt,
    uint32_t rd
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, cop0[rd])
    );


    return true;
}




// ─────────────────────────────────────────────
// COP0 Helpers
// ─────────────────────────────────────────────


void EE_Recompiler::emit_exception(
    ARM64Emitter& emitter,
    uint32_t code
)
{

    // Cause register
    emitter.mov_imm(
        ARM64Emitter::REG_TMP0,
        code << 2
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, cop0[13])
    );


    // EXL bit Status
    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, cop0[12])
    );


    emitter.or_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        0x2
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, cop0[12])
    );


    // EPC = PC
    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, cop0[14])
    );


    // Exception vector
    emitter.mov_imm(
        ARM64Emitter::REG_TMP0,
        0x80000180
    );


    emitter.store_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, pc)
    );

}




void EE_Recompiler::update_cop0_status(
    uint32_t value
)
{

    m_state.cop0[12] = value;

}




uint32_t EE_Recompiler::read_cop0(
    uint32_t index
)
{

    return m_state.cop0[index & 31];

}




void EE_Recompiler::write_cop0(
    uint32_t index,
    uint32_t value
)
{

    m_state.cop0[index & 31] = value;

}



// ─────────────────────────────────────────────
// SYSCALL / BREAK
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_syscall(
    ARM64Emitter& emitter
)
{

    emit_exception(
        emitter,
        8
    );

    return true;
}



bool EE_Recompiler::emit_break(
    ARM64Emitter& emitter
)
{

    emit_exception(
        emitter,
        9
    );

    return true;
}



// ─────────────────────────────────────────────
// Memory byte / halfword / unaligned operations
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lb(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;

    emitter.load_memory8_signed(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );

    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );

    return true;
}



bool EE_Recompiler::emit_lbu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;

    emitter.load_memory8(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );

    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );

    return true;
}



bool EE_Recompiler::emit_lh(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;

    emitter.load_memory16_signed(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );

    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );

    return true;
}



bool EE_Recompiler::emit_lhu(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;

    emitter.load_memory16(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );

    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );

    return true;
}



bool EE_Recompiler::emit_sb(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.store_memory8(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_sh(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.store_memory16(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );


    return true;
}



// ─────────────────────────────────────────────
// Unaligned loads/stores
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_lwl(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load_memory32(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    return true;
}



bool EE_Recompiler::emit_lwr(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{
    if (rt == 0)
        return true;


    emitter.load_memory32(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    return true;
}



bool EE_Recompiler::emit_swl(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.store_memory32(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );


    return true;
}



bool EE_Recompiler::emit_swr(
    ARM64Emitter& emitter,
    uint32_t base,
    uint32_t rt,
    int16_t offset
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.store_memory32(
        ARM64Emitter::REG_TMP0,
        base,
        offset
    );


    return true;
}



// ─────────────────────────────────────────────
// Multiplicación MIPS R5900
// HI/LO handling
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mult(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{
    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );

    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.mul_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo)
    );


    emitter.smulh_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi)
    );


    return true;
}



// ─────────────────────────────────────────────
// MULT unsigned
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_multu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.mul_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo)
    );


    emitter.umulh_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi)
    );


    return true;
}



// ─────────────────────────────────────────────
// Move HI → registro
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mfhi(
    ARM64Emitter& emitter,
    uint32_t rd
)
{

    if(rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rd])
    );


    return true;
}



// ─────────────────────────────────────────────
// Move LO → registro
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mflo(
    ARM64Emitter& emitter,
    uint32_t rd
)
{

    if(rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rd])
    );


    return true;
}



// ─────────────────────────────────────────────
// MTHI
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mthi(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    return true;
}



// ─────────────────────────────────────────────
// MTLO
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mtlo(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    return true;
}




// ─────────────────────────────────────────────
// DIV signed
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_div(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.div_signed(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    emitter.mod_signed(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    return true;

}



// ─────────────────────────────────────────────
// DIV unsigned
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_divu(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rs])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.div_unsigned(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    emitter.mod_unsigned(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    return true;

}



// ─────────────────────────────────────────────
// Shift inmediato
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_shift_imm(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t shamt,
    uint32_t type
)
{

    if(rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    switch(type)
    {

        case 0:
            emitter.lsl_imm(
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP0,
                shamt
            );
            break;


        case 1:
            emitter.lsr_imm(
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP0,
                shamt
            );
            break;


        case 2:
            emitter.asr_imm(
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP0,
                shamt
            );
            break;


        default:
            return false;

    }


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rd])
    );


    return true;

}



// ─────────────────────────────────────────────
// Shift variable
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_shift_var(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t rs,
    uint32_t type
)
{

    if(rd == 0)
        return true;


    emitter.load_state32(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rt])
    );


    emitter.load_state32(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr_lo[rs])
    );


    switch(type)
    {

        case 0:
            emitter.lsl_reg(
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP1
            );
            break;


        case 1:
            emitter.lsr_reg(
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP1
            );
            break;


        case 2:
            emitter.asr_reg(
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP0,
                ARM64Emitter::REG_TMP1
            );
            break;


        default:
            return false;

    }


    emitter.store32_state(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr_lo[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// Shifts 64-bit R5900
// ─────────────────────────────────────────────


// DSLL
bool EE_Recompiler::emit_dsll(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.lsl64_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        sa
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// DSRL
bool EE_Recompiler::emit_dsrl(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.lsr64_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        sa
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// DSRA
bool EE_Recompiler::emit_dsra(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.asr64_imm(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        sa
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// DSLL32
bool EE_Recompiler::emit_dsll32(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{

    return emit_dsll(
        emitter,
        rd,
        rt,
        sa + 32
    );

}



// DSRL32
bool EE_Recompiler::emit_dsrl32(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{

    return emit_dsrl(
        emitter,
        rd,
        rt,
        sa + 32
    );

}



// DSRA32
bool EE_Recompiler::emit_dsra32(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t sa
)
{

    return emit_dsra(
        emitter,
        rd,
        rt,
        sa + 32
    );

}




// ─────────────────────────────────────────────
// Shifts 64-bit variables R5900
// ─────────────────────────────────────────────


// DSLLV
bool EE_Recompiler::emit_dsllv(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t rs
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rs])
    );


    emitter.lsl64_reg(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// DSRLV
bool EE_Recompiler::emit_dsrlv(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t rs
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rs])
    );


    emitter.lsr64_reg(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// DSRAV
bool EE_Recompiler::emit_dsrav(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt,
    uint32_t rs
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rs])
    );


    emitter.asr64_reg(
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// ─────────────────────────────────────────────
// Helpers de HI/LO 64-bit
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_mfhi64(
    ARM64Emitter& emitter,
    uint32_t rd
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi)
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



bool EE_Recompiler::emit_mflo64(
    ARM64Emitter& emitter,
    uint32_t rd
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo)
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 Dual HI/LO Unit
// MULT1 / MULTU1
// ─────────────────────────────────────────────


// MULT1 signed
bool EE_Recompiler::emit_mult1(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    // HI1 = parte alta del resultado
    emitter.mul64_high_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi1)
    );


    return true;

}



// MULTU1 unsigned
bool EE_Recompiler::emit_multu1(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    emitter.mul64_high_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi1)
    );


    return true;

}



// MFHI1
bool EE_Recompiler::emit_mfhi1(
    ARM64Emitter& emitter,
    uint32_t rd
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi1)
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// MFLO1
bool EE_Recompiler::emit_mflo1(
    ARM64Emitter& emitter,
    uint32_t rd
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo1)
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 Dual HI/LO Unit
// DIV1 / DIVU1 / MTHI1 / MTLO1
// ─────────────────────────────────────────────


// DIV1 signed
bool EE_Recompiler::emit_div1(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    // División por cero:
    // HI = numerador
    // LO = -1 o 1 según signo
    emitter.compare_zero(
        ARM64Emitter::REG_TMP1
    );


    emitter.div64_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    emitter.mod64_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi1)
    );


    return true;

}



// DIVU1 unsigned
bool EE_Recompiler::emit_divu1(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.div64_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    emitter.mod64_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, hi1)
    );


    return true;

}



// MTHI1
bool EE_Recompiler::emit_mthi1(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, hi1)
    );


    return true;

}



// MTLO1
bool EE_Recompiler::emit_mtlo1(
    ARM64Emitter& emitter,
    uint32_t rs
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, lo1)
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI0
// Packed arithmetic
// ─────────────────────────────────────────────


// PADDW
bool EE_Recompiler::emit_paddw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.add32_pair(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSUBW
bool EE_Recompiler::emit_psubw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.sub32_pair(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PADDUW
bool EE_Recompiler::emit_padduw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.add32_pair_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSUBUW
bool EE_Recompiler::emit_psubuw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.sub32_pair_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI0
// Packed Halfword Arithmetic
// ─────────────────────────────────────────────


// PADDH
bool EE_Recompiler::emit_paddh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.add16_pair(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSUBH
bool EE_Recompiler::emit_psubh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.sub16_pair(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PADDSH
bool EE_Recompiler::emit_paddsh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.add16_sat_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSUBSH
bool EE_Recompiler::emit_psubsh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.sub16_sat_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI1
// Packed Compare Instructions
// ─────────────────────────────────────────────



// PCEQB
bool EE_Recompiler::emit_pceqb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_equal_bytes(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCEQH
bool EE_Recompiler::emit_pceqh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_equal_halfwords(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCEQW
bool EE_Recompiler::emit_pceqw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_equal_words(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCGTB
bool EE_Recompiler::emit_pcgtb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_greater_bytes_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCGTH
bool EE_Recompiler::emit_pcgth(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_greater_halfwords_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCGTW
bool EE_Recompiler::emit_pcgtw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_greater_words_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI
// Packed Min / Max / Absolute
// ─────────────────────────────────────────────



// PMAXW
bool EE_Recompiler::emit_pmaxw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.max32_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PMINW
bool EE_Recompiler::emit_pminw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.min32_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PABSW
bool EE_Recompiler::emit_pabsw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.abs32_pair(
        ARM64Emitter::REG_TMP1,
        ARM64Emitter::REG_TMP0
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PADSBH
bool EE_Recompiler::emit_padsbh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.add_sub_bytes_halfwords_sat(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI
// Multiply / Accumulate Instructions
// ─────────────────────────────────────────────



// PMULTW
bool EE_Recompiler::emit_pmultw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul32_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PMULTUW
bool EE_Recompiler::emit_pmultuw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul32_pair_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PMADDW
bool EE_Recompiler::emit_pmaddw(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul32_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.add64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP3
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    return true;

}



// PMADDUW
bool EE_Recompiler::emit_pmadd_uw(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul32_pair_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.add64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP3
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI
// Multiply Subtract / Halfword Accumulate
// ─────────────────────────────────────────────



// PMSUBW
bool EE_Recompiler::emit_pmsubw(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul32_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.sub64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP3,
        ARM64Emitter::REG_TMP2
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    return true;

}



// PMSUBUW
bool EE_Recompiler::emit_pmsubuw(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul32_pair_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.sub64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP3,
        ARM64Emitter::REG_TMP2
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    return true;

}



// PMADDH
bool EE_Recompiler::emit_pmaddh(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul16_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.add64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP3
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    return true;

}



// PMADDUH
bool EE_Recompiler::emit_pmadd_uh(
    ARM64Emitter& emitter,
    uint32_t rs,
    uint32_t rt
)
{

    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.mul16_pair_unsigned(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.add64(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP3
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, lo1)
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI
// Horizontal Multiply / Variable Shifts
// ─────────────────────────────────────────────



// PHMADH
bool EE_Recompiler::emit_phmadh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.horizontal_mul16_acc(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PHMSBH
bool EE_Recompiler::emit_phmsbh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.horizontal_mul16_sub(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// QFSRV
bool EE_Recompiler::emit_qfsrv(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.qfsrv(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSLLVW
bool EE_Recompiler::emit_psllvw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.shift_left_words_variable(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSRLVW
bool EE_Recompiler::emit_psrlvw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.shift_right_words_variable(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PSRAVW
bool EE_Recompiler::emit_psravw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.shift_right_arith_words_variable(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI
// Pack / Extend Instructions
// ─────────────────────────────────────────────



// PEXTLB
bool EE_Recompiler::emit_pextlb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.extend_low_bytes(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PEXTLH
bool EE_Recompiler::emit_pextlh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.extend_low_halfwords(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PEXTLW
bool EE_Recompiler::emit_pextlw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.extend_low_words(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PEXTLBH
bool EE_Recompiler::emit_pextlbh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.extend_bytes_halfwords(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PPACB
bool EE_Recompiler::emit_ppacb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.pack_bytes(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PPACH
bool EE_Recompiler::emit_ppach(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.pack_halfwords(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PPACW
bool EE_Recompiler::emit_ppacw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.pack_words(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// R5900 MMI
// Logic / Copy / Pack 5-bit
// ─────────────────────────────────────────────



// PCPYLD
bool EE_Recompiler::emit_pcpyld(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.copy_low_doubleword(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCPYUD
bool EE_Recompiler::emit_pcpyud(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.copy_upper_doubleword(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PAND
bool EE_Recompiler::emit_pand(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.and_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// POR
bool EE_Recompiler::emit_por(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.or_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PXOR
bool EE_Recompiler::emit_pxor(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.eor_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PNOR
bool EE_Recompiler::emit_pnor(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.nor_reg(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PEXT5
bool EE_Recompiler::emit_pext5(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.extract_rgb5(
        ARM64Emitter::REG_TMP1,
        ARM64Emitter::REG_TMP0
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PPAC5
bool EE_Recompiler::emit_ppac5(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rt])
    );


    emitter.pack_rgb5(
        ARM64Emitter::REG_TMP1,
        ARM64Emitter::REG_TMP0
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// MMI: MAX / MIN signed byte
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_pminsb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.min8_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// Pmaxsb

bool EE_Recompiler::emit_pmaxsb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.max8_pair_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// MMI: Saturación y comparación de bytes
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_pceqb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_equal_bytes(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCLEB

bool EE_Recompiler::emit_pcleb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_less_equal_bytes_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCGTB

bool EE_Recompiler::emit_pcgtb(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_greater_bytes_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// MMI: Comparaciones halfword restantes
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_pceqh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_equal_halfwords(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCLEH

bool EE_Recompiler::emit_pcleh(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_less_equal_halfwords_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCGTH

bool EE_Recompiler::emit_pcgth(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_greater_halfwords_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}




// ─────────────────────────────────────────────
// MMI: Comparaciones word restantes
// ─────────────────────────────────────────────


bool EE_Recompiler::emit_pceqw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_equal_words(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCLEW

bool EE_Recompiler::emit_pclew(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_less_equal_words_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}



// PCGTW

bool EE_Recompiler::emit_pcgtw(
    ARM64Emitter& emitter,
    uint32_t rd,
    uint32_t rs,
    uint32_t rt
)
{

    if(rd == 0)
        return true;


    emitter.load_state64(
        ARM64Emitter::REG_TMP0,
        offsetof(EE_State, gpr[rs])
    );


    emitter.load_state64(
        ARM64Emitter::REG_TMP1,
        offsetof(EE_State, gpr[rt])
    );


    emitter.compare_greater_words_signed(
        ARM64Emitter::REG_TMP2,
        ARM64Emitter::REG_TMP0,
        ARM64Emitter::REG_TMP1
    );


    emitter.store_state64(
        ARM64Emitter::REG_TMP2,
        offsetof(EE_State, gpr[rd])
    );


    return true;

}


