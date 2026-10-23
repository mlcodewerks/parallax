/* Consolidated controllers device implementation.
 * Component copyright and license notices are retained below.
 */

#include "device/controllers/game_controller.h"
#include "core/log.h"
#include "core/types.h"
#include "core/controller_input.h"
#include "core/joybus.h"
#include <stdint.h>
#include <string.h>
#include "device/controllers/paks/biopak.h"
#include <time.h>
#include "device/controllers/paks/mempak.h"
#include "core/storage.h"
#include "device/controllers/paks/rumblepak.h"
#include "core/rumble.h"
#include "device/controllers/paks/transferpak.h"
#include "device/gb/gb_cart.h"

/* ---- controllers/game_controller.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - game_controller.c                                       *
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

enum { PAK_CHUNK_SIZE = 0x20 };

enum controller_status
{
    CONT_STATUS_PAK_PRESENT      = 0x01,
    CONT_STATUS_PAK_CHANGED      = 0x02,
    CONT_STATUS_PAK_ADDR_CRC_ERR = 0x04
};

/* Standard controller */
static void standard_controller_reset(struct game_controller* cont)
{
    /* reset controller status */
    cont->status = 0x00;

    /* if pak is connected */
    if (cont->ipak != NULL) {
        cont->status |= CONT_STATUS_PAK_PRESENT;
    }
    else {
        cont->status |= CONT_STATUS_PAK_CHANGED;
    }

    /* XXX: recalibrate joysticks  */
}

const struct game_controller_flavor g_standard_controller_flavor =
{
    "Standard controller",
    JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT,
    standard_controller_reset
};

/* Pak handling functions */
void change_pak(struct game_controller* cont,
                void* pak, const struct pak_interface* ipak)
{
    cont->status &= ~(CONT_STATUS_PAK_PRESENT | CONT_STATUS_PAK_CHANGED);

    /* unplug previous pak (if any) */
    if (cont->ipak != NULL) {
        cont->ipak->unplug(cont->pak);
        cont->status |= CONT_STATUS_PAK_CHANGED;
    }

    /* plug new one (if any) */
    if (ipak != NULL) {
        ipak->plug(pak);
        cont->status |= CONT_STATUS_PAK_PRESENT;
    }

    /* set new pak */
    cont->pak = pak;
    cont->ipak = ipak;
}

static uint8_t pak_data_crc(const uint8_t* data, size_t size)
{
    size_t i;
    uint8_t crc = 0;

    for(i = 0; i <= size; ++i)
    {
        int mask;
        for (mask = 0x80; mask >= 1; mask >>= 1)
        {
            uint8_t xor_tap = (crc & 0x80) ? 0x85 : 0x00;
            crc <<= 1;
            if (i != size && (data[i] & mask)) crc |= 1;
            crc ^= xor_tap;
        }
    }
    return crc;
}

static void pak_read_block(struct game_controller* cont,
    const uint8_t* addr_acrc, uint8_t* data, uint8_t* dcrc)
{
    uint16_t address = (addr_acrc[0] << 8) | (addr_acrc[1] & 0xe0);
#if 0
    uint8_t  acrc = addr_acrc[1] & 0x1f;
#endif

    if (cont->ipak != NULL) {
        cont->ipak->read(cont->pak, address, data, PAK_CHUNK_SIZE);
        *dcrc = pak_data_crc(data, PAK_CHUNK_SIZE);
    } else {
        //NOT the CRC value when pak is not present
        *dcrc = ~pak_data_crc(data, PAK_CHUNK_SIZE);
    }
}

static void pak_write_block(struct game_controller* cont,
    const uint8_t* addr_acrc, const uint8_t* data, uint8_t* dcrc)
{
    uint16_t address = (addr_acrc[0] << 8) | (addr_acrc[1] & 0xe0);
#if 0
    uint8_t  acrc = addr_acrc[1] & 0x1f;
#endif

    if (cont->ipak != NULL) {
        cont->ipak->write(cont->pak, address, data, PAK_CHUNK_SIZE);
        *dcrc = pak_data_crc(data, PAK_CHUNK_SIZE);
    } else {
        *dcrc = ~pak_data_crc(data, PAK_CHUNK_SIZE);
    }
}

