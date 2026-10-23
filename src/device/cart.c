/* Consolidated cart device implementation.
 * Component copyright and license notices are retained below.
 */

#define __STDC_FORMAT_MACROS

#include "device/cart.h"
#include "core/log.h"
#include "core/types.h"
#include "main/rom.h"
#include <stdint.h>
#include <string.h>
#include "core/clock.h"
#include <time.h>
#include "device/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/rcp.h"
#include <inttypes.h>
#include "core/storage.h"
#include "main/util.h"
#include <stddef.h>

/* ---- cart/cart.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cart.c                                                  *
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

static void process_cart_command(void* jbd,
    const uint8_t* tx, const uint8_t* tx_buf,
    uint8_t* rx, uint8_t* rx_buf)
{
    struct cart* cart = (struct cart*)jbd;

    uint8_t cmd = tx_buf[0];

    switch (cmd)
    {
    case JCMD_RESET:
        /* TODO: perform internal reset */
        /* fall through */
    case JCMD_STATUS: {
        JOYBUS_CHECK_COMMAND_FORMAT(1, 3)

        if (cart->eeprom.type) {
            /* set type, status, and extra */
            rx_buf[0] = (uint8_t)(cart->eeprom.type >> 0);
            rx_buf[1] = (uint8_t)(cart->eeprom.type >> 8);
            rx_buf[2] = 0x00;
        }
    } break;

    case JCMD_EEPROM_READ: {
        JOYBUS_CHECK_COMMAND_FORMAT(2, 8)
        eeprom_read_block(&cart->eeprom, tx_buf[1], &rx_buf[0]);
    } break;

    case JCMD_EEPROM_WRITE: {
        JOYBUS_CHECK_COMMAND_FORMAT(10, 1)
        eeprom_write_block(&cart->eeprom, tx_buf[1], &tx_buf[2], &rx_buf[0]);
    } break;

    case JCMD_AF_RTC_STATUS: {
        JOYBUS_CHECK_COMMAND_FORMAT(1, 3)

        /* set type and status */
        rx_buf[0] = (uint8_t)(JDT_AF_RTC >> 0);
        rx_buf[1] = (uint8_t)(JDT_AF_RTC >> 8);
        rx_buf[2] = 0x00;
    } break;

    case JCMD_AF_RTC_READ: {
        JOYBUS_CHECK_COMMAND_FORMAT(2, 9)
        af_rtc_read_block(&cart->af_rtc, tx_buf[1], &rx_buf[0], &rx_buf[8]);
    } break;

    case JCMD_AF_RTC_WRITE: {
        JOYBUS_CHECK_COMMAND_FORMAT(10, 1)
        af_rtc_write_block(&cart->af_rtc, tx_buf[1], &tx_buf[2], &rx_buf[0]);
    } break;

    default:
        core_log(CORE_LOG_WARNING, "cart: Unknown command %02x %02x %02x",
            *tx, *rx, cmd);
    }
}

const struct joybus_device_interface
    g_ijoybus_device_cart =
{
    NULL,
    process_cart_command,
    NULL
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
               void* sram_storage, const struct storage_backend_interface* isram_storage)
{
    init_af_rtc(&cart->af_rtc,
        af_rtc_clock, iaf_rtc_clock);

    init_cart_rom(&cart->cart_rom,
        rom, rom_size,
        r4300,
        pi);

    init_eeprom(&cart->eeprom,
        eeprom_type, eeprom_storage, ieeprom_storage);

    init_flashram(&cart->flashram,
        flashram_type,
        flashram_storage, iflashram_storage);

    init_sram(&cart->sram,
        sram_storage, isram_storage);

    if (ROM_SETTINGS.savetype == SAVETYPE_SRAM)
        cart->use_flashram = -1;
    else if (ROM_SETTINGS.savetype == SAVETYPE_FLASH_RAM)
        cart->use_flashram = 1;
    else
        cart->use_flashram = 0;
}

void poweron_cart(struct cart* cart)
{
    poweron_af_rtc(&cart->af_rtc);
    poweron_cart_rom(&cart->cart_rom);
    poweron_flashram(&cart->flashram);
}

