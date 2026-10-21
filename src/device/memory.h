/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - memory.h                                                *
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

#ifndef PARALLAX_DEVICE_MEMORY_MEMORY_H
#define PARALLAX_DEVICE_MEMORY_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "osal/preproc.h"

enum { RDRAM_MAX_SIZE = 0x800000 };
enum { CART_ROM_MAX_SIZE = 0x4000000 };
enum { DD_ROM_MAX_SIZE = 0x400000 };

typedef void (*read32fn)(void*,uint32_t,uint32_t*);
typedef void (*write32fn)(void*,uint32_t,uint32_t,uint32_t);

enum core_memory_type { CORE_MEMORY_NOMEM, CORE_MEMORY_NOTHING, CORE_MEMORY_RDRAM, CORE_MEMORY_RDRAMREG, CORE_MEMORY_RSPMEM, CORE_MEMORY_RSPREG, CORE_MEMORY_RSP, CORE_MEMORY_DP, CORE_MEMORY_DPS, CORE_MEMORY_VI, CORE_MEMORY_AI, CORE_MEMORY_PI, CORE_MEMORY_RI, CORE_MEMORY_SI, CORE_MEMORY_FLASHRAMSTAT, CORE_MEMORY_ROM, CORE_MEMORY_PIF, CORE_MEMORY_MI, CORE_MEMORY_BREAKPOINT };

struct mem_handler
{
    void* opaque;
    read32fn read32;
    write32fn write32;
};

struct mem_mapping
{
    uint32_t begin;
    uint32_t end;       /* inclusive */
    int type;
    struct mem_handler handler;
};

struct memory
{
    struct mem_handler handlers[0x10000];
    void* base;

};

static osal_force_inline void masked_write(uint32_t* dst, uint32_t value, uint32_t mask)
{
    *dst = (*dst & ~mask) | (value & mask);
}

void init_memory(struct memory* mem,
                 struct mem_mapping* mappings, size_t mappings_count,
                 void* base,
                 struct mem_handler* dbg_handler);

static osal_force_inline const struct mem_handler* mem_get_handler(const struct memory* mem, uint32_t address)
{
    return &mem->handlers[address >> 16];
}

static osal_force_inline void mem_read32(const struct mem_handler* handler, uint32_t address, uint32_t* value)
{
    handler->read32(handler->opaque, address, value);
}

static osal_force_inline void mem_write32(const struct mem_handler* handler, uint32_t address, uint32_t value, uint32_t mask)
{
    handler->write32(handler->opaque, address, value, mask);
}

void apply_mem_mapping(struct memory* mem, const struct mem_mapping* mapping);

void* init_mem_base(void);
void release_mem_base(void* mem_base);
uint32_t* mem_base_u32(void* mem_base, uint32_t address);

void read_with_bp_checks(void* opaque, uint32_t address, uint32_t* value);
void write_with_bp_checks(void* opaque, uint32_t address, uint32_t value, uint32_t mask);


#endif

