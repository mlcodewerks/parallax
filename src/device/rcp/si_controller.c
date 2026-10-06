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

#include "si_controller.h"

#include <string.h>

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "device/memory.h"
#include "device/pif/pif.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/mi_controller.h"
#include "device/rcp/ri_controller.h"
#include "device/rdram.h"
#include "osal/preproc.h"

static int validate_dma(struct si_controller* si, uint32_t reg)
{
    if ((si->regs[reg] & 0x1fffffff) != 0x1fc007c0)
    {
        DebugMessage(M64MSG_ERROR, "Unknown SI DMA PIF address: %08x", si->regs[reg]);
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

