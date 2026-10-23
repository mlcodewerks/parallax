/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-Next - libretro.c                                         *
 *   Copyright (C) 2020 M4xw <m4x@m4xw.net>                                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>

#include "libretro.h"
#include "libretro_private.h"
#include "core/lifecycle.h"
#include "core/file.h"

#include "core/frontend.h"
#include "device/rcp/rdp/angrylion/n64video.h"
#include "renderer_options.h"
#ifdef HAVE_GLRDP
#include "device/rcp/rdp/glrdp/libretro_glrdp.h"
#endif
#include "device/rcp/rsp/optional_rsp.h"
#include "device/rcp/rdp/performance_cores.h"
#ifdef HAVE_PARALLEL_RDP
#include "device/rcp/rdp/para_intf.h"
#endif

#ifdef HAVE_LIBNX
#include <switch.h>
#endif



#include "core/types.h"
#include "device/r4300/r4300_core.h"
#include "device/memory.h"
#include "main/main.h"
#include "core/log.h"

#include "main/util.h"



#include "main/rom.h"
#include "core/backend.h"
#include "device/rcp/rcp.h"
#include "device/pif/pif.h"
#include "libretro_memory.h"

#ifndef PRESCALE_WIDTH
#define PRESCALE_WIDTH 640
#endif

#ifndef PRESCALE_HEIGHT
#define PRESCALE_HEIGHT 625
#endif

#define PATH_SIZE 2048

#define ISHEXDEC ((codeLine[cursor] >= '0') && (codeLine[cursor] <= '9')) || ((codeLine[cursor] >= 'a') && (codeLine[cursor] <= 'f')) || ((codeLine[cursor] >= 'A') && (codeLine[cursor] <= 'F'))

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

static void quiet_log(enum retro_log_level level, const char *format, ...)
{ (void)level; (void)format; }
retro_log_printf_t log_cb = quiet_log;
retro_video_refresh_t video_cb = NULL;
retro_input_poll_t poll_cb = NULL;
retro_input_state_t input_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
#if defined(M64P_AUDIO_SYNC_TRACE) && defined(_WIN32)
#include "../benchmarks/audio_sync_trace.h"
#else
#define sync_open() ((void)0)
#define sync_close() ((void)0)
#define sync_begin() ((void)0)
#define sync_end() ((void)0)
#endif
retro_environment_t environ_cb = NULL;

struct retro_rumble_interface rumble;

save_memory_data saved_memory;

static uint8_t *game_data = NULL;
static uint32_t game_size = 0;
static bool emu_initialized = false;
static unsigned audio_buffer_size = 2048;

static unsigned retro_filtering = 0;
static bool first_context_reset = false;
static bool initializing = true;
static bool load_game_successful = false;
static double frontend_refresh_rate;
static bool renderer_open = false;
bool libretro_swap_buffer;

float retro_screen_aspect = 4.0 / 3.0;


// 64DD globals
char *retro_dd_path_img = NULL;
char *retro_dd_path_rom = NULL;

// Other Subsystems
char *retro_transferpak_rom_path = NULL;
char *retro_transferpak_ram_path = NULL;

uint32_t EnableFrameDuping = 1;
uint32_t EnableOverscan = 0;
uint32_t AspectRatio = 1;
uint32_t EnableFullspeed = 0;
uint32_t CountPerOp = 0;
uint32_t CountPerOpDenomPot = 0;
uint32_t CountPerScanlineOverride = 0;
uint32_t ForceDisableExtraMem = 0;
uint32_t IgnoreTLBExceptions = 0;

extern struct device g_dev;
extern unsigned int r4300_emumode;


// after the controller's core_controller* member has been assigned we can update
// them straight from here...
extern struct
{
    core_controller *control;
    core_buttons buttons;
} controller[4];
// ...but it won't be at least the first time we're called, in that case set
// these as the startup controller selection.
int pad_pak_types[4];
int pad_present[4] = {1, 1, 1, 1};



extern core_rom_header ROM_HEADER;
void update_variables(bool startup);
extern void deinit_audio_libretro(void);
extern void init_audio_libretro(unsigned max_audio_frames);
extern void audio_end_frame_libretro(double seconds);

