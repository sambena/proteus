// SPDX-License-Identifier: LGPL-2.1-or-later
// How a song's notes run: the strength of each of the 12 pitch classes over its first
// seconds. Songs that share instruments and loudness still differ in their melodies, so
// this tells apart songs whose sound CPU memory does not (drivers that load a whole group
// of songs at once, then only move a pointer to start one).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SongNotes
{
   std::vector<float> chroma;   // 12 values per frame, each frame scaled to length 1 (silence: 0)
   int frames = 0;
};

// Plays the first seconds of an .spc and measures its notes.
bool spc_notes(const std::vector<uint8_t> &spc, SongNotes &notes, std::string &error);
// 0 (the same notes) to 1, at the best shift of up to 2 seconds.
double notes_distance(const SongNotes &a, const SongNotes &b);
