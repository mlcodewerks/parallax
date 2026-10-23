#include "core/file.h"
/* Accurate executes native RSP instructions. Fast uses task HLE and streaming
 * walkers exclusively, stopping on unsupported programs or continuations. */
#include "optional_rsp.h"
#include "renderer_options.h"
#include "device/device.h"
#include "main/main.h"
#include "core/log.h"
#include "core/frontend.h"
#include "device/rcp/rdp/rdp_core.h"
#include "device/rcp/rsp/hle/rdp_emit_backend.h"
#include "device/rcp/rsp/hle/rdp_emit_hle.h"
#include "device/rcp/rsp/hle/rdp_emit_zsort.h"
#include "device/rcp/rsp/hle/rdp_emit_naboo.h"
#include "device/rcp/rsp/hle/rdp_emit_rsp.h"
#include "device/rcp/rdp/angrylion/n64video.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle/hle.h"
#include "device/rcp/rsp/hle/rdp_emit_state.h"
#include "hle/musyx_validation.h"
#include "hle/hle_external.h"
#include "hle/hle_timing.h"
#include "p_rsp/dpc_bridge.h"

extern unsigned int native_rsp_execute(unsigned int);
extern void parallelRSPResetExecutedCycles(void);
extern uint16_t parallelRSPGetVCO(void);
extern void parallelRSPSetVCO(uint16_t);
extern uint32_t parallelRSPGetT6(void);
extern void parallelRSPSetT6(uint32_t);
extern void parallelRSPGetF5Context(uint32_t values[7]);
extern void parallelRSPSetF5Context(const uint32_t values[7],uint32_t mask);
extern uint32_t parallelRSPGetPacketCursor(void);
extern void parallelRSPSetPacketCursor(uint32_t);
extern void parallelRSPGetGraphicsLatches(uint16_t values[32][8]);
extern void parallelRSPSetGraphicsLatches(const uint16_t values[32][8],uint32_t);
static struct hle_t hle;
static rsp_memory info;
static void capture_cp0(uint32_t words[16])
{
    const uint32_t values[16] = {
        *info.SP_MEM_ADDR_REG, *info.SP_DRAM_ADDR_REG, *info.SP_RD_LEN_REG, *info.SP_WR_LEN_REG,
        *info.SP_STATUS_REG, *info.SP_DMA_FULL_REG, *info.SP_DMA_BUSY_REG, *info.SP_SEMAPHORE_REG,
        *info.DPC_START_REG, *info.DPC_END_REG, *info.DPC_CURRENT_REG, *info.DPC_STATUS_REG,
        *info.DPC_CLOCK_REG, *info.DPC_BUFBUSY_REG, *info.DPC_PIPEBUSY_REG, *info.DPC_TMEM_REG
    };
    memcpy(words, values, sizeof(values));
}
static int strict_hle_failed;
static unsigned int graphics_checksum;
static const char *graphics_capture_prefix;
static unsigned int graphics_capture_started, graphics_capture_count;
static unsigned long long graphics_tasks, audio_tasks, other_tasks, boot_tasks, lle_tasks;
static unsigned long long graphics_resumes[HLE_RESUME_F3D + 1];
static int timing_probe_active, timing_probe_failed;
static uint32_t timing_probe_status;
uint32_t optional_rsp_timing_status(void){return timing_probe_status;}
static uint32_t timing_probe_supported;
uint32_t optional_rsp_timing_supported(void){return timing_probe_supported;}
static uint32_t timing_probe_full_rdram;
uint32_t optional_rsp_timing_full_rdram(void){return timing_probe_full_rdram;}
static uint32_t timing_probe_t6;
static uint32_t timing_probe_f5[7];
void optional_rsp_timing_f5(uint32_t *values){memcpy(values,timing_probe_f5,sizeof(timing_probe_f5));}
uint32_t optional_rsp_timing_t6(void){return timing_probe_t6;}

#include "hle/hle_timing_capture.h"

