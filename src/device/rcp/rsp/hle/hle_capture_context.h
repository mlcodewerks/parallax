/* Optional context trailer on private task captures. Old captures imply
 * zero carry, phase and retained lanes. Range layout remains unchanged. */
#ifndef HLE_CAPTURE_CONTEXT_H
#define HLE_CAPTURE_CONTEXT_H
#include <stdint.h>
#include <stdio.h>
#ifdef __LIBRETRO__
#include "core/file.h"
typedef core_file hle_capture_file;
#define capture_read core_file_read_items
#define capture_write core_file_write_items
#define capture_seek core_file_seek
#define capture_tell core_file_tell
#define capture_clear core_file_clear_error
#else
/* Standalone reference tools own their host files, outside the libretro core. */
typedef FILE hle_capture_file;
#define capture_read fread
#define capture_write fwrite
#define capture_seek fseek
#define capture_tell ftell
#define capture_clear clearerr
#endif
#define HLE_CAPTURE_VCO_MAGIC 0x56434f31u
#define HLE_CAPTURE_T6_MAGIC 0x46355436u
#define HLE_CAPTURE_F5_MAGIC 0x46355600u
#define HLE_CAPTURE_RSP_MAGIC 0x52535031u
#define HLE_CAPTURE_CP0_MAGIC 0x43503031u
/* Offline references merge DMA writes; sparse video output may exceed the
 * live capture logger's 512-entry array. Eight bytes is the minimum DMA. */
#define HLE_CAPTURE_MAX_RANGES (0x800000u / 8u)
static inline void hle_capture_write_cp0(hle_capture_file *file, const uint32_t words[16])
{
    uint32_t tag[2] = { HLE_CAPTURE_CP0_MAGIC, 16 };
    capture_write(tag, sizeof(tag), 1, file);
    capture_write(words, 16 * 4, 1, file);
}
static inline int hle_capture_cp0(hle_capture_file *file, uint32_t words[16])
{
    int64_t position = capture_tell(file);
    uint32_t count, span[2], tag[2];
    unsigned k;
    int found = 0;
    if (position < 0) return 0;
    if (capture_read(&count, 4, 1, file) != 1 || count > HLE_CAPTURE_MAX_RANGES) goto finish_cp0;
    for (k = 0; k < count; ++k) {
        if (capture_read(span, 8, 1, file) != 1 || span[0] >= 0x800000u ||
            span[1] > 0x800000u-span[0] || capture_seek(file, (long)span[1], SEEK_CUR)) goto finish_cp0;
    }
    for (k = 0; k < 12; ++k) {
        if (capture_read(tag, 8, 1, file) != 1) break;
        if (tag[0] == HLE_CAPTURE_CP0_MAGIC) {
            found = tag[1] == 16 && capture_read(words, 16 * 4, 1, file) == 1;
            break;
        }
        if (tag[0] == HLE_CAPTURE_RSP_MAGIC) {
            if (tag[1] != 369 || capture_seek(file, 369 * 4, SEEK_CUR)) break;
        } else if (tag[0] != HLE_CAPTURE_VCO_MAGIC && tag[0] != HLE_CAPTURE_T6_MAGIC &&
            (tag[0] < HLE_CAPTURE_F5_MAGIC || tag[0] >= HLE_CAPTURE_F5_MAGIC + 7u)) break;
    }
finish_cp0:
    capture_clear(file); capture_seek(file, position, SEEK_SET); return found;
}
static inline int hle_capture_rsp(hle_capture_file *file, uint32_t *words, unsigned capacity)
{
    int64_t position = capture_tell(file);
    uint32_t count, span[2], pair[2];
    unsigned k;
    int found = 0;
    if (position < 0) return 0;
    if (capture_read(&count, 4, 1, file) != 1 || count > HLE_CAPTURE_MAX_RANGES) goto finish_rsp;
    for (k = 0; k < count; k++) {
        if (capture_read(span, 8, 1, file) != 1 || span[0] >= 0x800000u ||
            span[1] > 0x800000u - span[0] || capture_seek(file, (long)span[1], SEEK_CUR)) goto finish_rsp;
    }
    for (k = 0; k < 10; k++) {
        if (capture_read(pair, 8, 1, file) != 1) break;
        if (pair[0] == HLE_CAPTURE_RSP_MAGIC) {
            if (pair[1] == capacity && capture_read(words, capacity * 4, 1, file) == 1) found = 1;
            break;
        }
        if (pair[0] != HLE_CAPTURE_VCO_MAGIC && pair[0] != HLE_CAPTURE_T6_MAGIC &&
            (pair[0] < HLE_CAPTURE_F5_MAGIC || pair[0] >= HLE_CAPTURE_F5_MAGIC + 7u)) break;
    }
finish_rsp:
    capture_clear(file); capture_seek(file, position, SEEK_SET); return found;
}
static inline uint32_t hle_capture_value(hle_capture_file *file,uint32_t magic)
{
    long position=capture_tell(file);
    uint32_t count,span[2],context[2];
    uint32_t value=0;
    unsigned k;
    if(position<0)return 0;
    if(capture_read(&count,4,1,file)!=1 || count>HLE_CAPTURE_MAX_RANGES)goto finish;
    for(k=0;k<count;k++){
        if(capture_read(span,8,1,file)!=1 || span[0]>=0x800000u || span[1]>0x800000u-span[0] ||
           capture_seek(file,(long)span[1],SEEK_CUR))goto finish;
    }
    for(k=0;k<9;k++){
        if(capture_read(context,8,1,file)!=1)break;
        if(context[0]==magic){value=context[1];break;}
        if(context[0]!=HLE_CAPTURE_VCO_MAGIC && context[0]!=HLE_CAPTURE_T6_MAGIC &&
           (context[0]<HLE_CAPTURE_F5_MAGIC || context[0]>=HLE_CAPTURE_F5_MAGIC+7u))break;
    }
finish:
    capture_clear(file);capture_seek(file,position,SEEK_SET);return value;
}
static inline uint16_t hle_capture_vco(hle_capture_file *file){
    uint32_t value=hle_capture_value(file,HLE_CAPTURE_VCO_MAGIC);
    return value<=0xffffu?(uint16_t)value:0;
}
static inline uint32_t hle_capture_t6(hle_capture_file *file){return hle_capture_value(file,HLE_CAPTURE_T6_MAGIC);}
static inline void hle_capture_f5(hle_capture_file *file,uint32_t values[7]){
    unsigned k;for(k=0;k<7;k++)values[k]=hle_capture_value(file,HLE_CAPTURE_F5_MAGIC+k);
}
#undef capture_read
#undef capture_write
#undef capture_seek
#undef capture_tell
#undef capture_clear
#endif
