#include "hle_timing.h"
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - musyx.c                                         *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2013 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <boolean.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "arithmetics.h"
#include "audio.h"
#include "common.h"
#include "hle_external.h"
#include "hle_internal.h"
#include "memory.h"
#include "resample_hq.h"

/* various constants */
enum { SUBFRAME_SIZE = 192 };
enum { MAX_VOICES = 32 };

enum { SAMPLE_BUFFER_SIZE = 0x200 };


enum {
    SFD_VOICE_COUNT     = 0x0,
    SFD_SFX_INDEX       = 0x2,
    SFD_VOICE_BITMASK   = 0x4,
    SFD_STATE_PTR       = 0x8,
    SFD_SFX_PTR         = 0xc,
    SFD_VOICES          = 0x10,

    /* v2 only */
    SFD2_10_PTR         = 0x10,
    SFD2_14_BITMASK     = 0x14,
    SFD2_15_BITMASK     = 0x15,
    SFD2_16_BITMASK     = 0x16,
    SFD2_18_PTR         = 0x18,
    SFD2_1C_PTR         = 0x1c,
    SFD2_20_PTR         = 0x20,
    SFD2_24_PTR         = 0x24,
    SFD2_VOICES         = 0x28
};

enum {
    VOICE_ENV_BEGIN         = 0x00,
    VOICE_ENV_STEP          = 0x10,
    VOICE_PITCH_Q16         = 0x20,
    VOICE_PITCH_SHIFT       = 0x22,
    VOICE_CATSRC_0          = 0x24,
    VOICE_CATSRC_1          = 0x30,
    VOICE_ADPCM_FRAMES      = 0x3c,
    VOICE_SKIP_SAMPLES      = 0x3e,

    /* for PCM16 */
    VOICE_U16_40            = 0x40,
    VOICE_U16_42            = 0x42,

    /* for ADPCM */
    VOICE_ADPCM_TABLE_PTR   = 0x40,

    VOICE_INTERLEAVED_PTR   = 0x44,
    VOICE_END_POINT         = 0x48,
    VOICE_RESTART_POINT     = 0x4a,
    VOICE_U16_4C            = 0x4c,
    VOICE_U16_4E            = 0x4e,

    VOICE_SIZE              = 0x50
};

enum {
    CATSRC_PTR1     = 0x00,
    CATSRC_PTR2     = 0x04,
    CATSRC_SIZE1    = 0x08,
    CATSRC_SIZE2    = 0x0a
};

enum {
    STATE_LAST_SAMPLE   = 0x0,
    STATE_BASE_VOL      = 0x100,
    STATE_CC0           = 0x110,
    STATE_740_LAST4_V1  = 0x290,

    STATE_740_LAST4_V2  = 0x110
};

enum {
    SFX_CBUFFER_PTR     = 0x00,
    SFX_CBUFFER_LENGTH  = 0x04,
    SFX_TAP_COUNT       = 0x08,
    SFX_FIR4_HGAIN      = 0x0a,
    SFX_TAP_DELAYS      = 0x0c,
    SFX_TAP_GAINS       = 0x2c,
    SFX_U16_3C          = 0x3c,
    SFX_U16_3E          = 0x3e,
    SFX_FIR4_HCOEFFS    = 0x40
};


/* struct definition */
typedef struct {
    /* internal subframes */
    int16_t left[SUBFRAME_SIZE];
    int16_t right[SUBFRAME_SIZE];
    int16_t cc0[SUBFRAME_SIZE];
    int16_t e50[SUBFRAME_SIZE];

    /* internal subframes base volumes */
    int32_t base_vol[4];

    /* */
    int16_t subframe_740_last4[4];
    uint16_t auxiliary_carry;
    int version2;
} musyx_t;

typedef void (*mix_sfx_with_main_subframes_t)(musyx_t *musyx, const int16_t *subframe,
                                              const uint16_t* gains);

/* helper functions prototypes */
static void load_base_vol(struct hle_t* hle, int32_t *base_vol, uint32_t address);
static void save_base_vol(struct hle_t* hle, const int32_t *base_vol, uint32_t address);
static void update_base_vol(struct hle_t* hle, int32_t *base_vol,
                            uint32_t voice_mask, uint32_t last_sample_ptr,
                            uint8_t mask_15, uint32_t ptr_24);

static void init_subframes_v1(musyx_t *musyx);
static void init_subframes_v2(musyx_t *musyx);

static uint32_t voice_stage(struct hle_t* hle, musyx_t *musyx,
                            uint32_t voice_ptr, uint32_t last_sample_ptr);

static void dma_cat8(struct hle_t* hle, uint8_t *dst, uint32_t catsrc_ptr);
static void dma_cat16(struct hle_t* hle, uint16_t *dst, uint32_t catsrc_ptr);

static void load_samples_PCM16(struct hle_t* hle, uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset, int version2);
static void load_samples_ADPCM(struct hle_t* hle, uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset, int version2);

static void adpcm_decode_frames(struct hle_t* hle,
                                int16_t *dst, const uint8_t *src,
                                const int16_t *table, uint8_t count,
                                uint8_t skip_samples);

static void adpcm_predict_frame(int16_t *dst, const uint8_t *src,
                                const uint8_t *nibbles,
                                unsigned int rshift);

static void mix_voice_samples(struct hle_t* hle, musyx_t *musyx,
                              uint32_t voice_ptr, const int16_t *samples,
                              unsigned segbase, unsigned offset, uint32_t last_sample_ptr);

static void sfx_stage(struct hle_t* hle,
                      mix_sfx_with_main_subframes_t mix_sfx_with_main_subframes,
                      musyx_t *musyx, uint32_t sfx_ptr, uint16_t idx);

static void mix_sfx_with_main_subframes_v1(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains);
static void mix_sfx_with_main_subframes_v2(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains);

static void mix_samples(int16_t *y, int16_t x, int16_t hgain);
static void mix_subframes(int16_t *y, const int16_t *x, int16_t hgain);
static void mix_fir4(int16_t *y, const int16_t *x, int16_t hgain, const int16_t *hcoeffs);


static void interleave_stage_v1(struct hle_t* hle, musyx_t *musyx,
                                uint32_t output_ptr);

static void interleave_stage_v2(struct hle_t* hle, musyx_t *musyx,
                                uint16_t mask_16, uint32_t ptr_18,
                                uint32_t ptr_1c, uint32_t output_ptr);

/* set for the revision whose envelope update emits the e50 pair in the
 * opposite order to the other three destinations */
static int musyx_env_e50_nocarry = 0;

static int32_t dot4(const int16_t *x, const int16_t *y)
{
    size_t i;
    int32_t t[4];
    int32_t s01;
    int32_t s23;

    /* The MusyX resample FIR, as the microcode actually computes it
     * (disassembly of the Gauntlet Legends v1 task text, insns
     * 0x5f8..0x670): one VMULF per tap, then a saturating VADD reduction
     * tree (element 1q, then 2h). VMULF rounds each tap individually --
     * acc = 2*x*y + 0x8000, result clamp_s16(acc >> 16), equivalent to
     * clamp_s16((x*y + 0x4000) >> 15) -- and VADD saturates at every
     * add. Neither a per-tap truncating shift nor a full-precision
     * accumulate with one terminal shift reproduces this arithmetic;
     * both leave a small DC-biased residue against the LLE reference
     * on real game content. This form is bit-exact against cxd4 over
     * the Gauntlet Legends attract sequence. */
    for (i = 0; i < 4; ++i)
        t[i] = clamp_s16(((int32_t)x[i] * (int32_t)y[i] + 0x4000) >> 15);

    s01 = clamp_s16(t[0] + t[1]);
    s23 = clamp_s16(t[2] + t[3]);

    return clamp_s16(s01 + s23);
}

/**************************************************************************
 * MusyX v1 audio ucode
 **************************************************************************/
