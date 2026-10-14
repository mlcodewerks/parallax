#pragma once
#include "gl_loader.hpp"
#include <array>

// GLES2 has neither VAOs nor separate read/draw framebuffer bindings.
class ES2Guard {
    inline static thread_local bool active_guard_ = false;
    bool nested_;
    GLint program_, fbo_, active_, buffer_, pack_, unpack_, viewport_[4], scissor_[4];
    GLint size_, stride_, type_, normalized_, attrib_buffer_, enabled_;
    void *pointer_;
    std::array<GLint, 8> textures_{};
    GLboolean color_[4], caps_[9];
    GLfloat clear_[4];
    inline static constexpr GLenum capabilities_[9] = {GL_BLEND,
                                                       GL_DEPTH_TEST,
                                                       GL_SCISSOR_TEST,
                                                       GL_CULL_FACE,
                                                       GL_STENCIL_TEST,
                                                       GL_DITHER,
                                                       GL_SAMPLE_ALPHA_TO_COVERAGE,
                                                       GL_SAMPLE_COVERAGE,
                                                       GL_POLYGON_OFFSET_FILL};

  public:
    ES2Guard() : nested_(active_guard_) {
        if (nested_)
            return;
        active_guard_ = true;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_);
        for (unsigned i = 0; i < 8; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures_[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer_);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_);
        glGetIntegerv(GL_VIEWPORT, viewport_);
        glGetIntegerv(GL_SCISSOR_BOX, scissor_);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size_);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride_);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type_);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized_);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attrib_buffer_);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled_);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer_);
        for (unsigned i = 0; i < 9; ++i) {
            caps_[i] = glIsEnabled(capabilities_[i]);
            glDisable(capabilities_[i]);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    ~ES2Guard() {
        if (nested_)
            return;
        glBindBuffer(GL_ARRAY_BUFFER, attrib_buffer_);
        glVertexAttribPointer(0, size_, type_, normalized_, stride_, pointer_);
        if (enabled_)
            glEnableVertexAttribArray(0);
        else
            glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        for (unsigned i = 0; i < 8; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures_[i]);
        }
        glActiveTexture(active_);
        glUseProgram(program_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        glPixelStorei(GL_PACK_ALIGNMENT, pack_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_);
        glColorMask(color_[0], color_[1], color_[2], color_[3]);
        glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
        for (unsigned i = 0; i < 9; ++i) {
            if (caps_[i])
                glEnable(capabilities_[i]);
            else
                glDisable(capabilities_[i]);
        }
        active_guard_ = false;
    }
};
