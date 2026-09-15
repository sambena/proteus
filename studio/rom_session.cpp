// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rom_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "gme.h"
#include "platform.h"
#include "movie_learner.h"
#include "nes_tap.h"
#include "nsf_init.h"
#include "spc_rip.h"

extern "C" {
#include "engine.h"
#include "profile.h"
}

static const unsigned kMemoryIds[] = { RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_VIDEO_RAM };
static const int kBootFrames     = 720;   // where scans start when no moment was chosen
static const int kWatchFrames    = 2400;  // how long a scan watches the game boot for its music code
static const int kRoutineSettle  = 300;   // music routines may upload a song before it starts
static const int kPrintRate      = 32000;
static const int kPrintSeconds   = 10;
static const int kWindowMs       = 100;
static const char kLibraryHeader[] = "# proteus-studio library 4";
static const char kLibraryHeader3[] = "# proteus-studio library 3";
static const char kLibraryHeader2[] = "# proteus-studio library 2";
static const uint32_t kTrials[]  = { 1, 2, 3, 5, 8, 13 };
// NES rips are recordings: long enough to name by their notes, mono at the notes' rate.
static const double kNesRipSeconds = 20;
static const int kRipRate        = 32000;

static std::string hex2(uint32_t v)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "0x%02X", (unsigned)v);
   return buf;
}

static std::string hex4(uint32_t v)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "%04X", (unsigned)v);
   return buf;
}

static std::string today()
{
   char buf[16];
   time_t now = time(nullptr);
   strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&now));
   return buf;
}

// ---------------------------------------------------------------------------
// Song prints
// ---------------------------------------------------------------------------

bool analyze_music(const std::vector<uint8_t> &data, int track, SongPrint &print, std::string &error)
{
   std::vector<double> samples;
   if (!render_music(data, track, kPrintRate, kPrintSeconds, samples, error))
      return false;

   const int window = kPrintRate * kWindowMs / 1000;
   print.envelope.clear();
   print.brightness.clear();
   int prev = 0;
   for (size_t w = 0; w + window <= samples.size(); w += window)
   {
      double sum = 0, hp = 0;
      for (int i = 0; i < window; i++)
      {
         // The scale gme's 16-bit output had, which the loudness thresholds expect.
         int mono = (int)(samples[w + i] * 32768.0);
         sum += (double)mono * mono;
         hp += (double)(mono - prev) * (mono - prev);
         prev = mono;
      }
      print.envelope.push_back((float)std::sqrt(sum / window));
      print.brightness.push_back((float)std::sqrt(hp / window));
   }

   size_t n = print.envelope.size();
   size_t tail_from = n > 30 ? n - 30 : 0;
   double all = 0, tail = 0;
   for (size_t i = 0; i < n; i++)
   {
      all += print.envelope[i];
      if (i >= tail_from)
         tail += print.envelope[i];
   }
   print.loudness = n ? (float)(all / n) : 0;
   print.tail = n > tail_from ? (float)(tail / (n - tail_from)) : 0;
   return true;
}

// How differently two prints sound, 0 (the same) to 1, at the best shift of up to 2 seconds:
// the worse of the loudness and brightness distances.
double song_distance(const SongPrint &a, const SongPrint &b)
{
   int n = (int)std::min({ a.envelope.size(), b.envelope.size(), a.brightness.size(), b.brightness.size() });
   if (n < 60)
      return 1.0;
   const int kMaxShift = 20;   // windows: 2 seconds
   auto distance = [&](const std::vector<float> &x, const std::vector<float> &y, int shift) {
      double diff = 0, total = 0;
      for (int i = kMaxShift; i < n - kMaxShift; i++)
      {
         diff += std::fabs(x[i] - y[i + shift]);
         total += x[i] + y[i + shift];
      }
      return total <= 1 ? 0.0 : diff / total;
   };
   double best = 1.0;
   for (int shift = -kMaxShift; shift <= kMaxShift; shift++)
      best = std::min(best, std::max(distance(a.envelope, b.envelope, shift), distance(a.brightness, b.brightness, shift)));
   return best;
}

// The same song ripped a moment earlier or later has the same loudness and
// brightness over time, shifted. Different songs rarely match in both.
bool same_song(const SongPrint &a, const SongPrint &b)
{
   return song_distance(a, b) < 0.05;
}

// Music keeps playing to the end of the print; jingles start loud and stop.
static bool classify(const SongPrint &p, SongKind &kind)
{
   if (p.loudness < 60)
      return false;
   if (p.tail > 40 && p.tail >= p.loudness * 0.2f)
   {
      kind = SONG_MUSIC;
      return true;
   }
   size_t head = std::min<size_t>(20, p.envelope.size());
   double first = 0;
   for (size_t i = 0; i < head; i++)
      first += p.envelope[i];
   if (head && first / head > 120)
   {
      kind = SONG_JINGLE;
      return true;
   }
   return false;
}

static std::string default_title(uint32_t value, SongKind kind)
{
   return (kind == SONG_JINGLE ? "Jingle " : "Song ") + hex2(value);
}

// Makes `song` the reference song: its name, how it sounds, and its file (kept in data until
// the song is added), which plays it from its start.
static bool use_reference(FoundSong &song, const ReferenceSong &ref, std::string &error)
{
   SongPrint print;
   if (!analyze_music(ref.data, ref.track, print, error))
      return false;
   song.print = print;
   if (!classify(print, song.kind))
      song.kind = SONG_JINGLE;
   song.title = ref.title;
   song.reference = ref.title;
   song.data = ref.data;
   song.track = ref.track;
   return true;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

RomSession::RomSession() : app_dir_(app_data_dir()) {}

RomSession::~RomSession()
{
   close();
}

void RomSession::log(const std::string &line)
{
   std::lock_guard<std::mutex> lock(log_mutex_);
   log_.push_back((display_name_.empty() ? "" : display_name_ + ": ") + line);
}

std::vector<std::string> RomSession::take_log()
{
   std::lock_guard<std::mutex> lock(log_mutex_);
   std::vector<std::string> out;
   out.swap(log_);
   return out;
}

bool RomSession::open(const std::string &rom_path, const std::string &core_path,
      const std::string &system_dir, std::string &error)
{
   close();
   std::string save_dir = app_dir_ + "\\saves";
   make_dirs(save_dir);
   {
      std::lock_guard<std::mutex> lock(core_mutex);
      if (!core.load(core_path, rom_path, system_dir, save_dir, error))
         return false;
      const std::vector<uint8_t> &data = core.content_data();
      nes_ = data.size() >= 16 && !memcmp(data.data(), "NES\x1A", 4);

      // Once per run: learn this snes9x version's save state layout.
      static bool calibrated = false;
      if (!calibrated && !nes_)
      {
         for (int f = 0; f < 60; f++)
         {
            core.run_frame(0);
            core.audio().clear();
         }
         std::vector<uint8_t> state = core.save_state();
         calibrated = spc_calibrate_snes9x(state, [&](const std::vector<uint8_t> &st) {
            core.load_state(st);
            return core.save_state();
         });
         core.load_state(state);
         core.reset();
         if (!calibrated)
            log("could not confirm the snes9x save state layout; rips may miss the game's last sound command");
      }
   }

   rom_path_ = rom_path;
   const std::string &content = core.content_path();
   size_t hash = content.find('#');
   game_name_ = stem_of(hash == std::string::npos ? content : content.substr(hash + 1));
   rom_ = SnesRom();
   if (nes_)
      load_nes_identity(core.content_data(), rom_);
   else
      rom_.load(core.content_data());

   address = SongAddress();
   start = SongStart();
   silence = SongSilence();
   tap.clear();
   patch.clear();
   address_source.clear();
   start_source.clear();

   GameInfo info;
   bool in_db = GameDb::get().find(rom_.crc32, info);
   display_name_ = in_db && !info.name.empty() ? info.name : game_name_;
   if (in_db && info.song.known)
   {
      address = info.song;
      address_source = "game database";
   }
   if (in_db && info.start.kind != SongStart::NONE)
   {
      start = info.start;
      start_source = "game database";
   }
   if (in_db)
   {
      silence = info.silence;
      tap = info.tap;
      patch = info.patch;
   }

   profile_path_.clear();
   char found[2048];
   if (px_engine_find_profile(content.c_str(), system_dir.c_str(), found, sizeof(found)))
   {
      auto profile = std::make_unique<px_profile>();   // large; folder scans open games on their own thread
      px_profile &p = *profile;
      char err[1200];
      if (px_profile_load(&p, found, err, sizeof(err)))
      {
         profile_path_ = found;
         SongAddress from_profile;
         from_profile.known = true;
         for (int i = 0; i < 3; i++)
            if (kMemoryIds[i] == p.memory_id)
               from_profile.memory = i;
         from_profile.address = p.address;
         from_profile.size = (int)p.size;
         from_profile.latch = p.latch;
         from_profile.debounce = (int)p.debounce;
         from_profile.events = p.events;
         from_profile.events_address = p.events_address;
         if (p.pattern_length > 0)
         {
            from_profile.bytes.assign(p.pattern, p.pattern + p.pattern_length);
            from_profile.offset = (int)p.pattern_offset;
            for (unsigned i = 0; i < p.pattern_length; i++)
               from_profile.any.push_back(p.pattern_mask[i] == 0 && i != p.pattern_offset);
         }
         if (!address.known)
         {
            address = from_profile;
            address_source = "profile";
         }
         else if (from_profile.address != address.address)
            log("the profile follows $" + hex4(from_profile.address) + "; the game database says $" +
                hex4(address.address) + ", which Generate INI will use");
      }
      else
         log(std::string("profile ") + found + ": " + err);
   }
   // Static analysis only suggests addresses: scans check them against songs they start.
   run_static_analysis();

   // A command register that is also the song address starts songs when written.
   if (start.kind == SongStart::NONE && address.known && address.latch)
   {
      start.kind = SongStart::RAM;
      start.address = address.address;
      start.bytes = address.bytes;
      start.offset = address.offset;
      start_source = address_source;
   }

   scan_state_.clear();
   have_last_ = false;
   pending_rip_frames_ = -1;
   open_ = true;
   load_library();
   load_references_async(false);
   log("opened " + rom_path + " (ROM " + hex4(rom_.crc32 >> 16) + hex4(rom_.crc32 & 0xFFFF) + ")");
   return true;
}

void RomSession::run_static_analysis()
{
   if (!rom_.data.empty() && !nes_)
   {
      apu_analysis = analyze_snes_apu(rom_);
      if (apu_analysis.found)
      {
         log("static analysis: discovered " + std::to_string(apu_analysis.candidates.size()) +
             " APU write candidate(s)");
      }
   }
}

void RomSession::close()
{
   stop_scan();
   if (worker_.joinable())
      worker_.join();
   if (ra_thread_.joinable())
      ra_thread_.join();
   if (tool_thread_.joinable())
      tool_thread_.join();
   if (ref_thread_.joinable())
      ref_thread_.join();
   if (learn_thread_.joinable())
      learn_thread_.join();
   {
      std::lock_guard<std::mutex> lock(learn_mutex_);
      learn_songs_.clear();
      learn_seen_.clear();
      learn_broken_.clear();
      learn_candidates_ = -1;
      learn_left_.clear();
      learned_ready_ = false;
      learned_values_.clear();
   }
   live_frames_ = 0;
   live_audio_.clear();
   live_phase_ = 0;
   references.clear();
   reference_notes.clear();
   song_table = SongTable();
   nsf_table.clear();
   {
      std::lock_guard<std::mutex> lock(ref_mutex_);
      refs_ready_ = false;
      ref_message_.clear();
   }
   command_state_.clear();
   {
      std::lock_guard<std::mutex> ra_lock(ra_mutex);
      ra_result = RaLookupResult();
   }
   std::lock_guard<std::mutex> lock(core_mutex);
   core.unload();
   apu_analysis = ApuAnalysisResult();
   std::lock_guard<std::mutex> songs_lock(songs_mutex);
   songs.clear();
   open_ = false;
}

void RomSession::query_retroachievements()
{
   if (!open_ || rom_.md5.empty() || querying_ra_)
      return;
   if (ra_thread_.joinable())
      ra_thread_.join();

   querying_ra_ = true;
   {
      std::lock_guard<std::mutex> lock(ra_mutex);
      ra_result = RaLookupResult();
      ra_result.status = RaLookupStatus::SEARCHING;
      ra_result.message = "Querying RetroAchievements for ROM " + rom_.md5.substr(0, 8) + "...";
   }

   std::string md5 = rom_.md5;
   ra_thread_ = std::thread([this, md5]() {
      RaLookupResult res = ra_lookup_music_notes(md5);
      {
         std::lock_guard<std::mutex> lock(ra_mutex);
         ra_result = res;
      }
      querying_ra_ = false;
      log(res.message);
   });
}

void RomSession::save_to_game_db(const std::string &note)
{
   if (!open_ || !rom_.crc32)
      return;
   GameInfo info;
   GameDb::get().find(rom_.crc32, info);
   info.crc32 = rom_.crc32;
   if (info.name.empty())
      info.name = display_name_;
   info.song = address;
   info.start = start;
   info.silence = silence;
   info.tap = tap;
   info.patch = patch;
   info.note = note + " " + today();
   GameDb::get().put(info);
}

FoundSong *RomSession::find_song(uint32_t value)
{
   for (auto &s : songs)
      if (s.has_value && s.value == value)
         return &s;
   return nullptr;
}

void RomSession::rename_song(size_t index, const std::string &title)
{
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      if (index >= songs.size())
         return;
      songs[index].title = title;
   }
   save_library();
}

void RomSession::remove_song(size_t index)
{
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      if (index >= songs.size())
         return;
      songs.erase(songs.begin() + index);
   }
   save_library();
}

void RomSession::clear_library()
{
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      songs.clear();
   }
   std::string dir = library_dir();
   for (const auto &file : list_files(dir))
   {
      std::string p = dir + "\\" + file;
      std::remove(p.c_str());
   }
   std::string st = dir + "\\songs.txt";
   std::remove(st.c_str());
}

// ---------------------------------------------------------------------------
// Reference songs: %APPDATA%/ProteusStudio/reference/<ROM CRC32>/*.spc
// ---------------------------------------------------------------------------

std::string RomSession::reference_dir() const
{
   return app_dir_ + "\\reference\\" + hex4(rom_.crc32 >> 16) + hex4(rom_.crc32 & 0xFFFF);
}

std::string RomSession::reference_message()
{
   std::lock_guard<std::mutex> lock(ref_mutex_);
   return ref_message_;
}

// The RAM requests the first .nsf of a reference set writes for its songs (see nsf_table).
static std::map<uint32_t, std::map<uint8_t, int>> nsf_request_table(const ReferenceSet &refs)
{
   std::map<uint32_t, std::map<uint8_t, int>> table;
   for (size_t r = 0; r < refs.size(); r++)
   {
      const ReferenceSong &ref = refs.song(r);
      if (ref.data.size() < 5 || (memcmp(ref.data.data(), "NESM\x1A", 5) && memcmp(ref.data.data(), "NSFE", 4)))
         continue;
      std::string err;
      for (const auto &q : nsf_song_requests(ref.data, err))
      {
         if (q.address >= 0x100 && q.address < 0x200)
            continue;
         for (const auto &v : q.song_values)
            for (size_t i = 0; i < refs.size(); i++)
               if (refs.song(i).path == ref.path && refs.song(i).track == v.first && v.second)
                  table[q.address].emplace(v.second, (int)i);   // the first song writing a value names it
      }
      break;
   }
   // A request says which song; bytes written with fewer than two values say nothing.
   for (auto it = table.begin(); it != table.end();)
      it = it->second.size() < 2 ? table.erase(it) : std::next(it);
   return table;
}

