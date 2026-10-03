/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - audio_backend_compat.c                                  *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

#include "api/m64p_types.h"
#include <libretro.h>
#include "device/rcp/ai_controller.h"
#include "../src/main/main.h"
#include "../src/device/device.h"
#include "../src/main/rom.h"
#include "plugin/plugin.h"
#include "device/rcp/ri_controller.h"
#include "device/rcp/rdp/vi_controller.h"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include <audio/conversion/float_to_s16.h>
#include <audio/conversion/s16_to_float.h>
#include <audio/audio_resampler.h>

extern retro_audio_sample_batch_t audio_batch_cb;

static unsigned MAX_AUDIO_FRAMES = 2048;

static double GameFreq = 33600.0;
#define AUDIO_QUEUE_FRAMES 16384
static int16_t audio_queue[AUDIO_QUEUE_FRAMES * 2];
static size_t audio_read, audio_count, audio_due;
static double audio_fraction;
static float dac_left, dac_right;
static int dac_valid;
static int audio_frame_started;
#ifdef M64P_AUDIO_SYNC_TRACE
size_t audio_sync_queued, audio_sync_due;
#endif

static const retro_resampler_t *resampler;
static void *resampler_audio_data;
static float *audio_in_buffer_float;
static float *audio_out_buffer_float;
static int16_t *audio_out_buffer_s16;

void (*audio_convert_s16_to_float_arm)(float *out,
      const int16_t *in, size_t samples, float gain);
void (*audio_convert_float_to_s16_arm)(int16_t *out,
      const float *in, size_t samples);

void deinit_audio_libretro(void)
{
   if (resampler && resampler_audio_data)
   {
      resampler->free(resampler_audio_data);
      resampler = NULL;
      resampler_audio_data = NULL;
   }
   free(audio_in_buffer_float);
   free(audio_out_buffer_float);
   free(audio_out_buffer_s16);
   audio_in_buffer_float = audio_out_buffer_float = NULL;
   audio_out_buffer_s16 = NULL;
}

void init_audio_libretro(unsigned max_audio_frames)
{
   audio_read = audio_due = 0;
   /* Cover resampler startup and sample quantization, below 1 ms at 44.1 kHz. */
   audio_count = 32;
   memset(audio_queue, 0, audio_count * 2 * sizeof(*audio_queue));
   audio_fraction = 0;
   dac_left = dac_right = 0;
   dac_valid = 0;
   audio_frame_started = 0;
   GameFreq = 33600.0;
   retro_resampler_realloc(&resampler_audio_data, &resampler, "sinc", RESAMPLER_QUALITY_DONTCARE, 1.0);

   MAX_AUDIO_FRAMES = max_audio_frames;

   audio_in_buffer_float  = malloc(2 * MAX_AUDIO_FRAMES * sizeof(float));
   audio_out_buffer_float = malloc(2 * MAX_AUDIO_FRAMES * sizeof(float));
   audio_out_buffer_s16   = malloc(2 * MAX_AUDIO_FRAMES * sizeof(int16_t));

   convert_s16_to_float_init_simd();
   convert_float_to_s16_init_simd();
}

/* A fully compliant implementation is not really possible with just the zilmar spec.
 * We assume bits == 16 (assumption compatible with audio-sdl plugin implementation)
 */
void set_audio_format_via_libretro(void* user_data,
      unsigned int frequency)
{
   const struct ai_controller* ai = user_data;
   GameFreq = ai && ai->vi && ai->vi->clock && ai->regs[AI_DACRATE_REG]
      ? ai->vi->clock / (ai->regs[AI_DACRATE_REG] + 1.0)
      : (frequency ? frequency : 44100.0);
}

static void queue_audio(const int16_t *samples, size_t frames)
{
   while (frames) {
      size_t write = (audio_read + audio_count) % AUDIO_QUEUE_FRAMES;
      size_t n = AUDIO_QUEUE_FRAMES - write;
      if (n > frames) n = frames;
      if (audio_count + n > AUDIO_QUEUE_FRAMES) {
         size_t discard = audio_count + n - AUDIO_QUEUE_FRAMES;
         audio_read = (audio_read + discard) % AUDIO_QUEUE_FRAMES;
         audio_count -= discard;
         audio_due -= discard < audio_due ? discard : audio_due;
      }
      memcpy(audio_queue + write * 2, samples, n * 2 * sizeof(*samples));
      audio_count += n;
      samples += n * 2;
      frames -= n;
   }
}

static size_t input_capacity(void)
{
   double ratio = 44100.0 / GameFreq;
   size_t n = ratio < 1 ? MAX_AUDIO_FRAMES : (size_t)(MAX_AUDIO_FRAMES / ratio);
   return n > 1 ? n - 1 : 1;
}