void musyx_v1_task(struct hle_t* hle)
{
    hle->musyx_ucode_data=*dmem_u32(hle,TASK_UCODE_DATA);
    /* Builds of this microcode differ in the order the envelope update
     * emits the e50 pair.  Where the carry-setting add lands on the high
     * half, that half never sees the carry out of its own low half and
     * the gain decays by the step's high half per iteration instead of
     * by the step.  Read the order out of the task's own text rather
     * than keeping a list of the games that have it: the two adds are
     * adjacent, and it is the second of the pair that says which. */
    {
        uint32_t text = *dmem_u32(hle, TASK_UCODE);
        uint32_t k;

        musyx_env_e50_nocarry = 0;
        for (k = 0; k + 8 <= 0x1000; k += 4) {
            uint32_t w0 = *dram_u32(hle, text + k);
            uint32_t w1 = *dram_u32(hle, text + k + 4);
            /* vadd vD,vD,v21[3] followed by vaddc vE,vE,v20[3] */
            if ((w0 >> 26) == 0x12 && ((w0 >> 25) & 1)
             && (w0 & 0x3f) == 0x10 && ((w0 >> 16) & 31) == 21
             && ((w0 >> 21) & 15) == 11
             && (w1 >> 26) == 0x12 && ((w1 >> 25) & 1)
             && (w1 & 0x3f) == 0x14 && ((w1 >> 16) & 31) == 20
             && ((w1 >> 21) & 15) == 11) {
                musyx_env_e50_nocarry = 1;
                break;
            }
        }
    }
    uint32_t sfd_ptr   = *dmem_u32(hle, TASK_DATA_PTR);
    uint32_t sfd_count = *dmem_u32(hle, TASK_DATA_SIZE);
    uint32_t state_ptr;
    musyx_t musyx;

    HleVerboseMessage(hle->user_defined,
                      "musyx_v1_task: *data=%x, #SF=%d",
                      sfd_ptr,
                      sfd_count);

    state_ptr = *dram_u32(hle, sfd_ptr + SFD_STATE_PTR) & ~7u;

    /* load initial state */
    load_base_vol(hle, musyx.base_vol, state_ptr + STATE_BASE_VOL);
    dram_load_u16(hle, (uint16_t *)musyx.cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
    dram_load_u16(hle, (uint16_t *)musyx.subframe_740_last4, state_ptr + STATE_740_LAST4_V1,
             4);

    for (;;) {
        /* parse SFD structure */
        hle_timing_add(HLE_WORK_COMMAND, 1); /* one subframe descriptor */
        uint16_t sfx_index   = *dram_u16(hle, sfd_ptr + SFD_SFX_INDEX);
        uint32_t voice_mask  = *dram_u32(hle, sfd_ptr + SFD_VOICE_BITMASK);
        uint32_t sfx_ptr     = *dram_u32(hle, sfd_ptr + SFD_SFX_PTR);
        uint32_t voice_ptr       = sfd_ptr + SFD_VOICES;
        uint32_t last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;
        uint32_t output_ptr;

        /* initialize internal subframes using updated base volumes */
        update_base_vol(hle, musyx.base_vol, voice_mask, last_sample_ptr, 0, 0);
        init_subframes_v1(&musyx);

        /* active voices get mixed into L,R,cc0,e50 subframes (optional) */
        output_ptr = voice_stage(hle, &musyx, voice_ptr, last_sample_ptr);
        output_ptr &= ~7u;

        /* apply delay-based effects (optional) */
        sfx_stage(hle, mix_sfx_with_main_subframes_v1,
                  &musyx, sfx_ptr, sfx_index);

        /* emit interleaved L,R subframes */
        interleave_stage_v1(hle, &musyx, output_ptr);

        --sfd_count;
        if (sfd_count == 0)
            break;

        sfd_ptr += SFD_VOICES + MAX_VOICES * VOICE_SIZE;
        state_ptr = *dram_u32(hle, sfd_ptr + SFD_STATE_PTR) & ~7u;
    }

    /* writeback updated state */
    save_base_vol(hle, musyx.base_vol, state_ptr + STATE_BASE_VOL);
    dram_store_u16(hle, (uint16_t *)musyx.cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
    dram_store_u16(hle, (uint16_t *)musyx.subframe_740_last4, state_ptr + STATE_740_LAST4_V1,
              4);

    hle->rsp_vco=0;hle->rsp_vco_written=1;
    hle->rsp_t6=0xffff8000u;hle->rsp_t6_written=1;
    rsp_break(hle, SP_STATUS_TASKDONE);
}

/**************************************************************************
 * MusyX v2 audio ucode
 **************************************************************************/
/* The first volume splat consumes incoming VCO. Its eight biased left
 * samples repeat through the entire 192-sample initialization; later channel
 * splats consume cleared flags. Carry is shared with the RSP task context. */
/* MusyX v2 overlay 6: four optional planar sources, each with a
 * four-tap filter and four unsigned Q0.16 destination gains. VMULF
 * rounds/clamps individual taps; VADDC folds wrap at sixteen bits.
 * The destination mix uses VMUDM/VMADM, with no fractional rounding. */
static void auxiliary_stage_v2(struct hle_t* hle, musyx_t *musyx,
                             uint32_t descriptors, unsigned mask, uint32_t state)
{
    uint8_t saved[128];
    unsigned slot, channel, sample, tap;
    load_u8(saved, hle->dmem, 0xce0, 32);
    dram_load_u8(hle, saved + 32, state + 32, 96);
    for (slot = 0; slot < 4; slot++)
    {
        int16_t source[3][SUBFRAME_SIZE + 4];
        int16_t filtered[3][SUBFRAME_SIZE];
        int16_t coefficients[4];
        uint16_t gains[4];
        uint8_t last[4][2] = {{0}};
        uint32_t descriptor, address;
        int filtering = 0;
        if (!(mask & (1u << slot))) continue;
        descriptor = descriptors + slot * 24;
        address = *dram_u32(hle, descriptor + 16) & ~7u;
        for (tap = 0; tap < 4; tap++)
        {
            gains[tap] = *dram_u16(hle, descriptor + tap * 2);
            coefficients[tap] = (int16_t)*dram_u16(hle, descriptor + 8 + tap * 2);
            filtering |= coefficients[tap];
        }
        for (channel = 0; channel < 3; channel++)
        {
            unsigned history = 32 + slot * 24 + channel * 8;
            for (tap = 0; tap < 4; tap++)
                source[channel][tap] = (int16_t)((saved[history + tap * 2] << 8) | saved[history + tap * 2 + 1]);
            dram_load_u16(hle, (uint16_t *)(source[channel] + 4),
                          address + channel * SUBFRAME_SIZE * 2, SUBFRAME_SIZE);
            /* These DMA planes share DMEM with the next voice's sample
             * window. PCM's four-tap lookahead can read their surviving
             * tail, even after both voice sample banks were replaced. */
            store_u16(hle->dmem,0x7a8u+channel*0x190u,
                      (uint16_t*)source[channel],SUBFRAME_SIZE+4);
            for (sample = 0; sample < SUBFRAME_SIZE; sample++)
            {
                if (filtering)
                {
                    uint32_t sum = 0;
                    for (tap = 0; tap < 4; tap++)
                    {
                        int64_t product = (int64_t)source[channel][sample + tap + 1] * coefficients[tap] * 2 + 0x8000;
                        sum += (uint16_t)clamp_s16((int32_t)(product >> 16));
                    }
                    filtered[channel][sample] = (int16_t)sum;
                }
                else filtered[channel][sample] = source[channel][sample + 4];
            }
            for (tap = 0; tap < 4; tap++)
            {
                /* LDV uses the FIR cursor minus eight; that cursor
                 * starts three samples before the planar input. */
                uint16_t value = (uint16_t)source[channel][SUBFRAME_SIZE - 3 + tap];
                saved[history + tap * 2] = value >> 8;
                saved[history + tap * 2 + 1] = value;
            }
        }
        if (filtering)
        {
            uint16_t product[8], pair[8];
            unsigned lane;
            musyx->auxiliary_carry = 0;
            for (lane = 0; lane < 8; lane++)
            {
                int64_t acc = (int64_t)source[2][SUBFRAME_SIZE - 4 + lane] * coefficients[lane & 3] * 2 + 0x8000;
                product[lane] = (uint16_t)clamp_s16((int32_t)(acc >> 16));
            }
            for (lane = 0; lane < 8; lane++) pair[lane] = product[lane] + product[(lane & ~1u) | 1u];
            for (lane = 0; lane < 8; lane++)
                if ((uint32_t)pair[lane] + pair[(lane & ~3u) | 2u] > 65535u)
                    musyx->auxiliary_carry |= 1u << lane;
        }
        hle_timing_add(HLE_WORK_FILTER, filtering ? 3u * SUBFRAME_SIZE / 8u : 0u);
        hle_timing_add(HLE_WORK_MIX, 4u * SUBFRAME_SIZE / 8u);
        {
            unsigned p;
            hle->rsp_f5[0]=(uint16_t)((int32_t)filtered[0][184]*gains[0]>>16)|
                ((uint32_t)(uint16_t)((int32_t)filtered[0][185]*gains[0]>>16)<<16);
            for(p=0;p<2;p++){
                hle->rsp_f5[5+p]=(uint16_t)((int32_t)filtered[1][188+p*2]*gains[1]>>16)|
                    ((uint32_t)(uint16_t)((int32_t)filtered[1][189+p*2]*gains[1]>>16)<<16);
            }
            hle->rsp_f5_written|=97u;
        }
        for (sample = 0; sample < SUBFRAME_SIZE; sample++)
        {
            int16_t *destinations[3] = {musyx->left, musyx->right, musyx->cc0};
            int64_t e50 = 0;
            for (channel = 0; channel < 3; channel++)
            {
                int64_t acc = (int64_t)filtered[channel][sample] * gains[channel];
                if (sample == SUBFRAME_SIZE - 5)
                    last[channel][0] = (uint8_t)clamp_s16((int32_t)(acc >> 16));
                if (sample == SUBFRAME_SIZE - 4)
                    last[channel][1] = (uint16_t)clamp_s16((int32_t)(acc >> 16)) >> 8;
                acc += (int64_t)destinations[channel][sample] * 65535;
                destinations[channel][sample] = clamp_s16((int32_t)(acc >> 16));
                e50 += (int64_t)filtered[channel][sample] * gains[3];
            }
            if (sample == SUBFRAME_SIZE - 5) last[3][0] = (uint8_t)clamp_s16((int32_t)(e50 >> 16));
            if (sample == SUBFRAME_SIZE - 4) last[3][1] = (uint16_t)clamp_s16((int32_t)(e50 >> 16)) >> 8;
            e50 += (int64_t)musyx->e50[sample] * 65535;
            musyx->e50[sample] = clamp_s16((int32_t)(e50 >> 16));
        }
        for (channel = 0; channel < 4; channel++)
        {
            /* SSV element 7 crosses bytes of lanes 3 and 4. */
            saved[slot * 8 + channel * 2] = last[channel][0];
            saved[slot * 8 + channel * 2 + 1] = last[channel][1];
        }
        for(sample=0;sample<2;sample++)
            hle->rsp_f5[3+sample]=(uint16_t)musyx->e50[188+sample*2]|
                ((uint32_t)(uint16_t)musyx->e50[189+sample*2]<<16);
        hle->rsp_f5_written|=24u;
    }
    dram_store_u8(hle, saved, state, sizeof(saved));
    store_u8(hle->dmem, 0xce0, saved, sizeof(saved));
}

void musyx_v2_task(struct hle_t* hle)
{
    hle->musyx_ucode_data=*dmem_u32(hle,TASK_UCODE_DATA);
    musyx_env_e50_nocarry = 0;
    uint32_t sfd_ptr   = *dmem_u32(hle, TASK_DATA_PTR);
    uint32_t sfd_count = *dmem_u32(hle, TASK_DATA_SIZE);
    musyx_t musyx;

    HleVerboseMessage(hle->user_defined,
                      "musyx_v2_task: *data=%x, #SF=%d",
                      sfd_ptr,
                      sfd_count);

    for (;;) {
        /* parse SFD structure */
        hle_timing_add(HLE_WORK_COMMAND, 1);
        uint16_t sfx_index       = *dram_u16(hle, sfd_ptr + SFD_SFX_INDEX);
        uint32_t voice_mask      = *dram_u32(hle, sfd_ptr + SFD_VOICE_BITMASK);
        uint32_t state_ptr       = *dram_u32(hle, sfd_ptr + SFD_STATE_PTR) & ~7u;
        uint32_t sfx_ptr         = *dram_u32(hle, sfd_ptr + SFD_SFX_PTR);
        uint32_t voice_ptr       = sfd_ptr + SFD2_VOICES;

        uint32_t ptr_10          = *dram_u32(hle, sfd_ptr + SFD2_10_PTR);
        uint8_t  mask_14         = *dram_u8 (hle, sfd_ptr + SFD2_14_BITMASK);
        uint8_t  mask_15         = *dram_u8 (hle, sfd_ptr + SFD2_15_BITMASK);
        uint16_t mask_16         = *dram_u16(hle, sfd_ptr + SFD2_16_BITMASK);
        uint32_t ptr_18          = *dram_u32(hle, sfd_ptr + SFD2_18_PTR) & ~7u;
        uint32_t ptr_1c          = *dram_u32(hle, sfd_ptr + SFD2_1C_PTR) & ~7u;
        uint32_t ptr_20          = *dram_u32(hle, sfd_ptr + SFD2_20_PTR) & ~7u;
        uint32_t ptr_24          = *dram_u32(hle, sfd_ptr + SFD2_24_PTR) & ~7u;

        uint32_t last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;
        uint32_t output_ptr;

        /* load state */
        load_base_vol(hle, musyx.base_vol, state_ptr + STATE_BASE_VOL);
        dram_load_u16(hle, (uint16_t *)musyx.subframe_740_last4,
                state_ptr + STATE_740_LAST4_V2, 4);

        /* initialize internal subframes using updated base volumes */
        update_base_vol(hle, musyx.base_vol, voice_mask, last_sample_ptr, mask_15, ptr_24);
        init_subframes_v2(&musyx);
        hle->rsp_t6=0x4a0u;hle->rsp_t6_written=1;
        {
            unsigned sample;
            for(sample=0;sample<SUBFRAME_SIZE;sample++)
                musyx.left[sample]=clamp_s16((int32_t)musyx.left[sample]+((hle->rsp_vco>>(sample&7u))&1u));
            hle->rsp_vco=0;
        }

        if (ptr_10) {
            auxiliary_stage_v2(hle, &musyx, ptr_10 & ~7u, mask_14, ptr_24);
            if(mask_14&15u)hle->rsp_t6=0x620u;
        }

        /* active voices get mixed into L,R,cc0,e50 subframes (optional) */
        output_ptr = voice_stage(hle, &musyx, voice_ptr, last_sample_ptr);
        output_ptr &= ~7u;

        /* apply delay-based effects (optional) */
        sfx_stage(hle, mix_sfx_with_main_subframes_v2,
                  &musyx, sfx_ptr, sfx_index);
        if(sfx_ptr)musyx.auxiliary_carry=0;

        dram_store_u16(hle, (uint16_t*)musyx.left,  output_ptr                  , SUBFRAME_SIZE);
        dram_store_u16(hle, (uint16_t*)musyx.right, output_ptr + 2*SUBFRAME_SIZE, SUBFRAME_SIZE);
        dram_store_u16(hle, (uint16_t*)musyx.cc0,   output_ptr + 4*SUBFRAME_SIZE, SUBFRAME_SIZE);

        /* store state */
        save_base_vol(hle, musyx.base_vol, state_ptr + STATE_BASE_VOL);
        dram_store_u16(hle, (uint16_t*)musyx.subframe_740_last4,
                state_ptr + STATE_740_LAST4_V2, 4);

        if (mask_16) {
            interleave_stage_v2(hle, &musyx, mask_16, ptr_18, ptr_1c, ptr_20);
            musyx.auxiliary_carry=0;
            hle->rsp_t6=0xae0u;
        }
        hle->rsp_vco=musyx.auxiliary_carry;
        hle->rsp_vco_written=1;

        --sfd_count;
        if (sfd_count == 0)
            break;

        sfd_ptr += SFD2_VOICES + MAX_VOICES * VOICE_SIZE;
    }

    rsp_break(hle, SP_STATUS_TASKDONE);
}





static void load_base_vol(struct hle_t* hle, int32_t *base_vol, uint32_t address)
{
    base_vol[0] = ((uint32_t)(*dram_u16(hle, address))     << 16) | (*dram_u16(hle, address +  8));
    base_vol[1] = ((uint32_t)(*dram_u16(hle, address + 2)) << 16) | (*dram_u16(hle, address + 10));
    base_vol[2] = ((uint32_t)(*dram_u16(hle, address + 4)) << 16) | (*dram_u16(hle, address + 12));
    base_vol[3] = ((uint32_t)(*dram_u16(hle, address + 6)) << 16) | (*dram_u16(hle, address + 14));
}

static void save_base_vol(struct hle_t* hle, const int32_t *base_vol, uint32_t address)
{
    unsigned k;

    for (k = 0; k < 4; ++k) {
        *dram_u16(hle, address) = (uint16_t)(base_vol[k] >> 16);
        address += 2;
    }

    for (k = 0; k < 4; ++k) {
        *dram_u16(hle, address) = (uint16_t)(base_vol[k]);
        address += 2;
    }
}

static void update_base_vol(struct hle_t* hle, int32_t *base_vol,
                            uint32_t voice_mask, uint32_t last_sample_ptr,
                            uint8_t mask_15, uint32_t ptr_24)
{
    unsigned i, k;
    uint32_t mask;

    HleVerboseMessage(hle->user_defined, "base_vol voice_mask = %08x", voice_mask);
    HleVerboseMessage(hle->user_defined,
                      "BEFORE: base_vol = %08x %08x %08x %08x",
                      base_vol[0], base_vol[1], base_vol[2], base_vol[3]);

    /* optim: skip voices contributions entirely if voice_mask is empty */
    if (voice_mask != 0) {
        for (i = 0, mask = 1; i < MAX_VOICES;
             ++i, mask <<= 1, last_sample_ptr += 8) {
            if ((voice_mask & mask) == 0)
                continue;

            for (k = 0; k < 4; ++k)
                base_vol[k] = (int32_t)((uint32_t)base_vol[k] +
                    (int16_t)*dram_u16(hle, last_sample_ptr + k * 2));
        }
    }

    /* optim: skip contributions entirely if mask_15 is empty */
    if (mask_15 != 0) {
        for(i = 0, mask = 1; i < 4;
                ++i, mask <<= 1, ptr_24 += 8) {
            if ((mask_15 & mask) == 0)
                continue;

            for(k = 0; k < 4; ++k)
                base_vol[k] = (int32_t)((uint32_t)base_vol[k] +
                    (int16_t)*dram_u16(hle, ptr_24 + k * 2));
        }
    }

    /* apply 3% decay */
    for (k = 0; k < 4; ++k)
        base_vol[k] = (int32_t)(((int64_t)base_vol[k] * 0xf850) >> 16);

    HleVerboseMessage(hle->user_defined,
                      "AFTER: base_vol = %08x %08x %08x %08x",
                      base_vol[0], base_vol[1], base_vol[2], base_vol[3]);
}




static void init_subframes_v1(musyx_t *musyx)
{
    musyx->version2 = 0;
    musyx->auxiliary_carry = 0;
    unsigned i;

    int16_t base_cc0 = clamp_s16(musyx->base_vol[2]);
    int16_t base_e50 = clamp_s16(musyx->base_vol[3]);

    int16_t *left  = musyx->left;
    int16_t *right = musyx->right;
    int16_t *cc0   = musyx->cc0;
    int16_t *e50   = musyx->e50;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        *(e50++)    = base_e50;
        *(left++)   = clamp_s16(*cc0 + base_cc0);
        *(right++)  = clamp_s16(-*cc0 - base_cc0);
        *(cc0++)    = 0;
    }
}

static void init_subframes_v2(musyx_t *musyx)
{
    unsigned i,k;
    int16_t values[4];
    int16_t* subframes[4];
    musyx->auxiliary_carry = 0;
    musyx->version2 = 1;

    for(k = 0; k < 4; ++k)
        values[k] = clamp_s16(musyx->base_vol[k]);

    /* The v2 prologue writes its fourth clamp to a1, then moves the
     * original t5 into v5[6]. Preserve that sixteen-bit truncation. */
    values[3] = (int16_t)musyx->base_vol[3];

    subframes[0] = musyx->left;
    subframes[1] = musyx->right;
    subframes[2] = musyx->cc0;
    subframes[3] = musyx->e50;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {

        for(k = 0; k < 4; ++k)
            *(subframes[k]++) = values[k];
    }
}

/* Process voices, and returns interleaved subframe destination address */
static uint32_t voice_stage(struct hle_t* hle, musyx_t *musyx,
                            uint32_t voice_ptr, uint32_t last_sample_ptr)
{
    uint32_t output_ptr;
    int i = 0;

    /* voice stage can be skipped if first voice has no samples */
    if (*dram_u16(hle, voice_ptr + VOICE_CATSRC_0 + CATSRC_SIZE1) == 0) {
        HleVerboseMessage(hle->user_defined, "Skipping Voice stage");
        output_ptr = *dram_u32(hle, voice_ptr + VOICE_INTERLEAVED_PTR);
    } else {
        /* otherwise process voices until a non null output_ptr is encountered */
        for (;;) {
            /* load voice samples (PCM16 or APDCM) */
            int16_t samples[SAMPLE_BUFFER_SIZE+4];
            unsigned segbase;
            unsigned offset;

            HleVerboseMessage(hle->user_defined, "Processing Voice #%d", i);

            if (*dram_u8(hle, voice_ptr + VOICE_ADPCM_FRAMES) == 0)
                load_samples_PCM16(hle, voice_ptr, samples, &segbase, &offset, musyx->version2);
            else
                load_samples_ADPCM(hle, voice_ptr, samples, &segbase, &offset, musyx->version2);

            /* mix them with each internal subframes */
            mix_voice_samples(hle, musyx, voice_ptr, samples, segbase, offset,
                              last_sample_ptr + i * 8);
            if (musyx->version2)
            {
                uint16_t last[4];
                load_u16(last, hle->dram, (last_sample_ptr + i * 8) & 0xffffff, 4);
                store_u16(hle->dmem, 0xce0 + (i & 15u) * 8u, last, 4);
            }

            /* check break condition */
            output_ptr = *dram_u32(hle, voice_ptr + VOICE_INTERLEAVED_PTR);
            if (output_ptr != 0)
                break;

            /* next voice */
            ++i;
            voice_ptr += VOICE_SIZE;
        }
    }

    return output_ptr;
}

static void dma_cat8(struct hle_t* hle, uint8_t *dst, uint32_t catsrc_ptr)
{
    uint32_t ptr1  = *dram_u32(hle, catsrc_ptr + CATSRC_PTR1);
    uint32_t ptr2  = *dram_u32(hle, catsrc_ptr + CATSRC_PTR2);
    uint16_t size1 = *dram_u16(hle, catsrc_ptr + CATSRC_SIZE1);
    uint16_t size2 = *dram_u16(hle, catsrc_ptr + CATSRC_SIZE2);

    size_t count1 = align(size1, 8);
    size_t count2 = align(size2, 8);

    HleVerboseMessage(hle->user_defined,
                      "dma_cat: %08x %08x %04x %04x",
                      ptr1,
                      ptr2,
                      size1,
                      size2);

    dram_load_u8(hle, dst, ptr1 & ~7u, count1);

    if (size2 == 0)
        return;

    dram_load_u8(hle, dst + (size1 & ~7u), ptr2 & ~7u, count2);
}

static void dma_cat16(struct hle_t* hle, uint16_t *dst, uint32_t catsrc_ptr)
{
    uint32_t ptr1  = *dram_u32(hle, catsrc_ptr + CATSRC_PTR1);
    uint32_t ptr2  = *dram_u32(hle, catsrc_ptr + CATSRC_PTR2);
    uint16_t size1 = *dram_u16(hle, catsrc_ptr + CATSRC_SIZE1);
    uint16_t size2 = *dram_u16(hle, catsrc_ptr + CATSRC_SIZE2);

    size_t count1 = align(size1, 8) >> 1;
    size_t count2 = align(size2, 8) >> 1;

    HleVerboseMessage(hle->user_defined,
                      "dma_cat: %08x %08x %04x %04x",
                      ptr1,
                      ptr2,
                      size1,
                      size2);

    dram_load_u16(hle, dst, ptr1 & ~7u, count1);

    if (size2 == 0)
        return;

    dram_load_u16(hle, dst + ((size1 & ~7u) >> 1), ptr2 & ~7u, count2);
}

static void load_samples_PCM16(struct hle_t* hle, uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset, int version2)
{

    uint8_t  u8_3e  = *dram_u8(hle, voice_ptr + VOICE_SKIP_SAMPLES);
    uint16_t u16_40 = *dram_u16(hle, voice_ptr + VOICE_U16_40);
    uint16_t u16_42 = *dram_u16(hle, voice_ptr + VOICE_U16_42);

    unsigned count = align(u16_40 + u8_3e, 4);
    unsigned base=version2?0x7a0u:0x600u;
    load_u16((uint16_t*)samples,hle->dmem,base,SAMPLE_BUFFER_SIZE+4);

    HleVerboseMessage(hle->user_defined, "Format: PCM16");

    *segbase = SAMPLE_BUFFER_SIZE - count;
    *offset  = u8_3e;

    dma_cat16(hle, (uint16_t *)samples + *segbase, voice_ptr + VOICE_CATSRC_0);

    if (u16_42 != 0)
        dma_cat16(hle, (uint16_t *)samples, voice_ptr + VOICE_CATSRC_1);
    store_u16(hle->dmem,base,(uint16_t*)samples,SAMPLE_BUFFER_SIZE);
}

static void load_samples_ADPCM(struct hle_t* hle, uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset, int version2)
{
    /* decompressed samples cannot exceed 0x400 bytes;
     * ADPCM has a compression ratio of 5/16 */
    uint8_t buffer[SAMPLE_BUFFER_SIZE * 2 * 5 / 16];
    int16_t adpcm_table[256];

    uint8_t u8_3c = *dram_u8(hle, voice_ptr + VOICE_ADPCM_FRAMES    );
    uint8_t u8_3d = *dram_u8(hle, voice_ptr + VOICE_ADPCM_FRAMES + 1);
    uint8_t u8_3e = *dram_u8(hle, voice_ptr + VOICE_SKIP_SAMPLES    );
    uint8_t u8_3f = *dram_u8(hle, voice_ptr + VOICE_SKIP_SAMPLES + 1);
    uint32_t adpcm_table_ptr = *dram_u32(hle, voice_ptr + VOICE_ADPCM_TABLE_PTR);
    unsigned count;
    unsigned sample_base=version2?0x7a0u:0x600u;
    unsigned encoded_base=sample_base+SAMPLE_BUFFER_SIZE*2u;
    unsigned table_base=version2?0xf00u:0xee0u,k;
    load_u16((uint16_t*)samples,hle->dmem,sample_base,SAMPLE_BUFFER_SIZE+4);
    load_u8(buffer,hle->dmem,encoded_base,sizeof(buffer));

    HleVerboseMessage(hle->user_defined, "Format: ADPCM");

    HleVerboseMessage(hle->user_defined, "Loading ADPCM table: %08x", adpcm_table_ptr);
    dram_load_u16(hle, (uint16_t *)adpcm_table, adpcm_table_ptr & ~7u, 128);
    /* Predictor addressing wraps in DMEM. V2's books start at f00;
     * v1 starts at ee0, so its first extra book also reads the OSTask. */
    for(k=128;k<256;k++){
        unsigned address=table_base+k*2u;
        adpcm_table[k]=(int16_t)(address<4096?*dmem_u16(hle,address):
            *dram_u16(hle,hle->musyx_ucode_data+(address&4095u)));
    }
    store_u16(hle->dmem,table_base,(uint16_t*)adpcm_table,128);

    count = u8_3c << 5;

    *segbase = SAMPLE_BUFFER_SIZE - count;
    *offset  = u8_3e & 0x1f;

    dma_cat8(hle, buffer, voice_ptr + VOICE_CATSRC_0);
    adpcm_decode_frames(hle, samples + *segbase, buffer, adpcm_table, u8_3c, u8_3e);

    if (u8_3d != 0) {
        dma_cat8(hle, buffer, voice_ptr + VOICE_CATSRC_1);
        adpcm_decode_frames(hle, samples, buffer, adpcm_table, u8_3d, u8_3f);
    }
    /* Resampling may read its four-tap lookahead past the decoded window.
     * Those samples are the adjacent encoded DMA bytes, not host stack. */
    store_u8(hle->dmem,encoded_base,buffer,sizeof(buffer));
    load_u16((uint16_t*)samples+SAMPLE_BUFFER_SIZE,hle->dmem,encoded_base,4);
    store_u16(hle->dmem,sample_base,(uint16_t*)samples,SAMPLE_BUFFER_SIZE);
}

static void adpcm_decode_frames(struct hle_t* hle,
                                int16_t *dst, const uint8_t *src,
                                const int16_t *table, uint8_t count,
                                uint8_t skip_samples)
{
    hle_timing_add(HLE_WORK_ADPCM, count);

    int16_t frame[32];
    const uint8_t *nibbles = src + 8;
    unsigned i;
    bool jump_gap = false;

    HleVerboseMessage(hle->user_defined,
                      "ADPCM decode: count=%d, skip=%d",
                      count, skip_samples);

    if (skip_samples >= 32) {
        jump_gap = true;
        nibbles += 16;
        src += 4;
    }

    for (i = 0; i < count; ++i) {
        uint8_t c2 = nibbles[0];

        const int16_t *book = (c2 & 0xf0) + table;
        unsigned int rshift = (c2 & 0x0f);

        adpcm_predict_frame(frame, src, nibbles, rshift);

        memcpy(dst, frame, 2 * sizeof(frame[0]));
        adpcm_compute_residuals(dst +  2, frame +  2, book, dst     , 6);
        adpcm_compute_residuals(dst +  8, frame +  8, book, dst +  6, 8);
        adpcm_compute_residuals(dst + 16, frame + 16, book, dst + 14, 8);
        adpcm_compute_residuals(dst + 24, frame + 24, book, dst + 22, 8);

        if (jump_gap) {
            nibbles += 8;
            src += 32;
        }

        jump_gap = !jump_gap;
        nibbles += 16;
        src += 4;
        dst += 32;
    }
}

static void adpcm_predict_frame(int16_t *dst, const uint8_t *src,
                                const uint8_t *nibbles,
                                unsigned int rshift)
{
    unsigned int i;

    *(dst++) = (src[0] << 8) | src[1];
    *(dst++) = (src[2] << 8) | src[3];

    for (i = 1; i < 16; ++i) {
        uint8_t byte = nibbles[i];

        *(dst++) = adpcm_predict_sample(byte, 0xf0,  8, rshift);
        *(dst++) = adpcm_predict_sample(byte, 0x0f, 12, rshift);
    }
}

static void mix_voice_samples(struct hle_t* hle, musyx_t *musyx,
                              uint32_t voice_ptr, const int16_t *samples,
                              unsigned segbase, unsigned offset, uint32_t last_sample_ptr)
{
    hle_timing_add(HLE_WORK_VOICE, 1);
    hle_timing_add(HLE_WORK_RESAMPLE, SUBFRAME_SIZE / 8);
    hle_timing_add(HLE_WORK_ENVMIX, SUBFRAME_SIZE / 8);
    unsigned coefficient_bias = musyx->auxiliary_carry;
    int16_t biased_lut[8][4];
    musyx->auxiliary_carry = 0;

    int i, k;

    /* parse VOICE structure */
    const uint16_t pitch_q16   = *dram_u16(hle, voice_ptr + VOICE_PITCH_Q16);
    const uint16_t pitch_shift = *dram_u16(hle, voice_ptr + VOICE_PITCH_SHIFT); /* Q4.12 */

    const uint16_t end_point     = *dram_u16(hle, voice_ptr + VOICE_END_POINT);
    const uint16_t restart_point = *dram_u16(hle, voice_ptr + VOICE_RESTART_POINT);

    const uint16_t u16_4e = *dram_u16(hle, voice_ptr + VOICE_U16_4E);

    /* init values and pointers */
    const int16_t       *sample         = samples + segbase + offset + u16_4e;
    const int16_t *const sample_end     = samples + segbase + end_point;
    const int16_t *const sample_restart = samples + (restart_point & 0x7fff) +
                                          (((restart_point & 0x8000) != 0) ? 0x000 : segbase);


    uint32_t pitch_accu = pitch_q16;
    uint32_t pitch_step = pitch_shift << 4;

    int32_t  v4_env[4];
    int16_t  e50_hi[8], e50_step_hi;
    int32_t  v4_env_step[4];
    int16_t *v4_dst[4];
    int16_t  v4[4];

    dram_load_u32(hle, (uint32_t *)v4_env,      voice_ptr + VOICE_ENV_BEGIN, 4);
    dram_load_u32(hle, (uint32_t *)v4_env_step, voice_ptr + VOICE_ENV_STEP,  4);
    {
        uint16_t left[2],right[4];
        unsigned lane,iteration;
        for(lane=0;lane<6;lane++){
            unsigned channel=lane<2?0:1;
            unsigned element=lane<2?lane:lane-2+4;
            uint32_t step=(uint32_t)v4_env_step[channel],begin=(uint32_t)v4_env[channel],advance=step*8u;
            uint32_t low=(uint16_t)step*element,carry;
            int high=clamp_s16((int32_t)(low>>16)+(int16_t)(step>>16)*(int)element);
            low=(uint16_t)low+(uint16_t)begin;
            high=clamp_s16(high+(int16_t)(begin>>16)+(int)(low>>16));low&=0xffffu;
            for(iteration=0;iteration<24;iteration++){
                carry=low+(uint16_t)advance;low=carry&0xffffu;
                high=clamp_s16(high+(int16_t)(advance>>16)+(int)(carry>>16));
            }
            if(lane<2)left[lane]=(uint16_t)high;
            else right[lane-2]=(uint16_t)high;
        }
        hle->rsp_f5[0]=left[0]|((uint32_t)left[1]<<16);
        hle->rsp_f5[3]=0;hle->rsp_f5[4]=1u<<16;
        for(lane=0;lane<2;lane++){
            hle->rsp_f5[1+lane]=(uint16_t)(pitch_q16+(196u+lane*2u)*pitch_step)|
                ((uint32_t)(uint16_t)(pitch_q16+(197u+lane*2u)*pitch_step)<<16);
            hle->rsp_f5[5+lane]=right[lane*2]|((uint32_t)right[lane*2+1]<<16);
        }
        hle->rsp_f5_written|=127u;
    }

    {
        unsigned j;
        e50_step_hi = (int16_t)((v4_env_step[3] << 3) >> 16);
        for (j = 0; j < 8; ++j)
            e50_hi[j] = (int16_t)((v4_env[3] + (int32_t)j * v4_env_step[3]) >> 16);
    }

    v4_dst[0] = musyx->left;
    v4_dst[1] = musyx->right;
    v4_dst[2] = musyx->cc0;
    v4_dst[3] = musyx->e50;

    HleVerboseMessage(hle->user_defined,
                      "Voice debug: segbase=%d"
                      "\tu16_4e=%04x\n"
                      "\tpitch: frac0=%04x shift=%04x\n"
                      "\tend_point=%04x restart_point=%04x\n"
                      "\tenv      = %08x %08x %08x %08x\n"
                      "\tenv_step = %08x %08x %08x %08x\n",
                      segbase,
                      u16_4e,
                      pitch_q16, pitch_shift,
                      end_point, restart_point,
                      v4_env[0],      v4_env[1],      v4_env[2],      v4_env[3],
                      v4_env_step[0], v4_env_step[1], v4_env_step[2], v4_env_step[3]);

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        /* update sample and lut pointers and then pitch_accu */
        const int16_t *lut = (RESAMPLE_LUT + ((pitch_accu & 0xfc00) >> 8));
        if (coefficient_bias)
        {
            if (!(i & 7))
            {
                const int16_t *cursor = sample;
                uint32_t phase = pitch_accu;
                uint16_t sample_address[8], coefficient_address[8];
                unsigned lane, tap;
                uint32_t data = hle->musyx_ucode_data;
                for (lane = 0; lane < 8; lane++)
                {
                    int distance;
                    cursor += phase >> 16;
                    phase &= 0xffff;
                    distance = cursor - sample_end;
                    if (distance >= 0) cursor = sample_restart + distance;
                    sample_address[lane] = (uint16_t)(0x7a0 + (cursor - samples) * 2);
                    coefficient_address[lane] = (uint16_t)(0x20 + ((phase & 0xfc00) >> 7) +
                                                    (((coefficient_bias >> lane) & 1u) * 32u));
                    phase += pitch_step;
                }
                for (lane = 0; lane < 8; lane++)
                    for (tap = 0; tap < 4; tap++)
                    {
                        unsigned address = coefficient_address[lane] + tap * 2;
                        if (address < 0x220) biased_lut[lane][tap] = RESAMPLE_LUT[(address - 0x20) / 2];
                        else if (address < 0x230) biased_lut[lane][tap] = (int16_t)*dram_u16(hle, data + address);
                        else biased_lut[lane][tap] = (int16_t)sample_address[(address - 0x230) / 2];
                    }
            }
            /* VADD seeds v28 with one plus the auxiliary carry; v28
             * remains in every phase lookup for this voice. */
            lut = biased_lut[i & 7];
        }
        /* The microcode reads its phase here, before the accumulator
         * advances for the next output; the wider kernel has to use the
         * same one.  Taking it after the advance is a whole step out,
         * which at half-rate playback is half a sample. */
        const uint32_t phase_accu = pitch_accu;
        int dist;
        int16_t v;

        sample += (pitch_accu >> 16);
        pitch_accu &= 0xffff;
        pitch_accu += pitch_step;

        /* handle end/restart points */
        dist = sample - sample_end;
        if (dist >= 0)
            sample = sample_restart + dist;

        /* apply resample filter */
        v = 0;
        {
            /* Opt-in wider kernel (see resample_hq.c).  The voice's sample
             * pointer carries loop and restart semantics and the buffer is
             * a fixed 0x200 window of the stream, so a wide window is only
             * taken where it lies clear of both: within a window's width of
             * the loop point it would splice in material from the far side
             * of the loop, and past the buffer it would read whatever
             * follows.  A boundary is crossed once per waveform period, so
             * the microcode's own four taps cover a small fraction of the
             * samples and the rest get the sinc. */
            /* A wide window cannot be centred near the start of the
             * decoded chunk, and the wider it is the more of each
             * subframe that covers: at 64 taps better than a quarter of
             * Indiana Jones' samples ended up on the microcode's kernel,
             * so the output alternated between two filters with
             * different responses.  Cap what this path asks for - a
             * consistent 32-tap result beats a mixture. */
            const int16_t* k = coefficient_bias ? NULL : resample_hq_taps_capped(pitch_step, phase_accu, 32);
            int            done = 0;

            if (k != NULL) {
                const int   taps  = resample_hq_taps_count();
                const int   lead  = taps / 2 - 2;
                const int16_t* lo = sample - lead;

                /* Taps that run past the end of the voice follow it round
                 * to the restart point, which is where the stream really
                 * does continue - the same jump the play position itself
                 * makes a moment later.  Bailing out instead handed those
                 * samples to the microcode's four taps, and the wider the
                 * kernel the more of them there were: at 64 taps that was
                 * a third of Indiana Jones' samples, so the output kept
                 * switching between two filters with different responses.
                 *
                 * Reading backwards is not the same question.  The decoder puts
                 * the voice's samples in the tail of the buffer - segbase is
                 * SAMPLE_BUFFER_SIZE minus the count it decoded - so anything
                 * below segbase belongs to whatever was there before, and the
                 * play position starts only a few samples above it.  A window
                 * reaching back past segbase would mix in unrelated audio, so
                 * it falls back instead. */
                if (lo >= samples + segbase) {
                    int64_t acc = 0;
                    int     t;

                    for (t = 0; t < taps; ++t) {
                        const int16_t* p = lo + t;

                        if (p >= sample_end) {
                            p = sample_restart + (p - sample_end);
                            if (p >= sample_end)      /* loop shorter than the window */
                                break;
                        }
                        if (p < samples + segbase || p >= samples + SAMPLE_BUFFER_SIZE)
                            break;

                        acc += (int64_t)*p * k[t];
                    }

                    if (t == taps) {
                        v = clamp_s16((int32_t)((acc + 0x4000) >> 15));
                        done = 1;
                    }
                }
            }

            if (!done)
                v = clamp_s16(dot4(sample, lut));
        }

        for (k = 0; k < 4; ++k) {
            /* envmix: VMULF by the envelope, so the product is rounded
             * (+0x4000 before the shift), not truncated; the microcode
             * then folds the destination into the same accumulator with
             * VMADH and saturates once on readout. */
            int32_t accu = (k == 3 && musyx_env_e50_nocarry)
                ? ((v * (int32_t)e50_hi[i & 7] + 0x4000) >> 15)
                : ((v * (v4_env[k] >> 16) + 0x4000) >> 15);
            v4[k] = clamp_s16(accu);
            *(v4_dst[k]) = clamp_s16(accu + *(v4_dst[k]) *
                (1 + (int)((coefficient_bias >> (i & 7)) & 1u)));

            /* update envelopes and dst pointers */
            ++(v4_dst[k]);
            v4_env[k] += v4_env_step[k];
        }

        /* The e50 envelope is a ramp across the eight lanes of a vector
         * pair - lane j holds env + j*step, split into a high and a low
         * half - and the mix loop consumes eight samples per iteration,
         * taking each sample's gain from its own lane.  The step is
         * scaled by eight so one add per iteration advances every lane.
         * The pair is emitted in the opposite order to the other three
         * destinations, so the high half is added with the
         * carry-setting instruction and never sees the carry out of its
         * own low half: the gain falls by the step's high half each
         * iteration rather than by the step itself. */
        if (musyx_env_e50_nocarry && (i & 7) == 7) {
            unsigned j;
            for (j = 0; j < 8; ++j) {
                uint32_t t = (uint32_t)(uint16_t)e50_hi[j] + (uint16_t)e50_step_hi;
                e50_hi[j] = (int16_t)t;
            }
        }
    }

    /* save last resampled sample */
    if(musyx->version2){
        int cursor=(int)(sample-samples);
        uint32_t phase=pitch_accu;
        unsigned lane;
        /* The eight-lane loop stages the next batch's source addresses
         * even on its final pass; t6 receives lane five of that batch. */
        for(lane=0;lane<6;lane++){
            int distance;
            cursor+=(int)(phase>>16);phase=(phase&0xffffu)+pitch_step;
            distance=cursor-(int)(sample_end-samples);
            if(distance>=0)cursor=(int)(sample_restart-samples)+distance;
        }
        hle->rsp_t6=(0x7a0u+(unsigned int)cursor*2u)&0xffffu;hle->rsp_t6_written=1;
    }
    dram_store_u16(hle, (uint16_t *)v4, last_sample_ptr, 4);

    HleVerboseMessage(hle->user_defined,
                      "last_sample = %04x %04x %04x %04x",
                      v4[0], v4[1], v4[2], v4[3]);
}


