// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_finder.h"

#include <cstring>

void SongFinder::reset(size_t ram_size)
{
   current_.assign(ram_size, 0);
   at_mark_.assign(ram_size, 0);
   tracks_.assign(ram_size, Track{});
   song_.assign(ram_size, 1);
   command_.assign(ram_size, 1);
   song_count_ = command_count_ = ram_size;
   frame_ = period_start_ = 0;
   marks_ = 0;
}

void SongFinder::update(const uint8_t *ram, size_t size)
{
   if (!ram || size != current_.size())
   {
      if (ram)
      {
         reset(size);
         memcpy(current_.data(), ram, size);
         memcpy(at_mark_.data(), ram, size);
      }
      return;
   }

   if (frame_ == 0)
   {
      memcpy(current_.data(), ram, size);
      memcpy(at_mark_.data(), ram, size);
   }
   frame_++;

   // Compare in chunks; most of RAM is unchanged from one frame to the next.
   const size_t chunk = 64;
   for (size_t base = 0; base < size; base += chunk)
   {
      size_t n = size - base < chunk ? size - base : chunk;
      if (!memcmp(&current_[base], ram + base, n))
         continue;
      for (size_t i = base; i < base + n; i++)
      {
         if (current_[i] == ram[i])
            continue;
         current_[i] = ram[i];
         Track &t = tracks_[i];
         t.when[t.head] = (uint32_t)frame_;
         t.head = (uint8_t)((t.head + 1) % kHistory);
         if (t.count < 255)
            t.count++;
      }
   }
}

void SongFinder::end_period()
{
   for (auto &t : tracks_)
      t = Track{};
   at_mark_      = current_;
   period_start_ = frame_;
}

void SongFinder::mark_changed()
{
   uint64_t change_from = frame_ > kReactionFrames ? frame_ - kReactionFrames : 0;
   uint64_t quiet_from  = period_start_ + kSettleFrames;

   song_count_ = command_count_ = 0;
   for (size_t i = 0; i < tracks_.size(); i++)
   {
      const Track &t = tracks_[i];
      unsigned in_window = 0, in_quiet = 0;
      unsigned known = t.count < kHistory ? t.count : kHistory;
      for (unsigned k = 0; k < known; k++)
      {
         uint32_t when = t.when[(t.head + kHistory - 1 - k) % kHistory];
         if (when >= change_from)
            in_window++;
         else if (when >= quiet_from)
            in_quiet++;
      }
      // Busy bytes (timers, positions) change more often than we can track.
      bool busy = t.count > kHistory;

      if (song_[i])
      {
         // A new song value arrives once (sometimes via a brief 0) and then holds.
         bool keep = !busy && in_quiet == 0 && in_window > 0 && current_[i] != at_mark_[i];
         song_[i] = keep;
      }
      if (command_[i])
      {
         // A command register pulses a value and goes back to idle.
         bool keep = !busy && in_quiet == 0 && in_window >= 2 && current_[i] == at_mark_[i];
         command_[i] = keep;
      }
      song_count_ += song_[i];
      command_count_ += command_[i];
   }
   marks_++;
   end_period();
}

void SongFinder::mark_same()
{
   uint64_t quiet_from = period_start_ + kSettleFrames;

   song_count_ = command_count_ = 0;
   for (size_t i = 0; i < tracks_.size(); i++)
   {
      const Track &t = tracks_[i];
      bool changed_late = false;
      unsigned known = t.count < kHistory ? t.count : kHistory;
      for (unsigned k = 0; k < known; k++)
         if (t.when[(t.head + kHistory - 1 - k) % kHistory] >= quiet_from)
            changed_late = true;
      bool busy = t.count > kHistory;
      // The music stayed the same, so neither kind of byte may have moved.
      if (busy || changed_late)
         song_[i] = command_[i] = 0;
      song_count_ += song_[i];
      command_count_ += command_[i];
   }
   marks_++;
   end_period();
}

std::vector<SongCandidate> SongFinder::candidates(size_t max, bool commands) const
{
   std::vector<SongCandidate> out;
   const std::vector<uint8_t> &set = commands ? command_ : song_;
   if (marks_ == 0)
      return out;
   for (size_t i = 0; i < set.size() && out.size() < max; i++)
      if (set[i])
         out.push_back({ (uint32_t)i, current_[i], commands });
   return out;
}
