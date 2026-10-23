#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* rdp_emit_hle.c -- activation glue for the angrylion HLE-graphics path.
 *
 * Called from renderer_process_display_list(). The core wires ProcessDList only when
 * the RSP plugin is HLE -- under cxd4/parallel the RDP is fed directly via
 * ProcessRDPList and this is never reached -- so there is no added cost to
 * the low-level configurations and no separate enable flag is needed.
 *
 * It reads the OSTask display-list pointer from DMEM, walks the display list
 * through the C89 geometry frontend (emitting RDP triangle commands into a
 * FIFO in RDRAM), then points the RDP at that FIFO and rasterizes it.
 *
 * Build check:
 *   gcc -std=c89 -pedantic -Wall -Wdeclaration-after-statement -Werror
 */

#include "rdp_emit_hle.h"
#include "rdp_emit_f3dex2.h"
#include "rdp_emit_zboss.h"
#include "rdp_emit_zsort.h"
#include "rdp_emit_f3d.h"
#include "rdp_emit_f3ddkr.h"
#include "rdp_emit_t3dux.h"
#include "rdp_emit_turbo3d.h"
#include "rdp_emit_rs.h"
#include "rdp_emit_rsp.h"
#include "rdp_emit_backend.h"
#include "rdp_emit_state.h"
#include "hle_timing.h"

/* The active rasterizer backend (angrylion or parallel-rdp).  Installed via
 * rdp_emit_set_backend() at plugin connect time.  The display-list walk,
 * microcode detection and command encoding below are identical for either;
 * only RDRAM/DMEM access and the finished-FIFO submit go through this. */
static const RdpEmitBackend *s_backend = 0;

void rdp_emit_set_backend(const RdpEmitBackend *be)
{
    if (be != 0)
        s_backend = be;
}

#define TASK_DATA_PTR_DMEM 0xff0u   /* OSTask data ptr, DMEM byte offset */

static GSPState s_gsp;
static RdpFifo  s_fifo;
static int      s_inited = 0;

void rdp_emit_hle_reset(void) {
    memset(&s_gsp, 0, sizeof(s_gsp)); s_inited = 0;
    f3dex2_state_load(0); rs_state_load(0); naboo_state_load(0);
    zboss_state_load(0); rsp_emit_state_load(0);
}
unsigned int rdp_emit_hle_state_size(void) { return sizeof(s_gsp) + sizeof(s_inited); }
void rdp_emit_hle_save(void *p)
{
    memcpy(p, &s_gsp, sizeof(s_gsp));
    memcpy((unsigned char *)p + sizeof(s_gsp), &s_inited, sizeof(s_inited));
}
void rdp_emit_hle_load(const void *p)
{
    memcpy(&s_gsp, p, sizeof(s_gsp));
    memcpy(&s_inited, (const unsigned char *)p + sizeof(s_gsp), sizeof(s_inited));
}

/* Host backing store for the synthesized RDP command FIFO. Kept out of
 * guest RDRAM: Expansion Pak games (Majora's Mask) use all 8 MiB, and
 * parking the FIFO in the top 256 KiB corrupted their heaps, ending in
 * the game's own Fault_AddHungupAndCrash. The DPC registers are pointed
 * at a virtual address range whose command fetch is redirected here via
 * n64video_set_hle_cmd_buffer; guest memory is never written. */
/* One flush must stay inside the smallest command window any backend
 * accepts.  parallel-rdp buffers a submission in a 32768-entry table and
 * drops the whole thing when (cmd_ptr + length) exceeds it - not the tail,
 * the entire submission, SYNC_FULL included, so the DP interrupt never
 * fires and the game's graphics thread blocks forever.  256 KiB is exactly
 * 32768 command words, one over its limit, so every full-cap flush was
 * discarded: Ocarina of Time froze on the transitions heavy enough to fill
 * the FIFO.  Angrylion has no such window and consumed them fine, which is
 * what made this look like a parallel-only fault.  Half that keeps a flush
 * comfortably inside the window with room for the partial-command tail. */
#define HLE_FIFO_CAP (128u * 1024u)
static unsigned char s_fifo_storage[HLE_FIFO_CAP];
static int s_output_only;

/* DMEM/RDRAM 32-bit words are host-native in this core (the RSP's u32()
 * accessor reads them directly); read native, not big-endian. */
static unsigned int read_dmem_u32(const unsigned char *dmem, unsigned int ofs)
{
    return *(const unsigned int *)(dmem + ofs);
}

/* Drain the FIFO through the rasterizer and reset it. Used both as the
 * mid-frame overflow flush (no SYNC_FULL involved; angrylion just executes
 * the batch) and by the activation below for the final batch. */
static void fifo_flush_to_rdp(RdpFifo *f)
{
    if (f->used == 0)
        return;
    if (s_output_only || s_backend == 0 || s_backend->submit == 0)
    {
        f->used = 0;
        return;
    }
    /* The backend points its DPC registers at [base, base+used), fetches the
     * command words from f->storage (guest RDRAM at `base` is never written),
     * runs its RDP, and restores guest-RDRAM command fetch on return. */
    s_backend->submit(f->storage, f->base, f->used);
    f->used = 0;
}


/* The ZSortBOSS microcode streams its RDP commands through a ring in
 * guest RDRAM at the OSTask's output_buff (the boot stub loads DMEM
 * 0xfe8 into the write cursor and points DPC_START/END at it; the
 * resident flush at IMEM 0x674 DMAs the DMEM 0xc20 staging block out,
 * advances DPC_END, and wraps by resetting to the base against the
 * DPC busy/current handshakes). The game's boot code observes that
 * ring -- with the walker keeping commands in a host-side fifo the
 * observation fails and the title boots into a degraded renderer. Write
 * the emitted stream through the guest ring and hand angrylion the
 * guest addresses, exactly as the microcode does. */
static unsigned int s_zb_ring_base;
static unsigned int s_zb_ring_end;
static unsigned int s_zb_ring_pos;
static int s_zb_ring_live;

/* Stage bytes stay in actual DMEM. The four saved transport words hold
 * the FIFO ring and pending byte count across HLE slices as well as ZBOSS. */
#define RING_GBI1 0x10000000u
#define RING_GBI2 0x20000000u
#define RING_XBUS 0x30000000u
#define RING_KIND 0x30000000u
#define RING_BUFFERED 0x40000000u
#define RING_OUTPUT_ONLY 0x80000000u
static unsigned int s_ring_stage[2], s_ring_limit;
static int s_ring_full_reload;
static unsigned char s_ring_sync_partial[4];
static unsigned int s_ring_noop_partial;
static unsigned int s_ring_tail, s_ring_scissor;
static unsigned int s_ring_attr_spill;
static unsigned int s_ring_group_active;
static unsigned int s_ring_split_prefix;
static unsigned int s_ring_reserve_before;
static unsigned int s_ring_vertex_table;
static unsigned int s_ring_dl_base, s_ring_dl_bytes;
static unsigned int s_ring_dl_end, s_ring_dl_pc, s_ring_dl_cmd, s_ring_dl_valid;

static int guest_ring_vertex_cache(int write,unsigned int vertex,int32_t clip[4])
{
    unsigned char *dm=s_backend->get_dmem();
    unsigned int table=s_ring_vertex_table+vertex*2u,address,k;
    unsigned int half_xor=0;
#ifndef MSB_FIRST
    half_xor=2u;
#endif
    if(!s_ring_vertex_table || vertex>32u || table>0xffeu)return 0;
    address=*(const unsigned short *)(dm+(table^half_xor));
    if((address&7u)||address>0xff0u)return 0;
    for(k=0;k<4;k++) {
        unsigned short *hi=(unsigned short *)(dm+((address+2*k)^half_xor));
        unsigned short *lo=(unsigned short *)(dm+((address+8+2*k)^half_xor));
        if(write){*hi=(unsigned short)((uint32_t)clip[k]>>16);*lo=(unsigned short)clip[k];}
        else clip[k]=(int32_t)(((uint32_t)*hi<<16)|*lo);
    }
    return 1;
}
static void guest_ring_vertex_spill(int apply)
{
    unsigned int state=(unsigned int)s_zb_ring_live;
    unsigned int at=s_ring_stage[(state>>12)&1u]+(state&0xfffu),words[4];
    int valid=rsp_vertex_spill_take(words);
    if(apply>=2) {
        unsigned char alpha[2];
        if((state&RING_KIND)==RING_XBUS)at=s_zb_ring_pos;
        if(!(state&RING_KIND)||at>0xfa8u)return;
        rsp_rej_prologue(words,alpha);
        memcpy(s_backend->get_dmem()+at+48u,words,8);
        memcpy(s_backend->get_dmem()+at+68u,words+2,8);
        if(apply==3) {
            s_backend->get_dmem()[(at+43u)^3u]=alpha[0];
            s_backend->get_dmem()[(at+63u)^3u]=alpha[1];
        }
        return;
    }
    if(!valid || !apply)return;
    if((state&RING_KIND)==RING_XBUS)at=s_zb_ring_pos;
    if(!(state&RING_KIND)||at>0xfb8u)return;
    if(valid&1)memcpy(s_backend->get_dmem()+at+24u,words,8);
    if(valid&2)memcpy(s_backend->get_dmem()+at+64u,words+2,8);
}
static void guest_ring_clip_spill(const GSPVertex *on, const GSPVertex *off)
{
    unsigned int state=(unsigned int)s_zb_ring_live;
    unsigned int at=s_ring_stage[(state>>12)&1u]+(state&0xfffu);
    unsigned int words[4];
#ifdef HLE_CLIP_OBSERVE
    HLE_CLIP_OBSERVE(on,off);
#endif
    if ((state&RING_KIND)==RING_XBUS) at=s_zb_ring_pos;
    if (!(state&RING_KIND) || at>0xfb8u) return;
    /* The clipper leaves LUV colours and LLV S/T in v25/v26. The first
     * iteration of the shared, pipelined projection stores those latches
     * before replacing them with the new vertex's screen coordinates. */
    rsp_clip_spill_words(((unsigned int)((on->r>>16)&255)<<24)
        | ((unsigned int)((on->g>>16)&255)<<16) | ((unsigned int)((on->b>>16)&255)<<8),
        ((unsigned int)(unsigned short)on->sv<<16)|(unsigned short)on->tv,
        (unsigned int)((off->b>>16)&255)<<8,(unsigned short)off->tv,words);
    memcpy(s_backend->get_dmem()+at+24u,words,8);
    memcpy(s_backend->get_dmem()+at+64u,words+2,8);
}

static int guest_ring_read_staging(unsigned int offset, int32_t *words, unsigned int count)
{
    unsigned int state = (unsigned int)s_zb_ring_live;
    unsigned int at = s_ring_stage[(state >> 12) & 1u] + (state & 0xfffu);
    if ((state & RING_KIND) == RING_XBUS) at = s_zb_ring_pos;
    if (!(state & RING_KIND) || at > 0x1000u || offset > 0x1000u - at ||
        count > (0x1000u - at - offset) / 4u) return 0;
    memcpy(words, s_backend->get_dmem() + at + offset, count * 4u);
#ifdef HLE_RING_TRACE
    fprintf(stderr,"STAGE %03x %u %08x %08x\n",at+offset,count,(unsigned)words[0],count>1?(unsigned)words[1]:0);
#endif
    return 1;
}

static void guest_ring_fetch_dl(unsigned int pc, unsigned int w0)
{
    unsigned int previous = s_ring_dl_cmd >> 24;
    unsigned int step = pc - s_ring_dl_pc;
    if (pc == ~0u) { s_ring_dl_valid = 0; s_fifo.dl_end = 0; return; }
    /* TEXRECT consumes its following half-commands inside the dispatcher.
     * Other nonsequential PCs enter the native fetch routine through a
     * display-list call, branch or return, regardless of the old buffer. */
    int sequential = step == 8u ||
        ((previous == 0xe4u || previous == 0xe5u) && (step == 16u || step == 24u));
    if (!s_ring_dl_valid || !sequential || pc >= s_ring_dl_end) {
        unsigned int size = s_backend->get_rdram_size();
        unsigned int bytes = s_ring_dl_bytes;
        if (pc <= size && bytes <= size - pc) {
            memcpy(s_backend->get_dmem() + s_ring_dl_base,
                   s_backend->get_rdram() + (pc & ~7u), bytes);
            s_ring_dl_end = pc + bytes;
            s_ring_dl_valid = 1;
        }
    }
    s_ring_dl_pc = pc;
    s_ring_dl_cmd = w0;
    s_fifo.dl_end = s_ring_dl_end;
}

static void guest_ring_store_indices(unsigned int packed)
{
    unsigned int state = (unsigned int)s_zb_ring_live;
    unsigned int at = s_ring_stage[(state >> 12) & 1u] + (state & 0xfffu);
    if ((state & RING_KIND) == RING_XBUS) at = s_zb_ring_pos;
    /* The native triangle entry stores indices before even the trivial
     * reject. A later sync can expose that scratch word without a triangle. */
    if ((state & RING_KIND) && at <= 0xff8u)
        memcpy(s_backend->get_dmem() + at + 4u, &packed, 4);
}

static void guest_ring_drain(void)
{
    unsigned int state = (unsigned int)s_zb_ring_live;
    unsigned int bytes = state & 0xfffu;
    unsigned int bank = (state >> 12) & 1u;
    unsigned char *ram, *dm;
    if (!(state & RING_KIND) || !bytes) return;
#ifdef HLE_RING_TRACE
    fprintf(stderr,"DMA %03x %06x %u\n",s_ring_stage[bank],s_zb_ring_pos,bytes);
#endif
    ram = s_backend->get_rdram();
    dm = s_backend->get_dmem();
    if (!(state & RING_OUTPUT_ONLY) && s_zb_ring_pos + bytes > s_zb_ring_end)
        s_zb_ring_pos = s_zb_ring_base;
    if (s_zb_ring_pos <= s_backend->get_rdram_size()
        && bytes <= s_backend->get_rdram_size() - s_zb_ring_pos)
        memcpy(ram + s_zb_ring_pos, dm + s_ring_stage[bank], bytes);
    /* A frozen DP still accepts the producer's DMA and END writes. Keep
     * those commands in the native guest FIFO; unfreezing consumes them
     * directly from RAM, so no transient host buffer must outlive this call. */
    if(!(state&RING_OUTPUT_ONLY) && s_backend->get_dpc_state && s_backend->publish_fifo) {
        unsigned int dp[3];s_backend->get_dpc_state(dp);
        if(dp[2]&2u)s_backend->publish_fifo(s_zb_ring_pos,bytes,0);
    }
    s_zb_ring_pos += bytes;
    if(!rsp_graphics_sdk_one(5)||!rsp_graphics_sdk_one(7))memcpy(dm+0xf0u,&s_zb_ring_pos,4);
    if (s_ring_stage[0] != s_ring_stage[1]) bank ^= 1u;
    s_zb_ring_live = (int)((state & (RING_KIND | RING_BUFFERED | RING_OUTPUT_ONLY)) | (bank << 12));
}

static void guest_ring_group(int begin)
{
    unsigned int state = (unsigned int)s_zb_ring_live;
    if (begin) {
        if((state&RING_KIND)==RING_GBI2) {
            s_ring_group_active=2;
            return;
        }
        if ((state & RING_KIND) == RING_GBI1 && !(state & RING_BUFFERED)) {
            guest_ring_drain();
            s_zb_ring_live |= RING_BUFFERED;
            s_ring_group_active = 1;
        }
    } else if (s_ring_group_active) {
        if(s_ring_group_active==2) {
            s_ring_group_active=0;
            if((state&0xfffu)>s_ring_limit)guest_ring_drain();
            return;
        }
        guest_ring_drain();
        s_zb_ring_live &= ~RING_BUFFERED;
        s_ring_group_active = 0;
    }
}

