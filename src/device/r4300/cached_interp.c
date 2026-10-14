

#include "cached_interp.h"
#include "timing.h"
#include "interp_memory.h"
#include "interp_branch.h"
#include "interp_pair.h"
#include "interp_fpu_move.h"
#include "interp_poll.h"
#include "interp_thread_poll.h"
#ifdef HAVE_ARI64_X64
#include "new_dynarec/ari64_x64.h"
#include "api/callbacks.h"
#include "api/m64p_types.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

#include "device/r4300/idec.h"
#include "device/r4300/pure_interp.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/tlb.h"

#if defined(__GNUC__) || defined(__clang__)
#define CI_LIKELY(x) __builtin_expect(!!(x), 1)
#define CI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CI_FORCE_INLINE inline __attribute__((always_inline))
#define CI_COLD_NOINLINE __attribute__((cold, noinline))
#elif defined(_MSC_VER)
#define CI_LIKELY(x) (x)
#define CI_UNLIKELY(x) (x)
#define CI_FORCE_INLINE __forceinline
#define CI_COLD_NOINLINE __declspec(noinline)
#else
#define CI_LIKELY(x) (x)
#define CI_UNLIKELY(x) (x)
#define CI_FORCE_INLINE inline
#define CI_COLD_NOINLINE
#endif


#ifndef CI_CACHED_SET_COUNT
#define CI_CACHED_SET_COUNT 32768
#endif

enum
{
    CI_PAGE_SHIFT = 12,
    CI_PAGE_SIZE = 1 << CI_PAGE_SHIFT,
    CI_PHYS_SIZE = 0x20000000,
    CI_PHYS_PAGE_COUNT = CI_PHYS_SIZE >> CI_PAGE_SHIFT,
    CI_BLOCK_INSTRUCTIONS = 32,
    /* Rare intros exceed 16,384 blocks; retain 65,536 decoded blocks. */
    CI_SET_COUNT = CI_CACHED_SET_COUNT,
    CI_WAYS = 2,
    CI_RECENT_COUNT = CI_SET_COUNT
};

enum
{
    CI_FLAG_MAY_INVALIDATE = 1u << 0,
    CI_FLAG_RAM_MEMORY = 1u << 1,
    CI_META_FAST_SHIFT = 2
};


enum ci_fast_op
{
    CI_FAST_NONE = 0,
    CI_FAST_NOP,
    CI_FAST_ADDIU,
    CI_FAST_ADDU,
    CI_FAST_AND,
    CI_FAST_ANDI,
    CI_FAST_DADDIU,
    CI_FAST_DADDU,
    CI_FAST_DSUBU,
    CI_FAST_LUI,
    CI_FAST_MFHI,
    CI_FAST_MFLO,
    CI_FAST_MTHI,
    CI_FAST_MTLO,
    CI_FAST_MULT,
    CI_FAST_MULTU,
    CI_FAST_DIV,
    CI_FAST_DIVU,
    CI_FAST_DDIV,
    CI_FAST_DDIVU,
    CI_FAST_DMULT,
    CI_FAST_DMULTU,
    CI_FAST_NOR,
    CI_FAST_OR,
    CI_FAST_ORI,
    CI_FAST_SLL,
    CI_FAST_SLLV,
    CI_FAST_SLT,
    CI_FAST_SLTI,
    CI_FAST_SLTIU,
    CI_FAST_SLTU,
    CI_FAST_SRA,
    CI_FAST_SRAV,
    CI_FAST_SRL,
    CI_FAST_SRLV,
    CI_FAST_SUBU,
    CI_FAST_XOR,
    CI_FAST_XORI,
    CI_FAST_DSLL,
    CI_FAST_DSLL32,
    CI_FAST_DSLLV,
    CI_FAST_DSRA,
    CI_FAST_DSRA32,
    CI_FAST_DSRAV,
    CI_FAST_DSRL,
    CI_FAST_DSRL32,
    CI_FAST_DSRLV,
    CI_FAST_LUI_PAIR
};

struct cached_instruction
{
    uint32_t op;
    uint8_t meta, cycles, fp_kind, ram_kind;
};

struct cached_block
{
    /* Lookup validity, successors and execution descriptors are hot in both
     * engines. Keep source verification and native-link publication data
     * after them rather than fetching several cold cache lines per edge. */
    uint32_t virtual_pc;
    uint32_t physical_page;
    uint32_t page_generation;
    uint32_t epoch;
    struct cached_block* successors[2];
    uint8_t count;
    uint8_t valid;
    uint8_t next_successor;
    uint8_t delay_fast_op;
    uint8_t has_delay;
    uint8_t idle_end;
    uint8_t ram_poll;
    uint32_t delay_op;
    r4300_interp_handler delay_handler;
    /* Execution reads one compact descriptor. Handler pointers stay cold
     * for arithmetic, guarded RAM and direct COP1 dispatch. */
    struct cached_instruction instructions[CI_BLOCK_INSTRUCTIONS];
    r4300_interp_handler handlers[CI_BLOCK_INSTRUCTIONS];
#ifdef HAVE_ARI64_X64
    ari64_x64_entry native_entry, native_one_entry, native_chain_entry;
    uint64_t native_generation, native_one_generation;
    uint8_t heat, native_candidate;
    uint8_t native_link_replace;
    struct ari64_x64_link_cache native_links[2];
#endif
    struct interp_ram_poll poll;
    uint32_t source_words[CI_BLOCK_INSTRUCTIONS + 2];
    uint8_t source_count;
#ifdef ARI64_PROFILE
    uint64_t profile_entries;
#endif
};

struct cached_set
{
    struct cached_block ways[CI_WAYS];
    uint8_t next_replace;
};

struct cached_interp_state
{
#ifdef HAVE_ARI64_X64
    struct ari64_x64* jit;
    uint64_t jit_entries, jit_commits;
    uint64_t jit_fast_commits, jit_links;
    uint64_t jit_direct_links;
    uint64_t jit_compile_misses[2], jit_verifications[2], jit_replacements;
#ifdef ARI64_PROFILE
    uint64_t profile_slow[64], profile_special[64], profile_cop1[64];
    uint64_t profile_regions[33], profile_fpu[64];
#endif
#endif
    struct cached_set* sets;
    uint64_t decoded_blocks, restored_blocks;
    uint32_t set_mask;
    uint32_t page_generation[CI_PHYS_PAGE_COUNT];
    uint64_t page_cached[(CI_PHYS_PAGE_COUNT + 63) / 64];
    r4300_interp_handler pending_delay_handler;
    uint32_t pending_delay_op;
    uint32_t pending_delay_pc;
    uint8_t pending_delay_fast_op;
    uint8_t pending_delay_valid;
    uint32_t epoch;
    struct cached_block** recent_blocks;
    struct cached_block* previous_block;
};

static CI_FORCE_INLINE int ci_page_is_cached(const struct cached_interp_state* state, uint32_t page)
{
    return (int)((state->page_cached[page >> 6] >> (page & 63)) & UINT64_C(1));
}

static CI_FORCE_INLINE void ci_page_set_cached(struct cached_interp_state* state, uint32_t page)
{
    state->page_cached[page >> 6] |= UINT64_C(1) << (page & 63);
}

static CI_FORCE_INLINE void ci_page_clear_cached(struct cached_interp_state* state, uint32_t page)
{
    state->page_cached[page >> 6] &= ~(UINT64_C(1) << (page & 63));
}

static CI_FORCE_INLINE uint32_t ci_hash_pc(uint32_t pc)
{
    uint32_t x = pc >> 2;
    x ^= x >> 11;
    x ^= x >> 22;
    return x;
}

static CI_FORCE_INLINE int ci_translate_exec_page(struct r4300_core* r4300, uint32_t address, uint32_t* physical_page)
{
    uint32_t physical;

    if ((address & UINT32_C(0xc0000000)) == UINT32_C(0x80000000))
    {
        physical = address & UINT32_C(0x1fffffff);
    }
    else
    {
        const uint32_t mapped = r4300->cp0.tlb.LUT_r[address >> CI_PAGE_SHIFT];
        if (CI_UNLIKELY(mapped == 0))
        {
            if (fast_mem_access(r4300, address) == NULL)
                return 0;

            physical = r4300->cp0.tlb.LUT_r[address >> CI_PAGE_SHIFT] & UINT32_C(0x1fffffff);
        }
        else
        {
            physical = mapped & UINT32_C(0x1fffffff);
        }
    }

    *physical_page = physical >> CI_PAGE_SHIFT;
    return 1;
}

static int ci_is_relative_idle(uint32_t op, uint32_t next_op)
{
    return (int16_t)op == -1 && next_op == 0;
}

static int ci_is_absolute_idle(uint32_t pc, uint32_t op, uint32_t next_op)
{
    return (op & UINT32_C(0x03ffffff)) == ((pc & UINT32_C(0x0fffffff)) >> 2) &&
           (pc & UINT32_C(0x0fffffff)) != UINT32_C(0x0ffffffc) &&
           next_op == 0;
}

static int ci_can_use_idle_handler(enum r4300_opcode opcode, uint32_t pc, uint32_t op, uint32_t next_op)
{
    switch (opcode)
    {
    case R4300_OP_J:
    case R4300_OP_JAL:
        return ci_is_absolute_idle(pc, op, next_op);

    case R4300_OP_BC1F:
    case R4300_OP_BC1FL:
    case R4300_OP_BC1T:
    case R4300_OP_BC1TL:
    case R4300_OP_BEQ:
    case R4300_OP_BEQL:
    case R4300_OP_BGEZ:
    case R4300_OP_BGEZAL:
    case R4300_OP_BGEZALL:
    case R4300_OP_BGEZL:
    case R4300_OP_BGTZ:
    case R4300_OP_BGTZL:
    case R4300_OP_BLEZ:
    case R4300_OP_BLEZL:
    case R4300_OP_BLTZ:
    case R4300_OP_BLTZAL:
    case R4300_OP_BLTZALL:
    case R4300_OP_BLTZL:
    case R4300_OP_BNE:
    case R4300_OP_BNEL:
        return ci_is_relative_idle(op, next_op);

    default:
        return 0;
    }
}

