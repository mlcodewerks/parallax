#include "vdac.h"
#include <libretro.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;
extern retro_log_printf_t log_cb;
extern bool libretro_swap_buffer;

static struct frame_buffer frame;
static unsigned width = 640, height = 480;

void vdac_init(struct n64video_config *config) { (void)config; }
void vdac_read(struct frame_buffer *fb, bool alpha) { (void)fb; (void)alpha; }
/* Render into Angrylion's own buffer, then copy at presentation time. This
 * preserves interlaced fields across frontend buffers with unspecified contents. */
struct rgba *vdac_acquire(uint32_t w, uint32_t h, uint32_t *pitch, bool read)
{
    (void)w; (void)h; (void)pitch; (void)read;
    return NULL;
}
void vdac_write(struct frame_buffer *fb) { frame = *fb; }
void vdac_sync(bool invalid)
{
    if (!invalid && frame.pixels && frame.width && frame.height)
        libretro_swap_buffer = true;
}
void vdac_close(void)
{
    memset(&frame, 0, sizeof(frame));
    width = 640;
    height = 480;
    libretro_swap_buffer = false;
}

void angrylion_present(bool new_frame)
{
    struct retro_framebuffer fb = {0};
    const void *pixels;
    size_t pitch;
    unsigned y;
    if (!new_frame || !frame.pixels)
    {
        video_cb(NULL, width, height, 0);
        return;
    }
    width = frame.width;
    height = frame.height;
    pixels = frame.pixels;
    pitch = (size_t)frame.pitch * sizeof(struct rgba);
    fb.width = width;
    fb.height = height;
    fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) &&
        fb.data && fb.width == width && fb.height == height &&
        fb.format == RETRO_PIXEL_FORMAT_XRGB8888 &&
        fb.pitch >= (size_t)width * sizeof(struct rgba))
    {
        for (y = 0; y < height; ++y)
            memcpy((uint8_t *)fb.data + y * fb.pitch,
                   (const uint8_t *)pixels + y * pitch,
                   (size_t)width * sizeof(struct rgba));
        pixels = fb.data;
        pitch = fb.pitch;
    }
    video_cb(pixels, width, height, pitch);
}

#define DEFINE_MESSAGE(name, level) \
void name(const char *fmt, ...) \
{ \
    char text[2048]; \
    va_list args; \
    va_start(args, fmt); \
    vsnprintf(text, sizeof(text), fmt, args); \
    va_end(args); \
    if (log_cb) log_cb(level, "%s\n", text); \
}
DEFINE_MESSAGE(msg_error, RETRO_LOG_ERROR)
DEFINE_MESSAGE(msg_warning, RETRO_LOG_WARN)
DEFINE_MESSAGE(msg_debug, RETRO_LOG_DEBUG)