// Loads the reference folder, after downloading into it, and looks for the ROM's song table.
void RomSession::load_references_async(bool download)
{
   if (loading_refs_ || !open_)
      return;
   if (ref_thread_.joinable())
      ref_thread_.join();
   loading_refs_ = true;
   std::string dir = reference_dir();
   std::vector<std::string> names = { game_name_, display_name_ };
   ref_thread_ = std::thread([this, dir, names, download]() {
      auto say = [this](const std::string &m) {
         std::lock_guard<std::mutex> lock(ref_mutex_);
         ref_message_ = m;
      };
      std::string err;
      if (download)
      {
         int n = download_reference_songs(names, dir, say, err,
               nes_ ? REFERENCES_ZOPHAR_NES : REFERENCES_ZOPHAR | REFERENCES_SNESMUSIC);
         if (n <= 0)
         {
            say(err);
            log(err);
            loading_refs_ = false;
            return;
         }
         log("downloaded " + std::to_string(n) + " reference songs to " + dir);
      }
      ReferenceSet refs;
      refs.load(dir, err);
      SongTable table;
      std::vector<SongNotes> notes;
      std::string summary;
      std::map<uint32_t, std::map<uint8_t, int>> nsf_table = nsf_request_table(refs);
      if (!refs.empty())
      {
         say("Looking for the song table in the ROM...");
         table = refs.find_song_table(rom_);
         say("Listening to the reference songs...");
         notes.resize(refs.size());
         for (size_t i = 0; i < refs.size(); i++)
            music_notes(refs.song(i).data, refs.song(i).track, notes[i], err);
         summary = std::to_string(refs.size()) + " reference songs";
         if (table.found)
         {
            char where[64];
            snprintf(where, sizeof(where), "ROM 0x%X", (unsigned)table.rom_offset);
            if (table.cpu_address)
               snprintf(where + strlen(where), sizeof(where) - strlen(where), ", $%06X", (unsigned)table.cpu_address);
            log("song table at " + std::string(where) + ": " + std::to_string(table.entries.size()) + " songs, " +
                std::to_string(table.matched) + " of them with a reference");
            summary += "; the ROM lists " + std::to_string(table.entries.size()) + " songs";
         }
         else
            log("no song table found in the ROM; scans name the songs they find");
      }
      std::lock_guard<std::mutex> lock(ref_mutex_);
      pending_refs_ = std::move(refs);
      pending_table_ = table;
      pending_nsf_table_ = nsf_table;
      pending_notes_.swap(notes);
      refs_ready_ = true;
      ref_message_ = summary;
      loading_refs_ = false;
   });
}

bool RomSession::apply_reference_results()
{
   if (scanning_ || learning_busy_)
      return false;
   std::lock_guard<std::mutex> lock(ref_mutex_);
   if (!refs_ready_)
      return false;
   refs_ready_ = false;
   references = std::move(pending_refs_);
   song_table = pending_table_;
   nsf_table = pending_nsf_table_;
   reference_notes.swap(pending_notes_);
   pending_notes_.clear();
   pending_refs_.clear();

   // Songs listed before the references came are named from the ROM song table, which
   // numbers songs the way the game's music command does. Titles someone typed are kept.
   int named = 0;
   if (song_table.found)
   {
      std::lock_guard<std::mutex> songs_lock(songs_mutex);
      for (auto &s : songs)
      {
         if (!s.has_value || !s.reference.empty() || s.value >= song_table.entries.size() || song_table.entries[s.value] < 0)
            continue;
         const std::string &title = references.song(song_table.entries[s.value]).title;
         if (s.title == default_title(s.value, SONG_MUSIC) || s.title == default_title(s.value, SONG_JINGLE))
            s.title = title;
         s.reference = title;
         named++;
      }
   }
   if (named)
   {
      save_library();
      log("named " + std::to_string(named) + " listed songs from the ROM song table");
   }
   return true;
}

int RomSession::import_references(const std::string &source, std::string &error)
{
   if (!open_)
      return -1;
   if (loading_refs_)
   {
      error = "reference songs are still loading";
      return -1;
   }
   int n = import_reference_songs(source, reference_dir(), error);
   if (n > 0)
   {
      log("imported " + std::to_string(n) + " reference songs from " + source);
      load_references_async(false);
   }
   return n;
}

void RomSession::download_references()
{
   load_references_async(true);
}

void RomSession::remove_references()
{
   if (loading_refs_ || scanning_)
      return;
   std::string dir = reference_dir();
   for (const auto &file : list_files(dir))
   {
      std::string p = dir + "\\" + file;
      std::remove(p.c_str());
   }
   references.clear();
   reference_notes.clear();
   song_table = SongTable();
   nsf_table.clear();
   std::lock_guard<std::mutex> lock(ref_mutex_);
   ref_message_.clear();
}

// Lists the song table's songs that have a reference, from the references alone. Measuring
// each song takes a moment, so this runs on the scan thread.
int RomSession::add_songs_from_table()
{
   if (!song_table.found)
      return 0;
   int added = 0;
   std::string err;
   for (size_t v = 0; v < song_table.entries.size() && !cancel_; v++)
   {
      int r = song_table.entries[v];
      if (r < 0)
         continue;
      {
         std::lock_guard<std::mutex> lock(songs_mutex);
         FoundSong *existing = find_song((uint32_t)v);
         if (existing && !existing->reference.empty())
            continue;
      }
      FoundSong song;
      song.value = (uint32_t)v;
      if (!use_reference(song, references.song(r), err))
         continue;
      std::lock_guard<std::mutex> lock(songs_mutex);
      add_song_locked(song);
      added++;
   }
   if (added)
      log("listed " + std::to_string(added) + " songs from the ROM song table");
   return added;
}

void RomSession::list_reference_songs()
{
   if (scanning_ || !open_ || references.empty())
      return;
   if (worker_.joinable())
      worker_.join();
   cancel_ = false;
   scanning_ = true;
   scan_changed_ = false;
   scan_done_ = 0;
   scan_found_ = 0;
   scan_total_ = (int)references.size();
   worker_ = std::thread([this]() {
      std::string err;
      for (size_t i = 0; i < references.size() && !cancel_; i++, scan_done_++)
      {
         const ReferenceSong &ref = references.song(i);
         {
            std::lock_guard<std::mutex> lock(songs_mutex);
            bool listed = false;
            for (const auto &s : songs)
               listed = listed || s.reference == ref.title;
            if (listed)
               continue;
         }
         set_scan_message("Adding \"" + ref.title + "\"...");
         FoundSong song;
         song.has_value = false;
         if (!use_reference(song, ref, err))
            continue;
         std::lock_guard<std::mutex> lock(songs_mutex);
         add_song_locked(song);
         scan_found_++;
      }
      save_library();
      std::string summary = "Added " + std::to_string((int)scan_found_) + " reference songs.";
      set_scan_message(summary);
      log(summary);
      scanning_ = false;
   });
}

// Names a rip after the reference song it matches and keeps the reference's .spc, which
// plays the song from its start. False when no reference matches.
bool RomSession::name_by_reference(FoundSong &song, const std::vector<uint8_t> *before, int elapsed,
      const std::vector<std::string> &same)
{
   if (references.empty())
      return false;
   ReferenceSet::Match m = references.match_spc(song.data, before);

   // Some drivers load a whole group of songs at once and start one by moving a pointer, so
   // little memory tells those songs apart. A weak memory match must also sound like its
   // song; without a memory match, a song whose notes clearly match one reference is it.
   // NES rips are recordings of the game compared with another emulator's playback, so their
   // notes match less closely (a sparse song, Super Mario Bros.' Underground, at 0.15 to 0.22), but
   // well ahead of every other song; a recording of the music already playing matches it closely.
   const double kStrongMemory = 4.0, kNotesMatch = nes_ ? 0.25 : 0.03, kNotesLead = nes_ ? 0.05 : 0.03;
   const double kSameMusic = 0.03;
   if ((m.index < 0 || m.score < kStrongMemory) && reference_notes.size() == references.size())
   {
      SongNotes notes;
      std::string err;
      if (!spc_notes(song.data, notes, err))
         return false;
      // What was playing before, still playing exactly as far along as the time that passed:
      // the number changed nothing. The same song started over lines up elsewhere.
      if (before)
      {
         if (before_notes_spc_ != *before)
         {
            before_notes_spc_ = *before;
            if (!spc_notes(*before, before_notes_, err))
               before_notes_ = SongNotes();
         }
         int shift = 0;
         if (notes_distance(notes, before_notes_, &shift) < kSameMusic)
         {
            if (elapsed < 0)
               return false;
            double expected = elapsed / 60.0 / notes_frame_seconds();
            if (std::fabs(shift - expected) <= 4)
               return false;
         }
      }
      std::vector<std::pair<double, int>> ranked;
      for (size_t i = 0; i < references.size(); i++)
         ranked.push_back({ notes_distance(notes, reference_notes[i]), (int)i });
      std::sort(ranked.begin(), ranked.end());
      auto stem = [&](int i) {
         const std::string &t = references.song(i).title;
         return t.substr(0, t.find(" ("));
      };
      if (m.index >= 0)
      {
         bool heard = false;
         for (size_t k = 0; k < 3 && k < ranked.size(); k++)
            for (int c : m.close)
               heard = heard || stem(ranked[k].second) == stem(c);
         if (!heard)
            return false;
      }
      else
      {
         if (ranked.empty() || ranked[0].first >= kNotesMatch)
            return false;
         double next = 1.0;
         for (const auto &r : ranked)
            if (stem(r.second) != stem(ranked[0].second))
            {
               next = r.first;
               break;
            }
         if (next - ranked[0].first < std::max(kNotesLead, nes_ ? ranked[0].first * 0.5 : 0.0))
            return false;
         m.index = ranked[0].second;
         m.close.clear();
         for (const auto &r : ranked)
            if (stem(r.second) == stem(ranked[0].second))
               m.close.push_back(r.second);
      }
   }
   if (m.index < 0)
      return false;
   if (!same.empty())
      for (int i : m.close)
         if (std::find(same.begin(), same.end(), references.song(i).title) != same.end())
         {
            for (size_t r = 0; r < references.size(); r++)
               if (references.song(r).title == same.front())
                  m.index = (int)r;
            break;
         }
   std::string err;
   return use_reference(song, references.song(m.index), err);
}

// The music playing at `state`, playable without the game: an .spc, or for NES a recording of
// what follows (the core returns to where it was; core_mutex held).
std::vector<uint8_t> RomSession::spc_of_state(const std::vector<uint8_t> &state)
{
   std::vector<uint8_t> spc;
   std::string err;
   if (nes_)
   {
      if (state.empty())
         return spc;
      std::vector<uint8_t> resume = core.save_state();
      core.load_state(state);
      spc = record_music(kNesRipSeconds);
      core.load_state(resume);
      return spc;
   }
   if (state.empty() || !spc_from_snes9x_state(state, SpcTags(), spc, nullptr, err))
      spc.clear();
   return spc;
}

// Downmixes and resamples the core's audio to kRipRate mono.
static void append_mono(const std::vector<int16_t> &stereo, double from_rate, double &phase, std::vector<int16_t> &out)
{
   double step = from_rate / kRipRate;
   size_t frames = stereo.size() / 2;
   for (; phase < (double)frames; phase += step)
   {
      size_t k = (size_t)phase;
      out.push_back((int16_t)((stereo[2 * k] + stereo[2 * k + 1]) / 2));
   }
   phase -= (double)frames;
}

std::vector<uint8_t> RomSession::record_music(double seconds)
{
   std::vector<int16_t> mono;
   double phase = 0;
   core.audio().clear();
   int frames = (int)(seconds * (core.fps() > 0 ? core.fps() : 60.0));
   mono.reserve((size_t)(seconds * kRipRate) + 1024);
   for (int f = 0; f < frames; f++)
   {
      core.run_frame(0);
      append_mono(core.audio(), core.sample_rate(), phase, mono);
      core.audio().clear();
   }
   return wav_file(mono, kRipRate);
}

// ---------------------------------------------------------------------------
// Library on disk: %APPDATA%/ProteusStudio/library/<game>/songs.txt + rips
// ---------------------------------------------------------------------------

// The file extension for a song's bytes.
static const char *music_extension(const std::vector<uint8_t> &bytes)
{
   auto starts = [&](const char *magic, size_t n) { return bytes.size() >= n && !memcmp(bytes.data(), magic, n); };
   if (starts("RIFF", 4))
      return "wav";
   if (starts("NESM\x1A", 5))
      return "nsf";
   if (starts("NSFE", 4))
      return "nsfe";
   return "spc";
}

std::string RomSession::library_dir() const
{
   return app_dir_ + "\\library\\" + sanitize_filename(game_name_);
}

void RomSession::save_library()
{
   std::string dir = library_dir();
   make_dirs(dir);
   std::ostringstream out;
   out << kLibraryHeader << "\n# value\thas_value\tkind\tfile\ttitle\treference\ttrack\tloudness tail count envelope... brightness...\n";
   std::lock_guard<std::mutex> lock(songs_mutex);
   for (const auto &s : songs)
   {
      out << s.value << '\t' << (s.has_value ? 1 : 0) << '\t' << (int)s.kind << '\t'
          << file_name(s.path) << '\t' << s.title << '\t' << s.reference << '\t' << s.track << '\t'
          << s.print.loudness << ' ' << s.print.tail
          << ' ' << s.print.envelope.size();
      for (float e : s.print.envelope)
         out << ' ' << (int)std::lround(e);
      for (float e : s.print.brightness)
         out << ' ' << (int)std::lround(e);
      out << '\n';
   }
   write_text(dir + "\\songs.txt", out.str());
}

void RomSession::load_library()
{
   std::string dir = library_dir();
   std::string text = read_text(dir + "\\songs.txt");
   // Libraries from older versions measured songs differently; measure them again.
   bool v4 = text.compare(0, strlen(kLibraryHeader), kLibraryHeader) == 0;
   bool v3 = v4 || text.compare(0, strlen(kLibraryHeader3), kLibraryHeader3) == 0;
   bool current = v3 || text.compare(0, strlen(kLibraryHeader2), kLibraryHeader2) == 0;
   const int fields = v4 ? 7 : v3 ? 6 : 5;
   std::istringstream in(text);
   std::string line;
   std::unique_lock<std::mutex> lock(songs_mutex);
   songs.clear();
   while (std::getline(in, line))
   {
      if (line.empty() || line[0] == '#')
         continue;
      if (line.back() == '\r')
         line.pop_back();
      std::vector<std::string> f;
      size_t pos = 0;
      for (int i = 0; i < fields; i++)
      {
         size_t tab = line.find('\t', pos);
         if (tab == std::string::npos)
            break;
         f.push_back(line.substr(pos, tab - pos));
         pos = tab + 1;
      }
      if ((int)f.size() < fields)
         continue;
      FoundSong s;
      s.value = (uint32_t)strtoul(f[0].c_str(), nullptr, 10);
      s.has_value = f[1] == "1";
      s.kind = f[2] == "1" ? SONG_JINGLE : SONG_MUSIC;
      s.path = dir + "\\" + f[3];
      s.title = f[4];
      if (v3)
         s.reference = f[5];
      if (v4)
         s.track = atoi(f[6].c_str());
      if (!file_exists(s.path))
         continue;
      std::istringstream nums(line.substr(pos));
      size_t count = 0;
      nums >> s.print.loudness >> s.print.tail >> count;
      float e;
      for (size_t i = 0; current && i < count && nums >> e; i++)
         s.print.envelope.push_back(e);
      for (size_t i = 0; current && i < count && nums >> e; i++)
         s.print.brightness.push_back(e);
      if (!current || s.print.brightness.size() != count)
      {
         std::vector<uint8_t> spc;
         std::string err;
         if (!read_file_bytes(s.path, spc) || !analyze_music(spc, s.track, s.print, err))
            continue;
      }
      songs.push_back(s);
   }
   lock.unlock();
   if (!v4 && !songs.empty())
      save_library();
}

