#pragma once

// Shared GLSL 1.00/3.30 VI reconstruction. The caller supplies vi_fetch,
// vi_scale, vi_aa, vi_aa_full, vi_resampling, vi_restore, vi_divot and vi_precomputed; no integer samplers
// or arrays. Reconstruction can run once per source pixel before resampling.
namespace glrdp_vi {
inline constexpr const char *filter = R"(
void vi_extreme(vec4 tap, inout vec3 hi, inout vec3 hi2, inout vec3 lo, inout vec3 lo2) {
    if (tap.a < 0.99)
        return;
    vec3 rgb = floor(tap.rgb * 255.0 + 0.5);
    hi2 = max(hi2, min(hi, rgb));
    hi = max(hi, rgb);
    lo2 = min(lo2, max(lo, rgb));
    lo = min(lo, rgb);
}
vec3 vi_restore_delta(vec2 p, vec3 center) {
    return sign(floor(vi_fetch(p).rgb * 255.0 / 8.0 + 0.01) - center);
}
vec4 vi_filter_tap(vec2 p) {
    vec4 c = vi_fetch(p);
    if (vi_precomputed())
        return c;
    if (vi_aa() > 1.0)
        c.a = 1.0;
    float scale = vi_scale();
    vec3 rgb = floor(c.rgb * 255.0 + 0.5);
    if (c.a < 0.99) {
        vec3 hi = rgb, hi2 = vec3(0.0), lo = rgb, lo2 = vec3(255.0);
        vi_extreme(vi_fetch(p + vec2(-1.0, -1.0) * scale), hi, hi2, lo, lo2);
        vi_extreme(vi_fetch(p + vec2(1.0, -1.0) * scale), hi, hi2, lo, lo2);
        if (vi_aa_full()) {
            vi_extreme(vi_fetch(p + vec2(-2.0, 0.0) * scale), hi, hi2, lo, lo2);
            vi_extreme(vi_fetch(p + vec2(2.0, 0.0) * scale), hi, hi2, lo, lo2);
        }
        vi_extreme(vi_fetch(p + vec2(-1.0, 1.0) * scale), hi, hi2, lo, lo2);
        vi_extreme(vi_fetch(p + vec2(1.0, 1.0) * scale), hi, hi2, lo, lo2);
        // Angrylion retains the center when it is a unique extreme.
        hi2 = mix(hi2, rgb, step(abs(hi - rgb), vec3(0.0)));
        lo2 = mix(lo2, rgb, step(abs(lo - rgb), vec3(0.0)));
        float coefficient = 8.0 - floor(c.a * 8.0 + 0.5);
        rgb = mod(rgb + floor(((lo2 + hi2 - 2.0 * rgb) * coefficient + 4.0) / 8.0), 256.0);
    } else if (vi_restore()) {
        vec3 center = floor(rgb / 8.0);
        rgb += vi_restore_delta(p + vec2(-1.0, -1.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(0.0, -1.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(1.0, -1.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(-1.0, 1.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(0.0, 1.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(1.0, 1.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(-1.0, 0.0) * scale, center);
        rgb += vi_restore_delta(p + vec2(1.0, 0.0) * scale, center);
    }
    c.rgb = rgb / 255.0;
    return c;
}
vec4 vi_divot_tap(vec2 p) {
    vec4 c = vi_filter_tap(p);
    if (vi_divot()) {
        vec4 a = vi_filter_tap(p - vec2(vi_scale(), 0.0));
        vec4 b = vi_filter_tap(p + vec2(vi_scale(), 0.0));
        if (min(c.a, min(a.a, b.a)) < 0.99)
            c.rgb = a.rgb + c.rgb + b.rgb - min(a.rgb, min(c.rgb, b.rgb)) -
                    max(a.rgb, max(c.rgb, b.rgb));
    }
    return c;
}
vec3 vi_lerp(vec3 a, vec3 b, float fraction) {
    a = floor(a * 255.0 + 0.5);
    b = floor(b * 255.0 + 0.5);
    return mod(a + floor(((b - a) * fraction + 16.0) / 32.0), 256.0) / 255.0;
}
vec4 vi_resample(vec2 pos) {
    vec2 base = floor(pos), f = floor(fract(pos) * 32.0);
    vec4 a = vi_divot_tap(base);
    if (!vi_resampling() || vi_aa() == 3.0)
        return a;
    // Zero fractions avoid all unused taps. Vertical rounding precedes
    // horizontal rounding, as on the VI; coverage never weights the lerp.
    if (f.y > 0.0)
        a.rgb = vi_lerp(a.rgb, vi_divot_tap(base + vec2(0.0, 1.0)).rgb, f.y);
    if (f.x > 0.0) {
        vec4 b = vi_divot_tap(base + vec2(1.0, 0.0));
        if (f.y > 0.0)
            b.rgb = vi_lerp(b.rgb, vi_divot_tap(base + vec2(1.0)).rgb, f.y);
        a.rgb = vi_lerp(a.rgb, b.rgb, f.x);
    }
    return a;
}
)";
}
