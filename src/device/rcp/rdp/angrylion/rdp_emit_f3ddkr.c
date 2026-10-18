#include "../../rsp/hle/hle_timing.h"
/* Rare's F3DDKR / JFG display-list decoder. Command dispatch, DMA,
 * projection and clipping were decoded from the retail programs, including
 * their reciprocal and clip overlays. See benchmarks/RARE_UCODE_ACCURACY.md
 * for ROM identities, instruction ranges and native replay checks.
 *
 * DKR: 10-byte XYZ/RGBA vertices, count-1 plus retained append base;
 * JFG/Mickey: unbiased count and an explicit 7-bit destination;
 * 0x05: 16-byte flag/index/S/T triangle records;
 * 0x07: raw RDP DMA, never a recursively interpreted GBI list.
 */
#include "rdp_emit_f3ddkr.h"
#include "rdp_emit_f3d.h"
#include "rdp_emit_rsp.h"

#if defined(_MSC_VER) && (_MSC_VER < 1600)
typedef signed   __int32 dkr_int32_t;
#else
#include <stdint.h>
#endif

/* ---- F3DDKR command opcodes (from f3ddkr.h) ----------------------------- */
#define DKR_G_MTX     0x01
#define JFG_G_DMA_TEX_OFFSET 0x02  /* JFG/Mickey build only */
#define DKR_G_MOVEMEM 0x03
#define DKR_G_VTX     0x04
#define DKR_G_TRIN    0x05   /* gSPPolygon */
#define DKR_G_DL      0x06
#define DKR_G_DMADL   0x07   /* gDkrDmaDisplayList */
#define DKR_G_MOVEWORD 0xBC
#define DKR_G_ENDDL   0xB8
#define DKR_G_SETGEOMETRYMODE   0xB7
#define DKR_G_CLEARGEOMETRYMODE 0xB6
#define DKR_G_TEXTURE 0xBB
#define DKR_G_SETOTHERMODE_H    0xBA
#define DKR_G_SETOTHERMODE_L    0xB9
#define JFG_G_DMA_OFFSETS       0xBF  /* JFG/Mickey build only */

/* G_MOVEWORD indices used by DKR. The index byte is the low 8 bits of w0
 * (gImmp21 packs G_MOVEWORD as cmd<<24 | offset<<8 | index). G_MW_SEGMENT is the stock
 * F3D segment-base move that gSPSegment/rsp_segment emit: DKR uses it to point
 * SEGMENT_FRAMEBUFFER/ZBUFFER/MAIN at the live buffers each frame, so the
 * walker must record it or the SET_COLOR_IMAGE/SET_*_IMAGE seg lookups below
 * resolve against a zero base and angrylion renders over low RDRAM (incl. the
 * 0x180 exception vectors -> CPU wedges in the exception handler). */
#define DKR_MW_BILLBOARD 0x02
#define DKR_MW_FOG       0x08
#define DKR_MW_SEGMENT   0x06
#define DKR_MW_MVPMATRIX 0x0A

/* G_VTX_APPEND lives in the full w0 word (GLideN64 tests w0 & 0x00010000);
 * the gSPVertexDKR macro ORs v0's low bit, which lands here. */
#define F3DDKR_VTX_APPEND 0x00010000u

#define DKR_DL_MAX_DEPTH 11

/* ---- module state -------------------------------------------------------- */
static unsigned char *s_rdram = 0;
static unsigned int   s_rdram_size = 0;
static unsigned int   s_seg[16];
static int   s_textured  = 0;
static int   s_zbuffered = 0;
static int   s_dl_depth  = 0;
static int   s_mtx_slot  = 0;   /* active MVP slot selected by gSPSelectMatrixDKR */
static int   s_billboard = 0;   /* nonzero adds the previous vertex-0 anchor */
static unsigned int s_othermode_h = 0;
static unsigned int s_othermode_l = 0;
static int   s_vtx_top   = 0;   /* preceding non-append count, retained across draws */
static unsigned int s_geom = 0; /* raw GBI 1 geometry mode; translated on write */
/* JFG/Mickey: three-bit destination/source matrix slots, multiply bit 23,
 * vertex/matrix DMA offsets, and a DMA'd 32-halfword texture shift table. */
