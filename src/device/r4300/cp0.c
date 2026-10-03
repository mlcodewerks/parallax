/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cp0.c                                                   *
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

#include <stdint.h>
#include <string.h>

#include "cp0.h"
#include "r4300_core.h"

#ifdef COMPARE_CORE
#include "api/debugger.h"
#endif

#ifdef DBG
#include "debugger/dbg_debugger.h"
#endif

/* global functions */
void init_cp0(struct cp0* cp0, unsigned int count_per_op, unsigned int count_per_op_denom_pot, const struct interrupt_handler* interrupt_handlers)
{
    cp0->count_per_op = count_per_op;
    cp0->count_per_op_denom_pot = count_per_op_denom_pot;

    memcpy(cp0->interrupt_handlers, interrupt_handlers, CP0_INTERRUPT_HANDLERS_COUNT*sizeof(*interrupt_handlers));
}

void poweron_cp0(struct cp0* cp0)
{
    uint32_t* cp0_regs;
    unsigned int* cp0_next_interrupt;
    int64_t* cp0_cycle_count;

    cp0_regs = r4300_cp0_regs(cp0);
    cp0_next_interrupt = r4300_cp0_next_interrupt(cp0);
    cp0_cycle_count = r4300_cp0_cycle_count(cp0);

    memset(cp0_regs, 0, CP0_REGS_COUNT * sizeof(cp0_regs[0]));
    cp0_regs[CP0_RANDOM_REG] = UINT32_C(31);
    cp0_regs[CP0_STATUS_REG]= UINT32_C(0x34000000);
    cp0_regs[CP0_CONFIG_REG]= UINT32_C(0x6e463);
    cp0_regs[CP0_PREVID_REG] = UINT32_C(0xb00);
    cp0_regs[CP0_COUNT_REG] = UINT32_C(0x5000);
    cp0_regs[CP0_CAUSE_REG] = UINT32_C(0x5c);
    cp0_regs[CP0_CONTEXT_REG] = UINT32_C(0x7ffff0);
    cp0_regs[CP0_EPC_REG] = UINT32_C(0xffffffff);
    cp0_regs[CP0_BADVADDR_REG] = UINT32_C(0xffffffff);
    cp0_regs[CP0_ERROREPC_REG] = UINT32_C(0xffffffff);
    cp0_reset_extended(cp0);

    /* XXX: clarify what is done on poweron, in soft_reset and in execute... */
    cp0->interrupt_unsafe_state = 0;
    *cp0_next_interrupt = 0;
    *cp0_cycle_count = 0;
    cp0->last_addr = UINT32_C(0xbfc00000);
    cp0->count_phase = 0;
    cp0->count_clock = 0;

    init_interrupt(cp0);

    poweron_tlb(&cp0->tlb);
}


void cp0_reset_extended(struct cp0* cp0)
{
    memset(cp0->regs_hi, 0, sizeof(cp0->regs_hi));
    memset(cp0->tlb_entryhi_hi, 0, sizeof(cp0->tlb_entryhi_hi));
    cp0->regs_hi[8] = (int32_t)cp0->regs[8] < 0 ? UINT32_MAX : 0;
    cp0->regs_hi[14] = (int32_t)cp0->regs[14] < 0 ? UINT32_MAX : 0;
    cp0->regs_hi[30] = (int32_t)cp0->regs[30] < 0 ? UINT32_MAX : 0;
    cp0->latch = 0;
    cp0->random_state = 0x6d2b79f5;
}

uint32_t cp0_random(struct cp0* cp0)
{
    uint32_t x = cp0->random_state, wired = cp0->regs[CP0_WIRED_REG] & 63;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    cp0->random_state = x;
    return cp0->regs[CP0_RANDOM_REG] = wired < 32 ? wired + x % (32 - wired) : x & 63;
}