/* Mouse controller */
static void mouse_controller_reset(struct game_controller* cont)
{
    cont->status = 0x00;
}

const struct game_controller_flavor g_mouse_controller_flavor =
{
    "Mouse controller",
    JDT_JOY_REL_COUNTERS,
    mouse_controller_reset
};

void init_game_controller(struct game_controller* cont,
    const struct game_controller_flavor* flavor,
    void* cin, const struct controller_input_backend_interface* icin,
    void* pak, const struct pak_interface* ipak)
{
    cont->flavor = flavor;
    cont->cin = cin;
    cont->icin = icin;
    cont->pak = pak;
    cont->ipak = ipak;
}

static void poweron_game_controller(void* jbd)
{
    struct game_controller* cont = (struct game_controller*)jbd;

    cont->flavor->reset(cont);

    if (cont->flavor == &g_standard_controller_flavor && cont->ipak != NULL) {
        cont->ipak->plug(cont->pak);
    }
}

static void process_controller_command(void* jbd,
    const uint8_t* tx, const uint8_t* tx_buf,
    uint8_t* rx, uint8_t* rx_buf)
{
    struct game_controller* cont = (struct game_controller*)jbd;
    uint32_t input_ = 0;
    uint8_t cmd = tx_buf[0];

    /* if controller can't successfully be polled, consider it to be absent */
    if (cont->icin->get_input(cont->cin, &input_) != CORE_OK) {
        *rx |= 0x80;
        return;
    }

    switch (cmd)
    {
    case JCMD_RESET:
        cont->flavor->reset(cont);
        /* fall through */
    case JCMD_STATUS: {
        JOYBUS_CHECK_COMMAND_FORMAT(1, 3)

        rx_buf[0] = (uint8_t)(cont->flavor->type >> 0);
        rx_buf[1] = (uint8_t)(cont->flavor->type >> 8);
        rx_buf[2] = cont->status;
    } break;

    case JCMD_CONTROLLER_READ: {
        JOYBUS_CHECK_COMMAND_FORMAT(1, 4)

        *((uint32_t*)(rx_buf)) = input_;
    } break;

    case JCMD_PAK_READ: {
        JOYBUS_CHECK_COMMAND_FORMAT(3, 33)
        pak_read_block(cont, &tx_buf[1], &rx_buf[0], &rx_buf[32]);
    } break;

    case JCMD_PAK_WRITE: {
        JOYBUS_CHECK_COMMAND_FORMAT(35, 1)
        pak_write_block(cont, &tx_buf[1], &tx_buf[3], &rx_buf[0]);
    } break;

    default:
        core_log(CORE_LOG_WARNING, "cont: Unknown command %02x %02x %02x",
            *tx, *rx, cmd);
    }
}

const struct joybus_device_interface g_ijoybus_device_controller =
{
    poweron_game_controller,
    process_controller_command,
    NULL
};

/* ---- controllers/paks/biopak.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - biopak.c                                             *
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

/* Implementation based on notes from raphnet
 * See http://www.raphnet.net/divers/n64_bio_sensor/index_en.php
 */

#ifdef HAVE_LIBNX
#include <switch.h>
#endif

void init_biopak(struct biopak* bpk,
    unsigned int bpm)
{
    bpk->bpm = bpm;
}

static void plug_biopak(void* pak)
{
}

static void unplug_biopak(void* pak)
{
}

