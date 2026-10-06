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

#include "mupen64plus-next_common.h"
#include "device/rcp/rdp/angrylion/n64video.h"
#include "renderer_options.h"
#include "device/rcp/rsp/optional_rsp.h"
#include "device/rcp/rdp/performance_cores.h"
#ifdef HAVE_PARALLEL_RDP
#include "device/rcp/rdp/para_intf.h"
#endif

#ifdef HAVE_LIBNX
#include <switch.h>
#endif
#include <pthread.h>

#include "api/m64p_frontend.h"
#include "api/m64p_types.h"
#include "device/r4300/r4300_core.h"
#include "device/memory.h"
#include "main/main.h"
#include "api/callbacks.h"
#include "main/version.h"
#include "main/util.h"
#include "main/mupen64plus.ini.h"
#include "api/m64p_config.h"
#include "osal_files.h"
#include "main/rom.h"
#include "plugin/plugin.h"
#include "device/rcp/pi_controller.h"
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

retro_log_printf_t log_cb = NULL;
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
retro_environment_t environ_clear_thread_waits_cb = NULL;

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
static bool renderer_open = false;
bool libretro_swap_buffer;

float retro_screen_aspect = 4.0 / 3.0;

static char rdp_plugin_last[32] = {0};

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

static bool emuThreadRunning = false;
static pthread_t emuThread;

// after the controller's CONTROL* member has been assigned we can update
// them straight from here...
extern struct
{
    CONTROL *control;
    BUTTONS buttons;
} controller[2];
// ...but it won't be at least the first time we're called, in that case set
// these instead for input_plugin to read.
int pad_pak_types[4];
int pad_present[4] = {1, 1, 1, 1};

static void n64DebugCallback(void *aContext, int aLevel, const char *aMessage)
{
    char buffer[1024];
    snprintf(buffer, 1024, CORE_NAME ": %s\n", aMessage);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, buffer);
}

extern m64p_rom_header ROM_HEADER;
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

static void n64StateCallback(void *Context, m64p_core_param param_type, int new_value)
{
}

static bool emu_step_load_data()
{
    m64p_error ret = CoreStartup(FRONTEND_API_VERSION, ".", ".", NULL, n64DebugCallback, 0, n64StateCallback);
    if (ret && log_cb)
        log_cb(RETRO_LOG_ERROR, CORE_NAME ": failed to initialize core (err=%i)\n", ret);

    log_cb(RETRO_LOG_DEBUG, CORE_NAME ": [EmuThread] M64CMD_ROM_OPEN\n");

    if (CoreDoCommand(M64CMD_ROM_OPEN, game_size, (void *)game_data))
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, CORE_NAME ": failed to load ROM\n");
        goto load_fail;
    }

    free(game_data);
    game_data = NULL;

    log_cb(RETRO_LOG_DEBUG, CORE_NAME ": [EmuThread] M64CMD_ROM_GET_HEADER\n");

    if (CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(ROM_HEADER), &ROM_HEADER))
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, CORE_NAME ": failed to query ROM header information\n");
        goto load_fail;
    }

    return true;

load_fail:
    free(game_data);
    game_data = NULL;
    // stop = 1;

    return false;
}

static void emu_step_initialize(void)
{
    if (emu_initialized)
        return;
    update_variables(true);
    emu_initialized = true;
    plugin_connect_all();
}

void EmuThreadFunction()
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
    const char *dir;
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
    environ_cb(RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB, &environ_clear_thread_waits_cb);

    setup_variables();
}

void retro_get_system_info(struct retro_system_info *info)
{
    info->library_name = "libgarbagefiar";
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
    info->timing.fps = vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
    info->timing.sample_rate = 44100.0;
}

unsigned retro_get_region(void)
{
    return ((ROM_PARAMS.systemtype == SYSTEM_PAL) ? RETRO_REGION_PAL : RETRO_REGION_NTSC);
}

void copy_file(char *ininame, char *fileName)
{
    const char *filename = ConfigGetSharedDataFilepath(fileName);
    FILE *fp = fopen(filename, "w");
    if (fp != NULL)
    {
        fputs(ininame, fp);
        fclose(fp);
    }
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

void retro_init(void)
{
    char *sys_pathname;
    wchar_t w_pathname[PATH_SIZE];
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_pathname);
    char pathname[PATH_SIZE];
    strncpy(pathname, sys_pathname, PATH_SIZE);
    if (pathname[(strlen(pathname) - 1)] != '/' && pathname[(strlen(pathname) - 1)] != '\\')
        strcat(pathname, "/");
    strcat(pathname, "Mupen64plus/");
    mbstowcs(w_pathname, pathname, PATH_SIZE);
    if (!osal_path_existsW(w_pathname) || !osal_is_directory(w_pathname))
        osal_mkdirp(w_pathname);
    copy_file(inifile, "mupen64plus.ini");

    struct retro_log_callback log;
    unsigned colorMode = RETRO_PIXEL_FORMAT_XRGB8888;

    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    else
        log_cb = NULL;

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

    rdp_plugin_last[0] = '\0';


}

