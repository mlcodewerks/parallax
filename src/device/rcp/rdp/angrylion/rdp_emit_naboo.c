#include "../../rsp/hle/hle_timing.h"
/* Factor 5 / LucasArts Naboo-era microcode walker (Star Wars Episode I:
 * Battle for Naboo, Indiana Jones and the Infernal Machine) for the
 * angrylion HLE path.
 *
 * A later revision of the Rogue Squadron engine (see rdp_emit_rs.c): a
 * persistent streaming command server with the same libultra yield
 * protocol, feeding the RDP through a DMEM FIFO (0xe80) spilled to an
 * RDRAM ring driven directly into the DPC registers.  Grammar decoded
 * from the task text (BfN text sum 0x25c16, IJ 0x25c53; dispatch at
 * text 0x2c):
 *
 *   - w0 bits 30-31 select the command class; the handler table lives
 *     in the ucode data segment at +0xb6, halfword-indexed by
 *     (w0 >> 23) & 0x1fe, each entry overlay-id (bits 12-15) :
 *     IMEM offset (bits 0-11); overlays are DMA'd on demand from the
 *     task ucode blob (loader at text 0x15c, current-overlay id at
 *     DMEM 0x143);
 *   - class 3 (0xc0-0xff) passes through to the RDP verbatim
 *     (text 0x98); G_TEXRECT (0xe4) carries its two extra words
 *     inline (text 0xb4);
 *   - the custom class-0 set (0x00-0x1f) is aliased by GBI-numbered
 *     opcodes 0xa9-0xc8 through the same table (0xb8 EndDL = slot
 *     0x0f, 0xba SetOtherModeH = slot 0x11, ...), the same compaction
 *     Rogue Squadron uses;
 *   - triangles reject on the AND of per-vertex outcodes (struct+0x24)
 *     masked 0x7070, the Rogue Squadron outcode model.
 *
 * This walker is intentionally incremental: commands it does not yet
 * implement return NABOO_R_FALLBACK and the caller reruns the slice on
 * the LLE fallback, so titles stay fully playable while the command
 * set is filled in against cxd4 A/B references. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdp_emit_f3dex2.h"
#include "rdp_emit_naboo.h"

extern void rdp_fifo_append(RdpFifo *f, const int32_t *words, int count);

static unsigned char *s_rdram;
static unsigned int   s_rdram_size;
static unsigned int s_clock;
typedef struct NbWrite {
    struct NbWrite *next;
    unsigned int address, bytes;
    unsigned char old[1];
} NbWrite;
static NbWrite *s_writes;
static int s_record_writes;
/* The particle phase index is scalar t6 in the native program. Keep its
 * transient view outside the walker snapshot; the RSP scalar snapshot owns
 * persistence, including transitions through audio tasks. */
static unsigned int nb_phase,nb_phase_saved;
static int nb_phase_written,nb_phase_written_saved;
static unsigned int nb_size_fraction,nb_size_fraction_saved;
static int nb_size_fraction_written,nb_size_fraction_written_saved;
static unsigned int nb_retained[6],nb_retained_saved[6];
static unsigned int nb_retained_written,nb_retained_written_saved;
static int nb_dmem_s16(unsigned int off);
void naboo_set_size_fraction(unsigned int fraction){nb_size_fraction=fraction;nb_size_fraction_written=0;}
int naboo_get_size_fraction(unsigned int *fraction){*fraction=nb_size_fraction;return nb_size_fraction_written;}
static void nb_size_fraction_set(unsigned int fraction){nb_size_fraction=fraction;nb_size_fraction_written=1;}
void naboo_set_context(const unsigned int context[7])
{
    naboo_set_size_fraction(context[0]);memcpy(nb_retained,context+1,sizeof(nb_retained));nb_retained_written=0;
}
unsigned int naboo_get_context(unsigned int context[7])
{
    context[0]=nb_size_fraction;memcpy(context+1,nb_retained,sizeof(nb_retained));
    return (nb_retained_written<<1)|(nb_size_fraction_written?1u:0u);
}
static void nb_retain(unsigned int reg,const short lanes[4])
{
    unsigned int i;
    for(i=0;i<2;i++)nb_retained[reg*2+i]=(unsigned short)lanes[i*2]|((unsigned int)(unsigned short)lanes[i*2+1]<<16);
    nb_retained_written|=3u<<(reg*2);
}
static void nb_retain_dmem(unsigned int reg,unsigned int address)
{
    short lanes[4];unsigned int i;
    for(i=0;i<4;i++)lanes[i]=(short)nb_dmem_s16(address+i*2);
    nb_retain(reg,lanes);
}
void naboo_set_phase(unsigned int phase){nb_phase=phase;nb_phase_written=0;}
int naboo_get_phase(unsigned int *phase){*phase=nb_phase;return nb_phase_written;}
static void nb_phase_set(unsigned int phase){nb_phase=phase;nb_phase_written=1;}
void naboo_set_clock(unsigned int clock) { s_clock = clock; }
void naboo_end_writes(int commit)
{
    while (s_writes) {
        NbWrite *write = s_writes;
        unsigned int i;
        if (!commit) for (i = 0; i < write->bytes; i++)
            s_rdram[((write->address + i) & 0x7fffffu) ^ 3u] = write->old[i];
        s_writes = write->next; free(write);
    }
    s_record_writes = 0;
    if(!commit){nb_phase=nb_phase_saved;nb_phase_written=nb_phase_written_saved;}
    if(!commit){nb_size_fraction=nb_size_fraction_saved;nb_size_fraction_written=nb_size_fraction_written_saved;}
    if(!commit){memcpy(nb_retained,nb_retained_saved,sizeof(nb_retained));nb_retained_written=nb_retained_written_saved;}
}
void naboo_begin_writes(void) {
    naboo_end_writes(1);s_record_writes=1;
    nb_phase_saved=nb_phase;nb_phase_written_saved=nb_phase_written;
    nb_size_fraction_saved=nb_size_fraction;nb_size_fraction_written_saved=nb_size_fraction_written;
    memcpy(nb_retained_saved,nb_retained,sizeof(nb_retained));nb_retained_written_saved=nb_retained_written;
}

void naboo_set_rdram(unsigned char *rdram) { s_rdram = rdram; }
void naboo_set_rdram_size(unsigned int size) { s_rdram_size = size; }

static unsigned int nb_read_u32(unsigned int addr)
{
    unsigned int a = addr & 0x7ffffcu;
    if (!s_rdram || a + 4u > s_rdram_size)
        return 0u;
    return ((unsigned int)s_rdram[(a + 0u) ^ 3u] << 24)
         | ((unsigned int)s_rdram[(a + 1u) ^ 3u] << 16)
         | ((unsigned int)s_rdram[(a + 2u) ^ 3u] << 8)
         |  (unsigned int)s_rdram[(a + 3u) ^ 3u];
}

/* Walk state persists across slices of one task (the server yields and
 * resumes); a fresh task launch resets it. */
#define NB_DL_STACK 8
static struct {
    /* Display lists are chained 0x100-byte chunks, each prefixed by an
     * 8-byte header whose first word points at the next chunk (zero =
     * chain end).  The microcode reads them through the DMEM ring at
     * 0x270 (fetch loop text 0x60-0x8c, 0x108-byte refills); the
     * walker models the same chunked cursor over RDRAM directly. */
    unsigned int chunk;         /* current chunk base (RDRAM) */
    unsigned int off;           /* byte offset within chunk (8..0x108) */
    unsigned int dl;            /* current command address (chunk+off) */
    unsigned int active;
    unsigned int sp;            /* DL call depth (slot 0x06 / 0x0f) */
    unsigned int clip_poly[10]; /* clip fan resume state */
    unsigned int clip_n, clip_idx, clip_active;
    unsigned int tri_phase;     /* quad resume: 1 = first triangle
                                   already emitted before a splice */
    unsigned int geom;          /* geometry-mode word (s5): slot 0x0e
                                   ORs w1 in, slot 0x0d ANDs w1
                                   (text 0xa68/0xa70) */
    unsigned int stack[NB_DL_STACK * 2];
    /* modeled microcode DMEM state: MoveWord (slot 0x13) writes land
     * here; render commands consume them (state words at 0x120-0x13c,
     * viewport/live-tail words, ...) */
    unsigned char dmem[0x1000];
} nb;

static void nb_dl_enter(unsigned int addr)
{
    nb.chunk = addr & 0x00fffff8u;
    nb.off = 8u;
    nb.dl = nb.chunk + nb.off;
}

/* Advance the chunked cursor by len bytes, following the chain link in
 * each chunk's header at the 0x108 boundary. */
static void nb_dl_step(unsigned int len)
{
    nb.off += len;
    while (nb.off >= 0x108u) {
        unsigned int rem = nb.off - 0x108u;
        unsigned int next = nb_read_u32(nb.chunk) & 0x00fffff8u;
        nb.chunk = next;
        nb.off = 8u + rem;
        if (next == 0u)
            break;
    }
    nb.dl = nb.chunk + nb.off;
}

unsigned int nb_task_ordinal;
void naboo_task_reset(unsigned int dl)
{
    nb_task_ordinal++;
    nb_dl_enter(dl);
    nb.active = 1;
    nb.sp = 0;
}

/* Seed the DMEM shadow from the live DMEM.  The streaming server's
 * working state (data segment, matrices, live-tail words, stream
 * cursor) persists in physical DMEM across task slices -- the data
 * segment is DMA'd once at server start and never again -- so the
 * live DMEM at (re)launch IS the authoritative state. */
static int nb_emit_on;
static short nb_morph_v28[8], nb_morph_v29[8];

/* Emission gate for native/HLE comparison runs. Both audited builds use
 * the emitter; unsupported overlay/register contexts return to LLE. */
void naboo_set_emit(int on)
{
    /* NABOO_NO_EMIT forces the fallback path on verified builds too:
     * the pixel-exactness instrument (same backend, same pacing --
     * emit vs LLE-rerun differ only by who rendered). */
    static int off = -1;
    if (off < 0)
        off = getenv("NABOO_NO_EMIT") != NULL;
    nb_emit_on = off ? 0 : on;
}

static unsigned int nb_dmem_r32(unsigned int off);
static void nb_ovl1b(unsigned int w1);
static void nb_ovl21(unsigned int w1);
struct rdp_fifo_s;
static int nb_emit_tri(RdpFifo *fifo, unsigned int ra, unsigned int rb,
                       unsigned int rc);
static void nb_dmem_w32(unsigned int off, unsigned int v);
static void nb_dmem_w16(unsigned int off, unsigned int v);
static void nb_dmem_w8(unsigned int off, unsigned int v);
static int nb_walk(RdpFifo *fifo, unsigned int stop_depth, int nested);
static int nb_attribute_callback(RdpFifo *fifo);
static int nb_call_splice(RdpFifo *fifo);

void naboo_seed_dmem(const unsigned char *dmem)
{
    unsigned int i;
    for (i = 0; i < 0x1000u; i++)
        nb.dmem[i ^ 3u] = dmem[i ^ 3u];

    /* HLE intercepts the boot stub before its data DMA. Load constants into
     * the private shadow; physical DMEM stays untouched for native fallback. */
    if (!(nb_dmem_r32(0xfc4u) & 1u)) {
        unsigned int data = nb_dmem_r32(0xfd8u) & 0x00ffffffu;
        unsigned int bytes = nb_dmem_r32(0xfdcu);
        if (bytes > 0xfc0u) bytes = 0xfc0u;
        if (data <= s_rdram_size && bytes <= s_rdram_size - data)
            for (i = 0; i < bytes; i++)
                nb.dmem[i ^ 3u] = s_rdram[(data + i) ^ 3u];
    }

    /* Boot init (overlay at IMEM 0xd60, also the op 0x80 handler),
     * skipped when the task flags carry the resume bit: reset the
     * overlay id and DL depth bytes, install the default attribute
     * routine, zero the live-tail scratch, seed the SET_OTHER_MODES
     * state pair, and clear the geometry mode. */
    if (!(nb_dmem_r32(0xfc4u) & 1u)) {
        nb_dmem_w32(0xfc4u, 0u);
        nb_dmem_w32(0x5b0u, 0u);
        nb_dmem_w32(0x5b4u, 0u);
        nb_dmem_w32(0x16cu, nb_dmem_r32(0xff8u));
        nb_dmem_w32(0x5a0u, nb_dmem_r32(0xff0u));
        nb_dmem_w8(0x142u, 0u);
        nb_dmem_w16(0x152u, (nb_dmem_r32(0x68u)>>16&0xfffu)==0xc70u?0x17b0u:0x17b4u);
        nb_dmem_w32(0x120u, 0xef000000u);
        nb_dmem_w32(0x124u, 0u);
        nb.dmem[0x58au ^ 3u] = 0u;      /* batching countdown */
        nb_dmem_w32(0x11cu, 0u);        /* conditional-insert state */
        nb_dmem_w32(0x58cu, 0u);        /* splice link */
        nb_dmem_w16(0x168u, 0xffu);
        nb.dmem[0x37eu ^ 3u] = 0u;
        nb_dmem_w32(0x110u, nb_dmem_r32(0xfe8u));
        nb_dmem_w32(0x114u, nb_dmem_r32(0xfecu));
        nb_dmem_w32(0x118u, nb_dmem_r32(0xfecu));
        nb.geom = 0;
    }
}

static void nb_watch(unsigned int a, unsigned int n, unsigned int v);

static unsigned int nb_dmem_r32(unsigned int off)
{
    unsigned int i, value = 0;
    for (i = 0; i < 4u; i++) value = (value << 8) | nb.dmem[((off + i) & 0xfffu) ^ 3u];
    return value;
}

static int nb_dmem_s16(unsigned int off)
{
    unsigned int a = off & 0xfffu;
    return (int)(short)(((unsigned int)nb.dmem[a ^ 3u] << 8)
                       | (unsigned int)nb.dmem[((a + 1u) & 0xfffu) ^ 3u]);
}

static void nb_dmem_w16(unsigned int off, unsigned int v)
{
    unsigned int a = off & 0xfffu;
    nb_watch(a, 2u, v);
    nb.dmem[a ^ 3u]        = (unsigned char)(v >> 8);
    nb.dmem[((a + 1u) & 0xfffu) ^ 3u] = (unsigned char)v;
}

static void nb_dmem_w8(unsigned int off, unsigned int v)
{
    nb.dmem[(off & 0xfffu) ^ 3u] = (unsigned char)v;
}

static void nb_watch(unsigned int a, unsigned int n, unsigned int v)
{
    static int t = -1;
    if (t < 0) t = getenv("NB_WATCH") != NULL;
    if (t && a < 0x12cu && a + n > 0x128u)
        fprintf(stderr, "[W!] %03x len %u = %08x dl=%06x\n", a, n, v, nb.dl);
}

static void nb_dmem_w32(unsigned int off, unsigned int v)
{
    unsigned int i;
    nb_watch(off & 0xfffu, 4u, v);
    for (i = 0; i < 4u; i++) nb.dmem[((off + i) & 0xfffu) ^ 3u] = (unsigned char)(v >> (24u-i*8u));
}

/* DMA (len bytes) from RDRAM into the DMEM shadow */
static void nb_load(unsigned int dmem_off, unsigned int addr, unsigned int len)
{
    unsigned int i;
    {
        static int t = -1;
        if (t < 0) t = getenv("NB_TRACE") != NULL;
        if (t) fprintf(stderr, "[NBL] dmem=%03x dram=%06x len=%x dl=%06x\n",
                       dmem_off & 0xfffu, addr & 0xffffffu, len, nb.dl);

    }
    dmem_off &= 0xff8u; addr &= 0x7ffff8u; len = (len + 7u) & ~7u;
    hle_timing_add(HLE_WORK_DMA_SETUP, 1);
    hle_timing_add(HLE_WORK_DMA, len / 8u);
    nb_watch(dmem_off & 0xfffu, len, 0x10ad);
    for (i = 0; i < len; i++)
        nb.dmem[((dmem_off + i) & 0xfffu) ^ 3u] =
            (unsigned char)(s_rdram ? s_rdram[((addr + i) & 0x7fffffu) ^ 3u] : 0u);
}

static int nb_store(unsigned int address, unsigned int source, unsigned int bytes)
{
    unsigned int i;
    address &= 0x7ffff8u; source &= 0xff8u; bytes = (bytes + 7u) & ~7u;
    if (!s_rdram || s_rdram_size < 0x800000u) return 0;
    if (s_record_writes) {
        NbWrite *write = (NbWrite *)malloc(sizeof(NbWrite) + bytes);
        if (!write) return 0;
        write->address = address; write->bytes = bytes; write->next = s_writes;
        for (i = 0; i < bytes; i++) write->old[i] = s_rdram[((address+i)&0x7fffffu)^3u];
        s_writes = write;
    }
    for (i = 0; i < bytes; i++) s_rdram[((address+i)&0x7fffffu)^3u] = nb.dmem[((source+i)&0xfffu)^3u];
    hle_timing_add(HLE_WORK_DMA_SETUP, 1); hle_timing_add(HLE_WORK_DMA, bytes / 8u);
    return 1;
}

#include "rdp_emit_rsp.h"

/* RSP 48-bit accumulator helpers (vmudn/vmadh chain, zboss style) */
static int64_t nb_p(int a, int b) { return (int64_t)a * (int64_t)b; }
static int64_t nb_acc48(int64_t acc)
{
    uint64_t bits = (uint64_t)acc & ((((uint64_t)1u) << 48) - 1u);
    return bits & ((uint64_t)1u << 47) ? (int64_t)bits - ((int64_t)1u << 48) : (int64_t)bits;
}
static int nb_acc_low(int64_t acc)
{
    int64_t mid = nb_acc48(acc) >> 16;
    if (mid > 32767) return 0xffff;
    if (mid < -32768) return 0;
    return (int)((uint64_t)acc & 0xffffu);
}
static int nb_acc_mid(int64_t acc)
{
    /* vmadh result register: clamped s16 of acc bits 16..47 */
    int64_t v = nb_acc48(acc) >> 16;
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int)v;
}

/* Vertex transform (text 0x838-0x9ec): translate, 4x4 16.16 matrix,
 * dual guard-band clip codes, reciprocal divide, viewport, into
 * 0x28-byte records at DMEM 0x600.  First pass; calibrated against the
 * cxd4 oracle (goracle) on captured task slices. */

/* Exact vch/vcl pair for the transform's clip tests (text 0x8cc-0x8dc):
 * 8 lanes, VS = position (int then frac pass), VT = per-vertex w
 * broadcast half-wise (element 3h).  Faithful to the RSP select-op
 * semantics including the carry/vce state vcl consumes from vch.
 * Returns the VCC halfword after the vcl (le bits 0-7, ge 8-15). */
static unsigned int nb_vch_vcl(const int16_t si[8], const int16_t sf[8],
                               const int16_t ti[8], const int16_t tf[8])
{
    int16_t sn[8], vce[8], ne[8], le1[8], ge1[8];
    unsigned int vcc = 0u;
    int i;

    /* vch on the integer lanes */
    for (i = 0; i < 8; i++) {
        int16_t vs = si[i], vt = ti[i], vc = vt;
        int cch = (vt == -32768);
        int s = ((vs ^ vt) < 0);
        int16_t eq, le, ge, diff;
        sn[i] = (int16_t)(s ? -1 : 0);
        if (s) vc = (int16_t)~vc;
        vce[i] = (int16_t)((vs == vc) && s);
        if (s && !cch) vc = (int16_t)(vc + 1);
        eq = (int16_t)(((vs == vc) && !cch) || vce[i]);
        diff = (int16_t)(sn[i] | vs);
        ge = (int16_t)(diff >= vt);
        diff = (int16_t)(vc - vs);
        le = (int16_t)(s ? (diff >= 0) : (vt < 0));
        ne[i] = (int16_t)(eq ^ 1);
        le1[i] = le; ge1[i] = ge;
    }
    /* vcl on the fraction lanes, consuming the vch flags */
    for (i = 0; i < 8; i++) {
        uint16_t vb = (uint16_t)sf[i], vc = (uint16_t)tf[i];
        int s = sn[i] != 0;
        int eq = (ne[i] == 0);
        int16_t diff; int uz, lz, gen, len, le, ge;
        if (s) vc = (uint16_t)(-(int16_t)vc);
        diff = (int16_t)(vb - vc);
        uz = (int)(((uint32_t)vb + (uint16_t)tf[i] - 65536u) >> 31);
        lz = (diff == 0);
        gen = lz | uz;
        len = lz & uz;
        gen = gen & (int)vce[i];
        len = len & (int)(vce[i] ^ 1);
        len = len | gen;
        gen = (vb >= vc);
        le = (eq && s)  ? len : (int)le1[i];
        ge = (eq && !s) ? gen : (int)ge1[i];
        if (le) vcc |= 1u << i;
        if (ge) vcc |= 1u << (8 + i);
    }
    return vcc;
}

/* Project one vertex record and compute its clip codes (text
 * 0x8c0-0x924): reads the clip-space position from the record
 * (+0x00 int, +0x08 frac), writes the screen triple (+0x18, z
 * fraction +0x1e), the doubled Newton-stepped reciprocal (+0x20),
 * and the outcode halfword (+0x24, sh flavor -- +0x26 preserved).
 * Factored from the batch transform for the clip overlay's
 * interpolated vertices. */
