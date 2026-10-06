#ifndef WTFN64_RDP_TIMING_H
#define WTFN64_RDP_TIMING_H

#include <stdint.h>
#include <string.h>

/* Reserved extra-state space before the CPU-cache extension at 4096. */
#define RDP_TIMING_STATE_OFFSET 4000

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
};

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

static uint64_t rdp_timing_span(const struct rdp_timing* t, int y, int left, int right)
{
    if (right <= left || !rdp_timing_keeps_row(t, y)) return 0;
    uint64_t pixels = (unsigned)(right - left);
    if (t->cycle_type < 2) return pixels * (t->cycle_type + 1);
    if (t->cycle_type == 2) return (pixels + 3) / 4;
    uint64_t bits_per_pixel = 4u << (t->fb_size & 3);
    uint64_t first = (uint64_t)t->fb_address * 8 +
        ((uint64_t)y * t->fb_width + (unsigned)left) * bits_per_pixel;
    return rdp_timing_bus_words(first, pixels * bits_per_pixel);
}

/* Rectangle spans repeat their bus alignment within 16 pixels/rows even
 * for 4-bit images. Bound host work instead of looping over tall rectangles. */
static uint64_t rdp_timing_rectangle(const struct rdp_timing* t,
    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    if (x1 <= x0 || y1 <= y0) return 0;
    if (t->cycle_type < 3) {
        unsigned rows = y1 - y0;
        if (t->scissor_field & 2) {
            if (!rdp_timing_keeps_row(t, y0)) ++y0;
            rows = y0 < y1 ? (y1 - y0 + 1) / 2 : 0;
        }
        uint64_t span = t->cycle_type == 2 ? (x1 - x0 + 3) / 4 :
            (uint64_t)(x1 - x0) * (t->cycle_type + 1);
        return span * rows;
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

/* Native raster work, independent of host rendering speed/upscaling. Costs
 * follow the documented 1/2-cycle pixel and 64-bit fill/copy throughputs.
 * Edge/coverage and RDRAM arbitration remain estimates, not a cycle-accurate
 * rasterizer. A packet can arrive over several DPC END writes. */
static uint64_t rdp_timing_clocks(struct rdp_timing* t)
{
    const uint32_t* w = t->words;
    unsigned op = (w[0] >> 24) & 63;
    uint64_t work = 0;
    if (op == 0x2f) { t->cycle_type = (w[0] >> 20) & 3; t->modes = w[1]; }
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
        for (int y = first; y < last; ++y) {
            if (!rdp_timing_keeps_row(t, y)) continue;
            /* For a partial first/last row, sample inside its live quarters. */
            int lo = y * 4, hi = lo + 4;
            if (lo < yh) lo = yh;
            if (lo < (int)t->top) lo = t->top;
            if (hi > yl) hi = yl;
            if (hi > (int)t->bottom) hi = t->bottom;
            if (hi <= lo) continue;
            int q = (lo + hi) / 2;
            int64_t a = xh + (int64_t)dh * (q - (yh & ~3)) / 4;
            int64_t b = q < ym ? xm + (int64_t)dm * (q - (yh & ~3)) / 4
                               : xl + (int64_t)dl * (q - ym) / 4;
            /* The major-edge side is encoded in the packet. Crossed edges
             * produce no span, rather than an artificial absolute width. */
            int flip = (w[0] >> 23) & 1;
            if ((flip ? a : b) >= (flip ? b : a)) continue;
            int64_t left = (flip ? a : b) >> 16, right = ((flip ? b : a) + 65535) >> 16;
            if (left < (int64_t)(t->left >> 2)) left = t->left >> 2;
            if (right > (int64_t)((t->right + 3) >> 2)) right = (t->right + 3) >> 2;
            work += rdp_timing_span(t, y, (int)left, (int)right);
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
            work = rdp_timing_rectangle(t, x0, top >> 2, x1, (bottom + 3) >> 2);
        }
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
    uint64_t clocks = t->length / 2 + work;
    /* Raster/transfer work is serialized with packet delivery here. Pipeline
     * overlap, row setup latency and RDRAM arbitration remain unmodeled. */
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
    uint64_t quarters = rdp_timing_clocks(t) * 3 - t->clock_overhang;
    uint64_t ticks = (quarters + 3) / 4;
    if (ticks > (uint64_t)INT64_MAX || t->deadline > INT64_MAX - (int64_t)ticks) {
        t->deadline = INT64_MAX;
        t->clock_overhang = 0;
    } else {
        t->deadline += (int64_t)ticks;
        t->clock_overhang = (uint32_t)(ticks * 4 - quarters);
    }
    if (sync && !sync->seen && ((t->words[0] >> 24) & 63) == 0x29) {
        sync->seen = 1;
        sync->deadline = t->deadline;
    }
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