static void read_biopak(void* pak, uint16_t address, uint8_t* data, size_t size)
{
    struct biopak* bpk = (struct biopak*)pak;

    if (address == 0xc000) {
        time_t now = time(NULL) * 1000;
        uint32_t period = UINT32_C(60*1000) / bpk->bpm;
        uint32_t k = now % period;

        memset(data, (2*k < period) ? 0x00 : 0x03, size);
    }
    else {
        core_log(CORE_LOG_WARNING, "Unexpected bio sensor read address %04x", address);
    }
}

static void write_biopak(void* pak, uint16_t address, const uint8_t* data, size_t size)
{
    core_log(CORE_LOG_WARNING, "Unexpected bio sensor write address %04x", address);
}

/* bio pak definition */
const struct pak_interface g_ibiopak =
{
    "Bio pak",
    plug_biopak,
    unplug_biopak,
    read_biopak,
    write_biopak
};

/* ---- controllers/paks/mempak.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mempak.c                                                *
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

void format_mempak(uint8_t* mem)
{
    enum { MPK_PAGE_SIZE = 256 };
    size_t i;

    static const uint8_t page_0[MPK_PAGE_SIZE] =
    {
        /* Label area */
        0x81,0x01,0x02,0x03, 0x04,0x05,0x06,0x07, 0x08,0x09,0x0a,0x0b, 0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13, 0x14,0x15,0x16,0x17, 0x18,0x19,0x1a,0x1b, 0x1c,0x1d,0x1e,0x1f,
        /* Main ID area */
        0xff,0xff,0xff,0xff, 0x05,0x1a,0x5f,0x13, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0x01,0xff, 0x66,0x25,0x99,0xcd,
        /* Unused */
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        /* ID area backup #1 */
        0xff,0xff,0xff,0xff, 0x05,0x1a,0x5f,0x13, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0x01,0xff, 0x66,0x25,0x99,0xcd,
        /* ID area backup #2 */
        0xff,0xff,0xff,0xff, 0x05,0x1a,0x5f,0x13, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0x01,0xff, 0x66,0x25,0x99,0xcd,
        /* Unused */
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        /* ID area backup #3 */
        0xff,0xff,0xff,0xff, 0x05,0x1a,0x5f,0x13, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0x01,0xff, 0x66,0x25,0x99,0xcd,
        /* Unused */
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00
    };

    /* Fill Page 0 with pre-initialized content */
    memcpy(mem, page_0, MPK_PAGE_SIZE);

    /* Fill INODE page 1 and update it's checkum */
    size_t start_page = 5;
    memset(mem + 1*MPK_PAGE_SIZE, 0, 2*start_page);
    for (i = 1*MPK_PAGE_SIZE+2*start_page; i < 2*MPK_PAGE_SIZE; i += 2) {
        mem[i+0] = 0x00;
        mem[i+1] = 0x03;
    }
    mem[1*MPK_PAGE_SIZE + 1] = 0x71;

    /* Page 2 is identical to page 1 */
    memcpy(mem + 2*MPK_PAGE_SIZE, mem + 1*MPK_PAGE_SIZE, MPK_PAGE_SIZE);

    /* Remaining pages DIR+DATA (3...) are initialized with 0x00 */
    memset(mem + 3*MPK_PAGE_SIZE, 0, MEMPAK_SIZE - 3*MPK_PAGE_SIZE);
}

void init_mempak(struct mempak* mpk,
                 void* storage,
                 const struct storage_backend_interface* istorage)
{
    mpk->storage = storage;
    mpk->istorage = istorage;
}

static void plug_mempak(void* pak)
{
}

static void unplug_mempak(void* pak)
{
}

static void read_mempak(void* pak, uint16_t address, uint8_t* data, size_t size)
{
    struct mempak* mpk = (struct mempak*)pak;

    if (address < 0x8000)
    {
        memcpy(data, mpk->istorage->data(mpk->storage) + address, size);
    }
    else
    {
        memset(data, 0x00, size);
    }
}

