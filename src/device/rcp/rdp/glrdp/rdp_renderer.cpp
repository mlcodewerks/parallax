#include "rdp_renderer.hpp"
#include "rdp_combiner_usage.hpp"
#include "vi_filter_shader.hpp"
#include "gl_state_guard.hpp"
#include "rdp_tmem.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
std::int64_t floor_div(std::int64_t n, std::int64_t d) {
    return n >= 0 ? n / d : -(((-n) + d - 1) / d);
}
std::int64_t edge(std::int32_t x, std::int32_t dx, std::int32_t y, std::int32_t y0) {
    return std::int64_t(x) + floor_div(std::int64_t(dx) * (std::int64_t(y) - y0), 4);
}
GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
        std::string log(std::max(n, 1), '\0');
        glGetShaderInfoLog(s, n, nullptr, log.data());
        glDeleteShader(s);
        throw std::runtime_error(log);
    }
    return s;
}
GLuint link(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs),
           p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
        std::string log(std::max(n, 1), '\0');
        glGetProgramInfoLog(p, n, nullptr, log.data());
        glDeleteProgram(p);
        throw std::runtime_error(log);
    }
    return p;
}
float f16(std::int64_t v) {
    return float(double(v) / 65536.0);
}
float texture_shift(float coordinate, unsigned shift) {
    return shift <= 10 ? coordinate / float(1u << shift) : coordinate * float(1u << (16 - shift));
}
float fy(std::int32_t v) {
    return float(double(v) / 4.0);
}
std::uint64_t hash_mix(std::uint64_t h, std::uint64_t v) {
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ull;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebull;
    v ^= v >> 31;
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}
std::uint64_t tile_sig(const RDPState &s, std::uint8_t i) {
    const auto &t = s.tiles[i & 7];
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = hash_mix(h, s.tmem_generation);
    h = hash_mix(h, std::uint64_t(t.format));
    h = hash_mix(h, std::uint64_t(t.size));
    h = hash_mix(h, t.line);
    h = hash_mix(h, t.tmem);
    h = hash_mix(h, t.palette);
    h = hash_mix(h, t.sl);
    h = hash_mix(h, t.tl);
    h = hash_mix(h, t.sh);
    h = hash_mix(h, t.th);
    h = hash_mix(h, t.mask_s | (std::uint64_t(t.shift_s) << 8) | (std::uint64_t(t.clamp_s) << 16) |
                        (std::uint64_t(t.mirror_s) << 17));
    h = hash_mix(h, t.mask_t | (std::uint64_t(t.shift_t) << 8) | (std::uint64_t(t.clamp_t) << 16) |
                        (std::uint64_t(t.mirror_t) << 17));
    h = hash_mix(h, std::uint64_t((s.other_modes >> 46) & 3));
    h = hash_mix(h, rdp_linear_copy_tile(s, i));
    return h;
}
void decode_combine(std::uint64_t w, int rgb0[4], int a0[4], int rgb1[4], int a1[4]) {
    rgb0[0] = int((w >> 52) & 15);
    rgb0[2] = int((w >> 47) & 31);
    a0[0] = int((w >> 44) & 7);
    a0[2] = int((w >> 41) & 7);
    rgb1[0] = int((w >> 37) & 15);
    rgb1[2] = int((w >> 32) & 31);
    rgb0[1] = int((w >> 28) & 15);
    rgb1[1] = int((w >> 24) & 15);
    a1[0] = int((w >> 21) & 7);
    a1[2] = int((w >> 18) & 7);
    rgb0[3] = int((w >> 15) & 7);
    a0[1] = int((w >> 12) & 7);
    a0[3] = int((w >> 9) & 7);
    rgb1[3] = int((w >> 6) & 7);
    a1[1] = int((w >> 3) & 7);
    a1[3] = int(w & 7);
}

void apply_rdp_scissor(const RDPState &s, int native_height, int scale) {
    if (s.scissor_xl > s.scissor_xh && s.scissor_yl > s.scissor_yh) {
        const int x0 = int(s.scissor_xh) >> 2;
        const int y0 = int(s.scissor_yh) >> 2;
        const int x1 = (int(s.scissor_xl) + 3) >> 2;
        const int y1 = (int(s.scissor_yl) + 3) >> 2;
        const int width = std::max(0, x1 - x0);
        const int height = std::max(0, y1 - y0);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0 * scale, std::max(0, native_height - y1) * scale, width * scale,
                  height * scale);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}
} // namespace

bool RDPRenderer::initialize(N64Memory *m) {
    memory_ = m;
    fb_.set_memory(m);
    fb_.set_readback_callback([this](std::uint32_t address, std::uint32_t size) {
        gpu_depth_.flush_cpu_write(address, size, *memory_, true);
        gpu_depth_.invalidate_range(address, size);
    });
    const char *vs = R"(#version 330 core
layout(location = 0) in vec3 p;
layout(location = 1) in vec2 uv;
layout(location = 3) in float inverse_w;
layout(location = 2) in vec4 color;
uniform vec2 viewport;
out vec3 fuvq;
out vec4 shade;
void main() {
    vec2 q = vec2(p.x / viewport.x * 2.0 - 1.0, 1.0 - p.y / viewport.y * 2.0);
    gl_Position = vec4(q, p.z, 1.0);
    fuvq = vec3(uv * inverse_w, inverse_w);
    shade = color;
}
)";
    const char *fs = R"(#version 330 core
in vec3 fuvq;
vec2 fuv;
in vec4 shade;
layout(location = 0) out vec4 outColor;
layout(location = 2) out uint depthStateOut;
layout(location = 3) out uint depthAcceptOut;

uniform sampler2D mip_tiles[8];
uniform sampler2D tile_atlas;
uniform ivec4 tile_region[8];

uniform sampler2D fbtex;
uniform bool framebuffer_rgba16;
uniform usampler2D depth_state_texture;
uniform usampler2D depth_acceptance_mask;
uniform ivec4 triangle_z_fixed; // Z18.13: start, dx, de, dy

uniform ivec4 rgb0;
uniform ivec4 alpha0;
uniform ivec4 rgb1;
uniform ivec4 alpha1;
uniform ivec4 blender0;
uniform ivec4 blender1;

uniform vec4 primitive_color;
uniform vec4 environment_color;
uniform vec4 fog_color;
uniform vec4 blend_color;
uniform vec4 key_center;
uniform vec4 key_scale;
uniform ivec4 convert_coefficients;
uniform ivec4 texture_convert_modes;
uniform int convert_k4_fixed;
uniform int convert_k5_fixed;

uniform vec2 viewport;
uniform float internalScale;
uniform float lodDerivativeScale;
uniform int alpha_threshold_fixed;
uniform bool fill_16bit_mode;
uniform ivec4 fill_color_even;
uniform ivec4 fill_color_odd;
uniform int fill_image_format;
uniform int fill_image_size;
uniform uint noise_seed;
uniform float primitive_lod_fraction;
uniform int primitive_min_lod;
uniform vec4 tile_transform[8];
uniform vec4 tile_clamp[8];
uniform ivec4 tile_wrap[8]; // mask S/T, clamp/mirror flags S/T
uniform int max_mip_level;
uniform bool triangle_coverage;
uniform vec4 rectangle_bounds;
uniform ivec4 edge_y; // YH, YM, YL, left-major
uniform ivec4 edge_x; // XH, XM, XL
uniform ivec4 edge_slope;
uniform ivec4 triangle_texture_fixed[3];
uniform ivec4 triangle_shade_fixed[4];
uniform bool triangle_shade_enable;
uniform vec4 triangle_texture_plane[3];
uniform vec4 triangle_z_plane;
uniform vec2 triangle_origin;
vec2 raster_offset = vec2(0.0);
float primitive_coverage = 1.0;
uniform int cycle_type;
uniform int coverage_mode;
uniform bool fast_blending;
uniform bool fast_combiner;
uniform bool accurate_shading;
uniform bool accurate_texcoords;
uniform float copy_start_x;
float copy_lane = 0.0;
uniform int alpha_compare_mode;
uniform int texture_filter_mode;
uniform int coverage_destination;
uniform bool texture_enable;
uniform bool texture1_enable;
uniform bool lod_fraction_used;
uniform bool tex0_framebuffer;
uniform bool tex1_framebuffer;
uniform bool lod_enable;
uniform bool texture_detail;
uniform bool texture_sharpen;
uniform bool image_read_enable;
uniform bool antialias_enable;
uniform bool force_blend;
uniform bool color_on_coverage;
uniform ivec4 alpha_scissor_pixels;
uniform ivec4 dither_modes;
uniform bool color_key_enable;
uniform bool coverage_times_alpha;
uniform bool alpha_coverage_select;
uniform bool depth_state_pass;
uniform bool combined_depth_color;
uniform bool depth_acceptance_enable;
uniform bool raw_z_fill;
uniform bool z_compare_enable;
uniform bool z_update_enable;
uniform bool primitive_z_source;
uniform int z_mode;
uniform uint primitive_z_fixed;
uniform uint primitive_dz_fixed;
uniform uint fill_color_word;

uint noise_byte() {
    uvec2 p = uvec2(ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0))));
    uint x = p.x * 0x1f123bb5u ^ p.y * 0x5f356495u ^ 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x & 255u;
}
float random_noise() {
    return float(noise_byte()) / 255.0;
}

int tile_axis(int p, int size, int mask, int mirror) {
    if ((mirror & 1) != 0 || mask == 0)
        p = clamp(p, 0, size - 1);
    if (mask > 0) {
        int period = 1 << mask;
        if ((mirror & 2) != 0 && (p & period) != 0)
            p = ~p;
        p &= period - 1;
    }
    // Decoded tiles contain the addressable TMEM footprint.
    return clamp(p, 0, size - 1);
}
ivec2 tile_address(ivec2 p, ivec2 size, ivec4 wrap) {
    return ivec2(tile_axis(p.x, size.x, wrap.x, wrap.z), tile_axis(p.y, size.y, wrap.y, wrap.w));
}
vec4 tile_fetch(sampler2D tex, ivec2 p, ivec4 region) {
    return texelFetch(tex, p + region.xy, 0);
}
vec4 sample_texture(sampler2D tex, vec2 uv, ivec4 wrap, int texture_cycle, ivec4 region) {
    ivec2 size = region.zw;
    // RDP coordinates address texels directly; filtering starts at the texel,
    // without the normalized GL sampling half-texel subtraction.
    vec2 p = uv;
    ivec2 base = ivec2(floor(p));
    if (cycle_type == 2 && (wrap.z & 16) != 0) {
        ivec2 coordinate = base;
        if (wrap.x > 0) {
            int period = 1 << min(wrap.x, 10);
            if ((wrap.z & 2) != 0 && (coordinate.x & period) != 0)
                coordinate.x = ~coordinate.x;
            coordinate.x &= period - 1;
        }
        if (wrap.y > 0) {
            int period = 1 << min(wrap.y, 10);
            if ((wrap.w & 2) != 0 && (coordinate.y & period) != 0)
                coordinate.y = ~coordinate.y;
            coordinate.y &= period - 1;
        }
        int index = (coordinate.x + coordinate.y * (wrap.w >> 2) * 4) & 2047;
        if ((coordinate.y & 1) != 0)
            index ^= 2;
        return tile_fetch(tex, ivec2(index & 511, index >> 9), region);
    }
    if (texture_filter_mode < 2 || cycle_type == 2)
        return tile_fetch(tex, tile_address(base, size, wrap), region);
    vec2 f = floor(fract(p) * 32.0) / 32.0;
    if ((wrap.z & 8) != 0 && texture_convert_modes[texture_cycle] == 0) {
        vec4 a = tile_fetch(tex, tile_address(base, size, wrap), region);
        vec4 d = tile_fetch(tex, tile_address(base + ivec2(2, 1), size, wrap), region);
        vec2 rg = vec2(floor(f.x * 16.0) / 32.0 + float(base.x & 1) * 0.5, f.y);
        return vec4(rg.x + rg.y >= 1.0 ? d.rg : a.rg, f.x + f.y >= 1.0 ? d.ba : a.ba);
    }
    if ((wrap.z & 12) != 0) {
        // YUV TLUT descriptors fetch the next column two byte indices away.
        // Red/green use the half-rate horizontal interpolation phase.
        vec4 a = tile_fetch(tex, tile_address(base, size, wrap), region);
        vec4 b = tile_fetch(tex, tile_address(base + ivec2(2, 0), size, wrap), region);
        vec4 c = tile_fetch(tex, tile_address(base + ivec2(0, 1), size, wrap), region);
        vec4 d = tile_fetch(tex, tile_address(base + ivec2(2, 1), size, wrap), region);
        vec2 rg = vec2(floor(f.x * 16.0) / 32.0 + float(base.x & 1) * 0.5, f.y);
        vec4 low = a + (b - a) * f.x + (c - a) * f.y;
        vec4 high = d + (c - d) * (1.0 - f.x) + (b - d) * (1.0 - f.y);
        vec4 result = f.x + f.y < 1.0 ? low : high;
        result.rg = rg.x + rg.y < 1.0
                        ? a.rg + (b.rg - a.rg) * rg.x + (c.rg - a.rg) * rg.y
                        : d.rg + (c.rg - d.rg) * (1.0 - rg.x) + (b.rg - d.rg) * (1.0 - rg.y);
        if (texture_filter_mode == 3) {
            if (all(equal(f, vec2(0.5))))
                result.ba = (a.ba + b.ba + c.ba + d.ba) * 0.25;
            if (all(equal(rg, vec2(0.5))))
                result.rg = (a.rg + b.rg + c.rg + d.rg) * 0.25;
        }
        return result;
    }
    if (texture_filter_mode == 3 && all(equal(f, vec2(0.5)))) {
        return (tile_fetch(tex, tile_address(base, size, wrap), region) +
                tile_fetch(tex, tile_address(base + ivec2(1, 0), size, wrap), region) +
                tile_fetch(tex, tile_address(base + ivec2(0, 1), size, wrap), region) +
                tile_fetch(tex, tile_address(base + ivec2(1, 1), size, wrap), region)) *
               0.25;
    }
    ivec2 p0 = base, p1 = base + ivec2(1, 0), p2 = base + ivec2(0, 1);
    if (f.x + f.y >= 1.0) {
        p0 = base + ivec2(1, 1);
        p1 = base + ivec2(0, 1);
        p2 = base + ivec2(1, 0);
        f = vec2(1.0) - f;
    }
    vec4 a = tile_fetch(tex, tile_address(p0, size, wrap), region);
    vec4 b = tile_fetch(tex, tile_address(p1, size, wrap), region);
    vec4 c = tile_fetch(tex, tile_address(p2, size, wrap), region);
    return a + (b - a) * f.x + (c - a) * f.y;
}

ivec4 to_fixed8(vec4 value) {
    return ivec4(clamp(floor(value * 255.0 + 0.5), vec4(0.0), vec4(255.0)));
}

vec4 from_fixed8(ivec4 value) {
    return vec4(clamp(value, ivec4(0), ivec4(255))) / 255.0;
}

int fixed_noise() {
    return int(noise_byte());
}

vec4 sample_mip_tile(int level, vec2 st, int texture_cycle) {
    vec2 uv = st * tile_transform[level].xy + tile_transform[level].zw;
    if (cycle_type == 2)
        uv.x = floor(uv.x) + copy_lane;
    ivec4 wrap = tile_wrap[level];
    // RDP maximum tests use absolute tile endpoints before subtracting SL/TL.
    // For inverted bounds, the unsigned clamp difference wraps to 10 bits;
    // clamping relative coordinates to an ordinary host rectangle is wrong.
    if (cycle_type != 2) {
        vec4 limits = tile_clamp[level];
        if (limits.x < 0.0 && ((wrap.z & 1) != 0 || wrap.x == 0) && uv.x >= limits.x)
            uv.x = limits.z;
        if (limits.y < 0.0 && ((wrap.w & 1) != 0 || wrap.y == 0) && uv.y >= limits.y)
            uv.y = limits.w;
    }
    if (tile_region[level].z > 0)
        return sample_texture(tile_atlas, uv, wrap, texture_cycle, tile_region[level]);
    switch (level) {
    case 0:
        return sample_texture(mip_tiles[0], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[0], 0)));
    case 1:
        return sample_texture(mip_tiles[1], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[1], 0)));
    case 2:
        return sample_texture(mip_tiles[2], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[2], 0)));
    case 3:
        return sample_texture(mip_tiles[3], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[3], 0)));
    case 4:
        return sample_texture(mip_tiles[4], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[4], 0)));
    case 5:
        return sample_texture(mip_tiles[5], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[5], 0)));
    case 6:
        return sample_texture(mip_tiles[6], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[6], 0)));
    default:
        return sample_texture(mip_tiles[7], uv, wrap, texture_cycle, ivec4(0, 0, textureSize(mip_tiles[7], 0)));
    }
}

// RDP LOD is the largest S/T delta in S10.5 across a console pixel,
// before the selected tile's shift/mask. Preserve the ninth fraction bit
// used for negative sharpening coefficients in the fixed-point combiner.
void rdp_lod(vec2 st, vec3 q, vec3 dx, vec3 dy, out int first, out int second, out int fraction) {
    vec3 qx = q + dx, qy = q + dy;
    vec2 sx = qx.xy / max(qx.z, 1e-8), sy = qy.xy / max(qy.z, 1e-8);
    ivec2 here = ivec2(floor(st * 32.0));
    ivec2 delta_x = ivec2(floor(sx * 32.0)) - here;
    ivec2 delta_y = ivec2(floor(sy * 32.0)) - here;
    // Hardware takes one's complement for negative differences.
    delta_x = ivec2(delta_x.x < 0 ? ~delta_x.x : delta_x.x, delta_x.y < 0 ? ~delta_x.y : delta_x.y);
    delta_y = ivec2(delta_y.x < 0 ? ~delta_y.x : delta_y.x, delta_y.y < 0 ? ~delta_y.y : delta_y.y);
    int lod = max(max(delta_x.x, delta_x.y), max(delta_y.x, delta_y.y));
    bool overflow = lod >= 16384 || q.z <= 0.0 || qx.z <= 0.0 || qy.z <= 0.0;
    bool magnify = false, distant = false;
    int level = 0;
    if (overflow) {
        distant = true;
        fraction = 255;
    } else if (lod < 32 || lod < primitive_min_lod) {
        magnify = true;
        distant = max_mip_level == 0;
        if (!texture_sharpen && !texture_detail)
            fraction = distant ? 255 : 0;
        else {
            fraction = max(lod, primitive_min_lod) << 3;
            if (texture_sharpen)
                fraction |= 256;
        }
    } else {
        int log_input = (lod >> 5) & 255;
        for (int bit = 1; bit < 8; ++bit)
            if (log_input >= (1 << bit))
                level = bit;
        distant = max_mip_level == 0 || (lod & 0x6000) != 0 || level >= max_mip_level;
        fraction =
            (!texture_sharpen && !texture_detail && distant) ? 255 : ((lod << 3) >> level) & 255;
    }
    if (distant)
        level = max_mip_level;
    if (!texture_detail) {
        first = level;
        second = (distant || (!texture_sharpen && magnify)) ? first : (first + 1) & 7;
    } else {
        first = (level + (magnify ? 0 : 1)) & 7;
        second = (level + ((!distant && !magnify) ? 2 : 1)) & 7;
    }
}

