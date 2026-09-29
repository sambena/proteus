/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Nintendo DS NCSF and miniNCSF rips (sequences from a game's SDAT), played by SSEQPlayer. */
#ifndef PROTEUS_NCSF_PLAY_H
#define PROTEUS_NCSF_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct px_ncsf px_ncsf;

/* Loads a .ncsf/.minincsf and the .ncsflib files its tags name, to play at `rate` Hz. */
px_ncsf *px_ncsf_open(const char *path, unsigned rate, char *err, size_t errlen);
/* Renders `frames` stereo frames; false on an error. */
bool px_ncsf_render(px_ncsf *n, int16_t *out, size_t frames);
/* Starts the song over; false when the sequence cannot be set up again. */
bool px_ncsf_restart(px_ncsf *n);
/* The tagged length and fade in milliseconds (0 when untagged), and title (may be ""). */
uint64_t px_ncsf_length_ms(const px_ncsf *n);
uint64_t px_ncsf_fade_ms(const px_ncsf *n);
const char *px_ncsf_title(const px_ncsf *n);
void px_ncsf_close(px_ncsf *n);

/* True for .ncsf and .minincsf (.ncsflib files are libraries, not songs). */
bool px_ncsf_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
