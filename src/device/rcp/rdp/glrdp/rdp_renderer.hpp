#pragma once
#include "shader_config.hpp"
#include "primitive_batch.hpp"
#include "primitive_footprint.hpp"
#include "primitive_scheduler.hpp"
#include "texture_atlas.hpp"
#include "framebuffer_manager.hpp"
#include "gl_loader.hpp"
#include "n64_memory.hpp"
#include "rdp_types.hpp"
#include "rdp_vertex.hpp"
#include "gpu_depth.hpp"
#include "rdp_accuracy.h"
#include "vi_field.hpp"
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
class RDPRenderer {
  public:
    bool initialize(N64Memory *);
    void shutdown();
    void reset();
    void set_output_size(int, int);
    void set_output_framebuffer(GLuint fbo) {
        output_fbo_ = fbo;
    }
    void set_internal_resolution_scale(int scale);
    void set_rendering_config(GLRDPConfig config) {
        flush();
        config_ = glrdp_validate_config(config);
        program_ = specialized_program_ && config_.specialize_shaders &&
                   glrdp_shader_config_key(config_) == specialized_key_
                       ? specialized_program_ : generic_program_;
    }
    void flush();
    struct SubmissionStats {
        std::uint64_t primitives = 0, draws = 0, batches = 0, batched_primitives = 0;
        std::uint64_t atlas_hits = 0, atlas_uploads = 0, atlas_resets = 0, standalone_uploads = 0;
        std::uint64_t framebuffer_tiles = 0, framebuffer_readbacks = 0;
        std::uint64_t queues = 0, queue_flushes = 0, state_flushes = 0;
    };
    const SubmissionStats &submission_stats() const { return stats_; }
    unsigned batch_capacity() const { return primitive_uniforms_.capacity; }
    void set_display_resolution(int width, int height);
    void set_display_aspect_mode(int mode);
    int internal_resolution_scale() const {
        return internal_scale_;
    }
    void draw_triangle(const RDPTriangle &, const RDPState &);
    void draw_texture_rectangle(const RDPTextureRectangle &, const RDPState &);
    void draw_fill_rectangle(const RDPFillRectangle &, const RDPState &);
    void present(const RDPState &, std::uint32_t vi_status, std::uint32_t origin,
                 std::uint32_t width, std::uint32_t hstart, std::uint32_t vstart,
                 std::uint32_t xscale, std::uint32_t yscale, std::uint32_t vsync = 525,
                 std::uint32_t current = UINT32_MAX);
    void fb_read(std::uint32_t);
    void fb_write(std::uint32_t, std::uint32_t);
    void read_screen(void *, int *, int *);
    void capture_bmp(const char *);
    FramebufferManager &framebuffers() {
        flush();
        return fb_;
    }
    std::pair<int, int> depth_dimensions(std::uint32_t address) {
        const auto *surface = gpu_depth_.find(address);
        return surface ? std::pair{surface->width, surface->height} : std::pair{0, 0};
    }

