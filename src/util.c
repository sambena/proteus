/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

wchar_t *px_utf8_to_wide(const char *s)
{
   int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
   wchar_t *w;
   if (len <= 0)
      return NULL;
   w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)len);
   if (w)
      MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
   return w;
}

FILE *px_fopen(const char *path, const char *mode)
{
   wchar_t *wpath = px_utf8_to_wide(path);
   wchar_t *wmode = px_utf8_to_wide(mode);
   FILE *f = NULL;
   if (wpath && wmode)
      f = _wfopen(wpath, wmode);
   free(wpath);
   free(wmode);
   return f;
}
#else
FILE *px_fopen(const char *path, const char *mode)
{
   return fopen(path, mode);
}
#endif

static bool is_sep(char c)
{
   return c == '/' || c == '\\';
}

static const char *last_sep(const char *path)
{
   const char *sep = NULL;
   for (const char *p = path; *p; p++)
      if (is_sep(*p))
         sep = p;
   return sep;
}

void px_path_dir(const char *path, char *out, size_t n)
{
   const char *sep = last_sep(path);
   size_t len = sep ? (size_t)(sep - path) : 0;
   if (!sep)
   {
      snprintf(out, n, ".");
      return;
   }
   if (len >= n)
      len = n - 1;
   memcpy(out, path, len);
   out[len] = '\0';
}

void px_path_stem(const char *path, char *out, size_t n)
{
   const char *sep = last_sep(path);
   const char *name = sep ? sep + 1 : path;
   const char *dot = strrchr(name, '.');
   size_t len = dot ? (size_t)(dot - name) : strlen(name);
   if (len >= n)
      len = n - 1;
   memcpy(out, name, len);
   out[len] = '\0';
}

void px_path_join(const char *dir, const char *name, char *out, size_t n)
{
   bool absolute = is_sep(name[0]) || (name[0] && name[1] == ':');
   if (absolute || !dir || !dir[0])
      snprintf(out, n, "%s", name);
   else
      snprintf(out, n, "%s/%s", dir, name);
}

bool px_file_exists(const char *path)
{
   FILE *f = px_fopen(path, "rb");
   if (!f)
      return false;
   fclose(f);
   return true;
}

const char *px_path_ext(const char *path)
{
   const char *sep = last_sep(path);
   const char *dot = strrchr(sep ? sep : path, '.');
   return dot ? dot + 1 : "";
}

#ifdef _WIN32
bool px_list_files(const char *dir, void (*cb)(const char *name, void *userdata), void *userdata)
{
   char pattern[PX_PATH_MAX + 4];
   wchar_t *wpattern;
   WIN32_FIND_DATAW fd;
   HANDLE h;

   snprintf(pattern, sizeof(pattern), "%s\\*", dir);
   if (!(wpattern = px_utf8_to_wide(pattern)))
      return false;
   h = FindFirstFileW(wpattern, &fd);
   free(wpattern);
   if (h == INVALID_HANDLE_VALUE)
      return false;
   do
   {
      char name[PX_PATH_MAX];
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
         continue;
      if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) > 0)
         cb(name, userdata);
   } while (FindNextFileW(h, &fd));
   FindClose(h);
   return true;
}
#else
#include <dirent.h>
#include <sys/stat.h>

bool px_list_files(const char *dir, void (*cb)(const char *name, void *userdata), void *userdata)
{
   DIR *d = opendir(dir);
   struct dirent *e;
   if (!d)
      return false;
   while ((e = readdir(d)))
   {
      char full[PX_PATH_MAX * 2];
      struct stat st;
      snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
      if (stat(full, &st) == 0 && S_ISREG(st.st_mode))
         cb(e->d_name, userdata);
   }
   closedir(d);
   return true;
}
#endif
