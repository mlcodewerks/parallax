/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - interp_branch.h                                           *
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

#ifndef M64P_R4300_INTERP_BRANCH_H
#define M64P_R4300_INTERP_BRANCH_H
#include "r4300_core.h"
#include "cp0.h"
#include "timing.h"

static osal_force_inline int interp_direct_nop_delay(struct r4300_core* r)
{
    uint32_t pc = r->interp_PC.addr;
    uint32_t status = r->cp0.regs[CP0_STATUS_REG];
    if (r->cache_timing || (pc & UINT32_C(0xc0000000)) != UINT32_C(0x80000000) ||
        ((status & CP0_STATUS_MODE_MASK) && !(status & (CP0_STATUS_EXL | CP0_STATUS_ERL))))
        return 0;
    if (*(uint32_t*)((uint8_t*)r->mem->base + (pc & UINT32_C(0x1ffffffc))) != 0)
        return 0;
    /* The delay-slot interrupt remains deferred until the branch resolves. */
    cp0_step_cycles(&r->cp0, 1);
    r->interp_PC.addr = pc + 4;
    r->regs[0] = 0;
    return 1;
}

static osal_force_inline void interp_branch_finish_step(struct r4300_core* r,
    int taken, uint32_t target, int likely, void (*execute_one)(struct r4300_core*), int direct_nop)
{
    if (!likely || taken) {
        r->interp_PC.addr += 4;
        r->delay_slot = 1;
        if (!direct_nop || !interp_direct_nop_delay(r)) execute_one(r);
        cp0_update_count(r);
        r->delay_slot = 0;
        unsigned jump = (unsigned)taken & (unsigned)(r->skip_jump == 0);
#if defined(M64P_INTERP_BRANCHLESS) && M64P_INTERP_BRANCHLESS
        uint32_t mask = 0u - jump;
        r->interp_PC.addr = (target & mask) | (r->interp_PC.addr & ~mask);
#else
        if (jump) r->interp_PC.addr = target;
#endif
    } else {
        r->interp_PC.addr += 8;
        cp0_update_count(r);
    }
    r->cp0.last_addr = r->interp_PC.addr;
    if (r->cp0.cycle_count >= 0) gen_interrupt(r);
}

static osal_force_inline void interp_branch_finish(struct r4300_core* r,
    int taken, uint32_t target, int likely)
{
    interp_branch_finish_step(r, taken, target, likely, r->execute_one, 0);
}

static osal_force_inline void interp_idle_advance(struct r4300_core* r)
{
    cp0_update_count(r);
    if (r->cp0.cycle_count < 0) {
        r->cp0.regs[CP0_COUNT_REG] += (uint32_t)(-r->cp0.cycle_count);
        r->cp0.count_clock += -r->cp0.cycle_count;
        r->cp0.count_phase = 0;
        r->cp0.cycle_count = 0;
    }
}

static osal_force_inline int interp_equal_branch(struct r4300_core* r,
    uint32_t op, int idle, void (*execute_one)(struct r4300_core*))
{
    unsigned major = op >> 26;
    if (major != 4 && major != 5 && major != 20 && major != 21) return 0;
    int taken = r->regs[(op >> 21) & 31] == r->regs[(op >> 16) & 31];
    if (major & 1) taken = !taken;
    uint32_t target = r->interp_PC.addr + ((int32_t)(int16_t)op + 1) * 4;
    if (taken && idle) interp_idle_advance(r);
    interp_branch_finish_step(r, taken, target, major >= 20, execute_one, 1);
    return 1;
}
#endif
