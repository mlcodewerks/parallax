#ifndef M64P_RSP_TIMING_H
#define M64P_RSP_TIMING_H
#include <stdint.h>

/* RSP runs at 62.5 MHz, COUNT at 46.875 MHz. Deadlines round up, but
 * continuation slices retain the excess quarter ticks instead of paying
 * another rounded tick at every short CP0 polling yield. */
static inline uint32_t rsp_count_ticks(uint32_t cycles, uint32_t overhang)
{
    return cycles ? (uint32_t)(((uint64_t)cycles * 3 + 3 - (overhang & 3)) / 4) : 0;
}
static inline uint32_t rsp_count_overhang(uint32_t cycles, uint32_t overhang)
{
    return (overhang - cycles * 3u) & 3u;
}
#endif
