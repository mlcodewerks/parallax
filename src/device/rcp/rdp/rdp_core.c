/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rdp_core.c                                              *
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

#include "rdp_core.h"
#include "vi_controller.h"

#include <string.h>

#include "device/memory.h"
#include "device/rdram.h"
#include "device/r4300/cp0.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/mi_controller.h"
#include "device/rcp/rsp/rsp_core.h"
#include "plugin/plugin.h"
#ifdef __LIBRETRO__
#include "renderer_options.h"
#include "angrylion/n64video.h"
#endif

static const uint32_t *hle_words;
static uint32_t hle_base, hle_bytes;
const uint32_t *rdp_hle_command_buffer(uint32_t address)
{
    return hle_words && address >= hle_base && address - hle_base < hle_bytes
        ? hle_words + ((address - hle_base) >> 2) : NULL;
}
#ifdef __LIBRETRO__
void rdp_submit_hle(struct rdp_core *dp, const uint32_t *words, uint32_t base, uint32_t bytes)
{
    if (!bytes || (dp->dpc_regs[DPC_STATUS_REG] & DPC_STATUS_FREEZE)) return;
    hle_words = words; hle_base = base; hle_bytes = bytes;
    n64video_set_hle_cmd_buffer(words, base, bytes);
    /* Generated commands use a host FIFO, leaving guest RDRAM intact. */
    write_rsp_dpc_regs(dp, DPC_STATUS_REG * 4, DPC_CLR_XBUS_DMEM_DMA, ~0u);
    write_rsp_dpc_regs(dp, DPC_START_REG * 4, base, ~0u);
    write_rsp_dpc_regs(dp, DPC_END_REG * 4, base + bytes, ~0u);
    n64video_set_hle_cmd_buffer(NULL, 0, 0);
    hle_words = NULL; hle_bytes = 0;
}
#endif
#if defined(HAVE_PARALLEL_RSP)
#include "device/rcp/rsp/p_rsp/dpc_bridge.h"
#endif


uint32_t rdp_dpc_clock_value(const struct rdp_core* dp, uint32_t count)
{
    uint32_t elapsed = count - dp->clock_base;
    uint32_t advance = (uint32_t)(((uint64_t)elapsed * 4u) / 3u);

    return (dp->clock_offset + advance) & UINT32_C(0x00ffffff);
}

void rdp_restore_dpc_clock(struct rdp_core* dp, uint32_t count)
{
    dp->clock_base = count;
    dp->clock_offset = dp->dpc_regs[DPC_CLOCK_REG] & UINT32_C(0x00ffffff);
}

static uint32_t dpc_clock_now(struct rdp_core* dp, int from_rsp)
{
    const uint32_t* cp0_regs;

    cp0_update_count(dp->mi->r4300);
    cp0_regs = r4300_cp0_regs(&dp->mi->r4300->cp0);
    uint32_t clock = rdp_dpc_clock_value(dp, cp0_regs[CP0_COUNT_REG]);
#if defined(HAVE_PARALLEL_RSP)
    if (from_rsp) clock += parallelRSPExecutedCycles();
#else
    (void)from_rsp;
#endif
    return clock & UINT32_C(0x00ffffff);
}

uint32_t rdp_sync_full_delay(const struct rdp_core* dp)
{

    if (!dp->vi_regs) return 4000;
    const uint32_t* vi = dp->vi_regs;
    uint32_t hs = (vi[VI_H_START_REG] >> 16) & 1023, he = vi[VI_H_START_REG] & 1023;
    uint32_t vs = (vi[VI_V_START_REG] >> 16) & 1023, ve = vi[VI_V_START_REG] & 1023;
    uint32_t w = (hs > he ? hs - he : he - hs);
    uint32_t h = (vs > ve ? vs - ve : ve - vs);
    if (!w) w = vi[VI_WIDTH_REG];
    w = (vi[VI_X_SCALE_REG] & 4095) ? w * (vi[VI_X_SCALE_REG] & 4095) / 1024 : 320;
    h = (vi[VI_Y_SCALE_REG] & 4095) ? h * (vi[VI_Y_SCALE_REG] & 4095) / 2048 : 240;
    uint32_t ticks = w * h * 2;
    return ticks > 4000 ? ticks : 4000;
}

