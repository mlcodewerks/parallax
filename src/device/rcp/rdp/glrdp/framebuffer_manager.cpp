#include "framebuffer_manager.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace {
std::uint64_t image_key(const RDPImage &image) {
    return std::uint64_t(image.address) | (std::uint64_t(image.width) << 32) |
           (std::uint64_t(std::uint8_t(image.size)) << 48) |
           (std::uint64_t(std::uint8_t(image.format)) << 52);
}

std::uint64_t framebuffer_byte_count(int width, int height, RDPImageSize size) {
    const std::uint64_t pixels =
        std::uint64_t(std::max(width, 0)) * std::uint64_t(std::max(height, 0));
    switch (size) {
    case RDPImageSize::Bits4:
        return (pixels + 1u) / 2u;
    case RDPImageSize::Bits8:
        return pixels;
    case RDPImageSize::Bits16:
        return pixels * 2u;
    case RDPImageSize::Bits32:
        return pixels * 4u;
    }
    return pixels;
}

void destroy_color_target(GLFramebuffer &framebuffer) {
    if (framebuffer.fbo)
        glDeleteFramebuffers(1, &framebuffer.fbo);
    if (framebuffer.color)
        glDeleteTextures(1, &framebuffer.color);
    framebuffer.fbo = 0;
    framebuffer.color = 0;
    framebuffer.width = 0;
    framebuffer.height = 0;
}
} // namespace

FramebufferManager::~FramebufferManager() {
    clear();
}

void FramebufferManager::clear() {
    for (auto &[_, framebuffer] : buffers_)
        destroy_color_target(framebuffer);

    buffers_.clear();
    last_bound_ = nullptr;
    max_texture_size_ = 0;
}

bool FramebufferManager::cpu_eligible(const GLFramebuffer &f) const {
    return memory_ && f.fbo && f.address && f.size != RDPImageSize::Bits4 &&
           uint64_t(f.address) + framebuffer_byte_count(f.native_width, f.native_height, f.size) <=
               memory_->size();
}

void FramebufferManager::synchronize_cpu_tracking(GLFramebuffer &active) {
    if (!memory_)
        return;
    if (active.cpu_untracked) {
        if (!active.rdram_snapshot.empty() && cpu_eligible(active) &&
            (active.rdram_snapshot.size() > memory_->size() - active.address ||
             !memory_->matches_be(active.address, active.rdram_snapshot.data(),
                                  active.rdram_snapshot.size())))
            active.cpu_dirty = true;
        active.cpu_untracked = false;
    }
    const bool eligible = cpu_eligible(active);
    // Repeated primitives in one target do not change the FBInfo selection.
    if (last_bound_ == &active && last_bound_eligible_ == eligible)
        return;
    last_bound_ = &active;
    last_bound_eligible_ = eligible;
    if (buffers_.size() <= cpu_color_slots)
        return;
    std::vector<GLFramebuffer *> candidates;
    for (auto &[_, f] : buffers_)
        if (cpu_eligible(f))
            candidates.push_back(&f);
    std::sort(candidates.begin(), candidates.end(),
              [](auto a, auto b) { return a->serial > b->serial; });
    GLint binding = 0, pack = 0;
    bool saved = false;
    for (size_t i = cpu_color_slots; i < candidates.size(); ++i) {
        auto &f = *candidates[i];
        f.cpu_untracked = true;
        // Raw-Z color targets are mirrors; the depth owner commits them when
        // SetZImage changes. Writing their stale mirror would overwrite Z.
        if (!f.gpu_dirty || f.raw_z)
            continue;
        if (!saved) {
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
            glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            saved = true;
        }
        download_to_rdram(f, *memory_);
    }
    if (saved) {
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(binding));
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
    }
}

