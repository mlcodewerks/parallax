/* MusyX v1/v2 preflight. Reject unsupported task layouts before any HLE write;
 * the native microcode can handle its persistent DMEM/overlay context. */
#ifndef MUSYX_VALIDATION_H
#define MUSYX_VALIDATION_H
#include <stdint.h>
#include <string.h>
#define MUSYX_RAM_BYTES 0x800000u
#ifdef MSB_FIRST
#define MUSYX_S16 0
#define MUSYX_S8 0
#else
#define MUSYX_S16 2
#define MUSYX_S8 3
#endif
static inline int musyx_range(uint32_t p, uint32_t bytes, unsigned alignment)
{
    p &= 0xffffff;
    return !(p & (alignment - 1)) && p < MUSYX_RAM_BYTES && bytes <= MUSYX_RAM_BYTES - p;
}
static inline uint32_t musyx_word(const unsigned char *ram, uint32_t p)
{
    uint32_t v; memcpy(&v, ram + (p & 0xffffff), 4); return v;
}
static inline unsigned musyx_half(const unsigned char *ram, uint32_t p)
{
    uint16_t v; memcpy(&v, ram + ((p & 0xffffff) ^ MUSYX_S16), 2); return v;
}
static inline unsigned musyx_byte(const unsigned char *ram, uint32_t p)
{
    return ram[(p & 0xffffff) ^ MUSYX_S8];
}
static inline int musyx_cat_supported(const unsigned char *ram, uint32_t p,
                                      unsigned capacity, unsigned alignment)
{
    unsigned a = musyx_half(ram, p + 8), b = musyx_half(ram, p + 10);
    return !(a & (alignment - 1)) && !(b & (alignment - 1)) &&
        a + b <= capacity && (!a || musyx_range(musyx_word(ram, p), a, alignment)) &&
        (!b || musyx_range(musyx_word(ram, p + 4), b, alignment));
}
static inline int musyx_voice_supported(const unsigned char *ram, uint32_t v)
{
    unsigned frames = musyx_byte(ram, v + 0x3c);
    unsigned frames2 = musyx_byte(ram, v + 0x3d);
    unsigned skip = musyx_byte(ram, v + 0x3e);
    unsigned count, count2, base;
    if (frames) {
        if (frames > 16 || frames2 > 16 ||
            !musyx_range(musyx_word(ram, v + 0x40), 256, 2) ||
            !musyx_cat_supported(ram, v + 0x24, 320, 1) ||
            (frames2 && !musyx_cat_supported(ram, v + 0x30, 320, 1))) return 0;
        count = frames * 32; count2 = frames2 * 32; skip &= 31;
        /* The table has eight predictors. Check headers at the same positions
         * visited by adpcm_decode_frames, including its alternate frame gap. */
        for (unsigned seg = 0; seg < (frames2 ? 2u : 1u); ++seg) {
            uint32_t cat = v + 0x24 + seg * 12;
            unsigned n = seg ? frames2 : frames, gap = musyx_byte(ram, v + 0x3e + seg) >= 32;
            unsigned offset = 8 + (gap ? 16 : 0);
            unsigned len1 = musyx_half(ram, cat + 8), len = len1 + musyx_half(ram, cat + 10);
            for (unsigned i = 0; i < n; ++i) {
                if (offset + 16 > len) return 0;
                uint32_t p = offset < len1 ? musyx_word(ram, cat) + offset :
                    musyx_word(ram, cat + 4) + offset - len1;
                if (musyx_byte(ram, p) >= 0x80) return 0;
                offset += gap ? 24 : 16; gap = !gap;
            }
        }
    } else {
        count = (musyx_half(ram, v + 0x40) + skip + 3) & ~3u;
        count2 = musyx_half(ram, v + 0x42);
        if (!count || count > 512 || count2 > 512 ||
            !musyx_cat_supported(ram, v + 0x24, count * 2, 2) ||
            (count2 && !musyx_cat_supported(ram, v + 0x30, count2 * 2, 2))) return 0;
        count = (musyx_half(ram, v + 0x40) + skip + 3) & ~3u;
        count2 = count2 ? (musyx_half(ram, v + 0x38) + musyx_half(ram, v + 0x3a)) / 2 : 0;
    }
    base = 512 - count;
    int position = base + skip + musyx_half(ram, v + 0x4e);
    int end = base + musyx_half(ram, v + 0x48);
    unsigned restart = musyx_half(ram, v + 0x4a);
    int loop = (restart & 0x7fff) + ((restart & 0x8000) ? 0 : base);
    uint32_t phase = musyx_half(ram, v + 0x20), step = musyx_half(ram, v + 0x22) << 4;
    unsigned loaded = frames ? count : (musyx_half(ram, v + 0x2c) + musyx_half(ram, v + 0x2e)) / 2;
    for (unsigned i = 0; i < 192; ++i) {
        position += phase >> 16; phase = (phase & 0xffff) + step;
        if (position >= end) position = loop + position - end;
        if (position < 0 || position > 508 ||
            !((position >= (int)base && position + 4 <= (int)(base + loaded)) ||
              position + 4 <= (int)count2)) return 0;
    }
    return 1;
}
static inline int musyx_task_supported(const unsigned char *ram, uint32_t sfd, uint32_t count, int v2)
{
    /* Both versions carry 32 voices. V1 persists cc0 and FIR history through
     * state +0x297 and emits stereo; v2 stores three planar subframes. */
    unsigned voices = v2 ? 0x28 : 0x10;
    unsigned stride = voices + 32 * 0x50;
    sfd &= 0xffffff;
    if (!count || !musyx_range(sfd, stride, 4) || count > (MUSYX_RAM_BYTES - sfd) / stride) return 0;
    for (uint32_t sf = 0; sf < count; ++sf, sfd += stride) {
        if (!musyx_range(musyx_word(ram, sfd + 8), v2 ? 0x118 : 0x298, 4) ||
            (v2 && musyx_word(ram, sfd + 0x10))) return 0; /* unimplemented auxiliary stage */
        if (v2 && musyx_byte(ram, sfd + 0x15) &&
            !musyx_range(musyx_word(ram, sfd + 0x24), 32, 2)) return 0;
        unsigned i;
        for (i = 0; i < 32; ++i) {
            uint32_t v = sfd + voices + i * 0x50;
            int empty = !musyx_half(ram, v + 0x2c);
            if ((empty && i != 0) || (!empty && !musyx_voice_supported(ram, v))) return 0;
            uint32_t out = musyx_word(ram, v + 0x44);
            if (empty || out) { if (!musyx_range(out, v2 ? 1152 : 768, 4)) return 0; break; }
        }
        if (i == 32) return 0;
        uint32_t fx = musyx_word(ram, sfd + 12);
        if (fx) {
            if (!musyx_range(fx, 0x48, 4)) return 0;
            unsigned len = musyx_word(ram, fx + 4), taps = musyx_half(ram, fx + 8);
            unsigned pos = musyx_half(ram, sfd + 2) * 192;
            if (len < 192 || len > MUSYX_RAM_BYTES / 2 || pos > len - 192 || taps > 8 ||
                !musyx_range(musyx_word(ram, fx), len * 2, 2)) return 0;
            for (unsigned k = 0; k < taps; ++k)
                if (musyx_word(ram, fx + 12 + 4*k) > len) return 0;
        }
        unsigned mask = v2 ? musyx_half(ram, sfd + 0x16) : 0;
        if (mask) {
            uint32_t entries = musyx_word(ram, sfd + 0x18);
            if ((mask & ~255u) || !musyx_range(entries, 64, 4) ||
                !musyx_range(musyx_word(ram, sfd + 0x1c), 384, 2) ||
                !musyx_range(musyx_word(ram, sfd + 0x20), 768, 4)) return 0;
            for (unsigned k = 0; k < 8; ++k)
                if ((mask & (1u << k)) && !musyx_range(musyx_word(ram, entries + k*8), 1152, 2)) return 0;
        }
    }
    return 1;
}
static inline int musyx_v1_supported(const unsigned char *ram, uint32_t sfd, uint32_t count)
{
    return musyx_task_supported(ram, sfd, count, 0);
}
static inline int musyx_v2_supported(const unsigned char *ram, uint32_t sfd, uint32_t count)
{
    return musyx_task_supported(ram, sfd, count, 1);
}
#undef MUSYX_S16
#undef MUSYX_S8
#undef MUSYX_RAM_BYTES
#endif
