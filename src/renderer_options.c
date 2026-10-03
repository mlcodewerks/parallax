#include "renderer_options.h"
#include <stdlib.h>
#include <string.h>

struct renderer_settings renderer_settings;
#ifdef HAVE_PARALLEL_RDP
#define RENDERERS "angrylion|parallel-rdp"
#else
#define RENDERERS "angrylion"
#endif
/* Legacy options are supported by every libretro frontend; first value is default.
 * Settings are latched at content load, keeping renderer resources and RDRAM intact. */
static const struct retro_variable options[] = {
    {"wtfn64_renderer", "Renderer (restart required); " RENDERERS},
    {"wtfn64_rsp_hle", "RSP HLE (restart required); disabled|enabled"},
    {"wtfn64_lle_graphics", "Send display lists to LLE RSP (restart required); disabled|enabled"},
    {"wtfn64_lle_audio", "Send audio lists to LLE RSP (restart required); disabled|enabled"},
    {"wtfn64_cache_emulation", "CPU cache emulation (restart required); enabled|disabled"},
    {"wtfn64_rsp_timing", "RSP cycle timing (restart required); enabled|disabled"},
    {"wtfn64_rdp_timing", "RDP instruction timing (restart required); enabled|disabled"},
    {"wtfn64_angrylion_threads", "Angrylion rendering threads (restart required); 6|2|4|8|10|auto"},
    {"wtfn64_performance_cores", "Performance cores only (restart required); enabled|disabled"},
    {"wtfn64_angrylion_upscale", "Angrylion upscaling (restart required); 1|2|4"},
#ifdef HAVE_PARALLEL_RDP
    {"wtfn64_parallel_upscale", "paraLLEl-RDP upscaling (restart required); 1|2|4|8"},
#endif
    {"wtfn64_vi_filter", "VI filtering (restart required); enabled|disabled"},
    {"wtfn64_vi_dedither", "VI dedithering (restart required); enabled|disabled"},
    {"wtfn64_vi_blur", "VI divot filter (restart required); enabled|disabled"},
    {"wtfn64_crop_overscan", "Crop overscan (restart required); disabled|enabled"},
    {"wtfn64_angrylion_deinterlace", "Angrylion deinterlacing (restart required); weave|bob"},
    {"wtfn64_native_lod", "Native texture LOD (restart required); enabled|disabled"},
#ifdef HAVE_PARALLEL_RDP
    {"wtfn64_parallel_downscale", "paraLLEl-RDP downsampling steps (restart required); 0|1|2|3"},
    {"wtfn64_native_tex_rect", "paraLLEl-RDP native texture rectangles (restart required); enabled|disabled"},
    {"wtfn64_ss_readbacks", "paraLLEl-RDP supersampled readbacks (restart required); disabled|enabled"},
    {"wtfn64_ss_dither", "paraLLEl-RDP supersampled dithering (restart required); disabled|enabled"},
#endif
    {NULL, NULL}
};

