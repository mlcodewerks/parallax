/* Algorithm-level command loading for the unshifted NAudio builds.
 * Slots 7/8 enter IMEM zero. Only boot 000..07c survives the resident
 * text DMA: its flags-clear branch reaches resident 08c and refills the
 * commands from the current gp/k1, without reloading text or data.
 * flags&2 takes boot 074 and then resident 080, restarting the task list.
 * The boot's 03c check can end that path on SIG0 (status write 5200). */
#include "alist.h"
#include "hle_internal.h"
#include "hle_external.h"
#include "hle_timing.h"
#include "memory.h"
#include <string.h>

void alist_naudio_refill(struct hle_t* hle)
{
    uint32_t count = (int32_t)hle->alist_remaining > 0x140
        ? 0x140u : hle->alist_remaining;
    alist_dmem_dma(hle, 0x2b0, hle->alist_next, count - 1u, 0);
    hle->alist_chunk = count;
    hle->alist_chunk_offset = 0;
    /* The wait at 0bc reads SP_DMA_BUSY into v0; the ready value is zero. */
    hle->alist_v0 = 0;
}

void alist_naudio_reenter(struct hle_t* hle)
{
    if (*dmem_u32(hle, TASK_FLAGS) & 2u) {
        if (*hle->sp_status & SP_STATUS_SIG0) {
            *hle->sp_semaphore = 0;
            *hle->sp_status &= ~SP_STATUS_SIG0;
            rsp_break(hle, SP_STATUS_SIG1 | SP_STATUS_TASKDONE);
            *hle->sp_pc = 0x60;
            hle->alist_stop = 1;
            return;
        }
        hle->alist_next = *dmem_u32(hle, TASK_DATA_PTR);
        hle->alist_remaining = *dmem_u32(hle, TASK_DATA_SIZE);
        hle->alist_processed = 0;
    }
    alist_naudio_refill(hle);
}

static uint32_t command_word(struct hle_t* hle, uint32_t at)
{
    uint32_t i, value = 0;
    for (i = 0; i < 4; ++i)
        value = value << 8 | hle->alist_buffer[((at + i) & 0xfffu) ^ S8];
    return value;
}

void alist_naudio_walk(struct hle_t* hle, const acmd_callback_t abi[], int banjo)
{
    hle->alist_next = *dmem_u32(hle, TASK_DATA_PTR);
    hle->alist_remaining = *dmem_u32(hle, TASK_DATA_SIZE);
    hle->alist_processed = 0;
    hle->alist_stop = 0;
    alist_naudio_refill(hle);
    for (;;) {
        uint32_t at = 0x2b0u + hle->alist_chunk_offset;
        uint32_t w1 = command_word(hle, at), w2 = command_word(hle, at + 4u);
        unsigned acmd = (w1 >> 24) & 0x7f;
        hle->alist_next += 8;
        hle->alist_remaining -= 8;
        hle->alist_processed += 8;
        hle->alist_chunk -= 8;
        hle->alist_chunk_offset += 8;
        hle_timing_add(HLE_WORK_COMMAND, 1);
        if (banjo && acmd == 0x0e) {
            /* 02b0 is inside SETVOL, after v0 was derived. */
            hle->alist_naudio.rate[1] = (int32_t)((hle->alist_v0 << 16) | (w2 & 0xffff));
            hle->alist_buffer[0xfe8 ^ S8] = hle->dmem[0xfe8 ^ S8] = (uint8_t)(hle->alist_v0 >> 8);
            hle->alist_buffer[0xfe9 ^ S8] = hle->dmem[0xfe9 ^ S8] = (uint8_t)hle->alist_v0;
            hle->alist_buffer[0xfea ^ S8] = hle->dmem[0xfea ^ S8] = (uint8_t)(w2 >> 8);
            hle->alist_buffer[0xfeb ^ S8] = hle->dmem[0xfeb ^ S8] = (uint8_t)w2;
            {
                unsigned byte;
                for (byte = 0; byte < 4; ++byte)
                    hle->alist_naudio.state_scratch[0x48+byte] = hle->alist_buffer[(0xfe8+byte)^S8];
            }
        } else if (acmd < 16) {
            abi[acmd](hle, w1, w2);
                if (acmd == 0x0b) hle->alist_v0 = w2 & 0xffffff;
        } else {
            HleWarnMessage(hle->user_defined, "Invalid NAudio command %u", acmd);
        }
        if (hle->alist_stop) return;
        if ((int32_t)hle->alist_chunk > 0) continue;
        if ((int32_t)hle->alist_remaining <= 0) break;
        alist_naudio_refill(hle);
    }
    memcpy(hle->dmem, hle->alist_buffer, sizeof(hle->alist_buffer));
    rsp_break(hle, SP_STATUS_TASKDONE);
}
