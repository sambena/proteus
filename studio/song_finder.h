// SPDX-License-Identifier: LGPL-2.1-or-later
// Finds the RAM byte that holds a game's current song.
//
// While the game runs, every byte's recent changes are tracked. The player marks
// moments when the music changed (or confirms it stayed the same). A song byte
// changes shortly before each "changed" mark and holds still otherwise; nearly
// everything else in RAM either never changes or changes all the time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct SongCandidate
{
   uint32_t address;
   uint8_t value;
   bool command;   // a one-shot command register: pulses and returns to its old value
};

class SongFinder
{
public:
   // Frames before a "changed" mark in which the song byte may change: players
   // hear the new music a moment after it starts, then react.
   static constexpr unsigned kReactionFrames = 240;
   // Frames after a mark that are ignored, for late fades and transitions.
   static constexpr unsigned kSettleFrames = 60;

   void reset(size_t ram_size);
   void update(const uint8_t *ram, size_t size);
   void mark_changed();
   void mark_same();

   size_t candidate_count() const { return song_count_; }
   size_t command_count() const { return command_count_; }
   unsigned marks() const { return marks_; }
   uint64_t frame() const { return frame_; }
   // Up to `max` candidates; command registers first when `commands` is true.
   std::vector<SongCandidate> candidates(size_t max, bool commands) const;

private:
   static constexpr unsigned kHistory = 4;
   struct Track
   {
      uint32_t when[kHistory]; // frames of the most recent changes
      uint8_t head;
      uint8_t count;           // changes since the last mark, saturating
   };

   void end_period();

   std::vector<uint8_t> current_;
   std::vector<uint8_t> at_mark_;   // values when the last mark was made
   std::vector<Track> tracks_;
   std::vector<uint8_t> song_;      // still a song byte candidate
   std::vector<uint8_t> command_;   // still a command register candidate
   size_t song_count_ = 0, command_count_ = 0;
   uint64_t frame_ = 0;
   uint64_t period_start_ = 0;      // frame of the last mark (or reset)
   unsigned marks_ = 0;
};
