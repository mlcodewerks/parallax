#include "rdp_profile.hpp"
#include "framebuffer_manager.hpp"
#include "rdp_tmem.hpp"
#include "core/framebuffer_notifications.h"
#include <stdexcept>
#include <string>

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
    if (framebuffer.alias_base_color) glDeleteTextures(1, &framebuffer.alias_base_color);
    if (framebuffer.alias_hidden) glDeleteTextures(1, &framebuffer.alias_hidden);
    framebuffer.alias_base_color = framebuffer.alias_hidden = 0;
    framebuffer.alias_serial = 0;
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

bool GLFramebuffer::gpu_pending_range(std::uint32_t query, std::uint32_t bytes) const {
    if (!gpu_dirty || raw_z || native_width <= 0 || native_height <= 0 || !bytes)
        return false;
    const auto end = std::uint64_t(query) + bytes;
    if (end <= address || query >= std::uint64_t(address) +
                                   framebuffer_byte_count(native_width, native_height, size))
        return false;
    if (size == RDPImageSize::Bits4 || dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0)
        return true;
    const unsigned pixel_bytes = (4u << unsigned(size)) / 8u;
    const auto first = (std::max<std::uint64_t>(query, address) - address) / pixel_bytes;
    const auto last = (end - 1 - address) / pixel_bytes;
    const auto y0 = std::max<std::uint64_t>(dirty_y0, first / native_width);
    const auto y1 = std::min<std::uint64_t>(dirty_y1 - 1, last / native_width);
    if (y0 > y1)
        return false;
    const auto row_overlaps = [&](std::uint64_t y) {
        const auto begin = std::uint64_t(address) + (y * native_width + dirty_x0) * pixel_bytes;
        const auto row_end = std::uint64_t(address) + (y * native_width + dirty_x1) * pixel_bytes;
        return query < row_end && begin < end;
    };
    return y1 > y0 + 1 || row_overlaps(y0) || row_overlaps(y1);
}

FramebufferManager::~FramebufferManager() {
    clear();
    aliases_.clear();
}

void FramebufferManager::clear() {
    for (auto &[_, framebuffer] : buffers_)
        destroy_color_target(framebuffer);

    buffers_.clear();
    readback_pixels_.clear();
    last_bound_ = nullptr;
    max_texture_size_ = 0;
    if (depth_export_program_) glDeleteProgram(depth_export_program_);
    if (depth_export_vao_) glDeleteVertexArrays(1, &depth_export_vao_);
    if (depth_export_base_) glDeleteTextures(1, &depth_export_base_);
    depth_export_program_ = depth_export_vao_ = depth_export_base_ = 0;
    depth_export_width_ = depth_export_height_ = 0;
    depth_export_pixels_.clear();
}

bool FramebufferManager::cpu_eligible(const GLFramebuffer &f) const {
    return memory_ && f.fbo && (f.address || native_notifications_) &&
           (native_notifications_ || f.size != RDPImageSize::Bits4) &&
           uint64_t(f.address) + framebuffer_byte_count(f.native_width, f.native_height, f.size) <=
               memory_->size();
}

void FramebufferManager::synchronize_cpu_tracking(GLFramebuffer &active) {
    if (native_notifications_) {
        active.cpu_untracked = false; last_bound_ = &active;
        last_bound_eligible_ = cpu_eligible(active); return;
    }
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
        GLRDP_PROFILE_SCOPE(color_retire);
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
    GLRDP_PROFILE_SCOPE(color_alloc);
    GLRDP_PROFILE_EVENT("color_alloc", framebuffer.address, width, height);
    destroy_color_target(framebuffer);
    ++framebuffer.texture_version;

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
    framebuffer.clear_gpu_written();
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
                                                    std::uint32_t z_address, int requested_scale, bool bind_gl) {
    GLRDP_PROFILE_SCOPE(color_bind);
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
        std::vector<GLFramebuffer *> aliases;
        for (auto &[_, other] : buffers_) {
            if (&other == &framebuffer || !other.gpu_dirty || other.raw_z || !cpu_eligible(other))
                continue;
            const std::uint64_t other_end =
                std::uint64_t(other.address) +
                framebuffer_byte_count(other.native_width, other.native_height, other.size);
            if (begin < other_end && std::uint64_t(other.address) < end) {
                if (native_notifications_ && aliases_.ready() &&
                    framebuffer.size != RDPImageSize::Bits4 && !framebuffer.raw_z &&
                    native_width <= max_texture_size && native_height <= max_texture_size &&
                    other.alias_base_color && other.alias_hidden) {
                    if (other.write_serial > framebuffer.alias_serial &&
                        other.gpu_pending_range(framebuffer.address, std::uint32_t(end - begin)))
                        aliases.push_back(&other);
                } else {
                    GLRDP_PROFILE_SCOPE(color_alias);
                    download_to_rdram(other, *memory_);
                }
            }
        }
        if (!framebuffer.rdram_snapshot.empty() && cpu_eligible(framebuffer) &&
            !memory_->matches_be(framebuffer.address, framebuffer.rdram_snapshot.data(),
                                 framebuffer.rdram_snapshot.size()))
            framebuffer.cpu_dirty = true;
        if (!aliases.empty()) {
            if (framebuffer.cpu_dirty) {
                if (framebuffer.gpu_dirty && !framebuffer.raw_z) download_to_rdram(framebuffer, *memory_);
                upload_from_rdram(framebuffer, *memory_);
            }
            // Materialize this view's own packing/coverage on the GPU before
            // changing its interpretation, including inherited hidden bits.
            bool normalize = framebuffer.gpu_dirty;
            std::sort(aliases.begin(), aliases.end(), [](auto a, auto b) { return a->write_serial < b->write_serial; });
            for (auto *source : aliases) if (source->gpu_dirty) {
                aliases_.copy(*source, framebuffer, *memory_, normalize);
                normalize = false;
            }
            aliases_.finish(framebuffer);
        }
        framebuffer.alias_serial = framebuffer.serial;
    }

    // N64 Z/DZ state is maintained by GPUDepthBuffer in native-resolution
    // integer ping-pong textures. The color framebuffer deliberately has no
    // unrelated fixed-function depth attachment.
    if (bind_gl) glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.fbo);

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

