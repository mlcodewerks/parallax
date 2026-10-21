/* Native regions, guarded side exits and publication. GPL-2.0-or-later. */
static void emit_step_call(void* block,unsigned index,unsigned phase,ari64_step step)
{
    emit_mov64(STATE,ARG1_REG); emit_movimm64((intptr_t)block,ARG2_REG);
    emit_movimm(index,ARG3_REG); emit_movimm(phase,ARG4_REG); emit_helper_call((const void*)step);
}
int ari64_has_native(const uint32_t* ops, unsigned count, uint32_t delay_op, unsigned has_delay, unsigned idle)
{
    for (unsigned i = 0; i < count; ++i) {
        enum r4300_opcode op = r4300_get_idec(ops[i])->opcode;
        if (native_opcode(op) || native_ram_opcode(op) || native_fpu_transfer(op) || native_fpu_instruction(ops[i])) return 1;
    }
    return count && count < MAX_INSTRUCTIONS && has_delay && !idle &&
        native_branch_opcode(r4300_get_idec(ops[count - 1])->opcode) &&
        native_opcode(r4300_get_idec(delay_op)->opcode);
}
static unsigned char* emit_step(void* block, unsigned index, unsigned phase, ari64_step step)
{
    emit_step_call(block, index, phase, step);
    emit_reg_op(0x85,T0,T0,0);
    return emit_jump(4);
}

/* The prepare helper already charged a COP1 transfer's clocks and hazards.
 * The register-only transfer cannot invalidate code or change mappings.
 * Commit locally while no event is due; the original helper still handles
 * exact event boundaries and all preparation faults. */
static void emit_transfer_commit(void* block, unsigned index,
    const struct ari64_config* config, ari64_step step,
    unsigned char** exits, unsigned* exit_count)
{
    emit_mem_imm(0x83, 1, 7, STATE, -1, 0, offsetof(struct r4300_core, cp0.cycle_count),0);
    unsigned char* event = emit_jump(13); /* signed >= 0 */
    emit_state_imm(offsetof(struct r4300_core, interp_PC.addr), config->virtual_pc + (index + 1) * 4, 0);
    emit_state_imm(gpr_offset(0), 0, 1);
    if (config->commits) {
        emit_movimm64((intptr_t)config->commits, ADDRESS);
        emit_mem_imm(0x83, 1, 0, ADDRESS, -1, 0, 0,1);
    }
    emit_movimm64((intptr_t)&breakloop, ADDRESS);
    emit_mem_imm(0x80, 0, 7, ADDRESS, -1, 0, 0,0);
    exits[(*exit_count)++] = emit_jump(5);
    unsigned char* done = emit_b();
    patch_jump(event, out);
    exits[(*exit_count)++] = emit_step(block, index, 1, step);
    patch_jump(done, out);
}

