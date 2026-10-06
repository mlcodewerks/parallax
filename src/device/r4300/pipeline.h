#ifndef WTFN64_R4300_PIPELINE_H
#define WTFN64_R4300_PIPELINE_H

#include "idec.h"

/* Instruction-boundary model of the VR4300 interlocks.  The interpreter is
 * not a cycle-by-cycle IC/RF/EX/DC/WB implementation, but it preserves the
 * hazards which remain visible between consecutive issued instructions:
 *
 *   - the deliberately imprecise one-cycle load interlock (LDI),
 *   - the one-cycle data-cache-busy interlock after a cached store (DCB),
 *   - the missing COP1 EX->EX result bypass, and
 *   - MTC1/DMTC1 transfer-to-COP1 load-delay behaviour.
 *
 * Cache miss/bus costs remain in cache_timing.h and multi-cycle execution
 * costs remain in timing.h.  The two-entry instruction micro-TLB is modeled
 * alongside instruction fetch in cache_timing.h.
 */
static osal_force_inline uint64_t r4300_pipeline_gpr(unsigned reg)
{
    return reg ? UINT64_C(1) << reg : 0;
}

static osal_force_inline uint64_t r4300_pipeline_fpr(unsigned reg)
{
    return UINT64_C(1) << (32 + reg);
}

static osal_force_inline unsigned int r4300_pipeline_fpr_index(unsigned reg,
                                                               unsigned int fr)
{
    return fr ? reg : reg & ~1u;
}

static osal_force_inline int r4300_pipeline_is_float_instruction(uint32_t op)
{
    switch (op >> 26) {
    case 0x11: /* COP1 */
    case 0x31: /* LWC1 */
    case 0x35: /* LDC1 */
    case 0x39: /* SWC1 */
    case 0x3d: /* SDC1 */
        return 1;
    default:
        return 0;
    }
}

static osal_force_inline int r4300_pipeline_is_fpr_type(enum r4300_register_type type)
{
    return type == IDEC_REGTYPE_FPR || type == IDEC_REGTYPE_FPR32 ||
           type == IDEC_REGTYPE_FPR64;
}

static osal_force_inline uint64_t r4300_pipeline_fpr_operand(uint32_t op,
                                                             uint8_t field,
                                                             unsigned int fr)
{
    enum r4300_register_type type = U53_TYPE(field);
    unsigned int reg;
    if (!r4300_pipeline_is_fpr_type(type)) return 0;
    reg = (op >> U53_SHIFT(field)) & 31;
    return r4300_pipeline_fpr(r4300_pipeline_fpr_index(reg, fr));
}

/* The hardware LDI detector is intentionally imprecise.  For integer loads it
 * compares the load's rt against the following non-COP1 instruction's raw
 * rs/rt fields, even when a field is not actually a source (e.g. LUI, or a
 * second load to the same rt).  COP1 has its analogous FPR field detector. */
static osal_force_inline uint64_t r4300_pipeline_ldi_candidates(uint32_t op,
                                                                uint64_t load_mask,
                                                                unsigned int fr)
{
    unsigned int rs = (op >> 21) & 31;
    unsigned int rt = (op >> 16) & 31;
    unsigned int major = op >> 26;

    if ((load_mask & UINT64_C(0xffffffff00000000)) == 0) {
        if (r4300_pipeline_is_float_instruction(op)) return 0;
        return r4300_pipeline_gpr(rs) | r4300_pipeline_gpr(rt);
    }

    if (!r4300_pipeline_is_float_instruction(op)) return 0;

    if (major == 0x11) {
        /* COP1 computational instructions use fs/ft.  Transfers use fs; BC1
         * consumes FCR31 rather than an FPR and therefore has no FPR LDI. */
        if (rs == 8) return 0;
        if (rs >= 16)
            return r4300_pipeline_fpr(r4300_pipeline_fpr_index((op >> 11) & 31, fr)) |
                   r4300_pipeline_fpr(r4300_pipeline_fpr_index(rt, fr));
        if (rs <= 6)
            return r4300_pipeline_fpr(r4300_pipeline_fpr_index((op >> 11) & 31, fr));
        return 0;
    }

    /* COP1 loads/stores encode ft in the rt field.  Treating that raw field as
     * a dependency intentionally reproduces the hardware's false interlock on
     * back-to-back loads to the same FPR. */
    return r4300_pipeline_fpr(r4300_pipeline_fpr_index(rt, fr));
}

static osal_force_inline uint64_t r4300_pipeline_fpu_reads(uint32_t op,
                                                           const struct r4300_idec* d,
                                                           unsigned int fr)
{
    unsigned int rs = (op >> 21) & 31;
    if ((op >> 26) != 0x11 || rs < 16) return 0;
    return r4300_pipeline_fpr_operand(op, d->u53[1], fr) |
           r4300_pipeline_fpr_operand(op, d->u53[2], fr);
}

