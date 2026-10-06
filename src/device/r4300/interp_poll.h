#ifndef M64P_R4300_INTERP_POLL_H
#define M64P_R4300_INTERP_POLL_H

#include "timing.h"
#include "device/rdram.h"

/* Two-block RAM flag waits: LUI/ADDIU/LBU/ANDI/BEQ/NOP, followed by
 * LUI/LW/BEQ/NOP. Only unchanged, fully cached iterations can be elided.
 * The plan is decoded interpreter data; it is rebuilt after invalidation. */
struct interp_ram_poll {
    uint32_t pc, target, byte_address, word_address, mask;
    int64_t base_value;
    unsigned char base_reg, byte_reg, mask_reg, word_reg;
};

static int interp_ram_poll_decode(struct interp_ram_poll* p, uint32_t pc,
    const uint32_t* first, uint32_t target, const uint32_t* second)
{
    unsigned a = (first[0] >> 16) & 31;
    unsigned b = (first[2] >> 16) & 31;
    unsigned c = (first[3] >> 16) & 31;
    unsigned d = (second[0] >> 16) & 31;
    if ((pc & UINT32_C(0xe0000000)) != UINT32_C(0x80000000) ||
        (pc >> 12) != (target >> 12) || (pc & 4095) > 4096 - 24 ||
        (target & 4095) > 4096 - 16 || !a || !b || !c || !d ||
        a == b || a == c || a == d || b == c || b == d || c == d)
        return 0;
    if ((first[0] & UINT32_C(0xffe00000)) != UINT32_C(0x3c000000) ||
        (first[1] & UINT32_C(0xffff0000)) != (UINT32_C(0x24000000) | a << 21 | a << 16) ||
        (first[2] & UINT32_C(0xffe00000)) != (UINT32_C(0x90000000) | a << 21) ||
        (first[3] & UINT32_C(0xffe00000)) != (UINT32_C(0x30000000) | b << 21) ||
        (first[4] & UINT32_C(0xffff0000)) != (UINT32_C(0x10000000) | c << 21) ||
        first[5] ||
        pc + 20 + (int32_t)(int16_t)first[4] * 4 != target ||
        (second[0] & UINT32_C(0xffe00000)) != UINT32_C(0x3c000000) ||
        (second[1] & UINT32_C(0xffff0000)) != (UINT32_C(0x8c000000) | d << 21 | d << 16) ||
        (second[2] & UINT32_C(0xffff0000)) != (UINT32_C(0x10000000) | d << 21) ||
        target + 12 + (int32_t)(int16_t)second[2] * 4 != pc || second[3])
        return 0;
    p->pc = pc; p->target = target;
    p->base_value = (int64_t)(int32_t)((first[0] & 65535) << 16);
    p->base_value = (int64_t)(int32_t)((uint32_t)p->base_value + (int32_t)(int16_t)first[1]);
    p->byte_address = (uint32_t)p->base_value + (int32_t)(int16_t)first[2];
    p->word_address = ((second[0] & 65535) << 16) + (int32_t)(int16_t)second[1];
    p->mask = first[3] & 65535;
    p->base_reg = a; p->byte_reg = b; p->mask_reg = c; p->word_reg = d;
    return 1;
}

static int interp_ram_poll_fetch_hits(const struct r4300_core* r, uint32_t pc, unsigned n)
{
    for (unsigned i = 0; i < n; ++i, pc += 4)
        if (r->icache_tags[(pc >> 5) & 511] != ((pc & UINT32_C(0x1ffff000)) | 1))
            return 0;
    return 1;
}

static int interp_ram_poll_read(const struct r4300_core* r, uint32_t address, uint32_t* word)
{
    uint32_t physical = address & UINT32_C(0x1fffffff);
    if ((address & UINT32_C(0xe0000000)) != UINT32_C(0x80000000) ||
        physical >= UINT32_C(0x800000)) return 0;
    const struct mem_handler* h = &r->mem->handlers[physical >> 16];
    if (h->read32 != read_rdram_dram || !h->opaque ||
        physical >= ((const struct rdram*)h->opaque)->dram_size) return 0;
    unsigned index = (address >> 4) & 511;
    if ((r->dcache_tags[index] & ~2u) != ((physical & UINT32_C(0x1ffff000)) | 1)) return 0;
    *word = r->dcache_words[index][(physical >> 2) & 3];
    return 1;
}

static int interp_ram_poll_advance(struct r4300_core* r, const struct interp_ram_poll* p)
{
    uint32_t byte, word;
    uint32_t status = r->cp0.regs[CP0_STATUS_REG];
    if (!r->cache_timing || (r->cp0.regs[CP0_CONFIG_REG] & 7) == 2 ||
        r->delay_slot || r->skip_jump || r->cp0.cycle_count >= -11 ||
        ((status & CP0_STATUS_MODE_MASK) && !(status & (CP0_STATUS_EXL | CP0_STATUS_ERL))) ||
        r->pipeline_load_mask || r->pipeline_cached_store ||
        r->pipeline_fpu_result_mask || r->pipeline_fpu_cc ||
        r->regs[p->base_reg] != p->base_value || r->regs[p->mask_reg] || r->regs[p->word_reg] ||
        (p->word_address & 3) ||
        !interp_ram_poll_fetch_hits(r, p->pc, 6) ||
        !interp_ram_poll_fetch_hits(r, p->target, 4) ||
        !interp_ram_poll_read(r, p->byte_address, &byte) ||
        !interp_ram_poll_read(r, p->word_address, &word))
        return 0;
    byte = (byte >> ((3 - (p->byte_address & 3)) * 8)) & 255;
    if ((byte & p->mask) || word || r->regs[p->byte_reg] != byte) return 0;

    /* Ten one-cycle instructions, ten compatibility issue clocks, and
     * two D-cache hits. Each possible LDI is one clock and overlaps the
     * reservation. Both NOP slots leave pipeline history empty. Elide
     * whole iterations strictly before the next event, then execute the
     * final iteration normally for the exact PC, delay slot and phase. */
    const unsigned cycles = 20 + 2 * R4300_DCACHE_HIT_CYCLES;
    uint64_t iterations = (uint64_t)(-(r->cp0.cycle_count + 1)) / (cycles / 2);
    if (iterations > UINT32_MAX / cycles) iterations = UINT32_MAX / cycles;
    if (!iterations) return 0;
    cp0_step_cycles(&r->cp0, (unsigned)iterations * cycles);
    r->cp0.last_addr = p->pc;
    return 1;
}

#endif
