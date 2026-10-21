#ifndef PARALLAX_RCP_H
#define PARALLAX_RCP_H

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - ai_controller.h                                         *
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


#include <stddef.h>
#include <stdint.h>

#include "osal/preproc.h"

struct mi_controller;
struct ri_controller;
struct vi_controller;
struct audio_out_backend_interface;

enum ai_registers
{
    AI_DRAM_ADDR_REG,
    AI_LEN_REG,
    AI_CONTROL_REG,
    AI_STATUS_REG,
    AI_DACRATE_REG,
    AI_BITRATE_REG,
    AI_REGS_COUNT
};

struct ai_dma
{
    uint32_t address;
    uint32_t length;
    unsigned int duration;
};

enum { AI_DMA_FIFO_SIZE = 2 };

struct ai_controller
{
    uint32_t regs[AI_REGS_COUNT];
    struct ai_dma fifo[AI_DMA_FIFO_SIZE];
    unsigned int samples_format_changed;
    uint32_t last_read;
    uint32_t delayed_carry;
    /* Monotonic video-clock time; CP0 COUNT writes must not move the DAC. */
    int64_t dma_start_clock;
    int64_t idle_clock;
    uint32_t idle_phase;

    struct mi_controller* mi;
    struct ri_controller* ri;
    struct vi_controller* vi;

    void* aout;
    const struct audio_out_backend_interface* iaout;
};

static osal_inline uint32_t ai_reg(uint32_t address)
{
    return (address & 0xffff) >> 2;
}

void init_ai(struct ai_controller* ai,
             struct mi_controller* mi,
             struct ri_controller* ri,
             struct vi_controller* vi,
             void* aout,
             const struct audio_out_backend_interface* iaout);

void poweron_ai(struct ai_controller* ai);

void read_ai_regs(void* opaque, uint32_t address, uint32_t* value);
void write_ai_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void ai_end_of_dma_event(void* opaque);
void ai_flush_samples(struct ai_controller* ai);
void ai_rebase_timing(struct ai_controller* ai);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mi_controller.h                                         *
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


#include <stdint.h>

#include "osal/preproc.h"

struct r4300_core;

enum mi_registers
{
    MI_INIT_MODE_REG,
    MI_VERSION_REG,
    MI_INTR_REG,
    MI_INTR_MASK_REG,
    MI_REGS_COUNT
};


enum mi_intr
{
    MI_INTR_SP = 0x01,
    MI_INTR_SI = 0x02,
    MI_INTR_AI = 0x04,
    MI_INTR_VI = 0x08,
    MI_INTR_PI = 0x10,
    MI_INTR_DP = 0x20
};

struct mi_controller
{
    uint32_t regs[MI_REGS_COUNT];

    struct r4300_core* r4300;
};

static osal_inline uint32_t mi_reg(uint32_t address)
{
    return (address & 0xffff) >> 2;
}

void init_mi(struct mi_controller* mi, struct r4300_core* r4300);
void poweron_mi(struct mi_controller* mi);

void read_mi_regs(void* opaque, uint32_t address, uint32_t* value);
void write_mi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void raise_rcp_interrupt(struct mi_controller* mi, uint32_t mi_intr);
void signal_rcp_interrupt(struct mi_controller* mi, uint32_t mi_intr);
void clear_rcp_interrupt(struct mi_controller* mi, uint32_t mi_intr);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pi_controller.h                                         *
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


#include <stddef.h>
#include <stdint.h>

#include "osal/preproc.h"

struct cart;
struct dd_controller;
struct mi_controller;
struct ri_controller;
struct rdp_core;

enum pi_registers
{
    PI_DRAM_ADDR_REG,
    PI_CART_ADDR_REG,
    PI_RD_LEN_REG,
    PI_WR_LEN_REG,
    PI_STATUS_REG,
    PI_BSD_DOM1_LAT_REG,
    PI_BSD_DOM1_PWD_REG,
    PI_BSD_DOM1_PGS_REG,
    PI_BSD_DOM1_RLS_REG,
    PI_BSD_DOM2_LAT_REG,
    PI_BSD_DOM2_PWD_REG,
    PI_BSD_DOM2_PGS_REG,
    PI_BSD_DOM2_RLS_REG,
    PI_REGS_COUNT
};

enum
{
    /* PI_STATUS - read */
    PI_STATUS_DMA_BUSY  = 0x01,
    PI_STATUS_IO_BUSY   = 0x02,
    PI_STATUS_ERROR     = 0x04,
    PI_STATUS_INTERRUPT = 0x08,

    /* PI_STATUS - write */
    PI_STATUS_RESET     = 0x01,
    PI_STATUS_CLR_INTR  = 0x02
};

