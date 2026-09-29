/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pure_interp.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2015 Nebuleon <nebuleon.fumika@gmail.com>               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "pure_interp.h"
#include "cached_interp.h"
#include "idec.h"
#include "timing.h"

#include <stdint.h>
#include <stdbool.h>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "device/r4300/r4300_core.h"
#include "osal/preproc.h"

#if (defined(__GNUC__) || defined(__clang__)) && !defined(M64P_DISABLE_COMPUTED_GOTO)
#define M64P_USE_COMPUTED_GOTO 1
#define M64P_LIKELY(x) __builtin_expect(!!(x), 1)
#define M64P_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define M64P_UNREACHABLE() __builtin_unreachable()
#define M64P_HOT __attribute__((hot))
#else
#define M64P_USE_COMPUTED_GOTO 0
#define M64P_LIKELY(x) (x)
#define M64P_UNLIKELY(x) (x)
#if defined(_MSC_VER)
#define M64P_UNREACHABLE() __assume(0)
#else
#define M64P_UNREACHABLE() ((void)0)
#endif
#define M64P_HOT
#endif

#if defined(_MSC_VER)
#define M64P_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define M64P_FORCE_INLINE inline __attribute__((always_inline))
#else
#define M64P_FORCE_INLINE inline
#endif

#ifndef M64P_INTERP_BRANCHLESS
#define M64P_INTERP_BRANCHLESS 1
#endif

static M64P_FORCE_INLINE uint32_t interp_select_u32(unsigned int condition,
                                                    uint32_t true_value,
                                                    uint32_t false_value)
{
#if M64P_INTERP_BRANCHLESS
    const uint32_t mask = UINT32_C(0) - (uint32_t)(condition != 0);
    return (true_value & mask) | (false_value & ~mask);
#else
    return condition ? true_value : false_value;
#endif
}

#if M64P_USE_COMPUTED_GOTO
#define M64P_MAJOR_CASE(n) pi_major_##n: ;
#define M64P_MAJOR_DEFAULT pi_major_default: ;
#define M64P_SPECIAL_CASE(n) pi_special_##n: ;
#define M64P_SPECIAL_DEFAULT pi_special_default: ;
#define M64P_REGIMM_CASE(n) pi_regimm_##n: ;
#define M64P_REGIMM_DEFAULT pi_regimm_default: ;
#define M64P_COP0_CASE(n) pi_cop0_##n: ;
#define M64P_COP0_DEFAULT pi_cop0_default: ;
#define M64P_COP1_CASE(n) pi_cop1_##n: ;
#define M64P_COP1_DEFAULT pi_cop1_default: ;
#define M64P_CP1S_CASE(n) pi_cp1s_##n: ;
#define M64P_CP1S_DEFAULT pi_cp1s_default: ;
#define M64P_CP1D_CASE(n) pi_cp1d_##n: ;
#define M64P_CP1D_DEFAULT pi_cp1d_default: ;
#else
#define M64P_MAJOR_CASE(n) case n: ;
#define M64P_MAJOR_DEFAULT default: ;
#define M64P_SPECIAL_CASE(n) case n: ;
#define M64P_SPECIAL_DEFAULT default: ;
#define M64P_REGIMM_CASE(n) case n: ;
#define M64P_REGIMM_DEFAULT default: ;
#define M64P_COP0_CASE(n) case n: ;
#define M64P_COP0_DEFAULT default: ;
#define M64P_COP1_CASE(n) case n: ;
#define M64P_COP1_DEFAULT default: ;
#define M64P_CP1S_CASE(n) case n: ;
#define M64P_CP1S_DEFAULT default: ;
#define M64P_CP1D_CASE(n) case n: ;
#define M64P_CP1D_DEFAULT default: ;
#endif

static void InterpretOpcode(struct r4300_core* r4300, bool continuous) M64P_HOT;

#define DECLARE_R4300
#define PCADDR r4300->interp_PC.addr
#define ADD_TO_PC(x) r4300->interp_PC.addr += (x) * 4;
#define DECLARE_INSTRUCTION(name) static void name(struct r4300_core* r4300, uint32_t op)
#define DECLARE_JUMP(name, destination, condition, link, likely, cop1) \
    static void name(struct r4300_core* r4300, uint32_t op) \
    { \
        int take_jump = (condition); \
        const uint32_t jump_target = (destination); \
        int64_t* link_register = (link); \
        if (cop1 && check_cop1_unusable(r4300)) return; \
        if (cop1) r4300->cp1.fcr31 &= ~UINT32_C(0x3f000); \
        if (link_register != &r4300->regs[0]) \
        { \
            *link_register = SE32(r4300->interp_PC.addr + 8); \
            if ((op >> 26) == 1 && ((op >> 16) & 31) != 17) take_jump = (condition); \
        } \
        if (!likely || take_jump) \
        { \
            r4300->interp_PC.addr += 4; \
            r4300->delay_slot = 1; \
            r4300->execute_one(r4300); \
            cp0_update_count(r4300); \
            r4300->delay_slot = 0; \
            r4300->interp_PC.addr = interp_select_u32( \
                (unsigned int)take_jump & (unsigned int)(r4300->skip_jump == 0), \
                jump_target, r4300->interp_PC.addr); \
        } \
        else \
        { \
            r4300->interp_PC.addr += 8; \
            cp0_update_count(r4300); \
        } \
        r4300->cp0.last_addr = r4300->interp_PC.addr; \
        if (r4300->cp0.cycle_count >= 0) gen_interrupt(r4300); \
    } \
    static void name##_IDLE(struct r4300_core* r4300, uint32_t op) \
    { \
        uint32_t* cp0_regs = r4300->cp0.regs; \
        int* cp0_cycle_count = &r4300->cp0.cycle_count; \
        const int take_jump = (condition); \
        if (cop1 && check_cop1_unusable(r4300)) return; \
        if ((op >> 26) == 1 && ((op >> 16) & 16) && ((op >> 21) & 31) == 31) \
        { name(r4300, op); return; } \
        if (take_jump) \
        { \
            cp0_update_count(r4300); \
            if (*cp0_cycle_count < 0) \
            { \
                cp0_regs[CP0_COUNT_REG] -= *cp0_cycle_count; \
                r4300->cp0.count_phase = 0; \
                *cp0_cycle_count = 0; \
            } \
        } \
        name(r4300, op); \
    }

#define RD_OF(op)      (((op) >> 11) & 0x1F)
#define RS_OF(op)      (((op) >> 21) & 0x1F)
#define RT_OF(op)      (((op) >> 16) & 0x1F)
#define SA_OF(op)      (((op) >>  6) & 0x1F)
#define IMM16S_OF(op)  ((int16_t) (op))
#define IMM16U_OF(op)  ((uint16_t) (op))
#define FD_OF(op)      (((op) >>  6) & 0x1F)
#define FS_OF(op)      (((op) >> 11) & 0x1F)
#define FT_OF(op)      (((op) >> 16) & 0x1F)
#define JUMP_OF(op)    ((op) & UINT32_C(0x3FFFFFF))


#define IS_RELATIVE_IDLE_LOOP(r4300, op, addr) \
    (IMM16S_OF(op) == -1 && *interp_fast_mem_access((r4300), (addr) + 4) == 0)


#define IS_ABSOLUTE_IDLE_LOOP(r4300, op, addr) \
    (JUMP_OF(op) == ((addr) & UINT32_C(0x0FFFFFFF)) >> 2 \
     && ((addr) & UINT32_C(0x0FFFFFFF)) != UINT32_C(0x0FFFFFFC) \
     && *interp_fast_mem_access((r4300), (addr) + 4) == 0)

/* These macros parse opcode fields. */
#define rrt r4300->regs[RT_OF(op)]
#define rrd r4300->regs[RD_OF(op)]
#define rfs FS_OF(op)
#define rrs r4300->regs[RS_OF(op)]
#define rsa SA_OF(op)
#define irt r4300->regs[RT_OF(op)]
#define ioffset IMM16S_OF(op)
#define iimmediate IMM16S_OF(op)
#define irs r4300->regs[RS_OF(op)]
#define ibase r4300->regs[RS_OF(op)]
#define jinst_index JUMP_OF(op)
#define lfbase RS_OF(op)
#define lfft FT_OF(op)
#define lfoffset IMM16S_OF(op)
#define cfft FT_OF(op)
#define cffs FS_OF(op)
#define cffd FD_OF(op)


#define rrt32 ((int32_t)(uint32_t)r4300->regs[RT_OF(op)])
#define rrd32 ((int32_t)(uint32_t)r4300->regs[RD_OF(op)])
#define rrs32 ((int32_t)(uint32_t)r4300->regs[RS_OF(op)])
#define irs32 ((int32_t)(uint32_t)r4300->regs[RS_OF(op)])
#define irt32 ((int32_t)(uint32_t)r4300->regs[RT_OF(op)])

// two functions are defined from the macros above but never used
// these prototype declarations will prevent a warning
#if defined(__GNUC__)
  static void JR_IDLE(struct r4300_core*, uint32_t) __attribute__((used));
  static void JALR_IDLE(struct r4300_core*, uint32_t) __attribute__((used));
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mips_instructions.def                                   *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Before #including this file the following macros must be defined:
 *
 * instruction operands accessor macros:
 * rrt, rrd, rfs, rrs, rsa,
 * rrt32, rrd32, rrs32, irs32, irt32,
 * irt, irs, ibase, ioffset, iimmediate,
 * jinst_index,
 * lfbase, lfft, lfoffset,
 * cfft, cffs, cffd
 *
 * DECLARE_R4300: An optionnal way of declaring teh r4300 pointer used
 * in instructions definitions
 * PCADDR: Program counter (memory address of the current instruction).
 *
 * ADD_TO_PC(x): Increment the program counter in 'x' instructions.
 * This is only used for small changes to PC, so the new program counter
 * is guaranteed to fall in the current cached interpreter or dynarec block.
 *
 * DECLARE_INSTRUCTION(name)
 * Declares an instruction function which is not a jump.
 * Followed by a block of code.
 *
 * DECLARE_JUMP(name, destination, condition, link, likely, cop1)
 * name is the name of the jump or branch instruction.
 * destination is the destination memory address of the jump.
 * If condition is nonzero, the jump is taken.
 * link is a pointer to a variable where (PC+8) is written unconditionally.
 *     To avoid linking, pass &reg[0]
 * If likely is nonzero, the delay slot is only executed if the jump is taken.
 * If cop1 is nonzero, a COP1 unusable check will be done.
 */

#include <math.h>
#define FCR31_CMP_BIT UINT32_C(0x800000)
#include "r4300_core.h"
#include "device/memory.h"
#include "device/rcp/mi_controller.h"
#include "device/rdram.h"
#include "osal/preproc.h"

#include <inttypes.h>
#include <stdint.h>

/* Fast instruction fetch for the common unmapped KSEG0/KSEG1 path.
 * Keep the generic helper for TLB-mapped addresses and exceptions. */
static M64P_FORCE_INLINE uint32_t* interp_fast_mem_access(struct r4300_core* r4300, uint32_t address)
{
    if (M64P_LIKELY((address & UINT32_C(0xc0000000)) == UINT32_C(0x80000000)))
    {
        address &= UINT32_C(0x1ffffffc);
        return (uint32_t*)((uint8_t*)r4300->mem->base + address);
    }

    return fast_mem_access(r4300, address);
}

/* Assists unaligned memory accessors with making masks to preserve or apply
 * bits in registers and memory.
 *
 * BITS_BELOW_MASK32 and BITS_BELOW_MASK64 make masks where bits 0 to (x - 1)
 * are set.
 *
 * BITS_ABOVE_MASK32 makes masks where bits x to 31 are set.
 * BITS_ABOVE_MASK64 makes masks where bits x to 63 are set.
 *
 * e.g. x = 8
 * 0000 0000 0000 0000 0000 0000 1111 1111 <- BITS_BELOW_MASK32(8)
 * 1111 1111 1111 1111 1111 1111 0000 0000 <- BITS_ABOVE_MASK32(8)
 *
 * Giving a negative value or one that is >= the bit count of the mask results
 * in undefined behavior.
 */

#define BITS_BELOW_MASK32(x) ((UINT32_C(1) << (x)) - 1)
#define BITS_ABOVE_MASK32(x) (~(BITS_BELOW_MASK32((x))))

#define BITS_BELOW_MASK64(x) ((UINT64_C(1) << (x)) - 1)
#define BITS_ABOVE_MASK64(x) (~(BITS_BELOW_MASK64((x))))


static M64P_FORCE_INLINE unsigned int bshift(uint32_t address)
{
    return ((address & 3) ^ 3) << 3;
}

static M64P_FORCE_INLINE unsigned int hshift(uint32_t address)
{
    return ((address & 2) ^ 2) << 3;
}


/* M64P Pseudo instructions */

