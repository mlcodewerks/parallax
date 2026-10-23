/* Consolidated pif device implementation.
 * Component copyright and license notices are retained below.
 */

#define __STDC_FORMAT_MACROS

#include "device/pif/bootrom_hle.h"
#include <stdint.h>
#include <string.h>
#include "core/types.h"
#include "device/device.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/rcp.h"
#include "device/rcp/rsp/rsp_core.h"
#include "main/rom.h"
#include "device/pif/cic.h"
#include <stddef.h>
#include "core/log.h"
#include <inttypes.h>
#include "device/pif/n64_cic_nus_6105.h"
#include "device/pif/pif.h"
#include <assert.h>
#include "core/backend.h"
#include "core/joybus.h"
#include "device/memory.h"

/* ---- pif/bootrom_hle.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - bootrom_hle.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2016 Bobby Smiles                                       *
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

static unsigned int get_tv_type(void)
{
    switch(ROM_PARAMS.systemtype)
    {
    default:
    case SYSTEM_NTSC: return 1;
    case SYSTEM_PAL: return 0;
    case SYSTEM_MPAL: return 2;
    }
}

void pif_bootrom_hle_execute(struct r4300_core* r4300)
{
    if (r4300->start_address == 0xbfc00000) return;

    uint32_t pif24;
    unsigned int seed;       /* seed (depends on CIC version) */
    unsigned int rom_type;   /* 0:Cart, 1:DD */
    unsigned int reset_type; /* 0:ColdReset, 1:NMI */
    unsigned int s7;         /* ??? */
    uint32_t bsd_dom1_config;
    unsigned int tv_type = get_tv_type();   /* 0:PAL, 1:NTSC, 2:MPAL */

    int64_t* r4300_gpregs = r4300_regs(r4300);
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);

    /* setup CP0 registers */
    cp0_regs[CP0_STATUS_REG] = 0x34000000;
    cp0_regs[CP0_CONFIG_REG] = 0x0006e463;

    /* XXX: wait for SP to finish last operation (poll halt) */

    /* stop RSP (halt + clear interrupts) */
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_RSP_REGS + 4*SP_STATUS_REG, 0x0a, ~UINT32_C(0));

    /* XXX: wait for SP DMA to finish (poll busy) */

    /* stop PI (reset, clear interrupt) */
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_PI_REGS + 4*PI_STATUS_REG, 0x3, ~UINT32_C(0));

    /* blank screen */
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_VI_REGS + 4*VI_V_INTR_REG, 1023, ~UINT32_C(0));
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_VI_REGS + 4*VI_CURRENT_REG,   0, ~UINT32_C(0));
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_VI_REGS + 4*VI_H_START_REG,   0, ~UINT32_C(0));

    /* mute sound */
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_AI_REGS + 4*AI_DRAM_ADDR_REG, 0, ~UINT32_C(0));
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_AI_REGS + 4*AI_LEN_REG,       0, ~UINT32_C(0));

    /* XXX: wait for SP DMA to finish (poll busy) */

    /* XXX: copy IPL2 to IMEM (partialy done later as required by CIC x105)
     * and transfer execution to IPL2 */

    /* XXX: wait for PIF_3c[7] to be cleared */

    /* read and parse pif24 */
    r4300_read_aligned_word(r4300, R4300_KSEG1 + MM_PIF_MEM + PIF_ROM_SIZE + 0x24, &pif24);
    rom_type   = (pif24 >> 19) & 0x01;
    s7         = (pif24 >> 18) & 0x01;
    reset_type = (pif24 >> 17) & 0x01;
    seed       = (pif24 >>  8) & 0xff;
    /* setup s3-s7 registers (needed by OS) */
    r4300_gpregs[19] = rom_type;    /* s3 */
    r4300_gpregs[20] = tv_type;     /* s4 */
    r4300_gpregs[21] = reset_type;  /* s5 */
    r4300_gpregs[22] = seed;        /* s6 */
    r4300_gpregs[23] = s7;          /* s7 */

    /* XXX: wait for SI_RD_BUSY to be cleared, set PIF_3c[4] */

    /* configure ROM access
     * XXX: we skip the first temporary configuration */
    uint32_t rom_base = (rom_type == 0) ? MM_CART_ROM : MM_DD_ROM;
    r4300_read_aligned_word(r4300, R4300_KSEG1 + rom_base, &bsd_dom1_config);
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_PI_REGS + 4*PI_BSD_DOM1_LAT_REG, (bsd_dom1_config      ) & 0xff, ~UINT32_C(0));
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_PI_REGS + 4*PI_BSD_DOM1_PWD_REG, (bsd_dom1_config >>  8) & 0xff, ~UINT32_C(0));
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_PI_REGS + 4*PI_BSD_DOM1_PGS_REG, (bsd_dom1_config >> 16) & 0x0f, ~UINT32_C(0));
    r4300_write_aligned_word(r4300, R4300_KSEG1 + MM_PI_REGS + 4*PI_BSD_DOM1_RLS_REG, (bsd_dom1_config >> 20) & 0x03, ~UINT32_C(0));

    /* XXX: if XBus is used, wait until DPC_pipe_busy is cleared */

    /* copy IPL3 to dmem */
    memcpy((unsigned char*)mem_base_u32(r4300->mem->base, MM_RSP_MEM + 0x40),
           (unsigned char*)mem_base_u32(r4300->mem->base, rom_base + 0x40),
           0xfc0);

    /* XXX: compute IPL3 checksum */
    /* XXX: wait for SI_RD_BUSY to be cleared, set PIF_30 */
    /* XXX: wait for SI_RD_BUSY to be cleared, set PIF_34 */
    /* XXX: wait for SI_RD_BUSY to be cleared, set PIF_3c[5] */
    /* XXX: wait for PIF_3c[7] to be set */
    /* XXX: wait for SI_RD_BUSY to be cleared, set PIF_3c[6] */

    /* required by CIC x105 */
    uint32_t* imem = mem_base_u32(r4300->mem->base, MM_RSP_MEM + 0x1000);
    imem[0x0000/4] = 0x3c0dbfc0;
    imem[0x0004/4] = 0x8da807fc;
    imem[0x0008/4] = 0x25ad07c0;
    imem[0x000c/4] = 0x31080080;
    imem[0x0010/4] = 0x5500fffc;
    imem[0x0014/4] = 0x3c0dbfc0;
    imem[0x0018/4] = 0x8da80024;
    imem[0x001c/4] = 0x3c0bb000;

    /* required by CIC x105 */
    r4300_gpregs[11] = INT64_C(0xffffffffa4000040); /* t3 */
    r4300_gpregs[29] = INT64_C(0xffffffffa4001ff0); /* sp */
    r4300_gpregs[31] = INT64_C(0xffffffffa4001550); /* ra */

    /* XXX: should prepare execution of IPL3 in DMEM here :
     * e.g. jump to 0xa4000040 */
    *r4300_cp0_last_addr(&r4300->cp0) = 0xa4000040;
}