static int   s_jfg = 0;
static unsigned int s_dma_mtx = 0;  /* G_DMA_OFFSETS matrix base */
static unsigned int s_dma_vtx = 0;  /* G_DMA_OFFSETS vertex base */
static unsigned int s_tex_ofs = 0;  /* G_DMA_TEX_OFFSET table address */
static unsigned int s_tex_count = 0;
static unsigned short s_tex_offsets[32];
static unsigned int s_rdp_half = 0;
/* ---- helpers (mirror rdp_emit_f3d.c) ------------------------------------ */
static unsigned int rd32(const unsigned char *r, unsigned int a)
{
    return ((unsigned int)r[(a + 0) ^ 3] << 24)
         | ((unsigned int)r[(a + 1) ^ 3] << 16)
         | ((unsigned int)r[(a + 2) ^ 3] << 8)
         |  (unsigned int)r[(a + 3) ^ 3];
}

static unsigned int seg_rsp(unsigned int w1)
{
    unsigned int seg = (w1 >> 24) & 0x0f;
    return s_seg[seg] + (w1 & 0x00ffffffu);
}

static unsigned int seg_phys(unsigned int w1)
{
    return seg_rsp(w1) & 0x00ffffffu;
}

static int in_range(unsigned int a, unsigned int bytes)
{
    return (a != 0u) && (a + bytes <= s_rdram_size);
}

/* ---- public setup -------------------------------------------------------- */
void f3ddkr_seg_reset(void)
{
    int i;
    for (i = 0; i < 16; i++)
        s_seg[i] = 0u;
    s_textured  = 0;
    s_zbuffered = 0;
    s_dl_depth  = 0;
    s_mtx_slot  = 0;
    s_billboard = 0;
    s_vtx_top   = 0;
    s_geom      = 0u;
    s_othermode_h = 0;
    s_othermode_l = 0;
    s_dma_mtx   = 0u;
    s_dma_vtx   = 0u;
    s_tex_ofs   = 0u;
    s_tex_count = 0u;
    s_rdp_half = 0u;
    /* s_jfg is not reset here: f3ddkr_is_ucode() sets it per task, and the
     * hle.c dispatch calls that detector before this reset. */
}

void f3ddkr_set_rdram(unsigned char *rdram)   { s_rdram = rdram; }
void f3ddkr_set_rdram_size(unsigned int size) { s_rdram_size = size; }

