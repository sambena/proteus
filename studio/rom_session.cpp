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
#include <sstream>

#include "gme.h"
#include "platform.h"
#include "movie_learner.h"
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
static const char kLibraryHeader[] = "# proteus-studio library 3";
static const char kLibraryHeader2[] = "# proteus-studio library 2";
static const uint32_t kTrials[]  = { 1, 2, 3, 5, 8, 13 };

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

bool analyze_spc(const std::vector<uint8_t> &spc, SongPrint &print, std::string &error)
{
   Music_Emu *emu = nullptr;
   if (gme_err_t e = gme_open_data(spc.data(), (long)spc.size(), &emu, kPrintRate))
   {
      error = e;
      return false;
   }
   gme_ignore_silence(emu, 1);
   if (gme_err_t e = gme_start_track(emu, 0))
   {
      error = e;
      gme_delete(emu);
      return false;
   }

   const int window = kPrintRate * kWindowMs / 1000;
   std::vector<short> buf(window * 2);
   print.envelope.clear();
   print.brightness.clear();
   int prev = 0;
   for (int w = 0; w < kPrintSeconds * 1000 / kWindowMs; w++)
   {
      if (gme_play(emu, (int)buf.size(), buf.data()))
         break;
      double sum = 0, hp = 0;
      for (size_t i = 0; i < buf.size(); i += 2)
      {
         int mono = (buf[i] + buf[i + 1]) / 2;
         sum += (double)mono * mono;
         hp += (double)(mono - prev) * (mono - prev);
         prev = mono;
      }
      print.envelope.push_back((float)std::sqrt(sum / window));
      print.brightness.push_back((float)std::sqrt(hp / window));
   }
   gme_delete(emu);

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

      // Once per run: learn this snes9x version's save state layout.
      static bool calibrated = false;
      if (!calibrated)
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
   rom_.load(core.content_data());

   address = SongAddress();
   start = SongStart();
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

   profile_path_.clear();
   char found[2048];
   if (px_engine_find_profile(content.c_str(), system_dir.c_str(), found, sizeof(found)))
   {
      static px_profile p;   // large; only used here, on the UI thread
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
   if (!rom_.data.empty())
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
   references.clear();
   reference_notes.clear();
   song_table = SongTable();
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
         int n = download_reference_songs(names, dir, say, err);
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
      if (!refs.empty())
      {
         say("Looking for the song table in the ROM...");
         table = refs.find_song_table(rom_);
         say("Listening to the reference songs...");
         notes.resize(refs.size());
         for (size_t i = 0; i < refs.size(); i++)
            spc_notes(refs.song(i).spc, notes[i], err);
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
      const ReferenceSong &ref = references.song(r);
      FoundSong song;
      song.value = (uint32_t)v;
      if (!analyze_spc(ref.spc, song.print, err))
         continue;
      if (!classify(song.print, song.kind))
         song.kind = SONG_JINGLE;
      song.title = ref.title;
      song.reference = ref.title;
      song.spc_path.assign((const char*)ref.spc.data(), ref.spc.size());
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
         if (!analyze_spc(ref.spc, song.print, err))
            continue;
         if (!classify(song.print, song.kind))
            song.kind = SONG_JINGLE;
         song.title = ref.title;
         song.reference = ref.title;
         song.spc_path.assign((const char*)ref.spc.data(), ref.spc.size());
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
   std::vector<uint8_t> rip(song.spc_path.begin(), song.spc_path.end());
   ReferenceSet::Match m = references.match_spc(rip, before);

   // Some drivers load a whole group of songs at once and start one by moving a pointer, so
   // little memory tells those songs apart. A weak memory match must also sound like its
   // song; without a memory match, a song whose notes clearly match one reference is it.
   const double kStrongMemory = 4.0, kNotesMatch = 0.03, kNotesLead = 0.03;
   if ((m.index < 0 || m.score < kStrongMemory) && reference_notes.size() == references.size())
   {
      SongNotes notes;
      std::string err;
      if (!spc_notes(rip, notes, err))
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
         if (notes_distance(notes, before_notes_, &shift) < kNotesMatch)
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
         if (next - ranked[0].first < kNotesLead)
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
   const ReferenceSong &ref = references.song(m.index);
   std::string err;
   SongPrint print;
   if (analyze_spc(ref.spc, print, err))
   {
      song.print = print;
      if (!classify(print, song.kind))
         song.kind = SONG_JINGLE;
   }
   song.title = ref.title;
   song.reference = ref.title;
   song.spc_path.assign((const char*)ref.spc.data(), ref.spc.size());
   return true;
}

std::vector<uint8_t> RomSession::spc_of_state(const std::vector<uint8_t> &state)
{
   std::vector<uint8_t> spc;
   std::string err;
   if (state.empty() || !spc_from_snes9x_state(state, SpcTags(), spc, nullptr, err))
      spc.clear();
   return spc;
}

// ---------------------------------------------------------------------------
// Library on disk: %APPDATA%/ProteusStudio/library/<game>/songs.txt + rips
// ---------------------------------------------------------------------------

std::string RomSession::library_dir() const
{
   return app_dir_ + "\\library\\" + sanitize_filename(game_name_);
}

void RomSession::save_library()
{
   std::string dir = library_dir();
   make_dirs(dir);
   std::ostringstream out;
   out << kLibraryHeader << "\n# value\thas_value\tkind\tfile\ttitle\treference\tloudness tail count envelope... brightness...\n";
   std::lock_guard<std::mutex> lock(songs_mutex);
   for (const auto &s : songs)
   {
      out << s.value << '\t' << (s.has_value ? 1 : 0) << '\t' << (int)s.kind << '\t'
          << file_name(s.spc_path) << '\t' << s.title << '\t' << s.reference << '\t' << s.print.loudness << ' ' << s.print.tail
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
   bool v3 = text.compare(0, strlen(kLibraryHeader), kLibraryHeader) == 0;
   bool current = v3 || text.compare(0, strlen(kLibraryHeader2), kLibraryHeader2) == 0;
   const int fields = v3 ? 6 : 5;
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
      s.spc_path = dir + "\\" + f[3];
      s.title = f[4];
      if (v3)
         s.reference = f[5];
      if (!file_exists(s.spc_path))
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
         if (!read_file_bytes(s.spc_path, spc) || !analyze_spc(spc, s.print, err))
            continue;
      }
      songs.push_back(s);
   }
   lock.unlock();
   if (!v3 && !songs.empty())
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
   for (int tries = 0;; tries++)
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
   // Keep the bytes in spc_path until the song is accepted.
   out.spc_path.assign((const char*)spc.data(), spc.size());
   return true;
}

void RomSession::add_song_locked(FoundSong song)
{
   std::string dir = library_dir();
   make_dirs(dir);
   char name[64];
   if (song.has_value)
      snprintf(name, sizeof(name), "%02X.spc", (unsigned)song.value);
   else
   {
      unsigned n = 1;
      do snprintf(name, sizeof(name), "rip_%u.spc", n++);
      while (file_exists(dir + "\\" + name));
   }
   std::string path = dir + "\\" + name;
   FILE *f = px_fopen(path.c_str(), "wb");
   if (!f)
      return;
   fwrite(song.spc_path.data(), 1, song.spc_path.size(), f);
   fclose(f);
   song.spc_path = path;

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
      t.settled.assign(ram, ram + ram_size);
      run_frames(120);
      ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
      t.later.assign(ram, ram + ram_size);
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
   if (ranked.empty() || ranked[0].first >= 0.05)
      return -1;
   for (const auto &r : ranked)
      if (stem(r.second) != stem(ranked[0].second))
      {
         if (r.first - ranked[0].first < 0.03)
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
      write_text(keep_rips_dir + "\\before.spc",std::string(scan_before_spc_.begin(), scan_before_spc_.end()));
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
   std::vector<uint8_t> ram_before(ram, ram + ram_size);
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
   if (!usable && !cancel_ && !references.empty() && find_song_variable())
   {
      usable = true;
      // Scans now start from a later moment: what plays there is the new baseline.
      core.load_state(scan_state_);
      run_frames(kRoutineSettle);
      have_baseline = rip_state(core.save_state(), 0, false, baseline, err);
      base_print = have_baseline ? &baseline.print : nullptr;
   }
   if (!usable && !cancel_)
      usable = find_song_start(base_print);
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
   scan_total_ = std::max(1, last - first + 1);
   scan_done_ = 0;
   int ignored = 0, unmatched = 0, confirmed = 0;
   std::vector<std::string> heard;
   for (int v = first; v <= last && !cancel_; v++)
   {
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
            write_text(keep_rips_dir + "\\rip_" + hex2(key).substr(2) + ".spc", song.spc_path);
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
            // Every number of a reference song is kept: the game may play it under several.
            std::lock_guard<std::mutex> lock(songs_mutex);
            if (std::find(heard.begin(), heard.end(), song.reference) == heard.end())
               heard.push_back(song.reference);
            FoundSong *existing = find_song(key);
            if (existing && existing->reference == song.reference)
               confirmed++;
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

   adopt_learned_address();
   if (++live_frames_ % 120 == 0 && !references.empty() && !learning_busy_ && !state.empty())
   {
      size_t size = 0;
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      if (ram)
         learn_moment(spc_of_state(state), std::vector<uint8_t>(ram, ram + size));
   }

   // Any command the game sends the sound CPU may start a song: once the ports
   // have been quiet long enough for it to start, rip what plays.
   uint8_t ports[4];
   if (spc_snes9x_ports(state, ports) && memcmp(ports, last_ports_, 4) != 0)
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
   if (!rip_state(state, value, has_value, song, err))
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
         set_tool_message("TASVideos has " + std::to_string(found.size()) + " movies of " + name + ", " +
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
            log("heard \"" + references.song(learner.heard()[heard]).title + "\" at " + movie_time(frame) + " in the movie");
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
   // The songs heard join the list, numbered when the address is known.
   int added = 0;
   for (int r : learner.heard())
   {
      const ReferenceSong &ref = references.song(r);
      FoundSong song;
      auto v = res.found ? res.values.find(r) : res.values.end();
      song.has_value = v != res.values.end();
      song.value = song.has_value ? v->second : 0;
      if (!analyze_spc(ref.spc, song.print, err))
         continue;
      if (!classify(song.print, song.kind))
         song.kind = SONG_JINGLE;
      song.title = ref.title;
      song.reference = ref.title;
      song.spc_path.assign((const char *)ref.spc.data(), ref.spc.size());
      std::lock_guard<std::mutex> lock(songs_mutex);
      bool listed = false;
      for (const auto &s : songs)
         listed = listed || (s.reference == ref.title && (!song.has_value || (s.has_value && s.value == song.value)));
      if (listed)
         continue;
      add_song_locked(song);
      added++;
   }
   if (added)
      save_library();
   log(outcome);
   set_scan_message((cancel_ ? "Stopped. " : "") + outcome);
   scanning_ = false;
}