// ---------------------------------------------------------------------------
// Ripping
// ---------------------------------------------------------------------------

bool RomSession::read_song_value(const SongAddress &a, uint32_t &value)
{
   size_t size = 0;
   const uint8_t *ram = core.memory(kMemoryIds[a.memory], &size);
   return read_song_address(a, ram, size, value);
}

// Builds a song from the game's state; the core must be at `state` (core_mutex held).
bool RomSession::rip_state(const std::vector<uint8_t> &state, uint32_t value, bool has_value,
      FoundSong &out, std::string &error)
{
   std::vector<uint8_t> current = state, spc;
   SpcState info{};
   SpcTags tags;
   tags.game = display_name_;
   if (nes_)
      spc = record_music(kNesRipSeconds);
   for (int tries = 0; !nes_; tries++)
   {
      if (!spc_from_snes9x_state(current, tags, spc, &info, error))
         return false;
      // Stopped inside an instruction: a frame later it is usually between two.
      if (!info.mid_opcode || tries >= 30)
         break;
      core.run_frame(0);
      core.audio().clear();
      current = core.save_state();
   }
   out = FoundSong();
   out.value = value;
   out.has_value = has_value;
   if (!analyze_spc(spc, out.print, error))
      return false;
   if (!classify(out.print, out.kind))
   {
      error = "silent";
      return false;
   }
   // Keep the bytes until the song is accepted.
   out.data = spc;
   return true;
}

void RomSession::add_song_locked(FoundSong song)
{
   std::string dir = library_dir();
   make_dirs(dir);
   char name[64];
   const char *ext = music_extension(song.data);
   if (song.has_value)
      snprintf(name, sizeof(name), "%02X.%s", (unsigned)song.value, ext);
   else
   {
      unsigned n = 1;
      do snprintf(name, sizeof(name), "rip_%u.%s", n++, ext);
      while (file_exists(dir + "\\" + name));
   }
   // A song number keeps one file, whatever kind it was before.
   if (song.has_value)
      for (const char *other : { "spc", "wav", "nsf", "nsfe" })
         if (strcmp(other, ext))
         {
            char old[64];
            snprintf(old, sizeof(old), "%02X.%s", (unsigned)song.value, other);
            std::remove((dir + "\\" + old).c_str());
         }
   std::string path = dir + "\\" + name;
   FILE *f = px_fopen(path.c_str(), "wb");
   if (!f)
      return;
   fwrite(song.data.data(), 1, song.data.size(), f);
   fclose(f);
   song.path = path;
   song.data.clear();
   song.data.shrink_to_fit();

   for (auto it = songs.begin(); it != songs.end(); ++it)
      if (song.has_value && it->has_value && it->value == song.value)
      {
         if (song.title.empty())
            song.title = it->title;
         songs.erase(it);
         break;
      }
   songs.push_back(song);
   std::stable_sort(songs.begin(), songs.end(), [](const FoundSong &a, const FoundSong &b) {
      if (a.has_value != b.has_value)
         return a.has_value;
      return a.value < b.value;
   });
}

// ---------------------------------------------------------------------------
// Starting songs
// ---------------------------------------------------------------------------

// Where a block starts inside a snes9x snapshot, after its "TAG:LLLLLL:" header.
static size_t snapshot_block(const std::vector<uint8_t> &st, const char *tag)
{
   size_t pos = 14;
   while (pos + 11 <= st.size() && st[pos + 3] == ':')
   {
      size_t n = strtoul(std::string((const char*)&st[pos + 4], 6).c_str(), nullptr, 10);
      if (!memcmp(&st[pos], tag, 3))
         return pos + 11;
      pos += 11 + n;
   }
   return 0;
}

// The CPU address of a work RAM offset ($7E:0000-$7F:FFFF).
static uint32_t ram_cpu_address(uint32_t offset)
{
   return 0x7E0000 + offset;
}

