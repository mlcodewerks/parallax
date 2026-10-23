/* Consolidated gb device implementation.
 * Component copyright and license notices are retained below.
 */

#include "device/gb.h"
#include "core/types.h"
#include "core/log.h"
#include "core/rumble.h"
#include "core/storage.h"
#include "core/video_capture.h"
#include <assert.h>
#include <string.h>
#include "core/clock.h"

/* ---- gb/gb_cart.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - gb_cart.c                                               *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2015 Bobby Smiles                                       *
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

/* Most of the mappers information comes from
 * "The Cycle-Accurate Game Boy Docs" by AntonioND
 */

#if 0
void cv_imshow(const char* name, unsigned int width, unsigned int height, int channels, void* data);
#else
#define cv_imshow(...)
#endif

enum gbcart_extra_devices
{
    GED_NONE          = 0x00,
    GED_RAM           = 0x01,
    GED_BATTERY       = 0x02,
    GED_RTC           = 0x04,
    GED_RUMBLE        = 0x08,
    GED_ACCELEROMETER = 0x10,
    GED_CAMERA        = 0x20,
};

/* various helper functions for ram, rom, or MBC uses */

static void read_rom(const void* rom_storage, const struct storage_backend_interface* irom_storage, uint16_t address, uint8_t* data, size_t size)
{
    assert(size > 0);

    if (address + size > irom_storage->size(rom_storage))
    {
        core_log(CORE_LOG_WARNING, "Out of bound read from GB ROM %04x", address);
        return;
    }

    memcpy(data, irom_storage->data(rom_storage) + address, size);
}

static void read_ram(const void* ram_storage, const struct storage_backend_interface* iram_storage, unsigned int enabled, uint16_t address, uint8_t* data, size_t size, uint8_t mask)
{
    size_t i;
    assert(size > 0);

    /* RAM has to be enabled before use */
    if (!enabled) {
        core_log(CORE_LOG_WARNING, "Trying to read from non enabled GB RAM %04x", address);
        memset(data, 0xff, size);
        return;
    }

    /* RAM must be present */
    if (iram_storage->data(ram_storage) == NULL) {
        core_log(CORE_LOG_WARNING, "Trying to read from absent GB RAM %04x", address);
        memset(data, 0xff, size);
        return;
    }

    if (address + size > iram_storage->size(ram_storage))
    {
        core_log(CORE_LOG_WARNING, "Out of bound read from GB RAM %04x", address);
        return;
    }

    memcpy(data, iram_storage->data(ram_storage) + address, size);

    if (mask != UINT8_C(0xff)) {
        for (i = 0; i < size; ++i) {
            data[i] &= mask;
        }
    }
}

static void write_ram(void* ram_storage, const struct storage_backend_interface* iram_storage, unsigned int enabled, uint16_t address, const uint8_t* data, size_t size, uint8_t mask)
{
    size_t i;
    uint8_t* dst;

    assert(size > 0);

    /* RAM has to be enabled before use */
    if (!enabled) {
        core_log(CORE_LOG_WARNING, "Trying to write to non enabled GB RAM %04x", address);
        return;
    }

    /* RAM must be present */
    if (iram_storage->data(ram_storage) == NULL) {
        core_log(CORE_LOG_WARNING, "Trying to write to absent GB RAM %04x", address);
        return;
    }

    if (address + size > iram_storage->size(ram_storage))
    {
        core_log(CORE_LOG_WARNING, "Out of bound write to GB RAM %04x", address);
        return;
    }

    dst = iram_storage->data(ram_storage) + address;
    memcpy(dst, data, size);

    if (mask != UINT8_C(0xff)) {
        for (i = 0; i < size; ++i) {
            dst[i] &= mask;
        }
    }
    iram_storage->save(ram_storage, address, size);
}

static void set_ram_enable(struct gb_cart* gb_cart, uint8_t value)
{
    gb_cart->ram_enable = ((value & 0x0f) == 0x0a) ? 1 : 0;
    core_log(CORE_LOG_VERBOSE, "RAM enable = %02x", gb_cart->ram_enable);
}

static int read_gb_cart_nombc(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    switch(address >> 13)
    {
    /* 0x0000-0x7fff: ROM */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, address, data, size);
        break;

    /* 0xa000-0xbfff: RAM */
    case (0xa000 >> 13):
        read_ram(gb_cart->ram_storage, gb_cart->iram_storage, 1, address - 0xa000, data, size, UINT8_C(0xff));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart read (nombc): %04x", address);
    }

    return 0;
}

static int write_gb_cart_nombc(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    switch(address >> 13)
    {
    /* 0x0000-0x7fff: ROM */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        core_log(CORE_LOG_VERBOSE, "Trying to write to GB ROM %04x", address);
        break;

    /* 0xa000-0xbfff: RAM */
    case (0xa000 >> 13):
        write_ram(gb_cart->ram_storage, gb_cart->iram_storage, 1, address - 0xa000, data, size, UINT8_C(0xff));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart write (nombc): %04x", address);
    }

    return 0;
}

static int read_gb_cart_mbc1(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    switch(address >> 13)
    {
    /* 0x0000-0x3fff: ROM bank 00 */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, address, data, size);
        break;

    /* 0x4000-0x7fff: ROM bank 01-7f */
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, (address - 0x4000) + (gb_cart->rom_bank * 0x4000), data, size);
        break;

    /* 0xa000-0xbfff: RAM bank 00-03 */
    case (0xa000 >> 13):
        read_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + (gb_cart->ram_bank * 0x2000), data, size, UINT8_C(0xff));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart read (MBC1): %04x", address);
    }

    return 0;
}