GLFramebuffer *FramebufferManager::find_image(const RDPImage &image) {
    const auto it = buffers_.find(image_key(image));
    return it == buffers_.end() ? nullptr : &it->second;
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

GLFramebuffer *FramebufferManager::find_containing(std::uint32_t address, std::uint32_t span) {
    // VI can initially create a CPU image at a cropped origin, then RDP draws
    // a larger image starting before it. Select the latest image covering the
    // address rather than retaining that older exact-origin CPU image.
    GLFramebuffer *best = nullptr;
    std::uint64_t best_coverage = 0;
    for (auto &[_, framebuffer] : buffers_) {
        const auto end = std::uint64_t(framebuffer.address) +
                         framebuffer_byte_count(framebuffer.native_width, framebuffer.native_height,
                                                framebuffer.size);
        if (framebuffer.fbo && framebuffer.color && address >= framebuffer.address &&
            address < end) {
            // Scissor extents can let adjacent images overlap by a few rows.
            // WDC's 482-row menu images are 480 rows apart: the newest image
            // may contain the VI origin only in its tail. Prefer the view
            // covering the scanout, then recency among equally complete views.
            const auto coverage = std::min<std::uint64_t>(std::max(span, 1u), end - address);
            if (!best || coverage > best_coverage ||
                (coverage == best_coverage && framebuffer.serial > best->serial)) {
                best = &framebuffer;
                best_coverage = coverage;
            }
        }
    }
    return best;
}

void FramebufferManager::upload_from_rdram(GLFramebuffer &framebuffer, const N64Memory &memory) {
    GLRDP_PROFILE_SCOPE(color_upload);
    if (!framebuffer.color || framebuffer.width <= 0 || framebuffer.height <= 0)
        return;
    ++framebuffer.texture_version;

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
    if (native_notifications_ && aliases_.ready() && framebuffer.size != RDPImageSize::Bits4 &&
        native_width <= max_texture_size_ && native_height <= max_texture_size_)
        aliases_.upload_base(framebuffer, memory, rgba.data());
    framebuffer.cpu_written.assign(framebuffer.rdram_snapshot.size(), 0);
    framebuffer.cpu_dirty = false;
    framebuffer.clear_gpu_written();
}

void FramebufferManager::download_to_rdram(GLFramebuffer &framebuffer, N64Memory &memory) {
    if (native_notifications_ && !committing_aliases_) {
        commit_color_range(framebuffer.address, std::uint32_t(framebuffer_byte_count(
            framebuffer.native_width, framebuffer.native_height, framebuffer.size)));
        return;
    }
    GLRDP_PROFILE_SCOPE(color_read);
    GLRDP_PROFILE_EVENT("color_read", framebuffer.address, framebuffer.native_width, framebuffer.native_height);
    if (!framebuffer.fbo || framebuffer.width <= 0 || framebuffer.height <= 0)
        return;

    const int native_width = std::max(framebuffer.native_width, 1);
    const int native_height = std::max(framebuffer.native_height, 1);
    const int scale = std::max(framebuffer.internal_scale, 1);
    const bool bounded = framebuffer.gpu_dirty && framebuffer.size != RDPImageSize::Bits4 &&
                         framebuffer.dirty_x1 > framebuffer.dirty_x0 &&
                         framebuffer.dirty_y1 > framebuffer.dirty_y0;
    const int x0 = bounded ? std::clamp(framebuffer.dirty_x0, 0, native_width) : 0;
    const int y0 = bounded ? std::clamp(framebuffer.dirty_y0, 0, native_height) : 0;
    const int x1 = bounded ? std::clamp(framebuffer.dirty_x1, x0, native_width) : native_width;
    const int y1 = bounded ? std::clamp(framebuffer.dirty_y1, y0, native_height) : native_height;
    const int read_x = std::min(x0 * scale + scale / 2, framebuffer.width - 1);
    const int read_y = framebuffer.height - 1 -
                       std::min((y1 - 1) * scale + scale / 2, framebuffer.height - 1);
    const int read_width = std::min((x1 - 1) * scale + scale / 2, framebuffer.width - 1) + 1 - read_x;
    const int read_height = framebuffer.height -
                            std::min(y0 * scale + scale / 2, framebuffer.height - 1) - read_y;
    if (read_width <= 0 || read_height <= 0)
        return;
    // Alias imports may carry hidden coverage which cannot be reconstructed
    // from a 32-bit color word. Resolve this view's own writes before the CPU
    // explicitly requests its visible and hidden memory.
    std::vector<std::uint8_t> alias_hidden;
    if (native_notifications_ && aliases_.ready() && framebuffer.alias_hidden &&
        framebuffer.size == RDPImageSize::Bits32) {
        aliases_.copy(framebuffer, framebuffer, memory);
        aliases_.finish(framebuffer);
        alias_hidden.resize(std::size_t(native_width) * native_height * 4);
        GLuint hidden_fbo = 0;
        glGenFramebuffers(1, &hidden_fbo); glBindFramebuffer(GL_FRAMEBUFFER, hidden_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebuffer.alias_hidden, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (glrdp_gles3) {
            std::vector<std::uint32_t> pixels(alias_hidden.size());
            glReadPixels(0, 0, native_width, native_height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, pixels.data());
            std::transform(pixels.begin(), pixels.end(), alias_hidden.begin(), [](auto value) { return std::uint8_t(value); });
        } else glReadPixels(0, 0, native_width, native_height, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, alias_hidden.data());
        glDeleteFramebuffers(1, &hidden_fbo);
    }
    // Retain capacity across the many small framebuffer copies in Tennis/Golf.
    readback_pixels_.resize(std::size_t(read_width) * read_height * 4);
    auto &rgba = readback_pixels_;
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(read_x, read_y, read_width, read_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    const bool unchanged =
        std::none_of(framebuffer.cpu_written.begin(), framebuffer.cpu_written.end(),
                     [](std::uint8_t written) { return written != 0; }) &&
        memory.matches_be(framebuffer.address, framebuffer.rdram_snapshot.data(),
                          framebuffer.rdram_snapshot.size());
    if (!unchanged)
        framebuffer.cpu_dirty = true;
    std::vector<std::uint8_t> nibble_cpu_written;
    if (!unchanged && framebuffer.size == RDPImageSize::Bits4) {
        // Both pixels share one byte. Capture overlays before writing either
        // nibble so our first GPU write is not mistaken for a CPU store.
        nibble_cpu_written.resize(framebuffer.rdram_snapshot.size());
        for (std::size_t i = 0; i < nibble_cpu_written.size(); ++i)
            nibble_cpu_written[i] = (i < framebuffer.cpu_written.size() && framebuffer.cpu_written[i]) ||
                memory.read8(framebuffer.address + unsigned(i)) != framebuffer.rdram_snapshot[i];
    }
    if (unchanged && framebuffer.size == RDPImageSize::Bits16 && scale == 1 &&
        !(framebuffer.address & 1u) && framebuffer.width == native_width &&
        framebuffer.height == native_height &&
        framebuffer.rdram_snapshot.size() == std::size_t(native_width) * native_height * 2) {
        for (int y = y0; y < y1; ++y)
            memory.write_rgba16(framebuffer.address + std::uint32_t((y * native_width + x0) * 2),
                rgba.data() + std::size_t(y1 - 1 - y) * read_width * 4, x1 - x0);
    } else {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const int render_x = std::min(x * scale + scale / 2, framebuffer.width - 1);
                const int render_y = std::min(y * scale + scale / 2, framebuffer.height - 1);
                const int texture_y = framebuffer.height - 1 - render_y;
                const auto input = (std::size_t(texture_y - read_y) * read_width + render_x - read_x) * 4;
                const std::size_t destination_index = std::size_t(y) * native_width + x;
                const unsigned bits = 4u << unsigned(framebuffer.size);
                const std::size_t byte_offset = destination_index * bits / 8;
                const unsigned bytes = std::max(bits / 8, 1u);
                // CPU stores and DMA can reuse an old image before its next RDP list.
                // Preserve those writes even when the core did not track that old image.
                bool cpu_changed = !unchanged && framebuffer.rdram_snapshot.size() < byte_offset + bytes;
                if (!unchanged && !cpu_changed) {
                    if (framebuffer.size == RDPImageSize::Bits4) cpu_changed = nibble_cpu_written[byte_offset];
                    else for (unsigned b = 0; !cpu_changed && b < bytes; ++b)
                        cpu_changed = (byte_offset + b < framebuffer.cpu_written.size() &&
                                       framebuffer.cpu_written[byte_offset + b]) ||
                                      memory.read8(framebuffer.address + std::uint32_t(byte_offset + b)) !=
                                          framebuffer.rdram_snapshot[byte_offset + b];
                }
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
                    if (!alias_hidden.empty()) {
                        memory.write_hidden16(address, alias_hidden[destination_index * 4]);
                        memory.write_hidden16(address + 2, alias_hidden[destination_index * 4 + 1]);
                    } else if (value != prior) {
                        memory.write_hidden16(address, (value & 0x10000) ? 3 : 0);
                        memory.write_hidden16(address + 2, (value & 1) ? 3 : 0);
                    }
                }
            }
        }
    }

    memory.copy_be(framebuffer.address, framebuffer.rdram_snapshot.data(),
                   framebuffer.rdram_snapshot.size());
    framebuffer.clear_gpu_written();
    if (native_notifications_) {
        // Renderer commits are not CPU stores. Refresh the baseline of newer
        // views so their pending GPU writes are not mistaken for CPU overlays.
        // Older views will import the now-current RAM when they are reused.
        for (auto &[_, other] : buffers_) {
            if (&other == &framebuffer || other.rdram_snapshot.empty()) continue;
            const auto begin = std::max<std::uint64_t>(framebuffer.address, other.address);
            const auto end = std::min<std::uint64_t>(std::uint64_t(framebuffer.address) + framebuffer.rdram_snapshot.size(),
                std::uint64_t(other.address) + other.rdram_snapshot.size());
            if (begin >= end) continue;
            memory.copy_be(std::uint32_t(begin), other.rdram_snapshot.data() + (begin - other.address), end - begin);
            if (other.write_serial < framebuffer.write_serial) other.cpu_dirty = true;
        }
    }
    if (readback_callback_)
        readback_callback_(framebuffer.address, std::uint32_t(framebuffer.rdram_snapshot.size()));
}

