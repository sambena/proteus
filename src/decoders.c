/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Audio sources: WAV, MP3 and Ogg Vorbis through dr_wav, dr_mp3 and stb_vorbis;
 * SPC, NSF, VGM, GBS and other chip music emulated by libgme. */
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
#include "gme.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "usf_play.h"
#include "rsn_play.h"
#include "gsf_play.h"

typedef enum { SRC_WAV, SRC_MP3, SRC_OGG, SRC_GME, SRC_USF, SRC_GSF } src_kind;
#include "twosf_play.h"
#include "ncsf_play.h"

typedef enum { SRC_WAV, SRC_MP3, SRC_OGG, SRC_GME, SRC_USF,
   SRC_NDS /* DS 2SF and NCSF rips */
} src_kind;
#include "snsf_play.h"

typedef enum { SRC_WAV, SRC_MP3, SRC_OGG, SRC_GME, SRC_USF, SRC_SNSF } src_kind;

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
      Music_Emu *gme;
      struct
      {
         px_usf *player;
         uint64_t position;   /* frames rendered since the start */
         uint64_t end;        /* frames before the fade ends when not looping, else 0 */
         uint64_t fade;       /* frames of fade-out before `end` */
      } usf;
      struct
      {
         px_gsf *player;
         uint64_t position;   /* frames rendered since the start */
         uint64_t end;        /* frames before the fade ends when not looping, else 0 */
         uint64_t fade;       /* frames of fade-out before `end` */
      } gsf;
         px_2sf *twosf;       /* one of these two plays */
         px_ncsf *ncsf;
         uint64_t position, end, fade; /* as for USF */
      } nds;
         px_snsf *player;
         uint64_t position, end, fade;   /* as for usf */
      } snsf;
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

#define GME_MAX_FILE (64 * 1024 * 1024)

/* Loads a whole file; libgme parses from memory so UTF-8 paths work on Windows. */
static void *read_file(const char *path, long *size)
{
   FILE *f = px_fopen(path, "rb");
   void *data = NULL;
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) == 0 && (*size = ftell(f)) > 0 && *size <= GME_MAX_FILE
         && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)*size)))
   {
      if (fread(data, 1, (size_t)*size, f) != (size_t)*size)
      {
         free(data);
         data = NULL;
      }
   }
   fclose(f);
   return data;
}

static Music_Emu *open_gme_emu(const char *path, int rate, char *err, size_t errlen)
{
   long size = 0;
   void *data = read_file(path, &size);
   Music_Emu *emu = NULL;
   gme_err_t e;

   if (!data)
   {
      snprintf(err, errlen, "cannot read %s", path);
      return NULL;
   }
   e = gme_open_data(data, size, &emu, rate);
   free(data);
   if (e)
   {
      snprintf(err, errlen, "%s: %s", path, e);
      return NULL;
   }
   return emu;
}

static bool open_gme(px_source *s, const char *path, unsigned subtrack, bool loop,
      double rate_hint, char *err, size_t errlen)
{
   int rate = (rate_hint >= 8000.0 && rate_hint <= 192000.0) ? (int)(rate_hint + 0.5) : 44100;
   Music_Emu *emu = open_gme_emu(path, rate, err, errlen);
   gme_err_t e;

   if (!emu)
      return false;
   if ((int)subtrack >= gme_track_count(emu))
   {
      snprintf(err, errlen, "%s has %d songs, track=%u requested", path,
            gme_track_count(emu), subtrack + 1);
      gme_delete(emu);
      return false;
   }

   /* Game music loops forever; Proteus decides when a track stops, not tag lengths
    * or silence detection. */
   gme_set_autoload_playback_limit(emu, 0);
   gme_ignore_silence(emu, 1);
   if ((e = gme_start_track(emu, (int)subtrack)))
   {
      snprintf(err, errlen, "%s: %s", path, e);
      gme_delete(emu);
      return false;
   }
   if (!loop)
   {
      gme_info_t *info;
      if (!gme_track_info(emu, &info, (int)subtrack))
      {
         if (info->length > 0)
            gme_set_fade(emu, info->length);
         gme_free_info(info);
      }
   }

   s->kind     = SRC_GME;
   s->u.gme    = emu;
   s->rate     = (unsigned)rate;
   s->channels = 2;
   return true;
}

