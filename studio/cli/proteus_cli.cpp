// SPDX-License-Identifier: LGPL-2.1-or-later
// proteus-cli: Proteus Studio's song work without the window, for scripts and testing.
//
//   proteus-cli table <rom> <spc folder>
//   proteus-cli match <spc folder> <rip.spc> [before.spc]
//   proteus-cli scan <rom> --core <snes9x_libretro.dll> [--system <dir>] [--refs <spc folder>] [--first N] [--last N] [--keep-rips dir]
//   proteus-cli download "<game name>" <folder>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"
#include "reference.h"
#include "rom_session.h"
#include "snes_rom.h"
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
   if (cmd == "download" && argc == 4)
   {
      std::string err;
      int n = download_reference_songs(argv[2], argv[3], [](const std::string &m) { printf("  %s\n", m.c_str()); }, err);
      printf("%d songs%s\n", n, err.empty() ? "" : (": " + err).c_str());
      return n > 0 ? 0 : 1;
   }
   fprintf(stderr, "usage:\n"
         "  proteus-cli table <rom> <spc folder>\n"
         "  proteus-cli match <spc folder> <rip.spc> [before.spc]\n"
         "  proteus-cli scan <rom> --core <snes9x_libretro.dll> [--system dir] [--refs spc folder] [--first N] [--last N] [--keep-rips dir]\n"
         "  proteus-cli download \"<game name>\" <folder>\n");
   return 2;
}
