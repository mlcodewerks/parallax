#ifndef PARALLAX_DEVICE_GB_H
#define PARALLAX_DEVICE_GB_H

#include <stdint.h>
#include <time.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct clock_backend_interface;
struct storage_backend_interface;
struct rumble_backend_interface;
struct video_capture_backend_interface;

/* ---- m64282fp ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - m64282fp.h                                              *
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

enum
{
    M64282FP_SENSOR_W = 128,
    M64282FP_SENSOR_H = 128,
};

enum m64282fp_registers
{
    M64282FP_Z_O,
    M64282FP_N_VH_G,
    M64282FP_C_LO,
    M64282FP_C_HI,
    M64282FP_P,
    M64282FP_M,
    M64282FP_X,
    M64282FP_E_I_V,
    M64282FP_REGS_COUNT
};

void process_m64282fp_image(
    uint8_t img[M64282FP_SENSOR_H][M64282FP_SENSOR_W],
    const uint8_t regs[M64282FP_REGS_COUNT]);

/* ---- mbc3_rtc ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mbc3_rtc.h                                              *
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

enum mbc3_rtc_registers
{
    MBC3_RTC_SECONDS,   /* 0-59 */
    MBC3_RTC_MINUTES,   /* 0-59 */
    MBC3_RTC_HOURS,     /* 0-23 */
    MBC3_RTC_DAYS_L,    /* Days Counter is 16 bit wide */
    MBC3_RTC_DAYS_H,    /* bits 0-8: days counter (0-511) */
                        /* bits 9-13: zeros ? */
                        /* bits 14: halt (0: stop timer, 1: active timer) */
                        /* bits 15: carry (0: no overflow, 1: couter overflowed) */
    MBC3_RTC_REGS_COUNT
};

struct mbc3_rtc
{
    uint8_t regs[MBC3_RTC_REGS_COUNT];

    unsigned int latch;
    uint8_t latched_regs[MBC3_RTC_REGS_COUNT];

    time_t last_time;

    void* clock;
    const struct clock_backend_interface* iclock;
};

void init_mbc3_rtc(struct mbc3_rtc* rtc,
                   void* clock,
                   const struct clock_backend_interface* iclock);

void poweron_mbc3_rtc(struct mbc3_rtc* rtc);

uint8_t read_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg);
void write_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg, uint8_t value);
void latch_mbc3_rtc_regs(struct mbc3_rtc* rtc, uint8_t data);

/* ---- gb_cart ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - gb_cart.h                                               *
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

enum pocket_cam_registers
{
    /* TODO: use pretty names for registers */
    POCKET_CAM_REGS_COUNT = 0x36
};

struct pocket_cam
{
    uint8_t regs[POCKET_CAM_REGS_COUNT];

    void* vcap;
    const struct video_capture_backend_interface* ivcap;

    uint8_t* ram;
};

struct gb_cart
{
    void* rom_storage;
    const struct storage_backend_interface* irom_storage;

    void* ram_storage;
    const struct storage_backend_interface* iram_storage;

    unsigned int rom_bank;
    unsigned int ram_bank;

    unsigned int ram_enable;
    unsigned int mbc1_mode;

    unsigned int extra_devices;

    struct mbc3_rtc rtc;
    struct pocket_cam cam;

    void* rumble;
    const struct rumble_backend_interface* irumble;

    int (*read_gb_cart)(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size);
    int (*write_gb_cart)(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size);
};

void init_gb_cart(struct gb_cart* gb_cart,
        void* rom_opaque, void (*init_rom)(void* user_data, void** rom_storage, const struct storage_backend_interface** irom_storage), void (*release_rom)(void* user_data),
        void* ram_opaque, void (*init_ram)(void* user_data, size_t ram_size, void** ram_storage, const struct storage_backend_interface** iram_storage), void (*release_ram)(void* user_data),
        void* clock, const struct clock_backend_interface* iclock,
        void* rumble, const struct rumble_backend_interface* irumble,
        void* vcap, const struct video_capture_backend_interface* ivcap);

void poweron_gb_cart(struct gb_cart* gb_cart);

int read_gb_cart(struct gb_cart* gb_cart, uint16_t address, uint8_t* data, size_t size);
int write_gb_cart(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
