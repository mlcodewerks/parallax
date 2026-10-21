/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "file.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
struct core_file {
    struct retro_vfs_file_handle *handle;
    struct retro_vfs_interface api;
    int error;
    bool append;
};
static struct retro_vfs_interface frontend;
static bool available;
bool core_file_init(retro_environment_t environment)
{
    struct retro_vfs_interface_info info = {1, NULL};
    available = false;
    memset(&frontend, 0, sizeof(frontend));
    if (!environment || !environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info) ||
        !info.iface || !info.iface->open || !info.iface->close ||
        !info.iface->size || !info.iface->tell || !info.iface->seek ||
        !info.iface->read || !info.iface->write || !info.iface->flush) return false;
    /* Read only v1 fields: a v1 frontend need not allocate later extensions. */
    frontend.open = info.iface->open; frontend.close = info.iface->close;
    frontend.size = info.iface->size; frontend.tell = info.iface->tell;
    frontend.seek = info.iface->seek; frontend.read = info.iface->read;
    frontend.write = info.iface->write; frontend.flush = info.iface->flush;
    available = true;
    return true;
}
core_file *core_file_open(const char *path, const char *mode)
{
    if (!available || !path || !*path || !mode) return NULL;
    unsigned access;
    switch (mode[0]) {
        case 'r': access = strchr(mode, '+') ? RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING : RETRO_VFS_FILE_ACCESS_READ; break;
        case 'w': access = strchr(mode, '+') ? RETRO_VFS_FILE_ACCESS_READ_WRITE : RETRO_VFS_FILE_ACCESS_WRITE; break;
        case 'a': access = (strchr(mode, '+') ? RETRO_VFS_FILE_ACCESS_READ_WRITE : RETRO_VFS_FILE_ACCESS_WRITE) |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING; break;
        default: return NULL;
    }
    core_file *file = calloc(1, sizeof(*file));
    if (!file) return NULL;
    file->api = frontend;
    file->append = mode[0] == 'a';
    file->handle = file->api.open(path, access, RETRO_VFS_FILE_ACCESS_HINT_NONE);
    if (!file->handle && mode[0] == 'a')
        file->handle = file->api.open(path, access & ~RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);
    if (!file->handle) { free(file); return NULL; }
    if (mode[0] == 'a' && core_file_seek(file, 0, SEEK_END)) { core_file_close(file); return NULL; }
    return file;
}
int core_file_close(core_file *file)
{
    if (!file) return -1;
    int result = file->api.close(file->handle);
    free(file);
    return result;
}
int64_t core_file_size(core_file *file) { return file ? file->api.size(file->handle) : -1; }
int64_t core_file_tell(core_file *file) { return file ? file->api.tell(file->handle) : -1; }
int64_t core_file_seek_position(core_file *file, int64_t offset, int origin)
{
    if (!file) return -1;
    int64_t result = file->api.seek(file->handle, offset, origin);
    if (result < 0) file->error = 1;
    return result;
}
int core_file_seek(core_file *file, int64_t offset, int origin)
{
    int position = origin == SEEK_SET ? RETRO_VFS_SEEK_POSITION_START :
        origin == SEEK_CUR ? RETRO_VFS_SEEK_POSITION_CURRENT :
        origin == SEEK_END ? RETRO_VFS_SEEK_POSITION_END : -1;
    return position < 0 || core_file_seek_position(file, offset, position) < 0 ? -1 : 0;
}
int64_t core_file_read(core_file *file, void *data, int64_t size)
{
    if (!file || size < 0 || (!data && size)) return -1;
    int64_t n = file->api.read(file->handle, data, (uint64_t)size);
    if (n < 0 || n > size) { file->error = 1; return -1; }
    return n;
}
int64_t core_file_write(core_file *file, const void *data, int64_t size)
{
    if (!file || size < 0 || (!data && size)) return -1;
    if (file->append && core_file_seek_position(file, 0, RETRO_VFS_SEEK_POSITION_END) < 0) return -1;
    int64_t n = file->api.write(file->handle, data, (uint64_t)size);
    if (n < 0 || n > size) { file->error = 1; return -1; }
    return n;
}
int core_file_flush(core_file *file)
{
    if (!file) return -1;
    int result = file->api.flush(file->handle);
    if (result) file->error = 1;
    return result;
}
int core_file_error(core_file *file) { return !file || file->error; }
void core_file_clear_error(core_file *file) { if (file) file->error = 0; }
size_t core_file_read_items(void *data, size_t size, size_t count, core_file *file)
{
    if (!size || count > SIZE_MAX / size) return 0;
    unsigned char *out = data;
    size_t remaining = size * count;
    while (remaining) {
        int64_t request = remaining > INT64_MAX ? INT64_MAX : (int64_t)remaining;
        int64_t n = core_file_read(file, out, request);
        if (n <= 0) break;
        out += n; remaining -= (size_t)n;
    }
    return (size * count - remaining) / size;
}
size_t core_file_write_items(const void *data, size_t size, size_t count, core_file *file)
{
    if (!size || count > SIZE_MAX / size) return 0;
    const unsigned char *in = data;
    size_t remaining = size * count;
    while (remaining) {
        int64_t request = remaining > INT64_MAX ? INT64_MAX : (int64_t)remaining;
        int64_t n = core_file_write(file, in, request);
        if (n <= 0) break;
        in += n; remaining -= (size_t)n;
    }
    return (size * count - remaining) / size;
}
bool core_file_read_exact(core_file *file, void *data, size_t size)
{ return core_file_read_items(data, 1, size, file) == size; }
bool core_file_write_exact(core_file *file, const void *data, size_t size)
{ return core_file_write_items(data, 1, size, file) == size; }
int core_file_getc(core_file *file)
{ unsigned char ch; return core_file_read(file, &ch, 1) == 1 ? ch : EOF; }
char *core_file_gets(char *buffer, int size, core_file *file)
{
    if (!buffer || size < 2) return NULL;
    int i = 0, ch;
    while (i < size - 1 && (ch = core_file_getc(file)) != EOF) {
        buffer[i++] = (char)ch;
        if (ch == '\n') break;
    }
    buffer[i] = 0;
    return i ? buffer : NULL;
}
int core_file_printf(core_file *file, const char *format, ...)
{
    va_list args, copy;
    va_start(args, format); va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) { va_end(args); return -1; }
    char *text = malloc((size_t)length + 1);
    if (!text) { va_end(args); return -1; }
    vsnprintf(text, (size_t)length + 1, format, args); va_end(args);
    bool ok = core_file_write_exact(file, text, (size_t)length);
    free(text); return ok ? length : -1;
}