void FramebufferManager::allocate_color(GLFramebuffer &framebuffer, int width, int height) {
    destroy_color_target(framebuffer);

    framebuffer.width = width;
    framebuffer.height = height;

    glGenTextures(1, &framebuffer.color);
    glBindTexture(GL_TEXTURE_2D, framebuffer.color);
    glTexImage2D(GL_TEXTURE_2D, 0, es2_ ? GL_RGBA : GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &framebuffer.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebuffer.color,
                           0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        destroy_color_target(framebuffer);
        return;
    }
    // A newly allocated/reallocated GL target has no valid copy of RDRAM.
    // Force an upload before the next primitive so framebuffer effects and
    // partial draws start from the emulated memory contents rather than
    // undefined texture storage.
    framebuffer.cpu_dirty = true;
    framebuffer.gpu_dirty = false;
}

int FramebufferManager::drawing_height(const RDPImage &image, int scissor_height) const {
    // SetColorImage has no height. Scissor gives a minimum drawing extent,
    // rather than permission to discard rows outside a later smaller clip.
    // Match the complete image layout so unrelated aliases do not set its size.
    auto existing = buffers_.find(image_key(image));
    return existing == buffers_.end() ? scissor_height
                                      : std::max(scissor_height, existing->second.native_height);
}

GLFramebuffer &FramebufferManager::bind_color_image(const RDPImage &image, int requested_height,
                                                    std::uint32_t z_address, int requested_scale) {
    const std::uint64_t key = image_key(image);

    auto &framebuffer = buffers_[key];
    framebuffer.address = image.address;
    framebuffer.format = image.format;
    framebuffer.size = image.size;
    framebuffer.serial = ++serial_counter_;
    framebuffer.cpu_display = false;

    const int native_width = std::clamp<int>(image.width, 1, 4096);
    const int native_height = std::clamp(requested_height, 1, 4096);
    requested_scale = std::clamp(requested_scale, 1, 8);

    if (!max_texture_size_) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);
        max_texture_size_ = std::max(max_texture_size_, 1);
    }
    const int max_texture_size = max_texture_size_;

    // Preserve the framebuffer aspect and integer scaling. If the requested
    // scale exceeds a driver's texture limit, lower the scale rather than
    // independently clipping width or height.
    const int max_width_scale = std::max(1, max_texture_size / native_width);
    const int max_height_scale = std::max(1, max_texture_size / native_height);
    const int effective_scale =
        std::max(1, std::min({requested_scale, max_width_scale, max_height_scale}));

    const int render_width = std::min(native_width * effective_scale, max_texture_size);
    const int render_height = std::min(native_height * effective_scale, max_texture_size);

    if (!framebuffer.fbo || framebuffer.width != render_width ||
        framebuffer.height != render_height || framebuffer.native_width != native_width ||
        framebuffer.native_height != native_height ||
        framebuffer.internal_scale != effective_scale) {
        allocate_color(framebuffer, render_width, render_height);
    }

    framebuffer.native_width = native_width;
    framebuffer.native_height = native_height;
    framebuffer.internal_scale = effective_scale;
    framebuffer.z_address = z_address;

    if (!framebuffer.fbo || !framebuffer.color)
        return framebuffer;

    if (memory_ && last_bound_ != &framebuffer) {
        // Distinct CI origins/layouts can address the same physical pixels
        // (including interlaced images shifted by half a row). Commit the
        // previous view before importing or drawing through the new view.
        const std::uint64_t begin = framebuffer.address;
        const std::uint64_t end =
            begin + framebuffer_byte_count(native_width, native_height, framebuffer.size);
        for (auto &[_, other] : buffers_) {
            if (&other == &framebuffer || !other.gpu_dirty || other.raw_z || !cpu_eligible(other))
                continue;
            const std::uint64_t other_end =
                std::uint64_t(other.address) +
                framebuffer_byte_count(other.native_width, other.native_height, other.size);
            if (begin < other_end && std::uint64_t(other.address) < end)
                download_to_rdram(other, *memory_);
        }
        if (!framebuffer.rdram_snapshot.empty() && cpu_eligible(framebuffer) &&
            !memory_->matches_be(framebuffer.address, framebuffer.rdram_snapshot.data(),
                                 framebuffer.rdram_snapshot.size()))
            framebuffer.cpu_dirty = true;
    }

    // N64 Z/DZ state is maintained by GPUDepthBuffer in native-resolution
    // integer ping-pong textures. The color framebuffer deliberately has no
    // unrelated fixed-function depth attachment.
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.fbo);

    synchronize_cpu_tracking(framebuffer);
    return framebuffer;
}

