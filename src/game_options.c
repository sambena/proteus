/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_options.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

void px_game_options_path(const char *config_dir, const char *library_name, const char *content_path,
      char *out, size_t n)
{
   char path[PX_PATH_MAX], name[PX_PATH_MAX];
   char *hash, *dot;
   const char *base = content_path;

   if (!content_path || !*content_path)
   {
      if (n)
         *out = '\0';
      return;
   }
   for (const char *p = content_path; *p; p++)
      if (*p == '/' || *p == '\\')
         base = p + 1;
   snprintf(path, sizeof(path), "%s", content_path);
   /* An archive entry is named after the archive. */
   if ((hash = strchr(path, '#')))
   {
      *hash = '\0';
      base = path;
      for (const char *p = path; *p; p++)
         if (*p == '/' || *p == '\\')
            base = p + 1;
   }
   snprintf(name, sizeof(name), "%s", base);
   if ((hash = strchr(name, '#')))
      *hash = '\0';
   if ((dot = strrchr(name, '.')))
      *dot = '\0';
   snprintf(out, n, "%s/%s/%s.opt", config_dir, library_name, name);
}

static char *read_all(const char *path)
{
   FILE *f = path ? px_fopen(path, "rb") : NULL;
   char *buf;
   long len;

   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = (char*)malloc((size_t)(len > 0 ? len : 0) + 1);
   if (buf)
      buf[len > 0 ? fread(buf, 1, (size_t)len, f) : 0] = '\0';
   fclose(f);
   return buf;
}

static void make_parent_dir(const char *path)
{
   char dir[PX_PATH_MAX];
   px_path_dir(path, dir, sizeof(dir));
#ifdef _WIN32
   wchar_t *w = px_utf8_to_wide(dir);
   if (w)
   {
      CreateDirectoryW(w, NULL);
      free(w);
   }
#else
   mkdir(dir, 0755);
#endif
}

/* Appends to a growing string. */
static bool append(char **out, size_t *len, size_t *cap, const char *s, size_t n)
{
   if (*len + n + 1 > *cap)
   {
      size_t c = (*cap ? *cap * 2 : 4096) + n;
      char *p = (char*)realloc(*out, c);
      if (!p)
         return false;
      *out = p;
      *cap = c;
   }
   memcpy(*out + *len, s, n);
   *len += n;
   (*out)[*len] = '\0';
   return true;
}

int px_game_options_set(const char *path, const char *global_path, const char *const *keys,
      const char *const *values, unsigned count)
{
   char *text = read_all(path);
   char *out = NULL;
   size_t len = 0, cap = 0;
   bool *seen = (bool*)calloc(count ? count : 1, sizeof(bool));
   int changed = 0;
   FILE *f;

   if (!text)
   {
      text = read_all(global_path);
      changed = -1; /* a new file is written even when the copied values already match */
   }
   if (!seen)
   {
      free(text);
      return -1;
   }

   /* Lines look like: key = "value" */
   for (char *line = text; line && *line;)
   {
      char *end = strchr(line, '\n');
      size_t line_len = end ? (size_t)(end - line + 1) : strlen(line);
      bool replaced = false;

      for (unsigned i = 0; i < count; i++)
      {
         size_t k = strlen(keys[i]);
         if (!strncmp(line, keys[i], k) && (line[k] == ' ' || line[k] == '='))
         {
            char want[512];
            int w = snprintf(want, sizeof(want), "%s = \"%s\"\n", keys[i], values[i]);
            seen[i] = true;
            if (line_len < (size_t)w - 1 || strncmp(line, want, (size_t)w - 1))
            {
               append(&out, &len, &cap, want, (size_t)w);
               if (changed >= 0)
                  changed++;
               replaced = true;
            }
            break;
         }
      }
      if (!replaced)
         append(&out, &len, &cap, line, line_len);
      line = end ? end + 1 : NULL;
   }
   if (len && out[len - 1] != '\n')
      append(&out, &len, &cap, "\n", 1);
   for (unsigned i = 0; i < count; i++)
      if (!seen[i])
      {
         char want[512];
         int w = snprintf(want, sizeof(want), "%s = \"%s\"\n", keys[i], values[i]);
         append(&out, &len, &cap, want, (size_t)w);
         if (changed >= 0)
            changed++;
      }
   free(text);
   free(seen);

   if (changed == 0)
   {
      free(out);
      return 0;
   }
   make_parent_dir(path);
   if (!(f = px_fopen(path, "wb")))
   {
      free(out);
      return -1;
   }
   if (len)
      fwrite(out, 1, len, f);
   fclose(f);
   free(out);
   return changed < 0 ? (int)count : changed;
}
