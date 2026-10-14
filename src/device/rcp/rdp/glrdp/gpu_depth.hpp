#pragma once

#include "gl_loader.hpp"
#include "n64_memory.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

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
        std::vector<std::uint32_t> visible_state;
        std::vector<std::uint16_t> visible_words;
    };

    ~GPUDepthBuffer();

    void clear();
    void invalidate_range(std::uint32_t address, std::uint32_t size);

    Surface *ensure(std::uint32_t address, int width, int height, N64Memory *memory,
                    bool import_rdram);
    Surface *find(std::uint32_t address);
    const Surface *find(std::uint32_t address) const;
    bool has_pending_read(std::uint32_t address) const;
    bool flush_range(std::uint32_t address, std::uint32_t size, N64Memory &memory);

    bool begin_pass(Surface &surface, int x, int y, int width, int height);
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

    static constexpr std::uint32_t kMaxZ = 0x3ffffu;
    static constexpr std::uint32_t kDefaultState = kMaxZ | (15u << 18);

    static std::uint32_t z_decompress_visible(std::uint16_t value);
    static std::uint16_t z_compress_visible(std::uint32_t z, std::uint8_t dz_exponent);
    static std::uint32_t pack_state(std::uint32_t z, std::uint8_t dz_exponent,
                                    std::uint8_t coverage);

    void destroy_surface(Surface &surface);
    bool allocate_surface(Surface &surface, std::uint32_t address, int width, int height);
    void initialize_default(Surface &surface);
    void import_from_rdram(Surface &surface, const N64Memory &memory);
};