static int ci_has_delay_slot(enum r4300_opcode opcode)
{
    switch (opcode)
    {
    case R4300_OP_BC1F:
    case R4300_OP_BC1FL:
    case R4300_OP_BC1T:
    case R4300_OP_BC1TL:
    case R4300_OP_BEQ:
    case R4300_OP_BEQL:
    case R4300_OP_BGEZ:
    case R4300_OP_BGEZAL:
    case R4300_OP_BGEZALL:
    case R4300_OP_BGEZL:
    case R4300_OP_BGTZ:
    case R4300_OP_BGTZL:
    case R4300_OP_BLEZ:
    case R4300_OP_BLEZL:
    case R4300_OP_BLTZ:
    case R4300_OP_BLTZAL:
    case R4300_OP_BLTZALL:
    case R4300_OP_BLTZL:
    case R4300_OP_BNE:
    case R4300_OP_BNEL:
    case R4300_OP_J:
    case R4300_OP_JAL:
    case R4300_OP_JALR:
    case R4300_OP_JR:
        return 1;

    default:
        return 0;
    }
}

static int ci_is_block_end_opcode(enum r4300_opcode opcode)
{
    switch (opcode)
    {
    case R4300_OP_RESERVED:
    case R4300_OP_BC1F:
    case R4300_OP_BC1FL:
    case R4300_OP_BC1T:
    case R4300_OP_BC1TL:
    case R4300_OP_BEQ:
    case R4300_OP_BEQL:
    case R4300_OP_BGEZ:
    case R4300_OP_BGEZAL:
    case R4300_OP_BGEZALL:
    case R4300_OP_BGEZL:
    case R4300_OP_BGTZ:
    case R4300_OP_BGTZL:
    case R4300_OP_BLEZ:
    case R4300_OP_BLEZL:
    case R4300_OP_BLTZ:
    case R4300_OP_BLTZAL:
    case R4300_OP_BLTZALL:
    case R4300_OP_BLTZL:
    case R4300_OP_BNE:
    case R4300_OP_BNEL:
    case R4300_OP_BREAK:
    case R4300_OP_ERET:
    case R4300_OP_J:
    case R4300_OP_JAL:
    case R4300_OP_JALR:
    case R4300_OP_JR:
    case R4300_OP_SYSCALL:
    case R4300_OP_TLBWI:
    case R4300_OP_TLBWR:
    case R4300_OP_TEQ:
    case R4300_OP_TEQI:
    case R4300_OP_TGE:
    case R4300_OP_TGEI:
    case R4300_OP_TGEIU:
    case R4300_OP_TGEU:
    case R4300_OP_TLT:
    case R4300_OP_TLTI:
    case R4300_OP_TLTIU:
    case R4300_OP_TLTU:
    case R4300_OP_TNE:
    case R4300_OP_TNEI:
        return 1;

    default:
        return 0;
    }
}

static int ci_may_invalidate_code_opcode(enum r4300_opcode opcode)
{
    switch (opcode)
    {
    case R4300_OP_SB:
    case R4300_OP_SC:
    case R4300_OP_SCD:
    case R4300_OP_SD:
    case R4300_OP_SDC1:
    case R4300_OP_SDL:
    case R4300_OP_SDR:
    case R4300_OP_SH:
    case R4300_OP_SW:
    case R4300_OP_SWC1:
    case R4300_OP_SWL:
    case R4300_OP_SWR:
        return 1;

    default:
        return 0;
    }
}

static int ci_is_ram_memory_opcode(enum r4300_opcode opcode)
{
    switch (opcode) {
    case R4300_OP_LB: case R4300_OP_LBU: case R4300_OP_LH: case R4300_OP_LHU:
    case R4300_OP_LW: case R4300_OP_LWU: case R4300_OP_LD:
    case R4300_OP_SB: case R4300_OP_SH: case R4300_OP_SW: case R4300_OP_SD:
    case R4300_OP_LWC1: case R4300_OP_LDC1: case R4300_OP_SWC1: case R4300_OP_SDC1:
        return 1;
    default: return 0;
    }
}

static uint8_t ci_classify_fp(uint32_t op)
{
    if ((op >> 26) != 17) return 0;
    unsigned fmt = (op >> 21) & 31, fn = op & 63;
    if (fmt == 16 || fmt == 17) {
        if (fn == 6) return 3;
    if (fn >= 48) return 2;
        if (fn < 16 || fn == 36 || fn == 37 || fn == (fmt == 16 ? 33u : 32u)) return 1;
    } else if ((fmt == 20 || fmt == 21) && (fn == 32 || fn == 33)) return 1;
    return 0;
}

/* Width is encoded as log2(bytes), with write/unsigned/COP1 flags. */
static uint8_t ci_classify_ram(enum r4300_opcode opcode)
{
    switch (opcode) {
    case R4300_OP_LB: return 0;
    case R4300_OP_LH: return 1;
    case R4300_OP_LW: return 2;
    case R4300_OP_LD: return 3 | 8;
    case R4300_OP_LBU: return 0 | 8;
    case R4300_OP_LHU: return 1 | 8;
    case R4300_OP_LWU: return 2 | 8;
    case R4300_OP_SB: return 0 | 4;
    case R4300_OP_SH: return 1 | 4;
    case R4300_OP_SW: return 2 | 4;
    case R4300_OP_SD: return 3 | 4;
    case R4300_OP_LWC1: return 2 | 16;
    case R4300_OP_LDC1: return 3 | 16;
    case R4300_OP_SWC1: return 2 | 4 | 16;
    case R4300_OP_SDC1: return 3 | 4 | 16;
    default: return 0;
    }
}

static uint8_t ci_classify_fast_op(enum r4300_opcode opcode, uint32_t op)
{
    const unsigned int rd = (op >> 11) & 31;
    const unsigned int rt = (op >> 16) & 31;

#define CI_FAST_RD(name) return rd != 0 ? CI_FAST_##name : CI_FAST_NOP
#define CI_FAST_RT(name) return rt != 0 ? CI_FAST_##name : CI_FAST_NOP

    switch (opcode)
    {
    case R4300_OP_NOP:
    case R4300_OP_SYNC:
        return CI_FAST_NOP;
    case R4300_OP_ADDIU: CI_FAST_RT(ADDIU);
    case R4300_OP_ADDU: CI_FAST_RD(ADDU);
    case R4300_OP_AND: CI_FAST_RD(AND);
    case R4300_OP_ANDI: CI_FAST_RT(ANDI);
    case R4300_OP_DADDIU: CI_FAST_RT(DADDIU);
    case R4300_OP_DADDU: CI_FAST_RD(DADDU);
    case R4300_OP_DSUBU: CI_FAST_RD(DSUBU);
    case R4300_OP_LUI: CI_FAST_RT(LUI);
    case R4300_OP_MFHI: CI_FAST_RD(MFHI);
    case R4300_OP_MFLO: CI_FAST_RD(MFLO);
    case R4300_OP_MTHI: return CI_FAST_MTHI;
    case R4300_OP_MTLO: return CI_FAST_MTLO;
    case R4300_OP_MULT: return CI_FAST_MULT;
    case R4300_OP_MULTU: return CI_FAST_MULTU;
    case R4300_OP_DIV: return CI_FAST_DIV;
    case R4300_OP_DIVU: return CI_FAST_DIVU;
    case R4300_OP_DDIV: return CI_FAST_DDIV;
    case R4300_OP_DDIVU: return CI_FAST_DDIVU;
#if defined(__SIZEOF_INT128__) || (defined(_MSC_VER) && defined(_M_X64))
    case R4300_OP_DMULT: return CI_FAST_DMULT;
    case R4300_OP_DMULTU: return CI_FAST_DMULTU;
#endif
    case R4300_OP_NOR: CI_FAST_RD(NOR);
    case R4300_OP_OR: CI_FAST_RD(OR);
    case R4300_OP_ORI: CI_FAST_RT(ORI);
    case R4300_OP_SLL: CI_FAST_RD(SLL);
    case R4300_OP_SLLV: CI_FAST_RD(SLLV);
    case R4300_OP_SLT: CI_FAST_RD(SLT);
    case R4300_OP_SLTI: CI_FAST_RT(SLTI);
    case R4300_OP_SLTIU: CI_FAST_RT(SLTIU);
    case R4300_OP_SLTU: CI_FAST_RD(SLTU);
    case R4300_OP_SRA: CI_FAST_RD(SRA);
    case R4300_OP_SRAV: CI_FAST_RD(SRAV);
    case R4300_OP_SRL: CI_FAST_RD(SRL);
    case R4300_OP_SRLV: CI_FAST_RD(SRLV);
    case R4300_OP_SUBU: CI_FAST_RD(SUBU);
    case R4300_OP_XOR: CI_FAST_RD(XOR);
    case R4300_OP_XORI: CI_FAST_RT(XORI);
    case R4300_OP_DSLL: CI_FAST_RD(DSLL);
    case R4300_OP_DSLL32: CI_FAST_RD(DSLL32);
    case R4300_OP_DSLLV: CI_FAST_RD(DSLLV);
    case R4300_OP_DSRA: CI_FAST_RD(DSRA);
    case R4300_OP_DSRA32: CI_FAST_RD(DSRA32);
    case R4300_OP_DSRAV: CI_FAST_RD(DSRAV);
    case R4300_OP_DSRL: CI_FAST_RD(DSRL);
    case R4300_OP_DSRL32: CI_FAST_RD(DSRL32);
    case R4300_OP_DSRLV: CI_FAST_RD(DSRLV);
    default:
        return CI_FAST_NONE;
    }

#undef CI_FAST_RD
#undef CI_FAST_RT
}

