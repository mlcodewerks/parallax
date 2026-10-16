/* Work-based RSP clock estimates. Counters describe executed work, never
 * host wall time; resets happen once per HLE invocation. */
#ifndef HLE_TIMING_H
#define HLE_TIMING_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum hle_work_kind {
    HLE_WORK_COMMAND, HLE_WORK_DMA, HLE_WORK_COPY, HLE_WORK_ADPCM,
    HLE_WORK_RESAMPLE, HLE_WORK_ENVMIX, HLE_WORK_MIX, HLE_WORK_FILTER,
    HLE_WORK_VERTEX, HLE_WORK_TRIANGLE, HLE_WORK_MATRIX, HLE_WORK_VOICE,
    HLE_WORK_EFFECT, HLE_WORK_BLOCK, HLE_WORK_PIXEL, HLE_WORK_MP3,
    HLE_WORK_RDP, HLE_WORK_LIGHT, HLE_WORK_MATRIX_MUL, HLE_WORK_DMA_SETUP,
    HLE_WORK_INDEX,
    HLE_WORK_COUNT
};
struct hle_timing_work {
    uint32_t units[HLE_WORK_COUNT];
    uint32_t override_cycles;
    uint32_t ucode_checksum;
    const char *family;
};
void hle_timing_begin(void);
void hle_timing_ucode(uint32_t checksum);
void hle_timing_family(const char *family);
void hle_timing_graphics_family(const char *family);
void hle_timing_add(enum hle_work_kind kind, uint32_t units);
void hle_timing_override(uint32_t cycles);
uint32_t hle_timing_cycles(void);
void hle_timing_set_slice_limit(uint32_t cycles);
uint32_t hle_timing_slice_limit(void);
struct hle_timing_work hle_timing_get(void);
void hle_timing_restore(struct hle_timing_work saved);
#ifdef __cplusplus
}
#endif
#endif
