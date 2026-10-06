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

#include "ai_controller.h"

#include <string.h>

#include "backends/api/audio_out_backend.h"
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/mi_controller.h"
#include "device/rcp/ri_controller.h"
#include "device/rcp/rdp/vi_controller.h"
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
    if (frames && ai->iaout->push_idle_samples)
        ai->iaout->push_idle_samples(ai->aout, frames);
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