void FramebufferManager::commit_color_range(std::uint32_t address, std::uint32_t size, bool texture_read) {
    if (!memory_ || !size)
        return;
    if (native_notifications_) {
        // Resolve the connected alias group in write order. A view can span
        // outside the requested page; include older overlapping owners before
        // committing its complete pending rectangle.
        std::vector<GLFramebuffer *> pending;
        std::uint64_t begin = address, end = begin + size;
        bool added;
        do {
            added = false;
            for (auto &[_, f] : buffers_) {
                const auto last = std::uint64_t(f.address) + framebuffer_byte_count(f.native_width, f.native_height, f.size);
                if (!f.gpu_dirty || f.raw_z || !cpu_eligible(f) || begin >= last || f.address >= end ||
                    std::find(pending.begin(), pending.end(), &f) != pending.end()) continue;
                pending.push_back(&f); begin = std::min<std::uint64_t>(begin, f.address); end = std::max(end, last); added = true;
            }
        } while (added);
        // Remember unnotified external writes before any renderer commit can
        // change RAM under another view's snapshot. Native CPU/DMA writes are
        // already recorded, including stores of the same value.
        for (auto *f : pending) {
            f->cpu_written.resize(f->rdram_snapshot.size());
            for (std::size_t i = 0; i < f->rdram_snapshot.size(); ++i)
                if (memory_->read8(f->address + unsigned(i)) != f->rdram_snapshot[i]) f->cpu_written[i] = 1;
        }
        std::sort(pending.begin(), pending.end(), [](auto a, auto b) { return a->write_serial < b->write_serial; });
        committing_aliases_ = true;
        try { for (auto *f : pending) download_to_rdram(*f, *memory_); }
        catch (...) { committing_aliases_ = false; throw; }
        committing_aliases_ = false;
        return;
    }
    std::vector<GLFramebuffer *> pending;
    for (auto &[_, f] : buffers_) {
        const auto image_end = std::uint64_t(f.address) +
                               framebuffer_byte_count(f.native_width, f.native_height, f.size);
        // Depth ownership changes invalidate complete color views. Commit even
        // their disjoint pending pixels before that view can be discarded.
        const bool overlaps = texture_read ? f.gpu_pending_range(address, size) :
            f.gpu_dirty && !f.raw_z && address < image_end && f.address < std::uint64_t(address) + size;
        if (overlaps && f.fbo && f.color && image_end <= memory_->size())
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
            f.clear_gpu_written();
        }
}

void FramebufferManager::discard_overwritten(std::uint32_t address, std::uint32_t size) {
    const auto end = std::uint64_t(address) + size;
    for (auto &[_, f] : buffers_) {
        const auto bytes = framebuffer_byte_count(f.native_width, f.native_height, f.size);
        const bool bounded = f.size != RDPImageSize::Bits4 && f.dirty_x1 > f.dirty_x0 && f.dirty_y1 > f.dirty_y0;
        const unsigned pixel_bytes = (4u << unsigned(f.size)) / 8u;
        const auto first = std::uint64_t(f.address) + (bounded ?
            (std::uint64_t(f.dirty_y0) * f.native_width + f.dirty_x0) * pixel_bytes : 0);
        const auto last = std::uint64_t(f.address) + (bounded ?
            (std::uint64_t(f.dirty_y1 - 1) * f.native_width + f.dirty_x1) * pixel_bytes : bytes);
        if (f.gpu_dirty && !f.raw_z && first >= address && last <= end) {
            f.clear_gpu_written(); f.cpu_dirty = true;
        }
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
            if (native_notifications_ && framebuffer.gpu_dirty && !framebuffer.raw_z &&
                framebuffer.size != RDPImageSize::Bits4) {
                const auto bytes = 1u << (unsigned(framebuffer.size) - 1);
                const bool bounded = framebuffer.dirty_x1 > framebuffer.dirty_x0 &&
                                     framebuffer.dirty_y1 > framebuffer.dirty_y0;
                const auto pending_begin = bounded ? begin +
                    (std::uint64_t(framebuffer.dirty_y0) * framebuffer.native_width + framebuffer.dirty_x0) * bytes : begin;
                const auto pending_end = bounded ? begin +
                    (std::uint64_t(framebuffer.dirty_y1 - 1) * framebuffer.native_width + framebuffer.dirty_x1) * bytes : end;
                if (write_begin <= pending_begin && write_end >= pending_end)
                    framebuffer.clear_gpu_written();
            }
        }
    }
}


