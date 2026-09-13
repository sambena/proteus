/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROTEUS_OPTIONS_H
#define PROTEUS_OPTIONS_H

#include <stdbool.h>

#include "libretro.h"
#include "profile.h"

/* Proteus settings shown in RetroArch's Quick Menu > Core Options, merged with the
 * wrapped core's own options. */
#define PX_OPT_ENABLED      "proteus_enabled"
#define PX_OPT_MUSIC_VOLUME "proteus_music_volume"
#define PX_OPT_GAME_VOLUME  "proteus_game_volume"
#define PX_OPT_CROSSFADE    "proteus_crossfade"
#define PX_OPT_NOTIFY       "proteus_notify"
#define PX_OPT_SONG_FMT     "proteus_song_%X"

/* Option value meaning "use what the profile says". */
#define PX_OPT_PROFILE      "profile"

void px_options_set_frontend(retro_environment_t env);
/* If `cmd` declares core options, remembers the inner core's options, publishes
 * them merged with Proteus's own, stores the frontend's answer in `*result` and
 * returns true. */
bool px_options_intercept(unsigned cmd, void *data, bool *result);
bool px_options_inner_declared(void);
/* Rebuilds Proteus's options for a game; `profile` (may be NULL) adds one song
 * picker per mapped song. Takes effect on the next publish. */
void px_options_set_profile(const px_profile *profile);
/* Publishes the inner core's options merged with Proteus's own. */
bool px_options_publish(void);
/* Current value of an option, or NULL. */
const char *px_options_get(const char *key);
void px_options_free(void);

#endif
