static int native_ram_opcode(enum r4300_opcode op)
{
    switch (op) {
    case R4300_OP_LB: case R4300_OP_LBU: case R4300_OP_LH: case R4300_OP_LHU:
    case R4300_OP_LW: case R4300_OP_LWU: case R4300_OP_LD:
    case R4300_OP_SB: case R4300_OP_SH: case R4300_OP_SW: case R4300_OP_SD:
    case R4300_OP_LWC1: case R4300_OP_LDC1: case R4300_OP_SWC1: case R4300_OP_SDC1: return 1;
    default: return 0;
    }
}
struct memory_exit {
    struct regcache_state registers;
    struct regcache_state resume_registers;
    unsigned char* resume;
    unsigned index, start, end, guard_count;
    unsigned char* guards[16];
};
static void memory_guard(struct memory_exit* exit, unsigned condition)
{
    assert(exit->guard_count < 16);
    exit->guards[exit->guard_count++] = emit_jump(condition);
}
static void emit_itlb_native(uint32_t pc)
{
    uint32_t vpn = pc >> 12;
    emit_mem(0x8b, 0, T0, STATE, -1, 0, offsetof(struct r4300_core, cp0.tlb.LUT_r) + vpn * 4);
    emit_imm_op(4, T0, UINT32_C(0xfffff000), 0);
    unsigned char* hits[2], *slot_misses[3];
    for (unsigned slot = 0; slot < 2; ++slot) {
        emit_mem_imm(0xf7, 0, 0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_valid),1u << slot);
        slot_misses[0] = emit_jump(4);
        emit_mem(0x39, 0, T0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_map) + slot * 4);
        slot_misses[1] = emit_jump(5);
        emit_mem_imm(0x81, 0, 7, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_vpn) + slot * 4,vpn);
        slot_misses[2] = emit_jump(5);
        emit_state_imm(offsetof(struct r4300_core, pipeline_itlb_lru), slot ^ 1, 0);
        hits[slot] = emit_b();
        for (unsigned n = 0; n < 3; ++n) patch_jump(slot_misses[n], out);
    }
    emit_mem_imm(0xf7, 0, 0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_valid),1);
    unsigned char* empty_zero = emit_jump(4);
    emit_mem_imm(0xf7, 0, 0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_valid),2);
    unsigned char* empty_one = emit_jump(4);
    emit_mem_imm(0xf7, 0, 0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_lru),1);
    unsigned char* lru_zero = emit_jump(4);
    unsigned char* fills[2];
    for (int slot = 1; slot >= 0; --slot) {
        if (slot) patch_jump(empty_one, out);
        else { patch_jump(empty_zero, out); patch_jump(lru_zero, out); }
        emit_mem(0x89, 0, T0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_map) + slot * 4);
        emit_state_imm(offsetof(struct r4300_core, pipeline_itlb_vpn) + slot * 4, vpn, 0);
        emit_mem_imm(0x81, 0, 1, STATE, -1, 0, offsetof(struct r4300_core, pipeline_itlb_valid),1u << slot);
        emit_state_imm(offsetof(struct r4300_core, pipeline_itlb_lru), slot ^ 1, 0);
        fills[slot] = emit_b();
    }
    patch_jump(fills[0], out); patch_jump(fills[1], out);
    emit_addimm(CLOCKS, 3, CLOCKS);
    patch_jump(hits[0], out); patch_jump(hits[1], out);
}
/* Fetch state changes happen only after the current instruction's guards.
 * A region stays on one code page, so its first fetch establishes the exact
 * two-entry instruction micro-TLB hit/replacement state for later fetches. */
