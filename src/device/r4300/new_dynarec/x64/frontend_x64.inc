/* Core-specific x64 frontend for the Ari64 emitter. GPL-2.0-or-later. */
#ifndef ARI64_X64_ARENA_BYTES
#define ARI64_X64_ARENA_BYTES (8 << TARGET_SIZE_2)
#endif
enum { CODE_BYTES = ARI64_X64_ARENA_BYTES, MAX_INSTRUCTIONS = 32 };
#ifndef ARI64_X64_SEGMENTS
/* Keep the default 8 MiB recycling granularity as the arena grows. */
#define ARI64_X64_SEGMENTS (CODE_BYTES >= (256 << 20) ? 32 : CODE_BYTES >= (1 << 20) ? 16 : 1)
#endif
enum { CODE_SEGMENTS = ARI64_X64_SEGMENTS,
    SEGMENT_BYTES = CODE_BYTES / CODE_SEGMENTS };
struct ari64_x64 {
    unsigned char* code;
    size_t used, page_size;
    uint64_t generation, blocks, native;
    uint64_t emitted_bytes, relative_calls, absolute_calls;
    uint64_t segment_generation[CODE_SEGMENTS];
};

static int code_protect(void* ptr, size_t size, int executable)
{
#ifdef _WIN32
    DWORD previous;
    return VirtualProtect(ptr, size, executable ? PAGE_EXECUTE_READ : PAGE_READWRITE, &previous) != 0;
#else
    return mprotect(ptr, size, executable ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE) == 0;
#endif
}

