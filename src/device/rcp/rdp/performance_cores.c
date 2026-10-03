#include "performance_cores.h"
#include <stdlib.h>
#include <string.h>
#include <features/features_cpu.h>
#include <libretro.h>

extern retro_log_printf_t log_cb;
static bool initialized, enabled = true;
static unsigned core_count;
#ifdef _WIN32
static GROUP_AFFINITY cores[1024];
#endif

void performance_cores_init(void)
{
    if (initialized) return;
    initialized = true;
#ifdef _WIN32
    DWORD bytes = 0, offset;
    unsigned highest = 0;
    GROUP_AFFINITY current;
    DWORD_PTR process_mask = 0, system_mask = 0;
    unsigned char *buffer;
    GetThreadGroupAffinity(GetCurrentThread(), &current);
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &bytes);
    buffer = bytes ? (unsigned char *)malloc(bytes) : NULL;
    if (buffer && GetLogicalProcessorInformationEx(RelationProcessorCore,
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &bytes))
    {
        /* Higher EfficiencyClass means greater performance on Windows.
         * All-zero classes describe a homogeneous CPU: every core qualifies. */
        for (offset = 0; offset < bytes; )
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (void *)(buffer + offset);
            if (!info->Size || info->Size > bytes - offset) break;
            if (info->Relationship == RelationProcessorCore &&
                info->Processor.EfficiencyClass > highest)
                highest = info->Processor.EfficiencyClass;
            offset += info->Size;
        }
        for (offset = 0; offset < bytes; )
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (void *)(buffer + offset);
            if (!info->Size || info->Size > bytes - offset) break;
            if (info->Relationship == RelationProcessorCore &&
                info->Processor.EfficiencyClass == highest && info->Processor.GroupCount)
            {
                GROUP_AFFINITY affinity = info->Processor.GroupMask[0];
                if (affinity.Group == current.Group && process_mask)
                    affinity.Mask &= process_mask;
                if (affinity.Mask && core_count < sizeof(cores) / sizeof(cores[0]))
                    cores[core_count++] = affinity;
            }
            offset += info->Size;
        }
    }
    free(buffer);
#endif
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Renderer: %u physical performance cores detected; affinity %s.\n",
                core_count, core_count ? "available" : "unavailable (OS scheduling)");
}

unsigned performance_cores_count(void)
{
    performance_cores_init();
    /* Do not invent P-core topology when the OS cannot report it. */
    return core_count ? core_count : cpu_features_get_core_amount();
}

void performance_cores_set_enabled(bool value) { enabled = value; }

bool performance_emulation_enter(struct performance_affinity *state)
{
    memset(state, 0, sizeof(*state));
#ifdef _WIN32
    if (enabled && core_count)
    {
        GROUP_AFFINITY current, preferred;
        KAFFINITY eligible = 0;
        if (!GetThreadGroupAffinity(GetCurrentThread(), &current))
            return false;
        preferred = current;
        preferred.Mask = 0;
        for (unsigned i = 0; i < core_count; ++i)
            if (cores[i].Group == current.Group)
            {
                KAFFINITY available = cores[i].Mask & current.Mask;
                eligible |= available;
                if (!preferred.Mask) preferred.Mask = available;
            }
        /* Respect explicit host pinning, including masks with no P-core.
         * Homogeneous CPUs and already restricted threads need no change. */
        /* Rendering lane zero already runs on the first eligible physical
         * core. Keep CPU/RSP execution there between batches too, rather
         * than migrating its working set across P/E cores repeatedly. */
        if (eligible && eligible != current.Mask)
            state->changed = SetThreadGroupAffinity(GetCurrentThread(),
                    &preferred, &state->previous) != 0;
    }
#endif
    return state->changed;
}

bool performance_core_enter(unsigned worker, struct performance_affinity *state)
{
    memset(state, 0, sizeof(*state));
#ifdef _WIN32
    if (enabled && core_count)
        state->changed = SetThreadGroupAffinity(GetCurrentThread(),
                &cores[worker % core_count], &state->previous) != 0;
#else
    (void)worker;
#endif
    return state->changed;
}

void performance_core_leave(struct performance_affinity *state)
{
#ifdef _WIN32
    if (state->changed)
        SetThreadGroupAffinity(GetCurrentThread(), &state->previous, NULL);
#endif
    state->changed = false;
}