static void emit_fetch_native(const struct ari64_config* config, unsigned index, unsigned start, unsigned base, int check_line)
{
    unsigned cycles = base + r4300_issue_reservation(config->compat_issue_clock);
    emit_addimm(CLOCKS, cycles, CLOCKS);
    if (!config->cache_timing) return;
    uint32_t pc = config->virtual_pc + index * 4;
    if ((pc & UINT32_C(0xc0000000)) != UINT32_C(0x80000000) && index == start) emit_itlb_native(pc);
    emit_mem_imm(0xf7, 0, 0, SP, -1, 0, 4,2);
    unsigned char* uncached = emit_jump(4);
    if (check_line) {
        size_t tag = offsetof(struct r4300_core, icache_tags) + ((pc >> 5) & 511) * 4;
        uint32_t key = (config->physical_page << 12) | 1;
        emit_mem_imm(0x81, 0, 7, STATE, -1, 0, tag,key);
        unsigned char* hit = emit_jump(4);
        emit_state_imm(tag, key, 0);
        emit_addimm(CLOCKS, 48, CLOCKS);
        patch_jump(hit, out);
    }
    unsigned char* done = emit_b();
    patch_jump(uncached, out);
    emit_addimm(CLOCKS, 40, CLOCKS);
    patch_jump(done, out);
}
static void emit_pipeline_clear(void)
{
    emit_state_imm(offsetof(struct r4300_core, pipeline_load_mask), 0, 1);
    emit_state_imm(offsetof(struct r4300_core, pipeline_fpu_result_mask), 0, 1);
    emit_state_imm(offsetof(struct r4300_core, pipeline_fpu_cc), 0, 0);
    emit_state_imm(offsetof(struct r4300_core, pipeline_cached_store), 0, 0);
}
/* Native timing cannot absorb LDI in a compatibility issue reservation. Test
 * raw fields just like the hardware's deliberately imprecise detector. */
static void emit_ldi_native(uint32_t instruction, int fp, uint32_t previous, int known)
{
    if (known) {
        unsigned major = previous >> 26, load = (previous >> 16) & 31;
        if ((major >= 0x20 && major <= 0x25) || major == 0x27 || major == 0x37) {
            if (!fp && load && (load == ((instruction >> 21) & 31) || load == ((instruction >> 16) & 31)))
                emit_addimm(CLOCKS, 1, CLOCKS);
        } else if (fp && (major == 0x31 || major == 0x35)) {
            unsigned consumer = (instruction >> 16) & 31;
            if (load == consumer) emit_addimm(CLOCKS, 1, CLOCKS);
            else if ((load & ~1u) == (consumer & ~1u)) {
                emit_mem_imm(0xf7, 0, 0, STATE, -1, 0, offsetof(struct r4300_core, cp0.regs[CP0_STATUS_REG]),CP0_STATUS_FR);
                unsigned char* fr = emit_jump(5);
                emit_addimm(CLOCKS, 1, CLOCKS); patch_jump(fr, out);
            }
        }
        return;
    }
    unsigned rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
    uint32_t candidates = (rs ? UINT32_C(1) << rs : 0) | (rt ? UINT32_C(1) << rt : 0);
    if (!fp && !candidates) return;
    emit_mem(0x8b, 1, T0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_load_mask));
    unsigned char* fpr_history = NULL;
    if (fp) {
        emit_movimm64(UINT64_C(1) << (32 + rt), T2);
        if (rt & 1) {
            emit_mem_imm(0xf7, 0, 0, STATE, -1, 0, offsetof(struct r4300_core, cp0.regs[CP0_STATUS_REG]),CP0_STATUS_FR);
            unsigned char* fr = emit_jump(5);
            emit_movimm64(UINT64_C(1) << (32 + (rt & ~1u)), T2); patch_jump(fr, out);
        }
        emit_reg_op(0x21, T2, T0, 1);
    } else {
        emit_mov64(T0, T2); emit_shift_reg(T2, 5, 32, 1);
        emit_reg_op(0x85, T2, T2, 0); fpr_history = emit_jump(5);
        emit_imm_op(4, T0, candidates, 0);
    }
    emit_reg_op(0x85, T0, T0, fp ? 1 : 0);
    unsigned char* no_load = emit_jump(4);
    emit_addimm(CLOCKS, 1, CLOCKS);
    patch_jump(no_load, out);
    if (fpr_history) patch_jump(fpr_history, out);
}
/* Match the authoritative TLB-entry scan, including overlapping entries,
 * global mappings, ASID and cache mode. Read it live rather than caching an
 * answer that could survive a same-PFN rewrite with different attributes. */