struct pi_dma_handler
{
    unsigned int (*dma_read)(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
    unsigned int (*dma_write)(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
};

typedef void (*pi_dma_handler_getter)(struct cart* cart, struct dd_controller* dd, uint32_t address, void** opaque, const struct pi_dma_handler** handler);

struct pi_controller
{
    uint32_t regs[PI_REGS_COUNT];

    pi_dma_handler_getter get_pi_dma_handler;

    struct cart* cart;
    struct dd_controller* dd;
    struct mi_controller* mi;
    struct ri_controller* ri;
    struct rdp_core* dp;
};

static osal_inline uint32_t pi_reg(uint32_t address)
{
    return (address & 0x3f) >> 2;
}



void init_pi(struct pi_controller* pi,
             pi_dma_handler_getter get_pi_dma_handler,
             struct cart* cart,
             struct dd_controller* dd,
             struct mi_controller* mi,
             struct ri_controller* ri,
             struct rdp_core* dp);

void poweron_pi(struct pi_controller* pi);

void read_pi_regs(void* opaque, uint32_t address, uint32_t* value);
void write_pi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void pi_end_of_dma_event(void* opaque);
int validate_pi_request(struct pi_controller* pi);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - ri_controller.h                                         *
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


#include <stddef.h>
#include <stdint.h>

#include "osal/preproc.h"

struct rdram;

enum ri_registers
{
    RI_MODE_REG,
    RI_CONFIG_REG,
    RI_CURRENT_LOAD_REG,
    RI_SELECT_REG,
    RI_REFRESH_REG,
    RI_LATENCY_REG,
    RI_ERROR_REG,
    RI_WERROR_REG,
    RI_REGS_COUNT
};

struct ri_controller
{
    uint32_t regs[RI_REGS_COUNT];

    struct rdram* rdram;
};

static osal_inline uint32_t ri_reg(uint32_t address)
{
    return (address & 0x1f) >> 2;
}

static osal_inline uint16_t ri_address_to_id_field(uint32_t address)
{
    /* XXX: pure guessing, need harware test */
    return (uint16_t)(((address >> 20) == 0x03f)
        ? (address & 0x0007fc00) >> 10   /* RDRAM registers id_field: [19..10] */
        : (address & 0x00f00000) >> 20); /* RDRAM memory    id_field: [23..20] */
}



void init_ri(struct ri_controller* ri, struct rdram* rdram);

void poweron_ri(struct ri_controller* ri);

void read_ri_regs(void* opaque, uint32_t address, uint32_t* value);
void write_ri_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - si_controller.h                                         *
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


#include <stdint.h>

#include "osal/preproc.h"

struct mi_controller;
struct ri_controller;
struct pif;

enum si_dma_dir
{
    SI_NO_DMA,
    SI_DMA_READ,
    SI_DMA_WRITE
};

enum si_registers
{
    SI_DRAM_ADDR_REG,
    SI_PIF_ADDR_RD64B_REG,
    SI_R2_REG, /* reserved */
    SI_R3_REG, /* reserved */
    SI_PIF_ADDR_WR64B_REG,
    SI_R5_REG, /* reserved */
    SI_STATUS_REG,
    SI_REGS_COUNT
};

enum
{
    /* SI_STATUS - read */
    SI_STATUS_DMA_BUSY  = 0x0001,
    SI_STATUS_IO_BUSY   = 0x0002,
    SI_STATUS_DMA_ERROR = 0x0008,
    SI_STATUS_INTERRUPT = 0x1000,
};

struct si_controller
{
    uint32_t regs[SI_REGS_COUNT];
    unsigned char dma_dir;

    unsigned int dma_duration;

    struct mi_controller* mi;
    struct pif* pif;
    struct ri_controller* ri;
};

static osal_inline uint32_t si_reg(uint32_t address)
{
    return (address & 0x1f) >> 2;
}


void init_si(struct si_controller* si,
             unsigned int dma_duration,
             struct mi_controller* mi,
             struct pif* pif,
             struct ri_controller* ri);

void poweron_si(struct si_controller* si);

void read_si_regs(void* opaque, uint32_t address, uint32_t* value);
void write_si_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void si_end_of_dma_event(void* opaque);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - vi_controller.h                                         *
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


#include <stdint.h>
#include "core/types.h"
#include "osal/preproc.h"

struct mi_controller;
struct rdp_core;

enum vi_registers
{
    VI_STATUS_REG,
    VI_ORIGIN_REG,
    VI_WIDTH_REG,
    VI_V_INTR_REG,
    VI_CURRENT_REG,
    VI_BURST_REG,
    VI_V_SYNC_REG,
    VI_H_SYNC_REG,
    VI_LEAP_REG,
    VI_H_START_REG,
    VI_V_START_REG,
    VI_V_BURST_REG,
    VI_X_SCALE_REG,
    VI_Y_SCALE_REG,
    VI_REGS_COUNT
};

struct vi_controller
{
    uint32_t regs[VI_REGS_COUNT];
    unsigned int field;
    unsigned int clock;
    unsigned int expected_refresh_rate;

    /* Oscillator phase: field boundaries follow their scheduled deadlines,
     * independent of CPU interrupt-service latency or COUNT writes. */
    uint64_t field_tick_phase;
    int64_t field_start_count_clock;
    uint32_t field_ticks;

    struct mi_controller* mi;
    struct rdp_core* dp;
};

static osal_inline uint32_t vi_reg(uint32_t address)
{
    return (address & 0xffff) >> 2;
}


unsigned int vi_clock_from_tv_standard(core_system_type tv_standard);
unsigned int vi_expected_refresh_rate_from_tv_standard(core_system_type tv_standard);
double vi_actual_refresh_rate(const struct vi_controller* vi);
unsigned int vi_legacy_savestate_delay(const struct vi_controller* vi);
unsigned int vi_legacy_savestate_count_per_scanline(const struct vi_controller* vi);
void vi_rebase_timing(struct vi_controller* vi, unsigned int legacy_delay);
void vi_schedule_vertical_interrupt(struct vi_controller* vi);
void set_vi_vertical_interrupt(struct vi_controller* vi);

void init_vi(struct vi_controller* vi, unsigned int clock, unsigned int expected_refresh_rate,
             struct mi_controller* mi, struct rdp_core* dp);

void poweron_vi(struct vi_controller* vi);

void read_vi_regs(void* opaque, uint32_t address, uint32_t* value);
void write_vi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void vi_vertical_interrupt_event(void* opaque);



#endif