static int write_gb_cart_mbc1(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    uint8_t bank;
    uint8_t value = data[size-1];

    switch(address >> 13)
    {
    /* 0x0000-0x1fff: RAM enable */
    case (0x0000 >> 13):
        set_ram_enable(gb_cart, value);
        break;

    /* 0x2000-0x3fff: ROM bank select (low 5 bits) */
    case (0x2000 >> 13):
        bank = value & 0x1f;
        gb_cart->rom_bank = (gb_cart->rom_bank & ~UINT8_C(0x1f)) | (bank == 0) ? 1 : bank;
        core_log(CORE_LOG_VERBOSE, "MBC1 set rom bank %02x", gb_cart->rom_bank);
        break;

    /* 0x4000-0x5fff: RAM bank / upper ROM bank select (2 bits) */
    case (0x4000 >> 13):
        bank = value & 0x3;
        if (gb_cart->mbc1_mode == 0) {
            /* ROM mode */
            gb_cart->rom_bank = (gb_cart->rom_bank & 0x1f) | (bank << 5);
        }
        else {
            /* RAM mode */
            gb_cart->ram_bank = bank;
        }
        core_log(CORE_LOG_VERBOSE, "MBC1 set ram bank %02x", gb_cart->ram_bank);
        break;

    /* 0x6000-0x7fff: ROM/RAM mode (1 bit) */
    case (0x6000 >> 13):
        gb_cart->mbc1_mode = (value & 0x1);
        if (gb_cart->mbc1_mode == 0) {
            /* only RAM bank 0 is accessible in ROM mode */
            gb_cart->ram_bank = 0;
        } else {
            /* only ROM banks 0x01 - 0x1f are accessible in RAM mode */
            gb_cart->rom_bank &= 0x1f;
        }
        break;

    /* 0xa000-0xbfff: RAM bank 00-03 */
    case (0xa000 >> 13):
        write_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + (gb_cart->ram_bank * 0x2000), data, size, UINT8_C(0xff));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart write (MBC1): %04x", address);
    }

    return 0;
}

static int read_gb_cart_mbc2(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    switch (address >> 13)
    {
    /* 0x0000-0x3fff: ROM bank 00 */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, address, data, size);
        break;

    /* 0x4000-0x7fff: ROM bank 01-0f */
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, (address - 0x4000) + (gb_cart->rom_bank * 0x4000), data, size);
        break;

    /* 0xa000-0xa1ff: internal 512x4bit RAM */
    case (0xa000 >> 13):
        read_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000), data, size, UINT8_C(0x0f));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart read (MBC2): %04x", address);
    }

    return 0;
}

static int write_gb_cart_mbc2(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    uint8_t bank;
    uint8_t value = data[size-1];

    switch(address >> 13)
    {
    /* 0x0000-0x1fff: RAM enable */
    case (0x0000 >> 13):
        if ((address & 0x0100) == 0) {
            set_ram_enable(gb_cart, value);
        }
        break;

    /* 0x2000-0x3fff: ROM bank select (low 4 bits) */
    case (0x2000 >> 13):
        if ((address & 0x0100) != 0) {
            bank = value & 0x0f;
            gb_cart->rom_bank = (bank == 0) ? 1 : bank;
            core_log(CORE_LOG_VERBOSE, "MBC2 set rom bank %02x", gb_cart->rom_bank);
        }
        break;

    /* 0xa000-0xa1ff: internal 512x4bit RAM */
    case (0xa000 >> 13):
        write_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000), data, size, UINT8_C(0x0f));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart write (MBC2): %04x", address);
    }

    return 0;
}

static int read_gb_cart_mbc3(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    switch(address >> 13)
    {
    /* 0x0000-0x3fff: ROM bank 00 */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, address, data, size);
        break;

    /* 0x4000-0x7fff: ROM bank 01-7f */
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, (address - 0x4000) + (gb_cart->rom_bank * 0x4000), data, size);
        break;

    /* 0xa000-0xbfff: RAM bank 00-07 or RTC register 08-0c */
    case (0xa000 >> 13):
        switch(gb_cart->ram_bank)
        {
        /* RAM banks */
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
            read_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + (gb_cart->ram_bank * 0x2000), data, size, UINT8_C(0xff));
            break;

        /* RTC registers */
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0c:
            /* RAM has to be enabled before use */
            if (!gb_cart->ram_enable) {
                core_log(CORE_LOG_WARNING, "Trying to read from non enabled GB RAM %04x", address);
                memset(data, 0xff, size);
                break;
            }

            if (!(gb_cart->extra_devices & GED_RTC)) {
                core_log(CORE_LOG_WARNING, "Trying to read from absent RTC %04x", address);
                memset(data, 0xff, size);
                break;
            }

            memset(data, read_mbc3_rtc_regs(&gb_cart->rtc, gb_cart->ram_bank - 0x08), size);
            break;

        default:
            core_log(CORE_LOG_WARNING, "Unknown device mapped in RAM/RTC space: %04x", address);
        }
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart read (MBC3): %04x", address);
    }

    return 0;
}

static int write_gb_cart_mbc3(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    uint8_t bank;
    uint8_t value = data[size-1];

    switch(address >> 13)
    {
    /* 0x0000-0x1fff: RAM/RTC enable */
    case (0x0000 >> 13):
        set_ram_enable(gb_cart, value);
        break;

    /* 0x2000-0x3fff: ROM bank select */
    case (0x2000 >> 13):
        bank = value & 0x7f;
        gb_cart->rom_bank = (bank == 0) ? 1 : bank;
        core_log(CORE_LOG_VERBOSE, "MBC3 set rom bank %02x", gb_cart->rom_bank);
        break;

    /* 0x4000-0x5fff: RAM bank / RTC register select */
    case (0x4000 >> 13):
        gb_cart->ram_bank = value;
        core_log(CORE_LOG_VERBOSE, "MBC3 set ram bank %02x", gb_cart->ram_bank);
        break;

    /* 0x6000-0x7fff: latch clock registers */
    case (0x6000 >> 13):
        if (!(gb_cart->extra_devices & GED_RTC)) {
            core_log(CORE_LOG_WARNING, "Trying to latch to absent RTC %04x", address);
            break;
        }

        latch_mbc3_rtc_regs(&gb_cart->rtc, value);
        break;

    /* 0xa000-0xbfff: RAM bank 00-07 or RTC register 08-0c */
    case (0xa000 >> 13):
        switch(gb_cart->ram_bank)
        {
        /* RAM banks */
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
            write_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + (gb_cart->ram_bank * 0x2000), data, size, UINT8_C(0xff));
            break;

        /* RTC registers */
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0c:
            /* RAM has to be enabled before use */
            if (!gb_cart->ram_enable) {
                core_log(CORE_LOG_WARNING, "Trying to write to non enabled GB RAM %04x", address);
                break;
            }

            if (!(gb_cart->extra_devices & GED_RTC)) {
                core_log(CORE_LOG_WARNING, "Trying to write to absent RTC %04x", address);
                break;
            }

            write_mbc3_rtc_regs(&gb_cart->rtc, gb_cart->ram_bank - 0x08, value);
            break;

        default:
            core_log(CORE_LOG_WARNING, "Unknwown device mapped in RAM/RTC space: %04x", address);
        }
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart write (MBC3): %04x", address);
    }

    return 0;
}

