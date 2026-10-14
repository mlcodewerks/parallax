#include "gpu_depth.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
constexpr std::uint32_t kZMask = 0x3ffffu;

std::uint64_t surface_bytes(const GPUDepthBuffer::Surface &surface) {
    return std::uint64_t(std::max(surface.width, 0)) * std::uint64_t(std::max(surface.height, 0)) *
           2u;
}
} // namespace

GPUDepthBuffer::~GPUDepthBuffer() {
    clear();
}

std::uint32_t GPUDepthBuffer::z_decompress_visible(std::uint16_t value) {
    const std::uint32_t packed = (value >> 2) & 0x3fffu;
    const unsigned exponent = (packed >> 11) & 7u;
    const std::uint32_t mantissa = packed & 0x7ffu;
    static constexpr std::uint8_t shifts[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    static constexpr std::uint32_t adds[8] = {
        0x00000u, 0x20000u, 0x30000u, 0x38000u, 0x3c000u, 0x3e000u, 0x3f000u, 0x3f800u,
    };
    return ((mantissa << shifts[exponent]) + adds[exponent]) & kZMask;
}

std::uint16_t GPUDepthBuffer::z_compress_visible(std::uint32_t z, std::uint8_t dz_exponent) {
    z &= kZMask;
    const unsigned top = (z >> 11) & 0x7fu;
    std::uint16_t compressed = 0;
    if (top <= 0x3f)
        compressed = std::uint16_t((z >> 4) & 0x1ffcu);
    else if (top <= 0x5f)
        compressed = std::uint16_t(((z >> 3) & 0x1ffcu) | 0x2000u);
    else if (top <= 0x6f)
        compressed = std::uint16_t(((z >> 2) & 0x1ffcu) | 0x4000u);
    else if (top <= 0x77)
        compressed = std::uint16_t(((z >> 1) & 0x1ffcu) | 0x6000u);
    else if (top <= 0x7b)
        compressed = std::uint16_t((z & 0x1ffcu) | 0x8000u);
    else if (top <= 0x7d)
        compressed = std::uint16_t(((z << 1) & 0x1ffcu) | 0xa000u);
    else if (top == 0x7e)
        compressed = std::uint16_t(((z << 2) & 0x1ffcu) | 0xc000u);
    else
        compressed = std::uint16_t(((z << 2) & 0x1ffcu) | 0xe000u);

    return std::uint16_t(compressed | ((dz_exponent >> 2) & 3u));
}

std::uint32_t GPUDepthBuffer::pack_state(std::uint32_t z, std::uint8_t dz_exponent,
                                         std::uint8_t coverage) {
    return (z & kZMask) | ((std::uint32_t(dz_exponent) & 15u) << 18) |
           ((std::uint32_t(coverage) & 7u) << 22);
}

void GPUDepthBuffer::destroy_surface(Surface &surface) {
    glDeleteFramebuffers(2, surface.fbo);
    glDeleteTextures(2, surface.state_texture);
    glDeleteTextures(2, surface.acceptance_texture);
    glDeleteTextures(2, surface.linear_depth_texture);
    surface = {};
}

void GPUDepthBuffer::clear() {
    for (auto &[_, surface] : surfaces_)
        destroy_surface(surface);
    surfaces_.clear();
}

void GPUDepthBuffer::invalidate_range(std::uint32_t address, std::uint32_t size) {
    const std::uint64_t begin = address;
    const std::uint64_t end = begin + std::max<std::uint32_t>(size, 1u);
    for (auto it = surfaces_.begin(); it != surfaces_.end();) {
        const std::uint64_t surface_begin = it->first;
        const std::uint64_t surface_end = surface_begin + surface_bytes(it->second);
        if (begin < surface_end && end > surface_begin) {
            destroy_surface(it->second);
            it = surfaces_.erase(it);
        } else {
            ++it;
        }
    }
}

bool GPUDepthBuffer::allocate_surface(Surface &surface, std::uint32_t address, int width,
                                      int height) {
    destroy_surface(surface);
    surface.address = address;
    surface.width = width;
    surface.height = height;
    surface.active = 0;

    glGenFramebuffers(2, surface.fbo);
    glGenTextures(2, surface.state_texture);
    glGenTextures(2, surface.acceptance_texture);
    glGenTextures(2, surface.linear_depth_texture);

    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, surface.state_texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, surface.acceptance_texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, surface.linear_depth_texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT,
                     GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, surface.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               surface.state_texture[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                               surface.acceptance_texture[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               surface.linear_depth_texture[i], 0);
        const GLenum draw_buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, draw_buffers);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            destroy_surface(surface);
            return false;
        }
    }
    return true;
}