static void write_mempak(void* pak, uint16_t address, const uint8_t* data, size_t size)
{
    struct mempak* mpk = (struct mempak*)pak;

    if (address < 0x8000)
    {
        memcpy(mpk->istorage->data(mpk->storage) + address, data, size);
        mpk->istorage->save(mpk->storage, address, size);
    }
    else
    {
        /* do nothing */
    }
}

/* Memory pak definition */
const struct pak_interface g_imempak =
{
    "Memory pak",
    plug_mempak,
    unplug_mempak,
    read_mempak,
    write_mempak
};

/* ---- controllers/paks/rumblepak.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rumblepak.c                                             *
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

void set_rumble_reg(struct rumblepak* rpk, uint8_t value)
{
    rpk->state = value;
    rpk->irumble->exec(rpk->rumble, (rpk->state == 0) ? RUMBLE_STOP : RUMBLE_START);
}

void init_rumblepak(struct rumblepak* rpk,
    void* rumble, const struct rumble_backend_interface* irumble)
{
    rpk->rumble = rumble;
    rpk->irumble = irumble;
}

void poweron_rumblepak(struct rumblepak* rpk)
{
    set_rumble_reg(rpk, 0x00);
}

static void plug_rumblepak(void* pak)
{
    struct rumblepak* rpk = (struct rumblepak*)pak;

    poweron_rumblepak(rpk);
}

static void unplug_rumblepak(void* pak)
{
    struct rumblepak* rpk = (struct rumblepak*)pak;

    /* Stop rumbling if pak gets disconnected */
    set_rumble_reg(rpk, 0x00);
}

static void read_rumblepak(void* pak, uint16_t address, uint8_t* data, size_t size)
{
    uint8_t value;

    if ((address >= 0x8000) && (address < 0x9000))
    {
        value = 0x80;
    }
    else
    {
        value = 0x00;
    }

    memset(data, value, size);
}

static void write_rumblepak(void* pak, uint16_t address, const uint8_t* data, size_t size)
{
    struct rumblepak* rpk = (struct rumblepak*)pak;

    if (address == 0xc000)
    {
        set_rumble_reg(rpk, data[size - 1]);
    }
}

/* Rumble pak definition */
const struct pak_interface g_irumblepak =
{
    "Rumble pak",
    plug_rumblepak,
    unplug_rumblepak,
    read_rumblepak,
    write_rumblepak
};

/* ---- controllers/paks/transferpak.c ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - transferpak.c                                           *
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

/* This work is based on the NRage plugin source.
 * Transfer pak emulation is certainly NOT accurate
 * but without proper reverse-engineering this as good as we can get.
 */

static uint16_t gb_cart_address(unsigned int bank, uint16_t address)
{
    return (address & 0x3fff) | ((bank & 0x3) * 0x4000) ;
}

void init_transferpak(struct transferpak* tpk, struct gb_cart* gb_cart)
{
    tpk->gb_cart = gb_cart;
}

void poweron_transferpak(struct transferpak* tpk)
{
    tpk->enabled = 0;
    tpk->bank = 0;
    tpk->access_mode = (tpk->gb_cart == NULL)
        ? CART_NOT_INSERTED
        : CART_ACCESS_MODE_0;
    tpk->access_mode_changed = 0x44;

    if (tpk->gb_cart != NULL) {
        poweron_gb_cart(tpk->gb_cart);
    }
}

void change_gb_cart(struct transferpak* tpk, struct gb_cart* gb_cart)
{
    tpk->enabled = 0;

    if (gb_cart == NULL) {
        tpk->access_mode = CART_NOT_INSERTED;
    }
    else {
        tpk->access_mode = CART_ACCESS_MODE_0;
        poweron_gb_cart(gb_cart);
    }

    tpk->gb_cart = gb_cart;
}

static void plug_transferpak(void* pak)
{
    struct transferpak* tpk = (struct transferpak*)pak;
    poweron_transferpak(tpk);
}

static void unplug_transferpak(void* pak)
{
}