static CI_FORCE_INLINE int64_t ci_se32(uint32_t value)
{
    return (int64_t)(int32_t)value;
}

static CI_COLD_NOINLINE void ci_execute_fast_op(struct r4300_core* r4300, uint8_t fast_op, uint32_t op)
{
    if (fast_op == CI_FAST_NOP)
        return;

    int64_t* const regs = r4300->regs;
    const unsigned int rs = (op >> 21) & 31;
    const unsigned int rt = (op >> 16) & 31;
    const unsigned int rd = (op >> 11) & 31;
    const unsigned int sa = (op >> 6) & 31;
    const int32_t simm = (int16_t)op;

    switch ((enum ci_fast_op)fast_op)
    {
    case CI_FAST_NOP:
        break;
    case CI_FAST_ADDIU:
        regs[rt] = ci_se32((uint32_t)regs[rs] + (uint32_t)simm);
        break;
    case CI_FAST_ADDU:
        regs[rd] = ci_se32((uint32_t)regs[rs] + (uint32_t)regs[rt]);
        break;
    case CI_FAST_AND:
        regs[rd] = regs[rs] & regs[rt];
        break;
    case CI_FAST_ANDI:
        regs[rt] = regs[rs] & (uint16_t)op;
        break;
    case CI_FAST_DADDIU:
        regs[rt] = (int64_t)((uint64_t)regs[rs] + (uint64_t)(int64_t)simm);
        break;
    case CI_FAST_DADDU:
        regs[rd] = (int64_t)((uint64_t)regs[rs] + (uint64_t)regs[rt]);
        break;
    case CI_FAST_DSUBU:
        regs[rd] = (int64_t)((uint64_t)regs[rs] - (uint64_t)regs[rt]);
        break;
    case CI_FAST_LUI:
    case CI_FAST_LUI_PAIR:
        regs[rt] = ci_se32((uint32_t)simm << 16);
        break;
    case CI_FAST_MFHI:
        regs[rd] = r4300->hi;
        break;
    case CI_FAST_MFLO:
        regs[rd] = r4300->lo;
        break;
    case CI_FAST_MTHI:
        r4300->hi = regs[rs];
        break;
    case CI_FAST_MTLO:
        r4300->lo = regs[rs];
        break;
    case CI_FAST_MULT:
    {
        const int64_t rhs = (int64_t)((uint64_t)regs[rt] << 29) >> 29;
        const uint64_t product = (uint64_t)regs[rs] * (uint64_t)rhs;
        r4300->hi = (int64_t)(int32_t)(uint32_t)((uint64_t)product >> 32);
        r4300->lo = ci_se32((uint32_t)product);
        break;
    }
    case CI_FAST_MULTU:
    {
        const uint64_t product = (uint32_t)regs[rs] * (uint64_t)(uint32_t)regs[rt];
        r4300->hi = (int64_t)(int32_t)(uint32_t)(product >> 32);
        r4300->lo = ci_se32((uint32_t)product);
        break;
    }
    case CI_FAST_DIV:
    {
        const int64_t lhs = (int32_t)(uint32_t)regs[rs];
        const int64_t rhs = regs[rt];
        if (rhs != 0)
        {
            if (lhs == INT32_MIN && rhs == -1)
            {
                r4300->lo = ci_se32((uint32_t)lhs);
                r4300->hi = 0;
            }
            else
            {
                r4300->lo = ci_se32((uint32_t)(lhs / rhs));
                r4300->hi = ci_se32((uint32_t)(lhs % rhs));
            }
        }
        else
        {
            r4300->lo = lhs < 0 ? 1 : -1;
            r4300->hi = ci_se32((uint32_t)lhs);
        }
        break;
    }
    case CI_FAST_DIVU:
    {
        const uint32_t lhs = (uint32_t)regs[rs];
        const uint32_t rhs = (uint32_t)regs[rt];
        if (rhs != 0)
        {
            r4300->lo = ci_se32(lhs / rhs);
            r4300->hi = ci_se32(lhs % rhs);
        }
        else
        {
            r4300->lo = -1;
            r4300->hi = ci_se32(lhs);
        }
        break;
    }
    case CI_FAST_DDIV:
    {
        const int64_t lhs = regs[rs];
        const int64_t rhs = regs[rt];
        if (rhs != 0)
        {
            if (lhs == INT64_MIN && rhs == -1)
            {
                r4300->lo = lhs;
                r4300->hi = 0;
            }
            else
            {
                r4300->lo = lhs / rhs;
                r4300->hi = lhs % rhs;
            }
        }
        else
        {
            r4300->lo = lhs < 0 ? 1 : -1;
            r4300->hi = lhs;
        }
        break;
    }
    case CI_FAST_DDIVU:
    {
        const uint64_t lhs = (uint64_t)regs[rs];
        const uint64_t rhs = (uint64_t)regs[rt];
        if (rhs != 0)
        {
            r4300->lo = (int64_t)(lhs / rhs);
            r4300->hi = (int64_t)(lhs % rhs);
        }
        else
        {
            r4300->lo = -1;
            r4300->hi = (int64_t)lhs;
        }
        break;
    }
    case CI_FAST_DMULT:
    {
#if defined(__SIZEOF_INT128__)
        const __int128 product = (__int128)regs[rs] * (__int128)regs[rt];
        const unsigned __int128 bits = (unsigned __int128)product;
        r4300->lo = (int64_t)(uint64_t)bits;
        r4300->hi = (int64_t)(uint64_t)(bits >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
        __int64 high;
        r4300->lo = (int64_t)_mul128((__int64)regs[rs], (__int64)regs[rt], &high);
        r4300->hi = (int64_t)high;
#endif
        break;
    }
    case CI_FAST_DMULTU:
    {
#if defined(__SIZEOF_INT128__)
        const unsigned __int128 product = (unsigned __int128)(uint64_t)regs[rs] *
                                          (unsigned __int128)(uint64_t)regs[rt];
        r4300->lo = (int64_t)(uint64_t)product;
        r4300->hi = (int64_t)(uint64_t)(product >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
        unsigned __int64 high;
        r4300->lo = (int64_t)_umul128((unsigned __int64)regs[rs],
                                      (unsigned __int64)regs[rt], &high);
        r4300->hi = (int64_t)high;
#endif
        break;
    }
    case CI_FAST_NOR:
        regs[rd] = ~(regs[rs] | regs[rt]);
        break;
    case CI_FAST_OR:
        regs[rd] = regs[rs] | regs[rt];
        break;
    case CI_FAST_ORI:
        regs[rt] = regs[rs] | (uint16_t)op;
        break;
    case CI_FAST_SLL:
        regs[rd] = ci_se32((uint32_t)regs[rt] << sa);
        break;
    case CI_FAST_SLLV:
        regs[rd] = ci_se32((uint32_t)regs[rt] << ((uint32_t)regs[rs] & 31));
        break;
    case CI_FAST_SLT:
        regs[rd] = (int64_t)(regs[rs] < regs[rt]);
        break;
    case CI_FAST_SLTI:
        regs[rt] = (int64_t)(regs[rs] < (int64_t)simm);
        break;
    case CI_FAST_SLTIU:
        regs[rt] = (int64_t)((uint64_t)regs[rs] < (uint64_t)(int64_t)simm);
        break;
    case CI_FAST_SLTU:
        regs[rd] = (int64_t)((uint64_t)regs[rs] < (uint64_t)regs[rt]);
        break;
    case CI_FAST_SRA:
        regs[rd] = ci_se32((uint32_t)(regs[rt] >> sa));
        break;
    case CI_FAST_SRAV:
        regs[rd] = ci_se32((uint32_t)(regs[rt] >> ((uint32_t)regs[rs] & 31)));
        break;
    case CI_FAST_SRL:
        regs[rd] = ci_se32((uint32_t)regs[rt] >> sa);
        break;
    case CI_FAST_SRLV:
        regs[rd] = ci_se32((uint32_t)regs[rt] >> ((uint32_t)regs[rs] & 31));
        break;
    case CI_FAST_SUBU:
        regs[rd] = ci_se32((uint32_t)regs[rs] - (uint32_t)regs[rt]);
        break;
    case CI_FAST_XOR:
        regs[rd] = regs[rs] ^ regs[rt];
        break;
    case CI_FAST_XORI:
        regs[rt] = regs[rs] ^ (uint16_t)op;
        break;
    case CI_FAST_DSLL:
        regs[rd] = (int64_t)((uint64_t)regs[rt] << sa);
        break;
    case CI_FAST_DSLL32:
        regs[rd] = (int64_t)((uint64_t)regs[rt] << (32 + sa));
        break;
    case CI_FAST_DSLLV:
        regs[rd] = (int64_t)((uint64_t)regs[rt] << ((uint32_t)regs[rs] & 63));
        break;
    case CI_FAST_DSRA:
        regs[rd] = regs[rt] >> sa;
        break;
    case CI_FAST_DSRA32:
        regs[rd] = regs[rt] >> (32 + sa);
        break;
    case CI_FAST_DSRAV:
        regs[rd] = regs[rt] >> ((uint32_t)regs[rs] & 63);
        break;
    case CI_FAST_DSRL:
        regs[rd] = (int64_t)((uint64_t)regs[rt] >> sa);
        break;
    case CI_FAST_DSRL32:
        regs[rd] = (int64_t)((uint64_t)regs[rt] >> (32 + sa));
        break;
    case CI_FAST_DSRLV:
        regs[rd] = (int64_t)((uint64_t)regs[rt] >> ((uint32_t)regs[rs] & 63));
        break;
    case CI_FAST_NONE:
    default:
        break;
    }
}

static CI_COLD_NOINLINE struct cached_block* ci_build_block(struct r4300_core* r4300, uint32_t pc, uint32_t physical_page)
{
    struct cached_interp_state* state = r4300->cached_interp;
    struct cached_set* set = &state->sets[ci_hash_pc(pc) & state->set_mask];
    struct cached_block* block = &set->ways[set->next_replace];
    uint32_t* op_ptr;
    unsigned int max_count, page_count;
    unsigned int i;



    set->next_replace ^= 1;
#ifdef HAVE_ARI64_X64
    if (state->jit && block->valid && block->virtual_pc != pc) ++state->jit_replacements;
#endif
    block->valid = 0;
#ifdef HAVE_ARI64_X64
    block->native_entry = block->native_one_entry = NULL;
    block->heat = 0;
    block->native_candidate = 0;
    block->native_link_replace = 0;
    memset(block->native_links,0,sizeof(block->native_links));
#ifdef ARI64_PROFILE
    block->profile_entries = 0;
#endif
#endif
    block->count = 0;
    block->has_delay = 0;
    block->idle_end = 0;
    block->ram_poll = 0;
    block->successors[0] = block->successors[1] = NULL;
    block->next_successor = 0;

    op_ptr = fast_mem_access(r4300, pc);
    if (CI_UNLIKELY(op_ptr == NULL))
        return NULL;

    page_count = (CI_PAGE_SIZE - (pc & (CI_PAGE_SIZE - 1))) >> 2;
    max_count = page_count;
    if (max_count > CI_BLOCK_INSTRUCTIONS)
        max_count = CI_BLOCK_INSTRUCTIONS;

    for (i = 0; i < max_count; ++i)
    {
        const uint32_t instruction_pc = pc + (i << 2);
        const uint32_t op = op_ptr[i];
        /* Idle-loop recognition must not depend on the decoded block size. */
        const uint32_t next_op = (i + 1 < page_count) ? op_ptr[i + 1] : UINT32_C(0xffffffff);
        const enum r4300_opcode opcode = r4300_get_idec(op)->opcode;
        const int idle = ci_can_use_idle_handler(opcode, instruction_pc, op, next_op);
        r4300_interp_handler handler = pure_interp_decode(op, idle);

        if (handler == NULL)
        {
            if (i == 0)
                return NULL;
            break;
        }

        block->handlers[i] = handler;
        block->instructions[i].op = op;
        /* COP1 charges its extra latency in the handler after commit. All
         * other base latencies depend only on the encoded instruction. */
        block->instructions[i].cycles = (uint8_t)((op >> 26) == 17 ? 1 : r4300_base_cycles(op, 0));
        block->instructions[i].fp_kind = ci_classify_fp(op);
        block->instructions[i].ram_kind = ci_classify_ram(opcode);
        block->instructions[i].meta = (uint8_t)(
            (ci_classify_fast_op(opcode, op) << CI_META_FAST_SHIFT) |
            (ci_is_ram_memory_opcode(opcode) ? CI_FLAG_RAM_MEMORY : 0) |
            (ci_may_invalidate_code_opcode(opcode) ? CI_FLAG_MAY_INVALIDATE : 0));
        block->count = (uint8_t)(i + 1);

        if (ci_is_block_end_opcode(opcode))
        {
            block->idle_end = idle;
            if (ci_has_delay_slot(opcode) && i + 1 < max_count)
            {
                const uint32_t delay_op = op_ptr[i + 1];
                const uint32_t delay_pc = instruction_pc + 4;
                const uint32_t delay_next_op =
                    (i + 2 < max_count) ? op_ptr[i + 2] : UINT32_C(0xffffffff);
                const enum r4300_opcode delay_opcode = r4300_get_idec(delay_op)->opcode;
                const int delay_idle =
                    ci_can_use_idle_handler(delay_opcode, delay_pc, delay_op, delay_next_op);
                r4300_interp_handler delay_handler = pure_interp_decode(delay_op, delay_idle);

                if (delay_handler != NULL)
                {
                    block->delay_handler = delay_handler;
                    block->delay_op = delay_op;
                    block->delay_fast_op = ci_classify_fast_op(delay_opcode, delay_op);
                    block->has_delay = 1;
                }
            }
            break;
        }
    }

    if (block->count == 0)
        return NULL;

    for (i = 0; i + 1 < block->count; ++i) {
        if ((block->instructions[i].op >> 26) == 15 &&
            interp_lui_pair(block->instructions[i].op, block->instructions[i + 1].op))
            block->instructions[i].meta = (uint8_t)(CI_FAST_LUI_PAIR << CI_META_FAST_SHIFT);
    }
    /* Keep recognition out of the dispatch loop. Both halves must share
     * this tracked code page, so normal invalidation covers the whole plan. */
    if (block->count == 5 && block->has_delay && block->delay_op == 0 &&
        (op_ptr[0] >> 26) == 15 && (op_ptr[4] >> 26) == 4) {
        uint32_t target = pc + 20 + (int32_t)(int16_t)op_ptr[4] * 4;
        if ((pc >> 12) == (target >> 12) && (target & 4095) <= 4096 - 16 &&
            (pc & UINT32_C(0xe0000000)) == UINT32_C(0x80000000)) {
            const uint32_t* second = op_ptr + ((int32_t)(target & 4095) - (int32_t)(pc & 4095)) / 4;
            block->ram_poll = interp_ram_poll_decode(&block->poll, pc, op_ptr, target, second);
        }
    }
    if (!r4300->cache_timing && block->count == 1 && block->has_delay && page_count >= 5 &&
        interp_thread_poll_shape(pc, op_ptr) &&
        interp_thread_poll_getter(r4300, pc, op_ptr)) block->ram_poll = 2;
    block->virtual_pc = pc;
    /* Keep the lookahead used by idle/delay decoding as well as the body.
     * Dirty code is reusable only after comparing every dependent word. */
    block->source_count = (uint8_t)(block->count + 2 < page_count ? block->count + 2 : page_count);
    memcpy(block->source_words, op_ptr, block->source_count * sizeof(uint32_t));
    block->physical_page = physical_page;
    block->page_generation = state->page_generation[physical_page];
    block->epoch = state->epoch;
    block->valid = 1;
    ++state->decoded_blocks;
    ci_page_set_cached(state, physical_page);
    return block;
}

static CI_FORCE_INLINE int ci_block_matches(const struct cached_block* block,
                                                  const struct cached_interp_state* state,
                                                  uint32_t pc,
                                                  uint32_t physical_page)
{
    return block->valid &&
           block->virtual_pc == pc &&
           block->physical_page == physical_page &&
           block->epoch == state->epoch &&
           block->page_generation == state->page_generation[physical_page];
}

static CI_FORCE_INLINE int ci_mapping_matches(struct r4300_core* r4300, const struct cached_block* block)
{
    if ((block->virtual_pc & UINT32_C(0xc0000000)) == UINT32_C(0x80000000)) return 1;
    uint32_t mapped = r4300->cp0.tlb.LUT_r[block->virtual_pc >> CI_PAGE_SHIFT];
    return mapped != 0 && ((mapped & UINT32_C(0x1fffffff)) >> CI_PAGE_SHIFT) == block->physical_page;
}

static int ci_verify_dirty_block(struct r4300_core* r, struct cached_block* block,
    uint32_t pc, uint32_t physical_page)
{
    if (!block->valid || block->ram_poll ||
        block->virtual_pc != pc || block->physical_page != physical_page) return 0;
    if (block->epoch == r->cached_interp->epoch &&
        block->page_generation == r->cached_interp->page_generation[physical_page]) return 1;
    const uint32_t* source = fast_mem_access(r, pc);
    if (!source || memcmp(source, block->source_words, block->source_count * sizeof(uint32_t))) {
#ifdef HAVE_ARI64_X64
        ++r->cached_interp->jit_verifications[1];
#endif
        return 0;
    }
    if (block->epoch != r->cached_interp->epoch || block->page_generation != r->cached_interp->page_generation[physical_page]) {
        ++r->cached_interp->restored_blocks;
#ifdef HAVE_ARI64_X64
        ++r->cached_interp->jit_verifications[0];
#endif
    }
    /* A data write elsewhere on the page or an unchanged TLB mapping does
     * not change the compiled instructions. Retain code and hotness, but
     * restore write tracking before any native execution resumes. */
    block->epoch = r->cached_interp->epoch;
    block->page_generation = r->cached_interp->page_generation[physical_page];
    ci_page_set_cached(r->cached_interp, physical_page);
    return 1;
}

static struct cached_block* ci_lookup_block(struct r4300_core* r4300, uint32_t pc)
{
    struct cached_interp_state* state = r4300->cached_interp;
    struct cached_set* set;
    struct cached_block* block;
    uint32_t physical_page;
    const unsigned int recent_index = ci_hash_pc(pc) & state->set_mask;
    struct cached_block* previous = state->previous_block;

    /* Most block edges have one or two destinations. Validate linked blocks
     * just like recent hits: slots may be replaced, and writes/TLB changes
     * must never let a link bypass page generations or the mapping epoch. */
    if (previous != NULL) {
        for (unsigned edge = 0; edge < 2; ++edge) {
            block = previous->successors[edge];
            if (block != NULL && block->valid && block->virtual_pc == pc &&
                block->epoch == state->epoch &&
                block->page_generation == state->page_generation[block->physical_page] &&
                ci_mapping_matches(r4300, block)) {
                state->previous_block = block;
                return block;
            }
        }
    }

    block = state->recent_blocks[recent_index];
    if (CI_LIKELY(block != NULL &&
                  block->valid &&
                  block->virtual_pc == pc &&
                  block->epoch == state->epoch &&
                  block->page_generation == state->page_generation[block->physical_page] &&
                  ci_mapping_matches(r4300, block)))
        goto found;

    if (CI_UNLIKELY(!ci_translate_exec_page(r4300, pc, &physical_page)))
        return NULL;

    set = &state->sets[ci_hash_pc(pc) & state->set_mask];

    for (unsigned way = 0; way < CI_WAYS; ++way) {
        block = &set->ways[way];
        if (ci_verify_dirty_block(r4300, block, pc, physical_page)) {
            set->next_replace = way ^ 1;
            state->recent_blocks[recent_index] = block;
            goto found;
        }
    }

    block = &set->ways[0];
    if (CI_LIKELY(ci_block_matches(block, state, pc, physical_page)))
    {
        set->next_replace = 1;
        state->recent_blocks[recent_index] = block;
        goto found;
    }

    block = &set->ways[1];
    if (CI_LIKELY(ci_block_matches(block, state, pc, physical_page)))
    {
        set->next_replace = 0;
        state->recent_blocks[recent_index] = block;
        goto found;
    }

    block = ci_build_block(r4300, pc, physical_page);
    if (block != NULL)
        state->recent_blocks[recent_index] = block;
found:
    if (block != NULL) {
        if (previous != NULL) {
            previous->successors[previous->next_successor] = block;
            previous->next_successor ^= 1;
        }
        state->previous_block = block;
    }
    return block;
}

static void ci_bump_page(struct cached_interp_state* state, uint32_t physical_page)
{
    if (!ci_page_is_cached(state, physical_page))
        return;

    ci_page_clear_cached(state, physical_page);
    if (++state->page_generation[physical_page] == 0)
    {
        memset(state->sets, 0, (state->set_mask + 1) * sizeof(*state->sets));
        memset(state->page_generation, 0, sizeof(state->page_generation));
        memset(state->page_cached, 0, sizeof(state->page_cached));
        ++state->epoch;
        if (state->epoch == 0)
            state->epoch = 1;
    }
}

static int ci_virtual_to_physical_page_noexcept(struct r4300_core* r4300, uint32_t address, uint32_t* physical_page)
{
    uint32_t physical;

    if ((address & UINT32_C(0xc0000000)) == UINT32_C(0x80000000))
    {
        physical = address & UINT32_C(0x1fffffff);
    }
    else if (address < UINT32_C(0x20000000))
    {
        physical = address;
    }
    else
    {
        uint32_t mapped = r4300->cp0.tlb.LUT_w[address >> CI_PAGE_SHIFT];
        if (mapped == 0)
            mapped = r4300->cp0.tlb.LUT_r[address >> CI_PAGE_SHIFT];
        if (mapped == 0)
            return 0;
        physical = mapped & UINT32_C(0x1fffffff);
    }

    *physical_page = physical >> CI_PAGE_SHIFT;
    return 1;
}

int cached_interp_init(struct r4300_core* r4300)
{
    if (r4300->cached_interp != NULL)
        return 1;

    r4300->cached_interp = (struct cached_interp_state*)calloc(1, sizeof(*r4300->cached_interp));
    if (r4300->cached_interp == NULL)
        return 0;
    unsigned set_count = CI_SET_COUNT;
#ifdef HAVE_ARI64_X64
#ifndef CI_JIT_SET_COUNT
#define CI_JIT_SET_COUNT 32768
#endif
    if (r4300->emumode == EMUMODE_DYNAREC) set_count = CI_JIT_SET_COUNT;
#endif
    r4300->cached_interp->set_mask = set_count - 1;
    r4300->cached_interp->sets = calloc(set_count, sizeof(*r4300->cached_interp->sets));
    if (!r4300->cached_interp->sets) { cached_interp_destroy(r4300); return 0; }
    r4300->cached_interp->recent_blocks = calloc(set_count, sizeof(*r4300->cached_interp->recent_blocks));
    if (!r4300->cached_interp->recent_blocks) { cached_interp_destroy(r4300); return 0; }

    r4300->cached_interp->epoch = 1;
    r4300->cached_code_pages = r4300->cached_interp->page_cached;
#ifdef HAVE_ARI64_X64
    if (r4300->emumode == EMUMODE_DYNAREC) {
        r4300->cached_interp->jit = ari64_x64_create();
        if (!r4300->cached_interp->jit) {
            cached_interp_destroy(r4300);
            return 0;
        }
    }
#endif
    return 1;
}

void cached_interp_destroy(struct r4300_core* r4300)
{
#ifdef HAVE_ARI64_X64
    if (r4300->cached_interp) ari64_x64_destroy(r4300->cached_interp->jit);
#endif
    if (r4300->cached_interp) free(r4300->cached_interp->sets);
    if (r4300->cached_interp) free(r4300->cached_interp->recent_blocks);
    free(r4300->cached_interp);
    r4300->cached_interp = NULL;
    r4300->cached_code_pages = NULL;
    r4300->execute_one = NULL;
}
void cached_interp_cache_stats(struct r4300_core* r, uint64_t stats[2])
{
    stats[0] = r->cached_interp ? r->cached_interp->decoded_blocks : 0;
    stats[1] = r->cached_interp ? r->cached_interp->restored_blocks : 0;
}

void cached_interp_invalidate(struct r4300_core* r4300, uint32_t address, size_t size)
{
    struct cached_interp_state* state = r4300->cached_interp;
    uint64_t first;
    uint64_t last;
    uint64_t page_address;

    if (state == NULL)
        return;

    if (size == 0)
    {
        ++state->epoch;
        memset(state->page_cached, 0, sizeof(state->page_cached));
        if (state->epoch == 0)
        {
            memset(state->sets, 0, (state->set_mask + 1) * sizeof(*state->sets));
            memset(state->recent_blocks, 0, (state->set_mask + 1) * sizeof(*state->recent_blocks));
            memset(state->page_generation, 0, sizeof(state->page_generation));
            state->previous_block = NULL;
            state->pending_delay_valid = 0;
            state->epoch = 1;
        }
        return;
    }

    first = address & ~(uint64_t)(CI_PAGE_SIZE - 1);
    last = ((uint64_t)address + size - 1) & ~(uint64_t)(CI_PAGE_SIZE - 1);

    for (page_address = first; page_address <= last; page_address += CI_PAGE_SIZE)
    {
        uint32_t physical_page;
        if (ci_virtual_to_physical_page_noexcept(r4300, (uint32_t)page_address, &physical_page))
            ci_bump_page(state, physical_page);

        if (page_address + CI_PAGE_SIZE > UINT32_MAX)
            break;
    }
}

static CI_FORCE_INLINE int ci_execute_hot_fast_op(struct r4300_core* r4300, uint8_t fast_op, uint32_t op)
{
    int64_t* const regs = r4300->regs;

    if (CI_LIKELY(fast_op == CI_FAST_ADDIU))
    {
        const unsigned int rs = (op >> 21) & 31;
        const unsigned int rt = (op >> 16) & 31;
        regs[rt] = ci_se32((uint32_t)regs[rs] + (uint32_t)(int32_t)(int16_t)op);
        return 1;
    }

    if (CI_LIKELY(fast_op == CI_FAST_ADDU))
    {
        const unsigned int rs = (op >> 21) & 31;
        const unsigned int rt = (op >> 16) & 31;
        const unsigned int rd = (op >> 11) & 31;
        regs[rd] = ci_se32((uint32_t)regs[rs] + (uint32_t)regs[rt]);
        return 1;
    }

    const unsigned int rs = (op >> 21) & 31, rt = (op >> 16) & 31;
    const unsigned int rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    switch (fast_op) {
    case CI_FAST_NOP: break;
    case CI_FAST_AND: regs[rd] = regs[rs] & regs[rt]; break;
    case CI_FAST_ANDI: regs[rt] = regs[rs] & (uint16_t)op; break;
    case CI_FAST_OR: regs[rd] = regs[rs] | regs[rt]; break;
    case CI_FAST_ORI: regs[rt] = regs[rs] | (uint16_t)op; break;
    case CI_FAST_XOR: regs[rd] = regs[rs] ^ regs[rt]; break;
    case CI_FAST_XORI: regs[rt] = regs[rs] ^ (uint16_t)op; break;
    case CI_FAST_NOR: regs[rd] = ~(regs[rs] | regs[rt]); break;
    case CI_FAST_SUBU: regs[rd] = ci_se32((uint32_t)regs[rs] - (uint32_t)regs[rt]); break;
    case CI_FAST_LUI_PAIR:
    case CI_FAST_LUI: regs[rt] = ci_se32((uint32_t)(uint16_t)op << 16); break;
    case CI_FAST_SLL: regs[rd] = ci_se32((uint32_t)regs[rt] << sa); break;
    case CI_FAST_SRL: regs[rd] = ci_se32((uint32_t)regs[rt] >> sa); break;
    case CI_FAST_SRA: regs[rd] = ci_se32((uint32_t)(regs[rt] >> sa)); break;
    case CI_FAST_SLLV: regs[rd] = ci_se32((uint32_t)regs[rt] << (regs[rs] & 31)); break;
    case CI_FAST_SRLV: regs[rd] = ci_se32((uint32_t)regs[rt] >> (regs[rs] & 31)); break;
    case CI_FAST_SRAV: regs[rd] = ci_se32((uint32_t)(regs[rt] >> (regs[rs] & 31))); break;
    case CI_FAST_SLT: regs[rd] = regs[rs] < regs[rt]; break;
    case CI_FAST_SLTU: regs[rd] = (uint64_t)regs[rs] < (uint64_t)regs[rt]; break;
    case CI_FAST_SLTI: regs[rt] = regs[rs] < (int64_t)(int16_t)op; break;
    case CI_FAST_SLTIU: regs[rt] = (uint64_t)regs[rs] < (uint64_t)(int64_t)(int16_t)op; break;
    default: return 0;
    }
    return 1;
}

static CI_FORCE_INLINE void ci_execute_cached_instruction(struct r4300_core* r4300,
                                                          r4300_interp_handler handler,
                                                          uint32_t op,
                                                          uint8_t fast_op,
                                                          uint32_t pc)
{
    if (!r4300_begin_instruction(r4300, op, pc)) return;
    if (CI_LIKELY(fast_op != CI_FAST_NONE))
    {
        if (!ci_execute_hot_fast_op(r4300, fast_op, op)) ci_execute_fast_op(r4300, fast_op, op);
        r4300->interp_PC.addr = pc + 4;
    }
    else
    {
        if (!interp_word_memory(r4300, op)) handler(r4300, op);
    }
    r4300->regs[0] = 0;
    if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0)
        gen_interrupt(r4300);
}

static CI_FORCE_INLINE void ci_execute_one(struct r4300_core* r4300)
{
    if (r4300->interp_PC.addr & 3) { pure_interp_execute_one(r4300); return; }
    struct cached_interp_state* state = r4300->cached_interp;
    const uint32_t pc = r4300->interp_PC.addr;

    if (CI_LIKELY(state != NULL && state->pending_delay_valid && state->pending_delay_pc == pc))
    {
        r4300_interp_handler handler = state->pending_delay_handler;
        const uint32_t op = state->pending_delay_op;
        const uint8_t fast_op = state->pending_delay_fast_op;
        state->pending_delay_valid = 0;
        ci_execute_cached_instruction(r4300, handler, op, fast_op, pc);
        return;
    }

    {
        struct cached_block* block = ci_lookup_block(r4300, pc);
        if (CI_UNLIKELY(block == NULL))
        {
            if (r4300->interp_PC.addr != pc) {
                /* Lookup already raised a fetch exception. Charge the failed
                 * fetch, rather than executing the exception vector here. */
                cp0_step_cycles(&r4300->cp0, 1);
                if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0) gen_interrupt(r4300);
                return;
            }
            pure_interp_execute_one(r4300);
            return;
        }

        ci_execute_cached_instruction(r4300, block->handlers[0], block->instructions[0].op,
                                      block->instructions[0].meta >> CI_META_FAST_SHIFT, pc);
    }
}

