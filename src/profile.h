/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROTEUS_PROFILE_H
#define PROTEUS_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define PX_MAX_TRACKS 256
#define PX_MAX_MUTE   64
#define PX_MAX_LIBRARY 8
#define PX_MAX_PATTERN 16
#define PX_MAX_PATCH  8
#define PX_MAX_HOLD   16

/* The song value while the [song] active flag is clear: the game plays no music. */
#define PX_SONG_STOPPED 0xFFFFFFFFu

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

/* Cheat codes for one core, in its format. A core's lines are joined with '+'. */
#define PX_PATCH_CODE_MAX 1024
typedef struct
{
   char core[64];
   char code[PX_PATCH_CODE_MAX];
} px_patch;

/* A RAM write held every frame while the original music is muted: "0x128B8C = 00000000" (as many
 * bytes as hex digits) or "0x128B60 |= 04", with ", <value>" written back when the mute lifts
 * (otherwise what was there before). */
typedef struct
{
   uint32_t address;
   unsigned size;
   uint32_t value;
   bool or_bits;
   bool has_release;
   uint32_t release;
} px_hold;

typedef struct
{
   bool loaded;
   char path[PX_PATH_MAX];

   /* [song] */
   unsigned memory_id;  /* RETRO_MEMORY_* */
   uint32_t address;
   unsigned size;       /* 1, 2 or 4 bytes, little endian (big endian with byte_order = n64) */
   uint32_t mask;
   /* byte_order = n64: addresses are N64 ones (0x80000000 or physical) in RDRAM that the core keeps
    * as 32-bit words in the host's byte order, as Mupen64Plus does */
   bool n64;
   /* active = <address> & <mask>: while those bits are clear the song value is PX_SONG_STOPPED,
    * which gets the `stopped` action (a sequence player that is not playing) */
   bool active;
   uint32_t active_address;
   uint8_t active_mask;
   px_action stopped;
   unsigned debounce;   /* frames a value must be stable before it counts */
   bool latch;          /* the address is a one-shot command: ignore zero, keep the last song */
   px_action unmapped;
   uint8_t pattern[PX_MAX_PATTERN];
   uint8_t pattern_mask[PX_MAX_PATTERN];
   unsigned pattern_length;
   unsigned pattern_offset;
   /* A second command register, for jingles (Super Mario Bros.: $FC): a command there is song
    * value 0x100 + command, and wins over the song address in the frame it is set. */
   bool events;
   uint32_t events_address;

   /* [mute] core options forced while the original music is muted */
   px_option mute[PX_MAX_MUTE];
   unsigned mute_count;

   /* [patch] code patches that stop the game's own music while a replacement (or silence)
    * plays, keeping its sound effects: cheat codes, one per core, in that core's format
    * ("fceumm = 809D?D0:F0") */
   px_patch patch[PX_MAX_PATCH];
   unsigned patch_count;

   /* [tap] code patches on while the game runs, which make it report its song requests in
    * RAM for the song address to read (a stub in blank ROM that stores what the game asks its
    * sound routine to play) */
   px_patch tap[PX_MAX_PATCH];
   unsigned tap_count;

   /* [silence] a request written to the game's RAM to stop its own music while a replacement
    * (or silence) plays; the sound channels stay on for sound effects */
   bool silence;
   unsigned silence_memory;   /* RETRO_MEMORY_* */
   uint32_t silence_address;
   uint8_t silence_value;

   /* [hold] RAM writes that silence the game's music player while a replacement (or silence) plays */
   px_hold hold[PX_MAX_HOLD];
   unsigned hold_count;

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
/* The code in `patches` for a core ("fceumm" matches fceumm_libretro), or NULL. */
const char *px_profile_patch_for(const px_patch *patches, unsigned count, const char *core);

#ifdef __cplusplus
}
#endif

#endif