static void emit_mapped_cacheability(void)
{
    emit_mem(0x89, 0, T2, SP, -1, 0, 0);
    emit_mem(0x8d, 1, T0, STATE, -1, 0, offsetof(struct r4300_core, cp0.tlb.entries));
    emit_mem(0x8b, 0, T1, STATE, -1, 0, offsetof(struct r4300_core, cp0.regs[CP0_ENTRYHI_REG]));
    emit_imm_op(4, T1, 255, 0); emit_movimm(32, T2);
    unsigned char* loop = out;
    emit_mem_imm(0x80, 0, 7, T0, -1, 0, offsetof(struct tlb_entry, g),0);
    unsigned char* global = emit_jump(5);
    emit_mem(0x38, 0, T1, T0, -1, 0, offsetof(struct tlb_entry, asid));
    unsigned char* other_asid = emit_jump(5);
    patch_jump(global, out);
    emit_mem_imm(0x80, 0, 7, T0, -1, 0, offsetof(struct tlb_entry, v_even),0);
    unsigned char* invalid_even = emit_jump(4);
    emit_mem(0x3b, 0, ADDRESS, T0, -1, 0, offsetof(struct tlb_entry, start_even));
    unsigned char* before_even = emit_jump(2);
    emit_mem(0x3b, 0, ADDRESS, T0, -1, 0, offsetof(struct tlb_entry, end_even));
    unsigned char* after_even = emit_jump(7);
    emit_mem_imm(0x80, 0, 7, T0, -1, 0, offsetof(struct tlb_entry, c_even),2);
    unsigned char* even_uncached = emit_jump(4);
    unsigned char* even_cached = emit_b();
    patch_jump(invalid_even, out); patch_jump(before_even, out); patch_jump(after_even, out);
    emit_mem_imm(0x80, 0, 7, T0, -1, 0, offsetof(struct tlb_entry, v_odd),0);
    unsigned char* invalid_odd = emit_jump(4);
    emit_mem(0x3b, 0, ADDRESS, T0, -1, 0, offsetof(struct tlb_entry, start_odd));
    unsigned char* before_odd = emit_jump(2);
    emit_mem(0x3b, 0, ADDRESS, T0, -1, 0, offsetof(struct tlb_entry, end_odd));
    unsigned char* after_odd = emit_jump(7);
    emit_mem_imm(0x80, 0, 7, T0, -1, 0, offsetof(struct tlb_entry, c_odd),2);
    unsigned char* odd_uncached = emit_jump(4);
    unsigned char* odd_cached = emit_b();
    patch_jump(other_asid, out); patch_jump(invalid_odd, out); patch_jump(before_odd, out); patch_jump(after_odd, out);
    emit_addimm64(T0, sizeof(struct tlb_entry), T0); emit_addimm(T2, -1, T2);
    unsigned char* again = emit_jump(5); patch_jump(again, loop);
    patch_jump(even_uncached, out); patch_jump(odd_uncached, out);
    emit_movimm(0, T0);
    unsigned char* done = emit_b();
    patch_jump(even_cached, out); patch_jump(odd_cached, out); emit_movimm(1, T0);
    patch_jump(done, out);
    emit_mem(0x8b, 0, T2, SP, -1, 0, 0);
}
static void emit_memory_native(uint32_t instruction, unsigned index,
    const struct ari64_config* config, struct memory_exit* exit, int check_line,
    uint32_t previous, int known)
{
    enum r4300_opcode op = r4300_get_idec(instruction)->opcode;
    int fp = op == R4300_OP_LWC1 || op == R4300_OP_LDC1 || op == R4300_OP_SWC1 || op == R4300_OP_SDC1;
    unsigned rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
    unsigned size = op == R4300_OP_LD || op == R4300_OP_SD || op == R4300_OP_LDC1 || op == R4300_OP_SDC1 ? 8 :
        op == R4300_OP_LW || op == R4300_OP_LWU || op == R4300_OP_SW || fp ? 4 :
        op == R4300_OP_LH || op == R4300_OP_LHU || op == R4300_OP_SH ? 2 : 1;
    int write = op == R4300_OP_SB || op == R4300_OP_SH || op == R4300_OP_SW || op == R4300_OP_SD || op == R4300_OP_SWC1 || op == R4300_OP_SDC1;
    emit_state_load(gpr_offset(rs), ADDRESS);
    emit_addimm(ADDRESS, (int16_t)instruction, ADDRESS);
    exit->registers = regcache;
    if (fp) {
        emit_mem_imm(0xf7,0,0,STATE,-1,0,offsetof(struct r4300_core,cp0.regs[CP0_STATUS_REG]),CP0_STATUS_CU1);
        memory_guard(exit,4);
    }
    if (size > 1) {
        emit_mov(ADDRESS, T0); emit_imm_op(4, T0, size - 1, 0);
        emit_reg_op(0x85, T0, T0, 0); memory_guard(exit, 5);
    }
    emit_mov(ADDRESS, T2);
    emit_mov(ADDRESS, T0); emit_imm_op(4, T0, UINT32_C(0xc0000000), 0);
    emit_imm_op(7, T0, UINT32_C(0x80000000), 0);
    unsigned char* direct = emit_jump(4);
    emit_mov(ADDRESS, T0); emit_shift_reg(T0, 5, 12, 0);
    emit_mem(0x8b, 0, T2, STATE, T0, 2, write ?
        offsetof(struct r4300_core, cp0.tlb.LUT_w) : offsetof(struct r4300_core, cp0.tlb.LUT_r));
    emit_reg_op(0x85, T2, T2, 0); memory_guard(exit, 4);
    emit_imm_op(4, T2, UINT32_C(0xfffff000), 0);
    emit_mov(ADDRESS, T0); emit_imm_op(4, T0, 4095, 0);
    emit_reg_op(0x09, T0, T2, 0);
    emit_reg_op(0x85, T2, T2, 0); memory_guard(exit, 4);
    patch_jump(direct, out);
    emit_imm_op(4, T2, UINT32_C(0x1ffffffc), 0);
    if (write) {
        emit_mov(T2, T1); emit_shift_reg(T1, 5, 12, 0);
        emit_mem(0x8b, 1, T0, STATE, -1, 0, offsetof(struct r4300_core, cached_code_pages));
        /* BT treats the memory operand as a bit string, indexing the complete
         * physical-page bitmap. A tracked page must use shared invalidation. */
        emit_bit_test(T0,T1);
        memory_guard(exit, 2);
    }
    emit_mem(0x8b, 1, TARGET, STATE, -1, 0, offsetof(struct r4300_core, mem));
    emit_mov(T2, T0); emit_shift_reg(T0, 5, 16, 0);
    emit_mem(0x8d, 1, T0, T0, T0, 1, 0); /* three pointers per handler */
    emit_mem(0x8d, 1, TARGET, TARGET, T0, 3, offsetof(struct memory, handlers));
    emit_movimm64((intptr_t)(write ? (void*)write_rdram_dram : (void*)read_rdram_dram), T1);
    emit_mem(0x39, 1, T1, TARGET, -1, 0, write ? offsetof(struct mem_handler, write32) : offsetof(struct mem_handler, read32));
    memory_guard(exit, 5);
    emit_mem(0x8b, 1, TARGET, TARGET, -1, 0, offsetof(struct mem_handler, opaque));
    emit_mov(T2, T0); emit_imm_op(4, T0, UINT32_C(0x00fffffc), 0);
    emit_addimm(T0, size == 8 ? 8 : 4, T0);
    emit_mem(0x3b, 1, T0, TARGET, -1, 0, offsetof(struct rdram, dram_size));
    memory_guard(exit, 7); /* unsigned greater: dispatch retains bounds behavior */
    emit_mem(0x8b, 1, TARGET, TARGET, -1, 0, offsetof(struct rdram, dram));
    emit_mov(T2, T0); emit_imm_op(4, T0, UINT32_C(0x00fffffc), 0);
    emit_reg_op(0x01, T0, TARGET, 1);
    if (config->cache_timing) {
        emit_mov(ADDRESS, T0); emit_imm_op(4, T0, UINT32_C(0xe0000000), 0);
        emit_imm_op(7, T0, UINT32_C(0xa0000000), 0);
        unsigned char* uncached = emit_jump(4);
        emit_imm_op(7, T0, UINT32_C(0x80000000), 0);
        unsigned char* mapped = emit_jump(5);
        emit_mem(0x8b, 0, T0, STATE, -1, 0, offsetof(struct r4300_core, cp0.regs[CP0_CONFIG_REG]));
        emit_imm_op(4, T0, 7, 0); emit_imm_op(7, T0, 2, 0);
        unsigned char* config_uncached = emit_jump(4);
        unsigned char* direct_cached = emit_b();
        patch_jump(mapped, out);
        emit_mapped_cacheability(); emit_reg_op(0x85, T0, T0, 0);
        unsigned char* mapped_uncached = emit_jump(4);
        patch_jump(direct_cached, out);
        emit_mov(ADDRESS, T1); emit_shift_reg(T1, 5, 4, 0); emit_imm_op(4, T1, 511, 0);
        emit_mem(0x8b, 0, T0, STATE, T1, 2, offsetof(struct r4300_core, dcache_tags));
        emit_imm_op(4, T0, ~UINT32_C(2), 0);
        /* Cached accesses no longer need PA after forming the tag key.
         * Reuse that scratch register in tags-only mode, preserving the RAM
         * pointer without a stack save/restore on every load and store.
         * Uncached branches above still retain the original PA. */
        int key_reg = config->cache_timing == 2 ? T2 : TARGET;
        if (key_reg != T2) emit_mov(T2, key_reg);
        emit_imm_op(4, key_reg, UINT32_C(0x1ffff000), 0); emit_imm_op(1, key_reg, 1, 0);
        emit_reg_op(0x39, key_reg, T0, 0);
        memory_guard(exit, 5);
        if (write) {
            emit_mem_imm(0x81, 0, 1, STATE, T1, 2, offsetof(struct r4300_core, dcache_tags),2);
        }
        if (config->cache_timing != 2) {
            emit_mov(ADDRESS, TARGET); emit_imm_op(4, TARGET, UINT32_C(0x1ffc), 0);
            emit_mem(0x8d, 1, TARGET, STATE, TARGET, 0, offsetof(struct r4300_core, dcache_words));
        }
        emit_movimm(1, T1); emit_movimm(1, T2);
        unsigned char* data_ready = emit_b();
        patch_jump(uncached, out); patch_jump(config_uncached, out); patch_jump(mapped_uncached, out);
        emit_movimm(0, T1);
        if (!write) {
            emit_imm_op(7, T2, UINT32_C(0x00800000), 0);
            unsigned char* larger = emit_jump(3);
            emit_movimm(31, T2);
            unsigned char* bus_ready = emit_b();
            patch_jump(larger, out); emit_movimm(38, T2); patch_jump(bus_ready, out);
        } else emit_movimm(38, T2);
        patch_jump(data_ready, out);
        emit_fetch_native(config, index, exit->start, 1, check_line);
        emit_reg_op(0x01, T2, CLOCKS, 0);
        if (!r4300_issue_reservation(config->compat_issue_clock)) {
            emit_ldi_native(instruction, fp, previous, known);
            emit_reg_op(0x85, T1, T1, 0); unsigned char* no_cache = emit_jump(4);
            emit_mem_imm(0x81, 0, 7, STATE, -1, 0, offsetof(struct r4300_core, pipeline_cached_store),0);
            unsigned char* no_store = emit_jump(4);
            emit_addimm(CLOCKS, 1, CLOCKS);
            patch_jump(no_store, out); patch_jump(no_cache, out);
        } else {
        /* LDI and DCB can coexist in restored/test history at region entry.
         * Thereafter every instruction writes mutually exclusive load/store
         * history, so the reserved issue clock absorbs either single stall. */
        if (index == exit->start) {
        emit_reg_op(0x85, T1, T1, 0); unsigned char* no_cache = emit_jump(4);
        emit_mem_imm(0x81, 0, 7, STATE, -1, 0, offsetof(struct r4300_core, pipeline_cached_store),0);
        unsigned char* no_store = emit_jump(4);
        unsigned char* fpr_history = NULL;
        if (fp) {
            emit_movimm64(UINT64_C(1) << (32 + rt),T2);
            if (rt & 1) {
                emit_mem_imm(0xf7,0,0,STATE,-1,0,offsetof(struct r4300_core,cp0.regs[CP0_STATUS_REG]),CP0_STATUS_FR);
                unsigned char* fr = emit_jump(5); emit_movimm64(UINT64_C(1) << (32 + (rt & ~1u)),T2); patch_jump(fr,out);
            }
            emit_mem(0x8b,1,T0,STATE,-1,0,offsetof(struct r4300_core,pipeline_load_mask));
            emit_reg_op(0x21,T2,T0,1); emit_reg_op(0x85,T0,T0,1);
        } else {
            uint32_t candidates = (rs ? UINT32_C(1) << rs : 0) | (rt ? UINT32_C(1) << rt : 0);
            /* Shared LDI detection gives FPR history priority, including
             * restored masks containing both integer and FP bits. */
            emit_mem(0x8b, 1, T0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_load_mask));
            emit_mov64(T0, T2); emit_shift_reg(T2, 5, 32, 1);
            emit_reg_op(0x85, T2, T2, 0); fpr_history = emit_jump(5);
            emit_imm_op(4, T0, candidates, 0); emit_reg_op(0x85, T0, T0, 0);
        }
        unsigned char* no_load = emit_jump(4);
        emit_addimm(CLOCKS, 1, CLOCKS);
        patch_jump(no_load, out); patch_jump(no_store, out); patch_jump(no_cache, out);
        if (fpr_history) patch_jump(fpr_history, out);
        }
        }
        if (!write) emit_movimm(0, T1);
        emit_mem(0x89, 0, T1, STATE, -1, 0, offsetof(struct r4300_core, pipeline_cached_store));
    } else {
        emit_fetch_native(config, index, exit->start, 1, check_line);
        emit_state_imm(offsetof(struct r4300_core, pipeline_cached_store), 0, 0);
    }
    emit_movimm64(config->cache_timing && !write && (rt || fp) ? UINT64_C(1) << (rt + (fp ? 32 : 0)) : 0, T0);
    if (fp && config->cache_timing && !write && (rt & 1)) {
        emit_mem_imm(0xf7,0,0,STATE,-1,0,offsetof(struct r4300_core,cp0.regs[CP0_STATUS_REG]),CP0_STATUS_FR);
        unsigned char* fr = emit_jump(5); emit_movimm64(UINT64_C(1) << (32 + (rt & ~1u)),T0); patch_jump(fr,out);
    }
    emit_mem(0x89, 1, T0, STATE, -1, 0, offsetof(struct r4300_core, pipeline_load_mask));
    if (index == exit->start) {
        emit_state_imm(offsetof(struct r4300_core, pipeline_fpu_result_mask), 0, 1);
        emit_state_imm(offsetof(struct r4300_core, pipeline_fpu_cc), 0, 0);
    }
    if (size < 4) {
        emit_mov(ADDRESS, T2); emit_imm_op(4, T2, size == 1 ? 3 : 2, 0); emit_imm_op(6, T2, size == 1 ? 3 : 2, 0);
    }
    if (write) {
        if (fp) {
            emit_state_load_raw((size == 8 ? offsetof(struct r4300_core,cp1.regs_double) : offsetof(struct r4300_core,cp1.regs_simple)) + rt * sizeof(void*),T1);
            emit_mem(0x8b,size == 8,T0,T1,-1,0,0);
        } else emit_state_load(gpr_offset(rt), T0);
        if (size == 8) {
            emit_mov64(T0, T1); emit_shift_reg(T1, 5, 32, 1);
            emit_mem(0x89, 0, T1, TARGET, -1, 0, 0);
            emit_mem(0x89, 0, T0, TARGET, -1, 0, 4);
        } else {
            if (size == 2) { emit_address(TARGET,T2,0,0); emit_store_at(T0,0,2); } else
            emit_mem(size == 1 ? 0x88 : 0x89, 0, T0, TARGET, size < 4 ? T2 : -1, 0, 0);
        }
    } else {
        if (size < 4) {
            int sign = op == R4300_OP_LB || op == R4300_OP_LH;
            emit_address(TARGET,T2,0,0); emit_load_at(T0,sign,size,sign);
        } else if (size == 4) emit_mem(op == R4300_OP_LW ? 0x63 : 0x8b, op == R4300_OP_LW, T0, TARGET, -1, 0, 0);
        else {
            emit_mem(0x8b, 0, T0, TARGET, -1, 0, 0); emit_shift_reg(T0, 4, 32, 1);
            emit_mem(0x8b, 0, T1, TARGET, -1, 0, 4); emit_reg_op(0x09, T1, T0, 1);
        }
        if (fp) {
            emit_state_load_raw((size == 8 ? offsetof(struct r4300_core,cp1.regs_double) : offsetof(struct r4300_core,cp1.regs_simple)) + rt * sizeof(void*),T1);
            emit_mem(0x89,size == 8,T0,T1,-1,0,0);
        } else if (rt) emit_state_store(gpr_offset(rt), T0);
    }
}