// Loads the scan start state and starts song `value` (core_mutex held).
bool RomSession::start_song(const SongStart &s, uint32_t value)
{
   size_t ram_size = 0;
   if (s.kind == SongStart::RAM)
   {
      core.load_state(scan_state_);
      uint8_t *ram = core.memory_mut(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      size_t length = s.bytes.empty() ? 1 : s.bytes.size();
      if (!ram || (uint64_t)s.address + length > ram_size)
         return false;
      if (s.bytes.empty())
         ram[s.address] = (uint8_t)value;
      else
      {
         memcpy(ram + s.address, s.bytes.data(), s.bytes.size());
         ram[s.address + s.offset] = (uint8_t)value;
      }
      return true;
   }
   if (s.kind != SongStart::ROUTINE)
      return false;

   // Code in spare RAM that sets up the call like the game does, calls the routine,
   // and then waits forever while the song plays:
   //    SEP #$30 / LDA #b : STA $7Exxxx ... / [LDA #song] / JSL routine / BRA *
   // A routine ending in RTS is entered with JML after pushing a return through an RTL
   // byte in its own bank:  PHK / PEA wait-1 / PEA rtl-1 / JML routine.
   uint32_t stub = ram_cpu_address(stub_ram_);
   uint32_t rtl = 0;
   if (s.jsr)
   {
      uint8_t bank = s.address >> 16;
      for (uint32_t a = 0x8000; a <= 0xFFFF && !rtl; a++)
      {
         const uint8_t *b = rom_.at((uint32_t)bank << 16 | a);
         if (b && *b == 0x6B)
            rtl = (uint32_t)bank << 16 | a;
      }
      if (!rtl)
         return false;
   }
   std::vector<uint8_t> code;
   for (int pass = 0; pass < 2; pass++)
   {
      uint32_t wait = stub + (uint32_t)code.size() - 2;   // known on the second pass
      code.clear();
      code.insert(code.end(), { 0xE2, 0x30 });
      if (s.fill_block)
         for (size_t i = 0; i < s.bytes.size(); i++)
         {
            uint32_t at = ram_cpu_address(s.block + (uint32_t)i);
            uint8_t b = (int)i == s.offset ? (uint8_t)value : s.bytes[i];
            code.insert(code.end(), { 0xA9, b, 0x8F, (uint8_t)at, (uint8_t)(at >> 8), (uint8_t)(at >> 16) });
         }
      if (s.jsr)
         code.insert(code.end(), { 0x4B, 0xF4, (uint8_t)(wait - 1), (uint8_t)((wait - 1) >> 8),
                                   0xF4, (uint8_t)(rtl - 1), (uint8_t)((rtl - 1) >> 8) });
      if (s.song_in_a)
         code.insert(code.end(), { 0xA9, (uint8_t)value });
      code.insert(code.end(), { (uint8_t)(s.jsr ? 0x5C : 0x22), (uint8_t)s.address, (uint8_t)(s.address >> 8), (uint8_t)(s.address >> 16) });
      code.insert(code.end(), { 0x80, 0xFE });
   }

   std::vector<uint8_t> st = scan_state_;
   size_t reg = snapshot_block(st, "REG");
   if (!reg || reg + 16 > st.size())
      return false;
   // REG: PB DB P(2) A(2) D(2) S(2) X(2) Y(2) PC(2), big-endian. The emulation flag in
   // P's high byte is kept; M, X and I are set.
   st[reg + 0] = (uint8_t)(stub >> 16);
   st[reg + 3] = 0x34;
   st[reg + 14] = (uint8_t)(stub >> 8);
   st[reg + 15] = (uint8_t)stub;
   core.load_state(st);
   uint8_t *ram = core.memory_mut(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
   if (!ram || stub_ram_ + code.size() > ram_size)
      return false;
   memcpy(ram + stub_ram_, code.data(), code.size());
   return true;
}

// Picks RAM the game left alone between the scan start state and now for the call code.
bool RomSession::choose_stub_area(const std::vector<uint8_t> &before)
{
   size_t size = 0;
   const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
   if (!ram || before.size() != size || size < 0x20000)
      return false;
   const size_t kNeed = 96;
   size_t run = 0;
   for (size_t i = size; i-- > 0x10000;)
   {
      bool untouched = ram[i] == before[i] && (i + 1 >= size || ram[i] == ram[i + 1]);
      run = untouched ? run + 1 : 0;
      if (run >= kNeed)
      {
         stub_ram_ = (uint32_t)i + 8;
         return true;
      }
   }
   return false;
}

void RomSession::run_frames(int frames)
{
   for (int f = 0; f < frames; f++)
   {
      core.run_frame(0);
      core.audio().clear();
   }
}

// Runs `frames` frames. When the game falls silent and then sound starts (a song loading,
// then playing), `onset` receives the state at its first sound.
int RomSession::run_until_heard(int frames, std::vector<uint8_t> &onset)
{
   int onset_frame = -1;
   onset.clear();
   int quiet = 0;
   for (int f = 0; f < frames; f++)
   {
      core.run_frame(0);
      int peak = 0;
      for (int16_t sample : core.audio())
         peak = std::max(peak, std::abs((int)sample));
      core.audio().clear();
      if (!onset.empty())
         continue;
      if (peak < 200)
         quiet++;
      else
      {
         if (quiet >= 6)
         {
            onset = core.save_state();
            onset_frame = f + 1;
         }
         quiet = 0;
      }
   }
   return onset_frame;
}

// Finds the RAM the game keeps its song number in, by starting a few songs and reading
// every byte back. A byte that holds exactly the song started, and keeps it, is where the
// game itself records it, so Proteus sees the same numbers while the game runs. Addresses
// from the game database, RetroAchievements notes and static analysis are preferred when
// they pass; one that holds a different but consistent number for each song numbers the
// songs its own way. Failing both, Proteus follows the command itself.
bool RomSession::choose_song_address(const SongStart &s, bool &by_address)
{
   by_address = false;
   size_t ram_size = 0;
   core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);

   struct Trial
   {
      uint32_t song;
      std::vector<uint8_t> settled, later;
   };
   std::vector<Trial> trials;
   for (uint32_t v : kTrials)
   {
      if (cancel_ || !start_song(s, v))
         break;
      Trial t;
      t.song = v;
      run_frames(s.settle_frames);
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      t.settled.assign(ram, ram + (ram ? ram_size : 0));
      run_frames(120);
      ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      t.later.assign(ram, ram + (ram ? ram_size : 0));
      trials.push_back(std::move(t));
   }

   // Bytes the start itself writes hold the song number whether the game keeps it or not.
   auto written = [&](uint32_t at) {
      uint32_t from = s.kind == SongStart::RAM ? s.address : s.fill_block ? s.block : 0xFFFFFFFF;
      size_t length = std::max<size_t>(1, s.bytes.size());
      return (at >= from && at < from + length) || (s.kind == SongStart::ROUTINE && at >= stub_ram_ && at < stub_ram_ + 128);
   };

   std::vector<uint32_t> exact;
   if (trials.size() >= 3)
      for (uint32_t at = 0; at < ram_size; at++)
      {
         bool holds = !written(at);
         for (size_t i = 0; holds && i < trials.size(); i++)
            holds = trials[i].settled[at] == (uint8_t)trials[i].song && trials[i].later[at] == (uint8_t)trials[i].song;
         if (holds)
            exact.push_back(at);
      }
   if (!exact.empty())
   {
      std::string list;
      for (size_t i = 0; i < exact.size() && i < 12; i++)
         list += " $" + hex4(exact[i]);
      log("RAM holding the song number:" + list + (exact.size() > 12 ? " ..." : ""));
   }

   // A different number per song, held steadily: the game's own numbering.
   auto consistent = [&](const SongAddress &a) {
      std::vector<uint32_t> seen;
      for (const auto &t : trials)
      {
         uint32_t x = 0, y = 0;
         if (!read_song_address(a, t.settled.data(), t.settled.size(), x) ||
             !read_song_address(a, t.later.data(), t.later.size(), y) || x != y ||
             std::find(seen.begin(), seen.end(), x) != seen.end())
            return false;
         seen.push_back(x);
      }
      return seen.size() >= 3;
   };

   auto adopt = [&](uint32_t at, const std::string &source) {
      scan_address_ = SongAddress();
      scan_address_.known = true;
      scan_address_.address = at;
      scan_address_.debounce = 2;
      scan_address_source_ = source;
      scan_changed_ = true;
   };

   std::vector<std::pair<SongAddress, std::string>> hints = address_hints_;
   for (const auto &h : hints)
      if (h.first.memory == 0 && h.first.bytes.empty() && h.first.size == 1 &&
          std::find(exact.begin(), exact.end(), h.first.address) != exact.end())
      {
         adopt(h.first.address, "scan, confirming " + h.second);
         log("songs are kept at $" + hex4(h.first.address) + " (" + h.second + ", confirmed)");
         return true;
      }
   if (!exact.empty())
   {
      // Low work RAM is mirrored into every bank the game's code runs in; prefer it.
      adopt(exact.front(), "scan");
      log("songs are kept at $" + hex4(exact.front()) + " (confirmed)");
      return true;
   }
   for (const auto &h : hints)
      if (h.first.memory == 0 && !written(h.first.address) && trials.size() >= 3 && consistent(h.first))
      {
         scan_address_ = h.first;
         scan_address_source_ = "scan, confirming " + h.second;
         scan_changed_ = true;
         by_address = true;
         log("songs are kept at " + describe_song_address(h.first) + " with their own numbers (" + h.second + ", confirmed)");
         return true;
      }

   if (scan_address_.known)
      log("the song address " + describe_song_address(scan_address_) + " does not follow the songs started");
   if (s.kind == SongStart::RAM || (s.kind == SongStart::ROUTINE && s.fill_block))
   {
      // Proteus follows the command. A routine's command block also carries values that
      // change from song to song (Chrono Trigger: 10 song FF 05, 10 song EC 0F), so only
      // its first byte, the command, and the song number are matched.
      adopt(s.kind == SongStart::RAM ? s.address : s.block, "scan (the command)");
      scan_address_.bytes = s.bytes;
      scan_address_.offset = s.offset;
      scan_address_.size = s.bytes.empty() ? 1 : (int)s.bytes.size();
      if (s.kind == SongStart::ROUTINE)
         for (size_t i = 0; i < s.bytes.size(); i++)
            scan_address_.any.push_back(i != 0 && (int)i != s.offset);
      scan_address_.latch = true;
      scan_address_.debounce = 1;
      log("profiles will follow the command " + describe_song_address(scan_address_));
      return true;
   }
   log("found no song address; songs are numbered as they were started");
   return false;
}

// Starts each trial song and counts the different songs heard (music 2, jingles 1). With
// reference songs, only those count (3 each), and only when at least two differ: a game that
// moves on to its next song by itself plays the same one whatever number was sent.
int RomSession::start_score(const SongStart &s, const SongPrint *baseline, const std::vector<uint32_t> *values)
{
   std::vector<uint32_t> trials(kTrials, kTrials + sizeof(kTrials) / sizeof(kTrials[0]));
   if (values)
      trials = *values;
   std::vector<SongPrint> heard;
   std::vector<std::string> named;
   int score = 0;
   std::string err;
   for (uint32_t v : trials)
   {
      if (cancel_ || !start_song(s, v))
         break;
      run_frames(s.settle_frames);
      FoundSong song;
      if (!rip_state(core.save_state(), v, true, song, err))
         continue;
      // A song from the reference set is certainly a song.
      if (name_by_reference(song, scan_before_spc_.empty() ? nullptr : &scan_before_spc_, s.settle_frames))
      {
         if (std::find(named.begin(), named.end(), song.reference) == named.end())
         {
            named.push_back(song.reference);
            score += 3;
         }
         continue;
      }
      if (baseline && same_song(song.print, *baseline))
         continue;
      bool dup = false;
      for (const auto &p : heard)
         dup = dup || same_song(p, song.print);
      if (dup)
         continue;
      heard.push_back(song.print);
      score += song.kind == SONG_MUSIC ? 2 : 1;
   }
   if (!references.empty())
      return named.size() >= 2 ? 3 * (int)named.size() : 0;
   return score;
}

// The reference whose notes clearly match an .spc (versions of a song count as its first), or -1.
int RomSession::reference_by_notes(const std::vector<uint8_t> &spc)
{
   SongNotes notes;
   std::string err;
   if (spc.empty() || reference_notes.size() != references.size() || !spc_notes(spc, notes, err))
      return -1;
   auto stem = [&](size_t i) {
      const std::string &t = references.song(i).title;
      return t.substr(0, t.find(" ("));
   };
   std::vector<std::pair<double, size_t>> ranked;
   for (size_t i = 0; i < references.size(); i++)
      ranked.push_back({ notes_distance(notes, reference_notes[i]), i });
   std::sort(ranked.begin(), ranked.end());
   // NES recordings match less closely; see name_by_reference.
   const double kMatch = nes_ ? 0.25 : 0.05, kLead = nes_ ? 0.05 : 0.03;
   if (ranked.empty() || ranked[0].first >= kMatch)
      return -1;
   for (const auto &r : ranked)
      if (stem(r.second) != stem(ranked[0].second))
      {
         if (r.first - ranked[0].first < std::max(kLead, nes_ ? ranked[0].first * 0.5 : 0.0))
            return -1;
         break;
      }
   for (size_t i = 0; i < references.size(); i++)
      if (stem(i) == stem(ranked[0].second))
         return (int)i;
   return -1;
}

// Games that upload each song to the sound CPU when their own logic changes the music
// (ActRaiser 2) start no song from a command written to RAM: the song they want is kept in a
// variable. With reference songs, play the game for a while noting which reference plays, and
// keep the RAM bytes that hold one value per song, the same each time that song plays. Each is
// tried by writing song values at moments of the game; one that starts two different reference
// songs is how songs start, and scans start from that moment.
bool RomSession::find_song_variable()
{
   if (references.empty() || reference_notes.size() != references.size())
      return false;
   set_scan_message("Playing the game to learn which RAM follows its music...");
   std::vector<uint8_t> resume = core.save_state();
   core.reset();

   const int kFrames = 5400, kListen = 120, kWindow = 90, kQuiet = 30;
   struct Moment
   {
      int frame;
      int song;
      std::vector<uint8_t> ram, state;
   };
   // A command to the sound CPU after a quiet spell, with the RAM of the frames before it.
   struct Event
   {
      int frame;
      std::vector<std::vector<uint8_t>> window;   // oldest first, the event's frame last
   };
   std::vector<Moment> moments;
   std::vector<Event> events;
   std::vector<std::vector<uint8_t>> ring(kWindow);
   size_t ram_size = 0;
   uint8_t last_ports[4] = { 0, 0, 0, 0 };
   int quiet = 0;
   scan_total_ = kFrames;
   scan_done_ = 0;
   for (int f = 0; f < kFrames && !cancel_; f++, scan_done_++)
   {
      uint16_t buttons = (f > 240 && f % 180 < 6) ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
      core.run_frame(buttons);
      core.audio().clear();
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      std::vector<uint8_t> state = core.save_state();
      if (!ram || state.empty())
         break;
      ring[f % kWindow].assign(ram, ram + ram_size);
      uint8_t ports[4];
      if (spc_snes9x_ports(state, ports))
      {
         bool changed = memcmp(ports, last_ports, 4) != 0;
         if (changed && quiet >= kQuiet && f >= kWindow && events.size() < 32)
         {
            Event e;
            e.frame = f;
            for (int k = 1; k <= kWindow; k++)
               e.window.push_back(ring[(f + k) % kWindow]);
            events.push_back(std::move(e));
         }
         quiet = changed ? 0 : quiet + 1;
         memcpy(last_ports, ports, 4);
      }
      if (f % kListen != kListen - 1)
         continue;
      Moment m;
      m.frame = f;
      m.state = std::move(state);
      m.ram.assign(ram, ram + ram_size);
      m.song = reference_by_notes(spc_of_state(m.state));
      moments.push_back(std::move(m));
   }
   std::vector<int> songs;
   std::string heard;
   for (const auto &m : moments)
      if (m.song >= 0 && std::find(songs.begin(), songs.end(), m.song) == songs.end())
      {
         songs.push_back(m.song);
         heard += (heard.empty() ? "" : ", ") + references.song(m.song).title;
      }
   log("while playing, heard: " + (heard.empty() ? std::string("no reference songs") : heard));
   if (cancel_ || songs.size() < 2)
   {
      core.load_state(resume);
      return false;
   }

   struct Candidate
   {
      uint32_t address;
      int songs;
      int changes;
      std::map<int, uint8_t> value_of;
   };
   std::vector<Candidate> candidates;

   // Request bytes: set just before the music changes (and perhaps cleared once handled), with
   // one value per song that follows.
   auto song_at = [&](int frame, bool after) {
      int found = -1;
      for (const auto &m : moments)
         if (after ? m.frame >= frame + 180 : m.frame <= frame)
         {
            found = m.song;
            if (after)
               break;
         }
      return found;
   };
   std::vector<std::pair<const Event *, int>> changes_to;   // event, the song after it
   for (const auto &e : events)
   {
      int before = song_at(e.frame, false), after = song_at(e.frame, true);
      if (after >= 0 && after != before)
         changes_to.push_back({ &e, after });
   }
   std::vector<Candidate> requests;
   if (changes_to.size() >= 2)
      for (uint32_t at = 0; at < ram_size; at++)
      {
         std::map<int, uint8_t> value_of;
         bool ok = true;
         for (const auto &c : changes_to)
         {
            const auto &w = c.first->window;
            uint8_t start = w.front()[at];
            int written = -1;
            for (size_t k = w.size(); k-- > 1;)
               if (w[k][at] != start)
               {
                  written = w[k][at];
                  break;
               }
            if (written <= 0)
            {
               ok = false;
               break;
            }
            auto it = value_of.find(c.second);
            if (it == value_of.end())
               value_of[c.second] = (uint8_t)written;
            else if (it->second != written)
            {
               ok = false;
               break;
            }
         }
         if (!ok || value_of.size() < 2)
            continue;
         std::vector<uint8_t> values;
         for (const auto &v : value_of)
            values.push_back(v.second);
         std::sort(values.begin(), values.end());
         if (std::unique(values.begin(), values.end()) == values.end())
            requests.push_back({ at, (int)value_of.size(), 0, value_of });
      }
   std::sort(requests.begin(), requests.end(), [](const Candidate &a, const Candidate &b) {
      return a.songs != b.songs ? a.songs > b.songs : a.address < b.address;
   });
   if (requests.size() > 16)
      requests.resize(16);
   log(std::to_string(changes_to.size()) + " music changes seen; RAM set before them:" + [&] {
      std::string s;
      for (const auto &c : requests)
         s += " $" + hex4(c.address);
      return s.empty() ? std::string(" none") : s;
   }());

   // Bytes holding one value per song, a different one for each song.
   for (uint32_t at = 0; at < ram_size; at++)
   {
      std::map<int, uint8_t> value_of;
      bool ok = true;
      int changes = 0;
      for (size_t i = 0; i < moments.size() && ok; i++)
      {
         if (i > 0 && moments[i].ram[at] != moments[i - 1].ram[at])
            changes++;
         if (moments[i].song < 0)
            continue;
         auto it = value_of.find(moments[i].song);
         if (it == value_of.end())
            value_of[moments[i].song] = moments[i].ram[at];
         else
            ok = it->second == moments[i].ram[at];
      }
      if (!ok || value_of.size() < 2)
         continue;
      std::vector<uint8_t> values;
      for (const auto &v : value_of)
         values.push_back(v.second);
      std::sort(values.begin(), values.end());
      if (std::unique(values.begin(), values.end()) != values.end())
         continue;
      candidates.push_back({ at, (int)value_of.size(), changes, value_of });
   }
   // Most songs followed, then the fewest changes: a song variable changes only with the music.
   std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
      if (a.songs != b.songs)
         return a.songs > b.songs;
      if (a.changes != b.changes)
         return a.changes < b.changes;
      return a.address < b.address;
   });
   if (candidates.size() > 16)
      candidates.resize(16);
   candidates.insert(candidates.begin(), requests.begin(), requests.end());
   log("RAM following the music: " + [&] {
      std::string s;
      for (const auto &c : candidates)
         s += " $" + hex4(c.address);
      return s.empty() ? std::string(" none") : s;
   }());

   // One moment per song heard to try from: the middle of its first stretch.
   std::vector<size_t> starts;
   for (int song : songs)
   {
      size_t first = moments.size(), last = 0;
      for (size_t i = 0; i < moments.size(); i++)
         if (moments[i].song == song)
         {
            first = std::min(first, i);
            last = i;
            if (i + 1 < moments.size() && moments[i + 1].song != song)
               break;
         }
      if (first < moments.size())
         starts.push_back((first + last) / 2);
   }

   std::vector<uint8_t> keep_state = scan_state_, keep_before = scan_before_spc_;
   scan_total_ = (int)(candidates.size() * starts.size());
   scan_done_ = 0;
   for (const auto &c : candidates)
      for (size_t st : starts)
      {
         if (cancel_)
            break;
         scan_done_++;
         SongStart s;
         s.kind = SongStart::RAM;
         s.address = c.address;
         s.settle_frames = kRoutineSettle;
         std::vector<uint32_t> values;
         for (const auto &v : c.value_of)
            if (v.second != moments[st].ram[c.address])
               values.push_back(v.second);
         for (uint32_t extra : { 1u, 2u, 3u })
            if (std::find(values.begin(), values.end(), extra) == values.end() && extra != moments[st].ram[c.address])
               values.push_back(extra);
         set_scan_message("Trying $" + hex4(c.address) + " from " + std::to_string(moments[st].frame / 60) + " seconds in...");
         scan_state_ = moments[st].state;
         scan_before_spc_ = spc_of_state(scan_state_);
         if (start_score(s, nullptr, &values) >= 6)
         {
            scan_start_ = s;
            scan_start_source_ = "scan (reference songs)";
            scan_changed_ = true;
            log("songs start with " + describe_song_start(s) + " from " + std::to_string(moments[st].frame / 60) +
                " seconds into the game; scans start there");
            core.load_state(scan_state_);
            return true;
         }
      }
   scan_state_ = keep_state;
   scan_before_spc_ = keep_before;
   core.load_state(resume);
   log("no RAM that follows the music starts songs when written");
   return false;
}

// Watches the game start up and finds how it starts songs: first a RAM block it copies
// to the sound CPU (a command it polls), then the music routine it calls, found from
// the return addresses on the stack whenever a command goes out.
bool RomSession::find_song_start(const SongPrint *baseline)
{
   set_scan_message("Watching how the game starts its music...");
   std::vector<uint8_t> resume = core.save_state();
   core.reset();

   struct Block
   {
      int votes = 0;
      std::map<uint32_t, int> commands;   // the 4 port bytes, and how often they were sent
   };
   std::map<uint32_t, Block> blocks;
   std::map<uint64_t, int> routines;      // bit 32 set: JSR
   size_t ram_size = 0;
   core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
   const size_t search = std::min<size_t>(ram_size, 0x2000);
   std::vector<uint8_t> prev_ram;
   uint8_t prev_ports[4] = { 0 };
   int quiet = 0;

   scan_total_ = kWatchFrames;
   scan_done_ = 0;
   for (int f = 0; f < kWatchFrames && !cancel_; f++, scan_done_++)
   {
      // Tap Start now and then to get past title screens to more music.
      uint16_t buttons = (f > 900 && f % 300 < 6) ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
      core.run_frame(buttons);
      core.audio().clear();
      std::vector<uint8_t> st = core.save_state();
      uint8_t ports[4];
      if (!spc_snes9x_ports(st, ports))
         break;
      size_t size = 0;
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      if (!ram)
         break;
      bool changed = memcmp(ports, prev_ports, 4) != 0;
      bool nonzero = ports[0] | ports[1] | ports[2] | ports[3];
      bool matched = false;
      if (changed && nonzero && prev_ram.size() >= search)
      {
         uint32_t cmd = ports[0] | ports[1] << 8 | ports[2] << 16 | (uint32_t)ports[3] << 24;
         for (uint32_t x = 0; x + 4 <= search; x++)
            if (!memcmp(&prev_ram[x], ports, 4) || !memcmp(&ram[x], ports, 4))
            {
               Block &b = blocks[x];
               b.votes++;
               b.commands[cmd]++;
               matched = true;
            }
      }
      // A command from a block, or the first port change after a quiet spell: the
      // routine that sent it left its return address on the stack.
      if (changed && nonzero && (matched || quiet >= 30))
      {
         size_t reg = snapshot_block(st, "REG");
         uint16_t sp = reg ? (uint16_t)(st[reg + 8] << 8 | st[reg + 9]) : 0;
         uint32_t lo = sp > 0x80 ? sp - 0x80 : 0, hi = std::min<uint32_t>(sp + 0x40, (uint32_t)search - 3);
         for (uint32_t a = lo; reg && a < hi; a++)
         {
            // JSL pushes the address of its last byte: opcode 22 sits 3 bytes before.
            uint32_t pushed = ram[a + 2] << 16 | ram[a + 1] << 8 | ram[a];
            const uint8_t *op = rom_.at(pushed - 3, 4);
            if (op && op[0] == 0x22)
            {
               uint32_t target = op[3] << 16 | op[2] << 8 | op[1];
               if (rom_.at(target))
                  routines[target]++;
            }
            // JSR pushes the 16-bit address of its last byte, within the caller's bank.
            uint16_t pushed16 = ram[a + 1] << 8 | ram[a];
            for (uint8_t bank : { st[reg], (uint8_t)0x00, (uint8_t)0x80, (uint8_t)0xC0 })
            {
               const uint8_t *jsr = rom_.at(((uint32_t)bank << 16 | pushed16) - 2, 3);
               if (jsr && jsr[0] == 0x20)
               {
                  uint32_t target = (uint32_t)bank << 16 | jsr[2] << 8 | jsr[1];
                  if (rom_.at(target))
                     routines[(1ull << 32) | target]++;
               }
            }
         }
      }
      quiet = changed ? 0 : quiet + 1;
      memcpy(prev_ports, ports, 4);
      prev_ram.assign(ram, ram + search);
   }
   core.load_state(resume);
   if (cancel_)
      return false;

   // Command blocks carry many different commands; bytes that merely hold the same
   // value as a port for a moment do not.
   std::vector<std::pair<int, uint32_t>> ranked_blocks;
   for (auto &b : blocks)
      ranked_blocks.push_back({ (int)b.second.commands.size() * 100 + b.second.votes, b.first });
   std::sort(ranked_blocks.rbegin(), ranked_blocks.rend());
   if (ranked_blocks.size() > 4)
      ranked_blocks.resize(4);

   auto commands_of = [&](uint32_t block, size_t max) {
      std::vector<std::pair<int, uint32_t>> cmds;
      for (auto &c : blocks[block].commands)
         cmds.push_back({ c.second, c.first });
      std::sort(cmds.rbegin(), cmds.rend());
      if (cmds.size() > max)
         cmds.resize(max);
      return cmds;
   };
   auto with_bytes = [](SongStart s, uint32_t cmd, int offset) {
      s.bytes.clear();
      for (int b = 0; b < 4; b++)
         s.bytes.push_back((uint8_t)(cmd >> (8 * b)));
      s.offset = offset;
      return s;
   };

   SongStart best;
   int best_score = 0;
   auto try_all = [&](const std::vector<SongStart> &trials) {
      scan_total_ = (int)trials.size();
      scan_done_ = 0;
      for (const auto &s : trials)
      {
         if (cancel_ || best_score >= 8)
            break;
         set_scan_message("Trying " + describe_song_start(s) + "...");
         int score = start_score(s, baseline);
         if (score > best_score)
         {
            best_score = score;
            best = s;
         }
         scan_done_++;
      }
   };

   // 1. A RAM command the game polls.
   std::vector<SongStart> trials;
   for (auto &r : ranked_blocks)
      for (auto &c : commands_of(r.second, 3))
         for (int offset = 0; offset < 4; offset++)
         {
            SongStart s;
            s.kind = SongStart::RAM;
            s.address = r.second;
            s.settle_frames = 150;
            trials.push_back(with_bytes(s, c.second, offset));
         }
   try_all(trials);

   // 2. The music routine, with its parameters in a command block or in A.
   if (best_score < 3 && !routines.empty())
   {
      // JSL return addresses are three bytes that must decode to a long call, so they
      // are rarely coincidence; two-byte JSR ones often are. Try JSL targets first.
      std::vector<std::pair<int, uint64_t>> jsl, jsr;
      for (auto &r : routines)
         ((r.first >> 32) ? jsr : jsl).push_back({ r.second, r.first });
      std::sort(jsl.rbegin(), jsl.rend());
      std::sort(jsr.rbegin(), jsr.rend());
      jsl.resize(std::min<size_t>(jsl.size(), 4));
      jsr.resize(std::min<size_t>(jsr.size(), 3));
      std::vector<std::pair<int, uint64_t>> ranked = jsl;
      ranked.insert(ranked.end(), jsr.begin(), jsr.end());
      log("music routine candidates:" + [&] {
         std::string s;
         for (auto &r : ranked)
            s += std::string(" ") + ((r.second >> 32) ? "JSR $" : "JSL $") + hex4((uint32_t)(r.second >> 16) & 0xFF).substr(2) + hex4((uint32_t)r.second & 0xFFFF);
         return s;
      }());
      trials.clear();
      for (auto &r : ranked)
      {
         SongStart base;
         base.kind = SongStart::ROUTINE;
         base.address = (uint32_t)r.second & 0xFFFFFF;
         base.jsr = (r.second >> 32) != 0;
         base.settle_frames = kRoutineSettle;
         for (size_t b = 0; b < ranked_blocks.size() && b < 2; b++)
            for (auto &c : commands_of(ranked_blocks[b].second, 2))
               for (int offset = 0; offset < 4; offset++)
               {
                  SongStart s = with_bytes(base, c.second, offset);
                  s.fill_block = true;
                  s.block = ranked_blocks[b].second;
                  trials.push_back(s);
               }
         SongStart in_a = base;
         in_a.song_in_a = true;
         trials.push_back(in_a);
      }
      try_all(trials);
   }

   if (best_score < 3)
   {
      log("found no way to start songs from outside the game");
      return false;
   }
   scan_start_ = best;
   scan_start_source_ = "scan";
   scan_changed_ = true;
   log("songs start with " + describe_song_start(best));
   return true;
}

