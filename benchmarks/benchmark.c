#define _GNU_SOURCE
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define mkdir(path, mode) _mkdir(path)
static void* dlopen(const char* path, int flags) { (void)flags; return (void*)LoadLibraryA(path); }
static void* dlsym(void* core, const char* name) { return (void*)GetProcAddress((HMODULE)core, name); }
static void dlclose(void* core) { FreeLibrary((HMODULE)core); }
static const char* dlerror(void)
{
    static char message[64];
    snprintf(message, sizeof(message), "Windows loader error %lu", (unsigned long)GetLastError());
    return message;
}
#else
#include <dlfcn.h>
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "libretro.h"
#include "gl_context.h"
#include <string.h>
#include "input_script.h"

static const char *g_directory = ".";
static uint64_t g_vi_count;
static void (*g_debug_vi)(uint32_t[14]);
static FILE *g_vi_trace;

static double benchmark_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
#endif
}
static uint64_t g_video_hash;
static const char *g_capture;
static unsigned g_capture_interval = 300;
static uint64_t g_capture_force_at;
static uint64_t g_gameplay_start;
static unsigned g_video_width, g_video_height;
static uint8_t *g_video_rgb;
static size_t g_video_capacity;
static uint64_t g_audio_frames;
static uint64_t g_audio_hash = UINT64_C(1469598103934665603);
static uint64_t g_audio_interval_frames;
static uint64_t g_audio_interval_hash = UINT64_C(1469598103934665603);

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

#ifdef _WIN32
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return bench_request_gl((struct retro_hw_render_callback *)data);
#endif
        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
            *(const void **)data = NULL;
            return false;

        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            struct retro_variable* variable = (struct retro_variable*)data;
            variable->value = getenv(variable->key);
            return variable->value != NULL;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
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
    size_t count;
    unsigned x, y;
    ++g_vi_count;
    if (g_vi_trace && g_debug_vi) {
        uint32_t regs[14];
        g_debug_vi(regs);
        fprintf(g_vi_trace, "%llu", (unsigned long long)g_vi_count);
        for (unsigned i=0; i<14; ++i) fprintf(g_vi_trace, ",%08x", regs[i]);
        fputc('\n', g_vi_trace);
    }
    if (!g_capture) return;
    /* A NULL callback repeats the displayed image; some cores report zero
     * dimensions on that callback. Keep the preceding display dimensions. */
    if (!data && (!width || !height)) {width=g_video_width;height=g_video_height;}
    if (!width || !height) return;
    if (width > 2048 || height > 2048) { fprintf(stderr, "capture dimensions exceed limit\n"); exit(4); }
    count = (size_t)width * height * 3;
    if (count > g_video_capacity) {
        g_video_rgb = (uint8_t *)realloc(g_video_rgb, count);
        if (!g_video_rgb) exit(4);
        g_video_capacity = count;
    }
    if (data && data != RETRO_HW_FRAME_BUFFER_VALID) {
        for (y = 0; y < height; ++y) {
            const uint8_t *row = (const uint8_t *)data + y * pitch;
            for (x = 0; x < width; ++x) {
                uint8_t *out = g_video_rgb + ((size_t)y * width + x) * 3;
                out[0] = row[x*4+2]; out[1] = row[x*4+1]; out[2] = row[x*4];
            }
        }
        g_video_width = width; g_video_height = height;
    }
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {g_video_width=width;g_video_height=height;}
    if (g_vi_count != g_capture_force_at &&
        (g_vi_count < g_gameplay_start || (g_vi_count-g_gameplay_start) % g_capture_interval)) return;
