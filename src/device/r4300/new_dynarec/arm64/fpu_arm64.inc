/* Scalar A64 FP emission with shared VR4300 validation and result exceptions.
 * Host FPCR/FPSR is saved/restored around every arithmetic operation; frontend
 * rounding and flush controls and sticky flags cannot leak into guest arithmetic.
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
        if (fs == 31 || fs == 0) emit_mem(0x63,1,T0,STATE,-1,0,fs == 31 ? offsetof(struct r4300_core,cp1.fcr31) : offsetof(struct r4300_core,cp1.fcr0));
        else emit_movimm(0,T0);
        if (rt) emit_state_store_raw(gpr_offset(rt),T0);
        return;
    }
    emit_state_load_raw((wide ? offsetof(struct r4300_core,cp1.regs_double) : offsetof(struct r4300_core,cp1.regs_simple)) + fs * sizeof(void*),T1);
    if (op == R4300_OP_MTC1 || op == R4300_OP_DMTC1) {
        emit_state_load_raw(gpr_offset(rt),T0); emit_mem(0x89,wide,T0,T1,-1,0,0);
    } else {
        emit_mem(wide ? 0x8b : 0x63,1,T0,T1,-1,0,0);
        if (rt) emit_state_store_raw(gpr_offset(rt),T0);
    }
}
/* The shared prepare has validated inputs and charged fetch/issue hazards.
 * Commit ordinary results here; exceptional outputs, enabled exceptions and
 * event boundaries retain the shared finish without repeating arithmetic. */