static void setup_variables(void)
{
    renderer_register_options(environ_cb);

    static const struct retro_controller_description port[] = {
        {"Controller", RETRO_DEVICE_JOYPAD}};

    static const struct retro_controller_info ports[] = {
        {port, 1},
        {port, 1},
        {0, 0}};

    //environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

static void cleanup_global_paths()
{
    // Ensure potential leftovers are cleaned up
    if (retro_dd_path_img)
    {
        free(retro_dd_path_img);
        retro_dd_path_img = NULL;
    }

    if (retro_dd_path_rom)
    {
        free(retro_dd_path_rom);
        retro_dd_path_rom = NULL;
    }

    if (retro_transferpak_rom_path)
    {
        free(retro_transferpak_rom_path);
        retro_transferpak_rom_path = NULL;
    }

    if (retro_transferpak_ram_path)
    {
        free(retro_transferpak_ram_path);
        retro_transferpak_ram_path = NULL;
    }
}



static bool emu_step_load_data(void)
{
    bool loaded = core_open_content(game_data, game_size);
    free(game_data);
    game_data = NULL;
    if (!loaded && log_cb) log_cb(RETRO_LOG_ERROR, "Failed to load content.\n");
    return loaded;
}

static void emu_step_initialize(void)
{
    if (emu_initialized)
        return;
    update_variables(true);
    emu_initialized = true;
    core_bind_backends();
}

void run_emulation_frame()
{
    if (initializing)
    {
        extern void main_prerun(void);
        main_prerun();
        initializing = false;
    }
    main_run();
}

const char *retro_get_system_directory(void)
{
    const char *dir = NULL;
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir);

    return dir ? dir : ".";
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
#if defined(M64P_AUDIO_SYNC_TRACE) && defined(_WIN32)
    sync_frontend = cb;
    audio_batch_cb = sync_audio;
#else
    audio_batch_cb = cb;
#endif
}
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    size_t outSize = 0;
    bool result = false;
    void *gameBuffer = NULL;

    cleanup_global_paths();

    switch (game_type)
    {
    case RETRO_GAME_TYPE_DD:
        if (num_info == 1)
        {
            retro_dd_path_img = strdup(info[0].path);
        }
        else if (num_info == 2)
        {
            retro_dd_path_img = strdup(info[0].path);
            retro_dd_path_rom = strdup(info[1].path);
        }
        else
        {
            return false;
        }

        log_cb(RETRO_LOG_INFO, "Loading %s...\n", info[0].path);

        result = load_file(info[1].path, &gameBuffer, &outSize) == file_ok;
        if (result)
        {
            memcpy(&info[1].data, &gameBuffer, sizeof(void *));
            memcpy(&info[1].size, &outSize, sizeof(size_t));
            result = result && retro_load_game(&info[1]);

            if (gameBuffer)
            {
                free(gameBuffer);
                gameBuffer = NULL;
                // To prevent potential double free // TODO: revisit later
                memcpy(&info[1].data, &gameBuffer, sizeof(void *));
            }
        }
        break;
    case RETRO_GAME_TYPE_TRANSFERPAK:
        if (num_info == 3)
        {
            retro_transferpak_ram_path = strdup(info[0].path);
            retro_transferpak_rom_path = strdup(info[1].path);
        }
        else
        {
            return false;
        }

        log_cb(RETRO_LOG_INFO, "Loading %s...\n", info[0].path);
        log_cb(RETRO_LOG_INFO, "Loading %s...\n", info[1].path);
        log_cb(RETRO_LOG_INFO, "Loading %s...\n", info[2].path);
        result = load_file(info[2].path, &gameBuffer, &outSize) == file_ok;
        if (result)
        {
            memcpy(&info[2].data, &gameBuffer, sizeof(void *));
            memcpy(&info[2].size, &outSize, sizeof(size_t));
            result = result && retro_load_game(&info[2]);
            if (gameBuffer)
            {
                free(gameBuffer);
                gameBuffer = NULL;
                // To prevent potential double free // TODO: revisit later
                memcpy(&info[2].data, &gameBuffer, sizeof(void *));
            }
        }
        break;
    default:
        return false;
    }

    return result;
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    static const struct retro_subsystem_memory_info memory_info_dd[] = {
        {"srm", RETRO_MEMORY_DD},
        {}};

    static const struct retro_subsystem_memory_info memory_info_transferpak[] = {
        {"srm", RETRO_MEMORY_TRANSFERPAK},
        {}};

    static const struct retro_subsystem_rom_info dd_roms[] = {
        {"Disk", "ndd", true, false, true, memory_info_dd, 1},
        {"Cartridge", "n64|v64|z64|bin|u1", true, false, true, NULL, 0},
        {}};

    static const struct retro_subsystem_rom_info transferpak_roms[] = {
        {"Gameboy RAM", "ram|sav", true, false, true, NULL, 0},
        {"Gameboy ROM", "rom|gb|gbc", true, false, true, NULL, 0},
        {"Cartridge", "n64|v64|z64|bin|u1", true, false, true, memory_info_transferpak, 1},
        {}};

    static const struct retro_subsystem_info subsystems[] = {
        {"N64 Disk Drive", "ndd", dd_roms, 2, RETRO_GAME_TYPE_DD},
        {"N64 Transferpak", "gb", transferpak_roms, 3, RETRO_GAME_TYPE_TRANSFERPAK},
        {}};

    environ_cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void *)subsystems);

    setup_variables();
}