uint64_t cp0_get_control(struct cp0* cp0, unsigned int reg)
{
    uint64_t value = ((uint64_t)cp0->regs_hi[reg] << 32) | cp0->regs[reg];
    switch (reg) {
    case 0: return value & 0x8000003f;
    case 1: return cp0_random(cp0);
    case 2: case 3: return value & 0x3fffffff;
    case 4: case 20: return value & ~UINT64_C(15);
    case 5: return value & 0x01ffe000;
    case 6: return value & 63;
    case 8: case 14: case 30: return value;
    case 9: case 11: case 17: case 27: case 28: return (uint32_t)value;
    case 10: return value & UINT64_C(0xc00000ffffffe0ff);
    case 12: return value & 0xff77ffff;
    case 13: return value & 0xb000ff7c;
    case 15: return value & 0xffff;
    case 16: return (value & 0x7f00800f) | 0x66460;
    case 18: return value & 0xfffffffb;
    case 19: return value & 15;
    case 26: return value & 255;
    case 29: return 0;
    default: return cp0->latch;
    }
}
uint32_t* r4300_cp0_regs(struct cp0* cp0)
{
    return cp0->regs;
}

uint32_t* r4300_cp0_last_addr(struct cp0* cp0)
{
    return &cp0->last_addr;
}

unsigned int* r4300_cp0_next_interrupt(struct cp0* cp0)
{
    return &cp0->next_interrupt;
}

int64_t* r4300_cp0_cycle_count(struct cp0* cp0)
{
    return &cp0->cycle_count;
}


int check_cop1_unusable(struct r4300_core* r4300)
{
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);

    if (!(cp0_regs[CP0_STATUS_REG] & CP0_STATUS_CU1))
    {
        cp0_regs[CP0_CAUSE_REG] = (cp0_regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c))
            | CP0_CAUSE_EXCCODE_CPU | CP0_CAUSE_CE1;
        exception_general(r4300);
        return 1;
    }
    return 0;
}

