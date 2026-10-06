/* Optional task HLE and streaming graphics walkers. The native RSP remains
 * connected for unknown code, forced-LLE lists and unsupported overlays. */
#include "optional_rsp.h"
#include "renderer_options.h"
#include "device/device.h"
#include "main/main.h"
#include "api/callbacks.h"
#include "device/rcp/rdp/rdp_core.h"
#include "device/rcp/rdp/angrylion/rdp_emit_backend.h"
#include "device/rcp/rdp/angrylion/rdp_emit_hle.h"
#include "device/rcp/rdp/angrylion/rdp_emit_zsort.h"
#include "device/rcp/rdp/angrylion/n64video.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle/hle.h"
#include "device/rcp/rdp/angrylion/rdp_emit_state.h"
#include "hle/musyx_validation.h"
#include "hle/hle_external.h"

extern unsigned int parallelRSPDoRspCycles(unsigned int);
extern void parallelRSPResetExecutedCycles(void);
static struct hle_t hle;
static RSP_INFO info;
static unsigned int forwarded_cycles;
static unsigned int start_status;
static int forwarded;
static unsigned long long graphics_tasks, audio_tasks, other_tasks, boot_tasks, lle_tasks;

static unsigned char *get_rdram(void) { return info.RDRAM; }
static unsigned char *get_dmem(void) { return info.DMEM; }
static unsigned int get_rdram_size(void) { return 0x800000; }
static void submit(const unsigned char *storage, unsigned int base, unsigned int bytes)
{
    rdp_submit_hle(&g_dev.dp, (const uint32_t *)storage, base, bytes);
}
static const RdpEmitBackend backend = {get_rdram, get_rdram_size, get_dmem, submit};

void optional_rsp_init(RSP_INFO rsp_info)
{
    info = rsp_info;
    memset(&hle, 0, sizeof(hle));
    graphics_tasks = audio_tasks = other_tasks = boot_tasks = lle_tasks = 0;
    hle_init(&hle, info.RDRAM, info.DMEM, info.IMEM, info.MI_INTR_REG,
        info.SP_MEM_ADDR_REG, info.SP_DRAM_ADDR_REG, info.SP_RD_LEN_REG,
        info.SP_WR_LEN_REG, info.SP_STATUS_REG, info.SP_DMA_FULL_REG,
        info.SP_DMA_BUSY_REG, info.SP_PC_REG, info.SP_SEMAPHORE_REG,
        info.DPC_START_REG, info.DPC_END_REG, info.DPC_CURRENT_REG,
        info.DPC_STATUS_REG, info.DPC_CLOCK_REG, info.DPC_BUFBUSY_REG,
        info.DPC_PIPEBUSY_REG, info.DPC_TMEM_REG, NULL);
    hle.hle_gfx = 1;
    hle.hle_aud = 0;
    rdp_emit_set_backend(&backend);
    rdp_emit_hle_reset();
    rdp_emit_stream_load(NULL);
    hle_stream_state_load(NULL);
}

static int graphics_supported(const uint32_t *task)
{
    unsigned int sum = 0, i;

    if (zsort_ucode_match(info.RDRAM, 0x800000, task[6], task[7]))
    {
        /* Optional local capture for RSP disassembly/oracle tests. RDRAM
         * words retain the core's host-native byte order in this file. */
        const char *path = getenv("HLE_ZSORT_UCODE_DUMP");
        static int captured;
        if (path && !captured)
        {
            FILE *file = fopen(path, "wb");
            if (file)
            {
                unsigned int text_bytes = task[5] < 0xf80 ? task[5] : 0xf80;
                fwrite(info.RDRAM + task[4], 1, text_bytes, file);
                fwrite(info.RDRAM + task[6], 1, task[7], file);
                fclose(file);
                captured = 1;
                DebugMessage(M64MSG_INFO, "ZSortp capture: text %08x/%x data %08x/%x",
                    task[4], text_bytes, task[6], task[7]);
            }
        }
        return zsort_validate(info.RDRAM, 0x800000, info.DMEM, task[12] & 0xffffff);
    }

    for (i = 0; i < (task[5] < 0xf80 ? task[5] : 0xf80) / 2; i++)
        sum += info.RDRAM[task[4] + i];
    if (sum == 0x28b9e || sum == 0x2095b || sum == 0x1f7bb ||
        sum == 0x25c16 || sum == 0x25c53)
        return !(task[1] & 1) || hle_stream_can_resume(sum);
    return rdp_emit_hle_supported(info.RDRAM, 0x800000, task[4], task[6], task[7]);
}

static int audio_supported(const uint32_t *task)
{
    const uint32_t *data = (const uint32_t *)(info.RDRAM + task[6]);
    /* Match upstream hle.c's try_audio_task_detection branch order: ABI1
     * takes precedence over the v2 signature; v1 is in the non-ABI1 branch.
     * Validate every task, even when the dispatcher has cached its ucode. */
    if (data[0] == 1) {
        if (data[12] != 0xf0000f00 && data[4] == 0x00010010)
            return musyx_v2_supported(info.RDRAM, task[12], task[13]);
    } else if (data[4] == 1)
        return musyx_v1_supported(info.RDRAM, task[12], task[13]);
    return 1;
}

