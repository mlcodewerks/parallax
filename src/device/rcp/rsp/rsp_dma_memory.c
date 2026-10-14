/* Shared memory-coherence callbacks for CPU- and RSP-initiated DMA. */
#include "rsp_core.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/mi_controller.h"
#include "device/rcp/rdp/rdp_core.h"

void rsp_dma_read_memory(void* opaque, uint32_t address, uint32_t length)
{
    struct rsp_core* sp = (struct rsp_core*)opaque;
    address &= 0x7fffff;
    while (length) {
        uint32_t size = length < 0x800000 - address ? length : 0x800000 - address;
        pre_framebuffer_read_range(&sp->dp->fb, address, size);
        length -= size; address = 0;
    }
}

void rsp_dma_write_memory(void* opaque, uint32_t address, uint32_t length)
{
    struct rsp_core* sp = (struct rsp_core*)opaque;
    address &= 0x7fffff;
    while (length) {
        uint32_t size = length < 0x800000 - address ? length : 0x800000 - address;
        invalidate_r4300_cached_code(sp->mi->r4300, address, size);
        post_framebuffer_write(&sp->dp->fb, address, size);
        length -= size; address = 0;
    }
}
