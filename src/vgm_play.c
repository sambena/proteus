/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vgmstream behind px_source, set up as Hyrule Chronicle's audio engine sets it up: 16-bit
 * output folded to stereo, two loops then a ten-second fade unless looping, and subsong N+1 of a
 * container for song N. vgmstream opens files itself (and a stream's companions, such as the
 * right half of a "_L"/"_R" pair); on Windows it is built with VGM_STDIO_UNICODE so UTF-8 paths
 * open there too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvgmstream.h"
#include "libvgmstream_streamfile.h"

#include "vgm_play.h"
#include "util.h"

struct px_vgm
{
   libvgmstream_t *lib;
   int16_t *tmp;          /* vgmstream's frames before folding to stereo (mono streams) */
   size_t tmp_frames;
};

/* Nintendo's streams and containers, and CRI's, all decoded by vgmstream itself. Left out are
 * formats that need an external codec in this build (Switch Opus, KTSS, AHX, Ogg, AT9...) and
 * extensions other px_source players claim. */
static const char *const kExts[] = {
   "dsp", "idsp", "mdsp", "adp", "ast", "hps", "thp", "bns",   /* GameCube and Wii */
   "brstm", "brwav", "rwar",                                   /* Wii NW4R */
   "bcstm", "bcwav", "bfstm", "bfwav", "bwav",                 /* 3DS, Wii U, Switch */
   "strm", "swav",                                             /* DS */
   "adx", "aax", "hca",                                        /* CRI */
};

bool px_vgm_path(const char *path)
{
   const char *e = px_path_ext(path);
   for (size_t i = 0; i < sizeof(kExts) / sizeof(kExts[0]); i++)
   {
      const char *a = e, *b = kExts[i];
      while (*a && *b && (*a | 0x20) == *b)
         a++, b++;
      if (!*a && !*b)
         return true;
   }
   return false;
}

static libvgmstream_t *open_lib(const char *path, int subsong, bool loop)
{
   libvgmstream_config_t cfg;
   libstreamfile_t *sf;
   libvgmstream_t *lib;

   memset(&cfg, 0, sizeof(cfg));
   cfg.loop_count            = 2.0;
   cfg.fade_time             = 10.0;
   cfg.allow_play_forever    = loop;
   cfg.play_forever          = loop;
   cfg.auto_downmix_channels = 2;
   cfg.force_sfmt            = LIBVGMSTREAM_SFMT_PCM16;

   if (!(sf = libstreamfile_open_from_stdio(path)))
      return NULL;
   lib = libvgmstream_create(sf, subsong, &cfg);
   libstreamfile_close(sf);
   return lib;
}

px_vgm *px_vgm_open(const char *path, unsigned subsong, bool loop, char *err, size_t errlen)
{
   px_vgm *v;
   libvgmstream_t *lib;

   if (subsong >= 0x7FFFFFFFu)
   {
      snprintf(err, errlen, "%s: no song %u", path, subsong + 1);
      return NULL;
   }
   if (!(lib = open_lib(path, (int)subsong + 1, loop)))
   {
      snprintf(err, errlen, "%s: not a stream vgmstream can play (song %u)", path, subsong + 1);
      return NULL;
   }
   /* vgmstream plays a file without subsongs whatever subsong is asked for. */
   if (subsong > 0 && subsong >= (unsigned)(lib->format->subsong_count > 0 ? lib->format->subsong_count : 1))
   {
      snprintf(err, errlen, "%s has %d songs, song %u requested", path,
         lib->format->subsong_count > 0 ? lib->format->subsong_count : 1, subsong + 1);
      libvgmstream_free(lib);
      return NULL;
   }
   if (lib->format->channels < 1 || lib->format->channels > 2 || lib->format->sample_rate <= 0)
   {
      snprintf(err, errlen, "%s: %d channels at %d Hz", path, lib->format->channels, lib->format->sample_rate);
      libvgmstream_free(lib);
      return NULL;
   }
   if (!(v = (px_vgm*)calloc(1, sizeof(*v))))
   {
      snprintf(err, errlen, "out of memory");
      libvgmstream_free(lib);
      return NULL;
   }
   v->lib = lib;
   return v;
}

size_t px_vgm_render(px_vgm *v, int16_t *out, size_t frames)
{
   libvgmstream_t *lib = v->lib;
   size_t done = 0;

   while (done < frames && !lib->decoder->done)
   {
      size_t want = frames - done > 4096 ? 4096 : frames - done;
      int got;
      if (lib->format->channels == 2)
      {
         if (libvgmstream_fill(lib, out + done * 2, (int)want) < 0)
            break;
         got = lib->decoder->buf_samples;
      }
      else
      {
         if (v->tmp_frames < want)
         {
            int16_t *p = (int16_t*)realloc(v->tmp, want * sizeof(int16_t));
            if (!p)
               break;
            v->tmp = p;
            v->tmp_frames = want;
         }
         if (libvgmstream_fill(lib, v->tmp, (int)want) < 0)
            break;
         got = lib->decoder->buf_samples;
         for (int i = 0; i < got; i++)
            out[(done + i) * 2] = out[(done + i) * 2 + 1] = v->tmp[i];
      }
      if (got <= 0)
         break;
      done += (size_t)got;
   }
   return done;
}

bool px_vgm_seek(px_vgm *v, uint64_t frame)
{
   if (frame > (uint64_t)INT64_MAX)
      return false;
   libvgmstream_seek(v->lib, (int64_t)frame);
   return true;
}

unsigned px_vgm_rate(const px_vgm *v)
{
   return (unsigned)v->lib->format->sample_rate;
}

uint64_t px_vgm_length(const px_vgm *v)
{
   const libvgmstream_format_t *f = v->lib->format;
   return f->play_forever || f->play_samples <= 0 ? 0 : (uint64_t)f->play_samples;
}

void px_vgm_close(px_vgm *v)
{
   if (!v)
      return;
   libvgmstream_free(v->lib);
   free(v->tmp);
   free(v);
}

/* "name_R.ext" whose "name_L.ext" (or "_l") is beside it: the left half opens as the pair. */
static bool right_half_of_pair(const char *path)
{
   const char *ext = px_path_ext(path);   /* "" (not in `path`) when there is none */
   size_t base = *ext ? (size_t)(ext - path) : 0;
   char partner[1024];

   if (base < 4 || path[base - 1] != '.' || path[base - 3] != '_' || (path[base - 2] | 0x20) != 'r'
         || strlen(path) >= sizeof(partner))
      return false;
   strcpy(partner, path);
   partner[base - 2] = path[base - 2] == 'R' ? 'L' : 'l';
   return px_file_exists(partner);
}

unsigned px_vgm_count(const char *path)
{
   libvgmstream_t *lib = open_lib(path, 0, false);
   unsigned count;

   if (!lib)
      return 0;
   count = lib->format->subsong_count > 0 ? (unsigned)lib->format->subsong_count : 1;
   /* Hidden only when it opens in stereo, as a half vgmstream paired does. */
   if (count == 1 && lib->format->channels == 2 && right_half_of_pair(path))
      count = 0;
   libvgmstream_free(lib);
   return count;
}
