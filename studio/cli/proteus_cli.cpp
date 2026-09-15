// SPDX-License-Identifier: LGPL-2.1-or-later
// proteus-cli: Proteus Studio's song work without the window, for scripts and testing.
//
//   proteus-cli table <rom> <spc folder>
//   proteus-cli match <spc folder> <rip.spc> [before.spc]
//   proteus-cli scan <rom> --core <snes9x_libretro.dll> [--system <dir>] [--refs <spc folder>] [--first N] [--last N] [--keep-rips dir]
//   proteus-cli list <rom> <snes9x core> [app dir]
//   proteus-cli import <folder, archive or .spc> <folder>
//   proteus-cli download "<game name>" <folder> [zophar|snesmusic]
//   proteus-cli tas <core> <rom> [bizhawk | movies | download N | play <movie> <spc folder> [speed %]]
//   proteus-cli folder <core> <rom folder> [--movies] [--rescan] [--nes-core <fceumm_libretro.dll>]
//   proteus-cli dumps <spc folder> <dump folder>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"
#include "reference.h"
#include "folder_scan.h"
#include "movie_learner.h"
#include "nsf_init.h"
#include "rom_session.h"
#include "snes_rom.h"
#include "song_notes.h"
#include "spc_rip.h"
#include "zip_read.h"

static bool load_rom(const std::string &path, SnesRom &rom)
{
   std::vector<uint8_t> data;
   if (!read_file_bytes(path, data))
      return false;
   if (is_zip(data))
   {
      std::vector<uint8_t> inner;
      std::string err;
      zip_read(data, [](const std::string &n) { std::string e = lower_ext(n); return e == "sfc" || e == "smc" || e == "swc" || e == "fig"; },
            [&](const std::string &, std::vector<uint8_t> &d) { inner.swap(d); return false; }, err);
      data.swap(inner);
   }
   return rom.load(data);
}

static int cmd_table(const std::string &rom_path, const std::string &dir)
{
   SnesRom rom;
   if (!load_rom(rom_path, rom))
   {
      fprintf(stderr, "cannot load %s\n", rom_path.c_str());
      return 1;
   }
   ReferenceSet refs;
   std::string err;
   refs.load(dir, err);
   printf("%zu references\n", refs.size());
   auto t0 = std::chrono::steady_clock::now();
   SongTable t = refs.find_song_table(rom);
   double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   if (!t.found)
   {
      printf("no song table (%.2fs)\n", secs);
      return 0;
   }
   printf("song table at ROM 0x%zX (CPU $%06X), %d-byte pointers, %zu entries, %d references (%.2fs)\n",
         t.rom_offset, t.cpu_address, t.width, t.entries.size(), t.matched, secs);
   for (size_t i = 0; i < t.entries.size(); i++)
      printf("  %02zX  %s\n", i, t.entries[i] >= 0 ? refs.song(t.entries[i]).title.c_str() : "-");
   return 0;
}

static int cmd_match(const std::string &dir, const std::string &rip_path, const char *before_path)
{
   ReferenceSet refs;
   std::string err;
   refs.load(dir, err);
   std::vector<uint8_t> rip, before;
   if (!read_file_bytes(rip_path, rip) || (before_path && !read_file_bytes(before_path, before)))
      return 1;
   ReferenceSet::Match m = refs.match_spc(rip, before_path ? &before : nullptr);
   printf("%s  score %.2f  next %.2f\n", m.index >= 0 ? refs.song(m.index).title.c_str() : "(no match)", m.score, m.second);
   return 0;
}