DECLARE_INSTRUCTION(NI)
{
    if ((op >> 26) == 0x11) {
        if (check_cop1_unusable(r4300)) return;
        r4300->cp1.fcr31 = (r4300->cp1.fcr31 & ~UINT32_C(0x3f000)) | UINT32_C(0x20000);
        r4300->cp0.regs[CP0_CAUSE_REG] =
            (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_FPE;
        exception_general(r4300);
        return;
    }
    r4300->cp0.regs[CP0_CAUSE_REG] =
        (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_RI;
    exception_general(r4300);
}

DECLARE_INSTRUCTION(BREAK)
{
    (void)op;
    r4300->cp0.regs[CP0_CAUSE_REG] =
        (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_BP;
    exception_general(r4300);
}

/* Reserved */

/* Load instructions */

static int check_alignment(struct r4300_core* r4300, uint32_t address, unsigned int mask, int store)
{
    if ((address & mask) == 0) return 0;
    r4300->cp0.regs[CP0_BADVADDR_REG] = address;
    r4300->cp0.regs_hi[CP0_BADVADDR_REG] = (int32_t)address < 0 ? UINT32_MAX : 0;
    r4300->cp0.regs[CP0_CAUSE_REG] =
        (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) |
        (store ? CP0_CAUSE_EXCCODE_ADES : CP0_CAUSE_EXCCODE_ADEL);
    exception_general(r4300);
    return 1;
}

DECLARE_INSTRUCTION(LLD)
{
    uint32_t address = (uint32_t)irs + (uint32_t)iimmediate;
    uint64_t value;
    if (check_alignment(r4300, address, 7, 0)) return;
    ADD_TO_PC(1);
    if (r4300_read_aligned_dword(r4300, address, &value)) {
        irt = (int64_t)value;
        r4300->llbit = 1;
        uint32_t physical = (address & UINT32_C(0xc0000000)) == UINT32_C(0x80000000)
            ? address : virtual_to_physical_address(r4300, address, 0);
        r4300->cp0.regs[CP0_LLADDR_REG] = (physical & UINT32_C(0x1fffffff)) >> 4;
    }
}

DECLARE_INSTRUCTION(SCD)
{
    uint32_t address = (uint32_t)irs + (uint32_t)iimmediate;
    if (r4300->llbit && check_alignment(r4300, address, 7, 1)) return;
    ADD_TO_PC(1);
    if (r4300->llbit) irt = r4300_write_aligned_dword(r4300, address, (uint64_t)irt, UINT64_MAX);
    else irt = 0;
}

DECLARE_INSTRUCTION(LB)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);
    uint32_t value;
    unsigned int shift = bshift(lsaddr);

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = SE8((value >> shift) & 0xff);
    }
}

DECLARE_INSTRUCTION(LBU)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);
    uint32_t value;
    unsigned int shift = bshift(lsaddr);

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = (value >> shift) & 0xff;
    }
}

DECLARE_INSTRUCTION(LH)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 1, 0)) return;
    ADD_TO_PC(1);
    uint32_t value;
    unsigned int shift = hshift(lsaddr);

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = SE16((value >> shift) & 0xffff);
    }
}

DECLARE_INSTRUCTION(LHU)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 1, 0)) return;
    ADD_TO_PC(1);
    uint32_t value;
    unsigned int shift = hshift(lsaddr);

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = (value >> shift) & 0xffff;
    }
}

DECLARE_INSTRUCTION(LL)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 3, 0)) return;
    ADD_TO_PC(1);
    uint32_t value;

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = SE32(value);
        r4300->llbit = 1;
        uint32_t physical = (lsaddr & UINT32_C(0xc0000000)) == UINT32_C(0x80000000)
            ? lsaddr : virtual_to_physical_address(r4300, lsaddr, 0);
        r4300->cp0.regs[CP0_LLADDR_REG] = (physical & UINT32_C(0x1fffffff)) >> 4;
    }
}

DECLARE_INSTRUCTION(LW)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 3, 0)) return;
    ADD_TO_PC(1);
    uint32_t value;

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = SE32(value);
    }
}

DECLARE_INSTRUCTION(LWU)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 3, 0)) return;
    ADD_TO_PC(1);

    uint32_t value;

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = value;
    }
}

DECLARE_INSTRUCTION(LWL)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 3);
    unsigned int shift = 8 * n;
    uint32_t mask = BITS_BELOW_MASK32(8 * n);
    uint32_t value;

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        *lsrtp = SE32(((uint32_t)*lsrtp & mask) | ((uint32_t)value << shift));
    }
}

DECLARE_INSTRUCTION(LWR)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 3);
    unsigned int shift = 8 * (3 - n);
    uint32_t mask = (n == 3)
        ? UINT32_C(0)
        : BITS_ABOVE_MASK32(8 * (n + 1));
    uint32_t value;

    if (r4300_read_aligned_word(r4300, lsaddr, &value)) {
        uint32_t merged = ((uint32_t)*lsrtp & mask) | (value >> shift);
        *lsrtp = n == 3 ? SE32(merged)
            : (int64_t)(((uint64_t)*lsrtp & UINT64_C(0xffffffff00000000)) | merged);
    }
}

DECLARE_INSTRUCTION(LD)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 7, 0)) return;
    ADD_TO_PC(1);

    r4300_read_aligned_dword(r4300, lsaddr, (uint64_t*)lsrtp);
}

DECLARE_INSTRUCTION(LDL)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 7);
    unsigned int shift = 8 * n;
    uint64_t mask = BITS_BELOW_MASK64(8 * n);
    uint64_t value;

    if (r4300_read_aligned_dword(r4300, lsaddr & ~UINT32_C(7), &value)) {
        *lsrtp = ((uint64_t)*lsrtp & mask) | (value << shift);
    }
}

DECLARE_INSTRUCTION(LDR)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 7);
    unsigned int shift = 8 * (7 - n);
    uint64_t mask = (n == 7)
        ? UINT64_C(0)
        : BITS_ABOVE_MASK64(8 * (n + 1));
    uint64_t value;

    if (r4300_read_aligned_dword(r4300, lsaddr & ~UINT32_C(7), &value)) {
        *lsrtp = ((uint64_t)*lsrtp & mask) | (value >> shift);
    }
}

/* Store instructions */

DECLARE_INSTRUCTION(SB)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);
    unsigned int shift = bshift(lsaddr);

    r4300_write_aligned_word(r4300, lsaddr, (uint32_t)*lsrtp << shift, UINT32_C(0xff) << shift);
}

DECLARE_INSTRUCTION(SH)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 1, 1)) return;
    ADD_TO_PC(1);
    unsigned int shift = hshift(lsaddr);

    r4300_write_aligned_word(r4300, lsaddr, (uint32_t)*lsrtp << shift, UINT32_C(0xffff) << shift);
}

DECLARE_INSTRUCTION(SC)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (r4300->llbit && check_alignment(r4300, lsaddr, 3, 1)) return;
    ADD_TO_PC(1);

    if (r4300->llbit)
    {
        *lsrtp = r4300_write_aligned_word(r4300, lsaddr, (uint32_t)*lsrtp, ~UINT32_C(0));
    }
    else
    {
        *lsrtp = 0;
    }
}

DECLARE_INSTRUCTION(SW)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 3, 1)) return;
    ADD_TO_PC(1);

    r4300_write_aligned_word(r4300, lsaddr, (uint32_t)*lsrtp, ~UINT32_C(0));
}

DECLARE_INSTRUCTION(SWL)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 3);
    unsigned int shift = 8 * n;
    uint32_t mask = (n == 0)
        ? ~UINT32_C(0)
        : BITS_BELOW_MASK32(8 * (4 - n));
    uint32_t value = (uint32_t)*lsrtp;

    r4300_write_aligned_word(r4300, lsaddr & ~UINT32_C(0x3), value >> shift, mask);
}

DECLARE_INSTRUCTION(SWR)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 3);
    unsigned int shift = 8 * (3 - n);
    uint32_t mask = BITS_ABOVE_MASK32(8 * (3 - n));
    uint32_t value = (uint32_t)*lsrtp;

    r4300_write_aligned_word(r4300, lsaddr & ~UINT32_C(0x3), value << shift, mask);
}

DECLARE_INSTRUCTION(SD)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    if (check_alignment(r4300, lsaddr, 7, 1)) return;
    ADD_TO_PC(1);

    r4300_write_aligned_dword(r4300, lsaddr, (uint64_t)*lsrtp, ~UINT64_C(0));
}

DECLARE_INSTRUCTION(SDL)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 7);
    unsigned int shift = 8 * n;
    uint64_t mask = (n == 0)
        ? ~UINT64_C(0)
        : BITS_BELOW_MASK64(8 * (8 - n));
    uint64_t value = (uint64_t)*lsrtp;

    r4300_write_aligned_dword(r4300, lsaddr & ~UINT32_C(0x7), value >> shift, mask);
}

DECLARE_INSTRUCTION(SDR)
{
    DECLARE_R4300
    const uint32_t lsaddr = (uint32_t) irs32 + (uint32_t) iimmediate;
    int64_t *lsrtp = &irt;
    ADD_TO_PC(1);

    unsigned int n = (lsaddr & 7);
    unsigned int shift = 8 * (7 - n);
    uint64_t mask = BITS_ABOVE_MASK64(8 * (7 - n));
    uint64_t value = (uint64_t)*lsrtp;

    r4300_write_aligned_dword(r4300, lsaddr & ~UINT32_C(0x7), value << shift, mask);
}

static M64P_FORCE_INLINE int add_overflow_s32(int32_t a, int32_t b, int32_t* result)
{
    const uint32_t ua = (uint32_t)a;
    const uint32_t ub = (uint32_t)b;
    const uint32_t ur = ua + ub;
    *result = (int32_t)ur;
    return (int)((~(ua ^ ub) & (ua ^ ur)) >> 31);
}

static M64P_FORCE_INLINE int sub_overflow_s32(int32_t a, int32_t b, int32_t* result)
{
    const uint32_t ua = (uint32_t)a;
    const uint32_t ub = (uint32_t)b;
    const uint32_t ur = ua - ub;
    *result = (int32_t)ur;
    return (int)(((ua ^ ub) & (ua ^ ur)) >> 31);
}

static M64P_FORCE_INLINE int add_overflow_s64(int64_t a, int64_t b, int64_t* result)
{
    const uint64_t ua = (uint64_t)a;
    const uint64_t ub = (uint64_t)b;
    const uint64_t ur = ua + ub;
    *result = (int64_t)ur;
    return (int)((~(ua ^ ub) & (ua ^ ur)) >> 63);
}

static M64P_FORCE_INLINE int sub_overflow_s64(int64_t a, int64_t b, int64_t* result)
{
    const uint64_t ua = (uint64_t)a;
    const uint64_t ub = (uint64_t)b;
    const uint64_t ur = ua - ub;
    *result = (int64_t)ur;
    return (int)(((ua ^ ub) & (ua ^ ur)) >> 63);
}

