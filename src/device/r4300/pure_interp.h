/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pure_interp.h                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
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

#ifndef PARALLAX_DEVICE_R4300_PURE_INTERP_H
#define PARALLAX_DEVICE_R4300_PURE_INTERP_H

#include <stdbool.h>
#include <stdint.h>

struct r4300_core;

typedef void (*r4300_interp_handler)(struct r4300_core* r4300, uint32_t op);

extern bool breakloop;

r4300_interp_handler pure_interp_decode(uint32_t op, int idle);
void pure_interp_execute_one(struct r4300_core* r4300);
/* Shared COP1 bodies; callers have decoded a legal arithmetic/compare opcode. */
void pure_interp_fpu_arithmetic(struct r4300_core* r4300, uint32_t op);
void pure_interp_fpu_compare(struct r4300_core* r4300, uint32_t op);
/* Integer ABI for native COP1 emission; shared validation and result rules. */
int pure_interp_fpu_prepare(struct r4300_core*, uint32_t op, uint64_t inputs[3]);
void pure_interp_fpu_finish(struct r4300_core*, uint32_t op, uint64_t bits, unsigned int flags);
int pure_interp_fpu_compare_prepare(struct r4300_core*, uint32_t op, uint64_t inputs[3]);
int pure_interp_jit_fpu_prepare(struct r4300_core*, uint32_t op, uint64_t inputs[3], uint64_t* commits);
void pure_interp_fpu_compare_finish(struct r4300_core*, int result);

#endif /* PARALLAX_DEVICE_R4300_PURE_INTERP_H */
