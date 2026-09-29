/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "twosf_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "psflib/psflib.h"
#include "vio2sf/vio2sf_names.h"
#include "vio2sf/desmume/state.h"
#include "util.h"

/* The PSF version byte of 2SF files. */
#define TWOSF_VERSION 0x24
/* The largest ROM or save state image a rip may map (the DS cartridge bus reaches 512 MB, but no
 * sound rip comes near this). */
#define TWOSF_MAX_IMAGE (64u * 1024 * 1024)

struct px_2sf
{
   NDS_state *state;      /* vio2sf's emulator */
   uint8_t *rom, *save;   /* the images the rip maps, kept to restart the song */
   size_t rom_size, save_size;
   int initial_frames, sync_type, clockdown, arm9_clockdown, arm7_clockdown;
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
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Copies a mapped block ([offset][size][bytes]) into an image, growing it as needed. The ROM is
 * kept a power of two in size, as vio2sf masks addresses with it. */
static int map_block(uint8_t **image, size_t *image_size, bool rom, const uint8_t *block, size_t block_size)
{
   uint32_t offset, size;
   size_t need;
   if (block_size < 8)
      return -1;
   offset = le32(block);
   size   = le32(block + 4);
   if (size > block_size - 8 || offset > TWOSF_MAX_IMAGE || size > TWOSF_MAX_IMAGE - offset)
      return -1;
   need = (size_t)offset + size;
   if (rom)
   {
      size_t p = 1;
      while (p < need)
         p <<= 1;
      need = p;
   }
   if (need > *image_size)
   {
      uint8_t *grown = (uint8_t*)realloc(*image, need + 16);
      if (!grown)
         return -1;
      memset(grown + *image_size, 0, need + 16 - *image_size);
      *image      = grown;
      *image_size = need;
   }
   memcpy(*image + offset, block + 8, size);
   return 0;
}

/* The program section maps ROM; the reserved section holds zlib-compressed "SAVE" blocks that
 * map the save state (main memory, as the ripper left it). Libraries load first, so later
 * files overlay them. */
static int upload(void *context, const uint8_t *exe, size_t exe_size, const uint8_t *reserved, size_t reserved_size)
{
   px_2sf *t = (px_2sf*)context;
   size_t pos = 0;
   if (exe_size >= 8 && map_block(&t->rom, &t->rom_size, true, exe, exe_size))
      return -1;
   while (reserved_size >= 12 && pos + 12 <= reserved_size)
   {
      uint32_t tag = le32(reserved + pos), zsize = le32(reserved + pos + 4);
      if (zsize > reserved_size - pos - 12)
         return -1;
      if (tag == 0x45564153) /* "SAVE" */
      {
         /* The block's header gives its own size, so inflate the header first, then the rest. */
         uint8_t head[8];
         uLongf hsize = sizeof(head);
         uint8_t *block;
         uLongf bsize;
         int e = uncompress(head, &hsize, reserved + pos + 12, zsize);
         if ((e != Z_OK && e != Z_BUF_ERROR) || hsize < 8 || le32(head + 4) > TWOSF_MAX_IMAGE)
            return -1;
         bsize = (uLongf)le32(head + 4) + 8;
         if (!(block = (uint8_t*)malloc(bsize)))
            return -1;
         if (uncompress(block, &bsize, reserved + pos + 12, zsize) != Z_OK
               || map_block(&t->save, &t->save_size, false, block, bsize))
         {
            free(block);
            return -1;
         }
         free(block);
      }
      pos += 12 + zsize;
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
   px_2sf *t = (px_2sf*)context;
   if (!strcasecmp(name, "_frames"))
      t->initial_frames = atoi(value);
   else if (!strcasecmp(name, "_clockdown"))
      t->clockdown = atoi(value);
   else if (!strcasecmp(name, "_vio2sf_sync_type"))
      t->sync_type = atoi(value);
   else if (!strcasecmp(name, "_vio2sf_arm9_clockdown_level"))
      t->arm9_clockdown = atoi(value);
   else if (!strcasecmp(name, "_vio2sf_arm7_clockdown_level"))
      t->arm7_clockdown = atoi(value);
   else if (!strcasecmp(name, "length"))
      t->length_ms = parse_time(value);
   else if (!strcasecmp(name, "fade"))
      t->fade_ms = parse_time(value);
   else if (!strcasecmp(name, "title"))
      snprintf(t->title, sizeof(t->title), "%s", value);
   return 0;
}

static void stop(px_2sf *t)
{
   if (t->state)
   {
      state_deinit(t->state);
      free(t->state);
      t->state = NULL;
   }
}

/* A fresh emulator with the rip's ROM and save state loaded: the song from its start. */
static bool start(px_2sf *t)
{
   stop(t);
   if (!(t->state = (NDS_state*)calloc(1, sizeof(NDS_state))))
      return false;
   if (state_init(t->state))
   {
      stop(t);
      return false;
   }
   t->state->dwInterpolation      = 4; /* cubic, vio2sf's usual choice */
   t->state->dwChannelMute        = 0;
   t->state->initial_frames       = t->initial_frames;
   t->state->sync_type            = t->sync_type;
   t->state->arm7_clockdown_level = t->arm7_clockdown ? t->arm7_clockdown : t->clockdown;
   t->state->arm9_clockdown_level = t->arm9_clockdown ? t->arm9_clockdown : t->clockdown;
   if (t->rom)
      state_setrom(t->state, t->rom, (u32)t->rom_size, 0);
   state_loadstate(t->state, t->save, (u32)t->save_size);
   return true;
}

px_2sf *px_2sf_open(const char *path, char *err, size_t errlen)
{
   px_2sf *t = (px_2sf*)calloc(1, sizeof(*t));
   if (!t)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   if (psf_load(path, &kFiles, TWOSF_VERSION, upload, t, tag, t, 0, NULL, NULL) < 0)
   {
      snprintf(err, errlen, "not a 2SF rip, or its .2sflib is missing: %s", path);
      px_2sf_close(t);
      return NULL;
   }
   if (!t->rom && !t->save)
   {
      snprintf(err, errlen, "2SF rip holds no program: %s", path);
      px_2sf_close(t);
      return NULL;
   }
   if (!start(t))
   {
      snprintf(err, errlen, "cannot start the DS emulator for %s", path);
      px_2sf_close(t);
      return NULL;
   }
   return t;
}

bool px_2sf_render(px_2sf *t, int16_t *out, size_t frames)
{
   if (!t->state)
      return false;
   while (frames)
   {
      unsigned n = frames > 4096 ? 4096 : (unsigned)frames;
      state_render(t->state, out, n);
      out    += n * 2;
      frames -= n;
   }
   return true;
}

bool px_2sf_restart(px_2sf *t)
{
   return start(t);
}

uint64_t px_2sf_length_ms(const px_2sf *t) { return t->length_ms; }
uint64_t px_2sf_fade_ms(const px_2sf *t) { return t->fade_ms; }
const char *px_2sf_title(const px_2sf *t) { return t->title; }

void px_2sf_close(px_2sf *t)
{
   if (!t)
      return;
   stop(t);
   free(t->rom);
   free(t->save);
   free(t);
}

bool px_2sf_path(const char *path)
{
   const char *ext = px_path_ext(path);
   return !strcasecmp(ext, "2sf") || !strcasecmp(ext, "mini2sf");
}
