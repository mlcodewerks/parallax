/* Optional context trailer on private task captures. Old captures imply a
 * clean vector carry. Ranges stay in their original format and position. */
#ifndef HLE_CAPTURE_CONTEXT_H
#define HLE_CAPTURE_CONTEXT_H
#include <stdint.h>
#include <stdio.h>
#define HLE_CAPTURE_VCO_MAGIC 0x56434f31u
static inline uint16_t hle_capture_vco(FILE *file)
{
    long position=ftell(file);
    uint32_t count,span[2],context[2];
    uint16_t value=0;
    unsigned k;
    if(position<0)return 0;
    if(fread(&count,4,1,file)!=1 || count>512)goto finish;
    for(k=0;k<count;k++){
        if(fread(span,8,1,file)!=1 || span[0]>=0x800000u || span[1]>0x800000u-span[0] ||
           fseek(file,(long)span[1],SEEK_CUR))goto finish;
    }
    if(fread(context,8,1,file)==1 && context[0]==HLE_CAPTURE_VCO_MAGIC && context[1]<=0xffffu)
        value=(uint16_t)context[1];
finish:
    clearerr(file);fseek(file,position,SEEK_SET);return value;
}
#endif