struct ari64_x64* ari64_x64_create(void)
{
    struct ari64_x64* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    s->page_size = info.dwPageSize;
    /* Upstream linkage uses rel32 calls. Keep the arena within range of this
     * module, so precise helpers can use the same five-byte call encoding. */
    uintptr_t anchor = (uintptr_t)&ari64_x64_create;
    uintptr_t cursor = anchor > UINT32_C(0x60000000) ? anchor - UINT32_C(0x60000000) : (uintptr_t)info.lpMinimumApplicationAddress;
    uintptr_t limit = anchor + UINT32_C(0x60000000);
    MEMORY_BASIC_INFORMATION region;
    while (cursor < limit && VirtualQuery((void*)cursor, &region, sizeof(region))) {
        uintptr_t end = (uintptr_t)region.BaseAddress + region.RegionSize;
        uintptr_t aligned = (cursor + info.dwAllocationGranularity - 1) & ~((uintptr_t)info.dwAllocationGranularity - 1);
        if (region.State == MEM_FREE && aligned < end && end - aligned >= CODE_BYTES && limit - cursor >= CODE_BYTES) {
            s->code = VirtualAlloc((void*)aligned, CODE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (s->code) break;
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!s->code) s->code = VirtualAlloc(NULL, CODE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    s->page_size = (size_t)sysconf(_SC_PAGESIZE);
    s->code = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s->code == MAP_FAILED) s->code = NULL;
#endif
    if (!s->code) { free(s); return NULL; }
    s->generation = 1;
    s->segment_generation[0] = 1;
    return s;
}

void ari64_x64_destroy(struct ari64_x64* s)
{
    if (!s) return;
#ifdef _WIN32
    VirtualFree(s->code, 0, MEM_RELEASE);
#else
    munmap(s->code, CODE_BYTES);
#endif
    free(s);
}

uint64_t ari64_x64_generation(const struct ari64_x64* s) { return s->generation; }
int ari64_x64_entry_valid(const struct ari64_x64* s, ari64_x64_entry entry, uint64_t generation)
{
    uintptr_t offset = (uintptr_t)entry - (uintptr_t)s->code;
    return entry && offset < CODE_BYTES && s->segment_generation[offset / SEGMENT_BYTES] == generation;
}
const uint64_t* ari64_x64_entry_generation_ptr(const struct ari64_x64* s, ari64_x64_entry entry)
{
    uintptr_t offset = (uintptr_t)entry - (uintptr_t)s->code;
    return entry && offset < CODE_BYTES ? &s->segment_generation[offset / SEGMENT_BYTES] : NULL;
}
void ari64_x64_stats(const struct ari64_x64* s, uint64_t* blocks, uint64_t* native)
{
    *blocks = s ? s->blocks : 0;
    *native = s ? s->native : 0;
}
void ari64_x64_code_stats(const struct ari64_x64* s, uint64_t stats[3])
{
    stats[0] = s ? s->emitted_bytes : 0;
    stats[1] = s ? s->relative_calls : 0;
    stats[2] = s ? s->absolute_calls : 0;
}

static void emit_mem(unsigned opcode, int wide, int reg, int base, int index, unsigned scale, size_t offset);

/* Every guest value lives in the existing r4300_core across helper calls.
 * R12 holds that base; RAX/RCX are disposable instruction temporaries. This
 * avoids assuming upstream's assembly hot-state offsets or spill layout. */
static void emit_state_load_raw(size_t offset, int reg)
{
    emit_mem(0x8b, 1, reg, R12, -1, 0, offset);
}
static void emit_state_store_raw(size_t offset, int reg)
{
    emit_mem(0x89, 1, reg, R12, -1, 0, offset);
}
static size_t gpr_offset(unsigned reg) { return offsetof(struct r4300_core, regs) + reg * 8; }
#include "regalloc_x64.inc"
static void emit_binary(unsigned opcode, int wide)
{
    if (wide) output_rex(1, 0, 0, 0);
    output_byte(opcode);
    output_modrm(3, RAX, RCX);
}
static void emit_immediate(unsigned ext, uint32_t imm, int wide)
{
    if (wide) output_rex(1, 0, 0, 0);
    output_byte(0x81);
    output_modrm(3, RAX, ext);
    output_w32(imm);
}
static void emit_shift(unsigned ext, unsigned amount, int variable, int wide)
{
    if (variable) {
        /* Word SRAV uses the full 64-bit input in this core, but masks the
         * shift amount to five bits before truncating/sign-extending. */
        if (wide == 2) {
            output_byte(0x83); output_modrm(3, RCX, 4); output_byte(31);
        }
        if (wide) output_rex(1, 0, 0, 0);
        output_byte(0xd3); output_modrm(3, RAX, ext);
    } else if (amount) {
        if (wide) {
            if (ext == 4) emit_shlimm64(RAX, amount, RAX);
            else if (ext == 5) emit_shrimm64(RAX, amount, RAX);
            else {
                output_rex(1, 0, 0, 0);
                output_byte(0xc1); output_modrm(3, RAX, 7); output_byte(amount);
            }
        } else {
            if (ext == 4) emit_shlimm(RAX, amount, RAX);
            else if (ext == 5) emit_shrimm(RAX, amount, RAX);
            else emit_sarimm(RAX, amount, RAX);
        }
    }
}
static void emit_sign_extend_word(void)
{
    output_rex(1, 0, 0, 0); output_byte(0x63); output_modrm(3, RAX, RAX);
}

static int native_opcode(enum r4300_opcode op)
{
    switch (op) {
    case R4300_OP_NOP: case R4300_OP_SYNC:
    case R4300_OP_ADDIU: case R4300_OP_DADDIU: case R4300_OP_ADDU: case R4300_OP_DADDU:
    case R4300_OP_SUBU: case R4300_OP_DSUBU: case R4300_OP_LUI:
    case R4300_OP_AND: case R4300_OP_ANDI: case R4300_OP_OR: case R4300_OP_ORI:
    case R4300_OP_XOR: case R4300_OP_XORI: case R4300_OP_NOR:
    case R4300_OP_SLT: case R4300_OP_SLTU: case R4300_OP_SLTI: case R4300_OP_SLTIU:
    case R4300_OP_SLL: case R4300_OP_SLLV: case R4300_OP_SRL: case R4300_OP_SRLV:
    case R4300_OP_SRA: case R4300_OP_SRAV:
    case R4300_OP_DSLL: case R4300_OP_DSLL32: case R4300_OP_DSLLV:
    case R4300_OP_DSRL: case R4300_OP_DSRL32: case R4300_OP_DSRLV:
    case R4300_OP_DSRA: case R4300_OP_DSRA32: case R4300_OP_DSRAV:
    case R4300_OP_MFHI: case R4300_OP_MFLO: case R4300_OP_MTHI: case R4300_OP_MTLO:
    case R4300_OP_MULT: case R4300_OP_MULTU: case R4300_OP_DMULT: case R4300_OP_DMULTU:
    case R4300_OP_DIV: case R4300_OP_DIVU: case R4300_OP_DDIV: case R4300_OP_DDIVU:
        return 1;
    default: return 0;
    }
}

static void emit_muldiv(uint32_t instruction, enum r4300_opcode op);
static void emit_native(uint32_t instruction, enum r4300_opcode op)
{
    unsigned rs = (instruction >> 21) & 31, rt = (instruction >> 16) & 31;
    unsigned rd = (instruction >> 11) & 31, sa = (instruction >> 6) & 31;
    unsigned destination = rd;
    int word = 0;
    if (op == R4300_OP_NOP || op == R4300_OP_SYNC) return;
    if ((instruction >> 26) == 0 && (instruction & 63) >= 24 && (instruction & 63) <= 31) {
        emit_muldiv(instruction, op); return;
    }
    if (op == R4300_OP_MTHI || op == R4300_OP_MTLO) {
        emit_state_load(gpr_offset(rs), RAX);
        emit_state_store(op == R4300_OP_MTHI ? offsetof(struct r4300_core, hi) : offsetof(struct r4300_core, lo), RAX);
        return;
    }
    switch (op) {
    case R4300_OP_ADDIU: case R4300_OP_DADDIU:
        destination = rt;
        emit_state_load(gpr_offset(rs), RAX);
        if (op == R4300_OP_ADDIU) { emit_addimm(RAX, (int16_t)instruction, RAX); word = 1; }
        else emit_addimm64(RAX, (int16_t)instruction, RAX);
        break;
    case R4300_OP_ANDI: case R4300_OP_ORI: case R4300_OP_XORI:
        destination = rt;
        emit_state_load(gpr_offset(rs), RAX);
        emit_immediate(op == R4300_OP_ANDI ? 4 : op == R4300_OP_ORI ? 1 : 6,
                       (uint16_t)instruction, op != R4300_OP_ANDI);
        break;
    case R4300_OP_LUI:
        destination = rt;
        emit_movimm((uint32_t)(uint16_t)instruction << 16, RAX); word = 1;
        break;
    case R4300_OP_SLTI: case R4300_OP_SLTIU:
        destination = rt;
        emit_state_load(gpr_offset(rs), RAX);
        emit_movimm64((int64_t)(int16_t)instruction, RCX);
        emit_binary(0x39, 1);
        output_byte(0x0f); output_byte(op == R4300_OP_SLTI ? 0x9c : 0x92); output_modrm(3, RAX, 0);
        output_byte(0x0f); output_byte(0xb6); output_modrm(3, RAX, RAX);
        break;
    case R4300_OP_MFHI: case R4300_OP_MFLO:
        emit_state_load(op == R4300_OP_MFHI ? offsetof(struct r4300_core, hi) : offsetof(struct r4300_core, lo), RAX);
        break;
    case R4300_OP_SLL: case R4300_OP_SLLV: case R4300_OP_SRL: case R4300_OP_SRLV:
    case R4300_OP_SRA: case R4300_OP_SRAV:
    case R4300_OP_DSLL: case R4300_OP_DSLL32: case R4300_OP_DSLLV:
    case R4300_OP_DSRL: case R4300_OP_DSRL32: case R4300_OP_DSRLV:
    case R4300_OP_DSRA: case R4300_OP_DSRA32: case R4300_OP_DSRAV: {
        int variable = op == R4300_OP_SLLV || op == R4300_OP_SRLV || op == R4300_OP_SRAV ||
                       op == R4300_OP_DSLLV || op == R4300_OP_DSRLV || op == R4300_OP_DSRAV;
        word = op == R4300_OP_SLL || op == R4300_OP_SLLV || op == R4300_OP_SRL ||
               op == R4300_OP_SRLV || op == R4300_OP_SRA || op == R4300_OP_SRAV;
        unsigned ext = op == R4300_OP_SLL || op == R4300_OP_SLLV || op == R4300_OP_DSLL ||
                       op == R4300_OP_DSLL32 || op == R4300_OP_DSLLV ? 4 :
                       op == R4300_OP_SRL || op == R4300_OP_SRLV || op == R4300_OP_DSRL ||
                       op == R4300_OP_DSRL32 || op == R4300_OP_DSRLV ? 5 : 7;
        if (op == R4300_OP_DSLL32 || op == R4300_OP_DSRL32 || op == R4300_OP_DSRA32) sa += 32;
        emit_state_load(gpr_offset(rt), RAX);
        if (variable) emit_state_load(gpr_offset(rs), RCX);
        emit_shift(ext, sa, variable, word ? (ext == 7 ? 2 : 0) : 1);
        break;
    }
    default:
        emit_state_load(gpr_offset(rs), RAX);
        emit_state_load(gpr_offset(rt), RCX);
        switch (op) {
        case R4300_OP_ADDU: case R4300_OP_DADDU:
            word = op == R4300_OP_ADDU; emit_binary(0x01, !word); break;
        case R4300_OP_SUBU: case R4300_OP_DSUBU:
            word = op == R4300_OP_SUBU; emit_binary(0x29, !word); break;
        case R4300_OP_AND: emit_binary(0x21, 1); break;
        case R4300_OP_OR: case R4300_OP_NOR:
            emit_binary(0x09, 1);
            if (op == R4300_OP_NOR) { output_rex(1, 0, 0, 0); output_byte(0xf7); output_modrm(3, RAX, 2); }
            break;
        case R4300_OP_XOR: emit_binary(0x31, 1); break;
        case R4300_OP_SLT: case R4300_OP_SLTU:
            emit_binary(0x39, 1);
            output_byte(0x0f); output_byte(op == R4300_OP_SLT ? 0x9c : 0x92); output_modrm(3, RAX, 0);
            output_byte(0x0f); output_byte(0xb6); output_modrm(3, RAX, RAX);
            break;
        default: abort();
        }
    }
    if (word) emit_sign_extend_word();
    if (destination) emit_state_store(gpr_offset(destination), RAX);
}

static uint64_t compile_relative_calls, compile_absolute_calls;
static void emit_helper_call(const void* function)
{
    int64_t displacement = (int64_t)(uintptr_t)function - (int64_t)(uintptr_t)(out + 5);
    if (displacement >= INT32_MIN && displacement <= INT32_MAX) {
        output_byte(0xe8); output_w32((uint32_t)(int32_t)displacement);
        ++compile_relative_calls;
    } else {
        emit_movimm64((intptr_t)function, R11);
        output_rex(0, 0, 0, 1); output_byte(0xff); output_modrm(3, R11 & 7, 2);
        ++compile_absolute_calls;
    }
}
static void emit_step_call(void* block, unsigned index, unsigned phase, ari64_x64_step step)
{
    emit_mov64(R12, ARG1_REG);
    emit_movimm64((intptr_t)block, ARG2_REG);
    emit_movimm(index, ARG3_REG);
    emit_movimm(phase, ARG4_REG);
    emit_helper_call((const void*)step);
}
static unsigned char* emit_jump(unsigned condition)
{
    output_byte(0x0f); output_byte(0x80 | condition);
    unsigned char* displacement = out;
    output_w32(0);
    return displacement;
}
static void patch_jump(unsigned char* displacement, unsigned char* target)
{
    int32_t relative = (int32_t)(target - (displacement + 4));
    memcpy(displacement, &relative, 4);
}
#include "memory_x64.inc"
#include "muldiv_x64.inc"
#include "branch_x64.inc"
#include "timing_x64.inc"
#include "fpu_x64.inc"
#include "alu_x64.inc"
#include "link_x64.inc"
int ari64_x64_has_native(const uint32_t* ops, unsigned count, uint32_t delay_op, unsigned has_delay, unsigned idle)
{
    for (unsigned i = 0; i < count; ++i) {
        enum r4300_opcode op = r4300_get_idec(ops[i])->opcode;
        if (native_opcode(op) || native_ram_opcode(op) || native_fpu_transfer(op) || native_fpu_instruction(ops[i])) return 1;
    }
    return count && count < MAX_INSTRUCTIONS && has_delay && !idle &&
        native_branch_opcode(r4300_get_idec(ops[count - 1])->opcode) &&
        native_opcode(r4300_get_idec(delay_op)->opcode);
}
static unsigned char* emit_step(void* block, unsigned index, unsigned phase, ari64_x64_step step)
{
    emit_step_call(block, index, phase, step);
    output_byte(0x85); output_modrm(3, RAX, RAX);
    return emit_jump(4);
}

static ari64_x64_entry compile_block(struct ari64_x64* s, void* block,
    const uint32_t* ops, unsigned count, ari64_x64_step step, int optimized, const struct ari64_x64_config* config)
{
    ari64_x64_link link = config ? config->link : NULL;
    if (!count || count > MAX_INSTRUCTIONS) return NULL;
    uint32_t extended_ops[MAX_INSTRUCTIONS];
    unsigned branch_index = MAX_INSTRUCTIONS;
    if (optimized && config && config->has_delay && !config->idle_end && count < MAX_INSTRUCTIONS &&
        native_branch_opcode(r4300_get_idec(ops[count - 1])->opcode) && native_opcode(r4300_get_idec(config->delay_op)->opcode)) {
        memcpy(extended_ops, ops, count * sizeof(*ops));
        branch_index = count - 1; extended_ops[count++] = config->delay_op; ops = extended_ops;
    }
    /* Native RAM guards have a snapshot writeback stub at every fault site.
     * The maximum encoding is bounded by 2048 bytes per instruction plus
     * the prologue/epilogue. Compile only while no generated frame is active. */
    size_t reserve = (count * 2048 + 512 + 15) & ~(size_t)15;
    if (reserve > SEGMENT_BYTES) return NULL;
    if ((s->used % SEGMENT_BYTES) + reserve > SEGMENT_BYTES) {
        s->used = ((s->used / SEGMENT_BYTES + 1) * SEGMENT_BYTES) % CODE_BYTES;
        if (!code_protect(s->code + s->used, SEGMENT_BYTES, 0)) return NULL;
        s->segment_generation[s->used / SEGMENT_BYTES] = ++s->generation;
    }
    unsigned char* start = s->code + s->used;
    size_t page_start = s->used & ~(s->page_size - 1);
    size_t page_end = (s->used + reserve + s->page_size - 1) & ~(s->page_size - 1);
    /* Pack aligned entries into pages. Publication may share the preceding
     * block's page; compilation only runs with every generated frame unwound,
     * so temporarily making that page writable cannot interrupt native code. */
    if (!code_protect(s->code + page_start, page_end - page_start, 0)) return NULL;
    unsigned char* exits[MAX_INSTRUCTIONS * 9 + 16];
    struct memory_exit memory_exits[MAX_INSTRUCTIONS];
    unsigned memory_exit_count = 0;
    unsigned exit_count = 0;
    out = start;
    compile_relative_calls = compile_absolute_calls = 0;
    /* ENDBR64 is a NOP on CPUs without CET. */
    output_byte(0xf3); output_byte(0x0f); output_byte(0x1e); output_byte(0xfa);
    output_byte(0x41); output_byte(0x54); /* push R12; 16-byte call alignment */
    if (optimized) {
        output_byte(0x53); output_byte(0x55); /* RBX/RBP register cache */
#ifdef _WIN32
        output_byte(0x56); output_byte(0x57); /* RSI/RDI register cache */
#endif
        output_byte(0x41); output_byte(0x55); /* R13 pending clocks */
        output_byte(0x41); output_byte(0x56); /* R14 memory pointer */
        output_byte(0x41); output_byte(0x57); /* R15 register cache */
    }
    emit_mov64(ARG1_REG, R12);
#ifdef _WIN32
    emit_addimm64(RSP, optimized ? -88 : -32, RSP); /* shadow space, FPU scratch, alignment */
#else
    if (optimized) emit_addimm64(RSP, -56, RSP);
#endif
    unsigned char* chain_entry = out;
    output_byte(0xf3); output_byte(0x0f); output_byte(0x1e); output_byte(0xfa);
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
                emit_mem(0xc7, 0, 0, RSP, -1, 0, 8); output_w32(0);
                if (memory_region || likely_region) regcache.allow_dead_store = 0;
                emit_movimm(0, R13);
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
                    output_byte(0xe9); unsigned char* taken = out; output_w32(0);
                    patch_jump(annul, out);
                    regcache = annul_registers; regcache_flush();
                    emit_pipeline_clear();
                    emit_region_commit_native(config, i, end - i - 1, 1);
                    patch_jump(taken, out);
                }
                /* Unconditional jump over the precise event/mode slow path. */
                output_byte(0xe9); unsigned char* done = out; output_w32(0);
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
            exits[exit_count++] = emit_step(block,i,1,step);
            ++s->native; ++i;
        } else if (optimized && config->fpu_prepare && config->fpu_finish && native_fpu_instruction(ops[i])) {
            emit_fpu_native(ops[i], i, config, exits, &exit_count); ++s->native; ++i;
        } else { exits[exit_count++] = emit_step(block, i, 2, step); ++i; }
    }
    if (memory_exit_count) {
        output_byte(0xe9); exits[exit_count++] = out; output_w32(0);
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
            emit_mem(0xc7, 0, 0, RSP, -1, 0, 8); output_w32(exit->index + 1 - exit->start);
            emit_movimm(0, R13);
            /* Helpers may clobber volatile host registers and the loaded
             * guest destination. Restore the post-instruction allocation. */
            for (unsigned h = 0; h < CACHE_REGS; ++h)
                if (exit->resume_registers.map[h] >= 0)
                    emit_state_load_raw(guest_offset(exit->resume_registers.map[h]), cache_host[h]);
            output_byte(0xe9); unsigned char* resume = out; output_w32(0);
            patch_jump(resume, exit->resume);
        }
    }
    unsigned char* epilogue = out;
    if (link) {
        if (config->links) emit_direct_links(block,config);
        emit_mov64(R12, ARG1_REG);
        emit_helper_call((const void*)link);
        emit_reg_op(0x85,RAX,RAX,1);
        output_byte(0x74); output_byte(2);
        output_byte(0xff); output_modrm(3,RAX,4);
    }
