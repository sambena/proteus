// SPDX-License-Identifier: GPL-3.0-or-later
// N64 games with Nintendo EAD's sound engine (Super Mario 64, Ocarina of Time): finds the sequence
// players in RDRAM while the game runs, and writes a profile that follows the background music
// player's song and holds its volume at zero while replacing.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct N64Song
{
   int value;
   const char *name;
   const char *zophar;   // the song's title in Zophar's Domain's USF set, when it differs from `name`
};

struct N64Rom
{
   std::string code;    // header game code, "CZLE"
   int version = 0;     // header version byte
   std::string title;   // header title, "THE LEGEND OF ZELDA"
};

// Reads a .z64/.v64/.n64 header (the byte order is fixed up); false when it is not an N64 ROM.
bool n64_rom_header(const std::vector<uint8_t> &rom, N64Rom &out);
// True for N64 ROM files (.z64, .n64, .v64), and zip archives holding one.
bool is_n64_rom_file(const std::string &path);
// Song names for a game code's first three characters ("CZL"), or nullptr.
const std::vector<N64Song> *n64_song_names(const std::string &code);
// The song a file of the game's USF set plays ("19a Hyrule Field Main Theme.miniusf", "LOZ57.miniusf"),
// or -1.
int n64_song_of_reference(const std::string &code, const std::string &file_name);

struct N64ScanResult
{
   bool found = false;
   std::string engine;          // "sm64" or "oot": the sequence player layout
   uint32_t players = 0;        // N64 address of the first player (0x80xxxxxx)
   int player_count = 0;
   std::vector<int> songs_heard; // BGM player songs seen while scanning, in order
   N64Rom rom;
};

// Runs `rom_path` in `core_path` (Mupen64Plus-Next) for up to `seconds`, pressing Start now and
// then, until a sequence player array is found in RDRAM and its BGM player has played a song.
bool n64_scan(const std::string &core_path, const std::string &rom_path, const std::string &save_dir,
      double seconds, N64ScanResult &result, std::string &error,
      const std::function<void(const std::string &)> &log);
// The players an existing profile follows, when it was written for one of the known layouts.
bool n64_scan_from_profile(const std::string &profile_path, const N64Rom &rom, N64ScanResult &result);
// The address of the BGM player's song number, for showing.
uint32_t n64_song_address(const N64ScanResult &scan);

// The profile text for a scan: [song], [hold] and every known song, as `original` unless `tracks`
// gives its entry and a comment ({"music/field.ogg | track=2", "<- label"}, {"silence", ""});
// `music_dir` is the [library] folder.
std::string n64_profile(const N64ScanResult &scan, const std::string &rom_name,
      const std::map<int, std::pair<std::string, std::string>> &tracks = {}, const std::string &music_dir = "");
