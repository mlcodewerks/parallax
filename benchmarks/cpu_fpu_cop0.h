/* Ares-derived edge cases, executed through both real interpreter dispatchers. */
#include <fenv.h>

static uint32_t fp_op(unsigned fmt, unsigned fn)
{
    return 0x44000000 | (fmt << 21) | (2 << 16) | (1 << 11) | (3 << 6) | fn;
}

static void fp_setup(int cached)
{
    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] = CP0_STATUS_CU1 | CP0_STATUS_FR;
    set_fpr_pointers(&cpu.cp1, cpu.cp0.regs[CP0_STATUS_REG]);
    cpu.cp1.fcr31 = 0;
    cpu.cp1.regs[3].dword = INT64_C(0x123456789abcdef0);
}

static void check_fp_arithmetic(int cached)
{
    const double expected[] = {9, 3, 18, 2, 0, 6, 6, -6};
    for (unsigned fmt = 16; fmt <= 17; ++fmt) {
        for (unsigned fn = 0; fn < 8; ++fn) {
            fp_setup(cached);
            if (fmt == 16) {
                cpu.cp1.regs[1].float32[0] = fn == 4 ? 4 : 6;
                cpu.cp1.regs[2].float32[0] = 3;
            } else {
                cpu.cp1.regs[1].float64 = fn == 4 ? 4 : 6;
                cpu.cp1.regs[2].float64 = 3;
            }
            execute_test_op(fp_op(fmt, fn));
            double result = fmt == 16 ? cpu.cp1.regs[3].float32[0] : cpu.cp1.regs[3].float64;
            assert(result == (fn == 4 ? 2 : expected[fn]));
            assert(!(cpu.cp1.fcr31 & 0x3f000));
            assert(cpu.cp0.regs[CP0_COUNT_REG] * 2 + cpu.cp0.count_phase ==
                r4300_base_cycles(fp_op(fmt, fn), CP0_STATUS_CU1));
            if (fmt == 16 && fn != 6) assert((uint64_t)cpu.cp1.regs[3].dword >> 32 == 0);
        }
        for (unsigned fn = 8; fn <= 15; ++fn) {
            const int rounded[] = {2, 2, 3, 2};
            fp_setup(cached);
            if (fmt == 16) cpu.cp1.regs[1].float32[0] = 2.5f;
            else cpu.cp1.regs[1].float64 = 2.5;
            execute_test_op(fp_op(fmt, fn));
            assert(cpu.cp1.regs[3].dword == rounded[fn & 3]);
            assert((cpu.cp1.fcr31 & 0x1004) == 0x1004);
        }
        for (unsigned enabled = 0; enabled < 2; ++enabled) {
            fp_setup(cached);
            if (fmt == 16) cpu.cp1.regs[1].float32[0] = 1;
            else cpu.cp1.regs[1].float64 = 1;
            cpu.cp1.fcr31 = enabled ? 0x400 : 0;
            execute_test_op(fp_op(fmt, 3)); /* 1 / 0 */
            assert(cpu.cp1.fcr31 & 0x8000);
            if (enabled) {
                assert(test_exception_code() == CP0_CAUSE_EXCCODE_FPE);
                assert(cpu.cp1.regs[3].dword == INT64_C(0x123456789abcdef0));
                assert(!(cpu.cp1.fcr31 & 0x20));
                assert(cpu.cp0.regs[CP0_COUNT_REG] == 0 && cpu.cp0.count_phase == 1);
            } else {
                assert(cpu.cp1.fcr31 & 0x20);
                assert((uint64_t)cpu.cp1.regs[3].dword ==
                    (fmt == 16 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000)));
            }
        }
        for (unsigned kind = 0; kind < 3; ++kind) {
            fp_setup(cached);
            cpu.cp1.regs[1].dword = kind == 0 ? 1 : fmt == 16 ?
                (kind == 1 ? 0x7fbfffff : 0x7fc00000) :
                (kind == 1 ? INT64_C(0x7ff7ffffffffffff) : INT64_C(0x7ff8000000000000));
            execute_test_op(fp_op(fmt, 0));
            if (kind < 2) {
                assert(cpu.cp1.fcr31 & 0x20000);
                assert(cpu.cp1.regs[3].dword == INT64_C(0x123456789abcdef0));
            } else {
                assert((cpu.cp1.fcr31 & 0x10040) == 0x10040);
                assert((uint64_t)cpu.cp1.regs[3].dword ==
                    (fmt == 16 ? UINT64_C(0x7fbfffff) : UINT64_C(0x7ff7ffffffffffff)));
            }
        }
        for (unsigned rm = 0; rm < 4; ++rm) {
            fp_setup(cached);
            cpu.cp1.fcr31 = 0x1000000 | rm;
            cpu.cp1.regs[1].dword = fmt == 16 ? 0x00800000 : INT64_C(0x0010000000000000);
            if (fmt == 16) cpu.cp1.regs[2].float32[0] = 0.5f;
            else cpu.cp1.regs[2].float64 = 0.5;
            execute_test_op(fp_op(fmt, 2));
            assert((cpu.cp1.fcr31 & 0x300c) == 0x300c);
            assert(cpu.cp1.regs[3].dword == (rm == 2 ? cpu.cp1.regs[1].dword : 0));
        }
        for (unsigned mode = 0; mode < 3; ++mode) {
            fp_setup(cached);
            cpu.cp1.fcr31 = mode == 0 ? 0 : 0x1000000 | (mode == 1 ? 0x80 : 0x100);
            cpu.cp1.regs[1].dword = fmt == 16 ? 0x00800000 : INT64_C(0x0010000000000000);
            if (fmt == 16) cpu.cp1.regs[2].float32[0] = 0.5f;
            else cpu.cp1.regs[2].float64 = 0.5;
            execute_test_op(fp_op(fmt, 2));
            assert(cpu.cp1.fcr31 & 0x20000);
            assert(cpu.cp1.regs[3].dword == INT64_C(0x123456789abcdef0));
        }
        fp_setup(cached);
        cpu.cp1.regs[1].dword = fmt == 16 ? 0x7f7fffff : INT64_C(0x7fefffffffffffff);
        if (fmt == 16) cpu.cp1.regs[2].float32[0] = 2;
        else cpu.cp1.regs[2].float64 = 2;
        execute_test_op(fp_op(fmt, 2));
        assert((cpu.cp1.fcr31 & 0x5014) == 0x5014); /* Overflow plus inexact. */
        fp_setup(cached);
        cpu.cp1.fcr31 = 0x800;
        if (fmt == 16) cpu.cp1.regs[1].float32[0] = -1;
        else cpu.cp1.regs[1].float64 = -1;
        execute_test_op(fp_op(fmt, 4));
        assert((cpu.cp1.fcr31 & 0x10040) == 0x10000);
        assert(cpu.cp1.regs[3].dword == INT64_C(0x123456789abcdef0));
    }
    for (unsigned fmt = 20; fmt <= 21; ++fmt) {
        for (unsigned fn = 32; fn <= 33; ++fn) {
            fp_setup(cached);
            cpu.cp1.regs[1].dword = -123;
            execute_test_op(fp_op(fmt, fn));
            assert((fn == 32 ? cpu.cp1.regs[3].float32[0] : cpu.cp1.regs[3].float64) == -123);
        }
    }
    for (unsigned fmt = 16; fmt <= 17; ++fmt) {
        fp_setup(cached);
        if (fmt == 16) cpu.cp1.regs[1].float32[0] = 1.5f;
        else cpu.cp1.regs[1].float64 = 1.5;
        execute_test_op(fp_op(fmt, fmt == 16 ? 33 : 32));
        assert((fmt == 16 ? cpu.cp1.regs[3].float64 : cpu.cp1.regs[3].float32[0]) == 1.5);
    }
    for (unsigned rm = 0; rm < 4; ++rm) {
        const int rounded[] = {-2, -2, -2, -3};
        fp_setup(cached);
        cpu.cp1.fcr31 = rm;
        cpu.cp1.regs[1].float64 = -2.5;
        execute_test_op(fp_op(17, 36));
        assert((uint64_t)cpu.cp1.regs[3].dword == (uint32_t)rounded[rm]);
    }
    const double invalid[] = {2147483648.0, -2147483649.0, 2147483647.75};
    for (unsigned i = 0; i < 3; ++i) {
        fp_setup(cached);
        cpu.cp1.regs[1].float64 = invalid[i];
        execute_test_op(fp_op(17, 36));
        assert(cpu.cp1.fcr31 & 0x20000);
        assert(cpu.cp1.regs[3].dword == INT64_C(0x123456789abcdef0));
    }
    fp_setup(cached);
    cpu.cp1.regs[1].float64 = 9007199254740992.0;
    execute_test_op(fp_op(17, 37));
    assert(cpu.cp1.fcr31 & 0x20000);
    fp_setup(cached);
    cpu.cp1.regs[1].dword = INT64_C(0x0080000000000000);
    execute_test_op(fp_op(21, 33));
    assert(cpu.cp1.fcr31 & 0x20000);

    /* FR=0 arithmetic sources pair FS, but read FT and write FD directly. */
    fp_setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] &= ~CP0_STATUS_FR;
    cpu.cp1.regs[0].float32[0] = 2;
    cpu.cp1.regs[1].float32[0] = 99;
    cpu.cp1.regs[2].float32[0] = 3;
    execute_test_op(fp_op(16, 0));
    assert(cpu.cp1.regs[3].float32[0] == 5 && cpu.cp1.regs[2].float32[0] == 3);

    /* Host rounding and exception flags survive guest arithmetic. */
    fp_setup(cached);
    cpu.cp1.regs[1].float32[0] = 1;
    cpu.cp1.regs[2].float32[0] = 3;
    fenv_t env;
    fegetenv(&env);
    fesetround(FE_DOWNWARD);
    feraiseexcept(FE_OVERFLOW);
    int flags = fetestexcept(FE_ALL_EXCEPT);
    execute_test_op(fp_op(16, 3));
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == flags);
    fesetenv(&env);
}

