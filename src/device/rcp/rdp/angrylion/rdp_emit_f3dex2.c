#include "../../rsp/hle/hle_timing.h"
/* rdp_emit_f3dex2.c -- F3DEX2 display-list dispatcher for the angrylion HLE
 * path. Reads F3DEX2 commands from RDRAM, routes geometry to the C89
 * frontend (rdp_emit_frontend), and appends the resulting RDP triangle
 * commands to a FIFO the angrylion rasterizer consumes.
 *
 * Opcode/argument layout follows the documented F3DEX2 microcode (the same
 * decode GLideN64 uses); no external plugin code is linked.
 *
 * Build check:
 *   gcc -std=c89 -pedantic -Wall -Wdeclaration-after-statement -Werror
 */

#include "rdp_emit_f3dex2.h"
#include "rdp_emit_s2dex.h"
#include "rdp_emit_f3d.h"
#include "rdp_emit_rsp.h"
#include <string.h>

/* F3DEX2 command bytes (high byte of w0) */
#define F3DEX2_VTX        0x01
#define F3DEX2_TRI1       0x05
#define F3DEX2_TRI2       0x06
#define F3DEX2_QUAD       0x07
#define F3DEX2_TEXTURE    0xD7
#define F3DEX2_POPMTX     0xD8
#define F3DEX2_GEOMETRY   0xD9
#define F3DEX2_MTX        0xDA
#define F3DEX2_DL         0xDE
#define F3DEX2_ENDDL      0xDF
#define F3DEX2_SETOTHERMODE_L 0xE2
#define F3DEX2_SETOTHERMODE_H 0xE3
#define F3DEX2_RDPSETOTHERMODE 0xEF
#define F3DEX2_TEXRECT        0xE4
#define F3DEX2_TEXRECTFLIP    0xE5
#define F3DEX2_RDPHALF_1      0xF1
#define F3DEX2_RDPHALF_2      0xE1

/* matrix param bits */
#define MTX_PROJECTION    0x04
#define MTX_LOAD          0x02
#define MTX_PUSH          0x01

#define DL_NOPUSH         0x01

#define F3DEX2_MOVEWORD   0xDB
#define G_MW_SEGMENT      0x06
#define G_MW_NUMLIGHT     0x02
#define G_MW_LIGHTCOL     0x0a
#define G_MW_FOG          0x08
#define G_MW_PERSPNORM    0x0e

#define F3DEX2_MOVEMEM    0xDC
#define G_MV_VIEWPORT     0x08
#define G_MV_LIGHT        0x0a
#define G_MV_NORMALES     0x0e

/* RDRAM/DMEM 32-bit words are stored host-native in this core (the RSP's
 * u32() accessor reads them directly with no byteswap), so read native. */
static unsigned int s_rdram_size;   /* set per frame; 0 => assume 8 MiB */

static unsigned int rd_u32_be(const unsigned char *r, unsigned int a)
{
    unsigned int limit = s_rdram_size ? s_rdram_size : (8u * 1024u * 1024u);
    if (a + 4u > limit)
        return 0u;          /* out of range: treat as G_SPNOOP */
    return *(const unsigned int *)(r + a);
}

/* F3DEX2 segment table: addresses are (segment << 24) | offset, resolved as
 * seg_table[segment] + offset. Set by G_MOVEWORD/G_MW_SEGMENT. Segment 0 and
 * the KSEG0 0x80-based pointers resolve to a 0 base, i.e. physical == offset. */
static unsigned int s_seg_table[16];

/* G_RDPHALF_1 (0xE1) latch: F3DZEX2's G_BRANCH_W (0x04) branches to the DL
 * address staged here by the preceding RDPHALF_1. */
static unsigned int s_half1;

/* Which microcode class the walker is currently interpreting. Kirby 64
 * switches between F3DEX2 (3D), L3DEX2 (lines) and S2DEX2 (sprites)
 * mid-display-list with gSPLoadUcodeL; the low opcode space means
 * entirely different things per class. Probe FIFO 2.05's complete program
 * before the older word-1 signatures: its c81f201b instruction collides
 * with L3DEX2_2.04H. c81f2018 identifies S2DEX2_2.04; other builds use the
 * F3DEX2 family. Arithmetic feature bits share the existing serialized class
 * word and preserve the base class's low-opcode dispatch. */
#define UCODE_F3DEX2 0
#define UCODE_L3DEX2 1
#define UCODE_S2DEX2 2
#define UCODE_S2DEX1 3
#define UCODE_F3DEX205 4
#define UCODE_TEXRECIP 0x100
#define UCODE_LIT_ALPHA 0x200
#define UCODE_CBFD_NORMAL 0x400
#define UCODE_DMA_INLINE 0x800
#define UCODE_OM_COALESCE 0x1000
#define UCODE_REJRECIP 0x2000
static int s_ucode_class;

int f3dex205_ucode_match(const unsigned char *r, unsigned int size,
                        unsigned int text)
{
    unsigned int i, hash = 2166136261u;
    if (r == 0 || text == 0 || text > size || 0xf80u > size - text)
        return 0;
    /* The 2.05 F3DEX build shares its second instruction with L3DEX2
     * 2.04H. Fingerprint the resident program, not that one-word alias or
     * the shared data segment (which also contains a neighbouring Rej ID).
     * Canonical-byte FNV-1a; captured text SHA256 ff3fce27...0a3a3c3. */
    if (rd_u32_be(r, text + 4u) != 0xc81f201bu)
        return 0;
    for (i = 0; i < 0xf80u; ++i)
        hash = (hash ^ r[(text + i) ^ 3u]) * 16777619u;
    return hash == 0x8ee883e2u;
}

int f3dex2_native_texrecip(void)
{
    if(s_ucode_class&UCODE_REJRECIP)return 2;
    return (s_ucode_class & 0xff) == UCODE_F3DEX205 || (s_ucode_class & UCODE_TEXRECIP) != 0;
}

static int probe_texture_reciprocal(const unsigned char *r, unsigned int text)
{
    unsigned int offset, i;
    if (text > s_rdram_size || 0xf80u > s_rdram_size - text) return 0;
    for (offset = 0; offset + 44u <= 0xf80u; offset += 4u)
    {
        if(rd_u32_be(r,text+offset)==0x4b184772u &&
           rd_u32_be(r,text+offset+4u)==0x4b384131u &&
           rd_u32_be(r,text+offset+8u)==0x4a790150u &&
           rd_u32_be(r,text+offset+12u)==0x4b004632u &&
           rd_u32_be(r,text+offset+20u)==0x4b04cf45u &&
           rd_u32_be(r,text+offset+24u)==0x4b042f4cu &&
           rd_u32_be(r,text+offset+32u)==0x4b18294eu &&
           rd_u32_be(r,text+offset+40u)==0x4b18ce4fu)return 2;
        unsigned int hash = 2166136261u;
        if (rd_u32_be(r, text + offset) != 0x4b1b4772u ||
            rd_u32_be(r, text + offset + 4u) != 0x4b3b42b1u) continue;
        /* Complete 11-instruction DIV/MAC normalizer, including operand
         * registers. Shared by the captured BAR, DK64 and FIFO 2.05 builds.
         * No ROM identity or version-name heuristic selects this math. */
        for (i = 0; i < 44u; ++i)
            hash = (hash ^ r[(text + offset + i) ^ 3u]) * 16777619u;
        if (hash == 0x71592df1u) return 1;
    }
    return 0;
}

static int probe_lit_alpha(const unsigned char *r, unsigned int text, unsigned int data)
{
    unsigned int at;
    if (text > s_rdram_size || 0xf80u > s_rdram_size - text
        || data > s_rdram_size || 0x1000u > s_rdram_size - data) return 0;
    {
        unsigned int load = rd_u32_be(r, text + 4u);
        unsigned int row = (load & 0x7fu) * 16u;
        if ((load & 0xffffff80u) != 0xc81f2000u || row + 16u > 0x1000u
            || (rd_u32_be(r, data + row + 12u) & 0xffffu) != 0x7fffu) return 0;
    }
    /* Rejection builds keep this lighting loop resident instead of
     * installing it through the standard overlay descriptor. */
    for(at=28u;at+16u<=0xf80u;at+=4u) {
        const unsigned int *p=(const unsigned int *)(r+text+at);
        if(p[0]==0x4bffef40u && p[1]==0x4a952748u &&
           (p[2]&0xffff0000u)==0x156d0000u && p[3]==0x4a9c1f48u &&
           rd_u32_be(r,text+at-28u)==0x48802300u &&
           rd_u32_be(r,text+at-20u)==0x48802700u)
            return UCODE_LIT_ALPHA;
    }
    /* Locate the lighting overlay through the native boot relocation,
     * rather than scanning unrelated code adjacent to the program. */
    for (at = 0; at + 40u <= 0x180u; at += 4u) {
        const unsigned int *w = (const unsigned int *)(r + text + at);
        unsigned int desc, source, bytes, k;
        if ((w[0] & 0xffff0000u) != 0x8c040000u
            || (w[1] & 0xffff0000u) != 0x8c050000u
            || w[6] != 0x00812020u || w[7] != 0x00a12820u) continue;
        desc = w[0] & 0xffffu;
        if ((desc & 3u) || desc > 0xff8u) continue;
        source = rd_u32_be(r, data + desc);
        bytes = (rd_u32_be(r, data + desc + 4u) >> 16) + 1u;
        if ((source & 7u) || bytes > 0x1000u || source > s_rdram_size - text
            || bytes > s_rdram_size - text - source) continue;
        source += text;
        for (k = 28; k + 16u <= bytes; k += 4u) {
            const unsigned int *p = (const unsigned int *)(r + source + k);
            /* RGB MAC lanes have zeroed alpha; VMULF also scales the
             * merged source alpha by v31[15] before the packed store. */
            if (p[0] == 0x4bffef40u && p[1] == 0x4a952748u
                && (p[2] & 0xffff0000u) == 0x156d0000u && p[3] == 0x4a9c1f48u
                && rd_u32_be(r, source + k - 28u) == 0x48802300u
                && rd_u32_be(r, source + k - 20u) == 0x48802700u)
                return UCODE_LIT_ALPHA;
        }
    }
    return 0;
}

static int probe_ucode_class(const unsigned char *r, unsigned int text, unsigned int data)
{
    unsigned int w1, attributes;
    if (r == 0 || text == 0 || text > s_rdram_size || 8u > s_rdram_size - text)
        return UCODE_F3DEX2;
    w1 = rd_u32_be(r, text + 4);
    if (f3dex205_ucode_match(r, s_rdram_size, text))
        return UCODE_F3DEX205;
    {
        int reciprocal=probe_texture_reciprocal(r,text);
        attributes=(reciprocal==2?UCODE_REJRECIP:reciprocal?UCODE_TEXRECIP:0)
                  |probe_lit_alpha(r,text,data);
    }
    {
        unsigned int at;
        for(at=0;at+60u<=0x1800u && at+60u<=s_rdram_size-text;at+=4u)
            if(rd_u32_be(r,text+at)==0x8d610078u &&
               (rd_u32_be(r,text+at+36u)&0xffff0000u)==0x83790000u &&
               rd_u32_be(r,text+at+44u)==0x13600004u &&
               rd_u32_be(r,text+at+52u)==0x232c0047u &&
               rd_u32_be(r,text+at+56u)==0x318c00feu) {
                attributes|=UCODE_OM_COALESCE;break;
            }
    }
    if (0x198u <= s_rdram_size - text &&
        rd_u32_be(r,text+0x168u)==0x37330000u &&
        rd_u32_be(r,text+0x16cu)==0x00139a00u &&
        rd_u32_be(r,text+0x170u)==0x00139942u &&
        rd_u32_be(r,text+0x174u)==0x0013e022u &&
        rd_u32_be(r,text+0x178u)==0x24140a50u &&
        rd_u32_be(r,text+0x194u)==0)
        attributes |= UCODE_DMA_INLINE;
    if (data<=s_rdram_size && 0x6b0u<=s_rdram_size-data &&
        rd_u32_be(r,data+0x6acu)==0x038712d8u) {
        unsigned int offset=rd_u32_be(r,data+0x6a8u);
        if(offset<=s_rdram_size-text && 0x11cu<=s_rdram_size-text-offset &&
            rd_u32_be(r,text+offset)==0x900b023cu &&
            rd_u32_be(r,text+offset+0x100u)==0xe808201bu &&
            rd_u32_be(r,text+offset+0x10cu)==0x81c20007u &&
            rd_u32_be(r,text+offset+0x118u)==0x848a0512u)
            attributes|=UCODE_CBFD_NORMAL;
    }
    if (w1 == 0xc81f201bu) {
        unsigned int at;
        /* The constant-row load also occurs in triangle microcodes.
         * Identify the line writer's paired Y loads and register clears
         * before selecting its opcode 08 and unlit vertex path. This
         * must also work after a mid-list LOAD_UCODE. */
        for(at=0;at+16u<=0xf80u && at+16u<=s_rdram_size-text;at+=4u)
            if(rd_u32_be(r,text+at)==0x8447001au &&
               rd_u32_be(r,text+at+4u)==0x4bff00adu &&
               rd_u32_be(r,text+at+8u)==0x8468001au &&
               rd_u32_be(r,text+at+12u)==0x4bff00edu)
                return UCODE_L3DEX2 | attributes;
    }
    if (w1 == 0xc81f2018u)
        return UCODE_S2DEX2;
    return UCODE_F3DEX2 | attributes;
}