/* ---- pif/cic.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cic.c                                                   *
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

void init_cic_using_ipl3(struct cic* cic, const void* ipl3)
{
    size_t i;
    uint64_t crc = 0;

    static const struct cic cics[] =
    {
        { "5101", CIC_5101, 0xac },
        { "X101", CIC_X101, 0x3f },
        { "X102", CIC_X102, 0x3f },
        { "X103", CIC_X103, 0x78 },
        { "X105", CIC_X105, 0x91 },
        { "X106", CIC_X106, 0x85 },
        { "5167", CIC_5167, 0xdd },
        { "8303", CIC_8303, 0xdd },
        { "8401", CIC_8401, 0xdd },
        { "8501", CIC_8501, 0xde }
    };

    for (i = 0; i < 0xfc0/4; i++)
        crc += ((uint32_t*)ipl3)[i];

    switch(crc)
    {
        default:
            core_log(CORE_LOG_WARNING, "Unknown CIC type (%016" PRIX64 ")! using CIC 6102.", crc);
            /* fall through */
        case UINT64_C(0x000000D057C85244): i = 2; break; /* CIC_X102 */
        case UINT64_C(0x000000D0027FDF31):               /* CIC_X101 */
        case UINT64_C(0x000000CFFB631223): i = 1; break; /* CIC_X101 */
        case UINT64_C(0x000000D6497E414B): i = 3; break; /* CIC_X103 */
        case UINT64_C(0x0000011A49F60E96): i = 4; break; /* CIC_X105 */
        case UINT64_C(0x000000D6D5BE5580): i = 5; break; /* CIC_X106 */
        case UINT64_C(0x000001053BC19870): i = 6; break; /* CIC 5167 */
        case UINT64_C(0x000000A5F80BF620): i = 0; break; /* CIC 5101 */
        case UINT64_C(0x000000D2E53EF008): i = 7; break; /* CIC 8303 */
        case UINT64_C(0x000000D2E53EF39F): i = 8; break; /* CIC 8401 */
        case UINT64_C(0x000000D2E53E5DDA): i = 9; break; /* CIC 8501 */
    }

    memcpy(cic, &cics[i], sizeof(*cic));

    core_log(CORE_LOG_INFO, "Using CIC type %s", cic->name);
}