int check_instruction_mode(struct r4300_core* r4300, uint32_t op)
{
    const uint32_t status = r4300->cp0.regs[CP0_STATUS_REG];
    const unsigned int major = op >> 26, fn = op & 63;
    const unsigned int mode = (status & CP0_STATUS_MODE_MASK) >> 3;
    uint32_t code = CP0_CAUSE_EXCCODE_RI;
    int wide = 0;
    if ((status & (CP0_STATUS_EXL | CP0_STATUS_ERL)) || mode == 0) return 0;
    if (major == 0x10 && !(status & CP0_STATUS_CU0)) {
        code = CP0_CAUSE_EXCCODE_CPU;
        goto fault;
    }
    if (status & (mode == 1 ? CP0_STATUS_SX : CP0_STATUS_UX)) return 0;
    if (major == 0) {
        wide = fn == 0x14 || fn == 0x16 || fn == 0x17 ||
            (fn >= 0x1c && fn <= 0x1f) || (fn >= 0x2c && fn <= 0x2f) ||
            fn == 0x38 || fn == 0x3a || fn == 0x3b || fn == 0x3c || fn == 0x3e || fn == 0x3f;
    } else {
        wide = (major >= 0x18 && major <= 0x1b) || major == 0x2c || major == 0x2d ||
            major == 0x34 || major == 0x37 || major == 0x3c || major == 0x3f ||
            (major == 0x10 && (((op >> 21) & 31) == 1 || ((op >> 21) & 31) == 5));
    }
    if (!wide) return 0;
fault:
    r4300->cp0.regs[CP0_CAUSE_REG] =
        (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | code;
    exception_general(r4300);
    return 1;
}

void cp0_update_count(struct r4300_core* r4300)
{
    struct cp0* cp0 = &r4300->cp0;
    cp0->last_addr = *r4300_pc(r4300);

#ifdef COMPARE_CORE
   if (r4300->delay_slot)
     CoreCompareCallback();
#endif
/*#ifdef DBG
   if (g_DebuggerActive && !r4300->delay_slot) update_debugger(*r4300_pc(r4300));
#endif
*/
}

static void exception_epilog(struct r4300_core* r4300)
{
        if (r4300->delay_slot)
        {
            r4300->skip_jump = *r4300_pc(r4300);
            *r4300_cp0_next_interrupt(&r4300->cp0) = 0;
            *r4300_cp0_cycle_count(&r4300->cp0) = 0;
        }
}


void TLB_refill_exception(struct r4300_core* r4300, uint32_t address, int w)
{
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);
    int usual_handler = 0, i;

    if (w != 2) {
        cp0_update_count(r4300);
    }

    cp0_regs[CP0_CAUSE_REG] = (w == 1)
        ? CP0_CAUSE_EXCCODE_TLBS
        : CP0_CAUSE_EXCCODE_TLBL;

    cp0_regs[CP0_BADVADDR_REG] = address;
    r4300->cp0.regs_hi[CP0_BADVADDR_REG] = (int32_t)address < 0 ? UINT32_MAX : 0;
    r4300->cp0.regs_hi[CP0_ENTRYHI_REG] = (int32_t)address < 0 ? 0xc00000ff : 0;
    cp0_regs[CP0_CONTEXT_REG] = (cp0_regs[CP0_CONTEXT_REG] & UINT32_C(0xFF80000F))
        | ((address >> 9) & UINT32_C(0x007FFFF0));
    cp0_regs[CP0_ENTRYHI_REG] = address & UINT32_C(0xFFFFE000);

    if (cp0_regs[CP0_STATUS_REG] & CP0_STATUS_EXL)
    {
        generic_jump_to(r4300, UINT32_C(0x80000180));


        if (r4300->delay_slot == 1 || r4300->delay_slot == 3) {
            cp0_regs[CP0_CAUSE_REG] |= CP0_CAUSE_BD;
        }
        else {
            cp0_regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_BD;
        }
    }
    else
    {
       
        cp0_regs[CP0_EPC_REG] = *r4300_pc(r4300);
        r4300->cp0.regs_hi[CP0_EPC_REG] = (int32_t)*r4300_pc(r4300) < 0 ? UINT32_MAX : 0;

        cp0_regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_BD;
        cp0_regs[CP0_STATUS_REG] |= CP0_STATUS_EXL;

        if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000)) {
            usual_handler = 1;
        }

        for (i = 0; i < 32; i++)
        {
            if (/*r4300->cp0.tlb.entries[i].v_even &&*/ address >= r4300->cp0.tlb.entries[i].start_even &&
                    address <= r4300->cp0.tlb.entries[i].end_even) {
                usual_handler = 1;
            }
            if (/*r4300->cp0.tlb.entries[i].v_odd &&*/ address >= r4300->cp0.tlb.entries[i].start_odd &&
                    address <= r4300->cp0.tlb.entries[i].end_odd) {
                usual_handler = 1;
            }
        }

        generic_jump_to(r4300, (usual_handler)
                ? UINT32_C(0x80000180)
                : UINT32_C(0x80000000));
    }

    if (r4300->delay_slot == 1 || r4300->delay_slot == 3)
    {
        cp0_regs[CP0_CAUSE_REG] |= CP0_CAUSE_BD;
        cp0_regs[CP0_EPC_REG] -= 4;
    }
    else
    {
        cp0_regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_BD;
    }
    if (w != 2) {
        cp0_regs[CP0_EPC_REG] -= 4;
    }

    r4300->cp0.last_addr = *r4300_pc(r4300);

    exception_epilog(r4300);
}

void exception_general(struct r4300_core* r4300)
{
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);

    cp0_update_count(r4300);
    if (!(cp0_regs[CP0_STATUS_REG] & CP0_STATUS_EXL))
    {
        cp0_regs[CP0_EPC_REG] = *r4300_pc(r4300);
        r4300->cp0.regs_hi[CP0_EPC_REG] = (int32_t)*r4300_pc(r4300) < 0 ? UINT32_MAX : 0;
        if (r4300->delay_slot == 1 || r4300->delay_slot == 3) {
            cp0_regs[CP0_CAUSE_REG] |= CP0_CAUSE_BD;
            cp0_regs[CP0_EPC_REG] -= 4;
        } else {
            cp0_regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_BD;
        }
    }
    cp0_regs[CP0_STATUS_REG] |= CP0_STATUS_EXL;

    generic_jump_to(r4300, (cp0_regs[CP0_STATUS_REG] & CP0_STATUS_BEV)
        ? UINT32_C(0xbfc00380) : UINT32_C(0x80000180));

    r4300->cp0.last_addr = *r4300_pc(r4300);

    exception_epilog(r4300);
}