void read_cart_dom2(void* opaque, uint32_t address, uint32_t* value)
{
    struct cart* cart = (struct cart*)opaque;

    if (cart->use_flashram == -1)
    {
        read_sram(&cart->sram, address, value);
    }
    else
    {
        if ((address & 0xffff) != 0)
        {
            core_log(CORE_LOG_ERROR, "unknown read in read_cart_dom2()");
            return;
        }

        cart->use_flashram = 1;
        read_flashram(&cart->flashram, address, value);
    }
}

void write_cart_dom2(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct cart* cart = (struct cart*)opaque;

    if (cart->use_flashram == -1)
    {
        write_sram(&cart->sram, address, value, mask);
    }
    else
    {
        if ((address & 0xffff) != 0)
        {
            core_log(CORE_LOG_ERROR, "unknown write in write_cart_dom2()");
            return;
        }

        cart->use_flashram = 1;
        write_flashram(&cart->flashram, address, value, mask);
    }
}

unsigned int cart_dom2_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct cart* cart = (struct cart*)opaque;
    unsigned int cycles;

    if (cart->use_flashram != 1)
    {
        cycles = sram_dma_read(&cart->sram, dram, dram_addr, cart_addr, length);
        cart->use_flashram = -1;
    }
    else
    {
        cycles = flashram_dma_read(&cart->flashram, dram, dram_addr, cart_addr, length);
    }

    return cycles;
}

unsigned int cart_dom2_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct cart* cart = (struct cart*)opaque;
    unsigned int cycles;

    if (cart->use_flashram != 1)
    {
        cycles = sram_dma_write(&cart->sram, dram, dram_addr, cart_addr, length);
        cart->use_flashram = -1;
    }
    else
    {
        cycles = flashram_dma_write(&cart->flashram, dram, dram_addr, cart_addr, length);
    }

    return cycles;
}

unsigned int cart_dom3_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct cart* cart = (struct cart*)opaque;
    return cart_rom_dma_read(&cart->cart_rom, dram, dram_addr, cart_addr, length);
}

unsigned int cart_dom3_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct cart* cart = (struct cart*)opaque;
    return cart_rom_dma_write(&cart->cart_rom, dram, dram_addr, cart_addr, length);
}

/* ---- cart/af_rtc.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - af_rtc.c                                                *
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

static uint8_t byte2bcd(int n)
{
    n %= 100;
    return (uint8_t)(((n / 10) << 4) | (n % 10));
}

static void time2data(uint8_t* data, time_t now)
{
    const struct tm* tm = localtime(&now);

    data[0] = byte2bcd(tm->tm_sec);
    data[1] = byte2bcd(tm->tm_min);
    data[2] = 0x80 + byte2bcd(tm->tm_hour);
    data[3] = byte2bcd(tm->tm_mday);
    data[4] = byte2bcd(tm->tm_wday);
    data[5] = byte2bcd(tm->tm_mon + 1);
    data[6] = byte2bcd(tm->tm_year);
    data[7] = byte2bcd(tm->tm_year / 100);
}

static void update_rtc(struct af_rtc* rtc)
{
    /* update rtc->now */
    time_t now = rtc->iclock->get_time(rtc->clock);
    rtc->now += now - rtc->last_update_rtc;
    rtc->last_update_rtc = now;
}

void init_af_rtc(struct af_rtc* rtc,
                 void* clock,
                 const struct clock_backend_interface* iclock)
{
    rtc->clock = clock;
    rtc->iclock = iclock;
}

void poweron_af_rtc(struct af_rtc* rtc)
{
    /* blocks 1&2 are read/write, timer is activated */
    rtc->control = 0x0200;

    rtc->now = 0;
    rtc->last_update_rtc = 0;
}

void af_rtc_read_block(struct af_rtc* rtc,
    uint8_t block, uint8_t* data, uint8_t* status)
{
    switch (block)
    {
    case 0:
        data[0] = (uint8_t)(rtc->control >> 0);
        data[1] = (uint8_t)(rtc->control >> 8);
        *status = 0x00;
        break;
    case 1:
        core_log(CORE_LOG_ERROR, "AF-RTC reading block 1 is not implemented !");
        break;
    case 2:
        update_rtc(rtc);
        time2data(data, rtc->now);
        *status = 0x00;
        break;

    default:
        core_log(CORE_LOG_ERROR, "AF-RTC read invalid block: %u", block);
    }
}

