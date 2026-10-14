#include "gl_state_guard.hpp"

#include <algorithm>

namespace {
thread_local bool guard_active = false;
void restore_enable(GLenum capability, GLboolean enabled) {
    if (enabled)
        glEnable(capability);
    else
        glDisable(capability);
}
} // namespace

GLStateGuard::GLStateGuard() {
    nested_ = guard_active;
    if (nested_)
        return;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer_);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_array_buffer_);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer_);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pixel_unpack_buffer_);

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer_);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer_);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer_);

    GLint max_draw_buffers = 1;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
    max_draw_buffers = std::clamp(max_draw_buffers, 1, 16);
    draw_buffers_.resize(static_cast<std::size_t>(max_draw_buffers));
    for (GLint i = 0; i < max_draw_buffers; ++i) {
        GLint value = GL_NONE;
        glGetIntegerv(GL_DRAW_BUFFER0 + i, &value);
        draw_buffers_[static_cast<std::size_t>(i)] = GLenum(value);
    }

    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture_);
    for (unsigned unit = 0; unit < texture_units_.size(); ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_units_[unit].texture_2d);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE,
                      &texture_units_[unit].texture_2d_multisample);
        glGetIntegerv(GL_SAMPLER_BINDING, &texture_units_[unit].sampler);
    }
    glActiveTexture(GLenum(active_texture_));

    glGetIntegerv(GL_VIEWPORT, viewport_.data());
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box_.data());
    glGetIntegerv(GL_POLYGON_MODE, polygon_mode_.data());
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_.data());
    glGetFloatv(GL_BLEND_COLOR, blend_color_.data());
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color_.data());
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth_);

    blend_enabled_ = glIsEnabled(GL_BLEND);
    depth_test_enabled_ = glIsEnabled(GL_DEPTH_TEST);
    scissor_test_enabled_ = glIsEnabled(GL_SCISSOR_TEST);
    multisample_enabled_ = glIsEnabled(GL_MULTISAMPLE);
    cull_face_enabled_ = glIsEnabled(GL_CULL_FACE);
    stencil_test_enabled_ = glIsEnabled(GL_STENCIL_TEST);
    dither_enabled_ = glIsEnabled(GL_DITHER);
    color_logic_op_enabled_ = glIsEnabled(GL_COLOR_LOGIC_OP);
    framebuffer_srgb_enabled_ = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    sample_alpha_to_coverage_enabled_ = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    sample_coverage_enabled_ = glIsEnabled(GL_SAMPLE_COVERAGE);
    rasterizer_discard_enabled_ = glIsEnabled(GL_RASTERIZER_DISCARD);
    polygon_offset_fill_enabled_ = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask_);

    glGetIntegerv(GL_DEPTH_FUNC, &depth_func_);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb_);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb_);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha_);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha_);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb_);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha_);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment_);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment_);

    // Client pointers passed by this renderer must never be interpreted as PBO
    // byte offsets. Sampler objects are disabled on units owned temporarily by
    // the renderer so host sampler state cannot override plugin texture state.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (unsigned unit = 0; unit < texture_units_.size(); ++unit)
        glBindSampler(unit, 0);

    // Host-side fixed-function state can otherwise modify fragment writes even
    // when our own shader/FBO state is correct. Disable these while the guard
    // is active and restore them verbatim in the destructor.
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_COVERAGE);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_POLYGON_OFFSET_FILL);
    guard_active = true;
}

GLStateGuard::~GLStateGuard() {
    if (nested_)
        return;
    guard_active = false;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw_framebuffer_));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_framebuffer_));

    if (!draw_buffers_.empty()) {
        if (draw_framebuffer_ == 0)
            glDrawBuffer(draw_buffers_.front());
        else
            glDrawBuffers(GLsizei(draw_buffers_.size()), draw_buffers_.data());
    }
    glReadBuffer(GLenum(read_buffer_));

    for (unsigned unit = 0; unit < texture_units_.size(); ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, GLuint(texture_units_[unit].texture_2d));
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE,
                      GLuint(texture_units_[unit].texture_2d_multisample));
        glBindSampler(unit, GLuint(texture_units_[unit].sampler));
    }
    glActiveTexture(GLenum(active_texture_));

    glUseProgram(GLuint(program_));
    glBindVertexArray(GLuint(vao_));
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(array_buffer_));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GLuint(element_array_buffer_));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(pixel_pack_buffer_));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(pixel_unpack_buffer_));

    glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    glScissor(scissor_box_[0], scissor_box_[1], scissor_box_[2], scissor_box_[3]);
    glPolygonMode(GL_FRONT_AND_BACK, GLenum(polygon_mode_[0]));

    glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
    glDepthMask(depth_mask_);
    glDepthFunc(GLenum(depth_func_));
    glBlendColor(blend_color_[0], blend_color_[1], blend_color_[2], blend_color_[3]);
    glClearColor(clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]);
    glClearDepth(clear_depth_);
    glBlendFuncSeparate(GLenum(blend_src_rgb_), GLenum(blend_dst_rgb_), GLenum(blend_src_alpha_),
                        GLenum(blend_dst_alpha_));
    glBlendEquationSeparate(GLenum(blend_equation_rgb_), GLenum(blend_equation_alpha_));

    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment_);

    restore_enable(GL_BLEND, blend_enabled_);
    restore_enable(GL_DEPTH_TEST, depth_test_enabled_);
    restore_enable(GL_SCISSOR_TEST, scissor_test_enabled_);
    restore_enable(GL_MULTISAMPLE, multisample_enabled_);
    restore_enable(GL_CULL_FACE, cull_face_enabled_);
    restore_enable(GL_STENCIL_TEST, stencil_test_enabled_);
    restore_enable(GL_DITHER, dither_enabled_);
    restore_enable(GL_COLOR_LOGIC_OP, color_logic_op_enabled_);
    restore_enable(GL_FRAMEBUFFER_SRGB, framebuffer_srgb_enabled_);
    restore_enable(GL_SAMPLE_ALPHA_TO_COVERAGE, sample_alpha_to_coverage_enabled_);
    restore_enable(GL_SAMPLE_COVERAGE, sample_coverage_enabled_);
    restore_enable(GL_RASTERIZER_DISCARD, rasterizer_discard_enabled_);
    restore_enable(GL_POLYGON_OFFSET_FILL, polygon_offset_fill_enabled_);
}
