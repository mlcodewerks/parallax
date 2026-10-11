#pragma once
#include <stdbool.h>
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
enum renderer_type { RENDERER_ANGRYLION, RENDERER_PARALLEL };
enum cpu_core_type { CPU_CORE_CACHED, CPU_CORE_PURE, CPU_CORE_DYNAREC };
enum controller_pak_option { CONTROLLER_PAK_NONE, CONTROLLER_PAK_MEMORY,
    CONTROLLER_PAK_RUMBLE, CONTROLLER_PAK_TRANSFER };
struct renderer_settings {
    bool rsp_hle, lle_graphics, lle_audio;
    bool per_cycle_timing;
    enum cpu_core_type cpu_core;
    enum renderer_type renderer;
    unsigned threads, upscale, downscale;
    bool performance_only, vi_filter, dedither, blur, overscan, bob;
    bool native_lod, native_tex_rect, ss_readbacks, ss_dither;
};
extern struct renderer_settings renderer_settings;
void renderer_register_options(retro_environment_t environment);
void renderer_read_options(retro_environment_t environment);
enum controller_pak_option controller_read_pak_option(retro_environment_t environment, unsigned port);
#ifdef __cplusplus
}
#endif
