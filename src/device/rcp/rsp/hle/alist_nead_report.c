#include "hle_internal.h"
#include "memory.h"
#include "hle_timing.h"
#include "alist.h"

static int16_t* nead_s16(struct hle_t* hle, uint16_t dmem)
{
    return (int16_t*)(hle->alist_buffer + ((dmem ^ S16) & 0xfff));
}

/* OoT opcode 00 (IMEM f68): load a caller state block at fb0, increment
 * its invocation count, report total/chunk bytes left and the command tag,
 * and DMA the block back. This is separate from old NAudio's zero entries. */

void alist_nead_report(struct hle_t* hle,uint32_t w1,uint32_t w2,uint16_t nead_slab)
{
    uint32_t encoded=((w1>>12)&0xff0u)-1u;
    uint32_t counter,chunk,byte;
    alist_dmem_dma(hle,nead_slab,w2,encoded,0);
    counter=(uint32_t)(uint16_t)*nead_s16(hle,nead_slab+4u)<<16;
    counter|=(uint16_t)*nead_s16(hle,nead_slab+6u);counter++;
    chunk=hle->alist_chunk_capacity ? hle->alist_chunk : 64u-(((hle->alist_processed-8u)&63u)+8u);
    if(chunk>hle->alist_remaining)chunk=hle->alist_remaining;
    *nead_s16(hle,nead_slab+4u)=(int16_t)(counter>>16);
    *nead_s16(hle,nead_slab+6u)=(int16_t)counter;
    *nead_s16(hle,nead_slab+8u)=(int16_t)(hle->alist_remaining>>16);
    *nead_s16(hle,nead_slab+10u)=(int16_t)hle->alist_remaining;
    *nead_s16(hle,nead_slab+12u)=(int16_t)(chunk>>16);
    *nead_s16(hle,nead_slab+14u)=(int16_t)chunk;
    *nead_s16(hle,nead_slab)=(int16_t)w1;
    for(byte=0;byte<16u;byte++)
        hle->dmem[((nead_slab+byte)&0xfffu)^S8]=hle->alist_buffer[((nead_slab+byte)&0xfffu)^S8];
    alist_dmem_dma(hle,nead_slab,w2,encoded,1);
}

