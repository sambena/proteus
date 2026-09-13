// SPDX-License-Identifier: LGPL-2.1-or-later
// One ROM open in Proteus Studio: its emulator, how it keeps and starts songs, and the
// songs found in it. Songs are ripped to .spc files that play without the game.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core_host.h"
#include "game_db.h"
#include "snes_rom.h"

enum SongKind { SONG_MUSIC, SONG_JINGLE };

// How a rip sounds over its first seconds, for telling songs apart.
struct SongPrint
{
   std::vector<float> envelope;   // loudness per 100 ms
   std::vector<float> brightness; // loudness of the signal's changes per 100 ms (high frequencies)
   float loudness = 0;            // mean envelope
   float tail = 0;                // mean envelope over the last seconds
};

struct FoundSong
{
   uint32_t value = 0;          // song number
   bool has_value = true;       // false for rips made while playing without a song number
   std::string title;
   std::string spc_path;
   SongKind kind = SONG_MUSIC;
   SongPrint print;
};

class RomSession
{
public:
   RomSession();
   ~RomSession();

   // Loads the ROM with the core. `system_dir` is RetroArch's system folder.
   bool open(const std::string &rom_path, const std::string &core_path,
         const std::string &system_dir, std::string &error);
   void close();
   bool is_open() const { return open_; }

   const std::string &rom_path() const { return rom_path_; }
   // The name Proteus matches profiles by: the ROM file (or zip entry) without extension.
   const std::string &game_name() const { return game_name_; }
   const std::string &display_name() const { return display_name_; }
   const std::string &profile_path() const { return profile_path_; }   // existing profile, if any
   uint32_t rom_crc32() const { return rom_.crc32; }

   // Where the song number is kept and how songs are started, with where each came from
   // ("game database", "profile", "scan", "song finder", "manual entry").
   SongAddress address;
   std::string address_source;
   SongStart start;
   std::string start_source;
   // Records `address` and `start` in the game database (UI thread).
   void save_to_game_db(const std::string &note);

   // Songs, in value order. Lock songs_mutex while reading from another thread.
   std::mutex songs_mutex;
   std::vector<FoundSong> songs;
   FoundSong *find_song(uint32_t value);
   void rename_song(size_t index, const std::string &title);
   void remove_song(size_t index);

   // Scanning: from the scan start state, starts each song number and rips what plays.
   // Without a working way to start songs, the scan first watches the game boot to find
   // one: a RAM command the game polls, or the game's music routine.
   void start_scan(int first, int last);
   void stop_scan();
   bool scanning() const { return scanning_; }
   float scan_progress() const;
   int scan_found() const { return scan_found_; }
   std::string scan_message();
   // Adopts what a finished scan found and records it in the game database (UI thread).
   void apply_scan_results();

   // The moment scans start from. Defaults to a few seconds after boot.
   void use_current_moment_for_scans();

   // Live play (Advanced view). Lock core_mutex around every use of core.
   std::mutex core_mutex;
   CoreHost core;
   void play_frame(uint16_t buttons);   // core_mutex held by the caller
   // Rips whatever the game is playing now; core_mutex held by the caller.
   bool rip_now(std::string &message);
   uint32_t last_song_value() const { return last_value_; }
   bool heard_song() const { return have_last_; }

   std::vector<std::string> take_log();

private:
   void scan_thread(int first, int last);
   bool rip_state(const std::vector<uint8_t> &state, uint32_t value, bool has_value, FoundSong &out, std::string &error);
   bool rip_playing(const std::vector<uint8_t> &state, bool automatic, std::string &message);
   bool start_song(const SongStart &s, uint32_t value);
   bool read_song_value(const SongAddress &a, uint32_t &value);
   void set_scan_message(const std::string &message);
   void run_frames(int frames);
   int start_score(const SongStart &s, const SongPrint *baseline);
   bool find_song_start(const SongPrint *baseline);
   bool choose_stub_area(const std::vector<uint8_t> &before);
   void add_song_locked(FoundSong song);
   std::string library_dir() const;
   void load_library();
   void save_library();
   void log(const std::string &line);

   bool open_ = false;
   std::string rom_path_, game_name_, display_name_, profile_path_;
   SnesRom rom_;

   std::vector<uint8_t> scan_state_;
   uint32_t stub_ram_ = 0x1FF00;          // spare RAM for the code that calls music routines
   SongAddress scan_address_;             // the scan thread's copies
   SongStart scan_start_;
   std::string scan_address_source_, scan_start_source_;
   std::atomic<bool> scan_changed_{false};
   std::thread worker_;
   std::atomic<bool> scanning_{false}, cancel_{false};
   std::atomic<int> scan_done_{0}, scan_total_{0}, scan_found_{0};
   std::mutex message_mutex_;
   std::string scan_message_;

   // Live song tracking while playing.
   uint32_t candidate_ = 0, last_value_ = 0;
   unsigned candidate_frames_ = 0;
   bool have_last_ = false;
   int pending_rip_frames_ = -1;
   uint8_t last_ports_[4] = { 0, 0, 0, 0 };

   std::mutex log_mutex_;
   std::vector<std::string> log_;
};

// Renders the first seconds of an .spc and measures it.
bool analyze_spc(const std::vector<uint8_t> &spc, SongPrint &print, std::string &error);
bool same_song(const SongPrint &a, const SongPrint &b);
