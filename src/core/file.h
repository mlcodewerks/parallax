#ifndef PARALLAX_FILE_H
#define PARALLAX_FILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct core_file core_file;
bool core_file_init(retro_environment_t environment);
core_file *core_file_open(const char *path, const char *mode);
int core_file_close(core_file *file);
int64_t core_file_size(core_file *file);
int64_t core_file_tell(core_file *file);
int64_t core_file_seek_position(core_file *file, int64_t offset, int origin);
int core_file_seek(core_file *file, int64_t offset, int origin);
int64_t core_file_read(core_file *file, void *data, int64_t size);
int64_t core_file_write(core_file *file, const void *data, int64_t size);
int core_file_flush(core_file *file);
int core_file_error(core_file *file);
void core_file_clear_error(core_file *file);
int core_file_getc(core_file *file);
char *core_file_gets(char *buffer, int size, core_file *file);
int core_file_printf(core_file *file, const char *format, ...);
size_t core_file_read_items(void *data, size_t size, size_t count, core_file *file);
size_t core_file_write_items(const void *data, size_t size, size_t count, core_file *file);
bool core_file_read_exact(core_file *file, void *data, size_t size);
bool core_file_write_exact(core_file *file, const void *data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