static unsigned char *get_rdram(void) { return info.RDRAM; }
static unsigned char *get_dmem(void) { return info.DMEM; }
static void boot_probe(struct hle_t *h, uint32_t state[PARALLEL_RSP_STATE_WORDS], int commit)
{
    uint32_t result=parallelRSPHleBootProbe((const uint32_t*)h->imem,
        (const uint32_t*)h->dmem,state,&h->rsp_vco);
    if(result&0x80000u) {
        if(!(result&0x100000u) || !(*h->sp_status&0x4000u)) {
            uint32_t clock=*h->dpc_clock;
            if(!(clock&0x7fu)) {
                const unsigned char *source=(clock&0x1000u)?h->imem:h->dmem;
                uint32_t address=(clock>>2)&0x7ffff8u;
                memmove(h->dram+address,source+(clock&0xff8u),8);
            }
        }
        return;
    }
    if(result&0x10000u) {
        uint32_t address=*(const uint32_t*)(h->dmem+0xfc8)&0x7ffff8u;
        memcpy(h->dram+address,h->imem+(result&0xff8u),8);
        *h->sp_status=(*h->sp_status&~0x4000u)|((result&0x20000u)?0x4000u:0u);
        if(result&0x40000u) {
            h->rsp_vco_written=1;
            if(commit)parallelRSPLoadState(state);
        }
    }
}
static unsigned int get_rdram_size(void) { return 0x800000; }
static void submit(const unsigned char *storage, unsigned int base, unsigned int bytes)
{
    const uint32_t *task=(const uint32_t*)(info.DMEM+0xfc0);
    const struct hle_timing_work work=hle_timing_get();
    uint32_t first=task[10]&0xffffffu,last=task[11]&0xffffffu;
    uint32_t cursor=*info.DPC_CURRENT_REG,start=*info.DPC_START_REG;
    int factor5=work.family && (!strcmp(work.family,"rs_gfx_task") || !strcmp(work.family,"naboo_gfx_task"));
    rdp_submit_hle(&g_dev.dp, (const uint32_t *)storage, base, bytes, hle_timing_cycles());
    if(factor5 && storage && !(*info.DPC_STATUS_REG & DPC_STATUS_FREEZE) && first<last && last<=0x800000u){
        const uint32_t *words=(const uint32_t*)storage;
        unsigned int at=0;
        if(cursor<first || cursor>last)cursor=last;
        while(at<bytes/4u){
            static const unsigned int triangle_words[8]={8,12,24,28,24,28,40,44};
            unsigned int op=(words[at]>>24)&63u;
            unsigned int count=op>=8 && op<=15?triangle_words[op-8]:op==0x24 || op==0x25?4u:2u;
            if(count>bytes/4u-at)count=bytes/4u-at;
            if(count*4u>last-first)break;
            if(cursor+count*4u>last){cursor=first;start=first;}
            cursor+=count*4u;at+=count;
        }
        /* The host FIFO is only command storage. Factor 5's CPU producer
         * polls the guest output ring; exposing the synthetic 7e0000 range
         * makes its ring-space arithmetic consume unrelated memory. */
        *info.DPC_START_REG=start;
        *info.DPC_CURRENT_REG=*info.DPC_END_REG=cursor;
    }
}
static void time_packet(const int32_t *words, unsigned int count)
{
    rdp_time_hle(&g_dev.dp, (const uint32_t *)words, count, hle_timing_cycles());
}
static void get_dpc_state(unsigned int state[3])
{
    state[0]=*info.DPC_END_REG;state[1]=*info.DPC_CURRENT_REG;state[2]=*info.DPC_STATUS_REG;
}
static void publish_fifo(unsigned int base,unsigned int bytes,int xbus)
{ rdp_publish_hle_fifo(&g_dev.dp,base,bytes,xbus); }
static unsigned int unsupported_hle_task(void);
static void graphics_diagnostic(const char *family, unsigned int pc,
                                unsigned int w0, unsigned int w1, int fatal)
{
    core_log(2, "Fast RSP %s: unsupported command at %08x: %08x %08x%s",
             family, pc, w0, w1, fatal ? " (task rejected)" : " (reserved or unimplemented)");
    if (fatal) unsupported_hle_task();
}
static const RdpEmitBackend backend = {get_rdram, get_rdram_size, get_dmem, submit, time_packet, get_dpc_state, publish_fifo, graphics_diagnostic};

