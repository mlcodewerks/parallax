/* Operate in allocated host registers rather than copying every operand
 * through T0/T1. Preserve source aliases and full-width word semantics.
 * GPL-2.0-or-later. */
static int allocated_source(unsigned guest, int temporary)
{
    if (regcache.known & (UINT64_C(1) << guest)) {
        emit_movimm64(regcache.constants[guest],temporary); return temporary;
    }
    int h = cache_find(guest);
    if (h < 0) { h = cache_allocate(guest); emit_state_load_raw(guest_offset(guest),cache_host[h]); }
    return cache_host[h];
}
static int allocated_destination(unsigned guest)
{
    int h = cache_allocate(guest);
    regcache.known &= ~(UINT64_C(1) << guest); regcache.constant_dirty &= ~(UINT64_C(1) << guest);
    regcache.dirty |= 1u << h;
    return cache_host[h];
}
static void emit_word_extend(int reg)
{
    a64(0x93407c00 | (reg<<5) | reg);
}
static int regcache_direct(uint32_t instruction)
{
    enum r4300_opcode op = r4300_get_idec(instruction)->opcode;
    unsigned a,b,d; guest_operands(instruction,&a,&b,&d);
    int word = 0, immediate = 0, compare = 0;
    unsigned opcode = 0, extension = 0;
    switch (op) {
    case R4300_OP_ADDIU: case R4300_OP_DADDIU: immediate = 1; extension = 0; word = op == R4300_OP_ADDIU; break;
    case R4300_OP_ANDI: immediate = 1; extension = 4; break;
    case R4300_OP_ORI: immediate = 1; extension = 1; break;
    case R4300_OP_XORI: immediate = 1; extension = 6; break;
    case R4300_OP_ADDU: case R4300_OP_DADDU: opcode = 0x01; word = op == R4300_OP_ADDU; break;
    case R4300_OP_SUBU: case R4300_OP_DSUBU: opcode = 0x29; word = op == R4300_OP_SUBU; break;
    case R4300_OP_AND: opcode = 0x21; break;
    case R4300_OP_OR: case R4300_OP_NOR: opcode = 0x09; break;
    case R4300_OP_XOR: opcode = 0x31; break;
    case R4300_OP_SLT: case R4300_OP_SLTU: opcode = 0x39; compare = 1; break;
    case R4300_OP_SLTI: case R4300_OP_SLTIU: immediate = compare = 1; extension = 7; break;
    default: return 0;
    }
    if (!d) return 1;
    int left = allocated_source(a,T0), right = immediate ? -1 : allocated_source(b,T1);
    if (compare) {
        if (immediate) emit_imm_op(extension,left,(uint32_t)(int32_t)(int16_t)instruction,1);
        else emit_reg_op(opcode,right,left,1);
        int dest = allocated_destination(d);
        emit_set_condition(dest,op == R4300_OP_SLT || op == R4300_OP_SLTI ? 12 : 2);
        return 1;
    }
    int dest = allocated_destination(d);
    if (!immediate && right == dest && left != dest) {
        if (opcode != 0x29) { int saved = left; left = right; right = saved; }
        else { emit_mov64(right,T1); right = T1; }
    }
    if (left != dest) emit_mov64(left,dest);
    if (immediate) {
        uint32_t imm = op == R4300_OP_ADDIU || op == R4300_OP_DADDIU ? (uint32_t)(int32_t)(int16_t)instruction : (uint16_t)instruction;
        if (imm || op == R4300_OP_ANDI) emit_imm_op(extension,dest,imm,!word && op != R4300_OP_ANDI);
    } else emit_reg_op(opcode,right,dest,!word);
    if (op == R4300_OP_NOR) { a64(0xaa2003e0 | (dest<<16) | dest); }
    if (word) emit_word_extend(dest);
    return 1;
}
