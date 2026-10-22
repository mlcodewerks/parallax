#pragma once
#include "gl_loader.hpp"
#include "n64_memory.hpp"
#include "rdp_types.hpp"
#include <algorithm>
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
    // Native pixel bounds of pending color writes. Empty bounds with gpu_dirty
    // set mean the whole image (including callers which invalidate manually).
    int dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = 0, dirty_y1 = 0;
    void clear_gpu_written() {
        gpu_dirty = false;
        dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
    }
    void mark_gpu_written(int x, int y, int w, int h) {
        if (!gpu_dirty) {
            dirty_x0 = x; dirty_y0 = y; dirty_x1 = x + w; dirty_y1 = y + h;
        } else if (dirty_x1 > dirty_x0 && dirty_y1 > dirty_y0) {
            dirty_x0 = std::min(dirty_x0, x); dirty_y0 = std::min(dirty_y0, y);
            dirty_x1 = std::max(dirty_x1, x + w); dirty_y1 = std::max(dirty_y1, y + h);
        }
        gpu_dirty = true;
    }
    bool gpu_pending_range(std::uint32_t address, std::uint32_t bytes) const;
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
                                    int internal_scale, bool bind_gl = true);
    int drawing_height(const RDPImage &, int scissor_height) const;
    GLFramebuffer *find(std::uint32_t);
    GLFramebuffer *find_containing(std::uint32_t, std::uint32_t span = 1);
    GLFramebuffer *find_overlapping(std::uint32_t, std::uint32_t);
    void upload_from_rdram(GLFramebuffer &, const N64Memory &);
    void download_to_rdram(GLFramebuffer &, N64Memory &);
    void commit_color_range(std::uint32_t address, std::uint32_t size, bool texture_read = false);
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
    std::vector<std::uint8_t> readback_pixels_;
    bool cpu_eligible(const GLFramebuffer &) const;
    void synchronize_cpu_tracking(GLFramebuffer &);
    void allocate_color(GLFramebuffer &, int, int);
};