ivec4 yuv_convert_fixed(ivec4 value) {
    ivec3 rgb =
        value.b +
        ivec3((convert_coefficients.x * value.g + 128) >> 8,
              (convert_coefficients.y * value.r + convert_coefficients.z * value.g + 128) >> 8,
              (convert_coefficients.w * value.r + 128) >> 8);
    rgb &= ivec3(511);
    // A/B/D combiner inputs extend only the 384..511 range as negative.
    rgb = ivec3(rgb.r >= 384 ? rgb.r - 512 : rgb.r, rgb.g >= 384 ? rgb.g - 512 : rgb.g,
                rgb.b >= 384 ? rgb.b - 512 : rgb.b);
    return ivec4(rgb, value.b & 511);
}
ivec4 texture_fixed(vec4 value, int level, int cycle) {
    ivec4 raw = to_fixed8(value);
    if (!texture_enable || (tile_wrap[level & 7].z & 8) == 0)
        return raw;
    raw.rg -= ivec2(128);
    return texture_convert_modes[cycle] != 0 ? raw : yuv_convert_fixed(raw);
}

ivec3 rgb_a_fixed(int mux, ivec4 combined, ivec4 t0, ivec4 t1, ivec4 primitive, ivec4 shade8,
                  ivec4 environment) {
    if (mux == 0)
        return combined.rgb;
    if (mux == 1)
        return t0.rgb;
    if (mux == 2)
        return t1.rgb;
    if (mux == 3)
        return primitive.rgb;
    if (mux == 4)
        return shade8.rgb;
    if (mux == 5)
        return environment.rgb;
    if (mux == 6)
        return ivec3(255);
    if (mux == 7)
        return ivec3(fixed_noise());
    return ivec3(0);
}

ivec3 rgb_b_fixed(int mux, ivec4 combined, ivec4 t0, ivec4 t1, ivec4 primitive, ivec4 shade8,
                  ivec4 environment) {
    if (mux == 0)
        return combined.rgb;
    if (mux == 1)
        return t0.rgb;
    if (mux == 2)
        return t1.rgb;
    if (mux == 3)
        return primitive.rgb;
    if (mux == 4)
        return shade8.rgb;
    if (mux == 5)
        return environment.rgb;
    if (mux == 6)
        return to_fixed8(key_center).rgb;
    if (mux == 7)
        return ivec3(convert_k4_fixed);
    return ivec3(0);
}

ivec3 rgb_c_fixed(int mux, ivec4 combined, ivec4 t0, ivec4 t1, ivec4 primitive, ivec4 shade8,
                  ivec4 environment, int lod8, int primitive_lod8) {
    if (mux <= 5)
        return rgb_a_fixed(mux, combined, t0, t1, primitive, shade8, environment);
    if (mux == 6)
        return to_fixed8(key_scale).rgb;
    if (mux == 7)
        return ivec3(combined.a);
    if (mux == 8)
        return ivec3(t0.a);
    if (mux == 9)
        return ivec3(t1.a);
    if (mux == 10)
        return ivec3(primitive.a);
    if (mux == 11)
        return ivec3(shade8.a);
    if (mux == 12)
        return ivec3(environment.a);
    if (mux == 13)
        return ivec3(lod8);
    if (mux == 14)
        return ivec3(primitive_lod8);
    if (mux == 15)
        return ivec3(convert_k5_fixed);
    return ivec3(0);
}

ivec3 rgb_d_fixed(int mux, ivec4 combined, ivec4 t0, ivec4 t1, ivec4 primitive, ivec4 shade8,
                  ivec4 environment) {
    if (mux == 0)
        return combined.rgb;
    if (mux == 1)
        return t0.rgb;
    if (mux == 2)
        return t1.rgb;
    if (mux == 3)
        return primitive.rgb;
    if (mux == 4)
        return shade8.rgb;
    if (mux == 5)
        return environment.rgb;
    if (mux == 6)
        return ivec3(255);
    return ivec3(0);
}

int alpha_abd_fixed(int mux, ivec4 combined, ivec4 t0, ivec4 t1, ivec4 primitive, ivec4 shade8,
                    ivec4 environment) {
    if (mux == 0)
        return combined.a;
    if (mux == 1)
        return t0.a;
    if (mux == 2)
        return t1.a;
    if (mux == 3)
        return primitive.a;
    if (mux == 4)
        return shade8.a;
    if (mux == 5)
        return environment.a;
    if (mux == 6)
        return 255;
    return 0;
}

int alpha_c_fixed(int mux, ivec4 combined, ivec4 t0, ivec4 t1, ivec4 primitive, ivec4 shade8,
                  ivec4 environment, int lod8, int primitive_lod8) {
    if (mux == 0)
        return lod8;
    if (mux == 1)
        return t0.a;
    if (mux == 2)
        return t1.a;
    if (mux == 3)
        return primitive.a;
    if (mux == 4)
        return shade8.a;
    if (mux == 5)
        return environment.a;
    if (mux == 6)
        return primitive_lod8;
    return 0;
}

ivec4 combine_cycle_fixed(ivec4 rgb_mux, ivec4 alpha_mux, ivec4 combined, ivec4 t0, ivec4 t1,
                          ivec4 primitive, ivec4 shade8, ivec4 environment, int lod8,
                          int primitive_lod8) {
    ivec3 a = rgb_a_fixed(rgb_mux.x, combined, t0, t1, primitive, shade8, environment);
    ivec3 b = rgb_b_fixed(rgb_mux.y, combined, t0, t1, primitive, shade8, environment);
    ivec3 c = rgb_c_fixed(rgb_mux.z, combined, t0, t1, primitive, shade8, environment, lod8,
                          primitive_lod8);
    ivec3 d = rgb_d_fixed(rgb_mux.w, combined, t0, t1, primitive, shade8, environment);

    if (fast_combiner) {
        // Normalized eight-bit factors and saturating arithmetic, both cycles retained.
        ivec3 rgb = ivec3(clamp(floor(vec3(a - b) * vec3(c) / 255.0 + vec3(d) + 0.5), 0.0, 255.0));
        int aa = alpha_abd_fixed(alpha_mux.x, combined, t0, t1, primitive, shade8, environment);
        int ab = alpha_abd_fixed(alpha_mux.y, combined, t0, t1, primitive, shade8, environment);
        int ac = alpha_c_fixed(alpha_mux.z, combined, t0, t1, primitive, shade8, environment, lod8,
                               primitive_lod8);
        int ad = alpha_abd_fixed(alpha_mux.w, combined, t0, t1, primitive, shade8, environment);
        return ivec4(rgb, int(clamp(floor(float(aa - ab) * float(ac) / 255.0 + float(ad) + 0.5),
                                    0.0, 255.0)));
    }
    // C is signed 9-bit: sharpening sets its high bit to extrapolate.
    c = ((c & ivec3(511)) ^ ivec3(256)) - ivec3(256);
    // Signed 9-bit A-B. The +128 implements the
    // fixed-point half-up rounding used when reducing the product to 8 bits.
    ivec3 rgb = ((a - b) * c + (d << 8) + ivec3(128)) >> 8;
    rgb &= ivec3(511);
    rgb = ivec3(rgb.r >= 384 ? 0 : min(rgb.r, 255), rgb.g >= 384 ? 0 : min(rgb.g, 255),
                rgb.b >= 384 ? 0 : min(rgb.b, 255));

    int aa = alpha_abd_fixed(alpha_mux.x, combined, t0, t1, primitive, shade8, environment);
    int ab = alpha_abd_fixed(alpha_mux.y, combined, t0, t1, primitive, shade8, environment);
    int ac = alpha_c_fixed(alpha_mux.z, combined, t0, t1, primitive, shade8, environment, lod8,
                           primitive_lod8);
    int ad = alpha_abd_fixed(alpha_mux.w, combined, t0, t1, primitive, shade8, environment);
    ac = ((ac & 511) ^ 256) - 256;
    int alpha = ((aa - ab) * ac + (ad << 8) + 128) >> 8;
    alpha = clamp(alpha, -256, 255);

    return clamp(ivec4(rgb, alpha), ivec4(0), ivec4(255));
}

ivec3 blender_color_fixed(int mux, ivec4 pixel, ivec4 memory, ivec4 blend8, ivec4 fog8) {
    if (mux == 0)
        return pixel.rgb;
    if (mux == 1)
        return memory.rgb;
    if (mux == 2)
        return blend8.rgb;
    return fog8.rgb;
}

int blender_alpha_a_fixed(int mux, ivec4 pixel, ivec4 fog8, ivec4 shade8) {
    if (mux == 0)
        return pixel.a;
    if (mux == 1)
        return fog8.a;
    if (mux == 2)
        return shade8.a;
    return 0;
}

int blender_alpha_b_fixed(int mux, int a, ivec4 memory) {
    if (mux == 0)
        return 255 - a;
    if (mux == 1)
        return memory.a;
    if (mux == 2)
        return 255;
    return 0;
}

uint dz_compress_gpu(uint value);
uint normalize_dzpix_gpu(uint sum);
uint framebuffer_depth;

int blender_divide_gpu(int numerator, int divisor) {
    // The RDP divider is a restoring eight-step circuit, not integer division.
    // Its denominator is four-bit and its numerator is eleven-bit after >>2.
    int d = (divisor >> 2) & 15, n = (numerator >> 2) & 2047;
    int invd = (~d) & 15, partial = (invd + (n >> 8) + 1) & 7, result = 0;
    for (int k = 0; k < 8; ++k) {
        int bit = (n >> (7 - k)) & 1;
        int temp =
            (result & (256 >> k)) != 0 ? invd + (partial << 1) + bit + 1 : d + (partial << 1) + bit;
        partial = temp & 7;
        if ((temp & 16) != 0)
            result |= 1 << (7 - k);
    }
    return result;
}

ivec4 blend_cycle_fixed(ivec4 mux, ivec4 pixel, ivec4 memory, ivec4 blend8, ivec4 fog8,
                        ivec4 shade8, bool final_cycle) {
    // Alpha factors lose their low three bits; the second factor receives
    // a one-unit bias after the optional memory-alpha shifts.
    int raw_a = blender_alpha_a_fixed(mux.y, pixel, fog8, shade8);
    int a = raw_a >> 3;
    int b = blender_alpha_b_fixed(mux.w, raw_a, memory) >> 3;
    if (mux.w == 1) {
        uint dz = primitive_z_source
                      ? primitive_dz_fixed
                      : normalize_dzpix_gpu(triangle_coverage ? uint(triangle_z_plane.w) : 0u);
        int incoming_exp = int(dz_compress_gpu(dz)), stored_exp = 15;
        if (z_compare_enable && (depth_acceptance_enable || combined_depth_color)) {
            ivec2 pixel = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
            stored_exp = int((framebuffer_depth >> 18u) & 15u);
        }
        a = (a >> clamp(incoming_exp - stored_exp, 0, 4)) & 60;
        b = (b >> clamp(stored_exp - incoming_exp, 0, 4)) | 3;
    }

    ivec3 p = blender_color_fixed(mux.x, pixel, memory, blend8, fog8);
    ivec3 m = blender_color_fixed(mux.z, pixel, memory, blend8, fog8);

    // Final inverse-alpha blends reject opaque pixels before the equation.
    if (final_cycle && mux.y == 0 && mux.w == 0 && pixel.a >= 255)
        return ivec4(p, pixel.a);
    int divisor = force_blend || !final_cycle ? 32 : max((a & ~3) + (b & ~3) + 4, 1);
    ivec3 numerator = p * a + m * (b + 1);

    if (fast_blending)
        return ivec4(ivec3(clamp(floor(vec3(numerator) / float(divisor)), 0.0, 255.0)), pixel.a);
    ivec3 rgb = force_blend || !final_cycle ? (numerator >> 5) & ivec3(255)
                                            : ivec3(blender_divide_gpu(numerator.r, divisor),
                                                    blender_divide_gpu(numerator.g, divisor),
                                                    blender_divide_gpu(numerator.b, divisor));
    return ivec4(rgb, pixel.a);
}

ivec2 dither_values() {
    const int magic[16] = int[16](0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0);
    const int bayer[16] = int[16](0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2);
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
    pixel.y = int(viewport.y) - 1 - pixel.y;
    int index = ((pixel.y & 3) << 2) | (pixel.x & 3);
    int matrix = dither_modes.z == 1 || dither_modes.z == 3 ? bayer[index] : magic[index];
    int rgb = dither_modes.x == 2 ? int(noise_byte()) : dither_modes.x == 3 ? 7 : matrix;
    int alpha = dither_modes.y == 3   ? 0
                : dither_modes.y == 2 ? int(noise_byte() & 7u)
                : dither_modes.y == 1 ? (matrix ^ 7)
                                      : matrix;
    return ivec2(rgb, alpha);
}
ivec3 dither_rgb(ivec3 rgb, int threshold) {
    ivec3 compare = dither_modes.x == 2
                        ? ivec3(threshold & 7, (threshold >> 3) & 7, (threshold >> 6) & 7)
                        : ivec3(threshold);
    ivec3 rounded = min((rgb & ivec3(248)) + ivec3(8), ivec3(255));
    return ivec3((rgb.r & 7) > compare.r ? rounded.r : rgb.r,
                 (rgb.g & 7) > compare.g ? rounded.g : rgb.g,
                 (rgb.b & 7) > compare.b ? rounded.b : rgb.b);
}
bool alpha_test_fixed(int alpha) {
    if (cycle_type == 3)
        return true;
    if (cycle_type == 2 && fill_image_size == 2 && (alpha_compare_mode & 1) != 0)
        return alpha > 0;
    if (alpha_compare_mode == 0)
        return true;
    if (alpha_compare_mode == 1)
        return alpha >= alpha_threshold_fixed;
    if (alpha_compare_mode == 3)
        return alpha >= int(noise_byte());
    // Mode 2 is reserved by the RDP command encoding. Treat it as disabled
    // rather than inventing an unsupported comparison rule.
    return true;
}

const uint RDP_MAX_Z = 0x3ffffu;

uint dz_compress_gpu(uint value) {
    value &= 0xffffu;
    if (value == 0u)
        return 0u;
    uint exponent = 0u;
    while ((value >>= 1u) != 0u)
        exponent++;
    return min(exponent, 15u);
}

uint dz_decompress_gpu(uint exponent) {
    return 1u << min(exponent, 15u);
}

uint normalize_dzpix_gpu(uint sum) {
    sum &= 0xffffu;
    if ((sum & 0xc000u) != 0u)
        return 0x8000u;
    if (sum == 0u)
        return 1u;
    if (sum == 1u)
        return 3u;
    for (uint bit = 0x2000u; bit != 0u; bit >>= 1u)
        if ((sum & bit) != 0u)
            return bit << 1u;
    return 1u;
}

uint largest_power_two_gpu(uint value) {
    value &= 0xffffu;
    if (value == 0u)
        return 0u;
    uint result = 1u;
    while ((value >>= 1u) != 0u)
        result <<= 1u;
    return result;
}

uint z_precision_factor_gpu(uint z) {
    uint top = (z >> 11u) & 0x7fu;
    if (top <= 0x3fu)
        return 0u;
    if (top <= 0x5fu)
        return 1u;
    if (top <= 0x6fu)
        return 2u;
    if (top <= 0x77u)
        return 3u;
    if (top <= 0x7bu)
        return 4u;
    if (top <= 0x7du)
        return 5u;
    if (top == 0x7eu)
        return 6u;
    return 7u;
}

uint z_decompress_visible_gpu(uint value) {
    uint packed_value = (value >> 2u) & 0x3fffu;
    uint exponent = (packed_value >> 11u) & 7u;
    uint mantissa = packed_value & 0x7ffu;
    const uint shifts[8] = uint[8](6u, 5u, 4u, 3u, 2u, 1u, 0u, 0u);
    const uint adds[8] =
        uint[8](0x00000u, 0x20000u, 0x30000u, 0x38000u, 0x3c000u, 0x3e000u, 0x3f000u, 0x3f800u);
    return ((mantissa << shifts[exponent]) + adds[exponent]) & RDP_MAX_Z;
}

uvec2 n64_z_compare_gpu(uint incoming, uint stored, uint incoming_dz, uint stored_dz_exponent,
                        uint mode, uint incoming_coverage, uint memory_coverage, out bool farther) {
    incoming &= RDP_MAX_Z;
    stored &= RDP_MAX_Z;
    uint dzmem = dz_decompress_gpu(stored_dz_exponent);
    bool force_coplanar = false;
    uint zprec = z_precision_factor_gpu(stored);
    if (zprec < 3u) {
        if (dzmem != 0x8000u) {
            uint modifier = 16u >> zprec;
            dzmem <<= 1u;
            if (dzmem < modifier)
                dzmem = modifier;
        } else {
            force_coplanar = true;
            dzmem = 0xffffu;
        }
    }

    uint dz_base = largest_power_two_gpu((incoming_dz | dzmem) & 0xffffu);
    uint dz_tolerance = dz_base << 3u;
    farther = force_coplanar || (incoming + dz_tolerance >= stored);
    bool overflow = ((memory_coverage + incoming_coverage) & 8u) != 0u;
    bool max_z = stored == RDP_MAX_Z;
    bool in_front = incoming < stored;
    bool nearer = force_coplanar || (int(incoming) - int(dz_tolerance) <= int(stored));

    if ((mode & 3u) == 0u)
        return uvec2((max_z || (overflow ? in_front : nearer)) ? 1u : 0u, 8u);
    if ((mode & 3u) == 1u) {
        if (!in_front || !farther || !overflow)
            return uvec2((max_z || (overflow ? in_front : nearer)) ? 1u : 0u, 8u);
        uint dzenc = dz_compress_gpu(dz_base & 0xffffu);
        uint coefficient = ((stored >> dzenc) - (incoming >> dzenc)) & 0xfu;
        uint reduced = min((coefficient * incoming_coverage) >> 3u, 8u);
        return uvec2(1u, reduced);
    }
    if ((mode & 3u) == 2u)
        return uvec2((in_front || max_z) ? 1u : 0u, 8u);
    return uvec2((farther && nearer && !max_z) ? 1u : 0u, 8u);
}

uint update_coverage_gpu(uint stored, uint incoming, int destination, bool farther) {
    uint mem = stored & 7u;
    if ((destination & 3) == 0) {
        bool blend = force_blend || (antialias_enable && farther && ((mem + incoming) & 8u) == 0u);
        return blend ? min(mem + incoming, 7u) : ((incoming - 1u) & 7u);
    }
    if ((destination & 3) == 1)
        return (mem + incoming) & 7u;
    if ((destination & 3) == 2)
        return 7u;
    return mem;
}

int stepped_edge(int x, int slope, int steps) {
    return int(uint(x & ~1) + uint((slope >> 2) & ~1) * uint(steps));
}

