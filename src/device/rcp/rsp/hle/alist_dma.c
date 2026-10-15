#include "alist.h"
#include "hle_internal.h"
#include "hle_timing.h"
#include "memory.h"

/* Completed SP DMA, including row rounding, bank wrapping and residue. */
void alist_dmem_dma(struct hle_t* hle, uint32_t dmem, uint32_t address,
    uint32_t encoded, int write)
{
    uint32_t length = ((encoded & 0xfffu) | 7u) + 1u;
    uint32_t rows = ((encoded >> 12) & 255u) + 1u;
    uint32_t skip = (encoded >> 20) & 0xff8u;
    uint32_t da = dmem & 0xff8u, ra = address & 0xfffff8u, row, byte;
    unsigned char* bank = (dmem & 0x1000u) ? hle->imem : hle->alist_buffer;
    hle_timing_add(HLE_WORK_DMA_SETUP, 1);
    hle_timing_add(HLE_WORK_DMA, length / 8u * rows);
    for (row = 0; row < rows; ++row) {
        for (byte = 0; byte < length; ++byte) {
            uint32_t d = (da + byte) & 0xfffu, r = (ra + byte) & 0x7fffffu;
            if (write) hle->dram[r ^ S8] = bank[d ^ S8];
            else {
                bank[d ^ S8] = hle->dram[r ^ S8];
                if (!(dmem & 0x1000u)) hle->dmem[d ^ S8] = bank[d ^ S8];
            }
        }
        da = (da + length) & 0xfffu;
        ra += length + (row + 1u < rows ? skip : 0u);
    }
    *hle->sp_mem_addr = (dmem & 0x1000u) | da;*hle->sp_dram_addr = ra & 0xffffffu;
    *hle->sp_rd_length = *hle->sp_wr_length = (encoded & 0xff800000u) | 0xff8u;
    *hle->sp_semaphore = 0;
}