/* ---- pif/n64_cic_nus_6105.c ---- */

/*
 * Copyright 2011 X-Scale. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY X-Scale ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL X-Scale OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of X-Scale.
 *
 * This software provides an algorithm that emulates the protection scheme of
 * N64 PIF/CIC-NUS-6105, by determining the proper response to each challenge.
 * It was synthesized after a careful, exhaustive and detailed analysis of the
 * challenge/response pairs stored in the 'pif2.dat' file from Project 64.
 * These challenge/response pairs were the only resource used during this
 * project. There was no kind of physical access to N64 hardware.
 *
 * This project would have never been possible without the contribuitions of
 * the following individuals and organizations:
 *
 * - Oman: For being at the right place at the right time and being brave
 *       enough to pay a personal price so we could understand in a much deeper
 *       way how this magical console really works. We owe you so much.
 *
 * - Jovis: For all the positive energy and impressive hacking spirit that you
 *       shared with the N64 community. You were absolutely instrumental in
 *       several key events that shaped the N64 community in the last 14 years.
 *       Even if you're not physically with us anymore, your heritage, your
 *       knowledge and your attitude will never be forgotten.
 *
 *          'The candle that burns twice as bright burns half as long.'
 *
 * - LaC: For the endless contributions that you've given to the N64 community
 *       since the early days, when N64 was the next big thing. I've always
 *       admired the deep knowledge that you've gathered about the most little
 *       hardware details. Recently, you challanged us to find a small and
 *       concise algorithm that would emulate the behaviour of CIC-NUS-6105
 *       challenge/response protection scheme and here is the final result.
 *       LaC, Oman and Jovis were definitly the dream team of N64 reversing in
 *       the late 90's. Without your contributions, we would be much poorer.
 *
 * - marshall: For keeping the N64 scene alive during the last decade, when
 *       most people lost interest and moved along to different projects. You
 *       are the force that has been keeping us all together in the later
 *       years. When almost nobody cared about N64 anymore, you were always
 *       there, spreading the word, developing in the console, and lately,
 *       making impressive advances on the hardware side. I wish the best
 *       success to your new 64drive project.
 *
 * - hcs: For your contributions to the better understanding of the inner
 *       workings of the Reality Co-Processor (RCP). Your skills have impressed
 *       me for a long time now. And without your precious help by sharing your
 *       kownledge, I would have never understood the immense importance of
 *       Oman, Jovis and LaC achievements. Thank you !
 *
 * - Azimer & Tooie: For sharing with the N64 community your findings about the
 *       challenge/response pair used in 'Jet Force Gemini' and the 267
 *       challenge/response pairs used in 'Banjo Tooie', all stored in the
 *       'pif2.dat' file of Project 64. They were instrumental to the final
 *       success of this endeavour.
 *
 * - Silicon Graphics, Inc. (SGI): For creating MIPS R4000, MIPS R4300 and
 *       Reality Co-Processor (RCP). You were the ultimate dream creator during
 *       the late 80's and early 90's. A very special word of gratitude goes to
 *       the two teams that during those years created RCP and MIPS R4300. They
 *       were technological breakthroughs back then.
 *
 * On a personal note, I would like to show my deepest gratitude to _Bijou_,
 * for being always a source of endless hope and inspiration.
 *
 *   -= X-Scale =- (#n64dev@EFnet)
 */