/* N64 USF rips emulate the whole console, so they render at 44.1 kHz or the output rate,
 * resampled by lazyusf2 from whatever rate the game picks. They loop forever; a song not looped
 * ends after its tagged length, fading out over its tagged fade. */
static bool open_usf(px_source *s, const char *path, bool loop, double rate_hint, char *err, size_t errlen)
{
   px_usf *u = px_usf_open(path, err, errlen);
   if (!u)
      return false;
   s->kind     = SRC_USF;
   s->channels = 2;
   s->rate     = rate_hint >= 8000 && rate_hint <= 192000 ? (unsigned)rate_hint : 44100;
   s->u.usf.player = u;
   if (!loop && px_usf_length_ms(u))
   {
      s->u.usf.fade = px_usf_fade_ms(u) * s->rate / 1000;
      s->u.usf.end  = (px_usf_length_ms(u) + px_usf_fade_ms(u)) * s->rate / 1000;
   }
   return true;
}

static size_t read_usf(px_source *s, int16_t *out, size_t frames)
{
   uint64_t pos = s->u.usf.position, end = s->u.usf.end, fade = s->u.usf.fade;
   if (end)
   {
      if (pos >= end)
         return 0;
      if (frames > end - pos)
         frames = (size_t)(end - pos);
   }
   if (!px_usf_render(s->u.usf.player, out, frames, s->rate))
      return 0;
   if (end && fade && pos + frames > end - fade)
      for (size_t i = 0; i < frames; i++)
      {
         uint64_t at = pos + i;
         if (at + fade > end)
         {
            float g = (float)(end - at) / (float)fade;
            out[i * 2]     = (int16_t)(out[i * 2] * g);
            out[i * 2 + 1] = (int16_t)(out[i * 2 + 1] * g);
         }
      }
   s->u.usf.position = pos + frames;
   return frames;
}

/* Renders forward to `frame`, from the start when it lies behind. */
static bool seek_usf(px_source *s, uint64_t frame)
{
   int16_t scratch[1024 * 2];
   if (frame < s->u.usf.position)
   {
      px_usf_restart(s->u.usf.player);
      s->u.usf.position = 0;
   }
   while (s->u.usf.position < frame)
   {
      uint64_t left = frame - s->u.usf.position;
      size_t n = left > 1024 ? 1024 : (size_t)left;
      if (!px_usf_render(s->u.usf.player, scratch, n, s->rate))
         return false;
      s->u.usf.position += n;
   }
   return true;
}

/* RSN soundtracks (RAR archives of .spc files): song N is the Nth .spc by name, unpacked into
 * memory and played by libgme like a lone .spc. A song not looped ends after its ID666 length,
 * fading out over its tagged fade. */
