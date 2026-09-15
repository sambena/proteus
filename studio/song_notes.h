// SPDX-License-Identifier: GPL-3.0-or-later
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

// The first `seconds` of a song as mono samples at `rate`: any file libgme plays (.spc, or song
// `track` of an .nsf), or a recording in a .wav file (NES rips), which may be shorter.
bool render_music(const std::vector<uint8_t> &data, int track, int rate, int seconds,
      std::vector<double> &mono, std::string &error);
// A 16-bit mono .wav file of `samples` at `rate`.
std::vector<uint8_t> wav_file(const std::vector<int16_t> &samples, int rate);
bool is_wav(const std::vector<uint8_t> &data);

// Plays the first 30 seconds of a song (see render_music) and measures its notes.
bool music_notes(const std::vector<uint8_t> &data, int track, SongNotes &notes, std::string &error);
inline bool spc_notes(const std::vector<uint8_t> &spc, SongNotes &notes, std::string &error)
{
   return music_notes(spc, 0, notes, error);
}
// 0 (the same notes) to 1, at the best alignment of the two (any shift within the 30 seconds).
// `shift` receives that alignment: a's frame i lines up with b's frame i + shift.
double notes_distance(const SongNotes &a, const SongNotes &b, int *shift = nullptr);
// Seconds per notes frame.
double notes_frame_seconds();