GLFramebuffer *FramebufferManager::find(std::uint32_t address) {
    GLFramebuffer *best = nullptr;
    for (auto &[_, framebuffer] : buffers_) {
        if (!framebuffer.fbo || !framebuffer.color)
            continue;
        if (framebuffer.address == address && (!best || framebuffer.serial > best->serial))
            best = &framebuffer;
    }
    return best;
}

GLFramebuffer *FramebufferManager::find_overlapping(std::uint32_t address, std::uint32_t size) {
    GLFramebuffer *best = nullptr;
    std::uint64_t best_overlap = 0;
    const std::uint64_t query_begin = address;
    const std::uint64_t query_end = query_begin + std::max<std::uint32_t>(size, 1);

    for (auto &[_, framebuffer] : buffers_) {
        if (!framebuffer.fbo || !framebuffer.color)
            continue;

        const std::uint64_t begin = framebuffer.address;
        const std::uint64_t byte_count = framebuffer_byte_count(
            framebuffer.native_width, framebuffer.native_height, framebuffer.size);
        const std::uint64_t end = begin + byte_count;
        const std::uint64_t overlap_begin = std::max(begin, query_begin);
        const std::uint64_t overlap_end = std::min(end, query_end);
        if (overlap_end <= overlap_begin)
            continue;

        const std::uint64_t overlap = overlap_end - overlap_begin;
        const bool exact_start = address == framebuffer.address;
        const bool best_exact_start = best && address == best->address;
        if (!best || overlap > best_overlap ||
            (overlap == best_overlap && exact_start && !best_exact_start) ||
            (overlap == best_overlap && exact_start == best_exact_start &&
             framebuffer.serial > best->serial)) {
            best = &framebuffer;
            best_overlap = overlap;
        }
    }

    return best;
}

GLFramebuffer *FramebufferManager::find_containing(std::uint32_t address) {
    // VI can initially create a CPU image at a cropped origin, then RDP draws
    // a larger image starting before it. Select the latest image covering the
    // address rather than retaining that older exact-origin CPU image.
    GLFramebuffer *best = nullptr;
    for (auto &[_, framebuffer] : buffers_) {
        const auto end = std::uint64_t(framebuffer.address) +
                         framebuffer_byte_count(framebuffer.native_width, framebuffer.native_height,
                                                framebuffer.size);
        if (framebuffer.fbo && framebuffer.color && address >= framebuffer.address &&
            address < end && (!best || framebuffer.serial > best->serial))
            best = &framebuffer;
    }
    return best;
}

