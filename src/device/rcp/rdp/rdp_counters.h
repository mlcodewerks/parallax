#ifndef WTFN64_RDP_COUNTERS_H
#define WTFN64_RDP_COUNTERS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RDP_COUNTER_STATE_OFFSET 151552u
#define RDP_COUNTER_STATE_BYTES 262144u
#define RDP_EXTRA_STATE_BYTES (RDP_COUNTER_STATE_OFFSET + RDP_COUNTER_STATE_BYTES)
#define RDP_COUNTER_MODULUS (UINT64_C(0x1000000) * 3)

/* Times are quarter COUNT ticks: three per RCP clock. Unlike a single busy
 * deadline, this retains idle gaps and future clears produced synchronously
 * by an RSP slice while the CPU is still earlier on the shared timeline. */
struct rdp_counter_interval { int64_t begin, end; uint32_t mask; };
struct rdp_counter_clear { int64_t at; uint32_t mask; };
struct rdp_counters {
    int64_t epoch, last_clear[3];
    uint64_t accumulated[3];
    struct rdp_counter_interval *work;
    struct rdp_counter_clear *clears;
    size_t work_count, work_capacity, clear_count, clear_capacity;
};

static inline void *rdp_counter_reserve(void *p, size_t *capacity, size_t count, size_t stride)
{
    if (count <= *capacity) return p;
    size_t n = *capacity ? *capacity * 2 : 16;
    if (n < count) n = count;
    if (n > SIZE_MAX / stride) abort();
    void *q = realloc(p, n * stride);
    if (!q) abort();
    *capacity = n;
    return q;
}

static inline void rdp_counters_reset(struct rdp_counters *c)
{
    free(c->work); free(c->clears);
    memset(c, 0, sizeof(*c));
}

