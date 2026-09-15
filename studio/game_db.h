// SPDX-License-Identifier: LGPL-2.1-or-later
// What Proteus Studio knows about each game, keyed by ROM checksum: where it keeps its
// song number and how its songs are started. Stored in %APPDATA%\ProteusStudio\games.ini,
// written whenever a scan confirms something, and editable by hand.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Where a game keeps its song number. Proteus reads it to follow the music.
struct SongAddress
{
   bool known = false;
   int memory = 0;              // 0 system_ram, 1 save_ram, 2 video_ram
   uint32_t address = 0;
   int size = 1;
   bool latch = false;          // a command register: holds a song number briefly, then 0
   int debounce = 2;
   std::vector<uint8_t> bytes;  // pattern for command blocks (e.g. 10 xx .. ..)
   std::vector<bool> any;       // per byte of `bytes`: matches any value (written "..")
   int offset = 0;              // offset of xx in bytes
   // A second command register for jingles (Super Mario Bros.: $FC); its commands are song
   // values 0x100 + command.
   bool events = false;
   uint32_t events_address = 0;
};

// Reads the song number at `a` from `ram`; false when a pattern does not match.
bool read_song_address(const SongAddress &a, const uint8_t *ram, size_t ram_size, uint32_t &value);

// "$1DFB", "$1E00 = 10 song .. .."
std::string describe_song_address(const SongAddress &a);
std::string format_song_pattern(const SongAddress &a);
bool parse_song_pattern(const std::string &text, SongAddress &a);

// How to start a song from outside the game.
struct SongStart
{
   enum Kind
   {
      NONE,
      RAM,       // write a command to RAM that the game polls (Super Mario World: $1DFB)
      ROUTINE,   // call the game's music routine (Chrono Trigger: $1E00 = 10 song FF 05, JSL $C70004)
   } kind = NONE;

   uint32_t address = 0;        // RAM: the command's RAM offset; ROUTINE: the routine's CPU address
   bool jsr = false;            // ROUTINE: returns with RTS rather than RTL
   bool fill_block = false;     // ROUTINE: fill `block` with `bytes` before the call
   uint32_t block = 0;          // ROUTINE: RAM offset of the parameters
   std::vector<uint8_t> bytes;  // the command; the song number goes at `offset`. Empty: the song number alone
   int offset = 0;
   bool song_in_a = false;      // ROUTINE: pass the song number in A
   int settle_frames = 150;     // frames a started song gets before it is ripped
};

// "$1DFB = song", "JSL $C70004 with $1E00 = 10 song FF 05"
std::string describe_song_start(const SongStart &s);
// The database form: "ram 0x1DFB bytes=xx settle=150",
// "routine jsl 0xC70004 block=0x1E00 bytes=10 xx FF 05 settle=300". Empty for NONE.
std::string format_song_start(const SongStart &s);
bool parse_song_start(const std::string &text, SongStart &s);

// A value written to RAM that stops the game's own music until it starts another song
// (Super Mario Bros.: $FB = 80). Stopping the music this way keeps the sound channels, and so
// the sound effects, that muting them would lose.
struct SongSilence
{
   bool known = false;
   uint32_t address = 0;
   uint8_t value = 0;
};

// Cheat codes by core ("fceumm" -> "809D?D0:F0"), in each core's format.
using CoreCodes = std::map<std::string, std::string>;

struct GameInfo
{
   uint32_t crc32 = 0;
   std::string name;
   SongAddress song;
   SongStart start;
   SongSilence silence;
   // A tap makes the game report the sounds it requests at the song address (a latch whose
   // unlisted values, sound effects, leave the music alone); a patch stops its music code
   // while a replacement plays, keeping its sound effects.
   CoreCodes tap;
   CoreCodes patch;
   std::string note;            // how the entry was confirmed
};

class GameDb
{
public:
   static GameDb &get();
   bool find(uint32_t crc32, GameInfo &out);
   void put(const GameInfo &info);     // stores and saves the file
   std::string path() const;

private:
   void load_locked();
   void save_locked();

   std::mutex mutex_;
   bool loaded_ = false;
   std::map<uint32_t, GameInfo> games_;
};
