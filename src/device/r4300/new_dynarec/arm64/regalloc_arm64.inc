/* Ari64-style next-use allocation, dirty writeback and constant propagation.
 * The original allocator's principles are applied to full-width guest values
 * and this core's ABI. Guarded regions have no traps or observable events;
 * their precise paths commit each instruction without allocation shortcuts.
 * GPL-2.0-or-later; see the upstream notice in ari64_arm64.c. */
enum { GUEST_HI=32, GUEST_LO=33, GUEST_REGS=34, CACHE_REGS=7 };
static const int cache_host[CACHE_REGS]={CACHE0,CACHE1,CACHE2,CACHE3,CACHE4,CACHE5,CACHE6};
struct regcache_state {
    int active, map[CACHE_REGS], allow_dead_store;
    unsigned dirty, index, end;
    const uint32_t* ops;
    uint64_t constants[GUEST_REGS];
    uint64_t known, constant_dirty;
};
static struct regcache_state regcache;

static size_t guest_offset(unsigned reg)
{
    return reg < 32 ? gpr_offset(reg) : reg == GUEST_HI ?
        offsetof(struct r4300_core, hi) : offsetof(struct r4300_core, lo);
}
static int offset_guest(size_t offset)
{
    if (offset < gpr_offset(32) && offset % 8 == 0) return (int)(offset / 8);
    if (offset == offsetof(struct r4300_core, hi)) return GUEST_HI;
    if (offset == offsetof(struct r4300_core, lo)) return GUEST_LO;
    return -1;
}
static void guest_operands(uint32_t instruction, unsigned* source1, unsigned* source2, unsigned* dest)
{
    enum r4300_opcode op = r4300_get_idec(instruction)->opcode;
    unsigned rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
    *source1 = rs; *source2 = rt; *dest = (instruction >> 11) & 31;
    switch (op) {
    case R4300_OP_NOP: case R4300_OP_SYNC: *source1 = *source2 = *dest = 0; break;
    case R4300_OP_LUI: *source1 = *source2 = 0; *dest = rt; break;
    case R4300_OP_ADDIU: case R4300_OP_DADDIU: case R4300_OP_ANDI:
    case R4300_OP_ORI: case R4300_OP_XORI: case R4300_OP_SLTI: case R4300_OP_SLTIU:
        *source2 = 0; *dest = rt; break;
    case R4300_OP_MFHI: case R4300_OP_MFLO:
        *source1 = op == R4300_OP_MFHI ? GUEST_HI : GUEST_LO; *source2 = 0; break;
    case R4300_OP_MTHI: case R4300_OP_MTLO:
        *source2 = 0; *dest = op == R4300_OP_MTHI ? GUEST_HI : GUEST_LO; break;
    case R4300_OP_MULT: case R4300_OP_MULTU: case R4300_OP_DMULT: case R4300_OP_DMULTU:
    case R4300_OP_DIV: case R4300_OP_DIVU: case R4300_OP_DDIV: case R4300_OP_DDIVU:
        *dest = GUEST_HI; break; /* LO remains conservatively live until written. */
    case R4300_OP_SLL: case R4300_OP_SRL: case R4300_OP_SRA:
    case R4300_OP_DSLL: case R4300_OP_DSLL32: case R4300_OP_DSRL:
    case R4300_OP_DSRL32: case R4300_OP_DSRA: case R4300_OP_DSRA32:
        *source1 = rt; *source2 = 0; break;
    case R4300_OP_LB: case R4300_OP_LBU: case R4300_OP_LH: case R4300_OP_LHU:
    case R4300_OP_LW: case R4300_OP_LWU: case R4300_OP_LD:
        *source2 = 0; *dest = rt; break;
    case R4300_OP_SB: case R4300_OP_SH: case R4300_OP_SW: case R4300_OP_SD:
        *dest = 0; break;
    case R4300_OP_LWC1: case R4300_OP_LDC1: case R4300_OP_SWC1: case R4300_OP_SDC1:
        *source2 = *dest = 0; break;
    case R4300_OP_BEQ: case R4300_OP_BNE: case R4300_OP_BEQL: case R4300_OP_BNEL: *dest = 0; break;
    case R4300_OP_BLEZ: case R4300_OP_BGTZ: case R4300_OP_BLTZ: case R4300_OP_BGEZ:
    case R4300_OP_BLEZL: case R4300_OP_BGTZL: case R4300_OP_BLTZL: case R4300_OP_BGEZL:
        *source2 = *dest = 0; break;
    case R4300_OP_BLTZAL: case R4300_OP_BGEZAL: case R4300_OP_BLTZALL: case R4300_OP_BGEZALL:
        *source2 = 0; *dest = 31; break;
    case R4300_OP_J: case R4300_OP_JAL:
        *source1 = *source2 = 0; *dest = op == R4300_OP_JAL ? 31 : 0; break;
    case R4300_OP_JR: *source2 = *dest = 0; break;
    case R4300_OP_JALR: *source2 = 0; break;
    default: break; /* Other ALU instructions use the default rs/rt/rd fields. */
    }
}
/* Distance to the next read, stopping at an overwrite. Values overwritten
 * without a read are dead; untouched values remain live at the region exit. */