static inline int64_t rdp_counter_max(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t rdp_counter_min(int64_t a, int64_t b) { return a < b ? a : b; }

static inline uint64_t rdp_counter_quarters(const struct rdp_counters *c, unsigned index, int64_t now)
{
    int64_t from = c->epoch;
    uint64_t value = c->accumulated[index];
    for (size_t i = 0; i < c->clear_count; ++i) {
        if (c->clears[i].at > now) break;
        if (c->clears[i].mask & (1u << index)) {
            from = c->clears[i].at;
            value = 0;
        }
    }
    int64_t covered = from;
    for (size_t i = 0; i < c->work_count; ++i) {
        const struct rdp_counter_interval *w = &c->work[i];
        if (!(w->mask & (1u << index))) continue;
        int64_t begin = rdp_counter_max(w->begin, covered);
        int64_t end = rdp_counter_min(w->end, now);
        if (end > begin) { value += (uint64_t)(end - begin); covered = end; }
    }
    return value % RDP_COUNTER_MODULUS;
}

static inline uint32_t rdp_counter_value(const struct rdp_counters *c, unsigned index, int64_t now)
{
    return (uint32_t)(rdp_counter_quarters(c, index, now) / 3);
}

static inline void rdp_counters_advance(struct rdp_counters *c, int64_t now)
{
    if (now <= c->epoch) return;
    uint64_t values[3];
    for (unsigned i = 0; i < 3; ++i) values[i] = rdp_counter_quarters(c, i, now);
    size_t n = 0;
    for (size_t i = 0; i < c->work_count; ++i) {
        if (c->work[i].end <= now) continue;
        c->work[n] = c->work[i];
        c->work[n].begin = rdp_counter_max(c->work[n].begin, now);
        ++n;
    }
    c->work_count = n;
    n = 0;
    for (size_t i = 0; i < c->clear_count; ++i) {
        if (c->clears[i].at <= now) {
            for (unsigned j = 0; j < 3; ++j)
                if (c->clears[i].mask & (1u << j)) c->last_clear[j] = c->clears[i].at;
        } else c->clears[n++] = c->clears[i];
    }
    c->clear_count = n;
    memcpy(c->accumulated, values, sizeof(values));
    c->epoch = now;
}

static inline void rdp_counters_work(struct rdp_counters *c, int64_t begin, int64_t end, unsigned mask)
{
    if (end <= begin || !mask) return;
    /* Late service of a producer can publish already elapsed work. Serial DP
     * commands cannot overlap older retired work; charge its elapsed prefix. */
    for (unsigned i = 0; i < 3; ++i) if (mask & (1u << i)) {
        int64_t lo = rdp_counter_max(begin, c->last_clear[i]);
        int64_t hi = rdp_counter_min(end, c->epoch);
        if (hi > lo) c->accumulated[i] =
            (c->accumulated[i] + (uint64_t)(hi - lo)) % RDP_COUNTER_MODULUS;
    }
    begin = rdp_counter_max(begin, c->epoch);
    if (end <= begin) return;
    if (c->work_count) {
        struct rdp_counter_interval *last = &c->work[c->work_count - 1];
        if (last->end == begin && last->mask == mask) { last->end = end; return; }
    }
    c->work = (struct rdp_counter_interval *)rdp_counter_reserve(c->work, &c->work_capacity, c->work_count + 1, sizeof(*c->work));
    size_t at = c->work_count++;
    while (at && c->work[at - 1].begin > begin) { c->work[at] = c->work[at - 1]; --at; }
    c->work[at] = (struct rdp_counter_interval){begin, end, mask};
}

static inline void rdp_counters_clear(struct rdp_counters *c, int64_t at, unsigned mask)
{
    c->clears = (struct rdp_counter_clear *)rdp_counter_reserve(c->clears, &c->clear_capacity, c->clear_count + 1, sizeof(*c->clears));
    size_t i = c->clear_count++;
    while (i && c->clears[i - 1].at > at) { c->clears[i] = c->clears[i - 1]; --i; }
    c->clears[i] = (struct rdp_counter_clear){at, mask};
}

static inline void rdp_counter_put(uint8_t **p, uint64_t value, unsigned bytes)
{ for (unsigned i = 0; i < bytes; ++i) *(*p)++ = (uint8_t)(value >> (i * 8)); }
static inline uint64_t rdp_counter_get(const uint8_t **p, unsigned bytes)
{ uint64_t v = 0; for (unsigned i = 0; i < bytes; ++i) v |= (uint64_t)*(*p)++ << (i * 8); return v; }

static inline int rdp_counters_save(const struct rdp_counters *c, uint8_t *out, size_t size)
{
    if (size < 72 || c->work_count > (size - 72) / 24 ||
        c->clear_count > (size - 72 - c->work_count * 24) / 16) return 0;
    rdp_counter_put(&out, UINT32_C(0x31435044), 4);
    rdp_counter_put(&out, c->work_count, 4); rdp_counter_put(&out, c->clear_count, 4);
    rdp_counter_put(&out, 0, 4); rdp_counter_put(&out, (uint64_t)c->epoch, 8);
    for (unsigned i = 0; i < 3; ++i) rdp_counter_put(&out, c->accumulated[i], 8);
    for (unsigned i = 0; i < 3; ++i) rdp_counter_put(&out, (uint64_t)c->last_clear[i], 8);
    for (size_t i = 0; i < c->work_count; ++i) {
        rdp_counter_put(&out, (uint64_t)c->work[i].begin, 8);
        rdp_counter_put(&out, (uint64_t)c->work[i].end, 8);
        rdp_counter_put(&out, c->work[i].mask, 8);
    }
    for (size_t i = 0; i < c->clear_count; ++i) {
        rdp_counter_put(&out, (uint64_t)c->clears[i].at, 8);
        rdp_counter_put(&out, c->clears[i].mask, 8);
    }
    return 1;
}

static inline int rdp_counters_load(struct rdp_counters *c, const uint8_t *in, size_t size)
{
    if (size < 72 || rdp_counter_get(&in, 4) != UINT32_C(0x31435044)) return 0;
    size_t nw = (size_t)rdp_counter_get(&in, 4), nc = (size_t)rdp_counter_get(&in, 4);
    if (nw > (size - 72) / 24 || nc > (size - 72 - nw * 24) / 16) return 0;
    rdp_counter_get(&in, 4);
    struct rdp_counters next = {0};
    next.epoch = (int64_t)rdp_counter_get(&in, 8);
    if (next.epoch < 0) goto invalid;
    for (unsigned i = 0; i < 3; ++i) next.accumulated[i] = rdp_counter_get(&in, 8) % RDP_COUNTER_MODULUS;
    for (unsigned i = 0; i < 3; ++i) {
        next.last_clear[i] = (int64_t)rdp_counter_get(&in, 8);
        if (next.last_clear[i] < 0 || next.last_clear[i] > next.epoch) goto invalid;
    }
    for (size_t i = 0; i < nw; ++i) {
        int64_t begin = (int64_t)rdp_counter_get(&in, 8), end = (int64_t)rdp_counter_get(&in, 8);
        uint64_t mask = rdp_counter_get(&in, 8);
        if (begin < next.epoch || end <= begin || !mask || mask > 7) goto invalid;
        rdp_counters_work(&next, begin, end, (unsigned)mask);
    }
    for (size_t i = 0; i < nc; ++i) {
        int64_t at = (int64_t)rdp_counter_get(&in, 8);
        uint64_t mask = rdp_counter_get(&in, 8);
        if (at < next.epoch || !mask || mask > 7) goto invalid;
        rdp_counters_clear(&next, at, (unsigned)mask);
    }
    rdp_counters_reset(c); *c = next; return 1;
invalid:
    rdp_counters_reset(&next); return 0;
}
#endif
