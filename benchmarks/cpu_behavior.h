/* Shares the ROM-free fixture in cpu_timing.c. */
static void execute_test_op(uint32_t op)
{
    code[0] = op;
    cpu.interp_PC.addr = 0x80000000;
    cached_interp_invalidate(&cpu, 0x80000000, 4);
    cpu.execute_one(&cpu);
    assert(cpu.regs[0] == 0);
}

static uint32_t test_exception_code(void)
{
    return cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_EXCCODE_MASK;
}

static void check_integer_behavior(int cached)
{
    uint64_t random = UINT64_C(0x193a872cdf839482);
    setup(cached);
    for (unsigned i = 0; i < 256; ++i) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        cpu.regs[1] = (int64_t)random;
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        cpu.regs[2] = (int64_t)random;
        int64_t rhs35 = (int64_t)((uint64_t)cpu.regs[2] << 29) >> 29;
        uint64_t product = (uint64_t)cpu.regs[1] * (uint64_t)rhs35;
        execute_test_op(0x00220018); /* MULT */
        assert(cpu.lo == (int32_t)product && cpu.hi == (int32_t)(product >> 32));
        int64_t numerator = (int32_t)cpu.regs[1], denominator = cpu.regs[2];
        execute_test_op(0x0022001a); /* DIV uses full signed divisor */
        assert(cpu.lo == (int32_t)(numerator / denominator));
        assert(cpu.hi == (int32_t)(numerator % denominator));
        execute_test_op(0x00221807); /* SRAV */
        assert(cpu.regs[3] == (int32_t)(cpu.regs[2] >> (cpu.regs[1] & 31)));
        execute_test_op(0x00021803 | ((i & 31) << 6)); /* SRA */
        assert(cpu.regs[3] == (int32_t)(cpu.regs[2] >> (i & 31)));
    }
    setup(cached);
    cpu.regs[1] = INT32_MAX; cpu.regs[2] = 1;
    execute_test_op(0x00220020); /* ADD $zero must still overflow */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_OV);
    assert(cpu.cp0.regs[CP0_EPC_REG] == 0x80000000);

    setup(cached);
    cpu.regs[1] = INT64_MIN; cpu.regs[2] = -1;
    execute_test_op(0x0022001e);
    assert(cpu.lo == INT64_MIN && cpu.hi == 0);

    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] = CP0_STATUS_MODE_U;
    execute_test_op(0x0022002d); /* DADDU $zero still traps in 32-bit user mode */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_RI);
    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] = CP0_STATUS_MODE_U;
    execute_test_op(0x40004800); /* MFC0 $zero requires CU0 */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_CPU);
}

