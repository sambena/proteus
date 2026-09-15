// SPDX-License-Identifier: LGPL-2.1-or-later
// What an .nsf's init routine does to the NES's RAM when it starts a song. NSF rips contain the
// game's own music code, and many start a song the way the game does: by writing a request to
// the RAM its music code polls (The Legend of Zelda: $0600 = 80 for the title song). Running
// init for each song on a small 6502 shows those bytes, which a scan can then write in the game.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The RAM ($0000-$07FF) init leaves written when starting song `song` (from 0): address -> value.
bool nsf_init_ram(const std::vector<uint8_t> &nsf, int song, std::map<uint16_t, uint8_t> &writes, std::string &error);

struct NsfRequest
{
   uint16_t address = 0;                 // RAM the request is written to
   std::map<int, uint8_t> song_values;   // NSF song (from 0) -> value written there
};

// The RAM bytes init writes with a value that depends on the song: for each, the value of every
// song that writes it. Bytes most songs write come first.
std::vector<NsfRequest> nsf_song_requests(const std::vector<uint8_t> &nsf, std::string &error);

enum { NSF_CHANNELS = 5 };   // pulse 1, pulse 2, triangle, noise, samples

// How song `song` uses the sound channels: for each channel, the frames (of `frames`, after init)
// it is left audible. `holds` are RAM bytes set to their values before every frame, as a profile
// would hold them in the game.
bool nsf_channel_activity(const std::vector<uint8_t> &nsf, int song, int frames,
      const std::vector<std::pair<uint16_t, uint8_t>> &holds, int activity[NSF_CHANNELS], std::string &error);

struct NsfSwitch
{
   uint16_t address = 0;
   uint8_t value = 0;
   int silenced = 0;   // channels (bit 0 pulse 1 .. bit 4 samples) the song stops using
};

// The RAM bytes that, held at a value, make song `song` stop using channels it uses: the music
// code's own switches (a track's pointer, its "playing" flag). `used` receives the channels the
// song uses. Switches silencing the most channels come first.
std::vector<NsfSwitch> nsf_music_switches(const std::vector<uint8_t> &nsf, int song, int &used, std::string &error);

struct NsfPatch
{
   uint16_t address = 0;           // CPU address of the patched instruction
   std::vector<uint8_t> bytes;     // what it becomes
   std::vector<uint8_t> original;  // what the song's banks hold there (a cheat's compare bytes)
   int silenced = 0;
};

// Code patches to the music code (skipping a subroutine call, or changing a branch) that make
// song `song` stop using channels it uses. Patches silencing the most channels come first.
std::vector<NsfPatch> nsf_music_patches(const std::vector<uint8_t> &nsf, int song, int &used, std::string &error);
struct NsfVariable
{
   uint16_t address = 0;
   std::map<int, uint8_t> song_values;   // song (from 0) -> the value held while it plays
   int distinct = 0;                     // different values among those songs
};

// RAM the music code keeps steady while each of `songs` plays (read after 1, 2 and 3 seconds),
// with a value that tells the songs apart: where the game may keep its song playing. Bytes
// telling the most songs apart come first.
std::vector<NsfVariable> nsf_song_variables(const std::vector<uint8_t> &nsf, const std::vector<int> &songs, std::string &error);

struct NsfCall
{
   uint16_t routine = 0;                 // CPU address the .nsf's init calls
   std::map<int, uint8_t> song_values;   // song (from 0) -> A when it calls
   int distinct = 0;
};

// The subroutines init calls with A depending on the song: the game's own "play this sound"
// routine, and what each song passes it. Routines telling the most songs apart come first.
std::vector<NsfCall> nsf_init_calls(const std::vector<uint8_t> &nsf, std::string &error);

// The bytes at a CPU address once init has set up song 0's banks.
std::vector<uint8_t> nsf_code_at(const std::vector<uint8_t> &nsf, uint16_t address, size_t length, std::string &error);

// Channel activity (see nsf_channel_activity) with code patches applied.
bool nsf_channel_activity_patched(const std::vector<uint8_t> &nsf, int song, int frames,
      const std::vector<std::pair<uint16_t, uint8_t>> &patches, int activity[NSF_CHANNELS], std::string &error);
