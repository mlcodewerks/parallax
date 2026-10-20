#ifndef WTFN64_RDP_TIMING_H
#define WTFN64_RDP_TIMING_H

#include <stdint.h>
#include <string.h>

#ifndef RDP_TIMING_OBSERVE_SPAN
#define RDP_TIMING_OBSERVE_SPAN(t, pixels, rows) ((void)0)
#define RDP_TIMING_OBSERVE_COMMAND(t, op, work) ((void)0)
#define RDP_TIMING_OBSERVE_FINISH(t, submitted) ((void)0)
#endif

#ifndef RDP_TIMING_OBSERVE_RESPONSE
#define RDP_TIMING_OBSERVE_RESPONSE(t, clocks) ((void)0)
#endif

/* Reserved extra-state space before the CPU-cache extension at 4096. */
#define RDP_TIMING_STATE_OFFSET 4000

#define RDP_RASTER_STATE_OFFSET 18432u
struct rdp_uniform_depth { uint32_t address, width, height, z, valid; };
struct rdp_timing {
    int64_t deadline;
    uint32_t words[8];
    uint32_t pos, length;
    uint32_t cycle_type, fb_size, modes;
    uint32_t left, top, right, bottom;
    uint32_t tex_size, tex_width, tex_address;
    uint32_t fb_width, fb_address, scissor_field;
    /* Quarter COUNT ticks by which the rounded deadline exceeds real work. */
    uint32_t clock_overhang;
    uint32_t z_address, atomic_primitive;
    uint32_t primitive_color, environment_color, blend_color, fill_color;
    uint32_t combine_hi, combine_lo, primitive_depth, reject;
    struct rdp_uniform_depth uniform[8];
    void (*observe_busy)(void *, int64_t, int64_t, unsigned);
    void *busy_context;
    uint32_t rdram_bytes, validate_uniform_ram;
};

/* Command-derived uniform regions are proofs, not samples from GPU-stale
 * RDRAM. CPU/DMA and overlapping raster writes invalidate them. Unknown or
 * partially overwritten regions retain conservative work. */
static void rdp_timing_invalidate_range(struct rdp_timing *t, uint32_t address, uint64_t bytes)
{
    address &= 0xffffffu;
    uint64_t end_write = (uint64_t)address + bytes;
    for (unsigned i = 0; i < 8; ++i) {
        struct rdp_uniform_depth *u = &t->uniform[i];
        uint64_t end = (uint64_t)u->address + (uint64_t)u->width * u->height * 2;
        if (u->valid && (bytes >= 0x1000000 ||
            (address < end && u->address < end_write) ||
            (end_write > 0x1000000 && u->address < end_write - 0x1000000))) u->valid = 0;
    }
}
static uint32_t rdp_timing_decompress_z(uint32_t packed)
{
    static const unsigned shift[8] = {6,5,4,3,2,1,0,0};
    static const uint32_t add[8] = {0,0x20000,0x30000,0x38000,0x3c000,0x3e000,0x3f000,0x3f800};
    unsigned exponent = (packed >> 13) & 7;
    return add[exponent] + (((packed >> 2) & 0x7ff) << shift[exponent]);
}
static uint32_t rdp_timing_quantize_z(uint32_t z)
{
    unsigned shift = z < 0x20000 ? 6 : z < 0x30000 ? 5 : z < 0x38000 ? 4 :
        z < 0x3c000 ? 3 : z < 0x3e000 ? 2 : z < 0x3f000 ? 1 : 0;
    return (z >> shift) << shift;
}
static struct rdp_uniform_depth *rdp_timing_uniform(struct rdp_timing *t, uint32_t address)
{
    for (unsigned i = 0; i < 8; ++i)
        if (t->uniform[i].valid && t->uniform[i].address == address) return &t->uniform[i];
    return NULL;
}
static void rdp_timing_record_uniform(struct rdp_timing *t, uint32_t address,
    uint32_t width, uint32_t height, uint32_t z)
{
    if ((address & 7) || !t->rdram_bytes || (uint64_t)address + (uint64_t)width * height * 2 > t->rdram_bytes) return;
    struct rdp_uniform_depth *u = rdp_timing_uniform(t, address);
    if (!u) {
        for (unsigned i = 0; i < 8; ++i) if (!t->uniform[i].valid) { u = &t->uniform[i]; break; }
        if (!u) u = &t->uniform[(address >> 10) & 7];
    }
    u->address = address; u->width = width; u->height = height; u->z = z; u->valid = 1;
}
/* -1 unknown, 0 passes, 1 rejected. The cancelled alpha mux leaves only D.
 * Coverage-dependent alpha and noise thresholds need per-pixel information. */
