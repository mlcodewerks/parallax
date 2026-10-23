/* RCP interfaces: AI, MI, PI, RI, SI and VI. RDP/RSP remain separate. */
#include "rcp.h"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - ai_controller.c                                         *
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


#include <string.h>

#include "core/audio.h"
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/rcp.h"
#include "device/rcp/rcp.h"
#include "device/rcp/rcp.h"
#include "device/rdram.h"


#define AI_STATUS_BUSY UINT32_C(0x40000000)
#define AI_STATUS_FULL UINT32_C(0x80000000)

static void ai_set_format(struct ai_controller* ai)
{
    if (ai->samples_format_changed) {
        unsigned frequency = ai->regs[AI_DACRATE_REG]
            ? ai->vi->clock / (1u + ai->regs[AI_DACRATE_REG]) : 44100;
        ai->iaout->set_frequency(ai->aout, frequency);
        ai->samples_format_changed = 0;
    }
}

/* DAC dividers are video-clock periods, while the event queue and monotonic
 * CPU clock use 46.875 MHz COUNT ticks. Keep device phase in video clocks and
 * convert absolute DMA endpoints to COUNT, avoiding per-buffer rounding drift. */
static int64_t ai_clock(const struct ai_controller* ai)
{
    int64_t count = ai->mi->r4300->cp0.count_clock;
    return count / 46875000 * ai->vi->clock +
        count % 46875000 * ai->vi->clock / 46875000;
}

static int64_t ai_count_deadline(const struct ai_controller* ai, int64_t clock)
{
    return clock / ai->vi->clock * 46875000 +
        (clock % ai->vi->clock * 46875000 + ai->vi->clock - 1) / ai->vi->clock;
}

static void ai_emit_idle(struct ai_controller* ai, int64_t now)
{
    int64_t elapsed = now - ai->idle_clock;
    unsigned divider = ai->regs[AI_DACRATE_REG] + 1u;
    if (elapsed <= 0) return;
    ai_set_format(ai);
    uint64_t ticks = (uint64_t)elapsed;
    if (!ai->regs[AI_DACRATE_REG]) {
        ticks *= 44100;
        divider = ai->vi->clock;
    }
    ticks += ai->idle_phase;
    size_t frames = (size_t)(ticks / divider);
    ai->idle_phase = ticks % divider;
    ai->idle_clock = now;
    if (frames && ai->iaout->push_idle_samples) {
#ifdef AI_TIMING_OBSERVE
        AI_TIMING_OBSERVE(ai, "idle", frames);
#endif
        ai->iaout->push_idle_samples(ai->aout, frames);
    }
}

static uint32_t get_remaining_dma_length(struct ai_controller* ai)
{
    if (!(ai->regs[AI_STATUS_REG] & AI_STATUS_BUSY) || !ai->fifo[0].duration)
        return 0;
    cp0_update_count(ai->mi->r4300);
    int64_t elapsed = ai_clock(ai) - ai->dma_start_clock;
    if (elapsed <= 0) return ai->fifo[0].length;
    if (elapsed >= ai->fifo[0].duration) return 0;
    uint64_t consumed = (uint64_t)elapsed * (ai->fifo[0].length / 4) / ai->fifo[0].duration;
    return ai->fifo[0].length - (uint32_t)consumed * 4;
}

static unsigned int get_dma_duration(struct ai_controller* ai)
{
    unsigned int bytes_per_sample = 4; /* 16-bit stereo */
    unsigned int divider = 1 + ai->regs[AI_DACRATE_REG];

    if (!ai->regs[AI_DACRATE_REG])
        return (unsigned int)((uint64_t)(ai->regs[AI_LEN_REG] / bytes_per_sample)
                              * ai->vi->clock / 44100);

    if (divider == 0)
        return 0;
    return (unsigned int)(((uint64_t)ai->regs[AI_LEN_REG] * divider)
                          / bytes_per_sample);
}


static void do_dma(struct ai_controller* ai, struct ai_dma* dma, int64_t start)
{
    ai_set_format(ai);
    ai->dma_start_clock = start;

    ai->last_read = dma->length;

    if (ai->delayed_carry) dma->address += 0x2000;

    if (((dma->address + dma->length) & 0x1FFF) == 0)
        ai->delayed_carry = 1;
    else
        ai->delayed_carry = 0;

    /* schedule end of dma event */
    cp0_update_count(ai->mi->r4300);
    struct cp0* cp0 = &ai->mi->r4300->cp0;
    add_interrupt_event_count(cp0, AI_INT, cp0->regs[CP0_COUNT_REG] +
        (uint32_t)(ai_count_deadline(ai, start + dma->duration) - cp0->count_clock));
#ifdef AI_TIMING_OBSERVE
    AI_TIMING_OBSERVE(ai, "dma", dma->length / 4);
#endif
}

static void fifo_push(struct ai_controller* ai)
{
    unsigned int duration = get_dma_duration(ai);

    if (ai->regs[AI_STATUS_REG] & AI_STATUS_BUSY)
    {
        ai->fifo[1].address = ai->regs[AI_DRAM_ADDR_REG] & UINT32_C(0x00fffff8);
        ai->fifo[1].length = ai->regs[AI_LEN_REG] & ~UINT32_C(7);
        ai->fifo[1].duration = duration;
        ai->regs[AI_STATUS_REG] |= AI_STATUS_FULL;
    }
    else
    {
        cp0_update_count(ai->mi->r4300);
        ai_emit_idle(ai, ai_clock(ai));
        ai->fifo[0].address = ai->regs[AI_DRAM_ADDR_REG] & UINT32_C(0x00fffff8);
        ai->fifo[0].length = ai->regs[AI_LEN_REG] & ~UINT32_C(7);
        ai->fifo[0].duration = duration;
        ai->regs[AI_STATUS_REG] |= AI_STATUS_BUSY;

        do_dma(ai, &ai->fifo[0], ai_clock(ai) -
            (ai->regs[AI_DACRATE_REG] ? ai->idle_phase : ai->idle_phase / 44100));
        ai->idle_phase = 0;
    }
}

