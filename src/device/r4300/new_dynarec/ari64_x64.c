/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - assem_x64.c                                             *
 *   Copyright (C) 2009-2011 Ari64                                         *
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

/* Adapted from libretro/parallel-n64 ef73c7e6fa356262f88e05f85cdfad092f8f90e0.
 * Only the x64 machine-code emitter is retained. CPU state, instruction
 * boundaries and exception-sensitive operations use this core's shared model.
 */
#include "ari64_x64.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/idec.h"
#include "device/r4300/timing.h"
#include "device/r4300/pure_interp.h"
#include "device/memory.h"
#include "device/rdram.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__x86_64__) && !defined(_M_X64)
#error The Ari64 backend supports x64 only
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
typedef unsigned char u_char;
typedef unsigned int u_int;
#include "x64/assem_x64.h"
/* Compilation is confined to the emulation thread, outside generated code. */
static u_char* out;
static void output_byte(u_char byte)
{
  *(out++)=byte;
}

static void output_modrm(u_char mod,u_char rm,u_char ext)
{
  assert(mod<4);
  assert(rm<8);
  assert(ext<8);
  u_char byte=(mod<<6)|(ext<<3)|rm;
  *(out++)=byte;
}

static void output_rex(u_char w,u_char r,u_char x,u_char b)
{
  assert(w<2);
  assert(r<2);
  assert(x<2);
  assert(b<2);
  u_char byte=0x40|(w<<3)|(r<<2)|(x<<1)|b;
  *(out++)=byte;
}

static void output_w32(u_int word)
{
  memcpy(out, &word, sizeof(word));
  out+=4;
}

static void output_w64(uint64_t dword)
{
  memcpy(out, &dword, sizeof(dword));
  out+=8;
}

static void emit_mov(int rs,int rt)
{
  if(rs>=8||rt>=8) output_rex(0,rs>>3,0,rt>>3);
  output_byte(0x89);
  output_modrm(3,rt&7,rs&7);
}

static void emit_mov64(int rs,int rt)
{
  output_rex(1,rs>>3,0,rt>>3);
  output_byte(0x89);
  output_modrm(3,rt&7,rs&7);
}

static void emit_movimm(int imm,u_int rt)
{
  assert(rt<16);
  if(rt>=8) output_rex(0,0,0,1);
  output_byte(0xB8+(rt&7));
  output_w32(imm);
}

static void emit_movimm64(int64_t imm,u_int rt)
{
  assert(rt<16);
  if((uint64_t)imm <= UINT32_MAX) { emit_movimm((uint32_t)imm,rt); return; }
  if(imm >= INT32_MIN && imm <= INT32_MAX) {
    output_rex(1,0,0,rt>>3); output_byte(0xc7); output_modrm(3,rt&7,0); output_w32((uint32_t)imm); return;
  }
  output_rex(1,0,0,rt>>3);
  output_byte(0xB8+(rt&7));
  output_w64(imm);
}

static void emit_addimm(int rs,int imm,int rt)
{
  if(rs==rt) {
    if(imm!=0) {
          if(rt>=8) output_rex(0,0,0,1);
      if(imm<128&&imm>=-128) {
        output_byte(0x83);
        output_modrm(3,rt&7,0);
        output_byte(imm);
      }
      else
      {
        output_byte(0x81);
        output_modrm(3,rt&7,0);
        output_w32(imm);
      }
    }
  }
  else {
    if(imm!=0) {
          if(rs>=8||rt>=8) output_rex(0,rt>>3,0,rs>>3);
      output_byte(0x8D);
      if(imm<128&&imm>=-128) {
        output_modrm(1,rs&7,rt&7);
        if((rs&7)==4) output_byte(0x24);
        output_byte(imm);
      }else{
        output_modrm(2,rs&7,rt&7);
        if((rs&7)==4) output_byte(0x24);
        output_w32(imm);
      }
    }else{
      emit_mov(rs,rt);
    }
  }
}

static void emit_addimm64(int rs,int imm,int rt)
{
  if(rs==rt) {
    if(imm!=0) {
          if(imm<128&&imm>=-128) {
        output_rex(1,0,0,rt>>3);
        output_byte(0x83);
        output_modrm(3,rt&7,0);
        output_byte(imm);
      }
      else
      {
        output_rex(1,0,0,rt>>3);
        output_byte(0x81);
        output_modrm(3,rt&7,0);
        output_w32(imm);
      }
    }
  }
  else {
    if(imm!=0) {
          output_rex(1,rt>>3,0,rs>>3);
      output_byte(0x8D);
      if(imm<128&&imm>=-128) {
        output_modrm(1,rs&7,rt&7);
        if((rs&7)==4) output_byte(0x24);
        output_byte(imm);
      }else{
        output_modrm(2,rs&7,rt&7);
        if((rs&7)==4) output_byte(0x24);
        output_w32(imm);
      }
    }else{
      emit_mov64(rs,rt);
    }
  }
}

static void emit_shlimm(int rs,u_int imm,int rt)
{
  if(rs==rt) {
      assert(imm>0);
    if(rt>=8) output_rex(0,0,0,rt>>3);
    if(imm==1) output_byte(0xD1);
    else output_byte(0xC1);
    output_modrm(3,rt&7,4);
    if(imm>1) output_byte(imm);
  }
  else {
    emit_mov(rs,rt);
    emit_shlimm(rt,imm,rt);
  }
}

static void emit_shlimm64(int rs,u_int imm,int rt)
{
  if(rs==rt) {
      assert(imm>0);
    output_rex(1,0,0,rt>>3);
    if(imm==1) output_byte(0xD1);
    else output_byte(0xC1);
    output_modrm(3,rt&7,4);
    if(imm>1) output_byte(imm);
  }
  else {
    emit_mov64(rs,rt);
    emit_shlimm64(rt,imm,rt);
  }
}

static void emit_shrimm(int rs,u_int imm,int rt)
{
  if(rs==rt) {
      assert(imm>0);
    if(rt>=8) output_rex(0,0,0,rt>>3);
    if(imm==1) output_byte(0xD1);
    else output_byte(0xC1);
    output_modrm(3,rt&7,5);
    if(imm>1) output_byte(imm);
  }
  else {
    emit_mov(rs,rt);
    emit_shrimm(rt,imm,rt);
  }
}

static void emit_shrimm64(int rs,u_int imm,int rt)
{
  assert(rs==rt);
  if(rs==rt) {
      assert(imm>0);
    output_rex(1,0,0,rt>>3);
    if(imm==1) output_byte(0xD1);
    else output_byte(0xC1);
    output_modrm(3,rt&7,5);
    if(imm>1) output_byte(imm);
  }
  else {
    emit_mov64(rs,rt);
    emit_shrimm64(rt,imm,rt);
  }
}

static void emit_sarimm(int rs,u_int imm,int rt)
{
  if(rs==rt) {
      assert(imm>0);
    if(rt>=8) output_rex(0,0,0,rt>>3);
    if(imm==1) output_byte(0xD1);
    else output_byte(0xC1);
    output_modrm(3,rt&7,7);
    if(imm>1) output_byte(imm);
  }
  else {
    emit_mov(rs,rt);
    emit_sarimm(rt,imm,rt);
  }
}
#include "x64/frontend_x64.inc"