void n64_cic_nus_6105(char chl[], char rsp[], int len)
{
    static const char lut0[0x10] = {
        0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
        0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9
    };
    static const char lut1[0x10] = {
        0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
        0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9
    };
    char key;
    const char* lut;
    int i, sgn, mag, mod;

    for (key = 0xB, lut = lut0, i = 0; i < len; i++) {
        rsp[i] = (key + 5 * chl[i]) & 0xF;
        key = lut[(int) rsp[i]];
        sgn = (rsp[i] >> 3) & 0x1;
        mag = ((sgn == 1) ? ~rsp[i] : rsp[i]) & 0x7;
        mod = (mag % 3 == 1) ? sgn : 1 - sgn;
        if (lut == lut1 && (rsp[i] == 0x1 || rsp[i] == 0x9))
            mod = 1;
        if (lut == lut1 && (rsp[i] == 0xB || rsp[i] == 0xE))
            mod = 0;
        lut = (mod == 1) ? lut1 : lut0;
    }
}

/* ---- pif/pif.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pif.c                                                   *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2002 Hacktarux                                          *
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

//#define DEBUG_PIF
#ifdef DEBUG_PIF
void print_pif(struct pif* pif)
{
    int i;
    for (i=0; i<(64/8); i++) {
        core_log(CORE_LOG_INFO, "%02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " | %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8,
                     pif->ram[i*8+0], pif->ram[i*8+1],pif->ram[i*8+2], pif->ram[i*8+3],
                     pif->ram[i*8+4], pif->ram[i*8+5],pif->ram[i*8+6], pif->ram[i*8+7]);
    }

    for(i = 0; i < PIF_CHANNELS_COUNT; ++i) {
        if (pif->channels[i].tx != NULL) {
            core_log(CORE_LOG_INFO, "Channel %u, tx=%02x rx=%02x cmd=%02x",
                i,
                *(pif->channels[i].tx),
                *(pif->channels[i].rx),
                pif->channels[i].tx_buf[0]);
        }
    }
}
#endif

static void process_channel(struct pif_channel* channel)
{
    /* don't process channel if it has been disabled */
    if (channel->tx == NULL) {
        return;
    }

    /* reset Tx/Rx just in case */
    *channel->tx &= 0x3f;
    *channel->rx &= 0x3f;

    /* set NoResponse if no device is connected */
    if (channel->ijbd == NULL) {
        *channel->rx |= 0x80;
        return;
    }

    /* do device processing */
    channel->ijbd->process(channel->jbd,
        channel->tx, channel->tx_buf,
        channel->rx, channel->rx_buf);
}

static void post_setup_channel(struct pif_channel* channel)
{
    if ((channel->ijbd == NULL)
    || (channel->ijbd->post_setup == NULL)) {
        return;
    }

    channel->ijbd->post_setup(channel->jbd,
        channel->tx, channel->tx_buf,
        channel->rx, channel->rx_buf);
}

void disable_pif_channel(struct pif_channel* channel)
{
    channel->tx = NULL;
    channel->rx = NULL;
    channel->tx_buf = NULL;
    channel->rx_buf = NULL;
}

size_t setup_pif_channel(struct pif_channel* channel, uint8_t* buf)
{
    uint8_t tx = buf[0] & 0x3f;
    uint8_t rx = buf[1] & 0x3f;

    /* XXX: check out of bounds accesses */

    channel->tx = buf;
    channel->rx = buf + 1;
    channel->tx_buf = buf + 2;
    channel->rx_buf = buf + 2 + tx;

    post_setup_channel(channel);

    return 2 + tx + rx;
}

void init_pif(struct pif* pif,
    uint8_t* pif_base,
    void* jbds[PIF_CHANNELS_COUNT],
    const struct joybus_device_interface* ijbds[PIF_CHANNELS_COUNT],
    const uint8_t* ipl3,
    struct r4300_core* r4300,
    struct si_controller* si)
{
    size_t i;