static int cmd_scan(int argc, char **argv)
{
   std::string rom_path = argv[2], core, system, refs, keep;
   int first = -1, last = -1;
   for (int i = 3; i + 1 < argc; i += 2)
   {
      std::string k = argv[i];
      if (k == "--core") core = argv[i + 1];
      else if (k == "--system") system = argv[i + 1];
      else if (k == "--refs") refs = argv[i + 1];
      else if (k == "--first") first = (int)strtol(argv[i + 1], nullptr, 0);
      else if (k == "--last") last = (int)strtol(argv[i + 1], nullptr, 0);
      else if (k == "--keep-rips") keep = argv[i + 1];
   }
   if (core.empty())
   {
      fprintf(stderr, "--core is required\n");
      return 2;
   }
   RomSession s;
   s.set_app_dir(app_data_dir() + "\\cli");
   std::string err;
   if (!s.open(rom_path, core, system.empty() ? dir_of(core) : system, err))
   {
      fprintf(stderr, "open: %s\n", err.c_str());
      return 1;
   }
   s.clear_library();
   auto flush = [&] { for (auto &l : s.take_log()) printf("  | %s\n", l.c_str()); };
   auto wait_refs = [&] {
      while (s.loading_references())
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
      s.apply_reference_results();
      flush();
   };
   wait_refs();
   if (!refs.empty())
   {
      s.remove_references();
      int n = s.import_references(refs, err);
      printf("imported %d references%s\n", n, err.empty() ? "" : (" (" + err + ")").c_str());
      wait_refs();
   }
   s.keep_rips_dir = keep;
   s.start_scan(first, last);
   std::string last_msg;
   while (s.scanning())
   {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::string m = s.scan_message();
      if (m != last_msg)
         printf("  %s\n", (last_msg = m).c_str());
      flush();
   }
   // The game database is left alone; the log shows what the scan settled on.
   flush();
   printf("%s\n", s.scan_message().c_str());
   std::lock_guard<std::mutex> lock(s.songs_mutex);
   for (const auto &song : s.songs)
      printf("  %s  %-40s %s%s\n", song.has_value ? ("0x" + std::string(song.value < 16 ? "0" : "") + [&] { char b[8]; snprintf(b, 8, "%X", song.value); return std::string(b); }()).c_str() : "  --",
            song.title.c_str(), song.kind == SONG_JINGLE ? "jingle " : "", song.reference.empty() ? "" : "[reference]");
   return 0;
}

