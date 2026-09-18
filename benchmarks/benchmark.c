#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "libretro.h"

static const char *g_directory = ".";
static uint64_t g_vi_count;
static uint64_t g_audio_frames;
static uint64_t g_audio_hash = UINT64_C(1469598103934665603);

static uint64_t fnv1a_update(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static uint64_t fnv1a(const void *data, size_t size)
{
    return fnv1a_update(UINT64_C(1469598103934665603), data, size);
}

static void log_callback(enum retro_log_level level, const char *format, ...)
{
    va_list args;
    (void)level;

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

static bool environment_callback(unsigned command, void *data)
{
    switch (command)
    {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = g_directory;
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *)data)->log = log_callback;
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            return true;

        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
            *(const void **)data = NULL;
            return false;

        case RETRO_ENVIRONMENT_GET_VARIABLE:
            ((struct retro_variable *)data)->value = NULL;
            return false;

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
            return false;

        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned *)data = 1;
            return true;

        default:
            return false;
    }
}

static void video_callback(const void *data, unsigned width, unsigned height, size_t pitch)
{
    (void)data;
    (void)width;
    (void)height;
    (void)pitch;
    ++g_vi_count;
}

static size_t audio_batch_callback(const int16_t *data, size_t frames)
{
    g_audio_frames += frames;
    g_audio_hash = fnv1a_update(g_audio_hash, data, frames * 2 * sizeof(*data));
    return frames;
}

static void audio_callback(int16_t left, int16_t right)
{
    const int16_t samples[2] = {left, right};

    ++g_audio_frames;
    g_audio_hash = fnv1a_update(g_audio_hash, samples, sizeof(samples));
}

static void input_poll_callback(void)
{
}

static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;
}

