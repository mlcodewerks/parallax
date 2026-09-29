/* Compare optimized arithmetic/exception flags with the portable backend.
 * Operand generation is integer-only so the driver can use fast-math. */
#include <stdint.h>
#include <stdio.h>
#include "device/r4300/fpu_native.h"

unsigned int fpu_native_eval_portable(unsigned int, unsigned int,
    uint64_t, uint64_t, unsigned int, uint64_t*);

static uint64_t random_bits(void)
{
    static uint64_t state = UINT64_C(0x52719643179063ad);
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}

static int nan_bits(uint64_t value, int single)
{
    uint64_t exp = single ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    uint64_t frac = single ? UINT64_C(0x007fffff) : UINT64_C(0x000fffffffffffff);
    return (value & exp) == exp && (value & frac) != 0;
}

static int compare(unsigned int fmt, unsigned int fn, uint64_t a, uint64_t b, unsigned int rm)
{
    uint64_t fast, portable;
    unsigned int ff = fpu_native_eval(fmt, fn, a, b, rm, &fast);
    unsigned int pf = fpu_native_eval_portable(fmt, fn, a, b, rm, &portable);
    int integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    int single = fn == 32 || (fmt == 16 && fn != 33);
    if (ff == pf && ((ff & 32) || fast == portable ||
        (!integer && nan_bits(fast, single) && nan_bits(portable, single)))) return 0;
    fprintf(stderr, "FPU mismatch fmt=%u fn=%u rm=%u a=%016llx b=%016llx "
        "native=%016llx/%02x portable=%016llx/%02x\n", fmt, fn, rm,
        (unsigned long long)a, (unsigned long long)b, (unsigned long long)fast, ff,
        (unsigned long long)portable, pf);
    return 1;
}

int main(void)
{
    unsigned int checks = 0;
    for (unsigned int fmt = 16; fmt <= 17; ++fmt) {
        for (unsigned int i = 0; i < 1024; ++i) {
            uint64_t a = random_bits(), b = random_bits();
            /* Normal operands spanning the exponent range, including values
             * that overflow/underflow and mantissas invalid in the other format. */
            if (fmt == 16) {
                a = (a & 0x807fffff) | ((1 + (a >> 32) % 254) << 23);
                b = (b & 0x807fffff) | ((1 + (b >> 32) % 254) << 23);
            } else {
                a = (a & UINT64_C(0x800fffffffffffff)) | ((1 + (a >> 32) % 2046) << 52);
                b = (b & UINT64_C(0x800fffffffffffff)) | ((1 + (b >> 32) % 2046) << 52);
            }
            for (unsigned int rm = 0; rm < 4; ++rm) {
                for (unsigned int fn = 0; fn <= 4; ++fn) {
                    if (compare(fmt, fn, a, b, rm)) return 1;
                    ++checks;
                }
                if (compare(fmt, fmt == 16 ? 33 : 32, a, b, rm)) return 1;
                ++checks;
            }
            /* Conversion inputs within both W/L limits; retain random payload
             * bits so the unused format often represents an unrelated value. */
            if (fmt == 16) a = (a & 0x807fffff) | ((uint64_t)(111 + i % 46) << 23);
            else a = (a & UINT64_C(0x800fffffffffffff)) | ((uint64_t)(1007 + i % 46) << 52);
            for (unsigned int rm = 0; rm < 4; ++rm) {
                for (unsigned int fn = 8; fn <= 15; ++fn) {
                    if (compare(fmt, fn, a, b, fn & 3)) return 1;
                    ++checks;
                }
                if (compare(fmt, 36, a, b, rm) || compare(fmt, 37, a, b, rm)) return 1;
                checks += 2;
            }
        }
    }
    for (unsigned int fmt = 20; fmt <= 21; ++fmt) {
        for (unsigned int i = 0; i < 1024; ++i) {
            uint64_t a = random_bits();
            if (fmt == 21) a = (uint64_t)((int64_t)a >> 10); /* Within 2^55. */
            for (unsigned int rm = 0; rm < 4; ++rm) {
                if (compare(fmt, 32, a, 0, rm) || compare(fmt, 33, a, 0, rm)) return 1;
                checks += 2;
            }
        }
    }
    printf("FPU native/portable differential checks passed (%u cases).\n", checks);
    return 0;
}