typedef struct NbFixed { int hi; unsigned int lo; } NbFixed;
static int nb_clamp_s16(int64_t v);
static NbFixed nb_fixed_mul(NbFixed a, NbFixed b)
{
    NbFixed result;
    int64_t acc = ((int64_t)a.lo * b.lo >> 16) + (int64_t)a.hi * b.lo;
    acc += (int64_t)a.lo * b.hi;
    result.lo = (unsigned int)nb_acc_low(acc);
    acc += (int64_t)a.hi * b.hi * 65536;
    result.hi = nb_acc_mid(acc);
    return result;
}
static NbFixed nb_fixed_scale(NbFixed a, unsigned int scale)
{
    NbFixed result;
    int64_t acc = ((int64_t)a.lo * scale >> 16) + (int64_t)a.hi * scale;
    result.lo = (unsigned int)nb_acc_low(acc); result.hi = nb_acc_mid(acc);
    return result;
}
static unsigned int nb_project(unsigned int rec)
{
    NbFixed position[4], scaled[4], reciprocal, product, error;
    unsigned int persp = (unsigned short)nb_dmem_s16(0x14eu);
    unsigned int rbits, t1, t2, oc;
    int16_t si[8], sf[8], ti[8], tf[8];
    int j;
    for (j = 0; j < 4; j++) {
        position[j].hi = nb_dmem_s16(rec + (unsigned int)j * 2u);
        position[j].lo = (unsigned short)nb_dmem_s16(rec + 8u + (unsigned int)j * 2u);
        scaled[j] = nb_fixed_scale(position[j], persp);
        si[j] = si[j+4] = (short)position[j].hi;
        sf[j] = sf[j+4] = (short)position[j].lo;
    }
    for (j = 0; j < 4; j++) {
        ti[j] = ti[j+4] = (short)position[3].hi;
        tf[j] = tf[j+4] = (short)position[3].lo;
    }
    t1 = nb_vch_vcl(si, sf, ti, tf);
    {
        unsigned int gain = (unsigned short)nb_dmem_s16(6u);
        int64_t acc = (int64_t)position[3].lo * gain + (int64_t)position[3].hi * gain * 65536;
        int hi = nb_acc_mid(acc);
        unsigned int lo = (position[3].lo * gain) & 0xffffu;
        for (j = 0; j < 8; j++) { ti[j] = (short)hi; tf[j] = (short)lo; }
    }
    t2 = nb_vch_vcl(si, sf, ti, tf);
    oc = ((t1 & 0x707u) << 4) | (t2 & 0x707u);
    rbits = (unsigned int)rsp_rcp32_dp((int32_t)((unsigned int)(unsigned short)scaled[3].hi * 65536u | scaled[3].lo));
    {
        int64_t acc = (int64_t)(rbits & 0xffffu) * 2 + (int64_t)(short)(rbits >> 16) * 131072;
        reciprocal.hi = nb_acc_mid(acc); reciprocal.lo = (unsigned int)nb_acc_low(acc);
    }
    product = nb_fixed_mul(reciprocal, scaled[3]);
    error.lo = (0u - product.lo) & 0xffffu;
    error.hi = nb_clamp_s16(2 - product.hi - (product.lo != 0));
    reciprocal = nb_fixed_mul(reciprocal, error);
    nb_dmem_w16(rec + 0x20u, (unsigned int)reciprocal.hi);
    nb_dmem_w16(rec + 0x22u, reciprocal.lo);
    if (reciprocal.hi < 0) reciprocal.hi = 0x7fff;
    for (j = 0; j < 3; j++) {
        NbFixed projected = nb_fixed_scale(nb_fixed_mul(position[j], reciprocal), persp);
        int scale = nb_dmem_s16(0x130u + (unsigned int)j * 2u);
        int offset = nb_dmem_s16(0x138u + (unsigned int)j * 2u);
        int64_t acc = (int64_t)offset * 65536 + (int64_t)projected.lo * scale + (int64_t)projected.hi * scale * 65536;
        int screen = nb_acc_mid(acc), lower = nb_dmem_s16(0x50u + (unsigned int)((j & ~1) | 1) * 2u);
        if (screen < lower) screen = lower;
        nb_dmem_w16(rec + 0x18u + (unsigned int)j * 2u, (unsigned int)screen);
        if (j == 2) {
            nb_dmem_w16(rec + 0x1eu, (unsigned int)nb_acc_low(acc));
            if (nb.geom & 0x10000u) {
                NbFixed coefficients, fog;
                unsigned int sum;
                coefficients.hi = nb_dmem_s16(0x160u); coefficients.lo = (unsigned short)nb_dmem_s16(0x162u);
                fog = nb_fixed_mul(projected, coefficients);
                sum = fog.lo + (unsigned short)nb_dmem_s16(0x166u);
                fog.lo = sum & 0xffffu;
                fog.hi = nb_clamp_s16((int64_t)fog.hi + nb_dmem_s16(0x164u) + (sum >> 16));
                acc = (int64_t)fog.lo * nb_dmem_s16(0x168u) + (int64_t)fog.hi * nb_dmem_s16(0x168u) * 65536;
                screen = nb_acc_mid(acc);
                if (screen < 0) screen = 0;
                if (screen >= nb_dmem_s16(0x168u)) screen = nb_dmem_s16(0x168u);
                nb_dmem_w8(rec + 0x13u, (unsigned int)screen);
            }
        }
    }
    return oc;
}

static void nb_xfrm_range(unsigned int count, unsigned int src,
                          unsigned int dst, unsigned int matrix, int translate, int pad_fog)
{
    int tr[4];
    int mi[4][4], mf[4][4];
    int i, j;
    unsigned int v;
    unsigned int prefetch=src+(translate?((count+1u)&~1u)*8u:8u);

    /* translation from the state struct +0x18 (r18 = DMEM 0x110,
     * pinned by the op 0x80 init handler): DMEM 0x128, the MoveWord
     * targets in every frame prologue. */
    for (j = 0; j < 4; j++)
        tr[j] = translate ? nb_dmem_s16(0x128u + (unsigned int)j * 2u) : 0;
    {
        static int t = -1;
        if (t < 0) t = getenv("NB_TRACE") != NULL;
        if (t) fprintf(stderr, "[XFRM] n=%u persp=%04x m30=%04x m31=%04x sc0=%04x of0=%04x tr=%04x %04x %04x %04x in0=%04x %04x %04x\n",
                       count,
                       (unsigned)nb_dmem_s16(0x14e)&0xffff,
                       (unsigned)nb_dmem_s16(0x5d8)&0xffff,
                       (unsigned)nb_dmem_s16(0x5da)&0xffff,
                       (unsigned)nb_dmem_s16(0x130)&0xffff,
                       (unsigned)nb_dmem_s16(0x138)&0xffff,
                       tr[0]&0xffff, tr[1]&0xffff, tr[2]&0xffff, tr[3]&0xffff,
                       (unsigned)nb_dmem_s16(0x170)&0xffff,
                       (unsigned)nb_dmem_s16(0x172)&0xffff,
                       (unsigned)nb_dmem_s16(0x174)&0xffff);
    }

    /* matrix at 0x5c0: 4 int rows then 4 frac rows, 4 lanes each */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            mi[i][j] = nb_dmem_s16(matrix + (unsigned int)(i * 8 + j * 2));
            /* fraction rows are unsigned (vmudn operand) */
            mf[i][j] = (int)(nb_dmem_s16(matrix + 32u + (unsigned int)(i * 8 + j * 2)) & 0xffff);
        }

    nb_size_fraction_set((unsigned short)mf[0][0]|((unsigned int)(unsigned short)mf[0][1]<<16));
    nb_retain_dmem(1,matrix+24u);
    nb_retain_dmem(2,matrix+40u);

    for (v = 0; v < count; v++, src += 8u, dst += 0x28u) {
        int in[3], p[3];
        int ci[4]; unsigned int cf[4];
        int64_t acc;
        for (j = 0; j < 3; j++)
            in[j] = nb_dmem_s16(src + (unsigned int)j * 2u);
        /* vaddc pre-translate: unsigned per-lane add, no clamp */
        for (j = 0; j < 3; j++)
            p[j] = (int)(short)((unsigned short)((unsigned)in[j] + (unsigned)tr[j]));
        for (j = 0; j < 4; j++) {
            acc  = nb_p(mf[0][j], p[0]) + nb_p(mi[0][j], p[0]) * 65536;
            acc += nb_p(mf[1][j], p[1]) + nb_p(mi[1][j], p[1]) * 65536;
            acc += nb_p(mf[2][j], p[2]) + nb_p(mi[2][j], p[2]) * 65536;
            acc += mf[3][j];
            cf[j] = (unsigned int)nb_acc_low(acc);
            acc += (int64_t)mi[3][j] * 65536;
            ci[j] = nb_acc_mid(acc);
        }
        /* store clip-space position (record +0x00 int, +0x08 frac) */
        for (j = 0; j < 4; j++) {
            nb_dmem_w16(dst + (unsigned int)j * 2u, (unsigned int)ci[j] & 0xffffu);
            nb_dmem_w16(dst + 8u + (unsigned int)j * 2u, cf[j]);
        }
        /* projection and clip codes: identical to the standalone
         * record path, so share nb_project rather than carrying a
         * second transcription of text 0x8c0-0x9b8. */
        {
            unsigned int oc = nb_project(dst);
            static unsigned int oc_prev;
            {
                static int toc = -1;
                if (toc < 0) toc = getenv("NB_OC_TRACE") != NULL;
                if (toc) {
                    if (v & 1u)
                        fprintf(stderr, "[OC] sw r9=%03x v0=%08x\n",
                                dst - 0x28u, (oc << 16) | oc_prev);
                    else
                        fprintf(stderr, "[OC] sh r9=%03x v0=%08x\n",
                                dst, oc);
                }
            }
            if (v & 1u)
                nb_dmem_w32(dst + 0x24u, (oc << 16) | oc_prev);
            else
                nb_dmem_w16(dst + 0x24u, oc);
            oc_prev = oc;
        }
    }
    if (pad_fog && (count & 1u) && (nb.geom & 0x10000u)) {
        unsigned char saved[40], alpha;
        unsigned int byte;
        for (byte = 0; byte < 40u; byte++) saved[byte] = nb.dmem[((dst + byte)&0xfffu)^3u];
        /* The RSP computes both lanes even when only one record remains.
         * Its second fog-byte store precedes the count check. */
        nb_xfrm_range(1u, src, dst, matrix, translate, 0);
        alpha = nb.dmem[((dst + 0x13u)&0xfffu)^3u];
        for (byte = 0; byte < 40u; byte++) nb.dmem[((dst + byte)&0xfffu)^3u] = saved[byte];
        nb_dmem_w8(dst + 0x13u, alpha);
    }
    /* The projection pipeline prefetches the next pair before testing the
     * remaining count. An eight-byte-aligned LQV leaves the upper half. */
    if(!(prefetch&15u))nb_retain_dmem(0,prefetch+8u);
    hle_timing_add(HLE_WORK_VERTEX, count);
}
static void nb_xfrm(unsigned int count)
{
    nb_xfrm_range(count ? count : 1u, 0x170u, 0x600u, 0x5c0u, 1, 1);
}
/* Overlay 0x2d expands six-byte indexed XYZ records backwards, preserving
 * each packed record's final halfword before the ordinary transform. */
static int nb_expand_indices(unsigned int count)
{
    unsigned int source = nb_dmem_r32(0x154u), slot;
    if (!count || count > 32u) return 0;
    for (slot = count; slot > 0u; slot--) {
        unsigned int index = (unsigned short)nb_dmem_s16(0x170u + (slot - 1u) * 2u);
        unsigned int address = source + index * 6u, offset = address & 7u;
        unsigned int destination = 0x170u + (slot - 1u) * 8u;
        nb_load(0xf80u, address, 16u);
        nb_dmem_w32(destination, nb_dmem_r32(0xf80u + offset));
        nb_dmem_w16(destination + 4u, (unsigned int)nb_dmem_s16(0xf84u + offset));
    }
    return 1;
}
static void nb_ovl18(unsigned int w0, unsigned int w1)
{
    unsigned int mask = (w1 & 0xffff0000u) | (w0 & 0xffffu);
    unsigned int slot;
    nb_size_fraction_set((unsigned short)nb_dmem_s16(0xe60u)|
        ((unsigned int)(unsigned short)nb_dmem_s16(0xe62u)<<16));
    nb_retain_dmem(1,0xe58u);nb_retain_dmem(2,0xe68u);
    for (slot = 0; slot < 32u; slot++)
        if (mask & (1u << slot)) {
            unsigned int dst=0x600u+slot*40u,j;
            short input[4];unsigned char saved[40],alpha;
            for(j=0;j<4;j++)input[j]=(short)(nb_retained[j/2]>>((j&1)*16u));
            nb_xfrm_range(1u,0x170u+slot*8u,dst,0xe40u,0,0);
            if(!(nb.geom&0x10000u))continue;
            /* A one-record masked transform still projects the retained
             * second input lane and stores its fog byte in the next record.
             * Its other outputs are discarded by the native count branch. */
            dst+=40u;
            for(j=0;j<40;j++)saved[j]=nb.dmem[((dst+j)&0xfffu)^3u];
            for(j=0;j<4;j++){
                unsigned int k;int64_t acc=0;
                for(k=0;k<3;k++){
                    acc+=(int64_t)(unsigned short)nb_dmem_s16(0xe60u+k*8u+j*2u)*input[k];
                    acc+=(int64_t)nb_dmem_s16(0xe40u+k*8u+j*2u)*input[k]*65536;
                }
                acc+=(unsigned short)nb_dmem_s16(0xe78u+j*2u);
                nb_dmem_w16(dst+8u+j*2u,(unsigned int)nb_acc_low(acc));
                acc+=(int64_t)nb_dmem_s16(0xe58u+j*2u)*65536;
                nb_dmem_w16(dst+j*2u,(unsigned int)nb_acc_mid(acc));
            }
            nb_project(dst);alpha=nb.dmem[((dst+0x13u)&0xfffu)^3u];
            for(j=0;j<40;j++)nb.dmem[((dst+j)&0xfffu)^3u]=saved[j];
            nb_dmem_w8(dst+0x13u,alpha);
        }
}

/* Overlay 0x0f (entry 0xcf0, its own IMEM page): the descriptor/setup
 * half of the 0x0f / 0x09 pair -- it configures the 0xda0 work area
 * that the vertex-morphing overlay 0x09 then consumes.  Entirely
 * scalar; a3 = r17 - 8, i.e. the payload is the command itself plus
 * the bytes that follow it in the display list.
 *
 * Three paths, keyed on w1 bit 6 and the one-shot gate at DMEM 0x37e:
 *   bit 6 set          -> mode 2 (0xf40): three state stores + an
 *                         IMEM DMA of the second mode routine.
 *   gate set, bit clear -> short path (0xec0): 8-byte command.
 *   gate clear          -> first-time path: 0x38-byte payload, two
 *                         DMAs, and the 0xd58 vertex-record pointer
 *                         table.
 */
static void nb_dl_fetch(unsigned char *dst, unsigned int len)
{
    unsigned int chunk = nb.chunk, off = nb.off, i;
    for (i = 0; i < len; i++) {
        unsigned int a;
        if (off >= 0x108u) {
            chunk = nb_read_u32(chunk) & 0x00fffff8u;
            off = 8u + (off - 0x108u);
        }
        a = (chunk + off) & 0x7fffffu;
        dst[i] = (unsigned char)((s_rdram && a < s_rdram_size)
                                 ? s_rdram[a ^ 3u] : 0u);
        off++;
    }
}

static int nb_pl_s16(const unsigned char *pl, unsigned int o)
{
    return (int)(short)(((unsigned int)pl[o] << 8) | pl[o + 1u]);
}

static unsigned int nb_pl_u32(const unsigned char *pl, unsigned int o)
{
    return ((unsigned int)pl[o] << 24) | ((unsigned int)pl[o + 1u] << 16)
         | ((unsigned int)pl[o + 2u] << 8) | (unsigned int)pl[o + 3u];
}

static unsigned int nb_ovl0f(unsigned int w1)
{
    unsigned char pl[0x40];
    unsigned int a2 = 0xda0u;
    int indiana=(nb_dmem_s16(0x68u)&0xfffu)==0xc70u;
    int j;

    if (w1 & 0x40u) {
        /* 0xf40: mode 2.  The trailing DMA lands in IMEM, which the
         * walker does not model; the three state stores are the whole
         * DMEM-visible effect. */
        if(!indiana)nb_dmem_w16(0x152u, 0x17b4u);
        nb_dmem_w8(0x37eu, 0u);
        nb_dmem_w8(0x143u, 0x3fu);
        return 0u;
    }

    nb_dl_fetch(pl, sizeof pl);

    if (nb.dmem[0x37eu ^ 3u] != 0u) {
        /* 0xec0: already initialised -- 8-byte command. */
        int r14, r15;
        nb_dmem_w16(0xdb8u, 0u);
        if(!indiana)nb_dmem_w32(0x100u, 0u);
        nb_dmem_w16(0xdbcu, (unsigned int)nb_pl_s16(pl, 2u) & 0xffffu);
        r14 = (int)(signed char)pl[indiana?4:1];
        nb_phase_set((unsigned int)r14);
        r15 = (int)(signed char)pl[indiana?5:4];
        nb_dmem_w32(a2 + 0x28u, (unsigned int)r14 << 24);
        nb_dmem_w32(a2 + 0x20u, ((unsigned int)r14 << 24) | 0xffffu);
        nb_dmem_w32(a2 + 0x24u, (unsigned int)(r14 + r14));
        nb_dmem_w32(a2 + 0x2cu, (unsigned int)r15);
        nb_dmem_w16(a2 + 0x36u, (unsigned int)(r15 + r15) & 0xffffu);
        for (j = 0; j < 4; j++) {
            unsigned int v = (unsigned int)nb_pl_s16(pl, 8u + (unsigned int)j * 2u)
                             & 0xffffu;
            nb_dmem_w16(a2 + 0x42u + (unsigned int)j * 0x10u, v);
            nb_dmem_w16(a2 + 0x4au + (unsigned int)j * 0x10u, v);
        }
        if(!indiana){
            unsigned int v = (unsigned int)nb_pl_s16(pl, 5u) & 0xffffu;
            nb_dmem_w16(a2 + 0x86u, v);
            nb_dmem_w16(a2 + 0x8eu, v);
        }
        if(indiana){
            nb_dmem_w32(0xe50u,0xe7000000u);nb_dmem_w32(0xe54u,0);
            nb_dmem_w32(0xe60u,0xef000000u);nb_dmem_w32(0xe68u,0xfcffffffu);
            nb_dmem_w32(0xe6cu,0xfffe793cu);
        }
        return 8u;
    }

    /* first-time path (0xd08-0xebc), 0x38-byte payload */
    {
        int r9 = nb_pl_s16(pl, 8u), r10 = nb_pl_s16(pl, 10u);
        nb_dmem_w16(a2 + 0x30u, (unsigned int)r9 & 0xffffu);
        nb_dmem_w16(a2 + 0x38u, (unsigned int)r9 & 0xffffu);
        nb_dmem_w16(a2 + 0x34u, (unsigned int)r10 & 0xffffu);
        nb_dmem_w32(a2 + 0x3cu, ((unsigned int)r10 << 16) | 0x100u);
        nb_dmem_w16(a2 + 0x32u, (unsigned int)nb_pl_s16(pl, 0x38u) & 0xffffu);
        nb_dmem_w16(a2 + 0x3au, (unsigned int)nb_pl_s16(pl, 0x3au) & 0xffffu);
        nb_dmem_w32(0x0f8u, nb_pl_u32(pl, 12u));
        nb_dmem_w8(0x0fcu, pl[1]);
        nb_dmem_w8(0x0fdu, pl[0x2bu]);
        /* four halfword quads fanned across the 0x40..0x7f grid */
        for (j = 0; j < 4; j++) {
            unsigned int b = a2 + 0x40u + (unsigned int)j * 0x10u;
            nb_dmem_w16(b + 0u, (unsigned int)nb_pl_s16(pl, 0x10u + (unsigned int)j * 2u) & 0xffffu);
            nb_dmem_w16(b + 8u, (unsigned int)nb_pl_s16(pl, 0x10u + (unsigned int)j * 2u) & 0xffffu);
            nb_dmem_w16(b + 4u, (unsigned int)nb_pl_s16(pl, 0x18u + (unsigned int)j * 2u) & 0xffffu);
            nb_dmem_w16(b + 0xcu, (unsigned int)nb_pl_s16(pl, 0x18u + (unsigned int)j * 2u) & 0xffffu);
            nb_dmem_w16(b + 6u, (unsigned int)nb_pl_s16(pl, 0x20u + (unsigned int)j * 2u) & 0xffffu);
            nb_dmem_w16(b + 0xeu, (unsigned int)nb_pl_s16(pl, 0x20u + (unsigned int)j * 2u) & 0xffffu);
        }
        nb_dmem_w32(a2 + 0xa0u, nb_pl_u32(pl, 0x28u));
        nb_dmem_w32(a2 + 0xa4u, nb_pl_u32(pl, 0x28u));
        nb_dmem_w32(a2 + 0xa8u, nb_pl_u32(pl, 0x2cu));
        nb_dmem_w32(a2 + 0xacu, nb_pl_u32(pl, 0x2cu));
        nb_dmem_w16(a2 + 0x84u, (unsigned int)nb_pl_s16(pl, 0x3cu) & 0xffffu);
        nb_dmem_w16(a2 + 0x8cu, (unsigned int)nb_pl_s16(pl, 0x3eu) & 0xffffu);
        if(indiana){
            nb_dmem_w16(a2+0x86u,(unsigned int)nb_pl_s16(pl,0x36u));
            nb_dmem_w16(a2+0x8eu,(unsigned int)nb_pl_s16(pl,0x36u));
        }
        {
            int v0 = nb_pl_s16(pl, 2u);
            unsigned int v1 = ((unsigned int)(v0 + 0xff)) & 0xff00u;
            nb_dmem_w16(a2 + 0x80u, (unsigned int)v0 & 0xffffu);
            nb_dmem_w16(a2 + 0x88u, v1);
            nb_dmem_w16(a2 + 0x82u, (unsigned int)nb_pl_s16(pl, 0x34u) & 0xffffu);
            nb_dmem_w16(a2 + 0x8au, (unsigned int)(signed char)pl[0x2fu] & 0xffffu);
        }
        nb_dmem_w32(a2 + 0x90u, 0x7fff7fffu);
        nb_dmem_w32(a2 + 0x98u, 0x7fff7fffu);
        nb_dmem_w32(a2 + 0x94u, nb_pl_u32(pl, 0x30u));
        nb_dmem_w32(a2 + 0x9cu, nb_pl_u32(pl, 0x30u));
        /* DMA 0xf0 bytes from (w1 >> 8) into DMEM 0xc68 */
        nb_load(0xc68u, w1 >> 8, 0xf0u);
        nb_dmem_w8(0x37eu, 0xffu);
        /* The second DMA (0xe64-0xe90) reads a descriptor at DMEM
         * 0x7c and lands outside the data page: the oracle sandwich
         * shows no DMEM delta from it on any captured call, so it
         * targets IMEM and is not modeled here. */
        if(!indiana)nb_dmem_w16(0x152u, 0x1cc8u);
        for (j = 0; j <= 0x1c; j++)
            nb_dmem_w16(0xd58u + (unsigned int)j * 2u,
                        0x600u + (unsigned int)j * 0x28u);
        if(indiana){
            nb_dmem_w32(0x100u,0);nb_dmem_w32(0x104u,0xffff0000u);
            nb_dmem_w32(0x108u,0x0000ffffu);nb_dmem_w32(0x10cu,0xffffffffu);
        }
        return 0x38u;
    }
}

