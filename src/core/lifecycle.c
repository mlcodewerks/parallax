/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lifecycle.h"
#include "main/main.h"
#include "main/rom.h"
#include "device/memory.h"
#include "device/rcp/rsp/optional_rsp.h"
static bool content_open;
bool core_open_content(const void *data, unsigned size)
{
    if (content_open || g_mem_base || !data || size < 4096) return false;
    g_mem_base = init_mem_base();
    if (!g_mem_base) return false;
    romdatabase_open();
    if (open_rom(data, size) != CORE_OK) {
        romdatabase_close();
        release_mem_base(g_mem_base);
        g_mem_base = NULL;
        return false;
    }
    content_open = true;
    return true;
}
void core_close_content(void)
{
    if (!g_mem_base) return;
    main_stop();
    optional_rsp_shutdown();
    if (content_open) close_rom();
    content_open = false;
    romdatabase_close();
    release_mem_base(g_mem_base);
    g_mem_base = NULL;
}