static void guest_ring_write(int32_t *words, unsigned int count)
{
    unsigned int state = (unsigned int)s_zb_ring_live;
    unsigned int used = state & 0xfffu;
    unsigned int stage = s_ring_stage[(state >> 12) & 1u];
    unsigned int bytes = count * 4u;
    unsigned int op = ((unsigned int)words[0] >> 24) & 63u;
    if(!s_ring_split_prefix &&
        ((state&RING_KIND)==RING_GBI2 ||
         ((state&RING_KIND)==RING_GBI1 && !(state&RING_BUFFERED)))) {
        static const unsigned int lengths[8]={8,12,24,28,24,28,40,44};
        unsigned int pos=0;
        /* Clipped fans are one host batch. The resident writer checks its
         * DMA threshold after each triangle, which also determines where
         * the guest output ring wraps. Validate the complete batch first. */
        while(pos<count) {
            unsigned int id=((unsigned int)words[pos]>>24)&63u;
            if(id<8u || id>15u || lengths[id-8u]>count-pos)break;
            pos+=lengths[id-8u];
        }
        if(pos==count && op>=8u && op<=15u && count>lengths[op-8u]) {
            pos=0;
            while(pos<count) {
                unsigned int len=lengths[(((unsigned int)words[pos]>>24)&63u)-8u];
                /* Only the last triangle owns the retained attribute tail. */
                s_ring_split_prefix=pos+len<count;
                guest_ring_write(words+pos,len);
                pos+=len;
            }
            s_ring_split_prefix=0;
            return;
        }
    }
    {
        unsigned int packet_words, offset;
        int32_t spill[16];
        int kind = s_ring_split_prefix?0:rsp_tri_take_base_spill(&packet_words,&offset,spill);
        if (kind && op >= 8u && op <= 15u &&
            s_ring_attr_spill && packet_words <= count) {
            unsigned int at = ((state & RING_KIND) == RING_XBUS ? s_zb_ring_pos : stage + used)
                + (count - packet_words + offset) * 4u;
            if (at <= 0x1000u && (unsigned int)kind <= (0x1000u-at)/4u)
                memcpy(s_backend->get_dmem()+at,spill,(unsigned int)kind*4u);
        }
    }
    if (count == 2 && op == 0x2du && s_ring_scissor)
        memcpy(s_backend->get_dmem() + s_ring_scissor, words, 8);
    if ((state & RING_KIND) == RING_XBUS) {
        stage = s_zb_ring_pos;
        used = 0;
    }
    /* GBI1 forwards each TEXRECT half through its own eight-byte DMA.
     * The RDP still receives one four-word packet in the host batch. */
    if ((state & RING_KIND) == RING_GBI1 &&
        (!(state & RING_BUFFERED) || s_ring_reserve_before) &&
        count == 4 && (op == 0x24u || op == 0x25u)) {
        guest_ring_write(words, 2);
        guest_ring_write(words + 2, 2);
        return;
    }
    if(s_ring_reserve_before && used+bytes>s_ring_limit) {
        guest_ring_drain();
        state=(unsigned int)s_zb_ring_live;
        used=state&0xfffu;
        stage=s_ring_stage[(state>>12)&1u];
    }
    if (!(state & RING_KIND) || stage + used > 0x1000u
        || bytes > 0x1000u - stage - used) return;
    /* SGI's sync entry stores only word zero. The second word remains
     * from the previous occupant of that staging bank, even though the
     * RDP ignores it. Preserve it in guest RAM and the submitted packet. */
    if (count == 2 && ((op >= 0x26u && op <= 0x29u && s_ring_sync_partial[op-0x26u])
        || ((unsigned int)words[0] >> 24 == 0u && s_ring_noop_partial)))
        words[1] = (int32_t)read_dmem_u32(s_backend->get_dmem(), stage + used + 4u);
    memcpy(s_backend->get_dmem() + stage + used, words, bytes);
    if ((state & RING_KIND) == RING_XBUS) {
        if(s_backend->get_dpc_state && s_backend->publish_fifo) {
            unsigned int dp[3];s_backend->get_dpc_state(dp);
            if(dp[2]&2u)s_backend->publish_fifo(s_zb_ring_pos,bytes,1);
        }
        s_zb_ring_pos += bytes;
        if (s_zb_ring_pos > s_zb_ring_end) s_zb_ring_pos = s_zb_ring_base;
        return;
    }
    used += bytes;
    s_zb_ring_live = (int)((state & ~0xfffu) | used);
    if (!s_ring_group_active &&
        (((state & RING_KIND) == RING_GBI1 && !(state & RING_BUFFERED)) || used > s_ring_limit))
        guest_ring_drain();
}

static void guest_ring_config(RdpFifo *fifo, unsigned int text, int resume,
                              unsigned int data, unsigned int data_bytes,
                              unsigned int data_skip)
{
    unsigned char *ram = s_backend->get_rdram();
    unsigned char *dm = s_backend->get_dmem();
    unsigned int size = s_backend->get_rdram_size();
    unsigned int mode = 0, at, dp[3] = {0,0,0}, flags = 0;
    unsigned int first = read_dmem_u32(dm, 0xfe8u) & 0xffffffu;
    unsigned int last = read_dmem_u32(dm, 0xfecu) & 0xffffffu;
    s_ring_full_reload=s2dex1_ucode_match(ram,size,data,data_bytes);
    fifo->dmem = dm;
    memset(s_ring_sync_partial, 0, sizeof(s_ring_sync_partial));
    s_ring_noop_partial = 0;
    s_ring_tail = s_ring_scissor = 0;
    s_ring_attr_spill = 0;
    s_output_only=0;
    gsp_set_clip_spill(0);
    gsp_set_vertex_spill(0);
    gsp_set_legacy_lighting(0);
    gsp_set_vertex_cache(0);
    s_ring_vertex_table=0;
    rsp_vertex_spill_reset();
    rsp_set_vtx_z_signed(0);
    fifo->spnoop = 0;
    fifo->store_indices = 0;
    fifo->fetch_dl = 0;
    fifo->matrix_dma_table=0;
    fifo->drain = 0;
    fifo->read_staging = 0;
    fifo->group = 0;
    s_ring_group_active = 0;
    s_ring_split_prefix = 0;
    s_ring_reserve_before = 0;
    if (!data_skip && !resume) s_ring_dl_valid = 0;
    fifo->write = 0;
    if (!resume) s_zb_ring_live = 0;
    if ((text & 3u) || text > size || 0x1000u > size - text) return;
    /* Decode native flush operations. XBUS programs lack these FIFO
     * sequences and must not acquire guest-RAM command writes. */
    for (at = 0; at + 24u <= 0x1000u; at += 4u) {
        const unsigned int *w = (const unsigned int *)(ram + text + at);
        if(w[0]==0x039e1021u && w[1]==0x2441f040u &&
           (w[2]&0xffff0000u)==0x18200000u && w[3]==0xac060accu) {
            /* This self-booting writer reserves space before each store,
             * using gp+s8 against 0xfc0, then drains from DMEM 0xb00. */
            s_ring_stage[0]=s_ring_stage[1]=0xb00u;
            s_ring_limit=0x4c0u;
            s_ring_reserve_before=1;
            mode=RING_GBI1;flags=RING_BUFFERED;
            break;
        }
        /* The original DRAM-output writer accumulates a byte count at
         * OSTask.output_buff_size. It never submits these packets to DPC;
         * the CPU owns that later submission. */
        if(at+28u<=0x1000u && w[0]==0x8fb30018u &&
           (w[1]&0xffff0000u)==0x20140000u && w[2]==0x8fb10044u &&
           w[3]==0x02f49022u && w[4]==0x02328820u &&
           w[5]==0xafb10044u && w[6]==0x2252ffffu) {
            unsigned int stage=w[1]&0xffffu;
            if(stage && stage+176u<=0x1000u) {
                s_ring_stage[0]=s_ring_stage[1]=stage;
                s_ring_limit=0;
                mode=RING_GBI1;flags=RING_OUTPUT_ONLY;
            }
            break;
        }
        if (w[0] == 0x8e480008u && (w[1] & 0xffff0000u) == 0x260a0000u &&
            (w[2] & 0xffff0000u) == 0x11400000u && w[3] == 0x001ff025u) {
            unsigned int stage = (unsigned int)(-(int)(short)w[1]);
            if (stage && !(stage & 7u) && stage + 176u <= 0x1000u) {
                s_ring_stage[0] = s_ring_stage[1] = stage;
                s_ring_limit = 0xfffu;
                mode = RING_GBI1;
                flags = RING_BUFFERED;
                fifo->drain = guest_ring_drain;
            }
            break;
        }
        if ((w[0] == 0x8fb30004u && (w[1] & 0xffff0000u) == 0x22f20000u &&
             w[2] == 0x8fb7001cu && (w[3] & 0xffff0000u) == 0x1a400000u) ||
            (w[0] == 0x8c1300f4u && w[1] == 0x400c5000u &&
             w[2] == 0x90110225u && (w[3] & 0xffff0000u) == 0x22f20000u)) {
            unsigned int stage = (unsigned int)(-(int)(short)(w[w[0] == 0x8fb30004u ? 1 : 3]));
            if (stage && !(stage & 7u) && stage + 176u <= 0x1000u) {
                s_ring_stage[0] = s_ring_stage[1] = stage;
                s_ring_limit = 0xfffu;
                mode = RING_GBI1;
                flags = RING_BUFFERED;
                fifo->drain = guest_ring_drain;
            }
            break;
        }
        if (at + 32u <= 0x1000u && (w[0] & 0xffff0000u) == 0x22eb0000u &&
            (w[1] & 0xffff0000u) == 0x19600000u &&
            (w[2] & 0xffff0000u) == 0x201f0000u && w[3] == w[0] &&
            (w[4] & 0xffff0000u) == 0x19600000u &&
            (w[5] & 0xffff0000u) == 0x8c130000u && w[6] == 0x8c0c0fecu &&
            (w[7] & 0xffff0000u) == 0x22f20000u) {
            unsigned int stage = (unsigned int)(-(int)(short)w[7]);
            unsigned int threshold = (unsigned int)(-(int)(short)w[0]);
            if (stage && stage < threshold && threshold + 176u <= 0x1000u) {
                s_ring_stage[0] = s_ring_stage[1] = stage;
                s_ring_limit = threshold - stage;
                mode = RING_GBI1;
                flags = RING_BUFFERED;
            }
            break;
        }
        if (at + 32u <= 0x1000u && (w[0] & 0xffff0000u) == 0x22eb0000u
            && w[1] == 0x1960000au && w[2] == 0x40974800u && w[3] == 0x400b5800u
            && w[4] == 0x316b0600u && w[5] == 0x1560fffdu && w[6] == 0x400b5000u
            && (w[7] & 0xffff0000u) == 0x20170000u) {
            unsigned int stage = w[7] & 0xffffu;
            unsigned int limit = (unsigned int)(-(int)(short)w[0]);
            if (stage && stage < limit && limit + 176u <= 0x1000u) {
                s_ring_stage[0] = s_ring_stage[1] = stage;
                first = stage; last = limit;
                mode = RING_XBUS;
            }
            break;
        }
        if (w[0] == 0x02f65822u && (w[1] & 0xffff0000u) == 0x19600000u
            && w[2] == 0x400c3000u && w[3] == 0x8c1800f0u
            && (w[4] & 0xffff0000u) == 0x25730000u && w[5] == 0x1580fffcu) {
            unsigned int k, stage = 0, bank = 0, limit = w[4] & 0xffffu;
            /* Some residents overwrite the allocation end loaded from
             * OSTask with a constant LUI before the wrap comparison. */
            if (at + 32u <= 0x1000u && w[6] == 0x8c0c0fecu &&
                (w[7] & 0xffff0000u) == 0x3c0c0000u)
                last = (w[7] & 0xffffu) << 16;
            for (k = 0; k + 4u <= at; k += 4u) {
                unsigned int op = *(const unsigned int *)(ram + text + k);
                if ((op & 0xffff0000u) == 0x20170000u) stage = op & 0xffffu;
            }
            for (k = 6; k < 32u && at + (k + 2u) * 4u <= 0x1000u; ++k)
                if ((w[k] & 0xffff0000u) == 0x3ad60000u
                    && (w[k+2] & 0xffff0000u) == 0x22d70000u
                    && (short)w[k+2] == -(int)limit)
                    bank = ((stage + limit) ^ (w[k] & 0xffffu)) - limit;
            if (stage && bank && limit && stage + limit + 176u <= 0x1000u
                && bank + limit + 176u <= 0x1000u) {
                s_ring_stage[0] = stage;
                s_ring_stage[1] = bank;
                s_ring_limit = limit;
                mode = RING_GBI2;
            }
            break;
        }
        if (w[0] == 0x8fb30018u && (w[1] & 0xffff0000u) == 0x22f20000u
            && w[2] == 0x8fb70044u && (w[3] & 0xffff0000u) == 0x1a400000u) {
            unsigned int stage = (unsigned int)(-(int)(short)(w[1] & 0xffffu));
            if (stage && stage + 176u <= 0x1000u) {
                s_ring_stage[0] = s_ring_stage[1] = stage;
                s_ring_limit = 0;
                mode = RING_GBI1;
            }
            break;
        }
        if (w[0] == 0x87b4004au && w[1] == 0x40063000u
            && w[2] == 0x8fb30018u && w[3] == 0x001fa820u
            && w[4] == 0x14c0fffcu && w[5] == 0x02f49022u) {
            unsigned int k, stage = 0, toggle = 0;
            for (k = 0; k + 4u <= 0x1000u; k += 4u) {
                unsigned int op = *(const unsigned int *)(ram + text + k);
                if (!stage && (op & 0xffff0000u) == 0x20170000u) stage = op & 0xffffu;
                if (k >= at && k < at + 160u && (op & 0xffff0000u) == 0x3a940000u)
                    toggle = op & 0xffffu;
            }
            if (stage && toggle && stage + 176u <= 0x1000u && (stage ^ toggle) + 176u <= 0x1000u) {
                s_ring_stage[0] = stage;
                s_ring_stage[1] = stage ^ toggle;
                s_ring_limit = 0;
                mode = RING_GBI1;
            }
            break;
        }
    }
    if (!mode) return;
    for (at=0;at+20u<=0xf80u;at+=4u) {
        const unsigned int *w=(const unsigned int *)(ram+text+at);
        if(w[0]==0xe9591c7eu && w[2]==0xe919187cu &&
           w[4]==0xe91a0e7du) {
            gsp_set_vertex_spill(guest_ring_vertex_spill);
            s_ring_attr_spill=1;
        }
        if (w[0]==0xe9191879u &&
            (w[4]==0xe91a1173u || w[4]==0xe91a0a67u)) {
            gsp_set_clip_spill(guest_ring_clip_spill);
            gsp_set_vertex_spill(guest_ring_vertex_spill);
            /* SDV/SSV store the signed depth directly; the later
             * SLV variant inserts a VGE against zero before the store. */
            rsp_set_vtx_z_signed(w[4]==0xe91a0a67u);
            {
                unsigned int k;
                for(k=0;k+20u<=0xf80u;k+=4u) {
                    const unsigned int *p=(const unsigned int *)(ram+text+k);
                    if((p[0]&0xffff0000u)==0x94210000u &&
                       (p[2]&0xffff0000u)==0x94420000u &&
                       (p[4]&0xffff0000u)==0x94630000u &&
                       (p[0]&0xffffu)==(p[2]&0xffffu) &&
                       (p[0]&0xffffu)==(p[4]&0xffffu) &&
                       (p[0]&0xffffu)<=0xfbcu && !(p[0]&1u)) {
                        s_ring_vertex_table=p[0]&0xffffu;
                        gsp_set_vertex_cache(guest_ring_vertex_cache);
                        break;
                    }
                }
            }
            break;
        }
    }
    for (at = 0; at + 20u <= 0xf80u; at += 4u) {
        const unsigned int *w = (const unsigned int *)(ram + text + at);
        if (w[0] == 0xe8451802u && w[1] == 0xe8521800u && w[2] == 0xe8251c02u &&
            (w[3] & 0xfc000000u) == 0x08000000u && w[4] == 0xe8321c00u) {
            s_ring_attr_spill = 1;
            break;
        }
    }
    for (at = 0; at + 28u <= 0x1000u; at += 4u) {
        const unsigned int *w = (const unsigned int *)(ram + text + at);
        unsigned int bytes = (w[0] & 0xfffu) + 1u;
        unsigned int base = w[3] & 0xfffu;
        if ((w[0] & 0xffff0000u) == 0x20120000u && w[1] == 0x37530000u &&
            (w[2] & 0xfc000000u) == 0x0c000000u &&
            (w[3] & 0xffff0000u) == 0x20140000u &&
            w[4] == 0x275a0000u + bytes &&
            (w[5] & 0xfc000000u) == 0x0c000000u &&
            w[6] == (0x201b0000u | ((0u - bytes) & 0xffffu)) &&
            !(bytes & 7u) && !(base & 7u) && base + bytes <= 0xfc0u) {
            s_ring_dl_base = base;
            s_ring_dl_bytes = bytes;
            fifo->fetch_dl = guest_ring_fetch_dl;
            break;
        }
    }
    for (at = 0; at + 24u <= 0x1000u; at += 4u) {
        const unsigned int *w = (const unsigned int *)(ram + text + at);
        unsigned int bytes = (w[0] & 0xfffu) + 1u;
        unsigned int base = w[3] & 0xfffu;
        if ((w[0] & 0xffff0000u) == 0x20130000u && w[1] == 0x37580000u &&
            (w[2] & 0xfc000000u) == 0x0c000000u &&
            (w[3] & 0xffff0000u) == 0x24140000u &&
            w[4] == 0x275a0000u + bytes &&
            w[5] == (0x201b0000u | ((0u - bytes) & 0xffffu)) &&
            !(bytes & 7u) && !(base & 7u) && base + bytes <= 0xfc0u) {
            s_ring_dl_base = base;
            s_ring_dl_bytes = bytes;
            fifo->fetch_dl = guest_ring_fetch_dl;
            break;
        }
    }
    /* The limit is compared as an address, never used as a DMA base.
     * Valid tasks can end their allocation between eight-byte boundaries. */
    for(at=0;at+24u<=0x1800u && at+24u<=size-text;at+=4u) {
        const unsigned int *w=(const unsigned int *)(ram+text+at);
        if((w[0]&0xffff0000u)==0x93730000u &&
           (w[1]&0xffff0000u)==0x94340000u && w[2]==0x00191142u &&
           (w[3]&0xffff0000u)==0x959f0000u && w[5]==0x0282a020u) {
            fifo->matrix_dma_table=w[1]&0xfffu;break;
        }
    }
    if (mode != RING_XBUS && ((first & 7u) || first >= last || last > size)) return;
    if (!resume) {
        unsigned int bytes = data_bytes;
        /* The boot data DMA initializes staging bytes as well as constant
         * tables. Partial sync/no-op stores retain those loaded bytes,
         * not the preceding task's DMEM. SP transfers round to eight. */
        if (bytes && bytes <= 0x1000u) {
            bytes = (bytes + 7u) & ~7u;
            if (data_skip < bytes && data <= size && bytes <= size - data)
                memcpy(dm + data_skip, ram + data + data_skip, bytes - data_skip);
        }
    }
    if (mode == RING_GBI2 || mode == RING_XBUS) {
        unsigned int table = 0, op;
        for (at = 0; at + 4u <= 0x1000u; at += 4u) {
            unsigned int w = *(const unsigned int *)(ram + text + at);
            if ((w & 0xffff0000u) == 0x956b0000u) { table = w & 0xfffu; break; }
        }
        if (table && data <= size - 0x1000u) {
            unsigned int nop = *(const unsigned short *)(ram + data + (table ^ 2u));
            unsigned int sync = *(const unsigned short *)(ram + data + (((table - 50u) & 0xfffu) ^ 2u));
            /* Opcode zero shares the native sync-copy entry in these
             * dispatch tables, including S2DEX's loaded code overlay. */
            if (nop && nop == sync) fifo->spnoop = s_ring_noop_partial = 1;
            {
                unsigned int offset = (table - 38u) & 0xfffu;
                unsigned int pc = *(const unsigned short *)(ram + data + (offset ^ 2u)) & 0xfffu;
                unsigned int pos = (pc - 0x80u) & 0xfffu;
                const unsigned int *w = (const unsigned int *)(ram + text + pos);
                if (pos + 12u <= 0x1000u && (w[0] & 0xffff0000u) == 0xac190000u
                    && (((w[1] & 0xfc000000u) == 0x08000000u
                         && w[2] == 0xac180000u + (w[0] & 0xffffu) + 4u)
                        || (w[1] == 0xac180000u + (w[0] & 0xffffu) + 4u
                            && (w[2] & 0xffff0000u) == 0x201f0000u))
                    && (w[0] & 0xffffu) <= 0xff8u)
                    s_ring_scissor = w[0] & 0xffffu;
            }
            {
                unsigned int offset = (table + 10u) & 0xfffu;
                unsigned int pc = *(const unsigned short *)(ram + data + (offset ^ 2u)) & 0xfffu;
                unsigned int pos = (pc - 0x80u) & 0xfffu;
                const unsigned int *w = (const unsigned int *)(ram + text + pos);
                if (pos + 12u <= 0x1000u && (w[0] & 0xffff0000u) == 0x201f0000u
                    && w[1] == 0xaef90004u && w[2] == 0xcae23000u)
                    fifo->store_indices = guest_ring_store_indices;
            }
            for (op = 0xe6u; op <= 0xe9u; ++op) {
                unsigned int offset = (table + 2u * (op - 256u)) & 0xfffu;
                unsigned int entry = *(const unsigned short *)(ram + data + (offset ^ 2u));
                unsigned int pc = entry & 0xfffu;
                unsigned int pos = (pc - 0x80u) & 0xfffu;
                const unsigned int *w = (const unsigned int *)(ram + text + pos);
                if ((fifo->spnoop && entry == nop) ||
                    (pos + 12u <= 0x1000u && w[0] == 0xaef90000u
                    && (w[1] & 0xfc000000u) == 0x08000000u && w[2] == 0x22f70008u)
                    )
                    s_ring_sync_partial[op-0xe6u] = 1;
            }
        }
    }
    {
        const unsigned int *w = (const unsigned int *)(ram + text + 0xf80u);
        /* Some native completion/load overlays restore the cached scissor
         * before draining, even when the list ended with a FullSync. */
        if ((w[0] & 0xffffff80u) == 0xc81d1800u && w[1] == 0x22f70008u
            && w[2] == 0xeafd187fu && w[3] == 0x02f65822u) {
            int offset = (int)(w[0] & 0x7fu);
            if (offset & 0x40) offset -= 128;
            if (offset > 0 && offset * 8 <= 0xff8) s_ring_tail = (unsigned int)offset * 8u;
        }
    }
    if (!resume) {
        s_zb_ring_base = first;
        s_zb_ring_end = last;
        s_zb_ring_pos = read_dmem_u32(dm, 0xfecu) & 0xffffffu;
        if (mode == RING_XBUS || (flags & RING_OUTPUT_ONLY)) s_zb_ring_pos = first;
        else {
            if (s_backend->get_dpc_state) s_backend->get_dpc_state(dp);
            if (!(dp[2] & 1u) && first <= dp[0] && dp[1]
                && dp[1] < last && dp[1] != dp[0]) s_zb_ring_pos = dp[0];
        }
        s_zb_ring_live = (int)(mode | flags);
        if(mode==RING_GBI2 && (read_dmem_u32(dm,0xfc4u)&1u))
            s_zb_ring_pos=read_dmem_u32(dm,0xf0u)&0x7fffffu;
    }
    if (((unsigned int)s_zb_ring_live & RING_KIND) == mode) {
        s_output_only=((unsigned int)s_zb_ring_live&RING_OUTPUT_ONLY)!=0;
        fifo->write = guest_ring_write;
        fifo->read_staging = guest_ring_read_staging;
        fifo->group = guest_ring_group;
    }
}