namespace alias_detail {
const char *vertex = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char *common = R"(
uniform sampler2D source_color, source_base, previous_color, destination_base;
uniform int destination_pending;
uniform ivec4 destination_dirty;
uniform usampler2D source_hidden, previous_hidden, source_mask;
uniform ivec4 src; // address, native width, native height, byte shift
uniform ivec4 dst;
uniform ivec4 scale; // source scale, destination scale, unused, unused
uvec4 bytes(sampler2D t, ivec2 p) { return uvec4(floor(texelFetch(t, p, 0) * 255.0 + 0.5)); }
ivec2 source_position(uint pixel) { return ivec2(int(pixel % uint(src.y)), int(pixel / uint(src.y))); }
ivec2 source_render(ivec2 p) {
    return clamp(ivec2(p.x * scale.x + scale.x / 2, src.z * scale.x - 1 - (p.y * scale.x + scale.x / 2)),
                 ivec2(0), textureSize(source_color, 0) - 1);
}
bool owned(uint address, out uint pixel) {
    if (address < uint(src.x)) return false;
    pixel = (address - uint(src.x)) >> uint(src.w);
    if (pixel >= uint(src.y * src.z)) return false;
    return texelFetch(source_mask, source_position(pixel), 0).r != 0u;
}
uint packed_color(uvec4 c, int shift) {
    if (shift == 1) return ((c.r >> 3u) << 11u) | ((c.g >> 3u) << 6u) | ((c.b >> 3u) << 1u) | uint(c.a > 128u);
    return (c.r << 24u) | (c.g << 16u) | (c.b << 8u) | c.a;
}
uint source_byte(uint address, uint pixel) {
    uvec4 c = bytes(source_color, source_render(source_position(pixel)));
    if (src.w == 0) return c[address & 1u];
    uint count = 1u << uint(src.w), offset = (address - uint(src.x)) & (count - 1u);
    return (packed_color(c, src.w) >> ((count - 1u - offset) * 8u)) & 255u;
}
uint hidden_at(uint address, uint prior) {
    // Hidden bits use physical halfwords, including odd color origins.
    uint at = (address & ~1u) + (uint(src.x) & 1u), pixel;
    if (!owned(at, pixel)) return prior;
    ivec2 p = source_position(pixel), r = source_render(p);
    uvec4 inherited = texelFetch(source_hidden, p, 0);
    if (src.w == 0) return inherited.r;
    uvec4 c = bytes(source_color, r);
    if (src.w == 1) {
        uint samples = (c.a * 8u + 127u) / 255u;
        return (samples != 0u ? samples - 1u : 0u) & 3u;
    }
    bool second = ((at - uint(src.x)) & 2u) != 0u;
    uint value = packed_color(c, src.w);
    if (value == packed_color(bytes(source_base, r), src.w)) return second ? inherited.g : inherited.r;
    return (value & (second ? 1u : 65536u)) != 0u ? 3u : 0u;
}
uvec4 result_hidden(ivec2 p) {
    uint address = uint(dst.x) + uint(p.y * dst.y + p.x) * (1u << uint(dst.w));
    uvec4 old = texelFetch(previous_hidden, p, 0);
    if (destination_pending != 0 && all(greaterThanEqual(p, destination_dirty.xy)) && all(lessThan(p, destination_dirty.zw))) {
        ivec2 r = clamp(ivec2(p.x * scale.y + scale.y / 2, dst.z * scale.y - 1 - (p.y * scale.y + scale.y / 2)),
                        ivec2(0), textureSize(previous_color, 0) - 1);
        uvec4 c = bytes(previous_color, r);
        if (dst.w == 1) {
            uint samples = (c.a * 8u + 127u) / 255u;
            old.r = (samples != 0u ? samples - 1u : 0u) & 3u;
        } else if (dst.w == 2 && packed_color(c, dst.w) != packed_color(bytes(destination_base, r), dst.w)) {
            uint value = packed_color(c, dst.w);
            old.r = (value & 65536u) != 0u ? 3u : 0u;
            old.g = (value & 1u) != 0u ? 3u : 0u;
        }
    }
    old.r = hidden_at(address, old.r);
    if (dst.w == 2) old.g = hidden_at(address + 2u, old.g);
    return old;
}
)";
const char *color_main = R"(
layout(location=0) out vec4 color;
void main() {
    ivec2 render = ivec2(gl_FragCoord.xy);
    ivec2 p = ivec2(render.x / scale.y, dst.z - 1 - render.y / scale.y);
    uvec4 old = bytes(previous_color, render);
    uint address = uint(dst.x) + uint(p.y * dst.y + p.x) * (1u << uint(dst.w));
    uint value = dst.w == 0 ? old[address & 1u] : packed_color(old, dst.w);
    uint count = 1u << uint(dst.w);
    for (uint b = 0u; b < count; ++b) {
        uint pixel;
        if (owned(address + b, pixel)) {
            uint shift = (count - 1u - b) * 8u;
            value = (value & ~(255u << shift)) | (source_byte(address + b, pixel) << shift);
        }
    }
    uvec4 c;
    if (dst.w == 0) c = uvec4(value, value, value, 255u);
    else if (dst.w == 1) {
        uint coverage = ((value & 1u) << 2u) | result_hidden(p).r;
        c = uvec4(((value >> 11u) & 31u) << 3u, ((value >> 6u) & 31u) << 3u,
                  ((value >> 1u) & 31u) << 3u, ((coverage + 1u) * 255u + 4u) / 8u);
    } else c = uvec4(value >> 24u, (value >> 16u) & 255u, (value >> 8u) & 255u, value & 255u);
    color = vec4(c) / 255.0;
}
)";
const char *hidden_main = R"(
layout(location=0) out uvec4 hidden;
void main() { hidden = result_hidden(ivec2(gl_FragCoord.xy)); }
)";
GLuint shader(GLenum type, const std::string &text) {
    GLuint id = glCreateShader(type); const char *p = text.c_str();
    glShaderSource(id, 1, &p, nullptr); glCompileShader(id);
    GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0; glGetShaderiv(id, GL_INFO_LOG_LENGTH, &length);
        std::string error(std::max(length, 1), '\0'); glGetShaderInfoLog(id, length, nullptr, error.data());
        glDeleteShader(id); throw std::runtime_error(error);
    }
    return id;
}
std::string source(const char *text) {
    std::string result = text;
    if (glrdp_gles3) result.replace(0, result.find('\n') + 1,
        "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp usampler2D;\n");
    return result;
}
void texture(GLuint &id, int width, int height, bool integer, const void *data = nullptr) {
    if (!id) glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, integer ? GL_RGBA8UI : GL_RGBA8, width, height, 0,
                 integer ? GL_RGBA_INTEGER : GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
}

