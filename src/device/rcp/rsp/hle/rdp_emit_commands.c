/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Decode native command tables without assuming a particular GBI layout. */
#include "rdp_emit_f3dex2.h"
#include <string.h>

void rdp_fifo_decode_commands(RdpFifo *fifo, const unsigned char *ram,
                              unsigned int size, unsigned int text)
{
    unsigned int at;
    fifo->moveword_table=0;
    memset(fifo->native_noops,0,sizeof(fifo->native_noops));
    if (!ram || text>=size || (text&3u)) return;
    for(at=0;at+24u<=0xf80u && at+24u<=size-text;at+=4u) {
        const unsigned int *w=(const unsigned int *)(ram+text+at);
        if(w[0]==0x9361fffbu && w[1]==0x9762fff9u &&
           (w[2]&0xffff0000u)==0x84250000u && w[3]==0x00a22820u &&
           (w[4]&0xfc000000u)==0x08000000u && w[5]==0xacb80000u)
            fifo->moveword_table=w[2]&0xfffu;
        if(w[0]==0x326200ffu && w[1]==0x9623fff9u &&
           (w[2]&0xffff0000u)==0x94420000u && w[3]==0x00431021u &&
           (w[4]&0xfc000000u)==0x08000000u && w[5]==0xac540000u)
            fifo->moveword_table=w[2]&0xfffu;
    }

    if (!fifo->dmem) return;
    for(at=0;at+32u<=0xf80u && at+32u<=size-text;at+=4u) {
        const unsigned int *w=(const unsigned int *)(ram+text+at);
        if(w[0]==0x40012000u && (w[1]&0xffff0000u)==0x8f790000u &&
           w[2]==0x1360fff6u && w[3]==0x30210080u &&
           w[4]==0x00196603u && w[5]==0x000c5840u &&
           (w[6]&0xffff0000u)==0x956b0000u) {
            unsigned int op;
            for(op=0;op<256;op++) {
                unsigned int table=((w[6]&0xffffu)+2u*(op<128?op:op-256u))&0xfffu;
                unsigned int target=((unsigned int)fifo->dmem[table^3u]<<8)|
                    fifo->dmem[((table+1u)&0xfffu)^3u];
                if(target==(0x1080u+at))fifo->native_noops[op>>5]|=1u<<(op&31u);
            }
        }
    }
}

