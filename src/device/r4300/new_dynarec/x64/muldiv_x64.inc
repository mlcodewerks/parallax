/* Full-width integer multiply/divide matching pure_interp.c. GPL-2.0-or-later. */
static void emit_muldiv(uint32_t instruction, enum r4300_opcode op)
{
    unsigned rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
    int word = op == R4300_OP_MULT || op == R4300_OP_MULTU || op == R4300_OP_DIV || op == R4300_OP_DIVU;
    int sign = op == R4300_OP_MULT || op == R4300_OP_DMULT || op == R4300_OP_DIV || op == R4300_OP_DDIV;
    int multiply = op == R4300_OP_MULT || op == R4300_OP_MULTU || op == R4300_OP_DMULT || op == R4300_OP_DMULTU;
    emit_state_load(gpr_offset(rs), RAX); emit_state_load(gpr_offset(rt), RCX);
    if (multiply) {
        if (word) {
            if (sign) {
                /* The core's MULT multiplies a 64-bit lhs by a signed 35-bit
                 * rhs. Truncating both operands to 32 bits changes HI. */
                emit_shift_reg(RCX, 4, 29, 1); emit_shift_reg(RCX, 7, 29, 1);
            } else { emit_mov(RAX, RAX); emit_mov(RCX, RCX); }
            output_rex(1, 0, 0, 0); output_byte(0x0f); output_byte(0xaf); output_modrm(3, RCX, RAX);
            emit_mov64(RAX, RDX); emit_shift_reg(RDX, 5, 32, 1);
        } else {
            output_rex(1, 0, 0, 0); output_byte(0xf7); output_modrm(3, RCX, sign ? 5 : 4);
        }
    } else {
        if (word) {
            if (sign) emit_sign_extend_word();
            else { emit_mov(RAX, RAX); emit_mov(RCX, RCX); }
        }
        emit_reg_op(0x85, RCX, RCX, 1); unsigned char* zero = emit_jump(4);
        unsigned char* overflow = NULL;
        if (op == R4300_OP_DDIV) {
            emit_movimm64(UINT64_C(0x8000000000000000), RDX);
            emit_reg_op(0x39, RDX, RAX, 1); unsigned char* ordinary = emit_jump(5);
            emit_imm_op(7, RCX, UINT32_MAX, 1); overflow = emit_jump(4);
            patch_jump(ordinary, out);
        }
        if (sign) { output_rex(1, 0, 0, 0); output_byte(0x99); } /* CQO */
        else emit_reg_op(0x31, RDX, RDX, 0);
        output_rex(1, 0, 0, 0); output_byte(0xf7); output_modrm(3, RCX, sign ? 7 : 6);
        output_byte(0xe9); unsigned char* normal_done = out; output_w32(0);
        unsigned char* overflow_done = NULL;
        if (overflow) {
            patch_jump(overflow, out); emit_movimm(0, RDX);
            output_byte(0xe9); overflow_done = out; output_w32(0);
        }
        patch_jump(zero, out);
        emit_mov64(RAX, RDX);
        if (sign) {
            emit_shift_reg(RAX, 7, 63, 1);
            output_rex(1, 0, 0, 0); output_byte(0xf7); output_modrm(3, RAX, 2);
            emit_imm_op(1, RAX, 1, 1);
        } else emit_movimm64(UINT64_MAX, RAX);
        patch_jump(normal_done, out); if (overflow_done) patch_jump(overflow_done, out);
    }
    if (word) {
        emit_sign_extend_word();
        output_rex(1, 0, 0, 0); output_byte(0x63); output_modrm(3, RDX, RDX);
    }
    emit_state_store(offsetof(struct r4300_core, lo), RAX);
    emit_state_store(offsetof(struct r4300_core, hi), RDX);
}
