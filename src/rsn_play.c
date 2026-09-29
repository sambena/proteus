/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RSN soundtracks: a RAR archive (RAR 1.5 to 5) of .spc files, read whole into memory and
 * unpacked by dmc_unrar, which is built without stdio or Win32 so it stays portable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMC_UNRAR_HEADER_FILE_ONLY
#define DMC_UNRAR_DISABLE_STDIO 1
#define DMC_UNRAR_DISABLE_WIN32 1
#include "dmc_unrar/dmc_unrar.c"

#include "rsn_play.h"
#include "util.h"

#define RSN_MAX_FILE  (64 * 1024 * 1024)
#define RSN_MAX_SPC   (1024 * 1024)
#define RSN_NAME_MAX  256

typedef struct
{
   dmc_unrar_size_t index;
   char name[RSN_NAME_MAX];
} rsn_entry;

typedef struct
{
   void *data;               /* the archive; dmc_unrar reads from it, so it outlives `ar` */
   dmc_unrar_archive ar;
   rsn_entry *songs;         /* the .spc entries in name order */
   unsigned count;
} rsn;

bool px_rsn_path(const char *path)
{
   const char *e = px_path_ext(path);
   return (e[0] | 0x20) == 'r' && (e[1] | 0x20) == 's' && (e[2] | 0x20) == 'n' && !e[3];
}

static bool spc_name(const char *name)
{
   size_t n = strlen(name);
   return n > 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 's'
      && (name[n - 2] | 0x20) == 'p' && (name[n - 1] | 0x20) == 'c';
}

/* Case-insensitive, so "Track 10" and "track 2" sort as a file browser shows them. */
static int entry_cmp(const void *a, const void *b)
{
   const char *x = ((const rsn_entry*)a)->name, *y = ((const rsn_entry*)b)->name;
   for (; *x && *y; x++, y++)
   {
      int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x;
      int cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
      if (cx != cy)
         return cx - cy;
   }
   if (*x || *y)
      return *x ? 1 : -1;
   return strcmp(((const rsn_entry*)a)->name, ((const rsn_entry*)b)->name);
}

static void rsn_close(rsn *r)
{
   dmc_unrar_archive_close(&r->ar);
   free(r->songs);
   free(r->data);
}

static bool rsn_open(rsn *r, const char *path, char *err, size_t errlen)
{
   FILE *f = px_fopen(path, "rb");
   long size = 0;
   dmc_unrar_return e;
   dmc_unrar_size_t files;

   memset(r, 0, sizeof(*r));
   if (!f)
   {
      snprintf(err, errlen, "cannot read %s", path);
      return false;
   }
   if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size <= RSN_MAX_FILE
         && fseek(f, 0, SEEK_SET) == 0 && (r->data = malloc((size_t)size))
         && fread(r->data, 1, (size_t)size, f) != (size_t)size)
   {
      free(r->data);
      r->data = NULL;
   }
   fclose(f);
   if (!r->data)
   {
      snprintf(err, errlen, "cannot read %s", path);
      return false;
   }

   if ((e = dmc_unrar_archive_init(&r->ar)) != DMC_UNRAR_OK
         || (e = dmc_unrar_archive_open_mem(&r->ar, r->data, (dmc_unrar_size_t)size)) != DMC_UNRAR_OK)
   {
      snprintf(err, errlen, "%s: %s", path, dmc_unrar_strerror(e));
      free(r->data);
      r->data = NULL;
      return false;
   }

   files = dmc_unrar_get_file_count(&r->ar);
   if (files && !(r->songs = (rsn_entry*)calloc((size_t)files, sizeof(*r->songs))))
   {
      snprintf(err, errlen, "out of memory");
      rsn_close(r);
      return false;
   }
   for (dmc_unrar_size_t i = 0; i < files; i++)
   {
      rsn_entry *s = &r->songs[r->count];
      if (dmc_unrar_file_is_directory(&r->ar, i)
            || !dmc_unrar_get_filename(&r->ar, i, s->name, sizeof(s->name))
            || !spc_name(s->name))
         continue;
      s->index = i;
      r->count++;
   }
   qsort(r->songs, r->count, sizeof(*r->songs), entry_cmp);
   if (!r->count)
   {
      snprintf(err, errlen, "%s holds no .spc files", path);
      rsn_close(r);
      return false;
   }
   return true;
}

unsigned px_rsn_count(const char *path)
{
   char err[256];
   rsn r;
   unsigned n;
   if (!rsn_open(&r, path, err, sizeof(err)))
      return 0;
   n = r.count;
   rsn_close(&r);
   return n;
}

void *px_rsn_extract(const char *path, unsigned index, size_t *size, char *name, size_t namelen,
      char *err, size_t errlen)
{
   rsn r;
   void *buf = NULL;
   dmc_unrar_size_t len = 0;
   const dmc_unrar_file *st;
   dmc_unrar_return e;

   if (!rsn_open(&r, path, err, errlen))
      return NULL;
   if (index >= r.count)
   {
      snprintf(err, errlen, "%s has %u songs, track=%u requested", path, r.count, index + 1);
      rsn_close(&r);
      return NULL;
   }
   st = dmc_unrar_get_file_stat(&r.ar, r.songs[index].index);
   if (!st || st->uncompressed_size == 0 || st->uncompressed_size > RSN_MAX_SPC)
   {
      snprintf(err, errlen, "%s: %s is not a valid SPC", path, r.songs[index].name);
      rsn_close(&r);
      return NULL;
   }
   e = dmc_unrar_extract_file_to_heap(&r.ar, r.songs[index].index, &buf, &len, true);
   if (e != DMC_UNRAR_OK)
   {
      snprintf(err, errlen, "%s: %s: %s", path, r.songs[index].name, dmc_unrar_strerror(e));
      free(buf);
      buf = NULL;
   }
   else
   {
      *size = (size_t)len;
      if (name && namelen)
         snprintf(name, namelen, "%s", r.songs[index].name);
   }
   rsn_close(&r);
   return buf;
}
