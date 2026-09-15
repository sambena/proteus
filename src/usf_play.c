/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usf_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psflib/psflib.h"
#include "lazyusf2/usf/usf.h"
#include "util.h"

/* The PSF version byte of USF files. */
#define USF_VERSION 0x21

struct px_usf
{
   void *state;           /* lazyusf2's emulator */
   bool compare, fifo_full;
   uint64_t length_ms, fade_ms;
   char title[128];
};

static void *file_open(void *context, const char *path) { (void)context; return px_fopen(path, "rb"); }
static size_t file_read(void *buffer, size_t size, size_t count, void *handle) { return fread(buffer, size, count, (FILE*)handle); }
static int file_seek(void *handle, int64_t offset, int whence) { return fseek((FILE*)handle, (long)offset, whence) ? -1 : 0; }
static int file_close(void *handle) { return fclose((FILE*)handle) ? -1 : 0; }
static long file_tell(void *handle) { return ftell((FILE*)handle); }

static const psf_file_callbacks kFiles = { "\\/", NULL, file_open, file_read, file_seek, file_close, file_tell };

/* USF files carry no program section: each file's reserved section holds ROM pages and a save state. */
static int upload(void *context, const uint8_t *exe, size_t exe_size, const uint8_t *reserved, size_t reserved_size)
{
   px_usf *u = (px_usf*)context;
   if (exe && exe_size)
      return -1;
   return usf_upload_section(u->state, reserved, reserved_size);
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
   px_usf *u = (px_usf*)context;
   if (!strcasecmp(name, "_enablecompare") && *value)
      u->compare = true;
   else if (!strcasecmp(name, "_enablefifofull") && *value)
      u->fifo_full = true;
   else if (!strcasecmp(name, "length"))
      u->length_ms = parse_time(value);
   else if (!strcasecmp(name, "fade"))
      u->fade_ms = parse_time(value);
   else if (!strcasecmp(name, "title"))
      snprintf(u->title, sizeof(u->title), "%s", value);
   return 0;
}

px_usf *px_usf_open(const char *path, char *err, size_t errlen)
{
   px_usf *u = (px_usf*)calloc(1, sizeof(*u));
   if (!u || !(u->state = malloc(usf_get_state_size())))
   {
      free(u);
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   usf_clear(u->state);
   /* High-level audio: fast, and what USF sets are ripped and checked with. */
   usf_set_hle_audio(u->state, 1);
   if (psf_load(path, &kFiles, USF_VERSION, upload, u, tag, u, 0, NULL, NULL) < 0)
   {
      snprintf(err, errlen, "not a USF rip, or its .usflib is missing: %s", path);
      px_usf_close(u);
      return NULL;
   }
   usf_set_compare(u->state, u->compare);
   usf_set_fifo_full(u->state, u->fifo_full);
   /* Starting the emulator (one block of samples, then back to the start) catches rips that load
    * but cannot play. */
   {
      int32_t rate = 0;
      const char *failed = usf_render(u->state, NULL, 0, &rate);
      if (failed || rate <= 0)
      {
         snprintf(err, errlen, "cannot play %s: %s", path, failed ? failed : "no audio");
         px_usf_close(u);
         return NULL;
      }
      usf_restart(u->state);
   }
   return u;
}

bool px_usf_render(px_usf *u, int16_t *out, size_t frames, unsigned rate)
{
   return usf_render_resampled(u->state, out, frames, (int32_t)rate) == NULL;
}

void px_usf_restart(px_usf *u)
{
   usf_restart(u->state);
}

uint64_t px_usf_length_ms(const px_usf *u) { return u->length_ms; }
uint64_t px_usf_fade_ms(const px_usf *u) { return u->fade_ms; }
const char *px_usf_title(const px_usf *u) { return u->title; }

void px_usf_close(px_usf *u)
{
   if (!u)
      return;
   if (u->state)
   {
      usf_shutdown(u->state);
      free(u->state);
   }
   free(u);
}

bool px_usf_path(const char *path)
{
   const char *ext = px_path_ext(path);
   return !strcasecmp(ext, "usf") || !strcasecmp(ext, "miniusf");
}
