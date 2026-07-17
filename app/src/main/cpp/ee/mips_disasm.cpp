// ee/mips_disasm.cpp
// Minimal MIPS R5900 disassembler — used only in debug builds.
#include <cstdint>
#include <cstdio>
#include <cstring>

static const char* GPR_NAMES[32] = {
    "r0","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra"
};

// Disassemble one MIPS instruction. Returns pointer to a static buffer.
const char* mips_disasm(uint32_t insn, uint32_t pc) {
    static char buf[64];
    uint32_t op  = (insn >> 26) & 0x3F;
    uint32_t rs  = (insn >> 21) & 0x1F;
    uint32_t rt  = (insn >> 16) & 0x1F;
    uint32_t rd  = (insn >> 11) & 0x1F;
    uint32_t sa  = (insn >>  6) & 0x1F;
    uint32_t fn  = (insn >>  0) & 0x3F;
    int16_t  imm = (int16_t)(insn & 0xFFFF);
    uint32_t tgt = (insn & 0x03FFFFFFu) << 2;

    switch (op) {
        case 0x00: // SPECIAL
            switch (fn) {
                case 0x00: snprintf(buf,sizeof(buf),"sll %s,%s,%u",    GPR_NAMES[rd],GPR_NAMES[rt],sa); break;
                case 0x02: snprintf(buf,sizeof(buf),"srl %s,%s,%u",    GPR_NAMES[rd],GPR_NAMES[rt],sa); break;
                case 0x03: snprintf(buf,sizeof(buf),"sra %s,%s,%u",    GPR_NAMES[rd],GPR_NAMES[rt],sa); break;
                case 0x08: snprintf(buf,sizeof(buf),"jr %s",           GPR_NAMES[rs]); break;
                case 0x09: snprintf(buf,sizeof(buf),"jalr %s,%s",      GPR_NAMES[rd],GPR_NAMES[rs]); break;
                case 0x0C: snprintf(buf,sizeof(buf),"syscall"); break;
                case 0x10: snprintf(buf,sizeof(buf),"mfhi %s",         GPR_NAMES[rd]); break;
                case 0x12: snprintf(buf,sizeof(buf),"mflo %s",         GPR_NAMES[rd]); break;
                case 0x18: snprintf(buf,sizeof(buf),"mult %s,%s",      GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x1A: snprintf(buf,sizeof(buf),"div %s,%s",       GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x20: snprintf(buf,sizeof(buf),"add %s,%s,%s",    GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x21: snprintf(buf,sizeof(buf),"addu %s,%s,%s",   GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x22: snprintf(buf,sizeof(buf),"sub %s,%s,%s",    GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x24: snprintf(buf,sizeof(buf),"and %s,%s,%s",    GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x25: snprintf(buf,sizeof(buf),"or %s,%s,%s",     GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x26: snprintf(buf,sizeof(buf),"xor %s,%s,%s",    GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x27: snprintf(buf,sizeof(buf),"nor %s,%s,%s",    GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x2A: snprintf(buf,sizeof(buf),"slt %s,%s,%s",    GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                case 0x2B: snprintf(buf,sizeof(buf),"sltu %s,%s,%s",   GPR_NAMES[rd],GPR_NAMES[rs],GPR_NAMES[rt]); break;
                default:   snprintf(buf,sizeof(buf),"SPECIAL fn=%02X", fn); break;
            }
            break;
        case 0x02: snprintf(buf,sizeof(buf),"j 0x%08X",   (pc & 0xF0000000u)|tgt); break;
        case 0x03: snprintf(buf,sizeof(buf),"jal 0x%08X", (pc & 0xF0000000u)|tgt); break;
        case 0x04: snprintf(buf,sizeof(buf),"beq %s,%s,%+d",  GPR_NAMES[rs],GPR_NAMES[rt],(int)imm*4); break;
        case 0x05: snprintf(buf,sizeof(buf),"bne %s,%s,%+d",  GPR_NAMES[rs],GPR_NAMES[rt],(int)imm*4); break;
        case 0x08: snprintf(buf,sizeof(buf),"addi %s,%s,%d",  GPR_NAMES[rt],GPR_NAMES[rs],(int)imm); break;
        case 0x09: snprintf(buf,sizeof(buf),"addiu %s,%s,%d", GPR_NAMES[rt],GPR_NAMES[rs],(int)imm); break;
        case 0x0A: snprintf(buf,sizeof(buf),"slti %s,%s,%d",  GPR_NAMES[rt],GPR_NAMES[rs],(int)imm); break;
        case 0x0C: snprintf(buf,sizeof(buf),"andi %s,%s,%u",  GPR_NAMES[rt],GPR_NAMES[rs],(uint16_t)imm); break;
        case 0x0D: snprintf(buf,sizeof(buf),"ori %s,%s,%u",   GPR_NAMES[rt],GPR_NAMES[rs],(uint16_t)imm); break;
        case 0x0F: snprintf(buf,sizeof(buf),"lui %s,0x%04X",  GPR_NAMES[rt],(uint16_t)imm); break;
        case 0x20: snprintf(buf,sizeof(buf),"lb %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x21: snprintf(buf,sizeof(buf),"lh %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x23: snprintf(buf,sizeof(buf),"lw %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x24: snprintf(buf,sizeof(buf),"lbu %s,%d(%s)",  GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x25: snprintf(buf,sizeof(buf),"lhu %s,%d(%s)",  GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x28: snprintf(buf,sizeof(buf),"sb %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x29: snprintf(buf,sizeof(buf),"sh %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x2B: snprintf(buf,sizeof(buf),"sw %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x37: snprintf(buf,sizeof(buf),"ld %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        case 0x3F: snprintf(buf,sizeof(buf),"sd %s,%d(%s)",   GPR_NAMES[rt],(int)imm,GPR_NAMES[rs]); break;
        default:   snprintf(buf,sizeof(buf),"UNK op=%02X @%08X", op, pc); break;
    }
    return buf;
}
