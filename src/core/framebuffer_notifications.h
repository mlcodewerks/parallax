#ifndef PARALLAX_FRAMEBUFFER_NOTIFICATIONS_H
#define PARALLAX_FRAMEBUFFER_NOTIFICATIONS_H
#include <stdint.h>

enum {
    CORE_FB_PAGE_SHIFT = 12,
    CORE_FB_PAGE_READ = 1,
    CORE_FB_PAGE_WRITE = 2,
    /* RAM is authoritative: one page invalidation suffices until GPU use. */
    CORE_FB_PAGE_INVALIDATE = 4
};

/* Internal page ownership, independent of the legacy six-image FBInfo ABI. */
static inline void core_framebuffer_mark_pages(uint8_t *pages, unsigned count,
                                               uint32_t address, uint64_t bytes, uint8_t flags)
{
    const uint64_t limit = (uint64_t)count << CORE_FB_PAGE_SHIFT;
    uint64_t end = (uint64_t)address + bytes;
    if (!bytes || address >= limit) return;
    if (end > limit) end = limit;
    for (unsigned page = address >> CORE_FB_PAGE_SHIFT; page <= (end - 1) >> CORE_FB_PAGE_SHIFT; ++page)
        pages[page] |= flags;
}
#endif