#define LOAD_SYMBOL(symbol)                                                                  \
    do                                                                                        \
    {                                                                                         \
        *(void **)(&(symbol)) = dlsym(core, #symbol);                                         \
        if (!(symbol))                                                                        \
        {                                                                                     \
            fprintf(stderr, "missing %s: %s\n", #symbol, dlerror());                        \
            return 2;                                                                         \
        }                                                                                     \
    } while (0)

int main(int argc, char **argv)
{
    const char *bench_dir;
    const char *core_path;
    const char *rom_path;
    uint64_t target_vi;
    uint64_t interval;
    FILE *rom_file;
    long rom_size;
    void *rom_data;
    void *core;
    size_t ram_size;
    void *ram = NULL;
    uint64_t last_logged_vi = UINT64_MAX;
    char save_dir[512];

    void (*retro_set_environment)(retro_environment_t) = NULL;
    void (*retro_set_video_refresh)(retro_video_refresh_t) = NULL;
    void (*retro_set_audio_sample)(retro_audio_sample_t) = NULL;
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
    void (*retro_set_input_poll)(retro_input_poll_t) = NULL;
    void (*retro_set_input_state)(retro_input_state_t) = NULL;
    void (*retro_init)(void) = NULL;
    void (*retro_deinit)(void) = NULL;
    bool (*retro_load_game)(const struct retro_game_info *) = NULL;
    void (*retro_unload_game)(void) = NULL;
    void (*retro_run)(void) = NULL;
    void *(*retro_get_memory_data)(unsigned) = NULL;
    size_t (*retro_get_memory_size)(unsigned) = NULL;
    void (*retro_debug_timing)(uint32_t[12]) = NULL;

    if (argc < 5)
    {
        fprintf(stderr, "usage: %s core rom target_vi interval [out.csv]\n", argv[0]);
        return 2;
    }

    core_path = argv[1];
    rom_path = argv[2];
    target_vi = strtoull(argv[3], NULL, 0);
    interval = strtoull(argv[4], NULL, 0);

    if (interval == 0)
    {
        fprintf(stderr, "interval must be greater than zero\n");
        return 2;
    }

    if (argc > 5 && !freopen(argv[5], "w", stdout))
    {
        perror("output");
        return 2;
    }

    bench_dir = getenv("M64P_BENCH_DIR");
    if (bench_dir && *bench_dir)
        g_directory = bench_dir;

    mkdir(g_directory, 0777);
    snprintf(save_dir, sizeof(save_dir), "%s/Mupen64plus", g_directory);
    mkdir(save_dir, 0777);

    rom_file = fopen(rom_path, "rb");
    if (!rom_file)
    {
        perror("rom");
        return 2;
    }

    fseek(rom_file, 0, SEEK_END);
    rom_size = ftell(rom_file);
    fseek(rom_file, 0, SEEK_SET);

    if (rom_size <= 0)
    {
        fclose(rom_file);
        fprintf(stderr, "invalid ROM size\n");
        return 2;
    }

    rom_data = malloc((size_t)rom_size);
    if (!rom_data || fread(rom_data, 1, (size_t)rom_size, rom_file) != (size_t)rom_size)
    {
        fclose(rom_file);
        free(rom_data);
        fprintf(stderr, "failed to read ROM\n");
        return 2;
    }
    fclose(rom_file);

    core = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!core)
    {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        free(rom_data);
        return 2;
    }

    LOAD_SYMBOL(retro_set_environment);
    LOAD_SYMBOL(retro_set_video_refresh);
    LOAD_SYMBOL(retro_set_audio_sample);
    LOAD_SYMBOL(retro_set_audio_sample_batch);
    LOAD_SYMBOL(retro_set_input_poll);
    LOAD_SYMBOL(retro_set_input_state);
    LOAD_SYMBOL(retro_init);
    LOAD_SYMBOL(retro_deinit);
    LOAD_SYMBOL(retro_load_game);
    LOAD_SYMBOL(retro_unload_game);
    LOAD_SYMBOL(retro_run);
    LOAD_SYMBOL(retro_get_memory_data);
    LOAD_SYMBOL(retro_get_memory_size);
    LOAD_SYMBOL(retro_debug_timing);

    retro_set_environment(environment_callback);
    retro_set_video_refresh(video_callback);
    retro_set_audio_sample(audio_callback);
    retro_set_audio_sample_batch(audio_batch_callback);
    retro_set_input_poll(input_poll_callback);
    retro_set_input_state(input_state_callback);
    retro_init();

    {
        const struct retro_game_info game = {
            rom_path,
            rom_data,
            (size_t)rom_size,
            NULL,
        };

        if (!retro_load_game(&game))
        {
            fprintf(stderr, "load failed\n");
            retro_deinit();
            dlclose(core);
            free(rom_data);
            return 3;
        }
    }

    ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    fprintf(stderr, "loaded ram=%zu target=%llu interval=%llu\n", ram_size,
            (unsigned long long)target_vi, (unsigned long long)interval);

    puts("vi,ram_fnv64,audio_frames,audio_fnv64,pc,cp0_count,cycle_count,next_interrupt,"
         "last_addr,delay_slot,q_type,q_count,cause,status,emumode,unsafe");

    while (g_vi_count < target_vi)
    {
        retro_run();

        if (!ram)
            ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);

        if (g_vi_count && g_vi_count != last_logged_vi &&
            ((g_vi_count % interval) == 0 || g_vi_count == target_vi))
        {
            const uint64_t ram_hash = getenv("NO_RAM_HASH") ? 0 : (ram ? fnv1a(ram, ram_size) : 0);
            uint32_t timing[12] = {0};

            retro_debug_timing(timing);
            printf("%llu,%016llx,%llu,%016llx,%08x,%u,%d,%u,%08x,%u,%u,%u,%08x,%08x,%u,%u\n",
                   (unsigned long long)g_vi_count, (unsigned long long)ram_hash,
                   (unsigned long long)g_audio_frames, (unsigned long long)g_audio_hash,
                   timing[0], timing[1], (int32_t)timing[2], timing[3], timing[4], timing[5],
                   timing[6], timing[7], timing[8], timing[9], timing[10], timing[11]);
            fflush(stdout);
            last_logged_vi = g_vi_count;
        }
    }

    fprintf(stderr, "done vis=%llu audio=%llu\n", (unsigned long long)g_vi_count,
            (unsigned long long)g_audio_frames);

    retro_unload_game();
    retro_deinit();
    free(rom_data);
    dlclose(core);
    return 0;
}
