/* AArch64 Advanced SIMD/FP backend. Included by fpu_native.c.
 * Scalar SIMD instructions avoid exceptions from unused vector lanes. Explicit
 * instructions keep FPCR-dependent operations ordered, even under LTO.
 * AArch32 NEON lacks double precision and retains the portable backend. */
#ifndef M64P_DEVICE_R4300_FPU_NEON_H
#define M64P_DEVICE_R4300_FPU_NEON_H

#if defined(__aarch64__) && defined(__ARM_NEON) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(M64P_FPU_PORTABLE)
#define FPU_NATIVE_NEON 1

static FPU_NOINLINE uint64_t fpu_neon_calc(unsigned int fmt, unsigned int fn,
    uint64_t a, uint64_t b)
{
    uint64_t result;
    if ((fn >= 8 && fn <= 15) || fn == 36 || fn == 37) {
        /* FRINTX obeys FPCR and reports inexact. FCVTZS then converts the exact
         * integral value. Input limits were validated by the guest layer. */
        if (fmt == 16) {
            __asm__ volatile("fmov s0, %w1\n\tfrintx s0, s0\n\tfcvtzs %0, s0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        } else {
            __asm__ volatile("fmov d0, %1\n\tfrintx d0, d0\n\tfcvtzs %0, d0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        }
        return result;
    }
    if (fn == 32) {
        if (fmt == 17) {
            __asm__ volatile("fmov d0, %1\n\tfcvt s0, d0\n\tfmov %w0, s0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        } else if (fmt == 20) {
            __asm__ volatile("scvtf s0, %w1\n\tfmov %w0, s0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        } else {
            __asm__ volatile("scvtf s0, %1\n\tfmov %w0, s0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        }
        return result;
    }
    if (fn == 33) {
        if (fmt == 16) {
            __asm__ volatile("fmov s0, %w1\n\tfcvt d0, s0\n\tfmov %0, d0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        } else if (fmt == 20) {
            __asm__ volatile("scvtf d0, %w1\n\tfmov %0, d0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        } else {
            __asm__ volatile("scvtf d0, %1\n\tfmov %0, d0"
                : "=r"(result) : "r"(a) : "v0", "memory");
        }
        return result;
    }

#define NEON_BINARY_S(instruction) \
    __asm__ volatile("fmov s0, %w1\n\tfmov s1, %w2\n\t" instruction " s0, s0, s1\n\tfmov %w0, s0" \
        : "=r"(result) : "r"(a), "r"(b) : "v0", "v1", "memory")
#define NEON_BINARY_D(instruction) \
    __asm__ volatile("fmov d0, %1\n\tfmov d1, %2\n\t" instruction " d0, d0, d1\n\tfmov %0, d0" \
        : "=r"(result) : "r"(a), "r"(b) : "v0", "v1", "memory")
    if (fmt == 16) {
        switch (fn) {
        case 0: NEON_BINARY_S("fadd"); break;
        case 1: NEON_BINARY_S("fsub"); break;
        case 2: NEON_BINARY_S("fmul"); break;
        case 3: NEON_BINARY_S("fdiv"); break;
        default:
            __asm__ volatile("fmov s0, %w1\n\tfsqrt s0, s0\n\tfmov %w0, s0"
                : "=r"(result) : "r"(a) : "v0", "memory");
            break;
        }
    } else {
        switch (fn) {
        case 0: NEON_BINARY_D("fadd"); break;
        case 1: NEON_BINARY_D("fsub"); break;
        case 2: NEON_BINARY_D("fmul"); break;
        case 3: NEON_BINARY_D("fdiv"); break;
        default:
            __asm__ volatile("fmov d0, %1\n\tfsqrt d0, d0\n\tfmov %0, d0"
                : "=r"(result) : "r"(a) : "v0", "memory");
            break;
        }
    }
#undef NEON_BINARY_S
#undef NEON_BINARY_D
    return result;
}

static unsigned int fpu_neon_eval(unsigned int fmt, unsigned int fn,
    uint64_t a, uint64_t b, unsigned int rm, uint64_t* result)
{

    static const unsigned int modes[4] = {0, 3, 1, 2};
    uint64_t control = (uint64_t)modes[rm] << 22;
    uint64_t saved_control, saved_status, status;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr\n\tmsr fpcr, %2\n\tmsr fpsr, xzr"
        : "=&r"(saved_control), "=&r"(saved_status) : "r"(control) : "memory");
    *result = fpu_neon_calc(fmt, fn, a, b);
    __asm__ volatile("mrs %0, fpsr\n\tmsr fpcr, %1\n\tmsr fpsr, %2"
        : "=&r"(status) : "r"(saved_control), "r"(saved_status) : "memory");
    if ((fn >= 12 && fn <= 15) || fn == 36) {
        int64_t value = (int64_t)*result;
        if (value > INT32_MAX || value < INT32_MIN) return 32;
        *result = (uint32_t)value;
    }

    return ((status >> 4) & 1) | ((status >> 2) & 2) | (status & 4) |
        ((status << 2) & 8) | ((status << 4) & 16);
}
#endif
#endif
