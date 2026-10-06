/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef M64P_INTERP_PAIR_H
#define M64P_INTERP_PAIR_H

/* LUI followed by a nontrapping immediate operation on its destination.
 * The caller must keep the pair within one code page and exclude interrupt
 * boundaries, delay slots, user mode and guest cache emulation. */
static osal_force_inline int interp_lui_pair(uint32_t first, uint32_t second)
{
    unsigned int rt = (first >> 16) & 31;
    unsigned int major = second >> 26;
    return rt && ((second >> 16) & 31) == rt && ((second >> 21) & 31) == rt &&
        (major == 9 || major == 13);
}

static osal_force_inline int64_t interp_lui_pair_value(uint32_t first, uint32_t second)
{
    uint32_t value = first << 16;
    if ((second >> 26) == 9) value += (uint32_t)(int32_t)(int16_t)second;
    else value |= (uint16_t)second;
    return (int64_t)(int32_t)value;
}
#endif
