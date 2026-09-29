/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GBA GSF rips: psflib gathers the ROM image from the song and its .gsflib files, and viogsf's
 * GBA core (VBA-M's ARM7 interpreter and sound, no video) runs it and hands back its audio. */
#include "gsf_play.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psflib/psflib.h"
#include "viogsf/vbam/gba/GBA.h"
#include "viogsf/vbam/gba/Sound.h"
#include "util.h"

/* The PSF version byte of GSF files. */
#define GSF_VERSION 0x22
/* The GBA's 16.78 MHz clock: CPU ticks per emulator slice, about 15 ms. */
#define GSF_SLICE 250000
/* Slices without a sample before a rip is taken for dead (about 15 s of silence-less output). */
#define GSF_STALL 1000

/* Collects what the emulator writes, one slice at a time. */
struct GsfOut : public GBASoundOut
{
   int16_t *buf;
   size_t frames, cap;   /* stereo frames held and room for */
   bool oom;

   GsfOut() : buf(NULL), frames(0), cap(0), oom(false) { }
   ~GsfOut() { free(buf); }
   void write(const void *samples, unsigned long bytes)
   {
      size_t n = bytes / (2 * sizeof(int16_t));
      if (frames + n > cap)
      {
         size_t want = (frames + n) * 2;
         int16_t *p = (int16_t*)realloc(buf, want * 2 * sizeof(int16_t));
         if (!p)
         {
            oom = true;
            return;
         }
         buf = p;
         cap = want;
      }
      memcpy(buf + frames * 2, samples, n * 2 * sizeof(int16_t));
      frames += n;
   }
};

struct px_gsf
{
   GBASystem *gba;
   GsfOut out;
   size_t read;           /* frames of `out` already handed on */
   unsigned rate;
   uint8_t *rom;          /* the ROM image the files build, from offset 0 of its region */
   size_t rom_size;
   uint32_t entry;        /* 0x2000000 (multiboot, work RAM) or 0x8000000 (cartridge) */
   bool have_entry;
   uint64_t length_ms, fade_ms;
   char title[128];
};

static void *file_open(void *context, const char *path) { (void)context; return px_fopen(path, "rb"); }
static size_t file_read(void *buffer, size_t size, size_t count, void *handle) { return fread(buffer, size, count, (FILE*)handle); }
static int file_seek(void *handle, int64_t offset, int whence) { return fseek((FILE*)handle, (long)offset, whence) ? -1 : 0; }
static int file_close(void *handle) { return fclose((FILE*)handle) ? -1 : 0; }
static long file_tell(void *handle) { return ftell((FILE*)handle); }

static const psf_file_callbacks kFiles = { "\\/", NULL, file_open, file_read, file_seek, file_close, file_tell };

static uint32_t le32(const uint8_t *p)
{
   return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Each file's program section: entry point, load address and size, then that many bytes of ROM.
 * Libraries load first, so the song's own section lands on top of theirs. */
static int upload(void *context, const uint8_t *exe, size_t exe_size, const uint8_t *reserved, size_t reserved_size)
{
   px_gsf *g = (px_gsf*)context;
   (void)reserved;
   (void)reserved_size;
   if (!exe || exe_size < 12)
      return -1;
   uint32_t entry = le32(exe), offset = le32(exe + 4) & 0x1FFFFFF, size = le32(exe + 8);
   if (size > exe_size - 12 || (uint64_t)offset + size > 0x2000000)
      return -1;
   if (!g->have_entry)
   {
      g->entry      = entry;
      g->have_entry = true;
   }
   if (offset + size > g->rom_size)
   {
      uint8_t *p = (uint8_t*)realloc(g->rom, offset + size);
      if (!p)
         return -1;
      memset(p + g->rom_size, 0, offset + size - g->rom_size);
      g->rom      = p;
      g->rom_size = offset + size;
   }
   memcpy(g->rom + offset, exe + 12, size);
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
   px_gsf *g = (px_gsf*)context;
   if (!strcasecmp(name, "length"))
      g->length_ms = parse_time(value);
   else if (!strcasecmp(name, "fade"))
      g->fade_ms = parse_time(value);
   else if (!strcasecmp(name, "title"))
      snprintf(g->title, sizeof(g->title), "%s", value);
   return 0;
}

static void stop(px_gsf *g)
{
   if (!g->gba)
      return;
   soundShutdown(g->gba);
   CPUCleanUp(g->gba);
   delete g->gba;
   g->gba = NULL;
}

/* A fresh console with the ROM loaded, as at power-on. */
static bool start(px_gsf *g)
{
   stop(g);
   g->out.frames = 0;
   g->read       = 0;
   g->gba = new (std::nothrow) GBASystem();
   if (!g->gba)
      return false;
   g->gba->cpuIsMultiBoot = (g->entry >> 24) == 2;
   if (!CPULoadRom(g->gba, g->rom, (u32)g->rom_size))
   {
      stop(g);
      return false;
   }
   soundInit(g->gba, &g->out);
   soundSetSampleRate(g->gba, (long)g->rate);
   soundReset(g->gba);
   CPUInit(g->gba);
   CPUReset(g->gba);
   return true;
}

px_gsf *px_gsf_open(const char *path, unsigned rate, char *err, size_t errlen)
{
   px_gsf *g = new (std::nothrow) px_gsf();
   if (!g)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   g->rate = rate >= 8000 && rate <= 192000 ? rate : 44100;
   if (psf_load(path, &kFiles, GSF_VERSION, upload, g, tag, g, 0, NULL, NULL) < 0 || !g->rom_size)
   {
      snprintf(err, errlen, "not a GSF rip, or its .gsflib is missing: %s", path);
      px_gsf_close(g);
      return NULL;
   }
   /* Running the console until its first sound catches rips that load but cannot play. */
   {
      int16_t probe[2];
      if (!start(g) || !px_gsf_render(g, probe, 1) || !start(g))
      {
         snprintf(err, errlen, "cannot play %s: no audio", path);
         px_gsf_close(g);
         return NULL;
      }
   }
   return g;
}

bool px_gsf_render(px_gsf *g, int16_t *out, size_t frames)
{
   if (!g->gba)
      return false;
   while (frames)
   {
      if (g->read == g->out.frames)
      {
         g->out.frames = 0;
         g->read       = 0;
         for (int i = 0; !g->out.frames; i++)
         {
            if (i == GSF_STALL || g->out.oom)
               return false;
            CPULoop(g->gba, GSF_SLICE);
         }
      }
      size_t n = g->out.frames - g->read;
      if (n > frames)
         n = frames;
      memcpy(out, g->out.buf + g->read * 2, n * 2 * sizeof(int16_t));
      out    += n * 2;
      frames -= n;
      g->read += n;
   }
   return true;
}

bool px_gsf_restart(px_gsf *g)
{
   return start(g);
}

uint64_t px_gsf_length_ms(const px_gsf *g) { return g->length_ms; }
uint64_t px_gsf_fade_ms(const px_gsf *g) { return g->fade_ms; }
const char *px_gsf_title(const px_gsf *g) { return g->title; }

void px_gsf_close(px_gsf *g)
{
   if (!g)
      return;
   stop(g);
   free(g->rom);
   delete g;
}

bool px_gsf_path(const char *path)
{
   const char *ext = px_path_ext(path);
   return !strcasecmp(ext, "gsf") || !strcasecmp(ext, "minigsf");
}
