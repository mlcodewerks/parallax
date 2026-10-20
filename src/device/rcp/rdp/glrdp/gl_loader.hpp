#pragma once
#include <glad.h>
#include <libretro.h>
// Resolve only through the frontend's current context.
bool load_gl33(retro_hw_get_proc_address_t resolver, bool gles3);
extern bool glrdp_gles3;
// GLES draw-buffer slots must match attachment indices.
inline GLenum glrdp_depth_attachment() { return glrdp_gles3 ? GL_COLOR_ATTACHMENT2 : GL_COLOR_ATTACHMENT0; }
inline GLenum glrdp_acceptance_attachment() { return glrdp_gles3 ? GL_COLOR_ATTACHMENT3 : GL_COLOR_ATTACHMENT1; }
inline GLenum glrdp_combined_attachment() { return glrdp_gles3 ? GL_COLOR_ATTACHMENT0 : GL_COLOR_ATTACHMENT2; }
inline void glrdp_draw_buffer(GLenum attachment) {
    if (!glrdp_gles3) { glDrawBuffer(attachment); return; }
    const unsigned count = attachment == GL_BACK || attachment == GL_NONE ? 1 : attachment - GL_COLOR_ATTACHMENT0 + 1;
    GLenum buffers[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    buffers[count - 1] = attachment;
    glDrawBuffers(count, buffers);
}
using GLRDPPrecisionQuery = void(APIENTRY *)(GLenum, GLenum, GLint *, GLint *);
extern GLRDPPrecisionQuery glrdp_precision_query;
using GLRDPTextureBarrier = void(APIENTRY *)(void);
extern GLRDPTextureBarrier glrdp_texture_barrier;
