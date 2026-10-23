/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - fb.c                                                    *
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

#include "fb.h"
#include "rdp_timing.h"

#include "core/types.h"
#include "core/log.h"
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rdram.h"
#include "osal/preproc.h"
#include "core/backend.h"

#include <string.h>

static osal_inline size_t fb_buffer_size(const core_framebuffer_info* fb_info)
{
    return fb_info->width * fb_info->height * fb_info->size;
}

void pre_framebuffer_read(struct fb* fb, uint32_t address)
{
    if (fb->native_notifications) {
        unsigned page = address >> CORE_FB_PAGE_SHIFT;
        if (page < FB_DIRTY_PAGES_COUNT && fb->dirty_page[page]) {
            // Resolve every image/depth owner on this page before clearing it.
            // A second image sharing the page must not lose its notification.
            renderer_framebuffer_read_range(page << CORE_FB_PAGE_SHIFT, 1u << CORE_FB_PAGE_SHIFT);
            fb->dirty_page[page] = 0;
        }
        return;
    }
    if (!fb->infos[0].addr) {
        return;
    }

    size_t i;

    for (i = 0; i < FB_INFOS_COUNT; ++i) {

        /* skip empty fb info */
        if (fb->infos[i].addr == 0) {
            continue;
        }

        /* if address in within a fb and its page is dirty,
         * notify GFX plugin and mark page as not dirty */
        uint32_t begin = fb->infos[i].addr;
        uint32_t end   = fb->infos[i].addr + fb_buffer_size(&fb->infos[i]) - 1;

        if ((address >= begin) && (address <= end) && (fb->dirty_page[address >> 12])) {
            renderer_framebuffer_read(address);
            fb->dirty_page[address >> 12] = 0;
        }
    }
}

void pre_framebuffer_read_range(struct fb* fb, uint32_t address, uint32_t length)
{
    if (fb->native_notifications) {
        uint64_t end = (uint64_t)address + length;
        const uint64_t limit = (uint64_t)FB_DIRTY_PAGES_COUNT << CORE_FB_PAGE_SHIFT;
        if (end > limit) end = limit;
        for (uint64_t at = address; at < end; at = (at & ~UINT64_C(4095)) + 4096)
            pre_framebuffer_read(fb, (uint32_t)at);
        return;
    }
    if (!length || !fb->infos[0].addr) return;
    const uint64_t limit = (uint64_t)FB_DIRTY_PAGES_COUNT << 12;
    const uint64_t read_end = (uint64_t)address + length;
    for (size_t i = 0; i < FB_INFOS_COUNT; ++i) {
        if (!fb->infos[i].addr) continue;
        const uint64_t begin = fb->infos[i].addr;
        const uint64_t end = begin + fb_buffer_size(&fb->infos[i]);
        uint64_t at = address > begin ? address : begin;
        uint64_t last = read_end < end ? read_end : end;
        if (last > limit) last = limit;
        // A transfer can begin before an image or cross several dirty pages.
        // Synchronize each intersecting page before copying any DMA words.
        while (at < last) {
            pre_framebuffer_read(fb, (uint32_t)at);
            at = (at & ~UINT64_C(4095)) + 4096;
        }
    }
}

void post_framebuffer_write(struct fb* fb, uint32_t address, uint32_t length)
{
    if (fb->timing && length) rdp_timing_invalidate_range(fb->timing, address, length);
    if (fb->native_notifications) {
        const uint64_t limit = (uint64_t)FB_DIRTY_PAGES_COUNT << CORE_FB_PAGE_SHIFT;
        uint64_t end = (uint64_t)address + length;
        if (end > limit) end = limit;
        if (!length || address >= end) return;
        const unsigned first = address >> CORE_FB_PAGE_SHIFT, last = (unsigned)((end - 1) >> CORE_FB_PAGE_SHIFT);
        for (unsigned page = first; page <= last; ++page)
            if (fb->native_pages[page] & CORE_FB_PAGE_WRITE) {
                renderer_framebuffer_write(address, (uint32_t)(end - address));
                return;
            }
        // Clean cached views only need one invalidation per page. There are
        // no pending GPU bytes on these pages to protect with exact stores.
        for (unsigned page = first; page <= last; ++page)
            if (fb->native_pages[page] & CORE_FB_PAGE_INVALIDATE) {
                fb->native_pages[page] &= ~CORE_FB_PAGE_INVALIDATE;
                renderer_framebuffer_write(page << CORE_FB_PAGE_SHIFT, 1u << CORE_FB_PAGE_SHIFT);
            }
        return;
    }
    if (!fb->infos[0].addr) {
        return;
    }

    size_t i;
    if (!length) return;
    const uint64_t write_end = (uint64_t)address + length;

    for (i = 0; i < FB_INFOS_COUNT; ++i) {

        /* skip empty fb info */
        if (fb->infos[i].addr == 0) {
            continue;
        }

        const uint64_t begin = fb->infos[i].addr;
        const uint64_t end = begin + fb_buffer_size(&fb->infos[i]);
        if ((uint64_t)address < end && write_end > begin) {
            /* DMA has already committed the entire range. A renderer may
             * flush pending GPU depth inside this callback, so splitting it
             * into word notifications would overwrite later same-value DMA
             * bytes before their notifications arrive. Notify the complete
             * write once, also when it crosses an image boundary. */
            renderer_framebuffer_write(address, length);
            return;
        }
    }
}