void optional_rsp_init(rsp_memory rsp_info)
{
    if (timing_capture_file) { core_file_close(timing_capture_file); timing_capture_file = NULL; }
    timing_capture_prefix = getenv("HLE_TIMING_CAPTURE");
    timing_capture_skip = getenv("HLE_TIMING_CAPTURE_SKIP") ? (unsigned int)atoi(getenv("HLE_TIMING_CAPTURE_SKIP")) : 16u;
    timing_capture_stride = getenv("HLE_TIMING_CAPTURE_STRIDE") ? (unsigned int)atoi(getenv("HLE_TIMING_CAPTURE_STRIDE")) : 13u;
    if (!timing_capture_stride) timing_capture_stride = 1u;
    timing_capture_started = timing_capture_count = timing_capture_elapsed = 0;
    info = rsp_info;
    graphics_capture_prefix = getenv("HLE_GRAPHICS_CAPTURE");
    graphics_capture_started = graphics_capture_count = 0;
    memset(&hle, 0, sizeof(hle));
    graphics_tasks = audio_tasks = other_tasks = boot_tasks = lle_tasks = 0;
    strict_hle_failed = 0;
    memset(graphics_resumes, 0, sizeof(graphics_resumes));
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

static int graphics_supported_memory(const uint32_t *task, unsigned char *ram, unsigned char *dmem)
{
    unsigned int sum = 0, i;
    graphics_checksum=0;

    for (i = 0; i < (task[5] < 0xf80 ? task[5] : 0xf80) / 2; i++)
        sum += ram[task[4] + i];
    graphics_checksum=sum;
    /* A yielded task resumes from its saved DMEM, not TASK_DATA_PTR.
     * Only resident layouts with a recognized restore entry may import it;
     * rdp_emit_hle_yield_supported keeps uncertified layouts disabled. */
    if ((task[1] & 1u) && !hle_stream_can_resume(sum)) {
        int naboo_restore = (sum == 0x25c16 || sum == 0x25c53) &&
            naboo_yield_supported(ram,0x800000u,dmem);
        if (!naboo_restore && !rdp_emit_hle_yield_supported(ram,0x800000u,task[4],task[14],task[15]))
            return 0;
    }

    if (zsort_ucode_match(ram, 0x800000, task[6], task[7]))
    {
        /* Optional local capture for RSP disassembly/oracle tests. RDRAM
         * words retain the core's host-native byte order in this file. */
        const char *path = getenv("HLE_ZSORT_UCODE_DUMP");
        static int captured;
        if (path && !captured)
        {
            core_file *file = core_file_open(path, "wb");
            if (file)
            {
                unsigned int text_bytes = task[5] < 0xf80 ? task[5] : 0xf80;
                core_file_write_items(ram + task[4], 1, text_bytes, file);
                core_file_write_items(ram + task[6], 1, task[7], file);
                core_file_close(file);
                captured = 1;
                core_log(CORE_LOG_INFO, "ZSortp capture: text %08x/%x data %08x/%x",
                    task[4], text_bytes, task[6], task[7]);
            }
        }
        return zsort_validate(ram, 0x800000, dmem, task[12] & 0xffffff);
    }

    if ((sum == 0x25c16 || sum == 0x25c53) && (task[1]&1))
        return hle_stream_can_resume(sum) || naboo_yield_supported(ram,0x800000,dmem);
    if (sum == 0x28b9e || sum == 0x2095b || sum == 0x1f7bb ||
        sum == 0x25c16 || sum == 0x25c53)
        return !(task[1] & 1) || hle_stream_can_resume(sum);
    return rdp_emit_hle_supported(ram, 0x800000, task[4], task[6], task[7]);
}
static int graphics_supported(const uint32_t *task)
{ return graphics_supported_memory(task, info.RDRAM, info.DMEM); }

static int audio_supported_memory(const uint32_t *task, unsigned char *ram)
{
    const uint32_t *data = (const uint32_t *)(ram + task[6]);
    /* Match upstream hle.c's try_audio_task_detection branch order: ABI1
     * takes precedence over the v2 signature; v1 is in the non-ABI1 branch.
     * Validate every task, even when the dispatcher has cached its ucode. */
    if (data[0] == 1) {
        if (data[12] != 0xf0000f00 && data[4] == 0x00010010)
            return musyx_v2_supported(ram, task[12], task[13]);
    } else if (data[4] == 1)
        return musyx_v1_supported(ram, task[12], task[13]);
    return 1;
}
static int audio_supported(const uint32_t *task)
{ return audio_supported_memory(task, info.RDRAM); }

/* Standalone input snapshots for the private ROM-instruction replay harness.
 * Capture only task starts: a streaming continuation needs the walker's private
 * state and cannot be replayed as a fresh OSTask. No ROM bytes enter reports. */
static void graphics_capture_begin(const uint32_t *task)
{
    unsigned int i, sum = 0, skip, stride, limit;
    uint32_t header[4] = {0x484c5431u, 0, 0, 0x47465849u}, context[7], zero = 0;
    char path[1024];
    core_file *file;
    if (!graphics_capture_prefix || (task[1] & 1u)) return;
    if (getenv("HLE_GRAPHICS_CAPTURE_CLOCK") &&
        g_dev.r4300.cp0.count_clock < (int64_t)strtoull(getenv("HLE_GRAPHICS_CAPTURE_CLOCK"), NULL, 0)) return;
    for (i = 0; i < (task[5] < 0xf80 ? task[5] : 0xf80) / 2; i++)
        sum += info.RDRAM[task[4] + i];
    if (hle_stream_can_resume(sum)) return;
    skip = getenv("HLE_GRAPHICS_CAPTURE_SKIP") ? strtoul(getenv("HLE_GRAPHICS_CAPTURE_SKIP"), NULL, 0) : 0;
    stride = getenv("HLE_GRAPHICS_CAPTURE_STRIDE") ? strtoul(getenv("HLE_GRAPHICS_CAPTURE_STRIDE"), NULL, 0) : 13;
    limit = getenv("HLE_GRAPHICS_CAPTURE_LIMIT") ? strtoul(getenv("HLE_GRAPHICS_CAPTURE_LIMIT"), NULL, 0) : 24;
    if (!stride) stride = 1;
    i = graphics_capture_started++;
    if (i < skip || (i - skip) % stride || graphics_capture_count >= limit) return;
    snprintf(path, sizeof(path), "%s-%04u.rsp", graphics_capture_prefix, graphics_capture_count++);
    file = core_file_open(path, "wb");
    if (!file) return;
    header[2] = sum;
    core_file_write_items(header, sizeof(header), 1, file);
    core_file_write_items(info.RDRAM, 0x800000, 1, file);
    core_file_write_items(info.DMEM, 0x1000, 1, file);
    core_file_write_items(info.IMEM, 0x1000, 1, file);
    core_file_write_items(&zero, sizeof(zero), 1, file);
    {
        uint32_t state[4] = {HLE_CAPTURE_VCO_MAGIC, parallelRSPGetVCO(),
                            HLE_CAPTURE_T6_MAGIC, parallelRSPGetT6()};
        core_file_write_items(state, sizeof(state), 1, file);
    }
    parallelRSPGetF5Context(context);
    for (i = 0; i < 7; i++) {
        uint32_t pair[2] = {HLE_CAPTURE_F5_MAGIC + i, context[i]};
        core_file_write_items(pair, sizeof(pair), 1, file);
    }
    {
        uint32_t state[PARALLEL_RSP_STATE_WORDS];
        uint32_t tag[2] = {HLE_CAPTURE_RSP_MAGIC, PARALLEL_RSP_STATE_WORDS};
        parallelRSPSaveState(state);
        core_file_write_items(tag, sizeof(tag), 1, file);
        core_file_write_items(state, sizeof(state), 1, file);
        { uint32_t cp0[16]; capture_cp0(cp0); hle_capture_write_cp0(file, cp0); }
    }
    core_file_close(file);
}

static unsigned int unsupported_hle_task(void)
{
    static const char message[] = "Fast RSP: unsupported task. Select Accurate timing and restart content.";
    if (!strict_hle_failed) {
        strict_hle_failed = 1;
        core_log(CORE_LOG_ERROR, "%s (task=%u, PC=%03x)", message,
            ((const uint32_t *)(info.DMEM + 0xfc0))[0], *info.SP_PC_REG & 0xfff);
        if (environ_cb) {
            struct retro_message notification = {message, 600};
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &notification);
        }
    }
    *info.SP_STATUS_REG |= 3u; /* HALT/BROKE; never execute a native fallback. */
    stop_device(&g_dev);
    return 0;
}