void retro_get_system_info(struct retro_system_info *info)
{
    info->library_name = "Parallax";
    info->library_version = "0.01";
    info->valid_extensions = "n64|v64|z64|bin|u1";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->geometry.base_width = 640;
    info->geometry.base_height = 480;
    info->geometry.max_width = 640 * (renderer_settings.upscale ? renderer_settings.upscale : 1);
    info->geometry.max_height = 625 * (renderer_settings.upscale ? renderer_settings.upscale : 1);
    info->geometry.aspect_ratio = 640.0 / 480.0;
    info->timing.fps = frontend_refresh_rate > 0.0 ? frontend_refresh_rate :
        vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
    info->timing.sample_rate = 44100.0;
}

unsigned retro_get_region(void)
{
    return ((ROM_PARAMS.systemtype == SYSTEM_PAL) ? RETRO_REGION_PAL : RETRO_REGION_NTSC);
}

#ifdef HAVE_PARALLEL_RDP
static struct retro_hw_render_callback hw_render;
static struct retro_hw_render_context_negotiation_interface_vulkan hw_negotiation;

static void context_reset(void)
{
    const struct retro_hw_render_interface *iface = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) || !iface ||
        iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        iface->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
        return;
    vk_set_hw_render_interface((const struct retro_hw_render_interface_vulkan *)iface);
    if (renderer_open && renderer_settings.renderer == RENDERER_PARALLEL)
    {
        vk_destroy();
        if (!vk_init() && log_cb)
            log_cb(RETRO_LOG_ERROR, "paraLLEl-RDP: failed to restore Vulkan renderer.\n");
    }
}
static void context_destroy(void) { vk_context_destroy(); }
static bool request_vulkan_context(void)
{
    memset(&hw_render, 0, sizeof(hw_render));
    memset(&hw_negotiation, 0, sizeof(hw_negotiation));
    hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
    hw_render.context_reset = context_reset;
    hw_render.context_destroy = context_destroy;
    hw_render.cache_context = false;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) return false;
    hw_negotiation.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
    hw_negotiation.interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
    hw_negotiation.get_application_info = vk_get_application_info;
    hw_negotiation.create_device = vk_create_device;
    hw_negotiation.destroy_device = vk_context_destroy;
    return environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
            &hw_negotiation);
}
#endif

#ifdef HAVE_GLRDP
#include "device/rcp/rdp/glrdp/context_request.h"
static struct retro_hw_render_callback gl_hw;
static void gl_context_reset(void)
{
    glrdp_context_reset(&gl_hw);
}
static void gl_context_destroy(void) { glrdp_context_destroy(); }
static bool request_gl_context(void)
{
    memset(&gl_hw, 0, sizeof(gl_hw));
    gl_hw.context_reset = gl_context_reset;
    gl_hw.context_destroy = gl_context_destroy;
    gl_hw.bottom_left_origin = true;
    gl_hw.cache_context = false;
    return glrdp_request_context(environ_cb, &gl_hw);
}
#endif