void f3ddkr_seed_task(GSPState *gsp, const unsigned char *rdram,
                           unsigned int rdram_size, unsigned int ud)
{
    unsigned int w0, w1;
    if (rdram == 0 || ud == 0 || ud + 0x120 > rdram_size)
        return;
    w0 = ((unsigned int)rdram[(ud + 0x118) ^ 3] << 24)
       | ((unsigned int)rdram[(ud + 0x119) ^ 3] << 16)
       | ((unsigned int)rdram[(ud + 0x11a) ^ 3] << 8)
       |  (unsigned int)rdram[(ud + 0x11b) ^ 3];
    w1 = ((unsigned int)rdram[(ud + 0x11c) ^ 3] << 24)
       | ((unsigned int)rdram[(ud + 0x11d) ^ 3] << 16)
       | ((unsigned int)rdram[(ud + 0x11e) ^ 3] << 8)
       |  (unsigned int)rdram[(ud + 0x11f) ^ 3];
    if ((w0 >> 24) != 0xefu)
        return;
    s_othermode_h = w0;
    s_othermode_l = w1;
    {
        int plane, lane;
        for (plane=0;plane<6;plane++)
            for (lane=0;lane<4;lane++) {
                unsigned int a=ud+0x98u-plane*8u+lane*2u;
                gsp->dkr_clip_rows[plane][lane]=(short)
                    ((rdram[a^3u]<<8)|rdram[(a+1u)^3u]);
            }
        gsp_set_persp_norm(gsp, rd32(rdram,ud+0x110u)&0xffffu);
        s_geom=rd32(rdram,ud+0x114u);
        gsp_set_geometry_mode(gsp,f3d_xlate_geom(s_geom));
        if (ud + 0x420u <= rdram_size) {
            unsigned int bank=s_jfg ? 0x220u : 0x210u;
            unsigned int fog=rd32(rdram,ud+0x1e0u);
            for (lane=0;lane<8;lane++)
                gsp_matrix_dkr(gsp,rdram,ud+bank+lane*64u,lane,0,0);
            s_mtx_slot=(int)((rd32(rdram,ud+0x148u)>>6)&(s_jfg?7u:3u));
            gsp_select_matrix_dkr(gsp,s_mtx_slot);
            gsp_set_viewport(gsp,rdram,ud+0x1d0u);
            gsp_set_fog(gsp,(short)(fog>>16),(short)fog);
            s_billboard=rd32(rdram,ud+0x144u)!=0u;
            s_vtx_top=(short)(rd32(rdram,ud+0x14cu)>>16);
            s_textured=(int)((s_geom>>1)&1u);
            gsp->tex_tile=(int)((rd32(rdram,ud+0x120u)>>8)&7u);
            gsp->tex_level=(int)((rd32(rdram,ud+0x120u)>>11)&7u);
            for(lane=0;lane<16;lane++) s_seg[lane]=rd32(rdram,ud+0x160u+lane*4u);
        }
    }
    s_zbuffered = (((s_othermode_l >> 4) & 1u) ||
                   ((s_othermode_l >> 5) & 1u)) ? 1 : 0;
}

/* Positive fingerprints in the resident primary/secondary dispatch tables.
 * DKR and JFG/Mickey are separate programs; they share the command family
 * but differ in DMA field layout. Do not infer a build from a NOP pattern. */
int f3ddkr_is_ucode(const unsigned char *rdram, unsigned int rdram_size,
                    unsigned int ud, unsigned int uds)
{
    unsigned short tbl[16];
    int i;
    (void)uds;
    if (rdram == 0 || ud == 0 || ud + 0xbc + 16u * 2u > rdram_size)
        return 0;
    for (i = 0; i < 16; i++)
        tbl[i] = (unsigned short)(((unsigned int)rdram[(ud + 0xbc + i * 2u) ^ 3] << 8)
                                 | (unsigned int)rdram[(ud + 0xbd + i * 2u) ^ 3]);
    if (tbl[0] == 0x1340 && tbl[5] == 0x1358
        && tbl[7] == 0x145c && tbl[8] == 0x146c)
    {
        s_jfg = 0;
        return 1;
    }
    /* The captured retail JFG and Mickey resident/overlay images match. */
    if (tbl[0] == 0x1390 && tbl[5] == 0x13a8
        && tbl[7] == 0x1504 && tbl[8] == 0x1514)
    {
        s_jfg = 1;
        return 1;
    }
    return 0;
}

/* ---- the F3DDKR walker --------------------------------------------------- */
static void f3ddkr_run_dl_impl(GSPState *gsp, RdpFifo *fifo, unsigned int addr,
                               int textured, int z_buffered);


void f3ddkr_run_dl(GSPState *gsp, RdpFifo *fifo, unsigned int addr,
                   int textured, int z_buffered)
{
    /* Both builds use z+w near clipping, a reversed stable fan, and the
     * doubled/Newton divide with a final +3 factor accumulator seed. */
    gsp->clip_near_z = 1;
    gsp->clip_fan_first = 2;
    rsp_set_clip_lerp_l3dex(1);
    /* Preserve Rare's raw S/T domain while selecting its exact screen model. */
    gsp_set_rsp_screen_model(gsp, 0);
    gsp->viewport.dkr_model = 1;
    rsp_set_tri_attr_dkr(1);
    f3ddkr_run_dl_impl(gsp, fifo, addr, textured, z_buffered);
}

