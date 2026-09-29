/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Game Boy Advance GSF and miniGSF rips, played by viogsf (VBA-M's GBA core). */
#ifndef PROTEUS_GSF_PLAY_H
#define PROTEUS_GSF_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct px_gsf px_gsf;

/* Loads a .gsf/.minigsf and the .gsflib files its tags name; renders at `rate` (8-192 kHz). */
px_gsf *px_gsf_open(const char *path, unsigned rate, char *err, size_t errlen);
/* Renders `frames` stereo frames; false on an emulation error. */
bool px_gsf_render(px_gsf *g, int16_t *out, size_t frames);
/* Starts the song over; false when the emulator cannot be rebuilt. */
bool px_gsf_restart(px_gsf *g);
/* The tagged length and fade in milliseconds (0 when untagged), and title (may be ""). */
uint64_t px_gsf_length_ms(const px_gsf *g);
uint64_t px_gsf_fade_ms(const px_gsf *g);
const char *px_gsf_title(const px_gsf *g);
void px_gsf_close(px_gsf *g);

/* True for .gsf and .minigsf (.gsflib files are libraries, not songs). */
bool px_gsf_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