static int read_gb_cart_mbc5(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    switch(address >> 13)
    {
    /* 0x0000-0x3fff: ROM bank 00 */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, address, data, size);
        break;

    /* 0x4000-0x7fff: ROM bank 00-ff (???) */
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, (address - 0x4000) + (gb_cart->rom_bank * 0x4000), data, size);
        break;

    /* 0xa000-0xbfff: RAM bank 00-07 */
    case (0xa000 >> 13):
        read_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + ((gb_cart->ram_bank & 0x7) * 0x2000), data, size, UINT8_C(0xff));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart read (MBC5): %04x", address);
    }

    return 0;
}

static int write_gb_cart_mbc5(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    uint8_t value = data[size-1];

    switch(address >> 13)
    {
    /* 0x0000-0x1fff: RAM enable */
    case (0x0000 >> 13):
        set_ram_enable(gb_cart, value);
        break;

    /* 0x2000-0x3fff: ROM bank select */
    case (0x2000 >> 13):
        if (address < 0x3000)
        {
            gb_cart->rom_bank &= 0xff00;
            gb_cart->rom_bank |= value;
        }
        else
        {
            gb_cart->rom_bank &= 0x00ff;
            gb_cart->rom_bank |= (value & 0x01) << 8;
        }
        core_log(CORE_LOG_VERBOSE, "MBC5 set rom bank %04x", gb_cart->rom_bank);
        break;

    /* 0x4000-0x5fff: RAM bank select */
    case (0x4000 >> 13):
        gb_cart->ram_bank = value & 0x0f;
        if (gb_cart->extra_devices & GED_RUMBLE) {
            gb_cart->irumble->exec(gb_cart->rumble, ((gb_cart->ram_bank & 0x8) == 0) ? RUMBLE_STOP : RUMBLE_START);
        }
        core_log(CORE_LOG_VERBOSE, "MBC5 set ram bank %02x", gb_cart->ram_bank);
        break;

    /* 0xa000-0xbfff: RAM bank 00-0f */
    case (0xa000 >> 13):
        write_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + ((gb_cart->ram_bank & 0x07)* 0x2000), data, size, UINT8_C(0xff));
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart write (MBC5): %04x", address);
    }

    return 0;
}

static int read_gb_cart_mbc6(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return 0;
}

static int write_gb_cart_mbc6(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return 0;
}

static int read_gb_cart_mbc7(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return 0;
}

static int write_gb_cart_mbc7(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return 0;
}

static int read_gb_cart_mmm01(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return 0;
}

static int write_gb_cart_mmm01(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return 0;
}

/* TODO: extract each mbc into its own module, store in gb_cart a union of them */
static uint8_t apply_dithering_matrix(const uint8_t* d, uint8_t value, unsigned int x, unsigned int y)
{
    d += ((y & 3) * 4 + (x & 3)) * 3;

    if      (value < d[0]) return 0x00; /* black */
    else if (value < d[1]) return 0x40; /* dark gray */
    else if (value < d[2]) return 0x80; /* light gray */
    return 0xc0; /* white */
}