#ifdef _WIN32
    if (bench_gl_active) {
        uint8_t *rgba = (uint8_t *)malloc((size_t)width * height * 4);
        if (!rgba) exit(4);
        bench_bind_fb(GL_FRAMEBUFFER, bench_fbo);
        bench_pack(GL_PACK_ALIGNMENT, 1);
        bench_read(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        for (y=0; y<height; ++y) for (x=0; x<width; ++x)
            memcpy(g_video_rgb + ((size_t)y*width+x)*3, rgba + ((size_t)(height-1-y)*width+x)*4, 3);
        free(rgba);
        g_video_width=width; g_video_height=height;
    }
#endif
    if (g_video_width && g_video_height) {
        char path[1024]; FILE *file;
        snprintf(path,sizeof(path),"%s-%06llu.ppm",g_capture,(unsigned long long)g_vi_count);
        file=fopen(path,"wb");
        if (!file) { fprintf(stderr,"capture open failed: %s\n",path); exit(4); }
        count=(size_t)g_video_width*g_video_height*3;
        fprintf(file,"P6\n%u %u\n255\n",g_video_width,g_video_height);
        if (fwrite(g_video_rgb,1,count,file)!=count) exit(4);
        fclose(file);
        g_video_hash=fnv1a(g_video_rgb,count);
    }
}

static size_t audio_batch_callback(const int16_t *data, size_t frames)
{
    g_audio_frames += frames;
    g_audio_hash = fnv1a_update(g_audio_hash, data, frames * 2 * sizeof(*data));
    g_audio_interval_frames += frames;
    g_audio_interval_hash = fnv1a_update(g_audio_interval_hash, data, frames * 2 * sizeof(*data));
    return frames;
}

static void audio_callback(int16_t left, int16_t right)
{
    const int16_t samples[2] = {left, right};

    ++g_audio_frames;
    g_audio_hash = fnv1a_update(g_audio_hash, samples, sizeof(samples));
    ++g_audio_interval_frames;
    g_audio_interval_hash = fnv1a_update(g_audio_interval_hash, samples, sizeof(samples));
}

static void input_poll_callback(void)
{
}

static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if(port>=4)return 0;
    if(device==RETRO_DEVICE_JOYPAD) {
        if(id==RETRO_DEVICE_ID_JOYPAD_MASK)return (int16_t)bench_input[port].buttons;
        if(id<16)return (bench_input[port].buttons>>id)&1;
    }
    if(device==RETRO_DEVICE_ANALOG&&index==RETRO_DEVICE_INDEX_ANALOG_LEFT)
        return id==RETRO_DEVICE_ID_ANALOG_X?bench_input[port].x:
            (id==RETRO_DEVICE_ID_ANALOG_Y?bench_input[port].y:0);
    return 0;
}

/* Interactive exploration records the exact same intervals later replayed by
 * every renderer. Waiting happens between retro_run calls, never inside a VI. */
