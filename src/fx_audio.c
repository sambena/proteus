/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The TIA's two voices, processed apart: each is freed of its offset, filtered and placed
 * in the stereo picture, and both get a room around them.
 *
 * Stella gives the voices apart when its stereo sound is on: voice 0 on the left channel
 * and voice 1 on the right one, each between zero and full scale.
 */
#include "fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define COMBS     4
#define ALLPASSES 2
#define MAX_DELAY 2048

typedef struct
{
   float buf[MAX_DELAY];
   unsigned len, pos;
   float store;
} delay_line;

struct px_fx_audio
{
   double rate;
   float dc_r;

   float dc_x[2], dc_y[2];
   float lp[2];

   delay_line comb[2][COMBS];
   delay_line allpass[2][ALLPASSES];
};

static void set_lengths(px_fx_audio *a)
{
   /* Freeverb's tunings, which are for 44.1 kHz. */
   static const unsigned comb[COMBS] = { 1116, 1188, 1277, 1356 };
   static const unsigned allpass[ALLPASSES] = { 556, 341 };
   const double k = a->rate / 44100.0;

   for (unsigned ch = 0; ch < 2; ch++)
   {
      for (unsigned i = 0; i < COMBS; i++)
      {
         unsigned len = (unsigned)((comb[i] + ch * 23) * k + 0.5);
         a->comb[ch][i].len = len < 8 ? 8 : len > MAX_DELAY ? MAX_DELAY : len;
      }
      for (unsigned i = 0; i < ALLPASSES; i++)
      {
         unsigned len = (unsigned)((allpass[i] + ch * 23) * k + 0.5);
         a->allpass[ch][i].len = len < 8 ? 8 : len > MAX_DELAY ? MAX_DELAY : len;
      }
   }
}

px_fx_audio *px_fx_audio_new(void)
{
   px_fx_audio *a = (px_fx_audio*)calloc(1, sizeof(*a));
   if (a)
      px_fx_audio_set_rate(a, 31440.0);
   return a;
}

void px_fx_audio_free(px_fx_audio *a)
{
   free(a);
}

void px_fx_audio_reset(px_fx_audio *a)
{
   double rate;
   if (!a)
      return;
   rate = a->rate;
   memset(a, 0, sizeof(*a));
   px_fx_audio_set_rate(a, rate);
}

void px_fx_audio_set_rate(px_fx_audio *a, double rate)
{
   if (!a || rate < 8000.0)
      return;
   if (rate == a->rate)
      return;
   memset(a, 0, sizeof(*a));
   a->rate = rate;
   a->dc_r = (float)(1.0 - 2.0 * M_PI * 20.0 / rate);
   set_lengths(a);
}

static inline float comb_run(delay_line *d, float in, float feedback, float damp)
{
   float out = d->buf[d->pos];
   d->store = out * (1.0f - damp) + d->store * damp;
   d->buf[d->pos] = in + d->store * feedback;
   if (++d->pos >= d->len)
      d->pos = 0;
   return out;
}

static inline float allpass_run(delay_line *d, float in)
{
   float held = d->buf[d->pos];
   float out  = held - in;
   d->buf[d->pos] = in + held * 0.5f;
   if (++d->pos >= d->len)
      d->pos = 0;
   return out;
}

static inline float limit(float x)
{
   const float knee = 0.8f;
   float m = fabsf(x);
   if (m <= knee)
      return x;
   m = knee + (1.0f - knee) * tanhf((m - knee) / (1.0f - knee));
   return x < 0.0f ? -m : m;
}

void px_fx_audio_process(px_fx_audio *a, const px_fx_config *c, int16_t *frames, size_t count)
{
   static const float feedback[4] = { 0.0f, 0.70f, 0.80f, 0.88f };
   static const float wet[4]      = { 0.0f, 0.10f, 0.17f, 0.26f };
   /* A voice alone is as loud here as it is in Stella's own mix of the two. */
   const float voice_gain = 0.62f;
   const float width = (float)c->width / 100.0f;
   const float t0 = (float)((1.0 - width) * M_PI / 4.0), t1 = (float)((1.0 + width) * M_PI / 4.0);
   const float l0 = cosf(t0), r0 = sinf(t0), l1 = cosf(t1), r1 = sinf(t1);
   const float cutoff = c->lowpass == 1 ? 8000.0f : 4500.0f;
   float lp_a;

   if (!a || !frames)
      return;
   lp_a = c->lowpass ? (float)(1.0 - exp(-2.0 * M_PI * cutoff / a->rate)) : 1.0f;

   for (size_t i = 0; i < count; i++)
   {
      float v[2], left, right;

      for (unsigned k = 0; k < 2; k++)
      {
         float x = (float)frames[i * 2 + k] * (1.0f / 32768.0f);
         float y = x - a->dc_x[k] + a->dc_r * a->dc_y[k];
         a->dc_x[k] = x;
         a->dc_y[k] = y;
         a->lp[k]  += lp_a * (y - a->lp[k]);
         v[k] = a->lp[k] * voice_gain;
      }

      left  = v[0] * l0 + v[1] * l1;
      right = v[0] * r0 + v[1] * r1;

      if (c->reverb)
      {
         const float in = (v[0] + v[1]) * 0.25f;
         const float fb = feedback[c->reverb & 3], mix = wet[c->reverb & 3];
         float room[2] = { 0.0f, 0.0f };
         for (unsigned ch = 0; ch < 2; ch++)
         {
            for (unsigned k = 0; k < COMBS; k++)
               room[ch] += comb_run(&a->comb[ch][k], in, fb, 0.3f);
            for (unsigned k = 0; k < ALLPASSES; k++)
               room[ch] = allpass_run(&a->allpass[ch][k], room[ch]);
         }
         left  += room[0] * mix;
         right += room[1] * mix;
      }

      left  = limit(left) * 32767.0f;
      right = limit(right) * 32767.0f;
      frames[i * 2]     = (int16_t)lrintf(left);
      frames[i * 2 + 1] = (int16_t)lrintf(right);
   }
}