static unsigned next_use(unsigned reg, int* dead)
{
    *dead = 0;
    for (unsigned i = regcache.index; i < regcache.end; ++i) {
        unsigned a,b,d; guest_operands(regcache.ops[i], &a,&b,&d);
        if (a == reg || b == reg) return i - regcache.index;
        if (d == reg) { *dead = 1; return MAX_INSTRUCTIONS + 2; }
    }
    return MAX_INSTRUCTIONS + 1;
}
static int cache_find(unsigned reg)
{
    for (unsigned h = 0; h < CACHE_REGS; ++h) if (regcache.map[h] == (int)reg) return h;
    return -1;
}
static int cache_allocate(unsigned reg)
{
    int h = cache_find(reg);
    if (h >= 0) return h;
    unsigned distance = 0, victim = 0;
    for (unsigned j = 0; j < CACHE_REGS; ++j) {
        if (regcache.map[j] < 0) { victim = j; break; }
        int dead;
        unsigned use = next_use(regcache.map[j], &dead);
        if (use > distance) { distance = use; victim = j; }
    }
    if (regcache.map[victim] >= 0 && (regcache.dirty & (1u << victim))) {
        int dead; next_use(regcache.map[victim], &dead);
        if (!dead || !regcache.allow_dead_store) emit_state_store_raw(guest_offset(regcache.map[victim]), cache_host[victim]);
    }
    regcache.map[victim] = reg;
    regcache.dirty &= ~(1u << victim);
    return victim;
}
static void emit_state_load(size_t offset, int reg)
{
    int guest = offset_guest(offset);
    if (!regcache.active || guest < 0) { emit_state_load_raw(offset, reg); return; }
    if (regcache.known & (UINT64_C(1) << guest)) {
        emit_movimm64(regcache.constants[guest], reg);
        return;
    }
    int h = cache_find(guest);
    if (h < 0) { h = cache_allocate(guest); emit_state_load_raw(offset, cache_host[h]); }
    emit_mov64(cache_host[h], reg);
}
static void emit_state_store(size_t offset, int reg)
{
    int guest = offset_guest(offset);
    if (!regcache.active || guest < 0) { emit_state_store_raw(offset, reg); return; }
    regcache.known &= ~(UINT64_C(1) << guest);
    regcache.constant_dirty &= ~(UINT64_C(1) << guest);
    int h = cache_allocate(guest);
    emit_mov64(reg, cache_host[h]);
    regcache.dirty |= 1u << h;
}
static uint64_t word_signed(uint64_t value) { return (uint64_t)(int64_t)(int32_t)(uint32_t)value; }
static int regcache_fold(uint32_t instruction)
{
    unsigned a,b,d; guest_operands(instruction, &a,&b,&d);
    enum r4300_opcode op = r4300_get_idec(instruction)->opcode;
    if (!d) return 1; /* Nontrapping instructions writing r0 have no data effect. */
    if (!(regcache.known & (UINT64_C(1) << a)) || !(regcache.known & (UINT64_C(1) << b))) return 0;
    uint64_t x = regcache.constants[a], y = regcache.constants[b], value;
    int64_t imm = (int16_t)instruction;
    unsigned shift = (instruction >> 6) & 31;
    switch (op) {
    case R4300_OP_ADDIU: value = word_signed(x + imm); break;
    case R4300_OP_DADDIU: value = x + imm; break;
    case R4300_OP_ADDU: value = word_signed(x + y); break;
    case R4300_OP_DADDU: value = x + y; break;
    case R4300_OP_SUBU: value = word_signed(x - y); break;
    case R4300_OP_DSUBU: value = x - y; break;
    case R4300_OP_AND: value = x & y; break;
    case R4300_OP_OR: value = x | y; break;
    case R4300_OP_XOR: value = x ^ y; break;
    case R4300_OP_NOR: value = ~(x | y); break;
    case R4300_OP_ANDI: value = x & (uint16_t)instruction; break;
    case R4300_OP_ORI: value = x | (uint16_t)instruction; break;
    case R4300_OP_XORI: value = x ^ (uint16_t)instruction; break;
    case R4300_OP_LUI: value = word_signed((uint32_t)(uint16_t)instruction << 16); break;
    case R4300_OP_SLT: value = (int64_t)x < (int64_t)y; break;
    case R4300_OP_SLTU: value = x < y; break;
    case R4300_OP_SLTI: value = (int64_t)x < imm; break;
    case R4300_OP_SLTIU: value = x < (uint64_t)imm; break;
    case R4300_OP_SLL: value = word_signed((uint32_t)x << shift); break;
    case R4300_OP_SRL: value = word_signed((uint32_t)x >> shift); break;
    case R4300_OP_SRA: value = word_signed((int64_t)x >> shift); break;
    case R4300_OP_SLLV: value = word_signed((uint32_t)y << (x & 31)); break;
    case R4300_OP_SRLV: value = word_signed((uint32_t)y >> (x & 31)); break;
    case R4300_OP_SRAV: value = word_signed((int64_t)y >> (x & 31)); break;
    case R4300_OP_DSLL: value = x << shift; break;
    case R4300_OP_DSLL32: value = x << (shift + 32); break;
    case R4300_OP_DSRL: value = x >> shift; break;
    case R4300_OP_DSRL32: value = x >> (shift + 32); break;
    case R4300_OP_DSRA: value = (int64_t)x >> shift; break;
    case R4300_OP_DSRA32: value = (int64_t)x >> (shift + 32); break;
    case R4300_OP_DSLLV: value = y << (x & 63); break;
    case R4300_OP_DSRLV: value = y >> (x & 63); break;
    case R4300_OP_DSRAV: value = (int64_t)y >> (x & 63); break;
    case R4300_OP_MFHI: case R4300_OP_MFLO: case R4300_OP_MTHI: case R4300_OP_MTLO: value = x; break;
    default: return 0;
    }
    int h = cache_find(d);
    if (h >= 0) { regcache.map[h] = -1; regcache.dirty &= ~(1u << h); }
    regcache.known |= UINT64_C(1) << d;
    regcache.constant_dirty |= UINT64_C(1) << d;
    regcache.constants[d] = value;
    return 1;
}
static void regcache_begin(const uint32_t* ops, unsigned start, unsigned end)
{
    memset(&regcache, 0, sizeof(regcache));
    for (unsigned i = 0; i < CACHE_REGS; ++i) regcache.map[i] = -1;
    regcache.known = 1;
    regcache.allow_dead_store = 1;
    regcache.ops = ops; regcache.index = start; regcache.end = end; regcache.active = 1;
}
static void regcache_flush(void)
{
    for (unsigned h = 0; h < CACHE_REGS; ++h)
        if (regcache.dirty & (1u << h)) emit_state_store_raw(guest_offset(regcache.map[h]), cache_host[h]);
    for (unsigned g = 1; g < GUEST_REGS; ++g) {
        if (regcache.constant_dirty & (UINT64_C(1) << g)) {
            emit_movimm64(regcache.constants[g], T0);
            emit_state_store_raw(guest_offset(g), T0);
        }
    }
    regcache.active = 0;
}
