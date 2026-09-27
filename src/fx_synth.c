/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sounds of Proteus's own. The TIA has two voices, and a game that wants a shot, a step and
 * an explosion at once lets one cut the other short. Here every sound has a voice to itself,
 * a place between left and right, and several of them make one sound of a game's: a thump
 * below a click, a sweep over a burst of noise.
 */
#include "fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct
{
   unsigned id;          /* 0: free */
   px_tone p;
   float pan_l, pan_r, gain;
   float target_l, target_r, target_gain;
   double phase, lfo;
   double t;             /* seconds since it began */
   float level;          /* of its envelope */
   float release;        /* what the level is multiplied by a sample while it fades; 0: not */
   float low;            /* the filter's state */
   float freq_to;        /* a pitch asked for while it sounds; 0: none */
   float freq_now;
   uint32_t noise;
   float noise_held;
   double noise_phase;
} voice;

struct px_synth
{
   double rate;
   voice voices[PX_SYNTH_VOICES];
   unsigned next_id;
};

px_synth *px_synth_new(void)
{
   px_synth *s = (px_synth*)calloc(1, sizeof(*s));
   if (s)
   {
      s->rate    = 31440.0;
      s->next_id = 1;
   }
   return s;
}

void px_synth_free(px_synth *s)
{
   free(s);
}

void px_synth_reset(px_synth *s)
{
   if (s)
      memset(s->voices, 0, sizeof(s->voices));
}

void px_synth_set_rate(px_synth *s, double rate)
{
   if (s && rate >= 8000.0)
      s->rate = rate;
}

static void place(float pan, float *l, float *r)
{
   float a;
   if (pan < -1.0f) pan = -1.0f;
   if (pan > 1.0f)  pan = 1.0f;
   a  = (pan + 1.0f) * (float)(M_PI / 4.0);
   *l = cosf(a);
   *r = sinf(a);
}

unsigned px_synth_play(px_synth *s, const px_tone *p, float pan, float gain)
{
   voice *v = NULL;
   if (!s || !p)
      return 0;
   /* A free voice, or else the one that is nearest to silence. */
   for (unsigned i = 0; i < PX_SYNTH_VOICES; i++)
   {
      voice *c = &s->voices[i];
      if (!c->id)
      {
         v = c;
         break;
      }
      if (!v || c->level * c->gain < v->level * v->gain)
         v = c;
   }
   memset(v, 0, sizeof(*v));
   v->id = s->next_id++;
   if (!s->next_id)
      s->next_id = 1;
   v->p = *p;
   place(pan, &v->pan_l, &v->pan_r);
   v->target_l    = v->pan_l;
   v->target_r    = v->pan_r;
   v->gain        = gain;
   v->target_gain = gain;
   v->noise       = 0x1234567u + v->id * 2654435761u;
   v->freq_now    = p->freq;
   v->level       = p->attack > 0.0f ? 0.0f : 1.0f;
   return v->id;
}

static voice *find(px_synth *s, unsigned id)
{
   for (unsigned i = 0; s && id && i < PX_SYNTH_VOICES; i++)
      if (s->voices[i].id == id)
         return &s->voices[i];
   return NULL;
}

bool px_synth_move(px_synth *s, unsigned id, float pan, float gain, float freq)
{
   voice *v = find(s, id);
   if (!v)
      return false;
   place(pan, &v->target_l, &v->target_r);
   v->target_gain = gain;
   if (freq > 0.0f)
      v->freq_to = freq;
   return true;
}

void px_synth_stop(px_synth *s, unsigned id, float seconds)
{
   voice *v = find(s, id);
   if (!v)
      return;
   if (seconds < 0.002f)
      seconds = 0.002f;
   /* To a thousandth in that time. */
   v->release = (float)exp(log(0.001) / (seconds * s->rate));
}

unsigned px_synth_sounding(const px_synth *s)
{
   unsigned n = 0;
   for (unsigned i = 0; s && i < PX_SYNTH_VOICES; i++)
      if (s->voices[i].id)
         n++;
   return n;
}

