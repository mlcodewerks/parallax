#pragma once
#include <stdbool.h>
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
enum renderer_type { RENDERER_ANGRYLION, RENDERER_PARALLEL };
struct renderer_settings {
    bool rsp_hle, lle_graphics, lle_audio;
    bool per_cycle_timing;
    bool cached_interpreter;
    enum renderer_type renderer;
    unsigned threads, upscale, downscale;
    bool performance_only, vi_filter, dedither, blur, overscan, bob;
    bool native_lod, native_tex_rect, ss_readbacks, ss_dither;
};
extern struct renderer_settings renderer_settings;
void renderer_register_options(retro_environment_t environment);
void renderer_read_options(retro_environment_t environment);
#ifdef __cplusplus
}
#endif
