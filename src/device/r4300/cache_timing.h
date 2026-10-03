#ifndef M64P_R4300_CACHE_TIMING_H
#define M64P_R4300_CACHE_TIMING_H
#include "device/memory.h"
#if defined(__GNUC__) || defined(__clang__)
#define R4300_CACHE_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define R4300_CACHE_NOINLINE __declspec(noinline)
#else
#define R4300_CACHE_NOINLINE
#endif

static R4300_CACHE_NOINLINE int r4300_mapped_access_cached(struct r4300_core* r, uint32_t va)
{
    unsigned int i;
    for (i = 0; i < 32; ++i) {
        const struct tlb_entry* e = &r->cp0.tlb.entries[i];
        if (!e->g && e->asid != (r->cp0.regs[CP0_ENTRYHI_REG] & 255)) continue;
        if (e->v_even && va >= e->start_even && va <= e->end_even) return e->c_even != 2;
        if (e->v_odd && va >= e->start_odd && va <= e->end_odd) return e->c_odd != 2;
    }
    return 0;
}

static osal_force_inline int r4300_access_cached(struct r4300_core* r, uint32_t va)
{
    if ((va & 0xe0000000) == 0x80000000)
        return (r->cp0.regs[CP0_CONFIG_REG] & 7) != 2;
    if ((va & 0xe0000000) == 0xa0000000) return 0;
    return r4300_mapped_access_cached(r, va);
}

static inline void r4300_dcache_writeback(struct r4300_core* r, uint32_t va)
{
    unsigned index = (va >> 4) & 511;
    uint32_t pa = (r->dcache_tags[index] & 0x1ffff000) | (va & 0xff0);
    cp0_step_cycles(&r->cp0, 40);
    for (unsigned i = 0; i < 4; ++i)
        mem_write32(mem_get_handler(r->mem, pa), pa + i * 4,
                    r->dcache_words[index][i], ~0u);
    if (r->cached_interp) invalidate_r4300_cached_code(r, pa, 16);
    r->dcache_tags[index] &= ~2u;
}

static R4300_CACHE_NOINLINE void r4300_dcache_refill(struct r4300_core* r,
                                                   uint32_t va, uint32_t pa)
{
    unsigned index = (va >> 4) & 511;
    if ((r->dcache_tags[index] & 3) == 3) r4300_dcache_writeback(r, va);
    cp0_step_cycles(&r->cp0, 40);
    uint32_t line = pa & 0x1ffffff0;
    const struct mem_handler* handler = mem_get_handler(r->mem, line);
    for (unsigned i = 0; i < 4; ++i)
        mem_read32(handler, line + i * 4, &r->dcache_words[index][i]);
    r->dcache_tags[index] = (pa & 0x1ffff000) | 1;
}


static osal_force_inline int r4300_data_access_cycles(struct r4300_core* r, uint32_t va,
                                           uint32_t pa, int write)
{
    uint32_t* tag;
    uint32_t key;
    if (!r->cache_timing) return 0;
    if (!r4300_access_cached(r, va)) {
        /* CEN64 fault.h: distinguish RCP's posted handoff from RAM reads. */
        uint32_t physical = pa & 0x1fffffff;
        unsigned cycles = 38;
        if (physical >= 0x04000000 && physical < 0x05000000)
            cycles = write ? 8 : 21;
        else if (!write && physical < 0x00800000) cycles = 31;
        cp0_step_cycles(&r->cp0, cycles);
        return 0;
    }
    tag = &r->dcache_tags[(va >> 4) & 511];
    key = (pa & 0x1ffff000) | 1;
    if ((*tag & ~2u) != key) {
        r4300_dcache_refill(r, va, pa);
    } else cp0_step_cycles(&r->cp0, 1);
    if (write) *tag |= 2;
    return 1;
}

static osal_force_inline void r4300_fetch_access_cycles(struct r4300_core* r, uint32_t va)
{
    uint32_t pa = va;
    uint32_t* tag;
    uint32_t key;
    if (!r->cache_timing) return;
    if ((va & 0xc0000000) != 0x80000000) {
        uint32_t page = r->cp0.tlb.LUT_r[va >> 12];
        if (!page) return; 
        pa = (page & 0xfffff000) | (va & 0xfff);
    }
    if (!r4300_access_cached(r, va)) {
        cp0_step_cycles(&r->cp0, 40);
        return;
    }
    tag = &r->icache_tags[(va >> 5) & 511];
    key = (pa & 0x1ffff000) | 1;
    if (*tag != key) {
        cp0_step_cycles(&r->cp0, 48);
        *tag = key;
    }
}

static inline void r4300_cache_operation(struct r4300_core* r, unsigned op,
                                        uint32_t va, uint32_t pa)
{
    uint32_t* tag = (op & 1) ? &r->dcache_tags[(va >> 4) & 511]
                             : &r->icache_tags[(va >> 5) & 511];
    uint32_t key = (pa & 0x1ffff000) | 1;
    int hit = (*tag & ~2u) == key;
    if (!r->cache_timing) return;
    switch (op) {
    case 0x00: *tag &= ~1u; break;
    case 0x01:
        if ((*tag & 3) == 3) r4300_dcache_writeback(r, va);
        *tag &= ~1u;
        break;
    case 0x04: case 0x05:
        r->cp0.regs[CP0_TAGLO_REG] = ((*tag & 0x1ffff000) >> 4) |
            ((*tag & 1) ? ((op & 1) ? 0xc0 : 0x80) : 0);
        break;
    case 0x08: case 0x09:
        *tag = ((r->cp0.regs[CP0_TAGLO_REG] << 4) & 0x1ffff000) |
            ((r->cp0.regs[CP0_TAGLO_REG] & 0x80) ? 1 : 0) |
            ((op & 1) && (r->cp0.regs[CP0_TAGLO_REG] & 0x40) ? 2 : 0);
        break;
    case 0x0d:
        if (!hit && (*tag & 3) == 3) r4300_dcache_writeback(r, va);
        *tag = key; /* Newly allocated line has no written bytes yet. */
        break;
    case 0x10: case 0x11: if (hit) *tag &= ~1u; break;
    case 0x14: cp0_step_cycles(&r->cp0, 48); *tag = key; break;
    case 0x15:
        if (hit) {
            if (*tag & 2) r4300_dcache_writeback(r, va);
            *tag &= ~1u;
        }
        break;
    case 0x18: if (hit) cp0_step_cycles(&r->cp0, 48); break;
    case 0x19:
        if (hit && (*tag & 2)) r4300_dcache_writeback(r, va);
        break;
    }
}
#undef R4300_CACHE_NOINLINE
#endif
