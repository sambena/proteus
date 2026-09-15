// SPDX-License-Identifier: LGPL-2.1-or-later
// Scans every ROM in a folder, one after another, in the background: downloads each game's
// reference songs, runs the song scan (and, when asked, plays a TAS movie for games the scan
// cannot number), and rates how well the game's songs could be told apart. Results go to each
// game's song library and the game database, as a scan in the window would, and to a report
// (folder-scan.tsv) that lets a stopped run carry on where it left off.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FolderScanRow
{
   std::string rom;          // file name
   std::string game;         // display name
   int references = 0;       // reference songs
   bool table = false;       // a song table was found in the ROM
   int songs = 0;            // songs in the list
   int named = 0;            // numbered songs named by a reference
   std::string address;      // "$1DFB", or empty
   std::string how;          // "scan", "TAS movie", "game database", ...
   std::string verdict;      // "easy", "partly", "skip"
   std::string note;
   std::string start;        // how songs start ("$FB = song"), or empty
   std::string silence;      // the RAM request that stops the music ("$FB = 80"), or empty
};

// An NES ROM (.nes), or a zip holding one.
bool is_nes_rom_file(const std::string &path);

class FolderScan
{
public:
   struct Options
   {
      std::string folder, core_path, system_dir, app_dir;
      std::string nes_core_path;     // for NES ROMs; they are skipped without one
      bool download_references = true;
      bool use_movies = false;       // play a TAS movie (needs BizHawk) when the scan leaves songs unnumbered
      int movie_speed = 6400;
      bool rescan = false;           // scan games the report already has
      std::vector<std::string> skip; // ROM paths open elsewhere
   };

   ~FolderScan();
   void start(const Options &options);
   void stop();
   bool running() const { return running_; }
   // How far along: ROMs done, ROMs in the folder, and what is happening now.
   int done() const { return done_; }
   int total() const { return total_; }
   std::string message();
   std::vector<FolderScanRow> rows();
   std::string report_path() const;

private:
   void run(Options options);
   void set_message(const std::string &m);
   void save_report();

   std::thread thread_;
   std::atomic<bool> running_{false}, cancel_{false};
   std::atomic<int> done_{0}, total_{0};
   std::mutex mutex_;
   std::string message_, report_;
   std::vector<FolderScanRow> rows_;
};
