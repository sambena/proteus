// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rom_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "gme.h"
#include "platform.h"
#include "spc_rip.h"

extern "C" {
#include "engine.h"
#include "profile.h"
}

static const unsigned kMemoryIds[] = { RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_VIDEO_RAM };
static const int kBootFrames   = 720;   // where scans start when no moment was chosen
static const int kPrintRate    = 32000;
static const int kPrintSeconds = 10;
static const int kWindowMs     = 100;

static std::string hex2(uint32_t v)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "0x%02X", (unsigned)v);
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
   for (int w = 0; w < kPrintSeconds * 1000 / kWindowMs; w++)
   {
      if (gme_play(emu, (int)buf.size(), buf.data()))
         break;
      double sum = 0;
      for (short s : buf)
         sum += (double)s * s;
      print.envelope.push_back((float)std::sqrt(sum / buf.size()));
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

bool same_song(const SongPrint &a, const SongPrint &b)
{
   size_t n = std::min(a.envelope.size(), b.envelope.size());
   if (!n)
      return false;
   double diff = 0, total = 0;
   for (size_t i = 0; i < n; i++)
   {
      diff += std::fabs(a.envelope[i] - b.envelope[i]);
      total += a.envelope[i] + b.envelope[i];
   }
   return total <= 1 || diff / total < 0.04;
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

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

RomSession::RomSession() {}

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
   std::string save_dir = app_data_dir() + "\\saves";
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

   const auto &data = core.content_data();
   std::string header_title = detect_rom_header_title(data.data(), data.size(), system_);
   preset_ = detect_preset(data.data(), data.size(), rom_path);
   display_name_ = preset_ ? preset_->name : game_name_;
   if (system_.empty())
      system_ = preset_ ? preset_->system : "SNES";
   (void)header_title;

   address = SongAddress();
   profile_path_.clear();
   char found[2048];
   if (px_engine_find_profile(content.c_str(), system_dir.c_str(), found, sizeof(found)))
   {
      static px_profile p;   // large; only used here, on the UI thread
      char err[1200];
      if (px_profile_load(&p, found, err, sizeof(err)))
      {
         profile_path_ = found;
         address.known = true;
         address.source = "profile";
         for (int i = 0; i < 3; i++)
            if (kMemoryIds[i] == p.memory_id)
               address.memory = i;
         address.address = p.address;
         address.size = (int)p.size;
         address.latch = p.latch;
         address.debounce = (int)p.debounce;
      }
      else
         log(std::string("profile ") + found + ": " + err);
   }
   if (!address.known && preset_)
   {
      address.known = true;
      address.source = "preset";
      address.memory = preset_->memory;
      address.address = preset_->address;
      address.size = preset_->size;
      address.latch = preset_->latch;
      address.debounce = std::max(1, preset_->debounce);
   }
   if (address.known && address.latch)
   {
      address.scan_known = true;
      address.scan_address = address.address;
   }
   else if (preset_ && preset_->latch && preset_->memory == address.memory)
   {
      address.scan_known = true;
      address.scan_address = preset_->address;
   }

   scan_state_.clear();
   have_last_ = false;
   pending_rip_frames_ = -1;
   open_ = true;
   load_library();
   log("opened " + rom_path + (address.known ? " (song address " + hex2(address.address) + " from " + address.source + ")" : ""));
   return true;
}

void RomSession::close()
{
   stop_scan();
   if (worker_.joinable())
      worker_.join();
   std::lock_guard<std::mutex> lock(core_mutex);
   core.unload();
   std::lock_guard<std::mutex> songs_lock(songs_mutex);
   songs.clear();
   open_ = false;
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

// ---------------------------------------------------------------------------
// Library on disk: %APPDATA%/ProteusStudio/library/<game>/songs.txt + rips
// ---------------------------------------------------------------------------

std::string RomSession::library_dir() const
{
   return app_data_dir() + "\\library\\" + sanitize_filename(game_name_);
}

void RomSession::save_library()
{
   std::string dir = library_dir();
   make_dirs(dir);
   std::ostringstream out;
   out << "# value\thas_value\tkind\tfile\ttitle\tloudness tail envelope...\n";
   std::lock_guard<std::mutex> lock(songs_mutex);
   for (const auto &s : songs)
   {
      out << s.value << '\t' << (s.has_value ? 1 : 0) << '\t' << (int)s.kind << '\t'
          << file_name(s.spc_path) << '\t' << s.title << '\t' << s.print.loudness << ' ' << s.print.tail;
      for (float e : s.print.envelope)
         out << ' ' << (int)std::lround(e);
      out << '\n';
   }
   write_text(dir + "\\songs.txt", out.str());
}

void RomSession::load_library()
{
   std::string dir = library_dir();
   std::istringstream in(read_text(dir + "\\songs.txt"));
   std::string line;
   std::lock_guard<std::mutex> lock(songs_mutex);
   songs.clear();
   while (std::getline(in, line))
   {
      if (line.empty() || line[0] == '#')
         continue;
      if (line.back() == '\r')
         line.pop_back();
      std::vector<std::string> f;
      size_t start = 0;
      for (int i = 0; i < 5; i++)
      {
         size_t tab = line.find('\t', start);
         if (tab == std::string::npos)
            break;
         f.push_back(line.substr(start, tab - start));
         start = tab + 1;
      }
      if (f.size() < 5)
         continue;
      FoundSong s;
      s.value = (uint32_t)strtoul(f[0].c_str(), nullptr, 10);
      s.has_value = f[1] == "1";
      s.kind = f[2] == "1" ? SONG_JINGLE : SONG_MUSIC;
      s.spc_path = dir + "\\" + f[3];
      s.title = f[4];
      std::istringstream nums(line.substr(start));
      nums >> s.print.loudness >> s.print.tail;
      float e;
      while (nums >> e)
         s.print.envelope.push_back(e);
      if (file_exists(s.spc_path))
         songs.push_back(s);
   }
}

// ---------------------------------------------------------------------------
// Ripping
// ---------------------------------------------------------------------------

bool RomSession::write_value(uint32_t value)
{
   size_t size = 0;
   uint8_t *ram = core.memory_mut(kMemoryIds[address.memory], &size);
   if (!ram || (uint64_t)address.scan_address + address.size > size)
      return false;
   for (int i = 0; i < address.size; i++)
      ram[address.scan_address + i] = (uint8_t)(value >> (8 * i));
   return true;
}

bool RomSession::read_song_value(uint32_t &value)
{
   size_t size = 0;
   const uint8_t *ram = core.memory(kMemoryIds[address.memory], &size);
   if (!address.known || !ram || (uint64_t)address.address + address.size > size)
      return false;
   value = 0;
   for (int i = 0; i < address.size; i++)
      value |= (uint32_t)ram[address.address + i] << (8 * i);
   return true;
}

// Builds a song from the game's state; the core must be at `state` (core_mutex held).
bool RomSession::rip_state(const std::vector<uint8_t> &state, const std::string &title, uint32_t value,
      bool has_value, FoundSong &out, std::string &error)
{
   std::vector<uint8_t> current = state, spc;
   SpcState info{};
   SpcTags tags;
   tags.song = title;
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
   out.title = title;
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

bool RomSession::duplicate_locked(const SongPrint &print) const
{
   for (const auto &s : songs)
      if (same_song(s.print, print))
         return true;
   return false;
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

static std::string default_title(const RomPreset *preset, uint32_t value, SongKind kind)
{
   if (preset)
      for (const auto &ps : preset->songs)
         if (ps.value == value)
            return ps.title;
   return (kind == SONG_JINGLE ? "Jingle " : "Song ") + hex2(value);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void RomSession::start_scan(int first, int last)
{
   if (scanning_ || !open_ || !address.scan_known)
      return;
   if (worker_.joinable())
      worker_.join();
   cancel_ = false;
   scanning_ = true;
   scan_done_ = 0;
   scan_found_ = 0;
   scan_total_ = std::max(1, last - first + 1);
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

void RomSession::use_current_moment_for_scans()
{
   scan_state_ = core.save_state();
   log(scan_state_.empty() ? "this core cannot save states" : "scans now start from this moment");
}

void RomSession::scan_thread(int first, int last)
{
   auto set_message = [&](const std::string &m) {
      std::lock_guard<std::mutex> lock(message_mutex_);
      scan_message_ = m;
   };

   std::unique_lock<std::mutex> core_lock(core_mutex);
   core.set_skip_video(true);

   if (scan_state_.empty())
   {
      set_message("Starting the game...");
      core.reset();
      for (int f = 0; f < kBootFrames && !cancel_; f++)
      {
         core.run_frame(0);
         core.audio().clear();
      }
      scan_state_ = core.save_state();
   }
   if (scan_state_.empty())
   {
      set_message("This core cannot save states, so it cannot be scanned.");
      core.set_skip_video(false);
      scanning_ = false;
      return;
   }

   auto run_settle = [&]() {
      for (int f = 0; f < settle_frames; f++)
      {
         core.run_frame(0);
         core.audio().clear();
      }
   };

   // What plays without writing anything: values the game ignores sound like this.
   std::string err;
   FoundSong baseline;
   core.load_state(scan_state_);
   run_settle();
   uint32_t baseline_value = 0;
   bool follow = address.known && address.scan_address != address.address &&
         read_song_value(baseline_value);
   bool have_baseline = rip_state(core.save_state(), "", 0, false, baseline, err);

   int ignored = 0;
   for (int v = first; v <= last && !cancel_; v++)
   {
      set_message("Trying song " + hex2((uint32_t)v) + "...");
      core.load_state(scan_state_);
      if (!write_value((uint32_t)v))
      {
         set_message("The song address is outside the game's memory.");
         break;
      }
      run_settle();

      // Number the song the way Proteus will see it: by the song address.
      uint32_t key = (uint32_t)v, now = 0;
      if (follow && read_song_value(now) && now != baseline_value)
         key = now;

      FoundSong song;
      if (rip_state(core.save_state(), "", key, true, song, err))
      {
         if (have_baseline && same_song(song.print, baseline.print))
            ignored++;
         else
         {
            std::lock_guard<std::mutex> lock(songs_mutex);
            FoundSong *existing = find_song(key);
            bool dup = false;
            for (const auto &s : songs)
               if ((!existing || &s != existing) && same_song(s.print, song.print))
                  dup = true;
            if (!dup)
            {
               song.title = existing ? existing->title : default_title(preset_, song.value, song.kind);
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
   std::string summary = (cancel_ ? "Scan stopped: " : "Scan finished: ") + std::to_string((int)scan_found_) + " songs found";
   if (scan_found_ == 0)
      summary += ". Nothing new started from this address. If it only records the current song, "
                 "play the game in Advanced and rip songs as you hear them.";
   set_message(summary);
   log(summary + " (" + std::to_string(ignored) + " values changed nothing)");
   scanning_ = false;
}

// ---------------------------------------------------------------------------
// Live play
// ---------------------------------------------------------------------------

void RomSession::play_frame(uint16_t buttons)
{
   core.run_frame(buttons);
   if (!address.known)
      return;

   size_t size = 0;
   const uint8_t *ram = core.memory(kMemoryIds[address.memory], &size);
   if (!ram || (uint64_t)address.address + address.size > size)
      return;
   uint32_t v = 0;
   for (int i = 0; i < address.size; i++)
      v |= (uint32_t)ram[address.address + i] << (8 * i);

   if (!(address.latch && v == 0))
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
         bool known;
         {
            std::lock_guard<std::mutex> lock(songs_mutex);
            known = find_song(v) != nullptr;
         }
         if (!known)
         {
            pending_value_ = v;
            pending_rip_frames_ = settle_frames;
         }
         log("song " + hex2(v) + (known ? "" : " (new)"));
      }
   }

   // New songs heard while playing are ripped once they have started.
   if (pending_rip_frames_ >= 0 && --pending_rip_frames_ < 0)
   {
      FoundSong song;
      std::string err;
      std::vector<uint8_t> resume = core.save_state();
      if (rip_state(resume, "", pending_value_, true, song, err))
      {
         std::lock_guard<std::mutex> lock(songs_mutex);
         if (!find_song(pending_value_) && !duplicate_locked(song.print))
         {
            song.title = default_title(preset_, song.value, song.kind);
            add_song_locked(song);
            log("ripped " + song.title);
         }
      }
      // rip_state may step a frame or two; that is part of play, so no restore.
      save_library();
   }
}

bool RomSession::rip_now(std::string &message)
{
   std::vector<uint8_t> state = core.save_state();
   if (state.empty())
   {
      message = "This core cannot save states.";
      return false;
   }
   bool has_value = address.known && have_last_;
   uint32_t value = has_value ? last_value_ : 0;
   FoundSong song;
   std::string err;
   if (!rip_state(state, "", value, has_value, song, err))
   {
      message = err == "silent" ? "Nothing is playing right now." : "Could not rip: " + err;
      return false;
   }
   {
      std::lock_guard<std::mutex> lock(songs_mutex);
      for (const auto &s : songs)
         if (same_song(s.print, song.print))
         {
            message = "Already in the list as \"" + s.title + "\".";
            return false;
         }
      int rips = 0;
      for (const auto &s : songs)
         rips += s.has_value ? 0 : 1;
      song.title = has_value ? default_title(preset_, value, song.kind) : "Rip " + std::to_string(rips + 1);
      message = "Ripped \"" + song.title + "\".";
      add_song_locked(song);
   }
   save_library();
   return true;
}