int main(int argc, char **argv)
{
   std::string cmd = argc > 1 ? argv[1] : "";
   if (cmd == "table" && argc == 4)
      return cmd_table(argv[2], argv[3]);
   if (cmd == "match" && argc >= 4)
      return cmd_match(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
   if (cmd == "scan" && argc >= 3)
      return cmd_scan(argc, argv);
   if (cmd == "list" && argc >= 4)
   {
      // proteus-cli list <rom> <snes9x core> [app dir]: the song list as Studio shows it on opening.
      RomSession s;
      s.set_app_dir(argc > 4 ? argv[4] : app_data_dir() + "\\cli");
      std::string err;
      if (!s.open(argv[2], argv[3], dir_of(argv[3]), err))
      {
         fprintf(stderr, "open: %s\n", err.c_str());
         return 1;
      }
      while (s.loading_references())
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
      s.apply_reference_results();
      for (auto &l : s.take_log())
         printf("  | %s\n", l.c_str());
      std::lock_guard<std::mutex> lock(s.songs_mutex);
      for (const auto &song : s.songs)
         printf("  %02X  %-40s %s\n", song.value, song.title.c_str(), song.reference.empty() ? "no match" : "");
      return 0;
   }
   if (cmd == "play" && argc >= 6)
   {
      // proteus-cli play <core> <rom> <system dir> <seconds> [out.wav]: runs a core (the Proteus
      // wrapper, say) as RetroArch would, tapping Start now and then, and prints its log.
      CoreHost core;
      std::string err, save = app_data_dir() + "\\cli\\saves";
      make_dirs(save);
      if (!core.load(argv[2], argv[3], argv[4], save, err))
      {
         fprintf(stderr, "load: %s\n", err.c_str());
         return 1;
      }
      // PROTEUS_OPTIONS=key=value,key=value: core options, as set in RetroArch's Quick Menu.
      if (const char *o = getenv("PROTEUS_OPTIONS"))
      {
         std::string opts = o;
         for (size_t p = 0; p < opts.size();)
         {
            size_t comma = opts.find(',', p), eq = opts.find('=', p);
            if (comma == std::string::npos)
               comma = opts.size();
            if (eq != std::string::npos && eq < comma)
               core.set_option(opts.substr(p, eq - p), opts.substr(eq + 1, comma - eq - 1));
            p = comma + 1;
         }
      }
      int frames = (int)(atof(argv[5]) * 60);
      // PROTEUS_WATCH=1DFB,0DDA: print work RAM bytes whenever they change.
      std::vector<uint32_t> watch;
      if (const char *w = getenv("PROTEUS_WATCH"))
         for (const char *p = w; *p;)
         {
            char *end;
            watch.push_back((uint32_t)strtoul(p, &end, 16));
            p = *end ? end + 1 : end;
         }
      std::vector<int> last(watch.size(), -1);
      // PROTEUS_POKE=600:FB=04,900:FB=08: write work RAM bytes at those frames (after the frame runs).
      struct Poke { int frame; uint32_t address; uint8_t value; };
      std::vector<Poke> pokes;
      if (const char *p = getenv("PROTEUS_POKE"))
         for (const char *s = p; *s;)
         {
            char *end;
            Poke k;
            k.frame = (int)strtol(s, &end, 10);
            k.address = (uint32_t)strtoul(end + 1, &end, 16);
            k.value = (uint8_t)strtoul(end + 1, &end, 16);
            pokes.push_back(k);
            s = *end ? end + 1 : end;
         }
      // PROTEUS_START=240,600: tap Start at those frames only, instead of every 3 seconds.
      std::vector<int> taps;
      if (const char *t = getenv("PROTEUS_START"))
         for (const char *s = t; *s;)
         {
            char *end;
            taps.push_back((int)strtol(s, &end, 10));
            s = *end ? end + 1 : end;
         }
      // PROTEUS_PRESS=600:down,660:a,720:right:120: press buttons at those frames (6 frames each, or
      // as many as given), with no Start taps.
      struct Press { int at, button, frames; };
      std::vector<Press> presses;
      if (const char *p = getenv("PROTEUS_PRESS"))
      {
         static const std::pair<const char *, int> kNames[] = { { "b", RETRO_DEVICE_ID_JOYPAD_B }, { "y", RETRO_DEVICE_ID_JOYPAD_Y },
            { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START }, { "up", RETRO_DEVICE_ID_JOYPAD_UP },
            { "down", RETRO_DEVICE_ID_JOYPAD_DOWN }, { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
            { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "x", RETRO_DEVICE_ID_JOYPAD_X } };
         for (const char *s = p; *s;)
         {
            char *end;
            int at = (int)strtol(s, &end, 10);
            std::string name;
            int hold = 6;
            for (s = *end == ':' ? end + 1 : end; *s && *s != ',' && *s != ':'; s++)
               name += *s;
            if (*s == ':')
               hold = (int)strtol(s + 1, (char **)&s, 10);
            for (const auto &n : kNames)
               if (name == n.first)
                  presses.push_back({ at, n.second, hold });
            if (*s)
               s++;
         }
      }
      std::vector<int16_t> all;
      for (int f = 0; f < frames; f++)
      {
         bool tap = !presses.empty() ? false : taps.empty() ? f > 240 && f % 180 < 6
                                 : std::any_of(taps.begin(), taps.end(), [&](int at) { return f >= at && f < at + 6; });
         uint16_t buttons = tap ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
         for (const auto &p : presses)
            if (f >= p.at && f < p.at + p.frames)
               buttons |= (uint16_t)(1 << p.button);
         core.run_frame(buttons);
         size_t ram_size = 0;
         uint8_t *ram = core.memory_mut(RETRO_MEMORY_SYSTEM_RAM, &ram_size);
         for (const auto &k : pokes)
            if (k.frame == f && ram && k.address < ram_size)
               ram[k.address] = k.value;
         for (size_t i = 0; i < watch.size() && ram; i++)
            if (watch[i] < ram_size && ram[watch[i]] != last[i])
            {
               printf("  [%5.1fs] $%04X = %02X\n", f / 60.0, watch[i], ram[watch[i]]);
               last[i] = ram[watch[i]];
            }
         if (argc > 6)
            all.insert(all.end(), core.audio().begin(), core.audio().end());
         core.audio().clear();
         for (auto &l : core.log())
            printf("  [%5.1fs] %s", f / 60.0, l.c_str());
         core.log().clear();
      }
      if (argc > 6)
      {
         // 16-bit stereo WAV
         uint32_t rate = (uint32_t)core.sample_rate(), bytes = (uint32_t)(all.size() * 2);
         std::string h = "RIFF    WAVEfmt                     data    ";
         auto put = [&](size_t at, uint32_t v, int n) { for (int i = 0; i < n; i++) h[at + i] = (char)(v >> (8 * i)); };
         put(4, 36 + bytes, 4); put(16, 16, 4); put(20, 1, 2); put(22, 2, 2); put(24, rate, 4);
         put(28, rate * 4, 4); put(32, 4, 2); put(34, 16, 2); put(40, bytes, 4);
         write_text(argv[6], h + std::string((const char*)all.data(), bytes));
         printf("wrote %s (%u Hz)\n", argv[6], rate);
      }
      return 0;
   }
   if (cmd == "learn" && argc >= 6)
   {
      // proteus-cli learn <core> <rom> <spc folder> <seconds>: plays the game in Studio's Play & rip
      // with random presses, learning the song address from the reference songs heard.
      RomSession s;
      s.set_app_dir(app_data_dir() + "\\cli");
      std::string err;
      if (!s.open(argv[3], argv[2], dir_of(argv[2]), err))
      {
         fprintf(stderr, "open: %s\n", err.c_str());
         return 1;
      }
      s.clear_library();
      auto flush = [&] { for (auto &l : s.take_log()) printf("  | %s\n", l.c_str()); };
      auto wait_refs = [&] {
         while (s.loading_references())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
         s.apply_reference_results();
         flush();
      };
      wait_refs();
      s.remove_references();
      s.import_references(argv[4], err);
      wait_refs();
      s.address = SongAddress();
      int frames = (int)(atof(argv[5]) * 60);
      uint32_t seed = 7;
      uint16_t held = 0;
      int hold = 0;
      std::lock_guard<std::mutex> lock(s.core_mutex);
      s.core.set_skip_video(true);
      for (int f = 0; f < frames; f++)
      {
         if (--hold <= 0)
         {
            seed = seed * 1103515245 + 12345;
            int pick = (seed >> 16) % 10;
            static const int kButtons[] = { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
               RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
               RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X };
            held = (uint16_t)(1 << kButtons[pick]);
            hold = 6 + (int)((seed >> 8) % 60);
         }
         s.play_frame(held);
         s.core.audio().clear();
         if (f % 600 == 599)
            printf("[%3ds] %s\n", f / 60, s.learning_status().c_str());
         flush();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      s.play_frame(0);
      flush();
      printf("song address: %s (%s)\n", s.address.known ? describe_song_address(s.address).c_str() : "none", s.address_source.c_str());
      return 0;
   }
   if (cmd == "folder" && argc >= 4)
   {
      // proteus-cli folder <core> <rom folder> [--movies] [--rescan]: Scan folder, as in Studio.
      FolderScan::Options o;
      o.core_path = argv[2];
      o.system_dir = dir_of(argv[2]);
      o.folder = argv[3];
      o.app_dir = app_data_dir() + "\\cli";
      for (int i = 4; i < argc; i++)
      {
         o.use_movies = o.use_movies || std::string(argv[i]) == "--movies";
         o.rescan = o.rescan || std::string(argv[i]) == "--rescan";
         if (std::string(argv[i]) == "--nes-core" && i + 1 < argc)
            o.nes_core_path = argv[++i];
      }
      FolderScan scan;
      scan.start(o);
      std::string last;
      size_t shown = 0;
      while (true)
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(500));
         std::vector<FolderScanRow> rows = scan.rows();
         for (; shown < rows.size(); shown++)
         {
            const FolderScanRow &r = rows[shown];
            printf("%-6s %-40s refs %3d table %d songs %3d named %3d %-10s %s | %s\n", r.verdict.c_str(), r.game.c_str(), r.references,
                  r.table ? 1 : 0, r.songs, r.named, r.address.c_str(), r.how.c_str(), r.note.c_str());
         }
         if (!scan.running())
            break;
      }
      printf("%s\nreport: %s\n", scan.message().c_str(), scan.report_path().c_str());
      return 0;
   }
   if (cmd == "tas" && argc >= 4)
   {
      // proteus-cli tas <core> <rom> [bizhawk|movies|download N|play <movie> <spc folder> [speed %]]:
      // the TAS movie tools of Advanced > TAS movie.
      RomSession s;
      s.set_app_dir(app_data_dir() + "\\cli");
      std::string err, what = argc > 4 ? argv[4] : "movies";
      if (!s.open(argv[3], argv[2], dir_of(argv[2]), err))
      {
         fprintf(stderr, "open: %s\n", err.c_str());
         return 1;
      }
      auto flush = [&] { for (auto &l : s.take_log()) printf("  | %s\n", l.c_str()); };
      auto wait_tool = [&] {
         std::string last;
         while (s.tool_busy())
         {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::string m = s.tool_message();
            if (m != last)
               printf("  %s\n", (last = m).c_str());
         }
         printf("%s\n", s.tool_message().c_str());
         flush();
      };
      if (what == "bizhawk")
      {
         s.install_bizhawk();
         wait_tool();
         return bizhawk_installed(s.app_dir()) ? 0 : 1;
      }
      if (what == "movies" || what == "download")
      {
         s.find_movies();
         wait_tool();
         std::vector<TasPublication> list = s.movie_list();
         for (size_t i = 0; i < list.size(); i++)
            printf("  %2zu  %-4s %-9s %-16s %.2f  %s\n", i, list[i].playable() ? "ok" : "-", list[i].duration().c_str(),
                  list[i].emulator.c_str(), list[i].similarity, list[i].title.c_str());
         if (what == "download" && argc > 5 && (size_t)atoi(argv[5]) < list.size())
         {
            s.download_movie(list[atoi(argv[5])]);
            wait_tool();
         }
         for (const auto &m : s.downloaded_movies())
            printf("  downloaded: %s\n", m.c_str());
         return 0;
      }
      if (what == "play" && argc >= 7)
      {
         auto wait_refs = [&] {
            while (s.loading_references())
               std::this_thread::sleep_for(std::chrono::milliseconds(50));
            s.apply_reference_results();
            flush();
         };
         wait_refs();
         s.clear_library();
         s.remove_references();
         s.import_references(argv[6], err);
         wait_refs();
         s.address = SongAddress();
         s.start_movie(argv[5], argc > 7 ? atoi(argv[7]) : 6400, getenv("PROTEUS_SHOW_BIZHAWK") != nullptr);
         std::string last;
         auto t0 = std::chrono::steady_clock::now();
         while (s.scanning())
         {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::string m = s.scan_message();
            int secs = (int)std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (m != last && secs % 30 == 0)
               printf("  [%4ds] %s\n", secs, (last = m).c_str());
            flush();
         }
         flush();
         printf("%s\n", s.scan_message().c_str());
         std::lock_guard<std::mutex> lock(s.songs_mutex);
         for (const auto &song : s.songs)
            printf("  %s  %s\n", song.has_value ? ("0x" + std::string(song.value < 16 ? "0" : "") + [&] { char b[8]; snprintf(b, 8, "%X", song.value); return std::string(b); }()).c_str() : "  --", song.title.c_str());
         return 0;
      }
      fprintf(stderr, "unknown tas command\n");
      return 2;
   }
   if (cmd == "dumps" && argc >= 4)
   {
      // proteus-cli dumps <spc folder> <dump folder>: learns the song address from RAM dumps
      // (64 KB sound CPU, then game RAM) saved while BizHawk played a movie.
      ReferenceSet refs;
      std::string err;
      refs.load(argv[2], err);
      MovieLearner learner(refs);
      std::vector<std::string> files = list_files(argv[3]);
      std::sort(files.begin(), files.end());
      int last = -2;
      for (const auto &name : files)
      {
         if (lower_ext(name) != "bin")
            continue;
         std::vector<uint8_t> d;
         if (!read_file_bytes(std::string(argv[3]) + "\\" + name, d) || d.size() <= 0x10000)
            continue;
         int r = learner.add(d.data(), d.data() + 0x10000, d.size() - 0x10000);
         if (r != last)
         {
            printf("  %s  %s\n", name.c_str(), r >= 0 ? refs.song(r).title.c_str() : "-");
            last = r;
         }
      }
      MovieLearner::Result res = learner.result();
      printf("%d moments, %zu songs heard, %d settled, %d bytes follow the music\n", learner.moments(), learner.heard().size(), res.songs, res.candidates);
      for (auto &t : res.top)
      {
         printf("  $%05X tells %d songs apart\n", t.first, t.second);
         if (&t - &res.top[0] < 2)
            printf("%s", learner.describe(t.first).c_str());
      }
      if (res.found)
      {
         printf("song address $%04X", res.address);
         for (uint32_t t : res.ties)
            printf(" (ties $%04X)", t);
         printf("\n");
         for (const auto &v : res.values)
            printf("  %02X  %s\n", v.second, refs.song(v.first).title.c_str());
      }
      return 0;
   }
   if (cmd == "trace" && argc >= 6)
   {
      // proteus-cli trace <core> <rom> <spc folder> <seconds>: plays the game tapping Start and
      // prints each command sent to the sound CPU, and which reference plays every 2 seconds.
      CoreHost core;
      std::string err, save = app_data_dir() + "\\cli\\saves";
      make_dirs(save);
      if (!core.load(argv[2], argv[3], dir_of(argv[2]), save, err))
      {
         fprintf(stderr, "load: %s\n", err.c_str());
         return 1;
      }
      for (int f = 0; f < 60; f++)
         core.run_frame(0);
      std::vector<uint8_t> st = core.save_state();
      spc_calibrate_snes9x(st, [&](const std::vector<uint8_t> &x) { core.load_state(x); return core.save_state(); });
      core.reset();
      core.set_skip_video(true);
      ReferenceSet refs;
      refs.load(argv[4], err);
      std::vector<SongNotes> notes(refs.size());
      for (size_t i = 0; i < refs.size(); i++)
         music_notes(refs.song(i).data, refs.song(i).track, notes[i], err);
      uint8_t last[4] = { 0, 0, 0, 0 };
      std::string playing;
      int frames = (int)(atof(argv[5]) * 60);
      for (int f = 0; f < frames; f++)
      {
         uint16_t buttons = (f > 240 && f % 180 < 6) ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
         core.run_frame(buttons);
         core.audio().clear();
         std::vector<uint8_t> state = core.save_state();
         uint8_t ports[4];
         if (spc_snes9x_ports(state, ports) && memcmp(ports, last, 4))
         {
            printf("  [%5.1fs] ports %02X %02X %02X %02X\n", f / 60.0, ports[0], ports[1], ports[2], ports[3]);
            memcpy(last, ports, 4);
         }
         if (f % 120 == 119)
         {
            std::vector<uint8_t> spc;
            SongNotes n;
            std::string name = "(silence)";
            if (spc_from_snes9x_state(state, SpcTags(), spc, nullptr, err) && spc_notes(spc, n, err))
            {
               double best = 1;
               for (size_t i = 0; i < refs.size(); i++)
               {
                  double d = notes_distance(n, notes[i]);
                  if (d < best)
                  {
                     best = d;
                     name = refs.song(i).title;
                  }
               }
               char buf[32];
               snprintf(buf, sizeof(buf), " (%.3f)", best);
               name += buf;
            }
            if (name != playing)
               printf("  [%5.1fs] playing: %s\n", f / 60.0, (playing = name).c_str());
         }
      }
      return 0;
   }
   if (cmd == "nsfinit" && argc >= 3)
   {
      // proteus-cli nsfinit <file.nsf>: the RAM the .nsf's init writes differently from song to song.
      std::vector<uint8_t> data;
      std::string err;
      if (!read_file_bytes(argv[2], data))
         return 1;
      std::vector<NsfRequest> requests = nsf_song_requests(data, err);
      if (!err.empty())
         fprintf(stderr, "%s\n", err.c_str());
      for (const auto &r : requests)
      {
         printf("$%04X  %zu songs:", r.address, r.song_values.size());
         for (const auto &v : r.song_values)
            printf(" %d=%02X", v.first + 1, v.second);
         printf("\n");
      }
      return 0;
   }
   if (cmd == "notes" && argc >= 3)
   {
      // proteus-cli notes <song file> [track from 1]: the song's mean strength per pitch class.
      std::vector<uint8_t> data;
      SongNotes n;
      std::string err;
      int track = argc > 3 ? atoi(argv[3]) - 1 : 0;
      if (!read_file_bytes(argv[2], data) || !music_notes(data, track, n, err))
      {
         fprintf(stderr, "%s\n", err.c_str());
         return 1;
      }
      static const char *names[] = { "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#" };
      double mean[12] = { 0 };
      for (int f = 0; f < n.frames; f++)
         for (int k = 0; k < 12; k++)
            mean[k] += n.chroma[12 * f + k] / n.frames;
      for (int k = 0; k < 12; k++)
         printf("%s %.2f  ", names[k], mean[k]);
      printf("\n");
      return 0;
   }
   if (cmd == "sound" && argc >= 4)
   {
      // proteus-cli sound <spc folder> <rip.spc>...: references ranked by how the rip sounds.
      ReferenceSet refs;
      std::string err;
      refs.load(argv[2], err);
      std::vector<SongNotes> notes(refs.size());
      for (size_t i = 0; i < refs.size(); i++)
         music_notes(refs.song(i).data, refs.song(i).track, notes[i], err);
      for (int a = 3; a < argc; a++)
      {
         std::vector<uint8_t> rip;
         SongNotes n;
         if (!read_file_bytes(argv[a], rip) || !spc_notes(rip, n, err))
            continue;
         std::vector<std::pair<double, size_t>> ranked;
         for (size_t i = 0; i < refs.size(); i++)
            ranked.push_back({ notes_distance(n, notes[i]), i });
         std::sort(ranked.begin(), ranked.end());
         printf("%s", file_name(argv[a]).c_str());
         size_t top = getenv("PROTEUS_TOP") ? (size_t)atoi(getenv("PROTEUS_TOP")) : 3;
         for (size_t k = 0; k < top && k < ranked.size(); k++)
            printf("\t%s\t%.3f", refs.song(ranked[k].second).title.c_str(), ranked[k].first);
         printf("\n");
      }
      return 0;
   }
   if (cmd == "import" && argc == 4)
   {
      std::string err;
      int n = import_reference_songs(argv[2], argv[3], err);
      printf("%d songs%s\n", n, err.empty() ? "" : (": " + err).c_str());
      return n > 0 ? 0 : 1;
   }
   if (cmd == "download" && argc >= 4)
   {
      std::string err, from = argc > 4 ? argv[4] : "";
      int sources = from == "zophar" ? REFERENCES_ZOPHAR : from == "snesmusic" ? REFERENCES_SNESMUSIC :
                    from == "nes" ? REFERENCES_ZOPHAR_NES : REFERENCES_ZOPHAR | REFERENCES_SNESMUSIC;
      int n = download_reference_songs({ argv[2] }, argv[3], [](const std::string &m) { printf("  %s\n", m.c_str()); }, err, sources);
      printf("%d songs%s\n", n, err.empty() ? "" : (": " + err).c_str());
      return n > 0 ? 0 : 1;
   }
   fprintf(stderr, "usage:\n"
         "  proteus-cli table <rom> <spc folder>\n"
         "  proteus-cli match <spc folder> <rip.spc> [before.spc]\n"
         "  proteus-cli scan <rom> --core <snes9x_libretro.dll> [--system dir] [--refs spc folder] [--first N] [--last N] [--keep-rips dir]\n"
         "  proteus-cli list <rom> <snes9x core> [app dir]\n"
         "  proteus-cli import <folder, archive or .spc> <folder>\n"
         "  proteus-cli download \"<game name>\" <folder> [zophar|snesmusic]\n");
   return 2;
}
