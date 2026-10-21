/* Shared Ari64 host-emitter interface. GPL-2.0-or-later. */
#ifndef PARALLAX_ARI64_H
#define PARALLAX_ARI64_H
#ifndef ARI64_BACKEND_NAME
#if defined(__aarch64__)
#define ARI64_BACKEND_NAME "ari64-arm64"
#else
#define ARI64_BACKEND_NAME "ari64-x64"
#endif
#endif
#include <stddef.h>
#include <stdint.h>
struct r4300_core;
struct ari64;
typedef void (*ari64_entry)(struct r4300_core*);
/* 0: begin native instruction, 1: commit native instruction,
 * 2: shared precise instruction; 5 | (count << 8): precise region;
 * 11: begin COP1 transfer with its unusable check. Zero exits the block. */
typedef int (*ari64_step)(struct r4300_core*, void*, unsigned, unsigned);
typedef ari64_entry (*ari64_link)(struct r4300_core*);
typedef int (*ari64_fpu_prepare)(struct r4300_core*, uint32_t, uint64_t inputs[3], uint64_t* commits);
typedef int (*ari64_fpu_finish)(struct r4300_core*, uint32_t, uint64_t, unsigned);
/* Filled only by the validated C linker, consumed by emitted tail jumps.
 * Entry identity catches decoded-slot replacement; page/epoch/segment stamps
 * catch code writes, remapping and arena recycling. */
struct ari64_link_cache {
    uint32_t pc, epoch, page_generation;
    ari64_entry expected_entry, body;
    ari64_entry* current_entry;
    const uint32_t* current_page_generation;
    const uint32_t* current_mapping;
    uint32_t mapping;
    const uint64_t* current_segment_generation;
    uint64_t segment_generation;
    void* target_block;
    uint64_t* profile_entries;
};
struct ari64_config {
    uint32_t virtual_pc, physical_page;
    unsigned cache_timing;
    ari64_link link;
    uint32_t delay_op;
    unsigned has_delay, idle_end;
    uint64_t* commits;
    uint64_t* fast_commits;
    ari64_fpu_prepare fpu_prepare;
    ari64_fpu_finish fpu_finish;
    ari64_entry* chain_entry;
    struct ari64_link_cache* links;
    void* previous_block;
    const uint32_t* epoch;
    uint64_t* entries;
    uint64_t* linked_entries;
    uint64_t* direct_links;
    unsigned profile;
    unsigned compat_issue_clock;
};
struct ari64* ari64_create(void);
void ari64_destroy(struct ari64*);
uint64_t ari64_generation(const struct ari64*);
int ari64_entry_valid(const struct ari64*, ari64_entry, uint64_t generation);
const uint64_t* ari64_entry_generation_ptr(const struct ari64*, ari64_entry);
int ari64_has_native(const uint32_t* ops, unsigned count, uint32_t delay_op, unsigned has_delay, unsigned idle);
ari64_entry ari64_compile(struct ari64*, void* block,
    const uint32_t* ops, unsigned count, ari64_step step);
/* Emit host code with shared precise instruction boundaries. Native coverage
 * is host-specific (see README.md). Optional linking returns a published,
 * validated successor that must not grow the host stack. */
ari64_entry ari64_compile_optimized(struct ari64*, void* block,
    const uint32_t* ops, unsigned count, ari64_step step, const struct ari64_config*);
void ari64_stats(const struct ari64*, uint64_t* blocks, uint64_t* native);
void ari64_code_stats(const struct ari64*, uint64_t stats[3]);
#endif
