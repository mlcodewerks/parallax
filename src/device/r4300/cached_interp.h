

#ifndef M64P_DEVICE_R4300_CACHED_INTERP_H
#define M64P_DEVICE_R4300_CACHED_INTERP_H

#include <stddef.h>
#include <stdint.h>

struct r4300_core;
struct cached_interp_state;

int cached_interp_init(struct r4300_core* r4300);
void cached_interp_destroy(struct r4300_core* r4300);
void cached_interp_invalidate(struct r4300_core* r4300, uint32_t address, size_t size);
void cached_interp_execute_one(struct r4300_core* r4300);
void cached_interp_run(struct r4300_core* r4300);

#endif /* M64P_DEVICE_R4300_CACHED_INTERP_H */