// Loudness and brightness per 100 ms over the next `seconds` (NES start search; core_mutex held).
static void short_print(CoreHost &core, double seconds, std::vector<float> &out)
{
   std::vector<int16_t> mono;
   double phase = 0;
   core.audio().clear();
   int frames = (int)(seconds * 60);
   for (int f = 0; f < frames; f++)
   {
      core.run_frame(0);
      append_mono(core.audio(), core.sample_rate(), phase, mono);
      core.audio().clear();
   }
   const size_t window = kRipRate / 10;
   out.clear();
   int prev = 0;
   for (size_t w = 0; w + window <= mono.size(); w += window)
   {
      double sum = 0, hp = 0;
      for (size_t i = w; i < w + window; i++)
      {
         sum += (double)mono[i] * mono[i];
         hp += (double)(mono[i] - prev) * (mono[i] - prev);
         prev = mono[i];
      }
      out.push_back((float)std::sqrt(sum / window));
      out.push_back((float)std::sqrt(hp / window));
   }
}

static double short_distance(const std::vector<float> &a, const std::vector<float> &b)
{
   double diff = 0, total = 0;
   for (size_t i = 0; i < a.size() && i < b.size(); i++)
   {
      diff += std::fabs(a[i] - b[i]);
      total += a[i] + b[i];
   }
   return total < 1000 ? 0.0 : diff / total;
}

// NES games have no sound CPU to watch: their music code runs on the main CPU, and most ask
// it for a song through a RAM byte it polls (Super Mario Bros.: $FB, set for a frame and cleared
// once handled) or keep the song wanted in a variable. While the game starts up, note the RAM
// bytes that take a few values without ticking like counters, first those that changed just
// before sound started after a quiet spell, then those set only briefly. Each is written with
// values it took, and with small ones, from the scan start and then from a moment music was
// playing (many games play none on their title screen); bytes after which the game sounds
// different, differently for two values, are tried like any other start, keeping the one that
// starts the most songs.
bool RomSession::find_song_start_nes(const SongPrint *baseline)
{
   set_scan_message("Watching the game's RAM while it starts...");
   std::vector<uint8_t> resume = core.save_state();
   core.reset();
   size_t ram_size = 0;
   core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
   if (!ram_size || ram_size > 0x2000)
   {
      core.load_state(resume);
      return false;
   }

   struct Byte
   {
      int changes = 0, pulses = 0, quick = 0, onsets = 0, since_set = -1, last_change = -1000;
      std::vector<uint8_t> values;   // nonzero values, in the order first taken
   };
   std::vector<Byte> bytes(ram_size);
   std::vector<uint8_t> prev;
   std::vector<std::pair<int, std::vector<uint8_t>>> moments;        // frame, state: music playing
   std::vector<std::pair<double, size_t>> energy(180, { 0.0, 0 });   // per frame: sum of squares, samples
   double energy_sum = 0;
   size_t energy_samples = 0;
   int quiet_frames = 0, onsets = 0;
   scan_total_ = kWatchFrames;
   scan_done_ = 0;
   for (int f = 0; f < kWatchFrames && !cancel_; f++, scan_done_++)
   {
      // Tap Start to get past title screens until music plays; in most games it then pauses.
      uint16_t buttons = (moments.empty() && f > 900 && f % 300 < 6) ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
      core.run_frame(buttons);
      auto &slot = energy[f % energy.size()];
      energy_sum -= slot.first;
      energy_samples -= slot.second;
      slot = { 0.0, core.audio().size() };
      for (int16_t sample : core.audio())
         slot.first += (double)sample * sample;
      energy_sum += slot.first;
      energy_samples += slot.second;
      core.audio().clear();
      double frame_rms = slot.second ? std::sqrt(slot.first / slot.second) : 0;
      double rms = energy_samples ? std::sqrt(energy_sum / energy_samples) : 0;
      // Music over the last 3 seconds, well after a button press.
      if (f >= 180 && rms >= 300 && f % 300 >= 240 && (moments.empty() || f - moments.back().first >= 600))
         moments.push_back({ f, core.save_state() });

      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      if (!ram || ram_size != bytes.size())
         break;
      if (!prev.empty())
         for (size_t at = 0; at < ram_size; at++)
         {
            Byte &b = bytes[at];
            if (ram[at] == prev[at])
            {
               if (b.since_set >= 0)
                  b.since_set++;
               continue;
            }
            b.changes++;
            b.last_change = f;
            if (ram[at] && b.values.size() < 32 && std::find(b.values.begin(), b.values.end(), ram[at]) == b.values.end())
               b.values.push_back(ram[at]);
            if (!ram[at] && b.since_set >= 0 && b.since_set <= 8)
               b.pulses++;
            // Cleared the very next frame: a request the music code took at once.
            if (!ram[at] && b.since_set == 0)
               b.quick++;
            b.since_set = ram[at] ? 0 : -1;
         }
      prev.assign(ram, ram + ram_size);

      // Sound after a quiet spell: the bytes that changed just before may have asked for it.
      if (frame_rms >= 300 && quiet_frames >= 20)
      {
         onsets++;
         for (auto &b : bytes)
            if (b.last_change >= f - 15)
               b.onsets++;
      }
      quiet_frames = frame_rms < 100 ? quiet_frames + 1 : 0;
   }
   core.load_state(resume);
   if (cancel_)
      return false;

   // Few values, few changes, and not the stack page.
   std::vector<uint32_t> candidates;
   for (uint32_t at = 0; at < bytes.size(); at++)
   {
      const Byte &b = bytes[at];
      if ((at >= 0x100 && at < 0x200) || b.values.empty() || b.values.size() > 24 || b.changes > kWatchFrames / 20)
         continue;
      candidates.push_back(at);
   }
   std::stable_sort(candidates.begin(), candidates.end(), [&](uint32_t x, uint32_t y) {
      const Byte &a = bytes[x], &b = bytes[y];
      bool ra = a.quick && a.onsets, rb = b.quick && b.onsets;
      if (ra != rb)
         return ra;
      if (a.quick != b.quick)
         return a.quick > b.quick;
      if (a.onsets != b.onsets)
         return a.onsets > b.onsets;
      if (a.pulses != b.pulses)
         return a.pulses > b.pulses;
      return a.values.size() > b.values.size();
   });
   log(std::to_string(candidates.size()) + " RAM bytes to try (" + std::to_string(onsets) + " times sound started, music playing at " +
       std::to_string(moments.size()) + " moments)");

   // The reference .nsf may start its songs by writing the game's own request bytes (The Legend of
   // Zelda: $0600): those come first, with the values the .nsf writes.
   std::map<uint32_t, std::vector<uint32_t>> nsf_values;
   for (const auto &entry : nsf_table)
   {
      if (entry.first >= ram_size || nsf_values.size() >= 8)
         continue;
      // In the order of the songs they start.
      std::vector<std::pair<int, uint32_t>> by_song;
      for (const auto &v : entry.second)
         by_song.push_back({ v.second, v.first });
      std::sort(by_song.begin(), by_song.end());
      for (const auto &b : by_song)
         nsf_values[entry.first].push_back(b.second);
   }
   if (!nsf_values.empty())
      log("the reference .nsf starts songs by writing" + [&] {
         std::string s;
         for (const auto &n : nsf_values)
            s += " $" + hex4(n.first);
         return s;
      }());

   const double kDiffer = 0.25;
   struct Trial
   {
      uint32_t address;
      std::vector<uint32_t> values;   // those that changed the music, each differently
   };
   std::vector<Trial> heard;
   // Writes each candidate at the current scan start and keeps those that change the music: a
   // quick listen to every one, then a longer one, with more values, to the most likely.
   auto listen = [&](const std::string &where) {
      const int kQuickLead = 30, kLead = 60;
      const double kQuick = 1.0, kListen = 3.0;
      core.load_state(scan_state_);
      run_frames(kQuickLead);
      std::vector<float> untouched;
      short_print(core, kQuick, untouched);
      std::vector<std::pair<int, uint32_t>> likely;   // values that changed the music, byte
      for (const auto &n : nsf_values)
         likely.push_back({ 1000, n.first });
      scan_total_ = (int)candidates.size();
      scan_done_ = 0;
      for (uint32_t at : candidates)
      {
         if (cancel_)
            break;
         if (nsf_values.count(at))
            continue;
         if (scan_done_++ % 16 == 0)
            set_scan_message("Listening for music changes from RAM $" + hex4(at) + where + "...");
         const std::vector<uint8_t> &seen = bytes[at].values;
         uint32_t first = seen[0], second = seen.size() > 1 ? seen[1] : first == 1 ? 2 : 1;
         std::vector<std::vector<float>> prints;
         for (uint32_t v : { first, second })
         {
            SongStart s;
            s.kind = SongStart::RAM;
            s.address = at;
            if (!start_song(s, v))
               break;
            run_frames(kQuickLead);
            std::vector<float> p;
            short_print(core, kQuick, p);
            bool same = short_distance(p, untouched) < kDiffer;
            for (const auto &q : prints)
               same = same || short_distance(p, q) < kDiffer;
            if (!same)
               prints.push_back(p);
         }
         if (!prints.empty())
            likely.push_back({ (int)prints.size(), at });
      }
      std::stable_sort(likely.begin(), likely.end(), [](const std::pair<int, uint32_t> &x, const std::pair<int, uint32_t> &y) {
         return x.first > y.first;
      });
      if (likely.size() > 24)
         likely.resize(24);

      core.load_state(scan_state_);
      run_frames(kLead);
      short_print(core, kListen, untouched);
      scan_total_ = (int)likely.size();
      scan_done_ = 0;
      for (const auto &l : likely)
      {
         if (cancel_)
            break;
         uint32_t at = l.second;
         scan_done_++;
         set_scan_message("Listening closely to RAM $" + hex4(at) + where + "...");
         std::vector<uint32_t> values;
         auto from_nsf = nsf_values.find(at);
         if (from_nsf != nsf_values.end())
            values.assign(from_nsf->second.begin(), from_nsf->second.begin() + std::min<size_t>(4, from_nsf->second.size()));
         for (uint8_t v : bytes[at].values)
            if (values.size() < 3)
               values.push_back(v);
         for (uint32_t v : { 1u, 2u, 4u })
            if (values.size() < 4 && std::find(values.begin(), values.end(), v) == values.end())
               values.push_back(v);
         Trial t;
         t.address = at;
         std::vector<std::vector<float>> prints;
         for (uint32_t v : values)
         {
            SongStart s;
            s.kind = SongStart::RAM;
            s.address = at;
            if (!start_song(s, v))
               break;
            run_frames(kLead);
            std::vector<float> p;
            short_print(core, kListen, p);
            bool same = short_distance(p, untouched) < kDiffer;
            for (const auto &q : prints)
               same = same || short_distance(p, q) < kDiffer;
            if (same)
               continue;
            prints.push_back(p);
            t.values.push_back(v);
         }
         if (t.values.size() >= 2)
            heard.push_back(t);
      }
   };
   // First from the first moment music played, then from the scan start.
   std::vector<uint8_t> keep_state = scan_state_;
   if (!moments.empty())
   {
      scan_state_ = moments.front().second;
      listen(" at " + std::to_string(moments.front().first / 60) + " seconds in");
   }
   if (heard.empty() && !cancel_)
   {
      scan_state_ = keep_state;
      listen("");
   }
   std::stable_sort(heard.begin(), heard.end(), [](const Trial &x, const Trial &y) { return x.values.size() > y.values.size(); });
   if (heard.size() > 6)
      heard.resize(6);
   log("RAM that changes the music when written:" + [&] {
      std::string s;
      for (const auto &t : heard)
         s += " $" + hex4(t.address);
      return s.empty() ? std::string(" none") : s;
   }());

   SongStart best;
   int best_score = 0;
   scan_total_ = (int)heard.size();
   scan_done_ = 0;
   for (const auto &t : heard)
   {
      if (cancel_ || best_score >= 8)
         break;
      SongStart s;
      s.kind = SongStart::RAM;
      s.address = t.address;
      std::vector<uint32_t> values = t.values;
      auto from_nsf = nsf_values.find(t.address);
      if (from_nsf != nsf_values.end())
         for (uint32_t v : from_nsf->second)
            if (values.size() < 6 && std::find(values.begin(), values.end(), v) == values.end())
               values.push_back(v);
      for (uint32_t v : kTrials)
         if (values.size() < 6 && std::find(values.begin(), values.end(), v) == values.end())
            values.push_back(v);
      set_scan_message("Trying " + describe_song_start(s) + "...");
      int score = start_score(s, baseline, &values);
      if (score > best_score)
      {
         best_score = score;
         best = s;
      }
      scan_done_++;
   }
   if (best_score < 3)
   {
      scan_state_ = keep_state;
      core.load_state(scan_state_);
      log("found no RAM that starts songs when written");
      return false;
   }
   scan_start_ = best;
   scan_start_source_ = "scan";
   scan_changed_ = true;
   core.load_state(scan_state_);
   if (scan_state_ != keep_state)
   {
      scan_before_spc_ = spc_of_state(scan_state_);
      log("songs start with " + describe_song_start(best) + " once the game is under way; scans start from there");
   }
   else
      log("songs start with " + describe_song_start(best));
   return true;
}

