/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "snsf_play.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psflib/psflib.h"
#include "lakesnes/snes/snes.h"
#include "util.h"

/* The PSF version byte of SNSF files. */
#define SNSF_VERSION 0x23
/* The largest ROM image a rip may build (the SNES address space holds 8 MB at most). */
#define SNSF_MAX_ROM 0x800000u
#define SNSF_MAX_SRAM 0x20000u
/* The S-DSP's output rate. */
#define DSP_RATE 32040.0

/* Windowed-sinc resampler from the S-DSP rate to the output rate. */
#define RS_TAPS 16
#define RS_HALF (RS_TAPS / 2)
#define RS_PHASES 256
#define RS_CAP 8192

enum { MAP_AUTO, MAP_LOROM, MAP_HIROM };
enum { VIDEO_AUTO, VIDEO_NTSC, VIDEO_PAL };

struct px_snsf
{
   Snes *snes;
   /* The ROM image the files build, and the SRAM their reserved sections fill. */
   uint8_t *image;
   size_t image_size;
   bool base_set;
   uint32_t base;
   uint8_t *sram;
   size_t sram_used;
   /* The console the image is played on. */
   uint8_t *rom;
   uint32_t rom_size, ram_size;
   int cart_type;
   bool pal;
   /* Tags: from the song first, then its libraries. */
   int map, video, sram_fill;
   uint64_t length_ms, fade_ms;
   char title[128];
   /* The resampler: source frames buffered as floats, `pos` the next output's source position. */
   uint16_t dsp_read;
   float buf[RS_CAP * 2];
   size_t len;
   double pos;
   unsigned kernel_rate;
   double step;
   float kernel[(RS_PHASES + 1) * RS_TAPS];
};

static void *file_open(void *context, const char *path) { (void)context; return px_fopen(path, "rb"); }
static size_t file_read(void *buffer, size_t size, size_t count, void *handle) { return fread(buffer, size, count, (FILE*)handle); }
static int file_seek(void *handle, int64_t offset, int whence) { return fseek((FILE*)handle, (long)offset, whence) ? -1 : 0; }
static int file_close(void *handle) { return fclose((FILE*)handle) ? -1 : 0; }
static long file_tell(void *handle) { return ftell((FILE*)handle); }

static const psf_file_callbacks kFiles = { "\\/", NULL, file_open, file_read, file_seek, file_close, file_tell };

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Each file's program section is a piece of ROM: offset, size, bytes. The first file loaded sets a
 * base that later files' offsets are relative to. Its reserved section holds blocks of type, size
 * and data; type 0 is SRAM (offset, bytes). */
