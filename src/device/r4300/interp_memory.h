/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - interp_memory.h                                           *
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

#ifndef M64P_R4300_INTERP_MEMORY_H
#define M64P_R4300_INTERP_MEMORY_H
#include "r4300_core.h"
#include "timing.h"
#include "tlb.h"
#include "device/memory.h"
#include "device/rdram.h"
#include "api/callbacks.h"

#if defined(__GNUC__) || defined(__clang__)
#define INTERP_MEM_LIKELY(x) __builtin_expect(!!(x), 1)
#define INTERP_MEM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define INTERP_MEM_LIKELY(x) (x)
#define INTERP_MEM_UNLIKELY(x) (x)
#endif

/* Interpreter-side TLB hit path. The existing translator remains the sole
 * miss/exception path, so refill behavior and NEW_DYNAREC validation stay
 * unchanged. w == 1 selects the writable LUT; all other values use read LUT. */
static osal_force_inline uint32_t interp_translate_address(struct r4300_core* r4300,
                                                           uint32_t address,
                                                           int w)
{
#if defined(NEW_DYNAREC)
    if (INTERP_MEM_UNLIKELY(r4300->emumode == EMUMODE_DYNAREC))
        return virtual_to_physical_address(r4300, address, w);

#endif

    const uint32_t page = address >> 12;
    const uint32_t mapped = (w == 1)
        ? r4300->cp0.tlb.LUT_w[page]
        : r4300->cp0.tlb.LUT_r[page];

    if (INTERP_MEM_LIKELY(mapped != 0))
        return (mapped & UINT32_C(0xfffff000)) | (address & UINT32_C(0x00000fff));

    return virtual_to_physical_address(r4300, address, w);
}

/* Keep the dynamic memory map authoritative. Normal RDRAM is overwhelmingly
 * common for Conker, so inline its tiny handler after confirming the currently
 * installed handler is the normal one. Debug/breakpoint/corruption handlers
 * still take the generic dispatch path. */
static osal_force_inline void interp_mem_read32(const struct mem_handler* handler,
                                                uint32_t address,
                                                uint32_t* value)
{
    if (INTERP_MEM_LIKELY(handler->read32 == read_rdram_dram))
    {
        const struct rdram* rdram = (const struct rdram*)handler->opaque;
        *value = rdram->dram[rdram_dram_address(address)];
        return;
    }

    handler->read32(handler->opaque, address, value);
}

static osal_force_inline void interp_mem_write32(const struct mem_handler* handler,
                                                 uint32_t address,
                                                 uint32_t value,
                                                 uint32_t mask)
{
    if (INTERP_MEM_LIKELY(handler->write32 == write_rdram_dram))
    {
        struct rdram* rdram = (struct rdram*)handler->opaque;
        masked_write(&rdram->dram[rdram_dram_address(address)], value, mask);
        return;
    }

    handler->write32(handler->opaque, address, value, mask);
}

static osal_force_inline int interp_read_aligned_word(struct r4300_core* r4300,
                                                      uint32_t address,
                                                      uint32_t* value)
{
    const uint32_t virtual_address = address;
    if (INTERP_MEM_UNLIKELY((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)))
    {
        address = interp_translate_address(r4300, address, 0);
        if (INTERP_MEM_UNLIKELY(address == 0))
            return 0;
    }

    if (r4300_data_access_cycles(r4300, virtual_address, address, 0)) {
        *value = r4300->dcache_words[(virtual_address >> 4) & 511][(address >> 2) & 3];
        return 1;
    }
    address &= UINT32_C(0x1ffffffc);
    interp_mem_read32(&r4300->mem->handlers[address >> 16], address, value);
    return 1;
}

static osal_force_inline int interp_read_aligned_dword(struct r4300_core* r4300,
                                                       uint32_t address,
                                                       uint64_t* value)
{
    const uint32_t virtual_address = address;
    uint32_t w0;
    uint32_t w1;

    if (INTERP_MEM_UNLIKELY((address & UINT32_C(7)) != 0))
        DebugMessage(M64MSG_WARNING, "Unaligned dword read %08x", address);

    if (INTERP_MEM_UNLIKELY((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)))
    {
        address = interp_translate_address(r4300, address, 0);
        if (INTERP_MEM_UNLIKELY(address == 0))
            return 0;
    }

    if (r4300_data_access_cycles(r4300, virtual_address, address, 0)) {
        uint32_t* words = r4300->dcache_words[(virtual_address >> 4) & 511];
        *value = ((uint64_t)words[(address >> 2) & 3] << 32) | words[((address >> 2) & 3) + 1];
        return 1;
    }
    address &= UINT32_C(0x1ffffffc);
    const struct mem_handler* handler = &r4300->mem->handlers[address >> 16];
    interp_mem_read32(handler, address + 0, &w0);
    interp_mem_read32(handler, address + 4, &w1);
    *value = ((uint64_t)w0 << 32) | w1;
    return 1;
}

