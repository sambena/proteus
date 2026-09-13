#ifndef PROTEUS_MUSIC_H
#define PROTEUS_MUSIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A decoded audio stream, always delivered as interleaved stereo int16. */
typedef struct px_source px_source;

px_source *px_source_open(const char *path, char *err, size_t errlen);
/* Reads up to `frames` stereo frames; returns 0 at end of stream. */
size_t px_source_read(px_source *s, int16_t *out, size_t frames);
bool px_source_seek(px_source *s, uint64_t frame);
unsigned px_source_rate(const px_source *s);
void px_source_close(px_source *s);

#define PX_VOICE_BUFFER 1024

typedef struct
{
   px_source *src;
   int id;              /* caller-defined track id, -1 when idle */
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
/* Starts `path` as the current track, crossfading from whatever played before. */
bool px_mixer_play(px_mixer *m, int id, const char *path, bool loop, uint64_t loop_start,
      float volume, unsigned fade_frames, uint64_t start_frame, char *err, size_t errlen);
void px_mixer_stop(px_mixer *m, unsigned fade_frames);
/* Adds music to `frames` (interleaved stereo) after scaling the game audio. */
void px_mixer_mix(px_mixer *m, int16_t *frames, size_t count, float game_gain, float music_gain);
int px_mixer_current_id(const px_mixer *m);
uint64_t px_mixer_position(const px_mixer *m);
unsigned px_mixer_source_rate(const px_mixer *m);
bool px_mixer_seek(px_mixer *m, uint64_t frame);
void px_mixer_free(px_mixer *m);

#endif