void FramebufferManager::upload_from_rdram(GLFramebuffer &framebuffer, const N64Memory &memory) {
    if (!framebuffer.color || framebuffer.width <= 0 || framebuffer.height <= 0)
        return;

    const int native_width = std::max(framebuffer.native_width, 1);
    const int native_height = std::max(framebuffer.native_height, 1);
    const int scale = std::max(framebuffer.internal_scale, 1);
    std::vector<std::uint8_t> rgba(std::size_t(framebuffer.width) * framebuffer.height * 4);

    for (int render_y = 0; render_y < framebuffer.height; ++render_y) {
        const int source_y = std::min(render_y / scale, native_height - 1);
        // RDRAM is top-to-bottom, while an FBO texture's row zero corresponds
        // to the bottom of the rendered image. Flip only at the API boundary.
        const int texture_y = framebuffer.height - 1 - render_y;

        for (int render_x = 0; render_x < framebuffer.width; ++render_x) {
            const int source_x = std::min(render_x / scale, native_width - 1);
            const auto output = (std::size_t(texture_y) * framebuffer.width + render_x) * 4;
            const std::size_t source_index = std::size_t(source_y) * native_width + source_x;

            if (framebuffer.size == RDPImageSize::Bits4) {
                const auto packed =
                    memory.read8(framebuffer.address + std::uint32_t(source_index / 2));
                const std::uint8_t nibble =
                    (source_index & 1u) ? std::uint8_t(packed & 0x0f) : std::uint8_t(packed >> 4);
                const std::uint8_t intensity = std::uint8_t(nibble * 17u);
                rgba[output + 0] = intensity;
                rgba[output + 1] = intensity;
                rgba[output + 2] = intensity;
                rgba[output + 3] = framebuffer.format == RDPImageFormat::I ? intensity : 255;
            } else if (framebuffer.size == RDPImageSize::Bits16) {
                const auto value =
                    memory.read16(framebuffer.address + std::uint32_t(source_index * 2));
                // Framebuffer/VI reads clear the low three bits. Texture
                // RGBA16 decoding separately replicates the five-bit value.
                const auto expand5 = [](unsigned v) { return std::uint8_t(v << 3); };
                rgba[output + 0] = expand5((value >> 11) & 31);
                rgba[output + 1] = expand5((value >> 6) & 31);
                rgba[output + 2] = expand5((value >> 1) & 31);
                // Bit 0 is the high coverage bit, rather than texture alpha.
                const unsigned coverage =
                    ((value & 1u) << 2) |
                    memory.read_hidden16(framebuffer.address + std::uint32_t(source_index * 2));
                rgba[output + 3] = std::uint8_t(((coverage + 1) * 255 + 4) / 8);
            } else if (framebuffer.size == RDPImageSize::Bits8) {
                const auto value = memory.read8(framebuffer.address + std::uint32_t(source_index));
                // RDP framebuffer reads treat every 8-bit color format as a
                // raw intensity byte. IA8 nibble decoding belongs to TMEM,
                // and would corrupt untouched pixels on a later readback.
                rgba[output + 0] = value;
                rgba[output + 1] = value;
                rgba[output + 2] = value;
                rgba[output + 3] = 255;
            } else {
                const auto value =
                    memory.read32(framebuffer.address + std::uint32_t(source_index * 4));
                rgba[output + 0] = std::uint8_t(value >> 24);
                rgba[output + 1] = std::uint8_t(value >> 16);
                rgba[output + 2] = std::uint8_t(value >> 8);
                rgba[output + 3] = std::uint8_t(value);
            }
        }
    }

    const auto bytes = framebuffer_byte_count(native_width, native_height, framebuffer.size);
    framebuffer.rdram_snapshot.resize(std::size_t(bytes));
    memory.copy_be(framebuffer.address, framebuffer.rdram_snapshot.data(),
                   framebuffer.rdram_snapshot.size());
    glBindTexture(GL_TEXTURE_2D, framebuffer.color);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, framebuffer.width, framebuffer.height, GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba.data());
    framebuffer.cpu_written.assign(framebuffer.rdram_snapshot.size(), 0);
    framebuffer.cpu_dirty = false;
    framebuffer.gpu_dirty = false;
}

