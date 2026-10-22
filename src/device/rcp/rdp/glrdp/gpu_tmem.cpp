#include "gpu_tmem.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
constexpr const char *vertex = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char *load_fragment = R"(#version 330 core
uniform usampler2D previous_tmem;
uniform usampler2D byte_map;
uniform sampler2D framebuffer;
uniform ivec4 source_layout; // native width, render height, scale, image size
uniform ivec4 render_layout; // render width, base address parity
layout(location=0) out uvec4 bytes;
uint framebuffer_byte(uint offset) {
    uint size = uint(source_layout.w);
    uint pixel = offset >> (size - 1u);
    uint width = uint(source_layout.x);
    ivec2 p = ivec2(int(pixel % width), int(pixel / width));
    p = min(p * source_layout.z + source_layout.z / 2,
            ivec2(render_layout.x - 1, source_layout.y - 1));
    p.y = source_layout.y - 1 - p.y;
    uvec4 c = uvec4(floor(texelFetch(framebuffer, p, 0) * 255.0 + 0.5));
    uint packed_color;
    if (size == 2u) {
        packed_color = ((c.r >> 3u) << 11u) | ((c.g >> 3u) << 6u) |
                 ((c.b >> 3u) << 1u) | uint(c.a > 128u);
    } else if (size == 3u) {
        packed_color = (c.r << 24u) | (c.g << 16u) | (c.b << 8u) | c.a;
    } else {
        return c[(offset + uint(render_layout.y)) & 1u];
    }
    uint count = 1u << (size - 1u);
    return (packed_color >> ((count - 1u - (offset & (count - 1u))) * 8u)) & 255u;
}
void main() {
    ivec2 p = ivec2(int(gl_FragCoord.x), 0);
    uvec4 mapping = texelFetch(byte_map, p, 0);
    bytes = texelFetch(previous_tmem, p, 0);
    for (int i = 0; i < 4; ++i) {
        uint value = mapping[i];
        if (value == 0xffffffffu) continue;
        bytes[i] = (value & 0x80000000u) != 0u ? framebuffer_byte(value & 0x7fffffffu) : value;
    }
}
)";
constexpr const char *decode_fragment = R"(#version 330 core
uniform usampler2D raw_tmem;
uniform ivec4 tile; // format, size, line, TMEM word
uniform ivec4 mode; // palette bank, TLUT enable, TLUT IA, copy
uniform ivec4 storage; // width, height, flat, unused
uniform ivec4 region_origin;
layout(location=0) out vec4 color;
uint byte_at(uint a) {
    a &= 4095u;
    return texelFetch(raw_tmem, ivec2(int(a >> 2u), 0), 0)[int(a & 3u)];
}
uint rb(uint a, uint row) { return byte_at(a ^ ((row & 1u) != 0u ? 4u : 0u)); }
uint e5(uint v) { return (v << 3u) | (v >> 2u); }
uvec4 rgba16(uint v) { return uvec4(e5((v >> 11u) & 31u), e5((v >> 6u) & 31u),
                                  e5((v >> 1u) & 31u), (v & 1u) * 255u); }
