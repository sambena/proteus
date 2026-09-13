// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rom_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
static const char kLibraryHeader[] = "# proteus-studio library 2";

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

// The same song ripped a moment earlier or later has the same loudness and
// brightness over time, shifted. Different songs rarely match in both.
bool same_song(const SongPrint &a, const SongPrint &b)
{
   int n = (int)std::min({ a.envelope.size(), b.envelope.size(), a.brightness.size(), b.brightness.size() });
   if (n < 60)
      return false;
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
   for (int shift = -kMaxShift; shift <= kMaxShift; shift++)
      if (distance(a.envelope, b.envelope, shift) < 0.05 && distance(a.brightness, b.brightness, shift) < 0.05)
         return true;
   return false;
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

void RomSession::apply_scan_results()
{
   if (!scanning_ && scan_addr_changed_)
   {
      address = scan_addr_;
      scan_addr_changed_ = false;
   }
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
   if (!address.known && preset_ && preset_->verified)
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
   else if (preset_ && preset_->verified && preset_->latch && preset_->memory == address.memory)
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
   out << kLibraryHeader << "\n# value\thas_value\tkind\tfile\ttitle\tloudness tail count envelope... brightness...\n";
   std::lock_guard<std::mutex> lock(songs_mutex);
   for (const auto &s : songs)
   {
      out << s.value << '\t' << (s.has_value ? 1 : 0) << '\t' << (int)s.kind << '\t'
          << file_name(s.spc_path) << '\t' << s.title << '\t' << s.print.loudness << ' ' << s.print.tail
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
   bool current = text.compare(0, strlen(kLibraryHeader), kLibraryHeader) == 0;
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
      if (!file_exists(s.spc_path))
         continue;
      std::istringstream nums(line.substr(start));
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
   if (!current && !songs.empty())
      save_library();
}

// ---------------------------------------------------------------------------
// Ripping
// ---------------------------------------------------------------------------

ScanCommand scan_command(const SongAddress &a)
{
   ScanCommand cmd;
   cmd.address = a.scan_address;
   cmd.bytes = a.scan_bytes;
   cmd.offset = a.scan_offset;
   cmd.size = a.size;
   cmd.memory = a.memory;
   return cmd;
}

std::string describe_scan_command(const SongAddress &a)
{
   std::string s = "$" + hex4(a.scan_address);
   if (a.scan_bytes.empty())
      return s + " = song";
   s += " =";
   for (size_t i = 0; i < a.scan_bytes.size(); i++)
      s += (int)i == a.scan_offset ? " song" : " " + hex2(a.scan_bytes[i]).substr(2);
   return s;
}

bool RomSession::write_command(const ScanCommand &cmd, uint32_t value)
{
   size_t size = 0;
   uint8_t *ram = core.memory_mut(kMemoryIds[cmd.memory], &size);
   size_t length = cmd.bytes.empty() ? (size_t)cmd.size : cmd.bytes.size();
   if (!ram || (uint64_t)cmd.address + length > size)
      return false;
   if (cmd.bytes.empty())
   {
      for (int i = 0; i < cmd.size; i++)
         ram[cmd.address + i] = (uint8_t)(value >> (8 * i));
      return true;
   }
   memcpy(ram + cmd.address, cmd.bytes.data(), cmd.bytes.size());
   ram[cmd.address + cmd.offset] = (uint8_t)value;
   return true;
}

bool RomSession::read_song_value(const SongAddress &a, uint32_t &value)
{
   size_t size = 0;
   const uint8_t *ram = core.memory(kMemoryIds[a.memory], &size);
   if (!a.known || !ram || (uint64_t)a.address + a.size > size)
      return false;
   value = 0;
   for (int i = 0; i < a.size; i++)
      value |= (uint32_t)ram[a.address + i] << (8 * i);
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
   if (scanning_ || !open_)
      return;
   if (worker_.joinable())
      worker_.join();
   cancel_ = false;
   scanning_ = true;
   scan_done_ = 0;
   scan_found_ = 0;
   scan_addr_ = address;
   scan_addr_changed_ = false;
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

void RomSession::set_scan_message(const std::string &message)
{
   std::lock_guard<std::mutex> lock(message_mutex_);
   scan_message_ = message;
}

void RomSession::run_settle()
{
   for (int f = 0; f < settle_frames; f++)
   {
      core.run_frame(0);
      core.audio().clear();
   }
}

// Plays each trial value through a command and counts the different songs it starts.
int RomSession::command_score(const ScanCommand &cmd, const SongPrint *baseline, int *music_out)
{
   static const uint32_t kTrials[] = { 1, 2, 3, 5, 8, 13 };
   std::vector<SongPrint> heard;
   int score = 0, music = 0;
   std::string err;
   for (uint32_t v : kTrials)
   {
      if (cancel_)
         break;
      core.load_state(scan_state_);
      if (!write_command(cmd, v))
         return 0;
      run_settle();
      FoundSong song;
      if (!rip_state(core.save_state(), "", v, true, song, err))
         continue;
      if (baseline && same_song(song.print, *baseline))
         continue;
      bool dup = false;
      for (const auto &p : heard)
         dup = dup || same_song(p, song.print);
      if (dup)
         continue;
      heard.push_back(song.print);
      score += song.kind == SONG_MUSIC ? 2 : 1;
      music += song.kind == SONG_MUSIC;
   }
   if (music_out)
      *music_out = music;
   return score;
}

// Watches the game start up for commands sent to the sound CPU, finds the RAM
// bytes they came from, and picks the command and byte that start the most songs.
bool RomSession::find_command(const SongPrint *baseline)
{
   set_scan_message("Watching how the game starts its music...");
   std::vector<uint8_t> resume = core.save_state();
   core.reset();

   struct Candidate
   {
      int votes = 0;
      std::map<uint32_t, int> commands;   // the 4 port bytes, and how often they were sent
   };
   std::map<uint32_t, Candidate> candidates;
   size_t ram_size = 0;
   core.memory(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
   const size_t search = std::min<size_t>(ram_size, 0x2000);
   std::vector<uint8_t> prev_ram;
   uint8_t prev_ports[4] = { 0 };

   const int kWatchFrames = 2400;
   scan_total_ = kWatchFrames;
   scan_done_ = 0;
   for (int f = 0; f < kWatchFrames && !cancel_; f++, scan_done_++)
   {
      // Tap Start now and then to get past title screens to more music.
      uint16_t buttons = (f > 900 && f % 300 < 6) ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
      core.run_frame(buttons);
      core.audio().clear();
      uint8_t ports[4];
      if (!spc_snes9x_ports(core.save_state(), ports))
         break;
      size_t size = 0;
      const uint8_t *ram = core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      if (!ram)
         break;
      bool changed = memcmp(ports, prev_ports, 4) != 0;
      bool nonzero = ports[0] | ports[1] | ports[2] | ports[3];
      if (changed && nonzero && prev_ram.size() >= search)
      {
         uint32_t cmd = ports[0] | ports[1] << 8 | ports[2] << 16 | (uint32_t)ports[3] << 24;
         for (uint32_t x = 0; x + 4 <= search; x++)
            if (!memcmp(&prev_ram[x], ports, 4) || !memcmp(&ram[x], ports, 4))
            {
               Candidate &c = candidates[x];
               c.votes++;
               c.commands[cmd]++;
            }
      }
      memcpy(prev_ports, ports, 4);
      prev_ram.assign(ram, ram + search);
   }
   core.load_state(resume);
   if (cancel_)
      return false;

   // Command blocks carry many different commands; bytes that merely hold the same
   // value as a port for a moment do not.
   std::vector<std::pair<int, uint32_t>> ranked;
   for (auto &c : candidates)
      ranked.push_back({ (int)c.second.commands.size() * 100 + c.second.votes, c.first });
   std::sort(ranked.rbegin(), ranked.rend());
   if (ranked.size() > 4)
      ranked.resize(4);
   if (ranked.empty())
   {
      log("no RAM block copied to the sound CPU was seen while the game started");
      return false;
   }

   std::vector<ScanCommand> trials;
   for (auto &r : ranked)
   {
      std::vector<std::pair<int, uint32_t>> cmds;
      for (auto &c : candidates[r.second].commands)
         cmds.push_back({ c.second, c.first });
      std::sort(cmds.rbegin(), cmds.rend());
      for (size_t i = 0; i < cmds.size() && i < 3; i++)
         for (int offset = 0; offset < 4; offset++)
         {
            ScanCommand cmd;
            cmd.address = r.second;
            cmd.offset = offset;
            for (int b = 0; b < 4; b++)
               cmd.bytes.push_back((uint8_t)(cmds[i].second >> (8 * b)));
            trials.push_back(cmd);
         }
   }

   ScanCommand best;
   int best_score = 0;
   scan_total_ = (int)trials.size();
   scan_done_ = 0;
   for (const auto &cmd : trials)
   {
      if (cancel_)
         break;
      set_scan_message("Trying command at $" + hex4(cmd.address) + "...");
      int score = command_score(cmd, baseline, nullptr);
      if (score > best_score)
      {
         best_score = score;
         best = cmd;
      }
      scan_done_++;
   }
   if (best_score < 3)
   {
      log("no command started more than one song");
      return false;
   }
   scan_addr_.scan_known = true;
   scan_addr_.scan_address = best.address;
   scan_addr_.scan_bytes = best.bytes;
   scan_addr_.scan_offset = best.offset;
   scan_addr_changed_ = true;
   log("found the music command: " + describe_scan_command(scan_addr_));

   // Without a trustworthy song address, follow the song byte of the command:
   // it holds each song number for a moment when music starts.
   if (!scan_addr_.known || scan_addr_.source == "preset")
   {
      scan_addr_.known = true;
      scan_addr_.source = "detected music command";
      scan_addr_.memory = 0;
      scan_addr_.address = best.address + best.offset;
      scan_addr_.size = 1;
      scan_addr_.latch = true;
      scan_addr_.debounce = 1;
      log("song address set to $" + hex4(scan_addr_.address) + " (command register)");
   }
   return true;
}

void RomSession::scan_thread(int first, int last)
{
   std::unique_lock<std::mutex> core_lock(core_mutex);
   core.set_skip_video(true);

   if (scan_state_.empty())
   {
      set_scan_message("Starting the game...");
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
      set_scan_message("This core cannot save states, so it cannot be scanned.");
      core.set_skip_video(false);
      scanning_ = false;
      return;
   }

   // What plays without writing anything: values the game ignores sound like this.
   std::string err;
   FoundSong baseline;
   core.load_state(scan_state_);
   run_settle();
   uint32_t baseline_value = 0;
   bool have_baseline = rip_state(core.save_state(), "", 0, false, baseline, err);
   const SongPrint *base_print = have_baseline ? &baseline.print : nullptr;

   // Check the command we have starts songs; otherwise look for the game's own.
   bool usable = false;
   if (scan_addr_.scan_known)
   {
      set_scan_message("Checking the music command...");
      usable = command_score(scan_command(scan_addr_), base_print, nullptr) >= 3;
      if (!usable)
         log("the music command " + describe_scan_command(scan_addr_) + " starts no songs");
   }
   if (!usable && !cancel_)
      usable = find_command(base_print);
   if (!usable)
   {
      core.load_state(scan_state_);
      core.set_skip_video(false);
      core_lock.unlock();
      set_scan_message(cancel_ ? "Scan stopped." :
            "Could not find how this game starts songs. Play it in Advanced and rip songs as you hear them.");
      scanning_ = false;
      return;
   }

   core.load_state(scan_state_);
   run_settle();
   bool follow = scan_addr_.known && read_song_value(scan_addr_, baseline_value);
   ScanCommand cmd = scan_command(scan_addr_);
   if (follow && cmd.bytes.empty() && cmd.address == scan_addr_.address)
      follow = false;

   scan_total_ = std::max(1, last - first + 1);
   scan_done_ = 0;
   int ignored = 0;
   for (int v = first; v <= last && !cancel_; v++)
   {
      set_scan_message("Trying song " + hex2((uint32_t)v) + "...");
      core.load_state(scan_state_);
      if (!write_command(cmd, (uint32_t)v))
      {
         set_scan_message("The music command is outside the game's memory.");
         break;
      }
      run_settle();

      // Number the song the way Proteus will see it: by the song address.
      uint32_t key = (uint32_t)v, now = 0;
      if (follow && read_song_value(scan_addr_, now) && now != baseline_value)
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
   std::string summary = (cancel_ ? "Scan stopped: " : "Scan finished: ") + std::to_string((int)scan_found_) + " new songs";
   if (scan_found_ == 0)
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

   // Any command the game sends the sound CPU may start a song: once the ports
   // have been quiet long enough for it to start, rip what plays.
   uint8_t ports[4];
   if (spc_snes9x_ports(state, ports) && memcmp(ports, last_ports_, 4) != 0)
   {
      memcpy(last_ports_, ports, 4);
      pending_rip_frames_ = settle_frames;
   }

   if (address.known)
   {
      size_t size = 0;
      const uint8_t *ram = core.memory(kMemoryIds[address.memory], &size);
      if (ram && (uint64_t)address.address + address.size <= size)
      {
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
               pending_rip_frames_ = settle_frames;
               log("song " + hex2(v));
            }
         }
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
      // A different song under a number we already have: the address did not
      // follow this change (or a jingle played over it), so keep it unnumbered.
      if (has_value && find_song(value))
      {
         if (automatic && song.kind == SONG_JINGLE)
            return false;
         song.has_value = false;
      }
      int rips = 0;
      for (const auto &s : songs)
         rips += s.has_value ? 0 : 1;
      song.title = song.has_value ? default_title(preset_, value, song.kind) : "Rip " + std::to_string(rips + 1);
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