static void grab_pocket_cam_image(struct pocket_cam* cam)
{
    static const uint8_t pm[4][2] = {
        { 0x00, 0x01 }, /* negative */
        { 0x01, 0x00 }, /* positive */
        { 0x01, 0x02 }, /* edge detect */
        { 0x01, 0x02 }  /* edge detect */
    };

    uint8_t regs[M64282FP_REGS_COUNT];
    uint8_t tiles[16][16][16];
    uint8_t bgr[M64282FP_SENSOR_H][M64282FP_SENSOR_W][3];
    uint8_t img[M64282FP_SENSOR_H][M64282FP_SENSOR_W];
    unsigned int x, y;

    /* setup sensor regs */
    unsigned int pm_mode = (cam->regs[0] >> 1) & 0x3;
    regs[M64282FP_Z_O] = cam->regs[5];
    regs[M64282FP_N_VH_G] = cam->regs[1];
    regs[M64282FP_C_LO] = cam->regs[3];
    regs[M64282FP_C_HI] = cam->regs[2];
    regs[M64282FP_P] = pm[pm_mode][0];
    regs[M64282FP_M] = pm[pm_mode][1];
    regs[M64282FP_X] = 0x01;
    regs[M64282FP_E_I_V] = cam->regs[4];

#if 0
    core_log(CORE_LOG_INFO, "GB cam regs:\n"
        "P=%02x M=%02x X=%02x N_VH_G=%02x C=%04x E_I_V=%02x Z_O=%02x\n"
        "%02x %02x %02x  %02x %02x %02x  %02x %02x %02x  %02x %02x %02x\n"
        "%02x %02x %02x  %02x %02x %02x  %02x %02x %02x  %02x %02x %02x\n"
        "%02x %02x %02x  %02x %02x %02x  %02x %02x %02x  %02x %02x %02x\n"
        "%02x %02x %02x  %02x %02x %02x  %02x %02x %02x  %02x %02x %02x",
        regs[M64282FP_P], regs[M64282FP_M], regs[M64282FP_X], regs[M64282FP_N_VH_G],
        regs[M64282FP_C_HI] << 8 | regs[M64282FP_C_LO], regs[M64282FP_E_I_V], regs[M64282FP_Z_O],
        cam->regs[6], cam->regs[7], cam->regs[8],
        cam->regs[9], cam->regs[10], cam->regs[11],
        cam->regs[12], cam->regs[13], cam->regs[14],
        cam->regs[15], cam->regs[16], cam->regs[17],
        cam->regs[18], cam->regs[19], cam->regs[20],
        cam->regs[21], cam->regs[22], cam->regs[23],
        cam->regs[24], cam->regs[25], cam->regs[26],
        cam->regs[27], cam->regs[28], cam->regs[29],
        cam->regs[30], cam->regs[31], cam->regs[32],
        cam->regs[33], cam->regs[34], cam->regs[35],
        cam->regs[36], cam->regs[37], cam->regs[38],
        cam->regs[39], cam->regs[40], cam->regs[41],
        cam->regs[42], cam->regs[43], cam->regs[44],
        cam->regs[45], cam->regs[46], cam->regs[47],
        cam->regs[48], cam->regs[49], cam->regs[50],
        cam->regs[51], cam->regs[52], cam->regs[53]);
#endif

    /* grab BGR image */
    if (cam->ivcap->grab_image(cam->vcap, bgr) != CORE_OK) {
        memset(cam->ram, UINT8_C(0xff), sizeof(tiles));
        return;
    }

    cv_imshow("bgr image", M64282FP_SENSOR_W, M64282FP_SENSOR_H, 3, bgr);

    /* convert to gray using (2R+5G+1B)/8 formula */
    const uint8_t* bgr_ptr = &bgr[0][0][0];
    uint8_t* gray_ptr = &img[0][0];
    for (y = 0; y < M64282FP_SENSOR_H; ++y) {
        for (x = 0; x < M64282FP_SENSOR_W; ++x) {
            gray_ptr[0] = (1*bgr_ptr[0] + 5*bgr_ptr[1] + 2*bgr_ptr[2]) / 8;
            bgr_ptr += 3;
            gray_ptr += 1;
        }
    }

    /* apply m64282fp processings */
    cv_imshow("gray image", M64282FP_SENSOR_W, M64282FP_SENSOR_H, 1, img);
    process_m64282fp_image(img, regs);
    cv_imshow("m64282fp", M64282FP_SENSOR_W, M64282FP_SENSOR_H, 1, img);

    /* convert to dithered GB tile format */
    memset(tiles, 0, sizeof(tiles));
    for (y = 0; y < M64282FP_SENSOR_H; ++y) {
        for (x = 0; x < M64282FP_SENSOR_W; ++x) {
            /* apply dithering matrix */
            uint8_t c = UINT8_C(0xc0) - apply_dithering_matrix(&cam->regs[6], img[y][x], x, y);
            img[y][x] = c;
            /* encode as tiles */
            uint8_t* tile_base = &tiles[y >> 3][x >> 3][(y & 7) << 1];
            if (c & 0x40) { tile_base[0] |= (1U << (7 - (7 & x))); }
            if (c & 0x80) { tile_base[1] |= (1U << (7 - (7 & x))); }
        }
    }

    cv_imshow("dithered", M64282FP_SENSOR_W, M64282FP_SENSOR_H, 1, img);

    /* copy to ram */
    memcpy(cam->ram, tiles, sizeof(tiles));
}

static void init_pocket_cam(struct pocket_cam* cam, uint8_t* ram, void* vcap, const struct video_capture_backend_interface* ivcap)
{
    cam->ram = ram;

    cam->vcap = vcap;
    cam->ivcap = ivcap;
}

static void poweron_pocket_cam(struct pocket_cam* cam)
{
    memset(cam->regs, 0, POCKET_CAM_REGS_COUNT*sizeof(cam->regs[0]));
}

static uint8_t read_pocket_cam_regs(struct pocket_cam* cam, unsigned int reg)
{
    /* All registers except reg 0 are write-only and therefore return 0x00 */
    return (reg == 0)
        ? cam->regs[0]
        : 0x00;
}

static void write_pocket_cam_regs(struct pocket_cam* cam, unsigned int reg, uint8_t value)
{
    if (reg >= POCKET_CAM_REGS_COUNT) {
#if 0 /* silence it because it happens always (2x32 bytes writes go past 0x36 registers) */
        core_log(CORE_LOG_WARNING, "Out of range camera register write %02x = %02x",
            reg, value);
#endif
        return;
    }

    cam->regs[reg] = value;

    if (reg == 0) {
        cam->regs[reg] &= 0x7;

        if (cam->regs[reg] & 0x1) {
            grab_pocket_cam_image(cam);
            cam->regs[reg] &= ~0x01;
        }
    }
}

static int read_gb_cart_pocket_cam(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    switch(address >> 13)
    {
    /* 0x0000-0x3fff: ROM bank 00 */
    case (0x0000 >> 13):
    case (0x2000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, address, data, size);
        break;

    /* 0x4000-0x7fff: ROM bank 00-3f */
    case (0x4000 >> 13):
    case (0x6000 >> 13):
        read_rom(gb_cart->rom_storage, gb_cart->irom_storage, (address - 0x4000) + (gb_cart->rom_bank * 0x4000), data, size);
        break;

    /* 0xa000-0xbfff: RAM bank 00-0f, Camera registers & 0x10 */
    case (0xa000 >> 13):
        if (gb_cart->ram_bank & 0x10) {
            size_t i;
            for(i = 0; i < size; ++i) {
                data[i] = read_pocket_cam_regs(&gb_cart->cam, ((address+i) & 0x7f));
            }
        }
        else {
            read_ram(gb_cart->ram_storage, gb_cart->iram_storage, 1, (address - 0xa000) + (gb_cart->ram_bank * 0x2000), data, size, UINT8_C(0xff));
        }
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart read (cam): %04x", address);
    }

    return 0;
}

