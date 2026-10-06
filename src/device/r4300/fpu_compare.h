/* Included by pure_interp.c. Ares/VR4300 compare predicates and legacy NaNs. */
void pure_interp_fpu_compare(struct r4300_core* r4300, uint32_t op)
{
    const unsigned int fmt = RS_OF(op);
    const unsigned int fs = (r4300->cp0.regs[CP0_STATUS_REG] & CP0_STATUS_FR)
        ? FS_OF(op) : (FS_OF(op) & ~1);
    const unsigned int ft = FT_OF(op);
    const unsigned int predicate = op & 15;
    uint64_t lhs, rhs, exponent, fraction, signaling;
    int unordered, invalid, result;
    if (check_cop1_unusable(r4300)) return;
    r4300->cp1.fcr31 &= ~UINT32_C(0x3f000);
    lhs = (uint64_t)r4300->cp1.regs[fs].dword;
    rhs = (uint64_t)r4300->cp1.regs[ft].dword;
    if (fmt == 16) {
        lhs = (uint32_t)lhs;
        rhs = (uint32_t)rhs;
        exponent = UINT32_C(0x7f800000);
        fraction = UINT32_C(0x007fffff);
        signaling = UINT32_C(0x00400000);
    } else {
        exponent = UINT64_C(0x7ff0000000000000);
        fraction = UINT64_C(0x000fffffffffffff);
        signaling = UINT64_C(0x0008000000000000);
    }
    const int lhs_nan = (lhs & exponent) == exponent && (lhs & fraction) != 0;
    const int rhs_nan = (rhs & exponent) == exponent && (rhs & fraction) != 0;
    unordered = lhs_nan || rhs_nan;
    /* Legacy MIPS uses the opposite signaling bit convention to host IEEE. */
    invalid = (unordered && (predicate & 8)) ||
        (lhs_nan && (lhs & signaling)) || (rhs_nan && (rhs & signaling));
    if (invalid) {
        r4300->cp1.fcr31 |= UINT32_C(0x10000);
        if (r4300->cp1.fcr31 & UINT32_C(0x800)) {
            r4300->cp0.regs[CP0_CAUSE_REG] =
                (r4300->cp0.regs[CP0_CAUSE_REG] & ~UINT32_C(0x3000007c)) | CP0_CAUSE_EXCCODE_FPE;
            exception_general(r4300);
            return; /* Preserve the old condition on an enabled exception. */
        }
        r4300->cp1.fcr31 |= UINT32_C(0x40);
    }
    if (unordered) result = predicate & 1;
    else {
        /* IEEE encodings are monotonic within each sign. Compare integer bits
         * so fast-math and host DAZ cannot collapse subnormal operands. */
        uint64_t sign = fmt == 16 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
        int equal = lhs == rhs || ((lhs | rhs) & ~sign) == 0;
        int less = !equal && (((lhs ^ rhs) & sign) ? (lhs & sign) != 0 :
            (lhs & sign) ? lhs > rhs : lhs < rhs);
        result = ((predicate & 2) && equal) || ((predicate & 4) && less);
    }
    r4300->cp1.fcr31 = (r4300->cp1.fcr31 & ~FCR31_CMP_BIT) | (result ? FCR31_CMP_BIT : 0);
    ADD_TO_PC(1);
}

#define FPU_COMPARE_PAIR(name) \
    DECLARE_INSTRUCTION(name##_S) { pure_interp_fpu_compare(r4300, op); } \
    DECLARE_INSTRUCTION(name##_D) { pure_interp_fpu_compare(r4300, op); }
FPU_COMPARE_PAIR(C_F)
FPU_COMPARE_PAIR(C_UN)
FPU_COMPARE_PAIR(C_EQ)
FPU_COMPARE_PAIR(C_UEQ)
FPU_COMPARE_PAIR(C_OLT)
FPU_COMPARE_PAIR(C_ULT)
FPU_COMPARE_PAIR(C_OLE)
FPU_COMPARE_PAIR(C_ULE)
FPU_COMPARE_PAIR(C_SF)
FPU_COMPARE_PAIR(C_NGLE)
FPU_COMPARE_PAIR(C_SEQ)
FPU_COMPARE_PAIR(C_NGL)
FPU_COMPARE_PAIR(C_LT)
FPU_COMPARE_PAIR(C_NGE)
FPU_COMPARE_PAIR(C_LE)
FPU_COMPARE_PAIR(C_NGT)
#undef FPU_COMPARE_PAIR
