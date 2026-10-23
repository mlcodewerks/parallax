#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
bool glrdp_context_reset(const struct retro_hw_render_callback *hw);
void glrdp_context_destroy(void);
bool glrdp_open(void);
void glrdp_close(void);
void glrdp_process_commands(void);
void glrdp_present(void);
void glrdp_begin_frame(void);
void glrdp_end_frame(void);
void glrdp_fb_read(uint32_t address);
void glrdp_fb_write(uint32_t address, uint32_t size);
void glrdp_fb_info(void *info);
bool glrdp_fb_pages(uint8_t *pages, unsigned count);
void glrdp_fb_read_range(uint32_t address, uint32_t size);
#ifdef __cplusplus
}
#endif