static void fifo_pop(struct ai_controller* ai)
{
    int64_t end = ai->dma_start_clock + ai->fifo[0].duration;
    if (ai->regs[AI_STATUS_REG] & AI_STATUS_FULL)
    {
        ai->fifo[0].address = ai->fifo[1].address;
        ai->fifo[0].length = ai->fifo[1].length;
        ai->fifo[0].duration = ai->fifo[1].duration;
        ai->regs[AI_STATUS_REG] &= ~AI_STATUS_FULL;

        do_dma(ai, &ai->fifo[0], end);
    }
    else
    {
        ai->regs[AI_STATUS_REG] &= ~AI_STATUS_BUSY;
        ai->idle_clock = end;
        ai->idle_phase = 0;
    }
}


void init_ai(struct ai_controller* ai,
             struct mi_controller* mi,
             struct ri_controller* ri,
             struct vi_controller* vi,
             void* aout,
             const struct audio_out_backend_interface* iaout)
{
    ai->mi = mi;
    ai->ri = ri;
    ai->vi = vi;
    ai->aout = aout;
    ai->iaout = iaout;
}

void poweron_ai(struct ai_controller* ai)
{
    memset(ai->regs, 0, AI_REGS_COUNT*sizeof(uint32_t));
    memset(ai->fifo, 0, AI_DMA_FIFO_SIZE*sizeof(struct ai_dma));
    ai->samples_format_changed = 1;
    ai->last_read = 0;
    ai->delayed_carry = 0;
    ai->dma_start_clock = ai->idle_clock = ai_clock(ai);
    ai->idle_phase = 0;
}

static void ai_push_span(struct ai_controller* ai, unsigned int diff,
                         unsigned int length)
{
    size_t dram_size = ai->ri->rdram->dram_size;
    uint32_t start = ai->fifo[0].address & ~UINT32_C(7);

    if (start >= dram_size || diff > dram_size - start)
        return;

    if (length > dram_size - start - diff)
        length = (unsigned int)(dram_size - start - diff);

    if (length < 4)
        return;

    ai->iaout->push_samples(ai->aout,
        (unsigned char*)ai->ri->rdram->dram + start + diff, length);
}

static unsigned int ai_hand_over_played(struct ai_controller* ai, uint32_t remaining)
{
    unsigned int diff;
    unsigned int handed;

    if (remaining >= ai->last_read)
        return 0;

    diff = ai->fifo[0].length - ai->last_read;
    handed = ai->last_read - remaining;
    ai_push_span(ai, diff, handed);
    ai->last_read = remaining;
    return handed;
}

void ai_flush_samples(struct ai_controller* ai)
{
    cp0_update_count(ai->mi->r4300);
    if (ai->regs[AI_STATUS_REG] & AI_STATUS_BUSY)
        ai_hand_over_played(ai, get_remaining_dma_length(ai));
    else
        ai_emit_idle(ai, ai_clock(ai));
}

void ai_rebase_timing(struct ai_controller* ai)
{
    struct cp0* cp0 = &ai->mi->r4300->cp0;
    unsigned *event = get_event(&cp0->q, AI_INT);
    ai->idle_clock = ai_clock(ai);
    ai->idle_phase = 0;
    ai->dma_start_clock = ai->idle_clock - ai->fifo[0].duration;
    if (event) {
        int64_t end = cp0->count_clock + (int32_t)(*event - cp0->regs[CP0_COUNT_REG]);
        ai->dma_start_clock = end / 46875000 * ai->vi->clock +
            end % 46875000 * ai->vi->clock / 46875000 - ai->fifo[0].duration;
    }
    ai->samples_format_changed = 1;
}

void read_ai_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct ai_controller* ai = (struct ai_controller*)opaque;
    uint32_t reg = ai_reg(address);

    if (reg == AI_LEN_REG)
    {
        *value = get_remaining_dma_length(ai);
        ai_hand_over_played(ai, *value);
    }
    else
    {
        *value = ai->regs[reg];
    }
}

void write_ai_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct ai_controller* ai = (struct ai_controller*)opaque;
    uint32_t reg = ai_reg(address);

    switch (reg)
    {
    case AI_LEN_REG:
        if (ai->regs[AI_STATUS_REG] & AI_STATUS_FULL) return;
        masked_write(&ai->regs[AI_LEN_REG], value, mask);
        if (ai->regs[AI_LEN_REG] != 0) {
            fifo_push(ai);
        }
        else {
            /* stop sound */
        }
        return;

    case AI_STATUS_REG:
        clear_rcp_interrupt(ai->mi, MI_INTR_AI);
        return;

    case AI_DACRATE_REG:
        ai_flush_samples(ai);
        if ((ai->regs[reg] & mask) != (value & mask)) {
            ai->samples_format_changed = 1;
            ai->idle_phase = 0; 
        }

        masked_write(&ai->regs[reg], value, mask);
        return;
    }

    masked_write(&ai->regs[reg], value, mask);
}

