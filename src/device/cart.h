#ifndef PARALLAX_DEVICE_CART_H
#define PARALLAX_DEVICE_CART_H

#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include "osal/preproc.h"
#include "core/joybus.h"

#ifdef __cplusplus
extern "C" {
#endif

struct clock_backend_interface;
struct r4300_core;
struct pi_controller;
struct storage_backend_interface;

/* ---- af_rtc ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - af_rtc.h                                                *
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

struct af_rtc
{
    /* block 0 */
    uint16_t control;
    /* block 2 */
    time_t now;

    time_t last_update_rtc;
    void* clock;
    const struct clock_backend_interface* iclock;
};

void init_af_rtc(struct af_rtc* rtc,
                 void* clock,
                 const struct clock_backend_interface* iclock);

void poweron_af_rtc(struct af_rtc* rtc);

void af_rtc_read_block(struct af_rtc* rtc,
    uint8_t block, uint8_t* data, uint8_t* status);

void af_rtc_write_block(struct af_rtc* rtc,
    uint8_t block, const uint8_t* data, uint8_t* status);

/* ---- cart_rom ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cart_rom.h                                              *
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

struct cart_rom
{
    uint8_t* rom;
    size_t rom_size;

    uint32_t last_write;

    struct r4300_core* r4300;
    struct pi_controller* pi;
};

static osal_inline uint32_t rom_address(uint32_t address)
{
    return (address & 0x03fffffc);
}

void init_cart_rom(struct cart_rom* cart_rom,
                   uint8_t* rom, size_t rom_size,
                   struct r4300_core* r4300,
                   struct pi_controller* pi);

void poweron_cart_rom(struct cart_rom* cart_rom);

void read_cart_rom(void* opaque, uint32_t address, uint32_t* value);
void write_cart_rom(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

unsigned int cart_rom_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
unsigned int cart_rom_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);

/* ---- eeprom ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - eeprom.h                                                *
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

struct eeprom
{
    uint16_t type;
    void* storage;
    const struct storage_backend_interface* istorage;
};

void format_eeprom(uint8_t* eeprom, size_t size);

void init_eeprom(struct eeprom* eeprom,
    uint16_t type,
    void* storage,
    const struct storage_backend_interface* istorage);

void eeprom_read_block(struct eeprom* eeprom,
    uint8_t block, uint8_t* data);

void eeprom_write_block(struct eeprom* eeprom,
    uint8_t block, const uint8_t* data, uint8_t* status);

/* ---- flashram ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - flashram.h                                              *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

enum { FLASHRAM_SIZE = 0x20000 };

enum { FLASHRAM_TYPE_ID = 0x11118001 };

/* flashram manufacturer and device code */
enum {
    MX29L0000_ID = 0x00c20000,
    MX29L0001_ID = 0x00c20001,
    MX29L1100_ID = 0x00c2001e,
    MX29L1101_ID = 0x00c2001d,
    MN63F8MPN_ID = 0x003200f1,
};

enum flashram_mode
{
    FLASHRAM_MODE_READ_ARRAY,
    FLASHRAM_MODE_READ_SILICON_ID,
    FLASHRAM_MODE_STATUS,
    FLASHRAM_MODE_SECTOR_ERASE,
    FLASHRAM_MODE_CHIP_ERASE,
    FLASHRAM_MODE_PAGE_PROGRAM
};

struct flashram
{
    uint8_t page_buf[128];
    uint32_t silicon_id[2];
    uint32_t status; // supposedly only 8-bit
    uint16_t erase_page;
    enum flashram_mode mode;

    void* storage;
    const struct storage_backend_interface* istorage;
};

void init_flashram(struct flashram* flashram,
                   uint32_t flashram_id,
                   void* storage,
                   const struct storage_backend_interface* istorage);

void poweron_flashram(struct flashram* flashram);

void format_flashram(uint8_t* flash);

void read_flashram(void* opaque, uint32_t address, uint32_t* value);
void write_flashram(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

unsigned int flashram_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
unsigned int flashram_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);

/* ---- is_viewer ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - is_viewer.h                                             *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2021 loganmc10                                          *
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

#define IS_BUFFER_SIZE 0x1000

struct is_viewer
{
    char data[IS_BUFFER_SIZE];
    char output_buffer[IS_BUFFER_SIZE];
    uint32_t buffer_pos;
};

void poweron_is_viewer(struct is_viewer* is_viewer);

void read_is_viewer(void* opaque, uint32_t address, uint32_t* value);
void write_is_viewer(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

/* ---- sram ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - sram.h                                                  *
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

enum { SRAM_SIZE = 0x8000 };

struct sram
{
    void* storage;
    const struct storage_backend_interface* istorage;
};

void format_sram(uint8_t* sram);

void init_sram(struct sram* sram,
               void* storage, const struct storage_backend_interface* istorage);

unsigned int sram_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
unsigned int sram_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
void read_sram(void* opaque, uint32_t address, uint32_t* value);
void write_sram(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

/* ---- cart ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cart.h                                                  *
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

struct cart
{
    struct af_rtc af_rtc;
    struct cart_rom cart_rom;
    struct eeprom eeprom;
    struct flashram flashram;
    struct sram sram;

    int use_flashram;
};

void init_cart(struct cart* cart,
               /* AF-RTC */
               void* af_rtc_clock, const struct clock_backend_interface* iaf_rtc_clock,
               /* cart ROM */
               uint8_t* rom, size_t rom_size,
               struct r4300_core* r4300,
               struct pi_controller* pi,
               /* eeprom */
               uint16_t eeprom_type,
               void* eeprom_storage, const struct storage_backend_interface* ieeprom_storage,
               /* flashram */
               uint32_t flashram_type,
               void* flashram_storage, const struct storage_backend_interface* iflashram_storage,
               const uint8_t* dram,
               /* sram */
               void* sram_storage, const struct storage_backend_interface* isram_storage);

void poweron_cart(struct cart* cart);

void read_cart_dom2(void* opaque, uint32_t address, uint32_t* value);
void write_cart_dom2(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

unsigned int cart_dom2_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
unsigned int cart_dom2_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
unsigned int cart_dom3_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);
unsigned int cart_dom3_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length);

extern const struct joybus_device_interface
    g_ijoybus_device_cart;

#ifdef __cplusplus
}
#endif

#endif