void FramebufferAlias::initialize() {
    if (ready()) return;
    glGenVertexArrays(1, &vao_); glGenFramebuffers(2, framebuffers_);
    const auto vs = alias_detail::shader(GL_VERTEX_SHADER, alias_detail::source(alias_detail::vertex));
    for (unsigned i = 0; i < 2; ++i) {
        auto fs = alias_detail::shader(GL_FRAGMENT_SHADER, alias_detail::source("#version 330 core\n") + alias_detail::common + (i ? alias_detail::hidden_main : alias_detail::color_main));
        programs_[i] = glCreateProgram(); glAttachShader(programs_[i], vs); glAttachShader(programs_[i], fs);
        glLinkProgram(programs_[i]); glDeleteShader(fs);
        GLint ok = 0; glGetProgramiv(programs_[i], GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteShader(vs); throw std::runtime_error("Framebuffer alias program link failed"); }
    }
    glDeleteShader(vs);
    const char *names[] = {"source_color", "source_base", "previous_color", "source_hidden", "previous_hidden", "source_mask",
                            "destination_base", "src", "dst", "scale", "destination_pending", "destination_dirty"};
    for (unsigned pass = 0; pass < 2; ++pass) {
        glUseProgram(programs_[pass]);
        for (unsigned i = 0; i < 12; ++i) uniforms_[pass][i] = glGetUniformLocation(programs_[pass], names[i]);
        for (unsigned i = 0; i < 7; ++i) glUniform1i(uniforms_[pass][i], i);
    }
    glGenTextures(1, &mask_);
}
void FramebufferAlias::clear() {
    for (auto p : programs_) if (p) glDeleteProgram(p);
    programs_[0] = programs_[1] = 0;
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (framebuffers_[0]) glDeleteFramebuffers(2, framebuffers_);
    if (previous_color_) glDeleteTextures(1, &previous_color_);
    if (previous_hidden_) glDeleteTextures(1, &previous_hidden_);
    if (mask_) glDeleteTextures(1, &mask_);
    vao_ = framebuffers_[0] = framebuffers_[1] = previous_color_ = previous_hidden_ = mask_ = 0;
    width_ = height_ = native_width_ = native_height_ = mask_width_ = mask_height_ = 0;
    mask_pixels_.clear(); hidden_pixels_.clear();
}
void FramebufferAlias::upload_base(GLFramebuffer &f, const N64Memory &memory, const std::uint8_t *rgba) {
    glActiveTexture(GL_TEXTURE0); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!f.alias_base_color) alias_detail::texture(f.alias_base_color, f.width, f.height, false, rgba);
    else {
        glBindTexture(GL_TEXTURE_2D, f.alias_base_color);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    const unsigned bytes = 1u << (unsigned(f.size) - 1);
    hidden_pixels_.assign(std::size_t(f.native_width) * f.native_height * 4, 0);
    for (std::size_t i = 0; i < hidden_pixels_.size() / 4; ++i) {
        const auto address = f.address + std::uint32_t(i * bytes);
        hidden_pixels_[i * 4] = memory.read_hidden16(address & ~1u);
        if (bytes == 4) hidden_pixels_[i * 4 + 1] = memory.read_hidden16((address + 2) & ~1u);
    }
    if (!f.alias_hidden) alias_detail::texture(f.alias_hidden, f.native_width, f.native_height, true, hidden_pixels_.data());
    else {
        glBindTexture(GL_TEXTURE_2D, f.alias_hidden);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.native_width, f.native_height,
                        GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, hidden_pixels_.data());
    }
}
void FramebufferAlias::snapshot(GLuint input, GLuint &output, int width, int height, bool integer) {
    if (!output) alias_detail::texture(output, width, height, integer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers_[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, input, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers_[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    const GLenum draw = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &draw);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
void FramebufferAlias::copy(const GLFramebuffer &from, GLFramebuffer &to, const N64Memory &memory, bool normalize_destination) {
    GLRDP_PROFILE_SCOPE(gpu_alias);
    ++to.texture_version;
    const unsigned pixel_bytes = 1u << (unsigned(from.size) - 1);
    mask_pixels_.assign(std::size_t(from.native_width) * from.native_height, 0);
    const bool bounded = from.dirty_x1 > from.dirty_x0 && from.dirty_y1 > from.dirty_y0;
    const int x0 = bounded ? std::clamp(from.dirty_x0, 0, from.native_width) : 0;
    const int y0 = bounded ? std::clamp(from.dirty_y0, 0, from.native_height) : 0;
    const int x1 = bounded ? std::clamp(from.dirty_x1, x0, from.native_width) : from.native_width;
    const int y1 = bounded ? std::clamp(from.dirty_y1, y0, from.native_height) : from.native_height;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const auto pixel = std::size_t(y) * from.native_width + x, at = pixel * pixel_bytes;
            bool own = at + pixel_bytes <= from.rdram_snapshot.size();
            for (unsigned b = 0; own && b < pixel_bytes; ++b)
                own = !(at + b < from.cpu_written.size() && from.cpu_written[at + b]) &&
                    memory.read8(from.address + unsigned(at + b)) == from.rdram_snapshot[at + b];
            mask_pixels_[pixel] = own;
        }
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (!glrdp_gles3) { glDisable(GL_MULTISAMPLE); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); }
    glBindVertexArray(vao_); glActiveTexture(GL_TEXTURE0); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Retain scratch capacity across alternating camera layouts. Reallocating
    // these textures at every format switch can serialize the driver.
    auto capacity = [](int &width, int &height, int requested_width, int requested_height) {
        if (width >= requested_width && height >= requested_height) return false;
        int next_width = std::max(width, requested_width), next_height = std::max(height, requested_height);
        if (std::uint64_t(next_width) * next_height > 2 * std::max(std::uint64_t(width) * height,
                std::uint64_t(requested_width) * requested_height)) {
            next_width = requested_width; next_height = requested_height;
        }
        width = next_width; height = next_height; return true;
    };
    if (capacity(width_, height_, to.width, to.height))
        alias_detail::texture(previous_color_, width_, height_, false);
    if (capacity(native_width_, native_height_, to.native_width, to.native_height))
        alias_detail::texture(previous_hidden_, native_width_, native_height_, true);
    snapshot(to.color, previous_color_, to.width, to.height, false);
    snapshot(to.alias_hidden, previous_hidden_, to.native_width, to.native_height, true);
    glBindTexture(GL_TEXTURE_2D, mask_);
    if (capacity(mask_width_, mask_height_, from.native_width, from.native_height)) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, mask_width_, mask_height_, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, from.native_width, from.native_height, GL_RED_INTEGER,
                    GL_UNSIGNED_BYTE, mask_pixels_.data());
    const bool self = &from == &to;
    const GLuint textures[] = {self ? previous_color_ : from.color, from.alias_base_color, previous_color_,
                               self ? previous_hidden_ : from.alias_hidden, previous_hidden_, mask_, to.alias_base_color};
    for (unsigned unit = 0; unit < 7; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, textures[unit]);
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        glUseProgram(programs_[pass]); const auto &u = uniforms_[pass];
        glUniform4i(u[7], from.address, from.native_width, from.native_height, unsigned(from.size) - 1);
        glUniform4i(u[8], to.address, to.native_width, to.native_height, unsigned(to.size) - 1);
        glUniform4i(u[9], from.internal_scale, to.internal_scale, 0, 0);
        glUniform1i(u[10], normalize_destination);
        const bool bounded = to.dirty_x1 > to.dirty_x0 && to.dirty_y1 > to.dirty_y0;
        glUniform4i(u[11], bounded ? to.dirty_x0 : 0, bounded ? to.dirty_y0 : 0,
                    bounded ? to.dirty_x1 : to.native_width, bounded ? to.dirty_y1 : to.native_height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffers_[1]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pass ? to.alias_hidden : to.color, 0);
        const GLenum draw = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &draw);
        glViewport(0, 0, pass ? to.native_width : to.width, pass ? to.native_height : to.height);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    ++copies_;
}
void FramebufferAlias::finish(GLFramebuffer &f) {
    snapshot(f.color, f.alias_base_color, f.width, f.height, false);
}