/* Overlay 0x09: vertex morphing, consuming the 0xda0 work area set by
 * overlay 0x0f. The frame kernel and indexed transform spans are modeled;
 * its final overlay-12 attribute/list continuation still requires native
 * execution. The dispatcher falls back before starting a morph command.
 *
 * 0xcf0-0xd08: 24 payload bytes (r17-8 .. r17+0x10) are staged to
 * 0xda0, so the command is 0x18 long.
 * 0xd10-0xd50: two DMA passes of 0x28 bytes each.  The source base is
 * the staged word at 0xdb0 (second pass: 0xdb4); the two destinations
 * are the DMEM halfwords at r19+0 and r19+8, and the second source is
 * offset by the halfword at 0xdbc that overlay 0x0f latched.  The
 * pass repeats at destination offset 0x78 when bit 15 of r19 is set
 * (`sll a3, r19, 16` then `bltz`), with the delay slot clearing a3 so
 * it runs at most twice.
 */
/* --- 8-lane RSP vector helpers for the morph overlay ---------------
 * The morph code is the first Naboo overlay that needs a general
 * accumulator across all eight lanes rather than a hand-unrolled
 * chain, so model the accumulator explicitly: acc[l] is 48-bit, the
 * mul ops seed it and the mac ops add into it, and each op's
 * destination reads a different slice through its own clamp. */
typedef struct { int64_t a[8]; } NbAcc;

static void nb_vacc_clear(NbAcc *ac)
{
    int l;
    for (l = 0; l < 8; l++)
        ac->a[l] = 0;
}

/* element selector: RSP broadcast encodings 0/1 = identity, 2-3 = half,
 * 4-7 = quarter, 8-15 = full broadcast of one lane */
static int nb_ve(int e, int l)
{
    if (e < 2) return l;
    if (e < 4) return (l & ~1) | (e & 1);
    if (e < 8) return (l & ~3) | (e & 3);
    return e & 7;
}

static int nb_clamp_s16(int64_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int)v;
}

/* vmudl/vmadl: unsigned x unsigned, product >> 16 */
static void nb_vmudl(NbAcc *ac, const short *vs, const short *vt, int e,
                     int acc_in, short *dst)
{
    int l;
    short saved_s[8], saved_t[8];
    memcpy(saved_s, vs, sizeof(saved_s));
    memcpy(saved_t, vt, sizeof(saved_t));
    vs = saved_s; vt = saved_t;
    for (l = 0; l < 8; l++) {
        int64_t pr = (int64_t)(unsigned short)vs[l]
                   * (int64_t)(unsigned short)vt[nb_ve(e, l)];
        ac->a[l] = (acc_in ? ac->a[l] : 0) + (pr >> 16);
        ac->a[l] = nb_acc48(ac->a[l]);
        if (dst) dst[l] = (short)nb_acc_low(ac->a[l]);
    }
}

/* vmudm/vmadm: signed x unsigned */
static void nb_vmudm(NbAcc *ac, const short *vs, const short *vt, int e,
                     int acc_in, short *dst)
{
    int l;
    short saved_s[8], saved_t[8];
    memcpy(saved_s, vs, sizeof(saved_s));
    memcpy(saved_t, vt, sizeof(saved_t));
    vs = saved_s; vt = saved_t;
    for (l = 0; l < 8; l++) {
        int64_t pr = (int64_t)vs[l]
                   * (int64_t)(unsigned short)vt[nb_ve(e, l)];
        ac->a[l] = (acc_in ? ac->a[l] : 0) + pr;
        ac->a[l] = nb_acc48(ac->a[l]);
        if (dst) dst[l] = (short)nb_acc_mid(ac->a[l]);
    }
}

/* vmudn/vmadn: unsigned x signed, destination is the low slice */
static void nb_vmudn(NbAcc *ac, const short *vs, const short *vt, int e,
                     int acc_in, short *dst)
{
    int l;
    short saved_s[8], saved_t[8];
    memcpy(saved_s, vs, sizeof(saved_s));
    memcpy(saved_t, vt, sizeof(saved_t));
    vs = saved_s; vt = saved_t;
    for (l = 0; l < 8; l++) {
        int64_t pr = (int64_t)(unsigned short)vs[l]
                   * (int64_t)vt[nb_ve(e, l)];
        ac->a[l] = (acc_in ? ac->a[l] : 0) + pr;
        ac->a[l] = nb_acc48(ac->a[l]);
        if (dst) dst[l] = (short)nb_acc_low(ac->a[l]);
    }
}

/* vmudh/vmadh: signed x signed one slice up */
static void nb_vmudh(NbAcc *ac, const short *vs, const short *vt, int e,
                     int acc_in, short *dst)
{
    int l;
    short saved_s[8], saved_t[8];
    memcpy(saved_s, vs, sizeof(saved_s));
    memcpy(saved_t, vt, sizeof(saved_t));
    vs = saved_s; vt = saved_t;
    for (l = 0; l < 8; l++) {
        int64_t pr = (int64_t)vs[l] * (int64_t)vt[nb_ve(e, l)];
        ac->a[l] = (acc_in ? ac->a[l] : 0) + (pr * 65536);
        ac->a[l] = nb_acc48(ac->a[l]);
        if (dst) dst[l] = (short)nb_acc_mid(ac->a[l]);
    }
}

/* vmulf/vmacf: signed fractional, x2 with a rounding bias */
static void nb_vmulf8(NbAcc *ac, const short *vs, const short *vt, int e,
                     int acc_in, short *dst)
{
    int l;
    short saved_s[8], saved_t[8];
    memcpy(saved_s, vs, sizeof(saved_s));
    memcpy(saved_t, vt, sizeof(saved_t));
    vs = saved_s; vt = saved_t;
    for (l = 0; l < 8; l++) {
        /* vmulf seeds the accumulator with the rounding bias; vmacf
         * only accumulates the doubled product. */
        int64_t pr = (int64_t)vs[l] * (int64_t)vt[nb_ve(e, l)] * 2;
        ac->a[l] = acc_in ? (ac->a[l] + pr) : (pr + 0x8000);
        ac->a[l] = nb_acc48(ac->a[l]);
        if (dst) dst[l] = (short)nb_acc_mid(ac->a[l]);
    }
}

/* packed vector loads/stores: lpv scales each byte up by 8, luv by 7,
 * and suv is luv's inverse. */
static void nb_lpv_bytes(short *d, const unsigned char *first4,
                         unsigned int rest)
{
    int k;
    for (k = 0; k < 4; k++)
        d[k] = (short)((unsigned short)first4[k] << 8);
    for (k = 0; k < 4; k++)
        d[k + 4] = (short)((unsigned short)nb.dmem[(rest + (unsigned)k) ^ 3u]
                           << 8);
}

static void nb_lpv(short *dst, unsigned int off)
{
    int l;
    for (l = 0; l < 8; l++)
        dst[l] = (short)((int)(signed char)nb.dmem[((off + (unsigned int)l)
                                                    & 0xfffu) ^ 3u] << 8);
}

static void nb_luv(short *dst, unsigned int off)
{
    int l;
    for (l = 0; l < 8; l++)
        dst[l] = (short)((int)nb.dmem[((off + (unsigned int)l)
                                       & 0xfffu) ^ 3u] << 7);
}

static void nb_suv(const short *src, unsigned int off)
{
    int l;
    for (l = 0; l < 8; l++)
        nb.dmem[((off + (unsigned int)l) & 0xfffu) ^ 3u] =
            (unsigned char)((src[l] >> 7) & 0xffu);
}

static void nb_vcopy(short *d, const short *s)
{
    int l;
    for (l = 0; l < 8; l++)
        d[l] = s[l];
}

/* The morph loop proper (text 0x0d54-0x0f40).  Runs once per group of
 * four vertices, blending two keyframes into the 0x170/0x1c0 vertex
 * input array and the two colour arrays, after which the walker's
 * ordinary nb_xfrm path transforms the morphed vertices -- the tail's
 * `j 0x840` is that same resident transform, so it is not repeated
 * here.
 *
 * The inner fetch/splice runs twice, using the delay-slot update of r11
 * to select the second keyframe pair.
 */
/* A single billboard consumes the saved low two size-fraction lanes. */
static int nb_sprite15_packet(unsigned int w0, unsigned int fraction,
                              unsigned int words[10], unsigned int *size)
{
    short v[32][8] = {{0}};
    NbAcc acc;
    unsigned int record = 0x600u + ((w0 >> 5) & 0x7f8u), lane;
    int swap = w0 & 1u, left[2], right[2], slope[2], origin[2];
    int64_t value;
    for (lane = 0; lane < 8u; lane++) {
        v[30][lane] = (short)nb_dmem_s16(0x30u + lane * 2u);
        v[31][lane] = (short)nb_dmem_s16(0x40u + lane * 2u);
        v[12][lane] = (short)(fraction>>(lane==1u?16u:0u));
    }
    v[2][0] = (short)nb_dmem_s16(record + 0x20u);
    v[3][0] = (short)nb_dmem_s16(record + 0x22u); v[3][2] = 128;
    v[10][0] = (short)nb_dmem_s16(0x14eu);
    for (lane = 0; lane < 2u; lane++) {
        unsigned int shift = (1u-lane)*16u;
        v[8][lane] = (short)(nb_read_u32(nb.dl+12u) >> shift);
        v[11][lane] = (short)(nb_read_u32(nb.dl+16u) >> shift);
        v[13][lane] = (short)nb_dmem_s16(record+0x18u+lane*2u);
        v[14][lane] = (short)nb_dmem_s16(0x130u+lane*2u);
        v[15][lane] = (short)(nb_read_u32(nb.dl+20u) >> shift);
        v[16][lane] = (short)(nb_read_u32(nb.dl+24u) >> shift);
    }
    nb_vmudm(&acc,v[14],v[31],13,0,v[14]);
    if (swap) { short t=v[8][0];v[8][0]=v[8][1];v[8][1]=t; }
    nb_vmudm(&acc,v[8],v[3],8,0,v[4]);
    nb_vmudn(&acc,v[8],v[30],8,1,v[9]);
    nb_vmudh(&acc,v[8],v[2],8,1,v[8]);
    nb_vmudl(&acc,v[9],v[10],8,0,v[9]);
    nb_vmudm(&acc,v[8],v[10],8,1,v[8]);
    nb_vmudn(&acc,v[9],v[30],8,1,v[9]);
    nb_vmudn(&acc,v[9],v[14],0,0,v[9]);
    nb_vmudh(&acc,v[8],v[14],0,1,v[8]);
    nb_vmudl(&acc,v[9],v[16],0,0,v[9]);
    nb_vmudm(&acc,v[8],v[16],0,1,v[9]);
    nb_vmudn(&acc,v[9],v[15],0,1,v[9]);
    nb_vmudh(&acc,v[8],v[15],0,1,v[8]);
    for (lane=0;lane<2u;lane++) {
        int raw=(short)(v[13][lane]-v[8][lane]);
        int end=(short)(v[13][lane]+v[8][lane]);
        v[4][lane]=(short)(raw<0?raw:0);
        left[lane]=raw<0?0:raw;right[lane]=end<0?0:end;
    }
    if (swap) { short t=v[8][0];v[8][0]=v[8][1];v[8][1]=t; }
    for (lane=0;lane<2u;lane++) {
        unsigned int reciprocal=(unsigned int)rsp_rcp32_dp(v[8][lane]);
        v[9][lane]=(short)reciprocal;v[8][lane]=(short)(reciprocal>>16);
    }
    nb_vmudl(&acc,v[9],v[3],10,0,v[9]);
    nb_vmudm(&acc,v[8],v[3],10,1,v[8]);
    nb_vmudn(&acc,v[9],v[30],8,1,v[9]);
    nb_vmudl(&acc,v[12],v[9],0,0,v[12]);
    nb_vmudm(&acc,v[11],v[9],0,1,v[12]);
    nb_vmudn(&acc,v[12],v[8],0,1,v[12]);
    nb_vmudh(&acc,v[11],v[8],0,1,v[11]);
    for (lane=0;lane<2u;lane++) {
        unsigned int flip=w0 & (2u<<lane);
        slope[lane]=flip?-v[11][lane]:v[11][lane];
        origin[lane]=flip?(unsigned short)(nb_read_u32(nb.dl+16u)>>((1u-lane)*16u)):0;
        value=-(int64_t)slope[lane]*64;
        v[12][lane]=(short)value;v[11][lane]=(short)(value>>16);
    }
    if (swap) { short t=v[4][0];v[4][0]=v[4][1];v[4][1]=t; }
    nb_vmudn(&acc,v[12],v[4],0,0,v[12]);
    nb_vmudh(&acc,v[11],v[4],0,1,v[11]);
    nb_size_fraction_set((unsigned short)v[12][0]|((unsigned int)(unsigned short)v[12][1]<<16));
    if (right[0]<=left[0] || right[1]<=left[1]) { *size=0;return 1; }
    for (lane=0;lane<2u;lane++) origin[lane]+=v[11][lane]*8-16;
    words[0]=0x2e000000u;
    words[1]=(unsigned int)(unsigned short)((int32_t)nb_dmem_r32(record+0x1cu)>>11)<<16;
    words[2]=0x3a000000u;words[3]=nb_read_u32(nb.dl+28u);
    *size=4;
    if (nb.geom&0x10000u) {
        words[(*size)++]=0x38000000u;
        words[(*size)++]=(nb_read_u32(nb.dl+8u)&0xffffff00u)|nb.dmem[((record+0x13u)&0xfffu)^3u];
    }
    words[(*size)++]=((unsigned int)right[0]<<12)|(unsigned int)right[1];
    words[(*size)++]=((unsigned int)left[0]<<12)|(unsigned int)left[1];
    words[*size-2]=(words[*size-2]&0x00ffffffu)|((0x24u|(unsigned int)swap)<<24);
    words[*size-1]=(words[*size-1]&0x00ffffffu)|((nb.dmem[0x14au^3u]&7u)<<24);
    words[(*size)++]=((unsigned int)(unsigned short)origin[0]<<16)|(unsigned short)origin[1];
    words[(*size)++]=((unsigned int)(unsigned short)slope[0]<<16)|(unsigned short)slope[1];
    return 1;
}
static int nb_ovl15(RdpFifo *fifo,unsigned int w0)
{
    unsigned int record=0x600u+((w0>>5)&0x7f8u),low[10],nl,words[10],count,k;
    if (nb_dmem_s16(record+0x24u)&0x4343u) return 1;
    if (!nb_sprite15_packet(w0,nb_size_fraction,low,&nl)) return 0;
    if(!nl)return 1;
    /* Coordinate and texture setup precede the splice; depth, fog enable,
     * fog alpha and tile selection are read after the child list returns. */
    if(nb_call_splice(fifo)<0)return 0;
    words[0]=0x2e000000u;
    words[1]=(unsigned int)(unsigned short)((int32_t)nb_dmem_r32(record+0x1cu)>>11)<<16;
    words[2]=0x3a000000u;words[3]=low[3];count=4;
    if(nb.geom&0x10000u){
        words[count++]=0x38000000u;
        words[count++]=(nb_read_u32(nb.dl+8u)&0xffffff00u)|nb.dmem[((record+0x13u)&0xfffu)^3u];
    }
    for(k=0;k<4;k++)words[count+k]=low[nl-4u+k];
    words[count+1u]=(words[count+1u]&0x00ffffffu)|((nb.dmem[0x14au^3u]&7u)<<24);
    rdp_fifo_append(fifo,(const int32_t*)words,(int)(count+4u));
    return 1;
}

/* Batched billboards carry the known transform/multiply fraction from one
 * packet into the next, including intermediate accumulator latches. */
static int nb_sprite24_packet(unsigned int record, unsigned int position,
                               unsigned int fraction, unsigned int words[8],
                               unsigned int *size, unsigned int *next_fraction)
{
    short v[32][8] = {{0}};
    NbAcc acc;
    unsigned int lane;
    int left[2], right[2], slope[2];
    for (lane=0;lane<8u;lane++) {
        v[30][lane]=(short)nb_dmem_s16(0x30u+lane*2u);
        v[31][lane]=(short)nb_dmem_s16(0x40u+lane*2u);
        v[9][lane]=(short)(fraction>>(lane==1u?16u:0u));
    }
    *next_fraction=fraction;
    v[2][0]=(short)nb_dmem_s16(record+0x20u);
    v[3][0]=(short)nb_dmem_s16(record+0x22u);v[3][2]=128;
    v[7][0]=(short)nb_dmem_s16(0x14eu);
    for (lane=0;lane<2u;lane++) {
        unsigned int shift=(1u-lane)*16u;
        v[5][lane]=(short)(nb_read_u32(nb.dl+28u)>>shift);
        v[8][lane]=(short)(nb_read_u32(nb.dl+24u)>>shift);
        v[10][lane]=(short)nb_dmem_s16(record+0x18u+lane*2u);
        v[11][lane]=(short)nb_dmem_s16(0x130u+lane*2u);
        v[12][lane]=(short)(nb_read_u32(nb.dl+16u)>>shift);
        v[13][lane]=(short)(nb_read_u32(nb.dl+20u)>>shift);
        v[14][lane]=(short)((nb_read_u32(nb.dl+(lane?4u:0u))>>(lane?16u:0u))&0xffffu);
        v[17][lane]=(short)nb_dmem_s16(0x480u+nb.dmem[((position+6u)&0xfffu)^3u]+lane*2u);
    }
    nb_vmudm(&acc,v[11],v[31],13,0,v[11]);
    nb_vmudm(&acc,v[11],v[14],0,0,v[14]);
    nb_vmudm(&acc,v[5],v[3],8,0,v[4]);
    nb_vmudn(&acc,v[5],v[30],8,1,v[6]);
    nb_vmudh(&acc,v[5],v[2],8,1,v[5]);
    nb_vmudl(&acc,v[6],v[7],8,0,v[6]);
    nb_vmudm(&acc,v[5],v[7],8,1,v[5]);
    nb_vmudn(&acc,v[6],v[30],8,1,v[6]);
    nb_vmudn(&acc,v[6],v[11],0,0,v[6]);
    nb_vmudh(&acc,v[5],v[11],0,1,v[5]);
    nb_vmudl(&acc,v[6],v[13],0,0,v[6]);
    nb_vmudm(&acc,v[5],v[13],0,1,v[6]);
    nb_vmudn(&acc,v[6],v[12],0,1,v[6]);
    nb_vmudh(&acc,v[5],v[12],0,1,v[5]);
    nb_vmudl(&acc,v[6],v[17],0,0,v[6]);
    nb_vmudm(&acc,v[5],v[17],0,1,v[5]);
    nb_vmudn(&acc,v[6],v[30],8,1,v[6]);
    for (lane=0;lane<2u;lane++) {
        int raw,end;
        unsigned int reciprocal;
        if (v[5][lane]>v[14][lane]) v[5][lane]=v[14][lane];
        if (v[5][lane]<v[30][5]) v[5][lane]=v[30][5];
        raw=(short)(v[10][lane]-v[5][lane]);end=(short)(v[10][lane]+v[5][lane]);
        v[4][lane]=(short)(raw<0?raw:0);
        left[lane]=raw<0?0:raw;right[lane]=end<0?0:end;
        reciprocal=(unsigned int)rsp_rcp32_dp(v[5][lane]);
        v[6][lane]=(short)reciprocal;v[5][lane]=(short)(reciprocal>>16);
    }
    if (right[0]<=left[0] || right[1]<=left[1]) { *size=0;return 1; }
    nb_vmudl(&acc,v[6],v[3],10,0,v[6]);
    nb_vmudm(&acc,v[5],v[3],10,1,v[5]);
    nb_vmudn(&acc,v[6],v[30],8,1,v[6]);
    nb_vmudl(&acc,v[9],v[6],0,0,v[9]);
    nb_vmudm(&acc,v[8],v[6],0,1,v[9]);
    nb_vmudn(&acc,v[9],v[5],0,1,v[9]);
    nb_vmudh(&acc,v[8],v[5],0,1,v[8]);
    for(lane=0;lane<2u;lane++) slope[lane]=v[8][lane];
    nb_vmudh(&acc,v[8],v[31],11,0,v[8]);
    nb_vmudn(&acc,v[30],v[30],8,1,v[9]);
    for(lane=0;lane<8u;lane++) {
        unsigned int borrow=(unsigned short)v[9][lane]!=0;
        v[9][lane]=(short)-(unsigned short)v[9][lane];
        v[8][lane]=nb_clamp_s16(-(int)v[8][lane]-(int)borrow);
    }
    nb_vmudn(&acc,v[9],v[4],0,0,v[9]);
    nb_vmudh(&acc,v[8],v[4],0,1,v[8]);
    for(lane=0;lane<8u;lane++) v[8][lane]=(short)(v[8][lane]-v[30][2]);
    nb_vmudh(&acc,v[8],v[31],10,0,v[8]);
    for(lane=0;lane<8u;lane++) {
        unsigned int color=0x380u+nb.dmem[((position+7u)&0xfffu)^3u]+lane;
        v[16][lane]=(short)((unsigned int)nb.dmem[(color&0xfffu)^3u]<<7);
        v[15][lane]=(short)((nb_read_u32(nb.dl+12u)>>(lane<4u?(3u-lane)*8u:0u)&0xffu)<<7);
    }
    nb_vmulf8(&acc,v[16],v[15],0,0,v[16]);
    words[0]=0x2e000000u;
    words[1]=(unsigned int)(unsigned short)((int32_t)nb_dmem_r32(record+0x1cu)>>11)<<16;
    words[2]=0x3a000000u;words[3]=0;
    for(lane=0;lane<4u;lane++) words[3]|=((unsigned int)(v[16][lane]>>7)&0xffu)<<((3u-lane)*8u);
    words[4]=(((unsigned int)right[0]<<12)|(unsigned int)right[1])&0xffffffu;words[4]|=0x24000000u;
    words[5]=(((unsigned int)left[0]<<12)|(unsigned int)left[1])&0xffffffu;
    words[6]=((unsigned int)(unsigned short)v[8][0]<<16)|(unsigned short)v[8][1];
    words[7]=((unsigned int)(unsigned short)slope[0]<<16)|(unsigned short)slope[1];
    *size=8;
    *next_fraction=(unsigned short)v[9][0]|((unsigned int)(unsigned short)v[9][1]<<16);
    return 1;
}
static int nb_ovl24(RdpFifo *fifo,unsigned int w0)
{
    unsigned int batches=(w0>>16)&0xffu,address=nb_read_u32(nb.dl+8u),batch;
    unsigned int boxes=nb_read_u32(nb.dl+36u)&0xffu;
    if (!batches || nb_call_splice(fifo)<0) return 0;
    for(batch=0;batch<batches;batch++,address+=256u) {
        unsigned int mask=nb_read_u32(nb.dl+32u),slot,box;
        if(!mask) continue;
        nb_load(0x170u,address,256u);
        for(box=0;box<boxes;box++) for(slot=0;slot<32u;slot++) {
            unsigned int axis;
            for(axis=0;axis<3u;axis++) {
                int p=nb_dmem_s16(0x170u+slot*8u+axis*2u);
                if(p<nb_dmem_s16(0xc28u+box*16u+axis*2u) ||
                   p>=nb_dmem_s16(0xc30u+box*16u+axis*2u)) break;
            }
            if(axis==3u) mask&=~(1u<<slot);
        }
        nb_xfrm(32u);
        {
            unsigned int value=nb_read_u32(nb.dl+16u);
            nb_size_fraction_set((value>>16)|((value&0xffffu)<<16));
        }
        /* The transform reloads vector 9 from integer matrix row one.
         * Each billboard then retains its final fraction for the next
         * record; these inputs are known state, not arbitrary endpoints. */
        {
        unsigned int fraction=(unsigned short)nb_dmem_s16(0x5c8u)|
            ((unsigned int)(unsigned short)nb_dmem_s16(0x5cau)<<16);
        for(slot=0;slot<32u;slot++) if((mask&(1u<<slot)) && !(nb_dmem_s16(0x624u+slot*40u)&0x4343u)) {
            unsigned int low[8],nl;
            if(!nb_sprite24_packet(0x600u+slot*40u,0x170u+slot*8u,fraction,low,&nl,&fraction)) return 0;
            if(nl) rdp_fifo_append(fifo,(const int32_t*)low,(int)nl);
        }
        }
    }
    nb_phase_set(address);
    return 1;
}