    pif->base = pif_base;
    pif->ram = pif_base + 0x7c0;

    for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
        pif->channels[i].jbd = jbds[i];
        pif->channels[i].ijbd = ijbds[i];
    }

    init_cic_using_ipl3(&pif->cic, ipl3);

    pif->r4300 = r4300;
    pif->si = si;
}

void reset_pif(struct pif* pif, unsigned int reset_type)
{
    size_t i;

    /* HACK: for allowing pifbootrom execution */
    unsigned int rom_type = (pif->cic.version == CIC_8303 || pif->cic.version == CIC_8401 || pif->cic.version == CIC_8501) ? 1 : 0;
    unsigned int s7 = 0;

    /* 0:ColdReset, 1:NMI */
    assert((reset_type & ~0x1) == 0);

    /* disable channel processing */
    for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
        disable_pif_channel(&pif->channels[i]);
    }

    /* set PIF_24 with reset informations */
    uint32_t* pif24 = (uint32_t*)(pif->ram + 0x24);
    *pif24 = (uint32_t)
         (((rom_type      & 0x1) << 19)
        | ((s7            & 0x1) << 18)
        | ((reset_type    & 0x1) << 17)
        | ((pif->cic.seed & 0xff) << 8)
        | 0x3f);
    *pif24 = fromhl(*pif24);

    /* clear PIF flags */
    pif->ram[0x3f] = 0x00;
}

void setup_channels_format(struct pif* pif)
{
    size_t i = 0;
    size_t k = 0;

    while (i < PIF_RAM_SIZE && k < PIF_CHANNELS_COUNT)
    {
        switch(pif->ram[i])
        {
        case 0x00: /* skip channel */
            disable_pif_channel(&pif->channels[k++]);
            ++i;
            break;

        case 0xff: /* dummy data */
            ++i;
            break;

        case 0xfe: /* end of channel setup - remaining channels are disabled */
            while (k < PIF_CHANNELS_COUNT) {
                disable_pif_channel(&pif->channels[k++]);
            }
            break;

        case 0xfd: /* channel reset - send reset command and discard the results */ {
            static uint8_t dummy_reset_buffer[PIF_CHANNELS_COUNT][6];

            /* setup reset command Tx=1, Rx=3, cmd=0xff */
            dummy_reset_buffer[k][0] = 0x01;
            dummy_reset_buffer[k][1] = 0x03;
            dummy_reset_buffer[k][2] = 0xff;

            setup_pif_channel(&pif->channels[k], dummy_reset_buffer[k]);
            ++k;
            ++i;
            }
            break;

        default: /* setup channel */

            /* HACK?: some games sends bogus PIF commands while accessing controller paks
             * Yoshi Story, Top Gear Rally 2, Indiana Jones, ...
             * When encountering such commands, we skip this bogus byte.
             */
            if ((i+1 < PIF_RAM_SIZE) && (pif->ram[i+1] == 0xfe)) {
                ++i;
                continue;
            }

            if ((i + 2) >= PIF_RAM_SIZE) {
                core_log(CORE_LOG_WARNING, "Truncated PIF command ! Stopping PIF channel processing");
                i = PIF_RAM_SIZE;
                continue;
            }

            i += setup_pif_channel(&pif->channels[k++], &pif->ram[i]);
        }
    }

    /* Zilmar-Spec plugin expect a call with control_id = -1 when RAM processing is done */
    if (core_input_command) {
        core_input_command(-1, NULL);
    }

#ifdef DEBUG_PIF
    core_log(CORE_LOG_INFO, "PIF setup channel");
    print_pif(pif);
#endif
}

static void process_cic_challenge(struct pif* pif)
{
    char challenge[30], response[30];
    size_t i;

    /* format the 'challenge' message into 30 nibbles for X-Scale's CIC code */
    for (i = 0; i < 15; ++i)
    {
        challenge[i*2]   = (pif->ram[0x30+i] >> 4) & 0x0f;
        challenge[i*2+1] =  pif->ram[0x30+i]       & 0x0f;
    }

    /* calculate the proper response for the given challenge (X-Scale's algorithm) */
    n64_cic_nus_6105(challenge, response, CHL_LEN - 2);
    pif->ram[0x2e] = 0;
    pif->ram[0x2f] = 0;

    /* re-format the 'response' into a byte stream */
    for (i = 0; i < 15; ++i)
    {
        pif->ram[0x30+i] = (response[i*2] << 4) + response[i*2+1];
    }

#ifdef DEBUG_PIF
    core_log(CORE_LOG_INFO, "PIF cic challenge");
    print_pif(pif);
#endif
}