static int rdp_timing_alpha_result(const struct rdp_timing *t)
{
    if (!(t->modes & 1)) return 0;
    if (t->modes & (2u | 0x3000u)) return -1;
    unsigned a = (t->combine_lo >> 21) & 7, b = (t->combine_lo >> 3) & 7;
    unsigned c = (t->combine_lo >> 18) & 7, d = t->combine_lo & 7;
    if (a != b && c != 7) return -1;
    unsigned alpha;
    if (d == 3) alpha = t->primitive_color & 255;
    else if (d == 5) alpha = t->environment_color & 255;
    else if (d == 6) alpha = 255;
    else if (d == 7) alpha = 0;
    else return -1;
    return alpha < (t->blend_color & 255);
}
static int rdp_timing_depth_result(struct rdp_timing *t, unsigned x1, unsigned y1)
{
    if (!(t->modes & 0x10)) return 0;
    if (!(t->modes & 4) || (t->modes & (0xc00u | 0x3000u | 8u))) return -1;
    struct rdp_uniform_depth *u = rdp_timing_uniform(t, t->z_address);
    if (!u || u->width != t->fb_width || u->height < y1 || x1 > u->width) return -1;
    unsigned z = ((t->primitive_depth >> 16) & 0x7fff) << 3;
    if (u->z == 0x3ffff || z < u->z) return 0;
    /* Known regions have DZ=1. Even at the coarsest Z precision the opaque
     * tolerance is at most 16 << 3. Close/equal depth remains unknown because
     * coverage selects strict versus tolerance comparison. */
    if (!(t->primitive_depth & 0xffff) && z > u->z + 128) return 1;
    return -1;
}

static int32_t rdp_timing_signed(uint32_t value, unsigned bits)
{
    return (int32_t)(value << (32 - bits)) >> (32 - bits);
}

/* Count 64-bit words touched by a contiguous bit range. Partial words
 * cannot be shared between scanlines. Address alignment is a bus-work
 * approximation, not an RDRAM latency or arbitration model. */
static uint64_t rdp_timing_bus_words(uint64_t first_bit, uint64_t bits)
{
    return bits ? ((first_bit & 63) + bits + 63) / 64 : 0;
}

static int rdp_timing_keeps_row(const struct rdp_timing* t, int y)
{
    return !(t->scissor_field & 2) || ((y & 1) == (int)(t->scissor_field & 1));
}

/* RDRAM transfers are buffered in eight-pixel (RGBA16) span segments. Charge bus
 * traffic independently of pixel throughput, allowing the two to overlap.
 * The 15-clock span startup, 5-clock transaction and 3-clock turnaround
 * overheads are estimates checked against Thar0/RDP-Timing-Tests, not
 * constants fitted to a game.
 * Other RCP clients and read-response buffering remain unmodeled. Uniform
 * command-derived rejection suppresses stores while retaining earlier reads. */
