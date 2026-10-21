#ifndef PARALLAX_LOG_H
#define PARALLAX_LOG_H
#include "core/types.h"
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__GNUC__)
#define ATTR_FMT(fmtpos, attrpos) __attribute__ ((format (printf, fmtpos, attrpos)))
#else
#define ATTR_FMT(fmtpos, attrpos)
#endif
void core_log(int level, const char *message, ...) ATTR_FMT(2,3);
#ifdef __cplusplus
}
#endif
#endif