static void guest_ring_finish(RdpFifo *fifo)
{
    if (s_ring_tail) {
        int32_t words[2];
        memcpy(words, s_backend->get_dmem() + s_ring_tail, 8);
        rdp_fifo_append(fifo, words, 2);
    }
    guest_ring_drain();
    if(((unsigned int)s_zb_ring_live&RING_OUTPUT_ONLY) &&
       s_zb_ring_end<=s_backend->get_rdram_size()-8u) {
        uint32_t count[2]={0,s_zb_ring_pos-s_zb_ring_base};
        memcpy(s_backend->get_rdram()+s_zb_ring_end,count,8);
    }
}

static void guest_ring_load_ucode(RdpFifo *fifo, unsigned int text,
                                  unsigned int data, unsigned int data_bytes)
{
    unsigned int kind = (unsigned int)s_zb_ring_live & RING_KIND;
    unsigned int pos;
    int full_reload=s_ring_full_reload;
    /* The newly loaded program runs its FIFO initialization as well as
     * replacing its data segment. Its cursor can restart at the ring base. */
    guest_ring_finish(fifo);
    pos = s_zb_ring_pos;
    /* GBI2's loader preserves DMEM 0..17f, including its ring cursor and
     * other-modes state; GBI1 reloads its entire data image. */
    guest_ring_config(fifo, text, 0, data & 0x7ffff8u, data_bytes,
                      !full_reload && (kind == RING_GBI2 || kind == RING_XBUS) ? 0x180u : 0u);
    memcpy(s_backend->get_dmem()+0xfd0u,&text,4);
    if (kind == RING_GBI2 && ((unsigned int)s_zb_ring_live & RING_KIND) == RING_GBI2
        && pos >= s_zb_ring_base && pos <= s_zb_ring_end)
        s_zb_ring_pos = pos;
}

static void guest_ring_begin(RdpFifo *fifo, unsigned int text, int resume)
{
    unsigned char *dm = s_backend->get_dmem();
    int yielded=!resume && (read_dmem_u32(dm,0xfc4u)&1u);
    fifo->load_ucode = guest_ring_load_ucode;
    guest_ring_config(fifo, text, resume,
                      read_dmem_u32(dm,yielded?0xff8u:0xfd8u)&0x7ffff8u,
                      read_dmem_u32(dm,yielded?0xffcu:0xfdcu),0);
}

/* bytes of RDP commands emitted by the current walker task, read by the
 * core to model the DP completion time (the boot calibration measures
 * the interval to the DP interrupt; a fixed forward delay reads as an
 * impossibly fast RDP and fails the measurement) */

static void zb_ring_flush_to_rdp(RdpFifo *f)
{
    unsigned char *rdram;
    unsigned int   i, need;
    if (f->used == 0)
        return;
    if (!s_zb_ring_live)
    {
        fifo_flush_to_rdp(f);
        return;
    }
    rdram = s_backend->get_rdram();
    need = f->used;
    if (s_zb_ring_pos + need > s_zb_ring_end)
        s_zb_ring_pos = s_zb_ring_base;    /* the wrap resets to base */
    if (s_zb_ring_pos + need > s_zb_ring_end)
    {
        /* a single batch larger than the ring cannot follow the
         * microcode's protocol; fall back to the host fifo */
        fifo_flush_to_rdp(f);
        return;
    }
    for (i = 0; i < need; i++)
        rdram[(s_zb_ring_pos + i) ^ 3u] =
            ((const unsigned char *)f->storage)[i ^ 3u];
    if (s_backend->submit)
        s_backend->submit(0, s_zb_ring_pos, need);
    s_zb_ring_pos += need;
    f->used = 0;
}

static int gsp_params_at_task_start;

/* F3DEX (v1) family classifier: 0 = not this family, 1 = F3D vertex family,
 * 2 = L3D line family. Defined below; forward-declared so the per-task setup
 * can route and configure the L3DEX line ucode the same way as the dispatcher. */
int f3d_ucode_family(const unsigned char *rdram, unsigned int rdram_size,
                            unsigned int ud, unsigned int uds);

/* Re-evaluate every microcode-build-specific parameter from a (data, text)
 * segment pair: the triangle-setup scale constants, the clip profile, the
 * near-plane mode, the clip-lerp build, the clip-fan orientation and the
 * G_BRANCH_Z/W flavor. Called at task start and again at every mid-list
 * G_LOAD_UCODE, since titles like Excitebike 64 swap between microcode
 * builds with different schemes inside one display list. */