void ai_end_of_dma_event(void* opaque)
{
    struct ai_controller* ai = (struct ai_controller*)opaque;

    if (ai->last_read != 0)
    {
        unsigned int diff = ai->fifo[0].length - ai->last_read;
        ai_push_span(ai, diff, ai->last_read);
        ai->last_read = 0;
    }

    fifo_pop(ai);
    raise_rcp_interrupt(ai->mi, MI_INTR_AI);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mi_controller.c                                         *
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


#include <string.h>

#include "device/r4300/cp0.h"
#include "device/r4300/interrupt.h"
#include "device/r4300/r4300_core.h"

static int update_mi_init_mode(uint32_t* mi_init_mode, uint32_t w)
{
    int clear_dp = 0;

    /* set init_length */
    *mi_init_mode &= ~0x7f;
    *mi_init_mode |= w & 0x7f;
    /* clear / set init_mode */
    if (w & 0x80)  *mi_init_mode &= ~0x80;
    if (w & 0x100) *mi_init_mode |= 0x80;
    /* clear / set ebus test_mode */
    if (w & 0x200) *mi_init_mode &= ~0x100;
    if (w & 0x400) *mi_init_mode |= 0x100;
    /* clear DP interrupt */
    if (w & 0x800) clear_dp = 1;
    /* clear / set RDRAM reg_mode */
    if (w & 0x1000) *mi_init_mode &= ~0x200;
    if (w & 0x2000) *mi_init_mode |= 0x200;

    return clear_dp;
}

static void update_mi_intr_mask(uint32_t* mi_intr_mask, uint32_t w)
{
    if (w & 0x1)   *mi_intr_mask &= ~MI_INTR_SP;
    if (w & 0x2)   *mi_intr_mask |= MI_INTR_SP;
    if (w & 0x4)   *mi_intr_mask &= ~MI_INTR_SI;
    if (w & 0x8)   *mi_intr_mask |= MI_INTR_SI;
    if (w & 0x10)  *mi_intr_mask &= ~MI_INTR_AI;
    if (w & 0x20)  *mi_intr_mask |= MI_INTR_AI;
    if (w & 0x40)  *mi_intr_mask &= ~MI_INTR_VI;
    if (w & 0x80)  *mi_intr_mask |= MI_INTR_VI;
    if (w & 0x100) *mi_intr_mask &= ~MI_INTR_PI;
    if (w & 0x200) *mi_intr_mask |= MI_INTR_PI;
    if (w & 0x400) *mi_intr_mask &= ~MI_INTR_DP;
    if (w & 0x800) *mi_intr_mask |= MI_INTR_DP;
}


void init_mi(struct mi_controller* mi, struct r4300_core* r4300)
{
    mi->r4300 = r4300;
}

void poweron_mi(struct mi_controller* mi)
{
    memset(mi->regs, 0, MI_REGS_COUNT*sizeof(uint32_t));
    mi->regs[MI_VERSION_REG] = 0x02020102;
}


void read_mi_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct mi_controller* mi = (struct mi_controller*)opaque;
    uint32_t reg = mi_reg(address);

    *value = mi->regs[reg];
}

void write_mi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct mi_controller* mi = (struct mi_controller*)opaque;
    uint32_t reg = mi_reg(address);

    int64_t* cp0_cycle_count = r4300_cp0_cycle_count(&mi->r4300->cp0);

    switch(reg)
    {
    case MI_INIT_MODE_REG:
        if (update_mi_init_mode(&mi->regs[MI_INIT_MODE_REG], value & mask) != 0)
        {
            clear_rcp_interrupt(mi, MI_INTR_DP);
        }
        break;
    case MI_INTR_MASK_REG:
        update_mi_intr_mask(&mi->regs[MI_INTR_MASK_REG], value & mask);

        r4300_check_interrupt(mi->r4300, CP0_CAUSE_IP2, mi->regs[MI_INTR_REG] & mi->regs[MI_INTR_MASK_REG]);
        cp0_update_count(mi->r4300);
        if (*cp0_cycle_count >= 0) gen_interrupt(mi->r4300);
        break;
    }
}

void raise_rcp_interrupt(struct mi_controller* mi, uint32_t mi_intr)
{
    mi->regs[MI_INTR_REG] |= mi_intr;

    if (mi->regs[MI_INTR_REG] & mi->regs[MI_INTR_MASK_REG])
        raise_maskable_interrupt(mi->r4300, CP0_CAUSE_IP2);
}

/* interrupt execution is scheduled (if not masked) */
void signal_rcp_interrupt(struct mi_controller* mi, uint32_t mi_intr)
{
    mi->regs[MI_INTR_REG] |= mi_intr;
    r4300_check_interrupt(mi->r4300, CP0_CAUSE_IP2, mi->regs[MI_INTR_REG] & mi->regs[MI_INTR_MASK_REG]);
}

void clear_rcp_interrupt(struct mi_controller* mi, uint32_t mi_intr)
{
    mi->regs[MI_INTR_REG] &= ~mi_intr;
    r4300_check_interrupt(mi->r4300, CP0_CAUSE_IP2, mi->regs[MI_INTR_REG] & mi->regs[MI_INTR_MASK_REG]);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pi_controller.c                                         *
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


#define PARALLAX_CORE_PROTOTYPES 1
#include <stdint.h>
#include <string.h>

#include "core/log.h"
#include "core/types.h"
#include "device/device.h"
#include "device/dd_controller.h"
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/rcp.h"
#include "device/rcp/rdp/rdp_core.h"
#include "device/rcp/rcp.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

int validate_pi_request(struct pi_controller* pi)
{
    if (pi->regs[PI_STATUS_REG] & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY)) {
        pi->regs[PI_STATUS_REG] |= PI_STATUS_ERROR;
        return 0;
    }

    return 1;
}

