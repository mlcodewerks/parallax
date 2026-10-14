#pragma once
#include "vi_filter_shader.hpp"
#include <string>
namespace glrdp_es2 {
inline constexpr const char *vertex = R"(
attribute vec2 position;
uniform vec4 viewport_info;
uniform vec2 origin;
uniform vec4 q0, qdx, qdy, shade0, shade_dx, shade_dy, z_plane;
varying P vec4 vQ, vQdx, vQdy;
varying P vec4 vShade, vShadeDx, vShadeDy, vZ, vPixel;
void main() {
    vec2 d = position - origin;
    gl_Position = vec4(position.x / viewport_info.x * 2.0 - 1.0,
                       1.0 - position.y / viewport_info.y * 2.0, 0.0, 1.0);
    vQ = vec4((q0 + qdx * d.x + qdy * d.y).xyz, q0.w);
    vQdx = qdx;
    vQdy = qdy;
    vShade = shade0 + shade_dx * d.x + shade_dy * d.y;
    vShadeDx = shade_dx;
    vShadeDy = shade_dy;
    vZ = vec4(z_plane.x + z_plane.y * d.x + z_plane.z * d.y, z_plane.yzw);
    vPixel = vec4(position, viewport_info.z, viewport_info.y);
}
)";

inline constexpr const char *fragment = R"(
uniform sampler2D atlas_image, metadata, color_memory, depth_memory;
uniform vec4 primitive, environment, fog, blend, key_center, key_scale, settings;
uniform vec2 atlas_size;
uniform float depth_pass;
uniform vec4 edge_y, edge_x, edge_slope;
varying P vec4 vQ, vQdx, vQdy;
varying P vec4 vShade, vShadeDx, vShadeDy, vZ, vPixel;
float copy_lane = 0.0;
vec4 data(float i) {
    return floor(
        texture2D(metadata, (vec2(mod(i, 16.0), floor(i / 16.0)) + 0.5) / vec2(16.0, 16.0)) *
            255.0 +
        0.5);
}
float number(float i) {
    vec4 b = data(i);
    float e = mod(b.x, 128.0) * 2.0 + floor(b.y / 128.0);
    if (e == 0.0)
        return 0.0;
    return (b.x >= 128.0 ? -1.0 : 1.0) * exp2(e - 127.0) *
           (1.0 + mod(b.y, 128.0) / 128.0 + b.z / 32768.0 + b.w / 8388608.0);
}
bool flag(float bits, float bit) {
    return mod(floor(bits / bit), 2.0) > 0.5;
}

#if PRECISE_Z
// Byte arithmetic keeps the full RDP accumulator without GLSL integer bit ops.
// Each product/carry remains within the highp 16-bit precision minimum.
vec4 tc_normalize(vec4 b) {
    b.y += floor(b.x / 256.0);
    b.x = mod(b.x, 256.0);
    b.z += floor(b.y / 256.0);
    b.y = mod(b.y, 256.0);
    b.w += floor(b.z / 256.0);
    b.z = mod(b.z, 256.0);
    b.w = mod(b.w, 256.0);
    return b;
}
vec4 tc_add(vec4 a, vec4 b) {
    return tc_normalize(a + b);
}
vec4 tc_part(vec4 b, float n) {
    return tc_normalize(b * n);
}
vec4 tc_mul(vec4 b, float n) {
    vec4 low = tc_part(b, mod(n, 256.0)), high = tc_part(b, floor(n / 256.0));
    return tc_add(low, vec4(0.0, high.xyz));
}
vec4 tc_shift(vec4 b, float bits) {
    // Arithmetic shifts by 0..14 bits, using bounded byte values. Move a
    // whole byte first, then carry the remaining low bits from its neighbour.
    if (bits >= 8.0) {
        b = vec4(b.yzw, b.w >= 128.0 ? 255.0 : 0.0);
        bits -= 8.0;
    }
    float divisor = exp2(bits);
    vec4 next = vec4(b.yzw, b.w >= 128.0 ? 255.0 : 0.0);
    return floor(b / divisor) + mod(next, divisor) * (256.0 / divisor);
}
float tc_sign16(vec4 b) {
    float v = b.z + b.w * 256.0;
    return v >= 32768.0 ? v - 65536.0 : v;
}
float tc_bit(vec4 b, float bit) {
    float value = bit < 8.0 ? b.x : bit < 16.0 ? b.y : bit < 24.0 ? b.z : b.w;
    return mod(floor(value / exp2(mod(bit, 8.0))), 2.0);
}
vec4 tc_span(float slot, vec2 pixel, vec4 major, bool depth) {
    vec4 start = data(slot).wzyx, dx = data(slot + 1.0).wzyx, de = data(slot + 2.0).wzyx,
         dy = data(slot + 3.0).wzyx;
    vec4 row = tc_add(start, tc_mul(de, pixel.y - floor(edge_y.x / 4.0)));
    row.x = 0.0;
    row.y = floor(row.y / 2.0) * 2.0;
    de.x = 0.0;
    de.y = floor(de.y / 2.0) * 2.0;
    dy.x = 0.0;
    dy.y = floor(dy.y / 2.0) * 2.0;
    bool offset = (edge_slope.x < 0.0) == (edge_y.w > 0.5);
    if (offset)
        row = tc_add(row, tc_add(tc_add(de, -tc_shift(de, 2.0)), tc_add(-dy, tc_shift(dy, 2.0))));
    vec4 fractional = vec4(floor(dx.y / 2.0) * 2.0, dx.z, dx.w, dx.w >= 128.0 ? 255.0 : 0.0);
    row = tc_add(row, -tc_mul(fractional, major.y));
    row.x = 0.0;
    row.y = floor(row.y / 4.0) * 4.0;
    if (!depth)
        dx.x = floor(dx.x / 32.0) * 32.0;
    float anchor = major.z + mod(major.w, 16.0) * 256.0;
    if (anchor >= 2048.0)
        anchor -= 4096.0;
    return tc_add(row, tc_mul(dx, pixel.x - anchor));
}
float tc_axis(float coordinate, float reciprocal, float shift, bool carry) {
    if (carry)
        return 32767.0;
    vec4 value = vec4(mod(coordinate, 256.0), mod(floor(coordinate / 256.0), 256.0),
                      coordinate < 0.0 ? 255.0 : 0.0, coordinate < 0.0 ? 255.0 : 0.0);
    vec4 product = tc_mul(value, reciprocal);
    // Bits 29..(29-shift) must be uniformly zero or one. Read that field
    // directly from the two high bytes instead of counting fifteen bits.
    float upper = product.z + product.w * 256.0;
    float field = shift == 14.0 ? mod(upper, 16384.0) * 2.0 + floor(product.y / 128.0)
                                : mod(floor(upper / exp2(13.0 - shift)), exp2(shift + 1.0));
    if (field != 0.0 && field != exp2(shift + 1.0) - 1.0)
        return coordinate < 0.0 ? -32768.0 : 32767.0;
    vec4 outvalue = shift == 14.0 ? tc_part(product, 2.0) : tc_shift(product, 13.0 - shift);
    float high = mod(outvalue.z, 2.0), sign = outvalue.y >= 128.0 ? 1.0 : 0.0;
    if (high == 0.0 && sign == 1.0)
        return 32767.0;
    if (high == 1.0 && sign == 0.0)
        return -32768.0;
    return outvalue.x + outvalue.y * 256.0 - sign * 65536.0;
}
vec2 tc_coordinates(vec2 pixel, bool perspective) {
    bool offset = (edge_slope.x < 0.0) == (edge_y.w > 0.5);
    vec4 major =
        tc_add(data(84.0).wzyx, tc_mul(data(85.0).wzyx, pixel.y * 4.0 + (offset ? 3.0 : 0.0) -
                                                            floor(edge_y.x / 4.0) * 4.0));
    vec3 value = vec3(tc_sign16(tc_span(72.0, pixel, major, false)),
                      tc_sign16(tc_span(76.0, pixel, major, false)),
                      tc_sign16(tc_span(80.0, pixel, major, false)));
    if (!perspective)
        return value.xy / 32.0;
    float w = mod(value.z, 32768.0), leading = 0.0, v = w;
    if (v >= 256.0) {
        v = floor(v / 256.0);
        leading += 8.0;
    }
    if (v >= 16.0) {
        v = floor(v / 16.0);
        leading += 4.0;
    }
    if (v >= 4.0) {
        v = floor(v / 4.0);
        leading += 2.0;
    }
    if (v >= 2.0)
        leading += 1.0;
    float shift = 14.0 - leading, norm = mod(w * exp2(shift), 16384.0);
    vec4 table = data(128.0 + floor(norm / 256.0));
    float weight = mod(norm, 256.0) * 4.0;
    float reciprocal =
        mod(table.x * 256.0 + table.y + floor((-table.z / 1024.0) * weight), 32768.0);
    return vec2(tc_axis(value.x, reciprocal, shift, value.z <= 0.0),
                tc_axis(value.y, reciprocal, shift, value.z <= 0.0)) /
           32.0;
}
#endif

