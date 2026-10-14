#pragma once
#include "framebuffer_manager.hpp"
#include "rdp_tmem.hpp"
#include "rdp_accuracy.h"
#include <array>
#include <algorithm>
#include <string>
#include <unordered_map>

// Native ES 2.0 rasterization: four fragment samplers, eight varying vectors,
// RGBA/UNSIGNED_BYTE storage and one color attachment per drawing pass.
class GLES2Renderer {
  public:
    bool initialize(N64Memory *, bool force_mediump = false);
    void shutdown();
    ~GLES2Renderer() {
        shutdown();
    }
    void set_internal_resolution_scale(int scale) {
        scale_ = std::clamp(scale, 1, 8);
    }
    void set_rendering_config(GLRDPConfig config) {
        config_ = glrdp_validate_config(config);
    }
    void set_output_size(int w, int h) {
        output_w_ = w;
        output_h_ = h;
    }
    void set_output_framebuffer(GLuint fbo) {
        output_fbo_ = fbo;
    }
    void draw_triangle(const RDPTriangle &, const RDPState &);
    void draw_texture_rectangle(const RDPTextureRectangle &, const RDPState &);
    void draw_fill_rectangle(const RDPFillRectangle &, const RDPState &);
    void present(const RDPState &, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                 uint32_t, uint32_t = 525);
    void fb_read(uint32_t);
    void fb_write(uint32_t, uint32_t);
    FramebufferManager &framebuffers() {
        return fb_;
    }
    std::pair<int, int> depth_dimensions(uint32_t address) const {
        const auto found = depth_.find(address);
        return found != depth_.end() ? std::pair{found->second.w, found->second.h}
                                     : std::pair{0, 0};
    }
    bool high_precision() const {
        return high_precision_;
    }

  private:
    struct Planes {
        std::array<float, 4> q{0, 0, 1, 0}, qx{}, qy{}, shade{255, 255, 255, 255}, sx{}, sy{}, z{};
        std::array<std::int32_t, 12> texture_fixed{};
        std::array<std::int32_t, 2> texture_edge{};
        std::array<std::int32_t, 4> depth_fixed{};
        std::array<std::int32_t, 16> shade_fixed{};
        std::array<float, 3> span_z{}; // start, de, fractional dx
        std::array<std::array<float, 4>, 3> edges{};
        float ox = 0, oy = 0;
        bool textured = false, triangle = false, shaded = false;
        unsigned tile = 0, level = 0;
    };
    struct Depth {
        GLuint texture[2]{}, fbo[2]{};
        int w = 0, h = 0, current = 0;
        bool dirty = false;
        int damage_x = 0, damage_y = 0, damage_w = 0, damage_h = 0;
        std::vector<uint16_t> visible;
        std::vector<uint8_t> imported;
        std::vector<uint8_t> ram_snapshot;
    };
    N64Memory *memory_ = nullptr;
    FramebufferManager fb_;
    std::unordered_map<uint32_t, Depth> depth_;
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> uniforms_;
    std::unordered_map<GLuint, std::unordered_map<GLint, std::array<float, 4>>> draw_uniforms_;
    std::array<std::array<GLuint, 4>, 3> cycle_programs_{};
    GLRDPConfig config_ = glrdp_reference_config();
    std::string shader_header_;
    GLuint program_ = 0, present_program_ = 0, vbo_ = 0, atlas_ = 0, metadata_ = 0, feedback_ = 0,
           output_fbo_ = 0;
    GLuint vi_fbo_ = 0, vi_color_ = 0;
    int vi_w_ = 0, vi_h_ = 0;
    int scale_ = 1, output_w_ = 640, output_h_ = 480, max_texture_ = 2048, atlas_w_ = 0,
        atlas_h_ = 0, feedback_w_ = 0, feedback_h_ = 0;
    bool high_precision_ = false;
    uint64_t atlas_signature_ = ~uint64_t(0);
    std::array<uint8_t, 1024> meta_{};
    std::array<uint8_t, 1024> uploaded_meta_{};
    bool metadata_valid_ = false;
    std::array<uint8_t, 224> tile_meta_{};
    std::vector<uint8_t> atlas_pixels_;
    void number(unsigned, float);
    void bytes(unsigned, unsigned, unsigned, unsigned, unsigned);
    GLint uniform(GLuint, const char *);
    void uniform4(const char *, const std::array<float, 4> &);
    GLuint cycle_program(unsigned);
    void quad(float, float, float, float);
    bool ensure_vi_target(int, int);
    void atlas(const RDPState &, unsigned, unsigned);
    void draw(const RDPState &, const Planes &, float, float, float, float);
    Depth &depth(uint32_t, int, int);
    void store_depth(uint32_t, Depth &, uint32_t protected_address = 0, uint32_t protected_size = 0,
                     bool protect_hidden = false);
    void invalidate_depth(uint32_t, uint32_t);
};