static void process_dpc_commands(struct rdp_core* dp, int from_rsp)
{
    uint32_t dp_pending;
    if (dp->dpc_regs[DPC_STATUS_REG] & DPC_STATUS_FREEZE)
        return;
    if (dp->dpc_regs[DPC_CURRENT_REG] == dp->dpc_regs[DPC_END_REG])
        return;

    dp_pending = dp->mi->regs[MI_INTR_REG] & MI_INTR_DP;
    if (dp->vi_regs && dp->instruction_timing) {
        int64_t submitted = dp->mi->r4300->cp0.count_clock;
#if defined(HAVE_PARALLEL_RSP)
        if (from_rsp) submitted += ((uint64_t)parallelRSPExecutedCycles() * 3 + 3) / 4;
#endif
        uint32_t begin = dp->dpc_regs[DPC_CURRENT_REG] & 0xfffff8;
        uint32_t end = dp->dpc_regs[DPC_END_REG] & 0xfffff8;
        for (uint32_t at = begin; at < end; at += 4) {
            const uint32_t *synthetic = rdp_hle_command_buffer(at);
            uint32_t word = synthetic ? *synthetic : (dp->dpc_regs[DPC_STATUS_REG] & DPC_STATUS_XBUS_DMEM_DMA)
                ? dp->sp->mem[(at & 4095) >> 2]
                : (at < 0x800000 ? dp->fb.rdram->dram[at >> 2] : 0);
            rdp_timing_word(&dp->timing, word, submitted);
        }
    }
    unprotect_framebuffers(&dp->fb);
    gfx.processRDPList();
    protect_framebuffers(&dp->fb);
    dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_CBUF_READY;


    if (!dp_pending && (dp->mi->regs[MI_INTR_REG] & MI_INTR_DP))
    {
        if (from_rsp || dp->vi_regs)
        {
     
            dp->mi->regs[MI_INTR_REG] &= ~MI_INTR_DP;
            if (!get_event(&dp->mi->r4300->cp0.q, DP_INT))
            {
                cp0_update_count(dp->mi->r4300);
                uint32_t delay = rdp_sync_full_delay(dp);
#if defined(HAVE_PARALLEL_RSP)
                if (from_rsp)
                    delay += (uint32_t)(((uint64_t)parallelRSPExecutedCycles() * 3 + 3) / 4);
#endif
                int64_t work = dp->instruction_timing ? dp->timing.deadline - dp->mi->r4300->cp0.count_clock : 0;
                if (work > delay) delay = work > UINT32_MAX ? UINT32_MAX : (uint32_t)work;
                add_interrupt_event(&dp->mi->r4300->cp0, DP_INT, delay);
            }
        }
        else
            signal_rcp_interrupt(dp->mi, MI_INTR_DP);
    }
}

static void update_dpc_status(struct rdp_core* dp, uint32_t w, int from_rsp)
{
    /* clear / set xbus_dmem_dma */
    if (w & DPC_CLR_XBUS_DMEM_DMA) dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_XBUS_DMEM_DMA;
    if (w & DPC_SET_XBUS_DMEM_DMA) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_XBUS_DMEM_DMA;

    /* clear / set freeze */
    if (w & DPC_CLR_FREEZE)
    {
        dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_FREEZE;

        if (dp->do_on_unfreeze & DELAY_DP_INT)
            signal_rcp_interrupt(dp->mi, MI_INTR_DP);
        if (dp->do_on_unfreeze & DELAY_UPDATESCREEN)
            gfx.updateScreen();
        dp->do_on_unfreeze = 0;
        process_dpc_commands(dp, from_rsp);
    }
    if (w & DPC_SET_FREEZE) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_FREEZE;

    /* clear / set flush */
    if (w & DPC_CLR_FLUSH) dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_FLUSH;
    if (w & DPC_SET_FLUSH) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_FLUSH;

    if (w & DPC_CLR_TMEM_CTR) dp->dpc_regs[DPC_TMEM_REG] = 0;
    if (w & DPC_CLR_PIPE_CTR) dp->dpc_regs[DPC_PIPEBUSY_REG] = 0;
    if (w & DPC_CLR_CMD_CTR) dp->dpc_regs[DPC_BUFBUSY_REG] = 0;

    /* clear clock counter */
    if (w & DPC_CLR_CLOCK_CTR)
    {
        const uint32_t* cp0_regs;

        cp0_update_count(dp->mi->r4300);
        cp0_regs = r4300_cp0_regs(&dp->mi->r4300->cp0);
        dp->clock_base = cp0_regs[CP0_COUNT_REG];
        dp->clock_offset = 0;
#if defined(HAVE_PARALLEL_RSP)
        if (from_rsp) dp->clock_offset = -parallelRSPExecutedCycles();
#endif
        dp->dpc_regs[DPC_CLOCK_REG] = 0;
    }
}


