#ifndef _LIBRETRO_PRIVATE_H_
#define _LIBRETRO_PRIVATE_H_

#include <stdio.h>
#include <string.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif


extern retro_get_cpu_features_t perf_get_cpu_features_cb;
extern retro_log_printf_t log_cb;
extern bool libretro_swap_buffer;
void retro_return();


#ifdef __cplusplus
}
#endif

#ifdef _MSC_VER
#define strcasecmp stricmp
#endif


#endif
