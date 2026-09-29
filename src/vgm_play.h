/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Streamed game music from the GameCube onward (DSP, ADP, AST, HPS, BRSTM, BFSTM, BCSTM, BWAV,
 * ADX, HCA...), played by vgmstream with its built-in decoders only. */
#ifndef PROTEUS_VGM_PLAY_H
#define PROTEUS_VGM_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct px_vgm px_vgm;

/* Opens song `subsong` (0 = the first) of a stream or container. Looped, a stream with loop
 * points plays forever; otherwise it plays its loop twice and fades out over ten seconds, as
 * vgmstream's own players do (a stream without loop points plays once). A mono file named
 * like "song_L.dsp" plays in stereo with its "song_R.dsp" when that is beside it. */
px_vgm *px_vgm_open(const char *path, unsigned subsong, bool loop, char *err, size_t errlen);
/* Renders up to `frames` stereo frames at px_vgm_rate(); fewer (0) at the end of the song. */
size_t px_vgm_render(px_vgm *v, int16_t *out, size_t frames);
bool px_vgm_seek(px_vgm *v, uint64_t frame);
/* The stream's own sample rate. */
unsigned px_vgm_rate(const px_vgm *v);
/* Frames the song plays for (loops and fade included); 0 when it plays forever. */
uint64_t px_vgm_length(const px_vgm *v);
void px_vgm_close(px_vgm *v);

/* True for the extensions px_source hands to vgmstream. */
bool px_vgm_path(const char *path);
/* Songs in the file: its subsong count, 1 for a plain stream, 0 when vgmstream cannot open it
 * or when it is the right half ("_R") of a pair whose left half ("_L") plays both. */
unsigned px_vgm_count(const char *path);

#ifdef __cplusplus
}
#endif

#endif