static void sfx_stage(struct hle_t* hle, mix_sfx_with_main_subframes_t mix_sfx_with_main_subframes,
                      musyx_t *musyx, uint32_t sfx_ptr, uint16_t idx)
{
    unsigned int i;

    int16_t buffer[SUBFRAME_SIZE + 4];
    int16_t *subframe = buffer + 4;

    uint32_t tap_delays[8];
    int16_t tap_gains[8];
    int16_t fir4_hcoeffs[4];

    int16_t delayed[SUBFRAME_SIZE+4];
    int dpos, dlength;

    const uint32_t pos = idx * SUBFRAME_SIZE;

    uint32_t cbuffer_ptr;
    uint32_t cbuffer_length;
    uint16_t tap_count;
    int16_t fir4_hgain;
    uint16_t sfx_gains[2];

    HleVerboseMessage(hle->user_defined, "SFX: %08x, idx=%d", sfx_ptr, idx);

    if (sfx_ptr == 0)
        return;
    sfx_ptr &= ~7u;

    /* load sfx  parameters */
    cbuffer_ptr    = *dram_u32(hle, sfx_ptr + SFX_CBUFFER_PTR);
    if(musyx->version2){hle->rsp_t6=cbuffer_ptr;hle->rsp_t6_written=1;}
    cbuffer_length = *dram_u32(hle, sfx_ptr + SFX_CBUFFER_LENGTH);

    tap_count      = *dram_u16(hle, sfx_ptr + SFX_TAP_COUNT);
    hle_timing_add(HLE_WORK_EFFECT, tap_count);
    hle_timing_add(HLE_WORK_FILTER, SUBFRAME_SIZE / 8);

    dram_load_u32(hle, tap_delays, sfx_ptr + SFX_TAP_DELAYS, 8);
    dram_load_u16(hle, (uint16_t *)tap_gains,  sfx_ptr + SFX_TAP_GAINS,  8);

    fir4_hgain     = *dram_u16(hle, sfx_ptr + SFX_FIR4_HGAIN);
    dram_load_u16(hle, (uint16_t *)fir4_hcoeffs, sfx_ptr + SFX_FIR4_HCOEFFS, 4);

    sfx_gains[0]   = *dram_u16(hle, sfx_ptr + SFX_U16_3C);
    sfx_gains[1]   = *dram_u16(hle, sfx_ptr + SFX_U16_3E);

    HleVerboseMessage(hle->user_defined,
                      "cbuffer: ptr=%08x length=%x", cbuffer_ptr,
                      cbuffer_length);

    HleVerboseMessage(hle->user_defined,
                      "fir4: hgain=%04x hcoeff=%04x %04x %04x %04x",
                      fir4_hgain,
                      fir4_hcoeffs[0], fir4_hcoeffs[1], fir4_hcoeffs[2], fir4_hcoeffs[3]);

    HleVerboseMessage(hle->user_defined,
                      "tap count=%d\n"
                      "delays: %08x %08x %08x %08x %08x %08x %08x %08x\n"
                      "gains:  %04x %04x %04x %04x %04x %04x %04x %04x",
                      tap_count,
                      tap_delays[0], tap_delays[1], tap_delays[2], tap_delays[3],
                      tap_delays[4], tap_delays[5], tap_delays[6], tap_delays[7],
                      tap_gains[0], tap_gains[1], tap_gains[2], tap_gains[3],
                      tap_gains[4], tap_gains[5], tap_gains[6], tap_gains[7]);

    HleVerboseMessage(hle->user_defined, "sfx_gains=%04x %04x", sfx_gains[0], sfx_gains[1]);

    /* mix up to 8 delayed subframes */
    memset(subframe, 0, SUBFRAME_SIZE * sizeof(subframe[0]));
    for (i = 0; i < tap_count; ++i) {

        dpos = pos - tap_delays[i];
        if (dpos < 0)
            dpos += cbuffer_length;
        dlength = SUBFRAME_SIZE;

        if ((uint32_t)(dpos + SUBFRAME_SIZE) > cbuffer_length) {
            dlength = cbuffer_length - dpos;
            /* The wrapped head is transferred first. SP DMA rounds its
             * destination down to eight bytes, then the rounded tail DMA
             * overwrites their partial overlap. */
            dram_load_u16(hle, (uint16_t *)delayed + (dlength&~3u),
                cbuffer_ptr&~7u,align(SUBFRAME_SIZE-dlength,4));
        }

        dram_load_u16(hle, (uint16_t *)delayed, (cbuffer_ptr + dpos * 2)&~7u, align(dlength,4));
        store_u16(hle->dmem,musyx->version2?0x970u:0x7d0u,(uint16_t*)delayed,SUBFRAME_SIZE);

        mix_subframes(subframe, delayed, tap_gains[i]);
    }

    /* add resulting subframe to main subframes */
    mix_sfx_with_main_subframes(musyx, subframe, sfx_gains);

    /* apply FIR4 filter and writeback filtered result */
    memcpy(buffer, musyx->subframe_740_last4, 4 * sizeof(int16_t));
    memcpy(musyx->subframe_740_last4, subframe + SUBFRAME_SIZE - 4, 4 * sizeof(int16_t));
    hle->rsp_f5[3]=0;hle->rsp_f5[4]=1u<<16;
    hle->rsp_f5_written|=24u;
    mix_fir4(musyx->e50, buffer + 1, fir4_hgain, fir4_hcoeffs);
    /* The gained return follows the FIR in the native schedule. Its last
     * right-channel vector replaces V12, while v1 leaves the FIR's folded
     * third gather in V13. */
    hle->rsp_f5[0]=(uint16_t)musyx->right[184]|((uint32_t)(uint16_t)musyx->right[185]<<16);
    for(i=0;i<2;i++){
        if(musyx->version2){
            hle->rsp_f5[1+i]=(uint16_t)((int32_t)subframe[188+i*2]*sfx_gains[1]>>16)|
                ((uint32_t)(uint16_t)((int32_t)subframe[189+i*2]*sfx_gains[1]>>16)<<16);
            hle->rsp_f5[5+i]=(uint16_t)musyx->cc0[188+i*2]|
                ((uint32_t)(uint16_t)musyx->cc0[189+i*2]<<16);
        }else{
            int tap=i*2,first=clamp_s16(((int32_t)fir4_hcoeffs[tap]*fir4_hgain+0x4000)>>15);
            int second=clamp_s16(((int32_t)fir4_hcoeffs[tap+1]*fir4_hgain+0x4000)>>15);
            int a=clamp_s16(((int32_t)subframe[187+tap]*first+0x4000)>>15);
            int b=clamp_s16(((int32_t)subframe[188+tap]*second+0x4000)>>15);
            uint32_t sum=(uint16_t)clamp_s16(a+b);
            hle->rsp_f5[5+i]=sum|((uint32_t)(uint16_t)clamp_s16(b+b)<<16);
        }
    }
    hle->rsp_f5_written|=103u;
    dram_store_u16(hle, (uint16_t *)musyx->e50, (cbuffer_ptr + pos * 2)&~7u, SUBFRAME_SIZE);
}