// Preserve the exact CPU materialization rules while transferring Z ownership
// to a GPU color view. Untouched noncanonical Z words remain literal RAM bytes;
// only changed Z/DZ is compressed. This avoids a readback followed by an upload
// when a game samples or copies an allocation that was previously its Z image.
bool FramebufferManager::import_depth(const GPUDepthBuffer::Surface &surface) {
    if (!native_notifications_ || !aliases_.ready() || !memory_ || !surface.initialized ||
        !surface.gpu_dirty || surface.address & 1u ||
        std::uint64_t(surface.address) + std::uint64_t(surface.width) * surface.height * 2 > memory_->size())
        return false;
    GLRDP_PROFILE_SCOPE(gpu_alias);
    GLRDP_PROFILE_EVENT("depth_export", surface.address, surface.width, surface.height);
    const RDPImage image{RDPImageFormat::RGBA, RDPImageSize::Bits16,
                         std::uint16_t(surface.width), surface.address};
    // Z activation already synchronized older color owners. Replace its
    // shadow directly: alias synchronization here could call back into the
    // depth pool while this very surface is being exported.
    last_bound_ = &buffers_[image_key(image)];
    auto &target = bind_color_image(image, surface.height, surface.address, 1, false);
    if (!target.color || target.width != surface.width || target.height != surface.height) return false;
    if (!depth_export_program_) {
        static constexpr const char *fragment = R"(#version 330 core
uniform usampler2D depth_state;
uniform usampler2D base_state;
layout(location=0) out vec4 color;
uint compress_z(uint z, uint dz) {
    uint top = (z >> 11u) & 127u, value;
    if (top <= 63u) value = (z >> 4u) & 8188u;
    else if (top <= 95u) value = ((z >> 3u) & 8188u) | 8192u;
    else if (top <= 111u) value = ((z >> 2u) & 8188u) | 16384u;
    else if (top <= 119u) value = ((z >> 1u) & 8188u) | 24576u;
    else if (top <= 123u) value = (z & 8188u) | 32768u;
    else if (top <= 125u) value = ((z << 1u) & 8188u) | 40960u;
    else value = ((z << 2u) & 8188u) | (top == 126u ? 49152u : 57344u);
    return value | ((dz >> 2u) & 3u);
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint state = texelFetch(depth_state, p, 0).r;
    uvec4 base = texelFetch(base_state, p, 0);
    uint visible = base.b, hidden = base.a;
    if (((state ^ base.r) & 4194303u) != 0u) {
        uint dz = (state >> 18u) & 15u;
        visible = compress_z(state & 262143u, dz);
        uint changed = base.b ^ base.g;
        if ((changed & 65280u) != 0u) visible = (visible & 255u) | (base.b & 65280u);
        if ((changed & 255u) != 0u) visible = (visible & 65280u) | (base.b & 255u);
        hidden = dz & 3u;
    }
    uint coverage = ((visible & 1u) << 2u) | hidden;
    color = vec4(float(((visible >> 11u) & 31u) << 3u),
                 float(((visible >> 6u) & 31u) << 3u),
                 float(((visible >> 1u) & 31u) << 3u),
                 float(((coverage + 1u) * 255u + 4u) / 8u)) / 255.0;
}
)";
        const auto vs = alias_detail::shader(GL_VERTEX_SHADER, alias_detail::source(alias_detail::vertex));
        const auto fs = alias_detail::shader(GL_FRAGMENT_SHADER, alias_detail::source(fragment));
        depth_export_program_ = glCreateProgram();
        glAttachShader(depth_export_program_, vs); glAttachShader(depth_export_program_, fs);
        glLinkProgram(depth_export_program_); glDeleteShader(vs); glDeleteShader(fs);
        GLint ok = 0; glGetProgramiv(depth_export_program_, GL_LINK_STATUS, &ok);
        if (!ok) throw std::runtime_error("Depth framebuffer export program link failed");
        glUseProgram(depth_export_program_);
        glUniform1i(glGetUniformLocation(depth_export_program_, "depth_state"), 0);
        glUniform1i(glGetUniformLocation(depth_export_program_, "base_state"), 1);
        glGenVertexArrays(1, &depth_export_vao_); glGenTextures(1, &depth_export_base_);
    }
    const auto pixels = std::size_t(surface.width) * surface.height;
    depth_export_pixels_.resize(pixels * 4);
    for (int y = 0; y < surface.height; ++y)
        for (int x = 0; x < surface.width; ++x) {
            const auto n = std::size_t(y) * surface.width + x;
            const auto gl = std::size_t(surface.height - 1 - y) * surface.width + x;
            depth_export_pixels_[gl * 4] = surface.visible_state[gl];
            depth_export_pixels_[gl * 4 + 1] = surface.visible_words[n];
            depth_export_pixels_[gl * 4 + 2] = memory_->read16(surface.address + unsigned(n * 2));
            depth_export_pixels_[gl * 4 + 3] = memory_->read_hidden16(surface.address + unsigned(n * 2));
        }
    if (!target.alias_base_color || !target.alias_hidden) upload_from_rdram(target, *memory_);
    target.rdram_snapshot.resize(pixels * 2);
    memory_->copy_be(target.address, target.rdram_snapshot.data(), target.rdram_snapshot.size());
    target.cpu_written.assign(pixels * 2, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, depth_export_base_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (depth_export_width_ != surface.width || depth_export_height_ != surface.height) {
        depth_export_width_ = surface.width; depth_export_height_ = surface.height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, surface.width, surface.height, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.width, surface.height,
                    GL_RGBA_INTEGER, GL_UNSIGNED_INT, depth_export_pixels_.data());
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, surface.state_texture[surface.active]);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    const GLenum draw = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &draw);
    glViewport(0, 0, surface.width, surface.height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (!glrdp_gles3) { glDisable(GL_MULTISAMPLE); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); }
    glUseProgram(depth_export_program_); glBindVertexArray(depth_export_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    target.cpu_dirty = target.raw_z = false;
    target.clear_gpu_written(); target.mark_gpu_written(0, 0, surface.width, surface.height);
    aliases_.finish(target);
    target.alias_serial = target.serial;
    for (auto &[_, other] : buffers_)
        if (&other != &target && other.raw_z && other.address < std::uint64_t(target.address) + pixels * 2 &&
            target.address < std::uint64_t(other.address) +
                framebuffer_byte_count(other.native_width, other.native_height, other.size)) {
            // Old clear mirrors no longer own these words. A later bind must
            // import the exported color alias rather than stale physical RAM.
            other.raw_z = false; other.cpu_dirty = true; other.clear_gpu_written();
        }
    return true;
}


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
    for (auto &surface : spare_surfaces_) destroy_surface(surface);
    spare_surfaces_.clear();
    spare_pixels_ = 0;
}

