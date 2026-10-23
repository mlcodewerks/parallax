#ifndef PARALLAX_DEVICE_CONTROLLERS_H
#define PARALLAX_DEVICE_CONTROLLERS_H

#include "core/joybus.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct game_controller;
struct controller_input_backend_interface;
struct storage_backend_interface;
struct rumble_backend_interface;
struct gb_cart;

/* ---- game_controller ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - game_controller.h                                       *
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

struct game_controller_flavor
{
    const char* name;
    uint16_t type;

    /* controller reset procedure */
    void (*reset)(struct game_controller* cont);
};

struct pak_interface
{
    const char* name;
    void (*plug)(void* pak);
    void (*unplug)(void* pak);
    void (*read)(void* pak, uint16_t address, uint8_t* data, size_t size);
    void (*write)(void* pak, uint16_t address, const uint8_t* data, size_t size);
};

struct game_controller
{
    uint8_t status;

    const struct game_controller_flavor* flavor;

    void* cin;
    const struct controller_input_backend_interface* icin;

    void* pak;
    const struct pak_interface* ipak;
};

void init_game_controller(struct game_controller* cont,
    const struct game_controller_flavor* flavor,
    void* cin, const struct controller_input_backend_interface* icin,
    void* pak, const struct pak_interface* ipak);

void change_pak(struct game_controller* cont,
                void* pak, const struct pak_interface* ipak);

/* Controller Joybus interface */
extern const struct joybus_device_interface
    g_ijoybus_device_controller;

/* Controller flavors */
extern const struct game_controller_flavor g_standard_controller_flavor;
extern const struct game_controller_flavor g_mouse_controller_flavor;

/* ---- biopak ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - biopak.h                                                *
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

struct biopak
{
    unsigned int bpm;
};

void init_biopak(struct biopak* bpk,
    unsigned int bpm);

extern const struct pak_interface g_ibiopak;

/* ---- mempak ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mempak.h                                                *
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

struct mempak
{
    void* storage;
    const struct storage_backend_interface* istorage;
};

enum { MEMPAK_SIZE = 0x8000 };

void format_mempak(uint8_t* mem);

void init_mempak(struct mempak* mpk,
                 void* storage,
                 const struct storage_backend_interface* istorage);

extern const struct pak_interface g_imempak;

/* ---- rumblepak ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rumblepak.h                                             *
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

struct rumblepak
{
    uint8_t state;
    void* rumble;
    const struct rumble_backend_interface* irumble;
};

void init_rumblepak(struct rumblepak* rpk,
    void* rumble, const struct rumble_backend_interface* irumble);

void poweron_rumblepak(struct rumblepak* rpk);

void set_rumble_reg(struct rumblepak* rpk, uint8_t value);

extern const struct pak_interface g_irumblepak;

/* ---- transferpak ---- */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - transferpak.h                                           *
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

enum cart_access_mode
{
    CART_NOT_INSERTED = 0x40,
    CART_ACCESS_MODE_0 = 0x80,
    CART_ACCESS_MODE_1 = 0x89
};

struct transferpak
{
    unsigned int enabled;
    unsigned int bank;
    unsigned int access_mode;
    unsigned int access_mode_changed;

    struct gb_cart* gb_cart;
};

void init_transferpak(struct transferpak* tpk, struct gb_cart* gb_cart);
void poweron_transferpak(struct transferpak* tpk);

void change_gb_cart(struct transferpak* tpk, struct gb_cart* gb_cart);

extern const struct pak_interface g_itransferpak;

#ifdef __cplusplus
}
#endif

#endif