float axis(float p, float size, float mask, float mode) {
    if (flag(mode, 1.0) || mask == 0.0)
        p = clamp(p, 0.0, size - 1.0);
    if (mask > 0.0) {
        float period = exp2(mask);
        if (flag(mode, 2.0) && mod(floor(p / period), 2.0) > 0.5)
            p = -p - 1.0;
        p = mod(p, period);
    }
    return clamp(p, 0.0, size - 1.0);
}
vec4 texel(vec2 p, vec2 size, vec4 wrap, vec2 offset) {
    p = vec2(axis(p.x, size.x, wrap.x, wrap.z), axis(p.y, size.y, wrap.y, wrap.w));
    return texture2D(atlas_image, (offset + p + 0.5) / atlas_size);
}
vec4 sample_tile(float level, vec2 st, float filter, float cycle, float texture_cycle) {
    float i = 16.0 + mod(level, 8.0) * 7.0;
    vec2 uv = st * vec2(number(i), number(i + 1.0)) + vec2(number(i + 2.0), number(i + 3.0));
    if (cycle == 2.0)
        uv.x = floor(uv.x) + copy_lane;
    vec4 wrap = data(i + 4.0), a = data(i + 5.0), b = data(i + 6.0);
    vec2 offset = vec2(a.x * 256.0 + a.y, a.z * 256.0 + a.w),
         size = vec2(b.x * 256.0 + b.y, b.z * 256.0 + b.w);
    vec2 p = floor(uv), f = floor(fract(uv) * 32.0) / 32.0;
    if (cycle == 2.0 && flag(wrap.z, 16.0)) {
        vec2 coordinate = p;
        if (wrap.x > 0.0) {
            float period = exp2(min(wrap.x, 10.0));
            if (flag(wrap.z, 2.0) && mod(floor(coordinate.x / period), 2.0) > 0.5)
                coordinate.x = -coordinate.x - 1.0;
            coordinate.x = mod(coordinate.x, period);
        }
        if (wrap.y > 0.0) {
            float period = exp2(min(wrap.y, 10.0));
            if (flag(wrap.w, 2.0) && mod(floor(coordinate.y / period), 2.0) > 0.5)
                coordinate.y = -coordinate.y - 1.0;
            coordinate.y = mod(coordinate.y, period);
        }
        vec2 line_bytes = data(123.0).xy;
        // Multiply modulo 512 in four-bit limbs. Intermediates stay below
        // 1024, preserving the bank bits even with mediump arithmetic.
        float line = line_bytes.x * 256.0 + line_bytes.y;
        vec2 low = mod(vec2(coordinate.y, line), 16.0),
             high = mod(floor(vec2(coordinate.y, line) / 16.0), 32.0);
        float row = mod(low.x * low.y + 16.0 * mod(low.x * high.y + high.x * low.y, 32.0) +
                            256.0 * mod(high.x * high.y, 2.0),
                        512.0);
        float sum = mod(coordinate.x, 512.0) + mod(row, 128.0) * 4.0;
        float column = mod(sum, 512.0),
              bank =
                  mod(floor(coordinate.x / 512.0) + floor(row / 128.0) + floor(sum / 512.0), 4.0);
        if (mod(coordinate.y, 2.0) > 0.5)
            column += mod(floor(column / 2.0), 2.0) < 0.5 ? 2.0 : -2.0;
        return texture2D(atlas_image, (offset + vec2(column, bank) + 0.5) / atlas_size) * 255.0;
    }
    if (filter < 2.0 || cycle == 2.0)
        return texel(p, size, wrap, offset) * 255.0;
    vec4 convert_mode = data(121.0);
    if (flag(wrap.z, 8.0) && (texture_cycle == 0.0 ? convert_mode.x : convert_mode.y) < 0.5) {
        vec4 a = texel(p, size, wrap, offset), d = texel(p + vec2(2, 1), size, wrap, offset);
        vec2 rg = vec2(floor(f.x * 16.0) / 32.0 + mod(p.x, 2.0) * 0.5, f.y);
        return vec4(rg.x + rg.y >= 1.0 ? d.rg : a.rg, f.x + f.y >= 1.0 ? d.ba : a.ba) * 255.0;
    }
    if (flag(wrap.z, 4.0) || flag(wrap.z, 8.0)) {
        vec4 a = texel(p, size, wrap, offset), b = texel(p + vec2(2, 0), size, wrap, offset);
        vec4 c = texel(p + vec2(0, 1), size, wrap, offset),
             d = texel(p + vec2(2, 1), size, wrap, offset);
        vec2 rg = vec2(floor(f.x * 16.0) / 32.0 + mod(p.x, 2.0) * 0.5, f.y);
        vec4 low = a + (b - a) * f.x + (c - a) * f.y;
        vec4 high = d + (c - d) * (1.0 - f.x) + (b - d) * (1.0 - f.y);
        vec4 result = f.x + f.y < 1.0 ? low : high;
        result.rg = rg.x + rg.y < 1.0
                        ? a.rg + (b.rg - a.rg) * rg.x + (c.rg - a.rg) * rg.y
                        : d.rg + (c.rg - d.rg) * (1.0 - rg.x) + (b.rg - d.rg) * (1.0 - rg.y);
        if (filter == 3.0) {
            if (f.x == 0.5 && f.y == 0.5)
                result.ba = (a.ba + b.ba + c.ba + d.ba) * 0.25;
            if (rg.x == 0.5 && rg.y == 0.5)
                result.rg = (a.rg + b.rg + c.rg + d.rg) * 0.25;
        }
        return floor(result * 255.0 + 0.5);
    }
    if (filter == 3.0 && f.x == 0.5 && f.y == 0.5)
        return (texel(p, size, wrap, offset) + texel(p + vec2(1, 0), size, wrap, offset) +
                texel(p + vec2(0, 1), size, wrap, offset) + texel(p + 1.0, size, wrap, offset)) *
               63.75;
    vec2 p0 = p, p1 = p + vec2(1, 0), p2 = p + vec2(0, 1);
    if (f.x + f.y >= 1.0) {
        p0 = p + 1.0;
        p1 = p + vec2(0, 1);
        p2 = p + vec2(1, 0);
        f = 1.0 - f;
    }
    vec4 c = texel(p0, size, wrap, offset);
    return floor((c + (texel(p1, size, wrap, offset) - c) * f.x +
                  (texel(p2, size, wrap, offset) - c) * f.y) *
                     255.0 +
                 0.5);
}
vec2 divide(vec3 q) {
    return q.xy / max(q.z, MIN_Q);
}
void lod(vec3 q, float flags, out float first, out float second, out float fraction) {
    vec2 here = divide(q), fractional = fract(fract(here) * 32.0);
    vec2 dx = floor((divide(q + vQdx.xyz) - here) * 32.0 + fractional),
         dy = floor((divide(q + vQdy.xyz) - here) * 32.0 + fractional);
    dx = vec2(dx.x < 0.0 ? -dx.x - 1.0 : dx.x, dx.y < 0.0 ? -dx.y - 1.0 : dx.y);
    dy = vec2(dy.x < 0.0 ? -dy.x - 1.0 : dy.x, dy.y < 0.0 ? -dy.y - 1.0 : dy.y);
    float delta = max(max(dx.x, dx.y), max(dy.x, dy.y)), level = 0.0;
    bool distant = false, magnify = false, detail = flag(flags, 2.0), sharp = flag(flags, 4.0);
    if (delta >= 16384.0 || q.z <= 0.0 || (q + vQdx.xyz).z <= 0.0 || (q + vQdy.xyz).z <= 0.0) {
        distant = true;
        fraction = 255.0;
    } else if (delta < 32.0 || delta < settings.x) {
        magnify = true;
        distant = settings.w == 0.0;
        fraction = (!sharp && !detail) ? (distant ? 255.0 : 0.0) : max(delta, settings.x) * 8.0;
        if (sharp)
            fraction += 256.0;
    } else {
        float input_value = mod(floor(delta / 32.0), 256.0);
        for (int bit = 1; bit < 8; ++bit)
            if (input_value >= exp2(float(bit)))
                level = float(bit);
        distant = settings.w == 0.0 || delta >= 8192.0 || level >= settings.w;
        fraction =
            (!sharp && !detail && distant) ? 255.0 : mod(floor(delta / exp2(level) * 8.0), 256.0);
    }
    if (distant)
        level = settings.w;
    first = detail ? mod(level + (magnify ? 0.0 : 1.0), 8.0) : level;
    second = detail ? mod(level + ((!distant && !magnify) ? 2.0 : 1.0), 8.0)
                    : ((distant || (!sharp && magnify)) ? first : mod(first + 1.0, 8.0));
    if (!flag(flags, 1.0)) {
        first = 0.0;
        second = 1.0;
    }
}
float noise() {
    return floor(fract(sin(dot(floor(vPixel.xy), vec2(12.9898, 78.233))) * 4375.8545) * 255.0);
}
vec2 dither_values(vec2 modes) {
    vec2 p = mod(floor(vPixel.xy), 4.0);
    vec4 row;
    if (modes.x == 1.0 || modes.x == 3.0) {
        row = p.y < 1.0   ? vec4(0, 4, 1, 5)
              : p.y < 2.0 ? vec4(4, 0, 5, 1)
              : p.y < 3.0 ? vec4(3, 7, 2, 6)
                          : vec4(7, 3, 6, 2);
    } else {
        row = p.y < 1.0   ? vec4(0, 6, 1, 7)
              : p.y < 2.0 ? vec4(4, 2, 5, 3)
              : p.y < 3.0 ? vec4(3, 5, 2, 4)
                          : vec4(7, 1, 6, 0);
    }
    float matrix = p.x < 1.0 ? row.x : p.x < 2.0 ? row.y : p.x < 3.0 ? row.z : row.w;
    float rgb = modes.x == 2.0 ? noise() : modes.x == 3.0 ? 7.0 : matrix;
    float alpha = modes.y == 3.0   ? 0.0
                  : modes.y == 2.0 ? mod(noise(), 8.0)
                  : modes.y == 1.0 ? 7.0 - matrix
                                   : matrix;
    return vec2(rgb, alpha);
}
vec3 dither_rgb(vec3 rgb, float threshold, float mode) {
    vec3 compare = mode == 2.0 ? mod(floor(vec3(threshold, threshold / 8.0, threshold / 64.0)), 8.0)
                               : vec3(threshold);
    vec3 rounded = min(floor(rgb / 8.0) * 8.0 + 8.0, 255.0);
    return vec3(mod(rgb.r, 8.0) > compare.r ? rounded.r : rgb.r,
                mod(rgb.g, 8.0) > compare.g ? rounded.g : rgb.g,
                mod(rgb.b, 8.0) > compare.b ? rounded.b : rgb.b);
}
vec4 yuv_convert(vec4 raw) {
    vec4 k = vec4(number(117.0), number(118.0), number(119.0), number(120.0)) / 256.0;
    vec3 rgb = raw.b + floor(vec3(k.x * raw.g, k.y * raw.r + k.z * raw.g, k.w * raw.r) + 0.5);
    rgb = mod(rgb, 512.0);
    rgb -= step(vec3(384.0), rgb) * 512.0;
    return vec4(rgb, mod(raw.b, 512.0));
}
vec4 texture_fixed(vec4 value, float level, float cycle) {
    if (!flag(data(20.0 + mod(level, 8.0) * 7.0).z, 8.0))
        return value;
    value.rg -= vec2(128.0);
    vec4 mode = data(121.0);
    return (cycle == 0.0 ? mode.x : mode.y) > 0.5 ? value : yuv_convert(value);
}
vec3 rgb(float mux, vec4 c, vec4 t0, vec4 t1, vec4 shade) {
    if (mux == 0.0)
        return c.rgb;
    if (mux == 1.0)
        return t0.rgb;
    if (mux == 2.0)
        return t1.rgb;
    if (mux == 3.0)
        return primitive.rgb;
    if (mux == 4.0)
        return shade.rgb;
    if (mux == 5.0)
        return environment.rgb;
    return vec3(0.0);
}
float alpha(float mux, vec4 c, vec4 t0, vec4 t1, vec4 shade) {
    if (mux == 0.0)
        return c.a;
    if (mux == 1.0)
        return t0.a;
    if (mux == 2.0)
        return t1.a;
    if (mux == 3.0)
        return primitive.a;
    if (mux == 4.0)
        return shade.a;
    if (mux == 5.0)
        return environment.a;
    return mux == 6.0 ? 255.0 : 0.0;
}
bool combine_rgb = true;
vec4 combine(float cycle, vec4 c, vec4 t0, vec4 t1, vec4 shade, float fraction) {
    vec4 n = data(cycle * 2.0 + 1.0);
    vec3 colour = vec3(0.0);
    if (combine_rgb) {
        vec4 m = data(cycle * 2.0);
        vec3 a = rgb(m.x, c, t0, t1, shade), b = rgb(m.y, c, t0, t1, shade),
             d = rgb(m.w, c, t0, t1, shade), f = rgb(m.z, c, t0, t1, shade);
        if (m.x == 6.0)
            a = vec3(255.0);
        if (m.x == 7.0)
            a = vec3(noise());
        if (m.y == 6.0)
            b = key_center.rgb;
        if (m.y == 7.0)
            b = vec3(number(114.0));
        if (m.w == 6.0)
            d = vec3(255.0);
        if (m.z == 6.0)
            f = key_scale.rgb;
        if (m.z >= 7.0 && m.z <= 12.0)
            f = vec3(alpha(m.z - 7.0, c, t0, t1, shade));
        if (m.z == 13.0)
            f = vec3(fraction);
        if (m.z == 14.0)
            f = vec3(settings.y);
        if (m.z == 15.0)
            f = vec3(number(116.0));
        f = mod(f + 256.0, 512.0) - 256.0;
        colour = (a - b) * (f / 256.0) + d;
    }
    float aa = alpha(n.x, c, t0, t1, shade), ab = alpha(n.y, c, t0, t1, shade),
          ad = alpha(n.w, c, t0, t1, shade);
    float ac = n.z == 0.0 ? fraction : (n.z == 6.0 ? settings.y : alpha(n.z, c, t0, t1, shade));
    ac = mod(ac + 256.0, 512.0) - 256.0;
    vec4 value = mod(floor(vec4(colour, (aa - ab) * (ac / 256.0) + ad) + 0.5), 512.0);
    return min(value, 255.0) * (1.0 - step(vec4(384.0), value));
}
vec3 blend_rgb(float mux, vec4 pixel, vec4 memory_value) {
    if (mux == 0.0)
        return pixel.rgb;
    if (mux == 1.0)
        return memory_value.rgb;
    if (mux == 2.0)
        return blend.rgb;
    return fog.rgb;
}
float blender_divide(float numerator, float divisor) {
    float d = mod(floor(divisor / 4.0), 16.0), n = mod(floor(numerator / 4.0), 2048.0);
    float invd = 15.0 - d, partial = mod(invd + floor(n / 256.0) + 1.0, 8.0), result = 0.0;
    float weight = 128.0;
    for (int k = 0; k < 8; ++k) {
        float bit = mod(floor(n / weight), 2.0);
        float temp = mod(floor(result / (weight * 2.0)), 2.0) > 0.0
                         ? invd + partial * 2.0 + bit + 1.0
                         : d + partial * 2.0 + bit;
        partial = mod(temp, 8.0);
        if (mod(floor(temp / 16.0), 2.0) > 0.0)
            result += weight;
        weight *= 0.5;
    }
    return result;
}
vec2 blend_shifts = vec2(0.0);
vec4 blend_cycle(float cycle, vec4 p, vec4 m, vec4 shade, bool force, bool final_cycle) {
    vec4 mux = data(4.0 + cycle);
    float raw = mux.y == 0.0 ? p.a : (mux.y == 1.0 ? fog.a : (mux.y == 2.0 ? shade.a : 0.0));
    float a = floor(raw / 8.0),
          b = floor(
              (mux.w == 0.0 ? 255.0 - raw : (mux.w == 1.0 ? m.a : (mux.w == 2.0 ? 255.0 : 0.0))) /
              8.0);
    if (mux.w == 1.0) {
        a = floor(floor(a / exp2(blend_shifts.x)) / 4.0) * 4.0;
        b = floor(b / exp2(blend_shifts.y));
        b = floor(b / 4.0) * 4.0 + 3.0;
    }
    vec3 x = blend_rgb(mux.x, p, m), y = blend_rgb(mux.z, p, m);
    if (final_cycle && mux.y == 0.0 && mux.w == 0.0 && p.a >= 255.0)
        return vec4(x, p.a);
    float divisor =
        force || !final_cycle ? 32.0 : max(floor(a / 4.0) * 4.0 + floor(b / 4.0) * 4.0 + 4.0, 1.0);
    vec3 numerator = x * a + y * (b + 1.0);
#if PRECISE_Z
    vec3 rgb = force || !final_cycle ? mod(floor(numerator / 32.0), 256.0)
                                     : vec3(blender_divide(numerator.r, divisor),
                                            blender_divide(numerator.g, divisor),
                                            blender_divide(numerator.b, divisor));
#else
    vec3 rgb = mod(floor(x * (a / divisor) + y * ((b + 1.0) / divisor)), 256.0);
#endif
    return vec4(rgb, p.a);
}
#if GLRDP_FAST_COVERAGE
vec2 fast_triangle_span(float y) {
    float major = edge_x.x + edge_slope.x * (y - floor(edge_y.x / 4.0) * 4.0);
    float minor = y < edge_y.y ? edge_x.y + edge_slope.y * (y - floor(edge_y.x / 4.0) * 4.0)
                               : edge_x.z + edge_slope.z * (y - edge_y.y);
    return edge_y.w > 0.5 ? vec2(major, minor) : vec2(minor, major);
}
bool fast_triangle_pixel(vec2 pixel, bool first_sample_only) {
    float y = pixel.y * 4.0;
    if (first_sample_only) {
        if (y < edge_y.x || y >= edge_y.z)
            return false;
        vec2 span = fast_triangle_span(y);
        return pixel.x >= span.x && pixel.x < span.y;
    }
    float lo = max(y, edge_y.x), hi = min(y + 3.0, edge_y.z - 1.0);
    if (lo > hi)
        return false;
    if (y + 2.0 >= edge_y.x && y + 2.0 < edge_y.z) {
        vec2 center = fast_triangle_span(y + 2.0);
        if (pixel.x + 0.5 >= center.x && pixel.x + 0.5 < center.y)
            return true;
    }
    vec2 a = fast_triangle_span(lo), b = fast_triangle_span(hi);
    float left = min(a.x, b.x), right = max(a.y, b.y);
    float width = max(a.y - a.x, b.y - b.x);
    if (edge_y.y > lo && edge_y.y <= hi) {
        a = fast_triangle_span(edge_y.y - 1.0);
        b = fast_triangle_span(edge_y.y);
        left = min(left, min(a.x, b.x));
        right = max(right, max(a.y, b.y));
        width = max(width, max(a.y - a.x, b.y - b.x));
    }
    return width < 1.0 && right > left && pixel.x < right && pixel.x + 1.0 > left;
}
#endif
float coverage(out vec2 offset, out bool coverage_bit, bool aa) {
    vec2 pixel = floor(vPixel.xy);
#if GLRDP_FAST_COVERAGE
    coverage_bit = true;
    offset = pixel - vPixel.xy;
    return fast_triangle_pixel(pixel, !aa && RDP_CYCLE < 2.0) ? 8.0 : 0.0;
#else
    float count = 0.0;
    bool found = false;
    coverage_bit = false;
    float yh = edge_y.x, ym = edge_y.y, yl = edge_y.z, major_side = edge_y.w;
    float xh = edge_x.x, xm = edge_x.y, xl = edge_x.z, dh = edge_slope.x, dm = edge_slope.y,
          dl = edge_slope.z;
    for (int row = 0; row < 4; ++row) {
        float y = pixel.y * 4.0 + float(row);
        if (y >= yh && y < yl) {
            float major = xh + dh * (y - floor(yh / 4.0) * 4.0);
            float minor = y < ym ? xm + dm * (y - floor(yh / 4.0) * 4.0) : xl + dl * (y - ym);
            float left = major_side > 0.0 ? major : minor, right = major_side > 0.0 ? minor : major;
            for (int col = 0; col < 2; ++col) {
#if GLRDP_REDUCED_COVERAGE
                if (col != ((row == 1 || row == 2) ? 1 : 0))
                    continue;
#endif
                float quarter = mod(float(row), 2.0) + float(col) * 2.0,
                      x = pixel.x + quarter * 0.25;
                if (x >= left && x < right) {
#if GLRDP_REDUCED_COVERAGE
                    count += 2.0;
#else
                    count += 1.0;
#endif
                    if (row == 0 && col == 0)
                        coverage_bit = true;
                    if (!found) {
                        offset = pixel + vec2(quarter, float(row)) * 0.25 - vPixel.xy;
                        found = true;
                    }
                }
            }
        }
    }
    return count;
#endif
}
float rectangle_coverage(out vec2 offset, out bool coverage_bit, bool aa) {
    vec2 pixel = floor(vPixel.xy);
    vec4 bounds = vec4(number(86.0), number(87.0), number(88.0), number(89.0));
#if GLRDP_FAST_COVERAGE
    bool inside =
        aa ? pixel.x < bounds.z && pixel.x + 1.0 > bounds.x && pixel.y < bounds.w &&
                 pixel.y + 1.0 > bounds.y
           : pixel.x >= bounds.x && pixel.x < bounds.z && pixel.y >= bounds.y && pixel.y < bounds.w;
    coverage_bit = true;
    offset = pixel - vPixel.xy;
    return inside ? 8.0 : 0.0;
#else
    float count = 0.0;
    bool found = false;
    coverage_bit = false;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 2; ++col) {
#if GLRDP_REDUCED_COVERAGE
            if (col != ((row == 1 || row == 2) ? 1 : 0))
                continue;
#endif
            float quarter = mod(float(row), 2.0) + float(col) * 2.0;
            vec2 p = pixel + vec2(quarter, float(row)) * 0.25;
            if (p.x >= bounds.x && p.x < bounds.z && p.y >= bounds.y && p.y < bounds.w) {
#if GLRDP_REDUCED_COVERAGE
                count += 2.0;
#else
                count += 1.0;
#endif
                if (row == 0 && col == 0)
                    coverage_bit = true;
                if (!found) {
                    offset = p - vPixel.xy;
                    found = true;
                }
            }
        }
    return count;
