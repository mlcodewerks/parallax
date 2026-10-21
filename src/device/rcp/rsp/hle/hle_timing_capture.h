#include "core/file.h"
/* Opt-in native task captures for offline timing calibration. The file
 * contains a versioned header, host-native RDRAM, DMEM and IMEM. Native
 * cycles are accumulated until BREAK, including all execution slices. */
static const char *timing_capture_prefix;
static core_file *timing_capture_file;
static unsigned int timing_capture_started, timing_capture_count;
static unsigned int timing_capture_elapsed;
static uint32_t timing_capture_ranges[512][2];
static unsigned int timing_capture_nranges;
static int timing_capture_incomplete;
static uint16_t timing_capture_vco;
static uint32_t timing_capture_t6;
static uint32_t timing_capture_f5[7];
static uint32_t timing_capture_rsp[PARALLEL_RSP_STATE_WORDS];
static uint32_t timing_capture_cp0[16];
static unsigned int timing_capture_skip, timing_capture_stride;
#include "hle_capture_context.h"

void optional_rsp_timing_dma_write(uint32_t address, uint32_t bytes)
{
    unsigned int i;
    if (!timing_capture_file || !bytes || address >= 0x800000u || bytes > 0x800000u-address) return;
    for (i = 0; i < timing_capture_nranges; i++) {
        uint32_t begin = timing_capture_ranges[i][0], end = begin + timing_capture_ranges[i][1];
        if (address <= end && address + bytes >= begin) {
            if (address < begin) begin = address;
            if (address + bytes > end) end = address + bytes;
            timing_capture_ranges[i][0] = begin; timing_capture_ranges[i][1] = end-begin;
            return;
        }
    }
    if (timing_capture_nranges < 512) {
        timing_capture_ranges[timing_capture_nranges][0] = address;
        timing_capture_ranges[timing_capture_nranges++][1] = bytes;
    }
    else timing_capture_incomplete = 1;
}

static void timing_capture_begin(const uint32_t *task)
{
    uint32_t header[4] = { 0x484c5431u, 0, 0, 0 };
    char path[1024];
    unsigned int i;
    int boot;
    if (!timing_capture_prefix || (*info.SP_PC_REG & 0xfff) != 0 ||
        timing_capture_file || timing_capture_count >= 24) return;
    boot = task[3] > 0x1000;
    if (boot) {
        if (hle_get_task_kind(&hle) != HLE_TASK_BOOT) return;
        for (i = 0; i < 44; i++) header[2] += info.IMEM[i];
    } else {
        if ((task[1] & 1u) || task[4] > 0x7ff000 || task[6] > 0x7ff000 ||
            task[12] >= 0x800000 || task[13] > 0x800000-task[12]) return;
        for (i = 0; i < (task[5] < 0xf80 ? task[5] : 0xf80) / 2; i++)
            header[2] += info.RDRAM[task[4] + i];
        if (++timing_capture_started < timing_capture_skip || timing_capture_started % timing_capture_stride)
            if (header[2] != 0x278u) return;
    }
    /* A streaming server's task lifetime includes arbitrary CPU waits;
     * use the ROM instruction corpus for its active-work model instead. */
    if (header[2] == 0x28b9e || header[2] == 0x2095b || header[2] == 0x1f7bb ||
        header[2] == 0x25c16 || header[2] == 0x25c53) return;
    if (hle_get_task_kind(&hle) == HLE_TASK_UNKNOWN) return;
    if (!boot) header[3] = *(const uint32_t *)(info.RDRAM + task[6] + 0x10);
    snprintf(path, sizeof(path), "%s-%04u.rsp", timing_capture_prefix, timing_capture_count++);
    timing_capture_file = core_file_open(path, "wb+");
    if (!timing_capture_file) return;
    if (core_file_write_items(header, sizeof(header), 1, timing_capture_file) != 1 ||
        core_file_write_items(info.RDRAM, 0x800000, 1, timing_capture_file) != 1 ||
        core_file_write_items(info.DMEM, 0x1000, 1, timing_capture_file) != 1 ||
        core_file_write_items(info.IMEM, 0x1000, 1, timing_capture_file) != 1) {
        core_file_close(timing_capture_file); timing_capture_file = NULL; return;
    }
    timing_capture_elapsed = 0;
    timing_capture_vco=parallelRSPGetVCO();
    timing_capture_t6=parallelRSPGetT6();
    parallelRSPGetF5Context(timing_capture_f5);
    parallelRSPSaveState(timing_capture_rsp);
    capture_cp0(timing_capture_cp0);
    timing_capture_nranges = 0;
    timing_capture_incomplete = 0;
}
static void timing_capture_end(unsigned int cycles)
{
    unsigned int i;
    if (!timing_capture_file) return;
    if (cycles > 0x7fffffffu - timing_capture_elapsed) timing_capture_elapsed = 0x7fffffffu;
    else timing_capture_elapsed += cycles;
    if (!(*info.SP_STATUS_REG & SP_STATUS_BROKE)) return;
    if (timing_capture_incomplete) {
        fprintf(stderr, "HLE timing capture omitted: more than 512 DMA output ranges\n");
        timing_capture_elapsed = 0;
    }
    core_file_seek(timing_capture_file, 0, SEEK_END);
    core_file_write_items(&timing_capture_nranges, sizeof(timing_capture_nranges), 1, timing_capture_file);
    for (i = 0; i < timing_capture_nranges; i++) {
        core_file_write_items(timing_capture_ranges[i], sizeof(timing_capture_ranges[i]), 1, timing_capture_file);
        core_file_write_items(info.RDRAM + timing_capture_ranges[i][0], timing_capture_ranges[i][1], 1, timing_capture_file);
    }
    {
        uint32_t context[4]={HLE_CAPTURE_VCO_MAGIC,timing_capture_vco,HLE_CAPTURE_T6_MAGIC,timing_capture_t6};
        core_file_write_items(context,sizeof(context),1,timing_capture_file);
        for(i=0;i<7;i++){
            uint32_t pair[2]={HLE_CAPTURE_F5_MAGIC+i,timing_capture_f5[i]};
            core_file_write_items(pair,sizeof(pair),1,timing_capture_file);
        }
        { uint32_t tag[2]={HLE_CAPTURE_RSP_MAGIC,PARALLEL_RSP_STATE_WORDS};
          core_file_write_items(tag,sizeof(tag),1,timing_capture_file);
          core_file_write_items(timing_capture_rsp,sizeof(timing_capture_rsp),1,timing_capture_file); }
        hle_capture_write_cp0(timing_capture_file,timing_capture_cp0);
    }
    core_file_seek(timing_capture_file, 4, SEEK_SET);
    core_file_write_items(&timing_capture_elapsed, sizeof(timing_capture_elapsed), 1, timing_capture_file);
    core_file_close(timing_capture_file);
    timing_capture_file = NULL;
}
