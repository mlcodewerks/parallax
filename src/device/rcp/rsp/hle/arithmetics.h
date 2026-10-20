/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - arithmetics.h                                   *
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

#ifndef ARITHMETICS_H
#define ARITHMETICS_H

#include <stdint.h>

#include "common.h"

static inline int16_t clamp_s16(int_fast32_t x)
{
    x = (x < INT16_MIN) ? INT16_MIN: x;
    x = (x > INT16_MAX) ? INT16_MAX: x;

    return x;
}

static inline int32_t vmulf(int16_t x, int16_t y)
{
    return (((int32_t)(x))*((int32_t)(y))+0x4000)>>15;
}

/* VMADN's ACC_L result saturates when ACC_H:ACC_M is not a signed
 * 16-bit value. Comparing only the clamped ACC_M to its raw low half
 * misses overflow whose middle half is already 7fff or 8000. */
static inline int16_t clamp_rsp_acc_low(int64_t accumulator)
{
    int64_t upper = accumulator >> 16;
    if (upper > 32767) return (int16_t)0xffff;
    if (upper < -32768) return 0;
    return (int16_t)(accumulator & 0xffff);
}

#endif

