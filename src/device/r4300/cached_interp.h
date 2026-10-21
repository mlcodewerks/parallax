

#ifndef PARALLAX_DEVICE_R4300_CACHED_INTERP_H
#define PARALLAX_DEVICE_R4300_CACHED_INTERP_H

#include <stddef.h>
#include <stdint.h>

struct r4300_core;
struct cached_interp_state;

int cached_interp_init(struct r4300_core* r4300);
void cached_interp_destroy(struct r4300_core* r4300);
void cached_interp_invalidate(struct r4300_core* r4300, uint32_t address, size_t size);
void cached_interp_execute_one(struct r4300_core* r4300);
void cached_interp_run(struct r4300_core* r4300);
void cached_interp_cache_stats(struct r4300_core*, uint64_t stats[2]);
#ifdef HAVE_ARI64_X64
void cached_dynarec_execute_one(struct r4300_core* r4300);
void cached_dynarec_run(struct r4300_core* r4300);
void cached_dynarec_stats(struct r4300_core* r4300, uint64_t stats[4]);
void cached_dynarec_optimization_stats(struct r4300_core* r4300, uint64_t stats[2]);
uint64_t cached_dynarec_direct_links(struct r4300_core* r4300);
void cached_dynarec_code_stats(struct r4300_core*, uint64_t stats[3]);
#endif

#endif /* PARALLAX_DEVICE_R4300_CACHED_INTERP_H */
