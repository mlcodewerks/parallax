/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-Next - core/frontend.h                          *
 *   Copyright (C) 2020 M4xw <m4x@m4xw.net>                                *
 *   Copyright (C) 2020 Daniel De Matteis                                  *
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

#ifndef PARALLAX_FRONTEND_H
#define PARALLAX_FRONTEND_H
#include <stdbool.h>
#include <libretro.h>
#include "core/backend.h"
#ifdef __cplusplus
extern "C" {
#endif
extern retro_environment_t environ_cb;
extern bool libretro_swap_buffer;
extern bool retro_savestate_complete;
extern int retro_savestate_result;
extern char *retro_dd_path_img, *retro_dd_path_rom;
extern char *retro_transferpak_rom_path, *retro_transferpak_ram_path;
extern uint32_t EnableFrameDuping, EnableFullspeed, EnableOverscan, AspectRatio;
extern uint32_t CountPerOp, CountPerOpDenomPot, CountPerScanlineOverride;
extern uint32_t ForceDisableExtraMem, IgnoreTLBExceptions;
uint32_t get_retro_screen_width(void);
uint32_t get_retro_screen_height(void);
#define RETRO_MEMORY_DD (0x100 + 1)
#define RETRO_GAME_TYPE_DD 1
#define RETRO_MEMORY_TRANSFERPAK (0x100 + 2)
#define RETRO_GAME_TYPE_TRANSFERPAK 2
#define CORE_NAME "parallax"
#ifdef __cplusplus
}
#endif
#endif
