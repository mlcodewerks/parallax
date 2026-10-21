/* Full-width VR4300 multiply/divide, including this core's MULT/DIV operand
 * rules and deterministic zero/overflow results. GPL-2.0-or-later. */
static void emit_muldiv(uint32_t instruction,enum r4300_opcode op)
{
    int word=op==R4300_OP_MULT || op==R4300_OP_MULTU || op==R4300_OP_DIV || op==R4300_OP_DIVU;
    int sign=op==R4300_OP_MULT || op==R4300_OP_DMULT || op==R4300_OP_DIV || op==R4300_OP_DDIV;
    int multiply=op==R4300_OP_MULT || op==R4300_OP_MULTU || op==R4300_OP_DMULT || op==R4300_OP_DMULTU;
    emit_state_load(gpr_offset((instruction>>21)&31),T0);
    emit_state_load(gpr_offset((instruction>>16)&31),T1);
    if(multiply) {
        if(word && sign) { emit_shift_reg(T1,4,29,1); emit_shift_reg(T1,7,29,1); }
        else if(word) { emit_mov(T0,T0); emit_mov(T1,T1); }
        if(!word) a64((sign?0x9b407c00:0x9bc07c00)|(T1<<16)|(T0<<5)|T2); /* SMULH/UMULH */
        a64(0x9b007c00|(T1<<16)|(T0<<5)|T0); /* MUL */
        if(word) { emit_mov64(T0,T2); emit_shift_reg(T2,5,32,1); }
    } else {
        if(word) {
            if(sign) emit_sign_extend_word();
            else { emit_mov(T0,T0); emit_mov(T1,T1); }
        }
        emit_reg_op(0x85,T1,T1,1); unsigned char* zero=emit_jump(4);
        emit_mov64(T0,T2);
        a64((sign?0x9ac00c00:0x9ac00800)|(T1<<16)|(T0<<5)|T0); /* SDIV/UDIV */
        a64(0x9b008000|(T1<<16)|(T2<<10)|(T0<<5)|T2); /* MSUB remainder */
        unsigned char* done=emit_b();
        patch_jump(zero,out); emit_mov64(T0,T2);
        if(sign) {
            emit_shift_reg(T0,7,63,1); a64(0xaa2003e0|(T0<<16)|T0); emit_imm_op(1,T0,1,1);
        } else emit_movimm64(-1,T0);
        patch_jump(done,out);
    }
    if(word) { emit_sign_extend_word(); a64(0x93407c00|(T2<<5)|T2); }
    emit_state_store(offsetof(struct r4300_core,lo),T0);
    emit_state_store(offsetof(struct r4300_core,hi),T2);
}