static void mix_sfx_with_main_subframes_v1(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* UNUSED(gains))
{
    unsigned i;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        int16_t v = subframe[i];
        musyx->left[i]  = clamp_s16(musyx->left[i]  + v);
        musyx->right[i] = clamp_s16(musyx->right[i] + v);
    }
}

static void mix_sfx_with_main_subframes_v2(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains)
{
    unsigned i;

    /* The send scales by a Q16 gain and shifts the product back down.
     * The shift truncates, so every sample loses up to a whole LSB in
     * the same direction - a half-LSB bias on the reverb return rather
     * than noise around it.  Rounding costs nothing and is 5.6 dB
     * closer to the exact product; like the resampler and the MP3
     * butterflies this is arithmetic getting nearer a defined answer,
     * not a change of intent, so it follows the enhanced option. */
    const int hq = resample_hq_enabled();

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        int16_t v = subframe[i];
        int16_t v1 = hq ? (int16_t)(((int32_t)(v * gains[0]) + 0x8000) >> 16)
                        : (int16_t)((int32_t)(v * gains[0]) >> 16);
        int16_t v2 = hq ? (int16_t)(((int32_t)(v * gains[1]) + 0x8000) >> 16)
                        : (int16_t)((int32_t)(v * gains[1]) >> 16);

        musyx->left[i]  = clamp_s16(musyx->left[i]  + v1);
        musyx->right[i] = clamp_s16(musyx->right[i] + v1);
        musyx->cc0[i]   = clamp_s16(musyx->cc0[i]   + v2);
    }
}

