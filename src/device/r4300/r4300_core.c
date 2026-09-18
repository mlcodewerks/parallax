/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - r4300_core.c                                            *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

#include "r4300_core.h"
#include "cached_interp.h"
#if defined(COUNT_INSTR)
#include "instr_counters.h"
#endif
#include "pure_interp.h"

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "main/main.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

void init_r4300(struct r4300_core* r4300, struct memory* mem, struct mi_controller* mi, struct rdram* rdram, const struct interrupt_handler* interrupt_handlers,
    unsigned int emumode, unsigned int count_per_op, unsigned int count_per_op_denom_pot, int no_compiled_jump, int randomize_interrupt, uint32_t start_address)
{

    r4300->emumode = emumode;
    init_cp0(&r4300->cp0, count_per_op, count_per_op_denom_pot, interrupt_handlers);
    init_cp1(&r4300->cp1);

    r4300->mem = mem;
    r4300->mi = mi;
    r4300->rdram = rdram;
    r4300->randomize_interrupt = randomize_interrupt;
    r4300->start_address = start_address;
    r4300->execute_one = NULL;
    r4300->cached_interp = NULL;
    srand((unsigned int) time(NULL));
}

void poweron_r4300(struct r4300_core* r4300)
{
    /* clear registers */
    memset(r4300_regs(r4300), 0, 32*sizeof(int64_t));
    *r4300_mult_hi(r4300) = 0;
    *r4300_mult_lo(r4300) = 0;
    r4300->llbit = 0;

    *r4300_pc_struct(r4300) = NULL;
    r4300->delay_slot = 0;
    r4300->skip_jump = 0;
    r4300->reset_hard_job = 0;
    r4300->startup =1;

    cached_interp_invalidate(r4300, 0, 0);

    /* setup CP0 registers */
    poweron_cp0(&r4300->cp0);

    /* setup CP1 registers */
    poweron_cp1(&r4300->cp1);
}

int64_t* r4300_regs(struct r4300_core* r4300)
{

    return r4300->regs;
}

int64_t* r4300_mult_hi(struct r4300_core* r4300)
{
    return &r4300->hi;
}

int64_t* r4300_mult_lo(struct r4300_core* r4300)
{
    return &r4300->lo;
}

unsigned int* r4300_llbit(struct r4300_core* r4300)
{
    return &r4300->llbit;
}

uint32_t* r4300_pc(struct r4300_core* r4300)
{
    return &(*r4300_pc_struct(r4300))->addr;
}

struct precomp_instr** r4300_pc_struct(struct r4300_core* r4300)
{
    return &r4300->pc;
}

int* r4300_stop(struct r4300_core* r4300)
{
    return &r4300->stop;
}

unsigned int get_r4300_emumode(struct r4300_core* r4300)
{
    return r4300->emumode;
}

uint32_t *fast_mem_access(struct r4300_core* r4300, uint32_t address)
{
    /* This code is performance critical, specially on pure interpreter mode.
     * Removing error checking saves some time, but the emulator may crash. */

    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 2);
        if (address == 0) // TLB exception
            return NULL;
    }

    address &= UINT32_C(0x1ffffffc);

    return mem_base_u32(r4300->mem->base, address);
}

/* Read aligned word from memory.
 * address may not be word-aligned for byte or hword accesses.
 * Alignment is taken care of when calling mem handler.
 */
int r4300_read_aligned_word(struct r4300_core* r4300, uint32_t address, uint32_t* value)
{
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 0);
        if (address == 0) {
            return 0;
        }
    }

    address &= UINT32_C(0x1ffffffc);

    mem_read32(mem_get_handler(r4300->mem, address), address & ~UINT32_C(3), value);

    return 1;
}

/* Read aligned dword from memory */
int r4300_read_aligned_dword(struct r4300_core* r4300, uint32_t address, uint64_t* value)
{
    uint32_t w[2];

    /* XXX: unaligned dword accesses should trigger a address error,
     * but inaccurate timing of the core can lead to unaligned address on reset
     * so just emit a warning and keep going */
    if ((address & 0x7) != 0) {
        DebugMessage(M64MSG_WARNING, "Unaligned dword read %08x", address);
    }

    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 0);
        if (address == 0) {
            return 0;
        }
    }

    address &= UINT32_C(0x1ffffffc);

    const struct mem_handler* handler = mem_get_handler(r4300->mem, address);
    mem_read32(handler, address + 0, &w[0]);
    mem_read32(handler, address + 4, &w[1]);

    *value = ((uint64_t)w[0] << 32) | w[1];

    return 1;
}

/* Write aligned word to memory.
 * address may not be word-aligned for byte or hword accesses.
 * Alignment is taken care of when calling mem handler.
 */
int r4300_write_aligned_word(struct r4300_core* r4300, uint32_t address, uint32_t value, uint32_t mask)
{
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 1);
        if (address == 0) {
            return 0;
        }
    }

    invalidate_r4300_cached_code(r4300, address, 4);
    address &= UINT32_C(0x1ffffffc);

    mem_write32(mem_get_handler(r4300->mem, address), address & ~UINT32_C(3), value, mask);

    return 1;
}

/* Write aligned dword to memory */
int r4300_write_aligned_dword(struct r4300_core* r4300, uint32_t address, uint64_t value, uint64_t mask)
{
    /* XXX: unaligned dword accesses should trigger a address error,
     * but inaccurate timing of the core can lead to unaligned address on reset
     * so just emit a warning and keep going */
    if ((address & 0x7) != 0) {
        DebugMessage(M64MSG_WARNING, "Unaligned dword write %08x", address);
    }

    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 1);
        if (address == 0) {
            return 0;
        }
    }

    invalidate_r4300_cached_code(r4300, address, 8);
    address &= UINT32_C(0x1ffffffc);

    const struct mem_handler* handler = mem_get_handler(r4300->mem, address);
    mem_write32(handler, address + 0, value >> 32,      mask >> 32);
    mem_write32(handler, address + 4, (uint32_t) value, (uint32_t) mask      );

    return 1;
}

void invalidate_r4300_cached_code(struct r4300_core* r4300, uint32_t address, size_t size)
{
    cached_interp_invalidate(r4300, address, size);
}


void generic_jump_to(struct r4300_core* r4300, uint32_t address)
{
    *r4300_pc(r4300) = address;
}


/* XXX: not really a good interface but it gets the job done... */
void savestates_load_set_pc(struct r4300_core* r4300, uint32_t pc)
{
    generic_jump_to(r4300, pc);
    invalidate_r4300_cached_code(r4300, 0, 0);
}
