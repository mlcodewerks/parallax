/* Standalone CPU timing regression tests; no ROM or graphics backend needed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "device/r4300/r4300_core.h"
#include "device/r4300/pure_interp.h"
#include "device/r4300/cached_interp.h"
#include "device/r4300/timing.h"
#include "device/memory.h"
#include "device/rcp/mi_controller.h"
#include "api/callbacks.h"

int g_gs_vi_counter;
unsigned int IgnoreTLBExceptions;
struct pif;
struct device;
void main_check_inputs(void) { }
void reset_pif(struct pif* pif, int reset) { (void)pif; (void)reset; abort(); }
void poweron_device(struct device* dev) { (void)dev; abort(); }
void pif_bootrom_hle_execute(struct r4300_core* core) { (void)core; abort(); }
void DebugMessage(int level, const char* message, ...) { (void)level; (void)message; }
uint32_t* mem_base_u32(void* base, uint32_t address) { return (uint32_t*)((char*)base + address); }
void read_rdram_dram(void* opaque, uint32_t address, uint32_t* value)
{
    (void)opaque; (void)address; (void)value; abort();
}
void write_rdram_dram(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    (void)opaque; (void)address; (void)value; (void)mask; abort();
}

static struct r4300_core cpu;
static struct memory memory;
static struct mi_controller mi;
static uint32_t code[1024];
static unsigned int reads, writes;

static void read_word(void* opaque, uint32_t address, uint32_t* value)
{
    (void)opaque;
    assert(address < sizeof(code));
    ++reads;
    *value = code[address >> 2];
}

static void write_word(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    (void)opaque;
    assert(address < sizeof(code));
    ++writes;
    code[address >> 2] = (code[address >> 2] & ~mask) | (value & mask);
}

static void setup(int cached)
{
    struct interrupt_handler handlers[CP0_INTERRUPT_HANDLERS_COUNT] = {0};
    cached_interp_destroy(&cpu);
    memset(&cpu, 0, sizeof(cpu));
    memset(code, 0, sizeof(code));
    memory.base = code;
    cpu.mem = &memory;
    cpu.mi = &mi;
    memory.handlers[0].read32 = read_word;
    memory.handlers[0].write32 = write_word;
    reads = writes = 0;
    cpu.pc = &cpu.interp_PC;
    cpu.interp_PC.addr = 0x80000000;
    handlers[2].callback = check_int_handler;
    handlers[2].opaque = &cpu;
    handlers[1].callback = compare_int_handler;
    handlers[1].opaque = &cpu;
    init_cp0(&cpu.cp0, 3, 2, handlers); /* Legacy scaling must not affect timing. */
    poweron_cp0(&cpu.cp0);
    poweron_cp1(&cpu.cp1);
    translate_event_queue(&cpu.cp0, 0);
    cpu.execute_one = cached ? cached_interp_execute_one : pure_interp_execute_one;
    if (cached) assert(cached_interp_init(&cpu));
    breakloop = false;
}

static void stop_event(void* opaque)
{
    (void)opaque;
    breakloop = true;
}