void GPUDepthBuffer::invalidate_range(std::uint32_t address, std::uint32_t size) {
    const std::uint64_t begin = address;
    const std::uint64_t end = begin + std::max<std::uint32_t>(size, 1u);
    for (auto it = surfaces_.begin(); it != surfaces_.end();) {
        const std::uint64_t surface_begin = it->first;
        const std::uint64_t surface_end = surface_begin + surface_bytes(it->second);
        if (begin < surface_end && end > surface_begin) {
            recycle_surface(std::move(it->second));
            it = surfaces_.erase(it);
        } else {
            ++it;
        }
    }
}

void GPUDepthBuffer::discard_overwritten(std::uint32_t address, int width, int height) {
    const auto end = std::uint64_t(address) + std::uint64_t(width) * height * 2;
    for (auto &[base, surface] : surfaces_)
        if (base >= address && std::uint64_t(base) + surface_bytes(surface) <= end) {
            // Repeated clears of the same GPU image share the original RAM
            // baseline until materialization. Rebuilding it per clear is both
            // unnecessary and expensive in games issuing several clears per VI.
            if (base != address || surface.width != width || surface.height != height)
                surface.gpu_dirty = surface.initialized = false;
            surface.changed_x = surface.changed_y = surface.changed_w = surface.changed_h = 0;
        }
}

void GPUDepthBuffer::recycle_surface(Surface &&surface) {
    const auto pixels = std::size_t(surface.width) * surface.height;
    // Cache allocations, never ownership or contents. Reimport on reuse.
    // Keep the existing limit of two allocations / about 6 MiB.
    if (surface.fbo[0] && spare_surfaces_.size() < 2 && pixels <= 262144 - spare_pixels_) {
        spare_pixels_ += pixels;
        spare_surfaces_.push_back(std::move(surface));
    } else destroy_surface(surface);
}