void retro_init(void)
{
    core_file_init(environ_cb);
    struct retro_log_callback log;
    unsigned colorMode = RETRO_PIXEL_FORMAT_XRGB8888;

    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    else
        log_cb = quiet_log;

    if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
        perf_get_cpu_features_cb = perf_cb.get_cpu_features;
    else
        perf_get_cpu_features_cb = NULL;

    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &colorMode);
    environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);



    initializing = true;
}

void retro_deinit(void)
{
    // Prevent yield to game_thread on unsuccessful context request
    if (load_game_successful)
        retro_unload_game();

    deinit_audio_libretro();

    if (perf_cb.perf_log)
        perf_cb.perf_log();



}

void update_controllers()
{
    for (unsigned port = 0; port < 4; ++port) {
        enum controller_pak_option selected = controller_read_pak_option(environ_cb, port);
        int pak = selected == CONTROLLER_PAK_MEMORY ? ACCESSORY_MEMPAK :
            selected == CONTROLLER_PAK_RUMBLE ? ACCESSORY_RAW :
            selected == CONTROLLER_PAK_TRANSFER ? ACCESSORY_TRANSFER_PAK : ACCESSORY_NONE;
        /* Keep startup selection current across content reloads. */
        pad_pak_types[port] = pak;
        if (controller[port].control) controller[port].control->accessory = pak;
    }
}

static char *find_sidecar(const char *path, const char *suffix)
{
    if (!path || strlen(path) > SIZE_MAX - strlen(suffix) - 1) return NULL;
    size_t size = strlen(path) + strlen(suffix) + 1;
    char *result = malloc(size);
    if (!result) return NULL;
    snprintf(result, size, "%s%s", path, suffix);
    core_file *file = core_file_open(result, "rb");
    if (!file) { free(result); return NULL; }
    core_file_close(file);
    return result;
}

static void format_saved_memory(void)
{
    format_sram(saved_memory.sram);
    format_eeprom(saved_memory.eeprom, EEPROM_MAX_SIZE);
    format_flashram(saved_memory.flashram);
    format_mempak(saved_memory.mempack + 0 * MEMPAK_SIZE);
    format_mempak(saved_memory.mempack + 1 * MEMPAK_SIZE);
    format_mempak(saved_memory.mempack + 2 * MEMPAK_SIZE);
    format_mempak(saved_memory.mempack + 3 * MEMPAK_SIZE);
}



bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || load_game_successful) return false;
    renderer_read_options(environ_cb);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Renderer: %s, upscale %ux, performance cores %s.\n",
                renderer_settings.renderer == RENDERER_ANGRYLION ? "Angrylion" : renderer_settings.renderer == RENDERER_GLRDP ? "GLRDP" : "paraLLEl-RDP",
                renderer_settings.upscale, renderer_settings.performance_only ? "only" : "unrestricted");
    performance_cores_init();
    performance_cores_set_enabled(renderer_settings.performance_only);
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL && !request_vulkan_context())
    {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "paraLLEl-RDP requires a Vulkan frontend; content load rejected.\n");
        return false;
    }
#endif
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP && !request_gl_context())
    {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "GLRDP requires a GL3.3 or GLES3 frontend context; content load rejected.\n");
        return false;
    }
#endif
    initializing = true;

    // Workaround for broken subsystem on static platforms
    if (!retro_dd_path_img) retro_dd_path_img = find_sidecar(game->path, ".ndd");
    if (!retro_transferpak_rom_path) retro_transferpak_rom_path = find_sidecar(game->path, ".gb");
    if (retro_transferpak_rom_path && !retro_transferpak_ram_path)
        retro_transferpak_ram_path = find_sidecar(game->path, ".sav");

    // Init default vals
    load_game_successful = false;


    if (game->size > UINT32_MAX) goto load_failed;
    if (game->data) {
        game_data = malloc(game->size);
        if (!game_data) goto load_failed;
        memcpy(game_data, game->data, game->size);
        game_size = (uint32_t)game->size;
    } else {
        size_t size = 0;
        if (!game->path || load_file(game->path, (void **)&game_data, &size) != file_ok)
            goto load_failed;
        if (size > UINT32_MAX) goto load_failed;
        game_size = (uint32_t)size;
    }

    if (!emu_step_load_data()) goto load_failed;
    format_saved_memory();
    deinit_audio_libretro();
    init_audio_libretro(audio_buffer_size);
    sync_open();
    emu_step_initialize();

    frontend_refresh_rate = vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
    load_game_successful = true;
    return true;
