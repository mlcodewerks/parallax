/* NEAD's command DMA is observable: SEGMENT and buffer commands can
 * overwrite prefetched commands, and saves can alter future RDRAM chunks. */
#include "alist.h"
#include "hle_internal.h"
#include "hle_external.h"
#include "hle_timing.h"
#include "memory.h"
#include <string.h>
#ifdef HLE_ALIST_TRACE
#include "../../../../../benchmarks/abi1_trace_hook.h"
#endif

static uint32_t staged_word(struct hle_t* hle, uint32_t at)
{
    uint32_t value = 0, byte;
    for (byte = 0; byte < 4; ++byte)
        value = value << 8 | hle->alist_buffer[((at+byte)&0xfffu)^S8];
    return value;
}
void alist_nead_walk(struct hle_t* hle, const acmd_callback_t abi[],
    unsigned abi_size, uint16_t stage, uint16_t capacity)
{
    hle->alist_next = *dmem_u32(hle, TASK_DATA_PTR);
    hle->alist_remaining = *dmem_u32(hle, TASK_DATA_SIZE);
    hle->alist_processed = 0;
    hle->alist_chunk_capacity = capacity;
    do {
        hle->alist_chunk = (int32_t)hle->alist_remaining > capacity ? capacity : hle->alist_remaining;
        hle->alist_chunk_offset = 0;
        alist_dmem_dma(hle, stage, hle->alist_next, hle->alist_chunk-1u, 0);
        do {
            uint32_t at = stage+hle->alist_chunk_offset;
            uint32_t w1 = staged_word(hle, at), w2 = staged_word(hle, at+4u);
            unsigned acmd = (w1 >> 24) & 0x7f;
            hle->alist_next += 8;hle->alist_remaining -= 8;
            hle->alist_chunk -= 8;hle->alist_chunk_offset += 8;
            hle->alist_processed += 8;
            hle_timing_command(acmd);
            if (acmd < abi_size) abi[acmd](hle, w1, w2);
            else HleWarnMessage(hle->user_defined, "Invalid NEAD command %u", acmd);
#ifdef HLE_ALIST_TRACE
            abi1_trace_command(hle, w1, w2);
#endif
        } while ((int32_t)hle->alist_chunk > 0);
    } while ((int32_t)hle->alist_remaining > 0);
    memcpy(hle->dmem, hle->alist_buffer, sizeof(hle->alist_buffer));
}