/* Explicit bit expectations remain valid when the test driver uses fast-math. */
static void check_fast_fp(int cached)
{
    /* DK64 boot: TRUNC.W.D on this ordinary finite value must not also
     * execute a single-precision conversion of its unrelated low word. */
    fp_setup(cached);
    cpu.cp1.fcr31 = 0x01801c00;
    cpu.cp1.regs[1].dword = INT64_C(0x3ff1fffffe632358);
    execute_test_op(fp_op(17, 13));
    assert(cpu.interp_PC.addr == 0x80000004);
    assert(cpu.cp1.regs[3].dword == 1);
    assert((cpu.cp1.fcr31 & 0x3f000) == 0x1000);
    /* The reverse case: the unused high word makes the raw 64-bit register
     * a NaN, but the single-precision operand is just 2.5. */
    fp_setup(cached);
    cpu.cp1.fcr31 = 0x01801c00;
    cpu.cp1.regs[1].dword = INT64_C(0x7ff8000040200000);
    execute_test_op(fp_op(16, 13));
    assert(cpu.interp_PC.addr == 0x80000004);
    assert(cpu.cp1.regs[3].dword == 2);
    assert((cpu.cp1.fcr31 & 0x3f000) == 0x1000);
    for (unsigned fmt = 16; fmt <= 17; ++fmt) {
        uint64_t one = fmt == 16 ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
        uint64_t half_ulp = fmt == 16 ? UINT64_C(0x33800000) : UINT64_C(0x3ca0000000000000);
        uint64_t sign = fmt == 16 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
        for (unsigned rm = 0; rm < 4; ++rm) {
            fp_setup(cached);
            cpu.cp1.fcr31 = rm;
            cpu.cp1.regs[1].dword = (int64_t)one;
            cpu.cp1.regs[2].dword = (int64_t)half_ulp;
#ifdef OSAL_SSE
            unsigned int saved = _mm_getcsr();
            unsigned int host = saved | 0x8040 | 0x20; /* FTZ, DAZ, old inexact. */
            _mm_setcsr(host);
#endif
            execute_test_op(fp_op(fmt, 0));
            assert((uint64_t)cpu.cp1.regs[3].dword == one + (rm == 2));
            assert((cpu.cp1.fcr31 & 0x1004) == 0x1004);
#ifdef OSAL_SSE
            assert(_mm_getcsr() == host);
            _mm_setcsr(saved);
#endif
            cpu.cp1.regs[1].dword = (int64_t)(one | sign);
            cpu.cp1.regs[2].dword = (int64_t)(half_ulp | sign);
            execute_test_op(fp_op(fmt, 0));
            assert((uint64_t)cpu.cp1.regs[3].dword == (one | sign) + (rm == 3));
        }
        fp_setup(cached);
        cpu.cp1.regs[1].dword = (int64_t)sign; /* -0 */
        cpu.cp1.regs[2].dword = (int64_t)one;
        execute_test_op(fp_op(fmt, 2));
        assert((uint64_t)cpu.cp1.regs[3].dword == sign);
        execute_test_op(fp_op(fmt, 5)); /* ABS(-0) */
        assert(cpu.cp1.regs[3].dword == 0);
        cpu.cp1.regs[1].dword = 0;
        execute_test_op(fp_op(fmt, 7)); /* NEG(+0) */
        assert((uint64_t)cpu.cp1.regs[3].dword == sign);
        cpu.cp1.regs[1].dword = (int64_t)(one + (fmt == 16 ? 0x800000 : UINT64_C(0x10000000000000)));
        execute_test_op(fp_op(fmt, 4)); /* SQRT(2) must report inexact. */
        assert((cpu.cp1.fcr31 & 0x1004) == 0x1004);

        /* Compare subnormals with host DAZ enabled, and treat signed zeros equally. */
        cpu.cp1.regs[1].dword = 0;
        cpu.cp1.regs[2].dword = 1;
#ifdef OSAL_SSE
        unsigned int saved = _mm_getcsr();
        _mm_setcsr(saved | 0x8040);
#endif
        execute_test_op(fp_op(fmt, 0x34)); /* C.OLT */
        assert(cpu.cp1.fcr31 & 0x800000);
        cpu.cp1.regs[2].dword = (int64_t)sign;
        execute_test_op(fp_op(fmt, 0x32)); /* C.EQ */
        assert(cpu.cp1.fcr31 & 0x800000);
#ifdef OSAL_SSE
        _mm_setcsr(saved);
#endif
    }
    for (unsigned rm = 0; rm < 4; ++rm) {
        fp_setup(cached);
        cpu.cp1.fcr31 = rm;
        cpu.cp1.regs[1].dword = 16777217;
        execute_test_op(fp_op(20, 32));
        assert(cpu.cp1.regs[3].dword == 0x4b800000 + (rm == 2));
        assert((cpu.cp1.fcr31 & 0x1004) == 0x1004);
    }
}
static void control_write(unsigned reg, uint64_t data, int wide)
{
    cpu.regs[1] = (int64_t)data;
    execute_test_op(0x40810000 | (wide ? 0x00200000 : 0) | (reg << 11));
}