uvec4 ia16(uint v) { return uvec4(v >> 8u, v >> 8u, v >> 8u, v & 255u); }
uvec4 tlut(uint index) {
    uint a = 2048u + index * 8u;
    uint v = (byte_at(a) << 8u) | byte_at(a + 1u);
    return mode.z != 0 ? ia16(v) : rgba16(v);
}
uvec4 sample_tmem(uint x, uint y) {
    uint base = uint(tile.w) * 8u + y * uint(tile.z) * 8u;
    if (mode.y != 0 && tile.x == 1) {
        uint index = rb((base + x) & 2047u, y);
        return tlut(tile.y == 0 ? (uint(mode.x) << 4u) | (index >> 4u) : index);
    }
    if (tile.y == 0) {
        uint a = base + x / 2u;
        if (mode.y != 0) a &= 2047u;
        uint p = rb(a, y);
        uint v = (x & 1u) != 0u ? p & 15u : p >> 4u;
        if (mode.y != 0) return tlut((uint(mode.x) << 4u) | v);
        if (tile.x == 0 || tile.x == 2 || tile.x == 4) return uvec4(v * 17u);
        if (tile.x == 3) {
            uint i = ((v >> 1u) * 255u + 3u) / 7u;
            return uvec4(i, i, i, (v & 1u) * 255u);
        }
    } else if (tile.y == 1) {
        uint a = base + x;
        if (mode.y != 0) a &= 2047u;
        uint v = rb(a, y);
        if (mode.y != 0) return tlut(v);
        if (tile.x == 0 || tile.x == 2 || tile.x == 4) return uvec4(v);
        if (tile.x == 3) return uvec4((v >> 4u) * 17u, (v >> 4u) * 17u,
                                     (v >> 4u) * 17u, (v & 15u) * 17u);
    } else if (tile.y == 2) {
        if (mode.y != 0) return tlut(rb((base + x * 2u) & 2047u, y));
        if (tile.x == 1) {
            uint pair = (base + (x & ~1u)) & 2047u;
            uint luma = rb(((base + x) & 2047u) | 2048u, y);
            return uvec4(rb(pair, y), rb(pair + 1u, y), luma, luma);
        }
        uint v = (rb(base + x * 2u, y) << 8u) | rb(base + x * 2u + 1u, y);
        if (tile.x == 0) return rgba16(v);
        if (tile.x == 3) return ia16(v);
    } else {
        uint lo = (base + x * 2u) & 2047u;
        if (mode.y != 0) return tlut(rb(lo, y));
        uint hi = lo | 2048u;
        return uvec4(rb(lo, y), rb(lo + 1u, y), rb(hi, y), rb(hi + 1u, y));
    }
    return uvec4(255u, 0u, 255u, 255u);
}
void main() {
    uvec2 p = uvec2(ivec2(gl_FragCoord.xy) - region_origin.xy);
    uint index = p.y * uint(storage.x) + p.x;
    uvec4 value;
    if (mode.w != 0) {
        uint a = uint(tile.w) * 8u + index * 2u;
        value = rgba16((byte_at(a) << 8u) | byte_at(a + 1u));
    } else {
        value = storage.z != 0 ? sample_tmem(index, 0u) : sample_tmem(p.x, p.y);
    }
    color = vec4(value) / 255.0;
}
)";

GLuint program(const char *fragment) {
    auto compile = [](GLenum kind, const char *source) {
        std::string es;
        if (glrdp_gles3) {
            es = source;
            es.replace(0, es.find('\n') + 1, "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp usampler2D;\n");
            source = es.c_str();
        }
        GLuint shader = glCreateShader(kind);
        glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
        GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint length = 0; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            std::string error(std::max(length, 1), '\0');
            glGetShaderInfoLog(shader, length, nullptr, error.data()); glDeleteShader(shader);
            throw std::runtime_error(error);
        }
        return shader;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = 0, result = 0;
    try { fs = compile(GL_FRAGMENT_SHADER, fragment); }
    catch (...) { glDeleteShader(vs); throw; }
    result = glCreateProgram(); glAttachShader(result, vs); glAttachShader(result, fs); glLinkProgram(result);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(result, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(result); throw std::runtime_error("GPU TMEM program link failed"); }
    return result;
}
void texture(GLuint &id, GLint internal, GLenum type, const void *pixels = nullptr) {
    glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, 1024, 1, 0, GL_RGBA_INTEGER, type, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
}