void af_rtc_write_block(struct af_rtc* rtc,
    uint8_t block, const uint8_t* data, uint8_t* status)
{
    switch (block)
    {
    case 0:
        rtc->control = (data[1] << 8) | data[0];
        *status = 0x00;
        break;
    case 1:
        /* block 1 read-only when control[0] is set */
        if (rtc->control & 0x01) {
            break;
        }
        core_log(CORE_LOG_ERROR, "AF-RTC writing block 1 is not implemented !");
        break;
    case 2:
        /* block 2 read-only when control[1] is set */
        if (rtc->control & 0x02) {
            break;
        }

        /* TODO: implement block 2 writes */
        core_log(CORE_LOG_ERROR, "AF-RTC writing block 2 is not implemented !");
        break;

    default:
        core_log(CORE_LOG_ERROR, "AF-RTC write invalid block: %u", block);
    }
}

/* ---- cart/cart_rom.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cart_rom.c                                              *
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

#define CART_ROM_ADDR_MASK UINT32_C(0x03ffffff);

void init_cart_rom(struct cart_rom* cart_rom,
                   uint8_t* rom, size_t rom_size,
                   struct r4300_core* r4300,
                   struct pi_controller* pi)
{
    cart_rom->rom = rom;
    cart_rom->rom_size = rom_size;

    cart_rom->r4300 = r4300;
    cart_rom->pi = pi;
}

void poweron_cart_rom(struct cart_rom* cart_rom)
{
    cart_rom->last_write = 0;
}

void read_cart_rom(void* opaque, uint32_t address, uint32_t* value)
{
    struct cart_rom* cart_rom = (struct cart_rom*)opaque;
    uint32_t addr = rom_address(address);

    if (cart_rom->pi->regs[PI_STATUS_REG] & PI_STATUS_IO_BUSY)
    {
        *value = cart_rom->last_write;
    }
    else
    {
        *value = *(uint32_t*)(cart_rom->rom + addr);
    }
}

void write_cart_rom(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct cart_rom* cart_rom = (struct cart_rom*)opaque;
    if (!validate_pi_request(cart_rom->pi))
        return;
    cart_rom->last_write = value & mask;

    /* Mark IO as busy */
    cart_rom->pi->regs[PI_STATUS_REG] |= PI_STATUS_IO_BUSY;
    cp0_update_count(cart_rom->r4300);
    add_interrupt_event(&cart_rom->r4300->cp0, PI_INT, 100);
}

unsigned int cart_rom_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    cart_addr &= CART_ROM_ADDR_MASK;

    core_log(CORE_LOG_WARNING, "DMA Writing to CART_ROM: 0x%" PRIX32 " -> 0x%" PRIX32 " (0x%" PRIX32 ")", dram_addr, cart_addr, length);

    return /* length / 8 */0x1000;
}

unsigned int cart_rom_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    size_t i;
    struct cart_rom* cart_rom = (struct cart_rom*)opaque;
    const uint8_t* mem = cart_rom->rom;

    cart_addr &= CART_ROM_ADDR_MASK;

    if (cart_addr + length < cart_rom->rom_size)
    {
        for(i = 0; i < length; ++i) {
            dram[(dram_addr+i)^S8] = mem[(cart_addr+i)^S8];
        }
    }
    else
    {
        unsigned int diff = (cart_rom->rom_size <= cart_addr)
            ? 0
            : cart_rom->rom_size - cart_addr;

        for (i = 0; i < diff; ++i) {
            dram[(dram_addr+i)^S8] = mem[(cart_addr+i)^S8];
        }
        for (; i < length; ++i) {
            dram[(dram_addr+i)^S8] = 0;
        }
    }

    return (length / 8) + add_random_interrupt_time(cart_rom->r4300);
}