static int write_gb_cart_pocket_cam(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    uint8_t value = data[size-1];

    switch(address >> 13)
    {
    /* 0x0000-0x1fff: RAM write enable */
    case (0x0000 >> 13):
        set_ram_enable(gb_cart, value);
        break;

    /* 0x2000-0x3fff: ROM bank select */
    case (0x2000 >> 13):
        gb_cart->rom_bank = (value & 0x3f);
        core_log(CORE_LOG_VERBOSE, "CAM set rom bank %04x", gb_cart->rom_bank);
        break;

    /* 0x4000-0x5fff: RAM bank select */
    case (0x4000 >> 13):
        if (value & 0x10) {
            gb_cart->ram_bank = value;
            core_log(CORE_LOG_VERBOSE, "CAM set register bank %02x", gb_cart->ram_bank);
        }
        else {
            gb_cart->ram_bank = (value & 0x0f);
            core_log(CORE_LOG_VERBOSE, "CAM set ram bank %02x", gb_cart->ram_bank);
        }
        break;

    /* 0xa000-0xbfff: RAM bank 00-0f, Camera registers & 0x10 */
    case (0xa000 >> 13):
        if (gb_cart->ram_bank & 0x10) {
            size_t i;
            for(i = 0; i < size; ++i) {
                write_pocket_cam_regs(&gb_cart->cam, ((address+i) & 0x7f), data[i]);
            }
        }
        else {
            write_ram(gb_cart->ram_storage, gb_cart->iram_storage, gb_cart->ram_enable, (address - 0xa000) + (gb_cart->ram_bank * 0x2000), data, size, UINT8_C(0xff));
        }
        break;

    default:
        core_log(CORE_LOG_WARNING, "Invalid cart write (cam): %04x", address);
    }

    return 0;
}

static int read_gb_cart_bandai_tama5(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return 0;
}

static int write_gb_cart_bandai_tama5(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return 0;
}

static int read_gb_cart_huc1(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return 0;
}

static int write_gb_cart_huc1(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return 0;
}

static int read_gb_cart_huc3(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return 0;
}

static int write_gb_cart_huc3(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return 0;
}

struct parsed_cart_type
{
    const char* mbc;
    int (*read_gb_cart)(struct gb_cart*,uint16_t,uint8_t*,size_t);
    int (*write_gb_cart)(struct gb_cart*,uint16_t,const uint8_t*,size_t);
    unsigned int extra_devices;
};

static const struct parsed_cart_type* parse_cart_type(uint8_t cart_type)
{
#define MBC(x) #x, read_gb_cart_ ## x, write_gb_cart_ ## x
    static const struct parsed_cart_type nombc_none           = { MBC(nombc),        GED_NONE };
    static const struct parsed_cart_type nombc_ram            = { MBC(nombc),        GED_RAM };
    static const struct parsed_cart_type nombc_ram_batt       = { MBC(nombc),        GED_RAM | GED_BATTERY };

    static const struct parsed_cart_type mbc1_none            = { MBC(mbc1),         GED_NONE };
    static const struct parsed_cart_type mbc1_ram             = { MBC(mbc1),         GED_RAM  };
    static const struct parsed_cart_type mbc1_ram_batt        = { MBC(mbc1),         GED_RAM | GED_BATTERY };

    static const struct parsed_cart_type mbc2_none            = { MBC(mbc2),         GED_NONE };
    static const struct parsed_cart_type mbc2_ram_batt        = { MBC(mbc2),         GED_RAM | GED_BATTERY };

    static const struct parsed_cart_type mmm01_none           = { MBC(mmm01),        GED_NONE };
    static const struct parsed_cart_type mmm01_ram            = { MBC(mmm01),        GED_RAM  };
    static const struct parsed_cart_type mmm01_ram_batt       = { MBC(mmm01),        GED_RAM | GED_BATTERY };

    static const struct parsed_cart_type mbc3_none            = { MBC(mbc3),         GED_NONE };
    static const struct parsed_cart_type mbc3_ram             = { MBC(mbc3),         GED_RAM  };
    static const struct parsed_cart_type mbc3_ram_batt        = { MBC(mbc3),         GED_RAM | GED_BATTERY };
    static const struct parsed_cart_type mbc3_batt_rtc        = { MBC(mbc3),         GED_BATTERY | GED_RTC };
    static const struct parsed_cart_type mbc3_ram_batt_rtc    = { MBC(mbc3),         GED_RAM | GED_BATTERY | GED_RTC };

    static const struct parsed_cart_type mbc5_none            = { MBC(mbc5),         GED_NONE };
    static const struct parsed_cart_type mbc5_ram             = { MBC(mbc5),         GED_RAM  };
    static const struct parsed_cart_type mbc5_ram_batt        = { MBC(mbc5),         GED_RAM | GED_BATTERY };
    static const struct parsed_cart_type mbc5_rumble          = { MBC(mbc5),         GED_RUMBLE };
    static const struct parsed_cart_type mbc5_ram_rumble      = { MBC(mbc5),         GED_RAM | GED_RUMBLE };
    static const struct parsed_cart_type mbc5_ram_batt_rumble = { MBC(mbc5),         GED_RAM | GED_BATTERY | GED_RUMBLE };

    static const struct parsed_cart_type mbc6                 = { MBC(mbc6),         GED_RAM | GED_BATTERY };

    static const struct parsed_cart_type mbc7                 = { MBC(mbc7),         GED_RAM | GED_BATTERY | GED_ACCELEROMETER };

    static const struct parsed_cart_type pocket_cam           = { MBC(pocket_cam),   GED_RAM | GED_CAMERA };

    static const struct parsed_cart_type bandai_tama5         = { MBC(bandai_tama5), GED_NONE };

    static const struct parsed_cart_type huc3                 = { MBC(huc3),         GED_NONE };

    static const struct parsed_cart_type huc1                 = { MBC(huc1),         GED_RAM | GED_BATTERY };
#undef MBC

    switch(cart_type)
    {
    case 0x00: return &nombc_none;
    case 0x01: return &mbc1_none;
    case 0x02: return &mbc1_ram;
    case 0x03: return &mbc1_ram_batt;
    /* 0x04 is unused */
    case 0x05: return &mbc2_none;
    case 0x06: return &mbc2_ram_batt;
    /* 0x07 is unused */
    case 0x08: return &nombc_ram;
    case 0x09: return &nombc_ram_batt;
    /* 0x0a is unused */
    case 0x0b: return &mmm01_none;
    case 0x0c: return &mmm01_ram;
    case 0x0d: return &mmm01_ram_batt;
    /* 0x0e is unused */
    case 0x0f: return &mbc3_batt_rtc;
    case 0x10: return &mbc3_ram_batt_rtc;
    case 0x11: return &mbc3_none;
    case 0x12: return &mbc3_ram;
    case 0x13: return &mbc3_ram_batt;
    /* 0x14-0x18 are unused */
    case 0x19: return &mbc5_none;
    case 0x1a: return &mbc5_ram;
    case 0x1b: return &mbc5_ram_batt;
    case 0x1c: return &mbc5_rumble;
    case 0x1d: return &mbc5_ram_rumble;
    case 0x1e: return &mbc5_ram_batt_rumble;
    /* 0x1f is unused */
    case 0x20: return &mbc6;
    /* 0x21 is unused */
    case 0x22: return &mbc7;
    /* 0x23-0xfb are unused */
    case 0xfc: return &pocket_cam;
    case 0xfd: return &bandai_tama5;
    case 0xfe: return &huc3;
    case 0xff: return &huc1;
    default:   return NULL;
    }
}

