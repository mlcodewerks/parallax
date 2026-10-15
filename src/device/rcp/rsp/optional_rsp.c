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
#include "device/rcp/rdp/angrylion/rdp_emit_naboo.h"
#include "device/rcp/rdp/angrylion/n64video.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle/hle.h"
#include "device/rcp/rdp/angrylion/rdp_emit_state.h"
#include "hle/musyx_validation.h"
#include "hle/hle_external.h"
#include "hle/hle_timing.h"

extern unsigned int parallelRSPDoRspCycles(unsigned int);
extern void parallelRSPResetExecutedCycles(void);
extern uint16_t parallelRSPGetVCO(void);
extern void parallelRSPSetVCO(uint16_t);
extern uint32_t parallelRSPGetT6(void);
extern void parallelRSPSetT6(uint32_t);
extern void parallelRSPGetF5Context(uint32_t values[7]);
extern void parallelRSPSetF5Context(const uint32_t values[7],uint32_t mask);
static struct hle_t hle;
static RSP_INFO info;
static unsigned int forwarded_cycles;
static unsigned int start_status;
static int forwarded;
static unsigned long long graphics_tasks, audio_tasks, other_tasks, boot_tasks, lle_tasks;
static int timing_probe_active, timing_probe_failed;
static uint32_t timing_probe_t6;
static uint32_t timing_probe_f5[7];
void optional_rsp_timing_f5(uint32_t *values){memcpy(values,timing_probe_f5,sizeof(timing_probe_f5));}
uint32_t optional_rsp_timing_t6(void){return timing_probe_t6;}

#include "hle/hle_timing_capture.h"

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
    if (timing_capture_file) { fclose(timing_capture_file); timing_capture_file = NULL; }
    timing_capture_prefix = getenv("HLE_TIMING_CAPTURE");
    timing_capture_skip = getenv("HLE_TIMING_CAPTURE_SKIP") ? (unsigned int)atoi(getenv("HLE_TIMING_CAPTURE_SKIP")) : 16u;
    timing_capture_stride = getenv("HLE_TIMING_CAPTURE_STRIDE") ? (unsigned int)atoi(getenv("HLE_TIMING_CAPTURE_STRIDE")) : 13u;
    if (!timing_capture_stride) timing_capture_stride = 1u;
    timing_capture_started = timing_capture_count = timing_capture_elapsed = 0;
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
    hle.rsp_vco=parallelRSPGetVCO();hle.rsp_vco_written=0;
    hle.rsp_t6=parallelRSPGetT6();hle.rsp_t6_written=0;naboo_set_phase(hle.rsp_t6);
    parallelRSPGetF5Context(hle.rsp_f5);hle.rsp_f5_written=0;naboo_set_context(hle.rsp_f5);
    hle_execute(&hle);
    if(!forwarded && hle.rsp_vco_written)parallelRSPSetVCO(hle.rsp_vco);
    if(!forwarded){
        if(kind==HLE_TASK_GRAPHICS)hle.rsp_t6_written=naboo_get_phase(&hle.rsp_t6);
        if(hle.rsp_t6_written)parallelRSPSetT6(hle.rsp_t6);
        if(kind==HLE_TASK_GRAPHICS)hle.rsp_f5_written=naboo_get_context(hle.rsp_f5);
        if(hle.rsp_f5_written)parallelRSPSetF5Context(hle.rsp_f5,hle.rsp_f5_written);
    }
    if (forwarded) lle_tasks++;
    else if (kind == HLE_TASK_GRAPHICS) graphics_tasks++;
    else if (kind == HLE_TASK_AUDIO) audio_tasks++;
    else if (kind == HLE_TASK_BOOT) boot_tasks++;
    else other_tasks++;
    return forwarded ? forwarded_cycles : hle_timing_cycles();
native:
    timing_capture_begin(task);
    lle_tasks++;
    cycles = parallelRSPDoRspCycles(cycles);
    timing_capture_end(cycles);
    return cycles;
}

void HleVerboseMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleInfoMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleErrorMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleWarnMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleCheckInterrupts(void *u) { (void)u; if (!timing_probe_active) info.CheckInterrupts(); }
void HleProcessDlistList(void *u)
{
    (void)u;
    naboo_set_clock(timing_probe_active || !info.DPC_CLOCK_REG ? 0u : *info.DPC_CLOCK_REG);
    rdp_emit_hle_process_dlist();
}
void HleProcessAlistList(void *u) { (void)u; }
void HleProcessRdpList(void *u) { (void)u; if (!timing_probe_active) info.ProcessRdpList(); }
void HleShowCFB(void *u) { (void)u; if (!timing_probe_active) info.ShowCFB(); }
int HleForwardTask(void *u)
{
    (void)u;
    if (timing_probe_active) { timing_probe_failed = 1; return 1; }
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
static unsigned int nead_state_offset(void) { return legacy_state_size() + rdp_emit_stream_state_size() + hle_stream_state_size(); }
unsigned int optional_rsp_state_size(void) { return nead_state_offset() + sizeof(hle.nead_v31); }
void optional_rsp_save(void *buffer)
{
    memcpy(buffer, (const unsigned char *)&hle + AUDIO_STATE_OFFSET, AUDIO_STATE_BYTES);
    rdp_emit_hle_save((unsigned char *)buffer + AUDIO_STATE_BYTES);
    n64video_save_noise((unsigned char *)buffer + AUDIO_STATE_BYTES + rdp_emit_hle_state_size());
    rdp_emit_stream_save((unsigned char *)buffer + legacy_state_size());
    hle_stream_state_save((unsigned char *)buffer + legacy_state_size() + rdp_emit_stream_state_size());
    memcpy((unsigned char *)buffer + nead_state_offset(), hle.nead_v31, sizeof(hle.nead_v31));
}
void optional_rsp_load_legacy(const void *buffer)
{
    memset(hle.nead_v31, 0, sizeof(hle.nead_v31));
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

void optional_rsp_load_v116(const void *buffer)
{
    optional_rsp_load_legacy(buffer);
    if (!buffer) return;
    rdp_emit_stream_load((const unsigned char *)buffer + legacy_state_size());
    hle_stream_state_load((const unsigned char *)buffer + legacy_state_size() + rdp_emit_stream_state_size());
}

void optional_rsp_load(const void *buffer)
{
    optional_rsp_load_v116(buffer);
    if (buffer) memcpy(hle.nead_v31, (const unsigned char *)buffer + nead_state_offset(), sizeof(hle.nead_v31));
}

void optional_rsp_load_legacy_stream(const void *buffer)
{
    optional_rsp_load_legacy(buffer);
    if (!buffer) return;
    rdp_emit_stream_load_legacy((const unsigned char *)buffer + legacy_state_size());
    hle_stream_state_load((const unsigned char *)buffer + legacy_state_size() + rdp_emit_stream_legacy_state_size());
}

/* Benchmark-only caller: replay the HLE on private RAM and local CP0
 * registers, restore every live walker afterwards, and report its work.
 * This does not run or submit native graphics a second time. */
static unsigned char *probe_ram, *probe_dmem;
static uint64_t probe_rdp_hash;
static unsigned int probe_rdp_words;
void optional_rsp_timing_rdp(uint64_t *hash, unsigned int *words)
{ *hash = probe_rdp_hash; *words = probe_rdp_words; }
static unsigned char *probe_get_ram(void) { return probe_ram; }
static unsigned char *probe_get_dmem(void) { return probe_dmem; }
static void probe_submit(const unsigned char *p, unsigned int base, unsigned int bytes)
{
    unsigned int i, shift;
    FILE *dump = getenv("HLE_TIMING_RDP_DUMP") ? fopen(getenv("HLE_TIMING_RDP_DUMP"), "ab") : NULL;
    (void)base;
    for (i = 0; i + 4u <= bytes; i += 4u) {
        uint32_t word; memcpy(&word, p + i, 4);
        for (shift = 24; ; shift -= 8) {
            probe_rdp_hash = (probe_rdp_hash ^ ((word >> shift) & 255u)) * UINT64_C(1099511628211);
            if (!shift) break;
        }
        probe_rdp_words++;
    }
    if (dump) { fwrite(p, bytes, 1, dump); fclose(dump); }
}
static const RdpEmitBackend probe_backend = {probe_get_ram, get_rdram_size, probe_get_dmem, probe_submit};

unsigned int optional_rsp_timing_replay(const char *path, char *family,
    uint32_t *units, unsigned int *estimate, unsigned int *diffs, unsigned int *written)
{
    FILE *f = fopen(path, "rb");
    uint32_t header[4], regs[32] = {0};
    unsigned char *memory = NULL, *snapshot = NULL;
    struct hle_t probe;
    struct hle_timing_work work;
    probe_rdp_hash = UINT64_C(14695981039346656037); probe_rdp_words = 0;
    if (getenv("HLE_TIMING_RDP_DUMP")) {
        FILE *dump = fopen(getenv("HLE_TIMING_RDP_DUMP"), "wb");
        if (dump) fclose(dump);
    }
    unsigned int measured = 0;
    *diffs = *written = 0xffffffffu;
    if (!f) return 0;
    memory = (unsigned char *)malloc(0x802000);
    snapshot = (unsigned char *)malloc(optional_rsp_state_size());
    if (!memory || !snapshot || fread(header, sizeof(header), 1, f) != 1 ||
        header[0] != 0x484c5431u || !header[1] ||
        fread(memory, 0x802000, 1, f) != 1) goto finish;
    optional_rsp_save(snapshot);
    memset(&probe, 0, sizeof(probe));
    hle_init(&probe, memory, memory + 0x800000, memory + 0x801000,
        &regs[0], &regs[1], &regs[2], &regs[3], &regs[4], &regs[5], &regs[6],
        &regs[7], &regs[8], &regs[9], &regs[10], &regs[11], &regs[12], &regs[13],
        &regs[14], &regs[15], &regs[16], &regs[17], NULL);
    probe.hle_gfx = 1;
    probe.hle_aud = 0;
    probe.rsp_vco=hle_capture_vco(f);
    probe.rsp_t6=hle_capture_t6(f);naboo_set_phase(probe.rsp_t6);
    hle_capture_f5(f,probe.rsp_f5);naboo_set_context(probe.rsp_f5);
    probe_ram = memory; probe_dmem = memory + 0x800000;
    rdp_emit_set_backend(&probe_backend);
    rdp_emit_hle_reset(); rdp_emit_stream_load(NULL); hle_stream_state_load(NULL);
    timing_probe_active = 1; timing_probe_failed = 0;
    hle_execute(&probe);
    timing_probe_t6=probe.rsp_t6;
    memcpy(timing_probe_f5,probe.rsp_f5,sizeof(timing_probe_f5));
    if(hle_get_task_kind(&probe)==HLE_TASK_GRAPHICS)naboo_get_phase(&timing_probe_t6);
    if(hle_get_task_kind(&probe)==HLE_TASK_GRAPHICS)naboo_get_context(timing_probe_f5);
    work = hle_timing_get();
    if (!timing_probe_failed) {
        snprintf(family, 64, "%s", work.family);
        memcpy(units, work.units, sizeof(work.units));
        *estimate = hle_timing_cycles(); measured = header[1];
        {
            uint32_t count;
            if (fread(&count, sizeof(count), 1, f) == 1 && count <= 512u) {
                unsigned int r;
                *diffs = *written = 0;
                for (r = 0; r < count; r++) {
                    uint32_t span[2], i;
                    if (fread(span, sizeof(span), 1, f) != 1 || span[0] >= 0x800000u ||
                        span[1] > 0x800000u-span[0]) { *diffs = *written = 0xffffffffu; break; }
                    *written += span[1];
                    for (i = 0; i < span[1]; i++) {
                        int expected = fgetc(f);
                        if (expected < 0) { *diffs = *written = 0xffffffffu; break; }
                        if (memory[span[0]+i] != (unsigned char)expected) {
                            if (*diffs < 24 && getenv("HLE_TIMING_DIFF_TRACE"))
                                fprintf(stderr, "DIFF %06x HLE=%02x REF=%02x\n", span[0]+i,
                                    memory[span[0]+i], (unsigned char)expected);
                            ++*diffs;
                        }
                    }
                    if (*written == 0xffffffffu) break;
                }
            }
        }
    }
    timing_probe_active = 0;
    optional_rsp_load(snapshot);
    rdp_emit_set_backend(&backend);
    probe_ram = probe_dmem = NULL;
finish:
    fclose(f); free(memory); free(snapshot);
    return measured;
}