static void check_execution(int cached)
{
    setup(cached);
    code[0] = 0x40014800; /* MFC0 r1, COUNT */
    code[1] = 0x40024800;
    cpu.execute_one(&cpu);
    assert(cpu.regs[1] == 0 && cpu.cp0.count_phase == 1);
    cpu.execute_one(&cpu);
    assert(cpu.regs[2] == 1 && cpu.cp0.count_phase == 0);
    cp0_update_count(&cpu);
    cpu.interp_PC.addr += 100;
    cp0_update_count(&cpu);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 1);

    setup(cached);
    cpu.regs[1] = 1;
    code[0] = 0x50010002; /* BEQL zero,r1: annul slot */
    code[1] = 0x0000001e; /* DDIV: must not charge annulled slot */
    cpu.execute_one(&cpu);
    assert(cpu.interp_PC.addr == 0x80000008);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 0 && cpu.cp0.count_phase == 1);

    setup(cached);
    code[0] = 0x10000002; /* BEQ with DIV delay slot */
    code[1] = 0x0000001a;
    cpu.execute_one(&cpu);
    assert(cpu.interp_PC.addr == 0x8000000c);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 19 && cpu.cp0.count_phase == 0);

    setup(cached);
    code[0] = 0x0000001e; /* DDIV crosses COMPARE */
    cpu.cp0.regs[CP0_COMPARE_REG] = 4;
    schedule_compare(&cpu.cp0);
    cpu.execute_one(&cpu);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 34 && cpu.cp0.count_phase == 1);
    assert(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_IP7);

    setup(cached);
    code[0] = 0x44800000; /* MTC1: ordinary one-cycle instruction */
    code[1] = 0x46000002; /* MUL.S = 5 */
    code[2] = 0x46200004; /* SQRT.D = 58 */
    cpu.execute_one(&cpu);
    cpu.execute_one(&cpu);
    cpu.execute_one(&cpu);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 32 && cpu.cp0.count_phase == 0);

    setup(cached);
    cpu.regs[1] = 123;
    code[0] = 0x40814800; /* MTC0 r1, COUNT resets divider */
    code[1] = 0x40024800;
    cpu.execute_one(&cpu);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 123 && cpu.cp0.count_phase == 0);
    cpu.execute_one(&cpu);
    assert(cpu.regs[2] == 123 && cpu.cp0.count_phase == 1);

    setup(cached);
    cpu.regs[1] = 1;
    cpu.cp0.regs[CP0_CAUSE_REG] |= CP0_CAUSE_IP7;
    code[0] = 0x40815800; /* MTC0 r1, COMPARE: clear pending timer */
    cpu.execute_one(&cpu);
    assert(!(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_IP7));
    cpu.execute_one(&cpu); /* NOP supplies second half of COUNT tick */
    assert(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_IP7);

    setup(cached);
    code[0] = 0x0000000c; /* SYSCALL: exception PC must not become cycles */
    cpu.execute_one(&cpu);
    assert(cpu.interp_PC.addr == 0x80000180);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 0 && cpu.cp0.count_phase == 1);
    cpu.execute_one(&cpu);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 1);

    setup(cached);
    cpu.cp0.regs[CP0_STATUS_REG] &= ~CP0_STATUS_CU1;
    code[0] = 0x46200003; /* Disabled COP1 traps without DIV.D latency. */
    cpu.execute_one(&cpu);
    assert(cpu.interp_PC.addr == 0x80000180);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 0 && cpu.cp0.count_phase == 1);

    /* Continuous dispatch must service events in straight-line fast ops. */
    setup(cached);
    for (unsigned i = 0; i < 100; ++i) code[i] = 0x24210001; /* ADDIU r1,r1,1 */
    cpu.cp0.interrupt_handlers[4].callback = stop_event;
    add_interrupt_event(&cpu.cp0, PI_INT, 10);
    if (cached) cached_interp_run(&cpu);
    else {
        cpu.startup = 0;
        cpu.emumode = EMUMODE_PURE_INTERPRETER;
        run_r4300(&cpu);
    }
    assert(cpu.regs[1] == 20 && cpu.cp0.regs[CP0_COUNT_REG] == 10);
    assert(cpu.interp_PC.addr == 0x80000050);
}

static void check_compare(void)
{
    setup(0);
    translate_event_queue(&cpu.cp0, 100);
    add_interrupt_event(&cpu.cp0, PI_INT, 10);
    cpu.cp0.regs[CP0_COMPARE_REG] = 101;
    schedule_compare(&cpu.cp0);
    assert(get_next_event_type(&cpu.cp0.q) == COMPARE_INT);
    cpu.cp0.regs[CP0_COMPARE_REG] = 100;
    schedule_compare(&cpu.cp0);
    assert(get_event(&cpu.cp0.q, COMPARE_INT) != NULL);
    assert(cpu.cp0.next_interrupt == 110);
    assert(cpu.cp0.cycle_count == -10);
    translate_event_queue(&cpu.cp0, 200);
    assert(*get_event(&cpu.cp0.q, PI_INT) == 210);
    assert(cpu.cp0.next_interrupt == 210);

    setup(0);
    translate_event_queue(&cpu.cp0, UINT32_MAX);
    cpu.cp0.regs[CP0_COMPARE_REG] = 0;
    schedule_compare(&cpu.cp0);
    cp0_step_cycles(&cpu.cp0, 2);
    assert(cpu.cp0.regs[CP0_COUNT_REG] == 0);
    while (cpu.cp0.cycle_count >= 0) gen_interrupt(&cpu);
    assert(cpu.cp0.regs[CP0_CAUSE_REG] & CP0_CAUSE_IP7);
    assert(get_event(&cpu.cp0.q, COMPARE_INT) != NULL);
    assert(cpu.cp0.cycle_count == -INT64_C(0x100000000));

    /* Equality means the next timer match is one full 32-bit COUNT wrap away. */
    setup(0);
    translate_event_queue(&cpu.cp0, 123);
    cpu.cp0.regs[CP0_COMPARE_REG] = 123;
    schedule_compare(&cpu.cp0);
    assert(cpu.cp0.cycle_count == -INT64_C(0x100000000));

    /* Matches beyond the old signed 2^31 horizon remain directly schedulable. */
    cpu.cp0.regs[CP0_COMPARE_REG] = UINT32_C(0x8000007c);
    schedule_compare(&cpu.cp0);
    assert(cpu.cp0.cycle_count == -INT64_C(0x80000001));

    /* Legacy savestates may still contain the old fake 0x020 event.  Loading
     * must discard it while retaining real events and rebuilding COMPARE. */
    {
        uint32_t legacy_queue[] = {
            UINT32_C(0x020), UINT32_C(0x80000000),
            PI_INT, UINT32_C(150),
            UINT32_C(0xffffffff)
        };
        struct node* e;

        setup(0);
        translate_event_queue(&cpu.cp0, 100);
        cpu.cp0.regs[CP0_COMPARE_REG] = 200;
        load_eventqueue_infos(&cpu.cp0, (const char*)legacy_queue);
        assert(*get_event(&cpu.cp0.q, PI_INT) == 150);
        assert(*get_event(&cpu.cp0.q, COMPARE_INT) == 200);
        for (e = cpu.cp0.q.first; e != NULL; e = e->next)
            assert(e->data.type != 0x020);
    }
}