static osal_force_inline int interp_write_aligned_word(struct r4300_core* r4300,
                                                       uint32_t address,
                                                       uint32_t value,
                                                       uint32_t mask)
{
    const uint32_t virtual_address = address;
    if (INTERP_MEM_UNLIKELY((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)))
    {
        address = interp_translate_address(r4300, address, 1);
        if (INTERP_MEM_UNLIKELY(address == 0))
            return 0;
    }

    /* Translation is complete: a clear physical-page bit means no decoded
     * instruction can be invalidated by this store. Page generations still
     * protect all old blocks after the first write clears that bit. */
    uint32_t code_page = (address & UINT32_C(0x1fffffff)) >> 12;
    if (INTERP_MEM_UNLIKELY(r4300->cached_code_pages != NULL &&
        ((r4300->cached_code_pages[code_page >> 6] >> (code_page & 63)) & 1)))
        invalidate_r4300_cached_code(r4300, address, 4);

    if (r4300_data_access_cycles(r4300, virtual_address, address, 1)) {
        masked_write(&r4300->dcache_words[(virtual_address >> 4) & 511][(address >> 2) & 3], value, mask);
        return 1;
    }
    address &= UINT32_C(0x1ffffffc);
    interp_mem_write32(&r4300->mem->handlers[address >> 16], address, value, mask);
    return 1;
}

static osal_force_inline int interp_write_aligned_dword(struct r4300_core* r4300,
                                                        uint32_t address,
                                                        uint64_t value,
                                                        uint64_t mask)
{
    const uint32_t virtual_address = address;
    if (INTERP_MEM_UNLIKELY((address & UINT32_C(7)) != 0))
        DebugMessage(M64MSG_WARNING, "Unaligned dword write %08x", address);

    if (INTERP_MEM_UNLIKELY((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)))
    {
        address = interp_translate_address(r4300, address, 1);
        if (INTERP_MEM_UNLIKELY(address == 0))
            return 0;
    }

    uint32_t code_page = (address & UINT32_C(0x1fffffff)) >> 12;
    if (INTERP_MEM_UNLIKELY(r4300->cached_code_pages != NULL &&
        ((r4300->cached_code_pages[code_page >> 6] >> (code_page & 63)) & 1)))
        invalidate_r4300_cached_code(r4300, address, 8);

    if (r4300_data_access_cycles(r4300, virtual_address, address, 1)) {
        uint32_t* words = r4300->dcache_words[(virtual_address >> 4) & 511];
        masked_write(&words[(address >> 2) & 3], (uint32_t)(value >> 32), (uint32_t)(mask >> 32));
        masked_write(&words[((address >> 2) & 3) + 1], (uint32_t)value, (uint32_t)mask);
        return 1;
    }
    address &= UINT32_C(0x1ffffffc);
    const struct mem_handler* handler = &r4300->mem->handlers[address >> 16];
    interp_mem_write32(handler, address + 0, (uint32_t)(value >> 32), (uint32_t)(mask >> 32));
    interp_mem_write32(handler, address + 4, (uint32_t)value, (uint32_t)mask);
    return 1;
}

static osal_force_inline int check_alignment(struct r4300_core* r4300, uint32_t address, unsigned int mask, int store)
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

/* Shared constructors keep both dispatchers on the same exception/access path. */
static osal_force_inline void interp_load_word(struct r4300_core* r, uint32_t op, int sign)
{
    unsigned rt = (op >> 16) & 31;
    uint32_t address = (uint32_t)r->regs[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)op;
    if (check_alignment(r, address, 3, 0)) return;
    r->interp_PC.addr += 4;
    uint32_t value;
    if (interp_read_aligned_word(r, address, &value))
        r->regs[rt] = sign ? (int64_t)(int32_t)value : (int64_t)value;
}

static osal_force_inline void interp_store_word(struct r4300_core* r, uint32_t op)
{
    unsigned rt = (op >> 16) & 31;
    uint32_t address = (uint32_t)r->regs[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)op;
    if (check_alignment(r, address, 3, 1)) return;
    r->interp_PC.addr += 4;
    interp_write_aligned_word(r, address, (uint32_t)r->regs[rt], ~UINT32_C(0));
}

static osal_force_inline int interp_word_memory(struct r4300_core* r, uint32_t op)
{
    unsigned major = op >> 26;
    if (major == 35 || major == 39) interp_load_word(r, op, major == 35);
    else if (major == 43) interp_store_word(r, op);
    else return 0;
    return 1;
}

#undef INTERP_MEM_LIKELY
#undef INTERP_MEM_UNLIKELY

#endif