#ifdef _WIN32
    emit_addimm64(RSP, optimized ? 88 : 32, RSP);
#else
    if (optimized) emit_addimm64(RSP, 56, RSP);
#endif
    if (optimized) {
        output_byte(0x41); output_byte(0x5f);
        output_byte(0x41); output_byte(0x5e);
        output_byte(0x41); output_byte(0x5d);
#ifdef _WIN32
        output_byte(0x5f); output_byte(0x5e);
#endif
        output_byte(0x5d); output_byte(0x5b);
    }
    output_byte(0x41); output_byte(0x5c);

    output_byte(0xc3);
    for (unsigned i = 0; i < exit_count; ++i) {
        patch_jump(exits[i], epilogue);
    }
    assert((size_t)(out - start) <= reserve);
    s->used = ((size_t)(out - s->code) + 15) & ~(size_t)15;
    page_end = (s->used + s->page_size - 1) & ~(s->page_size - 1);
    if (!code_protect(s->code + page_start, page_end - page_start, 1)) return NULL;
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), start, (size_t)(out - start));
#else
    __builtin___clear_cache((char*)start, (char*)out);
#endif
    ++s->blocks;
    s->emitted_bytes += out - start;
    s->relative_calls += compile_relative_calls;
    s->absolute_calls += compile_absolute_calls;
    if (config && config->chain_entry) *config->chain_entry = (ari64_x64_entry)(void*)chain_entry;
    return (ari64_x64_entry)(void*)start;
}

ari64_x64_entry ari64_x64_compile(struct ari64_x64* s, void* block,
    const uint32_t* ops, unsigned count, ari64_x64_step step)
{
    return compile_block(s, block, ops, count, step, 0, NULL);
}
ari64_x64_entry ari64_x64_compile_optimized(struct ari64_x64* s, void* block,
    const uint32_t* ops, unsigned count, ari64_x64_step step, const struct ari64_x64_config* config)
{
    return compile_block(s, block, ops, count, step, 1, config);
}