void poweron_pif(struct pif* pif)
{
    memset(pif->ram, 0, PIF_RAM_SIZE);

    reset_pif(pif, 0); /* cold reset */
}

void read_pif_mem(void* opaque, uint32_t address, uint32_t* value)
{
    struct pif* pif = (struct pif*)opaque;
    uint32_t addr = pif_address(address);

    memcpy(value, pif->base + addr, sizeof(*value));
    if (addr >= PIF_ROM_SIZE)
        *value = tohl(*value);
}

void write_pif_mem(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct pif* pif = (struct pif*)opaque;
    uint32_t addr = pif_address(address);

    if (addr < PIF_ROM_SIZE)
    {
        core_log(CORE_LOG_ERROR, "Invalid write to PIF ROM: %08" PRIX32, address);
        return;
    }

    if ((pif->si->regs[SI_STATUS_REG] & (SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY)) == SI_STATUS_DMA_BUSY)
        return;
    masked_write((uint32_t*)(&pif->base[addr]), fromhl(value), fromhl(mask));

    /* CPU word writes share one outstanding PIF processing event. Keep every
     * word of a command while avoiding duplicate SI completion events. */
    if (pif->si->regs[SI_STATUS_REG] & SI_STATUS_IO_BUSY) return;

    pif->si->dma_dir = SI_DMA_WRITE;

    cp0_update_count(pif->r4300);
    pif->si->regs[SI_STATUS_REG] |= (SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY);
    add_interrupt_event(&pif->r4300->cp0, SI_INT,
        pif->si->dma_duration ? pif->si->dma_duration : (2150*3+3)/4);
}

void process_pif_ram(struct pif* pif)
{
    uint8_t flags = pif->ram[0x3f];
    uint8_t clrmask = 0x00;
    size_t k;

    if (flags == 0) {
#ifdef DEBUG_PIF
        core_log(CORE_LOG_INFO, "PIF process pif ram status=0x00");
        print_pif(pif);
#endif
        return;
    }

    if (flags & 0x01)
    {
        /* setup channels then clear format flag */
        setup_channels_format(pif);
        clrmask |= 0x01;
    }

    if (flags & 0x02)
    {
        /* disable channel processing when doing CIC challenge */
        for (k = 0; k < PIF_CHANNELS_COUNT; ++k) {
            disable_pif_channel(&pif->channels[k]);
        }

        /* CIC Challenge */
        process_cic_challenge(pif);
        clrmask |= 0x02;
    }

    if (flags & 0x08)
    {
        clrmask |= 0x08;
    }

    if (flags & 0x30)
    {
        pif->ram[0x3f] = 0x80;
    }

#ifdef DEBUG_PIF
    if (flags & 0xf4)
    {
        core_log(CORE_LOG_ERROR, "error in process_pif_ram(): %" PRIX8, flags);
    }
#endif

    pif->ram[0x3f] &= ~clrmask;
}

void update_pif_ram(struct pif* pif)
{
    size_t k;

    /* perform PIF/Channel communications */
    for (k = 0; k < PIF_CHANNELS_COUNT; ++k) {
        process_channel(&pif->channels[k]);
    }

    /* Zilmar-Spec plugin expect a call with control_id = -1 when RAM processing is done */
    if (core_input_read) {
        core_input_read(-1, NULL);
    }

#ifdef DEBUG_PIF
    core_log(CORE_LOG_INFO, "PIF post read");
    print_pif(pif);
#endif
}

void hw2_int_handler(void* opaque)
{
    struct pif* pif = (struct pif*)opaque;

    raise_maskable_interrupt(pif->r4300, CP0_CAUSE_IP4);
}