unsigned int optional_rsp_execute(unsigned int cycles)
{
    const uint32_t *task = (const uint32_t *)(info.DMEM + 0xfc0);
    enum hle_task_kind kind;
    /* Fresh launches and OS yield relaunches still validate their ucode.
     * Only the unfinished task that already owns the SP may reuse it. */
    enum hle_graphics_resume resume = g_dev.sp.rsp_task_locked
        ? hle_graphics_resume_pending(&hle) : HLE_RESUME_NONE;
    if (!renderer_settings.rsp_hle)
        goto native;
    if (strict_hle_failed || (*info.SP_PC_REG & 0xfff) != 0)
        return unsupported_hle_task();
    /* Upstream identifies direct programs by boot size. Their DMEM tail is
     * not an OSTask and must not be subjected to guest-pointer checks.
     * Graphics streams terminate with commands; their unused data_size
     * field can contain unrelated words. Each walker bounds its reads. */
    if (task[3] <= 0x1000 && (
        (task[4] & 3) || task[4] > 0x7ff000 ||
        (task[6] & 3) || task[6] > 0x7ff000 || task[7] > 0x1000 ||
        (task[12] & 3) || task[12] >= 0x800000 ||
        (task[0] != 1u && task[13] > 0x800000 - task[12])))
        return unsupported_hle_task();
    kind = resume ? HLE_TASK_GRAPHICS : hle_get_task_kind(&hle);
    if (kind == HLE_TASK_UNKNOWN ||
        (kind == HLE_TASK_GRAPHICS && (renderer_settings.lle_graphics ||
            (!resume && !graphics_supported(task)))) ||
        (kind == HLE_TASK_AUDIO && ((task[12] & 7) ||
            renderer_settings.lle_audio || !audio_supported(task))))
        return unsupported_hle_task();
    if(kind==HLE_TASK_GRAPHICS && !(task[1]&1u) && !hle_stream_can_resume(graphics_checksum) &&
        (graphics_checksum==0x2095bu || graphics_checksum==0x25c16u || graphics_checksum==0x25c53u) &&
        task[10]<task[11] && task[11]<=0x800000u){
        *info.DPC_START_REG=*info.DPC_CURRENT_REG=*info.DPC_END_REG=task[11];
    }
    hle_timing_set_slice_limit(renderer_component_timing()?32768u:0u);
    parallelRSPResetExecutedCycles();
    hle.rsp_vco=parallelRSPGetVCO();hle.rsp_vco_written=0;
    rsp_graphics_entry_carry(hle.rsp_vco);
    hle.rsp_t6=parallelRSPGetT6();hle.rsp_t6_written=0;naboo_set_phase(hle.rsp_t6);
    parallelRSPGetF5Context(hle.rsp_f5);hle.rsp_f5_written=0;naboo_set_context(hle.rsp_f5);
    rsp_tri_factor5_seed_fraction(hle.rsp_f5[0]);
    if(kind==HLE_TASK_GRAPHICS){
        uint16_t latches[32][8];parallelRSPGetGraphicsLatches(latches);rsp_factor5_seed(latches);
        naboo_set_packet_cursor(parallelRSPGetPacketCursor());
    }
    if (kind == HLE_TASK_GRAPHICS) graphics_capture_begin(task);
    if ((*(const uint32_t*)hle.imem & 0xfc000000u) == 0x08000000u) {
        uint32_t state[PARALLEL_RSP_STATE_WORDS];parallelRSPSaveState(state);
        boot_probe(&hle,state,1);
    }
    if (resume) {
        graphics_resumes[resume]++;
        hle_resume_graphics(&hle, resume, graphics_checksum);
    }
    else hle_execute(&hle);
    if (strict_hle_failed) return 0;
    if(hle.rsp_vco_written)parallelRSPSetVCO(hle.rsp_vco);
    {
        if(kind==HLE_TASK_GRAPHICS)hle.rsp_t6_written=naboo_get_phase(&hle.rsp_t6);
        if(hle.rsp_t6_written)parallelRSPSetT6(hle.rsp_t6);
        if(kind==HLE_TASK_GRAPHICS)hle.rsp_f5_written=naboo_get_context(hle.rsp_f5);
        if(kind==HLE_TASK_GRAPHICS && graphics_checksum==0x2095bu){
            uint32_t context[5];rsp_tri_factor5_context(context);
            hle.rsp_f5[0]=context[0];hle.rsp_f5_written=1u;
        }
        if(kind==HLE_TASK_GRAPHICS){
            uint16_t latches[32][8];uint32_t mask=rsp_factor5_get(latches);
            parallelRSPSetGraphicsLatches(latches,mask);
            parallelRSPSetPacketCursor(naboo_get_packet_cursor());
        }
        if(hle.rsp_f5_written)parallelRSPSetF5Context(hle.rsp_f5,hle.rsp_f5_written);
    }
    if (kind == HLE_TASK_GRAPHICS) graphics_tasks++;
    else if (kind == HLE_TASK_AUDIO) audio_tasks++;
    else if (kind == HLE_TASK_BOOT) boot_tasks++;
    else other_tasks++;
    return hle_timing_cycles();
native:
    if ((*info.SP_PC_REG & 0xfff) == 0 && task[0] == 1 && task[3] <= 0x1000 &&
        task[4] <= 0x7ff000 && task[6] <= 0x7ff000)
        graphics_capture_begin(task);
    timing_capture_begin(task);
    lle_tasks++;
    cycles = native_rsp_execute(cycles);
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
void HleProcessRdpList(void *u) { (void)u; if (!timing_probe_active) renderer_process_commands(); }
void HleShowCFB(void *u) { (void)u; if (!timing_probe_active) renderer_show_framebuffer(); }
int HleForwardTask(void *u)
{
    (void)u;
    if (timing_probe_active) { timing_probe_failed = 1; return 1; }
    unsupported_hle_task();
    return 1;
}
void optional_rsp_shutdown(void)
{
    if (timing_capture_file) core_file_close(timing_capture_file);
    timing_capture_file = NULL;
    timing_capture_prefix = graphics_capture_prefix = NULL;
}
void optional_rsp_report(void)
{
    core_log(CORE_LOG_INFO, "RSP tasks: HLE graphics=%llu audio=%llu LLE=%llu other=%llu boot=%llu",
        graphics_tasks, audio_tasks, lle_tasks, other_tasks, boot_tasks);
    core_log(CORE_LOG_INFO, "RSP streaming continuations: F3DEX2=%llu Rogue=%llu BOSS=%llu Naboo=%llu",
        graphics_resumes[HLE_RESUME_STREAMING], graphics_resumes[HLE_RESUME_RS],
        graphics_resumes[HLE_RESUME_ZBOSS], graphics_resumes[HLE_RESUME_NABOO]);
    core_log(CORE_LOG_INFO, "RSP F3D continuations: %llu", graphics_resumes[HLE_RESUME_F3D]);
}

#define AUDIO_STATE_OFFSET offsetof(struct hle_t, alist_buffer)
#define AUDIO_STATE_BYTES (offsetof(struct hle_t, cached_ucodes) - AUDIO_STATE_OFFSET)
static unsigned int legacy_state_size(void) { return AUDIO_STATE_BYTES + rdp_emit_hle_state_size() + n64video_noise_state_size(); }
static unsigned int nead_state_offset(void) { return legacy_state_size() + rdp_emit_stream_state_size() + hle_stream_state_size(); }
static unsigned int f3d_state_offset(void) { return nead_state_offset() + sizeof(hle.nead_v31); }
unsigned int optional_rsp_state_size(void) { return f3d_state_offset() + f3d_state_size(); }
void optional_rsp_save(void *buffer)
{
    memcpy(buffer, (const unsigned char *)&hle + AUDIO_STATE_OFFSET, AUDIO_STATE_BYTES);
    rdp_emit_hle_save((unsigned char *)buffer + AUDIO_STATE_BYTES);
    n64video_save_noise((unsigned char *)buffer + AUDIO_STATE_BYTES + rdp_emit_hle_state_size());
    rdp_emit_stream_save((unsigned char *)buffer + legacy_state_size());
    hle_stream_state_save((unsigned char *)buffer + legacy_state_size() + rdp_emit_stream_state_size());
    memcpy((unsigned char *)buffer + nead_state_offset(), hle.nead_v31, sizeof(hle.nead_v31));
    f3d_state_save((unsigned char *)buffer + f3d_state_offset());
}
void optional_rsp_load_legacy(const void *buffer)
{
    memset(hle.nead_v31, 0, sizeof(hle.nead_v31));
    rdp_emit_stream_load(NULL);
    hle_stream_state_load(NULL);
    f3d_state_load(NULL);
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

void optional_rsp_load_v11b(const void *buffer)
{
    optional_rsp_load_v116(buffer);
    if (buffer) memcpy(hle.nead_v31, (const unsigned char *)buffer + nead_state_offset(), sizeof(hle.nead_v31));
}
void optional_rsp_load(const void *buffer)
{
    optional_rsp_load_v11b(buffer);
    if (buffer) f3d_state_load((const unsigned char *)buffer + f3d_state_offset());
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
    /* An invalid list can loop until the worker timeout. Bound the
     * benchmark dump too, so failed replay cannot exhaust the drive. */
    if(probe_rdp_words>0x400000u || bytes/4u>0x400000u-probe_rdp_words) {
        timing_probe_failed=1;
        return;
    }
    core_file *dump = getenv("HLE_TIMING_RDP_DUMP") ? core_file_open(getenv("HLE_TIMING_RDP_DUMP"), "ab") : NULL;
    (void)base;
    for (i = 0; i + 4u <= bytes; i += 4u) {
        uint32_t word; memcpy(&word, p + i, 4);
        for (shift = 24; ; shift -= 8) {
            probe_rdp_hash = (probe_rdp_hash ^ ((word >> shift) & 255u)) * UINT64_C(1099511628211);
            if (!shift) break;
        }
        probe_rdp_words++;
    }
    if (dump) { core_file_write_items(p, bytes, 1, dump); core_file_close(dump); }
}
static unsigned int probe_dpc[3];
static void probe_get_dpc_state(unsigned int state[3]) { memcpy(state,probe_dpc,sizeof(probe_dpc)); }
static const RdpEmitBackend probe_backend = {probe_get_ram, get_rdram_size, probe_get_dmem, probe_submit, NULL, probe_get_dpc_state};

unsigned int optional_rsp_timing_replay(const char *path, char *family,
    uint32_t *units, unsigned int *estimate, unsigned int *diffs, unsigned int *written)
{
    timing_probe_status = 0;
    timing_probe_supported = 0;
    timing_probe_full_rdram = 0;
    core_file *f = core_file_open(path, "rb");
    uint32_t header[4], regs[32] = {0};
    unsigned char *memory = NULL, *snapshot = NULL, *expected_ram = NULL;
    struct hle_t probe;
    struct hle_timing_work work;
    probe_rdp_hash = UINT64_C(14695981039346656037); probe_rdp_words = 0;
    if (getenv("HLE_TIMING_RDP_DUMP")) {
        core_file *dump = core_file_open(getenv("HLE_TIMING_RDP_DUMP"), "wb");
        if (dump) core_file_close(dump);
    }
    unsigned int measured = 0;
    *diffs = *written = 0xffffffffu;
    if (!f) return 0;
    memory = (unsigned char *)malloc(0x802000);
    snapshot = (unsigned char *)malloc(optional_rsp_state_size());
    expected_ram = (unsigned char *)malloc(0x800000);
    if (!memory || !snapshot || !expected_ram || core_file_read_items(header, sizeof(header), 1, f) != 1 ||
        header[0] != 0x484c5431u || (!header[1] && header[3] != 0x47465849u) ||
        core_file_read_items(memory, 0x802000, 1, f) != 1) goto finish;
    memcpy(expected_ram, memory, 0x800000);
    optional_rsp_save(snapshot);
    {
        uint32_t cp0[16];unsigned i;
        if (hle_capture_cp0(f, cp0)) {
            for (i=0;i<7;++i) regs[i+1]=cp0[i];
            regs[9]=cp0[7];
            for (i=8;i<16;++i) regs[i+2]=cp0[i];
            /* Offline DMA/RDP service completes immediately; keep input
             * signal bits and DPC_CLOCK, which some boot programs read. */
            regs[5]&=~3u;regs[6]=regs[7]=0;
            regs[12]=regs[11];regs[13]&=1u;regs[15]=regs[16]=regs[17]=0;
        }
    }
    memset(&probe, 0, sizeof(probe));
    hle_init(&probe, memory, memory + 0x800000, memory + 0x801000,
        &regs[0], &regs[1], &regs[2], &regs[3], &regs[4], &regs[5], &regs[6],
        &regs[7], &regs[8], &regs[9], &regs[10], &regs[11], &regs[12], &regs[13],
        &regs[14], &regs[15], &regs[16], &regs[17], NULL);
    probe.hle_gfx = 1;
    probe.hle_aud = 0;
    probe.rsp_vco=hle_capture_vco(f);
    rsp_graphics_entry_carry(probe.rsp_vco);
    probe.rsp_t6=hle_capture_t6(f);naboo_set_phase(probe.rsp_t6);
    hle_capture_f5(f,probe.rsp_f5);naboo_set_context(probe.rsp_f5);
    probe_ram = memory; probe_dmem = memory + 0x800000;
    probe_dpc[0]=regs[11];probe_dpc[1]=regs[12];probe_dpc[2]=regs[13];
    rdp_emit_set_backend(&probe_backend);
    rdp_emit_hle_reset(); rdp_emit_stream_load(NULL); hle_stream_state_load(NULL);
    rsp_tri_factor5_seed_fraction(probe.rsp_f5[0]);
    {
        uint32_t state[369];uint16_t latches[32][8]={{0}};
        if(hle_capture_rsp(f,state,369)){
            unsigned r,k;for(r=0;r<32;r++)for(k=0;k<8;k++)latches[r][k]=(uint16_t)state[PARALLEL_RSP_STATE_VECTOR_OFFSET+r*8+k];
            naboo_set_packet_cursor(state[21]);
        }else naboo_set_packet_cursor(0);
        rsp_factor5_seed(latches);
    }
    timing_probe_active = 1; timing_probe_failed = 0;
    /* Audio state slabs overlap OSTask in native DMEM. Completion may
     * legitimately overwrite task metadata, so classify its input once. */
    enum hle_task_kind probe_kind = hle_get_task_kind(&probe);
    {
        const uint32_t *task=(const uint32_t *)(probe.dmem+0xfc0);
        timing_probe_supported=probe_kind!=HLE_TASK_UNKNOWN;
        if(task[3]<=0x1000 && ((task[4]&3) || task[4]>0x7ff000 ||
            (task[6]&3) || task[6]>0x7ff000 || task[7]>0x1000 ||
            (task[12]&3) || task[12]>=0x800000 ||
            (task[0]!=1u && task[13]>0x800000-task[12])))
            timing_probe_supported=0;
        if(timing_probe_supported && probe_kind==HLE_TASK_GRAPHICS)
            timing_probe_supported=graphics_supported_memory(task,memory,probe.dmem);
        if(timing_probe_supported && probe_kind==HLE_TASK_AUDIO)
            timing_probe_supported=!(task[12]&7) && audio_supported_memory(task,memory);
    }
    uint32_t saved_limit=hle_timing_slice_limit();hle_timing_set_slice_limit(0);
    if ((*(const uint32_t*)probe.imem & 0xfc000000u) == 0x08000000u) {
        uint32_t state[PARALLEL_RSP_STATE_WORDS]={0};
        hle_capture_rsp(f,state,PARALLEL_RSP_STATE_WORDS);
        boot_probe(&probe,state,0);
    }
    hle_execute(&probe);
    timing_probe_status = *probe.sp_status;
    hle_timing_set_slice_limit(saved_limit);
    timing_probe_t6=probe.rsp_t6;
    memcpy(timing_probe_f5,probe.rsp_f5,sizeof(timing_probe_f5));
    if(probe_kind==HLE_TASK_GRAPHICS)naboo_get_phase(&timing_probe_t6);
    if(probe_kind==HLE_TASK_GRAPHICS)naboo_get_context(timing_probe_f5);
    work = hle_timing_get();
    if(work.family && !strcmp(work.family,"rs_gfx_task")){
        uint32_t context[5];rsp_tri_factor5_context(context);timing_probe_f5[0]=context[0];
    }
    if (!timing_probe_failed) {
        snprintf(family, 64, "%s", work.family);
        memcpy(units, work.units, sizeof(work.units));
        *estimate = hle_timing_cycles(); measured = header[1];
        {
            uint32_t count;
            if (core_file_read_items(&count, sizeof(count), 1, f) == 1 && count <= HLE_CAPTURE_MAX_RANGES) {
                unsigned int r;
                *diffs = *written = 0;
                for (r = 0; r < count; r++) {
                    uint32_t span[2], i;
                    if (core_file_read_items(span, sizeof(span), 1, f) != 1 || span[0] >= 0x800000u ||
                        span[1] > 0x800000u-span[0]) { *diffs = *written = 0xffffffffu; break; }
                    *written += span[1];
                    for (i = 0; i < span[1]; i++) {
                        int expected = core_file_getc(f);
                        if (expected < 0) { *diffs = *written = 0xffffffffu; break; }
                        expected_ram[span[0]+i] = (unsigned char)expected;
                        if (memory[span[0]+i] != (unsigned char)expected) {
                            if (*diffs < 24 && getenv("HLE_TIMING_DIFF_TRACE"))
                                fprintf(stderr, "DIFF %06x HLE=%02x REF=%02x\n", span[0]+i,
                                    memory[span[0]+i], (unsigned char)expected);
                            ++*diffs;
                        }
                    }
                    if (*written == 0xffffffffu) break;
                }
                if (*diffs != 0xffffffffu && *written != 0xffffffffu) {
                    unsigned int address;
                    *diffs = 0;
                    /* Native DMA spans define changes to the input RAM.
                     * Compare the whole resulting image too, so an HLE
                     * write outside those spans cannot be certified. */
                    if (memcmp(memory, expected_ram, 0x800000))
                        for (address=0;address<0x800000;++address)
                            if (memory[address]!=expected_ram[address]) {
                                if (*diffs<24 && getenv("HLE_TIMING_DIFF_TRACE"))
                                    fprintf(stderr,"RAM_DIFF %06x HLE=%02x REF=%02x\n",address,memory[address],expected_ram[address]);
                                ++*diffs;
                            }
                    timing_probe_full_rdram = 1;
                }
            }
        }
    }
    timing_probe_active = 0;
    if(getenv("HLE_TIMING_RAM_DUMP")) {
        core_file *out=core_file_open(getenv("HLE_TIMING_RAM_DUMP"),"wb");
        if(out){core_file_write_items(memory,0x800000,1,out);core_file_close(out);}
    }
    if (getenv("HLE_TIMING_GSP_DUMP")) {
        unsigned int size = rdp_emit_hle_state_size();
        void *state = malloc(size);
        if (state) {
            core_file *out = core_file_open(getenv("HLE_TIMING_GSP_DUMP"), "wb");
            rdp_emit_hle_save(state);
            if (out) { core_file_write_items(state, size, 1, out); core_file_close(out); }
            free(state);
        }
    }
    optional_rsp_load(snapshot);
    rdp_emit_set_backend(&backend);
    probe_ram = probe_dmem = NULL;
finish:
    core_file_close(f); free(memory); free(snapshot); free(expected_ram);
    return measured;
}