  private:
    PrimitiveUniforms primitive_uniforms_;
    GLuint primitive_ubo_ = 0, atlas_texture_ = 0;
    std::size_t ubo_offset_ = 0, ubo_slot_bytes_ = 0;
    TextureAtlas atlas_;
    std::array<TextureAtlas::Region, 8> tile_regions_{};
    bool standalone_tiles_ = false, atlas_full_ = false;
    unsigned current_tile_count_ = 1;
    std::vector<PrimitiveUniforms::Slot> batch_records_;
    std::vector<PrimitiveBounds> batch_bounds_;
    std::vector<PrimitiveFootprint> batch_footprints_;
    PrimitiveScheduler batch_schedule_;
    RDPEdgeCoefficients triangle_edge_{};
    GLFramebuffer *batch_target_ = nullptr;
    GPUDepthBuffer::Surface *batch_depth_ = nullptr;
    GLuint batch_program_ = 0;
    bool batch_writes_z_ = false, batch_reads_color_ = false;
    SubmissionStats stats_;
    void primitive_geometry(const std::vector<GLVertex> &);
    void bind_primitive_pipeline(GLuint);
    void submit_records(const PrimitiveUniforms::Slot *, unsigned);
    void bind_tiles();
    void flush_batch(const std::vector<PrimitiveUniforms::Slot> &, const std::vector<PrimitiveBounds> &);
    void uniform_1i(GLint l, GLint x) { if (!primitive_uniforms_.store(l, &x, 4)) glUniform1i(l, x); }
    void uniform_1ui(GLint l, GLuint x) { if (!primitive_uniforms_.store(l, &x, 4)) glUniform1ui(l, x); }
    void uniform_1f(GLint l, GLfloat x) { if (!primitive_uniforms_.store(l, &x, 4)) glUniform1f(l, x); }
    void uniform_2f(GLint l, GLfloat x, GLfloat y) {
        const GLfloat v[] = {x,y}; if (!primitive_uniforms_.store(l, v, sizeof(v))) glUniform2f(l,x,y);
    }
    void uniform_4f(GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
        const GLfloat v[] = {x,y,z,w}; if (!primitive_uniforms_.store(l, v, sizeof(v))) glUniform4f(l,x,y,z,w);
    }
    void uniform_4i(GLint l, GLint x, GLint y, GLint z, GLint w) {
        const GLint v[] = {x,y,z,w}; if (!primitive_uniforms_.store(l, v, sizeof(v))) glUniform4i(l,x,y,z,w);
    }
    GLint uniform(GLuint program, const char *name);
    void initialize_samplers(GLuint program);
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> uniforms_;
    std::unordered_map<GLuint, std::array<std::uint64_t, 9>> combiner_values_;
    std::unordered_map<GLuint, std::array<std::uint64_t, 9>> configuration_values_;
    struct TileUniforms {
        std::array<float, 4> transform{};
        std::array<int, 4> wrap{};
        std::array<float, 4> clamp{};
        bool valid = false;
    };
    std::unordered_map<GLuint, std::array<TileUniforms, 8>> tile_uniforms_;
    N64Memory *memory_ = nullptr;
    GLuint output_fbo_ = 0;
    bool triangle_coverage_ = false;
    bool combined_depth_color_ = false;
    std::uint8_t max_mip_level_ = 0;
    int bounds_x_ = 0, bounds_y_ = 0, bounds_w_ = 0, bounds_h_ = 0;
    void primitive_scissor(const GLFramebuffer &target, const RDPState &state, int scale);
    FramebufferManager fb_;
    GPUDepthBuffer gpu_depth_;
    GLuint generic_program_ = 0, specialized_program_ = 0;
    std::uint32_t specialized_key_ = 0;
    GLuint program_ = 0, present_program_ = 0, final_present_program_ = 0, vao_ = 0, vbo_ = 0;
    GLRDPConfig config_ = glrdp_reference_config();
    VIFieldHistory field_history_;
    GLuint post_fbo_ = 0, post_color_ = 0, post_depth_ = 0;
    GLuint vi_fbo_ = 0, vi_color_ = 0;
    int vi_w_ = 0, vi_h_ = 0;
    GLuint current_depth_acceptance_texture_ = 0;
    GLuint default_depth_acceptance_texture_ = 0;
    int post_w_ = 0, post_h_ = 0;
    std::array<GLuint, 8> tile_texture_{};
    std::array<int, 8> tile_width_{}, tile_height_{};
    std::array<std::vector<std::uint8_t>, 8> tile_pixels_;
    std::array<std::uint64_t, 8> tile_signature_{};
    GLuint feedback_texture_ = 0;
    GLuint current_feedback_texture_ = 0;
    int feedback_w_ = 0, feedback_h_ = 0;
    int output_w_ = 640, output_h_ = 480, rdp_w_ = 320, rdp_h_ = 240;
    int max_texture_size_ = 2048;
    int internal_scale_ = 1;
    int display_w_ = 0, display_h_ = 0;
    int display_aspect_mode_ = 0; // 0 = source, 1 = 4:3 canvas, 2 = 16:9 canvas
    float content_aspect_ = 4.0f / 3.0f;
    RDPImage current_color_{};
    void upload_tile(const RDPState &, std::uint8_t, unsigned);
    void update_feedback(GLFramebuffer &, bool copy = true, bool defer_barrier = false);
    void set_combiner_uniforms(const RDPState &, GLuint);
    bool render_gpu_depth(const std::vector<GLVertex> &vertices, bool textured,
                          GLFramebuffer &target, const RDPState &state, bool texture_is_framebuffer,
                          int cycle_type, bool rectangle, bool raw_z_fill,
                          std::uint32_t primitive_noise_seed, GPUDepthBuffer::Surface &surface);
    void configure_rdp_program(GLuint active_program, const GLFramebuffer &target,
                               const RDPState &state, bool textured, bool texture_is_framebuffer,
                               int cycle_type, float fragment_internal_scale, bool depth_state_pass,
                               bool depth_acceptance_enable, bool raw_z_fill,
                               std::uint32_t primitive_noise_seed,
                               bool combined_depth_color = false);
    void issue_geometry(const std::vector<GLVertex> &vertices, bool rectangle);
    bool ensure_post_target(int, int);
    bool ensure_vi_target(int, int);
    void draw_final_present();
    static void fit_aspect(int outer_w, int outer_h, float aspect, int &x, int &y, int &w, int &h);
    void draw_vertices(const std::vector<GLVertex> &, bool, const RDPState &, std::uint8_t,
                       bool rectangle = false);
    std::uint32_t noise_seed_ = 1;
};
