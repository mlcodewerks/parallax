/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - main.h                                                  *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2002 Blight                                             *
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

#ifndef __MAIN_H__
#define __MAIN_H__

#include <stdint.h>

#include "core/types.h"
#include "device/device.h"
#include "osal/preproc.h"

#if defined(__GNUC__)
#define ATTR_FMT(fmtpos, attrpos) __attribute__ ((format (printf, fmtpos, attrpos)))
#else
#define ATTR_FMT(fmtpos, attrpos)
#endif

/* globals */

extern int g_RomWordsLittleEndian;
extern int g_EmulatorRunning;
extern int g_rom_pause;

extern void* g_mem_base;

extern struct device g_dev;




extern int g_gs_vi_counter;

const char* get_savestatepath(void);
const char* get_savesrampath(void);

void new_frame(void);
void new_vi(void);

void main_switch_next_pak(int control_id);
void main_switch_accessory(int control_id);
void main_change_gb_cart(int control_id);

int  main_set_core_defaults(void);
void main_message(core_log_level level, unsigned int osd_corner, const char *format, ...) ATTR_FMT(3, 4);

core_result main_run(void);
void main_stop(void);
void main_toggle_pause(void);
void main_advance_one(void);

void main_speedup(int percent);
void main_speeddown(int percent);
void main_set_fastforward(int enable);

void main_take_next_screenshot(void);

void main_state_set_slot(int slot);
void main_state_inc_slot(void);
int main_stateload(const void *data);
int main_statesave(void *data);


core_result main_read_screen(void *pixels, int bFront);

core_result main_volume_up(void);
core_result main_volume_down(void);
core_result main_volume_get_level(int *level);
core_result main_volume_set_level(int level);
core_result main_volume_mute(void);
int        main_volume_get_muted(void);

core_result main_reset(int do_hard_reset);

core_result open_pif(const unsigned char* pifimage, unsigned int size);

#endif /* __MAIN_H__ */

