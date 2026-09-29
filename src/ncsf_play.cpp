/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ncsf_play.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <strings.h>

#include "psflib/psflib.h"
#include "sseqplayer/Player.h"
#include "sseqplayer/SDAT.h"
#include "util.h"

/* The PSF version byte of NCSF files. */
#define NCSF_VERSION 0x25

struct px_ncsf
{
   uint32_t sseq_index = 0;          /* which sequence of the SDAT the song plays */
   std::vector<uint8_t> sdat_data;   /* the SDAT, as the rip and its libraries map it */
   std::unique_ptr<SDAT> sdat;
   std::unique_ptr<Player> player;
   std::vector<uint8_t> buffer;      /* SSEQPlayer renders little-endian bytes */
   unsigned rate = 44100;
   uint64_t length_ms = 0, fade_ms = 0;
   char title[128] = "";
};

extern "C" {
static void *file_open(void *context, const char *path) { (void)context; return px_fopen(path, "rb"); }
static size_t file_read(void *buffer, size_t size, size_t count, void *handle) { return fread(buffer, size, count, (FILE*)handle); }
static int file_seek(void *handle, int64_t offset, int whence) { return fseek((FILE*)handle, (long)offset, whence) ? -1 : 0; }
static int file_close(void *handle) { return fclose((FILE*)handle) ? -1 : 0; }
static long file_tell(void *handle) { return ftell((FILE*)handle); }
}

static const psf_file_callbacks kFiles = { "\\/", NULL, file_open, file_read, file_seek, file_close, file_tell };

static uint32_t le32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The program section is an SDAT (its size at offset 8), laid over what the libraries loaded; the
 * reserved section names the sequence to play. */
static int upload(void *context, const uint8_t *exe, size_t exe_size, const uint8_t *reserved, size_t reserved_size)
{
   px_ncsf *n = static_cast<px_ncsf*>(context);
   if (reserved_size >= 4)
      n->sseq_index = le32(reserved);
   if (exe_size >= 12)
   {
      uint32_t size = le32(exe + 8);
      if (size > exe_size || size < 16)
         return -1;
      if (n->sdat_data.size() < size)
         n->sdat_data.resize(size, 0);
      memcpy(&n->sdat_data[0], exe, size);
   }
   return 0;
}

/* "2:25", "1:02:03.5" or "10" (seconds): milliseconds, or 0 when unreadable. */
static uint64_t parse_time(const char *s)
{
   double total = 0, part = 0, scale = 0;
   bool any = false;
   for (; *s; s++)
   {
      if (*s >= '0' && *s <= '9')
      {
         if (scale > 0)
         {
            part += (*s - '0') * scale;
            scale /= 10;
         }
         else
            part = part * 10 + (*s - '0');
         any = true;
      }
      else if (*s == ':')
      {
         total = (total + part) * 60;
         part  = 0;
         scale = 0;
      }
      else if (*s == '.' || *s == ',')
         scale = 0.1;
      else if (*s != ' ')
         return 0;
   }
   return any ? (uint64_t)((total + part) * 1000.0 + 0.5) : 0;
}

static int tag(void *context, const char *name, const char *value)
{
   px_ncsf *n = static_cast<px_ncsf*>(context);
   if (!strcasecmp(name, "length"))
      n->length_ms = parse_time(value);
   else if (!strcasecmp(name, "fade"))
      n->fade_ms = parse_time(value);
   else if (!strcasecmp(name, "title"))
      snprintf(n->title, sizeof(n->title), "%s", value);
   return 0;
}

/* A fresh player on the song's sequence: the song from its start. */
static bool start(px_ncsf *n)
{
   const SSEQ *sseq = n->sdat->sseq.get();
   std::unique_ptr<Player> p(new (std::nothrow) Player());
   if (!p || !sseq)
      return false;
   p->sseqVol       = Cnv_Scale(sseq->info.vol);
   p->sampleRate    = n->rate;
   p->interpolation = INTERPOLATION_SINC;
   if (!p->Setup(sseq))
      return false;
   p->Timer();
   n->player = std::move(p);
   return true;
}

px_ncsf *px_ncsf_open(const char *path, unsigned rate, char *err, size_t errlen)
{
   px_ncsf *n = new (std::nothrow) px_ncsf();
   if (!n)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   n->rate = rate >= 8000 && rate <= 192000 ? rate : 44100;
   try
   {
      if (psf_load(path, &kFiles, NCSF_VERSION, upload, n, tag, n, 0, NULL, NULL) < 0)
      {
         snprintf(err, errlen, "not an NCSF rip, or its .ncsflib is missing: %s", path);
         delete n;
         return NULL;
      }
      if (n->sdat_data.empty())
      {
         snprintf(err, errlen, "NCSF rip holds no SDAT: %s", path);
         delete n;
         return NULL;
      }
      PseudoFile file;
      file.data = &n->sdat_data;
      n->sdat.reset(new SDAT(file, n->sseq_index));
      if (!start(n))
      {
         snprintf(err, errlen, "cannot play sequence %u of %s", (unsigned)n->sseq_index, path);
         delete n;
         return NULL;
      }
   }
   catch (const std::exception &e)
   {
      /* SSEQPlayer throws on a malformed SDAT. */
      snprintf(err, errlen, "%s: %s", path, e.what());
      delete n;
      return NULL;
   }
   return n;
}

bool px_ncsf_render(px_ncsf *n, int16_t *out, size_t frames)
{
   if (!n->player)
      return false;
   while (frames)
   {
      unsigned count = frames > 4096 ? 4096 : (unsigned)frames;
      if (n->buffer.size() < count * 4u)
         n->buffer.resize(count * 4u);
      n->player->GenerateSamples(n->buffer, 0, count);
      const uint8_t *b = n->buffer.data();
      for (unsigned i = 0; i < count * 2; i++)
         out[i] = (int16_t)(uint16_t)(b[i * 2] | b[i * 2 + 1] << 8);
      out    += count * 2;
      frames -= count;
   }
   return true;
}

bool px_ncsf_restart(px_ncsf *n)
{
   return start(n);
}

uint64_t px_ncsf_length_ms(const px_ncsf *n) { return n->length_ms; }
uint64_t px_ncsf_fade_ms(const px_ncsf *n) { return n->fade_ms; }
const char *px_ncsf_title(const px_ncsf *n) { return n->title; }

void px_ncsf_close(px_ncsf *n)
{
   delete n;
}

bool px_ncsf_path(const char *path)
{
   const char *ext = px_path_ext(path);
   return !strcasecmp(ext, "ncsf") || !strcasecmp(ext, "minincsf");
}