/* ---- cart/eeprom.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - eeprom.c                                                *
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

enum { EEPROM_BLOCK_SIZE = 8 };

void format_eeprom(uint8_t* mem, size_t size)
{
    memset(mem, 0xff, size);
}

void init_eeprom(struct eeprom* eeprom,
    uint16_t type,
    void* storage,
    const struct storage_backend_interface* istorage)
{
    eeprom->type = type;
    eeprom->storage = storage;
    eeprom->istorage = istorage;
}

void eeprom_read_block(struct eeprom* eeprom,
    uint8_t block, uint8_t* data)
{
    unsigned int address = block * EEPROM_BLOCK_SIZE;

    if (address < eeprom->istorage->size(eeprom->storage))
    {
        memcpy(data, eeprom->istorage->data(eeprom->storage) + address, EEPROM_BLOCK_SIZE);
    }
    else
    {
        core_log(CORE_LOG_WARNING, "Invalid access to eeprom address=%04x", address);
    }
}

void eeprom_write_block(struct eeprom* eeprom,
    uint8_t block, const uint8_t* data, uint8_t* status)
{
    unsigned int address = block * EEPROM_BLOCK_SIZE;

    if (address < eeprom->istorage->size(eeprom->storage))
    {
        memcpy(eeprom->istorage->data(eeprom->storage) + address, data, EEPROM_BLOCK_SIZE);
        eeprom->istorage->save(eeprom->storage, address, EEPROM_BLOCK_SIZE);
        *status = 0x00;
    }
    else
    {
        core_log(CORE_LOG_WARNING, "Invalid access to eeprom address=%04x", address);
    }
}

/* ---- cart/flashram.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - flashram.c                                              *
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

static void flashram_command(struct flashram* flashram, uint32_t command)
{
    unsigned int i;
    unsigned int offset;
    uint8_t* mem = flashram->istorage->data(flashram->storage);

    switch (command & 0xff000000)
    {
    case 0x3c000000:
        /* set chip erase mode */
        flashram->mode = FLASHRAM_MODE_CHIP_ERASE;
        break;

    case 0x4b000000:
        /* set sector erase mode, set erase sector */
        flashram->mode = FLASHRAM_MODE_SECTOR_ERASE;
        flashram->erase_page = (command & 0xffff);
        break;

    case 0x78000000:
        /* set erase busy flag */
        flashram->status |= 0x02;

        /* do chip/sector erase */
        if (flashram->mode == FLASHRAM_MODE_SECTOR_ERASE) {
            offset = (flashram->erase_page & 0xff80) * 128;
            memset(mem + offset, 0xff, 128*128);
            flashram->istorage->save(flashram->storage, offset, 128*128);
        }
        else if (flashram->mode == FLASHRAM_MODE_CHIP_ERASE){
            memset(mem, 0xff, FLASHRAM_SIZE);
            flashram->istorage->save(flashram->storage, 0, FLASHRAM_SIZE);
        }
        else {
            core_log(CORE_LOG_WARNING, "Unexpected erase command (mode=%x)", flashram->mode);
        }

        /* clear erase busy flag, set erase success flag, transition to status mode */
        flashram->status &= ~UINT32_C(0x02);
        flashram->status |= 0x08;
        flashram->mode = FLASHRAM_MODE_STATUS;
        break;

    case 0xa5000000:
        /* set program busy flag */
        flashram->status |= 0x01;

        /* program selected page */
        offset = (command & 0xffff) * 128;
        for (i = 0; i < 128; ++i) {
            mem[(offset+i)^S8] = flashram->page_buf[i];
        }
        flashram->istorage->save(flashram->storage, offset, 128);

        /* clear program busy flag, set program success flag, transition to status mode */
        flashram->status &= ~UINT32_C(0x01);
        flashram->status |= 0x04;
        flashram->mode = FLASHRAM_MODE_STATUS;
        break;

    case 0xb4000000:
        /* set page program mode */
        flashram->mode = FLASHRAM_MODE_PAGE_PROGRAM;
        break;

    case 0xd2000000:
        /* set status mode */
        flashram->mode = FLASHRAM_MODE_STATUS;
        break;

    case 0xe1000000:
        /* set silicon_id mode */
        flashram->mode = FLASHRAM_MODE_READ_SILICON_ID;
        flashram->status |= 0x01; /* Needed for Pokemon Puzzle League */
        break;

    case 0xf0000000:
        /* set read mode */
        flashram->mode = FLASHRAM_MODE_READ_ARRAY;
        break;

    default:
        core_log(CORE_LOG_WARNING, "unknown flashram command: %" PRIX32, command);
    }
}