static void nb_ovl09_morph(unsigned int r8, unsigned int r9,
                           unsigned int r19, unsigned int w1_r20, int colored)
{
    short vg[16][8];        /* v2..v11 grid + scratch */
    short v1[8], v16[8], v17[8], v18[8], v19v[8], v20[8], v21[8];
    short v22[8], v23[8], v24[8], v25[8], v27[8];
    /* v28/v29 carry the previous invocation's lanes: the microcode
     * reads them before writing them on the first iteration, and the
     * RSP's vector file persists across overlay dispatches. */
    short *v28 = nb_morph_v28, *v29 = nb_morph_v29;
    short vz[8];
    unsigned int a2 = 0xda0u;
    unsigned int r12, r13, r14, r15, r22, r23;
    int pass;
    int indiana = (nb_dmem_s16(0x68u) & 0xfffu) == 0xc70u;
    NbAcc ac;
    int l;

    for (l = 0; l < 8; l++) { vz[l] = 0; v1[l] = 0; }
    for (l = 0; l < 8; l++) { v27[l] = 0; v17[l] = 0; v20[l] = 0; }

    r12 = (unsigned int)nb_dmem_s16((r19 & 0xfffu) + 0x20u) & 0xffffu;
    r13 = (unsigned int)nb_dmem_s16((r19 & 0xfffu) + 0x22u) & 0xffffu;
    r14 = (unsigned int)nb_dmem_s16((r19 & 0xfffu) + 0x24u) & 0xffffu;
    r15 = (unsigned int)nb_dmem_s16((r19 & 0xfffu) + 0x26u) & 0xffffu;
    if(!colored)nb_phase_set((unsigned int)(int)(short)r14);

    /* grid: lqv at +0x20 .. +0x90, luv at +0xa0/+0xa8 */
    for (l = 0; l < 8; l++) {
        int k;
        for (k = 0; k < 8; k++)
            vg[2 + l][k] = (short)nb_dmem_s16(a2 + 0x20u
                                              + (unsigned int)l * 0x10u
                                              + (unsigned int)k * 2u);
    }
    nb_luv(vg[10], a2 + 0xa0u);
    nb_luv(vg[11], a2 + 0xa8u);

    r22 = nb.dmem[0xdb0u ^ 3u];
    r23 = nb.dmem[0xdb4u ^ 3u];

    /* 0x0d94-0x0da0: the overlay patches a staged command word with
     * (r20 + v2[0]) and lpv's the result back as the seed for the
     * position accumulator v1.
     *
     * The patch lands at r17 - 24, which is in the display-list
     * STAGING RING, not in this work area -- the two are different
     * buffers in DMEM and only the walker had them aliased.  Writing
     * the patched word over 0xda0 left it there for the next reader,
     * and op 0xbe takes its record base from 0xda2, the high half of
     * that same word: it resolved through the 0xd58 table with a base
     * of 0x2209 instead of 0x8cb8 and landed on nonsense.  Keep the
     * patched word in a scratch copy so the staged command survives
     * intact. */
    {
        unsigned int w = (unsigned int)((int)w1_r20 + (int)vg[2][0]);
        unsigned char pw[4];
        pw[0] = (unsigned char)(w >> 24); pw[1] = (unsigned char)(w >> 16);
        pw[2] = (unsigned char)(w >> 8);  pw[3] = (unsigned char)w;
        nb_lpv_bytes(v1, pw, a2 + 4u);
    }

    /* 0x0dac-0x0dd8: seed the running parameter v13:v12 and its step
     * v15:v14 from the lpv'd v1 and the grid.  Vector v0 is zero here
     * (`vsubc v0, v0, v0[e0]` at 0x0d64) -- the `mfc2 v0` above wrote
     * the SCALAR v0, which the disassembler prints identically. */
    {
        short v27s[8];
        nb_vacc_clear(&ac);
        nb_vmudm(&ac, v1, vg[3], 15, 0, v27s);
        /* `vsubc v14, v0, v3[e0]` then `vsub v15, v27, v0[e0]`: the
         * vsub consumes the vsubc's per-lane BORROW, so v15 is one
         * less wherever v3 was non-zero.  Dropping that borrow costs
         * exactly one LSB per lane here and four after the x4 below,
         * which is what put v15 uniformly high. */
        for (l = 0; l < 8; l++) {
            unsigned int sub = (unsigned int)(unsigned short)vg[3][nb_ve(0, l)];
            int borrow = (sub > 0u) ? 1 : 0;
            vg[14][l] = (short)(0u - sub);
            vg[15][l] = (short)nb_clamp_s16(v27s[l] - borrow);
        }
        nb_vacc_clear(&ac);
        nb_vmudl(&ac, vg[14], vg[14], 0, 0, NULL);
        nb_vmudm(&ac, vg[15], vg[14], 0, 1, NULL);
        nb_vmudn(&ac, vg[14], vg[15], 0, 1, vg[12]);
        nb_vmudh(&ac, vg[15], vg[15], 0, 1, vg[13]);
        nb_vacc_clear(&ac);
        nb_vmudn(&ac, vg[14], vg[2], 11, 0, vg[14]);
        nb_vmudh(&ac, vg[15], vg[2], 11, 1, vg[15]);
        for (l = 0; l < 8; l++)
            vg[15][l] = (short)(vg[15][l] + vg[2][nb_ve(15, l)]);
        /* the source lanes must be read from the PRE-op register: a
         * vector op is not an in-place sequential update, and lane 2
         * feeds lane 3 here. */
        {
            short p12[8], p13[8];
            nb_vcopy(p12, vg[12]);
            nb_vcopy(p13, vg[13]);
            for (l = 0; l < 8; l++) {
                unsigned int sum = (unsigned int)(unsigned short)p12[l]
                                 + (unsigned int)(unsigned short)
                                   p12[nb_ve(6, l)];
                vg[12][l] = (short)(sum & 0xffffu);
                vg[13][l] = (short)nb_clamp_s16(p13[l] + p13[nb_ve(6, l)]
                                    + (int)(sum >> 16));
            }
        }
    }

    for (;;) {
        /* --- weight pairs (0x0ddc-0x0e14) --- */
        for (l = 0; l < 8; l++) {
            int lo = vg[4][l] - (int)(unsigned short)vg[12][nb_ve(4, l)];
            int borrow = (lo < 0) ? 1 : 0;
            v22[l] = (short)(lo & 0xffff);
            v23[l] = (short)nb_clamp_s16(vg[5][l] - vg[13][nb_ve(4, l)] - borrow);
        }
        nb_vacc_clear(&ac);
        nb_vmudl(&ac, v22, vg[6], 0, 0, NULL);
        nb_vmudm(&ac, v23, vg[6], 0, 1, NULL);
        nb_vmudn(&ac, v22, vg[7], 0, 1, v22);
        nb_vmudh(&ac, v23, vg[7], 0, 1, v23);
        for (l = 0; l < 8; l++) {
            int h = v23[l];
            if (h < 0) h = 0;
            if (h >= vg[9][l]) h = vg[9][l];
            v23[l] = (short)h;
            v22[l] = (short)(0x7fff - h);
        }
        nb_vacc_clear(&ac);
        nb_vmudl(&ac, vg[12], vg[8], 10, 0, NULL);
        nb_vmudm(&ac, vg[13], vg[8], 10, 1, NULL);
        nb_vmudn(&ac, vg[12], vg[8], 14, 1, v24);
        nb_vmudh(&ac, vg[13], vg[8], 14, 1, v25);
        nb_vacc_clear(&ac);
        nb_vmulf8(&ac, v25, v25, 0, 0, v25);
        for (l = 0; l < 8; l++)
            v24[l] = (short)(0x7fff - v25[l]);
        if (colored) for (l = 0; l < 8; l++) v18[l] = 0;

        /* --- fetch and splice (0x0e18-0x0e80) --- */
        /* This runs TWICE.  `bgez r11` is evaluated on the OLD r11,
         * so the first pass (r11 = r22, non-negative) always branches
         * back, and the delay slot's `addi r11, r11, 0x8078` leaves
         * r11 = r22 - 32648 for the second.  That looks like a wild
         * pointer until you mask to the 12-bit DMEM space, where
         * -32648 is exactly +0x78 -- the second keyframe set the
         * stage-1 DMA landed.  Verified: r24 goes 0xad8 then 0xb50. */
        for (pass = 0; pass < 2; pass++) {
            unsigned int r11 = (pass == 0)
                             ? r22 : (r22 - 32648u) & 0xfffu;
            unsigned int a0 = r11 & 4u;
            unsigned int s0 = r11 + a0, s1 = r11 - a0;
            unsigned int r24 = r8 + r11, r25 = r9 + r11;
            unsigned int r26 = r12 + s0, r27 = r13 + s1;
            unsigned int r28 = r14 + s0, r29 = r15 + s1;
            if (colored) { nb_luv(v16, r24); nb_luv(v17, r25 - 4u); }
            else { nb_lpv(v16, r24); nb_lpv(v17, r25 - 4u); }
            if (colored) {
                int index0, index1;
                unsigned int bias = nb.dmem[(pass ? 0xfdu : 0xfcu) ^ 3u];
                for (l = 0; l < 8; l++) {
                    unsigned int mask = (unsigned short)nb_dmem_s16(0x30u + (unsigned int)nb_ve(4,l)*2u);
                    v16[l] = (short)(((unsigned short)v16[l] & (unsigned short)vg[2][nb_ve(5,l)]) |
                                    ((unsigned short)v17[l] & mask));
                }
                index1 = (signed char)nb.dmem[((0x380u + ((nb.dmem[((r25 + 3u)&0xfffu)^3u] + bias) & 255u))) ^ 3u];
                /* The index rides byte three of each input color. */
                index0 = (signed char)nb.dmem[(0x380u + ((nb.dmem[((r24+3u)&0xfffu)^3u] + bias) & 255u)) ^ 3u];
                nb_vcopy(v19v, vg[pass ? 3 : 8]);
                v19v[3] = (short)index0; v19v[7] = (short)index1;
                {
                    short ones[8];
                    for (l = 0; l < 8; l++) ones[l] = (short)nb_dmem_s16(0x32u);
                    nb_vmudh(&ac, v18, ones, 0, 0, v18);
                    nb_vmudm(&ac, v19v, v19v, 9, 1, v18);
                    nb_vmudh(&ac, v19v, v19v, 13, 1, v18);
                }
                if (pass) {
                    for (l = 0; l < 8; l++) v18[l] = (short)(v18[l] + vg[8][0]);
                    for (l = 0; l < 8; l++) v27[l] = vg[8][4];
                    nb_vmudl(&ac, v18, v22, indiana ? 5 : 4, 0, v18);
                    nb_vmudl(&ac, v27, v23, indiana ? 5 : 4, 1, v18);
                    nb_vmudl(&ac, v18, v24, 4, 0, v18);
                    nb_vmudl(&ac, vg[8], v25, 4, 1, v18);
                    v1[1] = v18[3]; v1[5] = v18[7];
                    v20[0] = (short)(nb.dmem[((0x4ffu + (unsigned int)index0)&0xfffu)^3u] << (indiana ? 8 : 7));
                    v20[4] = (short)(nb.dmem[((0x4ffu + (unsigned int)index1)&0xfffu)^3u] << (indiana ? 8 : 7));
                    for (l = 0; l < 8; l++) { v27[l] = vg[9][0]; v28[l] = (short)(v27[l] - v20[l]); }
                    if (indiana) {
                        v20[1] = v20[0]; v20[5] = v20[4];
                        v20[0] = (short)(0xffffu - (unsigned short)v20[1]);
                        v20[4] = (short)(0xffffu - (unsigned short)v20[5]);
                        nb_vmudl(&ac, v16, v20, 4, 0, v16);
                        nb_vmudl(&ac, v27, v20, 5, 1, v16);
                    } else {
                        nb_vmulf8(&ac, v16, v28, 4, 0, v16);
                        nb_vmulf8(&ac, v27, v20, 4, 1, v16);
                    }
                }
                nb_vmudl(&ac, v16, v22, 6, 0, v27);
                nb_vmudl(&ac, vg[10], v23, 6, 1, v27);
                nb_vmudl(&ac, v16, v22, 7, 1, v27);
                nb_vmudl(&ac, vg[11], v23, 7, 1, v16);
                if (indiana) {
                    v16[3] = pass ? v20[0] : v23[0];
                    v16[7] = pass ? v20[4] : v23[4];
                    nb_suv(v16, r24 + 240u);
                    for (l = 0; l < 4; l++) v16[l] = v16[l + 4];
                    nb_suv(v16, r25 + 240u);
                } else { nb_vcopy(v28, v29); nb_vcopy(v29, v16); }
                continue;
            }
            nb_luv(v18, r26);
            nb_luv(v19v, r27);
            nb_luv(v20, r28 - 4u);
            nb_luv(v21, r29 - 4u);
            for (l = 0; l < 8; l++) {
                unsigned int mask = (unsigned short)nb_dmem_s16(0x30u + (unsigned int)nb_ve(4,l)*2u);
                unsigned int select = (unsigned short)vg[2][nb_ve(5,l)];
                v16[l] = (short)(((unsigned short)v16[l] & select) | ((unsigned short)v17[l] & mask));
                v19v[l] = (short)(((unsigned short)(v19v[l] + v18[l]) & select) |
                                 ((unsigned short)(v21[l] + v20[l]) & mask));
            }

            nb_vacc_clear(&ac);
            nb_vmudl(&ac, v16, v22, 5, 0, NULL);
            nb_vmudl(&ac, v19v, v23, 5, 1, v16);
            nb_vacc_clear(&ac);
            nb_vmudl(&ac, v16, v22, 6, 0, NULL);
            nb_vmudl(&ac, vg[10], v23, 6, 1, NULL);
            nb_vmudl(&ac, v16, v22, 7, 1, v27);
            nb_vmudl(&ac, vg[11], v23, 7, 1, v17);
            nb_vacc_clear(&ac);
            nb_vmudl(&ac, v16, v24, 4, 0, NULL);
            nb_vmudl(&ac, vg[8], v25, 4, 1, v27);
            v1[1] = v27[nb_ve(11, 1)];
            v1[5] = v27[nb_ve(15, 5)];
            if (indiana) {
                v17[3] = v23[0]; v17[7] = v23[4];
                nb_suv(v17, r24 + 240u);
                for (l = 0; l < 4; l++) v17[l] = v17[l + 4];
                nb_suv(v17, r25 + 240u);
            } else {
                nb_vcopy(v28, v29);
                v17[3] = v27[nb_ve(11, 3)];
                v17[7] = v27[nb_ve(15, 7)];
                nb_vcopy(v29, v17);
            }
        }

        /* --- combine and store (0x0ec4-0x0f28) --- */
        if (!indiana) {
        nb_vacc_clear(&ac);
        nb_vmulf8(&ac, v29, v22, 4, 0, NULL);
        nb_vmulf8(&ac, v28, v23, 4, 1, v27);
        if (colored) {
            nb_vmulf8(&ac, v20, v22, 5, 0, v20);
            nb_vmulf8(&ac, vg[9], v23, 5, 1, v20);
        } else {
            v1[1] = v27[nb_ve(11, 1)];
            v1[5] = v27[nb_ve(15, 5)];
        }
        nb_vacc_clear(&ac);
        nb_vmulf8(&ac, v23, v23, 0, 0, v23);
        v28[3] = v23[nb_ve(8, 3)];
        v28[7] = v23[nb_ve(12, 7)];
        v27[3] = colored ? v20[0] : v23[nb_ve(8, 3)];
        v27[7] = colored ? v20[4] : v23[nb_ve(12, 7)];
        }
        {
            unsigned int r24 = r8 + r22, r25 = r9 + r22;
            unsigned int o = r22 * 2u;
            if (!indiana) {
            nb_suv(v28, r24 + 240u);
            for (l = 0; l < 4; l++) v28[l] = v28[4 + l];
            nb_suv(v28, r25 + 240u);
            nb_suv(v27, r24 + 360u);
            for (l = 0; l < 4; l++) v27[l] = v27[4 + l];
            nb_suv(v27, r25 + 360u);
            }
            for (l = 0; l < 4; l++)
                nb_dmem_w16(0x170u + o + (unsigned int)l * 2u,
                            (unsigned int)v1[l] & 0xffffu);
            for (l = 0; l < 4; l++)
                nb_dmem_w16(0x1c0u + o + (unsigned int)l * 2u,
                            (unsigned int)v1[4 + l] & 0xffffu);
        }

        for (l = 0; l < 8; l++)
            v1[l] = (short)(v1[l] + vg[2][nb_ve(0, l)]);
        /* the loop tail is the same 32-bit add: `vaddc v12, v12, v14`
         * then `vadd v13, v13, v15` consuming its carry. */
        for (l = 0; l < 8; l++) {
            unsigned int sum = (unsigned int)(unsigned short)vg[12][l]
                             + (unsigned int)(unsigned short)vg[14][l];
            vg[12][l] = (short)(sum & 0xffffu);
            vg[13][l] = (short)nb_clamp_s16(vg[13][l] + vg[15][l] + (int)(sum >> 16));
            vg[15][l] = (short)(vg[15][l] + vg[3][nb_ve(11, l)]);
        }
        /* `bne r22, r23, 0xddc` compares the OLD r22 and the delay
         * slot's `addi r22, r22, 4` runs regardless, so the body also
         * executes for r22 == r23 -- nine passes for r23 = 32, not
         * eight.  Testing before the increment drops the last store. */
        if (r22 == r23)
            break;
        r22 += 4u;
        if (r22 > 0x400u)
            break;              /* guard: malformed count */
    }
    (void)vz;
    nb_size_fraction_set((unsigned short)vg[12][0]|((unsigned int)(unsigned short)vg[12][1]<<16));
    nb_retain(0,vg[2]+4);nb_retain(1,vg[11]+4);nb_retain(2,vg[13]+4);
}

/* Overlay 0x27: update 32 indexed particle positions and lifetime state.
 * Both audited builds have the same instruction stream. Each fixed-point
 * stage latches the low slice before the next multiply-add changes it. */