void gsp_detect_ucode_params(GSPState *st, const unsigned char *rdram,
                             unsigned int rdram_size,
                             unsigned int ud, unsigned int ut)
{
    int ut_is_task_start = gsp_params_at_task_start;
    unsigned int ud_task;
    if(ut_is_task_start) {
        int sdk=ut<=rdram_size && 24u<=rdram_size-ut &&
            read_dmem_u32(rdram,ut)==0x4a10802au &&
            read_dmem_u32(rdram,ut+20u)==0x4b1f0051u;
        rsp_graphics_sdk_boot(sdk);
        if(sdk && (!rsp_graphics_sdk_one(5)||!rsp_graphics_sdk_one(7)) &&
           !(read_dmem_u32(s_backend->get_dmem(),0xfc4u)&1u)) {
            unsigned int slots[4]={0x2e0,0x2e8,0x410,0x418},k;
            unsigned char *dm=s_backend->get_dmem();
            for(k=0;k<4;k++) {
                uint32_t address=read_dmem_u32(dm,slots[k])+ut;
                memcpy(dm+slots[k],&address,4);
            }
            memcpy(dm+0xf0u,&s_zb_ring_pos,4);
        }
    }
    if(ut_is_task_start && ut<=rdram_size && 0x1800u<=rdram_size-ut &&
       ud<=rdram_size && 0x400u<=rdram_size-ud) {
        unsigned int at;
        for(at=0;at+16u<=0x1800u;at+=4u) {
            const unsigned int *w=(const unsigned int *)(rdram+ut+at);
            if(w[0]==0x4a052180u && w[1]==0x4a6630d0u &&
               w[2]==0x4ac61990u && w[3]==0x4a8639c0u) {
                unsigned int count=read_dmem_u32(rdram,ud+0x12cu);
                if((count&0x8000001fu)==0x80000000u &&
                   (count&0xfffu)>=32u && (count&0xfffu)<=256u) {
                    unsigned int k;
                    /* Boot DMA supplies two look-at records, the light
                     * array and its ambient entry even before a list
                     * issues its first lighting command. */
                    gsp_set_lookat(st,rdram,ud+0x1b0u,0);
                    gsp_set_lookat(st,rdram,ud+0x1d0u,1);
                    for(k=0;k<GSP_MAX_LIGHTS;k++)
                        gsp_set_light(st,rdram,ud+0x1f0u+k*32u,(int)k);
                    gsp_set_num_lights(st,(int)((count&0xfffu)/32u)-1);
                }
                break;
            }
        }
    }
    if (ut_is_task_start && ut <= rdram_size && 0x1000u <= rdram_size - ut) {
        unsigned int at;
        /* GBI1's matrix-update tail passes the native modelview and
         * projection slots to mtx_multiply. Boot DMA restores these slots,
         * including zero matrices saved by a push before the first load. */
        for (at = 0; at + 16u <= 0x1000u; at += 4u) {
            const unsigned int *w = (const unsigned int *)(rdram + ut + at);
            unsigned int mv = w[0] & 0xffffu, pr = w[1] & 0xffffu;
            if ((w[0] & 0xffff0000u) == 0x20010000u &&
                (w[1] & 0xffff0000u) == 0x20020000u &&
                (w[2] & 0xfc000000u) == 0x08000000u &&
                (w[3] & 0xffff0000u) == 0x841f0000u &&
                !(mv & 7u) && pr == mv + 64u && pr + 64u <= 0xfc0u) {
                gsp_seed_matrices(st, s_backend->get_dmem(), mv, pr);
                break;
            }
        }
    }
    /* Star Wars Episode I Racer ships a LucasArts-customised F3DEX2 whose
     * data segment banner still reads "RSP Gfx ucode F3DEX.NoN   fifo 2.08"
     * (a GBI 1 name on a GBI 2 command set). The build drops the far-plane
     * outcode: the game's projection places all world geometry a fraction
     * beyond z == w, so the stock +z screen code trivially rejected every
     * world triangle and only the HUD survived. Key on the exact oddity --
     * an "F3DEX" stem NOT followed by '2' together with the GBI 2 "fifo"
     * token. That name heuristic alone is insufficient: stock F3DEX FIFO
     * 2.05 also omits the trailing '2', but retains far-plane rejection. */
    st->clip_no_far = 0;
    if (rdram != 0 && ud != 0)
    {
        static const char pre[] = "RSP Gfx ucode F3DEX";
        unsigned int plen = sizeof(pre) - 1u;
        unsigned int hi = ud + 0x1000u;
        unsigned int b;
        if (hi > rdram_size)
            hi = rdram_size;
        for (b = ud; b + plen + 24u <= hi; b++)
        {
            unsigned int k;
            for (k = 0; k < plen; k++)
                if (rdram[(b + k) ^ 3] != (unsigned char)pre[k])
                    break;
            if (k != plen)
                continue;
            if (rdram[(b + plen) ^ 3] != (unsigned char)'2')
            {
                unsigned int j, lim = b + plen + 24u;
                for (j = b + plen; j + 4u <= lim; j++)
                    if (rdram[(j + 0u) ^ 3] == 'f' && rdram[(j + 1u) ^ 3] == 'i' &&
                        rdram[(j + 2u) ^ 3] == 'f' && rdram[(j + 3u) ^ 3] == 'o')
                    {
                        st->clip_no_far = 1;
                        break;
                    }
            }
            break;
        }
    }
    /* The identified 2.05 resident tests AND & 0x7070 at IMEM 0x818,
     * including +Z, and loads clip-trigger mask 0x30304040. Disabling its
     * far outcodes fabricates hundreds of triangles per combat task. */
    if (f3dex205_ucode_match(rdram, rdram_size, ut))
        st->clip_no_far = 0;
    if (ut <= rdram_size && 0xf80u <= rdram_size-ut) {
        unsigned int at;
        /* A stock banner can also omit the "2". Its resident triangle
         * entry still ANDs all three outcodes with 0x7070, including +Z. */
        for (at=0;at+44u<=0xf80u;at+=4u) {
            const unsigned int *w=(const unsigned int *)(rdram+ut+at);
            if (w[0]==0x8c250024u && w[2]==0x8c460024u &&
                w[4]==0x8c670024u && (w[6]==0x30ab7070u || w[6]==0x30ab70b0u) &&
                w[8]==0x00cb5824u && w[10]==0x00eb5824u) {
                st->clip_no_far=0;
                break;
            }
        }
    }
    /* The data address at a mid-list G_LOAD_UCODE comes from the staged
     * G_RDPHALF_1 word, which display lists also use for branch targets;
     * validate that it really points at an F3DEX2-family data segment
     * before trusting any of its fields. Every known build carries the
     * v31 constant row prefix ffff 0004 0008 7f00 at data + 0x1b0. */
    /* The other-modes default pair below self-validates on its 0xEF
     * command byte and applies only at task start, when the data address
     * is the OSTask's own field; a pure S2DEX2 segment (no 3D constants,
     * so no v31 row -- Worms Armageddon's terrain tasks) must still get
     * its seed. Keep the unvalidated address for that read only. */
    ud_task = (ut_is_task_start
               && ud != 0
               && ud + 0xd0 <= rdram_size) ? ud : 0;
    unsigned int constants_shift=0;
    if (ut <= rdram_size && 12u <= rdram_size-ut &&
        ud <= rdram_size && 0x220u <= rdram_size-ud &&
        read_dmem_u32(rdram,ut+4u)==0xc81f2021u &&
        read_dmem_u32(rdram,ut+8u)==0xc81e2022u)
        constants_shift=0x60u;
    if (ud != 0 && ud + 0x1c0 + constants_shift <= rdram_size)
    {
        if (!(read_dmem_u32(rdram,ud+0x1b0u+constants_shift)==0xffff0004u &&
              read_dmem_u32(rdram,ud+0x1b4u+constants_shift)==0x00087f00u))
            ud = 0;
    }
    else
        ud = 0;
    /* Triangle-setup scale constants live in a microcode constants vector
     * loaded from the ucode data segment. Their location and lane layout
     * changed between revisions:
     *   F3DEX2/F3DZEX2 2.08: vector at data+0x1C0; dX scale lane 2
     *     (0x1000), VCR crimp bound lane 3 (0x0100), anchor fraction
     *     mask lane 5 (0xFFF8), inverse-dY scale lane 7 (0x0020).
     *   F3DEX2/F3DZEX2 2.0xH (e.g. 2.04H/2.06H): vector at data+0x1B0;
     *     fraction mask lane 0 (0xFFFF), inverse-dY lane 2 (0x0008),
     *     dX scale lane 5 (0x4000); the VCR bound (0x01CC) sits in the
     *     following vector's lane 2 (data+0x1C4).
     * The dX and inverse-dY scales split one 17-bit fixed-point factor, so
     * dxs * idys == 0x20000 in every revision; use that to validate which
     * layout the shipped data segment actually uses instead of trusting
     * offsets blindly (reading the 2.08 slots on a 2.0xH segment returns
     * unrelated constants and mis-sets every triangle edge). If neither
     * layout validates, keep the 2.0xH defaults. */
    {
        /* F3DEXBG relocates the two loaded vectors by 0x60. The plane
         * table remains at its original address, so relocate this lookup
         * without relocating the validated data address used below. */
        unsigned int ud_constants=ud ? ud+constants_shift : 0;
        unsigned int ud=ud_constants;
        if (ud != 0 && ud + 0x1d0 <= rdram_size)
        {
            int32_t dxs  = (int32_t)((rdram[(ud + 0x1c4) ^ 3] << 8)
                                   |  rdram[(ud + 0x1c5) ^ 3]);
            int32_t idys = (int32_t)((rdram[(ud + 0x1ce) ^ 3] << 8)
                                   |  rdram[(ud + 0x1cf) ^ 3]);
            int32_t fmsk = (int32_t)((rdram[(ud + 0x1ca) ^ 3] << 8)
                                   |  rdram[(ud + 0x1cb) ^ 3]);
            int32_t vcrb = (int32_t)((rdram[(ud + 0x1c6) ^ 3] << 8)
                                   |  rdram[(ud + 0x1c7) ^ 3]);
            if (!(dxs > 0 && idys > 0 && (int64_t)dxs * idys == 0x20000))
            {
                dxs  = (int32_t)((rdram[(ud + 0x1ba) ^ 3] << 8)
                               |  rdram[(ud + 0x1bb) ^ 3]);
                idys = (int32_t)((rdram[(ud + 0x1b4) ^ 3] << 8)
                               |  rdram[(ud + 0x1b5) ^ 3]);
                fmsk = (int32_t)((rdram[(ud + 0x1b0) ^ 3] << 8)
                               |  rdram[(ud + 0x1b1) ^ 3]);
                vcrb = (int32_t)((rdram[(ud + 0x1c4) ^ 3] << 8)
                               |  rdram[(ud + 0x1c5) ^ 3]);
            }
            if (dxs > 0 && idys > 0 && (int64_t)dxs * idys == 0x20000)
                gsp_set_tri_scales(st, dxs, idys, fmsk, vcrb);

        }
    }


    /* Seed the other-modes mirror from the microcode data defaults
     * (pair at ucode_data + 0xc8 -> DMEM 0xc8): the baseline the partial
     * 0xE2/0xE3 writes merge into for games that never send a wholesale
     * G_RDPSETOTHERMODE. Applied after the per-task reset. */
    {
        /* the task-start address: the seed self-validates on its 0xEF
         * command byte, so the F3DEX2 v31-row gate is not required and
         * would reject pure S2DEX2 segments */
        unsigned int uds = ud ? ud : ud_task;
        if (uds != 0 && uds + 0xd0 <= rdram_size)
        {
            unsigned int oh = ((unsigned int)rdram[(uds + 0xc8) ^ 3] << 24)
                            | ((unsigned int)rdram[(uds + 0xc9) ^ 3] << 16)
                            | ((unsigned int)rdram[(uds + 0xca) ^ 3] << 8)
                            |  (unsigned int)rdram[(uds + 0xcb) ^ 3];
            unsigned int ol = ((unsigned int)rdram[(uds + 0xcc) ^ 3] << 24)
                            | ((unsigned int)rdram[(uds + 0xcd) ^ 3] << 16)
                            | ((unsigned int)rdram[(uds + 0xce) ^ 3] << 8)
                            |  (unsigned int)rdram[(uds + 0xcf) ^ 3];
            /* The microcode's own gSPLoadUcode path preserves the
             * other-modes state across the swap (Kirby 64's S2DEX
             * round-trips validate pixel-exact only without a mid-list
             * re-seed), so the data-segment defaults only apply at task
             * start. */
            if (ut_is_task_start && (oh >> 24) == 0xefu)
                f3dex2_set_othermode_init(oh, ol);

            /* near clip-plane row (clip-ratio table at data + 0x180, row 5
             * = bytes +0x1a8..0x1af): NoN microcodes store {0,0,0,1} (the
             * near plane is just w), standard near clipping {0,0,1,1}
             * (z + w). Adopt the z coefficient when the row validates. */
            if (ud + 0x1b0 <= rdram_size)
            {
                int16_t nz = (int16_t)((rdram[(ud + 0x1ac) ^ 3] << 8)
                                      | rdram[(ud + 0x1ad) ^ 3]);
                int16_t nw = (int16_t)((rdram[(ud + 0x1ae) ^ 3] << 8)
                                      | rdram[(ud + 0x1af) ^ 3]);
                if (nw == 1 && (nz == 0 || nz == 1))
                    st->clip_near_z = (int)nz;
            }
        }
    }

    /* Clip-lerp build probe: F3DEX2 2.05+ and F3DZEX2 guard the boundary
     * lerp's sign extraction with a vor 1 (opcode word 4b015f6a in every
     * such build's clip overlay); the 2.04H build feeds the raw sum to
     * vabs. Scan the microcode text image, overlays included, for the
     * instruction and pick the build accordingly. */
    {
        /* ut from caller */
        int found = 0;
        if (ut != 0 && ut + 0x1800 <= rdram_size)
        {
            unsigned int k;
            for (k = 0; k + 4 <= 0x1800; k += 4)
            {
                if (rdram[(ut + k + 0) ^ 3] == 0x4bu
                    && rdram[(ut + k + 1) ^ 3] == 0x01u
                    && rdram[(ut + k + 2) ^ 3] == 0x5fu
                    && rdram[(ut + k + 3) ^ 3] == 0x6au)
                {
                    found = 1;
                    break;
                }
            }
        }
        /* Plain Fast3D (Super Mario 64 et al.) is neither F3DEX2 2.05+ nor
         * the 2.04H build: its clip overlay uses the standard reciprocal
         * sign step, not the 2.04H vabs-of-zero quirk. The probe above only
         * distinguishes 2.05+ (guard instruction present) from "everything
         * else", which would wrongly hand Fast3D the 2.04H behaviour and
         * collapse the boundary-vertex fade to 0 wherever an off vertex
         * lands exactly on a clip plane (cr.off == 0) -- e.g. the SM64
         * title's screen-edge background tiles, which then lose their
         * texture and render gray. Force the standard lerp for Fast3D. The
         * L3DEX line ucode is Fast3D-family too but lacks the version word, so
         * fold in the data-segment family check (uds unknown here, the scanner
         * defaults its bound); the "fifo" guard inside it keeps F3DEX2 builds,
         * which reach this through their own path, classified as non-family. */
        if (f3d_is_ucode(rdram, rdram_size, ut)
            || f3d_ucode_family(rdram, rdram_size, ud, 0u) != 0)
            found = 1;
        rsp_set_clip_lerp_204h(!found);
    }

    /* Rejection-microcode probe: F3DLX.Rej / F3DZEX.Rej carry ".Rej" in
     * the microcode ID string. The string sits in the shared text/data
     * region the ucode_data DMA brings in, at data + 0x146 (the
     * displayListStack/ID area, 0x138-0x180 in the microcode's DMEM map;
     * the v31 constant row this detector already keys on is at data +
     * 0x1b0 in the same segment). These builds have no polygon clipper:
     * a triangle past a fixed rejection bound is dropped whole rather
     * than subdivided, so the emitter must not run guard-band clipping
     * for them. Excitebike 64's title runs its 3D model under F3DLX.Rej
     * while its 2D/HUD passes use F3DEX.NoN. */
    {
        int rej = 0;
        if (ud != 0 && ud + 0x160 <= rdram_size)
        {
            unsigned int k;
            for (k = 0x130; k + 4 <= 0x160; k++)
            {
                if (rdram[(ud + k + 0) ^ 3] == 0x2eu    /* . */
                    && rdram[(ud + k + 1) ^ 3] == 0x52u /* R */
                    && rdram[(ud + k + 2) ^ 3] == 0x65u /* e */
                    && rdram[(ud + k + 3) ^ 3] == 0x6au)/* j */
                {
                    rej = 1;
                    break;
                }
            }
        }
        st->clip_reject = rej;
        if(rej)
            gsp_set_tri_scales(st,0x4000,8,0xffff,0x7fff);
    }

    /* F3DFLX.Rej ("FLX" rejection build, used by F-Zero X's in-race racer
     * models) does not implement the lookat texture-coordinate generator: it
     * leaves the vertex-supplied S/T in place even while G_TEXTURE_GEN is set
     * in the geometry mode. Detect the build by name so the emitter honors the
     * explicit per-vertex texcoords instead of running standard texgen. */
    {
        int flx = 0;
        if (ud != 0 && ud + 0x160 <= rdram_size)
        {
            unsigned int k;
            for (k = 0x100; k + 6 <= 0x160; k++)
            {
                if (rdram[(ud + k + 0) ^ 3] == 0x46u    /* F */
                    && rdram[(ud + k + 1) ^ 3] == 0x33u /* 3 */
                    && rdram[(ud + k + 2) ^ 3] == 0x44u /* D */
                    && rdram[(ud + k + 3) ^ 3] == 0x46u /* F */
                    && rdram[(ud + k + 4) ^ 3] == 0x4cu /* L */
                    && rdram[(ud + k + 5) ^ 3] == 0x58u)/* X */
                {
                    flx = 1;
                    break;
                }
            }
        }
        st->no_texgen = flx;
    }

    /* Clip-fan probe: the 2.04H clipper triangulates its output polygon
     * from the FIRST vertex with descending pairs (its draw loop reads
     * the pair through the output cursor: lhu v0, 0x3cc(s5) = 96a203cc),
     * where 2.05+/F3DZEX2 fans ascending pairs from the LAST vertex
     * (lhu v0, 0x3cc(s2)). Scan the text image for the 2.04H form. */
    {
        /* ut from caller */
        int first = 0;
        if (ut != 0 && ut + 0x1800 <= rdram_size)
        {
            unsigned int k;
            for (k = 0; k + 4 <= 0x1800; k += 4)
            {
                if (rdram[(ut + k + 0) ^ 3] == 0x96u
                    && rdram[(ut + k + 1) ^ 3] == 0xa2u
                    && rdram[(ut + k + 2) ^ 3] == 0x03u
                    && rdram[(ut + k + 3) ^ 3] == 0xccu)
                {
                    first = 1;
                    break;
                }
            }
        }
        st->clip_fan_first = first;
    }

    /* G_BRANCH_Z/W probe: opcode 0x04 compares a vertex field against the
     * command word and branches to the staged RDPHALF_1 list when it is
     * smaller. F3DEX2 builds (e.g. 2.04H) load the 32-bit screen-Z word
     * at vertex+0x1c (lhu rX, 0x380(rX); lw rY, 0x1c(rX)); F3DZEX2
     * builds load the s16 clip-W integer at vertex+0x6 (lh rY, 0x6(rX)).
     * Scan the text for the table-lookup/load pair, masking the register
     * fields. */
    {
        /* ut from caller */
        int z_mode = 0;
        if (ut != 0 && ut + 0x1800 <= rdram_size)
        {
            unsigned int k;
            for (k = 0; k + 8 <= 0x1800; k += 4)
            {
                unsigned int i0 = ((unsigned int)rdram[(ut + k + 0) ^ 3] << 24)
                                | ((unsigned int)rdram[(ut + k + 1) ^ 3] << 16)
                                | ((unsigned int)rdram[(ut + k + 2) ^ 3] << 8)
                                |  (unsigned int)rdram[(ut + k + 3) ^ 3];
                unsigned int i1 = ((unsigned int)rdram[(ut + k + 4) ^ 3] << 24)
                                | ((unsigned int)rdram[(ut + k + 5) ^ 3] << 16)
                                | ((unsigned int)rdram[(ut + k + 6) ^ 3] << 8)
                                |  (unsigned int)rdram[(ut + k + 7) ^ 3];
                if ((i0 & 0xfc00ffffu) == 0x94000380u)
                {
                    if ((i1 & 0xfc00ffffu) == 0x8c00001cu)
                    {
                        z_mode = 1;
                        break;
                    }
                    if ((i1 & 0xfc00ffffu) == 0x84000006u)
                    {
                        z_mode = 0;
                        break;
                    }
                }
            }
        }
        st->branch_z_mode = z_mode;
    }
}

