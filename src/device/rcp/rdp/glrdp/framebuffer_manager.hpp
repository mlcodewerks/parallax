#pragma once
#include "gl_loader.hpp"
#include "n64_memory.hpp"
#include "rdp_types.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <utility>
#include <unordered_map>
#include <vector>
struct GLFramebuffer {
    GLuint fbo = 0, color = 0;
    GLuint alias_base_color = 0, alias_hidden = 0;
    std::uint64_t alias_serial = 0;
    std::uint32_t address = 0, z_address = 0;
    int width = 0, height = 0;
    int native_width = 0, native_height = 0;
    int internal_scale = 1;
    RDPImageFormat format = RDPImageFormat::RGBA;
    RDPImageSize size = RDPImageSize::Bits16;
    std::uint64_t serial = 0;
    std::uint64_t write_serial = 0;
    std::uint64_t texture_version = 0;
    bool gpu_dirty = false, cpu_dirty = false;
    // Native pixel bounds of pending color writes. Empty bounds with gpu_dirty
    // set mean the whole image (including callers which invalidate manually).
    int dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = 0, dirty_y1 = 0;
    void clear_gpu_written() {
        gpu_dirty = false;
        dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
    }
    void mark_gpu_written(int x, int y, int w, int h) {
        ++texture_version;
        write_serial = serial;
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
// Reinterpret overlapping color views using physical RDRAM byte addresses.
// The hidden plane records the coverage inherited when each view was imported.
class FramebufferAlias {
  public:
    void initialize();
    void clear();
    bool ready() const { return programs_[0] != 0; }
    void upload_base(GLFramebuffer &, const N64Memory &, const std::uint8_t *rgba);
    void copy(const GLFramebuffer &, GLFramebuffer &, const N64Memory &, bool normalize_destination = false);
    void finish(GLFramebuffer &);
    std::uint64_t copies() const { return copies_; }
  private:
    GLuint programs_[2]{}, vao_ = 0, framebuffers_[2]{};
    std::array<std::array<GLint, 12>, 2> uniforms_{};
    GLuint previous_color_ = 0, previous_hidden_ = 0, mask_ = 0;
    int width_ = 0, height_ = 0, native_width_ = 0, native_height_ = 0;
    int mask_width_ = 0, mask_height_ = 0;
    std::vector<std::uint8_t> mask_pixels_, hidden_pixels_;
    std::uint64_t copies_ = 0;
    void snapshot(GLuint, GLuint &, int, int, bool integer);
};

class FramebufferManager;
class GPUDepthBuffer {
  public:
    struct Surface {
        std::uint32_t address = 0;
        int width = 0;
        int height = 0;
        GLuint fbo[2]{};
        GLuint state_texture[2]{};
        GLuint acceptance_texture[2]{};
        GLuint linear_depth_texture[2]{};
        int active = 0;
        int changed_x = 0, changed_y = 0, changed_w = 0, changed_h = 0;
        bool initialized = false;
        bool gpu_dirty = false;
        bool in_place = false;
        std::vector<std::uint32_t> visible_state;
        std::vector<std::uint16_t> visible_words;
    };

    ~GPUDepthBuffer();

    void clear();
    void invalidate_range(std::uint32_t address, std::uint32_t size);

    // full_clear requires the caller's next pass to replace every pixel of
    // state, acceptance and linear depth before any consumer can sample it.
    Surface *ensure(std::uint32_t address, int width, int height, N64Memory *memory,
                    bool import_rdram, bool full_clear = false);
    Surface *find(std::uint32_t address);
    const Surface *find(std::uint32_t address) const;
    void framebuffer_pages(std::uint8_t *pages, unsigned count) const;
    bool has_pending_read(std::uint32_t address, std::uint32_t size = 1) const;
    bool has_surface_range(std::uint32_t address, std::uint32_t size) const;
    bool flush_range(std::uint32_t address, std::uint32_t size, N64Memory &memory);
    bool export_color_range(std::uint32_t address, std::uint32_t size, FramebufferManager &);
    void discard_overwritten(std::uint32_t address, int width, int height);

    bool begin_pass(Surface &surface, int x, int y, int width, int height,
                    bool in_place = false, bool combined_color = false);
    void finish_pass(Surface &surface, bool writes_z = true);

    GLuint source_state_texture(const Surface &surface) const;
    GLuint active_state_texture(const Surface &surface) const;
    GLuint active_acceptance_texture(const Surface &surface) const;
    GLuint active_linear_depth_texture(const Surface &surface) const;

    void store_to_rdram(std::uint32_t address, N64Memory &memory,
                        std::uint32_t protected_address = 0, std::uint32_t protected_size = 0,
                        bool protect_hidden = false);
    void flush_cpu_write(std::uint32_t address, std::uint32_t size, N64Memory &memory,
                         bool protect_hidden = false);

  private:
    std::unordered_map<std::uint32_t, Surface> surfaces_;
    // Allocation cache only: invalidated contents never retain ownership of RAM.
    std::vector<Surface> spare_surfaces_;
    std::size_t spare_pixels_ = 0;

    static constexpr std::uint32_t kMaxZ = 0x3ffffu;
    static constexpr std::uint32_t kDefaultState = kMaxZ | (15u << 18);

    static std::uint32_t z_decompress_visible(std::uint16_t value);
    static std::uint16_t z_compress_visible(std::uint32_t z, std::uint8_t dz_exponent);
    static std::uint32_t pack_state(std::uint32_t z, std::uint8_t dz_exponent,
                                    std::uint8_t coverage);

    void destroy_surface(Surface &surface);
    void recycle_surface(Surface &&surface);
    bool allocate_surface(Surface &surface, std::uint32_t address, int width, int height);
    void initialize_default(Surface &surface);
    void import_from_rdram(Surface &surface, const N64Memory &memory, bool upload = true);
};

class GPUTMEM {
  public:
    GPUTMEM() = default;
    ~GPUTMEM() { clear(); }
    GPUTMEM(const GPUTMEM &) = delete;
    GPUTMEM &operator=(const GPUTMEM &) = delete;
    void initialize();
    void clear();
    void release_snapshots();
    // Map entries are literals (0..255), GPU framebuffer byte offsets with
    // bit 31 set, or UINT32_MAX to retain the previous snapshot byte.
    void write(RDPState &, const std::array<std::uint32_t, 4096> &, const GLFramebuffer *);
    bool materialize(RDPState &);
    void decode(const RDPState &, unsigned tile, GLuint destination,
                const std::array<int, 4> &region);

  private:
    GLuint load_program_ = 0, decode_program_ = 0, vao_ = 0;
    GLuint map_texture_ = 0, cpu_texture_ = 0, fallback_color_ = 0, decode_fbo_ = 0;
    std::vector<std::shared_ptr<RDPTMEMBacking>> snapshots_;
    std::uint64_t serial_ = 0;
    std::array<GLint, 5> load_uniforms_{};
    std::array<GLint, 5> decode_uniforms_{};
    std::shared_ptr<RDPTMEMBacking> acquire(const RDPTMEMBacking *);
    void pipeline(GLuint);
};

class FramebufferManager {
  public:
    static constexpr unsigned cpu_color_slots = 5;
    ~FramebufferManager();
    GLFramebuffer &bind_color_image(const RDPImage &, int, std::uint32_t z_address,
                                    int internal_scale, bool bind_gl = true);
    int drawing_height(const RDPImage &, int scissor_height) const;
    GLFramebuffer *find(std::uint32_t);
    GLFramebuffer *find_image(const RDPImage &);
    GLFramebuffer *find_containing(std::uint32_t, std::uint32_t span = 1);
    GLFramebuffer *find_overlapping(std::uint32_t, std::uint32_t);
    void upload_from_rdram(GLFramebuffer &, const N64Memory &);
    void download_to_rdram(GLFramebuffer &, N64Memory &);
    void commit_color_range(std::uint32_t address, std::uint32_t size, bool texture_read = false);
    void depth_written_to_ram(std::uint32_t address, std::uint32_t size);
    bool import_depth(const GPUDepthBuffer::Surface &);
    void discard_overwritten(std::uint32_t address, std::uint32_t size);
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
    void set_native_notifications(bool enabled) { native_notifications_ = enabled; }
    bool native_notifications() const { return native_notifications_; }
    void initialize_aliases() { aliases_.initialize(); }
    void shutdown_aliases() { aliases_.clear(); }
    std::uint64_t alias_copies() const { return aliases_.copies(); }
    const auto &buffers() const {
        return buffers_;
    }

  private:
    std::unordered_map<std::uint64_t, GLFramebuffer> buffers_;
    std::uint64_t serial_counter_ = 0;
    int max_texture_size_ = 0;
    bool es2_ = false;
    bool native_notifications_ = false;
    FramebufferAlias aliases_;
    bool committing_aliases_ = false;
    N64Memory *memory_ = nullptr;
    GLFramebuffer *last_bound_ = nullptr;
    bool last_bound_eligible_ = false;
    std::function<void(std::uint32_t, std::uint32_t)> readback_callback_;
    std::vector<std::uint8_t> readback_pixels_;
    GLuint depth_export_program_ = 0, depth_export_vao_ = 0, depth_export_base_ = 0;
    int depth_export_width_ = 0, depth_export_height_ = 0;
    std::vector<std::uint32_t> depth_export_pixels_;
    bool cpu_eligible(const GLFramebuffer &) const;
    void synchronize_cpu_tracking(GLFramebuffer &);
    void allocate_color(GLFramebuffer &, int, int);
};