unsigned int optional_rsp_execute(unsigned int cycles)
{
    const uint32_t *task = (const uint32_t *)(info.DMEM + 0xfc0);
    enum hle_task_kind kind;
    if (!renderer_settings.rsp_hle || (*info.SP_PC_REG & 0xfff) != 0)
        goto native;
    /* Upstream identifies direct programs by boot size. Their DMEM tail is
     * not an OSTask and must not be subjected to guest-pointer checks. */
    if (task[3] <= 0x1000 && (
        (task[4] & 3) || task[4] > 0x7ff000 ||
        (task[6] & 3) || task[6] > 0x7ff000 || task[7] > 0x1000 ||
        (task[12] & 3) || task[12] >= 0x800000 || task[13] > 0x800000 - task[12]))
        goto native;
    kind = hle_get_task_kind(&hle);
    if (kind == HLE_TASK_UNKNOWN ||
        (kind == HLE_TASK_GRAPHICS && (renderer_settings.lle_graphics ||
            (*info.DPC_STATUS_REG & DPC_STATUS_FREEZE) || !graphics_supported(task))) ||
        (kind == HLE_TASK_AUDIO && ((task[12] & 7) ||
            renderer_settings.lle_audio || !audio_supported(task))))
        goto native;
    forwarded = 0;
    start_status = *info.SP_STATUS_REG;
    parallelRSPResetExecutedCycles();
    hle_execute(&hle);
    if (forwarded) lle_tasks++;
    else if (kind == HLE_TASK_GRAPHICS) graphics_tasks++;
    else if (kind == HLE_TASK_AUDIO) audio_tasks++;
    else if (kind == HLE_TASK_BOOT) boot_tasks++;
    else other_tasks++;
    return forwarded ? forwarded_cycles : (kind == HLE_TASK_AUDIO ? 5334 : 1334);
native:
    lle_tasks++;
    return parallelRSPDoRspCycles(cycles);
}

void HleVerboseMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleInfoMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleErrorMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleWarnMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleCheckInterrupts(void *u) { (void)u; info.CheckInterrupts(); }
void HleProcessDlistList(void *u) { (void)u; rdp_emit_hle_process_dlist(); }
void HleProcessAlistList(void *u) { (void)u; }
void HleProcessRdpList(void *u) { (void)u; info.ProcessRdpList(); }
void HleShowCFB(void *u) { (void)u; info.ShowCFB(); }
int HleForwardTask(void *u)
{
    (void)u;
    *info.SP_STATUS_REG = start_status;
    forwarded_cycles = parallelRSPDoRspCycles(~0u);
    forwarded = 1;
    return 0;
}
void optional_rsp_report(void)
{
    DebugMessage(M64MSG_INFO, "RSP tasks: HLE graphics=%llu audio=%llu LLE=%llu other=%llu boot=%llu",
        graphics_tasks, audio_tasks, lle_tasks, other_tasks, boot_tasks);
}

#define AUDIO_STATE_OFFSET offsetof(struct hle_t, alist_buffer)
#define AUDIO_STATE_BYTES (offsetof(struct hle_t, cached_ucodes) - AUDIO_STATE_OFFSET)
static unsigned int legacy_state_size(void) { return AUDIO_STATE_BYTES + rdp_emit_hle_state_size() + n64video_noise_state_size(); }
unsigned int optional_rsp_state_size(void) { return legacy_state_size() + rdp_emit_stream_state_size() + hle_stream_state_size(); }
void optional_rsp_save(void *buffer)
{
    memcpy(buffer, (const unsigned char *)&hle + AUDIO_STATE_OFFSET, AUDIO_STATE_BYTES);
    rdp_emit_hle_save((unsigned char *)buffer + AUDIO_STATE_BYTES);
    n64video_save_noise((unsigned char *)buffer + AUDIO_STATE_BYTES + rdp_emit_hle_state_size());
    rdp_emit_stream_save((unsigned char *)buffer + legacy_state_size());
    hle_stream_state_save((unsigned char *)buffer + legacy_state_size() + rdp_emit_stream_state_size());
}
void optional_rsp_load_legacy(const void *buffer)
{
    rdp_emit_stream_load(NULL);
    hle_stream_state_load(NULL);
    if (!buffer) {
        memset((unsigned char *)&hle + AUDIO_STATE_OFFSET, 0, AUDIO_STATE_BYTES);
        memset(&hle.cached_ucodes, 0, sizeof(hle.cached_ucodes));
        rdp_emit_hle_reset();
        return;
    }
    memcpy((unsigned char *)&hle + AUDIO_STATE_OFFSET, buffer, AUDIO_STATE_BYTES);
    memset(&hle.cached_ucodes, 0, sizeof(hle.cached_ucodes));
    rdp_emit_hle_load((const unsigned char *)buffer + AUDIO_STATE_BYTES);
    n64video_load_noise((const unsigned char *)buffer + AUDIO_STATE_BYTES + rdp_emit_hle_state_size());
}

void optional_rsp_load(const void *buffer)
{
    optional_rsp_load_legacy(buffer);
    if (!buffer) return;
    rdp_emit_stream_load((const unsigned char *)buffer + legacy_state_size());
    hle_stream_state_load((const unsigned char *)buffer + legacy_state_size() + rdp_emit_stream_state_size());
}