load_failed:
    free(game_data); game_data = NULL; game_size = 0;
    core_close_content();
    cleanup_global_paths();
    return false;
}

void retro_unload_game(void)
{
    optional_rsp_report();
    sync_close();

    /* Release backends and decoded CPU code before unmapping guest memory. */
    core_close_content();
    rdp_counters_reset(&g_dev.dp.counters);


    cleanup_global_paths();

    emu_initialized = false;
    load_game_successful = false;
    initializing = true;
    frontend_refresh_rate = 0.0;
}

void update_variables(bool startup)
{
    if (startup)
    {

        EnableFrameDuping = 1;
        EnableFullspeed = 0;
        CountPerScanlineOverride = 0;
#if defined(PARALLAX_HEADLESS_BENCH_PURE)
        r4300_emumode = EMUMODE_PURE_INTERPRETER;
#elif defined(PARALLAX_HEADLESS_BENCHMARK) && !defined(M64P_BENCH_RUNTIME_CPU)
        r4300_emumode = EMUMODE_INTERPRETER;
#else
        r4300_emumode = renderer_settings.cpu_core == CPU_CORE_PURE ? EMUMODE_PURE_INTERPRETER : EMUMODE_INTERPRETER;
#ifdef HAVE_ARI64
        if (renderer_settings.cpu_core == CPU_CORE_DYNAREC) r4300_emumode = EMUMODE_DYNAREC;
#endif
#endif
        retro_screen_aspect = 4.0 / 3.0;
        AspectRatio = 1; // Aspect::a43
        CountPerOp = 1;  // Force CountPerOp == 1
        EnableOverscan = 0;
        ForceDisableExtraMem = 0;
        IgnoreTLBExceptions = 1;
        update_controllers();
    }
}

void retro_run(void)
{
    if (!load_game_successful) return;
    libretro_swap_buffer = false;
    static bool updated = false;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        update_variables(false);
        update_controllers();
    }

    struct performance_affinity emulation_affinity;
    performance_emulation_enter(&emulation_affinity);
    sync_begin();
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) glrdp_begin_frame();
#endif
    const bool first_frame = initializing;
    const int64_t audio_start_clock = g_dev.r4300.cp0.count_clock;
    run_emulation_frame();
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) glrdp_end_frame();
#endif
    /* The frontend must pace these same guest fields, rather than advancing
     * a nominal 60/50 fields per wall-clock second for every VI mode. */
    const double refresh_rate = vi_actual_refresh_rate(&g_dev.vi);
    if (refresh_rate > 0.0 &&
        (refresh_rate - frontend_refresh_rate > 0.000001 ||
         frontend_refresh_rate - refresh_rate > 0.000001))
    {
        struct retro_system_av_info info;
        frontend_refresh_rate = refresh_rate;
        retro_get_system_av_info(&info);
        environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
    }
    ai_flush_samples(&g_dev.ai);
    /* DMA samples follow the guest oscillator, including fractional fields
     * and CPU service latency. A fixed 1/60 budget accumulates audio until
     * the queue starts dropping it. Boot/reset may restart the guest clock;
     * budget just one field then, matching audio's silent startup. */
    int64_t audio_ticks = g_dev.r4300.cp0.count_clock - audio_start_clock;
    if (first_frame || audio_ticks < 0)
        audio_ticks = g_dev.vi.field_ticks;
    audio_end_frame_libretro(audio_ticks / 46875000.0);
    sync_end();
    performance_core_leave(&emulation_affinity);

#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL)
        video_cb(libretro_swap_buffer ? RETRO_HW_FRAME_BUFFER_VALID : NULL,
                vk_frame_width() ? vk_frame_width() : 640,
                vk_frame_height() ? vk_frame_height() : 480, 0);
    else
#endif
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP)
        video_cb(libretro_swap_buffer ? RETRO_HW_FRAME_BUFFER_VALID : NULL, 640, 480, 0);
    else
#endif
    {
        extern void angrylion_present(bool new_frame);
        angrylion_present(libretro_swap_buffer);
    }
}
void retro_reset(void)
{
    if (g_EmulatorRunning) main_reset(0);
}