ivec2 fast_triangle_span(int y) {
    int major = stepped_edge(edge_x.x, edge_slope.x, y - (edge_y.x & ~3));
    int minor = y < edge_y.y ? stepped_edge(edge_x.y, edge_slope.y, y - (edge_y.x & ~3))
                             : stepped_edge(edge_x.z, edge_slope.z, y - edge_y.y);
    return edge_y.w != 0 ? ivec2(major, minor) : ivec2(minor, major);
}
bool fast_triangle_pixel(ivec2 pixel, bool first_sample_only) {
    int y = pixel.y * 4, x = pixel.x << 16;
    if (first_sample_only) {
        if (y < edge_y.x || y >= edge_y.z)
            return false;
        ivec2 span = fast_triangle_span(y);
        return x >= span.x && x < span.y;
    }
    int lo = max(y, edge_y.x), hi = min(y + 3, edge_y.z - 1);
    if (lo > hi)
        return false;
    if (y + 2 >= edge_y.x && y + 2 < edge_y.z) {
        ivec2 center = fast_triangle_span(y + 2);
        if (x + 32768 >= center.x && x + 32768 < center.y)
            return true;
    }
    ivec2 a = fast_triangle_span(lo), b = fast_triangle_span(hi);
    int left = min(a.x, b.x), right = max(a.y, b.y);
    int width = max(a.y - a.x, b.y - b.x);
    // Broad triangles use one shared sample position, so adjacent faces cannot
    // blend the same edge pixel twice. Only subpixel spans get a conservative
    // fallback, preserving very thin geometry without eight sample counts.
    if (edge_y.y > lo && edge_y.y <= hi) {
        a = fast_triangle_span(edge_y.y - 1);
        b = fast_triangle_span(edge_y.y);
        left = min(left, min(a.x, b.x));
        right = max(right, max(a.y, b.y));
        width = max(width, max(a.y - a.x, b.y - b.x));
    }
    return width < 65536 && right > left && x < right && x + 65536 > left;
}

bool coverage_sample(int row, int col) {
    if (coverage_mode == 3)
        return (row == 0 && col == 0) || (row == 2 && col == 1);
    return coverage_mode != 1 || col == ((row == 1 || row == 2) ? 1 : 0);
}
float rdp_triangle_coverage() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
    pixel.y = int(viewport.y) - 1 - pixel.y;
    if (coverage_mode == 0) {
        raster_offset =
            vec2(pixel.x, viewport.y - float(pixel.y)) * max(internalScale, 1.0) - gl_FragCoord.xy;
        return fast_triangle_pixel(pixel, cycle_type < 2 && !antialias_enable) ? 1.0 : 0.0;
    } else {
        int covered = 0;
        ivec2 first = ivec2(0);
        bool found = false;
        bool coverage_bit = false;
        for (int row = 0; row < 4; ++row) {
            if (coverage_mode == 3 && (row == 1 || row == 3)) continue;
            int y = pixel.y * 4 + row;
            if (y < edge_y.x || y >= edge_y.z)
                continue;
            int major = stepped_edge(edge_x.x, edge_slope.x, y - (edge_y.x & ~3));
            int minor = y < edge_y.y ? stepped_edge(edge_x.y, edge_slope.y, y - (edge_y.x & ~3))
                                     : stepped_edge(edge_x.z, edge_slope.z, y - edge_y.y);
            int left = edge_y.w != 0 ? major : minor, right = edge_y.w != 0 ? minor : major;
            for (int col = 0; col < 2; ++col) {
                if (!coverage_sample(row, col)) continue;
                int quarter = (row & 1) + col * 2;
                int x = (pixel.x << 16) + (quarter << 14);
                if (x >= left && x < right) {
                    covered += coverage_mode == 3 ? 4 : (coverage_mode == 1 ? 2 : 1);
                    if (row == 0 && col == 0)
                        coverage_bit = true;
                    if (!found) {
                        first = ivec2(quarter, row);
                        found = true;
                    }
                }
            }
        }
        vec2 anchor = vec2(float(pixel.x) + float(first.x) * 0.25,
                           viewport.y - float(pixel.y) - float(first.y) * 0.25);
        raster_offset = anchor * max(internalScale, 1.0) - gl_FragCoord.xy;
        // Without AA the blender's write enable is coverage-mask bit 7,
        // the first sample, rather than any nonzero sample count. Z writes
        // use the same enable as color writes.
        if (cycle_type < 2 && !antialias_enable && !coverage_bit)
            discard;
        return float(covered) * 0.125;
    }
}

const int tc_norm_point[64] =
    int[64](0x4000, 0x3f04, 0x3e10, 0x3d22, 0x3c3c, 0x3b5d, 0x3a83, 0x39b1, 0x38e4, 0x381c, 0x375a,
            0x369d, 0x35e5, 0x3532, 0x3483, 0x33d9, 0x3333, 0x3291, 0x31f4, 0x3159, 0x30c3, 0x3030,
            0x2fa1, 0x2f15, 0x2e8c, 0x2e06, 0x2d83, 0x2d03, 0x2c86, 0x2c0b, 0x2b93, 0x2b1e, 0x2aab,
            0x2a3a, 0x29cc, 0x2960, 0x28f6, 0x288e, 0x2828, 0x27c4, 0x2762, 0x2702, 0x26a4, 0x2648,
            0x25ed, 0x2594, 0x253d, 0x24e7, 0x2492, 0x243f, 0x23ee, 0x239e, 0x234f, 0x2302, 0x22b6,
            0x226c, 0x2222, 0x21da, 0x2193, 0x214d, 0x2108, 0x20c5, 0x2082, 0x2041);
const int tc_norm_slope[64] = int[64](
    0xf03, 0xf0b, 0xf11, 0xf19, 0xf20, 0xf25, 0xf2d, 0xf32, 0xf37, 0xf3d, 0xf42, 0xf47, 0xf4c,
    0xf50, 0xf55, 0xf59, 0xf5d, 0xf62, 0xf64, 0xf69, 0xf6c, 0xf70, 0xf73, 0xf76, 0xf79, 0xf7c,
    0xf7f, 0xf82, 0xf84, 0xf87, 0xf8a, 0xf8c, 0xf8e, 0xf91, 0xf93, 0xf95, 0xf97, 0xf99, 0xf9b,
    0xf9d, 0xf9f, 0xfa1, 0xfa3, 0xfa4, 0xfa6, 0xfa8, 0xfa9, 0xfaa, 0xfac, 0xfae, 0xfaf, 0xfb0,
    0xfb2, 0xfb3, 0xfb5, 0xfb5, 0xfb7, 0xfb8, 0xfb9, 0xfba, 0xfbc, 0xfbc, 0xfbe, 0xfbe);
int tc_clamp_gpu(int value) {
    if ((value & 0x40000) != 0)
        return 32767;
    if ((value & 0x20000) != 0)
        return -32768;
    int bits = value & 0x18000;
    if (bits == 0x8000)
        return 32767;
    if (bits == 0x10000)
        return -32768;
    return ((value & 65535) ^ 32768) - 32768;
}
int tc_axis_gpu(int coordinate, int reciprocal, int shift, bool carry) {
    int product = coordinate * reciprocal;
    int mask = int(0x3fffffffu & uint(-int(0x20000000u >> uint(shift))));
    int overflow = product & mask;
    int result = shift == 14 ? int(uint(product) << 1u) : product >> (13 - shift);
    int flags = 0;
    // At shift 14 the RDP shifts the output, but tests overflow direction
    // against the original product. Using the shifted sign reversed W=1.
    if (overflow != 0 && overflow != mask)
        flags = ((shift == 14 ? product : result) & (1 << 29)) == 0 ? (2 << 17) : (1 << 17);
    if (carry)
        flags |= 2 << 17;
    return tc_clamp_gpu((result & 0x1ffff) | flags);
}
float rdp_rectangle_coverage() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
    pixel.y = int(viewport.y) - 1 - pixel.y;
    if (coverage_mode == 0) {
        vec2 p = vec2(pixel);
        bool inside = antialias_enable
                          ? p.x < rectangle_bounds.z && p.x + 1.0 > rectangle_bounds.x &&
                                p.y < rectangle_bounds.w && p.y + 1.0 > rectangle_bounds.y
                          : p.x >= rectangle_bounds.x && p.x < rectangle_bounds.z &&
                                p.y >= rectangle_bounds.y && p.y < rectangle_bounds.w;
        raster_offset = vec2(p.x, viewport.y - p.y) * max(internalScale, 1.0) - gl_FragCoord.xy;
        return inside ? 1.0 : 0.0;
    } else {
        int covered = 0;
        bool first_bit = false, found = false;
        ivec2 first = ivec2(0);
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 2; ++col) {
                if (!coverage_sample(row, col)) continue;
                int quarter = (row & 1) + col * 2;
                vec2 p = vec2(pixel) + vec2(quarter, row) * 0.25;
                if (p.x >= rectangle_bounds.x && p.x < rectangle_bounds.z &&
                    p.y >= rectangle_bounds.y && p.y < rectangle_bounds.w) {
                    covered += coverage_mode == 3 ? 4 : (coverage_mode == 1 ? 2 : 1);
                    if (row == 0 && col == 0)
                        first_bit = true;
                    if (!found) {
                        first = ivec2(quarter, row);
                        found = true;
                    }
                }
            }
        if (!antialias_enable && !first_bit)
            discard;
        vec2 anchor = vec2(float(pixel.x) + float(first.x) * 0.25,
                           viewport.y - float(pixel.y) - float(first.y) * 0.25);
        raster_offset = anchor * max(internalScale, 1.0) - gl_FragCoord.xy;
        return float(covered) * 0.125;
    }
}
int alpha_coverage_at(ivec2 pixel, out ivec2 first) {
    first = ivec2(0);
    int count = 0;
    if (pixel.x < alpha_scissor_pixels.x || pixel.y < alpha_scissor_pixels.y ||
        pixel.x >= alpha_scissor_pixels.z || pixel.y >= alpha_scissor_pixels.w)
        return 0;
    if (coverage_mode == 0) {
        if (triangle_coverage)
            return fast_triangle_pixel(pixel, false) ? 8 : 0;
        vec2 p = vec2(pixel);
        return p.x < rectangle_bounds.z && p.x + 1.0 > rectangle_bounds.x &&
                       p.y < rectangle_bounds.w && p.y + 1.0 > rectangle_bounds.y
                   ? 8
                   : 0;
    } else {
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 2; ++col) {
                if (!coverage_sample(row, col)) continue;
                int quarter = (row & 1) + col * 2;
                bool inside;
                if (triangle_coverage) {
                    int y = pixel.y * 4 + row;
                    int major = stepped_edge(edge_x.x, edge_slope.x, y - (edge_y.x & ~3));
                    int minor = y < edge_y.y
                                    ? stepped_edge(edge_x.y, edge_slope.y, y - (edge_y.x & ~3))
                                    : stepped_edge(edge_x.z, edge_slope.z, y - edge_y.y);
                    int left = edge_y.w != 0 ? major : minor, right = edge_y.w != 0 ? minor : major;
                    int x = (pixel.x << 16) + (quarter << 14);
                    inside = y >= edge_y.x && y < edge_y.z && x >= left && x < right;
                } else {
                    vec2 p = vec2(pixel) + vec2(quarter, row) * 0.25;
                    inside = p.x >= rectangle_bounds.x && p.x < rectangle_bounds.z &&
                             p.y >= rectangle_bounds.y && p.y < rectangle_bounds.w;
                }
                if (inside) {
                    if (count == 0)
                        first = ivec2(quarter, row);
                    count += coverage_mode == 3 ? 4 : (coverage_mode == 1 ? 2 : 1);
                }
            }
        return count;
    }
}
int tc_span_gpu(ivec4 c, ivec2 pixel);
int shade_span_gpu(ivec4 c, ivec2 pixel, ivec2 first, int coverage) {
    if (!accurate_shading) {
        vec2 point = vec2(pixel) + vec2(first) * 0.25 - triangle_origin;
        return int(clamp(floor((float(c.x) + float(c.y) * point.x + float(c.w) * point.y) /
                              65536.0 + 0.5), 0.0, 255.0));
    }
    int value = tc_span_gpu(c, pixel);
    int result;
    if (coverage == 8)
        result = value >> 16;
    else {
        int dx = (((c.y >> 14) & 8191) ^ 4096) - 4096;
        int dy = (((c.w >> 14) & 8191) ^ 4096) - 4096;
        result = (((value >> 14) << 2) + first.x * dx + first.y * dy) >> 4;
    }
    result &= 511;
    return result >= 384 ? 0 : min(result, 255);
}
ivec2 tc_div_gpu(ivec3 coordinates, bool perspective) {
    if (!perspective)
        return coordinates.xy;
    bool carry = coordinates.z <= 0;
    int w = coordinates.z & 32767;
    int leading = 0, v = w;
    if (v >= 256) {
        v >>= 8;
        leading += 8;
    }
    if (v >= 16) {
        v >>= 4;
        leading += 4;
    }
    if (v >= 4) {
        v >>= 2;
        leading += 2;
    }
    if (v >= 2)
        leading += 1;
    int shift = 14 - leading, norm = (w << shift) & 16383;
    int slope = (tc_norm_slope[norm >> 8] | ~1023) + 1;
    int reciprocal = (((slope * ((norm & 255) << 2)) >> 10) + tc_norm_point[norm >> 8]) & 32767;
    return ivec2(tc_axis_gpu(coordinates.x, reciprocal, shift, carry),
                 tc_axis_gpu(coordinates.y, reciprocal, shift, carry));
}
int tc_span_gpu(ivec4 c, ivec2 pixel) {
    bool offset = (edge_slope.x < 0) == (edge_y.w != 0);
    int major =
        stepped_edge(edge_x.x, edge_slope.x, pixel.y * 4 + (offset ? 3 : 0) - (edge_y.x & ~3));
    int anchor = int((uint(major) >> 16u) & 4095u);
    if (anchor >= 2048)
        anchor -= 4096;
    int xfrac = (major >> 8) & 255;
    uint row = uint(c.x) + uint(pixel.y - (edge_y.x >> 2)) * uint(c.z);
    int de = c.z & ~511, dy = c.w & ~511;
    int correction = offset ? de - (de >> 2) - dy + (dy >> 2) : 0;
    uint base = ((row & ~511u) + uint(correction) - uint(xfrac) * uint((c.y >> 8) & ~1)) & ~1023u;
    return int(base + uint(pixel.x - anchor) * uint(c.y & ~31));
}

uint rdp_triangle_z_gpu() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
    pixel.y = int(viewport.y) - 1 - pixel.y;
    bool offset = (edge_slope.x < 0) == (edge_y.w != 0);
    int subline = offset ? 3 : 0;
    int major = stepped_edge(edge_x.x, edge_slope.x, pixel.y * 4 + subline - (edge_y.x & ~3));
    int anchor = int((uint(major) >> 16u) & 4095u);
    if (anchor >= 2048)
        anchor -= 4096;
    int xfrac = (major >> 8) & 255;
    uint row_z =
        uint(triangle_z_fixed.x) + uint(pixel.y - (edge_y.x >> 2)) * uint(triangle_z_fixed.z);
    int de = triangle_z_fixed.z & ~511, dy = triangle_z_fixed.w & ~511;
    int correction = offset ? (de - (de >> 2) - dy + (dy >> 2)) : 0;
    uint base =
        (row_z & ~511u) + uint(correction) - uint(xfrac) * uint((triangle_z_fixed.y >> 8) & ~1);
    base &= ~1023u;
    uint value = base + uint(pixel.x - anchor) * uint(triangle_z_fixed.y);
    int z = int((value >> 10u) & 0x3fffffu);
    if (primitive_coverage >= 1.0)
        z >>= 3;
    else {
        vec2 point = (gl_FragCoord.xy + raster_offset) / max(internalScale, 1.0);
        point.y = viewport.y - point.y;
        ivec2 first = ivec2(floor((point - vec2(pixel)) * 4.0 + 0.5));
        z = ((z << 2) + first.x * (triangle_z_fixed.y >> 10) +
             first.y * (triangle_z_fixed.w >> 10)) >>
            5;
    }
    int overflow = (z >> 17) & 3;
    return overflow == 2 ? RDP_MAX_Z : overflow == 3 ? 0u : uint(z) & RDP_MAX_Z;
}

int framebuffer_coverage_fixed(ivec4 value) {
    if (!image_read_enable || fill_image_size < 2)
        return 7;
    if (fill_image_size == 3)
        return value.a >> 5;
    if (fill_image_format != 0)
        return (((value.g >> 3) & 3) << 1) | (value.b >> 7);
    return clamp(int(floor(value.a * 8.0 / 255.0 + 0.5)) - 1, 0, 7);
}
bool depth_compare_farther;
uint depth_compare_coverage;
ivec4 framebuffer_memory;
void run_depth_state_pass(uint incoming_mask) {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (incoming_mask == 0u)
        discard;
    uint incoming_coverage = min(incoming_mask & 15u, 8u);

    uint packed_state = framebuffer_depth;
    uint stored_z = packed_state & RDP_MAX_Z;
    uint stored_dz = (packed_state >> 18u) & 15u;
    // Coverage belongs to the destination color image, even when another
    // color image shares this Z surface. With image reads disabled the RDP
    // supplies full memory coverage. Feedback was captured before this pass;
    // normalized coordinates also select the right pixel in upscaled targets.
    uint memory_coverage = uint(framebuffer_coverage_fixed(framebuffer_memory));

    if (raw_z_fill) {
        int n64_y = int(viewport.y) - 1 - pixel.y;
        uint linear_pixel =
            uint(max(n64_y, 0)) * uint(max(int(viewport.x), 1)) + uint(max(pixel.x, 0));
        uint packed16 =
            ((linear_pixel & 1u) == 0u) ? (fill_color_word >> 16u) : (fill_color_word & 0xffffu);
        uint z = z_decompress_visible_gpu(packed16);
        uint dz = ((packed16 & 3u) << 2u) | ((packed16 & 1u) != 0u ? 3u : 0u);
        depthStateOut = (z & RDP_MAX_Z) | (dz << 18u);
        depthAcceptOut = incoming_coverage;
        gl_FragDepth = float(z) / float(RDP_MAX_Z);
        return;
    }

    uint incoming_z;
    uint incoming_dz;
    if (primitive_z_source) {
        incoming_z = min((primitive_z_fixed & 0x7fffu) << 3u, RDP_MAX_Z);
        incoming_dz = primitive_dz_fixed & 0xffffu;
    } else {
        vec2 console_point = (gl_FragCoord.xy + raster_offset) / max(internalScale, 1.0);
        console_point.y = viewport.y - console_point.y;
        float corrected_z =
            triangle_coverage
                ? triangle_z_plane.x + dot(triangle_z_plane.yz, console_point - triangle_origin)
                : gl_FragCoord.z * float(RDP_MAX_Z);
        incoming_z = triangle_coverage ? rdp_triangle_z_gpu()
                                       : uint(clamp(floor(corrected_z), 0.0, float(RDP_MAX_Z)));
        float dzf = triangle_coverage ? (abs(triangle_z_plane.y) + abs(triangle_z_plane.z))
                                      : (abs(dFdx(gl_FragCoord.z)) + abs(dFdy(gl_FragCoord.z))) *
                                            float(RDP_MAX_Z);
        incoming_dz = normalize_dzpix_gpu(triangle_coverage ? uint(triangle_z_plane.w)
                                                            : uint(min(dzf * 0.125, 65535.0)));
    }

    uvec2 compare_result = uvec2(1u, 8u);
    bool farther = true;
    if (z_compare_enable)
        compare_result =
            n64_z_compare_gpu(incoming_z, stored_z, incoming_dz, stored_dz, uint(z_mode),
                              incoming_coverage, memory_coverage, farther);
    if (compare_result.x == 0u)
        discard;

    uint accepted = min(incoming_coverage, compare_result.y);
    depth_compare_farther = farther;
    depth_compare_coverage = accepted;
    if (accepted == 0u && antialias_enable)
        discard;

    uint output_z = z_update_enable ? incoming_z : stored_z;
    uint output_dz = z_update_enable ? dz_compress_gpu(incoming_dz) : stored_dz;
    uint output_coverage =
        update_coverage_gpu(memory_coverage, accepted, coverage_destination, farther);
    depthStateOut =
        (output_z & RDP_MAX_Z) | ((output_dz & 15u) << 18u) | ((output_coverage & 7u) << 22u);
    // Bit 5 disables coverage blending for pixels clearly in front of Z.
    // Keep the low coverage count and zero-coverage sentinel unchanged.
    depthAcceptOut = (accepted == 0u ? 16u : accepted) | (farther ? 0u : 32u);
    gl_FragDepth = float(output_z) / float(RDP_MAX_Z);
}

