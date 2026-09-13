/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROTEUS_UTIL_H
#define PROTEUS_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

#define PX_PATH_MAX 1024

/* fopen() that accepts UTF-8 paths on every platform (libretro paths are UTF-8). */
FILE *px_fopen(const char *path, const char *mode);

#ifdef _WIN32
#include <wchar.h>
/* Converts a UTF-8 string to a newly allocated wide string. Caller frees. */
wchar_t *px_utf8_to_wide(const char *s);
#endif

/* Copies the directory part of `path` (without trailing separator) into `out`. */
void px_path_dir(const char *path, char *out, size_t n);
/* Copies the file name of `path` without its extension into `out`. */
void px_path_stem(const char *path, char *out, size_t n);
/* Joins `dir` and `name` with a separator unless `name` is already absolute. */
void px_path_join(const char *dir, const char *name, char *out, size_t n);
bool px_file_exists(const char *path);
/* Calls `cb` with the UTF-8 name of each regular file directly inside `dir`. */
bool px_list_files(const char *dir, void (*cb)(const char *name, void *userdata), void *userdata);
const char *px_path_ext(const char *path);

#ifdef __cplusplus
}
#endif

#endif