static unsigned int pi_dma_duration(const struct pi_controller* pi, uint32_t address, uint32_t length)
{
    unsigned domain = address >> 24;
    unsigned base = domain == 5 || (domain >= 8 && domain <= 15)
        ? PI_BSD_DOM2_LAT_REG : PI_BSD_DOM1_LAT_REG;
    uint64_t page_size = UINT64_C(1) << ((pi->regs[base+2] & 15) + 2);
    uint64_t first = address & ~UINT32_C(1), bytes = (length + 1u) & ~1u;
    uint64_t last = first + bytes - 2, pages = last/page_size - first/page_size + 1;
    uint64_t buffers = 0, partial = 0;
    if (pages == 1) {
        if (bytes == 128) buffers = 1;
        else partial = bytes;
    } else {
        if (!(first & (page_size-1))) ++buffers;
        else partial += page_size - (first & (page_size-1));
        if (!((last+2) & (page_size-1))) ++buffers;
        else partial += (last & (page_size-1)) + 2;
        if (pages > 2) buffers += (pages-2)*page_size/128;
    }
    uint64_t clocks = (15 + (pi->regs[base] & 255))*pages +
        ((pi->regs[base+1] & 255) + (pi->regs[base+3] & 3) + 2)*bytes/2 +
        buffers*28 + partial;
    return (unsigned int)((clocks*3 + 3)/4);
}

static void dma_pi_read(struct pi_controller* pi)
{
    if (!validate_pi_request(pi))
        return;

    uint32_t cart_addr = pi->regs[PI_CART_ADDR_REG] & ~UINT32_C(1);
    uint32_t dram_addr = pi->regs[PI_DRAM_ADDR_REG] & 0xfffffe;
    uint32_t length = (pi->regs[PI_RD_LEN_REG] & UINT32_C(0x00ffffff)) + 1;
    const uint8_t* dram = (uint8_t*)pi->ri->rdram->dram;

    const struct pi_dma_handler* handler = NULL;
    void* opaque = NULL;

    pi->get_pi_dma_handler(pi->cart, pi->dd, cart_addr, &opaque, &handler);

    if (handler == NULL) {
        core_log(CORE_LOG_WARNING, "Unknown PI DMA read: 0x%" PRIX32 " -> 0x%" PRIX32 " (0x%" PRIX32 ")", dram_addr, cart_addr, length);
        return;
    }

    pre_framebuffer_read(&pi->dp->fb, dram_addr);

    length = (length + 1) & ~1u;
    unsigned int cycles = pi_dma_duration(pi, cart_addr, length);
    uint32_t available = dram_addr < pi->ri->rdram->dram_size
        ? (uint32_t)pi->ri->rdram->dram_size - dram_addr : 0;
    if (available > length) available = length;
    if (available) handler->dma_read(opaque, dram, dram_addr, cart_addr, available);

    pi->regs[PI_STATUS_REG] |= PI_STATUS_DMA_BUSY;
    pi->regs[PI_DRAM_ADDR_REG] = (dram_addr + length + 7) & 0xfffff8;
    pi->regs[PI_CART_ADDR_REG] = cart_addr + length;
    cp0_update_count(pi->mi->r4300);
    add_interrupt_event(&pi->mi->r4300->cp0, PI_INT, cycles);
}

static void dma_pi_write(struct pi_controller* pi)
{
    if (!validate_pi_request(pi))
        return;

    uint32_t cart_addr = pi->regs[PI_CART_ADDR_REG] & ~UINT32_C(1);
    uint32_t dram_addr = pi->regs[PI_DRAM_ADDR_REG] & 0xfffffe;
    uint32_t length = (pi->regs[PI_WR_LEN_REG] & UINT32_C(0x00ffffff)) + 1;
    uint32_t bus_length = (length + 1) & ~1u;
    unsigned int cycles = pi_dma_duration(pi, cart_addr, length);
    uint8_t* dram = (uint8_t*)pi->ri->rdram->dram;

    const struct pi_dma_handler* handler = NULL;
    void* opaque = NULL;

    pi->get_pi_dma_handler(pi->cart, pi->dd, cart_addr, &opaque, &handler);

    if (handler == NULL) {
        core_log(CORE_LOG_WARNING, "Unknown PI DMA write: 0x%" PRIX32 " -> 0x%" PRIX32 " (0x%" PRIX32 ")", cart_addr, dram_addr, length);
        return;
    }

    if (length >= 0x7f && (length & 1))
        length += 1;
    if (length <= 0x80)
        length = length > (dram_addr & 7) ? length - (dram_addr & 7) : 0;
    uint32_t available = dram_addr < pi->ri->rdram->dram_size
        ? (uint32_t)pi->ri->rdram->dram_size - dram_addr : 0;
    if (available > length) available = length;
    if (available) handler->dma_write(opaque, dram, dram_addr, cart_addr, available);

    if (available) {
        invalidate_r4300_cached_code(pi->mi->r4300, dram_addr, available);
        post_framebuffer_write(&pi->dp->fb, dram_addr, available);
    }

    /* Mark DMA as busy */
    pi->regs[PI_STATUS_REG] |= PI_STATUS_DMA_BUSY;
    pi->regs[PI_DRAM_ADDR_REG] = (dram_addr + length + 7) & 0xfffff8;
    pi->regs[PI_CART_ADDR_REG] = cart_addr + bus_length;

   
    cp0_update_count(pi->mi->r4300);
    add_interrupt_event(&pi->mi->r4300->cp0, PI_INT, cycles);
}


void init_pi(struct pi_controller* pi,
             pi_dma_handler_getter get_pi_dma_handler,
             struct cart* cart,
             struct dd_controller* dd,
             struct mi_controller* mi,
             struct ri_controller* ri,
             struct rdp_core* dp)
{
    pi->get_pi_dma_handler = get_pi_dma_handler;
    pi->cart = cart;
    pi->dd = dd;
    pi->mi = mi;
    pi->ri = ri;
    pi->dp = dp;
}