void main() {
    vec3 texture_plane = fuvq;
    vec3 qdx = dFdx(fuvq), qdy = dFdy(fuvq);
    if (texture_enable) {
        vec2 point =
            vec2(gl_FragCoord.x / internalScale, viewport.y - gl_FragCoord.y / internalScale) -
            triangle_origin;
        texture_plane =
            vec3(triangle_texture_plane[0].x + dot(triangle_texture_plane[0].yz, point),
                 triangle_texture_plane[1].x + dot(triangle_texture_plane[1].yz, point),
                 triangle_texture_plane[2].x + dot(triangle_texture_plane[2].yz, point));
        qdx = vec3(triangle_texture_plane[0].y, triangle_texture_plane[1].y,
                   triangle_texture_plane[2].y) /
              internalScale;
        qdy = -vec3(triangle_texture_plane[0].z, triangle_texture_plane[1].z,
                    triangle_texture_plane[2].z) /
              internalScale;
    }
    fuv = texture_plane.xy / max(texture_plane.z, 1e-8);
    if (triangle_coverage) {
        primitive_coverage = rdp_triangle_coverage();
        if (primitive_coverage == 0.0)
            discard;
    } else if (cycle_type < 2) {
        primitive_coverage = rdp_rectangle_coverage();
        if (primitive_coverage == 0.0)
            discard;
    }
    // A single nearest texel read permits texture-barrier read/modify/write.
    framebuffer_memory =
        to_fixed8(texture(fbtex, gl_FragCoord.xy / (viewport * max(internalScale, 1.0))));
    framebuffer_depth = (depth_state_pass || depth_acceptance_enable)
                            ? texelFetch(depth_state_texture,
                                         ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0))), 0)
                                  .r
                            : 0u;
    if (coverage_mode == 0) {
        // Surviving pixels have full coverage in this mode; skip fractional
        // shade/Z correction and coverage arithmetic.
        primitive_coverage = 1.0;
        if (depth_state_pass && !combined_depth_color && cycle_type < 2 && !coverage_times_alpha &&
            alpha_compare_mode == 0 && !color_key_enable) {
            run_depth_state_pass(8u);
            return;
        }
    }
    if (depth_state_pass && raw_z_fill) {
        uint mask = uint(clamp(int(floor(primitive_coverage * 8.0 + 0.5)), 1, 8));
        run_depth_state_pass(mask);
        return;
    }

    if (depth_acceptance_enable) {
        ivec2 depth_pixel = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
        if (texelFetch(depth_acceptance_mask, depth_pixel, 0).r == 0u)
            discard;
    }

    // Coverage offsets correct shade/Z only. Textures use the span pixel
    // origin; evaluate the homogeneous plane before perspective division.
    // Derivatives are measured before divergent LOD sampling/discards.
    // Evaluate the same span origin in native depth and scaled colour passes.
    vec2 texture_offset =
        vec2(floor(gl_FragCoord.x) - gl_FragCoord.x, ceil(gl_FragCoord.y) - gl_FragCoord.y);
    vec3 q = texture_plane + qdx * texture_offset.x + qdy * texture_offset.y;
    if (cycle_type == 2) {
        // Copy fetches four adjacent texels from each group start.
        float scale = max(internalScale, 1.0);
        vec2 pixel = vec2(floor(gl_FragCoord.x / scale), ceil(gl_FragCoord.y / scale));
        // Conservative rectangle bounds can move an edge to an integer pixel.
        // The original texture plane retains the RDP scanline origin; mixing
        // interpolated UVs with its derivatives can make T slightly negative
        // and wrap a copy fetch to an unrelated TMEM row.
        q = texture_plane + qdx * (pixel.x * scale - gl_FragCoord.x) +
            qdy * (pixel.y * scale - gl_FragCoord.y);
        copy_lane = mod(pixel.x - copy_start_x, 4.0);
        q -= qdx * (copy_lane * scale);
    }
    vec2 st = q.xy / max(q.z, 1e-8);
    if (accurate_texcoords && texture_enable && triangle_coverage && internalScale == 1.0) {
        ivec2 pixel = ivec2(floor(gl_FragCoord.xy));
        pixel.y = int(viewport.y) - 1 - pixel.y;
        ivec3 raw = ivec3(tc_span_gpu(triangle_texture_fixed[0], pixel),
                          tc_span_gpu(triangle_texture_fixed[1], pixel),
                          tc_span_gpu(triangle_texture_fixed[2], pixel));
        ivec2 divided = tc_div_gpu(raw >> 16, (texture_convert_modes.w & 2) != 0);
        st = vec2(divided) / 32.0;
    }

    int first_tile = 0, second_tile = cycle_type == 1 ? 1 : 0, lod_fixed = 0;
    bool fraction_used = lod_fraction_used;
    if (texture_enable && cycle_type < 2 && (lod_enable || fraction_used)) {
        int lod_first, lod_second;
        rdp_lod(st, q, qdx * lodDerivativeScale, -qdy * lodDerivativeScale, lod_first, lod_second,
                lod_fixed);
        if (lod_enable) {
            first_tile = lod_first;
            second_tile = cycle_type == 0 ? first_tile : lod_second;
        }
    }
    vec4 t0 = texture_enable ? sample_mip_tile(first_tile, st, cycle_type == 0 ? 1 : 0) : vec4(1.0);
    vec4 t1 = texture_enable && texture1_enable ? sample_mip_tile(second_tile, st, 1) : t0;

    ivec4 t0_fixed = texture_fixed(t0, first_tile, cycle_type == 0 ? 1 : 0);
    ivec4 t1_fixed = texture1_enable ? texture_fixed(t1, second_tile, 1) : t0_fixed;
    if (texture_enable && cycle_type == 1 && texture_convert_modes.z != 0 &&
        (tile_wrap[second_tile].z & 8) != 0)
        t1_fixed = yuv_convert_fixed(t0_fixed);
    ivec4 shade_fixed =
        to_fixed8(shade + dFdx(shade) * raster_offset.x + dFdy(shade) * raster_offset.y);
    if (triangle_coverage && triangle_shade_enable) {
        vec2 point = vec2((gl_FragCoord.x + raster_offset.x) / internalScale,
                          viewport.y - (gl_FragCoord.y + raster_offset.y) / internalScale);
        if (internalScale == 1.0) {
            ivec2 pixel =
                ivec2(floor(gl_FragCoord.x), int(viewport.y) - 1 - int(floor(gl_FragCoord.y)));
            ivec2 first = ivec2(floor((point - vec2(pixel)) * 4.0 + 0.5));
            int coverage = int(floor(primitive_coverage * 8.0 + 0.5));
            shade_fixed = ivec4(shade_span_gpu(triangle_shade_fixed[0], pixel, first, coverage),
                                shade_span_gpu(triangle_shade_fixed[1], pixel, first, coverage),
                                shade_span_gpu(triangle_shade_fixed[2], pixel, first, coverage),
                                shade_span_gpu(triangle_shade_fixed[3], pixel, first, coverage));
        } else {
            point -= triangle_origin;
            for (int i = 0; i < 4; ++i)
                shade_fixed[i] = int(clamp(floor((float(triangle_shade_fixed[i].x) +
                                                  float(triangle_shade_fixed[i].y) * point.x +
                                                  float(triangle_shade_fixed[i].w) * point.y) /
                                                     65536.0 +
                                                 0.5),
                                           0.0, 255.0));
        }
    }
    ivec4 primitive_fixed = to_fixed8(primitive_color);
    ivec4 environment_fixed = to_fixed8(environment_color);
    ivec4 fog_fixed = to_fixed8(fog_color);
    ivec4 blend_fixed = to_fixed8(blend_color);

    int primitive_lod_fixed = int(clamp(floor(primitive_lod_fraction * 255.0 + 0.5), 0.0, 255.0));
    ivec4 pixel_fixed;
    int compare_alpha = -1;
    int compare_coverage = 0;
    if (cycle_type == 3) // fill
    {
        if (fill_16bit_mode) {
            pixel_fixed = ((int(floor(gl_FragCoord.x / max(internalScale, 1.0))) & 1) == 0)
                              ? fill_color_even
                              : fill_color_odd;
        } else if (fill_image_size == 1) {
            int lane = int(floor(gl_FragCoord.x / max(internalScale, 1.0))) & 3;
            int raw = (lane == 0)   ? shade_fixed.r
                      : (lane == 1) ? shade_fixed.g
                      : (lane == 2) ? shade_fixed.b
                                    : shade_fixed.a;
            // Color-image bytes select red/green lanes; texture IA/I decoding
            // does not change how the framebuffer fill word is stored.
            pixel_fixed = ivec4(raw, raw, raw, 255);
        } else
            pixel_fixed = shade_fixed;
    } else if (cycle_type == 2) // copy
    {
        pixel_fixed = t0_fixed;
        if (fill_image_size == 2)
            pixel_fixed.a = (pixel_fixed.a & 1) != 0 ? 255 : 0;
    } else {
        pixel_fixed =
            combine_cycle_fixed(cycle_type == 1 ? rgb0 : rgb1, cycle_type == 1 ? alpha0 : alpha1,
                                ivec4(0), t0_fixed, t1_fixed, primitive_fixed, shade_fixed,
                                environment_fixed, lod_fixed, primitive_lod_fixed);

        if (cycle_type == 1) {
            if ((alpha_compare_mode & 1) != 0) {
                // Alpha compare belongs to cycle zero of the following pixel.
                // The final cycle still supplies this pixel's blend alpha.
                int direction = triangle_coverage ? (edge_y.w != 0 ? 1 : -1) : 1;
                ivec2 np = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
                np.y = int(viewport.y) - 1 - np.y;
                np.x += direction;
                ivec2 nfirst;
                compare_coverage = alpha_coverage_at(np, nfirst);
                vec3 nq = q + qdx * lodDerivativeScale * float(direction);
                vec2 nst = nq.xy / max(nq.z, 1e-8);
                if (accurate_texcoords && texture_enable && triangle_coverage &&
                    internalScale == 1.0) {
                    ivec3 raw = ivec3(tc_span_gpu(triangle_texture_fixed[0], np),
                                      tc_span_gpu(triangle_texture_fixed[1], np),
                                      tc_span_gpu(triangle_texture_fixed[2], np));
                    nst = vec2(tc_div_gpu(raw >> 16, (texture_convert_modes.w & 2) != 0)) / 32.0;
                }
                int nf = first_tile, ns = second_tile, nlod = lod_fixed;
                if (texture_enable && (lod_enable || fraction_used)) {
                    int lf, ls;
                    rdp_lod(nst, nq, qdx * lodDerivativeScale, -qdy * lodDerivativeScale, lf, ls,
                            nlod);
                    if (lod_enable) {
                        nf = lf;
                        ns = ls;
                    }
                }
                ivec4 nt0 =
                    texture_enable ? texture_fixed(sample_mip_tile(nf, nst, 0), nf, 0) : ivec4(255);
                ivec4 nt1 =
                    texture_enable ? texture_fixed(sample_mip_tile(ns, nst, 1), ns, 1) : nt0;
                ivec4 nshade = shade_fixed;
                if (triangle_coverage && triangle_shade_enable) {
                    if (internalScale == 1.0)
                        nshade.a =
                            shade_span_gpu(triangle_shade_fixed[3], np, nfirst, compare_coverage);
                    else
                        nshade.a = clamp(
                            shade_fixed.a + (triangle_shade_fixed[3].y >> 16) * direction, 0, 255);
                }
                compare_alpha =
                    combine_cycle_fixed(rgb0, alpha0, ivec4(0), nt0, nt1, primitive_fixed, nshade,
                                        environment_fixed, nlod, primitive_lod_fixed)
                        .a;
            }
            pixel_fixed =
                combine_cycle_fixed(rgb1, alpha1, pixel_fixed, t1_fixed, t0_fixed, primitive_fixed,
                                    shade_fixed, environment_fixed, lod_fixed, primitive_lod_fixed);
        }
    }

    if (color_key_enable) {
        ivec3 key_fixed = to_fixed8(key_center).rgb;
        ivec3 scale_fixed = max(to_fixed8(key_scale).rgb, ivec3(1));
        if (all(lessThanEqual(abs(pixel_fixed.rgb - key_fixed), scale_fixed)))
            discard;
    }

    int geometric_covered = int(floor(primitive_coverage * 8.0 + 0.5));
    int covered = geometric_covered;
    // The combiner promotes full alpha to 256, computes A*Cvg once and
    // passes its resulting coverage to Z comparison and color storage.
    int alpha_value = pixel_fixed.a == 255 ? 256 : pixel_fixed.a;
    int alpha_coverage = (alpha_value * geometric_covered + 4) >> 3;
    if (coverage_times_alpha && cycle_type < 2)
        covered = (alpha_coverage >> 5) & 15;
    if (alpha_coverage_select && cycle_type < 2)
        pixel_fixed.a = min(coverage_times_alpha ? alpha_coverage : geometric_covered << 5, 255);
    ivec2 dith = dither_values();
    if (compare_alpha >= 0) {
        int a = compare_alpha == 255 ? 256 : compare_alpha;
        if (!alpha_coverage_select)
            a = min(a + dith.y, 255);
        else
            a = min(coverage_times_alpha ? ((a * compare_coverage + 4) >> 3)
                                         : (compare_coverage << 5),
                    255);
        compare_alpha = a;
    }
    if (cycle_type < 2) {
        if (!alpha_coverage_select && !color_key_enable)
            pixel_fixed.a = min(alpha_value + dith.y, 255);
        shade_fixed.a = min(shade_fixed.a + dith.y, 255);
    }
    vec4 pixel = from_fixed8(pixel_fixed);
    if (!alpha_test_fixed(compare_alpha >= 0 ? compare_alpha : pixel_fixed.a) ||
        (covered == 0 && antialias_enable))
        discard;
    if (depth_state_pass) {
        run_depth_state_pass(covered == 0 ? 16u : uint(covered));
        if (!combined_depth_color)
            return;
        covered = int(depth_compare_coverage);
    }
    bool blend_farther = combined_depth_color ? depth_compare_farther : true;
    if (depth_acceptance_enable) {
        ivec2 dp = ivec2(floor(gl_FragCoord.xy / max(internalScale, 1.0)));
        uint acceptance = texelFetch(depth_acceptance_mask, dp, 0).r;
        blend_farther = (acceptance & 32u) == 0u;
        covered = min(covered, int(min(acceptance & 15u, 8u)));
        if (covered == 0 && antialias_enable)
            discard;
    }

    ivec4 memory_fixed = framebuffer_memory;
    if (framebuffer_rgba16)
        memory_fixed.rgb &= ivec3(248);

    int memory_coverage = framebuffer_coverage_fixed(memory_fixed);
    bool intensity16 = fill_image_size == 2 && fill_image_format != 0;
    if (intensity16)
        memory_fixed.rgb = ivec3((memory_fixed.r & 248) | (memory_fixed.g >> 5));
    if (cycle_type < 2) {
        // Framebuffer alpha stores normalized coverage internally. The RDP
        // blender consumes memory coverage shifted by five, not that byte.
        memory_fixed.a = memory_coverage << 5;
        bool blend_enabled = force_blend || (antialias_enable && blend_farther &&
                                             ((memory_coverage + covered) & 8) == 0);
        bool coverage_color = color_on_coverage && ((memory_coverage + covered) & 8) == 0;
        if (cycle_type == 1) {
            pixel_fixed = blend_cycle_fixed(blender0, pixel_fixed, memory_fixed, blend_fixed,
                                            fog_fixed, shade_fixed, false);
            if (coverage_color)
                pixel_fixed.rgb = blender_color_fixed(blender1.z, pixel_fixed, memory_fixed,
                                                      blend_fixed, fog_fixed);
            else if (blend_enabled)
                pixel_fixed = blend_cycle_fixed(blender1, pixel_fixed, memory_fixed, blend_fixed,
                                                fog_fixed, shade_fixed, true);
            else
                pixel_fixed.rgb = blender_color_fixed(blender1.x, pixel_fixed, memory_fixed,
                                                      blend_fixed, fog_fixed);
        } else {
            if (coverage_color)
                pixel_fixed.rgb = blender_color_fixed(blender0.z, pixel_fixed, memory_fixed,
                                                      blend_fixed, fog_fixed);
            else if (blend_enabled)
                pixel_fixed = blend_cycle_fixed(blender0, pixel_fixed, memory_fixed, blend_fixed,
                                                fog_fixed, shade_fixed, true);
            else
                pixel_fixed.rgb = blender_color_fixed(blender0.x, pixel_fixed, memory_fixed,
                                                      blend_fixed, fog_fixed);
        }
    }

    if (cycle_type < 2 && dither_modes.x != 3)
        pixel_fixed.rgb = dither_rgb(pixel_fixed.rgb, dith.x);
    pixel = from_fixed8(pixel_fixed);
    outColor = pixel;
    {
        int stored = (covered - 1) & 7;
        bool blend_enabled = force_blend || (antialias_enable && blend_farther &&
                                             ((memory_coverage + covered) & 8) == 0);
        if (cycle_type < 2) {
            if (coverage_destination == 0)
                stored = blend_enabled ? min(memory_coverage + covered, 7) : stored;
            else if (coverage_destination == 1)
                stored = (memory_coverage + covered) & 7;
            else if (coverage_destination == 2)
                stored = 7;
            else
                stored = memory_coverage;
        }
        if (intensity16 && cycle_type < 2) {
            // Keep the FBO in physical RGB5551 representation. Non-RGBA
            // sixteen-bit writes store intensity plus visible coverage,
            // with zero hidden bits; raw copy/fill words retain their layout.
            int r = pixel_fixed.r;
            outColor =
                vec4(vec3(r >> 3, ((r & 7) << 2) | (stored >> 1), (stored & 1) << 4) * 8.0 / 255.0,
                     0.125);
        } else if (cycle_type == 3 && fill_image_size == 3)
            outColor.a = pixel.a;
        else if (cycle_type >= 2 && fill_image_size == 2)
            outColor.a = pixel.a > 0.5 ? 1.0 : 0.125;
        else
            outColor.a =
                fill_image_size == 3 ? float(stored * 32) / 255.0 : float(stored + 1) * 0.125;
    }
}
)";
    const char *pvs = R"(#version 330 core
