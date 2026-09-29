#ifndef M64P_DEVICE_R4300_TIMING_H
#define M64P_DEVICE_R4300_TIMING_H

#include "cp0.h"
#include "r4300_core.h"

/* Ares n64/cpu/interpreter-{ipu,fpu}.cpp: total CPU cycles, including
 * the one-cycle baseline. Cache misses and bus stalls are not modeled here.
 * https://github.com/ares-emulator/ares/tree/master/ares/n64/cpu
 */
static inline unsigned int r4300_base_cycles(uint32_t op, uint32_t status)
{
    unsigned int fmt = (op >> 21) & 31;
    unsigned int fn = op & 63;
    if ((op >> 26) == 0) {
        switch (fn) {
        case 0x18: case 0x19: return 5;  /* MULT/U */
        case 0x1a: case 0x1b: return 37; /* DIV/U */
        case 0x1c: case 0x1d: return 8;  /* DMULT/U */
        case 0x1e: case 0x1f: return 69; /* DDIV/U */
        }
    }
    if ((op >> 26) == 0x11 && (status & CP0_STATUS_CU1)) {
        if (fmt == 16 || fmt == 17) {
            switch (fn) {
            case 0x00: case 0x01: return 3; /* ADD, SUB */
            case 0x02: return fmt == 16 ? 5 : 8; /* MUL */
            case 0x03: case 0x04: return fmt == 16 ? 29 : 58;
            case 0x08: case 0x09: case 0x0a: case 0x0b:
            case 0x0c: case 0x0d: case 0x0e: case 0x0f:
            case 0x24: case 0x25: return 5; /* to integer */
            case 0x20: return fmt == 17 ? 2 : 1; /* CVT.S.D */
            }
        } else if ((fmt == 20 || fmt == 21) && (fn == 0x20 || fn == 0x21)) {
            return 5; /* CVT.S/D.W/L */
        }
    }
    return 1;
}

static inline void cp0_step_cycles(struct cp0* cp0, unsigned int cycles)
{
    /* COUNT runs at half the CPU clock. Retain odd cycles across dispatches,
     * register reads and control-flow changes instead of rounding each time. */
    unsigned int total = cycles + cp0->count_phase;
    uint32_t ticks = total >> 1;
    cp0->count_phase = total & 1;
    cp0->regs[CP0_COUNT_REG] += ticks;
    cp0->cycle_count = (int32_t)((uint32_t)cp0->cycle_count + ticks);
}

static inline int r4300_begin_instruction(struct r4300_core* r4300, uint32_t op, uint32_t pc)
{
    uint32_t status = r4300->cp0.regs[CP0_STATUS_REG];
    /* COP1 arithmetic charges additional latency only after committing. */
    unsigned int cycles = (op >> 26) == 17 ? 1 : r4300_base_cycles(op, status);
    if ((status & CP0_STATUS_MODE_MASK) && !(status & (CP0_STATUS_EXL | CP0_STATUS_ERL))) {
        r4300->interp_PC.addr = pc;
        cp0_step_cycles(&r4300->cp0, 1);
        if (check_instruction_mode(r4300, op)) return 0;
        cp0_step_cycles(&r4300->cp0, cycles - 1);
    } else {
        cp0_step_cycles(&r4300->cp0, cycles);
    }
    return 1;
}

#endif
