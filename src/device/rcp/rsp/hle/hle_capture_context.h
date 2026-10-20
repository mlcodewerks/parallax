/* Optional context trailer on private task captures. Old captures imply
 * zero carry, phase and retained lanes. Range layout remains unchanged. */
#ifndef HLE_CAPTURE_CONTEXT_H
#define HLE_CAPTURE_CONTEXT_H
#include <stdint.h>
#include <stdio.h>
#define HLE_CAPTURE_VCO_MAGIC 0x56434f31u
#define HLE_CAPTURE_T6_MAGIC 0x46355436u
#define HLE_CAPTURE_F5_MAGIC 0x46355600u
#define HLE_CAPTURE_RSP_MAGIC 0x52535031u
#define HLE_CAPTURE_CP0_MAGIC 0x43503031u
/* Offline references merge DMA writes; sparse video output may exceed the
 * live capture logger's 512-entry array. Eight bytes is the minimum DMA. */
#define HLE_CAPTURE_MAX_RANGES (0x800000u / 8u)
static inline void hle_capture_write_cp0(FILE *file, const uint32_t words[16])
{
    uint32_t tag[2] = { HLE_CAPTURE_CP0_MAGIC, 16 };
    fwrite(tag, sizeof(tag), 1, file);
    fwrite(words, 16 * 4, 1, file);
}
static inline int hle_capture_cp0(FILE *file, uint32_t words[16])
{
    long position = ftell(file);
    uint32_t count, span[2], tag[2];
    unsigned k;
    int found = 0;
    if (position < 0) return 0;
    if (fread(&count, 4, 1, file) != 1 || count > HLE_CAPTURE_MAX_RANGES) goto finish_cp0;
    for (k = 0; k < count; ++k) {
        if (fread(span, 8, 1, file) != 1 || span[0] >= 0x800000u ||
            span[1] > 0x800000u-span[0] || fseek(file, (long)span[1], SEEK_CUR)) goto finish_cp0;
    }
    for (k = 0; k < 12; ++k) {
        if (fread(tag, 8, 1, file) != 1) break;
        if (tag[0] == HLE_CAPTURE_CP0_MAGIC) {
            found = tag[1] == 16 && fread(words, 16 * 4, 1, file) == 1;
            break;
        }
        if (tag[0] == HLE_CAPTURE_RSP_MAGIC) {
            if (tag[1] != 369 || fseek(file, 369 * 4, SEEK_CUR)) break;
        } else if (tag[0] != HLE_CAPTURE_VCO_MAGIC && tag[0] != HLE_CAPTURE_T6_MAGIC &&
            (tag[0] < HLE_CAPTURE_F5_MAGIC || tag[0] >= HLE_CAPTURE_F5_MAGIC + 7u)) break;
    }
finish_cp0:
    clearerr(file); fseek(file, position, SEEK_SET); return found;
}
static inline int hle_capture_rsp(FILE *file, uint32_t *words, unsigned capacity)
{
    long position = ftell(file);
    uint32_t count, span[2], pair[2];
    unsigned k;
    int found = 0;
    if (position < 0) return 0;
    if (fread(&count, 4, 1, file) != 1 || count > HLE_CAPTURE_MAX_RANGES) goto finish_rsp;
    for (k = 0; k < count; k++) {
        if (fread(span, 8, 1, file) != 1 || span[0] >= 0x800000u ||
            span[1] > 0x800000u - span[0] || fseek(file, (long)span[1], SEEK_CUR)) goto finish_rsp;
    }
    for (k = 0; k < 10; k++) {
        if (fread(pair, 8, 1, file) != 1) break;
        if (pair[0] == HLE_CAPTURE_RSP_MAGIC) {
            if (pair[1] == capacity && fread(words, capacity * 4, 1, file) == 1) found = 1;
            break;
        }
        if (pair[0] != HLE_CAPTURE_VCO_MAGIC && pair[0] != HLE_CAPTURE_T6_MAGIC &&
            (pair[0] < HLE_CAPTURE_F5_MAGIC || pair[0] >= HLE_CAPTURE_F5_MAGIC + 7u)) break;
    }
finish_rsp:
    clearerr(file); fseek(file, position, SEEK_SET); return found;
}
static inline uint32_t hle_capture_value(FILE *file,uint32_t magic)
{
    long position=ftell(file);
    uint32_t count,span[2],context[2];
    uint32_t value=0;
    unsigned k;
    if(position<0)return 0;
    if(fread(&count,4,1,file)!=1 || count>HLE_CAPTURE_MAX_RANGES)goto finish;
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