void GPUTMEM::initialize() {
    load_program_ = program(load_fragment);
    decode_program_ = program(decode_fragment);
    const char *load_names[] = {"previous_tmem", "byte_map", "framebuffer", "source_layout", "render_layout"};
    const char *decode_names[] = {"raw_tmem", "tile", "mode", "storage", "region_origin"};
    for (unsigned i = 0; i < 5; ++i) {
        load_uniforms_[i] = glGetUniformLocation(load_program_, load_names[i]);
        decode_uniforms_[i] = glGetUniformLocation(decode_program_, decode_names[i]);
    }
    glGenVertexArrays(1, &vao_); glGenFramebuffers(1, &decode_fbo_);
    glActiveTexture(GL_TEXTURE0);
    texture(map_texture_, GL_RGBA32UI, GL_UNSIGNED_INT);
    texture(cpu_texture_, GL_RGBA8UI, GL_UNSIGNED_BYTE);
    glGenTextures(1, &fallback_color_); glBindTexture(GL_TEXTURE_2D, fallback_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void GPUTMEM::release_snapshots() {
    for (auto &snapshot : snapshots_) {
        glDeleteTextures(1, &snapshot->texture); glDeleteFramebuffers(1, &snapshot->fbo);
        snapshot->texture = snapshot->fbo = 0;
    }
    snapshots_.clear();
}
void GPUTMEM::clear() {
    release_snapshots();
    if (map_texture_) glDeleteTextures(1, &map_texture_);
    if (cpu_texture_) glDeleteTextures(1, &cpu_texture_);
    if (fallback_color_) glDeleteTextures(1, &fallback_color_);
    if (decode_fbo_) glDeleteFramebuffers(1, &decode_fbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (load_program_) glDeleteProgram(load_program_);
    if (decode_program_) glDeleteProgram(decode_program_);
    map_texture_ = cpu_texture_ = fallback_color_ = decode_fbo_ = vao_ = load_program_ = decode_program_ = 0;
}
std::shared_ptr<RDPTMEMBacking> GPUTMEM::acquire(const RDPTMEMBacking *previous) {
    for (auto &snapshot : snapshots_)
        if (snapshot.use_count() == 1 && snapshot.get() != previous) {
            snapshot->serial = ++serial_; snapshot->tiles.clear();
            return snapshot;
        }
    auto snapshot = std::make_shared<RDPTMEMBacking>();
    glActiveTexture(GL_TEXTURE0);
    texture(snapshot->texture, GL_RGBA8UI, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &snapshot->fbo); glBindFramebuffer(GL_FRAMEBUFFER, snapshot->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snapshot->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("GPU TMEM framebuffer incomplete");
    snapshot->serial = ++serial_; snapshots_.push_back(snapshot);
    return snapshot;
}
void GPUTMEM::pipeline(GLuint active) {
    glUseProgram(active); glBindVertexArray(vao_);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER);
    if (!glrdp_gles3) { glDisable(GL_MULTISAMPLE); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void GPUTMEM::write(RDPState &state, const std::array<std::uint32_t, 4096> &map, const GLFramebuffer *source) {
    std::array<std::uint8_t, 4096> mask{};
    if (state.gpu_tmem) mask = state.gpu_tmem->gpu_bytes;
    for (unsigned i = 0; i < map.size(); ++i) {
        if (map[i] == UINT32_MAX) continue;
        mask[i] = (map[i] >> 31) != 0;
        state.tmem[i] = mask[i] ? 0 : std::uint8_t(map[i]);
    }
    if (std::none_of(mask.begin(), mask.end(), [](auto byte) { return byte != 0; })) {
        state.gpu_tmem.reset();
        return;
    }
    const auto previous = state.gpu_tmem;
    auto next = acquire(previous.get());
    next->gpu_bytes = mask;
    pipeline(load_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous ? previous->texture : cpu_texture_);
    if (!previous)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, state.tmem.data());
    glUniform1i(load_uniforms_[0], 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, map_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, map.data());
    glUniform1i(load_uniforms_[1], 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, source ? source->color : fallback_color_);
    glUniform1i(load_uniforms_[2], 2);
    glUniform4i(load_uniforms_[3], source ? source->native_width : 1,
                source ? source->height : 1, source ? source->internal_scale : 1,
                source ? int(source->size) : 1);
    glUniform4i(load_uniforms_[4], source ? source->width : 1, source ? (source->address & 1u) : 0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, next->fbo);
    const GLenum target = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &target);
    glViewport(0, 0, 1024, 1); glDrawArrays(GL_TRIANGLES, 0, 3);
    state.gpu_tmem = std::move(next);
}

bool GPUTMEM::materialize(RDPState &state) {
    if (!state.gpu_tmem) return false;
    if (!state.gpu_tmem->fbo) throw std::runtime_error("GPU TMEM snapshot outlived its renderer context");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, state.gpu_tmem->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (glrdp_gles3) {
        // ES guarantees unsigned-int reads for integer attachments. Byte reads
        // depend on the implementation's preferred read format/type pair.
        std::array<std::uint32_t, 4096> bytes;
        glReadPixels(0, 0, 1024, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, bytes.data());
        std::transform(bytes.begin(), bytes.end(), state.tmem.begin(), [](auto byte) { return std::uint8_t(byte); });
    } else {
        glReadPixels(0, 0, 1024, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, state.tmem.data());
    }
    state.gpu_tmem.reset();
    return true;
}

void GPUTMEM::decode(const RDPState &state, unsigned ti, GLuint destination, const std::array<int, 4> &region) {
    if (!state.gpu_tmem || !state.gpu_tmem->texture) throw std::runtime_error("Missing GPU TMEM snapshot");
    pipeline(decode_program_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, state.gpu_tmem->texture);
    glUniform1i(decode_uniforms_[0], 0);
    const auto &t = state.tiles[ti & 7];
    const auto storage = rdp_tile_storage(state, ti, true);
    glUniform4i(decode_uniforms_[1], int(t.format), int(t.size), t.line, t.tmem);
    glUniform4i(decode_uniforms_[2], t.palette, (state.other_modes >> 47) & 1,
                (state.other_modes >> 46) & 1, rdp_linear_copy_tile(state, ti));
    glUniform4i(decode_uniforms_[3], region[2], region[3], storage.texel_shift != 0, 0);
    glUniform4i(decode_uniforms_[4], region[0], region[1], 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, decode_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    const GLenum target = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &target);
    glViewport(region[0], region[1], region[2], region[3]); glDrawArrays(GL_TRIANGLES, 0, 3);
}
