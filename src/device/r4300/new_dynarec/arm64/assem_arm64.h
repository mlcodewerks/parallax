/* A64 output primitives for Ari64's host-independent emission operations.
 * GPL-2.0-or-later. x13-x17 are reserved assembler temporaries. */
enum { T0=9, T1=10, T2=11, ADDRESS=12, STATE=19, CLOCKS=20, TARGET=21,
       CACHE3=22, CACHE0=23, CACHE1=24, CACHE2=25, CACHE4=26, CACHE5=27, CACHE6=28, SP=31,
       ARG1_REG=0, ARG2_REG=1, ARG3_REG=2, ARG4_REG=3, MAX_INSTRUCTIONS=32 };
static unsigned char* out;
static uint64_t compile_absolute_calls;
static void a64(uint32_t word) { memcpy(out,&word,4); out+=4; }
static void emit_movimm64(int64_t value, unsigned reg)
{
    uint64_t v=(uint64_t)value;
    a64(0xd2800000 | ((v&65535)<<5) | reg);
    for (unsigned i=1;i<4;++i) if ((v>>(16*i))&65535)
        a64(0xf2800000 | (i<<21) | (((v>>(16*i))&65535)<<5) | reg);
}
static void emit_movimm(uint32_t value, unsigned reg) { emit_movimm64(value,reg); }
static void emit_mov64(int rs,int rt)
{
    if (rs==rt) return;
    a64(rs==SP || rt==SP ? 0x91000000 | (rs<<5) | rt : 0xaa0003e0 | (rs<<16) | rt);
}
static void emit_mov(int rs,int rt) { a64(0x2a0003e0 | (rs<<16) | rt); }
/* Conditions use the frontend's existing abstract ordering:
 * overflow, !overflow, unsigned<, unsigned>=, ==, !=, unsigned<=, unsigned>,
 * negative, positive, reserved, reserved, signed<, signed>=, signed<=, signed>.
 * The native frontend never requests the reserved parity slots. */