static uint64_t rdp_timing_span(const struct rdp_timing* t, int y, int left, int right)
{
    if (right <= left || !rdp_timing_keeps_row(t, y)) return 0;
    uint64_t pixels = (unsigned)(right - left);
    if (t->cycle_type == 2) return (pixels + 3) / 4;
    uint64_t bits_per_pixel = 4u << (t->fb_size & 3);
    uint64_t first = (uint64_t)t->fb_address * 8 +
        ((uint64_t)y * t->fb_width + (unsigned)left) * bits_per_pixel;
    uint64_t color = rdp_timing_bus_words(first, pixels * bits_per_pixel);
    if (t->cycle_type == 3) return color;
    unsigned op = (t->words[0] >> 24) & 63;
    unsigned z = (op >= 8 && op <= 15 && (op & 1)) || (t->modes & 4);
    unsigned color_read = (t->modes >> 6) & 1;
    unsigned depth_read = z && (t->modes & 0x10);
    unsigned depth_write = z && (t->modes & 0x20) && !t->reject;
    unsigned color_write = !t->reject;
    uint64_t segments = ((unsigned)right + 7) / 8 - (unsigned)left / 8;
    uint64_t bus = color * (color_write + color_read);
    uint64_t depth = (uint64_t)t->z_address * 8 +
        ((uint64_t)y * t->fb_width + (unsigned)left) * 16;
    bus += rdp_timing_bus_words(depth, pixels * 16) * (depth_read + depth_write);
    bus += segments * 5 * (color_write + color_read + depth_read + depth_write);
    /* Reads precede writes in a span segment. Changing bus direction costs
     * time even when color/depth occupy separate banks. If the last read is
     * depth, its transition to the color write can overlap the row change;
     * count that boundary once rather than adding two turnaround penalties.
     * The three-clock estimate comes from console color read/write tests. */
    unsigned turnarounds = color_write && (color_read || depth_read);
    /* Alternating color/depth in the same 1 MiB bank closes its open
     * 2 KiB row. Account for the remaining boundaries in each segment. */
    if ((depth_read || depth_write) && (first >> 23) == (depth >> 23) &&
        (first >> 14) != (depth >> 14)) {
        unsigned switches = (color_read && depth_read) + depth_read + depth_write;
        turnarounds += switches - depth_read;
        /* With both stores suppressed, the final Z read switches back to
         * the color row for the following segment. The old read/write
         * boundary formula omitted these read-to-read row changes. */
        if (!color_write && !depth_write && color_read && depth_read && segments > 1)
            bus += (segments - 1) * 3;
    }
    bus += segments * 3 * turnarounds;
    uint64_t raster = pixels * (t->cycle_type + 1);
    /* Aggregate read-response budgets, estimated from moving NTSC scene
     * comparisons: 32/48 RCP clocks per read stream in one/two-cycle mode.
     * Short spans cannot hide the entire buffered memory/pipeline response
     * behind pixel work. Long spans hide it, retaining the console-checked
     * rectangle throughputs above. These are estimates, not measured DRAM
     * latencies; no ROM identity, frame position or host speed selects them. */
    uint64_t response = (color_read + depth_read) * (32u + 16u * t->cycle_type);
    uint64_t uncovered = response > raster ? response - raster : 0;
    RDP_TIMING_OBSERVE_RESPONSE(t, uncovered);
    unsigned startup = t->reject ? 4u + 2u * t->cycle_type : 15u;
    return startup + (bus > raster ? bus : raster) + uncovered;
}

/* Fill spans repeat their bus alignment within 16 rows even for 4-bit
 * images. Other render modes also depend on color/depth bank addresses. */
static uint64_t rdp_timing_rectangle(const struct rdp_timing* t,
    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    if (x1 <= x0 || y1 <= y0) return 0;
    if (t->cycle_type == 2) {
        unsigned rows = y1 - y0;
        if (t->scissor_field & 2) {
            if (!rdp_timing_keeps_row(t, y0)) ++y0;
            rows = y0 < y1 ? (y1 - y0 + 1) / 2 : 0;
        }
        return (uint64_t)((x1 - x0 + 3) / 4) * rows;
    }
    if (t->cycle_type < 2) {
        uint64_t work = 0;
        for (unsigned y = y0; y < y1; ++y)
            work += rdp_timing_span(t, y, x0, x1);
        return work;
    }
    unsigned period = 16u >> (t->fb_size & 3);
    unsigned rows = y1 - y0, tail = rows % period;
    uint64_t repeated = 0, remainder = 0;
    for (unsigned i = 0; i < period; ++i) {
        uint64_t span = rdp_timing_span(t, y0 + i, x0, x1);
        repeated += span;
        if (i < tail) remainder += span;
    }
    return repeated * (rows / period) + remainder;
}

/* A linear pair of edges has its extrema at the endpoints. If its order
 * crosses within these (at most four) quarter rows, retain the discrete
 * walk so rejecting crossed samples has exactly the original behavior. */
