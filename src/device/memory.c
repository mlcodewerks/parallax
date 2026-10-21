/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - memory.c                                                *
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

#include "memory.h"

#include "core/log.h"
#include "core/types.h"

#include "device/device.h"
#include "device/rcp/rsp/rsp_core.h"
#include "device/pif/pif.h"


#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32) || defined(HAVE_LIBNX)
#include <malloc.h>
#endif


void read_with_bp_checks(void* opaque, uint32_t address, uint32_t* value)
{
}

void write_with_bp_checks(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
}

void init_memory(struct memory* mem,
                 struct mem_mapping* mappings, size_t mappings_count,
                 void* base,
                 struct mem_handler* dbg_handler)
{
    size_t m;


    mem->base = base;

    for(m = 0; m < mappings_count; ++m) {
        apply_mem_mapping(mem, &mappings[m]);
    }
}

static void map_region(struct memory* mem,
                       uint16_t region,
                       int type,
                       const struct mem_handler* handler)
{
    {
        (void)type;
        mem->handlers[region] = *handler;
    }
}

void apply_mem_mapping(struct memory* mem, const struct mem_mapping* mapping)
{
    size_t i;
    uint16_t begin = mapping->begin >> 16;
    uint16_t end   = mapping->end   >> 16;

    for (i = begin; i <= end; ++i) {
        map_region(mem, i, mapping->type, &mapping->handler);
    }
}

/* For paraLLEl-RDP which needs to import RDRAM as a host pointer with potentially 64k of alignment. */
enum { MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT = 64 * 1024 };

enum {
    MB_RDRAM_DRAM = 0,
    MB_CART_ROM = MB_RDRAM_DRAM + RDRAM_MAX_SIZE,
    MB_RSP_MEM  = MB_CART_ROM   + CART_ROM_MAX_SIZE,
    MB_DD_ROM   = MB_RSP_MEM    + SP_MEM_SIZE,
    MB_PIF_MEM  = MB_DD_ROM     + DD_ROM_MAX_SIZE,
    MB_MAX_SIZE = MB_PIF_MEM    + PIF_ROM_SIZE + PIF_RAM_SIZE,
    MB_MAX_SIZE_FULL = 0x20000000
};

void *mem_alloc(size_t boundary, size_t size)
{
    void **place;
    uintptr_t addr = 0;
    void *ptr = malloc(boundary + size + sizeof(uintptr_t));
    if (ptr == NULL) return NULL;
    addr = ((uintptr_t)ptr + sizeof(uintptr_t) + boundary) & ~(boundary - 1);
    place = (void **) addr;
    place[-1] = ptr;

    return (void *) addr;
}

void* init_mem_base(void)
{ 
    void* mem_base;
    mem_base = mem_alloc(MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT, MB_MAX_SIZE_FULL);
    core_log(CORE_LOG_INFO, "Using full mem base");
    return mem_base;
}

void release_mem_base(void* mem_base)
{
    if (mem_base != NULL) free(((void**)mem_base)[-1]);
}

uint32_t* mem_base_u32(void* mem_base, uint32_t address)
{
    uint32_t* mem;
    mem = (uint32_t*)((uint8_t*)mem_base + address);
    return mem;
}
