#include "gles2_renderer.hpp"
#include "rdp_combiner_usage.hpp"
#include "gles2_shaders.hpp"
#include "rdp_tc_tables.hpp"
#include "gl_state_guard_es2.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
GLuint compile(GLenum type, const std::string &text) {
    GLuint s = glCreateShader(type);
    const char *source = text.c_str();
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]{};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error(log);
    }
    return s;
}
GLuint link(const std::string &header, const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, header + vs), f = 0, p = 0;
    try {
        f = compile(GL_FRAGMENT_SHADER, header + fs);
        p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        glBindAttribLocation(p, 0, "position");
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096]{};
            glGetProgramInfoLog(p, sizeof(log), nullptr, log);
            throw std::runtime_error(log);
        }
    } catch (...) {
        glDeleteShader(v);
        if (f)
            glDeleteShader(f);
        if (p)
            glDeleteProgram(p);
        throw;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}
void texture(GLuint id, int w, int h, const void *pixels = nullptr) {
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}
uint32_t decompress_z(uint16_t value) {
    unsigned e = (value >> 13) & 7;
    constexpr unsigned shift[] = {6, 5, 4, 3, 2, 1, 0, 0};
    constexpr uint32_t add[] = {0, 0x20000, 0x30000, 0x38000, 0x3c000, 0x3e000, 0x3f000, 0x3f800};
    return (((value >> 2) & 0x7ff) << shift[e]) + add[e];
}
uint16_t compress_z(uint32_t z, unsigned dz) {
    z &= 0x3ffff;
    unsigned top = z >> 11;
    uint16_t v;
    if (top <= 0x3f)
        v = (z >> 4) & 0x1ffc;
    else if (top <= 0x5f)
        v = ((z >> 3) & 0x1ffc) | 0x2000;
    else if (top <= 0x6f)
        v = ((z >> 2) & 0x1ffc) | 0x4000;
    else if (top <= 0x77)
        v = ((z >> 1) & 0x1ffc) | 0x6000;
    else if (top <= 0x7b)
        v = (z & 0x1ffc) | 0x8000;
    else if (top <= 0x7d)
        v = ((z << 1) & 0x1ffc) | 0xa000;
    else if (top == 0x7e)
        v = ((z << 2) & 0x1ffc) | 0xc000;
    else
        v = ((z << 2) & 0x1ffc) | 0xe000;
    return v | ((dz >> 2) & 3);
}
unsigned exponent(unsigned n) {
    unsigned e = 0;
    while (n >>= 1)
        ++e;
    return std::min(e, 15u);
}
unsigned normalize_dz(unsigned n) {
    n &= 65535;
    if (n & 0xc000)
        return 0x8000;
    if (n == 0)
        return 1;
    if (n == 1)
        return 3;
    return 2u << exponent(n);
}
int power_two(int n) {
    int p = 1;
    while (p < n)
        p *= 2;
    return p;
}
std::array<uint8_t, 4> color16(uint16_t p) {
    auto expand = [](unsigned c) { return uint8_t((c << 3) | (c >> 2)); };
    return {expand((p >> 11) & 31), expand((p >> 6) & 31), expand((p >> 1) & 31),
            uint8_t((p & 1) ? 255 : 0)};
}
} // namespace

