#ifndef RDP_EMIT_NABOO_H
#define RDP_EMIT_NABOO_H

#include "rdp_emit_f3dex2.h"

#define NABOO_R_DONE       0
#define NABOO_R_FALLBACK  (-1)

void naboo_set_rdram(unsigned char *rdram);
void naboo_set_rdram_size(unsigned int size);
void naboo_task_reset(unsigned int dl);
void naboo_seed_dmem(const unsigned char *dmem);
void naboo_seed_output(unsigned int end, unsigned int current);
void naboo_set_emit(int on);
void naboo_set_clock(unsigned int clock);
void naboo_set_phase(unsigned int phase);
int naboo_get_phase(unsigned int *phase);
void naboo_set_size_fraction(unsigned int fraction);
int naboo_get_size_fraction(unsigned int *fraction);
/* Packed retained lanes: V12[0:1], V2[4:7], V11[4:7], V13[4:7].
 * A pair stores its first lane in the low halfword. */
void naboo_set_context(const unsigned int context[7]);
void naboo_set_packet_cursor(unsigned int cursor);
unsigned int naboo_get_packet_cursor(void);
unsigned int naboo_get_context(unsigned int context[7]);
void naboo_begin_writes(void);
void naboo_end_writes(int commit);
int  naboo_run_dl(RdpFifo *fifo, unsigned int dl_addr, int resume);

#endif
