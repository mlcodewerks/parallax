/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - emulate_speaker_via_audio_plugin.h                      *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2014, 2016 Bobby Smiles                                 *
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

#ifndef PARALLAX_AUDIO_H
#define PARALLAX_AUDIO_H

#include <stddef.h>

struct audio_out_backend_interface
{
    /* Allow the backend to be notified of sample frequency.
     */
    void (*set_frequency)(void* aout, unsigned int frequency);

    /* Push samples to be played by the backend
     */
    void (*push_samples)(void* aout, const void* samples, size_t size);
    /* Advance the DAC while the FIFO is empty, in source stereo frames. */
    void (*push_idle_samples)(void* aout, size_t frames);
};

#ifdef __cplusplus
extern "C" {
#endif

void init_audio_libretro(unsigned max_frames);
void deinit_audio_libretro(void);

extern const struct audio_out_backend_interface g_libretro_audio_backend;

#ifdef __cplusplus
}
#endif

#endif