static void check_latencies(void)
{
    static const unsigned integer[] = {5, 5, 37, 37, 8, 8, 69, 69};
    static const unsigned single[] = {3, 3, 5, 29, 29, 1, 1, 1};
    static const unsigned dual[] = {3, 3, 8, 58, 58, 1, 1, 1};
    for (unsigned i = 0; i < 8; ++i) {
        assert(r4300_base_cycles(0x18 + i, 0) == integer[i]);
        assert(r4300_base_cycles(0x46000000 + i, CP0_STATUS_CU1) == single[i]);
        assert(r4300_base_cycles(0x46200000 + i, CP0_STATUS_CU1) == dual[i]);
        assert(r4300_base_cycles(0x46000000 + i, 0) == 1);
    }
    for (unsigned fmt = 16; fmt <= 17; ++fmt) {
        for (unsigned fn = 8; fn <= 15; ++fn)
            assert(r4300_base_cycles(0x44000000 | (fmt << 21) | fn, CP0_STATUS_CU1) == 5);
        for (unsigned fn = 0x30; fn <= 0x3f; ++fn)
            assert(r4300_base_cycles(0x44000000 | (fmt << 21) | fn, CP0_STATUS_CU1) == 1);
        assert(r4300_base_cycles(0x44000024 | (fmt << 21), CP0_STATUS_CU1) == 5);
        assert(r4300_base_cycles(0x44000025 | (fmt << 21), CP0_STATUS_CU1) == 5);
    }
    assert(r4300_base_cycles(0x46200020, CP0_STATUS_CU1) == 2);
    assert(r4300_base_cycles(0x46000021, CP0_STATUS_CU1) == 1);
    for (unsigned fmt = 20; fmt <= 21; ++fmt) {
        assert(r4300_base_cycles(0x44000020 | (fmt << 21), CP0_STATUS_CU1) == 5);
        assert(r4300_base_cycles(0x44000021 | (fmt << 21), CP0_STATUS_CU1) == 5);
    }
}

#include "cpu_behavior.h"
#include "cpu_fpu_cop0.h"

static void benchmark_fpu(void)
{
    const unsigned functions[] = {0, 2, 3, 4, 36};
    const char* names[] = {"ADD.S", "MUL.S", "DIV.S", "SQRT.S", "CVT.W.S"};
    const unsigned iterations = 2000000;
    for (unsigned i = 0; i < 5; ++i) {
        fp_setup(0);
        cpu.cp1.regs[1].float32[0] = 1.25f;
        cpu.cp1.regs[2].float32[0] = 3.5f;
        uint32_t op = fp_op(16, functions[i]);
        r4300_interp_handler handler = pure_interp_decode(op, 0);
        clock_t start = clock();
        for (unsigned n = 0; n < iterations; ++n) handler(&cpu, op);
        printf("%s: %.1f ns/op\n", names[i],
            (double)(clock() - start) * 1e9 / CLOCKS_PER_SEC / iterations);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) { benchmark_fpu(); return 0; }
    check_behavior();
    check_fp_arithmetic(0);
    check_fp_arithmetic(1);
    check_fast_fp(0);
    check_fast_fp(1);
    check_cop0_registers(0);
    check_cop0_registers(1);
    check_latencies();
    check_execution(0);
    check_execution(1);
    check_compare();
    cached_interp_destroy(&cpu);
    puts("CPU timing and behavior regressions passed (pure and cached).");
    return 0;
}
