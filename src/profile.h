/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROTEUS_PROFILE_H
#define PROTEUS_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define PX_MAX_TRACKS 256
#define PX_MAX_MUTE   64
#define PX_MAX_LIBRARY 8

typedef enum
{
   PX_ACTION_FILE,     /* play a replacement file, original music muted */
   PX_ACTION_SILENCE,  /* original music muted, nothing played */
   PX_ACTION_ORIGINAL, /* original music audible, nothing played */
   PX_ACTION_KEEP      /* leave whatever is currently happening alone */
} px_action;

typedef struct
{
   uint32_t value;
   px_action action;
   char path[PX_PATH_MAX]; /* resolved against the profile's directory */
   unsigned subtrack;      /* 0-based song within multi-song files (NSF, GBS, ...) */
   bool loop;
   uint64_t loop_start;    /* source sample frame the loop jumps back to */
   float volume;           /* 0..1 */
} px_track;

typedef struct
{
   char key[128];
   char value[128];
} px_option;

typedef struct
{
   bool loaded;
   char path[PX_PATH_MAX];

   /* [song] */
   unsigned memory_id;  /* RETRO_MEMORY_* */
   uint32_t address;
   unsigned size;       /* 1, 2 or 4 bytes, little endian */
   uint32_t mask;
   unsigned debounce;   /* frames a value must be stable before it counts */
   px_action unmapped;

   /* [mute] core options forced while the original music is muted */
   px_option mute[PX_MAX_MUTE];
   unsigned mute_count;

   /* [mix] */
   float music_volume;
   float game_volume;
   unsigned crossfade_ms;

   /* [tracks] */
   px_track tracks[PX_MAX_TRACKS];
   unsigned track_count;

   /* [library] folders listed in the RetroArch song pickers */
   char library[PX_MAX_LIBRARY][PX_PATH_MAX];
   unsigned library_count;

   /* [debug] */
   bool log_songs;
} px_profile;

bool px_profile_load(px_profile *p, const char *path, char *err, size_t errlen);
/* Returns the index of the track mapped to `value`, or -1. */
int px_profile_find(const px_profile *p, uint32_t value);

#endif