static M64P_FORCE_INLINE void raise_arithmetic_overflow(struct r4300_core* r4300)
{
    r4300->cp0.regs[CP0_CAUSE_REG] =
        (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_OV;
    exception_general(r4300);
}

DECLARE_INSTRUCTION(ADD)
{
    DECLARE_R4300
    int32_t result;
    if (M64P_UNLIKELY(add_overflow_s32(rrs32, rrt32, &result)))
    {
        raise_arithmetic_overflow(r4300);
        return;
    }
    rrd = SE32(result);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(ADDU)
{
    DECLARE_R4300
    rrd = SE32((uint32_t) rrs32 + (uint32_t) rrt32);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(ADDI)
{
    DECLARE_R4300
    int32_t result;
    if (M64P_UNLIKELY(add_overflow_s32(irs32, (int32_t)iimmediate, &result)))
    {
        raise_arithmetic_overflow(r4300);
        return;
    }
    irt = SE32(result);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(ADDIU)
{
    DECLARE_R4300
    irt = SE32((uint32_t) irs32 + (uint32_t) iimmediate);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DADD)
{
    DECLARE_R4300
    int64_t result;
    if (M64P_UNLIKELY(add_overflow_s64(rrs, rrt, &result)))
    {
        raise_arithmetic_overflow(r4300);
        return;
    }
    rrd = result;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DADDU)
{
    DECLARE_R4300
    rrd = (uint64_t) rrs + (uint64_t) rrt;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DADDI)
{
    DECLARE_R4300
    int64_t result;
    if (M64P_UNLIKELY(add_overflow_s64(irs, (int64_t)iimmediate, &result)))
    {
        raise_arithmetic_overflow(r4300);
        return;
    }
    irt = result;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DADDIU)
{
    DECLARE_R4300
    irt = (uint64_t) irs + (uint64_t) iimmediate;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SUB)
{
    DECLARE_R4300
    int32_t result;
    if (M64P_UNLIKELY(sub_overflow_s32(rrs32, rrt32, &result)))
    {
        raise_arithmetic_overflow(r4300);
        return;
    }
    rrd = SE32(result);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SUBU)
{
    DECLARE_R4300
    rrd = SE32((uint32_t) rrs32 - (uint32_t) rrt32);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSUB)
{
    DECLARE_R4300
    int64_t result;
    if (M64P_UNLIKELY(sub_overflow_s64(rrs, rrt, &result)))
    {
        raise_arithmetic_overflow(r4300);
        return;
    }
    rrd = result;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSUBU)
{
    DECLARE_R4300
    rrd = (uint64_t) rrs - (uint64_t) rrt;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SLT)
{
    DECLARE_R4300
    rrd = (int64_t)(rrs < rrt);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SLTU)
{
    DECLARE_R4300
    rrd = (int64_t)((uint64_t)rrs < (uint64_t)rrt);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SLTI)
{
    DECLARE_R4300
    irt = (int64_t)(irs < (int64_t)iimmediate);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SLTIU)
{
    DECLARE_R4300
    irt = (int64_t)((uint64_t)irs < (uint64_t)(int64_t)iimmediate);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(AND)
{
    DECLARE_R4300
    rrd = rrs & rrt;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(ANDI)
{
    DECLARE_R4300
    irt = irs & (uint16_t) iimmediate;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(OR)
{
    DECLARE_R4300
    rrd = rrs | rrt;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(ORI)
{
    DECLARE_R4300
    irt = irs | (uint16_t) iimmediate;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(XOR)
{
    DECLARE_R4300
    rrd = rrs ^ rrt;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(XORI)
{
    DECLARE_R4300
    irt = irs ^ (uint16_t) iimmediate;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(NOR)
{
    DECLARE_R4300
    rrd = ~(rrs | rrt);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(LUI)
{
    DECLARE_R4300
    irt = SE32((uint32_t) iimmediate << 16);
    ADD_TO_PC(1);
}

/* Shift instructions */

DECLARE_INSTRUCTION(NOP)
{
    DECLARE_R4300
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SLL)
{
    DECLARE_R4300
    rrd = SE32((uint32_t) rrt32 << rsa);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SLLV)
{
    DECLARE_R4300
    rrd = SE32((uint32_t) rrt32 << (rrs32 & 0x1F));
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSLL)
{
    DECLARE_R4300
    rrd = (uint64_t) rrt << rsa;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSLLV)
{
    DECLARE_R4300
    rrd = (uint64_t) rrt << (rrs32 & 0x3F);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSLL32)
{
    DECLARE_R4300
    rrd = (uint64_t) rrt << (32 + rsa);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SRL)
{
    DECLARE_R4300
    rrd = SE32((uint32_t) rrt32 >> rsa);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SRLV)
{
    DECLARE_R4300
    rrd = SE32((uint32_t) rrt32 >> (rrs32 & 0x1F));
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSRL)
{
    DECLARE_R4300
    rrd = (uint64_t) rrt >> rsa;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSRLV)
{
    DECLARE_R4300
    rrd = (uint64_t) rrt >> (rrs32 & 0x3F);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSRL32)
{
    DECLARE_R4300
    rrd = (uint64_t) rrt >> (32 + rsa);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SRA)
{
    DECLARE_R4300
    rrd = SE32(rrt >> rsa);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SRAV)
{
    DECLARE_R4300
    rrd = SE32(rrt >> (rrs32 & 0x1F));
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSRA)
{
    DECLARE_R4300
    rrd = rrt >> rsa;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSRAV)
{
    DECLARE_R4300
    rrd = (int64_t) rrt >> (rrs32 & 0x3F);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DSRA32)
{
    DECLARE_R4300
    rrd = (int64_t) rrt >> (32 + rsa);
    ADD_TO_PC(1);
}

/* Multiply / Divide instructions */

DECLARE_INSTRUCTION(MULT)
{
    DECLARE_R4300
    /* VR4300/Ares uses a signed 64-by-35-bit product for MULT. */
    const int64_t rhs = (int64_t)((uint64_t)rrt << 29) >> 29;
    const uint64_t temp = (uint64_t)rrs * (uint64_t)rhs;
    r4300->hi = SE32(temp >> 32);
    r4300->lo = SE32(temp);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MULTU)
{
    DECLARE_R4300
    uint64_t temp;
    temp = (uint32_t) rrs * (uint64_t) ((uint32_t) rrt);
    r4300->hi = (int64_t) temp >> 32;
    r4300->lo = SE32(temp);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DMULT)
{
    DECLARE_R4300
#if defined(__SIZEOF_INT128__)
    const __int128 product = (__int128)rrs * (__int128)rrt;
    const unsigned __int128 bits = (unsigned __int128)product;
    r4300->lo = (int64_t)(uint64_t)bits;
    r4300->hi = (int64_t)(uint64_t)(bits >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    __int64 high;
    r4300->lo = (int64_t)_mul128((__int64)rrs, (__int64)rrt, &high);
    r4300->hi = (int64_t)high;
#else
    uint64_t op1, op2, op3, op4;
    uint64_t result1, result2, result3, result4;
    uint64_t temp1, temp2, temp3, temp4;
    uint64_t lhs = (uint64_t)rrs;
    uint64_t rhs = (uint64_t)rrt;
    const uint64_t lhs_mask = UINT64_C(0) - (lhs >> 63);
    const uint64_t rhs_mask = UINT64_C(0) - (rhs >> 63);
    const uint64_t sign_mask = lhs_mask ^ rhs_mask;

    /* Absolute values without data-dependent branches. */
    lhs = (lhs ^ lhs_mask) - lhs_mask;
    rhs = (rhs ^ rhs_mask) - rhs_mask;

    op1 = lhs & UINT64_C(0xFFFFFFFF);
    op2 = lhs >> 32;
    op3 = rhs & UINT64_C(0xFFFFFFFF);
    op4 = rhs >> 32;

    temp1 = op1 * op3;
    temp2 = (temp1 >> 32) + op1 * op4;
    temp3 = op2 * op3;
    temp4 = (temp3 >> 32) + op2 * op4;

    result1 = temp1 & UINT64_C(0xFFFFFFFF);
    result2 = temp2 + (temp3 & UINT64_C(0xFFFFFFFF));
    result3 = (result2 >> 32) + temp4;
    result4 = result3 >> 32;

    r4300->lo = (int64_t)(result1 | (result2 << 32));
    r4300->hi = (int64_t)((result3 & UINT64_C(0xFFFFFFFF)) | (result4 << 32));

    {
        uint64_t lo = (uint64_t)r4300->lo;
        uint64_t hi = (uint64_t)r4300->hi;
        const uint64_t neg_lo = (~lo) + UINT64_C(1);
        const uint64_t neg_hi = (~hi) + (neg_lo == 0);
        lo = (lo & ~sign_mask) | (neg_lo & sign_mask);
        hi = (hi & ~sign_mask) | (neg_hi & sign_mask);
        r4300->lo = (int64_t)lo;
        r4300->hi = (int64_t)hi;
    }
#endif
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DMULTU)
{
    DECLARE_R4300
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 product = (unsigned __int128)(uint64_t)rrs *
                                      (unsigned __int128)(uint64_t)rrt;
    r4300->lo = (int64_t)(uint64_t)product;
    r4300->hi = (int64_t)(uint64_t)(product >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned __int64 high;
    r4300->lo = (int64_t)_umul128((unsigned __int64)rrs, (unsigned __int64)rrt, &high);
    r4300->hi = (int64_t)high;
#else
    uint64_t op1, op2, op3, op4;
    uint64_t result1, result2, result3, result4;
    uint64_t temp1, temp2, temp3, temp4;

    op1 = (uint64_t)rrs & UINT64_C(0xFFFFFFFF);
    op2 = (uint64_t)rrs >> 32;
    op3 = (uint64_t)rrt & UINT64_C(0xFFFFFFFF);
    op4 = (uint64_t)rrt >> 32;

    temp1 = op1 * op3;
    temp2 = (temp1 >> 32) + op1 * op4;
    temp3 = op2 * op3;
    temp4 = (temp3 >> 32) + op2 * op4;

    result1 = temp1 & UINT64_C(0xFFFFFFFF);
    result2 = temp2 + (temp3 & UINT64_C(0xFFFFFFFF));
    result3 = (result2 >> 32) + temp4;
    result4 = result3 >> 32;

    r4300->lo = (int64_t)(result1 | (result2 << 32));
    r4300->hi = (int64_t)((result3 & UINT64_C(0xFFFFFFFF)) | (result4 << 32));
#endif
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DIV)
{
    DECLARE_R4300
    if (rrt)
    {
        r4300->lo = SE32((int64_t)rrs32 / rrt);
        r4300->hi = SE32((int64_t)rrs32 % rrt);
    }
    else
    {
        r4300->lo = rrs32 < 0 ? 1 : -1;
        r4300->hi = SE32(rrs32);
    }
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DIVU)
{
    DECLARE_R4300
    if (rrt32)
    {
        r4300->lo = SE32((uint32_t) rrs32 / (uint32_t) rrt32);
        r4300->hi = SE32((uint32_t) rrs32 % (uint32_t) rrt32);
    }
    else
    {
        r4300->lo = -1;
        r4300->hi = SE32(rrs32);
    }
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DDIV)
{
    DECLARE_R4300
    if (rrt)
    {
        if (rrs == INT64_MIN && rrt == -1)
        {
            r4300->lo = rrs;
            r4300->hi = 0;
        }
        else
        {
            r4300->lo = rrs / rrt;
            r4300->hi = rrs % rrt;
        }
    }
    else
    {
        r4300->lo = rrs < 0 ? 1 : -1;
        r4300->hi = rrs;
    }
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DDIVU)
{
    DECLARE_R4300
    if (rrt)
    {
        r4300->lo = (uint64_t) rrs / (uint64_t) rrt;
        r4300->hi = (uint64_t) rrs % (uint64_t) rrt;
    }
    else
    {
        r4300->lo = -1;
        r4300->hi = rrs;
    }
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MFHI)
{
    DECLARE_R4300
    rrd = r4300->hi;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MTHI)
{
    DECLARE_R4300
    r4300->hi = rrs;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MFLO)
{
    DECLARE_R4300
    rrd = r4300->lo;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MTLO)
{
    DECLARE_R4300
    r4300->lo = rrs;
    ADD_TO_PC(1);
}


DECLARE_JUMP(J,   (jinst_index<<2) | ((PCADDR+4) & UINT32_C(0xF0000000)), 1, &r4300->regs[0],  0, 0)
DECLARE_JUMP(JAL, (jinst_index<<2) | ((PCADDR+4) & UINT32_C(0xF0000000)), 1, &r4300->regs[31], 0, 0)

DECLARE_JUMP(JR,   irs32, 1, &r4300->regs[0], 0, 0)
DECLARE_JUMP(JALR, irs32, 1, &rrd,    0, 0)

DECLARE_JUMP(BEQ,     PCADDR + (iimmediate + 1) * 4, irs == irt, &r4300->regs[0], 0, 0)
DECLARE_JUMP(BEQL,    PCADDR + (iimmediate + 1) * 4, irs == irt, &r4300->regs[0], 1, 0)

DECLARE_JUMP(BNE,     PCADDR + (iimmediate + 1) * 4, irs != irt, &r4300->regs[0], 0, 0)
DECLARE_JUMP(BNEL,    PCADDR + (iimmediate + 1) * 4, irs != irt, &r4300->regs[0], 1, 0)

DECLARE_JUMP(BLEZ,    PCADDR + (iimmediate + 1) * 4, irs <= 0,   &r4300->regs[0], 0, 0)
DECLARE_JUMP(BLEZL,   PCADDR + (iimmediate + 1) * 4, irs <= 0,   &r4300->regs[0], 1, 0)

DECLARE_JUMP(BGTZ,    PCADDR + (iimmediate + 1) * 4, irs > 0,    &r4300->regs[0], 0, 0)
DECLARE_JUMP(BGTZL,   PCADDR + (iimmediate + 1) * 4, irs > 0,    &r4300->regs[0], 1, 0)

DECLARE_JUMP(BLTZ,    PCADDR + (iimmediate + 1) * 4, irs < 0,    &r4300->regs[0],  0, 0)
DECLARE_JUMP(BLTZAL,  PCADDR + (iimmediate + 1) * 4, irs < 0,    &r4300->regs[31], 0, 0)
DECLARE_JUMP(BLTZL,   PCADDR + (iimmediate + 1) * 4, irs < 0,    &r4300->regs[0],  1, 0)
DECLARE_JUMP(BLTZALL, PCADDR + (iimmediate + 1) * 4, irs < 0,    &r4300->regs[31], 1, 0)

DECLARE_JUMP(BGEZ,    PCADDR + (iimmediate + 1) * 4, irs >= 0,   &r4300->regs[0],  0, 0)
DECLARE_JUMP(BGEZAL,  PCADDR + (iimmediate + 1) * 4, irs >= 0,   &r4300->regs[31], 0, 0)
DECLARE_JUMP(BGEZL,   PCADDR + (iimmediate + 1) * 4, irs >= 0,   &r4300->regs[0],  1, 0)
DECLARE_JUMP(BGEZALL, PCADDR + (iimmediate + 1) * 4, irs >= 0,   &r4300->regs[31], 1, 0)

DECLARE_JUMP(BC1F,  PCADDR + (iimmediate + 1) * 4, ((r4300->cp1.fcr31) & FCR31_CMP_BIT) == 0, &r4300->regs[0], 0, 1)
DECLARE_JUMP(BC1FL, PCADDR + (iimmediate + 1) * 4, ((r4300->cp1.fcr31) & FCR31_CMP_BIT) == 0, &r4300->regs[0], 1, 1)
DECLARE_JUMP(BC1T,  PCADDR + (iimmediate + 1) * 4, ((r4300->cp1.fcr31) & FCR31_CMP_BIT) != 0, &r4300->regs[0], 0, 1)
DECLARE_JUMP(BC1TL, PCADDR + (iimmediate + 1) * 4, ((r4300->cp1.fcr31) & FCR31_CMP_BIT) != 0, &r4300->regs[0], 1, 1)


DECLARE_INSTRUCTION(CACHE)
{
    DECLARE_R4300
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(ERET)
{
    DECLARE_R4300
    uint32_t* cp0_regs = r4300->cp0.regs;
    int* cp0_cycle_count = &r4300->cp0.cycle_count;

    cp0_update_count(r4300);
    if (cp0_regs[CP0_STATUS_REG] & CP0_STATUS_ERL)
    {
        cp0_regs[CP0_STATUS_REG] &= ~CP0_STATUS_ERL;
        generic_jump_to(r4300, cp0_regs[CP0_ERROREPC_REG]);
    }
    else
    {
        cp0_regs[CP0_STATUS_REG] &= ~CP0_STATUS_EXL;
        generic_jump_to(r4300, cp0_regs[CP0_EPC_REG]);
    }
    r4300->llbit = 0;
    r4300_check_interrupt(r4300, CP0_CAUSE_IP2, r4300->mi->regs[MI_INTR_REG] & r4300->mi->regs[MI_INTR_MASK_REG]); // ???
    r4300->cp0.last_addr = PCADDR;
    if (*cp0_cycle_count >= 0) { gen_interrupt(r4300); }
}

DECLARE_INSTRUCTION(SYNC)
{
    DECLARE_R4300
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(SYSCALL)
{
    DECLARE_R4300
    uint32_t* cp0_regs = r4300->cp0.regs;

    cp0_regs[CP0_CAUSE_REG] = (cp0_regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_SYS;
    exception_general(r4300);
}

/* Trap instructions */

#define DECLARE_TRAP(name, cond) \
DECLARE_INSTRUCTION(name) \
{ \
    DECLARE_R4300 \
    uint32_t* cp0_regs = r4300->cp0.regs; \
    if (cond) \
    { \
        cp0_regs[CP0_CAUSE_REG] = (cp0_regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_TR; \
        exception_general(r4300); \
    } \
    else \
    { \
        ADD_TO_PC(1); \
    } \
}

DECLARE_TRAP(TGE, rrs >= rrt)
DECLARE_TRAP(TGEU, (uint64_t) rrs >= (uint64_t) rrt)
DECLARE_TRAP(TGEI, rrs >= (int64_t)iimmediate)
DECLARE_TRAP(TGEIU, (uint64_t)rrs >= (uint64_t)(int64_t)iimmediate)
DECLARE_TRAP(TLT, rrs < rrt)
DECLARE_TRAP(TLTU, (uint64_t) rrs < (uint64_t) rrt)
DECLARE_TRAP(TLTI, rrs < (int64_t)iimmediate)
DECLARE_TRAP(TLTIU, (uint64_t)rrs < (uint64_t)(int64_t)iimmediate)
DECLARE_TRAP(TEQ, rrs == rrt)
DECLARE_TRAP(TEQI, rrs == (int64_t)iimmediate)
DECLARE_TRAP(TNE, rrs != rrt)
DECLARE_TRAP(TNEI, rrs != (int64_t)iimmediate)

#undef DECLARE_TRAP


DECLARE_INSTRUCTION(TLBP)
{
    DECLARE_R4300
    int i;
    uint32_t* cp0_regs = r4300->cp0.regs;

    cp0_regs[CP0_INDEX_REG] |= UINT32_C(0x80000000);
    for (i = 0; i < 32; ++i)
    {
        if (r4300->cp0.tlb_entryhi_hi[i] == r4300->cp0.regs_hi[CP0_ENTRYHI_REG] &&
            ((r4300->cp0.tlb.entries[i].vpn2 & (~r4300->cp0.tlb.entries[i].mask)) ==
                    (((cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFFFFE000)) >> 13) & (~r4300->cp0.tlb.entries[i].mask))) &&
                ((r4300->cp0.tlb.entries[i].g) ||
                 (r4300->cp0.tlb.entries[i].asid == (cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFF)))))
        {
            cp0_regs[CP0_INDEX_REG] = i;
            break;
        }
    }
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(TLBR)
{
    DECLARE_R4300
    uint32_t* cp0_regs = r4300->cp0.regs;

    int index;
    index = cp0_regs[CP0_INDEX_REG] & UINT32_C(0x3F);
    if (index >= 32) { ADD_TO_PC(1); return; }
    r4300->cp0.regs_hi[CP0_ENTRYHI_REG] = r4300->cp0.tlb_entryhi_hi[index];
    cp0_regs[CP0_PAGEMASK_REG] = r4300->cp0.tlb.entries[index].mask << 13;
    cp0_regs[CP0_ENTRYHI_REG] = ((r4300->cp0.tlb.entries[index].vpn2 << 13) | r4300->cp0.tlb.entries[index].asid);
    cp0_regs[CP0_ENTRYLO0_REG] = (r4300->cp0.tlb.entries[index].pfn_even << 6) | (r4300->cp0.tlb.entries[index].c_even << 3)
        | (r4300->cp0.tlb.entries[index].d_even << 2) | (r4300->cp0.tlb.entries[index].v_even << 1)
        | r4300->cp0.tlb.entries[index].g;
    cp0_regs[CP0_ENTRYLO1_REG] = (r4300->cp0.tlb.entries[index].pfn_odd << 6) | (r4300->cp0.tlb.entries[index].c_odd << 3)
        | (r4300->cp0.tlb.entries[index].d_odd << 2) | (r4300->cp0.tlb.entries[index].v_odd << 1)
        | r4300->cp0.tlb.entries[index].g;
    ADD_TO_PC(1);
}

static void TLBWrite(struct r4300_core* r4300, unsigned int idx)
{
    uint32_t* cp0_regs = r4300->cp0.regs;
    if (idx >= 32) return;

    r4300->cp0.tlb_entryhi_hi[idx] = r4300->cp0.regs_hi[CP0_ENTRYHI_REG];
    tlb_unmap(&r4300->cp0.tlb, idx);

    r4300->cp0.tlb.entries[idx].g = (cp0_regs[CP0_ENTRYLO0_REG] & cp0_regs[CP0_ENTRYLO1_REG] & 1);
    r4300->cp0.tlb.entries[idx].pfn_even = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x3FFFFFC0)) >> 6;
    r4300->cp0.tlb.entries[idx].pfn_odd = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x3FFFFFC0)) >> 6;
    r4300->cp0.tlb.entries[idx].c_even = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x38)) >> 3;
    r4300->cp0.tlb.entries[idx].c_odd = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x38)) >> 3;
    r4300->cp0.tlb.entries[idx].d_even = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x4)) >> 2;
    r4300->cp0.tlb.entries[idx].d_odd = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x4)) >> 2;
    r4300->cp0.tlb.entries[idx].v_even = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x2)) >> 1;
    r4300->cp0.tlb.entries[idx].v_odd = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x2)) >> 1;
    r4300->cp0.tlb.entries[idx].asid = (cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFF));
    r4300->cp0.tlb.entries[idx].vpn2 = (cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFFFFE000)) >> 13;
    //r4300->cp0.tlb.entries[idx].r = (cp0_regs[CP0_ENTRYHI_REG] & 0xC000000000000000LL) >> 62;
    r4300->cp0.tlb.entries[idx].mask = (cp0_regs[CP0_PAGEMASK_REG] & UINT32_C(0x1FFE000)) >> 13;

    r4300->cp0.tlb.entries[idx].start_even = r4300->cp0.tlb.entries[idx].vpn2 << 13;
    r4300->cp0.tlb.entries[idx].end_even = r4300->cp0.tlb.entries[idx].start_even+
        (r4300->cp0.tlb.entries[idx].mask << 12) + UINT32_C(0xFFF);
    r4300->cp0.tlb.entries[idx].phys_even = r4300->cp0.tlb.entries[idx].pfn_even << 12;


    r4300->cp0.tlb.entries[idx].start_odd = r4300->cp0.tlb.entries[idx].end_even+1;
    r4300->cp0.tlb.entries[idx].end_odd = r4300->cp0.tlb.entries[idx].start_odd+
        (r4300->cp0.tlb.entries[idx].mask << 12) + UINT32_C(0xFFF);
    r4300->cp0.tlb.entries[idx].phys_odd = r4300->cp0.tlb.entries[idx].pfn_odd << 12;

    tlb_map(&r4300->cp0.tlb, idx);
    invalidate_r4300_cached_code(r4300, 0, 0);
}