static void mix_samples(int16_t *y, int16_t x, int16_t hgain)
{
    *y = clamp_s16(*y + ((x * hgain + 0x4000) >> 15));
}

static void mix_subframes(int16_t *y, const int16_t *x, int16_t hgain)
{
    unsigned int i;

    for (i = 0; i < SUBFRAME_SIZE; ++i)
        mix_samples(&y[i], x[i], hgain);
}

static void mix_fir4(int16_t *y, const int16_t *x, int16_t hgain, const int16_t *hcoeffs)
{
    unsigned int i;
    int32_t h[4];
    const int hq = resample_hq_enabled();

    /* The microcode derives the filter taps with a single VMULF
     * (hcoeffs * hgain), so each tap is rounded and clamped, not
     * truncated (v2 task text, insn 0xe54). */
    h[0] = clamp_s16((hgain * hcoeffs[0] + 0x4000) >> 15);
    h[1] = clamp_s16((hgain * hcoeffs[1] + 0x4000) >> 15);
    h[2] = clamp_s16((hgain * hcoeffs[2] + 0x4000) >> 15);
    h[3] = clamp_s16((hgain * hcoeffs[3] + 0x4000) >> 15);

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        /* Same VMULF-per-tap + saturating VADD reduction tree as the
         * resample FIR (v2 task text, insns 0xe88..0xefc), with the
         * destination folded into the gather accumulator by VMADH and
         * saturated once on readout. */
        int32_t t0, t1, t2, t3, v;

        if (hq) {
            /* Four taps rounded separately and summed through clamped
             * pairs leave four roundings where the result only needs
             * one: 0.58 LSB against 0.29 for a single accumulate, which
             * is the floor for a 16-bit result.  Carrying the products
             * full width and rounding on the way out is 6.0 dB closer
             * to the exact filter, level for level.  The reverb this
             * feeds is shared by both task versions, so it reaches the
             * v1 titles that the gained sfx send does not. */
            int64_t acc = (int64_t)h[0] * x[i]
                        + (int64_t)h[1] * x[i + 1]
                        + (int64_t)h[2] * x[i + 2]
                        + (int64_t)h[3] * x[i + 3];

            y[i] = clamp_s16(y[i] + (int32_t)((acc + 0x4000) >> 15));
            continue;
        }

        t0 = clamp_s16((h[0] * x[i]     + 0x4000) >> 15);
        t1 = clamp_s16((h[1] * x[i + 1] + 0x4000) >> 15);
        t2 = clamp_s16((h[2] * x[i + 2] + 0x4000) >> 15);
        t3 = clamp_s16((h[3] * x[i + 3] + 0x4000) >> 15);
        v  = clamp_s16(t0 + t1) + clamp_s16(t2 + t3);
        y[i] = clamp_s16(y[i] + v);
    }
}

