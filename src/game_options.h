/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RetroArch's per-game core options files (Quick Menu > Core Options > Save Game Options).
 * The DSP plugin cannot change a core's options while it runs, so the profile's [mute]
 * options are saved there instead and apply from the next time the game is loaded. */
#ifndef PROTEUS_GAME_OPTIONS_H
#define PROTEUS_GAME_OPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* <config dir>/<core library name>/<content name>.opt, where the content name is the file
 * without its extension, or for "dir/archive.zip#game.sfc" the archive without its extension,
 * as RetroArch names it. */
void px_game_options_path(const char *config_dir, const char *library_name, const char *content_path,
      char *out, size_t n);

/* Sets `count` options in the game options file at `path`. A new file starts as a copy of
 * `global_path` (the core's own <library name>.opt, may be NULL) so the game keeps the other
 * option values it had. Returns how many values changed (0 when all were set already), or -1
 * when the file cannot be written. */
int px_game_options_set(const char *path, const char *global_path, const char *const *keys,
      const char *const *values, unsigned count);

#ifdef __cplusplus
}
#endif

#endif