static unsigned char* emit_fpu_fast_finish(uint32_t instruction, unsigned index,
    const struct ari64_config* config, unsigned char** exits, unsigned* exit_count)
{
    unsigned fn = instruction & 63, fmt = (instruction >> 21) & 31;
    int integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    int single = fn == 32 || (fmt == 16 && fn != 33);
    unsigned char* slow[5]; unsigned slow_count = 0;
    if (fn < 48 && fn != 6) {
        emit_mov(CACHE1,T0); emit_imm_op(4,T0,~UINT32_C(1),0);
        emit_reg_op(0x85,T0,T0,0); slow[slow_count++] = emit_jump(5);
        emit_mem_imm(0xf7,0,0,STATE,-1,0,offsetof(struct r4300_core,cp1.fcr31),0x80);
        slow[slow_count++] = emit_jump(5);
        if (!integer) {
            emit_mov64(CACHE0,T0);
            if (single) emit_imm_op(4,T0,0x7f800000,0);
            else { emit_shift_reg(T0,5,52,1); emit_imm_op(4,T0,2047,0); }
            emit_imm_op(7,T0,single ? 0x7f800000 : 2047,0); slow[slow_count++] = emit_jump(4);
            emit_reg_op(0x85,T0,T0,0); unsigned char* normal = emit_jump(5);
            emit_mov64(CACHE0,T0); emit_shift_reg(T0,4,single ? 41 : 12,1);
            emit_reg_op(0x85,T0,T0,1); slow[slow_count++] = emit_jump(5);
            patch_jump(normal,out);
        }
    }
    unsigned latency = fn >= 48 || fn == 6 ? 0 : r4300_base_cycles(instruction,CP0_STATUS_CU1) - 1;
    emit_mem(0x8b,0,T0,STATE,-1,0,offsetof(struct r4300_core,cp0.count_phase));
    emit_addimm(T0,latency,T0); emit_shift_reg(T0,5,1,0);
    emit_mem(0x8b,1,T2,STATE,-1,0,offsetof(struct r4300_core,cp0.cycle_count));
    emit_reg_op(0x01,T0,T2,1); emit_reg_op(0x85,T2,T2,1); slow[slow_count++] = emit_jump(9);
    if (fn >= 48) {
        emit_mem_imm(0x81,0,4,STATE,-1,0,offsetof(struct r4300_core,cp1.fcr31),~UINT32_C(0x800000));
        emit_mov(CACHE0,T0); emit_shift_reg(T0,4,23,0);
        emit_mem(0x09,0,T0,STATE,-1,0,offsetof(struct r4300_core,cp1.fcr31));
    } else {
        if (fn != 6) {
            emit_mov(CACHE1,T0); emit_shift_reg(T0,4,12,0);
            emit_mov(CACHE1,T2); emit_shift_reg(T2,4,2,0); emit_reg_op(0x09,T2,T0,0);
            emit_mem(0x09,0,T0,STATE,-1,0,offsetof(struct r4300_core,cp1.fcr31));
        }
        emit_mem(0x89,1,CACHE0,STATE,-1,0,offsetof(struct r4300_core,cp1.regs) + ((instruction >> 6) & 31) * sizeof(cp1_reg));
    }
    emit_movimm(latency,CLOCKS);
    emit_mem_imm(0xc7,0,0,SP,-1,0,8,0);
    emit_region_commit_native(config,index,1,0);
    emit_movimm64((intptr_t)&breakloop,ADDRESS);
    emit_mem_imm(0x80,0,7,ADDRESS,-1,0,0,0); exits[(*exit_count)++] = emit_jump(5);
    unsigned char* done = emit_b();
    for (unsigned n = 0; n < slow_count; ++n) patch_jump(slow[n],out);
    return done;
}
static void emit_fpu_environment_begin(void)
{
    static const uint32_t rounding[4]={0,3u<<22,1u<<22,2u<<22};
    a64(0xd53b4409); emit_mem(0x89,1,T0,SP,-1,0,16); /* FPCR */
    a64(0xd53b442a); emit_mem(0x89,1,T1,SP,-1,0,24); /* FPSR */
    emit_mem(0x8b,0,T0,SP,-1,0,48);
    emit_movimm64((intptr_t)rounding,ADDRESS); emit_mem(0x8b,0,T0,ADDRESS,T0,2,0);
    a64(0xd51b4409); a64(0xd51b443f); /* guest FPCR, clear FPSR */
}
static void emit_fpu_environment_end(void)
{
    a64(0xd53b4409|0x20); /* MRS X9,FPSR */
    emit_mov64(T0,ADDRESS);
    emit_mem(0x8b,1,T0,SP,-1,0,16); a64(0xd51b4409);
    emit_mem(0x8b,1,T1,SP,-1,0,24); a64(0xd51b442a);
    /* FPSR IXC/UFC/OFC/DZC/IOC -> guest I/U/O/Z/V. */
    emit_mov64(ADDRESS,CACHE1); emit_shift_reg(CACHE1,5,4,1); emit_imm_op(4,CACHE1,1,0);
    const unsigned shifts[4]={2,0,2,4}, masks[4]={2,4,8,16};
    for(unsigned n=0;n<4;++n) {
        emit_mov64(ADDRESS,T2); emit_shift_reg(T2,n<2?5:4,shifts[n],1);
        emit_imm_op(4,T2,masks[n],0); emit_reg_op(0x09,T2,CACHE1,0);
    }
}
static void emit_fpu_native(uint32_t instruction,unsigned index,const struct ari64_config* config,
    unsigned char** exits,unsigned* exit_count)
{
    unsigned fmt=(instruction>>21)&31,fn=instruction&63;
    int integer=(fn>=8 && fn<=15)||fn==36||fn==37;
    int single=fn==32 || (fmt==16 && fn!=33);
    emit_state_imm(offsetof(struct r4300_core,interp_PC.addr),config->virtual_pc+index*4,0);
    emit_mov64(STATE,ARG1_REG); emit_movimm(instruction,ARG2_REG);
    emit_mem(0x8d,1,ARG3_REG,SP,-1,0,32); emit_movimm64((intptr_t)config->commits,ARG4_REG);
    emit_helper_call((const void*)config->fpu_prepare);
    emit_reg_op(0x85,T0,T0,0); exits[(*exit_count)++]=emit_jump(4);
    if(fn>=48) {
        emit_mem_imm(0x81,0,7,SP,-1,0,48,0); unsigned char* unordered=emit_jump(5);
        emit_fpu_environment_begin();
        emit_mem(0x8b,1,T0,SP,-1,0,32); emit_mem(0x8b,1,T1,SP,-1,0,40);
        a64(fmt==16?0x1e270120:0x9e670120); a64(fmt==16?0x1e270141:0x9e670141);
        a64(fmt==16?0x1e212000:0x1e612000); /* FCMP */
        emit_movimm(0,CACHE0); emit_movimm(0,CACHE1);
        if(fn&2) emit_set_condition(CACHE0,4);
        if(fn&4) emit_set_condition(CACHE1,2);
        emit_reg_op(0x09,CACHE1,CACHE0,0);
        emit_fpu_environment_end(); emit_movimm(0,CACHE1);
        unsigned char* done=emit_b(); patch_jump(unordered,out);
        emit_movimm(fn&1,CACHE0); emit_movimm(0,CACHE1); patch_jump(done,out);
    } else if(fn>=5 && fn<=7) {
        emit_mem(0x8b,1,CACHE0,SP,-1,0,32); emit_movimm(0,CACHE1);
        if(fn!=6) {
            if(fmt==16) emit_mov(CACHE0,CACHE0);
            emit_movimm64(fmt==16?UINT64_C(0x80000000):UINT64_C(0x8000000000000000),T0);
            if(fn==5) a64(0xaa2003e0|(T0<<16)|T0);
            emit_reg_op(fn==5?0x21:0x31,T0,CACHE0,1);
        }
    } else {
        emit_fpu_environment_begin();
        emit_mem(0x8b,1,T0,SP,-1,0,32); emit_mem(0x8b,1,T1,SP,-1,0,40);
        a64(fmt==16?0x1e270120:0x9e670120); a64(fmt==16?0x1e270141:0x9e670141);
        if(integer) {
            a64(fmt==16?0x1e274000:0x1e674000); /* FRINTX */
            a64(fmt==16?0x9e380017:0x9e780017); /* FCVTZS X23 */
        } else if(fn==32 || fn==33) {
            if(fmt==16 || fmt==17) a64(fmt==16?0x1e22c000:0x1e624000);
            else a64((fn==32?0x1e220000:0x1e620000)|(fmt==21?0x80000000:0)|(T0<<5));
            a64(single?0x1e260017:0x9e660017);
        } else {
            static const uint32_t operations[5]={0x1e212800,0x1e213800,0x1e210800,0x1e211800,0x1e21c000};
            a64(operations[fn]|(fmt==17?0x00400000:0));
            a64(single?0x1e260017:0x9e660017);
        }
        emit_fpu_environment_end();
        if(integer && ((fn>=12 && fn<=15)||fn==36)) {
            emit_imm_op(7,CACHE0,INT32_MAX,1); unsigned char* high=emit_jump(15);
            emit_imm_op(7,CACHE0,(uint32_t)INT32_MIN,1); unsigned char* low=emit_jump(12);
            unsigned char* done=emit_b(); patch_jump(high,out); patch_jump(low,out);
            emit_movimm(32,CACHE1); patch_jump(done,out); emit_mov(CACHE0,CACHE0);
        }
    }
    unsigned char* done=emit_fpu_fast_finish(instruction,index,config,exits,exit_count);
    emit_mov64(CACHE0,ARG3_REG); emit_mov(CACHE1,ARG4_REG);
    emit_mov64(STATE,ARG1_REG); emit_movimm(instruction,ARG2_REG);
    emit_helper_call((const void*)config->fpu_finish);
    emit_reg_op(0x85,T0,T0,0); exits[(*exit_count)++]=emit_jump(4);
    patch_jump(done,out);
}