/* Conker's Bad Fur Day runs a custom F3DEX2 build, "F3DEXBG.NoN". It packs
 * the bulk of its geometry into a block of G_TRI4 opcodes (0x10..0x1f), each
 * drawing four triangles from twelve 5-bit vertex indices -- the stock
 * F3DEX2 walker leaves that whole opcode range unhandled, so most of every
 * scene fails to render. Detected by the SGI data-segment name string, the
 * same way f3d_ucode_family classifies the GBI 1 builds. */
static int s_variant_cbfd;
static int s_variant_acclaim;

static int cbfd_ucode_match(const unsigned char *rdram,
                            unsigned int ud, unsigned int uds)
{
    static const char sig[] = "F3DEXBG";
    unsigned int siglen = sizeof(sig) - 1u;
    unsigned int hi, b;
    if (rdram == 0 || ud == 0)
        return 0;
    if (uds == 0u || uds > 0x4000u)
        uds = 0x1000u;
    hi = ud + uds;
    if (s_rdram_size && hi > s_rdram_size)
        hi = s_rdram_size;
    if (hi < ud + siglen)
        return 0;
    for (b = ud; b + siglen <= hi; b++)
    {
        unsigned int k;
        for (k = 0; k < siglen; k++)
            if (rdram[(b + k) ^ 3] != (unsigned char)sig[k])
                break;
        if (k == siglen)
            return 1;
    }
    return 0;
}

/* RDP render state is global on the real RSP (one set of mode registers),
 * not per-display-list. Track it module-wide so a mode set in one DL is seen
 * by sibling DLs drawn afterward; reset per frame alongside the segments. */
static int s_textured;
static int s_zbuffered;

/* The RSP maintains the RDP "other modes" as two 32-bit words (high and low).
 * F3DEX2 updates them either wholesale (G_RDPSETOTHERMODE / 0xEF) or, far more
 * commonly, as partial bitfield writes (G_SETOTHERMODE_H/L / 0xE3/0xE2) that
 * read-modify-write a sub-range and then have the RSP emit a full RDP
 * SET_OTHER_MODES (0x2f). The cycle type (1- vs 2-cycle) lives in the high word
 * at bits 20-21 and on OoT is set through G_SETOTHERMODE_H, so the partial
 * writes MUST be merged and re-emitted as a full 0x2f or every such triangle
 * renders with a stale cycle type. We mirror that register here. The high word
 * keeps 0x2f in its command byte (bits 29-24) so the merged word is a valid
 * SET_OTHER_MODES when forwarded. */
static unsigned int s_othermode_h;
static unsigned int s_othermode_l;

/* RDRAM size and recursion depth bound the display-list walk so a malformed
 * or mis-segmented list cannot read past RDRAM or recurse without limit (both
 * would hard-hang the core). s_rdram_size (declared above) is set per frame by
 * the activation; 0 means "unknown", reads then assume the default 8 MiB. */


#define DL_MAX_DEPTH 32

/* Explicit display-list call stack (replaces the old C recursion for G_DL
 * so a streaming task can be suspended at any nesting depth and resumed).
 * Shared by every walk; top-level walks start with an empty stack. */
static unsigned int s_dl_stack[DL_MAX_DEPTH];
static int s_dl_sp;

/* Streaming mode: some microcodes (Gauntlet Legends' F3DEX2 2.0xH
 * derivative) run one persistent task per frame whose display list the CPU
 * extends while the RSP walks it. The list's live tail is a G_DL branch to
 * its own address: the RSP spins there, re-reading the command from RDRAM,
 * until the CPU patches it to point at the next chunk (or the terminating
 * G_ENDDL). In streaming mode the walker returns to the caller at that
 * self-branch instead of spinning, so the host CPU can run and extend the
 * list, and resumes from the same command on the next slice. */
static int s_streaming;
static int s_stream_resume;
static int s_stream_active;
static int s_stream_stalled;
static unsigned int s_stream_pc;

unsigned int f3dex2_restore_yield(GSPState *s,const unsigned char *dm)
{
    unsigned int i,k,texture=rd_u32_be(dm,0x1e4u),scale=rd_u32_be(dm,0x1e8u);
    unsigned int stride=(unsigned int)(uint16_t)(
        *(const uint16_t *)(dm+(0x382u^2u))-*(const uint16_t *)(dm+(0x380u^2u)));
    for(i=0;i<16;i++)s_seg_table[i]=rd_u32_be(dm,0xf8u+i*4u);
    f3dex2_set_othermode_init(rd_u32_be(dm,0xc8u),rd_u32_be(dm,0xccu));
    gsp_set_geometry_mode(s,rd_u32_be(dm,0x1ecu));
    gsp_set_texture(s,scale>>16,scale&0xffffu,(texture>>8)&7u,(texture>>11)&7u,0,0);
    s_textured=(texture&2u)!=0;s_zbuffered=(s->geometry_mode&1u)!=0;
    gsp_seed_matrices(s,dm,0,64);
    for(i=0;i<4;i++)gsp_force_matrix_chunk(s,dm,0x80u+i*16u,i*16u);
    s->combined_valid=dm[0x1d9u^3u]!=0;
    s->mtx_stack_ptr=rd_u32_be(dm,0xf4u);
    gsp_set_viewport(s,dm,0xe0u);
    s->viewport.persp_norm=*(const uint16_t *)(dm+(0xdcu^2u));
    s->fog_m=(int16_t)(rd_u32_be(dm,0xd0u)>>16);s->fog_o=(int16_t)rd_u32_be(dm,0xd0u);
    gsp_set_num_lights(s,dm[0x1ddu^3u]/24u);
    for(i=0;i<2;i++) {
        gsp_set_lookat(s,dm,0x1f0u+i*24u,i);
        for(k=0;k<3;k++)s->lookat[i][k]=(signed char)dm[(0x200u+i*24u+k)^3u];
    }
    for(i=0;i<8;i++) {
        gsp_set_light(s,dm,0x220u+i*24u,i);
        for(k=0;k<3;k++)s->light_dir[i][k]=(signed char)dm[(0x230u+i*24u+k)^3u];
    }
    s->lights_valid=dm[0x1dcu^3u]!=0;
    s->scis_w0=rd_u32_be(dm,0xc0u);s->scis_w1=rd_u32_be(dm,0xc4u);s->scis_valid=1;
    if(stride==20u) {
        s->clip_reject=1;
        s_ucode_class|=UCODE_REJRECIP;
    }
    gsp_restore_vertex_records(s,dm,0x380u,stride==20u?20u:40u,0);
    s_dl_sp=dm[0xdeu^3u]/4u;
    if(s_dl_sp>DL_MAX_DEPTH)s_dl_sp=DL_MAX_DEPTH;
    for(i=0;i<(unsigned int)s_dl_sp;i++)s_dl_stack[i]=rd_u32_be(dm,0x138u+i*4u)&0x7fffffu;
    s_stream_resume=1;
    return rd_u32_be(dm,0xbf8u)&0x7fffffu;
}

/* end the current display-list level: pop the caller, or stop the walk */
#define DL_RETURN() \
    do { if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0); \
         if (s_dl_sp > 0) pc = s_dl_stack[--s_dl_sp]; else running = 0; } while (0)

static void seg_reset(void)
{
    int i;
    for (i = 0; i < 16; i++)
        s_seg_table[i] = 0u;
    s_textured  = 0;
    s_zbuffered = 0;

    /* command byte 0x2f in bits 29-24 so the high word is a valid
     * SET_OTHER_MODES when forwarded; mode bits start cleared (1-cycle).
     *
     * The S2DEX1 data segment ships its other-modes-high default with the
     * low six bits set (alpha_dither = G_AD_DISABLE, the reserved blend
     * mask = 0xf): bits the RSP seeds once at task boot and that S2DEX1
     * titles -- Bangai-O, Yoshi's Story -- never re-send, relying on the
     * resident value. We do not observe the task-boot seed when replaying
     * from a mid-game savestate, and unlike the real RSP we re-derive the
     * mode every frame, so without restoring those bits the OBJ sprites
     * pick up alpha_dither = G_AD_PATTERN: the dither then lifts the
     * fully-transparent palette entries (alpha 0) over the alpha-compare
     * threshold, so the sprite's clear regions paint solid instead of
     * keying out (Bangai-O's boss and Sonic portrait rendered as banded
     * boxes). Seed the S2DEX1 default; F3DEX2 / S2DEX2 keep the bare
     * 1-cycle reset, matching their own data-segment defaults. */
    s_othermode_h = (0x2fu << 24)
                  | ((s_ucode_class & 0xff) == UCODE_S2DEX1 ? 0x3fu : 0x00u);
    s_othermode_l = 0u;
}

/* Seed the other-modes mirror with the microcode's data-segment default
 * pair (DMEM 0xc8, loaded from ucode_data + 0xc8 at task boot). Games
 * that never send a wholesale G_RDPSETOTHERMODE -- Super Smash Bros.
 * updates only bitfields via 0xE2/0xE3 -- inherit these defaults on the
 * real RSP, so partial writes must merge into them, not into zero. The
 * stored high word keeps the microcode's 0xEF command byte, exactly as
 * the RSP mirror does (the RDP decodes only the low 6 opcode bits). */
void f3dex2_set_othermode_init(unsigned int h, unsigned int l)
{
    s_othermode_h = h;
    s_othermode_l = l;
    s_zbuffered = (((s_othermode_l >> 4) & 1u) ||
                   ((s_othermode_l >> 5) & 1u)) ? 1 : 0;
}

unsigned int f3dex2_othermode_high(void) { return s_othermode_h; }

void f3dex2_seg_reset(void)
{
    s2dex_reset();
    seg_reset();
}

void f3dex2_set_rdram_size(unsigned int size)
{
    s_rdram_size = size;
}

/* RDRAM base for display-list, vertex and matrix reads. Previously the
 * walker borrowed the FIFO's pointer, which doubled as the FIFO backing
 * store; with the FIFO moved to host memory the two are distinct. */
static unsigned char *s_rdram_base = 0;

/* Identify the task's own microcode at task start (the task itself can be
 * S2DEX2 or L3DEX2, and any mid-list gSPLoadUcodeL switch from the previous
 * task must not leak in). */
void f3dex2_set_task_ucode(const unsigned char *rdram, unsigned int text, unsigned int data)
{
    s_ucode_class = probe_ucode_class(rdram, text, data);
    s2dex_set_version2((s_ucode_class & 0xff) == UCODE_S2DEX2);
    s_variant_cbfd = 0;
    s_variant_acclaim = 0;
}

/* Enable Conker's F3DEXBG.NoN G_TRI4 opcode block for this task, detected by
 * the data-segment name string. Cleared by f3dex2_set_task_ucode above, so it
 * must be called after it at task start. */
void f3dex2_set_variant_cbfd(const unsigned char *rdram, unsigned int ud,
                             unsigned int uds)
{
    /* c81f201b is a relocated constant-row load, shared by L3DEX2 and
     * F3DEX/F3DLX XBUS 2.06..2.08. Resolve that collision from the primary
     * ucode ID; combined SDK objects may carry a second L3DEX name. */
    if ((s_ucode_class & 0xff) == UCODE_L3DEX2 && rdram && ud < s_rdram_size)
    {
        static const char prefix[] = "RSP Gfx ucode ";
        unsigned int i, limit = uds && uds < 0x1000u ? uds : 0x1000u;
        if (limit > s_rdram_size-ud) limit=s_rdram_size-ud;
        for (i=0;i+sizeof(prefix)+3u<=limit;++i)
        {
            unsigned int j;
            for (j=0;j<sizeof(prefix)-1u;++j)
                if (rdram[(ud+i+j)^3u]!=(unsigned char)prefix[j]) break;
            if (j!=sizeof(prefix)-1u) continue;
            if (rdram[(ud+i+j)^3u]=='F' && rdram[(ud+i+j+1u)^3u]=='3' && rdram[(ud+i+j+2u)^3u]=='D')
                s_ucode_class=(s_ucode_class & ~0xff) | UCODE_F3DEX2;
            break;
        }
    }
    s_variant_cbfd = (s_ucode_class&UCODE_CBFD_NORMAL)?2:cbfd_ucode_match(rdram, ud, uds)?1:0;
}

