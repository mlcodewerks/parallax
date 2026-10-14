#pragma once

#include "gl_loader.hpp"

#include <array>
#include <vector>

class GLStateGuard {
  public:
    GLStateGuard();
    ~GLStateGuard();

    GLStateGuard(const GLStateGuard &) = delete;
    GLStateGuard &operator=(const GLStateGuard &) = delete;

  private:
    bool nested_ = false;
    struct TextureUnitState {
        GLint texture_2d = 0;
        GLint texture_2d_multisample = 0;
        GLint sampler = 0;
    };

    GLint program_ = 0;
    GLint vao_ = 0;
    GLint array_buffer_ = 0;
    GLint element_array_buffer_ = 0;
    GLint pixel_pack_buffer_ = 0;
    GLint pixel_unpack_buffer_ = 0;

    GLint draw_framebuffer_ = 0;
    GLint read_framebuffer_ = 0;
    GLint read_buffer_ = GL_BACK;
    std::vector<GLenum> draw_buffers_;

    GLint active_texture_ = GL_TEXTURE0;
    std::array<TextureUnitState, 12> texture_units_{};

    std::array<GLint, 4> viewport_{};
    std::array<GLint, 4> scissor_box_{};
    std::array<GLint, 2> polygon_mode_{};
    std::array<GLboolean, 4> color_mask_{};
    std::array<GLfloat, 4> blend_color_{};
    std::array<GLfloat, 4> clear_color_{};
    GLfloat clear_depth_ = 1.0f;

    GLboolean blend_enabled_ = GL_FALSE;
    GLboolean depth_test_enabled_ = GL_FALSE;
    GLboolean scissor_test_enabled_ = GL_FALSE;
    GLboolean multisample_enabled_ = GL_FALSE;
    GLboolean cull_face_enabled_ = GL_FALSE;
    GLboolean stencil_test_enabled_ = GL_FALSE;
    GLboolean dither_enabled_ = GL_FALSE;
    GLboolean color_logic_op_enabled_ = GL_FALSE;
    GLboolean framebuffer_srgb_enabled_ = GL_FALSE;
    GLboolean sample_alpha_to_coverage_enabled_ = GL_FALSE;
    GLboolean sample_coverage_enabled_ = GL_FALSE;
    GLboolean rasterizer_discard_enabled_ = GL_FALSE;
    GLboolean polygon_offset_fill_enabled_ = GL_FALSE;
    GLboolean depth_mask_ = GL_TRUE;

    GLint depth_func_ = GL_LESS;
    GLint blend_src_rgb_ = GL_ONE;
    GLint blend_dst_rgb_ = GL_ZERO;
    GLint blend_src_alpha_ = GL_ONE;
    GLint blend_dst_alpha_ = GL_ZERO;
    GLint blend_equation_rgb_ = GL_FUNC_ADD;
    GLint blend_equation_alpha_ = GL_FUNC_ADD;
    GLint pack_alignment_ = 4;
    GLint unpack_alignment_ = 4;
};