/* Recognise the F3DEX (v1) graphics-microcode family generically, by the
 * human-readable signature SGI embeds in every Gfx microcode's data segment:
 * "RSP Gfx ucode F3DEX ..." and its siblings (F3DLX/F3DLP/F3DEX.NoN), plus the
 * line variant "RSP Gfx ucode L3DEX ...". The task's OSTask carries the data-
 * segment pointer and size at DMEM 0xfd8/0xfdc; the string sits a few hundred
 * bytes in, so a single bounded scan over the (~2 KiB) data segment classifies
 * the microcode without a per-game text-CRC allowlist.
 *
 * Returns 0 for anything else (the plain Fast3D builds in Super Mario 64 and
 * Wave Race 64 carry an "RSP SW Version" string and never match, keeping their
 * divide-by-ten / divide-by-five decode; S2DEX/ZSort and friends fall through
 * too), 1 for the vertex family ("F3D" stem) and 2 for the line family ("L3D"
 * stem, gspL3DEX). Both 1 and 2 use the v1 (n<<10)|(16n-1) vertex count and the
 * halved triangle indices; 2 additionally draws G_LINE3D as a real two-vertex
 * line, so the automap that L3DEX renders needs both the variant decode and the
 * line expansion. F3DEX2 (GBI 2) is routed to its own walker before this runs.
 * One short scan replaces (and costs less than) the chain of full-text CRCs it
 * supersedes. */
int f3d_ucode_family(const unsigned char *rdram,
                            unsigned int rdram_size,
                            unsigned int ud, unsigned int uds)
{
    static const char pre[] = "RSP Gfx ucode ";
    unsigned int plen = sizeof(pre) - 1u;
    unsigned int hi, b;
    if (rdram == 0 || ud == 0)
        return 0;
    if (uds == 0 || uds > 0x4000u)
        uds = 0x1000u;          /* sane bound if the size field is absent/odd */
    hi = ud + uds;
    if (hi > rdram_size)
        hi = rdram_size;
    if (hi < ud + plen + 3u)
        return 0;
    for (b = ud; b + plen + 3u <= hi; b++)
    {
        unsigned int k;
        unsigned char c0, c1, c2;
        for (k = 0; k < plen; k++)
            if (rdram[(b + k) ^ 3] != (unsigned char)pre[k])
                break;
        if (k != plen)
            continue;
        c0 = rdram[(b + plen + 0u) ^ 3];
        c1 = rdram[(b + plen + 1u) ^ 3];
        c2 = rdram[(b + plen + 2u) ^ 3];
        if (!((c0 == 'F' || c0 == 'L') && c1 == '3' && c2 == 'D'))
            return 0;           /* S2DEX, ZSortp, etc. -- not this family */
        /* L3DEX and F3DEX each exist in a GBI 1 build ("... 1.21") and a GBI 2
         * build ("... fifo 2.xx"); only GBI 1 belongs on the F3D walker. The
         * stem width varies (F3DEX, F3DEX.NoN, L3DEX), so scan the short name
         * field that follows it for the "fifo" token that tags every GBI 2
         * build and reject those. */
        {
            unsigned int j, lim = b + plen + 28u;
            if (lim > hi) lim = hi;
            for (j = b + plen + 3u; j + 4u <= lim; j++)
            {
                if (rdram[(j + 0u) ^ 3] == 'f' && rdram[(j + 1u) ^ 3] == 'i' &&
                    rdram[(j + 2u) ^ 3] == 'f' && rdram[(j + 3u) ^ 3] == 'o')
                    return 0;
                /* The GBI2 XBUS builds retain the F3DEX/F3DLX names too.
                 * GBI1 XBUS identifies itself with a 1.xx version. */
                if (j + 7u <= lim && rdram[(j+0u)^3]=='x' && rdram[(j+1u)^3]=='b' &&
                    rdram[(j+2u)^3]=='u' && rdram[(j+3u)^3]=='s' &&
                    rdram[(j+4u)^3]==' ' && rdram[(j+5u)^3]=='2' && rdram[(j+6u)^3]=='.')
                    return 0;
            }
        }
        return (c0 == 'L') ? 2 : 1;
    }
    return 0;
}

/* Recognise standalone S2DEX (GBI 1) by the SGI data-segment signature
 * "RSP Gfx ucode S2DEX  1.xx ...". Yoshi's Story, Bangai-O and other
 * sprite-driven GBI 1 titles carry a build-specific text version word
 * (e.g. 0x4a00002c) that probe_ucode_class does not match, so without this
 * they fall through to the F3DEX2 walker and render nothing.
 *
 * The match keys on the task's *primary* (first) "RSP Gfx ucode " signature,
 * not on any S2DEX string anywhere in the data segment. The combined SDK
 * ucode objects shipped by a large class of F3DEX2 titles -- Dr. Mario 64,
 * AI Shougi 3, GT64, Carmageddon 64, Big Mountain 2000, 64 Oozumou 2, Densha
 * de Go!, Doraemon 3, ... -- keep a resident "RSP Gfx ucode S2DEX  1.07 ..."
 * author string deeper in the *same* data segment as their F3DEX2 microcode.
 * A scan that returned on the first S2DEX hit anywhere therefore misrouted
 * every one of those F3DEX2 tasks to the sprite decoder and rendered black.
 * The task's own microcode is named by the first signature in its segment, so
 * we require that first signature to be S2DEX (and GBI 1: not "S2DEX2", and
 * with no "fifo"/"xbus" GBI 2 token, which the c81f2018 text probe already
 * routes as UCODE_S2DEX2). */
int s2dex1_ucode_match(const unsigned char *rdram,
                              unsigned int rdram_size,
                              unsigned int ud, unsigned int uds)
{
    static const char pre[] = "RSP Gfx ucode ";
    unsigned int plen = sizeof(pre) - 1u;   /* 14 */
    unsigned int hi, b;
    if (rdram == 0 || ud == 0)
        return 0;
    if (uds == 0 || uds > 0x4000u)
        uds = 0x1000u;
    hi = ud + uds;
    if (hi > rdram_size)
        hi = rdram_size;
    if (hi < ud + plen + 6u)
        return 0;
    for (b = ud; b + plen + 6u <= hi; b++)
    {
        unsigned int k;
        for (k = 0; k < plen; k++)
            if (rdram[(b + k) ^ 3] != (unsigned char)pre[k])
                break;
        if (k != plen)
            continue;
        /* First signature located: it identifies the task's own microcode. */
        if (rdram[(b + plen + 0u) ^ 3] != 'S' ||
            rdram[(b + plen + 1u) ^ 3] != '2' ||
            rdram[(b + plen + 2u) ^ 3] != 'D' ||
            rdram[(b + plen + 3u) ^ 3] != 'E' ||
            rdram[(b + plen + 4u) ^ 3] != 'X')
            return 0;           /* primary ucode is F3DEX/L3DEX/etc., not S2DEX */
        if (rdram[(b + plen + 5u) ^ 3] == '2')
            return 0;           /* "S2DEX2" -> GBI 2, routed by the text probe */
        /* GBI 2 S2DEX is "S2DEX       fifo 2.0x" / "... xbus 2.0x"; the GBI 1
         * build carries a bare "1.xx" version with no fifo/xbus token. Reject
         * the GBI 2 form so only the genuine S2DEX 1.xx sprite ucode matches. */
        {
            unsigned int j, lim = b + plen + 28u;
            if (lim > hi) lim = hi;
            for (j = b + plen + 5u; j + 4u <= lim; j++)
            {
                if (rdram[(j + 0u) ^ 3] == 'f' && rdram[(j + 1u) ^ 3] == 'i' &&
                    rdram[(j + 2u) ^ 3] == 'f' && rdram[(j + 3u) ^ 3] == 'o')
                    return 0;
                if (rdram[(j + 0u) ^ 3] == 'x' && rdram[(j + 1u) ^ 3] == 'b' &&
                    rdram[(j + 2u) ^ 3] == 'u' && rdram[(j + 3u) ^ 3] == 's')
                    return 0;
            }
        }
        return 1;
    }
    return 0;
}

/* Recognise the F3DEX (v1, GBI 1) family from the *structure* of its data
 * segment, with no dependence on the "RSP Gfx ucode ..." signature string.
 * The Rare engine titles (Banjo-Kazooie/Tooie, Diddy Kong Racing, Donkey
 * Kong 64, Jet Force Gemini) ship the stock gspF3DEX.fifo microcode with that
 * author string zeroed out, so f3d_ucode_family()'s name scan misses them and
 * the dispatcher leaves the F3D walker in its plain-Fast3D decode (gSPVertex
 * count ((w0>>20)&0xf)+1, gSP1Triangle indices /10). F3DEX v1 is GBI 1 and
 * needs the n<<10 / index*2 decode instead, so without recognising the family
 * their geometry collapses.
 *
 * These markers were read out of the public SDK gspF3DEX.fifo object (.data,
 * SP_UCODE_DATA_SIZE = 0x800) and hold across the whole v1 vertex family
 * (F3DEX/F3DLX, NoN and base):
 *   - data+0x1b0 is NOT the F3DEX2 v31 constant row ffff 0004 0008 7f00 (that
 *     row is the positive F3DEX2 marker the dispatcher already keys on, so its
 *     absence keeps this test off every GBI 2 build);
 *   - data+0x118 carries the 0xef-tagged G_SETOTHERMODE init word the F3D
 *     walker already reads from this offset (the dispatcher's f3d othermode
 *     seed), present in the F3DEX-family data image and not in F3DEX2's;
 *   - the GBI command dispatch table at data+0xb8 is densely populated with
 *     IMEM handler addresses (every entry in [0x1000,0x2000) until the 0x0000
 *     terminator): the v1 build fills all 39 slots, where a GBI 2 segment has
 *     no such table here at all.
 * Requiring all three keeps the test specific to the F3DEX v1 GBI-1 vertex
 * family; it is used only to *select the GBI 1 decode*, never to re-route, so
 * a miss leaves existing behaviour untouched. */
static int f3dex1_data_family(const unsigned char *rdram,
                              unsigned int rdram_size, unsigned int ud)
{
    unsigned int i, dense, total;
    if (rdram == 0 || ud == 0 || ud + 0x1c0u > rdram_size)
        return 0;
    /* reject F3DEX2 (v31 constant row present) */
    if (rdram[(ud + 0x1b0) ^ 3] == 0xffu && rdram[(ud + 0x1b1) ^ 3] == 0xffu
        && rdram[(ud + 0x1b2) ^ 3] == 0x00u && rdram[(ud + 0x1b3) ^ 3] == 0x04u
        && rdram[(ud + 0x1b4) ^ 3] == 0x00u && rdram[(ud + 0x1b5) ^ 3] == 0x08u
        && rdram[(ud + 0x1b6) ^ 3] == 0x7fu && rdram[(ud + 0x1b7) ^ 3] == 0x00u)
        return 0;
    /* F3D-family othermode-init tag */
    if (rdram[(ud + 0x118) ^ 3] != 0xefu)
        return 0;
    /* dense GBI dispatch table at data+0xb8 */
    dense = 0;
    total = 0;
    for (i = 0; i < 48u; i++)
    {
        unsigned int v = ((unsigned int)rdram[(ud + 0xb8 + i * 2u) ^ 3] << 8)
                       |  (unsigned int)rdram[(ud + 0xb9 + i * 2u) ^ 3];
        if (v == 0u)
            break;
        total++;
        if (v >= 0x1000u && v < 0x2000u)
            dense++;
    }
    return (total >= 30u && dense == total) ? 1 : 0;
}

/* Recognise a GBI 1 F3D-family data segment by its other-mode init tag alone,
 * with no dependence on the name string or the dense GBI dispatch table that
 * f3dex1_data_family() requires. Wipeout 64 re-loads its F3DLX build through a
 * second data image that carries neither: the only string in it is "RSP SW
 * Version: 2.0H" at +0x270, and the 0xb8 dispatch table is absent, so both the
 * f3d_ucode_family() name scan and f3dex1_data_family() miss it and the task
 * falls through to the F3DEX2 walker -- which then mis-decodes its GBI 1
 * vertex/triangle stream (every intro and menu screen renders black or, where
 * a mis-read opcode lands on the BG handler, as garbage).
 *
 * data+0x118 holds the 0xef-tagged G_SETOTHERMODE word the F3D walker already
 * seeds from; it is present across the F3DEX v1 (GBI 1) family and absent on
 * every F3DEX2/S2DEX2 (GBI 2) build, which instead carry 0x00 there and the
 * ffff0004 0008 7f00 v31 constant row at data+0x1b0. Requiring the tag present
 * and that v31 row absent keeps this strictly off GBI 2. It is used only for an
 * otherwise-unclassified task (neither the text version probe nor the name
 * family matched), so plain Fast3D keeps its own decode; the caller further
 * excludes the GBI 1 S2DEX sprite ucode by name, leaving only the F3DEX/F3DLX/
 * L3DEX vertex-line family to route onto the F3D walker. */