static int upload(void *context, const uint8_t *exe, size_t exe_size, const uint8_t *reserved, size_t reserved_size)
{
   px_snsf *p = (px_snsf*)context;
   size_t at = 0;

   while (reserved && at + 8 <= reserved_size)
   {
      uint32_t type = le32(reserved + at), size = le32(reserved + at + 4);
      if (type == 0xFFFFFFFFu)
         break;
      if (size > reserved_size - at - 8)
         return -1;
      if (type == 0 && size > 4)
      {
         uint32_t offset = le32(reserved + at + 8);
         if (!p->sram)
         {
            if (!(p->sram = (uint8_t*)malloc(SNSF_MAX_SRAM)))
               return -1;
            memset(p->sram, p->sram_fill >= 0 ? p->sram_fill : 0xFF, SNSF_MAX_SRAM);
         }
         if (offset < SNSF_MAX_SRAM)
         {
            size_t n = size - 4;
            if (n > SNSF_MAX_SRAM - offset)
               n = SNSF_MAX_SRAM - offset;
            memcpy(p->sram + offset, reserved + at + 12, n);
            if (offset + n > p->sram_used)
               p->sram_used = offset + n;
         }
      }
      at += 8 + (size_t)size;
   }

   if (!exe || !exe_size)
      return 0;
   if (exe_size < 8)
      return -1;
   {
      uint32_t offset = le32(exe), size = le32(exe + 4);
      if (size > exe_size - 8)
         return -1;
      if (!p->base_set)
      {
         p->base_set = true;
         p->base     = offset;
      }
      else
         offset += p->base;
      offset &= 0x1FFFFFFF;
      if (offset > SNSF_MAX_ROM || size > SNSF_MAX_ROM - offset)
         return -1;
      if (offset + size > p->image_size)
      {
         uint8_t *grown = (uint8_t*)realloc(p->image, offset + size);
         if (!grown)
            return -1;
         memset(grown + p->image_size, 0, offset + size - p->image_size);
         p->image      = grown;
         p->image_size = offset + size;
      }
      memcpy(p->image + offset, exe + 8, size);
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

/* Tags of the song and its libraries, the song's first: the first setting of each wins. */
static int tag_nested(void *context, const char *name, const char *value)
{
   px_snsf *p = (px_snsf*)context;
   if (!strcasecmp(name, "_memory") && p->map == MAP_AUTO)
   {
      if (!strcasecmp(value, "LoROM"))
         p->map = MAP_LOROM;
      else if (!strcasecmp(value, "HiROM"))
         p->map = MAP_HIROM;
   }
   else if (!strcasecmp(name, "_video") && p->video == VIDEO_AUTO)
   {
      if (!strcasecmp(value, "NTSC"))
         p->video = VIDEO_NTSC;
      else if (!strcasecmp(value, "PAL"))
         p->video = VIDEO_PAL;
   }
   else if (!strcasecmp(name, "_sramfill") && p->sram_fill < 0 && *value)
      p->sram_fill = (int)(strtol(value, NULL, 0) & 0xFF);
   return 0;
}

/* The song's own tags. */
static int tag_song(void *context, const char *name, const char *value)
{
   px_snsf *p = (px_snsf*)context;
   if (!strcasecmp(name, "length"))
      p->length_ms = parse_time(value);
   else if (!strcasecmp(name, "fade"))
      p->fade_ms = parse_time(value);
   else if (!strcasecmp(name, "title"))
      snprintf(p->title, sizeof(p->title), "%s", value);
   return 0;
}

/* How much a cartridge header at `at` looks like one (after LakeSnes's scoring, MIT). */
static int header_score(const uint8_t *d, size_t len, size_t at)
{
   int score = 0;
   uint16_t checksum, complement, reset;
   size_t op;
   uint8_t opcode;

   if (at + 0x40 > len)
      return -1000;
   score += ((d[at + 0x15] >> 4) == 2 || (d[at + 0x15] >> 4) == 3) ? 5 : -4;
   score += ((d[at + 0x15] & 0xF) <= 3 || (d[at + 0x15] & 0xF) == 5) ? 5 : -2;
   score += ((d[at + 0x16] >> 4) <= 5 || (d[at + 0x16] >> 4) >= 0xE) ? 5 : -2;
   {
      uint8_t chips = d[at + 0x16] & 0xF;
      score += (chips <= 6 || chips == 9 || chips == 0xA) ? 5 : -2;
   }
   score += d[at + 0x19] <= 0x14 ? 5 : -2;
   complement = d[at + 0x1C] | (d[at + 0x1D] << 8);
   checksum   = d[at + 0x1E] | (d[at + 0x1F] << 8);
   score += (uint16_t)(checksum + complement) == 0xFFFF ? 8 : -6;
   reset = d[at + 0x3C] | (d[at + 0x3D] << 8);
   score += reset >= 0x8000 ? 8 : -20;
   op = at + 0x40 - 0x8000 + (reset & 0x7FFF);
   if (op >= len)
      return score - 14;
   opcode = d[op];
   if (opcode == 0x78 || opcode == 0x18)
      score += 6;
   if (opcode == 0x4C || opcode == 0x5C || opcode == 0x9C)
      score += 3;
   if (opcode == 0x00 || opcode == 0xFF || opcode == 0xDB)
      score -= 6;
   return score;
}

/* Picks the mapping, region and SRAM size, and builds the ROM LakeSnes maps: a power of two in
 * size, its tail mirrored as a cartridge's would be. */
static bool build_cart(px_snsf *p, char *err, size_t errlen)
{
   size_t header;
   uint32_t size = 0x8000;

   if (!p->image_size)
   {
      snprintf(err, errlen, "the rip holds no ROM");
      return false;
   }
   if (p->map == MAP_LOROM)
      p->cart_type = 1;
   else if (p->map == MAP_HIROM)
      p->cart_type = p->image_size > 0x400000 ? 3 : 2;
   else
   {
      int lo = header_score(p->image, p->image_size, 0x7FC0);
      int hi = header_score(p->image, p->image_size, 0xFFC0);
      int ex = p->image_size >= 0x410000 ? header_score(p->image, p->image_size, 0x40FFC0) : -1000;
      p->cart_type = ex >= hi && ex > lo ? 3 : hi > lo ? 2 : 1;
   }
   header = p->cart_type == 1 ? 0x7FC0 : p->cart_type == 2 ? 0xFFC0 : 0x40FFC0;

   if (p->video != VIDEO_AUTO)
      p->pal = p->video == VIDEO_PAL;
   else if (header + 0x20 <= p->image_size)
   {
      uint8_t region = p->image[header + 0x19];
      p->pal = (region >= 0x2 && region <= 0xC) || region == 0x11;
   }

   /* SRAM as the header sizes it, as Snes9x maps it: none when the size byte is 0. */
   p->ram_size = 0;
   if (header + 0x20 <= p->image_size)
   {
      uint8_t shift = p->image[header + 0x18];
      p->ram_size = shift && shift <= 7 ? 0x400u << shift : shift ? SNSF_MAX_SRAM : 0;
   }
   /* SRAM the rip fills but the header leaves out is mapped anyway. */
   while (p->sram_used > p->ram_size)
      p->ram_size = p->ram_size ? p->ram_size * 2 : 0x800;

   while (size < p->image_size)
      size *= 2;
   free(p->rom);
   if (!(p->rom = (uint8_t*)malloc(size)))
   {
      snprintf(err, errlen, "out of memory");
      return false;
   }
   memcpy(p->rom, p->image, p->image_size);
   {
      size_t length = p->image_size, test = 1;
      while (length != size)
      {
         if (length & test)
         {
            memcpy(p->rom + length, p->rom + length - test, test);
            length += test;
         }
         test *= 2;
      }
   }
   p->rom_size = size;
   return true;
}

/* Powers the console on with the rip's ROM and SRAM in place. */
static void power_on(px_snsf *p)
{
   Snes *snes = p->snes;
   cart_load(snes->cart, p->cart_type, p->rom, (int)p->rom_size, (int)p->ram_size, false);
   if (snes->cart->ram)
   {
      memset(snes->cart->ram, p->sram_fill >= 0 ? p->sram_fill : 0xFF, p->ram_size);
      if (p->sram)
         memcpy(snes->cart->ram, p->sram, p->ram_size < SNSF_MAX_SRAM ? p->ram_size : SNSF_MAX_SRAM);
   }
   /* Work RAM as Snes9x, which rips are made and checked with, leaves it. */
   snes->ramFill = 0x55;
   snes_reset(snes, true);
   snes->palTiming = p->pal;
   p->dsp_read = snes->apu->dsp->sampleOffset;
   /* The resampler starts on silence, centred on its first source frame. */
   memset(p->buf, 0, sizeof(float) * 2 * (RS_HALF - 1));
   p->len = RS_HALF - 1;
   p->pos = RS_HALF - 1;
}

/* Runs one video frame and queues the S-DSP samples it made; returns how many. */
static size_t run_frame(px_snsf *p)
{
   Dsp *dsp = p->snes->apu->dsp;
   uint16_t avail;
   size_t n = 0;

   snes_runFrame(p->snes);
   avail = (uint16_t)(dsp->sampleOffset - p->dsp_read);
   if (avail > 0x800)
   {
      p->dsp_read = (uint16_t)(dsp->sampleOffset - 0x800);
      avail = 0x800;
   }
   for (; n < avail && p->len < RS_CAP; n++, p->dsp_read++, p->len++)
   {
      p->buf[p->len * 2]     = dsp->sampleBuffer[(p->dsp_read & 0x7FF) * 2];
      p->buf[p->len * 2 + 1] = dsp->sampleBuffer[(p->dsp_read & 0x7FF) * 2 + 1];
   }
   return n;
}

static void build_kernel(px_snsf *p, unsigned rate)
{
   const double pi = 3.14159265358979323846;
   double ratio = rate / DSP_RATE, fc = 0.5 * (ratio < 1 ? ratio : 1) * 0.94;
   for (int ph = 0; ph <= RS_PHASES; ph++)
   {
      float *row = p->kernel + ph * RS_TAPS;
      double sum = 0;
      for (int j = 0; j < RS_TAPS; j++)
      {
         double x = (double)ph / RS_PHASES + (RS_HALF - 1) - j;
         double s = x == 0 ? 2 * fc : sin(2 * pi * fc * x) / (pi * x);
         double w = 0.42 + 0.5 * cos(pi * x / RS_HALF) + 0.08 * cos(2 * pi * x / RS_HALF);
         row[j] = (float)(s * w);
         sum += row[j];
      }
      for (int j = 0; j < RS_TAPS; j++)
         row[j] = (float)(row[j] / sum);
   }
   p->kernel_rate = rate;
   p->step = DSP_RATE / rate;
}

static int16_t clamp16(float v)
{
   return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)lrintf(v);
}

bool px_snsf_render(px_snsf *p, int16_t *out, size_t frames, unsigned rate)
{
   if (!rate)
      return false;
   if (rate != p->kernel_rate)
      build_kernel(p, rate);
   for (size_t f = 0; f < frames; f++)
   {
      size_t i = (size_t)p->pos;
      int empty = 0;
      /* Taps reach from i - (HALF - 1) to i + HALF. */
      while (i + RS_HALF >= p->len)
      {
         size_t first = i - (RS_HALF - 1);
         if (first > 0)
         {
            memmove(p->buf, p->buf + first * 2, (p->len - first) * 2 * sizeof(float));
            p->len -= first;
            p->pos -= (double)first;
            i      -= first;
         }
         if (!run_frame(p) && ++empty > 64)
            return false;
      }
      {
         double at = (p->pos - (double)i) * RS_PHASES;
         int ph = (int)at;
         float t = (float)(at - ph);
         const float *k0 = p->kernel + ph * RS_TAPS, *k1 = k0 + RS_TAPS;
         const float *src = p->buf + (i - (RS_HALF - 1)) * 2;
         float l = 0, r = 0;
         for (int j = 0; j < RS_TAPS; j++)
         {
            float k = k0[j] + (k1[j] - k0[j]) * t;
            l += src[j * 2] * k;
            r += src[j * 2 + 1] * k;
         }
         out[f * 2]     = clamp16(l);
         out[f * 2 + 1] = clamp16(r);
      }
      p->pos += p->step;
   }
   return true;
}

px_snsf *px_snsf_open(const char *path, char *err, size_t errlen)
{
   px_snsf *p = (px_snsf*)calloc(1, sizeof(*p));
   if (!p)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   p->sram_fill = -1;
   if (psf_load(path, &kFiles, SNSF_VERSION, upload, p, tag_nested, p, 1, NULL, NULL) < 0)
   {
      snprintf(err, errlen, "not an SNSF rip, or its .snsflib is missing: %s", path);
      px_snsf_close(p);
      return NULL;
   }
   psf_load(path, &kFiles, SNSF_VERSION, NULL, NULL, tag_song, p, 0, NULL, NULL);
   if (!build_cart(p, err, errlen))
   {
      px_snsf_close(p);
      return NULL;
   }
   free(p->image);
   p->image = NULL;
   if (!(p->snes = snes_init()))
   {
      snprintf(err, errlen, "out of memory");
      px_snsf_close(p);
      return NULL;
   }
   power_on(p);
   /* A few frames catch rips that load but never start the sound CPU's clock. */
   {
      size_t made = 0;
      for (int n = 0; n < 4; n++)
         made += run_frame(p);
      if (!made)
      {
         snprintf(err, errlen, "cannot play %s: no audio", path);
         px_snsf_close(p);
         return NULL;
      }
      power_on(p);
   }
   return p;
}

void px_snsf_restart(px_snsf *p)
{
   power_on(p);
}

uint64_t px_snsf_length_ms(const px_snsf *p) { return p->length_ms; }
uint64_t px_snsf_fade_ms(const px_snsf *p) { return p->fade_ms; }
const char *px_snsf_title(const px_snsf *p) { return p->title; }

void px_snsf_close(px_snsf *p)
{
   if (!p)
      return;
   if (p->snes)
      snes_free(p->snes);
   free(p->image);
   free(p->sram);
   free(p->rom);
   free(p);
}

bool px_snsf_path(const char *path)
{
   const char *ext = px_path_ext(path);
   return !strcasecmp(ext, "snsf") || !strcasecmp(ext, "minisnsf");
}
