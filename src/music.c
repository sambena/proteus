/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "music.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void voice_reset(px_voice *v)
{
   memset(v, 0, sizeof(*v));
}

static void voice_close(px_voice *v)
{
   if (v->src)
      px_source_close(v->src);
   voice_reset(v);
}

static void voice_next_frame(px_voice *v, float *out)
{
   if (v->buf_pos >= v->buf_len && !v->ended)
   {
      v->buf_pos = 0;
      v->buf_len = px_source_read(v->src, v->buf, PX_VOICE_BUFFER);
      if (v->buf_len == 0 && v->loop && px_source_seek(v->src, v->loop_start))
      {
         v->cursor  = v->loop_start;
         v->buf_len = px_source_read(v->src, v->buf, PX_VOICE_BUFFER);
      }
      if (v->buf_len == 0)
         v->ended = true;
   }

   if (v->ended)
   {
      out[0] = out[1] = 0.0f;
      return;
   }

   out[0] = v->buf[v->buf_pos * 2];
   out[1] = v->buf[v->buf_pos * 2 + 1];
   v->buf_pos++;
   v->cursor++;
}

static void voice_prime(px_voice *v)
{
   v->buf_len = v->buf_pos = 0;
   v->ended   = false;
   v->t       = 0.0;
   voice_next_frame(v, v->a);
   voice_next_frame(v, v->b);
}

void px_mixer_init(px_mixer *m)
{
   m->out_rate = 0.0;
   voice_reset(&m->current);
   voice_reset(&m->fading);
}

void px_mixer_set_rate(px_mixer *m, double out_rate)
{
   m->out_rate = out_rate;
}

bool px_mixer_play(px_mixer *m, const char *path, unsigned subtrack, bool loop,
      uint64_t loop_start, float volume, unsigned fade_frames, uint64_t start_frame,
      char *err, size_t errlen)
{
   px_source *src = px_source_open(path, subtrack, loop, m->out_rate, err, errlen);
   if (!src)
      return false;

   if (start_frame && !px_source_seek(src, start_frame))
      start_frame = 0;

   voice_close(&m->fading);
   if (m->current.src && fade_frames)
   {
      m->fading = m->current;
      m->fading.gain_step = -(m->fading.gain > 0.0f ? m->fading.gain : 1.0f) / (float)fade_frames;
   }
   else
      voice_close(&m->current);
   voice_reset(&m->current);

   m->current.src        = src;
   m->current.subtrack   = subtrack;
   snprintf(m->current.path, sizeof(m->current.path), "%s", path);
   m->current.loop       = loop;
   m->current.loop_start = loop_start;
   m->current.volume     = volume;
   m->current.cursor     = start_frame;
   m->current.gain       = fade_frames ? 0.0f : 1.0f;
   m->current.gain_step  = fade_frames ? 1.0f / (float)fade_frames : 0.0f;
   voice_prime(&m->current);
   return true;
}

void px_mixer_stop(px_mixer *m, unsigned fade_frames)
{
   voice_close(&m->fading);
   if (m->current.src && fade_frames)
   {
      m->fading = m->current;
      m->fading.gain_step = -(m->fading.gain > 0.0f ? m->fading.gain : 1.0f) / (float)fade_frames;
      voice_reset(&m->current);
   }
   else
      voice_close(&m->current);
}

/* Renders one output frame of `v` and advances the resampler. Returns false once
 * the voice is silent for good. */
static bool voice_render(px_voice *v, double step, float scale, float *l, float *r)
{
   float t = (float)v->t;
   float g = v->gain * v->volume * scale;

   *l += (v->a[0] + (v->b[0] - v->a[0]) * t) * g;
   *r += (v->a[1] + (v->b[1] - v->a[1]) * t) * g;

   v->t += step;
   while (v->t >= 1.0)
   {
      v->a[0] = v->b[0];
      v->a[1] = v->b[1];
      voice_next_frame(v, v->b);
      v->t -= 1.0;
   }

   if (v->gain_step != 0.0f)
   {
      v->gain += v->gain_step;
      if (v->gain >= 1.0f)
      {
         v->gain      = 1.0f;
         v->gain_step = 0.0f;
      }
      else if (v->gain <= 0.0f)
         return false;
   }
   return !(v->ended && v->a[0] == 0.0f && v->a[1] == 0.0f);
}

static int16_t clamp16(float x)
{
   if (x > 32767.0f)
      return 32767;
   if (x < -32768.0f)
      return -32768;
   return (int16_t)lrintf(x);
}

/* Adds the next output frame of both voices, in int16 scale, to *l and *r. */
static void render_frame(px_mixer *m, double cur_step, double fade_step, float music_gain,
      float *l, float *r)
{
   if (m->current.src && !voice_render(&m->current, cur_step, music_gain, l, r))
      voice_close(&m->current);
   if (m->fading.src && !voice_render(&m->fading, fade_step, music_gain, l, r))
      voice_close(&m->fading);
}

static bool voice_steps(const px_mixer *m, double *cur_step, double *fade_step)
{
   if (m->out_rate <= 0.0)
      return false;
   *cur_step  = m->current.src ? px_source_rate(m->current.src) / m->out_rate : 0.0;
   *fade_step = m->fading.src ? px_source_rate(m->fading.src) / m->out_rate : 0.0;
   return true;
}

void px_mixer_mix(px_mixer *m, int16_t *frames, size_t count, float game_gain, float music_gain)
{
   double cur_step, fade_step;

   if (!voice_steps(m, &cur_step, &fade_step))
      return;
   for (size_t i = 0; i < count; i++)
   {
      float l = frames[i * 2] * game_gain;
      float r = frames[i * 2 + 1] * game_gain;
      render_frame(m, cur_step, fade_step, music_gain, &l, &r);
      frames[i * 2]     = clamp16(l);
      frames[i * 2 + 1] = clamp16(r);
   }
}

void px_mixer_mix_float(px_mixer *m, float *frames, size_t count, float game_gain, float music_gain)
{
   double cur_step, fade_step;

   if (!voice_steps(m, &cur_step, &fade_step))
      return;
   for (size_t i = 0; i < count; i++)
   {
      float l = 0.0f, r = 0.0f;
      render_frame(m, cur_step, fade_step, music_gain, &l, &r);
      frames[i * 2]     = frames[i * 2] * game_gain + l / 32768.0f;
      frames[i * 2 + 1] = frames[i * 2 + 1] * game_gain + r / 32768.0f;
   }
}

bool px_mixer_is_playing(const px_mixer *m, const char *path, unsigned subtrack)
{
   return m->current.src && m->current.subtrack == subtrack && !strcmp(m->current.path, path);
}

uint64_t px_mixer_position(const px_mixer *m)
{
   return m->current.src ? m->current.cursor : 0;
}

unsigned px_mixer_source_rate(const px_mixer *m)
{
   return m->current.src ? px_source_rate(m->current.src) : 0;
}

bool px_mixer_seek(px_mixer *m, uint64_t frame)
{
   if (!m->current.src || !px_source_seek(m->current.src, frame))
      return false;
   m->current.cursor = frame;
   voice_prime(&m->current);
   return true;
}

void px_mixer_free(px_mixer *m)
{
   voice_close(&m->current);
   voice_close(&m->fading);
}
