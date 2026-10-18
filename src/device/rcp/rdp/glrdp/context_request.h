#ifndef GLRDP_CONTEXT_REQUEST_H
#define GLRDP_CONTEXT_REQUEST_H
#include <libretro.h>

/* Follow the frontend's loaded video driver. Older frontends can decline the
 * preference query; use the platform default, then try the other GL API. */
static inline bool glrdp_request_context(retro_environment_t environment,
                                        struct retro_hw_render_callback *hw)
{
    unsigned preferred = RETRO_HW_CONTEXT_NONE;
    bool es;
#ifdef GLRDP_DEFAULT_GLES2
    es = true;
#else
    es = false;
#endif
    if (environment(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred)) {
        if (preferred == RETRO_HW_CONTEXT_OPENGLES2 ||
            preferred == RETRO_HW_CONTEXT_OPENGLES3 ||
            preferred == RETRO_HW_CONTEXT_OPENGLES_VERSION)
            es = true;
        else if (preferred == RETRO_HW_CONTEXT_OPENGL ||
                 preferred == RETRO_HW_CONTEXT_OPENGL_CORE)
            es = false;
    }
    for (unsigned attempt = 0; attempt < 2; ++attempt, es = !es) {
        hw->context_type = es ? RETRO_HW_CONTEXT_OPENGLES2 : RETRO_HW_CONTEXT_OPENGL_CORE;
        hw->version_major = es ? 2 : 3;
        hw->version_minor = es ? 0 : 3;
        if (environment(RETRO_ENVIRONMENT_SET_HW_RENDER, hw)) return true;
    }
    return false;
}
#endif