static void resample_audio(size_t frames)
{
   struct resampler_data data = {0};
   data.data_in = audio_in_buffer_float;
   data.data_out = audio_out_buffer_float;
   data.input_frames = frames;
   data.ratio = 44100.0 / GameFreq;
   resampler->process(resampler_audio_data, &data);
   convert_float_to_s16(audio_out_buffer_s16, audio_out_buffer_float, data.output_frames * 2);
   queue_audio(audio_out_buffer_s16, data.output_frames);
}

void push_audio_idle_samples_via_libretro(void *user_data, size_t frames)
{
   (void)user_data;
   /* The initial boot can span many guest fields before its first video
    * callback. Frame budgeting supplies that silent startup, without backlog. */
   if (!audio_frame_started) return;
   float decay = (float)exp(-1.0 / (GameFreq * 0.003));
   while (frames) {
      size_t n = input_capacity();
      if (n > frames) n = frames;
      for (size_t i = 0; i < n; ++i) {
         dac_left *= decay; dac_right *= decay;
         if (dac_left > -1e-7f && dac_left < 1e-7f) dac_left = 0;
         if (dac_right > -1e-7f && dac_right < 1e-7f) dac_right = 0;
         audio_in_buffer_float[2*i] = dac_left;
         audio_in_buffer_float[2*i+1] = dac_right;
      }
      resample_audio(n);
      frames -= n;
   }
}

void audio_end_frame_libretro(double seconds)
{
   double elapsed = seconds * 44100.0 + audio_fraction;
   size_t frames = (size_t)elapsed;
   audio_fraction = elapsed - frames;
   audio_due += frames;
   if (audio_due > AUDIO_QUEUE_FRAMES) audio_due = AUDIO_QUEUE_FRAMES;
   if (!dac_valid) {
      static const int16_t silence[512] = {0};
      size_t target = audio_due + 32;
      if (target > AUDIO_QUEUE_FRAMES) target = AUDIO_QUEUE_FRAMES;
      while (audio_count < target) {
         size_t n = target - audio_count;
         queue_audio(silence, n < 256 ? n : 256);
      }
   }
   audio_frame_started = 1;
   while (audio_due && audio_count) {
      size_t n = AUDIO_QUEUE_FRAMES - audio_read;
      if (n > audio_due) n = audio_due;
      if (n > audio_count) n = audio_count;
      size_t accepted = audio_batch_cb(audio_queue + audio_read * 2, n);
      if (!accepted || accepted > n) break;
      audio_read = (audio_read + accepted) % AUDIO_QUEUE_FRAMES;
      audio_count -= accepted;
      audio_due -= accepted;
   }
#ifdef M64P_AUDIO_SYNC_TRACE
   audio_sync_queued = audio_count;
   audio_sync_due = audio_due;
#endif
}

/* Abuse core & audio plugin implementation details to obtain the desired effect. */
void push_audio_samples_via_libretro(void* user_data, const void* buffer, size_t size)
{
   (void)user_data;
   const int16_t *samples = buffer;
   size_t frames = size / 4;
   while (frames) {
      size_t n = input_capacity();
      if (n > frames) n = frames;
      convert_s16_to_float(audio_in_buffer_float, samples, n * 2, 1.0f);
#if S8 == 3
      /* Word-swapped RDRAM is right/left. Never swap the guest's own buffer. */
      for (size_t i = 0; i < n * 2; i += 2) {
         float left = audio_in_buffer_float[i+1];
         audio_in_buffer_float[i+1] = audio_in_buffer_float[i];
         audio_in_buffer_float[i] = left;
      }
#endif
      dac_left = audio_in_buffer_float[(n-1)*2];
      dac_right = audio_in_buffer_float[(n-1)*2+1];
      dac_valid = 1;
      resample_audio(n);
      samples += n * 2;
      frames -= n;
   }
}


m64p_error dummyaudio_PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion,
                                       int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_AUDIO;

    if (PluginVersion != NULL)
        *PluginVersion = 0x00010000;

    if (APIVersion != NULL)
        *APIVersion = AUDIO_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "Mupen64Plus-NoAudio";

    if (Capabilities != NULL)
        *Capabilities = 0;

    return M64ERR_SUCCESS;
}

void dummyaudio_AiDacrateChanged(int SystemType)
{
    return;
}

void dummyaudio_AiLenChanged(void)
{
    return;
}

int dummyaudio_InitiateAudio(AUDIO_INFO Audio_Info)
{
    return 1;
}

int dummyaudio_RomOpen(void)
{
    return 1;
}

void dummyaudio_RomClosed(void)
{
    return;
}

void dummyaudio_ProcessAList(void)
{
    return;
}

void dummyaudio_SetSpeedFactor(int percent)
{
    return;
}

void dummyaudio_VolumeUp(void)
{
    return;
}

void dummyaudio_VolumeDown(void)
{
    return;
}

int dummyaudio_VolumeGetLevel(void)
{
    return 0;
}

void dummyaudio_VolumeSetLevel(int level)
{
    return;
}

void dummyaudio_VolumeMute(void)
{
    return;
}

const char *dummyaudio_VolumeGetString(void)
{
    return "disabled";
}

