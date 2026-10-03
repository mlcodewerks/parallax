/* Optional upstream task HLE. The native LLE RSP remains connected for boot,
 * non-task code, unknown microcodes, forced-LLE lists and streaming servers. */
#include "optional_rsp.h"
#include "renderer_options.h"
#include "device/device.h"
#include "main/main.h"
#include "api/callbacks.h"
#include "device/rcp/rdp/rdp_core.h"
#include "device/rcp/rdp/angrylion/rdp_emit_backend.h"
#include "device/rcp/rdp/angrylion/rdp_emit_hle.h"
#include "device/rcp/rdp/angrylion/n64video.h"
#include <stddef.h>
#include <string.h>
#include "hle/hle.h"
#include "hle/hle_external.h"

extern unsigned int parallelRSPDoRspCycles(unsigned int);
extern void parallelRSPResetExecutedCycles(void);
static struct hle_t hle;
static RSP_INFO info;
static unsigned int forwarded_cycles;
static unsigned int start_status;
static int forwarded;
static unsigned long long graphics_tasks, audio_tasks, lle_tasks;

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
    graphics_tasks = audio_tasks = lle_tasks = 0;
    hle_init(&hle, info.RDRAM, info.DMEM, info.IMEM, info.MI_INTR_REG,
        info.SP_MEM_ADDR_REG, info.SP_DRAM_ADDR_REG, info.SP_RD_LEN_REG,
        info.SP_WR_LEN_REG, info.SP_STATUS_REG, info.SP_DMA_FULL_REG,
        info.SP_DMA_BUSY_REG, info.SP_PC_REG, info.SP_SEMAPHORE_REG,
        info.DPC_START_REG, info.DPC_END_REG, info.DPC_CURRENT_REG,
        info.DPC_STATUS_REG, info.DPC_CLOCK_REG, info.DPC_BUFBUSY_REG,
        info.DPC_PIPEBUSY_REG, info.DPC_TMEM_REG, NULL);
    hle.hle_gfx = 1;
    /* hle_aud=0 selects internal audio HLE, not the dummy audio plugin. */
    hle.hle_aud = 0;
    rdp_emit_set_backend(&backend);
    rdp_emit_hle_reset();
}

static int graphics_supported(const uint32_t *task)
{
    unsigned int sum = 0, i;

    for (i = 0; i < (task[5] < 0xf80 ? task[5] : 0xf80) / 2; i++)
        sum += info.RDRAM[task[4] + i];
    if (sum == 0x28b9e || sum == 0x2095b || sum == 0x1f7bb ||
        sum == 0x25c16 || sum == 0x25c53) return 0;
    return rdp_emit_hle_supported(info.RDRAM, 0x800000, task[4], task[6], task[7]);
}

static int audio_supported(const uint32_t *task)
{
    const uint32_t *data = (const uint32_t *)(info.RDRAM + task[6]);
    /* MusyX relies on task layouts and vector residue not covered by this HLE
     * integration. The ROM matrix found a voice-stage crash on its v2 tasks;
     * keep both variants on the native engine until that path is validated. */
    return !((data[0] != 1 && data[4] == 1) ||
             (data[0] == 1 && data[12] != 0xf0000f00 && data[4] == 0x00010010));
}

unsigned int optional_rsp_execute(unsigned int cycles)
{
    const uint32_t *task = (const uint32_t *)(info.DMEM + 0xfc0);
    unsigned int type = task[0];
    if (!renderer_settings.rsp_hle || (*info.SP_PC_REG & 0xfff) != 0 ||
        (type != 1 && type != 2) || task[3] == 0 || task[3] > 0x1000 ||
        (task[4] & 3) || task[4] > 0x7ff000 ||
        (task[6] & 3) || task[6] > 0x7ff000 || task[7] > 0x1000 ||
        (task[12] & 7) || task[12] >= 0x800000 || task[13] > 0x800000 - task[12] ||
        (type == 1 && (renderer_settings.lle_graphics ||
            (*info.DPC_STATUS_REG & DPC_STATUS_FREEZE) || !graphics_supported(task))) ||
        (type == 2 && (renderer_settings.lle_audio || !audio_supported(task))))
    {
        lle_tasks++;
        return parallelRSPDoRspCycles(cycles);
    }
    forwarded = 0;
    start_status = *info.SP_STATUS_REG;
    parallelRSPResetExecutedCycles();
    hle_execute(&hle);
    if (forwarded) lle_tasks++;
    else if (type == 1) graphics_tasks++;
    else audio_tasks++;
    /* HLE has no executed instruction stream: these are task-level estimates,
     * not hardware cycle accuracy. The shared core schedules completion. */
    return forwarded ? forwarded_cycles : (type == 2 ? 5334 : 1334);
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
    DebugMessage(M64MSG_INFO, "RSP tasks: HLE graphics=%llu audio=%llu LLE=%llu",
        graphics_tasks, audio_tasks, lle_tasks);
}

/* Persist guest-derived audio data only; never serialize host pointers or the
 * function-pointer microcode cache. Graphics task state is reset per task. */
#define AUDIO_STATE_OFFSET offsetof(struct hle_t, alist_buffer)
#define AUDIO_STATE_BYTES (offsetof(struct hle_t, cached_ucodes) - AUDIO_STATE_OFFSET)
unsigned int optional_rsp_state_size(void) { return AUDIO_STATE_BYTES + rdp_emit_hle_state_size() + n64video_noise_state_size(); }
void optional_rsp_save(void *buffer)
{
    memcpy(buffer, (const unsigned char *)&hle + AUDIO_STATE_OFFSET, AUDIO_STATE_BYTES);
    rdp_emit_hle_save((unsigned char *)buffer + AUDIO_STATE_BYTES);
    n64video_save_noise((unsigned char *)buffer + AUDIO_STATE_BYTES + rdp_emit_hle_state_size());
}
void optional_rsp_load(const void *buffer)
{
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
