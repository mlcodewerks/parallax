/* Native control flow with an allocated, nontrapping integer delay slot.
 * Conditions and indirect destinations are captured before the slot writes
 * its registers. Likely slots have separate taken/annulled commit paths.
 * Idle, COP1 and unsafe slots keep precise handlers.
 * GPL-2.0-or-later. */
static int native_branch_opcode(enum r4300_opcode op)
{
    switch (op) {
    case R4300_OP_BEQ: case R4300_OP_BNE: case R4300_OP_BLEZ: case R4300_OP_BGTZ:
    case R4300_OP_BLTZ: case R4300_OP_BGEZ:
    case R4300_OP_BEQL: case R4300_OP_BNEL: case R4300_OP_BLEZL: case R4300_OP_BGTZL:
    case R4300_OP_BLTZL: case R4300_OP_BGEZL:
    case R4300_OP_BLTZAL: case R4300_OP_BGEZAL:
    case R4300_OP_BLTZALL: case R4300_OP_BGEZALL:
    case R4300_OP_J: case R4300_OP_JAL: case R4300_OP_JR: case R4300_OP_JALR: return 1;
    default: return 0;
    }
}
static unsigned char* emit_branch_native(uint32_t instruction, uint32_t pc)
{
    enum r4300_opcode op = r4300_get_idec(instruction)->opcode;
    int likely = op == R4300_OP_BEQL || op == R4300_OP_BNEL || op == R4300_OP_BLEZL ||
        op == R4300_OP_BGTZL || op == R4300_OP_BLTZL || op == R4300_OP_BGEZL ||
        op == R4300_OP_BLTZALL || op == R4300_OP_BGEZALL;
    int regimm_link = op == R4300_OP_BLTZAL || op == R4300_OP_BGEZAL ||
        op == R4300_OP_BLTZALL || op == R4300_OP_BGEZALL;
    unsigned char* annul = NULL;
    unsigned rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
    unsigned link = op == R4300_OP_JAL || regimm_link ? 31 : op == R4300_OP_JALR ? (instruction >> 11) & 31 : 0;
    /* Match the precise handlers when r31 is also the condition source.
     * BGEZAL samples before linking; the other REGIMM link forms sample
     * after linking. Likely branches write the link even when annulled. */
    if (regimm_link && op != R4300_OP_BGEZAL) {
        emit_movimm64((int64_t)(int32_t)(pc + 8), RAX); emit_state_store(gpr_offset(31), RAX);
        link = 0;
    }
    if (op == R4300_OP_J || op == R4300_OP_JAL)
        emit_movimm(((pc + 4) & UINT32_C(0xf0000000)) | ((instruction & UINT32_C(0x03ffffff)) << 2), R14);
    else if (op == R4300_OP_JR || op == R4300_OP_JALR) emit_state_load(gpr_offset(rs), R14);
    else {
        unsigned condition;
        emit_state_load(gpr_offset(rs), RAX);
        if (op == R4300_OP_BEQ || op == R4300_OP_BNE || op == R4300_OP_BEQL || op == R4300_OP_BNEL) {
            emit_state_load(gpr_offset(rt), RCX); emit_reg_op(0x39, RCX, RAX, 1);
            condition = op == R4300_OP_BEQ || op == R4300_OP_BEQL ? 4 : 5;
        } else {
            emit_reg_op(0x85, RAX, RAX, 1);
            condition = op == R4300_OP_BLEZ || op == R4300_OP_BLEZL ? 14 :
                op == R4300_OP_BGTZ || op == R4300_OP_BGTZL ? 15 :
                op == R4300_OP_BLTZ || op == R4300_OP_BLTZL ||
                op == R4300_OP_BLTZAL || op == R4300_OP_BLTZALL ? 12 : 13;
        }
        emit_movimm(pc + 8, R14);
        unsigned char* not_taken = emit_jump(condition ^ 1);
        emit_movimm(pc + 4 + (int32_t)(int16_t)instruction * 4, R14);
        if (likely) annul = not_taken;
        else patch_jump(not_taken, out);
    }
    if (link) {
        emit_movimm64((int64_t)(int32_t)(pc + 8), RAX); emit_state_store(gpr_offset(link), RAX);
    }
    return annul;
}