static bool control_step(FILE *control,FILE *record,const char *path,uint64_t *end)
{
    char line[256],status_path[4096];
    snprintf(status_path,sizeof(status_path),"%s.status",path);
    FILE *status=fopen(status_path,"w");
    if(!status) {perror("control status");exit(4);}
    fprintf(status,"%llu\n",(unsigned long long)g_vi_count);fclose(status);
    for(;;) {
        long position=ftell(control);clearerr(control);
        if(fgets(line,sizeof(line),control)) {
            if(!strchr(line,'\n')) {fseek(control,position,SEEK_SET);}
            else if(!strncmp(line,"stop",4))return false;
            else {
                unsigned count,buttons,port;int x,y;char extra;
                if(sscanf(line,"%u,%u,%d,%d,%u %c",&count,&buttons,&x,&y,&port,&extra)!=5||
                    !count||count>1000000||buttons>65535||port>3||x<-32767||x>32767||y<-32767||y>32767) {
                    fprintf(stderr,"invalid control step: %s",line);exit(4);
                }
                memset(bench_input,0,sizeof(bench_input));
                bench_input[port]=(struct bench_input_event){g_vi_count,g_vi_count+count,(uint16_t)buttons,(int16_t)x,(int16_t)y,port};
                *end=g_vi_count+count;g_capture_force_at=*end;
                fprintf(record,"%llu,%llu,%u,%d,%d,%u\n",(unsigned long long)g_vi_count,
                    (unsigned long long)*end,buttons,x,y,port);fflush(record);
                return true;
            }
        }
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
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
    const char *control_path=getenv("M64P_BENCH_CONTROL");
    FILE *control=NULL,*input_record=NULL;
    uint64_t control_end=0;
    uint64_t state_vi=getenv("M64P_BENCH_STATE_ROUNDTRIP") ? strtoull(getenv("M64P_BENCH_STATE_ROUNDTRIP"),NULL,0) : 0;
    bool state_done=false;

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
    size_t (*retro_serialize_size)(void) = NULL;
    bool (*retro_serialize)(void *, size_t) = NULL;
    bool (*retro_unserialize)(const void *, size_t) = NULL;

    if (argc < 5)
    {
        fprintf(stderr, "usage: %s core rom target_vi interval [out.csv]\n", argv[0]);
        return 2;
    }

    g_capture = getenv("M64P_BENCH_CAPTURE");
    if (getenv("M64P_BENCH_CAPTURE_INTERVAL")) g_capture_interval = (unsigned)strtoul(getenv("M64P_BENCH_CAPTURE_INTERVAL"),NULL,10);
    if (!g_capture_interval) return 2;
    if(getenv("M64P_BENCH_INPUT")&&control_path) {fprintf(stderr,"input replay and interactive control are mutually exclusive\n");return 2;}
    if(getenv("M64P_BENCH_INPUT")&&!bench_input_load(getenv("M64P_BENCH_INPUT")))return 2;
    if(control_path) {
        char path[4096];control=fopen(control_path,"r");
        snprintf(path,sizeof(path),"%s.input.csv",control_path);input_record=fopen(path,"w");
        if(!control||!input_record) {perror("control file");return 2;}
        fprintf(input_record,"start_vi,end_vi,buttons,analog_x,analog_y,port\n");fflush(input_record);
    }
    core_path = argv[1];
    rom_path = argv[2];
    target_vi = strtoull(argv[3], NULL, 0);
    g_capture_force_at=target_vi;
    if(getenv("M64P_BENCH_GAMEPLAY_START"))g_gameplay_start=strtoull(getenv("M64P_BENCH_GAMEPLAY_START"),NULL,0);
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
    if (getenv("M64P_BENCH_VI_TRACE")) {
        g_debug_vi = (void (*)(uint32_t[14]))dlsym(core, "retro_debug_vi");
        g_vi_trace = fopen(getenv("M64P_BENCH_VI_TRACE"), "w");
        if (!g_debug_vi || !g_vi_trace) { fprintf(stderr, "VI trace unavailable\n"); return 4; }
        fputs("vi,status,origin,width,intr,current,burst,vsync,hsync,leap,hstart,vstart,vburst,xscale,yscale\n", g_vi_trace);
    }
    if(state_vi) {
        LOAD_SYMBOL(retro_serialize_size);
        LOAD_SYMBOL(retro_serialize);
        LOAD_SYMBOL(retro_unserialize);
    }

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

    bench_gl_reset();
    if (!bench_gl_check()) return 4;
    ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    fprintf(stderr, "loaded ram=%zu target=%llu interval=%llu\n", ram_size,
            (unsigned long long)target_vi, (unsigned long long)interval);

    puts("vi,ram_fnv64,audio_frames,audio_fnv64,pc,cp0_count,cycle_count,next_interrupt,"
         "last_addr,delay_slot,q_type,q_count,cause,status,emumode,unsafe,video_fnv64,"
         "audio_interval_frames,audio_interval_fnv64");

    FILE *frame_times = NULL;
    const char *frame_times_path = getenv("M64P_BENCH_FRAME_TIMES");
    const bool gpu_sync = getenv("M64P_BENCH_GPU_SYNC") != NULL;
    if (frame_times_path) {
        frame_times = fopen(frame_times_path, "w");
        if (!frame_times) { fprintf(stderr, "frame timing open failed\n"); return 4; }
        fputs("start_vi,end_vi,retro_run_ms\n", frame_times);
    }
    while (g_vi_count < target_vi)
    {
        if(control) {
            if(g_vi_count>=control_end&&!control_step(control,input_record,control_path,&control_end))break;
        }else bench_input_at(g_vi_count);
        const uint64_t start_vi = g_vi_count;
        const double frame_start = frame_times ? benchmark_seconds() : 0.0;
        retro_run();
        if (gpu_sync) bench_gl_finish();
        if(state_vi&&!state_done&&g_vi_count>=state_vi) {
            size_t bytes=retro_serialize_size();void *state=malloc(bytes);
            if(!bytes||!state||!retro_serialize(state,bytes)||!retro_unserialize(state,bytes)) {
                free(state);fprintf(stderr,"savestate roundtrip failed at VI %llu\n",(unsigned long long)g_vi_count);return 4;
            }
            free(state);state_done=true;
            fprintf(stderr,"savestate roundtrip at VI %llu: %zu bytes\n",(unsigned long long)g_vi_count,bytes);
        }
        if (frame_times)
            fprintf(frame_times, "%llu,%llu,%.6f\n", (unsigned long long)start_vi,
                    (unsigned long long)g_vi_count, (benchmark_seconds()-frame_start)*1000.0);
        if (!bench_gl_check()) return 4;

        if (!ram)
            ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);

        if (g_vi_count && g_vi_count != last_logged_vi &&
            ((g_vi_count < g_gameplay_start ? g_vi_count % interval : (g_vi_count-g_gameplay_start) % interval) == 0 || g_vi_count == target_vi))
        {
            const uint64_t ram_hash = getenv("NO_RAM_HASH") ? 0 : (ram ? fnv1a(ram, ram_size) : 0);
            uint32_t timing[12] = {0};

            if (ram && g_capture && getenv("M64P_BENCH_RAM_DUMP") && g_vi_count % g_capture_interval == 0) {
                char path[4096];
                snprintf(path,sizeof(path),"%s-%06llu.ram",g_capture,(unsigned long long)g_vi_count);
                FILE *dump=fopen(path,"wb");
                if (!dump || fwrite(ram,1,ram_size,dump)!=ram_size) { fprintf(stderr,"RAM dump failed\n"); return 4; }
                fclose(dump);
            }
            retro_debug_timing(timing);
            printf("%llu,%016llx,%llu,%016llx,%08x,%u,%d,%u,%08x,%u,%u,%u,%08x,%08x,%u,%u,%016llx,%llu,%016llx\n",
                   (unsigned long long)g_vi_count, (unsigned long long)ram_hash,
                   (unsigned long long)g_audio_frames, (unsigned long long)g_audio_hash,
                   timing[0], timing[1], (int32_t)timing[2], timing[3], timing[4], timing[5],
                   timing[6], timing[7], timing[8], timing[9], timing[10], timing[11], (unsigned long long)g_video_hash, (unsigned long long)g_audio_interval_frames, (unsigned long long)g_audio_interval_hash);
            g_audio_interval_frames=0;g_audio_interval_hash=UINT64_C(1469598103934665603);
            fflush(stdout);
            last_logged_vi = g_vi_count;
        }
    }

    if (frame_times) fclose(frame_times);
    if (g_vi_trace) { fclose(g_vi_trace); g_vi_trace = NULL; }
    fprintf(stderr, "done vis=%llu audio=%llu\n", (unsigned long long)g_vi_count,
            (unsigned long long)g_audio_frames);

    retro_unload_game();
    bench_gl_close();
    free(g_video_rgb);
    if(control)fclose(control);
    if(input_record)fclose(input_record);
    free(bench_events);
    retro_deinit();
    free(rom_data);
    dlclose(core);
    return 0;
}
