/* Optional context trailer on private task captures. Old captures imply
 * zero carry, phase and retained lanes. Range layout remains unchanged. */
#ifndef HLE_CAPTURE_CONTEXT_H
#define HLE_CAPTURE_CONTEXT_H
#include <stdint.h>
#include <stdio.h>
#define HLE_CAPTURE_VCO_MAGIC 0x56434f31u
#define HLE_CAPTURE_T6_MAGIC 0x46355436u
#define HLE_CAPTURE_F5_MAGIC 0x46355600u
static inline uint32_t hle_capture_value(FILE *file,uint32_t magic)
{
    long position=ftell(file);
    uint32_t count,span[2],context[2];
    uint32_t value=0;
    unsigned k;
    if(position<0)return 0;
    if(fread(&count,4,1,file)!=1 || count>512)goto finish;
    for(k=0;k<count;k++){
        if(fread(span,8,1,file)!=1 || span[0]>=0x800000u || span[1]>0x800000u-span[0] ||
           fseek(file,(long)span[1],SEEK_CUR))goto finish;
    }
    for(k=0;k<9;k++){
        if(fread(context,8,1,file)!=1)break;
        if(context[0]==magic){value=context[1];break;}
        if(context[0]!=HLE_CAPTURE_VCO_MAGIC && context[0]!=HLE_CAPTURE_T6_MAGIC &&
           (context[0]<HLE_CAPTURE_F5_MAGIC || context[0]>=HLE_CAPTURE_F5_MAGIC+7u))break;
    }
finish:
    clearerr(file);fseek(file,position,SEEK_SET);return value;
}
static inline uint16_t hle_capture_vco(FILE *file){
    uint32_t value=hle_capture_value(file,HLE_CAPTURE_VCO_MAGIC);
    return value<=0xffffu?(uint16_t)value:0;
}
static inline uint32_t hle_capture_t6(FILE *file){return hle_capture_value(file,HLE_CAPTURE_T6_MAGIC);}
static inline void hle_capture_f5(FILE *file,uint32_t values[7]){
    unsigned k;for(k=0;k<7;k++)values[k]=hle_capture_value(file,HLE_CAPTURE_F5_MAGIC+k);
}
#endif