static unsigned int nb_ovl27(unsigned int w0, unsigned int w1)
{
    unsigned int positions = nb_read_u32(nb.dl + 8u);
    unsigned int state = nb_read_u32(nb.dl + 12u);
    unsigned int mask, removed = 0, slot, lane;
    unsigned int flags = nb_read_u32(nb.dl + 20u) & 0xffffu;
    unsigned int step = w0 & 0xffffu;
    short velocity[4];
    nb_load(0xc10u, state, 8u);
    mask = nb_dmem_r32(0xc10u);
    if (!mask) return 8u;
    nb_size_fraction_set((unsigned short)(nb_dmem_s16(0xc00u)+nb_dmem_s16(0xc08u))|
        ((unsigned int)(unsigned short)(nb_dmem_s16(0xc02u)+nb_dmem_s16(0xc0au))<<16));
    for (lane = 0; lane < 4u; lane++) {
        unsigned int word = nb_read_u32(nb.dl + 16u + (lane / 2u) * 4u);
        velocity[lane] = (short)(word >> ((1u - (lane & 1u)) * 16u));
    }
    nb_load(0x170u, positions, 256u);
    for (slot = 0; slot < 32u; slot++) {
        unsigned int pos = 0x170u + slot * 8u, life = 0xb00u + slot * 8u;
        unsigned int phase = nb_phase, retry = 0;
        short original[4];
        int64_t acc[4];
        if (!(mask & (1u << slot))) continue;
        for (;;) {
            for (lane = 0; lane < 4u; lane++) original[lane] = (short)nb_dmem_s16(pos + lane * 2u);
            if (flags & 0x2000u) break;
            {
                int64_t value = (int64_t)nb_dmem_s16(life) * 4096 +
                    (int64_t)nb_dmem_s16(life + 2u) * step;
                value = (((unsigned int)value & 0xffffu) * 16u >> 16) + (int64_t)nb_acc_mid(value) * 16;
                phase = (unsigned int)(int)(short)nb_acc_low(value);
                nb_phase_set(phase);
                nb_dmem_w16(life, phase);
            }
            if ((int32_t)phase < 4096) break;
            nb_dmem_w16(life, 0);
            if (!(flags & 0x4000u)) { removed |= 1u << slot; break; }
            for (lane = 0; lane < 4u; lane++) {
                unsigned int value = lane == 0 ? s_clock : lane == 1 ? s_clock >> 3 : lane == 2 ? s_clock >> 9 : (unsigned short)original[3];
                value &= (unsigned short)nb_dmem_s16(0xc18u + lane * 2u);
                value += (unsigned short)nb_dmem_s16(0xc20u + lane * 2u);
                nb_dmem_w16(pos + lane * 2u, value);
            }
            if (++retry == 64u) { nb.active = 2u; return 0; }
        }
        if (removed & (1u << slot)) continue;
        if (!(flags & 0x2000u)) {
            phase >>= 4;nb_phase_set(phase);
            if (flags & 0x40u) nb_dmem_w8(pos + 6u, phase & 0xfcu);
            if (flags & 0x20u) nb_dmem_w8(pos + 7u, phase & 0xfcu);
        }
        for (lane = 0; lane < 4u; lane++) acc[lane] = (int64_t)original[lane] * 4096 + (int64_t)velocity[lane] * step;
        if (flags & 0x380u) {
            unsigned int index = flags & 0x200u ? slot * 8u : flags & 0x80u ? phase & 0xf8u : nb.dmem[(life + 4u)^3u] * 8u;
            for (lane = 0; lane < 4u; lane++) acc[lane] += (int64_t)nb_dmem_s16(0x380u + index + lane * 2u) * step;
        }
        if (flags & 0x1c00u) {
            unsigned int index = flags & 0x1000u ? slot * 8u : flags & 0x400u ? phase & 0xf8u : nb.dmem[(life + 5u)^3u] * 8u;
            for (lane = 0; lane < 4u; lane++) acc[lane] += (int64_t)nb_dmem_s16(0x480u + index + lane * 2u) * step;
        }
        for (lane = 0; lane < 3u; lane++) {
            int64_t value = (((unsigned int)acc[lane] & 0xffffu) * 16u >> 16) + (int64_t)nb_acc_mid(acc[lane]) * 16;
            unsigned int result = (unsigned int)nb_acc_low(value);
            if (flags & 8u) {
                int lower = nb_dmem_s16(0xc00u + lane * 2u);
                unsigned int period = (unsigned short)nb_dmem_s16(0xc08u + lane * 2u);
                int upper = (short)((unsigned int)lower + period);
                if ((short)result < lower) result = (result + period) & 0xffffu;
                if ((short)result >= upper) result = (result - period) & 0xffffu;
            }
            nb_dmem_w16(pos + lane * 2u, result);
        }
    }
    if (!nb_store(positions, 0x170u, 256u)) { nb.active = 2u; return 0; }
    if (!(flags & 4u)) {
        nb_dmem_w32(0xc10u, mask & ~removed);
        if (!nb_store(state, 0xc10u, 8u) || !nb_store(w1 >> 8, 0xb00u, 256u)) { nb.active = 2u; return 0; }
    }
    return 24u;
}

/* op 0x02 (text 0xd1c): open a segmented stream.  DMA the 8-byte
 * chain header at w1 into DMEM 0xfd8 (next-segment pointer), position
 * = w1 + 8, remaining = w0 & 0x1ff payload bytes. */
static void nb_stream_open(unsigned int base)
{
    nb_load(0xfd8u, base, 8u);
    nb_dmem_w32(0xfdcu, 0x100u);
    nb_dmem_w32(0xfd4u, base + 8u);
}

static void nb_op02(unsigned int w0, unsigned int w1)
{
    {
        static int t = -1;
        if (t < 0) t = getenv("NB_TRACE") != NULL;
        if (t) fprintf(stderr, "[OP02] @%06x w0=%08x w1=%08x\n", nb.dl, w0, w1);
    }
    nb_stream_open(w1 & 0x00ffffffu);
    nb_dmem_w32(0xfdcu, w0 & 0x1ffu);
}

/* op 0x01: segmented data load + subtype dispatch (text 0xc94).
 * Direct when w1's low 24 bits are nonzero; otherwise consume the
 * open stream, chaining to the next segment (pre-fetched header at
 * DMEM 0xfd8) when the payload remainder runs out mid-load. */
static void nb_process_loaded(unsigned int sub, unsigned int w0, unsigned int w1);
static void nb_op01(unsigned int w0, unsigned int w1)
{
    unsigned int len  = (w0 & 0xffu) + 1u;
    unsigned int off  = (w0 >> 8) & 0xfffu;
    unsigned int sub  = (w0 >> 19) & 6u;
    unsigned int addr = w1 & 0x00ffffffu;

    if ((w0 & 0xffu) == 0u) {
        /* count 0: no load, dispatch only (text 0xc98) */
    } else if (addr != 0u) {
        nb_load(off, addr, len);
    } else {
        unsigned int pos  = nb_dmem_r32(0xfd4u) & 0x00ffffffu;
        unsigned int rem  = nb_dmem_r32(0xfdcu);
        if (rem >= len) {
            nb_load(off, pos, len);
            nb_dmem_w32(0xfdcu, rem - len);
            nb_dmem_w32(0xfd4u, pos + len);
        } else {
            /* split: drain this segment, chain, load the rest */
            unsigned int next;
            nb_load(off, pos, rem);
            next = nb_dmem_r32(0xfd8u) & 0x00ffffffu;
            nb_stream_open(next);
            pos = nb_dmem_r32(0xfd4u) & 0x00ffffffu;
            nb_load(off + rem, pos, len - rem);
            nb_dmem_w32(0xfdcu, 0x100u - (len - rem));
            nb_dmem_w32(0xfd4u, pos + (len - rem));
        }
    }

    nb_process_loaded(sub, w0, w1);
}

/* The load command and direct slots 18/19/1a enter the same resident
 * processors. Their mode bits stay in s3 until the processor extracts them. */
static void nb_process_loaded(unsigned int sub, unsigned int w0, unsigned int w1)
{
    switch (sub) {
    case 2:                                     /* vertex transform */
        if ((w0 & 0x400000u) && !nb_expand_indices(w1 >> 24)) { nb.active = 2u; break; }
        if ((w1 >> 24) > 32u) { nb.active = 2u; break; }
        nb_xfrm((w1 >> 24) & 0x7fu);
        break;
    case 4:                                     /* color processor
         * (text 0xbd4-0xc70): in-place top-down expansion of the
         * loaded RGB565 halfwords at 0x480+2n into RGBA words at
         * 0x480+4n, then -- when the count byte's sign OR the
         * intensity byte at 0x58b is negative -- a vmulf per-channel
         * scale by the 8 intensity bytes at DMEM 0x158 into the
         * array at 0xd40.  w0 bits 0/1 dispatch overlays 0x21/0x1b
         * afterwards. */
        {
            int cnt7 = (int)((w1 >> 24) & 0x7fu);
            int neg  = ((w1 >> 24) & 0x80u) != 0u;
            int i2;
            if ((w0 >> 23) & 1u) {
                for (i2 = cnt7 - 1; i2 >= 0; i2--) {
                    unsigned int c565 =
                        ((unsigned int)nb.dmem[(0x480u + (unsigned)i2*2u) ^ 3u] << 8)
                       | (unsigned int)nb.dmem[(0x481u + (unsigned)i2*2u) ^ 3u];
                    unsigned int rgba = ((c565 & 0xf800u) << 16)
                                      | ((c565 & 0x07e0u) << 13)
                                      | ((c565 & 0x001fu) << 11)
                                      | 0xffu;
                    nb_dmem_w32(0x480u + (unsigned)i2 * 4u, rgba);
                }
            }
            {
                static int t = -1;
                if (t < 0) t = getenv("NB_CP_TRACE") != NULL;
                if (t) fprintf(stderr, "[CP] w0=%08x w1=%08x 58b=%02x neg=%d\n",
                               w0, w1, nb.dmem[0x58bu ^ 3u], neg);
            }
            if (neg || (nb.dmem[0x58bu ^ 3u] & 0x80u)) {
                /* intensity scale into 0xd40; per-channel vmulf by the
                 * 8 bytes at 0x158, no overlay dispatch on this path
                 * (text 0xc44-0xc70) */
                int rounded = (cnt7 + 3) & ~3;
                if (!rounded) rounded = 4;
                for (i2 = 0; i2 < rounded; i2++) {
                    unsigned int j2;
                    for (j2 = 0; j2 < 4u; j2++) {
                        int a2 = (int)nb.dmem[(0x480u + (unsigned)i2*4u + j2) ^ 3u] << 7;
                        int b2 = (int)nb.dmem[(0x158u + (j2 + ((unsigned)i2 & 1u) * 4u)) ^ 3u] << 7;
                        int64_t acc = (int64_t)a2 * b2 * 2 + 0x8000;
                        int r2 = (int)(acc >> 16);
                        if (r2 > 32767) r2 = 32767;
                        if (r2 < 0) r2 = 0;
                        nb.dmem[(0xd40u + (unsigned)i2*4u + j2) ^ 3u] =
                            (unsigned char)(r2 >> 7);
                    }
                }
            } else {
                /* The non-scaled path always dispatches a lighting
                 * overlay, selected by original w0 bit 22 (0xc74-0xc84).
                 * NB_SKIP_COLOVL is a validation-only bypass that
                 * leaves the colors unlit so downstream paths can be
                 * verified in isolation. */
                static int skip = -1;
                if (skip < 0) skip = getenv("NB_SKIP_COLOVL") != NULL;
                if (!skip) {
                    /* The resident handler shifts s3 by 22 before
                     * testing its low bit: even -> 1b, odd -> 21. */
                    if ((w0 >> 22) & 1u)
                        nb_ovl21(w1);
                    else
                        nb_ovl1b(w1);
                }
            }
        }
        break;
    case 6:                                     /* intensity byte */
        nb_dmem_w8(0x58bu, w1 >> 24);
        break;
    default:                                    /* load-only */
        break;
    }
}

/* Triangle command (slot 0x0b, 16 or 32 bytes).  Layout:
 *   w0: opcode | flags (bit 9 = inline ST present, bit 11 = overlay
 *       0x2a extended path); low bits carry the record-base constant
 *   w1: vertex record addresses A (hi16) and B (lo16)
 *   +8..+11: face color-list byte offsets (byte +8 = the extra/fog
 *       slot, +9/+10/+11 = per-vertex offsets into the scaled color
 *       array at DMEM 0xd40)
 *   +12: vertex record address C; +14: staged slot for sharing
 *   +16..+31 (when w0 bit 9): packed S/T words, lanes 0/2/4 -> A/B/C
 *
 * Emission side effects modeled here (text 0xa94): when geometry-mode
 * bit 16 is set, each face's fog bytes (record +0x13) propagate into
 * the color array's alpha (0xd40 + off + 3); then the color words are
 * poked into the records at +0x10 and the inline STs at +0x14. */
/* Build an RspTriVtx from a vertex record in the DMEM shadow. */
static void nb_vtx(unsigned int rec, RspTriVtx *v)
{
    v->x = (int16_t)nb_dmem_s16(rec + 0x18u);
    v->y = (int16_t)nb_dmem_s16(rec + 0x1au);
    v->z = (int32_t)((nb_dmem_s16(rec + 0x1cu) << 16)
                     | (nb_dmem_s16(rec + 0x1eu) & 0xffff));
    v->r = nb.dmem[((rec + 0x10u) & 0xfffu) ^ 3u];
    v->g = nb.dmem[((rec + 0x11u) & 0xfffu) ^ 3u];
    v->b = nb.dmem[((rec + 0x12u) & 0xfffu) ^ 3u];
    v->a = nb.dmem[((rec + 0x13u) & 0xfffu) ^ 3u];
    v->s = nb_dmem_s16(rec + 0x14u);
    v->t = nb_dmem_s16(rec + 0x16u);
    v->invw = (int32_t)((nb_dmem_s16(rec + 0x20u) << 16)
                        | (nb_dmem_s16(rec + 0x22u) & 0xffff));
    /* perspNorm'd w = the projection's divide input (clip w at record
     * +6/+14 scaled by the persp halfword), which the shared writer's
     * texture normalizer divides through */
    {
        int64_t w32 = ((int64_t)nb_dmem_s16(rec + 6u) << 16)
                    | (nb_dmem_s16(rec + 0xeu) & 0xffff);
        v->pw = (int32_t)((w32 * (nb_dmem_s16(0x14eu) & 0xffff)) >> 16);
    }
    v->flat2d = 0;
}

/* Emit one triangle from three records: trivial reject on the ANDed
 * outcodes (0x7070), clip trigger on the ORed outcodes (0x4343 ->
 * caller falls back), winding cull against the geometry-mode bit,
 * then the shared RSP-exact edge/attribute writer.  Returns -1 when
 * the clip overlay would run. */


/* vmulf: signed Q15 multiply with rounding and mid clamp */
static int nb_vmulf(int a, int b)
{
    int64_t acc = (int64_t)(int16_t)a * (int16_t)b * 2 + 0x8000;
    int32_t r = (int32_t)(acc >> 16);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return r;
}

/* vmulf into the accumulator, then vmadh += v8<<16, result = clamped
 * mid: the lighting overlays' contribution chain. */
static int nb_mulf_addh(int a, int b, int addend)
{
    int64_t acc = (int64_t)(int16_t)a * (int16_t)b * 2 + 0x8000;
    acc += (int64_t)(int16_t)addend << 16;
    return nb_acc_mid(acc);
}

/* Overlay 0x1b (vertex lighting; even op-0x01 subtypes with w0 bit 0
 * of the subtype field clear dispatch here from the color processor).
 * r20w1 = the color command's w1: vertex count in the top byte.
 * Lights are 0x18-byte records at DMEM 0xb00, counted by the byte at
 * 0x58b: +0x13 type (0 flat, 1 directional with the per-vertex s8
 * normal quads at 0x380 and the 0x5b0/0x5b4 alternate-direction
 * select, else positional with the vertex index riding byte 3 of
 * each normal quad), +0x10 color, +0/+8 direction or position pair,
 * +0x14 the attenuation halfwords.  Contributions accumulate into
 * the color array at 0xd40 scaled by the global bytes at 0x158; the
 * alpha lane always carries the scaled vertex alpha. */
static unsigned int nb_light_attenuation(unsigned int index, unsigned int record, unsigned int flags,int save_fraction)
{
    unsigned int k;
    unsigned int lo[4];
    int hi[4];
    int64_t acc;
    unsigned int fraction=0;
    for (k = 0; k < 4u; k++)
    {
        unsigned int alternate = ((flags >> k) & 1u) ? 8u : 0u;
        int position = k == 3u ? 0 : nb_dmem_s16(0x170u + index + k * 2u);
        int light = nb_dmem_s16(record + alternate + k * 2u);
        int difference = nb_clamp_s16((int64_t)position - light);
        int32_t distance = difference * 256;
        if(k<2u)fraction|=(unsigned int)(unsigned short)distance<<(k*16u);
        acc = ((int64_t)distance * distance) >> 16;
        lo[k] = (unsigned int)acc & 0xffffu;
        hi[k] = nb_acc_mid(acc);
    }
    if(save_fraction)nb_size_fraction_set(fraction);
    /* VADDC/VADD half and quarter folds. Preserve the
     * separate carry into each saturated signed add. */
    for (k = 0; k < 4u; k += 2u)
    {
        unsigned int sum = lo[k] + lo[k + 1u];
        lo[k] = sum & 0xffffu;
        hi[k] = nb_clamp_s16((int64_t)hi[k] + hi[k + 1u] + (sum >> 16));
    }
    {
        unsigned int sum = lo[0] + lo[2];
        lo[0] = sum & 0xffffu;
        hi[0] = nb_clamp_s16((int64_t)hi[0] + hi[2] + (sum >> 16));
    }
    {
        int ch = nb_dmem_s16(record + 0x14u);
        unsigned int cf = (unsigned int)nb_dmem_s16(record + 0x16u) & 0xffffu;
        int fraction, middle;
        acc = ((int64_t)lo[0] * cf >> 16) + (int64_t)hi[0] * cf;
        acc += (int64_t)lo[0] * ch;
        fraction = nb_acc_low(acc);
        acc += (int64_t)hi[0] * ch * 65536;
        middle = nb_acc_mid(acc);
        return middle == -1 ? (unsigned int)fraction : 0u;
    }
}

static void nb_ovl1b(unsigned int w1)
{
    unsigned int end = ((((w1 >> 24) & 0x7fu) << 2) + 4u) & 0xff8u;
    unsigned int nv = end >> 2;
    unsigned int lights = nb.dmem[0x58bu ^ 3u];
    unsigned int li, vi, k;

    /* The SQV clear rounds the active color span to sixteen bytes. */
    for (vi = 0; vi < ((end + 15u) & ~15u); vi++)
        nb.dmem[(0xd40u + vi) ^ 3u] = 0u;
    {
        static int t = -1;
        if (t < 0) t = getenv("NB_DOT_TRACE") != NULL;
        if (t) fprintf(stderr, "[L1B] nv=%u lights=%u types=%02x %02x %02x\n",
                       nv, lights,
                       nb.dmem[(0xb13u) ^ 3u],
                       nb.dmem[(0xb2bu) ^ 3u],
                       nb.dmem[(0xb43u) ^ 3u]);
    }
    if (lights == 0u)
        goto done;

    for (li = 0; li < lights; li++) {
        unsigned int lrec = 0xb00u + li * 0x18u;
        unsigned int type = nb.dmem[(lrec + 0x13u) ^ 3u];
        int lcol[4];
        for (k = 0; k < 3u; k++)
            lcol[k] = (int)nb.dmem[(lrec + 0x10u + k) ^ 3u] << 7;
        lcol[3] = 0;

        if (type == 0u) {
            for (vi = 0; vi < nv; vi++)
                for (k = 0; k < 4u; k++) {
                    unsigned int off = vi * 4u + k;
                    int c = (int)nb.dmem[(0x480u + off) ^ 3u] << 7;
                    int g = (int)nb.dmem[(0x158u + (k + (vi & 1u) * 4u)) ^ 3u] << 7;
                    int v5 = nb_vmulf(c, g);
                    int a8 = (int)nb.dmem[(0xd40u + off) ^ 3u] << 7;
                    int r = (k == 3u) ? v5 : nb_mulf_addh(v5, lcol[k], a8);
                    nb.dmem[(0xd40u + off) ^ 3u] =
                        (unsigned char)(((unsigned)(int16_t)r >> 7) & 0xffu);
                }
            continue;
        }

        if (type == 1u) {
            /* directional: per vertex pair, dot(normal, dir) */
            unsigned int selw0 = nb_dmem_r32(0x5b0u);
            unsigned int selw1 = nb_dmem_r32(0x5b4u);
            unsigned int have_sel = selw0 | selw1;
            if(have_sel)nb_phase_set(0xfff00f00u);
            unsigned int sbits = selw0, scount = 16u;
            for (vi = 0; vi < nv; vi += 2u) {
                int16_t dir[8], nrm[8];
                unsigned int vcc = 0u;
                int dot[2];
                unsigned int pair, lane;
                if (have_sel) {
                    vcc = (0xfff00f00u >> ((sbits & 3u) * 8u)) & 0xffu;
                    sbits >>= 2;
                    if (--scount == 0u) sbits = selw1;
                }
                for (lane = 0; lane < 8u; lane++) {
                    unsigned int base = ((vcc >> lane) & 1u) ? 8u : 0u;
                    dir[lane] = (int16_t)nb_dmem_s16(lrec + base + (lane & 3u) * 2u);
                }
                for (lane = 0; lane < 8u; lane++) {
                    int8_t nb8 = (int8_t)nb.dmem[(0x380u + vi * 4u + lane) ^ 3u];
                    nrm[lane] = (int16_t)((int)nb8 << 8);
                    if ((lane & 3u) == 3u)
                        nrm[lane] = 0;      /* x v14 zeroes lane 3/7 */
                }
                for (pair = 0; pair < 2u; pair++) {
                    int d = 0;
                    for (k = 0; k < 3u; k++) {
                        int m = nb_vmulf(dir[pair * 4u + k], nrm[pair * 4u + k]);
                        d += m;             /* vadd folds, clamped */
                        if (d > 32767) d = 32767;
                        if (d < -32768) d = -32768;
                    }
                    if (d < 0) d = 0;       /* vge 0 */
                    dot[pair] = d;
                }
                {
                    static int t = -1;
                    if (t < 0) t = getenv("NB_DOT_TRACE") != NULL;
                    if (t) fprintf(stderr, "[DOT] %d %d\n", dot[0], dot[1]);
                }
                {
                    int outl[8];
                    for (lane = 0; lane < 8u; lane++) {
                        unsigned int v = vi + (lane >> 2);
                        unsigned int ch = lane & 3u;
                        unsigned int off = v * 4u + ch;
                        int c = (int)nb.dmem[(0x480u + off) ^ 3u] << 7;
                        int g = (int)nb.dmem[(0x158u + (ch + (v & 1u) * 4u)) ^ 3u] << 7;
                        int v5 = nb_vmulf(c, g);
                        int contrib = nb_vmulf(lcol[ch],
                                               (int)(unsigned)dot[lane >> 2]);
                        int a8 = (int)nb.dmem[(0xd40u + off) ^ 3u] << 7;
                        int r = (ch == 3u) ? v5 : nb_mulf_addh(v5, contrib, a8);
                        outl[lane] = r;
                        if (lane < 8u) {
                            static int t3 = -1;
                            static int cbuf[8];
                            if (t3 < 0) t3 = getenv("NB_DOT_TRACE") != NULL;
                            cbuf[lane] = contrib;
                            if (t3 && lane == 7u)
                                fprintf(stderr,
                                    "[CT1] %d %d %d %d %d %d %d %d\n",
                                    cbuf[0],cbuf[1],cbuf[2],cbuf[3],
                                    cbuf[4],cbuf[5],cbuf[6],cbuf[7]);
                        }
                        if (v < nv)
                            nb.dmem[(0xd40u + off) ^ 3u] =
                                (unsigned char)(((unsigned)(int16_t)r >> 7) & 0xffu);
                    }
                    {
                        static int t2 = -1;
                        if (t2 < 0) t2 = getenv("NB_DOT_TRACE") != NULL;
                        if (t2) fprintf(stderr,
                            "[ST1] %d %d %d %d %d %d %d %d\n",
                            outl[0],outl[1],outl[2],outl[3],
                            outl[4],outl[5],outl[6],outl[7]);
                    }
                }
            }
            continue;
        }

        /* positional: index rides byte 3 of the normal quad */
        {
            for (vi = 0; vi < nv; vi++) {
                unsigned int idx = nb.dmem[((0x380u + vi * 4u + 3u) & 0xfffu) ^ 3u];
                unsigned int attn = nb_light_attenuation(idx, lrec, 0u,!(vi&1u));
                for (k = 0; k < 4u; k++) {
                    unsigned int off = vi * 4u + k;
                    int c = (int)nb.dmem[(0x480u + off) ^ 3u] << 7;
                    int g = (int)nb.dmem[(0x158u + (k + (vi & 1u) * 4u)) ^ 3u] << 7;
                    int v5 = nb_vmulf(c, g);
                    int contrib = (int)(((int64_t)lcol[k] * attn) >> 16);
                    int a8 = (int)nb.dmem[(0xd40u + off) ^ 3u] << 7;
                    int r = (k == 3u) ? v5 : nb_mulf_addh(v5, contrib, a8);
                    nb.dmem[(0xd40u + off) ^ 3u] =
                        (unsigned char)(((unsigned)(int16_t)r >> 7) & 0xffu);
                }
            }
        }
        {
            const short ones[4]={-1,-1,-1,-1};
            nb_retain(0,ones);nb_retain_dmem(1,lrec);
        }
    }
done:
    nb_dmem_w32(0x5b0u, 0u);
    nb_dmem_w32(0x5b4u, 0u);
}