void cached_interp_execute_one(struct r4300_core* r4300)
{
    ci_execute_one(r4300);
}

/* These accesses cannot trap or observe COUNT. Keep the installed handler
 * authoritative and flush the batch before MMIO, TLB faults, alignment, cache misses
 * or code-page accesses, which use the shared interpreter memory path. */
static CI_FORCE_INLINE int ci_execute_direct_ram(struct r4300_core* r, uint32_t op, uint8_t kind,
    unsigned int cache_timing, unsigned int* access_cycles, unsigned int* pipeline_cycles)
{
    unsigned fp = kind & 16;
    if (fp && !(r->cp0.regs[CP0_STATUS_REG] & CP0_STATUS_CU1)) return 0;
    unsigned alignment = (1u << (kind & 3)) - 1;
    uint32_t address = (uint32_t)r->regs[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)op;
    if (address & alignment) return 0;
    int write = (kind & 4) != 0;
    uint32_t translated = address;
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        uint32_t mapped = write ? r->cp0.tlb.LUT_w[address >> CI_PAGE_SHIFT] : r->cp0.tlb.LUT_r[address >> CI_PAGE_SHIFT];
        if (!mapped) return 0;
        translated = (mapped & UINT32_C(0xfffff000)) | (address & UINT32_C(0xfff));
        if (!translated) return 0;
    }
    uint32_t physical = translated & UINT32_C(0x1ffffffc);
    const struct mem_handler* handler = &r->mem->handlers[physical >> 16];
    unsigned rt = (op >> 16) & 31;
    if (write) {
        unsigned page = physical >> CI_PAGE_SHIFT;
        if (handler->write32 != write_rdram_dram || ci_page_is_cached(r->cached_interp, page)) return 0;
    } else if (handler->read32 != read_rdram_dram) return 0;
    uint32_t* word;
    if (cache_timing && r4300_access_cached(r, address)) {
        unsigned index = (address >> 4) & 511;
        uint32_t* tag = &r->dcache_tags[index];
        if ((*tag & ~2u) != ((physical & UINT32_C(0x1ffff000)) | 1)) return 0;
        word = &r->dcache_words[index][(physical >> 2) & 3];
        if (write) *tag |= 2;
        *access_cycles = R4300_DCACHE_HIT_CYCLES;
    } else {
        struct rdram* rdram = (struct rdram*)handler->opaque;
        word = &rdram->dram[rdram_dram_address(physical)];
        *access_cycles = cache_timing ? (!write && physical < UINT32_C(0x00800000) ? 31 : 38) : 0;
        if (cache_timing && physical >= UINT32_C(0x04000000) && physical < UINT32_C(0x05000000))
            *access_cycles = write ? 8 : 21;
    }
    /* Only commit history after every fast-path guard succeeds, before
     * a load can overwrite its own base register. Fallback must see the
     * previous instruction's history exactly once. */
    *pipeline_cycles = cache_timing ? r4300_pipeline_cycles(r, op) : 0;
    if (write) {
        uint64_t value = (uint64_t)r->regs[rt];
        if (fp) {
            if (alignment == 7) memcpy(&value, r->cp1.regs_double[rt], 8);
            else { uint32_t single; memcpy(&single, r->cp1.regs_simple[rt], 4); value = single; }
        }
        if (alignment == 7) { word[0] = (uint32_t)(value >> 32); word[1] = (uint32_t)value; }
        else if (alignment == 3) *word = (uint32_t)value;
        else {
            unsigned shift = (alignment == 0 ? 3 - (address & 3) : 2 - (address & 2)) * 8;
            uint32_t mask = (alignment == 0 ? UINT32_C(0xff) : UINT32_C(0xffff)) << shift;
            masked_write(word, (uint32_t)value << shift, mask);
        }
    } else {
        uint64_t value = *word;
        if (alignment == 7) value = (value << 32) | word[1];
        else if (alignment == 0) value = (value >> ((3 - (address & 3)) * 8)) & UINT32_C(0xff);
        else if (alignment == 1) value = (value >> ((2 - (address & 2)) * 8)) & UINT32_C(0xffff);
        if (fp) {
            if (alignment == 7) memcpy(r->cp1.regs_double[rt], &value, 8);
            else { uint32_t single = (uint32_t)value; memcpy(r->cp1.regs_simple[rt], &single, 4); }
        } else if (rt) {
            if (!(kind & 8)) {
                if (alignment == 0) value = (uint64_t)(int64_t)(int8_t)value;
                else if (alignment == 1) value = (uint64_t)(int64_t)(int16_t)value;
                else value = (uint64_t)(int64_t)(int32_t)value;
            }
            r->regs[rt] = (int64_t)value;
        }
    }
    return 1;
}