void main() {
    vec2 p;
    if (gl_VertexID == 0)
        p = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1)
        p = vec2(3.0, -1.0);
    else
        p = vec2(-1.0, 3.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
    const std::string pfs = std::string(R"(#version 330 core
layout(location = 0) out vec4 outColor;
uniform sampler2D source;
uniform vec2 sourceSize;
uniform bool framebuffer_rgba16;
uniform bool framebuffer_rgba32;
uniform vec2 outputSize;
uniform vec2 sourceLayout;
uniform vec2 fieldSize;
uniform vec2 timingOrigin;
uniform float hStart;
uniform float hEnd;
uniform float vStart;
uniform float vEnd;
uniform float xScale;
uniform float xOffset;
uniform float yScale;
uniform float yOffset;
uniform int aaMode;
uniform bool aaFull;
uniform bool resampleEnable;
uniform bool blankOutput;
uniform bool divotEnable;
uniform bool restoreEnable;
uniform bool gammaEnable;
uniform bool gammaDitherEnable;
uniform float internalScale;
uniform int viPass;
uniform vec4 fieldState; // interlaced, lower field, retain sibling field
vec2 vi_center;

float vi_noise(ivec2 p) {
    uint x = uint(p.x) * 0x1f123bb5u ^ uint(p.y) * 0x5f356495u ^ 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x & 255u) / 255.0;
}
vec4 fetch_clamped(ivec2 p) {
    ivec2 sz = textureSize(source, 0);
    int scale = int(max(internalScale, 1.0));
    ivec2 native = ivec2(floor(vec2(p) / float(scale)));
    ivec2 sub = p - native * scale;
    ivec2 nativeSize = max(sz / scale, ivec2(1));
    int address = int(sourceLayout.y) + native.y * int(sourceLayout.x) + native.x;
    if (viPass == 1)
        address = int(vi_center.y) * nativeSize.x + int(vi_center.x) +
                  (native.y - int(vi_center.y)) * int(sourceLayout.x) + native.x - int(vi_center.x);
    address = clamp(address, 0, nativeSize.x * nativeSize.y - 1);
    ivec2 pixel = ivec2(address % nativeSize.x, address / nativeSize.x) * scale + sub;
    pixel.y = sz.y - 1 - pixel.y;
    vec4 value = texelFetch(source, clamp(pixel, ivec2(0), sz - 1), 0);
    if (viPass != 2 && framebuffer_rgba16)
        value.rgb = floor(floor(value.rgb * 255.0 + 0.5) / 8.0) * 8.0 / 255.0;
    if (viPass != 2 && framebuffer_rgba32)
        value.a = (floor(floor(value.a * 255.0 + 0.5) / 32.0) + 1.0) * 0.125;
    return value;
}
vec4 vi_fetch(vec2 p) {
    return fetch_clamped(ivec2(p));
}
float vi_scale() {
    return max(internalScale, 1.0);
}
float vi_aa() {
    return float(aaMode);
}
bool vi_aa_full() {
    return aaFull;
}
bool vi_resampling() {
    return resampleEnable;
}
bool vi_restore() {
    return restoreEnable;
}
bool vi_divot() {
    return divotEnable;
}
bool vi_precomputed() {
    return viPass == 2;
}
)") + glrdp_vi::filter + R"(
void main() {
    if (viPass == 1) {
        vec2 p = vec2(gl_FragCoord.x - 0.5, float(textureSize(source, 0).y) - gl_FragCoord.y - 0.5);
        vi_center = floor(p / max(internalScale, 1.0));
        outColor = vi_filter_tap(p);
        return;
    }
    if (blankOutput) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Place the active scan in the VI field rather than enlarging it to
    // the whole output. The NTSC/PAL timing origins match Angrylion's VI.
    vec2 scan = gl_FragCoord.xy * fieldSize / outputSize;
    scan.y = fieldSize.y - scan.y;
    if (fieldState.x > 0.5) {
        float row = floor(scan.y * 2.0);
        if (fieldState.z > 0.5 && mod(row, 2.0) != fieldState.y)
            discard;
        scan.y = floor(row * 0.5) + 0.5;
    }
    vec2 activeStart = vec2(hStart - timingOrigin.x, trunc((vStart - timingOrigin.y) * 0.5));
    vec2 local = scan - activeStart;
    float activeW = hEnd - hStart;
    float activeH = floor((vEnd - vStart) * 0.5);
    float leftGuard = activeStart.x < 0.0 ? 0.0 : 8.0;
    float rightGuard = activeStart.x + activeW > fieldSize.x ? 0.0 : 7.0;
    if (local.x < leftGuard || local.x >= activeW - rightGuard || local.y < 0.0 ||
        local.y >= activeH) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    // Map each scanout tap through linear RAM addressing. VI stride can
    // differ from the RDP image width (including doubled interlaced stride).
    if (internalScale == 1.0)
        local = floor(local);
    vec2 srcPos = vec2(xOffset + local.x * xScale, yOffset + local.y * yScale);

    vec2 scaledPos = (srcPos + vec2(0.5)) * max(internalScale, 1.0) - vec2(0.5);
    vec4 c = vi_resample(scaledPos);
    if (gammaDitherEnable)
        c.rgb += (vi_noise(ivec2(gl_FragCoord.xy)) - 0.5) / 255.0;
    if (gammaEnable) {
        // Piecewise approximation is closer to the VI transfer curve than a
        // plain pow(1/2.2), and is stable on GLSL 3.30 hardware.
        vec3 lo = sqrt(max(c.rgb, vec3(0.0)));
        vec3 hi = pow(max(c.rgb, vec3(0.0)), vec3(1.0 / 2.2));
        c.rgb = mix(lo, hi, step(vec3(0.25), c.rgb));
    }
    outColor = clamp(c, 0.0, 1.0);
}
)";

    const char *final_vs = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p;
    if (gl_VertexID == 0)
        p = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1)
        p = vec2(3.0, -1.0);
    else
        p = vec2(-1.0, 3.0);
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
    const char *final_fs = R"(#version 330 core
in vec2 uv;
layout(location = 0) out vec4 outColor;
uniform sampler2D postColor;
void main() {
    outColor = texture(postColor, uv);
}
)";
    // All RDP primitives use a conservative quad. Edge, texture, shade and
    // depth planes are evaluated by the same fragment ubershader.
    GLint block_size = 16384;
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &block_size);
    primitive_uniforms_.initialize(fs, unsigned(block_size));
    const auto batch_vs = primitive_uniforms_.vertex();
    const auto batch_fs = primitive_uniforms_.fragment(fs);
    generic_program_ = link(batch_vs.c_str(), batch_fs.c_str());
    initialize_samplers(generic_program_);
    if (config_.specialize_shaders) {
        specialized_key_ = glrdp_shader_config_key(config_);
        const auto specialized = glrdp_specialize_shader(fs, config_, false);
        specialized_program_ = link(batch_vs.c_str(), primitive_uniforms_.fragment(specialized).c_str());
        initialize_samplers(specialized_program_);
    }
    set_rendering_config(config_);
    present_program_ = link(pvs, pfs.c_str());
    final_present_program_ = link(final_vs, final_fs);
    glGenVertexArrays(1, &vao_);
    // Keep the integer sampler complete even for draws without RDP Z.
    // A disabled branch must not depend on an unbound normalized texture.
    glGenTextures(1, &default_depth_acceptance_texture_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, default_depth_acceptance_texture_);
    const uint8_t full_coverage = 8;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 1, 1, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                 &full_coverage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &primitive_ubo_);
    GLint ubo_alignment = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_alignment);
    const auto block_bytes = primitive_uniforms_.values.size() * sizeof(PrimitiveUniforms::Slot) * primitive_uniforms_.capacity;
    ubo_slot_bytes_ = (block_bytes + ubo_alignment - 1) / ubo_alignment * ubo_alignment;
    ubo_offset_ = 0;
    glBindBuffer(GL_UNIFORM_BUFFER, primitive_ubo_);
    glBufferData(GL_UNIFORM_BUFFER, ubo_slot_bytes_ * 64, nullptr, GL_STREAM_DRAW);
    glGenBuffers(1, &vbo_);
    glGenTextures(GLsizei(tile_texture_.size()), tile_texture_.data());
    tile_signature_.fill(~0ull);
    glGenTextures(1, &feedback_texture_);
    GLint max_texture_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    max_texture_size_ = std::max<int>(max_texture_size, 1);
    atlas_.size = std::min(2048, max_texture_size_);
    glGenTextures(1, &atlas_texture_);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, atlas_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_.size, atlas_.size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &post_fbo_);
    glGenTextures(1, &post_color_);
    glGenTextures(1, &post_depth_);
    glGenFramebuffers(1, &vi_fbo_);
    glGenTextures(1, &vi_color_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex),
                          (void *)offsetof(GLVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex),
                          (void *)offsetof(GLVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex),
                          (void *)offsetof(GLVertex, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GLVertex),
                          (void *)offsetof(GLVertex, inverse_w));
    glBindVertexArray(0);
    return true;
}
GLint RDPRenderer::uniform(GLuint program, const char *name) {
    if (program && (program == generic_program_ || program == specialized_program_)) {
        auto found = primitive_uniforms_.locations.find(name);
        if (found != primitive_uniforms_.locations.end()) return found->second;
    }
    auto &locations = uniforms_[program];
    auto it = locations.find(name);
    if (it != locations.end())
        return it->second;
    GLint location = glGetUniformLocation(program, name);
    locations.emplace(name, location);
    return location;
}
void RDPRenderer::initialize_samplers(GLuint program) {
    glUseProgram(program);
    for (unsigned level = 0; level < 8; ++level) {
        const auto name = "mip_tiles[" + std::to_string(level) + "]";
        uniform_1i(uniform(program, name.c_str()), level < 2 ? level : level + 4);
    }
    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "RDPBatch"), 0);
    uniform_1i(uniform(program, "tile_atlas"), 4);
    uniform_1i(uniform(program, "fbtex"), 2);
    uniform_1i(uniform(program, "depth_state_texture"), 3);
    uniform_1i(uniform(program, "depth_acceptance_mask"), 5);
}
void RDPRenderer::shutdown() {
    flush();
    if (primitive_ubo_ && stats_.primitives)
        std::fprintf(stderr, "GLRDP submission = primitives=%llu draws=%llu batches=%llu batched=%llu atlas_hits=%llu atlas_uploads=%llu atlas_resets=%llu standalone=%llu framebuffer_tiles=%llu framebuffer_readbacks=%llu\n",
            (unsigned long long)stats_.primitives, (unsigned long long)stats_.draws,
            (unsigned long long)stats_.batches, (unsigned long long)stats_.batched_primitives,
            (unsigned long long)stats_.atlas_hits, (unsigned long long)stats_.atlas_uploads,
            (unsigned long long)stats_.atlas_resets, (unsigned long long)stats_.standalone_uploads,
            (unsigned long long)stats_.framebuffer_tiles, (unsigned long long)stats_.framebuffer_readbacks);
    if (primitive_ubo_) glDeleteBuffers(1, &primitive_ubo_);
    if (atlas_texture_) glDeleteTextures(1, &atlas_texture_);
    primitive_ubo_ = atlas_texture_ = 0;
    atlas_.clear(); tile_regions_ = {};
    field_history_.reset();
    uniforms_.clear();
    combiner_values_.clear();
    configuration_values_.clear();
    tile_uniforms_.clear();
    tile_width_ = {};
    tile_height_ = {};
    for (auto &pixels : tile_pixels_)
        pixels.clear();
    tile_signature_.fill(~0ull);
    fb_.clear();
    if (vi_color_)
        glDeleteTextures(1, &vi_color_);
    if (vi_fbo_)
        glDeleteFramebuffers(1, &vi_fbo_);
    vi_color_ = vi_fbo_ = 0;
    vi_w_ = vi_h_ = 0;
    if (post_depth_)
        glDeleteTextures(1, &post_depth_);
    if (post_color_)
        glDeleteTextures(1, &post_color_);
    if (post_fbo_)
        glDeleteFramebuffers(1, &post_fbo_);
    if (feedback_texture_)
        glDeleteTextures(1, &feedback_texture_);
    if (default_depth_acceptance_texture_)
        glDeleteTextures(1, &default_depth_acceptance_texture_);
    default_depth_acceptance_texture_ = 0;
    glDeleteTextures(GLsizei(tile_texture_.size()), tile_texture_.data());
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);

    if (generic_program_) glDeleteProgram(generic_program_);
    if (specialized_program_) glDeleteProgram(specialized_program_);
    generic_program_ = specialized_program_ = 0;
    if (present_program_)
        glDeleteProgram(present_program_);
    if (final_present_program_)
        glDeleteProgram(final_present_program_);
    feedback_texture_ = vbo_ = vao_ = program_ = present_program_ = final_present_program_ = 0;
    post_fbo_ = post_color_ = post_depth_ = 0;
    post_w_ = post_h_ = 0;
    gpu_depth_.clear();
}
void RDPRenderer::reset() {
    flush(); atlas_.clear(); tile_regions_ = {};
    field_history_.reset();
    gpu_depth_.clear();
    fb_.clear();
    tile_signature_.fill(~0ull);
}
void RDPRenderer::set_output_size(int w, int h) {
    output_w_ = std::max(w, 1);
    output_h_ = std::max(h, 1);
}

void RDPRenderer::set_display_resolution(int width, int height) {
    if (width <= 0 || height <= 0) {
        display_w_ = 0;
        display_h_ = 0;
        return;
    }
    display_w_ = std::clamp(width, 160, 7680);
    display_h_ = std::clamp(height, 120, 4320);
}

void RDPRenderer::set_display_aspect_mode(int mode) {
    display_aspect_mode_ = std::clamp(mode, 0, 2);
}

void RDPRenderer::fit_aspect(int outer_w, int outer_h, float aspect, int &x, int &y, int &w,
                             int &h) {
    outer_w = std::max(outer_w, 1);
    outer_h = std::max(outer_h, 1);
    if (!(aspect > 0.0f))
        aspect = 4.0f / 3.0f;

    const float outer_aspect = float(outer_w) / float(outer_h);
    if (outer_aspect > aspect) {
        h = outer_h;
        w = std::max(1, int(std::lround(float(h) * aspect)));
    } else {
        w = outer_w;
        h = std::max(1, int(std::lround(float(w) / aspect)));
    }
    x = (outer_w - w) / 2;
    y = (outer_h - h) / 2;
}

void RDPRenderer::set_internal_resolution_scale(int scale) {
    flush();
    scale = std::clamp(scale, 1, 8);
    if (internal_scale_ == scale)
        return;
    internal_scale_ = scale;
    // Existing render targets were allocated for the old scale. Recreate them
    // lazily on the next draw so no stale-sized framebuffer can be reused.
    fb_.clear();
    feedback_w_ = feedback_h_ = 0;
}

