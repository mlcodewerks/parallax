#pragma once
#include "gl_loader.hpp"
#include "n64_memory.hpp"
#include "rdp_types.hpp"
#include <cstdint>
#include <functional>
#include <utility>
#include <unordered_map>
#include <vector>
struct GLFramebuffer {
    GLuint fbo = 0, color = 0;
    std::uint32_t address = 0, z_address = 0;
    int width = 0, height = 0;
    int native_width = 0, native_height = 0;
    int internal_scale = 1;
    RDPImageFormat format = RDPImageFormat::RGBA;
    RDPImageSize size = RDPImageSize::Bits16;
    std::uint64_t serial = 0;
    bool gpu_dirty = false, cpu_dirty = false;
    bool cpu_display = false;
    // Address equality alone does not identify depth: games copy color into
    // the same allocation and later sample it as a texture.
    bool raw_z = false;
    bool cpu_untracked = false;
    std::vector<std::uint8_t> rdram_snapshot;
    std::vector<std::uint8_t> cpu_written;
};
class FramebufferManager {
  public:
    static constexpr unsigned cpu_color_slots = 5;
    ~FramebufferManager();
    GLFramebuffer &bind_color_image(const RDPImage &, int, std::uint32_t z_address,
                                    int internal_scale);
    int drawing_height(const RDPImage &, int scissor_height) const;
    GLFramebuffer *find(std::uint32_t);
    GLFramebuffer *find_containing(std::uint32_t);
    GLFramebuffer *find_overlapping(std::uint32_t, std::uint32_t);
    void upload_from_rdram(GLFramebuffer &, const N64Memory &);
    void download_to_rdram(GLFramebuffer &, N64Memory &);
    void commit_color_range(std::uint32_t address, std::uint32_t size);
    void depth_written_to_ram(std::uint32_t address, std::uint32_t size);
    void record_fill32_hidden(const RDPFillRectangle &, const RDPState &);
    void set_readback_callback(std::function<void(std::uint32_t, std::uint32_t)> callback) {
        readback_callback_ = std::move(callback);
    }
    void invalidate_cpu_write(std::uint32_t, std::uint32_t);
    void clear();
    void set_es2(bool enabled) {
        es2_ = enabled;
    }
    void set_memory(N64Memory *memory) {
        memory_ = memory;
        last_bound_ = nullptr;
    }
    const auto &buffers() const {
        return buffers_;
    }

  private:
    std::unordered_map<std::uint64_t, GLFramebuffer> buffers_;
    std::uint64_t serial_counter_ = 0;
    int max_texture_size_ = 0;
    bool es2_ = false;
    N64Memory *memory_ = nullptr;
    GLFramebuffer *last_bound_ = nullptr;
    bool last_bound_eligible_ = false;
    std::function<void(std::uint32_t, std::uint32_t)> readback_callback_;
    bool cpu_eligible(const GLFramebuffer &) const;
    void synchronize_cpu_tracking(GLFramebuffer &);
    void allocate_color(GLFramebuffer &, int, int);
};