static uint64_t control_read(unsigned reg, int wide)
{
    execute_test_op(0x40020000 | (wide ? 0x00200000 : 0) | (reg << 11));
    return (uint64_t)cpu.regs[2];
}

static void check_cop0_registers(int cached)
{
    static const struct { unsigned reg; uint64_t expected; } masks[] = {
        {0, 0x8000003f}, {2, 0x3fffffff}, {3, 0x3fffffff}, {5, 0x01ffe000},
        {6, 63}, {10, UINT64_C(0xc00000ffffffe0ff)}, {17, 0xffffffff},
        {18, 0xfffffffb}, {19, 15}, {26, 255}, {27, 0}, {28, 0xffffffff}, {29, 0}
    };
    setup(cached);
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        control_write(masks[i].reg, UINT64_MAX, 1);
        assert(control_read(masks[i].reg, 1) == masks[i].expected);
    }
    for (unsigned wide = 0; wide < 2; ++wide) {
        control_write(14, UINT64_C(0x1234567887654321), wide);
        assert(control_read(14, 1) == UINT64_C(0x1234567887654321));
        assert(control_read(14, 0) == UINT64_C(0xffffffff87654321));
        control_write(30, UINT64_C(0x9876543212345678), wide);
        assert(control_read(30, 1) == UINT64_C(0x9876543212345678));
    }
    control_write(4, UINT64_MAX, 1);
    assert(control_read(4, 1) == UINT64_C(0xfffffffffffffff0));
    control_write(20, UINT64_MAX, 1);
    assert(control_read(20, 1) == UINT64_C(0xfffffffe00000000));
    control_write(0, 3, 0);
    control_write(10, UINT64_C(0xc00000801234405a), 1);
    execute_test_op(0x42000002); /* TLBWI */
    control_write(10, 0, 1);
    execute_test_op(0x42000001); /* TLBR */
    assert(control_read(10, 1) == UINT64_C(0xc00000801234405a));
    execute_test_op(0x42000008); /* TLBP */
    assert(control_read(0, 1) == 3);
    control_write(15, UINT64_C(0x123456789abcdef0), 1);
    assert(control_read(15, 1) == 0xb00);
    assert(control_read(7, 1) == UINT64_C(0x123456789abcdef0));
    assert(control_read(31, 0) == UINT64_C(0xffffffff9abcdef0));
    control_write(16, UINT64_MAX, 1);
    assert(control_read(16, 1) == 0x0f06e46f);
    control_write(12, UINT64_MAX, 1);
    assert(control_read(12, 1) == 0xff57ffff);

    setup(cached);
    control_write(6, 29, 0);
    uint32_t seen = 0;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t value = control_read(1, 1);
        assert(value >= 29 && value <= 31);
        seen |= 1 << (value - 29);
    }
    assert(seen == 7);
    control_write(6, 63, 0);
    for (unsigned i = 0; i < 64; ++i) assert(control_read(1, 1) < 64);

    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] = CP0_STATUS_IE | CP0_STATUS_IM0;
    control_write(13, CP0_CAUSE_IP0, 0);
    assert(test_exception_code() == 0 && (cpu.cp0.regs[12] & CP0_STATUS_EXL));
    assert(control_read(14, 1) == UINT64_C(0xffffffff80000004));
    setup(cached);
    cpu.cp0.regs[13] = CP0_CAUSE_EXCCODE_BP;
    control_write(13, CP0_CAUSE_IP0, 0);
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_BP);
    execute_test_op(0x40410000); /* CFC0 is a no-op. */
    assert(cpu.interp_PC.addr == 0x80000004);
}