void update_controllers()
{
    struct retro_variable pk1var = {CORE_NAME "-pak1"};
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk1var) && pk1var.value)
    {
        int p1_pak = PLUGIN_NONE;
        if (!strcmp(pk1var.value, "rumble"))
            p1_pak = PLUGIN_RAW;
        else if (!strcmp(pk1var.value, "memory"))
            p1_pak = PLUGIN_MEMPAK;
        else if (!strcmp(pk1var.value, "transfer"))
            p1_pak = PLUGIN_TRANSFER_PAK;

        // If controller struct is not initialised yet, set pad_pak_types instead
        // which will be looked at when initialising the controllers.
        if (controller[0].control)
            controller[0].control->Plugin = p1_pak;
        else
            pad_pak_types[0] = p1_pak;
    }

    struct retro_variable pk2var = {CORE_NAME "-pak2"};
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk2var) && pk2var.value)
    {
        int p2_pak = PLUGIN_NONE;
        if (!strcmp(pk2var.value, "rumble"))
            p2_pak = PLUGIN_RAW;
        else if (!strcmp(pk2var.value, "memory"))
            p2_pak = PLUGIN_MEMPAK;
        else if (!strcmp(pk2var.value, "transfer"))
            p2_pak = PLUGIN_TRANSFER_PAK;

        if (controller[1].control)
            controller[1].control->Plugin = p2_pak;
        else
            pad_pak_types[1] = p2_pak;
    }
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
    char *gamePath;
    char *newPath;
    renderer_read_options(environ_cb);
    current_rdp_type = renderer_settings.renderer == RENDERER_ANGRYLION ? RDP_PLUGIN_ANGRYLION : RDP_PLUGIN_PARALLEL;
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Renderer: %s, upscale %ux, performance cores %s.\n",
                renderer_settings.renderer == RENDERER_ANGRYLION ? "Angrylion" : "paraLLEl-RDP",
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
    initializing = true;

    // Workaround for broken subsystem on static platforms
    // Note: game->path can be NULL if loading from a archive
    // Current impl. uses mupen internals so that wouldn't work either way for dd/tpak
    // So we just sanity check
    if (!retro_dd_path_img && game->path)
    {
        gamePath = (char *)game->path;
        newPath = (char *)calloc(1, strlen(gamePath) + 5);
        strcpy(newPath, gamePath);
        strcat(newPath, ".ndd");
        FILE *fileTest = fopen(newPath, "r");
        if (!fileTest)
        {
            free(newPath);
        }
        else
        {
            fclose(fileTest);
            // Free'd later in Mupen Core
            retro_dd_path_img = newPath;
        }
    }

    if (!retro_transferpak_rom_path && game->path)
    {
        gamePath = (char *)game->path;
        newPath = (char *)calloc(1, strlen(gamePath) + 4);
        strcpy(newPath, gamePath);
        strcat(newPath, ".gb");
        FILE *fileTest = fopen(newPath, "r");
        if (!fileTest)
        {
            free(newPath);
        }
        else
        {
            fclose(fileTest);
            // Free'd later in Mupen Core
            retro_transferpak_rom_path = newPath;

            // We have a gb rom!
            if (!retro_transferpak_ram_path)
            {
                gamePath = (char *)game->path;
                newPath = (char *)calloc(1, strlen(gamePath) + 5);
                strcpy(newPath, gamePath);
                strcat(newPath, ".sav");
                FILE *fileTest = fopen(newPath, "r");
                if (!fileTest)
                {
                    free(newPath);
                }
                else
                {
                    fclose(fileTest);
                    // Free'd later in Mupen Core
                    retro_transferpak_ram_path = newPath;
                }
            }
        }
    }

    // Init default vals
    load_game_successful = false;

    format_saved_memory();
    init_audio_libretro(audio_buffer_size);
    sync_open();

    game_data = malloc(game->size);
    memcpy(game_data, game->data, game->size);
    game_size = game->size;

    if (!emu_step_load_data())
        return false;
    emu_step_initialize();

    load_game_successful = true;

    return true;
}

void retro_unload_game(void)
{
    optional_rsp_report();
    sync_close();

    /* ROM_CLOSE is rejected while the emulator is running. Stop plugins
     * first so closing content can release the decoded CPU cache too. */
    CoreDoCommand(M64CMD_STOP, 0, NULL);
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();

    cleanup_global_paths();

    emu_initialized = false;
    load_game_successful = false;
    initializing = true;
}