static void rdp_timing_edge_bounds(int64_t a, int64_t b, int64_t da, int64_t db,
    int samples, int flip, int64_t* left, int64_t* right)
{
    int64_t l = flip ? a : b, r = flip ? b : a;
    int64_t dl = flip ? da : db, dr = flip ? db : da;
    int64_t last_l = l + dl * (samples - 1), last_r = r + dr * (samples - 1);
    if (l <= r && last_l <= last_r) {
        l = (l < last_l ? l : last_l) >> 16;
        r = ((r > last_r ? r : last_r) >> 16) + 1;
        if (l < *left) *left = l;
        if (r > *right) *right = r;
    } else if (l <= r || last_l <= last_r) {
        for (int q = 0; q < samples; ++q, l += dl, r += dr) {
            if (l > r) continue;
            int64_t lp = l >> 16, rp = (r >> 16) + 1;
            if (lp < *left) *left = lp;
            if (rp > *right) *right = rp;
        }
    }
}

/* Native raster work, independent of host rendering speed/upscaling. Costs
 * follow the documented 1/2-cycle pixel and 64-bit fill/copy throughputs.
 * Edge/coverage and RDRAM arbitration remain estimates, not a cycle-accurate
 * rasterizer. A packet can arrive over several DPC END writes. */
static uint64_t rdp_timing_clocks(struct rdp_timing* t)
{
    const uint32_t* w = t->words;
    unsigned op = (w[0] >> 24) & 63;
    uint64_t work = 0;
    t->reject = 0;
    if (op == 0x3a) t->primitive_color = w[1];
    if (op == 0x3b) t->environment_color = w[1];
    if (op == 0x39) t->blend_color = w[1];
    if (op == 0x37) t->fill_color = w[1];
    if (op == 0x2e) t->primitive_depth = w[1];
    if (op == 0x3c) { t->combine_hi = w[0]; t->combine_lo = w[1]; }
    if (op == 0x2f) {
        t->cycle_type = (w[0] >> 20) & 3; t->modes = w[1];
        t->atomic_primitive = (w[0] >> 23) & 1;
    }
    if (op == 0x3e) t->z_address = w[1] & 0xffffff;
    if (op == 0x3f) {
        t->fb_size = (w[0] >> 19) & 3;
        t->fb_width = (w[0] & 1023) + 1;
        t->fb_address = w[1] & 0xffffff;
    }
    if (op == 0x3d) {
        t->tex_size = (w[0] >> 19) & 3;
        t->tex_width = (w[0] & 1023) + 1;
        t->tex_address = w[1] & 0xffffff;
    }
    if (op == 0x2d) {
        t->left = (w[0] >> 12) & 4095; t->top = w[0] & 4095;
        t->right = (w[1] >> 12) & 4095; t->bottom = w[1] & 4095;
        t->scissor_field = (w[1] >> 24) & 3;
    }
    if (op >= 8 && op <= 15) {
        t->reject = rdp_timing_alpha_result(t) == 1;
        int yh = rdp_timing_signed(w[1], 14), ym = rdp_timing_signed(w[1] >> 16, 14);
        int yl = rdp_timing_signed(w[0], 14);
        int first = yh >> 2, last = (yl + 3) >> 2;
        int clip_first = t->top >> 2, clip_last = (t->bottom + 3) >> 2;
        if (first < clip_first) first = clip_first;
        if (last > clip_last) last = clip_last;
        int64_t xh = rdp_timing_signed(w[4], 28), xm = rdp_timing_signed(w[6], 28);
        int64_t xl = rdp_timing_signed(w[2], 28);
        /* Match the rasterizer's signed 30-bit edge increments. The upper
         * two packet bits do not turn a negative slope into positive work. */
        int32_t dh = rdp_timing_signed(w[5], 30), dm = rdp_timing_signed(w[7], 30);
        int32_t dl = rdp_timing_signed(w[3], 30);
        int flip = (w[0] >> 23) & 1;
        int64_t ah = xh & ~INT64_C(1), am = xm & ~INT64_C(1), al = xl & ~INT64_C(1);
        int64_t sh = (dh >> 2) & ~1, sm = (dm >> 2) & ~1, sl = (dl >> 2) & ~1;
        for (int y = first; y < last; ++y) {
            if (!rdp_timing_keeps_row(t, y)) continue;
            /* The rasterizer visits the union of all four quarter-row edges,
             * including its last pixel even when that pixel has zero coverage.
             * A midpoint sample drops work on thin or steep triangles. */
            int lo = y * 4, hi = lo + 4;
            if (lo < yh) lo = yh;
            if (lo < (int)t->top) lo = t->top;
            if (hi > yl) hi = yl;
            if (hi > (int)t->bottom) hi = t->bottom;
            if (hi <= lo) continue;
            int64_t left = INT64_MAX, right = INT64_MIN;
            int upper = hi < ym ? hi : ym;
            if (lo < upper)
                rdp_timing_edge_bounds(ah + sh * (lo - (yh & ~3)),
                    am + sm * (lo - (yh & ~3)), sh, sm, upper - lo, flip, &left, &right);
            int lower = lo > ym ? lo : ym;
            if (lower < hi)
                rdp_timing_edge_bounds(ah + sh * (lower - (yh & ~3)),
                    al + sl * (lower - ym), sh, sl, hi - lower, flip, &left, &right);
            if (left < (int64_t)(t->left >> 2)) left = t->left >> 2;
            if (right > (int64_t)((t->right + 3) >> 2)) right = (t->right + 3) >> 2;
            if (right > left) {
                RDP_TIMING_OBSERVE_SPAN(t, right-left, 1);
                work += rdp_timing_span(t, y, (int)left, (int)right);
            }
        }
    } else if (op == 0x24 || op == 0x25 || op == 0x36) {
        uint32_t left = (w[1] >> 12) & 4095, top = w[1] & 4095;
        uint32_t right = (w[0] >> 12) & 4095, bottom = w[0] & 4095;
        if (t->cycle_type >= 2) { right = (right | 3) + 1; bottom = (bottom | 3) + 1; }
        if (left < t->left) left = t->left;
        if (top < t->top) top = t->top;
        if (right > t->right) right = t->right;
        if (bottom > t->bottom) bottom = t->bottom;
        if (right > left && bottom > top) {
            int x0 = left >> 2, x1 = (right + 3) >> 2;
            unsigned rows = ((bottom + 3) >> 2) - (top >> 2);
            if (t->scissor_field & 2) {
                unsigned y0 = top >> 2;
                if (!rdp_timing_keeps_row(t, y0)) ++y0;
                rows = y0 < ((bottom+3)>>2) ? (((bottom+3)>>2)-y0+1)/2 : 0;
            }
            RDP_TIMING_OBSERVE_SPAN(t, x1-x0, rows);
            int alpha = rdp_timing_alpha_result(t);
            int depth = rdp_timing_depth_result(t, x1, (bottom + 3) >> 2);
            t->reject = t->cycle_type < 2 && (alpha == 1 || depth == 1);
            work = rdp_timing_rectangle(t, x0, top >> 2, x1, (bottom + 3) >> 2);
            uint64_t bytes = (uint64_t)t->fb_width * ((bottom + 3) >> 2) * (4u << t->fb_size) / 8;
            if (!t->reject) rdp_timing_invalidate_range(t, t->fb_address, bytes);
            if (t->cycle_type == 3 && t->fb_size == 2 && !left && !top &&
                !(t->scissor_field & 2) && !(bottom & 3) &&
                (unsigned)x1 == t->fb_width && !(t->fill_color & 3) &&
                (t->fill_color >> 16) == (t->fill_color & 0xffff))
                rdp_timing_record_uniform(t, t->fb_address, t->fb_width, (bottom + 3) >> 2,
                    rdp_timing_decompress_z(t->fill_color));
            if (t->cycle_type < 2 && (t->modes & 0x24u) == 0x24u && !t->reject) {
                rdp_timing_invalidate_range(t, t->z_address, (uint64_t)t->fb_width * ((bottom + 3) >> 2) * 2);
                if (!left && !top && !(t->scissor_field & 2) && !((right | bottom) & 3) &&
                    !(t->modes & (8u | 0x3000u)) &&
                    (unsigned)x1 == t->fb_width && alpha == 0 && depth == 0 &&
                    !(t->primitive_depth & 0xffff))
                    rdp_timing_record_uniform(t, t->z_address, t->fb_width, (bottom + 3) >> 2,
                        rdp_timing_quantize_z(((t->primitive_depth >> 16) & 0x7fff) << 3));
            }
        }
    }
    if (op >= 8 && op <= 15 && !t->reject) {
        rdp_timing_invalidate_range(t, t->fb_address, (uint64_t)t->fb_width * ((t->bottom + 3) >> 2) * (4u << t->fb_size) / 8);
        if (t->modes & 0x20) rdp_timing_invalidate_range(t, t->z_address,
            (uint64_t)t->fb_width * ((t->bottom + 3) >> 2) * 2);
    }
    /* Loads use texture-image size, not framebuffer size or cycle type.
     * LoadBlock coordinates are integers; Tile/TLUT coordinates are 10.2.
     * TLUT expands each 16-bit palette entry to one 64-bit TMEM word.
     * Native 4-bit image loads crash the RDP; supported 4-bit textures are
     * loaded through an 8-/16-bit texture-image view instead. */
    if ((op == 0x30 || op == 0x33 || op == 0x34) && t->tex_size) {
        unsigned sl = (w[0] >> 12) & 4095, tl = w[0] & 4095;
        unsigned sh = (w[1] >> 12) & 4095, th = w[1] & 4095;
        unsigned bpp = 4u << t->tex_size;
        if (op == 0x33) {
            if (sh >= sl) {
                uint64_t first = (uint64_t)t->tex_address * 8 +
                    ((uint64_t)tl * t->tex_width + sl) * bpp;
                work = rdp_timing_bus_words(first, (uint64_t)(sh - sl + 1) * bpp);
            }
        } else {
            sl >>= 2; tl >>= 2; sh >>= 2; th >>= 2;
            if (sh >= sl && th >= tl && (op != 0x30 || th == tl)) {
                if (op == 0x30 && t->tex_size == 2) work = sh - sl + 1;
                else {
                    unsigned period = 16u >> t->tex_size;
                    unsigned rows = th - tl + 1, tail = rows % period;
                    uint64_t repeated = 0, remainder = 0;
                    for (unsigned i = 0; i < period; ++i) {
                        uint64_t first = (uint64_t)t->tex_address * 8 +
                            ((uint64_t)(tl + i) * t->tex_width + sl) * bpp;
                        uint64_t span = rdp_timing_bus_words(first, (uint64_t)(sh - sl + 1) * bpp);
                        repeated += span;
                        if (i < tail) remainder += span;
                    }
                    work = repeated * (rows / period) + remainder;
                }
            }
        }
    }
    /* No-memory alpha failure: 4/6 clocks per span plus eleven drain clocks.
     * Including the rectangle delivery clock gives the measured twelve-clock
     * primitive overhead, independently of width, height or ROM identity. */
    if (work && t->reject && !(t->modes & 0x50u)) work += 11;
    if (work && t->atomic_primitive &&
        ((op >= 8 && op <= 15) || op == 0x24 || op == 0x25 || op == 0x36))
        work += 35; /* SDK specifies 30-40 idle clocks per primitive. */
    /* Pipeline synchronization has a cost even for an empty display list. */
    if (op == 0x27) work += 50;
    if (op == 0x28) work += 33;
    if (op == 0x26) work += 25; /* SyncLoad; 0x31 is reserved. */
    uint64_t clocks = t->length / 2 + work;
    RDP_TIMING_OBSERVE_COMMAND(t, op, work);
    /* Raster/transfer work is serialized with packet delivery here. Pipeline
     * overlap between commands and contention with other RCP clients remain
     * unmodeled; span memory traffic overlaps its pixel pipeline above. */
    return clocks;
}

