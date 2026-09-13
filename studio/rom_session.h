// SPDX-License-Identifier: LGPL-2.1-or-later
// One ROM open in Proteus Studio: its emulator, its song address, and the songs
// found in it. Songs are ripped to .spc files that play without the game.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core_host.h"
#include "presets.h"

// Where a game keeps its song number. Proteus reads `address` to follow the music;
// scans write song numbers to `scan_address`, a command register that starts
// songs. They are the same byte in games whose song address is a command register.
struct SongAddress
{
   bool known = false;
   std::string source;          // "profile", "preset", "song finder", "manual entry"
   int memory = 0;              // 0 system_ram, 1 save_ram, 2 video_ram
   uint32_t address = 0;
   int size = 1;
   bool latch = false;
   int debounce = 2;

   // The music command. With `scan_bytes` empty, the song number alone is written
   // (`size` bytes); otherwise the whole command is written with the song number at
   // `scan_offset` (Chrono Trigger: $1E00 = 10 song FF 05).
   bool scan_known = false;
   uint32_t scan_address = 0;
   std::vector<uint8_t> scan_bytes;
   int scan_offset = 0;
};

struct ScanCommand
{
   int memory = 0;
   uint32_t address = 0;
   std::vector<uint8_t> bytes;
   int offset = 0;
   int size = 1;
};

ScanCommand scan_command(const SongAddress &a);
// "$1E00 = 10 song FF 05"
std::string describe_scan_command(const SongAddress &a);

enum SongKind { SONG_MUSIC, SONG_JINGLE };

// How a rip sounds over its first seconds, for telling songs apart.
struct SongPrint
{
   std::vector<float> envelope; // loudness per 100 ms
   std::vector<float> brightness; // loudness of the signal's changes per 100 ms (high frequencies)
   float loudness = 0;          // mean envelope
   float tail = 0;              // mean envelope over the last seconds
};

struct FoundSong
{
   uint32_t value = 0;          // song number (what the song address holds)
   bool has_value = true;       // false for rips made while playing without an address
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

   // Loads the ROM with the core. `profile_dir` is RetroArch's system folder.
   bool open(const std::string &rom_path, const std::string &core_path,
         const std::string &system_dir, std::string &error);
   void close();
   bool is_open() const { return open_; }

   const std::string &rom_path() const { return rom_path_; }
   // The name Proteus matches profiles by: the ROM file (or zip entry) without extension.
   const std::string &game_name() const { return game_name_; }
   const std::string &display_name() const { return display_name_; }
   const std::string &system() const { return system_; }
   const std::string &profile_path() const { return profile_path_; }   // existing profile, if any
   const RomPreset *preset() const { return preset_; }

   SongAddress address;

   // Songs, in value order. Lock songs_mutex while reading from another thread.
   std::mutex songs_mutex;
   std::vector<FoundSong> songs;
   FoundSong *find_song(uint32_t value);
   void rename_song(size_t index, const std::string &title);
   void remove_song(size_t index);

   // Scanning: from the scan start state, sends each song number through the music
   // command, lets the sound driver start it, and rips what plays. Songs are numbered
   // by what the song address holds afterwards. Without a working command, the scan
   // first watches the game boot to find the one it uses.
   void start_scan(int first, int last);
   void stop_scan();
   bool scanning() const { return scanning_; }
   float scan_progress() const;
   int scan_found() const { return scan_found_; }
   std::string scan_message();

   // The moment scans start from. Defaults to a few seconds after boot.
   bool has_scan_state() const { return !scan_state_.empty(); }
   void use_current_moment_for_scans();
   int settle_frames = 150;

   // Live play (Advanced view). Lock core_mutex around every use of core.
   std::mutex core_mutex;
   CoreHost core;
   void play_frame(uint16_t buttons);   // core_mutex held by the caller
   // Rips whatever the game is playing now; core_mutex held by the caller.
   bool rip_now(std::string &message);
   uint32_t last_song_value() const { return last_value_; }
   bool heard_song() const { return have_last_; }

   std::vector<std::string> take_log();
   // Adopts the music command and song address a finished scan found (UI thread).
   void apply_scan_results();

private:
   void scan_thread(int first, int last);
   bool rip_state(const std::vector<uint8_t> &state, const std::string &title, uint32_t value,
         bool has_value, FoundSong &out, std::string &error);
   bool rip_playing(const std::vector<uint8_t> &state, bool automatic, std::string &message);
   bool write_command(const ScanCommand &cmd, uint32_t value);
   bool read_song_value(const SongAddress &a, uint32_t &value);
   void set_scan_message(const std::string &message);
   void run_settle();
   int command_score(const ScanCommand &cmd, const SongPrint *baseline, int *music_out);
   bool find_command(const SongPrint *baseline);
   void add_song_locked(FoundSong song);
   std::string library_dir() const;
   void load_library();
   void save_library();
   void log(const std::string &line);

   bool open_ = false;
   std::string rom_path_, game_name_, display_name_, system_, profile_path_;
   const RomPreset *preset_ = nullptr;

   std::vector<uint8_t> scan_state_;
   SongAddress scan_addr_;                  // the scan thread's copy of `address`
   std::atomic<bool> scan_addr_changed_{false};
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
