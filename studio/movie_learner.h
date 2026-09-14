// SPDX-License-Identifier: LGPL-2.1-or-later
// Learns a game's song address from moments of a long playthrough (a TAS movie): at each
// moment the sound CPU's RAM names the song against the reference songs, and the game RAM
// byte that holds one value per song, and a different one for each, is the song address.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "reference.h"

class MovieLearner
{
public:
   explicit MovieLearner(const ReferenceSet &refs) : refs_(refs) {}

   // One moment: the sound CPU's 64 KB of RAM and the game's RAM. Returns the reference named
   // by the sound CPU, or -1.
   int add(const uint8_t *apu, const uint8_t *ram, size_t ram_size);

   struct Result
   {
      bool found = false;
      uint32_t address = 0;               // game RAM offset
      std::vector<uint32_t> ties;         // other bytes that follow the music as well
      std::map<int, uint8_t> values;      // reference -> song number
      int songs = 0;                      // references heard long enough to count
      int candidates = 0;                 // bytes that tell the most songs apart
      std::vector<std::pair<uint32_t, int>> top;   // best bytes and the songs each tells apart
   };
   // What the moments so far say. Needs three songs.
   Result result() const;

   // Each settled song's value at a RAM byte, with its moments and disagreements.
   std::string describe(uint32_t address) const;

   int moments() const { return moments_; }
   // References heard, in the order first heard.
   const std::vector<int> &heard() const { return heard_; }

private:
   struct Row
   {
      int reference;
      int count = 0;
      std::vector<uint8_t> value;    // per RAM byte: the value first seen
      std::vector<uint16_t> bad;     // per RAM byte: moments that disagreed
   };
   void learn(int reference, const std::vector<uint8_t> &ram);

   const ReferenceSet &refs_;
   int moments_ = 0;
   std::vector<int> heard_;
   std::vector<Row> rows_;
   // A moment counts once the moments on both sides name the same song, so moments during a
   // change of music (the RAM already on the next song, the sound CPU not yet) do not.
   int prev_name_ = -1, cur_name_ = -1;
   std::vector<uint8_t> cur_ram_;
};
