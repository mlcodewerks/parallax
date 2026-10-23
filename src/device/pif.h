#ifndef PARALLAX_DEVICE_PIF_H
#define PARALLAX_DEVICE_PIF_H

#include <stddef.h>
#include <stdint.h>
#include "osal/preproc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r4300_core;
struct joybus_device_interface;
struct si_controller;

/* ---- cic ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cic.h                                                   *
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

enum cic_version
{
    CIC_X101,
    CIC_X102,
    CIC_X103,
    CIC_X105,
    CIC_X106,
    CIC_5101,
    CIC_5167,
    CIC_8303,
    CIC_8401,
    CIC_8501
};

struct cic
{
    const char* name;
    enum cic_version version;
    unsigned int seed;
};

void init_cic_using_ipl3(struct cic* cic, const void* ipl3);

/* ---- n64_cic_nus_6105 ---- */

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

#define CHL_LEN 0x20

void n64_cic_nus_6105(char chl[], char rsp[], int len);

/* ---- bootrom_hle ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - bootrom_hle.h                                           *
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

/* Simulates end result of PIFBootROM execution.
 * This bypasses entirely the execution of real PIFBootROM */
void pif_bootrom_hle_execute(struct r4300_core* r4300);

/* ---- pif ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pif.h                                                   *
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

enum { PIF_ROM_SIZE = 0x7c0 };
enum { PIF_RAM_SIZE = 0x40 };
enum { PIF_CHANNELS_COUNT = 5 };

struct pif_channel
{
    void* jbd;
    const struct joybus_device_interface* ijbd;

    uint8_t* tx;
    uint8_t* tx_buf;
    uint8_t* rx;
    uint8_t* rx_buf;
};

void disable_pif_channel(struct pif_channel* channel);
size_t setup_pif_channel(struct pif_channel* channel, uint8_t* buf);

struct pif
{
    uint8_t* base;
    uint8_t* ram;
    struct pif_channel channels[PIF_CHANNELS_COUNT];

    struct cic cic;

    struct r4300_core* r4300;
    struct si_controller* si;
};

static osal_inline uint32_t pif_address(uint32_t address)
{
    return (address & 0xfffc);
}

void init_pif(struct pif* pif,
    uint8_t* pif_base,
    void* jbds[PIF_CHANNELS_COUNT],
    const struct joybus_device_interface* ijbds[PIF_CHANNELS_COUNT],
    const uint8_t* ipl3,
    struct r4300_core* r4300,
    struct si_controller* si);

void poweron_pif(struct pif* pif);

void reset_pif(struct pif* pif, unsigned int reset_type);

void setup_channels_format(struct pif* pif);

void read_pif_mem(void* opaque, uint32_t address, uint32_t* value);
void write_pif_mem(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void process_pif_ram(struct pif* pif);
void update_pif_ram(struct pif* pif);

void hw2_int_handler(void* opaque);

#ifdef __cplusplus
}
#endif

#endif