// NES music code that keeps no song number (Mega Man 3): the reference .nsf's init calls the game's
// sound routine with the song in A, and a tap on that routine (see nes_tap.h) makes the game write
// each request to a byte of RAM it leaves alone. Once the tap reports one of the .nsf's songs in the
// game, its byte is the song address, every song of the set is listed under the value the game
// requests it with, and a code patch that silences the music but not the sound effects is looked for
// in the .nsf. The core is left anywhere; the caller restores it.
bool RomSession::scan_with_tap()
{
   if (core.library_name().find("FCEUmm") == std::string::npos)
   {
      log("taps on the sound routine are cheat codes for FCEUmm; " + core.library_name() + " is running");
      return false;
   }
   const ReferenceSong *nsf_ref = nullptr;
   for (size_t r = 0; r < references.size() && !nsf_ref; r++)
   {
      const auto &d = references.song(r).data;
      if (d.size() >= 5 && (!memcmp(d.data(), "NESM\x1A", 5) || !memcmp(d.data(), "NSFE", 4)))
         nsf_ref = &references.song(r);
   }
   if (!nsf_ref)
      return false;
   const std::vector<uint8_t> nsf = nsf_ref->data;
   const std::vector<uint8_t> &rom = core.content_data();
   std::string err;
   std::vector<NsfCall> calls = nsf_init_calls(nsf, err);
   if (calls.empty())
      return false;
   // A routine where a sound effect is asked for with a listed song's value would play the song's
   // replacement for it: routines with fewer such values go first.
   std::set<int> ref_tracks;
   for (size_t i = 0; i < references.size(); i++)
      if (references.song(i).path == nsf_ref->path)
         ref_tracks.insert(references.song(i).track);
   std::vector<int> effect_songs;
   for (int s : nsf_effect_songs(nsf, err))
      if (!ref_tracks.count(s))
         effect_songs.push_back(s);
   auto collisions = [&](const NsfCall &call) {
      std::set<uint8_t> song_values;
      int n = 0;
      for (int t : ref_tracks)
         if (call.song_values.count(t))
            song_values.insert(call.song_values.at(t));
      for (int s : effect_songs)
         n += call.song_values.count(s) && song_values.count(call.song_values.at(s));
      return n;
   };
   std::stable_sort(calls.begin(), calls.end(), [&](const NsfCall &x, const NsfCall &y) { return collisions(x) < collisions(y); });

   // RAM for the tap: bytes no instruction names that stay unchanged while the game runs.
   set_scan_message("Looking for RAM the game leaves alone...");
   std::vector<uint16_t> unnamed = nes_unnamed_ram(rom);
   size_t ram_size = 0;
   std::vector<uint8_t> settled;
   std::vector<bool> changed(0x800, false);
   core.cheat_reset();
   core.reset();
   scan_total_ = kWatchFrames;
   scan_done_ = 0;
   for (int f = 0; f < kWatchFrames && !cancel_; f++, scan_done_++)
   {
      core.run_frame(f > 900 && f % 300 < 6 ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0);
      core.audio().clear();
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      if (!ram || ram_size < 0x800)
         return false;
      if (f == 120)
         settled.assign(ram, ram + 0x800);
      else if (f > 120)
         for (size_t a = 0; a < 0x800; a++)
            changed[a] = changed[a] || ram[a] != settled[a];
   }
   uint16_t tap_ram = 0;
   for (uint16_t a : unnamed)
      if (!changed[a])
      {
         tap_ram = a;
         break;
      }
   if (!tap_ram || cancel_)
   {
      log("found no RAM the game leaves alone for a tap on its sound routine");
      return false;
   }

   for (size_t c = 0; c < calls.size() && c < 3 && !cancel_; c++)
   {
      const NsfCall &call = calls[c];
      NesTap tap;
      // A jump table entry moves nothing: the tap goes where it jumps.
      uint16_t body = nsf_jump_target(nsf, call.routine, err);
      if (!nes_tap_design(rom, body, nsf_code_at(nsf, body, 16, err), tap_ram, tap, err))
      {
         log("no tap on $" + hex4(body) + ": " + err);
         continue;
      }
      // The set's songs, by the value the game requests each with (the tap writes request + 1).
      std::map<uint8_t, int> by_value;
      for (size_t i = 0; i < references.size(); i++)
         if (references.song(i).path == nsf_ref->path)
         {
            auto v = call.song_values.find(references.song(i).track);
            if (v != call.song_values.end())
               by_value.emplace((uint8_t)(v->second + 1), (int)i);
         }
      if (by_value.size() < 2)
         continue;

      set_scan_message("Checking a tap on the sound routine $" + hex4(call.routine) + "...");
      const std::string cheat = tap.fceumm_cheat();
      core.cheat_set(0x7FFF, true, cheat);
      core.reset();
      int heard = -1;
      uint8_t heard_value = 0;
      // The game as the tap reports a song: where Proteus applies a patch when a replacement starts.
      // The second song reported is better, since the music code then also has the first one's
      // notes to let go of.
      std::vector<uint8_t> playing;
      int changes = 0;
      scan_done_ = 0;
      for (int f = 0, last = -1; f < kWatchFrames && changes < 2 && !cancel_; f++, scan_done_++)
      {
         core.run_frame(f > 900 && f % 300 < 6 ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0);
         core.audio().clear();
         const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
         if (ram && tap.ram < ram_size && by_value.count(ram[tap.ram]) && ram[tap.ram] != last)
         {
            last = ram[tap.ram];
            if (heard < 0)
            {
               heard = f;
               heard_value = ram[tap.ram];
            }
            playing = core.save_state();
            changes++;
         }
      }
      core.cheat_reset();
      if (heard < 0)
      {
         log("a tap on $" + hex4(call.routine) + " reported none of the reference songs");
         continue;
      }
      log("a tap on the sound routine $" + hex4(call.routine) + " (stub at $" + hex4(tap.stub) + ", request + 1 at $" +
          hex4(tap.ram) + ") reported \"" + references.song(by_value[heard_value]).title + "\" after " +
          std::to_string(heard / 60) + " s");

      SongAddress a;
      a.known = true;
      a.address = tap.ram;
      a.size = 1;
      a.latch = true;
      a.debounce = 1;
      scan_address_ = a;
      scan_address_source_ = "scan (a tap on the sound routine $" + hex4(call.routine) + ")";
      scan_silence_ = SongSilence();
      scan_tap_.clear();
      scan_tap_["fceumm"] = cheat;
      scan_patch_.clear();
      scan_changed_ = true;

      std::vector<int> music;
      std::set<int> listed_tracks;
      int listed = 0;
      for (const auto &entry : by_value)
      {
         const ReferenceSong &ref = references.song(entry.second);
         listed_tracks.insert(ref.track);
         std::lock_guard<std::mutex> lock(songs_mutex);
         FoundSong song;
         song.value = entry.first;
         if (!use_reference(song, ref, err))
            continue;
         if (song.kind == SONG_MUSIC)
            music.push_back(ref.track);
         FoundSong *existing = find_song(entry.first);
         if (existing && existing->reference == song.reference)
            continue;
         add_song_locked(song);
         listed++;
         scan_found_++;
      }
      log("listed " + std::to_string(listed) + " songs by the value the game requests them with");

      // Stopping the music code while a replacement plays keeps the sound effects. A patch that
      // silences the .nsf may still leave a note hanging in the game, so each is heard there: from
      // the song the tap reported, the game must go quiet with it.
      set_scan_message("Looking for a way to stop the music but not the sound effects...");
      auto loudness = [&](const std::string &codes) {
         core.load_state(playing);
         core.cheat_reset();
         core.cheat_set(0x7FFF, true, codes);
         double sum = 0;
         size_t n = 0;
         for (int f = 0; f < 240; f++)
         {
            core.run_frame(0);
            if (f >= 60)
               for (int16_t v : core.audio())
               {
                  sum += (double)v * v;
                  n++;
               }
            core.audio().clear();
         }
         core.cheat_reset();
         return n ? std::sqrt(sum / n) : 0.0;
      };
      std::vector<NsfMusicPatch> candidates;
      if (!music.empty() && !playing.empty())
         candidates = nsf_music_patch_candidates(nsf, music, effect_songs, err);
      double plain = candidates.empty() ? 0.0 : loudness(cheat);
      int heard_patches = 0;
      for (const NsfMusicPatch &mp : candidates)
      {
         if (mp.music_silenced < mp.music_tested || heard_patches >= 6 || cancel_ || plain < 200.0)
            break;
         std::vector<uint8_t> around = nsf_code_at(nsf, mp.patch.address, 12, err);
         if (around.size() < mp.patch.original.size() ||
               !std::equal(mp.patch.original.begin(), mp.patch.original.end(), around.begin()) ||
               !nes_rom_holds(rom, mp.patch.address, around))
            continue;
         heard_patches++;
         std::string code = fceumm_cheat(mp.patch.address, mp.patch.original, mp.patch.bytes);
         double patched = loudness(cheat + "+" + code);
         if (patched > plain * 0.1)
         {
            log("a code patch at $" + hex4(mp.patch.address) + " silences the .nsf but leaves " +
                std::to_string((int)(patched * 100 / plain)) + "% of the sound in the game");
            continue;
         }
         scan_patch_["fceumm"] = code;
         log("a code patch at $" + hex4(mp.patch.address) + " silences the music (" + std::to_string(mp.music_silenced) +
             " songs in the .nsf, " + std::to_string((int)(patched * 100 / plain)) + "% of the sound left in the game) and keeps " +
             (mp.effects_percent < 0 ? std::string("sound effects the .nsf lacks") :
                                       std::to_string(mp.effects_percent) + "% of the sound effects in the .nsf"));
         break;
      }
      if (scan_patch_.empty())
         log(plain < 200.0 && !candidates.empty() ? "the song the tap reported was too quiet to check a code patch; profiles mute the sound channels instead" :
             "found no code patch that silences the music in the game; profiles mute the sound channels instead");
      return true;
   }
   return false;
}

// Stopping the game's music without muting its channels: most NES music code takes a request that
// silences it until the next song (Super Mario Bros.: $FB = 80). From the scan start, with music
// playing and no one pressing buttons, each value of the song request is written; one after which
// the game goes quiet, and stays quiet, is kept. Values that start songs are skipped.
bool RomSession::find_silence(const SongStart &s)
{
   const double kQuiet = 0.05;   // of the music's loudness
   auto rms_ahead = [&](double seconds) {
      double sum = 0;
      size_t n = 0;
      for (int f = 0; f < (int)(seconds * 60); f++)
      {
         core.run_frame(0);
         for (int16_t v : core.audio())
            sum += (double)v * v;
         n += core.audio().size();
         core.audio().clear();
      }
      return n ? std::sqrt(sum / n) : 0.0;
   };
   core.load_state(scan_state_);
   run_frames(30);
   double music = rms_ahead(2.0);
   if (music < 300)
   {
      log("no music plays at the scan start, so no way to stop it could be checked");
      return false;
   }
   std::vector<uint32_t> order = { 0x80, 0xFF, 0x7F, 0xFE };
   for (uint32_t v = 1; v < 0x100; v++)
      if (std::find(order.begin(), order.end(), v) == order.end())
         order.push_back(v);
   std::vector<uint32_t> songs_listed;
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      for (const auto &song : songs)
         if (song.has_value)
            songs_listed.push_back(song.value);
   }
   auto nsf = nsf_table.find(s.address);
   set_scan_message("Looking for how to stop the game's music...");
   for (uint32_t v : order)
   {
      if (cancel_)
         break;
      if (std::find(songs_listed.begin(), songs_listed.end(), v) != songs_listed.end() ||
            (nsf != nsf_table.end() && nsf->second.count((uint8_t)v)))
         continue;
      if (!start_song(s, v))
         return false;
      run_frames(30);
      if (rms_ahead(1.0) >= music * kQuiet)
         continue;
      if (rms_ahead(3.0) >= music * kQuiet)
         continue;
      scan_silence_.known = true;
      scan_silence_.address = s.address;
      scan_silence_.value = (uint8_t)v;
      scan_changed_ = true;
      log("writing " + hex2(v) + " to $" + hex4(s.address) + " stops the game's music; profiles stop it this way "
          "instead of muting its sound channels");
      core.load_state(scan_state_);
      return true;
   }
   core.load_state(scan_state_);
   log("found no value of $" + hex4(s.address) + " that stops the game's music");
   return false;
}

