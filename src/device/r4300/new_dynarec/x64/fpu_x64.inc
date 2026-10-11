/* Scalar SSE2 emission with shared VR4300 validation and result exceptions.
 * Host MXCSR is saved/restored around every arithmetic operation; frontend
 * rounding, DAZ/FTZ and sticky flags cannot leak into guest arithmetic.
 * All values cross helpers through an integer ABI. GPL-2.0-or-later. */
static int native_fpu_instruction(uint32_t instruction)
{
    unsigned fmt = (instruction >> 21) & 31, fn = instruction & 63;
    if ((instruction >> 26) != 17) return 0;
    if (fmt == 16 || fmt == 17) return fn <= 15 || fn >= 48 || fn == 36 || fn == 37 || (fmt == 16 ? fn == 33 : fn == 32);
    return (fmt == 20 || fmt == 21) && (fn == 32 || fn == 33);
}
static int native_fpu_transfer(enum r4300_opcode op)
{
    return op == R4300_OP_MFC1 || op == R4300_OP_DMFC1 || op == R4300_OP_MTC1 || op == R4300_OP_DMTC1 || op == R4300_OP_CFC1;
}
static void emit_fpu_transfer(uint32_t instruction, enum r4300_opcode op)
{
    unsigned rt = (instruction >> 16) & 31, fs = (instruction >> 11) & 31;
    int wide = op == R4300_OP_DMFC1 || op == R4300_OP_DMTC1;
    if (op == R4300_OP_CFC1) {
        if (fs == 31 || fs == 0) emit_mem(0x63,1,RAX,R12,-1,0,fs == 31 ? offsetof(struct r4300_core,cp1.fcr31) : offsetof(struct r4300_core,cp1.fcr0));
        else emit_movimm(0,RAX);
        if (rt) emit_state_store_raw(gpr_offset(rt),RAX);
        return;
    }
    emit_state_load_raw((wide ? offsetof(struct r4300_core,cp1.regs_double) : offsetof(struct r4300_core,cp1.regs_simple)) + fs * sizeof(void*),RCX);
    if (op == R4300_OP_MTC1 || op == R4300_OP_DMTC1) {
        emit_state_load_raw(gpr_offset(rt),RAX); emit_mem(0x89,wide,RAX,RCX,-1,0,0);
    } else {
        emit_mem(wide ? 0x8b : 0x63,1,RAX,RCX,-1,0,0);
        if (rt) emit_state_store_raw(gpr_offset(rt),RAX);
    }
}
static void emit_mxcsr(unsigned ext, size_t offset)
{
    output_byte(0x0f); emit_mem(0xae, 0, ext, RSP, -1, 0, offset);
}
static void emit_xmm_bits(int integer, unsigned xmm, int load)
{
    output_byte(0x66); output_rex(1, xmm >> 3, 0, integer >> 3);
    output_byte(0x0f); output_byte(load ? 0x6e : 0x7e); output_modrm(3, integer & 7, xmm & 7);
}
static void emit_set_condition(int reg, unsigned condition)
{
    if (reg >= 4) output_rex(0,0,0,reg >> 3);
    output_byte(0x0f); output_byte(0x90 | condition); output_modrm(3,reg & 7,0);
    if (reg >= 4) output_rex(0,reg >> 3,0,reg >> 3);
    output_byte(0x0f); output_byte(0xb6); output_modrm(3,reg & 7,reg & 7);
}
/* The shared prepare has validated inputs and charged fetch/issue hazards.
 * Commit ordinary results here; exceptional outputs, enabled exceptions and
 * event boundaries retain the shared finish without repeating arithmetic. */
