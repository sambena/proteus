// SPDX-License-Identifier: LGPL-2.1-or-later
// N64 games with Nintendo EAD's sound engine (Super Mario 64, Ocarina of Time): finds the sequence
// players in RDRAM while the game runs, and writes a profile that follows the background music
// player's song and holds its volume at zero while replacing.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct N64Song
{
   int value;
   const char *name;
};

struct N64Rom
{
   std::string code;    // header game code, "CZLE"
   int version = 0;     // header version byte
   std::string title;   // header title, "THE LEGEND OF ZELDA"
};

// Reads a .z64/.v64/.n64 header (the byte order is fixed up); false when it is not an N64 ROM.
bool n64_rom_header(const std::vector<uint8_t> &rom, N64Rom &out);
// Song names for a game code's first three characters ("CZL"), or nullptr.
const std::vector<N64Song> *n64_song_names(const std::string &code);

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

// The profile text for a scan: [song], [hold] and every known song as `original` with its name.
std::string n64_profile(const N64ScanResult &scan, const std::string &rom_name);
