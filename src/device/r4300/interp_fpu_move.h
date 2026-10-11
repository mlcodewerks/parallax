/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef M64P_INTERP_FPU_MOVE_H
#define M64P_INTERP_FPU_MOVE_H
#include "r4300_core.h"

/* MOV.S and MOV.D are raw 64-bit copies in this VR4300 model. They preserve
 * FCR31 causes, accept every bit pattern, and select an even source in FR=0.
 * The caller has already charged instruction issue and checked CPU mode. */
static osal_force_inline void interp_fpu_move(struct r4300_core* r, uint32_t op)
{
    if (check_cop1_unusable(r)) return;
    unsigned fs = (op >> 11) & 31;
    if (!(r->cp0.regs[CP0_STATUS_REG] & CP0_STATUS_FR)) fs &= ~1u;
    r->cp1.regs[(op >> 6) & 31] = r->cp1.regs[fs];
    r->interp_PC.addr += 4;
}
#endif