void GPUDepthBuffer::initialize_default(Surface &surface) {
    const std::size_t pixels = std::size_t(surface.width) * std::size_t(surface.height);
    std::vector<std::uint32_t> states(pixels, kDefaultState);
    surface.visible_words.resize(pixels);
    std::vector<std::uint8_t> accept(pixels, 0);
    std::vector<float> linear(pixels, 1.0f);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, surface.state_texture[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height, GL_RED_INTEGER,
                        GL_UNSIGNED_INT, states.data());
        glBindTexture(GL_TEXTURE_2D, surface.acceptance_texture[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height, GL_RED_INTEGER,
                        GL_UNSIGNED_BYTE, accept.data());
        glBindTexture(GL_TEXTURE_2D, surface.linear_depth_texture[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height, GL_DEPTH_COMPONENT,
                        GL_FLOAT, linear.data());
    }
    surface.visible_state = states;
    surface.gpu_dirty = false;
    surface.initialized = true;
}

void GPUDepthBuffer::import_from_rdram(Surface &surface, const N64Memory &memory) {
    const std::size_t pixels = std::size_t(surface.width) * std::size_t(surface.height);
    std::vector<std::uint32_t> states(pixels, kDefaultState);
    surface.visible_words.resize(pixels);
    std::vector<float> linear(pixels, 1.0f);

    for (int y = 0; y < surface.height; ++y) {
        const int gl_y = surface.height - 1 - y;
        for (int x = 0; x < surface.width; ++x) {
            const std::size_t n64_pixel = std::size_t(y) * surface.width + x;
            const std::size_t gl_pixel = std::size_t(gl_y) * surface.width + x;
            const std::uint16_t packed =
                memory.read16(surface.address + std::uint32_t(n64_pixel * 2u));
            surface.visible_words[n64_pixel] = packed;
            const std::uint32_t z = z_decompress_visible(packed);
            const std::uint8_t dz_exponent =
                std::uint8_t(((packed & 3u) << 2) |
                             memory.read_hidden16(surface.address + std::uint32_t(n64_pixel * 2u)));
            states[gl_pixel] = pack_state(z, dz_exponent, 0);
            linear[gl_pixel] = float(z) / float(kMaxZ);
        }
    }

    std::vector<std::uint8_t> accept(pixels, 0);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, surface.state_texture[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height, GL_RED_INTEGER,
                        GL_UNSIGNED_INT, states.data());
        glBindTexture(GL_TEXTURE_2D, surface.acceptance_texture[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height, GL_RED_INTEGER,
                        GL_UNSIGNED_BYTE, accept.data());
        glBindTexture(GL_TEXTURE_2D, surface.linear_depth_texture[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height, GL_DEPTH_COMPONENT,
                        GL_FLOAT, linear.data());
    }
    surface.visible_state = states;
    surface.gpu_dirty = false;
    surface.initialized = true;
}

GPUDepthBuffer::Surface *GPUDepthBuffer::ensure(std::uint32_t address, int width, int height,
                                                N64Memory *memory, bool import_rdram) {
    if (!address || width <= 0 || height <= 0)
        return nullptr;
    auto &surface = surfaces_[address];
    if (!surface.fbo[0] || surface.width != width || surface.height != height) {
        if (surface.initialized && memory && import_rdram)
            store_to_rdram(address, *memory);
        if (!allocate_surface(surface, address, width, height)) {
            surfaces_.erase(address);
            return nullptr;
        }
    }
    if (!surface.initialized) {
        const std::uint64_t bytes = std::uint64_t(width) * std::uint64_t(height) * 2u;
        if (import_rdram && memory && std::uint64_t(address) + bytes <= memory->size())
            import_from_rdram(surface, *memory);
        else
            initialize_default(surface);
    }
    return &surface;
}

GPUDepthBuffer::Surface *GPUDepthBuffer::find(std::uint32_t address) {
    auto it = surfaces_.find(address);
    return it == surfaces_.end() ? nullptr : &it->second;
}

const GPUDepthBuffer::Surface *GPUDepthBuffer::find(std::uint32_t address) const {
    auto it = surfaces_.find(address);
    return it == surfaces_.end() ? nullptr : &it->second;
}

bool GPUDepthBuffer::begin_pass(Surface &surface, int x, int y, int width, int height) {
    if (!surface.initialized || !surface.fbo[0] || !surface.fbo[1])
        return false;
    const int src = surface.active;
    const int dst = src ^ 1;

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, surface.fbo[src]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, surface.fbo[dst]);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (surface.changed_w && surface.changed_h) {
        const int x0 = surface.changed_x, y0 = surface.changed_y;
        const int x1 = x0 + surface.changed_w, y1 = y0 + surface.changed_h;
        glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);
    }
    surface.changed_x = x;
    surface.changed_y = y;
    surface.changed_w = width;
    surface.changed_h = height;

    glBindFramebuffer(GL_FRAMEBUFFER, surface.fbo[dst]);
    const GLenum reset_buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, reset_buffers);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
    const GLuint zero[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 1, zero);

    const GLenum shader_buffers[4] = {GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(4, shader_buffers);
    return true;
}

void GPUDepthBuffer::finish_pass(Surface &surface, bool writes_z) {
    surface.active ^= 1;
    // Acceptance/coverage can change during a compare-only pass, but those
    // changes are not CPU-visible Z/DZ writes and need no RDRAM readback.
    surface.gpu_dirty |= writes_z;
}

GLuint GPUDepthBuffer::source_state_texture(const Surface &surface) const {
    return surface.state_texture[surface.active];
}