void update_variables(bool startup)
{
    if (startup)
    {

        EnableFrameDuping = 1;
        EnableFullspeed = 0;
        CountPerScanlineOverride = 0;
#if defined(M64P_HEADLESS_BENCH_PURE)
        r4300_emumode = EMUMODE_PURE_INTERPRETER;
#elif defined(M64P_HEADLESS_BENCHMARK)
        r4300_emumode = EMUMODE_INTERPRETER;
#else
        r4300_emumode = renderer_settings.cached_interpreter ? EMUMODE_INTERPRETER : EMUMODE_PURE_INTERPRETER;
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
    EmuThreadFunction();
    ai_flush_samples(&g_dev.ai);
    audio_end_frame_libretro(1.0 / vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype));
    sync_end();
    performance_core_leave(&emulation_affinity);

#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL)
        video_cb(libretro_swap_buffer ? RETRO_HW_FRAME_BUFFER_VALID : NULL,
                vk_frame_width() ? vk_frame_width() : 640,
                vk_frame_height() ? vk_frame_height() : 480, 0);
    else
#endif
    {
        extern void angrylion_present(bool new_frame);
        angrylion_present(libretro_swap_buffer);
    }
}
void retro_reset(void)
{
    CoreDoCommand(M64CMD_RESET, 0, (void *)0);
}

void *retro_get_memory_data(unsigned type)
{
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
    return 16788288 + 1024 + 4 + 151552;
}

bool retro_serialize(void *data, size_t size)
{
    if (!data || size < retro_serialize_size()) return false;
    return main_statesave(data) != 0;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (!data || size < 44) return false;
    const unsigned char* header = data;
    unsigned version = ((unsigned)header[8] << 24) | ((unsigned)header[9] << 16) |
                       ((unsigned)header[10] << 8) | header[11];
    size_t extra = version >= 0x00010e00 ? 151552 : version >= 0x00010d00 ? 20480 : version >= 0x00010c00 ? 16384 :
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
    StateChanged(M64CORE_INPUT_GAMESHARK, GamesharkActive);
}

void angrylionChangeWindow(void) {}

void angrylionReadScreen2(void *dest, int *width, int *height, int front) {}

void angrylionDrawScreen(void) {}

typedef struct
{
    uint16_t Version; /* Set to 0x0103 */
    uint16_t Type;    /* Set to PLUGIN_TYPE_GFX */
    char Name[100];   /* Name of the DLL */

    /* If DLL supports memory these memory options then set them to TRUE or FALSE
       if it does not support it */
    int NormalMemory;  /* a normal uint8_t array */
    int MemoryBswaped; /* a normal uint8_t array where the memory has been pre
                             bswap on a dword (32 bits) boundry */
} PLUGIN_INFO;

void angrylionGetDllInfo(PLUGIN_INFO *PluginInfo)
{
    PluginInfo->Version = 0x0103;
    PluginInfo->Type = 2;
    strcpy(
        PluginInfo->Name, "angrylion's RDP");
    PluginInfo->NormalMemory = true;
    PluginInfo->MemoryBswaped = true;
}

void angrylionSetRenderingCallback(void (*callback)(int)) {}

int angrylionInitiateGFX(GFX_INFO Gfx_Info)
{
    return 1;
}

void angrylionMoveScreen(int xpos, int ypos) {}

void angrylionProcessDList(void)
{
}

void angrylionProcessRDPList(void)
{
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL) { vk_process_commands(); return; }
#endif
    n64video_process_list();
}

void angrylionRomClosed(void)
{
    if (!renderer_open) return;
    renderer_open = false;
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL) { vk_destroy(); return; }
#endif
    n64video_close();
}

int angrylionRomOpen(void)
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

void angrylionUpdateScreen(void)
{
#ifdef HAVE_PARALLEL_RDP
    if (renderer_settings.renderer == RENDERER_PARALLEL) { vk_rasterize(); return; }
#endif
    n64video_update_screen();
}

void angrylionShowCFB(void)
{
    angrylionUpdateScreen();
}

void angrylionViStatusChanged(void) {}

void angrylionViWidthChanged(void) {}

void angrylionFBWrite(unsigned int addr, unsigned int size) {}

void angrylionFBRead(unsigned int addr) {}

void angrylionFBGetFrameBufferInfo(void *pinfo) {}

m64p_error angrylionPluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    /* set version info */
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_GFX;

    if (PluginVersion != NULL)
        *PluginVersion = 0x016304;

    if (APIVersion != NULL)
        *APIVersion = 0x020100;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "MAME/Angrylion/HatCat/ata4 video Plugin";

    if (Capabilities != NULL)
        *Capabilities = 0;

    return M64ERR_SUCCESS;
}
#if defined(M64P_HEADLESS_BENCHMARK)
/* Read-only timing snapshot used by benchmarks/headless/benchmark.c. */
EXPORT void retro_debug_timing(uint32_t out[12])
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