void *retro_get_memory_data(unsigned type)
{
    if (!load_game_successful) return NULL;
    switch (type)
    {
    case RETRO_MEMORY_SYSTEM_RAM:
        return g_dev.rdram.dram;
    case RETRO_MEMORY_TRANSFERPAK:
    case RETRO_MEMORY_DD:
    case RETRO_MEMORY_SAVE_RAM:
        return &saved_memory;
    }
    return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
    if (!load_game_successful) return 0;
    switch (type)
    {
    case RETRO_MEMORY_SYSTEM_RAM:
        return RDRAM_MAX_SIZE;
    case RETRO_MEMORY_TRANSFERPAK:
    case RETRO_MEMORY_DD:
    case RETRO_MEMORY_SAVE_RAM:
        return sizeof(saved_memory);
    }

    return 0;
}

size_t retro_serialize_size(void)
{
    return 16788288 + 1024 + 4 + RDP_EXTRA_STATE_BYTES;
}

bool retro_serialize(void *data, size_t size)
{
    if (!g_EmulatorRunning || !data || size < retro_serialize_size()) return false;
    return main_statesave(data) != 0;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (!g_EmulatorRunning || !data || size < 44) return false;
    const unsigned char* header = data;
    unsigned version = ((unsigned)header[8] << 24) | ((unsigned)header[9] << 16) |
                       ((unsigned)header[10] << 8) | header[11];
    size_t extra = version >= 0x00011a00 ? RDP_EXTRA_STATE_BYTES : version >= 0x00010e00 ? 151552 : version >= 0x00010d00 ? 20480 : version >= 0x00010c00 ? 16384 :
                   version >= 0x00010b00 ? 8192 : version >= 0x00010200 ? 4096 : 0;
    if (size < 16788288 + 1024 + 4 + extra) return false;
    return main_stateload(data) != 0;
}

// Needed to be able to detach controllers for Lylat Wars multiplayer
// Only sets if controller struct is initialised as addon paks do.
void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
    if (in_port < 4)
    {
        pad_present[in_port] = device != RETRO_DEVICE_NONE;
        switch (device)
        {
        case RETRO_DEVICE_NONE:
            if (controller[in_port].control)
            {
                controller[in_port].control->Present = 0;
                break;
            }
            else
            {
                pad_present[in_port] = 0;
                break;
            }

        case RETRO_DEVICE_JOYPAD:
        default:
            if (controller[in_port].control)
            {
                controller[in_port].control->Present = 1;
                break;
            }
            else
            {
                pad_present[in_port] = 1;
                break;
            }
        }
    }
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *codeLine)
{
}

void retro_return(void)
{
}

static int GamesharkActive = 0;

int event_gameshark_active(void)
{
    return GamesharkActive;
}

void event_set_gameshark(int active)
{
    // if boolean value doesn't change then just return
    if (!active == !GamesharkActive)
        return;

    // set the button state
    GamesharkActive = (active ? 1 : 0);

    // notify front-end application that gameshark button state has changed

}

















void renderer_process_display_list(void)
{
}

void renderer_process_commands(void)
{
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL) { vk_process_commands(); return; }
#endif
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) { glrdp_process_commands(); return; }
#endif
    n64video_process_list();
}

void renderer_close_content(void)
{
    if (!renderer_open) return;
    renderer_open = false;
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL) { vk_destroy(); return; }
#endif
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) {
        glrdp_close();
        return;
    }
#endif
    n64video_close();
}

int renderer_open_content(void)
{
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL)
    {
        renderer_open = true;
        if (vk_init()) return 1;
        if (log_cb) log_cb(RETRO_LOG_ERROR, "paraLLEl-RDP: Vulkan renderer initialization failed.\n");
        return 0;
    }
#endif
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) {
        if (!glrdp_open()) return 0;
        renderer_open = true;
        return 1;
    }