GLuint GPUDepthBuffer::active_state_texture(const Surface &surface) const {
    return surface.state_texture[surface.active];
}

GLuint GPUDepthBuffer::active_acceptance_texture(const Surface &surface) const {
    return surface.acceptance_texture[surface.active];
}

GLuint GPUDepthBuffer::active_linear_depth_texture(const Surface &surface) const {
    return surface.linear_depth_texture[surface.active];
}

void GPUDepthBuffer::flush_cpu_write(std::uint32_t address, std::uint32_t size, N64Memory &memory,
                                     bool protect_hidden) {
    const auto end = std::uint64_t(address) + std::max(size, 1u);
    for (auto &[base, surface] : surfaces_)
        if (std::uint64_t(address) < std::uint64_t(base) + surface_bytes(surface) && base < end)
            store_to_rdram(base, memory, address, std::max(size, 1u), protect_hidden);
}

bool GPUDepthBuffer::has_pending_read(std::uint32_t address) const {
    for (const auto &[base, surface] : surfaces_)
        if (surface.initialized && surface.gpu_dirty && std::uint64_t(address) >= base &&
            std::uint64_t(address) < std::uint64_t(base) + surface_bytes(surface))
            return true;
    return false;
}

bool GPUDepthBuffer::flush_range(std::uint32_t address, std::uint32_t size, N64Memory &memory) {
    bool pending = false;
    for (auto &[base, surface] : surfaces_)
        if (surface.initialized && surface.gpu_dirty &&
            std::uint64_t(address) < std::uint64_t(base) + surface_bytes(surface) &&
            std::uint64_t(base) < std::uint64_t(address) + size) {
            store_to_rdram(base, memory);
            pending = true;
        }
    return pending;
}

void GPUDepthBuffer::store_to_rdram(std::uint32_t address, N64Memory &memory,
                                    std::uint32_t protected_address, std::uint32_t protected_size,
                                    bool protect_hidden) {
    Surface *surface = find(address);
    std::uint32_t base_address = address;
    if (!surface) {
        for (auto &[candidate_address, candidate] : surfaces_) {
            const std::uint64_t begin = candidate_address;
            const std::uint64_t end = begin + surface_bytes(candidate);
            if (std::uint64_t(address) >= begin && std::uint64_t(address) < end) {
                base_address = candidate_address;
                surface = &candidate;
                break;
            }
        }
    }
    if (!surface || !surface->initialized || !surface->gpu_dirty)
        return;

    const std::size_t pixels = std::size_t(surface->width) * std::size_t(surface->height);
    if (std::uint64_t(base_address) + std::uint64_t(pixels) * 2u > memory.size())
        return;

    std::vector<std::uint32_t> states(pixels, kDefaultState);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, surface->fbo[surface->active]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, surface->width, surface->height, GL_RED_INTEGER, GL_UNSIGNED_INT,
                 states.data());

    for (int y = 0; y < surface->height; ++y) {
        const int gl_y = surface->height - 1 - y;
        for (int x = 0; x < surface->width; ++x) {
            const std::size_t gl_pixel = std::size_t(gl_y) * surface->width + x;
            const std::size_t n64_pixel = std::size_t(y) * surface->width + x;
            const std::uint32_t state = states[gl_pixel];
            // Re-encoding untouched arbitrary RAM as Z can corrupt texture assets
            // sharing this allocation. Write only Z/DZ that the GPU actually changed.
            if (((state ^ surface->visible_state[gl_pixel]) & 0x3fffffu) == 0)
                continue;
            surface->visible_state[gl_pixel] = state;
            const auto actual = memory.read16(base_address + std::uint32_t(n64_pixel * 2u));
            const auto prior = surface->visible_words[n64_pixel];
            const std::uint32_t z = state & kZMask;
            const std::uint8_t dz_exponent = std::uint8_t((state >> 18) & 15u);
            auto visible = z_compress_visible(z, dz_exponent);
            for (unsigned byte = 0; byte < 2; ++byte) {
                const auto byte_address = base_address + std::uint32_t(n64_pixel * 2u) + byte;
                const unsigned shift = (1 - byte) * 8;
                const bool reported =
                    protected_size && std::uint64_t(byte_address) >= protected_address &&
                    std::uint64_t(byte_address) < std::uint64_t(protected_address) + protected_size;
                if (reported || ((actual >> shift) & 255) != ((prior >> shift) & 255)) {
                    visible =
                        std::uint16_t((visible & ~(255u << shift)) | (actual & (255u << shift)));
                    surface->initialized = false;
                }
            }
            memory.write16(base_address + std::uint32_t(n64_pixel * 2u), visible);
            const auto halfword_address = std::uint64_t(base_address) + n64_pixel * 2u;
            const bool hidden_protected =
                protect_hidden && protected_size && halfword_address + 2 > protected_address &&
                halfword_address < std::uint64_t(protected_address) + protected_size;
            if (!hidden_protected)
                memory.write_hidden16(std::uint32_t(halfword_address), dz_exponent);
            surface->visible_words[n64_pixel] = visible;
        }
    }
    surface->gpu_dirty = false;
}
