/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PARALLAX_INTERP_THREAD_POLL_H
#define PARALLAX_INTERP_THREAD_POLL_H
#include <string.h>
#include "timing.h"
#include "pure_interp.h"
#include "device/memory.h"
#include "device/rdram.h"
#include "device/rcp/rcp.h"

/* An SDK thread-ID wait calls a getter that disables interrupts, reads the
 * running thread's ID, restores interrupts and compares a RAM flag. Recognize
 * the instruction sequence, never a ROM ID or fixed guest address. Detailed
 * timing keeps ordinary execution; only steady, untimed iterations qualify. */
static const uint32_t* interp_thread_poll_ram(const struct r4300_core* r,
    uint32_t address, unsigned words, int writing)
{
    uint32_t physical = address & UINT32_C(0x1fffffff);
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000) ||
        (address & 3) || physical >= UINT32_C(0x800000) ||
        words * 4 > UINT32_C(0x800000) - physical) return NULL;
    const struct mem_handler* h = &r->mem->handlers[physical >> 16];
    if (h->read32 != read_rdram_dram || !h->opaque ||
        (writing && h->write32 != write_rdram_dram) ||
        words * 4 > ((const struct rdram*)h->opaque)->dram_size ||
        physical > ((const struct rdram*)h->opaque)->dram_size - words * 4 ||
        (physical >> 16) != ((physical + words * 4 - 1) >> 16)) return NULL;
    if (writing && r->cached_code_pages) {
        for (uint32_t page = physical >> 12; page <= (physical + words * 4 - 1) >> 12; ++page)
            if ((r->cached_code_pages[page >> 6] >> (page & 63)) & 1) return NULL;
    }
    return ((const struct rdram*)h->opaque)->dram + (physical >> 2);
}

static int interp_thread_poll_shape(uint32_t pc, const uint32_t* code)
{
    unsigned rt = (code[2] >> 16) & 31;
    return (pc & 4095) <= 4096 - 20 && (code[0] >> 26) == 3 && code[1] == 0 &&
        (code[2] >> 26) == 35 && rt && rt != 2 && rt != 29 && rt != 31 &&
        (code[3] & UINT32_C(0xffff0000)) == (UINT32_C(0x10400000) | rt << 16) &&
        (int16_t)code[3] == -4 && code[4] == 0;
}

static uint32_t interp_thread_poll_call(uint32_t pc, uint32_t op)
{
    return ((pc + 4) & UINT32_C(0xf0000000)) | ((op & UINT32_C(0x03ffffff)) << 2);
}

/* Decode all three SDK routines before classifying a block as a poll. The
 * five-word caller alone also matches ordinary gameplay calls; marking those
 * as polls would unnecessarily prevent native successor links. Runtime
 * advancement repeats this check so changes outside the caller's code page
 * cannot make a stale classification skip different instructions. */
static const uint32_t* interp_thread_poll_getter(const struct r4300_core* r,
    uint32_t pc, const uint32_t* loop)
{
    uint32_t getter_pc = interp_thread_poll_call(pc, loop[0]);
    const uint32_t* getter = interp_thread_poll_ram(r, getter_pc, 16, 0);
    if (!getter || getter[0] != 0x27bdffd8 || getter[1] != 0xafbf001c ||
        (getter[2] >> 26) != 3 || getter[3] != 0xafb00018 ||
        (getter[4] & 0xffff0000) != 0x3c0e0000 || (getter[5] & 0xffff0000) != 0x8dce0000 ||
        getter[6] != 0x00408025 || getter[7] != 0x02002025 || getter[8] != 0x8dcf0004 ||
        (getter[9] >> 26) != 3 || getter[10] != 0xafaf0020 || getter[11] != 0x8fbf001c ||
        getter[12] != 0x8fa20020 || getter[13] != 0x8fb00018 || getter[14] != 0x03e00008 ||
        getter[15] != 0x27bd0028) return NULL;
    const uint32_t* disable = interp_thread_poll_ram(r, interp_thread_poll_call(getter_pc + 8, getter[2]), 8, 0);
    const uint32_t* restore = interp_thread_poll_ram(r, interp_thread_poll_call(getter_pc + 36, getter[9]), 7, 0);
    static const uint32_t disable_ops[] = {0x40086000,0x2401fffe,0x01014824,0x40896000,0x31020001,0,0x03e00008,0};
    static const uint32_t restore_ops[] = {0x40086000,0x01044025,0x40886000,0,0,0x03e00008,0};
    if (!disable || !restore || memcmp(disable, disable_ops, sizeof(disable_ops)) ||
        memcmp(restore, restore_ops, sizeof(restore_ops))) return NULL;
    return getter;
}