static void check_memory_behavior(int cached)
{
    setup(cached);
    cpu.regs[1] = 0x80000100;
    code[64] = 0xdeadbeef;
    execute_test_op(0x8c200000); /* LW $zero must read */
    assert(reads == 1);
    execute_test_op(0xc0200000); /* LL $zero must establish reservation */
    assert(reads == 2 && cpu.llbit == 1 && cpu.cp0.regs[CP0_LLADDR_REG] == 16);
    execute_test_op(0xe0200000); /* SC $zero stores zero */
    assert(writes == 1 && code[64] == 0 && cpu.llbit == 1);

    setup(cached);
    cpu.regs[1] = 0x80000100;
    code[64] = 0x89abcdef; code[65] = 0x12345678;
    execute_test_op(0xd0220000); /* LLD r2 */
    assert((uint64_t)cpu.regs[2] == UINT64_C(0x89abcdef12345678));
    assert(cpu.llbit == 1 && cpu.cp0.regs[CP0_LLADDR_REG] == 16);
    cpu.regs[2] = UINT64_C(0x76543210fedcba98);
    execute_test_op(0xf0220000); /* SCD r2 */
    assert(cpu.regs[2] == 1 && code[64] == 0x76543210 && code[65] == 0xfedcba98);
    assert(cpu.llbit == 1); /* Ares only clears this at ERET. */

    setup(cached);
    cpu.regs[1] = 0x80000100;
    cpu.regs[2] = UINT64_C(0x1234567887654321);
    code[64] = 0xaabbccdd;
    execute_test_op(0x98220000); /* partial LWR preserves upper 32 bits */
    assert((uint64_t)cpu.regs[2] == UINT64_C(0x12345678876543aa));
    execute_test_op(0x98220003); /* full LWR sign extends */
    assert(cpu.regs[2] == (int32_t)0xaabbccdd);

    const unsigned int ops[] = {0x21, 0x25, 0x23, 0x27, 0x30, 0x34, 0x37, 0x29, 0x2b, 0x3f};
    for (unsigned i = 0; i < sizeof(ops)/sizeof(ops[0]); ++i) {
        setup(cached);
        cpu.regs[1] = 0x80000101;
        execute_test_op((ops[i] << 26) | (1 << 21));
        assert(test_exception_code() == ((ops[i] == 0x29 || ops[i] == 0x2b || ops[i] == 0x3f)
            ? CP0_CAUSE_EXCCODE_ADES : CP0_CAUSE_EXCCODE_ADEL));
        assert(cpu.cp0.regs[CP0_BADVADDR_REG] == 0x80000101);
        assert(cpu.cp0.regs[CP0_EPC_REG] == 0x80000000 && reads == 0 && writes == 0);
    }
}

static void check_exception_behavior(int cached)
{
    setup(cached);
    cpu.regs[31] = 1;
    code[1] = 0x24010007; /* delay slot observes the link */
    execute_test_op(0x07f00002); /* BLTZAL ra evaluates after writing ra in Ares */
    assert(cpu.interp_PC.addr == 0x8000000c && cpu.regs[1] == 7);
    assert(cpu.regs[31] == (int32_t)0x80000008);

    setup(cached);
    cpu.cp0.regs[CP0_CAUSE_REG] = CP0_CAUSE_IP7 | CP0_CAUSE_IP2;
    execute_test_op(0x0000000d); /* BREAK */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_BP && !cpu.stop);
    assert((cpu.cp0.regs[CP0_CAUSE_REG] & 0xff00) == (CP0_CAUSE_IP7 | CP0_CAUSE_IP2));
    cpu.cp0.regs[CP0_EPC_REG] = 0x12345678;
    cpu.cp0.regs[CP0_CAUSE_REG] |= CP0_CAUSE_BD;
    execute_test_op(0x0000000c); /* nested SYSCALL */
    assert(cpu.cp0.regs[CP0_EPC_REG] == 0x12345678);
    assert(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_BD);
    raise_maskable_interrupt(&cpu, CP0_CAUSE_IP2);
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_SYS);

    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] |= CP0_STATUS_BEV;
    execute_test_op(0x70000000); /* reserved opcode */
    assert(cpu.interp_PC.addr == 0xbfc00380 && test_exception_code() == CP0_CAUSE_EXCCODE_RI);

    setup(cached);
    code[0] = 0x10000002; code[1] = 0x0000000d;
    cpu.execute_one(&cpu);
    assert(cpu.interp_PC.addr == 0x80000180 && cpu.cp0.regs[CP0_EPC_REG] == 0x80000000);
    assert(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_BD);

    setup(cached);
    cpu.cp0.regs[CP0_ERROREPC_REG] = 0x80000200;
    cpu.cp0.regs[CP0_STATUS_REG] |= CP0_STATUS_ERL;
    cpu.llbit = 1;
    execute_test_op(0x42000018); /* ERET from ERL */
    assert(cpu.interp_PC.addr == 0x80000200 && cpu.llbit == 0);
    assert(!(cpu.cp0.regs[CP0_STATUS_REG] & CP0_STATUS_ERL));

    setup(cached);
    cpu.interp_PC.addr = 0x80000001;
    cpu.execute_one(&cpu);
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_ADEL);
    assert(cpu.cp0.regs[CP0_BADVADDR_REG] == 0x80000001);

    setup(cached);
    cpu.cp0.regs[CP0_INDEX_REG] = 63;
    execute_test_op(0x42000002); /* invalid TLB index is ignored, never OOB */
    assert(cpu.interp_PC.addr == 0x80000004);
    cpu.cp0.regs[CP0_WIRED_REG] = 63;
    execute_test_op(0x40010800); /* RANDOM with WIRED > 31 */
    assert(cpu.regs[1] >= 0 && cpu.regs[1] <= 63);
}