static osal_force_inline uint64_t r4300_pipeline_fpu_result(uint32_t op,
                                                            const struct r4300_idec* d,
                                                            unsigned int fr)
{
    unsigned int rs = (op >> 21) & 31;
    if ((op >> 26) != 0x11 || rs < 16 || (op & 63) >= 0x30) return 0;
    return r4300_pipeline_fpr_operand(op, d->u53[0], fr);
}

static osal_force_inline unsigned int r4300_pipeline_cycles(struct r4300_core* r,
                                                            uint32_t op)
{
    const struct r4300_idec* d = r4300_get_idec(op);
    unsigned int major = op >> 26;
    unsigned int rs = (op >> 21) & 31;
    unsigned int rt = (op >> 16) & 31;
    unsigned int fs = (op >> 11) & 31;
    unsigned int fn = op & 63;
    unsigned int fr = (r->cp0.regs[CP0_STATUS_REG] & CP0_STATUS_FR) != 0;
    unsigned int cu1 = (r->cp0.regs[CP0_STATUS_REG] & CP0_STATUS_CU1) != 0;
    uint64_t load = 0;
    uint64_t fpu_result = 0;
    unsigned int fpu_cc = 0;
    unsigned int memory = 0;
    unsigned int dcache_operation = 0;
    unsigned int store = 0;
    unsigned int stalls = 0;

    switch (d->opcode) {
    case R4300_OP_LB: case R4300_OP_LBU: case R4300_OP_LH: case R4300_OP_LHU:
    case R4300_OP_LW: case R4300_OP_LWU: case R4300_OP_LD:
    case R4300_OP_LL: case R4300_OP_LLD:
    case R4300_OP_LWL: case R4300_OP_LWR: case R4300_OP_LDL: case R4300_OP_LDR:
        load = r4300_pipeline_gpr(rt);
        memory = 1;
        break;

    case R4300_OP_LWC1: case R4300_OP_LDC1:
        if (cu1) {
            load = r4300_pipeline_fpr(r4300_pipeline_fpr_index(rt, fr));
            memory = 1;
        }
        break;

    case R4300_OP_MTC1: case R4300_OP_DMTC1:
        if (cu1)
            load = r4300_pipeline_fpr(r4300_pipeline_fpr_index(fs, fr));
        break;

    case R4300_OP_SB: case R4300_OP_SH: case R4300_OP_SW: case R4300_OP_SD:
    case R4300_OP_SWL: case R4300_OP_SWR: case R4300_OP_SDL: case R4300_OP_SDR:
        memory = store = 1;
        break;

    case R4300_OP_SC: case R4300_OP_SCD:
        memory = store = r->llbit != 0;
        break;

    case R4300_OP_SWC1: case R4300_OP_SDC1:
        if (cu1) memory = store = 1;
        break;

    case R4300_OP_CACHE:
        /* Odd CACHE operation encodings target the data cache even when the
         * effective address lies in an uncached segment. */
        dcache_operation = (rt & 1) != 0;
        break;

    default:
        break;
    }

    /* LDI: one cycle, including the real chip's false dependencies. */
    if (r->pipeline_load_mask &&
        (r4300_pipeline_ldi_candidates(op, r->pipeline_load_mask, fr) &
         r->pipeline_load_mask) != 0)
        ++stalls;

    /* COP1 has no EX->EX bypass for a computational result.  The encoded
     * execution rate has already elapsed; a directly dependent arithmetic or
     * compare consumer needs one additional clock for DC->EX forwarding. */
    if (cu1 && r->pipeline_fpu_result_mask &&
        (r4300_pipeline_fpu_reads(op, d, fr) & r->pipeline_fpu_result_mask) != 0)
        ++stalls;

    /* A compare writes the FCR31 condition bit; an immediately following BC1
     * must likewise wait for the result to leave EX. */
    if (cu1 && r->pipeline_fpu_cc && major == 0x11 && rs == 8)
        ++stalls;

    {
        unsigned int cached = memory && r4300_access_cached(r,
            (uint32_t)r->regs[rs] + (uint32_t)(int32_t)(int16_t)op);
        unsigned int dcache = cached || dcache_operation;

        /* A cached store occupies the D-cache write port in WB for one extra
         * PClock.  Only an immediately following D-cache user is interlocked. */
        if (dcache && r->pipeline_cached_store) ++stalls;
        r->pipeline_cached_store = cached && store;
    }

    if (cu1 && major == 0x11 && rs >= 16) {
        if (fn >= 0x30) fpu_cc = 1;
        else fpu_result = r4300_pipeline_fpu_result(op, d, fr);
    }

    r->pipeline_load_mask = load;
    r->pipeline_fpu_result_mask = fpu_result;
    r->pipeline_fpu_cc = fpu_cc;
    return stalls;
}

#endif
