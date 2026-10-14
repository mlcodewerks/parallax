#pragma once
#include <glad.h>
#include <libretro.h>
// Resolve only through the frontend's current context.
bool load_gl33(retro_hw_get_proc_address_t resolver, bool gles2);
using GLRDPPrecisionQuery = void(APIENTRY *)(GLenum, GLenum, GLint *, GLint *);
extern GLRDPPrecisionQuery glrdp_precision_query;