void init_gb_cart(struct gb_cart* gb_cart,
        void* rom_opaque, void (*init_rom)(void* user_data, void** rom_storage, const struct storage_backend_interface** irom_storage), void (*release_rom)(void* user_data),
        void* ram_opaque, void (*init_ram)(void* user_data, size_t ram_size, void** ram_storage, const struct storage_backend_interface** iram_storage), void (*release_ram)(void* user_data),
        void* clock, const struct clock_backend_interface* iclock,
        void* rumble, const struct rumble_backend_interface* irumble,
        void* vcap, const struct video_capture_backend_interface* ivcap)
{
    const struct parsed_cart_type* type;
    void* rom_storage = NULL;
    const struct storage_backend_interface* irom_storage = NULL;
    void* ram_storage = NULL;
    const struct storage_backend_interface* iram_storage = NULL;
    struct mbc3_rtc rtc;
    struct pocket_cam cam;

    memset(&rtc, 0, sizeof(rtc));
    memset(&cam, 0, sizeof(cam));

    /* ask to load rom and initialize rom storage backend */
    init_rom(rom_opaque, &rom_storage, &irom_storage);

    /* handle no cart case */
    if (irom_storage == NULL) {
        goto no_cart;
    }

    /* check rom size */
    const uint8_t* rom_data = irom_storage->data(rom_storage);
    if (rom_data == NULL || irom_storage->size(rom_storage) < 0x8000)
    {
        core_log(CORE_LOG_ERROR, "Invalid GB ROM file size (< 32k)");
        goto error_release_rom;
    }

    /* get and parse cart type */
    uint8_t cart_type = rom_data[0x147];
    type = parse_cart_type(cart_type);
    if (type == NULL)
    {
        core_log(CORE_LOG_ERROR, "Invalid GB cart type (%02x)", cart_type);
        goto error_release_rom;
    }

    core_log(CORE_LOG_INFO, "GB cart type (%02x) %s%s%s%s%s%s%s",
            cart_type,
            type->mbc,
            (type->extra_devices & GED_RAM)            ? " RAM" : "",
            (type->extra_devices & GED_BATTERY)        ? " BATT" : "",
            (type->extra_devices & GED_RTC)            ? " RTC" : "",
            (type->extra_devices & GED_RUMBLE)         ? " RUMBLE" : "",
            (type->extra_devices & GED_ACCELEROMETER)  ? " ACCEL" : "",
            (type->extra_devices & GED_CAMERA)         ? " CAM" : "");

    /* load ram (if present) */
    if (type->extra_devices & GED_RAM)
    {
        size_t ram_size = 0;
        switch(rom_data[0x149])
        {
        case 0x00: ram_size = (strcmp(type->mbc, "mbc2") == 0)
                            ? 0x200 /* MBC2 have an integrated 512x4bit RAM */
                            : 0;
                   break;
        case 0x01: ram_size =  1*0x800; break;
        case 0x02: ram_size =  4*0x800; break;
        case 0x03: ram_size = 16*0x800; break;
        case 0x04: ram_size = 64*0x800; break;
        case 0x05: ram_size = 32*0x800; break;
        }

        if (ram_size != 0)
        {
            init_ram(ram_opaque, ram_size, &ram_storage, &iram_storage);

            /* check that init_ram succeeded */
            if (iram_storage == NULL) {
                core_log(CORE_LOG_ERROR, "Failed to initialize GB RAM");
                goto error_release_ram;
            }

            if (iram_storage->data(ram_storage) == NULL || iram_storage->size(ram_storage) != ram_size)
            {
                core_log(CORE_LOG_ERROR, "Cannot get GB RAM (%d bytes)", (uint32_t) ram_size);
                goto error_release_ram;
            }

            core_log(CORE_LOG_INFO, "Using a %d bytes GB RAM", (uint32_t) ram_size);
        }
    }

    /* set RTC clock (if present) */
    if (type->extra_devices & GED_RTC) {
        init_mbc3_rtc(&rtc, clock, iclock);
    }

    if (type->extra_devices & GED_CAMERA) {
        init_pocket_cam(&cam, iram_storage->data(ram_storage), vcap, ivcap);
    }

    /* update gb_cart */
    gb_cart->rom_storage = rom_storage;
    gb_cart->irom_storage = irom_storage;
    gb_cart->ram_storage = ram_storage;
    gb_cart->iram_storage = iram_storage;
    gb_cart->extra_devices = type->extra_devices;
    gb_cart->rtc = rtc;
    gb_cart->cam = cam;
    gb_cart->rumble = rumble;
    gb_cart->irumble = irumble;
    gb_cart->read_gb_cart = type->read_gb_cart;
    gb_cart->write_gb_cart = type->write_gb_cart;

    return;

error_release_ram:
    release_ram(ram_opaque);
error_release_rom:
    release_rom(rom_opaque);
no_cart:
    memset(gb_cart, 0, sizeof(*gb_cart));
}

