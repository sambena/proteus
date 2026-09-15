/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The music swapping logic shared by the wrapper core and the DSP plugin:
 * song detection, choosing what to play, and mixing. */
#ifndef PROTEUS_ENGINE_H
#define PROTEUS_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libretro.h"
#include "music.h"
#include "profile.h"

typedef struct
{
   void *userdata;
   /* Core memory region `id` (RETRO_MEMORY_*); NULL when unavailable. */
   const uint8_t *(*memory)(void *userdata, unsigned id, size_t *size);
   /* Current value of a core option, or NULL. Optional. */
   const char *(*option)(void *userdata, const char *key);
   void (*log)(void *userdata, enum retro_log_level level, const char *msg);
   /* Shows a short on-screen message. Optional. */
   void (*notify)(void *userdata, const char *msg);
   /* The original music should be muted (or unmuted). Optional. */
   void (*mute)(void *userdata, bool muted);
   /* Applies a [patch] cheat code, or removes it (NULL). Optional. */
   void (*patch)(void *userdata, const char *code);
   /* The running core's file name ("fceumm_libretro.dll"), to pick its [patch]. Optional. */
   const char *(*core_file)(void *userdata);
} px_host;

typedef struct
{
   px_host host;
   px_profile profile;
   px_mixer mixer;

   /* Effective settings: the profile, overridden by core options. */
   struct
   {
      bool enabled;
      float music_volume;
      float game_volume;
      unsigned crossfade_ms;
      bool notify;
   } cfg;

   bool muted;
   bool patched;   /* the core has a cheat from the profile */
   bool have_candidate;
   uint32_t candidate;
   unsigned stable_frames;
   bool have_applied;
   uint32_t applied;
   bool warned_memory;

   /* [silence]: frames left in which a song value change comes from the silence request
    * just written, and that value, which counts as the song applied. */
   unsigned silencing;
   bool have_silenced;
   uint32_t silenced;
   /* A command register read zero since the song was applied: a command for that song again is
    * the game starting it again. */
   bool idle;
   /* [hold] writes are applied; `hold_saved` has what each address held before them. */
   bool holding;
   uint32_t hold_saved[PX_MAX_HOLD];
} px_engine;

#define PX_ENGINE_STATE_SIZE 48u

void px_engine_init(px_engine *e, const px_host *host);
/* Looks for "<content dir>/<name>.proteus.ini", then "<system dir>/proteus/<name>.ini".
 * For "archive.zip#game.sfc" the name is the game's, then the archive's; for a bare .zip
 * (RetroArch's history), the archive's, then each game's inside it. */
bool px_engine_find_profile(const char *content_path, const char *system_dir, char *out, size_t n);
/* Resets all state and loads a profile; logs and returns false on errors. */
bool px_engine_load(px_engine *e, const char *profile_path);
void px_engine_unload(px_engine *e);
bool px_engine_loaded(const px_engine *e);

void px_engine_set_rate(px_engine *e, double rate);
/* Re-reads core options; call after loading and whenever options change. */
void px_engine_read_config(px_engine *e);
/* Applies changed core options to what is playing. */
void px_engine_options_changed(px_engine *e);
/* Reads the song address and reacts to changes; call once per frame. */
void px_engine_frame(px_engine *e);
void px_engine_reset(px_engine *e);

/* True when mixing should happen (a profile is loaded and replacement is enabled). */
bool px_engine_mixing(const px_engine *e);
void px_engine_mix_s16(px_engine *e, int16_t *frames, size_t count);
void px_engine_mix_float(px_engine *e, float *frames, size_t count);

/* The value a [mute] core option is forced to, or NULL. */
const char *px_engine_mute_override(const px_engine *e, const char *key);

void px_engine_save_state(const px_engine *e, uint8_t *block);
bool px_engine_is_state(const uint8_t *block);
bool px_engine_load_state(px_engine *e, const uint8_t *block);

#ifdef __cplusplus
}
#endif

#endif