static int interp_thread_poll_advance(struct r4300_core* r, uint32_t pc)
{
    const unsigned cycles = 36 * (1 + (r->compat_issue_clock != 0));
    uint32_t status = r->cp0.regs[CP0_STATUS_REG];
    if (r->cache_timing || r->delay_slot || r->skip_jump || breakloop ||
        r->cp0.cycle_count >= -(int)(cycles / 2) || r->cp0.last_addr != pc ||
        r->regs[1] != -2 || r->regs[31] != (int64_t)(int32_t)(pc + 8) ||
        ((status & CP0_STATUS_MODE_MASK) && !(status & (CP0_STATUS_EXL | CP0_STATUS_ERL))) ||
        r->pipeline_load_mask || r->pipeline_fpu_result_mask || r->pipeline_fpu_cc || r->pipeline_cached_store)
        return 0;
    const uint32_t* loop = interp_thread_poll_ram(r, pc, 5, 0);
    if (!loop || !interp_thread_poll_shape(pc, loop)) return 0;
    const uint32_t* getter = interp_thread_poll_getter(r, pc, loop);
    if (!getter) return 0;

    uint32_t control = status & UINT32_C(0xff77ffff);
    uint32_t final_status = (control & UINT32_C(0xff57ffff)) | (status & CP0_STATUS_TS);
    uint32_t pending = r->mi->regs[MI_INTR_REG] & r->mi->regs[MI_INTR_MASK_REG];
    if (status != final_status || !!(r->cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_IP2) != !!pending ||
        ((status & (CP0_STATUS_IE | CP0_STATUS_EXL | CP0_STATUS_ERL)) == CP0_STATUS_IE &&
         (status & r->cp0.regs[CP0_CAUSE_REG] & UINT32_C(0xff00)))) return 0;
    uint32_t pointer_address = (getter[4] << 16) + (int32_t)(int16_t)getter[5];
    const uint32_t* pointer = interp_thread_poll_ram(r, pointer_address, 1, 0);
    if (!pointer) return 0;
    const uint32_t* id = interp_thread_poll_ram(r, *pointer + 4, 1, 0);
    unsigned flag_reg = (loop[2] >> 16) & 31;
    uint32_t flag_address = (uint32_t)r->regs[(loop[2] >> 21) & 31] + (int32_t)(int16_t)loop[2];
    const uint32_t* flag = interp_thread_poll_ram(r, flag_address, 1, 0);
    const uint32_t* stack = interp_thread_poll_ram(r, (uint32_t)r->regs[29] - 40, 9, 1);
    if (!id || !flag || !stack || *id != *flag ||
        r->regs[0] || r->regs[1] != -2 || r->regs[2] != (int64_t)(int32_t)*id ||
        r->regs[4] != (control & 1) || r->regs[8] != (int64_t)(int32_t)control ||
        r->regs[9] != (int64_t)(int32_t)(control & ~1u) ||
        r->regs[14] != (int64_t)(int32_t)*pointer || r->regs[15] != (int64_t)(int32_t)*id ||
        r->regs[16] != (int64_t)(int32_t)r->regs[16] ||
        r->regs[29] != (int64_t)(int32_t)r->regs[29] ||
        r->regs[31] != (int64_t)(int32_t)(pc + 8) || r->regs[flag_reg] != (int64_t)(int32_t)*id ||
        r->cp0.latch != (uint64_t)(int64_t)(int32_t)control ||
        stack[6] != (uint32_t)r->regs[16] || stack[7] != pc + 8 || stack[8] != *id) return 0;
    /* All registers, stack words, STATUS/latch and IRQ state are already the
     * exact steady-state result. No instruction reads COUNT or a device.
     * Advance whole even-clock iterations strictly before the next event. */
    uint64_t iterations = (uint64_t)(-(r->cp0.cycle_count + 1)) / (cycles / 2);
    if (iterations > UINT32_MAX / cycles) iterations = UINT32_MAX / cycles;
    if (!iterations) return 0;
    cp0_step_cycles(&r->cp0, (unsigned)iterations * cycles);
    return 1;
}
#endif
