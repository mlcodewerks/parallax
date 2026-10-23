/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - fb.h                                                    *
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

#ifndef PARALLAX_DEVICE_RCP_RDP_FB_H
#define PARALLAX_DEVICE_RCP_RDP_FB_H

#include <stdint.h>

#include "core/backend.h"

struct memory;
struct rdram;
struct r4300_core;

enum { FB_INFOS_COUNT = 6 };
enum { FB_DIRTY_PAGES_COUNT = 0x800 };

struct rdp_timing;
struct fb
{
    struct rdp_timing *timing;
    struct memory* mem;
    struct rdram* rdram;
    struct r4300_core* r4300;

    unsigned char dirty_page[FB_DIRTY_PAGES_COUNT];
    unsigned char native_pages[FB_DIRTY_PAGES_COUNT];
    unsigned char native_regions[FB_DIRTY_PAGES_COUNT / 16];
    unsigned int native_notifications;
    core_framebuffer_info infos[FB_INFOS_COUNT];
    unsigned int once;
    uint32_t timing_address[8], timing_bytes[8];
};

void init_fb(struct fb* fb,
             struct memory* mem,
             struct rdram* rdram,
             struct r4300_core* r4300);

void poweron_fb(struct fb* fb);

void read_rdram_fb(void* opaque, uint32_t address, uint32_t* value);
void write_rdram_fb(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void protect_framebuffers(struct fb* fb);
void unprotect_framebuffers(struct fb* fb);

void pre_framebuffer_read(struct fb* fb, uint32_t address);
void pre_framebuffer_read_range(struct fb* fb, uint32_t address, uint32_t length);
void post_framebuffer_write(struct fb* fb, uint32_t address, uint32_t length);

#endif
