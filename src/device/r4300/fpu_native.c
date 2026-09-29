/* Strict host arithmetic backend. Guest register/exception handling remains in
 * the fast-math interpreter; only this translation unit needs FP environment
 * semantics. No host floating-point value crosses the public integer ABI. */
#include "fpu_native.h"
#include "cp1.h"
#include <fenv.h>
#include <math.h>

#if defined(_MSC_VER)
#define FPU_NOINLINE __declspec(noinline)
#else
#define FPU_NOINLINE __attribute__((noinline))
#endif
#include "fpu_neon.h"
static FPU_NOINLINE float fpu_calc_s(unsigned int fn, float x, float y)
{
    volatile float a = x, b = y, result;
    switch (fn) {
    case 0: result = a + b; break;
    case 1: result = a - b; break;
    case 2: result = a * b; break;
    case 3: result = a / b; break;
    case 4:
        /* Some CRTs return a NaN for domain errors without raising FE_INVALID. */
        if (a < 0) { feraiseexcept(FE_INVALID); result = NAN; }
        else result = sqrtf(a);
        break;
    case 5: result = fabsf(a); break;
    default: result = -a; break;
    }
    return result;
}

static FPU_NOINLINE double fpu_calc_d(unsigned int fn, double x, double y)
{
    volatile double a = x, b = y, result;
    switch (fn) {
    case 0: result = a + b; break;
    case 1: result = a - b; break;
    case 2: result = a * b; break;
    case 3: result = a / b; break;
    case 4:
        if (a < 0) { feraiseexcept(FE_INVALID); result = NAN; }
        else result = sqrt(a);
        break;
    case 5: result = fabs(a); break;
    default: result = -a; break;
    }
    return result;
}

static FPU_NOINLINE uint64_t fpu_convert(unsigned int fmt, unsigned int fn, uint64_t bits)
{
    cp1_reg input, output;
    input.dword = (int64_t)bits;
    output.dword = 0;
    if (fn == 32) {
        volatile float value;
        if (fmt == 17) value = (float)input.float64;
        else if (fmt == 20) value = (float)(int32_t)bits;
        else value = (float)input.dword;
        output.float32[0] = value;
    } else {
        volatile double value;
        if (fmt == 16) value = (double)input.float32[0];
        else if (fmt == 20) value = (double)(int32_t)bits;
        else value = (double)input.dword;
        output.float64 = value;
    }
    return (uint64_t)output.dword;
}

/* A function boundary keeps the arithmetic between the MXCSR writes/reads.
 * SSE scalar instructions also avoid CRT sqrt domain-error differences. */
#if defined(OSAL_SSE) && (defined(__SSE2__) || defined(_M_X64)) && !defined(M64P_FPU_PORTABLE)
#define FPU_NATIVE_SSE2 1
#if defined(__x86_64__) || defined(_M_X64)
/* Intrinsics can be speculated by the compiler even with strict FP flags.
 * Separate side-effecting calls prevent executing the unselected precision
 * and contaminating MXCSR with exceptions from unrelated register bits. */
static FPU_NOINLINE int64_t fpu_sse_integer_s(float input)
{
    volatile int64_t result = _mm_cvtss_si64(_mm_set_ss(input));
    return result;
}