void FramebufferManager::download_to_rdram(GLFramebuffer &framebuffer, N64Memory &memory) {
    if (!framebuffer.fbo || framebuffer.width <= 0 || framebuffer.height <= 0)
        return;

    std::vector<std::uint8_t> rgba(std::size_t(framebuffer.width) * framebuffer.height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, framebuffer.width, framebuffer.height, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());

    const int native_width = std::max(framebuffer.native_width, 1);
    const int native_height = std::max(framebuffer.native_height, 1);
    const int scale = std::max(framebuffer.internal_scale, 1);

    for (int y = 0; y < native_height; ++y) {
        for (int x = 0; x < native_width; ++x) {
            const int render_x = std::min(x * scale + scale / 2, framebuffer.width - 1);
            const int render_y = std::min(y * scale + scale / 2, framebuffer.height - 1);
            const int texture_y = framebuffer.height - 1 - render_y;
            const auto input = (std::size_t(texture_y) * framebuffer.width + render_x) * 4;
            const std::size_t destination_index = std::size_t(y) * native_width + x;
            const unsigned bits = 4u << unsigned(framebuffer.size);
            const std::size_t byte_offset = destination_index * bits / 8;
            const unsigned bytes = std::max(bits / 8, 1u);
            // CPU stores and DMA can reuse an old image before its next RDP list.
            // Preserve those writes even when the core did not track that old image.
            bool cpu_changed = framebuffer.rdram_snapshot.size() < byte_offset + bytes;
            for (unsigned b = 0; !cpu_changed && b < bytes; ++b)
                cpu_changed = (byte_offset + b < framebuffer.cpu_written.size() &&
                               framebuffer.cpu_written[byte_offset + b]) ||
                              memory.read8(framebuffer.address + std::uint32_t(byte_offset + b)) !=
                                  framebuffer.rdram_snapshot[byte_offset + b];
            if (cpu_changed) {
                framebuffer.cpu_dirty = true;
                continue;
            }

            if (framebuffer.size == RDPImageSize::Bits4) {
                const unsigned intensity =
                    (unsigned(rgba[input + 0]) + rgba[input + 1] + rgba[input + 2]) / 3;
                const std::uint8_t nibble = std::uint8_t((intensity * 15u + 127u) / 255u);
                const std::uint32_t byte_address =
                    framebuffer.address + std::uint32_t(destination_index / 2);
                std::uint8_t packed = memory.read8(byte_address);
                if (destination_index & 1u)
                    packed = std::uint8_t((packed & 0xf0u) | nibble);
                else
                    packed = std::uint8_t((packed & 0x0fu) | (nibble << 4));
                memory.write8(byte_address, packed);
            } else if (framebuffer.size == RDPImageSize::Bits16) {
                const std::uint16_t value = std::uint16_t((rgba[input + 0] >> 3) << 11) |
                                            std::uint16_t((rgba[input + 1] >> 3) << 6) |
                                            std::uint16_t((rgba[input + 2] >> 3) << 1) |
                                            (rgba[input + 3] > 128);
                memory.write16(framebuffer.address + std::uint32_t(destination_index * 2), value);
                const unsigned samples = (unsigned(rgba[input + 3]) * 8 + 127) / 255;
                memory.write_hidden16(framebuffer.address + std::uint32_t(destination_index * 2),
                                      std::uint8_t(samples ? samples - 1 : 0));
            } else if (framebuffer.size == RDPImageSize::Bits8) {
                const std::uint8_t value =
                    rgba[input + ((framebuffer.address + destination_index) & 1u)];
                memory.write8(framebuffer.address + std::uint32_t(destination_index), value);
            } else {
                const std::uint32_t value = (std::uint32_t(rgba[input + 0]) << 24) |
                                            (std::uint32_t(rgba[input + 1]) << 16) |
                                            (std::uint32_t(rgba[input + 2]) << 8) | rgba[input + 3];
                const auto address = framebuffer.address + std::uint32_t(destination_index * 4);
                const auto prior = memory.read32(address);
                memory.write32(address, value);
                if (value != prior) {
                    memory.write_hidden16(address, (value & 0x10000) ? 3 : 0);
                    memory.write_hidden16(address + 2, (value & 1) ? 3 : 0);
                }
            }
        }
    }

    memory.copy_be(framebuffer.address, framebuffer.rdram_snapshot.data(),
                   framebuffer.rdram_snapshot.size());
    framebuffer.gpu_dirty = false;
    if (readback_callback_)
        readback_callback_(framebuffer.address, std::uint32_t(framebuffer.rdram_snapshot.size()));
}

