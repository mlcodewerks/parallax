#pragma once
#include "rdp_types.hpp"

// Memory coverage is a constant seven with image reads disabled. In that case
// only explicit memory-colour mux inputs require a destination snapshot.
inline bool rdp_reads_color_memory(const RDPState &s) {
    const unsigned cycle = (s.other_modes >> 52) & 3;
    if (cycle >= 2)
        return false;
    if (s.other_modes & 0x40u)
        return true;
    auto reads = [&](unsigned p, unsigned m) {
        return ((s.other_modes >> p) & 3) == 1 || ((s.other_modes >> m) & 3) == 1;
    };
    return reads(30, 22) || (cycle == 1 && reads(28, 20));
}

inline bool rdp_rgb_difference_zero(unsigned a, unsigned b) {
    // A/B selectors 0..5 share their inputs; both banks return zero at 8+.
    // Selectors 6/7 differ between banks (one/noise versus key/K4).
    return (a == b && a < 6) || (a >= 8 && b >= 8);
}

// One-cycle mode evaluates the second combiner mux. Copy consumes TEXEL0;
// two-cycle mode swaps TEXEL0/TEXEL1, so retain both fetches conservatively.
inline bool rdp_uses_second_texel(const RDPState &s) {
    const unsigned cycle = (s.other_modes >> 52) & 3;
    if (cycle == 1)
        return true;
    if (cycle != 0)
        return false;
    const auto w = s.combine;
    const unsigned a = (w >> 37) & 15, b = (w >> 24) & 15, c = (w >> 32) & 31;
    const unsigned aa = (w >> 21) & 7, ab = (w >> 3) & 7, ac = (w >> 18) & 7;
    return ((w >> 6) & 7) == 2 || (w & 7) == 2 ||
           (!rdp_rgb_difference_zero(a, b) && c < 16 && (a == 2 || b == 2 || c == 2 || c == 9)) ||
           (aa != ab && ac != 7 && (aa == 2 || ab == 2 || ac == 2));
}

inline bool rdp_uses_lod_fraction(const RDPState &s) {
    const unsigned cycle = (s.other_modes >> 52) & 3;
    if (cycle >= 2)
        return false;
    const auto w = s.combine;
    return (((w >> 32) & 31) == 13 && !rdp_rgb_difference_zero((w >> 37) & 15, (w >> 24) & 15)) ||
           (((w >> 18) & 7) == 0 && ((w >> 21) & 7) != ((w >> 3) & 7)) ||
           (cycle == 1 &&
            ((((w >> 47) & 31) == 13 && !rdp_rgb_difference_zero((w >> 52) & 15, (w >> 28) & 15)) ||
             (((w >> 41) & 7) == 0 && ((w >> 44) & 7) != ((w >> 12) & 7))));
}