static int f3d_gbi1_othermode_data(const unsigned char *rdram,
                                   unsigned int rdram_size, unsigned int ud)
{
    if (rdram == 0 || ud == 0 || ud + 0x1c0u > rdram_size)
        return 0;
    if (rdram[(ud + 0x1b0) ^ 3] == 0xffu && rdram[(ud + 0x1b1) ^ 3] == 0xffu
        && rdram[(ud + 0x1b2) ^ 3] == 0x00u && rdram[(ud + 0x1b3) ^ 3] == 0x04u
        && rdram[(ud + 0x1b4) ^ 3] == 0x00u && rdram[(ud + 0x1b5) ^ 3] == 0x08u
        && rdram[(ud + 0x1b6) ^ 3] == 0x7fu && rdram[(ud + 0x1b7) ^ 3] == 0x00u)
        return 0;                       /* F3DEX2/S2DEX2 v31 row -> GBI 2 */
    return rdram[(ud + 0x118) ^ 3] == 0xefu;
}

static int guest_yield_gbi1(const unsigned char *ram,unsigned int size,
                            unsigned int text,unsigned int data,unsigned int bytes,
                            unsigned int saved[5])
{
    unsigned int at;
    if(!ram || text>size || 0xf80u>size-text || data>size ||
       bytes<0x900u || bytes>0xfc0u || bytes>size-data)return 0;
    for(at=0;at+20u<=0xf80u;at+=4u) {
        const unsigned int *w=(const unsigned int *)(ram+text+at);
        if((w[0]&0xffff0000u)==0x8c170000u &&
           (w[1]&0xffff0000u)==0x8c1c0000u &&
           (w[2]&0xffff0000u)==0x8c1b0000u &&
           (w[3]&0xfc000000u)==0x08000000u &&
           (w[4]&0xffff0000u)==0x8c1a0000u) {
            unsigned int k;
            for(k=0;k<4;k++) {
                unsigned int off=w[k==3?4:k]&0xffffu;
                if((off&3u)||off+4u>bytes)return 0;
                saved[k]=read_dmem_u32(ram,data+off);
            }
            if(saved[1]>0xfc0u || saved[2]>0xfc0u)return 0;
            if(saved[1]<saved[2]) {
                /* Older Fast3D stores a remaining-byte count in gp and
                 * advances k0 on every command. Later GBI1 instead keeps
                 * the DMEM buffer end in gp and the next DMA base in k0. */
                unsigned int k,remaining=0;
                for(k=0;k+8u<=0x1000u && k+8u<=size-text;k+=4u)
                    if(read_dmem_u32(ram,text+k)==0x237b0008u &&
                       read_dmem_u32(ram,text+k+4u)==0x239cfff8u) {remaining=1;break;}
                if(!remaining || saved[1]>0x140u)return 0;
                saved[4]=saved[3]&0x7fffffu;
            }else saved[4]=(saved[3]-(saved[1]-saved[2]))&0x7fffffu;
            return saved[4] && saved[4]<size-8u;
        }
    }
    return 0;
}

int rdp_emit_hle_yield_supported(const unsigned char *ram,unsigned int size,
                                unsigned int text,unsigned int data,unsigned int bytes)
{
    unsigned int pc,saved[5];
    if(guest_yield_gbi1(ram,size,text,data,bytes,saved))return 1;
    if(!ram || text>size || 64u>size-text || data>size ||
       bytes<0xc00u || bytes>0xfc0u || bytes>size-data)return 0;
    pc=read_dmem_u32(ram,data+0xbf8u)&0x7fffffu;
    return pc && pc<size-8u &&
        read_dmem_u32(ram,text+4u)==0xc81f201bu &&
        read_dmem_u32(ram,text+8u)==0xc81e201cu &&
        read_dmem_u32(ram,text+0x3cu)==0x8c1a0bf8u;
}

int rdp_emit_hle_supported(const unsigned char *ram, unsigned int size,
                          unsigned int text, unsigned int data, unsigned int bytes)
{
    unsigned int i;
    static const char *names[] = {"F3D", "S2DEX", "L3D"};
    if (zsort_ucode_match(ram, size, data, bytes)) return 1;
    if (!ram || size < 0x1b8u || data > size || text > size) return 0;
    if (f3d_is_ucode(ram, size, text) || f3d_is_seta_ucode(ram, size, text) ||
        f3dex1_data_family(ram, size, data) || f3d_gbi1_othermode_data(ram, size, data) ||
        turbo3d_ucode_match(ram, size, text) || t3dux_ucode_match(ram, size, data) ||
        f3ddkr_is_ucode(ram, size, data, bytes)) return 1;
    /* Stock F3DEX2 builds may leave OSTask.ucode_data_size zero. Their v31
     * constant row still identifies the GBI, without guessing from task type. */
    if (data && data <= size - 0x1b8u &&
        read_dmem_u32(ram, data + 0x1b0u) == 0xffff0004u &&
        read_dmem_u32(ram, data + 0x1b4u) == 0x00087f00u) return 1;
    /* Named stock and custom F3D/S2DEX/L3D builds share the emitter's classifiers.
     * Unidentified graphics tasks use LLE rather than guessing a GBI. */
    if (!bytes || bytes > 0x1000) bytes = 0x1000;
    if (data >= size || bytes > size - data) return 0;
    for (i = 0; i + 5 <= bytes; i++) {
        unsigned int n;
        for (n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
            unsigned int j;
            for (j = 0; names[n][j] && ram[(data + i + j) ^ 3] == (unsigned char)names[n][j]; j++) {}
            if (!names[n][j]) return 1;
        }
    }
    return 0;
}