static bool open_rsn(px_source *s, const char *path, unsigned subtrack, bool loop,
      double rate_hint, char *err, size_t errlen)
{
   int rate = (rate_hint >= 8000.0 && rate_hint <= 192000.0) ? (int)(rate_hint + 0.5) : 44100;
   size_t size = 0;
   char name[256];
   void *spc = px_rsn_extract(path, subtrack, &size, name, sizeof(name), err, errlen);
   Music_Emu *emu = NULL;
   gme_err_t e;

   if (!spc)
      return false;
   e = gme_open_data(spc, (long)size, &emu, rate);
   free(spc);
   if (!e)
   {
      gme_set_autoload_playback_limit(emu, 0);
      gme_ignore_silence(emu, 1);
      e = gme_start_track(emu, 0);
   }
   if (e)
   {
      snprintf(err, errlen, "%s: %s: %s", path, name, e);
      gme_delete(emu);
      return false;
   }
   if (!loop)
   {
      gme_info_t *info;
      if (!gme_track_info(emu, &info, 0))
      {
         if (info->length > 0)
            gme_set_fade_msecs(emu, info->length, info->fade_length > 0 ? info->fade_length : 8000);
         gme_free_info(info);
      }
   }
   s->kind     = SRC_GME;
   s->u.gme    = emu;
   s->rate     = (unsigned)rate;
   s->channels = 2;
/* GBA GSF rips emulate the console as USF rips do: rendered at the output rate (44.1 kHz by
 * default), looping forever, or ending after the tagged length and fade when not looped. */
static bool open_gsf(px_source *s, const char *path, bool loop, double rate_hint, char *err, size_t errlen)
{
   unsigned rate = rate_hint >= 8000 && rate_hint <= 192000 ? (unsigned)rate_hint : 44100;
   px_gsf *g = px_gsf_open(path, rate, err, errlen);
   if (!g)
      return false;
   s->kind     = SRC_GSF;
   s->channels = 2;
   s->rate     = rate;
   s->u.gsf.player = g;
   if (!loop && px_gsf_length_ms(g))
   {
      s->u.gsf.fade = px_gsf_fade_ms(g) * s->rate / 1000;
      s->u.gsf.end  = (px_gsf_length_ms(g) + px_gsf_fade_ms(g)) * s->rate / 1000;
/* Nintendo DS rips. 2SF emulates the whole console (vio2sf), at 44.1 kHz only; NCSF plays the
 * game's sequences (SSEQPlayer) at the output rate. Both loop forever; a song not looped ends
 * after its tagged length, fading out over its tagged fade, as USF songs do. */
static bool open_nds(px_source *s, const char *path, bool loop, double rate_hint, char *err, size_t errlen)
{
   uint64_t length, fade;
   s->kind     = SRC_NDS;
   s->channels = 2;
   if (px_2sf_path(path))
   {
      if (!(s->u.nds.twosf = px_2sf_open(path, err, errlen)))
         return false;
      s->rate = PX_2SF_RATE;
      length  = px_2sf_length_ms(s->u.nds.twosf);
      fade    = px_2sf_fade_ms(s->u.nds.twosf);
   }
   else
   {
      s->rate = rate_hint >= 8000 && rate_hint <= 192000 ? (unsigned)rate_hint : 44100;
      if (!(s->u.nds.ncsf = px_ncsf_open(path, s->rate, err, errlen)))
         return false;
      length = px_ncsf_length_ms(s->u.nds.ncsf);
      fade   = px_ncsf_fade_ms(s->u.nds.ncsf);
   }
   if (!loop && length)
   {
      s->u.nds.fade = fade * s->rate / 1000;
      s->u.nds.end  = (length + fade) * s->rate / 1000;
/* SNES SNSF rips emulate the whole console like USF rips: rendered at the output rate (44.1 kHz
 * when none is given), resampled from the S-DSP's 32 kHz. They loop forever; a song not looped
 * ends after its tagged length, fading out over its tagged fade. */
static bool open_snsf(px_source *s, const char *path, bool loop, double rate_hint, char *err, size_t errlen)
{
   px_snsf *p = px_snsf_open(path, err, errlen);
   if (!p)
      return false;
   s->kind     = SRC_SNSF;
   s->channels = 2;
   s->rate     = rate_hint >= 8000 && rate_hint <= 192000 ? (unsigned)rate_hint : 44100;
   s->u.snsf.player = p;
   if (!loop && px_snsf_length_ms(p))
   {
      s->u.snsf.fade = px_snsf_fade_ms(p) * s->rate / 1000;
      s->u.snsf.end  = (px_snsf_length_ms(p) + px_snsf_fade_ms(p)) * s->rate / 1000;
   }
   return true;
}

static size_t read_gsf(px_source *s, int16_t *out, size_t frames)
{
   uint64_t pos = s->u.gsf.position, end = s->u.gsf.end, fade = s->u.gsf.fade;
static bool render_nds(px_source *s, int16_t *out, size_t frames)
{
   return s->u.nds.twosf ? px_2sf_render(s->u.nds.twosf, out, frames)
                         : px_ncsf_render(s->u.nds.ncsf, out, frames);
}

static size_t read_nds(px_source *s, int16_t *out, size_t frames)
{
   uint64_t pos = s->u.nds.position, end = s->u.nds.end, fade = s->u.nds.fade;
static size_t read_snsf(px_source *s, int16_t *out, size_t frames)
{
   uint64_t pos = s->u.snsf.position, end = s->u.snsf.end, fade = s->u.snsf.fade;
   if (end)
   {
      if (pos >= end)
         return 0;
      if (frames > end - pos)
         frames = (size_t)(end - pos);
   }
   if (!px_gsf_render(s->u.gsf.player, out, frames))
   if (!render_nds(s, out, frames))
   if (!px_snsf_render(s->u.snsf.player, out, frames, s->rate))
      return 0;
   if (end && fade && pos + frames > end - fade)
      for (size_t i = 0; i < frames; i++)
      {
         uint64_t at = pos + i;
         if (at + fade > end)
         {
            float g = (float)(end - at) / (float)fade;
            out[i * 2]     = (int16_t)(out[i * 2] * g);
            out[i * 2 + 1] = (int16_t)(out[i * 2 + 1] * g);
         }
      }
   s->u.gsf.position = pos + frames;
   return frames;
}

/* Renders forward to `frame`, from power-on when it lies behind. */
static bool seek_gsf(px_source *s, uint64_t frame)
{
   int16_t scratch[1024 * 2];
   if (frame < s->u.gsf.position)
   {
      s->u.gsf.position = 0;
      if (!px_gsf_restart(s->u.gsf.player))
         return false;
   }
   while (s->u.gsf.position < frame)
   {
      uint64_t left = frame - s->u.gsf.position;
      size_t n = left > 1024 ? 1024 : (size_t)left;
      if (!px_gsf_render(s->u.gsf.player, scratch, n))
         return false;
      s->u.gsf.position += n;
   s->u.nds.position = pos + frames;
   s->u.snsf.position = pos + frames;
   return frames;
}

/* Renders forward to `frame`, from the start when it lies behind. */
static bool seek_nds(px_source *s, uint64_t frame)
{
   int16_t scratch[1024 * 2];
   if (frame < s->u.nds.position)
   {
      if (!(s->u.nds.twosf ? px_2sf_restart(s->u.nds.twosf) : px_ncsf_restart(s->u.nds.ncsf)))
         return false;
      s->u.nds.position = 0;
   }
   while (s->u.nds.position < frame)
   {
      uint64_t left = frame - s->u.nds.position;
      size_t n = left > 1024 ? 1024 : (size_t)left;
      if (!render_nds(s, scratch, n))
         return false;
      s->u.nds.position += n;
static bool seek_snsf(px_source *s, uint64_t frame)
{
   int16_t scratch[1024 * 2];
   if (frame < s->u.snsf.position)
   {
      px_snsf_restart(s->u.snsf.player);
      s->u.snsf.position = 0;
   }
   while (s->u.snsf.position < frame)
   {
      uint64_t left = frame - s->u.snsf.position;
      size_t n = left > 1024 ? 1024 : (size_t)left;
      if (!px_snsf_render(s->u.snsf.player, scratch, n, s->rate))
         return false;
      s->u.snsf.position += n;
   }
   return true;
}

static bool is_gme_path(const char *path)
{
   return gme_identify_extension(path) != NULL;
}

bool px_source_supported(const char *path)
{
   return ext_is(path, "wav") || ext_is(path, "mp3") || ext_is(path, "ogg") || is_gme_path(path) || px_usf_path(path)
      || px_rsn_path(path)
       || px_gsf_path(path)
      || px_2sf_path(path) || px_ncsf_path(path);
   if (px_snsf_path(path))
      return true;
   return ext_is(path, "wav") || ext_is(path, "mp3") || ext_is(path, "ogg") || is_gme_path(path) || px_usf_path(path);
}

unsigned px_source_song_count(const char *path)
{
   char err[256];
   Music_Emu *emu;
   int count;

   if (px_rsn_path(path))
      return px_rsn_count(path);
   if (!is_gme_path(path))
      return px_file_exists(path) ? 1 : 0;
   if (!(emu = open_gme_emu(path, 44100, err, sizeof(err))))
      return 0;
   count = gme_track_count(emu);
   gme_delete(emu);
   return count > 0 ? (unsigned)count : 0;
}

px_source *px_source_open(const char *path, unsigned subtrack, bool loop, double rate_hint,
      char *err, size_t errlen)
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

   snprintf(err, errlen, "unsupported or corrupt audio file: %s", path);
   if (ext_is(path, "wav"))
      ok = open_wav(s, path);
   else if (ext_is(path, "mp3"))
      ok = open_mp3(s, path);
   else if (ext_is(path, "ogg"))
      ok = open_ogg(s, path);
   else if (is_gme_path(path))
      ok = open_gme(s, path, subtrack, loop, rate_hint, err, errlen);
   else if (px_usf_path(path))
      ok = open_usf(s, path, loop, rate_hint, err, errlen);
   else if (px_rsn_path(path))
      ok = open_rsn(s, path, subtrack, loop, rate_hint, err, errlen);
   else if (px_gsf_path(path))
      ok = open_gsf(s, path, loop, rate_hint, err, errlen);
   else if (px_2sf_path(path) || px_ncsf_path(path))
      ok = open_nds(s, path, loop, rate_hint, err, errlen);
   else if (px_snsf_path(path))
      ok = open_snsf(s, path, loop, rate_hint, err, errlen);
   else
      ok = open_ogg(s, path) || open_mp3(s, path) || open_wav(s, path);

   if (!ok || s->rate == 0 || s->channels == 0)
   {
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
   if (s->kind == SRC_GME)
   {
      if (gme_track_ended(s->u.gme) || gme_play(s->u.gme, (int)(frames * 2), out))
         return 0;
      return frames;
   }
   if (s->kind == SRC_USF)
      return read_usf(s, out, frames);
   if (s->kind == SRC_GSF)
      return read_gsf(s, out, frames);
   if (s->kind == SRC_NDS)
      return read_nds(s, out, frames);
   if (s->kind == SRC_SNSF)
      return read_snsf(s, out, frames);

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
      case SRC_GME:
         /* libgme renders forward to the target, restarting the song to go back. */
         return frame <= 0x3FFFFFFFu && !gme_seek_samples(s->u.gme, (int)(frame * 2));
      case SRC_USF:
         return seek_usf(s, frame);
      case SRC_GSF:
         return seek_gsf(s, frame);
      case SRC_NDS:
         return seek_nds(s, frame);
      case SRC_SNSF:
         return seek_snsf(s, frame);
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
      case SRC_GME:
         gme_delete(s->u.gme);
         break;
      case SRC_USF:
         px_usf_close(s->u.usf.player);
         break;
      case SRC_GSF:
         px_gsf_close(s->u.gsf.player);
      case SRC_NDS:
         px_2sf_close(s->u.nds.twosf);
         px_ncsf_close(s->u.nds.ncsf);
      case SRC_SNSF:
         px_snsf_close(s->u.snsf.player);
         break;
   }
   free(s->scratch);
   free(s);
}