static ari64_entry compile_block(struct ari64* s, void* block,
    const uint32_t* ops, unsigned count, ari64_step step, int optimized, const struct ari64_config* config)
{
    ari64_link link = config ? config->link : NULL;
    if (!s || !ops || !step || !count || count > MAX_INSTRUCTIONS) return NULL;
    optimized = optimized && config;
    uint32_t extended_ops[MAX_INSTRUCTIONS];
    unsigned branch_index = MAX_INSTRUCTIONS;
    if (optimized && config && config->has_delay && !config->idle_end && count < MAX_INSTRUCTIONS &&
        native_branch_opcode(r4300_get_idec(ops[count - 1])->opcode) && native_opcode(r4300_get_idec(config->delay_op)->opcode)) {
        memcpy(extended_ops, ops, count * sizeof(*ops));
        branch_index = count - 1; extended_ops[count++] = config->delay_op; ops = extended_ops;
    }
    /* Reserve for the expanded A64 address/immediate sequences and per-memory
     * side exits. Compile only with all generated frames unwound. */
    size_t reserve = 4096;
    for (unsigned i=0;i<count;++i)
        reserve += optimized && native_ram_opcode(r4300_get_idec(ops[i])->opcode) ? 8192 :
                   optimized && native_fpu_instruction(ops[i]) ? 4096 : 1024;
    reserve = (reserve+15) & ~(size_t)15;
    if (reserve > ARI64_ARM64_SEGMENT_BYTES) return NULL;
    if ((s->used % ARI64_ARM64_SEGMENT_BYTES) + reserve > ARI64_ARM64_SEGMENT_BYTES) {
        s->used = ((s->used / ARI64_ARM64_SEGMENT_BYTES + 1) * ARI64_ARM64_SEGMENT_BYTES) % ARENA_BYTES;
        if (!code_protect(s->code + s->used, ARI64_ARM64_SEGMENT_BYTES, 0)) return NULL;
        s->stamps[s->used / ARI64_ARM64_SEGMENT_BYTES] = ++s->generation;
    }
    unsigned char* start = s->code + s->used;
    size_t page_start = s->used & ~(s->page - 1);
    size_t page_end = (s->used + reserve + s->page - 1) & ~(s->page - 1);
    /* Pack aligned entries into pages. Publication may share the preceding
     * block's page; compilation only runs with every generated frame unwound,
     * so temporarily making that page writable cannot interrupt native code. */
    if (!code_protect(s->code + page_start, page_end - page_start, 0)) return NULL;
    unsigned char* exits[MAX_INSTRUCTIONS * 9 + 16];
    struct memory_exit memory_exits[MAX_INSTRUCTIONS];
    unsigned memory_exit_count = 0;
    unsigned exit_count = 0;
    out = start;
    compile_absolute_calls = 0;
    emit_frame_enter();
    emit_mov64(ARG1_REG,STATE);
    unsigned char* chain_entry=out;
    a64(0xd50324df); /* BTI jc: both IP0/IP1 and general-register tail links */
    regcache.active=0;
    for (unsigned i = 0; i < count;) {
        enum r4300_opcode op = r4300_get_idec(ops[i])->opcode;
        if (native_opcode(op) || i == branch_index || (optimized && config && native_ram_opcode(op))) {
            unsigned end = i + 1;
            if (optimized) {
                int memory_region = native_ram_opcode(op);
                while (end < count) {
                    enum r4300_opcode next = r4300_get_idec(ops[end])->opcode;
                    if (!native_opcode(next) && end != branch_index && !(config && native_ram_opcode(next))) break;
                    memory_region |= native_ram_opcode(next); ++end;
                }
                unsigned base = 0, memory_count = 0;
                for (unsigned j = i; j < end; ++j) {
                    base += r4300_base_cycles(ops[j], 0);
                    memory_count += native_ram_opcode(r4300_get_idec(ops[j])->opcode) != 0;
                }
                int branch_region = branch_index >= i && branch_index < end;
                unsigned branch_major = branch_region ? ops[branch_index] >> 26 : 0;
                int likely_region = branch_region && ((branch_major >= 20 && branch_major <= 23) ||
                    (branch_major == 1 && ((ops[branch_index] >> 16) & 2)));
                unsigned char* annul = NULL;
                struct regcache_state annul_registers;
                struct memory_exit prepare = {0};
                emit_region_guard(config, i, end - i, base, memory_count, &prepare);
                regcache_begin(ops, i, end);
                emit_mem_imm(0xc7, 0, 0, SP, -1, 0, 8,0);
                if (memory_region || likely_region) regcache.allow_dead_store = 0;
                emit_movimm(0, CLOCKS);
                int pipeline_live = 1;
                for (unsigned j = i; j < end; ++j) {
                    regcache.index = j;
                    /* Native RAM does not alter I-cache tags. A precise RAM
                     * continuation has already fetched the current line;
                     * only the next line needs another tag check. */
                    int check_line = j == i || ((config->virtual_pc + j * 4) >> 5) !=
                        ((config->virtual_pc + (j - 1) * 4) >> 5);
                    enum r4300_opcode instruction_op = r4300_get_idec(ops[j])->opcode;
                    if (native_ram_opcode(instruction_op)) {
                        struct memory_exit* exit = &memory_exits[memory_exit_count++];
                        exit->start = i; exit->index = j; exit->end = end; exit->guard_count = 0;
                        emit_memory_native(ops[j], j, config, exit, check_line,
                            j == i ? 0 : ops[j - 1], j != i);
                        pipeline_live = 1;
                        exit->resume = out; exit->resume_registers = regcache;
                    } else {
                        emit_fetch_native(config, j, i, r4300_base_cycles(ops[j], 0), check_line);
                        unsigned reservation = r4300_issue_reservation(config->compat_issue_clock);
                        if (config->cache_timing && !reservation && pipeline_live) {
                            emit_ldi_native(ops[j], 0, j == i ? 0 : ops[j - 1], j != i);
                            emit_pipeline_clear();
                            pipeline_live = 0;
                        }
                        if (j == branch_index) {
                            annul = emit_branch_native(ops[j], config->virtual_pc + j * 4);
                            annul_registers = regcache;
                        }
                        else if (!regcache_fold(ops[j]) && !regcache_direct(ops[j])) emit_native(ops[j], instruction_op);
                        if ((!config->cache_timing || reservation) &&
                            (j + 1 == end || native_ram_opcode(r4300_get_idec(ops[j + 1])->opcode))) emit_pipeline_clear();
                    }
                }
                regcache_flush();
                emit_region_commit_native(config, i, end - i, branch_region);
                if (annul) {
                    unsigned char* taken = emit_b();
                    patch_jump(annul, out);
                    regcache = annul_registers; regcache_flush();
                    emit_pipeline_clear();
                    emit_region_commit_native(config, i, end - i - 1, 1);
                    patch_jump(taken, out);
                }
                /* Unconditional jump over the precise event/mode slow path. */
                unsigned char* done = emit_b();
                for (unsigned g = 0; g < prepare.guard_count; ++g) patch_jump(prepare.guards[g], out);
                exits[exit_count++] = emit_step(block, i, ((end - i) << 8) | 5, step);
                patch_jump(done, out);
            } else {
                exits[exit_count++] = emit_step(block, i, 0, step);
                emit_native(ops[i], op);
                exits[exit_count++] = emit_step(block, i, 1, step);
            }
            s->native += end - i;
            i = end;
        } else if (optimized && native_fpu_transfer(op)) {
            exits[exit_count++] = emit_step(block,i,11,step);
            emit_fpu_transfer(ops[i],op);
            emit_transfer_commit(block,i,config,step,exits,&exit_count);
            ++s->native; ++i;
        } else if (optimized && config->fpu_prepare && config->fpu_finish && native_fpu_instruction(ops[i])) {
            emit_fpu_native(ops[i], i, config, exits, &exit_count); ++s->native; ++i;
        } else { exits[exit_count++] = emit_step(block, i, 2, step); ++i; }
    }
    if (memory_exit_count) {
        exits[exit_count++] = emit_b();
        for (unsigned n = 0; n < memory_exit_count; ++n) {
            struct memory_exit* exit = &memory_exits[n];
            for (unsigned g = 0; g < exit->guard_count; ++g) patch_jump(exit->guards[g], out);
            regcache = exit->registers;
            regcache_flush();
            emit_region_commit_native(config, exit->start, exit->index - exit->start, 0);
            exits[exit_count++] = emit_step(block, exit->index, 2, step);
            unsigned base = 0, memory_count = 0;
            for (unsigned j = exit->index + 1; j < exit->end; ++j) {
                base += r4300_base_cycles(ops[j], 0);
                memory_count += native_ram_opcode(r4300_get_idec(ops[j])->opcode) != 0;
            }
            struct memory_exit prepare = {0};
            emit_region_guard(config, exit->index + 1, exit->end - exit->index - 1, base, memory_count, &prepare);
            for (unsigned g = 0; g < prepare.guard_count; ++g) exits[exit_count++] = prepare.guards[g];
            emit_mem_imm(0xc7, 0, 0, SP, -1, 0, 8,exit->index + 1 - exit->start);
            emit_movimm(0, CLOCKS);
            /* Helpers may clobber volatile host registers and the loaded
             * guest destination. Restore the post-instruction allocation. */
            for (unsigned h = 0; h < CACHE_REGS; ++h)
                if (exit->resume_registers.map[h] >= 0)
                    emit_state_load_raw(guest_offset(exit->resume_registers.map[h]), cache_host[h]);
            unsigned char* resume = emit_b();
            patch_jump(resume, exit->resume);
        }
    }
    unsigned char* epilogue = out;
    if (link) {
        if (config->links) emit_direct_links(block,config);
        emit_mov64(STATE, ARG1_REG);
        emit_helper_call((const void*)link);
        emit_reg_op(0x85,T0,T0,1);
        unsigned char* miss=emit_jump(4); a64(0xd61f0000 | (T0<<5)); patch_jump(miss,out);
    }
    emit_frame_leave(); a64(0xd65f03c0);
    for (unsigned i = 0; i < exit_count; ++i) {
        patch_jump(exits[i], epilogue);
    }
    assert((size_t)(out - start) <= reserve);
    s->used = ((size_t)(out - s->code) + 15) & ~(size_t)15;
    page_end = (s->used + s->page - 1) & ~(s->page - 1);
    if (!code_protect(s->code + page_start, page_end - page_start, 1)) return NULL;
    __builtin___clear_cache((char*)start,(char*)out);
    ++s->blocks;
    s->bytes += out - start;
    s->calls += compile_absolute_calls;
    if (config && config->chain_entry) *config->chain_entry = (ari64_entry)(void*)chain_entry;
    return (ari64_entry)(void*)start;
}

ari64_entry ari64_compile(struct ari64* s, void* block,
    const uint32_t* ops, unsigned count, ari64_step step)
{
    return compile_block(s, block, ops, count, step, 0, NULL);
}
ari64_entry ari64_compile_optimized(struct ari64* s, void* block,
    const uint32_t* ops, unsigned count, ari64_step step, const struct ari64_config* config)
{
    return compile_block(s, block, ops, count, step, 1, config);
}