bool RDPRenderer::ensure_vi_target(int w, int h) {
    if (vi_w_ == w && vi_h_ == h)
        return true;
    glBindTexture(GL_TEXTURE_2D, vi_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, vi_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vi_color_, 0);
    const GLenum buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    vi_w_ = w;
    vi_h_ = h;
    return true;
}
bool RDPRenderer::ensure_post_target(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (width > max_texture_size_ || height > max_texture_size_) {
        const double scale = std::min(double(max_texture_size_) / double(width),
                                      double(max_texture_size_) / double(height));
        width = std::max(1, int(std::floor(double(width) * scale)));
        height = std::max(1, int(std::floor(double(height) * scale)));
    }

    if (post_w_ == width && post_h_ == height && post_fbo_ && post_color_)
        return true;

    post_w_ = width;
    post_h_ = height;

    glBindTexture(GL_TEXTURE_2D, post_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, post_depth_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, post_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, post_color_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, post_depth_, 0);

    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buffer);

    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void RDPRenderer::draw_final_present() {
    glBindFramebuffer(GL_FRAMEBUFFER, output_fbo_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    // Clear the whole host backbuffer first. The fitted viewport below then
    // creates deterministic letterbox or pillarbox bars without stretching.
    glViewport(0, 0, output_w_, output_h_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float canvas_aspect = content_aspect_;
    if (display_aspect_mode_ == 1)
        canvas_aspect = 4.0f / 3.0f;
    else if (display_aspect_mode_ == 2)
        canvas_aspect = 16.0f / 9.0f;
    else if (display_w_ > 0 && display_h_ > 0)
        canvas_aspect = float(display_w_) / float(display_h_);

    int canvas_x = 0, canvas_y = 0, canvas_w = output_w_, canvas_h = output_h_;
    fit_aspect(output_w_, output_h_, canvas_aspect, canvas_x, canvas_y, canvas_w, canvas_h);

    int local_x = 0, local_y = 0, content_w = canvas_w, content_h = canvas_h;
    fit_aspect(canvas_w, canvas_h, content_aspect_, local_x, local_y, content_w, content_h);
    glViewport(canvas_x + local_x, canvas_y + local_y, content_w, content_h);

    glUseProgram(final_present_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, post_color_);
    uniform_1i(uniform(final_present_program_, "postColor"), 0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void RDPRenderer::upload_tile(const RDPState &s, std::uint8_t ti, unsigned unit) {
    auto sig = tile_sig(s, ti);
    const bool framebuffer_tile = fb_.find_containing(s.texture_image.address) != nullptr ||
        gpu_depth_.find(s.texture_image.address) != nullptr;
    if (framebuffer_tile) ++stats_.framebuffer_tiles;
    if (sig == tile_signature_[unit] && !(framebuffer_tile && tile_regions_[unit][2])) {
        return;
    }
    auto d = rdp_decode_tile(s, ti);
    if (d.rgba.empty())
        return;
    if (!framebuffer_tile && int(d.width) <= atlas_.size && int(d.height) <= atlas_.size) {
        const auto key = TextureAtlas::key(d.width, d.height, d.rgba);
        if (const auto *entry = atlas_.find(key, d.width, d.height, d.rgba)) {
            tile_regions_[unit] = entry->region;
            tile_signature_[unit] = sig;
            ++stats_.atlas_hits;
            return;
        }
        auto region = atlas_.allocate(int(d.width), int(d.height));
        if (!region[2]) {
            // Keep earlier mip levels valid for this primitive. Recycle only
            // at the next primitive boundary; this tile uses standalone storage.
            atlas_full_ = true;
        } else {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, atlas_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, region[0], region[1], region[2], region[3], GL_RGBA, GL_UNSIGNED_BYTE, d.rgba.data());
        atlas_.insert(key, region, std::move(d.rgba));
        tile_regions_[unit] = region; tile_signature_[unit] = sig; ++stats_.atlas_uploads;
        return;
        }
    }
    flush();
    tile_regions_[unit] = {};
    standalone_tiles_ = true;
    ++stats_.standalone_uploads;
    const auto &t = s.tiles[ti & 7];
    glActiveTexture(GL_TEXTURE0 + (unit < 2 ? unit : unit + 4));
    glBindTexture(GL_TEXTURE_2D, tile_texture_[unit]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const bool resized = tile_width_[unit] != int(d.width) || tile_height_[unit] != int(d.height);
    if (resized) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(d.width), GLsizei(d.height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, d.rgba.data());
    } else if (tile_pixels_[unit] != d.rgba) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(d.width), GLsizei(d.height), GL_RGBA,
                        GL_UNSIGNED_BYTE, d.rgba.data());
    }
    tile_width_[unit] = int(d.width);
    tile_height_[unit] = int(d.height);
    tile_pixels_[unit] = std::move(d.rgba);
    bool mirror = t.mirror_s || t.mirror_t;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    t.clamp_s ? GL_CLAMP_TO_EDGE : (t.mirror_s ? GL_MIRRORED_REPEAT : GL_REPEAT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    t.clamp_t ? GL_CLAMP_TO_EDGE : (t.mirror_t ? GL_MIRRORED_REPEAT : GL_REPEAT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    (void)mirror;
    tile_signature_[unit] = sig;
}
void RDPRenderer::update_feedback(GLFramebuffer &f, bool copy, bool defer_barrier) {
    current_feedback_texture_ = feedback_texture_;
    glActiveTexture(GL_TEXTURE2);
    if (glrdp_texture_barrier && f.internal_scale == 1 && copy) {
        // One quad invokes the fragment shader once per destination texel.
        // Separate native Z passes can read this colour image without writing it.
        // https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_barrier.txt
        if (!defer_barrier)
            glrdp_texture_barrier();
        current_feedback_texture_ = f.color;
        glBindTexture(GL_TEXTURE_2D, f.color);
        glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
        return;
    }
    if (feedback_w_ != f.width || feedback_h_ != f.height) {
        feedback_w_ = f.width;
        feedback_h_ = f.height;
        glBindTexture(GL_TEXTURE_2D, feedback_texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.width, f.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f.fbo);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, feedback_texture_);
    const int scale = f.internal_scale;
    const int x = bounds_x_ * scale, y = (f.native_height - bounds_y_ - bounds_h_) * scale;
    if (copy)
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, bounds_w_ * scale, bounds_h_ * scale);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f.fbo);
}

void RDPRenderer::configure_rdp_program(GLuint active_program, const GLFramebuffer &target,
                                        const RDPState &s, bool textured,
                                        bool texture_is_framebuffer, int cycle_type,
                                        float fragment_internal_scale, bool depth_state_pass,
                                        bool depth_acceptance_enable, bool raw_z_fill,
                                        std::uint32_t primitive_noise_seed,
                                        bool combined_depth_color) {
    auto loc = [&](const char *name) { return uniform(active_program, name); };

    // Probe, Z and color passes share the same stable RDP configuration.
    // Only pass selection and the primitive noise seed change on every draw.
    uniform_1i(loc("depth_state_pass"), depth_state_pass);
    uniform_1i(loc("combined_depth_color"), combined_depth_color);
    uniform_1i(loc("depth_acceptance_enable"), depth_acceptance_enable);
    uniform_1i(loc("raw_z_fill"), raw_z_fill);
    uniform_1ui(loc("noise_seed"), primitive_noise_seed);
    const bool scissor_valid = s.scissor_xl > s.scissor_xh && s.scissor_yl > s.scissor_yh;
    uniform_4i(loc("alpha_scissor_pixels"), scissor_valid ? s.scissor_xh >> 2 : 0,
                scissor_valid ? s.scissor_yh >> 2 : 0,
                scissor_valid ? int((s.scissor_xl + 3) >> 2) : target.native_width,
                scissor_valid ? int((s.scissor_yl + 3) >> 2) : target.native_height);
    const std::array<std::uint64_t, 9> configuration = {
        (std::uint64_t(target.native_width) << 32) | unsigned(target.native_height),
        unsigned(target.internal_scale),
        std::uint64_t(fragment_internal_scale),
        s.other_modes,
        unsigned(textured) | (unsigned(texture_is_framebuffer) << 1) |
            (unsigned(triangle_coverage_) << 2) | (unsigned(rdp_uses_second_texel(s)) << 3) |
            (unsigned(rdp_uses_lod_fraction(s)) << 4),
        unsigned(s.primitive_z) | (unsigned(s.primitive_dz) << 16),
        s.fill_color,
        unsigned(s.color_image.size) | (unsigned(s.color_image.format) << 8) |
            (unsigned(max_mip_level_) << 16) | (std::uint64_t(s.blend.a) << 24),
        glrdp_shader_config_key(config_)};
    if (cycle_type == 2)
        uniform_1f(loc("copy_start_x"), float(std::max(bounds_x_, int(s.scissor_xh) >> 2)));
    auto previous = configuration_values_.find(0);
    if (previous != configuration_values_.end() && previous->second == configuration) {
        set_combiner_uniforms(s, active_program);
        return;
    }
    configuration_values_[0] = configuration;
    uniform_1i(loc("coverage_mode"), config_.coverage);
    uniform_1i(loc("fast_blending"), config_.blending == GLRDP_ARITHMETIC_FAST);
    uniform_1i(loc("fast_combiner"), config_.combiner == GLRDP_ARITHMETIC_FAST);
    uniform_1i(loc("accurate_texcoords"), config_.accurate_texcoords);
    uniform_1i(loc("accurate_shading"), config_.accurate_shading);

    const int alpha_compare_mode = int(s.other_modes & 3);
    const int texture_filter_mode = config_.texture_filter ? int((s.other_modes >> 44) & 3) : 0;
    const int texture_detail_mode = int((s.other_modes >> 49) & 3);

    uniform_2f(loc("viewport"), float(target.native_width), float(target.native_height));
    uniform_1f(loc("internalScale"), std::max(fragment_internal_scale, 1.0f));
    const float lod_derivative_scale = std::max(fragment_internal_scale, 1.0f);
    uniform_1f(loc("lodDerivativeScale"), lod_derivative_scale);
    uniform_1i(loc("triangle_coverage"), triangle_coverage_);
    uniform_1i(loc("cycle_type"), cycle_type);
    uniform_1i(loc("alpha_compare_mode"), alpha_compare_mode);
    uniform_1i(loc("texture_filter_mode"), texture_filter_mode);
    uniform_1i(loc("coverage_destination"), int((s.other_modes >> 8) & 3));
    uniform_1i(loc("color_on_coverage"), (s.other_modes >> 7) & 1);
    uniform_1i(loc("texture_enable"), textured ? 1 : 0);
    uniform_1i(loc("texture1_enable"), rdp_uses_second_texel(s));
    uniform_1i(loc("lod_fraction_used"), config_.texture_lod && rdp_uses_lod_fraction(s));
    uniform_1i(loc("tex0_framebuffer"), texture_is_framebuffer ? 1 : 0);
    uniform_1i(loc("tex1_framebuffer"), texture_is_framebuffer ? 1 : 0);
    uniform_1i(loc("max_mip_level"), max_mip_level_);
    uniform_1i(loc("lod_enable"), config_.texture_lod && ((s.other_modes >> 48) & 1));
    uniform_1i(loc("texture_sharpen"), (texture_detail_mode & 1) ? 1 : 0);
    uniform_1i(loc("texture_detail"), (texture_detail_mode & 2) ? 1 : 0);
    uniform_1i(loc("framebuffer_rgba16"), s.color_image.size == RDPImageSize::Bits16 &&
                                               s.color_image.format == RDPImageFormat::RGBA);
    uniform_1i(loc("image_read_enable"), ((s.other_modes >> 6) & 1) ? 1 : 0);
    uniform_1i(loc("antialias_enable"), ((s.other_modes >> 3) & 1) ? 1 : 0);
    uniform_1i(loc("force_blend"), ((s.other_modes >> 14) & 1) ? 1 : 0);
    uniform_4i(loc("dither_modes"), config_.color_dither ? (s.other_modes >> 38) & 3 : 3,
                config_.alpha_dither ? (s.other_modes >> 36) & 3 : 3, (s.other_modes >> 38) & 3, 0);
    uniform_1i(loc("color_key_enable"), ((s.other_modes >> 40) & 1) ? 1 : 0);
    uniform_1i(loc("coverage_times_alpha"), ((s.other_modes >> 12) & 1) ? 1 : 0);
    uniform_1i(loc("alpha_coverage_select"), ((s.other_modes >> 13) & 1) ? 1 : 0);
    uniform_1i(loc("z_compare_enable"), ((s.other_modes >> 4) & 1u) ? 1 : 0);
    uniform_1i(loc("z_update_enable"), ((s.other_modes >> 5) & 1u) ? 1 : 0);
    uniform_1i(loc("primitive_z_source"), ((s.other_modes >> 2) & 1u) ? 1 : 0);
    uniform_1i(loc("z_mode"), int((s.other_modes >> 10) & 3u));
    uniform_1ui(loc("primitive_z_fixed"), GLuint(s.primitive_z));
    uniform_1ui(loc("primitive_dz_fixed"), GLuint(s.primitive_dz));
    uniform_1ui(loc("fill_color_word"), GLuint(s.fill_color));
    uniform_1i(loc("alpha_threshold_fixed"), int(s.blend.a));

    const auto rgba5551 = [](std::uint16_t value) {
        const auto e5 = [](unsigned v) { return int((v << 3) | (v >> 2)); };
        return std::array<int, 4>{e5((value >> 11) & 31), e5((value >> 6) & 31),
                                  e5((value >> 1) & 31), (value & 1) ? 255 : 0};
    };
    const auto fill_even = rgba5551(std::uint16_t(s.fill_color >> 16));
    const auto fill_odd = rgba5551(std::uint16_t(s.fill_color));
    uniform_1i(loc("fill_16bit_mode"),
                (cycle_type == 3 && s.color_image.size == RDPImageSize::Bits16) ? 1 : 0);
    uniform_1i(loc("fill_image_format"), int(s.color_image.format));
    uniform_1i(loc("fill_image_size"), int(s.color_image.size));
    uniform_4i(loc("fill_color_even"), fill_even[0], fill_even[1], fill_even[2], fill_even[3]);
    uniform_4i(loc("fill_color_odd"), fill_odd[0], fill_odd[1], fill_odd[2], fill_odd[3]);

    set_combiner_uniforms(s, active_program);
}

void RDPRenderer::primitive_geometry(const std::vector<GLVertex> &v) {
    uniform_4f(uniform(program_, "rdp_bounds"), float(bounds_x_), float(bounds_y_),
               float(bounds_x_ + bounds_w_), float(bounds_y_ + bounds_h_));
    const auto &a = v.front();
    uniform_4f(uniform(program_, "rdp_color"), a.r, a.g, a.b, a.a);
    uniform_4f(uniform(program_, "rdp_uv"), a.u * a.inverse_w, a.v * a.inverse_w, a.inverse_w, 0);
}
void RDPRenderer::submit_records(const PrimitiveUniforms::Slot *records, unsigned count, GLuint shader) {
    glUseProgram(shader);
    glBindVertexArray(vao_);
    glBindBuffer(GL_UNIFORM_BUFFER, primitive_ubo_);
    const auto stride = primitive_uniforms_.values.size() * sizeof(PrimitiveUniforms::Slot);
    // Stream into fresh, aligned ranges. Orphan only on wrap, so in-flight
    // draws never depend on bytes overwritten by a later batch.
    if (ubo_offset_ == ubo_slot_bytes_ * 64) {
        glBufferData(GL_UNIFORM_BUFFER, ubo_slot_bytes_ * 64, nullptr, GL_STREAM_DRAW);
        ubo_offset_ = 0;
    }
    glBufferSubData(GL_UNIFORM_BUFFER, ubo_offset_, stride * count, records);
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, primitive_ubo_, ubo_offset_, stride * primitive_uniforms_.capacity);
    ubo_offset_ += ubo_slot_bytes_;
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(count));
    ++stats_.draws;
}
void RDPRenderer::bind_tiles() {
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, atlas_texture_);
    if (standalone_tiles_)
        for (unsigned level = 0; level < 8; ++level) {
            glActiveTexture(GL_TEXTURE0 + (level < 2 ? level : level + 4));
            glBindTexture(GL_TEXTURE_2D, tile_texture_[level < current_tile_count_ ? level : 0]);
        }
}
void RDPRenderer::issue_geometry(const std::vector<GLVertex> &vertices, bool /*rectangle*/) {
    if (vertices.empty()) return;
    primitive_geometry(vertices);
    bind_tiles();
    submit_records(primitive_uniforms_.values.data(), 1, program_);
}
void RDPRenderer::flush() {
    if (batch_bounds_.empty()) return;
    auto &target = *batch_target_;
    PrimitiveBounds bounds = batch_bounds_.front();
    for (const auto &b : batch_bounds_) {
        const int x = std::min(bounds.x, b.x), y = std::min(bounds.y, b.y);
        bounds.w = std::max(bounds.x + bounds.w, b.x + b.w) - x;
        bounds.h = std::max(bounds.y + bounds.h, b.y + b.h) - y;
        bounds.x = x; bounds.y = y;
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER);
    glDisable(GL_MULTISAMPLE); glDisable(GL_BLEND);
    if (batch_depth_) {
        gpu_depth_.begin_pass(*batch_depth_, bounds.x, target.native_height - bounds.y - bounds.h,
                              bounds.w, bounds.h, true);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, target.color, 0);
        const GLenum buffers[] = {GL_COLOR_ATTACHMENT2, GL_NONE, GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(4, buffers);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, gpu_depth_.source_state_texture(*batch_depth_));
    } else {
        if (batch_reads_color_) {
            const int saved[] = {bounds_x_, bounds_y_, bounds_w_, bounds_h_};
            bounds_x_ = bounds.x; bounds_y_ = bounds.y; bounds_w_ = bounds.w; bounds_h_ = bounds.h;
            update_feedback(target, true);
            bounds_x_ = saved[0]; bounds_y_ = saved[1]; bounds_w_ = saved[2]; bounds_h_ = saved[3];
        }
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        const GLenum color = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &color);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    }
    glViewport(0, 0, target.width, target.height);
    // Each quad is already clipped to both its primitive bounds and scissor.
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, batch_depth_ || !batch_reads_color_ ? target.color : current_feedback_texture_);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, atlas_texture_);
    submit_records(batch_records_.data(), unsigned(batch_bounds_.size()), batch_program_);
    if (batch_depth_) gpu_depth_.finish_pass(*batch_depth_, batch_writes_z_);
    ++stats_.batches; stats_.batched_primitives += batch_bounds_.size();
    batch_bounds_.clear(); batch_records_.clear();
    batch_target_ = nullptr; batch_depth_ = nullptr;
    batch_writes_z_ = batch_reads_color_ = false;
}

bool RDPRenderer::render_gpu_depth(const std::vector<GLVertex> &vertices, bool textured,
                                   GLFramebuffer &target, const RDPState &s,
                                   bool texture_is_framebuffer, int cycle_type, bool rectangle,
                                   bool raw_z_fill, std::uint32_t primitive_noise_seed,
                                   GPUDepthBuffer::Surface &surface) {
    const bool combined =
        target.internal_scale == 1 && cycle_type < 2 && target.address != s.z_image_address;
    if (!gpu_depth_.begin_pass(surface, bounds_x_, target.native_height - bounds_y_ - bounds_h_,
                               bounds_w_, bounds_h_, combined))
        return false;
    if (combined) {
        // GL3.3 MRT writes physical colour and the ping-pong Z/DZ state in
        // one fragment invocation. All reads use the old feedback/state.
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, target.color,
                               0);
        const GLenum buffers[4] = {GL_COLOR_ATTACHMENT2, GL_NONE, GL_COLOR_ATTACHMENT0,
                                   GL_COLOR_ATTACHMENT1};
        glDrawBuffers(4, buffers);
    }
    glViewport(0, 0, target.native_width, target.native_height);
    glDisable(GL_BLEND);
    glDisable(GL_MULTISAMPLE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    primitive_scissor(target, s, 1);

    const GLuint active_program = program_;
    configure_rdp_program(active_program, target, s, textured, texture_is_framebuffer, cycle_type,
                          1.0f, true, false, raw_z_fill, primitive_noise_seed, combined);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, gpu_depth_.source_state_texture(surface));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, current_feedback_texture_);
    issue_geometry(vertices, rectangle);
    gpu_depth_.finish_pass(surface, raw_z_fill || (s.other_modes & 0x20u));
    combined_depth_color_ = combined;
    return true;
}