#endif
}
float unpack_z(vec4 b) {
    if (b.a >= 128.0) {
        float e = floor(b.r / 32.0), mantissa = mod(b.r, 32.0) * 64.0 + floor(b.g / 4.0);
        float shift = max(6.0 - e, 0.0), add = 1.0 - exp2(-e);
        if (e == 7.0)
            add = 0.9921875;
        return (mantissa * exp2(shift - 18.0) + add) * (262144.0 / 262143.0);
    }
    return dot(b.rgb, vec3(0.25, 0.0009765625, 0.000003814697265625)) * (262144.0 / 262143.0);
}
vec4 pack_z(float z, float exponent, float cvg) {
    // Round to Z18 before byte packing. Flooring a slightly inexact plane
    // could subtract one Z unit and change the compressed word read by CPU.
    float value = clamp(z, 0.0, 1.0) * (262143.0 / 262144.0) * 4.0 + Z_ROUND;
    if (value >= 4.0)
        return vec4(3.0, 255.0, 255.0, exponent * 8.0 + cvg) / 255.0;
    float r = floor(value);
    value = fract(value) * 256.0;
    float g = floor(value);
    float b = floor(fract(value) * 256.0 + 0.0001);
    return vec4(r, g, b, exponent * 8.0 + cvg) / 255.0;
}
#if PRECISE_Z
float triangle_span_z(float geometric_cvg, vec2 offset) {
    vec2 pixel = floor(vPixel.xy);
    bool correction = (edge_slope.x < 0.0) == (edge_y.w > 0.5);
    vec4 major =
        tc_add(data(84.0).wzyx, tc_mul(data(85.0).wzyx, pixel.y * 4.0 + (correction ? 3.0 : 0.0) -
                                                            floor(edge_y.x / 4.0) * 4.0));
    vec4 value = tc_shift(tc_span(86.0, pixel, major, true), 10.0);
    value.w = 0.0;
    value.z = mod(value.z, 64.0);
    if (geometric_cvg == 8.0)
        value = tc_shift(value, 3.0);
    else {
        vec2 first = floor((vPixel.xy + offset - pixel) * 4.0 + 0.5);
        vec4 dx = tc_shift(data(87.0).wzyx, 10.0), dy = tc_shift(data(89.0).wzyx, 10.0);
        value = tc_shift(
            tc_add(tc_part(value, 4.0), tc_add(tc_mul(dx, first.x), tc_mul(dy, first.y))), 5.0);
    }
    float z = value.x + value.y * 256.0 + mod(value.z, 8.0) * 65536.0;
    float overflow = mod(floor(z / 131072.0), 4.0);
    return overflow == 2.0 ? 262143.0 : overflow == 3.0 ? 0.0 : mod(z, 262144.0);
}
float shade_signed13(vec4 value) {
    float result = floor(value.y / 64.0) + value.z * 4.0 + mod(value.w, 8.0) * 1024.0;
    return result >= 4096.0 ? result - 8192.0 : result;
}
float triangle_shade_at(float slot, vec2 pixel, float geometric_cvg, vec2 first) {
    bool correction = (edge_slope.x < 0.0) == (edge_y.w > 0.5);
    vec4 major =
        tc_add(data(84.0).wzyx, tc_mul(data(85.0).wzyx, pixel.y * 4.0 + (correction ? 3.0 : 0.0) -
                                                            floor(edge_y.x / 4.0) * 4.0));
    vec4 value = tc_span(slot, pixel, major, false);
    float result;
    if (geometric_cvg == 8.0)
        result = value.z + mod(value.w, 2.0) * 256.0;
    else {
        result = mod(
            floor((shade_signed13(value) * 4.0 + first.x * shade_signed13(data(slot + 1.0).wzyx) +
                   first.y * shade_signed13(data(slot + 3.0).wzyx)) /
                  16.0),
            512.0);
    }
    return result >= 384.0 ? 0.0 : min(result, 255.0);
}
float triangle_shade(float slot, float geometric_cvg, vec2 offset) {
    vec2 pixel = floor(vPixel.xy);
    return triangle_shade_at(slot, pixel, geometric_cvg,
                             floor((vPixel.xy + offset - pixel) * 4.0 + 0.5));
}
#endif
float alpha_coverage_at(vec2 pixel, out vec2 first) {
    vec4 lo = data(208.0), hi = data(209.0);
    vec2 cliplo = lo.xz * 64.0 + floor(lo.yw / 4.0), cliphi = hi.xz * 64.0 + ceil(hi.yw / 4.0);
    first = vec2(0.0);
    float count = 0.0;
    if (pixel.x < cliplo.x || pixel.y < cliplo.y || pixel.x >= cliphi.x || pixel.y >= cliphi.y)
        return 0.0;
    bool triangle = flag(data(7.0).y, 2.0);
    vec4 bounds =
        triangle ? vec4(0.0) : vec4(number(86.0), number(87.0), number(88.0), number(89.0));
#if GLRDP_FAST_COVERAGE
    if (triangle)
        return fast_triangle_pixel(pixel, false) ? 8.0 : 0.0;
    return pixel.x < bounds.z && pixel.x + 1.0 > bounds.x && pixel.y < bounds.w &&
                   pixel.y + 1.0 > bounds.y
               ? 8.0
               : 0.0;
#else
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 2; ++col) {
#if GLRDP_REDUCED_COVERAGE
            if (col != ((row == 1 || row == 2) ? 1 : 0))
                continue;
#endif
            float quarter = mod(float(row), 2.0) + float(col) * 2.0, y = pixel.y * 4.0 + float(row);
            bool inside;
            if (triangle) {
                float major = edge_x.x + edge_slope.x * (y - floor(edge_y.x / 4.0) * 4.0);
                float minor = y < edge_y.y
                                  ? edge_x.y + edge_slope.y * (y - floor(edge_y.x / 4.0) * 4.0)
                                  : edge_x.z + edge_slope.z * (y - edge_y.y);
                float left = edge_y.w > 0.5 ? major : minor, right = edge_y.w > 0.5 ? minor : major;
                float x = pixel.x + quarter * 0.25;
                inside = y >= edge_y.x && y < edge_y.z && x >= left && x < right;
            } else {
                vec2 p = pixel + vec2(quarter, float(row)) * 0.25;
                inside = p.x >= bounds.x && p.x < bounds.z && p.y >= bounds.y && p.y < bounds.w;
            }
            if (inside) {
                if (count == 0.0)
                    first = vec2(quarter, float(row));
#if GLRDP_REDUCED_COVERAGE
                count += 2.0;
#else
                count += 1.0;
#endif
            }
        }
    return count;