/* Acclaim's four F3DEX2 titles (Turok 2 - Seeds of Evil, Turok 3, Armorines -
 * Project S.W.A.R.M., South Park) ship the same custom-lighting microcode: it
 * claims to be a stock "RSP Gfx ucode F3DEX.NoN fifo 2.05" build by its
 * data-segment name, so the name string cannot tell it apart, but the code
 * segment carries a custom L1-distance point-lighting routine no stock F3DEX2
 * has. The code text is byte-identical across all four games; fingerprint it by
 * a multiply-and-add checksum over the first 0x1000 bytes of the microcode text
 * in RDRAM (the same image the RSP DMAs resident before running). */
static int acclaim_ucode_match(const unsigned char *rdram, unsigned int text)
{
    unsigned int crc = 0u;
    unsigned int a;
    if (rdram == 0 || text == 0)
        return 0;
    if (s_rdram_size && text + 0x1000u > s_rdram_size)
        return 0;
    for (a = 0u; a < 0x1000u; a += 4u)
        crc = crc * 33u + rd_u32_be(rdram, text + a);
    return (crc == 0x4444ae70u) ? 1 : 0;
}

/* Enable Acclaim custom lighting for this task. Called after
 * f3dex2_set_task_ucode at task start (which clears the flag). */
void f3dex2_set_variant_acclaim(const unsigned char *rdram, unsigned int text)
{
    s_variant_acclaim = acclaim_ucode_match(rdram, text) ? 1 : 0;
}

int f3dex2_variant_acclaim(void)
{
    return s_variant_acclaim;
}

/* Adopt the caller's segment table for a mid-list microcode handoff: when a
 * vertex-family (F3D/F3DEX GBI 1) list hot-swaps to S2DEX with G_LOAD_UCODE,
 * the segment registers persist across the swap on the RSP, so the S2DEX
 * section resolves its background/object pointers through the same table the
 * F3D walker accumulated. */
void f3dex2_import_segments(const unsigned int *src)
{
    int i;
    for (i = 0; i < 16; i++)
        s_seg_table[i] = src[i];
}

/* Standalone S2DEX 1.xx (Yoshi's Story etc.) carries a build-specific text
 * version word rather than the S2DEX2 signature probe_ucode_class knows, so
 * the HLE entry detects it from the data-segment name string and forces the
 * GBI 1 sprite class here, after the text probe has run. */
void f3dex2_force_class_s2dex1(void)
{
    s_ucode_class = (s_ucode_class & ~0xff) | UCODE_S2DEX1;
    s2dex_set_version2(0);
}

/* 1 when the current task's ucode is an S2DEX (GBI 1 or GBI 2) sprite/BG
 * build, 0 for F3DEX2/L3DEX2. The HLE dispatcher uses this to scope the
 * threaded-rasterizer SET_TEXTURE_IMAGE barrier to the S2DEX BG path. */
int f3dex2_class_is_s2dex(void)
{
    return ((s_ucode_class & 0xff) == UCODE_S2DEX1 || (s_ucode_class & 0xff) == UCODE_S2DEX2)
           ? 1 : 0;
}

void f3dex2_set_rdram(unsigned char *rdram)
{
    s_rdram_base = rdram;
}

void rdp_fifo_init(RdpFifo *f, unsigned char *storage,
                   unsigned int base, unsigned int cap)
{
    f->storage = storage;
    f->base    = base;
    f->used    = 0;
    f->cap     = cap;
    f->flush   = 0;
    f->time    = 0;
    f->write   = 0;
    f->spnoop  = 0;
    f->store_indices = 0;
    f->fetch_dl = 0;
    f->dl_end = 0;
    f->drain = 0;
    f->read_staging = 0;
    f->group = 0;
    f->load_ucode = 0;
    f->dmem = 0;
}

static int s_dl_has_fullsync;

void rdp_fifo_fullsync_reset(void)
{
    s_dl_has_fullsync = 0;
}

void rdp_fifo_fullsync_note(void)
{
    s_dl_has_fullsync |= 1;
}

int rdp_fifo_fullsync_seen(void)
{
    return (s_dl_has_fullsync & 1) != 0;
}

int rdp_fifo_fullsync_pending(void)
{
    return s_dl_has_fullsync == 1;
}

void rdp_fifo_fullsync_append(RdpFifo *f, unsigned int w0, unsigned int w1)
{
    int32_t words[2];
    words[0] = (int32_t)w0;
    words[1] = (int32_t)w1;
    s_dl_has_fullsync = 3;
    rdp_fifo_append(f, words, 2);
}

void rdp_fifo_append(RdpFifo *f, const int32_t *words, int count)
{
    int i;
    unsigned int off;
    if (count > 0) hle_timing_add(HLE_WORK_RDP, (unsigned int)(count + 1) / 2u);
    if (f->used + (unsigned int)count * 4u > f->cap)
    {
        /* Heavy frames (camera swings over a full scene) can exceed the
         * FIFO; drain it mid-frame the way real hardware streams the DP
         * command buffer, then continue. Dropping commands here is not an
         * option: losing the frame-final SYNC_FULL (RDP 0x29) means the
         * DP interrupt is never raised and the game's graphics thread
         * blocks forever on the task completion. */
        if (f->flush)
            f->flush(f);
        if (f->used + (unsigned int)count * 4u > f->cap)
            return;
    }
    off = f->used;
    for (i = 0; i < count; i++)
    {
        unsigned int w = (unsigned int)words[i];
        unsigned int a = off + (unsigned int)i * 4u;
        /* RDP command words are fetched by angrylion in host-native order
         * (the overlay fetch mirrors rdram_read_idx32); store native. */
        *(int32_t *)(f->storage + a) = (int32_t)w;
    }
    if (count > 0 && f->write)
        f->write((int32_t *)(f->storage + off), (unsigned int)count);
    /* Timestamp the actual native packet after any overflow flush and
     * retained-word reconstruction. Rendering still uses one host batch. */
    if (count > 0 && f->time)
        f->time((const int32_t *)(f->storage + off), (unsigned int)count);
    f->used += (unsigned int)count * 4u;
}

/* Resolve an N64 segmented/virtual address to a physical RDRAM byte address:
 * physical = seg_table[(addr >> 24) & 0xf] + (addr & 0xffffff). For KSEG0
 * pointers (top byte 0x80/0xA0) the segment index is 0 (table base 0), so the
 * result is just the low 24 bits -- the standard virtual->physical strip. */
static unsigned int seg_addr_rsp(unsigned int w1)
{
    /* segmented_to_physical: clears the input's top byte and adds the raw
     * segment-table word, with no final mask. Games store KSEG0 pointers in
     * the table, so the sum keeps the 0x80000000 bit; the RDP and the RSP
     * DMA engine both ignore the upper address bits, but the emitted
     * SET*IMG words carry the full sum. */
    unsigned int seg = (w1 >> 24) & 0x0fu;
    return s_seg_table[seg] + (w1 & 0x00ffffffu);
}

static unsigned int seg_addr(unsigned int w1)
{
    /* physical address for this plugin's own RDRAM reads */
    return seg_addr_rsp(w1) & 0x00ffffffu;
}

/* Exported segment resolution for the S2DEX background renderers: the
 * uObjBg imagePtr is a segmented address (Zelda's pre-rendered rooms pass
 * segment-relative pointers) which the microcode resolves through the same
 * segment table before emitting it in SETTIMG. */
unsigned int gsp_seg_addr_rsp(unsigned int w1)
{
    return seg_addr_rsp(w1);
}

/* true if [a, a+bytes) lies within RDRAM; used to reject mis-segmented
 * geometry pointers before the frontend dereferences them. */
static int addr_in_range(unsigned int a, unsigned int bytes)
{
    unsigned int limit = s_rdram_size ? s_rdram_size : (8u * 1024u * 1024u);
    return (a < limit) && (bytes <= limit) && (a + bytes <= limit);
}

