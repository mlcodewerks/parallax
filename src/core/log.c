/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "log.h"
#include <libretro.h>
#include <stdarg.h>
#include <stdio.h>
extern retro_log_printf_t log_cb;
void core_log(int level, const char *message, ...)
{
    if (!log_cb) return;
    char text[2048];
    va_list args;
    va_start(args, message);
    vsnprintf(text, sizeof(text), message, args);
    va_end(args);
    enum retro_log_level severity = level == CORE_LOG_ERROR ? RETRO_LOG_ERROR :
        level == CORE_LOG_WARNING ? RETRO_LOG_WARN :
        level == CORE_LOG_VERBOSE ? RETRO_LOG_DEBUG : RETRO_LOG_INFO;
    log_cb(severity, "parallax: %s\n", text);
}