void rdp_emit_hle_process_dlist(void)
{
    unsigned char *rdram;
    unsigned char *dmem;
    unsigned int   rdram_size;
    unsigned int   dl_addr;
    unsigned int   fifo_base;

    if (s_backend == 0 || s_backend->get_rdram == 0
        || s_backend->get_dmem == 0 || s_backend->get_rdram_size == 0)
        return;

    rdram      = s_backend->get_rdram();
    dmem       = s_backend->get_dmem();
    rdram_size = s_backend->get_rdram_size();
    if (rdram == 0 || dmem == 0 || rdram_size == 0)
        return;

    /* forget any fullsync seen by a previous task before the parsers run */
    rdp_fifo_fullsync_reset();

    /* Ucode-profile flags are per-task: clear the Turbo3D/Wipeout vertex
     * store and triangle-writer behaviors here so a task that never sets
     * them (an F3DEX2 list after a T3DUX one in a mixed-ucode title) does
     * not inherit the previous task's profile. The T3DUX walker and the
     * F3D wo64 detection re-assert what they need after this point. */
    rsp_set_vtx_y_round(0);
    rsp_set_vtx_x_round(0);
    rsp_set_vtx_invw_raw(0);
    rsp_set_tri_attr_rs(0);
    s_gsp.viewport.rs_model = 0;
    rsp_set_vtx_z_quant(0);
    rsp_set_keep_degenerate(0);
    rsp_set_affine_tex(0);
    rsp_set_attr_lowp(0);


    if (!s_inited)
    {
        gsp_init(&s_gsp);
        s_inited = 1;
    }
    s_gsp.mvp_trans_last = 0;
    gsp_task_reset(&s_gsp);

    /* the FIFO lives in host memory; the top 256 KiB of RDRAM is used
     * only as the virtual address range the DPC registers report */
    if (rdram_size < (512u * 1024u))
        return;
    fifo_base = rdram_size - HLE_FIFO_CAP;
    rdp_fifo_init(&s_fifo, s_fifo_storage, fifo_base, HLE_FIFO_CAP);
    s_fifo.flush = fifo_flush_to_rdp;
    s_fifo.time = s_backend->time;

    dl_addr = read_dmem_u32(dmem, TASK_DATA_PTR_DMEM) & 0x00ffffffu;
    if (dl_addr == 0 || dl_addr >= rdram_size)
        return;

    guest_ring_begin(&s_fifo, read_dmem_u32(dmem, 0xfd0u) & 0xffffffu, 0);


    /* The OSTask names the RDRAM matrix stack (dram_stack at DMEM 0xfe0,
     * dram_stack_size at 0xfe4); the microcode pushes and pops through
     * it and drops pushes at base + size. */
    {
        unsigned int ms = read_dmem_u32(dmem, 0xfe0u) & 0x00ffffffu;
        unsigned int msz = read_dmem_u32(dmem, 0xfe4u);
        if (ms >= rdram_size || msz > 0x1000u)
            ms = 0;
        gsp_set_matrix_stack(&s_gsp, rdram, ms, msz);
    }

    /* walk the display list, emitting RDP commands into the FIFO. textured/
     * z_buffered default off here; a gDP/state-translation follow-up sets the
     * render mode and the per-frame setup commands. */
    f3dex2_seg_reset();
    {
        unsigned int ut = read_dmem_u32(dmem, 0xfd0) & 0x00ffffffu;
        unsigned int ud = read_dmem_u32(dmem, 0xfd8) & 0x00ffffffu;
        int fam;
        int gbi1_oth;
        int seta;
        unsigned int ud_om;
        gsp_params_at_task_start = 1;
        gsp_detect_ucode_params(&s_gsp, rdram, rdram_size, ud, ut);
        gsp_params_at_task_start = 0;
        /* Classify the Gfx ucode family from the SGI name string in its data
         * segment: the F3D vertex family (F3DEX/F3DLX/F3DLP, fam 1) and the L3D
         * line family (L3DEX, fam 2) are both GBI 1 and both belong on the F3D
         * walker. f3d_is_ucode routes the vertex ucodes by their SM64-shaped
         * version word, but the L3DEX line ucode boots differently and has no
         * such word, so route it here by family instead -- otherwise Doom 64's
         * and Hexen's automaps fall through to the F3DEX2 walker and scatter. */
        fam = f3d_ucode_family(rdram, rdram_size, ud,
                               read_dmem_u32(dmem, 0xfdc));
        /* Otherwise-unclassified task (no text version match, no name family):
         * route a nameless GBI 1 F3D-family data image onto the F3D walker
         * instead of letting it fall through to F3DEX2. Excludes the GBI 1
         * S2DEX sprite ucode (matched by name) so only the vertex-line family
         * is re-routed. */
        gbi1_oth = (!f3d_is_ucode(rdram, rdram_size, ut) && fam == 0)
                && f3d_gbi1_othermode_data(rdram, rdram_size, ud)
                && !s2dex1_ucode_match(rdram, rdram_size, ud,
                                       read_dmem_u32(dmem, 0xfdc));
        /* SETA's custom 2.0D build (Eikou no Saint Andrews) misses every
         * probe above (see f3d_is_seta_ucode); its lists are stock GBI 1
         * commands with the plain Fast3D geometry conventions -- G_VTX packs
         * (n-1)<<4 in the parameter byte with 16-byte vertices, and the
         * triangle indices are times ten, confirmed from its live attract
         * display list -- so it routes to the F3D walker with the plain
         * decode, not the GBI 1 x2 or the F3DBETA one. */
        seta = f3d_is_seta_ucode(rdram, rdram_size, ut);
        gbi1_oth = gbi1_oth || seta;
        if (rs_ucode_match(rdram, rdram_size, ut))
        {
            /* Factor 5 custom microcode (Star Wars: Rogue Squadron). No SGI
             * name string; the task text is a loader probed by its first
             * instruction words. Compacted GBI 1 derivative with 8-byte
             * packed vertices and per-face colour lists; see rdp_emit_rs.c. */
            rs_set_rdram(rdram);
            rs_set_rdram_size(rdram_size);
            rs_seed_fog_row(dmem);
            f3dex2_set_rdram(rdram);
            f3dex2_set_rdram_size(rdram_size);
            rs_run_dl(&s_gsp, &s_fifo, dl_addr);
            rsp_tri_set_rs_sort(0);
            rsp_tri_set_d64_sort(0);
            s_gsp.rs_clip_model = 0;
            s_gsp.clip_fan_first = 0;
        }
        else if (zsort_ucode_match(rdram, rdram_size, ud, read_dmem_u32(dmem, 0xfdc)))
        {
            zsort_run_dl(&s_gsp, &s_fifo, rdram, rdram_size, dmem, dl_addr);
        }
        else if (turbo3d_ucode_match(rdram, rdram_size, ut))
        {
            /* Original SGI Turbo3D (Dark Rift): the "RSP SW Version: 2.0D"
             * fast/low-quality microcode, identified by the CRC of its text
             * segment. Its display list is a flat array of four-word objects
             * (global-state, per-object-state, vertices, triangles) with
             * standard N64 vertices and triangles -- not a Fast3D/F3DEX
             * grammar. Route to the dedicated walker, checked first so its
             * task is never mis-claimed by the F3D name/family probes (its
             * data segment carries the same SGI version string). */
            turbo3d_set_rdram(rdram);
            turbo3d_set_rdram_size(rdram_size);
            f3dex2_set_rdram(rdram);
            f3dex2_set_rdram_size(rdram_size);
            turbo3d_run_dl(&s_gsp, &s_fifo, dl_addr);
        }
        else if (t3dux_ucode_match(rdram, rdram_size, ud))
        {
            /* T3DUX (Turbo3D UX): Yasumoto's compact Turbo3D format (Last
             * Legion UX, Toukon Road). Not a Fast3D/F3DEX grammar at all --
             * the display list is a flat array of six-word objects, each with
             * five segment pointers (global state, per-object state, vertices,
             * triangles, colours). Route to the dedicated walker, checked
             * first so its unique data segment is never mis-claimed. */
            t3dux_set_rdram(rdram);
            t3dux_set_rdram_size(rdram_size);
            f3dex2_set_rdram(rdram);
            f3dex2_set_rdram_size(rdram_size);
            t3dux_run_dl(&s_gsp, &s_fifo, dl_addr);
        }
        else if (f3ddkr_is_ucode(rdram, rdram_size, ud,
                            read_dmem_u32(dmem, 0xfdc)))
        {
            /* F3DDKR (Diddy Kong Racing custom microcode): a GBI 1 derivative
             * with its own command set (gSPPolygon batched triangles, indexed
             * matrices, billboarding). Neither the F3D nor F3DEX2 walker can
             * decode it; route to the dedicated walker. Checked first so its
             * custom data segment is never mis-claimed by the stock paths. */
            f3ddkr_seg_reset();
            f3ddkr_set_rdram(rdram);
            f3ddkr_set_rdram_size(rdram_size);
            f3ddkr_seed_task(&s_gsp, rdram, rdram_size, ud);
            f3ddkr_run_dl(&s_gsp, &s_fifo, dl_addr, 0, 0);
        }
        else if ((f3d_is_ucode(rdram, rdram_size, ut) || fam != 0 || gbi1_oth)
                 && !s2dex1_ucode_match(rdram,rdram_size,ud,read_dmem_u32(dmem,0xfdc)))
        {
            /* Plain Fast3D (e.g. Super Mario 64): different geometry opcode
             * encoding from F3DEX2, dispatched separately. gsp_detect_ucode_
             * params() validates ud against the F3DEX2 clip-table signature
             * and zeroes it for F3D, so F3D never inherits its data-segment
             * defaults there. Seed the other-modes mirror from the F3D data
             * segment here instead: the default pair lives at ud+0x118 (not
             * the F3DEX2 ud+0xc8). Partial SETOTHERMODE_H/L writes merge into
             * this baseline; without it bit 19 (G_MDSFT_TEXTPERSP) and the
             * dither/filter defaults are zero, so perspective-correct
             * texturing is off and the deformed head mesh smears and holes. */
            f3d_seg_reset();
            f3d_set_rdram(rdram);
            f3d_set_rdram_size(rdram_size);
            /* GBI 1 (F3DEX v1) needs the n<<10 / index*2 vertex+triangle
             * decode. f3d_ucode_family() matches it by the SGI name string,
             * but the Rare-engine titles (Banjo-Kazooie/Tooie, DKR, DK64,
             * Jet Force Gemini) strip that string, so fall back to the
             * structural data-segment family probe. Either signal selects the
             * GBI 1 decode; plain Fast3D (SM64) matches neither and keeps its
             * own path. The line variant stays gated on the name family (only
             * gspL3DEX, which the structural probe does not assert). */
            /* Blast Corps' line-capable Fast3D build (J-Bomb bonus scenes)
             * keeps the plain Fast3D vertex/index encoding but its data
             * segment trips the GBI 1 othermode probe, which would select
             * the n<<10 vertex decode (count field reads zero, so no
             * vertices load and the gSPLine3D segments collapse or connect
             * stale slots). Recognise the build by text CRC and pin the
             * plain decode with the line opcode enabled. */
            {
                int bcline = f3d_is_bcline_ucode(rdram, rdram_size, ut);
                int bhline = f3d_is_bhline_ucode(rdram, rdram_size, ut);
                f3d_set_variant(!bcline && !seta
                                && (fam != 0
                                    || f3dex1_data_family(rdram, rdram_size, ud)
                                    || gbi1_oth
                                    || f3d_is_early_ucode(rdram,rdram_size,ut)));
                f3d_set_variant_early(f3d_is_early_ucode(rdram,rdram_size,ut));
                if (f3d_is_early_ucode(rdram,rdram_size,ut)) {
                    s_gsp.mvp_trans_last = 1;
                    gsp_set_tri_scales(&s_gsp, 0x4000, 8, 0xfff8, 0x1cc);
                }
                f3d_set_line_variant(fam == 2 || bcline || bhline);
                f3d_set_variant_bhline(bhline);
                f3d_set_variant_ff2d(f3d_is_ff2d_ucode(rdram, rdram_size, ut));
            }
            f3d_set_variant_wr64(f3d_is_wr64_ucode(rdram, rdram_size, ut));
            f3d_set_variant_seta(seta);
            /* GoldenEye 007 / Perfect Dark run an early F3DEX (GBI 1) build
             * whose RSP version word at text+4 (0x201d0110) is the same one
             * SM64's plain Fast3D carries, so it reaches this F3D walker. It
             * is NOT Fast3D: per its gbi, vertex triangle indices are scaled
             * x2 (not x10) and opcode 0xB1 is G_TRI2 (two triangles per
             * command), which carries the bulk of the world geometry. Decoded
             * as Fast3D, 0xB1 is dropped and 0xBF reads the wrong indices, so
             * most of each level fails to render. Select the F3DEX decode by
             * the build's distinct first instruction word at text+0. */
            {
                unsigned int t0 = (ut + 4u <= (rdram_size ? rdram_size
                                                          : 0x800000u))
                    ? (((unsigned int)rdram[ut ^ 3] << 24)
                       | ((unsigned int)rdram[(ut + 1) ^ 3] << 16)
                       | ((unsigned int)rdram[(ut + 2) ^ 3] << 8)
                       |  (unsigned int)rdram[(ut + 3) ^ 3])
                    : 0u;
                /* Perfect Dark: GoldenEye's F3DEX GBI1 decode (x10 G_TRI1
                 * indices, 0xB1 G_TRIX) plus its own colour-indexed vertex
                 * record and the 0x07 G_VTXCOLORBASE command. Its boot jump
                 * word at text+0 is 0x090005ee where GoldenEye's build is
                 * 0x090005ea. */
                f3d_set_variant_f3dex(t0 == 0x090005eau
                                      || t0 == 0x090005eeu);
                f3d_set_variant_pd(t0 == 0x090005eeu);
                f3d_set_near_plane_from_data(rdram, rdram_size, ud);
                /* Wipeout 64's F3DLX fork: raw-doubled saturated vertex
                 * 1/w and the seedless l3dex clip fold (build id = the
                 * boot jump word, same keying as the F3DEX probe). */
                f3d_set_variant_wo64(t0 == 0x090005d8u);
                /* Star Wars: Shadows of the Empire runs an early "RSP SW
                 * Version: 2.0D, 04-01-96" graphics build (text+0 boot word
                 * 0x090005f8). It carries the SM64 version word at text+4
                 * (0x201d0110), so it reaches this F3D walker rather than the
                 * F3DEX2 one. Its geometry is encoded like Wave Race 64's
                 * F3DBETA -- gSPVertex packs the count as n<<9 (not n<<10) and
                 * gSP1Triangle / G_QUAD index vertices *5 (recovered by /5) --
                 * NOT the x2 F3DEX / Doom 64 encoding, confirmed from its live
                 * display list. Select it by its boot signature below. */
                if (t0 == 0x090005f8u)
                {
                    /* Correction: this build's geometry is not the x2 (d64)
                     * F3DEX encoding but the F3DBETA one Wave Race 64 uses,
                     * confirmed from its live display list -- gSPVertex packs
                     * the count as n<<9 (not n<<10) and gSP1Triangle / G_QUAD
                     * index vertices *5 (recovered by /5), not *2. Its data
                     * segment carries the 0xef G_SETOTHERMODE tag, so the
                     * gbi1_oth probe above already selected the x2 decode;
                     * decoded that way only half of each batch's vertices load
                     * and the triangle indices fall on stale slots, stretching
                     * the model into off-screen streaks. Clear the x2 decode
                     * and select the wr64 (n<<9, /5) path instead. */
                    f3d_set_variant(0);
                    f3d_set_variant_wr64(1);
                }
            }
            /* The F3D data segment ships its other-mode default pair at
             * +0x118; SETA's custom layout keeps the same 0xEF pair at +0xb8. */
            if (seta || f3d_is_early_ucode(rdram,rdram_size,ut))
                ud_om = ud + 0xb8u <= rdram_size - 8u ? ud + 0xb8u : 0u;
            else
                ud_om = ud + 0x118u;
            if (ud != 0 && ud_om != 0u && ud_om + 8u <= rdram_size)
            {
                unsigned int oh = ((unsigned int)rdram[(ud_om + 0) ^ 3] << 24)
                                | ((unsigned int)rdram[(ud_om + 1) ^ 3] << 16)
                                | ((unsigned int)rdram[(ud_om + 2) ^ 3] << 8)
                                |  (unsigned int)rdram[(ud_om + 3) ^ 3];
                unsigned int ol = ((unsigned int)rdram[(ud_om + 4) ^ 3] << 24)
                                | ((unsigned int)rdram[(ud_om + 5) ^ 3] << 16)
                                | ((unsigned int)rdram[(ud_om + 6) ^ 3] << 8)
                                |  (unsigned int)rdram[(ud_om + 7) ^ 3];
                if ((oh >> 24) == 0xefu)
                    f3d_set_othermode_init(oh, ol);
            }
            if(read_dmem_u32(dmem,0xfc4u)&1u) {
                unsigned int saved[5],at,vertex_base=0x440u;
                if(guest_yield_gbi1(rdram,rdram_size,ut,
                    read_dmem_u32(dmem,0xff8u)&0x7fffffu,read_dmem_u32(dmem,0xffcu),saved)) {
                    for(at=0;at+16u<=0xf80u;at+=4u) {
                        const unsigned int *w=(const unsigned int *)(rdram+ut+at);
                        if((w[0]&0xffff0000u)==0x20010000u &&
                           (w[1]&0xffff0000u)==0x20020000u &&
                           (w[1]&0xffffu)==(w[0]&0xffffu)+64u) {
                            vertex_base=(w[0]&0xffffu)+0xc0u;break;
                        }
                    }
                    f3d_restore_yield(&s_gsp,dmem,vertex_base,s_gsp.clip_reject!=0);
                    f3d_restore_prefetch(dmem,saved[4],saved[2],
                        saved[1]<saved[2]?saved[1]:saved[1]-saved[2]);
                    s_zb_ring_pos=read_dmem_u32(dmem,0x128u)&0x7fffffu;
                    /* FIFO residents announce the previous DMA cursor
                     * before writing the next batch. A native yield can
                     * therefore leave completed guest-RAM commands beyond
                     * DPC_END. Deliver that pending prefix before the host
                     * walker emits new commands, without writing it again. */
                    if(s_backend->get_dpc_state && s_backend->submit) {
                        unsigned int dp[3];
                        s_backend->get_dpc_state(dp);
                        if(!(dp[2]&1u) && !(dp[0]&7u) &&
                           dp[0]>=s_zb_ring_base && dp[0]<s_zb_ring_pos &&
                           s_zb_ring_pos<=s_zb_ring_end && s_zb_ring_pos<=rdram_size)
                            s_backend->submit(rdram+dp[0],dp[0],s_zb_ring_pos-dp[0]);
                    }
                    {
                        unsigned int hint=*(const unsigned short *)(dmem+(0x15au^2u));
                        unsigned int bank=hint==s_ring_stage[1] && hint!=s_ring_stage[0];
                        unsigned int stage=s_ring_stage[bank];
                        unsigned int used=saved[0]>=stage?saved[0]-stage:0;
                        if(used<=0x1000u-stage)
                            s_zb_ring_live=(s_zb_ring_live&~0x1fffu)|(bank<<12)|used;
                    }
                    dl_addr=saved[4];
                    *(unsigned int *)(dmem+0xfc4u)=0;
                }
            }
            f3d_run_dl(&s_gsp, &s_fifo, dl_addr, 0, 0);
        }
        else
        {
            f3dex2_set_rdram(rdram);
            f3dex2_set_rdram_size(rdram_size);
            f3dex2_set_task_ucode(rdram, ut, ud);
            /* Conker's F3DEXBG.NoN build: detect by data-segment name and
             * enable the CBFD G_TRI4 opcode block (0x10..0x1f). */
            f3dex2_set_variant_cbfd(rdram, ud,
                                    read_dmem_u32(dmem, 0xfdc));
            /* Acclaim's four F3DEX2 titles (Turok 2/3, Armorines, South Park)
             * ship a custom-lighting microcode with a stock F3DEX.NoN name;
             * detect it by the code-text checksum and enable the L1-distance
             * point-lighting path. */
            f3dex2_set_variant_acclaim(rdram, ut);
            /* Standalone S2DEX (GBI 1) is not caught by the text version probe;
             * detect it by data-segment name and force the GBI 1 sprite class. */
            if (s2dex1_ucode_match(rdram, rdram_size, ud,
                                   read_dmem_u32(dmem, 0xfdc))) {
                f3dex2_force_class_s2dex1();
                if ((read_dmem_u32(dmem,0xc8u)>>24)==0xefu)
                    f3dex2_set_othermode_init(read_dmem_u32(dmem,0xc8u),read_dmem_u32(dmem,0xccu));
            }
            if((read_dmem_u32(dmem,0xfc4u)&1u) &&
               rdp_emit_hle_yield_supported(rdram,rdram_size,ut,
                   read_dmem_u32(dmem,0xff8u)&0x7fffffu,read_dmem_u32(dmem,0xffcu))) {
                dl_addr=f3dex2_restore_yield(&s_gsp,dmem);
                *(unsigned int *)(dmem+0xfc4u)=0;
            }
            f3dex2_run_dl(&s_gsp, &s_fifo, dl_addr, 0, 0);
        }
    }

    /* Ordinary walkers already emitted each FullSync in its original
     * position. Only packet builders that requested a deferred fence need
     * a terminator here.
     * A display list without any G_RDPFULLSYNC raises no DP interrupt on
     * hardware, and games keep track of which tasks full-sync: Blast Corps'
     * scheduler retires a frame on every DP-done message and dereferences a
     * NULL frame pointer when one arrives for a task that never requested a
     * full sync (its first display list after the Rareware logo), faulting
     * the scheduler thread and softlocking the game. */
    if (rdp_fifo_fullsync_pending())
    {
        int32_t sync[2];
        sync[0] = (int32_t)0x29000000u;
        sync[1] = 0;
        rdp_fifo_append(&s_fifo, sync, 2);
    }

    guest_ring_finish(&s_fifo);
    /* submit the final batch (everything since the last overflow flush,
     * or the whole frame when no flush happened). */
    fifo_flush_to_rdp(&s_fifo);

}


/* ------------------------------------------------------------------ */
/* Streaming (co-routine) display list service for rsp-hle.
 *
 * Gauntlet Legends' graphics microcode is an F3DEX2 2.0xH derivative
 * that runs one persistent task per frame: the CPU keeps appending to
 * the display list while the RSP walks it, and the list's live tail is
 * a G_DL branch to its own address that the CPU patches forward chunk
 * by chunk.  rsp-hle detects that microcode and drives this entry
 * instead of the one-shot ProcessDList path:
 *
 *   resume == 0  new task: full per-task setup, then walk until the
 *                list completes or its live tail is reached
 *   resume == 1  continue a suspended walk after the host CPU ran
 *
 * returns  0  list complete (task done)
 *          1  suspended at the live tail (task still running)
 *         -1  not serviceable (caller should fall back to LLE)
 *
 * The rsp-hle plugin and this renderer are statically linked into one
 * core, so the direct call mirrors the existing cxd4 forward. */
/* Rogue Squadron streaming display-list service: the RS microcode walks
 * a page ring that the CPU extends while the task runs, so the task is
 * dispatched through the incomplete-return protocol (see rs_gfx_task in
 * the RSP HLE). Returns 0 when the list completed, 1 while suspended at
 * the live tail, negative when the renderer cannot service it. */
int angrylion_rs_dlist(int resume)
{
    unsigned char *rdram;
    unsigned char *dmem;
    unsigned int   rdram_size;
    unsigned int   fifo_base;
    unsigned int   dl_addr;
    int r;

    if (s_backend == 0 || s_backend->get_rdram == 0
        || s_backend->get_dmem == 0 || s_backend->get_rdram_size == 0)
        return -1;

    rdram      = s_backend->get_rdram();
    dmem       = s_backend->get_dmem();
    rdram_size = s_backend->get_rdram_size();
    if (rdram == 0 || dmem == 0 || rdram_size == 0)
        return -1;

    {
        unsigned int ut = read_dmem_u32(dmem, 0xfd0) & 0x00ffffffu;
        if (!rs_ucode_match(rdram, rdram_size, ut))
            return -1;
    }

    fifo_base = rdram_size - HLE_FIFO_CAP;
    rdp_fifo_init(&s_fifo, s_fifo_storage, fifo_base, HLE_FIFO_CAP);
    s_fifo.flush = fifo_flush_to_rdp;
    dl_addr = read_dmem_u32(dmem, 0xff0u) & 0x00ffffffu;
    if (dl_addr == 0 || dl_addr >= rdram_size)
        return -1;

    if (!resume)
    {
        rdp_fifo_fullsync_reset();
        if (!s_inited)
        {
            gsp_init(&s_gsp);
            s_inited = 1;
        }
        gsp_task_reset(&s_gsp);
    }

    guest_ring_begin(&s_fifo, read_dmem_u32(dmem, 0xfd0u) & 0xffffffu, resume);

    rs_set_rdram(rdram);
    rs_set_rdram_size(rdram_size);
    if (!resume) rs_seed_fog_row(dmem);
    f3dex2_set_rdram(rdram);
    f3dex2_set_rdram_size(rdram_size);
    r = rs_run_dl_streaming(&s_gsp, &s_fifo, dl_addr, resume);

    guest_ring_finish(&s_fifo);

    /* Rogue forwards the original RDP FullSync in place. Task completion
     * must not add a second DP fence. */
    /* submit everything generated in this slice */
    fifo_flush_to_rdp(&s_fifo);

    return r;
}