void poweron_pi(struct pi_controller* pi)
{
    memset(pi->regs, 0, PI_REGS_COUNT*sizeof(uint32_t));
}

void read_pi_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct pi_controller* pi = (struct pi_controller*)opaque;
    uint32_t reg = pi_reg(address);

    *value = reg < PI_REGS_COUNT ? pi->regs[reg] : 0;

    if (reg == PI_WR_LEN_REG || reg == PI_RD_LEN_REG)
        *value = 0x7F;
    else if (reg == PI_CART_ADDR_REG)
        *value &= 0xFFFFFFFE;
    else if (reg == PI_DRAM_ADDR_REG)
        *value &= 0xFFFFFE;
}

void write_pi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct pi_controller* pi = (struct pi_controller*)opaque;
    uint32_t reg = pi_reg(address);

    if (reg >= PI_REGS_COUNT) return;
    if (reg != PI_STATUS_REG && !validate_pi_request(pi)) return;
    switch (reg)
    {
    case PI_CART_ADDR_REG:
        if (pi->dd != NULL) {
            masked_write(&pi->regs[PI_CART_ADDR_REG], value, mask);
            dd_on_pi_cart_addr_write(pi->dd, pi->regs[PI_CART_ADDR_REG]);
            return;
        }
        break;

    case PI_RD_LEN_REG:
        masked_write(&pi->regs[PI_RD_LEN_REG], value, mask);
        dma_pi_read(pi);
        return;

    case PI_WR_LEN_REG:
        masked_write(&pi->regs[PI_WR_LEN_REG], value, mask);
        dma_pi_write(pi);
        return;

    case PI_STATUS_REG:
        if (value & mask & PI_STATUS_CLR_INTR)
        {
            pi->regs[reg] &= ~PI_STATUS_INTERRUPT;
            clear_rcp_interrupt(pi->mi, MI_INTR_PI);
        }
        if (value & mask & PI_STATUS_RESET) {
            struct cp0* cp0 = &pi->mi->r4300->cp0;
            if (pi->regs[PI_STATUS_REG] & PI_STATUS_DMA_BUSY) {
                cp0_update_count(pi->mi->r4300);
                if (cp0->q.first && cp0->q.first->data.type == PI_INT)
                    remove_interrupt_event(cp0);
                else remove_event(&cp0->q, PI_INT);
            }
            pi->regs[PI_STATUS_REG] &= ~(PI_STATUS_DMA_BUSY | PI_STATUS_ERROR);
        }
        return;

    case PI_BSD_DOM1_LAT_REG:
    case PI_BSD_DOM1_PWD_REG:
    case PI_BSD_DOM2_LAT_REG:
    case PI_BSD_DOM2_PWD_REG:
        masked_write(&pi->regs[reg], value, mask);
        pi->regs[reg] &= 255;
        return;
    case PI_BSD_DOM1_PGS_REG:
    case PI_BSD_DOM2_PGS_REG:
        masked_write(&pi->regs[reg], value, mask);
        pi->regs[reg] &= 15;
        return;
    case PI_BSD_DOM1_RLS_REG:
    case PI_BSD_DOM2_RLS_REG:
        masked_write(&pi->regs[reg], value, mask);
        pi->regs[reg] &= 3;
        return;
    }

    masked_write(&pi->regs[reg], value, mask);
    if (reg == PI_DRAM_ADDR_REG) pi->regs[reg] &= 0xfffffe;
    if (reg == PI_CART_ADDR_REG) pi->regs[reg] &= ~1u;
}

void pi_end_of_dma_event(void* opaque)
{
    struct pi_controller* pi = (struct pi_controller*)opaque;
    if (!(pi->regs[PI_STATUS_REG] & PI_STATUS_DMA_BUSY)) {
        pi->regs[PI_STATUS_REG] &= ~PI_STATUS_IO_BUSY;
        return; /* CPU bus writes do not generate a DMA interrupt. */
    }
    pi->regs[PI_STATUS_REG] &= ~(PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY);
    pi->regs[PI_STATUS_REG] |= PI_STATUS_INTERRUPT;

    if (pi->dd != NULL) {
        if (((pi->regs[PI_CART_ADDR_REG] >= MM_DD_C2S_BUFFER) && (pi->regs[PI_CART_ADDR_REG] < MM_DD_DS_BUFFER)) ||
            ((pi->regs[PI_CART_ADDR_REG] >= MM_DD_DS_BUFFER) && (pi->regs[PI_CART_ADDR_REG] < MM_DD_REGS))) {
            dd_update_bm(pi->dd);
        }
    }

    raise_rcp_interrupt(pi->mi, MI_INTR_PI);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - ri_controller.c                                         *
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


#include <string.h>

#include "device/memory.h"

void init_ri(struct ri_controller* ri, struct rdram* rdram)
{
    ri->rdram = rdram;
}

void poweron_ri(struct ri_controller* ri)
{
    memset(ri->regs, 0, RI_REGS_COUNT*sizeof(uint32_t));
}


void read_ri_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct ri_controller* ri = (struct ri_controller*)opaque;
    uint32_t reg = ri_reg(address);

    *value = ri->regs[reg];
    if (reg == RI_CURRENT_LOAD_REG)
        *value = (ri->regs[RI_ERROR_REG] & 1) | 6 |
            (ri->regs[RI_MODE_REG] & 8) | (ri->regs[RI_SELECT_REG] & 16);
}