void init_fb(struct fb* fb,
             struct memory* mem,
             struct rdram* rdram,
             struct r4300_core* r4300)
{
    fb->timing = NULL;
    fb->mem = mem;
    fb->rdram = rdram;
    fb->r4300 = r4300;
}

void poweron_fb(struct fb* fb)
{
    memset(fb->native_pages, 0, sizeof(fb->native_pages));
    memset(fb->native_regions, 0, sizeof(fb->native_regions));
    fb->native_notifications = 0;
    memset(fb->dirty_page, 0, FB_DIRTY_PAGES_COUNT*sizeof(fb->dirty_page[0]));
    memset(fb->infos, 0, FB_INFOS_COUNT*sizeof(fb->infos[0]));
    fb->once = 1;
    memset(fb->timing_bytes, 0, sizeof(fb->timing_bytes));
}

void read_rdram_fb(void* opaque, uint32_t address, uint32_t* value)
{
    struct fb* fb = (struct fb*)opaque;
    pre_framebuffer_read(fb, address);
    read_rdram_dram(fb->rdram, address, value);
}

void write_rdram_fb(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct fb* fb = (struct fb*)opaque;
    /* Masked unaligned stores can select no bytes. In particular, WDC uses
     * these while updating CPU framebuffers. Do not invalidate GPU pixels
     * or report a four-byte CPU write when RDRAM was left untouched. */
    if (!mask)
        return;
    write_rdram_dram(fb->rdram, address, value, mask);

    /* Plugin notifications use N64 addresses, like PI/RSP DMA callbacks.
     * The host byte swap is already handled by write_rdram_dram; applying
     * S8/S16 here marks a different byte as CPU-written in GPU framebuffers. */
    uint32_t addr = address & ~0x3;
    size_t size = 4;

    switch(mask)
    {
    case 0x000000ff:
        addr += 3;
        size = 1;
        break;

    case 0x0000ff00:
        addr += 2;
        size = 1;
        break;

    case 0x00ff0000:
        addr += 1;
        size = 1;
        break;

    case 0xff000000:
        addr += 0;
        size = 1;
        break;

    case 0x0000ffff:
        addr += 2;
        size = 2;
        break;

    case 0xffff0000:
        addr += 0;
        size = 2;
        break;

    case 0xffffff00:
        addr += 0;
        size = 3;
        break;

    case 0x00ffffff:
        addr += 1;
        size = 3;
        break;

    case 0xffffffff:
        addr += 0;
        size = 4;
        break;

    default:
        core_log(CORE_LOG_WARNING, "Unknown mask %08x !!!", mask);
    }

    post_framebuffer_write(fb, addr, size);
}


#define R(x) read_ ## x
#define W(x) write_ ## x
#define RW(x) R(x), W(x)

static void unprotect_timing_ranges(struct fb *fb)
{
    struct mem_mapping mapping = {0, 0, CORE_MEMORY_RDRAM, {fb->rdram, RW(rdram_dram)}};
    for (unsigned i = 0; i < 8; ++i) if (fb->timing_bytes[i]) {
        mapping.begin = fb->timing_address[i];
        mapping.end = mapping.begin + fb->timing_bytes[i] - 1;
        apply_mem_mapping(fb->mem, &mapping);
        fb->timing_bytes[i] = 0;
    }
}