void RDPRenderer::set_combiner_uniforms(const RDPState &s, GLuint shader_program) {
    auto packed = [](RDPColor c) {
        return std::uint64_t(c.r) | (std::uint64_t(c.g) << 8) | (std::uint64_t(c.b) << 16) |
               (std::uint64_t(c.a) << 24);
    };
    const std::array<std::uint64_t, 9> values = {
        s.combine,
        (s.other_modes >> 16) & 0xffff,
        packed(s.primitive),
        packed(s.environment),
        packed(s.fog),
        packed(s.blend),
        std::uint64_t(s.key.center_r) | (std::uint64_t(s.key.center_g) << 8) |
            (std::uint64_t(s.key.center_b) << 16) | (std::uint64_t(s.key.scale_r) << 24) |
            (std::uint64_t(s.key.scale_g) << 32) | (std::uint64_t(s.key.scale_b) << 40),
        std::uint16_t(s.convert.k[4]) | (std::uint64_t(std::uint16_t(s.convert.k[5])) << 16),
        unsigned(s.primitive_min_lod) | (unsigned(s.primitive_lod_fraction) << 8)};
    auto previous = combiner_values_.find(0);
    if (previous != combiner_values_.end() && previous->second == values)
        return;
    combiner_values_[0] = values;
    int r0[4], a0v[4], r1[4], a1v[4];
    decode_combine(s.combine, r0, a0v, r1, a1v);
    auto loc = [&](const char *n) { return uniform(shader_program, n); };

    uniform_4i(loc("rgb0"), r0[0], r0[1], r0[2], r0[3]);
    uniform_4i(loc("alpha0"), a0v[0], a0v[1], a0v[2], a0v[3]);
    uniform_4i(loc("rgb1"), r1[0], r1[1], r1[2], r1[3]);
    uniform_4i(loc("alpha1"), a1v[0], a1v[1], a1v[2], a1v[3]);

    const unsigned bm = unsigned((s.other_modes >> 16) & 0xffff);
    const int b0[4] = {int((bm >> 14) & 3), int((bm >> 10) & 3), int((bm >> 6) & 3),
                       int((bm >> 2) & 3)};
    const int b1[4] = {int((bm >> 12) & 3), int((bm >> 8) & 3), int((bm >> 4) & 3), int(bm & 3)};
    uniform_4i(loc("blender0"), b0[0], b0[1], b0[2], b0[3]);
    uniform_4i(loc("blender1"), b1[0], b1[1], b1[2], b1[3]);

    auto color = [](RDPColor c) {
        return std::array<float, 4>{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
    };
    const auto primitive = color(s.primitive);
    const auto environment = color(s.environment);
    const auto fog = color(s.fog);
    const auto blend = color(s.blend);

    uniform_4f(loc("primitive_color"), primitive[0], primitive[1], primitive[2], primitive[3]);
    uniform_4f(loc("environment_color"), environment[0], environment[1], environment[2],
                environment[3]);
    uniform_4f(loc("fog_color"), fog[0], fog[1], fog[2], fog[3]);
    uniform_4f(loc("blend_color"), blend[0], blend[1], blend[2], blend[3]);

    uniform_4f(loc("key_center"), s.key.center_r / 255.f, s.key.center_g / 255.f,
                s.key.center_b / 255.f, 1.0f);
    uniform_4f(loc("key_scale"), s.key.scale_r / 255.f, s.key.scale_g / 255.f,
                s.key.scale_b / 255.f, 1.0f);

    // RGB-B selects signed K4; RGB-C selects signed K5. Keep these as
    // integer uniforms so the signed 9-bit conversion constants are not
    // accidentally clamped through the normalized color path.
    uniform_1i(loc("convert_k4_fixed"), int(s.convert.k[4]));
    uniform_1i(loc("convert_k5_fixed"), int(s.convert.k[5]));

    uniform_1f(loc("primitive_lod_fraction"), s.primitive_lod_fraction / 255.f);
    uniform_1i(loc("primitive_min_lod"), s.primitive_min_lod);
}
void RDPRenderer::primitive_scissor(const GLFramebuffer &target, const RDPState &state, int scale) {
    int x0 = bounds_x_, x1 = bounds_x_ + bounds_w_;
    int y0 = bounds_y_, y1 = bounds_y_ + bounds_h_;
    if (state.scissor_xl > state.scissor_xh && state.scissor_yl > state.scissor_yh) {
        x0 = std::max(x0, int(state.scissor_xh) / 4);
        x1 = std::min(x1, (int(state.scissor_xl) + 3) / 4);
        y0 = std::max(y0, int(state.scissor_yh) / 4);
        y1 = std::min(y1, (int(state.scissor_yl) + 3) / 4);
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * scale, (target.native_height - y1) * scale, std::max(0, x1 - x0) * scale,
              std::max(0, y1 - y0) * scale);
}
void RDPRenderer::draw_vertices(const std::vector<GLVertex> &v, bool textured, const RDPState &s,
                                std::uint8_t ti, bool rectangle) {
    if (v.empty())
        return;
    if (atlas_full_) {
        flush(); atlas_.clear(); tile_signature_.fill(~0ull);
        atlas_full_ = false; ++stats_.atlas_resets;
    }
    standalone_tiles_ = false;
    ++stats_.primitives;
    rdp_w_ = std::max<int>(s.color_image.width, 1);
    rdp_h_ = std::max(1, int(s.scissor_yl >> 2));
    if (rdp_h_ <= 1)
        rdp_h_ = 240;
    rdp_h_ = fb_.drawing_height(s.color_image, rdp_h_);

    if (auto *old = fb_.find(s.color_image.address);
        old && old->gpu_dirty &&
        (old->native_width != int(s.color_image.width) || old->native_height != rdp_h_))
        fb_read(old->address);
    if (batch_target_ && (batch_target_->address != s.color_image.address ||
        batch_target_->format != s.color_image.format || batch_target_->size != s.color_image.size ||
        batch_target_->native_width != rdp_w_ || batch_target_->native_height != rdp_h_ ||
        batch_target_->z_address != s.z_image_address || batch_target_->cpu_dirty ||
        batch_program_ != program_)) flush();
    const int writing_cycle = int((s.other_modes >> 52) & 3);
    const bool incoming_depth = (writing_cycle < 2 && (s.other_modes & 0x30u)) ||
        (writing_cycle == 3 && s.color_image.size == RDPImageSize::Bits16 && s.color_image.address == s.z_image_address);
    if (batch_target_ && (bool(batch_depth_) != incoming_depth || (batch_depth_ && writing_cycle >= 2))) flush();
    const bool raw16_fill = writing_cycle == 3 && s.color_image.size == RDPImageSize::Bits16 &&
                            s.color_image.address == s.z_image_address;
    const auto destination_bytes =
        (std::uint64_t(rdp_w_) * rdp_h_ * (4u << unsigned(s.color_image.size)) + 7u) / 8u;
    if (writing_cycle >= 2 && !raw16_fill && memory_ &&
        gpu_depth_.flush_range(s.color_image.address, std::uint32_t(destination_bytes), *memory_))
        fb_.depth_written_to_ram(s.color_image.address, std::uint32_t(destination_bytes));
    auto &target = fb_.bind_color_image(s.color_image, rdp_h_, s.z_image_address, internal_scale_, false);
    current_color_ = s.color_image;
    if (!target.fbo || !target.color || target.width <= 0 || target.height <= 0)
        return;

    if (target.cpu_dirty && memory_) {
        flush();
        // Merge pending GPU pixels with CPU/DMA writes before importing RAM.
        if (target.gpu_dirty && !target.raw_z)
            fb_.download_to_rdram(target, *memory_);
        fb_.upload_from_rdram(target, *memory_);
    }
    float min_x = v[0].x, max_x = v[0].x, min_y = v[0].y, max_y = v[0].y;
    for (const auto &vertex : v) {
        min_x = std::min(min_x, vertex.x);
        max_x = std::max(max_x, vertex.x);
        min_y = std::min(min_y, vertex.y);
        max_y = std::max(max_y, vertex.y);
    }
    if (s.scissor_xl > s.scissor_xh && s.scissor_yl > s.scissor_yh) {
        min_x = std::max(min_x, float(s.scissor_xh >> 2));
        min_y = std::max(min_y, float(s.scissor_yh >> 2));
        max_x = std::min(max_x, float((s.scissor_xl + 3) >> 2));
        max_y = std::min(max_y, float((s.scissor_yl + 3) >> 2));
    }
    bounds_x_ = std::clamp(int(std::floor(min_x)), 0, target.native_width);
    bounds_y_ = std::clamp(int(std::floor(min_y)), 0, target.native_height);
    bounds_w_ = std::clamp(int(std::ceil(max_x)), bounds_x_, target.native_width) - bounds_x_;
    bounds_h_ = std::clamp(int(std::ceil(max_y)), bounds_y_, target.native_height) - bounds_y_;
    if (!bounds_w_ || !bounds_h_)
        return;

    const bool texture_is_framebuffer = false;
    triangle_coverage_ = !rectangle;
    const int cycle = int((s.other_modes >> 52) & 3);
    const bool mipmapped =
        config_.texture_lod && textured && ((s.other_modes >> 48) & 1) && cycle < 2;
    const bool detail = ((s.other_modes >> 50) & 1) != 0;
    // Detail mode can consume the tile after the maximum level; level indices
    // wrap through the eight RDP descriptors, including base tiles near 7.
    const unsigned tile_count = mipmapped
                                    ? std::min(8u, unsigned(max_mip_level_) + (detail ? 2u : 1u))
                                    : (cycle == 1 ? 2u : 1u);
    current_tile_count_ = tile_count;
    const GLuint draw_program = program_;
    if (rectangle)
        uniform_4f(uniform(draw_program, "rectangle_bounds"), min_x, min_y, max_x, max_y);
    if (textured) {
        uniform_4i(uniform(draw_program, "convert_coefficients"), 2 * s.convert.k[0] + 1,
                    2 * s.convert.k[1] + 1, 2 * s.convert.k[2] + 1, 2 * s.convert.k[3] + 1);
        uniform_4i(uniform(draw_program, "texture_convert_modes"), (s.other_modes >> 43) & 1,
                    (s.other_modes >> 42) & 1, (s.other_modes >> 41) & 1,
                    ((s.other_modes >> 45) & 1) | (((s.other_modes >> 51) & 1) << 1));
    }
    if (textured)
        for (unsigned level = 0; level < 8; ++level) {
            const unsigned selected = level < tile_count ? level : 0;
            if (level < tile_count) {
                const auto &tile = s.tiles[(ti + selected) & 7];
                const std::array<float, 4> transform = {
                    texture_shift(1.0f, tile.shift_s), texture_shift(1.0f, tile.shift_t),
                    -float(tile.sl) / 4.0f, -float(tile.tl) / 4.0f};
                const bool linear = rdp_linear_copy_tile(s, ti + selected);
                const std::array<int, 4> wrap = {tile.mask_s, tile.mask_t,
                                                 int(tile.clamp_s) | (int(tile.mirror_s) << 1) |
                                                     (tile.format == RDPImageFormat::YUV
                                                          ? (((s.other_modes >> 47) & 1) ? 4 : 8)
                                                          : 0) |
                                                     (linear ? 16 : 0),
                                                 int(tile.clamp_t) | (int(tile.mirror_t) << 1) |
                                                     (linear ? int(tile.line) << 2 : 0)};
                const std::array<float, 4> clamp = {
                    float(int(tile.sh) - int(tile.sl)) / 4.0f,
                    float(int(tile.th) - int(tile.tl)) / 4.0f,
                    float(((tile.sh >> 2) - (tile.sl >> 2)) & 0x3ff),
                    float(((tile.th >> 2) - (tile.tl >> 2)) & 0x3ff)};
                auto &cached = tile_uniforms_[0][level];
                if (!cached.valid || cached.transform != transform) {
                    const auto name = "tile_transform[" + std::to_string(level) + "]";
                    uniform_4f(uniform(draw_program, name.c_str()), transform[0], transform[1],
                                transform[2], transform[3]);
                }
                if (!cached.valid || cached.wrap != wrap) {
                    const auto name = "tile_wrap[" + std::to_string(level) + "]";
                    uniform_4i(uniform(draw_program, name.c_str()), wrap[0], wrap[1], wrap[2],
                                wrap[3]);
                }
                if (!cached.valid || cached.clamp != clamp) {
                    const auto name = "tile_clamp[" + std::to_string(level) + "]";
                    uniform_4f(uniform(draw_program, name.c_str()), clamp[0], clamp[1], clamp[2], clamp[3]);
                }
                cached = {transform, wrap, clamp, true};
            }
            if (textured) {
                if (level < tile_count)
                    upload_tile(s, std::uint8_t((ti + level) & 7), level);
                const auto &region = tile_regions_[selected];
                const auto name = "tile_region[" + std::to_string(level) + "]";
                uniform_4i(uniform(draw_program, name.c_str()), region[0], region[1], region[2], region[3]);
                standalone_tiles_ |= region[2] == 0;
            }
        }

    const std::uint32_t depth_surface_id =
        s.z_image_address ? s.z_image_address : (target.address ^ 0x80000000u);
    const int cycle_type = int((s.other_modes >> 52) & 3);
    const std::uint32_t primitive_noise_seed = noise_seed_++;
    const bool fill_writes_z_image =
        cycle_type == 3 && s.color_image.size == RDPImageSize::Bits16 && s.z_image_address != 0 &&
        s.color_image.address == s.z_image_address;
    // Copy/fill and draws with Z disabled do not need integer depth passes.
    const bool needs_depth = fill_writes_z_image || ((cycle_type < 2) && (s.other_modes & 0x30u));

    // A color copy can reuse a depth allocation. Preserve the old raw clear
    // before changing ownership; subsequent texture reads must use the color
    // target and must not overwrite it with stale depth ping-pong contents.
    if (target.raw_z && !fill_writes_z_image && target.gpu_dirty) {
        flush();
        gpu_depth_.store_to_rdram(target.address, *memory_);
        // A depth readback is a renderer write, not a later CPU modification.
        // Refresh the color snapshot so it cannot suppress the new color copy.
        fb_.upload_from_rdram(target, *memory_);
    }
    if (needs_depth && s.z_image_address && !batch_depth_)
        fb_.commit_color_range(s.z_image_address, std::uint32_t(std::uint64_t(target.native_width) *
                                                                target.native_height * 2u));

    GPUDepthBuffer::Surface *depth_surface =
        needs_depth ? gpu_depth_.ensure(depth_surface_id, target.native_width, target.native_height,
                                        memory_, s.z_image_address != 0)
                    : nullptr;
    // The in-place depth barrier also makes prior colour writes visible.
    const bool combined = depth_surface && target.internal_scale == 1 && cycle_type < 2 &&
                          target.address != s.z_image_address;
    const bool can_batch = !standalone_tiles_ && !fill_writes_z_image && target.address != target.z_address &&
        (!needs_depth || (combined && glrdp_texture_barrier));
    if (can_batch) {
        const PrimitiveBounds bounds{bounds_x_, bounds_y_, bounds_w_, bounds_h_};
        if ((batch_target_ && (batch_target_ != &target || batch_depth_ != depth_surface)) ||
            batch_bounds_.size() >= primitive_uniforms_.capacity ||
            std::any_of(batch_bounds_.begin(), batch_bounds_.end(), [&](const auto &b) { return b.intersects(bounds); }))
            flush();
        configure_rdp_program(program_, target, s, textured, texture_is_framebuffer, cycle_type,
                              float(target.internal_scale), depth_surface != nullptr, false, false,
                              primitive_noise_seed, depth_surface != nullptr);
        primitive_geometry(v);
        batch_records_.insert(batch_records_.end(), primitive_uniforms_.values.begin(), primitive_uniforms_.values.end());
        batch_bounds_.push_back(bounds); batch_target_ = &target; batch_depth_ = depth_surface;
        batch_program_ = program_; batch_writes_z_ |= (s.other_modes & 0x20u) != 0;
        batch_reads_color_ |= rdp_reads_color_memory(s);
        target.raw_z = false; target.gpu_dirty = true;
        return;
    }
    flush();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER);
    update_feedback(target, rdp_reads_color_memory(s), combined);
    combined_depth_color_ = false;
    const bool gpu_depth_active =
        depth_surface &&
        render_gpu_depth(v, textured, target, s, texture_is_framebuffer, cycle_type, rectangle,
                         fill_writes_z_image, primitive_noise_seed, *depth_surface);
    current_depth_acceptance_texture_ =
        gpu_depth_active ? gpu_depth_.active_acceptance_texture(*depth_surface) : 0;

    if (!combined_depth_color_) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw_buffer);
        glViewport(0, 0, target.width, target.height);
        glDisable(GL_MULTISAMPLE);
        primitive_scissor(target, s, target.internal_scale);

        // RDP Z compare/update is performed by the preceding GLSL 3.30 integer-texture
        // pass. Fixed-function depth testing remains disabled because LEQUAL-style GL
        // depth tests cannot represent RDP DZ tolerance or Z-mode semantics.
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        const GLuint active_program = program_;
        configure_rdp_program(active_program, target, s, textured, texture_is_framebuffer,
                              cycle_type, float(target.internal_scale), false, gpu_depth_active,
                              false, primitive_noise_seed);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, current_feedback_texture_);
        if (gpu_depth_active) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, current_depth_acceptance_texture_);
        }
        issue_geometry(v, rectangle);
    }

    target.raw_z = fill_writes_z_image;
    if (target.address == target.z_address && !target.raw_z) {
        const auto bytes = std::uint64_t(target.native_width) * target.native_height *
                           (4u << unsigned(target.size)) / 8u;
        gpu_depth_.invalidate_range(target.address, std::uint32_t(bytes));
    }
    target.gpu_dirty = true;
}
void RDPRenderer::draw_triangle(const RDPTriangle &t, const RDPState &s) {
    const auto &e = t.edge;
    max_mip_level_ = e.level;
    if (e.yh >= e.yl)
        return;
    std::vector<GLVertex> v;
    v.reserve(12);
    auto span = [&](int y) {
        auto ma = edge(e.xh, e.dxhdy, y, e.yh & ~3);
        auto mi = y < e.ym ? edge(e.xm, e.dxmdy, y, e.yh & ~3) : edge(e.xl, e.dxldy, y, e.ym);
        return e.right_major ? std::pair{ma, mi} : std::pair{mi, ma};
    };
    auto make = [&](std::int64_t x, int y) {
        GLVertex vertex;
        vertex.x = f16(x);
        vertex.y = fy(y);
        return vertex;
    };
    // Bound both linear edge segments; attributes come from fragment uniforms.
    const int split = std::clamp(e.ym, e.yh, e.yl);
    const int boundaries[] = {e.yh, split, e.yl};
    for (int i = 0; i < 2; ++i) {
        const int y = boundaries[i], y1 = boundaries[i + 1];
        if (y1 <= y)
            continue;
        auto [l0, r0] = span(y);
        // Evaluate the upper minor edge at YM before switching to the lower one.
        const auto minor_end =
            i == 0 ? edge(e.xm, e.dxmdy, y1, e.yh & ~3) : edge(e.xl, e.dxldy, y1, e.ym);
        const auto major_end = edge(e.xh, e.dxhdy, y1, e.yh & ~3);
        const auto l1 = e.right_major ? major_end : minor_end;
        const auto r1 = e.right_major ? minor_end : major_end;
        if (r0 <= l0 && r1 <= l1)
            continue;
        auto a = make(l0, y), b = make(r0, y), c = make(r1, y1), d = make(l1, y1);
        v.insert(v.end(), {a, b, c, a, c, d});
    }
    const GLuint shader = program_;
    uniform_4i(uniform(shader, "edge_y"), e.yh, e.ym, e.yl, e.right_major);
    uniform_4i(uniform(shader, "edge_x"), e.xh, e.xm, e.xl, 0);
    uniform_4i(uniform(shader, "edge_slope"), e.dxhdy, e.dxmdy, e.dxldy, 0);
    uniform_2f(uniform(shader, "triangle_origin"), f16(e.xh), fy(e.yh & ~3));
    uniform_1i(uniform(shader, "triangle_shade_enable"), t.shade_enabled);
    for (unsigned i = 0; i < 4; ++i) {
        const auto name = "triangle_shade_fixed[" + std::to_string(i) + "]";
        uniform_4i(uniform(shader, name.c_str()), t.shade.value[i], t.shade.dx[i], t.shade.de[i],
                    t.shade.dy[i]);
    }
    // Fit no attribute plane from near-degenerate tessellation corners.
    // Extrapolating tiny triangles into conservative boxes magnifies vertex
    // rounding and can give the Z probe a different texture alpha.
    const bool perspective = ((s.other_modes >> 51) & 1) != 0;
    for (unsigned i = 0; i < 3; ++i) {
        const double divisor = perspective ? (i == 2 ? 65536.0 : 64.0) : (65536.0 * 32.0);
        const auto name = "triangle_texture_plane[" + std::to_string(i) + "]";
        const auto fixed = "triangle_texture_fixed[" + std::to_string(i) + "]";
        uniform_4i(uniform(shader, fixed.c_str()), t.texture.value[i], t.texture.dx[i],
                    t.texture.de[i], t.texture.dy[i]);

        if (i == 2 && !perspective)
            uniform_4f(uniform(shader, name.c_str()), 1, 0, 0, 0);
        else
            uniform_4f(uniform(shader, name.c_str()), float(t.texture.value[i] / divisor),
                        float(t.texture.dx[i] / divisor), float(t.texture.dy[i] / divisor), 0);
    }
    uniform_4i(uniform(shader, "triangle_z_fixed"), t.depth.z, t.depth.dzdx, t.depth.dzde,
                t.depth.dzdy);
    // Raw interpolated Z is Z18.13; avoid a lossy vertex-depth fit,
    // especially for nearly degenerate triangles and saturated corners.
    // Keep the Z18.13 scale binary until the final truncation. Normalizing
    // by MAX_Z then rounding changed CPU-visible near-plane depth words.
    constexpr double z_plane_scale = 8192.0;
    const auto dz_magnitude = [](std::int32_t derivative) {
        const unsigned value = (std::uint32_t(derivative) >> 16) & 0xffffu;
        return (value & 0x8000u) ? ((~value) & 0x7fffu) : value;
    };
    const unsigned dz_sum = dz_magnitude(t.depth.dzdx) + dz_magnitude(t.depth.dzdy);
    uniform_4f(uniform(shader, "triangle_z_plane"),
                t.z_enabled ? float(double(std::uint32_t(t.depth.z)) / z_plane_scale) : 0.0f,
                t.z_enabled ? float(double(t.depth.dzdx) / z_plane_scale) : 0.0f,
                t.z_enabled ? float(double(t.depth.dzdy) / z_plane_scale) : 0.0f,
                t.z_enabled ? float(dz_sum) : 0.0f);
    draw_vertices(v, t.texture_enabled, s, e.tile);
}
void RDPRenderer::draw_texture_rectangle(const RDPTextureRectangle &r, const RDPState &s) {
    max_mip_level_ = 0;
    const int cycle = int((s.other_modes >> 52) & 3);
    float x0 = float(r.xh) / 4.0f, y0 = float(r.yh) / 4.0f;
    float x1 = float(r.xl) / 4.0f, y1 = float(r.yl) / 4.0f;
    if (cycle == 2) {
        x1 = std::floor(x1) + 1.0f;
        y1 = std::floor(y1) + 1.0f;
    }
    float u0 = float(r.s) / 32.0f, v0 = float(r.t) / 32.0f;
    float ds = float(r.dsdx) / 1024.0f, dt = float(r.dtdy) / 1024.0f;
    if (cycle == 2)
        ds *= 0.25f;
    float u1 = u0 + (r.flipped ? y1 - y0 : x1 - x0) * ds;
    float v1 = v0 + (r.flipped ? x1 - x0 : y1 - y0) * dt;
    // Texture rectangles have zero shade coefficients, just like one/two-cycle
    // fill rectangles. A white placeholder changes shade-based fades/blending.
    GLVertex a{x0, y0, 0, u0, v0, 0, 0, 0, 0}, b{x1, y0, 0, u1, v0, 0, 0, 0, 0},
        c{x1, y1, 0, u1, v1, 0, 0, 0, 0}, d{x0, y1, 0, u0, v1, 0, 0, 0, 0};
    if (r.flipped) {
        b.u = u0;
        b.v = v1;
        d.u = u1;
        d.v = v0;
    }
    uniform_2f(uniform(program_, "triangle_origin"), cycle == 2 ? std::floor(x0) : x0,
                std::floor(y0));
    uniform_4f(uniform(program_, "triangle_texture_plane[0]"), u0, r.flipped ? 0 : ds,
                r.flipped ? ds : 0, 0);
    uniform_4f(uniform(program_, "triangle_texture_plane[1]"), v0, r.flipped ? dt : 0,
                r.flipped ? 0 : dt, 0);
    uniform_4f(uniform(program_, "triangle_texture_plane[2]"), 1, 0, 0, 0);
    draw_vertices({a, b, c, a, c, d}, true, s, r.tile, true);
}
void RDPRenderer::draw_fill_rectangle(const RDPFillRectangle &r, const RDPState &s) {
    max_mip_level_ = 0;
    std::uint32_t packed = s.fill_color;
    float R = float((packed >> 24) & 255) / 255.0f, G = float((packed >> 16) & 255) / 255.0f,
          B = float((packed >> 8) & 255) / 255.0f, A = float(packed & 255) / 255.0f;
    // Fill colour only supplies the raw fill cycle. One/two-cycle rectangles
    // enter the combiner with zero shade coefficients.
    if (((s.other_modes >> 52) & 3) < 2)
        R = G = B = A = 0.0f;
    float x0 = float(r.xh) / 4.0f, y0 = float(r.yh) / 4.0f;
    float x1 = float(r.xl) / 4.0f, y1 = float(r.yl) / 4.0f;
    if (((s.other_modes >> 52) & 3) >= 2) {
        x1 = std::floor(x1) + 1.0f;
        y1 = std::floor(y1) + 1.0f;
    }
    GLVertex a{x0, y0, 0, 0, 0, R, G, B, A}, b{x1, y0, 0, 0, 0, R, G, B, A},
        c0{x1, y1, 0, 0, 0, R, G, B, A}, d{x0, y1, 0, 0, 0, R, G, B, A};
    draw_vertices({a, b, c0, a, c0, d}, false, s, 0, true);
    fb_.record_fill32_hidden(r, s);
}
void RDPRenderer::present(const RDPState &state, std::uint32_t vi_status, std::uint32_t origin,
                          std::uint32_t width, std::uint32_t hstart, std::uint32_t vstart,
                          std::uint32_t xscale, std::uint32_t yscale, std::uint32_t vsync,
                          std::uint32_t current) {
    flush();
    const auto scanout = glrdp_scanout_config(config_);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DITHER);
    (void)state;

    // VI pixel type zero is blank. Clear the host backbuffer even if no RDP
    // color image has been produced for this frame.
    if (!(vi_status & 2u)) {
        field_history_.reset();
        glBindFramebuffer(GL_FRAMEBUFFER, output_fbo_);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, output_w_, output_h_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    GLFramebuffer *source =
        fb_.find_containing(origin, vi_source_byte_count(vi_status, width, vstart, yscale));
    const unsigned vi_type = vi_status & 3u;
    if (!source && memory_ && width && (vi_type == 2 || vi_type == 3)) {
        const int lines = std::max(1, int(vstart & 0x3ff) - int((vstart >> 16) & 0x3ff));
        const unsigned height = std::max(1u, unsigned(lines) * (yscale & 0xfff) / 2048u);
        const unsigned pixel_bytes = vi_type == 2 ? 2 : 4;
        if (width <= 4096 && height <= 4096 &&
            uint64_t(origin) + uint64_t(width) * height * pixel_bytes <= memory_->size()) {
            RDPImage image{RDPImageFormat::RGBA,
                           vi_type == 2 ? RDPImageSize::Bits16 : RDPImageSize::Bits32,
                           uint16_t(width), origin};
            source = &fb_.bind_color_image(image, int(height), 0, internal_scale_);
            source->cpu_dirty = true;
            source->cpu_display = true;
        }
    }
    // DMA/direct CPU stores can also overlay GPU-created images without an
    // FBWrite callback. Detect the changed RAM, merge pending GPU pixels while
    // preserving CPU stores, then import the combined image before VI reads it.
    if (source && memory_ && !source->cpu_dirty &&
        !memory_->matches_be(source->address, source->rdram_snapshot.data(),
                             source->rdram_snapshot.size()))
        source->cpu_dirty = true;
    if (source && source->cpu_dirty && memory_) {
        if (source->gpu_dirty && !source->raw_z)
            fb_.download_to_rdram(*source, *memory_);
        fb_.upload_from_rdram(*source, *memory_);
    }
    if (!source || !source->fbo || !source->color) {
        field_history_.reset();
        glBindFramebuffer(GL_FRAMEBUFFER, output_fbo_);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, output_w_, output_h_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    // The shader already places the active scan inside the complete TV
    // raster, including borders. Its active window is not the canvas aspect:
    // WDC's 640x119 field must not be letterboxed a second time here.
    content_aspect_ = 4.0f / 3.0f;
    const bool pal = (vsync & 0x3ff) > 550;
    const bool interlaced = (vi_status & 64) != 0;
    bool preserve_field = field_history_.update(vi_status, width, hstart, vstart, xscale, yscale,
                                                vsync, current, scanout.vi_bob);

    const int canvas_w = display_w_ > 0 ? display_w_ : output_w_;
    const int canvas_h = display_h_ > 0 ? display_h_ : output_h_;
    float canvas_aspect = content_aspect_;
    if (display_aspect_mode_ == 1)
        canvas_aspect = 4.0f / 3.0f;
    else if (display_aspect_mode_ == 2)
        canvas_aspect = 16.0f / 9.0f;

    int canvas_x = 0, canvas_y = 0, fitted_canvas_w = canvas_w, fitted_canvas_h = canvas_h;
    fit_aspect(canvas_w, canvas_h, canvas_aspect, canvas_x, canvas_y, fitted_canvas_w,
               fitted_canvas_h);

    int content_x = 0, content_y = 0, content_w = fitted_canvas_w, content_h = fitted_canvas_h;
    fit_aspect(fitted_canvas_w, fitted_canvas_h, content_aspect_, content_x, content_y, content_w,
               content_h);
    if (interlaced) {
        // Store whole VI scanlines independently of frontend size, so a resize
        // or a non-integer output scale cannot mix the two fields in history.
        content_w = 640;
        content_h = pal ? 576 : 480;
    }
    preserve_field &= post_w_ == content_w && post_h_ == content_h;
    if (!ensure_post_target(content_w, content_h)) {
        field_history_.reset();
        return;
    }

    // Stage 1: VI reconstruction into an aspect-correct offscreen target.
    glBindFramebuffer(GL_FRAMEBUFFER, post_fbo_);
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buffer);
    glViewport(0, 0, post_w_, post_h_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    if (interlaced && !preserve_field) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glUseProgram(present_program_);
    // Seed a newly entered mode from the first field; subsequent fields weave
    // into the persistent target. Bob explicitly refreshes both sibling rows.
    uniform_4f(uniform(present_program_, "fieldState"), interlaced, float(field_history_.lower),
                preserve_field && !scanout.vi_bob, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source->color);
    uniform_1i(uniform(present_program_, "source"), 0);
    uniform_1i(uniform(present_program_, "framebuffer_rgba32"),
                source->size == RDPImageSize::Bits32);
    uniform_1i(uniform(present_program_, "framebuffer_rgba16"),
                source->size == RDPImageSize::Bits16 && source->format == RDPImageFormat::RGBA);

    uniform_2f(uniform(present_program_, "sourceSize"), float(source->width),
                float(source->height));
    uniform_1f(uniform(present_program_, "internalScale"), float(source->internal_scale));
    uniform_2f(uniform(present_program_, "fieldSize"), 640.0f, pal ? 288.0f : 240.0f);
    uniform_2f(uniform(present_program_, "timingOrigin"), pal ? 128.0f : 108.0f,
                pal ? 44.0f : 34.0f);
    uniform_2f(uniform(present_program_, "outputSize"), float(post_w_), float(post_h_));

    const std::uint32_t byte_offset = origin >= source->address ? origin - source->address : 0u;
    std::uint32_t pixel_offset = 0;
    switch (source->size) {
    case RDPImageSize::Bits4:
        pixel_offset = byte_offset * 2u;
        break;
    case RDPImageSize::Bits8:
        pixel_offset = byte_offset;
        break;
    case RDPImageSize::Bits16:
        pixel_offset = byte_offset / 2u;
        break;
    case RDPImageSize::Bits32:
        pixel_offset = byte_offset / 4u;
        break;
    }
    const unsigned source_stride = std::max<unsigned>(width, 1u);
    uniform_2f(uniform(present_program_, "sourceLayout"), float(source_stride),
                float(pixel_offset));
    uniform_1f(uniform(present_program_, "hStart"), float((hstart >> 16) & 0x3ff));
    uniform_1f(uniform(present_program_, "hEnd"), float(hstart & 0x3ff));
    uniform_1f(uniform(present_program_, "vStart"), float((vstart >> 16) & 0x3ff));
    uniform_1f(uniform(present_program_, "vEnd"), float(vstart & 0x3ff));
    uniform_1f(uniform(present_program_, "xScale"), float(xscale & 0xfff) / 1024.0f);
    uniform_1f(uniform(present_program_, "xOffset"), float((xscale >> 16) & 0xfff) / 1024.0f);
    uniform_1f(uniform(present_program_, "yScale"), float(yscale & 0xfff) / 1024.0f);
    uniform_1f(uniform(present_program_, "yOffset"), float((yscale >> 16) & 0xfff) / 1024.0f);

    const int vi_aa_mode = int(glrdp_vi_aa_mode(scanout, vi_status));
    const bool restore = scanout.vi_restore && (vi_status & 0x10000u) != 0;
    uniform_1i(uniform(present_program_, "aaFull"), scanout.vi_aa == GLRDP_VI_AA_FULL);
    uniform_1i(uniform(present_program_, "resampleEnable"), scanout.vi_resample);
    uniform_1i(uniform(present_program_, "aaMode"), vi_aa_mode);
    uniform_1i(uniform(present_program_, "blankOutput"), vi_type == 0 ? 1 : 0);
    uniform_1i(uniform(present_program_, "divotEnable"),
                scanout.vi_divot && (vi_status & 0x10u) != 0);
    uniform_1i(uniform(present_program_, "restoreEnable"), restore);
    uniform_1i(uniform(present_program_, "gammaEnable"),
                scanout.vi_gamma && (vi_status & 0x08u) != 0);
    uniform_1i(uniform(present_program_, "gammaDitherEnable"),
                scanout.vi_gamma_dither && (vi_status & 0x04u) != 0);

    // Reconstruct once at source resolution rather than repeating six/eight
    // neighbour reads for every resampling and divot tap at display resolution.
    const bool reconstruct = (vi_aa_mode < 2 || restore);
    bool precomputed = false;
    if (reconstruct && ensure_vi_target(source->width, source->height)) {
        glBindFramebuffer(GL_FRAMEBUFFER, vi_fbo_);
        glViewport(0, 0, vi_w_, vi_h_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source->color);
        uniform_1i(uniform(present_program_, "viPass"), 1);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, post_fbo_);
        glViewport(0, 0, post_w_, post_h_);
        glBindTexture(GL_TEXTURE_2D, vi_color_);
        precomputed = true;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, post_fbo_);
        glViewport(0, 0, post_w_, post_h_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source->color);
    }
    uniform_1i(uniform(present_program_, "viPass"), precomputed ? 2 : 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Stage 2: final copy from the screen-resolution post-process target to
    // Project64's default framebuffer.
    draw_final_present();
}