/* Overlay 0x21: indexed positions, squared-distance attenuation and
 * flat/additive/modulated color lights. The fraction is latched before
 * the final VMADH, and VEQ retains it only for a signed middle of -1. */
static void nb_ovl21(unsigned int w1)
{
    unsigned int pairs = (((w1 >> 24) & 0x7fu) + 1u) >> 1;
    unsigned int clear = ((pairs + 3u) & ~3u) * 8u;
    unsigned int lights = nb.dmem[0x58bu ^ 3u];
    unsigned int li, vi, k, i;
    if (!clear) clear = 32u;
    for (i = 0; i < clear; i++) nb_dmem_w8(0xd40u + i, 0);
    if (!lights) return;
    if (!pairs) { nb.active = 2u; return; }
    hle_timing_add(HLE_WORK_LIGHT, pairs * lights * 2u);
    for (li = 0; li < lights; li++)
    {
        unsigned int record = 0xb00u + li * 24u;
        unsigned int type = nb.dmem[((record + 0x13u) & 0xfffu) ^ 3u];
        unsigned int select = nb_dmem_r32(0x5b0u), select2 = nb_dmem_r32(0x5b4u);
        unsigned int selected = select | select2, remaining = 16u;
        if(type)nb_phase_set(select2);
        for (vi = 0; vi < pairs; vi++)
        {
            unsigned int flags = 0;
            unsigned int factor[2] = {0, 0};
            if (selected && type)
            {
                flags = (0xfff00f00u >> ((select & 3u) * 8u)) & 0xffu;
                select >>= 2;
                if (--remaining == 0u) select = select2;
            }
            if (type)
            {
                unsigned int p;
                for (p = 0; p < 2u; p++)
                {
                    unsigned int index = nb.dmem[((0x380u + vi * 2u + p) & 0xfffu) ^ 3u];
                    factor[p] = nb_light_attenuation(index, record, flags >> (p * 4u),p==0u);
                }
            }
            for (k = 0; k < 8u; k++)
            {
                unsigned int channel = k & 3u;
                unsigned int offset = vi * 8u + k;
                int source = nb.dmem[((0x480u + offset) & 0xfffu) ^ 3u] << 7;
                int gain = nb.dmem[(0x158u + k) ^ 3u] << 7;
                int scaled = nb_vmulf(source, gain);
                int color = channel == 3u ? 0 : nb.dmem[((record + 0x10u + channel) & 0xfffu) ^ 3u] << 7;
                int old = nb.dmem[((0xd40u + offset) & 0xfffu) ^ 3u] << 7;
                int result;
                if (type) color = (int)(((int64_t)color * factor[k >> 2]) >> 16);
                result = type == 4u ? nb_clamp_s16((int64_t)old + color)
                                   : nb_mulf_addh(scaled, color, old);
                if (channel == 3u) result |= scaled;
                nb_dmem_w8(0xd40u + offset, ((unsigned int)(int16_t)result >> 7) & 0xffu);
            }
        }
        if(type){
            const short ones[4]={-1,-1,-1,-1};
            nb_retain(0,ones);
            if(selected)nb_retain_dmem(1,record);
        }
    }
    nb_dmem_w32(0x5b0u, 0);
    nb_dmem_w32(0x5b4u, 0);
    if (nb_dmem_r32(0x5a8u) != 0xffffffffu)
        for (vi = 0; vi < pairs; vi++)
        {
            short color[8], gain[8];
            NbAcc acc;
            nb_luv(color, 0xd40u + vi * 8u);
            for (k = 0; k < 8u; k++) gain[k] = (short)(nb.dmem[(0x5a8u + (k & 3u)) ^ 3u] << 7);
            nb_vmulf8(&acc, color, gain, 0, 0, color);
            nb_suv(color, 0xd40u + vi * 8u);
        }
}

/* Overlay 2a: four normal transforms, exact square/fold/RSQ normalization,
 * then packed S/T exports. Every fraction is latched at its VMADN store. */
static void nb_ovl2a(unsigned int cmd, unsigned int out_st[4])
{
    short v[32][8]={{0}};
    NbAcc acc;
    unsigned int row,lane,i;
    for(row=0;row<8u;row++) for(lane=0;lane<4u;lane++)
        v[row+1u][lane]=(short)nb_dmem_s16(0xe40u+row*8u+lane*2u);
    for(lane=0;lane<4u;lane++) v[12][lane]=(short)nb_dmem_s16(0xf0u+lane*2u);
    nb_size_fraction_set((unsigned short)v[12][0]|((unsigned int)(unsigned short)v[12][1]<<16));
    for(lane=0;lane<2u;lane++) v[21][lane]=(short)nb_dmem_s16(0xecu+lane*2u);
    for(lane=0;lane<8u;lane++) v[30][lane]=(short)nb_dmem_s16(0x30u+lane*2u);
    v[22][0]=0xab;
    for(i=0;i<4u;i++) {
        unsigned int src=0x170u+(nb_read_u32(cmd+16u+i*4u)&0xffu);
        unsigned int sum,rsq;
        for(lane=0;lane<4u;lane++) v[11][lane]=(short)nb_dmem_s16(src+lane*2u);
        nb_vmudn(&acc,v[5],v[11],4,0,v[9]);
        nb_vmudh(&acc,v[1],v[11],4,1,v[9]);
        nb_vmudn(&acc,v[6],v[11],5,1,v[9]);
        nb_vmudh(&acc,v[2],v[11],5,1,v[9]);
        nb_vmudn(&acc,v[7],v[11],6,1,v[9]);
        nb_vmudh(&acc,v[3],v[11],6,1,v[9]);
        nb_vmudn(&acc,v[8],v[30],9,1,v[9]);
        nb_vmudh(&acc,v[4],v[30],9,1,v[10]);
        for(lane=0;lane<8u;lane++) v[10][lane]=(short)(v[10][lane]-v[12][lane]);
        nb_vmudl(&acc,v[9],v[9],0,0,v[17]);
        nb_vmudm(&acc,v[10],v[9],0,1,v[17]);
        nb_vmudn(&acc,v[9],v[10],0,1,v[17]);
        nb_vmudh(&acc,v[10],v[10],0,1,v[18]);
        v[17][3]=v[18][3]=0;
        for(lane=0;lane<4u;lane+=2u) {
            sum=(unsigned short)v[17][lane]+(unsigned short)v[17][lane+1u];
            v[17][lane]=(short)sum;
            v[18][lane]=nb_clamp_s16((int)v[18][lane]+v[18][lane+1u]+(int)(sum>>16));
        }
        sum=(unsigned short)v[17][0]+(unsigned short)v[17][2];
        v[17][0]=(short)sum;
        v[18][0]=nb_clamp_s16((int)v[18][0]+v[18][2]+(int)(sum>>16));
        rsq=(unsigned int)rsp_rsq32((int32_t)(((unsigned int)(unsigned short)v[18][0]<<16)|(unsigned short)v[17][0]));
        v[19][0]=(short)rsq;v[20][0]=(short)(rsq>>16);
        nb_vmudl(&acc,v[19],v[22],8,0,v[19]);
        nb_vmudm(&acc,v[20],v[22],8,1,v[20]);
        nb_vmudn(&acc,v[30],v[30],8,1,v[19]);
        nb_vmudl(&acc,v[9],v[19],8,0,v[17]);
        nb_vmudm(&acc,v[10],v[19],8,1,v[17]);
        nb_vmudn(&acc,v[9],v[20],8,1,v[17]);
        nb_vmudh(&acc,v[10],v[20],8,1,v[18]);
        nb_vmudm(&acc,v[21],v[17],0,0,v[9]);
        nb_vmudh(&acc,v[21],v[18],0,1,v[10]);
        out_st[i]=((unsigned int)(unsigned short)v[10][0]<<16)|(unsigned short)v[10][1];
        nb_dmem_w32(0xf80u+i*4u,out_st[i]);
    }
    hle_timing_add(HLE_WORK_MATRIX_MUL,4u);
}

/* Naboo clip weight: t = (P.in) / (P.in - P.out) through the exact
 * fixed-point path (overlay text 0xe70-0xf24 + the resident refined
 * reciprocal at 0xa08): the coarse doubled reciprocal of D (negated
 * when D >= 0, saturated to a u16 fraction when its integer part is
 * nonzero), one Newton refinement of D x coarse against 2.0, and
 * the recombine N x refined x coarse over the v30 x 4 seed.
 * Returns the u16 weight applied to the INSIDE vertex (1 - t); the
 * outside weight is its two's complement. */
static unsigned int nb_clip_wt(const int32_t in4[4], const int32_t out4[4],
                               const int16_t P[4])
{
    int64_t n48 = 0, d48 = 0;
    int32_t n32, d32;
    NbFixed denominator,numerator,coarse,scaled,refined,product,error;
    int64_t acc;
    unsigned int cu16, w;
    int k;

    for (k = 0; k < 4; k++) {
        n48 += (int64_t)P[k] * in4[k];
        d48 += (int64_t)P[k] * out4[k];
    }
    d48 = n48 - d48;
    n32 = (int32_t)n48;
    d32 = (int32_t)d48;

    denominator.hi=(short)((uint32_t)d32>>16);denominator.lo=(uint16_t)d32;
    numerator.hi=(short)((uint32_t)n32>>16);numerator.lo=(uint16_t)n32;
    {
        uint32_t bits=(uint32_t)rsp_rcp32_dp(d32);
        acc=(int64_t)(uint16_t)bits*2;
        coarse.lo=(unsigned int)nb_acc_low(acc);
        acc+=(int64_t)(short)(bits>>16)*131072;
        coarse.hi=nb_acc_mid(acc);
    }
    if(d32<0){
        acc=-(int64_t)coarse.lo;
        coarse.lo=(unsigned int)nb_acc_low(acc);
        acc-=(int64_t)coarse.hi*65536;
        coarse.hi=nb_acc_mid(acc);
    }
    cu16=coarse.hi==0?coarse.lo:0xffffu;
    /* The denominator is scaled by the clamped UNSIGNED fraction, not
     * the full reciprocal pair. The subsequent divide refines that
     * scaled denominator before recombining the numerator and scale. */
    scaled=nb_fixed_scale(denominator,cu16);
    {
        uint32_t bits=(uint32_t)rsp_rcp32_dp((int32_t)((uint32_t)(uint16_t)scaled.hi<<16|scaled.lo));
        acc=(int64_t)(uint16_t)bits*2+(int64_t)(short)(bits>>16)*131072;
        refined.lo=(unsigned int)nb_acc_low(acc);refined.hi=nb_acc_mid(acc);
    }
    product=nb_fixed_mul(refined,scaled);
    error.lo=(0u-product.lo)&0xffffu;
    error.hi=nb_clamp_s16(2-product.hi-(product.lo!=0));
    refined=nb_fixed_mul(refined,error);
    product=nb_fixed_mul(numerator,refined);
    /* The selected lane starts with (v30[3] * v30[5]) >> 16 == 1. */
    acc=1+((int64_t)product.lo*cu16>>16)+(int64_t)product.hi*cu16;
    w=nb_acc_mid(acc)==0?(unsigned int)nb_acc_low(acc):0xffffu;
    if (w == 0u)
        w = 1u;
    return w;
}

/* Overlay 0x03: polygon clip (Sutherland-Hodgman against five
 * frustum planes), the Rogue Squadron algorithm on Naboo
 * addressing.  Ping-pong vertex-handle lists at DMEM 0x560/0x574
 * (zero-terminated), plane masks at DMEM 0x58 + 2k, plane
 * coefficients at DMEM (plane_sel << 2), interpolated vertices
 * staged at 0x380 with the record stride, each reprojected and
 * re-outcoded through the resident path; the surviving polygon
 * fans from its first vertex through the emitter's post-gate
 * entry.  Returns 0, or -1 to fall back. */
static int nb_clip(RdpFifo *fifo, unsigned int ra, unsigned int rb,
                   unsigned int rc)
{
    unsigned int lists[2][12];
    unsigned int n_in, n_out, cur;
    unsigned int stage = 0x380u;
    unsigned int r25;
    unsigned int i;

    nb.dmem[0x58au ^ 3u] = 0u;          /* batching countdown reset */
    nb_dmem_w16(0x588u, ra);            /* fan-centre handle */

    lists[0][0] = ra; lists[0][1] = rb; lists[0][2] = rc;
    nb_dmem_w16(0x560u,ra);nb_dmem_w16(0x562u,rb);
    nb_dmem_w16(0x564u,rc);nb_dmem_w16(0x566u,0u);
    n_in = 3u; cur = 0u;

    /* six planes: the microcode's bgtz tests before the delay-slot
     * decrement, so r25 walks a,8,6,4,2,0 (text 0xdbc) */
    for (r25 = 0xau; (int)r25 >= 0; r25 -= 2u) {
        unsigned int mask = (unsigned int)nb_dmem_s16(0x58u + r25) & 0xffffu;
        int16_t P[4];
        unsigned int k;
        for (k = 0; k < 4u; k++)
            P[k] = (int16_t)nb_dmem_s16((r25 << 2) + k * 2u);
        n_out = 0u;
        for (i = 0; i < n_in; i++) {
            unsigned int va = lists[cur][i];
            unsigned int vb = lists[cur][(i + 1u) % n_in];
            unsigned int sa = (unsigned int)nb_dmem_s16(va + 0x24u) & mask;
            unsigned int sb2 = (unsigned int)nb_dmem_s16(vb + 0x24u) & mask;
            if (sa == 0u)
                lists[cur ^ 1u][n_out++] = va;
            if (sa != sb2) {
                /* crossing: build the interpolated vertex */
                unsigned int vin  = sa ? vb : va;
                unsigned int vout = sa ? va : vb;
                int32_t in4[4], out4[4], wc, wt;
                for (k = 0; k < 4u; k++) {
                    in4[k]  = (int32_t)((nb_dmem_s16(vin + k * 2u) << 16)
                              | ((unsigned)nb_dmem_s16(vin + 8u + k * 2u) & 0xffffu));
                    out4[k] = (int32_t)((nb_dmem_s16(vout + k * 2u) << 16)
                              | ((unsigned)nb_dmem_s16(vout + 8u + k * 2u) & 0xffffu));
                }
                wt = (int32_t)nb_clip_wt(in4, out4, P);
                wc = (int32_t)((0x10000u - (unsigned)wt) & 0xffffu);
                {
                    static int t = -1;
                    if (t < 0) t = getenv("NB_CLIP_TRACE") != NULL;
                    if (t) fprintf(stderr,
                        "[CWT] in=%03x out=%03x wc=%d:%d wt=%d:%d\n",
                        vin, vout,
                        (int)(int16_t)(wc >> 16), (int)(int16_t)wc,
                        (int)(int16_t)(wt >> 16), (int)(int16_t)wt);
                }
                for (k = 0; k < 4u; k++) {
                    /* Q16 lerp: in x w + out x ~w (udl/adm/adl/adm) */
                    int64_t pa = ((int64_t)(in4[k] & 0xffff) * wc) >> 16;
                    int64_t p2;
                    pa += (int64_t)(in4[k] >> 16) * wc;
                    pa += ((int64_t)(out4[k] & 0xffff) * wt) >> 16;
                    pa += (int64_t)(out4[k] >> 16) * wt;
                    p2 = pa;            /* 32-bit position */
                    nb_dmem_w16(stage + k * 2u, ((uint64_t)p2 >> 16) & 0xffffu);
                    nb_dmem_w16(stage + 8u + k * 2u, (uint64_t)p2 & 0xffffu);
                }
                /* colors (bytes +0x10..13, u8 << 7 lanes) and the
                 * S/T halfwords (+0x14/+0x16) lerp with the same
                 * weight pair (text 0xf40-0xf60) */
                {
                    short outside[4];
                    nb_size_fraction_set(((unsigned int)nb.dmem[((vin+0x10u)&0xfffu)^3u]<<7)|
                        ((unsigned int)nb.dmem[((vin+0x11u)&0xfffu)^3u]<<23));
                    outside[0]=(short)nb_dmem_s16(vout+0x14u);
                    outside[1]=(short)nb_dmem_s16(vout+0x16u);
                    outside[2]=(short)(nb.dmem[((vout+0x16u)&0xfffu)^3u]<<7);
                    outside[3]=(short)(nb.dmem[((vout+0x17u)&0xfffu)^3u]<<7);
                    nb_retain(1,outside);
                }
                for (k = 0; k < 4u; k++) {
                    int a8 = (int)nb.dmem[((vin + 0x10u + k) & 0xfffu) ^ 3u] << 7;
                    int b8 = (int)nb.dmem[((vout + 0x10u + k) & 0xfffu) ^ 3u] << 7;
                    /* vmudm signed x u16-weight pair */
                    int r = (int)((((int64_t)a8 * wc) + ((int64_t)b8 * wt)) >> 16);
                    if (r > 32767) r = 32767;
                    if (r < -32768) r = -32768;
                    nb.dmem[((stage + 0x10u + k) & 0xfffu) ^ 3u] =
                        (unsigned char)(((unsigned)(int16_t)r >> 7) & 0xffu);
                }
                for (k = 0; k < 2u; k++) {
                    int a16 = nb_dmem_s16(vin + 0x14u + k * 2u);
                    int b16 = nb_dmem_s16(vout + 0x14u + k * 2u);
                    int r = (int)((((int64_t)a16 * wc) + ((int64_t)b16 * wt)) >> 16);
                    if (r > 32767) r = 32767;
                    if (r < -32768) r = -32768;
                    nb_dmem_w16(stage + 0x14u + k * 2u,
                                (unsigned int)r & 0xffffu);
                }
                nb_dmem_w16(stage + 0x24u, nb_project(stage));
                if(!(rc&15u))nb_retain_dmem(0,rc+8u);
                lists[cur ^ 1u][n_out++] = stage;
                stage += 0x28u;
                if (stage > 0x560u)
                    return -1;          /* staging overrun */
            }
            if (n_out >= 10u)
                return -1;
        }
        {
            static int t = -1;
            if (t < 0) t = getenv("NB_CLIP_TRACE") != NULL;
            if (t) {
                unsigned int q;
                fprintf(stderr, "[CLP] plane r25=%x mask=%04x -> n=%u:",
                        r25, mask, n_out);
                for (q = 0; q < n_out; q++)
                    fprintf(stderr, " %03x(%04x)",
                            lists[cur ^ 1u][q],
                            (unsigned)nb_dmem_s16(lists[cur ^ 1u][q] + 0x24u) & 0xffffu);
                fprintf(stderr, "\n");
            }
        }
        {
            unsigned int base=cur?0x560u:0x574u;
            for(i=0;i<n_out;i++)nb_dmem_w16(base+i*2u,lists[cur^1u][i]);
            nb_dmem_w16(base+n_out*2u,0u);
        }
        cur ^= 1u;
        n_in = n_out;
        if (n_in == 0u)
            return 0;                   /* clipped away entirely */
    }

    /* persist the surviving polygon for the fan (the microcode's
     * final ping-pong list is live DMEM state; write it back with
     * the zero terminator) */
    {
        unsigned int base = (cur == 0u) ? 0x560u : 0x574u;
        for (i = 0; i < n_in; i++) {
            nb.clip_poly[i] = lists[cur][i];
            nb_dmem_w16(base + i * 2u, lists[cur][i]);
        }
        nb_dmem_w16(base + n_in * 2u, 0u);
    }
    nb.clip_n = n_in;
    nb.clip_idx = 1u;
    nb.clip_active = 1u;
    return 0;
}

/* Fan emission for a built clip polygon: (poly[0], poly[i],
 * poly[i+1]) through the post-gate path -- the winding cull applies,
 * the outcode gates do not, and the splice check runs per triangle
 * exactly as it does for direct triangles (the jalr at text 0x2a8).
 * Returns 0 done, 1 suspended on a splice, -1 fallback. */