void write_ri_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct ri_controller* ri = (struct ri_controller*)opaque;
    uint32_t reg = ri_reg(address);

    if (reg == RI_ERROR_REG)
        ri->regs[reg] = 0;
    else if (reg == RI_WERROR_REG)
        ri->regs[reg] = 0xff; 
    else
        masked_write(&ri->regs[reg], value, mask);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - si_controller.c                                         *
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


#include <string.h>

#include "core/log.h"
#include "core/types.h"
#include "device/memory.h"
#include "device/pif.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/rcp.h"
#include "device/rcp/rcp.h"
#include "device/rdram.h"
#include "osal/preproc.h"

static int validate_dma(struct si_controller* si, uint32_t reg)
{
    if ((si->regs[reg] & 0x1fffffff) != 0x1fc007c0)
    {
        core_log(CORE_LOG_ERROR, "Unknown SI DMA PIF address: %08x", si->regs[reg]);
        return 0;
    }

    /* if DMA already busy, error, and ignore request */
    if (si->regs[SI_STATUS_REG] & SI_STATUS_DMA_BUSY) {
        si->regs[SI_STATUS_REG] |= SI_STATUS_DMA_ERROR;
        return 0;
    }

    return 1;
}

static void copy_pif_rdram(struct si_controller* si)
{
    size_t i;
    uint32_t dram_addr = si->regs[SI_DRAM_ADDR_REG] & UINT32_C(0xfffff8);

    uint32_t* pif_ram = (uint32_t*)si->pif->ram;
    struct rdram* ram = si->ri->rdram;

    for (i = 0; i < PIF_RAM_SIZE / 4; ++i) {
        uint32_t address = (dram_addr + (uint32_t)i*4) & 0xffffff;
        if (si->dma_dir == SI_DMA_WRITE)
            pif_ram[i] = address < ram->dram_size ? fromhl(ram->dram[address/4]) : 0;
        else if (address < ram->dram_size)
            ram->dram[address/4] = tohl(pif_ram[i]);
    }
    if (si->dma_dir == SI_DMA_READ)
        invalidate_r4300_cached_code(si->mi->r4300, dram_addr, PIF_RAM_SIZE);
}

static unsigned int si_duration(struct si_controller* si, int read)
{
    if (si->dma_duration) return si->dma_duration; 
    unsigned clocks = read ? 13600 : 4065;
    if (read) {
        unsigned offset = 0, channel = 0;
        const uint8_t* ram = si->pif->ram;
        while (offset < 64 && channel < 5) {
            unsigned send = ram[offset++];
            if (send == 0xfe) { clocks += 1420; break; }
            if (send == 0xff) { clocks += 1420; continue; }
            if (send == 0 || send == 0xfd) { clocks += 1420; ++channel; continue; }
            if (offset >= 64) break;
            unsigned receive = ram[offset++] & 63;
            offset += (send & 63) + receive;
            clocks += channel == 4 ? 20000 : si->pif->channels[channel].jbd ? 22000 : 18000;
            ++channel;
        }
    }
    return (clocks*3 + 3)/4;
}

static void dma_si_write(struct si_controller* si)
{
    if (!validate_dma(si, SI_PIF_ADDR_WR64B_REG))
        return;

    si->dma_dir = SI_DMA_WRITE;

    cp0_update_count(si->mi->r4300);
    si->regs[SI_STATUS_REG] |= SI_STATUS_DMA_BUSY;
    add_interrupt_event(&si->mi->r4300->cp0, SI_INT, si_duration(si, 0) + add_random_interrupt_time(si->mi->r4300));
}

static void dma_si_read(struct si_controller* si)
{
    if (!validate_dma(si, SI_PIF_ADDR_RD64B_REG))
        return;

    si->dma_dir = SI_DMA_READ;

    cp0_update_count(si->mi->r4300);
    si->regs[SI_STATUS_REG] |= SI_STATUS_DMA_BUSY;
    add_interrupt_event(&si->mi->r4300->cp0, SI_INT, si_duration(si, 1) + add_random_interrupt_time(si->mi->r4300));
}

void init_si(struct si_controller* si,
             unsigned int dma_duration,
             struct mi_controller* mi,
             struct pif* pif,
             struct ri_controller* ri)
{
    si->dma_duration = dma_duration;
    si->mi = mi;
    si->pif = pif;
    si->ri = ri;
}

void poweron_si(struct si_controller* si)
{
    memset(si->regs, 0, SI_REGS_COUNT*sizeof(uint32_t));
    si->dma_dir = SI_NO_DMA;
}


void read_si_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct si_controller* si = (struct si_controller*)opaque;
    uint32_t reg = si_reg(address);

    *value = reg < SI_REGS_COUNT ? si->regs[reg] : 0;
}

void write_si_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct si_controller* si = (struct si_controller*)opaque;
    uint32_t reg = si_reg(address);

    switch (reg)
    {
    case SI_DRAM_ADDR_REG:
        masked_write(&si->regs[SI_DRAM_ADDR_REG], value, mask);
        si->regs[SI_DRAM_ADDR_REG] &= 0xfffff8;
        break;

    case SI_PIF_ADDR_RD64B_REG:
        masked_write(&si->regs[SI_PIF_ADDR_RD64B_REG], value, mask);
        dma_si_read(si);
        break;

    case SI_PIF_ADDR_WR64B_REG:
        masked_write(&si->regs[SI_PIF_ADDR_WR64B_REG], value, mask);
        dma_si_write(si);
        break;

    case SI_STATUS_REG:
        /* clear si interrupt */
        si->regs[SI_STATUS_REG] &= ~SI_STATUS_INTERRUPT;
        clear_rcp_interrupt(si->mi, MI_INTR_SI);
        break;
    }
}