static void read_transferpak(void* pak, uint16_t address, uint8_t* data, size_t size)
{
    struct transferpak* tpk = (struct transferpak*)pak;
    uint8_t value;

    core_log(CORE_LOG_VERBOSE, "tpak read: %04x", address);

    switch(address >> 12)
    {
    case 0x8:
        /* get gb cart state (enabled/disabled) */
        value = (tpk->enabled)
              ? 0x84
              : 0x00;

        core_log(CORE_LOG_VERBOSE, "tpak get cart state: %02x", value);
        memset(data, value, size);
        break;

    case 0xb:
        /* get gb cart access mode */
        if (tpk->enabled)
        {
            core_log(CORE_LOG_VERBOSE, "tpak get access mode: %02x", tpk->access_mode);
            memset(data, tpk->access_mode, size);
            if (tpk->access_mode != CART_NOT_INSERTED)
            {
                data[0] |= tpk->access_mode_changed;
            }
            tpk->access_mode_changed = 0;
        }
        break;

    case 0xc:
    case 0xd:
    case 0xe:
    case 0xf:
        /* read gb cart */
        if (tpk->enabled)
        {
            core_log(CORE_LOG_VERBOSE, "tpak read cart: %04x", address);

            if (tpk->gb_cart != NULL) {
                read_gb_cart(tpk->gb_cart, gb_cart_address(tpk->bank, address), data, size);
            }
        }
        break;

    default:
        core_log(CORE_LOG_WARNING, "Unknown tpak read: %04x", address);
    }
}

static void write_transferpak(void* pak, uint16_t address, const uint8_t* data, size_t size)
{
    struct transferpak* tpk = (struct transferpak*)pak;
    uint8_t value = data[size-1];

    core_log(CORE_LOG_VERBOSE, "tpak write: %04x <- %02x", address, value);

    switch(address >> 12)
    {
    case 0x8:
        /* enable / disable gb cart */
        switch(value)
        {
        case 0xfe:
            tpk->enabled = 0;
            core_log(CORE_LOG_VERBOSE, "tpak disabled");
            break;
        case 0x84:
            tpk->enabled = 1;
            core_log(CORE_LOG_VERBOSE, "tpak enabled");
            break;
        default:
            core_log(CORE_LOG_WARNING, "Unknown tpak write: %04x <- %02x", address, value);
        }
        break;

    case 0xa:
        /* set gb cart bank */
        if (tpk->enabled)
        {
            tpk->bank = value;
            core_log(CORE_LOG_VERBOSE, "tpak set bank %02x", tpk->bank);
        }
        break;

    case 0xb:
        /* set gb cart access mode */
        if (tpk->enabled)
        {
            tpk->access_mode_changed = 0x04;

            tpk->access_mode = ((value & 1) == 0)
                              ? CART_ACCESS_MODE_0
                              : CART_ACCESS_MODE_1;

            if ((value & 0xfe) != 0)
            {
                core_log(CORE_LOG_WARNING, "Unknown tpak write: %04x <- %02x", address, value);
            }

            core_log(CORE_LOG_VERBOSE, "tpak set access mode %02x", tpk->access_mode);
        }
        break;

    case 0xc:
    case 0xd:
    case 0xe:
    case 0xf:
        /* write gb cart */
//        if (tpk->enabled)
        {
            core_log(CORE_LOG_VERBOSE, "tpak write gb: %04x <- %02x", address, value);

            if (tpk->gb_cart != NULL) {
                write_gb_cart(tpk->gb_cart, gb_cart_address(tpk->bank, address), data, size);
            }
        }
        break;
    default:
        core_log(CORE_LOG_WARNING, "Unknown tpak write: %04x <- %02x", address, value);
    }
}

/* Transfer pak definition */
const struct pak_interface g_itransferpak =
{
    "Transfer pak",
    plug_transferpak,
    unplug_transferpak,
    read_transferpak,
    write_transferpak
};
