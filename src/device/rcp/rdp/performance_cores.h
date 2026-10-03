#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct performance_affinity {
#ifdef _WIN32
    GROUP_AFFINITY previous;
#endif
    bool changed;
};
/* Initialize on the emulation thread before starting renderer workers. */
void performance_cores_init(void);
unsigned performance_cores_count(void);
void performance_cores_set_enabled(bool enabled);
bool performance_core_enter(unsigned worker, struct performance_affinity *state);
/* Keep CPU/RSP execution on eligible cores, preserving narrower host masks. */
bool performance_emulation_enter(struct performance_affinity *state);
void performance_core_leave(struct performance_affinity *state);
#ifdef __cplusplus
}
#endif
