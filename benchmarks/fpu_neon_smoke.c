/* Standalone AArch64 backend checks, without emulator or libc dependencies.
 * cc -O3 benchmarks/fpu_neon_smoke.c -o fpu_neon_smoke && ./fpu_neon_smoke
 * Nonzero exit is the failing case index (100: host FP state changed).
 * M64P_FPU_FREESTANDING also permits cross-target frontend checks without a sysroot. */
#ifdef M64P_FPU_FREESTANDING
typedef __UINT64_TYPE__ uint64_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT32_TYPE__ uint32_t;
#define INT32_MAX 2147483647
#define INT32_MIN (-2147483647 - 1)
#else
#include <stdint.h>
#endif
#define FPU_NOINLINE __attribute__((noinline))
#include "../src/device/r4300/fpu_neon.h"
#ifndef FPU_NATIVE_NEON
#error This test requires an AArch64 compiler with Advanced SIMD enabled.
#endif

int main(void)
{
    static const struct {
        unsigned int fmt, fn, rm;
        uint64_t a, b, result;
        unsigned int flags;
    } cases[] = {
        {16, 0, 0, 0x3f800000, 0x33800000, 0x3f800000, 1},
        {16, 0, 2, 0x3f800000, 0x33800000, 0x3f800001, 1},
        {16, 0, 3, 0xbf800000, 0xb3800000, 0xbf800001, 1},
        {17, 0, 2, 0x3ff0000000000000ULL, 0x3ca0000000000000ULL, 0x3ff0000000000001ULL, 1},
        {16, 1, 0, 0x40c00000, 0x40400000, 0x40400000, 0},
        {17, 1, 0, 0x4018000000000000ULL, 0x4008000000000000ULL, 0x4008000000000000ULL, 0},
        {16, 2, 0, 0x80000000, 0x3f800000, 0x80000000, 0},
        {17, 2, 0, 0x3ff8000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL, 0},
        {16, 2, 0, 0x7f7fffff, 0x40000000, 0x7f800000, 5},
        {16, 2, 0, 0x00800000, 0x3f000000, 0x00400000, 0},
        {16, 2, 0, 0x00800001, 0x3f000000, 0x00400000, 3},
        {16, 3, 0, 0x3f800000, 0, 0x7f800000, 8},
        {17, 3, 0, 0x3ff0000000000000ULL, 0, 0x7ff0000000000000ULL, 8},
        {16, 4, 0, 0x40800000, 0, 0x40000000, 0},
        {17, 4, 0, 0x4010000000000000ULL, 0, 0x4000000000000000ULL, 0},
        {16, 4, 0, 0x80000000, 0, 0x80000000, 0},
        {16, 36, 0, 0x40200000, 0, 2, 1},
        {16, 36, 1, 0xc0200000, 0, 0xfffffffe, 1},
        {16, 36, 2, 0x40200000, 0, 3, 1},
        {16, 36, 3, 0xc0200000, 0, 0xfffffffd, 1},
        {17, 37, 0, 0x4004000000000000ULL, 0, 2, 1},
        {17, 37, 3, 0xc004000000000000ULL, 0, 0xfffffffffffffffdULL, 1},
        {17, 13, 1, 0x3ff1fffffe632358ULL, 0, 1, 1}, /* DK64 boot */
        {20, 32, 0, 16777217, 0, 0x4b800000, 1},
        {20, 32, 2, 16777217, 0, 0x4b800001, 1},
        {21, 32, 0, 16777217, 0, 0x4b800000, 1},
        {21, 33, 2, 9007199254740993ULL, 0, 0x4340000000000001ULL, 1},
        {20, 33, 0, 0xffffffff, 0, 0xbff0000000000000ULL, 0},
        {16, 33, 0, 0x3fc00000, 0, 0x3ff8000000000000ULL, 0},
        {17, 32, 0, 0x3ff8000000000000ULL, 0, 0x3fc00000, 0},
    };
    uint64_t original_control, original_status, control, status;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr"
        : "=r"(original_control), "=r"(original_status));
    /* Deliberately hostile caller settings: FZ, DN, rounding down, old flags. */
    uint64_t host_control = 0x03800000, host_status = 0x0800001f;
    int failure = 0;
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1"
        : : "r"(host_control), "r"(host_status) : "memory");
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint64_t result;
        unsigned int flags = fpu_neon_eval(cases[i].fmt, cases[i].fn,
            cases[i].a, cases[i].b, cases[i].rm, &result);
        if (flags != cases[i].flags || result != cases[i].result) { failure = i + 1; break; }
        __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(control), "=r"(status));
        if (control != host_control || status != host_status) { failure = 100; break; }
    }
    uint64_t result;
    if (!failure && fpu_neon_eval(16, 4, 0xbf800000, 0, 0, &result) != 16) failure = 101;
    /* Rounded word overflow must report unimplemented, without committing. */
    if (!failure && fpu_neon_eval(17, 36, 0x41dffffffff00000ULL, 0, 2, &result) != 32) failure = 102;
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1"
        : : "r"(original_control), "r"(original_status) : "memory");
    return failure;
}