static void check_fpu_behavior(int cached)
{
    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] &= ~CP0_STATUS_CU1;
    execute_test_op(0x44000000); /* MFC1 $zero must trap */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_CPU);
    assert(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_CE1);

    setup(cached);
    cpu.cp1.regs[2].dword = UINT64_C(0xdeadbeef3f800000);
    execute_test_op(0x460010c6); /* MOV.S f3,f2 copies all 64 bits */
    assert(cpu.cp1.regs[3].dword == cpu.cp1.regs[2].dword);
    cpu.regs[1] = 42;
    execute_test_op(0x44411000); /* CFC1 unsupported control register => zero */
    assert(cpu.regs[1] == 0);

    setup(cached);
    cpu.regs[1] = 0x10800; /* Cause.V + Enable.V */
    execute_test_op(0x44c1f800);
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_FPE);

    for (unsigned fmt = 16; fmt <= 17; ++fmt) {
        for (unsigned pred = 0; pred < 16; ++pred) {
            setup(cached);
            if (fmt == 16) { cpu.cp1.regs[2].float32[0] = -1; cpu.cp1.regs[4].float32[0] = 2; }
            else { cpu.cp1.regs[2].float64 = -1; cpu.cp1.regs[4].float64 = 2; }
            execute_test_op(0x44041030 | (fmt << 21) | pred);
            assert(!!(cpu.cp1.fcr31 & 0x800000) == !!(pred & 4));
            for (unsigned signaling = 0; signaling < 2; ++signaling) {
                setup(cached);
                cpu.cp1.regs[2].dword = fmt == 16
                    ? (signaling ? 0x7fc00001 : 0x7f800001)
                    : (signaling ? UINT64_C(0x7ff8000000000001) : UINT64_C(0x7ff0000000000001));
                cpu.cp1.regs[4].dword = 0;
                cpu.cp1.fcr31 = 0x3f000; /* Each compare clears old cause bits. */
                execute_test_op(0x44041030 | (fmt << 21) | pred);
                assert(!cpu.stop && cpu.interp_PC.addr == 0x80000004);
                assert(!!(cpu.cp1.fcr31 & 0x800000) == !!(pred & 1));
                assert(!!(cpu.cp1.fcr31 & 0x10040) == !!(signaling || (pred & 8)));
                assert(!(cpu.cp1.fcr31 & 0x2f000));
            }
        }
    }
    setup(cached);
    cpu.cp1.regs[2].dword = 0x7fc00001;
    cpu.cp1.fcr31 = 0x800800; /* enabled invalid, previous condition true */
    execute_test_op(0x46041032); /* C.EQ.S */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_FPE && !cpu.stop);
    assert(cpu.cp1.fcr31 == 0x810800); /* cause, no sticky flag, condition retained */

    setup(cached);
    execute_test_op(0x46000020); /* unimplemented CVT.S.S */
    assert(test_exception_code() == CP0_CAUSE_EXCCODE_FPE && (cpu.cp1.fcr31 & 0x20000));
}

static void check_behavior(void)
{
    for (int cached = 0; cached <= 1; ++cached) {
        check_integer_behavior(cached);
        check_memory_behavior(cached);
        check_exception_behavior(cached);
        check_fpu_behavior(cached);
    }
}