/* Standalone command cost; queued work carries its rounding in finish(). */
static uint32_t rdp_timing_command(struct rdp_timing* t)
{
    uint64_t ticks = (rdp_timing_clocks(t) * 3 + 3) / 4;
    return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

/* Submission-local milestone, never part of serialized timing state. */
struct rdp_timing_sync {
    int seen;
    int64_t deadline;
};

static void rdp_timing_finish(struct rdp_timing* t, int64_t submitted,
    struct rdp_timing_sync* sync)
{
    /* Once submission reaches the rounded deadline, the preceding work
     * has drained. Otherwise retain the 3:4 clock conversion remainder
     * across commands and fragmented DPC windows, rather than rounding
     * every command up independently. FullSync observes the rounded time. */
    if (t->deadline <= submitted) {
        t->deadline = submitted;
        t->clock_overhang = 0;
    }
    int64_t begin = t->deadline >= INT64_MIN / 4 && t->deadline <= INT64_MAX / 4 ?
        t->deadline * 4 - t->clock_overhang : INT64_MAX;
    uint64_t quarters = rdp_timing_clocks(t) * 3 - t->clock_overhang;
    uint64_t ticks = (quarters + 3) / 4;
    if (ticks > (uint64_t)INT64_MAX || t->deadline > INT64_MAX - (int64_t)ticks) {
        t->deadline = INT64_MAX;
        t->clock_overhang = 0;
    } else {
        t->deadline += (int64_t)ticks;
        t->clock_overhang = (uint32_t)(ticks * 4 - quarters);
    }
    if (t->observe_busy && begin != INT64_MAX && t->deadline <= INT64_MAX / 4) {
        unsigned op = (t->words[0] >> 24) & 63;
        unsigned primitive = (op >= 8 && op <= 15) || op == 0x24 || op == 0x25 || op == 0x36;
        unsigned sync_op = op >= 0x26 && op <= 0x29;
        unsigned load = op == 0x30 || op == 0x33 || op == 0x34;
        unsigned mask = 1u | ((primitive || sync_op || load) ? 2u : 0u) | (load ? 4u : 0u);
        int64_t end = t->deadline * 4 - t->clock_overhang;
        if (primitive && t->reject && !(t->modes & 0x50u)) {
            t->observe_busy(t->busy_context, begin, end, 1);
            t->observe_busy(t->busy_context, begin, end - (t->cycle_type + 1) * 3, 2);
        } else t->observe_busy(t->busy_context, begin, end, mask);
    }
    if (sync && !sync->seen && ((t->words[0] >> 24) & 63) == 0x29) {
        sync->seen = 1;
        sync->deadline = t->deadline;
    }
    RDP_TIMING_OBSERVE_FINISH(t, submitted);
    t->pos = 0;
}

static void rdp_timing_word(struct rdp_timing* t, uint32_t word, int64_t submitted)
{
    static const unsigned triangle_words[8] = {8,12,24,28,24,28,40,44};
    if (!t->pos) {
        unsigned op = (word >> 24) & 63;
        t->length = op >= 8 && op <= 15 ? triangle_words[op - 8] :
                    (op == 0x24 || op == 0x25 ? 4 : 2);
    }
    if (t->pos < 8) t->words[t->pos] = word;
    if (++t->pos == t->length) {
        rdp_timing_finish(t, submitted, NULL);
    }
}

/* The estimator needs the first eight triangle words, not the shade/texture/Z
 * payload. Consume contiguous command windows by packet, retaining the same
 * framing and partial-packet state as the scalar feeder. */
static void rdp_timing_words_sync(struct rdp_timing* t, const uint32_t* words,
    uint32_t count, int64_t submitted, struct rdp_timing_sync* sync)
{
    while (count) {
        if (!t->pos) {
            rdp_timing_word(t, *words++, submitted);
            --count;
        }
        uint32_t n = t->length - t->pos;
        if (n > count) n = count;
        if (t->pos < 8) {
            uint32_t keep = 8 - t->pos;
            if (keep > n) keep = n;
            memcpy(t->words + t->pos, words, keep * sizeof(*words));
        }
        t->pos += n;
        words += n;
        count -= n;
        if (t->pos == t->length) rdp_timing_finish(t, submitted, sync);
    }
}

static void rdp_timing_words(struct rdp_timing* t, const uint32_t* words,
    uint32_t count, int64_t submitted)
{
    rdp_timing_words_sync(t, words, count, submitted, NULL);
}
#endif
