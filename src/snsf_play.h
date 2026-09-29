/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SNES SNSF and miniSNSF rips, played by LakeSnes. */
#ifndef PROTEUS_SNSF_PLAY_H
#define PROTEUS_SNSF_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct px_snsf px_snsf;

/* Loads a .snsf/.minisnsf and the .snsflib files its tags name. */
px_snsf *px_snsf_open(const char *path, char *err, size_t errlen);
/* Renders `frames` stereo frames at `rate`; false on an emulation error. */
bool px_snsf_render(px_snsf *p, int16_t *out, size_t frames, unsigned rate);
/* Starts the song over. */
void px_snsf_restart(px_snsf *p);
/* The tagged length and fade in milliseconds (0 when untagged), and title (may be ""). */
uint64_t px_snsf_length_ms(const px_snsf *p);
uint64_t px_snsf_fade_ms(const px_snsf *p);
const char *px_snsf_title(const px_snsf *p);
void px_snsf_close(px_snsf *p);

/* True for .snsf and .minisnsf (.snsflib files are libraries, not songs). */
bool px_snsf_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
