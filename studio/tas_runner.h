// SPDX-License-Identifier: LGPL-2.1-or-later
// TAS movies as a way to hear every song of a game: finds and downloads a game's movies from
// TASVideos, installs BizHawk (the emulator they were made with, so they stay in sync), and
// plays a movie in BizHawk with a Lua script that saves the sound CPU and game RAM every
// 2 seconds for Proteus Studio to read.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TasPublication
{
   int id = 0;
   std::string title;       // "SNES ActRaiser 2 by fred in 44:12.60"
   std::string game;        // "ActRaiser 2"
   std::string goal;        // "baseline", "100%", ...
   std::string emulator;    // "BizHawk 1.9.4"
   std::string file;        // movie file name
   int frames = 0;
   bool current = true;     // not obsoleted by a newer publication
   double similarity = 0;   // to the game it was looked up for

   // BizHawk plays its own movies (.bk2, .bkm); others were made with emulators that differ.
   bool playable() const;
   std::string duration() const;   // "44:12"
};

// Every SNES publication on TASVideos. Blocking; for worker threads.
bool tasvideos_snes_publications(std::vector<TasPublication> &out, std::string &error);
// Publications of the game named `game`, the most useful first: movies BizHawk plays, closest
// name, longest (a longer movie reaches more of the game's music).
std::vector<TasPublication> tasvideos_for_game(const std::vector<TasPublication> &all, const std::string &game);
// Downloads a publication's movie into `dir`; `path` is the saved movie (.bk2 taken out of its zip).
bool tasvideos_download(const TasPublication &pub, const std::string &dir, std::string &path, std::string &error);

// BizHawk under <app dir>\tools\BizHawk.
std::string bizhawk_exe(const std::string &app_dir);
bool bizhawk_installed(const std::string &app_dir);
// Downloads the latest BizHawk release for Windows from GitHub and unpacks it.
bool bizhawk_install(const std::string &app_dir, const std::function<void(const std::string &message, float progress)> &progress,
      const std::atomic<bool> *cancel, std::string &error);

// One movie playing in BizHawk.
class BizHawkRun
{
public:
   BizHawkRun() = default;
   ~BizHawkRun();
   BizHawkRun(const BizHawkRun &) = delete;
   BizHawkRun &operator=(const BizHawkRun &) = delete;

   // Starts BizHawk on `rom` playing `movie`. `speed` is a percentage (100 = real time; BizHawk
   // goes as fast as the computer allows up to 6400). `show` leaves its window up.
   bool start(const std::string &app_dir, const std::string &rom, const std::string &movie, int speed, bool show,
         std::string &error);
   void set_speed(int speed);
   // Takes the oldest saved moment: 64 KB of sound CPU RAM, then the game's RAM.
   bool next_dump(uint32_t &frame, std::vector<uint8_t> &data);
   // The movie ended (or BizHawk closed); the moments left can still be taken.
   bool finished();
   uint32_t frame();     // how far the movie has played
   uint32_t length();    // frames in the movie
   void stop();

private:
   void read_progress();
   void *process_ = nullptr;
   std::string work_;
   uint32_t frame_ = 0, length_ = 0;
};