static void interleave_stage_v1(struct hle_t* hle, musyx_t *musyx, uint32_t output_ptr)
{
    size_t i;

    int16_t base_left;
    int16_t base_right;

    int16_t *left;
    int16_t *right;
    uint32_t *dst;

    HleVerboseMessage(hle->user_defined, "interleave: %08x", output_ptr);

    base_left  = clamp_s16(musyx->base_vol[0]);
    base_right = clamp_s16(musyx->base_vol[1]);

    left  = musyx->left;
    right = musyx->right;
    dst  = dram_u32(hle, output_ptr);

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        uint16_t l = clamp_s16(*(left++)  + base_left);
        uint16_t r = clamp_s16(*(right++) + base_right);

        *(dst++) = (l << 16) | r;
    }
    for(i=0;i<2;i++)hle->rsp_f5[1+i]=(uint16_t)clamp_s16(musyx->right[188+i*2]+base_right)|
        ((uint32_t)(uint16_t)clamp_s16(musyx->right[189+i*2]+base_right)<<16);
    hle->rsp_f5_written|=6u;
}

static void interleave_stage_v2(struct hle_t* hle, musyx_t *musyx,
                                uint16_t mask_16, uint32_t ptr_18,
                                uint32_t ptr_1c, uint32_t output_ptr)
{
    unsigned i, k;
    int16_t subframe[SUBFRAME_SIZE];
    uint32_t *dst;
    uint16_t mask;

    HleVerboseMessage(hle->user_defined,
                      "mask_16=%04x ptr_18=%08x ptr_1c=%08x output_ptr=%08x",
                      mask_16, ptr_18, ptr_1c, output_ptr);

    /* compute L_total, R_total and update subframe @ptr_1c */
    memset(subframe, 0, SUBFRAME_SIZE*sizeof(subframe[0]));

    for(i = 0; i < SUBFRAME_SIZE; ++i) {
        int16_t v = *dram_u16(hle, ptr_1c + i*2);
        musyx->left[i] = v;
        /* VSUB consumes VCO left by the final auxiliary FIR fold on
         * its first vector only; later iterations see cleared flags. */
        musyx->right[i] = clamp_s16(-v - (i < 8 ? (int)((musyx->auxiliary_carry >> i) & 1u) : 0));
    }

    for (k = 0, mask = 1; k < 8; ++k, mask <<= 1, ptr_18 += 8) {
        int16_t hgain;
        uint32_t address;

        if ((mask_16 & mask) == 0)
            continue;

        address = *dram_u32(hle, ptr_18) & ~7u;
        hgain   = *dram_u16(hle, ptr_18 + 4);

        for(i = 0; i < SUBFRAME_SIZE; ++i, address += 2) {
            mix_samples(&musyx->left[i],  *dram_u16(hle, address), hgain);
            mix_samples(&musyx->right[i], *dram_u16(hle, address + 2*SUBFRAME_SIZE), hgain);
            mix_samples(&subframe[i],     *dram_u16(hle, address + 4*SUBFRAME_SIZE), hgain);
        }
    }

    /* interleave L_total and R_total */
    dst = dram_u32(hle, output_ptr);
    for(i = 0; i < SUBFRAME_SIZE; ++i) {
        uint16_t l = musyx->left[i];
        uint16_t r = musyx->right[i];
        *(dst++) = (l << 16) | r;
    }

    /* writeback subframe @ptr_1c */
    dram_store_u16(hle, (uint16_t*)subframe, ptr_1c, SUBFRAME_SIZE);
    for(i=0;i<2;i++)hle->rsp_f5[1+i]=(uint16_t)musyx->right[188+i*2]|
        ((uint32_t)(uint16_t)musyx->right[189+i*2]<<16);
    hle->rsp_f5_written|=6u;
}
