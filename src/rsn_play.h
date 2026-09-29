/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RSN soundtracks: RAR archives of SNES .spc files (the SNESmusic.org set format),
 * unpacked in memory by dmc_unrar. Each .spc entry, sorted by name, is one song. */
#ifndef PROTEUS_RSN_PLAY_H
#define PROTEUS_RSN_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* True for .rsn. */
bool px_rsn_path(const char *path);
/* The number of .spc entries in the archive; 0 when it cannot be read. */
unsigned px_rsn_count(const char *path);
/* Unpacks song `index` (0-based, in name order) into a malloc'd buffer, NULL on error.
 * `name`, when not NULL, receives the entry's file name. */
void *px_rsn_extract(const char *path, unsigned index, size_t *size, char *name, size_t namelen,
      char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
