#ifndef WTFN64_RSP_DPC_BRIDGE_H
#define WTFN64_RSP_DPC_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal integration hook; keep the public RSP plugin ABI unchanged. */
void parallelRSPSetDpcCallbacks(void* opaque,
    void (*read)(void*, uint32_t, uint32_t*),
    void (*write)(void*, uint32_t, uint32_t, uint32_t));
unsigned int parallelRSPExecutedCycles(void);
enum { PARALLEL_RSP_STATE_WORDS = 369 };
void parallelRSPSaveState(uint32_t* words);
void parallelRSPLoadState(const uint32_t* words);

#ifdef __cplusplus
}
#endif

#endif
