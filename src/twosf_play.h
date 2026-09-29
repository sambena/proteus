/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Nintendo DS 2SF and mini2SF rips, played by vio2sf. */
#ifndef PROTEUS_TWOSF_PLAY_H
#define PROTEUS_TWOSF_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* vio2sf emulates the DS sound hardware at this rate only. */
#define PX_2SF_RATE 44100

typedef struct px_2sf px_2sf;

/* Loads a .2sf/.mini2sf and the .2sflib files its tags name. */
px_2sf *px_2sf_open(const char *path, char *err, size_t errlen);
/* Renders `frames` stereo frames at PX_2SF_RATE; false on an emulation error. */
bool px_2sf_render(px_2sf *t, int16_t *out, size_t frames);
/* Starts the song over; false when the emulator cannot be set up again. */
bool px_2sf_restart(px_2sf *t);
/* The tagged length and fade in milliseconds (0 when untagged), and title (may be ""). */
uint64_t px_2sf_length_ms(const px_2sf *t);
uint64_t px_2sf_fade_ms(const px_2sf *t);
const char *px_2sf_title(const px_2sf *t);
void px_2sf_close(px_2sf *t);

/* True for .2sf and .mini2sf (.2sflib files are libraries, not songs). */
bool px_2sf_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