static FPU_NOINLINE int64_t fpu_sse_integer_d(double input)
{
    volatile int64_t result = _mm_cvtsd_si64(_mm_set_sd(input));
    return result;
}
#endif
static FPU_NOINLINE uint64_t fpu_sse_calc(unsigned int fmt, unsigned int fn,
    uint64_t abits, uint64_t bbits)
{
    cp1_reg a, b;
    /* Prevent IPA from treating this helper as a pure function independent of
     * MXCSR and moving/reusing its result across environment changes. */
    volatile cp1_reg result;
    a.dword = (int64_t)abits;
    b.dword = (int64_t)bbits;
    result.dword = 0;
#if defined(__x86_64__) || defined(_M_X64)
    if ((fn >= 8 && fn <= 15) || fn == 36 || fn == 37) {
        return fmt == 16 ? (uint64_t)fpu_sse_integer_s(a.float32[0]) :
            (uint64_t)fpu_sse_integer_d(a.float64);
    }
    if (fn == 32) {
        __m128 value;
        if (fmt == 17) value = _mm_cvtsd_ss(_mm_setzero_ps(), _mm_set_sd(a.float64));
        else if (fmt == 20) value = _mm_cvtsi32_ss(_mm_setzero_ps(), (int32_t)abits);
        else value = _mm_cvtsi64_ss(_mm_setzero_ps(), (int64_t)abits);
        result.float32[0] = _mm_cvtss_f32(value);
        return (uint64_t)result.dword;
    }
    if (fn == 33) {
        __m128d value;
        if (fmt == 16) value = _mm_cvtss_sd(_mm_setzero_pd(), _mm_set_ss(a.float32[0]));
        else if (fmt == 20) value = _mm_cvtsi32_sd(_mm_setzero_pd(), (int32_t)abits);
        else value = _mm_cvtsi64_sd(_mm_setzero_pd(), (int64_t)abits);
        result.float64 = _mm_cvtsd_f64(value);
        return (uint64_t)result.dword;
    }
#endif
    if (fmt == 16) {
        __m128 x = _mm_set_ss(a.float32[0]), y = _mm_set_ss(b.float32[0]), z;
        switch (fn) {
        case 0: z = _mm_add_ss(x, y); break;
        case 1: z = _mm_sub_ss(x, y); break;
        case 2: z = _mm_mul_ss(x, y); break;
        case 3: z = _mm_div_ss(x, y); break;
        default: z = _mm_sqrt_ss(x); break;
        }
        result.float32[0] = _mm_cvtss_f32(z);
    } else {
        __m128d x = _mm_set_sd(a.float64), y = _mm_set_sd(b.float64), z;
        switch (fn) {
        case 0: z = _mm_add_sd(x, y); break;
        case 1: z = _mm_sub_sd(x, y); break;
        case 2: z = _mm_mul_sd(x, y); break;
        case 3: z = _mm_div_sd(x, y); break;
        default: z = _mm_sqrt_sd(x, x); break;
        }
        result.float64 = _mm_cvtsd_f64(z);
    }
    return (uint64_t)result.dword;
}
#endif

unsigned int fpu_native_eval(unsigned int fmt, unsigned int fn,
    uint64_t abits, uint64_t bbits, unsigned int rm, uint64_t* bits)
{
#ifdef FPU_NATIVE_NEON
    return fpu_neon_eval(fmt, fn, abits, bbits, rm, bits);
#endif
#ifdef FPU_NATIVE_SSE2
    if (fn <= 4
#if defined(__x86_64__) || defined(_M_X64)
        || fn >= 8
#endif
    ) {
        static const unsigned int modes[4] = {0, 3, 2, 1};
        unsigned int saved = _mm_getcsr();
        _mm_setcsr(0x1f80 | (modes[rm] << 13));
        *bits = fpu_sse_calc(fmt, fn, abits, bbits);
        unsigned int flags = _mm_getcsr();
        _mm_setcsr(saved);
        if ((fn >= 12 && fn <= 15) || fn == 36) {
            int64_t value = (int64_t)*bits;
            if (value > INT32_MAX || value < INT32_MIN) return 32;
            *bits = (uint32_t)value;
        }
        return ((flags >> 5) & 1) | ((flags >> 3) & 2) |
            ((flags >> 1) & 4) | ((flags << 1) & 8) | ((flags << 4) & 16);
    }
#endif
    cp1_reg a, b, result;
    fenv_t environment;
    static const int rounding[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    int to_integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    unsigned int flags = 0;
    a.dword = (int64_t)abits;
    b.dword = (int64_t)bbits;
    result.dword = 0;
    feholdexcept(&environment);
#ifdef OSAL_SSE
    unsigned int mxcsr = _mm_getcsr();
    _mm_setcsr(mxcsr & ~UINT32_C(0x8040));
#endif
    fesetround(rounding[rm]);
    if (to_integer) {
        volatile double value = fmt == 16 ? (double)a.float32[0] : a.float64;
        volatile double rounded = rint(value);
        int word = (fn >= 12 && fn <= 15) || fn == 36;
        if (word && (rounded >= 2147483648.0 || rounded < -2147483648.0)) {
            flags = 32;
            goto restore;
        }
        result.dword = word ? (uint32_t)(int32_t)rounded : (int64_t)rounded;
        if (rounded != value) flags |= 1;
    } else if (fn == 32 || fn == 33) {
        result.dword = (int64_t)fpu_convert(fmt, fn, abits);
    } else if (fmt == 16) result.float32[0] = fpu_calc_s(fn, a.float32[0], b.float32[0]);
    else result.float64 = fpu_calc_d(fn, a.float64, b.float64);
    int exceptions = fetestexcept(FE_ALL_EXCEPT);
    if (exceptions & FE_INEXACT) flags |= 1;
    if (exceptions & FE_UNDERFLOW) flags |= 2;
    if (exceptions & FE_OVERFLOW) flags |= 4;
    if (exceptions & FE_DIVBYZERO) flags |= 8;
    if (exceptions & FE_INVALID) flags |= 16;
restore:
#ifdef OSAL_SSE
    _mm_setcsr(mxcsr);
#endif
    fesetenv(&environment);
    *bits = (uint64_t)result.dword;
    return flags;
}