DECLARE_INSTRUCTION(TLBWR)
{
    DECLARE_R4300
    uint32_t* cp0_regs = r4300->cp0.regs;
    cp0_update_count(r4300);
    cp0_regs[CP0_RANDOM_REG] = cp0_random(&r4300->cp0);
    TLBWrite(r4300, cp0_regs[CP0_RANDOM_REG]);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(TLBWI)
{
    DECLARE_R4300
    uint32_t* cp0_regs = r4300->cp0.regs;

    TLBWrite(r4300, cp0_regs[CP0_INDEX_REG] & UINT32_C(0x3F));
    ADD_TO_PC(1);
}


DECLARE_INSTRUCTION(MFC0)
{
    uint64_t value = cp0_get_control(&r4300->cp0, rfs);
    rrt = RS_OF(op) == 1 ? (int64_t)value : SE32(value);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MTC0)
{
    struct cp0* cp0 = &r4300->cp0;
    uint32_t* regs = cp0->regs;
    uint64_t data = (uint64_t)rrt;
    uint64_t old = ((uint64_t)cp0->regs_hi[rfs] << 32) | regs[rfs];
    cp0->latch = data;
    switch (rfs) {
    case 0: regs[0] = data & 0x8000003f; break;
    case 2: case 3: regs[rfs] = data & 0x3fffffff; break;
    case 4:
        data = (data & ~UINT64_C(0x7fffff)) | (old & 0x7ffff0);
        goto wide;
    case 5: regs[5] = data & 0x01ffe000; break;
    case 6: regs[6] = data & 63; break;
    case 9:
        cp0->interrupt_unsafe_state |= INTR_UNSAFE_R4300;
        if (cp0->cycle_count >= 0) gen_interrupt(r4300);
        cp0->interrupt_unsafe_state &= ~INTR_UNSAFE_R4300;
        translate_event_queue(cp0, (uint32_t)data);
        break;
    case 10: data &= UINT64_C(0xc00000ffffffe0ff); goto wide;
    case 11:
        regs[11] = (uint32_t)data;
        schedule_compare(cp0);
        regs[13] &= ~CP0_CAUSE_IP7;
        break;
    case 12:
        data = (data & 0xff57ffff) | (regs[12] & CP0_STATUS_TS);
        if ((data ^ regs[12]) & CP0_STATUS_FR) set_fpr_pointers(&r4300->cp1, (uint32_t)data);
        regs[12] = (uint32_t)data;
        break;
    case 13: regs[13] = (regs[13] & ~0x300) | (data & 0x300); break;
    case 14: case 30: goto wide;
    case 16: regs[16] = (regs[16] & ~UINT32_C(0x0f00800f)) | (data & 0x0f00800f); break;
    case 17: case 28: regs[rfs] = (uint32_t)data; break;
    case 18: regs[18] = data & 0xfffffffb; break;
    case 19: regs[19] = data & 15; break;
    case 20: data = (data & ~UINT64_C(0x1ffffffff)) | (old & UINT64_C(0x1fffffff0)); goto wide;
    case 26: regs[26] = data & 255; break;
    case 27: regs[27] = 0; break;
    default: break; /* Read-only or write-latch-only register. */
    }
    goto done;
wide:
    regs[rfs] = (uint32_t)data;
    cp0->regs_hi[rfs] = (uint32_t)(data >> 32);
done:
    ADD_TO_PC(1);
    if (rfs == 12 || rfs == 13 || rfs == 11) {
        r4300_check_interrupt(r4300, CP0_CAUSE_IP2,
            r4300->mi->regs[MI_INTR_REG] & r4300->mi->regs[MI_INTR_MASK_REG]);
    }
}

DECLARE_INSTRUCTION(COP0)
{
    unsigned int rs = RS_OF(op);
    if (rs == 0 || rs == 1) { MFC0(r4300, op); return; }
    if (rs == 4 || rs == 5) { MTC0(r4300, op); return; }
    if (rs == 2 || rs == 6 || rs == 8) { ADD_TO_PC(1); return; }
    if (rs >= 16) {
        switch (op & 63) {
        case 1: TLBR(r4300, op); return;
        case 2: TLBWI(r4300, op); return;
        case 6: TLBWR(r4300, op); return;
        case 8: TLBP(r4300, op); return;
        case 24: ERET(r4300, op); return;
        }
    }
    NI(r4300, op);
}
/* CP1 load/store instructions */

DECLARE_INSTRUCTION(LWC1)
{
    DECLARE_R4300
    const unsigned char lslfft = lfft;
    const uint32_t lslfaddr = (uint32_t) r4300->regs[lfbase] + lfoffset;
    if (check_cop1_unusable(r4300)) { return; }
    if (check_alignment(r4300, lslfaddr, 3, 0)) return;
    ADD_TO_PC(1);

    r4300_read_aligned_word(r4300, lslfaddr, (uint32_t*)r4300->cp1.regs_simple[lslfft]);
}

DECLARE_INSTRUCTION(LDC1)
{
    DECLARE_R4300
    const unsigned char lslfft = lfft;
    const uint32_t lslfaddr = (uint32_t) r4300->regs[lfbase] + lfoffset;
    if (check_cop1_unusable(r4300)) { return; }
    if (check_alignment(r4300, lslfaddr, 7, 0)) return;
    ADD_TO_PC(1);

    r4300_read_aligned_dword(r4300, lslfaddr, (uint64_t*)r4300->cp1.regs_double[lslfft]);
}

DECLARE_INSTRUCTION(SWC1)
{
    DECLARE_R4300
    const unsigned char lslfft = lfft;
    const uint32_t lslfaddr = (uint32_t) r4300->regs[lfbase] + lfoffset;
    if (check_cop1_unusable(r4300)) { return; }
    if (check_alignment(r4300, lslfaddr, 3, 1)) return;
    ADD_TO_PC(1);

    r4300_write_aligned_word(r4300, lslfaddr, *((uint32_t*)(r4300->cp1.regs_simple)[lslfft]), ~UINT32_C(0));
}

DECLARE_INSTRUCTION(SDC1)
{
    DECLARE_R4300
    const unsigned char lslfft = lfft;
    const uint32_t lslfaddr = (uint32_t) r4300->regs[lfbase] + lfoffset;
    if (check_cop1_unusable(r4300)) { return; }
    if (check_alignment(r4300, lslfaddr, 7, 1)) return;
    ADD_TO_PC(1);

    r4300_write_aligned_dword(r4300, lslfaddr, *((uint64_t*)(r4300->cp1.regs_double)[lslfft]), ~UINT64_C(0));
}

DECLARE_INSTRUCTION(MFC1)
{
    DECLARE_R4300
    if (check_cop1_unusable(r4300)) { return; }
    rrt = SE32(*((int32_t*) (r4300->cp1.regs_simple)[rfs]));
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DMFC1)
{
    DECLARE_R4300
    if (check_cop1_unusable(r4300)) { return; }
    rrt = *((int64_t*) (r4300->cp1.regs_double)[rfs]);
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(CFC1)
{
    DECLARE_R4300
    if (check_cop1_unusable(r4300)) { return; }
    rrt = 0;
    if (rfs==31)
    {
        rrt = SE32((r4300->cp1.fcr31));
    }
    if (rfs==0)
    {
        rrt = SE32((r4300->cp1.fcr0));
    }
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(MTC1)
{
    DECLARE_R4300
    if (check_cop1_unusable(r4300)) { return; }
    *((int32_t*) (r4300->cp1.regs_simple)[rfs]) = rrt32;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(DMTC1)
{
    DECLARE_R4300
    if (check_cop1_unusable(r4300)) { return; }
    *((int64_t*) (r4300->cp1.regs_double)[rfs]) = rrt;
    ADD_TO_PC(1);
}

DECLARE_INSTRUCTION(CTC1)
{
    DECLARE_R4300
    if (check_cop1_unusable(r4300)) { return; }
    if (rfs==31)
    {
        (r4300->cp1.fcr31) = (uint32_t)rrt32 & UINT32_C(0x0183ffff);
        update_x86_rounding_mode(&r4300->cp1);
        if ((r4300->cp1.fcr31 & UINT32_C(0x20000)) ||
            ((r4300->cp1.fcr31 >> 12) & (r4300->cp1.fcr31 >> 7) & 31))
        {
            r4300->cp0.regs[CP0_CAUSE_REG] =
                (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_FPE;
            exception_general(r4300);
            return;
        }
    }
    //if (((r4300->cp1.fcr31) >> 7) & 0x1F) printf("FPU Exception enabled : %x\n",
    //                 (int)(((r4300->cp1.fcr31) >> 7) & 0x1F));
    ADD_TO_PC(1);
}


#include "fpu_arithmetic.h"

#include "fpu_compare.h"

r4300_interp_handler pure_interp_decode(uint32_t op, int idle)
{
    if ((op >> 26) == 16) return COP0;
    const struct r4300_idec* idec = r4300_get_idec(op);
    const unsigned int fmt = (op >> 21) & 0x1f;
    if ((op >> 26) == 17 && fmt == 8 && RT_OF(op) >= 4) return NI;

#define PI_RETURN(name) case R4300_OP_##name: return name
#define PI_RETURN_RD(name) case R4300_OP_##name: return name
#define PI_RETURN_RT(name) case R4300_OP_##name: return name
#define PI_RETURN_BRANCH(name) case R4300_OP_##name: return idle ? name##_IDLE : name

    switch (idec->opcode)
    {
    PI_RETURN_RD(ADD);
    PI_RETURN_RT(ADDI);
    PI_RETURN_RT(ADDIU);
    PI_RETURN_RD(ADDU);
    PI_RETURN_RD(AND);
    PI_RETURN_RT(ANDI);

    PI_RETURN_BRANCH(BC1F);
    PI_RETURN_BRANCH(BC1FL);
    PI_RETURN_BRANCH(BC1T);
    PI_RETURN_BRANCH(BC1TL);
    PI_RETURN_BRANCH(BEQ);
    PI_RETURN_BRANCH(BEQL);
    PI_RETURN_BRANCH(BGEZ);
    PI_RETURN_BRANCH(BGEZAL);
    PI_RETURN_BRANCH(BGEZALL);
    PI_RETURN_BRANCH(BGEZL);
    PI_RETURN_BRANCH(BGTZ);
    PI_RETURN_BRANCH(BGTZL);
    PI_RETURN_BRANCH(BLEZ);
    PI_RETURN_BRANCH(BLEZL);
    PI_RETURN_BRANCH(BLTZ);
    PI_RETURN_BRANCH(BLTZAL);
    PI_RETURN_BRANCH(BLTZALL);
    PI_RETURN_BRANCH(BLTZL);
    PI_RETURN_BRANCH(BNE);
    PI_RETURN_BRANCH(BNEL);

    case R4300_OP_BREAK:
        return BREAK;
    PI_RETURN(CACHE);
    PI_RETURN_RT(CFC1);
    PI_RETURN(CTC1);

    PI_RETURN_RD(DADD);
    PI_RETURN_RT(DADDI);
    PI_RETURN_RT(DADDIU);
    PI_RETURN_RD(DADDU);
    PI_RETURN(DDIV);
    PI_RETURN(DDIVU);
    PI_RETURN(DIV);
    PI_RETURN(DIVU);
    PI_RETURN_RT(DMFC1);
    PI_RETURN(DMTC1);
    PI_RETURN(DMULT);
    PI_RETURN(DMULTU);
    PI_RETURN_RD(DSLL);
    PI_RETURN_RD(DSLL32);
    PI_RETURN_RD(DSLLV);
    PI_RETURN_RD(DSRA);
    PI_RETURN_RD(DSRA32);
    PI_RETURN_RD(DSRAV);
    PI_RETURN_RD(DSRL);
    PI_RETURN_RD(DSRL32);
    PI_RETURN_RD(DSRLV);
    PI_RETURN_RD(DSUB);
    PI_RETURN_RD(DSUBU);
    PI_RETURN(ERET);

    PI_RETURN_BRANCH(J);
    PI_RETURN_BRANCH(JAL);
    PI_RETURN(JALR);
    PI_RETURN(JR);

    PI_RETURN_RT(LB);
    PI_RETURN_RT(LBU);
    PI_RETURN_RT(LD);
    PI_RETURN(LDC1);
    PI_RETURN_RT(LDL);
    PI_RETURN_RT(LDR);
    PI_RETURN_RT(LH);
    PI_RETURN_RT(LHU);
    PI_RETURN_RT(LL);
    case R4300_OP_LLD: return LLD;
    PI_RETURN_RT(LUI);
    PI_RETURN_RT(LW);
    PI_RETURN(LWC1);
    PI_RETURN_RT(LWL);
    PI_RETURN_RT(LWR);
    PI_RETURN_RT(LWU);

    PI_RETURN_RT(MFC0);
    PI_RETURN_RT(MFC1);
    PI_RETURN_RD(MFHI);
    PI_RETURN_RD(MFLO);
    PI_RETURN(MTC0);
    PI_RETURN(MTC1);
    PI_RETURN(MTHI);
    PI_RETURN(MTLO);
    PI_RETURN(MULT);
    PI_RETURN(MULTU);

    PI_RETURN(NOP);
    PI_RETURN_RD(NOR);
    PI_RETURN_RD(OR);
    PI_RETURN_RT(ORI);

    PI_RETURN(SB);
    PI_RETURN_RT(SC);
    case R4300_OP_SCD: return SCD;
    PI_RETURN(SD);
    PI_RETURN(SDC1);
    PI_RETURN(SDL);
    PI_RETURN(SDR);
    PI_RETURN(SH);
    PI_RETURN_RD(SLL);
    PI_RETURN_RD(SLLV);
    PI_RETURN_RD(SLT);
    PI_RETURN_RT(SLTI);
    PI_RETURN_RT(SLTIU);
    PI_RETURN_RD(SLTU);
    PI_RETURN_RD(SRA);
    PI_RETURN_RD(SRAV);
    PI_RETURN_RD(SRL);
    PI_RETURN_RD(SRLV);
    PI_RETURN_RD(SUB);
    PI_RETURN_RD(SUBU);
    PI_RETURN(SW);
    PI_RETURN(SWC1);
    PI_RETURN(SWL);
    PI_RETURN(SWR);
    PI_RETURN(SYNC);
    PI_RETURN(SYSCALL);

    PI_RETURN(TEQ);
    PI_RETURN(TEQI);
    PI_RETURN(TGE);
    PI_RETURN(TGEI);
    PI_RETURN(TGEIU);
    PI_RETURN(TGEU);
    PI_RETURN(TLBP);
    PI_RETURN(TLBR);
    PI_RETURN(TLBWI);
    PI_RETURN(TLBWR);
    PI_RETURN(TLT);
    PI_RETURN(TLTI);
    PI_RETURN(TLTIU);
    PI_RETURN(TLTU);
    PI_RETURN(TNE);
    PI_RETURN(TNEI);
    PI_RETURN_RD(XOR);
    PI_RETURN_RT(XORI);

    case R4300_OP_CP1_ABS:
        return fmt == 16 ? ABS_S : (fmt == 17 ? ABS_D : NULL);
    case R4300_OP_CP1_ADD:
        return fmt == 16 ? ADD_S : (fmt == 17 ? ADD_D : NULL);
    case R4300_OP_CP1_CEIL_L:
        return fmt == 16 ? CEIL_L_S : (fmt == 17 ? CEIL_L_D : NULL);
    case R4300_OP_CP1_CEIL_W:
        return fmt == 16 ? CEIL_W_S : (fmt == 17 ? CEIL_W_D : NULL);
    case R4300_OP_CP1_C_EQ:
        return fmt == 16 ? C_EQ_S : (fmt == 17 ? C_EQ_D : NULL);
    case R4300_OP_CP1_C_F:
        return fmt == 16 ? C_F_S : (fmt == 17 ? C_F_D : NULL);
    case R4300_OP_CP1_C_LE:
        return fmt == 16 ? C_LE_S : (fmt == 17 ? C_LE_D : NULL);
    case R4300_OP_CP1_C_LT:
        return fmt == 16 ? C_LT_S : (fmt == 17 ? C_LT_D : NULL);
    case R4300_OP_CP1_C_NGE:
        return fmt == 16 ? C_NGE_S : (fmt == 17 ? C_NGE_D : NULL);
    case R4300_OP_CP1_C_NGL:
        return fmt == 16 ? C_NGL_S : (fmt == 17 ? C_NGL_D : NULL);
    case R4300_OP_CP1_C_NGLE:
        return fmt == 16 ? C_NGLE_S : (fmt == 17 ? C_NGLE_D : NULL);
    case R4300_OP_CP1_C_NGT:
        return fmt == 16 ? C_NGT_S : (fmt == 17 ? C_NGT_D : NULL);
    case R4300_OP_CP1_C_OLE:
        return fmt == 16 ? C_OLE_S : (fmt == 17 ? C_OLE_D : NULL);
    case R4300_OP_CP1_C_OLT:
        return fmt == 16 ? C_OLT_S : (fmt == 17 ? C_OLT_D : NULL);
    case R4300_OP_CP1_C_SEQ:
        return fmt == 16 ? C_SEQ_S : (fmt == 17 ? C_SEQ_D : NULL);
    case R4300_OP_CP1_C_SF:
        return fmt == 16 ? C_SF_S : (fmt == 17 ? C_SF_D : NULL);
    case R4300_OP_CP1_C_UEQ:
        return fmt == 16 ? C_UEQ_S : (fmt == 17 ? C_UEQ_D : NULL);
    case R4300_OP_CP1_C_ULE:
        return fmt == 16 ? C_ULE_S : (fmt == 17 ? C_ULE_D : NULL);
    case R4300_OP_CP1_C_ULT:
        return fmt == 16 ? C_ULT_S : (fmt == 17 ? C_ULT_D : NULL);
    case R4300_OP_CP1_C_UN:
        return fmt == 16 ? C_UN_S : (fmt == 17 ? C_UN_D : NULL);

    case R4300_OP_CP1_CVT_D:
        if (fmt == 16)
            return CVT_D_S;
        if (fmt == 20)
            return CVT_D_W;
        if (fmt == 21)
            return CVT_D_L;
        return NULL;
    case R4300_OP_CP1_CVT_L:
        return fmt == 16 ? CVT_L_S : (fmt == 17 ? CVT_L_D : NULL);
    case R4300_OP_CP1_CVT_S:
        if (fmt == 17)
            return CVT_S_D;
        if (fmt == 20)
            return CVT_S_W;
        if (fmt == 21)
            return CVT_S_L;
        return NULL;
    case R4300_OP_CP1_CVT_W:
        return fmt == 16 ? CVT_W_S : (fmt == 17 ? CVT_W_D : NULL);
    case R4300_OP_CP1_DIV:
        return fmt == 16 ? DIV_S : (fmt == 17 ? DIV_D : NULL);
    case R4300_OP_CP1_FLOOR_L:
        return fmt == 16 ? FLOOR_L_S : (fmt == 17 ? FLOOR_L_D : NULL);
    case R4300_OP_CP1_FLOOR_W:
        return fmt == 16 ? FLOOR_W_S : (fmt == 17 ? FLOOR_W_D : NULL);
    case R4300_OP_CP1_MOV:
        return fmt == 16 ? MOV_S : (fmt == 17 ? MOV_D : NULL);
    case R4300_OP_CP1_MUL:
        return fmt == 16 ? MUL_S : (fmt == 17 ? MUL_D : NULL);
    case R4300_OP_CP1_NEG:
        return fmt == 16 ? NEG_S : (fmt == 17 ? NEG_D : NULL);
    case R4300_OP_CP1_ROUND_L:
        return fmt == 16 ? ROUND_L_S : (fmt == 17 ? ROUND_L_D : NULL);
    case R4300_OP_CP1_ROUND_W:
        return fmt == 16 ? ROUND_W_S : (fmt == 17 ? ROUND_W_D : NULL);
    case R4300_OP_CP1_SQRT:
        return fmt == 16 ? SQRT_S : (fmt == 17 ? SQRT_D : NULL);
    case R4300_OP_CP1_SUB:
        return fmt == 16 ? SUB_S : (fmt == 17 ? SUB_D : NULL);
    case R4300_OP_CP1_TRUNC_L:
        return fmt == 16 ? TRUNC_L_S : (fmt == 17 ? TRUNC_L_D : NULL);
    case R4300_OP_CP1_TRUNC_W:
        return fmt == 16 ? TRUNC_W_S : (fmt == 17 ? TRUNC_W_D : NULL);

    default:
        return NULL;
    }

#undef PI_RETURN_BRANCH
#undef PI_RETURN_RT
#undef PI_RETURN_RD
#undef PI_RETURN
}

bool breakloop;
extern void main_check_inputs(void);

void new_vi(void)
{
    // apply_speed_limiter();
    main_check_inputs();
    breakloop = true;
}

static void InterpretOpcode(struct r4300_core* r4300, bool continuous)
{
    uint32_t* sequential_op = NULL;
    uint32_t sequential_pc = 0;
    uint32_t sequential_left = 0;

next_opcode: ;
    const uint32_t pc = r4300->interp_PC.addr;
    uint32_t* op_address;
    if (M64P_UNLIKELY(pc & 3)) {
        cp0_step_cycles(&r4300->cp0, 1);
        check_alignment(r4300, pc, 3, 0);
        if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0) gen_interrupt(r4300);
        sequential_op = NULL;
        if (continuous && !breakloop) goto next_opcode;
        return;
    }

    if (M64P_LIKELY(continuous && sequential_op != NULL && pc == sequential_pc))
    {
        op_address = sequential_op;

        if (M64P_LIKELY(--sequential_left != 0))
        {
            sequential_pc = pc + 4;
            sequential_op = op_address + 1;
        }
        else
        {
            sequential_op = NULL;
        }
    }
    else
    {
        op_address = interp_fast_mem_access(r4300, pc);
        if (M64P_UNLIKELY(op_address == NULL))
        {
            cp0_step_cycles(&r4300->cp0, 1);
            if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0)
                gen_interrupt(r4300);
            sequential_op = NULL;
            if (continuous && !breakloop)
                goto next_opcode;
            return;
        }

        if (M64P_LIKELY(continuous &&
                        (pc & UINT32_C(0xc0000000)) == UINT32_C(0x80000000) &&
                        (pc & UINT32_C(0x0fff)) != UINT32_C(0x0ffc)))
        {
            sequential_pc = pc + 4;
            sequential_op = op_address + 1;
            sequential_left = (UINT32_C(0x0ffc) - (pc & UINT32_C(0x0fff))) >> 2;
        }
        else
        {
            sequential_op = NULL;
        }
    }

    const uint32_t op = *op_address;
    if (!r4300_begin_instruction(r4300, op, pc)) goto instruction_done;
    if (M64P_UNLIKELY(op == 0))
    {
        r4300->interp_PC.addr = pc + 4;
        goto instruction_done;
    }

#if M64P_USE_COMPUTED_GOTO
    static void* const pi_major_dispatch[64] =
    {
        &&pi_major_0, &&pi_major_1, &&pi_major_2, &&pi_major_3,
        &&pi_major_4, &&pi_major_5, &&pi_major_6, &&pi_major_7,
        &&pi_major_8, &&pi_major_9, &&pi_major_10, &&pi_major_11,
        &&pi_major_12, &&pi_major_13, &&pi_major_14, &&pi_major_15,
        &&pi_major_16, &&pi_major_17, &&pi_major_default, &&pi_major_default,
        &&pi_major_20, &&pi_major_21, &&pi_major_22, &&pi_major_23,
        &&pi_major_24, &&pi_major_25, &&pi_major_26, &&pi_major_27,
        &&pi_major_default, &&pi_major_default, &&pi_major_default, &&pi_major_default,
        &&pi_major_32, &&pi_major_33, &&pi_major_34, &&pi_major_35,
        &&pi_major_36, &&pi_major_37, &&pi_major_38, &&pi_major_39,
        &&pi_major_40, &&pi_major_41, &&pi_major_42, &&pi_major_43,
        &&pi_major_44, &&pi_major_45, &&pi_major_46, &&pi_major_47,
        &&pi_major_48, &&pi_major_49, &&pi_major_default, &&pi_major_default,
        &&pi_major_52, &&pi_major_53, &&pi_major_default, &&pi_major_55,
        &&pi_major_56, &&pi_major_57, &&pi_major_default, &&pi_major_default,
        &&pi_major_60, &&pi_major_61, &&pi_major_default, &&pi_major_63
    };
    do
    {
        goto *pi_major_dispatch[op >> 26];
#else
    switch ((op >> 26) & 0x3F) {
#endif
    M64P_MAJOR_CASE(0) /* SPECIAL prefix */
#if M64P_USE_COMPUTED_GOTO
        static void* const pi_special_dispatch[64] =
        {
            &&pi_special_0, &&pi_special_default, &&pi_special_2, &&pi_special_3,
            &&pi_special_4, &&pi_special_default, &&pi_special_6, &&pi_special_7,
            &&pi_special_8, &&pi_special_9, &&pi_special_default, &&pi_special_default,
            &&pi_special_12, &&pi_special_13, &&pi_special_default, &&pi_special_15,
            &&pi_special_16, &&pi_special_17, &&pi_special_18, &&pi_special_19,
            &&pi_special_20, &&pi_special_default, &&pi_special_22, &&pi_special_23,
            &&pi_special_24, &&pi_special_25, &&pi_special_26, &&pi_special_27,
            &&pi_special_28, &&pi_special_29, &&pi_special_30, &&pi_special_31,
            &&pi_special_32, &&pi_special_33, &&pi_special_34, &&pi_special_35,
            &&pi_special_36, &&pi_special_37, &&pi_special_38, &&pi_special_39,
            &&pi_special_default, &&pi_special_default, &&pi_special_42, &&pi_special_43,
            &&pi_special_44, &&pi_special_45, &&pi_special_46, &&pi_special_47,
            &&pi_special_48, &&pi_special_49, &&pi_special_50, &&pi_special_51,
            &&pi_special_52, &&pi_special_default, &&pi_special_54, &&pi_special_default,
            &&pi_special_56, &&pi_special_default, &&pi_special_58, &&pi_special_59,
            &&pi_special_60, &&pi_special_default, &&pi_special_62, &&pi_special_63
        };
        do
        {
            goto *pi_special_dispatch[op & 0x3F];
#else
        switch (op & 0x3F) {
#endif
        M64P_SPECIAL_CASE(0) /* SPECIAL opcode 0: SLL */
            SLL(r4300, op);
            break;
        M64P_SPECIAL_CASE(2) /* SPECIAL opcode 2: SRL */
            SRL(r4300, op);
            break;
        M64P_SPECIAL_CASE(3) /* SPECIAL opcode 3: SRA */
            SRA(r4300, op);
            break;
        M64P_SPECIAL_CASE(4) /* SPECIAL opcode 4: SLLV */
            SLLV(r4300, op);
            break;
        M64P_SPECIAL_CASE(6) /* SPECIAL opcode 6: SRLV */
            SRLV(r4300, op);
            break;
        M64P_SPECIAL_CASE(7) /* SPECIAL opcode 7: SRAV */
            SRAV(r4300, op);
            break;
        M64P_SPECIAL_CASE(8) JR(r4300, op); break;
        M64P_SPECIAL_CASE(9) /* SPECIAL opcode 9: JALR */

            JALR(r4300, op);
            break;
        M64P_SPECIAL_CASE(12) SYSCALL(r4300, op); break;
        M64P_SPECIAL_CASE(13) 
            BREAK(r4300, op);
            break;
        M64P_SPECIAL_CASE(15) SYNC(r4300, op); break;
        M64P_SPECIAL_CASE(16) /* SPECIAL opcode 16: MFHI */
            MFHI(r4300, op);
            break;
        M64P_SPECIAL_CASE(17) MTHI(r4300, op); break;
        M64P_SPECIAL_CASE(18) /* SPECIAL opcode 18: MFLO */
            MFLO(r4300, op);
            break;
        M64P_SPECIAL_CASE(19) MTLO(r4300, op); break;
        M64P_SPECIAL_CASE(20) /* SPECIAL opcode 20: DSLLV */
            DSLLV(r4300, op);
            break;
        M64P_SPECIAL_CASE(22) /* SPECIAL opcode 22: DSRLV */
            DSRLV(r4300, op);
            break;
        M64P_SPECIAL_CASE(23) /* SPECIAL opcode 23: DSRAV */
            DSRAV(r4300, op);
            break;
        M64P_SPECIAL_CASE(24) MULT(r4300, op); break;
        M64P_SPECIAL_CASE(25) MULTU(r4300, op); break;
        M64P_SPECIAL_CASE(26) DIV(r4300, op); break;
        M64P_SPECIAL_CASE(27) DIVU(r4300, op); break;
        M64P_SPECIAL_CASE(28) DMULT(r4300, op); break;
        M64P_SPECIAL_CASE(29) DMULTU(r4300, op); break;
        M64P_SPECIAL_CASE(30) DDIV(r4300, op); break;
        M64P_SPECIAL_CASE(31) DDIVU(r4300, op); break;
        M64P_SPECIAL_CASE(32) /* SPECIAL opcode 32: ADD */
            ADD(r4300, op);
            break;
        M64P_SPECIAL_CASE(33) /* SPECIAL opcode 33: ADDU */
            ADDU(r4300, op);
            break;
        M64P_SPECIAL_CASE(34) /* SPECIAL opcode 34: SUB */
            SUB(r4300, op);
            break;
        M64P_SPECIAL_CASE(35) /* SPECIAL opcode 35: SUBU */
            SUBU(r4300, op);
            break;
        M64P_SPECIAL_CASE(36) /* SPECIAL opcode 36: AND */
            AND(r4300, op);
            break;
        M64P_SPECIAL_CASE(37) /* SPECIAL opcode 37: OR */
            OR(r4300, op);
            break;
        M64P_SPECIAL_CASE(38) /* SPECIAL opcode 38: XOR */
            XOR(r4300, op);
            break;
        M64P_SPECIAL_CASE(39) /* SPECIAL opcode 39: NOR */
            NOR(r4300, op);
            break;
        M64P_SPECIAL_CASE(42) /* SPECIAL opcode 42: SLT */
            SLT(r4300, op);
            break;
        M64P_SPECIAL_CASE(43) /* SPECIAL opcode 43: SLTU */
            SLTU(r4300, op);
            break;
        M64P_SPECIAL_CASE(44) /* SPECIAL opcode 44: DADD */
            DADD(r4300, op);
            break;
        M64P_SPECIAL_CASE(45) /* SPECIAL opcode 45: DADDU */
            DADDU(r4300, op);
            break;
        M64P_SPECIAL_CASE(46) /* SPECIAL opcode 46: DSUB */
            DSUB(r4300, op);
            break;
        M64P_SPECIAL_CASE(47) /* SPECIAL opcode 47: DSUBU */
            DSUBU(r4300, op);
            break;
        M64P_SPECIAL_CASE(48) TGE(r4300, op); break;
        M64P_SPECIAL_CASE(49) TGEU(r4300, op); break;
        M64P_SPECIAL_CASE(50) TLT(r4300, op); break;
        M64P_SPECIAL_CASE(51) TLTU(r4300, op); break;
        M64P_SPECIAL_CASE(52) TEQ(r4300, op); break;
        M64P_SPECIAL_CASE(54) TNE(r4300, op); break;
        M64P_SPECIAL_CASE(56) /* SPECIAL opcode 56: DSLL */
            DSLL(r4300, op);
            break;
        M64P_SPECIAL_CASE(58) /* SPECIAL opcode 58: DSRL */
            DSRL(r4300, op);
            break;
        M64P_SPECIAL_CASE(59) /* SPECIAL opcode 59: DSRA */
            DSRA(r4300, op);
            break;
        M64P_SPECIAL_CASE(60) /* SPECIAL opcode 60: DSLL32 */
            DSLL32(r4300, op);
            break;
        M64P_SPECIAL_CASE(62) /* SPECIAL opcode 62: DSRL32 */
            DSRL32(r4300, op);
            break;
        M64P_SPECIAL_CASE(63) /* SPECIAL opcode 63: DSRA32 */
            DSRA32(r4300, op);
            break;
        M64P_SPECIAL_DEFAULT /* SPECIAL opcodes 1, 5, 10, 11, 14, 21, 40, 41, 53, 55, 57,
            61: Reserved Instructions */
            NI(r4300, op);
            break;
#if M64P_USE_COMPUTED_GOTO
        } while (0);
#else
        }
#endif /* switch (op & 0x3F) for the SPECIAL prefix */
        break;
    M64P_MAJOR_CASE(1) /* REGIMM prefix */
#if M64P_USE_COMPUTED_GOTO
        static void* const pi_regimm_dispatch[32] =
        {
            &&pi_regimm_0, &&pi_regimm_1, &&pi_regimm_2, &&pi_regimm_3,
            &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default,
            &&pi_regimm_8, &&pi_regimm_9, &&pi_regimm_10, &&pi_regimm_11,
            &&pi_regimm_12, &&pi_regimm_default, &&pi_regimm_14, &&pi_regimm_default,
            &&pi_regimm_16, &&pi_regimm_17, &&pi_regimm_18, &&pi_regimm_19,
            &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default,
            &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default,
            &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default, &&pi_regimm_default
        };
        do
        {
            goto *pi_regimm_dispatch[(op >> 16) & 0x1F];
#else
        switch ((op >> 16) & 0x1F) {
#endif
        M64P_REGIMM_CASE(0) /* REGIMM opcode 0: BLTZ */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BLTZ_IDLE(r4300, op);
            else BLTZ(r4300, op);
            break;
        M64P_REGIMM_CASE(1) /* REGIMM opcode 1: BGEZ */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BGEZ_IDLE(r4300, op);
            else BGEZ(r4300, op);
            break;
        M64P_REGIMM_CASE(2) /* REGIMM opcode 2: BLTZL */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BLTZL_IDLE(r4300, op);
            else BLTZL(r4300, op);
            break;
        M64P_REGIMM_CASE(3) /* REGIMM opcode 3: BGEZL */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BGEZL_IDLE(r4300, op);
            else BGEZL(r4300, op);
            break;
        M64P_REGIMM_CASE(8) TGEI(r4300, op); break;
        M64P_REGIMM_CASE(9) TGEIU(r4300, op); break;
        M64P_REGIMM_CASE(10) TLTI(r4300, op); break;
        M64P_REGIMM_CASE(11) TLTIU(r4300, op); break;
        M64P_REGIMM_CASE(12) TEQI(r4300, op); break;
        M64P_REGIMM_CASE(14) TNEI(r4300, op); break;
        M64P_REGIMM_CASE(16) /* REGIMM opcode 16: BLTZAL */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BLTZAL_IDLE(r4300, op);
            else BLTZAL(r4300, op);
            break;
        M64P_REGIMM_CASE(17) /* REGIMM opcode 17: BGEZAL */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BGEZAL_IDLE(r4300, op);
            else BGEZAL(r4300, op);
            break;
        M64P_REGIMM_CASE(18) /* REGIMM opcode 18: BLTZALL */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BLTZALL_IDLE(r4300, op);
            else BLTZALL(r4300, op);
            break;
        M64P_REGIMM_CASE(19) /* REGIMM opcode 19: BGEZALL */
            if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BGEZALL_IDLE(r4300, op);
            else BGEZALL(r4300, op);
            break;
        M64P_REGIMM_DEFAULT /* REGIMM opcodes 4..7, 13, 15, 20..31:
            Reserved Instructions */
            NI(r4300, op);
            break;
#if M64P_USE_COMPUTED_GOTO
        } while (0);
#else
        }
#endif /* switch ((op >> 16) & 0x1F) for the REGIMM prefix */
        break;
    M64P_MAJOR_CASE(2) /* Major opcode 2: J */
        if (IS_ABSOLUTE_IDLE_LOOP(r4300, op, pc)) J_IDLE(r4300, op);
        else J(r4300, op);
        break;
    M64P_MAJOR_CASE(3) /* Major opcode 3: JAL */
        if (IS_ABSOLUTE_IDLE_LOOP(r4300, op, pc)) JAL_IDLE(r4300, op);
        else JAL(r4300, op);
        break;
    M64P_MAJOR_CASE(4) /* Major opcode 4: BEQ */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BEQ_IDLE(r4300, op);
        else BEQ(r4300, op);
        break;
    M64P_MAJOR_CASE(5) /* Major opcode 5: BNE */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BNE_IDLE(r4300, op);
        else BNE(r4300, op);
        break;
    M64P_MAJOR_CASE(6) /* Major opcode 6: BLEZ */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BLEZ_IDLE(r4300, op);
        else BLEZ(r4300, op);
        break;
    M64P_MAJOR_CASE(7) /* Major opcode 7: BGTZ */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BGTZ_IDLE(r4300, op);
        else BGTZ(r4300, op);
        break;
    M64P_MAJOR_CASE(8) /* Major opcode 8: ADDI */
        ADDI(r4300, op);
        break;
    M64P_MAJOR_CASE(9) /* Major opcode 9: ADDIU */
        ADDIU(r4300, op);
        break;
    M64P_MAJOR_CASE(10) /* Major opcode 10: SLTI */
        SLTI(r4300, op);
        break;
    M64P_MAJOR_CASE(11) /* Major opcode 11: SLTIU */
        SLTIU(r4300, op);
        break;
    M64P_MAJOR_CASE(12) /* Major opcode 12: ANDI */
        ANDI(r4300, op);
        break;
    M64P_MAJOR_CASE(13) /* Major opcode 13: ORI */
        ORI(r4300, op);
        break;
    M64P_MAJOR_CASE(14) /* Major opcode 14: XORI */
        XORI(r4300, op);
        break;
    M64P_MAJOR_CASE(15) /* Major opcode 15: LUI */
        LUI(r4300, op);
        break;
    M64P_MAJOR_CASE(16) COP0(r4300, op); break;
    M64P_MAJOR_CASE(17) /* Coprocessor 1 prefix */
#if M64P_USE_COMPUTED_GOTO
        static void* const pi_cop1_dispatch[32] =
        {
            &&pi_cop1_0, &&pi_cop1_1, &&pi_cop1_2, &&pi_cop1_default,
            &&pi_cop1_4, &&pi_cop1_5, &&pi_cop1_6, &&pi_cop1_default,
            &&pi_cop1_8, &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default,
            &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default,
            &&pi_cop1_16, &&pi_cop1_17, &&pi_cop1_default, &&pi_cop1_default,
            &&pi_cop1_20, &&pi_cop1_21, &&pi_cop1_default, &&pi_cop1_default,
            &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default,
            &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default, &&pi_cop1_default
        };
        do
        {
            goto *pi_cop1_dispatch[(op >> 21) & 0x1F];
#else
        switch ((op >> 21) & 0x1F) {
#endif
        M64P_COP1_CASE(0) /* Coprocessor 1 opcode 0: MFC1 */
            MFC1(r4300, op);
            break;
        M64P_COP1_CASE(1) /* Coprocessor 1 opcode 1: DMFC1 */
            DMFC1(r4300, op);
            break;
        M64P_COP1_CASE(2) /* Coprocessor 1 opcode 2: CFC1 */
            CFC1(r4300, op);
            break;
        M64P_COP1_CASE(4) MTC1(r4300, op); break;
        M64P_COP1_CASE(5) DMTC1(r4300, op); break;
        M64P_COP1_CASE(6) CTC1(r4300, op); break;
        M64P_COP1_CASE(8) /* Coprocessor 1 opcode 8: Branch on C1 condition */
            switch ((op >> 16) & 0x1f) {
            default: NI(r4300, op); break;
            case 0:
                if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BC1F_IDLE(r4300, op);
                else BC1F(r4300, op);
                break;
            case 1:
                if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BC1T_IDLE(r4300, op);
                else BC1T(r4300, op);
                break;
            case 2:
                if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BC1FL_IDLE(r4300, op);
                else BC1FL(r4300, op);
                break;
            case 3:
                if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BC1TL_IDLE(r4300, op);
                else BC1TL(r4300, op);
                break;
            }
            break;
        M64P_COP1_CASE(16) /* Coprocessor 1 S-format opcodes */
#if M64P_USE_COMPUTED_GOTO
            static void* const pi_cp1s_dispatch[64] =
            {
                &&pi_cp1s_0, &&pi_cp1s_1, &&pi_cp1s_2, &&pi_cp1s_3,
                &&pi_cp1s_4, &&pi_cp1s_5, &&pi_cp1s_6, &&pi_cp1s_7,
                &&pi_cp1s_8, &&pi_cp1s_9, &&pi_cp1s_10, &&pi_cp1s_11,
                &&pi_cp1s_12, &&pi_cp1s_13, &&pi_cp1s_14, &&pi_cp1s_15,
                &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_default, &&pi_cp1s_33, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_36, &&pi_cp1s_37, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default, &&pi_cp1s_default,
                &&pi_cp1s_48, &&pi_cp1s_49, &&pi_cp1s_50, &&pi_cp1s_51,
                &&pi_cp1s_52, &&pi_cp1s_53, &&pi_cp1s_54, &&pi_cp1s_55,
                &&pi_cp1s_56, &&pi_cp1s_57, &&pi_cp1s_58, &&pi_cp1s_59,
                &&pi_cp1s_60, &&pi_cp1s_61, &&pi_cp1s_62, &&pi_cp1s_63
            };
            do
            {
                goto *pi_cp1s_dispatch[op & 0x3F];
#else
            switch (op & 0x3F) {
#endif
            M64P_CP1S_CASE(0) ADD_S(r4300, op); break;
            M64P_CP1S_CASE(1) SUB_S(r4300, op); break;
            M64P_CP1S_CASE(2) MUL_S(r4300, op); break;
            M64P_CP1S_CASE(3) DIV_S(r4300, op); break;
            M64P_CP1S_CASE(4) SQRT_S(r4300, op); break;
            M64P_CP1S_CASE(5) ABS_S(r4300, op); break;
            M64P_CP1S_CASE(6) MOV_S(r4300, op); break;
            M64P_CP1S_CASE(7) NEG_S(r4300, op); break;
            M64P_CP1S_CASE(8) ROUND_L_S(r4300, op); break;
            M64P_CP1S_CASE(9) TRUNC_L_S(r4300, op); break;
            M64P_CP1S_CASE(10) CEIL_L_S(r4300, op); break;
            M64P_CP1S_CASE(11) FLOOR_L_S(r4300, op); break;
            M64P_CP1S_CASE(12) ROUND_W_S(r4300, op); break;
            M64P_CP1S_CASE(13) TRUNC_W_S(r4300, op); break;
            M64P_CP1S_CASE(14) CEIL_W_S(r4300, op); break;
            M64P_CP1S_CASE(15) FLOOR_W_S(r4300, op); break;
            M64P_CP1S_CASE(33) CVT_D_S(r4300, op); break;
            M64P_CP1S_CASE(36) CVT_W_S(r4300, op); break;
            M64P_CP1S_CASE(37) CVT_L_S(r4300, op); break;
            M64P_CP1S_CASE(48) C_F_S(r4300, op); break;
            M64P_CP1S_CASE(49) C_UN_S(r4300, op); break;
            M64P_CP1S_CASE(50) C_EQ_S(r4300, op); break;
            M64P_CP1S_CASE(51) C_UEQ_S(r4300, op); break;
            M64P_CP1S_CASE(52) C_OLT_S(r4300, op); break;
            M64P_CP1S_CASE(53) C_ULT_S(r4300, op); break;
            M64P_CP1S_CASE(54) C_OLE_S(r4300, op); break;
            M64P_CP1S_CASE(55) C_ULE_S(r4300, op); break;
            M64P_CP1S_CASE(56) C_SF_S(r4300, op); break;
            M64P_CP1S_CASE(57) C_NGLE_S(r4300, op); break;
            M64P_CP1S_CASE(58) C_SEQ_S(r4300, op); break;
            M64P_CP1S_CASE(59) C_NGL_S(r4300, op); break;
            M64P_CP1S_CASE(60) C_LT_S(r4300, op); break;
            M64P_CP1S_CASE(61) C_NGE_S(r4300, op); break;
            M64P_CP1S_CASE(62) C_LE_S(r4300, op); break;
            M64P_CP1S_CASE(63) C_NGT_S(r4300, op); break;
            M64P_CP1S_DEFAULT
                NI(r4300, op);
                break;
#if M64P_USE_COMPUTED_GOTO
            } while (0);
#else
            }
#endif
            break;
        M64P_COP1_CASE(17) /* Coprocessor 1 D-format opcodes */
#if M64P_USE_COMPUTED_GOTO
            static void* const pi_cp1d_dispatch[64] =
            {
                &&pi_cp1d_0, &&pi_cp1d_1, &&pi_cp1d_2, &&pi_cp1d_3,
                &&pi_cp1d_4, &&pi_cp1d_5, &&pi_cp1d_6, &&pi_cp1d_7,
                &&pi_cp1d_8, &&pi_cp1d_9, &&pi_cp1d_10, &&pi_cp1d_11,
                &&pi_cp1d_12, &&pi_cp1d_13, &&pi_cp1d_14, &&pi_cp1d_15,
                &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_32, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_36, &&pi_cp1d_37, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default, &&pi_cp1d_default,
                &&pi_cp1d_48, &&pi_cp1d_49, &&pi_cp1d_50, &&pi_cp1d_51,
                &&pi_cp1d_52, &&pi_cp1d_53, &&pi_cp1d_54, &&pi_cp1d_55,
                &&pi_cp1d_56, &&pi_cp1d_57, &&pi_cp1d_58, &&pi_cp1d_59,
                &&pi_cp1d_60, &&pi_cp1d_61, &&pi_cp1d_62, &&pi_cp1d_63
            };
            do
            {
                goto *pi_cp1d_dispatch[op & 0x3F];
#else
            switch (op & 0x3F) {
#endif
            M64P_CP1D_CASE(0) ADD_D(r4300, op); break;
            M64P_CP1D_CASE(1) SUB_D(r4300, op); break;
            M64P_CP1D_CASE(2) MUL_D(r4300, op); break;
            M64P_CP1D_CASE(3) DIV_D(r4300, op); break;
            M64P_CP1D_CASE(4) SQRT_D(r4300, op); break;
            M64P_CP1D_CASE(5) ABS_D(r4300, op); break;
            M64P_CP1D_CASE(6) MOV_D(r4300, op); break;
            M64P_CP1D_CASE(7) NEG_D(r4300, op); break;
            M64P_CP1D_CASE(8) ROUND_L_D(r4300, op); break;
            M64P_CP1D_CASE(9) TRUNC_L_D(r4300, op); break;
            M64P_CP1D_CASE(10) CEIL_L_D(r4300, op); break;
            M64P_CP1D_CASE(11) FLOOR_L_D(r4300, op); break;
            M64P_CP1D_CASE(12) ROUND_W_D(r4300, op); break;
            M64P_CP1D_CASE(13) TRUNC_W_D(r4300, op); break;
            M64P_CP1D_CASE(14) CEIL_W_D(r4300, op); break;
            M64P_CP1D_CASE(15) FLOOR_W_D(r4300, op); break;
            M64P_CP1D_CASE(32) CVT_S_D(r4300, op); break;
            M64P_CP1D_CASE(36) CVT_W_D(r4300, op); break;
            M64P_CP1D_CASE(37) CVT_L_D(r4300, op); break;
            M64P_CP1D_CASE(48) C_F_D(r4300, op); break;
            M64P_CP1D_CASE(49) C_UN_D(r4300, op); break;
            M64P_CP1D_CASE(50) C_EQ_D(r4300, op); break;
            M64P_CP1D_CASE(51) C_UEQ_D(r4300, op); break;
            M64P_CP1D_CASE(52) C_OLT_D(r4300, op); break;
            M64P_CP1D_CASE(53) C_ULT_D(r4300, op); break;
            M64P_CP1D_CASE(54) C_OLE_D(r4300, op); break;
            M64P_CP1D_CASE(55) C_ULE_D(r4300, op); break;
            M64P_CP1D_CASE(56) C_SF_D(r4300, op); break;
            M64P_CP1D_CASE(57) C_NGLE_D(r4300, op); break;
            M64P_CP1D_CASE(58) C_SEQ_D(r4300, op); break;
            M64P_CP1D_CASE(59) C_NGL_D(r4300, op); break;
            M64P_CP1D_CASE(60) C_LT_D(r4300, op); break;
            M64P_CP1D_CASE(61) C_NGE_D(r4300, op); break;
            M64P_CP1D_CASE(62) C_LE_D(r4300, op); break;
            M64P_CP1D_CASE(63) C_NGT_D(r4300, op); break;
            M64P_CP1D_DEFAULT
                NI(r4300, op);
                break;
#if M64P_USE_COMPUTED_GOTO
            } while (0);
#else
            }
#endif
            break;
        M64P_COP1_CASE(20) /* Coprocessor 1 W-format opcodes */
            switch (op & 0x3F) {
            case 32: CVT_S_W(r4300, op); break;
            case 33: CVT_D_W(r4300, op); break;
            default:
                NI(r4300, op);
                break;
            }
            break;
        M64P_COP1_CASE(21) /* Coprocessor 1 L-format opcodes */
            switch (op & 0x3F) {
            case 32: CVT_S_L(r4300, op); break;
            case 33: CVT_D_L(r4300, op); break;
            default:
                NI(r4300, op);
                break;
            }
            break;
        M64P_COP1_DEFAULT /* Coprocessor 1 opcodes 3, 7, 9..15, 18..19, 22..31:
                             Reserved Instructions */
            NI(r4300, op);
            break;
#if M64P_USE_COMPUTED_GOTO
        } while (0);
#else
        }
#endif
        break;
    M64P_MAJOR_CASE(20) /* Major opcode 20: BEQL */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BEQL_IDLE(r4300, op);
        else BEQL(r4300, op);
        break;
    M64P_MAJOR_CASE(21) /* Major opcode 21: BNEL */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BNEL_IDLE(r4300, op);
        else BNEL(r4300, op);
        break;
    M64P_MAJOR_CASE(22) /* Major opcode 22: BLEZL */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BLEZL_IDLE(r4300, op);
        else BLEZL(r4300, op);
        break;
    M64P_MAJOR_CASE(23) /* Major opcode 23: BGTZL */
        if (IS_RELATIVE_IDLE_LOOP(r4300, op, pc)) BGTZL_IDLE(r4300, op);
        else BGTZL(r4300, op);
        break;
    M64P_MAJOR_CASE(24) /* Major opcode 24: DADDI */
        DADDI(r4300, op);
        break;
    M64P_MAJOR_CASE(25) /* Major opcode 25: DADDIU */
        DADDIU(r4300, op);
        break;
    M64P_MAJOR_CASE(26) /* Major opcode 26: LDL */
        LDL(r4300, op);
        break;
    M64P_MAJOR_CASE(27) /* Major opcode 27: LDR */
        LDR(r4300, op);
        break;
    M64P_MAJOR_CASE(32) /* Major opcode 32: LB */
        LB(r4300, op);
        break;
    M64P_MAJOR_CASE(33) /* Major opcode 33: LH */
        LH(r4300, op);
        break;
    M64P_MAJOR_CASE(34) /* Major opcode 34: LWL */
        LWL(r4300, op);
        break;
    M64P_MAJOR_CASE(35) /* Major opcode 35: LW */
        LW(r4300, op);
        break;
    M64P_MAJOR_CASE(36) /* Major opcode 36: LBU */
        LBU(r4300, op);
        break;
    M64P_MAJOR_CASE(37) /* Major opcode 37: LHU */
        LHU(r4300, op);
        break;
    M64P_MAJOR_CASE(38) /* Major opcode 38: LWR */
        LWR(r4300, op);
        break;
    M64P_MAJOR_CASE(39) /* Major opcode 39: LWU */
        LWU(r4300, op);
        break;
    M64P_MAJOR_CASE(40) SB(r4300, op); break;
    M64P_MAJOR_CASE(41) SH(r4300, op); break;
    M64P_MAJOR_CASE(42) SWL(r4300, op); break;
    M64P_MAJOR_CASE(43) SW(r4300, op); break;
    M64P_MAJOR_CASE(44) SDL(r4300, op); break;
    M64P_MAJOR_CASE(45) SDR(r4300, op); break;
    M64P_MAJOR_CASE(46) SWR(r4300, op); break;
    M64P_MAJOR_CASE(47) CACHE(r4300, op); break;
    M64P_MAJOR_CASE(48) /* Major opcode 48: LL */
        LL(r4300, op);
        break;
    M64P_MAJOR_CASE(49) LWC1(r4300, op); break;
    M64P_MAJOR_CASE(52) /* Major opcode 52: LLD */
        LLD(r4300, op);
        break;
    M64P_MAJOR_CASE(53) LDC1(r4300, op); break;
    M64P_MAJOR_CASE(55) /* Major opcode 55: LD */
        LD(r4300, op);
        break;
    M64P_MAJOR_CASE(56) /* Major opcode 56: SC */
        SC(r4300, op);
        break;
    M64P_MAJOR_CASE(57) SWC1(r4300, op); break;
    M64P_MAJOR_CASE(60) /* Major opcode 60: SCD */
        SCD(r4300, op);
        break;
    M64P_MAJOR_CASE(61) SDC1(r4300, op); break;
    M64P_MAJOR_CASE(63) SD(r4300, op); break;
    M64P_MAJOR_DEFAULT /* Major opcodes 18..19, 28..31, 50..51, 54, 58..59, 62:
        Reserved Instructions */
        NI(r4300, op);
        break;
#if M64P_USE_COMPUTED_GOTO
    } while (0);
#else
    }
#endif /* switch ((op >> 26) & 0x3F) */

instruction_done:
    r4300->regs[0] = 0;
    if (!r4300->delay_slot && r4300->cp0.cycle_count >= 0)
        gen_interrupt(r4300);
    if (M64P_LIKELY(continuous && !breakloop))
        goto next_opcode;
}

void pure_interp_execute_one(struct r4300_core* r4300)
{
    InterpretOpcode(r4300, false);
}

void run_r4300(struct r4300_core* r4300)
{
#ifdef OSAL_SSE
    // Save FTZ/DAZ mode.
    unsigned int daz = _MM_GET_DENORMALS_ZERO_MODE();
    unsigned int ftz = _MM_GET_FLUSH_ZERO_MODE();
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
#endif

    breakloop = false;
    if (r4300->startup)
    {
        r4300->stop = 0;
        r4300->pc = &r4300->interp_PC;
        r4300->interp_PC.addr = r4300->cp0.last_addr = r4300->start_address;
        if (r4300->emumode == EMUMODE_INTERPRETER && !cached_interp_init(r4300))
            r4300->emumode = EMUMODE_PURE_INTERPRETER;
        r4300->startup = 0;
    }

    if (r4300->emumode == EMUMODE_INTERPRETER && r4300->cached_interp != NULL)
    {
        r4300->execute_one = cached_interp_execute_one;
        cached_interp_run(r4300);
    }
    else
    {
        r4300->execute_one = pure_interp_execute_one;
        InterpretOpcode(r4300, true);
    }

#ifdef OSAL_SSE
    // Restore FTZ/DAZ mode.
    _MM_SET_DENORMALS_ZERO_MODE(daz);
    _MM_SET_FLUSH_ZERO_MODE(ftz);
#endif
}
