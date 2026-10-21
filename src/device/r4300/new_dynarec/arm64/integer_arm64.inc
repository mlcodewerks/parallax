static int native_opcode(enum r4300_opcode op)
{
    switch (op) {
    case R4300_OP_NOP: case R4300_OP_SYNC:
    case R4300_OP_ADDIU: case R4300_OP_DADDIU: case R4300_OP_ADDU: case R4300_OP_DADDU:
    case R4300_OP_SUBU: case R4300_OP_DSUBU: case R4300_OP_LUI:
    case R4300_OP_AND: case R4300_OP_ANDI: case R4300_OP_OR: case R4300_OP_ORI:
    case R4300_OP_XOR: case R4300_OP_XORI: case R4300_OP_NOR:
    case R4300_OP_SLT: case R4300_OP_SLTU: case R4300_OP_SLTI: case R4300_OP_SLTIU:
    case R4300_OP_SLL: case R4300_OP_SLLV: case R4300_OP_SRL: case R4300_OP_SRLV:
    case R4300_OP_SRA: case R4300_OP_SRAV:
    case R4300_OP_DSLL: case R4300_OP_DSLL32: case R4300_OP_DSLLV:
    case R4300_OP_DSRL: case R4300_OP_DSRL32: case R4300_OP_DSRLV:
    case R4300_OP_DSRA: case R4300_OP_DSRA32: case R4300_OP_DSRAV:
    case R4300_OP_MFHI: case R4300_OP_MFLO: case R4300_OP_MTHI: case R4300_OP_MTLO:
    case R4300_OP_MULT: case R4300_OP_MULTU: case R4300_OP_DMULT: case R4300_OP_DMULTU:
    case R4300_OP_DIV: case R4300_OP_DIVU: case R4300_OP_DDIV: case R4300_OP_DDIVU:
        return 1;
    default: return 0;
    }
}


static void emit_native(uint32_t op, enum r4300_opcode decoded)
{
    (void)decoded;
    if (!(op >> 26) && (op & 63) >= 24 && (op & 63) <= 31) { emit_muldiv(op,decoded); return; }
    unsigned rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
    unsigned major = op >> 26, fn = op & 63, sa = (op >> 6) & 31;
    int word = 0;
    if (!op || (!major && fn == 15)) return;
    emit_state_load(gpr_offset(rs),9); emit_state_load(gpr_offset(rt),10);
    if (major) {
        rd = rt;
        emit_movimm64(major >= 12 && major <= 14 ? (uint16_t)op : (uint64_t)(int64_t)(int16_t)op,10);
        switch (major) {
        case 9: case 25: /* ADDIU / DADDIU */
            a64( (major == 9 ? 0x0b0a0129 : 0x8b0a0129)); word = major == 9; break;
        case 12: a64( 0x8a0a0129); break;
        case 13: a64( 0xaa0a0129); break;
        case 14: a64( 0xca0a0129); break;
        case 15: emit_movimm64((uint64_t)(int64_t)(int32_t)(op << 16),9); break;
        case 10: case 11:
            a64( 0xeb0a013f); /* CMP X9,X10 */
            a64( major == 10 ? 0x9a9fa7e9 : 0x9a9f27e9); /* CSET X9,LT/LO */
            break;
        }
    } else if (fn == 16 || fn == 18) {
        emit_state_load(fn == 16 ? offsetof(struct r4300_core, hi) : offsetof(struct r4300_core, lo),9);
    } else if (fn == 17 || fn == 19) {
        emit_state_store(fn == 17 ? offsetof(struct r4300_core, hi) : offsetof(struct r4300_core, lo),9); return;
    } else if (fn <= 7 || fn == 20 || fn == 22 || fn == 23 || fn >= 56) {
        unsigned kind = fn & 3; /* 0: left, 2: logical right, 3: arithmetic right */
        int variable = (fn >= 4 && fn <= 7) || (fn >= 20 && fn <= 23);
        word = fn <= 7;
        if (variable) {
            if (word && kind == 3) { /* shared SRAV uses full 64-bit operand, five-bit shift */
                a64( 0x92401129); /* AND X9,X9,#31 */
            }
        } else emit_movimm64(sa + (fn >= 60 ? 32 : 0),9);
        uint32_t base = kind == 0 ? 0x1ac02000 : kind == 2 ? 0x1ac02400 : 0x1ac02800;
        if (!word || kind == 3) base |= 0x80000000;
        a64( base | (9 << 16) | (10 << 5) | 9); /* LSLV/LSRV/ASRV X/W9,X/W10,X/W9 */
    } else {
        switch (fn) {
        case 33: case 45: a64( fn == 33 ? 0x0b0a0129 : 0x8b0a0129); word = fn == 33; break;
        case 35: case 47: a64( fn == 35 ? 0x4b0a0129 : 0xcb0a0129); word = fn == 35; break;
        case 36: a64( 0x8a0a0129); break;
        case 37: case 39:
            a64( 0xaa0a0129);
            if (fn == 39) a64( 0xaa2903e9); /* MVN X9,X9 */
            break;
        case 38: a64( 0xca0a0129); break;
        case 42: case 43:
            a64( 0xeb0a013f);
            a64( fn == 42 ? 0x9a9fa7e9 : 0x9a9f27e9); break;
        }
    }
    if (word) a64( 0x93407d29); /* SXTW X9,W9 */
    if (rd) emit_state_store(offsetof(struct r4300_core, regs) + rd * 8,9);
}