bool GPUDepthBuffer::allocate_surface(Surface &surface, std::uint32_t address, int width,
                                      int height) {
    GLRDP_PROFILE_SCOPE(depth_alloc);
    GLRDP_PROFILE_EVENT("depth_alloc", address, width, height);
    Surface previous = std::move(surface);
    surface = {};
    // Take the requested size before recycling the old size. Otherwise a full
    // cache can discard the texture needed by the next camera change.
    for (auto it = spare_surfaces_.begin(); it != spare_surfaces_.end(); ++it) {
        if (it->width != width || it->height != height) continue;
        surface = std::move(*it);
        spare_pixels_ -= std::size_t(width) * height;
        spare_surfaces_.erase(it);
        break;
    }
    if (previous.fbo[0]) recycle_surface(std::move(previous));
    surface.address = address;
    surface.width = width;
    surface.height = height;
    surface.active = 0;
    if (surface.fbo[0]) {
        surface.changed_x = surface.changed_y = surface.changed_w = surface.changed_h = 0;
        surface.initialized = surface.gpu_dirty = surface.in_place = false;
        for (auto fbo : surface.fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, glrdp_combined_attachment(), GL_TEXTURE_2D, 0, 0);
            const GLenum buffers[4] = {GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
            const GLenum desktop[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glDrawBuffers(glrdp_gles3 ? 4 : 2, glrdp_gles3 ? buffers : desktop);
        }
        return true;
    }

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
        glTexImage2D(GL_TEXTURE_2D, 0, (glrdp_gles3 ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24), width, height, 0, GL_DEPTH_COMPONENT,
                     GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, surface.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, glrdp_depth_attachment(), GL_TEXTURE_2D,
                               surface.state_texture[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, glrdp_acceptance_attachment(), GL_TEXTURE_2D,
                               surface.acceptance_texture[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               surface.linear_depth_texture[i], 0);
        const GLenum draw_buffers[4] = {GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        const GLenum desktop_buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(glrdp_gles3 ? 4 : 2, glrdp_gles3 ? draw_buffers : desktop_buffers);
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
    surface.visible_words.assign(pixels, 0);
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

void GPUDepthBuffer::import_from_rdram(Surface &surface, const N64Memory &memory, bool upload) {
    GLRDP_PROFILE_SCOPE(depth_import);
    GLRDP_PROFILE_EVENT("depth_import", surface.address, surface.width, surface.height);
    const std::size_t pixels = std::size_t(surface.width) * std::size_t(surface.height);
    std::vector<std::uint32_t> states(pixels, kDefaultState);
    surface.visible_words.resize(pixels);
    std::vector<float> linear(upload ? pixels : 0, 1.0f);

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
            if (upload) linear[gl_pixel] = float(z) / float(kMaxZ);
        }
    }

    std::vector<std::uint8_t> accept(upload ? pixels : 0, 0);
    for (int i = 0; upload && i < 2; ++i) {
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
    surface.visible_state = std::move(states);
    // A full raw clear immediately replaces every texel, including acceptance
    // and linear depth. Retain the CPU baseline without uploading six images
    // or copying old ping-pong contents that cannot survive that clear.
    if (!upload) surface.changed_x = surface.changed_y = surface.changed_w = surface.changed_h = 0;
    surface.gpu_dirty = false;
    surface.initialized = true;
}

GPUDepthBuffer::Surface *GPUDepthBuffer::ensure(std::uint32_t address, int width, int height,
                                                N64Memory *memory, bool import_rdram, bool full_clear) {
    GLRDP_PROFILE_SCOPE(depth_ensure);
    if (!address || width <= 0 || height <= 0)
        return nullptr;
    if (memory && import_rdram) {
        const auto end = std::uint64_t(address) + std::uint64_t(width) * height * 2;
        // Keep disjoint Z allocations resident. A shifted origin can reuse
        // physical words of another pending surface and must import them.
        for (auto &[other_address, other] : surfaces_)
            if (other_address != address && other.gpu_dirty &&
                address < std::uint64_t(other_address) + surface_bytes(other) && other_address < end)
                store_to_rdram(other_address, *memory);
    }
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
            import_from_rdram(surface, *memory, !full_clear);
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

void GPUDepthBuffer::framebuffer_pages(std::uint8_t *pages, unsigned count) const {
    for (const auto &[_, surface] : surfaces_)
        if (surface.initialized && surface.fbo[0])
            core_framebuffer_mark_pages(pages, count, surface.address, surface_bytes(surface),
                surface.gpu_dirty ? CORE_FB_PAGE_READ | CORE_FB_PAGE_WRITE : CORE_FB_PAGE_INVALIDATE);
}

bool GPUDepthBuffer::begin_pass(Surface &surface, int x, int y, int width, int height,
                                bool in_place, bool combined_color) {
    if (!surface.initialized || !surface.fbo[0] || !surface.fbo[1])
        return false;
    const int src = surface.active;
    surface.in_place = in_place && glrdp_texture_barrier;
    if (surface.in_place) {
        glrdp_texture_barrier();
        glBindFramebuffer(GL_FRAMEBUFFER, surface.fbo[src]);
        // The inactive image can lag behind several in-place draws. Keep their
        // union so a later upscaled/separate pass restores the ping-pong invariant.
        const int x0 = surface.changed_w ? std::min(x, surface.changed_x) : x;
        const int y0 = surface.changed_h ? std::min(y, surface.changed_y) : y;
        const int x1 = std::max(x + width, surface.changed_x + surface.changed_w);
        const int y1 = std::max(y + height, surface.changed_y + surface.changed_h);
        surface.changed_x = x0;
        surface.changed_y = y0;
        surface.changed_w = x1 - x0;
        surface.changed_h = y1 - y0;
        const GLenum shader_buffers[4] = {GL_NONE, GL_NONE, glrdp_depth_attachment(),
                                          glrdp_acceptance_attachment()};
        if (!combined_color) glDrawBuffers(4, shader_buffers);
        return true;
    }
    const int dst = src ^ 1;

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, surface.fbo[src]);
    glReadBuffer(glrdp_depth_attachment());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, surface.fbo[dst]);
    glrdp_draw_buffer(glrdp_depth_attachment());
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
    // Combined draws consume acceptance directly in the same invocation. They
    // never sample the mask, so rejected pixels need no acceptance clear. The
    // caller sets the combined MRT layout once after attaching its color image.
    // Separate/upscaled passes still clear before exposing this mask to color.
    if (!combined_color) {
        const GLenum reset_buffers[4] = {GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        const GLenum desktop_buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(glrdp_gles3 ? 4 : 2, glrdp_gles3 ? reset_buffers : desktop_buffers);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, width, height);
        const GLuint zero[4] = {0, 0, 0, 0};
        glClearBufferuiv(GL_COLOR, glrdp_gles3 ? 3 : 1, zero);

        const GLenum shader_buffers[4] = {GL_NONE, GL_NONE, glrdp_depth_attachment(), glrdp_acceptance_attachment()};
        glDrawBuffers(4, shader_buffers);
    }
    return true;
}

void GPUDepthBuffer::finish_pass(Surface &surface, bool writes_z) {
    if (!surface.in_place)
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

bool GPUDepthBuffer::has_pending_read(std::uint32_t address, std::uint32_t size) const {
    for (const auto &[base, surface] : surfaces_)
        if (surface.initialized && surface.gpu_dirty && base < std::uint64_t(address) + size &&
            std::uint64_t(address) < std::uint64_t(base) + surface_bytes(surface))
            return true;
    return false;
}

bool GPUDepthBuffer::has_surface_range(std::uint32_t address, std::uint32_t size) const {
    const auto end = std::uint64_t(address) + size;
    for (const auto &[base, surface] : surfaces_)
        if (std::uint64_t(address) < std::uint64_t(base) + surface_bytes(surface) && base < end)
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

bool GPUDepthBuffer::export_color_range(std::uint32_t address, std::uint32_t size, FramebufferManager &fb) {
    bool exported = false;
    for (auto &[base, surface] : surfaces_)
        if (surface.initialized && surface.gpu_dirty && base < std::uint64_t(address) + size &&
            address < std::uint64_t(base) + surface_bytes(surface) && fb.import_depth(surface)) {
            surface.gpu_dirty = surface.initialized = false;
            for (auto &[other_address, other] : surfaces_)
                if (&other != &surface && !other.gpu_dirty && other_address < std::uint64_t(base) + surface_bytes(surface) &&
                    base < std::uint64_t(other_address) + surface_bytes(other))
                    other.initialized = false;
            exported = true;
        }
    return exported;
}

void GPUDepthBuffer::store_to_rdram(std::uint32_t address, N64Memory &memory,
                                    std::uint32_t protected_address, std::uint32_t protected_size,
                                    bool protect_hidden) {
    GLRDP_PROFILE_SCOPE(depth_read);
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
    GLRDP_PROFILE_EVENT("depth_read", base_address, surface->width, surface->height);

    std::vector<std::uint32_t> states(pixels, kDefaultState);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, surface->fbo[surface->active]);
    glReadBuffer(glrdp_depth_attachment());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (glrdp_gles3) {
        // RGBA_INTEGER/UNSIGNED_INT is the portable ES3 integer readback pair.
        std::vector<std::uint32_t> rgba(pixels * 4);
        glReadPixels(0, 0, surface->width, surface->height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, rgba.data());
        for (std::size_t i = 0; i < pixels; ++i) states[i] = rgba[i * 4];
    } else {
        glReadPixels(0, 0, surface->width, surface->height, GL_RED_INTEGER, GL_UNSIGNED_INT, states.data());
    }

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
    for (auto &[other_address, other] : surfaces_)
        if (&other != surface && !other.gpu_dirty &&
            base_address < std::uint64_t(other_address) + surface_bytes(other) &&
            other_address < std::uint64_t(base_address) + pixels * 2)
            other.initialized = false;
}


namespace tmem_detail {
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
    load_program_ = tmem_detail::program(tmem_detail::load_fragment);
    decode_program_ = tmem_detail::program(tmem_detail::decode_fragment);
    const char *load_names[] = {"previous_tmem", "byte_map", "framebuffer", "source_layout", "render_layout"};
    const char *decode_names[] = {"raw_tmem", "tile", "mode", "storage", "region_origin"};
    for (unsigned i = 0; i < 5; ++i) {
        load_uniforms_[i] = glGetUniformLocation(load_program_, load_names[i]);
        decode_uniforms_[i] = glGetUniformLocation(decode_program_, decode_names[i]);
    }
    // Sampler assignments are invariant for the lifetime of these programs.
    glUseProgram(load_program_);
    for (unsigned i = 0; i < 3; ++i) glUniform1i(load_uniforms_[i], i);
    glUseProgram(decode_program_); glUniform1i(decode_uniforms_[0], 0);
    glGenVertexArrays(1, &vao_); glGenFramebuffers(1, &decode_fbo_);
    glActiveTexture(GL_TEXTURE0);
    tmem_detail::texture(map_texture_, GL_RGBA32UI, GL_UNSIGNED_INT);
    tmem_detail::texture(cpu_texture_, GL_RGBA8UI, GL_UNSIGNED_BYTE);
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
    tmem_detail::texture(snapshot->texture, GL_RGBA8UI, GL_UNSIGNED_BYTE);
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
    GLRDP_PROFILE_SCOPE(gpu_load);
    if (state.gpu_tmem && state.gpu_tmem->last_load_valid && (!source || source->serial) &&
        state.gpu_tmem->last_load_source == (source ? source->color : 0) &&
        state.gpu_tmem->last_load_serial == (source ? source->serial : 0) &&
        state.gpu_tmem->last_load_version == (source ? source->texture_version : 0) &&
        state.gpu_tmem->last_load_map == map)
        return;
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
    next->last_load_map = map;
    next->last_load_source = source ? source->color : 0;
    next->last_load_serial = source ? source->serial : 0;
    next->last_load_version = source ? source->texture_version : 0;
    next->last_load_valid = !source || source->serial;
    pipeline(load_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous ? previous->texture : cpu_texture_);
    if (!previous)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, state.tmem.data());
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, map_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, map.data());
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, source ? source->color : fallback_color_);
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
    GLRDP_PROFILE_SCOPE(gpu_decode);
    if (!state.gpu_tmem || !state.gpu_tmem->texture) throw std::runtime_error("Missing GPU TMEM snapshot");
    pipeline(decode_program_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, state.gpu_tmem->texture);
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
