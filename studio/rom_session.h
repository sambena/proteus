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

#include "apu_analyzer.h"
#include "core_host.h"
#include "game_db.h"
#include "ra_client.h"
#include "reference.h"
#include "song_notes.h"
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
   std::string reference;       // title of the reference song it matched, if any
   SongKind kind = SONG_MUSIC;
   SongPrint print;
};

class RomSession
{
public:
   RomSession();
   ~RomSession();

   // Where the song library and reference songs are kept (default %APPDATA%/ProteusStudio).
   void set_app_dir(const std::string &dir) { app_dir_ = dir; }

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
   const std::string &rom_md5() const { return rom_.md5; }

   // Where the song number is kept and how songs are started, with where each came from
   // ("game database", "profile", "scan", "song finder", "manual entry").
   SongAddress address;
   std::string address_source;
   SongStart start;
   std::string start_source;
   // Records `address` and `start` in the game database (UI thread).
   void save_to_game_db(const std::string &note);

   // RetroAchievements code notes lookup
   std::mutex ra_mutex;
   RaLookupResult ra_result;
   void query_retroachievements();
   bool querying_retroachievements() const { return querying_ra_; }

   // Static 65816 APU analysis
   ApuAnalysisResult apu_analysis;
   void run_static_analysis();

   // Reference songs: the game's soundtrack as .spc files. Scans and rips are named after the
   // reference they match, and a song table found in the ROM lists every song by number.
   // Replaced only by apply_reference_results(), never while scanning (UI thread).
   ReferenceSet references;
   std::vector<SongNotes> reference_notes;   // each reference's notes, for songs memory cannot tell apart
   SongTable song_table;
   std::string reference_dir() const;
   // Copies .spc files (a folder, zip or .spc) into this game's reference folder and reloads.
   int import_references(const std::string &source, std::string &error);
   // Downloads the game's SPC set from Zophar's Domain in the background.
   void download_references();
   void remove_references();
   bool loading_references() const { return loading_refs_; }
   std::string reference_message();
   // Adopts references loaded in the background; true when they changed (UI thread).
   bool apply_reference_results();
   // Lists every song of the ROM song table that has a reference, without playing the game
   // (scan thread).
   int add_songs_from_table();
   // Adds every reference song to the list, without song numbers: a music source needs no
   // more. Runs in the background like a scan.
   void list_reference_songs();

   // Songs, in value order. Lock songs_mutex while reading from another thread.
   std::mutex songs_mutex;
   std::vector<FoundSong> songs;
   FoundSong *find_song(uint32_t value);
   void rename_song(size_t index, const std::string &title);
   void remove_song(size_t index);
   void clear_library();

   // Scanning: from the scan start state, starts each song number and rips what plays.
   // Without a working way to start songs, the scan first watches the game boot to find
   // one: a RAM command the game polls, or the game's music routine.
   // With a ROM song table, the scan plays the table's song numbers instead of first..last.
   void start_scan(int first, int last);
   // When set, scans also save each raw rip (rip_XX.spc) and the scan start (before.spc) here.
   std::string keep_rips_dir;
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
   // Returns the frame (from the start) of the onset, or -1.
   int run_until_heard(int frames, std::vector<uint8_t> &onset);
   bool choose_song_address(const SongStart &s, bool &by_address);
   int start_score(const SongStart &s, const SongPrint *baseline);
   bool find_song_start(const SongPrint *baseline);
   bool choose_stub_area(const std::vector<uint8_t> &before);
   void add_song_locked(FoundSong song);
   // "same": titles of references that are versions of one song, the first one preferred;
   // a match to any of them, or nearly as good as one, takes that name. "elapsed": frames the
   // game ran from "before" to the rip, to tell a song that simply kept playing (the same music,
   // that much later) from one started again; negative when unknown.
   bool name_by_reference(FoundSong &song, const std::vector<uint8_t> *before, int elapsed,
         const std::vector<std::string> &same = std::vector<std::string>());
   void load_references_async(bool download);
   std::vector<uint8_t> spc_of_state(const std::vector<uint8_t> &state);
   std::string library_dir() const;
   void load_library();
   void save_library();
   void log(const std::string &line);

   bool open_ = false;
   std::string app_dir_;
   std::thread ref_thread_;
   std::atomic<bool> loading_refs_{false};
   std::mutex ref_mutex_;
   bool refs_ready_ = false;
   ReferenceSet pending_refs_;
   std::vector<SongNotes> pending_notes_;
   std::vector<uint8_t> before_notes_spc_;   // the sound CPU state before_notes_ measures
   SongNotes before_notes_;
   SongTable pending_table_;
   std::string ref_message_;
   std::vector<uint8_t> scan_before_spc_;   // the sound CPU at the scan start state
   std::vector<uint8_t> command_state_;     // the game when it last sent the sound CPU a command
   std::string rom_path_, game_name_, display_name_, profile_path_;
   SnesRom rom_;

   std::vector<uint8_t> scan_state_;
   uint32_t stub_ram_ = 0x1FF00;          // spare RAM for the code that calls music routines
   // Song addresses to check first, and where each came from (game database, RetroAchievements, static analysis).
   std::vector<std::pair<SongAddress, std::string>> address_hints_;
   SongAddress scan_address_;             // the scan thread's copies
   SongStart scan_start_;
   std::string scan_address_source_, scan_start_source_;
   std::atomic<bool> scan_changed_{false};
   std::thread worker_;
   std::atomic<bool> scanning_{false}, cancel_{false};
   std::atomic<int> scan_done_{0}, scan_total_{0}, scan_found_{0};
   std::mutex message_mutex_;
   std::string scan_message_;

   std::atomic<bool> querying_ra_{false};
   std::thread ra_thread_;

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
double song_distance(const SongPrint &a, const SongPrint &b);
bool same_song(const SongPrint &a, const SongPrint &b);