void protect_framebuffers(struct fb* fb)
{
    size_t i, j;
    struct mem_mapping fb_mapping = { 0, 0, CORE_MEMORY_RDRAM, { fb, RW(rdram_fb) } };

    unprotect_timing_ranges(fb);
    memset(fb->native_pages, 0, sizeof(fb->native_pages));
    fb->native_notifications = renderer_framebuffer_pages(fb->native_pages, FB_DIRTY_PAGES_COUNT);
    if (fb->native_notifications) {
        memset(fb->infos, 0, sizeof(fb->infos));
        for (unsigned page = 0; page < FB_DIRTY_PAGES_COUNT; ++page)
            fb->dirty_page[page] = (fb->native_pages[page] & CORE_FB_PAGE_READ) != 0;
        // Memory handlers cover 64 KiB regions; ownership remains at 4 KiB.
        for (unsigned region = 0; region < sizeof(fb->native_regions); ++region) {
            unsigned flags = 0;
            for (unsigned page = region * 16; page < (region + 1) * 16; ++page)
                flags |= fb->native_pages[page];
            fb->native_regions[region] = flags != 0;
            if (!flags) continue;
            fb_mapping.begin = region << 16;
            fb_mapping.end = fb_mapping.begin + 65535;
            apply_mem_mapping(fb->mem, &fb_mapping);
            if (fb->once) {
                fb->once = 0;
                invalidate_r4300_cached_code(fb->r4300, 0, 0);
            }
        }
    } else {
        if (renderer_framebuffer_info && renderer_framebuffer_read && renderer_framebuffer_write)
            renderer_framebuffer_info(fb->infos);
        else
            memset(fb->infos, 0, sizeof(fb->infos));

        for (i = 0; i < FB_INFOS_COUNT; ++i) {

            /* skip empty fb info */
            if (fb->infos[i].addr == 0) {
                continue;
            }

            /* map fb rw handlers */
            fb_mapping.begin = fb->infos[i].addr;
            fb_mapping.end   = fb->infos[i].addr + fb_buffer_size(&fb->infos[i]) - 1;
            apply_mem_mapping(fb->mem, &fb_mapping);

            /* mark all pages that are within a fb as dirty */
            for (j = fb_mapping.begin >> 12; j <= (fb_mapping.end >> 12); ++j) {
                fb->dirty_page[j] = 1;
            }

            /* disable dynarec "fast memory" code generation to avoid direct memory accesses */
            if (fb->once) {
                fb->once = 0;

                /* also need to invalidate cached code to regen non fast memory code path */
                invalidate_r4300_cached_code(fb->r4300, 0, 0);
            }
        }
    }
    /* Timing proofs need CPU store notifications even on backends without
     * FBInfo. Preserve the mapped ranges separately: a CPU store can discard
     * its proof before the following unprotect, and must not strand a mapping. */
    if (fb->timing) for (unsigned n = 0; n < 8; ++n) {
        const struct rdp_uniform_depth *u = &fb->timing->uniform[n];
        uint64_t bytes = (uint64_t)u->width * u->height * 2;
        if (!u->valid || !bytes || (uint64_t)u->address + bytes > fb->rdram->dram_size) continue;
        fb->timing_address[n] = u->address;
        fb->timing_bytes[n] = (uint32_t)bytes;
        fb_mapping.begin = u->address;
        fb_mapping.end = u->address + (uint32_t)bytes - 1;
        apply_mem_mapping(fb->mem, &fb_mapping);
        if (fb->once) {
            fb->once = 0;
            invalidate_r4300_cached_code(fb->r4300, 0, 0);
        }
    }

}

void unprotect_framebuffers(struct fb* fb)
{
    size_t i;
    struct mem_mapping ram_mapping = { 0, 0, CORE_MEMORY_RDRAM, { fb->rdram, RW(rdram_dram) } };

    unprotect_timing_ranges(fb);

    if (fb->native_notifications) {
        for (unsigned region = 0; region < sizeof(fb->native_regions); ++region) {
            if (!fb->native_regions[region]) continue;
            ram_mapping.begin = region << 16;
            ram_mapping.end = ram_mapping.begin + 65535;
            apply_mem_mapping(fb->mem, &ram_mapping);
            fb->native_regions[region] = 0;
        }
        return;
    }

    /* return early if FB info is not supported or empty */
    if (!fb->infos[0].addr) {
        return;
    }

    for (i = 0; i < FB_INFOS_COUNT; ++i) {

        /* skip empty fb info */
        if (fb->infos[i].addr == 0) {
            continue;
        }

        /* restore ram rw handlers */
        ram_mapping.begin = fb->infos[i].addr;
        ram_mapping.end   = fb->infos[i].addr + fb_buffer_size(&fb->infos[i]) - 1;
        apply_mem_mapping(fb->mem, &ram_mapping);
    }
}