void px_synth_render(px_synth *s, float *frames, size_t count)
{
   if (!s || !frames)
      return;
   for (unsigned i = 0; i < PX_SYNTH_VOICES; i++)
   {
      voice *v = &s->voices[i];
      const px_tone *p = &v->p;
      const double dt = 1.0 / s->rate;
      const float decay = p->decay > 0.0f ? (float)exp(log(0.001) / (p->decay * s->rate)) : 1.0f;
      const float rise = p->attack > 0.0f ? (float)(dt / p->attack) : 1.0f;

      if (!v->id)
         continue;
      for (size_t n = 0; n < count; n++)
      {
         float along = p->glide > 0.0f ? (float)(v->t / p->glide) : 1.0f;
         float freq, out, cutoff;

         if (along > 1.0f)
            along = 1.0f;
         /* A pitch glides by ratios, not by differences. */
         freq = p->freq_end > 0.0f && p->freq > 0.0f
               ? p->freq * powf(p->freq_end / p->freq, along) : p->freq;
         if (v->freq_to > 0.0f)
         {
            v->freq_now += (v->freq_to - v->freq_now) * 0.0008f;
            freq = v->freq_now;
         }
         if (p->vibrato_hz > 0.0f)
         {
            v->lfo += p->vibrato_hz * dt;
            if (v->lfo >= 1.0)
               v->lfo -= 1.0;
            freq *= 1.0f + p->vibrato * sinf((float)(v->lfo * 2.0 * M_PI));
         }

         v->phase += freq * dt;
         if (v->phase >= 1.0)
            v->phase -= floor(v->phase);
         switch (p->wave)
         {
            case PX_WAVE_SINE:
               out = sinf((float)(v->phase * 2.0 * M_PI));
               break;
            case PX_WAVE_TRIANGLE:
               out = v->phase < 0.5 ? (float)(v->phase * 4.0 - 1.0) : (float)(3.0 - v->phase * 4.0);
               break;
            case PX_WAVE_SQUARE:
               out = v->phase < 0.5 ? 0.7f : -0.7f;
               break;
            case PX_WAVE_SAW:
               out = (float)(v->phase * 2.0 - 1.0) * 0.8f;
               break;
            default:
               /* Noise whose grain is the pitch: a new value that many times a second. */
               v->noise_phase += freq * dt;
               if (v->noise_phase >= 1.0 || freq <= 0.0f)
               {
                  v->noise_phase -= floor(v->noise_phase);
                  v->noise      = v->noise * 1664525u + 1013904223u;
                  v->noise_held = (float)(int32_t)v->noise * (1.0f / 2147483648.0f);
               }
               out = v->noise_held;
               break;
         }

         if (p->cutoff > 0.0f)
         {
            cutoff = p->cutoff_end > 0.0f
                  ? p->cutoff * powf(p->cutoff_end / p->cutoff, along) : p->cutoff;
            if (cutoff > 0.45f * (float)s->rate)
               cutoff = 0.45f * (float)s->rate;
            v->low += (1.0f - expf(-2.0f * (float)M_PI * cutoff * (float)dt)) * (out - v->low);
            out = v->low;
         }

         if (v->release > 0.0f)
            v->level *= v->release;
         else if (v->t < p->attack)
         {
            v->level += rise;
            if (v->level > 1.0f)
               v->level = 1.0f;
         }
         else if (v->t >= p->attack + p->hold)
            v->level *= decay;

         v->pan_l += (v->target_l - v->pan_l) * 0.002f;
         v->pan_r += (v->target_r - v->pan_r) * 0.002f;
         v->gain  += (v->target_gain - v->gain) * 0.002f;

         out *= v->level * v->gain * p->gain;
         frames[n * 2]     += out * v->pan_l;
         frames[n * 2 + 1] += out * v->pan_r;
         v->t += dt;

         if (v->level < 0.0008f && (v->release > 0.0f || v->t > p->attack + p->hold))
         {
            v->id = 0;
            break;
         }
      }
   }
}

void px_sound_rumble(px_sound *s, unsigned strong, unsigned weak, unsigned frames)
{
   if (!s)
      return;
   if (strong > 65535) strong = 65535;
   if (weak > 65535)   weak = 65535;
   if (strong > s->rumble_strong) s->rumble_strong = strong;
   if (weak > s->rumble_weak)     s->rumble_weak = weak;
   if (frames > s->rumble_frames) s->rumble_frames = frames;
}