void init_flashram(struct flashram* flashram,
                   uint32_t flashram_id,
                   void* storage, const struct storage_backend_interface* istorage)
{
    flashram->silicon_id[0] = FLASHRAM_TYPE_ID;
    flashram->silicon_id[1] = flashram_id;
    flashram->storage = storage;
    flashram->istorage = istorage;
}

void poweron_flashram(struct flashram* flashram)
{
    flashram->mode = FLASHRAM_MODE_READ_ARRAY;
    flashram->status = 0x00;
    flashram->erase_page = 0;
    memset(flashram->page_buf, 0xff, 128);
}

void format_flashram(uint8_t* flash)
{
    memset(flash, 0xff, FLASHRAM_SIZE);
}

void read_flashram(void* opaque, uint32_t address, uint32_t* value)
{
    struct flashram* flashram = (struct flashram*)opaque;

    if ((address & 0x1ffff) == 0x00000 && flashram->mode == FLASHRAM_MODE_STATUS) {
        /* read Status register */
        *value = flashram->status;
    }
    else if ((address & 0x1ffff) == 0x0000 && flashram->mode == FLASHRAM_MODE_READ_ARRAY) {
        /* flashram MMIO read are not supported except for the "dummy" read @0x0000 done before DMA.
         * returns a "dummy" value. */
        *value = 0;
    }
    else {
        /* other accesses are not implemented */
        core_log(CORE_LOG_WARNING, "unknown Flashram read IO (mode=%x) @%08x", flashram->mode, address);
    }
}

void write_flashram(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct flashram* flashram = (struct flashram*)opaque;

    if ((address & 0x1ffff) == 0x00000 && flashram->mode == FLASHRAM_MODE_STATUS) {
        /* clear/set Status register */
        flashram->status = (value & mask) & 0xff;
    }
    else if ((address & 0x1ffff) == 0x10000) {
        /* set command */
        flashram_command(flashram, value & mask);
    }
    else {
        /* other accesses are not implemented */
        core_log(CORE_LOG_WARNING, "unknown Flashram write IO (mode=%x) @%08x <- %08x & %08x", flashram->mode, address, value, mask);
    }
}

unsigned int flashram_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    size_t i;
    struct flashram* flashram = (struct flashram*)opaque;
    const uint8_t* mem = flashram->istorage->data(flashram->storage);

    if ((cart_addr & 0x1ffff) == 0x00000 && length == 8 && flashram->mode == FLASHRAM_MODE_READ_SILICON_ID) {
        /* read Silicon ID using DMA */
        ((uint32_t*)dram)[dram_addr/4+0] = flashram->silicon_id[0];
        ((uint32_t*)dram)[dram_addr/4+1] = flashram->silicon_id[1];
    }
    else if ((cart_addr & 0x1ffff) < 0x10000 && flashram->mode == FLASHRAM_MODE_READ_ARRAY) {

        /* adjust flashram address before starting DMA. */
        if (flashram->silicon_id[1] == MX29L1100_ID
            || flashram->silicon_id[1] == MX29L0000_ID
            || flashram->silicon_id[1] == MX29L0001_ID) {
            /* "old" flash needs special address adjusting */
            cart_addr = (cart_addr & 0xffff) * 2;
        }
        else {
            /* "new" flash doesn't require special address adjusting at DMA start. */
            cart_addr &= 0xffff;
        }

        /* do actual DMA */
        for(i = 0; i < length; ++i) {
            dram[(dram_addr+i)^S8] = mem[(cart_addr+i)^S8];
        }
    }
    else {
        /* other accesses are not implemented */
        core_log(CORE_LOG_WARNING, "unknown Flashram DMA Write (mode=%x) @%08x <- %08x length=%08x",
            flashram->mode, dram_addr, cart_addr, length);
    }

    return /* length / 8 */0x1000;
}

unsigned int flashram_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct flashram* flashram = (struct flashram*)opaque;
    unsigned int i;

    if ((cart_addr & 0x1ffff) == 0x00000 && length == 128 && flashram->mode == FLASHRAM_MODE_PAGE_PROGRAM) {
        /* load page buf using DMA */
        for(i = 0; i < length; ++i) {
            flashram->page_buf[i] = dram[(dram_addr+i)^S8];
        }
    }
    else {
        /* other accesses are not implemented */
        core_log(CORE_LOG_WARNING, "unknown Flashram DMA Read (mode=%x) @%08x <- %08x length=%08x",
            flashram->mode, cart_addr, dram_addr, length);
    }

    return /* length / 8 */0x1000;
}

