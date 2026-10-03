

#include "cached_interp.h"
#include "timing.h"

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


enum
{
    CI_PAGE_SHIFT = 12,
    CI_PAGE_SIZE = 1 << CI_PAGE_SHIFT,
    CI_PHYS_SIZE = 0x20000000,
    CI_PHYS_PAGE_COUNT = CI_PHYS_SIZE >> CI_PAGE_SHIFT,
    CI_BLOCK_INSTRUCTIONS = 32,
    CI_SET_COUNT = 2048,
    CI_WAYS = 2
};

enum
{
    CI_FLAG_MAY_INVALIDATE = 1u << 0,
    CI_META_FAST_SHIFT = 1
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
    CI_FAST_DSRLV
};

struct cached_block
{
    r4300_interp_handler handlers[CI_BLOCK_INSTRUCTIONS];
    uint32_t ops[CI_BLOCK_INSTRUCTIONS];
    uint8_t meta[CI_BLOCK_INSTRUCTIONS];
    r4300_interp_handler delay_handler;
    uint32_t delay_op;
    uint8_t delay_fast_op;
    uint8_t has_delay;
    uint32_t virtual_pc;
    uint32_t physical_page;
    uint32_t page_generation;
    uint32_t epoch;
    uint8_t count;
    uint8_t valid;
};

struct cached_set
{
    struct cached_block ways[CI_WAYS];
    uint8_t next_replace;
};

struct cached_interp_state
{
    struct cached_set sets[CI_SET_COUNT];
    uint32_t page_generation[CI_PHYS_PAGE_COUNT];
    uint64_t page_cached[(CI_PHYS_PAGE_COUNT + 63) / 64];
    r4300_interp_handler pending_delay_handler;
    uint32_t pending_delay_op;
    uint32_t pending_delay_pc;
    uint8_t pending_delay_fast_op;
    uint8_t pending_delay_valid;
    uint32_t epoch;
    struct cached_block* recent_blocks[16];
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
    return x & (CI_SET_COUNT - 1);
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

static CI_FORCE_INLINE void ci_execute_fast_op(struct r4300_core* r4300, uint8_t fast_op, uint32_t op)
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
    struct cached_set* set = &state->sets[ci_hash_pc(pc)];
    struct cached_block* block = &set->ways[set->next_replace];
    uint32_t* op_ptr;
    unsigned int max_count;
    unsigned int i;

    set->next_replace ^= 1;
    block->valid = 0;
    block->count = 0;
    block->has_delay = 0;

    op_ptr = fast_mem_access(r4300, pc);
    if (CI_UNLIKELY(op_ptr == NULL))
        return NULL;

    max_count = (CI_PAGE_SIZE - (pc & (CI_PAGE_SIZE - 1))) >> 2;
    if (max_count > CI_BLOCK_INSTRUCTIONS)
        max_count = CI_BLOCK_INSTRUCTIONS;

    for (i = 0; i < max_count; ++i)
    {
        const uint32_t instruction_pc = pc + (i << 2);
        const uint32_t op = op_ptr[i];
        const uint32_t next_op = (i + 1 < max_count) ? op_ptr[i + 1] : UINT32_C(0xffffffff);
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
        block->ops[i] = op;
        block->meta[i] = (uint8_t)(
            (ci_classify_fast_op(opcode, op) << CI_META_FAST_SHIFT) |
            (ci_may_invalidate_code_opcode(opcode) ? CI_FLAG_MAY_INVALIDATE : 0));
        block->count = (uint8_t)(i + 1);

        if (ci_is_block_end_opcode(opcode))
        {
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

    block->virtual_pc = pc;
    block->physical_page = physical_page;
    block->page_generation = state->page_generation[physical_page];
    block->epoch = state->epoch;
    block->valid = 1;
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

static struct cached_block* ci_lookup_block(struct r4300_core* r4300, uint32_t pc)
{
    struct cached_interp_state* state = r4300->cached_interp;
    struct cached_set* set;
    struct cached_block* block;
    uint32_t physical_page;
    const unsigned int recent_index = (pc >> 2) & 15;

    block = state->recent_blocks[recent_index];
    if (CI_LIKELY(block != NULL &&
                  block->valid &&
                  block->virtual_pc == pc &&
                  block->epoch == state->epoch &&
                  block->page_generation == state->page_generation[block->physical_page]))
        return block;

    if (CI_UNLIKELY(!ci_translate_exec_page(r4300, pc, &physical_page)))
        return NULL;

    set = &state->sets[ci_hash_pc(pc)];

    block = &set->ways[0];
    if (CI_LIKELY(ci_block_matches(block, state, pc, physical_page)))
    {
        set->next_replace = 1;
        state->recent_blocks[recent_index] = block;
        return block;
    }

    block = &set->ways[1];
    if (CI_LIKELY(ci_block_matches(block, state, pc, physical_page)))
    {
        set->next_replace = 0;
        state->recent_blocks[recent_index] = block;
        return block;
    }

    block = ci_build_block(r4300, pc, physical_page);
    if (block != NULL)
        state->recent_blocks[recent_index] = block;
    return block;
}

static void ci_bump_page(struct cached_interp_state* state, uint32_t physical_page)
{
    if (!ci_page_is_cached(state, physical_page))
        return;

    ci_page_clear_cached(state, physical_page);
    if (++state->page_generation[physical_page] == 0)
    {
        memset(state->sets, 0, sizeof(state->sets));
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

    r4300->cached_interp->epoch = 1;
    return 1;
}

void cached_interp_destroy(struct r4300_core* r4300)
{
    free(r4300->cached_interp);
    r4300->cached_interp = NULL;
    r4300->execute_one = NULL;
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
            memset(state, 0, sizeof(*state));
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

    return 0;
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
        ci_execute_fast_op(r4300, fast_op, op);
        r4300->interp_PC.addr = pc + 4;
    }
    else
    {
        handler(r4300, op);
    }
    r4300->regs[0] = 0;
    if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0)
        gen_interrupt(r4300);
}

void cached_interp_execute_one(struct r4300_core* r4300)
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
            pure_interp_execute_one(r4300);
            return;
        }

        ci_execute_cached_instruction(r4300, block->handlers[0], block->ops[0],
                                      block->meta[0] >> CI_META_FAST_SHIFT, pc);
    }
}

static void ci_execute_block(struct r4300_core* r4300)
{
    if (r4300->interp_PC.addr & 3) { pure_interp_execute_one(r4300); return; }
    struct cached_interp_state* state = r4300->cached_interp;
    const uint32_t start_pc = r4300->interp_PC.addr;
    struct cached_block* block = ci_lookup_block(r4300, start_pc);
    uint32_t local_pc = start_pc;
    unsigned int i;

    if (CI_UNLIKELY(block == NULL))
    {
        pure_interp_execute_one(r4300);
        return;
    }

    for (i = 0; i < block->count; ++i)
    {
        const uint8_t meta = block->meta[i];
        const uint8_t fast_op = meta >> CI_META_FAST_SHIFT;
        const uint32_t op = block->ops[i];
        if (!r4300_begin_instruction(r4300, op, local_pc)) return;

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
    while (!breakloop)
        ci_execute_block(r4300);
}