static int nb_clip_fan(RdpFifo *fifo)
{
    {
        static int t = -1;
        if (t < 0) t = getenv("NB_CLIP_TRACE") != NULL;
        if (t) fprintf(stderr, "[CFAN] task=%u n=%u idx=%u active=%u\n",
                       nb_task_ordinal, nb.clip_n, nb.clip_idx,
                       nb.clip_active);
    }
    while (nb.clip_idx + 1u < nb.clip_n) {
        unsigned int va = nb.clip_poly[0];
        unsigned int vb = nb.clip_poly[nb.clip_idx];
        unsigned int vc = nb.clip_poly[nb.clip_idx + 1u];
        /* winding cull exactly as the emitter does it (text 0x230-
         * 0x244): cross = (C-A)x(B-A)y - (B-A)x(C-A)y from the
         * screen coordinates, culled when negative and geometry
         * mode bit 13 is set (geom << 18 puts bit 13 in the sign,
         * bltz exits through the jr r30 at 0x9e8).  Degenerate
         * zero-cross triangles fall out of the emitter naturally. */
        {
            int32_t ax = (int16_t)nb_dmem_s16(va + 0x18u);
            int32_t ay = (int16_t)nb_dmem_s16(va + 0x1au);
            int32_t bx = (int16_t)nb_dmem_s16(vb + 0x18u);
            int32_t by = (int16_t)nb_dmem_s16(vb + 0x1au);
            int32_t cx = (int16_t)nb_dmem_s16(vc + 0x18u);
            int32_t cy = (int16_t)nb_dmem_s16(vc + 0x1au);
            nb_size_fraction_set((unsigned short)nb_clamp_s16(ax-bx)|
                ((unsigned int)(unsigned short)nb_clamp_s16(ay-by)<<16));
            int64_t cross = (int64_t)nb_clamp_s16(cx-ax)*nb_clamp_s16(by-ay)
                          - (int64_t)nb_clamp_s16(bx-ax)*nb_clamp_s16(cy-ay);
            if (!cross || (cross < 0 && (nb.geom & 0x2000u))) {
                nb.clip_idx++;
                continue;
            }
        }
        /* the staged-list check sits at text 0x2a8, which is PAST the
         * winding-cull exit at 0x244 (bltz -> 0x9e8, jr r30): a culled
         * triangle leaves the emitter before ever reaching it and so
         * does NOT consume a pending splice.  Checking ahead of the
         * cull made the walker swallow arms the microcode leaves
         * standing, which is what pulled a later fan into a list the
         * microcode never entered. */
        if ((nb_dmem_s16(0x152u)&0xfffu)!=0xcc8u && nb_call_splice(fifo)<0)return -1;
        if (nb_emit_tri(fifo, va, vb, vc) < 0)
            return -1;
        nb.clip_idx++;
    }
    nb.clip_active = 0u;
    return 0;
}

/* Gate a triangle exactly as the emitter entry does BEFORE its jalr
 * (text 0x1a8-0x2a4): trivial reject on the ANDed outcodes, clip
 * trigger on the ORed outcodes, winding cull, degenerate skip.
 * Returns 0 = draw, 1 = silently skipped, -1 = clip overlay. */
static int nb_tri_gate(unsigned int ra, unsigned int rb, unsigned int rc)
{
    unsigned int oa = nb_dmem_s16(ra + 0x24u) & 0xffffu;
    unsigned int ob = nb_dmem_s16(rb + 0x24u) & 0xffffu;
    unsigned int oc = nb_dmem_s16(rc + 0x24u) & 0xffffu;
    int32_t ax, ay, bx, by, cx, cy;
    int32_t d1x, d1y, d2x, d2y;
    int64_t cross;

    if (oa & ob & oc & 0x7070u)
        return 1;                       /* trivial reject */
    if ((oa | ob | oc) & 0x4343u)
        return -1;                      /* clip overlay: fall back */

    ax = (int16_t)nb_dmem_s16(ra + 0x18u); ay = (int16_t)nb_dmem_s16(ra + 0x1au);
    bx = (int16_t)nb_dmem_s16(rb + 0x18u); by = (int16_t)nb_dmem_s16(rb + 0x1au);
    cx = (int16_t)nb_dmem_s16(rc + 0x18u); cy = (int16_t)nb_dmem_s16(rc + 0x1au);
    nb_size_fraction_set((unsigned short)nb_clamp_s16(ax-bx)|
        ((unsigned int)(unsigned short)nb_clamp_s16(ay-by)<<16));
    d1x = bx - ax; d1y = by - ay;
    d2x = cx - ax; d2y = cy - ay;
    if (d1x > 32767)  d1x = 32767;
    if (d1x < -32768) d1x = -32768;
    if (d1y > 32767)  d1y = 32767;
    if (d1y < -32768) d1y = -32768;
    if (d2x > 32767)  d2x = 32767;
    if (d2x < -32768) d2x = -32768;
    if (d2y > 32767)  d2y = 32767;
    if (d2y < -32768) d2y = -32768;
    cross = (int64_t)d2x * d1y - (int64_t)d1x * d2y;
    if (cross == 0)
        return 1;                       /* degenerate */
    if (cross < 0 && (nb.geom & 0x2000u))
        return 1;                       /* winding cull */
    return 0;
}

static int nb_emit_tri(RdpFifo *fifo, unsigned int ra, unsigned int rb,
                       unsigned int rc)
{
    RspTriVtx va, vb, vc;
    int32_t ew[64];
    int nw;
    int tilebyte, prefix;

    prefix=nb_attribute_callback(fifo);
    if (prefix < 0) return -1;

    nb_vtx(ra, &va); nb_vtx(rb, &vb); nb_vtx(rc, &vc);
    /* text 0x3fc-0x400: `andi v0, r21, 0x1000` / `bne v0, zero, 0x474`
     * jumps clear over the whole 1/w block (0x404-0x470), so with the
     * geometry word's bit 12 set the writer stores the texel shorts as
     * loaded, with no per-vertex perspective normalizer. */
    rsp_set_persp_skip((nb.geom & 0x1000u) ? 1 : 0);
    {
        static int t = -1;
        if (t < 0) t = getenv("NB_EMIT_TRACE") != NULL;
        if (t) fprintf(stderr, "[EV] a=(%d,%d) b=(%d,%d) c=(%d,%d)\n",
                       va.x, va.y, vb.x, vb.y, vc.x, vc.y);
    }

    tilebyte = (int)nb.dmem[0x14au ^ 3u];
    /* the Naboo emitter shares Rogue Squadron's edge-anchor and
     * attribute conventions; the shared writer's flags are global, so
     * assert them per call (another walker may have run) */
    rsp_set_tri_attr_rs(2);
    rsp_tri_set_rs_sort(1);
    nw = rsp_tri_write(ew, &va, &vb, &vc,
                       (int)(nb.geom >> 1) & 1,
                       (int)nb.geom & 1,
                       (int)(nb.geom >> 2) & 1, 1,
                       tilebyte & 7, (tilebyte >> 3) & 7,
                       0x1000, 0x20, (int32_t)0xfff8, 0x1cc);
    if (nw > 0) {
        unsigned int context[5],p;
        rsp_tri_factor5_context(context);
        nb_size_fraction_set(context[0]);
        for(p=0;p<2;p++){
            unsigned int old=nb_retained[4+p];
            unsigned int scale=(unsigned short)nb_dmem_s16(0x36u);
            nb_retained[p]=(unsigned short)((unsigned short)old*scale)|
                ((unsigned int)(unsigned short)((old>>16)*scale)<<16);
            nb_retained[2+p]=context[1+p];
            nb_retained[4+p]=context[3+p];
        }
        nb_retained_written=63u;
        /* The resident writer ORs the complete tile byte into the winding
         * byte, and all low geometry bits into the opcode. Preserve even
         * their reserved bits, rather than truncating them in the shared
         * writer's tile/level arguments. */
        ew[0] |= (int32_t)(((unsigned int)tilebyte & 0xc0u)<<16);
        ew[0] |= (int32_t)((nb.geom & 0x30u)<<24);
        nb_phase_set(((unsigned int)ew[0]>>16)&0xffu);
        {
            int k;
            /* The per-triangle callback drains the previous DMEM packet.
             * Its bytes remain observable as padding in a later mode sync. */
            for(k=0;k<nw;k++)nb_dmem_w32(0xe80u+(unsigned int)prefix+(unsigned int)k*4u,(unsigned int)ew[k]);
        }
        rdp_fifo_append(fifo, ew, nw);
    }
    return 0;
}

static int nb_tri(RdpFifo *fifo, unsigned int w0, unsigned int w1, int quad)
{
    if (!nb_emit_on)
        return -1;

    unsigned int va = (w1 >> 16) & 0xfff8u;
    unsigned int vb = w1 & 0xfff8u;
    unsigned int vc, vd, off_a, off_b, off_c, off_x;
    unsigned int cd;
    unsigned int len = 16u;

    /* batching countdown (text 0xb8c); on a post-splice reprocess
     * (tri_phase nonzero) the countdown was already consumed.
     * Phases: 0 fresh, 2 countdown consumed / first triangle pending,
     * 1 first triangle emitted / second pending. */
    if (nb.tri_phase == 0u) {
        cd = nb.dmem[0x58au ^ 3u];
        if (cd == 0u) {
            nb.dmem[0x58au ^ 3u] = (unsigned char)w0;
        } else {
            nb.dmem[0x58au ^ 3u] = (unsigned char)(cd - 1u);
            nb_dl_step((w0 & 0x200u) ? 32u : 16u);
            return 0;                   /* skipped by the countdown */
        }
        nb.tri_phase = 2u;
    }


    vc    = nb_read_u32(nb.dl + 12u) >> 16;
    vc   &= 0xfff8u;
    off_x = (nb_read_u32(nb.dl + 8u) >> 24) & 0xffu;
    off_a = (nb_read_u32(nb.dl + 8u) >> 16) & 0xffu;
    off_b = (nb_read_u32(nb.dl + 8u) >> 8) & 0xffu;
    off_c =  nb_read_u32(nb.dl + 8u) & 0xffu;

    vd = nb_read_u32(nb.dl + 12u) & 0xfff8u;    /* halfword +14 */

    if (nb.geom & 0x10000u) {
        /* fog-into-alpha propagation (text 0xab8-0xad4); the extra
         * slot pairs with the staged fourth vertex D on the quad op
         * (slot 0x0b, entry 1:b44) and with DMEM byte 0x13 on the
         * single-triangle op (slot 0x16, entry 1:b24, r15 = 0) */
        nb.dmem[(0xd43u + off_x) ^ 3u] = quad
            ? nb.dmem[((vd + 0x13u) & 0xfffu) ^ 3u]
            : nb.dmem[0x13u ^ 3u];
        nb.dmem[(0xd43u + off_a) ^ 3u] = nb.dmem[((va + 0x13u) & 0xfffu) ^ 3u];
        nb.dmem[(0xd43u + off_b) ^ 3u] = nb.dmem[((vb + 0x13u) & 0xfffu) ^ 3u];
        nb.dmem[(0xd43u + off_c) ^ 3u] = nb.dmem[((vc + 0x13u) & 0xfffu) ^ 3u];
    }

    {
        static int t = -1;
        if (t < 0) t = getenv("NB_TRI_TRACE") != NULL;
        if (t) fprintf(stderr, "[TRI] @%06x va=%03x vb=%03x vc=%03x off=%02x/%02x/%02x col_a=%08x\n",
                       nb.dl, va, vb, vc, off_a, off_b, off_c,
                       nb_dmem_r32(0xd40u + off_a));
    }
    nb_dmem_w32(va + 0x10u, nb_dmem_r32(0xd40u + off_a));
    nb_dmem_w32(vb + 0x10u, nb_dmem_r32(0xd40u + off_b));
    nb_dmem_w32(vc + 0x10u, nb_dmem_r32(0xd40u + off_c));
    /* every command is a quad: A/B/C poked by the parser, D by the
     * handler tail (text 0xb5c-0xb60), which then emits triangles
     * (A,B,C) and (A,C,D) */
    if (quad)
        nb_dmem_w32(vd + 0x10u, nb_dmem_r32(0xd40u + off_x));

    if (w0 & 0x800u) {
        /* overlay 0x2a: environment-mapped S/T computed from the
         * vertex normals; the four index bytes ride the low byte of
         * each would-be inline S/T word (+19/+23/+27/+31), and the
         * results replace the inline pokes (rejoin text 0xb10 for
         * A/B/C, the quad tail 0xb60 lane e12 for D) */
        unsigned int est[4];
        len = 32u;
        nb_ovl2a(nb.dl, est);
        nb_dmem_w32(va + 0x14u, est[0]);
        nb_dmem_w32(vb + 0x14u, est[1]);
        nb_dmem_w32(vc + 0x14u, est[2]);
        if (quad)
            nb_dmem_w32(vd + 0x14u, est[3]);
    } else if (w0 & 0x200u) {
        len = 32u;
        /* inline S/T: slv elements e0/e4/e8/e12 map words +16/+20/
         * +24/+28 to A/B/C/D */
        nb_dmem_w32(va + 0x14u, nb_read_u32(nb.dl + 16u));
        nb_dmem_w32(vb + 0x14u, nb_read_u32(nb.dl + 20u));
        nb_dmem_w32(vc + 0x14u, nb_read_u32(nb.dl + 24u));
        if (quad)
            nb_dmem_w32(vd + 0x14u, nb_read_u32(nb.dl + 28u));
    }
    /* emit: (A,B,C), then (A,C,D) on the quad op.  The emitter's
     * jalr through DMEM 0x152 (the default 0x17b4 splice-check) runs
     * PER TRIANGLE: a staged list in 0x58c is walked before that
     * triangle, and the microcode resumes mid-quad (state at
     * 0x590-0x594).  The walker models the resume with tri_phase:
     * on reprocess after a splice, already-emitted triangles are not
     * re-emitted (the pokes above are idempotent). */
    if (nb.tri_phase == 2u) {
        int g = nb_tri_gate(va, vb, vc);
        if (g < 0) {
            /* outcode 0x4343: the clip overlay */
            if (!nb.clip_active) {
                if (nb_clip(fifo, va, vb, vc) < 0)
                    return -1;
            }
            if (nb.clip_active) {
                int cr = nb_clip_fan(fifo);
                if (cr < 0)
                    return -1;
                if (cr > 0)
                    return 1;           /* splice: reprocess */
            }
            g = 1;                      /* handled; skip direct emit */
        }
        if (g == 0) {
            unsigned int splice = nb_dmem_r32(0x58cu) & 0x00fffff8u;
            if (splice != 0u) {
                if (nb.sp >= NB_DL_STACK)
                    return -1;
                nb_dmem_w32(0x58cu, 0u);
                nb.stack[nb.sp * 2u] = nb.chunk;
                nb.stack[nb.sp * 2u + 1u] = nb.off;
                nb.sp++;
                nb_dl_enter(splice);
                return 1;               /* re-enter after the list */
            }
            if (nb_emit_tri(fifo, va, vb, vc) < 0)
                return -1;
        }
        nb.tri_phase = 1u;
    }
    if (quad && nb.tri_phase == 1u) {
        int g = nb_tri_gate(va, vc, vd);
        if (g < 0) {
            /* outcode 0x4343: the clip overlay */
            if (!nb.clip_active) {
                if (nb_clip(fifo, va, vc, vd) < 0)
                    return -1;
            }
            if (nb.clip_active) {
                int cr = nb_clip_fan(fifo);
                if (cr < 0)
                    return -1;
                if (cr > 0)
                    return 1;           /* splice: reprocess */
            }
            g = 1;                      /* handled; skip direct emit */
        }
        if (g == 0) {
            unsigned int splice = nb_dmem_r32(0x58cu) & 0x00fffff8u;
            if (splice != 0u) {
                if (nb.sp >= NB_DL_STACK)
                    return -1;
                nb_dmem_w32(0x58cu, 0u);
                nb.stack[nb.sp * 2u] = nb.chunk;
                nb.stack[nb.sp * 2u + 1u] = nb.off;
                nb.sp++;
                nb_dl_enter(splice);
                return 1;
            }
            if (nb_emit_tri(fifo, va, vc, vd) < 0)
                return -1;
        }
    }
    nb.tri_phase = 0u;
    nb_dl_step(len);
    return 0;
}

static unsigned int nb_pertri_st(unsigned int w0, unsigned int w1)
{
    static const unsigned int offsets[4] = { 18u, 20u, 22u, 28u };
    unsigned int records[4], k;
    unsigned int base = (unsigned int)(nb_dmem_s16(0xda2u) + nb_dmem_s16(0xdb2u));
    int bias = (int)(signed char)nb.dmem[0xda1u ^ 3u];
    int indiana = (nb_dmem_s16(0x68u) & 0xfffu) == 0xc70u;
    if (indiana) nb_dmem_w8(0xdb9u, w0 >> 12);
    else nb_dmem_w16(0xdb8u, w0);
    for (k = 0; k < 4; ++k) {
        unsigned int index = (unsigned int)(nb_dmem_s16(base+offsets[k]) + bias);
        records[k] = (unsigned int)nb_dmem_s16(0xd58u+index);
    }
    if (indiana) {
        unsigned int masked = w1 & nb_dmem_r32(w0 & 0xfffu);
        nb_dmem_w32(records[2]+0x14u, masked);
        nb_dmem_w32(records[1]+0x14u, (w1 & 0xffff0000u) ^ masked);
        nb_dmem_w32(records[0]+0x14u, (w1 & 0xffffu) ^ masked);
        w1 ^= masked;
        nb_dmem_w32(records[3]+0x14u, w1);
    } else {
        nb_dmem_w32(records[2]+0x14u, 0u);
        nb_dmem_w32(records[3]+0x14u, w1);
        nb_dmem_w32(records[0]+0x14u, w1 & 0xffffu);
        nb_dmem_w32(records[1]+0x14u, w1 & 0xffff0000u);
    }
    return w1;
}

/* Overlay 12 resolves triangle indices through the morph record table, then
 * selects its RGBA bank and classifies alpha. The three source words must be
 * latched before stores: the record handles can alias each other or the bank.
 * Return 3 for Indiana's transparent-bank skip, 2 for its additional alpha
 * pass, 1 for its preliminary pass, and 0 for one ordinary draw. */
static unsigned int nb_morph_attributes(unsigned int half, unsigned int records[3])
{
    unsigned int indices[3], colors[3], k, alpha;
    unsigned int base = (unsigned int)(nb_dmem_s16(0xda2u) +
                                       nb_dmem_s16(0xdb2u)) + half;
    int bias = (signed char)nb.dmem[0xda1u ^ 3u];
    int mode, indiana = (nb_dmem_s16(0x68u) & 0xfffu) == 0xc70u;
    for (k = 0; k < 3; ++k) indices[k] = (unsigned int)(nb_dmem_s16(base+18u+k*2u)+bias);
    for (k = 0; k < 3; ++k) records[k] = (unsigned int)nb_dmem_s16(0xd58u+indices[k]);
    mode = nb_dmem_s16(0xdb8u);
    for (k = 0; k < 3; ++k) colors[k] = nb_dmem_r32(0xb78u+indices[k]*2u+(unsigned int)mode);
    for (k = 0; k < 3; ++k) nb_dmem_w32(records[k]+0x10u, colors[k]);
    if (indiana) {
        if (mode > 0) { nb_dmem_w16(0xdb8u, 0x8000u); return 1; }
        if (mode < 0) {
            nb_dmem_w16(0xdb8u, 0x78u);
            nb_dmem_w16(0xdbau, half ? 0x1ac4u : 0x1abcu);
            if ((colors[0] | colors[1] | colors[2]) & 255u) return 2;
            return 3;
        }
        return 0;
    }
    if (!mode) { nb_dmem_w32(0x100u, 0); return 0; }
    alpha = (colors[0]&255u)+(colors[1]&255u)+(colors[2]&255u);
    if (mode > 0) { nb_dmem_w8(0x103u, alpha ? 16u : 0u); return 0; }
    if (alpha == 765u) {
        unsigned int fog[3];
        for (k = 0; k < 3; ++k) fog[k] = nb.dmem[((0xb03u+indices[k]*2u+(unsigned int)mode)&0xfffu)^3u];
        /* The first store is the branch delay slot, including an empty fog
         * sum; the other two stores run only for the nonzero sum. */
        nb_dmem_w8(records[0]+0x13u, fog[0]);
        if (fog[0]+fog[1]+fog[2]) {
            nb_dmem_w8(records[1]+0x13u, fog[1]);
            nb_dmem_w8(records[2]+0x13u, fog[2]);
            nb_dmem_w8(0x103u, 16u);
        } else nb_dmem_w8(0x103u, 32u);
    } else nb_dmem_w8(0x103u, 0u);
    return 0;
}

static int nb_call_splice(RdpFifo *fifo)
{
    unsigned int splice = nb_dmem_r32(0x58cu) & 0x00fffff8u;
    unsigned int depth = nb.sp;
    if (!splice) return 0;
    if (depth >= NB_DL_STACK) return -1;
    nb_dmem_w32(0x58cu, 0);
    nb.stack[depth*2u] = nb.chunk;
    nb.stack[depth*2u+1u] = nb.off;
    nb.sp++;
    nb_dl_enter(splice);
    return nb_walk(fifo, depth, 1);
}

/* The morph callback selects one of the three mode blocks at e50/e60/e70.
 * It packs the previous/current classification in DMEM 100 and emits a pipe
 * sync plus the selected mode pair only when that classification changes. */
static int nb_attribute_callback(RdpFifo *fifo)
{
    int32_t old, tag;
    int32_t words[6];
    unsigned int k;
    if ((nb_dmem_s16(0x152u)&0xfffu) != 0xcc8u) return 0;
    old = nb_dmem_s16(0x101u);
    tag = (signed char)nb.dmem[0x103u^3u];
    nb_dmem_w16(0x100u, (unsigned int)old | (unsigned int)tag);
    if (nb_dmem_r32(0x58cu)) {
        unsigned int mode;
        if (nb_call_splice(fifo) < 0) return -1;
        mode = nb_dmem_r32(0x120u)&0xffcfffffu;
        nb_dmem_w32(0xe70u, mode);
        nb_dmem_w32(0xe60u, mode|0x00100000u);
        tag = (signed char)nb.dmem[0x101u^3u];
        if (tag) {
            for (k=0;k<4;k++) words[k]=(int32_t)nb_dmem_r32(0xe50u+(unsigned int)tag+k*4u);
            for (k=0;k<4;k++) nb_dmem_w32(0xe80u+k*4u,(unsigned int)words[k]);
            rdp_fifo_append(fifo, words, 4);
        }
        nb_dmem_w32(0x58cu,0);
        return tag ? 16 : 0;
    }
    if ((unsigned int)tag != (unsigned int)old*256u) {
        words[0]=(int32_t)0xe7000000u;
        words[1]=(int32_t)nb_dmem_r32(0xe84u);
        for(k=0;k<4;k++) words[k+2]=(int32_t)nb_dmem_r32(0xe50u+(unsigned int)tag+k*4u);
        for(k=0;k<6;k++) nb_dmem_w32(0xe80u+k*4u,(unsigned int)words[k]);
        rdp_fifo_append(fifo,words,6);
        return 24;
    }
    return 0;
}