/* Fast descriptors contain only nontrapping integer operations. Their issue
 * history needs the raw-field integer LDI check, then clears every outgoing
 * load/FPU/store dependency. Avoid decoding the instruction again here. */
static CI_FORCE_INLINE unsigned ci_integer_pipeline_cycles(struct r4300_core* r, uint32_t op)
{
    uint64_t candidates = r4300_pipeline_gpr((op >> 21) & 31) | r4300_pipeline_gpr((op >> 16) & 31);
    unsigned stalls = !(r->pipeline_load_mask >> 32) && (r->pipeline_load_mask & candidates) != 0;
    r->pipeline_load_mask = r->pipeline_fpu_result_mask = 0;
    r->pipeline_fpu_cc = r->pipeline_cached_store = 0;
    return stalls;
}

static CI_FORCE_INLINE void ci_execute_block(struct r4300_core* r4300, unsigned int cache_timing, struct cached_block* known_block)
{
    if (r4300->interp_PC.addr & 3) { pure_interp_execute_one(r4300); return; }
    struct cached_interp_state* state = r4300->cached_interp;
    if (!cache_timing) r4300_pipeline_reset(r4300);
    const uint32_t start_pc = r4300->interp_PC.addr;
    struct cached_block* block = known_block ? known_block : ci_lookup_block(r4300, start_pc);
    uint32_t local_pc = start_pc;
    unsigned int i;

    if (CI_UNLIKELY(block == NULL))
    {
        if (r4300->interp_PC.addr != start_pc) {
            cp0_step_cycles(&r4300->cp0, 1);
            if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0) gen_interrupt(r4300);
            return;
        }
        pure_interp_execute_one(r4300);
        return;
    }

