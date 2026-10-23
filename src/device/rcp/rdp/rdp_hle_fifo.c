/* SPDX-License-Identifier: GPL-2.0-or-later
 * Retained HLE commands while the DP is frozen, including savestate storage. */
#include "rdp_core.h"
#include <stdlib.h>
#include <string.h>

void rdp_hle_frozen_reset(struct rdp_core *dp)
{
    free(dp->hle_frozen_words);
    dp->hle_frozen_words = NULL;
    dp->hle_frozen_bytes = dp->hle_frozen_capacity = dp->hle_frozen_guest_end = 0;
}
static void reserve_frozen_hle(struct rdp_core *dp, uint32_t bytes)
{
    if (bytes > dp->hle_frozen_capacity) {
        uint32_t capacity = dp->hle_frozen_capacity ? dp->hle_frozen_capacity : 131072u;
        while (capacity < bytes)
            capacity = capacity > UINT32_MAX/2u ? bytes : capacity*2u;
        void *words = realloc(dp->hle_frozen_words, capacity);
        if (!words) abort();
        dp->hle_frozen_words = words;
        dp->hle_frozen_capacity = capacity;
    }
}
static uint32_t frozen_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void frozen_put32(unsigned char *p, uint32_t word)
{ for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(word>>(i*8)); }
int rdp_hle_frozen_save(const struct rdp_core *dp, void *data, uint32_t bytes)
{
    unsigned char *p = data;
    if (bytes < 12u || dp->hle_frozen_bytes > bytes - 12u) return 0;
    frozen_put32(p, 0x484c4651u);
    frozen_put32(p+4, dp->hle_frozen_bytes);
    frozen_put32(p+8, dp->hle_frozen_guest_end);
    for (uint32_t i=0;i<dp->hle_frozen_bytes/4u;++i)
        frozen_put32(p+12+i*4, dp->hle_frozen_words[i]);
    return 1;
}
int rdp_hle_frozen_load(struct rdp_core *dp, const void *data, uint32_t bytes)
{
    const unsigned char *p = data;
    uint32_t count;
    if (!p) { rdp_hle_frozen_reset(dp); return 1; }
    if (bytes < 12u || frozen_u32(p) != 0x484c4651u) return 0;
    count = frozen_u32(p+4);
    if ((count&7u) || count > bytes-12u || frozen_u32(p+8)>0xfffff8u) return 0;
    reserve_frozen_hle(dp, count);
    for (uint32_t i=0;i<count/4u;++i) dp->hle_frozen_words[i]=frozen_u32(p+12+i*4);
    dp->hle_frozen_bytes=count;
    dp->hle_frozen_guest_end=frozen_u32(p+8);
    return 1;
}

void rdp_hle_frozen_append(struct rdp_core *dp, const uint32_t *words, uint32_t bytes)
{
    if (bytes > UINT32_MAX - dp->hle_frozen_bytes) abort();
    reserve_frozen_hle(dp, dp->hle_frozen_bytes + bytes);
    memcpy((unsigned char *)dp->hle_frozen_words + dp->hle_frozen_bytes, words, bytes);
    dp->hle_frozen_bytes += bytes;
    dp->hle_frozen_guest_end = dp->dpc_regs[DPC_END_REG];
}