static unsigned char* emit_fpu_fast_finish(uint32_t instruction, unsigned index,
    const struct ari64_x64_config* config, unsigned char** exits, unsigned* exit_count)
{
    unsigned fn = instruction & 63, fmt = (instruction >> 21) & 31;
    int integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    int single = fn == 32 || (fmt == 16 && fn != 33);
    unsigned char* slow[5]; unsigned slow_count = 0;
    if (fn < 48 && fn != 6) {
        emit_mov(R9,RAX); emit_imm_op(4,RAX,~UINT32_C(1),0);
        emit_reg_op(0x85,RAX,RAX,0); slow[slow_count++] = emit_jump(5);
        emit_mem(0xf7,0,0,R12,-1,0,offsetof(struct r4300_core,cp1.fcr31)); output_w32(0x80);
        slow[slow_count++] = emit_jump(5);
        if (!integer) {
            emit_mov64(R8,RAX);
            if (single) emit_imm_op(4,RAX,0x7f800000,0);
            else { emit_shift_reg(RAX,5,52,1); emit_imm_op(4,RAX,2047,0); }
            emit_imm_op(7,RAX,single ? 0x7f800000 : 2047,0); slow[slow_count++] = emit_jump(4);
            emit_reg_op(0x85,RAX,RAX,0); unsigned char* normal = emit_jump(5);
            emit_mov64(R8,RAX); emit_shift_reg(RAX,4,single ? 41 : 12,1);
            emit_reg_op(0x85,RAX,RAX,1); slow[slow_count++] = emit_jump(5);
            patch_jump(normal,out);
        }
    }
    unsigned latency = fn >= 48 || fn == 6 ? 0 : r4300_base_cycles(instruction,CP0_STATUS_CU1) - 1;
    emit_mem(0x8b,0,RAX,R12,-1,0,offsetof(struct r4300_core,cp0.count_phase));
    emit_addimm(RAX,latency,RAX); emit_shift_reg(RAX,5,1,0);
    emit_mem(0x8b,1,RDX,R12,-1,0,offsetof(struct r4300_core,cp0.cycle_count));
    emit_reg_op(0x01,RAX,RDX,1); emit_reg_op(0x85,RDX,RDX,1); slow[slow_count++] = emit_jump(9);
    if (fn >= 48) {
        emit_mem(0x81,0,4,R12,-1,0,offsetof(struct r4300_core,cp1.fcr31)); output_w32(~UINT32_C(0x800000));
        emit_mov(R8,RAX); emit_shift_reg(RAX,4,23,0);
        emit_mem(0x09,0,RAX,R12,-1,0,offsetof(struct r4300_core,cp1.fcr31));
    } else {
        if (fn != 6) {
            emit_mov(R9,RAX); emit_shift_reg(RAX,4,12,0);
            emit_mov(R9,RDX); emit_shift_reg(RDX,4,2,0); emit_reg_op(0x09,RDX,RAX,0);
            emit_mem(0x09,0,RAX,R12,-1,0,offsetof(struct r4300_core,cp1.fcr31));
        }
        emit_mem(0x89,1,R8,R12,-1,0,offsetof(struct r4300_core,cp1.regs) + ((instruction >> 6) & 31) * sizeof(cp1_reg));
    }
    emit_movimm(latency,R13);
    emit_mem(0xc7,0,0,RSP,-1,0,8); output_w32(0);
    emit_region_commit_native(config,index,1,0);
    emit_movimm64((intptr_t)&breakloop,R11);
    emit_mem(0x80,0,7,R11,-1,0,0); output_byte(0); exits[(*exit_count)++] = emit_jump(5);
    output_byte(0xe9); unsigned char* done = out; output_w32(0);
    for (unsigned n = 0; n < slow_count; ++n) patch_jump(slow[n],out);
    return done;
}
static void emit_fpu_native(uint32_t instruction, unsigned index, const struct ari64_x64_config* config,
    unsigned char** exits, unsigned* exit_count)
{
    unsigned fmt = (instruction >> 21) & 31, fn = instruction & 63;
    int integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    int out_single = fn == 32 || (fmt == 16 && fn != 33);
    emit_state_imm(offsetof(struct r4300_core, interp_PC.addr), config->virtual_pc + index * 4, 0);
    emit_mov64(R12, ARG1_REG); emit_movimm(instruction, ARG2_REG);
    emit_mem(0x8d, 1, ARG3_REG, RSP, -1, 0, 32);
    emit_movimm64((intptr_t)config->commits,ARG4_REG);
    emit_helper_call((const void*)config->fpu_prepare);
    emit_reg_op(0x85, RAX, RAX, 0); exits[(*exit_count)++] = emit_jump(4);
    if (fn >= 48) {
        emit_mem(0x81,0,7,RSP,-1,0,48); output_w32(0); unsigned char* unordered = emit_jump(5);
        emit_mxcsr(3,16);
        emit_mem(0xc7,0,0,RSP,-1,0,20); output_w32(0x1f80); emit_mxcsr(2,20);
        emit_mem(0x8b,1,RAX,RSP,-1,0,32); emit_mem(0x8b,1,RCX,RSP,-1,0,40);
        emit_xmm_bits(RAX,0,1); emit_xmm_bits(RCX,1,1);
        emit_movimm(0,R8); emit_movimm(0,R9);
        if (fmt == 17) output_byte(0x66);
        output_byte(0x0f); output_byte(0x2e); output_modrm(3,1,0); /* UCOMISS/SD */
        if (fn & 2) emit_set_condition(R8,4);
        if (fn & 4) emit_set_condition(R9,2);
        emit_reg_op(0x09,R9,R8,0); emit_movimm(0,R9); emit_mxcsr(2,16);
        output_byte(0xe9); unsigned char* done = out; output_w32(0);
        patch_jump(unordered,out); emit_movimm(fn & 1,R8); emit_movimm(0,R9); patch_jump(done,out);
    } else if (fn >= 5 && fn <= 7) {
        emit_mem(0x8b, 1, R8, RSP, -1, 0, 32); emit_movimm(0, R9);
        if (fn != 6) {
            if (fmt == 16) emit_mov(R8, R8);
            emit_movimm64(fmt == 16 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000), RAX);
            if (fn == 5) { output_rex(1,0,0,0); output_byte(0xf7); output_modrm(3,RAX,2); }
            emit_reg_op(fn == 5 ? 0x21 : 0x31, RAX, R8, 1);
        }
    } else {
        emit_mxcsr(3, 16); /* STMXCSR */
        emit_mem(0x8b, 0, RAX, RSP, -1, 0, 48);
        emit_movimm(4, RDX); emit_reg_op(0x29, RAX, RDX, 0);
        emit_reg_op(0x85, RAX, RAX, 0); unsigned char* nonzero = emit_jump(5);
        emit_movimm(0, RDX); patch_jump(nonzero, out);
        emit_shift_reg(RDX, 4, 13, 0); emit_imm_op(1, RDX, 0x1f80, 0);
        emit_mem(0x89, 0, RDX, RSP, -1, 0, 20); emit_mxcsr(2, 20); /* LDMXCSR */
        emit_mem(0x8b, 1, RAX, RSP, -1, 0, 32);
        emit_mem(0x8b, 1, RCX, RSP, -1, 0, 40);
        emit_xmm_bits(RAX, 0, 1); emit_xmm_bits(RCX, 1, 1);
        if (integer) {
            output_byte(fmt == 16 ? 0xf3 : 0xf2); output_rex(1,1,0,0);
            output_byte(0x0f); output_byte(0x2d); output_modrm(3,0,R8 & 7);
        } else if (fn == 32 || fn == 33) {
            if (fmt == 16 || fmt == 17) {
                output_byte(fmt == 16 ? 0xf3 : 0xf2); output_byte(0x0f); output_byte(0x5a); output_modrm(3,0,0);
            } else {
                output_byte(fn == 32 ? 0xf3 : 0xf2); if (fmt == 21) output_rex(1,0,0,0);
                output_byte(0x0f); output_byte(0x2a); output_modrm(3,RAX,0);
            }
            emit_xmm_bits(R8, 0, 0); if (out_single) emit_mov(R8, R8);
        } else {
            const unsigned opcodes[] = {0x58,0x5c,0x59,0x5e,0x51};
            output_byte(fmt == 16 ? 0xf3 : 0xf2); output_byte(0x0f); output_byte(opcodes[fn]); output_modrm(3,fn == 4 ? 0 : 1,0);
            emit_xmm_bits(R8, 0, 0); if (out_single) emit_mov(R8, R8);
        }
        emit_mxcsr(3, 20); emit_mxcsr(2, 16);
        emit_mem(0x8b, 0, RAX, RSP, -1, 0, 20);
        emit_mov(RAX, R9); emit_shift_reg(R9,5,5,0); emit_imm_op(4,R9,1,0);
        const unsigned shifts[] = {3,1,1,4}, masks[] = {2,4,8,16};
        for (unsigned n = 0; n < 4; ++n) {
            emit_mov(RAX,RDX); emit_shift_reg(RDX,n < 2 ? 5 : 4,shifts[n],0);
            emit_imm_op(4,RDX,masks[n],0); emit_reg_op(0x09,RDX,R9,0);
        }
        if (integer && ((fn >= 12 && fn <= 15) || fn == 36)) {
            emit_imm_op(7,R8,INT32_MAX,1); unsigned char* too_high = emit_jump(15);
            emit_imm_op(7,R8,(uint32_t)INT32_MIN,1); unsigned char* too_low = emit_jump(12);
            output_byte(0xe9); unsigned char* word_ok = out; output_w32(0);
            patch_jump(too_high,out); patch_jump(too_low,out); emit_movimm(32,R9);
            patch_jump(word_ok,out); emit_mov(R8,R8);
        }
    }
    unsigned char* fast_done = emit_fpu_fast_finish(instruction,index,config,exits,exit_count);
    emit_mov64(R8,ARG3_REG); emit_mov(R9,ARG4_REG);
    emit_mov64(R12,ARG1_REG); emit_movimm(instruction,ARG2_REG);
    emit_helper_call((const void*)config->fpu_finish);
    emit_reg_op(0x85,RAX,RAX,0); exits[(*exit_count)++] = emit_jump(4);
    patch_jump(fast_done,out);
}