#endif
}
// With alpha supplied directly by geometric coverage, alpha compare and key
// disabled, Z survival has no texture/shade/combiner dependency. Keep the
// exact packed depth and memory-coverage equations used by the general path.
// Keep temporaries local to each path: ANGLE's D3D compiler rejects a
// conditional-return helper that exports these mutable globals to colour.
void depth_only(vec4 mode, vec4 features, vec4 extra, float cycle, float cvg, float geometric_cvg,
                vec2 offset) {
    vec4 p = vec4(0.0);
    float width = number(115.0);
    vec2 uv = vec2(vPixel.x / width, 1.0 - vPixel.y / vPixel.w);
    vec4 previous = texture2D(color_memory, uv),
         stored = floor(texture2D(depth_memory, uv) * 255.0 + 0.5);
    vec4 previous_fixed = floor(previous * 255.0 + 0.5);
    bool intensity16 = extra.y == 2.0 && extra.w != 0.0;
    float memory_cvg =
        flag(features.z, 1.0) && extra.y >= 2.0
            ? (intensity16
                   ? mod(floor(previous_fixed.g / 8.0), 4.0) * 2.0 + floor(previous_fixed.b / 128.0)
               : extra.y == 3.0 ? floor(previous_fixed.a / 32.0)
                                : clamp(floor(previous.a * 8.0 + 0.5) - 1.0, 0.0, 7.0))
            : 7.0;
    float z;
#if PRECISE_Z
    z = (flag(features.y, 2.0) && extra.z == 0.0
             ? triangle_span_z(geometric_cvg, offset)
             : clamp(floor(vZ.x + vZ.y * offset.x + vZ.z * offset.y), 0.0, 262143.0)) /
        262143.0;
#else
    z = clamp(vZ.x + vZ.y * offset.x + vZ.z * offset.y, 0.0, 1.0);
#endif
    float zold = unpack_z(stored), dzexp = vZ.w;
    float oldexp = mod(floor(stored.a / 8.0), 16.0);
    float blend_exp = flag(features.w, 1.0) ? oldexp : 15.0;
    blend_shifts = clamp(vec2(dzexp - blend_exp, blend_exp - dzexp), 0.0, 4.0);
    float dzmem = exp2(max(oldexp, MIN_DZ_EXP) - 18.0), zprec = 0.0;
    if (zold >= 0.5)
        zprec = 1.0;
    if (zold >= 0.75)
        zprec = 2.0;
    if (zold >= 0.875)
        zprec = 3.0;
    bool coplanar = false;
    if (zprec < 3.0) {
        if (oldexp != 15.0)
            dzmem = max(dzmem * 2.0, exp2(-14.0 - zprec));
        else {
            coplanar = true;
            dzmem = 0.2499961853;
        }
    }
    float dzbase = exp2(floor(log2(max(exp2(max(dzexp, MIN_DZ_EXP) - 18.0), dzmem)))),
          tolerance = dzbase * 8.0 * (262144.0 / 262143.0);
    bool farther = coplanar || z + tolerance >= zold, nearer = coplanar || z - tolerance <= zold,
         infront = z < zold;
    bool overflow = memory_cvg + cvg >= 8.0, maxz = zold >= 1.0;
    bool accepted = maxz || (overflow ? infront : nearer);
    if (extra.x == 2.0)
        accepted = infront || maxz;
    if (extra.x == 3.0)
        accepted = farther && nearer && !maxz;
    if (flag(features.w, 4.0)) {
        if (depth_pass > 0.5)
            gl_FragColor = data(mod(floor(vPixel.x), 2.0) == 0.0 ? 9.0 : 10.0) / 255.0;
        else
            gl_FragColor =
                vec4(p.rgb / 255.0, extra.y == 3.0 ? p.a / 255.0 : (p.a > 0.5 ? 1.0 : 0.125));
        return;
    }
    if (flag(features.w, 1.0) && !accepted)
        discard;
    if (flag(features.w, 1.0) && extra.x == 1.0 && infront && farther && overflow) {
        // Split the quotient to keep intermediate values in mediump range.
        float coefficient = mod(
            floor(mod(zold, dzbase * 16.0) / dzbase) - floor(mod(z, dzbase * 16.0) / dzbase), 16.0);
        cvg = min(floor(coefficient * cvg / 8.0), 8.0);
        if (cvg == 0.0 && flag(features.z, 2.0))
            discard;
    }
    bool blend_farther = !flag(features.w, 1.0) || farther;
    float destination = mod(cvg - 1.0, 8.0);
    if (cycle < 2.0) {
        if (mode.w == 0.0)
            destination = (flag(features.z, 4.0) ||
                           (flag(features.z, 2.0) && blend_farther && memory_cvg + cvg < 8.0))
                              ? min(memory_cvg + cvg, 7.0)
                              : mod(cvg - 1.0, 8.0);
        else if (mode.w == 1.0)
            destination = mod(memory_cvg + cvg, 8.0);
        else if (mode.w == 2.0)
            destination = 7.0;
        else
            destination = memory_cvg;
    }
    if (depth_pass > 0.5) {
        if (flag(features.w, 2.0))
            gl_FragColor = pack_z(z, dzexp, destination);
        else
            gl_FragColor =
                vec4(stored.rgb, oldexp * 8.0 + destination + (stored.a >= 128.0 ? 128.0 : 0.0)) /
                255.0;
        return;
    }
}
void main() {
    vec4 mode = data(6.0), features = data(7.0), extra = data(8.0);
    const float cycle = RDP_CYCLE;
    float cvg = 8.0;
    vec2 offset = vec2(0.0);
    bool coverage_bit = true;
    if (flag(features.y, 2.0))
        cvg = coverage(offset, coverage_bit, flag(features.z, 2.0));
    else if (cycle < 2.0)
        cvg = rectangle_coverage(offset, coverage_bit, flag(features.z, 2.0));
    float geometric_cvg = cvg;
    if (cvg == 0.0)
        discard;
    if (cycle < 2.0 && !flag(features.z, 2.0) && !coverage_bit)
        discard;
#if GLRDP_FAST_COVERAGE
    cvg = 8.0;
    geometric_cvg = 8.0;
    const bool fast_coverage = true;
#else
    const bool fast_coverage = false;
#endif
    if (depth_pass > 0.5 && cycle < 2.0 && (fast_coverage || flag(features.z, 16.0)) &&
        !flag(features.z, 8.0) && mode.z == 0.0 && !flag(features.x, 8.0)) {
        depth_only(mode, features, extra, cycle, cvg, geometric_cvg, offset);
        return;
    }
    combine_rgb = depth_pass < 0.5 || flag(features.x, 8.0);
    vec4 shade = clamp(floor(vShade + vShadeDx * offset.x + vShadeDy * offset.y + 0.5), 0.0, 255.0);
#if PRECISE_Z
    if (flag(features.y, 2.0) && flag(features.y, 4.0) && vPixel.z == 1.0) {
        shade.a = triangle_shade(204.0, geometric_cvg, offset);
        if (combine_rgb)
            shade.rgb = vec3(triangle_shade(192.0, geometric_cvg, offset),
                             triangle_shade(196.0, geometric_cvg, offset),
                             triangle_shade(200.0, geometric_cvg, offset));
    }
#endif
    vec3 q = vQ.xyz - (vQdx.xyz + vQdy.xyz) * (0.5 / max(vPixel.z, 1.0));
    if (cycle == 2.0) {
        vec2 pixel = floor(vPixel.xy);
        q = vQ.xyz + vQdx.xyz * (pixel.x - vPixel.x) + vQdy.xyz * (pixel.y - vPixel.y);
        copy_lane = mod(pixel.x - number(112.0), 4.0);
        q -= vQdx.xyz * copy_lane;
    }
    if (flag(features.y, 1.0) && !flag(features.y, 2.0)) {
        vec2 d = floor(vPixel.xy) - vec2(number(84.0), number(85.0));
        vec3 dx = vec3(number(73.0), number(77.0), number(81.0));
        q = vec3(number(72.0), number(76.0), number(80.0)) + dx * d.x +
            vec3(number(74.0), number(78.0), number(82.0)) * d.y;
        if (cycle == 2.0)
            q -= dx * copy_lane;
    }
    float first = 0.0, second = 1.0, fraction = settings.y;
    if (cycle < 2.0 && flag(features.y, 1.0) && (flag(features.x, 1.0) || flag(features.x, 32.0)))
        lod(q, features.x, first, second, fraction);
    vec4 t0 = vec4(255.0), t1 = vec4(255.0), p;
    float compare_alpha = -1.0, compare_coverage = 0.0;
    vec2 coordinates = divide(q);
#if PRECISE_Z
    if (flag(features.y, 1.0) && flag(features.y, 2.0) && vPixel.z == 1.0)
        coordinates = tc_coordinates(floor(vPixel.xy), flag(data(121.0).w, 2.0));
#endif
    if (flag(features.y, 1.0)) {
        t0 = sample_tile(first, coordinates, mode.y, cycle, cycle == 0.0 ? 1.0 : 0.0);
        if (flag(features.x, 16.0))
            t1 = sample_tile(second, coordinates, mode.y, cycle, 1.0);
    }
    if (flag(features.y, 1.0)) {
        t0 = texture_fixed(t0, first, cycle == 0.0 ? 1.0 : 0.0);
        if (flag(features.x, 16.0))
            t1 = texture_fixed(t1, second, 1.0);
        if (cycle == 1.0 && data(121.0).z > 0.5 && flag(data(20.0 + mod(second, 8.0) * 7.0).z, 8.0))
            t1 = yuv_convert(t0);
    }
    if (cycle == 3.0)
        p = data(extra.y == 3.0 ? 13.0 : (mod(floor(vPixel.x), 2.0) == 0.0 ? 11.0 : 12.0));
    else if (cycle == 2.0) {
        p = t0;
        if (extra.y == 2.0)
            p.a = mod(p.a, 2.0) > 0.5 ? 255.0 : 0.0;
    } else if (cycle == 0.0)
        p = combine(1.0, vec4(0.0), t0, t1, shade, fraction);
    else {
        if (mod(mode.z, 2.0) > 0.5) {
            float direction = flag(features.y, 2.0) ? (edge_y.w > 0.5 ? 1.0 : -1.0) : 1.0;
            vec3 nq = q + vQdx.xyz * direction;
            vec2 nc = divide(nq);
#if PRECISE_Z
            if (flag(features.y, 1.0) && flag(features.y, 2.0) && vPixel.z == 1.0)
                nc = tc_coordinates(floor(vPixel.xy) + vec2(direction, 0.0),
                                    flag(data(121.0).w, 2.0));
#endif
            float nf = first, ns = second, nfraction = fraction;
            if (flag(features.y, 1.0) && (flag(features.x, 1.0) || flag(features.x, 32.0)))
                lod(nq, features.x, nf, ns, nfraction);
            vec4 nt0 = vec4(255.0), nt1 = vec4(255.0);
            if (flag(features.y, 1.0)) {
                nt0 = texture_fixed(sample_tile(nf, nc, mode.y, cycle, 0.0), nf, 0.0);
                nt1 = texture_fixed(sample_tile(ns, nc, mode.y, cycle, 1.0), ns, 1.0);
            }
            vec4 nshade = shade + vShadeDx * direction;
            vec2 np = floor(vPixel.xy) + vec2(direction, 0.0), nfirst;
            compare_coverage = alpha_coverage_at(np, nfirst);
#if PRECISE_Z
            if (flag(features.y, 2.0) && flag(features.y, 4.0) && vPixel.z == 1.0)
                nshade.a = triangle_shade_at(204.0, np, compare_coverage, nfirst);
#endif
            compare_alpha = combine(0.0, vec4(0.0), nt0, nt1, nshade, nfraction).a;
        }
        p = combine(1.0, combine(0.0, vec4(0.0), t0, t1, shade, fraction), t1, t0, shade, fraction);
    }
    if (flag(features.x, 8.0) &&
        all(lessThanEqual(abs(p.rgb - key_center.rgb), max(key_scale.rgb, vec3(1.0)))))
        discard;
    if (cycle < 2.0) {
        float product = floor(((p.a == 255.0 ? 256.0 : p.a) * cvg + 4.0) / 8.0);
        if (flag(features.z, 8.0))
            cvg = floor(product / 32.0);
        if (flag(features.z, 16.0))
            p.a = min(flag(features.z, 8.0) ? product : cvg * 32.0, 255.0);
    }
    vec2 dither_modes = data(122.0).xy, dith = dither_values(dither_modes);
    if (compare_alpha >= 0.0) {
        float a = compare_alpha == 255.0 ? 256.0 : compare_alpha;
        if (!flag(features.z, 16.0))
            a = min(a + dith.y, 255.0);
        else
            a = min(flag(features.z, 8.0) ? floor((a * compare_coverage + 4.0) / 8.0)
                                          : compare_coverage * 32.0,
                    255.0);
        compare_alpha = a;
    }
    if (cycle < 2.0) {
        if (!flag(features.z, 16.0) && !flag(features.x, 8.0))
            p.a = min(p.a + dith.y, 255.0);
        shade.a = min(shade.a + dith.y, 255.0);
    }
    if (cvg == 0.0 && flag(features.z, 2.0))
        discard;
    if (cycle < 3.0) {
        if (cycle == 2.0 && extra.y == 2.0 && mod(mode.z, 2.0) > 0.5) {
            if (p.a == 0.0)
                discard;
        } else {
            float a = compare_alpha >= 0.0 ? compare_alpha : p.a;
            if ((mode.z == 1.0 && a < settings.z) || (mode.z == 3.0 && a < noise()))
                discard;
        }
    }
    // Copy/fill cannot reach the blender or colour coverage destination.
    // Keep raw Z clears in the packed depth plane without sampling old Z.
    if (cycle >= 2.0) {
        if (flag(features.w, 4.0) && depth_pass > 0.5)
            gl_FragColor = data(mod(floor(vPixel.x), 2.0) == 0.0 ? 9.0 : 10.0) / 255.0;
        else {
            float alpha = extra.y == 3.0 ? (cycle == 3.0 ? p.a : mod(cvg - 1.0, 8.0) * 32.0) / 255.0
                          : extra.y == 2.0 ? (p.a > 0.5 ? 1.0 : 0.125)
                                           : cvg / 8.0;
            gl_FragColor = vec4(p.rgb / 255.0, alpha);
        }
        return;
    }
    float width = number(115.0);
    vec2 uv = vec2(vPixel.x / width, 1.0 - vPixel.y / vPixel.w);
    vec4 previous = texture2D(color_memory, uv),
         stored = floor(texture2D(depth_memory, uv) * 255.0 + 0.5);
    vec4 previous_fixed = floor(previous * 255.0 + 0.5);
    bool intensity16 = extra.y == 2.0 && extra.w != 0.0;
    float memory_cvg =
        flag(features.z, 1.0) && extra.y >= 2.0
            ? (intensity16
                   ? mod(floor(previous_fixed.g / 8.0), 4.0) * 2.0 + floor(previous_fixed.b / 128.0)
               : extra.y == 3.0 ? floor(previous_fixed.a / 32.0)
                                : clamp(floor(previous.a * 8.0 + 0.5) - 1.0, 0.0, 7.0))
            : 7.0;
    float z;
#if PRECISE_Z
    z = (flag(features.y, 2.0) && extra.z == 0.0
             ? triangle_span_z(geometric_cvg, offset)
             : clamp(floor(vZ.x + vZ.y * offset.x + vZ.z * offset.y), 0.0, 262143.0)) /
        262143.0;
#else
    z = clamp(vZ.x + vZ.y * offset.x + vZ.z * offset.y, 0.0, 1.0);
#endif
    float zold = unpack_z(stored), dzexp = vZ.w;
    float oldexp = mod(floor(stored.a / 8.0), 16.0);
    float blend_exp = flag(features.w, 1.0) ? oldexp : 15.0;
    blend_shifts = clamp(vec2(dzexp - blend_exp, blend_exp - dzexp), 0.0, 4.0);
    float dzmem = exp2(max(oldexp, MIN_DZ_EXP) - 18.0), zprec = 0.0;
    if (zold >= 0.5)
        zprec = 1.0;
    if (zold >= 0.75)
        zprec = 2.0;
    if (zold >= 0.875)
        zprec = 3.0;
    bool coplanar = false;
    if (zprec < 3.0) {
        if (oldexp != 15.0)
            dzmem = max(dzmem * 2.0, exp2(-14.0 - zprec));
        else {
            coplanar = true;
            dzmem = 0.2499961853;
        }
    }
    float dzbase = exp2(floor(log2(max(exp2(max(dzexp, MIN_DZ_EXP) - 18.0), dzmem)))),
          tolerance = dzbase * 8.0 * (262144.0 / 262143.0);
    bool farther = coplanar || z + tolerance >= zold, nearer = coplanar || z - tolerance <= zold,
         infront = z < zold;
    bool overflow = memory_cvg + cvg >= 8.0, maxz = zold >= 1.0;
    bool accepted = maxz || (overflow ? infront : nearer);
    if (extra.x == 2.0)
        accepted = infront || maxz;
    if (extra.x == 3.0)
        accepted = farther && nearer && !maxz;
    if (flag(features.w, 4.0)) {
        if (depth_pass > 0.5)
            gl_FragColor = data(mod(floor(vPixel.x), 2.0) == 0.0 ? 9.0 : 10.0) / 255.0;
        else
            gl_FragColor =
                vec4(p.rgb / 255.0, extra.y == 3.0 ? p.a / 255.0 : (p.a > 0.5 ? 1.0 : 0.125));
        return;
    }
    if (flag(features.w, 1.0) && !accepted)
        discard;
    if (flag(features.w, 1.0) && extra.x == 1.0 && infront && farther && overflow) {
        // Split the quotient to keep intermediate values in mediump range.
        float coefficient = mod(
            floor(mod(zold, dzbase * 16.0) / dzbase) - floor(mod(z, dzbase * 16.0) / dzbase), 16.0);
        cvg = min(floor(coefficient * cvg / 8.0), 8.0);
        if (cvg == 0.0 && flag(features.z, 2.0))
            discard;
    }
    bool blend_farther = !flag(features.w, 1.0) || farther;
    float destination = mod(cvg - 1.0, 8.0);
    if (cycle < 2.0) {
        if (mode.w == 0.0)
            destination = (flag(features.z, 4.0) ||
                           (flag(features.z, 2.0) && blend_farther && memory_cvg + cvg < 8.0))
                              ? min(memory_cvg + cvg, 7.0)
                              : mod(cvg - 1.0, 8.0);
        else if (mode.w == 1.0)
            destination = mod(memory_cvg + cvg, 8.0);
        else if (mode.w == 2.0)
            destination = 7.0;
        else
            destination = memory_cvg;
    }
    if (depth_pass > 0.5) {
        if (flag(features.w, 2.0))
            gl_FragColor = pack_z(z, dzexp, destination);
        else
            gl_FragColor =
                vec4(stored.rgb, oldexp * 8.0 + destination + (stored.a >= 128.0 ? 128.0 : 0.0)) /
                255.0;
        return;
    }
    if (cycle < 2.0) {
        vec4 memory_value = vec4(previous_fixed.rgb, memory_cvg * 32.0);
        if (intensity16)
            memory_value.rgb =
                vec3(floor(previous_fixed.r / 8.0) * 8.0 + floor(previous_fixed.g / 32.0));
        if (extra.y == 2.0 && extra.w == 0.0)
            memory_value.rgb = floor(memory_value.rgb / 8.0) * 8.0;
        bool force = flag(features.z, 4.0), aa = flag(features.z, 2.0);
        if (cycle == 1.0)
            p = blend_cycle(0.0, p, memory_value, shade, force, false);
        if (flag(features.z, 32.0) && memory_cvg + cvg < 8.0)
            p.rgb = blend_rgb(data(cycle == 1.0 ? 5.0 : 4.0).z, p, memory_value);
        else if (force || (aa && blend_farther && memory_cvg + cvg < 8.0)) {
            p = blend_cycle(cycle == 1.0 ? 1.0 : 0.0, p, memory_value, shade, force, true);
        } else
            p.rgb = blend_rgb(data(cycle == 1.0 ? 5.0 : 4.0).x, p, memory_value);
    }
    if (cycle < 2.0 && dither_modes.x != 3.0)
        p.rgb = dither_rgb(p.rgb, dith.x, dither_modes.x);
    float stored_alpha = extra.y == 3.0 ? destination * 32.0 / 255.0 : (destination + 1.0) / 8.0;
    if (intensity16 && cycle < 2.0) {
        p.rgb = vec3(floor(p.r / 8.0), mod(p.r, 8.0) * 4.0 + floor(destination / 2.0),
                     mod(destination, 2.0) * 16.0) *
                8.0;
        stored_alpha = 0.125;
    } else if (cycle == 3.0 && extra.y == 3.0)
        stored_alpha = p.a / 255.0;
    else if (cycle >= 2.0 && extra.y == 2.0)
        stored_alpha = p.a > 0.5 ? 1.0 : 0.125;
    gl_FragColor = vec4(p.rgb / 255.0, stored_alpha);
}
)";