void f3dex2_run_dl(GSPState *gsp, RdpFifo *fifo, unsigned int addr,
                   int textured, int z_buffered)
{
    int guard = 0;
    unsigned int pc = addr;
    unsigned int inline_end = 0, inline_return = 0;
    int running = 1;
    /* render-state is shared module-wide (see s_textured/s_zbuffered) so it
     * persists across DL recursion; the textured/z_buffered arguments only
     * seed it when non-zero (callers pass the current state down). */
    if (textured)   s_textured  = textured;
    if (z_buffered) s_zbuffered = z_buffered;
    if(!rsp_graphics_sdk_one(5) || !rsp_graphics_sdk_one(7))
        gsp_sdk_vertex_memory(fifo->dmem,rd_u32_be(fifo->dmem,0x1bcu)>>16);

    /* Non-resuming walks start with an empty DL stack; a streaming resume
     * keeps the stack saved when the walk was suspended. */
    if (s_stream_resume)
        s_stream_resume = 0;
    else {
        s_dl_sp = 0;
        if ((s_ucode_class & 0xff) == UCODE_S2DEX2 || (s_ucode_class & 0xff) == UCODE_S2DEX1)
            s2dex_seed_scissor(fifo->dmem);
    }

    while (running && guard++ < 100000)
    {
        unsigned int w0, w1;
        int cmd;
        const unsigned char *r = s_rdram_base;
        /* The CPU must get a chance to request a task yield while a long
         * streaming list is still running. Completing it synchronously and
         * delaying only SP-done leaves DK64's task scheduler out of order.
         * Inline command spans keep their local return state until done. */
        if (s_streaming && !inline_end && hle_timing_slice_limit() &&
            hle_timing_cycles() >= hle_timing_slice_limit()) {
            s_stream_pc = pc;
            s_stream_stalled = 1;
            return;
        }

        if (r == 0)
            return;
        if (inline_end && pc == inline_end) {
            pc = inline_return;
            inline_end = 0;
        }

        w0 = rd_u32_be(r, pc);
        w1 = rd_u32_be(r, pc + 4);
        if (fifo->fetch_dl && !inline_end) fifo->fetch_dl(pc, w0);
        pc += 8;
        cmd = (int)((w0 >> 24) & 0xff);
#ifdef HLE_GBI_OBSERVE
        HLE_GBI_OBSERVE(pc - 8u, w0, w1, gsp);
#endif
        hle_timing_graphics_family("gfx_f3dex2");
        hle_timing_command(cmd);
        if(fifo->dmem && (!rsp_graphics_sdk_one(5)||!rsp_graphics_sdk_one(7))) {
            unsigned char *dm=fifo->dmem;
            unsigned int high=s_othermode_h|0xef000000u;
            memcpy(dm+0xc8u,&high,4);memcpy(dm+0xccu,&s_othermode_l,4);
            memcpy(dm+0x1ecu,&gsp->geometry_mode,4);
            memcpy(dm+0xf4u,&gsp->mtx_stack_ptr,4);
            dm[0xdeu^3u]=(unsigned char)(s_dl_sp*4u);
            if(cmd==F3DEX2_MOVEMEM && fifo->matrix_dma_table) {
                unsigned int at=(fifo->matrix_dma_table+(w0&0xfeu))&0xfffu;
                unsigned int dest,bytes=((w0>>16)&255u)+8u,source=seg_addr(w1)&~7u;
#ifndef MSB_FIRST
                at^=2u;
#endif
                dest=(*(const unsigned short *)(dm+at)+(w0>>5))&0xff8u;
                if(bytes<=0x1000u-dest && addr_in_range(source,bytes))memcpy(dm+dest,r+source,bytes);
            }
            if(cmd==F3DEX2_MOVEWORD) {
                unsigned int at=(0x2feu+((w0>>16)&255u))&0xfffu,dest;
#ifndef MSB_FIRST
                at^=2u;
#endif
                dest=(*(const unsigned short *)(dm+at)+w0)&0xfffu;
                {unsigned int k;for(k=0;k<4;k++)dm[((dest+k)&0xfffu)^3u]=(unsigned char)(w1>>(24-8*k));}
            }
            if(cmd==F3DEX2_TEXTURE) {
                memcpy(dm+0x1e4u,&w0,4);memcpy(dm+0x1e8u,&w1,4);
            }
        }

        /* G_RDPHALF_0 also prefixes conditional display-list calls/branches.
         * It supplies SID, the low address half and flag; SELECT_DL supplies
         * the upper address half, push bit and mask. Decode the pair before
         * the shared E4 texture-rectangle path can consume it as geometry. */
        if (((s_ucode_class & 0xff) == UCODE_S2DEX1 || (s_ucode_class & 0xff) == UCODE_S2DEX2) &&
            cmd == 0xe4 && addr_in_range(pc, 8u))
        {
            unsigned int select = rd_u32_be(r, pc);
            unsigned int select_op = select >> 24;
            unsigned int expected = (s_ucode_class & 0xff) == UCODE_S2DEX1 ? 0xb0u : 0x04u;
            if (select_op == expected)
            {
                unsigned int mask = rd_u32_be(r, pc + 4u);
                unsigned int target = ((select & 0xffffu) << 16) | (w0 & 0xffffu);
                hle_timing_command(select_op);
                pc += 8u;
                if (s2dex_select_dl((w0 >> 16) & 0xffu, w1, mask))
                {
                    if (!(select & 0x00010000u) && s_dl_sp < DL_MAX_DEPTH)
                        s_dl_stack[s_dl_sp++] = pc;
                    pc = seg_addr(target);
                    if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0);
                }
                continue;
            }
        }



        /* The low opcode space is class-specific: under S2DEX2 it is the
         * sprite/object command set, under L3DEX2 0x08 is G_LINE3D. Route
         * those before the F3DEX2 interpretation; the shared flow-control
         * and RDP-passthrough opcodes (0xD7..0xFF minus the class
         * differences) fall through to the main switch. */
        if ((s_ucode_class & 0xff) == UCODE_S2DEX1)
        {
            /* Standalone S2DEX 1.06 (GBI 1) is the F3DEX command set with the
             * 2D object commands overlaid: the low opcodes 0x01/0x03/0x04/0x06
             * keep their F3DEX meaning (G_MTX / G_MOVEMEM / G_VTX / G_DL) and
             * the sprite/object commands live at the high opcodes 0xb0-0xc4 and
             * 0xe4 (gs2dex.h !F3DEX_GBI_2 build). Nested display lists (G_DL,
             * 0x06) MUST be followed -- the object draws live inside them. */
            if (cmd == 0xaf)   /* G_LOAD_UCODE (GBI1): mid-list ucode swap */
            {
                unsigned int t = seg_addr(w1);
                if (fifo->load_ucode)
                    fifo->load_ucode(fifo, t, seg_addr(s_half1), (w0 & 0xffffu) + 1u);
                /* S2DEX 1.06 lists (Yoshi's Story title) hot-swap to an
                 * F3DEX.NoN build to draw geometry, then swap back. When the
                 * loaded microcode is an F3D vertex-family build, route the
                 * section through the F3D walker (its NoN/extended encoding
                 * matches the Doom 64 / Turok family), stopping at the next
                 * G_LOAD_UCODE so 2D object drawing resumes here. */
                if (f3d_is_ucode(s_rdram_base, s_rdram_size, t))
                {
                    f3d_set_rdram(s_rdram_base);
                    f3d_set_rdram_size(s_rdram_size);
                    f3d_set_variant(1);          /* n<<10 vtx / x2 indices */
                    f3d_set_line_variant(0);
                    f3d_set_variant_wr64(0);
                    {
                        unsigned int data=seg_addr(s_half1);
                        if(addr_in_range(data,0x120u) && (rd_u32_be(r,data+0x118u)>>24)==0xefu)
                            f3d_set_othermode_init(rd_u32_be(r,data+0x118u),rd_u32_be(r,data+0x11cu));
                    }
                    f3d_import_segments(s_seg_table);
                    f3d_set_stop_on_ucode(1);
                    f3d_run_dl(gsp, fifo, pc, s_textured, s_zbuffered);
                    f3d_set_stop_on_ucode(0);
                    if (f3d_stopped_at_ucode()) {
                        pc = f3d_get_resume_pc();
                        s_half1 = f3d_get_half1();
                    }
                    else
                    {
                        /* The inline F3DEX.NoN run hit G_ENDDL, not a swap-back:
                         * G_LOAD_UCODE does not branch, so this ends the host
                         * display list level too. */
                        DL_RETURN();
                    }
                } else {
                    /* GBI1 reloads the entire data image, including the
                     * other-mode defaults, before resuming the list. */
                    f3dex2_set_task_ucode(r,t,seg_addr(s_half1));
                    if (s2dex1_ucode_match(r,s_rdram_size,seg_addr(s_half1),(w0&0xffffu)+1u))
                        f3dex2_force_class_s2dex1();
                    s2dex_reset();
                    s2dex_seed_scissor(fifo->dmem);
                    if (fifo->dmem && (rd_u32_be(fifo->dmem,0xc8u)>>24)==0xefu)
                        f3dex2_set_othermode_init(rd_u32_be(fifo->dmem,0xc8u),
                                                 rd_u32_be(fifo->dmem,0xccu));
                }
                continue;
            }
            switch (cmd)
            {
            case 0x00:                          /* G_SPNOOP */
                break;
            case 0xb8:                          /* G_ENDDL */
                DL_RETURN();
                break;
            case 0x06:                          /* G_DL: nested display list */
            {
                unsigned int da = seg_addr(w1);
                /* w0 bit0 = G_DL_NOPUSH (branch) */
                if (w0 & 0x00010000u)
                {
                    if (addr_in_range(da, 8u)) {
                        pc = da;
                        if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0);
                    } else
                        DL_RETURN();
                }
                else if (addr_in_range(da, 8u) && s_dl_sp < DL_MAX_DEPTH)
                {
                    s_dl_stack[s_dl_sp++] = pc;
                    pc = da;
                    if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0);
                }
                break;
            }
            case 0xc1:                          /* G_OBJ_LOADTXTR */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo, gsp_seg_addr_rsp);
                break;
            case 0xc2:                          /* G_OBJ_LDTX_SPRITE (txtr+sprite) */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo, gsp_seg_addr_rsp);
                s2dex_obj_sprite(gsp, r, s_rdram_size, seg_addr(w1) + 24u, fifo);
                break;
            case 0xc3:                          /* G_OBJ_LDTX_RECT (txtr+rect) */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo, gsp_seg_addr_rsp);
                s2dex_obj_rectangle(gsp, r, s_rdram_size, seg_addr(w1) + 24u, fifo);
                break;
            case 0xb1:                          /* G_OBJ_RENDERMODE */
                s2dex_set_obj_rendermode(w1);
                break;
            case 0x03:                          /* G_OBJ_RECTANGLE */
                s2dex_obj_rectangle(gsp, r, s_rdram_size, seg_addr(w1), fifo);
                break;
            case 0x04:                          /* G_OBJ_SPRITE */
                s2dex_obj_sprite(gsp, r, s_rdram_size, seg_addr(w1), fifo);
                break;
            case 0x05:                          /* G_OBJ_MOVEMEM (object matrix) */
                s2dex_obj_movemem(r, s_rdram_size, w0, seg_addr(w1));
                break;
            case 0xb9:                          /* G_SETOTHERMODE_L (GBI 1) */
            case 0xba:                          /* G_SETOTHERMODE_H (GBI 1) */
            {
                unsigned int length = w0 & 0xffu;
                unsigned int shift  = (w0 >> 8) & 0xffu;
                unsigned int mask;
                int32_t two[2];
                if (length >= 32u)
                    mask = 0xffffffffu;
                else
                    mask = ((1u << length) - 1u) << shift;
                if (cmd == 0xba)
                    s_othermode_h = (s_othermode_h & ~mask)
                                  | (unsigned int)w1
                                  | (0x2fu << 24);
                else
                    s_othermode_l = (s_othermode_l & ~mask)
                                  | (unsigned int)w1;
                two[0] = (int32_t)(s_othermode_h | (0xefu << 24));
                two[1] = (int32_t)s_othermode_l;
                /* Native secondary-prefix lookahead emits only the final
                 * B9/BA update in a run, and cannot cross a DMA refill. */
                if (!((s_ucode_class & UCODE_OM_COALESCE) && pc < fifo->dl_end &&
                    ((rd_u32_be(r,pc) >> 24) == 0xb9u ||
                     (rd_u32_be(r,pc) >> 24) == 0xbau)))
                    rdp_fifo_append(fifo, two, 2);
                break;
            }
            case 0xbc:                          /* G_MOVEWORD (GBI 1) */
            {
                /* GBI1 packs the moveword index in the low byte of w0 and
                 * the DMEM offset in bits 8..23 (GBI2 swaps these). This
                 * case originally only resolved G_MW_SEGMENT for the S2DEX
                 * 2D path; F3D/F3DEX 3D lists send the full set, and
                 * dropping them left persp_norm at the 0xffff default --
                 * Last Legion UX sends gSPPerspNormalize(4), and without it
                 * every screen coordinate and z slope of the F3DLX terrain
                 * was off by reciprocal-precision ULPs, so the T3DUX mech
                 * legs z-failed against the mis-placed ground. */
                int index = (int)(w0 & 0xffu);
                unsigned int off = (w0 >> 8) & 0xffffu;
                if (index == G_MW_SEGMENT)
                {
                    unsigned int seg = off >> 2;
                    if (seg < 16u)
                        s_seg_table[seg] = w1;
                }
                else if (index == G_MW_PERSPNORM)
                {
                    gsp_set_persp_norm(gsp, w1 & 0xffffu);
                }
                else if (index == 0x04)
                {
                    /* G_MW_CLIP: same four-word ratio write as GBI2, with
                     * the offset field relocated. */
                    if (off == 0x04u || off == 0x0cu)
                    {
                        int rv = (int)(int16_t)(w1 & 0xffffu);
                        if (rv > 0)
                            gsp->clip_ratio = rv;
                    }
                }
                else if (index == G_MW_FOG)
                {
                    /* S2DEX reuses this index for G_MW_GENSTAT. */
                    s2dex_set_status(off, w1);
                }
                else if (index == G_MW_NUMLIGHT)
                {
                    /* GBI1 encodes the count as 0x80000000 + (n + 1) * 32. */
                    gsp_set_num_lights(gsp,
                        (int)(((w1 - 0x80000000u) >> 5) - 1u));
                }
                break;
            }
            case 0xc4:                          /* G_OBJ_LDTX_RECT_R: load + rect_R */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo, gsp_seg_addr_rsp);
                s2dex_obj_rectangle_r(gsp, r, s_rdram_size, seg_addr(w1) + 24u, fifo);
                break;
            case 0xb4:                          /* G_RDPHALF_1: ucode data pointer */
                s_half1=w1;
                break;
            case 0xb0:                          /* G_SELECT_DL handled with HALF_0 above */
            case 0xb2:                          /* G_OBJ_RECTANGLE_R (rotated; TODO) */
            case 0xbb:                          /* G_TEXTURE (sprite tile sets scale) */
            case 0xb3:                          /* G_RDPHALF_2 (texrect tail) */
            case 0xb6:                          /* G_(CLEAR)GEOMETRYMODE (unused 2D) */
            case 0xb7:
                break;
            case 0xe4:                          /* G_TEXRECT */
            case 0xe5:                          /* G_TEXRECTFLIP */
            {
                /* TEXTURE_RECTANGLE is a 4-word RDP command (angrylion reads
                 * 16 bytes for ids 0x24/0x25). GBI1 S2DEX delivers it as three
                 * DL commands: the G_TEXRECT(FLIP) word pair (xl/yl/xh/yh +
                 * tile) followed by two G_RDPHALF commands carrying the texel
                 * tail -- RDPHALF_1 (0xB4) the s/t pair and RDPHALF_2 (0xB3)
                 * the dsdx/dtdy pair. The opcode was mis-tagged G_RDPHALF_0
                 * and dropped, so the hundreds of texrects Yoshi's Story blits
                 * to build the level backdrop -- into an offscreen image the
                 * G_BG_1CYC compositor later draws -- never reached the RDP and
                 * the stage rendered black. Assemble the full 4-word command
                 * and skip the two RDPHALF words. */
                unsigned int st = 0u, dxy = 0u;
                unsigned int c0;
                int32_t tr[4];
                c0 = rd_u32_be(r, pc);
                if(fifo->fetch_dl && ((c0>>24)==0xb4u || (c0>>24)==0xb3u))
                    fifo->fetch_dl(pc,c0);
                if (((c0 >> 24) & 0xffu) == 0xb4u)        /* s, t */
                {
                    st = rd_u32_be(r, pc + 4);
                    pc += 8;
                }
                else if (((c0 >> 24) & 0xffu) == 0xb3u)   /* dsdx, dtdy */
                {
                    dxy = rd_u32_be(r, pc + 4);
                    pc += 8;
                }
                c0 = rd_u32_be(r, pc);
                if(fifo->fetch_dl && ((c0>>24)==0xb4u || (c0>>24)==0xb3u))
                    fifo->fetch_dl(pc,c0);
                if (((c0 >> 24) & 0xffu) == 0xb4u)
                {
                    st = rd_u32_be(r, pc + 4);
                    pc += 8;
                }
                else if (((c0 >> 24) & 0xffu) == 0xb3u)
                {
                    dxy = rd_u32_be(r, pc + 4);
                    pc += 8;
                }
                tr[0] = (int32_t)w0;    /* keep raw 0xE4/0xE5 byte + xh/yh */
                tr[1] = (int32_t)w1;    /* tile + xl/yl */
                tr[2] = (int32_t)st;    /* s, t       (RDPHALF_1) */
                tr[3] = (int32_t)dxy;   /* dsdx, dtdy (RDPHALF_2) */
                rdp_fifo_append(fifo, tr, 4);
                break;
            }
            case 0x01:                          /* S2DEX1 G_BG_1CYC */
            {
                /* GBI1 S2DEX draws scrolling level backgrounds with
                 * gSPBgRect1Cyc (opcode 0x01), not F3DEX geometry. Yoshi's
                 * Story's in-game scenery is several of these stacked BG
                 * layers; dropping them left the whole stage black. Route to
                 * the same transcribed BG renderer the S2DEX2 path uses. */
                unsigned int bga = seg_addr(w1);
                if (addr_in_range(bga, 40u))
                    s2dex_bg_1cyc(r, s_rdram_size ? s_rdram_size
                                                  : (8u * 1024u * 1024u),
                                  bga, fifo);
                break;
            }
            case 0x02:                          /* S2DEX1 G_BG_COPY */
            {
                unsigned int bga = seg_addr(w1);
                if (addr_in_range(bga, 40u))
                    s2dex_bg_copy(r, s_rdram_size ? s_rdram_size
                                                  : (8u * 1024u * 1024u),
                                  bga, fifo);
                break;
            }
            default:
                if (cmd >= 0xe5)                /* RDP hardware command */
                {
                    int rdp_id = cmd & 0x3f;
                    int32_t two[2];
                    two[0] = (int32_t)w0;
                    /* sniff Set Scissor (0xED -> 0x2d) into the shadow the
                     * S2DEX1 background renderer clips against: BG_COPY /
                     * BG_1CYC read s_scis_ulx to align their strips, and the
                     * game (e.g. Bangai-O) sets an inset 8,8 scissor once and
                     * leaves it resident, so without this the strips draw from
                     * 0,0 and the whole BG shifts 8 texels. The S2DEX2 block
                     * already does this; the GBI1 path was missing it. */
                    if (rdp_id == 0x2d)
                        s2dex_set_scissor(w0, w1);
                    if(rdp_id==0x27 && fifo->dmem)fifo->dmem[0xd2u^3u]=0;
                    /* SET_COLOR_IMAGE (0x3f), SET_Z_IMAGE (0x3e) and
                     * SET_TEXTURE_IMAGE (0x3d) carry a DRAM pointer in w1
                     * that may be segmented; the S2DEX microcode resolves it
                     * before the command reaches the RDP, just as the F3DEX2
                     * splitter does. Forwarding the raw segmented pointer
                     * masked the framebuffer address to 0, so the logo task
                     * rendered to a null color image (whole frame black). */
                    if (rdp_id == 0x3f || rdp_id == 0x3e || rdp_id == 0x3d)
                        two[1] = (int32_t)seg_addr_rsp(w1);
                    else
                        two[1] = (int32_t)w1;
                    rdp_fifo_append(fifo, two, 2);
                }
                break;
            }
            continue;
        }
        if ((s_ucode_class & 0xff) == UCODE_S2DEX2)
        {
            int s2 = 1;
            switch (cmd)
            {
            case 0x05:                  /* G_OBJ_LOADTXTR */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo,
                                   seg_addr);
                break;
            case 0x01:                  /* G_OBJ_RECTANGLE */
                s2dex_obj_rectangle(gsp, r, s_rdram_size, seg_addr(w1), fifo);
                break;
            case 0x02:                  /* G_OBJ_SPRITE */
                s2dex_obj_sprite(gsp, r, s_rdram_size, seg_addr(w1), fifo);
                break;
            case 0x06:                  /* G_OBJ_LDTX_SPRITE (txtr + sprite) */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo,
                                   seg_addr);
                s2dex_obj_sprite(gsp, r, s_rdram_size, seg_addr(w1) + 24u,
                                 fifo);
                break;
            case 0x07:                  /* G_OBJ_LDTX_RECT (txtr + rect) */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo,
                                   seg_addr);
                s2dex_obj_rectangle(gsp, r, s_rdram_size, seg_addr(w1) + 24u,
                                    fifo);
                break;
            case 0x08:                  /* G_OBJ_LDTX_RECT_R (txtr + rect_R) */
                s2dex_obj_loadtxtr(r, s_rdram_size, seg_addr(w1), fifo,
                                   seg_addr);
                s2dex_obj_rectangle_r(gsp, r, s_rdram_size, seg_addr(w1) + 24u,
                                      fifo);
                break;
            case 0xDA:                  /* G_OBJ_RECTANGLE_R */
                s2dex_obj_rectangle_r(gsp, r, s_rdram_size, seg_addr(w1), fifo);
                break;
            case 0xDC:                  /* G_OBJ_MOVEMEM (object matrix) */
                s2dex_obj_movemem(r, s_rdram_size, w0, seg_addr(w1));
                break;
            case 0x04:                  /* G_SELECT_DL handled with HALF_0 above */
                /* A standalone selector has no latched HALF_0 operand. */
                break;
            default:
                s2 = 0;
                break;
            }
            if (s2)
                continue;
        }
        else if ((s_ucode_class & 0xff) == UCODE_L3DEX2 && cmd == 0x08)
        {
            int32_t packet[46]={0};int n;
            if(fifo->read_staging)fifo->read_staging(0,packet,46);
            gsp->line_native=8|(s_textured?2:0);
            gsp->line_clip_3d=1;
            n=gsp_line(gsp,packet,(w0>>17)&127,(w0>>9)&127,(signed char)w0);
            gsp->line_native=0;
            if(n)rdp_fifo_append(fifo,packet,n);
            continue;
        }

        switch (cmd)
        {
        case 0xd5:
            if (s_ucode_class & UCODE_DMA_INLINE) {
                unsigned int bytes = (w0 & 0xffffffu) * 8u;
                unsigned int source = w1 & 0xffffffu;
                /* The secondary DMA list uses gp as its negative byte
                 * counter; k0/k1 and the ordinary call stack stay live. */
                if (bytes && bytes <= 0xa50u && addr_in_range(source, bytes)) {
                    inline_return = pc;
                    inline_end = source + bytes;
                    pc = source;
                    if (fifo->dmem)
                        memcpy(fifo->dmem + 0xa50u - bytes, r + (source & ~7u), bytes);
                }
            } else
                gsp_combine_matrices(gsp);
            break;
        case F3DEX2_MTX:
        {
            int param = (int)((w0 & 0xff) ^ MTX_PUSH);
            int projection = (param & MTX_PROJECTION) ? 1 : 0;
            int load = (param & MTX_LOAD) ? 1 : 0;
            int push = (param & MTX_PUSH) ? 1 : 0;
            unsigned int ma = seg_addr(w1);
            if (addr_in_range(ma, 64u)) {   /* 16 s16 ints + 16 u16 fracs */
                if(fifo->dmem && fifo->matrix_dma_table) {
                    unsigned int at=(fifo->matrix_dma_table+(w0&0xfeu))&0xfffu;
                    unsigned int dest;
#ifndef MSB_FIRST
                    at^=2u;
#endif
                    dest=(*(const unsigned short *)(fifo->dmem+at)+(w0>>5))&0xff8u;
                    if(dest<=0xfc0u)memcpy(fifo->dmem+dest,r+(ma&~7u),64);
                }
                gsp_matrix_load(gsp, r, ma, projection, load, push);
                gsp_sdk_matrix_store(gsp);
            }
            break;
        }
        case F3DEX2_POPMTX:
        {
            /* F3DEX2 G_POPMTX pops w1 / 64 matrices (64 bytes per matrix), not
             * always one. Skeleton rendering pops several levels at once; only
             * popping one leaves the stack too deep and transforms subsequent
             * draws with a stale limb matrix. */
            unsigned int npop = w1 >> 6;
            while (npop--)
                gsp_matrix_pop(gsp, r);
            break;
        }

        case 0x00:
            /* The verified FIFO builds dispatch SPNoOp through a two-word
             * RDP copy handler (2.05 IMEM 0x20c). Keep the actual stream. */
            if (fifo->spnoop || f3dex2_native_texrecip())
            {
                int32_t nop[2];
                nop[0] = (int32_t)w0; nop[1] = (int32_t)w1;
                rdp_fifo_append(fifo, nop, 2);
            }
            break;

        case F3DEX2_VTX:
        {
            int n  = (int)((w0 >> 12) & 0xff);
            int v0 = (int)((w0 >> 1) & 0x7f) - n;
            unsigned int va = seg_addr(w1);
            gsp->cbfd = s_variant_cbfd;
            gsp->acclaim = s_variant_acclaim;
            if (n > 0 && addr_in_range(va, (unsigned int)n * 16u))
            {
                int i;
                unsigned int geometry=gsp->geometry_mode;
                if(fifo->dmem && (!rsp_graphics_sdk_one(5)||!rsp_graphics_sdk_one(7))) {
                    if(!gsp->combined_valid)fifo->dmem[0x1d9u^3u]=(unsigned char)w0;
                    if(!gsp->lights_valid && (geometry&0x20000u))fifo->dmem[0x1dcu^3u]=(unsigned char)w0;
                }
                if((s_ucode_class&0xff)==UCODE_L3DEX2)gsp->geometry_mode&=~0x20000u;
                if (s_variant_cbfd && fifo->dmem)
                    gsp_vertex_cbfd(gsp,r,fifo->dmem,va,n,v0);
                else
                    gsp_vertex(gsp, r, va, n, v0);
                gsp->geometry_mode=geometry;
                /* FIFO 2.05's lighting overlay merges the source alpha into
                 * v29, then VMULF by 0x7fff (0x434/0x4cc) before SUV stores
                 * the byte. The RGB-only light helper must not leave alpha
                 * unscaled. Fog subsequently replaces alpha; MODIFYVTX RGBA
                 * writes are already-final bytes and must stay untouched. */
                if (((s_ucode_class & 0xff) == UCODE_F3DEX205 || (s_ucode_class & UCODE_LIT_ALPHA)) &&
                    (gsp->geometry_mode & 0x00020000u) &&
                    !(gsp->geometry_mode & 0x00010000u))
                    for (i = 0; i < n; ++i)
                        if (v0 + i >= 0 && v0 + i < GSP_MAX_VERTICES) {
                            GSPVertex *v = &gsp->vtx[v0 + i];
                            unsigned int alpha = (unsigned int)v->a >> 16;
                            v->a = (int32_t)((alpha * 0x7fffu + 0x80u) >> 15) << 16;
                        }
                for(i=0;i<n;i++)if(v0+i>=0)gsp_sdk_vertex_store(gsp,(unsigned int)(v0+i));
            }
            break;
        }

        case 0x09: /* S2DEX2 G_BG_1CYC */
        {
            /* Zelda's pre-rendered backgrounds switch the RSP to the
             * S2DEX2 microcode (gSPLoadUcodeL) for one gSPBgRect1Cyc
             * command. The BG opcodes are unused in F3DZEX2, so route
             * them to the transcribed S2DEX2 background renderer. */
            unsigned int bga = seg_addr(w1);
            if (addr_in_range(bga, 40u))
                s2dex_bg_1cyc(r, s_rdram_size ? s_rdram_size
                                              : (8u * 1024u * 1024u),
                              bga, fifo);
            break;
        }

        case 0x0a: /* S2DEX2 G_BG_COPY */
        {
            /* gSPBgRectCopy: the transcribed copy-mode renderer */
            unsigned int bga = seg_addr(w1);
            if (addr_in_range(bga, 40u))
                s2dex_bg_copy(r, s_rdram_size ? s_rdram_size
                                              : (8u * 1024u * 1024u),
                              bga, fifo);
            break;
        }

        case 0x0b: /* S2DEX2 G_OBJ_RENDERMODE */
            s2dex_set_obj_rendermode(w1);
            break;

        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1a: case 0x1b:
        case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        {
            /* Both Conker lighting variants use G_TRI4, including the
             * normal-indexed resident selected by a mid-list LOAD_UCODE.
             * Opcodes 0x10..0x1f each draw four
             * triangles from twelve 5-bit vertex indices. The third index of
             * the first triangle is split across the two command words (three
             * high bits in w0, two low bits in w1); the other eleven indices
             * are packed 5 bits each. Only active on the CBFD build -- every
             * other F3DEX2 title leaves 0x10..0x1f as unused no-ops. */
            if (s_variant_cbfd)
            {
                int idx[4][3];
                int t;
                /* Native 0x994..0xa64 consumes w1 high, w1 low, w0 low,
                 * then the triangle split across the two words. */
                idx[3][0] = (int)((w0 >> 23) & 0x1f);
                idx[3][1] = (int)((w0 >> 18) & 0x1f);
                idx[3][2] = (int)((((w0 >> 15) & 0x07) << 2)
                                  | ((w1 >> 30) & 0x03));
                idx[2][0] = (int)((w0 >> 10) & 0x1f);
                idx[2][1] = (int)((w0 >>  5) & 0x1f);
                idx[2][2] = (int)((w0 >>  0) & 0x1f);
                idx[0][0] = (int)((w1 >> 25) & 0x1f);
                idx[0][1] = (int)((w1 >> 20) & 0x1f);
                idx[0][2] = (int)((w1 >> 15) & 0x1f);
                idx[1][0] = (int)((w1 >> 10) & 0x1f);
                idx[1][1] = (int)((w1 >>  5) & 0x1f);
                idx[1][2] = (int)((w1 >>  0) & 0x1f);
                for (t = 0; t < 4; t++)
                {
                    if(fifo->store_indices) {
                        int32_t old=0;
                        if(fifo->read_staging)fifo->read_staging(4u,&old,1u);
                        fifo->store_indices(((unsigned int)old&0xff000000u)
                            |((unsigned int)idx[t][0]<<17)
                            |((unsigned int)idx[t][1]<<9)
                            |((unsigned int)idx[t][2]<<1));
                    }
                    int32_t cmdw[GSP_TRI_CMD_WORDS];
                    int nc;
                    if (idx[t][0] == idx[t][1] && idx[t][0] == idx[t][2])
                        continue;   /* degenerate padding slot */
                    nc = gsp_triangle_with_texrecip(gsp, cmdw, idx[t][0], idx[t][1],
                                      idx[t][2], s_textured, s_zbuffered, f3dex2_native_texrecip());
                    if (nc > 0) rdp_fifo_append(fifo, cmdw, nc);
                }
            }
            break;
        }

        case F3DEX2_TRI1:
        {
            int a = (int)((w0 >> 17) & 0x7f);
            int b = (int)((w0 >> 9) & 0x7f);
            int c = (int)((w0 >> 1) & 0x7f);
            int32_t cmdw[GSP_TRI_CMD_WORDS];
            int nc;
            if (fifo->store_indices) fifo->store_indices(w0);
            nc = gsp_triangle_with_texrecip(gsp, cmdw, a, b, c, s_textured, s_zbuffered, f3dex2_native_texrecip());
            if (nc > 0) rdp_fifo_append(fifo, cmdw, nc);
            break;
        }

        case F3DEX2_TRI2:
        case F3DEX2_QUAD:
        {
            int a0 = (int)((w0 >> 17) & 0x7f);
            int b0 = (int)((w0 >> 9) & 0x7f);
            int c0 = (int)((w0 >> 1) & 0x7f);
            int a1 = (int)((w1 >> 17) & 0x7f);
            int b1 = (int)((w1 >> 9) & 0x7f);
            int c1 = (int)((w1 >> 1) & 0x7f);
            int32_t cmdw[GSP_TRI_CMD_WORDS];
            int nc;
            /* The G_TRI2 handler stores the second triangle's indices and
             * jals tri_to_rdp before falling through to the G_TRI1 path
             * for the first triangle: the microcode emits the SECOND
             * triangle of the pair first. */
            if(gsp->clip_reject) {
                /* Rej's paired vector halves emit the first half first. */
                int tmp=a0;a0=a1;a1=tmp;
                tmp=b0;b0=b1;b1=tmp;
                tmp=c0;c0=c1;c1=tmp;
            }
            if (fifo->store_indices) fifo->store_indices(w1);
            nc = gsp_triangle_with_texrecip(gsp, cmdw, a1, b1, c1, s_textured, s_zbuffered, f3dex2_native_texrecip());
            if (nc > 0) rdp_fifo_append(fifo, cmdw, nc);
            if (fifo->store_indices) fifo->store_indices(w0);
            nc = gsp_triangle_with_texrecip(gsp, cmdw, a0, b0, c0, s_textured, s_zbuffered, f3dex2_native_texrecip());
            if (nc > 0) rdp_fifo_append(fifo, cmdw, nc);
            break;
        }

        case F3DEX2_DL:
        {
            int nopush = (int)((w0 >> 16) & 0xff) & DL_NOPUSH;
            unsigned int da = seg_addr(w1);
            if (nopush)
            {
                /* branch (no return): continue at the target */
                if (s_streaming && da == pc - 8u)
                {
                    /* the streaming list's live tail: a branch to its own
                     * address. Suspend here; the CPU will patch this
                     * command, and the next slice re-reads it. */
                    s_stream_pc = pc - 8u;
                    s_stream_stalled = 1;
                    running = 0;
                }
                else if (addr_in_range(da, 8u)) {
                    pc = da;
                    if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0);
                } else
                    DL_RETURN();        /* invalid branch ended the list */
            }
            else if (addr_in_range(da, 8u))
            {
                if (s_dl_sp < DL_MAX_DEPTH)
                {
                    if(fifo->dmem && (!rsp_graphics_sdk_one(5)||!rsp_graphics_sdk_one(7)))
                        memcpy(fifo->dmem+0x138u+s_dl_sp*4u,&pc,4);
                    s_dl_stack[s_dl_sp++] = pc;
                    pc = da;
                    if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0);
                }
                /* at the depth bound the call is dropped, as the old
                 * recursion guard did */
            }
            break;
        }

        case F3DEX2_MOVEWORD:
        {
            /* G_MOVEWORD with index G_MW_SEGMENT (0x06) sets a segment-table
             * base: segment = (w0 >> 2) & 0xf, base = w1 & 0xffffff. Other
             * MOVEWORD indices (numlight, fog, clip, ...) don't affect address
             * resolution here, so ignore them. */
            int index = (int)((w0 >> 16) & 0xff);
            if (s_variant_cbfd && fifo->dmem && index<=0x10 && !(index&1)) {
                unsigned int table=0x560u+(unsigned int)index;
                unsigned int destination;
#ifndef MSB_FIRST
                table^=2u;
#endif
                /* The resident's MOVEWORD handler reads this base table.
                 * Preserve both paired coordinate/light copies in DMEM. */
                destination=(*(const unsigned short *)(fifo->dmem+table)+(w0&0xffffu))&0xfffu;
                if (!(destination&3u)) memcpy(fifo->dmem+destination,&w1,4);
            }
            if ((s_ucode_class & 0xff) == UCODE_S2DEX2 && index == G_MW_FOG)
            {
                s2dex_set_status(w0 & 0xffffu, w1);
            }
            else if (index == G_MW_SEGMENT)
            {
                unsigned int seg = (w0 >> 2) & 0x0fu;
                s_seg_table[seg] = w1;
            }
            else if (index == 0x00)
            {
                /* G_MW_MATRIX: gSPInsertMatrix patches one 32-bit word of
                 * the combined MVP in DMEM (two s16 element halves).
                 * Offsets 0x00-0x1f address the integer halves of the
                 * row-major 4x4, 0x20-0x3f the fraction halves. The write
                 * lands on the current combined copy; a later G_MTX
                 * recompute discards it, exactly as on the RSP. Kirby 64
                 * billboards its sprite quads this way (recompute via
                 * G_SPECIAL_1, then overwrite the rotation rows). */
                unsigned int off = w0 & 0xffffu;
                if (off < 0x40u && (off & 3u) == 0u)
                {
                    int mr = (int)((off & 0x1fu) >> 3);
                    int mc = (int)((off & 7u) >> 1);
                    int32_t *e0 = &gsp->combined[mr][mc];
                    int32_t *e1 = &gsp->combined[mr][mc + 1];
                    if (off < 0x20u)
                    {
                        *e0 = (int32_t)((w1 & 0xffff0000u)
                                        | ((uint32_t)*e0 & 0xffffu));
                        *e1 = (int32_t)(((w1 & 0xffffu) << 16)
                                        | ((uint32_t)*e1 & 0xffffu));
                    }
                    else
                    {
                        *e0 = (int32_t)(((uint32_t)*e0 & 0xffff0000u)
                                        | ((w1 >> 16) & 0xffffu));
                        *e1 = (int32_t)(((uint32_t)*e1 & 0xffff0000u)
                                        | (w1 & 0xffffu));
                    }
                }
            }
            else if (index == G_MW_NUMLIGHT)
            {
                /* CBFD packs the light count as n * 48, stock as n * 24. */
                gsp_set_num_lights(gsp,
                    (int)(w1 / (s_variant_cbfd ? 48u : 24u)));
            }
            else if (index == G_MW_LIGHTCOL)
            {
                /* gSPLightColor: recolor a light without touching its
                 * direction. Two movewords per call (col and colc copies,
                 * 4 bytes apart) carry the same rgba32; the light index is
                 * the 24-byte slot of the 16-bit offset. The ambient is the
                 * last light, so index == numlights recolors the ambient
                 * with no special casing. */
                unsigned int ofs = w0 & 0xffffu;
                unsigned int stride=s_variant_cbfd?48u:24u;
                if ((ofs % stride) == 0)
                    gsp_set_light_color(gsp, (int)(ofs / stride),
                                        (int32_t)((w1 >> 24) & 0xffu),
                                        (int32_t)((w1 >> 16) & 0xffu),
                                        (int32_t)((w1 >> 8) & 0xffu));
            }
            else if (index == G_MW_FOG)
            {
                /* gSPFogFactor / gSPFogPosition: fog multiplier in the high
                 * s16 of w1, offset in the low s16. */
                gsp_set_fog(gsp, (int)(short)((w1 >> 16) & 0xffffu),
                                 (int)(short)(w1 & 0xffffu));
            }
            else if (index == 0x04)
            {
                /* G_MW_CLIP (gSPClipRatio): four words set the guard-band
                 * clip ratio, offsets 0x04/0x0c = +RX/+RY (positive value)
                 * and 0x14/0x1c = -RX/-RY (negative as u16). The gbi sets
                 * all four consistently; take the magnitude from the
                 * positive words. OoT's pause screen sets ratio 1 (clip at
                 * the screen edges) and the scene restores 2; the scaled
                 * outcodes and the clipper's plane rows both depend on it. */
                unsigned int off = w0 & 0xffffu;
                if (off == 0x04u || off == 0x0cu)
                {
                    /* Any positive ratio is legal: the value is written
                     * verbatim into the DMEM clip table's w column, and
                     * the VCH scale multiply clamps it through the
                     * accumulator like every other ratio. Excitebike 64
                     * runs its 3D scenes at FRUSTRATIO_6 (the gbi's
                     * maximum); the old <= 4 guard silently dropped that,
                     * leaving the walker's scaled outcodes and clip
                     * planes at whatever ratio was set before. */
                    int rv = (int)(int16_t)(w1 & 0xffffu);
                    if (rv > 0)
                        gsp->clip_ratio = rv;
                }
            }
            else if (index == G_MW_PERSPNORM)
            {
                /* gSPPerspNormalize: the low 16 bits are the perspective
                 * normalization scale the RSP applies to 1/w. */
                gsp_set_persp_norm(gsp, w1 & 0xffffu);
            }
            else if (index == 0x10 && s_variant_cbfd)
            {
                /* Conker G_MW_COORD_MOD: the vertex-position modifier the CBFD
                 * point-light path uses. Bit 3 selects the second vertex's
                 * copy, retained above in native DMEM; bits 1..2
                 * index the row; w0 & 0x30 selects which pair of rows the two
                 * s16 halves of w1 write: 0x00 -> integer offset rows 0/1,
                 * 0x10 -> 16.16 scale rows 4/5 and the combined rows 12/13
                 * (offset << 16 + scale), 0x20 -> integer rows 8/9. */
                if ((w0 & 8u) == 0u)
                {
                    int cmi = (int)((w0 >> 1) & 3u);
                    unsigned int cpos = w0 & 0x30u;
                    int32_t chi = (int32_t)(short)((w1 >> 16) & 0xffffu);
                    int32_t clo = (int32_t)(short)(w1 & 0xffffu);
                    if (cpos == 0x00u)
                    {
                        gsp->cbfd_cmod[0 + cmi] = chi;
                        gsp->cbfd_cmod[1 + cmi] = clo;
                    }
                    else if (cpos == 0x10u)
                    {
                        gsp->cbfd_cmod[4 + cmi] = chi;
                        gsp->cbfd_cmod[5 + cmi] = clo;
                        gsp->cbfd_cmod[12 + cmi] =
                            (gsp->cbfd_cmod[0 + cmi] << 16)
                            + gsp->cbfd_cmod[4 + cmi];
                        gsp->cbfd_cmod[13 + cmi] =
                            (gsp->cbfd_cmod[1 + cmi] << 16)
                            + gsp->cbfd_cmod[5 + cmi];
                    }
                    else if (cpos == 0x20u)
                    {
                        gsp->cbfd_cmod[8 + cmi] = chi;
                        gsp->cbfd_cmod[9 + cmi] = clo;
                    }
                }
            }
            break;
        }

        case F3DEX2_MOVEMEM:
        {
            /* G_MOVEMEM with index G_MV_VIEWPORT (0x08) loads the viewport
             * (vscale/vtrans) from the segmented address in w1. Other MOVEMEM
             * targets (lights, matrices) are not needed for screen mapping. */
            int index = (int)(w0 & 0xff);
            if (index == G_MV_LIGHT && s_variant_acclaim
                && (((w0 >> 5) & 0x7ffu) >= 0x60u))
            {
                /* Acclaim custom lighting: eight 16-byte light structs load via
                 * G_MV_LIGHT to DMEM 0x250 + n*16, encoded as movemem byte
                 * offset 0x60 + n*16 (light 0 at 0x60). w1 points straight at
                 * the struct in RDRAM. Layout:
                 *   [0..5]   position x,y,z (s16, eye space)
                 *   [6][7][8] R,G,B (byte; the RSP scales by <<7 internally)
                 *   [9]      pad
                 *   [10..11] A: L1-distance cutoff (s16). Bit 0x8000 set marks
                 *            the light disabled (the ucode tests lh & 0x8000
                 *            and skips the accumulate).
                 *   [12..13] B: intensity scale (s16)
                 *   [14..15] per-channel output clamp (s16, 0x7F80)
                 * The Acclaim microcode reuses G_MV_LIGHT for both these custom
                 * L1 lights (DMEM offset 0x60+) AND the stock directional/
                 * ambient lights (offsets 0x00..0x58, the same 24-byte slots
                 * every F3DEX2 build uses). Only the 0x60+ range is the custom
                 * table; the lower offsets must fall through to the stock light
                 * loader below, or geometry drawn with plain G_LIGHTING (no
                 * custom-flood geometry-mode bit) loses its lights and renders
                 * flat white -- e.g. Turok 2 walls seen head-on. */
                unsigned int aoff = (w0 >> 5) & 0x7ffu;
                unsigned int la = seg_addr(w1);
                if (aoff >= 0x60u && aoff < 0x60u + 8u * 16u
                    && addr_in_range(la, 16u))
                {
                    int an = (int)((aoff - 0x60u) / 16u);
                    gsp->accl_pos[an][0] = (int)(short)
                        ((r[(la + 0) ^ 3u] << 8) | r[(la + 1) ^ 3u]);
                    gsp->accl_pos[an][1] = (int)(short)
                        ((r[(la + 2) ^ 3u] << 8) | r[(la + 3) ^ 3u]);
                    gsp->accl_pos[an][2] = (int)(short)
                        ((r[(la + 4) ^ 3u] << 8) | r[(la + 5) ^ 3u]);
                    gsp->accl_rgb[an][0] = (int)r[(la + 6) ^ 3u];
                    gsp->accl_rgb[an][1] = (int)r[(la + 7) ^ 3u];
                    gsp->accl_rgb[an][2] = (int)r[(la + 8) ^ 3u];
                    gsp->accl_a[an] = (int)(short)
                        ((r[(la + 10) ^ 3u] << 8) | r[(la + 11) ^ 3u]);
                    gsp->accl_b[an] = (int)(short)
                        ((r[(la + 12) ^ 3u] << 8) | r[(la + 13) ^ 3u]);
                    gsp->accl_clampmax = (int)(short)
                        ((r[(la + 14) ^ 3u] << 8) | r[(la + 15) ^ 3u]);
                    if (an + 1 > gsp->acclaim_nlights)
                        gsp->acclaim_nlights = an + 1;
                }
            }
            else if (index == G_MV_NORMALES && s_variant_cbfd)
            {
                /* Conker CBFD: the base of the per-vertex normal table the
                 * CBFD vertex loader reads (two s8 per vertex, slot * 2). */
                gsp->cbfd_nbase = seg_addr(w1);
            }
            else if (index == G_MV_LIGHT && s_variant_cbfd)
            {
                /* CBFD G_MV_LIGHT uses 48-byte destination slots (stock uses
                 * 24): n = (w0 bits 5..18) / 48; n < 2 is a LookAt entry,
                 * n >= 2 a directional/ambient light slot n - 2. */
                unsigned int coff = (w0 >> 5) & 0x3fffu;
                int cn = (int)(coff / 48u);
                unsigned int cla = seg_addr(w1);
                const unsigned char *light_memory = r;
                unsigned int light_address = cla;
                int light_valid = addr_in_range(cla,cn>=2?48u:16u);
                gsp->cbfd = 1;
                if (fifo->dmem) {
                    unsigned int bytes=(((w0>>19)&31u)+1u)*8u;
                    unsigned int destination=0x260u+coff;
                    if (destination<=0x1000u && bytes<=0x1000u-destination && addr_in_range(cla,bytes))
                        memcpy(fifo->dmem+destination,r+cla,bytes);
                    light_memory=fifo->dmem;
                    light_address=destination;
                    light_valid=destination<=0x1000u && (cn>=2?48u:16u)<=0x1000u-destination;
                }
                if (light_valid)
                {
                    if (cn >= 2)
                        gsp_set_light(gsp, light_memory, light_address, cn - 2);
                    else
                        gsp_set_lookat(gsp, light_memory, light_address, cn);
                }
            }
            else if (index == G_MV_VIEWPORT)
            {
                unsigned int vp = seg_addr(w1);
                if (addr_in_range(vp, 16u))     /* 8 s16: vscale + vtrans */
                    gsp_set_viewport(gsp, r, vp);
            }
            else if (index == 0x0e)
            {
                /* G_MV_MATRIX: gSPForceMatrix's MOVEMEM half. The CPU has
                 * already concatenated modelview and projection and hands the
                 * microcode the finished combined matrix; the DMA lands the
                 * 64-byte packed matrix straight in the mvp slot, bypassing
                 * the on-RSP multiply. Star Wars Ep1 Racer forces a
                 * CPU-computed MVP before every world batch -- recombining
                 * the stacks instead put every batch behind a stale
                 * modelview. The paired MW_FORCEMTX moveword only rewrites
                 * the mvpValid flag, which loading the matrix here already
                 * implies. */
                unsigned int ma = seg_addr(w1);
                if (addr_in_range(ma, 64u))
                {
                    unsigned int ch;
                    for (ch = 0u; ch < 64u; ch += 16u)
                        gsp_force_matrix_chunk(gsp, r, ma + ch, ch);
                }
            }
            else if (index == G_MV_LIGHT)
            {
                /* G_MOVEMEM/G_MV_LIGHT loads one light. The destination slot is
                 * encoded in w0 bits 5..15 as a byte offset: offset =
                 * ((w0 >> 5) & 0x7ff) & 0x7f8, n = offset / 24. n < 2 selects a
                 * LookAt entry (ignored -- only used for texture-coord gen);
                 * n >= 2 is directional/ambient light slot n - 2. */
                unsigned int offset = ((w0 >> 5) & 0x7ffu) & 0x7f8u;
                int n = (int)(offset / 24u);
                if (n >= 2)
                {
                    unsigned int la = seg_addr(w1);
                    if (addr_in_range(la, 24u))
                        gsp_set_light(gsp, r, la, n - 2);
                }
                else
                {
                    /* slots 0/1: the texgen lookat X/Y directions. F3DFLX
                     * repurposes the n==1 entry as the reflection "alpha
                     * light" (an s16 direction) and the custom routine reads
                     * it from slot 0. */
                    unsigned int la = seg_addr(w1);
                    if (addr_in_range(la, 24u))
                    {
                        if (gsp->no_texgen && n == 1)
                            gsp_set_alpha_light(gsp, r, la, 0);
                        else
                            gsp_set_lookat(gsp, r, la, n);
                    }
                }
            }
            break;
        }

        case 0xE1:                       /* G_RDPHALF_1: stage branch target */
            s_half1 = w1;
            break;

        case 0xD6:                       /* G_DMA_IO (gSPDmaRead/Write) */
        {
            /* F3DFLX's racer draw streams a 1D reflection ramp into DMEM with
             * gSPDmaRead(0x8B0, ...) just before the body. The microcode's
             * custom lighting routine indexes it with the lookat dot product
             * to produce a per-vertex fog factor (the "reflection"). The HLE
             * vertex path needs the ramp to reproduce that alpha, so capture
             * it on the DMEM-read direction. */
            unsigned int flag = (w0 >> 23) & 1u;
            unsigned int size = (w0 & 0xfffu) + 1u;
            if (flag == 0u && size <= sizeof(gsp->reflect_lut))
            {
                unsigned int src = seg_addr(w1);
                if (src != 0u && src + size <= s_rdram_size)
                {
                    unsigned int k;
                    for (k = 0; k < size; k++)
                        gsp->reflect_lut[k] = r[(src + k) ^ 3u];
                    for (; k < sizeof(gsp->reflect_lut); k++)
                        gsp->reflect_lut[k] = 0u;
                    gsp->reflect_valid = 1;
                }
            }
            break;
        }

        case 0xDD:                       /* G_LOAD_UCODE (gSPLoadUcodeEx) */
        {
            /* w1 = microcode text address; the preceding G_RDPHALF_1
             * staged the data address. The RSP swaps the running
             * microcode; the walker swaps its opcode interpretation. */
            unsigned int text = seg_addr(w1);
            if (fifo->load_ucode)
                fifo->load_ucode(fifo, text, seg_addr(s_half1), (w0 & 0xffffu) + 1u);
            s_ucode_class = probe_ucode_class(r, text, seg_addr(s_half1));
            s2dex_set_version2((s_ucode_class & 0xff) == UCODE_S2DEX2);
            f3dex2_set_variant_cbfd(r,seg_addr(s_half1),(w0&0xffffu)+1u);
            /* The microcode reload re-DMAs the data segment, restoring
             * the S2DEX scissor/status defaults. The data address was
             * staged by the preceding G_RDPHALF_1; re-detect the clip
             * profile from it, since titles like Excitebike 64 swap
             * between microcode builds with different clip schemes
             * mid-list. */
            if ((s_ucode_class & 0xff) == UCODE_S2DEX2) {
                s2dex_reset();
                s2dex_seed_scissor(fifo->dmem);
            } else
                gsp_detect_ucode_params(gsp, r, s_rdram_size,
                                        seg_addr(s_half1), text);
            break;
        }


        case 0x03:                       /* G_CULLDL (gSPCullDisplayList) */
        {
            /* The RSP tests the AND of the clip flags of vertices
             * vstart..vend (inclusive): if every vertex lies outside the
             * same screen frustum plane (x or y beyond +-w), it jumps to
             * the ENDDL handler, terminating the remainder of the current
             * display list. OoT bounds each room mesh section and many
             * actors this way; without it the walker renders sub-lists the
             * RSP rejects, and their guard-band-clipped remnants land as
             * extra layers over near geometry. Vertices with w <= 0 carry
             * no outside flags here, which can only under-cull (render
             * more), never over-cull. */
            int v0 = (int)((w0 & 0xffffu) >> 1);
            int v1 = (int)((w1 & 0xffffu) >> 1);
            if (v0 >= 0 && v1 >= v0 && v1 < GSP_MAX_VERTICES)
            {
                unsigned int all = GSP_CLIP_REJECT;
                int vi;
                for (vi = v0; vi <= v1 && all; vi++)
                    all &= (unsigned int)gsp->vtx[vi].clip;
                if (all)
                    DL_RETURN();
            }
            break;
        }

        case 0x04:                       /* F3DZEX2 G_BRANCH_W (gSPBranchLessW) */
        {
            /* Branch (no return) to the DL staged by the preceding RDPHALF_1
             * when vertex[(w0>>1)&0x7f]'s clip-space w integer part is less
             * than w1. OoT's F3DZEX2 uses this for LOD selection and
             * distance-based skip; ignoring it renders the wrong variant. */
            int bv = (int)((w0 >> 1) & 0x7f);
            unsigned int ba = seg_addr(s_half1);
            if (bv >= 0 && bv < GSP_MAX_VERTICES && addr_in_range(ba, 8u))
            {
                /* F3DZEX2 compares the s16 clip-w integer; plain F3DEX2's
                 * G_BRANCH_Z compares the 32-bit screen-z word stored at
                 * vertex+0x1c (sub; bgez -> fall through). */
                int32_t lhs = gsp->branch_z_mode
                            ? gsp->vtx[bv].scr_z
                            : (gsp->vtx[bv].cw >> 16);
                if (lhs < (int32_t)w1) {
                    pc = ba;
                    if (fifo->fetch_dl) fifo->fetch_dl(~0u, 0);
                }
            }
            break;
        }

        case F3DEX2_ENDDL:
            DL_RETURN();
            break;

        case F3DEX2_TEXRECT:
        case F3DEX2_TEXRECTFLIP:
        {
            /* TEXTURE_RECTANGLE is a 4-word RDP command (angrylion reads 16
             * bytes for ids 0x24/0x25), but the F3DEX2 gSPTextureRectangle
             * macro delivers it as three display-list commands: the
             * G_TEXRECT(FLIP) word pair carries the rectangle (xl/yl/xh/yh +
             * tile) and the following two G_RDPHALF commands carry the texture
             * coordinates -- RDPHALF_2 (0xE1) the s/t pair and RDPHALF_1 (0xF1)
             * the dsdx/dtdy pair. The microcode's texrect path only completes
             * and issues the RDP rectangle once it has pulled those two
             * RDPHALF words; the coordinates live nowhere else.
             *
             * A G_TEXRECT opcode NOT followed by the RDPHALF pair is therefore
             * not a rectangle the RSP would ever emit -- the microcode consumes
             * the opcode and moves on, and the words that follow are executed
             * as their own commands (they surface in the RDP stream as no-ops
             * when their high byte is 0x00). Some streams reach the 0xE4 opcode
             * this way; emitting a rectangle for it invents geometry the RSP
             * never draws (a texel-(0,0) block that shows up as a flat
             * primitive-coloured square) and, worse, swallowing two words as
             * the missing coordinate tail eats the following commands -- a
             * G_MTX in the case that first surfaced this -- and desynchronises
             * the rest of the frame.
             *
             * So peek the next command: only when it is an RDPHALF do we treat
             * this as a real rectangle, pull both halves, and skip them. When
             * it is not, emit nothing and fall through, leaving the following
             * words for the normal walker -- matching the LLE RSP, which draws
             * no rectangle here and lets those words run as commands. */
            unsigned int nb = (rd_u32_be(r, pc) >> 24) & 0xff;
            if (nb == F3DEX2_RDPHALF_1 || nb == F3DEX2_RDPHALF_2)
            {
                unsigned int h0 = 0u, h1 = 0u;
                unsigned int c0;
                int32_t tr[4];
                /* first RDPHALF */
                c0 = rd_u32_be(r, pc);
                hle_timing_command(c0 >> 24);
                if (((c0 >> 24) & 0xff) == F3DEX2_RDPHALF_2)
                    h0 = rd_u32_be(r, pc + 4);
                else if (((c0 >> 24) & 0xff) == F3DEX2_RDPHALF_1)
                    h1 = rd_u32_be(r, pc + 4);
                pc += 8;
                /* second RDPHALF */
                c0 = rd_u32_be(r, pc);
                hle_timing_command(c0 >> 24);
                if (((c0 >> 24) & 0xff) == F3DEX2_RDPHALF_2)
                    h0 = rd_u32_be(r, pc + 4);
                else if (((c0 >> 24) & 0xff) == F3DEX2_RDPHALF_1)
                    h1 = rd_u32_be(r, pc + 4);
                pc += 8;
                tr[0] = (int32_t)w0; /* keep the raw 0xE4/0xE5 byte, as the RSP does */
                tr[1] = (int32_t)w1;
                tr[2] = (int32_t)h0; /* s, t   (RDPHALF_2) */
                tr[3] = (int32_t)h1; /* dsdx, dtdy (RDPHALF_1) */
                /* S2DEX FIFO 2.05's HALF_2 handler (low overlay 0x128)
                 * restores the list scissor before issuing the rectangle.
                 * BG/object commands can have left a narrower RDP scissor. */
                if ((s_ucode_class & 0xff) == UCODE_S2DEX2)
                    s2dex_emit_scissor(fifo);
                rdp_fifo_append(fifo, tr, 4);
            }
            break;
        }

        case F3DEX2_SETOTHERMODE_H:
        case F3DEX2_SETOTHERMODE_L:
        {
            /* Partial other-modes update. F3DEX2 encodes a bitfield write as
             * length = (w0 & 0xff) + 1 bits, placed at
             * shift = 32 - ((w0 >> 8) & 0xff) - length (clamped at 0); the new
             * bits come from w1. Merge into the mirrored high (0xE3) or low
             * (0xE2) word, then emit a full RDP SET_OTHER_MODES so angrylion
             * sees the resulting cycle type / render mode. */
            unsigned int len = ((w0 >> 0) & 0xffu) + 1u;
            unsigned int shb = (w0 >> 8) & 0xffu;
            unsigned int shift;
            unsigned int mask;
            int32_t two[2];
            if ((32u - shb) < len)
                shift = 0u;
            else
                shift = 32u - shb - len;
            if (len >= 32u)
                mask = 0xffffffffu;
            else
                mask = ((1u << len) - 1u) << shift;
            /* As in the GBI1 handler: the microcode clears the field but
             * ORs the full w1, so bits outside the addressed field stick.
             * SDK-built lists never carry any, but sloppy lists (Top Gear
             * Rally on GBI1) depend on the hardware behavior. */
            if (cmd == F3DEX2_SETOTHERMODE_H)
                s_othermode_h = (s_othermode_h & ~mask)
                              | (unsigned int)w1
                              | (0x2fu << 24);
            else
                s_othermode_l = (s_othermode_l & ~mask)
                              | (unsigned int)w1;
            s_zbuffered = (((s_othermode_l >> 4) & 1u) ||
                           ((s_othermode_l >> 5) & 1u)) ? 1 : 0;
            two[0] = (int32_t)(s_othermode_h | (0x2fu << 24));
            two[1] = (int32_t)s_othermode_l;
            rdp_fifo_append(fifo, two, 2);
            break;
        }

        case F3DEX2_RDPSETOTHERMODE:
        {
            /* Wholesale other-modes write: w0 carries the high word (with the
             * command byte already in bits 29-24), w1 the low word. Mirror both
             * and forward verbatim as SET_OTHER_MODES. */
            int32_t two[2];
            s_othermode_h = (unsigned int)w0;
            s_othermode_l = (unsigned int)w1;
            s_zbuffered = (((s_othermode_l >> 4) & 1u) ||
                           ((s_othermode_l >> 5) & 1u)) ? 1 : 0;
            two[0] = (int32_t)w0;
            two[1] = (int32_t)w1;
            rdp_fifo_append(fifo, two, 2);
            break;
        }

        case F3DEX2_TEXTURE:
            /* G_TEXTURE sets the texture scale/tile/level the frontend uses
             * for emitted texcoords, and the on/off flag that selects whether
             * subsequent triangles are textured. w1 holds the S/T scale
             * (s.16 each); w0 bits select tile, max LOD level, and on. */
        {
            int on    = (int)((w0 >> 1) & 0x7f);
            int level = (int)((w0 >> 11) & 0x07);
            int tile  = (int)((w0 >> 8) & 0x07);
            unsigned int ss = (unsigned int)((w1 >> 16) & 0xffff);
            unsigned int ts = (unsigned int)(w1 & 0xffff);
            s_textured = (on != 0) ? 1 : 0;
            gsp_set_texture(gsp, ss, ts, tile, level, gsp->tex_w, gsp->tex_h);
            break;
        }

        case F3DEX2_GEOMETRY:
            /* G_GEOMETRYMODE clears the bits in ~(w0 & 0xffffff) and sets the
             * bits in w1: mode = (mode & (w0 & 0xffffff)) | w1. The cull-enable
             * bits (G_CULL_FRONT/G_CULL_BACK) drive backface rejection, which
             * the RSP does on the CPU before submitting triangles -- angrylion
             * (software RDP) performs no culling, so we must apply it here. */
            gsp_set_geometry_mode(gsp,
                (gsp_get_geometry_mode(gsp) & (w0 & 0x00ffffffu)) | w1);
            break;

        case 0x02:
        {
            /* G_MODIFYVTX: byte 1 selects the field (G_MWO_POINT_*), the
             * low half-word is the vertex index times two, w1 the value. */
            gsp_modify_vertex(gsp, (int)((w0 & 0xffffu) / 2u),
                              (w0 >> 16) & 0xffu, (unsigned int)w1);
            break;
        }

        default:
            /* RDP render-state commands embedded in the display list
             * (G_SETxIMG, G_SETSCISSOR, G_SETCOMBINE, G_RDPSETOTHERMODE,
             * G_SETTILE/SIZE, G_LOADBLOCK/TILE/TLUT, the SET_*_COLOR commands,
             * the sync commands, and the rectangles) are already in RDP wire
             * format: angrylion decodes the command as (word0 >> 24) & 0x3f,
             * and the GBI bytes 0xE0..0xFF map exactly onto the RDP command
             * ids under that mask. So these are forwarded to the FIFO
             * verbatim -- this is the gDP -> RDP "translation", which for the
             * shared commands is a pass-through.
             *
             * The geometry commands (handled above) are the only ones the RSP
             * actually computes; everything else the RSP passes to the RDP
             * unchanged, and so do we. Commands outside the RDP range are
             * RSP-only (matrix/vertex/etc, already handled) or unknown and
             * are skipped. */
            if (cmd >= 0xc8 && cmd != F3DEX2_DL && cmd != F3DEX2_ENDDL)
            {
                int rdp_id = cmd & 0x3f;
                /* sniff Set Other Modes (0xEF -> 0x2f) for the depth-test
                 * enables so subsequent triangles pick the Z variant.
                 * z_update_en = word1 bit 5, z_compare_en = word1 bit 4. */
                if (rdp_id == 0x2f)
                {
                    int zc = (int)((w1 >> 4) & 1);
                    int zu = (int)((w1 >> 5) & 1);
                    s_zbuffered = (zc || zu) ? 1 : 0;
                }
                /* sniff Set Scissor (0xED -> 0x2d) for the S2DEX background
                 * renderer, mirroring the microcode's command splitter. */
                if (rdp_id == 0x2d) {
                    s2dex_set_scissor(w0, w1);
                    gsp->scis_w0=w0;gsp->scis_w1=w1;gsp->scis_valid=1;
                }
                /* The S2DEX2 rectangle handler restores the display
                 * list's scissor before every G_TEXRECT it forwards. */
                if ((s_ucode_class & 0xff) == UCODE_S2DEX2 &&
                    (rdp_id == 0x24 || rdp_id == 0x25))
                    s2dex_emit_scissor(fifo);
                /* sniff Set Tile (0xF5 -> 0x35) for the wrap masks: the
                 * texture-coordinate wrap fold in gsp_triangle may shift a
                 * triangle's S/T plane only by multiples of the tile's mask
                 * period, so it needs the mask exponents of the tile the
                 * draw will sample. */
                if (rdp_id == 0x35)
                {
                    unsigned int ti = (w1 >> 24) & 7;
                    gsp->tile_mask_t[ti] = (unsigned char)((w1 >> 14) & 0xf);
                    gsp->tile_mask_s[ti] = (unsigned char)((w1 >> 4) & 0xf);
                }
                /* Preserve FullSync's original words and position. Its
                 * ignored payload is still observable in the guest FIFO. */
                if (rdp_id == 0x29)
                    rdp_fifo_fullsync_append(fifo, w0, w1);
                if (rdp_id >= 0x24 && rdp_id <= 0x3f &&
                    rdp_id != 0x31 && rdp_id != 0x29)
                {
                    int32_t two[2];
                    two[0] = (int32_t)w0;
                    /* SET_COLOR_IMAGE (0x3f), SET_Z_IMAGE (0x3e) and
                     * SET_TEXTURE_IMAGE (0x3d): w1 is the image DRAM pointer
                     * and may be a segmented address (segment id in the top
                     * byte); the format/size/width fields all live in w0.
                     * The RSP resolves the segment before the command reaches
                     * the RDP, so resolve fully here -- keeping the segment id
                     * byte only works for identity-mapped segments. */
                    if (rdp_id == 0x3f || rdp_id == 0x3e || rdp_id == 0x3d)
                        two[1] = (int32_t)seg_addr_rsp(w1);
                    else
                        two[1] = (int32_t)w1;
                    rdp_fifo_append(fifo, two, 2);
                }
            }
            break;
        }
    }


}