void RDPRenderer::fb_read(std::uint32_t address) {
    if (batch_target_) {
        const auto color_bytes = (std::uint64_t(batch_target_->native_width) * batch_target_->native_height *
                                  (4u << unsigned(batch_target_->size)) + 7) / 8;
        const bool color = address >= batch_target_->address && std::uint64_t(address) < batch_target_->address + color_bytes;
        const bool depth = batch_depth_ && address >= batch_depth_->address &&
            std::uint64_t(address) < std::uint64_t(batch_depth_->address) + std::uint64_t(batch_depth_->width) * batch_depth_->height * 2;
        if (color || depth) { GLStateGuard guard; flush(); }
    }
    if (!memory_)
        return;
    auto *framebuffer = fb_.find_containing(address);
    const bool color_pending = framebuffer && framebuffer->gpu_dirty && !framebuffer->raw_z;
    // Most texture loads read ordinary ROM assets, not a GPU-owned image.
    // Keep those loads entirely on the metadata path; saving/restoring GL
    // state otherwise adds driver queries for every texture load.
    if (!color_pending && !gpu_depth_.has_pending_read(address))
        return;
    GLStateGuard state_guard;
    if (color_pending) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        fb_.download_to_rdram(*framebuffer, *memory_);
    }
    ++stats_.framebuffer_readbacks;
    // Commit visible compressed Z/DZ and the internal hidden-bit sidecar.
    gpu_depth_.store_to_rdram(address, *memory_);
}

void RDPRenderer::fb_write(std::uint32_t address, std::uint32_t size) {
    GLStateGuard guard;
    flush();
    size = std::max(size, 1u);
    fb_.invalidate_cpu_write(address, size);
    // Flush untouched pending depth before invalidating. Explicit store bytes
    // win even when their values equal the original RAM snapshot.
    gpu_depth_.flush_cpu_write(address, size, *memory_);
    gpu_depth_.invalidate_range(address, size);
}

void RDPRenderer::read_screen(void *destination, int *width, int *height) {
    if (width)
        *width = output_w_;
    if (height)
        *height = output_h_;
    if (!destination)
        return;

    GLStateGuard state_guard;
    flush();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, output_fbo_);
    glReadBuffer(output_fbo_ ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, output_w_, output_h_, GL_BGRA, GL_UNSIGNED_BYTE, destination);
}

void RDPRenderer::capture_bmp(const char *path) {
    if (!path)
        return;

    GLStateGuard state_guard;
    flush();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, output_fbo_);
    glReadBuffer(output_fbo_ ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    std::vector<std::uint8_t> pixels(std::size_t(output_w_) * output_h_ * 4);
    glReadPixels(0, 0, output_w_, output_h_, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());

#pragma pack(push, 1)
    struct BitmapFileHeader {
        std::uint16_t type;
        std::uint32_t size;
        std::uint16_t reserved1;
        std::uint16_t reserved2;
        std::uint32_t pixel_offset;
    };
    struct BitmapInfoHeader {
        std::uint32_t info_size;
        std::int32_t width;
        std::int32_t height;
        std::uint16_t planes;
        std::uint16_t bits;
        std::uint32_t compression;
        std::uint32_t image_size;
        std::int32_t x_pixels_per_meter;
        std::int32_t y_pixels_per_meter;
        std::uint32_t colors_used;
        std::uint32_t important_colors;
    };
#pragma pack(pop)

    static_assert(sizeof(BitmapFileHeader) == 14);
    static_assert(sizeof(BitmapInfoHeader) == 40);

    BitmapFileHeader file_header{};
    BitmapInfoHeader info_header{};
    file_header.type = 0x4d42;
    file_header.pixel_offset = sizeof(BitmapFileHeader) + sizeof(BitmapInfoHeader);
    info_header.info_size = sizeof(BitmapInfoHeader);
    info_header.width = output_w_;
    info_header.height = output_h_;
    info_header.planes = 1;
    info_header.bits = 32;
    info_header.image_size = std::uint32_t(pixels.size());
    file_header.size = file_header.pixel_offset + info_header.image_size;

    FILE *file = std::fopen(path, "wb");
    if (!file)
        return;
    std::fwrite(&file_header, 1, sizeof(file_header), file);
    std::fwrite(&info_header, 1, sizeof(info_header), file);
    std::fwrite(pixels.data(), 1, pixels.size(), file);
    std::fclose(file);
}
