#ifndef M64P_DEVICE_R4300_FPU_NATIVE_H
#define M64P_DEVICE_R4300_FPU_NATIVE_H

#include <stdint.h>

unsigned int fpu_native_eval(unsigned int fmt, unsigned int fn,
    uint64_t a, uint64_t b, unsigned int rm, uint64_t* result);

#endif
