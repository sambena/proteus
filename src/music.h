/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROTEUS_MUSIC_H
#define PROTEUS_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

/* A decoded or emulated audio stream, always delivered as interleaved stereo int16. */
typedef struct px_source px_source;

/* `subtrack` selects a song inside multi-song files (NSF, GBS, ...). Emulated
 * formats render at `rate_hint` when it is sensible, so they need no resampling.
 * Non-looping emulated tracks end after their tagged length. */
px_source *px_source_open(const char *path, unsigned subtrack, bool loop, double rate_hint,
      char *err, size_t errlen);
/* Reads up to `frames` stereo frames; returns 0 at end of stream. */
size_t px_source_read(px_source *s, int16_t *out, size_t frames);
bool px_source_seek(px_source *s, uint64_t frame);
unsigned px_source_rate(const px_source *s);
void px_source_close(px_source *s);

/* True for extensions Proteus can play. */
bool px_source_supported(const char *path);
/* Number of songs in a multi-song file, 1 for everything else, 0 if unreadable. */
unsigned px_source_song_count(const char *path);

#define PX_VOICE_BUFFER 1024

typedef struct
{
   px_source *src;
   char path[PX_PATH_MAX];
   unsigned subtrack;
   bool loop;
   uint64_t loop_start;
   float volume;

   int16_t buf[PX_VOICE_BUFFER * 2];
   size_t buf_len, buf_pos;
   uint64_t cursor;     /* source frame index of the next frame to be pulled */
   bool ended;

   /* linear resampler: output lies between frames a and b at fraction t */
   float a[2], b[2];
   double t;

   float gain, gain_step;
} px_voice;

typedef struct
{
   double out_rate;
   px_voice current;
   px_voice fading;     /* previous track fading out during a crossfade */
} px_mixer;

void px_mixer_init(px_mixer *m);
void px_mixer_set_rate(px_mixer *m, double out_rate);
/* Starts a track as the current one, crossfading from whatever played before. */
bool px_mixer_play(px_mixer *m, const char *path, unsigned subtrack, bool loop,
      uint64_t loop_start, float volume, unsigned fade_frames, uint64_t start_frame,
      char *err, size_t errlen);
void px_mixer_stop(px_mixer *m, unsigned fade_frames);
/* Adds music to `frames` (interleaved stereo) after scaling the game audio. */
void px_mixer_mix(px_mixer *m, int16_t *frames, size_t count, float game_gain, float music_gain);
/* Same, for float audio in the -1..1 range. */
void px_mixer_mix_float(px_mixer *m, float *frames, size_t count, float game_gain, float music_gain);
bool px_mixer_is_playing(const px_mixer *m, const char *path, unsigned subtrack);
uint64_t px_mixer_position(const px_mixer *m);
unsigned px_mixer_source_rate(const px_mixer *m);
bool px_mixer_seek(px_mixer *m, uint64_t frame);
void px_mixer_free(px_mixer *m);

#ifdef __cplusplus
}
#endif

#endif