void si_end_of_dma_event(void* opaque)
{
    struct si_controller* si = (struct si_controller*)opaque;

    /* DRAM -> PIF : start the PIF processing */
    if (si->dma_dir == SI_DMA_WRITE) {
        if (!(si->regs[SI_STATUS_REG] & SI_STATUS_IO_BUSY))
            copy_pif_rdram(si);
        process_pif_ram(si->pif);
    }
    /* PIF -> DRAM : copy to RDRAM */
    else if (si->dma_dir == SI_DMA_READ) {
        update_pif_ram(si->pif);
        copy_pif_rdram(si);
    }

    /* end DMA */
    si->dma_dir = SI_NO_DMA;
    si->regs[SI_STATUS_REG] &= ~(SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY);

    /* raise si interrupt */
    si->regs[SI_STATUS_REG] |= SI_STATUS_INTERRUPT;
    raise_rcp_interrupt(si->mi, MI_INTR_SI);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - vi_controller.c                                         *
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


#include <string.h>

#include "core/types.h"
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/rcp.h"
#include "main/main.h"
#include "core/backend.h"
#include <core/frontend.h>

unsigned int vi_clock_from_tv_standard(core_system_type tv_standard)
{
    switch(tv_standard)
    {
    case SYSTEM_PAL:
        return 49656530;
    case SYSTEM_MPAL:
        return 48628316;
    case SYSTEM_NTSC:
    default:
        return 48681812;
    }
}

unsigned int vi_expected_refresh_rate_from_tv_standard(core_system_type tv_standard)
{
    switch (tv_standard)
    {
    case SYSTEM_PAL:
        return 50;
    case SYSTEM_NTSC:
    case SYSTEM_MPAL:
    default:
        return 60;
    }
}

static unsigned int vi_v_total(const struct vi_controller* vi)
{
    return (vi->regs[VI_V_SYNC_REG] & UINT32_C(0x3ff)) + 1u;
}

/* VI periods are video-clock cycles; interrupt deadlines are COUNT ticks.
 * COUNT is the fixed 93.75 MHz CPU clock divided by two. */
static uint64_t vi_period_numerator(const struct vi_controller* vi)
{
    if (vi->regs[VI_H_SYNC_REG] && vi->regs[VI_V_SYNC_REG] && vi->clock)
        return UINT64_C(46875000) * vi_v_total(vi) *
               ((vi->regs[VI_H_SYNC_REG] & 0xfff) + 1u);
    return UINT64_C(46875000);
}

static uint32_t vi_period_denominator(const struct vi_controller* vi)
{
    if (vi->regs[VI_H_SYNC_REG] && vi->regs[VI_V_SYNC_REG] && vi->clock)
        return vi->clock * 2u;
    return vi->expected_refresh_rate ? vi->expected_refresh_rate : 60u;
}

static unsigned int vi_next_field_ticks(struct vi_controller* vi)
{
    uint64_t ticks;

    if (CountPerScanlineOverride)
    {
        ticks = (uint64_t)CountPerScanlineOverride * vi_v_total(vi);
        vi->field_tick_phase = 0;
    }
    else
    {
        const uint32_t denominator = vi_period_denominator(vi);
        const uint64_t scaled = vi_period_numerator(vi) + vi->field_tick_phase;
        ticks = scaled / denominator;
        vi->field_tick_phase = scaled % denominator;
    }

    if (ticks == 0)
        ticks = 1;
    if (ticks > UINT32_MAX)
        ticks = UINT32_MAX;

    vi->field_ticks = (uint32_t)ticks;
    return (unsigned int)ticks;
}

double vi_actual_refresh_rate(const struct vi_controller* vi)
{
    uint64_t ticks;

    if (vi == NULL || vi->clock == 0)
        return 0.0;

    if (!CountPerScanlineOverride)
        return 46875000.0 * vi_period_denominator(vi) / vi_period_numerator(vi);

    ticks = (uint64_t)CountPerScanlineOverride * vi_v_total(vi);
    return ticks != 0 ? 46875000.0 / (double)ticks : 0.0;
}

unsigned int vi_legacy_savestate_delay(const struct vi_controller* vi)
{
    uint64_t ticks;

    if (vi == NULL)
        return 0;

    if (CountPerScanlineOverride)
        ticks = (uint64_t)CountPerScanlineOverride * vi_v_total(vi);
    else
    {
        const uint32_t denominator = vi_period_denominator(vi);
        ticks = (vi_period_numerator(vi) + denominator / 2u) / denominator;
    }

    if (ticks > UINT32_MAX)
        ticks = UINT32_MAX;
    return (unsigned int)ticks;
}

unsigned int vi_legacy_savestate_count_per_scanline(const struct vi_controller* vi)
{
    const unsigned int vtotal = vi_v_total(vi);
    uint64_t delay;

    if (CountPerScanlineOverride)
        return CountPerScanlineOverride;
    if (vtotal == 0)
        return 0;

    delay = vi_legacy_savestate_delay(vi);
    return (unsigned int)((delay + vtotal / 2u) / vtotal);
}

void vi_rebase_timing(struct vi_controller* vi, unsigned int legacy_delay)
{
    struct cp0* cp0;
    struct node* e;
    int64_t remaining;
    int64_t elapsed;

    if (vi == NULL || vi->mi == NULL || vi->mi->r4300 == NULL)
        return;

    cp0 = &vi->mi->r4300->cp0;
    vi->field_tick_phase = 0;
    vi->field_ticks = legacy_delay ? legacy_delay : vi_legacy_savestate_delay(vi);

    cp0_update_count(vi->mi->r4300);
    vi->field_start_count_clock = cp0->count_clock;

    /* Old savestates serialized delay but not fractional phase. Keep the
     * queued VI event exactly where it was and reconstruct only the current
     * field position. */
    for (e = cp0->q.first; e != NULL; e = e->next)
    {
        if (e->data.type != VI_INT)
            continue;

        remaining = e->data.deadline - cp0->count_clock;
        elapsed = (int64_t)vi->field_ticks - remaining;
        if (elapsed > 0 && elapsed < (int64_t)vi->field_ticks)
            vi->field_start_count_clock -= elapsed;
        break;
    }
}

void vi_schedule_vertical_interrupt(struct vi_controller* vi)
{
    struct cp0* cp0 = &vi->mi->r4300->cp0;
    unsigned int ticks;

    cp0_update_count(vi->mi->r4300);
    vi->field_start_count_clock = cp0->count_clock;
    ticks = vi_next_field_ticks(vi);
    add_interrupt_event(cp0, VI_INT, ticks);
}

void set_vi_vertical_interrupt(struct vi_controller* vi)
{
    if (!get_event(&vi->mi->r4300->cp0.q, VI_INT)
        && (vi->regs[VI_V_INTR_REG] < vi->regs[VI_V_SYNC_REG]))
    {
        vi_schedule_vertical_interrupt(vi);
    }
}

void init_vi(struct vi_controller* vi, unsigned int clock, unsigned int expected_refresh_rate,
             struct mi_controller* mi, struct rdp_core* dp)
{
    vi->clock = clock;
    vi->expected_refresh_rate = expected_refresh_rate;
    vi->mi = mi;
    vi->dp = dp;
}

void poweron_vi(struct vi_controller* vi)
{
    memset(vi->regs, 0, VI_REGS_COUNT*sizeof(uint32_t));
    vi->field = 0;
    vi->field_tick_phase = 0;
    vi->field_start_count_clock = 0;
    vi->field_ticks = 0;
}

void read_vi_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct vi_controller* vi = (struct vi_controller*)opaque;
    uint32_t reg = vi_reg(address);

    if (reg == VI_CURRENT_REG)
    {
        struct cp0* cp0 = &vi->mi->r4300->cp0;
        uint64_t elapsed;
        uint64_t halfline;
        const unsigned int vtotal = vi_v_total(vi);

        cp0_update_count(vi->mi->r4300);
        elapsed = cp0->count_clock > vi->field_start_count_clock
            ? (uint64_t)(cp0->count_clock - vi->field_start_count_clock) : 0;

        /* Do not reconstruct the scanline through a separately rounded
         * count_per_scanline. The exact scheduled field interval is the
         * denominator, so VI_CURRENT follows the same phase as the event. */
        halfline = (vi->field_ticks != 0)
            ? (elapsed * vtotal) / vi->field_ticks : 0;

        if (vtotal != 0)
            halfline %= vtotal;
        vi->regs[VI_CURRENT_REG] = ((uint32_t)halfline & ~UINT32_C(1)) | vi->field;
    }

    *value = vi->regs[reg];
}

