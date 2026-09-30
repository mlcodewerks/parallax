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

#include "vi_controller.h"

#include <string.h>

#include "api/m64p_types.h"
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/mi_controller.h"
#include "main/main.h"
#include "plugin/plugin.h"
#include <mupen64plus-next_common.h>

unsigned int vi_clock_from_tv_standard(m64p_system_type tv_standard)
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

unsigned int vi_expected_refresh_rate_from_tv_standard(m64p_system_type tv_standard)
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
        const unsigned int refresh = vi->expected_refresh_rate
            ? vi->expected_refresh_rate : 60u;
        const uint64_t scaled = (uint64_t)vi->clock + vi->field_tick_phase;
        ticks = scaled / refresh;
        vi->field_tick_phase = scaled % refresh;
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
        return (double)(vi->expected_refresh_rate ? vi->expected_refresh_rate : 60u);

    ticks = (uint64_t)CountPerScanlineOverride * vi_v_total(vi);
    return ticks != 0 ? (double)vi->clock / (double)ticks : 0.0;
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
        const unsigned int refresh = vi->expected_refresh_rate
            ? vi->expected_refresh_rate : 60u;
        ticks = ((uint64_t)vi->clock + refresh / 2u) / refresh;
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
            gfx.viStatusChanged();
        }
        return;

    case VI_WIDTH_REG:
        if ((vi->regs[VI_WIDTH_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_WIDTH_REG], value, mask);
            gfx.viWidthChanged();
        }
        return;

    case VI_CURRENT_REG:
        clear_rcp_interrupt(vi->mi, MI_INTR_VI);
        return;

    case VI_V_SYNC_REG:
        if ((vi->regs[VI_V_SYNC_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_V_SYNC_REG], value, mask);
            /* Preserve the current queued VI. The next field uses the new
             * vertical total only for an explicit CountPerScanlineOverride;
             * normal timing remains clock / nominal refresh. */
            if (CountPerScanlineOverride)
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
        gfx.updateScreen();

    /* allow main module to do things on VI event */
    new_vi();

    /* toggle vi field if in interlaced mode */
    vi->field ^= (vi->regs[VI_STATUS_REG] >> 6) & 0x1;

    /* Racer-compatible hybrid scheduling: fractional field periods are
     * accumulated, but every new field starts from the COUNT value at which
     * this VI is actually serviced. Late service therefore never shortens the
     * next software-visible field. */
    remove_interrupt_event(cp0);
    cp0_update_count(vi->mi->r4300);
    vi->field_start_count_clock = cp0->count_clock;
    add_interrupt_event(cp0, VI_INT, vi_next_field_ticks(vi));

    /* trigger interrupt */
    raise_rcp_interrupt(vi->mi, MI_INTR_VI);
}
