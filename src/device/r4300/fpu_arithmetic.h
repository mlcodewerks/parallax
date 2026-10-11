/* Included by pure_interp.c. VR4300 arithmetic semantics, following Ares's
 * interpreter-fpu.cpp. Host FP state is isolated from the emulator frontend. */
#include <math.h>
#include "fpu_native.h"
#include "interp_fpu_move.h"

static void fpu_exception(struct r4300_core* cpu)
{
    cpu->cp0.regs[CP0_CAUSE_REG] =
        (cpu->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_FPE;
    exception_general(cpu);
}

static int fpu_unimplemented(struct r4300_core* cpu)
{
    cpu->cp1.fcr31 |= UINT32_C(0x20000);
    fpu_exception(cpu);
    return 1;
}

/* flags: inexact, underflow, overflow, divide-by-zero, invalid. */
static int fpu_flags(struct r4300_core* cpu, unsigned int flags)
{
    unsigned int enabled = (cpu->cp1.fcr31 >> 7) & 31;
    cpu->cp1.fcr31 |= (flags << 12) | ((flags & ~enabled) << 2);
    if (flags & enabled) { fpu_exception(cpu); return 1; }
    return 0;
}

static int fpu_class(uint64_t bits, int single)
{
    uint64_t exponent = single ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    uint64_t fraction = single ? UINT64_C(0x007fffff) : UINT64_C(0x000fffffffffffff);
    if ((bits & exponent) == exponent) return bits & fraction ? FP_NAN : FP_INFINITE;
    if (!(bits & exponent)) return bits & fraction ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

static int fpu_inputs(struct r4300_core* cpu, uint64_t a, uint64_t b, int single, int binary)
{
    int ca = fpu_class(a, single), cb = binary ? fpu_class(b, single) : FP_ZERO;
    uint64_t signal = single ? UINT64_C(0x00400000) : UINT64_C(0x0008000000000000);
    /* Quiet legacy NaNs and denormals take priority over invalid-operation. */
    if ((ca == FP_NAN && !(a & signal)) || (cb == FP_NAN && !(b & signal)) ||
        ca == FP_SUBNORMAL || cb == FP_SUBNORMAL) return fpu_unimplemented(cpu);
    if (ca == FP_NAN || cb == FP_NAN) return fpu_flags(cpu, 16);
    return 0;
}

static int fpu_output(struct r4300_core* cpu, uint64_t* bits, int single)
{
    int cls = fpu_class(*bits, single);
    if (cls == FP_NAN) {
        *bits = single ? UINT64_C(0x7fbfffff) : UINT64_C(0x7ff7ffffffffffff);
    } else if (cls == FP_SUBNORMAL) {
        uint64_t sign = *bits & (single ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000));
        unsigned int rm = cpu->cp1.fcr31 & 3;
        if (!(cpu->cp1.fcr31 & UINT32_C(0x1000000)) || (cpu->cp1.fcr31 & 0x180))
            return fpu_unimplemented(cpu);
        fpu_flags(cpu, 3);
        *bits = sign;
        if ((rm == 2 && !sign) || (rm == 3 && sign))
            *bits |= single ? UINT64_C(0x00800000) : UINT64_C(0x0010000000000000);
    }
    return 0;
}

static M64P_FORCE_INLINE int fpu_prepare(struct r4300_core* cpu, uint32_t op, uint64_t inputs[3])
{
    unsigned int fmt = RS_OF(op), fn = op & 63;
    unsigned int fs = (cpu->cp0.regs[CP0_STATUS_REG] & CP0_STATUS_FR) ? FS_OF(op) : (FS_OF(op) & ~1);
    unsigned int ft = FT_OF(op);
    int single = fmt == 16;
    int to_integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    cp1_reg a = cpu->cp1.regs[fs], b = cpu->cp1.regs[ft];
    unsigned int rm = cpu->cp1.fcr31 & 3;
    inputs[0] = (uint64_t)a.dword; inputs[1] = (uint64_t)b.dword;
    inputs[2] = to_integer && fn >= 8 && fn <= 15 ? fn & 3 : rm;
    if (check_cop1_unusable(cpu)) return 0;
    if (fn == 6 && (fmt == 16 || fmt == 17)) {
        return 1; /* MOV.S copies 64 bits and does not clear causes. */
    }
    cpu->cp1.fcr31 &= ~UINT32_C(0x3f000);
    if ((fmt != 16 && fmt != 17 && fmt != 20 && fmt != 21) ||
        ((fmt == 20 || fmt == 21) && fn != 32 && fn != 33) ||
        (fmt == 16 && fn == 32) || (fmt == 17 && fn == 33)) {
        fpu_unimplemented(cpu); return 0;
    }
    if (fmt == 16 || fmt == 17) {
        if (to_integer) {
            int cls = fpu_class((uint64_t)a.dword, single);
            if (cls == FP_SUBNORMAL || cls == FP_INFINITE || cls == FP_NAN) {
                fpu_unimplemented(cpu); return 0;
            }
            int word = fn >= 12 && fn <= 15 ? 1 : fn == 36;
            uint64_t sign = single ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
            uint64_t raw = single ? (uint32_t)a.dword : (uint64_t)a.dword;
            uint64_t magnitude = raw & ~sign;
            uint64_t limit = word ? (single ? UINT64_C(0x4f000000) : UINT64_C(0x41e0000000000000))
                : (single ? UINT64_C(0x5a000000) : UINT64_C(0x4340000000000000));
            /* Bitwise bounds keep fast-math from speculating a conversion
             * before operand validation or changing host exception flags. */
            if (magnitude > limit || (magnitude == limit && (!word || !(raw & sign)))) {
                fpu_unimplemented(cpu); return 0;
            }
        } else if (fpu_inputs(cpu, (uint64_t)a.dword, (uint64_t)b.dword, single, fn <= 3)) return 0;
    } else if (fmt == 21 && (a.dword >= INT64_C(0x0080000000000000) || a.dword < -INT64_C(0x0080000000000000))) {
        fpu_unimplemented(cpu); return 0;
    }
    return 1;
}

static M64P_FORCE_INLINE void fpu_finish(struct r4300_core* cpu, uint32_t op, uint64_t bits, unsigned int flags)
{
    unsigned int fmt = RS_OF(op), fn = op & 63, fd = FD_OF(op);
    int single = fmt == 16, out_single = fn == 32 ? 1 : fn == 33 ? 0 : single;
    int to_integer = (fn >= 8 && fn <= 15) || fn == 36 || fn == 37;
    if (fn == 6) { cpu->cp1.regs[fd].dword = (int64_t)bits; cpu->interp_PC.addr += 4; return; }
    if ((flags & 32) || ((flags & 2) &&
        (!(cpu->cp1.fcr31 & UINT32_C(0x1000000)) || (cpu->cp1.fcr31 & 0x180)))) {
        fpu_unimplemented(cpu); return;
    }
    if (fpu_flags(cpu, flags)) return;
    if (!to_integer && fpu_output(cpu, &bits, out_single)) return;
    cpu->cp1.regs[fd].dword = (int64_t)bits;
    cp0_step_cycles(&cpu->cp0, r4300_base_cycles(op, cpu->cp0.regs[CP0_STATUS_REG]) - 1);
    cpu->interp_PC.addr += 4;
}

int pure_interp_fpu_prepare(struct r4300_core* cpu, uint32_t op, uint64_t inputs[3])
{
    return fpu_prepare(cpu, op, inputs);
}
void pure_interp_fpu_finish(struct r4300_core* cpu, uint32_t op, uint64_t bits, unsigned int flags)
{
    fpu_finish(cpu, op, bits, flags);
}

void pure_interp_fpu_arithmetic(struct r4300_core* cpu, uint32_t op)
{
    uint64_t inputs[3], bits;
    unsigned int fmt = RS_OF(op), fn = op & 63, flags = 0;
    if (fn == 6 && (fmt == 16 || fmt == 17)) {
        interp_fpu_move(cpu, op);
        return;
    }
    if (!fpu_prepare(cpu, op, inputs)) return;
    if (fn == 6) bits = inputs[0];
    else if (fn == 5 || fn == 7) {
        uint64_t sign = fmt == 16 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
        bits = fmt == 16 ? (uint32_t)inputs[0] : inputs[0];
        bits = fn == 5 ? bits & ~sign : bits ^ sign;
    } else flags = fpu_native_eval(fmt, fn, inputs[0], inputs[1], (unsigned int)inputs[2], &bits);
    fpu_finish(cpu, op, bits, flags);
}

#define FPU_ARITH_PAIR(name) \
    DECLARE_INSTRUCTION(name##_S) { pure_interp_fpu_arithmetic(r4300, op); } \
    DECLARE_INSTRUCTION(name##_D) { pure_interp_fpu_arithmetic(r4300, op); }
FPU_ARITH_PAIR(ABS)
FPU_ARITH_PAIR(ADD)
FPU_ARITH_PAIR(DIV)
DECLARE_INSTRUCTION(MOV_S) { interp_fpu_move(r4300, op); }
DECLARE_INSTRUCTION(MOV_D) { interp_fpu_move(r4300, op); }
FPU_ARITH_PAIR(MUL)
FPU_ARITH_PAIR(NEG)
FPU_ARITH_PAIR(SQRT)
FPU_ARITH_PAIR(SUB)
FPU_ARITH_PAIR(TRUNC_W)
FPU_ARITH_PAIR(TRUNC_L)
FPU_ARITH_PAIR(ROUND_W)
FPU_ARITH_PAIR(ROUND_L)
FPU_ARITH_PAIR(CEIL_W)
FPU_ARITH_PAIR(CEIL_L)
FPU_ARITH_PAIR(FLOOR_W)
FPU_ARITH_PAIR(FLOOR_L)
FPU_ARITH_PAIR(CVT_W)
FPU_ARITH_PAIR(CVT_L)
#define FPU_CONVERT(name) DECLARE_INSTRUCTION(name) { pure_interp_fpu_arithmetic(r4300, op); }
FPU_CONVERT(CVT_S_D)
FPU_CONVERT(CVT_S_W)
FPU_CONVERT(CVT_S_L)
FPU_CONVERT(CVT_D_S)
FPU_CONVERT(CVT_D_W)
FPU_CONVERT(CVT_D_L)
#undef FPU_CONVERT
#undef FPU_ARITH_PAIR