/* ---- cart/is_viewer.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - is_viewer.c                                             *
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

#define IS_ADDR_MASK UINT32_C(0x00000fff)

void poweron_is_viewer(struct is_viewer* is_viewer)
{
    memset(is_viewer->data, 0, IS_BUFFER_SIZE);
    memset(is_viewer->output_buffer, 0, IS_BUFFER_SIZE);
    is_viewer->buffer_pos = 0;
}

void read_is_viewer(void* opaque, uint32_t address, uint32_t* value)
{
    struct is_viewer* is_viewer = (struct is_viewer*)opaque;
    address &= IS_ADDR_MASK;
    memcpy(value, &is_viewer->data[address], 4);
    *value = core_swap32(*value);
}

void write_is_viewer(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct is_viewer* is_viewer = (struct is_viewer*)opaque;
    address &= IS_ADDR_MASK;
    uint32_t word = value & mask;
    if (address == 0x14)
    {
        if (word > 0)
        {
            if (word > IS_BUFFER_SIZE - 0x20) {
                core_log(CORE_LOG_WARNING, "IS64: invalid transfer length %u", word);
                return;
            }
            /* core_log has a 512-byte buffer. Emit bounded continuation
             * records instead of losing long lines (e.g. timing sample arrays).
             * Process every newline, retaining the partial line after it. */
            for (uint32_t i = 0; i < word; ++i) {
                char ch = is_viewer->data[0x20 + i];
                if (ch == '\n') {
                    core_log(CORE_LOG_INFO, "IS64: %.*s", (int)is_viewer->buffer_pos,
                                 is_viewer->output_buffer);
                    is_viewer->buffer_pos = 0;
                } else {
                    is_viewer->output_buffer[is_viewer->buffer_pos++] = ch;
                    if (is_viewer->buffer_pos == 480) {
                        core_log(CORE_LOG_INFO, "IS64+: %.*s", (int)is_viewer->buffer_pos,
                                     is_viewer->output_buffer);
                        is_viewer->buffer_pos = 0;
                    }
                }
            }
        }
    }
    else
    {
        word = core_swap32(word);
        memcpy(&is_viewer->data[address], &word, sizeof(word));
    }
}

/* ---- cart/sram.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - sram.c                                                  *
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

#define SRAM_ADDR_MASK UINT32_C(0x0000ffff)

void format_sram(uint8_t* mem)
{
    memset(mem, 0xff, SRAM_SIZE);
}

void init_sram(struct sram* sram,
               void* storage, const struct storage_backend_interface* istorage)
{
    sram->storage = storage;
    sram->istorage = istorage;
}

unsigned int sram_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    size_t i;
    struct sram* sram = (struct sram*)opaque;
    uint8_t* mem = sram->istorage->data(sram->storage);

    cart_addr &= SRAM_ADDR_MASK;

    for (i = 0; i < length; ++i) {
        mem[(cart_addr+i)^S8] = dram[(dram_addr+i)^S8];
    }

    sram->istorage->save(sram->storage, cart_addr, length);

    return /* length / 8 */0x1000;
}

unsigned int sram_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    size_t i;
    struct sram* sram = (struct sram*)opaque;
    const uint8_t* mem = sram->istorage->data(sram->storage);

    cart_addr &= SRAM_ADDR_MASK;

    for (i = 0; i < length; ++i) {
        dram[(dram_addr+i)^S8] = mem[(cart_addr+i)^S8];
    }

    return /* length / 8 */0x1000;
}

void read_sram(void* opaque, uint32_t address, uint32_t* value)
{
    struct sram* sram = (struct sram*)opaque;
    const uint8_t* mem = sram->istorage->data(sram->storage);

    address &= SRAM_ADDR_MASK;

    *value = *(uint32_t*)(mem + address);
}

void write_sram(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct sram* sram = (struct sram*)opaque;
    uint8_t* mem = sram->istorage->data(sram->storage);

    address &= SRAM_ADDR_MASK;

    masked_write((uint32_t*)(mem + address), value, mask);

    sram->istorage->save(sram->storage, address, sizeof(value));
}
