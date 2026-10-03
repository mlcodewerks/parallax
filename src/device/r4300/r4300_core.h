/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - r4300_core.h                                            *
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

#ifndef M64P_DEVICE_R4300_R4300_CORE_H
#define M64P_DEVICE_R4300_R4300_CORE_H

#include <stddef.h>
#include <stdint.h>

#if defined(PROFILE_R4300)
#include <stdio.h>
#endif

#include "cp0.h"
#include "cp1.h"

#include <stddef.h>
#include <stdint.h>

struct precomp_instr
{
    void (*ops)(void);
    union
    {
        struct
        {
            int64_t *rs;
            int64_t *rt;
            int16_t immediate;
        } i;
        struct
        {
            uint32_t inst_index;
        } j;
        struct
        {
            int64_t *rs;
            int64_t *rt;
            int64_t *rd;
            unsigned char sa;
            unsigned char nrd;
        } r;
        struct
        {
            unsigned char base;
            unsigned char ft;
            short offset;
        } lf;
        struct
        {
            unsigned char ft;
            unsigned char fs;
            unsigned char fd;
        } cf;
    } f;
    uint32_t addr; /* word-aligned instruction address in r4300 address space */
};

#include "osal/preproc.h"

struct memory;
struct mi_controller;
struct rdram;

struct jump_table;
struct cached_interp_state;

enum {
    EMUMODE_PURE_INTERPRETER = 0,
    EMUMODE_INTERPRETER      = 1,
    EMUMODE_DYNAREC          = 2,
};


struct r4300_core
{
    int64_t regs[32];
    int64_t hi;
    int64_t lo;

    unsigned int llbit;

    struct precomp_instr* pc;

    unsigned int delay_slot;
    uint32_t skip_jump;
	/* New dynarec uses a different memory layout */
    int stop;
    int startup;

    /* When reset_hard_job is set, next interrupt will cause hard reset */
    int reset_hard_job;

    /* from pure_interp.c */
    struct precomp_instr interp_PC;
    void (*execute_one)(struct r4300_core* r4300);
    struct cached_interp_state* cached_interp;

    unsigned int emumode;

    struct cp0 cp0;

    struct cp1 cp1;

    struct memory* mem;
    struct mi_controller* mi;
    struct rdram* rdram;

    uint32_t randomize_interrupt;

    uint32_t start_address;

    /* Virtual-indexed caches. DMA observes RAM, not dirty D-cache words. */
    uint32_t icache_tags[512];
    uint32_t dcache_tags[512];
    unsigned int cache_timing;
    uint32_t dcache_words[512][4];
};

#define R4300_KSEG0 UINT32_C(0x80000000)
#define R4300_KSEG1 UINT32_C(0xa0000000)


#define R4300_REGS_OFFSET \
    offsetof(struct r4300_core, regs)


void init_r4300(struct r4300_core* r4300, struct memory* mem, struct mi_controller* mi, struct rdram* rdram, const struct interrupt_handler* interrupt_handlers, unsigned int emumode, unsigned int count_per_op, unsigned int count_per_op_denom_pot, int no_compiled_jump, int randomize_interrupt, uint32_t start_address);
void poweron_r4300(struct r4300_core* r4300);



int64_t* r4300_regs(struct r4300_core* r4300);
int64_t* r4300_mult_hi(struct r4300_core* r4300);
int64_t* r4300_mult_lo(struct r4300_core* r4300);
unsigned int* r4300_llbit(struct r4300_core* r4300);
uint32_t* r4300_pc(struct r4300_core* r4300);
struct precomp_instr** r4300_pc_struct(struct r4300_core* r4300);
int* r4300_stop(struct r4300_core* r4300);
void run_r4300(struct r4300_core* r4300);
unsigned int get_r4300_emumode(struct r4300_core* r4300);

/* Returns a pointer to a block of contiguous memory
 * Can access RDRAM, SP_DMEM, SP_IMEM and ROM, using TLB if necessary
 * Useful for getting fast access to a zone with executable code. */
uint32_t *fast_mem_access(struct r4300_core* r4300, uint32_t address);

int r4300_read_aligned_word(struct r4300_core* r4300, uint32_t address, uint32_t* value);
int r4300_read_aligned_dword(struct r4300_core* r4300, uint32_t address, uint64_t* value);
int r4300_write_aligned_word(struct r4300_core* r4300, uint32_t address, uint32_t value, uint32_t mask);
int r4300_write_aligned_dword(struct r4300_core* r4300, uint32_t address, uint64_t value, uint64_t mask);

/* Allow cached/dynarec r4300 implementations to invalidate
 * their cached code at [address, address+size]
 *
 * If size == 0, r4300 implementation should invalidate
 * all cached code.
 */
void invalidate_r4300_cached_code(struct r4300_core* r4300, uint32_t address, size_t size);

/* Jump to the given address. This works for all r4300 emulator, but is slower.
 * Use this for common code which can be executed from any r4300 emulator. */
void generic_jump_to(struct r4300_core* r4300, unsigned int address);

void savestates_load_set_pc(struct r4300_core* r4300, uint32_t pc);

#endif