void init_rdp(struct rdp_core* dp,
              struct rsp_core* sp,
              struct mi_controller* mi,
              struct memory* mem,
              struct rdram* rdram,
              struct r4300_core* r4300)
{
    dp->sp = sp;
    dp->mi = mi;
    dp->vi_regs = NULL;
    dp->instruction_timing = 1;
#ifdef __LIBRETRO__
    dp->instruction_timing = renderer_settings.rdp_timing;
#endif

    init_fb(&dp->fb, mem, rdram, r4300);
}

void poweron_rdp(struct rdp_core* dp)
{
    memset(dp->dpc_regs, 0, DPC_REGS_COUNT*sizeof(uint32_t));
    memset(dp->dps_regs, 0, DPS_REGS_COUNT*sizeof(uint32_t));
    dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_START_GCLK;

    dp->do_on_unfreeze = 0;
    dp->clock_base = 0;
    dp->clock_offset = 0;
    memset(&dp->timing, 0, sizeof(dp->timing));
    dp->timing.right = dp->timing.bottom = 4095;

    poweron_fb(&dp->fb);
}


void read_dpc_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dpc_reg(address);

    if (reg == DPC_CLOCK_REG)
        dp->dpc_regs[DPC_CLOCK_REG] = dpc_clock_now(dp, 0);

    *value = dp->dpc_regs[reg];
}

void read_rsp_dpc_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    if (dpc_reg(address) == DPC_CLOCK_REG)
        dp->dpc_regs[DPC_CLOCK_REG] = dpc_clock_now(dp, 1);
    *value = dp->dpc_regs[dpc_reg(address)];
}

static void write_dpc_regs_from(void* opaque, uint32_t address, uint32_t value, uint32_t mask, int from_rsp)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dpc_reg(address);

    switch(reg)
    {
    case DPC_STATUS_REG:
        update_dpc_status(dp, value & mask, from_rsp);
    case DPC_CURRENT_REG:
    case DPC_CLOCK_REG:
    case DPC_BUFBUSY_REG:
    case DPC_PIPEBUSY_REG:
    case DPC_TMEM_REG:
        return;
    }

    switch(reg)
    {
    case DPC_START_REG:
        if (!(dp->dpc_regs[DPC_STATUS_REG] & DPC_STATUS_START_VALID))
        {
            masked_write(&dp->dpc_regs[reg], value, mask);
            dp->dpc_regs[reg] &= 0x00fffff8;
        }
        dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_START_VALID;
        break;
    case DPC_END_REG:
        masked_write(&dp->dpc_regs[reg], value, mask);
        dp->dpc_regs[reg] &= 0x00fffff8;
        if (dp->dpc_regs[DPC_STATUS_REG] & DPC_STATUS_START_VALID)
        {
            dp->dpc_regs[DPC_CURRENT_REG] = dp->dpc_regs[DPC_START_REG];
            dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_START_VALID;
        }
        process_dpc_commands(dp, from_rsp);
        break;
    }
}

void write_dpc_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    write_dpc_regs_from(opaque, address, value, mask, 0);
}

void write_rsp_dpc_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    write_dpc_regs_from(opaque, address, value, mask, 1);
}

void read_dps_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dps_reg(address);

    *value = dp->dps_regs[reg];
}

void write_dps_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dps_reg(address);

    masked_write(&dp->dps_regs[reg], value, mask);
}

void rdp_interrupt_event(void* opaque)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;

    raise_rcp_interrupt(dp->mi, MI_INTR_DP);
}