static unsigned char* emit_jump(unsigned condition)
{
    static const unsigned conditions[16]={6,7,3,2,0,1,9,8,4,5,6,7,11,10,13,12};
    unsigned char* site=out; a64(0x54000000|conditions[condition]); return site;
}
static unsigned char* emit_b(void) { unsigned char* site=out; a64(0x14000000); return site; }
static void emit_set_condition(int reg,unsigned condition)
{
    static const unsigned conditions[16]={6,7,3,2,0,1,9,8,4,5,6,7,11,10,13,12};
    a64(0x9a9f07e0 | ((conditions[condition]^1)<<12) | reg);
}
static void patch_jump(unsigned char* site,unsigned char* target)
{
    int64_t delta=(target-site)/4;
    uint32_t word; memcpy(&word,site,4);
    if ((word&0x7c000000)==0x14000000) {
        assert(delta>=-(1<<25) && delta<(1<<25)); word=(word&0xfc000000)|((uint32_t)delta&0x3ffffff);
    } else {
        assert(delta>=-(1<<18) && delta<(1<<18)); word=(word&~0x00ffffe0)|(((uint32_t)delta&0x7ffff)<<5);
    }
    memcpy(site,&word,4);
}
static void emit_reg_op(unsigned opcode,int source,int dest,int wide)
{
    uint32_t sf=wide?0x80000000:0, word;
    switch(opcode) {
    case 0x01: word=0x2b000000; break; /* ADD flags */
    case 0x29: word=0x6b000000; break;
    case 0x39: a64(sf|0x6b00001f|(source<<16)|(dest<<5)); return;
    case 0x85: a64(sf|0x6a00001f|(source<<16)|(dest<<5)); return;
    case 0x21: word=0x0a000000; break;
    case 0x09: word=0x2a000000; break;
    case 0x31: word=0x4a000000; break;
    default: abort();
    }
    a64(sf|word|(source<<16)|(dest<<5)|dest);
    if(opcode==0x21 || opcode==0x09 || opcode==0x31) a64(sf|0x6a00001f|(dest<<16)|(dest<<5));
}
static void emit_imm_op(unsigned ext,int reg,uint32_t value,int wide)
{
    emit_movimm64(wide?(int64_t)(int32_t)value:(int64_t)value,15);
    static const unsigned ops[8]={0x01,0x09,0,0,0x21,0x29,0x31,0x39};
    assert(ops[ext]); emit_reg_op(ops[ext],15,reg,wide);
}
static void emit_addimm64(int rs,int value,int rt)
{
    emit_movimm64(value,15);
    a64((rs==rt?0xab000000:0x8b000000)|(15<<16)|(rs<<5)|rt);
}
static void emit_addimm(int rs,int value,int rt)
{
    emit_movimm(value,15);
    a64((rs==rt?0x2b000000:0x0b000000)|(15<<16)|(rs<<5)|rt);
}
static void emit_shift_reg(int reg,unsigned kind,unsigned amount,int wide)
{
    if (!amount) return;
    unsigned bits=wide?64:32; uint32_t sf=wide?0x80000000:0;
    if(kind==4) a64(sf|0x53000000|((wide?1u:0)<<22)|(((bits-amount)%bits)<<16)|((bits-1-amount)<<10)|(reg<<5)|reg);
    else a64(sf|(kind==7?0x13000000:0x53000000)|((wide?1u:0)<<22)|(amount<<16)|((bits-1)<<10)|(reg<<5)|reg);
}
static void emit_address(int base,int index,unsigned scale,size_t offset)
{
    emit_mov64(base,13);
    if(index>=0) a64(0x8b000000|(index<<16)|(scale<<10)|(13<<5)|13);
    if(offset) { emit_movimm64((int64_t)offset,14); a64(0x8b0e01ad); }
}
static void emit_load_at(int reg,int wide,unsigned bytes,int sign)
{
    uint32_t code=bytes==1?(sign?0x39800000:0x39400000):bytes==2?(sign?0x79800000:0x79400000):sign?0xb9800000:wide?0xf9400000:0xb9400000;
    a64(code|(13<<5)|reg);
}
static void emit_store_at(int reg,int wide,unsigned bytes)
{
    a64((bytes==1?0x39000000:bytes==2?0x79000000:wide?0xf9000000:0xb9000000)|(13<<5)|reg);
}
static void emit_mem(unsigned opcode,int wide,int reg,int base,int index,unsigned scale,size_t offset)
{
    emit_address(base,index,scale,offset);
    switch(opcode) {
    case 0x8b: emit_load_at(reg,wide,wide?8:4,0); break;
    case 0x63: emit_load_at(reg,1,4,1); break;
    case 0x89: emit_store_at(reg,wide,wide?8:4); break;
    case 0x88: emit_store_at(reg,0,1); break;
    case 0x8d: emit_mov64(13,reg); break;
    case 0x39: case 0x3b: case 0x38:
        emit_load_at(14,wide,opcode==0x38?1:wide?8:4,0);
        emit_reg_op(0x39,opcode==0x3b?14:reg,opcode==0x3b?reg:14,wide); break;
    case 0x01: case 0x09:
        emit_load_at(14,wide,wide?8:4,0); emit_reg_op(opcode,reg,14,wide); emit_store_at(14,wide,wide?8:4); break;
    case 0xff: emit_load_at(16,1,8,0); a64(0xd61f0200); break;
    default: abort();
    }
}
static void emit_mem_imm(unsigned opcode,int wide,int ext,int base,int index,unsigned scale,size_t offset,uint32_t value)
{
    emit_address(base,index,scale,offset);
    if(opcode==0xc7) { emit_movimm64(wide?(int64_t)(int32_t)value:value,14); emit_store_at(14,wide,wide?8:4); return; }
    emit_load_at(14,wide,opcode==0x80?1:wide?8:4,0);
    if(opcode==0xf7) { emit_movimm64(wide?(int64_t)(int32_t)value:value,15); emit_reg_op(0x85,15,14,wide); return; }
    emit_imm_op(ext,14,value,wide);
    if(ext!=7) emit_store_at(14,wide,opcode==0x80?1:wide?8:4);
}
static void emit_state_imm(size_t offset,uint32_t value,int wide) { emit_mem_imm(0xc7,wide,0,STATE,-1,0,offset,value); }
static size_t gpr_offset(unsigned reg) { return offsetof(struct r4300_core,regs)+reg*8; }
static void emit_state_load_raw(size_t offset,int reg) { emit_mem(0x8b,1,reg,STATE,-1,0,offset); }
static void emit_state_store_raw(size_t offset,int reg) { emit_mem(0x89,1,reg,STATE,-1,0,offset); }
static void emit_sign_extend_word(void) { a64(0x93407c00|(T0<<5)|T0); }
static void emit_helper_call(const void* function)
{
    emit_movimm64((intptr_t)function,16); a64(0xd63f0200); emit_mov64(0,T0); ++compile_absolute_calls;
}
static void emit_bit_test(int base,int index)
{
    emit_mov64(index,15); emit_shift_reg(15,5,6,1);
    emit_address(base,15,3,0); emit_load_at(14,1,8,0);
    a64(0x9ac02400 | (index<<16) | (14<<5) | 14);
    emit_imm_op(4,14,1,1);
    a64(0xeb0003ff | (14<<16)); /* CMP XZR,X14: carry clear iff bit set */
}
static void emit_frame_enter(void)
{
    a64(0xd503245f); /* BTI c */
    a64(0xd10283ff); /* SUB SP,SP,#160: scratch 0..63, saved registers 64..159 */
    a64(0xa9097bfd); /* STP X29,X30,[SP,#144] */
    a64(0x910243fd); /* ADD X29,SP,#144 */
    for(unsigned i=0;i<5;++i) a64(0xa9000000|((8+2*i)<<15)|((20+2*i)<<10)|(31<<5)|(19+2*i));
}
static void emit_frame_leave(void)
{
    for(unsigned i=0;i<5;++i) a64(0xa9400000|((8+2*i)<<15)|((20+2*i)<<10)|(31<<5)|(19+2*i));
    a64(0xa9497bfd); a64(0x910283ff);
}
