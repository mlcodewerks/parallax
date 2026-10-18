#include "renderer_options.h"
#include <stdlib.h>
#include <string.h>

struct renderer_settings renderer_settings;
#ifdef HAVE_PARALLEL_RDP
#define RENDERERS_BASE "angrylion|parallel-rdp"
#else
#define RENDERERS_BASE "angrylion"
#endif
#ifdef HAVE_GLRDP
#define RENDERERS RENDERERS_BASE "|glrdp"
#else
#define RENDERERS RENDERERS_BASE
#endif
#ifdef HAVE_ARI64_X64
#define CPU_CORES "cached|pure|dynarec"
#else
#define CPU_CORES "cached|pure"
#endif
/* Legacy options are supported by every libretro frontend; first value is default.
 * Settings are latched at content load, keeping renderer resources and RDRAM intact. */
static const struct retro_variable options[] = {
    {"wtfn64_renderer", "Renderer (restart required); " RENDERERS},
#ifdef HAVE_GLRDP
    {"wtfn64_glrdp_upscale", "GLRDP internal upscaling (restart required); 1|2|4|8"},
    {"wtfn64_glrdp_blending", "GLRDP blending accuracy (restart required); accurate|fast"},
    {"wtfn64_glrdp_combiner", "GLRDP combiner accuracy (restart required); accurate|fast"},
    {"wtfn64_glrdp_texture_filter", "GLRDP texture filtering (restart required); native|point"},
    {"wtfn64_glrdp_texcoords", "GLRDP texture coordinate accuracy (restart required); accurate|fast"},
    {"wtfn64_glrdp_shading", "GLRDP shade interpolation (restart required); accurate|fast"},
    {"wtfn64_glrdp_shader_specialization", "GLRDP shader specialization (restart required); disabled|enabled"},
    {"wtfn64_glrdp_texture_lod", "GLRDP texture LOD (restart required); enabled|disabled"},
    {"wtfn64_glrdp_color_dither", "GLRDP RGB dithering (restart required); enabled|disabled"},
    {"wtfn64_glrdp_alpha_dither", "GLRDP alpha dithering (restart required); enabled|disabled"},
    {"wtfn64_glrdp_vi_filter", "GLRDP VI filters (restart required); enabled|disabled"},
    {"wtfn64_glrdp_deinterlace", "GLRDP deinterlacing (restart required); weave|bob"},
    {"wtfn64_glrdp_coverage", "GLRDP coverage sampling (restart required); off|2x|reduced|full"},
    {"wtfn64_glrdp_vi_aa", "GLRDP VI antialiasing (restart required); off|reduced|full"},
    {"wtfn64_glrdp_vi_restore", "GLRDP VI dedithering (restart required); disabled|enabled"},
    {"wtfn64_glrdp_vi_divot", "GLRDP VI divot filter (restart required); disabled|enabled"},
    {"wtfn64_glrdp_vi_resample", "GLRDP VI resampling (restart required); enabled|disabled"},
    {"wtfn64_glrdp_vi_gamma", "GLRDP VI gamma correction (restart required); enabled|disabled"},
    {"wtfn64_glrdp_vi_gamma_dither", "GLRDP VI gamma dithering (restart required); enabled|disabled"},
#endif
    {"wtfn64_rsp_hle", "RSP HLE (restart required); enabled|disabled"},
    {"wtfn64_cpu_core", "CPU core (restart required); " CPU_CORES},
    {"wtfn64_lle_graphics", "Send display lists to LLE RSP (restart required); disabled|enabled"},
    {"wtfn64_lle_audio", "Send audio lists to LLE RSP (restart required); disabled|enabled"},
    {"wtfn64_timing", "Timing accuracy (restart required); fast|accurate"},
    {"wtfn64_angrylion_threads", "Angrylion rendering threads (restart required); 6|2|4|8|10|auto"},
    {"wtfn64_performance_cores", "Performance cores only (restart required); enabled|disabled"},
    {"wtfn64_angrylion_upscale", "Angrylion upscaling (restart required); 1|2|4"},
#ifdef HAVE_PARALLEL_RDP
    {"wtfn64_parallel_upscale", "paraLLEl-RDP upscaling (restart required); 1|2|4|8"},
#endif
    {"wtfn64_vi_filter", "Angrylion/paraLLEl-RDP VI filtering (restart required); enabled|disabled"},
    {"wtfn64_vi_dedither", "Angrylion/paraLLEl-RDP VI dedithering (restart required); enabled|disabled"},
    {"wtfn64_vi_blur", "Angrylion/paraLLEl-RDP VI divot filter (restart required); enabled|disabled"},
    {"wtfn64_crop_overscan", "Crop overscan (restart required); disabled|enabled"},
    {"wtfn64_angrylion_deinterlace", "Angrylion deinterlacing (restart required); weave|bob"},
    {"wtfn64_native_lod", "Native texture LOD (restart required); enabled|disabled"},
    {"mupen64plus-pak1", "Player 1 controller pak; memory|none|rumble|transfer"},
    {"mupen64plus-pak2", "Player 2 controller pak; none|memory|rumble|transfer"},
    {"mupen64plus-pak3", "Player 3 controller pak; none|memory|rumble|transfer"},
    {"mupen64plus-pak4", "Player 4 controller pak; none|memory|rumble|transfer"},
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
        {"emulation", "Emulation", "CPU core, RSP task routing and accuracy controls. Changes require restarting content."},
        {"video", "Video", "Renderer selection and shared presentation options."},
        {"input", "Controllers", "Controller accessories."},
#ifdef HAVE_GLRDP
        {"glrdp", "GLRDP", "Native GPU RDP rendering with OpenGL 3.3 or OpenGL ES 2.0."},
#endif
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
            else if (strstr(d->key, "glrdp")) d->category_key = "glrdp";
            else if (strstr(d->key, "parallel") || strstr(d->key, "ss_") || strstr(d->key, "native_tex_rect")) d->category_key = "parallel";
            else if (strstr(d->key, "rsp_") || strstr(d->key, "lle_") || strstr(d->key, "_timing") || strstr(d->key, "wtfn64_cpu_")) d->category_key = "emulation";
            if (!strcmp(d->key, "wtfn64_glrdp_blending"))
                d->info = "Accurate uses the RDP divider and quantized blend factors. Fast uses arithmetic division with the same colour sources and factors; translucent edges may differ. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_combiner"))
                d->info = "Accurate preserves signed nine-bit combiner arithmetic and rounding. Fast clamps the colour equation and uses eight-bit normalized factors; lighting and sharpening may differ. Both retain both cycles and alpha tests. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_filter"))
                d->info = "Disable all GLRDP VI filters, including resampling, gamma and gamma dithering. Individual selections are retained and apply when enabled. Independent of RDP coverage and blending. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_deinterlace"))
                d->info = "Weave retains alternating VI fields for stable interlaced detail. Bob duplicates the latest field for less motion combing at half vertical detail. Independent of VI filtering. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_texture_filter"))
                d->info = "Native follows the game's RDP texture filtering. Point skips multi-tap texture filtering for lower GPU cost and a sharper, pixelated image. Independent of VI resampling. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_texcoords"))
                d->info = "Accurate uses native fixed-point span coordinates and the RDP perspective divider at 1x where supported. Fast uses floating-point planes and perspective division; texture alignment and wobble can differ. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_shading"))
                d->info = "Accurate uses native fixed-point span and coverage corrections for vertex colours. Fast uses floating-point colour planes; gradients and alpha thresholds can differ. Independent of combiner arithmetic and texture coordinates. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_shader_specialization"))
                d->info = "Precompile a shader for the selected coverage and accuracy settings when content loads. Removes unused shader paths without lowering accuracy; increases startup compilation time. No shaders compile during gameplay. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_texture_lod"))
                d->info = "Allow the game's per-pixel mip level and LOD fraction calculation. Disabled uses base tiles and skips dynamic LOD; distant textures may shimmer and detail or sharpening effects can change. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_color_dither"))
                d->info = "Allow the game's RGB dithering before framebuffer storage. Disabled reduces shader work but can expose colour banding. Independent of alpha dithering and VI filters. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_alpha_dither"))
                d->info = "Allow dither added to pixel and shade alpha. Disabled can change transparency thresholds and fades. Alpha comparison and depth rejection remain enabled. Independent of RGB dithering. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_vi_filter") || !strcmp(d->key, "wtfn64_vi_dedither") ||
                !strcmp(d->key, "wtfn64_vi_blur"))
                d->info = "Applies to Angrylion and paraLLEl-RDP. GLRDP has independent VI controls in the GLRDP category. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_coverage"))
                d->info = "Off treats surviving pixels as fully covered. 2x estimates coverage from two native samples; Reduced uses four; Full uses eight. Lower sample counts can lose thin edges. Alpha, depth and AA-off first-sample tests remain active. Independent of VI filters. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_aa"))
                d->info = "Controls the game's requested VI edge reconstruction independently of RDP coverage. Off skips reconstruction; Reduced uses four neighbours; Full uses all six. Does not change resampling or other VI filters. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_restore"))
                d->info = "Allow the game's eight-neighbour VI dither restoration filter. Disable to reduce scanout cost and retain the framebuffer's dither pattern. Independent of VI antialiasing. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_divot"))
                d->info = "Allow the game's horizontal three-pixel median filter at partially covered edges. Independent of other VI filters. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_resample"))
                d->info = "Allow the game's native VI bilinear resampling. Disabled uses point sampling without disabling edge reconstruction, dedithering or divot filtering. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_gamma"))
                d->info = "Allow gamma correction when requested by the game. Independent of gamma dithering. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_glrdp_vi_gamma_dither"))
                d->info = "Allow gamma dithering when requested by the game. Independent of gamma correction. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_timing"))
                d->info = "Accurate uses full CPU cache contents and detailed per-cycle timing. Fast tracks cache tags, instruction interlocks and bus delays while accessing RAM directly. Both time RSP tasks and RDP commands by their work, including pipeline and DMA waits. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_cpu_core"))
                d->info = "Select the CPU execution core. All choices use the same instruction behavior and Timing accuracy setting. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_rsp_hle"))
                d->info = "Use HLE for supported RSP microcodes. Unsupported RSP tasks use LLE. Graphics/audio LLE overrides apply to their respective tasks. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_lle_graphics"))
                d->info = "Force display lists through the LLE RSP instead of the HLE microcode emitters. Restart content to apply.";
            if (!strcmp(d->key, "wtfn64_lle_audio"))
                d->info = "Force audio lists through the LLE RSP instead of HLE audio processing. Restart content to apply.";
            if (strstr(d->key, "-pak")) {
                d->category_key = "input";
                d->info = "Choose the accessory attached to this controller. A Memory Pak allows games to save progress.";
            }
            p = values[i];
            do {
                next = strchr(p, '|');
                if (next) *next++ = 0;
                d->values[j++].value = p;
                if (!strcmp(d->key, "wtfn64_cpu_core"))
                    d->values[j - 1].label = !strcmp(p, "cached") ? "Cached interpreter" :
                        !strcmp(p, "pure") ? "Pure interpreter" :
                        "Dynarec";
                if (strstr(d->key, "-pak"))
                    d->values[j - 1].label = !strcmp(p, "memory") ? "Memory Pak" :
                        !strcmp(p, "rumble") ? "Rumble Pak" :
                        !strcmp(p, "transfer") ? "Transfer Pak" : "None";
                p = next;
            } while (p && j < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1);
            if (!strcmp(d->key, "wtfn64_glrdp_coverage")) {
                d->values[0].label = "Off (full coverage per surviving pixel)";
                d->values[1].label = "2x (2 samples)";
                d->values[2].label = "4x (4 samples, reduced)";
                d->values[3].label = "8x (8 samples, full)";
            }
            if (!strcmp(d->key, "wtfn64_glrdp_vi_aa")) {
                d->values[0].label = "Off";
                d->values[1].label = "Reduced (4 neighbours)";
                d->values[2].label = "Full (6 neighbours)";
            }
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
enum controller_pak_option controller_read_pak_option(retro_environment_t env, unsigned port)
{
    static const char *keys[] = {"mupen64plus-pak1", "mupen64plus-pak2",
        "mupen64plus-pak3", "mupen64plus-pak4"};
    if (port >= 4) return CONTROLLER_PAK_NONE;
    enum controller_pak_option fallback = port == 0 ? CONTROLLER_PAK_MEMORY : CONTROLLER_PAK_NONE;
    const char *v = value(env, keys[port], port == 0 ? "memory" : "none");
    if (!strcmp(v, "memory")) return CONTROLLER_PAK_MEMORY;
    if (!strcmp(v, "rumble")) return CONTROLLER_PAK_RUMBLE;
    if (!strcmp(v, "transfer")) return CONTROLLER_PAK_TRANSFER;
    if (!strcmp(v, "none")) return CONTROLLER_PAK_NONE;
    return fallback;
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
    s->rsp_hle = toggle(env, "wtfn64_rsp_hle", true);
    s->lle_graphics = toggle(env, "wtfn64_lle_graphics", false);
    s->lle_audio = toggle(env, "wtfn64_lle_audio", false);
    v = value(env, "wtfn64_timing", "fast");
    s->per_cycle_timing = strcmp(v, "accurate") == 0;
    s->component_timing = true;
    s->fast_cpu_timing = true;
    v = value(env, "wtfn64_cpu_core", "cached");
    s->cpu_core = !strcmp(v, "pure") ? CPU_CORE_PURE : CPU_CORE_CACHED;
#ifdef HAVE_ARI64_X64
    if (!strcmp(v, "dynarec")) s->cpu_core = CPU_CORE_DYNAREC;
#endif
#ifdef HAVE_PARALLEL_RDP
    if (strcmp(value(env, "wtfn64_renderer", "angrylion"), "parallel-rdp") == 0)
        s->renderer = RENDERER_PARALLEL;
#endif
#ifdef HAVE_GLRDP
    if (!strcmp(value(env, "wtfn64_renderer", "angrylion"), "glrdp"))
        s->renderer = RENDERER_GLRDP;
    s->glrdp = glrdp_default_config();
    s->glrdp.blending = !strcmp(value(env, "wtfn64_glrdp_blending", "accurate"), "fast") ?
        GLRDP_ARITHMETIC_FAST : GLRDP_ARITHMETIC_ACCURATE;
    s->glrdp.combiner = !strcmp(value(env, "wtfn64_glrdp_combiner", "accurate"), "fast") ?
        GLRDP_ARITHMETIC_FAST : GLRDP_ARITHMETIC_ACCURATE;
    s->glrdp.vi_filter = toggle(env, "wtfn64_glrdp_vi_filter", true);
    s->glrdp.vi_bob = !strcmp(value(env, "wtfn64_glrdp_deinterlace", "weave"), "bob");
    s->glrdp.texture_filter = strcmp(value(env, "wtfn64_glrdp_texture_filter", "native"), "point") != 0;
    s->glrdp.accurate_texcoords = strcmp(value(env, "wtfn64_glrdp_texcoords", "accurate"), "fast") != 0;
    s->glrdp.accurate_shading = strcmp(value(env, "wtfn64_glrdp_shading", "accurate"), "fast") != 0;
    s->glrdp.specialize_shaders = toggle(env, "wtfn64_glrdp_shader_specialization", false);
    s->glrdp.texture_lod = toggle(env, "wtfn64_glrdp_texture_lod", true);
    s->glrdp.color_dither = toggle(env, "wtfn64_glrdp_color_dither", true);
    s->glrdp.alpha_dither = toggle(env, "wtfn64_glrdp_alpha_dither", true);
    v = value(env, "wtfn64_glrdp_coverage", "off");
    s->glrdp.coverage = !strcmp(v, "off") ? GLRDP_COVERAGE_OFF :
        !strcmp(v, "2x") ? GLRDP_COVERAGE_2X :
        !strcmp(v, "reduced") ? GLRDP_COVERAGE_REDUCED : GLRDP_COVERAGE_FULL;
    v = value(env, "wtfn64_glrdp_vi_aa", "off");
    s->glrdp.vi_aa = !strcmp(v, "off") ? GLRDP_VI_AA_OFF :
        !strcmp(v, "reduced") ? GLRDP_VI_AA_REDUCED : GLRDP_VI_AA_FULL;
    s->glrdp.vi_restore = toggle(env, "wtfn64_glrdp_vi_restore", false);
    s->glrdp.vi_divot = toggle(env, "wtfn64_glrdp_vi_divot", false);
    s->glrdp.vi_resample = toggle(env, "wtfn64_glrdp_vi_resample", true);
    s->glrdp.vi_gamma = toggle(env, "wtfn64_glrdp_vi_gamma", true);
    s->glrdp.vi_gamma_dither = toggle(env, "wtfn64_glrdp_vi_gamma_dither", true);
#endif
    v = value(env, "wtfn64_angrylion_threads", "6");
    s->threads = strcmp(v, "auto") == 0 ? 0 : (unsigned)atoi(v);
    if (strcmp(v, "auto") != 0 && (s->threads < 2 || s->threads > 10 || s->threads % 2))
        s->threads = 6;
    s->performance_only = toggle(env, "wtfn64_performance_cores", true);
    s->upscale = s->renderer == RENDERER_ANGRYLION ?
        scale(env, "wtfn64_angrylion_upscale", 4) : scale(env, "wtfn64_parallel_upscale", 8);
    if (s->renderer == RENDERER_GLRDP)
        s->upscale = scale(env, "wtfn64_glrdp_upscale", 8);
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
