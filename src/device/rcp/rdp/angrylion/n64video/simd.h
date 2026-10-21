/* Shared integer SIMD backend for RDP and VI. GPL-2.0-or-later.
 * AL_SIMD_SSE2 is the legacy intrinsic-path feature name, including NEON
 * through sse2neon. New kernels use the SSE4.2 intrinsic interface. */
#ifndef ANGRYLION_SIMD_H
#define ANGRYLION_SIMD_H
#if !defined(ANGRYLION_SIMD_DISABLE)
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include "sse2neon.h"
#define AL_SIMD_SSE2 1
#define AL_SIMD_SSE42 1
#define AL_SIMD_SSE2NEON 1
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define AL_SIMD_SSE2 1
#define AL_SIMD_SSE42 1
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define AL_SIMD_SSE2 1
#endif
#endif
#endif
