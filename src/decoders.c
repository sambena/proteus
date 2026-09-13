/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* WAV, MP3 and Ogg Vorbis sources built on dr_wav, dr_mp3 and stb_vorbis. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "music.h"
#include "util.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_WCHAR_WARNINGS
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "stb_vorbis.c"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef enum { SRC_WAV, SRC_MP3, SRC_OGG } src_kind;

#define MP3_SEEK_POINTS 256

struct px_source
{
   src_kind kind;
   unsigned rate;
   unsigned channels;
   union
   {
      drwav wav;
      struct
      {
         drmp3 dec;
         drmp3_seek_point seek[MP3_SEEK_POINTS];
      } mp3;
      stb_vorbis *ogg;
   } u;
   int16_t *scratch; /* holds multichannel frames before folding to stereo */
   size_t scratch_frames;
};

static bool ext_is(const char *path, const char *ext)
{
   const char *e = px_path_ext(path);
   for (; *e && *ext; e++, ext++)
      if ((*e | 0x20) != *ext)
         return false;
   return !*e && !*ext;
}

static bool open_wav(px_source *s, const char *path)
{
#ifdef _WIN32
   wchar_t *w = px_utf8_to_wide(path);
   bool ok = w && drwav_init_file_w(&s->u.wav, w, NULL);
   free(w);
#else
   bool ok = drwav_init_file(&s->u.wav, path, NULL);
#endif
   if (!ok)
      return false;
   s->kind     = SRC_WAV;
   s->rate     = s->u.wav.sampleRate;
   s->channels = s->u.wav.channels;
   return true;
}

static bool open_mp3(px_source *s, const char *path)
{
   drmp3_uint32 points = MP3_SEEK_POINTS;
#ifdef _WIN32
   wchar_t *w = px_utf8_to_wide(path);
   bool ok = w && drmp3_init_file_w(&s->u.mp3.dec, w, NULL);
   free(w);
#else
   bool ok = drmp3_init_file(&s->u.mp3.dec, path, NULL);
#endif
   if (!ok)
      return false;
   /* A seek table keeps save state loads and loop points from rescanning the file. */
   if (drmp3_calculate_seek_points(&s->u.mp3.dec, &points, s->u.mp3.seek))
      drmp3_bind_seek_table(&s->u.mp3.dec, points, s->u.mp3.seek);
   s->kind     = SRC_MP3;
   s->rate     = s->u.mp3.dec.sampleRate;
   s->channels = s->u.mp3.dec.channels;
   return true;
}

static bool open_ogg(px_source *s, const char *path)
{
   FILE *f = px_fopen(path, "rb");
   stb_vorbis_info info;
   int error = 0;
   if (!f)
      return false;
   s->u.ogg = stb_vorbis_open_file(f, 1, &error, NULL);
   if (!s->u.ogg)
   {
      fclose(f);
      return false;
   }
   info        = stb_vorbis_get_info(s->u.ogg);
   s->kind     = SRC_OGG;
   s->rate     = info.sample_rate;
   s->channels = 2; /* stb_vorbis folds to the requested channel count */
   return true;
}

px_source *px_source_open(const char *path, char *err, size_t errlen)
{
   px_source *s = (px_source*)calloc(1, sizeof(*s));
   bool ok;

   if (!s)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }

   if (!px_file_exists(path))
   {
      snprintf(err, errlen, "file not found: %s", path);
      free(s);
      return NULL;
   }

   if (ext_is(path, "wav"))
      ok = open_wav(s, path);
   else if (ext_is(path, "mp3"))
      ok = open_mp3(s, path);
   else if (ext_is(path, "ogg"))
      ok = open_ogg(s, path);
   else
      ok = open_ogg(s, path) || open_mp3(s, path) || open_wav(s, path);

   if (!ok || s->rate == 0 || s->channels == 0)
   {
      snprintf(err, errlen, "unsupported or corrupt audio file: %s", path);
      if (ok)
         px_source_close(s);
      else
         free(s);
      return NULL;
   }
   return s;
}

size_t px_source_read(px_source *s, int16_t *out, size_t frames)
{
   size_t got = 0;
   int16_t *dst;

   if (s->kind == SRC_OGG)
      return (size_t)stb_vorbis_get_samples_short_interleaved(s->u.ogg, 2, out, (int)(frames * 2));

   if (s->channels == 2)
      dst = out;
   else
   {
      if (s->scratch_frames < frames)
      {
         int16_t *p = (int16_t*)realloc(s->scratch, frames * s->channels * sizeof(int16_t));
         if (!p)
            return 0;
         s->scratch        = p;
         s->scratch_frames = frames;
      }
      dst = s->scratch;
   }

   if (s->kind == SRC_WAV)
      got = (size_t)drwav_read_pcm_frames_s16(&s->u.wav, frames, dst);
   else
      got = (size_t)drmp3_read_pcm_frames_s16(&s->u.mp3.dec, frames, dst);

   if (s->channels == 1)
   {
      for (size_t i = got; i-- > 0;)
         out[i * 2] = out[i * 2 + 1] = dst[i];
   }
   else if (s->channels > 2)
   {
      for (size_t i = 0; i < got; i++)
      {
         out[i * 2]     = dst[i * s->channels];
         out[i * 2 + 1] = dst[i * s->channels + 1];
      }
   }
   return got;
}

bool px_source_seek(px_source *s, uint64_t frame)
{
   switch (s->kind)
   {
      case SRC_WAV:
         return drwav_seek_to_pcm_frame(&s->u.wav, frame);
      case SRC_MP3:
         return drmp3_seek_to_pcm_frame(&s->u.mp3.dec, frame);
      case SRC_OGG:
         return frame <= 0xFFFFFFFFu && stb_vorbis_seek(s->u.ogg, (unsigned)frame);
   }
   return false;
}

unsigned px_source_rate(const px_source *s)
{
   return s->rate;
}

void px_source_close(px_source *s)
{
   if (!s)
      return;
   switch (s->kind)
   {
      case SRC_WAV:
         drwav_uninit(&s->u.wav);
         break;
      case SRC_MP3:
         drmp3_uninit(&s->u.mp3.dec);
         break;
      case SRC_OGG:
         stb_vorbis_close(s->u.ogg);
         break;
   }
   free(s->scratch);
   free(s);
}