int f3dex2_run_dl_streaming(GSPState *gsp, RdpFifo *fifo,
                            unsigned int addr, int resume)
{
    s_streaming = 1;
    s_stream_stalled = 0;
    if (resume && s_stream_active)
    {
        /* continue the suspended walk: pc from the saved stall point,
         * DL stack kept as saved */
        s_stream_resume = 1;
        f3dex2_run_dl(gsp, fifo, s_stream_pc, 0, 0);
    }
    else
    {
        s_stream_active = 1;
        f3dex2_run_dl(gsp, fifo, addr, 0, 0);
    }
    s_streaming = 0;
    if (!s_stream_stalled)
        s_stream_active = 0;
    return s_stream_stalled;
}

#include "rdp_emit_state.h"
#define WALKER_FIELDS(X) X(s_seg_table) X(s_half1) X(s_ucode_class) X(s_variant_cbfd) X(s_variant_acclaim) X(s_textured) X(s_zbuffered) X(s_othermode_h) X(s_othermode_l) X(s_dl_stack) X(s_dl_sp) X(s_streaming) X(s_stream_resume) X(s_stream_active) X(s_stream_stalled) X(s_stream_pc) X(s_dl_has_fullsync)
EMIT_STATE_DEFINE(f3dex2, WALKER_FIELDS)
#undef WALKER_FIELDS