#include "rdp_emit_naboo.h"

static void fifo_discard(RdpFifo *fifo) { fifo->used = 0; }

int angrylion_naboo_dlist(int resume, int emit)
{
    unsigned char *rdram;
    unsigned char *dmem;
    unsigned int   rdram_size;
    unsigned int   fifo_base;
    unsigned int   dl_addr;
    int r;

    if (s_backend == 0 || s_backend->get_rdram == 0
        || s_backend->get_dmem == 0 || s_backend->get_rdram_size == 0)
        return -1;

    rdram      = s_backend->get_rdram();
    dmem       = s_backend->get_dmem();
    rdram_size = s_backend->get_rdram_size();
    if (rdram == 0 || dmem == 0 || rdram_size == 0)
        return -1;

    fifo_base = rdram_size - HLE_FIFO_CAP;
    rdp_fifo_init(&s_fifo, s_fifo_storage, fifo_base, HLE_FIFO_CAP);
    s_fifo.flush = fifo_flush_to_rdp;

    dl_addr = read_dmem_u32(dmem, 0xff0u) & 0x00ffffffu;
    if (dl_addr == 0 || dl_addr >= rdram_size)
        return -1;

    if (!resume)
        rdp_fifo_fullsync_reset();

    naboo_set_rdram(rdram);
    naboo_set_rdram_size(rdram_size);
    naboo_set_emit(emit);
    if (!resume) {
        naboo_seed_dmem(dmem);
        if (!(read_dmem_u32(dmem, 0xfc4u) & 1u) && s_backend->get_dpc_state) {
            unsigned int dp[3];
            s_backend->get_dpc_state(dp);
            naboo_seed_output(dp[0], dp[1]);
        }
    }
    /* Probe before an overflow flush can send commands to the RDP. Roll
     * back particle DMA writes as well as private DMEM/emitter state. */
    {
        unsigned int ns = naboo_state_size(), rs = rsp_emit_state_size();
        unsigned int fs = f3dex2_state_size();
        unsigned char *saved = (unsigned char *)malloc(ns + rs + fs);
        struct hle_timing_work saved_work = hle_timing_get();
        if (!saved) return -1;
        naboo_state_save(saved);
        rsp_emit_state_save(saved + ns);
        f3dex2_state_save(saved + ns + rs);
        s_fifo.flush = fifo_discard;
        naboo_begin_writes();
        r = naboo_run_dl(&s_fifo, dl_addr, resume);
        naboo_end_writes(0);
        naboo_state_load(saved);
        rsp_emit_state_load(saved + ns);
        f3dex2_state_load(saved + ns + rs);
        free(saved);
        hle_timing_restore(saved_work);
        s_fifo.used = 0;
        s_fifo.flush = fifo_flush_to_rdp;
        if (r < 0) return r;
    }
    naboo_begin_writes();
    r = naboo_run_dl(&s_fifo, dl_addr, resume);
    naboo_end_writes(r >= 0);
    {
        static int done, fb, t = -1;
        if (t < 0) t = getenv("NB_STATS") != NULL;
        if (t) {
            if (r == NABOO_R_FALLBACK) fb++; else done++;
            fprintf(stderr, "[NBSTAT] done=%d fallback=%d\n", done, fb);
        }
    }

    /* On fallback the LLE rerun re-executes the whole list from its
     * head; anything this slice already appended would be emitted
     * twice. Discard instead of flushing. */
    if (r < 0) {
        s_fifo.used = 0;
        return r;
    }

    /* submit everything generated in this slice */
    fifo_flush_to_rdp(&s_fifo);

    return r;
}

int angrylion_streaming_dlist(int resume)
{
    unsigned char *rdram;
    unsigned char *dmem;
    unsigned int   rdram_size;
    unsigned int   fifo_base;
    int r;

    if (s_backend == 0 || s_backend->get_rdram == 0
        || s_backend->get_dmem == 0 || s_backend->get_rdram_size == 0)
        return -1;

    rdram      = s_backend->get_rdram();
    dmem       = s_backend->get_dmem();
    rdram_size = s_backend->get_rdram_size();
    if (rdram == 0 || dmem == 0 || rdram_size == 0)
        return -1;

    /* the FIFO submit machinery is per-slice */
    fifo_base = rdram_size - HLE_FIFO_CAP;
    rdp_fifo_init(&s_fifo, s_fifo_storage, fifo_base, HLE_FIFO_CAP);
    s_fifo.flush = fifo_flush_to_rdp;
    if (!resume)
    {
        unsigned int dl_addr;
        unsigned int ut, ud;

        rdp_fifo_fullsync_reset();

        rsp_set_vtx_y_round(0);
        rsp_set_vtx_x_round(0);
        rsp_set_vtx_invw_raw(0);
        rsp_set_tri_attr_rs(0);
        s_gsp.viewport.rs_model = 0;
        rsp_set_vtx_z_quant(0);
        rsp_set_keep_degenerate(0);
        rsp_set_affine_tex(0);
        rsp_set_attr_lowp(0);

        if (!s_inited)
        {
            gsp_init(&s_gsp);
            s_inited = 1;
        }
        s_gsp.mvp_trans_last = 0;
        gsp_task_reset(&s_gsp);

        /* Install the projection/cache callbacks after resetting task state.
         * Resetting after guest_ring_begin discarded the native VTX spills
         * on the first slice of streaming F3DEX2 tasks. */
        guest_ring_begin(&s_fifo, read_dmem_u32(dmem, 0xfd0u) & 0xffffffu, 0);

        dl_addr = read_dmem_u32(dmem, TASK_DATA_PTR_DMEM) & 0x00ffffffu;
        if (dl_addr == 0 || dl_addr >= rdram_size)
            return -1;

        {
            unsigned int ms = read_dmem_u32(dmem, 0xfe0u) & 0x00ffffffu;
            unsigned int msz = read_dmem_u32(dmem, 0xfe4u);
            if (ms >= rdram_size || msz > 0x1000u)
                ms = 0;
            gsp_set_matrix_stack(&s_gsp, rdram, ms, msz);
        }

        f3dex2_seg_reset();
        ut = read_dmem_u32(dmem, 0xfd0) & 0x00ffffffu;
        ud = read_dmem_u32(dmem, 0xfd8) & 0x00ffffffu;
        gsp_params_at_task_start = 1;
        gsp_detect_ucode_params(&s_gsp, rdram, rdram_size, ud, ut);
        gsp_params_at_task_start = 0;
        f3dex2_set_rdram(rdram);
        f3dex2_set_rdram_size(rdram_size);
        f3dex2_set_task_ucode(rdram, ut, ud);

        r = f3dex2_run_dl_streaming(&s_gsp, &s_fifo, dl_addr, 0);
    }
    else
    {
        guest_ring_begin(&s_fifo, read_dmem_u32(dmem, 0xfd0u) & 0xffffffu, 1);
        r = f3dex2_run_dl_streaming(&s_gsp, &s_fifo, 0, 1);
    }

    if (r == 0 && rdp_fifo_fullsync_pending())
    {
        int32_t sync[2];
        sync[0] = (int32_t)0x29000000u;
        sync[1] = 0;
        rdp_fifo_append(&s_fifo, sync, 2);
    }

    if (r == 0) guest_ring_finish(&s_fifo);

    /* submit everything generated in this slice */
    fifo_flush_to_rdp(&s_fifo);

    return r;
}


/* ------------------------------------------------------------------ */
/* ZSortBOSS display-list service for rsp-hle (World Driver
 * Championship, Stunt Racer 64). Same sliced transport as the Gauntlet
 * streaming service: rsp-hle owns SP_STATUS and only resumes the walk
 * once the wait condition returned by the previous slice is satisfied.
 *
 *   resume == 0  new task (full setup)
 *   resume == 1  continue after the wait condition cleared
 *
 * returns ZBOSS_R_DONE / ZBOSS_R_WAIT_SIG3 / ZBOSS_R_WAIT_SIG0, or -1
 * when the renderer cannot service the task (caller falls back). */
/* ---- zboss shadow: walk a task read-only during an LLE run, dumping
 * the emitted command words to stderr for oracle comparison. Free-runs
 * the CPU handshakes (record contents are task-stable) and never
 * touches the real RDP. */
static unsigned char s_shadow_storage[HLE_FIFO_CAP];
static unsigned char s_shadow_dmem[0x1000];

unsigned char *angrylion_zboss_shadow_dmem(void);
unsigned char *angrylion_zboss_shadow_dmem(void)
{
    return s_shadow_dmem;
}

static void shadow_flush(RdpFifo *f)
{
    unsigned int i, k;
    const int lens[64] = {
        2,2,2,2,2,2,2,2, 8,12,24,28,24,28,40,44,
        2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
        2,2,2,2,4,4,2,2, 2,2,2,2,2,2,2,2,
        2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2 };
    for (i = 0; i + 8u <= f->used; )
    {
        unsigned int w0, cmd, nw;
        w0 = (unsigned int)*(const int32_t *)(f->storage + i);
        cmd = (w0 >> 24) & 0x3fu;
        nw = (unsigned int)lens[cmd];
        if (i + nw * 4u > f->used)
            nw = (f->used - i) / 4u;
        fprintf(stderr, "S44 c=%02x", cmd);
        for (k = 0; k < nw; k++)
            fprintf(stderr, " %08x",
                    (unsigned int)*(const int32_t *)(f->storage + i + k * 4u));
        fprintf(stderr, "\n");
        i += nw * 4u;
    }
    f->used = 0;
}

int angrylion_zboss_shadow(unsigned char *rdram, unsigned int rdram_size,
                           unsigned char *dmem)
{
    /* the walker mutates DMEM (MOVEWORD, MOVEMEM, transform results,
     * the audio service's wrapped scribbles); the live LLE task about
     * to run this same DMEM must not see any of it, so the shadow
     * walks a private copy */
    RdpFifo sf;
    int r;
    struct hle_timing_work saved_work = hle_timing_get();
    void *saved_rsp = malloc(rsp_emit_state_size());
    if (!saved_rsp) return -1;
    rsp_emit_state_save(saved_rsp);
    memcpy(s_shadow_dmem, dmem, 0x1000);
    rdp_fifo_init(&sf, s_shadow_storage, rdram_size - HLE_FIFO_CAP,
                  HLE_FIFO_CAP);
    sf.flush = shadow_flush;
    r = zboss_run_shadow(rdram, rdram_size, s_shadow_dmem, &sf);
    fprintf(stderr, "SHADOW r=%d\n", r);
    shadow_flush(&sf);
    rsp_emit_state_load(saved_rsp);
    free(saved_rsp);
    hle_timing_restore(saved_work);
    return r;
}

int angrylion_zboss_dlist(int resume, unsigned int *sp_status)
{
    unsigned char *rdram;
    unsigned char *dmem;
    unsigned int   rdram_size;
    unsigned int   fifo_base;
    int r;

    if (s_backend == 0 || s_backend->get_rdram == 0
        || s_backend->get_dmem == 0 || s_backend->get_rdram_size == 0)
        return -1;

    rdram      = s_backend->get_rdram();
    dmem       = s_backend->get_dmem();
    rdram_size = s_backend->get_rdram_size();
    if (rdram == 0 || dmem == 0 || rdram_size == 0)
        return -1;

    fifo_base = rdram_size - HLE_FIFO_CAP;
    rdp_fifo_init(&s_fifo, s_fifo_storage, fifo_base, HLE_FIFO_CAP);
    s_fifo.flush = zb_ring_flush_to_rdp;

    if (!resume)
    {
        unsigned int rb, re;
        rdp_fifo_fullsync_reset();
        rb = ((unsigned int)dmem[0xfe8 ^ 3u] << 24)
           | ((unsigned int)dmem[0xfe9 ^ 3u] << 16)
           | ((unsigned int)dmem[0xfea ^ 3u] << 8)
           |  (unsigned int)dmem[0xfeb ^ 3u];
        re = ((unsigned int)dmem[0xfec ^ 3u] << 24)
           | ((unsigned int)dmem[0xfed ^ 3u] << 16)
           | ((unsigned int)dmem[0xfee ^ 3u] << 8)
           |  (unsigned int)dmem[0xfef ^ 3u];
        rb &= 0x00ffffffu;
        re &= 0x00ffffffu;
        if (rb != 0 && re > rb && re <= rdram_size)
        {
            s_zb_ring_base = rb;
            s_zb_ring_end  = re;
            s_zb_ring_pos  = rb;   /* the boot stub reloads the cursor
                                    * from the header every task */
            s_zb_ring_live = 1;
        }
        else
            s_zb_ring_live = 0;
    }

    r = zboss_run(rdram, rdram_size, dmem, &s_fifo,
                  resume ? ZBOSS_OP_RESUME : ZBOSS_OP_FRESH, sp_status);

    if (r == ZBOSS_R_DONE && rdp_fifo_fullsync_pending())
    {
        int32_t sync[2];
        sync[0] = (int32_t)0x29000000u;
        sync[1] = 0;
        rdp_fifo_append(&s_fifo, sync, 2);
    }

    zb_ring_flush_to_rdp(&s_fifo);
    return r;
}

#define TRANSPORT_FIELDS(X) X(s_zb_ring_base) X(s_zb_ring_end) X(s_zb_ring_pos) X(s_zb_ring_live)
EMIT_STATE_DEFINE(transport, TRANSPORT_FIELDS)
#undef TRANSPORT_FIELDS
unsigned int rdp_emit_stream_state_size(void)
{
    return f3dex2_state_size() + rs_state_size() + naboo_state_size()
        + zboss_state_size() + rsp_emit_state_size() + transport_state_size();
}
void rdp_emit_stream_save(void *buffer)
{
    unsigned char *p = (unsigned char *)buffer;
#define SAVE_PART(name) name##_state_save(p); p += name##_state_size();
    SAVE_PART(f3dex2) SAVE_PART(rs) SAVE_PART(naboo) SAVE_PART(zboss)
    SAVE_PART(rsp_emit) SAVE_PART(transport)
#undef SAVE_PART
}
void rdp_emit_stream_load(const void *buffer)
{
    const unsigned char *p = (const unsigned char *)buffer;
#define LOAD_PART(name) name##_state_load(p); if (p) p += name##_state_size();
    LOAD_PART(f3dex2) LOAD_PART(rs) LOAD_PART(naboo) LOAD_PART(zboss)
    LOAD_PART(rsp_emit) LOAD_PART(transport)
#undef LOAD_PART
}

unsigned int rdp_emit_stream_legacy_state_size(void)
{
    return rdp_emit_stream_state_size() - zboss_state_size() + zboss_legacy_state_size();
}
void rdp_emit_stream_load_legacy(const void *buffer)
{
    const unsigned char *p = (const unsigned char *)buffer;
#define LOAD_PART(name) name##_state_load(p); if (p) p += name##_state_size();
    LOAD_PART(f3dex2) LOAD_PART(rs) LOAD_PART(naboo)
    zboss_state_load_legacy(p); if (p) p += zboss_legacy_state_size();
    LOAD_PART(rsp_emit) LOAD_PART(transport)
#undef LOAD_PART
}