GLint GLES2Renderer::uniform(GLuint p, const char *name) {
    auto &cache = uniforms_[p];
    auto found = cache.find(name);
    if (found != cache.end())
        return found->second;
    return cache.emplace(name, glGetUniformLocation(p, name)).first->second;
}
void GLES2Renderer::uniform4(const char *name, const std::array<float, 4> &value) {
    const GLint location = uniform(program_, name);
    auto &values = draw_uniforms_[program_];
    const auto found = values.find(location);
    if (found != values.end() && found->second == value)
        return;
    glUniform4f(location, value[0], value[1], value[2], value[3]);
    values[location] = value;
}
void GLES2Renderer::bytes(unsigned slot, unsigned a, unsigned b, unsigned c, unsigned d) {
    auto *out = meta_.data() + slot * 4;
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}
void GLES2Renderer::number(unsigned slot, float value) {
    static_assert(sizeof(float) == 4);
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bytes(slot, bits >> 24, bits >> 16, bits >> 8, bits);
}
bool GLES2Renderer::initialize(N64Memory *memory, bool force_mediump) {
    ES2Guard guard;
    memory_ = memory;
    fb_.set_es2(true);
    fb_.set_memory(memory);
    fb_.set_readback_callback([this](uint32_t address, uint32_t size) {
        for (auto &[base, s] : depth_)
            if (uint64_t(address) < uint64_t(base) + uint64_t(s.w) * s.h * 2 &&
                uint64_t(base) < uint64_t(address) + size)
                store_depth(base, s, address, size, true);
        invalidate_depth(address, size);
    });
    GLint range[2]{}, precision = 0;
    glrdp_precision_query(GL_FRAGMENT_SHADER, 0x8df2, range, &precision); // GL_HIGH_FLOAT
    high_precision_ = precision > 0 && !force_mediump;
    const std::string header = std::string("#version 100\n#define P ") +
                               (high_precision_ ? "highp" : "mediump") +
                               (high_precision_ ? "\n#define MIN_DZ_EXP 0.0\n"
                                                  "#define MIN_Q 0.00000001\n"
                                                  "#define Z_ROUND 0.00000762939453125\n"
                                                  "#define PRECISE_Z 1\n"
                                                : "\n#define MIN_DZ_EXP 4.0\n"
                                                  "#define MIN_Q 0.00006103515625\n"
                                                  "#define Z_ROUND 0.0\n"
                                                  "#define PRECISE_Z 0\n") +
                               "precision P float;\nprecision mediump int;\n";
    program_ = link(header, glrdp_es2::vertex, glrdp_es2::fragment);
    present_program_ = link(header, glrdp_es2::present_vertex, glrdp_es2::present_fragment.c_str());
    glGenFramebuffers(1, &vi_fbo_);
    glGenTextures(1, &vi_color_);
    glGenBuffers(1, &vbo_);
    glGenTextures(1, &atlas_);
    glGenTextures(1, &metadata_);
    glGenTextures(1, &feedback_);
    glActiveTexture(GL_TEXTURE0);
    const uint8_t white[] = {255, 255, 255, 255};
    texture(atlas_, 1, 1, white);
    atlas_w_ = atlas_h_ = 1;
    for (unsigned i = 0; i < 64; ++i)
        bytes(128 + i, rdp_tc_points[i] >> 8, rdp_tc_points[i], rdp_tc_slopes[i], 0);
    glActiveTexture(GL_TEXTURE1);
    texture(metadata_, 16, 16, meta_.data());
    glUseProgram(program_);
    glUniform1i(uniform(program_, "atlas_image"), 0);
    glUniform1i(uniform(program_, "metadata"), 1);
    glUniform1i(uniform(program_, "color_memory"), 2);
    glUniform1i(uniform(program_, "depth_memory"), 3);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_);
    return true;
}
void GLES2Renderer::shutdown() {
    if (vi_color_)
        glDeleteTextures(1, &vi_color_);
    if (vi_fbo_)
        glDeleteFramebuffers(1, &vi_fbo_);
    vi_color_ = vi_fbo_ = 0;
    vi_w_ = vi_h_ = 0;
    fb_.clear();
    for (auto &[address, s] : depth_) {
        glDeleteTextures(2, s.texture);
        glDeleteFramebuffers(2, s.fbo);
    }
    depth_.clear();
    if (program_)
        glDeleteProgram(program_);
    if (present_program_)
        glDeleteProgram(present_program_);
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    GLuint ids[] = {atlas_, metadata_, feedback_};
    glDeleteTextures(3, ids);
    program_ = present_program_ = vbo_ = atlas_ = metadata_ = feedback_ = 0;
    uniforms_.clear();
    atlas_signature_ = ~uint64_t(0);
    feedback_w_ = feedback_h_ = 0;
    atlas_pixels_.clear();
    metadata_valid_ = false;
    draw_uniforms_.clear();
}
bool GLES2Renderer::ensure_vi_target(int w, int h) {
    if (vi_w_ == w && vi_h_ == h)
        return true;
    texture(vi_color_, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, vi_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vi_color_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    vi_w_ = w;
    vi_h_ = h;
    return true;
}
void GLES2Renderer::quad(float x0, float y0, float x1, float y1) {
    const float vertices[] = {x0, y0, x1, y0, x0, y1, x1, y1};
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
void GLES2Renderer::atlas(const RDPState &s, unsigned base, unsigned level) {
    unsigned count = ((s.other_modes >> 52) & 3) == 2 ? 1u
                     : (config_.texture_lod && (s.other_modes & (uint64_t(1) << 48)))
                         ? std::min(level + 1 + unsigned((s.other_modes >> 50) & 1), 8u)
                         : (rdp_uses_second_texel(s) ? 2u : 1u);
    uint64_t signature = 1469598103934665603ull;
    auto hash = [&](const void *data, size_t count) {
        const auto *p = static_cast<const uint8_t *>(data);
        while (count--)
            signature = (signature ^ *p++) * 1099511628211ull;
    };
    hash(&s.tmem_generation, sizeof(s.tmem_generation));
    hash(&count, sizeof(count));
    for (unsigned i = 0; i < count; ++i)
        hash(&s.tiles[(base + i) & 7], sizeof(RDPTile));
    uint64_t modes =
        s.other_modes & ((uint64_t(3) << 46) | (uint64_t(7) << 48) | (uint64_t(3) << 52));
    hash(&modes, 8);
    hash(&base, sizeof(base));
    hash(&level, sizeof(level));
    if (signature == atlas_signature_) {
        std::copy(tile_meta_.begin(), tile_meta_.end(), meta_.begin() + 64);
        return;
    }
    std::array<DecodedTexture, 8> tiles;
    int width = 1;
    size_t area = 0;
    for (unsigned i = 0; i < count; ++i) {
        tiles[i] = rdp_decode_tile(s, (base + i) & 7);
        if (tiles[i].rgba.empty())
            tiles[i] = {1, 1, {255, 255, 255, 255}};
        width = std::max(width, int(tiles[i].width));
        area += size_t(tiles[i].width) * tiles[i].height;
    }
    width = std::min(max_texture_, power_two(std::max(width, int(std::sqrt(double(area))))));
    std::array<int, 8> x{}, y{};
    int end = 0;
    for (;;) {
        int px = 0, py = 0, row = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (px + int(tiles[i].width) > width) {
                px = 0;
                py += row;
                row = 0;
            }
            x[i] = px;
            y[i] = py;
            px += tiles[i].width;
            row = std::max(row, int(tiles[i].height));
        }
        end = py + row;
        if (end <= max_texture_ || width == max_texture_)
            break;
        width = std::min(width * 2, max_texture_);
    }
    int height = std::min(max_texture_, power_two(end));
    if (end > height)
        throw std::runtime_error("RDP tile atlas exceeds GLES2 texture limits");
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (unsigned i = 0; i < count; ++i)
        for (unsigned row = 0; row < tiles[i].height; ++row)
            std::memcpy(pixels.data() + ((y[i] + row) * width + x[i]) * 4,
                        tiles[i].rgba.data() + size_t(row) * tiles[i].width * 4,
                        size_t(tiles[i].width) * 4);
    glActiveTexture(GL_TEXTURE0);
    if (atlas_w_ != width || atlas_h_ != height)
        texture(atlas_, width, height, pixels.data());
    else if (pixels != atlas_pixels_) {
        glBindTexture(GL_TEXTURE_2D, atlas_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                        pixels.data());
    }
    atlas_pixels_ = std::move(pixels);
    atlas_w_ = width;
    atlas_h_ = height;
    for (unsigned relative = 0; relative < 8; ++relative) {
        unsigned i = relative < count ? relative : 0;
        const auto &tile = s.tiles[(base + i) & 7];
        unsigned slot = 16 + relative * 7;
        auto shift = [](unsigned v) {
            return v <= 10 ? 1.0f / float(1u << v) : float(1u << (16 - v));
        };
        number(slot, shift(tile.shift_s));
        number(slot + 1, shift(tile.shift_t));
        number(slot + 2, -float(tile.sl) / 4);
        number(slot + 3, -float(tile.tl) / 4);
        bytes(slot + 4, tile.mask_s, tile.mask_t,
              tile.clamp_s | unsigned(tile.mirror_s) * 2 |
                  (tile.format == RDPImageFormat::YUV ? (((s.other_modes >> 47) & 1) ? 4 : 8) : 0) |
                  (rdp_linear_copy_tile(s, base + i) ? 16 : 0),
              tile.clamp_t | unsigned(tile.mirror_t) * 2);
        bytes(slot + 5, x[i] >> 8, x[i], y[i] >> 8, y[i]);
        bytes(slot + 6, tiles[i].width >> 8, tiles[i].width, tiles[i].height >> 8, tiles[i].height);
    }
    std::copy(meta_.begin() + 64, meta_.begin() + 288, tile_meta_.begin());
    atlas_signature_ = signature;
}
GLES2Renderer::Depth &GLES2Renderer::depth(uint32_t address, int w, int h) {
    auto &s = depth_[address];
    if (s.w != w || s.h != h) {
        if (s.dirty)
            store_depth(address, s);
        glDeleteTextures(2, s.texture);
        glDeleteFramebuffers(2, s.fbo);
        s = {};
        s.w = w;
        s.h = h;
        glGenTextures(2, s.texture);
        glGenFramebuffers(2, s.fbo);
        for (unsigned i = 0; i < 2; ++i) {
            texture(s.texture[i], w, h);
            glBindFramebuffer(GL_FRAMEBUFFER, s.fbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   s.texture[i], 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("GLES2 depth framebuffer incomplete");
        }
    }
    if (s.ram_snapshot.empty() ||
        !memory_->matches_be(address, s.ram_snapshot.data(), s.ram_snapshot.size())) {
        if (s.dirty)
            store_depth(address, s);
        s.visible.resize(size_t(w) * h);
        s.imported.resize(size_t(w) * h * 4);
        s.ram_snapshot.resize(size_t(w) * h * 2);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t pixel = size_t(y) * w + x, dest = (size_t(h - 1 - y) * w + x) * 4;
                uint16_t value = memory_->read16(address + pixel * 2);
                s.visible[pixel] = value;
                uint32_t z = decompress_z(value);
                unsigned dz = ((value & 3) << 2) | memory_->read_hidden16(address + pixel * 2);
                s.imported[dest] = z >> 16;
                s.imported[dest + 1] = z >> 8;
                s.imported[dest + 2] = z;
                s.imported[dest + 3] = dz * 8 + 7;
            }
        memory_->copy_be(address, s.ram_snapshot.data(), s.ram_snapshot.size());
        for (auto id : s.texture) {
            glBindTexture(GL_TEXTURE_2D, id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                            s.imported.data());
        }
        s.dirty = false;
        s.damage_w = s.damage_h = 0;
    }
    return s;
}
void GLES2Renderer::store_depth(uint32_t address, Depth &s, uint32_t protected_address,
                                uint32_t protected_size, bool protect_hidden) {
    if (!s.dirty)
        return;
    std::vector<uint8_t> pixels(size_t(s.w) * s.h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo[s.current]);
    glReadPixels(0, 0, s.w, s.h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    bool cpu_changed = false;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            size_t index = size_t(y) * s.w + x, p = (size_t(s.h - 1 - y) * s.w + x) * 4;
            uint16_t actual = memory_->read16(address + index * 2);
            cpu_changed |= actual != s.visible[index];
            if (std::memcmp(pixels.data() + p, s.imported.data() + p, 4) == 0)
                continue;
            uint32_t z =
                (uint32_t(pixels[p] & 3) << 16) | (uint32_t(pixels[p + 1]) << 8) | pixels[p + 2];
            uint16_t value = pixels[p + 3] >= 128
                                 ? uint16_t((unsigned(pixels[p]) << 8) | pixels[p + 1])
                                 : compress_z(z, pixels[p + 3] >> 3);
            for (unsigned byte = 0; byte < 2; ++byte) {
                uint32_t byte_address = address + uint32_t(index * 2) + byte;
                unsigned shift = (1 - byte) * 8;
                bool reported =
                    protected_size && uint64_t(byte_address) >= protected_address &&
                    uint64_t(byte_address) < uint64_t(protected_address) + protected_size;
                if (reported || ((actual >> shift) & 255) != ((s.visible[index] >> shift) & 255)) {
                    value = uint16_t((value & ~(255u << shift)) | (actual & (255u << shift)));
                    cpu_changed = true;
                }
            }
            memory_->write16(address + index * 2, value);
            s.visible[index] = value;
            const auto halfword_address = uint64_t(address) + index * 2;
            if (!(protect_hidden && protected_size && halfword_address + 2 > protected_address &&
                  halfword_address < uint64_t(protected_address) + protected_size))
                memory_->write_hidden16(uint32_t(halfword_address), pixels[p + 3] >= 128
                                                                        ? ((value & 1) ? 3 : 0)
                                                                        : (pixels[p + 3] >> 3));
        }
    s.imported = std::move(pixels);
    if (cpu_changed)
        s.ram_snapshot.clear();
    else
        memory_->copy_be(address, s.ram_snapshot.data(), s.ram_snapshot.size());
    s.dirty = false;
}
void GLES2Renderer::invalidate_depth(uint32_t address, uint32_t size) {
    for (auto it = depth_.begin(); it != depth_.end();) {
        auto &s = it->second;
        if (uint64_t(address) < uint64_t(it->first) + uint64_t(s.w) * s.h * 2 &&
            uint64_t(it->first) < uint64_t(address) + size) {
            glDeleteTextures(2, s.texture);
            glDeleteFramebuffers(2, s.fbo);
            it = depth_.erase(it);
        } else
            ++it;
    }
}
void GLES2Renderer::fb_read(uint32_t address) {
    if (!memory_)
        return;
    auto *f = fb_.find_containing(address);
    const bool color_pending = f && f->gpu_dirty && !f->raw_z;
    bool depth_pending = false;
    for (const auto &[base, s] : depth_)
        if (s.dirty && uint64_t(address) >= base &&
            uint64_t(address) < uint64_t(base) + uint64_t(s.w) * s.h * 2) {
            depth_pending = true;
            break;
        }
    if (!color_pending && !depth_pending)
        return;
    ES2Guard guard;
    if (color_pending)
        fb_.download_to_rdram(*f, *memory_);
    for (auto &[base, s] : depth_)
        if (uint64_t(address) >= base &&
            uint64_t(address) < uint64_t(base) + uint64_t(s.w) * s.h * 2)
            store_depth(base, s);
}
void GLES2Renderer::fb_write(uint32_t address, uint32_t size) {
    size = std::max(size, 1u);
    fb_.invalidate_cpu_write(address, size);
    // Preserve pending GPU depth at pixels untouched by the CPU store.
    ES2Guard guard;
    for (auto &[base, s] : depth_)
        if (uint64_t(address) < uint64_t(base) + uint64_t(s.w) * s.h * 2 &&
            uint64_t(base) < uint64_t(address) + size) {
            store_depth(base, s, address, size);
            s.ram_snapshot.clear();
        }
}
void GLES2Renderer::draw(const RDPState &s, const Planes &p, float x0, float y0, float x1,
                         float y1) {
    ES2Guard guard;
    if (!memory_ || !s.color_image.width)
        return;
    int height = std::max(1, int(s.scissor_yl >> 2));
    if (height == 1)
        height = 240;
    height = fb_.drawing_height(s.color_image, height);
    if (auto *old = fb_.find(s.color_image.address);
        old && (old->native_width != s.color_image.width || old->native_height != height))
        fb_read(old->address);
    const unsigned cycle = (s.other_modes >> 52) & 3;
    const bool raw_z = cycle == 3 && s.color_image.size == RDPImageSize::Bits16 &&
                       s.color_image.address == s.z_image_address;
    const auto destination_bytes =
        (uint64_t(s.color_image.width) * height * (4u << unsigned(s.color_image.size)) + 7u) / 8u;
    bool depth_committed = false;
    if (cycle >= 2 && !raw_z)
        for (auto &[base, z] : depth_)
            if (z.dirty &&
                uint64_t(s.color_image.address) < uint64_t(base) + uint64_t(z.w) * z.h * 2 &&
                uint64_t(base) < uint64_t(s.color_image.address) + destination_bytes) {
                store_depth(base, z);
                depth_committed = true;
            }
    if (depth_committed)
        fb_.depth_written_to_ram(s.color_image.address, uint32_t(destination_bytes));
    auto &f = fb_.bind_color_image(s.color_image, height, s.z_image_address, scale_);
    if (!f.fbo)
        return;
    bool needs_depth =
        (raw_z || (cycle < 2 && (s.other_modes & 0x30))) &&
        uint64_t(s.z_image_address) + uint64_t(f.native_width) * height * 2 <= memory_->size();
    if (f.raw_z && !raw_z && f.gpu_dirty) {
        auto it = depth_.find(f.address);
        if (it != depth_.end())
            store_depth(it->first, it->second);
        fb_.upload_from_rdram(f, *memory_);
    }
    if (f.cpu_dirty) {
        if (f.gpu_dirty && !f.raw_z)
            fb_.download_to_rdram(f, *memory_);
        fb_.upload_from_rdram(f, *memory_);
    }
    if (needs_depth)
        fb_.commit_color_range(s.z_image_address, uint32_t(uint64_t(f.native_width) * height * 2));
    int left = std::max(0, int(std::floor(x0))), top = std::max(0, int(std::floor(y0)));
    int right = std::min(f.native_width, int(std::ceil(x1))),
        bottom = std::min(height, int(std::ceil(y1)));
    if (s.scissor_xl > s.scissor_xh && s.scissor_yl > s.scissor_yh) {
        left = std::max(left, int(s.scissor_xh >> 2));
        top = std::max(top, int(s.scissor_yh >> 2));
        right = std::min(right, int((s.scissor_xl + 3) >> 2));
        bottom = std::min(bottom, int((s.scissor_yl + 3) >> 2));
    }
    if (right <= left || bottom <= top)
        return;
    glActiveTexture(GL_TEXTURE2);
    if (feedback_w_ != f.width || feedback_h_ != f.height) {
        texture(feedback_, f.width, f.height);
        feedback_w_ = f.width;
        feedback_h_ = f.height;
    }
    glBindTexture(GL_TEXTURE_2D, feedback_);
    glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
    if (rdp_reads_color_memory(s))
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, left * f.internal_scale,
                            (height - bottom) * f.internal_scale, left * f.internal_scale,
                            (height - bottom) * f.internal_scale, (right - left) * f.internal_scale,
                            (bottom - top) * f.internal_scale);
    if (p.textured)
        atlas(s, p.tile, p.level);
    auto combine = s.combine;
    bytes(0, (combine >> 52) & 15, (combine >> 28) & 15, (combine >> 47) & 31, (combine >> 15) & 7);
    bytes(1, (combine >> 44) & 7, (combine >> 12) & 7, (combine >> 41) & 7, (combine >> 9) & 7);
    bytes(2, (combine >> 37) & 15, (combine >> 24) & 15, (combine >> 32) & 31, (combine >> 6) & 7);
    bytes(3, (combine >> 21) & 7, (combine >> 3) & 7, (combine >> 18) & 7, combine & 7);
    bytes(4, (s.other_modes >> 30) & 3, (s.other_modes >> 26) & 3, (s.other_modes >> 22) & 3,
          (s.other_modes >> 18) & 3);
    bytes(5, (s.other_modes >> 28) & 3, (s.other_modes >> 24) & 3, (s.other_modes >> 20) & 3,
          (s.other_modes >> 16) & 3);
    bytes(6, cycle, config_.texture_filter ? (s.other_modes >> 44) & 3 : 0, s.other_modes & 3,
          (s.other_modes >> 8) & 3);
    bytes(7,
          (config_.texture_lod ? (s.other_modes >> 48) & 1 : 0) |
              (((s.other_modes >> 50) & 1) << 1) | (((s.other_modes >> 49) & 1) << 2) |
              (((s.other_modes >> 40) & 1) << 3) | unsigned(rdp_uses_second_texel(s)) * 16 |
              unsigned(config_.texture_lod && rdp_uses_lod_fraction(s)) * 32 |
              (config_.texture_lod ? 0 : 64),
          unsigned(p.textured) | unsigned(p.triangle) * 2 | unsigned(p.shaded) * 4,
          ((s.other_modes >> 6) & 1) | (((s.other_modes >> 3) & 1) << 1) |
              (((s.other_modes >> 14) & 1) << 2) | (((s.other_modes >> 12) & 1) << 3) |
              (((s.other_modes >> 13) & 1) << 4) | (((s.other_modes >> 7) & 1) << 5),
          needs_depth ? unsigned((s.other_modes >> 4) & 1) |
                            (unsigned((s.other_modes >> 5) & 1) << 1) | unsigned(raw_z) * 4
                      : 0);
    bytes(8, (s.other_modes >> 10) & 3, unsigned(s.color_image.size), (s.other_modes >> 2) & 1,
          unsigned(s.color_image.format));
    const bool alpha_scissor = s.scissor_xl > s.scissor_xh && s.scissor_yl > s.scissor_yh;
    const unsigned axh = alpha_scissor ? s.scissor_xh : 0, ayh = alpha_scissor ? s.scissor_yh : 0;
    const unsigned axl = alpha_scissor ? s.scissor_xl : f.native_width * 4,
                   ayl = alpha_scissor ? s.scissor_yl : f.native_height * 4;
    bytes(208, axh >> 8, axh, ayh >> 8, ayh);
    bytes(209, axl >> 8, axl, ayl >> 8, ayl);
    auto even = color16(s.fill_color >> 16), odd = color16(s.fill_color);
    bytes(11, even[0], even[1], even[2], even[3]);
    bytes(12, odd[0], odd[1], odd[2], odd[3]);
    bytes(13, s.fill_color >> 24, s.fill_color >> 16, s.fill_color >> 8, s.fill_color);
    for (unsigned i = 0; i < 2; ++i) {
        uint16_t value = i ? uint16_t(s.fill_color) : uint16_t(s.fill_color >> 16);
        bytes(9 + i, value >> 8, value, 0,
              128 + ((((value & 3) << 2) | ((value & 1) ? 3 : 0)) * 8) + 7);
    }
    for (unsigned i = 0; i < 12; ++i) {
        const uint32_t v = p.texture_fixed[i];
        bytes(72 + i, v >> 24, v >> 16, v >> 8, v);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t v = p.texture_edge[i];
        bytes(84 + i, v >> 24, v >> 16, v >> 8, v);
    }
    if (p.textured && !p.triangle) {
        for (unsigned i = 0; i < 3; ++i) {
            number(72 + i * 4, p.q[i]);
            number(73 + i * 4, p.qx[i]);
            number(74 + i * 4, p.qy[i]);
        }
        number(84, cycle == 2 ? std::floor(p.ox) : p.ox);
        number(85, std::floor(p.oy));
    }
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t v = p.depth_fixed[i];
        bytes(86 + i, v >> 24, v >> 16, v >> 8, v);
    }
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t v = p.shade_fixed[i];
        bytes(192 + i, v >> 24, v >> 16, v >> 8, v);
    }
    if (!p.triangle) {
        number(86, x0);
        number(87, y0);
        number(88, x1);
        number(89, y1);
    }
    for (unsigned i = 0; i < 4; ++i)
        number(117 + i, float(2 * s.convert.k[i] + 1));
    bytes(121, (s.other_modes >> 43) & 1, (s.other_modes >> 42) & 1, (s.other_modes >> 41) & 1,
          ((s.other_modes >> 45) & 1) | (((s.other_modes >> 51) & 1) << 1));
    bytes(122, config_.color_dither ? (s.other_modes >> 38) & 3 : 3,
          config_.alpha_dither ? (s.other_modes >> 36) & 3 : 3, (s.other_modes >> 38) & 3, 0);
    bytes(123, s.tiles[p.tile & 7].line >> 8, s.tiles[p.tile & 7].line, 0, 0);
    number(112, float(left));
    number(114, float(s.convert.k[4]));
    number(115, float(f.native_width));
    number(116, float(s.convert.k[5]));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, metadata_);
    if (!metadata_valid_ || meta_ != uploaded_meta_) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, meta_.data());
        uploaded_meta_ = meta_;
        metadata_valid_ = true;
    }
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glUniform2f(uniform(program_, "atlas_size"), atlas_w_, atlas_h_);
    auto color = [&](const char *name, const RDPColor &c) {
        uniform4(name, {float(c.r), float(c.g), float(c.b), float(c.a)});
    };
    color("primitive", s.primitive);
    color("environment", s.environment);
    color("fog", s.fog);
    color("blend", s.blend);
    uniform4("key_center",
             {float(s.key.center_r), float(s.key.center_g), float(s.key.center_b), 0});
    uniform4("key_scale", {float(s.key.scale_r), float(s.key.scale_g), float(s.key.scale_b), 0});
    uniform4("accuracy_config", {float(config_.coverage), float(config_.blending),
                                 float(config_.combiner), float(config_.accurate_texcoords)});
    uniform4("settings", {float(s.primitive_min_lod), float(s.primitive_lod_fraction),
                          float(s.blend.a), float(p.level)});
    auto plane = [&](const char *name, const std::array<float, 4> &v) { uniform4(name, v); };
    // Spare texture-plane components carry scanline Z without another varying
    // vector or per-fragment metadata fetch; GLES2 still uses eight vec4s.
    glUniform4f(uniform(program_, "q0"), p.q[0], p.q[1], p.q[2],
                high_precision_ ? p.span_z[0] : 0.0f);
    glUniform4f(uniform(program_, "qdx"), p.qx[0], p.qx[1], p.qx[2],
                high_precision_ ? p.span_z[1] : 0.0f);
    glUniform4f(uniform(program_, "qdy"), p.qy[0], p.qy[1], p.qy[2],
                high_precision_ ? p.span_z[2] : 0.0f);
    plane("shade0", p.shade);
    plane("shade_dx", p.sx);
    plane("shade_dy", p.sy);
    plane("z_plane", p.z);
    plane("edge_y", p.edges[0]);
    plane("edge_x", p.edges[1]);
    plane("edge_slope", p.edges[2]);
    glUniform2f(uniform(program_, "origin"), p.ox, p.oy);
    Depth *z = nullptr;
    glActiveTexture(GL_TEXTURE3);
    if (needs_depth) {
        z = &depth(s.z_image_address, f.native_width, height);
        // Only the previous primitive's damage differs between the two copies.
        glBindFramebuffer(GL_FRAMEBUFFER, z->fbo[z->current]);
        glBindTexture(GL_TEXTURE_2D, z->texture[z->current ^ 1]);
        if (z->damage_w && z->damage_h)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, z->damage_x, z->damage_y, z->damage_x,
                                z->damage_y, z->damage_w, z->damage_h);
        glBindTexture(GL_TEXTURE_2D, z->texture[z->current]);
        glBindFramebuffer(GL_FRAMEBUFFER, z->fbo[z->current ^ 1]);
        glViewport(0, 0, f.native_width, height);
        glEnable(GL_SCISSOR_TEST);
        glScissor(left, height - bottom, right - left, bottom - top);
        glUniform4f(uniform(program_, "viewport_info"), f.native_width, height, 1, 0);
        glUniform1f(uniform(program_, "depth_pass"), 1);
        quad(left, top, right, bottom);
        z->damage_x = left;
        z->damage_y = height - bottom;
        z->damage_w = right - left;
        z->damage_h = bottom - top;
    } else
        glBindTexture(GL_TEXTURE_2D, metadata_);
    glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
    glViewport(0, 0, f.width, f.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(left * f.internal_scale, (height - bottom) * f.internal_scale,
              (right - left) * f.internal_scale, (bottom - top) * f.internal_scale);
    glUniform4f(uniform(program_, "viewport_info"), f.native_width, height, f.internal_scale, 0);
    glUniform1f(uniform(program_, "depth_pass"), 0);
    quad(left, top, right, bottom);
    if (z) {
        z->current ^= 1;
        z->dirty |= raw_z || (s.other_modes & 0x20u);
    }
    f.raw_z = raw_z;
    f.gpu_dirty = true;
    if (f.address == s.z_image_address && !raw_z)
        invalidate_depth(
            f.address, uint32_t(uint64_t(f.native_width) * height * (4u << unsigned(f.size)) / 8));
}
void GLES2Renderer::draw_triangle(const RDPTriangle &t, const RDPState &s) {
    const auto &e = t.edge;
    if (e.yh >= e.yl)
        return;
    Planes p;
    p.triangle = true;
    p.textured = t.texture_enabled;
    p.shaded = t.shade_enabled;
    p.tile = e.tile;
    p.level = e.level;
    p.ox = float(double(e.xh) / 65536);
    p.oy = float(e.yh & ~3) / 4;
    for (unsigned i = 0; i < 4; ++i) {
        p.shade_fixed[i * 4] = t.shade.value[i];
        p.shade_fixed[i * 4 + 1] = t.shade.dx[i];
        p.shade_fixed[i * 4 + 2] = t.shade.de[i];
        p.shade_fixed[i * 4 + 3] = t.shade.dy[i];
    }
    for (unsigned i = 0; i < 4; ++i)
        if (t.shade_enabled) {
            p.shade[i] = float(double(t.shade.value[i]) / 65536);
            p.sx[i] = float(double(t.shade.dx[i]) / 65536);
            p.sy[i] = float(double(t.shade.dy[i]) / 65536);
        }
    if (t.texture_enabled) {
        bool perspective = (s.other_modes >> 51) & 1;
        for (unsigned i = 0; i < 3; ++i) {
            double divisor = perspective ? (i == 2 ? 65536.0 : 64.0) : (65536.0 * 32.0);
            p.q[i] = float(double(t.texture.value[i]) / divisor);
            p.qx[i] = float(double(t.texture.dx[i]) / divisor);
            p.qy[i] = float(double(t.texture.dy[i]) / divisor);
        }
        if (!perspective) {
            p.q[2] = 1;
            p.qx[2] = p.qy[2] = 0;
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        p.texture_fixed[i * 4] = t.texture.value[i];
        p.texture_fixed[i * 4 + 1] = t.texture.dx[i];
        p.texture_fixed[i * 4 + 2] = t.texture.de[i];
        p.texture_fixed[i * 4 + 3] = t.texture.dy[i];
    }
    p.texture_edge = {e.xh & ~1, (e.dxhdy >> 2) & ~1};
    p.depth_fixed = {t.depth.z, t.depth.dzdx, t.depth.dzde, t.depth.dzdy};
    if ((s.other_modes >> 2) & 1) {
        p.z[0] = float((s.primitive_z & 0x7fff) * 8) / (high_precision_ ? 1.0f : 262143.0f);
        p.z[3] = float(exponent(s.primitive_dz));
    } else if (t.z_enabled) {
        p.span_z = {float(double(uint32_t(t.depth.z)) / 8192.0),
                    float(double(t.depth.dzde) / 8192.0),
                    float(double((t.depth.dzdx >> 8) & ~1) / 8192.0)};
        p.z[0] = float(double(uint32_t(t.depth.z)) / (high_precision_ ? 8192.0 : 2147475456.0));
        p.z[1] = float(double(t.depth.dzdx) / (high_precision_ ? 8192.0 : 2147475456.0));
        p.z[2] = float(double(t.depth.dzdy) / (high_precision_ ? 8192.0 : 2147475456.0));
        // The RDP complements a negative signed-16 derivative instead of
        // negating it. abs() crosses DZ exponent boundaries for some slopes.
        auto magnitude = [](int32_t derivative) {
            unsigned value = (uint32_t(derivative) >> 16) & 65535u;
            return value & 0x8000u ? (~value) & 0x7fffu : value;
        };
        unsigned sum = magnitude(t.depth.dzdx) + magnitude(t.depth.dzdy);
        p.z[3] = float(exponent(normalize_dz(sum)));
    }
    p.edges[0] = {float(e.yh), float(e.ym), float(e.yl), float(e.right_major)};
    p.edges[1] = {float(double(e.xh & ~1) / 65536), float(double(e.xm & ~1) / 65536),
                  float(double(e.xl & ~1) / 65536), 0};
    p.edges[2] = {float(double((e.dxhdy >> 2) & ~1) / 65536),
                  float(double((e.dxmdy >> 2) & ~1) / 65536),
                  float(double((e.dxldy >> 2) & ~1) / 65536), 0};
    double minx = 1e20, maxx = -1e20;
    auto edge = [&](int x, int slope, int y, int anchor) {
        double v = (double(x) + double((slope >> 2) & ~1) * (y - anchor)) / 65536;
        minx = std::min(minx, v);
        maxx = std::max(maxx, v);
    };
    for (int y : {e.yh, std::clamp(e.ym, e.yh, e.yl), e.yl}) {
        edge(e.xh, e.dxhdy, y, e.yh & ~3);
        if (y <= e.ym)
            edge(e.xm, e.dxmdy, y, e.yh & ~3);
        if (y >= e.ym)
            edge(e.xl, e.dxldy, y, e.ym);
    }
    draw(s, p, float(minx), float(e.yh) / 4, float(maxx), float(e.yl) / 4);
}
void GLES2Renderer::draw_texture_rectangle(const RDPTextureRectangle &r, const RDPState &s) {
    Planes p;
    p.textured = true;
    p.tile = r.tile;
    p.ox = float(r.xh) / 4;
    p.oy = float(r.yh) / 4;
    p.q = {float(r.s) / 32, float(r.t) / 32, 1, 0};
    p.shade = {0, 0, 0, 0};
    unsigned cycle = (s.other_modes >> 52) & 3;
    float ds = float(r.dsdx) / 1024 * (cycle == 2 ? 0.25f : 1), dt = float(r.dtdy) / 1024;
    if (r.flipped) {
        p.qy[0] = ds;
        p.qx[1] = dt;
    } else {
        p.qx[0] = ds;
        p.qy[1] = dt;
    }
    if ((s.other_modes >> 2) & 1) {
        p.z[0] = float((s.primitive_z & 0x7fff) * 8) / (high_precision_ ? 1.0f : 262143.0f);
        p.z[3] = float(exponent(s.primitive_dz));
    }
    float x1 = float(r.xl) / 4, y1 = float(r.yl) / 4;
    if (cycle == 2) {
        x1 = std::floor(x1) + 1;
        y1 = std::floor(y1) + 1;
    }
    draw(s, p, p.ox, p.oy, x1, y1);
}
void GLES2Renderer::draw_fill_rectangle(const RDPFillRectangle &r, const RDPState &s) {
    Planes p;
    float x1 = float(r.xl) / 4, y1 = float(r.yl) / 4;
    unsigned cycle = (s.other_modes >> 52) & 3;
    if (cycle < 2)
        p.shade = {0, 0, 0, 0};
    if (cycle >= 2) {
        x1 = std::floor(x1) + 1;
        y1 = std::floor(y1) + 1;
    }
    if ((s.other_modes >> 2) & 1) {
        p.z[0] = float((s.primitive_z & 0x7fff) * 8) / (high_precision_ ? 1.0f : 262143.0f);
        p.z[3] = float(exponent(s.primitive_dz));
    }
    draw(s, p, float(r.xh) / 4, float(r.yh) / 4, x1, y1);
    fb_.record_fill32_hidden(r, s);
}
void GLES2Renderer::present(const RDPState &, uint32_t status, uint32_t origin, uint32_t width,
                            uint32_t hstart, uint32_t vstart, uint32_t xscale, uint32_t yscale,
                            uint32_t vsync) {
    const auto scanout = glrdp_scanout_config(config_);
    ES2Guard guard;
    auto *source = fb_.find_containing(origin);
    unsigned type = status & 3;
    if (!source && width && (type == 2 || type == 3)) {
        unsigned lines = std::max(1, int(vstart & 1023) - int((vstart >> 16) & 1023)),
                 height = std::max(1u, lines * (yscale & 4095) / 2048);
        if (width <= 4096 && height <= 4096 &&
            uint64_t(origin) + uint64_t(width) * height * (type == 2 ? 2 : 4) <= memory_->size()) {
            source = &fb_.bind_color_image({RDPImageFormat::RGBA,
                                            type == 2 ? RDPImageSize::Bits16 : RDPImageSize::Bits32,
                                            uint16_t(width), origin},
                                           height, 0, scale_);
            source->cpu_display = true;
        }
    }
    if (source && !source->cpu_dirty &&
        !memory_->matches_be(source->address, source->rdram_snapshot.data(),
                             source->rdram_snapshot.size()))
        source->cpu_dirty = true;
    if (source && source->cpu_dirty) {
        if (source->gpu_dirty && !source->raw_z)
            fb_.download_to_rdram(*source, *memory_);
        fb_.upload_from_rdram(*source, *memory_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, output_fbo_);
    glViewport(0, 0, output_w_, output_h_);
    glDisable(GL_SCISSOR_TEST);
    if (!source || !source->color || !type) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    glUseProgram(present_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source->color);
    glUniform1i(uniform(present_program_, "source"), 0);
    glUniform4f(
        uniform(present_program_, "image_info"), source->width, source->height,
        source->internal_scale,
        source->size == RDPImageSize::Bits32
            ? 3
            : (source->size == RDPImageSize::Bits16 && source->format == RDPImageFormat::RGBA ? 2
                                                                                              : 0));
    float pixel_offset = float(origin >= source->address ? origin - source->address : 0) * 8 /
                         float(4u << unsigned(source->size));
    unsigned stride = std::max(width, 1u), native_width = std::max(source->native_width, 1);
    unsigned offset = unsigned(pixel_offset);
    glUniform4f(uniform(present_program_, "layout_info"), stride / native_width,
                stride % native_width, offset % native_width, offset / native_width);
    bool pal = (vsync & 1023) > 550;
    const unsigned aa_mode = glrdp_vi_aa_mode(scanout, status);
    const bool restore = scanout.vi_restore && (status & 0x10000) != 0;
    glUniform2f(uniform(present_program_, "filter_config"), scanout.vi_aa == GLRDP_VI_AA_FULL,
                scanout.vi_resample);
    glUniform4f(uniform(present_program_, "timing"), pal ? 128 : 108, pal ? 44 : 34, restore,
                pal ? 288 : 240);
    glUniform4f(uniform(present_program_, "active_scan"), (hstart >> 16) & 1023, hstart & 1023,
                (vstart >> 16) & 1023, vstart & 1023);
    glUniform4f(uniform(present_program_, "scales"), float(xscale & 4095) / 1024,
                float((xscale >> 16) & 4095) / 1024, float(yscale & 4095) / 1024,
                float((yscale >> 16) & 4095) / 1024);
    glUniform4f(uniform(present_program_, "flags"), aa_mode, scanout.vi_divot && (status & 16) != 0,
                scanout.vi_gamma && (status & 8) != 0,
                scanout.vi_gamma_dither && (status & 4) != 0);
    bool precomputed = false;
    if (aa_mode < 2 || restore) {
        if (ensure_vi_target(source->width, source->height)) {
            glBindFramebuffer(GL_FRAMEBUFFER, vi_fbo_);
            glViewport(0, 0, vi_w_, vi_h_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, source->color);
            glUniform1f(uniform(present_program_, "vi_pass"), 1);
            quad(-1, -1, 1, 1);
            precomputed = true;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, output_fbo_);
    glViewport(0, 0, output_w_, output_h_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, precomputed ? vi_color_ : source->color);
    glUniform1f(uniform(present_program_, "vi_pass"), precomputed ? 2 : 0);
    quad(-1, -1, 1, 1);
}