void poweron_gb_cart(struct gb_cart* gb_cart)
{
    gb_cart->rom_bank = 1;
    gb_cart->ram_bank = 0;
    gb_cart->ram_enable = 0;
    gb_cart->mbc1_mode = 0;

    if (gb_cart->extra_devices & GED_RTC) {
        poweron_mbc3_rtc(&gb_cart->rtc);
    }

    if (gb_cart->extra_devices & GED_CAMERA) {
        poweron_pocket_cam(&gb_cart->cam);
    }

    if (gb_cart->extra_devices & GED_RUMBLE) {
        gb_cart->irumble->exec(gb_cart->rumble, RUMBLE_STOP);
    }
}

int read_gb_cart(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size)
{
    return gb_cart->read_gb_cart(gb_cart, address, data, size);
}

int write_gb_cart(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size)
{
    return gb_cart->write_gb_cart(gb_cart, address, data, size);
}

/* ---- gb/mbc3_rtc.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mbc3_rtc.c                                              *
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

/* TODO: halt bit is not implemented */

static void update_rtc_regs(struct mbc3_rtc* rtc)
{
    /* compute elapsed time since last update */
    time_t now = rtc->iclock->get_time(rtc->clock);
    time_t diff = now - rtc->last_time;
    rtc->last_time = now;

    /* increment regs */
    if (diff > 0) {
        rtc->regs[MBC3_RTC_SECONDS] += (int)(diff % 60);
        if (rtc->regs[MBC3_RTC_SECONDS] >= 60) {
            rtc->regs[MBC3_RTC_SECONDS] -= 60;
            ++rtc->regs[MBC3_RTC_MINUTES];
        }
        diff /= 60;

        rtc->regs[MBC3_RTC_MINUTES] += (int)(diff % 60);
        if (rtc->regs[MBC3_RTC_MINUTES] >= 60) {
            rtc->regs[MBC3_RTC_MINUTES] -= 60;
            ++rtc->regs[MBC3_RTC_HOURS];
        }
        diff /= 60;

        rtc->regs[MBC3_RTC_HOURS] += (int)(diff % 24);
        if (rtc->regs[MBC3_RTC_HOURS] >= 24) {
            rtc->regs[MBC3_RTC_HOURS] -= 24;
            ++rtc->regs[MBC3_RTC_DAYS_L];
        }
        diff /= 24;

        /* update days counter */
        unsigned int days = (((rtc->regs[MBC3_RTC_DAYS_H] & 0x01) << 8) | rtc->regs[MBC3_RTC_DAYS_L])
            + (int)diff;

        rtc->regs[MBC3_RTC_DAYS_L] = days & 0xff;
        rtc->regs[MBC3_RTC_DAYS_H] = (rtc->regs[MBC3_RTC_DAYS_H] & ~0x01) | (days & 0x100);

        /* set carry bit if days overflow */
        if (days > 511) { rtc->regs[MBC3_RTC_DAYS_H] |= 0x80; }
    }
}

void init_mbc3_rtc(struct mbc3_rtc* rtc,
                   void* clock,
                   const struct clock_backend_interface* iclock)
{
    rtc->clock = clock;
    rtc->iclock = iclock;
}

void poweron_mbc3_rtc(struct mbc3_rtc* rtc)
{
    memset(rtc->regs, 0, MBC3_RTC_REGS_COUNT);
    memset(rtc->latched_regs, 0, MBC3_RTC_REGS_COUNT);
    rtc->latch = 0;
    rtc->last_time = 0;
}

uint8_t read_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg)
{
    if (rtc->latch) {
        return rtc->latched_regs[reg];
    }

    update_rtc_regs(rtc);
    return rtc->regs[reg];
}

void write_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg, uint8_t value)
{
    rtc->regs[reg] = value;
}

void latch_mbc3_rtc_regs(struct mbc3_rtc* rtc, uint8_t data)
{
    if (rtc->latch == 0 && data == 1) {
        update_rtc_regs(rtc);
        memcpy(rtc->latched_regs, rtc->regs, MBC3_RTC_REGS_COUNT);
    }

    rtc->latch = data & 0x1;
}

/* ---- gb/m64282fp.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - m64282fp.c                                              *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2017 Bobby Smiles                                       *
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

/* Most of the mappers information comes from
 * "Game Boy Camera Technical Information v1.1.1" by AntonioND
 */

int min(int a, int b) { return (a < b) ? a : b; }
int max(int a, int b) { return (a < b) ? b : a; }

int clamp(int v, int low, int high)
{
    return min(high, max(low, v));
}

static void do_kernel_filtering(int img[M64282FP_SENSOR_H][M64282FP_SENSOR_W], int a, const int k[6])
{
    unsigned int x, y;
    /* north line and last west pixel previous values */
    int tmp[1+M64282FP_SENSOR_W];

    memcpy(tmp, &img[0][0], M64282FP_SENSOR_W*sizeof(img[0][0]));

    for (y = 0; y < M64282FP_SENSOR_H; ++y) {

        tmp[M64282FP_SENSOR_W] = img[y][0];

        for (x = 0; x < M64282FP_SENSOR_W; ++x) {

            int px = img[y][x];
            int ms = img[min(y+1, M64282FP_SENSOR_H-1)][x];
            int me = img[y][min(x+1, M64282FP_SENSOR_W-1)];
            int mn = tmp[x];
            int mw = tmp[M64282FP_SENSOR_W];

            img[y][x] = k[0]*px+((a*(k[1]*px+k[2]*mn+k[3]*mw+k[4]*me+k[5]*ms))/4);

            tmp[x] = px;
            tmp[M64282FP_SENSOR_W] = px;
        }
    }
}