void renderer_register_options(retro_environment_t environment)
{
    static struct retro_core_option_v2_definition definitions[64];
    static struct retro_core_option_definition legacy_definitions[64];
    static char descriptions[64][160], values[64][256];
    static struct retro_core_option_v2_category categories[] = {
        {"emulation", "Emulation", "RSP task routing and accuracy controls. Changes require restarting content."},
        {"video", "Video", "Renderer selection and shared presentation options."},
        {"angrylion", "Angrylion", "Software RDP rendering and synchronization."},
#ifdef HAVE_PARALLEL_RDP
        {"parallel", "paraLLEl-RDP", "Vulkan RDP rendering."},
#endif
        {NULL, NULL, NULL}
    };
    unsigned version = 0, i;
    environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version);
    if (version >= 1) {
        memset(definitions, 0, sizeof(definitions));
        memset(legacy_definitions, 0, sizeof(legacy_definitions));
        for (i = 0; options[i].key && i < 63; ++i) {
            struct retro_core_option_v2_definition *d = &definitions[i];
            const char *separator = strchr(options[i].value, ';');
            size_t n = (size_t)(separator - options[i].value);
            unsigned j = 0;
            char *p, *next;
            memcpy(descriptions[i], options[i].value, n);
            descriptions[i][n] = 0;
            strcpy(values[i], separator + 2);
            d->key = options[i].key;
            d->desc = descriptions[i];
            d->info = "Changes take effect when content is restarted.";
            d->category_key = "video";
            if (strstr(d->key, "angrylion")) d->category_key = "angrylion";
            else if (strstr(d->key, "parallel") || strstr(d->key, "ss_") || strstr(d->key, "native_tex_rect")) d->category_key = "parallel";
            else if (strstr(d->key, "rsp_") || strstr(d->key, "lle_") || strstr(d->key, "cache_emulation") || strstr(d->key, "rdp_timing")) d->category_key = "emulation";
            if (!strcmp(d->key, "wtfn64_rsp_hle"))
                d->info = "Use upstream audio HLE and RDP command emission for supported graphics microcodes. Unsupported tasks use LLE. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_lle_graphics"))
                d->info = "Force display lists through the LLE RSP instead of the HLE microcode emitters. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_lle_audio"))
                d->info = "Force audio lists through the LLE RSP instead of HLE audio processing. Restart content to apply.";
            p = values[i];
            do {
                next = strchr(p, '|');
                if (next) *next++ = 0;
                d->values[j++].value = p;
                p = next;
            } while (p && j < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1);
            d->default_value = d->values[0].value;
            legacy_definitions[i].key = d->key;
            legacy_definitions[i].desc = d->desc;
            legacy_definitions[i].info = d->info;
            memcpy(legacy_definitions[i].values, d->values, sizeof(d->values));
            legacy_definitions[i].default_value = d->default_value;
        }
        if (version >= 2) {
            struct retro_core_options_v2 v2 = {categories, definitions};
            /* SET_CORE_OPTIONS_V2 returns category support, not registration success. */
            environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &v2);
        } else environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, legacy_definitions);
        return;
    }
    environment(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)options);
}
static const char *value(retro_environment_t environment, const char *key, const char *fallback)
{
    struct retro_variable var = {key, NULL};
    return environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : fallback;
}
static bool toggle(retro_environment_t env, const char *key, bool fallback)
{
    const char *v = value(env, key, fallback ? "enabled" : "disabled");
    return !strcmp(v, "enabled") ? true : !strcmp(v, "disabled") ? false : fallback;
}
static unsigned scale(retro_environment_t env, const char *key, unsigned maximum)
{
    unsigned n = (unsigned)atoi(value(env, key, "1"));
    return (n == 2 || n == 4 || (n == 8 && maximum == 8)) ? n : 1;
}
void renderer_read_options(retro_environment_t env)
{
    struct renderer_settings *s = &renderer_settings;
    const char *v;
    memset(s, 0, sizeof(*s));
    s->rsp_hle = toggle(env, "wtfn64_rsp_hle", false);
    s->lle_graphics = toggle(env, "wtfn64_lle_graphics", false);
    s->lle_audio = toggle(env, "wtfn64_lle_audio", false);
    s->cache_emulation = toggle(env, "wtfn64_cache_emulation", true);
    s->rsp_timing = toggle(env, "wtfn64_rsp_timing", true);
    s->rdp_timing = toggle(env, "wtfn64_rdp_timing", true);
#ifdef HAVE_PARALLEL_RDP
    if (strcmp(value(env, "wtfn64_renderer", "angrylion"), "parallel-rdp") == 0)
        s->renderer = RENDERER_PARALLEL;
#endif
    v = value(env, "wtfn64_angrylion_threads", "6");
    s->threads = strcmp(v, "auto") == 0 ? 0 : (unsigned)atoi(v);
    if (strcmp(v, "auto") != 0 && (s->threads < 2 || s->threads > 10 || s->threads % 2))
        s->threads = 6;
    s->performance_only = toggle(env, "wtfn64_performance_cores", true);
    s->upscale = s->renderer == RENDERER_ANGRYLION ?
        scale(env, "wtfn64_angrylion_upscale", 4) : scale(env, "wtfn64_parallel_upscale", 8);
    s->vi_filter = toggle(env, "wtfn64_vi_filter", true);
    s->dedither = toggle(env, "wtfn64_vi_dedither", true);
    s->blur = toggle(env, "wtfn64_vi_blur", true);
    s->overscan = toggle(env, "wtfn64_crop_overscan", false);
    s->bob = strcmp(value(env, "wtfn64_angrylion_deinterlace", "weave"), "bob") == 0;
    s->downscale = (unsigned)atoi(value(env, "wtfn64_parallel_downscale", "0"));
    if (s->downscale > 3) s->downscale = 0;
    s->native_lod = toggle(env, "wtfn64_native_lod", true);
    s->native_tex_rect = toggle(env, "wtfn64_native_tex_rect", true);
    s->ss_readbacks = toggle(env, "wtfn64_ss_readbacks", false);
    s->ss_dither = toggle(env, "wtfn64_ss_dither", false);
}