void FramebufferManager::commit_color_range(std::uint32_t address, std::uint32_t size) {
    if (!memory_ || !size)
        return;
    const std::uint64_t end = std::uint64_t(address) + size;
    std::vector<GLFramebuffer *> pending;
    for (auto &[_, f] : buffers_) {
        const auto image_end = std::uint64_t(f.address) +
                               framebuffer_byte_count(f.native_width, f.native_height, f.size);
        if (f.gpu_dirty && !f.raw_z && cpu_eligible(f) && std::uint64_t(f.address) < end &&
            address < image_end)
            pending.push_back(&f);
    }
    std::sort(pending.begin(), pending.end(),
              [](const auto *a, const auto *b) { return a->serial < b->serial; });
    for (auto *f : pending)
        download_to_rdram(*f, *memory_);
}

void FramebufferManager::depth_written_to_ram(std::uint32_t address, std::uint32_t size) {
    for (auto &[_, f] : buffers_)
        if (std::uint64_t(address) <
                std::uint64_t(f.address) +
                    framebuffer_byte_count(f.native_width, f.native_height, f.size) &&
            std::uint64_t(f.address) < std::uint64_t(address) + size) {
            // Z activation synchronized earlier color writes. The committed depth
            // is now the newer physical view, including changes to hidden bits.
            f.cpu_dirty = true;
            f.gpu_dirty = false;
        }
}

void FramebufferManager::record_fill32_hidden(const RDPFillRectangle &r, const RDPState &s) {
    if (!memory_ || s.color_image.size != RDPImageSize::Bits32 || ((s.other_modes >> 52) & 3) != 3)
        return;
    const auto *f = find(s.color_image.address);
    if (!f || !f->fbo)
        return;
    int x0 = r.xh >> 2, y0 = r.yh >> 2, x1 = (r.xl >> 2) + 1, y1 = (r.yl >> 2) + 1;
    if (s.scissor_xl > s.scissor_xh && s.scissor_yl > s.scissor_yh) {
        x0 = std::max(x0, int(s.scissor_xh >> 2));
        y0 = std::max(y0, int(s.scissor_yh >> 2));
        x1 = std::min(x1, int((s.scissor_xl + 3) >> 2));
        y1 = std::min(y1, int((s.scissor_yl + 3) >> 2));
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, f->native_width);
    y1 = std::min(y1, f->native_height);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const auto address =
                std::uint64_t(f->address) + (std::uint64_t(y) * f->native_width + x) * 4;
            if (address + 4 > memory_->size())
                continue;
            memory_->write_hidden16(std::uint32_t(address), (s.fill_color & 0x10000) ? 3 : 0);
            memory_->write_hidden16(std::uint32_t(address + 2), (s.fill_color & 1) ? 3 : 0);
        }
}

void FramebufferManager::invalidate_cpu_write(std::uint32_t address, std::uint32_t size) {
    const std::uint64_t write_begin = address;
    const std::uint64_t write_end = write_begin + std::max<std::uint32_t>(size, 1);

    for (auto &[_, framebuffer] : buffers_) {
        const std::uint64_t begin = framebuffer.address;
        const std::uint64_t end =
            begin + framebuffer_byte_count(framebuffer.native_width, framebuffer.native_height,
                                           framebuffer.size);
        if (write_begin < end && write_end > begin) {
            framebuffer.cpu_dirty = true;
            framebuffer.cpu_written.resize(std::size_t(end - begin));
            const auto first = std::size_t(std::max(write_begin, begin) - begin);
            const auto last = std::size_t(std::min(write_end, end) - begin);
            std::fill(framebuffer.cpu_written.begin() + first,
                      framebuffer.cpu_written.begin() + last, 1);
        }
    }
}
