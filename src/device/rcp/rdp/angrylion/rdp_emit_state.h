/* Pointer-free snapshots of persistent display-list walkers. FIFOs are drained
 * at slice boundaries; host memory and backend pointers are rebound on resume. */
#ifndef RDP_EMIT_STATE_H
#define RDP_EMIT_STATE_H
#include <string.h>
#define EMIT_STATE_SIZE(v) n += sizeof(v);
#define EMIT_STATE_SAVE(v) memcpy(p, &(v), sizeof(v)); p += sizeof(v);
#define EMIT_STATE_LOAD(v) memcpy(&(v), p, sizeof(v)); p += sizeof(v);
#define EMIT_STATE_RESET(v) memset(&(v), 0, sizeof(v));
#define EMIT_STATE_DEFINE(name, fields) \
unsigned int name##_state_size(void) { unsigned int n = 0; fields(EMIT_STATE_SIZE) return n; } \
void name##_state_save(void *buffer) { unsigned char *p = (unsigned char *)buffer; fields(EMIT_STATE_SAVE) } \
void name##_state_load(const void *buffer) { const unsigned char *p = (const unsigned char *)buffer; \
    if (!p) { fields(EMIT_STATE_RESET) return; } fields(EMIT_STATE_LOAD) }
#define EMIT_STATE_DECLARE(name) \
unsigned int name##_state_size(void); \
void name##_state_save(void *); \
void name##_state_load(const void *);
EMIT_STATE_DECLARE(f3dex2)
EMIT_STATE_DECLARE(rs)
EMIT_STATE_DECLARE(naboo)
EMIT_STATE_DECLARE(zboss)
EMIT_STATE_DECLARE(rsp_emit)
EMIT_STATE_DECLARE(hle_stream)
int hle_stream_can_resume(unsigned int checksum);
#endif