inline constexpr const char *present_vertex = R"(
attribute vec2 position;
varying P vec2 scan;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    scan = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
}
)";
inline const std::string present_fragment = std::string(R"(
varying P vec2 scan;
uniform sampler2D source;
uniform vec4 image_info, layout_info, timing, active_scan, scales, flags;
uniform float vi_pass;
uniform vec2 filter_config;
vec2 vi_center;
vec4 fetch(vec2 p) {
    float scale = image_info.z;
    vec2 native = floor(p / scale), sub = p - native * scale;
    vec2 size = image_info.xy / scale;
    float column = layout_info.z + native.x + native.y * layout_info.y;
    float row = layout_info.w + native.y * layout_info.x + floor(column / size.x);
    if (vi_pass == 1.0) {
        column = native.x + (native.y - vi_center.y) * layout_info.y;
        row = vi_center.y + (native.y - vi_center.y) * layout_info.x + floor(column / size.x);
    }
    vec2 pixel = vec2(mod(column, size.x), row);
    if (row < 0.0)
        pixel = vec2(0.0);
    if (row >= size.y)
        pixel = size - 1.0;
    pixel = pixel * scale + sub;
    pixel.y = image_info.y - 1.0 - pixel.y;
    vec4 value =
        texture2D(source, (clamp(pixel, vec2(0.0), image_info.xy - 1.0) + 0.5) / image_info.xy);
    if (vi_pass != 2.0 && image_info.w == 2.0)
        value.rgb = floor(floor(value.rgb * 255.0 + 0.5) / 8.0) * 8.0 / 255.0;
    if (vi_pass != 2.0 && image_info.w == 3.0)
        value.a = (floor(floor(value.a * 255.0 + 0.5) / 32.0) + 1.0) / 8.0;
    return value;
}
vec4 vi_fetch(vec2 p) {
    return fetch(p);
}
float vi_scale() {
    return image_info.z;
}
float vi_aa() {
    return flags.x;
}
bool vi_aa_full() {
    return filter_config.x > 0.0;
}
bool vi_resampling() {
    return filter_config.y > 0.0;
}
bool vi_restore() {
    return timing.z > 0.0;
}
bool vi_divot() {
    return flags.y > 0.0;
}
bool vi_precomputed() {
    return vi_pass == 2.0;
}
)") + glrdp_vi::filter + R"(
void main() {
    if (vi_pass == 1.0) {
        vec2 p = floor(scan * image_info.xy);
        vi_center = floor(p / image_info.z);
        gl_FragColor = vi_filter_tap(p);
        return;
    }
    vec2 field = vec2(640.0, timing.w),
         start = vec2(active_scan.x - timing.x, (active_scan.z - timing.y) * 0.5);
    vec2 local = scan * field - start,
         span = vec2(active_scan.y - active_scan.x, (active_scan.w - active_scan.z) * 0.5);
    if (local.x < (start.x < 0.0 ? 0.0 : 8.0) ||
        local.x >= span.x - (start.x + span.x > 640.0 ? 0.0 : 7.0) || local.y < 0.0 ||
        local.y >= span.y) {
        gl_FragColor = vec4(0, 0, 0, 1);
        return;
    }
    if (image_info.z == 1.0)
        local = floor(local);
    vec2 pos =
        (vec2(scales.y + local.x * scales.x, scales.w + local.y * scales.z) + 0.5) * image_info.z -
        0.5;
    vec4 c = vi_resample(pos);
    if (flags.z > 0.0)
        c.rgb = sqrt(max(c.rgb, vec3(0.0)));
    if (flags.w > 0.0)
        c.rgb +=
            vec3(fract(sin(dot(floor(scan * field), vec2(12.9898, 78.233))) * 4375.8545) / 255.0);
    gl_FragColor = vec4(clamp(c.rgb, 0.0, 1.0), 1.0);
}
)";
} // namespace glrdp_es2