void write_vi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct vi_controller* vi = (struct vi_controller*)opaque;
    uint32_t reg = vi_reg(address);

    switch(reg)
    {
    case VI_STATUS_REG:
        if ((vi->regs[VI_STATUS_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_STATUS_REG], value, mask);
            renderer_vi_status_changed();
        }
        return;

    case VI_WIDTH_REG:
        if ((vi->regs[VI_WIDTH_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_WIDTH_REG], value, mask);
            renderer_vi_width_changed();
        }
        return;

    case VI_CURRENT_REG:
        clear_rcp_interrupt(vi->mi, MI_INTR_VI);
        return;

    case VI_V_SYNC_REG:
    case VI_H_SYNC_REG:
        if ((vi->regs[reg] & mask) != (value & mask))
        {
            masked_write(&vi->regs[reg], value, mask);
            /* Keep the queued boundary; subsequent fields use the new period. */
            vi->field_tick_phase = 0;
            set_vi_vertical_interrupt(vi);
        }
        return;

    case VI_V_INTR_REG:
        masked_write(&vi->regs[VI_V_INTR_REG], value, mask);
        set_vi_vertical_interrupt(vi);
        return;
    }

    masked_write(&vi->regs[reg], value, mask);
}

void vi_vertical_interrupt_event(void* opaque)
{
    struct vi_controller* vi = (struct vi_controller*)opaque;
    struct cp0* cp0 = &vi->mi->r4300->cp0;

    if (vi->dp->do_on_unfreeze & DELAY_DP_INT)
        vi->dp->do_on_unfreeze |= DELAY_UPDATESCREEN;
    else
        renderer_present();

    /* allow main module to do things on VI event */
    new_vi();

    /* toggle vi field if in interlaced mode */
    vi->field ^= (vi->regs[VI_STATUS_REG] >> 6) & 0x1;

    /* The oscillator does not stop while the CPU finishes an instruction.
     * Anchor to the scheduled field boundary, not its eventual service time,
     * or every late VI permanently moves video behind the audio DAC. */
    int64_t boundary = cp0->q.first->data.deadline;
    remove_interrupt_event(cp0);
    cp0_update_count(vi->mi->r4300);
    vi->field_start_count_clock = boundary;
    uint32_t ticks = vi_next_field_ticks(vi);
    add_interrupt_event_count(cp0, VI_INT, cp0->regs[CP0_COUNT_REG] +
        (uint32_t)(boundary + ticks - cp0->count_clock));

    /* trigger interrupt */
    raise_rcp_interrupt(vi->mi, MI_INTR_VI);
}
