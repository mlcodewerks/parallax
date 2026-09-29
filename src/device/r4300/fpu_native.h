#ifndef M64P_DEVICE_R4300_FPU_NATIVE_H
#define M64P_DEVICE_R4300_FPU_NATIVE_H

#include <stdint.h>

/* Integer ABI keeps fast-math callers from transforming host FP operations.
 * Return bits 0..4: I/U/O/Z/V; bit 5: unimplemented conversion. */
unsigned int fpu_native_eval(unsigned int fmt, unsigned int fn,
    uint64_t a, uint64_t b, unsigned int rm, uint64_t* result);

#endif