#endif
    struct n64video_config config;
    n64video_config_init(&config);
    config.parallel = true;
    config.num_workers = renderer_settings.threads;
    config.upscale = renderer_settings.upscale;
    config.dithering = true;
    config.vi.mode = renderer_settings.vi_filter ? VI_MODE_NORMAL : VI_MODE_COLOR;
    config.vi.vi_dedither = renderer_settings.dedither;
    config.vi.vi_blur = renderer_settings.blur;
    config.vi.hide_overscan = renderer_settings.overscan;
    config.vi.bob_deinterlace = renderer_settings.bob;
    config.gfx.rdram = gfx_info.RDRAM;
    config.gfx.rdram_size = *gfx_info.RDRAM_SIZE;
    config.gfx.dmem = gfx_info.DMEM;
    config.gfx.mi_intr_reg = gfx_info.MI_INTR_REG;
    config.gfx.mi_intr_cb = gfx_info.CheckInterrupts;
    config.gfx.dp_reg = (uint32_t **)&gfx_info.DPC_START_REG;
    config.gfx.vi_reg = (uint32_t **)&gfx_info.VI_STATUS_REG;
    n64video_init(&config);
    n64video_set_native_lod(renderer_settings.native_lod);
    if (log_cb) {
        extern uint32_t parallel_num_workers(void);
        log_cb(RETRO_LOG_INFO, "Angrylion: %u rendering workers, adaptive synchronization.\n",
                parallel_num_workers());
    }
    renderer_open = true;

    return 1;
}

void renderer_present(void)
{
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL) { vk_rasterize(); return; }
#endif
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) { glrdp_present(); return; }
#endif
    n64video_update_screen();
}

void renderer_show_framebuffer(void)
{
    renderer_present();
}

void renderer_vi_status_changed(void) {}

void renderer_vi_width_changed(void) {}

void renderer_framebuffer_write(unsigned int addr, unsigned int size) {
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) glrdp_fb_write(addr, size);
#endif
}

void renderer_framebuffer_read(unsigned int addr) {
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) glrdp_fb_read(addr);
#endif
}

void renderer_framebuffer_info(void *pinfo) {
    memset(pinfo, 0, 6 * sizeof(core_framebuffer_info));
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) glrdp_fb_info(pinfo);
#endif
}

int renderer_framebuffer_pages(uint8_t *pages, unsigned count) {
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) return glrdp_fb_pages(pages, count);
#endif
    (void)pages; (void)count;
    return 0;
}

void renderer_framebuffer_read_range(unsigned address, unsigned size) {
#ifdef HAVE_GLRDP
    if (renderer_settings.renderer == RENDERER_GLRDP) { glrdp_fb_read_range(address, size); return; }
#endif
    (void)size;
    renderer_framebuffer_read(address);
}


#if defined(PARALLAX_HEADLESS_BENCHMARK)
/* Sample at the video callback, before the next field changes VI registers. */
CORE_EXPORT void retro_debug_vi(uint32_t out[14])
{
    memcpy(out, g_dev.vi.regs, sizeof(g_dev.vi.regs));
}

/* Read-only timing snapshot used by benchmarks/headless/benchmark.c. */
/* Observe a physical RAM word through the matching cached KSEG0 line without
 * filling, flushing or charging the guest cache. NULL output probes support. */
CORE_EXPORT bool retro_debug_peek_word(uint32_t address, uint32_t *out)
{
    struct r4300_core *r = &g_dev.r4300;
    if ((address & 3) || address >= 0x800000) return false;
    if (out) {
        unsigned index = (address >> 4) & 511;
        if (r->cache_timing == 1 &&
            (r->dcache_tags[index] & ~2u) == ((address & 0x1ffff000) | 1))
            *out = r->dcache_words[index][(address >> 2) & 3];
        else *out = g_dev.rdram.dram[address >> 2];
    }
    return true;
}

CORE_EXPORT void retro_debug_timing(uint32_t out[12])
{
    struct r4300_core *r = &g_dev.r4300;
    struct cp0 *c = &r->cp0;

    out[0] = *r4300_pc(r);
    out[1] = c->regs[CP0_COUNT_REG];
    out[2] = (uint32_t)c->cycle_count;
    out[3] = c->next_interrupt;
    out[4] = c->last_addr;
    out[5] = r->delay_slot;
    out[6] = c->q.first ? (uint32_t)c->q.first->data.type : UINT32_C(0xffffffff);
    out[7] = c->q.first ? c->q.first->data.count : 0;
    out[8] = c->regs[CP0_CAUSE_REG];
    out[9] = c->regs[CP0_STATUS_REG];
    out[10] = r->emumode;
    out[11] = c->interrupt_unsafe_state;
}
#endif
