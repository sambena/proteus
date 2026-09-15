/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Nintendo 64 USF and miniUSF rips, played by lazyusf2. */
#ifndef PROTEUS_USF_PLAY_H
#define PROTEUS_USF_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct px_usf px_usf;

/* Loads a .usf/.miniusf and the .usflib files its tags name. */
px_usf *px_usf_open(const char *path, char *err, size_t errlen);
/* Renders `frames` stereo frames at `rate`; false on an emulation error. */
bool px_usf_render(px_usf *u, int16_t *out, size_t frames, unsigned rate);
/* Starts the song over. */
void px_usf_restart(px_usf *u);
/* The tagged length and fade in milliseconds (0 when untagged), and title (may be ""). */
uint64_t px_usf_length_ms(const px_usf *u);
uint64_t px_usf_fade_ms(const px_usf *u);
const char *px_usf_title(const px_usf *u);
void px_usf_close(px_usf *u);

/* True for .usf and .miniusf (.usflib files are libraries, not songs). */
bool px_usf_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