#ifndef WTFN64_DISABLE_RAM_POLL
    if ((block->ram_poll == 1 && interp_ram_poll_advance(r4300, &block->poll)) ||
        (block->ram_poll == 2 && interp_thread_poll_advance(r4300, start_pc))) return;
#endif

    for (i = 0; i < block->count; ++i)
    {
        const uint8_t meta = block->instructions[i].meta;
        const uint8_t fast_op = meta >> CI_META_FAST_SHIFT;
        const uint32_t op = block->instructions[i].op;
        uint32_t status = r4300->cp0.regs[CP0_STATUS_REG];
        if (CI_LIKELY((fast_op != CI_FAST_NONE || (meta & CI_FLAG_RAM_MEMORY)) &&
                      (!(status & CP0_STATUS_MODE_MASK) ||
                       (status & (CP0_STATUS_EXL | CP0_STATUS_ERL))))) {
            /* Arithmetic and guarded RAM accesses cannot observe COUNT,
             * modify mappings/code, trap in kernel mode, or call a device.
             * Keep elapsed clocks
             * local until a handler or the exact interrupt boundary. */
            unsigned int elapsed = 0;
            const unsigned int phase = r4300->cp0.count_phase;
            /* These guarded operations only return elapsed clocks. Keep the
             * unchanged deadline in a register across RAM stores instead of
             * making the compiler reload CP0 through possible pointer aliases. */
            const int64_t deadline = r4300->cp0.cycle_count;
            do {
                const uint32_t fast_instruction = block->instructions[i].op;
                const uint8_t kind = block->instructions[i].meta >> CI_META_FAST_SHIFT;
                unsigned int access_cycles = 0, pipeline_cycles = 0;
                if (!cache_timing && kind == CI_FAST_LUI_PAIR &&
                    deadline + ((elapsed + 2 * (1 + (r4300->compat_issue_clock != 0)) + phase) >> 1) < 0) {
                    r4300->regs[(fast_instruction >> 16) & 31] =
                        interp_lui_pair_value(fast_instruction, block->instructions[i + 1].op);
                    elapsed += 2 * (1 + (r4300->compat_issue_clock != 0));
                    local_pc += 8;
                    i += 2;
                    if (i == block->count) break;
                    continue;
                }
                if (kind == CI_FAST_NONE && !ci_execute_direct_ram(r4300, fast_instruction, block->instructions[i].ram_kind, cache_timing, &access_cycles, &pipeline_cycles)) break;
                if (cache_timing && kind != CI_FAST_NONE)
                    pipeline_cycles = ci_integer_pipeline_cycles(r4300, fast_instruction);
                /* Interlocks overlap the reserved compatibility issue clock. */
                elapsed += block->instructions[i].cycles + access_cycles +
                    r4300_pipeline_issue_cycles(cache_timing || r4300->compat_issue_clock, pipeline_cycles);
                if (cache_timing) elapsed += r4300_fetch_cycles(r4300, local_pc);
                if (kind != CI_FAST_NONE && !ci_execute_hot_fast_op(r4300, kind, fast_instruction))
                    ci_execute_fast_op(r4300, kind, fast_instruction);
                local_pc += 4;
                if (CI_UNLIKELY(deadline + ((elapsed + phase) >> 1) >= 0)) {
                    cp0_step_cycles(&r4300->cp0, elapsed);
                    r4300->interp_PC.addr = local_pc;
                    gen_interrupt(r4300);
                    return;
                }
                ++i;
            } while (i < block->count && ((block->instructions[i].meta >> CI_META_FAST_SHIFT) != CI_FAST_NONE ||
                     (block->instructions[i].meta & CI_FLAG_RAM_MEMORY)));
            if (elapsed) {
                cp0_step_cycles(&r4300->cp0, elapsed);
                --i;
                continue;
            }
        }
        if (!r4300_begin_instruction_cycles(r4300, op, local_pc, block->instructions[i].cycles, cache_timing)) return;

        if (CI_LIKELY(fast_op != CI_FAST_NONE))
        {

            if (!ci_execute_hot_fast_op(r4300, fast_op, op))
                ci_execute_fast_op(r4300, fast_op, op);
            local_pc += 4;
            if (CI_UNLIKELY(r4300->cp0.cycle_count >= 0))
            {
                r4300->interp_PC.addr = local_pc;
                gen_interrupt(r4300);
                return;
            }
            continue;
        }


        r4300->interp_PC.addr = local_pc;

        if (block->has_delay && i + 1 == block->count)
        {
            state->pending_delay_handler = block->delay_handler;
            state->pending_delay_op = block->delay_op;
            state->pending_delay_fast_op = block->delay_fast_op;
            state->pending_delay_pc = local_pc + 4;
            state->pending_delay_valid = 1;
        }

        if (block->instructions[i].fp_kind == 1) pure_interp_fpu_arithmetic(r4300, op);
        else if (block->instructions[i].fp_kind == 2) pure_interp_fpu_compare(r4300, op);
        else if (block->instructions[i].fp_kind == 3) interp_fpu_move(r4300, op);
        else if (!interp_word_memory(r4300, op) && !interp_equal_branch(r4300, op, block->idle_end, ci_execute_one))
            block->handlers[i](r4300, op);
        r4300->regs[0] = 0;
        state->pending_delay_valid = 0;

        if (CI_UNLIKELY(r4300->cp0.cycle_count >= 0))
        {
            gen_interrupt(r4300);
            return;
        }
        local_pc += 4;
        if (CI_UNLIKELY(breakloop || r4300->interp_PC.addr != local_pc))
            return;

        if ((meta & CI_FLAG_MAY_INVALIDATE) != 0 &&
            CI_UNLIKELY(block->epoch != state->epoch ||
                        block->page_generation != state->page_generation[block->physical_page]))
            return;
    }

    r4300->interp_PC.addr = local_pc;
}

