#ifndef RDP_EMIT_ZBOSS_H
#define RDP_EMIT_ZBOSS_H

#include "rdp_emit_f3dex2.h"    /* RdpFifo */

#ifdef __cplusplus
extern "C" {
#endif

/* triangle command buffer: one edge+shade+texture triangle is at most 44
 * words; a quad emits two */
#define GSP_ZB_TRI_WORDS 44

#define ZBOSS_OP_FRESH   0
#define ZBOSS_OP_RESUME  1

#define ZBOSS_R_DONE       0
#define ZBOSS_R_WAIT_SIG3  1
#define ZBOSS_R_WAIT_SIG0  2

/* Sliced ZSortBOSS interpreter (World Driver Championship, Stunt Racer
 * 64). op ZBOSS_OP_FRESH starts the task from the OSTask double display
 * list (DMEM 0xff0 main / 0xff8 sub); ZBOSS_OP_RESUME continues after
 * the caller has satisfied the returned wait condition. Returns a
 * ZBOSS_R_* code, or -1 on error (caller falls back to LLE). */
void zboss_set_shadow(int on);
/* Estimated clocks of the last executed slice, excluding external waits. */
unsigned int zboss_cycles(void);

int zboss_run(unsigned char *rdram, unsigned int rdram_size,
              unsigned char *dmem, RdpFifo *fifo, int op,
              unsigned int *sp_status);
/* Diagnostic walk; preserve the live BOSS continuation and bindings. */
int zboss_run_shadow(unsigned char *rdram, unsigned int rdram_size,
                     unsigned char *dmem, RdpFifo *fifo);

#ifdef __cplusplus
}
#endif

#endif