static void do_1d_filtering(int img[M64282FP_SENSOR_H][M64282FP_SENSOR_W], uint8_t P, uint8_t M)
{
    unsigned int x, y;

    for (y = 0; y < M64282FP_SENSOR_H; ++y) {
        for (x = 0; x < M64282FP_SENSOR_W; ++x) {

            int px = img[y][x];
            int s1 = img[min(y+1, M64282FP_SENSOR_H-1)][x];
            int s2 = img[min(y+2, M64282FP_SENSOR_H-1)][x];
            int s3 = img[min(y+3, M64282FP_SENSOR_H-1)][x];

            int value = 0;
            if (P & 0x01) { value += px; }
            if (P & 0x02) { value += s1; }
            if (P & 0x04) { value += s2; }
            if (P & 0x08) { value += s3; }

            if (M & 0x01) { value -= px; }
            if (M & 0x02) { value -= s1; }
            if (M & 0x04) { value -= s2; }
            if (M & 0x08) { value -= s3; }

            img[y][x] = value;
        }
    }
}

void process_m64282fp_image(
    uint8_t img[M64282FP_SENSOR_H][M64282FP_SENSOR_W],
    const uint8_t regs[M64282FP_REGS_COUNT])
{
    unsigned int x, y;
    int tmp[M64282FP_SENSOR_H][M64282FP_SENSOR_W];

    static const int kernels[6][6] = {
        /* px +a*(px +mn +mw +me +ms) */
        {   1,     2,  0, -1, -1,  0 }, /* horiz enhancement */
        {   0,     2,  0, -1, -1,  0 }, /* horiz extraction  */
        {   1,     2, -1,  0,  0, -1 }, /* vert  enhancement */
        {   0,     2, -1,  0,  0, -1 }, /* vert  extraction  */
        {   1,     4, -1, -1, -1, -1 }, /* 2d    enhancement */
        {   0,     4, -1, -1, -1, -1 }, /* 2d    extraction  */
    };

    /* edge ratio (alpha) Q.2 format */
#define Q2(x) (4*x)
    static const int alpha_lut[8] = {
        Q2(2/4), Q2(3/4), Q2(1), Q2(5/4),
        Q2(2), Q2(3), Q2(4), Q2(5)
    };
#undef Q2

    /* apply exposure effect */
    uint16_t ext_exposure = 0x0300; /* 0x0300: could be other value */
    uint16_t exposure = (regs[M64282FP_C_HI] << 8) | regs[M64282FP_C_LO];

    /* TODO: handle the zero-exposure case */

    for (y = 0; y < M64282FP_SENSOR_H; ++y) {
        for (x = 0; x < M64282FP_SENSOR_W; ++x) {
            int v = img[y][x];
            v = ((v * exposure) / ext_exposure);
            v = ((v - 128) / 8) + 128; /* adapt to 3.1V / 5V */
            img[y][x] = clamp(v, 0, 255);
        }
    }

    /* invert image when I bit is set */
    if (regs[M64282FP_E_I_V] & 0x8) {
        for (y = 0; y < M64282FP_SENSOR_H; ++y) {
            for (x = 0; x < M64282FP_SENSOR_W; ++x) {
                img[y][x] = 255 - img[y][x];
            }
        }
    }

    /* make signed */
    for (y = 0; y < M64282FP_SENSOR_H; ++y) {
        for (x = 0; x < M64282FP_SENSOR_W; ++x) {
            tmp[y][x] = img[y][x] - 128;
        }
    }

    unsigned int mode
        = (((regs[M64282FP_N_VH_G] & 0x80) >> 7) << 3)  /* N  */
        | (((regs[M64282FP_N_VH_G] & 0x60) >> 5) << 1)  /* VH */
        | (((regs[M64282FP_E_I_V]  & 0x80) >> 7) << 0); /* E3 */

    int alpha = alpha_lut[(regs[M64282FP_E_I_V] & 0x70) >> 4];

    switch(mode)
    {
    case 0x0: /* 0000: positive image */
        do_1d_filtering(tmp, regs[M64282FP_P], regs[M64282FP_M]);
        break;
    case 0x1: /* 0001: undocumented - bug ??? */
        memset(tmp, 0, sizeof(tmp));
        break;

    case 0x2: /* 0010: horiz enhancement */
        do_kernel_filtering(tmp, alpha, kernels[0]);
        do_1d_filtering(tmp, regs[M64282FP_P], regs[M64282FP_M]);
        break;
    case 0x3: /* 0011: horiz extraction */
        do_kernel_filtering(tmp, alpha, kernels[1]);
        do_1d_filtering(tmp, regs[M64282FP_P], regs[M64282FP_M]);
        break;

    case 0xc: /* 1100: vert enhancement */
        do_kernel_filtering(tmp, alpha, kernels[2]);
        break;
    case 0xd: /* 1101: vert extraction */
        do_kernel_filtering(tmp, alpha, kernels[3]);
        break;

    case 0xe: /* 1110: 2D enhancement */
        do_kernel_filtering(tmp, alpha, kernels[4]);
        break;
    case 0xf: /* 1111: 2D extraction */
        do_kernel_filtering(tmp, alpha, kernels[5]);
        break;

    default:
        break;
    }

    /* back to 0..255 range */
    for (y = 0; y < M64282FP_SENSOR_H; ++y) {
        for (x = 0; x < M64282FP_SENSOR_W; ++x) {
            img[y][x] = clamp(128 + tmp[y][x], 0, 255);
        }
    }

    /* gain and level control are not emulated */
}