void cached_interp_run(struct r4300_core* r4300)
{
    /* Cache emulation is fixed at startup. Specialize outside the dispatch
     * loop so disabled emulation does not carry the I-cache machinery. */
    if (r4300->cache_timing) {
        while (!breakloop) ci_execute_block(r4300, 1, NULL);
    } else {
        while (!breakloop) ci_execute_block(r4300, 0, NULL);
    }
}

#ifdef HAVE_ARI64_X64
/* Precise stubs retain the shared instruction ordering and delay-slot rules.
 * Native guards/timing commit in machine code. A memory stub writes back its
 * prefix before calling here, and reloads its allocation before resuming. */
static int ci_jit_step(struct r4300_core* r, void* opaque, unsigned i, unsigned phase)
{
    struct cached_block* block = opaque;
    struct cached_interp_state* state = r->cached_interp;
    uint32_t pc = block->virtual_pc + i * 4;

    unsigned count = (phase >> 8) & 63;
    if ((phase & 255) == 5) {
#ifdef ARI64_PROFILE
        ++state->profile_regions[count];
#endif
        for (unsigned n = 0; n < count; ++n)
            if (!ci_jit_step(r, block, i + n, 2)) return 0;
        return 1;
    }

    /* A precise branch may already have executed a sequential slot. */
    if (i >= block->count) { ci_execute_one(r); return 0; }
    uint32_t op = block->instructions[i].op;
#ifdef ARI64_PROFILE
    if (phase == 2) {
        ++state->profile_slow[op >> 26];
        if (!(op >> 26)) ++state->profile_special[op & 63];
        if ((op >> 26) == 17) ++state->profile_cop1[(op >> 21) & 31];
    }
#endif
    if (phase != 1) {
        if (!r4300_begin_instruction_cycles(r, op, pc, block->instructions[i].cycles, r->cache_timing)) return 0;
        if (phase == 0) return 1;
        if (phase == 11) {
            r->interp_PC.addr = pc;
            if (!check_cop1_unusable(r)) return 1;
            r->regs[0] = 0; ++state->jit_commits;
            if (r->cp0.cycle_count >= 0) gen_interrupt(r);
            return 0;
        }
        r->interp_PC.addr = pc;
        if (block->has_delay && i + 1 == block->count) {
            state->pending_delay_handler = block->delay_handler;
            state->pending_delay_op = block->delay_op;
            state->pending_delay_fast_op = block->delay_fast_op;
            state->pending_delay_pc = pc + 4;
            state->pending_delay_valid = 1;
        }
        if (block->instructions[i].fp_kind == 1) pure_interp_fpu_arithmetic(r, op);
        else if (block->instructions[i].fp_kind == 2) pure_interp_fpu_compare(r, op);
        else if (block->instructions[i].fp_kind == 3) interp_fpu_move(r, op);
        else if (!interp_word_memory(r, op) && !interp_equal_branch(r, op, block->idle_end, ci_execute_one))
            block->handlers[i](r, op);
        state->pending_delay_valid = 0;
    } else r->interp_PC.addr = pc + 4;
    r->regs[0] = 0;
    ++state->jit_commits;
    if (!r->delay_slot && r->cp0.cycle_count >= 0) {
        gen_interrupt(r);
        return 0;
    }
    if (breakloop || r->interp_PC.addr != pc + 4) return 0;
    return block->epoch == state->epoch &&
           block->page_generation == state->page_generation[block->physical_page] &&
           ci_mapping_matches(r, block);
}