static int nb_morph_draw(RdpFifo *fifo, const unsigned int records[3])
{
    int gate=nb_tri_gate(records[0],records[1],records[2]);
    if(gate>0){nb_dmem_w8(0x58au,0);return 0;}
    if(gate<0){
        int result;
        if(nb_clip(fifo,records[0],records[1],records[2])<0)return -1;
        while(nb.clip_active){
            result=nb_clip_fan(fifo);
            if(result<0)return -1;
            if(result>0 && nb_walk(fifo,nb.sp-1u,1)<0)return -1;
        }
        return 0;
    }
    if((nb_dmem_s16(0x152u)&0xfffu)!=0xcc8u && nb_call_splice(fifo)<0)return -1;
    gate=nb_emit_tri(fifo,records[0],records[1],records[2]);
    nb_dmem_w8(0x58au,0);
    return gate;
}

/* One attribute quad is two triangles. Indiana draws its first RGBA bank,
 * reloads the same triangle from the second bank, and optionally draws an
 * additional alpha pass with the geometry shade bit toggled. */
static int nb_morph_quad(RdpFifo *fifo)
{
    unsigned int half;
    for(half=0;half<=8u;half+=8u){
        unsigned int records[3],kind=nb_morph_attributes(half,records);
        if(kind==1u){
            if(nb_morph_draw(fifo,records)<0)return -1;
            kind=nb_morph_attributes(half,records);
        }
        if(kind==3u)continue;
        if(kind==2u){
            int32_t words[6];unsigned int k;
            if(nb_call_splice(fifo)<0)return -1;
            words[0]=(int32_t)0xe7000000u;
            words[1]=(int32_t)nb_dmem_r32(0xe84u+(fifo->used&0x17fu));
            for(k=0;k<4;k++)words[k+2]=(int32_t)nb_dmem_r32(0xe60u+k*4u);
            rdp_fifo_append(fifo,words,6);
            nb.geom^=2u;
            if(nb_morph_draw(fifo,records)<0){nb.geom^=2u;return -1;}
            nb.geom^=2u;
            for(k=0;k<4;k++)words[k]=(int32_t)nb_dmem_r32(0xe50u+k*4u);
            words[4]=(int32_t)nb_dmem_r32(0x120u);words[5]=(int32_t)nb_dmem_r32(0x124u);
            rdp_fifo_append(fifo,words,6);
        } else if(nb_morph_draw(fifo,records)<0)return -1;
    }
    return 0;
}

static int nb_morph_tail(RdpFifo *fifo)
{
    unsigned int iterations=0;
    for(;;){
        unsigned int masks[2]={0xdacu,0xdaeu},bank;
        nb_dmem_w16(0xdb2u,0xfff0u);
        for(bank=0;bank<2;bank++){
            int value=nb_dmem_s16(masks[bank]);
            nb_dmem_w16(masks[bank],(unsigned int)value*2u);
            if(value&0x80){
                if(value<0)return 0;
                if(nb_morph_quad(fifo)<0)return -1;
            }
            nb_dmem_w16(0xdb2u,0);
        }
        nb_dmem_w8(0xda1u, (unsigned int)((signed char)nb.dmem[0xda1u^3u]+(signed char)nb.dmem[0xda5u^3u]));
        if(!(nb.dmem[0xdadu^3u]|nb.dmem[0xdafu^3u]))return 0;
        if(++iterations>16u)return -1;
    }
}

static int nb_ovl09(RdpFifo *fifo,unsigned int w0,unsigned int w1,int colored)
{
    unsigned char payload[24];
    unsigned int source,offset,destination[2],pass,k;
    nb_dl_fetch(payload,sizeof(payload));
    for(k=0;k<sizeof(payload);k++)nb_dmem_w8(0xda0u+k,payload[k]);
    if(colored)nb_load(0x380u,nb_dmem_r32(0xf8u),512u);
    destination[0]=(unsigned int)nb_dmem_s16(w0);
    destination[1]=(unsigned int)nb_dmem_s16(w0+8u);
    offset=(unsigned int)nb_dmem_s16(0xdbcu);
    source=nb_dmem_r32(0xdb0u);
    for(pass=0;pass<((w0&0x8000u)?2u:1u);pass++){
        nb_load(destination[0]+pass*120u,source,40u);
        nb_load(destination[1]+pass*120u,source+offset,40u);
        source=nb_dmem_r32(0xdb4u);
    }
    nb_ovl09_morph(destination[0],destination[1],w0,w1,colored);
    for(k=0;k<2;k++){
        unsigned int span=(unsigned short)nb_dmem_s16(0xda8u+k*2u);
        unsigned int first=span>>5,count=span&31u;
        if(count)nb_xfrm_range(count,0x170u+k*0x50u+first,
            (unsigned int)nb_dmem_s16(w0+0x10u+k*8u)+first*5u,0x5c0u,1,1);
    }
    return nb_morph_tail(fifo);
}

static int nb_walk(RdpFifo *fifo, unsigned int stop_depth, int nested)
{
    unsigned int guard = 0;
    if (!nb.active)
        return NABOO_R_FALLBACK;

    for (;;) {
        if(nested && nb.sp==stop_depth)return NABOO_R_DONE;
        if (++guard > 200000u || nb.dl > s_rdram_size ||
            s_rdram_size - nb.dl < 8u) {
            nb.active = 0;
            return NABOO_R_FALLBACK;
        }
        unsigned int w0 = nb_read_u32(nb.dl);
        unsigned int w1 = nb_read_u32(nb.dl + 4);
        hle_timing_add(HLE_WORK_COMMAND, 1);
        {
            static int t = -1;
            if (t < 0) t = getenv("NB_CMD_TRACE") != NULL;
            if (t) fprintf(stderr, "[NC] @%06x %08x %08x\n", nb.dl, w0, w1);
        }
        unsigned int cls = w0 >> 30;
        unsigned int op  = (w0 >> 24) & 0xffu;

        if (cls == 3u) {
            /* RDP passthrough (text 0x98).  G_TEXRECT carries two
             * extra inline words. */
            int32_t words[2];
            if (op == 0xe4u || op == 0xe5u) {
                /* G_TEXRECT is the compositor's splice point (text
                 * 0xb4, jal 0x7b4): when the CPU has staged a
                 * texture-setup list in the DMEM word 0x58c, the
                 * handler saves its position and walks that list
                 * first -- its top-level EndDL returns here and the
                 * texrect reprocesses with the link cleared.  The
                 * 'expanded' load blocks in the reference stream are
                 * these CPU-built lists, not generated commands. */
                unsigned int splice = nb_dmem_r32(0x58cu) & 0x00fffff8u;
                if (splice != 0u) {
                    if (nb.sp >= NB_DL_STACK) {
                        nb.active = 0;
                        return NABOO_R_FALLBACK;
                    }
                    nb_dmem_w32(0x58cu, 0u);
                    nb.stack[nb.sp * 2u] = nb.chunk;
                    nb.stack[nb.sp * 2u + 1u] = nb.off;
                    nb.sp++;
                    nb_dl_enter(splice);
                    continue;
                }
            }
            words[0] = (int32_t)w0;
            words[1] = (int32_t)w1;
            rdp_fifo_append(fifo, words, 2);
            nb_dl_step(8u);
            if (op == 0xe4u || op == 0xe5u) {
                words[0] = (int32_t)nb_read_u32(nb.dl);
                words[1] = (int32_t)nb_read_u32(nb.dl + 4);
                rdp_fifo_append(fifo, words, 2);
                nb_dl_step(8u);
            }
            continue;
        }

        /* alias GBI-numbered opcodes onto the custom slot set */
        if (op >= 0xa9u && op <= 0xc8u)
            op -= 0xa9u;

        switch (op) {
        case 0x03: case 0x04:
        case 0x08: case 0x09: case 0x0a: case 0x17:
        case 0x00:                              /* status poll / no-op aliases */
            nb_dl_step(8u);
            continue;
        case 0x15:                              /* overlay-12 per-triangle S/T */
            nb_pertri_st(w0, w1);
            if(nb_morph_quad(fifo)<0)return NABOO_R_FALLBACK;
            nb_dl_step(8u);
            continue;
        case 0x05:                              /* load+run overlay */
            if ((w1 & 0x3fu) == 0x24u) {
                if (!nb_ovl24(fifo,w0)) return NABOO_R_FALLBACK;
                nb_dl_step(40u);
                continue;
            }
            if ((w1 & 0x3fu) == 0x15u) {
                if (!nb_ovl15(fifo,w0)) return NABOO_R_FALLBACK;
                nb_dl_step(32u);
                continue;
            }
            if ((w1 & 0x3fu) == 0x18u) {
                nb_ovl18(w0, w1);
                if (nb.active == 2u) return NABOO_R_FALLBACK;
                nb_dl_step(8u);
                continue;
            }
            if ((w1 & 0x3fu) == 0x27u) {
                unsigned int length = nb_ovl27(w0, w1);
                if (nb.active == 2u) return NABOO_R_FALLBACK;
                nb_dl_step(length);
                continue;
            }
            if ((w1 & 0x3fu) == 0x21u) {
                nb_ovl21(w1);
                if (nb.active == 2u) return NABOO_R_FALLBACK;
                nb_dl_step(8u);
                continue;
            }
            if ((w1 & 0x3fu) == 0x09u || (w1 & 0x3fu) == 0x0cu) {
                /* vertex morphing.  The command is 0x18 long: the
                 * dispatcher consumes eight bytes and the overlay
                 * stages sixteen more (`addi r17, r17, 0x10`). */
                if(nb_ovl09(fifo,w0,w1,(w1&0x3fu)==0x0cu)<0)return NABOO_R_FALLBACK;
                nb_dl_step(24u);
                continue;
            }
            if ((w1 & 0x3fu) == 0x0fu) {
                /* descriptor/setup for the vertex-morphing overlay
                 * 0x09.  The dispatcher has already consumed the
                 * 8-byte command (the overlay reads its payload from
                 * r17 - 8), so the extra length it reports is on top
                 * of that. */
                nb_dl_step(8u + nb_ovl0f(w1));
                continue;
            }
            nb.active = 0;
            return NABOO_R_FALLBACK;
        case 0x07:                              /* DL jump (entry
             * 1:76c): enter the chunk-formatted list at w1 with no
             * stack push -- the current position is abandoned */
            if (!nb_emit_on) {
                nb.active = 0;
                return NABOO_R_FALLBACK;
            }
            nb_dl_enter(w1 & 0x00fffff8u);
            continue;
        case 0x06:                              /* DisplayList: call w1,
             * push the return cursor (text 0x754: stack at DMEM 0xfe0,
             * depth byte at struct+0x32) */
            if (nb.sp >= NB_DL_STACK) {
                nb.active = 0;
                return NABOO_R_FALLBACK;
            }
            nb.stack[nb.sp * 2u] = nb.chunk;
            nb.stack[nb.sp * 2u + 1u] = nb.off + 8u;
            nb.sp++;
            nb_dl_enter(w1);
            continue;
        case 0x0f:                              /* EndDL (GBI 0xb8):
             * pop a pushed cursor, or finish at top level (text 0x778).
             * On the pop path a zero w1 selects the plain resume and
             * a nonzero one the splice-aware resume (text 0x790); the
             * walker's resume covers both. A top-level return loads
             * overlay 0x1e and breaks immediately, even with a pending
             * splice at DMEM 0x58c. */
            if (nb.sp) {
                nb.sp--;
                nb.chunk = nb.stack[nb.sp * 2u];
                nb.off = nb.stack[nb.sp * 2u + 1u];
                nb.dl = nb.chunk + nb.off;
                if (nb.off >= 0x108u) {
                    nb.off -= 8u;
                    nb.dl = nb.chunk + nb.off;
                    nb_dl_step(8u);
                }
                /* the zero-w1 pop resumes through the boot vector
                 * (text 0x790 -> 0x010), where the server follows a
                 * pending live tail and otherwise polls the CPU's
                 * status signal, yielding when it is set.  The signal
                 * is live state a static capture cannot carry, so the
                 * walker continues unconditionally; against a
                 * captured task this can run a few commands past the
                 * microcode's yield point into the next task's work,
                 * which the live baselines show to be a benign
                 * boundary shift. */
                continue;
            }
            nb.active = 0;
            return NABOO_R_DONE;
        case 0x14: {                            /* display-list return
             * with state restore (live IMEM 0xc24).  It pops the same
             * stack the DisplayList call pushes -- depth byte at
             * struct+0x32, cursor pair at 0xfe0/0xfe4 -- so the
             * walker performs the pop it already models for EndDL and
             * adds the state restore on top.
             *
             * `lb v0, 257(zero)` then `lqv v9[e0], -432(v0)` reads
             * sixteen bytes at 0xe50 + v0 and stores them through the
             * RDP output pointer, so a return can emit a command.  On
             * the captured task the gate byte is 0x10 on 60 of 236
             * dispatches, making the emit path live rather than
             * theoretical: the source is then 0xe60, the mode word
             * this same handler has just written. */
            unsigned int mode, gate;
            /* Indiana's table maps this slot directly to the no-op. */
            if ((nb_dmem_s16(0x68u)&0xfffu)==0xc70u) {
                nb_dl_step(8u);
                continue;
            }
            if (nb.sp == 0u) {
                nb.active = 0;
                return NABOO_R_FALLBACK;
            }
            nb.sp--;
            nb.chunk = nb.stack[nb.sp * 2u];
            nb.off   = nb.stack[nb.sp * 2u + 1u];
            nb.dl    = nb.chunk + nb.off;
            nb_dmem_w32(0x5a0u, nb.chunk);
            mode = nb_dmem_r32(0x120u) & 0xffcfffffu;
            nb_dmem_w32(0xe70u, mode);
            nb_dmem_w32(0xe60u, mode | 0x00100000u);
            gate = (unsigned int)(signed char)nb.dmem[0x101u ^ 3u];
            if (gate != 0u) {
                int32_t words[2];
                unsigned int src = (0xe50u + gate) & 0xfffu;
                words[0] = (int32_t)nb_dmem_r32(src);
                words[1] = (int32_t)nb_dmem_r32(src + 4u);
                rdp_fifo_append(fifo, words, 2);
                words[0] = (int32_t)nb_dmem_r32(src + 8u);
                words[1] = (int32_t)nb_dmem_r32(src + 12u);
                rdp_fifo_append(fifo, words, 2);
            }
            nb_dmem_w32(0x58cu, 0u);
            continue;
        }
        case 0x02:                              /* open segmented
             * stream (text 0xd1c) */
            nb_op02(w0, w1);
            nb_dl_step(8u);
            continue;
        case 0x01:                              /* segmented load +
             * subtype processor (state modeling; RDP output comes from
             * the triangle path, which still falls back) */
            nb_op01(w0, w1);
            if (nb.active == 2u) {
                nb.active = 0;
                return NABOO_R_FALLBACK;
            }
            nb_dl_step(8u);
            continue;
        case 0x18: case 0x19: case 0x1a:
            nb_process_loaded(2u + (op - 0x18u) * 2u, w0, w1);
            if (nb.active == 2u) return NABOO_R_FALLBACK;
            nb_dl_step(8u);
            continue;
        case 0x0b:                              /* quad: two
             * triangles (A,B,C) and (A,C,D) (entry 1:b44) */
        case 0x16:                              /* single triangle
             * (A,B,C) (entry 1:b24) */
            {
                int tr = nb_tri(fifo, w0, w1,
                                op == 0x0bu);
                if (tr < 0) {
                    nb.active = 0;
                    return NABOO_R_FALLBACK;
                }
            }
            continue;
        case 0x0c:                              /* end-of-chunk marker:
             * the dispatch entry (1:068) is the chunk-refill routine
             * itself -- it reloads the ring window from the chain
             * pointer and restarts the fetch at the new chunk's first
             * command.  Bytes after this op in the current chunk are
             * dead padding the microcode never walks.  Gated with the
             * emitter on unverified builds. */
            if (!nb_emit_on) {
                nb.active = 0;
                return NABOO_R_FALLBACK;
            }
            {
                unsigned int next = nb_read_u32(nb.chunk) & 0x00fffff8u;
                if (next == 0u) {
                    nb.active = 0;
                    return NABOO_R_FALLBACK;
                }
                nb.chunk = next;
                nb.off = 8u;
                nb.dl = nb.chunk + nb.off;
            }
            continue;
        case 0x0d:                              /* GeometryMode &= w1
             * (text 0xa70) */
            nb.geom &= w1;
            nb_dl_step(8u);
            continue;
        case 0x0e:                              /* GeometryMode |= w1
             * (text 0xa68) */
            nb.geom |= w1;
            nb_dl_step(8u);
            continue;
        case 0x10:                              /* conditional state
             * insert (text 0x7d0): compare w0 bit 23 against the
             * struct word +0xc; on mismatch skip, else perform the
             * slot 0x11 insert */
            {
                static int t = -1;
                if (t < 0) t = getenv("NB_MW_TRACE") != NULL;
                if (t) fprintf(stderr, "[B9] @%06x bit=%u 11c=%08x\n",
                               nb.dl, (w0 >> 23) & 1u, nb_dmem_r32(0x11cu));
            }
            if (((w0 >> 23) & 1u) != nb_dmem_r32(0x11cu)) {
                nb_dl_step(8u);
                continue;
            }
            /* fall through */
        case 0x11:                              /* state-word bitfield
             * insert (text 0x7e0): word = 0x120 + ((w0>>16)&7);
             * clear a field of width (w0&31)+1 at shift (w0>>8)&31,
             * OR in w1 (pre-shifted by the CPU). Words 0/1 double as
             * the RDP SET_OTHER_MODES pair (emitted on the real
             * path); words 4-7 are the viewport block the transform
             * reads. */
            {
                /* RSP scalar words support every byte offset, including
                 * fields that straddle the two other-mode words. */
                unsigned int idx = (w0 >> 16) & 7u;
                unsigned int cur = nb_dmem_r32(0x120u + idx);
                unsigned int msk =
                    (unsigned int)((int32_t)0x80000000 >> (w0 & 31u));
                msk >>= (w0 >> 8) & 31u;
                cur = (cur & ~msk) | w1;
                nb_dmem_w32(0x120u + idx, cur);
            }
            /* forward the refreshed SET_OTHER_MODES pair, replacing
             * the pair at the fifo tail when it is already an EF
             * command (text 0x80c-0x834: consecutive mode inserts
             * collapse into one RDP command) */
            if (nb_emit_on) {
                int32_t words[2];
                words[0] = (int32_t)nb_dmem_r32(0x120u);
                words[1] = (int32_t)nb_dmem_r32(0x124u);
                if (fifo->used >= 8u &&
                    (*(const uint32_t *)(fifo->storage + fifo->used - 8u)
                     >> 24) == 0xefu)
                    fifo->used -= 8u;
                rdp_fifo_append(fifo, words, 2);
            }
            nb_dl_step(8u);
            continue;
        case 0x12:                              /* state toggle (text
             * 0xa80): store w0 at struct +0x38; flip geometry-mode
             * bit 1 when (geom ^ w0) & 2 */
            nb_dmem_w32(0x148u, w0);
            nb.geom ^= (nb.geom ^ w0) & 2u;
            nb_dl_step(8u);
            continue;
        case 0x13:                              /* MoveWord (GBI 0xbc):
             * DMEM[w0 & 0xffc] = w1 (text 0xa78) */
            {
                static int t = -1;
                if (t < 0) t = getenv("NB_MW_TRACE") != NULL;
                if (t && (w0 & 0xfe0u) == 0x120u)
                    fprintf(stderr, "[MW] %03x = %08x\n", w0 & 0xffcu, w1);
            }
            nb_dmem_w32(w0, w1);
            nb_dl_step(8u);
            continue;
        default:
            /* not yet implemented: rerun this slice on the LLE
             * fallback.  (Test harness: NB_PERMISSIVE skips unknown
             * commands so the loader/transform path can be
             * oracle-diffed in isolation; never set in production.) */
            {
                static int perm = -1;
                if (perm < 0)
                    perm = getenv("NB_PERMISSIVE") != NULL;
                if (perm) {
                    unsigned int step = 8u;
                    if (op == 0x05u) {
                        /* load+run overlay: never skippable (overlay
                         * code has arbitrary effects, and the oracle
                         * truncation technique patches list tails
                         * into overlay-0x1e exits) -- stop here */
                        nb.active = 0;
                        return NABOO_R_FALLBACK;
                    }
                    if (op == 0x0bu) {
                        /* empirical (goracle command trace, task 240,
                         * 3826 dispatches): triangles and slot 0x16
                         * are 32 bytes in this workload */
                        step = 32u;
                        nb_dl_step(step);
                        continue;
                    }
                    if (0) {
                        /* triangle batching (text 0xb8c): countdown
                         * byte at DMEM 0x58a, seeded from the first
                         * tri's w0; while active, each tri decrements
                         * it and consumes 16 bytes, or 32 when w0 bit
                         * 9 is set.  INCOMPLETE: triangles also carry
                         * inline per-vertex attribute payloads whose
                         * length depends on the mode routine at DMEM
                         * 0x152 (Rogue Squadron's inline S/T pattern),
                         * so the permissive walk still desyncs on
                         * attribute-bearing batches; exact stepping
                         * lands with the emitter. */
                        unsigned int cd = nb.dmem[0x58au ^ 3u];
                        step = 16u;
                        if (cd == 0u) {
                            nb.dmem[0x58au ^ 3u] = (unsigned char)w0;
                        } else {
                            nb.dmem[0x58au ^ 3u] = (unsigned char)(cd - 1u);
                            if (w0 & 0x200u)
                                step = 32u;
                        }
                    } else if (op == 0x15u) {
                        step = 16u;
                    }
                    nb_dl_step(step);
                    continue;
                }
            }
            nb.active = 0;
            return NABOO_R_FALLBACK;
        }
    }
}

int naboo_run_dl(RdpFifo *fifo, unsigned int dl_addr, int resume)
{
    if(!resume)naboo_task_reset(dl_addr);
    return nb_walk(fifo,0,0);
}

/* Unit-test hook for the clip overlay: seed DMEM via
 * naboo_seed_dmem first, then call with the triangle handles.
 * Emits into the given fifo. */
int naboo_clip_unit(void *fifo, unsigned int ra, unsigned int rb,
                    unsigned int rc)
{
    int r = nb_clip((RdpFifo *)fifo, ra, rb, rc);
    if (r == 0 && nb.clip_active)
        r = nb_clip_fan((RdpFifo *)fifo);
    return r;
}

#include "rdp_emit_state.h"
#define WALKER_FIELDS(X) X(nb) X(nb_emit_on) X(nb_task_ordinal) X(nb_morph_v28) X(nb_morph_v29)
EMIT_STATE_DEFINE(naboo, WALKER_FIELDS)
#undef WALKER_FIELDS