// A profile that stops the game's music makes the RAM that holds the song playing useless: it
// reads the silence from then on, so the game's next song (Super Mario Bros.: the death jingle)
// would go unseen. The requests show every song the game asks for, so Proteus follows those: the
// song request, and the register the .nsf starts its other songs with (jingles, at 0x100 + value),
// when writing it makes the game take and clear it.
void RomSession::follow_requests(const SongStart &s)
{
   SongAddress a;
   a.known = true;
   a.address = s.address;
   a.latch = true;
   a.debounce = 1;

   size_t ram_size = 0;
   core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
   uint32_t best = 0;
   size_t best_songs = 0;
   for (const auto &entry : nsf_table)
   {
      if (entry.first == s.address || entry.first >= ram_size || entry.second.size() <= best_songs)
         continue;
      // A request is taken and cleared within a few frames; RAM holding the song playing keeps it.
      core.load_state(scan_state_);
      uint8_t *ram = core.memory_mut(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      if (!ram)
         break;
      ram[entry.first] = entry.second.begin()->first;
      run_frames(4);
      ram = core.memory_mut(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      if (ram && ram[entry.first] == 0)
      {
         best = entry.first;
         best_songs = entry.second.size();
      }
   }
   core.load_state(scan_state_);
   if (best_songs)
   {
      a.events = true;
      a.events_address = best;
      std::string err;
      int listed = 0;
      for (const auto &v : nsf_table[best])
      {
         std::lock_guard<std::mutex> lock(songs_mutex);
         uint32_t value = 0x100u | v.first;
         FoundSong *existing = find_song(value);
         if (existing && !existing->reference.empty())
            continue;
         FoundSong song;
         song.value = value;
         if (use_reference(song, references.song(v.second), err))
         {
            add_song_locked(song);
            listed++;
         }
      }
      log("jingles are requested at $" + hex4(best) + ": listed " + std::to_string(listed) + " of them, as songs from 0x100");
   }
   scan_address_ = a;
   scan_address_source_ = "scan (the requests, since the music is stopped through RAM)";
   scan_changed_ = true;
   log("profiles will follow " + describe_song_address(a) + ", where the game asks for each song");
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void RomSession::start_scan(int first, int last)
{
   // A scan uses the references it starts with; wait for ones still loading.
   if (scanning_ || !open_ || loading_refs_)
      return;
   if (worker_.joinable())
      worker_.join();
   cancel_ = false;
   scanning_ = true;
   scan_done_ = 0;
   scan_found_ = 0;
   scan_address_ = address;
   scan_start_ = start;
   scan_silence_ = silence;
   scan_tap_ = tap;
   scan_patch_ = patch;
   scan_address_source_ = address_source;
   scan_start_source_ = start_source;
   scan_changed_ = false;

   address_hints_.clear();
   if (address.known)
      address_hints_.push_back({ address, address_source.empty() ? "the song address" : address_source });
   {
      std::lock_guard<std::mutex> lock(ra_mutex);
      for (const auto &note : ra_result.notes)
      {
         SongAddress a;
         a.known = true;
         a.memory = note.memory;
         a.address = note.address;
         a.size = note.size;
         address_hints_.push_back({ a, "RetroAchievements note \"" + note.note + "\"" });
      }
   }
   for (const auto &c : apu_analysis.candidates)
      address_hints_.push_back({ c.address, "static analysis" });

   worker_ = std::thread(&RomSession::scan_thread, this, first, last);
}

void RomSession::stop_scan()
{
   cancel_ = true;
}

float RomSession::scan_progress() const
{
   return (float)scan_done_ / (float)std::max(1, (int)scan_total_);
}

std::string RomSession::scan_message()
{
   std::lock_guard<std::mutex> lock(message_mutex_);
   return scan_message_;
}

void RomSession::set_scan_message(const std::string &message)
{
   std::lock_guard<std::mutex> lock(message_mutex_);
   scan_message_ = message;
}

void RomSession::apply_scan_results()
{
   if (scanning_ || !scan_changed_)
      return;
   scan_changed_ = false;
   address = scan_address_;
   start = scan_start_;
   silence = scan_silence_;
   tap = scan_tap_;
   patch = scan_patch_;
   address_source = scan_address_source_;
   start_source = scan_start_source_;
   save_to_game_db("confirmed by a scan");
   log("saved to the game database (" + GameDb::get().path() + ")");
}

void RomSession::use_current_moment_for_scans()
{
   scan_state_ = core.save_state();
   log(scan_state_.empty() ? "this core cannot save states" : "scans now start from this moment");
}

void RomSession::scan_thread(int first, int last)
{
   std::unique_lock<std::mutex> core_lock(core_mutex);
   core.set_skip_video(true);

   if (scan_state_.empty())
   {
      set_scan_message("Starting the game...");
      core.reset();
      run_frames(kBootFrames);
      scan_state_ = core.save_state();
   }
   if (scan_state_.empty())
   {
      set_scan_message("This core cannot save states, so it cannot be scanned.");
      core.set_skip_video(false);
      scanning_ = false;
      return;
   }

   scan_before_spc_ = spc_of_state(scan_state_);
   if (!keep_rips_dir.empty())
   {
      make_dirs(keep_rips_dir);
      write_text(keep_rips_dir + "\\before." + music_extension(scan_before_spc_),
            std::string(scan_before_spc_.begin(), scan_before_spc_.end()));
   }
   if (song_table.found)
   {
      set_scan_message("Listing the songs of the ROM song table...");
      add_songs_from_table();
   }

   // What plays without starting anything: values the game ignores sound like this.
   // The same run shows which RAM the game leaves alone, for the routine call code.
   std::string err;
   FoundSong baseline;
   core.load_state(scan_state_);
   size_t ram_size = 0;
   const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
   std::vector<uint8_t> ram_before(ram, ram + (ram ? ram_size : 0));
   run_frames(kRoutineSettle);
   choose_stub_area(ram_before);
   bool have_baseline = rip_state(core.save_state(), 0, false, baseline, err);
   const SongPrint *base_print = have_baseline ? &baseline.print : nullptr;

   // Check the known way to start songs works; otherwise look for the game's own.
   bool usable = false;
   if (scan_start_.kind != SongStart::NONE)
   {
      set_scan_message("Checking " + describe_song_start(scan_start_) + "...");
      usable = start_score(scan_start_, base_print) >= 3;
      if (!usable)
         log(describe_song_start(scan_start_) + " starts no songs");
   }
   // An .nsf that starts songs through RAM names the bytes to try: faster than playing the game.
   if (!usable && !cancel_ && nes_ && !nsf_table.empty())
      usable = find_song_start_nes(base_print);
   // Music code keeping no song number: the game's requests, through a tap on its sound routine.
   if (!usable && !cancel_ && nes_ && !references.empty() && scan_with_tap())
   {
      core.load_state(scan_state_);
      core.audio().clear();
      core.set_skip_video(false);
      core_lock.unlock();
      save_library();
      set_scan_message((cancel_ ? "Scan stopped: " : "Scan finished: ") + std::to_string((int)scan_found_) +
            " songs listed by the requests a tap on the game's sound routine reports.");
      scanning_ = false;
      return;
   }
   if (!usable && !cancel_ && !references.empty() && find_song_variable())
   {
      usable = true;
      // Scans now start from a later moment: what plays there is the new baseline.
      core.load_state(scan_state_);
      run_frames(kRoutineSettle);
      have_baseline = rip_state(core.save_state(), 0, false, baseline, err);
      base_print = have_baseline ? &baseline.print : nullptr;
   }
   if (!usable && !cancel_ && !(nes_ && !nsf_table.empty()))
      usable = nes_ ? find_song_start_nes(base_print) : find_song_start(base_print);
   if (!usable)
   {
      core.load_state(scan_state_);
      core.set_skip_video(false);
      core_lock.unlock();
      set_scan_message(cancel_ ? "Scan stopped." : song_table.found ?
            "Listed the ROM song table's songs, but could not find how this game starts songs to check them." :
            "Could not find how this game starts songs. Play it and rip songs as you hear them.");
      save_library();
      scanning_ = false;
      return;
   }
   const SongStart s = scan_start_;

   // Songs must be numbered the way Proteus will see them in the game.
   set_scan_message("Checking where the game keeps its song number...");
   bool by_address = false;
   choose_song_address(s, by_address);
   scan_changed_ = true;   // record the confirmed start in the game database

   if (song_table.found)
   {
      first = 0;
      last = (int)song_table.entries.size() - 1;
   }
   else if (first < 0)
   {
      first = 1;
      last = 255;
   }
   std::vector<uint32_t> values;
   for (int v = first; v <= last; v++)
      values.push_back((uint32_t)v);
   // NES: when the .nsf starts its songs with this very request, its values are the game's songs.
   // They are listed by name at once, and only they are played to check them.
   auto nsf_songs = nsf_table.end();
   if (nes_ && s.kind == SongStart::RAM && s.bytes.empty() && !by_address)
      nsf_songs = nsf_table.find(s.address);
   if (nsf_songs != nsf_table.end())
   {
      values.clear();
      std::string err;
      int listed = 0;
      for (const auto &entry : nsf_songs->second)
      {
         values.push_back(entry.first);
         std::lock_guard<std::mutex> lock(songs_mutex);
         FoundSong *existing = find_song(entry.first);
         if (existing && !existing->reference.empty())
            continue;
         FoundSong song;
         song.value = entry.first;
         if (use_reference(song, references.song(entry.second), err))
         {
            add_song_locked(song);
            listed++;
         }
      }
      log("the reference .nsf starts " + std::to_string(values.size()) + " songs with " + describe_song_start(s) +
          "; listed " + std::to_string(listed) + " of them");
   }
   scan_total_ = std::max(1, (int)values.size());
   scan_done_ = 0;
   int ignored = 0, unmatched = 0, confirmed = 0, aliases = 0;
   std::vector<std::string> heard;
   for (uint32_t v : values)
   {
      if (cancel_)
         break;
      set_scan_message("Trying song " + hex2((uint32_t)v) + "...");
      if (!start_song(s, (uint32_t)v))
      {
         set_scan_message("Could not start songs with " + describe_song_start(s) + ".");
         break;
      }
      std::vector<uint8_t> onset;
      int onset_frame = run_until_heard(s.settle_frames, onset);

      uint32_t key = (uint32_t)v, now = 0;
      if (by_address && read_song_value(scan_address_, now))
         key = now;

      // Rip from the song's first notes, so the rip plays it from its start.
      if (!onset.empty())
         core.load_state(onset);
      FoundSong song;
      if (rip_state(onset.empty() ? core.save_state() : onset, key, true, song, err))
      {
         if (!keep_rips_dir.empty())
            write_text(keep_rips_dir + "\\rip_" + hex2(key).substr(2) + "." + music_extension(song.data), std::string(song.data.begin(), song.data.end()));
         // The song already listed under this number, and the versions the ROM table has for it.
         std::vector<std::string> same;
         {
            std::lock_guard<std::mutex> lock(songs_mutex);
            if (FoundSong *existing = find_song(key))
               if (!existing->reference.empty())
                  same.push_back(existing->reference);
         }
         if (!same.empty() && song_table.found && key < song_table.versions.size())
            for (int r : song_table.versions[key])
               same.push_back(references.song(r).title);
         if (name_by_reference(song, scan_before_spc_.empty() ? nullptr : &scan_before_spc_,
                  onset.empty() ? s.settle_frames : onset_frame, same))
         {
            // Every number of a reference song is kept: the game may play it under several. NES games
            // that read requests as bits (Super Mario Bros.: 06 plays 02's song) would list a song
            // under dozens of numbers, so there only its first number and single bits are kept.
            std::lock_guard<std::mutex> lock(songs_mutex);
            if (std::find(heard.begin(), heard.end(), song.reference) == heard.end())
               heard.push_back(song.reference);
            FoundSong *existing = find_song(key);
            bool listed = false;
            for (const auto &x : songs)
               listed = listed || (x.has_value && x.reference == song.reference);
            if (existing && existing->reference == song.reference)
               confirmed++;
            else if (nes_ && !existing && listed && (key & (key - 1)) != 0)
               aliases++;
            else
            {
               if (existing && !existing->reference.empty())
                  log("song " + hex2(key) + " plays \"" + song.reference + "\", not \"" + existing->reference + "\" as listed");
               add_song_locked(song);
               scan_found_++;
            }
         }
         else if (have_baseline && same_song(song.print, baseline.print))
            ignored++;
         else
         {
            std::lock_guard<std::mutex> lock(songs_mutex);
            FoundSong *existing = find_song(key);
            if (!references.empty())
               unmatched++;
            bool dup = existing && !existing->reference.empty();
            for (const auto &x : songs)
               if ((!existing || &x != existing) && same_song(x.print, song.print))
                  dup = true;
            if (!dup)
            {
               song.title = existing ? existing->title : default_title(song.value, song.kind);
               add_song_locked(song);
               scan_found_++;
            }
         }
      }
      scan_done_++;
   }

   // With the songs known, a value that stops the music lets profiles keep the sound effects.
   if (nes_ && !cancel_ && s.kind == SongStart::RAM && s.bytes.empty() &&
         !(scan_silence_.known && scan_silence_.address == s.address))
      find_silence(s);
   if (nes_ && !cancel_ && scan_silence_.known && scan_silence_.address == s.address)
      follow_requests(s);

   core.load_state(scan_state_);
   core.audio().clear();
   core.set_skip_video(false);
   core_lock.unlock();

   save_library();
   std::string summary = (cancel_ ? "Scan stopped: " : "Scan finished: ") + std::to_string((int)scan_found_) + " new songs";
   if (!references.empty())
   {
      summary += ", " + std::to_string(heard.size()) + " of " + std::to_string(references.size()) + " reference songs heard";
      if (confirmed)
         summary += " (" + std::to_string(confirmed) + " already listed)";
      if (unmatched)
         log(std::to_string(unmatched) + " song numbers played something that is not in the reference set");
      if (aliases)
         log(std::to_string(aliases) + " more song numbers play songs already listed; they are not listed again");
   }
   if (scan_found_ == 0 && heard.empty())
      summary += ". Songs loaded later in the game may need Scan from this moment (Advanced).";
   set_scan_message(summary);
   log(summary + " (" + std::to_string(ignored) + " values changed nothing)");
   scanning_ = false;
}

// ---------------------------------------------------------------------------
// Live play
// ---------------------------------------------------------------------------

void RomSession::play_frame(uint16_t buttons)
{
   core.run_frame(buttons);
   std::vector<uint8_t> state = core.save_state();

   // NES music is named from what was just heard; keep the last seconds of it.
   const size_t kLiveSamples = 12 * kRipRate;
   if (nes_)
   {
      append_mono(core.audio(), core.sample_rate(), live_phase_, live_audio_);
      if (live_audio_.size() > kLiveSamples * 2)
         live_audio_.erase(live_audio_.begin(), live_audio_.end() - kLiveSamples);
   }

   adopt_learned_address();
   if (++live_frames_ % 120 == 0 && !references.empty() && !learning_busy_ && !state.empty())
   {
      size_t size = 0;
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      if (ram && nes_ && live_audio_.size() >= kLiveSamples)
         learn_moment(wav_file(std::vector<int16_t>(live_audio_.end() - kLiveSamples, live_audio_.end()), kRipRate),
               std::vector<uint8_t>(ram, ram + size));
      else if (ram && !nes_)
         learn_moment(spc_of_state(state), std::vector<uint8_t>(ram, ram + size));
   }

   // Any command the game sends the sound CPU may start a song: once the ports
   // have been quiet long enough for it to start, rip what plays.
   uint8_t ports[4];
   if (!nes_ && spc_snes9x_ports(state, ports) && memcmp(ports, last_ports_, 4) != 0)
   {
      memcpy(last_ports_, ports, 4);
      command_state_ = state;
      pending_rip_frames_ = std::max(150, start.settle_frames);
   }

   uint32_t v = 0;
   if (read_song_value(address, v) && !(address.latch && address.bytes.empty() && v == 0))
   {
      if (v != candidate_)
      {
         candidate_ = v;
         candidate_frames_ = 0;
      }
      if (++candidate_frames_ >= (unsigned)std::max(1, address.debounce) && (!have_last_ || v != last_value_))
      {
         last_value_ = v;
         have_last_ = true;
         pending_rip_frames_ = std::max(150, start.settle_frames);
         log("song " + hex2(v));
      }
   }

   if (pending_rip_frames_ >= 0 && --pending_rip_frames_ < 0)
   {
      std::string message;
      // rip_playing may step a frame or two; that is part of play, so no restore.
      if (rip_playing(state, true, message))
         log(message);
   }
}

void RomSession::learn_moment(std::vector<uint8_t> spc, std::vector<uint8_t> ram)
{
   if (learn_thread_.joinable())
      learn_thread_.join();
   learning_busy_ = true;
   learn_thread_ = std::thread([this, spc = std::move(spc), ram = std::move(ram)]() {
      int song = reference_by_notes(spc);
      if (song >= 0)
      {
         std::lock_guard<std::mutex> lock(learn_mutex_);
         size_t n = ram.size();
         if (learn_broken_.size() != n)
         {
            learn_broken_.assign(n, 0);
            learn_seen_.clear();
            learn_songs_.clear();
         }
         size_t row = std::find(learn_songs_.begin(), learn_songs_.end(), song) - learn_songs_.begin();
         bool new_song = row == learn_songs_.size();
         if (new_song)
         {
            learn_songs_.push_back(song);
            learn_seen_.push_back(std::vector<int16_t>(n, -1));
         }
         std::vector<int16_t> &seen = learn_seen_[row];
         for (size_t a = 0; a < n; a++)
         {
            if (learn_broken_[a])
               continue;
            if (seen[a] < 0)
               seen[a] = ram[a];
            else if (seen[a] != ram[a])
               learn_broken_[a] = 1;
         }
         if (new_song)
            log("heard \"" + references.song(song).title + "\" while playing");

         // Bytes still following the music: one value per song, a different one for each.
         if (learn_songs_.size() >= 2)
         {
            std::vector<uint32_t> left;
            for (size_t a = 0; a < n; a++)
            {
               if (learn_broken_[a])
                  continue;
               bool used[256] = { false };
               bool unique = true;
               for (size_t r = 0; r < learn_seen_.size() && unique; r++)
               {
                  uint8_t v = (uint8_t)learn_seen_[r][a];
                  unique = !used[v];
                  used[v] = true;
               }
               if (unique)
                  left.push_back((uint32_t)a);
            }
            learn_left_ = left.size() <= 8 ? left : std::vector<uint32_t>();
            if ((int)left.size() != learn_candidates_)
            {
               learn_candidates_ = (int)left.size();
               log(std::to_string(left.size()) + " RAM bytes follow the music after " + std::to_string(learn_songs_.size()) + " songs");
            }
            if (left.size() == 1 && learn_songs_.size() >= 3 && (!learned_ready_ || learned_address_ != left[0]))
            {
               learned_address_ = left[0];
               learned_values_.clear();
               for (size_t r = 0; r < learn_songs_.size(); r++)
                  learned_values_[learn_songs_[r]] = (uint8_t)learn_seen_[r][left[0]];
               learned_ready_ = true;
            }
         }
      }
      learning_busy_ = false;
   });
}

std::string RomSession::learning_status()
{
   std::lock_guard<std::mutex> lock(learn_mutex_);
   if (learn_songs_.empty())
      return "";
   std::string s = std::to_string(learn_songs_.size()) + " songs heard";
   if (learn_candidates_ >= 0)
      s += ", " + std::to_string(learn_candidates_) + " RAM bytes follow the music";
   if (!learn_left_.empty())
   {
      s += ":";
      for (uint32_t a : learn_left_)
         s += " $" + hex4(a);
   }
   return s;
}

// On the thread that plays: makes a learned song address the one Proteus follows, and numbers
// the reference songs already ripped.
void RomSession::adopt_learned_address()
{
   std::map<int, uint8_t> values;
   uint32_t at;
   {
      std::lock_guard<std::mutex> lock(learn_mutex_);
      if (!learned_ready_)
         return;
      learned_ready_ = false;
      at = learned_address_;
      values = learned_values_;
   }
   if (address.known && address.memory == 0 && address.bytes.empty() && address.address == at)
      return;
   address = SongAddress();
   address.known = true;
   address.address = at;
   address.size = 1;
   address.debounce = 2;
   address_source = "learned while playing";
   have_last_ = false;
   std::string list;
   for (const auto &v : values)
      list += (list.empty() ? "" : ", ") + references.song(v.first).title + " " + hex2(v.second);
   log("the music follows $" + hex4(at) + " (" + list + "); Proteus will follow it");
   save_to_game_db("learned while playing");
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      for (auto &s : songs)
      {
         if (s.has_value || s.reference.empty())
            continue;
         for (const auto &v : values)
            if (references.song(v.first).title == s.reference && !find_song(v.second))
            {
               s.has_value = true;
               s.value = v.second;
               break;
            }
      }
      std::stable_sort(songs.begin(), songs.end(), [](const FoundSong &a, const FoundSong &b) {
         if (a.has_value != b.has_value)
            return a.has_value;
         return a.value < b.value;
      });
   }
   save_library();
}

bool RomSession::rip_playing(const std::vector<uint8_t> &state, bool automatic, std::string &message)
{
   if (state.empty())
   {
      message = "This core cannot save states.";
      return false;
   }
   bool has_value = address.known && have_last_;
   uint32_t value = has_value ? last_value_ : 0;
   FoundSong song;
   std::string err;
   // An NES rip records the seconds ahead, then play carries on from where it was.
   bool ripped = rip_state(state, value, has_value, song, err);
   if (nes_)
   {
      core.load_state(state);
      core.audio().clear();
   }
   if (!ripped)
   {
      message = err == "silent" ? "Nothing is playing right now." : "Could not rip: " + err;
      return false;
   }
   std::vector<uint8_t> before = spc_of_state(command_state_);
   // Live rips follow a song change the game made itself.
   bool named = name_by_reference(song, before.empty() ? nullptr : &before, -1);
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      for (const auto &s : songs)
         if (named ? s.reference == song.reference && (!has_value || !s.has_value || s.value == value)
                   : same_song(s.print, song.print))
         {
            message = "Already in the list as \"" + s.title + "\".";
            return false;
         }
      // A different song under a number we already have: the address did not
      // follow this change (or a jingle played over it), so keep it unnumbered.
      FoundSong *existing = has_value ? find_song(value) : nullptr;
      if (existing && !(named && existing->reference.empty()))
      {
         if (automatic && song.kind == SONG_JINGLE)
            return false;
         song.has_value = false;
      }
      int rips = 0;
      for (const auto &s : songs)
         rips += s.has_value ? 0 : 1;
      if (!named)
         song.title = song.has_value ? default_title(value, song.kind) : "Rip " + std::to_string(rips + 1);
      message = "Ripped \"" + song.title + "\".";
      add_song_locked(song);
   }
   save_library();
   return true;
}

bool RomSession::rip_now(std::string &message)
{
   return rip_playing(core.save_state(), false, message);
}

// ---------------------------------------------------------------------------
// TAS movies
// ---------------------------------------------------------------------------

std::string RomSession::movies_dir() const
{
   return app_dir_ + "\\movies\\" + hex4(rom_.crc32 >> 16) + hex4(rom_.crc32 & 0xFFFF);
}

std::vector<std::string> RomSession::downloaded_movies() const
{
   std::vector<std::string> out;
   for (const auto &f : list_files(movies_dir()))
   {
      std::string e = lower_ext(f);
      if (e == "bk2" || e == "bkm")
         out.push_back(movies_dir() + "\\" + f);
   }
   std::sort(out.begin(), out.end());
   return out;
}

std::string RomSession::tool_message()
{
   std::lock_guard<std::mutex> lock(tool_mutex_);
   return tool_message_;
}

void RomSession::set_tool_message(const std::string &message, float progress)
{
   std::lock_guard<std::mutex> lock(tool_mutex_);
   tool_message_ = message;
   tool_progress_ = progress;
}

std::vector<TasPublication> RomSession::movie_list()
{
   std::lock_guard<std::mutex> lock(tool_mutex_);
   return movie_list_;
}

void RomSession::run_tool(std::function<void()> job)
{
   if (tool_busy_)
      return;
   if (tool_thread_.joinable())
      tool_thread_.join();
   tool_busy_ = true;
   tool_thread_ = std::thread([this, job]() {
      job();
      tool_busy_ = false;
   });
}

void RomSession::find_movies()
{
   std::string name = display_name_;
   run_tool([this, name]() {
      set_tool_message("Looking up " + name + " on TASVideos...", 0);
      // The publication list is the same for every game; fetch it once per run.
      static std::mutex cache_mutex;
      static std::vector<TasPublication> cache;
      std::vector<TasPublication> all;
      std::string err;
      {
         std::lock_guard<std::mutex> lock(cache_mutex);
         if (cache.empty())
            tasvideos_snes_publications(cache, err);
         all = cache;
      }
      std::vector<TasPublication> found = tasvideos_for_game(all, name);
      {
         std::lock_guard<std::mutex> lock(tool_mutex_);
         movie_list_ = found;
      }
      int playable = 0;
      for (const auto &p : found)
         playable += p.playable();
      if (all.empty())
         set_tool_message("Could not reach TASVideos: " + err, 0);
      else if (found.empty())
         set_tool_message("TASVideos has no movie of " + name + ".", 0);
      else
         set_tool_message("TASVideos has " + std::to_string(found.size()) + (found.size() == 1 ? " movie" : " movies") + " of " + name + ", " +
               std::to_string(playable) + " made with BizHawk.", 1);
   });
}

void RomSession::download_movie(const TasPublication &pub)
{
   std::string dir = movies_dir();
   run_tool([this, pub, dir]() {
      set_tool_message("Downloading " + pub.title + "...", 0);
      std::string path, err;
      if (tasvideos_download(pub, dir, path, err))
      {
         set_tool_message("Downloaded " + file_name(path) + ".", 1);
         log("downloaded the TAS movie " + file_name(path) + " (" + pub.title + ")");
      }
      else
         set_tool_message("Could not download the movie: " + err, 0);
   });
}

void RomSession::install_bizhawk()
{
   run_tool([this]() {
      std::string err;
      if (!bizhawk_install(app_dir_, [this](const std::string &m, float p) { set_tool_message(m, p); }, nullptr, err))
         set_tool_message("Could not install BizHawk: " + err, 0);
   });
}

void RomSession::start_movie(const std::string &movie_path, int speed, bool show)
{
   if (scanning_ || !open_ || loading_refs_)
      return;
   if (references.empty())
   {
      set_scan_message("Add this game's reference songs first: the songs a movie plays are named by them.");
      return;
   }
   if (worker_.joinable())
      worker_.join();
   cancel_ = false;
   scanning_ = true;
   scan_done_ = 0;
   scan_total_ = 1;
   scan_found_ = 0;
   scan_address_ = address;
   scan_start_ = start;
   scan_silence_ = silence;
   scan_tap_ = tap;
   scan_patch_ = patch;
   scan_address_source_ = address_source;
   scan_start_source_ = start_source;
   scan_changed_ = false;
   movie_speed_ = speed;
   worker_ = std::thread([this, movie_path, show]() { movie_thread(movie_path, show); });
}

static std::string movie_time(uint32_t frames)
{
   char buf[32];
   unsigned s = frames / 60;
   if (s >= 3600)
      snprintf(buf, sizeof(buf), "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
   else
      snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
   return buf;
}

void RomSession::movie_thread(std::string movie_path, bool show)
{
   BizHawkRun run;
   std::string err;
   set_scan_message("Starting BizHawk...");
   int speed = movie_speed_;
   if (!run.start(app_dir_, rom_path_, movie_path, speed, show, err))
   {
      set_scan_message("Could not play the movie: " + err);
      scanning_ = false;
      return;
   }
   log("playing " + file_name(movie_path) + " in BizHawk");
   MovieLearner learner(references);
   size_t heard = 0;
   // A song joins the list as soon as it is heard, without a number; numbers come at the end.
   auto list_song = [&](int r, bool has_value, uint8_t value) {
      const ReferenceSong &ref = references.song(r);
      FoundSong song;
      song.has_value = has_value;
      song.value = value;
      if (!use_reference(song, ref, err))
         return false;
      std::lock_guard<std::mutex> lock(songs_mutex);
      for (auto it = songs.begin(); it != songs.end();)
      {
         if (it->reference == ref.title && (!has_value || (it->has_value && it->value == value)))
            return false;   // listed already
         // The unnumbered entry gives way to the numbered one.
         if (has_value && it->reference == ref.title && !it->has_value)
         {
            std::remove(it->path.c_str());
            it = songs.erase(it);
         }
         else
            ++it;
      }
      add_song_locked(song);
      return true;
   };
   uint32_t last_frame = 0;
   auto last_change = std::chrono::steady_clock::now();
   bool stalled = false;
   while (true)
   {
      if (cancel_)
      {
         run.stop();
         break;
      }
      if (movie_speed_ != speed)
         run.set_speed(speed = movie_speed_);
      uint32_t frame = 0;
      std::vector<uint8_t> d;
      bool got = false;
      while (!cancel_ && run.next_dump(frame, d))
      {
         got = true;
         learner.add(d.data(), d.data() + 0x10000, d.size() - 0x10000);
         for (; heard < learner.heard().size(); heard++)
         {
            log("heard \"" + references.song(learner.heard()[heard]).title + "\" at " + movie_time(frame) + " in the movie");
            if (list_song(learner.heard()[heard], false, 0))
               save_library();
         }
         scan_found_ = (int)heard;
      }
      bool done = run.finished();
      uint32_t f = run.frame(), len = run.length();
      if (len)
      {
         scan_total_ = (int)len;
         scan_done_ = (int)std::min(f, len);
      }
      auto now = std::chrono::steady_clock::now();
      if (f != last_frame)
      {
         last_frame = f;
         last_change = now;
      }
      set_scan_message("Playing the movie in BizHawk: " + movie_time(f) + " of " + movie_time(len) + ", " +
            std::to_string(heard) + " songs heard");
      if (done)
      {
         while (run.next_dump(frame, d))
            learner.add(d.data(), d.data() + 0x10000, d.size() - 0x10000);
         break;
      }
      if (now - last_change > std::chrono::seconds(90))
      {
         stalled = true;
         run.stop();
         break;
      }
      if (!got)
         std::this_thread::sleep_for(std::chrono::milliseconds(250));
   }

   MovieLearner::Result res = learner.result();
   std::string outcome;
   if (stalled && last_frame == 0)
      outcome = "BizHawk did not start playing the movie. It needs .NET Framework 4.8 and the Microsoft Visual C++ runtime; "
            "open it from " + dir_of(bizhawk_exe(app_dir_)) + " to see why.";
   else if (learner.heard().empty())
      outcome = "No reference song was heard. The movie may be for another version of the game, or fall out of sync.";
   else if (!res.found)
   {
      std::string top;
      for (size_t i = 0; i < res.top.size() && i < 3; i++)
         top += (top.empty() ? " (closest: $" : ", $") + hex4(res.top[i].first) + " tells " + std::to_string(res.top[i].second) + " apart";
      if (!top.empty())
         top += ")";
      outcome = std::to_string(learner.heard().size()) + " songs heard, but no RAM byte holds one number per song" + top +
            ". The game may start songs through a command instead.";
   }
   else
   {
      SongAddress a;
      a.known = true;
      a.address = res.address;
      a.size = 1;
      a.debounce = 2;
      scan_address_ = a;
      scan_address_source_ = "learned from a TAS movie";
      scan_changed_ = true;
      std::string list;
      for (const auto &v : res.values)
         list += (list.empty() ? "" : ", ") + references.song(v.first).title + " " + hex2(v.second);
      log("the music follows $" + hex4(res.address) + " (" + list + ")");
      if (!res.ties.empty())
      {
         std::string ties;
         for (uint32_t t : res.ties)
            ties += " $" + hex4(t);
         log("these bytes follow the music as well:" + ties);
      }
      outcome = std::to_string(learner.heard().size()) + " songs heard; the song address is $" + hex4(res.address) + ".";
   }
   // With the song address known, the songs heard get their numbers.
   int added = 0;
   for (int r : learner.heard())
   {
      auto v = res.found ? res.values.find(r) : res.values.end();
      added += v != res.values.end() ? list_song(r, true, v->second) : list_song(r, false, 0);
   }
   if (added)
      save_library();
   log(outcome);
   set_scan_message((cancel_ ? "Stopped. " : "") + outcome);
   scanning_ = false;
}