static ari64_x64_entry ci_jit_link(struct r4300_core* r)
{
    if (breakloop || r->delay_slot || r->stop || (r->interp_PC.addr & 3)) return NULL;
    struct cached_interp_state* state = r->cached_interp;
    uint32_t pc = r->interp_PC.addr;
    /* Do not fetch, compile, run a polling shortcut or recycle an arena from
     * a generated frame. A miss returns to the normal CPU dispatcher. */
    struct cached_block* next = state->recent_blocks[ci_hash_pc(pc) & state->set_mask];
    if (!next || !next->valid || next->virtual_pc != pc || next->ram_poll ||
        next->epoch != state->epoch ||
        next->page_generation != state->page_generation[next->physical_page] ||
        !ci_mapping_matches(r, next) ||
        !ari64_x64_entry_valid(state->jit, next->native_entry, next->native_generation)) return NULL;
    struct cached_block* previous = state->previous_block;
    /* Fixed KSEG edges need no mapping load. Mapped edges guard the live LUT
     * entry in assembly; cache attributes remain live in the target body. */
    if (previous) {
        unsigned slot = previous->native_links[0].pc == pc ? 0 : previous->native_links[1].pc == pc ? 1 : previous->native_link_replace++ & 1;
        struct ari64_x64_link_cache* cache = &previous->native_links[slot];
        cache->pc = pc; cache->epoch = state->epoch;
        cache->page_generation = state->page_generation[next->physical_page];
        cache->expected_entry = next->native_entry; cache->body = next->native_chain_entry;
        cache->current_entry = &next->native_entry;
        cache->current_page_generation = &state->page_generation[next->physical_page];
        cache->current_mapping = (pc & UINT32_C(0xc0000000)) == UINT32_C(0x80000000) ? NULL : &r->cp0.tlb.LUT_r[pc >> CI_PAGE_SHIFT];
        cache->mapping = cache->current_mapping ? *cache->current_mapping : 0;
        cache->current_segment_generation = ari64_x64_entry_generation_ptr(state->jit,next->native_entry);
        cache->segment_generation = next->native_generation;
        cache->target_block = next;
        cache->profile_entries = NULL;
#ifdef ARI64_PROFILE
        cache->profile_entries = &next->profile_entries;
#endif
    }
    state->previous_block = next;
    ++state->jit_entries;
    ++state->jit_links;
#ifdef ARI64_PROFILE
    ++next->profile_entries;
#endif
    return next->native_chain_entry;
}

#ifdef ARI64_PROFILE
static int ci_jit_fpu_prepare(struct r4300_core* r, uint32_t op, uint64_t inputs[3], uint64_t* commits)
{
    ++r->cached_interp->profile_fpu[op & 63];
    return pure_interp_jit_fpu_prepare(r,op,inputs,commits);
}
#else
#define ci_jit_fpu_prepare pure_interp_jit_fpu_prepare
#endif
static int ci_jit_fpu_finish(struct r4300_core* r, uint32_t op, uint64_t bits, unsigned flags)
{
    uint32_t pc = r->interp_PC.addr;
    if ((op & 63) >= 48) pure_interp_fpu_compare_finish(r, (int)(bits & 1));
    else pure_interp_fpu_finish(r, op, bits, flags);
    r->regs[0] = 0;
    ++r->cached_interp->jit_commits;
    ++r->cached_interp->jit_fast_commits;
    if (r->cp0.cycle_count >= 0) { gen_interrupt(r); return 0; }
    return !breakloop && r->interp_PC.addr == pc + 4;
}

static void ci_execute_jit(struct r4300_core* r, int one)
{
    /* A delay slot remains one precise instruction, never a nested native
     * compilation that could recycle the caller's executable arena. */
    if (r->delay_slot) { ci_execute_one(r); return; }
    if (r->interp_PC.addr & 3) { pure_interp_execute_one(r); return; }
    struct cached_interp_state* state = r->cached_interp;
    uint32_t pc = r->interp_PC.addr;
    struct cached_block* block = ci_lookup_block(r, pc);
    if (!block) {
        if (r->interp_PC.addr != pc) {
            cp0_step_cycles(&r->cp0, 1);
            if (r->cp0.cycle_count >= 0) gen_interrupt(r);
        } else pure_interp_execute_one(r);
        return;
    }
#ifndef WTFN64_DISABLE_RAM_POLL
    if (!one && ((block->ram_poll == 1 && interp_ram_poll_advance(r, &block->poll)) ||
        (block->ram_poll == 2 && interp_thread_poll_advance(r, pc)))) return;
#endif
    if (!one && !block->native_candidate) {
        uint32_t ops[CI_BLOCK_INSTRUCTIONS];
        for (unsigned i = 0; i < block->count; ++i) ops[i] = block->instructions[i].op;
        block->native_candidate = ari64_x64_has_native(ops,block->count,block->delay_op,block->has_delay,block->idle_end) ? 1 : 2;
    }

    ari64_x64_entry* entry = one ? &block->native_one_entry : &block->native_entry;
    uint64_t* generation = one ? &block->native_one_generation : &block->native_generation;
    if (!ari64_x64_entry_valid(state->jit, *entry, *generation)) {
        /* Ari64-style hot block compilation: execute cold decoded blocks
         * before spending code space and protection syscalls on them. */
        /* Short precise-only wrappers pay off by retaining native tail chains
         * across unsafe delay slots. Compile those sooner than cold regions. */
        if (!one && block->heat < (block->native_candidate == 2 ? 3 : 8)) {
            ++block->heat;
            ci_execute_block(r, r->cache_timing != 0, block);
            return;
        }
        ++state->jit_compile_misses[*entry != NULL];
        uint32_t ops[CI_BLOCK_INSTRUCTIONS];
        unsigned count = one ? 1 : block->count;
        for (unsigned i = 0; i < count; ++i) ops[i] = block->instructions[i].op;
        struct ari64_x64_config config = {block->virtual_pc, block->physical_page, r->cache_timing, one ? NULL : ci_jit_link,
            block->delay_op, !one && block->has_delay, block->idle_end, &state->jit_commits, &state->jit_fast_commits,
            ci_jit_fpu_prepare, ci_jit_fpu_finish, one ? NULL : &block->native_chain_entry};
        config.compat_issue_clock = r->compat_issue_clock;
        config.links = one ? NULL : block->native_links;
        config.previous_block = &state->previous_block; config.epoch = &state->epoch;
        config.entries = &state->jit_entries; config.linked_entries = &state->jit_links; config.direct_links = &state->jit_direct_links;
#ifdef ARI64_PROFILE
        config.profile = 1;
#endif
        *entry = ari64_x64_compile_optimized(state->jit, block, ops, count, ci_jit_step, &config);
        *generation = ari64_x64_generation(state->jit);
        if (!*entry) {
            DebugMessage(M64MSG_ERROR, "Ari64 x64 executable code allocation/protection failed; stopping CPU");
            r->stop = 1;
            breakloop = true;
            return;
        }
    }
    ++state->jit_entries;
#ifdef ARI64_PROFILE
    ++block->profile_entries;
#endif
    (*entry)(r);
}

void cached_dynarec_execute_one(struct r4300_core* r) { ci_execute_jit(r, 1); }
void cached_dynarec_run(struct r4300_core* r) { while (!breakloop) ci_execute_jit(r, 0); }
void cached_dynarec_stats(struct r4300_core* r, uint64_t stats[4])
{
    struct cached_interp_state* s = r->cached_interp;
    ari64_x64_stats(s ? s->jit : NULL, &stats[0], &stats[1]);
    stats[2] = s ? s->jit_entries : 0;
    stats[3] = s ? s->jit_commits : 0;
    if (s) DebugMessage(M64MSG_INFO, "Ari64 x64 cache: new=%llu recycled=%llu verified=%llu changed=%llu replaced=%llu",
        (unsigned long long)s->jit_compile_misses[0], (unsigned long long)s->jit_compile_misses[1],
        (unsigned long long)s->jit_verifications[0], (unsigned long long)s->jit_verifications[1], (unsigned long long)s->jit_replacements);
    if (s) DebugMessage(M64MSG_INFO,"Ari64 x64 links: direct=%llu",(unsigned long long)s->jit_direct_links);
#ifdef ARI64_PROFILE
    if (s) {
        for (unsigned i = 0; i < 64; ++i)
            if (s->profile_slow[i] || s->profile_special[i] || s->profile_cop1[i] || s->profile_fpu[i])
                DebugMessage(M64MSG_INFO,"Ari64 profile opcode %u: slow=%llu special=%llu cop1rs=%llu fpu=%llu", i,
                    (unsigned long long)s->profile_slow[i], (unsigned long long)s->profile_special[i],
                    (unsigned long long)s->profile_cop1[i], (unsigned long long)s->profile_fpu[i]);
        for (unsigned i = 1; i <= 32; ++i)
            if (s->profile_regions[i]) DebugMessage(M64MSG_INFO,"Ari64 profile guard count %u: %llu",i,(unsigned long long)s->profile_regions[i]);
        for (unsigned rank = 0; rank < 24; ++rank) {
            struct cached_block* best = NULL;
            for (unsigned set = 0; set <= s->set_mask; ++set)
                for (unsigned way = 0; way < CI_WAYS; ++way) {
                    struct cached_block* b = &s->sets[set].ways[way];
                    if (b->valid && b->profile_entries && (!best || b->profile_entries > best->profile_entries)) best = b;
                }
            if (!best) break;
            DebugMessage(M64MSG_INFO,"Ari64 profile block %08x count=%u entries=%llu",best->virtual_pc,best->count,(unsigned long long)best->profile_entries);
            for (unsigned n = 0; n < best->count; ++n)
                DebugMessage(M64MSG_INFO,"Ari64 profile instruction %08x %08x",best->virtual_pc + 4*n,best->instructions[n].op);
            best->profile_entries = 0;
        }
    }
#endif
}
void cached_dynarec_code_stats(struct r4300_core* r, uint64_t stats[3])
{
    ari64_x64_code_stats(r->cached_interp ? r->cached_interp->jit : NULL, stats);
}
void cached_dynarec_optimization_stats(struct r4300_core* r, uint64_t stats[2])
{
    struct cached_interp_state* s = r->cached_interp;
    stats[0] = s ? s->jit_fast_commits : 0;
    stats[1] = s ? s->jit_links : 0;
}
uint64_t cached_dynarec_direct_links(struct r4300_core* r)
{
    return r->cached_interp ? r->cached_interp->jit_direct_links : 0;
}
#endif