static void f3ddkr_run_dl_impl(GSPState *gsp, RdpFifo *fifo, unsigned int addr,
                               int textured, int z_buffered)
{
    int guard = 0;
    unsigned int pc = addr;
    int running = 1;
    const unsigned char *r = s_rdram;


    if (textured)   s_textured  = textured;
    if (z_buffered) s_zbuffered = z_buffered;
    if (r == 0)
        return;
    if (s_dl_depth >= DKR_DL_MAX_DEPTH)
        return;
    s_dl_depth++;

    while (running && guard++ < 100000)
    {
        unsigned int w0, w1;
        int cmd;
        if (!in_range(pc, 8u)) break;
        w0 = rd32(r, pc);
        w1 = rd32(r, pc + 4);
        cmd = (int)((w0 >> 24) & 0xff);
        hle_timing_graphics_family("gfx_f3ddkr");
        hle_timing_add(HLE_WORK_COMMAND, 1);
        pc += 8;

        switch (cmd)
        {
        case DKR_G_MTX:
        {
            /* DKR selects bits 22..23. JFG selects bits 16..18 and,
             * when bit 23 is set, multiplies by source bits 19..21. */
            unsigned int len = w0 & 0xffffu;
            unsigned int ma  = seg_phys(s_dma_mtx + w1) & ~7u;
            if (len == 64u && in_range(ma, 64u))
            {
                int index = s_jfg ? (int)((w0 >> 16) & 7u)
                                  : (int)((w0 >> 22) & 3u);
                int multiply = 0;
                if (s_jfg)
                    multiply = (int)((w0 >> 23) & 0x01u);
                gsp_matrix_dkr(gsp, r, ma, index, multiply,
                               s_jfg ? (int)((w0 >> 19) & 7u) : 0);
            }
            break;
        }

        case DKR_G_VTX:
        {
            /* The DMA address is aligned down to eight bytes; parameter
             * bits 1..2 select the in-DMEM halfword-aligned start. */
            int append = (int)(w0 & F3DDKR_VTX_APPEND);
            int n  = (int)((w0 >> 19) & 0x1fu) + (s_jfg ? 0 : 1);
            int dst;
            unsigned int va = (seg_phys(s_dma_vtx + w1) & ~7u)
                            + ((w0 >> 16) & 6u);
            if (append)
            {
                /* The RSP retains the preceding non-append count as the
                 * append base. Repeated appends overwrite that same span. */
            }
            else
                s_vtx_top = 0;
            dst = s_jfg ? (int)((w0 >> 9) & 0x7fu) : s_vtx_top;
            if (n > 0 && dst >= 0 && dst + n <= GSP_MAX_VERTICES
                && in_range(va, (unsigned int)n * 10u))
            {
                gsp_vertex_dkr(gsp, r, va, n, dst, s_billboard);
                if (!append) s_vtx_top = n;
            }
            break;
        }

        case DKR_G_TRIN:
        {
            /* gSPPolygon: a batch of DKR triangles. Verified layout
             * (GLideN64 F3DDKR_DMA_Tri + DKRTriangle struct):
             *   count    = ((w0 >> 20) & 0x0f) + 1
             *   texture  =  (w0 >> 16) & 0x0f
             *   w1       -> array of 16-byte DKRTriangle entries:
             *     +0 u8 flag  +1 u8 v0  +2 u8 v1  +3 u8 v2
             *     +4 s16 s0 +6 s16 t0  +8 s16 s1 +10 s16 t1
             *     +12 s16 s2 +14 s16 t2
             * The S/T are per-vertex S10.5 texel coords carried on the
             * triangle (not the vertex), so patch them into the three cached
             * vertices before emitting. flag bit 0x40 selects cull mode but a
             * triangle is still drawn; we emit unconditionally (the rasterizer
             * honours the geometry-mode cull). */
            int num_tris = (int)((w0 >> 20) & 0x0fu) + 1;
            int tex_en   = (int)((w0 >> 16) & 0x0fu);
            unsigned int ta = seg_phys(w1) & ~7u;
            int t;
            s_textured = tex_en & 1;
            if (in_range(ta, (unsigned int)num_tris * 16u))
            {
                for (t = 0; t < num_tris; t++)
                {
                    unsigned int e = ta + (unsigned int)t * 16u;
                    unsigned int flag = r[(e + 0) ^ 3];
                    int v0 = (int)r[(e + 1) ^ 3];
                    int v1 = (int)r[(e + 2) ^ 3];
                    int v2 = (int)r[(e + 3) ^ 3];
                    int32_t cmdw[GSP_TRI_CMD_WORDS];
                    int nc;
                    /* Reject triangles whose indices fall outside the vertex
                     * buffer or past the highest vertex actually loaded this
                     * batch. Without this a malformed or partially-restored
                     * (savestate) display list indexes stale/garbage vertices
                     * and gsp_triangle emits a degenerate RDP triangle that
                     * can wedge the rasterizer. */
                    if (v0 < 0 || v0 >= GSP_MAX_VERTICES
                        || v1 < 0 || v1 >= GSP_MAX_VERTICES
                        || v2 < 0 || v2 >= GSP_MAX_VERTICES)
                        continue;
                    /* Per-vertex texture coordinates: S is the first s16 of
                     * each pair (GLideN64 DKRTriangle {s0,t0,...}), not the
                     * second, and DKR's microcode uses the raw S10.5 values
                     * with no G_TEXTURE scale (its two G_TEXTURE commands
                     * carry scale 0). gsp_set_vertex_st stores the raw
                     * values directly on the DKR path. */
                    gsp_set_vertex_st(gsp, v0,
                        (int)(short)((r[(e + 4) ^ 3] << 8) | r[(e + 5) ^ 3]),
                        (int)(short)((r[(e + 6) ^ 3] << 8) | r[(e + 7) ^ 3]));
                    gsp_set_vertex_st(gsp, v1,
                        (int)(short)((r[(e + 8) ^ 3] << 8) | r[(e + 9) ^ 3]),
                        (int)(short)((r[(e + 10) ^ 3] << 8) | r[(e + 11) ^ 3]));
                    gsp_set_vertex_st(gsp, v2,
                        (int)(short)((r[(e + 12) ^ 3] << 8) | r[(e + 13) ^ 3]),
                        (int)(short)((r[(e + 14) ^ 3] << 8) | r[(e + 15) ^ 3]));
                    /* Flag 0x40 disables culling. Otherwise geometry bit
                     * 0x1000 selects front versus back, as in the native
                     * writer's 0x1a80..0x1b60 cull ladder. */
                    gsp_set_geometry_mode(gsp, (flag & 0x40u)
                        ? (f3d_xlate_geom(s_geom) & ~0x600u)
                        : ((f3d_xlate_geom(s_geom) & ~0x600u)
                            | ((s_geom & 0x1000u) ? 0x200u : 0x400u)));
                    nc = gsp_triangle(gsp, cmdw, v0, v1, v2,
                                      s_textured, s_zbuffered);
                    if (nc > 0) rdp_fifo_append(fifo, cmdw, nc);
                }
                gsp_set_geometry_mode(gsp, f3d_xlate_geom(s_geom));
            }
            break;
        }

        case DKR_G_TEXTURE:
            /* DKR IMEM 0x1230: only the low command bit is copied to
             * geometry bit 1. w1 contains no texture scales in this build. */
            s_textured = (int)(w0 & 1u);
            gsp->tex_tile = (int)((w0 >> 8) & 7u);
            gsp->tex_level = (int)((w0 >> 11) & 7u);
            break;

        case 0xb4: /* Rare's perspective-normalization halfword */
            gsp_set_persp_norm(gsp, w1 & 0xffffu);
            break;

        case 0xb3: /* first RDP payload half */
            s_rdp_half = w1;
            break;

        case 0xb1:
        case 0xb2:
        {
            int32_t pair[2];
            pair[0] = (int32_t)s_rdp_half;
            pair[1] = (int32_t)w1;
            rdp_fifo_append(fifo, pair, 2);
            break;
        }

        case DKR_G_DL:
        {
            unsigned int da = seg_phys(w1);
            if (in_range(da, 8u))
            {
                /* The parameter byte selects call (zero) or branch
                 * (nonzero). A branch must not execute the old list's tail. */
                if ((w0 & 0x00ff0000u) != 0u)
                    pc = da;
                else
                    f3ddkr_run_dl_impl(gsp, fifo, da,
                                       s_textured, s_zbuffered);
            }
            break;
        }

        case DKR_G_DMADL:
        {
            /* Retail DKR IMEM 0x176c..0x179c copies `num` 64-bit words
             * directly to the RDP. These are packets, not GBI instructions:
             * rectangle tails and segmented-looking image addresses remain
             * opaque, and SET_OTHER_MODES does not change RSP state. */
            unsigned int num = (w0 >> 16) & 0xffu;
            unsigned int da  = seg_phys(w1) & ~7u;
            if (num > 0u && in_range(da, num * 8u))
            {
                unsigned int k;
                int32_t words[510];
                for (k = 0; k < num; k++)
                {
                    words[k*2u] = (int32_t)rd32(r, da + k * 8u);
                    words[k*2u+1u] = (int32_t)rd32(r, da + k * 8u + 4u);
                }
                /* Keep the DMA block atomic at FIFO rollover: a rectangle
                 * or triangle packet may span multiple 64-bit words. */
                rdp_fifo_append(fifo, words, (int)(num*2u));
            }
            break;
        }

        case DKR_G_MOVEWORD:
        {
            /* Index is the low byte of w0 (matches GLideN64 F3DDKR_MoveWord /
             * the gImmp21 G_MOVEWORD packing), not bits 16-23. */
            unsigned int index = w0 & 0xffu;
            if (index == 0x00u) /* G_MW_MATRIX: two packed halves in bank zero */
            {
                unsigned int off=(w0>>8)&0xffffu;
                if (off<64u && (off&3u)==0u) {
                    unsigned int element=(off&31u)/2u, k;
                    for(k=0;k<2;k++) {
                        unsigned int row=(element+k)/4u, col=(element+k)&3u;
                        unsigned int old=(unsigned int)gsp->modelview[0][row][col];
                        unsigned int half=k ? (w1&0xffffu) : (w1>>16);
                        gsp->modelview[0][row][col]=(int32_t)(off<32u
                            ? ((old&0xffffu)|(half<<16))
                            : ((old&0xffff0000u)|half));
                    }
                    gsp->combined_valid=0;
                }
            }
            else if (index == DKR_MW_BILLBOARD)
                s_billboard = (w1 != 0u) ? 1 : 0;
            else if (index == DKR_MW_MVPMATRIX)
            {
                s_mtx_slot = (int)((w1 >> 6) & (s_jfg ? 7u : 3u));
                gsp_select_matrix_dkr(gsp, s_mtx_slot);
            }
            else if (index == 0x04u)
            {
                unsigned int off=(w0>>8)&0xffffu;
                if (off<=44u && (off&3u)==0u) {
                    unsigned int plane=5u-off/8u, lane=(off&7u)/2u;
                    gsp->dkr_clip_rows[plane][lane]=(short)(w1>>16);
                    gsp->dkr_clip_rows[plane][lane+1u]=(short)w1;
                }
            }
            else if (index == 0x0cu) /* G_MW_POINTS */
            {
                unsigned int off=(w0>>8)&0xffffu;
                gsp_modify_vertex(gsp,(int)(off/40u),off%40u,w1);
            }
            else if (index == DKR_MW_FOG)
            {
                /* Signed multiplier/offset for the projected-z fog lane. */
                gsp_set_fog(gsp, (int)(short)(w1 >> 16),
                                 (int)(short)(w1 & 0xffffu));
            }
            else if (index == DKR_MW_SEGMENT)
            {
                /* gSPSegment(seg,base): offset field = seg*4, so the segment
                 * number is bits 10-13 of w0. Store the FULL base word: the
                 * microcode's table keeps it whole and its pass-through
                 * address resolution is base + (w1 & 0xffffff), so high bits
                 * of the base survive into the emitted RDP words. Mickey's
                 * Speedway USA runs gSPSegment(0, 0x80000000) and the cxd4
                 * LLE stream carries SET_*_IMAGE addresses with bit 31 set;
                 * masking the base here dropped it. DMA reads go through
                 * seg_phys(), which still masks to a physical address. */
                unsigned int seg = (w0 >> 10) & 0x0fu;
                s_seg[seg] = w1;
            }
            break;
        }

        case DKR_G_MOVEMEM:
        {
            /* Stock F3D G_MOVEMEM; DKR uses it for the viewport (index
             * 0x80). The boot frame switches between a 160/120-scale
             * viewport and one with vscale.y = 160: with the command
             * unhandled the frontend default rendered every batch under
             * the second viewport with the wrong Y scale. */
            unsigned int mvidx = (w0 >> 16) & 0xffu;
            unsigned int ma    = seg_phys(w1);
            if (mvidx == 0x80u && in_range(ma, 16u))
                gsp_set_viewport(gsp, r, ma);
            break;
        }

        case DKR_G_SETGEOMETRYMODE:
            /* DKR keeps the GBI 1 geometry-mode bit layout; the shared
             * frontend interprets the F3DEX2 layout, where GBI 1's
             * G_SHADING_SMOOTH (0x200) lands on G_CULL_FRONT. Keep a raw
             * shadow and translate on write, exactly like the F3D walker,
             * or every smooth-shaded batch is front-culled. */
            s_geom |= w1;
            gsp_set_geometry_mode(gsp, f3d_xlate_geom(s_geom));
            break;

        case DKR_G_CLEARGEOMETRYMODE:
            s_geom &= ~w1;
            gsp_set_geometry_mode(gsp, f3d_xlate_geom(s_geom));
            break;

        case DKR_G_SETOTHERMODE_H:
        case DKR_G_SETOTHERMODE_L:
        {
            /* F3D partial other-modes write (DKR binds these to the stock
             * F3D_SetOtherMode_H/L).  shift/length are direct (not the
             * F3DEX2 inverted form) and w1 already holds the positioned bits.
             * DKR uses these to flip the cycle type to FILL for its color /
             * Z-buffer clears -- if they are dropped the Z clear never runs in
             * FILL mode, the Z buffer is left at "near", and every 3D pixel
             * fails the depth test (the whole 3D scene renders black behind
             * the 2D HUD).  Maintain the running H/L mode words and re-emit a
             * full RDP SET_OTHER_MODES so angrylion picks up the change. */
            unsigned int shift = (w0 >> 8) & 0xffu;
            unsigned int length = w0 & 0xffu;
            unsigned int mask;
            int32_t two[2];
            /* SLLV masks both variable shift counts to five bits. The
             * payload is ORed whole after clearing the selected field. */
            mask = ((1u << (length & 31u)) - 1u) << (shift & 31u);
            if (cmd == DKR_G_SETOTHERMODE_H)
                s_othermode_h = (s_othermode_h & ~mask) | w1;
            else
                s_othermode_l = (s_othermode_l & ~mask) | w1;
            s_zbuffered = (((s_othermode_l >> 4) & 1u) ||
                           ((s_othermode_l >> 5) & 1u)) ? 1 : 0;
            two[0] = (int32_t)s_othermode_h;
            two[1] = (int32_t)s_othermode_l;
            rdp_fifo_append(fifo, two, 2);
            break;
        }

        case JFG_G_DMA_OFFSETS:
            /* Added before segment resolution by the JFG DMA entry. */
            if (s_jfg)
            {
                s_dma_mtx = w0 & 0x00ffffffu;
                s_dma_vtx = w1;
                s_tex_ofs = 0u; /* BF also clears the table-enable byte. */
            }
            break;

        case JFG_G_DMA_TEX_OFFSET:
            /* JFG 0x14d4 copies 64 bytes to DMEM 0x320 and resets the
             * byte cursor. Each FD consumes one halfword unconditionally. */
            if (s_jfg)
            {
                s_tex_ofs   = seg_phys(w1) & ~7u;
                            s_tex_count = 0u;
                if (in_range(s_tex_ofs, 64u))
                {
                    unsigned int k;
                    for (k=0;k<32;k++)
                        s_tex_offsets[k] = (unsigned short)
                            ((r[(s_tex_ofs+k*2u)^3u]<<8)
                              | r[(s_tex_ofs+k*2u+1u)^3u]);
                }
                else s_tex_ofs = 0u;
            }
            break;

        case DKR_G_ENDDL:
            running = 0;
            break;

        case 0xe4: /* G_TEXRECT */
        case 0xe5: /* G_TEXRECTFLIP */
        {
            /* Coalesce the B3/B2 form into one atomic FIFO packet.
             * Contiguous rectangle packets use raw DMA instead. */
            unsigned int n0 = rd32(r, pc);
            unsigned int n1 = rd32(r, pc + 4);
            int32_t tr[4];
            unsigned int tail_cmd = (n0 >> 24) & 0xffu;
            tr[0] = (int32_t)w0;
            tr[1] = (int32_t)w1;
            if (tail_cmd == 0xb3u && in_range(pc, 16u)
                && ((rd32(r, pc + 8) >> 24) == 0xb2u
                    || (rd32(r, pc + 8) >> 24) == 0xb1u))
            {
                /* RDPHALF form: two immediate commands carry the coords. */
                unsigned int m0, m1;
                tr[2] = (int32_t)n1;          /* first RDPHALF payload  */
                m0 = rd32(r, pc + 8);
                m1 = rd32(r, pc + 12);
                (void)m0;
                tr[3] = (int32_t)m1;          /* second RDPHALF payload */
                s_rdp_half = n1;
                pc += 16;
            }
            else
            {
                /* Contiguous packets are sent through raw DMA (0x07).
                 * Here the next word is still a GBI instruction. */
                rdp_fifo_append(fifo, tr, 2);
                break;
            }
            rdp_fifo_append(fifo, tr, 4);
            break;
        }

        default:
            /* RDP pass-through opcodes (0xC8..0xFF) and any DKR command not
             * yet decoded. The shared RDP commands (SET_*, TEXRECT, sync) are
             * forwarded verbatim so state/texture setup reaches angrylion. */
            if (cmd >= 0xc0 && cmd <= 0xff)
            {
                int rdp_id = cmd & 0x3f;
                if (rdp_id <= 0x3f)
                {
                    int32_t two[2];
                    two[0] = (int32_t)w0;
                    /* Resolve all image pointers, including segment zero,
                     * preserving carry and the full segment-base word. */
                    if (rdp_id == 0x3f || rdp_id == 0x3e || rdp_id == 0x3d)
                        two[1] = (int32_t)seg_rsp(w1);
                    else
                        two[1] = (int32_t)w1;
                    /* The JFG image handler consumes a DMA'd halfword on
                     * every SET_TEXTURE_IMAGE, before segment resolution.
                     * LOAD_BLOCK and the image format do not alter it. */
                    if (rdp_id == 0x3d && s_tex_ofs != 0u)
                    {
                        if (s_tex_count < 32u)
                            two[1] = (int32_t)seg_rsp(w1 + s_tex_offsets[s_tex_count]);
                        s_tex_count = (s_tex_count + 1u) & 127u;
                    }
                    rdp_fifo_append(fifo, two, 2);
                }
            }
            break;
        }
    }

    s_dl_depth--;
}
